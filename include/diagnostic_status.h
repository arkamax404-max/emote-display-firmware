#pragma once
#include <cstdint>
#include "display_settings.h"

// T7 serial RX diagnostic indicator (S6–S11): a 4 px triangle overlay in the
// top-right of the ROTATED logical canvas showing USB serial reception state.
// Host-testable central state machine: the firmware wires its events in and
// draws through draw(), so tests can pin the exact timeline and pixels.
//
// Timing rules (user-corrected):
//  - No marker or timeout from port opening or boot: only the FIRST received
//    byte (any traffic class: console bytes, partial magic, frame bytes)
//    arms the pending window.
//  - Amber while bytes are pending; green only after a binary Image or
//    Control is actually accepted (owner rules + full decode/present); red
//    on binary parse/decode/admission rejection or reception timeout.
//  - Timeout windows mirror the transport contract exactly: 1 s inactivity,
//    10 s total FROM THE FIRST RX BYTE. Nothing is armed after a completed
//    frame, so silence after a terminal result never rejects.
//  - A terminal green/red is held for HOLD_MS before amber may replace it;
//    a new terminal outcome replaces another terminal immediately. With no
//    new bytes the terminal result persists indefinitely.
//  - The EMOTE handshake is a protocol boundary, never an accepted
//    operation: it leaves the signal pending (amber, no timeout armed) until
//    a binary operation; EMTE clears pending amber.
namespace diag {

// Saturating counters: uint32 that never wraps past the maximum.
struct SaturatingCounter {
  uint32_t value = 0;
  void inc() {
    if (value != UINT32_MAX) ++value;
  }
};

enum class Signal : uint8_t { Blank, Amber, Green, Red };

class Status {
 public:
  // Diagnostic timing mirrors the serial transport windows exactly so the
  // indicator can never fire before the transport's own legal slow-frame
  // window (docs/serial-transport.md); the transport reset policy itself is
  // unchanged and owned by serial::Mux.
  static constexpr uint32_t INACTIVITY_MS = 1000;
  static constexpr uint32_t TOTAL_MS = 10000;
  static constexpr uint32_t HOLD_MS = 750;

  SaturatingCounter images, controls, rejected;

  // One byte read from the port, in any mode (console or binary). Wrap-safe
  // uint32 arithmetic throughout: millis() rollover must never fabricate a
  // rejection.
  void onByte(uint32_t now) {
    if (!pending_) {
      pending_ = true;
      firstByte_ = now;
    }
    lastByte_ = now;
    if (signal_ == Signal::Green || signal_ == Signal::Red) {
      if (holding_) return; // amber may only replace the terminal after the hold
      signal_ = Signal::Amber;
      dirty_ = true;
      return;
    }
    if (signal_ != Signal::Amber) {
      signal_ = Signal::Amber;
      dirty_ = true;
    }
  }

  // Only after a complete binary Image was actually presented (decode +
  // panel update); a green claim/heartbeat/release never touches images.
  void onImageAccepted(uint32_t now) {
    accept(now);
    images.inc();
  }

  void onControlAccepted(uint32_t now) {
    accept(now);
    controls.inc();
  }

  // Binary parse/decode/admission rejection. Counters are diagnostic-only:
  // callers keep every pre-existing no-state-change guarantee on rejection.
  void onRejected(uint32_t now) {
    reject(now);
    rejected.inc();
  }

  // EMOTE\n accepted: protocol boundary, not an accepted operation. The
  // handshake bytes already armed pending via onByte; the boundary clears
  // the timeout arming so preparing a frame can never look like a rejection.
  void onBinaryEnter(uint32_t now) {
    (void)now;
    pending_ = false;
    if (signal_ == Signal::Blank) {
      signal_ = Signal::Amber;
      dirty_ = true;
    }
  }

  // EMTE\n accepted: the session closed cleanly, so pending amber has no
  // reception left to describe; a held terminal result persists.
  void onBinaryExit(uint32_t now) {
    (void)now;
    pending_ = false;
    if (signal_ == Signal::Amber) {
      signal_ = Signal::Blank;
      dirty_ = true;
    }
  }

  // Once per loop (batched: at most one transition pass and one redraw).
  // Returns true when the displayed signal changed and was not yet drawn;
  // presentDisplay() clears the same flag, so exactly one show per change.
  bool update(uint32_t now) {
    if (holding_ && uint32_t(now - outcomeAt_) >= HOLD_MS) {
      holding_ = false;
      if (pending_) {
        // Traffic occurred during the hold and is still pending: amber now,
        // even without a new byte at the exact boundary.
        signal_ = Signal::Amber;
        dirty_ = true;
      }
      // Otherwise the terminal result persists: no timeout is armed.
    }
    if (pending_ && (uint32_t(now - lastByte_) >= INACTIVITY_MS ||
                     uint32_t(now - firstByte_) >= TOTAL_MS)) {
      onRejected(now); // timeout is a rejection: red + rejected counter
    }
    const bool changed = dirty_;
    dirty_ = false;
    return changed;
  }

  // The overlay: 4 px right triangle flush to the top-right of the rotated
  // logical canvas — rows 4/3/2/1 pixels all ending at x63 (60..63 at y0,
  // 61..63 at y1, 62..63 at y2, 63 at y3). Ordinary logical drawPixel, same
  // as avatar/text: no additional inverse rotation. Exactly ten writes, so
  // it can never touch any other framebuffer pixel.
  template <typename Display>
  void draw(Display &display) const {
    if (signal_ == Signal::Blank) return;
    const uint16_t color = panel::color(signal_ == Signal::Green
                                            ? display.color565(0, 255, 0)
                                            : signal_ == Signal::Red
                                                  ? display.color565(255, 0, 0)
                                                  : display.color565(255, 191, 0));
    for (int x = 60; x <= 63; ++x) display.drawPixel(x, 0, color);
    for (int x = 61; x <= 63; ++x) display.drawPixel(x, 1, color);
    for (int x = 62; x <= 63; ++x) display.drawPixel(x, 2, color);
    display.drawPixel(63, 3, color);
  }

  Signal signal() const { return signal_; }
  uint32_t imageCount() const { return images.value; }
  uint32_t controlCount() const { return controls.value; }
  uint32_t rejectedCount() const { return rejected.value; }

  // The present wrapper draws the current signal, so any change it included
  // must not trigger a second redraw in the same loop.
  void clearDirty() { dirty_ = false; }

 private:
  void accept(uint32_t now) {
    pending_ = false; // completed frame: no timeout is armed afterwards
    holding_ = true;
    outcomeAt_ = now;
    signal_ = Signal::Green;
    dirty_ = true;
  }

  void reject(uint32_t now) {
    pending_ = false;
    holding_ = true;
    outcomeAt_ = now;
    signal_ = Signal::Red;
    dirty_ = true;
  }

  bool pending_ = false;   // RX traffic seen, no terminal outcome yet
  bool holding_ = false;   // terminal result inside its HOLD_MS window
  bool dirty_ = false;     // displayed signal changed and not yet drawn
  Signal signal_ = Signal::Blank;
  uint32_t firstByte_ = 0; // first pending RX byte (total window origin)
  uint32_t lastByte_ = 0;  // most recent RX byte (inactivity window origin)
  uint32_t outcomeAt_ = 0; // when the current terminal result was set
};
} // namespace diag
