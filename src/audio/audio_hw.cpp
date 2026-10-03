// Tab5 audio hardware: the ES8388 codec on I2S0 drives both the headphone jack and the
// NS4150B speaker amp. The ES7210 mic ADC shares the bus (data in on GPIO 28); its receive
// channel is created here so microphone sampling can be added without reworking the bus.
//
// The ES8388 register sequence comes from M5Stack's M5Unified (MIT); see
// LICENSE-THIRD-PARTY.md.
#include "audio/audio_hw.h"

#include <M5Unified.h>
#include <driver/i2s_std.h>
#include <esp_log.h>

#include "config.h"
#include "board.h"

static const char *TAG = "audio_hw";

namespace audio_hw {
namespace {

constexpr uint32_t kI2cHz = 400000;
constexpr uint8_t kIoeOutputReg = 0x05;
constexpr uint8_t kIoeInputReg = 0x0F;
constexpr uint8_t kSpkEnBit = 1 << 1;
constexpr uint8_t kHpDetBit = 1 << 7;

i2s_chan_handle_t tx;
i2s_chan_handle_t rx; // for the mics; created but not enabled yet

// ES8388 in I2S slave mode, 16-bit, DAC on, all four outputs (LOUT1/ROUT1 headphone,
// LOUT2/ROUT2 speaker) enabled, ADC powered down. {register, value} pairs.
constexpr uint8_t kEs8388Init[][2] = {
    {0, 0x80},  {0, 0x00},  {0, 0x00},  {0, 0x0E}, {1, 0x00},
    {2, 0x0A},  // chip power: all up
    {3, 0xFF},  // ADC power: all down
    {4, 0x3C},  // DAC power: up, LOUT1/ROUT1/LOUT2/ROUT2 enabled
    {5, 0x00},  {6, 0x00}, {7, 0x7C},
    {8, 0x00},  // I2S slave
    {23, 0x18}, // I2S format: 16-bit
    {24, 0x00}, // MCLK ratio
    {25, 0x20}, // DAC unmute
    {26, 0x00}, {27, 0x00}, // DAC volume L/R: 0 dB
    {28, 0x08}, // click-free power up/down
    {29, 0x00}, {38, 0x00},
    {39, 0xB8}, {42, 0xB8}, // left/right output mixers: DAC
    {43, 0x08}, // ADC and DAC separate
    {45, 0x00},
    {46, 0x21}, {47, 0x21}, {48, 0x21}, {49, 0x21}, // LOUT1/ROUT1/LOUT2/ROUT2 volume
};

void set_speaker_amp(bool on) {
  board::I2CLock lock;
  if (on) {
    M5.In_I2C.bitOn(TAB5_IOE0_ADDR, kIoeOutputReg, kSpkEnBit, kI2cHz);
  } else {
    M5.In_I2C.bitOff(TAB5_IOE0_ADDR, kIoeOutputReg, kSpkEnBit, kI2cHz);
  }
}

} // namespace

bool begin(int sample_rate) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;
  if (i2s_new_channel(&chan_cfg, &tx, &rx) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed");
    return false;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = (gpio_num_t)TAB5_I2S_MCLK,
              .bclk = (gpio_num_t)TAB5_I2S_BCLK,
              .ws = (gpio_num_t)TAB5_I2S_WS,
              .dout = (gpio_num_t)TAB5_I2S_DOUT,
              .din = (gpio_num_t)TAB5_I2S_DIN,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256; // as M5Unified uses with the ES8388
  if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK ||
      i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
    ESP_LOGE(TAG, "i2s init failed");
    return false;
  }

  // The codec needs MCLK running before it's configured.
  bool ok = true;
  {
    board::I2CLock lock;
    for (const auto &rv : kEs8388Init) {
      ok &= M5.In_I2C.writeRegister8(TAB5_ES8388_ADDR, rv[0], rv[1], kI2cHz);
    }
  }
  if (!ok) ESP_LOGE(TAG, "ES8388 setup failed (I2C)");
  ESP_LOGI(TAG, "ES8388 on I2S0 at %d Hz", sample_rate);
  return ok;
}

bool has_headphones() { return true; }

void route(audio::Output output) {
  // The codec always drives the headphone jack; the speaker amp is switched on only when the
  // speaker is selected.
  set_speaker_amp(output == audio::Output::kSpeaker);
}

void write(const int16_t *stereo, int frames) {
  size_t written;
  i2s_channel_write(tx, stereo, (size_t)frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

int headphones_inserted() {
  // Read HP_DET at most every 100 ms; this is polled from the UI loop.
  static int raw = -1;
  static uint32_t last_read = 0;
  const uint32_t now = millis();
  if (raw < 0 || now - last_read >= 100) {
    last_read = now;
    uint8_t in;
    {
      board::I2CLock lock;
      in = M5.In_I2C.readRegister8(TAB5_IOE0_ADDR, kIoeInputReg, kI2cHz);
    }
    const int level = (in & kHpDetBit) ? 1 : 0;
    if (level != raw) {
      // Logged so HP_DET's polarity can be confirmed: plug and unplug headphones and watch.
      ESP_LOGI(TAG, "HP_DET reads %d", level);
      raw = level;
    }
  }
#if TAB5_HP_DET_INSERTED_LEVEL >= 0
  return raw == TAB5_HP_DET_INSERTED_LEVEL ? 1 : 0;
#else
  return -1;
#endif
}

} // namespace audio_hw
