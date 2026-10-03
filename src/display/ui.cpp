// Tab5 display: 720x1280 portrait through M5GFX. The M8's 320x240 screen is drawn at 2x,
// the M8 key layout sits along the bottom, and a status bar sits between them.
#include "display/ui.h"

#include <M5Unified.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "input/keypad.h"
#include "m8/screen.h"

static const char *TAG = "ui";

namespace ui {
namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const uint16_t kBg = rgb(0, 0, 0);
const uint16_t kFrame = rgb(28, 28, 32);
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

// Status bar controls, relative to STATUS_X.
constexpr int kOutputX = 416;
constexpr int kOutputW = 96;
constexpr int kVolDownX = 528;
constexpr int kVolBtnW = 60;
constexpr int kVolTextX = 592;
constexpr int kVolTextW = 52;
constexpr int kVolUpX = 648;

constexpr int kScaledW = M8_SCREEN_W * M8_SCALE;
uint16_t *scaled; // one band, scaled: kScaledW x (kBandRows * M8_SCALE)

uint8_t drawn_mask;
Status drawn_status;
bool status_drawn;
std::atomic<bool> output_toggle_shown{false}; // read by the touch task

M5GFX &gfx() { return M5.Display; }

void draw_arrow(keypad::Icon icon, int cx, int cy, uint16_t color) {
  const int s = 24; // half-size
  auto &d = gfx();
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

void draw_key(const keypad::Key &k, bool down) {
  auto &d = gfx();
  const int x = k.vx + keypad::kInset;
  const int y = k.vy + keypad::kInset;
  const int w = k.vw - 2 * keypad::kInset;
  const int h = k.vh - 2 * keypad::kInset;

  d.fillRoundRect(x, y, w, h, kKeyRadius, down ? kKeyFillDown : kKeyFill);
  d.drawRoundRect(x, y, w, h, kKeyRadius, down ? kKeyFillDown : kKeyEdge);

  const uint16_t fg = down ? kKeyLabelDown : kKeyLabel;
  if (k.icon != keypad::Icon::kNone) {
    draw_arrow(k.icon, x + w / 2, y + h / 2, fg);
    return;
  }
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(fg);
  d.drawString(k.label, x + w / 2, y + h / 2);
}

void draw_button(int x, int y, int w, int h, const char *label, bool lit = false) {
  auto &d = gfx();
  d.fillRoundRect(x, y, w, h, 8, lit ? kKeyFillDown : kKeyFill);
  d.drawRoundRect(x, y, w, h, 8, lit ? kKeyFillDown : kKeyEdge);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(lit ? kKeyLabelDown : kKeyLabel);
  d.drawString(label, x + w / 2, y + h / 2);
}

bool same(const Status &a, const Status &b) {
  return a.link == b.link && a.device == b.device && a.fw_major == b.fw_major &&
         a.fw_minor == b.fw_minor && a.fw_patch == b.fw_patch && a.audio == b.audio &&
         a.volume == b.volume && a.oversized == b.oversized &&
         a.headphones_available == b.headphones_available && a.headphones == b.headphones &&
         a.battery == b.battery;
}

} // namespace

bool begin() {
  auto &d = gfx();

  // Portrait: rotations 0/2 and 1/3 differ by 90 degrees.
  int rotation = d.getRotation();
  if (d.width() > d.height()) rotation ^= 1;
#if defined(TAB5_ROTATE_180) && TAB5_ROTATE_180
  rotation ^= 2;
#endif
  d.setRotation(rotation);
  if (d.width() != LCD_WIDTH || d.height() != LCD_HEIGHT) {
    ESP_LOGW(TAG, "unexpected display size %dx%d (rotation %d)", d.width(), d.height(), rotation);
  }

  const size_t band_px = (size_t)kScaledW * m8screen::kBandRows * M8_SCALE;
  scaled = (uint16_t *)heap_caps_malloc(band_px * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!scaled) scaled = (uint16_t *)heap_caps_malloc(band_px * 2, MALLOC_CAP_8BIT);
  if (!scaled) {
    ESP_LOGE(TAG, "scaling buffer allocation failed");
    return false;
  }

  d.fillScreen(kBg);
  d.drawRect(M8_DRAW_X - 2, M8_DRAW_Y - 2, kScaledW + 4, M8_SCREEN_H * M8_SCALE + 4, kFrame);
  draw_keys(0, true);
  set_backlight(255);
  return true;
}

void set_backlight(uint8_t level) { gfx().setBrightness(level); }

void flush_m8() {
  const uint64_t bands = m8screen::take_dirty_bands();
  if (!bands) return;

  const uint16_t *fb = m8screen::framebuffer();
  const int fb_w = m8screen::width();
  const int vis_w = fb_w < M8_SCREEN_W ? fb_w : M8_SCREEN_W;
  const int vis_h = m8screen::height() < M8_SCREEN_H ? m8screen::height() : M8_SCREEN_H;
  const int out_w = vis_w * M8_SCALE;
  const int band_count = (vis_h + m8screen::kBandRows - 1) / m8screen::kBandRows;

  auto &d = gfx();
  d.startWrite();
  for (int b = 0; b < band_count; b++) {
    if (!(bands & (1ULL << b))) continue;
    const int y0 = b * m8screen::kBandRows;
    int y1 = y0 + m8screen::kBandRows;
    if (y1 > vis_h) y1 = vis_h;

    // Nearest-neighbour upscale: each source pixel becomes an M8_SCALE x M8_SCALE block.
    for (int y = y0; y < y1; y++) {
      const uint16_t *src = fb + y * fb_w;
      uint16_t *dst = scaled + (y - y0) * M8_SCALE * out_w;
      for (int x = 0; x < vis_w; x++) {
        for (int s = 0; s < M8_SCALE; s++) dst[x * M8_SCALE + s] = src[x];
      }
      for (int r = 1; r < M8_SCALE; r++) memcpy(dst + r * out_w, dst, out_w * sizeof(uint16_t));
    }
    d.pushImage(M8_DRAW_X, M8_DRAW_Y + y0 * M8_SCALE, out_w, (y1 - y0) * M8_SCALE,
                (const m5gfx::rgb565_t *)scaled);
  }
  d.endWrite();
}

void draw_keys(uint8_t pressed_mask, bool force) {
  const keypad::Key *keys = keypad::keys();
  auto &d = gfx();
  d.startWrite();
  for (int i = 0; i < keypad::kKeyCount; i++) {
    const keypad::Key &k = keys[i];
    const bool down = pressed_mask & k.bit;
    if (force || down != (bool)(drawn_mask & k.bit)) draw_key(k, down);
  }
  d.endWrite();
  drawn_mask = pressed_mask;
}

void draw_status(const Status &s) {
  if (status_drawn && same(s, drawn_status)) return;
  drawn_status = s;
  status_drawn = true;

  auto &d = gfx();
  d.startWrite();
  d.fillRect(STATUS_X, STATUS_Y, STATUS_W, STATUS_H, kBg);

  uint16_t dot = kWarn;
  char text[64];
  switch (s.link) {
  case Status::Link::kConnected:
    dot = s.oversized ? kWarn : kOk;
    if (s.device) {
      snprintf(text, sizeof(text), "%s %u.%u.%u%s", s.device, s.fw_major, s.fw_minor, s.fw_patch,
               s.oversized ? " (crop)" : "");
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

  const int cy = STATUS_Y + STATUS_H / 2;
  d.fillCircle(STATUS_X + 28, cy, 7, dot);
  d.setFont(&fonts::FreeSans12pt7b);
  d.setTextDatum(textdatum_t::middle_left);
  d.setTextColor(kStatusText);
  int x = STATUS_X + 48;
  x += d.drawString(text, x, cy);

  if (s.link == Status::Link::kConnected && !s.audio) {
    d.setTextColor(kWarn);
    x += 14;
    x += d.drawString("no audio", x, cy);
  }
  if (s.battery >= 0) {
    snprintf(text, sizeof(text), "%d%%", s.battery);
    d.setTextColor(s.battery <= 15 ? kErr : kStatusText);
    d.setTextDatum(textdatum_t::middle_right);
    d.drawString(text, STATUS_X + kOutputX - 16, cy);
  }

  const int by = STATUS_Y + 6;
  const int bh = STATUS_H - 12;
  output_toggle_shown = s.headphones_available;
  if (s.headphones_available) {
    draw_button(STATUS_X + kOutputX, by, kOutputW, bh, s.headphones ? "HP" : "SPK", s.headphones);
  }
  draw_button(STATUS_X + kVolDownX, by, kVolBtnW, bh, "-");
  draw_button(STATUS_X + kVolUpX, by, kVolBtnW, bh, "+");
  snprintf(text, sizeof(text), "%d", s.volume);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(kKeyLabel);
  d.drawString(text, STATUS_X + kVolTextX + kVolTextW / 2, cy);
  d.endWrite();
}

Hit hit_test_status(int x, int y) {
  if (y < STATUS_Y || y >= STATUS_Y + STATUS_H) return Hit::kNone;
  const int rx = x - STATUS_X;
  if (output_toggle_shown && rx >= kOutputX - 4 && rx < kOutputX + kOutputW + 4) {
    return Hit::kOutputToggle;
  }
  if (rx >= kVolDownX - 4 && rx < kVolTextX) return Hit::kVolumeDown;
  if (rx >= kVolUpX - 4 && rx < STATUS_W) return Hit::kVolumeUp;
  return Hit::kNone;
}

} // namespace ui
