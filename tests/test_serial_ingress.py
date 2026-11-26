import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_serial_protocol import (CLAIM, F_NAME, F_CLOCK, F_SESSION, HEARTBEAT,
                                  IMAGE, RELEASE, chunk, clock_utc, frame, msvc_vcvars,
                                  png)

ROOT = pathlib.Path(__file__).resolve().parents[1]


def mux_script(lines):
    return "\n".join(lines) + "\n"


def feed(data):
    return f"feed {data.hex()}"


class SerialIngressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.temp.name) / "serial_ingress"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        source = str(ROOT / "tests/serial_ingress_test.cpp")
        include = str(ROOT / "include")
        if cxx:
            subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", include, source, "-o", str(cls.binary)], check=True)
        elif msvc_vcvars():
            bat = pathlib.Path(cls.temp.name) / "compile.bat"
            bat.write_text(
                '@echo off\r\n'
                'call "{}" >nul\r\n'
                'cl /nologo /EHsc /std:c++14 /W3 /WX /D_CRT_SECURE_NO_WARNINGS '
                '/I "{}" "{}" /Fe:"{}" /Fo:"{}\\\\SerialIngressTest.obj"\r\n'.format(
                    msvc_vcvars(), include, source, cls.binary,
                    pathlib.Path(cls.temp.name) / "obj"), encoding="ascii")
            (pathlib.Path(cls.temp.name) / "obj").mkdir()
            subprocess.run(["cmd", "/c", str(bat)], check=True, capture_output=True)
        else:
            raise unittest.SkipTest("no host C++ compiler available")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_script(self, lines, mode="mux"):
        result = subprocess.run([str(self.binary), mode],
                                input=mux_script(lines).encode("latin-1"),
                                capture_output=True, check=True)
        self.assertEqual(result.stderr, b"")
        return result.stdout.decode("latin-1").splitlines()

    def dispatcher(self, raw):
        result = subprocess.run([str(self.binary), "dispatcher"], input=raw,
                                capture_output=True, check=True)
        return result.stdout.decode("latin-1").splitlines()

    @staticmethod
    def escape(raw):
        # Mirror of the CLI console-text escaping (printable ASCII kept,
        # CR/LF and everything else escaped) for exact line assertions.
        out = []
        for byte in raw:
            if byte == 0x0a:
                out.append("\\n")
            elif byte == 0x0d:
                out.append("\\r")
            elif 0x20 <= byte <= 0x7e:
                out.append(chr(byte))
            else:
                out.append(f"\\x{byte:02x}")
        return "".join(out)

    def good_image(self, request_id=1, name=None, session=b"abcd1234", payload=None):
        flags = (F_SESSION if session else 0) | (F_NAME if name else 0)
        return frame(op=IMAGE, request_id=request_id, flags=flags, session=session,
                     name=name or b"", payload=payload or png())

    # --- mux routing -------------------------------------------------------

    def test_reentry_after_idle_junk(self):
        # The entry window is a six-byte masked comparison: idle junk between
        # the magic and the LF must not prevent the re-entry re-ack.
        out = self.run_script(["enter", feed(b"junk junk" + b"EMOTE\n"), "show"])
        self.assertIn("reenter", out)
        self.assertIn("mode=binary status=idle", " ".join(out))

    def test_reentry_after_error_and_timeout(self):
        # A rejected frame and a timeout reset clear the window: the entry
        # sequence right after either must still re-ack.
        bad = frame(op=IMAGE, flags=0, payload=b"junk", crc=0xdeadbeef)
        out = self.run_script(["enter", feed(bad), feed(b"EMOTE\n"),
                               "tick 1001", feed(b"EMOTE\n"), "show"])
        self.assertEqual(out.count("reenter"), 2)

    def test_partial_exit_magic_prefix_after_timeout_is_not_console(self):
        # Boundary scope (finding 4): after a timeout reset a truncated frame
        # prefix is binary skipped bytes — it never reaches the console and a
        # plain magic never exits. Only the complete EMTE\n sequence while
        # idle exits (documented resync exit; arbitrary corrupted tails are
        # not promised protection).
        out = self.run_script(["enter", feed(b"EMTS\x01"), "tick 1001",
                               feed(b"xxEMTSxxEMTS"), "show", "tick 1001",
                               feed(b"EMTE\n"), "show"])
        self.assertEqual(out[0], "entered")
        self.assertEqual(out[1], "console -")
        self.assertEqual(out[2], "tick binary idle")    # timeout reset
        self.assertEqual(out[3], "console -")           # junk magic never console
        self.assertEqual(out[4], "mode=binary status=active skipped=2 req=0")
        self.assertEqual(out[5], "tick binary idle")    # junk frame discarded
        self.assertEqual(out[6], "exit")                # documented resync exit
        self.assertEqual(out[7], "console -")
        self.assertEqual(out[8], "mode=provision status=idle skipped=5 req=0")

    def test_provision_is_default_and_frames_never_parse_outside_binary(self):
        # Outside a binary session an EMTS frame is plain console data; the
        # parser is never fed (this preserves SSIDs/passwords starting EMTS).
        out = self.run_script([feed(self.good_image()), "show"])
        self.assertEqual(out, ["console " + self.escape(self.good_image()),
                               "mode=provision status=idle skipped=0 req=0"])

    def test_emote_line_enters_binary_via_console_event(self):
        for request in (b"EMOTE\n", b"EMOTE\r\n"):
            out = self.run_script(["cfeed " + request.hex()], mode="console")
            self.assertEqual(out, ["out EMOTE_CLOCK_UTC1", "out EMOTE_READY", "enter-binary"])

    def test_binary_bytes_never_reach_console(self):
        out = self.run_script(["enter", feed(self.good_image(request_id=5)),
                               "show", "state"])
        self.assertEqual(out, ["entered", "ack ok 5", "console -",
                               "mode=binary status=complete skipped=0 req=5",
                               "owner=0 owned=- title=- presented=1"])

    def test_dispatcher_emote_and_frame_in_same_buffered_input(self):
        # Functional dispatcher pin (finding 3): one buffered input holding
        # EMOTE\n plus a whole frame enters the session and immediately
        # processes the frame; the frame is never coerced into console lines.
        whole = self.good_image(request_id=42, name=b"lab")
        out = self.dispatcher(b"EMOTE\n" + whole)
        self.assertEqual(out, ["entered", "ack ok 42"])
        out = self.dispatcher(b"EMOTE\n" + whole + b"\nstate-dump\n")
        # The bytes after the frame stay in the mux (binary skipped bytes),
        # never routed to the console: no provisioning output appears.
        self.assertEqual(out, ["entered", "ack ok 42"])

    def test_exit_sequence_recognized_only_while_idle(self):
        # EMTE inside a valid PNG payload belongs to the frame and can never
        # trigger the exit; the same sequence while idle exits the session.
        raw = png()
        idat_body = b"EMTE\n" + zlib.compress(b"\0" + b"\xff\x00\x00\xff" * 64) + b"\x00"
        payload = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 6, 0, 0, 0))
                   + chunk(b"IDAT", idat_body) + chunk(b"IEND", b""))
        out = self.run_script(["enter", feed(self.good_image(payload=payload, name=b"lab")),
                               "state",
                               feed(b"EMTE\n"), "show"])
        self.assertEqual(out[0], "entered")
        self.assertEqual(out[1], "ack ok 1")
        self.assertEqual(out[2], "console -")
        self.assertEqual(out[3], "owner=0 owned=- title=lab presented=1")
        self.assertEqual(out[4], "exit")
        self.assertEqual(out[5], "console -")
        self.assertEqual(out[6], "mode=provision status=idle skipped=5 req=0")

    def test_reentry_ack_while_binary_idle(self):
        # A sender that missed EMOTE_READY can resend the entry line; the
        # parser resyncs and the integration re-acks instead of stranding it.
        out = self.run_script(["enter", feed(b"EMOTE\n"), "show"])
        self.assertEqual(out, ["entered", "out EMOTE_CLOCK_UTC1", "out EMOTE_READY",
                               "reenter", "console -",
                               "mode=binary status=idle skipped=0 req=0"])

    def test_clock_capability_repeated_in_binary_mode(self):
        for request in (b"EMOTE\n", b"EMOTE\r\n"):
            with self.subTest(request=request):
                out = self.run_script(["enter", feed(request[:2]), feed(request[2:]), "show"])
                self.assertEqual(out, ["entered", "console -", "out EMOTE_CLOCK_UTC1",
                                       "out EMOTE_READY", "reenter", "console -",
                                       "mode=binary status=idle skipped=0 req=0"])

    def test_inactivity_timeout_discards_tail_and_stays_binary(self):
        whole = self.good_image()
        out = self.run_script(["enter", feed(whole[:30]), "tick 1001",
                               feed(b"console garbage that must not leak"),
                               feed(whole), "state"])
        self.assertEqual(out[0], "entered")
        self.assertEqual(out[2], "tick binary idle")   # partial frame discarded
        self.assertEqual(out[3], "console -")          # tail never reaches console
        self.assertEqual(out[4], "ack ok 1")           # next valid frame succeeds
        self.assertEqual(out[5], "console -")
        self.assertEqual(out[6], "owner=0 owned=- title=- presented=1")

    def test_total_frame_timeout_bounds_a_stalled_frame(self):
        # Bytes keep flowing (so the inactivity window never fires) but the
        # frame takes longer than the total window: it is discarded, and the
        # resent whole frame then completes.
        big = self.good_image(payload=png(pad_to=8000))
        lines = ["enter"]
        for index in range(0, 3600, 300):
            lines.append(feed(big[index:index + 300]))
            lines.append("tick 900")        # 12 cycles = 10.8 s total
        lines += [feed(big), "state"]
        out = self.run_script(lines)
        ticks = [line for line in out if line.startswith("tick ")]
        self.assertEqual(ticks[:11], ["tick binary active"] * 11)
        self.assertEqual(ticks[11], "tick binary idle")
        self.assertIn("ack ok 1", out)
        self.assertIn("owner=0 owned=- title=- presented=1", out)

    def test_slow_sender_within_timeouts_completes(self):
        whole = self.good_image()
        lines = ["enter"]
        for index in range(0, len(whole), 32):
            lines.append(feed(whole[index:index + 32]))
            lines.append("tick 100")        # 100 ms gaps, ~700 ms total
        out = self.run_script(lines)
        self.assertIn("ack ok 1", out)

    def test_binary_garbage_before_frame_is_skipped_not_console(self):
        out = self.run_script(["enter",
                               feed(b"WIFI\nORIENT\n" + self.good_image(request_id=9)),
                               "state"])
        self.assertEqual(out[1], "ack ok 9")
        self.assertEqual(out[2], "console -")
        self.assertEqual(out[3], "owner=0 owned=- title=- presented=1")

    # --- image admission and commit rules ----------------------------------

    # --- T7 diagnostic indicator wiring (through the actual diag helper) ---

    def diag_state(self, lines):
        return self.run_script(lines + ["diag"])[-1]

    def test_rejected_partial_ingress_is_now_visible_red_with_counter(self):
        # Reproduces the reported invisible rejected/partial ingress on the
        # public host interface: a frame that stops mid-stream used to vanish
        # silently at the timeout; the diagnostic must show red exactly then.
        whole = self.good_image()
        out = self.run_script(["enter", feed(whole[:30]), "tick 1001", "diag"])
        self.assertEqual(out[2], "tick binary idle")
        self.assertEqual(out[3], "diag red images=0 controls=0 rejected=1")

    def test_claim_reply_is_green_control_not_image(self):
        # S10: the extension sends a claim on port open; a green from that
        # claim must never be mistaken for a delivered frame.
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        out = self.run_script(["enter", feed(claim), "diag"])
        self.assertEqual(out[3], "diag green images=0 controls=1 rejected=0")

    def test_accepted_image_counts_image_and_decode_failure_counts_rejected(self):
        good = self.good_image(name=b"lab")
        out = self.run_script(["enter", feed(good), "diag", "present fail",
                               feed(self.good_image(request_id=2)), "diag", "state"])
        self.assertEqual(out[3], "diag green images=1 controls=0 rejected=0")
        self.assertEqual(out[7], "diag red images=1 controls=0 rejected=1")
        # Old rejection guarantee unchanged: title and presentation counter.
        self.assertEqual(out[8], "owner=0 owned=- title=lab presented=1")

    def test_crc_rejection_is_red_and_changes_no_state(self):
        bad = frame(op=IMAGE, flags=0, payload=b"junk", crc=0xdeadbeef)
        out = self.run_script(["enter", feed(bad), "diag", "state"])
        self.assertEqual(out[3], "diag red images=0 controls=0 rejected=1")
        self.assertEqual(out[4], "owner=0 owned=- title=- presented=0")

    def test_emote_handshake_stays_amber_without_timeout(self):
        out = self.run_script(["enter", "tick 60000", "diag"])
        self.assertEqual(out[2], "diag amber images=0 controls=0 rejected=0")

    def test_exit_clears_pending_amber(self):
        out = self.run_script(["enter", feed(b"EMTE\n"), "diag"])
        self.assertEqual(out[3], "diag blank images=0 controls=0 rejected=0")

    def test_main_wiring_structural_callsites(self):
        # Secondary evidence only (no source-text-only proof): the functional
        # pin is the rig above, which drives the same public diag helper that
        # main.cpp wires. This check pins that the firmware actually calls it.
        source = (ROOT / "src/main.cpp").read_text()
        for call in ("serialDiag.onByte(", "serialDiag.onImageAccepted(",
                     "serialDiag.onControlAccepted(", "serialDiag.onRejected(",
                     "serialDiag.onBinaryEnter(", "serialDiag.onBinaryExit(",
                     "serialDiag.update(", "serialDiag.draw(matrix);"):
            self.assertIn(call, source)
        # One present wrapper owns overlay+show; no other matrix.show() site.
        self.assertEqual(source.count("matrix.show()"), 1)
        present = source.split("static void presentDisplay() {", 1)[1]
        present = present[:present.index("\n}")]
        self.assertLess(present.index("serialDiag.draw(matrix);"),
                        present.index("matrix.show();"))
        for field in ("serialRxImages", "serialRxControls", "serialRxRejected"):
            self.assertIn(field, source)

    def test_ownerless_image_commits_name_and_presentation(self):
        out = self.run_script(["enter", feed(self.good_image(name=b"Hello%20World")), "state"])
        self.assertEqual(out[1], "ack ok 1")
        self.assertEqual(out[3], "owner=0 owned=- title=Hello World presented=1")

    def test_image_without_name_clears_stored_title(self):
        out = self.run_script(["enter", feed(self.good_image(name=b"lab")),
                               feed(self.good_image(request_id=2)), "state"])
        self.assertEqual(out[-1], "owner=0 owned=- title=- presented=2")

    def test_rejected_presentation_changes_nothing(self):
        # A decode failure answers Png and must not renew, rename or count.
        out = self.run_script(["enter", feed(self.good_image(name=b"lab")), "present fail",
                               feed(self.good_image(request_id=2, name=b"other")),
                               "state"])
        self.assertEqual(out[4], "ack png 2")
        self.assertEqual(out[6], "owner=0 owned=- title=lab presented=1")

    def test_conflicting_image_is_busy_and_changes_nothing(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        intruder = self.good_image(request_id=2, session=b"session-b", name=b"x")
        out = self.run_script(["enter", feed(claim), "state",
                               feed(intruder), "state"])
        self.assertEqual(out[1], "ack ok 1")
        self.assertEqual(out[3], "owner=1 owned=session-a title=- presented=0")
        self.assertEqual(out[4], "ack busy 2")
        self.assertEqual(out[6], "owner=1 owned=session-a title=- presented=0")

    def test_sessionless_image_while_owned_is_busy(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        out = self.run_script(["enter", feed(claim),
                               feed(frame(op=IMAGE, flags=0, payload=png(),
                                          request_id=2)),
                               "state"])
        self.assertEqual(out[3], "ack busy 2")
        self.assertEqual(out[5], "owner=1 owned=session-a title=- presented=0")

    def test_metadata_rejection_never_reaches_admission(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        bad_name = frame(op=IMAGE, flags=F_SESSION | F_NAME, session=b"session-a",
                         name=b"%0A", payload=png(), request_id=2)
        out = self.run_script(["enter", feed(claim), feed(bad_name), "state"])
        self.assertEqual(out[3], "nack name 2")
        self.assertEqual(out[5], "owner=1 owned=session-a title=- presented=0")

    def test_clock_session_gate_and_rtc_failure_preserve_settled_frame(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        image = self.good_image(session=b"session-a", name=b"lab", request_id=2)
        out = self.run_script(["enter", feed(claim), feed(image), "clockstate",
                               feed(clock_utc(session=b"foreign")), "clockstate",
                               "rtc fail", feed(clock_utc()), "clockstate", "diag", "state",
                               "rtc ok", feed(clock_utc(2147483647)), "clockstate", "diag", "state",
                               feed(image), "clockstate"])
        self.assertEqual(out, [
            "entered", "ack ok 1", "console -", "ack ok 2", "console -",
            "utc=0 frame=1 sets=0 clock-paints=0", "ack busy 41", "console -",
            "utc=0 frame=1 sets=0 clock-paints=0", "rtc=fail", "ack time 41", "console -",
            "utc=0 frame=1 sets=1 clock-paints=0", "diag red images=1 controls=1 rejected=2",
            "owner=1 owned=session-a title=lab presented=1", "rtc=ok", "ack ok 41", "console -",
            "utc=2147483647 frame=0 sets=2 clock-paints=1", "diag green images=1 controls=2 rejected=2",
            "owner=1 owned=session-a title=lab presented=1", "ack ok 2", "console -",
            "utc=2147483647 frame=1 sets=2 clock-paints=1",
        ])

    def test_clock_does_not_claim_or_renew_the_existing_lease(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a")
        out = self.run_script(["enter", feed(clock_utc()), "state", feed(claim),
                               "tick 29999", feed(clock_utc()), "tick 1",
                               feed(clock_utc(session=b"foreign")), "clockstate"])
        self.assertEqual(out, ["entered", "ack ok 41", "console -",
                               "owner=0 owned=- title=- presented=0", "ack ok 1", "console -",
                               "tick binary complete", "ack ok 41", "console -",
                               "tick binary complete", "ack ok 41", "console -",
                               "utc=1577836800 frame=0 sets=3 clock-paints=3"])

    # --- lifecycle controls: identical Owner policy -------------------------

    def test_claim_heartbeat_release_cycle(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        beat = frame(op=HEARTBEAT, flags=F_SESSION, session=b"session-a", request_id=2)
        foreign = frame(op=HEARTBEAT, flags=F_SESSION, session=b"session-b", request_id=3)
        release = frame(op=RELEASE, flags=F_SESSION, session=b"session-a", request_id=4)
        again = frame(op=RELEASE, flags=F_SESSION, session=b"session-a", request_id=5)
        out = self.run_script(["enter", feed(claim), "state", feed(beat), feed(foreign),
                               feed(release), feed(again), "state"])
        self.assertEqual(out[1], "ack ok 1")
        self.assertEqual(out[3], "owner=1 owned=session-a title=- presented=0")
        self.assertEqual(out[4], "ack ok 2")
        self.assertEqual(out[6], "ack busy 3")
        self.assertEqual(out[8], "ack ok 4")
        self.assertEqual(out[10], "ack ok 5")  # release is idempotent
        self.assertEqual(out[12], "owner=0 owned=- title=- presented=0")

    def test_owner_image_admitted_after_claim(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        owned = self.good_image(request_id=2, session=b"session-a", name=b"lab")
        out = self.run_script(["enter", feed(claim), feed(owned), "state"])
        self.assertEqual(out[3], "ack ok 2")
        self.assertEqual(out[5], "owner=1 owned=session-a title=lab presented=1")

    def test_released_owner_image_is_busy_again(self):
        claim = frame(op=CLAIM, flags=F_SESSION, session=b"session-a", request_id=1)
        release = frame(op=RELEASE, flags=F_SESSION, session=b"session-a", request_id=2)
        later = self.good_image(request_id=3, session=b"session-a")
        out = self.run_script(["enter", feed(claim), feed(release), feed(later), "state"])
        self.assertEqual(out[5], "ack ok 3")   # ownerless again: legacy behavior
        self.assertEqual(out[7], "owner=0 owned=- title=- presented=1")

    def test_control_ignores_name_field_like_http(self):
        claim = frame(op=CLAIM, flags=F_SESSION | F_NAME, session=b"session-a",
                      name=b"%0A", request_id=1)
        out = self.run_script(["enter", feed(claim), "state"])
        self.assertEqual(out[1], "ack ok 1")
        self.assertEqual(out[3], "owner=1 owned=session-a title=- presented=0")

    # --- console line protocol preservation ---------------------------------

    def test_wifi_flow_exact_strings_and_storage(self):
        out = self.run_script([
            "cfeed " + b"WIFI\nmyssid\npassword123\n".hex(), "cshow"], mode="console")
        self.assertEqual(out[:-1], ["out WIFI_SSID", "out WIFI_PASSWORD", "out WIFI_SAVED"])
        self.assertEqual(out[-1], "store=myssid|password123 orient=0 flush=1 reboot=1")

    def test_ssid_may_be_emote_or_emts_prefixed(self):
        out = self.run_script([
            "cfeed " + b"WIFI\nEMOTE\nEMTS-secret-9\n".hex(), "cshow"], mode="console")
        # The entry command only exists at the top level; both values are
        # accepted as credentials exactly as before.
        self.assertEqual(out[:-1], ["out WIFI_SSID", "out WIFI_PASSWORD", "out WIFI_SAVED"])
        self.assertEqual(out[-1], "store=EMOTE|EMTS-secret-9 orient=0 flush=1 reboot=1")

    def test_orient_command(self):
        out = self.run_script(["cfeed " + b"ORIENT\n".hex(), "cshow"], mode="console")
        self.assertEqual(out, ["store=-|- orient=1 flush=0 reboot=0"])

    def test_carriage_returns_are_skipped(self):
        out = self.run_script(["cfeed " + b"WIFI\r\nssid1\r\n".hex()], mode="console")
        self.assertEqual(out, ["out WIFI_SSID", "out WIFI_PASSWORD"])

    def test_unknown_top_level_lines_are_ignored(self):
        out = self.run_script(["cfeed " + b"hello\nEMTS suspicious\n".hex()], mode="console")
        self.assertEqual(out, [])

    def test_line_overflow_error(self):
        out = self.run_script(["cfeed " + (b"x" * 65 + b"\n").hex()], mode="console")
        self.assertEqual(out, ["out WIFI_ERROR: line too long"])

    def test_invalid_ssid_lengths(self):
        for ssid in (b"", b"a" * 33):
            with self.subTest(ssid=ssid[:8]):
                out = self.run_script(["cfeed " + (b"WIFI\n" + ssid + b"\n").hex()],
                                      mode="console")
                self.assertEqual(out, ["out WIFI_SSID", "out WIFI_ERROR: invalid SSID length"])

    def test_invalid_password_lengths(self):
        for password in (b"short", b"p" * 64):
            with self.subTest(password=password[:8]):
                out = self.run_script(
                    ["cfeed " + (b"WIFI\nssid1\n" + password + b"\n").hex()], mode="console")
                self.assertEqual(out, ["out WIFI_SSID", "out WIFI_PASSWORD",
                                       "out WIFI_ERROR: invalid password length"])

    def test_stage_timeout_resets_to_top_level(self):
        out = self.run_script(["cfeed " + b"WIFI\n".hex(), "ctick 60001",
                               "cfeed " + b"ssid1\n".hex(),
                               "cfeed " + b"WIFI\nssid1\npassword123\n".hex(),
                               "cshow"], mode="console")
        # After the 60 s stage timeout the pending SSID line is ignored, and a
        # fresh WIFI flow works again.
        self.assertEqual(out, ["out WIFI_SSID", "ctick", "out WIFI_SSID",
                               "out WIFI_PASSWORD", "out WIFI_SAVED",
                               "store=ssid1|password123 orient=0 flush=1 reboot=1"])

    def test_empty_password_is_allowed_like_http_convention(self):
        # len==0 skips the minimum-8 rule, exactly as the existing console did.
        out = self.run_script(["cfeed " + b"WIFI\nssid1\n\n".hex(), "cshow"],
                              mode="console")
        self.assertEqual(out[-1], "store=ssid1|- orient=0 flush=1 reboot=1")


if __name__ == "__main__":
    unittest.main()
