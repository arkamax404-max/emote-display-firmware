#pragma once
#include <cstring>
#include "display_settings.h"

namespace firmware {
constexpr const char *VERSION = "v1.0.0";

// Add only the READY footer; leave existing diagnostic rendering untouched.
template <typename Display>
void drawVersion(Display &display, const char *first, const char *second) {
  if (std::strcmp(first, "READY") != 0 || std::strcmp(second, "SEND PNG") != 0) return;
  display.setTextSize(1);
  display.setTextColor(panel::color(display.color565(128, 128, 128)));
  display.setCursor(2, 44 + panel::TOP_OFFSET);
  display.print(VERSION);
}
} // namespace firmware
