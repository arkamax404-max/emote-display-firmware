// Host test CLI for the bounded serial ingress protocol
// (docs/serial-transport.md). Modes:
//   frame [chunk] [capacity]  parse stdin as one stream, report the final state
//   seq [chunk]               stdin parts separated by \n----\n share one parser;
//                             the literal parts RESET and SHOW control/report state
//   reply <requestId> <code>  write the encoded reply frame to stdout
//   reply-for                 parse stdin and write the correlated reply frame
//   crc                       check incremental CRC parity with receiver::crc32
//   bounds                    print MAX_FRAME
#include "receiver_protocol.h"
#include "serial_protocol.h"
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
// feed() now returns the number of bytes consumed and stops right after a
// terminal event; the caller must advance by the returned count instead of
// assuming the whole chunk was taken.
void feedChunked(serial::Parser &parser, const std::vector<uint8_t> &bytes, size_t chunk) {
  size_t offset = 0;
  bool fed = false;
  while (offset < bytes.size()) {
    // Report the first terminal event of THIS call, like the firmware
    // integration: bytes after a Complete/Error are handled by the next
    // poll, not silently rescanned into this report. A fresh call always
    // feeds: the parser auto-resyncs from a previous terminal state.
    if (fed && (parser.status() == serial::Parser::Status::Complete ||
                parser.status() == serial::Parser::Status::Error)) break;
    const size_t take = (chunk && chunk < bytes.size() - offset) ? chunk : bytes.size() - offset;
    const size_t consumed = parser.feed(bytes.data() + offset, take);
    if (consumed == 0) break;
    fed = true;
    offset += consumed;
  }
}

// One report line per parser state: "complete <op> <requestId> <session|->
// <clock|avatar> <payloadLen> <payloadCrc> <name|-> skipped=<n> [utc=<epoch>]",
// "error <code> skipped=<n>", "active"/"idle skipped=<n>".
void report(const serial::Parser &parser, const uint8_t *payload) {
  switch (parser.status()) {
    case serial::Parser::Status::Complete: {
      const serial::Frame &f = parser.frame();
      std::cout << "complete " << serial::opName(f.op) << ' ' << f.requestId << ' '
                << (f.hasSession ? f.sessionId : "-") << ' '
                << (f.clockState ? "clock" : "avatar") << ' ' << f.payloadLength << ' '
                << receiver::crc32(payload, f.payloadLength) << ' '
                << (f.hasName ? f.projectName : "-")
                << " skipped=" << parser.skipped();
      if (f.op == serial::Op::ClockUtc) std::cout << " utc=" << f.utcSeconds;
      std::cout << '\n';
      break;
    }
    case serial::Parser::Status::Error:
      std::cout << "error " << serial::codeName(parser.error())
                << " skipped=" << parser.skipped() << '\n';
      break;
    case serial::Parser::Status::Active:
      std::cout << "active\n";
      break;
    default:
      std::cout << "idle skipped=" << parser.skipped() << '\n';
  }
}

// "last ..." prints the exposed frame regardless of state: after a Complete it
// must equal the accepted frame, after an Error it must be scrubbed.
void showLast(const serial::Parser &parser) {
  const serial::Frame &f = parser.frame();
  std::cout << "last " << serial::opName(f.op) << ' ' << f.requestId << ' '
            << (f.hasSession ? f.sessionId : "-") << ' '
            << (f.clockState ? "clock" : "avatar") << ' ' << f.payloadLength << ' '
            << (f.hasName ? f.projectName : "-") << '\n';
}
} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);  // keep CR/LF and 0x1A intact like protocol_cli
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  const std::string input((std::istreambuf_iterator<char>(std::cin)),
                          std::istreambuf_iterator<char>());
  const std::string mode = argc > 1 ? argv[1] : "frame";
  if (mode == "bounds") {
    std::cout << serial::MAX_FRAME << '\n';
    return 0;
  }
  if (mode == "crc") {
    const std::vector<uint8_t> bytes(input.begin(), input.end());
    uint32_t incremental = serial::crcBegin();
    for (const uint8_t b : bytes) incremental = serial::crcFeed(incremental, b);
    std::cout << (serial::crcEnd(incremental) == receiver::crc32(bytes.data(), bytes.size())
                      ? "ok" : "mismatch") << '\n';
    return 0;
  }
  if (mode == "reply") {
    uint8_t out[serial::REPLY_SIZE];
    serial::encodeReply(out, static_cast<uint16_t>(std::atoi(argv[2])),
                        static_cast<serial::Code>(std::atoi(argv[3])));
    std::fwrite(out, 1, serial::REPLY_SIZE, stdout);
    return 0;
  }

  // "frame": one stream through a fresh parser; argv[3] optionally shrinks
  // the payload capacity to exercise the oversize rejection.
  const size_t capacity = argc > 3 ? static_cast<size_t>(std::atoi(argv[3])) : serial::MAX_PNG;
  std::vector<uint8_t> payload(capacity);
  serial::Parser parser(payload.data(), capacity);
  const size_t chunk = argc > 2 ? static_cast<size_t>(std::atoi(argv[2])) : 0;
  const size_t take = chunk ? chunk : input.size();
  const std::vector<uint8_t> bytes(input.begin(), input.end());

  if (mode == "consume") {
    // Report the public feed() contract: bytes consumed per call and the
    // resulting status; a suffix after a terminal event stays unconsumed.
    size_t offset = 0;
    while (offset < bytes.size()) {
      const size_t consumed = parser.feed(bytes.data() + offset, bytes.size() - offset);
      offset += consumed;
      std::cout << consumed << ' ';
      switch (parser.status()) {
        case serial::Parser::Status::Complete: std::cout << "complete\n"; break;
        case serial::Parser::Status::Error: std::cout << "error\n"; break;
        case serial::Parser::Status::Active: std::cout << "active\n"; break;
        default: std::cout << "idle\n";
      }
      if (consumed == 0) break;
    }
    return 0;
  }
  if (mode == "reply-for") {
    // Stop-and-wait with resync: every terminal event answers exactly one
    // correlated reply, then feeding resumes with the unconsumed remainder.
    size_t offset = 0;
    while (offset < bytes.size()) {
      const size_t consumed = parser.feed(bytes.data() + offset, bytes.size() - offset);
      offset += consumed;
      if (parser.status() == serial::Parser::Status::Complete)
        serial::encodeReply(payload.data(), parser.frame().requestId, serial::Code::Ok);
      else if (parser.status() == serial::Parser::Status::Error)
        serial::encodeReply(payload.data(), parser.lastRequestId(), parser.error());
      else break;
      std::fwrite(payload.data(), 1, serial::REPLY_SIZE, stdout);
      if (consumed == 0) break;
    }
    return 0;
  }
  if (mode == "seq") {
    const std::string separator = "\n----\n";
    size_t start = 0;
    while (true) {
      const size_t next = input.find(separator, start);
      const std::string part =
          input.substr(start, next == std::string::npos ? std::string::npos : next - start);
      if (part == "RESET") {
        parser.reset();
        std::cout << "reset\n";
      } else if (part == "SHOW") {
        showLast(parser);
      } else {
        const std::vector<uint8_t> partBytes(part.begin(), part.end());
        feedChunked(parser, partBytes, take);
        report(parser, payload.data());
      }
      if (next == std::string::npos) break;
      start = next + separator.size();
    }
    return 0;
  }
  // "frame": one stream through a fresh parser.
  feedChunked(parser, bytes, take);
  report(parser, payload.data());
  return 0;
}
