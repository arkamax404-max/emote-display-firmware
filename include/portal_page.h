#pragma once
#include <string>

// T2/T3 portal page renderer (S3, S4): the captive-portal configuration page
// as a pure, host-testable string builder with no Arduino/WiFi symbols. The
// firmware serves it from the dedicated portal TCP server.
//
// Secrets by construction:
// - The inputs are pre-filled with nothing: neither the offered home-network
//   passphrase nor the generated AP password is ever echoed into the HTML.
// - The rendered error text is an escaped human-readable reason, never the
//   values that caused it.
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
// never pre-filled.
inline std::string formPage(const std::string &error = "") {
  std::string page =
      "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>gentle-matrix Wi-Fi setup</title>"
      "<style>"
      "body{font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em}"
      "input{display:block;width:100%;box-sizing:border-box;margin:0.5em 0;padding:0.5em}"
      "button{padding:0.5em 1em}"
      ".error{color:#b00;font-weight:bold}"
      "</style></head><body>"
      "<h1>gentle-matrix</h1>"
      "<p>Choose the Wi-Fi network this display should join.</p>";
  if (!error.empty()) page += "<p class=\"error\">" + escapeHtml(error) + "</p>";
  page +=
      "<form method=\"POST\" action=\"/save\">"
      "<input name=\"ssid\" maxlength=\"32\" autocapitalize=\"off\" "
      "autocomplete=\"off\" autocorrect=\"off\" spellcheck=\"false\" "
      "placeholder=\"Network name (SSID)\" required>"
      "<input type=\"password\" name=\"password\" maxlength=\"63\" "
      "autocomplete=\"new-password\" "
      "placeholder=\"Password (leave empty for an open network)\">"
      "<button type=\"submit\">Save</button>"
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
} // namespace portal_page
