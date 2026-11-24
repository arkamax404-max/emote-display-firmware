#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Pure weather layer (T1): bounded hand-written extraction of the two small
// fixed Open-Meteo JSON bodies (forecast and geocoding), the Celsius/
// Fahrenheit tenths conversion, the micro-degree city record, the day
// labels, the clock/weather alternation slots and the 30-minute fetch
// cadence. No Arduino/WiFi symbols and no generic JSON document tree — the
// bodies are small and fixed, so the extractors are bounded needle greps in
// the style of the existing parsers. A dependency addition would be a
// separate decision.
namespace weather {

// S2: the resolved city persists as integers of micro-degrees to avoid float
// persistence; the name is bounded to NAME_MAX_BYTES (UTF-8 safe clamp).
constexpr size_t NAME_MAX_BYTES = 24;
constexpr int32_t LAT_MICRO_MAX = 90 * 1000000;
constexpr int32_t LON_MICRO_MAX = 180 * 1000000;

// S1/S4: the duration rules live next to the alternation slot rule; the
// stored values and the portal review functions live with the panel
// settings.
constexpr uint8_t SECONDS_MIN = 5;
constexpr uint8_t SECONDS_MAX = 120;
constexpr uint8_t SECONDS_STEP = 5;
constexpr uint8_t CLOCK_SECONDS_DEFAULT = 20;   // S4 user decision.
constexpr uint8_t WEATHER_SECONDS_DEFAULT = 10; // S4 user decision.
inline bool validSeconds(uint8_t seconds) {
  return seconds >= SECONDS_MIN && seconds <= SECONDS_MAX &&
         (seconds - SECONDS_MIN) % SECONDS_STEP == 0;
}

constexpr uint32_t FETCH_INTERVAL_MS = 30u * 60u * 1000u; // every 30 minutes.
constexpr const char *FORECAST_HOST = "api.open-meteo.com";
constexpr const char *GEOCODE_HOST = "geocoding-api.open-meteo.com";
constexpr uint16_t HTTPS_PORT = 443; // S5: TLS with NO certificate validation.

enum class Unit : uint8_t { Celsius = 0, Fahrenheit = 1 };
inline uint8_t unitCode(Unit unit) {
  return unit == Unit::Fahrenheit ? 1u : 0u;
}
inline bool unitFromCode(uint8_t code, Unit &out) {
  if (code > 1) return false;
  out = code == 1u ? Unit::Fahrenheit : Unit::Celsius;
  return true;
}

// Tenths of a degree everywhere, so no float ever crosses a boundary.
// C->F scales the MAGNITUDE by 9/5 with half-up rounding and then applies
// the sign (truncating the signed value toward zero would round negatives
// the wrong way: -40.0 C must map exactly to -40.0 F) and adds 320 tenths;
// F->C subtracts 320 and divides by 9/5 with the same magnitude rounding,
// so both directions are symmetric.
inline int16_t celsiusToFahrenheitTenths(int16_t celsiusTenths) {
  const int32_t magnitude = celsiusTenths < 0
                                ? -static_cast<int32_t>(celsiusTenths)
                                : static_cast<int32_t>(celsiusTenths);
  const int32_t scaled = (magnitude * 9 + 2) / 5;
  return static_cast<int16_t>(
      (celsiusTenths < 0 ? -scaled : scaled) + 320);
}
inline int16_t fahrenheitToCelsiusTenths(int16_t fahrenheitTenths) {
  int32_t value = static_cast<int32_t>(fahrenheitTenths) - 320;
  const bool negative = value < 0;
  const uint32_t magnitude = negative ? static_cast<uint32_t>(-value)
                                      : static_cast<uint32_t>(value);
  const uint32_t result = (magnitude * 5u + 4u) / 9u;
  return static_cast<int16_t>(negative ? -static_cast<int32_t>(result)
                                       : static_cast<int32_t>(result));
}
// The record stores whatever the fetch unit produced; the renderer converts
// only when the user has since moved the unit switch, until the next fetch
// stores the new unit.
inline int16_t displayedTenths(int16_t stored, Unit storedUnit, Unit shown) {
  if (storedUnit == shown) return stored;
  return shown == Unit::Fahrenheit ? celsiusToFahrenheitTenths(stored)
                                   : fahrenheitToCelsiusTenths(stored);
}
// Printed integer degrees round half away from zero.
inline int roundedDegrees(int16_t tenths) {
  return tenths >= 0 ? (tenths + 5) / 10 : -((-tenths + 5) / 10);
}

// Four cells: today, +1, +2, +3 (temperature tenths in `unit`, rain
// probability percent). `valid` is false until an accepted forecast body
// was parsed from the network.
struct Forecast {
  int16_t tmax[4];
  int16_t tmin[4];
  uint8_t prob[4];
  Unit unit = Unit::Celsius;
  bool valid = false;
};

struct CityRecord {
  int32_t latMicro = 0;
  int32_t lonMicro = 0;
  std::string name;
  bool valid = false;
};

// S6: the default city — Sevilla, the coordinates Open-Meteo's own
// geocoding response reports for name=Sevilla&language=es
// (37.38283, -5.97317). A freshly flashed board with nothing stored reports
// this, so it shows a forecast without being configured; saving another
// city from the portal replaces it.
inline CityRecord defaultCity() {
  CityRecord city;
  city.valid = true;
  city.latMicro = 37382830;
  city.lonMicro = -5973170;
  city.name = "Sevilla";
  return city;
}

// --- bounded hand-written extraction --------------------------------------
namespace detail {

// Bounded naive needle search inside [begin, end); nullptr when absent.
inline const char *findNeedle(const char *begin, const char *end,
                              const char *needle) {
  const size_t needleLength = std::strlen(needle);
  if (needleLength == 0 || static_cast<size_t>(end - begin) < needleLength)
    return nullptr;
  for (const char *p = begin; p + needleLength <= end; ++p)
    if (std::memcmp(p, needle, needleLength) == 0) return p;
  return nullptr;
}

// `"key"` follows by optional spaces, ':', optional spaces; returns the
// pointer just past that, or nullptr before the buffer end.
inline const char *afterField(const char *begin, const char *end,
                              const char *key) {
  const char *p = findNeedle(begin, end, key);
  if (!p) return nullptr;
  p += std::strlen(key);
  while (p < end && *p == ' ') ++p;
  if (p >= end || *p != ':') return nullptr;
  ++p;
  while (p < end && *p == ' ') ++p;
  return p < end ? p : nullptr;
}

// Same, but the value must be a JSON array: occurrences of the key whose
// value is not '[' (the forecast body repeats the array key names inside
// `daily_units` with string values) are skipped and the search continues
// after them, so the array field itself is always the one found. Returns
// the pointer just past the opening bracket, or nullptr.
inline const char *afterArrayField(const char *begin, const char *end,
                                   const char *key) {
  const size_t keyLength = std::strlen(key);
  const char *from = begin;
  for (;;) {
    const char *p = findNeedle(from, end, key);
    if (!p) return nullptr;
    const char *value = p + keyLength;
    while (value < end && *value == ' ') ++value;
    if (value < end && *value == ':') {
      ++value;
      while (value < end && *value == ' ') ++value;
      if (value < end && *value == '[') return value + 1;
    }
    from = p + keyLength; // not this occurrence; keep searching after it
  }
}

// A JSON number with optional '-' (no '+', no exponent, no escape forms —
// the API emits plain decimals; anything else is a rejection, not a
// coercion) parsed into tenths of a unit, with half-up rounding on the
// first truncated fractional digit. |out| <= bound, else failure.
inline bool parseTenths(const char *&p, const char *end, int32_t bound,
                        int32_t &out) {
  bool negative = false;
  if (p < end && *p == '-') {
    negative = true;
    ++p;
  }
  if (p >= end || *p < '0' || *p > '9') return false;
  int32_t integer = 0;
  int digits = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (++digits > 5) return false; // bounded walk before the bound check
    integer = integer * 10 + (*p - '0');
    ++p;
  }
  int32_t tenths = integer * 10;
  if (p < end && *p == '.') {
    ++p;
    if (p >= end || *p < '0' || *p > '9') return false;
    tenths += *p - '0';
    ++p;
    // The first truncated (second) fractional digit decides the half-up
    // rounding; later digits only walk, never downgrade the decision.
    if (p < end && *p >= '5' && *p <= '9') ++tenths;
    while (p < end && *p >= '0' && *p <= '9') ++p;
  }
  if (tenths > bound) return false;
  out = negative ? -tenths : tenths;
  return true;
}

// Non-negative JSON integer within [0, 100], for the rain probability.
inline bool parseProbability(const char *&p, const char *end, uint8_t &out) {
  if (p >= end || *p < '0' || *p > '9') return false;
  int value = 0;
  int digits = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (++digits > 3) return false;
    value = value * 10 + (*p - '0');
    ++p;
  }
  if (value > 100) return false;
  out = static_cast<uint8_t>(value);
  return true;
}

// A signed decimal into micro-degrees: |out| <= bound, exactly six kept
// fractional digits (padded with zeros when the source has fewer), the
// seventh decides a half-away rounding of the magnitude.
inline bool parseMicroDegrees(const char *&p, const char *end, int32_t bound,
                              int32_t &out) {
  bool negative = false;
  if (p < end && *p == '-') {
    negative = true;
    ++p;
  }
  if (p >= end || *p < '0' || *p > '9') return false;
  int64_t integer = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (integer > bound / 1000000) return false; // bounded walk
    integer = integer * 10 + (*p - '0');
    ++p;
  }
  int64_t micro = integer;
  int fracCount = 0;
  int roundUp = 0;
  if (p < end && *p == '.') {
    ++p;
    while (p < end && *p >= '0' && *p <= '9') {
      const int digit = *p - '0';
      ++p;
      ++fracCount;
      if (fracCount <= 6) micro = micro * 10 + digit;
      else if (fracCount == 7) roundUp = digit >= 5 ? 1 : 0;
      else if (fracCount > 12) return false; // absurd input, bail bounded
    }
    if (fracCount == 0) return false;
  }
  while (fracCount < 6) { // pad the kept digits up to the micro scale
    micro *= 10;
    ++fracCount;
  }
  if (roundUp) ++micro; // the magnitude grows: half away from zero
  if (micro > bound) return false;
  out = static_cast<int32_t>(negative ? -micro : micro);
  return true;
}

// JSON string tail after the opening quote: bytes are accepted while
// printable or non-ASCII UTF-8; a backslash or a control byte is a
// rejection (no escape handling — city names never carry them, and a
// forged body must fail, not be coerced). Returns false when nothing was
// read. The value is clamped by clampName() before it is ever stored.
inline bool parseName(const char *&p, const char *end, std::string &out) {
  out.clear();
  while (p < end && *p != '"') {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '\\' || c < 0x20) return false;
    out.push_back(*p);
    ++p;
    if (out.size() > 96) return false;
  }
  return !out.empty();
}

} // namespace detail

// The forecast body: the three daily arrays each carry today plus three
// further days in position order — exactly the four cells. Any absent,
// malformed, null or out-of-range value fails the whole parse and the
// caller keeps its previous forecast, because `out` is written only on
// success.
inline bool parseForecast(const char *json, size_t length, Forecast &out) {
  const char *end = json + length;
  Forecast candidate;
  struct Array {
    const char *key;
    int16_t *values;
  };
  const Array arrays[2] = {{"\"temperature_2m_max\"", candidate.tmax},
                           {"\"temperature_2m_min\"", candidate.tmin}};
  for (const Array &array : arrays) {
    // afterArrayField skips the same key names inside `daily_units` (string
    // values there) so only the real array field is read.
    const char *p = detail::afterArrayField(json, end, array.key);
    if (!p) return false;
    for (int i = 0; i < 4; ++i) {
      while (p < end && *p == ' ') ++p;
      int32_t value = 0;
      const int32_t bound = 10000; // ±1000.0 degrees in tenths
      if (!detail::parseTenths(p, end, bound, value)) return false;
      array.values[i] = static_cast<int16_t>(value);
      while (p < end && *p == ' ') ++p;
      if (i < 3) {
        if (p >= end || *p != ',') return false;
        ++p;
      }
    }
    if (p >= end || *p != ']') return false; // the array must actually close
    ++p;
  }
  {
    const char *p = detail::afterArrayField(
        json, end, "\"precipitation_probability_max\"");
    if (!p) return false;
    for (int i = 0; i < 4; ++i) {
      while (p < end && *p == ' ') ++p;
      if (!detail::parseProbability(p, end, candidate.prob[i])) return false;
      while (p < end && *p == ' ') ++p;
      if (i < 3) {
        if (p >= end || *p != ',') return false;
        ++p;
      }
    }
    if (p >= end || *p != ']') return false; // the array must actually close
    ++p;
  }
  candidate.valid = true;
  out = candidate;
  return true;
}

// The geocoding body: the FIRST result object (needles searched from the
// start of the results list) supplies latitude, longitude and name;
// a missing result list (unknown city), a missing field or any malformed
// value fails the whole parse without touching `out`.
inline bool parseCity(const char *json, size_t length, CityRecord &out) {
  const char *end = json + length;
  const char *p = detail::afterField(json, end, "\"results\"");
  if (!p || p >= end || *p != '[') return false;
  ++p;
  CityRecord candidate;
  const char *lat = detail::afterField(p, end, "\"latitude\"");
  if (!lat ||
      !detail::parseMicroDegrees(lat, end, LAT_MICRO_MAX, candidate.latMicro))
    return false;
  const char *lon = detail::afterField(p, end, "\"longitude\"");
  if (!lon ||
      !detail::parseMicroDegrees(lon, end, LON_MICRO_MAX, candidate.lonMicro))
    return false;
  const char *name = detail::afterField(p, end, "\"name\"");
  if (!name || name >= end || *name != '"') return false;
  ++name;
  if (!detail::parseName(name, end, candidate.name)) return false;
  candidate.valid = true;
  out = candidate;
  return true;
}

// S1: the day labels. Today is always HOY; the +1..+3 cells advance the
// RTC weekday (tm_wday, 0 = DOM). ASCII-only names — the shared 3x5 glyph
// table has no accented glyphs, and an unknown character would draw blank
// cells.
inline const char *dayLabel(size_t index, int weekday) {
  static const char *const WEEK[7] = {"DOM", "LUN", "MAR", "MIE",
                                      "JUE", "VIE", "SAB"};
  if (index == 0) return "HOY";
  int wday = weekday % 7;
  if (wday < 0) wday += 7;
  return WEEK[static_cast<size_t>((wday + static_cast<int>(index)) % 7)];
}

// S1/S4: the alternation slot over the monotonic tick the clock animation
// already uses, with the two configured durations as a cycle. The degenerate
// weather duration of zero collapses to the clock (nothing weather appears).
enum class Slot : uint8_t { Clock, Weather };
inline Slot slotAt(uint64_t seconds, uint8_t clockSecs, uint8_t weatherSecs) {
  const uint64_t cycle = uint64_t(clockSecs) + uint64_t(weatherSecs);
  if (cycle == 0 || weatherSecs == 0) return Slot::Clock;
  return (seconds % cycle) < clockSecs ? Slot::Clock : Slot::Weather;
}

// S1: the fetch cadence — at once on the first connection, then at most one
// attempt per FETCH_INTERVAL_MS, decided on the unsigned millis() delta so
// the wrap is safe. A disconnected board is never due.
inline bool fetchDue(uint32_t now, bool connected, bool everAttempted,
                     uint32_t lastAttemptMs) {
  if (!connected) return false;
  return !everAttempted ||
         static_cast<uint32_t>(now - lastAttemptMs) >= FETCH_INTERVAL_MS;
}

// --- URL and JSON text helpers ---------------------------------------------

inline std::string urlEncode(const std::string &raw) {
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  for (const char c : raw) {
    const unsigned char b = static_cast<unsigned char>(c);
    const bool unreserved = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
                            (b >= '0' && b <= '9') || b == '-' || b == '.' ||
                            b == '_' || b == '~';
    if (unreserved) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(HEX[b >> 4]);
      out.push_back(HEX[b & 15]);
    }
  }
  return out;
}

// Micro-degrees to the exact decimal form the API expects, always with six
// fractional digits.
inline std::string microDegreesString(int32_t micro) {
  const bool negative = micro < 0;
  const int64_t magnitude =
      negative ? -static_cast<int64_t>(micro) : static_cast<int64_t>(micro);
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%s%lld.%06lld",
                negative ? "-" : "", static_cast<long long>(magnitude / 1000000),
                static_cast<long long>(magnitude % 1000000));
  return buffer;
}

// The forecast query: today plus three further days as the four cells, the
// three daily arrays, automatic timezone; Fahrenheit only in °F mode.
inline std::string forecastPath(int32_t latMicro, int32_t lonMicro, Unit unit) {
  std::string path =
      std::string("/v1/forecast?latitude=") + microDegreesString(latMicro) +
      "&longitude=" + microDegreesString(lonMicro) +
      "&daily=temperature_2m_max,temperature_2m_min,"
      "precipitation_probability_max&timezone=auto&forecast_days=4";
  if (unit == Unit::Fahrenheit) path += "&temperature_unit=fahrenheit";
  return path;
}

// The geocoding query: one result, Spanish language, the city encoded.
inline std::string geocodePath(const std::string &city) {
  return std::string("/v1/search?name=") + urlEncode(city) +
         "&count=1&language=es";
}

// The stored city name is bounded to NAME_MAX_BYTES without ever tearing a
// UTF-8 sequence: a whole sequence that would not fit is dropped entire.
inline std::string clampName(const std::string &raw) {
  size_t keep = 0;
  size_t i = 0;
  while (i < raw.size() && keep < NAME_MAX_BYTES) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    size_t length = 1;
    if (c >= 0xF0) length = 4;
    else if (c >= 0xE0) length = 3;
    else if (c >= 0xC0) length = 2;
    if (i + length > raw.size() || keep + length > NAME_MAX_BYTES) break;
    keep += length;
    i += length;
  }
  return raw.substr(0, keep);
}

// Escapes a value for a /health JSON string: quotes and backslashes are
// escaped, every other byte >= 0x20 passes (UTF-8 included) and control
// bytes are dropped. The city name comes from the API, so this is a
// rendering-time guarantee, not a storage guarantee.
inline std::string jsonEscape(const std::string &raw) {
  std::string out;
  for (const char c : raw) {
    const unsigned char b = static_cast<unsigned char>(c);
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (b >= 0x20) out.push_back(c);
  }
  return out;
}

// The bounded HTTP body split used by the TLS fetch driver: the first
// "\r\n\r\n" ends the header block and everything after it is the body.
inline bool httpBodyStart(const char *data, size_t length, size_t &bodyOffset) {
  for (size_t i = 0; i + 4 <= length; ++i)
    if (std::memcmp(data + i, "\r\n\r\n", 4) == 0) {
      bodyOffset = i + 4;
      return true;
    }
  return false;
}

} // namespace weather
