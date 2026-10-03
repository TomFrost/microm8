// M8 framebuffer renderer. Drawing semantics follow m8c's render.c / inprint2.c
// (MIT, Jonne Kokkonen) so that font offsets, background tracking and the oscilloscope
// behave identically.

#include "screen.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>

#include "config.h"
#include "fonts/fonts.h"

static const char *TAG = "m8screen";

namespace m8screen {
namespace {

constexpr int kMaxW = 480;
constexpr int kMaxH = 320;
constexpr int kGlyphs = 94;     // ASCII 33..126
constexpr int kFirstGlyph = 33; // m8c: 127 - CHARACTERS_PER_ROW
constexpr int kMaxGlyphRows = 16;
constexpr int kMaxFonts = 5;

struct DecodedFont {
  const inline_font *meta;
  int glyph_w; // source glyph width in the BMP strip
  int glyph_h;
  uint16_t rows[kGlyphs][kMaxGlyphRows]; // bit c set = ink at column c
};

DecodedFont fonts[kMaxFonts];
int font_count = 0;

uint16_t *fb = nullptr;
int fb_w = M8_SCREEN_W;
int fb_h = M8_SCREEN_H;
uint64_t dirty = 0;

uint8_t hw_model = 0;
int requested_font_mode = 0; // as sent by the M8 (0..2)
int font_mode = -1;          // index into fonts[]
const DecodedFont *font = nullptr;
int screen_offset_y = 0;
int text_offset_y = 0;
int waveform_max_height = 24;

Rgb background = {0, 0, 0};
int prev_waveform_size = 0;
bool waveform_cleared = false;

inline uint16_t rgb565(Rgb c) {
  return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}

inline uint32_t rgb24(Rgb c) { return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b; }

inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void mark_rows(int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 >= fb_h) y1 = fb_h - 1;
  if (y0 > y1) return;
  for (int b = y0 / kBandRows; b <= y1 / kBandRows; b++) dirty |= (1ULL << b);
}

void fill(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > fb_w) w = fb_w - x;
  if (y + h > fb_h) h = fb_h - y;
  if (w <= 0 || h <= 0) return;

  for (int row = y; row < y + h; row++) {
    uint16_t *p = fb + row * fb_w + x;
    for (int i = 0; i < w; i++) p[i] = color;
  }
  mark_rows(y, y + h - 1);
}

inline void plot(int x, int y, uint16_t color) {
  if ((unsigned)x < (unsigned)fb_w && (unsigned)y < (unsigned)fb_h) fb[y * fb_w + x] = color;
}

// Decodes one of m8c's embedded 1-bit BMP font strips into per-glyph row masks.
bool decode_font(const inline_font *meta, DecodedFont *out) {
  const uint8_t *bmp = meta->image_data;
  if (meta->image_size < 62 || bmp[0] != 'B' || bmp[1] != 'M') return false;

  const uint32_t data_off = rd32(bmp + 10);
  const uint32_t hdr_size = rd32(bmp + 14);
  const int32_t w = (int32_t)rd32(bmp + 18);
  const int32_t h_raw = (int32_t)rd32(bmp + 22);
  const uint16_t bpp = bmp[28] | (bmp[29] << 8);
  if (bpp != 1) return false;

  const bool bottom_up = h_raw > 0;
  const int h = bottom_up ? h_raw : -h_raw;
  const int stride = ((w + 31) / 32) * 4;

  // Palettes differ between the font files; whichever entry is not black is the ink.
  const uint8_t *pal = bmp + 14 + hdr_size;
  const int ink = (pal[0] | pal[1] | pal[2]) ? 0 : 1;

  out->meta = meta;
  out->glyph_w = w / kGlyphs;
  out->glyph_h = h;
  if (out->glyph_w > 16 || h > kMaxGlyphRows) return false;
  if (data_off + (uint32_t)(stride * h) > (uint32_t)meta->image_size) return false;

  memset(out->rows, 0, sizeof(out->rows));
  for (int row = 0; row < h; row++) {
    const uint8_t *src = bmp + data_off + (bottom_up ? (h - 1 - row) : row) * stride;
    for (int g = 0; g < kGlyphs; g++) {
      uint16_t mask = 0;
      for (int c = 0; c < out->glyph_w; c++) {
        const int x = g * out->glyph_w + c;
        const int bit = (src[x >> 3] >> (7 - (x & 7))) & 1;
        if (bit == ink) mask |= (1u << c);
      }
      out->rows[g][row] = mask;
    }
  }
  return true;
}

void blit_glyph(const DecodedFont *f, char c, int x, int y, uint16_t fg) {
  const int id = (uint8_t)c - kFirstGlyph;
  if (id < 0 || id >= kGlyphs) return; // includes space: the fonts have no glyph for it
  for (int row = 0; row < f->glyph_h; row++) {
    const uint16_t mask = f->rows[id][row];
    if (!mask) continue;
    for (int col = 0; col < f->glyph_w; col++) {
      if (mask & (1u << col)) plot(x + col, y + row, fg);
    }
  }
  mark_rows(y, y + f->glyph_h - 1);
}

void resize(int w, int h) {
  if (w == fb_w && h == fb_h) return;
  fb_w = w;
  fb_h = h;
  memset(fb, 0, sizeof(uint16_t) * kMaxW * kMaxH);
  mark_all_dirty();
}

} // namespace

bool init() {
  fb = (uint16_t *)heap_caps_calloc(kMaxW * kMaxH, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  if (!fb) {
    ESP_LOGW(TAG, "PSRAM framebuffer allocation failed, trying internal RAM");
    fb = (uint16_t *)heap_caps_calloc(kMaxW * kMaxH, sizeof(uint16_t), MALLOC_CAP_8BIT);
  }
  if (!fb) {
    ESP_LOGE(TAG, "framebuffer allocation failed");
    return false;
  }

  font_count = 0;
  for (size_t i = 0; i < fonts_count() && font_count < kMaxFonts; i++) {
    if (!decode_font(fonts_get(i), &fonts[font_count])) {
      ESP_LOGE(TAG, "failed to decode font %u", (unsigned)i);
      return false;
    }
    font_count++;
  }

  set_font_mode(0);
  mark_all_dirty();
  return true;
}

void set_hardware_model(uint8_t hw_type) {
  const uint8_t model = (hw_type == 0x03) ? 1 : 0;
  if (model == hw_model) return;
  hw_model = model;
  if (model) {
    ESP_LOGW(TAG, "M8 reports Model:02 (480x320); only the top-left 320x240 will be visible");
    resize(480, 320);
  } else {
    resize(320, 240);
  }
  // Font indices shift by 2 for Model:02, so re-resolve the requested mode.
  font_mode = -1;
  set_font_mode(requested_font_mode);
}

void set_font_mode(int mode) {
  if (mode < 0 || mode > 2) return;
  requested_font_mode = mode;
  if (hw_model == 1) mode += 2;
  if (mode >= font_count || mode == font_mode) return;

  font_mode = mode;
  font = &fonts[mode];
  screen_offset_y = font->meta->screen_offset_y;
  text_offset_y = font->meta->text_offset_y;
  waveform_max_height = font->meta->waveform_max_height;
  ESP_LOGI(TAG, "font mode %d, screen offset %d", mode, screen_offset_y);
}

void draw_rect(int x, int y, int w, int h, Rgb color) {
  y += screen_offset_y;

  // A full-screen fill means the M8 changed its background colour (theme change / clear).
  if (x == 0 && y <= 0 && w == fb_w && h >= fb_h) background = color;

  fill(x, y, w, h, rgb565(color));
}

void draw_char(char c, int x, int y, Rgb fg, Rgb bg) {
  if (!font) return;
  y += text_offset_y + screen_offset_y;

  // Matching fg/bg means a transparent background.
  if (rgb24(fg) != rgb24(bg)) fill(x, y, font->meta->glyph_x, font->meta->glyph_y, rgb565(bg));
  blit_glyph(font, c, x, y, rgb565(fg));
}

void draw_waveform(Rgb color, const uint8_t *samples, int count) {
  // Skip if the scope is hidden and has already been cleared.
  if (waveform_cleared && count == 0) return;

  const int clear_w = count > 0 ? count : prev_waveform_size;
  const int x0 = fb_w - clear_w;
  fill(x0, 0, clear_w, waveform_max_height + 1, rgb565(background));
  prev_waveform_size = count;

  const uint16_t c = rgb565(color);
  const int sx = fb_w - count;
  for (int i = 0; i < count; i++) {
    int v = samples[i];
    if (v > waveform_max_height) v = waveform_max_height; // m8c: avoid random glitches
    plot(sx + i, v, c);
  }
  waveform_cleared = (count == 0);
}

void clear(Rgb color) {
  background = color;
  fill(0, 0, fb_w, fb_h, rgb565(color));
}

void fill_rect(int x, int y, int w, int h, Rgb color) { fill(x, y, w, h, rgb565(color)); }

void draw_text(int x, int y, const char *str, Rgb fg, Rgb bg, int font_index) {
  if (font_index < 0 || font_index >= font_count) return;
  const DecodedFont *f = &fonts[font_index];
  const uint16_t fg565 = rgb565(fg);
  const uint16_t bg565 = rgb565(bg);
  const bool opaque = rgb24(fg) != rgb24(bg);
  for (; *str; str++) {
    if (opaque) fill(x, y, f->meta->glyph_x + 1, f->meta->glyph_y, bg565);
    blit_glyph(f, *str, x, y, fg565);
    x += f->meta->glyph_x + 1;
  }
}

int glyph_advance(int font_index) {
  if (font_index < 0 || font_index >= font_count) return 0;
  return fonts[font_index].meta->glyph_x + 1;
}

int width() { return fb_w; }
int height() { return fb_h; }
bool oversized() { return fb_w > M8_SCREEN_W || fb_h > M8_SCREEN_H; }
const uint16_t *framebuffer() { return fb; }

uint64_t take_dirty_bands() {
  const uint64_t d = dirty;
  dirty = 0;
  return d;
}

void mark_all_dirty() { mark_rows(0, fb_h - 1); }

} // namespace m8screen
