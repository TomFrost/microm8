// Tab5 display through M5GFX, in portrait (720x1280) or landscape (1280x720).
//
// The M8's 320x240 framebuffer is scaled to the layout's M8 rectangle with pixel-area
// resampling: each display pixel averages the M8 pixels it covers, weighted by coverage. At
// integer scales that's a plain pixel copy; otherwise only the pixels on M8 pixel edges blend,
// so the M8's fonts stay sharp with even stroke widths.
//
// The M8 screen bypasses M5GFX: it's scaled in the panel's own (portrait) orientation, whatever
// the UI's rotation, into small buffers in internal RAM, and the DSI driver's 2D DMA copies
// those into the panel's framebuffer (PSRAM). The CPU never writes the framebuffer itself:
// pushing that much data through the cache, which both cores share, slowed the other core's
// USB audio enough to drop packets.
// The panel reads that framebuffer continuously (about 57 times a second), so each update is
// timed against its scan: written before the scan reaches it, or just after the scan has
// passed, never across the scan. Otherwise one refresh would show half an old M8 frame and
// half a new one, which breaks up the moving oscilloscope.
#include "display/ui.h"

#include <M5Unified.h>
#include <atomic>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "input/keypad.h"
#include "m8/protocol.h"
#include "m8/screen.h"

static const char *TAG = "ui";

namespace ui {
namespace {

using Gfx = lgfx::LovyanGFX; // M5GFX (the display) and M5Canvas both derive from it

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const uint16_t kBg = rgb(0, 0, 0);
const uint16_t kKeyPurple = rgb(150, 100, 240); // M8 keys: just a label or icon
const uint16_t kKeyPressed = rgb(255, 255, 255); // label and touch-area outline while held
const uint16_t kKeyFill = rgb(30, 30, 34);
const uint16_t kKeyEdge = rgb(72, 72, 80);
const uint16_t kKeyLabel = rgb(200, 200, 200);
const uint16_t kKeyFillDown = rgb(225, 225, 225);
const uint16_t kKeyLabelDown = rgb(0, 0, 0);
const uint16_t kStatusText = rgb(150, 150, 150);
const uint16_t kOk = rgb(60, 200, 90);
const uint16_t kWarn = rgb(230, 170, 40);
const uint16_t kErr = rgb(220, 60, 60);

constexpr int kKeyRadius = 14;

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------

struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum IconIndex { kIconVolume, kIconBrightness, kIconRotation, kIconPower, kIconKeys, kIconCount };

struct Layout {
  bool landscape;
  int width, height;
  Rect m8;
  Rect status;        // cleared and redrawn with the status
  bool status_text;   // connection text (portrait only)
  int text_x, text_y; // the connection text's left edge and vertical centre
  int icon_x[kIconCount], icon_y[kIconCount];
  Rect row;      // slider / power details row, or the landscape side panel
  bool vertical; // the row is a column (landscape): vertical slider, wrapped text
  int readout_x, readout_y; // power diagnostics, right-aligned at x
  Rect swipe;               // where swipes start (landscape arrows)
};

constexpr int kPortStatusCy = PORT_STATUS_Y + PORT_STATUS_H / 2;

const Layout kPortrait = {
    false,
    LCD_WIDTH,
    LCD_HEIGHT,
    {PORT_M8_X, PORT_M8_Y, PORT_M8_W, PORT_M8_H},
    {0, PORT_STATUS_Y, LCD_WIDTH, PORT_STATUS_H},
    true,
    48,
    kPortStatusCy,
    {448, 520, 592, 668, 376},
    {kPortStatusCy, kPortStatusCy, kPortStatusCy, kPortStatusCy, kPortStatusCy},
    {0, PORT_ROW_Y, LCD_WIDTH, PORT_ROW_H},
    false,
    330,
    kPortStatusCy,
    {0, 0, 0, 0},
};

// Landscape: icons down the left margin (battery first), the panel under them, and SHIFT in
// the corner (keypad_layout.cpp). The key layout icon heads the right margin, over the keys.
const Layout kLandscape = {
    true,
    LCD_HEIGHT,
    LCD_WIDTH,
    {LAND_M8_X, LAND_M8_Y, LAND_M8_W, LAND_M8_H},
    {0, 0, LAND_M8_X, LAND_ROW_Y - 2},
    false,
    0,
    0,
    {LAND_M8_X / 2, LAND_M8_X / 2, LAND_M8_X / 2, LAND_M8_X / 2,
     LAND_M8_X + LAND_M8_W + LAND_M8_X / 2},
    {104, 168, 232, 40, 40},
    {0, LAND_ROW_Y, LAND_M8_X, LAND_ROW_H},
    true,
    LAND_M8_X - 10,
    280,
    {0, 0, LCD_HEIGHT / 2, LCD_WIDTH},
};

std::atomic<const Layout *> layout_ptr{&kPortrait}; // read from the touch task too
const Layout &L() { return *layout_ptr.load(); }

int base_rotation;           // M5GFX rotation for kPortrait
Orientation cur_orientation; // UI task only
bool compact;                // compact key layout

constexpr int kIconHalfZone = 32; // touch zone half-size around an icon

// ---------------------------------------------------------------------------
// State of what's drawn
// ---------------------------------------------------------------------------

uint8_t drawn_mask;
Status drawn_status;
bool status_drawn;
Controls drawn_controls;
bool controls_drawn;

M5GFX &display() { return M5.Display; }

// ---------------------------------------------------------------------------
// Scaling
// ---------------------------------------------------------------------------

uint16_t col_src[LAND_M8_W > PORT_M8_W ? LAND_M8_W : PORT_M8_W];
uint16_t col_wt[sizeof(col_src) / sizeof(col_src[0])]; // weight of col_src, 0..256
uint16_t row_src[LAND_M8_H > PORT_M8_H ? LAND_M8_H : PORT_M8_H];
uint16_t row_wt[sizeof(row_src) / sizeof(row_src[0])];

// Pixel-area resampling along one axis: output pixel o covers [o*in/out, (o+1)*in/out) in
// source pixels. It either sits inside one source pixel (weight 256), or straddles two.
void build_axis(uint16_t *src, uint16_t *wt, int out, int in) {
  for (int o = 0; o < out; o++) {
    const int start = o * in; // in units of 1/out source pixel
    const int end = start + in;
    const int i0 = start / out;
    const int boundary = (i0 + 1) * out;
    src[o] = (uint16_t)i0;
    wt[o] = end <= boundary ? 256 : (uint16_t)((boundary - start) * 256 / in);
  }
}

// Blends two RGB565 colours; `w` (0..256) is the weight of `a`.
inline uint16_t blend565(uint16_t a, uint16_t b, int w) {
  if (w >= 256 || a == b) return a;
  if (w <= 0) return b;
  const uint32_t wa = (uint32_t)(w + 4) >> 3; // 0..32
  const uint32_t A = (a | ((uint32_t)a << 16)) & 0x07E0F81F;
  const uint32_t B = (b | ((uint32_t)b << 16)) & 0x07E0F81F;
  const uint32_t mix = ((A * wa + B * (32 - wa)) >> 5) & 0x07E0F81F;
  return (uint16_t)(mix | (mix >> 16));
}

// The M8 framebuffer, as the scaler reads it.
struct Source {
  const uint16_t *px;
  int stride;       // pixels per row
  int vis_w, vis_h; // the part shown (320x240 of a larger Model:02 screen)
};

Source source() {
  const int w = m8screen::width(), h = m8screen::height();
  return {m8screen::framebuffer(), w, w < M8_SCREEN_W ? w : M8_SCREEN_W,
          h < M8_SCREEN_H ? h : M8_SCREEN_H};
}

// `n` scaled pixels along output column `ox`, from output row `oy` in steps of `step`.
void span_along_col(uint16_t *dst, int n, const Source &s, int ox, int oy, int step) {
  const int i0 = col_src[ox];
  const int i1 = i0 + 1 < s.vis_w ? i0 + 1 : i0;
  const int cw = col_wt[ox];
  for (int k = 0; k < n; k++, oy += step) {
    const int j0 = row_src[oy];
    const int rw = row_wt[oy];
    const uint16_t *s0 = s.px + j0 * s.stride;
    uint16_t p = blend565(s0[i0], s0[i1], cw);
    if (rw < 256) {
      const uint16_t *s1 = s.px + (j0 + 1 < s.vis_h ? j0 + 1 : j0) * s.stride;
      p = blend565(p, blend565(s1[i0], s1[i1], cw), rw);
    }
    dst[k] = p;
  }
}

// ---------------------------------------------------------------------------
// The panel's framebuffer, and its scan
// ---------------------------------------------------------------------------

// Reaches Panel_DSI's esp_lcd handle (protected) to register the frame callback.
struct DsiAccess : lgfx::Panel_DSI {
  static esp_lcd_panel_handle_t handle(lgfx::Panel_DSI *p) {
    return p->*(&DsiAccess::_disp_panel_handle);
  }
};

struct Native {
  esp_lcd_panel_handle_t panel;
  int w, h;        // panel size
  int rot;         // M5GFX's internal rotation: how UI coordinates map to the panel's
  int blank_lines; // lines from the end-of-frame event to the first visible line
  int total_lines; // visible + blank: one refresh
} nat;

// End of each refresh, from the DSI driver's interrupt. (On P4 chips before revision 3 there's
// no true VSYNC interrupt; the driver signals when the DMA has sent the last line of a frame.)
// 32-bit microseconds (wrapping), so the task never reads a half-written value.
std::atomic<uint32_t> frame_end_us{0};
std::atomic<uint32_t> frame_us{0}; // measured refresh period

uint32_t now_us() { return (uint32_t)esp_timer_get_time(); }

bool IRAM_ATTR on_frame_sent(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
  const uint32_t now = (uint32_t)esp_timer_get_time();
  const uint32_t last = frame_end_us.load(std::memory_order_relaxed);
  if (last) {
    const uint32_t period = now - last;
    if (period > 5000 && period < 50000) frame_us.store(period, std::memory_order_relaxed);
  }
  frame_end_us.store(now ? now : 1, std::memory_order_relaxed);
  return false;
}

// Two internal-RAM buffers: one is scaled into while the DMA copies the other.
constexpr int kDmaBufPx = 16384; // 32 KB each
uint16_t *dma_buf[2];
SemaphoreHandle_t copy_done;
bool copy_pending;

bool IRAM_ATTR on_copy_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(copy_done, &woken);
  return woken == pdTRUE;
}

void wait_copy() {
  if (!copy_pending) return;
  if (xSemaphoreTake(copy_done, pdMS_TO_TICKS(100)) != pdTRUE) ESP_LOGW(TAG, "DMA copy timed out");
  copy_pending = false;
}

bool init_native() {
  auto *panel = display().getPanel();
  if (!panel || !panel->getBus() || panel->getBus()->busType() != lgfx::bus_type_t::bus_dsi) {
    ESP_LOGE(TAG, "display is not a DSI panel");
    return false;
  }
  auto *dsi = static_cast<lgfx::Panel_DSI *>(panel);
  const auto &det = dsi->config_detail();
  nat.panel = DsiAccess::handle(dsi);
  nat.w = panel->config().panel_width;
  nat.h = panel->config().panel_height;
  nat.blank_lines = det.vsync_front_porch + det.vsync_pulse_width + det.vsync_back_porch;
  nat.total_lines = nat.h + nat.blank_lines;
  for (auto &b : dma_buf) {
    b = (uint16_t *)heap_caps_aligned_alloc(128, kDmaBufPx * sizeof(uint16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  }
  copy_done = xSemaphoreCreateBinary();
  if (!nat.panel || !dma_buf[0] || !dma_buf[1] || !copy_done) {
    ESP_LOGE(TAG, "M8 screen setup failed");
    nat.panel = nullptr;
    return false;
  }

  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = on_frame_sent;
  cbs.on_color_trans_done = on_copy_done;
  const esp_err_t err = esp_lcd_dpi_panel_register_event_callbacks(nat.panel, &cbs, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "DSI callbacks: %s", esp_err_to_name(err));
    nat.panel = nullptr;
    return false;
  }
  ESP_LOGI(TAG, "panel %dx%d, %d blank lines", nat.w, nat.h, nat.blank_lines);
  return true;
}

// UI (rotated) coordinates <-> panel coordinates, as M5GFX's Panel_FrameBufferBase maps them.
struct NRect {
  int x, y, w, h;
};

NRect to_native(const Layout &l, const Rect &r) {
  switch (nat.rot) {
  case 1:
    return {l.height - (r.y + r.h), r.x, r.h, r.w};
  case 2:
    return {l.width - (r.x + r.w), l.height - (r.y + r.h), r.w, r.h};
  case 3:
    return {r.y, l.width - (r.x + r.w), r.h, r.w};
  default:
    return {r.x, r.y, r.w, r.h};
  }
}

// The scaler's hot loops, compiled for speed rather than the build's default (size).
#pragma GCC push_options
#pragma GCC optimize("O2")

// A source line's pixels, each repeated k times (integer scaling): n output pixels from output
// position `o` along the line, in steps of `step`. `pitch` is the distance between neighbouring
// source pixels along the line (1 along a row, the row stride down a column).
void replicate(uint16_t *dst, int n, const uint16_t *line, int pitch, int k, int o, int step) {
  int i = o / k;
  int left = step > 0 ? k - o % k : o % k + 1; // output pixels left from source pixel i
  uint16_t p = line[i * pitch];
  for (int t = 0; t < n; t++) {
    if (!left) {
      i += step;
      p = line[i * pitch];
      left = k;
    }
    dst[t] = p;
    left--;
  }
}

// Source row j scaled horizontally only: n output pixels from output column `ox`, in steps
// of `step`.
void scale_row_h(uint16_t *dst, int n, const Source &s, int j, int ox, int step) {
  const uint16_t *row = s.px + j * s.stride;
  for (int t = 0; t < n; t++, ox += step) {
    const int i0 = col_src[ox];
    const int i1 = i0 + 1 < s.vis_w ? i0 + 1 : i0;
    dst[t] = blend565(row[i0], row[i1], col_wt[ox]);
  }
}

int int_scale; // k when the M8 rectangle is exactly k times the M8 screen on both axes, else 0

// Scales panel rows [y0, y1), columns [x0, x0 + w), which must lie in the M8 rectangle, into
// `dst` (w pixels per row).
//
// Each panel row is one output row (rotations 0 and 2) or one output column (1 and 3), so
// consecutive panel rows that sample the same source line unblended are identical and are
// copied rather than rescaled. With integer scaling there's no blending at all; otherwise,
// along rows, each source row is scaled horizontally once and output rows blend two of those.
void render_native(uint16_t *dst, const Layout &l, const Source &s, int y0, int y1, int x0, int w) {
  static uint16_t hline[2][LAND_M8_W > PORT_M8_W ? LAND_M8_W : PORT_M8_W];
  int hline_src[2] = {-1, -1}; // source row in each, for this call's x0, w and step
  auto scaled = [&](int j, int o0, int step) -> const uint16_t * {
    const int slot = j & 1; // j and j + 1, blended together, never share a slot
    if (hline_src[slot] != j) {
      scale_row_h(hline[slot], w, s, j, o0, step);
      hline_src[slot] = j;
    }
    return hline[slot];
  };

  const bool along_row = !(nat.rot & 1);
  const int k = int_scale;
  int prev_key = -1;
  const uint16_t *prev = nullptr;
  for (int ny = y0; ny < y1; ny++, dst += w) {
    // `fixed`: this panel row's output row or column; o0 and step: the other axis along it.
    int fixed, o0, step;
    switch (nat.rot) {
    case 1:
      fixed = ny - l.m8.x, o0 = l.height - 1 - x0 - l.m8.y, step = -1;
      break;
    case 2:
      fixed = l.height - 1 - ny - l.m8.y, o0 = l.width - 1 - x0 - l.m8.x, step = -1;
      break;
    case 3:
      fixed = l.width - 1 - ny - l.m8.x, o0 = x0 - l.m8.y, step = 1;
      break;
    default:
      fixed = ny - l.m8.y, o0 = x0 - l.m8.x, step = 1;
      break;
    }

    // The source line this row samples, if it samples just one, unblended.
    int key = -1;
    if (k) {
      key = fixed / k;
    } else if ((along_row ? row_wt : col_wt)[fixed] >= 256) {
      key = (along_row ? row_src : col_src)[fixed];
    }
    if (key >= 0 && key == prev_key) {
      memcpy(dst, prev, w * sizeof(uint16_t));
    } else if (k) {
      if (along_row) {
        replicate(dst, w, s.px + key * s.stride, 1, k, o0, step);
      } else {
        replicate(dst, w, s.px + key, s.stride, k, o0, step);
      }
    } else if (along_row) {
      const int j0 = row_src[fixed], rw = row_wt[fixed];
      const uint16_t *h0 = scaled(j0, o0, step);
      if (rw >= 256) {
        memcpy(dst, h0, w * sizeof(uint16_t));
      } else {
        const uint16_t *h1 = scaled(j0 + 1 < s.vis_h ? j0 + 1 : j0, o0, step);
        for (int t = 0; t < w; t++) dst[t] = blend565(h0[t], h1[t], rw);
      }
    } else {
      span_along_col(dst, w, s, fixed, o0, step);
    }
    prev_key = key;
    prev = dst;
  }
}

#pragma GCC pop_options

// What flush_m8() has done, for log_display_stats().
struct {
  uint32_t flushes, ahead, waits, unsynced, too_big;
  uint32_t late; // timed writes that took longer than the scan gave them
  uint64_t px, write_us, wait_us;
  uint64_t render_us, copy_wait_us; // write time scaling (CPU), and waiting for the DMA
} stats;

// Scales panel rectangles and has the DMA copy them into the framebuffer, in bands of panel
// rows from the top (the order the panel scans), scaling each piece while the last is copied.
void write_rects(const Layout &l, const Source &s, const NRect *rects, int count, int ny0, int ny1,
                 int band_rows) {
  static int next_buf = 0;
  for (int y = ny0; y < ny1; y += band_rows) {
    const int yb = y + band_rows;
    for (int i = 0; i < count; i++) {
      const NRect &n = rects[i];
      const int y0 = y > n.y ? y : n.y;
      const int y1 = yb < n.y + n.h ? yb : n.y + n.h;
      if (y0 >= y1) continue;
      uint16_t *buf = dma_buf[next_buf];
      next_buf ^= 1;
      const int64_t t0 = esp_timer_get_time();
      render_native(buf, l, s, y0, y1, n.x, n.w); // the other buffer may still be copying
      const int64_t t1 = esp_timer_get_time();
      wait_copy();
      stats.render_us += t1 - t0;
      stats.copy_wait_us += esp_timer_get_time() - t1;
      if (esp_lcd_panel_draw_bitmap(nat.panel, n.x, y0, n.x + n.w, y1, buf) == ESP_OK) {
        copy_pending = true;
      }
    }
  }
}

// Scan timing. Positions are in lines since the last end-of-frame event: the blank lines come
// first, then panel row r at blank_lines + r.
constexpr int kScanMarginLines = 32; // slack for timing jitter and the DSI FIFO
float ns_per_px = 15;                // measured write cost, refined as we go

struct Scan {
  bool valid;
  int line;      // current position
  float line_us; // duration of one line
};

Scan scan_now() {
  const uint32_t end = frame_end_us.load(std::memory_order_relaxed);
  const uint32_t period = frame_us.load(std::memory_order_relaxed);
  if (!end || !period) return {false, 0, 0};
  const float line_us = (float)period / nat.total_lines;
  const uint32_t since = now_us() - end;
  if (since > 2 * period) return {false, 0, 0}; // the callback has stopped
  return {true, (int)(since / line_us) % nat.total_lines, line_us};
}

// Sleeps for most of a long wait, then spins for the rest (the tick is 1 ms).
void wait_us(int64_t us) {
  if (us <= 0) return;
  const int64_t until = esp_timer_get_time() + us;
  if (us > 2000) vTaskDelay(pdMS_TO_TICKS((us - 1000) / 1000));
  while (esp_timer_get_time() < until) {
  }
}

int mod(int a, int n) { return ((a % n) + n) % n; }

// An area of the M8 rectangle to redraw, in output pixels.
struct Run {
  int r0, r1; // rows
  int c0, c1; // columns
};

// Writes the runs, timed against the panel's scan so a refresh never shows part of an update.
// The runs cover panel rows [ny0, ny1), written in scan order, either:
// - ahead of the scan: started before the scan reaches the area, when writing a row is faster
//   than the scan moves on a row, so the scan never catches up; or
// - between scans: all of it while the scan is elsewhere.
// If the scan is in the way, wait for it to pass the area (at most one refresh).
void write_runs(const Run *runs, int count) {
  const Layout &l = L();
  const Source s = source();
  int ny0 = nat.h, ny1 = 0, max_w = 1;
  int64_t px = 0;
  NRect rects[16];
  for (int i = 0; i < count; i++) {
    const Run &r = runs[i];
    rects[i] = to_native(l, {l.m8.x + r.c0, l.m8.y + r.r0, r.c1 - r.c0, r.r1 - r.r0});
    const NRect &n = rects[i];
    if (n.y < ny0) ny0 = n.y;
    if (n.y + n.h > ny1) ny1 = n.y + n.h;
    if (n.w > max_w) max_w = n.w;
    px += (int64_t)n.w * n.h;
  }
  if (!px) return;
  int band_rows = kDmaBufPx / max_w;
  if (band_rows > 64) band_rows = 64; // small bands: the first lands soon after starting
  if (band_rows < 1) band_rows = 1;

  // With headroom: the per-pixel cost varies from update to update.
  const float est_us = px * ns_per_px * 1.3f / 1000.0f + 150;
  const float row_us = est_us / (ny1 - ny0);  // average write time per panel row
  const float lead_us = row_us * band_rows + 100; // until the first band has landed

  int64_t waited = 0;
  bool timed = false;
  int deadline_start = 0; // where the scan enters the area, to check the write beat it
  const Scan sc = scan_now();
  if (!sc.valid) {
    stats.unsynced++;
  } else {
    const int total = nat.total_lines;
    const int start = nat.blank_lines + ny0 - kScanMarginLines; // the scan enters the area
    const int span = ny1 - ny0 + 2 * kScanMarginLines;          // and leaves it this much later
    const float window_us = (total - span) * sc.line_us;        // time it's away
    const bool outrun = row_us < 0.8f * sc.line_us;             // writes stay ahead of the scan
    const float needed_us = outrun ? lead_us : est_us; // head start needed before it arrives
    if (needed_us > window_us) {
      stats.too_big++; // can't avoid the scan (e.g. a full landscape redraw): just write
    } else {
      const bool inside = mod(sc.line - start, total) < span;
      const int until_enter = mod(start - sc.line, total);
      if (inside || until_enter * sc.line_us <= needed_us) {
        const int lines = mod(start + span - sc.line, total);
        const int64_t t0 = esp_timer_get_time();
        wait_us((int64_t)(lines * sc.line_us));
        waited = esp_timer_get_time() - t0;
        stats.waits++;
      }
      if (outrun && est_us > window_us) {
        stats.ahead++;
      } else {
        timed = true;
        deadline_start = start;
      }
    }
  }

  // For a write between scans: how long until the scan reaches the area, from now.
  float deadline_us = 0;
  if (timed) {
    const Scan now = scan_now();
    deadline_us = mod(deadline_start - now.line, nat.total_lines) * now.line_us;
  }
  const int64_t t0 = esp_timer_get_time();
  write_rects(l, s, rects, count, ny0, ny1, band_rows);
  wait_copy();
  const int64_t dt = esp_timer_get_time() - t0;
  if (timed && dt > deadline_us) stats.late++;
  if (px > 2000) ns_per_px = ns_per_px * 0.9f + (dt * 1000.0f / px) * 0.1f;
  stats.flushes++;
  stats.px += px;
  stats.write_us += dt;
  stats.wait_us += waited;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// Pencil, tip at the bottom left, about 2s across.
void draw_pencil(Gfx &d, int cx, int cy, int s, uint16_t color) {
  const float r = s * 0.24f;                                   // half the body's width
  const float bx = cx - s * 0.45f, by = cy + s * 0.45f;        // the tip's base
  d.drawWideLine(cx - s * 0.28f, cy + s * 0.28f, cx + s, cy - s, r, color);
  d.fillTriangle(cx - s, cy + s, bx - r * 0.7071f, by - r * 0.7071f, bx + r * 0.7071f,
                 by + r * 0.7071f, color);
}

// The Option key symbol (as on Mac keyboards), about 2s across.
void draw_option(Gfx &d, int cx, int cy, int s, uint16_t color) {
  const float r = s * 0.12f < 1.2f ? 1.2f : s * 0.12f;
  const float top = cy - s * 0.55f, bottom = cy + s * 0.55f;
  d.drawWideLine(cx - s, top, cx - s * 0.35f, top, r, color);
  d.drawWideLine(cx - s * 0.35f, top, cx + s * 0.35f, bottom, r, color);
  d.drawWideLine(cx + s * 0.35f, bottom, cx + s, bottom, r, color);
  d.drawWideLine(cx + s * 0.15f, top, cx + s, top, r, color);
}

// Arrow as a thin chevron, 2s wide and s deep (so it can't be mistaken for PLAY's triangle).
void draw_chevron(Gfx &d, keypad::Icon icon, int cx, int cy, int s, uint16_t color) {
  const float r = s * 0.09f < 1.2f ? 1.2f : s * 0.09f;
  const float h = s / 2.0f;
  float tip_x = cx, tip_y = cy, ax = 0, ay = 0; // tip, and one arm's end relative to the tip
  switch (icon) {
  case keypad::Icon::kUp:
    tip_y = cy - h, ax = s, ay = s;
    break;
  case keypad::Icon::kDown:
    tip_y = cy + h, ax = s, ay = -s;
    break;
  case keypad::Icon::kLeft:
    tip_x = cx - h, ax = s, ay = s;
    break;
  default: // right
    tip_x = cx + h, ax = -s, ay = s;
    break;
  }
  const bool vertical = icon == keypad::Icon::kUp || icon == keypad::Icon::kDown;
  if (vertical) {
    d.drawWideLine(tip_x, tip_y, tip_x - ax, tip_y + ay, r, color);
    d.drawWideLine(tip_x, tip_y, tip_x + ax, tip_y + ay, r, color);
  } else {
    d.drawWideLine(tip_x, tip_y, tip_x + ax, tip_y - ay, r, color);
    d.drawWideLine(tip_x, tip_y, tip_x + ax, tip_y + ay, r, color);
  }
}

// Arrows and key symbols, about 2s across.
void draw_icon(Gfx &d, keypad::Icon icon, int cx, int cy, int s, uint16_t color) {
  switch (icon) {
  case keypad::Icon::kEdit:
    draw_pencil(d, cx, cy, s, color);
    break;
  case keypad::Icon::kOption:
    draw_option(d, cx, cy, s, color);
    break;
  case keypad::Icon::kPlay:
    d.fillTriangle(cx - s * 7 / 10, cy - s, cx - s * 7 / 10, cy + s, cx + s, cy, color);
    break;
  case keypad::Icon::kUp:
  case keypad::Icon::kDown:
  case keypad::Icon::kLeft:
  case keypad::Icon::kRight:
    draw_chevron(d, icon, cx, cy, s, color);
    break;
  default:
    break;
  }
}

constexpr int kPressInset = 4; // the pressed outline, inside the touch area

// Outline of a key's touch area, as shown while it's held: its hit rect, kept out of the M8
// screen (landscape hit rects reach a little into it) and off-screen parts.
Rect press_box(const Layout &l, const keypad::Key &k) {
  int x0 = k.x, x1 = k.x + k.w, y0 = k.y, y1 = k.y + k.h;
  if (l.landscape) {
    if (x0 < l.m8.x) {
      if (x1 > l.m8.x) x1 = l.m8.x;
    } else if (x0 < l.m8.x + l.m8.w) {
      x0 = l.m8.x + l.m8.w;
    }
  }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > l.width) x1 = l.width;
  if (y1 > l.height) y1 = l.height;
  return {x0 + kPressInset, y0 + kPressInset, x1 - x0 - 2 * kPressInset,
          y1 - y0 - 2 * kPressInset};
}

// A key: a purple label or arrow; while held, white, with its touch area outlined.
void draw_key(Gfx &d, const keypad::Key &k, bool down) {
  const Rect b = press_box(L(), k);
  d.fillRect(b.x, b.y, b.w, b.h, kBg);
  if (down) {
    d.drawRoundRect(b.x, b.y, b.w, b.h, kKeyRadius, kKeyPressed);
    d.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, kKeyRadius - 1, kKeyPressed);
  }
  const uint16_t fg = down ? kKeyPressed : kKeyPurple;
  const int cx = k.vx + k.vw / 2, cy = k.vy + k.vh / 2;
  if (k.icon != keypad::Icon::kNone) {
    draw_icon(d, k.icon, cx, cy, 24, fg);
    return;
  }
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(fg);
  d.drawString(k.label, cx, cy);
}

void draw_button(Gfx &d, int x, int y, int w, int h, const char *label, bool lit = false) {
  d.fillRoundRect(x, y, w, h, 8, lit ? kKeyFillDown : kKeyFill);
  d.drawRoundRect(x, y, w, h, 8, lit ? kKeyFillDown : kKeyEdge);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(lit ? kKeyLabelDown : kKeyLabel);
  d.drawString(label, x + w / 2, y + h / 2);
}

void draw_speaker_icon(Gfx &d, int cx, int cy, uint16_t color, bool muted) {
  d.fillRect(cx - 18, cy - 6, 7, 12, color);
  d.fillTriangle(cx - 12, cy, cx - 2, cy - 13, cx - 2, cy + 13, color);
  d.fillRect(cx - 12, cy - 6, 6, 12, color);
  if (muted) {
    d.drawWideLine(cx + 5, cy - 7, cx + 17, cy + 7, 1.5f, color);
    d.drawWideLine(cx + 5, cy + 7, cx + 17, cy - 7, 1.5f, color);
  } else {
    d.fillArc(cx - 2, cy, 7, 9, -45, 45, color);
    d.fillArc(cx - 2, cy, 13, 15, -45, 45, color);
  }
}

void draw_headphones_icon(Gfx &d, int cx, int cy, uint16_t color, bool muted) {
  d.fillArc(cx, cy + 3, 13, 16, 180, 360, color);
  d.fillRoundRect(cx - 17, cy + 1, 8, 13, 3, color);
  d.fillRoundRect(cx + 9, cy + 1, 8, 13, 3, color);
  if (muted) d.drawWideLine(cx - 18, cy + 15, cx + 18, cy - 15, 1.5f, kErr);
}

void draw_sun_icon(Gfx &d, int cx, int cy, uint16_t color) {
  d.fillCircle(cx, cy, 7, color);
  for (int i = 0; i < 8; i++) {
    const float a = i * (float)M_PI / 4;
    d.drawWideLine(cx + cosf(a) * 11, cy + sinf(a) * 11, cx + cosf(a) * 16, cy + sinf(a) * 16,
                   1.3f, color);
  }
}

// Circular arrow; crossed out in red when rotation is locked.
void draw_rotation_icon(Gfx &d, int cx, int cy, uint16_t color, bool locked) {
  d.fillArc(cx, cy, 12, 15, 40, 320, color);
  const float a = 40 * (float)M_PI / 180;
  const int ex = cx + (int)(13.5f * cosf(a));
  const int ey = cy + (int)(13.5f * sinf(a));
  d.fillTriangle(ex + 7, ey - 3, ex - 7, ey - 3, ex, ey + 8, color);
  if (locked) {
    d.drawWideLine(cx - 15, cy - 15, cx + 15, cy + 15, 2.0f, kErr);
    d.drawWideLine(cx - 15, cy + 15, cx + 15, cy - 15, 2.0f, kErr);
  }
}

// Colour for a battery level: red when nearly empty, through yellow at 50%, to green at 100%.
uint16_t battery_color(int percent) {
  struct Rgb {
    float r, g, b;
  };
  const Rgb red = {220, 60, 60}, yellow = {230, 200, 40}, green = {60, 200, 90};
  const float t = percent <= 10 ? 0.0f : (percent >= 100 ? 1.0f : (percent - 10) / 90.0f);
  Rgb a = red, b = yellow;
  float u = t / 0.444f; // 10% -> 50% is the first 44% of the range
  if (t > 0.444f) {
    a = yellow;
    b = green;
    u = (t - 0.444f) / 0.556f;
  }
  return rgb((uint8_t)(a.r + (b.r - a.r) * u), (uint8_t)(a.g + (b.g - a.g) * u),
             (uint8_t)(a.b + (b.b - a.b) * u));
}

// Lightning bolt, about 16 x 26, centred on (cx, cy).
void draw_bolt(Gfx &d, int cx, int cy, uint16_t color) {
  const int x = cx - 8, y = cy - 13;
  d.fillTriangle(x + 11, y, x + 1, y + 15, x + 9, y + 15, color);
  d.fillTriangle(x + 7, y + 11, x + 15, y + 11, x + 5, y + 26, color);
}

// Bolt with a 2 px border in `edge`, so it reads over the battery fill and the background.
void draw_outlined_bolt(Gfx &d, int cx, int cy, uint16_t color, uint16_t edge) {
  for (int dx = -2; dx <= 2; dx += 2) {
    for (int dy = -2; dy <= 2; dy += 2) draw_bolt(d, cx + dx, cy + dy, edge);
  }
  draw_bolt(d, cx, cy, color);
}

// Mains plug, centred on (cx, cy): shown when there's no battery.
void draw_plug_icon(Gfx &d, int cx, int cy, uint16_t color) {
  d.fillRect(cx - 7, cy - 14, 4, 8, color); // prongs
  d.fillRect(cx + 3, cy - 14, 4, 8, color);
  d.fillRoundRect(cx - 11, cy - 7, 22, 14, 4, color); // body
  d.fillRect(cx - 2, cy + 7, 4, 7, color);            // cord
}

constexpr int kBatteryW = 48;
constexpr int kBatteryH = 24;

// Power icon: a battery filled to its charge level and coloured by it; while charging, an
// empty battery with a green bolt (the voltage-based gauge reads high then); plugged in and
// full, a full green battery with a light, outlined bolt; with no battery, a plug.
void draw_power_icon(Gfx &d, const Status &s, int cx, int cy) {
  if (s.power == PowerSource::kNoBattery) {
    draw_plug_icon(d, cx, cy, kKeyLabel);
    return;
  }
  const int x = cx - kBatteryW / 2 - 2;
  const int y = cy - kBatteryH / 2;
  d.drawRoundRect(x, y, kBatteryW, kBatteryH, 5, kStatusText);
  d.drawRoundRect(x + 1, y + 1, kBatteryW - 2, kBatteryH - 2, 4, kStatusText);
  d.fillRoundRect(x + kBatteryW + 1, y + kBatteryH / 2 - 5, 4, 10, 1, kStatusText);

  if (s.power == PowerSource::kCharging) {
    draw_bolt(d, x + kBatteryW / 2, cy, kOk);
    return;
  }
  if (s.power == PowerSource::kPluggedIn) {
    d.fillRoundRect(x + 4, y + 4, kBatteryW - 8, kBatteryH - 8, 2, kOk);
    draw_outlined_bolt(d, x + kBatteryW / 2, cy, kKeyFillDown, kBg);
    return;
  }
  if (s.battery < 0) return;
  const int pct = s.battery > 100 ? 100 : s.battery;
  int fill_w = (kBatteryW - 8) * pct / 100;
  if (pct > 0 && fill_w < 3) fill_w = 3; // keep a visible sliver
  if (fill_w > 0) d.fillRoundRect(x + 4, y + 4, fill_w, kBatteryH - 8, 2, battery_color(pct));
}

// Icon background: lit while its row is open.
void draw_icon_zone(Gfx &d, int cx, int cy, bool lit) {
  d.fillRoundRect(cx - 32, cy - 26, 64, 52, 10, lit ? kKeyFill : kBg);
}

// Text, wrapped to `wrap` pixels if non-zero, at the current datum. Returns the y after the
// last line.
int draw_wrapped(Gfx &d, const char *text, int x, int y, int wrap, int line_h) {
  if (!wrap) {
    d.drawString(text, x, y);
    return y + line_h;
  }
  char line[64] = "";
  char word[32];
  const char *p = text;
  while (*p) {
    int n = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < (int)sizeof(word) - 1) word[n++] = *p++;
    word[n] = '\0';
    if (!n) break;
    char candidate[64];
    snprintf(candidate, sizeof(candidate), "%s%s%s", line, line[0] ? " " : "", word);
    if (line[0] && d.textWidth(candidate) > wrap) {
      d.drawString(line, x, y);
      y += line_h;
      snprintf(line, sizeof(line), "%s", word);
    } else {
      snprintf(line, sizeof(line), "%s", candidate);
    }
  }
  if (line[0]) {
    d.drawString(line, x, y);
    y += line_h;
  }
  return y;
}

// ---------------------------------------------------------------------------
// The row of controls: under the status bar in portrait, a panel in the left margin in
// landscape. It holds a slider or power details, and in landscape otherwise a message or the
// swipe hint.
// ---------------------------------------------------------------------------

constexpr int kToggleInset = 24; // SPK/HP button (volume row) / sun (brightness row)
constexpr int kToggleW = 104;
constexpr int kToggleH = 52;
constexpr int kVToggleH = 44;
constexpr int kSliderInsetL = 168; // horizontal slider ends, from the row's ends
constexpr int kSliderInsetR = 92;
constexpr int kVSliderTop = 92; // vertical slider ends, from the panel's top and bottom
constexpr int kVSliderBottom = 20;
constexpr int kVSliderX = 56; // vertical slider track, from the panel's left
constexpr int kSliderSlop = 30;
constexpr int kKnobR = 16;

struct SliderRange {
  int min, max;
};

SliderRange slider_range(Popup popup) {
  return popup == Popup::kBrightness ? SliderRange{kBrightnessMin, kBrightnessMax}
                                     : SliderRange{0, 10};
}

// The slider track: from `lo` (minimum) to `hi` (maximum) along x (horizontal) or y
// (vertical, minimum at the bottom), at `across` on the other axis.
struct Track {
  bool vertical;
  int lo, hi, across;
};

Track track(const Layout &l) {
  if (l.vertical) {
    return {true, l.row.y + l.row.h - kVSliderBottom, l.row.y + kVSliderTop, l.row.x + kVSliderX};
  }
  return {false, l.row.x + kSliderInsetL, l.row.x + l.row.w - kSliderInsetR,
          l.row.y + l.row.h / 2};
}

// The SPK/HP button, or where the sun sits in the brightness row.
Rect toggle_rect(const Layout &l) {
  if (l.vertical) return {l.row.x + (l.row.w - kToggleW) / 2, l.row.y + 6, kToggleW, kVToggleH};
  return {l.row.x + kToggleInset, l.row.y + (l.row.h - kToggleH) / 2, kToggleW, kToggleH};
}

void draw_slider(Gfx &d, const Track &t, int value, SliderRange r) {
  const int pos = t.lo + (value - r.min) * (t.hi - t.lo) / (r.max - r.min);
  char text[8];
  snprintf(text, sizeof(text), "%d", value);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(kKeyLabel);
  if (t.vertical) {
    d.fillRoundRect(t.across - 5, t.hi, 10, t.lo - t.hi, 5, kKeyEdge);
    if (pos < t.lo) d.fillRoundRect(t.across - 5, pos, 10, t.lo - pos, 5, kKeyLabel);
    d.fillCircle(t.across, pos, kKnobR, kKeyFillDown);
    d.drawString(text, t.across + 52, pos); // beside the knob
  } else {
    d.fillRoundRect(t.lo, t.across - 5, t.hi - t.lo, 10, 5, kKeyEdge);
    if (pos > t.lo) d.fillRoundRect(t.lo, t.across - 5, pos - t.lo, 10, 5, kKeyLabel);
    d.fillCircle(pos, t.across, kKnobR, kKeyFillDown);
    d.drawString(text, t.hi + kSliderInsetR / 2, t.across);
  }
}

void power_text(const Controls &c, char *text, size_t len) {
  if (c.power == PowerSource::kNoBattery) {
    snprintf(text, len, "No battery");
  } else if (c.power == PowerSource::kCharging) {
    snprintf(text, len, "Battery charging");
  } else if (c.power == PowerSource::kPluggedIn) {
    snprintf(text, len, "Plugged in, battery full");
  } else if (c.battery >= 0) {
    snprintf(text, len, "Battery %d%%", c.battery);
  } else {
    snprintf(text, len, "Battery level unknown");
  }
}

// Text in the side panel, wrapped and centred.
void draw_panel_text(Gfx &d, const Layout &l, const char *text) {
  d.setFont(&fonts::FreeSans12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(kKeyLabel);
  draw_wrapped(d, text, l.row.x + l.row.w / 2, l.row.y + 40, l.row.w - 20, 30);
}

// Pads (keypad.h): each key's icon round the centre, and while held, the outline of its wedge
// within the pad's square.
struct PadKey {
  keypad::Icon icon;
  uint8_t bit;
  float dx, dy; // icon direction from the centre
};
const PadKey kArrowKeys[] = {{keypad::Icon::kUp, m8proto::kKeyUp, 0, -1},
                             {keypad::Icon::kDown, m8proto::kKeyDown, 0, 1},
                             {keypad::Icon::kLeft, m8proto::kKeyLeft, -1, 0},
                             {keypad::Icon::kRight, m8proto::kKeyRight, 1, 0}};
// OPTION and EDIT at 30 degrees above horizontal, PLAY straight down: the wedges' middles.
const PadKey kTriangleKeys[] = {{keypad::Icon::kOption, m8proto::kKeyOpt, -0.866f, -0.5f},
                                {keypad::Icon::kEdit, m8proto::kKeyEdit, 0.866f, -0.5f},
                                {keypad::Icon::kPlay, m8proto::kKeyStart, 0, 1}};

std::atomic<bool> pad_shown{false}; // the landscape panel shows its pad; read from touch

void draw_outline(Gfx &d, const int (*pts)[2], int n, uint16_t color) {
  for (int i = 0; i < n; i++) {
    const int *a = pts[i], *b = pts[(i + 1) % n];
    d.drawWideLine(a[0], a[1], b[0], b[1], 1.0f, color);
  }
}

// The wedge of key `bit` within a pad's square of half-size h.
void outline_wedge(Gfx &d, const keypad::Pad &p, uint8_t bit, int h, uint16_t color) {
  const int cx = p.cx, cy = p.cy;
  if (p.kind == keypad::PadKind::kArrows) {
    const int dx = bit == m8proto::kKeyLeft ? -1 : bit == m8proto::kKeyRight ? 1 : 0;
    const int dy = bit == m8proto::kKeyUp ? -1 : bit == m8proto::kKeyDown ? 1 : 0;
    const int pts[3][2] = {{cx, cy},
                           {cx + (dx ? dx : -1) * h, cy + (dy ? dy : -1) * h},
                           {cx + (dx ? dx : 1) * h, cy + (dy ? dy : 1) * h}};
    draw_outline(d, pts, 3, color);
    return;
  }
  const int t = h * 577 / 1000; // where the PLAY wedge's edges meet the square's sides
  if (bit == m8proto::kKeyStart) {
    const int pts[5][2] = {{cx, cy}, {cx - h, cy + t}, {cx - h, cy + h}, {cx + h, cy + h},
                           {cx + h, cy + t}};
    draw_outline(d, pts, 5, color);
  } else {
    const int side = bit == m8proto::kKeyOpt ? -1 : 1;
    const int pts[4][2] = {{cx, cy}, {cx, cy - h}, {cx + side * h, cy - h}, {cx + side * h, cy + t}};
    draw_outline(d, pts, 4, color);
  }
}

// Draws a pad, its held keys lit. The caller clears the background.
void draw_pad(Gfx &d, const keypad::Pad &p, uint8_t held) {
  const bool arrows = p.kind == keypad::PadKind::kArrows;
  const PadKey *keys = arrows ? kArrowKeys : kTriangleKeys;
  const int n = arrows ? 4 : 3;
  for (int i = 0; i < n; i++) {
    const bool down = held & keys[i].bit;
    if (down) outline_wedge(d, p, keys[i].bit, p.half - kPressInset, kKeyPressed);
    draw_icon(d, keys[i].icon, p.cx + (int)(keys[i].dx * p.icon_offset),
              p.cy + (int)(keys[i].dy * p.icon_offset), p.icon_size,
              down ? kKeyPressed : kKeyPurple);
  }
}

// Key layout icon: a device outline with an arrow diamond and a key triangle in it.
void draw_layout_icon(Gfx &d, int cx, int cy, uint16_t color) {
  d.drawRoundRect(cx - 22, cy - 14, 44, 28, 5, color);
  d.drawRoundRect(cx - 21, cy - 13, 42, 26, 4, color);
  const int ax = cx - 10;
  d.fillCircle(ax, cy - 6, 2, color);
  d.fillCircle(ax, cy + 6, 2, color);
  d.fillCircle(ax - 6, cy, 2, color);
  d.fillCircle(ax + 6, cy, 2, color);
  d.fillCircle(cx + 6, cy - 4, 2, color);
  d.fillCircle(cx + 14, cy - 4, 2, color);
  d.fillCircle(cx + 10, cy + 5, 2, color);
}

void paint_row(Gfx &d, const Layout &l, const Controls &c) {
  d.fillRect(l.row.x, l.row.y, l.row.w, l.row.h, kBg);
  const Rect tg = toggle_rect(l);
  if (c.popup == Popup::kVolume) {
    draw_button(d, tg.x, tg.y, tg.w, tg.h, c.headphones ? "HP" : "SPK", c.headphones);
    draw_slider(d, track(l), c.volume, slider_range(c.popup));
  } else if (c.popup == Popup::kBrightness) {
    draw_sun_icon(d, tg.x + tg.w / 2, tg.y + tg.h / 2, kKeyLabel);
    draw_slider(d, track(l), c.brightness, slider_range(c.popup));
  } else if (c.popup == Popup::kPower) {
    char text[32];
    power_text(c, text, sizeof(text));
    if (l.vertical) {
      draw_panel_text(d, l, text);
    } else {
      d.setFont(&fonts::FreeSansBold12pt7b);
      d.setTextDatum(textdatum_t::middle_right);
      d.setTextColor(kKeyLabel);
      d.drawString(text, l.row.x + l.row.w - 28, l.row.y + l.row.h / 2);
    }
  } else if (l.vertical) {
    if (c.message) {
      draw_panel_text(d, l, c.message);
    } else {
      const keypad::Table &t = keypad::table();
      for (int i = 0; i < t.pad_count; i++) {
        if (t.pads[i].in_panel) draw_pad(d, t.pads[i], c.arrows);
      }
    }
  }
}

bool same(const Status &a, const Status &b) {
  return a.link == b.link && a.device == b.device && a.fw_major == b.fw_major &&
         a.fw_minor == b.fw_minor && a.fw_patch == b.fw_patch && a.audio == b.audio &&
         a.oversized == b.oversized && a.volume == b.volume && a.headphones == b.headphones &&
         a.popup == b.popup && a.power == b.power && a.battery == b.battery &&
         a.power_mw == b.power_mw && a.rotation_locked == b.rotation_locked &&
         a.compact_keys == b.compact_keys && a.message == b.message;
}

bool same(const Controls &a, const Controls &b) {
  return a.popup == b.popup && a.volume == b.volume && a.headphones == b.headphones &&
         a.brightness == b.brightness && a.power == b.power && a.battery == b.battery &&
         a.message == b.message && a.arrows == b.arrows;
}

void apply_layout(Orientation o) {
  const bool landscape = o == Orientation::kLandscape || o == Orientation::kLandscapeFlipped;
  cur_orientation = o;
  layout_ptr = landscape ? &kLandscape : &kPortrait;
  keypad::set_arrangement(landscape ? keypad::Arrangement::kLandscape
                                    : keypad::Arrangement::kPortrait,
                          compact);

  auto &d = display();
  d.setRotation((base_rotation + (int)o) & 3);
  if (d.width() != L().width || d.height() != L().height) {
    ESP_LOGW(TAG, "unexpected display size %dx%d for orientation %d", d.width(), d.height(),
             (int)o);
  }

  const Layout &l = L();
  nat.rot = (d.getRotation() + d.getPanel()->config().offset_rotation) & 3;
  build_axis(col_src, col_wt, l.m8.w, M8_SCREEN_W);
  build_axis(row_src, row_wt, l.m8.h, M8_SCREEN_H);
  const bool exact = l.m8.w % M8_SCREEN_W == 0 && l.m8.h % M8_SCREEN_H == 0 &&
                     l.m8.w / M8_SCREEN_W == l.m8.h / M8_SCREEN_H;
  int_scale = exact ? l.m8.w / M8_SCREEN_W : 0;

  d.fillScreen(kBg);
  status_drawn = false;
  controls_drawn = false;
  pad_shown = false;
  draw_keys(drawn_mask, true);
  m8screen::mark_all_dirty();
  ESP_LOGI(TAG, "%s, M8 at %dx%d", landscape ? "landscape" : "portrait", l.m8.w, l.m8.h);
}

} // namespace

bool begin(Orientation orientation, bool compact_keys) {
  compact = compact_keys;
  auto &d = display();
  // The portrait base: rotations 0/2 and 1/3 differ by 90 degrees.
  base_rotation = d.getRotation();
  if (d.width() > d.height()) base_rotation ^= 1;
#if defined(TAB5_ROTATE_180) && TAB5_ROTATE_180
  base_rotation ^= 2;
#endif
  const bool ok = init_native();
  apply_layout(orientation);
  set_backlight(255);
  return ok;
}

void set_backlight(uint8_t level) { display().setBrightness(level); }

void set_orientation(Orientation orientation) {
  if (orientation != cur_orientation) apply_layout(orientation);
}

void set_compact_keys(bool on) {
  if (on == compact) return;
  compact = on;
  apply_layout(cur_orientation);
}

Orientation orientation() { return cur_orientation; }

void flush_m8() {
  if (!nat.panel) return;
  static m8screen::DirtyColumns cols[64];
  const uint64_t bands = m8screen::take_dirty(cols);
  if (!bands) return;

  const Layout &l = L();
  const Source src = source();
  const int band_count = (src.vis_h + m8screen::kBandRows - 1) / m8screen::kBandRows;

  // Each dirty band of M8 rows (and its dirty columns) maps to an area of output pixels, plus
  // one either side, which may blend with it. Merge vertically overlapping areas.
  Run runs[16];
  int count = 0;
  for (int b = 0; b < band_count; b++) {
    if (!(bands & (1ULL << b))) continue;
    const int y0 = b * m8screen::kBandRows;
    const int y1 = y0 + m8screen::kBandRows < src.vis_h ? y0 + m8screen::kBandRows : src.vis_h;
    const int x0 = cols[b].x0;
    const int x1 = cols[b].x1 < src.vis_w ? cols[b].x1 : src.vis_w;
    if (x0 >= x1) continue; // only off-screen columns of a larger (Model:02) screen
    Run r = {y0 * l.m8.h / M8_SCREEN_H - 1, (y1 * l.m8.h + M8_SCREEN_H - 1) / M8_SCREEN_H + 1,
             x0 * l.m8.w / M8_SCREEN_W - 1, (x1 * l.m8.w + M8_SCREEN_W - 1) / M8_SCREEN_W + 1};
    if (r.r0 < 0) r.r0 = 0;
    if (r.r1 > l.m8.h) r.r1 = l.m8.h;
    if (r.c0 < 0) r.c0 = 0;
    if (r.c1 > l.m8.w) r.c1 = l.m8.w;
    if (count && (r.r0 <= runs[count - 1].r1 || count == 16)) {
      Run &last = runs[count - 1];
      if (r.r1 > last.r1) last.r1 = r.r1;
      if (r.c0 < last.c0) last.c0 = r.c0;
      if (r.c1 > last.c1) last.c1 = r.c1;
    } else {
      runs[count++] = r;
    }
  }
  write_runs(runs, count);
}

void log_display_stats() {
  const uint32_t n = stats.flushes ? stats.flushes : 1;
  ESP_LOGI(TAG,
           "display: %lu M8 updates, avg %llu px in %llu us (%.1f ns/px; scaling %llu us, "
           "waiting for DMA %llu us), %lu written ahead of the scan, %lu waited for it (avg %llu "
           "us), %lu late, %lu too big to time, %lu untimed; refresh %ld us",
           (unsigned long)stats.flushes, stats.px / n, stats.write_us / n, ns_per_px,
           stats.render_us / n, stats.copy_wait_us / n, (unsigned long)stats.ahead,
           (unsigned long)stats.waits, stats.waits ? stats.wait_us / stats.waits : 0,
           (unsigned long)stats.late,
           (unsigned long)stats.too_big, (unsigned long)stats.unsynced,
           (long)frame_us.load());
  stats = {};
}

void draw_keys(uint8_t pressed_mask, bool force) {
  const keypad::Table &t = keypad::table();
  auto &d = display();
  d.startWrite();
  for (int i = 0; i < t.count; i++) {
    const keypad::Key &k = t.keys[i];
    const bool down = pressed_mask & k.bit;
    if (force || down != (bool)(drawn_mask & k.bit)) draw_key(d, k, down);
  }
  for (int i = 0; i < t.pad_count; i++) {
    const keypad::Pad &p = t.pads[i];
    if (p.in_panel) continue; // drawn with the panel (draw_controls)
    if (!force && !((pressed_mask ^ drawn_mask) & keypad::pad_keys(p))) continue;
    d.fillRect(p.cx - p.half, p.cy - p.half, 2 * p.half, 2 * p.half, kBg);
    draw_pad(d, p, pressed_mask);
  }
  d.endWrite();
  drawn_mask = pressed_mask;
}

void draw_status(const Status &s) {
  if (status_drawn && same(s, drawn_status)) return;
  drawn_status = s;
  status_drawn = true;

  const Layout &l = L();
  auto &d = display();
  d.startWrite();
  d.fillRect(l.status.x, l.status.y, l.status.w, l.status.h, kBg);

  if (l.status_text) {
    uint16_t dot = kWarn;
    char text[64];
    switch (s.link) {
    case Status::Link::kConnected:
      dot = s.oversized ? kWarn : kOk;
      if (s.device) {
        snprintf(text, sizeof(text), "%s %u.%u.%u%s", s.device, s.fw_major, s.fw_minor,
                 s.fw_patch, s.oversized ? " (crop)" : "");
      } else {
        snprintf(text, sizeof(text), "M8 connected");
      }
      break;
    case Status::Link::kWaiting:
      snprintf(text, sizeof(text), "Waiting for M8");
      break;
    case Status::Link::kUnsupported:
      dot = kErr;
      snprintf(text, sizeof(text), "Not an M8");
      break;
    case Status::Link::kError:
      dot = kErr;
      snprintf(text, sizeof(text), "USB setup failed");
      break;
    }
    if (s.message) snprintf(text, sizeof(text), "%s", s.message);

    d.fillCircle(l.text_x - 20, l.text_y, 7, dot);
    d.setFont(&fonts::FreeSans12pt7b);
    d.setTextDatum(textdatum_t::middle_left);
    d.setTextColor(s.message ? kKeyLabel : kStatusText);
    d.drawString(text, l.text_x, l.text_y);
    if (!s.message && s.link == Status::Link::kConnected && !s.audio) {
      d.setTextColor(kWarn);
      d.drawString("no audio", l.text_x + d.textWidth(text) + 14, l.text_y);
    }
  }

  if (s.power_mw >= 0) {
    char text[16];
    snprintf(text, sizeof(text), "%d.%02d W", s.power_mw / 1000, (s.power_mw % 1000) / 10);
    d.setFont(&fonts::FreeSans12pt7b);
    d.setTextColor(kWarn);
    d.setTextDatum(textdatum_t::middle_right);
    d.drawString(text, l.readout_x, l.readout_y);
  }

  for (int i = 0; i < kIconCount; i++) {
    const int cx = l.icon_x[i], cy = l.icon_y[i];
    const Popup owner = i == kIconVolume       ? Popup::kVolume
                        : i == kIconBrightness ? Popup::kBrightness
                        : i == kIconPower      ? Popup::kPower
                                               : Popup::kNone;
    const bool lit = i == kIconKeys ? s.compact_keys : owner != Popup::kNone && s.popup == owner;
    draw_icon_zone(d, cx, cy, lit);
    switch (i) {
    case kIconVolume:
      if (s.headphones) {
        draw_headphones_icon(d, cx, cy, kKeyLabel, s.volume == 0);
      } else {
        draw_speaker_icon(d, cx, cy, kKeyLabel, s.volume == 0);
      }
      break;
    case kIconBrightness:
      draw_sun_icon(d, cx, cy, kKeyLabel);
      break;
    case kIconRotation:
      draw_rotation_icon(d, cx, cy, kKeyLabel, s.rotation_locked);
      break;
    case kIconPower:
      draw_power_icon(d, s, cx, cy);
      break;
    case kIconKeys:
      draw_layout_icon(d, cx, cy, kKeyLabel);
      break;
    }
  }
  d.endWrite();
}

void draw_controls(const Controls &controls) {
  const Layout &l = L();
  Controls c = controls;
  if (!l.vertical) { // the portrait row shows neither
    c.message = nullptr;
    c.arrows = 0;
  }
  if (controls_drawn && same(c, drawn_controls)) return;
  drawn_controls = c;
  controls_drawn = true;

  auto &d = display();
  d.startWrite();
  paint_row(d, l, c);
  d.endWrite();
  pad_shown = l.vertical && c.popup == Popup::kNone && !c.message;
}

uint8_t backlight_for(int brightness) {
  // The backlight driver stops dimming below about 30% duty (measured), so the slider spans
  // 30%..100% linearly.
  constexpr int kMinDuty = 255 * 30 / 100;
  if (brightness < kBrightnessMin) brightness = kBrightnessMin;
  if (brightness > kBrightnessMax) brightness = kBrightnessMax;
  return (uint8_t)(kMinDuty + (255 - kMinDuty) * (brightness - kBrightnessMin) /
                                  (kBrightnessMax - kBrightnessMin));
}

Hit hit_test(int x, int y, Popup open, int *value) {
  const Layout &l = L();
  for (int i = 0; i < kIconCount; i++) {
    if (abs(x - l.icon_x[i]) < kIconHalfZone && abs(y - l.icon_y[i]) < kIconHalfZone) {
      return i == kIconVolume       ? Hit::kVolumeIcon
             : i == kIconBrightness ? Hit::kBrightnessIcon
             : i == kIconRotation   ? Hit::kRotationIcon
             : i == kIconKeys       ? Hit::kKeysIcon
                                    : Hit::kPowerIcon;
    }
  }
  if (open != Popup::kVolume && open != Popup::kBrightness) return Hit::kNone;
  if (!l.row.contains(x, y)) return Hit::kNone;

  const Rect tg = toggle_rect(l);
  const Rect toggle_zone = {tg.x - 8, tg.y - 8, tg.w + 16, tg.h + 16};
  if (open == Popup::kVolume && (l.vertical ? toggle_zone.contains(x, y)
                                            : x >= toggle_zone.x && x < toggle_zone.x + toggle_zone.w)) {
    return Hit::kOutputToggle;
  }
  const Track t = track(l);
  const int along = t.vertical ? y : x;
  const int a0 = t.lo < t.hi ? t.lo : t.hi, a1 = t.lo < t.hi ? t.hi : t.lo;
  if (along >= a0 - kSliderSlop && along < a1 + kSliderSlop) {
    *value = slider_value(x, y, open);
    return Hit::kSlider;
  }
  return Hit::kNone;
}

int slider_value(int x, int y, Popup open) {
  const Track t = track(L());
  const SliderRange r = slider_range(open);
  const float f = (float)((t.vertical ? y : x) - t.lo) / (float)(t.hi - t.lo);
  int v = r.min + (int)lroundf(f * (r.max - r.min));
  if (v < r.min) v = r.min;
  if (v > r.max) v = r.max;
  return v;
}

bool in_swipe_area(int x, int y) { return L().swipe.contains(x, y); }

uint8_t pad_key_at(int x, int y) {
  const keypad::Table &t = keypad::table();
  for (int i = 0; i < t.pad_count; i++) {
    const keypad::Pad &p = t.pads[i];
    if (p.in_panel && !pad_shown) continue;
    if (const uint8_t key = keypad::pad_key(p, x, y)) return key;
  }
  return 0;
}

} // namespace ui
