// microm8: a standalone frontend for the Dirtywave M8 Headless on the M5Stack Tab5 (ESP32-P4).
//
// Tasks:
//   usb_lib / usb_m8 (core 0)  USB host: M8 CDC serial + isochronous audio  -> usb/m8_usb.cpp
//   audio            (core 0)  ring buffer -> board audio hardware           -> audio/audio_out.cpp
//   ui               (core 1)  SLIP decode, M8 framebuffer, LCD updates      -> this file
//   touch            (core 1)  multi-touch -> M8 key bitmask                 -> this file

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <esp_log.h>
#include <freertos/stream_buffer.h>

#include "audio/audio_hw.h"
#include "audio/audio_out.h"
#include "board.h"
#include "config.h"
#include "diag/log_capture.h"
#include "display/ui.h"
#include "input/keypad.h"
#include "input/touch.h"
#include "m8/protocol.h"
#include "m8/screen.h"
#include "usb/m8_usb.h"

static const char *TAG = "microm8";

namespace {

constexpr size_t kRxStreamBytes = 32 * 1024;
constexpr uint32_t kFrameIntervalMs = 16; // cap LCD updates at ~60 Hz
constexpr uint32_t kSettingsSaveDelayMs = 2000;
constexpr uint32_t kJackDebounceMs = 100;
constexpr uint32_t kPowerLogIntervalMs = 10000;
constexpr uint32_t kPopupTimeoutMs = 4000; // the row under the status bar closes after this

StreamBufferHandle_t rx_stream;
Preferences prefs;

// Written by the touch task, read by the UI task.
std::atomic<uint8_t> held_keys{0};
std::atomic<bool> settings_changed{false};
std::atomic<ui::Popup> popup{ui::Popup::kNone};
std::atomic<uint32_t> popup_touched_at{0};
std::atomic<int> brightness{ui::kBrightnessMax};

void send_cmd(uint8_t cmd) { m8usb::send(&cmd, 1); }

void save_settings() {
  prefs.putUChar("vol_spk", (uint8_t)audio::volume_for(audio::Output::kSpeaker));
  prefs.putUChar("vol_hp", (uint8_t)audio::volume_for(audio::Output::kHeadphones));
  prefs.putUChar("output", (uint8_t)audio::output());
  prefs.putUChar("bright", (uint8_t)brightness.load());
}

// Switches to headphones when a plug goes in and back to the speaker when it comes out.
// Only edges count, so the on-screen toggle still works with a plug inserted.
void poll_jack_detect(uint32_t now) {
  static int stable = -1;
  static int pending = -1;
  static uint32_t pending_since = 0;

  const int inserted = audio_hw::headphones_inserted();
  if (inserted < 0) return; // jack detection not configured (see TAB5_HP_DET_INSERTED_LEVEL)
  if (inserted != pending) {
    pending = inserted;
    pending_since = now;
    return;
  }
  if (inserted == stable || now - pending_since < kJackDebounceMs) return;

  const bool first = stable < 0;
  stable = inserted;
  if (first && !inserted) return; // keep the saved choice at boot if nothing is plugged in
  audio::set_output(inserted ? audio::Output::kHeadphones : audio::Output::kSpeaker);
  settings_changed = true;
}

ui::Status::Link link_for(m8usb::State s) {
  switch (s) {
  case m8usb::State::kConnected:
    return ui::Status::Link::kConnected;
  case m8usb::State::kUnsupported:
    return ui::Status::Link::kUnsupported;
  case m8usb::State::kError:
    return ui::Status::Link::kError;
  default:
    return ui::Status::Link::kWaiting;
  }
}

void show_disconnected(m8usb::State s) {
  switch (s) {
  case m8usb::State::kUnsupported:
    ui::show_splash("USB device is not an M8", "Connect a Teensy running M8 Headless");
    break;
  case m8usb::State::kError:
    ui::show_splash("Could not set up the M8", "Check the serial log; try replugging");
    break;
  default:
    ui::show_splash("Connect M8 Headless", "Plug the Teensy into the USB-A port");
    break;
  }
}

void ui_task(void *) {
  static uint8_t buf[2048];

  uint32_t seen_connections = 0;
  m8usb::State last_state = m8usb::State::kWaiting;
  uint32_t last_frame = 0;
  uint32_t second_reset_at = 0;
  uint32_t settings_dirty_since = 0;
  uint32_t last_power_log = 0;
  int applied_brightness = -1;
  uint32_t shown_log_version = logcap::version() - 1; // draw the log on the first frame

  show_disconnected(last_state);
  ui::flush_m8();

  for (;;) {
    // Drain M8 serial data, but yield to the LCD at least once per frame.
    const uint32_t drain_start = millis();
    size_t n = xStreamBufferReceive(rx_stream, buf, sizeof(buf), pdMS_TO_TICKS(2));
    while (n > 0) {
      m8proto::feed(buf, n);
      if (millis() - drain_start >= kFrameIntervalMs) break;
      n = xStreamBufferReceive(rx_stream, buf, sizeof(buf), 0);
    }

    const uint32_t now = millis();

    // Connection handling, mirroring m8c's enable sequence: enable, reset, and a second
    // reset shortly after to avoid display glitches.
    const uint32_t connections = m8usb::connection_count();
    if (connections != seen_connections) {
      seen_connections = connections;
      m8proto::reset();
      m8screen::clear({0, 0, 0});
      send_cmd(m8proto::kCmdEnableDisplay);
      vTaskDelay(pdMS_TO_TICKS(5));
      send_cmd(m8proto::kCmdResetDisplay);
      second_reset_at = now + 100;
    }
    if (second_reset_at && (int32_t)(now - second_reset_at) >= 0) {
      second_reset_at = 0;
      send_cmd(m8proto::kCmdResetDisplay);
    }

    const m8usb::State state = m8usb::state();
    if (state != last_state) {
      if (state != m8usb::State::kConnected) {
        show_disconnected(state);
        shown_log_version = logcap::version() - 1; // force a log redraw
      }
      last_state = state;
    }

    poll_jack_detect(now);
    if (brightness != applied_brightness) {
      applied_brightness = brightness;
      ui::set_backlight(ui::backlight_for(applied_brightness));
    }
    if (popup != ui::Popup::kNone && now - popup_touched_at > kPopupTimeoutMs) {
      popup = ui::Popup::kNone;
    }
#if MICROM8_POWER_DIAG
    if (now - last_power_log >= kPowerLogIntervalMs) {
      last_power_log = now;
      board::log_power();
    }
#endif
    if (settings_changed.exchange(false)) settings_dirty_since = now ? now : 1;
    if (settings_dirty_since && now - settings_dirty_since >= kSettingsSaveDelayMs) {
      settings_dirty_since = 0;
      save_settings();
    }

    if (now - last_frame >= kFrameIntervalMs) {
      last_frame = now;

      // While waiting for the M8, show recent log lines in its screen area.
      if (state != m8usb::State::kConnected && logcap::version() != shown_log_version) {
        shown_log_version = logcap::version();
        static logcap::Line lines[logcap::kLines];
        ui::show_log(lines, logcap::copy_lines(lines, logcap::kLines));
      }

      ui::flush_m8();
      ui::draw_keys(held_keys.load());

      const m8proto::DeviceInfo &info = m8proto::device_info();
      ui::Status st{};
      st.link = link_for(state);
      st.device = info.valid ? m8proto::hw_name(info.hw_type) : nullptr;
      st.fw_major = info.fw_major;
      st.fw_minor = info.fw_minor;
      st.fw_patch = info.fw_patch;
      st.audio = m8usb::audio_streaming();
      st.oversized = m8screen::oversized();
      st.volume = audio::volume();
      st.headphones = audio::output() == audio::Output::kHeadphones;
      st.popup = popup;
      st.power = !board::battery_present() ? ui::PowerSource::kNoBattery
                 : board::charging()        ? ui::PowerSource::kCharging
                 : board::external_power()  ? ui::PowerSource::kPluggedIn
                                            : ui::PowerSource::kBattery;
      st.battery = board::battery_percent();
      st.power_mw = MICROM8_POWER_DIAG ? board::power_draw_mw() : -1;
      ui::draw_status(st);

      ui::Controls controls{};
      controls.popup = popup;
      controls.volume = st.volume;
      controls.headphones = st.headphones;
      controls.brightness = brightness;
      controls.power = st.power;
      controls.battery = st.battery;
      ui::draw_controls(controls);
    }
  }
}

void touch_task(void *) {
  touch::Point points[touch::kMaxPoints];
  int count = 0;
  uint8_t mask = 0;
  uint8_t sent_mask = 0;
  uint32_t seen_connections = 0;
  ui::Hit prev_hit = ui::Hit::kNone;

  for (;;) {
    if (touch::read(points, &count)) {
      mask = keypad::mask_for(points, count);
      held_keys = mask;

      // Status bar and slider row: the first touch landing on a target wins.
      const ui::Popup open = popup;
      ui::Hit hit = ui::Hit::kNone;
      int value = 0;
      for (int i = 0; i < count && hit == ui::Hit::kNone; i++) {
        hit = ui::hit_test(points[i].x, points[i].y, open, &value);
      }
      if (hit != ui::Hit::kNone) popup_touched_at = millis(); // keeps the row open

      // Buttons act once per press; sliders follow the finger.
      if (hit != prev_hit) {
        if (hit == ui::Hit::kVolumeIcon) {
          popup = open == ui::Popup::kVolume ? ui::Popup::kNone : ui::Popup::kVolume;
        } else if (hit == ui::Hit::kBrightnessIcon) {
          popup = open == ui::Popup::kBrightness ? ui::Popup::kNone : ui::Popup::kBrightness;
        } else if (hit == ui::Hit::kPowerIcon) {
          popup = open == ui::Popup::kPower ? ui::Popup::kNone : ui::Popup::kPower;
        } else if (hit == ui::Hit::kOutputToggle) {
          audio::set_output(audio::output() == audio::Output::kSpeaker ? audio::Output::kHeadphones
                                                                       : audio::Output::kSpeaker);
          settings_changed = true;
        }
      }
      if (hit == ui::Hit::kSlider) {
        if (open == ui::Popup::kVolume && value != audio::volume()) {
          audio::set_volume(value);
          settings_changed = true;
        } else if (open == ui::Popup::kBrightness && value != brightness) {
          brightness = value;
          settings_changed = true;
        }
      }
      prev_hit = hit;
    }

    // The M8 starts with no keys held after a (re)connect.
    const uint32_t connections = m8usb::connection_count();
    if (connections != seen_connections) {
      seen_connections = connections;
      sent_mask = 0;
    }

    if (mask != sent_mask && m8usb::state() == m8usb::State::kConnected) {
      const uint8_t msg[2] = {m8proto::kCmdController, mask};
      if (m8usb::send(msg, sizeof(msg))) sent_mask = mask; // retried next poll if the queue is full
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void audio_sink(const uint8_t *pcm, size_t bytes) { audio::push(pcm, bytes); }

} // namespace

void setup() {
  logcap::begin();
  Serial.begin(115200);
  ESP_LOGI(TAG, "microm8 starting");
  if (!board::begin()) ESP_LOGE(TAG, "board init failed");
  ESP_LOGI(TAG, "board: %s", board::name());

  prefs.begin("microm8", false);
  const int speaker_volume = prefs.getUChar("vol_spk", 7);
  const int headphone_volume = prefs.getUChar("vol_hp", 4); // line level is loud; start low
  const auto output = (audio::Output)prefs.getUChar("output", (uint8_t)audio::Output::kSpeaker);
  const int saved_brightness = prefs.getUChar("bright", ui::kBrightnessMax);
  brightness = saved_brightness < ui::kBrightnessMin || saved_brightness > ui::kBrightnessMax
                   ? ui::kBrightnessMax
                   : saved_brightness;

  if (!m8screen::init()) ESP_LOGE(TAG, "framebuffer init failed");
  m8proto::init();

  if (!ui::begin()) ESP_LOGE(TAG, "display init failed");
  const bool touch_ok = touch::begin();
  if (!touch_ok) ESP_LOGE(TAG, "touch init failed; on-screen keys disabled");
  if (!audio::start(speaker_volume, headphone_volume, output)) {
    ESP_LOGE(TAG, "audio init failed");
  }

  rx_stream = xStreamBufferCreate(kRxStreamBytes, 1);
  if (!m8usb::start(rx_stream, audio_sink)) ESP_LOGE(TAG, "USB host init failed");

  xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 10, nullptr, UI_CORE);
  if (touch_ok) xTaskCreatePinnedToCore(touch_task, "touch", 4096, nullptr, 12, nullptr, UI_CORE);
}

void loop() { vTaskDelete(nullptr); }
