# Weather screen alternating with the clock

In clock mode, when the autonomous clock is what the panel shows (no PNG vigente, no Wi-Fi info
overlay), the panel **alternates the clock with a weather screen**: a four-cell grid showing
today and the forecast for the next three days. The clock and weather durations, the city and
the temperature unit are all set from the LAN settings page (`/config`).

Status: compiled and host-verified, **not flashed**. Everything below describes the behaviour
as built and host-tested; the end of the page lists what only the physical panel and the live
network can confirm.

## The screen

- The canvas keeps **one pixel of margin on the left (column 0) and on the top (row 0)**, and there
  are **no divider lines**: the four cells are separated by whitespace alone. Each cell owns a
  30x30 region with origins (1,1), (33,1), (1,33) and (33,33), so the bottom cell ends at row 62
  and the panel keeps a free row below it; the right border mirrors the left one.
- The four cells: **today top left, +1 top right, +2 bottom left, +3 bottom right**. Each cell
  shows, top to bottom:
  - the three-letter day label — `HOY` for today, then the weekday (`LUN`..`DOM`) taken from
    the RTC calendar for the following days,
  - the day's **maximum temperature** in large glyphs (3x5 glyph table at scale 2), the shared
    clock palette (white),
  - the **minimum temperature** at scale 2, the same size as the maximum,
  - the **precipitation probability** as `NN%` at scale 1, on the same row as the day label and
    right-aligned against the cell edge. It is **not drawn at all when the probability is zero**,
    and otherwise its ink follows the value: white from 1 to 30, cyan from 31 to 60 and intense
    blue from 61 to 100. The percent sign is a locally defined 3x5 glyph, because the shared
    clock glyph table has none and must not change.
  - the **day label** in intense violet, which is what identifies each cell.
  - an **8x8 icon** derived from the day's weather code, centred in the space the figures leave
    and dropped when a figure needs three signless digits, so a digit is never cut for a picture.
- Times are stored in tenths of a degree and rendered as rounded integers (half away from
  zero), converted between Celsius and Fahrenheit with exact integer rules when the shown unit
  differs from the unit the forecast was fetched in. **Colour carries the temperature**: intense
  blue below zero, cyan to 14, green to 24, amber to 37 and red from 38, always chosen from the
  Celsius equivalent with a single rounding so a given weather looks the same in C and in F. A
  negative figure keeps its minus sign while that leaves it at two glyphs, so -1 to -9 print it
  and -10 and below rely on the colour.
- Every pixel goes through the same shared brightness helper (`panel::color`) as the clock, the
  messages and the received PNGs, so the screen dims with the panel brightness like everything
  else.
- **The weather screen appears only once a forecast has been received** (a board without
  internet, without a resolved city or with the fetch still pending keeps showing the clock —
  it never flickers to an empty grid), **and never once that forecast is stale**: with no
  successful fetch for 26 hours (one full calendar day — the daily arrays stop describing
  "today" once midnight passes — plus twice the fetch interval as margin) the panel returns to
  the clock until the next successful fetch. When the weather screen is not shown, the clock
  renders byte-for-byte exactly as before this feature.

## The data source

[Open-Meteo](https://open-meteo.com/), free, **keyless**, non-commercial-licensed. HTTPS only
(port 443): the board needs TLS for the first time in this project, and now needs outbound
access to **two hosts**:

- Forecast — the composed URL for the default city in Celsius, exactly as the firmware sends it:

  ```
  https://api.open-meteo.com/v1/forecast?latitude=37.382830&longitude=-5.973170&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=auto&forecast_days=4
  ```

  `latitude`/`longitude` come from the stored city in micro-degrees (six fractional digits
  always), `daily` lists the three arrays the firmware reads, `timezone=auto` and
  `forecast_days=4` (today plus three). In Fahrenheit mode only, the extra
  `&temperature_unit=fahrenheit` is appended.
- Geocoding — the search the city form issues (`<city>` percent-encoded):

  ```
  https://geocoding-api.open-meteo.com/v1/search?name=<city>&count=1&language=es
  ```

The `daily` arrays each carry today plus the next three days in position order, which is exactly
the four cells; the JSON is read with a bounded hand-written extractor (no generic JSON
library, no new dependency), and any missing, malformed, null or out-of-range value rejects the
**whole** body — the previous forecast, if any, is kept and displayed unchanged.

## Cadence and failures

- The first fetch happens as soon as the station connects (outside the AP portal window), then
  at most one attempt every **30 minutes**. The forecast is held in RAM only.
- The request runs over `WiFiClientSecure` with a **5 s connect budget** (TLS handshake
  included) and **3 s of read budget**, so a fetch can block the main loop for up to about
  **eight seconds — deliberately**. It is a synchronous request from the cooperative loop, not
  a background task; nothing else about the loop's timing budget changes, and a fetch is never
  attempted while the AP portal owns the radio, while a station HTTP request is being served or
  while a PNG body is being read. In practice a successful fetch is a few hundred milliseconds
  to a couple of seconds; the budget is the worst case, once per 30 minutes.
- After a **successful** fetch the next attempt waits the full 30 minutes. After a **failure**
  (connect, read, non-200 status, oversized body, rejected body) the next attempt is after
  **60 seconds** — a bounded retry that recovers from a transient loss within a minute instead
  of waiting half an hour, without hammering the API. A failure never changes the displayed
  forecast.
- Nothing is fetched in portal or AP mode. The portal window does not shift the schedule: when
  the portal closes, a due attempt fires at once.

## Settings

All persisted in the `gentle` NVS namespace with the same read-back-verified transaction
discipline as every other setting (a put result is never trusted; a commit or a rollback is
"done" only when reading the key back proves it). **None of these keys is cleared by the DOWN
reset** (which still erases exactly `ssid`, `password` and `updir):

| Key | Type | Range / values | Default |
| --- | --- | --- | --- |
| `gentle/clocksecs` | uint8 | 5..120 s in steps of 5 | **20 s** |
| `gentle/weathersecs` | uint8 | 5..120 s in steps of 5 | **10 s** |
| `gentle/units` | uint8 | 0 = Celsius, 1 = Fahrenheit | **0 (Celsius)** |
| `gentle/citylat` | int32 | micro-degrees of latitude, |lat| ≤ 90 000 000 | — |
| `gentle/citylon` | int32 | micro-degrees of longitude, |lon| ≤ 180 000 000 | — |
| `gentle/cityname` | string | ≤ 24 bytes, UTF-8-safe clamp | — |

- The two durations set how long the clock and the weather screen each stay on the panel in
  clock mode; both selects on the page offer exactly the allowed set (5..120 step 5).
- The unit applies immediately at the next render (the shown values convert without a refetch);
  the next successful fetch stores the array in the unit it was requested in.
- **The default city is Sevilla** (user decision): latitude `37382830` and longitude `-5973170`
  micro-degrees — exactly what Open-Meteo's geocoding returns for
  `name=Sevilla&language=es` (37.38283, -5.97317, `Europe/Madrid`), verified against the API,
  not transcribed from memory. A freshly flashed board with nothing stored shows the Sevilla
  forecast without being configured. A stored city always wins over the default; a stored name
  is re-clamped on load, so a torn UTF-8 sequence cannot be read back.
- The city is chosen from the `/config` page: a text field with its own submit that posts to
  `POST /config/city` (its own route, separate from the settings form), which runs the geocoding
  request and, on success, stores the resolved record through the same verified transaction and
  re-renders the page showing the resolved city. A name that is empty or longer than 64 bytes is
  rejected **before any request could run**; a search that fails, cannot be resolved or cannot
  be stored changes nothing — no stored key, no live record — and the page says why.
- Durations and unit join the main `POST /config` form (six keys now: `brightness`, `clock`,
  `order`, `clocksecs`, `weathersecs`, `units`), each reviewed with a reason on rejection; all
  six persist through **one** verified transaction, so a failure never leaves the stored
  settings mixed across the panel and weather surfaces.

## `/health` additions

Two additive fields only; every existing field keeps its meaning:

- `"weather":<bool>` — whether a forecast is currently held in RAM (it can be `true` while the
  screen is not visible, e.g. during the clock phase of the alternation, and turns false again
  once the held forecast is stale).
- `"city":"<name>"` — the resolved city name in use (JSON-escaped; the default is `Sevilla`).

## Honest limitations

- **Certificate validation is skipped by decision** (user decision S5): the TLS connection
  accepts whatever certificate the server presents, so someone positioned to intercept the
  board's traffic could serve a forged forecast, and the board cannot tell. The data shown is
  not secret; the risk is accepted and recorded here. The HTTP server of the panel itself
  remains plain HTTP — unchanged.
- **A fetch can block the loop for up to about eight seconds** (5 s connect + 3 s read),
  deliberately, as a synchronous request from the cooperative loop rather than a background
  task. During a fetch, PNG delivery, clock update or any other loop work waits. In the
  observed API the requests complete in well under a second; the worst case happens with a
  broken or unreachable network.
- **The alternation phase is tied to absolute epoch seconds** (the same drift-free base the
  clock's title band uses), so the switch points are stable across reboots but a jump of the
  system time (NTP resync, RTC recovery) moves the phase with it.
- **A missing daily value rejects the whole forecast**: there is no partial grid. The body must
  carry all twelve values (4 days x max/min/probability) in range, or the previous forecast, if
  any, is kept.
- The forecast is plaintext JSON over an unvalidated TLS session held in RAM; nothing about it
  is authenticated, signed or cached across reboots.

## What is verified, and what is not

Verified (host suite, current at commit 2b21a71 + the T4b wiring):

- 397/398 host tests (398 with the optional cross-repository decoder test), of which 100 cover this feature across four suites: the pure layer
  (`weather.h`: extraction, conversion, city record, labels, slots, cadence), the renderer
  (`weather_screen.h`: cell geometry and margins, cell isolation, icon slot and drop rule,
  temperature and probability colour bands, units, brightness pass-through on a full-canvas
  expected image), the fetch layer
  (`weather_fetch.h`: URLs, cadence state machine, staleness, rejection discipline over a fake
  transport) and the firmware wiring (structural pins over `main.cpp`: guards, budget, six-key
  transaction, alternation gate in front of the untouched clock).
- The `POST /config` six-key transaction and the city transaction are verified against fault
  injection: a lying or lost write is caught by read-back, the rollback restores every key
  including the failing one, and absence is restored faithfully.
- Differential check against the branch point `124cbe0`: every route and behaviour this feature
  does not name (`/emote`, session controls, PNG limits, serial protocol, AP portal, DOWN reset,
  clock rendering) is byte-identical in the headers that implement them, and their test files
  are unchanged. The intended deltas are `/health`'s additive fields and the two new `/config`
  surfaces.
- PlatformIO build for `matrixportal_s3`: **SUCCESS**, RAM 58.7 % (192 428 bytes), flash 50.7 %
  (1 062 517 bytes). The ~131 KB flash growth over the branch point is dominated by **mbedTLS**
  (the WiFiClientSecure TLS stack) — the feature's own code is only a small part of it.

Not verified — needs the physical panel and the live network:

- The on-glass readability of the grid: the violet day label set against the temperature bands,
  the white/cyan/blue probability bands against the same cyan and intense blue the temperature
  scale already uses (only their position separates the two meanings), the legibility of the
  one-pixel margins at distance, and the size-2 figures next to the 8x8 icons.
- The real TLS timing against `api.open-meteo.com` (the 5 s/3 s budgets are reasoned bounds, not
  measurements) and the loop-stall feel during a worst-case fetch.
- The city search in a real browser, end to end, and the NVS survival of `citylat`/`citylon`/
  `cityname` across real reboots (the transactions are verified by read-back in the fake stores;
  the real NVS round-trip of the new typed keys is not).
- The 26-hour staleness boundary against real clock drift, and the 30-minute cadence over days.
