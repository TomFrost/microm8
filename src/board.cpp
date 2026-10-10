// Tab5 bring-up through M5Unified: display, touch, power rails and I/O expanders. microm8
// drives the audio codec itself (see audio/audio_hw.cpp), so M5Unified's speaker and
// microphone drivers stay off.
#include "board.h"

#include <M5Unified.h>
#include <driver/temperature_sensor.h>
#include <esp_system.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "config.h"

static const char *TAG = "tab5";

namespace board {
namespace {

constexpr uint32_t kI2cHz = 400000;
constexpr uint8_t kIoeOutputReg = 0x05;
constexpr uint32_t kBatteryRefreshMs = 500;
constexpr uint32_t kM8PowerOffMs = 500; // long enough for the Teensy to drop out

SemaphoreHandle_t i2c_mutex; // created in begin(), before anything takes the lock
temperature_sensor_handle_t tsens;

struct {
  bool valid;
  uint32_t read_at;
  int percent;
  bool charging;
  bool external; // external power connected (charging or not)
  bool present;
  uint32_t low_at; // when the voltage last read below kBatteryPresentMv
  int load_mw;     // power drawn from the battery, or -1 while charging
} battery;

// Battery presence. The Tab5 has no presence signal, and with no pack fitted the charger
// pulses the battery terminal while probing for one, so single readings can't be trusted. A
// 2S Li-ion pack never sits below about 6 V: any lower reading means no pack, and the pack only
// counts as present again after the voltage has stayed up for kBatteryReturnMs.
constexpr int kBatteryPresentMv = 5500;
constexpr uint32_t kBatteryReturnMs = 15000;

// Charging and external power are judged from the battery current (the charger's CHG_STAT
// line floats once the charger is unplugged), with hysteresis so readings near a threshold
// don't flicker:
// - charging: clearly positive current into the battery;
// - plugged in but not charging (full): near zero, since the charger then carries the load;
// - on battery: clearly negative, as the Tab5 always draws at least about 1 W.
constexpr int kChargeStartMa = 50;
constexpr int kChargeStopMa = 20;
constexpr int kExternalStartMa = -40; // at or above: external power is carrying the load
constexpr int kExternalStopMa = -80;  // below: running from the battery

char boot_savings[96] = "not measured"; // power-cut results, repeated in every log_power()

void refresh_battery() {
  const uint32_t now = millis();
  if (battery.valid && now - battery.read_at < kBatteryRefreshMs) return;
  int mv, ma;
  {
    I2CLock lock;
    battery.percent = M5.Power.getBatteryLevel();
    mv = M5.Power.getBatteryVoltage();
    ma = M5.Power.getBatteryCurrent(); // + charging, - discharging
  }
  const bool first = !battery.valid;
  battery.valid = true;
  battery.read_at = now;

  if (mv < kBatteryPresentMv) {
    battery.present = false;
    battery.low_at = now;
  } else if (first) {
    battery.present = true;
  } else if (!battery.present && now - battery.low_at >= kBatteryReturnMs) {
    battery.present = true;
  }

  if (!battery.present || ma < kChargeStopMa) {
    battery.charging = false;
  } else if (ma > kChargeStartMa) {
    battery.charging = true;
  }
  if (!battery.present) {
    battery.external = true; // nothing else could be powering the Tab5
  } else if (ma >= kExternalStartMa) {
    battery.external = true;
  } else if (ma < kExternalStopMa) {
    battery.external = false;
  }
  battery.load_mw = ma < 0 ? (int)((int64_t)mv * -ma / 1000) : -1;
}

#if MICROM8_POWER_DIAG
// Average battery current over `ms` (mA; negative while discharging). On external power the
// battery current doesn't reflect the load, so the figures only mean something on battery.
int average_battery_ma(int ms) {
  vTaskDelay(pdMS_TO_TICKS(300)); // let the last change settle
  int64_t sum = 0;
  int n = 0;
  for (int t = 0; t < ms; t += 50, n++) {
    {
      I2CLock lock;
      sum += M5.Power.getBatteryCurrent();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return n ? (int)(sum / n) : 0;
}
#endif

// Turns off a power consumer microm8 doesn't use: clears `mask` in an I/O expander's output
// register. With MICROM8_POWER_DIAG, logs the change in battery draw.
void switch_off(const char *what, uint8_t ioe_addr, uint8_t mask) {
#if MICROM8_POWER_DIAG
  const int before = average_battery_ma(600);
#endif
  {
    I2CLock lock;
    M5.In_I2C.bitOff(ioe_addr, kIoeOutputReg, mask, kI2cHz);
  }
#if MICROM8_POWER_DIAG
  const int after = average_battery_ma(600);
  ESP_LOGI(TAG, "power: %s off: %d mA -> %d mA (saves %d mA)", what, -before, -after,
           after - before);
  const size_t len = strcmp(boot_savings, "not measured") == 0 ? 0 : strlen(boot_savings);
  snprintf(boot_savings + len, sizeof(boot_savings) - len, "%s%s -%d mA", len ? ", " : "", what,
           after - before);
#else
  ESP_LOGI(TAG, "power: %s off", what);
#endif
}

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
  cfg.internal_imu = true; // BMI270, for screen orientation
  {
    I2CLock lock;
    M5.begin(cfg);
  }

  // M5Unified powers up hardware microm8 never uses. Cut it before the Teensy is powered, so
  // the diagnostic measurements aren't disturbed by the M8 booting.
  switch_off("C6 wireless", TAB5_IOE1_ADDR, TAB5_IOE1_WLAN_PWR_EN);
  switch_off("camera", TAB5_IOE0_ADDR, TAB5_IOE0_CAM_RST);

  // A reset (after flashing, the reset button, a crash) restarts only the P4: the I/O expanders
  // keep USB-A powered, so the M8 would run on with its USB session to the old firmware.
  // Power-cycle it, so it starts fresh as on a cold boot. (The codec also runs on through a
  // reset; audio_hw::begin() restarts it.)
  const esp_reset_reason_t reset = esp_reset_reason();
  if (reset != ESP_RST_POWERON) {
    ESP_LOGI(TAG, "reset (reason %d), not power-on: power-cycling the M8", (int)reset);
    {
      I2CLock lock;
      M5.Power.setExtOutput(false, m5::ext_port_mask_t::ext_USB);
    }
    vTaskDelay(pdMS_TO_TICKS(kM8PowerOffMs));
  }
  {
    // 5 V on the USB-A host port powers the M8 (Teensy).
    I2CLock lock;
    M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);
  }

  temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
  if (temperature_sensor_install(&tcfg, &tsens) != ESP_OK ||
      temperature_sensor_enable(tsens) != ESP_OK) {
    tsens = nullptr;
  }

  ESP_LOGI(TAG, "display %dx%d, touch %s, IMU %s, USB-A power %s", M5.Display.width(),
           M5.Display.height(), M5.Touch.isEnabled() ? "ok" : "missing",
           M5.Imu.isEnabled() ? "ok" : "missing", M5.Power.getExtOutput() ? "on" : "off");
  return true;
}

const char *name() { return BOARD_NAME; }

int battery_percent() {
  refresh_battery();
  return battery.percent;
}

bool charging() {
  refresh_battery();
  return battery.charging;
}

bool external_power() {
  refresh_battery();
  return battery.external;
}

bool battery_present() {
  refresh_battery();
  return battery.present;
}

int power_draw_mw() {
  refresh_battery();
  return battery.load_mw;
}

bool accel(float *x, float *y, float *z) {
  if (!M5.Imu.isEnabled()) return false;
  I2CLock lock;
  M5.Imu.update();
  return M5.Imu.getAccel(x, y, z);
}

void log_power() {
  int mv, ma;
  {
    I2CLock lock;
    mv = M5.Power.getBatteryVoltage();
    ma = M5.Power.getBatteryCurrent();
  }
  float chip_c = NAN;
  if (tsens) temperature_sensor_get_celsius(tsens, &chip_c);
  // Battery current: + while charging, - while discharging.
  const float watts = mv * (float)(ma < 0 ? -ma : ma) / 1e6f;
  ESP_LOGI(TAG, "power: %.2f V, %+d mA (%.2f W %s), %d%%, %s, chip %.0f C; boot cuts: %s",
           mv / 1000.0f, ma, watts, ma < 0 ? "drawn" : "charging", battery_percent(),
           !battery_present() ? "no battery"
           : charging()       ? "charging"
           : external_power() ? "plugged in, full"
                              : "on battery",
           chip_c, boot_savings);
}

} // namespace board
