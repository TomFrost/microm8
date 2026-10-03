// Off-screen framebuffer for the M8 display. Draw commands from the M8 land here; the
// display layer copies dirty row bands to the LCD.
//
// Not thread-safe: only the UI task touches it.
#pragma once

#include <stdint.h>

namespace m8screen {

struct Rgb {
  uint8_t r, g, b;
};

constexpr int kBandRows = 8;

bool init();

// Reported by the M8's system-info packet. Type 3 is Model:02 (480x320); everything else,
// including Headless, uses the 320x240 layout.
void set_hardware_model(uint8_t hw_type);
void set_font_mode(int mode);

void draw_rect(int x, int y, int w, int h, Rgb color);
void draw_char(char c, int x, int y, Rgb fg, Rgb bg);
void draw_waveform(Rgb color, const uint8_t *samples, int count);

// Local drawing (splash/status messages), outside the M8 coordinate offsets.
void clear(Rgb color);
void fill_rect(int x, int y, int w, int h, Rgb color);
void draw_text(int x, int y, const char *str, Rgb fg, Rgb bg, int font_index = 0);
int glyph_advance(int font_index = 0);

int width();
int height();
bool oversized(); // the M8 resolution exceeds the area reserved for it on the LCD
const uint16_t *framebuffer();

// Returns the bitmask of row bands (kBandRows rows each) touched since the last call.
uint64_t take_dirty_bands();
void mark_all_dirty();

} // namespace m8screen
