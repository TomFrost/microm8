// Font table. Adapted from m8c (MIT); see LICENSE-THIRD-PARTY.md.
// Compiled as C: the font headers initialize a flexible array member, which C++ rejects.
#include "fonts.h"
#include "font1.h"
#include "font2.h"
#include "font3.h"
#include "font4.h"
#include "font5.h"

static const struct inline_font *fonts_storage[] = {
    &font_v1_small, &font_v1_large, &font_v2_small, &font_v2_large, &font_v2_huge,
};

#define FONT_COUNT (sizeof(fonts_storage) / sizeof(fonts_storage[0]))

size_t fonts_count(void) { return FONT_COUNT; }

const struct inline_font *fonts_get(size_t index) {
  return index < FONT_COUNT ? fonts_storage[index] : NULL;
}
