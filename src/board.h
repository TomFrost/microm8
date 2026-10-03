// Tab5 bring-up that doesn't belong to a specific subsystem: M5Unified, power rails, I/O
// expanders, battery gauge.
#pragma once

namespace board {

// Called first in setup(), before the display, touch, audio and USB are started.
bool begin();

const char *name();

// Battery charge in percent, or -1 if unknown. Cheap to call often.
int battery_percent();

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
