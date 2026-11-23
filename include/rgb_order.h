#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// S7/T10 panel channel order: pure, host-testable logic in the header-only
// style of include/portal_policy.h — no Arduino/WiFi symbols, every value
// derived from the base wiring constant below.
//
// The firmware's base wiring drives the six HUB75 data pins in Protomatter
// constructor order R1 G1 B1 R2 G2 B2 (the official Adafruit MatrixPortal
// S3 pinout). Some panels wire those inputs to different LED colours, so
// the firmware's red can come out green. The order NAMES the colour
// sequence the panel lights on its three data inputs of each row, in the
// base wiring order — `RGB` is therefore the untouched default — and
// applyOrder() hands Protomatter, for each of its six colour slots, the
// base pin wired to the panel input that lights that colour. All six
// permutations are named and distinct; a wrong pick just shows the wrong
// colours again, never harms the panel, so the choice can be cycled until
// the colours come out right.
namespace rgb_order {

// Base wiring: R1, G1, B1, R2, G2, B2 (GPIO).
constexpr uint8_t BASE_PINS[6] = {42, 41, 40, 38, 39, 37};

enum class Order : uint8_t { RGB, RBG, GRB, GBR, BRG, BGR };
constexpr uint8_t ORDER_COUNT = 6;

inline const char *orderName(Order order) {
  switch (order) {
    case Order::RBG: return "RBG";
    case Order::GRB: return "GRB";
    case Order::GBR: return "GBR";
    case Order::BRG: return "BRG";
    case Order::BGR: return "BGR";
    default: return "RGB";
  }
}

// The small code persisted in the `gentle` namespace (key `rgborder`): the
// enum value itself, 0-5. `RGB` is 0, so a missing key already decodes to
// the default.
inline uint8_t orderCode(Order order) { return static_cast<uint8_t>(order); }

inline bool orderFromCode(uint8_t code, Order &out) {
  if (code >= ORDER_COUNT) return false;
  out = static_cast<Order>(code);
  return true;
}

// Exact, case-insensitive match of the whole string against a name; letters
// outside A-Z (in any case) make the name unknown, and so do empty values,
// surrounding spaces and anything longer than three letters.
inline bool parseOrder(const std::string &text, Order &out) {
  if (text.size() != 3) return false;
  for (uint8_t i = 0; i < ORDER_COUNT; ++i) {
    const Order candidate = static_cast<Order>(i);
    const char *name = orderName(candidate);
    bool match = true;
    for (size_t j = 0; j < 3 && match; ++j) {
      char offered = text[j];
      if (offered >= 'a' && offered <= 'z')
        offered = static_cast<char>(offered - 'a' + 'A');
      if (offered != name[j]) match = false;
    }
    if (match) {
      out = candidate;
      return true;
    }
  }
  return false;
}

// Fills the six Protomatter constructor pins from BASE_PINS for the order:
// slot s (0-5, colours R G B | R G B) takes BASE_PINS[3*(s/3) + p] where p
// is the position of that slot's colour inside the order name.
inline void applyOrder(Order order, uint8_t (&pins)[6]) {
  const char *name = orderName(order);
  for (uint8_t slot = 0; slot < 6; ++slot) {
    const char colour = "RGB"[slot % 3];
    uint8_t position = 0;
    while (position < 3 && name[position] != colour) ++position;
    pins[slot] = BASE_PINS[3 * (slot / 3) + position];
  }
}

} // namespace rgb_order
