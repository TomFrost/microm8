#include "keypad.h"

#include "config.h"
#include "m8/protocol.h"

namespace keypad {
namespace {

using namespace m8proto;

// Panel rows: two action rows, a 16 px divider, then the cursor cluster. The divider is
// split between the neighbouring hit rects.
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
constexpr int kCursorRightX = kX0 + kCursorW + kCursorMidW;

// clang-format off
const Key kKeys[kKeyCount] = {
    // label     bit         icon          hit x               hit y   hit w          hit h   draw y draw h
    {"SHIFT",  kKeySelect, Icon::kNone,  kX0,                kRow0,  kActionW,      kRowH,  kRow0, kRowH},
    {"PLAY",   kKeyStart,  Icon::kNone,  kX0 + kActionW,     kRow0,  kActionRightW, kRowH,  kRow0, kRowH},
    {"OPTION", kKeyOpt,    Icon::kNone,  kX0,                kRow1,  kActionW,      kRow1H, kRow1, kRowH},
    {"EDIT",   kKeyEdit,   Icon::kNone,  kX0 + kActionW,     kRow1,  kActionRightW, kRow1H, kRow1, kRowH},
    {"UP",     kKeyUp,     Icon::kUp,    kX0 + kCursorW,     kRow2Y, kCursorMidW,   kRow2H, kRow2, kRowH},
    {"LEFT",   kKeyLeft,   Icon::kLeft,  kX0,                kRow3,  kCursorW,      kRow3H, kRow3, kRow3H},
    {"DOWN",   kKeyDown,   Icon::kDown,  kX0 + kCursorW,     kRow3,  kCursorMidW,   kRow3H, kRow3, kRow3H},
    {"RIGHT",  kKeyRight,  Icon::kRight, kCursorRightX,      kRow3,  kCursorW,      kRow3H, kRow3, kRow3H},
};
// clang-format on

} // namespace

const Key *keys() { return kKeys; }

uint8_t mask_for(const gt911::Point *points, int count) {
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int px = points[i].x, py = points[i].y;
    for (const Key &k : kKeys) {
      if (px >= k.x && px < k.x + k.w && py >= k.y && py < k.y + k.h) {
        mask |= k.bit;
        break;
      }
    }
  }
  return mask;
}

} // namespace keypad
