#pragma once
#include "weather.h"
#include "idle_clock_image.h"
#include <cstdio>
#include <cstring>
#include <ctime>

// Weather screen renderer (T2): the forecast grid over the whole 64x64
// canvas — four cells (today top left, +1 top right, +2 bottom left, +3
// bottom right) separated by whitespace only (S9 removed the two divider
// lines), each showing its three-letter day label with the precipitation
// probability NN% right-aligned on the very same top row (S7), then the
// maximum at scale 2 and the minimum at scale 2 (same size; the maximum
// is the primary figure), and the S8 8x8 icon for the day's WMO weather
// code beside the temperature block, centred at draw time in the free
// space its wider figure leaves, with every temperature figure coloured
// by its five-band temperature scale (S10) that carries the sign from
// -10 downwards (S11). Nothing is ever drawn in the S12 marginal columns
// 0 and 63 nor in the S13 marginal rows 0 and 63: the content starts one
// pixel inside the canvas on every border. All text is drawn through the
// same 3x5 glyph
// table and text() helper the clock uses; the icons use the same plot
// discipline with a small row-byte table of their own. Colors leave this
// renderer raw 565; the pixel callback applies the shared brightness
// helper panel::color() exactly like every idle_clock::draw call site, so
// the whole screen dims with the panel setting. No Arduino/WiFi symbols.
namespace weather_screen {

// Geometry fixed by the spec (S1), with the S12 margins: the four cells
// sit on the canvas with an empty crossing at (32, 32) — S9 removed the
// two separator lines, so nothing is ever drawn there. S12 leaves the
// marginal columns 0 and 63 dark, symmetrically: the left-hand cells
// begin one column in (LEFT_X = 1), and each cell's useful WIDTH shrinks
// one pixel to 30 px, so a right cell keeps x0 = 33 but ends at column
// 62 — its right-aligned probability moves its edge in by one and the
// right border keeps the same margin as the left. Simply shifting
// everything and clipping the overflow was rejected: it would eat the
// last column of the right-hand cells. S13 does the same VERTICALLY
// ("deja otro px por arriba que abajo tenemos sitio"): row 0 stays dark,
// the cells' useful HEIGHT shrinks one row to 30 rows, and the bottom
// cells end at row 62, so the bottom border keeps the same margin as the
// top — again shift-and-clip was rejected for the same reason. The empty
// columns 31/32 and the empty rows 31/32 still separate the halves.
constexpr int CELL_W = 30;   // useful WIDTH of a cell (S12)
constexpr int CELL_H = 30;   // useful HEIGHT of a cell (S13)
constexpr int LEFT_X = 1;    // x0 of the two left-hand cells (S12)
constexpr int TOP_Y = 1;     // y0 of the two top cells (S13)
constexpr int RIGHT_X = 33;  // left edge of the two right-hand cells
constexpr int BOTTOM_Y = 33; // top edge of the two bottom cells

// Row offsets inside a cell (S7): label y+0 (scale 1) with the probability
// right-aligned on that same row (the '%' ends at the cell's own edge,
// column x0+29 — S12: one column inside the canvas edge); maximum y+7
// and minimum y+19, both at scale 2. The icon
// (S8/S9) is CENTRED AT DRAW TIME between the end of the wider of the two
// figures actually drawn and the cell's own right edge, over the vertical
// middle rows y0+14..y0+21 — never a fixed column, so it leans neither to
// a figure nor to the edge. The last painted row is y+28.
constexpr int DY_LABEL = 0, DY_MAX = 7, DY_MIN = 19;
constexpr int SCALE_LABEL = 1, SCALE_MAX = 2, SCALE_MIN = 2;
constexpr int ICON_Y = 14;

// Inks (S10): the colour encodes the TEMPERATURE, not the role — the role
// (maximum, minimum) is carried by the position inside the cell. The five
// bands are chosen from the displayed value's CELSIUS EQUIVALENT, rounded
// exactly once at the place the display rounds, so the ink and the
// printed figure can never disagree about which side of a threshold they
// are on and a given weather colours identically in °C and °F modes:
//   below 0: intense blue 0x421F   0..14: cyan 0x55FD
//   15..24: green 0x4648          25..37: amber 0xFDE0
//   38 and above: red 0xF800
inline uint16_t inkForDegrees(int celsiusDegrees) {
  if (celsiusDegrees < 0) return idle_clock::rgb(64, 64, 255);   // 0x421F
  if (celsiusDegrees <= 14) return idle_clock::rgb(85, 189, 232); // 0x55FD
  if (celsiusDegrees <= 24) return idle_clock::rgb(64, 200, 64);  // 0x4648
  if (celsiusDegrees <= 37) return idle_clock::rgb(255, 191, 0);  // 0xFDE0
  return idle_clock::rgb(255, 0, 0);                              // 0xF800
}

// S11: a negative figure keeps its minus glyph while the figure stays two
// glyphs (-1..-9); from -10 on the sign is dropped (S9) and the cold ink
// carries it.
inline void figureText(int value, char (&buffer)[12], size_t &chars) {
  if (value < 0) {
    const unsigned magnitude = static_cast<unsigned>(-value);
    if (magnitude < 10)
      std::snprintf(buffer, sizeof buffer, "-%u", magnitude);
    else
      std::snprintf(buffer, sizeof buffer, "%u", magnitude);
  } else {
    std::snprintf(buffer, sizeof buffer, "%u", static_cast<unsigned>(value));
  }
  chars = std::strlen(buffer);
}

constexpr uint16_t INK_LABEL = idle_clock::MUTED;
// S10: the probability leaves amber (it would share the tone of the warm
// band); it takes the same neutral, low-weight muted ink as the day
// labels — present without competing with any temperature colour.
constexpr uint16_t INK_PROB = idle_clock::MUTED;

// The clock glyph table has no percent sign, and the table is shared with
// the clock (its bitmaps must never move), so the one missing glyph is
// defined here and drawn by the same plot discipline: a top-left dot, a
// bottom-right dot and the anti-diagonal between them.
constexpr uint8_t PCT[5] = {5, 1, 2, 4, 5};

// S8: the seven 8x8 icon bitmaps, one byte per row, bit 0x80 the leftmost
// column, in the same style as the clock's string-row pattern tables. Each
// icon reads at panel scale: the sun disc for clear, a sun behind a cloud,
// a two-puff cloud, three staggered bars for fog, a cloud with falling
// strokes for rain, with two grains for snow, and a bolt under the cloud
// for the storm group (storms stay amber — the WMO grouping puts weather
// that demands attention there).
constexpr uint8_t ICONS[int(weather::Icon::Unknown)][8] = {
    // clear: disc with N, S, E, W and four diagonal rays
    {0x18, 0x5A, 0x3C, 0xFF, 0xFF, 0x3C, 0x5A, 0x18},
    // partly cloudy: sun disc upper-left behind a cloud lower-right
    {0x60, 0xF0, 0x6F, 0x3F, 0x3F, 0x1F, 0x00, 0x00},
    // cloudy: cloud, flat base
    {0x00, 0x00, 0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x00},
    // fog: three staggered bars
    {0x00, 0x00, 0x3F, 0x00, 0xFC, 0x00, 0x3F, 0x00},
    // rain: cloud plus three diagonal downward strokes
    {0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x49, 0x92, 0x49},
    // snow: cloud plus two falling grains
    {0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x66, 0x66, 0x00},
    // storm: cloud plus a lightning stroke
    {0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x0C, 0x18, 0x30},
};
constexpr uint16_t ICON_INK[int(weather::Icon::Unknown)] = {
    idle_clock::AMBER, // clear
    idle_clock::AMBER, // partly cloudy
    idle_clock::WHITE, // cloudy
    idle_clock::MUTED, // fog
    idle_clock::BLUE,  // rain
    idle_clock::WHITE, // snow
    idle_clock::AMBER, // storm
};

// Truncation rule (T2 decision): a value that would not fit its 30 px
// cell (S12) loses WHOLE trailing characters (never a mid-glyph cut)
// until it fits, e.g. a maximum of ±1000.0 at scale 2 renders as four
// digit glyphs and fills the narrowed cell exactly. Combined with the
// per-cell region clamp below, nothing a cell draws can ever overwrite
// the neighbouring cell or touch a separator strip. Word width for n
// glyphs at scale s is 4*n*s - s (advance 4*s per character, the last
// glyph is 3*s wide).
namespace detail {
inline size_t fitLength(const char *value, int scale) {
  size_t n = std::strlen(value);
  while (n > 1 && 4 * static_cast<int>(n) * scale - scale > CELL_W) --n;
  return n;
}

// The probability digits plus the percent glyph: the digits are drawn by
// the shared text() helper and the percent sign follows them, occupying its
// own 4*scale cell exactly like any other character would.
inline int probabilityWidth(int digits) {
  return 4 * (digits + 1) - 1;
}
} // namespace detail

template<class Pixel>
void drawCell(int x0, int y0, size_t index, const weather::Forecast &forecast,
              weather::Unit shown, const std::tm &calendar, Pixel pixel) {
  // The cell's own viewport: any glyph pixel outside the 30x30 region is
  // dropped, so overflowing text is visibly truncated at the cell edge and
  // never crosses into the neighbour or over a separator strip.
  auto plot = [&](int x, int y, uint16_t color) {
    if (x >= x0 && x < x0 + CELL_W && y >= y0 && y < y0 + CELL_H)
      pixel(x, y, color);
  };
  char buffer[12];
  auto put = [&](const char *value, int dy, int scale, uint16_t color) {
    char fitted[12];
    const size_t n = detail::fitLength(value, scale);
    const size_t take = n < sizeof fitted ? n : sizeof fitted - 1;
    std::memcpy(fitted, value, take);
    fitted[take] = '\0';
    idle_clock::text(fitted, x0, y0 + dy, scale, color, plot);
  };
  // Three-letter day label: HOY for today, then the weekday from the RTC.
  put(weather::dayLabel(index, calendar.tm_wday), DY_LABEL, SCALE_LABEL,
      INK_LABEL);
  // S7: the probability shares the label row, right-aligned to the cell's
  // own right edge — the '%' ends at column x0+29, one column inside the
  // canvas edge since S12. Zero-padded two digits
  // (100 keeps three); with probability <= 100 the field is at most "100%"
  // (15 px), never truncated.
  std::snprintf(buffer, sizeof buffer, "%02u",
                static_cast<unsigned>(forecast.prob[index]));
  const int digits = static_cast<int>(std::strlen(buffer));
  const int left = x0 + CELL_W - detail::probabilityWidth(digits);
  idle_clock::text(buffer, left, y0 + DY_LABEL, SCALE_LABEL, INK_PROB, plot);
  const int px = left + 4 * digits;
  for (int row = 0; row < 5; ++row)
    for (int col = 0; col < 3; ++col)
      if (PCT[row] & (4 >> col))
        plot(px + col, y0 + DY_LABEL + row, INK_PROB);
  // S7: both temperatures at scale 2 — the maximum is the primary value.
  // S10: the ink is the band of the displayed value's Celsius equivalent
  // (rounded exactly once, like the printed figure); S11: the minus glyph
  // survives only while the figure stays two glyphs.
  size_t maxChars = 0, minChars = 0;
  int value = weather::roundedDegrees(weather::displayedTenths(
      forecast.tmax[index], forecast.unit, shown));
  figureText(value, buffer, maxChars);
  put(buffer, DY_MAX, SCALE_MAX,
      inkForDegrees(weather::roundedDegrees(weather::displayedTenths(
          forecast.tmax[index], forecast.unit, weather::Unit::Celsius))));
  value = weather::roundedDegrees(weather::displayedTenths(
      forecast.tmin[index], forecast.unit, shown));
  figureText(value, buffer, minChars);
  put(buffer, DY_MIN, SCALE_MIN,
      inkForDegrees(weather::roundedDegrees(weather::displayedTenths(
          forecast.tmin[index], forecast.unit, weather::Unit::Celsius))));
  // S8: the 8x8 icon for the day's WMO code, CENTRED in the free space of
  // its own cell at draw time: its columns are computed from the WIDER of
  // the two figures actually drawn, so it never sits closer to one
  // temperature row than to the other and cannot overlap either. A
  // scale-2 figure of n signless digits is 8n-2 px wide, so with d = the
  // larger drawn digit count:
  //   figureEnd = 8d - 2 (last column of that figure),
  //   leftGap   = (CELL_W - figureEnd - 7) / 2  (the odd pixel of an odd
  //               gap goes to the LEFT: the icon leans one pixel towards
  //               the cell's own right edge; with the S12 width of 30 the
  //               two gap halves come out even for every drawable d, so
  //               the tie-break no longer fires but stays documented),
  //   iconX     = x0 + figureEnd + leftGap  (== x0 + 10 + 4d).
  // d=1 -> icon at x0+14; d=2 -> icon at x0+18; the two figures can drop
  // the icon below: at scale 2 any figure wider than two signless digits
  // (any three-digit magnitude) leaves fewer than two clear columns, so
  // in that cell the icon is omitted and the FULL number is kept — a
  // temperature is never truncated to make room for the icon. The
  // unknown group (a code outside every group) has no bitmap and simply
  // shows no icon.
  const weather::Icon icon = weather::iconFor(forecast.code[index]);
  if (maxChars <= 2 && minChars <= 2 && icon != weather::Icon::Unknown) {
    const size_t widest = maxChars > minChars ? maxChars : minChars;
    const int figureEnd = 8 * static_cast<int>(widest) - 2;
    const int iconX = x0 + figureEnd + (CELL_W - figureEnd - 7) / 2;
    const int which = static_cast<int>(icon);
    for (int row = 0; row < 8; ++row)
      for (int col = 0; col < 8; ++col)
        if (ICONS[which][row] & (0x80 >> col))
          plot(iconX + col, y0 + ICON_Y + row, ICON_INK[which]);
  }
}

// The whole screen. A forecast that never parsed (no city, no network yet,
// or a rejected body — the pure layer only publishes valid records) paints
// nothing at all: the caller shows the clock instead until a forecast
// exists, so a board without internet never flickers to an empty grid.
// S9: no separator lines — the four cells keep the whole canvas to
// themselves, separated by whitespace only, and (S12) the marginal
// columns 0 and 63 stay dark, symmetrically.
template<class Pixel>
void draw(const weather::Forecast &forecast, weather::Unit shown,
          const std::tm &calendar, Pixel pixel) {
  if (!forecast.valid) return;
  const int ORIGIN[4][2] = {{LEFT_X, TOP_Y}, {RIGHT_X, TOP_Y},
                            {LEFT_X, BOTTOM_Y}, {RIGHT_X, BOTTOM_Y}};
  for (size_t index = 0; index < 4; ++index)
    drawCell(ORIGIN[index][0], ORIGIN[index][1], index, forecast, shown,
             calendar, pixel);
}

} // namespace weather_screen
