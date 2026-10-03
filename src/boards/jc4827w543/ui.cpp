#include "display/ui.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <atomic>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "input/keypad.h"
#include "m8/screen.h"

static const char *TAG = "ui";

namespace ui {
namespace {

Arduino_DataBus *bus;
Arduino_GFX *gfx;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const uint16_t kBg = rgb(0, 0, 0);
const uint16_t kKeyFill = rgb(30, 30, 34);
const uint16_t kKeyEdge = rgb(72, 72, 80);
const uint16_t kKeyLabel = rgb(200, 200, 200);
const uint16_t kKeyFillDown = rgb(225, 225, 225);
const uint16_t kKeyLabelDown = rgb(0, 0, 0);
const uint16_t kStatusText = rgb(150, 150, 150);
const uint16_t kOk = rgb(60, 200, 90);
const uint16_t kWarn = rgb(230, 170, 40);
const uint16_t kErr = rgb(220, 60, 60);

// Status bar controls, relative to STATUS_X.
constexpr int kOutputX = 174;
constexpr int kOutputW = 40;
constexpr int kVolDownX = 222;
constexpr int kVolTextX = 254;
constexpr int kVolUpX = 286;
constexpr int kVolBtnW = 32;


uint8_t drawn_mask;
Status drawn_status;
bool status_drawn;
std::atomic<bool> output_toggle_shown{false}; // read by the touch task

void draw_arrow(keypad::Icon icon, int cx, int cy, uint16_t color) {
  const int s = 11; // half-size
  switch (icon) {
  case keypad::Icon::kUp:
    gfx->fillTriangle(cx, cy - s, cx - s, cy + s / 2 + 2, cx + s, cy + s / 2 + 2, color);
    break;
  case keypad::Icon::kDown:
    gfx->fillTriangle(cx, cy + s, cx - s, cy - s / 2 - 2, cx + s, cy - s / 2 - 2, color);
    break;
  case keypad::Icon::kLeft:
    gfx->fillTriangle(cx - s, cy, cx + s / 2 + 2, cy - s, cx + s / 2 + 2, cy + s, color);
    break;
  case keypad::Icon::kRight:
    gfx->fillTriangle(cx + s, cy, cx - s / 2 - 2, cy - s, cx - s / 2 - 2, cy + s, color);
    break;
  default:
    break;
  }
}

void draw_key(const keypad::Key &k, bool down) {
  const int x = k.vx + keypad::kInset;
  const int y = k.vy + keypad::kInset;
  const int w = k.vw - 2 * keypad::kInset;
  const int h = k.vh - 2 * keypad::kInset;

  gfx->fillRoundRect(x, y, w, h, 6, down ? kKeyFillDown : kKeyFill);
  gfx->drawRoundRect(x, y, w, h, 6, down ? kKeyFillDown : kKeyEdge);

  const uint16_t fg = down ? kKeyLabelDown : kKeyLabel;
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  if (k.icon != keypad::Icon::kNone) {
    draw_arrow(k.icon, cx, cy, fg);
    return;
  }

  const int size = (int)strlen(k.label) * 12 <= w - 4 ? 2 : 1;
  const int tw = (int)strlen(k.label) * 6 * size - size;
  gfx->setTextSize(size);
  gfx->setTextColor(fg);
  gfx->setCursor(cx - tw / 2, cy - 4 * size);
  gfx->print(k.label);
}

void draw_button(int x, int y, int w, int h, const char *label, int size = 2, bool lit = false) {
  gfx->fillRoundRect(x, y, w, h, 4, lit ? kKeyFillDown : kKeyFill);
  gfx->drawRoundRect(x, y, w, h, 4, lit ? kKeyFillDown : kKeyEdge);
  gfx->setTextSize(size);
  gfx->setTextColor(lit ? kKeyLabelDown : kKeyLabel);
  const int tw = (int)strlen(label) * 6 * size - size;
  gfx->setCursor(x + (w - tw) / 2, y + (h - 7 * size) / 2);
  gfx->print(label);
}

} // namespace

bool begin() {
  ledcAttach(LCD_BL, 5000, 8);
  set_backlight(0);

  bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
  gfx = new Arduino_NV3041A(bus, GFX_NOT_DEFINED /* RST is an RC circuit */, 0 /* rotation */,
                            true /* IPS */);
  if (!gfx->begin()) {
    ESP_LOGE(TAG, "display init failed");
    return false;
  }
  gfx->fillScreen(kBg);

  // Panel + status bar backgrounds.
  draw_keys(0, true);
  set_backlight(255);
  return true;
}

void set_backlight(uint8_t level) { ledcWrite(LCD_BL, level); }

void flush_m8() {
  uint64_t bands = m8screen::take_dirty_bands();
  if (!bands) return;

  const uint16_t *fb = m8screen::framebuffer();
  const int fb_w = m8screen::width();
  const int vis_w = fb_w < M8_SCREEN_W ? fb_w : M8_SCREEN_W;
  const int vis_h = m8screen::height() < M8_SCREEN_H ? m8screen::height() : M8_SCREEN_H;
  const int band_count = (vis_h + m8screen::kBandRows - 1) / m8screen::kBandRows;

  int b = 0;
  while (b < band_count) {
    if (!(bands & (1ULL << b))) {
      b++;
      continue;
    }
    const int start = b;
    while (b < band_count && (bands & (1ULL << b))) b++;

    const int y0 = start * m8screen::kBandRows;
    int y1 = b * m8screen::kBandRows;
    if (y1 > vis_h) y1 = vis_h;

    if (fb_w == vis_w) {
      // Rows are contiguous in the framebuffer: one transfer for the whole run.
      gfx->draw16bitRGBBitmap(M8_SCREEN_X, M8_SCREEN_Y + y0, (uint16_t *)(fb + y0 * fb_w), vis_w,
                              y1 - y0);
    } else {
      // Oversized (Model:02) framebuffer: crop row by row.
      for (int y = y0; y < y1; y++) {
        gfx->draw16bitRGBBitmap(M8_SCREEN_X, M8_SCREEN_Y + y, (uint16_t *)(fb + y * fb_w), vis_w, 1);
      }
    }
  }
}

void draw_keys(uint8_t pressed_mask, bool force) {
  if (force) gfx->fillRect(PANEL_X, 0, PANEL_W, PANEL_H, kBg);
  const keypad::Key *keys = keypad::keys();
  for (int i = 0; i < keypad::kKeyCount; i++) {
    const keypad::Key &k = keys[i];
    const bool down = pressed_mask & k.bit;
    if (force || down != (bool)(drawn_mask & k.bit)) draw_key(k, down);
  }
  drawn_mask = pressed_mask;
}

bool same(const Status &a, const Status &b) {
  return a.link == b.link && a.device == b.device && a.fw_major == b.fw_major &&
         a.fw_minor == b.fw_minor && a.fw_patch == b.fw_patch && a.audio == b.audio &&
         a.volume == b.volume && a.oversized == b.oversized &&
         a.headphones_available == b.headphones_available && a.headphones == b.headphones;
}

void draw_status(const Status &s) {
  if (status_drawn && same(s, drawn_status)) return;
  drawn_status = s;
  status_drawn = true;

  gfx->fillRect(STATUS_X, STATUS_Y, STATUS_W, STATUS_H, kBg);
  gfx->drawFastHLine(STATUS_X, STATUS_Y, STATUS_W, rgb(40, 40, 44));

  uint16_t dot = kWarn;
  char text[48];
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
  gfx->fillCircle(STATUS_X + 10, cy, 4, dot);
  gfx->setTextSize(1);
  gfx->setTextColor(kStatusText);
  gfx->setCursor(STATUS_X + 20, cy - 4);
  gfx->print(text);

  if (s.link == Status::Link::kConnected && !s.audio) {
    gfx->setTextColor(kWarn);
    gfx->setCursor(gfx->getCursorX() + 8, cy - 4);
    gfx->print("no audio");
  }

  output_toggle_shown = s.headphones_available;
  if (s.headphones_available) {
    draw_button(STATUS_X + kOutputX, STATUS_Y + 3, kOutputW - 4, STATUS_H - 5,
                s.headphones ? "HP" : "SPK", 1, s.headphones);
  }
  draw_button(STATUS_X + kVolDownX, STATUS_Y + 3, 28, STATUS_H - 5, "-");
  draw_button(STATUS_X + kVolUpX + 2, STATUS_Y + 3, 28, STATUS_H - 5, "+");
  gfx->setTextSize(2);
  gfx->setTextColor(kKeyLabel);
  snprintf(text, sizeof(text), "%2d", s.volume);
  gfx->setCursor(STATUS_X + kVolTextX + 4, cy - 7);
  gfx->print(text);
}

Hit hit_test_status(int x, int y) {
  if (y < STATUS_Y || y >= STATUS_Y + STATUS_H) return Hit::kNone;
  const int rx = x - STATUS_X;
  if (output_toggle_shown && rx >= kOutputX - 4 && rx < kOutputX + kOutputW) {
    return Hit::kOutputToggle;
  }
  if (rx >= kVolDownX - 4 && rx < kVolDownX + kVolBtnW) return Hit::kVolumeDown;
  if (rx >= kVolUpX && rx < STATUS_W) return Hit::kVolumeUp;
  return Hit::kNone;
}

} // namespace ui
