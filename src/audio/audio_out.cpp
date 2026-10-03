#include "audio_out.h"

#include <atomic>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

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

i2s_chan_handle_t speaker_tx;
i2s_chan_handle_t headphone_tx; // null unless built with MICROM8_HEADPHONES

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

// A short beep on the selected output at boot: confirms the I2S path, amp and speaker (or
// headphone DAC) work independently of USB audio.
void boot_beep() {
  constexpr int kFreq = 880;
  constexpr int kMs = 120;
  static int16_t buf[kChunkFrames * 2];
  const int32_t gain = level_to_gain_q12(levels[(int)cur_output.load()].load());
  const bool hp_out = headphone_tx && cur_output == Output::kHeadphones;
  const int total = kSampleRate * kMs / 1000;
  const int half_period = kSampleRate / (2 * kFreq);

  for (int done = 0; done < total; done += kChunkFrames) {
    for (int i = 0; i < kChunkFrames; i++) {
      const int n = done + i;
      // Square wave at -12 dBFS with a short fade in/out to avoid clicks.
      int32_t s = ((n / half_period) & 1) ? 8192 : -8192;
      const int edge = n < 400 ? n : (total - n < 400 ? total - n : 400);
      s = s * (edge < 0 ? 0 : edge) / 400;
      buf[i * 2] = buf[i * 2 + 1] = scale(s, gain);
    }
    size_t written;
    i2s_channel_write(hp_out ? headphone_tx : speaker_tx, buf, sizeof(buf), &written,
                      portMAX_DELAY);
  }
}

void audio_task(void *) {
  static int16_t in[(kChunkFrames + 1) * 2];
  static int16_t spk[kChunkFrames * 2];
  static int16_t hp[kChunkFrames * 2];
  bool playing = false;

  boot_beep();

  for (;;) {
    const bool have_audio = read_chunk(in, playing);
    const Output out = cur_output.load(std::memory_order_relaxed);
    const int32_t gain = level_to_gain_q12(levels[(int)out].load(std::memory_order_relaxed));

    const bool to_speaker = have_audio && (out == Output::kSpeaker || !headphone_tx);
    const bool to_headphones = have_audio && headphone_tx && out == Output::kHeadphones;

    if (to_speaker) {
      for (int i = 0; i < kChunkFrames; i++) {
        const int16_t m = scale(((int32_t)in[i * 2] + (int32_t)in[i * 2 + 1]) / 2, gain);
        spk[i * 2] = m; // the NS4168 plays one channel; feed both
        spk[i * 2 + 1] = m;
      }
    } else {
      memset(spk, 0, sizeof(spk));
    }

    if (to_headphones) {
      for (int i = 0; i < kChunkFrames * 2; i++) hp[i] = scale(in[i], gain);
    } else {
      memset(hp, 0, sizeof(hp));
    }

    // Both channels run at the same rate, so each blocking write paces the loop equally.
    size_t written;
    i2s_channel_write(speaker_tx, spk, sizeof(spk), &written, portMAX_DELAY);
    if (headphone_tx) i2s_channel_write(headphone_tx, hp, sizeof(hp), &written, portMAX_DELAY);
  }
}

i2s_chan_handle_t open_output(i2s_port_t port, int bclk, int ws, int dout) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;

  i2s_chan_handle_t tx = nullptr;
  if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel(%d) failed", (int)port);
    return nullptr;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)bclk,
              .ws = (gpio_num_t)ws,
              .dout = (gpio_num_t)dout,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
    ESP_LOGE(TAG, "i2s init on port %d failed", (int)port);
    i2s_del_channel(tx);
    return nullptr;
  }
  return tx;
}

} // namespace

bool start(int speaker_volume, int headphone_volume, Output output) {
  ring = (uint8_t *)heap_caps_malloc(kRingBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!ring) {
    ESP_LOGE(TAG, "ring buffer allocation failed");
    return false;
  }

  speaker_tx = open_output(I2S_NUM_0, I2S_BCLK, I2S_LRCLK, I2S_DOUT);
  if (!speaker_tx) return false;

#if MICROM8_HEADPHONES
  // The PCM5102A derives its master clock from BCK, so MCLK is not needed (tie its SCK to GND).
  headphone_tx = open_output(I2S_NUM_1, HP_BCLK, HP_LRCLK, HP_DOUT);
  if (!headphone_tx) ESP_LOGE(TAG, "headphone output unavailable");
#endif

  levels[(int)Output::kSpeaker] = clamp_level(speaker_volume);
  levels[(int)Output::kHeadphones] = clamp_level(headphone_volume);
  set_output(output);

  xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 18, nullptr, AUDIO_CORE);
  ESP_LOGI(TAG, "I2S output running at %d Hz%s", kSampleRate,
           headphone_tx ? " (speaker + headphone DAC)" : "");
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

bool headphones_available() { return headphone_tx != nullptr; }

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
