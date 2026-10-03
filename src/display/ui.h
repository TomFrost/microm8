// Owns the LCD. Composes the M8 framebuffer, the on-screen keypad and the status bar.
// All functions must be called from the UI task.
#pragma once

#include <stdint.h>

#include "diag/log_capture.h"

namespace ui {

struct Status {
  enum class Link : uint8_t { kWaiting, kConnected, kUnsupported, kError } link;
  const char *device; // e.g. "Headless"
  uint8_t fw_major, fw_minor, fw_patch;
  bool audio;
  int volume;     // of the selected output
  bool oversized; // M8 resolution larger than the on-screen area
  bool headphones_available; // show the SPK/HP output toggle
  bool headphones;           // headphones are the selected output
};

bool begin();
void set_backlight(uint8_t level);

// Copies changed rows of the M8 framebuffer to the LCD.
void flush_m8();

// Redraws keys whose pressed state changed (or all, if `force`).
void draw_keys(uint8_t pressed_mask, bool force = false);

void draw_status(const Status &s);

// Fills the M8 area with a message while no M8 is connected.
void show_splash(const char *line1, const char *line2);

// Draws recent log lines below the splash, for debugging without a serial adapter.
void show_log(const logcap::Line *lines, int count);

// Status-bar touch targets.
enum class Hit : uint8_t { kNone, kOutputToggle, kVolumeDown, kVolumeUp };
Hit hit_test_status(int x, int y);

} // namespace ui
