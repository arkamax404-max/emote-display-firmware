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
        for percent in range(25, 81, 5):
            lines.append(f"scale {percent}")
            expected.append(f"scale {percent * 255 // 100}")
        self.assertEqual(self.run_script(lines), expected)

    def test_exactly_the_allowed_set_validates(self):
        valid = {25 + 5 * step for step in range(12)}
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
    # T4a: the form originally required six keys — the original three plus
    # the clock duration, the weather duration and the temperature unit.
    # S14: a seventh, the weather switch `weatheron` ("on"/"off").

    def test_form_parses_all_seven_required_keys(self):
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=45&clock=12h&order=GRB&clocksecs=20&weathersecs=10"
            "&units=C&weatheron=on")]),
            ["form ok b=45 c=12h o=GRB s=20 w=10 u=C n=on"])
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=80&clock=24h&order=rgb&clocksecs=120&weathersecs=5"
            "&units=F&weatheron=off")]),
            ["form ok b=80 c=24h o=rgb s=120 w=5 u=F n=off"])

    def test_form_url_rules_mirroring_the_portal(self):
        # %XX decoding for the keys; '+' is a space; bare spaces and control
        # bytes are rejected, not coerced.
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=%34%35&clock=12h&order=BRG&clocksecs=10&weathersecs=5"
            "&units=F&weatheron=on")]),
            ["form ok b=45 c=12h o=BRG s=10 w=5 u=F n=on"])
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=+10&clock=12h&order=RGB&clocksecs=10&weathersecs=5"
            "&units=C&weatheron=off")]),
            ["form ok b= 10 c=12h o=RGB s=10 w=5 u=C n=off"])  # '+' -> space, review rejects below
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=1 0&clock=12h&order=RGB&clocksecs=10&weathersecs=5"
            "&units=C&weatheron=on")]),
            ["form error"])
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=10&clock=12%68&order=BGR&clocksecs=10&weathersecs=5"
            "&units=C&weatheron=on")]),
            ["form ok b=10 c=12h o=BGR s=10 w=5 u=C n=on"])
        # An empty or unreviewable weather-switch VALUE still parses; the
        # per-value review rejects it with its reason below.
        self.assertEqual(self.run_script(["form " + feed(
            "brightness=45&clock=12h&order=RGB&clocksecs=20&weathersecs=10"
            "&units=C&weatheron=")]),
            ["form ok b=45 c=12h o=RGB s=20 w=10 u=C n="])

    def test_form_rejects_missing_unknown_and_duplicate_keys(self):
        complete = ("brightness=45&clock=12h&order=RGB&clocksecs=20"
                    "&weathersecs=10&units=C&weatheron=on")
        pairs = [complete.replace("brightness=45&", ""),           # missing brightness
                 complete.replace("clock=12h&", ""),               # missing clock
                 complete.replace("order=RGB&", ""),               # missing order
                 complete.replace("clocksecs=20&", ""),            # missing clocksecs
                 complete.replace("weathersecs=10&", ""),          # missing weathersecs
                 complete.replace("&units=C", ""),                 # missing units
                 complete.replace("&weatheron=on", ""),            # missing weatheron
                 complete.replace("clocksecs=20", "clocksecs=20&extra=1"),
                 complete.replace("order=RGB", "order=RGB&order=RBG"),
                 complete.replace("units=C", "units=C&units=F"),
                 complete.replace("weatheron=on", "weatheron=on&weatheron=off"),
                 complete.replace("brightness=45", "brightness45=45"),
                 complete.replace("clocksecs=20", "clocksecs=20&clocksecs=30")]
        for body in pairs + ["", "brightness"]:
            with self.subTest(body=body):
                self.assertEqual(self.run_script(["form " + feed(body)]),
                                 ["form error"])

    def test_brightness_review_is_the_documented_allowed_set(self):
        for value, ok in (("25", 25), ("30", 30), ("75", 75), ("80", 80),
                          ("030", 30), ("10", None), ("15", None), ("20", None),
                          ("9", None), ("76", None), ("81", None), ("85", None),
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

    def test_orientation_route_classified_on_the_exact_post_lines(self):
        # T17: the recalibration action has its own form and its own exact
        # POST route; a GET of the path stays Other (like the city route).
        self.assertEqual(
            self.run_script(["route " + feed("POST /config/orientation HTTP/1.1")]),
            ["route orient"])
        self.assertEqual(
            self.run_script(["route " + feed("POST /config/orientation HTTP/1.0")]),
            ["route orient"])
        for line in ("GET /config/orientation HTTP/1.1",
                     "POST /config/orientationx HTTP/1.1",
                     "POST /config/orientation HTTP/9.9",
                     "post /config/orientation HTTP/1.1",
                     "POST /config/orientation"):
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
        for percent in range(25, 81, 5):
            self.assertIn(f'value="{percent}"', out)
        self.assertIn('value="30" selected', out)
        self.assertNotIn('value="25" selected', out)
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

    # --- T17: the orientation recalibration action ---------------------------

    def test_page_carries_the_orientation_recalibration_in_its_own_form(self):
        # Its own form and route, with the stillness requirement and its
        # 350 ms reason said plainly on the page itself.
        out = self.run_script(["pagew 30 24h RGB C 20 10 " + feed("Sevilla")
                               + " -"])[0]
        self.assertIn('action="/config/orientation"', out)
        self.assertIn('method="POST"', out)
        self.assertIn('action="/config/orientation"><button type="submit">'
                      'Recalibrate orientation</button></form>', out)
        self.assertIn("restart", out)
        self.assertIn("350 ms", out)
        self.assertIn("still", out)
        # The legacy page command renders it too.
        self.assertIn('action="/config/orientation"',
                      self.run_script(["page 30 24h RGB -"])[0])

    def test_orientation_cleared_page_says_restart_and_stillness(self):
        out = self.run_script(["orisaved"])[0]
        self.assertIn("cleared", out)
        self.assertIn("re-learns", out)
        self.assertIn("restart", out)
        self.assertIn("350 ms", out)
        self.assertIn("still", out)
        self.assertIn('href="/config"', out)

    def test_orientclear_removes_only_updir(self):
        # T17: the verified single-key transaction — exactly `updir`, never
        # the credentials (never in this surface at all), never the panel
        # triple. Only updir stored: it is cleared and the rest was absent.
        self.assertEqual(self.run_script(["orientclear 8 0"]),
                         ["orientclear ok updir=- b=- c=- o=-"])
        # Everything stored: only the orientation byte goes; the three panel
        # keys survive byte-identically (30 %, H24 = 0, BGR = 5).
        self.assertEqual(self.run_script(["orientclear 15 0"]),
                         ["orientclear ok updir=- b=30 c=0 o=5"])
        # Nothing stored: already the goal state, verified success, no write.
        self.assertEqual(self.run_script(["orientclear 0 0"]),
                         ["orientclear ok updir=- b=- c=- o=-"])

    def test_orientclear_reports_a_removal_that_does_not_take_effect(self):
        # The removal is decided by read-back alone: when the store keeps the
        # key, the outcome is failure (the adapter must not restart) and the
        # panel keys stay untouched either way.
        self.assertEqual(self.run_script(["orientclear 8 32768"]),
                         ["orientclear fail updir=3 b=- c=- o=-"])
        self.assertEqual(self.run_script(["orientclear 15 32768"]),
                         ["orientclear fail updir=3 b=30 c=0 o=5"])

    def test_settings_store_commands_never_touch_the_orientation_key(self):
        # Explicit key-set pin: with updir seeded (present bit 3) the panel
        # and weather transactions still store exactly their own keys and
        # leave the orientation byte exactly as it was.
        self.assertEqual(self.run_store(15, 0, 45, 1, 3), "store ok b=45 c=1 o=3")
        self.assertEqual(self.run_store(15, 16), "store fail b=30 c=0 o=5")
        self.assertEqual(self.run_script(["storew 15 0"])[0],
                         "storew ok s=60 w=30 u=1")

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
        # T4b: the page render now carries the live weather values as well;
        # the previous exact-arg pin ("clockFormat);") describe the retired
        # four-argument call.
        self.assertEqual(source.count("weatherUnit, clockSecs,"), 1)
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
        # Every drawing consumer still scales through the one helper. T4b
        # adds the weather screen as the sixth call site.
        self.assertEqual(source.count("panel::color("), 6)
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
        self.assertNotIn("weatherOn =", pre)
        # The board only ever restarts for an ACTUAL change of the channel
        # order; a rejected value, a malformed body or an unverified store
        # must not even reach the store.
        self.assertNotIn("ESP.restart", pre)
        self.assertLess(body.index('"Unknown colour order."'),
                        body.index("if (storePanelSettings"))
        applied = body.split(
            "if (storePanelSettings(brightness.percent, clock.format, order.value,\n"
            "                         clockDuration.seconds, weatherDuration.seconds,\n"
            "                         unit.unit, weatherSwitch.on)) {",
            1)[1]
        applied = applied[:applied.index("\n  }")]
        for wired in ("panel::setBrightnessPercent(",
                      "clockFormat = clock.format;", "presentDisplay();",
                      "clockSecs = clockDuration.seconds;",
                      "weatherSecs = weatherDuration.seconds;",
                      "weatherUnit = unit.unit;",
                      "weatherOn = weatherSwitch.on;"):
            self.assertIn(wired, applied)
        # S14: switching back on with a missing or stale forecast books an
        # immediate attempt — a fresh scheduler fetches on the very first
        # tick — while a still-fresh forecast keeps the 30-minute cadence.
        self.assertIn("weather_fetch::Scheduler", applied)
        self.assertIn("weatherScheduler.stale(millis())", applied)
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
        # put. T4b: POST /config now stores all six keys as ONE verified
        # transaction (the combined storeAllSettings over the same
        # storeUCharKeys core), and the typed weather-city overrides are on
        # the same store.
        self.assertIn("panel_settings::storeAllSettings(", source)
        self.assertIn("panel_settings::SettingsStore", source)
        self.assertIn("preferences_.getUChar(key, 255)", source)


class OrientationResetWiringTests(unittest.TestCase):
    """T17 (S11) wiring pins over src/main.cpp: the /config orientation
    recalibration route lingers like the other settings POSTs, the verified
    clear is the pure panel-settings transaction over the shared store, and
    the restart happens ONLY after a verified clearing — a failure answers
    the settings page with the reason and changes nothing. The AP portal
    page and the ORIENT console command stay exactly as they are."""

    def source(self):
        return (ROOT / "src/main.cpp").read_text()

    def test_orient_route_lingers_like_the_other_settings_posts(self):
        # First-line classification only; the bounded length parse happens
        # at the complete header block, and the receiver parser never sees
        # the request. The latched state is cleared with the request state.
        source = self.source()
        classify = source.split("static bool handleConfigRequestLine() {", 1)[1]
        classify = classify[:classify.index("\n}")]
        self.assertIn("panel_settings::Route::OrientSave", classify)
        self.assertIn("orientSavePending = true;", classify)
        self.assertNotIn("parseContentLength", classify)
        self.assertIn("configSurface = true;", classify)
        close = source.split("static void closeRequest() {", 1)[1]
        close = close[:close.index("\n}")]
        self.assertIn("orientSavePending = false;", close)
        poll = source.split("static void pollHttp() {", 1)[1]
        poll = poll[:poll.index("\n}")]
        self.assertLess(poll.index("beginOrientSave();"),
                        poll.index("receiver::parseHeaders(headers, request)"))

    def test_orient_body_is_bounded_and_fieldless(self):
        # The form carries no fields: an empty body applies immediately; a
        # non-empty body still goes through the shared bounded buffer and
        # the bounded length discipline, never the PNG path.
        begin = self.source().split("static void beginOrientSave() {", 1)[1]
        begin = begin[:begin.index("\n}")]
        self.assertIn("panel_settings::parseContentLength(headers)", begin)
        self.assertIn("panel_settings::CONFIG_BODY_MAX", begin)
        self.assertIn("applyOrientSave();", begin)
        self.assertLess(begin.index("applyOrientSave();"),
                        begin.index("readingBody = true;"))
        source = self.source()
        self.assertIn("configBodyIsOrientation", source)
        self.assertIn("else if (configBodyIsOrientation) applyOrientSave();",
                      source)

    def test_restart_only_after_a_verified_clear_and_failure_says_so(self):
        # The clear is the host-tested pure transaction, the restart sits
        # inside the verified branch (exactly once), and a failed clear
        # leaves everything untouched, answers the settings page with the
        # reason and never restarts.
        source = self.source()
        self.assertIn("panel_settings::clearOrientationReference(store)", source)
        body = source.split("static void applyOrientSave() {", 1)[1]
        body = body[:body.index("\n}")]
        self.assertIn("clearOrientationReference(store)", body)
        self.assertIn("orientationClearedPage()", body)
        self.assertEqual(body.count("ESP.restart()"), 1)
        self.assertLess(body.index("clearOrientationReference(store)"),
                        body.index("ESP.restart()"))
        unverified = body.split("if (!verified)", 1)[1]
        unverified = unverified[:unverified.index("return;")]
        self.assertIn("left unchanged", unverified)
        self.assertNotIn("ESP.restart()", unverified)
        # The live orientation object is not touched by this route at all:
        # the restart re-learns from storage.
        self.assertNotIn("orientation.resetReference()", body)

    def test_ap_portal_page_and_orient_console_stay_untouched(self):
        source = self.source()
        # The portal route set keeps its exact two POST paths.
        self.assertIn('if (line.path != "/save" && line.path != "/rgborder") {',
                      source)
        portal_page = (ROOT / "include/portal_page.h").read_text()
        self.assertNotIn("/config/orientation", portal_page)
        # The ORIENT console entry stays wired, first recovery hook position.
        console = source.split(
            "static serial::ProvisionConsole provisionConsole({", 1)[1]
        console = console[:console.index("});")]
        self.assertIn("[]() { resetOrientationReference(); },", console)
        # The pure header carries the route constant the firmware classifies.
        header = (ROOT / "include/panel_settings.h").read_text()
        self.assertIn('"/config/orientation"', header)
        self.assertIn("ORIENT_PATH", header)


class WeatherSettingsTests(SettingsTests):
    """T4a (S1-S4, S6): the weather settings surface of /config — the
    persisted duration, unit and city keys with their verified
    transactions, the page extensions and the city-search intake. Inherits
    the compile/run helpers of the panel-settings suite."""

    # --- the exact allowed sets, reviewed from the form values ---------------

    def test_seconds_review_is_exactly_the_documented_set(self):
        valid = {5 + 5 * step for step in range(24)}
        lines, expected = [], []
        for value in [str(n) for n in range(0, 130)] + ["0005", "020", "",
                                                        "abc", " 5", "-5",
                                                        "5x", "1200", "1 5"]:
            lines.append("seconds " + feed(value))
            parsed = int(value) if value.isdigit() and len(value) <= 3 else None
            if parsed is not None and parsed in valid:
                expected.append("seconds ok %d" % parsed)
            else:
                expected.append("seconds error")
        self.assertEqual(self.run_script(lines), expected)

    def test_second_boundaries(self):
        # 5 and 120 are inside; 4 and 121 are just outside; 125 is beyond.
        self.assertEqual(self.run_script(["seconds " + feed("5"),
                                          "seconds " + feed("120"),
                                          "seconds " + feed("4"),
                                          "seconds " + feed("121"),
                                          "seconds " + feed("125")]),
                         ["seconds ok 5", "seconds ok 120", "seconds error",
                          "seconds error", "seconds error"])

    def test_unit_review_accepts_exactly_C_and_F(self):
        for value, out in (("C", "unitsrev ok C"), ("F", "unitsrev ok F"),
                           ("c", "unitsrev error"), ("f", "unitsrev error"),
                           ("0", "unitsrev error"), ("1", "unitsrev error"),
                           ("", "unitsrev error"), ("CF", "unitsrev error"),
                           (" C", "unitsrev error"), ("24h", "unitsrev error")):
            with self.subTest(value=value):
                self.assertEqual(self.run_script(["unitsrev " + feed(value)]), [out])

    # --- S14: the weather switch review — exactly "on" and "off" -------------

    def test_weather_switch_review_accepts_exactly_on_and_off(self):
        for value, out in (("on", "onrev ok on"), ("off", "onrev ok off"),
                           ("On", "onrev error"), ("OFF", "onrev error"),
                           ("1", "onrev error"), ("0", "onrev error"),
                           ("", "onrev error"), ("no", "onrev error"),
                           (" on", "onrev error"), ("on ", "onrev error"),
                           ("true", "onrev error")):
            with self.subTest(value=value):
                self.assertEqual(self.run_script(["onrev " + feed(value)]), [out])

    # --- stored values outside their sets fall back to the defaults ----------

    def test_stored_durations_fall_back_to_their_defaults(self):
        self.assertEqual(self.run_script(["csdef 200", "csdef 255", "csdef 0",
                                          "csdef 60", "csdef 20", "csdef 5",
                                          "csdef 150", "csdef 24"]),
                         ["csdef 20", "csdef 20", "csdef 20", "csdef 60",
                          "csdef 20", "csdef 5", "csdef 20", "csdef 20"])
        self.assertEqual(self.run_script(["wsdef 200", "wsdef 255", "wsdef 9",
                                          "wsdef 10", "wsdef 120", "wsdef 0",
                                          "wsdef 30"]),
                         ["wsdef 10", "wsdef 10", "wsdef 10", "wsdef 10",
                          "wsdef 120", "wsdef 10", "wsdef 30"])

    def test_stored_unit_falls_back_to_celsius(self):
        self.assertEqual(self.run_script(["udef 0", "udef 1", "udef 2",
                                          "udef 42", "udef 255"]),
                         ["udef C", "udef F", "udef C", "udef C", "udef C"])

    def test_stored_weather_switch_falls_back_to_on(self):
        # S14: the stored byte is 0 = off and 1 = on; an absent key (the
        # 255 sentinel the loaders read) and every invalid byte fall back
        # to the documented default, on.
        self.assertEqual(self.run_script(["odef 0", "odef 1", "odef 2",
                                          "odef 42", "odef 200",
                                          "odef 255"]),
                         ["odef off", "odef on", "odef on", "odef on",
                          "odef on", "odef on"])

    def test_stored_city_falls_back_to_sevilla(self):
        # Nothing stored: the S6 default city, so a fresh board has a city.
        self.assertEqual(self.run_script(["citydef"]),
                         ["citydef ok lat=37382830 lon=-5973170 name=Sevilla"])
        # A complete stored record survives byte-for-byte (the name, like
        # every string argument to the rig, is hex-encoded).
        self.assertEqual(
            self.run_script(["citydef 7 40416775 -3703790 " + feed("Madrid")]),
            ["citydef ok lat=40416775 lon=-3703790 name=Madrid"])
        # An out-of-range coordinate, an empty name or a missing stored flag
        # all collapse to the same default; nothing else changes (the
        # resolved record is always a whole record).
        self.assertEqual(self.run_script(["citydef 7 90000001 -5973170 " + feed("x"),
                                          "citydef 7 37382830 -180999999 " + feed("x"),
                                          "citydef 7 37382830 -5973170 -",
                                          "citydef 0 1 2 " + feed("x")]),
                         ["citydef ok lat=37382830 lon=-5973170 name=Sevilla"] * 4)
        # A stored name is re-clamped on load, UTF-8-safely: 23 letters plus
        # the two-byte N-tilde exceed the 24-byte bound, so the pair goes
        # whole and the name ends at the letters.
        self.assertEqual(self.run_script(
            ["citydef 7 37382830 -5973170 " + feed("abcdefghijklmnopqrstuvw\xc3\xb1")]),
            ["citydef ok lat=37382830 lon=-5973170 name=abcdefghijklmnopqrstuvw"])

    # --- the settings transaction over the three uchar keys ------------------

    def run_storew(self, present, fail, secs=60, weather=30, unit=1):
        return self.run_script([f"storew {present} {fail} {secs} {weather} {unit}"])[0]

    def test_storew_success_persists_all_three_values(self):
        self.assertEqual(self.run_storew(7, 0), "storew ok s=60 w=30 u=1")
        self.assertEqual(self.run_storew(7, 0, 5, 120, 0), "storew ok s=5 w=120 u=0")

    def test_storew_failed_commit_restores_the_exact_previous_triple(self):
        # Stored (20, 10, 0), offered (60, 30, F); the weathersecs commit
        # cannot be persisted: read-back decides, the rollback restores all
        # three keys and the call reports failure with the exact snapshot.
        self.assertEqual(self.run_storew(7, 16), "storew fail s=20 w=10 u=0")
        # A lying commit write is caught by read-back the same way.
        self.assertEqual(self.run_storew(7, 32), "storew fail s=20 w=10 u=0")

    def test_storew_rollback_removal_and_unverified_outcomes(self):
        # Nothing stored before; the units commit fails (key u, bit 8); the
        # rollback must leave the namespace empty.
        self.assertEqual(self.run_storew(0, 256), "storew fail s=- w=- u=-")
        # The same shape with a removal that does not take effect on the
        # freshly written clocksecs (removal bit 12, next to the
        # weathersecs commit failure bit 4): the offered clocksecs stays
        # visible and the outcome stays unverified.
        self.assertEqual(self.run_storew(0, 4112), "storew fail s=60 w=- u=-")
        # A lying restore (rollback-lie on the clock key, bit 3, alongside
        # the weathersecs commit failure, bit 4): the stored clocksecs
        # becomes 21 (the write persisted 20^1) and the outcome stays
        # unverified while every other key still restores exactly.
        self.assertEqual(self.run_storew(7, 24, 20, 15, 0),
                         "storew fail s=21 w=10 u=0")
        # The same discipline with the clock commit itself failing (bit 0):
        # every rollback write still runs, including the lying one.
        self.assertEqual(self.run_storew(7, 25), "storew fail s=21 w=10 u=0")

    # --- the combined seven-key transaction (S14 joins POST /config) ---------

    def run_storea(self, present, fail, percent=80, clock=1, order=0,
                   secs=60, weather=30, unit=1, on=1):
        return self.run_script(
            [f"storea {present} {fail} {percent} {clock} {order} {secs} "
             f"{weather} {unit} {on}"])[0]

    def test_storea_success_persists_all_seven_values(self):
        self.assertEqual(self.run_storea(127, 0),
                         "storea ok b=80 c=1 o=0 s=60 w=30 u=1 n=1")
        # A fresh namespace round-trips every key, including zero codes
        # (H24, RGB, Celsius, off) that must never read as failures.
        self.assertEqual(self.run_storea(0, 0, 25, 0, 0, 5, 120, 0, 0),
                         "storea ok b=25 c=0 o=0 s=5 w=120 u=0 n=0")
        # Offered "off" round-trips as the stored 0 next to the other keys.
        self.assertEqual(self.run_storea(127, 0, 80, 1, 3, 60, 30, 1, 0),
                         "storea ok b=80 c=1 o=3 s=60 w=30 u=1 n=0")

    def test_storea_failed_commit_restores_the_exact_previous_seven(self):
        # The weathersecs commit cannot be persisted (key w = k4, bit 16):
        # read-back decides, the rollback restores ALL seven keys, and the
        # call reports failure with the exact snapshot (off was never stored,
        # weatheron stays 1).
        self.assertEqual(self.run_storea(127, 1 << 16),
                         "storea fail b=30 c=0 o=5 s=20 w=10 u=0 n=1")
        # The switch key itself failing commit (key n = k6, bit 24); the
        # offered switch is OFF so the committed byte would differ from the
        # stored 1 — read-back decides.
        self.assertEqual(self.run_storea(127, 1 << 24, on=0),
                         "storea fail b=30 c=0 o=5 s=20 w=10 u=0 n=1")
        # A lying switch commit (bit 25), again offering the other value.
        self.assertEqual(self.run_storea(127, 1 << 25, on=0),
                         "storea fail b=30 c=0 o=5 s=20 w=10 u=0 n=1")

    def test_storea_rollback_removal_and_unverified_outcomes(self):
        # Nothing stored before; the units commit fails (key u = k5, bit 20);
        # the rollback must leave the namespace empty — absence fidelity for
        # every key including the freshly created switch.
        self.assertEqual(self.run_storea(0, 1 << 20),
                         "storea fail b=- c=- o=- s=- w=- u=- n=-")
        # A removal that does not take effect on the freshly written switch
        # (removal n bit 34, next to the weathersecs commit failure bit 16):
        # the offered switch stays visible and the outcome stays unverified;
        # weathersecs was never persisted, so it reads absent.
        self.assertEqual(self.run_storea(0, (1 << 16) | (1 << 34)),
                         "storea fail b=- c=- o=- s=- w=- u=- n=1")
        # A lying restore of the switch (bit 27) alongside the clocksecs
        # commit failure (bit 12): every restore still runs — clocksecs
        # lands exactly on its snapshot (20, it was never changed) while
        # the lying switch byte stays visible and the transaction reports
        # failure, never success.
        self.assertEqual(self.run_storea(127, (1 << 12) | (1 << 27)),
                         "storea fail b=30 c=0 o=5 s=20 w=10 u=0 n=0")

    # --- the city transaction over the typed keys -----------------------------

    def run_storec(self, present, fail, lat=40416775, lon=-3703790,
                   name="Madrid"):
        line = f"storec {present} {fail} {lat} {lon} " + feed(name)
        return self.run_script([line])[0]

    def test_storec_success_persists_the_whole_record(self):
        self.assertEqual(self.run_storec(7, 0),
                         "storec ok lat=40416775 lon=-3703790 name=Madrid")
        self.assertEqual(self.run_storec(7, 0, -5973170, 37382830, "Sevilla"),
                         "storec ok lat=-5973170 lon=37382830 name=Sevilla")

    def test_storec_failed_commit_restores_the_exact_previous_record(self):
        # Stored Sevilla, offered Madrid; the citylon commit cannot be
        # persisted (key lon, bit 4): the rollback restores all three keys,
        # including the exact previous STRING, and reports failure.
        self.assertEqual(self.run_storec(7, 16),
                         "storec fail lat=37382830 lon=-5973170 name=Sevilla")
        # The cityname commit cannot be persisted (key name, bit 8).
        self.assertEqual(self.run_storec(7, 256),
                         "storec fail lat=37382830 lon=-5973170 name=Sevilla")

    def test_storec_lies_and_rollback_restores(self):
        # A lying string commit (name kept as 'MadridX', bit 9) is caught
        # by read-back and the exact previous record comes back.
        self.assertEqual(self.run_storec(7, 512),
                         "storec fail lat=37382830 lon=-5973170 name=Sevilla")
        # A restore that cannot take effect (citylat commit noop, bit 0,
        # plus the cityname restore noop, bit 10) keeps the offered name
        # visible and the transaction unverified; the int keys still
        # restore faithfully (the citylat restore has nothing to change,
        # the citylon restore lands exactly).
        self.assertEqual(self.run_storec(7, 1025),
                         "storec fail lat=37382830 lon=-5973170 name=Madrid")
        # Nothing stored before; the cityname commit fails; the rollback
        # leaves the namespace empty.
        self.assertEqual(self.run_storec(0, 256),
                         "storec fail lat=- lon=- name=-")
        # A removal that does not take effect (removal citylat, bit 12,
        # plus cityname commit noop, bit 8) keeps the offered latitude
        # visible and stays unverified.
        self.assertEqual(self.run_storec(0, 4352),
                         "storec fail lat=40416775 lon=- name=-")

    # --- the /config page extensions ------------------------------------------

    def test_pagecarries_the_new_controls_preselected(self):
        out = self.run_script(["pagew 65 12h BRG C 60 30 " + feed("Sevilla")
                               + " -"])[0]
        # The two duration selects with every allowed value, current one
        # selected, labelled in seconds.
        self.assertIn('name="clocksecs"', out)
        self.assertIn('name="weathersecs"', out)
        for seconds in range(5, 121, 5):
            self.assertIn(f'value="{seconds}"', out)
        self.assertIn('value="60" selected', out)
        self.assertIn('value="30" selected', out)
        self.assertIn('value="120"', out)
        self.assertIn("seconds", out)
        # The unit select: C default and F, current preselected.
        self.assertIn('name="units"', out)
        self.assertIn('value="C" selected', out)
        self.assertIn('value="F">', out)
        # The city search has its own form, its own route, a bounded field
        # and the current city shown; the page states the resolved-city rule.
        self.assertIn('action="/config/city"', out)
        self.assertIn('method="POST"', out)
        self.assertIn('name="city"', out)
        self.assertIn('maxlength="64"', out)
        self.assertIn("Sevilla", out)
        self.assertIn("resolved city", out)

    def test_page_preselects_fahrenheit_and_escapes_city_markup(self):
        out = self.run_script(["pagew 30 24h RGB F 20 10 "
                               + feed("<x>&") + " -"])[0]
        self.assertIn('value="F" selected', out)
        self.assertIn('value="C">', out)
        self.assertIn('&lt;x&gt;&amp;', out)

    def test_page_error_block_still_escapes(self):
        out = self.run_script(["pagew 80 24h RGB C 20 10 " + feed("madrid")
                               + " " + feed("<e>")])[0]
        self.assertIn("&lt;e&gt;", out)

    # --- S14: the weather switch on the page ----------------------------------

    def test_page_carries_the_weather_switch_preselected(self):
        out = self.run_script(["pagew 65 12h BRG C 60 30 " + feed("Sevilla")
                               + " off -"])[0]
        # The switch beside the other weather settings: exactly the two
        # accepted values, the current one preselected.
        self.assertIn('name="weatheron"', out)
        self.assertIn('value="on">', out)
        self.assertIn('value="off" selected>', out)
        self.assertNotIn('value="on" selected>', out)
        # The page states plainly that turning it off stops the downloads
        # and hands the screen to the clock.
        self.assertIn("stops downloading the forecast", out)
        self.assertIn("clock always stays on screen", out)

    def test_page_preselects_the_switch_on_by_default(self):
        out = self.run_script(["pagew 30 24h RGB C 20 10 " + feed("Sevilla")
                               + " on -"])[0]
        self.assertIn('value="on" selected>', out)
        self.assertIn('value="off">', out)
        self.assertNotIn('value="off" selected>', out)
        # The legacy eight-argument pagew form (and the legacy page command)
        # still render the switch, preselected on.
        legacy = self.run_script(["pagew 30 24h RGB C 20 10 " + feed("Sevilla")
                                  + " -"])[0]
        self.assertIn('value="on" selected>', legacy)
        self.assertIn('name="weatheron"', legacy)
        self.assertIn('name="weatheron"', self.run_script(["page 30 24h RGB -"])[0])

    def test_old_page_command_still_renders_every_control(self):
        # PRESERVE: the previous signature keeps rendering the full extended
        # page, preselecting the defaults (Celsius, 20 s / 10 s, Sevilla).
        out = self.run_script(["page 30 24h RGB -"])[0]
        self.assertIn('name="clocksecs"', out)
        self.assertIn('name="weathersecs"', out)
        self.assertIn('name="units"', out)
        self.assertIn('value="30" selected', out)  # brightness
        self.assertIn('value="C" selected', out)
        self.assertIn('value="20" selected', out)
        self.assertIn('value="10" selected', out)
        self.assertIn("Sevilla", out)

    # --- the city-search intake -----------------------------------------------

    def test_city_route_classified_on_the_exact_post_lines(self):
        self.assertEqual(self.run_script(["route " + feed("POST /config/city HTTP/1.1")]),
                         ["route city"])
        self.assertEqual(self.run_script(["route " + feed("POST /config/city HTTP/1.0")]),
                         ["route city"])
        for line in ("GET /config/city HTTP/1.1",
                     "POST /config/cit HTTP/1.1", "POST /Config/city HTTP/1.1",
                     "POST /config/city", "post /config/city HTTP/1.1"):
            self.assertEqual(self.run_script(["route " + feed(line)]), ["route other"])

    def test_only_a_single_city_field_is_parsed(self):
        self.assertEqual(self.run_script(["cityform " + feed("city=Madrid")]),
                         ["cityform ok city=Madrid"])
        # The UTF-8 bytes print escaped by the rig (\xc3\xb1 for ñ).
        self.assertEqual(
            self.run_script(["cityform " + feed("city=A%20Coru%C3%B1a")]),
            ["cityform ok city=A Coru\\xc3\\xb1a"])
        # An EMPTY value still parses (the shared URL rules accept it, like
        # an empty open-network password); the review below rejects it.
        self.assertEqual(self.run_script(["cityform " + feed("city=")]),
                         ["cityform ok city="])
        for body in ("", "city", "city=A&city=B", "name=Madrid",
                     "city=A&extra", "city=%2", "city=A B"):
            with self.subTest(body=body):
                self.assertEqual(self.run_script(["cityform " + feed(body)]),
                                 ["cityform error"], body)

    def test_city_search_rejected_before_any_request_could_run(self):
        # The review is the FIRST decision on a submitted name: empty,
        # oversized (the 64-byte query bound) names never reach the
        # transport; the review itself is pure, so 'no request' is
        # structural here and the transport check lives in weather_fetch.
        self.assertEqual(self.run_script(["citysearch " + feed("")]),
                         ["citysearch error"])
        self.assertEqual(
            self.run_script(["citysearch " + feed("x" * 65)]),
            ["citysearch error"])
        self.assertEqual(self.run_script(["citysearch " + feed("x" * 64)]),
                         ["citysearch ready"])
        self.assertEqual(
            self.run_script(["citysearch " + feed("Sevilla")]),
            ["citysearch ready"])

    # --- the geocoding result -> stored city -----------------------------------

    def test_geocoding_result_becomes_the_stored_record(self):
        body = feed('{"results":[{"name":"Madrid","latitude":40.416775,'
                    '"longitude":-3.70379,"country_code":"ES"}]}')
        self.assertEqual(self.run_script(["cityfrom 200 " + body]),
                         ["cityfrom ok lat=40416775 lon=-3703790 name=Madrid"])

    def test_stored_name_is_clamped_utf8_safely(self):
        # The raw UTF-8 bytes of the N-tilde (the JSON source would need the
        # \u00f1 escape, which the committed parseCity rejects by design).
        body = feed('{"results":[{"name":"abcdefghijklmnopqrstuvw\xc3\xb1x",'
                    '"latitude":40.4,"longitude":-3.7}]}')
        # 23 ASCII letters + the two-byte N-tilde = 25 bytes; the trailing
        # 'x' would exceed the bound, so the whole name is cut at 23 and
        # the sequence never tears.
        self.assertEqual(self.run_script(["cityfrom 200 " + body]),
                         ["cityfrom ok lat=40400000 lon=-3700000 "
                          "name=abcdefghijklmnopqrstuvw"])

    def test_geocoding_failures_report_error_never_partial(self):
        # Every failure reports error; the caller keeps its previous record
        # because cityFromResponse only writes on success (the committed
        # parseCity contract guarantees the untouched record, and the
        # response discipline sits in front of it).
        cases = [(503, feed('{"results":[{"name":"Madrid","latitude":40.4,'
                            '"longitude":-3.7}]}')),  # bad status
                 (200, feed("")),                    # empty body
                 (200, feed("{}")),
                 (200, feed('{"error":true,"reason":"..."}')),
                 (200, feed('{"results":[]}')),      # unknown city
                 (200, "x" * (4097 * 2)),            # oversized body
                 (-1, feed('{"results":[{"name":"X","latitude":40.4,'
                           '"longitude":-3.7}]}'))]  # transport error
        for status, body in cases:
            with self.subTest(status=status, body=len(body)):
                self.assertEqual(self.run_script([f"cityfrom {status} " + body]),
                                 ["cityfrom error"])

    def test_reset_scope_stays_exactly_untouched_by_the_weather_keys(self):
        # None of the six new keys is ever in the DOWN-reset scope.
        reset = (ROOT / "include/wifi_credentials_reset.h").read_text()
        # Read-only report: the reset scope is defined by its own header and
        # unchanged; this pin documents the promise for the new keys.
        self.assertNotIn('"clocksecs"', reset)
        self.assertNotIn('"weathersecs"', reset)
        self.assertNotIn('"units"', reset)
        self.assertNotIn('"citylat"', reset)
        self.assertNotIn('"citylon"', reset)
        self.assertNotIn('"cityname"', reset)
        self.assertNotIn('"weatheron"', reset)
