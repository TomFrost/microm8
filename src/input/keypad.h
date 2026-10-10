// On-screen M8 keypad: the key layouts (keypad_layout.cpp) and multi-touch hit testing.
//
// A key's hit rect may be larger than its drawn rect, so fingers sliding between
// neighbouring keys don't fall into gaps.
#pragma once

#include <stdint.h>

#include "input/touch.h"

namespace keypad {

enum class Icon : uint8_t { kNone, kUp, kDown, kLeft, kRight };

struct Key {
  const char *label;
  uint8_t bit; // m8proto::Key
  Icon icon;
  int16_t x, y, w, h;     // hit rect, screen coordinates
  int16_t vx, vy, vw, vh; // drawn rect (before kInset); the hit rect may extend past it
};

constexpr int kMaxKeys = 9;
constexpr int kInset = 3; // drawn keys are inset by this much from their drawn rect

// Portrait: the M8's own layout. Landscape: keys in the side margins, SHIFT on both sides, and
// no arrow keys (they're swipes).
enum class Arrangement : uint8_t { kPortrait, kLandscape };

// Switches the active layout (from the UI task; the touch task picks it up atomically).
void set_arrangement(Arrangement arrangement);
Arrangement arrangement();

struct Table {
  const Key *keys;
  int count;
};

// The active layout's keys (defined in keypad_layout.cpp). Read once and use the result, so
// keys and count always belong to the same layout.
const Table &table();

// Bitmask of M8 keys currently held by any touch point.
uint8_t mask_for(const touch::Point *points, int count);

} // namespace keypad
