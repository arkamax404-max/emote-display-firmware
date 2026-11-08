#pragma once
#include <cmath>
#include <cstdint>

// Gravity selects one of four orientations. Diagonal, flat or rapidly moving
// readings hold the current image; a new direction must persist for 350 ms.
class Orientation {
 public:
  explicit Orientation(int reference = -1) : reference_(reference >= 0 && reference < 4 ? reference : -1) {}
  int reference() const { return reference_; }
  uint8_t rotation() const { return rotation_; }
  void resetReference() { reference_ = -1; rotation_ = 0; candidate_ = -1; }

  bool update(float x, float y, float z, uint32_t now) {
    const float magnitudeSquared = x * x + y * y + z * z;
    const float ax = std::fabs(x), ay = std::fabs(y);
    int direction = -1;
    if (std::isfinite(magnitudeSquared) && magnitudeSquared >= 7 * 7 && magnitudeSquared <= 12 * 12) {
      // The direction index advances clockwise in display coordinates.
      if (ay >= 5.5f && ay > ax * 1.3f) direction = y > 0 ? 0 : 2;
      else if (ax >= 5.5f && ax > ay * 1.3f) direction = x > 0 ? 1 : 3;
    }
    if (direction < 0) { candidate_ = -1; return false; }
    if (candidate_ != direction) { candidate_ = direction; since_ = now; return false; }
    if (uint32_t(now - since_) < 350) return false;
    if (reference_ < 0) reference_ = direction;
    // Counter-rotate the content as the physical panel rotates.
    const uint8_t next = (reference_ - direction + 4) % 4;
    if (next == rotation_) return false;
    rotation_ = next;
    return true;
  }

 private:
  int reference_;
  uint8_t rotation_ = 0;
  int candidate_ = -1;
  uint32_t since_ = 0;
};
