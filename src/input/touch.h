// Multi-touch input. Each board provides an implementation (GT911 on the JC4827W543,
// M5Unified on the Tab5).
#pragma once

#include <stdint.h>

namespace touch {

constexpr int kMaxPoints = 5;

struct Point {
  int16_t x, y; // screen coordinates, in the UI's orientation
  uint8_t id;
};

bool begin();

// Polls the touch controller. Returns true if a fresh report was read, in which case `points`
// and `count` hold the current set of touches (count may be 0 when all fingers lift). Returns
// false if no new data is ready; callers should keep their previous state.
bool read(Point *points, int *count);

} // namespace touch
