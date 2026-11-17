#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "session_owner.h"

namespace receiver {
constexpr size_t MAX_HEADERS = 4096;
constexpr size_t MAX_PNG = 32768;
struct Request {
  enum class Kind : uint8_t { Image, Claim, Heartbeat, Release };
  size_t length = 0;
  bool health = false;
  bool clockState = false;
  Kind kind = Kind::Image;
  bool hasSession = false;
  // Bounded storage for the single validated session identity; never
  // disclosed by /health.
  char sessionId[session::MAX_ID_LENGTH + 1] = {};
};

inline const char *kindName(const Request &request) {
  if (request.health) return "health";
  switch (request.kind) {
    case Request::Kind::Claim: return "claim";
    case Request::Kind::Heartbeat: return "heartbeat";
    case Request::Kind::Release: return "release";
    default: return "image";
  }
}

inline std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return "";
  return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}
inline std::string lower(std::string value) {
  for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
// Deliberately small HTTP subset: no chunked uploads, keep-alive or pipelining.
inline int parseHeaders(const std::string &headers, Request &request) {
  request = Request{};
  if (headers.size() > MAX_HEADERS) return 431;
  if (headers.size() < 4 || headers.compare(headers.size() - 4, 4, "\r\n\r\n")) return 400;
  const size_t first = headers.find("\r\n");
  const std::string line = headers.substr(0, first);
  request.health = line == "GET /health HTTP/1.1" || line == "GET /health HTTP/1.0";
  if (!request.health) {
    if (line == "POST /emote HTTP/1.1" || line == "POST /emote HTTP/1.0") {
      request.kind = Request::Kind::Image;
    } else if (line == "POST /emote/claim HTTP/1.1" || line == "POST /emote/claim HTTP/1.0") {
      request.kind = Request::Kind::Claim;
    } else if (line == "POST /emote/heartbeat HTTP/1.1" || line == "POST /emote/heartbeat HTTP/1.0") {
      request.kind = Request::Kind::Heartbeat;
    } else if (line == "POST /emote/release HTTP/1.1" || line == "POST /emote/release HTTP/1.0") {
      request.kind = Request::Kind::Release;
    } else return 404;
  }
  bool hasLength = false, hasType = false, hasVersion = false;
  size_t start = first + 2;
  while (start < headers.size() - 2) {
    const size_t end = headers.find("\r\n", start);
    const size_t colon = headers.find(':', start);
    if (end == std::string::npos || colon == std::string::npos || colon >= end || colon == start) return 400;
    const std::string key = lower(headers.substr(start, colon - start));
    const std::string value = trim(headers.substr(colon + 1, end - colon - 1));
    if (key == "transfer-encoding") return 400;
    if (key == "expect") return 417;
    if (key == "content-length") {
      if (hasLength || value.empty()) return 400;
      hasLength = true;
      for (const char c : value) {
        if (c < '0' || c > '9') return 400;
        request.length = request.length * 10 + (c - '0');
        if (request.length > MAX_PNG) return 413;
      }
    } else if (key == "content-type") {
      if (hasType || lower(value) != "image/png") return 415;
      hasType = true;
    } else if (key == "x-emote-version") {
      if (hasVersion || value != "1") return 400;
      hasVersion = true;
    } else if (key == "x-emote-session-id") {
      // Exactly one session identity per request. Duplicates and malformed
      // values reject the request before any ownership effect.
      if (request.hasSession) return 400;
      if (!session::validSessionId(value.data(), value.size())) return 400;
      std::memcpy(request.sessionId, value.data(), value.size());
      request.sessionId[value.size()] = '\0';
      request.hasSession = true;
    } else if (key == "x-emote-state") {
      // Sender metadata for the clock skin. Missing or unknown values keep
      // the default avatar rendering; no value is rejected here.
      request.clockState = lower(value) == "clock";
    }
    start = end + 2;
  }
  if (request.health) return request.length == 0 ? 200 : 400;
  if (!hasLength) return 411;
  if (request.kind == Request::Kind::Image) {
    if (!request.length) return 400;
    if (!hasType) return 415;
    if (!hasVersion) return 400;
    return 200;
  }
  // Bodyless control routes use the fixed protocol: version header, no
  // payload, and exactly one valid session identity.
  if (!hasVersion) return 400;
  if (request.length) return 400;
  if (!request.hasSession) return 400;
  return 200;
}
inline uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return crc ^ 0xffffffff;
}
// Accept the sender's exact format: 64x64 RGBA8, non-interlaced, IHDR/IDAT/IEND.
// PNGdec subsequently validates the compressed image before the panel is changed.
inline bool validPngEnvelope(const uint8_t *data, size_t size) {
  const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (size < 57 || size > MAX_PNG || std::memcmp(data, signature, 8)) return false;
  bool header = false, pixels = false;
  for (size_t offset = 8; offset < size;) {
    if (size - offset < 12) return false;
    const uint32_t length = be32(data + offset);
    if (length > size - offset - 12) return false;
    const uint8_t *type = data + offset + 4, *body = data + offset + 8;
    if (crc32(type, length + 4) != be32(body + length)) return false;
    if (!std::memcmp(type, "IHDR", 4)) {
      if (header || offset != 8 || length != 13 || be32(body) != 64 || be32(body + 4) != 64 ||
          body[8] != 8 || body[9] != 6 || body[10] || body[11] || body[12]) return false;
      header = true;
    } else if (!std::memcmp(type, "IDAT", 4)) {
      if (!header) return false;
      pixels = true;
    } else if (!std::memcmp(type, "IEND", 4)) {
      return header && pixels && length == 0 && offset + 12 == size;
    } else return false;
    offset += length + 12;
  }
  return false;
}
} // namespace receiver
