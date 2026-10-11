// Owns the display: the scaled M8 framebuffer, the on-screen keys, the status icons and the
// row of controls (volume/brightness sliders, power details), in portrait or landscape. All
// functions must be called from the UI task, except hit_test(), slider_value(),
// in_swipe_area() and pad_key_at(), which are safe from any task.
#pragma once

#include <stdint.h>

#include "diag/log_capture.h"

namespace ui {

// Screen orientation. Values are quarter turns clockwise from portrait.
enum class Orientation : uint8_t { kPortrait, kLandscape, kPortraitFlipped, kLandscapeFlipped };

// Which row is open under the status bar: a slider, or battery details.
enum class Popup : uint8_t { kNone, kVolume, kBrightness, kPower, kMic };

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
  bool rotation_locked;
  bool compact_keys; // the key layout icon is lit
  bool mic_on;       // the mic icon shows live (red); struck through while off
  const char *message; // shown in place of the connection text when set (portrait)
};

// Contents of the row under the status bar (portrait), or of the panel in the left margin
// (landscape).
struct Controls {
  Popup popup;
  int volume;
  bool headphones;
  int brightness; // kBrightnessMin..kBrightnessMax
  PowerSource power;
  int battery;         // percent, or -1 if unknown
  const char *message; // landscape: shown in the panel while no row is open
  uint8_t arrows;      // landscape: arrow keys held, lit on the panel's arrow pad
  bool mic_on;
  int mic_boost; // 0 none, 1 +6 dB, 2 +12 dB
};

bool begin(Orientation orientation, bool compact_keys);
void set_backlight(uint8_t level);

// Rotates the display and redraws everything for the new layout.
void set_orientation(Orientation orientation);
Orientation orientation();

// Switches to the compact key layout (pads for each thumb) or back, and redraws.
void set_compact_keys(bool on);

// Brightness level (kBrightnessMin..kBrightnessMax) to backlight PWM level.
uint8_t backlight_for(int brightness);

// Draws changed rows of the M8 framebuffer, scaled. May wait (up to one display refresh) so
// the write doesn't cross the panel's scan.
void flush_m8();

// Logs M8 drawing performance since the last call.
void log_display_stats();

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
// on which row is open); for kOutputToggle, 0 for the speaker half, 1 for
// headphones; for kMicToggle, 0 for off, 1 for on; for kMicBoost, 0 none, 1 +6 dB, 2 +12 dB.
enum class Hit : uint8_t {
  kNone,
  kVolumeIcon,
  kBrightnessIcon,
  kRotationIcon,
  kPowerIcon,
  kKeysIcon,
  kMicIcon,
  kMicToggle, // value: 0 off, 1 on
  kMicBoost,  // value: 0..2
  kOutputToggle,
  kSlider,
};
Hit hit_test(int x, int y, Popup open, int *value);

// The level of the open slider for a finger at (x, y) anywhere on the screen: its position
// along the slider's axis, clamped to the ends. For a finger dragging a slider it has grabbed.
int slider_value(int x, int y, Popup open);

// True where a touch that isn't on a key or a status/row target starts a swipe (landscape's
// left half; nowhere in portrait).
bool in_swipe_area(int x, int y);

// The key under (x, y) on a pad (the arrow pad or the OPTION/EDIT/PLAY triangle), or 0. The
// landscape panel's arrow pad only counts while the panel shows it.
uint8_t pad_key_at(int x, int y);

} // namespace ui
