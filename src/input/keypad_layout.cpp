// Key layouts.
//
// Portrait: the M8's own layout across the bottom of the screen. Keys sit on a regular grid
// (KEY_SIZE keys, KEY_SPACING apart), with two groups pushed out by KEY_OFFSET:
//
//                         [OPTION][ EDIT ]   <- up and right by KEY_OFFSET
//            [ UP ]
//    [LEFT ] [DOWN] [RIGHT]
//                                            <- extra KEY_OFFSET gap
//            [SHIFT][ PLAY]                  <- under DOWN and RIGHT
//
// Portrait hit rects extend half the key spacing beyond each key, and to the screen edge for
// outer keys, so near misses still register without neighbouring keys overlapping.
//
// Landscape: in the margins either side of the M8 screen. The arrows are swipes (main.cpp),
// or taps on the arrow pad in the left panel (display/ui.cpp).
//
//    (icons)  +------------+
//             |            |  [ EDIT ]
//    (arrows) |     M8     |  [OPTION]
//             |            |  [ PLAY ]
//    [SHIFT]  +------------+  [SHIFT ]
#include "input/keypad.h"

#include <atomic>

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
static_assert(kOriginY - KEY_SPACING / 2 >= PORT_ROW_Y + PORT_ROW_H,
              "portrait keypad overlaps the slider row");

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

const Key kPortraitKeys[] = {
    make_key("OPTION", kKeyOpt, Icon::kNone, kCol2 + KEY_OFFSET, kRowTop),
    make_key("EDIT", kKeyEdit, Icon::kNone, kCol3 + KEY_OFFSET, kRowTop),
    make_key("UP", kKeyUp, Icon::kUp, kCol1, kRowUp),
    make_key("LEFT", kKeyLeft, Icon::kLeft, kCol0, kRowMid),
    make_key("DOWN", kKeyDown, Icon::kDown, kCol1, kRowMid),
    make_key("RIGHT", kKeyRight, Icon::kRight, kCol2, kRowMid),
    make_key("SHIFT", kKeySelect, Icon::kNone, kCol1, kRowBottom),
    make_key("PLAY", kKeyStart, Icon::kNone, kCol2, kRowBottom),
};

// Landscape: 1280x720, keys in the margins either side of the M8 screen.
constexpr int kLandW = 1280;
constexpr int kLandH = 720;
constexpr int kLPitch = LKEY_SIZE + LKEY_GAP;
constexpr int kLLeftX = LKEY_MARGIN;
constexpr int kLRightX = kLandW - LKEY_MARGIN - LKEY_SIZE;
constexpr int kLRowY3 = kLandH - LKEY_MARGIN - LKEY_SIZE; // the bottom row
constexpr int kLRowY0 = kLRowY3 - 3 * kLPitch;            // EDIT, the top of the column
static_assert(kLRowY0 >= 0, "landscape key column is taller than the screen");
static_assert(LKEY_SIZE + 2 * LKEY_MARGIN <= LAND_M8_X, "landscape keys are wider than the margin");
static_assert(LAND_ROW_Y + LAND_ROW_H <= kLRowY3 - LKEY_GAP / 2, "landscape panel overlaps SHIFT");

// Hit rects reach a little into the M8 screen, which has no other touch function on the right,
// and only on the left beside SHIFT (the rest of the left half is for swipes).
constexpr int kLReach = 16;
constexpr int kLReachUp = 40; // above EDIT, which nothing else needs

constexpr Key make_landscape_key(const char *label, uint8_t bit, int vx, int vy) {
  const int slop = LKEY_GAP / 2;
  int x0 = vx == kLLeftX ? 0 : LAND_M8_X + LAND_M8_W - kLReach;
  int x1 = vx == kLLeftX ? LAND_M8_X + kLReach : kLandW;
  int y0 = vy == kLRowY0 ? vy - kLReachUp : vy - slop;
  int y1 = vy == kLRowY3 ? kLandH : vy + LKEY_SIZE + slop;
  return Key{label,
             bit,
             Icon::kNone,
             (int16_t)x0,
             (int16_t)y0,
             (int16_t)(x1 - x0),
             (int16_t)(y1 - y0),
             (int16_t)vx,
             (int16_t)vy,
             LKEY_SIZE,
             LKEY_SIZE};
}

const Key kLandscapeKeys[] = {
    make_landscape_key("EDIT", kKeyEdit, kLRightX, kLRowY0),
    make_landscape_key("OPTION", kKeyOpt, kLRightX, kLRowY0 + kLPitch),
    make_landscape_key("PLAY", kKeyStart, kLRightX, kLRowY0 + 2 * kLPitch),
    make_landscape_key("SHIFT", kKeySelect, kLRightX, kLRowY3),
    make_landscape_key("SHIFT", kKeySelect, kLLeftX, kLRowY3),
};

constexpr int kPortraitCount = sizeof(kPortraitKeys) / sizeof(kPortraitKeys[0]);
constexpr int kLandscapeCount = sizeof(kLandscapeKeys) / sizeof(kLandscapeKeys[0]);
static_assert(kPortraitCount <= kMaxKeys && kLandscapeCount <= kMaxKeys, "raise kMaxKeys");

const Table kPortraitTable = {kPortraitKeys, kPortraitCount};
const Table kLandscapeTable = {kLandscapeKeys, kLandscapeCount};
std::atomic<const Table *> active{&kPortraitTable};

} // namespace

void set_arrangement(Arrangement arrangement) {
  active = arrangement == Arrangement::kLandscape ? &kLandscapeTable : &kPortraitTable;
}

Arrangement arrangement() {
  return active.load() == &kLandscapeTable ? Arrangement::kLandscape : Arrangement::kPortrait;
}

const Table &table() { return *active.load(); }

} // namespace keypad
