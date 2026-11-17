import binascii
import pathlib
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data))


def png(width=64, height=64, color_type=6, interlace=0):
    header = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, interlace)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress((b"\0" + b"\xff\0\0\xff" * width) * height))
            + chunk(b"IEND", b""))


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.temp.name) / "protocol"
        subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                        str(ROOT / "tests/protocol_cli.cpp"), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def parse(self, body, mode="headers"):
        return int(subprocess.run([str(self.binary), mode], input=body, capture_output=True, check=True).stdout)

    def parse_state(self, body):
        output = subprocess.run([str(self.binary), "state"], input=body, capture_output=True, check=True).stdout.decode()
        status, state = output.split()
        return int(status), state

    def headers(self, extra=b"", length=b"100"):
        return (b"POST /emote HTTP/1.1\r\nContent-Type: image/png\r\nX-Emote-Version: 1\r\n"
                + b"Content-Length: " + length + b"\r\n" + extra + b"\r\n")

    def test_sender_headers(self):
        self.assertEqual(self.parse(self.headers()), 200)
        self.assertEqual(self.parse(self.headers(length=b"32768")), 200)
        self.assertEqual(self.parse(b"GET /health HTTP/1.1\r\nHost: gentle-matrix.local\r\n\r\n"), 200)

    def test_http_rejections(self):
        for raw, expected in [
            (self.headers(length=b"32769"), 413),
            (self.headers(length=b"999999999999999999999999999999999999"), 413),
            (self.headers(length=b"0"), 400),
            (self.headers(length=b"-1"), 400),
            (self.headers(length=b"1x"), 400),
            (self.headers(b"Content-Length: 100\r\n"), 400),
            (self.headers(b"Transfer-Encoding: chunked\r\n"), 400),
            (self.headers(b"Expect: 100-continue\r\n"), 417),
            (self.headers(b"X-Emote-Version: 1\r\n"), 400),
            (self.headers().replace(b"image/png", b"image/jpeg"), 415),
            (self.headers().replace(b"Content-Length: 100\r\n", b""), 411),
            (self.headers().replace(b"X-Emote-Version: 1\r\n", b""), 400),
            (self.headers().replace(b"/emote", b"/other"), 404),
            (self.headers(b"X-Padding: " + b"x" * 4096 + b"\r\n"), 431),
            (self.headers()[:-1], 400),
        ]:
            with self.subTest(raw=raw[:100]):
                self.assertEqual(self.parse(raw), expected)

    def test_emote_state_clock_metadata(self):
        self.assertEqual(self.parse_state(self.headers(b"X-Emote-State: clock\r\n")), (200, "clock"))
        # The clock metadata must never be rejected by the existing parser.
        self.assertEqual(self.parse(self.headers(b"X-Emote-State: clock\r\n")), 200)

    def test_missing_or_unknown_state_defaults_avatar(self):
        self.assertEqual(self.parse_state(self.headers()), (200, "avatar"))
        for value in [b"avatar", b"image", b"photo", b""]:
            with self.subTest(value=value):
                self.assertEqual(self.parse_state(self.headers(b"X-Emote-State: " + value + b"\r\n")),
                                 (200, "avatar"))
                self.assertEqual(self.parse(self.headers(b"X-Emote-State: " + value + b"\r\n")), 200)

    def test_session_header_on_image(self):
        # The session header is optional metadata for PNG delivery: a valid
        # value must keep the legacy image request accepted, while duplicates
        # or malformed values reject the whole request without side effects.
        self.assertEqual(self.parse(self.headers(b"X-Emote-Session-Id: abcd1234\r\n")), 200)
        self.assertEqual(self.parse(self.headers(b"X-Emote-Session-Id: bad id\r\n")), 400)
        self.assertEqual(self.parse(self.headers(b"X-Emote-Session-Id: a\r\nX-Emote-Session-Id: b\r\n")), 400)

    def test_project_name_validation_matrix(self):
        # Desktop parity (S2): one decode pass, '+' literal, '%' needs two hex
        # digits; valid names ride image requests, every rejection is 400.
        for value in [b"lab", b"Hello%20World", b"a+b", b"%C3%A9",
                      b"a" * 128,                      # 128 UTF-16 units
                      b"%C3%A9" * 128,                 # 128 units, 256 bytes
                      b"%F0%9F%98%80" * 64]:           # 64 astral scalars = 128 units
            with self.subTest(value=value[:40]):
                self.assertEqual(self.parse(self.headers(b"X-Emote-Project-Name: " + value + b"\r\n")), 200)
        for value in [
            b"",                                # blank after trim
            b"%20",                             # decoded blank
            b".", b"..", b"%2E", b"%2E%2E",     # exactly dot / dotdot
            b"%", b"%A", b"%ZZ", b"%4",         # malformed escapes
            b"%C3", b"%FF", b"%C3%28",          # invalid UTF-8 decoded
            b"%C0%80",                          # overlong
            b"%ED%A0%80",                       # UTF-16 surrogate
            b"%0A", b"%1F", b"%00", b"%7F",     # control characters and DEL
            b"a/b", b"a%2Fb", b"a\\b",          # path separators
            b"a" * 129,                         # 129 UTF-16 units
            b"%C3%A9" * 129,                    # 129 units, 258 bytes
            b"%F0%9F%98%80" * 65,               # 130 units
            b"a" * 1152,                        # decoded UTF-8 bound
            b"%41" * 385,                       # encoded bound (1155 bytes)
        ]:
            with self.subTest(value=value[:40]):
                self.assertEqual(self.parse(self.headers(b"X-Emote-Project-Name: " + value + b"\r\n")), 400)

    def test_project_name_parity_trims_only_for_blankness(self):
        # Desktop parity (receiver.rs:338-384): the trim only decides blankness;
        # the dot, control-character and 128-unit rules and the stored value all
        # use the untrimmed name. Its JavaScript-style trim counts a BOM and NBSP
        # as whitespace and does not count U+0085 (kept out of this test because
        # the runner's line splitting treats U+0085 as a line break).
        sep = b"\n----\n"
        raw = (self.headers(b"X-Emote-Project-Name: %20x\r\n") + sep
               + self.headers(b"X-Emote-Project-Name: %20.%20\r\n") + sep
               + self.headers(b"X-Emote-Project-Name: %09lab\r\n") + sep
               + self.headers(b"X-Emote-Project-Name: %EF%BB%BF\r\n") + sep
               + self.headers(b"X-Emote-Project-Name: %C2%A0\r\n") + sep
               + self.headers(b"X-Emote-Project-Name: %C2%A1\r\n"))
        self.assertEqual(self.store(raw),
                         ["200  x", "200  . ", "400  .  invalid-name", "400  .  invalid-name",
                          "400  .  invalid-name", "200 \u00a1"],
                         "spaces survive, a leading tab is a control character, a BOM-only or NBSP-only name is blank, a non-ASCII name is kept")

    def test_project_name_duplicate_header_rejects(self):
        # A second X-Emote-Project-Name header rejects the whole image request.
        # Bodyless controls are lifecycle endpoints: the desktop receiver skips
        # the header entirely there (receiver.rs:262), so a duplicate or invalid
        # value on a control is ignored instead of rejecting the order.
        self.assertEqual(self.parse(self.headers(
            b"X-Emote-Project-Name: a\r\nX-Emote-Project-Name: b\r\n")), 400)
        self.assertEqual(self.parse(
            b"POST /emote/heartbeat HTTP/1.1\r\nX-Emote-Version: 1\r\n"
            b"X-Emote-Session-Id: abcd1234\r\nContent-Length: 0\r\n"
            b"X-Emote-Project-Name: a\r\nX-Emote-Project-Name: b\r\n\r\n"), 200)
        self.assertEqual(self.parse(
            b"POST /emote/claim HTTP/1.1\r\nX-Emote-Version: 1\r\n"
            b"X-Emote-Session-Id: abcd1234\r\nContent-Length: 0\r\n"
            b"X-Emote-Project-Name: bad id\r\n\r\n"), 200)

    def store(self, raw):
        out = subprocess.run([str(self.binary), "store"], input=raw,
                             capture_output=True, check=True).stdout.decode()
        return out.splitlines()

    def test_image_store_and_absent_clears_name(self):
        # Only the image path stores the name; a later accepted image
        # without the header clears it.
        sep = b"\n----\n"
        raw = (self.headers(b"X-Emote-Project-Name: LAB\r\n") + sep
               + self.headers() + sep
               + self.headers(b"X-Emote-Project-Name: LAB\r\n"))
        self.assertEqual(self.store(raw), ["200 LAB", "200 -", "200 LAB"])

    def test_rejected_image_is_distinct_and_changes_no_state(self):
        sep = b"\n----\n"
        raw = (self.headers(b"X-Emote-Project-Name: LAB\r\n") + sep
               + self.headers(b"X-Emote-Project-Name: %0A\r\n"))
        # The invalid name is a distinct parser result ("invalid-name") and
        # leaves the stored title untouched.
        self.assertEqual(self.store(raw), ["200 LAB", "400 LAB invalid-name"])

    def test_controls_ignore_project_name(self):
        # claim/heartbeat/release never change the stored name, with or without
        # the header; an invalid or duplicate value is ignored rather than
        # rejecting the order, exactly like the desktop lifecycle endpoints.
        sep = b"\n----\n"
        def control(route, name=None):
            extra = b"" if name is None else b"X-Emote-Project-Name: " + name + b"\r\n"
            return (b"POST /emote/" + route + b" HTTP/1.1\r\nX-Emote-Version: 1\r\n"
                    b"X-Emote-Session-Id: abcd1234\r\nContent-Length: 0\r\n"
                    + extra + b"\r\n")
        raw = (self.headers(b"X-Emote-Project-Name: LAB\r\n") + sep
               + control(b"claim", b"OTHER") + sep
               + control(b"heartbeat", b"OTHER") + sep
               + control(b"release", b"OTHER") + sep
               + control(b"heartbeat") + sep
               + control(b"heartbeat", b"%0A"))
        self.assertEqual(self.store(raw),
                         ["200 LAB", "200 LAB", "200 LAB", "200 LAB", "200 LAB",
                          "200 LAB"],
                         "lifecycle endpoints ignore the name, so the stored title survives every control")

    def test_sender_png(self):
        self.assertEqual(self.parse(png(), "png"), 200)

    def test_invalid_pngs(self):
        good = png()
        bad_crc = bytearray(good)
        bad_crc[50] ^= 1
        huge_chunk = bytearray(good)
        huge_chunk[33:37] = b"\xff\xff\xff\xff"
        for data in [b"", good[:-1], good + b"garbage", bytes(bad_crc), bytes(huge_chunk),
                     png(width=32), png(height=32), png(color_type=2), png(interlace=1),
                     good[:33] + good[:33][8:] + good[33:], good[:33] + chunk(b"IEND", b""),
                     good[:33] + chunk(b"tEXt", b"x") + good[33:]]:
            with self.subTest(size=len(data)):
                self.assertEqual(self.parse(data, "png"), 422)


if __name__ == "__main__":
    unittest.main()
