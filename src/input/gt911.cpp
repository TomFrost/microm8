#include "gt911.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_log.h>

#include "config.h"

static const char *TAG = "gt911";

namespace gt911 {
namespace {

constexpr uint8_t kAddrPrimary = 0x5D;   // INT low during reset
constexpr uint8_t kAddrSecondary = 0x14; // INT high during reset

constexpr uint16_t kRegProductId = 0x8140;
constexpr uint16_t kRegXRes = 0x8048; // config: x resolution (LE16), then y resolution
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoints = 0x814F;
constexpr int kPointStride = 8;

uint8_t addr = kAddrPrimary;
uint16_t res_x = LCD_WIDTH;
uint16_t res_y = LCD_HEIGHT;

bool read_reg(uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool write_reg(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool probe(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

// Reset sequence from the GT911 datasheet; INT level at the RST rising edge selects 0x5D.
void hw_reset() {
  pinMode(TOUCH_RST, OUTPUT);
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  digitalWrite(TOUCH_INT, LOW);
  delay(11);
  digitalWrite(TOUCH_RST, HIGH);
  delay(6);
  digitalWrite(TOUCH_INT, LOW);
  delay(55);
  pinMode(TOUCH_INT, INPUT);
  delay(50);
}

} // namespace

bool begin() {
  hw_reset();
  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_HZ);

  if (probe(kAddrPrimary)) {
    addr = kAddrPrimary;
  } else if (probe(kAddrSecondary)) {
    addr = kAddrSecondary;
  } else {
    ESP_LOGE(TAG, "GT911 not found on I2C");
    return false;
  }

  uint8_t id[4] = {0};
  read_reg(kRegProductId, id, 4);
  uint8_t res[4];
  if (read_reg(kRegXRes, res, 4)) {
    const uint16_t rx = res[0] | (res[1] << 8);
    const uint16_t ry = res[2] | (res[3] << 8);
    if (rx > 0 && ry > 0) {
      res_x = rx;
      res_y = ry;
    }
  }
  ESP_LOGI(TAG, "GT%c%c%c%c at 0x%02X, configured %ux%u", id[0], id[1], id[2], id[3] ? id[3] : ' ',
           addr, res_x, res_y);
  return true;
}

bool read(Point *points, int *count) {
  uint8_t status;
  if (!read_reg(kRegStatus, &status, 1)) return false;
  if (!(status & 0x80)) return false; // no new report

  int n = status & 0x0F;
  if (n > kMaxPoints) n = 0; // garbage report

  uint8_t raw[kMaxPoints * kPointStride];
  if (n > 0 && !read_reg(kRegPoints, raw, n * kPointStride)) n = 0;
  write_reg(kRegStatus, 0);

  for (int i = 0; i < n; i++) {
    const uint8_t *p = raw + i * kPointStride;
    int32_t x = p[1] | (p[2] << 8);
    int32_t y = p[3] | (p[4] << 8);
    int32_t span_x = res_x, span_y = res_y;
#if TOUCH_SWAP_XY
    int32_t t = x;
    x = y;
    y = t;
    span_x = res_y;
    span_y = res_x;
#endif
    x = x * LCD_WIDTH / span_x;
    y = y * LCD_HEIGHT / span_y;
#if TOUCH_MIRROR_X
    x = LCD_WIDTH - 1 - x;
#endif
#if TOUCH_MIRROR_Y
    y = LCD_HEIGHT - 1 - y;
#endif
    points[i] = {(int16_t)x, (int16_t)y, p[0]};
  }
  *count = n;
  return true;
}

} // namespace gt911
