# Wi-Fi captive portal fallback

What the firmware does when it cannot join its saved Wi-Fi: after an exact,
bounded boot-time retry schedule it opens a **WPA2 SoftAP** (`gentle-matrix`)
with a captive portal (DNS wildcard responder plus a configuration page on
port 80) so the network can be reconfigured from a phone. Implemented in
`include/portal_policy.h` (host-testable decisions), `include/portal_page.h`
(host-testable HTML renderer) and the portal block of `src/main.cpp`.
Covered by `tests/test_portal.py` (26 tests). Task
`odd/tasks/wifi-captive-portal-fallback.md`, T1-T5; T6 (UF2 + device write)
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
  1, at most 4 clients, address `192.168.4.1`.
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
4. **Failure**: the station disconnects, the board returns to AP-only, the
   stored credentials are left untouched, the offered buffers are zeroed
   and the page re-renders with an error. The portal stays open for another
   try.

The offered home-network passphrase keeps the stronger secret handling: it
is never logged, never echoed into the HTML, never shown on the panel, and
the buffers holding it are zeroed as soon as the validation attempt
concludes (both outcomes). Nothing about it is exposed in `/health`.

## The page and the routes

- `GET` **any** path returns the form page (200, HTML), so operating-system
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
  work unchanged; `pollProvisioning` remains the only reader of `Serial`
  and no new serial console command was added.
- During validation the panel shows `TRY WIFI / CHECK`; on successful save
  `WIFI / SAVING` until the restart.

## Recovery

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
