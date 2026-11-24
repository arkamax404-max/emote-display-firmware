import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class WeatherScreenTests(unittest.TestCase):
    """T2 (S1, S3): the weather screen renderer, host-tested on a 64x64
    canvas — the divider geometry, the four cells with label/maximum/
    minimum/probability, cell isolation, the truncation rule, unit
    conversion and the shared brightness helper."""

    def run_case(self, case):
        # Compile per rule so a missing implementation is visible as RED for
        # every rule rather than hidden by one setUpClass error.
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "weather_screen.exe"
            build = subprocess.run(
                ["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"),
                 str(ROOT / "tests/weather_screen_test.cpp"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary), case],
                                    capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr),
                             (0, "OK\n", ""))

    def test_divider_lines_own_their_row_and_column(self): self.run_case("dividers")
    def test_cells_are_isolated_and_match_the_documented_layout(self): self.run_case("cells")
    def test_oversized_values_truncate_without_crossing_a_divider(self): self.run_case("truncation")
    def test_digit_counts_signs_and_probability_formatting(self): self.run_case("values")
    def test_shown_unit_converts_the_stored_tenths(self): self.run_case("units")
    def test_hoy_and_the_weekday_labels_track_the_calendar(self): self.run_case("labels")
    def test_no_valid_forecast_paints_nothing(self): self.run_case("invalid")
    def test_every_pixel_goes_through_the_brightness_helper(self): self.run_case("brightness")


if __name__ == "__main__":
    unittest.main()
