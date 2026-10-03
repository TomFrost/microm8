// The M8's key layout, across the bottom of the portrait screen. Keys sit on a regular grid
// (KEY_SIZE keys, KEY_SPACING apart), with two groups pushed out by KEY_OFFSET:
//
//                         [OPTION][ EDIT ]   <- up and right by KEY_OFFSET
//            [ UP ]
//    [LEFT ] [DOWN] [RIGHT]
//                                            <- extra KEY_OFFSET gap
//            [SHIFT][ PLAY]                  <- under DOWN and RIGHT
//
// Hit rects extend half the key spacing beyond each key, and to the screen edge for outer
// keys, so near misses still register without neighbouring keys overlapping.
#include "input/keypad.h"

#include "config.h"
#include "m8/protocol.h"

namespace keypad {
namespace {

using namespace m8proto;

constexpr int kPitch = KEY_SIZE + KEY_SPACING;

constexpr int kWidth = 4 * KEY_SIZE + 3 * KEY_SPACING + KEY_OFFSET;
constexpr int kHeight = 3 * KEY_SIZE + 2 * KEY_SPACING + 2 * KEY_OFFSET;
constexpr int kOriginX = (LCD_WIDTH - kWidth) / 2;
constexpr int kOriginY = LCD_HEIGHT - KEY_BOTTOM_MARGIN - kHeight;
static_assert(kOriginX >= 0, "keypad is wider than the screen");
static_assert(kOriginY >= STATUS_Y + STATUS_H, "keypad overlaps the status bar");

// Grid columns and rows.
constexpr int kCol0 = kOriginX;
constexpr int kCol1 = kCol0 + kPitch;
constexpr int kCol2 = kCol1 + kPitch;
constexpr int kCol3 = kCol2 + kPitch;
constexpr int kRowTop = kOriginY;                         // OPTION, EDIT
constexpr int kRowUp = kRowTop + KEY_OFFSET;              // UP
constexpr int kRowMid = kRowUp + kPitch;                  // LEFT, DOWN, RIGHT
constexpr int kRowBottom = kRowMid + kPitch + KEY_OFFSET; // SHIFT, PLAY

constexpr int kSlop = KEY_SPACING / 2;

constexpr Key make_key(const char *label, uint8_t bit, Icon icon, int vx, int vy) {
  int x0 = vx - kSlop, x1 = vx + KEY_SIZE + kSlop;
  int y0 = vy - kSlop, y1 = vy + KEY_SIZE + kSlop;
  if (vx == kCol0) x0 = 0;                             // LEFT: to the screen edge
  if (vx == kCol3 + KEY_OFFSET) x1 = LCD_WIDTH;        // EDIT
  if (vy == kRowBottom) y1 = LCD_HEIGHT;               // SHIFT, PLAY
  return Key{label,
             bit,
             icon,
             (int16_t)x0,
             (int16_t)y0,
             (int16_t)(x1 - x0),
             (int16_t)(y1 - y0),
             (int16_t)vx,
             (int16_t)vy,
             KEY_SIZE,
             KEY_SIZE};
}

const Key kKeys[kKeyCount] = {
    make_key("OPTION", kKeyOpt, Icon::kNone, kCol2 + KEY_OFFSET, kRowTop),
    make_key("EDIT", kKeyEdit, Icon::kNone, kCol3 + KEY_OFFSET, kRowTop),
    make_key("UP", kKeyUp, Icon::kUp, kCol1, kRowUp),
    make_key("LEFT", kKeyLeft, Icon::kLeft, kCol0, kRowMid),
    make_key("DOWN", kKeyDown, Icon::kDown, kCol1, kRowMid),
    make_key("RIGHT", kKeyRight, Icon::kRight, kCol2, kRowMid),
    make_key("SHIFT", kKeySelect, Icon::kNone, kCol1, kRowBottom),
    make_key("PLAY", kKeyStart, Icon::kNone, kCol2, kRowBottom),
};

} // namespace

const Key *keys() { return kKeys; }

} // namespace keypad
