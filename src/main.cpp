// microm8: a standalone frontend for the Dirtywave M8 Headless on the M5Stack Tab5 (ESP32-P4).
//
// Tasks:
//   usb_lib / usb_m8 (core 0)  USB host: M8 CDC serial + isochronous audio  -> usb/m8_usb.cpp
//   audio            (core 0)  ring buffer -> board audio hardware           -> audio/audio_out.cpp
//   ui               (core 1)  SLIP decode, M8 framebuffer, LCD updates      -> this file
//   touch            (core 1)  multi-touch -> M8 key bitmask                 -> this file
//   passthru         (core 0)  USB-C: the M8, passed through to a computer   -> usb/passthru.cpp

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <esp_log.h>
#include <freertos/stream_buffer.h>
#include <math.h>

#include "audio/audio_hw.h"
#include "audio/audio_out.h"
#include "audio/mic.h"
#include "board.h"
#include "config.h"
#include "diag/log_capture.h"
#include "diag/screenshot.h"
#include "display/ui.h"
#include "input/keypad.h"
#include "input/touch.h"
#include "m8/protocol.h"
#include "m8/screen.h"
#include "usb/m8_usb.h"
#include "usb/passthru.h"

static const char *TAG = "microm8";

namespace {

constexpr size_t kRxStreamBytes = 32 * 1024;
constexpr uint32_t kFrameIntervalMs = 16; // cap LCD updates at ~60 Hz
constexpr uint32_t kSettingsSaveDelayMs = 2000;
constexpr uint32_t kJackDebounceMs = 100;
constexpr uint32_t kPowerLogIntervalMs = 10000;
constexpr uint32_t kAudioLogIntervalMs = 5000; // audio path report, for a while after connecting
constexpr uint32_t kAudioLogForMs = 30000;
constexpr uint32_t kPopupTimeoutMs = 4000; // the row under the status bar closes after this
constexpr uint32_t kMessageMs = 2000;      // transient status messages
constexpr uint32_t kOrientationPollMs = 100;
constexpr uint32_t kOrientationDwellMs = 700; // a new orientation must hold this long
// Gravity must lie mostly in the screen's plane, and clearly along one of its axes, to count:
// flat on a table or held diagonally, the orientation stays as it is.
constexpr float kMinInPlaneG = 0.55f;
constexpr float kMinAxisMarginG = 0.25f;

StreamBufferHandle_t rx_stream;
Preferences prefs;

// Written by the touch task, read by the UI task.
std::atomic<uint8_t> held_keys{0};
std::atomic<bool> settings_changed{false};
std::atomic<ui::Popup> popup{ui::Popup::kNone};
std::atomic<uint32_t> popup_touched_at{0};
std::atomic<int> brightness{ui::kBrightnessMax};
std::atomic<bool> rotation_locked{false};
std::atomic<bool> compact_keys{false};
std::atomic<bool> hold_mode{false}; // HOLD: SHIFT and OPTION stay down (see apply_hold)
std::atomic<uint32_t> message_at{0}; // when the rotation lock was last toggled

constexpr uint8_t kArrowKeys = m8proto::kKeyUp | m8proto::kKeyDown | m8proto::kKeyLeft |
                               m8proto::kKeyRight;

void send_cmd(uint8_t cmd) { m8usb::send(&cmd, 1); }

void save_settings() {
  prefs.putUChar("vol_spk", (uint8_t)audio::volume_for(audio::Output::kSpeaker));
  prefs.putUChar("vol_hp", (uint8_t)audio::volume_for(audio::Output::kHeadphones));
  prefs.putUChar("output", (uint8_t)audio::output());
  prefs.putUChar("bright", (uint8_t)brightness.load());
  prefs.putUChar("rot_lock", rotation_locked ? 1 : 0);
  prefs.putUChar("compact", compact_keys ? 1 : 0);
  prefs.putUChar("hold", hold_mode ? 1 : 0);
  prefs.putUChar("mic_boost", (uint8_t)mic::boost());
  prefs.putUChar("orient", (uint8_t)ui::orientation());
}

// The orientation gravity points to, or -1 if the Tab5 is flat or held diagonally.
int sensed_orientation(float ax, float ay, float az) {
  const float gx = fabsf(ax), gy = fabsf(ay);
  if ((gx > gy ? gx : gy) < kMinInPlaneG || fabsf(gx - gy) < kMinAxisMarginG) return -1;
  if (gx > gy) return ax > 0 ? TAB5_GRAVITY_POS_X : TAB5_GRAVITY_NEG_X;
  return ay > 0 ? TAB5_GRAVITY_POS_Y : TAB5_GRAVITY_NEG_Y;
}

// Follows the accelerometer while rotation is unlocked. Returns true if the orientation changed.
bool poll_orientation(uint32_t now) {
  static uint32_t polled_at = 0;
  static int candidate = -1;
  static uint32_t candidate_since = 0;

  if (rotation_locked || now - polled_at < kOrientationPollMs) return false;
  polled_at = now;
  float ax, ay, az;
  if (!board::accel(&ax, &ay, &az)) return false;

  const int o = sensed_orientation(ax, ay, az);
  if (o < 0 || o == (int)ui::orientation()) {
    candidate = -1;
    return false;
  }
  if (o != candidate) {
    candidate = o;
    candidate_since = now;
    return false;
  }
  if (now - candidate_since < kOrientationDwellMs) return false;

  ESP_LOGI(TAG, "orientation: accel %+.2f %+.2f %+.2f g -> %d", ax, ay, az, o);
  candidate = -1;
  ui::set_orientation((ui::Orientation)o);
  return true;
}

// Switches to headphones when a plug goes in and back to the speaker when it comes out, and
// at boot picks whichever matches the jack. After that only edges count, so the on-screen
// selector still works with a plug inserted.
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

  stable = inserted;
  const auto output = inserted ? audio::Output::kHeadphones : audio::Output::kSpeaker;
  if (output == audio::output()) return;
  audio::set_output(output);
  settings_changed = true;
}

// Reports how far audio is getting (USB packets in, the loudest sample received, the playback
// buffer) and how M8 drawing is keeping up. Logged every few seconds for a while after the M8
// connects or the screen rotates.
void log_audio(uint32_t now, uint32_t log_from) {
  static uint32_t logged_at = 0;
  if (!log_from || now - log_from > kAudioLogForMs) return;
  if (now - logged_at < kAudioLogIntervalMs) return;
  logged_at = now;
  const m8usb::AudioStats a = m8usb::audio_stats();
  ESP_LOGI(TAG,
           "audio: USB %lu packets ok, %lu empty, %lu failed (last status %d), %lu bytes; "
           "loudest sample %d; buffered %lu ms; %lu underruns",
           (unsigned long)a.packets_ok, (unsigned long)a.packets_empty,
           (unsigned long)a.packets_failed, a.last_packet_status, (unsigned long)a.bytes,
           audio::take_peak(), (unsigned long)audio::buffered_ms(),
           (unsigned long)audio::underruns());
  if (mic::enabled()) {
    const m8usb::InputStats in = m8usb::input_stats_snapshot();
    ESP_LOGI(TAG,
             "mic: loudest sample %d, buffered %lu ms, %lu underruns; to the M8: %lu packets ok, "
             "%lu failed; M8 rate %.4f samples/microframe (%lu reports used; last raw 0x%08lX, "
             "%lu bytes; %lu empty, %lu failed)",
             mic::take_peak(), (unsigned long)mic::buffered_ms(), (unsigned long)mic::underruns(),
             (unsigned long)in.packets_ok, (unsigned long)in.packets_failed,
             in.feedback / 65536.0, (unsigned long)in.feedback_count,
             (unsigned long)in.feedback_raw, (unsigned long)in.feedback_bytes,
             (unsigned long)in.feedback_empty, (unsigned long)in.feedback_failed);
  }
  ui::log_display_stats();
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
  uint32_t log_from = 0; // when the audio/display report window started
  bool mic_was_on = false;
  int applied_brightness = -1;
  bool applied_compact = compact_keys;
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
      log_from = now ? now : 1;
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
    if (mic::enabled() != mic_was_on) { // report on the mic's audio after switching it
      mic_was_on = mic::enabled();
      log_from = now ? now : 1;
    }
    log_audio(now, log_from);
    if (compact_keys != applied_compact) {
      applied_compact = compact_keys;
      ui::set_compact_keys(applied_compact);
    }
    if (poll_orientation(now)) {
      settings_changed = true;
      log_from = now ? now : 1;
    }
    if (brightness != applied_brightness) {
      applied_brightness = brightness;
      ui::set_backlight(ui::backlight_for(applied_brightness));
    }
    // Signed: the touch task may have stamped a time a little after `now`.
    if (popup != ui::Popup::kNone && (int32_t)(now - popup_touched_at) > (int32_t)kPopupTimeoutMs) {
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

      const uint8_t held = held_keys;
      ui::draw_keys(held);

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
      st.rotation_locked = rotation_locked;
      st.compact_keys = applied_compact;
      st.hold = hold_mode;
      st.mic_on = mic::enabled();
      const uint32_t message_since = message_at;
      if (message_since && (int32_t)(now - message_since) < (int32_t)kMessageMs) {
        st.message = st.rotation_locked ? "Rotation locked" : "Rotation enabled";
      }

      ui::Controls controls{};
      controls.popup = popup;
      controls.volume = st.volume;
      controls.headphones = st.headphones;
      controls.brightness = brightness;
      controls.power = st.power;
      controls.battery = st.battery;
      controls.message = st.message;
      controls.arrows = held & kArrowKeys;
      controls.mic_on = st.mic_on;
      controls.mic_boost = mic::boost();
      ui::draw_controls(controls);

      ui::flush_m8();
      ui::draw_status(st);
      if (screenshot::requested()) screenshot::capture(); // a complete frame
    }
  }
}

// Touch handling. Each finger gets a role when it lands, and keeps it until it lifts:
// - on a status icon or row button: acts once, then is ignored;
// - on a slider: drags it, following the finger's position along the slider's axis anywhere
//   on the screen (so the thumb can move off the slider to uncover the knob);
// - on a pad (arrows, or the OPTION/EDIT/PLAY triangle): holds the key under it, like a key;
// - in the swipe area (landscape's left half) but not on a key: an arrow swipe, holding that
//   arrow from when the swipe is recognised until the finger lifts;
// - anywhere else: presses whichever key it's over, so fingers can slide between keys.
enum class Role : uint8_t { kIgnore, kKeys, kSlider, kSwipe, kPad };

struct Finger {
  bool down;
  uint8_t id;
  Role role;
  int16_t x0, y0;   // where it landed
  ui::Popup slider; // kSlider: the row whose slider it grabbed
  uint8_t arrow;    // kSwipe: the arrow held, once recognised
  uint32_t arrow_at;
};

// The arrow for a swipe by (dx, dy), or 0 while it's too short or too diagonal to tell.
uint8_t swipe_arrow(int dx, int dy) {
  const int ax = abs(dx), ay = abs(dy);
  const int major = ax > ay ? ax : ay, minor = ax > ay ? ay : ax;
  if (major < SWIPE_DISTANCE || major * 2 < minor * 3) return 0;
  if (ax > ay) return dx > 0 ? m8proto::kKeyRight : m8proto::kKeyLeft;
  return dy > 0 ? m8proto::kKeyDown : m8proto::kKeyUp;
}

void set_slider(ui::Popup row, int value) {
  if (row == ui::Popup::kVolume && value != audio::volume()) {
    audio::set_volume(value);
    settings_changed = true;
  } else if (row == ui::Popup::kBrightness && value != brightness) {
    brightness = value;
    settings_changed = true;
  }
}

// The mic feeds the M8's USB audio input while on. Not saved: it starts off, so the speaker
// can't unexpectedly feed back into it.
void set_mic(bool on) {
  mic::set_enabled(on);
  m8usb::set_input_enabled(on);
}

// A finger has landed: act on any target under it and give it its role.
void finger_down(Finger &f, const touch::Point &p, uint32_t now) {
  const ui::Popup open = popup;
  f = Finger{true, p.id, Role::kIgnore, p.x, p.y, open, 0, 0};
  int value = 0;
  const ui::Hit hit = ui::hit_test(p.x, p.y, open, &value);
  if (hit != ui::Hit::kNone) popup_touched_at = now; // keeps the row open

  switch (hit) {
  case ui::Hit::kVolumeIcon:
    popup = open == ui::Popup::kVolume ? ui::Popup::kNone : ui::Popup::kVolume;
    break;
  case ui::Hit::kBrightnessIcon:
    popup = open == ui::Popup::kBrightness ? ui::Popup::kNone : ui::Popup::kBrightness;
    break;
  case ui::Hit::kPowerIcon:
    popup = open == ui::Popup::kPower ? ui::Popup::kNone : ui::Popup::kPower;
    break;
  case ui::Hit::kHoldIcon:
    hold_mode = !hold_mode;
    settings_changed = true;
    break;
  case ui::Hit::kKeysIcon:
    compact_keys = !compact_keys;
    settings_changed = true;
    break;
  case ui::Hit::kMicIcon:
    popup = open == ui::Popup::kMic ? ui::Popup::kNone : ui::Popup::kMic;
    break;
  case ui::Hit::kMicToggle:
    set_mic(value != 0);
    break;
  case ui::Hit::kMicBoost:
    if (value != mic::boost()) {
      mic::set_boost(value);
      settings_changed = true;
    }
    break;
  case ui::Hit::kRotationIcon:
    rotation_locked = !rotation_locked;
    message_at = now ? now : 1;
    settings_changed = true;
    break;
  case ui::Hit::kOutputToggle: {
    const auto chosen = value ? audio::Output::kHeadphones : audio::Output::kSpeaker;
    if (chosen != audio::output()) {
      audio::set_output(chosen);
      settings_changed = true;
    }
    break;
  }
  case ui::Hit::kSlider:
    f.role = Role::kSlider;
    set_slider(open, value);
    break;
  case ui::Hit::kNone:
    if (keypad::mask_for(&p, 1)) {
      f.role = Role::kKeys;
    } else if (ui::pad_key_at(p.x, p.y)) {
      f.role = Role::kPad;
    } else {
      f.role = ui::in_swipe_area(p.x, p.y) ? Role::kSwipe : Role::kKeys;
    }
    break;
  }
}

// Turns held arrows into taps at microm8's repeat rate (see ARROW_REPEAT_MS in config.h).
// `mask` is the keys held on screen; returns what the M8 should see.
// HOLD, for playing chords one key at a time (with one hand, or one finger). With HOLD on,
// tapping SHIFT or OPTION holds it down until it's tapped again, whatever else is pressed in
// between: SHIFT, then RIGHT, RIGHT, RIGHT, then SHIFT again. HOLD itself stays on until its
// icon is tapped. `keys`: the keys under fingers. Returns what the M8 should be sent, which is
// also what to light on screen.
constexpr uint8_t kLatchable = m8proto::kKeySelect | m8proto::kKeyOpt;

uint8_t apply_hold(uint8_t keys, bool on) {
  static uint8_t prev, latched;
  if (!on) {
    prev = keys;
    latched = 0;
    return keys;
  }
  latched ^= keys & ~prev & kLatchable; // a tap toggles
  prev = keys;
  return (keys & ~kLatchable) | latched;
}

uint8_t repeat_arrows(uint8_t mask, uint32_t now) {
#if ARROW_REPEAT_MS == 0
  (void)now;
  return mask;
#else
  static uint8_t held = 0;
  static uint32_t held_since[8];
  uint8_t out = mask & ~kArrowKeys;
  for (int b = 0; b < 8; b++) {
    const uint8_t bit = (uint8_t)(1 << b);
    if (!(kArrowKeys & bit)) continue;
    if (!(mask & bit)) {
      held &= ~bit;
      continue;
    }
    if (!(held & bit)) {
      held |= bit;
      held_since[b] = now;
    }
    if (mask & m8proto::kKeySelect) { // SHIFT + arrow: screen navigation, held as is
      out |= bit;
      continue;
    }
    const uint32_t t = now - held_since[b];
    const bool tap = t < ARROW_REPEAT_DELAY_MS
                         ? t < ARROW_TAP_MS
                         : (t - ARROW_REPEAT_DELAY_MS) % ARROW_REPEAT_MS < ARROW_TAP_MS;
    if (tap) out |= bit;
  }
  return out;
#endif
}

void touch_task(void *) {
  touch::Point points[touch::kMaxPoints];
  int count = 0;
  Finger fingers[touch::kMaxPoints] = {};
  uint8_t mask = 0;
  uint8_t sent_mask = 0;
  uint8_t flicked = 0; // arrows from swipes that already lifted, held for SWIPE_MIN_HOLD_MS
  uint32_t flicked_until = 0;
  uint32_t seen_connections = 0;

  for (;;) {
    if (touch::read(points, &count)) {
      const uint32_t now = millis();
      bool seen[touch::kMaxPoints] = {};
      touch::Point key_points[touch::kMaxPoints];
      int key_count = 0;
      uint8_t pad_keys = 0; // held on pads and by swipes

      for (int i = 0; i < count; i++) {
        const touch::Point &p = points[i];
        int slot = -1;
        for (int j = 0; j < touch::kMaxPoints && slot < 0; j++) {
          if (fingers[j].down && fingers[j].id == p.id) slot = j;
        }
        if (slot < 0) {
          for (int j = 0; j < touch::kMaxPoints && slot < 0; j++) {
            if (!fingers[j].down) slot = j;
          }
          if (slot < 0) continue;
          finger_down(fingers[slot], p, now);
        }
        seen[slot] = true;

        Finger &f = fingers[slot];
        switch (f.role) {
        case Role::kKeys:
          key_points[key_count++] = p;
          break;
        case Role::kSlider:
          if (f.slider == popup) {
            popup_touched_at = now; // the row stays open while its slider is held
            set_slider(f.slider, ui::slider_value(p.x, p.y, f.slider));
          }
          break;
        case Role::kPad:
          pad_keys |= ui::pad_key_at(p.x, p.y);
          break;
        case Role::kSwipe:
          if (!f.arrow) {
            f.arrow = swipe_arrow(p.x - f.x0, p.y - f.y0);
            f.arrow_at = now;
          }
          pad_keys |= f.arrow;
          break;
        case Role::kIgnore:
          break;
        }
      }

      // Lifted fingers. A quick flick still holds its arrow long enough to reach the M8.
      for (int j = 0; j < touch::kMaxPoints; j++) {
        Finger &f = fingers[j];
        if (!f.down || seen[j]) continue;
        f.down = false;
        if (f.role == Role::kSwipe && f.arrow && now - f.arrow_at < SWIPE_MIN_HOLD_MS) {
          flicked |= f.arrow;
          flicked_until = f.arrow_at + SWIPE_MIN_HOLD_MS;
        }
      }
      if (flicked && (int32_t)(now - flicked_until) >= 0) flicked = 0;

      mask = apply_hold(keypad::mask_for(key_points, key_count) | pad_keys | flicked, hold_mode);
      held_keys = mask;
    }

    // The M8 starts with no keys held after a (re)connect.
    const uint32_t connections = m8usb::connection_count();
    if (connections != seen_connections) {
      seen_connections = connections;
      sent_mask = 0;
    }

    // Keys held from a computer through USB passthrough count as held here too.
    const uint8_t out = repeat_arrows(mask, millis()) | passthru::remote_keys();
    if (out != sent_mask && m8usb::state() == m8usb::State::kConnected) {
      const uint8_t msg[2] = {m8proto::kCmdController, out};
      if (m8usb::send(msg, sizeof(msg))) sent_mask = out; // retried next poll if the queue is full
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void audio_sink(const uint8_t *pcm, size_t bytes) {
  audio::push(pcm, bytes);
  if (MICROM8_USB_PASSTHROUGH) passthru::push_audio(pcm, bytes);
}

} // namespace

void setup() {
  logcap::begin();
  Serial.begin(115200);
  screenshot::begin();
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
  rotation_locked = prefs.getUChar("rot_lock", 0) != 0;
  compact_keys = prefs.getUChar("compact", 0) != 0;
  hold_mode = prefs.getUChar("hold", 0) != 0;
  const int saved_orientation = prefs.getUChar("orient", 0);
  const auto orientation = saved_orientation <= (int)ui::Orientation::kLandscapeFlipped
                               ? (ui::Orientation)saved_orientation
                               : ui::Orientation::kPortrait;

  if (!m8screen::init()) ESP_LOGE(TAG, "framebuffer init failed");
  m8proto::init();

  if (!ui::begin(orientation, compact_keys)) ESP_LOGE(TAG, "display init failed");
  const bool touch_ok = touch::begin();
  if (!touch_ok) ESP_LOGE(TAG, "touch init failed; on-screen keys disabled");
  if (!audio::start(speaker_volume, headphone_volume, output)) {
    ESP_LOGE(TAG, "audio init failed");
  }
  if (!mic::start(prefs.getUChar("mic_boost", 0))) ESP_LOGE(TAG, "mic init failed");

  rx_stream = xStreamBufferCreate(kRxStreamBytes, 1);
  if (MICROM8_USB_PASSTHROUGH) m8usb::set_taps(passthru::push_serial, passthru::push_midi);
  if (!m8usb::start(rx_stream, audio_sink, mic::pull)) ESP_LOGE(TAG, "USB host init failed");
  // After audio::start(): routing the I2S pins (GPIO 26/27) turns off the OTG pads passthrough
  // turns on.
  if (MICROM8_USB_PASSTHROUGH && !passthru::start()) ESP_LOGE(TAG, "passthrough init failed");

  xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 10, nullptr, UI_CORE);
  if (touch_ok) xTaskCreatePinnedToCore(touch_task, "touch", 4096, nullptr, 12, nullptr, UI_CORE);
}

void loop() { vTaskDelete(nullptr); }
