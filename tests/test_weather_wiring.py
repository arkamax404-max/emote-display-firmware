import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/main.cpp"


def block(source, start_marker, end_marker="\n}\n"):
    """The body of a function/static block, for containment pins."""
    return source.split(start_marker, 1)[1].split(end_marker, 1)[0]


class WeatherWiringTests(unittest.TestCase):
    """T4b: the firmware wiring of the committed weather layers, pinned
    structurally over src/main.cpp — the TLS transport with the accepted
    S5 insecurity and the tightened budget, the fetch guards (never in
    portal/AP mode, never while a station request or a PNG body is in
    flight), the scheduler-driven cadence, the byte-identical clock path
    with the alternation decision only in front of it, the city route and
    the typed storage overrides."""

    def source(self):
        return SOURCE.read_text()

    # --- the transport -------------------------------------------------------

    def test_transport_class_exists_with_the_documented_budget(self):
        source = self.source()
        self.assertIn("#include <WiFiClientSecure.h>", source)
        self.assertIn("#include \"weather.h\"", source)
        self.assertIn("#include \"weather_screen.h\"", source)
        self.assertIn("#include \"weather_fetch.h\"", source)
        transport = source.split("class WeatherHttpsTransport", 1)[1]
        transport = transport[:transport.index("\n};")]
        # S5: the accepted risk, spelled out — no certificate validation.
        self.assertIn("secure.setInsecure()", transport)
        # Port 443 (the committed constant) and the tightened budget:
        # 5 s connect, 3 s read budget (setTimeout takes whole seconds).
        self.assertIn("secure.connect(host, weather::HTTPS_PORT,", transport)
        self.assertIn("secure.setTimeout(3)", transport)
        self.assertIn("USE_WEATHER_CONNECT_TIMEOUT_MS = 5000", transport)
        self.assertIn("USE_WEATHER_READ_TIMEOUT_MS = 3000", transport)
        # GET on the committed URL shape and nothing else.
        self.assertIn('secure.printf(', transport)
        self.assertIn('"GET %s HTTP/1.1', transport)
        # Bounded raw read: twice the fetch body bound, so oversized bodies
        # are rejected by the committed application rules, never coerced.
        self.assertIn("weather_fetch::BODY_MAX * 2", transport)
        # The status line and the bounded body split come from the committed
        # helpers.
        self.assertIn("weather::httpBodyStart(", transport)

    def test_poll_weather_never_fetches_in_forbidden_windows(self):
        source = self.source()
        poll = source.split("static void pollWeather() {", 1)[1]
        poll = poll[:poll.index("\n}\n")]
        self.assertIn("if (portalActive) return;", poll)
        self.assertIn("if (WiFi.status() != WL_CONNECTED) return;", poll)
        self.assertIn("if (client) return;", poll)
        # S14: the persisted switch sits FIRST — while it is off the
        # scheduler is never ticked at all, in any mode (no portal or
        # connection check gets to decide anything): no TLS connection, not
        # even the first attempt after boot.
        self.assertIn("if (!weatherOn) return;", poll)
        self.assertLess(poll.index("if (!weatherOn) return;"),
                        poll.index("weatherScheduler.tick("))
        self.assertIn("weatherScheduler.tick(millis(), true, false)", poll)
        self.assertIn("weather_fetch::fetchForecast(weatherTransport,",
                      poll)
        # Whole-record commit on success; the previous record survives a
        # failure and the scheduler books the attempt either way.
        self.assertIn("weatherForecast = candidate;", poll)
        self.assertIn("weatherScheduler.completed(millis(), true)", poll)
        self.assertIn("weatherScheduler.completed(millis(), false)", poll)
        # Driven from the main loop, after the portal poll.
        loop = source.split("void loop() {", 1)[1]
        self.assertIn("pollWeather();", loop)
        self.assertLess(loop.index("pollWeather();"),
                        loop.index("pollClock();"))

    # --- the alternation ------------------------------------------------------

    def test_alternation_gate_sits_in_front_of_the_untouched_clock(self):
        source = self.source()
        render = source.split("static void renderContent() {", 1)[1]
        render = render[:render.index("static void presentDisplay()")]
        clock_branch = render.split("if (source == idle_clock::Source::Clock)",
                                    1)[1]
        gate = clock_branch.split("idle_clock::draw(calendar, animationTick,",
                                  1)[0]
        # The weather screen only with a HELD forecast that is NOT stale,
        # and only in the Weather phase of the committed slot rule. S14:
        # also only while the persisted switch is on — with it off the
        # clock owns the screen even if a forecast is still held.
        self.assertIn("weatherOn &&", gate)
        self.assertIn("weatherForecast.valid &&", gate)
        self.assertIn("!weatherScheduler.stale(millis())", gate)
        self.assertIn("weather::slotAt(", gate)
        self.assertIn("weather::Slot::Weather", gate)
        self.assertIn("weather_screen::draw(weatherForecast, weatherUnit,",
                      gate)
        # The clock render keeps its exact parameter sequence: byte
        # identical whenever the gate does not take the branch.
        self.assertIn("idle_clock::draw(calendar, animationTick, epochMillis,",
                      clock_branch)

    # --- the city route -------------------------------------------------------

    def test_city_search_route_is_wired_end_to_end(self):
        source = self.source()
        self.assertIn("citySavePending", source)
        classify = source.split("static bool handleConfigRequestLine() {",
                                1)[1].split("\n}\n", 1)[0]
        # Only the exact POST lines are taken; the rejection order puts the
        # city route BEFORE the settings save and after the escape.
        self.assertIn("panel_settings::Route::CitySave", classify)
        begin = source.split("static void beginCitySave() {", 1)[1].split(
            "\n}\n", 1)[0]
        # The same bounded Content-Length discipline as the settings POST.
        self.assertIn("panel_settings::parseContentLength(headers)", begin)
        self.assertIn("panel_settings::CONFIG_BODY_MAX", begin)
        self.assertIn("configBodyIsCity = true;", begin)
        apply = source.split("static void applyCitySave() {", 1)[1].split(
            "\n}\n", 1)[0]
        # The committed intake review rejects empty/oversized names BEFORE
        # any request could occur; a failed search changes nothing; only a
        # VERIFIED city store moves the live record.
        self.assertIn("panel_settings::reviewCitySearch(fields.city)", apply)
        self.assertLess(apply.index("reviewCitySearch"),
                        apply.index("weatherTransport.get("))
        self.assertIn("panel_settings::cityFromResponse(", apply)
        self.assertIn("storeWeatherCity(resolved)", apply)
        self.assertLess(apply.index("storeWeatherCity(resolved)"),
                        apply.index("weatherCity = resolved;"))
        store = source.split("static bool storeWeatherCity(", 1)[1]
        store = store[:store.index("\n}\n")]
        self.assertIn("panel_settings::storeCity(store, city)", store)
        # The served body accumulates in the same bounded settings buffer,
        # and the routing branch runs before the settings dispatch.
        poll = source.split("static void pollHttp() {", 1)[1].split(
            "\n}\n", 1)[0]
        self.assertIn("if (configBodyIsCity) applyCitySave();", poll)
        self.assertLess(poll.index("citySavePending = false;"),
                        poll.index("receiver::parseHeaders(headers, request)"))

    # --- the settings surface -------------------------------------------------

    def test_seven_key_transaction_and_live_application(self):
        source = self.source()
        apply = source.split("static void applyConfigSave() {", 1)[1].split(
            "\n}\n", 1)[0]
        for review in ("reviewBrightness", "reviewClock", "reviewOrder",
                       "clockDuration", "weatherDuration", "reviewUnits",
                       "reviewWeatherOn", "weatherSwitch"):
            self.assertIn(review, apply)
        for reason in ("Unknown clock duration.", "Unknown weather duration.",
                       "Unknown temperature unit.", "Unknown weather switch."):
            self.assertIn(reason, apply)
        # One transaction across all seven keys; the live state moves only
        # inside the verified branch.
        self.assertIn("clockDuration.seconds, weatherDuration.seconds,", apply)
        self.assertIn("unit.unit, weatherSwitch.on", apply)
        applied = apply.split("if (storePanelSettings(", 1)[1]
        for wired in ("clockSecs = clockDuration.seconds;",
                      "weatherSecs = weatherDuration.seconds;",
                      "weatherUnit = unit.unit;",
                      "weatherOn = weatherSwitch.on;"):
            self.assertIn(wired, applied)
        # S14: switching back on with a missing or stale forecast resets the
        # scheduler so the very first tick fetches immediately; a still-fresh
        # held forecast keeps the running cadence. No fetch can be in flight
        # here (pollWeather never runs while a station request is open), so
        # nothing pending is abandoned.
        self.assertIn("weatherScheduler.stale(millis())", applied)
        self.assertIn("weatherScheduler = weather_fetch::Scheduler();", applied)
        self.assertLess(applied.index("weatherOn = weatherSwitch.on;"),
                        applied.index("weatherScheduler = weather_fetch::Scheduler();"))
        # The page carries the live weather values.
        self.assertIn("configPage(panel::brightnessPercent(), clockFormat,\n"
                      "                                    channelOrder, weatherUnit, clockSecs,",
                      source)

    def test_typed_store_overrides_keep_read_back_verification(self):
        source = self.source()
        store_class = source.split("class ProvisionedSettingsStore", 1)[1]
        store_class = store_class[:store_class.index("\n};")]
        # int32 micro-degrees and the city-name string, fire-and-verify
        # writes with isKey-based reads exactly like the byte keys (this
        # core's Preferences calls its int32 accessors getInt/putInt).
        for wired in ("preferences_.getInt(key, 0)",
                      "preferences_.putInt(key, value)",
                      "preferences_.getString(key, \"\")",
                      "preferences_.putString(key, String(value.c_str()))"):
            self.assertIn(wired, store_class)

    def test_settings_load_falls_back_to_the_committed_defaults(self):
        source = self.source()
        load = source.split("static void loadWeatherSettings() {", 1)[1]
        load = load[:load.index("\n}\n")]
        self.assertIn("weather::defaultCity()", load)
        self.assertIn("panel_settings::clockSecondsOrDefault(", load)
        self.assertIn("panel_settings::weatherSecondsOrDefault(", load)
        self.assertIn("panel_settings::unitOrDefault(", load)
        self.assertIn("panel_settings::storedCityOrDefault(", load)
        # S14: the switch loads through the same fallback loader, defaulting
        # to on for an absent or invalid stored byte.
        self.assertIn('preferences.getUChar("weatheron", 255)', load)
        self.assertIn("panel_settings::weatherOnOrDefault(", load)
        setup = source.split("void setup() {", 1)[1]
        self.assertLess(setup.index("loadPanelSettings();"),
                        setup.index("loadWeatherSettings();"))

    # --- /health --------------------------------------------------------------

    def test_health_gains_only_the_two_additive_weather_fields(self):
        source = self.source()
        health = source.split("if (request.health) {", 1)[1].split("return;", 1)[0]
        self.assertIn('\\"weather\\":', health)
        self.assertIn("weather::jsonEscape(weatherCity.name)", health)
        # S14: the field keeps its exact meaning — a forecast is HELD — and
        # is independent of the persisted switch: it still reads the held
        # record, never the switch state.
        self.assertIn('weatherForecast.valid ? "true" : "false"',
                      health)
        self.assertNotIn("weatherOn", health)
        # The portal identity field keeps its position at the end.
        self.assertLess(health.index("weather"),
                        health.index('\\"portal\\":false}'))


if __name__ == "__main__":
    unittest.main()
