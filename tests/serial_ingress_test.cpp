// Host test CLI for the serial ingress integration (T2). Two modes driven by
// a stdin script, one command per line:
//
//   mux       Mux routing + the integration decision core (admission,
//             presentation stand-in, commit, correlated replies).
//       enter              mux.enter(now)                     → "entered"
//       feed <hex>         route bytes via mux                → event lines
//                                                             + "console <text|->"
//       tick <ms>          now += ms; mux.update(now)         → "tick <mode> <status>"
//       show               → "mode=<> status=<> skipped=<n> req=<id>"
//       present ok|fail    presentation (PNGdec+render) stand-in
//       rtc ok|fail        RTC setter stand-in
//       clockstate         → UTC, settled-frame flag, set and clock paint counts
//       state              → "owner=<0|1> owned=<id|-> title=<t|-> presented=<n>"
//
//   console   ProvisionConsole line protocol in isolation.
//       cfeed <hex>        console bytes                      → "out <line>" events
//                                                             + "enter-binary"
//       ctick <ms>         now += ms; console.tick(now)       → "ctick"
//       cshow              → "store=<ssid>|<pass> orient=<n> reboot=<0|1>"
#include "serial_protocol.h"
#include "serial_ingress.h"
#include "image_ingress.h"
#include "diagnostic_status.h"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
std::string unhex(const std::string &hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    out.push_back(static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

std::string escape(const std::string &text) {
  std::string out;
  char buf[8];
  for (const unsigned char c : text) {
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c >= 0x20 && c <= 0x7e) out += static_cast<char>(c);
    else {
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

const char *statusName(serial::Parser::Status status) {
  switch (status) {
    case serial::Parser::Status::Active: return "active";
    case serial::Parser::Status::Complete: return "complete";
    case serial::Parser::Status::Error: return "error";
    default: return "idle";
  }
}

// Silent console hooks for the mux rig: routing tests never assert console
// text.
serial::ProvisionConsole::Hooks silentHooks() {
  serial::ProvisionConsole::Hooks hooks;
  hooks.print = [](const char *) {};
  hooks.flush = []() {};
  hooks.store = [](const char *, const char *) { return false; };
  hooks.recalibrate = []() {};
  hooks.reboot = []() {};
  return hooks;
}

// The integration decision core: the exact admission/presentation/commit
// sequence pollSerialFrames() performs on the device, with the PNG decode and
// panel render replaced by a flag so host tests can pin the state rules.
struct Rig {
  serial::Parser parser;
  serial::Mux mux;
  serial::ProvisionConsole console;
  session::Owner owner;
  idle_clock::State clock;
  // The same public diag::Status helper main.cpp wires (T7): the rig drives
  // it with the identical event sequence, so these pins are helper pins, not
  // a detached mirror of the firmware dispatch.
  diag::Status diag;
  uint32_t now = 1000;
  bool presentOk = true;
  bool rtcOk = true, hasFrame = false;
  uint32_t utc = 0;
  int presented = 0, rtcSets = 0, clockPaints = 0;
  std::string claimed;

  explicit Rig(uint8_t *buffer, size_t capacity)
      : parser(buffer, capacity), mux(parser), console(silentHooks()) {}

  void handleComplete() {
    const serial::Frame &frame = parser.frame();
    serial::Code code;
    if (frame.op == serial::Op::Image) {
      code = image_ingress::admitImage(frame, owner, now);
      if (code == serial::Code::Ok) {
        const bool decoded = presentOk;
        clock.pngResult(decoded, now);
        if (decoded) {
          image_ingress::commitImage(owner, clock, frame, now);
          ++presented;
          hasFrame = true;
          diag.onImageAccepted(now);
        } else {
          code = serial::Code::Png;
          diag.onRejected(now);
        }
      } else {
        diag.onRejected(now);
      }
    } else if (frame.op == serial::Op::ClockUtc) {
      code = serial::activateClock(frame, owner, now, [&](uint32_t seconds) {
        ++rtcSets;
        if (!rtcOk) return false;
        utc = seconds;
        return true;
      }, [&]() {
        hasFrame = false;
        ++clockPaints;
        diag.onControlAccepted(now);
      });
      if (code != serial::Code::Ok) diag.onRejected(now);
    } else {
      code = image_ingress::admitControl(frame, owner, clock, now);
      if (code == serial::Code::Ok) {
        if (frame.op == serial::Op::Claim) claimed = frame.sessionId;
        if (frame.op == serial::Op::Release) claimed.clear();
        diag.onControlAccepted(now);
      } else {
        diag.onRejected(now);
      }
    }
    std::cout << "ack " << serial::codeName(code) << ' ' << frame.requestId << '\n';
  }

  void handleError() {
    diag.onRejected(now);
    std::cout << "nack " << serial::codeName(parser.error()) << ' '
              << parser.lastRequestId() << '\n';
  }

  void handleRepeatEntry() {
    serial::printEntryAck([](const char *line) { std::cout << "out " << line << '\n'; });
    std::cout << "reenter\n";
  }

  void handleExit() {
    diag.onBinaryExit(now);
    std::cout << "exit\n";
  }
};

// Dispatcher mirror of the firmware pollSerialDispatch(): all buffered
// bytes go through one bounded pass. EnterBinary routes the remaining bytes
// to the mux immediately instead of finishing the console loop over frame
// bytes (finding 3).
void pollFrames(Rig &rig, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    rig.diag.onByte(rig.now);
    const serial::Mux::Routed routed = rig.mux.feed(bytes[i], rig.now);
    switch (routed.event) {
      case serial::Mux::Event::Complete: rig.handleComplete(); break;
      case serial::Mux::Event::Error: rig.handleError(); break;
      case serial::Mux::Event::Exit: rig.handleExit(); break;
      case serial::Mux::Event::RepeatEntry: rig.handleRepeatEntry(); break;
      default: break;
    }
  }
}

void runMuxScript() {
  std::vector<uint8_t> buffer(serial::MAX_PNG);
  Rig rig(buffer.data(), buffer.size());
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "enter") {
      rig.mux.enter(rig.now);
      rig.diag.onBinaryEnter(rig.now);
      std::cout << "entered\n";
    } else if (cmd == "feed") {
      std::string hex;
      args >> hex;
      const std::string bytes = unhex(hex);
      std::string consoleText;
      for (const unsigned char b : bytes) {
        rig.diag.onByte(rig.now);
        const serial::Mux::Routed routed = rig.mux.feed(b, rig.now);
        switch (routed.event) {
          case serial::Mux::Event::Console: consoleText += routed.consoleByte; break;
          case serial::Mux::Event::Complete: rig.handleComplete(); break;
          case serial::Mux::Event::Error: rig.handleError(); break;
          case serial::Mux::Event::Exit: rig.handleExit(); break;
          case serial::Mux::Event::RepeatEntry: rig.handleRepeatEntry(); break;
          default: break;
        }
      }
      std::cout << "console " << (consoleText.empty() ? "-" : escape(consoleText)) << '\n';
    } else if (cmd == "tick") {
      uint32_t ms = 0;
      args >> ms;
      rig.now += ms;
      rig.mux.update(rig.now);
      rig.diag.update(rig.now);
      std::cout << "tick "
                << (rig.mux.mode() == serial::Mux::Mode::Binary ? "binary" : "provision")
                << ' ' << statusName(rig.parser.status()) << '\n';
    } else if (cmd == "diag") {
      const char *name = rig.diag.signal() == diag::Signal::Amber ? "amber"
                         : rig.diag.signal() == diag::Signal::Green ? "green"
                         : rig.diag.signal() == diag::Signal::Red   ? "red"
                                                                    : "blank";
      std::cout << "diag " << name
                << " images=" << rig.diag.imageCount()
                << " controls=" << rig.diag.controlCount()
                << " rejected=" << rig.diag.rejectedCount() << '\n';
    } else if (cmd == "show") {
      std::cout << "mode="
                << (rig.mux.mode() == serial::Mux::Mode::Binary ? "binary" : "provision")
                << " status=" << statusName(rig.parser.status())
                << " skipped=" << rig.parser.skipped()
                << " req=" << rig.parser.lastRequestId() << '\n';
    } else if (cmd == "present") {
      std::string value;
      args >> value;
      rig.presentOk = value == "ok";
      std::cout << "present=" << value << '\n';
    } else if (cmd == "rtc") {
      std::string value;
      args >> value;
      rig.rtcOk = value == "ok";
      std::cout << "rtc=" << value << '\n';
    } else if (cmd == "clockstate") {
      std::cout << "utc=" << rig.utc << " frame=" << rig.hasFrame
                << " sets=" << rig.rtcSets << " clock-paints=" << rig.clockPaints << '\n';
    } else if (cmd == "state") {
      const char *title = rig.clock.sessionTitle(rig.now);
      std::cout << "owner=" << (rig.owner.active() ? 1 : 0) << " owned="
                << (rig.owner.active() && !rig.claimed.empty() ? rig.claimed : "-")
                << " title=" << (*title ? title : "-")
                << " presented=" << rig.presented << '\n';
    }
  }
}

void runConsoleScript() {
  std::string storedSsid, storedPass;
  int orientCount = 0, flushCount = 0;
  bool rebooted = false;
  serial::ProvisionConsole::Hooks hooks;
  hooks.print = [](const char *text) { std::cout << "out " << text << '\n'; };
  hooks.flush = [&flushCount]() { ++flushCount; };
  hooks.store = [&storedSsid, &storedPass](const char *ssid, const char *password) {
    storedSsid = ssid;
    storedPass = password;
    return true;
  };
  hooks.recalibrate = [&orientCount]() { ++orientCount; };
  hooks.reboot = [&rebooted]() { rebooted = true; };
  serial::ProvisionConsole console(hooks);
  uint32_t now = 1000;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "cfeed") {
      std::string hex;
      args >> hex;
      for (const char c : unhex(hex))
        if (console.feed(c, now) == serial::ProvisionConsole::Event::EnterBinary)
          std::cout << "enter-binary\n";
    } else if (cmd == "ctick") {
      uint32_t ms = 0;
      args >> ms;
      now += ms;
      console.tick(now);
      std::cout << "ctick\n";
    } else if (cmd == "cshow") {
      std::cout << "store=" << (storedSsid.empty() ? "-" : storedSsid) << '|'
                << (storedPass.empty() ? "-" : storedPass) << " orient=" << orientCount
                << " flush=" << flushCount << " reboot=" << (rebooted ? 1 : 0) << '\n';
    }
  }
}
} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  (void)argc;
  const std::string mode = argc > 1 ? argv[1] : "mux";
  if (mode == "console") runConsoleScript();
  else if (mode == "mux") runMuxScript();
  else {
    // "dispatcher": one buffered pass with the firmware dispatcher shape —
    // EMOTE\n inside the input enters the session and the remaining bytes
    // route straight to the mux (finding 3 functional pin).
    const std::string input((std::istreambuf_iterator<char>(std::cin)),
                            std::istreambuf_iterator<char>());
    std::vector<uint8_t> buffer(serial::MAX_PNG);
    Rig rig(buffer.data(), buffer.size());
    const std::vector<uint8_t> bytes(input.begin(), input.end());
    for (size_t i = 0; i < bytes.size(); ++i) {
      if (rig.console.feed(static_cast<char>(bytes[i]), rig.now) ==
          serial::ProvisionConsole::Event::EnterBinary) {
        std::cout << "entered\n";
        rig.mux.enter(rig.now);
        pollFrames(rig, bytes.data() + i + 1, bytes.size() - i - 1);
        break;
      }
    }
    return 0;
  }
  return 0;
}
