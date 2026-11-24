import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class WeatherFetchTests(unittest.TestCase):
    """T3 (S1, S2, S5): the fetch layer, host-tested without a socket —
    URL building and escaping, the cadence state machine with injected
    timestamps, the staleness report, and the full request/response
    decision path over a fake transport."""

    def run_case(self, case):
        # Compile per rule so a missing implementation is visible as RED
        # for every rule rather than hidden by one setUpClass error.
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "weather_fetch.exe"
            build = subprocess.run(
                ["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"),
                 str(ROOT / "tests/weather_fetch_test.cpp"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary), case],
                                    capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr),
                             (0, "OK\n", ""))

    def test_forecast_and_geocoding_urls_and_query_bound(self): self.run_case("urls")
    def test_cadence_state_machine_with_injected_timestamps(self): self.run_case("cadence")
    def test_staleness_report_and_its_boundaries(self): self.run_case("stale")
    def test_forecast_application_rejects_without_partial_updates(self): self.run_case("forecast")
    def test_city_application_rejects_without_partial_updates(self): self.run_case("city")
    def test_fake_transport_drives_the_right_requests(self): self.run_case("transport")


if __name__ == "__main__":
    unittest.main()
