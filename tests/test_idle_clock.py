import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IdleClockTests(unittest.TestCase):
    def run_case(self, case):
        # Compile per rule so the initial missing implementation is visible
        # as RED for every rule rather than hidden by one setUpClass error.
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "idle_clock.exe"
            build = subprocess.run(
                ["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"), str(ROOT / "tests/idle_clock_test.cpp"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary), case], capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "OK\n", ""))

    def test_boot_and_time_validity(self): self.run_case("boot")
    def test_lease_boundary_rejection_and_new_frame(self): self.run_case("lease")
    def test_millis_wrap_and_expiry_latch(self): self.run_case("wrap")
    def test_wifi_overlay_and_existing_sources(self): self.run_case("overlay")
    def test_redraw_pacing_and_forced_source_changes(self): self.run_case("pacing")
    def test_madrid_configuration_and_local_labels(self): self.run_case("labels")
    def test_glyph_pixels_footer_two_walkers_and_clipping(self): self.run_case("pixels")
    def test_title_phase_alternation_keeps_canonical_rows(self): self.run_case("titlephase")
    def test_title_truncates_above_sixteen_cells(self): self.run_case("truncation")
    def test_stale_no_session_restores_and_clears(self): self.run_case("nosession")


if __name__ == "__main__":
    unittest.main()
