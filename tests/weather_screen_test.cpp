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
// The 8x8 icon bitmaps are pinned HERE as raw bytes, and the intense blue
// of the negative figures is pinned HERE as a raw 565 word — the
// exact-image match therefore proves the renderer draws precisely these
// bitmaps and precisely this ink, not merely that the used constant
// matches itself.
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
//             (S9)
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

// The documented cell origins: today top left, +1 top right, +2 bottom
// left, +3 bottom right. Each cell owns a 31x31 area [x0, x0+30] x
// [y0, y0+30]; the empty column 31 and the empty crossing of column 32 /
// row 32 keep every cell clear of its neighbours (S9: no line is drawn
// anywhere).
struct Region { int x0, y0; };
const Region ORIGIN[4] = {{0, 0}, {33, 0}, {0, 33}, {33, 33}};
constexpr int REGION_SPAN = 31;

Canvas canvas, expected;

void expPixel(int x, int y, uint16_t color) {
  if (x >= 0 && x < 64 && y >= 0 && y < 64) expected[y * 64 + x] = color;
}

// The expected number formatting re-derives the documented rule with the
// committed pure-layer helpers: stored tenths -> shown unit -> integer
// degrees rounding half away from zero (SIGNED text; the sign itself is
// consumed below).
std::string degreesText(const weather::Forecast &f, int index, bool maximum,
                        weather::Unit shown) {
  const int16_t stored = maximum ? f.tmax[index] : f.tmin[index];
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "%d",
                weather::roundedDegrees(
                    weather::displayedTenths(stored, f.unit, shown)));
  return buffer;
}

// S9: the displayed figure drops its minus glyph entirely; the colour
// carries the sign. Returns the digit-only text and reports the sign.
std::string degreesDigits(const weather::Forecast &f, int index, bool maximum,
                          weather::Unit shown, bool &negative) {
  const std::string signedText = degreesText(f, index, maximum, shown);
  negative = !signedText.empty() && signedText[0] == '-';
  return negative ? signedText.substr(1) : signedText;
}

// The documented probability rule: zero-padded two digits (100 keeps
// three); the percent glyph follows the digits.
std::string probText(uint8_t probability) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "%02u", unsigned(probability));
  return buffer;
}

// The truncation rule re-derived here (NOT taken from the header under
// test): a word wider than the 31 px cell loses whole trailing characters
// until it fits; width = 4*chars*scale - scale for the 3x5 glyph engine.
std::string fitted(const std::string &value, int scale) {
  size_t n = value.size();
  while (n > 1 && 4 * static_cast<int>(n) * scale - scale >
                      weather_screen::CELL)
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

// S9: the intense blue of every negative figure, pinned RAW here (the
// header builds it with idle_clock::rgb(64,64,255) = 0x421F — a deep
// saturated blue, clearly distinct from the clock's sky-blue
// rgb(85,189,232) = 0x55FD).
constexpr uint16_t PINNED_NEG = 0x421F;

void expectCell(const Region &r, const weather::Forecast &f, int index,
                int wday, weather::Unit shown) {
  // S7 offsets: label y+0 (scale 1) with the probability right-aligned ON
  // THE SAME ROW — its '%' ends exactly at the cell's own edge, column
  // x0+30; maximum y+7 (scale 2); minimum y+19 (scale 2, same size, the
  // maximum reads first). The icon (S8/S9, centred at draw time between
  // the wider figure and the cell's right edge) spans the vertical middle
  // rows (y0+14..y0+21).
  idle_clock::text(weather::dayLabel(static_cast<size_t>(index), wday), r.x0,
                   r.y0 + 0, 1, idle_clock::MUTED, expPixel);
  const std::string digits = probText(f.prob[index]);
  const int left = r.x0 + weather_screen::CELL -
                   (4 * (static_cast<int>(digits.size()) + 1) - 1);
  idle_clock::text(digits.c_str(), left, r.y0 + 0, 1, idle_clock::AMBER,
                   expPixel);
  const int px = left + 4 * static_cast<int>(digits.size());
  for (int row = 0; row < 5; ++row)
    for (int col = 0; col < 3; ++col)
      if (weather_screen::PCT[row] & (4 >> col))
        expPixel(px + col, r.y0 + 0 + row, idle_clock::AMBER);

  // S9: the digits lose their sign; the ink encodes it. A negative figure
  // (maximum or minimum) is intense blue; a positive maximum stays white
  // and the positive minimum is muted.
  bool maxNegative = false, minNegative = false;
  const std::string maximum = fitted(
      degreesDigits(f, index, true, shown, maxNegative), 2);
  idle_clock::text(maximum.c_str(), r.x0, r.y0 + 7, 2,
                   maxNegative ? PINNED_NEG : idle_clock::WHITE, expPixel);
  const std::string minimum = fitted(
      degreesDigits(f, index, false, shown, minNegative), 2);
  idle_clock::text(minimum.c_str(), r.x0, r.y0 + 19, 2,
                   minNegative ? PINNED_NEG : idle_clock::MUTED, expPixel);

  // S8 (positions per S9 + the centring correction): the icon is drawn
  // only when BOTH figures fit beside it — at most two signless digits
  // each — and the day's WMO code belongs to a group; the unknown state
  // has no bitmap. Its 8 px are CENTRED in the free space between the end
  // of the WIDER of the two figures actually drawn and the cell's own
  // right edge: figureW = 8*digits - 2, leftGap = (CELL - figureW - 7)/2
  // (the odd pixel of an odd gap goes to the LEFT). d=1: icon at x0+15;
  // d=2: icon at x0+19.
  const weather::Icon icon = weather::iconFor(f.code[index]);
  if (maximum.size() <= 2 && minimum.size() <= 2 &&
      icon != weather::Icon::Unknown) {
    const int widest = static_cast<int>(maximum.size() > minimum.size()
                                            ? maximum.size()
                                            : minimum.size());
    const int figureW = 8 * widest - 2;
    const int iconStart =
        r.x0 + figureW + (weather_screen::CELL - figureW - 7) / 2;
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
    for (int x = r.x0; x < r.x0 + REGION_SPAN; ++x)
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

// The rightmost painted column of one colour inside the cell's region.
int rightmostOf(const Canvas &c, const Region &r, uint16_t color) {
  for (int x = r.x0 + REGION_SPAN - 1; x >= r.x0; --x)
    for (int y = r.y0; y < r.y0 + REGION_SPAN; ++y)
      if (c[y * 64 + x] == color) return x;
  return -1;
}

// The rightmost column of ANY non-sentinel pixel within one row band of
// the cell (used for digit-width comparisons).
int rightmostInkInBand(const Canvas &c, const Region &r, int dy, int rowspan) {
  int rightmost = -1;
  for (int y = r.y0 + dy; y < r.y0 + dy + rowspan; ++y)
    for (int x = r.x0; x < r.x0 + REGION_SPAN; ++x)
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
            if (x < ORIGIN[i].x0 || x >= ORIGIN[i].x0 + REGION_SPAN ||
                y < ORIGIN[i].y0 || y >= ORIGIN[i].y0 + REGION_SPAN)
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
    // Explicit proof that the extreme cells stay inside their region: the
    // four-glyph maximum/minimum bands end at column x0+29, the spare
    // column 31 stays empty and — with the dividers gone — even column 32
    // and row 32 carry nothing at all.
    for (int y = 7; y <= 16; ++y) {
      CHECK(canvas[y * 64 + 30] == SENTINEL); // "1000" ends at column 29
      CHECK(canvas[y * 64 + 31] == SENTINEL); // spare column empty
      CHECK(canvas[y * 64 + 32] == SENTINEL); // no separator line
    }
    for (int i = 0; i < 64; ++i) {
      CHECK(canvas[i * 64 + 32] == SENTINEL);
      CHECK(canvas[31 * 64 + i] == SENTINEL);
      CHECK(canvas[i * 64 + 31] == SENTINEL);
    }
    CHECK(bandHas(canvas, ORIGIN[0], 7, 10, PINNED_NEG));
    CHECK(bandHas(canvas, ORIGIN[1], 7, 10, idle_clock::WHITE));
    CHECK(bandHas(canvas, ORIGIN[2], 19, 10, PINNED_NEG));
  } else if (test == "values") {
    // Two-digit, three-digit, negative and zero temperatures; probabilities
    // 0 (as 00%), single digit (07%) and three digits (100%). A negative
    // minimum (-40) and a negative maximum (-5) are both drawn WITHOUT the
    // minus glyph, in intense blue (S9).
    const int16_t tmax[4] = {253, 1000, -52, 0};
    const int16_t tmin[4] = {0, -1, -400, 400};
    const uint8_t prob[4] = {0, 7, 100, 55};
    const uint8_t code[4] = {0, 2, 45, 61};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(2); // MAR today
    realize(forecast, weather::Unit::Celsius, calendar);
    CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
    // Band probes: positive maxima white (cells 0, 1, 3), positive minima
    // muted instead of blue (S9), negative figures intense blue (cell 2
    // maximum "5" and minimum "40").
    for (int i : {0, 1, 3}) {
      CHECK(bandHas(canvas, ORIGIN[i], 7, 10, idle_clock::WHITE));
      CHECK(bandHas(canvas, ORIGIN[i], 19, 10, idle_clock::MUTED));
      CHECK(!bandHas(canvas, ORIGIN[i], 7, 10, PINNED_NEG));
      CHECK(!bandHas(canvas, ORIGIN[i], 19, 10, PINNED_NEG));
    }
    CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_NEG));
    CHECK(bandHas(canvas, ORIGIN[2], 19, 10, PINNED_NEG));
    CHECK(!bandHas(canvas, ORIGIN[2], 7, 10, idle_clock::WHITE));
    // The label row keeps its muted day labels and amber probability at
    // the cell's right edge on row 0 only.
    for (int i = 0; i < 4; ++i) {
      CHECK(bandHas(canvas, ORIGIN[i], 0, 5, idle_clock::MUTED));
      CHECK(bandHas(canvas, ORIGIN[i], 0, 5, idle_clock::AMBER));
      CHECK(rightmostOf(canvas, ORIGIN[i], idle_clock::AMBER) ==
            ORIGIN[i].x0 + 30);
      for (int y = ORIGIN[i].y0 + 22; y < ORIGIN[i].y0 + REGION_SPAN; ++y)
        for (int x = ORIGIN[i].x0; x < ORIGIN[i].x0 + REGION_SPAN; ++x)
          CHECK(canvas[y * 64 + x] != idle_clock::AMBER);
    }
    // S9: the negative maximum of cell 2 starts with its digit, not with
    // a dash — glyph "5" lights its top-left pixel at the cell origin in
    // the NEGATIVE ink, whereas a drawn minus would place the dash lower
    // (row y0+7+8..y0+7+9 at scale 2, the scaled glyph row 2) and leave
    // the top rows blank.
    CHECK(canvas[(ORIGIN[2].y0 + 7 + 0) * 64 + ORIGIN[2].x0] == PINNED_NEG);
    for (int dy = 0; dy < 4; ++dy)
      CHECK(canvas[(ORIGIN[2].y0 + 7 + dy) * 64 + ORIGIN[2].x0] != SENTINEL);
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
      // leftGap = (CELL - figureW - 7)/2, icon = figureW + leftGap.
      // d=2 -> 19, d=1 -> 15. A one-digit cell (c1) and a two-digit cell
      // (c0) in the same slot geometry therefore differ, and the mixed
      // cells (c2, c3) follow the WIDER figure.
      const int expectedStart[4] = {19, 15, 19, 19};
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
        // x0+30 included via <= 30). Left gap must equal the right gap,
        // allowing exactly the one odd pixel — which goes to the LEFT.
        const int leftGap = expectedStart[i] - figureW[i];
        const int rightGap = 30 - (expectedStart[i] + 7);
        CHECK(leftGap == rightGap || leftGap == rightGap + 1);
        CHECK(leftGap > 0); // the icon never touches a figure
        for (int y = r.y0 + 14; y < r.y0 + 22; ++y) {
          for (int x = r.x0 + figureW[i]; x < start; ++x)
            CHECK(canvas[y * 64 + x] == SENTINEL); // left gap empty
          for (int x = start + 8; x <= r.x0 + 30; ++x)
            CHECK(canvas[y * 64 + x] == SENTINEL); // right gap empty
        }
        // The icon never reaches the cell's last column (all four cells
        // have narrow figures here) and never leaves the cell.
        CHECK(expectedStart[i] + 7 <= 26);
      }
      // Sanity: same slot geometry, different widths, different starts.
      CHECK(expectedStart[0] != expectedStart[1]);
      // The probabilities still end at the cell's right edge on row 0.
      for (int i = 0; i < 4; ++i)
        CHECK(rightmostOf(canvas, ORIGIN[i], idle_clock::AMBER) ==
              ORIGIN[i].x0 + 30);
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
      CHECK(!slotHasInk(canvas, 0, 22, 30)); // dropped, three-digit max
      CHECK(rightmostInkInBand(canvas, ORIGIN[0], 7, 10) ==
            ORIGIN[0].x0 + 21); // "100" kept whole
      CHECK(!slotHasInk(canvas, 1, 22, 30)); // dropped, three-digit min
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
      CHECK(bandHas(canvas, ORIGIN[0], 7, 10, idle_clock::WHITE));
      CHECK(!bandHas(canvas, ORIGIN[1], 7, 10, idle_clock::WHITE));
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, PINNED_NEG));
      CHECK(bandHas(canvas, ORIGIN[1], 7, 10, idle_clock::MUTED) == false);
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
      CHECK(bandHas(canvas, ORIGIN[2], 7, 10, PINNED_NEG));
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
      // today, the weekday for the other three; the exact glyphs are
      // pinned by the expected-image match above).
      for (int i = 0; i < 4; ++i)
        CHECK(bandHas(canvas, ORIGIN[i], 0, 5, idle_clock::MUTED));
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
    CHECK(bandHas(scaled, ORIGIN[0], 7, 10, panel::color(PINNED_NEG)));
    CHECK(bandHas(scaled, ORIGIN[0], 19, 10,
                  panel::color(idle_clock::MUTED)));
    CHECK(bandHas(scaled, ORIGIN[1], 7, 10,
                  panel::color(idle_clock::WHITE)));
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
