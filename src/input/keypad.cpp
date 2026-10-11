#include "keypad.h"

#include <stdlib.h>

#include "m8/protocol.h"

namespace keypad {

uint8_t mask_for(const touch::Point *points, int count) {
  const Table &t = table();
  const Key *k = t.keys;
  const int n = t.count;
  uint8_t mask = 0;
  for (int i = 0; i < count; i++) {
    const int px = points[i].x, py = points[i].y;
    for (int j = 0; j < n; j++) {
      if (px >= k[j].x && px < k[j].x + k[j].w && py >= k[j].y && py < k[j].y + k[j].h) {
        mask |= k[j].bit;
        break;
      }
    }
  }
  return mask;
}

uint8_t pad_key(const Pad &p, int x, int y) {
  if (x < p.x || x >= p.x + p.w || y < p.y || y >= p.y + p.h) return 0;
  const int dx = x - p.cx, dy = y - p.cy;
  if (abs(dx) > abs(dy)) return dx > 0 ? m8proto::kKeyRight : m8proto::kKeyLeft;
  return dy > 0 ? m8proto::kKeyDown : m8proto::kKeyUp;
}

uint8_t pad_keys(const Pad &) {
  return m8proto::kKeyUp | m8proto::kKeyDown | m8proto::kKeyLeft | m8proto::kKeyRight;
}

} // namespace keypad
