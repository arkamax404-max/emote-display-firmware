#include "wifi_connect_banner.h"
#include <cassert>
#include <cstdint>
#include <string>

using wifi_banner::ConnectBanner;
using wifi_banner::addressLines;
int main() {
  // The window opens ONLY on the disconnected -> connected transition, and
  // the three seconds are counted from the connection event, never from the
  // first paint.
  ConnectBanner banner;
  assert(!banner.active(0));
  assert(!banner.active(999));
  assert(!banner.update(false, 100)); // still disconnected: no window
  assert(!banner.active(100));
  assert(banner.update(true, 1000)); // the connection event
  banner.capture("192.168.1.227");
  assert(banner.active(1000));
  assert(banner.address() == "192.168.1.227");

  // Every redraw, repaint and repeated WL_CONNECTED poll between the event
  // and the expiry is a pure read: none of them extends or restarts the
  // three seconds.
  for (uint32_t now = 1000; now < 4000; now += 137) {
    assert(banner.active(now));
    assert(!banner.update(true, now)); // repeated connected poll
  }
  assert(banner.active(3999));
  assert(!banner.active(4000)); // exactly 3000 ms after the event
  // The address stays recorded after expiry (it is the live one, never a
  // stored or guessed value), and a repeated connected poll never opens a
  // second window.
  assert(banner.address() == "192.168.1.227");
  assert(!banner.update(true, 4500));
  assert(!banner.active(4500));

  // A loss ends the window at once — a dead connection never paints an
  // address — and a reconnect after a real loss triggers the banner again,
  // counting the new three seconds from the new connection event.
  ConnectBanner again;
  assert(again.update(true, 1000));
  again.capture("10.1.2.3");
  assert(again.active(2500));
  again.capture("10.1.2.30"); // capture only happens at the transition
  assert(again.address() == "10.1.2.30");
  assert(!again.update(false, 2600));
  assert(!again.active(2600));
  assert(!again.update(false, 5000));
  assert(again.update(true, 6000));
  assert(again.active(6000));
  again.capture("10.9.9.9");
  assert(again.address() == "10.9.9.9");
  assert(again.active(8999));
  assert(!again.active(9000));

  // An incoming PNG keeps priority: once a frame is staged during the
  // window the banner is closed for good — a later connected poll must not
  // resurrect it — and only a real disconnect -> connect transition can
  // bring it back.
  ConnectBanner png;
  assert(png.update(true, 1000));
  png.capture("1.2.3.4");
  assert(png.active(1500));
  png.close(); // stagePng succeeded while the banner was showing
  assert(!png.active(1500));
  assert(!png.update(true, 1700));
  assert(!png.active(1700));
  assert(!png.update(false, 2000));
  assert(png.update(true, 3000));
  png.capture("1.2.3.5");
  assert(png.active(3000));
  assert(png.address() == "1.2.3.5");

  // Wrap-safe uint32 window across the millis() rollover: the event at
  // UINT32_MAX - 100 expires 3000 ms later, at 2899.
  ConnectBanner wrapped;
  assert(wrapped.update(true, UINT32_MAX - 100));
  assert(wrapped.active(UINT32_MAX));
  assert(wrapped.active(2000));
  assert(wrapped.active(2898));
  assert(!wrapped.active(2899));

  // The documented two-row split, exactly like the Wi-Fi info screen: the
  // address is cut at its second dot and no row exceeds ten characters, so
  // both rows fit the 64 px width with the 2 px margin.
  const auto rows = addressLines("192.168.1.227"); // three-digit final octet
  assert(rows[0] == "192.168." && rows[1] == "1.227");
  assert(rows[0] + rows[1] == "192.168.1.227");
  const auto wide = addressLines("255.255.255.255");
  assert(wide[0] + wide[1] == "255.255.255.255");
  const auto small = addressLines("1.2.3.4");
  assert(small[0] == "1.2." && small[1] == "3.4");
  for (const auto *text : {"192.168.1.227", "255.255.255.255", "1.2.3.4",
                           "10.0.0.1"}) {
    const auto lines = addressLines(text);
    for (const auto &line : lines) {
      assert(line.size() <= 10);
      assert(2 + line.size() * 6 <= 64); // at 6 px per character
    }
  }

  // The panel render: the two rows in the info screen's exact text style,
  // at the info grid's rows 2 and 3 with the 2 px margin, and scaled
  // through the one shared brightness helper like every other consumer.
  struct FakeDisplay {
    std::string printed[2];
    int x[2] = {-1, -1}, y[2] = {-1, -1};
    bool wrap = true;
    int size = 0;
    uint16_t color565(int r, int g, int b) const {
      return static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
    void setTextWrap(bool value) { wrap = value; }
    void setTextSize(int value) { size = value; }
    void setTextColor(uint16_t) {}
    void setCursor(int px, int py) {
      if (printed[0].empty()) { x[0] = px; y[0] = py; }
      else { x[1] = px; y[1] = py; }
    }
    void print(const char *text) {
      (printed[0].empty() ? printed[0] : printed[1]) = text;
    }
  };
  FakeDisplay display;
  wifi_banner::draw(display, std::string("192.168.1.227"));
  assert(!display.wrap && display.size == 1);
  assert(display.printed[0] == "192.168." && display.printed[1] == "1.227");
  assert(display.x[0] == 2 && display.x[1] == 2);
  assert(display.y[0] == panel::TOP_OFFSET + 16);
  assert(display.y[1] == panel::TOP_OFFSET + 24);
}
