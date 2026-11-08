#pragma once
#include <cstdint>

namespace panel {
constexpr uint8_t BRIGHTNESS = 64; // 64 / 255 ~= 25% of maximum channel intensity.
constexpr uint8_t TOP_OFFSET = 3;
constexpr uint8_t BIT_DEPTH = 4; // Prefer the previously stable refresh rate over extra color planes.
constexpr int SIZE = 64;
constexpr int VISIBLE_SOURCE_ROWS = SIZE - TOP_OFFSET;

inline uint16_t color(uint16_t rgb565) {
  // Round instead of truncating to avoid unnecessarily losing dark shades.
  const uint16_t red = ((rgb565 >> 11) * BRIGHTNESS + 127) / 255;
  const uint16_t green = (((rgb565 >> 5) & 63) * BRIGHTNESS + 127) / 255;
  const uint16_t blue = ((rgb565 & 31) * BRIGHTNESS + 127) / 255;
  return (red << 11) | (green << 5) | blue;
}
} // namespace panel
