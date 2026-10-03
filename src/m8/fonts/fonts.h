// M8 bitmap fonts. The font*.h files are taken unmodified from m8c
// (MIT, Jonne Kokkonen); each embeds a 1-bit BMP strip of 94 glyphs (ASCII 33..126).
#pragma once

#include <stddef.h>

struct inline_font {
  const int width;  // BMP width: 94 glyphs side by side
  const int height; // BMP height
  const int glyph_x;
  const int glyph_y;
  const int screen_offset_x;
  const int screen_offset_y;
  const int text_offset_y;
  const int waveform_max_height;
  const long image_size;
  const unsigned char image_data[];
};

#ifdef __cplusplus
extern "C" {
#endif

size_t fonts_count(void);
const struct inline_font *fonts_get(size_t index);

#ifdef __cplusplus
}
#endif
