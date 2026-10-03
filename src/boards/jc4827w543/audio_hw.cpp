// JC4827W543 audio hardware: the onboard NS4168 speaker amp on I2S0, and optionally an
// external I2S DAC (e.g. PCM5102A) for headphones on I2S1 (-DMICROM8_HEADPHONES=1).
#include "audio/audio_hw.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_log.h>
#include <string.h>

#include "config.h"

static const char *TAG = "audio_hw";

namespace audio_hw {
namespace {

i2s_chan_handle_t speaker_tx;
i2s_chan_handle_t headphone_tx; // null unless built with MICROM8_HEADPHONES
audio::Output routed = audio::Output::kSpeaker;

i2s_chan_handle_t open_output(i2s_port_t port, int sample_rate, int bclk, int ws, int dout) {
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
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
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

bool begin(int sample_rate) {
  speaker_tx = open_output(I2S_NUM_0, sample_rate, I2S_BCLK, I2S_LRCLK, I2S_DOUT);
  if (!speaker_tx) return false;

#if MICROM8_HEADPHONES
  // The PCM5102A derives its master clock from BCK, so MCLK is not needed (tie its SCK to GND).
  headphone_tx = open_output(I2S_NUM_1, sample_rate, HP_BCLK, HP_LRCLK, HP_DOUT);
  if (!headphone_tx) ESP_LOGE(TAG, "headphone output unavailable");
#if HP_DETECT_PIN >= 0
  pinMode(HP_DETECT_PIN, INPUT_PULLUP);
#endif
#endif
  return true;
}

bool has_headphones() { return headphone_tx != nullptr; }

void route(audio::Output output) { routed = output; }

void write(const int16_t *stereo, int frames) {
  // Both I2S ports run continuously at the same rate; the unrouted one gets silence, so each
  // blocking write paces the audio task equally.
  static int16_t silence[512 * 2];
  const size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
  const bool to_headphones = headphone_tx && routed == audio::Output::kHeadphones;

  size_t written;
  i2s_channel_write(speaker_tx, to_headphones ? silence : stereo, bytes, &written, portMAX_DELAY);
  if (headphone_tx) {
    i2s_channel_write(headphone_tx, to_headphones ? stereo : silence, bytes, &written,
                      portMAX_DELAY);
  }
}

int headphones_inserted() {
#if MICROM8_HEADPHONES && HP_DETECT_PIN >= 0
  return digitalRead(HP_DETECT_PIN) == HP_DETECT_INSERTED_LEVEL ? 1 : 0;
#else
  return -1;
#endif
}

} // namespace audio_hw
