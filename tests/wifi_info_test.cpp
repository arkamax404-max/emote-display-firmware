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
  assert(overlay.page() == 2);
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

  WifiInfoOverlay senders;
  senders.update(true, 0, 8);
  senders.update(true, 35, 8);
  for (uint8_t page = 0; page < 10; ++page) {
    senders.update(false, 35 + page * 5000, 8);
    assert(senders.visible() && senders.page() == page);
  }
  senders.update(false, 50035, 8);
  assert(!senders.visible());

  // New senders extend the bounded schedule without restarting earlier pages.
  WifiInfoOverlay growing;
  growing.update(true, 0);
  growing.update(true, 35);
  growing.update(false, 10035, 2);
  assert(growing.page() == 2);
  growing.update(false, 15035, 2);
  assert(growing.visible() && growing.page() == 3);
  growing.update(false, 20035, 2);
  assert(!growing.visible());

  SenderStats stats;
  const auto empty = wifiInfoLines(2, true, "x", "1.2.3.4", -1, &stats);
  assert(empty[0] == "SENDERS" && empty[2] == "NONE YET" && empty[6] == "UP: CLOSE");
  stats.record(0xffffffff);
  auto sender = wifiInfoLines(2, true, "x", "1.2.3.4", -1, &stats);
  assert(sender[0] == "SENDER 1/1" && sender[1] + sender[2] == "255.255.255.255");
  assert(sender[3] == "REQUESTS" && sender[4] == "1" && sender[5].empty());
  for (uint32_t i = 0; i < 8; ++i) stats.record(i);
  sender = wifiInfoLines(9, false, "", "", 0, &stats);
  assert(sender[0] == "SENDER 8/8" && sender[1] + sender[2] == "0.0.0.6");
  assert(sender[5] == "MORE IPS!" && sender[6] == "UP: CLOSE");
  assert(wifiInfoAscii(std::to_string(UINT32_MAX)).size() == 10);
  for (const auto &lines : {empty, sender})
    for (const auto &line : lines) assert(line.size() <= 10);

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
