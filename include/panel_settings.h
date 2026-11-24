#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "portal_policy.h"    // portal::decodeUrlValue, the shared URL rules
#include "portal_page.h"      // portal_page::escapeHtml
#include "idle_clock.h"       // idle_clock::ClockFormat and its code mapping
#include "display_settings.h" // panel:: brightness percent rules (S10)
#include "rgb_order.h"        // rgb_order::Order, the six named permutations (S7)
#include "weather.h"          // T4a domains: the durations, the unit, the city record
#include "weather_fetch.h"    // weather_fetch::validQuery and the geocoding intake

// S8-S10 (T11-T13) + T4a: the LAN panel-settings surface served by the
// station HTTP server at GET/POST /config — pure, host-testable parsing and
// rendering in the header-only style of include/portal_policy.h (no
// Arduino/WiFi symbols). The value rules live with their domains: the
// brightness percent set and the percent->scale mapping in
// include/display_settings.h, the 12/24 h format and its storage code in
// include/idle_clock.h, and from T4a the weather durations, the temperature
// unit and the resolved city in include/weather.h (the API host and query
// rules in include/weather_fetch.h). The station serving loop decides the
// route on the exact request line BELOW this module (routeOf), outside the
// receiver protocol parser, so the receiver's own routes keep their exact
// semantics.
namespace panel_settings {

constexpr const char *CONFIG_PATH = "/config";

// The settings form body is tiny (two short keys); bounded like the portal
// forms so the station server's one-client, 2.5 s request limits always
// cover it.
constexpr size_t CONFIG_BODY_MAX = 255;

// S7/S9/S10 + T4a persistence transaction discipline: the settings are
// stored through an injected store and every decision is made by READ-BACK —
// a putUChar/putInt-style return value is never trusted and zero is a valid
// stored code (the H24 default and the RGB default order). Each transaction
// is atomic across the keys of its own surface (the panel triple, the T4a
// weather triple, the T4a city triple):
// - the commit writes are followed by a read-back; any miss triggers the
//   rollback,
// - every rollback write — including the failing key's own — is attempted
//   (even after an earlier rollback miss) and followed by its own read-back;
//   a rollback write that cannot be persisted or lies leaves the result
//   unverified,
// - the call reports success ONLY after a fully verified store, so the
//   caller applies the live values (repaints the panel; restarts only for an
//   actual order change) only then — a failed or unverified call changes no
//   live state, even when the stored bytes could not be perfectly restored.
// An absent stored state stays distinguishable from any stored value, so a
// rollback can faithfully restore "was not stored" for every key of its
// surface.
class SettingsStore {
 public:
  virtual ~SettingsStore() = default;
  virtual bool hasUChar(const char *key) = 0;
  // false = the key is absent or unreadable.
  virtual bool readUChar(const char *key, uint8_t &value) = 0;
  // Fire-and-verify writes: a failing store may persist nothing or
  // something else; the transaction decides only through its read-back.
  virtual void writeUChar(const char *key, uint8_t value) = 0;
  virtual void removeUChar(const char *key) = 0;
  // T4a typed extensions for the weather keys (int32 micro-degrees and the
  // city-name string). They carry DEFAULT FAILING implementations instead of
  // being pure, so adapters that do not override them keep compiling and the
  // T4a transactions honestly see an unwritable store: every read-back then
  // misses and no write is ever verified. (Removal stays type-agnostic: it
  // is removeUChar, the same key-erasing operation Preferences::remove is.)
  virtual bool hasInt32(const char *key) { (void)key; return false; }
  virtual bool readInt32(const char *key, int32_t &value) {
    (void)key; (void)value; return false;
  }
  virtual void writeInt32(const char *key, int32_t value) {
    (void)key; (void)value;
  }
  virtual bool hasString(const char *key) { (void)key; return false; }
  virtual bool readString(const char *key, std::string &value) {
    (void)key; (void)value; return false;
  }
  virtual void writeString(const char *key, const std::string &value) {
    (void)key; (void)value;
  }
};

// The shared commit/rollback core of the byte transactions (S7/S9/S10 and
// the T4a weather triple): identical discipline, identical write order,
// absence fidelity and read-back-only decisions for every surface.
namespace detail {
struct UCharKey { const char *name; uint8_t code; };
template <size_t N>
inline bool storeUCharKeys(SettingsStore &store,
                           const UCharKey (&keys)[N]) {
  bool existed[N] = {};
  uint8_t previous[N] = {};
  for (size_t i = 0; i < N; ++i) {
    existed[i] = store.hasUChar(keys[i].name);
    if (existed[i] && !store.readUChar(keys[i].name, previous[i])) return false;
  }
  auto verifiedWrite = [](SettingsStore &s, const char *key, uint8_t value) {
    s.writeUChar(key, value);
    uint8_t back = 0;
    return s.readUChar(key, back) && back == value;
  };
  bool allOk = true;
  for (size_t i = 0; i < N; ++i)
    allOk = verifiedWrite(store, keys[i].name, keys[i].code) && allOk;
  if (allOk) return true;
  // Rollback: every restore is attempted and verified by read-back; nothing
  // here can turn a failed store into success.
  auto restore = [](SettingsStore &s, bool existedKey, const char *key,
                    uint8_t value) {
    if (!existedKey) {
      s.removeUChar(key);
      uint8_t back = 0;
      return !s.hasUChar(key) && !s.readUChar(key, back);
    }
    s.writeUChar(key, value);
    uint8_t back = 0;
    return s.readUChar(key, back) && back == value;
  };
  for (size_t i = 0; i < N; ++i)
    restore(store, existed[i], keys[i].name, previous[i]);
  return false;
}
} // namespace detail

inline bool storeSettings(SettingsStore &store, uint8_t brightnessPercent,
                          idle_clock::ClockFormat clock, rgb_order::Order order) {
  if (!panel::validBrightnessPercent(brightnessPercent)) return false;
  // The three keys of this one transaction, in their fixed write order:
  // `brightness` (the percent byte), `clockformat` (the format code) and
  // `rgborder` (the order code 0-5, RGB = 0).
  const detail::UCharKey keys[3] = {{"brightness", brightnessPercent},
                                    {"clockformat",
                                     idle_clock::clockFormatCode(clock)},
                                    {"rgborder", rgb_order::orderCode(order)}};
  return detail::storeUCharKeys(store, keys);
}

// Exact first-line classification for the settings routes (S8 + T4a). Only
// these exact lines belong to the settings surface; everything else — every
// receiver route and the unknown paths — is left to the receiver parser
// exactly as before, including its unknown-path 404. The city form of T4a
// posts to its OWN route (POST /config/city): only the POST lines are
// classified as the city submission, a GET of that path stays Other.
enum class Route : uint8_t { Other, Get, Save, CitySave, OrientSave };
constexpr const char *CITY_PATH = "/config/city";
// T17: the orientation recalibration has its own form and its own route;
// only the exact POST lines belong to it, a GET of the path stays Other
// (like the city route).
constexpr const char *ORIENT_PATH = "/config/orientation";
inline Route routeOf(const std::string &firstLine) {
  if (firstLine == "GET /config HTTP/1.1" || firstLine == "GET /config HTTP/1.0")
    return Route::Get;
  if (firstLine == "POST /config HTTP/1.1" || firstLine == "POST /config HTTP/1.0")
    return Route::Save;
  if (firstLine == "POST /config/city HTTP/1.1" ||
      firstLine == "POST /config/city HTTP/1.0")
    return Route::CitySave;
  if (firstLine == "POST /config/orientation HTTP/1.1" ||
      firstLine == "POST /config/orientation HTTP/1.0")
    return Route::OrientSave;
  return Route::Other;
}

// Bounded Content-Length extraction from an accumulated header block, same
// rules as the portal serving loop uses: a digits-only value, -1 when the
// header is absent or malformed. Shared by both station-side surfaces.
inline long parseContentLength(const std::string &requestHeaders) {
  std::string lower;
  for (const char c : requestHeaders)
    lower.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
  const size_t key = lower.find("content-length:");
  if (key == std::string::npos) return -1;
  const size_t valueStart = requestHeaders.find_first_not_of(" \t", key + 15);
  if (valueStart == std::string::npos) return -1;
  const size_t valueEnd = requestHeaders.find('\r', valueStart);
  if (valueEnd == std::string::npos) return -1;
  const std::string digits = requestHeaders.substr(valueStart, valueEnd - valueStart);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos)
    return -1;
  return strtol(digits.c_str(), nullptr, 10);
}

// application/x-www-form-urlencoded POST /config body (T4a + S14): exactly
// the SEVEN keys `brightness`, `clock`, `order`, `clocksecs`, `weathersecs`,
// `units` and `weatheron`, each at most once, no unknown keys — the page's
// own form always submits all seven, so all are required; '+' is a space and
// %XX requires two hex digits (the shared portal urlencoded discipline, so
// malformed bodies are rejected, not coerced). Empty values parse and are
// rejected later by the per-value review with its reason.
struct SettingsFields {
  bool ok = false;
  std::string brightness, clock, order, clocksecs, weathersecs, units,
      weatheron;
};

inline SettingsFields parseSettingsForm(const char *body, size_t length) {
  SettingsFields result;
  const std::string input(body, length);
  bool saw[7] = {false, false, false, false, false, false, false};
  const char *const KEY[7] = {"brightness", "clock", "order", "clocksecs",
                              "weathersecs", "units", "weatheron"};
  size_t start = 0;
  for (;;) {
    const size_t amp = input.find('&', start);
    const bool last = amp == std::string::npos;
    const std::string pair =
        input.substr(start, (last ? input.size() : amp) - start);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) return result;
      const std::string key = pair.substr(0, eq);
      std::string value;
      if (!portal::decodeUrlValue(pair.substr(eq + 1), value)) return result;
      size_t index = 7;
      for (size_t k = 0; k < 7; ++k)
        if (key == KEY[k]) index = k;
      if (index == 7 || saw[index]) return result;
      saw[index] = true;
      if (index == 0) result.brightness = value;
      else if (index == 1) result.clock = value;
      else if (index == 2) result.order = value;
      else if (index == 3) result.clocksecs = value;
      else if (index == 4) result.weathersecs = value;
      else if (index == 5) result.units = value;
      else result.weatheron = value;
    }
    if (last) break;
    start = amp + 1;
  }
  result.ok = saw[0] && saw[1] && saw[2] && saw[3] && saw[4] && saw[5] &&
              saw[6];
  return result;
}

// T4a: the city-search submission (POST /config/city) carries exactly one
// key, `city`, with the same URL rules.
struct CitySearchFields {
  bool ok = false;
  std::string city;
};

inline CitySearchFields parseCityForm(const char *body, size_t length) {
  CitySearchFields result;
  const std::string input(body, length);
  bool sawCity = false;
  size_t start = 0;
  for (;;) {
    const size_t amp = input.find('&', start);
    const bool last = amp == std::string::npos;
    const std::string pair =
        input.substr(start, (last ? input.size() : amp) - start);
    if (!pair.empty()) {
      const size_t eq = pair.find('=');
      if (eq == std::string::npos) return result;
      const std::string key = pair.substr(0, eq);
      std::string value;
      if (!portal::decodeUrlValue(pair.substr(eq + 1), value)) return result;
      if (key != "city" || sawCity) return result;
      result.city = value;
      sawCity = true;
    }
    if (last) break;
    start = amp + 1;
  }
  result.ok = sawCity;
  return result;
}

// S10: short decimal percent inside the allowed set; the percent->scale
// mapping stays in panel:: so there is one source of the rule.
struct BrightnessChoice {
  bool ok = false;
  uint8_t percent = panel::BRIGHTNESS_DEFAULT_PERCENT;
};

inline BrightnessChoice reviewBrightness(const std::string &value) {
  BrightnessChoice choice;
  if (value.empty() || value.size() > 3) return choice;
  unsigned parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return choice;
    parsed = parsed * 10 + unsigned(c - '0');
    if (parsed > panel::BRIGHTNESS_MAX_PERCENT) return choice;
  }
  if (!panel::validBrightnessPercent(static_cast<uint8_t>(parsed))) return choice;
  choice.ok = true;
  choice.percent = static_cast<uint8_t>(parsed);
  return choice;
}

// S9: exactly "24h" or "12h"; anything else is rejected, not coerced.
struct ClockChoice {
  bool ok = false;
  idle_clock::ClockFormat format = idle_clock::ClockFormat::H24;
};

inline ClockChoice reviewClock(const std::string &value) {
  ClockChoice choice;
  if (value == "24h") choice.ok = true;
  else if (value == "12h") {
    choice.ok = true;
    choice.format = idle_clock::ClockFormat::H12;
  }
  return choice;
}

inline const char *clockName(idle_clock::ClockFormat format) {
  return format == idle_clock::ClockFormat::H12 ? "12h" : "24h";
}

// S7: one of the six named permutations, reviewed by the shared exact,
// case-insensitive parse (the same rule the USB console and the portal
// route use); anything else is rejected, not coerced.
struct OrderChoice {
  bool ok = false;
  rgb_order::Order value = rgb_order::Order::RGB;
};

inline OrderChoice reviewOrder(const std::string &value) {
  OrderChoice choice;
  if (rgb_order::parseOrder(value, choice.value)) choice.ok = true;
  return choice;
}

// --- T4a (+ S14): the weather settings of the LAN page ---------------------
// New keys next to the panel triple, all in `gentle`, all with read-back
// verification and none in the DOWN-reset scope: `clocksecs`,
// `weathersecs` (seconds, 5..120 step 5; defaults 20 and 10), `units`
// (0 = Celsius default, 1 = Fahrenheit), the S14 switch `weatheron` (1 = on
// default, 0 = off) and the resolved city
// `citylat`/`citylon` (signed int32 micro-degrees) + `cityname` (<= 24
// bytes). Absent or outside-the-set stored values fall back to the
// documented defaults — including the whole Sevilla record from
// weather::defaultCity(), so a board with nothing stored already has a city.

// Exactly "C" or "F" (the page sends these values); anything else is
// rejected, not coerced and not case-forgiven.
struct UnitChoice {
  bool ok = false;
  weather::Unit unit = weather::Unit::Celsius;
};
inline UnitChoice reviewUnits(const std::string &value) {
  UnitChoice choice;
  if (value == "C") choice.ok = true;
  else if (value == "F") {
    choice.ok = true;
    choice.unit = weather::Unit::Fahrenheit;
  }
  return choice;
}
inline const char *unitName(weather::Unit unit) {
  return unit == weather::Unit::Fahrenheit ? "F" : "C";
}

// Short decimal seconds inside weather::validSeconds (5..120 step 5); the
// same shape as the brightness review, including leading zeros ("020" =
// 20); anything else is rejected, not coerced.
struct SecondsChoice {
  bool ok = false;
  uint8_t seconds = 0;
};
inline SecondsChoice reviewSeconds(const std::string &value) {
  SecondsChoice choice;
  if (value.empty() || value.size() > 3) return choice;
  unsigned parsed = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return choice;
    parsed = parsed * 10 + unsigned(c - '0');
    if (parsed > weather::SECONDS_MAX) return choice;
  }
  if (!weather::validSeconds(static_cast<uint8_t>(parsed))) return choice;
  choice.ok = true;
  choice.seconds = static_cast<uint8_t>(parsed);
  return choice;
}

// The load-side fallbacks, mirroring loadPanelSettings(): an absent,
// out-of-range or off-step stored byte falls back to its documented
// default; a valid byte passes through untouched.
inline uint8_t clockSecondsOrDefault(uint8_t stored) {
  return weather::validSeconds(stored) ? stored
                                       : weather::CLOCK_SECONDS_DEFAULT;
}
inline uint8_t weatherSecondsOrDefault(uint8_t stored) {
  return weather::validSeconds(stored) ? stored
                                       : weather::WEATHER_SECONDS_DEFAULT;
}
inline weather::Unit unitOrDefault(uint8_t stored) {
  weather::Unit unit = weather::Unit::Celsius;
  return weather::unitFromCode(stored, unit) ? unit : weather::Unit::Celsius;
}

// S14: the persisted weather-feature switch, its own byte in `gentle` —
// code 1 = on (the documented DEFAULT), code 0 = off. An absent or invalid
// stored byte falls back to the documented default (on) exactly like the
// other fallback loaders above. While the switch is off the firmware never
// ticks the fetch scheduler (no TLS connection at all, not even the first
// one after boot) and the clock owns the screen; the /health `weather`
// field keeps its independent meaning (a forecast is HELD).
constexpr uint8_t WEATHER_ON_CODE = 1;
constexpr uint8_t WEATHER_OFF_CODE = 0;

// Exactly "on" or "off" (the page sends these values); anything else is
// rejected, not coerced and not case-forgiven.
struct WeatherOnChoice {
  bool ok = false;
  bool on = true;
};
inline WeatherOnChoice reviewWeatherOn(const std::string &value) {
  WeatherOnChoice choice;
  if (value == "on") choice.ok = true;
  else if (value == "off") {
    choice.ok = true;
    choice.on = false;
  }
  return choice;
}
inline const char *weatherOnName(bool on) { return on ? "on" : "off"; }

inline bool weatherOnOrDefault(uint8_t stored) {
  if (stored == WEATHER_OFF_CODE) return false;
  // 1 = on; every other byte (including an absent key's sentinel) is
  // invalid and falls back to the documented default, on.
  return true;
}

// The stored city, falling back to the S6 default: the transaction below
// guarantees all-or-nothing, so an incomplete or unusable stored record
// resolves to the whole Sevilla default (exact values committed in the
// weather layer) and a complete one is loaded byte-for-byte, with the name
// re-clamped UTF-8-safety (never torn) and the coordinates bounds-checked.
inline weather::CityRecord storedCityOrDefault(bool stored, int32_t latMicro,
                                               int32_t lonMicro,
                                               const std::string &name) {
  weather::CityRecord candidate;
  candidate.valid =
      stored && !name.empty() &&
      latMicro >= -weather::LAT_MICRO_MAX && latMicro <= weather::LAT_MICRO_MAX &&
      lonMicro >= -weather::LON_MICRO_MAX && lonMicro <= weather::LON_MICRO_MAX;
  if (!candidate.valid) return weather::defaultCity();
  candidate.latMicro = latMicro;
  candidate.lonMicro = lonMicro;
  candidate.name = weather::clampName(name);
  return candidate;
}

// The T4b intake of a geocoding ANSWER into the stored city record: the
// committed fetch application rules decide (transport error, status, size
// and the parse discipline in weather_fetch::applyCity), and the STORED
// name is additionally clamped to the 24-byte bound the record persists
// with. The caller in T4b only has to perform the request.
inline bool cityFromResponse(const weather_fetch::Transport::Response &response,
                             weather::CityRecord &out) {
  if (!weather_fetch::applyCity(response, out)) return false;
  out.name = weather::clampName(out.name);
  return true;
}

// The submitted search name BEFORE any request could run: bounded exactly
// like the fetch layer's own query rule, so an empty or oversized name is
// rejected at the form review and never reaches the transport.
inline bool reviewCitySearch(const std::string &name) {
  return weather_fetch::validQuery(name);
}

// T4a: the weather durations and unit persist through the SAME verified
// transaction discipline as the panel triple — write order `clocksecs`,
// `weathersecs`, `units`, read-back-only decisions, rollback of every key
// including the failing one, absence fidelity, and never success for an
// unverified outcome. Rejected values change nothing.
inline bool storeWeatherSettings(SettingsStore &store, uint8_t clockSeconds,
                                 uint8_t weatherSeconds, weather::Unit unit) {
  if (!weather::validSeconds(clockSeconds) ||
      !weather::validSeconds(weatherSeconds))
    return false;
  const detail::UCharKey keys[3] = {{"clocksecs", clockSeconds},
                                    {"weathersecs", weatherSeconds},
                                    {"units", weather::unitCode(unit)}};
  return detail::storeUCharKeys(store, keys);
}

// T4b: the combined transaction of POST /config — the panel triple and the
// weather settings in ONE atomic store over the same verified discipline, so
// a failure never leaves the stored settings mixed across the two surfaces:
// one snapshot, one rollback, read-back-only decisions, write order
// brightness, clockformat, rgborder, clocksecs, weathersecs, units and, as
// the seventh key (S14), weatheron.
inline bool storeAllSettings(SettingsStore &store, uint8_t brightnessPercent,
                             idle_clock::ClockFormat clock,
                             rgb_order::Order order, uint8_t clockSeconds,
                             uint8_t weatherSeconds, weather::Unit unit,
                             bool weatherOn) {
  if (!panel::validBrightnessPercent(brightnessPercent)) return false;
  if (!weather::validSeconds(clockSeconds) ||
      !weather::validSeconds(weatherSeconds))
    return false;
  const detail::UCharKey keys[7] = {{"brightness", brightnessPercent},
                                    {"clockformat",
                                     idle_clock::clockFormatCode(clock)},
                                    {"rgborder", rgb_order::orderCode(order)},
                                    {"clocksecs", clockSeconds},
                                    {"weathersecs", weatherSeconds},
                                    {"units", weather::unitCode(unit)},
                                    {"weatheron",
                                     weatherOn ? WEATHER_ON_CODE
                                               : WEATHER_OFF_CODE}};
  return detail::storeUCharKeys(store, keys);
}

// T4a: the resolved city persists through the same verified transaction
// discipline over its typed keys — write order `citylat`, `citylon`,
// `cityname`, snapshot of all three (int32/int32/string), verified
// commits, a rollback that restores even the failing key, absence fidelity
// for the string, and a lying write (bytes changed behind the store's
// back) caught only through the read-back. An unusable record (invalid,
// out of bounds or an empty name) is rejected before anything is written.
inline bool storeCity(SettingsStore &store, const weather::CityRecord &city) {
  if (!city.valid || city.name.empty() ||
      city.latMicro > weather::LAT_MICRO_MAX ||
      city.latMicro < -weather::LAT_MICRO_MAX ||
      city.lonMicro > weather::LON_MICRO_MAX ||
      city.lonMicro < -weather::LON_MICRO_MAX)
    return false;
  const std::string name = weather::clampName(city.name);
  const char *const keys[3] = {"citylat", "citylon", "cityname"};
  bool existed[3] = {false, false, false};
  int32_t previousLat = 0, previousLon = 0;
  std::string previousName;
  existed[0] = store.hasInt32(keys[0]);
  if (existed[0] && !store.readInt32(keys[0], previousLat)) return false;
  existed[1] = store.hasInt32(keys[1]);
  if (existed[1] && !store.readInt32(keys[1], previousLon)) return false;
  existed[2] = store.hasString(keys[2]);
  if (existed[2] && !store.readString(keys[2], previousName)) return false;
  auto verifiedInt = [&](const char *key, int32_t value) {
    store.writeInt32(key, value);
    int32_t back = 0;
    return store.readInt32(key, back) && back == value;
  };
  auto verifiedString = [&](const char *key, const std::string &value) {
    store.writeString(key, value);
    std::string back;
    return store.readString(key, back) && back == value;
  };
  bool allOk = true;
  allOk = verifiedInt(keys[0], city.latMicro) && allOk;
  allOk = verifiedInt(keys[1], city.lonMicro) && allOk;
  allOk = verifiedString(keys[2], name) && allOk;
  if (allOk) return true;
  // Rollback: every restore is attempted — including the failing key's
  // own — and every restore's read-back decides; "was not stored" is
  // restored faithfully for the string as well.
  auto restoreInt = [&](int index) {
    if (!existed[index]) {
      store.removeUChar(keys[index]);
      int32_t back = 0;
      return !store.hasInt32(keys[index]) && !store.readInt32(keys[index], back);
    }
    const int32_t value = index == 0 ? previousLat : previousLon;
    store.writeInt32(keys[index], value);
    int32_t back = 0;
    return store.readInt32(keys[index], back) && back == value;
  };
  restoreInt(0);
  restoreInt(1);
  if (!existed[2]) {
    store.removeUChar(keys[2]);
    std::string back;
    if (store.hasString(keys[2]) || store.readString(keys[2], back)) return false;
  } else {
    store.writeString(keys[2], previousName);
    std::string back;
    if (!store.readString(keys[2], back) || back != previousName) return false;
  }
  return false;
}

// T17: the deliberate orientation recalibration of /config clears EXACTLY
// the single key `updir` — never the panel keys, never the weather keys,
// never the credentials (the DOWN reset owns those, and since S11 it no
// longer touches this one either). Verified removal, read-back only: an
// absent key is already the goal state and reports success with no write
// at all, while a removal that does not take effect reports failure. The
// caller applies only on success — it serves the cleared page and
// restarts; a failure answers the settings page with the reason and
// changes nothing.
inline bool clearOrientationReference(SettingsStore &store) {
  if (!store.hasUChar("updir")) return true;
  store.removeUChar("updir");
  uint8_t back = 0;
  return !store.hasUChar("updir") && !store.readUChar("updir", back);
}

// GET /config (S8 + T4a + S14): the settings page with the current values
// preselected. No authentication by decision — the page lives on the local
// network like every other route of this server. An error re-renders inside
// a visible error block carrying only the escaped reason. Brightness and
// clock format need no restart to take effect; a changed channel order
// does, which the page states plainly — the display driver fixes its pins
// at construction. T4a additions: the two duration seconds (5..120 step 5)
// and the unit choice join the main form; the city lives on its OWN form
// under POST /config/city (a separate submission with its own route, so
// the settings form and the search cannot be mixed); the page states that
// the weather screen needs a resolved city and that the durations are in
// seconds; the current city name is shown escaped. S14: a `weatheron`
// select joins the weather settings with exactly the two accepted values
// and the page states plainly that turning the feature off also stops the
// forecast downloads — the clock owns the screen. T17: an "Orientation"
// section offers the deliberate recalibration in its own form (POST
// /config/orientation): clear the saved upright reference and the board
// restarts to re-learn it, which the page says plainly, together with the
// stillness requirement and its 350 ms reason.
inline std::string configPage(
    uint8_t brightnessPercent, idle_clock::ClockFormat clock,
    rgb_order::Order order, weather::Unit unit, uint8_t clockSecs,
    uint8_t weatherSecs, bool weatherOn, const std::string &cityName,
    const std::string &error = "") {
  std::string page =
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>gentle-matrix panel settings</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em}"
      "select{display:block;width:100%;box-sizing:border-box;"
      "margin:0.25em 0 1em;padding:0.5em}"
      "input[type=text]{display:block;width:100%;box-sizing:border-box;"
      "margin:0.5em 0;padding:0.5em}"
      "button{padding:0.5em 1em}"
      ".error{color:#b00;font-weight:bold}"
      "</style></head><body>"
      "<h1>gentle-matrix</h1>"
      "<p>Panel settings. Brightness and clock format apply immediately. "
      "Changing the panel colour order restarts the board to apply it: the "
      "display driver fixes its pins at construction. Durations are in "
      "seconds. The weather screen shows only with a resolved city: search "
      "one below. Turning the weather feature off also stops downloading "
      "the forecast: the clock always stays on screen and the board makes "
      "no network requests for it.</p>";
  if (!error.empty())
    page += "<p class=\"error\">" + portal_page::escapeHtml(error) + "</p>";
  // T4a: the city search is its own submission to its own route, with a
  // bounded input and the current city shown escaped (the resolved city
  // name is public data; no secret is ever echoed by this surface, and
  // this surface carries no CSRF token by decision, like the settings
  // form itself).
  page += "<h2>Weather city</h2>" "<p>Currently set: <strong>" +
          portal_page::escapeHtml(cityName) +
          "</strong>.</p>"
          "<form method=\"POST\" action=\"" + std::string(CITY_PATH) + "\">"
          "<label>City<input type=\"text\" name=\"city\" maxlength=\"" +
          std::to_string(weather_fetch::QUERY_MAX_BYTES) +
          "\" placeholder=\"City name\" required></label>"
          "<button type=\"submit\">Search city</button>"
          "</form>";
  // T17: the orientation recalibration is its own submission to its own
  // route, with no fields; the page states the stillness requirement and
  // its reason (the re-learned upright reference needs about 350 ms of
  // stable gravity while the board restarts).
  page += "<h2>Orientation</h2>"
          "<p>If the panel shows the picture rotated (for example after the "
          "board was moved to a different mounting, or the saved reference "
          "no longer matches where the panel stands), clear the saved "
          "upright reference below. The board restarts and re-learns the "
          "reference with the panel in its final position. Keep the panel "
          "resting still during the restart: the reference needs about "
          "350 ms of stable gravity.</p>"
          "<form method=\"POST\" action=\"" + std::string(ORIENT_PATH) +
          "\"><button type=\"submit\">Recalibrate orientation</button></form>";
  page += "<h2>Display</h2>"
          "<form method=\"POST\" action=\"/config\">"
          "<label>Brightness<select name=\"brightness\">";
  for (uint8_t percent = panel::BRIGHTNESS_MIN_PERCENT;
       percent <= panel::BRIGHTNESS_MAX_PERCENT;
       percent = static_cast<uint8_t>(percent + panel::BRIGHTNESS_STEP_PERCENT)) {
    page += "<option value=\"" + std::to_string(percent);
    page += percent == brightnessPercent ? "\" selected>" : "\">";
    page += std::to_string(percent) + "%</option>";
  }
  page += "</select></label>"
          "<label>Clock display<select name=\"clock\">";
  for (const idle_clock::ClockFormat format :
       {idle_clock::ClockFormat::H24, idle_clock::ClockFormat::H12}) {
    page += "<option value=\"";
    page += clockName(format);
    page += format == clock ? "\" selected>" : "\">";
    page += format == idle_clock::ClockFormat::H24 ? "24 h (default)"
                                                   : "12 h with AM/PM";
    page += "</option>";
  }
  page += "</select></label>"
          "<label>Clock duration<select name=\"clocksecs\">";
  for (uint8_t secs = weather::SECONDS_MIN;
       secs <= weather::SECONDS_MAX;
       secs = static_cast<uint8_t>(secs + weather::SECONDS_STEP)) {
    page += "<option value=\"" + std::to_string(secs);
    page += secs == clockSecs ? "\" selected>" : "\">";
    page += std::to_string(secs) + " s</option>";
  }
  page += "</select></label>"
          "<label>Weather duration<select name=\"weathersecs\">";
  for (uint8_t secs = weather::SECONDS_MIN;
       secs <= weather::SECONDS_MAX;
       secs = static_cast<uint8_t>(secs + weather::SECONDS_STEP)) {
    page += "<option value=\"" + std::to_string(secs);
    page += secs == weatherSecs ? "\" selected>" : "\">";
    page += std::to_string(secs) + " s</option>";
  }
  page += "</select></label>"
          "<label>Temperature unit<select name=\"units\">";
  for (const weather::Unit candidate :
       {weather::Unit::Celsius, weather::Unit::Fahrenheit}) {
    page += "<option value=\"";
    page += unitName(candidate);
    page += candidate == unit ? "\" selected>" : "\">";
    page += candidate == weather::Unit::Celsius ? "Celsius (default)"
                                                : "Fahrenheit";
    page += "</option>";
  }
  page += "</select></label>"
          "<label>Weather feature<select name=\"weatheron\">";
  for (const bool on : {true, false}) {
    page += "<option value=\"";
    page += weatherOnName(on);
    page += on == weatherOn ? "\" selected>" : "\">";
    page += on ? "On (default)" : "Off";
    page += "</option>";
  }
  page += "</select></label>"
          "<label>Panel colour order<select name=\"order\">";
  for (uint8_t i = 0; i < rgb_order::ORDER_COUNT; ++i) {
    const rgb_order::Order candidate = static_cast<rgb_order::Order>(i);
    page += "<option value=\"";
    page += rgb_order::orderName(candidate);
    page += candidate == order ? "\" selected>" : "\">";
    page += rgb_order::orderName(candidate);
    page += i == 0 ? " (default)" : "";
    page += "</option>";
  }
  page += "</select></label>"
          "<button type=\"submit\">Apply</button>"
          "</form></body></html>";
  return page;
}

// The previous four-argument signature keeps compiling for its existing
// call site until T4b switches it to the live weather values: the extended
// page is rendered with the DOCUMENTED DEFAULTS (Celsius, 20 s / 10 s, the
// switch on, the Sevilla default city), so nothing old breaks and nothing
// extra has to be stored yet.
inline std::string configPage(uint8_t brightnessPercent,
                              idle_clock::ClockFormat clock,
                              rgb_order::Order order,
                              const std::string &error = "") {
  return configPage(brightnessPercent, clock, order, weather::Unit::Celsius,
                    weather::CLOCK_SECONDS_DEFAULT,
                    weather::WEATHER_SECONDS_DEFAULT, true,
                    weather::defaultCity().name, error);
}

// The success page. An unchanged order: the settings were persisted and are
// already applied — no restart, unlike the portal forms. An actually changed
// order: the board saves it and restarts, because the display driver fixes
// its pins at construction.
inline std::string configSavedPage(bool orderRestart = false) {
  const char *body =
      orderRestart
          ? "<p>Settings saved and applied. Brightness and clock format are "
            "already live; the changed colour order takes effect when the "
            "board restarts (it restarts now, because the display driver "
            "fixes its pins at construction). "
            "<a href=\"/config\">Back to the settings</a>.</p>"
          : "<p>Settings saved and applied immediately &mdash; no restart "
            "needed. <a href=\"/config\">Back to the settings</a>.</p>";
  return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>gentle-matrix settings saved</title></head><body>"
         "<h1>gentle-matrix</h1>" +
         std::string(body) + "</body></html>";
}

// T17: the success page of the orientation recalibration — the reference
// was cleared and the board is restarting to re-learn it, with the
// stillness requirement and its reason said plainly. A failed clear
// answers the settings page with the reason instead and nothing restarts.
inline std::string orientationClearedPage() {
  return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>gentle-matrix orientation</title></head><body>"
         "<h1>gentle-matrix</h1>"
         "<p>Saved orientation reference cleared. The board is restarting "
         "now &mdash; keep the panel resting still and let it stand in its "
         "usual position, because it re-learns the reference during the "
         "restart and needs about 350 ms of stable gravity. You can close "
         "this page and return to the <a href=\"/config\">settings</a> "
         "afterwards.</p></body></html>";
}
} // namespace panel_settings
