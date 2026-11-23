#include "display_settings.h"
#include <cassert>
#include <initializer_list>

int main() {
  // S10: brightness is a runtime percent, range 10..80 step 5, default 30.
  assert(panel::BRIGHTNESS_MIN_PERCENT == 10);
  assert(panel::BRIGHTNESS_MAX_PERCENT == 80);
  assert(panel::BRIGHTNESS_STEP_PERCENT == 5);
  assert(panel::BRIGHTNESS_DEFAULT_PERCENT == 30);
  // A fresh process starts at the default; the channel scale follows the
  // stated rule percent * 255 / 100 (30 % -> 76, the era of the old
  // compile-time constant 64).
  assert(panel::brightnessPercent() == panel::BRIGHTNESS_DEFAULT_PERCENT);
  assert(panel::brightnessScale() == 30u * 255u / 100u);
  // Exactly the allowed set is valid.
  for (unsigned percent = 0; percent <= 255; ++percent) {
    const bool allowed = percent >= 10 && percent <= 80 && (percent - 10) % 5 == 0;
    assert(panel::validBrightnessPercent(static_cast<uint8_t>(percent)) == allowed);
  }
  // Every valid percent maps exactly by the rule (truncating, no rounding).
  for (unsigned percent = 10; percent <= 80; percent += 5)
    assert(panel::brightnessScaleFromPercent(static_cast<uint8_t>(percent)) ==
           percent * 255 / 100);
  // The setter applies valid values; invalid ones are rejected with the
  // runtime value unchanged.
  panel::setBrightnessPercent(80);
  assert(panel::brightnessPercent() == 80 && panel::brightnessScale() == 204);
  for (const uint8_t bad : {0, 5, 9, 11, 12, 31, 81, 85, 255}) {
    panel::setBrightnessPercent(bad);
    assert(panel::brightnessPercent() == 80 && panel::brightnessScale() == 204);
  }
  // color() follows the LIVE scale through the one shared helper.
  assert(panel::color(0) == 0);
  assert(panel::color(0xf800) == (25 << 11)); // (31*204+127)/255 = 25
  assert(panel::color(0x07e0) == (50 << 5));  // (63*204+127)/255 = 50
  assert(panel::color(0x001f) == 25);
  panel::setBrightnessPercent(panel::BRIGHTNESS_DEFAULT_PERCENT);
  assert(panel::brightnessPercent() == 30 && panel::brightnessScale() == 76);
  const unsigned scale = panel::brightnessScale();
  const unsigned red = (31 * scale + 127) / 255, green = (63 * scale + 127) / 255;
  assert(panel::color(0xffff) ==
         static_cast<uint16_t>((red << 11) | (green << 5) | red));
  assert(panel::color(0xffff) == static_cast<uint16_t>((9 << 11) | (19 << 5) | 9));
  // Preserve dark shades when possible; do not mix RGB channels.
  assert(panel::color(4 << 11) == 1 << 11);
  for (int channel = 0; channel < 3; ++channel) {
    const int shift = channel == 0 ? 11 : channel == 1 ? 5 : 0;
    const int maximum = channel == 1 ? 63 : 31;
    uint16_t previous = 0;
    for (int value = 0; value <= maximum; ++value) {
      const uint16_t scaled = panel::color(value << shift);
      assert(scaled >= previous && scaled <= (value << shift));
      previous = scaled;
    }
  }
  // 30 % keeps the same order of magnitude as the compile-time era (64),
  // inside a factor of two either way.
  assert(panel::brightnessScale() >= 32 && panel::brightnessScale() <= 128);
  // All drawn pixels remain inside 64 rows; both text labels still fit.
  for (int y = 0; y < panel::VISIBLE_SOURCE_ROWS; ++y) {
    assert(y + panel::TOP_OFFSET == y && y + panel::TOP_OFFSET < 64);
  }
  assert(58 + panel::TOP_OFFSET < 64); // Last lit phase-text row.
}
