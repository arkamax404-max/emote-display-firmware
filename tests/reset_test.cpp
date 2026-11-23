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
//             bit2 updir, bit3 brightness. The string keys hold fixed
//             non-empty values; `updir` is stored as the unsigned byte it
//             actually is (Preferences getUChar/putUChar), read and written
//             through the typed accessors. removeKey leaves both key and
//             value in place when its fail bit is set (simulated write
//             failure); a failWrite bit makes the matching write report
//             failure and store nothing (a restore write that cannot be
//             persisted).
//       erase <present> <fail> [<failwrite>]
//                             → "erase <ok|unverified> left=<mask>"
//       dump                   → "dump ssid=<len>:<hex> password=<len>:<hex>
//                              updir=<n> brightness=<len>:<hex>"
//                              with "-" instead of the spec for absent keys,
//                              so tests can pin byte-identical restore; the
//                              typed `updir` byte is shown in decimal.
#include "wifi_credentials_reset.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
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
  FakeStore(unsigned keys, unsigned failMask, unsigned failWriteMask)
      : keys_(keys), failMask_(failMask), failWriteMask_(failWriteMask) {
    values_["ssid"] = "my ssid";
    values_["password"] = "p4ssw0rd!";
    updir_ = 3;          // typed: the orientation byte the panel stores
    values_["brightness"] = "16"; // out of scope, must survive untouched
  }
  bool hasKey(const char *key) override { return (keys_ & maskFor(key)) != 0; }
  bool readString(const char *key, std::string &value) override {
    if (std::strcmp(key, "updir") == 0) return false; // typed key, not a string
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    value = it->second;
    return true;
  }
  bool writeString(const char *key, const std::string &value) override {
    if (std::strcmp(key, "updir") == 0) return false;
    if ((failWriteMask_ & maskFor(key)) != 0) return false; // store nothing
    values_[key] = value;
    keys_ |= maskFor(key);
    return true;
  }
  bool readUChar(const char *key, uint8_t &value) override {
    if (std::strcmp(key, "updir") != 0) return false;
    if ((keys_ & maskFor(key)) == 0) return false; // absent, distinguishable
    value = updir_;
    return true;
  }
  bool writeUChar(const char *key, uint8_t value) override {
    if (std::strcmp(key, "updir") != 0) return false;
    if ((failWriteMask_ & maskFor(key)) != 0) return false; // store nothing
    updir_ = value;
    keys_ |= maskFor(key);
    return true;
  }
  void removeKey(const char *key) override {
    keys_ &= ~maskFor(key) | failMask_;
  }
  unsigned keys() const { return keys_; }

  std::string valueSpec(const char *key) const {
    if ((keys_ & maskFor(key)) == 0) return "-";
    if (std::strcmp(key, "updir") == 0) {
      char spec[16];
      std::snprintf(spec, sizeof(spec), "%u", static_cast<unsigned>(updir_));
      return spec;
    }
    auto it = values_.find(key);
    if (it == values_.end() || it->second.empty()) return "-";
    static const char *kHex = "0123456789abcdef";
    std::string spec;
    char len[16];
    std::snprintf(len, sizeof(len), "%d:", static_cast<int>(it->second.size()));
    spec += len;
    for (unsigned char c : it->second) {
      spec += kHex[c >> 4];
      spec += kHex[c & 0xf];
    }
    return spec;
  }

  static unsigned maskFor(const char *key) {
    if (std::strcmp(key, "ssid") == 0) return 1u;
    if (std::strcmp(key, "password") == 0) return 2u;
    if (std::strcmp(key, "updir") == 0) return 4u;
    if (std::strcmp(key, "brightness") == 0) return 8u; // must stay untouched
    return 0u;
  }

 private:
  unsigned keys_ = 0, failMask_ = 0, failWriteMask_ = 0;
  std::map<std::string, std::string> values_;
  uint8_t updir_ = 0;
};

void runEraseScript() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    unsigned present = 0, fail = 0, failWrite = 0;
    args >> cmd >> present >> fail >> failWrite; // failWrite optional (0)
    if (cmd != "erase") continue;
    FakeStore store(present, fail, failWrite);
    const bool verified = wifi_reset::eraseCredentials(store);
    std::cout << "erase " << (verified ? "ok" : "unverified") << " left="
              << store.keys() << '\n';
    std::cout << "dump ssid=" << store.valueSpec("ssid")
              << " password=" << store.valueSpec("password")
              << " updir=" << store.valueSpec("updir")
              << " brightness=" << store.valueSpec("brightness") << '\n';
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
