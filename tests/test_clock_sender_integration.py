import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ClockSenderIntegrationTests(unittest.TestCase):
    def run_case(self, case):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "clock_sender_integration"
            build = subprocess.run(
                ["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"),
                 str(ROOT / "tests/clock_sender_integration_test.cpp"),
                 "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary), case], capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "OK\n", ""))

    def test_sender_attempts_do_not_renew_accepted_png_lease(self):
        self.run_case("attempts")

    def test_dynamic_sender_pages_override_clock_then_restore_current_source(self):
        self.run_case("overlay")


if __name__ == "__main__":
    unittest.main()
