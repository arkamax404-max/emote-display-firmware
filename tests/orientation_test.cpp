#include "orientation.h"
#include <cassert>
#include <limits>

int main() {
  Orientation orientation;
  assert(!orientation.update(0, 9.81f, 0, 0));
  assert(!orientation.update(0, 9.81f, 0, 349));
  assert(orientation.reference() == -1);
  assert(!orientation.update(0, 9.81f, 0, 350));
  assert(orientation.reference() == 0);
  assert(orientation.rotation() == 0);

  // Four stable cardinal positions, counter-rotated in 90-degree steps.
  const float positions[][3] = {{9.81f, 0, 0}, {0, -9.81f, 0}, {-9.81f, 0, 0}, {0, 9.81f, 0}};
  const uint8_t rotations[] = {3, 2, 1, 0};
  uint32_t time = 1000;
  for (int i = 0; i < 4; ++i) {
    assert(!orientation.update(positions[i][0], positions[i][1], 0, time));
    assert(orientation.update(positions[i][0], positions[i][1], 0, time + 350));
    assert(orientation.rotation() == rotations[i]);
    time += 1000;
  }

  // Reject diagonal, flat, moving and non-finite samples, even if held.
  const float invalid[][3] = {{7, 7, 0}, {0, 0, 9.81f}, {20, 0, 0}, {0, 0, 0},
                             {std::numeric_limits<float>::quiet_NaN(), 0, 0}};
  for (const auto &sample : invalid) {
    orientation.update(sample[0], sample[1], sample[2], time);
    assert(!orientation.update(sample[0], sample[1], sample[2], time + 1000));
    assert(orientation.rotation() == 0);
    time += 2000;
  }
  orientation.update(9.81f, 0, 0, time);
  orientation.update(0, 0, 9.81f, time + 300);
  assert(!orientation.update(9.81f, 0, 0, time + 400));
  assert(orientation.rotation() == 0);
  assert(orientation.update(9.81f, 0, 0, time + 750));

  orientation.resetReference();
  assert(orientation.reference() == -1 && orientation.rotation() == 0);
  orientation.update(-9.81f, 0, 0, time + 1000);
  orientation.update(-9.81f, 0, 0, time + 1350);
  assert(orientation.reference() == 3 && orientation.rotation() == 0);

  // Persisted reference and millis() overflow.
  Orientation restored(3);
  restored.update(0, 9.81f, 0, UINT32_MAX - 100);
  assert(restored.update(0, 9.81f, 0, 300));
  assert(restored.rotation() == 3);
  assert(Orientation(255).reference() == -1);
}
