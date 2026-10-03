#include "keypad.h"

namespace keypad {

uint8_t mask_for(const touch::Point *points, int count) {
  const Key *k = keys();
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int px = points[i].x, py = points[i].y;
    for (int j = 0; j < kKeyCount; j++) {
      if (px >= k[j].x && px < k[j].x + k[j].w && py >= k[j].y && py < k[j].y + k[j].h) {
        mask |= k[j].bit;
        break;
      }
    }
  }
  return mask;
}

} // namespace keypad
