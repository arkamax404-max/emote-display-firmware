// Host test CLI for the S7-S10 panel-settings surface (T10 + T11-T13 extendida): the
// runtime brightness value rules (include/display_settings.h), the 12/24 h
// clock format (include/idle_clock.h), the six-way panel channel order
// (include/rgb_order.h), the /config form parsing and page rendering
// (include/panel_settings.h) and the bounded header extraction
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
//   order <hex>       reviewOrder → "order <error|ok <NAME>>"
//   form <hex>        parseSettingsForm → "form <error|ok b=<v> c=<v> o=<v>>"
//   route <hex>       routeOf(first line) → "route <other|get|save>"
//   length <hex>      parseContentLength → "length <n|-1>"
//   page <p> <24h|12h> <order> <errhex>
//                     configPage → "page <escaped html>" (current order
//                     preselected on the six-option order select)
//   saved [restart]   configSavedPage → "saved <escaped html>"; the restart
//                     variant is the page served when the order changed
//   store <present> <fail> [<p>] [<c>] [<o>]
//                     panel_settings::storeSettings over a fake store →
//                     "store <ok|fail> b=<n|-> c=<n|-> o=<n|->" (the stored
//                     triple after the call, '-' = absent). <present>
//                     bit0/bit1/bit2: brightness/clockformat/rgborder keys
//                     exist up front (defaults 30, 0 = H24 and 5 = BGR).
//                     <fail> bits, for key b/c/o (k = 0/1/2):
//                     bit(4k+0) commit stores nothing,
//                     bit(4k+1) commit persists value^1,
//                     bit(4k+2) rollback stores nothing,
//                     bit(4k+3) rollback persists value^1,
//                     bit(12+k): a rollback removal of the key does not
//                     take effect (removeUChar keeps the key).
//                     [<p>] [<c>] [<o>] = the offered percent, clock code
//                     and order code (defaults 80, 1 = H12, 0 = RGB).
//   storew <present> <fail> [<s>] [<w>] [<u>]
//                     storeWeatherSettings over a fake mixed store, same
//                     fail-bit scheme for clocksecs/weathersecs/units →
//                     "storew <ok|fail> s=<n|-> w=<n|-> u=<n|->". <present>
//                     bitmap seeds clocksecs=20 / weathersecs=10 / units=0.
//                     [<s>] [<w>] [<u>] = the offered seconds (defaults
//                     60/30) and unit code 0/1 (default 1 = Fahrenheit).
//   storec <present> <fail> [<lat>] [<lon>] [<namehex>]
//                     storeCity over the fake mixed store, same fail-bit
//                     scheme for citylat/citylon/cityname (a string lie
//                     appends 'X', an int lie flips bit 0) →
//                     "storec <ok|fail> lat=<n|-> lon=<n|-> name=<v|->".
//                     <present> bitmap seeds 37382830 / -5973170 / Sevilla.
//                     Defaults offered: 40416775 / -3703790 / Madrid.
//   seconds <hex>     reviewSeconds → "seconds <error|ok <n>>"
//   unitsrev <hex>    reviewUnits → "unitsrev <error|ok <C|F>>"
//   csdef <n> | wsdef <n> | udef <n>
//                     the stored-value fallback loaders → their resolved
//                     values (udef <C|F>)
//   citydef [<present> [<lat> [<lon> [<namehex>]]]]
//                     storedCityOrDefault → "citydef ok lat=<n> lon=<n>
//                     name=<v>" (always a usable record: the default city
//                     whenever the stored state is incomplete/absent)
//   cityform <hex>    parseCityForm → "cityform <error|ok city=<v>>"
//   citysearch <hex>  reviewCitySearch → "citysearch <error|ready>" (the
//                     rejection that happens BEFORE any request could run)
//   cityfrom <status> <bodyhex>
//                     cityFromResponse → "cityfrom <error|ok lat=<n>
//                     lon=<n> name=<v>"; a negative status fakes a
//                     transport-level error (ok=false)
//   pagew <p> <24h|12h> <order> <C|F> <clocksecs> <weathersecs> <citynamehex> <errhex>
//                     the extended configPage → "pagew <escaped html>"
#include "display_settings.h"
#include "rgb_order.h"
#include "panel_settings.h"
#include "weather.h"
#include "weather_fetch.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
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

// Fake store for the S7/S9/S10 persistence transaction: writes are counted per
// key (first write = commit, further writes = its rollback) so the fail mask
// can target a commit while leaving the rollback healthy, and every write can
// either store nothing (a write that cannot be persisted — detected only by
// the transaction's read-back) or persist value^1 (a write that lies).
class FakeSettingsStore : public panel_settings::SettingsStore {
 public:
  FakeSettingsStore(unsigned present, unsigned fail) : fail_(fail) {
    if (present & 1u) bytes_["brightness"] = panel::BRIGHTNESS_DEFAULT_PERCENT;
    if (present & 2u) bytes_["clockformat"] = 0; // 0 = H24, a valid stored code
    if (present & 4u) bytes_["rgborder"] = 5;    // 5 = BGR, a stored snapshot
  }
  bool hasUChar(const char *key) override { return bytes_.count(key) != 0; }
  bool readUChar(const char *key, uint8_t &value) override {
    const auto it = bytes_.find(key);
    if (it == bytes_.end()) return false;
    value = it->second;
    return true;
  }
  void writeUChar(const char *key, uint8_t value) override {
    const unsigned keyIndex = keyIndex_(key);
    const bool rollback = writes_[key]++ != 0;
    const unsigned base = 4u * keyIndex + (rollback ? 2u : 0u);
    if (fail_ & (1u << base)) return; // write stores nothing
    if (fail_ & (1u << (base + 1))) value = static_cast<uint8_t>(value ^ 1u);
    bytes_[key] = value;
  }
  void removeUChar(const char *key) override {
    if (fail_ & (1u << (12u + keyIndex_(key)))) return;
    bytes_.erase(key);
  }

 private:
  static unsigned keyIndex_(const char *key) {
    return std::strcmp(key, "brightness") == 0 ? 0u
        : std::strcmp(key, "clockformat") == 0 ? 1u
                                               : 2u; // rgborder
  }
  std::map<std::string, uint8_t> bytes_;
  std::map<std::string, unsigned> writes_;
  unsigned fail_;
};

// Fake mixed store for the T4a transactions: covers the weather-seconds
// triple (clocksecs/weathersecs/units, all uchar) and the city triple
// (citylat/citylon int32 plus the cityname string) with the SAME fail-bit
// scheme per transaction key; a string lie appends 'X', an int lie flips
// bit 0. Removal is type-agnostic, exactly like Preferences::remove.
class FakeMixedStore : public panel_settings::SettingsStore {
 public:
  FakeMixedStore(int kind, unsigned present, unsigned fail)
      : kind_(kind), fail_(fail) {
    if (kind_ == 0) {
      if (present & 1u) bytes_["clocksecs"] = 20;
      if (present & 2u) bytes_["weathersecs"] = 10;
      if (present & 4u) bytes_["units"] = 0;
    } else {
      if (present & 1u) ints_["citylat"] = 37382830;
      if (present & 2u) ints_["citylon"] = -5973170;
      if (present & 4u) strings_["cityname"] = "Sevilla";
    }
  }
  bool hasUChar(const char *key) override { return bytes_.count(key) != 0; }
  bool readUChar(const char *key, uint8_t &value) override {
    const auto it = bytes_.find(key);
    if (it == bytes_.end()) return false;
    value = it->second;
    return true;
  }
  void writeUChar(const char *key, uint8_t value) override {
    const unsigned k = keyIndex_(key);
    const bool rollback = writes_[key]++ != 0;
    if (storeNothing_(k, rollback)) return;
    bytes_[key] = lie_(k, rollback) ? static_cast<uint8_t>(value ^ 1u) : value;
  }
  bool hasInt32(const char *key) override { return ints_.count(key) != 0; }
  bool readInt32(const char *key, int32_t &value) override {
    const auto it = ints_.find(key);
    if (it == ints_.end()) return false;
    value = it->second;
    return true;
  }
  void writeInt32(const char *key, int32_t value) override {
    const unsigned k = keyIndex_(key);
    const bool rollback = writes_[key]++ != 0;
    if (storeNothing_(k, rollback)) return;
    ints_[key] = lie_(k, rollback) ? value | 1 : value;
  }
  bool hasString(const char *key) override { return strings_.count(key) != 0; }
  bool readString(const char *key, std::string &value) override {
    const auto it = strings_.find(key);
    if (it == strings_.end()) return false;
    value = it->second;
    return true;
  }
  void writeString(const char *key, const std::string &value) override {
    const unsigned k = keyIndex_(key);
    const bool rollback = writes_[key]++ != 0;
    if (storeNothing_(k, rollback)) return;
    strings_[key] = lie_(k, rollback) ? value + "X" : value;
  }
  void removeUChar(const char *key) override {
    if (fail_ & (1u << (12u + keyIndex_(key)))) return;
    bytes_.erase(key);
    ints_.erase(key);
    strings_.erase(key);
  }

 private:
  static unsigned keyIndex_(const char *key) {
    return std::strcmp(key, "clocksecs") == 0 ||
                   std::strcmp(key, "citylat") == 0
               ? 0u
           : std::strcmp(key, "weathersecs") == 0 ||
                     std::strcmp(key, "citylon") == 0
               ? 1u
               : 2u; // units / cityname
  }
  bool storeNothing_(int k, bool rollback) const {
    return (fail_ & (1u << (4u * k + (rollback ? 2u : 0u)))) != 0;
  }
  bool lie_(int k, bool rollback) const {
    return (fail_ & (1u << (4u * k + (rollback ? 3u : 1u)))) != 0;
  }
  int kind_;
  std::map<std::string, uint8_t> bytes_;
  std::map<std::string, int32_t> ints_;
  std::map<std::string, std::string> strings_;
  std::map<std::string, unsigned> writes_;
  unsigned fail_;
};
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
      else
        std::cout << "clock error\n";
    } else if (cmd == "order") {
      std::string hex;
      args >> hex;
      const panel_settings::OrderChoice choice =
          panel_settings::reviewOrder(unhex(hex));
      if (choice.ok)
        std::cout << "order ok " << rgb_order::orderName(choice.value) << '\n';
      else
        std::cout << "order error\n";
    } else if (cmd == "form") {
      std::string hex;
      args >> hex;
      const std::string body = unhex(hex);
      const panel_settings::SettingsFields fields =
          panel_settings::parseSettingsForm(body.data(), body.size());
      if (!fields.ok) std::cout << "form error\n";
      else std::cout << "form ok b=" << fields.brightness
                     << " c=" << fields.clock << " o=" << fields.order
                     << " s=" << fields.clocksecs
                     << " w=" << fields.weathersecs
                     << " u=" << fields.units << '\n';
    } else if (cmd == "route") {
      std::string hex;
      args >> hex;
      const panel_settings::Route route = panel_settings::routeOf(unhex(hex));
      std::cout << "route "
                << (route == panel_settings::Route::Get ? "get"
                    : route == panel_settings::Route::Save ? "save"
                    : route == panel_settings::Route::CitySave ? "city"
                                                              : "other")
                << '\n';
    } else if (cmd == "length") {
      std::string hex;
      args >> hex;
      std::cout << "length " << panel_settings::parseContentLength(unhex(hex)) << '\n';
    } else if (cmd == "page") {
      std::string formatHex, orderHex, errorHex;
      unsigned percent = 0;
      args >> percent >> formatHex >> orderHex >> errorHex;
      const idle_clock::ClockFormat format = formatHex == "12h"
          ? idle_clock::ClockFormat::H12 : idle_clock::ClockFormat::H24;
      rgb_order::Order order = rgb_order::Order::RGB;
      if (!rgb_order::parseOrder(orderHex, order))
        order = rgb_order::Order::RGB;
      if (errorHex.empty()) errorHex = "-";
      std::cout << "page "
                << escape(panel_settings::configPage(
                       static_cast<uint8_t>(percent), format, order,
                       unhex(errorHex)))
                << '\n';
    } else if (cmd == "store") {
      unsigned present = 0, fail = 0, percent = 80, code = 1, orderCode = 0;
      args >> present >> fail;
      if (args >> percent) {
        args >> code;
        if (!(args >> orderCode)) orderCode = 0;
      }
      FakeSettingsStore store(present, fail);
      const idle_clock::ClockFormat format = code == 1
          ? idle_clock::ClockFormat::H12 : idle_clock::ClockFormat::H24;
      rgb_order::Order order = rgb_order::Order::RGB;
      if (!rgb_order::orderFromCode(static_cast<uint8_t>(orderCode & 7u), order))
        order = rgb_order::Order::RGB;
      const bool ok = panel_settings::storeSettings(store,
          static_cast<uint8_t>(percent), format, order);
      uint8_t brightness = 0, clock = 0, orderBack = 0;
      const bool hasBrightness = store.readUChar("brightness", brightness);
      const bool hasClock = store.readUChar("clockformat", clock);
      const bool hasOrder = store.readUChar("rgborder", orderBack);
      std::cout << "store " << (ok ? "ok" : "fail")
                << " b=" << (hasBrightness ? std::to_string(brightness) : "-")
                << " c=" << (hasClock ? std::to_string(clock) : "-")
                << " o=" << (hasOrder ? std::to_string(orderBack) : "-") << '\n';
    } else if (cmd == "saved") {
      std::string variant;
      args >> variant;
      std::cout << "saved "
                << escape(panel_settings::configSavedPage(variant == "restart"))
                << '\n';
    } else if (cmd == "seconds") {
      std::string hex;
      args >> hex;
      const panel_settings::SecondsChoice choice =
          panel_settings::reviewSeconds(unhex(hex));
      if (choice.ok) std::cout << "seconds ok " << unsigned(choice.seconds) << '\n';
      else std::cout << "seconds error\n";
    } else if (cmd == "unitsrev") {
      std::string hex;
      args >> hex;
      const panel_settings::UnitChoice choice =
          panel_settings::reviewUnits(unhex(hex));
      if (choice.ok)
        std::cout << "unitsrev ok "
                  << (choice.unit == weather::Unit::Fahrenheit ? "F" : "C")
                  << '\n';
      else
        std::cout << "unitsrev error\n";
    } else if (cmd == "csdef") {
      unsigned stored = 0;
      args >> stored;
      std::cout << "csdef "
                << unsigned(panel_settings::clockSecondsOrDefault(
                       static_cast<uint8_t>(stored)))
                << '\n';
    } else if (cmd == "wsdef") {
      unsigned stored = 0;
      args >> stored;
      std::cout << "wsdef "
                << unsigned(panel_settings::weatherSecondsOrDefault(
                       static_cast<uint8_t>(stored)))
                << '\n';
    } else if (cmd == "udef") {
      unsigned stored = 0;
      args >> stored;
      const weather::Unit unit = panel_settings::unitOrDefault(
          static_cast<uint8_t>(stored));
      std::cout << "udef " << (unit == weather::Unit::Fahrenheit ? "F" : "C")
                << '\n';
    } else if (cmd == "citydef") {
      unsigned present = 0;
      int32_t lat = 37382830, lon = -5973170;
      std::string nameHex = "-", name = "Sevilla";
      if (args >> present) {
        if (!(args >> lat)) { lat = 37382830; lon = -5973170; }
        else if (!(args >> lon)) { lon = -5973170; }
        else {
          if (args >> nameHex) name = unhex(nameHex);
          else name = "Sevilla";
        }
      }
      const weather::CityRecord city = panel_settings::storedCityOrDefault(
          (present & 1u) != 0, lat, lon, name);
      std::cout << "citydef ok lat=" << city.latMicro
                << " lon=" << city.lonMicro << " name=" << escape(city.name)
                << '\n';
    } else if (cmd == "cityform") {
      std::string hex;
      args >> hex;
      const std::string body = unhex(hex);
      const panel_settings::CitySearchFields fields =
          panel_settings::parseCityForm(body.data(), body.size());
      if (!fields.ok) std::cout << "cityform error\n";
      else std::cout << "cityform ok city=" << escape(fields.city) << '\n';
    } else if (cmd == "citysearch") {
      std::string hex;
      args >> hex;
      std::cout << "citysearch "
                << (panel_settings::reviewCitySearch(unhex(hex)) ? "ready"
                                                                 : "error")
                << '\n';
    } else if (cmd == "cityfrom") {
      int status = 200;
      std::string bodyHex;
      args >> status >> bodyHex;
      weather_fetch::Transport::Response response;
      response.status = status;
      if (status >= 0) response.ok = true;
      response.body = unhex(bodyHex);
      weather::CityRecord city;
      if (panel_settings::cityFromResponse(response, city))
        std::cout << "cityfrom ok lat=" << city.latMicro
                  << " lon=" << city.lonMicro << " name=" << escape(city.name)
                  << '\n';
      else
        std::cout << "cityfrom error\n";
    } else if (cmd == "storew") {
      unsigned present = 0, fail = 0, secs = 60, weather = 30, unitCode = 1;
      args >> present >> fail;
      if (args >> secs) {
        args >> weather;
        if (!(args >> unitCode)) unitCode = 0;
      }
      FakeMixedStore store(0, present, fail);
      weather::Unit unit = weather::Unit::Celsius;
      if (!weather::unitFromCode(static_cast<uint8_t>(unitCode), unit))
        unit = weather::Unit::Celsius;
      const bool ok = panel_settings::storeWeatherSettings(
          store, static_cast<uint8_t>(secs), static_cast<uint8_t>(weather), unit);
      uint8_t s = 0, w = 0, u = 0;
      const bool hasS = store.readUChar("clocksecs", s);
      const bool hasW = store.readUChar("weathersecs", w);
      const bool hasU = store.readUChar("units", u);
      std::cout << "storew " << (ok ? "ok" : "fail")
                << " s=" << (hasS ? std::to_string(s) : "-")
                << " w=" << (hasW ? std::to_string(w) : "-")
                << " u=" << (hasU ? std::to_string(u) : "-") << '\n';
    } else if (cmd == "storec") {
      unsigned present = 0, fail = 0;
      int32_t lat = 40416775, lon = -3703790;
      std::string nameHex, name = "Madrid";
      args >> present >> fail;
      if (args >> lat) {
        if (!(args >> lon)) lon = -3703790;
        else if (args >> nameHex) name = unhex(nameHex);
      }
      FakeMixedStore store(1, present, fail);
      weather::CityRecord offered;
      offered.valid = true;
      offered.latMicro = lat;
      offered.lonMicro = lon;
      offered.name = name;
      const bool ok = panel_settings::storeCity(store, offered);
      int32_t storedLat = 0, storedLon = 0;
      std::string storedName;
      const bool hasLat = store.readInt32("citylat", storedLat);
      const bool hasLon = store.readInt32("citylon", storedLon);
      const bool hasName = store.readString("cityname", storedName);
      std::cout << "storec " << (ok ? "ok" : "fail")
                << " lat=" << (hasLat ? std::to_string(storedLat) : "-")
                << " lon=" << (hasLon ? std::to_string(storedLon) : "-")
                << " name=" << (hasName ? escape(storedName) : "-") << '\n';
    } else if (cmd == "pagew") {
      unsigned percent = 0, clockSecs = 20, weatherSecs = 10;
      std::string formatHex, orderHex, unitHex, cityHex, errorHex;
      args >> percent >> formatHex >> orderHex >> unitHex;
      if (!(args >> clockSecs)) clockSecs = 20;
      else {
        if (!(args >> weatherSecs)) weatherSecs = 10;
        else {
          if (!(args >> cityHex)) cityHex = "-";
          else {
            if (!(args >> errorHex)) errorHex = "-";
          }
        }
      }
      const idle_clock::ClockFormat format = formatHex == "12h"
          ? idle_clock::ClockFormat::H12 : idle_clock::ClockFormat::H24;
      rgb_order::Order order = rgb_order::Order::RGB;
      if (!rgb_order::parseOrder(orderHex, order)) order = rgb_order::Order::RGB;
      weather::Unit unit = weather::Unit::Celsius;
      if (!weather::unitFromCode(unitHex == "F" ? 1u : 0u, unit))
        unit = weather::Unit::Celsius;
      std::cout << "pagew "
                << escape(panel_settings::configPage(
                       static_cast<uint8_t>(percent), format, order, unit,
                       static_cast<uint8_t>(clockSecs),
                       static_cast<uint8_t>(weatherSecs), unhex(cityHex),
                       unhex(errorHex)))
                << '\n';
    }
  }
  return 0;
}
