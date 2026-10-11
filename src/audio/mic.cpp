#include "audio/mic.h"

#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "audio/audio_hw.h"
#include "config.h"

static const char *TAG = "mic";

namespace mic {
namespace {

constexpr int kSampleRate = 44100;
constexpr int kFrameBytes = 4;               // S16 stereo
constexpr uint32_t kRingBytes = 16 * 1024;   // ~93 ms, power of two
constexpr int kReadFrames = 128;             // ~2.9 ms per I2S read
constexpr uint32_t kPrebufferFrames = 768;   // ~17 ms before delivering
constexpr uint32_t kLowFrames = 512;
constexpr uint32_t kHighFrames = 1280;
constexpr int kMaxPull = 16; // frames per USB packet is 11 or 12

constexpr int kPreampStep = 14; // ES7210 preamp: 37.5 dB, its maximum
constexpr int kBoostMultiplier[kBoostMax + 1] = {1, 2, 4}; // 0, +6, +12 dB

uint8_t *ring;
std::atomic<uint32_t> head{0}; // written by the capture task
std::atomic<uint32_t> tail{0}; // written by pull()
bool primed;                   // pull() only

std::atomic<bool> want_on{false};
std::atomic<int> boost_level{0};
std::atomic<int> peak{0};
std::atomic<uint32_t> underrun_count{0};
TaskHandle_t task;

uint32_t ring_used() {
  return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
}

void ring_write(const int16_t *frames, int n) {
  const uint32_t bytes = (uint32_t)n * kFrameBytes;
  const uint32_t h = head.load(std::memory_order_relaxed);
  if (h - tail.load(std::memory_order_acquire) + bytes > kRingBytes) return; // full: drop
  const uint32_t idx = h & (kRingBytes - 1);
  const uint32_t first = bytes < kRingBytes - idx ? bytes : kRingBytes - idx;
  memcpy(ring + idx, frames, first);
  memcpy(ring, (const uint8_t *)frames + first, bytes - first);
  head.store(h + bytes, std::memory_order_release);
}

void ring_read(int16_t *frames, int n) {
  const uint32_t bytes = (uint32_t)n * kFrameBytes;
  const uint32_t t = tail.load(std::memory_order_relaxed);
  const uint32_t idx = t & (kRingBytes - 1);
  const uint32_t first = bytes < kRingBytes - idx ? bytes : kRingBytes - idx;
  memcpy(frames, ring + idx, first);
  memcpy((uint8_t *)frames + first, ring, bytes - first);
  tail.store(t + bytes, std::memory_order_release);
}

void capture_task(void *) {
  static int16_t buf[kReadFrames * 2];
  bool on = false;
  audio_hw::mic_set_gain(kPreampStep);
  for (;;) {
    const bool want = want_on.load();
    if (want != on) on = audio_hw::mic_enable(want) && want;
    if (!on) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
      continue;
    }

    const int n = audio_hw::mic_read(buf, kReadFrames, 20);
    if (n <= 0) continue;
    const int mult = kBoostMultiplier[boost_level.load()];
    int p = 0;
    for (int i = 0; i < n * 2; i++) {
      int v = buf[i] * mult;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      buf[i] = (int16_t)v;
      const int a = v < 0 ? -v : v;
      if (a > p) p = a;
    }
    if (p > peak.load(std::memory_order_relaxed)) peak.store(p, std::memory_order_relaxed);
    ring_write(buf, n);
  }
}

} // namespace

bool start(int boost) {
  ring = (uint8_t *)heap_caps_malloc(kRingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ring) {
    ESP_LOGE(TAG, "buffer allocation failed");
    return false;
  }
  set_boost(boost);
  xTaskCreatePinnedToCore(capture_task, "mic", 4096, nullptr, 17, &task, AUDIO_CORE);
  return true;
}

void set_enabled(bool on) {
  want_on = on;
  if (task) xTaskNotifyGive(task);
}

bool enabled() { return want_on; }

void set_boost(int boost) { boost_level = boost < 0 ? 0 : (boost > kBoostMax ? kBoostMax : boost); }

int boost() { return boost_level; }

void pull(int16_t *stereo, int frames) {
  if (frames <= 0) return;
  if (frames > kMaxPull - 1) frames = kMaxPull - 1;
  const uint32_t avail = ring ? ring_used() / kFrameBytes : 0;

  if (!want_on || !ring) {
    primed = false;
    memset(stereo, 0, frames * kFrameBytes);
    return;
  }
  if (!primed && avail >= kPrebufferFrames) primed = true;
  if (primed && avail < (uint32_t)frames + 1) {
    primed = false;
    underrun_count++;
  }
  if (!primed) {
    memset(stereo, 0, frames * kFrameBytes);
    return;
  }

  // One frame more or less than asked for, to hold the buffer near its target.
  int take = frames;
  if (avail > kHighFrames) {
    take = frames + 1;
  } else if (avail < kLowFrames && frames > 1) {
    take = frames - 1;
  }
  int16_t tmp[kMaxPull * 2];
  ring_read(tmp, take);
  if (take < frames) { // repeat the last frame
    tmp[take * 2] = tmp[(take - 1) * 2];
    tmp[take * 2 + 1] = tmp[(take - 1) * 2 + 1];
  }
  memcpy(stereo, tmp, frames * kFrameBytes); // a dropped frame is simply not copied
}

int take_peak() { return peak.exchange(0); }

uint32_t buffered_ms() { return ring ? ring_used() / kFrameBytes * 1000 / kSampleRate : 0; }

uint32_t underruns() { return underrun_count; }

} // namespace mic
