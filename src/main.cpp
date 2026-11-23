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
#include "wifi_credentials_reset.h"
#include <DNSServer.h>
#include <esp_system.h>

// TI USB gate: a USB HOST is enumerated by the bus — what S1 asks for — not
// Serial's operator bool(), which only turns true with DTR+RTS asserted and
// would therefore claim a charger or a closed terminal is a computer.
// Declared here instead of pulling the full TinyUSB headers for one banner
// call; the symbol ships with the core's TinyUSB CDC support.
extern "C" bool tud_mounted(void);

static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35, 21};
// Standard 1/32-scan HUB75; official Adafruit MatrixPortal S3 pinout.
static Adafruit_Protomatter matrix(64, panel::BIT_DEPTH, 1, rgbPins, 5, addrPins, 2, 47, 14, true);
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
static uint32_t requestStarted = 0;
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
    return preferences_.putString(key, String(value.c_str())) != 0;
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
// verified gone by read-back; brightness and every other stored value stay
// untouched. False means the removal could not be verified (the caller then
// does NOT restart, so another hold can retry).
static bool clearProvisionedCredentials() {
  static ProvisionedPrefs store(preferences);
  if (!preferences.begin("gentle", false)) return false;
  const bool verified = wifi_reset::eraseCredentials(store);
  preferences.end();
  return verified;
}

static void pollCredentialsReset() {
  if (!downHold.update(millis(), digitalRead(PIN_BUTTON_DOWN) == LOW)) return;
  if (clearProvisionedCredentials()) {
    Serial.println("RESET: Wi-Fi credentials erased; restarting");
    delay(100); // let the log line drain
    ESP.restart();
  }
  Serial.println("RESET: credential erase could not be verified; holding DOWN again retries");
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
    idle_clock::draw(calendar, animationTick, epochMillis,
                     clockState.sessionTitle(millis()),
                     [](int x, int y, uint16_t color) {
      matrix.drawPixel(x, y, panel::color(color));
    });
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
  char token[9];
  snprintf(token, sizeof(token), "%08x", static_cast<unsigned>(esp_random()));
  portalFormToken = token;
  // The validation path never scans (S5 decision): a page served while the
  // 20 s credential check runs lists no networks; the phase is already back
  // to Portal by the time the post-conclusion re-renders happen.
  if (portalPhase == PortalPhase::Validating)
    return portal_page::formPage(error, token);
  return portal_page::formPage(error, token, scanVisibleNetworks());
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

// Bounded Content-Length extraction from the accumulated portal headers.
static long parsePortalContentLength(const std::string &requestHeaders) {
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
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
  return strtol(digits.c_str(), nullptr, 10);
}

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
  if (!fields.ok) {
    portalError = "Malformed form data.";
    portalReply(400, "text/html; charset=utf-8", renderPortalForm(portalError));
    return;
  }
  if (!portal::tokenMatches(portalFormToken, fields.token)) {
    portalError = "Invalid or missing form token. Reload the page and try again.";
    portalReply(403, "text/html; charset=utf-8", renderPortalForm(portalError));
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
  if (line.path != "/save") {
    portalReply(405, "text/html; charset=utf-8",
                renderPortalForm("Unsupported method or path."));
    return;
  }
  if (portalPhase == PortalPhase::Validating) {
    portalReply(503, "text/html; charset=utf-8",
                renderPortalForm("Already validating. Wait a moment."));
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
      const long contentLength = parsePortalContentLength(portalRequest.substr(0, portalHeaderEnd));
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
}

// extra carries additional response headers (each ending in CRLF), used by
// the session control routes to mark successful ownership responses.
static void reply(int code, const String &body, const char *extra = "") {
  client.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n%sContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                code, code == 200 ? "OK" : "Error", extra, static_cast<unsigned>(body.length()));
  client.print(body);
  closeRequest();
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
  if (millis() - requestStarted > REQUEST_TIMEOUT_MS) { reply(408, "{\"error\":\"timeout\"}"); return; }
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
      if (headers.size() < 4 || headers.compare(headers.size() - 4, 4, "\r\n\r\n")) continue;
      const int status = receiver::parseHeaders(headers, request);
      if (status == 400 && request.hasName && !request.nameValid) {
        // Distinct parser result for an invalid or duplicate
        // X-Emote-Project-Name: the desktop parity body, no state change.
        reply(400, "invalid project name\n"); return;
      }
      if (status != 200) { reply(status, "{\"error\":\"unsupported request\"}"); return; }
      if (request.health) {
        reply(200, "{\"ok\":true,\"width\":64,\"height\":64,\"frames\":" + String(accepted) +
            ",\"brightness\":" + String(panel::BRIGHTNESS) + ",\"topOffset\":" + String(panel::TOP_OFFSET) +
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
  const ProtomatterStatus status = matrix.begin();
  if (status != PROTOMATTER_OK) {
    for (;;) { Serial.printf("Matrix initialization failed: %d\n", static_cast<int>(status)); delay(1000); }
  }
  headers.reserve(receiver::MAX_HEADERS);
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
  pollClock();
  // Lease expiry is also evaluated before every ownership decision; this
  // loop-level check keeps an abandoned reservation from outliving silence.
  sessionOwner.expire(millis());
  // One batched diagnostic pass per loop: timeout/hold transitions and any
  // signal change not already drawn this loop get exactly one redraw.
  if (serialDiag.update(millis())) presentDisplay();
  delay(1);
}
