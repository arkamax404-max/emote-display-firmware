#include "idle_clock.h"
#include "wifi_info.h"
#include <iostream>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "line " << __LINE__ << ": " #condition "\n"; return 1; } } while (false)
using idle_clock::Source;

static void attempt(SenderConnection &connection, SenderStats &stats, uint32_t ip) {
  connection.reset();
  for (char c : std::string("POST /emote HTTP/1.1\r\n")) connection.feed(c, ip, stats);
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const std::string test = argv[1];
  SenderStats stats;
  SenderConnection connection;
  idle_clock::State clock;
  if (test == "attempts") {
    attempt(connection, stats, 0xc0a80101);
    CHECK(stats.size() == 1 && stats.at(0).requests == 1);
    CHECK(clock.source(0, true, false, false) == Source::Clock);
    clock.pngResult(true, 1000);
    CHECK(clock.source(1000, true, true, false) == Source::Frame);
    attempt(connection, stats, 0xc0a80101);
    clock.pngResult(false, 10999); // Rejected body still counts as an attempt.
    CHECK(stats.size() == 1 && stats.at(0).requests == 2);
    CHECK(clock.source(10999, true, true, false) == Source::Frame);
    CHECK(clock.source(11000, true, true, false) == Source::Clock);
    CHECK(clock.source(11000, false, true, false) == Source::Frame);
    connection.reset();
    for (char c : std::string("GET /health HTTP/1.1\r\n")) connection.feed(c, 2, stats);
    CHECK(stats.size() == 1 && stats.at(0).requests == 2);
    CHECK(clock.source(11001, true, true, false) == Source::Clock);
    attempt(connection, stats, 0xc0a80102);
    clock.pngResult(true, 12000);
    CHECK(stats.size() == 2 && stats.at(1).requests == 1);
    CHECK(clock.source(21999, true, true, false) == Source::Frame);
    CHECK(clock.source(22000, true, true, false) == Source::Clock);
  } else if (test == "overlay") {
    WifiInfoOverlay overlay;
    clock.pngResult(true, 0);
    overlay.update(true, 0, stats.size());
    CHECK(overlay.update(true, 35, stats.size()));
    CHECK(overlay.visible() && overlay.page() == 0);
    CHECK(wifiInfoLines(overlay.page(), true, "LAN", "192.168.1.9", -40, &stats)[0] == "WIFI OK");
    overlay.update(false, 5035, stats.size());
    CHECK(overlay.page() == 1);
    CHECK(wifiInfoLines(overlay.page(), true, "LAN", "", -40, &stats)[1] == "LAN");
    overlay.update(false, 10035, stats.size());
    CHECK(overlay.page() == 2);
    CHECK(wifiInfoLines(overlay.page(), true, "LAN", "", -40, &stats)[2] == "NONE YET");
    CHECK(clock.source(10035, true, true, overlay.visible()) == Source::WifiInfo);
    // New senders extend the active overlay, even after the PNG lease expires.
    for (uint32_t i = 1; i <= SenderStats::CAPACITY; ++i) attempt(connection, stats, 0xc0a80100 + i);
    attempt(connection, stats, 0xc0a80101);
    CHECK(stats.size() == 8 && stats.at(0).requests == 2);
    const auto lines = wifiInfoLines(overlay.page(), true, "LAN", "", -40, &stats);
    CHECK(lines[0] == "SENDER 1/8" && lines[1] == "192.168." && lines[2] == "1.1");
    CHECK(lines[3] == "REQUESTS" && lines[4] == "2");
    for (uint32_t page = 3; page <= 9; ++page) {
      overlay.update(false, 35 + page * 5000, stats.size());
      CHECK(overlay.visible() && overlay.page() == page);
      CHECK(wifiInfoLines(overlay.page(), true, "LAN", "", -40, &stats)[0] ==
            "SENDER " + std::to_string(page - 1) + "/8");
      CHECK(clock.source(35 + page * 5000, true, true, overlay.visible()) == Source::WifiInfo);
    }
    // Accepted PNGs keep renewing their lease while the overlay has priority.
    clock.pngResult(true, 49000);
    CHECK(clock.source(49000, true, true, overlay.visible()) == Source::WifiInfo);
    overlay.update(false, 50034, stats.size());
    CHECK(overlay.visible());
    CHECK(overlay.update(false, 50035, stats.size()));
    CHECK(!overlay.visible());
    CHECK(clock.source(50035, true, true, overlay.visible()) == Source::Frame);
    clock.pngResult(false, 58999);
    CHECK(clock.source(59000, true, true, overlay.visible()) == Source::Clock);
    CHECK(stats.size() == 8 && stats.at(0).requests == 2);
  } else return 2;
  std::cout << "OK\n";
}
