#define local static // PNGdec zutil.h exports this macro in the target translation unit.
#include "idle_clock.h"
#include "idle_clock_image.h"
#include <array>
#include <cstring>
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
  } else return 2;
  std::cout << "OK\n";
}
