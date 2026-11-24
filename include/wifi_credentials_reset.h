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

// Storage interface of the verified erase (S6, trimmed by S11): the host
// tests inject a fake; the firmware adapts an open Preferences `gentle`
// namespace onto it. Every reset-scope key is a string: `ssid` and
// `password` round-trip faithfully through the string accessors, so a
// restore can never replace a stored value with an empty one. A restore
// write that cannot be persisted is never ignored: every write-back
// verifies (write result plus read-back) and any failed verification
// propagates. There is no byte-typed accessor on purpose: since S11 the
// upright orientation byte is NOT in the reset scope (only the /config
// orientation recalibration clears it), so the interface cannot touch it.
class CredentialStore {
 public:
  virtual ~CredentialStore() = default;
  virtual bool hasKey(const char *key) = 0;
  virtual void removeKey(const char *key) = 0;
  // String-valued scope keys (`ssid`, `password`): false read = absent;
  // false write = the value could not be persisted.
  virtual bool readString(const char *key, std::string &value) = 0;
  virtual bool writeString(const char *key, const std::string &value) = 0;
};

// The user's reset scope (S6 decision, trimmed by S11): exactly `ssid` and
// `password` from the `gentle` namespace — the stored upright orientation
// byte, brightness and every other stored value are untouched, and nothing
// is written outside that namespace. Read-back verification: each existing
// key must report gone right after its removal, and a failed removal must
// leave the stored configuration exactly as it was — every value is
// snapshotted before mutating, the keys already removed are written back
// byte-identically with verified read-backs, and a restore write that
// cannot be verified reports failure (never silently ignored). Returns
// true only when the whole scope is verified removed (an empty namespace
// qualifies); the caller restarts only on true.
inline bool eraseCredentials(CredentialStore &store) {
  static const char *const kStringKeys[] = {"ssid", "password"};
  const int kStringKeyCount = 2;
  std::string stringSnapshot[kStringKeyCount];
  bool stringExisted[kStringKeyCount] = {false, false};
  for (int i = 0; i < kStringKeyCount; ++i) {
    if (!store.hasKey(kStringKeys[i])) continue;
    if (!store.readString(kStringKeys[i], stringSnapshot[i])) return false;
    stringExisted[i] = true;
  }

  // Writes every snapshot up to and including `last` back and verifies each
  // one by read-back (attempting them all even when one verifies short, so
  // the configuration is repaired as far as possible); false means at least
  // one restore could not be confirmed.
  auto restoreStrings = [&](int last) {
    bool verified = true;
    for (int j = 0; j <= last; ++j) {
      if (!stringExisted[j]) continue;
      if (!store.writeString(kStringKeys[j], stringSnapshot[j])) {
        verified = false;
        continue;
      }
      std::string back;
      if (!store.readString(kStringKeys[j], back) ||
          back != stringSnapshot[j]) verified = false;
    }
    return verified;
  };

  for (int i = 0; i < kStringKeyCount; ++i) {
    if (!stringExisted[i]) continue;
    store.removeKey(kStringKeys[i]);
    if (store.hasKey(kStringKeys[i])) {
      // The removal could not be verified: restore the string keys up to
      // and including this one byte-identically, then report failure (the
      // caller must not restart). Restoring the failing key too is harmless
      // when it is still present and essential when the store removed it
      // but the read-back lied.
      restoreStrings(i);
      return false;
    }
  }
  return true;
}

} // namespace wifi_reset
