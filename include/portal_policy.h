#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// T1 captive-portal policy layer (S1, S2, S3, S4): pure, host-testable
// decisions for the Wi-Fi captive portal fallback — the header-only style of
// include/diagnostic_status.h and include/serial_ingress.h. Every input is
// injected (timestamps, flags, the randomness draw); there are no globals and
// no Arduino/WiFi symbols, so the host tests pin the exact decisions the
// firmware then wires up.

// Constants of the portal surface (S3 corrected AP security):
// - WPA2-protected SoftAP with a random password generated once when the
//   portal opens and never stored: nothing secret gains NVS persistence.
// - Exactly 8 characters: the WPA2 PSK minimum, drawn through esp_random().
// - The alphabet is restricted to glyphs the panel can actually draw through
//   the displayMessage path. The message path prints with the classic 5x7
//   GFX font, which covers the printable ASCII range, so the constraint that
//   really shapes the alphabet is the exclusion of visually confusable pairs
//   (0/O and 1/I/L).
namespace portal {

constexpr const char *AP_SSID = "gentle-matrix";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CLIENTS = 4;

// Boot retry policy (S2): a saved network gets MAX_ATTEMPTS connection
// attempts of ATTEMPT_MS each, separated by PAUSE_MS pauses. Worst case
// before the portal is offered: 4 x 10 s + 3 x 5 s = 55 s. This policy is
// boot-only: a later link loss keeps today's runtime reconnect behaviour and
// never opens the portal.
constexpr uint8_t MAX_ATTEMPTS = 4;
constexpr uint32_t ATTEMPT_MS = 10000;
constexpr uint32_t PAUSE_MS = 5000;
// Exact exhaustion instant: the end of the fourth attempt window.
constexpr uint32_t BOOT_TOTAL_MS = MAX_ATTEMPTS * ATTEMPT_MS +
                                   (MAX_ATTEMPTS - 1) * PAUSE_MS;

// Credential validation before persist (S4): up to 20 s for WL_CONNECTED.
constexpr uint32_t VALIDATION_MS = 20000;

// AP password: length and draw alphabet.
constexpr size_t AP_PASSWORD_LEN = 8;
constexpr size_t AP_PASSWORD_MAX = 63; // same limit the serial rule enforces
constexpr char AP_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
static_assert(sizeof(AP_ALPHABET) - 1 == 31, "alphabet excludes 0, O, 1, I, L");

// Draws full-entropy uint32 words; on the firmware this wraps esp_random().
typedef uint32_t (*RandomDraw)();

// Generates one AP password with the injected draw source: exactly
// AP_PASSWORD_LEN symbols from AP_ALPHABET, NUL-terminated. The caller owns
// the buffer; the password is only ever shown on the panel — never echoed,
// logged, stored or exposed in /health.
inline void generateApPassword(char (&out)[AP_PASSWORD_LEN + 1], RandomDraw draw) {
  for (size_t i = 0; i < AP_PASSWORD_LEN; ++i)
    out[i] = AP_ALPHABET[draw() % (sizeof(AP_ALPHABET) - 1)];
  out[AP_PASSWORD_LEN] = '\0';
}

// The portal decision (S1 + "Missing credentials" decision): enumerated by a
// USB host means a computer is attached (tested with tud_mounted(), never
// with Serial's DTR/RTS operator bool) and provisioning via USB stays
// authoritative — the portal never opens, whatever the credential state.
// With no SSID stored there is nothing to exhaust: the portal opens when no
// USB host is enumerated. Otherwise the portal opens only when the saved
// network's retry schedule is exhausted. The gate is evaluated once, when
// this decision is taken; a USB host appearing later does not retroactively
// cancel a portal already announced.
enum class Decision : uint8_t { None, OpenPortal };
inline Decision decide(bool usbEnumerated, bool hasStoredCredentials,
                       bool attemptsExhausted) {
  if (usbEnumerated) return Decision::None;
  if (!hasStoredCredentials || attemptsExhausted) return Decision::OpenPortal;
  return Decision::None;
}

// Bounded boot-time retry schedule (S2), wrap-safe in uint32 millis().
// Protocol: the caller's first attempt begins at start() (setup()'s
// Wi-Fi.begin counts as attempt one — attempts() returns 1); update() is
// polled every loop with the current connection state and returns the action
// for exactly that poll:
//   Wait      keep as-is (never inside a pause),
//   Retry     a new attempt window opened: reconnect now,
//   Exhausted the whole 55 s schedule ran out (sticky until reboot).
// A connected poll changes nothing here: the caller leaves the boot phase.
class BootRetry {
 public:
  enum class Tick : uint8_t { Wait, Retry, Exhausted };

  void start(uint32_t now) {
    started_ = true;
    start_ = now;
    attempts_ = 1;
  }

  Tick update(uint32_t now, bool connected) {
    if (!started_ || connected) return Tick::Wait;
    const uint32_t elapsed = uint32_t(now - start_);
    if (elapsed >= BOOT_TOTAL_MS) {
      attempts_ = MAX_ATTEMPTS;
      return Tick::Exhausted;
    }
    // Attempt k occupies [start_ + (k-1)*GAP, +ATTEMPT_MS); the pause fills
    // the rest of the GAP. A reconnect is due exactly when the pause ends.
    if (elapsed >= attempts_ * (ATTEMPT_MS + PAUSE_MS)) {
      ++attempts_; // bounded by MAX_ATTEMPTS (else exhausted above)
      return Tick::Retry;
    }
    return Tick::Wait;
  }

  uint8_t attempts() const { return attempts_; }

 private:
  bool started_ = false;
  uint32_t start_ = 0;
  uint8_t attempts_ = 0;
};

// Outcome mapping of one credential-validation attempt (S4): connected
// within VALIDATION_MS means persist the offered credentials and restart;
// otherwise the portal stays open with the stored credentials untouched.
enum class Outcome : uint8_t { KeepPortal, SaveAndReboot };
inline Outcome concludeValidation(bool connected) {
  return connected ? Outcome::SaveAndReboot : Outcome::KeepPortal;
}

// Credential review, mirroring the serial console rules exactly: SSID
// 1-32 bytes, password 8-63 bytes or empty. Same error strings, same order.
enum class CredError : uint8_t { Ok, Ssid, Password };
inline const char *credErrorText(CredError error) {
  switch (error) {
    case CredError::Ssid: return "invalid SSID length";
    case CredError::Password: return "invalid password length";
    default: return "";
  }
}

// Review of the values offered through the portal form (POST /save).
inline CredError reviewCredentials(const std::string &ssid,
                                   const std::string &password) {
  if (ssid.empty() || ssid.size() > 32) return CredError::Ssid;
  if (password.size() > 63 || (password.size() != 0 && password.size() < 8))
    return CredError::Password;
  return CredError::Ok;
}

inline bool tokenMatches(const std::string &expected, const std::string &offered) {
  if (expected.empty() || expected.size() != offered.size()) return false;
  char diff = 0;
  for (size_t i = 0; i < expected.size(); ++i)
    diff = static_cast<char>(diff | (expected[i] ^ offered[i]));
  return diff == 0;
}

// Shaping of one page render's scan results (S5, T7). The portal runs one
// fresh WiFi.scanNetworks() per render; these pure rules select what the
// page offers:
// - hidden networks (empty SSID) are dropped: with the manual SSID field
//   removed by the user's decision there is no way to enter them, and USB
//   provisioning remains the route for those;
// - duplicated SSIDs collapse to their strongest RSSI (the 2.4/5 GHz pair
//   of one network is one choice);
// - the list sorts by descending RSSI, the input order breaking ties, so
//   the strongest networks come first deterministically;
// - the list is capped at SCAN_MAX_ENTRIES so the page stays bounded.
constexpr size_t SCAN_MAX_ENTRIES = 12;

struct ScanEntry {
  std::string ssid;
  int32_t rssi;
};

inline std::vector<ScanEntry> selectNetworks(const ScanEntry *results,
                                             size_t count) {
  std::vector<ScanEntry> unique;
  for (size_t i = 0; i < count; ++i) {
    const ScanEntry &candidate = results[i];
    if (candidate.ssid.empty()) continue; // hidden network: not selectable
    bool kept = false;
    for (ScanEntry &entry : unique) {
      if (entry.ssid != candidate.ssid) continue;
      if (candidate.rssi > entry.rssi) entry.rssi = candidate.rssi;
      kept = true;
      break;
    }
    if (!kept) unique.push_back(candidate);
  }
  std::stable_sort(unique.begin(), unique.end(),
                   [](const ScanEntry &a, const ScanEntry &b) {
                     return a.rssi > b.rssi;
                   });
  if (unique.size() > SCAN_MAX_ENTRIES)
    unique.resize(SCAN_MAX_ENTRIES);
  return unique;
}

// Bounded portal HTTP request shaping (T2). The portal parser accepts only
// the shapes the page emits and OS captive probes produce.
struct RequestLine {
  bool ok = false;
  bool isPost = false;
  std::string path;
};

// "METHOD SP path SP HTTP/1.0|HTTP/1.1"; everything else is an error.
inline RequestLine parseRequestLine(const std::string &firstLine) {
  RequestLine result;
  const size_t methodEnd = firstLine.find(' ');
  if (methodEnd == std::string::npos) return result;
  const std::string method = firstLine.substr(0, methodEnd);
  if (method != "GET" && method != "POST") return result;
  const size_t pathStart = methodEnd + 1;
  const size_t pathEnd = firstLine.find(' ', pathStart);
  if (pathEnd == std::string::npos) return result;
  const std::string version = firstLine.substr(pathEnd + 1);
  if (version != "HTTP/1.0" && version != "HTTP/1.1") return result;
  result.path = firstLine.substr(pathStart, pathEnd - pathStart);
  if (result.path.empty()) return result;
  result.ok = true;
  result.isPost = method == "POST";
  return result;
}

// application/x-www-form-urlencoded body of POST /save: exactly the two
// form keys `ssid` and `password` plus the optional CSRF `token` (each at
// most once, no unknown keys), '+' is a space and %XX requires two hex digits.
struct FormFields {
  bool ok = false;
  std::string ssid, password, token;
};

inline bool decodeUrlValue(const std::string &raw, std::string &out) {
  auto isHexDigit = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
  };
  auto nibble = [](char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
  };
  out.clear();
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%') {
      if (i + 2 >= raw.size()) return false;
      // Exactly two hex digits are required.
      if (!isHexDigit(raw[i + 1]) || !isHexDigit(raw[i + 2])) return false;
      out += static_cast<char>((nibble(raw[i + 1]) << 4) | nibble(raw[i + 2]));
      i += 2;
    } else {
      // Form encoding carries spaces as + or %20 and never raw control
      // bytes; a bare one means the body is not form-encoded.
      if (c == ' ' || (c >= 0 && c < 32) || c == 127) return false;
      out += c;
    }
  }
  return true;
}

inline FormFields parseForm(const char *body, size_t length) {
  FormFields result;
  const std::string input(body, length);
  bool sawSsid = false, sawPassword = false, sawToken = false;
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
      if (!decodeUrlValue(pair.substr(eq + 1), value)) return result;
      if (key == "ssid") {
        if (sawSsid) return result;
        result.ssid = value;
        sawSsid = true;
      } else if (key == "password") {
        if (sawPassword) return result;
        result.password = value;
        sawPassword = true;
      } else if (key == "token") {
        if (sawToken) return result;
        result.token = value;
        sawToken = true;
      } else {
        return result;
      }
    }
    if (last) break;
    start = amp + 1;
  }
  result.ok = sawSsid && sawPassword;
  return result;
}
} // namespace portal
