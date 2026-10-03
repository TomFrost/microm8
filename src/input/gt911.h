// Minimal GT911 capacitive touch driver with multi-touch (up to 5 points).
#pragma once

#include <stdint.h>

namespace gt911 {

constexpr int kMaxPoints = 5;

struct Point {
  int16_t x, y; // LCD coordinates
  uint8_t id;
};

bool begin();

// Polls the controller. Returns true if a fresh report was read, in which case `points` and
// `count` hold the current set of touches (count may be 0 when all fingers lift). Returns
// false if no new data is ready; callers should keep their previous state.
bool read(Point *points, int *count);

} // namespace gt911
