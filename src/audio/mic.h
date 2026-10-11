// Microphone capture for the M8's USB audio input, so samples can be recorded on the M8 from
// the Tab5's mics. The mics (audio_hw) are read into a buffer that the USB task drains at
// the M8's rate (m8usb), as 44.1 kHz 16-bit stereo: MIC1 left, MIC2 right.
#pragma once

#include <stdint.h>

namespace mic {

// The mic preamp runs at its maximum, 37.5 dB. On top of that, an optional digital boost of
// +6 or +12 dB (which raises the noise along with the signal): 0 none, 1 +6 dB, 2 +12 dB.
constexpr int kBoostMax = 2;

// Creates the capture task, with the mics off. Call after audio::start().
bool start(int boost);

// Turns capture on or off. Any task.
void set_enabled(bool on);
bool enabled();

// Any task.
void set_boost(int boost);
int boost();

// Fills `frames` stereo frames for the M8 (USB task). The mics run on the Tab5's clock and the
// M8 consumes on its own, so the buffer is kept near its target by now and then dropping or
// repeating a frame. Silence while capture is off or the buffer has run dry.
void pull(int16_t *stereo, int frames);

// Diagnostics: loudest sample captured since the last call, buffered audio, and how often
// the M8 found the buffer empty.
int take_peak();
uint32_t buffered_ms();
uint32_t underruns();

} // namespace mic
