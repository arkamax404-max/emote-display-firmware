#pragma once
#include <array>
#include <cstdint>
#include <string>

/** Debounced UP button, with a temporary two-page overlay. No blocking delays. */
class WifiInfoOverlay {
 public:
  static constexpr uint32_t DEBOUNCE_MS = 35;
  static constexpr uint32_t DURATION_MS = 15000;
  static constexpr uint32_t PAGE_MS = 5000;

  bool update(bool pressed, uint32_t now) {
    const bool before = visible_;
    const uint8_t beforePage = page_;
    if (pressed != raw_) { raw_ = pressed; rawSince_ = now; }
    if (raw_ != stable_ && uint32_t(now - rawSince_) >= DEBOUNCE_MS) {
      stable_ = raw_;
      if (stable_) { visible_ = !visible_; started_ = now; }
    }
    if (visible_ && uint32_t(now - started_) >= DURATION_MS) visible_ = false;
    page_ = visible_ ? (uint32_t(now - started_) / PAGE_MS) % 2 : 0;
    return before != visible_ || beforePage != page_;
  }
  bool visible() const { return visible_; }
  uint8_t page() const { return page_; }

 private:
  bool raw_ = false, stable_ = false, visible_ = false;
  uint8_t page_ = 0;
  uint32_t rawSince_ = 0, started_ = 0;
};

// Built-in GFX font is 6x8: ten characters fit inside 64 px with 2 px padding.
// Replace unsupported characters rather than displaying arbitrary control bytes.
inline std::string wifiInfoAscii(const std::string &value) {
  std::string result;
  for (unsigned char c : value) {
    if ((c & 0xc0) == 0x80) continue; // UTF-8 continuation byte.
    result += c >= 32 && c <= 126 ? static_cast<char>(c) : '?';
  }
  return result;
}

inline std::array<std::string, 7> wifiInfoLines(uint8_t page, bool connected,
                                              const std::string &ssid,
                                              const std::string &ip, int rssi) {
  std::array<std::string, 7> lines{};
  if (page == 1) {
    lines[0] = "SSID:";
    const std::string name = ssid.empty() ? "(UNSET)" : wifiInfoAscii(ssid).substr(0, 32);
    for (size_t row = 0; row < 4; ++row) {
      const size_t offset = row * 10;
      if (offset < name.size()) lines[row + 1] = name.substr(offset, 10);
    }
    lines[5] = connected ? "CONNECTED" : "NO WIFI";
  } else {
    lines[0] = connected ? "WIFI OK" : "NO WIFI";
    lines[1] = connected ? "IP:" : "NO IP";
    if (connected) {
      const size_t firstDot = ip.find('.');
      const size_t secondDot = firstDot == std::string::npos ? firstDot : ip.find('.', firstDot + 1);
      if (secondDot != std::string::npos) {
        lines[2] = ip.substr(0, secondDot + 1).substr(0, 10);
        lines[3] = ip.substr(secondDot + 1, 10);
      }
      lines[5] = ("RSSI " + std::to_string(rssi)).substr(0, 10);
    } else lines[5] = "RETRYING";
  }
  lines[6] = "UP: CLOSE";
  return lines;
}
