// Tab5 bring-up that doesn't belong to a specific subsystem: M5Unified, power rails, I/O
// expanders, battery gauge.
#pragma once

namespace board {

// Called first in setup(), before the display, touch, audio and USB are started.
bool begin();

const char *name();

// Battery charge in percent, or -1 if unknown. Cheap to call often. The Tab5's gauge derives
// it from battery voltage, so it dips under load and recovers at rest.
int battery_percent();

// True while the battery is charging. Cheap to call often.
bool charging();

// True while external power (USB-C) is carrying the load, whether or not it's charging.
// Cheap to call often.
bool external_power();

// False when no battery is fitted (the Tab5 has no presence signal; this is inferred from the
// battery voltage). Cheap to call often.
bool battery_present();

// Power drawn from the battery in mW, or -1 while charging (the battery current then
// doesn't reflect the load). Cheap to call often.
int power_draw_mw();

// Logs battery voltage, current, power and the P4's temperature.
void log_power();

// The touch controller, audio codec, I/O expanders and battery gauge share one internal I2C
// bus, reached from tasks on both cores (touch polling, audio routing, jack detection), so
// every access goes through this lock.
class I2CLock {
 public:
  I2CLock();
  ~I2CLock();
  I2CLock(const I2CLock &) = delete;
  I2CLock &operator=(const I2CLock &) = delete;
};

} // namespace board
