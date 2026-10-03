// Tab5 key layout: the M8's own arrangement across the bottom of the portrait screen, for
// two-thumb play. Cursor keys as an inverted T on the left; SHIFT/PLAY over OPTION/EDIT on
// the right.
//
//        [ UP ]              [SHIFT][PLAY]
//  [LEFT][DOWN][RIGHT]       [ OPT ][EDIT]
//
// Hit rects extend half the key spacing beyond each key, and to the screen edge for keys on
// the outside, so there are no dead zones within a group. The wider gap between the two
// groups stays dead, so a thumb on the cursor keys can't catch SHIFT or OPTION.
#include "input/keypad.h"

#include "config.h"
#include "m8/protocol.h"

namespace keypad {
namespace {

using namespace m8proto;

constexpr int kStep = KEY_SIZE + KEY_SPACING;

// Columns
constexpr int kCursorX0 = KEY_MARGIN;
constexpr int kCursorX1 = kCursorX0 + kStep;
constexpr int kCursorX2 = kCursorX1 + kStep;
constexpr int kActionX0 = kCursorX2 + KEY_SIZE + KEY_GROUP_GAP;
constexpr int kActionX1 = kActionX0 + kStep;
static_assert(kActionX1 + KEY_SIZE + KEY_MARGIN == LCD_WIDTH, "keypad must span the width");

// Rows
constexpr int kRowLower = LCD_HEIGHT - KEY_BOTTOM_MARGIN - KEY_SIZE;
constexpr int kRowUpper = kRowLower - kStep;

constexpr int kSlop = KEY_SPACING / 2;

constexpr Key make_key(const char *label, uint8_t bit, Icon icon, int vx, int vy) {
  int x0 = vx - kSlop, x1 = vx + KEY_SIZE + kSlop;
  int y0 = vy - kSlop, y1 = vy + KEY_SIZE + kSlop;
  if (vx == KEY_MARGIN) x0 = 0;
  if (vx + KEY_SIZE + KEY_MARGIN == LCD_WIDTH) x1 = LCD_WIDTH;
  if (vy == kRowLower) y1 = LCD_HEIGHT;
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
    make_key("UP", kKeyUp, Icon::kUp, kCursorX1, kRowUpper),
    make_key("LEFT", kKeyLeft, Icon::kLeft, kCursorX0, kRowLower),
    make_key("DOWN", kKeyDown, Icon::kDown, kCursorX1, kRowLower),
    make_key("RIGHT", kKeyRight, Icon::kRight, kCursorX2, kRowLower),
    make_key("SHIFT", kKeySelect, Icon::kNone, kActionX0, kRowUpper),
    make_key("PLAY", kKeyStart, Icon::kNone, kActionX1, kRowUpper),
    make_key("OPTION", kKeyOpt, Icon::kNone, kActionX0, kRowLower),
    make_key("EDIT", kKeyEdit, Icon::kNone, kActionX1, kRowLower),
};

} // namespace

const Key *keys() { return kKeys; }

} // namespace keypad
