// Plays the M8's USB audio stream through the board's NS4168 speaker amp, and optionally
// through an external I2S DAC (e.g. a PCM5102A module) for stereo headphones.
//
// USB delivers 44.1 kHz S16LE stereo. The speaker is mono, so channels are mixed down for it;
// the headphone output keeps stereo. Only one output plays at a time (the other gets
// silence), and each output remembers its own volume.
//
// The M8 and the ESP32's I2S clocks drift relative to each other, so the ring buffer level is
// held near a target by occasionally dropping or repeating a single frame. Both I2S outputs
// are clocked from the same source at the same rate, so they never drift from each other.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio {

constexpr int kSampleRate = 44100;
constexpr int kMaxVolume = 10;

enum class Output : uint8_t { kSpeaker, kHeadphones };

bool start(int speaker_volume, int headphone_volume, Output output);

// Producer side, called from the USB task.
void push(const uint8_t *pcm, size_t bytes);

// True when the firmware was built with the headphone DAC output (MICROM8_HEADPHONES).
bool headphones_available();

void set_output(Output output);
Output output();

// Volume of the currently selected output, 0..kMaxVolume.
void set_volume(int level);
int volume();
int volume_for(Output output);

uint32_t underruns();
uint32_t buffered_ms();

// Largest absolute sample value received from USB since the last call (0 = silence).
int take_peak();

} // namespace audio
