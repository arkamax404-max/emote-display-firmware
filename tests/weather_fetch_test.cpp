// Host test rig for the fetch layer (T3): URLs and escaping, the cadence
// state machine with injected timestamps, staleness, and the full
// request/response decision path over a FAKE transport (status, body,
// error — no socket). No Arduino symbols: the whole translation unit
// compiles on the host against include/ only.
//
// One case per invocation: weather_fetch_test <case>; OK + exit 0 or
// "line N: condition" on stderr + exit 1.
//
// Cases:
//   urls      forecastUrl from a city record + unit (exact string, the
//             unit parameter only in Fahrenheit mode), geocodeUrl with a
//             space, an accent and an ampersand; invalid record -> empty
//   cadence   the scheduler: first connect fetches, never while disconnec-
//             ted, never while the portal/AP is open (and catches up after
//             it closes), the 30-minute success interval, the 60 s failure
//             retry, the forgotten-completed case, millis wrap
//   stale     never-succeeded reports stale; fresh/boundary/wrap after 26 h
//   forecast  applyForecast: 200 + canonical body updates; non-200, empty,
//             oversized (boundary at exactly BODY_MAX) and parse-rejected
//             bodies all leave the previous forecast byte-identical
//   city      searchCity query validation; applyCity 200/non-200/error-body
//             discipline, previous record untouched on failure
//   transport fetchForecast/searchCity drive the fake transport with the
//             right host/path and skip the call entirely for an invalid
//             city or an invalid query; the failure path never calls again
#include "weather.h"
#include "weather_fetch.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "line " << __LINE__ << ": " #condition "\n";                \
      return 1;                                                                \
    }                                                                          \
  } while (false)

namespace {
// Fake transport: the scripted response is returned for every get() call;
// every call is recorded so the rig can assert who called what.
struct FakeTransport : weather_fetch::Transport {
  struct Call {
    std::string host, path;
  };
  std::vector<Call> calls;
  Response next; // ok=false, status=0, empty body by default

  Response get(const char *host, const char *path) override {
    calls.push_back(Call{host, path});
    return next;
  }
};

bool sameForecast(const weather::Forecast &a, const weather::Forecast &b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool sameCity(const weather::CityRecord &a, const weather::CityRecord &b) {
  return a.valid == b.valid && a.latMicro == b.latMicro &&
         a.lonMicro == b.lonMicro && a.name == b.name;
}

const char *CANONICAL_BODY =
    "{\"daily\":{\"temperature_2m_max\":[12.4,11.1,10.2,9.8],"
    "\"temperature_2m_min\":[3.1,-2.5,4.05,1.2],"
    "\"precipitation_probability_max\":[10,45,80,5]},"
    "\"utc_offset_seconds\":3600}";

const char *CANONICAL_CITY =
    "{\"results\":[{\"id\":2643743,\"name\":\"Madrid\","
    "\"latitude\":40.416775,\"longitude\":-3.70379,\"elevation\":657,"
    "\"feature_code\":\"PPLC\",\"country_code\":\"ES\"}],"
    "\"generationtime_ms\":0.5}";
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];

  if (test == "urls") {
    // The exact composed forecast URL for the S6 default city, Celsius:
    // the committed path builder supplies the query, the fetch layer adds
    // scheme and host. The unit parameter appears ONLY in Fahrenheit mode.
    const weather::CityRecord city = weather::defaultCity();
    CHECK(weather_fetch::forecastUrl(city, weather::Unit::Celsius) ==
          "https://api.open-meteo.com/v1/forecast?latitude=37.382830"
          "&longitude=-5.973170&daily=temperature_2m_max,"
          "temperature_2m_min,precipitation_probability_max&timezone=auto"
          "&forecast_days=4");
    CHECK(weather_fetch::forecastUrl(city, weather::Unit::Fahrenheit) ==
          "https://api.open-meteo.com/v1/forecast?latitude=37.382830"
          "&longitude=-5.973170&daily=temperature_2m_max,"
          "temperature_2m_min,precipitation_probability_max&timezone=auto"
          "&forecast_days=4&temperature_unit=fahrenheit");
    // Geocoding: spaces, accents and an ampersand are percent-escaped; the
    // unreserved set (letters, digits, -._~, ASCII here) passes through.
    CHECK(weather_fetch::geocodeUrl("Madrid") ==
          "https://geocoding-api.open-meteo.com/v1/search?name=Madrid"
          "&count=1&language=es");
    CHECK(weather_fetch::geocodeUrl("A Coru\xc3\xb1" "a") ==
          "https://geocoding-api.open-meteo.com/v1/search"
          "?name=A%20Coru%C3%B1a&count=1&language=es");
    CHECK(weather_fetch::geocodeUrl("Benalm\xc3\xa1" "den Costa & Mar") ==
          "https://geocoding-api.open-meteo.com/v1/search"
          "?name=Benalm%C3%A1den%20Costa%20%26%20Mar&count=1&language=es");
    // A record that never resolved (nothing stored yet) builds no URL: the
    // caller must not fetch a forecast for an unusable record.
    weather::CityRecord invalid;
    CHECK(!invalid.valid);
    CHECK(weather_fetch::forecastUrl(invalid, weather::Unit::Celsius).empty());
    // The search query itself is bounded and never empty.
    CHECK(weather_fetch::validQuery("Sevilla"));
    CHECK(!weather_fetch::validQuery(""));
    CHECK(!weather_fetch::validQuery(std::string(
        weather_fetch::QUERY_MAX_BYTES + 1, 'x')));
    CHECK(weather_fetch::validQuery(
        std::string(weather_fetch::QUERY_MAX_BYTES, 'x')));
  } else if (test == "cadence") {
    weather_fetch::Scheduler scheduler;
    // Before any connection: nothing. In portal/AP mode: nothing.
    CHECK(scheduler.tick(0, false, false) == weather_fetch::Action::None);
    CHECK(scheduler.tick(0, true, true) == weather_fetch::Action::None);
    CHECK(scheduler.tick(1000, false, true) == weather_fetch::Action::None);
    // Once the station is connected OUTSIDE the portal the first fetch is
    // immediate — and tick books the attempt, so a caller that forgets
    // completed() cannot hammer: the pessimistic default treats silence as
    // a failure and allows the next attempt one retry interval later.
    CHECK(scheduler.tick(2000, true, false) == weather_fetch::Action::Fetch);
    CHECK(scheduler.tick(2001, true, false) == weather_fetch::Action::None);
    CHECK(scheduler.tick(2000 + weather_fetch::RETRY_AFTER_MS - 1, true,
                         false) == weather_fetch::Action::None);
    CHECK(scheduler.tick(2000 + weather_fetch::RETRY_AFTER_MS, true, false) ==
          weather_fetch::Action::Fetch);
    // A success moves the next attempt to the 30-minute cadence.
    scheduler.completed(2000 + weather_fetch::RETRY_AFTER_MS, true);
    const uint32_t secondAttempt =
        2000 + weather_fetch::RETRY_AFTER_MS + weather::FETCH_INTERVAL_MS;
    CHECK(scheduler.tick(2000 + weather_fetch::RETRY_AFTER_MS + 1, true,
                         false) == weather_fetch::Action::None);
    CHECK(scheduler.tick(secondAttempt - 1, true, false) ==
          weather_fetch::Action::None);
    CHECK(scheduler.tick(secondAttempt, true, false) ==
          weather_fetch::Action::Fetch);
    scheduler.completed(secondAttempt, true);
    // A failure moves the next attempt to the short retry interval only.
    const uint32_t thirdAttempt = secondAttempt + weather::FETCH_INTERVAL_MS;
    CHECK(scheduler.tick(secondAttempt + 1, true, false) ==
          weather_fetch::Action::None);
    CHECK(scheduler.tick(thirdAttempt, true, false) ==
          weather_fetch::Action::Fetch);
    scheduler.completed(thirdAttempt, false);
    CHECK(scheduler.tick(thirdAttempt + 1, true, false) ==
          weather_fetch::Action::None);
    CHECK(scheduler.tick(thirdAttempt + weather_fetch::RETRY_AFTER_MS, true,
                         false) == weather_fetch::Action::Fetch);
    // A failure followed by a success restores the long cadence.
    scheduler.completed(thirdAttempt + weather_fetch::RETRY_AFTER_MS, true);
    CHECK(scheduler.tick(thirdAttempt + weather_fetch::RETRY_AFTER_MS +
                             weather::FETCH_INTERVAL_MS - 1,
                         true, false) == weather_fetch::Action::None);
    CHECK(scheduler.tick(thirdAttempt + weather_fetch::RETRY_AFTER_MS +
                             weather::FETCH_INTERVAL_MS,
                         true, false) == weather_fetch::Action::Fetch);
    const uint32_t lastBooked = thirdAttempt + weather_fetch::RETRY_AFTER_MS +
                                weather::FETCH_INTERVAL_MS;
    scheduler.completed(lastBooked, true);
    // The portal suppresses attempts but never shifts the schedule: when it
    // closes, a due attempt fires at once.
    CHECK(scheduler.tick(lastBooked + weather::FETCH_INTERVAL_MS - 100, true,
                         true) == weather_fetch::Action::None);
    CHECK(scheduler.tick(lastBooked + weather::FETCH_INTERVAL_MS + 5000, true,
                         true) == weather_fetch::Action::None);
    CHECK(scheduler.tick(lastBooked + weather::FETCH_INTERVAL_MS + 5001, true,
                         false) == weather_fetch::Action::Fetch);
    // millis() wrap: the unsigned delta still decides, both intervals.
    weather_fetch::Scheduler successWrap;
    CHECK(successWrap.tick(UINT32_MAX - 1, true, false) ==
          weather_fetch::Action::Fetch);
    successWrap.completed(UINT32_MAX - 1, true);
    CHECK(successWrap.tick(UINT32_MAX, true, false) ==
          weather_fetch::Action::None);
    CHECK(successWrap.tick(static_cast<uint32_t>(
                               UINT32_MAX - 1 + weather::FETCH_INTERVAL_MS),
                           true, false) == weather_fetch::Action::Fetch);
    weather_fetch::Scheduler failureWrap;
    CHECK(failureWrap.tick(UINT32_MAX - 1, true, false) ==
          weather_fetch::Action::Fetch);
    failureWrap.completed(UINT32_MAX - 1, false);
    CHECK(failureWrap.tick(UINT32_MAX, true, false) ==
          weather_fetch::Action::None);
    CHECK(failureWrap.tick(static_cast<uint32_t>(
                               UINT32_MAX - 1 + weather_fetch::RETRY_AFTER_MS),
                           true, false) == weather_fetch::Action::Fetch);
  } else if (test == "stale") {
    weather_fetch::Scheduler scheduler;
    // A scheduler that has never seen a successful fetch reports stale —
    // the /health field must be able to say "nothing usable" without a
    // separate flag.
    CHECK(scheduler.stale(0));
    CHECK(scheduler.stale(123456));
    // Fresh after a success; the boundary is exactly STALE_AFTER_MS.
    scheduler.completed(1000, true);
    CHECK(!scheduler.stale(1000));
    CHECK(!scheduler.stale(1000 + weather_fetch::STALE_AFTER_MS - 1));
    CHECK(scheduler.stale(1000 + weather_fetch::STALE_AFTER_MS));
    CHECK(scheduler.stale(1000 + weather_fetch::STALE_AFTER_MS + 1));
    // A later FAILED attempt does NOT make a held forecast stale: the
    // record stays usable exactly to the same boundary it already had.
    // Mid-window the cadence is not due yet, but a completed(false) from
    // a real attempt still must not shorten the staleness clock.
    scheduler.completed(20000, false);
    CHECK(!scheduler.stale(1000 + weather_fetch::STALE_AFTER_MS - 1));
    CHECK(scheduler.stale(1000 + weather_fetch::STALE_AFTER_MS));
    // millis() wrap around the staleness boundary.
    weather_fetch::Scheduler wrapper;
    CHECK(wrapper.stale(UINT32_MAX - 1));
    wrapper.completed(UINT32_MAX - 1, true);
    CHECK(!wrapper.stale(static_cast<uint32_t>(UINT32_MAX - 1 +
                                               weather_fetch::STALE_AFTER_MS -
                                               1)));
    CHECK(wrapper.stale(static_cast<uint32_t>(UINT32_MAX - 1 +
                                              weather_fetch::STALE_AFTER_MS)));
  } else if (test == "forecast") {
    // 200 + a canonical body: an updated record in the REQUESTED unit.
    FakeTransport transport;
    transport.next = {true, 200, CANONICAL_BODY};
    weather::Forecast previous;
    previous.valid = true;
    previous.unit = weather::Unit::Fahrenheit;
    previous.tmax[0] = 1;
    weather::Forecast updated = previous;
    CHECK(weather_fetch::applyForecast(transport.next,
                                       weather::Unit::Celsius, updated));
    CHECK(updated.valid);
    CHECK(updated.unit == weather::Unit::Celsius);
    CHECK(updated.tmax[0] == 124);
    CHECK(updated.tmin[1] == -25);
    CHECK(updated.prob[2] == 80);
    CHECK(updated.tmax[3] == 98);
    // Rejection discipline: EVERY failure leaves the previous record
    // untouched, never a partial update.
    const weather::Forecast untouched = previous;
    const weather_fetch::Transport::Response bad[] = {
        {false, 200, CANONICAL_BODY},  // transport error, no body at all
        {true, 404, "{}"},
        {true, 500, CANONICAL_BODY},   // a good-looking body on a bad status
        {true, 200, ""},               // empty body
        {true, 200, "{}"},             // no daily arrays
        {true, 200,
         "{\"daily\":{\"temperature_2m_max\":[1,2,3]}}"}, // truncated array
        {true, 200, "{\"error\":true,\"reason\":\"Bad Request\"}"},
    };
    for (const weather_fetch::Transport::Response &response : bad) {
      weather::Forecast probe = previous;
      CHECK(!weather_fetch::applyForecast(response, weather::Unit::Celsius,
                                          probe));
      CHECK(sameForecast(probe, untouched));
    }
    // The size boundary: an oversized body is rejected whatever it holds;
    // a body of exactly BODY_MAX parses (here: valid body padded with
    // trailing spaces, which the needle extraction ignores).
    weather_fetch::Transport::Response oversized = {true, 200, ""};
    oversized.body = std::string(CANONICAL_BODY) +
                     std::string(weather_fetch::BODY_MAX, ' ');
    CHECK(oversized.body.size() > weather_fetch::BODY_MAX);
    weather::Forecast probe = previous;
    CHECK(!weather_fetch::applyForecast(oversized, weather::Unit::Celsius,
                                        probe));
    CHECK(sameForecast(probe, untouched));
    weather_fetch::Transport::Response exact = {true, 200, ""};
    const size_t pad = weather_fetch::BODY_MAX - std::strlen(CANONICAL_BODY);
    exact.body = std::string(CANONICAL_BODY) + std::string(pad, ' ');
    CHECK(exact.body.size() == weather_fetch::BODY_MAX);
    CHECK(weather_fetch::applyForecast(exact, weather::Unit::Celsius, probe));
    CHECK(probe.tmax[0] == 124);
  } else if (test == "city") {
    // 200 + a canonical geocoding body: the resolved record.
    weather_fetch::Transport::Response good = {true, 200, CANONICAL_CITY};
    weather::CityRecord city;
    CHECK(weather_fetch::applyCity(good, city));
    CHECK(city.valid);
    CHECK(city.name == "Madrid");
    CHECK(city.latMicro == 40416775);
    CHECK(city.lonMicro == -3703790);
    // Rejection discipline, previous record untouched on every failure.
    weather::CityRecord previous;
    previous.valid = true;
    previous.latMicro = 37382830;
    previous.lonMicro = -5973170;
    previous.name = "Sevilla";
    const weather::CityRecord untouched = previous;
    const weather_fetch::Transport::Response bad[] = {
        {false, 200, CANONICAL_CITY}, // transport error
        {true, 503, CANONICAL_CITY},  // good-looking body on a bad status
        {true, 200, ""},              // empty body
        {true, 200, "{\"results\":[]}"}, // unknown city
        {true, 200, "{\"error\":true,\"reason\":\"...\"}"},
        {true, 200, std::string(weather_fetch::BODY_MAX + 1, ' ')},
    };
    for (const weather_fetch::Transport::Response &response : bad) {
      weather::CityRecord probe = previous;
      CHECK(!weather_fetch::applyCity(response, probe));
      CHECK(sameCity(probe, untouched));
    }
    // Query validation feeding searchCity: empty and oversized names are
    // rejected before any transport call.
    CHECK(weather_fetch::validQuery("Sevilla"));
    CHECK(!weather_fetch::validQuery(""));
    CHECK(!weather_fetch::validQuery(std::string(
        weather_fetch::QUERY_MAX_BYTES + 1, 'x')));
  } else if (test == "transport") {
    FakeTransport transport;
    // The forecast request goes to the documented host with the committed
    // path, built from the city record and the unit choice.
    transport.next = {true, 200, CANONICAL_BODY};
    weather::Forecast forecast;
    const weather::CityRecord city = weather::defaultCity();
    CHECK(weather_fetch::fetchForecast(transport, city,
                                       weather::Unit::Fahrenheit, forecast));
    CHECK(transport.calls.size() == 1);
    CHECK(transport.calls[0].host == weather::FORECAST_HOST);
    CHECK(transport.calls[0].path ==
          weather::forecastPath(city.latMicro, city.lonMicro,
                                weather::Unit::Fahrenheit));
    CHECK(forecast.valid && forecast.unit == weather::Unit::Fahrenheit);
    // An unusable city record never reaches the transport and changes
    // nothing: no fetch for a record that has no coordinates.
    transport.calls.clear();
    transport.next = {true, 200, CANONICAL_BODY};
    weather::CityRecord invalid;
    weather::Forecast untouched;
    CHECK(!weather_fetch::fetchForecast(transport, invalid,
                                        weather::Unit::Celsius, untouched));
    CHECK(transport.calls.empty());
    CHECK(!untouched.valid);
    // A failed transport response leaves the previous forecast intact (the
    // caller then keeps displaying it) and there is no second call.
    transport.calls.clear();
    transport.next = {false, 0, ""};
    weather::Forecast previous = forecast;
    CHECK(!weather_fetch::fetchForecast(transport, city,
                                        weather::Unit::Celsius, previous));
    CHECK(transport.calls.size() == 1);
    CHECK(sameForecast(previous, forecast));
    // searchCity: right host and path (the name percent-encoded), a
    // resolved record on success, nothing on failure.
    transport.calls.clear();
    transport.next = {true, 200, CANONICAL_CITY};
    weather::CityRecord resolved;
    CHECK(weather_fetch::searchCity(transport, "A Coru\xc3\xb1" "a",
                                    resolved));
    CHECK(transport.calls.size() == 1);
    CHECK(transport.calls[0].host == weather::GEOCODE_HOST);
    CHECK(transport.calls[0].path ==
          "/v1/search?name=A%20Coru%C3%B1a&count=1&language=es");
    CHECK(resolved.valid && resolved.name == "Madrid");
    transport.calls.clear();
    transport.next = {true, 200, "{\"results\":[]}"};
    weather::CityRecord kept = resolved;
    CHECK(!weather_fetch::searchCity(transport, "Nowhere", kept));
    CHECK(sameCity(kept, resolved));
    // An invalid query is rejected BEFORE the transport is touched.
    transport.calls.clear();
    transport.next = {true, 200, CANONICAL_CITY};
    CHECK(!weather_fetch::searchCity(transport, "", kept));
    CHECK(!weather_fetch::searchCity(
        transport, std::string(weather_fetch::QUERY_MAX_BYTES + 1, 'x'),
        kept));
    CHECK(transport.calls.empty());
  } else {
    std::cerr << "unknown case\n";
    return 2;
  }
  std::cout << "OK\n";
  return 0;
}
