// On-screen M8 keypad: layout and multi-touch hit testing.
//
// The M8's eight keys are laid out in a 160x272 panel beside the M8 screen. They keep the
// M8's grouping, folded to fit the column: SHIFT/PLAY over OPTION/EDIT, then the cursor
// cluster as an inverted T. Hit areas tile the panel with no dead space between keys, so a
// finger sliding across the cursor keys moves cleanly from one to the next.
#pragma once

#include <stdint.h>

#include "input/gt911.h"

namespace keypad {

enum class Icon : uint8_t { kNone, kUp, kDown, kLeft, kRight };

struct Key {
  const char *label;
  uint8_t bit; // m8proto::Key
  Icon icon;
  int16_t x, y, w, h; // hit rect, LCD coordinates
  int16_t vy, vh;     // drawn extent (the hit rect also covers half the row divider)
};

constexpr int kKeyCount = 8;
constexpr int kInset = 3; // visual gap inside each hit rect

const Key *keys();

// Bitmask of M8 keys currently held by any touch point.
uint8_t mask_for(const gt911::Point *points, int count);

} // namespace keypad
