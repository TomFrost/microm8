#include "audio_out.h"

#include <atomic>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "audio/audio_hw.h"
#include "config.h"

static const char *TAG = "audio";

namespace audio {
namespace {

constexpr int kFrameBytes = 4; // S16 stereo
constexpr uint32_t kRingBytes = 32 * 1024; // ~185 ms, power of two
constexpr int kChunkFrames = 128;          // ~2.9 ms per I2S write

// Fill-level targets, in frames.
constexpr uint32_t kPrebufferFrames = 1024; // ~23 ms before (re)starting playback
constexpr uint32_t kLowFrames = 768;
constexpr uint32_t kHighFrames = 2048;

uint8_t *ring;
std::atomic<uint32_t> head{0}; // written by producer
std::atomic<uint32_t> tail{0}; // written by consumer

std::atomic<Output> cur_output{Output::kSpeaker};
std::atomic<int> levels[2] = {{7}, {5}}; // indexed by Output
std::atomic<uint32_t> underrun_count{0};
std::atomic<int> peak{0};

int clamp_level(int level) { return level < 0 ? 0 : (level > kMaxVolume ? kMaxVolume : level); }

int32_t level_to_gain_q12(int level) {
  // Roughly perceptual: quadratic curve, unity at max.
  return (int32_t)((level * level * 4096) / (kMaxVolume * kMaxVolume));
}

inline int16_t scale(int32_t sample, int32_t gain_q12) {
  sample = (sample * gain_q12) >> 12;
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return (int16_t)sample;
}

uint32_t ring_used() {
  return head.load(std::memory_order_acquire) - tail.load(std::memory_order_relaxed);
}

void ring_read(uint8_t *dst, uint32_t bytes) {
  uint32_t t = tail.load(std::memory_order_relaxed);
  const uint32_t idx = t & (kRingBytes - 1);
  const uint32_t first = bytes < kRingBytes - idx ? bytes : kRingBytes - idx;
  memcpy(dst, ring + idx, first);
  memcpy(dst + first, ring, bytes - first);
  tail.store(t + bytes, std::memory_order_release);
}

// Reads one chunk of input, compensating for clock drift. Returns false (and zero-fills) when
// there is not enough buffered audio to play.
bool read_chunk(int16_t *in, bool &playing) {
  const uint32_t avail = ring_used() / kFrameBytes;

  if (!playing && avail >= kPrebufferFrames) playing = true;
  if (playing && avail < kChunkFrames + 1) {
    playing = false;
    underrun_count++;
  }
  if (!playing) {
    memset(in, 0, kChunkFrames * kFrameBytes);
    return false;
  }

  // Consume one frame more or less than we output to hold the buffer near its target.
  int take = kChunkFrames;
  if (avail > kHighFrames) take = kChunkFrames + 1;
  else if (avail < kLowFrames) take = kChunkFrames - 1;

  ring_read((uint8_t *)in, take * kFrameBytes);
  if (take == kChunkFrames - 1) {
    in[(kChunkFrames - 1) * 2] = in[(kChunkFrames - 2) * 2];
    in[(kChunkFrames - 1) * 2 + 1] = in[(kChunkFrames - 2) * 2 + 1];
  }
  return true;
}

// Prepares one chunk for the selected output: stereo for headphones; for the speaker, a mono
// mix in both channels (the boards' speaker amps play a single channel).
void render(const int16_t *in, int16_t *out, Output output, int32_t gain) {
  if (output == Output::kHeadphones) {
    for (int i = 0; i < kChunkFrames * 2; i++) out[i] = scale(in[i], gain);
    return;
  }
  for (int i = 0; i < kChunkFrames; i++) {
    const int16_t m = scale(((int32_t)in[i * 2] + (int32_t)in[i * 2 + 1]) / 2, gain);
    out[i * 2] = m;
    out[i * 2 + 1] = m;
  }
}

// A short beep on the selected output at boot: confirms the audio hardware works
// independently of USB audio.
void boot_beep() {
  constexpr int kFreq = 880;
  constexpr int kMs = 120;
  static int16_t in[kChunkFrames * 2];
  static int16_t out[kChunkFrames * 2];
  const Output output = cur_output.load();
  const int32_t gain = level_to_gain_q12(levels[(int)output].load());
  const int total = kSampleRate * kMs / 1000;
  const int half_period = kSampleRate / (2 * kFreq);

  for (int done = 0; done < total; done += kChunkFrames) {
    for (int i = 0; i < kChunkFrames; i++) {
      const int n = done + i;
      // Square wave at -12 dBFS with a short fade in/out to avoid clicks.
      int32_t s = ((n / half_period) & 1) ? 8192 : -8192;
      const int edge = n < 400 ? n : (total - n < 400 ? total - n : 400);
      s = s * (edge < 0 ? 0 : edge) / 400;
      in[i * 2] = in[i * 2 + 1] = (int16_t)s;
    }
    render(in, out, output, gain);
    audio_hw::write(out, kChunkFrames);
  }
}

void audio_task(void *) {
  static int16_t in[(kChunkFrames + 1) * 2];
  static int16_t out[kChunkFrames * 2];
  bool playing = false;

  Output routed = cur_output.load();
  audio_hw::route(routed);
  boot_beep();

  for (;;) {
    const Output output = cur_output.load(std::memory_order_relaxed);
    if (output != routed) {
      routed = output;
      audio_hw::route(routed);
    }

    if (read_chunk(in, playing)) {
      render(in, out, output, level_to_gain_q12(levels[(int)output].load(std::memory_order_relaxed)));
    } else {
      memset(out, 0, sizeof(out));
    }
    audio_hw::write(out, kChunkFrames);
  }
}

} // namespace

bool start(int speaker_volume, int headphone_volume, Output output) {
  ring = (uint8_t *)heap_caps_malloc(kRingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ring) {
    ESP_LOGE(TAG, "ring buffer allocation failed");
    return false;
  }

  if (!audio_hw::begin(kSampleRate)) {
    ESP_LOGE(TAG, "audio hardware init failed");
    return false;
  }

  levels[(int)Output::kSpeaker] = clamp_level(speaker_volume);
  levels[(int)Output::kHeadphones] = clamp_level(headphone_volume);
  set_output(output);

  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 18, nullptr, AUDIO_CORE);
  ESP_LOGI(TAG, "audio output running at %d Hz%s", kSampleRate,
           audio_hw::has_headphones() ? " (speaker + headphones)" : "");
  return true;
}

void push(const uint8_t *pcm, size_t bytes) {
  if (!ring) return;
  bytes -= bytes % kFrameBytes;

  int p = 0;
  const int16_t *s = (const int16_t *)pcm;
  for (size_t i = 0; i < bytes / 2; i++) {
    const int a = s[i] < 0 ? -s[i] : s[i];
    if (a > p) p = a;
  }
  if (p > peak.load(std::memory_order_relaxed)) peak.store(p, std::memory_order_relaxed);

  const uint32_t h = head.load(std::memory_order_relaxed);
  const uint32_t used = h - tail.load(std::memory_order_acquire);
  if (used + bytes > kRingBytes) return; // overflow: drop this packet

  const uint32_t idx = h & (kRingBytes - 1);
  const uint32_t first = bytes < kRingBytes - idx ? bytes : kRingBytes - idx;
  memcpy(ring + idx, pcm, first);
  memcpy(ring, pcm + first, bytes - first);
  head.store(h + bytes, std::memory_order_release);
}

bool headphones_available() { return audio_hw::has_headphones(); }

void set_output(Output output) {
  if (output == Output::kHeadphones && !headphones_available()) output = Output::kSpeaker;
  cur_output = output;
}

Output output() { return cur_output; }

void set_volume(int level) { levels[(int)cur_output.load()] = clamp_level(level); }

int volume() { return levels[(int)cur_output.load()]; }
int volume_for(Output output) { return levels[(int)output]; }
uint32_t underruns() { return underrun_count; }
int take_peak() { return peak.exchange(0); }
uint32_t buffered_ms() { return ring_used() / kFrameBytes * 1000 / kSampleRate; }

} // namespace audio
