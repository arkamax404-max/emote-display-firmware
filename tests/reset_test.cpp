// Host test CLI for the DOWN long-press credential reset (T8/S6): the
// header-only hold state machine with injected timestamps and the verified
// credential erase with an injected store. Two stdin-script modes:
//
//   hold      include/wifi_credentials_reset.h HoldReset (no Arduino symbols).
//       hold <now> <0|1>       one poll with the raw DOWN level (1 = pressed)
//                              → "hold <fire|->"  (fire = the 5000 ms
//                              continuously-debounced press completed now)
//       reset                  a fresh HoldReset for the next scenario
//
//   erase     include/wifi_credentials_reset.h eraseCredentials over a fake
//             store whose keys are a bitmask: bit0 ssid, bit1 password,
//             bit2 updir, bit3 brightness. removeKey leaves the key in
//             place when its fail bit is set (simulated write failure).
//       erase <present> <fail> → "erase <ok|unverified> left=<mask>"
#include "wifi_credentials_reset.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace {
void runHoldScript() {
  wifi_reset::HoldReset hold;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "reset") {
      hold = wifi_reset::HoldReset();
      std::cout << "reset\n";
    } else if (cmd == "hold") {
      uint32_t now = 0;
      int pressed = 0;
      args >> now >> pressed;
      std::cout << "hold " << (hold.update(now, pressed != 0) ? "fire" : "-") << '\n';
    }
  }
}

class FakeStore : public wifi_reset::CredentialStore {
 public:
  explicit FakeStore(unsigned keys, unsigned failMask)
      : keys_(keys), failMask_(failMask) {}
  bool hasKey(const char *key) override { return (keys_ & maskFor(key)) != 0; }
  void removeKey(const char *key) override {
    keys_ &= ~maskFor(key) | failMask_;
  }
  unsigned keys() const { return keys_; }

  static unsigned maskFor(const char *key) {
    if (std::strcmp(key, "ssid") == 0) return 1u;
    if (std::strcmp(key, "password") == 0) return 2u;
    if (std::strcmp(key, "updir") == 0) return 4u;
    if (std::strcmp(key, "brightness") == 0) return 8u; // must stay untouched
    return 0u;
  }

 private:
  unsigned keys_ = 0, failMask_ = 0;
};

void runEraseScript() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    unsigned present = 0, fail = 0;
    args >> cmd >> present >> fail;
    if (cmd != "erase") continue;
    FakeStore store(present, fail);
    const bool verified = wifi_reset::eraseCredentials(store);
    std::cout << "erase " << (verified ? "ok" : "unverified") << " left="
              << store.keys() << '\n';
  }
}
} // namespace

int main(int argc, char **argv) {
  (void)argc;
  const std::string mode = argc > 1 ? argv[1] : "hold";
  if (mode == "erase") runEraseScript();
  else runHoldScript();
  return 0;
}
