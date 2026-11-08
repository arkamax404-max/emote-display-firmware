#pragma once
#include <PNGdec.h>
#include "receiver_protocol.h"

// Staging buffer is never presented until decoding has succeeded in full.
class FrameDecoder {
 public:
  bool decode(uint8_t *bytes, size_t length) {
    if (!receiver::validPngEnvelope(bytes, length)) return false;
    if (png.openRAM(bytes, static_cast<int>(length), drawRow) != PNG_SUCCESS) return false;
    rows = 0;
    invalidRow = false;
    const bool dimensions = png.getWidth() == 64 && png.getHeight() == 64 && !png.isInterlaced();
    const int result = dimensions ? png.decode(this, PNG_CHECK_CRC) : PNG_INVALID_FILE;
    png.close();
    return result == PNG_SUCCESS && !invalidRow && rows == UINT64_MAX;
  }
  const uint16_t *pixels() const { return staging; }

 private:
  PNG png;
  uint16_t staging[64 * 64];
  uint64_t rows = 0;
  bool invalidRow = false;

  static int drawRow(PNGDRAW *draw) {
    auto *self = static_cast<FrameDecoder *>(draw->pUser);
    // Pinned PNGdec 1.1.7 retains the filter byte immediately before pPixels
    // but silently treats unknown filters as None. Reject them explicitly.
    if (draw->iWidth != 64 || draw->y < 0 || draw->y >= 64 || draw->pPixels[-1] > 4 ||
        (self->rows & (uint64_t(1) << draw->y))) {
      self->invalidRow = true;
      return 0;
    }
    self->png.getLineAsRGB565(draw, self->staging + draw->y * 64, PNG_RGB565_LITTLE_ENDIAN, 0);
    self->rows |= uint64_t(1) << draw->y;
    return 1;
  }
};
