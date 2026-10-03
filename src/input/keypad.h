// On-screen M8 keypad: the key layout (keypad_layout.cpp) and multi-touch hit testing.
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

constexpr int kKeyCount = 8;
constexpr int kInset = 3; // drawn keys are inset by this much from their drawn rect

const Key *keys(); // kKeyCount entries; defined in keypad_layout.cpp

// Bitmask of M8 keys currently held by any touch point.
uint8_t mask_for(const touch::Point *points, int count);

} // namespace keypad
