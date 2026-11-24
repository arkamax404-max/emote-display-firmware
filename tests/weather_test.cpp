// Host test CLI for the pure weather layer (T1): bounded hand-written JSON
// extraction for the two Open-Meteo responses, the C/F tenths conversion,
// the day labels, the alternation slots, the 30-minute fetch cadence and
// the URL/JSON text helpers. No Arduino symbols are pulled in — the whole
// translation unit compiles on the host against include/ only.
//
// One case per invocation: weather_test <case>; every case prints OK and
// exits 0 or fails with "line N: condition" on stderr and exit 1.
//
// Cases:
//   convert   unit code mapping, tenths conversion and display rounding
//   parse     parseForecast: canonical body, tolerances and rejections
//   city      parseCity: geocoding body, rejections, clampName
//   labels    dayLabel: HOY plus the seven weekday names shifted
//   slot      slotAt: alternation boundaries for the defaults and edge values
//   cadence   fetchDue: first connect, the 30-minute cadence and wrap
//   paths     urlEncode, microDegreesString, forecastPath, geocodePath,
//             jsonEscape and the bounded HTTP body split
#include "weather.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "line " << __LINE__ << ": " #condition "\n";                \
      return 1;                                                                \
    }                                                                          \
  } while (false)

namespace {
bool hasForecast(const weather::Forecast &f, weather::Unit unit,
                 const int16_t tmax[4], const int16_t tmin[4],
                 const unsigned prob[4]) {
  bool ok = f.valid && f.unit == unit;
  for (int i = 0; i < 4; ++i)
    ok = ok && f.tmax[i] == tmax[i] && f.tmin[i] == tmin[i] &&
         f.prob[i] == prob[i];
  return ok;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];
  if (test == "convert") {
    for (unsigned code = 0; code < 2; ++code) {
      weather::Unit unit = weather::Unit::Celsius;
      CHECK(weather::unitFromCode(static_cast<uint8_t>(code), unit));
      CHECK(weather::unitCode(unit) == code);
    }
    weather::Unit unit = weather::Unit::Celsius;
    CHECK(!weather::unitFromCode(2, unit));
    CHECK(weather::unitCode(unit) == 0); // untouched fallback

    // Exact integer rules: 0.0 C -> 32.0 F; 0.1 C -> 32.2 F (the 9/5 factor
    // rounds up); 37.0 C -> 98.6 F; -40.0 C -> -40.0 F.
    CHECK(weather::celsiusToFahrenheitTenths(0) == 320);
    CHECK(weather::celsiusToFahrenheitTenths(1) == 322);
    CHECK(weather::celsiusToFahrenheitTenths(370) == 986);
    CHECK(weather::celsiusToFahrenheitTenths(-400) == -400);
    // F -> C: 32.0 F -> 0.0 C, 98.6 F -> 37.0 C, -40.0 F -> -40.0 C, and a
    // value landing between tenths rounds half away from zero, symmetrically.
    CHECK(weather::fahrenheitToCelsiusTenths(320) == 0);
    CHECK(weather::fahrenheitToCelsiusTenths(986) == 370);
    CHECK(weather::fahrenheitToCelsiusTenths(-400) == -400);
    CHECK(weather::fahrenheitToCelsiusTenths(321) == 1);
    // The symmetric half-away counterpart sits just BELOW 320 (0 C = 32 F,
    // so -32.1 F is -35.6 C, not -0.05 C): 31.9 F -> -0.0555 C -> -1 tenth.
    CHECK(weather::fahrenheitToCelsiusTenths(319) == -1);
    // Display conversion happens only when the units differ.
    CHECK(weather::displayedTenths(370, weather::Unit::Celsius,
                                   weather::Unit::Celsius) == 370);
    CHECK(weather::displayedTenths(370, weather::Unit::Celsius,
                                   weather::Unit::Fahrenheit) == 986);
    CHECK(weather::displayedTenths(986, weather::Unit::Fahrenheit,
                                   weather::Unit::Celsius) == 370);
    // Printed integer degrees round half away from zero.
    CHECK(weather::roundedDegrees(124) == 12);
    CHECK(weather::roundedDegrees(125) == 13);
    CHECK(weather::roundedDegrees(-125) == -13);
    CHECK(weather::roundedDegrees(-124) == -12);
    CHECK(weather::roundedDegrees(4) == 0);
  } else if (test == "parse") {
    // Canonical Open-Meteo daily body (Celsius fetch), fields in the exact
    // order the API emits them, with unrelated fields around them. The
    // `daily_units` object repeats the key names with string values; the
    // needle grep requires the following ':' AND '[', so it never matches.
    const char *body =
        "{\"latitude\":40.416775,\"longitude\":-3.70379,"
        "\"timezone\":\"Europe/Madrid\","
        "\"daily_units\":{\"temperature_2m_max\":\"\xC2\xB0" "C\","
        "\"precipitation_probability_max\":\"%\"},"
        "\"daily\":{"
        "\"time\":[\"2026-02-05\",\"2026-02-06\",\"2026-02-07\","
        "\"2026-02-08\"],"
        "\"temperature_2m_max\":[12.4,11.1,10.2,9.8],"
        "\"temperature_2m_min\":[3.1,-2.5,4.05,1.2],"
        "\"precipitation_probability_max\":[10,45,80,5]},"
        "\"utc_offset_seconds\":3600}";
    weather::Forecast f;
    CHECK(weather::parseForecast(body, std::strlen(body), f));
    const int16_t tmax[4] = {124, 111, 102, 98};
    const int16_t tmin[4] = {31, -25, 41, 12}; // 4.05 -> 41 (second digit 5)
    const unsigned prob[4] = {10, 45, 80, 5};
    CHECK(hasForecast(f, weather::Unit::Celsius, tmax, tmin, prob));

    // Re-parsing into a used record fully rewrites every field.
    CHECK(weather::parseForecast(body, std::strlen(body), f));

    // Rounding at the truncated second decimal, and integer-valued entries.
    const char *rounded =
        "{\"daily\":{\"temperature_2m_max\":[12.49,12.5,12.51,7],"
        "\"temperature_2m_min\":[3.15,3.149,0,0],"
        "\"precipitation_probability_max\":[0,100,3,7]}}";
    CHECK(weather::parseForecast(rounded, std::strlen(rounded), f));
    const int16_t rmax[4] = {125, 125, 125, 70};
    const int16_t rmin[4] = {32, 31, 0, 0};
    const unsigned rprob[4] = {0, 100, 3, 7};
    CHECK(hasForecast(f, weather::Unit::Celsius, rmax, rmin, rprob));

    weather::Forecast before;
    before.valid = true;
    before.unit = weather::Unit::Fahrenheit;
    for (int i = 0; i < 4; ++i) {
      before.tmax[i] = int16_t(i + 1);
      before.tmin[i] = int16_t(-i - 1);
      before.prob[i] = uint8_t(i * 10);
    }
    // Every failure leaves the caller's record byte-for-byte untouched: the
    // previous forecast survives a bad body by construction.
    const char *bad[] = {
        "",
        "{}",
        "{\"daily\":{\"temperature_2m_max\":[1,2,3]}}",   // truncated array
        "{\"daily\":{\"temperature_2m_max\":[1,2,3,4]}}", // other arrays missing
        "{\"daily\":{\"temperature_2m_max\":[1,2,3,null],"
        "\"temperature_2m_min\":[1,2,3,4],"
        "\"precipitation_probability_max\":[1,2,3,4]}}", // a null value
        "{\"daily\":{\"temperature_2m_max\":[1,2,3,4e2],"
        "\"temperature_2m_min\":[1,2,3,4],"
        "\"precipitation_probability_max\":[1,2,3,4]}}", // exponent form
        "{\"daily\":{\"temperature_2m_max\":[-5000.1,0,0,0],"
        "\"temperature_2m_min\":[0,0,0,0],"
        "\"precipitation_probability_max\":[0,0,0,0]}}", // tenths bound
        "{\"daily\":{\"temperature_2m_max\":[1,2,3,4],"
        "\"temperature_2m_min\":[1,2,3,4],"
        "\"precipitation_probability_max\":[1,2,3,101]}}", // probability range
        "{\"daily\":{\"temperature_2m_max\":[1,2,3,4],"
        "\"temperature_2m_min\":[1,2,3,4],"
        "\"precipitation_probability_max\":[-1,2,3,4]}}", // negative probability
        "{\"error\":true,\"reason\":\"Bad Request\"}"      // Open-Meteo error
    };
    for (const char *b : bad) {
      weather::Forecast probe = before;
      CHECK(!weather::parseForecast(b, std::strlen(b), probe));
      CHECK(std::memcmp(&probe, &before, sizeof(probe)) == 0);
    }
  } else if (test == "city") {
    // Canonical geocoding body for a single result (the API emits results in
    // this field order; the extractor resolves latitude, longitude and name
    // independently, so field order inside the object does not matter).
    const char *body =
        "{\"results\":[{\"id\":2643743,\"name\":\"Madrid\","
        "\"latitude\":40.416775,\"longitude\":-3.70379,\"elevation\":657,"
        "\"feature_code\":\"PPLC\",\"country_code\":\"ES\"}],"
        "\"generationtime_ms\":0.5}";
    weather::CityRecord city;
    CHECK(weather::parseCity(body, std::strlen(body), city));
    CHECK(city.valid);
    CHECK(city.latMicro == 40416775);
    CHECK(city.lonMicro == -3703790); // -3.70379 exactly
    CHECK(city.name == "Madrid");

    // A missing result list (unknown city) is a clean failure.
    weather::CityRecord before;
    before.valid = true;
    before.latMicro = 1;
    before.lonMicro = 2;
    before.name = "keep";
    const char *bad[] = {
        "",
        "{}",
        "{\"results\":[]}",                                   // empty results
        "{\"error\":true,\"reason\":\"...\"}",                 // API error body
        "{\"results\":[{\"name\":\"Madrid\"}]}",               // no coordinates
        "{\"results\":[{\"latitude\":40.4,\"longitude\":-3.7}]}", // no name
        "{\"results\":[{\"name\":\"\","
        "\"latitude\":40.4,\"longitude\":-3.7}]}",             // empty name
        "{\"results\":[{\"name\":\"A\\\\B\","
        "\"latitude\":40.4,\"longitude\":-3.7}]}",             // escape in name
        "{\"results\":[{\"name\":\"Madrid\",\"latitude\":181.0,"
        "\"longitude\":-3.7}]}",                               // out of range
        "{\"results\":[{\"name\":\"Madrid\",\"latitude\":40.4,"
        "\"longitude\":-181.1}]}",                             // out of range
    };
    for (const char *b : bad) {
      weather::CityRecord probe = before;
      CHECK(!weather::parseCity(b, std::strlen(b), probe));
      CHECK(probe.valid && probe.latMicro == 1 && probe.lonMicro == 2 &&
            probe.name == "keep");
    }
    // Name truncation is bounded and UTF-8 safe: cutting 24 bytes never
    // leaves a dangling continuation byte.
    const std::string longName = "Santiago de Compostela";
    CHECK(weather::clampName(longName) == longName); // 22 bytes fit
    // 23 letters + the two-byte N-tilde: the cut at 24 bytes must never
    // leave a dangling UTF-8 continuation byte, so the pair goes whole.
    CHECK(weather::clampName("abcdefghijklmnopqrstuvw\xc3\xb1") ==
          "abcdefghijklmnopqrstuvw");
    CHECK(weather::clampName(std::string(40, 'x')).size() == 24);

    // S6: the default city — what a board with nothing stored reports —
    // gets no other stored record in its way, and its micro-degree values
    // round-trip through the exact conversion the geocoding extraction
    // uses (the decimals Open-Meteo printed for Sevilla).
    const weather::CityRecord def = weather::defaultCity();
    CHECK(def.valid);
    CHECK(def.name == "Sevilla");
    CHECK(def.latMicro == 37382830);
    CHECK(def.lonMicro == -5973170);
    const std::string roundTrip =
        std::string("{\"results\":[{\"name\":\"") + def.name +
        "\",\"latitude\":" + weather::microDegreesString(def.latMicro) +
        ",\"longitude\":" + weather::microDegreesString(def.lonMicro) +
        "}]}";
    weather::CityRecord back;
    CHECK(weather::parseCity(roundTrip.data(), roundTrip.size(), back));
    CHECK(back.valid);
    CHECK(back.latMicro == def.latMicro);
    CHECK(back.lonMicro == def.lonMicro);
    CHECK(back.name == def.name);
  } else if (test == "labels") {
    // tm_wday: 0 = DOM. Today is always HOY; +k advances through the week.
    CHECK(std::string(weather::dayLabel(0, 0)) == "HOY");
    CHECK(std::string(weather::dayLabel(0, 6)) == "HOY");
    const char *const week[7] = {"DOM", "LUN", "MAR", "MIE",
                                 "JUE", "VIE", "SAB"};
    for (int wday = 0; wday < 7; ++wday)
      for (size_t index = 1; index <= 3; ++index)
        CHECK(std::string(weather::dayLabel(index, wday)) ==
              week[(wday + index) % 7]);
  } else if (test == "slot") {
    // Defaults S4: 20 s clock, 10 s weather — a 30 s cycle.
    for (uint64_t s = 0; s < 20; ++s)
      CHECK(weather::slotAt(s, 20, 10) == weather::Slot::Clock);
    for (uint64_t s = 20; s < 30; ++s)
      CHECK(weather::slotAt(s, 20, 10) == weather::Slot::Weather);
    CHECK(weather::slotAt(30, 20, 10) == weather::Slot::Clock);
    CHECK(weather::slotAt(50, 20, 10) == weather::Slot::Weather);
    CHECK(weather::slotAt(60, 20, 10) == weather::Slot::Clock);
    // Large monotonic values follow the same remainder rule: the cycle is
    // 30 s, so 600000000 lands at phase start (Clock) and 21 s into the
    // cycle is the weather window.
    CHECK(weather::slotAt(600000000, 20, 10) == weather::Slot::Clock);
    CHECK(weather::slotAt(600000019, 20, 10) == weather::Slot::Clock);
    CHECK(weather::slotAt(600000021, 20, 10) == weather::Slot::Weather);
    // Degenerate settings collapse to the clock (weather never shown).
    CHECK(weather::slotAt(3, 0, 0) == weather::Slot::Clock);
    CHECK(weather::slotAt(3, 20, 0) == weather::Slot::Clock);
  } else if (test == "cadence") {
    // First connection fetches at once; then at most one attempt per 30 min.
    CHECK(weather::fetchDue(0, true, false, 0));
    CHECK(!weather::fetchDue(0, false, false, 0));
    const uint32_t interval = weather::FETCH_INTERVAL_MS;
    CHECK(!weather::fetchDue(interval - 1, true, true, 0));
    CHECK(weather::fetchDue(interval, true, true, 0));
    CHECK(weather::fetchDue(interval + 1, true, true, 0));
    // millis() wrap: the unsigned delta still decides.
    CHECK(!weather::fetchDue(UINT32_MAX, true, true, UINT32_MAX - interval + 1));
    CHECK(weather::fetchDue(100, true, true, UINT32_MAX - interval + 101));
    // A disconnected board never comes due, whatever the phase.
    CHECK(!weather::fetchDue(interval + 5, false, true, 5));
  } else if (test == "paths") {
    constexpr const char *forecastHost = "api.open-meteo.com";
    constexpr const char *geocodeHost = "geocoding-api.open-meteo.com";
    CHECK(std::string(weather::FORECAST_HOST) == forecastHost);
    CHECK(std::string(weather::GEOCODE_HOST) == geocodeHost);
    // Micro-degrees to the exact decimal the API expects.
    CHECK(std::string(weather::microDegreesString(40416775)) == "40.416775");
    CHECK(std::string(weather::microDegreesString(-3703790)) == "-3.703790");
    CHECK(std::string(weather::microDegreesString(0)) == "0.000000");
    // The forecast query: today plus three days, the three daily arrays,
    // timezone auto, and the unit parameter ONLY in Fahrenheit.
    const std::string celsius = weather::forecastPath(40416775, -3703790,
                                                    weather::Unit::Celsius);
    CHECK(std::string(weather::FORECAST_HOST) == forecastHost);
    CHECK(celsius ==
          "/v1/forecast?latitude=40.416775&longitude=-3.703790"
          "&daily=temperature_2m_max,temperature_2m_min,"
          "precipitation_probability_max&timezone=auto&forecast_days=4");
    const std::string farenheit = weather::forecastPath(0, 0,
                                                      weather::Unit::Fahrenheit);
    CHECK(farenheit ==
          "/v1/forecast?latitude=0.000000&longitude=0.000000"
          "&daily=temperature_2m_max,temperature_2m_min,"
          "precipitation_probability_max&timezone=auto&forecast_days=4"
          "&temperature_unit=fahrenheit");
    // Geocoding: one result, Spanish, the city percent-encoded.
    CHECK(weather::geocodePath("Madrid") ==
          "/v1/search?name=Madrid&count=1&language=es");
    CHECK(weather::geocodePath("A Coru\xc3\xb1""a") ==
          "/v1/search?name=A%20Coru%C3%B1a&count=1&language=es");
    CHECK(weather::geocodePath("a.b-c_d~e") ==
          "/v1/search?name=a.b-c_d~e&count=1&language=es");
    // JSON escaping for the /health string fields.
    CHECK(weather::jsonEscape("Madrid") == "Madrid");
    CHECK(weather::jsonEscape("a\"b\\c") == "a\\\"b\\\\c");
    CHECK(weather::jsonEscape(std::string("a\x01z")) == "az"); // control gone
    // The bounded HTTP body split: header block end, then the body.
    const char *response = "HTTP/1.0 200 OK\r\nContent-Type: application/json"
                           "\r\n\r\n{\"ok\":true}";
    size_t offset = 0;
    CHECK(weather::httpBodyStart(response, std::strlen(response), offset));
    CHECK(std::string(response + offset) == "{\"ok\":true}");
    CHECK(!weather::httpBodyStart("HTTP/1.0 200 OK\r\n\r", 17, offset));
  } else {
    std::cerr << "unknown case\n";
    return 2;
  }
  std::cout << "OK\n";
  return 0;
}
