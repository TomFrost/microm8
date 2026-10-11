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
// or taps on the arrow pad in the left panel.
//
//    (icons)  +------------+  (icons)
//    (arrows) |            |  [ EDIT ]
//             |     M8     |  [OPTION]
//    [OPTION] |            |  [ PLAY ]
//    [SHIFT]  +------------+  [SHIFT ]
//
// Compact: symbols, the arrow pad opposite the M8's right-hand block. Landscape (in the
// margins), and portrait (replacing the keypad):
//
//       ^           [OPT ][EDIT]            ^      [OPT ][EDIT]
//     <   >   ...   [SHIFT][PLAY]         <   >
//       v                                   v      [SHIFT][PLAY]
//    [OPT ][SHIFT]
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

constexpr Key make_key_at(const char *label, uint8_t bit, Icon icon, int x0, int y0, int x1,
                          int y1, int vx, int vy, int vw, int vh) {
  return Key{label,           bit,          icon,         (int16_t)x0,  (int16_t)y0,
             (int16_t)(x1 - x0), (int16_t)(y1 - y0), (int16_t)vx, (int16_t)vy, (int16_t)vw,
             (int16_t)vh};
}

// Landscape: 1280x720, keys in the margins either side of the M8 screen, in low rows up from
// the bottom. Touch areas reach the screen edges, a little into the M8 screen (which has no
// other touch function on the right; on the left only beside the keys, the rest being for
// swipes), and up the right margin to below the icons.
constexpr int kLandW = 1280;
constexpr int kLandH = 720;
constexpr int kLReach = 16;
constexpr int kLIconsBottom = 204;        // the power, key layout and HOLD icons
constexpr int kLPadY = 418;               // the arrow pad's centre
constexpr int kLPitch = LKEY_H + LKEY_GAP;
constexpr int kLRow3 = kLandH - LKEY_MARGIN - LKEY_H; // the bottom row
constexpr int kLRow2 = kLRow3 - kLPitch;
constexpr int kLRow0 = kLRow3 - 3 * kLPitch;
constexpr int kLSlop = LKEY_GAP / 2;
constexpr int kLLeftX = LKEY_MARGIN;
constexpr int kLRightX = kLandW - LKEY_MARGIN - LKEY_W;
constexpr int kLReachL = LAND_M8_X + kLReach;           // left keys' touch areas end here
constexpr int kLReachR = LAND_M8_X + LAND_M8_W - kLReach; // right keys' start here
static_assert(LKEY_W + 2 * LKEY_MARGIN <= LAND_M8_X, "landscape keys are wider than the margin");
static_assert(kLRow0 - kLSlop >= kLIconsBottom, "landscape keys overlap the icons");
static_assert(LAND_ROW_Y + LAND_ROW_H <= kLRow2 - kLSlop, "landscape panel overlaps the keys");
static_assert(kLPadY - LAND_M8_X / 2 >= LAND_ROW_Y && kLPadY + LAND_M8_X / 2 <= LAND_ROW_Y + LAND_ROW_H,
              "landscape arrow pad doesn't fit the panel");

// A key in landscape's right column (rows 0..3) or the left (rows 2..3).
constexpr Key make_landscape_key(const char *label, uint8_t bit, bool left, int row) {
  const int vy = kLRow3 - (3 - row) * kLPitch;
  const int y0 = left ? vy - kLSlop : (row == 0 ? kLIconsBottom : vy - kLSlop);
  const int y1 = row == 3 ? kLandH : vy + LKEY_H + kLSlop;
  return left ? make_key_at(label, bit, Icon::kNone, 0, y0, kLReachL, y1, kLLeftX, vy, LKEY_W,
                            LKEY_H)
              : make_key_at(label, bit, Icon::kNone, kLReachR, y0, kLandW, y1, kLRightX, vy,
                            LKEY_W, LKEY_H);
}

const Key kLandscapeKeys[] = {
    make_landscape_key("EDIT", kKeyEdit, false, 0),
    make_landscape_key("OPTION", kKeyOpt, false, 1),
    make_landscape_key("PLAY", kKeyStart, false, 2),
    make_landscape_key("SHIFT", kKeySelect, false, 3),
    make_landscape_key("OPTION", kKeyOpt, true, 2),
    make_landscape_key("SHIFT", kKeySelect, true, 3),
};

// The landscape arrow pad, in the left panel (which shows sliders and messages in its place).
const Pad kLandscapeArrows = {LAND_M8_X / 2, kLPadY, LAND_M8_X / 2, 50,        15,
                              0,             LAND_ROW_Y, kLReachL,  LAND_ROW_H, true};

// Landscape compact: the M8's own right-hand block (OPTION and EDIT over SHIFT and PLAY) as a
// square grid level with the arrow pad, and OPTION and SHIFT again in a low row under the left
// thumb, so every chord has a key under each thumb (OPTION+EDIT included: one thumb can't press
// two keys on a touchscreen). Keys show symbols. The block's touch areas run up to the icons
// and down to the bottom of the screen.
constexpr int kLCompactGap = 8;
constexpr int kLCompactW = (LAND_M8_X - 3 * kLCompactGap) / 2; // a square key in the grid
constexpr int kLOuterL = kLCompactGap;
constexpr int kLInnerL = kLOuterL + kLCompactW + kLCompactGap;
constexpr int kLInnerR = LAND_M8_X + LAND_M8_W + kLCompactGap;
constexpr int kLOuterR = kLInnerR + kLCompactW + kLCompactGap;
// The compact arrow pad sits lower than normal mode's (its UP where the normal pad's LEFT and
// RIGHT are), reaching below the panel down to the lower keys; the grid is level with it.
constexpr int kLCompactPadY = kLPadY + 50;
constexpr int kLGridTop = kLCompactPadY - kLCompactW - kLCompactGap / 2;
constexpr int kLGridBottom = kLCompactPadY + kLCompactGap / 2;
// OPTION and SHIFT under the left thumb sit low, in the bottom row, well clear of DOWN (closer,
// they caught taps meant for it); the pad's touch area reaches down to theirs.
constexpr int kLCompactPadOffset = 50; // the pad's arrows from its centre
constexpr int kLLowerTop = kLRow3 - kLSlop;  // where their touch areas start
constexpr int kLSplitL = LAND_M8_X / 2;
static_assert(kLCompactPadY + LAND_M8_X / 2 <= kLLowerTop, "compact arrow pad hits the keys");
constexpr int kLSplitR = kLOuterR - kLCompactGap / 2;

const Key kLandscapeCompactKeys[] = {
    make_key_at("OPTION", kKeyOpt, Icon::kOption, 0, kLLowerTop, kLSplitL, kLandH, kLOuterL,
                kLRow3, kLCompactW, LKEY_H),
    make_key_at("SHIFT", kKeySelect, Icon::kShift, kLSplitL, kLLowerTop, kLReachL, kLandH,
                kLInnerL, kLRow3, kLCompactW, LKEY_H),
    make_key_at("OPTION", kKeyOpt, Icon::kOption, kLReachR, kLIconsBottom, kLSplitR,
                kLCompactPadY, kLInnerR, kLGridTop, kLCompactW, kLCompactW),
    make_key_at("EDIT", kKeyEdit, Icon::kEdit, kLSplitR, kLIconsBottom, kLandW, kLCompactPadY,
                kLOuterR, kLGridTop, kLCompactW, kLCompactW),
    make_key_at("SHIFT", kKeySelect, Icon::kShift, kLReachR, kLCompactPadY, kLSplitR, kLandH,
                kLInnerR, kLGridBottom, kLCompactW, kLCompactW),
    make_key_at("PLAY", kKeyStart, Icon::kPlay, kLSplitR, kLCompactPadY, kLandW, kLandH, kLOuterR,
                kLGridBottom, kLCompactW, kLCompactW),
};
const Pad kLandscapeCompactArrows = {LAND_M8_X / 2,
                                     kLCompactPadY,
                                     LAND_M8_X / 2,
                                     kLCompactPadOffset,
                                     15,
                                     0,
                                     LAND_ROW_Y,
                                     kLReachL,
                                     kLLowerTop - LAND_ROW_Y,
                                     true};

// Portrait compact: the arrow pad for the left thumb and the M8's right-hand block, full-size,
// for the right, centred between the status bar and the bottom of the screen, the pad on the
// block's two rows. Touch areas reach the bottom of the screen.
constexpr int kCMargin = 20;
constexpr int kCTop = PORT_ROW_Y + PORT_ROW_H + 8;
constexpr int kCHalf = LCD_WIDTH / 2;
constexpr int kCMid = (PORT_STATUS_Y + PORT_STATUS_H + LCD_HEIGHT) / 2;
constexpr int kCRow0 = kCMid - KEY_SPACING / 2 - KEY_SIZE;
constexpr int kCRow1 = kCMid + KEY_SPACING / 2;
constexpr int kCSplitY = kCMid;
static_assert(kCRow0 - KEY_SPACING / 2 >= kCTop, "portrait compact keys reach the slider row");
constexpr int kCOuterR = LCD_WIDTH - kCMargin - KEY_SIZE;
constexpr int kCInnerR = kCOuterR - KEY_SPACING - KEY_SIZE;
constexpr int kCSplitX = kCOuterR - KEY_SPACING / 2;
constexpr int kCPadY = kCMid;
constexpr int kCPadHalf = 150;
static_assert(kCInnerR >= kCHalf, "portrait compact keys cross the middle");
static_assert(kCPadY + kCPadHalf <= LCD_HEIGHT && kCPadY - kCPadHalf >= kCTop,
              "portrait compact arrow pad doesn't fit");

const Key kPortraitCompactKeys[] = {
    make_key_at("OPTION", kKeyOpt, Icon::kOption, kCHalf, kCTop, kCSplitX, kCSplitY, kCInnerR,
                kCRow0, KEY_SIZE, KEY_SIZE),
    make_key_at("EDIT", kKeyEdit, Icon::kEdit, kCSplitX, kCTop, LCD_WIDTH, kCSplitY, kCOuterR,
                kCRow0, KEY_SIZE, KEY_SIZE),
    make_key_at("SHIFT", kKeySelect, Icon::kShift, kCHalf, kCSplitY, kCSplitX, LCD_HEIGHT,
                kCInnerR, kCRow1, KEY_SIZE, KEY_SIZE),
    make_key_at("PLAY", kKeyStart, Icon::kPlay, kCSplitX, kCSplitY, LCD_WIDTH, LCD_HEIGHT,
                kCOuterR, kCRow1, KEY_SIZE, KEY_SIZE),
};
const Pad kPortraitCompactArrows = {kCHalf / 2, kCPadY, kCPadHalf, 95,    24,
                                    0,          kCTop,  kCHalf,    LCD_HEIGHT - kCTop, false};

template <typename T, int N> constexpr int count_of(const T (&)[N]) { return N; }
static_assert(count_of(kPortraitKeys) <= kMaxKeys && count_of(kLandscapeKeys) <= kMaxKeys &&
                  count_of(kPortraitCompactKeys) <= kMaxKeys &&
                  count_of(kLandscapeCompactKeys) <= kMaxKeys,
              "raise kMaxKeys");

const Table kPortraitTable = {kPortraitKeys, count_of(kPortraitKeys), nullptr, 0};
const Table kLandscapeTable = {kLandscapeKeys, count_of(kLandscapeKeys), &kLandscapeArrows, 1};
const Table kPortraitCompactTable = {kPortraitCompactKeys, count_of(kPortraitCompactKeys),
                                     &kPortraitCompactArrows, 1};
const Table kLandscapeCompactTable = {kLandscapeCompactKeys, count_of(kLandscapeCompactKeys),
                                      &kLandscapeCompactArrows, 1};
std::atomic<const Table *> active{&kPortraitTable};

} // namespace

void set_arrangement(Arrangement arrangement, bool compact) {
  if (arrangement == Arrangement::kLandscape) {
    active = compact ? &kLandscapeCompactTable : &kLandscapeTable;
  } else {
    active = compact ? &kPortraitCompactTable : &kPortraitTable;
  }
}

const Table &table() { return *active.load(); }

} // namespace keypad
