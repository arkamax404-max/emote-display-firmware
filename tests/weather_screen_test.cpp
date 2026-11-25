// Host test rig for the weather screen renderer (T2, S5/S7/S8/S9): a 64x64
// uint16_t canvas painted through a pixel callback exactly like
// idle_clock::draw, compared against an expected image built in this rig
// from the DOCUMENTED geometry (the four cell origins, the field offsets
// and scales, the truncation rule, the icon slot rule, the sign-by-colour
// rule) using the same idle_clock::text glyph engine — so a layout drift
// in weather_screen.h is visible, not just "some pixels exist". Every case
// is one invocation: weather_screen_test <case>; OK + exit 0 or
// "line N: condition" on stderr + exit 1.
//
// The 8x8 icon bitmaps are pinned HERE as raw bytes, and the intense
// blue of the negative figures together with every S15 ink (the violet
// day label and the three probability bands) are pinned HERE as raw
// 565 words — the exact-image match therefore proves the renderer draws
// precisely these bitmaps and precisely these inks, not merely that the
// used constants match themselves.
//
// Cases (case id "dividers" now proves the S9 absence of separators):
//   dividers  NO separator anywhere: row 32 and column 32 are empty in
//             every ink, and no cell content may reach them
//   cells     per-cell isolation: mutating one cell's inputs moves pixels
//             only inside that cell's region; full expected image match
//   truncation the extreme in-range figures (±1000.0) still fit exactly
//             and never reach the spare strip, a neighbour or row/col 32
//   values    two-digit, three-digit, negative and zero temperatures;
//             zero-padded NN% probabilities parked at the cell's right
//             edge exactly on the label row (S7); negative figures in
//             intense blue without a minus glyph, positive minimum muted
//             (S9); the zero probability draws NOTHING and the drawn
//             ink follows the S15 band
//   rain      S15 probability rule at every boundary on both sides:
//             0 draws nothing (no digits, no '%'), 1..30 white,
//             31..60 cyan, 61..100 intense blue
//   icons     S7/S8/S9 layout proofs: both temperatures the same height;
//             the icon slot 4 px further left, never touching the figures;
//             inked only when both figures fit AND the WMO code maps;
//             three-digit magnitudes drop the icon and keep EVERY digit;
//             negative and positive figures of the same magnitude have
//             the same digit widths; unknown codes draw no icon
//   units     the shown unit converts the stored tenths without a refetch
//   labels    HOY for today; the weekday names advance with tm_wday
//   invalid   no valid forecast paints nothing at all (including a body
//             with fewer than four days, rejected by the committed parser)
//   brightness every painted pixel goes through the shared panel::color()
#include "weather.h"
#include "weather_screen.h"
#include "idle_clock_image.h"
#include "display_settings.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "line " << __LINE__ << ": " #condition "\n";                \
      return 1;                                                                \
    }                                                                          \
  } while (false)

namespace {
using Canvas = std::array<uint16_t, 64 * 64>;
// Black (0x0000) is a legitimate pixel colour, so the guard value must be
// outside everything the renderer can emit for the palette it uses.
constexpr uint16_t SENTINEL = 0x5a5a;

// The documented cell origins (S12/S13 margins): today top left, +1 top
// right, +2 bottom left, +3 bottom right. Each cell owns a 30x30 area
// [x0, x0+29] x [y0, y0+29]: columns 0/63 and rows 0/63 stay dark, each
// border keeping a one-pixel margin, so the left cells begin at x=1 (30
// px of useful width) and the right cells end one column in (columns
// 33..62), while the top cells begin one row down and the bottom cells
// end one row up (rows 1..30 and 33..62 — the bottom cell's last painted
// row is 62, with row 63 dark); the empty columns 31/32 and rows 31/32
// keep every cell clear of its neighbours (S9: no line is drawn anywhere).
struct Region { int x0, y0; };
const Region ORIGIN[4] = {{1, 1}, {33, 1}, {1, 33}, {33, 33}};
constexpr int REGION_W = 30; // useful width after the S12 margins
constexpr int REGION_H = 30; // useful height after the S13 margins

Canvas canvas, expected;

void expPixel(int x, int y, uint16_t color) {
  if (x >= 0 && x < 64 && y >= 0 && y < 64) expected[y * 64 + x] = color;
}

// The documented probability rule: zero-padded two digits (100 keeps
// three); the percent glyph follows the digits.
std::string probText(uint8_t probability) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "%02u", unsigned(probability));
  return buffer;
}

// The truncation rule re-derived here (NOT taken from the header under
// test): a word wider than the 30 px cell (S12) loses whole trailing
// characters until it fits; width = 4*chars*scale - scale for the 3x5
// glyph engine.
std::string fitted(const std::string &value, int scale) {
  size_t n = value.size();
  while (n > 1 && 4 * static_cast<int>(n) * scale - scale >
                      weather_screen::CELL_W)
    --n;
  return value.substr(0, n);
}

// The seven S8 icon bitmaps pinned here as raw bytes (one byte per row,
// bit 0x80 = leftmost column), used only to build the expected image.
const uint8_t PINNED_ICONS[7][8] = {
    {0x18, 0x5A, 0x3C, 0xFF, 0xFF, 0x3C, 0x5A, 0x18}, // clear
    {0x60, 0xF0, 0x6F, 0x3F, 0x3F, 0x1F, 0x00, 0x00}, // partly cloudy
    {0x00, 0x00, 0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x00}, // cloudy
    {0x00, 0x00, 0x3F, 0x00, 0xFC, 0x00, 0x3F, 0x00}, // fog
    {0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x49, 0x92, 0x49}, // rain
    {0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x66, 0x66, 0x00}, // snow
    {0x1E, 0x7E, 0xFF, 0xFF, 0x00, 0x0C, 0x18, 0x30}, // storm
};
const uint16_t PINNED_ICON_INK[7] = {idle_clock::AMBER, idle_clock::AMBER,
                                     idle_clock::WHITE, idle_clock::MUTED,
                                     idle_clock::BLUE, idle_clock::WHITE,
                                     idle_clock::AMBER};

// S10: the five temperature-band inks pinned RAW here (the cyan band is
// numerically the clock's sky-blue, reused for the 0..14 range; the rain
// icon keeps it too, in a different context), together with the neutral
// ink of the probability. The BAND is chosen from the displayed value's
// CELSIUS EQUIVALENT (unit-invariant banding: the thresholds always mean
// the same physical temperature whatever unit the board paints, computed
// as a single conversion from the stored tenths, so no double rounding);
// the renderer's inkForDegrees() must agree exactly.
constexpr uint16_t PINNED_COLD = 0x421F; // below 0: intense blue
constexpr uint16_t PINNED_COOL = 0x55FD; // 0..14: cyan
constexpr uint16_t PINNED_MILD = 0x4648; // 15..24: green
constexpr uint16_t PINNED_WARM = 0xFDE0; // 25..37: amber
constexpr uint16_t PINNED_HOT  = 0xF800; // 38 and above: red
constexpr uint16_t PINNED_VIOLET = 0x91FF; // day label: intense violet (S15)
constexpr uint16_t PINNED_PROB_WHITE = 0xF79B;   // 1..30: white (S15)
constexpr uint16_t PINNED_PROB_CYAN = 0x55FD;    // 31..60: cyan (S15)
constexpr uint16_t PINNED_PROB_INTENSE = 0x421F; // 61..100: intense blue

uint16_t pinProbInk(uint8_t probability) {
  if (probability <= 30) return PINNED_PROB_WHITE;
  if (probability <= 60) return PINNED_PROB_CYAN;
  return PINNED_PROB_INTENSE;
}

uint16_t pinInk(int celsiusDegrees) {
  if (celsiusDegrees < 0) return PINNED_COLD;
  if (celsiusDegrees <= 14) return PINNED_COOL;
  if (celsiusDegrees <= 24) return PINNED_MILD;
  if (celsiusDegrees <= 37) return PINNED_WARM;
  return PINNED_HOT;
}

// S10/S11: one drawn temperature figure — its TEXT (the S11 rule keeps
// the minus glyph only while the figure stays two glyphs, i.e. -1..-9;
// from -10 on the sign is dropped and the colour carries it) and its BAND
// ink (from the Celsius-equivalent displayed value).
struct DrawnFigure { std::string text; uint16_t ink; };
DrawnFigure drawFigure(const weather::Forecast &f, int index, bool maximum,
                       weather::Unit shown) {
  const int16_t stored = maximum ? f.tmax[index] : f.tmin[index];
  const int value = weather::roundedDegrees(
      weather::displayedTenths(stored, f.unit, shown));
  const int bandDegrees = weather::roundedDegrees(
      weather::displayedTenths(stored, f.unit, weather::Unit::Celsius));
  DrawnFigure figure;
  figure.ink = pinInk(bandDegrees);
  char buffer[12];
  if (value < 0) {
    const unsigned magnitude = static_cast<unsigned>(-value);
    if (magnitude < 10) // S11: -1..-9 keep their sign
      std::snprintf(buffer, sizeof buffer, "-%u", magnitude);
    else
      std::snprintf(buffer, sizeof buffer, "%u", magnitude);
  } else {
    std::snprintf(buffer, sizeof buffer, "%u", static_cast<unsigned>(value));
  }
  figure.text = buffer;
  return figure;
}

void expectCell(const Region &r, const weather::Forecast &f, int index,
                int wday, weather::Unit shown) {
  // S7 offsets: label y+0 (scale 1) with the probability right-aligned ON
  // THE SAME ROW — its '%' ends exactly at the cell's own edge, column
  // x0+29 (S12: one column inside the canvas edge); maximum y+7 (scale 2);
  // minimum y+19 (scale 2, same size, the maximum reads first). The icon
  // (S8/S9, centred at draw time between
  // the wider figure and the cell's right edge) spans the vertical middle
  // rows (y0+14..y0+21).
  idle_clock::text(weather::dayLabel(static_cast<size_t>(index), wday), r.x0,
                   r.y0 + 0, 1, PINNED_VIOLET, expPixel);
  // S15: not a pixel of probability is drawn at zero — the whole right
  // side of the label row stays empty; drawn, the ink follows the
  // probability's own band.
  if (f.prob[index] != 0) {
    const std::string digits = probText(f.prob[index]);
    const int left = r.x0 + weather_screen::CELL_W -
                     (4 * (static_cast<int>(digits.size()) + 1) - 1);
    const uint16_t probInk = pinProbInk(f.prob[index]);
    idle_clock::text(digits.c_str(), left, r.y0 + 0, 1, probInk, expPixel);
    const int px = left + 4 * static_cast<int>(digits.size());
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3; ++col)
        if (weather_screen::PCT[row] & (4 >> col))
          expPixel(px + col, r.y0 + 0 + row, probInk);
  }

  // S10: the band ink encodes the temperature; S11: the minus glyph
  // survives only while the figure stays two glyphs.
  const DrawnFigure maximum = drawFigure(f, index, true, shown);
  idle_clock::text(fitted(maximum.text, 2).c_str(), r.x0, r.y0 + 7, 2,
                   maximum.ink, expPixel);
  const DrawnFigure minimum = drawFigure(f, index, false, shown);
  idle_clock::text(fitted(minimum.text, 2).c_str(), r.x0, r.y0 + 19, 2,
                   minimum.ink, expPixel);

  // S8 (S9 centring, drawn width): the icon is drawn only when BOTH drawn
  // figures stay within two glyphs (S11: a drawn sign makes a single digit
  // two) and the day's WMO code belongs to a group; the unknown state has
  // no bitmap. Its 8 px are CENTRED in the free space between the end of
  // the WIDER drawn figure and the cell's own right edge: figureW =
  // 8*chars - 2, leftGap = (CELL - figureW - 7)/2 (the odd pixel of an odd
  // gap goes to the LEFT).
  const weather::Icon icon = weather::iconFor(f.code[index]);
  if (maximum.text.size() <= 2 && minimum.text.size() <= 2 &&
      icon != weather::Icon::Unknown) {
    const int widest = static_cast<int>(
        maximum.text.size() > minimum.text.size() ? maximum.text.size()
                                                  : minimum.text.size());
    const int figureW = 8 * widest - 2;
    const int iconStart =
        r.x0 + figureW + (weather_screen::CELL_W - figureW - 7) / 2;
    const int which = static_cast<int>(icon);
    for (int row = 0; row < 8; ++row)
      for (int col = 0; col < 8; ++col)
        if (PINNED_ICONS[which][row] & (0x80 >> col))
          expPixel(iconStart + col, r.y0 + 14 + row, PINNED_ICON_INK[which]);
  }
}

Canvas expectDraw(const weather::Forecast &f, weather::Unit shown,
                  const std::tm &calendar) {
  expected.fill(SENTINEL);
  if (!f.valid) return expected;
  // S9: no separator lines at all — the four cells sit on the empty
  // canvas, separated by whitespace only.
  for (int i = 0; i < 4; ++i)
    expectCell(ORIGIN[i], f, i, calendar.tm_wday, shown);
  return expected;
}

void realize(const weather::Forecast &f, weather::Unit shown,
             const std::tm &calendar) {
  canvas.fill(SENTINEL);
  weather_screen::draw(f, shown, calendar,
                       [](int x, int y, uint16_t color) {
                         if (x >= 0 && x < 64 && y >= 0 && y < 64)
                           canvas[y * 64 + x] = color;
                       });
}

// Band-presence probe: at least one pixel of `color` inside the rows
// [y0+dy, y0+dy+rowspan) of the quadrant's columns. The exact-image match
// pins the glyph geometry; this proves a field is present in its band
// without re-deriving per-glyph pixel offsets by hand.
bool bandHas(const Canvas &c, const Region &r, int dy, int rowspan,
             uint16_t color) {
  for (int y = r.y0 + dy; y < r.y0 + dy + rowspan; ++y)
    for (int x = r.x0; x < r.x0 + REGION_W; ++x)
      if (c[y * 64 + x] == color) return true;
  return false;
}

// S9 icon slot: ink presence inside columns [x0+from..x0+to], rows
// y0+14..y0+21. Presence probes use the whole slot 19..26; absence probes
// may need to start past column 21 when a three-glyph figure (which the
// drop rule keeps in full) reaches x0+21 and its band rows overlap the
// slot rows — that ink is the FIGURE, not an icon.
bool slotHasInk(const Canvas &c, int index, int from = 19, int to = 26) {
  const Region &r = ORIGIN[index];
  for (int y = r.y0 + 14; y < r.y0 + 22; ++y)
    for (int x = r.x0 + from; x <= r.x0 + to; ++x)
      if (c[y * 64 + x] != SENTINEL) return true;
  return false;
}

// The rightmost painted column of ANY non-sentinel pixel within one row band of
// the cell (used for digit-width comparisons).
int rightmostInkInBand(const Canvas &c, const Region &r, int dy, int rowspan) {
  int rightmost = -1;
  for (int y = r.y0 + dy; y < r.y0 + dy + rowspan; ++y)
    for (int x = r.x0; x < r.x0 + REGION_W; ++x)
      if (c[y * 64 + x] != SENTINEL) rightmost = x;
  return rightmost;
}

weather::Forecast makeForecast(weather::Unit unit, const int16_t tmax[],
                               const int16_t tmin[], const uint8_t prob[],
                               const uint8_t code[]) {
  weather::Forecast f;
  f.valid = true;
  f.unit = unit;
  std::memcpy(f.tmax, tmax, sizeof f.tmax);
  std::memcpy(f.tmin, tmin, sizeof f.tmin);
  std::memcpy(f.prob, prob, sizeof f.prob);
  std::memcpy(f.code, code, sizeof f.code);
  return f;
}

std::tm makeCalendar(int wday) {
  std::tm calendar = {};
  calendar.tm_year = 126;
  calendar.tm_mon = 1;
  calendar.tm_mday = 5 + wday; // only tm_wday drives the labels
  calendar.tm_wday = wday;
  return calendar;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];

  if (test == "dividers" || test == "cells") {
    // Every field distinct per cell, all inside truncation-free widths.
    const int16_t tmax[4] = {123, 111, 102, 98};
    const int16_t tmin[4] = {31, -25, 41, 12};
    const uint8_t prob[4] = {10, 45, 80, 5};
    const uint8_t code[4] = {0, 1, 3, 63};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(4); // JUE today
    realize(forecast, weather::Unit::Celsius, calendar);
    CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
    // S9: NO separator anywhere — row 32 and column 32 are empty in every
    // ink, and so are the spare column 31 and row 31 strips.
    for (int i = 0; i < 64; ++i) {
      CHECK(canvas[32 * 64 + i] == SENTINEL);
      CHECK(canvas[i * 64 + 32] == SENTINEL);
      CHECK(canvas[i * 64 + 31] == SENTINEL);
      CHECK(canvas[31 * 64 + i] == SENTINEL);
      CHECK(canvas[i * 64 + 0] == SENTINEL);  // S12: nothing in column 0
      CHECK(canvas[i * 64 + 63] == SENTINEL); // S12: equal right margin
      CHECK(canvas[0 * 64 + i] == SENTINEL);  // S13: nothing in row 0
      CHECK(canvas[63 * 64 + i] == SENTINEL); // S13: equal bottom margin
    }
    if (test == "cells") {
      // Mutating ONE cell's inputs may move pixels only inside that cell's
      // region — no bleed into the neighbour across the empty strip.
      const Canvas base = canvas;
      for (int i = 0; i < 4; ++i) {
        weather::Forecast mutated = forecast;
        mutated.tmax[i] = static_cast<int16_t>(mutated.tmax[i] + 701);
        mutated.tmin[i] = static_cast<int16_t>(mutated.tmin[i] - 235);
        mutated.prob[i] = static_cast<uint8_t>((mutated.prob[i] + 26) % 101);
        mutated.code[i] = 80; // a storm code: the icon drops within the cell
        realize(mutated, weather::Unit::Celsius, calendar);
        CHECK(canvas == expectDraw(mutated, weather::Unit::Celsius, calendar));
        int differences = 0;
        bool outside = false;
        for (int p = 0; p < 64 * 64; ++p)
          if (canvas[p] != base[p]) {
            ++differences;
            const int x = p % 64, y = p / 64;
            if (x < ORIGIN[i].x0 || x >= ORIGIN[i].x0 + REGION_W ||
                y < ORIGIN[i].y0 || y >= ORIGIN[i].y0 + REGION_H)
              outside = true;
          }
        CHECK(differences > 0);
        CHECK(!outside);
      }
    }
  } else if (test == "truncation") {
    // S9 drops the sign, so an in-range extreme never truncates: ±1000.0
    // renders as four digit glyphs (30 px at scale 2) and fits exactly.
    // Cell 0 maximum -1000.0 (intense blue), cell 1 maximum +1000.0
    // (white), cell 2 minimum -1000.0 (intense blue) — all four glyphs
    // kept, nothing beyond the cell.
    const int16_t tmax[4] = {-10000, 10000, 123, 98};
    const int16_t tmin[4] = {31, -25, -10000, 12};
    const uint8_t prob[4] = {10, 45, 80, 5};
    const uint8_t code[4] = {0, 3, 45, 63};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(0); // DOM today
    realize(forecast, weather::Unit::Celsius, calendar);
    CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
    // Explicit proof that the extreme cells stay inside their region, S12
    // edition: the 30 px four-glyph figures now fill the narrowed cell
    // EXACTLY — the maximum bands of cells 0 and 1 end at their own last
    // column x0+29 (canvas columns 30 and 62), so every digit stays whole
    // inside the cell it belongs to; the separation columns 31/32 stay
    // empty and — with the dividers gone — even column 32 and row 32
    // carry nothing at all. The S12 marginal columns 0 and 63 stay dark
    // in every row.
    CHECK(rightmostInkInBand(canvas, ORIGIN[0], 7, 10) == ORIGIN[0].x0 + 29);
    CHECK(rightmostInkInBand(canvas, ORIGIN[1], 7, 10) == ORIGIN[1].x0 + 29);
    CHECK(rightmostInkInBand(canvas, ORIGIN[2], 19, 10) == ORIGIN[2].x0 + 29);
    for (int y = 7; y <= 16; ++y) {
      CHECK(canvas[y * 64 + 31] == SENTINEL); // separation column empty
      CHECK(canvas[y * 64 + 32] == SENTINEL); // (no separator line either)
    }
    for (int i = 0; i < 64; ++i) {
      CHECK(canvas[i * 64 + 0] == SENTINEL);  // S12: nothing in column 0
      CHECK(canvas[i * 64 + 63] == SENTINEL); // S12: equal right margin
      CHECK(canvas[0 * 64 + i] == SENTINEL);  // S13: nothing in row 0
      CHECK(canvas[63 * 64 + i] == SENTINEL); // S13: equal bottom margin
      CHECK(canvas[i * 64 + 32] == SENTINEL);
      CHECK(canvas[31 * 64 + i] == SENTINEL);
      CHECK(canvas[i * 64 + 31] == SENTINEL);
    }
    // The bottom cell's last painted row (33+29 = 62) leaves row 63 dark
    // with the SAME margin the top margin gives row 0 — the extended
    // extremes above are the general proof per row/column.
    CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_COLD)); // -1000 °C
    CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_HOT));  // +1000 °C
    CHECK(bandHas(canvas, ORIGIN[2], 19, 10, PINNED_COLD));
  } else if (test == "bands") {
    // S10: every band boundary, both sides of every threshold, on the
    // ROUNDED displayed Celsius value; exactly zero is COOL, not COLD.
    const uint8_t prob[4] = {10, 45, 80, 5};
    const uint8_t zeroCode[4] = {0, 0, 0, 0};
    const std::tm calendar = makeCalendar(4);
    {
      const int16_t tmax[4] = {-50, 0, 140, 150};
      const int16_t tmin[4] = {240, 250, 370, 380};
      const weather::Forecast forecast =
          makeForecast(weather::Unit::Celsius, tmax, tmin, prob, zeroCode);
      realize(forecast, weather::Unit::Celsius, calendar);
      CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
      CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_COLD));
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_COOL));
      CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_COOL));
      CHECK(bandHas(canvas, ORIGIN[3], 7, 10, PINNED_MILD));
      CHECK(bandHas(canvas, ORIGIN[0], 19, 10, PINNED_MILD));
      CHECK(bandHas(canvas, ORIGIN[1], 19, 10, PINNED_WARM));
      CHECK(bandHas(canvas, ORIGIN[2], 19, 10, PINNED_WARM));
      CHECK(bandHas(canvas, ORIGIN[3], 19, 10, PINNED_HOT));
      for (int i = 0; i < 4; ++i)
        CHECK(!bandHas(canvas, ORIGIN[i], 7, 10, idle_clock::WHITE));
    }
    // Render B: the other side of every threshold, both ways, plus the
    // single-rounding pin: -0.4 °C rounds to the displayed 0 and must be
    // COOL (the band compares the ROUNDED value, never the raw tenths),
    // and 24.5 °C rounds to 25 -> WARM while 24.4 stays MILD.
    {
      const int16_t tmax[4] = {-5, -4, 244, 245};
      const int16_t tmin[4] = {145, 145, 145, 145}; // 14.5 °C rounds to 15
      const weather::Forecast forecast =
          makeForecast(weather::Unit::Celsius, tmax, tmin, prob, zeroCode);
      realize(forecast, weather::Unit::Celsius, calendar);
      CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
      CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_COLD)); // -1°C "-1"
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_COOL)); // 0°C "0"
      CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_MILD)); // 24
      CHECK(bandHas(canvas, ORIGIN[3], 7, 10, PINNED_WARM)); // 25
      for (int i = 0; i < 4; ++i)
        CHECK(bandHas(canvas, ORIGIN[i], 19, 10, PINNED_MILD)); // 15
    }
    // Render C (the pinned unit rule): a Fahrenheit board colours by the
    // CELSIUS EQUIVALENT — 14.4 °C prints "58" in Fahrenheit (a naive
    // Fahrenheit reading would fall near the warm band) but stays COOL;
    // 15 °C prints "59" and is MILD; 37 °C prints "99" WARM; 38 °C prints
    // "100" HOT. The same weather must show the same colour in °C and °F.
    {
      const int16_t tmax[4] = {144, 150, 370, 380};
      const int16_t tmin[4] = {-50, -50, -50, -50}; // -5 °C in every cell
      const weather::Forecast celsiusStored =
          makeForecast(weather::Unit::Celsius, tmax, tmin, prob, zeroCode);
      realize(celsiusStored, weather::Unit::Fahrenheit, calendar);
      CHECK(canvas ==
            expectDraw(celsiusStored, weather::Unit::Fahrenheit, calendar));
      CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_COOL)); // prints 58
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_MILD)); // prints 59
      CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_WARM)); // prints 99
      CHECK(bandHas(canvas, ORIGIN[3], 7, 10, PINNED_HOT));  // prints 100
      for (int i = 0; i < 4; ++i)
        CHECK(bandHas(canvas, ORIGIN[i], 19, 10, PINNED_COLD));
      // A Fahrenheit-STORED record shown in Celsius bands from its
      // Celsius equivalent too: 59 °F = 15 °C -> MILD (the exact-image
      // match pins it; no white, no warm ink in the cell).
      const int16_t ftmax[4] = {590, 590, 590, 590};
      const weather::Forecast fahrenheitStored =
          makeForecast(weather::Unit::Fahrenheit, ftmax, tmin, prob,
                       zeroCode);
      realize(fahrenheitStored, weather::Unit::Celsius, calendar);
      CHECK(canvas ==
            expectDraw(fahrenheitStored, weather::Unit::Celsius, calendar));
      for (int i = 0; i < 4; ++i)
        CHECK(bandHas(canvas, ORIGIN[i], 7, 10, PINNED_MILD));
    }
    // S11 in one image: -1..-9 print their dash, -10 and beyond do not;
    // both keep the cold ink and the icon centred against the DRAWN
    // width ("-5" and "10" both occupy two glyphs -> icon at 19; a
    // bare "5" would centre at 15).
    {
      const int16_t tmax[4] = {-50, -100, 49, -250};
      const int16_t tmin[4] = {120, 120, 120, 120}; // 12 °C MILD control
      const weather::Forecast forecast =
          makeForecast(weather::Unit::Celsius, tmax, tmin, prob, zeroCode);
      realize(forecast, weather::Unit::Celsius, calendar);
      CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
      // "-5" (c0): dash at scaled rows 4..5 of the first glyph cell.
      CHECK(canvas[(ORIGIN[0].y0 + 7 + 4) * 64 + ORIGIN[0].x0] ==
            PINNED_COLD);
      for (int dy = 0; dy < 4; ++dy)
        CHECK(canvas[(ORIGIN[0].y0 + 7 + dy) * 64 + ORIGIN[0].x0] ==
              SENTINEL);
      // "10" (c1): signless, cold ink, AND two glyphs wide — the drawn
      // width is the robust sign discriminator (a pixel probe would depend
      // on the digit's own glyph shape, which may touch the dash row).
      CHECK(rightmostInkInBand(canvas, ORIGIN[1], 7, 7) ==
            ORIGIN[1].x0 + 13);
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_COLD));
      // "5" (c2): ONE glyph, cool ink (5 °C sits in the 0..14 band) —
      // a signed "-5" would span 13 columns.
      CHECK(rightmostInkInBand(canvas, ORIGIN[2], 7, 7) ==
            ORIGIN[2].x0 + 5);
      CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_COOL));
      // "25" (c3): two-digit magnitude -> signless, cold ink.
      CHECK(rightmostInkInBand(canvas, ORIGIN[3], 7, 7) ==
            ORIGIN[3].x0 + 13);
      CHECK(bandHas(canvas, ORIGIN[3], 7, 10, PINNED_COLD));
      // Drawn-width icon centring: c0 "-5" (2 glyphs) and c1 "10"
      // (2 glyphs) both centre the icon at 18 (S12 width); a neighbouring
      // one-glyph figure would centre at 14 (proven in the icons case).
      CHECK(slotHasInk(canvas, 0, 18, 25));
      CHECK(slotHasInk(canvas, 1, 18, 25));
      for (int y = ORIGIN[0].y0 + 14; y < ORIGIN[0].y0 + 22; ++y)
        for (int x = ORIGIN[0].x0 + 14; x <= ORIGIN[0].x0 + 17; ++x)
          CHECK(canvas[y * 64 + x] == SENTINEL); // gap left of the icon
    }
  } else if (test == "values") {
    // Two-digit, three-digit, negative and zero temperatures; the S15
    // probability rule itself: 0 draws NOTHING (cell 0), one digit (07%)
    // and three digits (100%). The negative
    // minimum (-40) is signless and COLD; the negative maximum (-5) is
    // SIGNED per S11, in the cold ink; the band ink is the value itself.
    const int16_t tmax[4] = {253, 1000, -52, 0};
    const int16_t tmin[4] = {0, -1, -400, 400};
    const uint8_t prob[4] = {0, 7, 100, 55};
    const uint8_t code[4] = {0, 2, 45, 61};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(2); // MAR today
    realize(forecast, weather::Unit::Celsius, calendar);
    CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
    // S10 band probes for every cell (the exact-image match pins glyph
    // pixels): tmax {25.3→25 WARM, 100°C HOT, -5°C COLD signed, 0°C COOL};
    // tmin {0 COOL, -0.1 rounds to 0 → COOL not COLD, -40 COLD, 40 HOT}.
    CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_WARM));
    CHECK(bandHas(canvas, ORIGIN[0], 19, 10, PINNED_COOL));
    CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_HOT));
    CHECK(bandHas(canvas, ORIGIN[1], 19, 10, PINNED_COOL));
    CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_COLD));
    CHECK(bandHas(canvas, ORIGIN[2], 19, 10, PINNED_COLD));
    CHECK(bandHas(canvas, ORIGIN[3], 7, 10, PINNED_COOL));
    CHECK(bandHas(canvas, ORIGIN[3], 19, 10, PINNED_HOT));
    // The label row: violet day labels (S15, the intense violet 0x91FF
    // pinned raw above); the ZERO probability draws NOTHING (cell 0: the
    // whole right side of the label row, past the 11 px "MAR" label, is
    // empty), the other three right-aligned on the cell's edge in the
    // probability's own band ink (7 -> white, 100 -> intense blue,
    // 55 -> cyan) — including its "no digits, no '%'" absence
    // (tmax/tmin probes below already scanned no amber here).
    for (int i = 0; i < 4; ++i) {
      CHECK(bandHas(canvas, ORIGIN[i], 0, 5, PINNED_VIOLET));
      for (int y = ORIGIN[i].y0 + 22; y < ORIGIN[i].y0 + REGION_H; ++y)
        for (int x = ORIGIN[i].x0; x < ORIGIN[i].x0 + REGION_W; ++x)
          CHECK(canvas[y * 64 + x] != idle_clock::AMBER);
    }
    CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_WHITE));
    CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_CYAN));
    CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_INTENSE));
    CHECK(rightmostInkInBand(canvas, ORIGIN[0], 0, 5) == ORIGIN[0].x0 + 9); // "HOY" ends at x0+9 (the Y glyph never paints its third col)
    CHECK(bandHas(canvas, ORIGIN[1], 0, 5, PINNED_PROB_WHITE));
    CHECK(rightmostInkInBand(canvas, ORIGIN[1], 0, 5) == ORIGIN[1].x0 + 29);
    CHECK(bandHas(canvas, ORIGIN[2], 0, 5, PINNED_PROB_INTENSE));
    CHECK(bandHas(canvas, ORIGIN[3], 0, 5, PINNED_PROB_CYAN));
    // S11: cell 2's -5 prints its sign — the dash glyph occupies scaled
    // rows y0+7+4..y0+7+5 at columns x0..x0+5, and nothing sits above it
    // in the first glyph cell (a digit would start at the top).
    CHECK(canvas[(ORIGIN[2].y0 + 7 + 4) * 64 + ORIGIN[2].x0] == PINNED_COLD);
    for (int dy = 0; dy < 4; ++dy)
      CHECK(canvas[(ORIGIN[2].y0 + 7 + dy) * 64 + ORIGIN[2].x0] == SENTINEL);
    // The signed figure is two glyphs wide: "-5" ends exactly where a
    // two-digit figure would (the dash costs one glyph cell while it is
    // drawn), keeping the icon centring well-defined. Scanned above the
    // icon rows so the fog icon's own ink cannot masquerade as a digit.
    CHECK(rightmostInkInBand(canvas, ORIGIN[2], 7, 7) ==
          ORIGIN[2].x0 + 13);
  } else if (test == "rain") {
    // S15 probability rule pinned at every boundary on BOTH sides of
    // every threshold: {0, 1, 30, 31} in the first render and {60, 61,
    // 100, 0} in the second — zero never draws a pixel (no digits, no
    // '%'), 1..30 is white, 31..60 cyan and 61..100 intense blue; the
    // label row carries the violet label and nothing else, so the band
    // probes below are unambiguous. The exact-image match pins the
    // geometry, the right-edge alignment and the hiding at zero.
    const std::tm calendar = makeCalendar(4); // JUE today
    auto renderRain = [&](const uint8_t *prob) {
      const int16_t tmax[4] = {253, 49, 120, 300};
      const int16_t tmin[4] = {120, 120, 120, 120};
      const uint8_t code[4] = {0, 0, 0, 0};
      const weather::Forecast forecast =
          makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
      realize(forecast, weather::Unit::Celsius, calendar);
      if (!(canvas ==
            expectDraw(forecast, weather::Unit::Celsius, calendar))) {
        std::cerr << "line 0: exact image mismatch\n";
        return true;
      }
      return false;
    };
    { // Boundary render A: hidden at 0, white at 1 AND at 30, cyan from
      // 31; the white band also rejects cyan, the 31 rejects white.
      const uint8_t prob[4] = {0, 1, 30, 31};
      CHECK(!renderRain(prob));
      CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_WHITE));
      CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_CYAN));
      CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_INTENSE));
      CHECK(rightmostInkInBand(canvas, ORIGIN[0], 0, 5) == ORIGIN[0].x0 + 10);
      CHECK(bandHas(canvas, ORIGIN[1], 0, 5, PINNED_PROB_WHITE));
      CHECK(!bandHas(canvas, ORIGIN[1], 0, 5, PINNED_PROB_CYAN));
      CHECK(rightmostInkInBand(canvas, ORIGIN[1], 0, 5) == ORIGIN[1].x0 + 29);
      CHECK(bandHas(canvas, ORIGIN[2], 0, 5, PINNED_PROB_WHITE));
      CHECK(!bandHas(canvas, ORIGIN[2], 0, 5, PINNED_PROB_CYAN));
      CHECK(bandHas(canvas, ORIGIN[3], 0, 5, PINNED_PROB_CYAN));
      CHECK(!bandHas(canvas, ORIGIN[3], 0, 5, PINNED_PROB_WHITE));
    }
    { // Boundary render B: cyan up to 60, intense blue from 61 (rejecting
      // cyan) and at 100; zero hidden again, in a different cell.
      const uint8_t prob[4] = {60, 61, 100, 0};
      CHECK(!renderRain(prob));
      CHECK(bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_CYAN));
      CHECK(!bandHas(canvas, ORIGIN[0], 0, 5, PINNED_PROB_INTENSE));
      CHECK(bandHas(canvas, ORIGIN[1], 0, 5, PINNED_PROB_INTENSE));
      CHECK(!bandHas(canvas, ORIGIN[1], 0, 5, PINNED_PROB_CYAN));
      CHECK(bandHas(canvas, ORIGIN[2], 0, 5, PINNED_PROB_INTENSE));
      CHECK(rightmostInkInBand(canvas, ORIGIN[2], 0, 5) == ORIGIN[2].x0 + 29);
      CHECK(!bandHas(canvas, ORIGIN[3], 0, 5, PINNED_PROB_INTENSE));
      CHECK(!bandHas(canvas, ORIGIN[3], 0, 5, PINNED_PROB_CYAN));
      CHECK(!bandHas(canvas, ORIGIN[3], 0, 5, PINNED_PROB_WHITE));
      CHECK(rightmostInkInBand(canvas, ORIGIN[3], 0, 5) == ORIGIN[3].x0 + 10);
    }
  } else if (test == "icons") {
    // S7/S8/S9 layout proofs, one image per rule.
    const uint8_t prob[4] = {10, 45, 80, 5};
    const std::tm calendar = makeCalendar(4); // JUE today
    auto render = [&](const int16_t *tmax, const int16_t *tmin,
                      const uint8_t *code) {
      const weather::Forecast forecast =
          makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
      realize(forecast, weather::Unit::Celsius, calendar);
      if (!(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar))) {
        std::cerr << "line 0: exact image mismatch\n";
        return true;
      }
      return false;
    };
    { // (a) Centring: the icon sits in the middle of the free space of its
      // own cell, computed from the WIDER figure, not at a fixed column.
      // Cells: c0 (2,2) digits, c1 (1,1), c2 (2,1), c3 (1,2) — the mixed
      // cells align the icon against the wider figure.
      const int16_t tmax[4] = {253, 49, 253, 49};
      const int16_t tmin[4] = {120, 12, 9, 120};
      const uint8_t code[4] = {0, 63, 3, 95};
      CHECK(!render(tmax, tmin, code));
      // Expected icon start offset from the cell origin: figureW = 8d-2,
      // leftGap = (CELL_W - figureW - 7)/2, icon = figureW + leftGap.
      // d=2 -> 18, d=1 -> 14 (S12 width). A one-digit cell (c1) and a
      // two-digit cell (c0) in the same slot geometry therefore differ,
      // and the mixed cells (c2, c3) follow the WIDER figure.
      const int expectedStart[4] = {18, 14, 18, 18};
      const int figureW[4] = {14, 6, 14, 14};
      for (int i = 0; i < 4; ++i) {
        const Region &r = ORIGIN[i];
        const int start = r.x0 + expectedStart[i];
        // The icon is present exactly in its computed columns.
        bool present = false;
        for (int y = r.y0 + 14; y < r.y0 + 22; ++y)
          for (int x = start; x <= start + 7; ++x)
            if (canvas[y * 64 + x] != SENTINEL) present = true;
        CHECK(present);
        // The gaps on both sides of the icon: columns between the end of
        // the wider figure and the icon start (left), and between the
        // icon's last column and the cell's last column (right, column
        // column x0+29 included via <= 29). Left gap must equal the right gap,
        // allowing exactly the one odd pixel — which goes to the LEFT.
        const int leftGap = expectedStart[i] - figureW[i];
        const int rightGap = 29 - (expectedStart[i] + 7);
        CHECK(leftGap == rightGap || leftGap == rightGap + 1);
        CHECK(leftGap > 0); // the icon never touches a figure
        for (int y = r.y0 + 14; y < r.y0 + 22; ++y) {
          for (int x = r.x0 + figureW[i]; x < start; ++x)
            CHECK(canvas[y * 64 + x] == SENTINEL); // left gap empty
          for (int x = start + 8; x <= r.x0 + 29; ++x)
            CHECK(canvas[y * 64 + x] == SENTINEL); // right gap empty
        }
        // The icon never reaches the cell's last column (x0+29 since
        // S12; all four cells have narrow figures here).
        CHECK(expectedStart[i] + 7 <= 28);
      }
      // Sanity: same slot geometry, different widths, different starts.
      CHECK(expectedStart[0] != expectedStart[1]);
      // The probabilities (hidden at zero, otherwise inked by their S15
      // band — white, cyan or intense blue) still end at the
      // cell's own edge on row 0 — one column inside the canvas edge (S12
      // moved the right cells' edge from column 63 to 62).
      for (int i = 0; i < 4; ++i)
        CHECK(rightmostInkInBand(canvas, ORIGIN[i], 0, 5) ==
              ORIGIN[i].x0 + 29);
    }
    { // (b) The three-glyph drop rule still holds, and an unknown WMO
      // code still draws no icon even when both figures fit: c0 drops for
      // its three-digit MAXIMUM, c1 for its three-digit MINIMUM (every
      // digit kept), c2 carries the unknown code 44, c3 is the known-code
      // control whose icon sits at its width-computed columns.
      const int16_t tmax[4] = {1000, 250, 250, 98};
      const int16_t tmin[4] = {120, 1000, 120, 12};
      const uint8_t code[4] = {0, 2, 44, 0};
      CHECK(!render(tmax, tmin, code));
      CHECK(!slotHasInk(canvas, 0, 22, 29)); // dropped, three-digit max
      CHECK(rightmostInkInBand(canvas, ORIGIN[0], 7, 10) ==
            ORIGIN[0].x0 + 21); // "100" kept whole
      CHECK(!slotHasInk(canvas, 1, 22, 29)); // dropped, three-digit min
      CHECK(rightmostInkInBand(canvas, ORIGIN[1], 19, 10) ==
            ORIGIN[1].x0 + 21); // "100" kept whole
      CHECK(!slotHasInk(canvas, 2, 19, 26)); // unknown code: no icon
      CHECK(slotHasInk(canvas, 3, 19, 26));  // control: icon present
    }
    { // (c) S9 sign rule under centring: "-5" and "5" render with the SAME
      // digit width, the negative maximum is intense blue and the positive
      // one white; "-100" drops the icon exactly like "100", keeping every
      // digit; "-5" beside a two-figure minimum keeps the icon, aligned
      // against the wider (two-digit) minimum.
      const int16_t tmax[4] = {250, -250, -1000, -52};
      const int16_t tmin[4] = {120, 120, 12, 12};
      const uint8_t code[4] = {0, 0, 0, 0};
      CHECK(!render(tmax, tmin, code));
      // Same digits, same width: cells 0 and 1 both draw "25" and their
      // maximum bands end at the same OFFSET from their own cell origin
      // (scanned above the slot rows so the icons' ink cannot interfere).
      CHECK(rightmostInkInBand(canvas, ORIGIN[0], 7, 7) - ORIGIN[0].x0 ==
            rightmostInkInBand(canvas, ORIGIN[1], 7, 7) - ORIGIN[1].x0);
      CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_WARM)); // "25" both
      CHECK(!bandHas(canvas, ORIGIN[1], 7, 10, PINNED_WARM));
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_COLD));
      // No minus anywhere: nothing in either maximum band rows sits left
      // of the first digit — glyph "2" starts top-left at the origin.
      for (int i : {0, 1})
        CHECK(canvas[(ORIGIN[i].y0 + 7) * 64 + ORIGIN[i].x0] != SENTINEL);
      // "-100" (cell 2): the icon drops, all three digits kept in the
      // intense blue; the slot probe starts past the kept figure's last
      // column (x0+21). "-5" (cell 3, beside the two-digit minimum): icon
      // kept at the 19 offset of a 2-digit widest figure.
      CHECK(!slotHasInk(canvas, 2, 22, 26));
      CHECK(rightmostInkInBand(canvas, ORIGIN[2], 7, 7) ==
            ORIGIN[2].x0 + 21);
      CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_COLD));
      CHECK(slotHasInk(canvas, 3, 19, 26));
    }
  } else if (test == "units") {
    // A Celsius-stored forecast shown in Fahrenheit converts through the
    // committed rule without a refetch; a Fahrenheit-stored forecast shown
    // in Celsius converts back. The renderer only rearranges pixels.
    const int16_t tmax[4] = {200, -50, 370, 0};
    const int16_t tmin[4] = {80, -150, 0, 100};
    const uint8_t prob[4] = {1, 22, 99, 40};
    const uint8_t code[4] = {0, 0, 0, 0};
    const weather::Forecast celsius =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const weather::Forecast fahrenheit =
        makeForecast(weather::Unit::Fahrenheit, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(6); // SAB today
    realize(celsius, weather::Unit::Fahrenheit, calendar);
    CHECK(canvas == expectDraw(celsius, weather::Unit::Fahrenheit, calendar));
    realize(fahrenheit, weather::Unit::Celsius, calendar);
    CHECK(canvas ==
          expectDraw(fahrenheit, weather::Unit::Celsius, calendar));
    // The same stored record, same shown unit: byte-identical canvas.
    const Canvas asFahrenheit = canvas;
    realize(fahrenheit, weather::Unit::Fahrenheit, calendar);
    CHECK(canvas == expectDraw(fahrenheit, weather::Unit::Fahrenheit, calendar));
    CHECK(!(canvas == asFahrenheit)); // the unit switch changed the image
  } else if (test == "labels") {
    // Today is always HOY; +1..+3 advance the supplied calendar weekday.
    const int16_t tmax[4] = {123, 111, 102, 98};
    const int16_t tmin[4] = {31, -25, 41, 12};
    const uint8_t prob[4] = {10, 45, 80, 5};
    const uint8_t code[4] = {1, 61, 0, 95};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    for (int wday = 0; wday < 7; ++wday) {
      const std::tm calendar = makeCalendar(wday);
      realize(forecast, weather::Unit::Celsius, calendar);
      CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
      // Every cell carries a visible day label in its label band (HOY for
      // today, the weekday for the other three; the exact glyphs and the
      // S15 violet ink are pinned by the expected-image match above).
      for (int i = 0; i < 4; ++i)
        CHECK(bandHas(canvas, ORIGIN[i], 0, 5, PINNED_VIOLET));
    }
  } else if (test == "invalid") {
    // A record without a valid forecast paints NOTHING at all — the board
    // keeps the clock until a forecast exists (the S1 gating lives in the
    // caller, but the renderer itself must be inert).
    const weather::Forecast invalid = {};
    realize(invalid, weather::Unit::Celsius, makeCalendar(3));
    for (int p = 0; p < 64 * 64; ++p) CHECK(canvas[p] == SENTINEL);
    // A body with fewer than four days never becomes a valid forecast
    // (the committed parser rejects it), so it draws nothing either.
    const char *threeDays =
        "{\"daily\":{\"temperature_2m_max\":[1,2,3,4],"
        "\"temperature_2m_min\":[1,2,3,4],"
        "\"precipitation_probability_max\":[1,2,3]}}";
    weather::Forecast parsed;
    CHECK(!weather::parseForecast(threeDays, std::strlen(threeDays), parsed));
    CHECK(!parsed.valid);
    realize(parsed, weather::Unit::Celsius, makeCalendar(3));
    for (int p = 0; p < 64 * 64; ++p) CHECK(canvas[p] == SENTINEL);
  } else if (test == "brightness") {
    // Every painted pixel must pass through the shared brightness helper:
    // the same render through a panel::color() wrapper equals the raw
    // canvas mapped through panel::color() per pixel, sentinel untouched.
    // Cell 0 carries a negative maximum (-52.0) in the intense blue and a
    // positive minimum in the muted ink; cell 1 a positive maximum in
    // white; the clear icon of cell 0 sits in its S9 slot.
    const int16_t tmax[4] = {-520, 111, 102, 98};
    const int16_t tmin[4] = {31, -25, 41, 12};
    const uint8_t prob[4] = {10, 45, 80, 5};
    const uint8_t code[4] = {0, 1, 3, 63};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(1); // LUN today
    realize(forecast, weather::Unit::Celsius, calendar);
    const Canvas raw = canvas;
    Canvas scaled;
    scaled.fill(SENTINEL);
    weather_screen::draw(forecast, weather::Unit::Celsius, calendar,
                         [&scaled](int x, int y, uint16_t color) {
                           if (x >= 0 && x < 64 && y >= 0 && y < 64)
                             scaled[y * 64 + x] = panel::color(color);
                         });
    for (int p = 0; p < 64 * 64; ++p)
      CHECK(scaled[p] == (raw[p] == SENTINEL ? SENTINEL : panel::color(raw[p])));
    // S10 bands graded through panel::color: cell 0 maximum -52 °C
    // (COLD) over a 3 °C minimum (COOL); cell 1 maximum 11 °C (COOL); the
    // amber clear icon and the S15 white probability ink (prob 10)
    // graded too.
    CHECK(bandHas(scaled, ORIGIN[0], 7, 10, panel::color(PINNED_COLD)));
    CHECK(bandHas(scaled, ORIGIN[0], 19, 10,
                  panel::color(PINNED_COOL)));
    CHECK(bandHas(scaled, ORIGIN[1], 7, 10,
                  panel::color(PINNED_COOL)));
    CHECK(bandHas(scaled, ORIGIN[0], 0, 5,
                  panel::color(PINNED_PROB_WHITE)));
    CHECK(bandHas(scaled, ORIGIN[0], 14, 8,
                  panel::color(idle_clock::AMBER)));
    CHECK(!(scaled == raw)); // 30 % default actually changed the values
  } else {
    std::cerr << "unknown case\n";
    return 2;
  }
  std::cout << "OK\n";
  return 0;
}
