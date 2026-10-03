// Splash screen and on-screen log, drawn into the M8 framebuffer while no M8 is connected.
// ui.cpp presents the framebuffer like any M8 frame.
#include <string.h>

#include "config.h"
#include "display/ui.h"
#include "m8/screen.h"

namespace ui {
namespace {

// Splash and log layout in the M8 area (while no M8 is connected).
constexpr int kSplashTitleY = 22;
constexpr int kLogY = 78;
constexpr int kLogLineH = 12;
constexpr int kLogRows = 12;
constexpr int kLogCols = 52; // 5x7 font, 6 px advance, in 320 px
constexpr int kLogIndent = 2;

} // namespace

void show_splash(const char *line1, const char *line2) {
  const m8screen::Rgb bg = {0, 0, 0};
  m8screen::clear(bg);

  const int big = 1; // m8c font_v1_large
  const char *title = "microm8";
  const int tx = (m8screen::width() - (int)strlen(title) * m8screen::glyph_advance(big)) / 2;
  m8screen::draw_text(tx, kSplashTitleY, title, {255, 255, 255}, bg, big);

  const int adv = m8screen::glyph_advance(0);
  if (line1) {
    const int x = (m8screen::width() - (int)strlen(line1) * adv) / 2;
    m8screen::draw_text(x, kSplashTitleY + 24, line1, {180, 180, 180}, bg, 0);
  }
  if (line2) {
    const int x = (m8screen::width() - (int)strlen(line2) * adv) / 2;
    m8screen::draw_text(x, kSplashTitleY + 38, line2, {110, 110, 110}, bg, 0);
  }
}

void show_log(const logcap::Line *lines, int count) {
  const m8screen::Rgb panel = {12, 12, 16};
  m8screen::fill_rect(0, kLogY - 4, m8screen::width(), M8_SCREEN_H - (kLogY - 4), panel);
  m8screen::draw_text(4, kLogY - 2, "log", {90, 90, 100}, panel, 0);

  // Wrap entries into screen rows (continuations indented), then show the newest rows.
  struct Row {
    const logcap::Line *line;
    int offset;
  };
  constexpr int kMaxRows = logcap::kLines * (logcap::kLineLen / (kLogCols - kLogIndent) + 1);
  static Row rows[kMaxRows];
  int n = 0;
  for (int i = 0; i < count; i++) {
    const int len = (int)strlen(lines[i].text);
    int off = 0;
    while (off < len && n < kMaxRows) {
      rows[n++] = {&lines[i], off};
      off += off == 0 ? kLogCols : kLogCols - kLogIndent;
    }
  }

  const int first = n > kLogRows ? n - kLogRows : 0;
  for (int r = first; r < n; r++) {
    const logcap::Line &line = *rows[r].line;
    m8screen::Rgb fg = {150, 150, 150};
    if (line.level == 'E') fg = {230, 80, 80};
    else if (line.level == 'W') fg = {230, 170, 40};

    const int indent = rows[r].offset ? kLogIndent : 0;
    char text[kLogCols + 1];
    strncpy(text, line.text + rows[r].offset, kLogCols - indent);
    text[kLogCols - indent] = '\0';
    const int x = 4 + indent * m8screen::glyph_advance(0);
    m8screen::draw_text(x, kLogY + 10 + (r - first) * kLogLineH, text, fg, fg, 0);
  }
}

} // namespace ui
