#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "rgb_order.h"
#include "serial_protocol.h"

// Serial-port multiplexing for T2. The USB serial port carries two traffic
// classes: the existing line console (WIFI/ORIENT provisioning) and binary
// frames. They must never mix — a frame's bytes or a corrupted tail must not
// be parsed as WIFI/ORIENT/password, and console lines starting with the
// frame magic must keep working as credentials. The coherent resolution is
// an explicit binary session: provisioning is the default mode and owns
// every byte until the exact line command EMOTE enters the binary session;
// the exact idle-only sequence EMTE\n leaves it.
namespace serial {

// Binary-session timeouts (T2). A full 32 KiB frame needs ~2.9 s at
// 115200 8N1, so the total frame window is 10 s (3x margin). USB CDC
// inter-byte gaps are not baud-bounded, but a live stop-and-wait sender
// never stalls mid-frame for a full second, so 1 s of silence discards an
// open frame. The HTTP REQUEST_TIMEOUT_MS (2.5 s) is a distinct policy and
// remains unchanged.
constexpr uint32_t FRAME_INACTIVITY_MS = 1000;
constexpr uint32_t FRAME_TOTAL_MS = 10000;

constexpr const char *ENTRY_COMMAND = "EMOTE";
constexpr const char *ENTRY_ACK = "EMOTE_READY";
constexpr const char *EXIT_ACK = "EMOTE_EXIT";

// Byte values of "EMTE\n" (5 bytes) and "EMOTE\n" (6 bytes) for the
// idle-scan window.
constexpr uint64_t EXIT_SEQUENCE = 0x454D54450AULL;
constexpr uint64_t ENTRY_SEQUENCE = 0x454D4F54450AULL;
// Mask for the six-byte entry comparison (the exit sequence is five bytes
// and already fits below).
constexpr uint64_t SEQ_MASK = 0xFFFFFFFFFFFFULL;

// The console line protocol, extracted verbatim from pollProvisioning so the
// exact previous behavior (strings, limits, stage machine, 60 s stage
// timeout) runs host-testably. Secrets are never echoed; output, storage and
// side effects arrive through the injected hooks.
class ProvisionConsole {
 public:
  struct Hooks {
    std::function<void(const char *)> print;               // Serial.println
    std::function<void()> flush;                           // Serial.flush
    std::function<bool(const char *, const char *)> store; // ssid, password → saved
    std::function<void()> recalibrate;                     // ORIENT action
    std::function<void()> reboot;                          // after WIFI_SAVED / RGB_SAVED
    // RGB <order> (S7): persist the panel channel order, then reboot.
    std::function<bool(rgb_order::Order)> setOrder;
  };

  enum class Event : uint8_t { None, EnterBinary };

  explicit ProvisionConsole(const Hooks &hooks) : hooks_(hooks) {}

  // One console byte. '\r' is skipped exactly as before; '\n' terminates a
  // line. EMOTE is only a command at the top level (stage 0): an SSID or
  // password may be "EMOTE" or start with any magic prefix.
  Event feed(char c, uint32_t now) {
    if (c == '\r') return Event::None;
    if (c != '\n') {
      if (line_.size() < 64) line_ += c;
      else overflow_ = true;
      return Event::None;
    }
    Event result = Event::None;
    if (overflow_) {
      hooks_.print("WIFI_ERROR: line too long");
      stage_ = 0;
      pendingSsid_.clear();
    } else if (stage_ == 0 && line_ == "ORIENT") {
      hooks_.recalibrate();
    } else if (stage_ == 0 && line_ == "WIFI") {
      stage_ = 1;
      started_ = now;
      hooks_.print("WIFI_SSID");
    } else if (stage_ == 0 && line_ == ENTRY_COMMAND) {
      hooks_.print(ENTRY_ACK);
      result = Event::EnterBinary;
    } else if (stage_ == 0 && line_ == "RGB") {
      // `RGB` with no order name: the same rejection as a name that does
      // not parse, so the console never waits for a second line here.
      hooks_.print("RGB_ERROR: unknown channel order");
    } else if (stage_ == 0 && line_.rfind("RGB ", 0) == 0) {
      // S7: RGB <order> — single-line command that persists the panel
      // channel order and reboots, mirroring the WIFI flow's report
      // (message, flush, restart). The order name parses case-
      // insensitively; anything else, and a store that cannot be
      // verified, is rejected with the console's usual error shape and
      // changes no stored state.
      rgb_order::Order order;
      if (!rgb_order::parseOrder(line_.substr(4), order)) {
        hooks_.print("RGB_ERROR: unknown channel order");
      } else if (!hooks_.setOrder || !hooks_.setOrder(order)) {
        hooks_.print("RGB_ERROR: storage failed");
      } else {
        hooks_.print("RGB_SAVED");
        hooks_.flush();
        hooks_.reboot();
      }
    } else if (stage_ == 1) {
      if (line_.empty() || line_.size() > 32) {
        stage_ = 0;
        hooks_.print("WIFI_ERROR: invalid SSID length");
      } else {
        pendingSsid_ = line_;
        stage_ = 2;
        hooks_.print("WIFI_PASSWORD");
      }
    } else if (stage_ == 2) {
      if (line_.size() > 63 || (line_.size() != 0 && line_.size() < 8)) {
        hooks_.print("WIFI_ERROR: invalid password length");
        stage_ = 0;
        pendingSsid_.clear();
      } else {
        const bool saved = hooks_.store(pendingSsid_.c_str(), line_.c_str());
        line_.clear();
        pendingSsid_.clear();
        stage_ = 0;
        hooks_.print(saved ? "WIFI_SAVED" : "WIFI_ERROR: storage failed");
        hooks_.flush();
        if (saved) hooks_.reboot();
        overflow_ = false;
        return result;
      }
    }
    line_.clear();
    overflow_ = false;
    return result;
  }

  // Stage timeout, evaluated once per poll exactly like the old loop guard.
  void tick(uint32_t now) {
    if (stage_ && now - started_ > 60000) {
      stage_ = 0;
      line_.clear();
      pendingSsid_.clear();
      overflow_ = false;
    }
  }

 private:
  Hooks hooks_;
  std::string line_, pendingSsid_;
  int stage_ = 0;
  bool overflow_ = false;
  uint32_t started_ = 0;
};

// Routes port bytes between the console and the frame parser.
class Mux {
 public:
  enum class Mode : uint8_t { Provision, Binary };
  enum class Event : uint8_t {
    None,        // binary byte absorbed by the parser
    Console,     // provision byte handed to the console
    RepeatEntry, // "EMOTE\n" while already binary: re-ack a lost EMOTE_READY
    Exit,        // "EMTE\n" while idle: back to provision mode
    Complete,    // a frame finished validating
    Error,       // a frame was rejected
  };
  struct Routed {
    Event event;
    char consoleByte;
  };

  explicit Mux(Parser &parser) : parser_(parser) {}

  Mode mode() const { return mode_; }

  // Console command: start the binary session with a clean parser.
  void enter(uint32_t now) {
    mode_ = Mode::Binary;
    window_ = 0;
    parser_.reset();
    lastByte_ = frameStart_ = now;
  }

  // One byte from the port. Provision mode hands it to the console; binary
  // mode routes it to the parser and never to the console, whatever it is.
  Routed feed(uint8_t byte, uint32_t now) {
    lastByte_ = now;
    if (mode_ == Mode::Provision) return {Event::Console, static_cast<char>(byte)};
    const bool scanning = parser_.status() == Parser::Status::Idle;
    parser_.feed(&byte, 1);
    if (scanning && parser_.status() == Parser::Status::Active) frameStart_ = now;
    if (parser_.status() == Parser::Status::Complete) {
      window_ = 0;
      return {Event::Complete, 0};
    }
    if (parser_.status() == Parser::Status::Error) {
      window_ = 0;
      return {Event::Error, 0};
    }
    if (parser_.status() == Parser::Status::Idle) {
      // Exit/entry sequences are recognized only while no frame is open, so
      // the bytes of a frame — including the PNG payload — can never trigger
      // them. The window resets on every frame boundary.
      window_ = (window_ << 8) | byte;
      if ((window_ & 0xFFFFFFFFFFULL) == EXIT_SEQUENCE) {
        mode_ = Mode::Provision;
        return {Event::Exit, 0};
      }
      if ((window_ & SEQ_MASK) == ENTRY_SEQUENCE) {
        parser_.reset();
        window_ = 0;
        return {Event::RepeatEntry, 0};
      }
    }
    return {Event::None, 0};
  }

  // Timeout policy in binary mode: an open frame that stalls past the
  // inactivity window or the total window is discarded and the parser
  // resyncs. The session stays binary — a dead frame's tail is skipped, not
  // returned to the console, and the scan window is cleared so stale bytes
  // cannot later complete an entry/exit sequence. A partial magic match in
  // the scanner is harmless and simply continues with the next bytes.
  void update(uint32_t now) {
    if (mode_ != Mode::Binary) return;
    if (parser_.status() == Parser::Status::Active &&
        (uint32_t(now - lastByte_) >= FRAME_INACTIVITY_MS ||
         uint32_t(now - frameStart_) >= FRAME_TOTAL_MS)) {
      parser_.reset();
      window_ = 0;
    }
  }

 private:
  Mode mode_ = Mode::Provision;
  Parser &parser_;
  uint64_t window_ = 0;
  uint32_t lastByte_ = 0;
  uint32_t frameStart_ = 0;
};
} // namespace serial
