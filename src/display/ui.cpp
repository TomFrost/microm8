// Tab5 display through M5GFX, in portrait (720x1280) or landscape (1280x720).
//
// The M8's 320x240 framebuffer is scaled to the layout's M8 rectangle with pixel-area
// resampling: each display pixel averages the M8 pixels it covers, weighted by coverage. At
// integer scales that's a plain pixel copy; otherwise only the pixels on M8 pixel edges blend,
// so the M8's fonts stay sharp with even stroke widths.
//
// The M8 screen is scaled in chunks of rows into an off-screen buffer, then pushed to the
// display. Nothing else overlaps it.
#include "display/ui.h"

#include <M5Unified.h>
#include <atomic>
#include <esp_log.h>
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

enum IconIndex { kIconVolume, kIconBrightness, kIconRotation, kIconPower, kIconCount };

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
    {448, 520, 592, 668},
    {kPortStatusCy, kPortStatusCy, kPortStatusCy, kPortStatusCy},
    {0, PORT_ROW_Y, LCD_WIDTH, PORT_ROW_H},
    false,
    404,
    kPortStatusCy,
    {0, 0, 0, 0},
};

// Landscape: icons down the left margin (battery first), the panel under them, and SHIFT in
// the corner (keypad_layout.cpp).
const Layout kLandscape = {
    true,
    LCD_HEIGHT,
    LCD_WIDTH,
    {LAND_M8_X, LAND_M8_Y, LAND_M8_W, LAND_M8_H},
    {0, 0, LAND_M8_X, LAND_ROW_Y - 2},
    false,
    0,
    0,
    {LAND_M8_X / 2, LAND_M8_X / 2, LAND_M8_X / 2, LAND_M8_X / 2},
    {104, 168, 232, 40},
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

constexpr int kChunkRows = 24;
M5Canvas canvas(&M5.Display);

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

// Scales output row `r` of the M8 rectangle into `dst` (byte-swapped, as M5Canvas stores it).
void scale_row(int r, uint16_t *dst, const uint16_t *fb, int fb_w, int vis_w, int vis_h) {
  const int w = L().m8.w;
  const int j0 = row_src[r];
  const int j1 = j0 + 1 < vis_h ? j0 + 1 : j0;
  const int rw = row_wt[r];
  const uint16_t *s0 = fb + j0 * fb_w;
  const uint16_t *s1 = fb + j1 * fb_w;
  for (int x = 0; x < w; x++) {
    const int i0 = col_src[x];
    const int i1 = i0 + 1 < vis_w ? i0 + 1 : i0;
    const int cw = col_wt[x];
    uint16_t p;
    if (rw >= 256) {
      p = blend565(s0[i0], s0[i1], cw);
    } else {
      p = blend565(blend565(s0[i0], s0[i1], cw), blend565(s1[i0], s1[i1], cw), rw);
    }
    dst[x] = (uint16_t)((p >> 8) | (p << 8));
  }
}

// Draws output rows [r0, r1) of the M8 rectangle.
void render_rows(int r0, int r1) {
  const Layout &l = L();
  const uint16_t *fb = m8screen::framebuffer();
  const int fb_w = m8screen::width();
  const int vis_w = fb_w < M8_SCREEN_W ? fb_w : M8_SCREEN_W;
  const int vis_h = m8screen::height() < M8_SCREEN_H ? m8screen::height() : M8_SCREEN_H;
  uint16_t *buf = (uint16_t *)canvas.getBuffer();
  if (!buf) return;

  for (int r = r0; r < r1; r += kChunkRows) {
    const int rows = r1 - r < kChunkRows ? r1 - r : kChunkRows;
    for (int i = 0; i < rows; i++) scale_row(r + i, buf + i * l.m8.w, fb, fb_w, vis_w, vis_h);
    display().pushImage(l.m8.x, l.m8.y + r, l.m8.w, rows, (const lgfx::swap565_t *)buf);
  }
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void draw_arrow(Gfx &d, keypad::Icon icon, int cx, int cy, int s, uint16_t color) {
  switch (icon) {
  case keypad::Icon::kUp:
    d.fillTriangle(cx, cy - s, cx - s, cy + s / 2 + 4, cx + s, cy + s / 2 + 4, color);
    break;
  case keypad::Icon::kDown:
    d.fillTriangle(cx, cy + s, cx - s, cy - s / 2 - 4, cx + s, cy - s / 2 - 4, color);
    break;
  case keypad::Icon::kLeft:
    d.fillTriangle(cx - s, cy, cx + s / 2 + 4, cy - s, cx + s / 2 + 4, cy + s, color);
    break;
  case keypad::Icon::kRight:
    d.fillTriangle(cx + s, cy, cx - s / 2 - 4, cy - s, cx - s / 2 - 4, cy + s, color);
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
    draw_arrow(d, k.icon, cx, cy, 24, fg);
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

// The arrow pad in the landscape panel: four arrows in a diamond, in a square as wide as the
// panel. Diagonals through the square's corners split it into four triangles, one per arrow,
// and each arrow owns its triangle; the panel's space above and below the square, and a strip
// of the M8 screen beside it, extend the up/down and right triangles outwards. Shown while the
// panel has nothing else to show.
constexpr int kPadArrowOffset = 50; // arrow centres from the pad's centre
constexpr int kPadArrowSize = 15;
constexpr int kPadReach = 16; // into the M8 screen

struct PadArrow {
  keypad::Icon icon;
  uint8_t bit;
  int dx, dy; // direction
};
const PadArrow kPad[] = {{keypad::Icon::kUp, m8proto::kKeyUp, 0, -1},
                         {keypad::Icon::kDown, m8proto::kKeyDown, 0, 1},
                         {keypad::Icon::kLeft, m8proto::kKeyLeft, -1, 0},
                         {keypad::Icon::kRight, m8proto::kKeyRight, 1, 0}};

std::atomic<bool> pad_shown{false}; // read from the touch task

void draw_arrow_pad(Gfx &d, const Layout &l, uint8_t arrows) {
  const int cx = l.row.x + l.row.w / 2, cy = l.row.y + l.row.h / 2;
  const int h = l.row.w / 2 - kPressInset; // the square's half-size, inset like key outlines
  for (const PadArrow &a : kPad) {
    const bool down = arrows & a.bit;
    if (down) {
      // The arrow's triangle: the centre and the two corners on its side.
      const int px = a.dx ? a.dx : -1, py = a.dy ? a.dy : -1;
      const int qx = a.dx ? a.dx : 1, qy = a.dy ? a.dy : 1;
      const int x1 = cx + px * h, y1 = cy + py * h, x2 = cx + qx * h, y2 = cy + qy * h;
      d.drawWideLine(cx, cy, x1, y1, 1.0f, kKeyPressed);
      d.drawWideLine(x1, y1, x2, y2, 1.0f, kKeyPressed);
      d.drawWideLine(x2, y2, cx, cy, 1.0f, kKeyPressed);
    }
    draw_arrow(d, a.icon, cx + a.dx * kPadArrowOffset, cy + a.dy * kPadArrowOffset,
               kPadArrowSize, down ? kKeyPressed : kKeyPurple);
  }
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
      draw_arrow_pad(d, l, c.arrows);
    }
  }
}

bool same(const Status &a, const Status &b) {
  return a.link == b.link && a.device == b.device && a.fw_major == b.fw_major &&
         a.fw_minor == b.fw_minor && a.fw_patch == b.fw_patch && a.audio == b.audio &&
         a.oversized == b.oversized && a.volume == b.volume && a.headphones == b.headphones &&
         a.popup == b.popup && a.power == b.power && a.battery == b.battery &&
         a.power_mw == b.power_mw && a.rotation_locked == b.rotation_locked &&
         a.message == b.message;
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
                                    : keypad::Arrangement::kPortrait);

  auto &d = display();
  d.setRotation((base_rotation + (int)o) & 3);
  if (d.width() != L().width || d.height() != L().height) {
    ESP_LOGW(TAG, "unexpected display size %dx%d for orientation %d", d.width(), d.height(),
             (int)o);
  }

  const Layout &l = L();
  build_axis(col_src, col_wt, l.m8.w, M8_SCREEN_W);
  build_axis(row_src, row_wt, l.m8.h, M8_SCREEN_H);
  canvas.deleteSprite();
  canvas.setColorDepth(16);
  canvas.setPsram(false);
  if (!canvas.createSprite(l.m8.w, kChunkRows)) {
    canvas.setPsram(true);
    if (!canvas.createSprite(l.m8.w, kChunkRows)) ESP_LOGE(TAG, "canvas allocation failed");
  }

  d.fillScreen(kBg);
  status_drawn = false;
  controls_drawn = false;
  pad_shown = false;
  draw_keys(drawn_mask, true);
  m8screen::mark_all_dirty();
  ESP_LOGI(TAG, "%s, M8 at %dx%d", landscape ? "landscape" : "portrait", l.m8.w, l.m8.h);
}

} // namespace

bool begin(Orientation orientation) {
  auto &d = display();
  // The portrait base: rotations 0/2 and 1/3 differ by 90 degrees.
  base_rotation = d.getRotation();
  if (d.width() > d.height()) base_rotation ^= 1;
#if defined(TAB5_ROTATE_180) && TAB5_ROTATE_180
  base_rotation ^= 2;
#endif
  apply_layout(orientation);
  set_backlight(255);
  return canvas.getBuffer() != nullptr;
}

void set_backlight(uint8_t level) { display().setBrightness(level); }

void set_orientation(Orientation orientation) {
  if (orientation != cur_orientation) apply_layout(orientation);
}

Orientation orientation() { return cur_orientation; }

void flush_m8() {
  const uint64_t bands = m8screen::take_dirty_bands();
  if (!bands) return;

  const Layout &l = L();
  const int vis_h = m8screen::height() < M8_SCREEN_H ? m8screen::height() : M8_SCREEN_H;
  const int band_count = (vis_h + m8screen::kBandRows - 1) / m8screen::kBandRows;

  // Each dirty band of M8 rows maps to a range of output rows (plus one either side, which
  // may blend with it); merge overlapping ranges and draw them.
  auto &d = display();
  d.startWrite();
  int run0 = -1, run1 = -1;
  for (int b = 0; b <= band_count; b++) {
    const bool dirty = b < band_count && (bands & (1ULL << b));
    if (dirty) {
      const int y0 = b * m8screen::kBandRows;
      const int y1 = y0 + m8screen::kBandRows < vis_h ? y0 + m8screen::kBandRows : vis_h;
      int r0 = y0 * l.m8.h / M8_SCREEN_H - 1;
      int r1 = (y1 * l.m8.h + M8_SCREEN_H - 1) / M8_SCREEN_H + 1;
      if (r0 < 0) r0 = 0;
      if (r1 > l.m8.h) r1 = l.m8.h;
      if (run0 >= 0 && r0 <= run1) {
        run1 = r1 > run1 ? r1 : run1;
        continue;
      }
      if (run0 >= 0) render_rows(run0, run1);
      run0 = r0;
      run1 = r1;
    } else if (run0 >= 0) {
      render_rows(run0, run1);
      run0 = -1;
    }
  }
  d.endWrite();
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
    draw_icon_zone(d, cx, cy, owner != Popup::kNone && s.popup == owner);
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

uint8_t arrow_at(int x, int y) {
  if (!pad_shown) return 0;
  const Layout &l = L();
  if (x < l.row.x || x >= l.row.x + l.row.w + kPadReach || y < l.row.y ||
      y >= l.row.y + l.row.h) {
    return 0;
  }
  const int dx = x - (l.row.x + l.row.w / 2), dy = y - (l.row.y + l.row.h / 2);
  if (abs(dx) > abs(dy)) return dx > 0 ? m8proto::kKeyRight : m8proto::kKeyLeft;
  return dy > 0 ? m8proto::kKeyDown : m8proto::kKeyUp;
}

} // namespace ui
