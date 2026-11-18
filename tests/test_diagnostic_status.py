import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def panel_color(color565):
    # Mirror of panel::color (display_settings.h): brightness-scaled channels.
    red = ((color565 >> 11) * 64 + 127) // 255
    green = (((color565 >> 5) & 63) * 64 + 127) // 255
    blue = ((color565 & 31) * 64 + 127) // 255
    return (red << 11) | (green << 5) | blue


def color565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


GREEN = panel_color(color565(0, 255, 0))
RED = panel_color(color565(255, 0, 0))
AMBER = panel_color(color565(255, 191, 0))

# The 4 px right triangle: row lengths 4/3/2/1, all flush right at x63.
TRIANGLE = [(60, 0), (61, 0), (62, 0), (63, 0),
            (61, 1), (62, 1), (63, 1),
            (62, 2), (63, 2),
            (63, 3)]


class DiagnosticStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = pathlib.Path(cls.temp.name) / "diagnostic_status"
        cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
        if not cxx:
            raise unittest.SkipTest("no host C++ compiler available")
        subprocess.run([cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "include"),
                        str(ROOT / "tests/diagnostic_status_test.cpp"),
                        "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_script(self, lines):
        result = subprocess.run([str(self.binary)],
                                input="\n".join(lines).encode("ascii"),
                                capture_output=True, check=True)
        return result.stdout.decode("ascii").splitlines()

    # --- no traffic never rejects -------------------------------------------

    def test_no_traffic_never_rejects_even_across_millis_wrap(self):
        out = self.run_script(["setnow 4294963000", "tick 20000", "signal", "counters"])
        self.assertEqual(out, ["tick 0", "signal blank",
                               "counters images=0 controls=0 rejected=0"])

    def test_first_byte_arms_pending_but_counters_stay_zero(self):
        out = self.run_script(["setnow 4294963000", "byte", "signal", "counters"])
        self.assertEqual(out, ["signal amber",
                               "counters images=0 controls=0 rejected=0"])

    # --- timeout windows from the first RX byte ------------------------------

    def test_first_byte_just_before_inactivity_timeout_stays_amber(self):
        out = self.run_script(["byte", "tick 999", "signal", "tick 1", "signal",
                               "counters"])
        self.assertEqual(out, ["tick 1", "signal amber", "tick 1", "signal red",
                               "counters images=0 controls=0 rejected=1"])

    def test_byte_refreshes_inactivity_but_total_window_ends_from_first_byte(self):
        lines = ["byte", "sync"]
        for _ in range(11):  # 11 x 900 ms of live traffic = 9900 ms total
            lines += ["tick 900", "byte"]
        lines += ["signal", "tick 100", "signal", "counters"]
        out = self.run_script(lines)
        # 9900 ms in: inactivity is only 0 ms (byte just arrived), total not
        # yet 10 s: still amber, not red.
        self.assertEqual(out[:11], ["tick 0"] * 11)
        self.assertEqual(out[11], "signal amber")
        # 10000 ms total from the FIRST RX byte: red even though the sender
        # never stalled for the full inactivity window.
        self.assertEqual(out[12], "tick 1")
        self.assertEqual(out[13], "signal red")
        self.assertEqual(out[14], "counters images=0 controls=0 rejected=1")

    # --- 750 ms terminal hold -------------------------------------------------

    def test_hold_749_green_then_750_amber_with_pending_traffic(self):
        out = self.run_script(["byte", "image", "sync", "signal", "byte",
                               "tick 749", "signal", "tick 1", "signal",
                               "counters"])
        self.assertEqual(out, ["signal green", "tick 0", "signal green",
                               "tick 1", "signal amber",
                               "counters images=1 controls=0 rejected=0"])

    def test_hold_boundary_even_without_new_byte_at_that_instant(self):
        # Traffic during the hold that then goes silent: amber exactly when the
        # hold ends, driven by the loop update, not by a new byte.
        out = self.run_script(["byte", "image", "sync", "byte", "tick 750",
                               "signal"])
        self.assertEqual(out, ["tick 1", "signal amber"])

    def test_no_traffic_after_completion_keeps_terminal_forever(self):
        out = self.run_script(["byte", "image", "sync", "tick 749", "signal",
                               "tick 750", "signal", "tick 60000", "signal",
                               "counters"])
        self.assertEqual(out, ["tick 0", "signal green", "tick 0", "signal green",
                               "tick 0", "signal green",
                               "counters images=1 controls=0 rejected=0"])

    def test_new_terminal_replaces_terminal_immediately(self):
        out = self.run_script(["control", "sync", "signal", "tick 100", "signal",
                               "reject", "signal", "counters"])
        self.assertEqual(out, ["signal green", "tick 0", "signal green",
                               "signal red",
                               "counters images=0 controls=1 rejected=1"])

    def test_stalled_traffic_during_hold_times_out_red(self):
        out = self.run_script(["byte", "image", "sync", "byte", "tick 1000",
                               "signal", "counters"])
        self.assertEqual(out, ["tick 1", "signal red",
                               "counters images=1 controls=0 rejected=1"])

    # --- counters --------------------------------------------------------------

    def test_claim_green_increments_only_controls_not_images(self):
        out = self.run_script(["byte", "control", "signal", "counters"])
        self.assertEqual(out, ["signal green",
                               "counters images=0 controls=1 rejected=0"])

    def test_image_green_increments_only_images(self):
        out = self.run_script(["byte", "image", "signal", "counters"])
        self.assertEqual(out, ["signal green",
                               "counters images=1 controls=0 rejected=0"])

    def test_counters_saturate_at_uint32_max(self):
        out = self.run_script(["setcounters 4294967295 4294967295 4294967294",
                               "image", "control", "reject", "counters"])
        self.assertEqual(out, ["counters images=4294967295 controls=4294967295 "
                               "rejected=4294967295"])

    # --- binary session boundaries ----------------------------------------------

    def test_emote_handshake_is_not_an_accepted_operation_and_never_times_out(self):
        # The handshake arms no timeout: amber stays pending until a binary
        # operation instead of turning red while the sender prepares a frame.
        out = self.run_script(["byte", "enter", "sync", "tick 60000", "signal",
                               "counters"])
        self.assertEqual(out, ["tick 0", "signal amber",
                               "counters images=0 controls=0 rejected=0"])

    def test_exit_boundary_clears_pending_amber_but_keeps_terminal(self):
        out = self.run_script(["byte", "enter", "exit", "signal"])
        self.assertEqual(out[-1], "signal blank")
        out = self.run_script(["byte", "image", "exit", "signal", "counters"])
        self.assertEqual(out[-2:], ["signal green",
                                    "counters images=1 controls=0 rejected=0"])

    # --- overlay draw -------------------------------------------------------------

    def test_green_overlay_draws_exactly_ten_pixels_at_exact_coordinates(self):
        out = self.run_script(["image", "draw"])
        pixels = [line.split(" ", 1)[1] for line in out[:-1]]
        self.assertEqual(out[-1], "draw 10")
        self.assertEqual(pixels, [f"{x},{y},{GREEN}" for x, y in TRIANGLE])

    def test_overlay_colors_per_signal(self):
        for command, color in (("control", GREEN), ("byte", AMBER), ("reject", RED)):
            with self.subTest(command):
                out = self.run_script([command, "draw"])
                pixels = [line.split(" ", 1)[1] for line in out[:-1]]
                self.assertEqual(out[-1], "draw 10")
                self.assertEqual(pixels, [f"{x},{y},{color}" for x, y in TRIANGLE])

    def test_blank_draws_nothing(self):
        out = self.run_script(["draw"])
        self.assertEqual(out, ["draw 0"])

    def test_overlay_does_not_write_any_other_framebuffer_pixel(self):
        # "draw 10" pins the exact write count: no content pixel outside the
        # ten triangle coordinates is ever touched by the diagnostic overlay.
        out = self.run_script(["byte", "image", "draw"])
        self.assertEqual(out[-1], "draw 10")


if __name__ == "__main__":
    unittest.main()
