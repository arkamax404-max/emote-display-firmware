// Host test CLI for the S8-S10 panel-settings surface (T11-T13): the
// runtime brightness value rules (include/display_settings.h), the 12/24 h
// clock format (include/idle_clock.h), the /config form parsing and page
// rendering (include/panel_settings.h) and the bounded header extraction
// the station serving loop reuses. One command per line on stdin; values in
// hex with '-' as the empty value, like tests/portal_test.cpp:
//
//   setb <p|default>  panel::setBrightnessPercent → "setb <ok|reject> p=<n> s=<n>"
//   get               → "get p=<percent> s=<scale>"
//   color <hex>       → "color <hex out>" through the live scale
//   scale <p>         → "scale <n>" (percent * 255 / 100)
//   valid <p>         → "valid <1|0>"
//   bright <hex>      reviewBrightness → "bright <error|ok <n>>"
//   clock <hex>       reviewClock → "clock <error|ok <24h|12h>>"
//   form <hex>        parseSettingsForm → "form <error|ok b=<v> c=<v>>"
//   route <hex>       routeOf(first line) → "route <other|get|save>"
//   length <hex>      parseContentLength → "length <n|-1>"
//   page <p> <24h|12h> <errhex> → "page <escaped html>"
//   saved            → "saved <escaped html>" (the success page)
#include "display_settings.h"
#include "panel_settings.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string unhex(const std::string &hex) {
  if (hex == "-") return {}; // convention: '-' means an empty value
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

std::string escape(const std::string &text) {
  std::string out;
  char buf[8];
  for (const unsigned char c : text) {
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c >= 0x20 && c <= 0x7e) out += static_cast<char>(c);
    else {
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}
} // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "setb") {
      std::string value;
      args >> value;
      const uint8_t percent = value == "default"
          ? panel::BRIGHTNESS_DEFAULT_PERCENT
          : static_cast<uint8_t>(std::strtoul(value.c_str(), nullptr, 10));
      panel::setBrightnessPercent(percent);
      std::cout << "setb "
                << (panel::brightnessPercent() == percent &&
                            panel::validBrightnessPercent(percent) ? "ok" : "reject")
                << " p=" << unsigned(panel::brightnessPercent())
                << " s=" << unsigned(panel::brightnessScale()) << '\n';
    } else if (cmd == "get") {
      std::cout << "get p=" << unsigned(panel::brightnessPercent())
                << " s=" << unsigned(panel::brightnessScale()) << '\n';
    } else if (cmd == "color") {
      std::string hex;
      args >> hex;
      const std::string raw = unhex(hex);
      const uint16_t rgb565 = raw.size() == 2
          ? static_cast<uint16_t>((static_cast<unsigned char>(raw[0]) << 8) |
                                  static_cast<unsigned char>(raw[1]))
          : 0;
      char out[8];
      std::snprintf(out, sizeof(out), "%04x", panel::color(rgb565));
      std::cout << "color " << out << '\n';
    } else if (cmd == "scale") {
      unsigned percent = 0;
      args >> percent;
      std::cout << "scale "
                << unsigned(panel::brightnessScaleFromPercent(
                       static_cast<uint8_t>(percent))) << '\n';
    } else if (cmd == "valid") {
      unsigned percent = 0;
      args >> percent;
      std::cout << "valid "
                << (panel::validBrightnessPercent(static_cast<uint8_t>(percent)) ? 1 : 0)
                << '\n';
    } else if (cmd == "bright") {
      std::string hex;
      args >> hex;
      const panel_settings::BrightnessChoice choice =
          panel_settings::reviewBrightness(unhex(hex));
      if (choice.ok) std::cout << "bright ok " << unsigned(choice.percent) << '\n';
      else std::cout << "bright error\n";
    } else if (cmd == "clock") {
      std::string hex;
      args >> hex;
      const panel_settings::ClockChoice choice =
          panel_settings::reviewClock(unhex(hex));
      if (choice.ok)
        std::cout << "clock ok " << panel_settings::clockName(choice.format) << '\n';
      else std::cout << "clock error\n";
    } else if (cmd == "form") {
      std::string hex;
      args >> hex;
      const std::string body = unhex(hex);
      const panel_settings::SettingsFields fields =
          panel_settings::parseSettingsForm(body.data(), body.size());
      if (!fields.ok) std::cout << "form error\n";
      else std::cout << "form ok b=" << fields.brightness
                     << " c=" << fields.clock << '\n';
    } else if (cmd == "route") {
      std::string hex;
      args >> hex;
      const panel_settings::Route route = panel_settings::routeOf(unhex(hex));
      std::cout << "route "
                << (route == panel_settings::Route::Get ? "get"
                    : route == panel_settings::Route::Save ? "save" : "other")
                << '\n';
    } else if (cmd == "length") {
      std::string hex;
      args >> hex;
      std::cout << "length " << panel_settings::parseContentLength(unhex(hex)) << '\n';
    } else if (cmd == "page") {
      std::string formatHex, errorHex;
      unsigned percent = 0;
      args >> percent >> formatHex >> errorHex;
      const idle_clock::ClockFormat format = formatHex == "12h"
          ? idle_clock::ClockFormat::H12 : idle_clock::ClockFormat::H24;
      std::cout << "page "
                << escape(panel_settings::configPage(
                       static_cast<uint8_t>(percent), format, unhex(errorHex)))
                << '\n';
    } else if (cmd == "saved") {
      std::cout << "saved " << escape(panel_settings::configSavedPage()) << '\n';
    }
  }
  return 0;
}
