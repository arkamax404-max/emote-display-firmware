import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_serial_protocol import msvc_vcvars

ROOT = pathlib.Path(__file__).resolve().parents[1]

# The S6 decision pins: 50 ms debounce, 5000 ms continuous hold. The erase
# scope pins follow S11: exactly ssid and password, never updir.
DEBOUNCE_MS = 50
HOLD_MS = 5000

# Fixed fake-store values (byte-identical restore assertions). `updir` is
# the unsigned byte it actually is (Preferences getUChar/putUChar), so the
# fake store represents it typed and its spec is shown in decimal — the
# defect under test restored it as an empty string instead.
SSID_VALUE = "my ssid"
PASSWORD_VALUE = "p4ssw0rd!"
UPDIR_VALUE = 3
BRIGHTNESS_VALUE = "16"


def _hexspec(value):
    """<len>:<hex> spec of a stored byte string."""
    return "%d:%s" % (len(value), value.encode("latin-1").hex())


class HoldResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = pathlib.Path(cls.temp.name)
        cls.hold = cls.work / "wifi_reset_hold"
        cls.eraseBin = cls.work / "wifi_reset_erase"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        source = str(ROOT / "tests/reset_test.cpp")
        include = str(ROOT / "include")
        if cxx:
            for binary in (cls.hold, cls.eraseBin):
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
                '/I "{}" "{}" /Fe:"{}" /Fo:"{}\\\\WifiResetTest.obj"\r\n'.format(
                    msvc_vcvars(), include, source, cls.hold, cls.work),
                encoding="ascii")
            subprocess.run(["cmd", "/c", str(bat)], check=True, capture_output=True)
            shutil.copyfile(cls.hold, cls.eraseBin)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_script(self, lines, mode="hold"):
        binary = self.hold if mode == "hold" else self.eraseBin
        result = subprocess.run([str(binary), mode],
                                input=("\n".join(lines) + "\n").encode("latin-1"),
                                capture_output=True, check=True)
        return result.stdout.decode("latin-1").splitlines()

    # --- T8: hold state machine, injected timestamps ---------------------------
    def poll(self, start, stop, step, pressed=1):
        """One hold scenario: fresh reset, raw level pressed from start."""
        lines = ["reset"]
        t = start
        while t <= stop:
            lines.append("hold %d %d" % (t, pressed))
            t += step
        return self.run_script(lines)

    def test_no_press_never_fires(self):
        out = self.poll(0, 10000, 50, pressed=0)
        self.assertEqual(out[0], "reset")
        self.assertEqual([line for line in out[1:] if line != "hold -"], [])

    def test_hold_fires_once_after_5000_continuous_ms(self):
        # Raw press at t=0 is debounced-accepted at 50 ms; the fire lands on
        # the first poll where the accepted press has lasted 5000 ms (5150).
        out = self.poll(0, 6000, 10)
        fired = [i for i, line in enumerate(out) if line == "hold fire"]
        self.assertEqual(len(fired), 1)
        # The line just before the fire was still un-failed at 5140 ms.
        self.assertLess(int((fired[0] - 2) * 10), 50 + HOLD_MS)
        self.assertEqual(out[fired[0] - 1], "hold -")
        self.assertEqual(out[fired[0]], "hold fire")

    def test_release_before_5000_discards_the_press(self):
        # 4999 ms is not enough; a release afterwards kills any later fire,
        # and a second short hold (2.9 s) never fires either.
        lines = ["reset"]
        t = 0
        while t <= 9000:
            press = t < 4990 or t >= 6000
            lines.append("hold %d %d" % (t, int(press)))
            t += 10
        out = self.run_script(lines)[1:]
        self.assertEqual([line for line in out if line != "hold -"], [])

    def test_bounce_within_debounce_does_not_abort_the_hold(self):
        # A 30 ms glitch below the 50 ms debounce window must be filtered:
        # the accepted press (and its anchor) survives.
        lines = ["reset"]
        t = 0
        while t <= 6000:
            press = t < 1000 or (1000 <= t < 1030) or t >= 1060
            # glitch: released during raw level 1000-1030, back at 1060? -- no:
            # pressed==False only in 1030-1060.
            lines.append("hold %d %d" % (t, int(press)))
            t += 10
        out = self.run_script(lines)
        fired = [i for i, line in enumerate(out) if line == "hold fire"]
        self.assertEqual(fired, [5050 // 10 + 1])  # exactly one fire, on schedule

    def test_brief_release_longer_than_debounce_resets(self):
        # A 60 ms release is a real release: the next fire needs a full new
        # 5050 ms hold from the new press.
        lines = ["reset"]
        t = 0
        while t <= 11000:
            # pressed 0-3000, released 3000-3060 (60 ms > debounce), pressed again
            press = t < 3000 or t >= 3060
            lines.append("hold %d %d" % (t, int(press)))
            t += 10
        out = self.run_script(lines)
        fired = [i for i, line in enumerate(out) if line == "hold fire"]
        # second hold: accepted at 3060+50=3110, fires at 3110+5000=8110
        self.assertEqual(fired, [(8110 // 10) + 1])

    def test_release_just_before_threshold_does_not_fire(self):
        # A release just before the threshold: raw pressed at 0/100/150, raw
        # low at 5149 and 5150. Firing requires the RAW level pressed at the
        # firing poll AND 5000 ms of continuous accepted press; the old
        # accepted-level-only check fired at 5150 (anchor 100: 5050 ms) while
        # the button was already released, erasing credentials for nothing.
        # The 5199/5200 polls show the debounced release finishing the cancel.
        lines = ["reset", "hold 0 1", "hold 100 1", "hold 150 1",
                 "hold 5149 0", "hold 5150 0", "hold 5199 0", "hold 5200 0"]
        out = self.run_script(lines)
        self.assertEqual([line for line in out[1:] if line != "hold -"], [])

    def test_short_release_glitch_during_hold_still_fires(self):
        # A 40 ms raw release (< DEBOUNCE_MS) straddling the hold completion
        # must not abort an ongoing hold; the fire is merely delayed until the
        # raw level reads pressed again.
        lines = ["reset"]
        t = 0
        while t <= 6000:
            press = t < 5000 or t >= 5040  # raw low only in [5000, 5040)
            lines.append("hold %d %d" % (t, int(press)))
            t += 10
        out = self.run_script(lines)
        fired = [i for i, line in enumerate(out) if line == "hold fire"]
        self.assertEqual(fired, [5050 // 10 + 1])  # exactly once, on schedule

    def test_hold_completing_while_pressed_fires_exactly_once(self):
        # Uninterrupted press: the fire lands on the first poll where the
        # accepted press has lasted 5000 ms and the raw level is still down,
        # and it never repeats.
        out = self.poll(0, 5100, 100)
        self.assertEqual([line for line in out if line == "hold fire"],
                         ["hold fire"])

    def test_fire_is_exactly_once_per_hold(self):
        # Continuing to hold past the fire never repeats it.
        lines = ["reset"]
        t = 0
        while t <= 12000:
            lines.append("hold %d %d" % (t, 1))
            t += 10
        out = self.run_script(lines)
        self.assertEqual([line for line in out if line == "hold fire"], ["hold fire"])

    def test_millis_wrap_still_fires(self):
        # uint32 millis() wrap around 2^32 must not break the arithmetic.
        self.assertEqual(
            self.run_script(["reset", "hold 4294967000 1", "hold 4294967050 1",
                             "hold 4754 1"]),
            ["reset", "hold -", "hold -", "hold fire"])

    # --- T8: verified erase scope ------------------------------------------------

    def erase_keys(self, present, fail=0, failwrite=0, updir=3, corrupt=0):
        # The `erase` result line only; use erase_and_dump for value-level
        # assertions. `updir` is the stored typed byte (default 3) and
        # `corrupt` is the corrupt-write mask (matching writes report success
        # but persist a different value, so the read-back differs).
        return self.run_script(["erase %d %d %d %d %d" % (
            present, fail, failwrite, updir, corrupt)], mode="erase")[:1]

    def erase_and_dump(self, present, fail=0, failwrite=0, updir=3, corrupt=0):
        return self.run_script(["erase %d %d %d %d %d" % (
            present, fail, failwrite, updir, corrupt), "dump"], mode="erase")

    def test_erase_removes_exactly_ssid_and_password(self):
        # S11: the scope is exactly ssid (bit 0) and password (bit 1).
        # updir (bit 2) and brightness (bit 3) are OUT of scope: both must
        # survive untouched.
        line = self.erase_keys(15)[0]
        self.assertEqual(line, "erase ok left=12")

    def test_erase_with_nothing_stored_is_verified(self):
        self.assertEqual(self.erase_keys(0), ["erase ok left=0"])

    def test_erase_partial_scope_is_verified(self):
        # Only some of the scope keys exist: the rest are simply absent.
        self.assertEqual(self.erase_keys(3), ["erase ok left=0"])  # ssid+password
        # ONLY updir stored (no scope key at all): a verified no-op erase
        # that must leave the byte exactly as it was.
        self.assertEqual(self.erase_keys(4), ["erase ok left=4"])
        self.assertEqual(self.erase_keys(12), ["erase ok left=12"])  # out-of-scope keys only

    def test_erase_reports_an_unverified_removal(self):
        # A simulated write failure leaves ssid in place: the outcome must be
        # "unverified" (the firmware then skips the restart). The loop stops
        # at the first unverifiable removal and restores the keys removed
        # before it, so the named keys survive as well — and the out-of-scope
        # updir byte is untouched either way.
        self.assertEqual(self.erase_keys(15, fail=1), ["erase unverified left=15"])  # ssid kept; brightness and updir were never in scope
        # Password fails: ssid was already removed and must be RESTORED
        # byte-identically before reporting failure, so every value lives on.
        self.assertEqual(self.erase_keys(15, fail=2), ["erase unverified left=15"])

    def test_failed_password_removal_restores_original_values_exactly(self):
        # The documented invariant: a failed erase leaves the stored
        # configuration exactly as it was. Values are snapshotted before
        # mutating and the keys already removed are written back — `updir`
        # as the same unsigned byte it was before, never an empty value.
        out = self.erase_and_dump(15, fail=2)  # password (bit1) removal fails
        self.assertEqual(out[0], "erase unverified left=15")
        self.assertEqual(
            out[1],
            "dump ssid=%s password=%s updir=%d brightness=%s" % (
                _hexspec(SSID_VALUE), _hexspec(PASSWORD_VALUE),
                UPDIR_VALUE, _hexspec(BRIGHTNESS_VALUE)))

    def test_updir_and_brightness_survive_the_erase_byte_identical(self):
        # S11: the upright reference is deliberately OUT of the reset scope —
        # a hold that only means "forget the Wi-Fi" must not unseat the
        # panel's rotation (the /config recalibration route clears the
        # reference deliberately). Both stored byte edges survive: the
        # scenario default 3 and the byte 0 (present, never absent).
        out = self.erase_and_dump(15)  # updir stored as 3
        self.assertEqual(out[0], "erase ok left=12")
        self.assertEqual(
            out[1],
            "dump ssid=- password=- updir=3 brightness=%s"
            % _hexspec(BRIGHTNESS_VALUE))
        out = self.erase_and_dump(15, updir=0)  # updir stored as byte 0
        self.assertEqual(out[0], "erase ok left=12")
        self.assertEqual(
            out[1],
            "dump ssid=- password=- updir=0 brightness=%s"
            % _hexspec(BRIGHTNESS_VALUE))

    def test_unverifiable_restore_write_reports_failure(self):
        # A restore write that cannot be persisted must not be ignored: the
        # erase reports failure (the caller then skips the restart), and the
        # store keeps the keys the restore could not write back.
        # ssid was removed, the password removal failed, and writing ssid
        # back fails: overall "unverified", ssid stays absent.
        out = self.erase_and_dump(15, fail=2, failwrite=1)
        self.assertEqual(out[0], "erase unverified left=14")
        self.assertEqual(
            out[1],
            "dump ssid=- password=%s updir=%d brightness=%s" % (
                _hexspec(PASSWORD_VALUE), UPDIR_VALUE,
                _hexspec(BRIGHTNESS_VALUE)))

    def test_restore_write_lies_but_read_back_catches_it(self):
        # The restore write reports success while the subsequent read-back
        # returns something else (the corrupt-write mode stores value+"*"),
        # so the erase must stay unverified — the only way the routine can
        # know the write lied is the read-back, so an unverified verdict here
        # is itself the proof the read-back actually happened — and the
        # caller must not restart (pinned for every unverified erase by
        # test_no_restart_when_erase_unverified). The dump shows the lying
        # value did land in the store: the write was not a silent no-op.
        out = self.erase_and_dump(15, fail=2, corrupt=1)  # ssid restore lies
        self.assertEqual(out[0], "erase unverified left=15")
        self.assertEqual(
            out[1],
            "dump ssid=%s password=%s updir=3 brightness=%s" % (
                _hexspec(SSID_VALUE + "*"), _hexspec(PASSWORD_VALUE),
                _hexspec(BRIGHTNESS_VALUE)))

    def test_success_path_removes_exactly_the_scope_keys(self):
        # The success path removes exactly the two scope keys with verified
        # read-back; updir and brightness byte-identically untouched.
        out = self.erase_and_dump(15, fail=0)
        self.assertEqual(out[0], "erase ok left=12")
        self.assertEqual(
            out[1],
            "dump ssid=- password=- updir=3 brightness=%s"
            % _hexspec(BRIGHTNESS_VALUE))


class MainWiringStructuralTests(unittest.TestCase):
    """Secondary evidence only (test_main_wiring_structural_ precedent): the
    DOWN poller is wired in setup/loop with the variant pin, the erase goes
    through the pure routine, and the restart is gated on verified removal."""

    def setUp(self):
        self.source = (ROOT / "src/main.cpp").read_text()

    def test_down_pin_is_the_variant_macro_as_input_pullup(self):
        self.assertIn("PIN_BUTTON_DOWN", self.source)
        self.assertIn("pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);", self.source)

    def test_down_poller_runs_in_loop(self):
        loop = self.source.split("void loop() {", 1)[1]
        loop = loop[:loop.index("\n}")]
        self.assertIn("pollCredentialsReset();", loop)

    def test_restart_only_after_verified_erase(self):
        poll = self.source.split("static void pollCredentialsReset()", 1)[1]
        poll = poll[:poll.index("\n}")]
        self.assertIn("clearProvisionedCredentials()", poll)
        self.assertLess(poll.index("clearProvisionedCredentials()"),
                        poll.index("ESP.restart()"))
        self.assertIn("wifi_reset::eraseCredentials", self.source)

    def test_no_restart_when_erase_unverified(self):
        # ESP.restart() sits inside the verified branch and nothing follows it:
        # an unverified erase just returns, and the next hold retries.
        poll = self.source.split("static void pollCredentialsReset()", 1)[1]
        poll = poll[:poll.index("\n}")]
        self.assertIn("if (clearProvisionedCredentials()) ESP.restart();", poll)
        self.assertEqual(poll.count("ESP.restart()"), 1)

    def test_adapter_touches_only_the_scope_keys(self):
        # S11: the reset scope is exactly ssid and password, both strings.
        # The adapter no longer serves any byte key (the typed accessors are
        # gone from the store interface): the orientation byte is only ever
        # cleared by the /config recalibration route, never by the DOWN hold.
        adapter = self.source.split("class ProvisionedPrefs", 1)[1]
        adapter = adapter[:adapter.index("\n};")]
        self.assertNotIn("updir", adapter)
        self.assertNotIn("getUChar", adapter)
        self.assertNotIn("putUChar", adapter)
        # ssid/password remain string-typed round-trips.
        self.assertIn("preferences_.getString(", adapter)
        self.assertIn("preferences_.putString(", adapter)

    def test_reset_header_scope_names_only_ssid_and_password(self):
        # The scope is defined by the host-tested header: exact case, exactly
        # the two string keys, and no byte accessor at all (which is what
        # makes touching updir impossible by construction). updir stays
        # unquoted in the header's prose as the documented out-of-scope key.
        header = (ROOT / "include/wifi_credentials_reset.h").read_text()
        erase = header.split("inline bool eraseCredentials", 1)[1]
        self.assertIn('"ssid"', erase)
        self.assertIn('"password"', erase)
        self.assertNotIn('"updir"', erase)
        self.assertNotIn("readUChar", erase)
        self.assertNotIn("writeUChar", erase)
        self.assertNotIn("updir", erase)


class OrientationFirstPaintStructuralTests(unittest.TestCase):
    """Delegated structural check (defect 2, this session): setup() paints the
    first panel content (SET WIFI / VIA USB, and the portal slides when the
    portal opens with no credentials) before the first accelerometer sample,
    so that first paint always uses rotation 0 and can stay visibly rotated
    until an unrelated redraw — up to 5 s in the portal. pollOrientation must
    repaint once on the first successful orientation sample, without waiting
    for a content change. Secondary evidence only, like
    MainWiringStructuralTests; presentDisplay() callers, the pinned 10-pixel
    diagnostic overlay and the Wi-Fi info overlay stay untouched."""

    def setUp(self):
        self.source = (ROOT / "src/main.cpp").read_text()
        poll = self.source.split("static void pollOrientation()", 1)[1]
        self.poll = poll[:poll.index("\n}")]

    def test_repaint_happens_on_the_first_successful_sample(self):
        # The first-sample repaint is its own branch (not gated on `changed`)
        # and runs after orientation.update, so it reflects the reference and
        # rotation first known.
        self.assertIn("if (changed)", self.poll)
        branch = self.poll.split("else if (firstSample)", 1)[1]
        branch = branch[:branch.index("\n  }")]
        self.assertIn("presentDisplay()", branch)
        self.assertLess(self.poll.index("orientation.update"),
                        self.poll.index("else if (firstSample)"))

    def test_first_sample_flag_survives_a_failed_getEvent(self):
        # A failed accelerometer read is not a sample: the first repaint must
        # still happen on the first successful one.
        self.assertLess(
            self.poll.index("if (!accelerometer.getEvent(&event)) return;"),
            self.poll.index("firstSampleSeen = true;"))
    def test_down_poller_is_silent_and_never_blocks_serial(self):
        # The poller runs in every mode and must not touch or hold the serial
        # link that the provisioning console and the frame path own.
        poll = self.source.split("static void pollCredentialsReset()", 1)[1]
        poll = poll[:poll.index("\n}")]
        self.assertNotIn("Serial.", poll)
        self.assertNotIn("delay(", poll)

    def test_empty_password_restore_is_readback_verified(self):
        # putString() returns zero for a legitimately empty password, so the
        # write must be checked by reading the key back, never by its return.
        adapter = self.source.split("class ProvisionedPrefs", 1)[1]
        adapter = adapter[:adapter.index("\n};")]
        self.assertNotIn("putString(key, String(value.c_str())) != 0", adapter)
        self.assertIn("preferences_.isKey(key)", adapter)
        self.assertIn('preferences_.getString(key, "")', adapter)


if __name__ == "__main__":
    unittest.main()
