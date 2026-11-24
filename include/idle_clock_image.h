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
// Digits, '/' and ':' are the canonical date/time glyphs; the letters, '*',
// '_' and '-' are label-only additions (S5) mirroring the desktop ID_GLYPHS
// table, so the date, time and walkers never move or change bitmap.
constexpr uint8_t GLYPHS[42][5] = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7},
  {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2},
  {7,5,7,5,7}, {7,5,7,1,7}, {1,1,2,4,4}, {0,2,0,2,0},
  // A..Z, 3x5, identical to the desktop label table.
  {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6},
  {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3}, {5,5,7,5,5},
  {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7},
  {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2}, {6,5,6,4,4},
  {2,5,5,7,3}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
  {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5},
  {5,5,2,2,2}, {7,1,2,4,7},
  // '.', '*' and '_'/'-' for labels like *NO SESSION* and '...' truncation.
  {0,0,0,0,2}, {0,5,2,5,0}, {0,0,0,0,7}, {0,0,7,0,0}
};

// Canonical 3x5 lookup for date/time characters; label characters resolve
// through the extended table. Unknown characters draw nothing (blank cells).
inline int glyphIndex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c == '/') return 10;
  if (c == ':') return 11;
  if (c >= 'A' && c <= 'Z') return 12 + (c - 'A');
  if (c == '.') return 38;
  if (c == '*') return 39;
  if (c == '_') return 40;
  if (c == '-') return 41;
  return -1;
}

template<class Pixel>
void text(const char *value, int x, int y, int scale, uint16_t color, Pixel pixel) {
  for (; *value; ++value, x += 4 * scale) {
    const int glyph = glyphIndex(*value);
    if (glyph < 0) continue;
    for (int row = 0; row < 5; ++row) for (int col = 0; col < 3; ++col) {
      if (!(GLYPHS[glyph][row] & (4 >> col))) continue;
      for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx)
        pixel(x + col * scale + dx, y + row * scale + dy, color);
    }
  }
}


// S9 follow-up: an hour of a single digit (only possible in H12, where the
// labels are unpadded) is shifted one cell to the right so it keeps the right
// edge a two-digit hour has: it hugs the colon instead of leaving the gap on
// its right. Two-digit hours, and therefore every H24 render, are unchanged.
inline int hourX(const char *hours, int baseX, int scale) {
  const size_t cells = std::strlen(hours);
  if (cells >= 2) return baseX;
  return baseX + static_cast<int>((2 - cells) * 4 * scale);
}

// Full 64-row canvas at y0; the renderer no longer relies on a panel offset.
// S9: `format` selects the clock display; H24 (the default) renders byte
// for byte what today's renderer renders. In H12 the hours go 12, 1..11
// (from idle_clock::labels) and the AM/PM label is drawn 3x5 at scale 1
// above the seconds block at the seconds' x (46): its rows are 24..28, so
// its baseline row 28 is exactly the scale-4 hour digits' baseline
// (9 + 5*4 - 1). Nothing outside the hour-digit cells and the label cells
// moves: the date band, the minutes, the colon, the seconds, the walkers
// and the path all stay where the H24 render puts them.
template<class Pixel>
void draw(const std::tm &calendar, uint64_t animationTick, Pixel pixel,
          ClockFormat format = ClockFormat::H24) {
  auto plot = [&](int x, int y, uint16_t color) {
    if (x >= 0 && x < 64 && y >= 0 && y < 64) pixel(x, y, color);
  };
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) plot(x, y, 0);
  const Labels value = labels(calendar, format);
  text(value.date, 12, 1, 1, MUTED, plot);
  text(value.hours, hourX(value.hours, 12, 4), 9, 4, WHITE, plot);
  if (format == ClockFormat::H12)
    text(value.meridiem, 46, 24, 1, MUTED, plot);
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

// S5: draw the canonical clock first, then in the odd half of the
// absolute-epoch 5 s alternation (floor(epochMillis / 5000) mod 2 == 1)
// replace the top band rows 1..5 with the session title instead of the
// centered DD/MM/YYYY date. The title is ASCII-uppercased, at most 16 cells
// (17 or more source characters become 13 plus '...'), centered with
// (64 - (cells * 4 - 1)) / 2, 3x5 glyphs at scale 1, muted ink, y=1. An empty
// or absent name always shows the date in both phases. Only rows 1..5 are
// repainted, so every other pixel keeps the canonical content.
template<class Pixel>
void draw(const std::tm &calendar, uint64_t animationTick, uint64_t epochMillis,
          const char *title, Pixel pixel, ClockFormat format = ClockFormat::H24) {
  draw(calendar, animationTick, pixel, format);
  if (!title || !title[0] || (epochMillis / 5000) % 2 == 0) return;
  for (int y = 1; y <= 5; ++y) for (int x = 0; x < 64; ++x) pixel(x, y, 0);
  // ASCII-uppercased copy; multibyte UTF-8 sequences pass through untouched.
  char upper[385];
  size_t in = 0;
  for (; title[in] && in < sizeof(upper) - 1; ++in)
    upper[in] = title[in] >= 'a' && title[in] <= 'z'
                    ? static_cast<char>(title[in] - 'a' + 'A') : title[in];
  upper[in] = '\0';
  int total = 0; // Cells are Unicode scalars, as on the desktop.
  for (const char *p = upper; *p; ++p)
    if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++total;
  if (total == 0) return;
  const int cells = total > 16 ? 16 : total;
  const int left = (64 - (cells * 4 - 1)) / 2;
  const int take = total > 16 ? 13 : total;
  char label[17]; // 13 title cells plus '...' at most; one byte per cell.
  int out = 0, seen = 0;
  for (const char *p = upper; *p && seen < take; ++p) {
    if ((static_cast<unsigned char>(*p) & 0xC0) == 0x80) continue;
    // Unsupported scalars reserve a blank cell exactly like the desktop.
    label[out++] = (static_cast<unsigned char>(*p) & 0x80) ? '\x01' : *p;
    ++seen;
  }
  if (total > 16) { label[out++] = '.'; label[out++] = '.'; label[out++] = '.'; }
  label[out] = '\0';
  text(label, left, 1, 1, MUTED, pixel);
}
} // namespace idle_clock
