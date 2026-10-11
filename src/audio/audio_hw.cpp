// Tab5 audio hardware: the ES8388 codec on I2S0 drives both the headphone jack and the
// NS4150B speaker amp. The ES7210 mic ADC shares the bus (its data in on GPIO 28), slaved to
// the same clocks, so mic capture runs at the playback rate.
//
// The ES8388 and ES7210 register sequences come from M5Stack's M5Unified (MIT); see
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

// ES8388 output volume registers LOUT1, ROUT1, LOUT2, ROUT2 (46..49), in 1.5 dB steps:
// 0x00 = -45 dB, 0x1E = 0 dB, 0x21 = +4.5 dB. Which pair feeds the speaker amp isn't
// documented, and only one output plays at a time, so all four get the routed volume.
constexpr uint8_t kRegOutVolFirst = 46;
constexpr uint8_t kRegOutVolLast = 49;
constexpr uint8_t kOutVolMax = 0x21;
constexpr int kOutVolStepsPerLevel = 3; // 4.5 dB per volume level

constexpr uint8_t kRegChipPower = 2;
constexpr uint8_t kChipPowerResetStm = 0xF0; // digital powered down, ADC/DAC state machines reset
constexpr uint8_t kChipPowerDacOn = 0x0A;    // DAC up; ADC DLL and reference down

i2s_chan_handle_t tx;
i2s_chan_handle_t rx; // the mics; enabled by mic_enable()
bool rx_on;

// ES7210 for the Tab5's two mics: MIC1/MIC2 as left/right on SDOUT1, standard I2S, 16-bit,
// MIC3/MIC4 (unused) powered down. {register, value} pairs, from M5Unified's Tab5 setup.
constexpr uint8_t kEs7210Addr = 0x40;
constexpr uint8_t kEs7210Gain1 = 0x43, kEs7210Gain2 = 0x44;
constexpr uint8_t kEs7210GainEnable = 0x10; // PGA on, plus the step in the low nibble
constexpr uint8_t kEs7210Init[][2] = {
    {0x00, 0xFF}, {0x00, 0x41}, // reset
    {0x01, 0x1F},               // clocks off while configuring
    {0x06, 0x00},               // digital power up
    {0x07, 0x20},               // ADC oversampling
    {0x08, 0x10},               // slave mode
    {0x09, 0x30}, {0x0A, 0x30}, // chip init timing
    {0x20, 0x0A}, {0x21, 0x2A}, {0x22, 0x0A}, {0x23, 0x2A}, // high-pass filters
    {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00}, // clock dividers
    {0x11, 0x60},               // 16-bit, I2S
    {0x40, 0x42},               // analog power
    {0x41, 0x70}, {0x42, 0x70}, // mic bias
    {0x45, 0x00}, {0x46, 0x00}, // MIC3/4 gain
    {0x47, 0x00}, {0x48, 0x00}, {0x49, 0x00}, {0x4A, 0x00}, // low power off
    {0x4B, 0x00},               // MIC1/2 powered
    {0x4C, 0xFF},               // MIC3/4 powered down
    {0x01, 0x14},               // clocks on
};
int mic_gain_step = 11; // 33 dB, as M5Unified uses

// ES8388 in I2S slave mode, 16-bit, DAC on, all four outputs (LOUT1/ROUT1 headphone,
// LOUT2/ROUT2 speaker) enabled, ADC powered down. {register, value} pairs.
constexpr uint8_t kEs8388Init[][2] = {
    {0, 0x80},  {0, 0x00},  {0, 0x00},  {0, 0x0E}, {1, 0x00},
    {2, 0x0A},  // chip power: DAC up, ADC down (kChipPowerDacOn)
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

void write_es7210_gain() {
  const uint8_t v = kEs7210GainEnable | (uint8_t)mic_gain_step;
  board::I2CLock lock;
  M5.In_I2C.writeRegister8(kEs7210Addr, kEs7210Gain1, v, kI2cHz);
  M5.In_I2C.writeRegister8(kEs7210Addr, kEs7210Gain2, v, kI2cHz);
}

} // namespace

bool mic_enable(bool on) {
  if (on == rx_on) return true;
  bool ok = true;
  if (on) {
    {
      board::I2CLock lock;
      for (const auto &rv : kEs7210Init) {
        ok &= M5.In_I2C.writeRegister8(kEs7210Addr, rv[0], rv[1], kI2cHz);
      }
    }
    write_es7210_gain();
    if (!ok) ESP_LOGE(TAG, "ES7210 setup failed (I2C)");
    ok = ok && i2s_channel_enable(rx) == ESP_OK;
  } else {
    i2s_channel_disable(rx);
    board::I2CLock lock;
    M5.In_I2C.writeRegister8(kEs7210Addr, 0x4B, 0xFF, kI2cHz); // MIC1/2 powered down
    M5.In_I2C.writeRegister8(kEs7210Addr, 0x06, 0x07, kI2cHz); // digital powered down
  }
  rx_on = on && ok;
  ESP_LOGI(TAG, "mics %s", rx_on ? "on" : "off");
  return ok;
}

void mic_set_gain(int step) {
  if (step < 0) step = 0;
  if (step > kMicGainMax) step = kMicGainMax;
  mic_gain_step = step;
  if (rx_on) write_es7210_gain();
}

int mic_read(int16_t *stereo, int frames, int wait_ms) {
  if (!rx_on) return 0;
  size_t got = 0;
  i2s_channel_read(rx, stereo, (size_t)frames * 2 * sizeof(int16_t), &got,
                   pdMS_TO_TICKS(wait_ms));
  return (int)(got / (2 * sizeof(int16_t)));
}

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
  // The control-port reset at the start of the sequence doesn't restart the codec's ADC/DAC
  // state machines, and after a reset of the P4 alone (flashing, the reset button) they were
  // left running from the previous boot with MCLK pulled from under them. Restart them, as
  // Espressif's ES8388 driver does when starting the codec.
  {
    board::I2CLock lock;
    ok &= M5.In_I2C.writeRegister8(TAB5_ES8388_ADDR, kRegChipPower, kChipPowerResetStm, kI2cHz);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  {
    board::I2CLock lock;
    ok &= M5.In_I2C.writeRegister8(TAB5_ES8388_ADDR, kRegChipPower, kChipPowerDacOn, kI2cHz);
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

void set_volume(int level) {
  // Level 10 = +4.5 dB (the previous fixed setting), 9 = 0 dB, 1 = -36 dB. Level 0 is digital
  // silence in the audio engine; the codec also goes to its minimum.
  int reg = level <= 0 ? 0 : kOutVolMax - (audio::kMaxVolume - level) * kOutVolStepsPerLevel;
  if (reg < 0) reg = 0;
  board::I2CLock lock;
  for (uint8_t r = kRegOutVolFirst; r <= kRegOutVolLast; r++) {
    M5.In_I2C.writeRegister8(TAB5_ES8388_ADDR, r, (uint8_t)reg, kI2cHz);
  }
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
      ESP_LOGI(TAG, "headphones %s", level == TAB5_HP_DET_INSERTED_LEVEL ? "in" : "out");
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
