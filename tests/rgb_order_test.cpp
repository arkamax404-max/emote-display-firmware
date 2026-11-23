// Host test CLI for the panel channel order (S7, T10). Four modes driven by
// a stdin script, one command per line:
//
//   order     include/rgb_order.h pure mapping (no Arduino symbols).
//       base                  base wiring → "base <p0> ... <p5>"
//       pins <hex>            order name (case as offered) →
//                             "pins <p0> ... <p5>" | "pins reject"
//       parse <hex>           order name → "parse <CANONICAL>" | "parse reject"
//       code <hex>            order name → "code <n>" | "code reject"
//       decode <n>            stored code → "decode <NAME>" | "decode reject"
//
//   policy    include/portal_policy.h form shaping for the colour order
//             (S7): the same URL rules as the credentials form.
//       oform <hex|->         urlencoded body → "orderform error" |
//                             "orderform order=<e> token=<e>"
//
//   console   the RGB command in the provisioning console (S7), mirroring
//             the serial_ingress_test console rig.
//       cfeed <hex>           console bytes → "out <line>" events
//       ofail <0|1>           make the order store succeed/fail
//       cshow                 → "order=<NAME|-> cred=<ssid>|<pass>
//                             flush=<n> reboot=<0|1>"
//
//   page      include/portal_page.h rendering with the S7 selector.
//       page <errHex|-> <tokenHex|-> <NAME>
//                             current order → "page <escaped html>"
//       savedpage             → "page <escaped html>"
#include "rgb_order.h"
#include "portal_policy.h"
#include "portal_page.h"
#include "serial_ingress.h"
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
std::string unhex(const std::string &hex) {
  if (hex == "-") return {}; // convention: '-' means an empty value
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

void runOrderScript() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "base") {
      std::cout << "base";
      for (const uint8_t pin : rgb_order::BASE_PINS) std::cout << ' ' << unsigned(pin);
      std::cout << '\n';
    } else if (cmd == "pins") {
      std::string nameHex;
      args >> nameHex;
      rgb_order::Order order;
      if (!rgb_order::parseOrder(unhex(nameHex), order)) {
        std::cout << "pins reject\n";
      } else {
        uint8_t pins[6];
        rgb_order::applyOrder(order, pins);
        std::cout << "pins";
        for (const uint8_t pin : pins) std::cout << ' ' << unsigned(pin);
        std::cout << '\n';
      }
    } else if (cmd == "parse") {
      std::string nameHex;
      args >> nameHex;
      rgb_order::Order order;
      if (!rgb_order::parseOrder(unhex(nameHex), order))
        std::cout << "parse reject\n";
      else
        std::cout << "parse " << rgb_order::orderName(order) << '\n';
    } else if (cmd == "code") {
      std::string nameHex;
      args >> nameHex;
      rgb_order::Order order;
      if (!rgb_order::parseOrder(unhex(nameHex), order))
        std::cout << "code reject\n";
      else
        std::cout << "code " << unsigned(rgb_order::orderCode(order)) << '\n';
    } else if (cmd == "decode") {
      unsigned code = 0;
      args >> code;
      rgb_order::Order order;
      if (!rgb_order::orderFromCode(static_cast<uint8_t>(code), order))
        std::cout << "decode reject\n";
      else
        std::cout << "decode " << rgb_order::orderName(order) << '\n';
    }
  }
}

void runPolicyScript() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd, bodyHex;
    args >> cmd >> bodyHex;
    if (cmd == "oform") {
      const std::string body = unhex(bodyHex);
      const portal::OrderFields fields = portal::parseOrderForm(body.data(), body.size());
      if (!fields.ok) {
        std::cout << "orderform error\n";
      } else {
        std::cout << "orderform order=" << escape(fields.order)
                  << " token=" << escape(fields.token) << '\n';
      }
    }
  }
}

void runConsoleScript() {
  std::string storedSsid, storedPass;
  rgb_order::Order savedOrder = rgb_order::Order::RGB;
  bool orderStored = false, failOrderStore = false, rebooted = false;
  int flushCount = 0;
  serial::ProvisionConsole::Hooks hooks;
  hooks.print = [](const char *text) { std::cout << "out " << text << '\n'; };
  hooks.flush = [&flushCount]() { ++flushCount; };
  hooks.store = [&storedSsid, &storedPass](const char *ssid, const char *password) {
    storedSsid = ssid;
    storedPass = password;
    return true;
  };
  hooks.recalibrate = []() {};
  hooks.reboot = [&rebooted]() { rebooted = true; };
  hooks.setOrder = [&](rgb_order::Order order) {
    if (failOrderStore) return false;
    savedOrder = order;
    orderStored = true;
    return true;
  };
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
    } else if (cmd == "ofail") {
      std::string value;
      args >> value;
      failOrderStore = value == "1";
    } else if (cmd == "cshow") {
      std::cout << "order=" << (orderStored ? rgb_order::orderName(savedOrder) : "-")
                << " cred=" << (storedSsid.empty() ? "-" : storedSsid) << '|'
                << (storedPass.empty() ? "-" : storedPass)
                << " flush=" << flushCount << " reboot=" << (rebooted ? 1 : 0) << '\n';
    }
  }
}

void runPageScript() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd, errorHex, tokenHex, name;
    args >> cmd;
    if (cmd == "page") {
      args >> errorHex >> tokenHex >> name;
      rgb_order::Order current = rgb_order::Order::RGB;
      rgb_order::parseOrder(name, current); // CLI accepts canonical names only
      std::cout << "page "
                << escape(portal_page::formPage(unhex(errorHex), unhex(tokenHex),
                                                {}, current))
                << '\n';
    } else if (cmd == "savedpage") {
      std::cout << "page " << escape(portal_page::orderSavedPage()) << '\n';
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
  const std::string mode = argc > 1 ? argv[1] : "order";
  if (mode == "policy") runPolicyScript();
  else if (mode == "console") runConsoleScript();
  else if (mode == "page") runPageScript();
  else runOrderScript();
  return 0;
}
