// On-screen M8 keypad: the key layouts (keypad_layout.cpp) and multi-touch hit testing.
//
// A key's hit rect may be larger than its drawn rect, so fingers sliding between
// neighbouring keys don't fall into gaps. Pads group several keys round a centre, each key
// owning a wedge of the pad's touch area, so the whole area is live.
#pragma once

#include <stdint.h>

#include "input/touch.h"

namespace keypad {

enum class Icon : uint8_t { kNone, kUp, kDown, kLeft, kRight, kEdit, kOption, kPlay, kShift };

struct Key {
  const char *label;
  uint8_t bit; // m8proto::Key
  Icon icon;
  int16_t x, y, w, h;     // hit rect, screen coordinates
  int16_t vx, vy, vw, vh; // drawn rect (before kInset); the hit rect may extend past it
};

constexpr int kMaxKeys = 9;
constexpr int kInset = 3; // drawn keys are inset by this much from their drawn rect

// The arrow pad: UP, DOWN, LEFT and RIGHT in a diamond. The diagonals through its centre split
// the touch area into one wedge per arrow.
struct Pad {
  int16_t cx, cy;      // centre
  int16_t half;        // half-size of the square the pressed key's wedge is outlined in
  int16_t icon_offset; // icon centres from the centre
  int16_t icon_size;
  int16_t x, y, w, h; // touch area
  bool in_panel;      // the arrows in landscape's left panel: live only while it shows them
};

// Portrait: the M8's own layout. Landscape: keys in the side margins, SHIFT on both sides, and
// an arrow pad (plus swipes). Compact: the arrow pad for the left thumb and the M8's
// OPTION/EDIT/SHIFT/PLAY block for the right, with OPTION and SHIFT under the left thumb too.
enum class Arrangement : uint8_t { kPortrait, kLandscape };

// Switches the active layout (from the UI task; the touch task picks it up atomically).
void set_arrangement(Arrangement arrangement, bool compact);

struct Table {
  const Key *keys;
  int count;
  const Pad *pads;
  int pad_count;
};

// The active layout's keys (defined in keypad_layout.cpp). Read once and use the result, so
// keys and count always belong to the same layout.
const Table &table();

// Bitmask of M8 keys currently held by any touch point (keys only, not pads).
uint8_t mask_for(const touch::Point *points, int count);

// The key under (x, y) in a pad's touch area, or 0 outside it.
uint8_t pad_key(const Pad &pad, int x, int y);

// All the keys on a pad (the arrows).
uint8_t pad_keys(const Pad &pad);

} // namespace keypad
