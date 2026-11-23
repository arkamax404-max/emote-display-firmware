import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_serial_protocol import msvc_vcvars

ROOT = pathlib.Path(__file__).resolve().parents[1]

# The AP password alphabet: only glyphs the classic 5x7 GFX font draws
# (printable ASCII), without the visually confusable pairs 0/O and 1/I/L.
AP_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
CONFUSABLES = "0oO1iIlL"


def feed(text):
    if isinstance(text, str):
        text = text.encode("latin-1")
    return text.hex()  # '-' later means "empty value" in CLI arguments


class PortalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.policy = cls.work / "portal_policy"
        cls.page = cls.work / "portal_page"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        source = str(ROOT / "tests/portal_test.cpp")
        include = str(ROOT / "include")
        if cxx:
            for binary in (cls.policy, cls.page):
                subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                                "-I", include, source, "-o", str(binary)], check=True)
        else:
            if not msvc_vcvars():
                raise unittest.SkipTest("no host C++ compiler available")
            bat = cls.work / "compile.bat"
            bat.write_text(
                '@echo off\r\n'
                'call "{}" >nul\r\n'
                'cl /nologo /EHsc /std:c++14 /W3 /WX /D_CRT_SECURE_NO_WARNINGS '
                '/I "{}" "{}" /Fe:"{}" /Fo:"{}\\\\PortalTest.obj"\r\n'.format(
                    msvc_vcvars(), include, source, cls.policy, cls.work),
                encoding="ascii")
            subprocess.run(["cmd", "/c", str(bat)], check=True, capture_output=True)
            shutil.copyfile(cls.policy, cls.page)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_script(self, lines, mode="policy"):
        binary = self.policy if mode == "policy" else self.page
        result = subprocess.run([str(binary), mode],
                                input=("\n".join(lines) + "\n").encode("latin-1"),
                                capture_output=True, check=True)
        return result.stdout.decode("latin-1").splitlines()

    # --- T1: bounded boot retry schedule ------------------------------------

    def test_boot_retry_timeline_exact(self):
        # 4 attempts of 10 s separated by 5 s pauses; reconnects at 15/30/45 s
        # (never inside a pause), exhaustion exactly at 55 s, attempt one
        # already running from boot.
        lines = ["boot 1000",
                 "update 1000 0", "update 10999 0",   # attempt one window
                 "update 11000 0", "update 15999 0",  # pause one: no retry
                 "update 16000 0", "update 30999 0",  # attempt two, then pause
                 "update 31000 0", "update 45999 0",
                 "update 46000 0", "update 54999 0",
                 "update 55000 0",                    # attempt four ends, not yet out
                 "update 56000 0", "update 60000 0"]  # exhaustion exactly at 55 s
        out = self.run_script(lines)
        self.assertEqual(out, [
            "boot attempts=1",
            "tick wait attempts=1", "tick wait attempts=1",
            "tick wait attempts=1", "tick wait attempts=1",
            "tick retry attempts=2", "tick wait attempts=2",
            "tick retry attempts=3", "tick wait attempts=3",
            "tick retry attempts=4", "tick wait attempts=4",
            "tick wait attempts=4",
            "tick exhausted attempts=4", "tick exhausted attempts=4"])

    def test_exhaustion_is_sticky_until_reboot(self):
        out = self.run_script(["boot 1000", "update 56000 0", "update 120000 0",
                               "update 200000 0"])
        self.assertEqual(out, ["boot attempts=1"] + ["tick exhausted attempts=4"] * 3)

    def test_connected_never_advances_boot_retry(self):
        # A successful attempt ends the boot phase in the caller; the policy
        # never schedules another reconnect from a connected state.
        out = self.run_script(["boot 1000", "update 2000 1", "update 20000 1",
                               "update 100000 1"])
        self.assertEqual(out, ["boot attempts=1"] + ["tick wait attempts=1"] * 3)

    # --- T1: USB gate + portal decision -------------------------------------

    def test_usb_host_blocks_portal_in_every_state(self):
        # Enumeration means a computer is attached: provisioning via USB is
        # authoritative and the portal never opens, even with attempts
        # exhausted or no stored credentials at all.
        self.assertEqual(self.run_script(["decide 1 0 0"]), ["decide none"])
        self.assertEqual(self.run_script(["decide 1 0 1"]), ["decide none"])
        self.assertEqual(self.run_script(["decide 1 1 1"]), ["decide none"])

    def test_no_usb_no_credentials_opens_portal_without_attempts(self):
        # No SSID stored means nothing to exhaust: the portal opens
        # immediately when no USB host is enumerated.
        self.assertEqual(self.run_script(["decide 0 0 0"]), ["decide open"])
        self.assertEqual(self.run_script(["decide 0 0 1"]), ["decide open"])

    def test_no_usb_exhausted_credentials_open_portal(self):
        self.assertEqual(self.run_script(["decide 0 1 1"]), ["decide open"])
        self.assertEqual(self.run_script(["decide 0 1 0"]), ["decide none"])

    # --- T3 policy side: credential review + outcome mapping ----------------

    def test_credential_review_mirrors_serial_rules(self):
        # Same rules the serial console enforces: SSID 1-32 bytes, password
        # 8-63 bytes or empty.
        cases = [
            (feed("myssid"), feed("pass12345"), True),
            ("-", feed("pass12345"), False),              # no SSID
            (feed("a" * 32), feed("pass12345"), True),     # max SSID
            (feed("a" * 33), feed("pass12345"), False),    # 33-byte SSID
            (feed("net"), "-", True),                      # empty password
            (feed("net"), feed("p" * 8), True),            # min password
            (feed("net"), feed("p" * 63), True),           # max password
            (feed("net"), feed("p" * 7), False),           # 7-byte password
            (feed("net"), feed("p" * 64), False),          # 64-byte password
        ]
        for ssid_hex, pass_hex, expected in cases:
            with self.subTest(ssid_len=len(ssid_hex) // 2, pass_len=len(pass_hex) // 2):
                out = self.run_script(["check " + ssid_hex + " " + pass_hex])
                if expected:
                    self.assertEqual(out, ["check ok"])
                else:
                    self.assertEqual(len(out), 1)
                    self.assertTrue(out[0].startswith("check invalid "), out)

    def test_credential_review_error_strings_are_pinned(self):
        self.assertEqual(
            self.run_script(["check " + feed("a" * 33) + " " + feed("pass12345")]),
            ["check invalid SSID length"])
        self.assertEqual(
            self.run_script(["check " + feed("net") + " " + feed("p" * 7)]),
            ["check invalid password length"])

    def test_validation_outcome_mapping(self):
        # Connected within the 20 s window: persist + restart. Timeout: the
        # portal stays open and stored credentials are untouched.
        self.assertEqual(self.run_script(["conclude 1", "conclude 0"]),
                         ["outcome save", "outcome portal"])

    # --- T2 protocol shaping: request line + form body -----------------------

    def test_request_line_parsing(self):
        cases = [
            (b"GET / HTTP/1.1", "req get /"),
            (b"GET /generate_204 HTTP/1.0", "req get /generate_204"),
            (b"GET /generate_204?x=1 HTTP/1.1", "req get /generate_204?x=1"),
            (b"POST /save HTTP/1.1", "req post /save"),
            (b"POST /save HTTP/1.0", "req post /save"),
            (b"BREW /x HTTP/1.1", "req error"),     # unknown method
            (b"GET /x HTTP/2", "req error"),        # unsupported version
            (b"POST /save", "req error"),           # malformed line
            (b"GET  HTTP/1.1", "req error"),        # empty path
        ]
        for raw, expected in cases:
            with self.subTest(line=raw):
                self.assertEqual(self.run_script(["req " + raw.hex()]), [expected])

    def test_form_field_decoding(self):
        # urlencoded POST body of the form: ssid and password, '+' as space,
        # %XX escapes, unknown or missing keys malformed.
        self.assertEqual(
            self.run_script(["form " + feed(b"ssid=Alpha&password=secret")]),
            ["form ssid=Alpha pass=secret"])
        self.assertEqual(
            self.run_script(["form " + feed(b"ssid=My+Network&password=a+b")]),
            ["form ssid=My Network pass=a b"])
        self.assertEqual(
            self.run_script(["form " + feed(b"ssid=Port%2FDoom&password=%41b")]),
            ["form ssid=Port/Doom pass=Ab"])
        self.assertEqual(
            self.run_script(["form " + feed(b"ssid=net&password=")]),
            ["form ssid=net pass="])  # empty password is a valid shape
        for raw in (b"ssid=alpha",                          # password missing
                    b"ssid=alpha&password=secret&extra=1",  # unknown key
                    b"ssid=%zz&password=secret",            # malformed escape
                    b"ssid=alpha &password=secret",         # undecoded bare space
                    b"password=secret&password=secret2"):   # duplicate key
            with self.subTest(body=raw):
                out = self.run_script(["form " + feed(raw)])
                self.assertEqual(len(out), 1)
                self.assertNotIn("form ssid=", out[0])

    # --- S3: generated AP password -------------------------------------------

    def test_ap_password_is_8_chars_from_alphabet(self):
        for seed in (1, 7, 42, 987654321, 0xFFFFFFFF):
            with self.subTest(seed=seed):
                out = self.run_script(["pass %d" % seed])
                prefix, length, password = out[0].split(" ")
                self.assertEqual(prefix, "pass")
                self.assertEqual(int(length), 8)
                for char in password:
                    self.assertIn(char, AP_ALPHABET)

    def test_ap_password_excludes_confusables(self):
        # The alphabet itself never contains 0/O or 1/I/L, so no generated
        # password can either.
        self.assertEqual(self.run_script(["alphabet"]), ["alphabet " + AP_ALPHABET])
        for char in CONFUSABLES:
            self.assertNotIn(char, AP_ALPHABET)

    def test_ap_password_generation_differs_by_seed(self):
        drawn = set()
        for seed in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10):
            drawn.add(self.run_script(["pass %d" % seed])[0].split(" ")[2])
        self.assertGreater(len(drawn), 1)  # injected randomness really draws

    # --- T2/T3: portal form page ----------------------------------------------

    def test_form_page_has_fields_and_save_action(self):
        raw = self.run_script(["page "], mode="page")[0]
        html = raw[len("page "):].replace("\\n", "\n")
        self.assertTrue(raw.startswith("page <!DOCTYPE html>"))
        self.assertIn('method="POST"', html)
        self.assertIn('action="/save"', html)
        self.assertIn('name="ssid"', html)
        self.assertIn('type="password"', html)
        self.assertIn('name="password"', html)

    def test_form_page_never_carries_a_password_value(self):
        # Whether fresh or after an error the inputs are never pre-filled:
        # neither the offered passphrase nor the AP password is echoed back.
        raw = self.run_script(["page " + feed(b"some error")], mode="page")[0]
        html = raw[len("page "):].replace("\\n", "\n")
        self.assertNotIn("value=", html)
        self.assertIn("some error", html)

    def test_error_block_is_html_escaped(self):
        raw = self.run_script(["page " + feed(b'Trying <net> & "pass"')],
                              mode="page")[0]
        html = raw[len("page "):]
        self.assertIn("Trying &lt;net&gt; &amp; &quot;pass&quot;", html)
        self.assertNotIn("<net>", html)


# --- structural pins on the firmware wiring (T4) ----------------------------

class MainWiringStructuralTests(unittest.TestCase):
    """Secondary evidence only, following the test_main_wiring_structural_
    callsites precedent in tests/test_serial_ingress.py: the functional
    pins are the policy/page rig above; these check main.cpp actually wires
    the portal the way the implementation decisions require."""

    def setUp(self):
        self.source = (ROOT / "src/main.cpp").read_text()

    def test_usb_gate_uses_tinyusb_enumeration(self):
        self.assertIn("tud_mounted()", self.source)

    def test_softap_is_wpa2_with_generated_password_and_bounds(self):
        self.assertEqual(self.source.count("WiFi.softAP("), 1)
        call = self.source.split("WiFi.softAP(", 1)[1].split(");", 1)[0]
        self.assertIn("portal::AP_SSID", call)
        self.assertNotIn('"",', call)  # never an open AP
        self.assertIn("portal::AP_CHANNEL", call)
        self.assertIn("portal::AP_MAX_CLIENTS", call)

    def test_portal_server_is_separate_and_mutually_exclusive(self):
        # Dedicated portal server on port 80; never begun while the station
        # server is up (its begin is guarded by !listening), and the station
        # server never begun while the portal owns the loop (early return).
        self.assertIn("static WiFiServer portalServer(80);", self.source)
        station = self.source.split("static void pollWifi() {", 1)[1]
        self.assertIn("if (portalActive) return;", station)
        self.assertIn("if (!listening) {", station)
        portal = self.source.split("static void openPortal()", 1)[1]
        self.assertIn("!listening", portal)

    def test_dns_responder_polled(self):
        self.assertIn("DNSServer", self.source)
        self.assertIn("dnsServer.processNextRequest()", self.source)

    def test_panel_shows_portal_identity_and_password(self):
        # The PORTAL / 192.168.4.1 message plus the AP-label + password
        # slide, through the existing displayMessage path only.
        self.assertGreaterEqual(self.source.count('displayMessage("PORTAL"'), 1)
        self.assertEqual(self.source.count('displayMessage("AP: GENTLE"'), 1)
        for label in ('"192.168.', '"4.1"'):
            self.assertIn(label, self.source)

    def test_offered_passphrase_zeroed_and_never_logged(self):
        self.assertIn("static char offeredPassword[portal::AP_PASSWORD_MAX + 1];",
                      self.source)  # 63 + NUL, the serial-rule limit
        zero = self.source.split("static void zeroOffered()", 1)[1]
        zero = zero[:zero.index("\n}")]
        self.assertIn("memset", zero)
        # No print statement can pass the offered buffers to Serial.
        self.assertIsNone(self.serialPrintMentioning("offeredSsid"))
        self.assertIsNone(self.serialPrintMentioning("offeredPassword"))

    def test_validation_window_and_ap_sta(self):
        self.assertIn("WiFi.mode(WIFI_AP_STA)", self.source)
        self.assertIn("portal::VALIDATION_MS", self.source)

    def test_health_exposes_portal_identity_never_secrets(self):
        # Both JSON literals are written with C escape sequences in source.
        self.assertIn('\\"portal\\":false', self.source)   # station /health
        self.assertIn('\\"portal\\":true', self.source)    # portal /health
        # The generated password reaches the panel, never /health and never
        # the serial log.
        self.assertGreaterEqual(self.source.count("apPassword"), 2)
        self.assertIsNone(self.serialPrintMentioning("apPassword"))

    @staticmethod
    def serialPrintMentioning(name):
        # Any Serial.print/printf/println whose argument list (up to the
        # terminating semicolon) mentions the variable. Comments between
        # print sites are deliberately ignored.
        return re.search(r"Serial\.printf?[^;]*?" + re.escape(name) + r"\b",
                         (ROOT / "src/main.cpp").read_text())

    def test_no_regression_pins(self):
        # Runtime reconnect behaviour unchanged; exactly the two pre-existing
        # Serial.read() sites remain (console provisioning and binary frames);
        # the portal adds none.
        self.assertIn("millis() - lastRetry >= 15000", self.source)
        self.assertIn("WiFi.reconnect()", self.source)
        self.assertEqual(self.source.count("Serial.read()"), 2)
        self.assertIn("serialDiag.draw(matrix);", self.source)
        self.assertIn("serialRxRejected", self.source)

    def test_bodyless_get_is_served_before_content_length(self):
        # Browsers and OS captive probes send GET without Content-Length; the
        # requirement must come after the GET dispatch or the portal answers
        # 411 and stays unreachable.
        poll = self.source.split("static void pollPortal() {", 1)[1]
        poll = poll[:poll.index("\n}")]
        self.assertLess(poll.index("portal::parseRequestLine"),
                        poll.index("parsePortalContentLength("))
        self.assertIn("if (head.ok && !head.isPost)", poll)

    def test_offered_body_wiped_from_the_request_buffer(self):
        # clear() drops the size but keeps the bytes: the raw form body (with
        # the passphrase) must be overwritten deliberately.
        self.assertIn("static void wipeRequestBuffer()", self.source)
        wipe = self.source.split("static void wipeRequestBuffer()", 1)[1]
        wipe = wipe[:wipe.index("\n}")]
        self.assertIn("volatile char *bytes", wipe)
        self.assertIn("bytes[i] = 0", wipe)
        # Exactly one clear() survives, inside the wiping helper, and every
        # reset path goes through it instead.
        self.assertEqual(self.source.count("portalRequest.clear();"), 1)
        self.assertEqual(self.source.count("wipeRequestBuffer();"), 3)

    def test_credential_storage_rolls_back_a_partial_write(self):
        # S4 promises the stored credentials stay untouched when a save fails;
        # a half-applied password/ssid pair would break that promise.
        store = self.source.split("static bool storeCredentials", 1)[1]
        store = store[:store.index("\n}")]
        self.assertIn('previousSsid', store)
        self.assertIn('previousPassword', store)
        self.assertIn('preferences.remove(', store)


if __name__ == "__main__":
    unittest.main()

