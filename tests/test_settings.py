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
    """S7-S10 (T10, T11-T13 extendida): the LAN settings page (now including
    the panel channel order), the 12/24 h clock format and the runtime
    brightness value rules, host-tested without a panel."""

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

    def test_form_parses_all_three_required_keys(self):
        self.assertEqual(self.run_script(["form " + feed("brightness=45&clock=12h&order=GRB")]),
                         ["form ok b=45 c=12h o=GRB"])
        self.assertEqual(self.run_script(["form " + feed("brightness=80&clock=24h&order=rgb")]),
                         ["form ok b=80 c=24h o=rgb"])

    def test_form_url_rules_mirroring_the_portal(self):
        # %XX decoding for the keys; '+' is a space; bare spaces and control
        # bytes are rejected, not coerced.
        self.assertEqual(self.run_script(["form " + feed("brightness=%34%35&clock=12h&order=BRG")]),
                         ["form ok b=45 c=12h o=BRG"])
        self.assertEqual(self.run_script(["form " + feed("brightness=+10&clock=12h&order=RGB")]),
                         ["form ok b= 10 c=12h o=RGB"])  # '+' -> space, review rejects below
        self.assertEqual(self.run_script(["form " + feed("brightness=1 0&clock=12h&order=RGB")]),
                         ["form error"])
        self.assertEqual(self.run_script(["form " + feed("brightness=10&clock=12%68&order=BGR")]),
                         ["form ok b=10 c=12h o=BGR"])

    def test_form_rejects_missing_unknown_and_duplicate_keys(self):
        for body in ("brightness=45&clock=12h",  # the order key is missing
                     "clock=12h&order=RGB", "brightness=45&order=RGB", "order=RGB", "",
                     "brightness=45&clock=12h&extra=1",
                     "brightness=45&brightness=45&clock=12h&order=RGB",
                     "clock=12h&clock=24h&brightness=45&order=RGB",
                     "brightness=45&clock=12h&order=RGB&order=RBG",
                     "brightness45=45&clock=12h&order=RGB", "brightness",
                     "brightness=45&clock=24h&clock=24h&order=RGB"):
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

    def test_order_review_accepts_exactly_the_six_names(self):
        for value, ok in (("RGB", "RGB"), ("RBG", "RBG"), ("GRB", "GRB"),
                          ("GBR", "GBR"), ("BRG", "BRG"), ("BGR", "BGR"),
                          ("rgb", "RGB"), ("bgr", "BGR"),
                          ("RGBX", None), ("RG", None), (" RGB", None),
                          ("", None), ("1GB", None)):
            with self.subTest(value=value):
                self.assertEqual(self.run_script(["order " + feed(value)]),
                                 ["order ok %s" % ok] if ok else ["order error"])

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

    def test_only_a_complete_header_block_enables_the_length_parse(self):
        # The identical bytes up to the request line yield no length; the
        # complete block does. First-line classification sees only the line.
        line = feed("POST /config HTTP/1.1\r\n")
        block = feed("POST /config HTTP/1.1\r\nContent-Length: 24\r\n\r\n")
        self.assertEqual(self.run_script(["length " + line, "length " + block]),
                         ["length -1", "length 24"])

    # --- S8: the page --------------------------------------------------------

    def test_page_offers_every_allowed_brightness_both_clocks_and_all_orders(self):
        out = self.run_script(["page 30 24h RGB -"])[0]
        self.assertIn('action="/config"', out)
        self.assertIn('method="POST"', out)
        for percent in range(10, 81, 5):
            self.assertIn(f'value="{percent}"', out)
        self.assertIn('value="30" selected', out)
        self.assertNotIn('value="10" selected', out)
        self.assertIn('value="24h" selected', out)
        self.assertIn('value="12h">', out)
        # The panel channel order (S7): all six named permutations, the
        # current one preselected, with the restart rule stated plainly.
        self.assertIn('name="order"', out)
        for name in ("RGB", "RBG", "GRB", "GBR", "BRG", "BGR"):
            self.assertIn(f'value="{name}"', out)
        self.assertIn('value="RGB" selected', out)
        self.assertNotIn('value="BGR" selected', out)
        self.assertIn('RGB (default)', out)
        self.assertIn("restarts", out)
        self.assertIn("construction", out)

    def test_page_preselects_the_current_values_and_escapes_the_error(self):
        out = self.run_script(["page 65 12h BRG " + feed("<x>&")])[0]
        self.assertIn('value="65" selected', out)
        self.assertIn('value="12h" selected', out)
        self.assertIn('value="24h">', out)
        self.assertIn('value="BRG" selected', out)
        self.assertIn('value="RGB">', out)
        self.assertIn('&lt;x&gt;&amp;', out)

    def test_saved_page_says_settings_apply_without_a_restart(self):
        out = self.run_script(["saved"])[0]
        self.assertIn("applied immediately", out)
        self.assertIn("no restart", out)
        self.assertIn('href="/config"', out)

    def test_saved_page_announces_the_order_restart_when_it_changed(self):
        out = self.run_script(["saved restart"])[0]
        self.assertIn("restarts", out)
        self.assertIn("construction", out)
        self.assertIn('href="/config"', out)
        self.assertNotIn("no restart needed", out)

    # --- S9/S10 persistence transaction (verified commit + verified rollback) -

    def run_store(self, present, fail, percent=80, code=1, order=0):
        return self.run_script([f"store {present} {fail} {percent} {code} {order}"])[0]

    def test_store_success_persists_all_three_values(self):
        # Fresh namespace: every key is written, each verified by read-back.
        self.assertEqual(self.run_store(7, 0, 45, 1, 3), "store ok b=45 c=1 o=3")
        # The default offered order (0 = RGB) round-trips; zero is a valid
        # stored code, neither a put result of zero nor a read-back of zero
        # may be mistaken for a failure.
        self.assertEqual(self.run_store(7, 0, 25, 0, 0), "store ok b=25 c=0 o=0")

    def test_failed_commit_restores_the_exact_previous_triple(self):
        # The transaction: initial (30, H24), offered (80, H12); the
        # clockformat commit write cannot be persisted (key c bit 0). Every
        # commit is decided by its read-back, so the failure is detected, the
        # rollback restores all three keys with verified read-backs and the
        # call reports failure while the stored triple is EXACTLY the
        # snapshot: brightness 30, clockformat 0 and the rgborder key
        # faithfully absent again.
        self.assertEqual(self.run_store(3, 16), "store fail b=30 c=0 o=-")

    def test_failed_restore_stays_unverified_and_never_applies(self):
        # The clockformat commit failed and the BRIGHTNESS rollback write
        # itself cannot be persisted (key b bit 2): the result stays
        # unverified (the stored state is visibly mixed — brightness still
        # holds the offered 80) and such an outcome must never be reported as
        # success, which is what keeps the live state untouched (pinned
        # structurally below).
        self.assertEqual(self.run_store(3, 20), "store fail b=80 c=0 o=-")

    def test_rollback_removes_freshly_created_keys(self):
        # Nothing was stored before: a failed commit must leave the
        # namespace exactly as it was — empty, not a lone written value.
        self.assertEqual(self.run_store(0, 16), "store fail b=- c=- o=-")

    def test_a_lying_commit_write_is_caught_by_read_back(self):
        # The clockformat commit write persists a DIFFERENT byte and answers
        # as if fine (key c bit 1): only the read-back decides, the rollback
        # follows, the result is failure with the exact previous triple.
        self.assertEqual(self.run_store(3, 32), "store fail b=30 c=0 o=-")

    def test_a_lying_restore_write_is_caught_by_read_back(self):
        # Same discipline on the rollback: a restore that persists another
        # byte is detected and the outcome stays unverified.
        self.assertEqual(self.run_store(3, 24), "store fail b=31 c=0 o=-")

    def test_rollback_all_writes_run_even_after_an_earlier_failure(self):
        # Every restore is attempted even when an earlier one cannot be
        # verified: the clockformat key is still restored here even though
        # the brightness rollback lied.
        self.assertEqual(self.run_store(3, 25), "store fail b=31 c=0 o=-")

    def test_rollback_removal_that_does_not_take_effect_stays_unverified(self):
        # With NOTHING stored before, the commit of the clockformat key
        # cannot be persisted and the rollback must remove the freshly
        # created brightness key; when that removal does not take effect
        # (removal-b bit), the rollback cannot be verified and the
        # transaction reports failure (unverified), never success.
        self.assertEqual(self.run_store(0, 4112), "store fail b=80 c=- o=-")

    # --- the order key inside the same transaction (S7 on the LAN page) ---

    def test_order_commit_failure_restores_the_exact_previous_triple(self):
        # rgborder stored as 5 (BGR) up front; the ORDER commit write cannot
        # be persisted (key o bit 0): the failure is caught by read-back and
        # every key — including the failing one — is restored and verified.
        self.assertEqual(self.run_store(7, 256, 80, 1, 0), "store fail b=30 c=0 o=5")

    def test_order_commit_failure_restores_the_absent_key(self):
        # The order key was NOT stored before; the rgborder commit cannot be
        # persisted; the rollback removes it again so the namespace ends
        # exactly as it began (absence fidelity for the third key too).
        self.assertEqual(self.run_store(3, 256, 80, 1, 4), "store fail b=30 c=0 o=-")

    def test_order_rollback_failure_stays_unverified_and_never_applies(self):
        # The clockformat commit cannot be persisted AND the ORDER restore
        # write itself cannot be persisted (key o bit 2): the offered order
        # stays stored (visibly mixed) and the transaction reports failure,
        # never success.
        self.assertEqual(self.run_store(7, 1040, 80, 1, 0), "store fail b=30 c=0 o=0")

    def test_a_lying_order_restore_is_caught_by_read_back(self):
        # The order restore persists a different byte (key o bit 3): the
        # result stays unverified with the lying byte visible.
        self.assertEqual(self.run_store(7, 2064, 80, 1, 0), "store fail b=30 c=0 o=4")

    def test_order_removal_that_does_not_take_effect_stays_unverified(self):
        # The order key was NOT stored before; the clockformat commit cannot
        # be persisted; the rollback must remove the freshly created rgborder
        # key; when that removal does not take effect (removal-o bit) the
        # rollback cannot be verified and the transaction reports failure
        # (unverified), never success — the restored snapshots stay visible.
        self.assertEqual(self.run_store(3, 16400, 80, 1, 0), "store fail b=30 c=0 o=0")


class ConfigWiringTests(unittest.TestCase):
    """Structural wiring pins over src/main.cpp only: no compilation, so a
    wrong wiring is RED even while the functional rig does not build."""

    def source(self):
        return (ROOT / "src/main.cpp").read_text()

    def test_config_length_parsing_waits_for_the_complete_header_block(self):
        # Blocker 1 pin: only the request LINE is available at the first-line
        # classification, so nothing there may parse the length or start the
        # body — a real POST would answer 411 forever. The length parse lives
        # in the handler that runs after the \r\n\r\n completion check, and
        # the receiver parser never sees a /config request.
        source = self.source()
        first = source.split("static bool handleConfigRequestLine() {", 1)[1]
        first = first[:first.index("\n}")]
        self.assertNotIn("parseContentLength", first)
        self.assertNotIn("readingBody = true", first)
        begin = source.split("static void beginConfigSave() {", 1)[1]
        begin = begin[:begin.index("\n}")]
        self.assertIn("panel_settings::parseContentLength(headers)", begin)
        self.assertIn("readingBody = true", begin)
        poll = source.split("static void pollHttp() {", 1)[1]
        poll = poll[:poll.index("\n}")]
        firstLine = poll.index("handleConfigRequestLine()")
        completion = poll.index('if (headers.size() < 4 ||')
        dispatch = poll.index("beginConfigSave();")
        parse = poll.index("receiver::parseHeaders(headers, request)")
        self.assertLess(firstLine, completion)
        self.assertLess(completion, dispatch)
        self.assertLess(dispatch, parse)

    def test_stalled_config_request_times_out_with_the_settings_page(self):
        # A stalled /config request must answer the re-rendered settings page
        # with the reason (the settings-surface rejection rule), while every
        # other request keeps the generic JSON timeout body and its exact
        # current behaviour. Neither timeout path touches stored or live
        # settings.
        source = self.source()
        poll = source.split("static void pollHttp() {", 1)[1]
        poll = poll[:poll.index("\n}")]
        branch = poll[poll.index("if (millis() - requestStarted > REQUEST_TIMEOUT_MS)"):]
        branch = branch[:branch.index("return;")]
        self.assertIn("if (configSurface)", branch)
        self.assertIn(
            'replyHtml(408, configPageWithError("Request timed out."))', branch)
        self.assertIn('reply(408, "{\\\"error\\\":\\\"timeout\\\"}")', branch)
        # The settings surface is marked at the first-line classification and
        # cleared with the request state, so only classified /config requests
        # ever take the HTML path.
        classify = source.split("static bool handleConfigRequestLine() {", 1)[1]
        classify = classify[:classify.index("\n}")]
        self.assertLess(
            classify.index("if (route == panel_settings::Route::Other)"),
            classify.index("configSurface = true;"))
        close = source.split("static void closeRequest() {", 1)[1]
        close = close[:close.index("\n}")]
        self.assertIn("configSurface = false;", close)

    def test_recorded_wiring_pins(self):
        # Secondary evidence only (the functional contracts drive the same
        # pure helpers main.cpp wires). These pins hold that the firmware
        # actually wires the settings surface and that the receiver parser
        # is untouched.
        source = self.source()
        self.assertLess(source.index("panel_settings::routeOf("),
                        source.index("receiver::parseHeaders(headers, request)"))
        self.assertIn("configFirstLineChecked = true;", source)
        self.assertIn("configFirstLineChecked = false;", source)
        self.assertIn("configBody.push_back", source)
        self.assertIn("applyConfigSave()", source)
        self.assertEqual(source.count("panel::setBrightnessPercent("), 2)
        self.assertIn("loadPanelSettings();", source)
        self.assertEqual(source.count("storePanelSettings("), 2)
        self.assertEqual(source.count("clockFormat);"), 1)
        self.assertEqual(source.count("panel::brightnessScale())"), 1)
        self.assertLess(source.index("panel::brightnessScale())"),
                        source.index("panel::brightnessPercent())"))

        # The DOWN reset scope is unchanged: no settings key is ever stored
        # or removed by the reset routine (the header only names brightness
        # in prose as an out-of-scope value).
        reset = (ROOT / "include/wifi_credentials_reset.h").read_text()
        self.assertNotIn('"brightness"', reset)
        self.assertNotIn('"clockformat"', reset)
        self.assertNotIn('"rgborder"', reset)
        # Every drawing consumer still scales through the one helper.
        self.assertEqual(source.count("panel::color("), 5)
        self.assertIn("panel::color(",
                      (ROOT / "include/firmware_version.h").read_text())
        self.assertIn("panel::color(",
                      (ROOT / "include/diagnostic_status.h").read_text())

    def test_live_values_apply_only_in_the_verified_store_branch(self):
        # Blocker 2 pin: `applyConfigSave` may touch the runtime state and
        # the panel ONLY inside the if of the verified storePanelSettings —
        # an unverified (or mixed-rollback) store applies nothing.
        source = self.source()
        body = source.split("static void applyConfigSave() {", 1)[1]
        body = body[:body.index("\n}")]
        pre = body.split("if (storePanelSettings", 1)[0]
        self.assertNotIn("panel::setBrightnessPercent(", pre)
        self.assertNotIn("clockFormat =", pre)
        # The board only ever restarts for an ACTUAL change of the channel
        # order; a rejected value, a malformed body or an unverified store
        # must not even reach the store.
        self.assertNotIn("ESP.restart", pre)
        self.assertLess(body.index('"Unknown colour order."'),
                        body.index("if (storePanelSettings"))
        applied = body.split(
            "if (storePanelSettings(brightness.percent, clock.format, order.value)) {",
            1)[1]
        applied = applied[:applied.index("\n  }")]
        for wired in ("panel::setBrightnessPercent(",
                      "clockFormat = clock.format;", "presentDisplay();"):
            self.assertIn(wired, applied)
        # Restart only when the order value actually changed: the driver
        # fixes the pins at construction, so the board reboots to apply it —
        # and never otherwise.
        self.assertIn("order.value != channelOrder", applied)
        self.assertIn("configSavedPage(true)", applied)
        self.assertIn("ESP.restart()", applied)
        self.assertEqual(applied.count("ESP.restart()"), 1)
        # The success page for an unchanged order must not mention a restart.
        self.assertIn("configSavedPage()", applied)
        # The store itself is the host-tested three-key transaction, adapted
        # onto Preferences with absence-distinguishing reads and no trusted
        # put.
        self.assertIn("panel_settings::storeSettings(store, percent, clock, order)",
                      source)
        self.assertIn("panel_settings::SettingsStore", source)
        self.assertIn("preferences_.getUChar(key, 255)", source)


if __name__ == "__main__":
    unittest.main()
