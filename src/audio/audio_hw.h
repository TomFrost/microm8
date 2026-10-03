// Audio output hardware (the Tab5's ES8388 codec and speaker amp). The audio engine
// (audio_out.cpp) prepares stereo S16 frames for the selected output; this layer gets them to
// the speaker or headphones.
#pragma once

#include "audio/audio_out.h"

namespace audio_hw {

bool begin(int sample_rate);

// True if there is a headphone output.
bool has_headphones();

// Routes audio to `output`, e.g. switching an amp or I2S port. Called from the audio task.
void route(audio::Output output);

// Writes `frames` stereo frames (interleaved L/R) to the routed output. Blocks until the
// hardware accepts them, which paces the audio task.
void write(const int16_t *stereo, int frames);

// Headphone jack state: 1 inserted, 0 not inserted, -1 unknown (detection not configured).
int headphones_inserted();

} // namespace audio_hw
