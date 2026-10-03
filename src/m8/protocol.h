// M8 display protocol: SLIP-framed draw commands from the M8, and the single-byte
// command set sent back to it. Ported from m8c's command.c (MIT, Jonne Kokkonen).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace m8proto {

// Host -> M8 commands
constexpr uint8_t kCmdEnableDisplay = 'E';
constexpr uint8_t kCmdResetDisplay = 'R';
constexpr uint8_t kCmdDisconnect = 'D';
constexpr uint8_t kCmdController = 'C'; // followed by the key bitmask
constexpr uint8_t kCmdKeyjazz = 'K';    // followed by note, velocity

// Key bits for kCmdController
enum Key : uint8_t {
  kKeyEdit = 1 << 0,
  kKeyOpt = 1 << 1,
  kKeyRight = 1 << 2,
  kKeyStart = 1 << 3, // PLAY
  kKeySelect = 1 << 4, // SHIFT
  kKeyDown = 1 << 5,
  kKeyUp = 1 << 6,
  kKeyLeft = 1 << 7,
};

struct DeviceInfo {
  bool valid;
  uint8_t hw_type; // 0 Headless, 1 Beta M8, 2 Production M8, 3 Model:02
  uint8_t fw_major, fw_minor, fw_patch;
  uint8_t font_mode;
};

void init();
void reset(); // drop partial frames, device info and stats, e.g. after reconnect

// Feed raw bytes from the M8's CDC endpoint.
void feed(const uint8_t *data, size_t len);

const DeviceInfo &device_info();
const char *hw_name(uint8_t hw_type);

// Statistics, for diagnostics.
uint32_t packets_ok();
uint32_t packets_bad();

} // namespace m8proto
