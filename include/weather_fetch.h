#pragma once
#include "weather.h"
#include <cstddef>
#include <cstdint>
#include <string>

// Fetch layer (T3): URL composition over the committed pure path builders,
// the fetch cadence as a pure state machine with injected timestamps, the
// staleness report and the whole request/response decision path behind an
// injected transport interface — so the host tests fake status, body and
// error without a socket. No Arduino/WiFi symbols; the real transport
// (WiFiClientSecure) is bound by the caller in T4 and MUST honour the
// documented timeout budget below.
namespace weather_fetch {

// --- the documented timeout budget the T4 caller must honour ---------------
// One attempt is at most CONNECT_TIMEOUT_MS to open the TLS connection
// (WiFiClientSecure::connect(host, weather::HTTPS_PORT, CONNECT_TIMEOUT_MS)
// — the TLS handshake is part of that connect) plus READ_TIMEOUT_MS of
// read budget (setTimeout() takes WHOLE SECONDS on this core: 5). S5 risk
// decision: setInsecure() — no certificate validation; the forecast data is
// not secret and the risk is accepted and documented. Neither request is
// ever issued while the AP portal is open. Worst case an attempt blocks the
// loop for CONNECT+READ = 15 s, but only once per FETCH_INTERVAL_MS on
// success (~0.3% duty) or once per RETRY_AFTER_MS after a failure (≤ 2.5%
// during an outage), and a failed attempt keeps the previous forecast on
// the panel.
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t READ_TIMEOUT_MS = 5000;

// Bounded HTTP body: the forecast body for four days is a few hundred
// bytes and a geocoding answer for one result stays under ~2 KiB; anything
// at or above this bound is rejected before parsing, never coerced.
constexpr size_t BODY_MAX = 4096;

// The geocoding search query is bounded too (T4 wires it to the portal form
// field): empty or over QUERY_MAX_BYTES is rejected before any request.
constexpr size_t QUERY_MAX_BYTES = 64;

// After how much time without a successful fetch the held forecast is
// reported stale: one full calendar day (the daily arrays stop describing
// "today" once midnight passes) plus twice the fetch interval as margin.
constexpr uint32_t STALE_AFTER_MS =
    24u * 60u * 60u * 1000u + 2u * weather::FETCH_INTERVAL_MS;

// --- URL composition --------------------------------------------------------
// The exact URLs over the committed builders (no duplication): the unit
// parameter appears ONLY in Fahrenheit mode, and a city record that never
// resolved (valid == false — nothing stored and the default replaced)
// builds no URL at all, so the caller cannot fetch without coordinates.
inline std::string forecastUrl(const weather::CityRecord &city,
                               weather::Unit unit) {
  if (!city.valid) return {};
  return std::string("https://") + weather::FORECAST_HOST +
         weather::forecastPath(city.latMicro, city.lonMicro, unit);
}

inline std::string geocodeUrl(const std::string &name) {
  return std::string("https://") + weather::GEOCODE_HOST +
         weather::geocodePath(name);
}

inline bool validQuery(const std::string &name) {
  return !name.empty() && name.size() <= QUERY_MAX_BYTES;
}

// --- transport ---------------------------------------------------------------
// The injected boundary the host tests fake. A response carries the
// transport-level verdict (ok: the request round-tripped at all), the HTTP
// status and the raw body; every decision below is made from these three
// fields, so the socket never leaks into the testable path.
class Transport {
 public:
  struct Response {
    bool ok = false;
    int status = 0;
    std::string body;
    Response() = default;
    Response(bool okValue, int statusValue, const char *bodyValue)
        : ok(okValue), status(statusValue), body(bodyValue) {}
    Response(bool okValue, int statusValue, const std::string &bodyValue)
        : ok(okValue), status(statusValue), body(bodyValue) {}
  };
  virtual ~Transport() = default;
  virtual Response get(const char *host, const char *path) = 0;
};

// --- cadence state machine ---------------------------------------------------
enum class Action : uint8_t { None, Fetch };

// The 60 s bounded retry after a failure: ~2.5% duty against a 15 s
// worst-case attempt, recovers from a transient loss within a minute while
// never hammering the API; a fixed interval stays auditable and needs no
// extra state, unlike an exponential ladder.
constexpr uint32_t RETRY_AFTER_MS = 60000;

// Pure decision core with injected timestamps (millis()), millis-wrap safe
// through unsigned deltas. Rules:
// - never due while disconnected, and never in portal/AP mode (the flag is
//   the caller's; the schedule is NOT shifted by a portal window — when it
//   closes, a due attempt fires at once),
// - the first moment the station is connected outside the portal fetches
//   immediately,
// - after a SUCCESS the next attempt is FETCH_INTERVAL_MS (30 min) later,
// - after a FAILURE the next attempt is RETRY_AFTER_MS later (bounded
//   retry, not hammering),
// - tick() books every attempt it returns (lastAttemptMs = now), so even a
//   caller that forgets completed() can never ask again sooner than one
//   retry interval; completed() only moves the bookkeeping that selects the
//   next interval and feeds staleness.
class Scheduler {
 public:
  Action tick(uint32_t now, bool connected, bool portalOrAp) {
    if (portalOrAp || !connected) return Action::None;
    if (!attempted_) {
      attempted_ = true;
      lastAttemptMs_ = now;
      return Action::Fetch;
    }
    const uint32_t interval =
        lastOk_ ? weather::FETCH_INTERVAL_MS : RETRY_AFTER_MS;
    if (static_cast<uint32_t>(now - lastAttemptMs_) < interval)
      return Action::None;
    lastAttemptMs_ = now;
    return Action::Fetch;
  }
  void completed(uint32_t now, bool ok) {
    lastOk_ = ok;
    if (ok) {
      succeeded_ = true;
      lastSuccessMs_ = now;
    }
  }
  // A held forecast is reported stale once STALE_AFTER_MS passed without a
  // successful fetch; a scheduler that never succeeded is stale from the
  // start. A later FAILED attempt never makes a held forecast stale.
  bool stale(uint32_t now) const {
    if (!succeeded_) return true;
    return static_cast<uint32_t>(now - lastSuccessMs_) >= STALE_AFTER_MS;
  }

 private:
  bool attempted_ = false;
  bool lastOk_ = false; // pessimistic default: silence counts as failure
  bool succeeded_ = false;
  uint32_t lastAttemptMs_ = 0;
  uint32_t lastSuccessMs_ = 0;
};

// --- response application -----------------------------------------------------
// Every failure — transport error, non-200 status, empty body, oversized
// body, or a body the committed weather::parseForecast rejects — leaves the
// previous record byte-for-byte intact and reports failure: never a
// partial update. On success the record is written whole, with the unit
// the request was made in (the renderer converts for display; the next
// successful fetch re-syncs the stored unit).
inline bool applyForecast(const Transport::Response &response,
                          weather::Unit unit, weather::Forecast &out) {
  if (!response.ok || response.status != 200) return false;
  if (response.body.empty() || response.body.size() > BODY_MAX) return false;
  if (!weather::parseForecast(response.body.data(), response.body.size(), out))
    return false;
  out.unit = unit;
  return true;
}

// Same discipline for the geocoding answer and the city record.
inline bool applyCity(const Transport::Response &response,
                      weather::CityRecord &out) {
  if (!response.ok || response.status != 200) return false;
  if (response.body.empty() || response.body.size() > BODY_MAX) return false;
  return weather::parseCity(response.body.data(), response.body.size(), out);
}

// The full forecast step: an unusable city record never reaches the
// transport, a valid one asks the documented host with the committed path;
// the bound and parsing rules live in applyForecast alone.
inline bool fetchForecast(Transport &transport, const weather::CityRecord &city,
                          weather::Unit unit, weather::Forecast &out) {
  if (!city.valid) return false;
  const std::string path = weather::forecastPath(city.latMicro, city.lonMicro, unit);
  return applyForecast(transport.get(weather::FORECAST_HOST, path.c_str()),
                       unit, out);
}

// The full geocoding step: an invalid query is rejected before the
// transport is touched; a valid one asks the geocoding host with the
// percent-encoded name.
inline bool searchCity(Transport &transport, const std::string &name,
                       weather::CityRecord &out) {
  if (!validQuery(name)) return false;
  const std::string path = weather::geocodePath(name);
  return applyCity(transport.get(weather::GEOCODE_HOST, path.c_str()), out);
}

} // namespace weather_fetch
