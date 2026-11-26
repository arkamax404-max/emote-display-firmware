#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "project_name.h"
#include "receiver_protocol.h"
#include "session_owner.h"

// Bounded binary serial ingress protocol (docs/serial-transport.md). One
// framed operation per frame — image, claim, heartbeat, release or UTC clock — with the
// same session identity, project-name decoder, PNG envelope check and clock
// metadata parity as the HTTP parser, plus a compact correlated reply. The
// parser is allocation-free: the caller supplies the payload buffer (T2
// provides a dedicated static serial buffer, never the shared HTTP PNG
// buffer), and there is no heap frame queue and no base64. A rejected frame
// never surfaces through the output and never mutates accepted display state.
namespace serial {
constexpr size_t MAX_PNG = receiver::MAX_PNG;
// magic(4) + version(1) + op(1) + flags(1) + reserved(1) + requestId(2)
// + sessionIdLen(2) + nameLen(2) + payloadLen(4) + metadata + payload + crc(4).
constexpr size_t HEADER_SIZE = 18;
constexpr size_t MAX_FRAME =
    HEADER_SIZE + session::MAX_ID_LENGTH + project_name::MAX_ENCODED + MAX_PNG + 4;
constexpr size_t REPLY_SIZE = 12;

constexpr uint8_t MAGIC[4] = {'E', 'M', 'T', 'S'};
constexpr uint8_t REPLY_MAGIC[4] = {'E', 'M', 'T', 'R'};

enum class Op : uint8_t { Image = 0, Claim = 1, Heartbeat = 2, Release = 3, ClockUtc = 4 };
constexpr uint32_t MIN_CLOCK_UTC = 1577836800u;
constexpr uint32_t MAX_CLOCK_UTC = 2147483647u;

// Parser error codes double as reply result codes; Busy is the ownership
// conflict (HTTP 409 parity), and Time also covers runtime clock-set failure.
enum class Code : uint8_t {
  Ok = 0, Crc = 1, Version = 2, Op = 3, Flags = 4, Reserved = 5,
  Length = 6, Oversize = 7, Session = 8, Name = 9, Png = 10, Busy = 11, Time = 12,
};

inline const char *opName(Op op) {
  switch (op) {
    case Op::Claim: return "claim";
    case Op::Heartbeat: return "heartbeat";
    case Op::Release: return "release";
    case Op::ClockUtc: return "clock-utc";
    default: return "image";
  }
}

inline const char *codeName(Code code) {
  switch (code) {
    case Code::Ok: return "ok";
    case Code::Crc: return "crc";
    case Code::Version: return "version";
    case Code::Op: return "op";
    case Code::Flags: return "flags";
    case Code::Reserved: return "reserved";
    case Code::Length: return "length";
    case Code::Oversize: return "oversize";
    case Code::Session: return "session";
    case Code::Name: return "name";
    case Code::Png: return "png";
    case Code::Time: return "time";
    default: return "busy";
  }
}

// Same reflected CRC-32 as receiver::crc32, split into incremental steps so a
// streamed frame can be covered without buffering it. Parity is pinned by
// test_incremental_crc_matches_receiver_crc32.
inline uint32_t crcBegin() { return 0xffffffffu; }
inline uint32_t crcFeed(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
  return crc;
}
inline uint32_t crcEnd(uint32_t crc) { return crc ^ 0xffffffffu; }

inline void putLe16(uint8_t *p, uint16_t value) { p[0] = value & 0xff; p[1] = value >> 8; }
inline void putLe32(uint8_t *p, uint32_t value) {
  p[0] = value & 0xff; p[1] = (value >> 8) & 0xff;
  p[2] = (value >> 16) & 0xff; p[3] = value >> 24;
}
inline uint16_t le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// The only data echoed back: magic, version, result code and the sender's
// request id. Session ids and names never appear in a reply, so a source
// console may log the wire without disclosing identity.
inline size_t encodeReply(uint8_t *out, uint16_t requestId, Code result) {
  for (int i = 0; i < 4; ++i) out[i] = REPLY_MAGIC[i];
  out[4] = 1;
  out[5] = static_cast<uint8_t>(result);
  putLe16(out + 6, requestId);
  putLe32(out + 8, receiver::crc32(out, 8));
  return REPLY_SIZE;
}

struct Frame {
  Op op = Op::Image;
  uint16_t requestId = 0;
  bool hasSession = false;
  bool clockState = false;
  char sessionId[session::MAX_ID_LENGTH + 1] = {};
  // Decoded title; set only for image frames carrying a valid name. Lifecycle
  // controls ignore the name field, exactly like their HTTP counterparts.
  bool hasName = false;
  char projectName[project_name::STORE] = {};
  size_t payloadLength = 0;
  uint32_t utcSeconds = 0;  // Set only for a validated ClockUtc frame.
};

// Streaming frame parser. Status is Idle while scanning for magic, Active
// while a frame is being collected, Complete after a fully validated frame,
// Error after a rejected one. The Frame output is stable only between a
// Complete and the next feed()/reset(); after an Error the exposed frame is
// scrubbed. feed() after a terminal result automatically rescans for the next
// magic, so a corrupted frame's tail can never leak into later frames.
class Parser {
 public:
  enum class Status : uint8_t { Idle, Active, Complete, Error };

  // payloadBuffer must outlive the parser and hold at least `capacity` bytes.
  Parser(uint8_t *payloadBuffer, size_t capacity)
      : payload_(payloadBuffer), capacity_(capacity) {}

  // Feed bytes; returns the number of bytes consumed, stopping right after a
  // terminal event (Complete or Error). A suffix after that point stays
  // unconsumed and is returned by the next call; it is never dropped. The
  // resulting status() tells the caller which terminal event, if any, fired.
  // Truncation alone never completes a frame.
  size_t feed(const uint8_t *data, size_t length) {
    size_t consumed = 0;
    while (consumed < length) {
      if (status_ == Status::Complete || status_ == Status::Error) restart();
      const Event event = feedByte(data[consumed++]);
      if (event != Event::NeedMore) break;
    }
    return consumed;
  }

  // Resync and clear diagnostics; T2 calls this on inactivity timeout. The
  // timeout itself is enforced by the integration, not here.
  void reset() {
    restart();
    skipped_ = 0;
  }

  Status status() const { return status_; }
  Code error() const { return errorCode_; }
  const Frame &frame() const { return frame_; }
  size_t skipped() const { return skipped_; }
  // Request id of the frame in flight, kept across a rejection so the runtime
  // can answer a NACK that the sender can still correlate.
  uint16_t lastRequestId() const { return lastRequestId_; }

 private:
  enum class Event : uint8_t { NeedMore, Complete, Error };

  void restart() {
    status_ = Status::Idle;
    errorCode_ = Code::Ok;
    frame_ = Frame{};
    lastRequestId_ = 0;  // cleared when starting the next frame
    match_ = 0;
    position_ = 0;
    payloadEnd_ = MAX_FRAME + 4;  // sentinel: unknown until the header lands
    crc_ = crcBegin();
  }

  Event feedByte(uint8_t byte) {
    if (status_ == Status::Idle) {
      if (byte == MAGIC[match_]) {
        if (++match_ == 4) beginFrame();
        return Event::NeedMore;
      }
      skipped_ += match_;
      if (byte == MAGIC[0]) match_ = 1;
      else { ++skipped_; match_ = 0; }
      return Event::NeedMore;
    }
    // Active: every byte before the trailing CRC is covered by the checksum.
    if (position_ < payloadEnd_) crc_ = crcFeed(crc_, byte);
    if (position_ == HEADER_SIZE - 4) {
      const Code code = validateHeader();
      if (code != Code::Ok) return fail(code);
    }
    if (position_ < HEADER_SIZE - 4) {
      header_[position_] = byte;
    } else if (position_ < nameStart_) {
      sessionBuf_[position_ - (HEADER_SIZE - 4)] = byte;
    } else if (position_ < payloadStart_) {
      nameBuf_[position_ - nameStart_] = byte;
    } else if (position_ < payloadEnd_) {
      payload_[position_ - payloadStart_] = byte;
    } else {
      crcBytes_[position_ - payloadEnd_] = byte;
    }
    ++position_;
    if (position_ == payloadEnd_ + 4) return finishFrame();
    return Event::NeedMore;
  }

  void beginFrame() {
    restart();
    status_ = Status::Active;
    // The scanner consumed the magic without reaching the CRC path, so the
    // four magic bytes are folded in here: the checksum covers the whole frame
    // from the first magic byte through the last payload byte.
    for (int i = 0; i < 4; ++i) crc_ = crcFeed(crc_, MAGIC[i]);
  }

  // Structural validation the moment the fixed header is complete: lengths
  // are checked before any metadata or payload is buffered, so an oversize
  // frame is rejected without waiting for bytes that may never arrive.
  Code validateHeader() {
    // Latch the correlation id as soon as the fixed header bytes are all
    // present, BEFORE any validation, so even a malformed header NACKs with
    // the frame's own request id (readback fix 1).
    lastRequestId_ = le16(header_ + 4);
    if (header_[0] != 1) return Code::Version;
    const uint8_t op = header_[1];
    if (op > static_cast<uint8_t>(Op::ClockUtc)) return Code::Op;
    const bool clockUtc = op == static_cast<uint8_t>(Op::ClockUtc);
    const uint8_t flags = header_[2];
    if ((flags & 0xF8) || (clockUtc && flags != 0x01)) return Code::Flags;
    if (header_[3]) return Code::Reserved;
    const bool hasSession = flags & 0x01;
    const bool hasName = flags & 0x02;
    const bool image = op == 0;
    if (!image && !hasSession) return Code::Session;
    const uint16_t sessionLen = le16(header_ + 6);
    const uint16_t nameLen = le16(header_ + 8);
    const uint32_t payloadLen = le32(header_ + 10);
    if (hasSession ? (sessionLen == 0 || sessionLen > session::MAX_ID_LENGTH)
                   : sessionLen != 0) return Code::Length;
    if (hasName ? (nameLen == 0 || nameLen > project_name::MAX_ENCODED)
                : nameLen != 0) return Code::Length;
    if (image) {
      if (payloadLen == 0) return Code::Length;
      if (payloadLen > MAX_PNG || payloadLen > capacity_) return Code::Oversize;
    } else if (clockUtc) {
      if (payloadLen != sizeof(uint32_t)) return Code::Length;
      if (payloadLen > capacity_) return Code::Oversize;
    } else if (payloadLen != 0) return Code::Length;
    frame_.op = static_cast<Op>(op);
    frame_.requestId = lastRequestId_;
    frame_.hasSession = hasSession;
    frame_.clockState = flags & 0x04;
    frame_.hasName = image && hasName;
    nameStart_ = (HEADER_SIZE - 4) + sessionLen;
    payloadStart_ = nameStart_ + nameLen;
    payloadEnd_ = payloadStart_ + payloadLen;
    return Code::Ok;
  }

  // Semantic validation after the checksum proves integrity: session identity
  // charset, the exact project-name matrix, and the same PNG envelope the
  // HTTP path enforces (full pixel decode stays with the presentation stage).
  Event finishFrame() {
    if (le32(crcBytes_) != crcEnd(crc_)) return fail(Code::Crc);
    if (frame_.hasSession &&
        !session::validSessionId(sessionBuf_, nameStart_ - (HEADER_SIZE - 4)))
      return fail(Code::Session);
    if (frame_.hasName &&
        project_name::decode(nameBuf_, payloadStart_ - nameStart_,
                             frame_.projectName) != project_name::Result::Valid)
      return fail(Code::Name);
    const size_t payloadLen = payloadEnd_ - payloadStart_;
    if (frame_.op == Op::Image && !receiver::validPngEnvelope(payload_, payloadLen))
      return fail(Code::Png);
    if (frame_.op == Op::ClockUtc) {
      const uint32_t epoch = le32(payload_);
      if (epoch < MIN_CLOCK_UTC || epoch > MAX_CLOCK_UTC) return fail(Code::Time);
      frame_.utcSeconds = epoch;
    }
    if (frame_.hasSession) {
      const size_t sessionLen = nameStart_ - (HEADER_SIZE - 4);
      std::memcpy(frame_.sessionId, sessionBuf_, sessionLen);
      frame_.sessionId[sessionLen] = '\0';
    }
    frame_.payloadLength = payloadLen;
    status_ = Status::Complete;
    return Event::Complete;
  }

  Event fail(Code code) {
    frame_ = Frame{};  // scrub: no partial identity or metadata survives a rejection
    errorCode_ = code;
    status_ = Status::Error;
    return Event::Error;
  }

  Status status_ = Status::Idle;
  Code errorCode_ = Code::Ok;
  Frame frame_{};
  size_t skipped_ = 0;
  size_t match_ = 0;
  size_t position_ = 0;
  size_t nameStart_ = 0;
  size_t payloadStart_ = 0;
  size_t payloadEnd_ = MAX_FRAME + 4;
  uint16_t lastRequestId_ = 0;
  uint32_t crc_ = crcBegin();
  uint8_t header_[HEADER_SIZE - 4] = {};
  uint8_t crcBytes_[4] = {};
  char sessionBuf_[session::MAX_ID_LENGTH] = {};
  char nameBuf_[project_name::MAX_ENCODED] = {};
  uint8_t *payload_;
  size_t capacity_;
};
} // namespace serial
