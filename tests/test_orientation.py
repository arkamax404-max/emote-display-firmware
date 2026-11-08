import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class OrientationTests(unittest.TestCase):
    def test_calibration_rotation_stability_and_clock_wrap(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "orientation"
            subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                            str(ROOT / "tests/orientation_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
