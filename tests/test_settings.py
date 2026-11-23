import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_serial_protocol import msvc_vcvars

ROOT = pathlib.Path(__file__).resolve().parents[1]


def feed(text):
    if isinstance(text, str):
        text = text.encode("latin-1")
    return text.hex()  # '-' later means "empty value" in CLI arguments


class SettingsTests(unittest.TestCase):
    """S8-S10 (T11-T13): the LAN settings page, the 12/24 h clock format and
    the runtime brightness value rules, host-tested without a panel."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.binary = cls.work / "panel_settings"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        if not cxx:
            if not msvc_vcvars():
                raise unittest.SkipTest("no host C++ compiler available")
            bat = cls.work / "compile.bat"
            bat.write_text(
                '@echo off\r\n'
                'call "{}" >nul\r\n'
                'cl /nologo /EHsc /std:c++14 /W3 /WX /D_CRT_SECURE_NO_WARNINGS '
                '/I "{}" "{}" /Fe:"{}" /Fo:"{}\\\\SettingsTest.obj"\r\n'.format(
                    msvc_vcvars(), str(ROOT / "include"),
                    str(ROOT / "tests/settings_test.cpp"), str(cls.binary),
                    str(cls.work)),
                encoding="ascii")
            subprocess.run(["cmd", "/c", str(bat)], check=True, capture_output=True)
            return
        subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "include"),
                        str(ROOT / "tests/settings_test.cpp"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_script(self, lines):
        result = subprocess.run([str(self.binary)],
                                input=("\n".join(lines) + "\n").encode("latin-1"),
                                capture_output=True, check=True)
        return result.stdout.decode("latin-1").splitlines()

    # --- S10: the runtime brightness value rules -----------------------------

    def test_default_is_30_percent_and_the_scale_model(self):
        # Fresh CLI process: the default is 30 % and the channel scale is the
        # stated rule percent * 255 / 100 -> 76, in the era of the old
        # compile-time constant 64.
        self.assertEqual(self.run_script(["get"]), ["get p=30 s=76"])

    def test_every_valid_percent_maps_exactly(self):
        lines, expected = [], []
        for percent in range(10, 81, 5):
            lines.append(f"scale {percent}")
            expected.append(f"scale {percent * 255 // 100}")
        self.assertEqual(self.run_script(lines), expected)

    def test_exactly_the_allowed_set_validates(self):
        valid = {10 + 5 * step for step in range(15)}
        lines = [f"valid {percent}" for percent in range(256)]
        self.assertEqual(self.run_script(lines),
                         [f"valid {1 if percent in valid else 0}"
                          for percent in range(256)])

    def test_setter_applies_valid_values_and_rejects_the_rest(self):
        self.assertEqual(self.run_script(["setb 80", "setb 33", "setb 81",
                                          "setb default"]),
                         ["setb ok p=80 s=204", "setb reject p=80 s=204",
                          "setb reject p=80 s=204", "setb ok p=30 s=76"])

    def test_color_scales_through_the_live_value_without_a_panel(self):
        # Full channels at 80 %: R (31*204+127)/255 = 25, G (63*204+127)/255 = 50.
        self.assertEqual(self.run_script(["setb 80", "color f800", "color 07e0",
                                          "color 001f", "color 0000"]),
                         ["setb ok p=80 s=204", "color c800", "color 0640",
                          "color 0019", "color 0000"])
        # And back at the default the same inputs give the 76-scale result.
        self.assertEqual(self.run_script(["setb default", "color f800"]),
                         ["setb ok p=30 s=76", "color 4800"])

    # --- POST /config form parsing and value review --------------------------

    def test_form_parses_both_required_keys(self):
        self.assertEqual(self.run_script(["form " + feed("brightness=45&clock=12h")]),
                         ["form ok b=45 c=12h"])
        self.assertEqual(self.run_script(["form " + feed("brightness=80&clock=24h")]),
                         ["form ok b=80 c=24h"])

    def test_form_url_rules_mirroring_the_portal(self):
        # %XX decoding for both keys; '+' is a space; bare spaces and control
        # bytes are rejected, not coerced.
        self.assertEqual(self.run_script(["form " + feed("brightness=%34%35&clock=12h")]),
                         ["form ok b=45 c=12h"])
        self.assertEqual(self.run_script(["form " + feed("brightness=+10&clock=12h")]),
                         ["form ok b= 10 c=12h"])  # '+' -> space, review rejects below
        self.assertEqual(self.run_script(["form " + feed("brightness=1 0&clock=12h")]),
                         ["form error"])
        self.assertEqual(self.run_script(["form " + feed("brightness=10&clock=12%68")]),
                         ["form ok b=10 c=12h"])

    def test_form_rejects_missing_unknown_and_duplicate_keys(self):
        for body in ("brightness=45", "clock=12h", "",
                     "brightness=45&clock=12h&extra=1",
                     "brightness=45&brightness=45&clock=12h",
                     "clock=12h&clock=24h&brightness=45",
                     "brightness45=45&clock=12h", "brightness",
                     "brightness=45&clock=24h&clock=24h"):
            with self.subTest(body=body):
                self.assertEqual(self.run_script(["form " + feed(body)]), ["form error"])

    def test_brightness_review_is_the_documented_allowed_set(self):
        for value, ok in (("10", 10), ("15", 15), ("75", 75), ("80", 80),
                          ("030", 30), ("9", None), ("76", None), ("81", None), ("85", None),
                          ("", None), ("999", None), ("1x0", None), (" 10", None),
                          ("abc", None), ("-10", None)):
            with self.subTest(value=value):
                self.assertEqual(self.run_script(["bright " + feed(value)]),
                                 ["bright ok %d" % ok] if ok is not None
                                 else ["bright error"])

    def test_clock_review_accepts_exactly_the_two_names(self):
        for value, ok in (("24h", "24h"), ("12h", "12h"), ("24", None),
                          ("12H", None), ("12hX", None), ("", None)):
            with self.subTest(value=value):
                self.assertEqual(self.run_script(["clock " + feed(value)]),
                                 ["clock ok %s" % ok] if ok else ["clock error"])

    # --- S8: the /config route contract --------------------------------------

    def test_only_the_exact_config_request_lines_are_route_owned(self):
        # The interception is decided on the exact first line BEFORE the
        # receiver protocol parser: the receiver's routes and the unknown
        # path 404 keep their exact semantics.
        self.assertEqual(self.run_script(["route " + feed("GET /config HTTP/1.1")]),
                         ["route get"])
        self.assertEqual(self.run_script(["route " + feed("GET /config HTTP/1.0")]),
                         ["route get"])
        self.assertEqual(self.run_script(["route " + feed("POST /config HTTP/1.1")]),
                         ["route save"])
        self.assertEqual(self.run_script(["route " + feed("POST /config HTTP/1.0")]),
                         ["route save"])
        for line in ("GET /emote HTTP/1.1", "POST /emote HTTP/1.1",
                     "GET /health HTTP/1.1", "GET /configx HTTP/1.1",
                     "GET /config HTTP/9.9", "get /config HTTP/1.1",
                     "HEAD /config HTTP/1.1",
                     "GET /config", "GET /Config HTTP/1.1"):
            self.assertEqual(self.run_script(["route " + feed(line)]), ["route other"])

    def test_content_length_extraction_is_bounded_and_strict(self):
        self.assertEqual(self.run_script(["length " + feed("POST /config HTTP/1.1\r\n"
                                               "Content-Length: 24\r\n\r\n")]),
                         ["length 24"])
        self.assertEqual(self.run_script(["length " + feed("POST /config HTTP/1.1\r\n"
                                               "content-length: 255\r\n\r\n")]),
                         ["length 255"])
        for headers in ("POST /config HTTP/1.1\r\n\r\n",
                        "POST /config HTTP/1.1\r\nContent-Length: \r\n\r\n",
                        "POST /config HTTP/1.1\r\nContent-Length: x\r\n\r\n",
                        "POST /config HTTP/1.1\r\nContent-Length: 1x\r\n\r\n"):
            self.assertEqual(self.run_script(["length " + feed(headers)]), ["length -1"])

    # --- S8: the page --------------------------------------------------------

    def test_page_offers_every_allowed_brightness_and_both_clocks(self):
        out = self.run_script(["page 30 24h -"])[0]
        self.assertIn('action="/config"', out)
        self.assertIn('method="POST"', out)
        for percent in range(10, 81, 5):
            self.assertIn(f'value="{percent}"', out)
        self.assertIn('value="30" selected', out)
        self.assertNotIn('value="10" selected', out)
        self.assertIn('value="24h" selected', out)
        self.assertIn('value="12h">', out)
        self.assertIn("no restart", out)

    def test_page_preselects_the_current_values_and_escapes_the_error(self):
        out = self.run_script(["page 65 12h " + feed("<x>&")])[0]
        self.assertIn('value="65" selected', out)
        self.assertIn('value="12h" selected', out)
        self.assertIn('value="24h">', out)
        self.assertIn('&lt;x&gt;&amp;', out)

    def test_saved_page_says_settings_apply_without_a_restart(self):
        out = self.run_script(["saved"])[0]
        self.assertIn("applied immediately", out)
        self.assertIn("no restart", out)
        self.assertIn('href="/config"', out)

    # --- wiring pins (structural only; the functional surface is above) ------

    def test_station_serving_wiring_pins(self):
        # Secondary evidence only (the functional contracts above drive the
        # same pure helpers main.cpp wires). These pins hold that the
        # firmware actually wires the settings surface and that the receiver
        # parser is untouched.
        source = (ROOT / "src/main.cpp").read_text()
        self.assertLess(source.index("panel_settings::routeOf("),
                        source.index("receiver::parseHeaders(headers, request)"))
        # The first-line check happens inside the header loop, before the
        # \r\n\r\n classification, and is reset with the request state.
        self.assertIn("configFirstLineChecked = true;", source)
        self.assertIn("configFirstLineChecked = false;", source)
        # The settings body never enters the PNG payload buffer.
        self.assertIn("configBody.push_back", source)
        self.assertIn("applyConfigSave()", source)
        # Runtime values apply through the ONE setter: the loader and the
        # POST /config application, nothing else.
        self.assertEqual(source.count("panel::setBrightnessPercent("), 2)
        self.assertIn("loadPanelSettings();", source)
        self.assertEqual(source.count("storePanelSettings("), 2)
        # The clock draw call passes the runtime format; exactly one site.
        self.assertEqual(source.count("clockFormat);"), 1)
        # /health reports the live scale first, then the additive percent.
        self.assertEqual(source.count("panel::brightnessScale())"), 1)
        self.assertLess(source.index("panel::brightnessScale())"),
                        source.index("panel::brightnessPercent())"))
        # Persistence keys with read-back verification.
        for key in ('putUChar("brightness"', 'putUChar("clockformat"',
                    'getUChar("brightness", 255)', 'getUChar("clockformat", 255)'):
            self.assertIn(key, source)
        # The DOWN reset scope is unchanged: no settings key is ever stored
        # or removed by the reset routine (the header only names brightness
        # in prose as an out-of-scope value).
        reset = (ROOT / "include/wifi_credentials_reset.h").read_text()
        self.assertNotIn('"brightness"', reset)
        self.assertNotIn('"clockformat"', reset)
        # Every drawing consumer still scales through the one helper.
        self.assertEqual(source.count("panel::color("), 5)
        self.assertIn("panel::color(",
                      (ROOT / "include/firmware_version.h").read_text())
        self.assertIn("panel::color(",
                      (ROOT / "include/diagnostic_status.h").read_text())


if __name__ == "__main__":
    unittest.main()
