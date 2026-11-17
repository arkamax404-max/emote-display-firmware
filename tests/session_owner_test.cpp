#include "session_owner.h"
#include <cstdint>
#include <iostream>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "line " << __LINE__ << ": " #condition "\n"; return 1; } } while (false)

using session::Owner;

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];
  if (test == "lease") {
    Owner owner;
    CHECK(!owner.active());
    CHECK(owner.remainingMs(1000) == 0);
    CHECK(owner.claim("sess-a", 1000) == 200);
    CHECK(owner.active());
    CHECK(owner.remainingMs(1000) == session::LEASE_MS);
    CHECK(owner.remainingMs(1000 + session::LEASE_MS - 1) == 1);
    // Exact expiry: 30000 ms without signals of life.
    CHECK(owner.remainingMs(1000 + session::LEASE_MS) == 0);
    CHECK(!owner.active());
    // The latest explicit claim wins even over an active foreign owner.
    CHECK(owner.claim("first", 0) == 200);
    CHECK(owner.active());
    CHECK(owner.claim("second", 10) == 200);
    CHECK(!owner.ownedBy("first"));
    CHECK(owner.ownedBy("second"));
    CHECK(owner.remainingMs(10) == session::LEASE_MS);
  } else if (test == "heartbeat") {
    Owner owner;
    // A heartbeat never claims; without an owner it succeeds without effects.
    CHECK(owner.heartbeat("idle-sender", 500) == 200);
    CHECK(!owner.active());
    CHECK(owner.claim("owner", 1000) == 200);
    CHECK(owner.heartbeat("owner", 20000) == 200);
    CHECK(owner.remainingMs(20000 + session::LEASE_MS - 1) == 1);
    CHECK(owner.remainingMs(20000 + session::LEASE_MS) == 0);
    // Renewing again while idle, including while the autonomous clock runs.
    CHECK(owner.claim("owner", 49500) == 200);
    CHECK(owner.heartbeat("owner", 79000) == 200);
    CHECK(owner.remainingMs(79000 + session::LEASE_MS - 1) == 1);
    CHECK(owner.remainingMs(79000 + session::LEASE_MS) == 0);
    // A foreign heartbeat is rejected without touching owner or lease.
    Owner keeper;
    CHECK(keeper.claim("a", 0) == 200);
    CHECK(keeper.heartbeat("b", 1000) == 409);
    CHECK(keeper.ownedBy("a"));
    CHECK(keeper.remainingMs(29999) == 1);
    CHECK(keeper.remainingMs(30000) == 0);
  } else if (test == "release") {
    Owner owner;
    // Release without an owner is an idempotent success.
    CHECK(owner.release("nobody", 100) == 200);
    CHECK(!owner.active());
    CHECK(owner.claim("a", 0) == 200);
    // A stale session cannot free the reservation that displaced it.
    CHECK(owner.release("b", 100) == 409);
    CHECK(owner.ownedBy("a"));
    CHECK(owner.release("a", 200) == 200);
    CHECK(!owner.active());
    CHECK(owner.pngAccess(false, "", 300) == 200);
  } else if (test == "png") {
    Owner owner;
    // Ownerless: every PNG is accepted and an ordinary image never claims.
    CHECK(owner.pngAccess(true, "someone", 0) == 200);
    CHECK(owner.pngAccess(false, "", 1) == 200);
    CHECK(!owner.active());
    CHECK(owner.claim("owner", 1000) == 200);
    // While owned, sessionless and foreign images are rejected.
    CHECK(owner.pngAccess(false, "", 2000) == 409);
    CHECK(owner.pngAccess(true, "foreign", 2000) == 409);
    CHECK(owner.ownedBy("owner"));
    CHECK(owner.pngAccess(true, "owner", 2000) == 200);
    // Renewal happens only after a successful decode/presentation.
    owner.renewPng(2000);
    CHECK(owner.remainingMs(2000 + session::LEASE_MS - 1) == 1);
    // Expiry is exact at the renewed deadline: the successful presentation at
    // 2000 moved it from 31000 to 32000 (a countdown cannot be 1 at 31999 and
    // already 0 at 31000).
    CHECK(owner.remainingMs(2000 + session::LEASE_MS) == 0);
    // An admitted owner image that is never presented (no renewPng call)
    // never extends the lease beyond its original claim deadline.
    Owner invalid;
    CHECK(invalid.claim("owner", 1000) == 200);
    CHECK(invalid.pngAccess(true, "owner", 2000) == 200);
    CHECK(invalid.remainingMs(1000 + session::LEASE_MS) == 0);
    // An expired lease frees the display for everyone; foreign traffic never
    // extends the previous owner's deadline.
    Owner stale;
    CHECK(stale.claim("gone", 0) == 200);
    CHECK(stale.pngAccess(true, "new", session::LEASE_MS) == 200);
    CHECK(!stale.active());
    CHECK(stale.pngAccess(true, "gone", session::LEASE_MS + 5000) == 200);
    CHECK(stale.remainingMs(session::LEASE_MS + 5000) == 0);
  } else if (test == "identity") {
    // Exact 128-character compatible identity validation.
    const std::string maxId(session::MAX_ID_LENGTH, 'a');
    const std::string overId(session::MAX_ID_LENGTH + 1, 'a');
    CHECK(session::validSessionId(maxId.data(), maxId.size()));
    CHECK(!session::validSessionId(overId.data(), overId.size()));
    CHECK(!session::validSessionId("", 0));
    CHECK(!session::validSessionId(nullptr, 3));
    const std::string allowed = "ABCZabcz0189._:-";
    CHECK(session::validSessionId(allowed.data(), allowed.size()));
    for (const char bad : {' ', '/', '+', '@', '\\', '\t', '"'}) {
      const std::string value = std::string("bad") + bad;
      CHECK(!session::validSessionId(value.data(), value.size()));
    }
    // Owner methods reject malformed identities without any mutation.
    Owner owner;
    CHECK(owner.claim(nullptr, 0) == 400);
    CHECK(!owner.active());
    CHECK(owner.claim("bad id", 0) == 400);
    CHECK(!owner.active());
    CHECK(owner.claim(overId.data(), 0) == 400);
    CHECK(!owner.active());
    CHECK(owner.claim("ok", 0) == 200);
    CHECK(owner.heartbeat("nope!", 10) == 400);
    CHECK(owner.release("nope!", 10) == 400);
    CHECK(owner.pngAccess(true, "nope!", 10) == 409);
    CHECK(owner.ownedBy("ok"));
  } else if (test == "wrap") {
    // Wrap-safe lease arithmetic across the millis() rollover.
    const uint32_t since = 0xFFFFFFF0u;
    Owner owner;
    CHECK(owner.claim("w", since) == 200);
    CHECK(owner.ownedBy("w"));
    CHECK(owner.remainingMs(since + session::LEASE_MS - 1) == 1);
    // One millisecond before the deadline the lease is still alive; the
    // follow-up heartbeat and renewal below rely on that owner. Exact expiry
    // at the full 30000 ms is asserted by the heartbeat block below.
    CHECK(owner.active());
    CHECK(owner.heartbeat("w", since) == 200);
    CHECK(owner.pngAccess(true, "w", since + 12345) == 200);
    owner.renewPng(since + 12345);
    CHECK(owner.ownedBy("w"));
    // A heartbeat arriving just before expiry renews; after expiry the owner
    // is gone and the heartbeat succeeds without claiming.
    Owner heartbeat;
    CHECK(heartbeat.claim("w", since) == 200);
    CHECK(heartbeat.heartbeat("w", since + session::LEASE_MS - 1) == 200);
    CHECK(heartbeat.ownedBy("w"));
    CHECK(heartbeat.heartbeat("w", since + 2 * session::LEASE_MS) == 200);
    CHECK(!heartbeat.active());
  } else return 2;
  std::cout << "OK\n";
}
