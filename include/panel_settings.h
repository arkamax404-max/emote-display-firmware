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

// S8-S10 (T11-T13): the LAN panel-settings surface served by the station
// HTTP server at GET/POST /config — pure, host-testable parsing and
// rendering in the header-only style of include/portal_policy.h (no
// Arduino/WiFi symbols). The value rules live with their domains: the
// brightness percent set and the percent->scale mapping in
// include/display_settings.h, the 12/24 h format and its storage code in
// include/idle_clock.h. The station serving loop decides the route on the
// exact request line BELOW this module (routeOf), outside the receiver
// protocol parser, so the receiver's own routes keep their exact semantics.
namespace panel_settings {

constexpr const char *CONFIG_PATH = "/config";

// The settings form body is tiny (two short keys); bounded like the portal
// forms so the station server's one-client, 2.5 s request limits always
// cover it.
constexpr size_t CONFIG_BODY_MAX = 255;

// S9/S10 persistence transaction: the settings pair is stored through an
// injected store and every decision is made by READ-BACK — a putUChar-style
// return value is never trusted and zero is a valid stored code (the H24
// default). The pair is atomic:
// - both commit writes are followed by a read-back; any miss triggers the
//   rollback,
// - every rollback write is attempted (even after an earlier rollback miss)
//   and followed by its own read-back; a rollback write that cannot be
//   persisted or lies leaves the result unverified,
// - the call reports success ONLY after a fully verified store, so the
//   caller applies the live values (and repaints the panel) only then — a
//   failed or unverified call changes no live state, even when the stored
//   bytes could not be perfectly restored.
// An absent stored state stays distinguishable from any stored value, so a
// rollback can faithfully restore "was not stored".
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
};

inline bool storeSettings(SettingsStore &store, uint8_t brightnessPercent,
                          idle_clock::ClockFormat clock) {
  if (!panel::validBrightnessPercent(brightnessPercent)) return false;
  const bool hadBrightness = store.hasUChar("brightness");
  uint8_t previousBrightness = 0;
  if (hadBrightness && !store.readUChar("brightness", previousBrightness))
    return false;
  const bool hadClock = store.hasUChar("clockformat");
  uint8_t previousClock = 0;
  if (hadClock && !store.readUChar("clockformat", previousClock)) return false;
  const uint8_t clockCode = idle_clock::clockFormatCode(clock);
  auto verifiedWrite = [](SettingsStore &s, const char *key, uint8_t value) {
    s.writeUChar(key, value);
    uint8_t back = 0;
    return s.readUChar(key, back) && back == value;
  };
  const bool brightnessOk =
      verifiedWrite(store, "brightness", brightnessPercent);
  const bool clockOk = verifiedWrite(store, "clockformat", clockCode);
  if (brightnessOk && clockOk) return true;
  // Rollback: every restore is attempted and verified by read-back; nothing
  // here can turn a failed store into success.
  auto restore = [](SettingsStore &s, bool existed, const char *key,
                    uint8_t value) {
    if (!existed) {
      s.removeUChar(key);
      uint8_t back = 0;
      return !s.hasUChar(key) && !s.readUChar(key, back);
    }
    s.writeUChar(key, value);
    uint8_t back = 0;
    return s.readUChar(key, back) && back == value;
  };
  restore(store, hadBrightness, "brightness", previousBrightness);
  restore(store, hadClock, "clockformat", previousClock);
  return false;
}

// Exact first-line classification for the settings routes (S8). Only these
// exact lines belong to the settings surface; everything else — every
// receiver route and the unknown paths — is left to the receiver parser
// exactly as before, including its unknown-path 404.
enum class Route : uint8_t { Other, Get, Save };
inline Route routeOf(const std::string &firstLine) {
  if (firstLine == "GET /config HTTP/1.1" || firstLine == "GET /config HTTP/1.0")
    return Route::Get;
  if (firstLine == "POST /config HTTP/1.1" || firstLine == "POST /config HTTP/1.0")
    return Route::Save;
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

// application/x-www-form-urlencoded POST /config body: exactly the keys
// `brightness` and `clock`, each at most once, no unknown keys; '+' is a
// space and %XX requires two hex digits (the shared portal urlencoded
// discipline, so malformed bodies are rejected, not coerced).
struct SettingsFields {
  bool ok = false;
  std::string brightness, clock;
};

inline SettingsFields parseSettingsForm(const char *body, size_t length) {
  SettingsFields result;
  const std::string input(body, length);
  bool sawBrightness = false, sawClock = false;
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
      if (key == "brightness") {
        if (sawBrightness) return result;
        result.brightness = value;
        sawBrightness = true;
      } else if (key == "clock") {
        if (sawClock) return result;
        result.clock = value;
        sawClock = true;
      } else {
        return result;
      }
    }
    if (last) break;
    start = amp + 1;
  }
  result.ok = sawBrightness && sawClock;
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

// GET /config (S8): the settings page with the current values preselected.
// No authentication by decision — the page lives on the local network like
// every other route of this server. An error re-renders inside a visible
// error block carrying only the escaped reason. Neither setting needs a
// restart to take effect, which the page says plainly.
inline std::string configPage(uint8_t brightnessPercent,
                              idle_clock::ClockFormat clock,
                              const std::string &error = "") {
  std::string page =
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>gentle-matrix panel settings</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em}"
      "select{display:block;width:100%;box-sizing:border-box;"
      "margin:0.25em 0 1em;padding:0.5em}"
      "button{padding:0.5em 1em}"
      ".error{color:#b00;font-weight:bold}"
      "</style></head><body>"
      "<h1>gentle-matrix</h1>"
      "<p>Panel settings. Changes apply immediately &mdash; no restart "
      "needed.</p>";
  if (!error.empty())
    page += "<p class=\"error\">" + portal_page::escapeHtml(error) + "</p>";
  page += "<form method=\"POST\" action=\"/config\">"
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
          "<button type=\"submit\">Apply</button>"
          "</form></body></html>";
  return page;
}

// The success page: the settings were persisted and are already applied —
// no restart, unlike the portal forms.
inline std::string configSavedPage() {
  return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>gentle-matrix settings saved</title></head><body>"
         "<h1>gentle-matrix</h1>"
         "<p>Settings saved and applied immediately &mdash; no restart "
         "needed. <a href=\"/config\">Back to the settings</a>.</p></body></html>";
}
} // namespace panel_settings
