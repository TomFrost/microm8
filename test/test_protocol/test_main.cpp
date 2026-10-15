// Host-side tests for the M8 protocol parser and framebuffer renderer.
// Packets are built in the M8's wire format (SLIP-framed, little-endian fields).

#include <unity.h>

#include <string.h>
#include <vector>

#include "m8/protocol.h"
#include "m8/screen.h"

namespace {

std::vector<uint8_t> wire;

void slip(std::initializer_list<uint8_t> payload) {
  for (uint8_t b : payload) {
    if (b == 0xC0) {
      wire.push_back(0xDB);
      wire.push_back(0xDC);
    } else if (b == 0xDB) {
      wire.push_back(0xDB);
      wire.push_back(0xDD);
    } else {
      wire.push_back(b);
    }
  }
  wire.push_back(0xC0);
}

void flush(size_t chunk = 37) {
  for (size_t i = 0; i < wire.size(); i += chunk) {
    m8proto::feed(wire.data() + i, wire.size() - i < chunk ? wire.size() - i : chunk);
  }
  wire.clear();
}

void system_info(uint8_t hw, uint8_t font) { slip({0xFF, hw, 6, 1, 2, font}); }

void rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  slip({0xFE, (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), (uint8_t)w,
        (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), r, g, b});
}

uint16_t px(int x, int y) { return m8screen::framebuffer()[y * m8screen::width() + x]; }

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

} // namespace

void setUp() {
  static bool screen_ready = false;
  if (!screen_ready) screen_ready = m8screen::init();
  m8proto::init();
  m8screen::set_hardware_model(0);
  m8screen::set_font_mode(0);
  m8screen::clear({0, 0, 0});
  m8screen::take_dirty_bands();
  wire.clear();
}

void tearDown() {}

void test_system_info_is_parsed() {
  system_info(0x00, 0);
  flush();
  const m8proto::DeviceInfo &info = m8proto::device_info();
  TEST_ASSERT_TRUE(info.valid);
  TEST_ASSERT_EQUAL_UINT8(0, info.hw_type);
  TEST_ASSERT_EQUAL_UINT8(6, info.fw_major);
  TEST_ASSERT_EQUAL_UINT8(1, info.fw_minor);
  TEST_ASSERT_EQUAL_UINT8(2, info.fw_patch);
  TEST_ASSERT_EQUAL_INT(320, m8screen::width());
  TEST_ASSERT_EQUAL_INT(240, m8screen::height());
}

void test_rect_fills_and_marks_dirty() {
  rect(10, 20, 4, 3, 0xFF, 0x00, 0x00);
  flush();
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0, 0), px(10, 20));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0, 0), px(13, 22));
  TEST_ASSERT_EQUAL_HEX16(0, px(14, 22));
  TEST_ASSERT_EQUAL_HEX16(0, px(10, 23));
  TEST_ASSERT_EQUAL_HEX64(1ULL << (20 / m8screen::kBandRows), m8screen::take_dirty_bands());
}

void test_short_rect_reuses_last_color() {
  rect(0, 0, 1, 1, 0x12, 0x34, 0x56);
  slip({0xFE, 50, 0, 60, 0}); // 5 bytes: position only, 1x1, previous color
  flush();
  TEST_ASSERT_EQUAL_HEX16(rgb565(0x12, 0x34, 0x56), px(50, 60));
}

void test_slip_escapes_are_decoded() {
  // 0xC0 and 0xDB inside the payload must survive escaping.
  rect(1, 1, 1, 1, 0xC0, 0xDB, 0xC0);
  flush(1);
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xC0, 0xDB, 0xC0), px(1, 1));
}

void test_char_draws_glyph_with_background() {
  // 'X' in font 0 (5x7), white on blue, drawn at the M8's (8, 100).
  slip({0xFD, 'X', 8, 0, 100, 0, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF});
  flush();
  const int y = 100 + 3; // font 0 text_offset_y
  int ink = 0, paper = 0;
  for (int dy = 0; dy < 7; dy++) {
    for (int dx = 0; dx < 5; dx++) {
      const uint16_t p = px(8 + dx, y + dy);
      if (p == rgb565(0xFF, 0xFF, 0xFF)) ink++;
      else if (p == rgb565(0, 0, 0xFF)) paper++;
    }
  }
  TEST_ASSERT_EQUAL_INT(35, ink + paper);
  TEST_ASSERT_GREATER_THAN(6, ink);
  TEST_ASSERT_GREATER_THAN(6, paper);
  // 'X' has ink in both top corners.
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0xFF, 0xFF), px(8, y));
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0xFF, 0xFF), px(12, y));
}

void test_waveform_is_right_aligned_and_clamped() {
  m8screen::clear({0, 0, 0});
  // 4 samples; the last exceeds font 0's 24 px scope height and must be clamped.
  slip({0xFC, 0x00, 0xFF, 0x00, 0, 5, 10, 200});
  flush();
  const uint16_t green = rgb565(0, 0xFF, 0);
  TEST_ASSERT_EQUAL_HEX16(green, px(316, 0));
  TEST_ASSERT_EQUAL_HEX16(green, px(317, 5));
  TEST_ASSERT_EQUAL_HEX16(green, px(318, 10));
  TEST_ASSERT_EQUAL_HEX16(green, px(319, 24));
}

void test_bad_packets_are_rejected() {
  slip({0xFE, 1, 2, 3}); // wrong rect length
  slip({0x42, 0x00});    // unknown command
  system_info(0, 0);
  flush();
  TEST_ASSERT_EQUAL_UINT32(2, m8proto::packets_bad());
  TEST_ASSERT_EQUAL_UINT32(1, m8proto::packets_ok());
}

void test_large_font_applies_screen_offset() {
  system_info(0x00, 1); // font_v1_large: screen_offset_y = -40
  rect(0, 50, 1, 1, 0xFF, 0xFF, 0xFF);
  flush();
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFF, 0xFF, 0xFF), px(0, 10));
}

void test_model02_switches_resolution() {
  system_info(0x03, 0);
  flush();
  TEST_ASSERT_EQUAL_INT(480, m8screen::width());
  TEST_ASSERT_EQUAL_INT(320, m8screen::height());
  TEST_ASSERT_TRUE(m8screen::oversized());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_system_info_is_parsed);
  RUN_TEST(test_rect_fills_and_marks_dirty);
  RUN_TEST(test_short_rect_reuses_last_color);
  RUN_TEST(test_slip_escapes_are_decoded);
  RUN_TEST(test_char_draws_glyph_with_background);
  RUN_TEST(test_waveform_is_right_aligned_and_clamped);
  RUN_TEST(test_bad_packets_are_rejected);
  RUN_TEST(test_large_font_applies_screen_offset);
  RUN_TEST(test_model02_switches_resolution);
  return UNITY_END();
}
