import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class FirmwareVersionTests(unittest.TestCase):
    def test_ready_version_and_unchanged_diagnostics(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = pathlib.Path(temp) / "firmware_version"
            build = subprocess.run(
                ["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "include"),
                 str(ROOT / "tests/firmware_version_test.cpp"), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "OK\n", ""))

    def test_version_is_only_integrated_in_diagnostic_branch(self):
        source = (ROOT / "src/main.cpp").read_text()
        render = source.split("static void renderDisplay() {", 1)[1].split("static void pollClock()", 1)[0]
        diagnostic = render.split("if (!hasFrame) {", 1)[1].split("return;", 1)[0]
        call = "firmware::drawVersion(matrix, messageFirst, messageSecond);"
        self.assertEqual(source.count(call), 1)
        self.assertIn(call, diagnostic)
        self.assertLess(diagnostic.index("matrix.print(messageSecond);"), diagnostic.index(call))
        self.assertLess(diagnostic.index(call), diagnostic.index("matrix.show();"))
        self.assertLess(render.index("if (wifiInfo.visible())"), render.index("if (!hasFrame)"))
        self.assertLess(render.index("source == idle_clock::Source::Clock"), render.index("if (!hasFrame)"))
        for line in ("matrix.setCursor(2, 12 + panel::TOP_OFFSET);",
                     "matrix.print(messageFirst);",
                     "matrix.setCursor(2, 28 + panel::TOP_OFFSET);",
                     "matrix.print(messageSecond);"):
            self.assertIn(line, diagnostic)


if __name__ == "__main__":
    unittest.main()
