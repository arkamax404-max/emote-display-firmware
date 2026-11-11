#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <Adafruit_LIS3DH.h>
#include <Wire.h>
#include "orientation.h"
#include "display_settings.h"
#include "wifi_info.h"
#include <ESPmDNS.h>
#include "frame_decoder.h"
#include <Preferences.h>
#include <WiFi.h>
#include "receiver_protocol.h"
#include "idle_clock_image.h"

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
static WifiInfoOverlay wifiInfo;
static idle_clock::State clockState;
static bool clockConfigured = false;

static bool readClock(std::tm &calendar, std::time_t &epoch) {
  epoch = std::time(nullptr);
  return localtime_r(&epoch, &calendar) && calendar.tm_year > 116;
}

static void renderDisplay() {
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
                                     connected ? WiFi.RSSI() : 0);
    matrix.setTextWrap(false);
    matrix.setTextSize(1);
    for (size_t row = 0; row < lines.size(); ++row) {
      matrix.setTextColor(panel::color(row == 0 ? matrix.color565(128, 255, 255) : matrix.color565(224, 224, 224)));
      matrix.setCursor(2, panel::TOP_OFFSET + row * 8);
      matrix.print(lines[row].c_str());
    }
    matrix.show();
    return;
  }
  if (source == idle_clock::Source::Clock) {
    // The renderer owns the full 64-row canvas; no vertical offset here.
    idle_clock::draw(calendar, uint64_t(epoch), [](int x, int y, uint16_t color) {
      matrix.drawPixel(x, y, panel::color(color));
    });
    matrix.show();
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
    matrix.show();
    return;
  }
  // Avatar frames keep the 3 px top margin and drop the sender's three
  // bottom background rows; sender clock frames use all 64 rows at y0.
  // Apply after setRotation so the margin always follows the displayed top.
  const int rows = frameIsClock ? panel::SIZE : panel::VISIBLE_SOURCE_ROWS;
  const int offset = frameIsClock ? 0 : panel::TOP_OFFSET;
  for (int y = 0; y < rows; ++y) for (int x = 0; x < 64; ++x) {
    matrix.drawPixel(x, y + offset, panel::color(lastFrame[y * 64 + x]));
  }
  matrix.show();
}

static void pollClock() {
  std::tm calendar = {};
  std::time_t epoch = 0;
  const uint32_t now = millis();
  const auto source = clockState.source(now, readClock(calendar, epoch), hasFrame, wifiInfo.visible());
  if (clockState.redrawDue(now, source)) renderDisplay();
}

static void displayMessage(const char *first, const char *second) {
  messageFirst = first;
  messageSecond = second;
  hasFrame = false;
  renderDisplay();
}

static bool showPng() {
  const bool decoded = frameDecoder.decode(pngBytes, received);
  clockState.pngResult(decoded, millis());
  if (!decoded) return false;
  // Separate from decode staging so invalid PNGs cannot corrupt later redraws
  // or alter the persisted frame classification.
  memcpy(lastFrame, frameDecoder.pixels(), sizeof(lastFrame));
  frameIsClock = request.clockState;
  hasFrame = true;
  renderDisplay();
  ++accepted;
  return true;
}

static void resetOrientationReference() {
  orientation.resetReference();
  if (preferences.begin("gentle", false)) {
    preferences.remove("updir");
    preferences.end();
  }
  renderDisplay();
  Serial.println("ORIENT: hold panel upright and still to calibrate");
}

static void pollOrientation() {
  static uint32_t lastSample = 0;
  if (!accelerometerReady || millis() - lastSample < 50) return;
  lastSample = millis();
  sensors_event_t event;
  if (!accelerometer.getEvent(&event)) return;
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
    renderDisplay(); // Works without receiving another HTTP frame.
    Serial.printf("ORIENT: rotation %u degrees\n", orientation.rotation() * 90);
  }
}

static void pollWifiInfoButton() {
  static uint32_t lastRefresh = 0;
  const uint32_t now = millis();
  const bool changed = wifiInfo.update(digitalRead(PIN_BUTTON_UP) == LOW, now);
  if (changed || (wifiInfo.visible() && uint32_t(now - lastRefresh) >= 1000)) {
    lastRefresh = now;
    renderDisplay();
  }
}

static void closeRequest() {
  client.stop();
  headers.clear();
  received = 0;
  readingBody = false;
  request = receiver::Request{};
}

static void reply(int code, const String &body) {
  client.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                code, code == 200 ? "OK" : "Error", static_cast<unsigned>(body.length()));
  client.print(body);
  closeRequest();
}

// One active request, no frame queue. Parsing is bounded and yields each loop.
static void pollHttp() {
  if (!client) {
    closeRequest();
    client = server.available();
    if (!client) return;
    requestStarted = millis();
    client.setTimeout(1); // Arduino-ESP32 WiFiClient takes seconds, not milliseconds.
  }
  if (millis() - requestStarted > REQUEST_TIMEOUT_MS) { reply(408, "{\"error\":\"timeout\"}"); return; }
  for (size_t budget = 0; budget < 1024 && client.available(); ++budget) {
    const int value = client.read();
    if (value < 0) break;
    if (!readingBody) {
      if (value == 0 || (value < 32 && value != '\r' && value != '\n' && value != '\t') || value > 126) {
        reply(400, "{\"error\":\"invalid headers\"}"); return;
      }
      headers.push_back(static_cast<char>(value));
      if (headers.size() > receiver::MAX_HEADERS) { reply(431, "{\"error\":\"headers too large\"}"); return; }
      if (headers.size() < 4 || headers.compare(headers.size() - 4, 4, "\r\n\r\n")) continue;
      const int status = receiver::parseHeaders(headers, request);
      if (status != 200) { reply(status, "{\"error\":\"unsupported request\"}"); return; }
      if (request.health) {
        reply(200, "{\"ok\":true,\"width\":64,\"height\":64,\"frames\":" + String(accepted) +
            ",\"brightness\":" + String(panel::BRIGHTNESS) + ",\"topOffset\":" + String(panel::TOP_OFFSET) +
            ",\"bitDepth\":" + String(panel::BIT_DEPTH) + ",\"wifiInfo\":" + (wifiInfo.visible() ? "true" : "false") +
            ",\"accelerometer\":" + (accelerometerReady ? "true" : "false") +
            ",\"rotation\":" + String(orientation.rotation() * 90) +
            ",\"orientationCalibrated\":" + (orientation.reference() >= 0 ? "true" : "false") + "}"); return;
      }
      readingBody = true;
    } else {
      pngBytes[received++] = static_cast<uint8_t>(value);
      if (received == request.length) {
        if (showPng()) reply(200, "{\"ok\":true}");
        else reply(422, "{\"error\":\"invalid 64x64 RGBA PNG\"}");
        return;
      }
    }
  }
  if (!client.connected() && !client.available()) closeRequest();
}

// USB provisioning: WIFI, SSID and password, one line each. Never echo secrets.
static void pollProvisioning() {
  static String line, pendingSsid;
  static int stage = 0;
  static bool overflow = false;
  static uint32_t started = 0;
  if (stage && millis() - started > 60000) { stage = 0; line = ""; pendingSsid = ""; overflow = false; }
  for (int budget = 0; budget < 128 && Serial.available(); ++budget) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (line.length() < 64) line += c;
      else overflow = true;
      continue;
    }
    if (overflow) {
      Serial.println("WIFI_ERROR: line too long");
      stage = 0; pendingSsid = "";
    } else if (stage == 0 && line == "ORIENT") {
      resetOrientationReference();
    } else if (stage == 0 && line == "WIFI") {
      stage = 1; started = millis(); Serial.println("WIFI_SSID");
    } else if (stage == 1) {
      if (line.length() == 0 || line.length() > 32) { stage = 0; Serial.println("WIFI_ERROR: invalid SSID length"); }
      else { pendingSsid = line; stage = 2; Serial.println("WIFI_PASSWORD"); }
    } else if (stage == 2) {
      if (line.length() > 63 || (line.length() != 0 && line.length() < 8)) {
        Serial.println("WIFI_ERROR: invalid password length"); stage = 0; pendingSsid = "";
      } else {
        bool saved = false;
        if (preferences.begin("gentle", false)) {
          preferences.putString("password", line);
          preferences.putString("ssid", pendingSsid);
          // putString returns zero for an empty password even on success.
          saved = preferences.isKey("password") && preferences.getString("password") == line &&
                  preferences.getString("ssid") == pendingSsid;
          preferences.end();
        }
        line = ""; pendingSsid = ""; stage = 0;
        Serial.println(saved ? "WIFI_SAVED" : "WIFI_ERROR: storage failed");
        Serial.flush();
        if (saved) { delay(100); ESP.restart(); }
      }
    }
    line = ""; overflow = false;
  }
}

static void pollWifi() {
  static uint32_t lastRetry = 0;
  if (ssid.isEmpty()) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (listening) { closeRequest(); server.end(); MDNS.end(); listening = false; }
    if (millis() - lastRetry >= 15000) { lastRetry = millis(); WiFi.reconnect(); }
    return;
  }
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
    displayMessage("SET WIFI", "VIA USB");
    Serial.println("No Wi-Fi credentials. Run scripts/provision_wifi.py via USB.");
  } else {
    displayMessage("WIFI", "CONNECT");
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    password = "";
  }
}

void loop() {
  pollProvisioning();
  pollOrientation();
  pollWifiInfoButton();
  pollWifi();
  pollClock();
  delay(1);
}
