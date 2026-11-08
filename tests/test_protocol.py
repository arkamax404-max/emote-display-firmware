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
