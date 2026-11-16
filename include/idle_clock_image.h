#pragma once
#include "idle_clock.h"

namespace idle_clock {
constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) {
  return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
constexpr uint16_t WHITE = rgb(242,241,220), MUTED = rgb(147,181,160);
constexpr uint16_t BLUE = rgb(85,189,232), GREEN = rgb(184,243,91);
constexpr uint16_t SKIN = rgb(240,208,208), AMBER = rgb(241,189,101);
constexpr uint16_t PATH = rgb(41,75,54);
// Sprite palette, distinct from the clock text/path colors above, sampled
// from pure interior blocks of lemmings_reference.png: saturated green hair,
// royal-blue overalls and pale pink skin (the old SKIN tint was provisional).
constexpr uint16_t HAIR = rgb(0,179,0), OUTFIT = rgb(64,64,224);

// Nine lemming walking poses (8x10 px, all facing right), extracted from the
// reference sheet lemmings_reference.png (362x545 RGBA, 15 px per logical
// pixel, grid origin (2,3), cell-center voting): green mop hair ('H'), pale
// pink face/arms/hands/feet ('S'), royal-blue overalls ('O'). Transparent
// '.' background; dark sheet background removed; poses progress row-major
// like the sheet frames 1..9. Sprite rows sit on 53..62, above path row 63.
constexpr char FRAMES[9][10][9] = {
  {"........", "..HHHH..", "..HHS...", "...SSS..", "...SO...",
   "...SO...", "...SO...", "...OO...", "..SOO...", "...SS..."},
  {"...H.H..", "..HHH...", "..HHS...", "...SSS..", "...SO...",
   "..SOO...", "..SOO.S.", "...OO.S.", "..OO.S..", "..SS...."},
  {"........", "..H.H...", "..HHH...", "...HS...", "...SSS..",
   "..SSO...", "..SOO...", ".SSOOO..", "..OOOO..", ".SS..SS."},
  {"........", "...HH...", "..HHSH..", "..HSSS..", "...SO...",
   "...SO...", "..SOO...", "...OO...", ".SOOOO..", ".S..SS.."},
  {"........", "..HHHH..", "..HHS...", "..HSSS..", "...SO...",
   "...OS...", "...SO...", "...OO...", "..SOO...", "...SS..."},
  {"...H.H..", "..HHH...", "..HHS...", "...SSS..", "...SO...",
   "...OS...", "...OS.S.", "...OO.S.", "..OO.S..", "..SS...."},
  {"........", "..H.H...", "..HHH...", "...HS...", "...SSS..",
   "...OS...", "...OS...", "...OOS..", "..OOOO..", ".SS..SS."},
  {"........", "...HH...", "..HHSH..", "..HSSS..", "...SO...",
   "...SO...", "...OS...", "...OO...", ".SOOOO..", ".S..SS.."},
  {"..HH....", ".HHS....", ".HSSS...", "..SO....", "..OS....",
   "..OOS...", "..OOS...", "..OO....", ".SO.....", "..S....."}
};

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
void draw(const std::tm &calendar, uint64_t animationTick, Pixel pixel) {
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
  // Poses cycle on the half-second animation tick (monotonic, so no
  // minute-boundary reset); two walkers keep the existing 2 px/s speed,
  // now 1 px per 500 ms tick, over an 80-tick (40 s) horizontal wrap. The
  // displayed time still changes only once per calendar second.
  const int frame = int(animationTick % 9);
  for (int walker = 0; walker < 2; ++walker) {
    const int left = int(((animationTick % 80) + walker * 40) % 80) - 8;
    for (int y = 0; y < 10; ++y) for (int x = 0; x < 8; ++x) {
      const char part = FRAMES[frame][y][x];
      if (part == '.') continue;
      const uint16_t color = part == 'H' ? HAIR : part == 'S' ? SKIN : OUTFIT;
      plot(left + x + 1, 53 + y, color);
    }
  }
}
} // namespace idle_clock
