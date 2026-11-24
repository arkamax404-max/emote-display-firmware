import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class WeatherTests(unittest.TestCase):
    """T1 (S1, S2, S3, S5): the pure weather layer, host-tested without a
    panel — bounded hand-written JSON extraction, unit conversion, the
    micro-degree city record, day labels, alternation slots, fetch cadence
    and the URL/JSON helpers."""

    def run_case(self, case):
        # Compile per rule so the initial missing implementation is visible
        # as RED for every rule rather than hidden by one setUpClass error.
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "weather.exe"
            build = subprocess.run(
                ["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"), str(ROOT / "tests/weather_test.cpp"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary), case],
                                    capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr),
                             (0, "OK\n", ""))

    def test_unit_codes_conversion_and_rounding(self): self.run_case("convert")
    def test_forecast_json_extraction_and_rejections(self): self.run_case("parse")
    def test_geocoding_extraction_and_name_clamp(self): self.run_case("city")
    def test_day_labels_from_the_weekday(self): self.run_case("labels")
    def test_alternation_slots_at_the_configured_durations(self): self.run_case("slot")
    def test_fetch_cadence_and_millis_wrap(self): self.run_case("cadence")
    def test_paths_encoding_and_json_escaping(self): self.run_case("paths")


if __name__ == "__main__":
    unittest.main()
