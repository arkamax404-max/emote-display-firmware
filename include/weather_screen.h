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
// space its wider figure leaves. A negative figure loses its minus glyph
// and is painted in intense blue (S9): the colour carries the sign. All text is drawn through the same 3x5 glyph
// table and text() helper the clock uses; the icons use the same plot
// discipline with a small row-byte table of their own. Colors leave this
// renderer raw 565; the pixel callback applies the shared brightness
// helper panel::color() exactly like every idle_clock::draw call site, so
// the whole screen dims with the panel setting. No Arduino/WiFi symbols.
namespace weather_screen {

// Geometry fixed by the spec (S1): the four cells sit on the canvas with
// an empty crossing at (32, 32) — S9 removed the two separator lines, so
// nothing is ever drawn there. Each cell still owns a 31x31 content area
// anchored at its origin; the empty column 31 and the empty crossing of
// column 32/row 32 keep every cell clear of its neighbours.
constexpr int CELL = 31;
constexpr int RIGHT_X = 33;  // left edge of the two right-hand cells
constexpr int BOTTOM_Y = 33; // top edge of the two bottom cells

// Row offsets inside a cell (S7): label y+0 (scale 1) with the probability
// right-aligned on that same row (the '%' ends at the cell's own edge,
// column x0+30); maximum y+7 and minimum y+19, both at scale 2. The icon
// (S8/S9) is CENTRED AT DRAW TIME between the end of the wider of the two
// figures actually drawn and the cell's own right edge, over the vertical
// middle rows y0+14..y0+21 — never a fixed column, so it leans neither to
// a figure nor to the edge. The last painted row is y+28.
constexpr int DY_LABEL = 0, DY_MAX = 7, DY_MIN = 19;
constexpr int SCALE_LABEL = 1, SCALE_MAX = 2, SCALE_MIN = 2;
constexpr int ICON_Y = 14;

// Inks (S9): a positive maximum is the white headline; the positive
// minimum is the MUTED ink — blue used to mean "minimum", but now the
// colour must mean the SIGN, so an unambiguous intense blue is reserved
// for negative figures (drawn without a minus glyph). The muted minimum
// still reads as the secondary figure through its lower brightness; the
// minus-glyph drop is the derived consequence the user did not ask for
// explicitly (see the task record).
constexpr uint16_t INK_MAX = idle_clock::WHITE;
constexpr uint16_t INK_MIN = idle_clock::MUTED;
constexpr uint16_t INK_NEG =
    idle_clock::rgb(64, 64, 255); // intense blue, not the clock's sky blue

constexpr uint16_t INK_LABEL = idle_clock::MUTED;
constexpr uint16_t INK_PROB = idle_clock::AMBER;

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

// Truncation rule (T2 decision): a value that would not fit its 31 px cell
// loses WHOLE trailing characters (never a mid-glyph cut) until it fits,
// e.g. a maximum of -100.0 at scale 2 renders as "-100". Combined with the
// per-cell region clamp below, nothing a cell draws can ever overwrite the
// neighbouring cell or touch a divider. Word width for n glyphs at scale s
// is 4*n*s - s (advance 4*s per character, the last glyph is 3*s wide).
namespace detail {
inline size_t fitLength(const char *value, int scale) {
  size_t n = std::strlen(value);
  while (n > 1 && 4 * static_cast<int>(n) * scale - scale > CELL) --n;
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
  // The cell's own viewport: any glyph pixel outside the 31x31 region is
  // dropped, so overflowing text is visibly truncated at the cell edge and
  // never crosses into the neighbour or over a divider.
  auto plot = [&](int x, int y, uint16_t color) {
    if (x >= x0 && x < x0 + CELL && y >= y0 && y < y0 + CELL)
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
  // own right edge — the '%' ends at column x0+30. Zero-padded two digits
  // (100 keeps three); with probability <= 100 the field is at most "100%"
  // (15 px), never truncated.
  std::snprintf(buffer, sizeof buffer, "%02u",
                static_cast<unsigned>(forecast.prob[index]));
  const int digits = static_cast<int>(std::strlen(buffer));
  const int left = x0 + CELL - detail::probabilityWidth(digits);
  idle_clock::text(buffer, left, y0 + DY_LABEL, SCALE_LABEL, INK_PROB, plot);
  const int px = left + 4 * digits;
  for (int row = 0; row < 5; ++row)
    for (int col = 0; col < 3; ++col)
      if (PCT[row] & (4 >> col))
        plot(px + col, y0 + DY_LABEL + row, INK_PROB);
  // S7: both temperatures at scale 2 — the maximum is the primary value.
  // S9: a negative figure is drawn WITHOUT the minus glyph, in intense
  // blue; a positive maximum stays white and the positive minimum is
  // muted, so the colour alone carries the sign at a glance.
  int value = weather::roundedDegrees(weather::displayedTenths(
      forecast.tmax[index], forecast.unit, shown));
  const bool maxNegative = value < 0;
  std::snprintf(buffer, sizeof buffer, "%u",
                static_cast<unsigned>(maxNegative ? -value : value));
  put(buffer, DY_MAX, SCALE_MAX, maxNegative ? INK_NEG : INK_MAX);
  const size_t maxChars = std::strlen(buffer);
  value = weather::roundedDegrees(weather::displayedTenths(
      forecast.tmin[index], forecast.unit, shown));
  const bool minNegative = value < 0;
  std::snprintf(buffer, sizeof buffer, "%u",
                static_cast<unsigned>(minNegative ? -value : value));
  put(buffer, DY_MIN, SCALE_MIN, minNegative ? INK_NEG : INK_MIN);
  const size_t minChars = std::strlen(buffer);
  // S8: the 8x8 icon for the day's WMO code, CENTRED in the free space of
  // its own cell at draw time: its columns are computed from the WIDER of
  // the two figures actually drawn, so it never sits closer to one
  // temperature row than to the other and cannot overlap either. A
  // scale-2 figure of n signless digits is 8n-2 px wide, so with d = the
  // larger drawn digit count:
  //   figureEnd = 8d - 2 (last column of that figure),
  //   leftGap   = (CELL - figureEnd - 7) / 2  (the odd pixel of an odd
  //               gap goes to the LEFT: the icon leans one pixel towards
  //               the cell's own right edge),
  //   iconX     = x0 + figureEnd + leftGap  (== x0 + 11 + 4d).
  // d=1 -> icon at x0+15; d=2 -> icon at x0+19; the two figures can drop
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
    const int iconX = x0 + figureEnd + (CELL - figureEnd - 7) / 2;
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
// themselves, separated by whitespace only.
template<class Pixel>
void draw(const weather::Forecast &forecast, weather::Unit shown,
          const std::tm &calendar, Pixel pixel) {
  if (!forecast.valid) return;
  const int ORIGIN[4][2] = {{0, 0}, {RIGHT_X, 0}, {0, BOTTOM_Y},
                            {RIGHT_X, BOTTOM_Y}};
  for (size_t index = 0; index < 4; ++index)
    drawCell(ORIGIN[index][0], ORIGIN[index][1], index, forecast, shown,
             calendar, pixel);
}

} // namespace weather_screen
