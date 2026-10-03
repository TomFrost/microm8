// Touch through M5Unified: GT911, ST7123 or ST7121 depending on the Tab5's revision (M5GFX
// detects which). Coordinates follow the display rotation set in display/ui.cpp.
#include "input/touch.h"

#include <M5Unified.h>

#include "board.h"

namespace touch {

bool begin() { return M5.Touch.isEnabled(); }

bool read(Point *points, int *count) {
  {
    board::I2CLock lock;
    M5.update();
  }

  int n = 0;
  const int reported = M5.Touch.getCount();
  for (int i = 0; i < reported && n < kMaxPoints; i++) {
    const auto &d = M5.Touch.getDetail(i);
    if (!d.isPressed()) continue;
    points[n++] = {(int16_t)d.x, (int16_t)d.y, (uint8_t)d.id};
  }
  *count = n;
  return true;
}

} // namespace touch
