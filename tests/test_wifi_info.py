import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class WifiInfoTests(unittest.TestCase):
    def test_button_debounce_timeout_pages_and_text_layout(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "wifi_info"
            subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                            str(ROOT / "tests/wifi_info_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
