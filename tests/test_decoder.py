import pathlib
import subprocess
import tempfile
import unittest
import zlib

from test_protocol import ROOT, chunk, png


class DecoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / ".pio/libdeps/matrixportal_s3/PNGdec/src"
        if not source.exists():
            raise RuntimeError("Run .venv/bin/pio run first to install the pinned PNGdec sources")
        cls.temp = tempfile.TemporaryDirectory()
        temp = pathlib.Path(cls.temp.name)
        cls.binary = temp / "decoder"
        objects = []
        for name in ["adler32", "crc32", "inflate", "inffast", "inftrees", "zutil"]:
            obj = temp / f"{name}.o"
            subprocess.run(["clang", "-w", "-I", str(source), "-c", str(source / f"{name}.c"), "-o", str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run(["clang++", "-std=c++11", "-w", "-I", str(ROOT / "include"), "-I", str(source),
                        str(ROOT / "tests/decoder_cli.cpp"), str(source / "PNGdec.cpp"), *objects,
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, data):
        return subprocess.run([str(self.binary)], input=data, capture_output=True, check=True).stdout.decode()

    def test_full_frame_pixels(self):
        self.assertEqual(self.decode(png()), "200 63488 63488")  # RGB565 red, first and last row.

    def test_compressed_stream_corruption_with_valid_chunk_crc(self):
        good = png()
        for stream in [b"not-zlib", zlib.compress(b"\0" * 32), zlib.compress(b"\x05" + b"\0" * 16447)]:
            bad = good[:33] + chunk(b"IDAT", stream) + chunk(b"IEND", b"")
            self.assertEqual(self.decode(bad), "422")

    def test_real_gentle_emote_png(self):
        sender = ROOT.parent / "gentle-emote"
        if not (sender / "src/display_image.ts").exists():
            self.skipTest("optional integration requires sibling gentle-emote checkout")
        script = """
          import {readFileSync} from 'node:fs';
          import {prepareDisplayImage} from './src/display_image.ts';
          const sprite = readFileSync('./emotes/gentleman/idle/idle.png').toString('base64');
          process.stdout.write(prepareDisplayImage(sprite, {status:'busy',phase:'executing',activeSubagents:2}));
        """
        body = subprocess.run(["node", "--input-type=module", "-e", script], cwd=sender, capture_output=True, check=True).stdout
        self.assertTrue(self.decode(body).startswith("200 "))

    def test_alpha_on_black(self):
        header = png()[:33]
        stream = zlib.compress((b"\0" + b"\xff\0\0\0" * 64) * 64)
        self.assertEqual(self.decode(header + chunk(b"IDAT", stream) + chunk(b"IEND", b"")), "200 0 0")


if __name__ == "__main__":
    unittest.main()
