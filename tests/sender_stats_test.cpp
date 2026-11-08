#include "sender_stats.h"
#include <cassert>
#include <string>

static void feed(SenderConnection &connection, SenderStats &stats, uint32_t ip,
                 const std::string &bytes) {
  for (char c : bytes) connection.feed(c, ip, stats);
}

int main() {
  SenderStats stats;
  assert(stats.size() == 0 && !stats.overflow());
  SenderConnection connection;
  feed(connection, stats, 1, "POST /emo");
  assert(stats.size() == 0);
  feed(connection, stats, 1, "te HTTP/1.1\r");
  assert(stats.size() == 0);
  feed(connection, stats, 1, "\n");
  assert(stats.size() == 1 && stats.at(0).ip == 1 && stats.at(0).requests == 1);
  // Invalid headers, invalid PNG, truncated bodies and timeouts cannot undo
  // classification. Repeated matching lines on the same connection do not count.
  feed(connection, stats, 1, std::string("Bad: ") + char(1) + "\r\nPOST /emote HTTP/1.1\r\n");
  assert(stats.at(0).requests == 1);
  for (const std::string tail : {"Bad: \001", "\r\ninvalid PNG", "\r\npartial", ""}) {
    connection.reset();
    feed(connection, stats, 1, "POST /emote HTTP/1.0\r\n" + tail);
  }
  assert(stats.at(0).requests == 5);
  for (const std::string line : {"GET /health HTTP/1.1\r\n", "POST /other HTTP/1.1\r\n",
       "POST /emote HTTP/2.0\r\n", "POST /emote HTTP/1.1\n", "POST /emote HTTP/1.1\r",
       "POST /emote HTTP/1.1X\r\n", "post /emote HTTP/1.1\r\n"}) {
    connection.reset();
    feed(connection, stats, 2, line);
    assert(stats.size() == 1 && stats.at(0).requests == 5);
  }
  connection.reset();
  feed(connection, stats, 2, "POST /emote HTTP/1.1\r\n");
  assert(stats.size() == 2 && stats.at(1).requests == 1);
  for (uint32_t ip = 3; ip <= 8; ++ip) stats.record(ip);
  stats.record(9); stats.record(10);
  assert(stats.size() == 8 && stats.overflow());
  stats.record(1);
  assert(stats.at(0).requests == 6);
  for (size_t i = 1; i < 8; ++i) {
    assert(stats.at(i).ip == i + 1 && stats.at(i).requests == 1);
  }
  assert(senderSaturatingIncrement(UINT32_MAX - 1) == UINT32_MAX);
  assert(senderSaturatingIncrement(UINT32_MAX) == UINT32_MAX);
}
