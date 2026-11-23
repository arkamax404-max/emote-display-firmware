# Wi-Fi captive portal fallback

What the firmware does when it cannot join its saved Wi-Fi: after an exact,
bounded boot-time retry schedule it opens a **WPA2 SoftAP** (`gentle-matrix`)
with a captive portal (DNS wildcard responder plus a configuration page on
port 80) so the network can be reconfigured from a phone. The page offers a
**selector of the networks a fresh scan found**, and holding the board's
**DOWN button for 5 seconds** erases the saved credentials and restarts.
Implemented in `include/portal_policy.h` (host-testable decisions),
`include/portal_page.h` (host-testable HTML renderer),
`include/wifi_credentials_reset.h` (host-testable hold state machine and
verified erase), `include/rgb_order.h` (the S7 panel channel-order mapping),
`include/panel_settings.h` (the S8 LAN settings page: form parsing, value
review, route classification, page rendering) and `src/main.cpp`. Covered by
`tests/test_portal.py`, `tests/test_reset.py`, `tests/test_rgb_order.py` and
`tests/test_settings.py`. Task
`odd/tasks/wifi-captive-portal-fallback.md`, T1-T14; T6 (UF2 + device write)
is separately authorized.

## When the portal opens

Evaluated **once at boot**, never retroactively:

1. **No stored SSID**: if a USB host is enumerated, the existing
   `SET WIFI / VIA USB` panel message and the serial provisioning assistant
   stay authoritative — no portal. If **no** USB host is enumerated, the
   portal opens immediately (nothing to exhaust).
2. **Saved SSID**: the board gets **4 connection attempts of 10 s each,
   separated by 5 s pauses** (worst case `4 × 10 s + 3 × 5 s = 55 s`, the
   setup `WiFi.begin` counts as attempt one). If every attempt fails and
   **no** USB host is enumerated, the portal opens. If a USB host is
   enumerated, no portal — re-provision over USB as before.
3. **A USB host appearing later does not cancel a portal already open.**

"Enumerated by a USB host" is tested with TinyUSB's `tud_mounted()` — the
real bus enumeration — never with `Serial`'s `operator bool()`, which is
true only with DTR+RTS asserted and would mistake a charger or a closed
terminal for a computer.

**Boot only.** A network that drops while the board is running keeps
today's runtime reconnect loop (`WiFi.reconnect()` every 15 s) and never
opens the portal; the portal appears at the next reboot if it cannot
reconnect.

## The AP and the password

- SoftAP `gentle-matrix`, **WPA2** (`WiFi.softAP` with a password), channel
  1, at most 4 clients, address `192.168.4.1`. While the portal is open the
  radio runs in `WIFI_AP_STA`: the AP and the phone's connection stay up and
  the station interface is available for the page's fresh scans.
- The AP password is **exactly 8 characters** — the WPA2 PSK minimum —
  generated once with `esp_random()` from the alphabet
  `ABCDEFGHJKMNPQRSTUVWXYZ23456789` (uppercase letters and digits without
  the visually confusable pairs `0`/`O` and `1`/`I`/`L`; every symbol is a
  glyph the classic 5×7 GFX font that `displayMessage` uses actually draws).
- The password is shown on the LED panel. The `displayMessage` path has a
  2-line/10-character budget, so the panel rotates three slides every 5 s:
  `AP: GENTLE` + the password, `PORTAL` + `WIFI SETUP`, and the address
  `192.168.` + `4.1` (the IP is split exactly like the Wi-Fi info overlay
  does; the password is never truncated).
- The password is never stored: nothing secret is added to NVS and a fresh
  one is drawn every boot. It is never echoed into the HTML, never printed
  to the serial log and never included in `/health`.

**Accepted tradeoff:** anyone with line of sight to the panel can read the
AP password while it is displayed. That is the intended design (S3): the
password exists so a person standing at the board can join without a data
cable. The board connects afterwards to the user's own network with the
stronger handling below.

## The network selector

The configuration page lists the networks the scan found; no manual SSID
field exists (the user removed it). The selection rules, host-tested in
`portal::selectNetworks` (`include/portal_policy.h`):

- **One fresh scan per page render**: `WiFi.scanNetworks()` runs each time
  the form is rendered, so every page load is a fresh list; the "Scan
  again" link simply reloads the page. The page is served after the scan.
- A scan blocks for a few seconds and can disturb the AP briefly; the
  validation attempt itself **never scans**, so a page served while the
  20 s credential check runs lists no networks and the check is not
  disturbed.
- Every visible network becomes a **radio button of the single `ssid`
  field**; the escape rules apply to both the value and the label. The
  offered values are reviewed with the same `1-32 bytes` rule the serial
  console enforces, and the per-render CSRF token is unchanged.
- The raw scan results are **de-duplicated by SSID** (the strongest RSSI of
  each name survives — a network seen on two bands is one choice), **dropped
  when the SSID is empty** (hidden networks are not selectable anywhere in
  the portal), **sorted by descending RSSI** (input order breaks ties) and
  **capped at 12 entries** to bound the page.
- If the list is empty the page says so and asks for a reload; reloading
  rescans.
- **Consequence stated honestly:** a hidden or out-of-range network cannot
  be configured from the portal; USB provisioning
  (`scripts/provision_wifi.py`) remains the route for those.
- After the scan the raw results buffer is deleted (`WiFi.scanDelete()`) so
  memory stays bounded.

## Validation before saving

`POST /save` (form fields `ssid`, `password`) never writes first:

1. The offered values are reviewed with the same rules the serial console
   enforces: SSID 1-32 bytes, password 8-63 bytes or empty. A rejected
   re-render shows the reason (`invalid SSID length` /
   `invalid password length`) and touches nothing.
2. The board switches to `WIFI_AP_STA` (the AP and the phone's connection
   stay up), calls `WiFi.begin` with the **offered** values and waits up to
   **20 s** for `WL_CONNECTED`. The panel shows `TRY WIFI / CHECK`.
3. **Success**: the offered credentials are written to the `gentle`
   namespace (`ssid`/`password`) with the same read-back verification the
   serial hook uses, a "saved, restarting" page is served, and the firmware
   restarts to join the network.
4. **Failure**: the station disconnects and goes idle (the portal keeps the
   radio in `WIFI_AP_STA` so the next page render rescans), the stored
   credentials are left untouched, the offered buffers are zeroed and the
   page re-renders — with a fresh scan's network list — and the error. The
   portal stays open for another try.

The offered home-network passphrase keeps the stronger secret handling: it
is never logged, never echoed into the HTML, never shown on the panel, and
the buffers holding it are zeroed as soon as the validation attempt
concludes (both outcomes). Nothing about it is exposed in `/health`.

## The page and the routes

- `GET` **any** path returns the form page (200, HTML) — a fresh scan's
  network list plus the password field — so operating-system
  captive-portal probe URLs (`/generate_204`, `/ncsi.txt`, ...) land on it
  and trigger the portal prompt.
- `GET /health` returns `{"ok":true,"portal":true,"apSsid":"gentle-matrix",
  "validation":<active>}` — portal identity for the diagnostics surface,
  never any password.
- `POST /save` handles the form as described above; other methods/paths are
  rejected without state changes.
- The `DNSServer` on port 53 answers every query with `192.168.4.1`
  (wildcard), which is what makes captive detection fire.

The portal server is a **separate** `WiFiServer` on port 80, strictly
mutually exclusive with the station server: while the portal is open the
station server is never begun, and the portal server is begun only while
the station server is not listening. The station's HTTP routes, mDNS and
`/emote` behavior are untouched.

## Display and diagnostics in portal mode

- The portal message is pinned: the autonomous clock does not take the
  panel back in portal mode (a surviving powered RTC would otherwise hide
  the password slides after a soft restart) until the rotated slides return
  to the password slide.
- The pinned 10-pixel serial RX diagnostic overlay, session ownership,
  PNG reception, orientation handling and the Wi-Fi info button overlay all
  work unchanged; `pollProvisioning` remains the only reader of `Serial`.
  The console gains one single-line command, `RGB <order>` (see below);
  nothing else about the console protocol changes.
- During validation the panel shows `TRY WIFI / CHECK`; on successful save
  `WIFI / SAVING` until the restart.

## Clearing the saved credentials: hold DOWN for 5 seconds

`PIN_BUTTON_DOWN` from the variant's own `pins_arduino.h` (**GPIO 7**,
read as `INPUT_PULLUP`, active low — nothing else in the firmware claims
that button; the UP button of the Wi-Fi info overlay is GPIO 6):

- The level is read every loop and debounced **50 ms**; the reset fires when
  the debounced press has lasted **5000 ms** continuously. Any release
  before that discards the press, and a glitch shorter than the debounce
  window cannot abort a running hold.
- The poller runs **in every mode**: the hold clears and restarts whether
  the board is connected, retrying at boot or serving the portal. It never
  reads or blocks `Serial` and does not disturb the UP-button overlay or the
  orientation handling.
- Exactly three keys are removed from the `gentle` namespace, each verified
  gone by **read-back**: `ssid`, `password` and `updir`. Brightness and
  every other stored value are untouched, and nothing is written outside
  that namespace. (Removing `updir` is part of the user-agreed scope: the
  upright reference is relearned on the first stable pose after the
  restart, as at the first boot.)
- **No panel message is shown** (the user chose silence): the restart is
  the only feedback. If a removal cannot be verified, the board does **not**
  restart — the configuration is left exactly as it was and another 5 s
  hold retries.
- After the restart with no credentials stored, the boot gate decides as
  always: with a USB host enumerated, `SET WIFI / VIA USB`; without one, the
  portal opens immediately.

## Panel channel order (colours)

`src/main.cpp` wires the six HUB75 data pins as R1 G1 B1 R2 G2 B2 on GPIOs
42, 41, 40, 38, 39, 37. Some panels connect those inputs to different LED
colours, so what the firmware draws as red can come out green (this specific
panel does). The firmware keeps that default (`RGB`) and adds the **six
named channel orders** — `RGB`, `RBG`, `GRB`, `GBR`, `BRG`, `BGR` — chosen
persistently and applied at every boot when the panel object is
constructed:

- The pure mapping lives in `include/rgb_order.h`, host-tested in
  `tests/test_rgb_order.py`: the order **names the colour sequence the panel
  lights on its three data inputs of each row, in the base wiring order**;
  `RGB` is therefore the untouched default, and the firmware then drives
  each Protomatter colour slot with the base pin that reaches the panel
  input which lights that colour. All six permutations are complete and
  distinct; a wrong pick just shows wrong colours again and never harms
  the panel, so the choice can simply be cycled until the colours come out
  right.
- The choice is persisted as a small code (0–5) under its own key
  `rgborder` in the `gentle` namespace, verified by read-back like the
  credential store, and read at boot before the panel is constructed (the
  Protomatter constructor copies the pin list, so the order must exist
  before the panel object exists). An absent or unreadable value falls
  back to `RGB`: existing boards are unchanged.
- **USB console**: send `RGB <order>` as a single provisioning line (the
  order name is case-insensitive, e.g. `RGB GRB` or `RGB bgr`). Success
  answers `RGB_SAVED` and the board restarts; an unparseable name answers
  `RGB_ERROR: unknown channel order` and a store that cannot be verified
  answers `RGB_ERROR: storage failed` — either error changes no stored
  state and does not restart.
- **Portal**: the configuration page gains a second, separate form
  ("Panel colours") with a dropdown of the six orders, submitted by its
  own form to its own route, `POST /rgborder`. It carries the same session
  token as the credentials form and a tokenless request is never accepted;
  the credential path, the scan list and the validate-before-persist flow
  are untouched. The page says plainly that **the board restarts to apply
  the choice**, and the success page repeats it.
- **The DOWN reset keeps its scope**: the 5-second hold still erases
  exactly `ssid`, `password` and `updir`; the channel order survives it.
- Still unverified without this specific panel on the bench: which of the
  six orders that panel actually needs, and how the colours look after the
  restart on real hardware.

## Panel settings over the LAN: `/config`

Once the board is connected to its network, the **station HTTP server** (the
receiver's own server, hosting `/emote` and `/health`) also serves a
**settings page** at `http://<panel-ip>/config` (also
`gentle-matrix.local/config` over mDNS), **open on the local network without
authentication**, like every other route of that server. It offers exactly
two settings and **neither needs a restart to take effect** (S8):

- **Brightness (S10)**: percent from **10 to 80 in steps of 5**, default
  **30 %**. A runtime value applied through ONE setter; every drawing layer
  (clock, messages, version footer, diagnostic overlay, received PNGs)
  scales through it with `percent * 255 / 100` (30 % = 76, the era of the
  old compile-time constant 64/255). Persisted as a percent byte in
  `gentle`/`brightness` with read-back verification; an absent or out-of-set
  stored value falls back to the 30 % default. `/health` reports the live
  channel scale in `brightness` (0-255) plus the additive live percent in
  `brightnessPercent`.
- **Clock format (S9)**: `24 h` (default, byte-identical to before) or
  `12 h`: the hours go `12, 1..11` unpadded and an **AM/PM** label (3x5
  glyphs at scale 1) is drawn above the seconds block at the seconds' x,
  its baseline (row 28) exactly the scale-4 hour digits' baseline. Nothing
  else moves — date/title band, minutes, colon, seconds, walkers and path
  keep their pixels, and the 500 ms redraw cadence and the autonomous clock
  timing are unchanged. Persisted as a small code (0 = 24 h, 1 = 12 h) in
  `gentle`/`clockformat` with the same read-back verification; an invalid
  code falls back to 24 h.

Contract of the two routes, decided on the exact first request line
(`GET /config HTTP/1.1|1.0`, `POST /config HTTP/1.1|1.0`) **before** the
receiver protocol parser runs, so the receiver's routes and the unknown-path
404 keep their exact semantics:

- `GET /config` → the settings page (200, HTML) with the current values
  preselected.
- `POST /config` → an `application/x-www-form-urlencoded` body (≤ 255 bytes,
  inside the one-client 2.5 s serving limits) with exactly the keys
  `brightness` (decimal percent of the allowed set) and `clock` (`24h` or
  `12h`), each at most once, no unknown keys; the shared portal URL rules
  apply (`+` is a space, `%XX` requires two hex digits, bare control bytes
  rejected). A rejected value or malformed body re-renders the page with the
  escaped reason (400) and changes nothing; an accepted pair is persisted
  through the verified transaction (`panel_settings::storeSettings`: every
  commit and rollback write is decided by read-back, a store that cannot be
  verified — including one whose rollback write itself fails — stays
  unverified and answers 500 without applying) and applied immediately (200
  success page, "no restart needed"). Missing `Content-Length` → 411, empty
  body → 400, oversized body → 413. The `Content-Length` is parsed from the
  complete header block (after `\r\n\r\n`), never from the first request
  line alone.
- The two new keys are NOT in the DOWN-reset scope (the 5-second hold still
  erases exactly `ssid`/`password`/`updir`); the brightness percent and the
  clock format survive it.
- The AP portal page does **not** expose these two settings: the task allowed
  it ("may"), but they are usable as soon as the board has LAN reachability,
  and keeping the hand-written portal server's route set unchanged was the
  lower-risk choice. USB provisioning and the portal remain the credential
  routes.

Host verification: pure parsing/rendering/rules in `include/panel_settings.h`
through `tests/settings_test.cpp` + `tests/test_settings.py`, brightness
value rules in `tests/display_settings_test.cpp`, 12/24 h geometry in
`tests/idle_clock_test.cpp`, and structural wiring pins in
`tests/test_settings.py`. Still unverified on hardware: the visual effect of
the brightness steps and of the 12 h AM/PM placement on the real panel.

## Residual risk: the passphrase crosses the air in the clear

The home-network passphrase crosses a **plaintext HTTP form** (no TLS): the
WPA2 link is protected by the PSK displayed on the panel, so **anyone in
radio range who can read the panel can join the AP and decrypt that
submission**; no firmware change removes this. The CSRF token protects the
form's integrity, not its confidentiality — **no** protection here.

**Operational guidance:** configure the board only where the panel is not
visible to others; otherwise provision over **USB** (`scripts/provision_wifi.py`).

## Recovery

- **Hold DOWN for 5 seconds** to erase `ssid`/`password`/`updir` (verified)
  and restart: the cleared board is back at the boot gate — the portal, or
  USB provisioning when a computer is attached.
- A bad save never bricks the board: the stored credentials are untouched
  on failure and the portal stays open.
- To leave portal mode with the old configuration: power-cycle (or RESET).
  The boot schedule runs again with the stored network; attach a computer
  over USB to keep the portal from opening, or let it open and enter the
  correct credentials there.
- The serial provisioning assistant (`scripts/provision_wifi.py`,
  `WIFI` / `ORIENT` console) still works and supersedes the portal — it
  saves with the same verification and reboots.
- The portal is boot-fallback only: once the board joins any network, it
  behaves exactly as before (receiver on port 80, mDNS, `/health` with the
  additive `"portal": false` field).
