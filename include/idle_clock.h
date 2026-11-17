#pragma once
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace idle_clock {
constexpr const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
enum class Source { Message, Frame, Clock, WifiInfo };

// Only the serial main loop mutates this state. No epoch-based lease deadlines.
class State {
 public:
  static constexpr uint32_t SESSION_STALE_MS = 15000; // S5 staleness window.
  static constexpr size_t MAX_NAME = 384;             // Decoded UTF-8 bound.
  void pngResult(bool accepted, uint32_t now) {
    if (accepted) { lastFrame_ = now; expired_ = false; }
  }
  Source source(uint32_t now, bool validTime, bool hasFrame, bool overlay) {
    if (hasFrame && uint32_t(now - lastFrame_) >= 10000) expired_ = true;
    if (overlay) return Source::WifiInfo;
    if (validTime && (!hasFrame || expired_)) return Source::Clock;
    return hasFrame ? Source::Frame : Source::Message;
  }
  bool redrawDue(uint32_t now, Source source) const {
    // Clock redraws every 500 ms so the lemming animation advances twice per
    // second; the calendar labels only change once per second, so repainting
    // identical text mid-second leaves the displayed time unchanged.
    return !painted_ || source != paintedSource_ ||
           (source == Source::Clock && uint32_t(now - lastPaint_) >= 500);
  }
  void painted(uint32_t now, Source source) {
    painted_ = true; paintedSource_ = source; lastPaint_ = now;
  }
  // Session title store (S5): committed only by an accepted image after the
  // ownership admission and a successful presentation; a later accepted image
  // without the header clears it. Every acceptance refreshes liveness.
  void commitTitle(uint32_t now, bool hasName, const char *name) {
    if (hasName && name && name[0]) {
      std::strncpy(name_, name, MAX_NAME);
      name_[MAX_NAME] = '\0';
      hasName_ = true;
    } else {
      name_[0] = '\0';
      hasName_ = false;
    }
    lastLive_ = now;
  }
  // A successful control call (claim/heartbeat/release) also refreshes
  // liveness; rejected traffic never reaches here.
  void touchTitle(uint32_t now) { lastLive_ = now; }
  // Title for the odd phase of the clock band: empty shows the date, more
  // than SESSION_STALE_MS without accepted traffic shows *NO SESSION*.
  const char *sessionTitle(uint32_t now) const {
    if (!hasName_) return "";
    return uint32_t(now - lastLive_) > SESSION_STALE_MS ? "*NO SESSION*" : name_;
  }
 private:
  uint32_t lastFrame_ = 0, lastPaint_ = 0, lastLive_ = 0;
  bool expired_ = false, painted_ = false, hasName_ = false;
  Source paintedSource_ = Source::Message;
  char name_[MAX_NAME + 1] = {};
};

struct Labels { char date[11], hours[3], minutes[3], seconds[3]; };
inline Labels labels(const std::tm &calendar) {
  Labels result = {};
  // RTC localtime supplies valid calendar fields; bound widths explicitly.
  std::snprintf(result.date, sizeof(result.date), "%02u/%02u/%04u",
                unsigned(calendar.tm_mday) % 100, unsigned(calendar.tm_mon + 1) % 100,
                unsigned(calendar.tm_year + 1900) % 10000);
  std::snprintf(result.hours, sizeof(result.hours), "%02u", unsigned(calendar.tm_hour) % 100);
  std::snprintf(result.minutes, sizeof(result.minutes), "%02u", unsigned(calendar.tm_min) % 100);
  std::snprintf(result.seconds, sizeof(result.seconds), "%02u", unsigned(calendar.tm_sec) % 100);
  return result;
}
} // namespace idle_clock
