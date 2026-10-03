// JC4827W543 key layout: a 160x272 column beside the M8 screen. The M8's grouping is kept but
// folded to fit: SHIFT/PLAY over OPTION/EDIT, then the cursor cluster as an inverted T. Hit
// rects tile the column with no dead space; the 16 px divider between the groups is split
// between their hit rects.
#include "input/keypad.h"

#include "config.h"
#include "m8/protocol.h"

namespace keypad {
namespace {

using namespace m8proto;

constexpr int kRowH = 64;
constexpr int kDivider = 16;
constexpr int kActionW = PANEL_W / 2;  // 80
constexpr int kCursorW = PANEL_W / 3;  // 53
constexpr int kCursorMidW = PANEL_W - 2 * kCursorW;

constexpr int kRow0 = 0;
constexpr int kRow1 = kRowH;
constexpr int kRow2 = 2 * kRowH + kDivider;
constexpr int kRow3 = 3 * kRowH + kDivider;

constexpr int kRow1H = kRowH + kDivider / 2;
constexpr int kRow2Y = kRow2 - kDivider / 2;
constexpr int kRow2H = kRowH + kDivider / 2;

constexpr int kX0 = PANEL_X;

constexpr int kRow3H = LCD_HEIGHT - kRow3;
constexpr int kActionRightW = PANEL_W - kActionW;
constexpr int kCursorMidX = kX0 + kCursorW;
constexpr int kCursorRightX = kX0 + kCursorW + kCursorMidW;
constexpr int kActionRightX = kX0 + kActionW;

// clang-format off
const Key kKeys[kKeyCount] = {
    // label     bit         icon          hit: x          y       w              h       drawn: x        y      w              h
    {"SHIFT",  kKeySelect, Icon::kNone,  kX0,           kRow0,  kActionW,      kRowH,  kX0,           kRow0, kActionW,      kRowH},
    {"PLAY",   kKeyStart,  Icon::kNone,  kActionRightX, kRow0,  kActionRightW, kRowH,  kActionRightX, kRow0, kActionRightW, kRowH},
    {"OPTION", kKeyOpt,    Icon::kNone,  kX0,           kRow1,  kActionW,      kRow1H, kX0,           kRow1, kActionW,      kRowH},
    {"EDIT",   kKeyEdit,   Icon::kNone,  kActionRightX, kRow1,  kActionRightW, kRow1H, kActionRightX, kRow1, kActionRightW, kRowH},
    {"UP",     kKeyUp,     Icon::kUp,    kCursorMidX,   kRow2Y, kCursorMidW,   kRow2H, kCursorMidX,   kRow2, kCursorMidW,   kRowH},
    {"LEFT",   kKeyLeft,   Icon::kLeft,  kX0,           kRow3,  kCursorW,      kRow3H, kX0,           kRow3, kCursorW,      kRow3H},
    {"DOWN",   kKeyDown,   Icon::kDown,  kCursorMidX,   kRow3,  kCursorMidW,   kRow3H, kCursorMidX,   kRow3, kCursorMidW,   kRow3H},
    {"RIGHT",  kKeyRight,  Icon::kRight, kCursorRightX, kRow3,  kCursorW,      kRow3H, kCursorRightX, kRow3, kCursorW,      kRow3H},
};
// clang-format on

} // namespace

const Key *keys() { return kKeys; }

} // namespace keypad
