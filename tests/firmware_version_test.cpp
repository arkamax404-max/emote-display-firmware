#include "firmware_version.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

struct FakeDisplay {
  std::vector<std::string> calls;
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t((r >> 3) << 11) | uint16_t((g >> 2) << 5) | (b >> 3);
  }
  void setTextSize(int size) { calls.push_back("size:" + std::to_string(size)); }
  void setTextColor(uint16_t color) { calls.push_back("color:" + std::to_string(color)); }
  void setCursor(int x, int y) { calls.push_back("cursor:" + std::to_string(x) + "," + std::to_string(y)); }
  void print(const char *text) { calls.push_back(text); }
};

int main() {
  FakeDisplay display;
  firmware::drawVersion(display, "READY", "SEND PNG");
  const std::vector<std::string> expected = {
      "size:1", "color:" + std::to_string(panel::color(display.color565(128, 128, 128))),
      "cursor:2," + std::to_string(44 + panel::TOP_OFFSET), "v1.2.0-dbg"};
  assert(display.calls == expected);
  // Debug identity is 10 ASCII cells; setTextSize(1) renders 6 px per
  // cell, so the footer at x=2 must still fit the 64 px panel width.
  assert(std::strlen(firmware::VERSION) == 10);
  assert(2 + 10 * 6 <= panel::SIZE);
  assert(44 + panel::TOP_OFFSET + 8 <= panel::SIZE);
  for (const auto &message : std::vector<std::pair<const char *, const char *>>{
           {"SET WIFI", "VIA USB"}, {"WIFI", "CONNECT"}, {"", ""},
           {"READY", "CONNECT"}, {"WIFI", "SEND PNG"}}) {
    display.calls = {"unchanged"};
    firmware::drawVersion(display, message.first, message.second);
    assert(display.calls == std::vector<std::string>{"unchanged"});
  }
  std::cout << "OK\n";
}
