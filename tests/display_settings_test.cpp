#include "display_settings.h"
#include <cassert>

int main() {
  assert(panel::BRIGHTNESS == 64);
  assert(panel::TOP_OFFSET == 3);
  assert(panel::BIT_DEPTH == 4);
  assert(panel::color(0) == 0);
  assert(panel::color(0xffff) == 0x4208);
  assert(panel::color(0xf800) == 0x4000);
  assert(panel::color(0x07e0) == 0x0200);
  assert(panel::color(0x001f) == 0x0008);
  // RGB565 -> 4 Protomatter bitplanes drops one R/B bit and two G bits.
  // At 25% brightness #303030 loses green, while #383838 stays neutral.
  const uint16_t gray30 = panel::color((6 << 11) | (12 << 5) | 6);
  assert((gray30 >> 12) == 1 && ((gray30 >> 7) & 15) == 0 && ((gray30 >> 1) & 15) == 1);
  const uint16_t gray38 = panel::color((7 << 11) | (14 << 5) | 7);
  assert((gray38 >> 12) == 1 && ((gray38 >> 7) & 15) == 1 && ((gray38 >> 1) & 15) == 1);
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
  // All drawn pixels remain inside 64 rows; both text labels still fit.
  for (int y = 0; y < panel::VISIBLE_SOURCE_ROWS; ++y) {
    assert(y + panel::TOP_OFFSET >= 3 && y + panel::TOP_OFFSET < 64);
  }
  assert(58 + panel::TOP_OFFSET < 64); // Last lit phase-text row.
}
