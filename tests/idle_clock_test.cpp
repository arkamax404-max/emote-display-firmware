#define local static // PNGdec zutil.h exports this macro in the target translation unit.
#include "idle_clock.h"
#include "idle_clock_image.h"
#include <array>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "line " << __LINE__ << ": " #condition "\n"; return 1; } } while (false)
using idle_clock::Source;

// Reference expectation, extracted independently from lemmings_reference.png
// (362x545 RGBA, 15 px per logical pixel, grid origin (2,3)): true palette
// sampled from pure interior blocks and the nine 8x10 pose grids, all facing
// right. H = green hair, S = pale pink skin, O = royal-blue overalls.
constexpr uint16_t EXP_HAIR = idle_clock::rgb(0, 179, 0);
constexpr uint16_t EXP_SKIN = idle_clock::rgb(240, 208, 208);
constexpr uint16_t EXP_OUTFIT = idle_clock::rgb(64, 64, 224);
constexpr const char *EXPECTED[9][10] = {
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

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];
  idle_clock::State state;
  if (test == "boot") {
    CHECK(state.source(0, false, false, false) == Source::Message);
    CHECK(state.source(1, true, false, false) == Source::Clock);
    // Network connectivity is deliberately not a time-validity requirement.
    CHECK(state.source(60000, true, false, false) == Source::Clock);
    CHECK(state.source(60001, false, false, false) == Source::Message);
  } else if (test == "lease") {
    state.pngResult(true, 1234);
    CHECK(state.source(11233, true, true, false) == Source::Frame);
    state.pngResult(false, 11233); // Rejected PNG must not renew the lease.
    CHECK(state.source(11234, true, true, false) == Source::Clock);
    CHECK(state.source(11234, false, true, false) == Source::Frame);
    state.pngResult(true, 12000);
    CHECK(state.source(12000, true, true, false) == Source::Frame);
    CHECK(state.source(21999, true, true, false) == Source::Frame);
    CHECK(state.source(22000, true, true, false) == Source::Clock);
  } else if (test == "wrap") {
    const uint32_t start = UINT32_MAX - 5000;
    state.pngResult(true, start);
    CHECK(state.source(uint32_t(start + 9999), true, true, false) == Source::Frame);
    CHECK(state.source(uint32_t(start + 10000), true, true, false) == Source::Clock);
    // Once expired, a full future counter wrap cannot resurrect the PNG.
    CHECK(state.source(start, true, true, false) == Source::Clock);
    state.pngResult(true, start);
    CHECK(state.source(start, true, true, false) == Source::Frame);
  } else if (test == "overlay") {
    state.pngResult(true, 0);
    CHECK(state.source(10000, true, true, true) == Source::WifiInfo);
    CHECK(state.source(10001, true, true, false) == Source::Clock);
    state.pngResult(true, 10002);
    CHECK(state.source(10002, true, true, true) == Source::WifiInfo);
    CHECK(state.source(10003, true, true, false) == Source::Frame);
    CHECK(state.source(10003, false, false, false) == Source::Message);
  } else if (test == "pacing") {
    CHECK(state.redrawDue(0, Source::Clock));
    state.painted(0, Source::Clock);
    // Clock redraws every 500 ms for the lemming animation; the calendar
    // labels themselves only change once per second.
    CHECK(!state.redrawDue(499, Source::Clock));
    CHECK(state.redrawDue(500, Source::Clock));
    CHECK(state.redrawDue(1, Source::Frame));
    state.painted(UINT32_MAX - 500, Source::Clock);
    CHECK(!state.redrawDue(UINT32_MAX - 1, Source::Clock));
    CHECK(state.redrawDue(UINT32_MAX, Source::Clock));
    CHECK(state.redrawDue(0, Source::Clock)); // Millis wrap: delta reaches 500.
    state.painted(499, Source::Frame);
    CHECK(!state.redrawDue(50000, Source::Frame));
    CHECK(state.redrawDue(50000, Source::Clock));
  } else if (test == "labels") {
    CHECK(std::string(idle_clock::TIMEZONE) == "CET-1CEST,M3.5.0,M10.5.0/3");
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 11; calendar.tm_mday = 31;
    calendar.tm_hour = 23; calendar.tm_min = 59; calendar.tm_sec = 59;
    auto labels = idle_clock::labels(calendar);
    CHECK(std::string(labels.date) == "31/12/2026");
    CHECK(std::string(labels.hours) == "23" && std::string(labels.minutes) == "59");
    CHECK(std::string(labels.seconds) == "59");
    ++calendar.tm_year; calendar.tm_mon = 0; calendar.tm_mday = 1;
    calendar.tm_hour = calendar.tm_min = calendar.tm_sec = 0;
    labels = idle_clock::labels(calendar);
    CHECK(std::string(labels.date) == "01/01/2027" && std::string(labels.hours) == "00");
    CHECK(std::string(labels.minutes) == "00" && std::string(labels.seconds) == "00");
    calendar.tm_hour = 13; calendar.tm_min = 7; calendar.tm_sec = 9;
    labels = idle_clock::labels(calendar);
    CHECK(std::string(labels.hours) == "13" && std::string(labels.minutes) == "07");
    CHECK(std::string(labels.seconds) == "09");
  } else if (test == "pixels") {
    std::array<uint16_t, 64 * 64> image;
    image.fill(0xffff);
    bool bounds = true;
    auto pixel = [&](int x, int y, uint16_t color) {
      if (x < 0 || x >= 64 || y < 0 || y >= 64) bounds = false;
      else image[y * 64 + x] = color;
    };
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 10; calendar.tm_mday = 9;
    calendar.tm_hour = 12; calendar.tm_min = 34; calendar.tm_sec = 56;
    idle_clock::draw(calendar, 4, pixel); // Walkers at left=0 and 40.
    CHECK(bounds && image[0] == 0);
    CHECK(image[9 * 64 + 16] == idle_clock::WHITE); // 1, scale 4.
    CHECK(image[31 * 64 + 12] == idle_clock::BLUE); // 3, scale 4.
    CHECK(image[33 * 64 + 46] == idle_clock::AMBER); // 5, scale 2.
    CHECK(image[1 * 64 + 12] == idle_clock::MUTED); // Date 0.
    CHECK(image[63 * 64 + 0] == idle_clock::GREEN);
    CHECK(image[63 * 64 + 4] == idle_clock::PATH);
    // Nine reference-extracted lemming frames (8x10) on rows 53..62.
    CHECK(idle_clock::HAIR == EXP_HAIR && idle_clock::SKIN == EXP_SKIN &&
          idle_clock::OUTFIT == EXP_OUTFIT); // True sampled palette, not provisional.
    for (int f = 0; f < 9; ++f) {
      bool hair = false, outfit = false, skin = false;
      for (int y = 0; y < 10; ++y) for (int x = 0; x < 8; ++x) {
        const char part = idle_clock::FRAMES[f][y][x];
        CHECK(part == '.' || part == 'H' || part == 'S' || part == 'O');
        hair |= part == 'H'; outfit |= part == 'O'; skin |= part == 'S';
      }
      CHECK(hair && outfit && skin);
      for (int y = 0; y < 10; ++y)
        CHECK(std::memcmp(idle_clock::FRAMES[f][y], EXPECTED[f][y], 9) == 0);
      for (int g = 0; g < f; ++g)
        CHECK(std::memcmp(idle_clock::FRAMES[f], idle_clock::FRAMES[g],
                          sizeof(idle_clock::FRAMES[0])) != 0);
    }
    // All nine poses cycle on the half-second animation tick (no
    // minute-boundary reset); two walkers keep the existing 2 px/s speed,
    // now 1 px per 500 ms tick, over an 80-tick (40 s) horizontal wrap.
    for (uint64_t tick = 0; tick < 80; ++tick) {
      idle_clock::draw(calendar, tick, pixel);
      CHECK(bounds); // Horizontal entry/exit clipping stays safe in every pose.
      const int frame = int(tick % 9);
      int lefts[2];
      for (int walker = 0; walker < 2; ++walker)
        lefts[walker] = int(((tick % 80) + walker * 40) % 80) - 8;
      for (int y = 53; y <= 62; ++y) for (int x = 0; x < 64; ++x) {
        uint16_t want = 0;
        for (int walker = 0; walker < 2; ++walker) {
          const int col = x - lefts[walker] - 1;
          if (col < 0 || col >= 8) continue;
          const char part = EXPECTED[frame][y - 53][col];
          const uint16_t color = part == 'H' ? EXP_HAIR
                               : part == 'S' ? EXP_SKIN : EXP_OUTFIT;
          CHECK(want == 0); // The two walkers never overlap.
          want = part == '.' ? 0 : color;
        }
        CHECK(image[y * 64 + x] == want); // Silhouette, palette, cleared prior pose.
      }
      for (int x = 0; x < 64; ++x) { // Rows 51..52 and path row 63 untouched.
        CHECK(image[51 * 64 + x] == 0 && image[52 * 64 + x] == 0);
        CHECK(image[63 * 64 + x] == (x % 8 < 3 ? idle_clock::GREEN : idle_clock::PATH));
      }
    }
    // Two consecutive half-second ticks: sprites move and re-pose while
    // every text row (0..50) and the path row stay untouched.
    idle_clock::draw(calendar, 10, pixel);
    const std::array<uint16_t, 64 * 64> previous = image;
    idle_clock::draw(calendar, 11, pixel);
    for (int y = 0; y <= 50; ++y) for (int x = 0; x < 64; ++x)
      CHECK(image[y * 64 + x] == previous[y * 64 + x]);
    for (int x = 0; x < 64; ++x)
      CHECK(image[63 * 64 + x] == previous[63 * 64 + x]);
    bool moved = false;
    for (int y = 53; y <= 62; ++y) for (int x = 0; x < 64; ++x)
      moved |= image[y * 64 + x] != previous[y * 64 + x];
    CHECK(moved); // Sprite pixels changed between consecutive ticks.
    // Full animation cycle: pose wraps at 9 ticks and position at 80, so
    // the sprites repeat exactly at tick 720 = LCM(9, 80).
    idle_clock::draw(calendar, 0, pixel);
    const std::array<uint16_t, 64 * 64> atZero = image;
    idle_clock::draw(calendar, 720, pixel);
    for (int y = 53; y <= 62; ++y) for (int x = 0; x < 64; ++x)
      CHECK(image[y * 64 + x] == atZero[y * 64 + x]);
    idle_clock::draw(calendar, 8, pixel); // Both walkers fully visible.
    CHECK(image[51 * 64 + 0] == 0 && image[51 * 64 + 63] == 0); // Text ends row 50.
    bool crown = false; // Green hair of pose 8 in the top sprite row.
    for (int x = 1; x <= 8; ++x) crown |= image[54 * 64 + x] == EXP_HAIR;
    CHECK(crown);
  } else if (test == "titlephase") {
    // S5: the top band (rows 1..5) alternates the centered date with the
    // session title on absolute-epoch half-decade (5 s) ticks.
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 10; calendar.tm_mday = 9;
    calendar.tm_hour = 12; calendar.tm_min = 34; calendar.tm_sec = 56;
    std::array<uint16_t, 64 * 64> even, odd, plain;
    auto render = [&](uint64_t epochMillis, const char *title,
                      std::array<uint16_t, 64 * 64> &out) {
      out.fill(0xffff);
      idle_clock::draw(calendar, 4, epochMillis, title,
                       [&](int x, int y, uint16_t color) { out[y * 64 + x] = color; });
    };
    render(10000, "HI", even);  // floor(10000/5000) mod 2 == 0: date phase.
    render(5000, "HI", odd);    // Odd phase: title replaces the date.
    render(10000, "", plain);
    // Even phase keeps the canonical centered date and ignores the title.
    CHECK(even[1 * 64 + 12] == idle_clock::MUTED);
    CHECK(even == plain);
    // Odd phase shows the centered title: "HI" is 2 cells, left = (64-7)/2 = 28.
    CHECK(odd[1 * 64 + 28] == idle_clock::MUTED); // 'H' 101 first row.
    CHECK(odd[1 * 64 + 30] == idle_clock::MUTED);
    CHECK(odd[1 * 64 + 12] == 0);                 // date pixel cleared.
    // An empty or absent name always shows the date in both phases.
    render(5000, "", plain);
    CHECK(plain[1 * 64 + 12] == idle_clock::MUTED);
    // Every pixel outside rows 1..5 keeps the canonical clock content.
    for (int y = 6; y < 64; ++y) for (int x = 0; x < 64; ++x)
      CHECK(even[y * 64 + x] == odd[y * 64 + x]);
    for (int x = 0; x < 64; ++x) CHECK(even[x] == odd[x]);
  } else if (test == "truncation") {
    // S5: at most 16 cells; 17 or more becomes 13 cells plus '...'.
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 10; calendar.tm_mday = 9;
    std::array<uint16_t, 64 * 64> image;
    auto render = [&](const char *title) {
      image.fill(0xffff);
      idle_clock::draw(calendar, 4, 5000, title,
                       [&](int x, int y, uint16_t color) { image[y * 64 + x] = color; });
    };
    render("0123456789ABCDEF"); // Exactly 16 cells, left = (64-63)/2 = 0.
    CHECK(image[1 * 64 + 0] == idle_clock::MUTED);  // '0' first row 111.
    CHECK(image[1 * 64 + 62] == idle_clock::MUTED); // 'F' first row 111.
    render("ABCDEFGHIJKLMNOPQ"); // 17 cells: first 13 + '...' (16 cells, left = 0).
    CHECK(image[1 * 64 + 1] == idle_clock::MUTED);   // 'A' apex, row 010.
    CHECK(image[1 * 64 + 0] == 0);
    CHECK(image[2 * 64 + 52] == 0);            // 14th letter must not be drawn.
    CHECK(image[5 * 64 + 53] == idle_clock::MUTED); // '.' dots in cells 13..15.
    CHECK(image[5 * 64 + 61] == idle_clock::MUTED);
  } else if (test == "nosession") {
    // S5 staleness: more than 15 s without an accepted image or successful
    // control turns the title phase into *NO SESSION*; the next one restores.
    idle_clock::State state;
    CHECK(std::string(state.sessionTitle(0)) == ""); // Boot: date in both phases.
    state.commitTitle(1000, true, "LAB");
    CHECK(std::string(state.sessionTitle(1000)) == "LAB");
    CHECK(std::string(state.sessionTitle(16000)) == "LAB");   // 15 s: not stale.
    CHECK(std::string(state.sessionTitle(16001)) == "*NO SESSION*");
    state.touchTitle(20000); // Successful control restores the stored name.
    CHECK(std::string(state.sessionTitle(20000)) == "LAB");
    CHECK(std::string(state.sessionTitle(35001)) == "*NO SESSION*");
    state.commitTitle(40000, false, ""); // Accepted image without header clears.
    CHECK(std::string(state.sessionTitle(40001)) == "");
    state.commitTitle(41000, true, "NEW");
    CHECK(std::string(state.sessionTitle(41000)) == "NEW");
    state.pngResult(false, 50000); // Rejected traffic never refreshes liveness.
    CHECK(std::string(state.sessionTitle(56001)) == "*NO SESSION*");
    // The stale marker renders in the odd-phase band: '*NO SESSION*' is 12
    // cells, centered at left = (64 - 47) / 2 = 8.
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 10; calendar.tm_mday = 9;
    std::array<uint16_t, 64 * 64> image;
    image.fill(0xffff);
    idle_clock::draw(calendar, 4, 5000, state.sessionTitle(56001),
                     [&](int x, int y, uint16_t color) { image[y * 64 + x] = color; });
    CHECK(image[2 * 64 + 8] == idle_clock::MUTED); // '*' row 101 at x=8, 10.
    CHECK(image[2 * 64 + 10] == idle_clock::MUTED);
    CHECK(image[1 * 64 + 13] == 0);                // Date replaced (blank in title).
    image.fill(0xffff);
    idle_clock::draw(calendar, 4, 10000, state.sessionTitle(56001),
                     [&](int x, int y, uint16_t color) { image[y * 64 + x] = color; });
    CHECK(image[1 * 64 + 12] == idle_clock::MUTED); // Even phase keeps the date.
  } else if (test == "h24identical") {
    // S9: the 24 h rendering is byte-identical to today: the legacy draw()
    // signatures and the format-taking overloads with H24 produce the exact
    // same canvases, and labels() keeps its exact strings.
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 11; calendar.tm_mday = 31;
    std::array<uint16_t, 64 * 64> legacy, formatted, legacyTitle, formattedTitle;
    for (const int hour : {0, 9, 12, 13, 23}) {
      calendar.tm_hour = hour; calendar.tm_min = 34; calendar.tm_sec = 56;
      legacy.fill(0xffff); formatted.fill(0xffff);
      idle_clock::draw(calendar, 4, [&](int x, int y, uint16_t color) {
        legacy[y * 64 + x] = color;
      });
      idle_clock::draw(calendar, 4, [&](int x, int y, uint16_t color) {
        formatted[y * 64 + x] = color;
      }, idle_clock::ClockFormat::H24);
      CHECK(legacy == formatted);
      legacyTitle.fill(0xffff); formattedTitle.fill(0xffff);
      idle_clock::draw(calendar, 4, 5000, "LAB", [&](int x, int y, uint16_t color) {
        legacyTitle[y * 64 + x] = color;
      });
      idle_clock::draw(calendar, 4, 5000, "LAB", [&](int x, int y, uint16_t color) {
        formattedTitle[y * 64 + x] = color;
      }, idle_clock::ClockFormat::H24);
      CHECK(legacyTitle == formattedTitle);
      const idle_clock::Labels a = idle_clock::labels(calendar);
      const idle_clock::Labels b =
          idle_clock::labels(calendar, idle_clock::ClockFormat::H24);
      CHECK(std::string(a.date) == std::string(b.date) &&
            std::string(a.hours) == std::string(b.hours) &&
            std::string(a.minutes) == std::string(b.minutes) &&
            std::string(a.seconds) == std::string(b.seconds));
      CHECK(b.meridiem[0] == '\0'); // H24 records no meridiem label.
    }
  } else if (test == "clock12") {
    // S9: 12 h labels: the hours go 12, 1..11 (never zero-padded) with the
    // AM/PM label carried by the extended Labels record.
    std::tm calendar = {};
    calendar.tm_year = 126; calendar.tm_mon = 10; calendar.tm_mday = 9;
    calendar.tm_min = 41; calendar.tm_sec = 17;
    auto twelve = [&](unsigned hour) {
      calendar.tm_hour = static_cast<int>(hour);
      const idle_clock::Labels labels =
          idle_clock::labels(calendar, idle_clock::ClockFormat::H12);
      return std::string(labels.meridiem) + " " + labels.hours;
    };
    CHECK(twelve(0) == "AM 12" && twelve(1) == "AM 1" && twelve(11) == "AM 11");
    CHECK(twelve(12) == "PM 12" && twelve(13) == "PM 1" && twelve(23) == "PM 11");
    // AM/PM geometry, hour = 9 (H24 shows "09", H12 "9"): the 3x5 scale-1
    // label sits above the seconds block at the seconds' x (46), its
    // baseline row 28 exactly the scale-4 hour digits' baseline (9 + 5*4 - 1).
    calendar.tm_hour = 9;
    std::array<uint16_t, 64 * 64> h24, h12;
    auto render = [&](idle_clock::ClockFormat format,
                      std::array<uint16_t, 64 * 64> &out) {
      out.fill(0xffff);
      idle_clock::draw(calendar, 4, [&](int x, int y, uint16_t color) {
        out[y * 64 + x] = color;
      }, format);
    };
    render(idle_clock::ClockFormat::H24, h24);
    render(idle_clock::ClockFormat::H12, h12);
    // Right alignment: a single-digit hour keeps the right edge a two-digit
    // hour has, so it hugs the colon instead of leaving the gap on its right.
    auto bandEdge = [](const std::array<uint16_t, 64 * 64> &img, bool rightmost) {
      int edge = rightmost ? -1 : 64;
      for (int y = 9; y <= 28; ++y) {
        for (int x = 12; x < 46; ++x) {
          if (img[y * 64 + x] != idle_clock::WHITE) continue;
          if (rightmost) { if (x > edge) edge = x; }
          else if (x < edge) edge = x;
        }
      }
      return edge;
    };
    CHECK(bandEdge(h24, false) < 28);   // "09" starts in the left cell.
    CHECK(bandEdge(h12, false) >= 28);  // "9" starts in the right cell.
    CHECK(bandEdge(h12, true) == bandEdge(h24, true)); // same right edge.
    CHECK(h12[9 * 64 + 12] == 0);       // the old left cell is now empty.
    CHECK(h12[24 * 64 + 47] == idle_clock::MUTED); // 'A' row0 010.
    CHECK(h12[26 * 64 + 46] == idle_clock::MUTED); // 'A' row2 111.
    CHECK(h12[24 * 64 + 50] == idle_clock::MUTED); // 'M' row0 101.
    CHECK(h12[24 * 64 + 52] == idle_clock::MUTED);
    CHECK(h12[28 * 64 + 50] == idle_clock::MUTED); // 'M' row4 101, baseline row.
    // The baseline row only carries the label's own legs: 'A' 101 lights
    // x46/48 and 'M' 101 lights x50/52; the gaps stay dark.
    CHECK(h12[28 * 64 + 47] == 0 && h12[28 * 64 + 49] == 0 &&
          h12[28 * 64 + 51] == 0);
    // The gap between the label (baseline 28) and the seconds (start 33).
    for (int y = 29; y <= 32; ++y) for (int x = 46; x <= 59; ++x)
      CHECK(h12[y * 64 + x] == 0);
    // The label tracks the hour: 22:41 -> "PM": 'P' {6,5,6,4,4} row0 110
    // lights x46/47 at cell0, 'M' {5,7,7,5,5} row0 101 lights x50/52.
    calendar.tm_hour = 22;
    render(idle_clock::ClockFormat::H12, h12);
    CHECK(h12[24 * 64 + 46] == idle_clock::MUTED &&
          h12[24 * 64 + 47] == idle_clock::MUTED);
    CHECK(h12[24 * 64 + 48] == 0);
    CHECK(h12[24 * 64 + 50] == idle_clock::MUTED &&
          h12[24 * 64 + 52] == idle_clock::MUTED);
    // Every difference from the 24 h canvas stays inside the hour-digit
    // block (x12..45, rows 9..28) plus the label cells (x46..59, rows
    // 24..28) — the date band, minutes, colon, seconds, walkers and path
    // never move.
    render(idle_clock::ClockFormat::H24, h24);
    bool moved = false;
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
      const uint16_t plain = h24[y * 64 + x], label = h12[y * 64 + x];
      if (plain == label) continue;
      moved = true;
      const bool hourBand = y >= 9 && y <= 28 && x >= 12 && x <= 45;
      const bool meridiem = y >= 24 && y <= 28 && x >= 46 && x <= 59;
      CHECK(hourBand || meridiem);
    }
    CHECK(moved);
    // The 12 h hours never zero-pad, and a single digit is right aligned: at
    // 9 o'clock H24 draws "09" across both cells, while H12 draws "9" in
    // the right cell and leaves the left one empty, hugging the colon.
    calendar.tm_hour = 9;
    render(idle_clock::ClockFormat::H24, h24);
    render(idle_clock::ClockFormat::H12, h12);
    bool leftInkedInH24 = false, leftEmptyInH12 = true, rightInkedInH12 = false;
    for (int x = 12; x <= 27; ++x) {
      leftInkedInH24 |= h24[9 * 64 + x] != 0;
      leftEmptyInH12 &= h12[9 * 64 + x] == 0;
    }
    for (int x = 28; x <= 43; ++x) rightInkedInH12 |= h12[9 * 64 + x] != 0;
    CHECK(leftInkedInH24 && leftEmptyInH12 && rightInkedInH12);
  } else return 2;
  std::cout << "OK\n";
}
