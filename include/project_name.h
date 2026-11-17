#pragma once
#include <cstddef>

// Session-title validation mirroring the desktop receiver matrix
// (gentle-emote-desktop src/receiver.rs): percent-decode exactly once with
// '+' literal and '%' requiring two hex digits, then the bound, trim and
// character rules. Every rule violation is the same distinct Result; the
// caller answers 400 "invalid project name\n" and changes no state.
namespace project_name {
constexpr size_t MAX_ENCODED = 1152; // encoded ASCII bytes in the header value
constexpr size_t MAX_DECODED = 384;  // decoded UTF-8 bytes (128 3-byte scalars)
constexpr int MAX_UNITS = 128;       // UTF-16 code units
constexpr size_t STORE = MAX_DECODED + 1;

enum class Result { Valid, Invalid };

inline bool hex(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline unsigned hexValue(unsigned char c) {
  if (c <= '9') return c - '0';
  if (c >= 'a') return c - 'a' + 10;
  return c - 'A' + 10;
}
// Unicode White_Space scalars reachable after the control-character rule.
inline bool space(uint32_t cp) {
  return (cp >= 9 && cp <= 13) || cp == 0x20 || cp == 0x85 || cp == 0xa0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

// Decode into out (STORE bytes, NUL-terminated) and apply the whole matrix.
// One decode pass: '+' stays literal, '%' needs two hex digits, escapes are
// never decoded twice. Non-ASCII header bytes are refused for direct callers
// (the byte-level HTTP filter already rejects them on the device).
inline Result decode(const char *data, size_t length, char *out) {
  if (length > MAX_ENCODED) return Result::Invalid;
  size_t written = 0;
  for (size_t i = 0; i < length;) {
    unsigned byte;
    if (data[i] == '%') {
      if (i + 2 >= length || !hex(static_cast<unsigned char>(data[i + 1])) ||
          !hex(static_cast<unsigned char>(data[i + 2]))) return Result::Invalid;
      byte = (hexValue(static_cast<unsigned char>(data[i + 1])) << 4) |
             hexValue(static_cast<unsigned char>(data[i + 2]));
      i += 3;
    } else {
      const unsigned char c = static_cast<unsigned char>(data[i]);
      if (c > 126) return Result::Invalid;
      byte = c;
      ++i;
    }
    if (written >= MAX_DECODED) return Result::Invalid;
    out[written++] = static_cast<char>(byte);
  }
  // Decoded bytes must be valid UTF-8; remember scalar lengths while checking.
  size_t scalarLen[384] = {};
  for (size_t i = 0; i < written;) {
    const unsigned char lead = static_cast<unsigned char>(out[i]);
    size_t len;
    uint32_t cp;
    if (lead < 0x80) { len = 1; cp = lead; }
    else if ((lead & 0xE0) == 0xC0) { len = 2; cp = lead & 0x1Fu; }
    else if ((lead & 0xF0) == 0xE0) { len = 3; cp = lead & 0x0Fu; }
    else if ((lead & 0xF8) == 0xF0) { len = 4; cp = lead & 0x07u; }
    else return Result::Invalid;
    if (i + len > written) return Result::Invalid;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cont = static_cast<unsigned char>(out[i + k]);
      if ((cont & 0xC0) != 0x80) return Result::Invalid;
      cp = (cp << 6) | (cont & 0x3Fu);
    }
    if ((len == 2 && cp < 0x80) ||
        (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return Result::Invalid;
    scalarLen[i] = len;
    i += len;
  }
  // Blank-after-trim on Unicode whitespace; boundaries fall between scalars.
  size_t start = 0, end = written;
  while (start < end) {
    uint32_t cp = static_cast<unsigned char>(out[start]);
    for (size_t k = 1; k < scalarLen[start]; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(out[start + k]) & 0x3Fu);
    if (!space(cp)) break;
    start += scalarLen[start];
  }
  while (end > start) {
    size_t lead = end - 1;
    while (lead > start && (out[lead] & 0xC0) == 0x80) --lead;
    uint32_t cp = static_cast<unsigned char>(out[lead]);
    for (size_t k = 1; k < scalarLen[lead]; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(out[lead + k]) & 0x3Fu);
    if (!space(cp)) break;
    end = lead;
  }
  if (start == end) return Result::Invalid;
  // Exactly "." or ".."; control characters, DEL and path separators; and the
  // 128 UTF-16 unit bound, all on the trimmed value.
  if (end - start == 1 && out[start] == '.') return Result::Invalid;
  if (end - start == 2 && out[start] == '.' && out[start + 1] == '.') return Result::Invalid;
  int units = 0;
  for (size_t i = start; i < end;) {
    uint32_t cp = static_cast<unsigned char>(out[i]);
    for (size_t k = 1; k < scalarLen[i]; ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(out[i + k]) & 0x3Fu);
    if (cp <= 0x1F || cp == 0x7F || cp == '/' || cp == '\\') return Result::Invalid;
    units += cp >= 0x10000 ? 2 : 1;
    if (units > MAX_UNITS) return Result::Invalid;
    i += scalarLen[i];
  }
  for (size_t i = 0; i < end - start; ++i) out[i] = out[start + i];
  out[end - start] = '\0';
  return Result::Valid;
}
} // namespace project_name
