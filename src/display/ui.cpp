// Tab5 display: 720x1280 portrait through M5GFX. The M8's 320x240 screen is drawn at 2x, the
// M8 key layout sits along the bottom, and a status bar sits between them. Tapping the volume
// or brightness icon in the status bar opens a slider row under it.
#include "display/ui.h"

#include <M5Unified.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <math.h>
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

// Status bar icons (centres and touch zones, relative to STATUS_X), right-aligned.
constexpr int kVolumeIconX = 520;
constexpr int kBrightnessIconX = 592;
constexpr int kIconHalfZone = 36; // touch zone extends this far either side of an icon
constexpr int kPowerIconX = 670;  // battery or plug icon, centre
constexpr int kBatteryX = kPowerIconX - 26; // battery body; the terminal nub follows
constexpr int kBatteryW = 48;
constexpr int kBatteryH = 24;
constexpr int kPowerReadoutRight = 476; // diagnostics only

// Slider row (CONTROLS_Y..+CONTROLS_H).
constexpr int kToggleX = 24; // SPK/HP button (volume row)
constexpr int kToggleW = 104;
constexpr int kSliderX0 = 168;
constexpr int kSliderX1 = 628;
constexpr int kSliderValueX = 672;
constexpr int kSliderSlop = 30; // touch zone beyond the track ends
constexpr int kKnobR = 16;

constexpr int kScaledW = M8_SCREEN_W * M8_SCALE;
uint16_t *scaled; // one band, scaled: kScaledW x (kBandRows * M8_SCALE)

uint8_t drawn_mask;
Status drawn_status;
bool status_drawn;
Controls drawn_controls;
bool controls_drawn;

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

// ---------------------------------------------------------------------------
// Status bar icons
// ---------------------------------------------------------------------------

void draw_speaker_icon(int cx, int cy, uint16_t color, bool muted) {
  auto &d = gfx();
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

void draw_headphones_icon(int cx, int cy, uint16_t color, bool muted) {
  auto &d = gfx();
  d.fillArc(cx, cy + 3, 13, 16, 180, 360, color);
  d.fillRoundRect(cx - 17, cy + 1, 8, 13, 3, color);
  d.fillRoundRect(cx + 9, cy + 1, 8, 13, 3, color);
  if (muted) d.drawWideLine(cx - 18, cy + 15, cx + 18, cy - 15, 1.5f, kErr);
}

void draw_sun_icon(int cx, int cy, uint16_t color) {
  auto &d = gfx();
  d.fillCircle(cx, cy, 7, color);
  for (int i = 0; i < 8; i++) {
    const float a = i * (float)M_PI / 4;
    d.drawWideLine(cx + cosf(a) * 11, cy + sinf(a) * 11, cx + cosf(a) * 16, cy + sinf(a) * 16,
                   1.3f, color);
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

// Icon background: lit while its row is open.
void draw_icon_zone(int cx, bool lit) {
  gfx().fillRoundRect(STATUS_X + cx - kIconHalfZone + 4, STATUS_Y + 6, 2 * kIconHalfZone - 8,
                      STATUS_H - 12, 10, lit ? kKeyFill : kBg);
}

// Lightning bolt, about 16 x 26, centred on (cx, cy).
void draw_bolt(int cx, int cy, uint16_t color) {
  auto &d = gfx();
  const int x = cx - 8, y = cy - 13;
  d.fillTriangle(x + 11, y, x + 1, y + 15, x + 9, y + 15, color);
  d.fillTriangle(x + 7, y + 11, x + 15, y + 11, x + 5, y + 26, color);
}

// Bolt with a 2 px border in `edge`, so it reads over the battery fill and the background.
void draw_outlined_bolt(int cx, int cy, uint16_t color, uint16_t edge) {
  for (int dx = -2; dx <= 2; dx += 2) {
    for (int dy = -2; dy <= 2; dy += 2) draw_bolt(cx + dx, cy + dy, edge);
  }
  draw_bolt(cx, cy, color);
}

// Mains plug, centred on (cx, cy): shown when there's no battery.
void draw_plug_icon(int cx, int cy, uint16_t color) {
  auto &d = gfx();
  d.fillRect(cx - 7, cy - 14, 4, 8, color); // prongs
  d.fillRect(cx + 3, cy - 14, 4, 8, color);
  d.fillRoundRect(cx - 11, cy - 7, 22, 14, 4, color); // body
  d.fillRect(cx - 2, cy + 7, 4, 7, color); // cord
}

// Power icon: a battery filled to its charge level and coloured by it; while charging, an
// empty battery with a green bolt (the voltage-based gauge reads high then); plugged in and
// full, a full green battery with a light, outlined bolt; with no battery, a plug.
void draw_power_icon(const Status &s) {
  auto &d = gfx();
  const int cx = STATUS_X + kPowerIconX;
  const int cy = STATUS_Y + STATUS_H / 2;
  draw_icon_zone(kPowerIconX, s.popup == Popup::kPower);

  if (s.power == PowerSource::kNoBattery) {
    draw_plug_icon(cx, cy, kKeyLabel);
    return;
  }

  const int x = STATUS_X + kBatteryX;
  const int y = cy - kBatteryH / 2;
  d.drawRoundRect(x, y, kBatteryW, kBatteryH, 5, kStatusText);
  d.drawRoundRect(x + 1, y + 1, kBatteryW - 2, kBatteryH - 2, 4, kStatusText);
  d.fillRoundRect(x + kBatteryW + 1, y + kBatteryH / 2 - 5, 4, 10, 1, kStatusText);

  if (s.power == PowerSource::kCharging) {
    draw_bolt(x + kBatteryW / 2, cy, kOk);
    return;
  }
  if (s.power == PowerSource::kPluggedIn) {
    d.fillRoundRect(x + 4, y + 4, kBatteryW - 8, kBatteryH - 8, 2, kOk);
    draw_outlined_bolt(x + kBatteryW / 2, cy, kKeyFillDown, kBg);
    return;
  }
  if (s.battery < 0) return;
  const int pct = s.battery > 100 ? 100 : s.battery;
  int fill_w = (kBatteryW - 8) * pct / 100;
  if (pct > 0 && fill_w < 3) fill_w = 3; // keep a visible sliver
  if (fill_w > 0) d.fillRoundRect(x + 4, y + 4, fill_w, kBatteryH - 8, 2, battery_color(pct));
}

bool same(const Status &a, const Status &b) {
  return a.link == b.link && a.device == b.device && a.fw_major == b.fw_major &&
         a.fw_minor == b.fw_minor && a.fw_patch == b.fw_patch && a.audio == b.audio &&
         a.oversized == b.oversized && a.volume == b.volume && a.headphones == b.headphones &&
         a.popup == b.popup && a.power == b.power && a.battery == b.battery &&
         a.power_mw == b.power_mw;
}

// ---------------------------------------------------------------------------
// Slider row
// ---------------------------------------------------------------------------

struct SliderRange {
  int min, max;
};

SliderRange slider_range(Popup popup) {
  return popup == Popup::kBrightness ? SliderRange{kBrightnessMin, kBrightnessMax}
                                     : SliderRange{0, 10};
}

int slider_pos(int value, SliderRange r) {
  return kSliderX0 + (value - r.min) * (kSliderX1 - kSliderX0) / (r.max - r.min);
}

void draw_slider(int value, SliderRange r, int cy) {
  auto &d = gfx();
  const int pos = slider_pos(value, r);
  d.fillRoundRect(kSliderX0, cy - 5, kSliderX1 - kSliderX0, 10, 5, kKeyEdge);
  if (pos > kSliderX0) d.fillRoundRect(kSliderX0, cy - 5, pos - kSliderX0, 10, 5, kKeyLabel);
  d.fillCircle(pos, cy, kKnobR, kKeyFillDown);

  char text[8];
  snprintf(text, sizeof(text), "%d", value);
  d.setFont(&fonts::FreeSansBold12pt7b);
  d.setTextDatum(textdatum_t::middle_center);
  d.setTextColor(kKeyLabel);
  d.drawString(text, kSliderValueX, cy);
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
  auto &d = gfx();
  if (status_drawn && same(s, drawn_status)) return;
  drawn_status = s;
  status_drawn = true;

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

  if (s.power_mw >= 0) {
    snprintf(text, sizeof(text), "%d.%02d W", s.power_mw / 1000, (s.power_mw % 1000) / 10);
    d.setTextColor(kWarn);
    d.setTextDatum(textdatum_t::middle_right);
    d.drawString(text, STATUS_X + kPowerReadoutRight, cy);
  }

  draw_icon_zone(kVolumeIconX, s.popup == Popup::kVolume);
  if (s.headphones) {
    draw_headphones_icon(STATUS_X + kVolumeIconX, cy, kKeyLabel, s.volume == 0);
  } else {
    draw_speaker_icon(STATUS_X + kVolumeIconX, cy, kKeyLabel, s.volume == 0);
  }
  draw_icon_zone(kBrightnessIconX, s.popup == Popup::kBrightness);
  draw_sun_icon(STATUS_X + kBrightnessIconX, cy, kKeyLabel);
  draw_power_icon(s);
  d.endWrite();
}

void draw_controls(const Controls &c) {
  if (controls_drawn && c.popup == drawn_controls.popup && c.volume == drawn_controls.volume &&
      c.headphones == drawn_controls.headphones && c.brightness == drawn_controls.brightness &&
      c.power == drawn_controls.power && c.battery == drawn_controls.battery) {
    return;
  }
  drawn_controls = c;
  controls_drawn = true;

  auto &d = gfx();
  d.startWrite();
  d.fillRect(0, CONTROLS_Y, LCD_WIDTH, CONTROLS_H, kBg);
  const int cy = CONTROLS_Y + CONTROLS_H / 2;
  if (c.popup == Popup::kVolume) {
    draw_button(kToggleX, cy - 26, kToggleW, 52, c.headphones ? "HP" : "SPK", c.headphones);
    draw_slider(c.volume, slider_range(c.popup), cy);
  } else if (c.popup == Popup::kBrightness) {
    draw_sun_icon(kToggleX + kToggleW / 2, cy, kKeyLabel);
    draw_slider(c.brightness, slider_range(c.popup), cy);
  } else if (c.popup == Popup::kPower) {
    char text[32];
    if (c.power == PowerSource::kNoBattery) {
      snprintf(text, sizeof(text), "No battery");
    } else if (c.power == PowerSource::kCharging) {
      snprintf(text, sizeof(text), "Battery charging");
    } else if (c.power == PowerSource::kPluggedIn) {
      snprintf(text, sizeof(text), "Plugged in, battery full");
    } else if (c.battery >= 0) {
      snprintf(text, sizeof(text), "Battery %d%%", c.battery);
    } else {
      snprintf(text, sizeof(text), "Battery level unknown");
    }
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.setTextDatum(textdatum_t::middle_right);
    d.setTextColor(kKeyLabel);
    d.drawString(text, STATUS_X + kPowerIconX + 24, cy);
  }
  d.endWrite();
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
  if (y >= STATUS_Y && y < STATUS_Y + STATUS_H) {
    const int rx = x - STATUS_X;
    if (abs(rx - kVolumeIconX) < kIconHalfZone) return Hit::kVolumeIcon;
    if (abs(rx - kBrightnessIconX) < kIconHalfZone) return Hit::kBrightnessIcon;
    if (abs(rx - kPowerIconX) < kIconHalfZone) return Hit::kPowerIcon;
    return Hit::kNone;
  }
  if (open == Popup::kNone || open == Popup::kPower) return Hit::kNone; // nothing to touch
  if (y < CONTROLS_Y || y >= CONTROLS_Y + CONTROLS_H) return Hit::kNone;

  if (open == Popup::kVolume && x >= kToggleX - 8 && x < kToggleX + kToggleW + 8) {
    return Hit::kOutputToggle;
  }
  if (x >= kSliderX0 - kSliderSlop && x < kSliderX1 + kSliderSlop) {
    const SliderRange r = slider_range(open);
    const int span = kSliderX1 - kSliderX0;
    int v = r.min + ((x - kSliderX0) * (r.max - r.min) + span / 2) / span;
    if (v < r.min) v = r.min;
    if (v > r.max) v = r.max;
    *value = v;
    return Hit::kSlider;
  }
  return Hit::kNone;
}

} // namespace ui
