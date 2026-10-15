// Adapted from m8c (MIT); see LICENSE-THIRD-PARTY.md.

#include "protocol.h"

#include <esp_log.h>

#include "screen.h"
#include "slip.h"

static const char *TAG = "m8proto";

namespace m8proto {
namespace {

enum : uint8_t {
  kDrawRect = 0xFE,
  kDrawChar = 0xFD,
  kDrawScope = 0xFC,
  kJoypadState = 0xFB,
  kSystemInfo = 0xFF,
};

enum : uint32_t {
  kRectPos = 5,
  kRectPosColor = 8,
  kRectPosSize = 9,
  kRectPosSizeColor = 12,
  kCharLen = 12,
  kScopeMin = 1 + 3,
  kScopeMax = 1 + 3 + 480,
  kJoypadLen = 3,
  kSystemInfoLen = 6,
};

uint8_t slip_buf[1024];
slip_handler_t slip;
DeviceInfo info;
uint32_t ok_count, bad_count;

// Rectangles may omit color; the last one sent is reused.
m8screen::Rgb last_rect_color = {0, 0, 0};

inline uint16_t u16(const uint8_t *d, int at) { return d[at] | (d[at + 1] << 8); }

void dump(const uint8_t *buf, uint32_t size) {
  ESP_LOG_BUFFER_HEXDUMP(TAG, buf, size > 32 ? 32 : size, ESP_LOG_DEBUG);
}

int process_command(const uint8_t *buf, uint32_t size) {
  switch (buf[0]) {
  case kDrawRect: {
    if (size != kRectPos && size != kRectPosColor && size != kRectPosSize &&
        size != kRectPosSizeColor) {
      ESP_LOGD(TAG, "bad rect packet length %u", (unsigned)size);
      dump(buf, size);
      return 0;
    }
    const int x = u16(buf, 1);
    const int y = u16(buf, 3);
    int w = 1, h = 1;
    if (size == kRectPosColor) {
      last_rect_color = {buf[5], buf[6], buf[7]};
    } else if (size == kRectPosSize) {
      w = u16(buf, 5);
      h = u16(buf, 7);
    } else if (size == kRectPosSizeColor) {
      w = u16(buf, 5);
      h = u16(buf, 7);
      last_rect_color = {buf[9], buf[10], buf[11]};
    }
    m8screen::draw_rect(x, y, w, h, last_rect_color);
    return 1;
  }

  case kDrawChar:
    if (size != kCharLen) {
      ESP_LOGD(TAG, "bad char packet length %u", (unsigned)size);
      dump(buf, size);
      return 0;
    }
    m8screen::draw_char((char)buf[1], u16(buf, 2), u16(buf, 4), {buf[6], buf[7], buf[8]},
                        {buf[9], buf[10], buf[11]});
    return 1;

  case kDrawScope:
    if (size < kScopeMin || size > kScopeMax) {
      ESP_LOGD(TAG, "bad scope packet length %u", (unsigned)size);
      dump(buf, size);
      return 0;
    }
    m8screen::draw_waveform({buf[1], buf[2], buf[3]}, buf + 4, (int)size - 4);
    return 1;

  case kJoypadState:
    return size == kJoypadLen;

  case kSystemInfo: {
    if (size != kSystemInfoLen) {
      ESP_LOGD(TAG, "bad system info packet length %u", (unsigned)size);
      dump(buf, size);
      return 0;
    }
    const bool first = !info.valid;
    info = {true, buf[1], buf[2], buf[3], buf[4], buf[5]};
    if (first) {
      ESP_LOGI(TAG, "device: %s, firmware %u.%u.%u", hw_name(info.hw_type), info.fw_major,
               info.fw_minor, info.fw_patch);
    }
    m8screen::set_hardware_model(info.hw_type);
    m8screen::set_font_mode(info.font_mode);
    return 1;
  }

  default:
    ESP_LOGD(TAG, "unknown packet 0x%02X len %u", buf[0], (unsigned)size);
    dump(buf, size);
    return 0;
  }
}

int on_slip_message(const uint8_t *data, uint32_t size) {
  if (process_command(data, size)) {
    ok_count++;
    return 1;
  }
  bad_count++;
  return 0;
}

} // namespace

void init() {
  slip_init(&slip, slip_buf, sizeof(slip_buf), on_slip_message);
  reset();
}

void reset() {
  slip_init(&slip, slip_buf, sizeof(slip_buf), on_slip_message);
  info = {};
  last_rect_color = {0, 0, 0};
  ok_count = 0;
  bad_count = 0;
}

void feed(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) slip_read_byte(&slip, data[i]);
}

const DeviceInfo &device_info() { return info; }

const char *hw_name(uint8_t hw_type) {
  static const char *names[] = {"Headless", "Beta M8", "M8", "M8 Model:02"};
  return hw_type < 4 ? names[hw_type] : "Unknown";
}

uint32_t packets_ok() { return ok_count; }
uint32_t packets_bad() { return bad_count; }

} // namespace m8proto
