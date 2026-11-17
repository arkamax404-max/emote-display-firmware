import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

SESSION = b"X-Emote-Session-Id: abcd1234\r\n"
MAX_ID = b"a" * 128
OVER_ID = b"a" * 129


class SessionOwnerTests(unittest.TestCase):
    """Native session lease logic plus the fixed control protocol parsing."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.owner = pathlib.Path(cls.temp.name) / "session_owner"
        cls.protocol = pathlib.Path(cls.temp.name) / "protocol"
        for source, output in [("tests/session_owner_test.cpp", cls.owner),
                               ("tests/protocol_cli.cpp", cls.protocol)]:
            subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "include"), str(ROOT / source), "-o", str(output)],
                           check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def owner_case(self, case):
        result = subprocess.run([str(self.owner), case], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "OK\n", ""))

    def parse_session(self, body):
        output = subprocess.run([str(self.protocol), "session"], input=body,
                                capture_output=True, check=True).stdout.decode()
        status, kind, has_session, session_id = output.split(maxsplit=3)
        return int(status), kind, int(has_session), session_id.strip()

    def image(self, extra=b"", length=b"100"):
        return (b"POST /emote HTTP/1.1\r\nContent-Type: image/png\r\nX-Emote-Version: 1\r\n"
                + b"Content-Length: " + length + b"\r\n" + extra + b"\r\n")

    def control(self, route, extra=SESSION, length=b"0"):
        return (b"POST " + route + b" HTTP/1.1\r\nX-Emote-Version: 1\r\n"
                + b"Content-Length: " + length + b"\r\n" + extra + b"\r\n")

    # Native lease behavior.

    def test_lease_claim_replacement_and_expiry(self):
        self.owner_case("lease")

    def test_heartbeat_renews_owner_never_claims(self):
        self.owner_case("heartbeat")

    def test_release_idempotent_and_foreign_rejected(self):
        self.owner_case("release")

    def test_png_access_and_renewal_rules(self):
        self.owner_case("png")

    def test_identity_charset_and_bounds(self):
        self.owner_case("identity")

    def test_wrap_safe_lease_arithmetic(self):
        self.owner_case("wrap")

    # Fixed control protocol: routing, headers, identity validation.

    def test_control_routes_parse_with_session(self):
        for route, kind in [(b"/emote/claim", "claim"), (b"/emote/heartbeat", "heartbeat"),
                            (b"/emote/release", "release")]:
            with self.subTest(route=route):
                self.assertEqual(self.parse_session(self.control(route)), (200, kind, 1, "abcd1234"))

    def test_control_accepts_http_10(self):
        raw = self.control(b"/emote/claim").replace(b"HTTP/1.1", b"HTTP/1.0")
        self.assertEqual(self.parse_session(raw), (200, "claim", 1, "abcd1234"))

    def test_control_requires_session_id(self):
        for route in [b"/emote/claim", b"/emote/heartbeat", b"/emote/release"]:
            with self.subTest(route=route):
                self.assertEqual(self.parse_session(self.control(route, extra=b""))[0], 400)

    def test_control_rejects_malformed_session_ids(self):
        for value in [b"bad id", b"", b"bad/slash", b"bad+plus", OVER_ID]:
            with self.subTest(value=value[:20]):
                raw = self.control(b"/emote/claim", extra=b"X-Emote-Session-Id: " + value + b"\r\n")
                self.assertEqual(self.parse_session(raw)[0], 400)

    def test_control_rejects_duplicate_session_headers(self):
        raw = self.control(b"/emote/claim", extra=SESSION + SESSION)
        self.assertEqual(self.parse_session(raw)[0], 400)

    def test_control_rejects_body_and_missing_headers(self):
        with_length = self.control(b"/emote/claim", length=b"1")
        self.assertEqual(self.parse_session(with_length)[0], 400)
        no_length = self.control(b"/emote/claim").replace(b"Content-Length: 0\r\n", b"")
        self.assertEqual(self.parse_session(no_length)[0], 411)
        no_version = self.control(b"/emote/claim").replace(b"X-Emote-Version: 1\r\n", b"")
        self.assertEqual(self.parse_session(no_version)[0], 400)

    def test_control_rejects_unsupported_routes_and_methods(self):
        for raw in [b"POST /emote/claims HTTP/1.1\r\nX-Emote-Version: 1\r\nContent-Length: 0\r\n"
                    + SESSION + b"\r\n",
                    b"GET /emote/claim HTTP/1.1\r\n\r\n",
                    b"POST /emote/claim?x=1 HTTP/1.1\r\nX-Emote-Version: 1\r\nContent-Length: 0\r\n"
                    + SESSION + b"\r\n",
                    b"PUT /emote/claim HTTP/1.1\r\nX-Emote-Version: 1\r\nContent-Length: 0\r\n"
                    + SESSION + b"\r\n"]:
            with self.subTest(raw=raw[:40]):
                self.assertEqual(self.parse_session(raw)[0], 404)

    def test_legacy_image_without_session_still_parses(self):
        self.assertEqual(self.parse_session(self.image()), (200, "image", 0, "-"))

    def test_image_with_session_header_parses(self):
        self.assertEqual(self.parse_session(self.image(SESSION)), (200, "image", 1, "abcd1234"))

    def test_image_rejects_malformed_and_duplicate_session(self):
        self.assertEqual(self.parse_session(self.image(b"X-Emote-Session-Id: bad id\r\n"))[0], 400)
        self.assertEqual(self.parse_session(self.image(SESSION + SESSION))[0], 400)

    def test_session_id_trim_and_full_charset(self):
        raw = self.image(b"X-Emote-Session-Id:  " + MAX_ID + b"  \r\n")
        self.assertEqual(self.parse_session(raw), (200, "image", 1, MAX_ID.decode()))

    def test_health_reports_no_session(self):
        self.assertEqual(self.parse_session(b"GET /health HTTP/1.1\r\n\r\n"), (200, "health", 0, "-"))


if __name__ == "__main__":
    unittest.main()
