// Owns the display. Composes the M8 framebuffer, the on-screen keypad, the status bar and the
// volume/brightness slider row. All drawing functions must be called from the UI task;
// hit_test() is safe from any task.
#pragma once

#include <stdint.h>

#include "diag/log_capture.h"

namespace ui {

// Which row is open under the status bar: a slider, or battery details.
enum class Popup : uint8_t { kNone, kVolume, kBrightness, kPower };

// Where power is coming from, for the status-bar icon and the details row. kPluggedIn means
// external power with the battery not charging (full).
enum class PowerSource : uint8_t { kBattery, kCharging, kPluggedIn, kNoBattery };

constexpr int kBrightnessMin = 1;
constexpr int kBrightnessMax = 10;

struct Status {
  enum class Link : uint8_t { kWaiting, kConnected, kUnsupported, kError } link;
  const char *device; // e.g. "Headless"
  uint8_t fw_major, fw_minor, fw_patch;
  bool audio;
  bool oversized;  // M8 resolution larger than the on-screen area
  int volume;      // of the selected output; 0 shows a muted icon
  bool headphones; // headphones are the selected output (icon shape)
  Popup popup;     // highlights the icon whose row is open
  PowerSource power;
  int battery;  // percent, or -1 if unknown
  int power_mw; // load in mW, shown for power diagnostics; -1 hides it
};

// Contents of the row under the status bar.
struct Controls {
  Popup popup;
  int volume;
  bool headphones;
  int brightness; // kBrightnessMin..kBrightnessMax
  PowerSource power;
  int battery; // percent, or -1 if unknown
};

bool begin();
void set_backlight(uint8_t level);

// Brightness level (kBrightnessMin..kBrightnessMax) to backlight PWM level.
uint8_t backlight_for(int brightness);

// Copies changed rows of the M8 framebuffer to the LCD.
void flush_m8();

// Redraws keys whose pressed state changed (or all, if `force`).
void draw_keys(uint8_t pressed_mask, bool force = false);

void draw_status(const Status &s);
void draw_controls(const Controls &c);

// Fills the M8 area with a message while no M8 is connected.
void show_splash(const char *line1, const char *line2);

// Draws recent log lines below the splash, for debugging without a serial adapter.
void show_log(const logcap::Line *lines, int count);

// Touch targets in the status bar and slider row. For kSlider, `value` receives the slider
// level under the touch (volume 0..10 or brightness kBrightnessMin..kBrightnessMax, depending
// on which row is open).
enum class Hit : uint8_t {
  kNone,
  kVolumeIcon,
  kBrightnessIcon,
  kPowerIcon,
  kOutputToggle,
  kSlider,
};
Hit hit_test(int x, int y, Popup open, int *value);

} // namespace ui
