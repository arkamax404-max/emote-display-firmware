#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "display_settings.h"
#include "wifi_info.h"

// S12: when the Wi-Fi connection is established, the panel shows the
// obtained IPv4 address for three seconds and then returns to whatever it
// was showing. The state machine below is the whole timing logic,
// header-only and host-tested; main.cpp only feeds it one connectivity
// check per loop and renders the two address rows while the window is
// active.

namespace wifi_banner {

// Three full seconds, counted from the connection event itself — never
// from a paint: a redraw, a repaint or a repeated WL_CONNECTED poll is a
// pure read for this machine and never restarts or extends the window.
// (C++11: no namespace-scope inline variables, so the constant lives in
// the class, like every other duration constant in this codebase.)
class ConnectBanner {
 public:
  static constexpr uint32_t DURATION_MS = 3000;
  // Exactly one connectivity check per call. Returns true ONLY on the
  // disconnected -> connected transition, the single retrigger: a repeated
  // WL_CONNECTED poll (same loss-free state) never opens a new window, and
  // any disconnected check ends the active window at once — a dead
  // connection paints no address but does arm the next transition, so a
  // reconnect after a real loss shows the banner again.
  bool update(bool connected, uint32_t now) {
    if (!connected) {
      armed_ = false;
      wasConnected_ = false;
      return false;
    }
    if (!wasConnected_) {
      armed_ = true;
      started_ = now;
      wasConnected_ = true;
      return true;
    }
    return false;
  }

  // An incoming PNG keeps priority over the diagnostic: once a frame is
  // staged while the window runs, main.cpp closes the banner, whose panel
  // slot ends for good. No later connected poll can resurrect it; only the
  // next real transition (new connection) can show it again.
  void close() { armed_ = false; }

  // Inside the window: armed and fewer than DURATION_MS from the
  // connection event. The unsigned subtraction is millis()-wrap safe.
  bool active(uint32_t now) const {
    return armed_ && uint32_t(now - started_) < DURATION_MS;
  }

  // The address of the live connection, captured by main.cpp at the
  // transition moment straight from WiFi.localIP(); never a stored or
  // guessed value.
  void capture(const char *address) { address_ = address; }
  const std::string &address() const { return address_; }

 private:
  bool armed_ = false;
  bool wasConnected_ = false;
  uint32_t started_ = 0;
  std::string address_;
};

// The documented split: a full IPv4 address does not fit one line (13
// characters at six pixels are 78 px against 64), so it is cut at its
// second dot into two rows of at most ten characters — byte for byte the
// same rule the Wi-Fi info screen already uses, through the same helper.
inline std::array<std::string, 2> addressLines(const std::string &ip) {
  const auto lines = wifiInfoLines(0, true, "", ip, 0);
  return {lines[2], lines[3]};
}

// The panel render, drawn from the header like every other drawing helper
// in this codebase (idle_clock, weather_screen, firmware_version): the two
// address rows in the Wi-Fi info screen's exact text style — size 1, no
// wrap, the 2 px margin, the same grey, the info grid's rows 2 and 3 — and
// scaled through the one shared brightness helper like every consumer.
template <typename Display>
void draw(Display &display, const std::string &ip) {
  const auto rows = addressLines(ip);
  display.setTextWrap(false);
  display.setTextSize(1);
  for (size_t row = 0; row < rows.size(); ++row) {
    display.setTextColor(panel::color(display.color565(224, 224, 224)));
    display.setCursor(2, panel::TOP_OFFSET + (2 + row) * 8);
    display.print(rows[row].c_str());
  }
}

} // namespace wifi_banner
