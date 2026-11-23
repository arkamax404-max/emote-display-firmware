// Host test CLI for the captive-portal policy layer (T1) and the portal
// form page (T2/T3). Two stdin-script modes, one command per line:
//
//   policy    include/portal_policy.h decisions (no Arduino/WiFi symbols).
//       boot <now>             start the boot retry schedule → "boot attempts=<n>"
//                              (the setup Wi-Fi.begin counts as attempt one)
//       update <now> <0|1>     one poll step (connected flag) →
//                              "tick <wait|retry|exhausted> attempts=<n>"
//       decide <usb> <creds> <exhausted>          → "decide <none|open>"
//       check <hex> <hex>      SSID, password review → "check ok|<error text>"
//       conclude <0|1>         validation outcome mapping → "outcome <portal|save>"
//       req <hex>              HTTP request line → "req <error|get|post> <path>"
//       form <hex>             urlencoded body → "form error" |
//                                                 "form ssid=<e> pass=<e>"
//       pass <seed>            8-char AP password with an injected LCG →
//                              "pass <len> <escaped>"
//       alphabet               panel-drawable AP password alphabet →
//                              "alphabet <escaped>"
//
//   page      include/portal_page.h rendering (HTML escape + error block).
//       page <hex>             error text; empty hex = no error block →
//                              "page <escaped html>"
#include "portal_policy.h"
#include "portal_page.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

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

const char *tickName(portal::BootRetry::Tick tick) {
  switch (tick) {
    case portal::BootRetry::Tick::Retry: return "retry";
    case portal::BootRetry::Tick::Exhausted: return "exhausted";
    default: return "wait";
  }
}

// Deterministic per-seed draw source for the injected randomness contract.
uint32_t lcgState = 0;
uint32_t lcgDraw() {
  lcgState = lcgState * 1664525u + 1013904223u;
  return lcgState;
}

void runPolicyScript() {
  portal::BootRetry bootRetry;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd;
    args >> cmd;
    if (cmd == "boot") {
      uint32_t now = 0;
      args >> now;
      bootRetry.start(now);
      std::cout << "boot attempts=" << unsigned(bootRetry.attempts()) << '\n';
    } else if (cmd == "update") {
      uint32_t now = 0;
      int connected = 0;
      args >> now >> connected;
      const portal::BootRetry::Tick tick = bootRetry.update(now, connected != 0);
      std::cout << "tick " << tickName(tick) << " attempts="
                << unsigned(bootRetry.attempts()) << '\n';
    } else if (cmd == "decide") {
      int usb = 0, creds = 0, exhausted = 0;
      args >> usb >> creds >> exhausted;
      const portal::Decision decision = portal::decide(usb != 0, creds != 0, exhausted != 0);
      std::cout << "decide "
                << (decision == portal::Decision::OpenPortal ? "open" : "none") << '\n';
    } else if (cmd == "check") {
      std::string ssidHex, passHex;
      args >> ssidHex >> passHex;
      const portal::CredError error =
          portal::reviewCredentials(unhex(ssidHex), unhex(passHex));
      std::cout << "check " << (error == portal::CredError::Ok
                                    ? "ok"
                                    : portal::credErrorText(error)) << '\n';
    } else if (cmd == "conclude") {
      int connected = 0;
      args >> connected;
      std::cout << "outcome "
                << (portal::concludeValidation(connected != 0) ==
                            portal::Outcome::SaveAndReboot
                        ? "save"
                        : "portal") << '\n';
    } else if (cmd == "req") {
      std::string textHex;
      args >> textHex;
      const portal::RequestLine parsed = portal::parseRequestLine(unhex(textHex));
      if (!parsed.ok) std::cout << "req error\n";
      else std::cout << "req " << (parsed.isPost ? "post" : "get") << ' '
                     << parsed.path << '\n';
    } else if (cmd == "form") {
      std::string bodyHex;
      args >> bodyHex;
      const std::string body = unhex(bodyHex);
      const portal::FormFields fields = portal::parseForm(body.data(), body.size());
      if (!fields.ok) {
        std::cout << "form error\n";
      } else {
        std::cout << "form ssid=" << escape(fields.ssid)
                  << " pass=" << escape(fields.password) << '\n';
      }
    } else if (cmd == "pass") {
      uint32_t seed = 0;
      args >> seed;
      lcgState = seed;
      char generated[portal::AP_PASSWORD_LEN + 1] = {};
      portal::generateApPassword(generated, lcgDraw);
      std::cout << "pass " << strlen(generated) << ' ' << escape(generated) << '\n';
    } else if (cmd == "alphabet") {
      std::cout << "alphabet " << portal::AP_ALPHABET << '\n';
    }
  }
}

void runPageScript() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream args(line);
    std::string cmd, errorHex;
    args >> cmd >> errorHex;
    if (cmd == "page") {
      const std::string error = unhex(errorHex);
      std::cout << "page " << escape(portal_page::formPage(error)) << '\n';
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  (void)argc;
  const std::string mode = argc > 1 ? argv[1] : "policy";
  if (mode == "page") runPageScript();
  else runPolicyScript();
  return 0;
}
