#include "diag/screenshot.h"

#include <Arduino.h>
#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>

#include "config.h"
#include "display/ui.h"

static const char *TAG = "screenshot";

namespace screenshot {
namespace {

constexpr int kLineBytes = 57; // encoded bytes per base64 line (76 characters)

std::atomic<bool> wanted{false};
std::atomic<bool> ready{false}; // a capture is waiting to be sent
uint16_t *pixels;
int width, height, rotation;

// Base64 for one line of up to kLineBytes bytes, plus a newline, into `out`.
size_t base64_line(const uint8_t *in, size_t n, char *out) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (in[i] << 16) | ((i + 1 < n ? in[i + 1] : 0) << 8) |
                       (i + 2 < n ? in[i + 2] : 0);
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kAlphabet[v & 63] : '=';
  }
  out[o++] = '\n';
  return o;
}

// Encodes the capture as runs and sends it as base64 lines.
void send() {
  esp_log_level_set("*", ESP_LOG_NONE); // keep log lines out of the image data
  Serial.printf("\nMICROM8-SCREENSHOT 1 %d %d %d\n", width, height, rotation);

  uint8_t block[kLineBytes];
  size_t fill = 0, total = 0;
  uint32_t crc = 0;
  char line[80];
  auto emit = [&](const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) {
      block[fill++] = bytes[i];
      if (fill == kLineBytes) {
        crc = esp_rom_crc32_le(crc, block, fill);
        total += fill;
        Serial.write((const uint8_t *)line, base64_line(block, fill, line));
        fill = 0;
      }
    }
  };

  const int count = width * height;
  for (int i = 0; i < count;) {
    const uint16_t color = pixels[i];
    int run = 1;
    while (i + run < count && run < 0xFFFF && pixels[i + run] == color) run++;
    const uint8_t rec[4] = {(uint8_t)(run & 0xFF), (uint8_t)(run >> 8), (uint8_t)(color & 0xFF),
                            (uint8_t)(color >> 8)};
    emit(rec, sizeof(rec));
    i += run;
  }
  if (fill) {
    crc = esp_rom_crc32_le(crc, block, fill);
    total += fill;
    Serial.write((const uint8_t *)line, base64_line(block, fill, line));
  }
  Serial.printf("MICROM8-SCREENSHOT-END %u %08lx\n", (unsigned)total, (unsigned long)crc);
  Serial.flush();
  esp_log_level_set("*", ESP_LOG_INFO);
  ESP_LOGI(TAG, "sent %dx%d (rotation %d) as %u bytes", width, height, rotation,
           (unsigned)total);
}

void task(void *) {
  for (;;) {
    while (Serial.available() > 0) {
      if (Serial.read() == 'S') wanted = true;
    }
    if (ready) {
      send();
      heap_caps_free(pixels);
      pixels = nullptr;
      ready = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

} // namespace

void begin() { xTaskCreatePinnedToCore(task, "screenshot", 4096, nullptr, 2, nullptr, 1); }

bool requested() { return wanted && !ready; }

void capture() {
  wanted = false;
  const size_t bytes = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t); // the panel
  pixels = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!pixels) {
    ESP_LOGE(TAG, "no memory for a capture");
    return;
  }
  if (!ui::snapshot(pixels, &width, &height, &rotation)) {
    heap_caps_free(pixels);
    pixels = nullptr;
    return;
  }
  ready = true;
}

} // namespace screenshot
