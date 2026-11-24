import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/main.cpp"


def block(source, start_marker, end_marker="\n}\n"):
    """The body of a function/static block, for containment pins."""
    return source.split(start_marker, 1)[1].split(end_marker, 1)[0]


class BannerHostTests(unittest.TestCase):
    def test_connect_banner_state_machine_and_two_row_split(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "wifi_banner"
            subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "include"),
                            str(ROOT / "tests/wifi_banner_test.cpp"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


class BannerWiringTests(unittest.TestCase):
    """S12: how the connect banner is wired into the firmware, pinned
    structurally over src/main.cpp. The banner opens on the
    disconnected -> connected transition only, is drawn between the Wi-Fi
    info overlay and the clock, is closed for good by any staged PNG, and
    defers the READY / SEND PNG hint until the three seconds end."""

    def source(self):
        return SOURCE.read_text()

    def test_render_branch_sits_between_the_info_overlay_and_the_clock(self):
        source = self.source()
        self.assertIn('#include "wifi_connect_banner.h"', source)
        self.assertIn("static wifi_banner::ConnectBanner connectBanner;", source)
        render = source.split("static void renderContent() {", 1)[1].split(
            "static void presentDisplay()", 1)[0]
        banner = block(render, "if (connectBanner.active(")
        # The UP-button Wi-Fi info overlay keeps its priority: the banner is
        # only drawn when the overlay is not visible.
        self.assertLess(render.index("if (wifiInfo.visible())"),
                        render.index("if (connectBanner.active("))
        # The clock branch, the weather alternation and the PNG path keep
        # their exact order behind the banner.
        self.assertLess(render.index("if (connectBanner.active("),
                        render.index("if (source == idle_clock::Source::Clock)"))
        # The two rows come from the header draw helper (the same pattern as
        # firmware_version/weather_screen), which holds the shared split,
        # the info screen's exact text style and the shared brightness
        # scaling — main.cpp gains no new colour call site.
        self.assertIn("wifi_banner::draw(matrix, connectBanner.address());",
                      banner)
        self.assertIn("return;", banner)
        header = (ROOT / "include/wifi_connect_banner.h").read_text()
        self.assertIn("panel::color(display.color565(224, 224, 224))", header)
        self.assertIn("panel::TOP_OFFSET + (2 + row) * 8", header)
        self.assertIn("display.setTextWrap(false)", header)
        self.assertIn("display.setTextSize(1)", header)
        self.assertIn("addressLines(ip)", header)
        # The banner never goes through displayMessage: it must not
        # overwrite the message pair or drop the staged frame.
        self.assertNotIn("displayMessage(", banner)

    def test_png_takes_the_panel_during_the_window(self):
        source = self.source()
        # Exactly one close() call, inside stagePng's success branch: both
        # ingress paths (HTTP and serial) stage through it, so any accepted
        # frame during the window cancels the diagnostic before the single
        # present that shows the frame.
        self.assertEqual(source.count("connectBanner.close()"), 1)
        stage = source.split("static bool stagePng(", 1)[1].split("\n}\n", 1)[0]
        self.assertLess(stage.index("if (!decoded) return false;"),
                        stage.index("connectBanner.close();"))

    def test_pollwifi_opens_the_window_on_the_transition_only(self):
        source = self.source()
        poll = block(source, "static void pollWifi() {")
        # One connectivity check per loop drives the banner.
        self.assertIn("const bool connected = WiFi.status() == WL_CONNECTED;",
                      poll)
        self.assertIn("const bool bannerWasActive = connectBanner.active(now);",
                      poll)
        self.assertIn("connectBanner.update(connected, now)", poll)
        opened = block(poll, "if (connectBanner.update(connected, now)) {")
        # The live address of the just-established connection is captured at
        # the transition moment, never a stored or guessed value.
        self.assertIn("connectBanner.capture(WiFi.localIP().toString().c_str());",
                      opened)
        self.assertLess(opened.index("connectBanner.capture("),
                        opened.index("presentDisplay();"))
        # The three seconds run from the event: when the window ends (expiry
        # or preemption) the panel returns to whatever it was showing.
        expired = block(poll, "else if (bannerWasActive && !connectBanner.active(now)) {")
        self.assertIn("presentDisplay();", expired)
        # The READY / SEND PNG hint waits until AFTER the banner: nothing
        # else is flashed before the three seconds are over, and a frame
        # staged during the window keeps the hint silent.
        self.assertIn('displayMessage("READY", "SEND PNG");', expired)
        self.assertIn("readyPending && !hasFrame", expired)
        # The old immediate first-listen flash is gone.
        self.assertNotIn('if (accepted == 0) displayMessage("READY", "SEND PNG");',
                         source)
        listen = block(source, "if (!listening) {")
        self.assertIn("if (accepted == 0) readyPending = true;", listen)


if __name__ == "__main__":
    unittest.main()
