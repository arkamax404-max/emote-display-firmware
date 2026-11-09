#pragma once
#include <cstdint>
#include <ctime>
#include <cstdio>

namespace idle_clock {
constexpr const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";
enum class Source { Message, Frame, Clock, WifiInfo };

// Only the serial main loop mutates this state. No epoch-based lease deadlines.
class State {
 public:
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
    return !painted_ || source != paintedSource_ ||
           (source == Source::Clock && uint32_t(now - lastPaint_) >= 1000);
  }
  void painted(uint32_t now, Source source) {
    painted_ = true; paintedSource_ = source; lastPaint_ = now;
  }
 private:
  uint32_t lastFrame_ = 0, lastPaint_ = 0;
  bool expired_ = false, painted_ = false;
  Source paintedSource_ = Source::Message;
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
