import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_serial_protocol import msvc_vcvars

ROOT = pathlib.Path(__file__).resolve().parents[1]

# The base HUB75 wiring in Protomatter constructor order: R1 G1 B1 R2 G2 B2
# (the pinout the firmware has always used).
BASE = (42, 41, 40, 38, 39, 37)

# Every order's Protomatter pin list. Formal rule pinned below: the order
# names the colour sequence the PANEL lights on its three data inputs of each
# row, in the base wiring order; applyOrder hands Protomatter, for each of
# its six colour slots, the base pin wired to the panel input that lights
# that colour. RGB is therefore the untouched default.
EXPECTED_PINS = {
    "RGB": (42, 41, 40, 38, 39, 37),
    "RBG": (42, 40, 41, 38, 37, 39),
    "GRB": (41, 42, 40, 39, 38, 37),
    "GBR": (40, 42, 41, 37, 38, 39),
    "BRG": (41, 40, 42, 39, 37, 38),
    "BGR": (40, 41, 42, 37, 39, 38),
}

ORDERS = ("RGB", "RBG", "GRB", "GBR", "BRG", "BGR")


def feed(text):
    if isinstance(text, str):
        text = text.encode("latin-1")
    return text.hex()  # '-' later means "empty value" in CLI arguments


class ChannelOrderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.binary = cls.work / "rgb_order"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        source = str(ROOT / "tests/rgb_order_test.cpp")
        include = str(ROOT / "include")
        if cxx:
            subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", include, source, "-o", str(cls.binary)],
                           check=True)
        else:
            if not msvc_vcvars():
                raise unittest.SkipTest("no host C++ compiler available")
            bat = cls.work / "compile.bat"
            bat.write_text(
                '@echo off\r\n'
                'call "{}" >nul\r\n'
                'cl /nologo /EHsc /std:c++14 /W3 /WX /D_CRT_SECURE_NO_WARNINGS '
                '/I "{}" "{}" /Fe:"{}" /Fo:"{}\\\\RgbOrderTest.obj"\r\n'.format(
                    msvc_vcvars(), include, source, cls.binary, cls.work),
                encoding="ascii")
            subprocess.run(["cmd", "/c", str(bat)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_script(self, lines, mode="order"):
        result = subprocess.run([str(self.binary), mode],
                                input=("\n".join(lines) + "\n").encode("latin-1"),
                                capture_output=True, check=True)
        return result.stdout.decode("latin-1").splitlines()

    # --- S7: the six named orders and their exact pin lists -----------------

    def test_base_wiring_is_untouched(self):
        self.assertEqual(self.run_script(["base"]),
                         ["base " + " ".join(str(p) for p in BASE)])

    def test_every_order_yields_its_exact_pin_list(self):
        for order, expected in EXPECTED_PINS.items():
            with self.subTest(order=order):
                out = self.run_script(["pins " + feed(order)])
                self.assertEqual(out, ["pins " + " ".join(str(p) for p in expected)])

    def test_every_permutation_is_a_complete_distinct_mapping(self):
        # Each row triple stays a permutation of the same base triple and the
        # six orders give six distinct answers, so nothing else can leak in.
        seen = set()
        for order in ORDERS:
            values = self.run_script(["pins " + feed(order)])[0].split()[1:]
            pins = tuple(int(v) for v in values)
            seen.add(pins)
            for row in range(2):
                triple = pins[row * 3:row * 3 + 3]
                self.assertEqual(sorted(triple), sorted(BASE[row * 3:row * 3 + 3]))
        self.assertEqual(len(seen), 6)

    # --- S7: order-name parsing ----------------------------------------------

    def test_parse_is_case_insensitive_and_canonicalises(self):
        for text, expected in (("RGB", "RGB"), ("rgb", "RGB"), ("GrB", "GRB"),
                               ("gbr", "GBR"), ("brG", "BRG"), ("BgR", "BGR")):
            with self.subTest(text=text):
                self.assertEqual(self.run_script(["parse " + feed(text)]),
                                 ["parse " + expected])

    def test_parse_rejects_everything_else(self):
        for text in ("", "RGBX", "rg bx", "RGG", "BGB", "12", "R G B", "rgb ",
                     " rgb", "rgb\r", "Grün"):  # nonsense, junk, repeats, ASCII spaces
            with self.subTest(text=text):
                out = self.run_script(["pins " + feed(text)])
                self.assertEqual(out, ["pins reject"])

    # --- S7: the small persisted code ----------------------------------------

    def test_codes_are_small_and_in_enum_order(self):
        self.assertEqual([self.run_script(["code " + feed(o)])[0].split()[1]
                          for o in ORDERS], ["0", "1", "2", "3", "4", "5"])

    def test_code_round_trip(self):
        for index, order in enumerate(ORDERS):
            with self.subTest(order=order):
                self.assertEqual(self.run_script(["decode %d" % index]),
                                 ["decode " + order])

    def test_decode_rejects_everything_outside_the_codes(self):
        for code in (6, 7, 255, 200):
            self.assertEqual(self.run_script(["decode %d" % code]), ["decode reject"])

    # --- S7: the colour-order form shaping (portal_policy) -------------------

    def test_order_form_decodes_order_and_token(self):
        self.assertEqual(
            self.run_script(["oform " + feed(b"order=GRB&token=ab12cd34")],
                            mode="policy")[0],
        "orderform order=GRB token=ab12cd34")

    def test_order_form_rejects_unknown_extra_and_duplicate_keys(self):
        for raw in (b"order=GRB",                          # token missing
                    b"token=ab12cd34",                     # order missing
                    b"order=GRB&token=ab12cd34&extra=1",   # unknown key
                    b"order=GRB&order=RGB&token=ab12cd34", # duplicate order
                    b"order=GRB&token=x&token=y",          # duplicate token
                    b"order=%zz&token=ab12cd34"):          # malformed escape
            with self.subTest(body=raw):
                self.assertEqual(self.run_script(["oform " + feed(raw)],
                                                 mode="policy"),
                                 ["orderform error"])

    # --- S7: the RGB console command -----------------------------------------

    def console_show(self, lines):
        return self.run_script(lines, mode="console")[-1]

    def test_rgb_command_saves_and_reboots(self):
        out = self.run_script(["cfeed " + b"RGB GRB\n".hex(),
                               "cshow"], mode="console")
        self.assertEqual(out[:-1], ["out RGB_SAVED"])
        self.assertEqual(out[-1], "order=GRB cred=-|- flush=1 reboot=1")

    def test_rgb_command_name_is_case_insensitive(self):
        out = self.run_script(["cfeed " + b"RGB bgr\n".hex(), "cshow"],
                              mode="console")
        self.assertEqual(out[:-1], ["out RGB_SAVED"])
        self.assertEqual(out[-1], "order=BGR cred=-|- flush=1 reboot=1")

    def test_rgb_command_rejects_an_unknown_name(self):
        for line in (b"RGB XYZ\n", b"RGB \n", b"RGB\n", b"RGB RGG\n"):
            with self.subTest(line=line):
                state = self.console_show(["cfeed " + line.hex(), "cshow"])
                self.assertEqual(
                    self.run_script(["cfeed " + line.hex()], mode="console"),
                    ["out RGB_ERROR: unknown channel order"])
                self.assertEqual(state, "order=- cred=-|- flush=0 reboot=0")

    def test_rgb_command_reports_a_failed_store_without_rebooting(self):
        out = self.run_script(["ofail 1",
                               "cfeed " + b"RGB GRB\n".hex(),
                               "cshow"], mode="console")
        self.assertEqual(out[:-1], ["out RGB_ERROR: storage failed"])
        self.assertEqual(out[-1], "order=- cred=-|- flush=0 reboot=0")

    def test_command_word_stays_exact_and_lines_are_ignored_like_before(self):
        # Lowercase command words are not commands, and longer words that
        # merely start with RGB are still unknown top-level lines: both are
        # ignored silently, exactly like every other unknown line before S7.
        for raw in (b"rgb GRB\n", b"RGBX GRB\n", b"RGBS GRB\n"):
            with self.subTest(raw=raw):
                self.assertEqual(self.run_script(["cfeed " + raw.hex(), "cshow"],
                                                 mode="console"),
                                 ["order=- cred=-|- flush=0 reboot=0"])

    def test_wifi_flow_unaffected_by_the_rgb_command(self):
        out = self.run_script(["cfeed " + b"WIFI\nmyssid\npassword123\n".hex(),
                               "cshow"], mode="console")
        self.assertEqual(out[:-1], ["out WIFI_SSID", "out WIFI_PASSWORD",
                                    "out WIFI_SAVED"])
        self.assertEqual(out[-1], "order=- cred=myssid|password123 flush=1 reboot=1")

    # --- S7: the portal selector on the page ----------------------------------

    def page(self, error=b"", token=b"ab12cd34", current="RGB"):
        error_hex = feed(error) if error else "-"
        token_hex = feed(token) if token else "-"
        raw = self.run_script(
            ["page " + error_hex + " " + token_hex + " " + current],
            mode="page")[0]
        return raw[len("page "):].replace("\\n", "\n")

    def test_colour_selector_is_its_own_form_on_its_own_route(self):
        html = self.page()
        self.assertIn('<form method="POST" action="/rgborder">', html)
        self.assertIn('<select name="order">', html)
        # The credentials form is untouched on the same page.
        self.assertIn('<form method="POST" action="/save">', html)

    def test_colour_selector_lists_all_six_orders(self):
        html = self.page()
        for order in ORDERS:
            self.assertIn('<option value="%s"' % order, html)

    def test_colour_selector_preselects_the_current_order(self):
        for order, others in (("GBR", ("RGB", "BGR")), ("RBG", ("RGB",))):
            html = self.page(current=order)
            self.assertIn('value="%s" selected>' % order, html)
            for other in others:
                self.assertNotIn('value="%s" selected>' % other, html)

    def test_colour_selector_carries_the_session_token(self):
        html = self.page(token=b"ab12cd34")
        self.assertIn('type="hidden" name="token" value="ab12cd34"', html)

    def test_page_says_the_board_restarts_to_apply_the_choice(self):
        html = self.page()
        self.assertIn("restarts to apply", html)

    def test_saved_page_says_the_board_restarts(self):
        raw = self.run_script(["savedpage"], mode="page")[0]
        html = raw[len("page "):].replace("\\n", "\n")
        self.assertIn("Colour order saved", html)
        self.assertIn("restarting to apply", html)

    def test_selector_never_echoes_the_password(self):
        html = self.page()
        self.assertNotIn("value=", html.split('name="password"', 1)[1].split(">", 1)[0])


if __name__ == "__main__":
    unittest.main()
