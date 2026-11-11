#pragma once
#include "idle_clock.h"

namespace idle_clock {
constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) {
  return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
constexpr uint16_t WHITE = rgb(242,241,220), MUTED = rgb(147,181,160);
constexpr uint16_t BLUE = rgb(85,189,232), GREEN = rgb(184,243,91);
constexpr uint16_t SKIN = rgb(244,195,140), AMBER = rgb(241,189,101);
constexpr uint16_t PATH = rgb(41,75,54);

// Each row is a three-bit glyph; no platform font or framebuffer allocation.
constexpr uint8_t GLYPHS[12][5] = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7},
  {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2},
  {7,5,7,5,7}, {7,5,7,1,7}, {1,1,2,4,4}, {0,2,0,2,0}
};

template<class Pixel>
void text(const char *value, int x, int y, int scale, uint16_t color, Pixel pixel) {
  for (; *value; ++value, x += 4 * scale) {
    const int glyph = *value >= '0' && *value <= '9' ? *value - '0' : *value == '/' ? 10 : 11;
    for (int row = 0; row < 5; ++row) for (int col = 0; col < 3; ++col) {
      if (!(GLYPHS[glyph][row] & (4 >> col))) continue;
      for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx)
        pixel(x + col * scale + dx, y + row * scale + dy, color);
    }
  }
}

template<class Pixel>
void draw(const std::tm &calendar, uint64_t epochSeconds, Pixel pixel) {
  // Full 64-row canvas at y0; the renderer no longer relies on a panel offset.
  auto plot = [&](int x, int y, uint16_t color) {
    if (x >= 0 && x < 64 && y >= 0 && y < 64) pixel(x, y, color);
  };
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) plot(x, y, 0);
  const Labels value = labels(calendar);
  text(value.date, 12, 1, 1, MUTED, plot);
  text(value.hours, 12, 9, 4, WHITE, plot);
  text(value.minutes, 12, 31, 4, BLUE, plot);
  text(":", 43, 35, 1, AMBER, plot);
  text(value.seconds, 46, 33, 2, AMBER, plot);
  for (int x = 0; x < 64; ++x) plot(x, 63, x % 8 < 3 ? GREEN : PATH);
  const bool odd = epochSeconds % 2;
  const char *rows[] = {" GGG ", "GGSS ", "  SS ", " BBB ", " BBB ", "  B  ",
                        odd ? " W W " : "  WW ", odd ? "W   W" : " W W "};
  for (int walker = 0; walker < 2; ++walker) {
    const int left = int(((epochSeconds % 40) * 2 + walker * 40) % 80) - 8;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 5; ++x) {
      const char part = rows[y][x];
      if (part == ' ') continue;
      const uint16_t color = part == 'G' ? GREEN : part == 'S' ? SKIN : part == 'B' ? BLUE : WHITE;
      plot(left + x + 1, 55 + y, color);
    }
  }
}
} // namespace idle_clock
