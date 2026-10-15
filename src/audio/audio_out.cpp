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

// Silence gate. The ES8388 appears to mute itself on exact digital silence, but stays active
// (with an audible noise floor) while it receives even a few LSB of signal, and the M8 leaves
// such low-level residue after playback stops. Once the input stays at or below this level
// for kGateHoldFrames, the output becomes exact zeros; it reopens on the first louder chunk.
constexpr int kGateThreshold = 8;                         // LSB peak, about -72 dBFS
constexpr uint32_t kGateHoldFrames = kSampleRate / 4;     // 250 ms

uint8_t *ring;
std::atomic<uint32_t> head{0}; // written by producer
std::atomic<uint32_t> tail{0}; // written by consumer

std::atomic<Output> cur_output{Output::kSpeaker};
std::atomic<int> levels[2] = {{7}, {5}}; // indexed by Output
std::atomic<uint32_t> underrun_count{0};
std::atomic<int> peak{0};

int clamp_level(int level) { return level < 0 ? 0 : (level > kMaxVolume ? kMaxVolume : level); }

// Volume is applied in the codec's analog output stage (audio_hw::set_volume), which lowers
// its noise floor along with the music. Samples stay at full scale; only level 0 is digital
// silence.
constexpr int32_t kUnityGain = 4096; // Q12
int32_t digital_gain_q12(int level) { return level > 0 ? kUnityGain : 0; }

inline int16_t scale(int32_t sample, int32_t gain_q12) {
  // Round symmetrically (half away from zero). A plain >> rounds toward minus infinity, which
  // turns tiny negative samples into a persistent -1 at any volume.
  const int32_t scaled = sample * gain_q12;
  sample = (scaled + (scaled >= 0 ? 2048 : -2048)) / 4096;
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
// mix in both channels (the Tab5's speaker amp plays a single channel).
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

// Largest absolute sample in one chunk of interleaved stereo input.
int chunk_peak(const int16_t *in) {
  int peak = 0;
  for (int i = 0; i < kChunkFrames * 2; i++) {
    const int a = in[i] < 0 ? -in[i] : in[i];
    if (a > peak) peak = a;
  }
  return peak;
}

void audio_task(void *) {
  static int16_t in[(kChunkFrames + 1) * 2];
  static int16_t out[kChunkFrames * 2];
  bool playing = false;
  uint32_t quiet_frames = kGateHoldFrames; // start gated: nothing has played yet
  int applied_level = -1;                  // routed output's volume, as last sent to the codec

  Output routed = cur_output.load();
  audio_hw::route(routed);

  for (;;) {
    const Output output = cur_output.load(std::memory_order_relaxed);
    if (output != routed) {
      routed = output;
      audio_hw::route(routed);
    }
    const int level = levels[(int)output].load(std::memory_order_relaxed);
    if (level != applied_level) {
      applied_level = level;
      audio_hw::set_volume(level);
    }

    bool audible = read_chunk(in, playing);
    if (audible) {
      if (chunk_peak(in) > kGateThreshold) {
        quiet_frames = 0;
      } else if (quiet_frames < kGateHoldFrames) {
        quiet_frames += kChunkFrames;
      }
      audible = quiet_frames < kGateHoldFrames;
    }

    if (audible) {
      render(in, out, output, digital_gain_q12(levels[(int)output].load(std::memory_order_relaxed)));
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
