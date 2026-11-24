// Host test rig for the weather screen renderer (T2, S5/S7/S8): a 64x64
// uint16_t canvas painted through a pixel callback exactly like
// idle_clock::draw, compared against an expected image built in this rig
// from the DOCUMENTED geometry (divider column/row, the four cell origins,
// the field offsets and scales, the truncation rule, the icon slot rule)
// using the same idle_clock::text glyph engine — so a layout drift in
// weather_screen.h is visible, not just "some pixels exist". Every case is
// one invocation: weather_screen_test <case>; OK + exit 0 or
// "line N: condition" on stderr + exit 1.
//
// The 8x8 icon bitmaps are pinned HERE as raw bytes — the exact-image
// match therefore proves the renderer draws precisely these bitmaps at the
// documented slot, not merely that the used constant matches itself.
//
// Cases:
//   dividers  the two 1 px lines own exactly their row and column
//   cells     per-cell isolation: mutating one cell's inputs moves pixels
//             only inside that cell's region; full expected image match
//   truncation oversized values keep whole trailing characters and never
//             reach the divider or the neighbour
//   values    two-digit, three-digit, negative and zero temperatures;
//             zero-padded NN% probabilities parked at the cell's right
//             edge exactly on the label row (S7)
//   icons     S7/S8 layout proofs: both temperatures the same height; the
//             icon slot inked only when both numbers fit AND the WMO code
//             maps; three-digit magnitudes drop the icon and keep EVERY
//             digit; unknown codes draw no icon
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
// [y0, y0+30]; the empty column 31 keeps the left cells clear of the
// divider column 32 even at full width.
struct Region { int x0, y0; };
const Region ORIGIN[4] = {{0, 0}, {33, 0}, {0, 33}, {33, 33}};
constexpr int REGION_SPAN = 31;

Canvas canvas, expected;

void expPixel(int x, int y, uint16_t color) {
  if (x >= 0 && x < 64 && y >= 0 && y < 64) expected[y * 64 + x] = color;
}

// The expected number formatting re-derives the documented rule with the
// committed pure-layer helpers: stored tenths -> shown unit -> integer
// degrees rounding half away from zero.
std::string degreesText(const weather::Forecast &f, int index, bool maximum,
                        weather::Unit shown) {
  const int16_t stored = maximum ? f.tmax[index] : f.tmin[index];
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "%d",
                weather::roundedDegrees(
                    weather::displayedTenths(stored, f.unit, shown)));
  return buffer;
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

void expectCell(const Region &r, const weather::Forecast &f, int index,
                int wday, weather::Unit shown) {
  // S7 offsets: label y+0 (scale 1) with the probability right-aligned ON
  // THE SAME ROW — its '%' ends exactly at the cell's own edge, column
  // x0+30; maximum y+7 (scale 2); minimum y+19 (scale 2, same size, the
  // maximum reads first). The icon slot is the last eight columns
  // (x0+23..x0+30) over the vertical middle rows (y0+14..y0+21).
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

  const std::string maximum =
      fitted(degreesText(f, index, true, shown), 2);
  idle_clock::text(maximum.c_str(), r.x0, r.y0 + 7, 2, idle_clock::WHITE,
                   expPixel);
  const std::string minimum =
      fitted(degreesText(f, index, false, shown), 2);
  idle_clock::text(minimum.c_str(), r.x0, r.y0 + 19, 2, idle_clock::BLUE,
                   expPixel);

  // S8: the icon is drawn only when BOTH temperatures fit beside it (at
  // scale 2 any three-character figure — or a two-digit one written with
  // its sign — would collide) and the day's WMO code belongs to a group;
  // the unknown state has no bitmap.
  const weather::Icon icon = weather::iconFor(f.code[index]);
  if (maximum.size() <= 2 && minimum.size() <= 2 &&
      icon != weather::Icon::Unknown) {
    const int which = static_cast<int>(icon);
    for (int row = 0; row < 8; ++row)
      for (int col = 0; col < 8; ++col)
        if (PINNED_ICONS[which][row] & (0x80 >> col))
          expPixel(r.x0 + 23 + col, r.y0 + 14 + row, PINNED_ICON_INK[which]);
  }
}

Canvas expectDraw(const weather::Forecast &f, weather::Unit shown,
                  const std::tm &calendar) {
  expected.fill(SENTINEL);
  if (!f.valid) return expected;
  for (int i = 0; i < 64; ++i) {
    expected[32 * 64 + i] = idle_clock::MUTED; // horizontal divider, row 32
    expected[i * 64 + 32] = idle_clock::MUTED; // vertical divider, column 32
  }
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

// The icon slot: any non-sentinel pixel inside columns x0+23..x0+30,
// rows y0+14..y0+21.
bool slotHasInk(const Canvas &c, int index) {
  const Region &r = ORIGIN[index];
  for (int y = r.y0 + 14; y < r.y0 + 22; ++y)
    for (int x = r.x0 + 23; x < r.x0 + 31; ++x)
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
    // The divider lines own exactly their row and column, in the muted ink,
    // with nothing else drawn there.
    for (int i = 0; i < 64; ++i) {
      CHECK(canvas[32 * 64 + i] == idle_clock::MUTED);
      CHECK(canvas[i * 64 + 32] == idle_clock::MUTED);
    }
    // The one spare column/row of each cell strip stays empty, except
    // where the divider lines legitimately cross them: row 32 paints
    // (31, 32) and column 32 paints (32, 31), both in the muted ink.
    for (int i = 0; i < 64; ++i) {
      CHECK(i == 32 ? canvas[i * 64 + 31] == idle_clock::MUTED
                    : canvas[i * 64 + 31] == SENTINEL);
      CHECK(i == 32 ? canvas[31 * 64 + i] == idle_clock::MUTED
                    : canvas[31 * 64 + i] == SENTINEL);
    }
    if (test == "cells") {
      // Mutating ONE cell's inputs may move pixels only inside that cell's
      // region — no bleed into the neighbour or the dividers.
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
    // Cell 0: -100.0 at scale 2 is five glyphs (38 px) and must lose whole
    // trailing characters down to "-100" (30 px), never a mid-glyph cut.
    // Cell 1: +1000.0 four glyphs (30 px) fits exactly. Cell 2: the minimum
    // -100.0 also sits at scale 2 now (30 px) and never truncates.
    const int16_t tmax[4] = {-1000, 10000, 123, 98};
    const int16_t tmin[4] = {31, -25, -1000, 12};
    const uint8_t prob[4] = {10, 45, 80, 5};
    const uint8_t code[4] = {0, 3, 45, 63};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(0); // DOM today
    realize(forecast, weather::Unit::Celsius, calendar);
    CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
    // Explicit proof that the truncated cell stays inside its region: the
    // maximum band of cell 0 paints nothing beyond column 29, the spare
    // column 31 carries nothing, and column 32 only ever shows the divider.
    for (int y = 7; y <= 16; ++y) {
      CHECK(canvas[y * 64 + 30] == SENTINEL); // "-100" ends at column 29
      CHECK(canvas[y * 64 + 31] == SENTINEL); // spare column empty
      CHECK(canvas[y * 64 + 32] == idle_clock::MUTED); // only the divider
    }
    for (int y = 0; y < 64; ++y)
      CHECK(y == 32 ? canvas[y * 64 + 31] == idle_clock::MUTED
                    : canvas[y * 64 + 31] == SENTINEL);
  } else if (test == "values") {
    // Two-digit, three-digit, negative and zero temperatures; probabilities
    // 0 (as 00%), single digit (07%) and three digits (100%). A negative
    // minimum ('-40') and a negative-sign maximum ('-5') are both drawn.
    const int16_t tmax[4] = {253, 1000, -52, 0};
    const int16_t tmin[4] = {0, -1, -400, 400};
    const uint8_t prob[4] = {0, 7, 100, 55};
    const uint8_t code[4] = {0, 2, 45, 61};
    const weather::Forecast forecast =
        makeForecast(weather::Unit::Celsius, tmax, tmin, prob, code);
    const std::tm calendar = makeCalendar(2); // MAR today
    realize(forecast, weather::Unit::Celsius, calendar);
    CHECK(canvas == expectDraw(forecast, weather::Unit::Celsius, calendar));
    // Positive band probes: every field of every cell is present inside
    // its own band in its own ink (the exact-image match above pins the
    // exact glyph pixels). S7: label and probability share the label row;
    // both temperatures use the ten scale-2 rows.
    for (int i = 0; i < 4; ++i) {
      CHECK(bandHas(canvas, ORIGIN[i], 0, 5, idle_clock::MUTED));
      CHECK(bandHas(canvas, ORIGIN[i], 7, 10, idle_clock::WHITE));
      CHECK(bandHas(canvas, ORIGIN[i], 19, 10, idle_clock::BLUE));
      CHECK(bandHas(canvas, ORIGIN[i], 0, 5, idle_clock::AMBER));
      // S7: the probability is parked at the cell's own right edge — the
      // rightmost amber pixel is exactly the last column of the cell, and
      // amber lives on the label row only: below the icon slot (rows
      // y0+22..y0+30, the old probability band) no amber can remain.
      CHECK(rightmostOf(canvas, ORIGIN[i], idle_clock::AMBER) ==
            ORIGIN[i].x0 + 30);
      for (int y = ORIGIN[i].y0 + 22; y < ORIGIN[i].y0 + REGION_SPAN; ++y)
        for (int x = ORIGIN[i].x0; x < ORIGIN[i].x0 + REGION_SPAN; ++x)
          CHECK(canvas[y * 64 + x] != idle_clock::AMBER);
    }
    // The negative maximum of cell 2 carries its sign: the '-' glyph is
    // always the FIRST character of a negative value, so its dash — at
    // scale 2 the scaled rows y0+7+4..y0+7+5 — starts at the cell origin
    // itself (col 0 is on in {0,0,7,0,0}).
    CHECK(canvas[(ORIGIN[2].y0 + 7 + 4) * 64 + ORIGIN[2].x0] ==
          idle_clock::WHITE);
  } else if (test == "icons") {
    // S7/S8 layout proofs, one image per rule.
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
    { // (a) Row 0: label left, probability right-aligned at x0+30; both
      // temperatures ten rows tall; icons for group codes.
      const int16_t tmax[4] = {250, 1000, 253, 98};
      const int16_t tmin[4] = {120, -25, 1000, 12};
      const uint8_t code[4] = {0, 63, 3, 95};
      CHECK(!render(tmax, tmin, code));
      for (int i = 0; i < 4; ++i) {
        CHECK(rightmostOf(canvas, ORIGIN[i], idle_clock::AMBER) ==
              ORIGIN[i].x0 + 30);
        CHECK(bandHas(canvas, ORIGIN[i], 0, 1, idle_clock::AMBER));
        // The same height: for sign-free numbers every one of the digit
        // glyphs lights all five rows, so the maximum band rows y0+7..y0+16
        // and the minimum band rows y0+19..y0+28 are all ten rows tall.
        for (int dy = 0; dy < 10; ++dy) {
          CHECK(bandHas(canvas, ORIGIN[i], 7 + dy, 1, idle_clock::WHITE));
          CHECK(bandHas(canvas, ORIGIN[i], 19 + dy, 1, idle_clock::BLUE));
        }
      }
      // The icon slot: cells 0 (clear) and 3 (storm) carry ink; cells 1
      // (three-digit maximum) and 2 (three-digit minimum) do not.
      CHECK(slotHasInk(canvas, 0));
      CHECK(slotHasInk(canvas, 3));
      CHECK(!slotHasInk(canvas, 1));
      CHECK(!slotHasInk(canvas, 2));
      // The three-digit drop keeps EVERY digit: "100" (white) spans
      // columns x0..x0+21 with the slot beside it empty… and the minimum
      // of cell 2 ("100" in blue) is what drops cell 2's icon — cell 2's
      // own maximum "25" would have fit alone.
      int lastWhite = -1, lastBlue = -1;
      for (int y = ORIGIN[1].y0 + 7; y <= ORIGIN[1].y0 + 16; ++y)
        for (int x = ORIGIN[1].x0; x < ORIGIN[1].x0 + REGION_SPAN; ++x)
          if (canvas[y * 64 + x] == idle_clock::WHITE) lastWhite = x;
      CHECK(lastWhite == ORIGIN[1].x0 + 21);
      for (int y = ORIGIN[2].y0 + 19; y <= ORIGIN[2].y0 + 28; ++y)
        for (int x = ORIGIN[2].x0; x < ORIGIN[2].x0 + REGION_SPAN; ++x)
          if (canvas[y * 64 + x] == idle_clock::BLUE) lastBlue = x;
      CHECK(lastBlue == ORIGIN[2].x0 + 21);
      // The icon never overlaps a number or a divider: the slot of an
      // iconized cell carries the very ink the pinned bitmap uses, and
      // nothing paints past the slot's right edge (the spare column 31
      // stays empty against the divider at 32).
      for (int i : {0, 3}) {
        const uint16_t ink =
            PINNED_ICON_INK[i == 0 ? int(weather::Icon::Clear)
                                   : int(weather::Icon::Storm)];
        bool present = false;
        for (int y = ORIGIN[i].y0 + 14; y < ORIGIN[i].y0 + 22; ++y)
          for (int x = ORIGIN[i].x0 + 23; x < ORIGIN[i].x0 + 31; ++x)
            if (canvas[y * 64 + x] == ink) present = true;
        CHECK(present);
        if (i == 0) // the right-hand cells end at the canvas edge (col 63)
          CHECK(canvas[(ORIGIN[i].y0 + 14) * 64 + ORIGIN[i].x0 + 31] ==
                SENTINEL);
      }
    }
    { // (b) Unknown codes draw no icon even when both numbers fit.
      const int16_t tmax[4] = {250, 250, 253, 98};
      const int16_t tmin[4] = {120, 120, 12, 12};
      const uint8_t code[4] = {0, 44, 45, 48};
      CHECK(!render(tmax, tmin, code));
      CHECK(slotHasInk(canvas, 0));  // clear
      CHECK(!slotHasInk(canvas, 1)); // code 44: outside every group
      CHECK(slotHasInk(canvas, 2));  // fog (45)
      CHECK(slotHasInk(canvas, 3));  // rime fog (48)
    }
    { // (c) Signs cost a glyph cell: "-5" keeps the icon, "-12" drops it
      // like a three-digit magnitude, keeping every digit.
      const int16_t tmax[4] = {250, 250, -120, -52};
      const int16_t tmin[4] = {120, 120, 12, 12};
      const uint8_t code[4] = {0, 0, 0, 0};
      CHECK(!render(tmax, tmin, code));
      CHECK(slotHasInk(canvas, 0));
      CHECK(slotHasInk(canvas, 1));
      CHECK(!slotHasInk(canvas, 2)); // "-12" is three characters
      int lastWhite = -1;
      for (int y = ORIGIN[2].y0 + 7; y <= ORIGIN[2].y0 + 16; ++y)
        for (int x = ORIGIN[2].x0; x < ORIGIN[2].x0 + REGION_SPAN; ++x)
          if (canvas[y * 64 + x] == idle_clock::WHITE) lastWhite = x;
      CHECK(lastWhite == ORIGIN[2].x0 + 21); // every digit of "-12" kept
      CHECK(slotHasInk(canvas, 3));          // "-5" fits beside the icon
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
    const int16_t tmax[4] = {123, 111, 102, 98};
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
    CHECK(scaled[32 * 64 + 5] == panel::color(idle_clock::MUTED));
    CHECK(bandHas(scaled, ORIGIN[0], 7, 10,
                  panel::color(idle_clock::WHITE)));
    // The scale-2 minimum band and the S8 icon slot are graded through
    // panel::color as well (cell 0 is clear at AMBER ink here).
    CHECK(bandHas(scaled, ORIGIN[0], 19, 10,
                  panel::color(idle_clock::BLUE)));
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
