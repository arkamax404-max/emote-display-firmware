import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]

MAGIC = b"EMTS"
IMAGE, CLAIM, HEARTBEAT, RELEASE = 0, 1, 2, 3
F_SESSION, F_NAME, F_CLOCK = 1, 2, 4
MAX_FRAME = 18 + 128 + 1152 + 32768 + 4


def chunk(kind, data):
    import binascii
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data))


def png(width=64, height=64, pad_to=None):
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    pixels = zlib.compress((b"\0" + b"\xff\0\0\xff" * width) * height)
    if pad_to:
        # validPngEnvelope never decompresses: padding inside IDAT keeps the
        # envelope valid while stretching the frame to an exact total size
        # (8 signature + 25 IHDR + 12+L IDAT + 12 IEND = 57 + L).
        pixels += b"\x00" * (pad_to - 57 - len(pixels))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", pixels) + chunk(b"IEND", b""))


def frame(op=IMAGE, request_id=1, flags=0, session=b"", name=b"", payload=b"",
          version=1, reserved=0, session_len=None, name_len=None,
          payload_len=None, crc=None):
    sl = len(session) if session_len is None else session_len
    nl = len(name) if name_len is None else name_len
    pl = len(payload) if payload_len is None else payload_len
    body = (MAGIC + struct.pack("<BBBBHHHI", version, op, flags, reserved,
                                request_id, sl, nl, pl)
            + session + name + payload)
    checksum = zlib.crc32(body) & 0xffffffff if crc is None else crc
    return body + struct.pack("<I", checksum)


def msvc_vcvars():
    for root in (r"C:\Program Files\Microsoft Visual Studio\2022",
                 r"C:\Program Files (x86)\Microsoft Visual Studio\2019"):
        for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
            candidate = pathlib.Path(root) / edition / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                return candidate
    return None


class SerialProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.build = pathlib.Path(cls.temp.name)
        cls.binary = cls.build / "serial_protocol"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        source = str(ROOT / "tests/serial_protocol_test.cpp")
        include = str(ROOT / "include")
        if cxx:
            subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", include, source, "-o", str(cls.binary)], check=True)
        elif msvc_vcvars():
            obj = cls.build / "obj"
            obj.mkdir()
            bat = cls.build / "compile.bat"
            bat.write_text(
                '@echo off\r\n'
                'call "{}" >nul\r\n'
                'cl /nologo /EHsc /std:c++14 /W3 /WX /D_CRT_SECURE_NO_WARNINGS '
                '/I "{}" "{}" /Fe:"{}" /Fo:"{}\\\\SerialProtocolTest.obj"\r\n'.format(
                    msvc_vcvars(), include, source, cls.binary, obj), encoding="ascii")
            subprocess.run(["cmd", "/c", str(bat)], check=True, capture_output=True)
        else:
            raise unittest.SkipTest("no host C++ compiler available")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_bytes(self, args, data):
        result = subprocess.run([str(self.binary)] + args, input=data,
                                capture_output=True, check=True)
        return result.stdout

    def run_cli(self, args, data):
        return self.run_bytes(args, data).decode("utf-8").rstrip("\n")

    @staticmethod
    def parse_line(line):
        # Structured line parsing: the decoded name may contain spaces, so it
        # is everything between the payload CRC and the trailing skipped= field.
        tokens = line.split()
        if tokens[0] == "complete":
            return {"event": "complete", "op": tokens[1], "request_id": tokens[2],
                    "session": tokens[3], "clock": tokens[4], "payload_len": tokens[5],
                    "payload_crc": tokens[6], "name": " ".join(tokens[7:-1]),
                    "skipped": tokens[-1]}
        if tokens[0] == "error":
            return {"event": "error", "code": tokens[1], "skipped": tokens[-1]}
        if tokens[0] in ("idle", "active", "reset"):
            return {"event": tokens[0],
                    "skipped": tokens[-1] if tokens[-1].startswith("skipped=") else None}
        return {"event": tokens[0], "raw": tokens}

    def out(self, data, mode="frame", chunk=None):
        args = [mode] + ([str(chunk)] if chunk else [])
        return self.parse_line(self.run_cli(args, data))

    def last(self, data, chunk=None):
        # SHOW lines: "last <op> <requestId> <session|-> <clock|avatar>
        # <payloadLen> <name|->" with the name possibly containing spaces.
        args = ["seq"] + ([str(chunk)] if chunk else [])
        tokens = self.run_cli(args, data).splitlines()[-1].split()
        self.assertEqual(tokens[0], "last")
        return {"op": tokens[1], "request_id": tokens[2], "session": tokens[3],
                "clock": tokens[4], "payload_len": tokens[5],
                "name": " ".join(tokens[6:])}

    def seq(self, data, chunk=None):
        args = ["seq"] + ([str(chunk)] if chunk else [])
        return [self.parse_line(line) for line in self.run_cli(args, data).splitlines()]

    # --- happy path ------------------------------------------------------

    def test_valid_image_frame_and_byte_splits(self):
        image = frame(op=IMAGE, request_id=4602, flags=F_SESSION | F_NAME | F_CLOCK,
                      session=b"abcd1234", name=b"Hello%20World", payload=png())
        for split in (None, 1, 2, 3, 5, 7):
            with self.subTest(split=split):
                result = self.out(image, chunk=split)
                self.assertEqual(result, {"event": "complete", "op": "image",
                                          "request_id": "4602", "session": "abcd1234",
                                          "clock": "clock", "payload_len": str(len(png())),
                                          "payload_crc": str(zlib.crc32(png())),
                                          "name": "Hello World", "skipped": "skipped=0"})

    def test_each_control_operation(self):
        for op, kind in [(CLAIM, "claim"), (HEARTBEAT, "heartbeat"), (RELEASE, "release")]:
            with self.subTest(op=op):
                result = self.out(frame(op=op, flags=F_SESSION, session=b"lab.session-1:x"))
                self.assertEqual(result["event"], "complete")
                self.assertEqual(result["op"], kind)
                self.assertEqual(result["session"], "lab.session-1:x")
                self.assertEqual(result["clock"], "avatar")
                self.assertEqual(result["payload_len"], "0")
                self.assertEqual(result["name"], "-")

    def test_clock_metadata_parity(self):
        self.assertEqual(self.out(frame(op=IMAGE, flags=F_SESSION | F_CLOCK,
                                        session=b"abcd1234", payload=png()))["clock"],
                         "clock")
        self.assertEqual(self.out(frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234",
                                        payload=png()))["clock"], "avatar")

    def test_request_id_boundary_values(self):
        for request_id in (0, 1, 65535):
            with self.subTest(request_id=request_id):
                result = self.out(frame(op=CLAIM, flags=F_SESSION, session=b"abcd1234",
                                        request_id=request_id))
                self.assertEqual((result["event"], result["request_id"]),
                                 ("complete", str(request_id)))

    # --- header structure rejections -------------------------------------

    def test_version_op_reserved_flags_rejections(self):
        base = dict(op=CLAIM, flags=F_SESSION)
        cases = [
            (frame(version=0, **base), "version"),
            (frame(version=2, **base), "version"),
            (frame(op=4, flags=F_SESSION), "op"),
            (frame(op=255, flags=F_SESSION), "op"),
            (frame(reserved=1, **base), "reserved"),
            (frame(op=CLAIM, flags=0x08), "flags"),
            (frame(op=CLAIM, flags=0xFF), "flags"),
        ]
        for raw, code in cases:
            with self.subTest(code=code):
                self.assertEqual(self.out(raw), {"event": "error", "code": code,
                                                 "skipped": "skipped=0"})

    def test_length_rules(self):
        cases = [
            (frame(flags=F_SESSION, session_len=0), "length"),          # flag without bytes
            (frame(flags=0, session=b"ab"), "length"),                   # bytes without flag
            (frame(flags=F_SESSION, session=b"a" * 129), "length"),      # over identity bound
            (frame(flags=F_NAME, name_len=0), "length"),
            (frame(flags=0, name=b"ab"), "length"),
            (frame(flags=F_NAME, name=b"a" * 1153), "length"),
            (frame(op=IMAGE, flags=0, payload_len=0), "length"),         # image needs payload
            (frame(op=CLAIM, flags=F_SESSION, payload=b"x"), "length"),  # control forbids payload
            (frame(op=CLAIM, flags=0), "session"),                       # control needs session
        ]
        for raw, code in cases:
            with self.subTest(code=code):
                self.assertEqual(self.out(raw), {"event": "error", "code": code,
                                                 "skipped": "skipped=0"})

    def test_oversize_lengths(self):
        self.assertEqual(self.out(frame(op=IMAGE, flags=0, payload_len=32769))["code"],
                         "oversize")
        self.assertEqual(self.out(frame(op=IMAGE, flags=0, payload_len=999999))["code"],
                         "oversize")

    def test_payload_over_receiver_capacity(self):
        raw = frame(op=IMAGE, flags=0, payload=b"x" * 200)
        result = subprocess.run([str(self.binary), "frame", str(len(raw)), "100"],
                                input=raw, capture_output=True, check=True)
        self.assertEqual(result.stdout.decode().split()[1], "oversize")

    def test_max_png_payload_bound(self):
        exact = png(pad_to=32768)
        self.assertEqual(len(exact), 32768)
        self.assertEqual(self.out(frame(op=IMAGE, flags=0, payload=exact))["payload_len"],
                         "32768")
        self.assertEqual(self.out(frame(op=IMAGE, flags=0, payload_len=32769))["event"],
                         "error")

    # --- semantic rejections ----------------------------------------------

    def test_session_identity_charset(self):
        self.assertEqual(self.out(frame(op=CLAIM, flags=F_SESSION,
                                        session=b"a" * 128))["event"], "complete")
        for session in (b"bad id", b"bad/id", b"bad\nid"):
            with self.subTest(session=session[:10]):
                self.assertEqual(self.out(frame(op=CLAIM, flags=F_SESSION,
                                                session=session))["code"], "session")

    def test_name_decode_matrix(self):
        valid = [(b"lab", "lab"), (b"Hello%20World", "Hello World"),
                 (b"%C3%A9", "\u00e9"), (b"a+b", "a+b"), (b"a" * 128, "a" * 128),
                 (b"%C3%A9" * 128, "\u00e9" * 128)]
        for encoded, decoded in valid:
            with self.subTest(name=encoded[:20]):
                result = self.out(frame(op=IMAGE, flags=F_NAME, payload=png(), name=encoded))
                self.assertEqual((result["event"], result["name"]), ("complete", decoded))
        invalid = [b"%20", b".", b"..", b"%0A", b"%C3", b"%ZZ", b"a/b",
                   b"a" * 129, b"%C3%A9" * 129, b"a" * 385, b"a" * 1152]
        for encoded in invalid:
            with self.subTest(name=encoded[:20]):
                self.assertEqual(self.out(frame(op=IMAGE, flags=F_NAME, payload=png(),
                                                name=encoded))["code"], "name")

    def test_name_ignored_on_controls_like_http(self):
        # The desktop lifecycle endpoints ignore X-Emote-Project-Name; the wire
        # format keeps that parity: a name on a control is carried, CRC-checked
        # and never decoded or stored.
        result = self.out(frame(op=CLAIM, flags=F_SESSION | F_NAME,
                                session=b"abcd1234", name=b"%0A"))
        self.assertEqual(result["event"], "complete")
        self.assertEqual(result["name"], "-")

    def test_invalid_png_payload_rejected(self):
        self.assertEqual(self.out(frame(op=IMAGE, flags=0, payload=b"not a png"))["code"],
                         "png")

    def test_bad_crc_rejected(self):
        raw = bytearray(frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png()))
        raw[-1] ^= 0x01  # corrupt the stored CRC itself
        self.assertEqual(self.out(bytes(raw))["code"], "crc")
        raw = bytearray(frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png()))
        raw[30] ^= 0x01  # corrupt a covered byte
        self.assertEqual(self.out(bytes(raw))["code"], "crc")

    def test_wrong_magic_never_completes(self):
        raw = frame().replace(b"EMTS", b"EMTX", 1)
        self.assertEqual(self.out(raw)["event"], "idle")

    # --- truncation, recovery, resync --------------------------------------

    def test_no_completion_on_truncation(self):
        whole = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        for keep in (18, 30, len(whole) - 5, len(whole) - 1):
            with self.subTest(keep=keep):
                self.assertEqual(self.out(whole[:keep])["event"], "active")

    def test_truncated_frame_completes_after_rest(self):
        whole = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        lines = self.seq(whole[:30] + b"\n----\n" + whole[30:])
        self.assertEqual([line["event"] for line in lines], ["active", "complete"])

    def test_recovery_after_error_takes_next_valid_frame(self):
        good = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        # Structurally valid frame whose covered bytes are corrupted: the CRC
        # check is the first rejection, before any semantic rule.
        bad = frame(op=IMAGE, flags=0, payload=b"junk", crc=0xdeadbeef)
        lines = self.seq(bad + b"\n----\n" + good)
        self.assertEqual(lines[0]["event"], "error")
        self.assertEqual(lines[0]["code"], "crc")
        self.assertEqual(lines[1]["event"], "complete")

    def test_resync_skips_interleaved_garbage(self):
        good = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        garbage = b"hello console line\r\n"
        self.assertEqual(self.out(garbage),
                         {"event": "idle", "skipped": "skipped=" + str(len(garbage))})
        result = self.out(garbage + good)
        self.assertEqual(result["event"], "complete")
        self.assertEqual(result["skipped"], "skipped=" + str(len(garbage)))

    def test_partial_magic_prefix_is_not_double_counted(self):
        good = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        result = self.out(b"xxEMxx" + good)
        self.assertEqual(result["event"], "complete")
        self.assertEqual(result["skipped"], "skipped=6")

    def test_reset_exposed_for_timeout_policy(self):
        whole = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        lines = self.seq(whole[:25] + b"\n----\nRESET\n----\n" + whole)
        self.assertEqual([line["event"] for line in lines], ["active", "reset", "complete"])

    def test_reject_scrubs_exposed_frame(self):
        # A rejected frame must not leave the previously accepted frame (or any
        # identity) visible through the parser output.
        good = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        shown = self.last(good + b"\n----\n"
                          + frame(op=IMAGE, flags=0, payload=b"junk", crc=0xdeadbeef)
                          + b"\n----\nSHOW")
        self.assertEqual(shown, {"op": "image", "request_id": "0", "session": "-",
                                 "clock": "avatar", "payload_len": "0", "name": "-"})

    def test_completed_frame_stable_until_next_activity(self):
        accepted = frame(op=CLAIM, flags=F_SESSION, request_id=99, session=b"abcd1234")
        shown = self.last(accepted + b"\n----\nSHOW")
        self.assertEqual(shown, {"op": "claim", "request_id": "99", "session": "abcd1234",
                                 "clock": "avatar", "payload_len": "0", "name": "-"})

    # --- replies -------------------------------------------------------------

    def test_reply_encoding_exact(self):
        for request_id, code in [(1, 0), (4602, 1), (65535, 11)]:
            with self.subTest(request_id=request_id, code=code):
                raw = self.run_bytes(["reply", str(request_id), str(code)], b"")
                head = b"EMTR" + bytes([1, code]) + struct.pack("<H", request_id)
                self.assertEqual(raw, head + struct.pack("<I", zlib.crc32(head)))

    def test_reply_correlates_and_echoes_no_identity(self):
        raw = self.run_bytes(["reply-for"],
                             frame(op=IMAGE, flags=F_SESSION | F_NAME, session=b"secret-session",
                                   name=b"top%20secret", payload=png()))
        self.assertEqual(raw[:4], b"EMTR")
        self.assertEqual(raw[5], 0)  # Ok
        self.assertEqual(struct.unpack("<H", raw[6:8])[0], 1)  # echoed request id
        self.assertNotIn(b"secret", raw)
        self.assertNotIn(b"top", raw)
        # A rejected frame answers NACK with the last request id, never the data.
        raw = self.run_bytes(["reply-for"],
                             frame(op=IMAGE, flags=0, payload=b"junk", request_id=777,
                                   crc=0xdeadbeef))
        self.assertEqual(raw[5], 1)  # Crc
        self.assertEqual(struct.unpack("<H", raw[6:8])[0], 777)
        self.assertNotIn(b"abcd1234", raw)

    def test_malformed_header_nack_correlates_its_own_request_id(self):
        # A rejected frame must answer with ITS request id, never the previous
        # frame's: the id is latched as soon as the fixed header bytes are
        # available, before any header validation runs (T2 readback fix 1).
        good = frame(op=CLAIM, flags=F_SESSION, request_id=99, session=b"abcd1234")
        bad = frame(op=CLAIM, flags=F_SESSION, request_id=123, session=b"abcd1234",
                    version=2)
        head_ok = b"EMTR" + bytes([1, 0]) + struct.pack("<H", 99)
        head_bad = b"EMTR" + bytes([1, 2]) + struct.pack("<H", 123)
        raw = self.run_bytes(["reply-for"], good + bad)
        self.assertEqual(raw[:12], head_ok + struct.pack("<I", zlib.crc32(head_ok)))
        self.assertEqual(raw[12:24], head_bad + struct.pack("<I", zlib.crc32(head_bad)))

    def test_late_header_rejection_also_correlates(self):
        # Same pin for a length-rule rejection, the deepest validation layer.
        bad = frame(op=CLAIM, flags=F_SESSION, request_id=77, session_len=0)
        head = b"EMTR" + bytes([1, 6]) + struct.pack("<H", 77)
        raw = self.run_bytes(["reply-for"], bad)
        self.assertEqual(raw, head + struct.pack("<I", zlib.crc32(head)))

    def test_feed_reports_consumed_and_keeps_suffix(self):
        # Public contract: feed() returns the bytes consumed and stops right
        # after a terminal event; the suffix stays unconsumed for the next
        # call and is never silently dropped (T2 readback fix 2).
        whole = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        lines = self.run_cli(["consume"], whole + b"tail").splitlines()
        self.assertEqual(lines[0], f"{len(whole)} complete")
        self.assertEqual(lines[1], "4 idle")

    def test_feed_reports_partial_progress(self):
        whole = frame(op=IMAGE, flags=F_SESSION, session=b"abcd1234", payload=png())
        lines = self.run_cli(["consume"], whole[:10]).splitlines()
        self.assertEqual(lines, ["10 active"])

    # --- CRC and bounds parity ----------------------------------------------

    def test_incremental_crc_matches_receiver_crc32(self):
        self.assertEqual(self.run_cli(["crc"], b"hello world"), "ok")
        self.assertEqual(self.run_cli(["crc"], bytes(range(256))), "ok")
        self.assertEqual(self.run_cli(["crc"], frame(op=IMAGE, flags=F_SESSION,
                                                     session=b"abcd1234", payload=png())), "ok")

    def test_bounds(self):
        self.assertEqual(self.run_cli(["bounds"], b""), str(MAX_FRAME))


if __name__ == "__main__":
    unittest.main()
