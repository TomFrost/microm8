// Tab5 bring-up through M5Unified: display, touch, power rails and I/O expanders. microm8
// drives the audio codec itself (see audio/audio_hw.cpp), so M5Unified's speaker and
// microphone drivers stay off.
#include "board.h"

#include <M5Unified.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

static const char *TAG = "tab5";

namespace board {
namespace {
SemaphoreHandle_t i2c_mutex; // created in begin(), before anything takes the lock
} // namespace

I2CLock::I2CLock() { xSemaphoreTakeRecursive(i2c_mutex, portMAX_DELAY); }
I2CLock::~I2CLock() { xSemaphoreGiveRecursive(i2c_mutex); }

bool begin() {
  i2c_mutex = xSemaphoreCreateRecursiveMutex();

  auto cfg = M5.config();
  cfg.serial_baudrate = 0; // Serial (USB-C, USB-Serial-JTAG) is started by main
  cfg.clear_display = true;
  cfg.internal_spk = false; // audio_hw.cpp owns I2S0 and the ES8388
  cfg.internal_mic = false;
  cfg.internal_imu = false;
  {
    I2CLock lock;
    M5.begin(cfg);

    // 5 V on the USB-A host port powers the M8 (Teensy). It is off after reset.
    M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);
  }
  ESP_LOGI(TAG, "display %dx%d, touch %s, USB-A power %s", M5.Display.width(),
           M5.Display.height(), M5.Touch.isEnabled() ? "ok" : "missing",
           M5.Power.getExtOutput() ? "on" : "off");
  return true;
}

const char *name() { return BOARD_NAME; }

int battery_percent() {
  // The gauge is read over the shared I2C bus; refresh it every few seconds at most.
  static int cached = -1;
  static bool read_once = false;
  static uint32_t last_read = 0;
  const uint32_t now = millis();
  if (!read_once || now - last_read >= 5000) {
    read_once = true;
    last_read = now;
    I2CLock lock;
    cached = M5.Power.getBatteryLevel();
  }
  return cached;
}

} // namespace board
