#pragma once
#include <string>
#include <vector>

#include "portal_policy.h" // portal::ScanEntry, one candidate per radio button
#include "rgb_order.h"     // rgb_order::ORDER_COUNT, the six named orders

// T2/T3 portal page renderer (S3, S4) with the S5 network selector (T7): the
// captive-portal configuration page as a pure, host-testable string builder
// with no Arduino/WiFi symbols. The firmware serves it from the dedicated
// portal TCP server.
//
// Secrets by construction:
// - The inputs are pre-filled with nothing: neither the offered home-network
//   passphrase nor the generated AP password is ever echoed into the HTML.
// - The rendered error text is an escaped human-readable reason, never the
//   values that caused it.
//
// S5 selector (T7, user decision): the visible networks the fresh scan
// found are offered as radio buttons of the single `ssid` field, every SSID
// HTML-escaped in both the value and the label. The manual SSID input is
// gone — a hidden or out-of-range network cannot be configured here; USB
// provisioning remains the route for those. The CSRF token and the
// never-echoed password field are unchanged.
//
// S7 selector: the page also carries a second, separate form (POST
// /rgborder) with the six named channel orders, carrying the same session
// token as the credentials form; saving reboots the board, which the page
// says plainly next to the selector. `current` preselects the order the
// board is actually running (the persisted value, RGB by default).
namespace portal_page {

inline std::string escapeHtml(const std::string &raw) {
  std::string out;
  for (const char c : raw) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// The form page. GET any path returns it (so probe URLs like
// /generate_204 land on it and trigger the captive-portal prompt); after a
// failed save, `error` re-renders inside a visible error block. Inputs are
// never pre-filled. `networks` is the already-shaped scan selection (the
// caller ran portal::selectNetworks).
inline std::string formPage(const std::string &error = "", const std::string &token = "",
                            const std::vector<portal::ScanEntry> &networks = {},
                            rgb_order::Order current = rgb_order::Order::RGB) {
  std::string page =
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>gentle-matrix Wi-Fi setup</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em}"
      // Full-width fields only for the text inputs: the earlier bare `input`
      // rule also hit the radios, turning every network into a full-width
      // block and pushing its label out of line.
      "input[type=password]{display:block;width:100%;box-sizing:border-box;"
      "margin:0.5em 0;padding:0.5em}"
      "select{display:block;width:100%;box-sizing:border-box;margin:0.5em 0;"
      "padding:0.5em}"
      "label.net{display:flex;align-items:center;gap:0.6em;padding:0.35em 0}"
      "label.net input{margin:0;flex:0 0 auto}"
      "label.net span{flex:1 1 auto;word-break:break-all}"
      "button{padding:0.5em 1em}"
      ".error{color:#b00;font-weight:bold}"
      "</style></head><body>"
      "<h1>gentle-matrix</h1>"
      "<p>Choose the Wi-Fi network this display should join.</p>";
  if (!error.empty()) page += "<p class=\"error\">" + escapeHtml(error) + "</p>";
  page += "<form method=\"POST\" action=\"/save\">";
  if (!token.empty()) page += "<input type=\"hidden\" name=\"token\" value=\"" + escapeHtml(token) + "\">";
  page +=
      "<fieldset><legend>Detected networks</legend>";
  if (networks.empty()) {
    page += "<p>No networks detected yet. Reload this page to scan again.</p>";
  } else {
    for (const portal::ScanEntry &network : networks) {
      const std::string escaped = escapeHtml(network.ssid);
      page += "<label class=\"net\"><input type=\"radio\" name=\"ssid\" value=\"" + escaped +
              "\" required><span>" + escaped + "</span></label>";
    }
  }
  page +=
      "</fieldset>"
      "<p><a href=\"/\">Scan again</a></p>"
      "<input type=\"password\" name=\"password\" maxlength=\"63\" "
      "autocomplete=\"new-password\" "
      "placeholder=\"Password (leave empty for an open network)\">"
      "<button type=\"submit\">Save</button>"
      "</form>";
  // S7: the channel-order selector is submitted by its OWN form to its OWN
  // route; the credentials form above is untouched. The session token is
  // required exactly like the credentials form and a tokenless POST is
  // never accepted. The board restarts to apply the choice — said plainly
  // on the page.
  page += "<h2>Panel colours</h2>"
          "<p>If the colours come out wrong, pick the channel order this "
          "panel shows on its data pins and save. The board restarts to "
          "apply it.</p>"
          "<form method=\"POST\" action=\"/rgborder\">";
  if (!token.empty()) page += "<input type=\"hidden\" name=\"token\" value=\"" + escapeHtml(token) + "\">";
  page += "<select name=\"order\">";
  for (uint8_t i = 0; i < rgb_order::ORDER_COUNT; ++i) {
    const rgb_order::Order order = static_cast<rgb_order::Order>(i);
    page += "<option value=\"";
    page += rgb_order::orderName(order);
    if (order == current) page += "\" selected>";
    else page += "\">";
    page += rgb_order::orderName(order);
    page += order == rgb_order::Order::RGB ? " (default)</option>" : "</option>";
  }
  page += "</select>"
          "<button type=\"submit\">Apply colour order</button>"
          "</form></body></html>";
  return page;
}

// The success page: credentials were validated against the real network and
// persisted; the display restarts to join them.
inline std::string savedPage() {
  return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>gentle-matrix saved</title></head><body>"
         "<h1>gentle-matrix</h1>"
         "<p>Credentials saved. The display is restarting to join that "
         "network &mdash; you can close this page.</p></body></html>";
}

// The S7 success page: the channel order was persisted; the display
// restarts to apply it on the panel.
inline std::string orderSavedPage() {
  return "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>gentle-matrix colour order saved</title></head><body>"
         "<h1>gentle-matrix</h1>"
         "<p>Colour order saved. The display is restarting to apply it "
         "&mdash; you can close this page.</p></body></html>";
}
} // namespace portal_page
