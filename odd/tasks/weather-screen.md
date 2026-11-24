# Weather screen alternating with the clock

Feature: in clock mode the panel alternates the clock with a weather screen split into a grid showing today and the next three days, with the clock and weather durations, the city and the temperature unit all set from the portal.

## Specs

S1. Objective, from the user (verbatim): "la nueva feature implica al modo reloj, en este modo vamos a intercalar la pantalla del reloj con otra con informacion climatologica, vamos a dividir la pantalla en una cuadricula mostransdo el tiempo para hoy y la prevision de los proximos 3 días, usa una linea vertical y horizontal para la division. El tiempo que permanece el reloj y el tiempo lo haremos configurable en el portal, asi como la seleccion de la ciudad y si la temperatura va en grados celcius o farenhei"

S2. City selection, user decision: "Texto libre con geocodificacion" — the portal takes a city name, resolves it with the geocoding API and stores the resulting latitude and longitude.

S3. Cell content, user decision: "Temperaturas y probabilidad de lluvia" — no icons; each cell shows the maximum and minimum temperature and the precipitation probability.

S4. Durations, user decision: "Reloj 20 s / tiempo 10 s" as the defaults, both configurable from the portal.

S5. TLS, user decision: "Sin validar el certificado" — the connection skips certificate validation, accepting that someone on the local network could intercept or forge the forecast. The data is not secret; the risk is accepted and recorded in the documentation.

## Implementation decisions

- Provider: Open-Meteo, free and keyless. Forecast `https://api.open-meteo.com/v1/forecast` with `daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max`, `timezone=auto`, `forecast_days=4`, `latitude`/`longitude`; `temperature_unit=fahrenheit` only in °F mode. Geocoding `https://geocoding-api.open-meteo.com/v1/search?name=<city>&count=1&language=es`.
- No new library dependencies: the JSON is small and fixed, so it is read with a bounded hand-written extractor in the style of the existing parsers, and never with a generic document tree. A dependency addition would be a separate decision.
- Grid: a 1 px vertical divider in column 32 and a 1 px horizontal divider in row 32, in the muted ink, over the whole 64x64 canvas. Cells are today (top left), +1 (top right), +2 (bottom left), +3 (bottom right). Each cell draws a three-letter day label (`HOY`, then `LUN`..`DOM` from the RTC), the maximum at scale 2, the minimum at scale 1 and the precipitation probability as `NN%` at scale 1, all through the shared brightness helper and the same 3x5 glyph table the clock uses.
- Alternation: the clock and the weather screen alternate while in clock mode, driven by the same monotonic tick the clock animation already uses, with the two durations read from settings. The weather screen is shown only when a forecast has been received at least once; until then the clock stays on screen, so a board without internet or without a city never flickers to an empty grid.
- Fetch: once the station is connected, then every 30 minutes, with a bounded connect/read timeout; a failed fetch keeps the previous forecast and never blocks the main loop. Requests are made only while connected; nothing is fetched in portal or AP mode.
- Settings, persisted in `gentle` with read-back verification and the same verified-transaction discipline already used: `clocksecs` (5..120 step 5, default 20), `weathersecs` (5..120 step 5, default 10), `units` (0 = Celsius default, 1 = Fahrenheit), `citylat`, `citylon` (integers of micro-degrees to avoid float persistence), `cityname` (up to 24 bytes). None of these keys is in the DOWN-reset scope.
- Portal `/config` gains a city text field with its own search action, the two duration fields and the unit choice, all validated with a reason on rejection and stored through the verified transaction; an invalid or failed search changes nothing.
- `/health` gains additive fields only: `weather` (whether a forecast is currently held) and the resolved city name. No existing field changes meaning.
- No new serial console command, and no change to `/emote`, the session controls, the PNG path, the diagnostics overlay, the Wi-Fi info overlay or the AP portal's existing routes.

## Tasks

T1 | S1,S2,S3,S5 | pending | Pure weather layer: bounded JSON extraction for the four daily arrays, temperature and unit conversion, the micro-degree city record and the day labels, with host tests and no Arduino symbols.
T2 | S1,S3 | pending | Weather screen rendering on a host-testable canvas: the divider lines and the four cells with label, maximum, minimum and precipitation probability, respecting the shared brightness rule.
T3 | S1,S2,S4 | pending | Fetching over TLS without validation, the 30-minute cadence and the failure behaviour, plus the geocoding search, both bounded and non-blocking.
T4 | S1,S2,S4 | pending | Portal `/config` additions (city search, the two durations, the unit), the persisted settings transaction and the alternation scheduler in the clock mode.
T5 | S1-S5 | pending | Documentation (README and docs) and the full host suite; state plainly what needs the physical panel.

## Log

L1. User request (verbatim): "la nueva feature implica al modo reloj, en este modo vamos a intercalar la pantalla del reloj con otra con informacion climatologica, vamos a dividir la pantalla en una cuadricula mostransdo el tiempo para hoy y la prevision de los proximos 3 días, usa una linea vertical y horizontal para la division. El tiempo que permanece el reloj y el tiempo lo haremos configurable en el portal, asi como la seleccion de la ciudad y si la temperatura va en grados celcius o farenhei"
L2. Research: Open-Meteo is free, keyless and non-commercial-licensed, with a documented daily-forecast endpoint and a separate geocoding endpoint; it requires HTTPS on port 443, so the board needs TLS for the first time in this project.
L3. Decisions taken by the user in this session: city by free text with geocoding (S2), cells showing temperatures and precipitation probability without icons (S3), defaults of 20 s clock and 10 s weather (S4), and no certificate validation (S5, accepted risk).
