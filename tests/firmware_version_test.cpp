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
      "cursor:2,47", "v1.0.0"};
  assert(display.calls == expected);
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
