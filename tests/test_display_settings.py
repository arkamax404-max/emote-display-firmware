import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DisplaySettingsTests(unittest.TestCase):
    def test_brightness_colors_and_margin(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "display_settings"
            subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                            str(ROOT / "tests/display_settings_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
