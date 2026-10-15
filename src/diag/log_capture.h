// Captures ESP-IDF log output (while still forwarding it to the serial console) so the most
// recent lines can be shown on screen, for debugging without a computer.
#pragma once

#include <stdint.h>

namespace logcap {

constexpr int kLines = 12;
constexpr int kLineLen = 160; // long lines are wrapped for display

struct Line {
  char level; // 'E', 'W', 'I', 'D', 'V', or ' ' if unknown
  char text[kLineLen];
};

void begin();

// Changes whenever a new line is captured.
uint32_t version();

// Copies up to `max` of the most recent lines, oldest first. Returns the number copied.
int copy_lines(Line *out, int max);

} // namespace logcap
