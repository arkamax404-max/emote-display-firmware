#pragma once
#include <cstdint>

namespace panel {
// S10/T11: brightness is a RUNTIME value, not a compile-time constant. It is
// stored as a whole percent and every drawing path — the clock, the
// messages, the version footer, the diagnostic overlay and the received PNG
// frames — shares one scaling through the single color() helper below.
// Range 10..80 percent in steps of 5, default 30 (the user's conservative
// default); percent * 255 / 100 is the channel scale, so the 30 % default
// (scale 76) stays in the same order of magnitude as the previous
// compile-time constant 64/255. The scale is capped at 255 and by itself
// never saturates a full channel above the requested percentage.
constexpr uint8_t BRIGHTNESS_MIN_PERCENT = 10;
constexpr uint8_t BRIGHTNESS_MAX_PERCENT = 80;
constexpr uint8_t BRIGHTNESS_STEP_PERCENT = 5;
constexpr uint8_t BRIGHTNESS_DEFAULT_PERCENT = 30;
constexpr uint8_t TOP_OFFSET = 0;
constexpr uint8_t BIT_DEPTH = 4; // Prefer the previously stable refresh rate over extra color planes.
constexpr int SIZE = 64;
constexpr int VISIBLE_SOURCE_ROWS = SIZE - TOP_OFFSET;

inline bool validBrightnessPercent(uint8_t percent) {
  return percent >= BRIGHTNESS_MIN_PERCENT && percent <= BRIGHTNESS_MAX_PERCENT &&
         (percent - BRIGHTNESS_MIN_PERCENT) % BRIGHTNESS_STEP_PERCENT == 0;
}

// The stated mapping rule, one truncating integer expression: 30 -> 76.
inline uint8_t brightnessScaleFromPercent(uint8_t percent) {
  return static_cast<uint8_t>(static_cast<unsigned>(percent) * 255u / 100u);
}

// The live runtime percent. Only setBrightnessPercent() may change it —
// out-of-range values are rejected with the current value untouched, so an
// invalid POST can never darken or blind the panel.
inline uint8_t &brightnessPercentRef() {
  static uint8_t value = BRIGHTNESS_DEFAULT_PERCENT;
  return value;
}
inline uint8_t brightnessPercent() { return brightnessPercentRef(); }
inline uint8_t brightnessScale() {
  return brightnessScaleFromPercent(brightnessPercentRef());
}
inline void setBrightnessPercent(uint8_t percent) {
  if (!validBrightnessPercent(percent)) return;
  brightnessPercentRef() = percent;
}

inline uint16_t color(uint16_t rgb565) {
  // Read the scale once so one colour is always one consistent mix.
  const unsigned scale = brightnessScale();
  // Round instead of truncating to avoid unnecessarily losing dark shades.
  const uint16_t red = ((rgb565 >> 11) * scale + 127) / 255;
  const uint16_t green = (((rgb565 >> 5) & 63) * scale + 127) / 255;
  const uint16_t blue = ((rgb565 & 31) * scale + 127) / 255;
  return (red << 11) | (green << 5) | blue;
}
} // namespace panel
