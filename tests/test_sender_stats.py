import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class SenderStatsTests(unittest.TestCase):
    def test_storage_and_connection_attempts(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "sender_stats"
            subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                            str(ROOT / "tests/sender_stats_test.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
