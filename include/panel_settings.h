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

// S7/S9/S10 persistence transaction: the settings triple is stored through
// an injected store and every decision is made by READ-BACK — a putUChar-style
// return value is never trusted and zero is a valid stored code (the H24
// default and the RGB default order). The transaction is atomic across the
// three keys (`brightness`, `clockformat`, `rgborder`):
// - the three commit writes are followed by a read-back; any miss triggers the
//   rollback,
// - every rollback write — including the failing key's own — is attempted
//   (even after an earlier rollback miss) and followed by its own read-back;
//   a rollback write that cannot be persisted or lies leaves the result
//   unverified,
// - the call reports success ONLY after a fully verified store, so the caller
//   applies the live values (repaints the panel; restarts only for an actual
//   order change) only then — a failed or unverified call changes no live
//   state, even when the stored bytes could not be perfectly restored.
// An absent stored state stays distinguishable from any stored value, so a
// rollback can faithfully restore "was not stored" for every one of the three
// keys.
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
                          idle_clock::ClockFormat clock, rgb_order::Order order) {
  if (!panel::validBrightnessPercent(brightnessPercent)) return false;
  // The three keys of this one transaction, in their fixed write order:
  // `brightness` (the percent byte), `clockformat` (the format code) and
  // `rgborder` (the order code 0-5, RGB = 0).
  const struct Key {
    const char *name;
    uint8_t code;
  } keys[3] = {{"brightness", brightnessPercent},
               {"clockformat", idle_clock::clockFormatCode(clock)},
               {"rgborder", rgb_order::orderCode(order)}};
  bool existed[3] = {false, false, false};
  uint8_t previous[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    existed[i] = store.hasUChar(keys[i].name);
    if (existed[i] && !store.readUChar(keys[i].name, previous[i])) return false;
  }
  auto verifiedWrite = [](SettingsStore &s, const char *key, uint8_t value) {
    s.writeUChar(key, value);
    uint8_t back = 0;
    return s.readUChar(key, back) && back == value;
  };
  bool allOk = true;
  // Every commit write is attempted — even after an earlier miss — matching
  // both commit writes of the previous pair version; the read-backs decide.
  for (int i = 0; i < 3; ++i)
    allOk = verifiedWrite(store, keys[i].name, keys[i].code) && allOk;
  if (allOk) return true;
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
  for (int i = 0; i < 3; ++i)
    restore(store, existed[i], keys[i].name, previous[i]);
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
// `brightness`, `clock` and `order`, each at most once, no unknown keys;
// '+' is a space and %XX requires two hex digits (the shared portal
// urlencoded discipline, so malformed bodies are rejected, not coerced).
struct SettingsFields {
  bool ok = false;
  std::string brightness, clock, order;
};

inline SettingsFields parseSettingsForm(const char *body, size_t length) {
  SettingsFields result;
  const std::string input(body, length);
  bool sawBrightness = false, sawClock = false, sawOrder = false;
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
      } else if (key == "order") {
        if (sawOrder) return result;
        result.order = value;
        sawOrder = true;
      } else {
        return result;
      }
    }
    if (last) break;
    start = amp + 1;
  }
  result.ok = sawBrightness && sawClock && sawOrder;
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

// GET /config (S8): the settings page with the current values preselected.
// No authentication by decision — the page lives on the local network like
// every other route of this server. An error re-renders inside a visible
// error block carrying only the escaped reason. Brightness and clock format
// need no restart to take effect; a changed channel order does, which the
// page states plainly — the display driver fixes its pins at construction.
inline std::string configPage(uint8_t brightnessPercent,
                              idle_clock::ClockFormat clock,
                              rgb_order::Order order,
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
      "<p>Panel settings. Brightness and clock format apply immediately. "
      "Changing the panel colour order restarts the board to apply it: the "
      "display driver fixes its pins at construction.</p>";
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
} // namespace panel_settings
