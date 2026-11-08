#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

inline uint32_t senderSaturatingIncrement(uint32_t value) {
  return value == UINT32_MAX ? value : value + 1;
}

// IPs use network octet order: 192.168.1.2 is 0xc0a80102.
class SenderStats {
 public:
  static constexpr size_t CAPACITY = 8;
  struct Entry { uint32_t ip = 0, requests = 0; };

  void record(uint32_t ip) {
    for (size_t i = 0; i < size_; ++i) {
      if (entries_[i].ip == ip) {
        entries_[i].requests = senderSaturatingIncrement(entries_[i].requests);
        return;
      }
    }
    if (size_ == CAPACITY) { overflow_ = true; return; }
    entries_[size_].ip = ip;
    entries_[size_++].requests = 1;
  }
  size_t size() const { return size_; }
  bool overflow() const { return overflow_; }
  const Entry &at(size_t index) const { return entries_[index]; }

 private:
  std::array<Entry, CAPACITY> entries_{};
  size_t size_ = 0;
  bool overflow_ = false;
};

// Recognize only an exact, complete supported first line. No allocation and no
// dependence on later header/body validation; one attempt per TCP connection.
class SenderConnection {
 public:
  void reset() { position_ = 0; done_ = false; }
  void feed(char byte, uint32_t ip, SenderStats &stats) {
    if (done_) return;
    static const char line[] = "POST /emote HTTP/1.1\r\n";
    const bool version = position_ == sizeof(line) - 4 && byte == '0';
    if (byte != line[position_] && !version) { done_ = true; return; }
    if (++position_ == sizeof(line) - 1) {
      stats.record(ip);
      done_ = true;
    }
  }

 private:
  size_t position_ = 0;
  bool done_ = false;
};
