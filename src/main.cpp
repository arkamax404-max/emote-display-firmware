#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <Adafruit_LIS3DH.h>
#include <Wire.h>
#include "orientation.h"
#include "display_settings.h"
#include "wifi_info.h"
#include <ESPmDNS.h>
#include <esp_timer.h>
#include "frame_decoder.h"
#include <Preferences.h>
#include <WiFi.h>
#include "receiver_protocol.h"
#include "serial_protocol.h"
#include "serial_ingress.h"
#include "image_ingress.h"
#include "diagnostic_status.h"
#include "idle_clock_image.h"
#include "firmware_version.h"
#include "portal_policy.h"
#include "portal_page.h"
#include "rgb_order.h"
#include "panel_settings.h"
#include "wifi_credentials_reset.h"
#include "weather.h"
#include "weather_screen.h"
#include "weather_fetch.h"
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <esp_system.h>
#include <new> // placement construction of the panel after the order is known

// TI USB gate: a USB HOST is enumerated by the bus — what S1 asks for — not
// Serial's operator bool(), which only turns true with DTR+RTS asserted and
// would therefore claim a charger or a closed terminal is a computer.
// Declared here instead of pulling the full TinyUSB headers for one banner
// call; the symbol ships with the core's TinyUSB CDC support.
extern "C" bool tud_mounted(void);

static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37}; // base wiring R1 G1 B1 R2 G2 B2; permuted per the S7 channel order before construction
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
// S7/T10: Adafruit_Protomatter copies the pin list inside its constructor
// (core.c _PM_init memcpy), which runs at static init — before NVS is
// reachable — so the persisted channel order could never reach an ordinary
// static object. The panel is instead constructed on this aligned storage
// in setup(), immediately after the stored order is applied to rgbPins; the
// `matrix` reference keeps every call site below unchanged.
alignas(Adafruit_Protomatter) static uint8_t panelStorage[sizeof(Adafruit_Protomatter)];
static Adafruit_Protomatter &matrix = *reinterpret_cast<Adafruit_Protomatter *>(panelStorage);
static rgb_order::Order channelOrder = rgb_order::Order::RGB; // applied before construction, rendered in the LAN settings page and the portal selector
static FrameDecoder frameDecoder;
static Adafruit_LIS3DH accelerometer;
static Orientation orientation;
static bool accelerometerReady = false;
static uint16_t lastFrame[64 * 64];
static bool hasFrame = false;
// Classification of lastFrame, updated only with a successful decode so an
// invalid PNG never changes what is redrawn.
static bool frameIsClock = false;
static const char *messageFirst = "", *messageSecond = "";
static WiFiServer server(80);
static WiFiClient client;
static session::Owner sessionOwner;
static Preferences preferences;
static uint8_t pngBytes[receiver::MAX_PNG];
static uint32_t accepted = 0;
static constexpr uint32_t REQUEST_TIMEOUT_MS = 2500;
static std::string headers;
static receiver::Request request;
static size_t received = 0;
static bool readingBody = false;
// S8 /config request state: the first request line is classified before the
// receiver protocol parser sees anything; the bounded settings form body
// accumulates in its own small buffer, never in the PNG payload buffer.
static bool configFirstLineChecked = false;
// A first line classified as POST /config lingers until the complete header
// block (see handleConfigRequestLine/beginConfigSave).
static bool configSavePending = false;
// The request belongs to the settings surface (`/config`): from the first-line
// classification until the request closes. The timeout branch uses it to
// re-render the settings page — the rejection rule of that surface — while
// every other request keeps the generic JSON body.
static bool configSurface = false;
static std::string configBody;
static size_t configBodyLength = 0;
// T4b city-search route state: the POST /config/city form lingers like the
// settings POST and its body reuses the same bounded settings buffer. The
// geocoding request runs synchronously inside the serving loop (the
// request timeout check cannot fire mid-handler, so no exception is
// needed) and may hold the loop for at most the transport budget —
// documented, bounded, and never while a PNG body is being read.
static bool citySavePending = false;
static bool configBodyIsCity = false;
static uint32_t requestStarted = 0;;
static bool listening = false;
static String ssid, password;

// WiFi captive portal fallback (T2-T4). The portal owns its own server,
// client and DNS responder while open; the station server below is mutually
// exclusive with it — pollWifi() returns while portalActive and the portal
// server is begun only while !listening, so port 80 and the two serving
// loops can never run at the same time.
static WiFiServer portalServer(80);
static WiFiClient portalClient;
static DNSServer dnsServer;
static bool portalActive = false;
enum class PortalPhase : uint8_t { Portal, Validating };
static PortalPhase portalPhase = PortalPhase::Portal;
static uint32_t validationStarted = 0;
// Portal HTTP request state: entirely separate from the station server's
// client/headers/request so neither serving loop can corrupt the other.
static std::string portalRequest;
static size_t portalHeaderEnd = 0;
static size_t portalContentLength = 0;
static uint32_t portalRequestStarted = 0;
// Boot-only retry schedule (S2): 4 x 10 s attempts, 5 s pauses; once the
// boot phase is over a later link loss keeps the runtime reconnect loop and
// never opens the portal.
static portal::BootRetry bootRetry;
static bool bootPhase = false;
// The offered home-network credentials read from POST /save. Never logged,
// echoed or displayed anywhere; zeroed as soon as the validation attempt
// concludes, whatever its outcome.
static char offeredSsid[33];
static char offeredPassword[portal::AP_PASSWORD_MAX + 1];
// The generated AP password (S3): intended to be read off the panel. It
// never reaches the HTML, the serial log, /health or NVS.
static char apPassword[portal::AP_PASSWORD_LEN + 1];
static std::string portalError; // last save failure, shown until reboot
// Portal panel slides (T4), through the existing displayMessage path only:
// the AP label with the password, the PORTAL banner with the setup hint and
// the AP address. The wider phase-3 strings keep the 10-char line budget by
// splitting the IP across both lines, exactly like the Wi-Fi info overlay.
static uint8_t portalSlide = 0;
static uint32_t slideSince = 0;
static void showPortalSlide();
static void pollPortal();

// Credential storage shared by USB provisioning and the portal (S4): same
// namespace, same keys, same read-back verification. putString returns zero
// for an empty password even on success, hence the isKey guard. A partial
// write is rolled back: S4 promises the stored credentials stay untouched,
// so a failed save must not leave a mixed old/new pair behind.
static bool storeCredentials(const char *newSsid, const char *newPassword) {
  bool saved = false;
  if (preferences.begin("gentle", false)) {
    const bool hadSsid = preferences.isKey("ssid");
    const bool hadPassword = preferences.isKey("password");
    const String previousSsid = preferences.getString("ssid", "");
    const String previousPassword = preferences.getString("password", "");
    preferences.putString("password", newPassword);
    preferences.putString("ssid", newSsid);
    saved = preferences.isKey("password") && preferences.getString("password") == newPassword &&
            preferences.getString("ssid") == newSsid;
    if (!saved) {
      if (hadPassword) preferences.putString("password", previousPassword);
      else preferences.remove("password");
      if (hadSsid) preferences.putString("ssid", previousSsid);
      else preferences.remove("ssid");
    }
    preferences.end();
  }
  return saved;
}

// S7/T10: the channel order persists as a small code (0-5) under its own
// key `rgborder` in the same `gentle` namespace — deliberately NOT one of
// the DOWN reset keys (those stay exactly ssid/password/updir). putUChar
// returns zero for the value 0 (the RGB default), so like the credential
// store the put result is never trusted: read-back decides.
static bool storeRgbOrder(rgb_order::Order order) {
  bool saved = false;
  if (preferences.begin("gentle", false)) {
    preferences.putUChar("rgborder", rgb_order::orderCode(order));
    saved = preferences.isKey("rgborder") &&
            preferences.getUChar("rgborder", 255) == rgb_order::orderCode(order);
    preferences.end();
  }
  return saved;
}

// S9: the clock display format is runtime state like the channel order,
// defaulting to the untouched 24 h.
static idle_clock::ClockFormat clockFormat = idle_clock::ClockFormat::H24;

// S7/S9/S10 settings persistence: all three live in the `gentle` namespace
// under their own keys (`brightness`, the percent byte; `clockformat`, the
// format code; `rgborder`, the order code 0-5). The commit/rollback/
// verification rules live in the host-tested panel_settings::storeSettings
// transaction, which now spans the THREE keys as one atomic triple; the
// firmware only adapts an open Preferences namespace onto it. No key is in
// the DOWN-reset scope (wifi_reset erases exactly ssid/password/updir). An
// absent or out-of-range stored value falls back to the defaults (30 %, 24 h,
// RGB — the order decode happens in setup(), before the panel exists).
class ProvisionedSettingsStore : public panel_settings::SettingsStore {
 public:
  explicit ProvisionedSettingsStore(Preferences &prefs) : preferences_(prefs) {}
  bool hasUChar(const char *key) override { return preferences_.isKey(key); }
  // Absence (isKey false) is returned as false, never as a sentinel value:
  // rollback must be able to restore "was not stored" faithfully.
  bool readUChar(const char *key, uint8_t &value) override {
    if (!preferences_.isKey(key)) return false;
    value = preferences_.getUChar(key, 255);
    return true;
  }
  // Fire-and-verify: putUChar's return is never trusted (it is zero for a
  // legitimately stored 0); the transaction's read-back decides.
  void writeUChar(const char *key, uint8_t value) override {
    preferences_.putUChar(key, value);
  }
  void removeUChar(const char *key) override { preferences_.remove(key); }
  // T4b typed adapters for the weather keys: the int32 micro-degrees and
  // the clamped city-name string, written fire-and-verify exactly like the
  // byte keys — the transaction's read-back decides, never the put value.
  bool hasInt32(const char *key) override { return preferences_.isKey(key); }
  bool readInt32(const char *key, int32_t &value) override {
    if (!preferences_.isKey(key)) return false;
    // This core's Preferences calls its int32 accessors getInt/putInt.
    value = preferences_.getInt(key, 0);
    return true;
  }
  void writeInt32(const char *key, int32_t value) override {
    preferences_.putInt(key, value);
  }
  bool hasString(const char *key) override { return preferences_.isKey(key); }
  bool readString(const char *key, std::string &value) override {
    if (!preferences_.isKey(key)) return false;
    value = std::string(preferences_.getString(key, "").c_str());
    return true;
  }
  void writeString(const char *key, const std::string &value) override {
    preferences_.putString(key, String(value.c_str()));
  }

 private:
  Preferences &preferences_;
};

static bool storePanelSettings(uint8_t percent, idle_clock::ClockFormat clock,
                               rgb_order::Order order, uint8_t clockSeconds,
                               uint8_t weatherSeconds, weather::Unit unit) {
  if (!preferences.begin("gentle", false)) return false;
  static ProvisionedSettingsStore store(preferences);
  // T4b: all six keys of POST /config persist as ONE verified transaction
  // (the combined storeAllSettings), so a failure never leaves the stored
  // settings mixed across the panel and weather surfaces.
  const bool verified = panel_settings::storeAllSettings(
      store, percent, clock, order, clockSeconds, weatherSeconds, unit);
  preferences.end();
  return verified;
}

// The resolved city persists through the committed THREE-key city
// transaction (citylat/citylon/cityname, typed on the same store), also
// verified by read-back; a failed store changes nothing.
static bool storeWeatherCity(const weather::CityRecord &city) {
  if (!preferences.begin("gentle", false)) return false;
  static ProvisionedSettingsStore store(preferences);
  const bool verified = panel_settings::storeCity(store, city);
  preferences.end();
  return verified;
}

static void loadPanelSettings() {
  if (!preferences.begin("gentle", true)) return;
  const uint8_t storedBrightness = preferences.getUChar("brightness", 255);
  const uint8_t storedFormat = preferences.getUChar("clockformat", 255);
  preferences.end();
  if (panel::validBrightnessPercent(storedBrightness))
    panel::setBrightnessPercent(storedBrightness);
  idle_clock::ClockFormat format = idle_clock::ClockFormat::H24;
  if (idle_clock::clockFormatFromCode(storedFormat, format)) clockFormat = format;
}

// T4b weather runtime state: the held forecast (RAM only), the committed
// scheduler, and the settings loaded from storage below. A failed fetch
// keeps the previous record; a board with nothing stored starts with the
// Sevilla default city (S6) and the documented duration/unit defaults (S4).
static weather::Forecast weatherForecast;
static weather_fetch::Scheduler weatherScheduler;
static weather::Unit weatherUnit = weather::Unit::Celsius;
static uint8_t clockSecs = weather::CLOCK_SECONDS_DEFAULT;
static uint8_t weatherSecs = weather::WEATHER_SECONDS_DEFAULT;
static weather::CityRecord weatherCity;

// T4b: the weather settings load — each key falls back to its documented
// default through the committed loaders, including the whole Sevilla record
// for an absent or unusable city, so a fresh board already has a usable city
// and the clock never waits for a fetch.
static void loadWeatherSettings() {
  weatherCity = weather::defaultCity();
  weatherUnit = weather::Unit::Celsius;
  clockSecs = weather::CLOCK_SECONDS_DEFAULT;
  weatherSecs = weather::WEATHER_SECONDS_DEFAULT;
  if (!preferences.begin("gentle", true)) return;
  const uint8_t storedClockSecs = preferences.getUChar("clocksecs", 255);
  const uint8_t storedWeatherSecs = preferences.getUChar("weathersecs", 255);
  const uint8_t storedUnits = preferences.getUChar("units", 255);
  const bool storedCity = preferences.isKey("citylat") &&
                          preferences.isKey("citylon") &&
                          preferences.isKey("cityname");
  const int32_t storedLat = preferences.getInt("citylat", 0);
  const int32_t storedLon = preferences.getInt("citylon", 0);
  const std::string storedName(preferences.getString("cityname", "").c_str());
  preferences.end();
  clockSecs = panel_settings::clockSecondsOrDefault(storedClockSecs);
  weatherSecs = panel_settings::weatherSecondsOrDefault(storedWeatherSecs);
  weatherUnit = panel_settings::unitOrDefault(storedUnits);
  weatherCity = panel_settings::storedCityOrDefault(storedCity, storedLat,
                                                    storedLon, storedName);
}

// T4b transport: one HTTPS GET per call over WiFiClientSecure with
// setInsecure() — the S5 decision (no certificate validation; the risk is
// accepted and documented). The parent's T4b budget TIGHTENS the T3
// envelope: 5 s connect (the TLS handshake is part of connect) and 3 s of
// read budget. WiFiClientSecure's setTimeout() takes WHOLE SECONDS on this
// core, hence 3. The requests are exactly the committed URL builders' GETs
// on port 443 and nothing else.
class WeatherHttpsTransport : public weather_fetch::Transport {
 public:
  Response get(const char *host, const char *path) override {
    Response response;
    WiFiClientSecure secure;
    secure.setInsecure();
    secure.setTimeout(3);
    if (!secure.connect(host, weather::HTTPS_PORT,
                        USE_WEATHER_CONNECT_TIMEOUT_MS))
      return response;
    secure.printf(
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: gentle-matrix\r\n"
        "Connection: close\r\n\r\n",
        path, host);
    // Bounded raw read: at most twice the fetch body cap (an oversized body
    // is then rejected by the committed application rules, never coerced)
    // within the read budget.
    std::string raw;
    const uint32_t started = millis();
    while (raw.size() < weather_fetch::BODY_MAX * 2 &&
           static_cast<uint32_t>(millis() - started) <
               USE_WEATHER_READ_TIMEOUT_MS) {
      while (secure.available() &&
             raw.size() < weather_fetch::BODY_MAX * 2)
        raw.push_back(static_cast<char>(secure.read()));
      if (!secure.connected() && !secure.available()) break;
      delay(2);
    }
    if (raw.compare(0, 7, "HTTP/1.") != 0) return response;
    const size_t statusStart = raw.find(' ');
    if (statusStart == std::string::npos || statusStart + 4 > raw.size())
      return response;
    const std::string code = raw.substr(statusStart + 1, 3);
    if (code.find_first_not_of("0123456789") != std::string::npos)
      return response;
    response.ok = true;
    response.status = atoi(code.c_str());
    size_t bodyOffset = 0;
    if (weather::httpBodyStart(raw.data(), raw.size(), bodyOffset))
      response.body = raw.substr(bodyOffset);
    return response;
  }

 private:
  static constexpr uint32_t USE_WEATHER_CONNECT_TIMEOUT_MS = 5000;
  static constexpr uint32_t USE_WEATHER_READ_TIMEOUT_MS = 3000;
};

static WeatherHttpsTransport weatherTransport;

// T4b weather fetch driver: the committed scheduler decides; a fetch runs
// only outside the portal/AP window, only while the station is connected,
// and never while a station request is in flight or a PNG body is being
// read (the single station client makes `client` the in-flight marker). A
// failed attempt keeps the previous forecast; the blocking budget is the
// transport's 5 s + 3 s.
static void pollWeather() {
  if (portalActive) return;               // portal/AP mode owns the radio
  if (WiFi.status() != WL_CONNECTED) return;
  if (client) return;                     // a station request is in flight
  const weather_fetch::Action action =
      weatherScheduler.tick(millis(), true, false);
  if (action != weather_fetch::Action::Fetch) return;
  weather::Forecast candidate;
  if (weather_fetch::fetchForecast(weatherTransport, weatherCity, weatherUnit,
                                   candidate)) {
    weatherForecast = candidate;          // whole-record commit, unit synced
    weatherScheduler.completed(millis(), true);
  } else {
    weatherScheduler.completed(millis(), false); // previous record kept
  }
}

// DOWN long-press credential reset (T8, S6). The poller runs in every mode —
// connected, retrying or serving the portal — never reads or blocks Serial,
// and shows no panel message (the user chose silence; the restart is the
// only feedback). PIN_BUTTON_DOWN from the variant pins_arduino.h: GPIO 7,
// INPUT_PULLUP, active low; the UP button (GPIO 6) keeps its Wi-Fi info
// overlay untouched.
static wifi_reset::HoldReset downHold;

// Preferences adapter for the host-tested erase routine: the `gentle`
// namespace is already open read-write when eraseCredentials runs. The typed
// accessors serve the failure-restore path (values snapshotted before
// mutating and written back byte-identically on an unverified erase) and are
// restricted to exactly the keys of the reset scope, each with the type it
// actually has: `ssid`/`password` are strings, `updir` is the unsigned byte
// it actually is (preferences_.getUChar("updir")/preferences_.putUChar("updir"), matching setup()
// and pollOrientation), so a restore never turns a stored byte into an
// empty string.
class ProvisionedPrefs : public wifi_reset::CredentialStore {
 public:
  explicit ProvisionedPrefs(Preferences &prefs) : preferences_(prefs) {}
  bool hasKey(const char *key) override { return preferences_.isKey(key); }
  void removeKey(const char *key) override { preferences_.remove(key); }
  bool readString(const char *key, std::string &value) override {
    if (!isStringResetKey(key)) return false;
    value = std::string(preferences_.getString(key, "").c_str());
    return true;
  }
  bool writeString(const char *key, const std::string &value) override {
    if (!isStringResetKey(key)) return false;
    // putString() returns the written length, which is zero for a legitimately
    // empty password, so its return value cannot prove the write: verify by
    // reading the key back instead of trusting it.
    preferences_.putString(key, String(value.c_str()));
    return preferences_.isKey(key) && preferences_.getString(key, "") == value.c_str();
  }
  bool readUChar(const char *key, uint8_t &value) override {
    if (std::strcmp(key, "updir") != 0) return false;
    value = preferences_.getUChar("updir", 255); // 255 = absent, as setup() reads it
    return true;
  }
  bool writeUChar(const char *key, uint8_t value) override {
    if (std::strcmp(key, "updir") != 0) return false;
    return preferences_.putUChar("updir", value) != 0;
  }

 private:
  // The reset scope (S6 decision) is fixed: `ssid`, `password` (strings)
  // and `updir` (unsigned byte, always accessed as the literal "updir").
  static bool isStringResetKey(const char *key) {
    return std::strcmp(key, "ssid") == 0 ||
           std::strcmp(key, "password") == 0;
  }
  Preferences &preferences_;
};

// S6 verified erase: only `ssid`, `password` and `updir` are removed, each
// verified gone by read-back; brightness, the S7 `rgborder` channel order
// and every other stored value stay untouched, and nothing is written
// outside that namespace. False means the removal could not be verified
// (the caller then does NOT restart, so another hold can retry).
static bool clearProvisionedCredentials() {
  static ProvisionedPrefs store(preferences);
  if (!preferences.begin("gentle", false)) return false;
  const bool verified = wifi_reset::eraseCredentials(store);
  preferences.end();
  return verified;
}

static void pollCredentialsReset() {
  if (!downHold.update(millis(), digitalRead(PIN_BUTTON_DOWN) == LOW)) return;
  // Deliberately silent and non-blocking: the reset poller runs in every mode
  // and must not touch or hold the serial link, which the provisioning console
  // and the binary frame path own. The user chose no panel feedback either, so
  // the restart is the only signal.
  if (clearProvisionedCredentials()) ESP.restart();
}

static WifiInfoOverlay wifiInfo;
static SenderStats senderStats;
static SenderConnection senderConnection;
static uint32_t senderIp = 0;
static idle_clock::State clockState;
static bool clockConfigured = false;
// T7: serial RX diagnostic indicator state (shared with the host tests).
static diag::Status serialDiag;
static void resetOrientationReference();
// USB serial ingress (T2): a dedicated static payload buffer — the shared
// HTTP buffer is never touched by serial traffic — plus the parser, the
// binary-session multiplexer and the extracted provisioning console.
static uint8_t serialPng[receiver::MAX_PNG];
static serial::Parser serialParser(serialPng, receiver::MAX_PNG);
static serial::Mux serialMux(serialParser);
static serial::ProvisionConsole provisionConsole({
    [](const char *line) { Serial.println(line); },
    []() { Serial.flush(); },
    [](const char *newSsid, const char *newPassword) {
      return storeCredentials(newSsid, newPassword);
    },
    []() { resetOrientationReference(); },
    []() { delay(100); ESP.restart(); },
    [](rgb_order::Order order) { return storeRgbOrder(order); },
});
// At most one bounded reply is pending (stop-and-wait): no application queue.
static uint8_t serialReply[serial::REPLY_SIZE];
static size_t replyPending = 0, replySent = 0;

static bool readClock(std::tm &calendar, std::time_t &epoch) {
  epoch = std::time(nullptr);
  return localtime_r(&epoch, &calendar) && calendar.tm_year > 116;
}

static void renderContent() {
  std::tm calendar = {};
  std::time_t epoch = 0;
  const auto source = clockState.source(millis(), readClock(calendar, epoch), hasFrame, wifiInfo.visible());
  clockState.painted(millis(), source);
  matrix.setRotation(orientation.rotation());
  matrix.fillScreen(0); // Clear the top margin after every redraw/rotation.
  if (wifiInfo.visible()) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const auto lines = wifiInfoLines(wifiInfo.page(), connected, ssid.c_str(),
                                     connected ? WiFi.localIP().toString().c_str() : "",
                                     connected ? WiFi.RSSI() : 0, &senderStats);
    matrix.setTextWrap(false);
    matrix.setTextSize(1);
    for (size_t row = 0; row < lines.size(); ++row) {
      matrix.setTextColor(panel::color(row == 0 ? matrix.color565(128, 255, 255) : matrix.color565(224, 224, 224)));
      matrix.setCursor(2, panel::TOP_OFFSET + row * 8);
      matrix.print(lines[row].c_str());
    }
    return;
  }
  if (source == idle_clock::Source::Clock) {
    // The renderer owns the full 64-row canvas; no vertical offset here.
    // Half-second animation tick from the monotonic ESP timer: poses and
    // positions advance twice per second while the calendar seconds text
    // only changes once per second.
    const uint64_t animationTick = uint64_t(esp_timer_get_time()) / 500000;
    // S5 alternation phase from the absolute epoch (the same clock the
    // calendar labels use) with the sub-second fraction from the monotonic
    // timer, so every panel flips together; injectable in the native tests.
    const uint64_t epochMillis = uint64_t(epoch) * 1000 +
                                 uint64_t(esp_timer_get_time() / 1000) % 1000;
    // T4b: the clock and the weather screen alternate over the ABSOLUTE
    // epoch seconds (the same drift-free absolute base the title band
    // uses), with the configured durations; the weather screen shows only
    // when a forecast is held in RAM AND not stale. When it is not shown,
    // the clock render below is byte for byte exactly as before - this
    // block is the only difference on this path.
    if (weatherForecast.valid && !weatherScheduler.stale(millis()) &&
        weather::slotAt(static_cast<uint64_t>(epoch), clockSecs,
                        weatherSecs) == weather::Slot::Weather) {
      weather_screen::draw(weatherForecast, weatherUnit, calendar,
                           [](int x, int y, uint16_t color) {
        matrix.drawPixel(x, y, panel::color(color));
      });
      return;
    }
    idle_clock::draw(calendar, animationTick, epochMillis,
                     clockState.sessionTitle(millis()),
                     [](int x, int y, uint16_t color) {
      matrix.drawPixel(x, y, panel::color(color));
    }, clockFormat);
    return;
  }
  if (!hasFrame) {
    matrix.setTextWrap(false);
    matrix.setTextColor(panel::color(matrix.color565(128, 192, 128)));
    matrix.setCursor(2, 12 + panel::TOP_OFFSET);
    matrix.print(messageFirst);
    matrix.setTextColor(panel::color(matrix.color565(128, 128, 128)));
    matrix.setCursor(2, 28 + panel::TOP_OFFSET);
    matrix.print(messageSecond);
    firmware::drawVersion(matrix, messageFirst, messageSecond);
    return;
  }
  // Avatar frames now use all 64 rows at y0 without a top margin or crop.
  // Sender clock positioning and the autonomous clock remain unchanged.
  // Apply after setRotation so the margin always follows the displayed top.
  const int rows = frameIsClock ? panel::SIZE : panel::VISIBLE_SOURCE_ROWS;
  const int offset = frameIsClock ? 0 : panel::TOP_OFFSET;
  for (int y = 0; y < rows; ++y) for (int x = 0; x < 64; ++x) {
    matrix.drawPixel(x, y + offset, panel::color(lastFrame[y * 64 + x]));
  }
}

// Single present wrapper (T7): every content path renders through
// renderContent(), then the diagnostic overlay is drawn LAST — after any
// avatar/clock/message/info content, so a newly presented frame can never
// erase the indicator — and one show() flushes both.
static void presentDisplay() {
  renderContent();
  serialDiag.draw(matrix);
  serialDiag.clearDirty();
  matrix.show();
}

static void pollClock() {
  // While the portal is open its message stays pinned: a surviving powered
  // RTC (soft restart without Wi-Fi) would otherwise pull the autonomous
  // clock back onto the panel and hide the SSID/password slides.
  if (portalActive) return;
  std::tm calendar = {};
  std::time_t epoch = 0;
  const uint32_t now = millis();
  const auto source = clockState.source(now, readClock(calendar, epoch), hasFrame, wifiInfo.visible());
  if (clockState.redrawDue(now, source)) presentDisplay();
}

static void displayMessage(const char *first, const char *second) {
  messageFirst = first;
  messageSecond = second;
  hasFrame = false;
  presentDisplay();
}

// ------------------------------------------------------------------------
// WiFi captive portal (T2, T3). Everything below runs only while
// portalActive; the station server state (server/client/headers/request)
// is never touched from here.
// ------------------------------------------------------------------------
static void zeroOffered() {
  // The passphrase offered through the form must not outlive the attempt:
  // zero both buffers whatever the validation outcome was.
  memset(offeredSsid, 0, sizeof(offeredSsid));
  memset(offeredPassword, 0, sizeof(offeredPassword));
}

// std::string::clear() drops the size but leaves the bytes in the heap:
// the raw form body (and the passphrase in it) would survive there, so
// overwrite it deliberately before releasing the buffer.
static void wipeRequestBuffer() {
  if (!portalRequest.empty()) {
    volatile char *bytes = &portalRequest[0];
    for (size_t i = 0; i < portalRequest.size(); ++i) bytes[i] = 0;
  }
  portalRequest.clear();
  portalHeaderEnd = 0;
  portalContentLength = 0;
}

// Per-render CSRF token (R1): esp_random(), RAM only, embedded as a hidden
// field in every rendered form and required exactly on POST /save.
static std::string portalFormToken;

// S5 (T7): one fresh scan per page render, under the
// WIFI_AP_STA mode openPortal() maintains while the portal is open (a scan
// needs the station interface; the AP and the phone's connection stay up).
// The raw scan buffer is copied into the selection and deleted immediately
// so memory stays bounded. The raw entries go through the host-tested
// portal::selectNetworks rules: hidden SSIDs dropped, de-duplicated by SSID,
// descending RSSI, capped at portal::SCAN_MAX_ENTRIES.
static std::vector<portal::ScanEntry> scanVisibleNetworks() {
  std::vector<portal::ScanEntry> raw;
  const int16_t found = WiFi.scanNetworks();
  if (found > 0) {
    raw.reserve(static_cast<size_t>(found));
    for (int16_t i = 0; i < found; ++i)
      raw.push_back(portal::ScanEntry{WiFi.SSID(i).c_str(), WiFi.RSSI(i)});
  }
  WiFi.scanDelete();
  return portal::selectNetworks(raw.data(), raw.size());
}

static std::string renderPortalForm(const std::string &error) {
  // The CSRF token is generated once per portal session (openPortal) and kept:
  // rotating it per render breaks the form, because the phone's captive-portal
  // probes and the "Scan again" link re-render the page and used to invalidate
  // the token of the form the user was looking at, answering 403
  // "Invalid or missing token".
  // The validation path never scans (S5 decision): a page served while the
  // 20 s credential check runs lists no networks; the phase is already back
  // to Portal by the time the post-conclusion re-renders happen.
  if (portalPhase == PortalPhase::Validating)
    return portal_page::formPage(error, portalFormToken, {}, channelOrder);
  return portal_page::formPage(error, portalFormToken, scanVisibleNetworks(),
                               channelOrder);
}

// A request that could not prove it holds the session token must not receive
// it back: otherwise anyone able to POST a malformed or tokenless body could
// read the live token from the error page. The served form carries no token,
// so the client reloads to get one.
static std::string renderPortalFormUnverified(const std::string &error) {
  if (portalPhase == PortalPhase::Validating)
    return portal_page::formPage(error, "", {}, channelOrder);
  return portal_page::formPage(error, "", scanVisibleNetworks(), channelOrder);
}

static void showPortalSlide() {
  if (portalSlide == 0) displayMessage("AP: GENTLE", apPassword);
  else if (portalSlide == 1) displayMessage("PORTAL", "WIFI SETUP");
  else displayMessage("192.168.", "4.1");
}

// Opens the WPA2 portal (S1, S3): random 8-character AP password (WPA2 PSK
// minimum) drawn once via esp_random() — never stored, so a fresh one every
// boot and nothing secret is added to NVS — shown on the panel so anyone
// standing at the board can join; never logged, echoed or exposed in
// /health. The decision was taken once at the gate (setup boot gate or the
// exhausted retry); an USB host appearing later does not cancel it.
static void openPortal() {
  portal::generateApPassword(apPassword, []() { return esp_random(); });
  // One CSRF token per portal session (R1): a fresh value every time the
  // portal opens, stable across every render of that session.
  char token[9];
  snprintf(token, sizeof(token), "%08x", static_cast<unsigned>(esp_random()));
  portalFormToken = token;
  // WPA2 (S3 correction): the password argument switches softAP to
  // WPA2_PSK; never an open AP. WIFI_AP_STA (S5/T7) keeps the station
  // interface on so every page render can run its fresh scan; the AP stays
  // up the whole time.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(portal::AP_SSID, apPassword, portal::AP_CHANNEL, 0,
              portal::AP_MAX_CLIENTS);
  // Wildcard answer with the AP address so the OS captive-portal detection
  // fires on every probe.
  dnsServer.start(53, "*", WiFi.softAPIP());
  if (!listening) portalServer.begin(); // never while the station server is up
  portalActive = true;
  portalPhase = PortalPhase::Portal;
  portalSlide = 0;
  slideSince = millis();
  showPortalSlide();
  Serial.println("Portal open: join gentle-matrix and open http://192.168.4.1");
  Serial.println("The AP password is shown on the panel (not printed here).");
}

static void portalReply(int code, const char *contentType, const std::string &body) {
  portalClient.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                      code, code == 200 ? "OK" : "Error", contentType,
                      static_cast<unsigned>(body.size()));
  portalClient.print(body.c_str());
  portalClient.stop();
  wipeRequestBuffer();
}

// Bounded Content-Length extraction from the accumulated portal headers:
// the shared rules live in panel_settings::parseContentLength now.

// S4 validation conclusion: success persists (same read-back verification as
// the serial hook), replies and restarts; failure keeps the portal open with
// the stored credentials untouched, zeros the offered buffers and re-renders
// the page with the error. The portal stays in WIFI_AP_STA either way so the
// next page render can scan (S5).
static void concludePortalValidation(bool connected) {
  portalPhase = PortalPhase::Portal;
  if (portal::concludeValidation(connected) == portal::Outcome::SaveAndReboot) {
    displayMessage("WIFI", "SAVING");
    const bool saved = storeCredentials(offeredSsid, offeredPassword);
    zeroOffered();
    if (saved) {
      portalReply(200, "text/html; charset=utf-8", portal_page::savedPage());
      delay(100); // let the small success page drain, exactly like USB provisioning
      ESP.restart();
    }
    portalError = "Storage failed. Try again.";
    portalReply(500, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  WiFi.disconnect(false);
  WiFi.mode(WIFI_AP_STA); // portal stays AP+station: next render rescans (S5)
  zeroOffered();
  portalError = "Could not connect to that network. Check the name and password, then try again.";
  portalSlide = 0;
  showPortalSlide();
  portalReply(200, "text/html; charset=utf-8", renderPortalForm(portalError));
}

// POST /save with a reviewed body: no reply to the client here — validation
// runs asynchronously up to portal::VALIDATION_MS and concludePortalValidation
// answers the same open client.
static void beginPortalValidation(const char *body, size_t length) {
  const portal::FormFields fields = portal::parseForm(body, length);
  // Neither branch below may echo the session token: a request that cannot
  // prove it holds the token must not be able to read it back from the error
  // page. Both answer with a tokenless form that the client re-fetches.
  if (!fields.ok) {
    portalError = "Malformed form data. Reload the page and try again.";
    portalReply(400, "text/html; charset=utf-8", renderPortalFormUnverified(portalError));
    return;
  }
  if (!portal::tokenMatches(portalFormToken, fields.token)) {
    portalError = "Invalid or missing form token. Reload the page and try again.";
    portalReply(403, "text/html; charset=utf-8", renderPortalFormUnverified(portalError));
    return;
  }
  const portal::CredError review = portal::reviewCredentials(fields.ssid, fields.password);
  if (review != portal::CredError::Ok) {
    portalError = portal::credErrorText(review);
    portalReply(400, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  strncpy(offeredSsid, fields.ssid.c_str(), sizeof(offeredSsid) - 1);
  strncpy(offeredPassword, fields.password.c_str(), sizeof(offeredPassword) - 1);
  wipeRequestBuffer(); // the raw body must not linger while we validate
  // S4: try the real network before persisting anything. The AP stays up
  // during AP_STA validation so the phone's connection and the DNS responder
  // both survive.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(offeredSsid, offeredPassword);
  portalPhase = PortalPhase::Validating;
  validationStarted = millis();
  displayMessage("TRY WIFI", "CHECK");
}

// S7 POST /rgborder — the colour-order selector's own form and route. Same
// session token as the credentials form: a request that cannot prove it
// holds the token is never accepted and is never shown the token (both
// pre-token rejections serve a tokenless form). On success the order is
// stored with read-back verification and the board restarts to apply it;
// any failure keeps the portal open and re-renders the form with the
// reason, touching nothing else (the credential path, the scan list and
// the validate-before-persist flow are untouched).
static void beginPortalOrderSave(const char *body, size_t length) {
  const portal::OrderFields fields = portal::parseOrderForm(body, length);
  if (!fields.ok) {
    portalError = "Malformed form data. Reload the page and try again.";
    portalReply(400, "text/html; charset=utf-8", renderPortalFormUnverified(portalError));
    return;
  }
  if (!portal::tokenMatches(portalFormToken, fields.token)) {
    portalError = "Invalid or missing form token. Reload the page and try again.";
    portalReply(403, "text/html; charset=utf-8", renderPortalFormUnverified(portalError));
    return;
  }
  rgb_order::Order order;
  if (!rgb_order::parseOrder(fields.order, order)) {
    portalError = "Unknown colour order.";
    portalReply(400, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  if (!storeRgbOrder(order)) {
    portalError = "Storage failed. Try again.";
    portalReply(500, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  displayMessage("RGB", "SAVED");
  portalReply(200, "text/html; charset=utf-8", portal_page::orderSavedPage());
  delay(100); // let the small success page drain, exactly like USB provisioning
  ESP.restart();
}

static void dispatchPortalRequest() {
  const size_t firstLineEnd = portalRequest.find("\r\n");
  const portal::RequestLine line =
      portal::parseRequestLine(portalRequest.substr(0, firstLineEnd));
  if (!line.ok) {
    portalError = "Unsupported request.";
    portalReply(400, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  if (!line.isPost) {
    if (line.path == "/health") {
      // Portal identity for the diagnostics surface (T4). The generated AP
      // password is never part of it.
      const std::string health = std::string("{\"ok\":true,\"portal\":true,\"apSsid\":\"") +
                                 portal::AP_SSID + "\",\"validation\":" +
                                 (portalPhase == PortalPhase::Validating ? "true}" : "false}");
      portalReply(200, "application/json", health);
      return;
    }
    // GET any path returns the form page, so OS probe URLs land on it.
    portalReply(200, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  // S7: the colour-order form has its own route; everything that is not
  // the credentials form and not that route stays rejected without state
  // changes.
  if (line.path != "/save" && line.path != "/rgborder") {
    portalReply(405, "text/html; charset=utf-8",
                renderPortalForm("Unsupported method or path."));
    return;
  }
  if (portalPhase == PortalPhase::Validating) {
    portalReply(503, "text/html; charset=utf-8",
                renderPortalForm("Already validating. Wait a moment."));
    return;
  }
  if (line.path == "/rgborder") {
    beginPortalOrderSave(portalRequest.data() + portalHeaderEnd,
                         portalRequest.size() - portalHeaderEnd);
    return;
  }
  beginPortalValidation(portalRequest.data() + portalHeaderEnd,
                        portalRequest.size() - portalHeaderEnd);
}

// The portal's cooperative loop: DNS responder plus one bounded request at a
// time (same 2.5 s timeout policy as the station server). The offered
// credentials stay intact while validating; the panel keeps the TRY WIFI
// slide until the conclusion.
static void pollPortal() {
  if (!portalActive) return;
  const uint32_t now = millis();
  if (uint32_t(now - slideSince) >= 5000) {
    slideSince = now;
    portalSlide = static_cast<uint8_t>((portalSlide + 1) % 3);
    showPortalSlide();
  }
  dnsServer.processNextRequest();
  if (portalPhase == PortalPhase::Validating) {
    if (WiFi.status() == WL_CONNECTED) {
      concludePortalValidation(true);
      return;
    }
    if (uint32_t(now - validationStarted) >= portal::VALIDATION_MS) {
      concludePortalValidation(false);
      return;
    }
    // R4: the validating client stays open — its reply is the conclusion above.
    return;
  }
  if (!portalClient) {
    portalClient = portalServer.available();
    if (!portalClient) return;
    wipeRequestBuffer(); // also overwrites any bytes left by the last request
    portalRequestStarted = now;
    portalClient.setTimeout(1); // seconds, not milliseconds
  }
  if (uint32_t(now - portalRequestStarted) > REQUEST_TIMEOUT_MS) {
    portalError = "Request timed out.";
    portalReply(408, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  for (size_t budget = 0; budget < 1024 && portalClient.available(); ++budget) {
    const int value = portalClient.read();
    if (value < 0) break;
    portalRequest.push_back(static_cast<char>(value));
    if (portalHeaderEnd == 0) {
      const size_t end = portalRequest.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (portalRequest.size() > 4096) {
          portalError = "Request too large.";
          portalReply(431, "text/html; charset=utf-8", renderPortalForm(portalError));
          return;
        }
        continue;
      }
      portalHeaderEnd = end + 4;
      // Only a POST carries a body. Browsers and OS captive-portal probes
      // send a body-less GET (no Content-Length at all), so requiring the
      // header there would answer 411 and leave the portal unreachable.
      const portal::RequestLine head = portal::parseRequestLine(
          portalRequest.substr(0, portalRequest.find("\r\n")));
      if (head.ok && !head.isPost) {
        portalContentLength = 0;
        dispatchPortalRequest();
        return;
      }
      const long contentLength = panel_settings::parseContentLength(portalRequest.substr(0, portalHeaderEnd));
      if (contentLength < 0) {
        portalError = "Missing content length.";
        portalReply(411, "text/html; charset=utf-8", renderPortalForm(portalError));
        return;
      }
      if (contentLength > 255) { // ssid + password bound, with margin
        portalError = "Form data too large.";
        portalReply(413, "text/html; charset=utf-8", renderPortalForm(portalError));
        return;
      }
      portalContentLength = static_cast<size_t>(contentLength);
    } else if (portalRequest.size() >= portalHeaderEnd + portalContentLength) {
      dispatchPortalRequest();
      return;
    }
  }
  if (!portalClient.connected() && !portalClient.available()) portalClient.stop();
}

// Decode-and-stage half of the shared presentation: on success the staged
// frame becomes the displayed content on the next present; on failure no
// staged state changes, so an invalid PNG cannot corrupt later redraws or
// alter the persisted frame classification.
static bool stagePng(uint8_t *data, size_t length, bool clockSkin) {
  const bool decoded = frameDecoder.decode(data, length);
  clockState.pngResult(decoded, millis());
  if (!decoded) return false;
  memcpy(lastFrame, frameDecoder.pixels(), sizeof(lastFrame));
  frameIsClock = clockSkin;
  hasFrame = true;
  return true;
}

// Presentation shared by HTTP and serial ingress: decode+stage, one present
// (content plus diagnostic overlay), then the frame counter.
static bool showPng(uint8_t *data, size_t length, bool clockSkin) {
  if (!stagePng(data, length, clockSkin)) return false;
  presentDisplay();
  ++accepted;
  return true;
}

static void resetOrientationReference() {
  orientation.resetReference();
  if (preferences.begin("gentle", false)) {
    preferences.remove("updir");
    preferences.end();
  }
  presentDisplay();
  Serial.println("ORIENT: hold panel upright and still to calibrate");
}

static void pollOrientation() {
  static uint32_t lastSample = 0;
  static bool firstSampleSeen = false;
  if (!accelerometerReady || millis() - lastSample < 50) return;
  lastSample = millis();
  sensors_event_t event;
  if (!accelerometer.getEvent(&event)) return;
  const bool firstSample = !firstSampleSeen;
  firstSampleSeen = true; // a failed getEvent is not a sample: try again later
  const int referenceBefore = orientation.reference();
  const bool changed = orientation.update(event.acceleration.x, event.acceleration.y, event.acceleration.z, lastSample);
  if (referenceBefore < 0 && orientation.reference() >= 0) {
    if (preferences.begin("gentle", false)) {
      preferences.putUChar("updir", orientation.reference());
      preferences.end();
    }
    Serial.println("ORIENT: upright reference saved");
  }
  if (changed) {
    presentDisplay(); // Works without receiving another HTTP frame.
    Serial.printf("ORIENT: rotation %u degrees\n", orientation.rotation() * 90);
  } else if (firstSample) {
    // setup() paints its first content (SET WIFI / VIA USB, and the portal
    // slides when the portal opens with no credentials) before any sample,
    // so that paint always used rotation 0; repaint once the orientation
    // reference and rotation are first known, without waiting for a content
    // change or disturbing any other drawing behaviour.
    presentDisplay();
  }
}

static void pollWifiInfoButton() {
  static uint32_t lastRefresh = 0;
  const uint32_t now = millis();
  const bool changed = wifiInfo.update(digitalRead(PIN_BUTTON_UP) == LOW, now, senderStats.size());
  if (changed || (wifiInfo.visible() && uint32_t(now - lastRefresh) >= 1000)) {
    lastRefresh = now;
    presentDisplay();
  }
}

static void closeRequest() {
  senderConnection.reset();
  client.stop();
  headers.clear();
  received = 0;
  readingBody = false;
  request = receiver::Request{};
  configFirstLineChecked = false;
  configSavePending = false;
  configSurface = false;
  configBody.clear();
  configBodyLength = 0;
}

// extra carries additional response headers (each ending in CRLF), used by
// the session control routes to mark successful ownership responses.
static void reply(int code, const String &body, const char *extra = "") {
  client.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n%sContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                code, code == 200 ? "OK" : "Error", extra, static_cast<unsigned>(body.length()));
  client.print(body);
  closeRequest();
}

// HTML replies (S8): the same bounded one-request reply discipline as
// reply(), with the page content type instead of JSON.
static void replyHtml(int code, const std::string &body) {
  client.printf("HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                code, code == 200 ? "OK" : "Error", static_cast<unsigned>(body.size()));
  client.print(body.c_str());
  closeRequest();
}

// --- S8 (T13): the LAN settings routes on the station server -------------

// The settings page with the live values: one render helper for the GET
// reply and for every POST rejection, now carrying the T4a weather values
// (durations, unit and the resolved city) preselected.
static std::string configPageWithError(const std::string &error) {
  return panel_settings::configPage(panel::brightnessPercent(), clockFormat,
                                    channelOrder, weatherUnit, clockSecs,
                                    weatherSecs, weatherCity.name, error);
}

// POST /config application (T4b): parse + review of all six values, then
// ONE verified store across them all (the combined storeAllSettings, so a
// failure never leaves the stored settings mixed across the panel and
// weather surfaces), then the live application. A rejected value or a
// malformed body re-renders the page with the reason and changes nothing;
// the runtime state only moves after the store verified. Brightness, clock
// format, the durations and the unit apply immediately; the channel order
// takes effect only through a restart, and ONLY when the value actually
// changed — the driver fixes the pins at construction, so saving
// the same value again must not reboot the board.
static void applyConfigSave() {
  const panel_settings::SettingsFields fields =
      panel_settings::parseSettingsForm(configBody.data(), configBody.size());
  const panel_settings::BrightnessChoice brightness =
      panel_settings::reviewBrightness(fields.brightness);
  const panel_settings::ClockChoice clock =
      panel_settings::reviewClock(fields.clock);
  const panel_settings::OrderChoice order =
      panel_settings::reviewOrder(fields.order);
  const panel_settings::SecondsChoice clockDuration =
      panel_settings::reviewSeconds(fields.clocksecs);
  const panel_settings::SecondsChoice weatherDuration =
      panel_settings::reviewSeconds(fields.weathersecs);
  const panel_settings::UnitChoice unit =
      panel_settings::reviewUnits(fields.units);
  const char *error = !fields.ok
      ? "Malformed form data. Reload the page and try again."
      : !brightness.ok ? "Unknown brightness value."
      : !clock.ok ? "Unknown clock format."
      : !order.ok ? "Unknown colour order."
      : !clockDuration.ok ? "Unknown clock duration."
      : !weatherDuration.ok ? "Unknown weather duration."
      : !unit.ok ? "Unknown temperature unit."
      : nullptr;
  if (error) {
    replyHtml(400, configPageWithError(error));
    return;
  }
  if (storePanelSettings(brightness.percent, clock.format, order.value,
                         clockDuration.seconds, weatherDuration.seconds,
                         unit.unit)) {
    panel::setBrightnessPercent(brightness.percent);
    clockFormat = clock.format;
    clockSecs = clockDuration.seconds;
    weatherSecs = weatherDuration.seconds;
    weatherUnit = unit.unit;
    presentDisplay(); // live application: this frame already uses the values
    if (order.value != channelOrder) {
      replyHtml(200, panel_settings::configSavedPage(true));
      delay(100); // let the small success page drain, exactly like the portal
      ESP.restart();
      return;
    }
    replyHtml(200, panel_settings::configSavedPage());
    return;
  }
  replyHtml(500, configPageWithError("Storage failed. Try again."));
}

// First-line classification of the settings routes (S8). Runs BEFORE the
// receiver protocol parser, whose routes (/emote, the session controls and
// /health) and unknown-path 404 keep their exact semantics: only the exact
// "GET /config" / "POST /config" lines are taken by this surface, everything
// else is left to the parser untouched. ONLY the first request LINE exists
// here: a GET /config (no body) may answer immediately, but a POST /config
// must NOT parse its Content-Length out of a header block that has not
// arrived yet — that used to answer 411 to every real POST. The POST instead
// lingers as pending until the complete header block, where beginConfigSave()
// takes over. Returns true when this request is now owned by the settings
// path (a page was replied, or a POST is pending), false when the receiver
// parser owns it.
static bool handleConfigRequestLine() {
  const panel_settings::Route route =
      panel_settings::routeOf(headers.substr(0, headers.size() - 2));
  if (route == panel_settings::Route::Other) return false;
  configSurface = true; // rejections of this surface re-render its page
  if (route == panel_settings::Route::Get) {
    // The page is open on the LAN by decision (no authentication) and never
    // scans: it renders from the live values only.
    replyHtml(200, configPageWithError(""));
    return true;
  }
  if (route == panel_settings::Route::CitySave) {
    // T4b: the city-search submission lingers until its complete header
    // block exactly like the settings POST.
    citySavePending = true;
    return false;
  }
  configSavePending = true;
  return false;
}

// The header block of a pending POST /config is complete (\r\n\r\n seen):
// only now can the bounded Content-Length be parsed, then the small form
// body is read exactly as before. The receiver parser never sees a /config
// request: this dispatch precedes it.
static void beginConfigSave() {
  const long contentLength = panel_settings::parseContentLength(headers);
  if (contentLength < 0) {
    replyHtml(411, configPageWithError("Missing content length."));
    return;
  }
  if (contentLength > static_cast<long>(panel_settings::CONFIG_BODY_MAX)) {
    replyHtml(413, configPageWithError("Form data too large."));
    return;
  }
  if (contentLength == 0) {
    replyHtml(400, configPageWithError("Empty form."));
    return;
  }
  configBody.clear();
  configBodyLength = static_cast<size_t>(contentLength);
  configBodyIsCity = false;
  readingBody = true;
}

// T4b: the bounded city-search intake - exactly the same Content-Length
// rules as the settings POST (the shared bounded buffer, never the PNG
// path); the geocoding request then runs synchronously inside the serving
// loop. It is a user-initiated search, never automatic, and the loop may
// hold for at most the transport budget (5 s connect + 3 s read).
static void beginCitySave() {
  const long contentLength = panel_settings::parseContentLength(headers);
  if (contentLength < 0) {
    replyHtml(411, configPageWithError("Missing content length."));
    return;
  }
  if (contentLength > static_cast<long>(panel_settings::CONFIG_BODY_MAX)) {
    replyHtml(413, configPageWithError("Form data too large."));
    return;
  }
  if (contentLength == 0) {
    replyHtml(400, configPageWithError("Empty form."));
    return;
  }
  configBody.clear();
  configBodyLength = static_cast<size_t>(contentLength);
  configBodyIsCity = true;
  readingBody = true;
}

// T4b city search application: the committed review rejects an empty or
// oversized name BEFORE any request could occur; the request goes to the
// committed geocoding host and its answer through the committed intake
// (status/size/parse discipline and the UTF-8-safe name clamp). A failed
// or invalid search changes NOTHING - no stored key, no live record - and
// the page says why; only a VERIFIED city store moves the live record.
static void applyCitySave() {
  const panel_settings::CitySearchFields fields =
      panel_settings::parseCityForm(configBody.data(), configBody.size());
  if (!fields.ok || !panel_settings::reviewCitySearch(fields.city)) {
    replyHtml(400, configPageWithError(
        "Enter a city name of at most 64 characters."));
    return;
  }
  const std::string geocodeRequestPath = weather::geocodePath(fields.city);
  const weather_fetch::Transport::Response geocodeResponse =
      weatherTransport.get(weather::GEOCODE_HOST, geocodeRequestPath.c_str());
  weather::CityRecord resolved;
  if (!panel_settings::cityFromResponse(geocodeResponse, resolved)) {
    replyHtml(400, configPageWithError(
        "Could not resolve that city. Check the name and try again."));
    return;
  }
  if (!storeWeatherCity(resolved)) {
    replyHtml(500, configPageWithError("Storage failed. Try again."));
    return;
  }
  weatherCity = resolved; // live record only after the verified store
  replyHtml(200, configPageWithError(""));
}

// One active request, no frame queue. Parsing is bounded and yields each loop.
static void pollHttp() {
  if (!client) {
    closeRequest();
    client = server.available();
    if (!client) return;
    const IPAddress remote = client.remoteIP();
    senderIp = (uint32_t(remote[0]) << 24) | (uint32_t(remote[1]) << 16) |
               (uint32_t(remote[2]) << 8) | uint32_t(remote[3]);
    requestStarted = millis();
    client.setTimeout(1); // Arduino-ESP32 WiFiClient takes seconds, not milliseconds.
  }
  if (millis() - requestStarted > REQUEST_TIMEOUT_MS) {
    // Rejections of the settings surface re-render the settings page with
    // the reason; every other request keeps the generic JSON body. Neither
    // path touches the stored or live settings.
    if (configSurface) replyHtml(408, configPageWithError("Request timed out."));
    else reply(408, "{\"error\":\"timeout\"}");
    return;
  }
  for (size_t budget = 0; budget < 1024 && client.available(); ++budget) {
    const int value = client.read();
    if (value < 0) break;
    if (!readingBody) {
      senderConnection.feed(static_cast<char>(value), senderIp, senderStats);
      if (value == 0 || (value < 32 && value != '\r' && value != '\n' && value != '\t') || value > 126) {
        reply(400, "{\"error\":\"invalid headers\"}"); return;
      }
      headers.push_back(static_cast<char>(value));
      if (headers.size() > receiver::MAX_HEADERS) { reply(431, "{\"error\":\"headers too large\"}"); return; }
      // S8: classify the settings routes on the exact first request line,
      // before the receiver protocol parser runs; see above.
      if (!configFirstLineChecked && headers.size() >= 2 &&
          headers.compare(headers.size() - 2, 2, "\r\n") == 0) {
        configFirstLineChecked = true;
        if (handleConfigRequestLine()) return;
      }
      if (headers.size() < 4 || headers.compare(headers.size() - 4, 4, "\r\n\r\n")) continue;
      if (configSavePending) {
        // A pending POST /config: the full header block is here now. The
        // receiver parser still never sees this request.
        configSavePending = false;
        beginConfigSave();
        return;
      }
      if (citySavePending) {
        // A pending POST /config/city (T4b): the full header block is here
        // now; the receiver parser never sees this request either.
        citySavePending = false;
        beginCitySave();
        return;
      }
      const int status = receiver::parseHeaders(headers, request);
      if (status == 400 && request.hasName && !request.nameValid) {
        // Distinct parser result for an invalid or duplicate
        // X-Emote-Project-Name: the desktop parity body, no state change.
        reply(400, "invalid project name\n"); return;
      }
      if (status != 200) { reply(status, "{\"error\":\"unsupported request\"}"); return; }
      if (request.health) {
        reply(200, "{\"ok\":true,\"width\":64,\"height\":64,\"frames\":" + String(accepted) +
            ",\"brightness\":" + String(panel::brightnessScale()) +
            ",\"brightnessPercent\":" + String(panel::brightnessPercent()) +
            ",\"topOffset\":" + String(panel::TOP_OFFSET) +
            ",\"bitDepth\":" + String(panel::BIT_DEPTH) + ",\"wifiInfo\":" + (wifiInfo.visible() ? "true" : "false") +
            ",\"accelerometer\":" + (accelerometerReady ? "true" : "false") +
            ",\"rotation\":" + String(orientation.rotation() * 90) +
            ",\"orientationCalibrated\":" + (orientation.reference() >= 0 ? "true" : "false") +
            // Additive session ownership fields; the owner identity is never
            // disclosed. remainingMs also evaluates expiry at this moment.
            ",\"sessionOwnership\":true,\"ownershipActive\":" +
            (sessionOwner.active() ? "true" : "false") +
            ",\"ownershipRemainingMs\":" + String(sessionOwner.remainingMs(millis())) +
            // Additive T7 diagnostic counters (saturating uint32).
            ",\"serialRxImages\":" + String(serialDiag.imageCount()) +
            ",\"serialRxControls\":" + String(serialDiag.controlCount()) +
            ",\"serialRxRejected\":" + String(serialDiag.rejectedCount()) +
            // Additive T4 weather fields (S1): whether a forecast is
            // currently held and the resolved city name (JSON-escaped; the
            // name is public data). No existing field changes meaning.
            ",\"weather\":" +
            String(weatherForecast.valid ? "true" : "false") +
            ",\"city\":\"" + weather::jsonEscape(weatherCity.name).c_str() +
            "\"" +
            // Additive T4 portal identity field: the portal serves its own
            // health with "portal":true; nothing secret is ever included.
            ",\"portal\":false}"); return;
      }
      if (request.kind != receiver::Request::Kind::Image) {
        // Session controls: claim/heartbeat/release. They never touch the
        // displayed image, the frame classification or the autonomous clock
        // timer, and the parser already guaranteed exactly one valid identity.
        const uint32_t now = millis();
        const char *id = request.sessionId;
        int control;
        switch (request.kind) {
          case receiver::Request::Kind::Claim: control = sessionOwner.claim(id, now); break;
          case receiver::Request::Kind::Heartbeat: control = sessionOwner.heartbeat(id, now); break;
          default: control = sessionOwner.release(id, now); break;
        }
        if (control == 200) {
          // Only successful control responses carry the ownership marker; a
          // sender treats anything else as legacy delivery with a warning.
          // Success also refreshes the session-title liveness window.
          clockState.touchTitle(now);
          reply(200, "{\"ok\":true}", "X-Emote-Ownership: 1\r\n");
        } else if (control == 409) {
          reply(409, "{\"error\":\"owned by another session\"}");
        } else {
          reply(400, "{\"error\":\"invalid session id\"}");
        }
        return;
      }
      readingBody = true;
    } else if (configBodyLength) {
      // S8 POST /config body: a small bounded form read into its own buffer;
      // completely disjoint from the PNG payload path.
      configBody.push_back(static_cast<char>(value));
      if (configBody.size() == configBodyLength) {
        if (configBodyIsCity) applyCitySave();
        else applyConfigSave();
        return;
      }
    } else {
      pngBytes[received++] = static_cast<uint8_t>(value);
      if (received == request.length) {
        // The declared body is fully consumed before the ownership decision,
        // so a rejected foreign or sessionless image leaves the displayed
        // frame, its classification, the frame counter and the 10 s clock
        // timer untouched.
        if (sessionOwner.pngAccess(request.hasSession, request.sessionId, millis()) != 200) {
          reply(409, "{\"error\":\"owned by another session\"}");
          return;
        }
        if (showPng(pngBytes, received, request.clockState)) {
          // Lease renewal only for a valid owned PNG after successful
          // presentation; ownerless images (legacy behavior) never claim.
          // The session title commits only here, after the ownership
          // admission and a successful presentation, so a rejected foreign,
          // sessionless or invalid image never renames the display; an image
          // without the header clears the stored name.
          sessionOwner.renewPng(millis());
          clockState.commitTitle(millis(), request.hasName, request.projectName);
          reply(200, "{\"ok\":true}");
        }
        else reply(422, "{\"error\":\"invalid 64x64 RGBA PNG\"}");
        return;
      }
    }
  }
  if (!client.connected() && !client.available()) closeRequest();
}

// USB provisioning: WIFI, SSID and password, one line each. Never echo
// secrets. While the binary serial session is open every byte goes to the
// frame parser instead — a frame or its tail is never parsed as
// WIFI/ORIENT/password, and console lines keep their exact previous protocol
// (including credentials that start with the frame magic).
static void pollSerialFrames();
static void pollProvisioning() {
  if (serialMux.mode() == serial::Mux::Mode::Binary) {
    pollSerialFrames();
    return;
  }
  provisionConsole.tick(millis());
  for (int budget = 0; budget < 128 && Serial.available(); ++budget) {
    const int value = Serial.read();
    if (value < 0) break;
    const uint32_t now = millis();
    serialDiag.onByte(now); // even console bytes start the pending-RX window
    if (provisionConsole.feed(static_cast<char>(value), now) ==
        serial::ProvisionConsole::Event::EnterBinary) {
      serialMux.enter(now);
      serialDiag.onBinaryEnter(now); // handshake is never an accepted operation
      // The rest of the buffered input belongs to the binary session: route
      // it immediately instead of finishing the console loop, which would
      // coerce frame bytes into provisioning lines.
      pollSerialFrames();
      return;
    }
  }
}

// Bounded nonblocking reply TX: at most one 12-byte reply is pending, flushed
// in availableForWrite()-limited chunks so a host that stopped reading can
// never block the cooperative loop.
static void flushSerialReply() {
  while (replySent < replyPending) {
    const int room = Serial.availableForWrite();
    if (room <= 0) return;
    const size_t take = static_cast<size_t>(room) < replyPending - replySent
                            ? static_cast<size_t>(room)
                            : replyPending - replySent;
    const size_t written = Serial.write(serialReply + replySent, take);
    if (written == 0) return;
    replySent += written;
  }
  replyPending = 0;
}

static void queueSerialReply(uint16_t requestId, serial::Code code) {
  serial::encodeReply(serialReply, requestId, code);
  replySent = 0;
  replyPending = serial::REPLY_SIZE;
  flushSerialReply();
}

// One frame handling pass, identical admission/commit rules as pollHttp: the
// frame is fully received and CRC-validated before the ownership decision;
// the reply is queued only after the accepted presentation; a rejected frame
// changes no state at all. USB frames never touch the sender IP stats.
static void handleSerialFrame() {
  const serial::Frame frame = serialParser.frame();
  const uint32_t now = millis();
  serial::Code code;
  if (frame.op == serial::Op::Image) {
    code = image_ingress::admitImage(frame, sessionOwner, now);
    if (code == serial::Code::Ok) {
      if (stagePng(serialPng, frame.payloadLength, frame.clockState)) {
        // Green is set BEFORE the single present, so the accepted frame
        // shows the green overlay immediately — never amber-then-green.
        serialDiag.onImageAccepted(now);
        image_ingress::commitImage(sessionOwner, clockState, frame, now);
        presentDisplay();
      } else {
        code = serial::Code::Png;
        serialDiag.onRejected(now);
      }
    } else {
      serialDiag.onRejected(now); // admission rejection, state untouched
    }
  } else {
    code = image_ingress::admitControl(frame, sessionOwner, clockState, now);
    if (code == serial::Code::Ok) {
      serialDiag.onControlAccepted(now);
    } else {
      serialDiag.onRejected(now);
    }
  }
  queueSerialReply(frame.requestId, code);
}

static void pollSerialFrames() {
  const uint32_t now = millis();
  serialMux.update(now); // discard stalled frames, session stays binary
  flushSerialReply();
  for (int budget = 0; budget < 256 && Serial.available(); ++budget) {
    const int value = Serial.read();
    if (value < 0) break;
    serialDiag.onByte(now);
    const serial::Mux::Routed routed =
        serialMux.feed(static_cast<uint8_t>(value), now);
    switch (routed.event) {
      case serial::Mux::Event::Complete:
        handleSerialFrame();
        break;
      case serial::Mux::Event::Error:
        queueSerialReply(serialParser.lastRequestId(), serialParser.error());
        serialDiag.onRejected(now);
        break;
      case serial::Mux::Event::RepeatEntry:
        Serial.println(serial::ENTRY_ACK);
        break;
      case serial::Mux::Event::Exit:
        Serial.println(serial::EXIT_ACK);
        serialDiag.onBinaryExit(now);
        return;
      default:
        break; // frame bytes never reach the console
    }
  }
}

static void pollWifi() {
  static uint32_t lastRetry = 0;
  // While the portal is open it owns port 80 and the serving loop; the
  // station server is never begun in the same window.
  if (portalActive) return;
  if (ssid.isEmpty()) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (listening) { closeRequest(); server.end(); MDNS.end(); listening = false; }
    if (bootPhase) {
      // S2 boot-only bounded schedule; the runtime 15 s loop below is for
      // everything after boot and never opens the portal.
      switch (bootRetry.update(millis(), false)) {
        case portal::BootRetry::Tick::Retry:
          WiFi.reconnect();
          break;
        case portal::BootRetry::Tick::Exhausted:
          bootPhase = false;
          // The gate is evaluated once, now (decide → portal) or at the
          // missing-credentials boot point; USB presence decides.
          if (portal::decide(tud_mounted(), true, true) ==
              portal::Decision::OpenPortal) {
            openPortal();
            return;
          }
          // USB host enumerated: provisioning stays authoritative, the
          // runtime retry loop continues.
          break;
        case portal::BootRetry::Tick::Wait:
          break;
      }
      return;
    }
    if (millis() - lastRetry >= 15000) { lastRetry = millis(); WiFi.reconnect(); }
    return;
  }
  bootPhase = false; // first connection success ends the boot countdown
  if (!listening) {
    if (!clockConfigured) {
      configTzTime(idle_clock::TIMEZONE, "pool.ntp.org", "time.cloudflare.com");
      clockConfigured = true; // SDK resynchronizes asynchronously over existing Wi-Fi.
    }
    server.begin();
    server.setNoDelay(true);
    listening = true;
    if (MDNS.begin("gentle-matrix")) MDNS.addService("http", "tcp", 80);
    Serial.print("Receiver ready: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/emote (also gentle-matrix.local)");
    if (accepted == 0) displayMessage("READY", "SEND PNG");
  }
  pollHttp();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON_UP, INPUT_PULLUP); // MatrixPortal S3 UP button: GPIO6, active low.
  pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP); // DOWN button: GPIO7 (variant PIN_BUTTON_DOWN), active low — S6 reset.
  // S7/T10: the channel order is storage-backed and must be known before
  // the panel object exists (see panelStorage): read the small code
  // read-only, fall back to the untouched RGB default on an unknown value,
  // permute the base pins, and only then construct and begin the panel.
  if (preferences.begin("gentle", true)) {
    const uint8_t code = preferences.getUChar("rgborder", 0);
    if (!rgb_order::orderFromCode(code, channelOrder))
      channelOrder = rgb_order::Order::RGB;
    preferences.end();
  }
  rgb_order::applyOrder(channelOrder, rgbPins);
  new (panelStorage) Adafruit_Protomatter(64, panel::BIT_DEPTH, 1, rgbPins, 5, addrPins, 2, 47, 14, true);
  const ProtomatterStatus status = matrix.begin();
  if (status != PROTOMATTER_OK) {
    for (;;) { Serial.printf("Matrix initialization failed: %d\n", static_cast<int>(status)); delay(1000); }
  }
  headers.reserve(receiver::MAX_HEADERS);
  // S9/S10: persisted panel settings — each verified, an absent or
  // out-of-range value falls back to its default (nothing is applied then).
  loadPanelSettings();
  // T4b: the weather settings and the city, with the committed defaults
  // (Sevilla included) whenever storage has nothing usable.
  loadWeatherSettings();
  preferences.begin("gentle", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  orientation = Orientation(preferences.getUChar("updir", 255));
  preferences.end();
  Wire.begin(SDA, SCL); // MatrixPortal S3: SDA GPIO16, SCL GPIO17.
  Wire.setTimeOut(20);
  accelerometerReady = accelerometer.begin(0x19);
  if (accelerometerReady) {
    accelerometer.setRange(LIS3DH_RANGE_2_G);
    accelerometer.setDataRate(LIS3DH_DATARATE_50_HZ);
    Serial.println("LIS3DH ready; first stable upright pose becomes the reference if not calibrated");
  } else {
    Serial.println("LIS3DH unavailable; automatic rotation disabled");
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  setenv("TZ", idle_clock::TIMEZONE, 1);
  tzset(); // Also use Madrid if the powered RTC survives an offline soft restart.
  WiFi.setHostname("gentle-matrix");
  if (ssid.isEmpty()) {
    // Missing credentials: no attempts to exhaust. With a USB host
    // enumerated the cable path stays authoritative; without one the portal
    // opens immediately (S1 + missing-credentials decision).
    if (tud_mounted()) {
      displayMessage("SET WIFI", "VIA USB");
      Serial.println("No Wi-Fi credentials. Run scripts/provision_wifi.py via USB.");
    } else {
      Serial.println("No Wi-Fi credentials and no USB host: opening the portal.");
      openPortal();
    }
  } else {
    displayMessage("WIFI", "CONNECT");
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    password = "";
    // The setup Wi-Fi.begin counts as attempt one of the boot schedule.
    bootRetry.start(millis());
    bootPhase = true;
  }
}

void loop() {
  pollProvisioning();
  pollOrientation();
  pollWifiInfoButton();
  pollCredentialsReset();
  pollWifi();
  pollPortal();
  pollWeather();
  pollClock();
  // Lease expiry is also evaluated before every ownership decision; this
  // loop-level check keeps an abandoned reservation from outliving silence.
  sessionOwner.expire(millis());
  // One batched diagnostic pass per loop: timeout/hold transitions and any
  // signal change not already drawn this loop get exactly one redraw.
  if (serialDiag.update(millis())) presentDisplay();
  delay(1);
}
