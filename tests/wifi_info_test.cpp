#include "wifi_info.h"
#include <cassert>

int main() {
  WifiInfoOverlay overlay;
  assert(!overlay.visible());
  assert(!overlay.update(true, 0));
  assert(!overlay.update(false, 10)); // Bounce.
  assert(!overlay.update(true, 20));
  assert(!overlay.update(true, 54));
  assert(overlay.update(true, 55));
  assert(overlay.visible() && overlay.page() == 0);
  assert(!overlay.update(true, 1000)); // Holding does not retrigger.
  assert(overlay.update(false, 5055));
  assert(overlay.visible() && overlay.page() == 1);
  assert(overlay.update(false, 10055));
  assert(overlay.page() == 0);
  assert(overlay.update(false, 15055));
  assert(!overlay.visible());

  overlay.update(true, 16000);
  overlay.update(true, 16035);
  assert(overlay.visible());
  overlay.update(false, 16100); overlay.update(false, 16135);
  overlay.update(true, 16200); overlay.update(true, 16235);
  assert(!overlay.visible()); // Second press dismisses early.

  WifiInfoOverlay wrapped;
  wrapped.update(true, UINT32_MAX - 20);
  assert(wrapped.update(true, 20));
  assert(wrapped.visible());
  assert(wrapped.update(false, 15020));
  assert(!wrapped.visible());

  const auto ip = wifiInfoLines(0, true, "test-network", "192.168.1.227", -65);
  assert(ip[2] + ip[3] == "192.168.1.227");
  assert(ip[0] == "WIFI OK" && ip[5] == "RSSI -65");
  const auto name = wifiInfoLines(1, true, "12345678901234567890123456789012", "255.255.255.255", -100);
  assert(name[1] + name[2] + name[3] + name[4] == "12345678901234567890123456789012");
  for (const auto &lines : {ip, name, wifiInfoLines(0, false, "", "", 0),
                           wifiInfoLines(1, false, "", "", 0),
                           wifiInfoLines(0, true, "x", "255.255.255.255", -100)}) {
    for (size_t row = 0; row < lines.size(); ++row) {
      assert(lines[row].size() <= 10);
      assert(2 + lines[row].size() * 6 <= 64);
      assert(3 + row * 8 + 7 < 64);
    }
  }
  const auto offline = wifiInfoLines(0, false, "test-network", "192.168.1.227", -65);
  assert(offline[0] == "NO WIFI" && offline[1] == "NO IP" && offline[2].empty());
  assert(wifiInfoAscii("a\nb") == "a?b");
  assert(wifiInfoAscii("caf\xc3\xa9") == "caf?");
}
