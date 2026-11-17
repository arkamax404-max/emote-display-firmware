#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Session ownership lease for the LED display
// (docs/led-session-ownership-handoff.md). Trusted-LAN coordination only: the
// identity is not authentication. The lease is independent of the 10 s
// autonomous clock timer, never persists, and boot always begins ownerless.
// All methods take a monotonic millis() timestamp and evaluate expiry before
// every decision, using wrap-safe elapsed arithmetic.
namespace session {
constexpr uint32_t LEASE_MS = 30000;
constexpr size_t MAX_ID_LENGTH = 128;

// Exactly the agreed identity charset: [A-Za-z0-9._:-]{1,128}.
inline bool validSessionId(const char *id, size_t length) {
  if (!id || length == 0 || length > MAX_ID_LENGTH) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = id[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '.' || c == '_' || c == ':' || c == '-')) return false;
  }
  return true;
}

// HTTP status codes double as decisions: 200 allows/succeeds, 409 conflicts
// with the current owner, 400 rejects a malformed identity. Rejections never
// mutate state, so an invalid request preserves image, frame and lease.
class Owner {
 public:
  bool active() const { return active_; }
  bool ownedBy(const char *id) const {
    return active_ && id && std::strcmp(id, sessionId_) == 0;
  }

  // The latest explicit control always wins, even over an active foreign
  // owner. An ordinary PNG never reaches here.
  int claim(const char *id, uint32_t now) {
    if (!accept(id)) return 400;
    set(id, now);
    return 200;
  }

  // Renews the lease of the current owner only; never claims. Without an
  // owner it succeeds without side effects; a foreign session gets 409.
  int heartbeat(const char *id, uint32_t now) {
    if (!accept(id)) return 400;
    expire(now);
    if (!active_) return 200;
    if (!ownedBy(id)) return 409;
    since_ = now;
    return 200;
  }

  // Owner release; idempotent without an owner; a foreign session gets 409
  // so a stale session cannot free the reservation that displaced it.
  int release(const char *id, uint32_t now) {
    if (!accept(id)) return 400;
    expire(now);
    if (!active_) return 200;
    if (!ownedBy(id)) return 409;
    clear();
    return 200;
  }

  // Admission decision for an ordinary PNG: ownerless accepts everything
  // (legacy behavior, nothing claims); while owned, only the owner's image
  // is admitted and a missing or foreign session id gets 409.
  int pngAccess(bool hasSession, const char *id, uint32_t now) {
    expire(now);
    if (!active_) return 200;
    if (hasSession && ownedBy(id)) return 200;
    return 409;
  }

  // Lease renewal for a successfully decoded and presented owner image.
  // Invalid PNGs never renew because callers only renew after a true result
  // from the presentation step.
  void renewPng(uint32_t now) {
    expire(now);
    if (active_) since_ = now;
  }

  // Wrap-safe expiry against millis() rollover.
  void expire(uint32_t now) {
    if (active_ && uint32_t(now - since_) >= LEASE_MS) clear();
  }

  uint32_t remainingMs(uint32_t now) {
    expire(now);
    return active_ ? LEASE_MS - uint32_t(now - since_) : 0;
  }

 private:
  static bool accept(const char *id) {
    return id && validSessionId(id, std::strlen(id));
  }
  void set(const char *id, uint32_t now) {
    std::strncpy(sessionId_, id, MAX_ID_LENGTH);
    sessionId_[MAX_ID_LENGTH] = '\0';
    since_ = now;
    active_ = true;
  }
  void clear() {
    active_ = false;
    sessionId_[0] = '\0';
  }

  char sessionId_[MAX_ID_LENGTH + 1] = {};
  uint32_t since_ = 0;
  bool active_ = false;
};
} // namespace session
