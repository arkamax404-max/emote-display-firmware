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
        # S5: the SSID now arrives only from the scan's radio buttons; the
        # save action and the password field are unchanged.
        raw = self.run_script(["net " + feed(b"Alpha") + " 40", "page "], mode="page")[0]
        html = raw[len("page "):].replace("\\n", "\n")
        self.assertTrue(raw.startswith("page <!DOCTYPE html>"))
        self.assertIn('method="POST"', html)
        self.assertIn('action="/save"', html)
        self.assertIn('type="radio" name="ssid" value="Alpha" required>', html)
        self.assertIn('type="password"', html)
        self.assertIn('name="password"', html)

    def test_form_page_never_carries_a_password_value(self):
        # Whether fresh or after an error the inputs are never pre-filled:
        # neither the offered passphrase nor the AP password is echoed back.
        raw = self.run_script(["page " + feed(b"some error") + " " + feed(b"ab12cd34")],
                              mode="page")[0]
        html = raw[len("page "):].replace("\\n", "\n")
        self.assertIn('type="hidden" name="token" value="ab12cd34"', html)  # only the token carries a value
        self.assertNotIn("value=", html.split('name="password"', 1)[1].split(">", 1)[0])
        self.assertIn("some error", html)

    def test_error_block_is_html_escaped(self):
        raw = self.run_script(["page " + feed(b'Trying <net> & "pass"')],
                              mode="page")[0]
        html = raw[len("page "):]
        self.assertIn("Trying &lt;net&gt; &amp; &quot;pass&quot;", html)
        self.assertNotIn("<net>", html)

    # --- R1: per-render CSRF token --------------------------------------------

    def test_csrf_token_review(self):
        # Exact match; missing, shorter, longer or differing values reject.
        self.assertEqual(self.run_script(["token " + feed(b"ab12cd34") + " " + feed(b"ab12cd34")]), ["token match"])
        for offered in (feed(b"ff12cd34"), feed(b"ab12cd3"), feed(b"ab12cd34x"), "-"):
            self.assertEqual(self.run_script(["token " + feed(b"ab12cd34") + " " + offered]), ["token reject"])

    def test_form_accepts_the_csrf_token_key(self):
        self.assertEqual(
            self.run_script(["form " + feed(b"ssid=net&password=secret1&token=ab12cd34")]),
            ["form ssid=net pass=secret1 token=ab12cd34"])
        # Unknown keys and a repeated token still reject.
        for raw in (b"ssid=a&password=b&extra=1", b"ssid=a&password=b&token=x&token=y"):
            self.assertEqual(self.run_script(["form " + feed(raw)]), ["form error"])
    # --- S5: scan selection policy --------------------------------------------

    def scan_selection(self, raw):
        tokens = []
        for rssi, ssid in raw:
            tokens.append(str(rssi))
            tokens.append(ssid.hex() if ssid is not None else "-")
        out = self.run_script(["nets " + " ".join(tokens)])[0]
        self.assertTrue(out.startswith("nets "), out)
        return [item.rsplit("|", 1) for item in out[len("nets "):].split(" ")]

    def test_scan_selection_dedups_sorts_and_skips_hidden(self):
        # Same SSID keeps the strongest RSSI, the empty (hidden) SSID is not
        # selectable anywhere in the portal, the list sorts by descending
        # RSSI and original order breaks ties.
        self.assertEqual(self.scan_selection([
            (40, b"Alpha"), (55, b"Alpha"), (-30, b"Beta"),
            (70, None), (15, b"Gamma"),
        ]), [["Alpha", "55"], ["Gamma", "15"], ["Beta", "-30"]])

    def test_scan_selection_ties_keep_input_order(self):
        self.assertEqual(self.scan_selection([(50, b"A"), (50, b"B"), (50, b"C")]),
                         [["A", "50"], ["B", "50"], ["C", "50"]])

    def test_scan_selection_is_capped_at_twelve(self):
        raw = [(rssi, ("net%d" % rssi).encode()) for rssi in range(15)]
        chosen = self.scan_selection(raw)
        self.assertEqual(len(chosen), 12)
        self.assertEqual([int(rssi) for _, rssi in chosen],
                         list(range(14, 2, -1)))

    # --- S5/T7: the selector on the page ----------------------------------------

    def page_with(self, networks):
        lines = ["net %s %d" % (n[0].hex(), n[1]) for n in networks]
        lines.append("page - -")
        return self.run_script(lines, mode="page")[0][len("page "):].replace("\\n", "\n")

    def test_form_page_lists_networks_as_radio_buttons(self):
        html = self.page_with([(b"Alpha", 40), (b"Beta", 20)])
        # The label wraps the radio in a flex row: the old block-level rule
        # pushed every name out of line with its button.
        self.assertIn('<label class="net"><input type="radio" name="ssid" value="Alpha" required><span>Alpha</span></label>', html)
        self.assertIn('<label class="net"><input type="radio" name="ssid" value="Beta" required><span>Beta</span></label>', html)
        self.assertIn('<a href="/">Scan again</a>', html)

    def test_form_page_escapes_ssid_values_and_labels(self):
        html = self.page_with([(b'Bad"Network<1>&', -5)])
        self.assertIn('value="Bad&quot;Network&lt;1&gt;&amp;"', html)
        self.assertIn('required><span>Bad&quot;Network&lt;1&gt;&amp;</span></label>', html)

    def test_form_page_without_networks_prompts_a_rescan(self):
        html = self.page_with([])
        self.assertNotIn('type="radio"', html)
        self.assertIn("No networks detected yet", html)

    def test_form_page_has_no_manual_ssid_field(self):
        # The user removed the manual SSID input (S5); the SSID can only
        # arrive as the value of one of the scan's radio buttons.
        html = self.page_with([(b"Alpha", 40)])
        self.assertNotIn("Network name (SSID)", html)
        self.assertNotIn("<input name=", html)

    def test_selector_keeps_token_and_never_reflects_a_password(self):
        # Token unchanged, password input still never pre-filled, now with
        # the radio list present.
        lines = ["net Alpha 40"]
        raw = self.run_script(lines + ["page " + feed(b"err") + " " + feed(b"ab12cd34")],
                              mode="page")[0]
        html = raw[len("page "):].replace("\\n", "\n")
        self.assertIn('type="hidden" name="token" value="ab12cd34"', html)
        self.assertNotIn("value=", html.split('name="password"', 1)[1].split(">", 1)[0])
        self.assertIn("err", html)


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

    def test_validating_client_is_kept_open(self):
        # R4: while Validating, pollPortal must return before the timeout or a re-read.
        poll = self.source.split("static void pollPortal() {", 1)[1]
        poll = poll[:poll.index("\n}")]
        start = poll.index("if (portalPhase == PortalPhase::Validating)")
        last = poll[:poll.index("\n  }", start)].rstrip().rsplit("\n", 1)[-1].strip()
        self.assertEqual(last, "return;")

    def test_post_save_requires_the_rendered_token(self):
        # R1: the rendered token is reviewed before the credentials are touched.
        self.assertIn("portal::tokenMatches(portalFormToken, fields.token)", self.source)
        self.assertIn("portalReply(403,", self.source)

    def test_scan_runs_once_per_page_render_under_ap_sta(self):
        # T7: the portal maintains WIFI_AP_STA while open so the scan has a
        # station interface; the raw scan buffer is drawn once, copied and
        # deleted; while validating no scan runs at all.
        self.assertEqual(self.source.count("WiFi.scanNetworks()"), 1)
        self.assertIn("WiFi.scanDelete()", self.source)
        self.assertEqual(self.source.count("WiFi.mode(WIFI_AP)"), 0)
        open_portal = self.source.split("static void openPortal()", 1)[1]
        self.assertIn("WiFi.mode(WIFI_AP_STA)", open_portal)
        render = self.source.split("static std::string renderPortalForm", 1)[1]
        render = render[:render.index("\n}")]
        self.assertIn("portalPhase == PortalPhase::Validating", render)
        self.assertIn("scanVisibleNetworks()", render)

    def test_credential_storage_rolls_back_a_partial_write(self):
        # S4 promises the stored credentials stay untouched when a save fails;
        # a half-applied password/ssid pair would break that promise.
        store = self.source.split("static bool storeCredentials", 1)[1]
        store = store[:store.index("\n}")]
        self.assertIn('previousSsid', store)
        self.assertIn('previousPassword', store)
        self.assertIn('preferences.remove(', store)


    def test_csrf_token_lives_for_the_whole_portal_session(self):
        # Rotating the token on every render broke configuration: the phone's
        # captive-portal probes and the "Scan again" link re-render the page,
        # which invalidated the token of the form the user had open and
        # answered 403 "Invalid or missing token".
        render = self.source.split("static std::string renderPortalForm", 1)[1]
        render = render[:render.index("\n}")]
        self.assertNotIn("esp_random()", render)
        opened = self.source.split("static void openPortal()", 1)[1]
        opened = opened[:opened.index("\n}")]
        self.assertIn("esp_random()", opened)
        self.assertIn("portalFormToken = token;", opened)

    def test_network_rows_keep_the_button_inline_with_the_name(self):
        # A bare `input` rule made every radio a full-width block and pushed
        # its name out of line; the full-width rule is scoped to the password
        # field and the rows are flex labels.
        page = (ROOT / "include" / "portal_page.h").read_text()
        self.assertIn("input[type=password]{display:block;width:100%", page)
        self.assertNotIn('"input{display:block', page)
        self.assertIn("label.net{display:flex;align-items:center", page)
        self.assertIn('label class=\\"net\\"', page)

    def test_rejection_never_echoes_the_live_session_token(self):
        # A request without a valid token must not be able to read that token
        # back from the error page: both pre-token rejections serve a form
        # with no token field at all.
        source = self.source
        for marker_text, code in (("if (!fields.ok) {", "400"),
                                  ("if (!portal::tokenMatches(portalFormToken, fields.token)) {",
                                   "403")):
            branch = source.split(marker_text, 1)[1]
            branch = branch[:branch.index("return;")]
            self.assertIn('portalReply(%s,' % code, branch)
            self.assertIn("renderPortalFormUnverified(", branch)
            self.assertNotIn("renderPortalForm(", branch)
        unverified = source.split("static std::string renderPortalFormUnverified", 1)[1]
        unverified = unverified[:unverified.index("\n}")]
        self.assertNotIn("portalFormToken", unverified)

# --- S7/T10: structural pins on the channel-order wiring ---------------------

class ChannelOrderStructuralTests(unittest.TestCase):
    """Secondary evidence only, in the MainWiringStructuralTests style: the
    functional pins are the rig in tests/test_rgb_order.py; these pin that
    main.cpp actually applies the persisted order when the panel object is
    initialised and wires the new surfaces exactly as the S7 decisions
    require."""

    def setUp(self):
        self.source = (ROOT / "src/main.cpp").read_text()

    def test_order_is_read_and_applied_before_the_panel_exists(self):
        # Adafruit_Protomatter copies the pin list inside its constructor
        # (core.c: _PM_init memcpy), so the NVS order must be read and the
        # base pins permuted before the panel object is constructed at boot,
        # never as an untouchable static global.
        setup = self.source.split("void setup() {", 1)[1]
        setup = setup[:setup.index("\n}")]
        self.assertLess(setup.index('preferences.getUChar("rgborder"'),
                        setup.index("new (panelStorage) Adafruit_Protomatter("))
        self.assertLess(setup.index("rgb_order::applyOrder(channelOrder, rgbPins);"),
                        setup.index("new (panelStorage) Adafruit_Protomatter("))
        self.assertLess(setup.index("new (panelStorage) Adafruit_Protomatter("),
                        setup.index("matrix.begin()"))
        # The static storage keeps the `matrix` identity the whole file uses.
        self.assertIn("alignas(Adafruit_Protomatter) static uint8_t panelStorage",
                      self.source)
        self.assertIn("static Adafruit_Protomatter &matrix = ", self.source)
        self.assertIn("#include <new>", self.source)

    def test_persisted_order_has_its_own_key_outside_the_reset_scope(self):
        # Own key, not one of the reset keys; the DOWN reset scope stays
        # exactly ssid/password/updir and can never erase the channel order.
        store = self.source.split("static bool storeRgbOrder(", 1)[1]
        store = store[:store.index("\n}")]
        self.assertIn('preferences.putUChar("rgborder"', store)
        self.assertIn('preferences.isKey("rgborder")', store)
        self.assertIn("preferences.getUChar", store)  # read-back verification
        reset_header = (ROOT / "include" / "wifi_credentials_reset.h").read_text()
        self.assertNotIn("rgborder", reset_header)

    def test_console_rgb_command_saves_through_the_shared_path(self):
        console = self.source.split("static serial::ProvisionConsole provisionConsole({", 1)[1]
        console = console[:console.index("});")]
        self.assertIn("storeRgbOrder(order)", console)

    def test_portal_order_route_is_token_gated_and_restarts(self):
        save = self.source.split("static void beginPortalOrderSave(", 1)[1]
        save = save[:save.index("\n}")]
        self.assertIn("portal::parseOrderForm(", save)
        self.assertIn("portal::tokenMatches(portalFormToken, fields.token)", save)
        self.assertIn("portalReply(403,", save)
        self.assertIn("renderPortalFormUnverified(", save[:save.index("return;")])
        self.assertIn("rgb_order::parseOrder(", save)
        self.assertIn("storeRgbOrder(", save)
        self.assertIn("portal_page::orderSavedPage()", save)
        self.assertIn("ESP.restart()", save)

    def test_dispatch_accepts_rgborder_without_disturbing_save(self):
        dispatch = self.source.split("static void dispatchPortalRequest() {", 1)[1]
        dispatch = dispatch[:dispatch.index("\n}")]
        self.assertIn('line.path != "/save" && line.path != "/rgborder"', dispatch)
        self.assertIn('line.path == "/rgborder"', dispatch)
        # The validating 503 guard still covers both POST routes.
        self.assertIn("portalPhase == PortalPhase::Validating", dispatch)
        self.assertIn("beginPortalValidation(", dispatch)

    def test_portal_form_renders_the_current_channel_order(self):
        # Both render paths pass the persisted order so the selector
        # preselects what the board is actually using.
        render = self.source.split("static std::string renderPortalForm(", 1)[1]
        render = render[:render.index("\n}")]
        self.assertIn("channelOrder", render)
        unverified = self.source.split("static std::string renderPortalFormUnverified(", 1)[1]
        unverified = unverified[:unverified.index("\n}")]
        self.assertIn("channelOrder", unverified)


    def test_panel_is_constructed_once_and_begun_after_pins_are_final(self):
        # Protomatter copies the pin list inside its constructor, so the order
        # has to be applied before the object exists: construct once, after
        # applyOrder, before begin(). A regression here leaves the panel dark
        # and no host test would notice.
        self.assertEqual(
            self.source.count("new (panelStorage) Adafruit_Protomatter("), 1)
        self.assertEqual(self.source.count("matrix.begin()"), 1)
        setup = self.source.split("void setup() {", 1)[1]
        construct = setup.index("new (panelStorage) Adafruit_Protomatter(")
        self.assertLess(setup.index("rgb_order::applyOrder(channelOrder, rgbPins);"),
                        construct)
        self.assertLess(construct, setup.index("matrix.begin()"))

    def test_rgb_store_uses_readback_not_putuchar_truthiness(self):
        # Code 0 is RGB and putUChar returns 0 on success anyway, so the store
        # must verify the written byte by reading it back.
        store = self.source.split("static bool storeRgbOrder(", 1)[1]
        store = store[:store.index("\n}")]
        self.assertIn('preferences.isKey("rgborder")', store)
        self.assertIn('preferences.getUChar("rgborder", 255) == rgb_order::orderCode(order)', store)
        self.assertNotIn('preferences.putUChar("rgborder", rgb_order::orderCode(order)) != 0', store)

if __name__ == "__main__":
    unittest.main()

