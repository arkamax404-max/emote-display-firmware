#pragma once
#include <cstdint>
#include <string>

// T8 DOWN long-press credential reset (S6): pure, host-testable logic in the
// header-only style of portal::BootRetry — every timestamp and level is
// injected, so the host tests pin the exact hold/debounce semantics the
// firmware then wires to PIN_BUTTON_DOWN (GPIO 7, INPUT_PULLUP, active low).
//
// Semantics from the implementation decision: the raw DOWN level is read
// every poll; a raw level change is *accepted* only after it has stayed
// stable for DEBOUNCE_MS (a shorter glitch is filtered and cannot abort an
// already-running hold or start a spurious one); the reset fires only when
// BOTH the accepted pressed level has lasted HOLD_MS continuously AND the
// raw level still reads pressed at that poll — so a release just before the
// threshold can never fire off the stale debounced level; any debounced
// release (low for at least DEBOUNCE_MS) discards the press (a later hold
// needs the full HOLD_MS again), while a glitch shorter than DEBOUNCE_MS
// cannot abort an ongoing hold (it can only delay the firing poll until the
// raw level reads pressed again). All arithmetic is uint32 wrap-safe like
// millis().
namespace wifi_reset {

constexpr uint32_t DEBOUNCE_MS = 50;
constexpr uint32_t HOLD_MS = 5000;

class HoldReset {
 public:
  // One poll: return value is true exactly on the poll where the 5000 ms
  // continuously-debounced press completes with the raw level still pressed
  // (never again until the button is released for at least DEBOUNCE_MS and
  // pressed again).
  bool update(uint32_t now, bool rawPressed) {
    bool fired = false;
    if (rawPressed != raw_) {
      raw_ = rawPressed;
      rawChanged_ = now;
    }
    if (rawPressed != accepted_ && uint32_t(now - rawChanged_) >= DEBOUNCE_MS) {
      accepted_ = rawPressed;
      if (accepted_) {
        anchor_ = now;
        firedSince_ = false; // a fresh hold starts un-fired
      }
    }
    if (accepted_ && !firedSince_ && rawPressed &&
        uint32_t(now - anchor_) >= HOLD_MS) {
      firedSince_ = true; // latched until the debounced release
      fired = true;
    }
    return fired;
  }

 private:
  bool raw_ = false;        // last raw level seen by update()
  bool accepted_ = false;   // debounced level
  bool firedSince_ = false; // fired during the current accepted hold
  uint32_t rawChanged_ = 0; // when the raw level last changed
  uint32_t anchor_ = 0;     // when the accepted press was accepted
};

// Storage interface of the verified erase (S6): the host tests inject a
// fake; the firmware adapts an open Preferences `gentle` namespace onto it.
class CredentialStore {
 public:
  virtual ~CredentialStore() = default;
  virtual bool hasKey(const char *key) = 0;
  // Byte-identical value access for the failure-restore path: readKey
  // returns the stored value (an empty string when absent) and writeKey
  // persists it again unchanged.
  virtual std::string readKey(const char *key) = 0;
  virtual bool writeKey(const char *key, const std::string &value) = 0;
  virtual void removeKey(const char *key) = 0;
};

// The user's reset scope (S6 decision): exactly `ssid`, `password` and
// `updir` from the `gentle` namespace — brightness and every other stored
// value are untouched, and nothing is written outside that namespace.
// Read-back verification: each existing key must report gone right after its
// removal, and a failed removal must leave the stored configuration exactly
// as it was — every value is snapshotted before mutating, and the keys
// already removed are written back byte-identically before reporting
// failure. Returns true only when the whole scope is verified removed (an
// empty namespace qualifies); the caller restarts only on true.
inline bool eraseCredentials(CredentialStore &store) {
  static const char *const kKeys[] = {"ssid", "password", "updir"};
  const int kKeyCount = 3;
  std::string snapshot[kKeyCount];
  bool existed[kKeyCount] = {false, false, false};
  for (int i = 0; i < kKeyCount; ++i) {
    if (!store.hasKey(kKeys[i])) continue;
    existed[i] = true;
    snapshot[i] = store.readKey(kKeys[i]);
  }
  for (int i = 0; i < kKeyCount; ++i) {
    if (!existed[i]) continue;
    store.removeKey(kKeys[i]);
    if (store.hasKey(kKeys[i])) {
      // The removal could not be verified: restore everything up to and
      // including this key byte-identically, then report failure (the caller
      // must not restart). Restoring the failing key too is harmless when it
      // is still present and essential when the store removed it but the
      // read-back lied.
      for (int j = 0; j <= i; ++j) {
        if (existed[j]) (void)store.writeKey(kKeys[j], snapshot[j]);
      }
      return false;
    }
  }
  return true;
}

} // namespace wifi_reset
