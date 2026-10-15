// Plays the M8's USB audio stream through the Tab5's speaker or headphone jack (hardware in
// audio_hw.cpp).
//
// USB delivers 44.1 kHz S16LE stereo. The speaker is mono, so channels are mixed down for it;
// headphones keep stereo. Only one output plays at a time, and each output remembers its own
// volume, which is applied in the codec's analog output stage.
//
// The M8's clock and the Tab5's I2S clock drift relative to each other, so the ring buffer level is
// held near a target by occasionally dropping or repeating a single frame.
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

// True if there is a headphone output (always, on the Tab5).
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
