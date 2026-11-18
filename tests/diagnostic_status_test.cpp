// Host test CLI for the T7 serial RX diagnostic indicator. One command per
// line on stdin; every command prints nothing except the explicit queries and
// the tick result, so timeline scripts assert exact observable state:
//
//   setnow <ms>            absolute monotonic now (wrap tests)
//   byte                   diag.onByte(now)
//   enter                  diag.onBinaryEnter(now)
//   exit                   diag.onBinaryExit(now)
//   image                  diag.onImageAccepted(now)
//   control                diag.onControlAccepted(now)
//   reject                 diag.onRejected(now)
//   tick <ms>              now += ms; changed = diag.update(now) → "tick <0|1>"
//   sync                   diag.update(now), result discarded (models the
//                          firmware loop redrawing and clearing dirty)
//   signal                 "signal <blank|amber|green|red>"
//   counters               "counters images=<i> controls=<c> rejected=<r>"
//   draw                   one "px x,y,color" per drawPixel + "draw <n>"
//   setcounters <i> <c> <r> preload raw counter values (saturation pin)
#include "diagnostic_status.h"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
const char *signalName(diag::Signal signal) {
  switch (signal) {
    case diag::Signal::Amber: return "amber";
    case diag::Signal::Green: return "green";
    case diag::Signal::Red: return "red";
    default: return "blank";
  }
}

// Recording stand-in for the rotated matrix: captures every drawPixel so the
// tests can pin all ten triangle coordinates, the colors and that nothing
// else in the framebuffer is written.
struct RecordingDisplay {
  struct Pixel { int x, y; uint16_t color; };
  std::vector<Pixel> pixels;
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t((r >> 3) << 11) | uint16_t((g >> 2) << 5) | (b >> 3);
  }
  void drawPixel(int x, int y, uint16_t color) { pixels.push_back({x, y, color}); }
};
} // namespace

int main() {
  diag::Status diag;
  RecordingDisplay display;
  uint32_t now = 1000;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "setnow") {
      args >> now;
    } else if (cmd == "byte") {
      diag.onByte(now);
    } else if (cmd == "enter") {
      diag.onBinaryEnter(now);
    } else if (cmd == "exit") {
      diag.onBinaryExit(now);
    } else if (cmd == "image") {
      diag.onImageAccepted(now);
    } else if (cmd == "control") {
      diag.onControlAccepted(now);
    } else if (cmd == "reject") {
      diag.onRejected(now);
    } else if (cmd == "tick") {
      uint32_t ms = 0;
      args >> ms;
      now += ms;
      std::cout << "tick " << (diag.update(now) ? 1 : 0) << '\n';
    } else if (cmd == "sync") {
      diag.update(now);
    } else if (cmd == "signal") {
      std::cout << "signal " << signalName(diag.signal()) << '\n';
    } else if (cmd == "counters") {
      std::cout << "counters images=" << diag.imageCount()
                << " controls=" << diag.controlCount()
                << " rejected=" << diag.rejectedCount() << '\n';
    } else if (cmd == "draw") {
      diag.draw(display);
      for (const auto &pixel : display.pixels)
        std::cout << "px " << pixel.x << ',' << pixel.y << ',' << pixel.color << '\n';
      std::cout << "draw " << display.pixels.size() << '\n';
      display.pixels.clear();
    } else if (cmd == "setcounters") {
      args >> diag.images.value >> diag.controls.value >> diag.rejected.value;
    }
  }
  return 0;
}
