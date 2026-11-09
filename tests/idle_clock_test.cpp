#define local static // PNGdec zutil.h exports this macro in the target translation unit.
#include "idle_clock.h"
#include "idle_clock_image.h"
#include <array>
#include <cstring>
#include <iostream>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "line " << __LINE__ << ": " #condition "\n"; return 1; } } while (false)
using idle_clock::Source;

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
    CHECK(!state.redrawDue(999, Source::Clock));
    CHECK(state.redrawDue(1000, Source::Clock));
    CHECK(state.redrawDue(1, Source::Frame));
    state.painted(UINT32_MAX - 500, Source::Clock);
    CHECK(!state.redrawDue(498, Source::Clock));
    CHECK(state.redrawDue(499, Source::Clock));
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
    std::array<uint16_t, 64 * 61> image;
    image.fill(0xffff);
    bool bounds = true;
    auto pixel = [&](int x, int y, uint16_t color) {
      if (x < 0 || x >= 64 || y < 0 || y >= 61) bounds = false;
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
    CHECK(image[52 * 64 + 2] == idle_clock::GREEN);
    CHECK(image[52 * 64 + 42] == idle_clock::GREEN);
    CHECK(image[59 * 64 + 2] == idle_clock::WHITE);
    CHECK(image[60 * 64 + 0] == idle_clock::GREEN);
    CHECK(image[60 * 64 + 4] == idle_clock::PATH);
    idle_clock::draw(calendar, 5, pixel);
    CHECK(bounds && image[59 * 64 + 3] == idle_clock::WHITE);
    CHECK(image[59 * 64 + 7] == idle_clock::WHITE);
    CHECK(image[59 * 64 + 2] == 0); // Previous feet were cleared.
    for (uint64_t step = 0; step < 80; ++step) idle_clock::draw(calendar, step, pixel);
    CHECK(bounds); // Both walkers clip safely at both edges in both gaits.
  } else return 2;
  std::cout << "OK\n";
}
