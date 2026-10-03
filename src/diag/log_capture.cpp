#include "log_capture.h"

#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace logcap {
namespace {

vprintf_like_t forward;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
Line ring[kLines];
int next_slot;
int stored;
std::atomic<uint32_t> ver{0};

// Turns "\e[0;31mE (12345) usb: message\e[0m\n" into level 'E' and "usb: message".
void clean(const char *raw, Line *out) {
  char plain[kLineLen];
  int n = 0;
  for (const char *p = raw; *p && n < (int)sizeof(plain) - 1; p++) {
    if (*p == '\033') { // skip ANSI colour sequences
      while (*p && *p != 'm') p++;
      if (!*p) break;
      continue;
    }
    if (*p == '\r' || *p == '\n') continue;
    plain[n++] = *p;
  }
  plain[n] = '\0';

  const char *text = plain;
  out->level = ' ';
  if (strchr("EWIDV", plain[0]) && plain[1] == ' ' && plain[2] == '(') {
    out->level = plain[0];
    const char *close = strchr(plain, ')');
    if (close) text = close[1] == ' ' ? close + 2 : close + 1;
  }
  strncpy(out->text, text, kLineLen - 1);
  out->text[kLineLen - 1] = '\0';
}

int hook(const char *fmt, va_list args) {
  char raw[224]; // room for colour codes and the timestamp prefix
  va_list copy;
  va_copy(copy, args);
  vsnprintf(raw, sizeof(raw), fmt, copy);
  va_end(copy);

  Line line;
  clean(raw, &line);
  if (line.text[0]) {
    portENTER_CRITICAL(&lock);
    ring[next_slot] = line;
    next_slot = (next_slot + 1) % kLines;
    if (stored < kLines) stored++;
    portEXIT_CRITICAL(&lock);
    ver++;
  }

  return forward ? forward(fmt, args) : vprintf(fmt, args);
}

} // namespace

void begin() { forward = esp_log_set_vprintf(hook); }

uint32_t version() { return ver; }

int copy_lines(Line *out, int max) {
  portENTER_CRITICAL(&lock);
  const int n = stored < max ? stored : max;
  const int first = (next_slot - n + kLines) % kLines;
  for (int i = 0; i < n; i++) out[i] = ring[(first + i) % kLines];
  portEXIT_CRITICAL(&lock);
  return n;
}

} // namespace logcap
