import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def block(source, signature):
    return source.split(signature, 1)[1].split("\n}", 1)[0]


class UsbClockWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        cls.ingress = (ROOT / "include/serial_ingress.h").read_text(encoding="utf-8")
        cls.protocol = (ROOT / "include/serial_protocol.h").read_text(encoding="utf-8")

    def test_entry_and_reentry_use_same_ordered_capability_announcement(self):
        self.assertIn('"EMOTE_CLOCK_UTC1"', self.ingress)
        announce = block(self.ingress, "inline void printEntryAck(")
        self.assertLess(announce.index("print(CLOCK_CAPABILITY);"),
                        announce.index("print(ENTRY_ACK);"))
        self.assertIn("printEntryAck(hooks_.print);", self.ingress)
        repeat = self.main.split("case serial::Mux::Event::RepeatEntry:", 1)[1]
        self.assertIn("serial::printEntryAck(", repeat.split("break;", 1)[0])

    def test_clock_dispatch_validates_then_retires_and_presents_before_reply(self):
        self.assertIn("static bool setSerialUtc(", self.main)
        activation = block(self.ingress, "inline Code activateClock(")
        self.assertIn("session::Owner snapshot = owner;", activation)
        admission = activation.index("image_ingress::admitImage(frame, snapshot, now)")
        setting = activation.index("if (!setUtc(frame.utcSeconds)) return Code::Time;")
        self.assertLess(admission, setting)
        self.assertLess(setting, activation.index("present();"))
        rtc = block(self.main, "static bool setSerialUtc(")
        self.assertLess(rtc.index("localtime_r("), rtc.index("settimeofday("))
        self.assertIn("return settimeofday(&time, nullptr) == 0;", rtc)
        dispatch = block(self.main, "static void handleSerialFrame() {")
        clock = dispatch.split("frame.op == serial::Op::ClockUtc", 1)[1]
        self.assertIn("serial::activateClock(frame, sessionOwner, now, setSerialUtc,", clock)
        self.assertLess(clock.index("hasFrame = false;"), clock.index("presentDisplay();"))
        self.assertLess(clock.index("presentDisplay();"), clock.index("queueSerialReply("))

    def test_parser_checks_clock_shape_then_crc_before_decoding_time(self):
        self.assertIn("ClockUtc = 4", self.protocol)
        self.assertIn("Time = 12", self.protocol)
        header = self.protocol.split("Code validateHeader() {", 1)[1].split("\n  }", 1)[0]
        self.assertIn("clockUtc && flags != 0x01", header)
        self.assertIn("payloadLen != sizeof(uint32_t)", header)
        finish = self.protocol.split("Event finishFrame() {", 1)[1].split("\n  }", 1)[0]
        self.assertLess(finish.index("return fail(Code::Crc)"), finish.index("le32(payload_)"))
        self.assertIn("epoch < MIN_CLOCK_UTC || epoch > MAX_CLOCK_UTC", finish)
        self.assertIn("1577836800u", self.protocol)
        self.assertIn("2147483647u", self.protocol)

    def test_usb_clock_retains_portal_overlay_and_weather_precedence(self):
        render = block(self.main, "static void renderContent() {")
        self.assertIn("!portalActive && readClock(calendar, epoch)", render)
        source = render.index("if (source == idle_clock::Source::Clock)")
        self.assertLess(render.index("if (wifiInfo.visible())"), source)
        self.assertLess(render.index("if (connectBanner.active(millis()))"), source)
        clock = render[source:]
        self.assertLess(clock.index("if (!tud_mounted() && weatherOn && weatherForecast.valid"),
                        clock.index("weather_screen::draw("))
        self.assertIn("idle_clock::draw(", clock)
        poll = block(self.main, "static void pollWeather() {")
        self.assertLess(poll.index("if (tud_mounted()) return;"),
                        poll.index("weatherScheduler.tick("))
        self.assertIn('configTzTime(idle_clock::TIMEZONE, "pool.ntp.org", "time.cloudflare.com");',
                      self.main)


if __name__ == "__main__":
    unittest.main()
