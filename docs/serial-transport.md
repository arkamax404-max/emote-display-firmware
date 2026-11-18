# USB serial display ingress — wire protocol v1

Firmware-side binary serial transport for display updates (task
`odd/tasks/usb-serial-ingress.md`, T1+T2). It carries the same operations and
metadata as the HTTP path — image, claim, heartbeat, release, session id,
project name, clock state — over USB CDC, with the same validation and the
same no-state-change guarantee on rejection. Implemented in
`include/serial_protocol.h` (frames), `include/serial_ingress.h`
(multiplexing) and `include/image_ingress.h` (admission/commit rules shared
with the HTTP path in `src/main.cpp`); the sender is a separate repository.

## Quick path

1. Sender opens the port (console/provision mode is the default) and sends
   the exact line `EMOTE\n`; the firmware answers `EMOTE_READY\n` and the
   binary session begins.
2. Sender builds one frame per operation (layout below) and writes it to the
   serial port, keeping exactly one frame in flight (stop-and-wait).
3. Firmware validates the frame, applies the ownership rules and presents
   accepted images, then answers with a 12-byte `EMTR` reply carrying the
   sender's request id; the sender matches replies by that id and the `EMTR`
   prefix (console log lines may interleave between replies, so the sender
   must scan, never read line-wise).
4. Sender sends the exact sequence `EMTE\n` to leave the binary session; the
   firmware answers `EMOTE_EXIT\n` and the console line protocol resumes.

## Frame layout (little-endian throughout)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `EMTS` (0x45 0x4D 0x54 0x53) |
| 4 | 1 | version, must be 1 |
| 5 | 1 | op: 0 image, 1 claim, 2 heartbeat, 3 release |
| 6 | 1 | flags: bit0 hasSession, bit1 hasName, bit2 clockState; bits 3–7 reserved 0 |
| 7 | 1 | reserved, must be 0 |
| 8 | 2 | requestId (uint16, sender-chosen correlation id, echoed in the reply) |
| 10 | 2 | sessionIdLen |
| 12 | 2 | nameLen (encoded name bytes) |
| 14 | 4 | payloadLen (uint32) |
| 18 | … | sessionId bytes (present iff bit0), name bytes (iff bit1), payload bytes (image only) |
| last 4 | 4 | CRC-32 of the whole frame, little-endian |

CRC-32 is the reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final xor
0xFFFFFFFF — identical to `zlib.crc32` and to `receiver::crc32`. Coverage is
exactly the bytes from the first magic byte through the last payload byte; the
four CRC bytes themselves are excluded from their own checksum.

### Length rules (checked at the header, before anything is buffered)

- sessionIdLen: 1..128 iff hasSession, otherwise exactly 0.
- nameLen: 1..1152 iff hasName, otherwise exactly 0.
- payloadLen: image 1..32768 (`receiver::MAX_PNG`); claim/heartbeat/release 0.
- payloadLen above 32768 or above the receiver's payload capacity is rejected
  immediately (`oversize`) without waiting for bytes that would never fit.
- claim/heartbeat/release require the hasSession flag.

A violation answers `length`, `oversize`, `flags`, `reserved`, `version`,
`op` or `session` (control without identity) and the frame is discarded. The
reply is correlated with the frame's own request id: the id is latched as
soon as the 14 fixed header bytes are present, before any header validation,
and cleared when the next frame starts — a rejected frame is never answered
with a previous frame's id.

### Semantic rules (checked only after the CRC verifies)

- Session id bytes match `[A-Za-z0-9._:-]{1,128}`
  (`session::validSessionId`, the exact HTTP rule) → otherwise `session`.
- Name bytes are percent-encoded exactly like the HTTP
  `X-Emote-Project-Name` header and decoded with the same one-pass matrix
  (`project_name::decode`); any violation → `name`. Names are only meaningful
  on images: like the desktop lifecycle endpoints, a name field on a control
  is carried and CRC-checked but never decoded or stored.
- Image payload must pass the same `receiver::validPngEnvelope` check as HTTP
  (64×64 RGBA8, non-interlaced, IHDR/IDAT/IEND) → otherwise `png`. Full pixel
  decoding stays with the presentation stage, as on HTTP.
- clockState is metadata parity with `X-Emote-State: clock`; it is never
  rejected.

## Reply frame (12 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `EMTR` |
| 4 | 1 | version, always 1 |
| 5 | 1 | result code |
| 6 | 2 | requestId echoed, little-endian |
| 8 | 4 | CRC-32 over bytes 0–7, little-endian |

Result codes: 0 ok, 1 crc, 2 version, 3 op, 4 flags, 5 reserved, 6 length,
7 oversize, 8 session, 9 name, 10 png, 11 busy (reserved for the T2 ownership
conflict, HTTP 409 parity). Only the request id is echoed — never a session id
or name — so wire logs stay free of identity data.

## Flow, backpressure and resync

| Topic | Contract |
|-------|----------|
| Session | Frames are only processed inside an explicit binary session. Provision mode is the default and owns every byte, so console lines — including credentials that start with `EMTS` or equal `EMOTE` — behave exactly as before. `EMOTE\n` at the top level enters the session (`EMOTE_READY`); `EMTE\n` leaves it (`EMOTE_EXIT`). |
| Exit safety | `EMTE\n` and a repeated `EMOTE\n` (a lost `EMOTE_READY`) are recognized only while the parser is idle — never inside a frame, so a PNG payload can never trigger them. The recognition window resets at every frame boundary and on timeout. Bounded scope: a plain truncated frame or its magic prefix stays binary skipped bytes and can never reach provisioning, and only the **complete** sequences trigger. The firmware cannot promise protection from an arbitrary corrupted tail that happens to contain the exact `EMTE\n` while idle — that tail is by definition a valid exit command, and it exits. |
| Stop-and-wait | The sender keeps exactly one frame in flight and waits for the `EMTR` reply (or a resend timeout) before the next frame. There is no frame queue on either side. |
| Retry (at-least-once) | There is no deduplication cache: `requestId` is correlation only, never exactly-once delivery. On a missing or NACK reply the sender resends the whole frame; a resent image is simply a new accepted image — it increments the frame counter, refreshes the lease and re-applies the title time. An ACK means the frame was actually applied (admitted and presented), so a sender that missed the ACK must be prepared for that re-application; controls are not retried automatically by the firmware, and a resent explicit claim deliberately re-claims (it can take ownership again, which is the claim's meaning). Release stays idempotent without an owner; heartbeat/release from a foreign session answer `busy` again. Reconnect and retry policy is sender responsibility. |
| Backpressure | If USB CDC drops bytes under load the CRC fails → `crc` NACK → stop-and-wait resend. The sender must never stream ahead of replies. |
| Timeouts | Two bounded windows apply inside the binary session: **1 s inactivity** (a live sender never stalls mid-frame that long; USB CDC gaps are not baud-bounded) and **10 s total per frame** (a 32 KiB frame needs ≈2.9 s at 115200 8N1; 3× margin). Expiry discards the open frame, resyncs the parser and clears the entry/exit scan window (stale bytes can never complete a sequence); the session stays binary so a dead frame's tail is skipped, never returned to the console; no reply is sent for a timed-out frame. A plain truncated magic never escapes to the console. The HTTP request timeout (2.5 s) is a distinct policy and is unchanged. |
| Resync | Outside a binary session, `EMTS` bytes are plain console data and the parser is never fed — provisioning routing cannot be hijacked by frame bytes or tails. Inside the session, bytes before a magic and the tail of a rejected frame are counted as skipped and never interpreted. |
| Truncation | A frame that simply stops mid-stream never completes; the parser stays `active` until the missing bytes arrive or a timeout discards it. |
| Buffering | The parser is allocation-free and takes an externally supplied payload buffer — a dedicated static serial buffer on the device; the shared HTTP PNG buffer is never used for serial traffic. Bytes in that buffer are untrusted until a frame reports complete. |
| Reply TX | The firmware writes the 12-byte reply in a single bounded, non-blocking write gated by `availableForWrite()`; at most one reply is pending (stop-and-wait), so a host that stopped reading can never stall the cooperative loop. |
| Output | The parsed `Frame` is stable only between a complete result and the next `feed()`/`reset()`. After any rejection the exposed frame is scrubbed and previously accepted display state is untouched. `Parser::feed(data, length)` returns the number of bytes consumed and stops right after a terminal event — a suffix after that point stays unconsumed for the next call and is never dropped. |
| Admission | The serial path reuses the exact HTTP admission and commit rules (`include/image_ingress.h`): ownership check before presentation, ACK only after a successful presentation, lease renewal and title commit only then. USB frames never create sender IP stat entries. |

## Diagnostic indicator (T7)

A 4 px right triangle is overlaid in the top-right of the **rotated logical
canvas** — rows of 4/3/2/1 pixels all flush at logical x63 (60..63 at y0,
61..63 at y1, 62..63 at y2, 63 at y3) — drawn with ordinary logical
`drawPixel` after `setRotation`, like the avatar/text content, with **no
additional inverse rotation**. It is drawn after every content path (avatar,
clock, message, info screen) and before each show, so a newly presented
frame can never erase it. Exactly ten pixels are written per draw.

| Signal | Meaning |
|--------|---------|
| Amber | Serial bytes are pending: the first received byte armed the window and no binary operation has completed yet. Any traffic class counts — console bytes and partial magic included — but **port opening or boot never marks anything**. |
| Green | A binary operation was actually accepted: an image decoded and presented, or a claim/heartbeat/release admitted. A green from the claim a sender sends on port open does **not** prove a frame arrived — the `/health` counters distinguish image from control acceptances. |
| Red | A binary parse/decode/admission rejection, or a reception timeout. Unframed console traffic (WIFI/ORIENT, garbage, partial magic) that never completes a binary operation also turns red after the inactivity window — documented, intended. |
| (none) | No serial traffic since boot; a held terminal result keeps showing with no new bytes. |

Timing rules (exact):

- The pending window is armed by the **first received byte**, never by the
  port opening or boot, so silence is never interpreted as a rejection.
- Timeout windows mirror the transport contract: **1 s inactivity**, **10 s
  total from the first RX byte**. Nothing is armed after a completed frame,
  so idle silence after a terminal result never rejects.
- A terminal green/red is held for **at least 750 ms** before amber may
  replace it; a new terminal outcome replaces another terminal immediately.
  If traffic occurred during the hold and is still pending when it ends,
  amber takes over at that moment, even without a new byte at the boundary.
- The `EMOTE` handshake is a protocol boundary, never an accepted operation:
  the signal stays pending (amber) with no timeout armed until a binary
  operation; `EMTE` clears pending amber.

`GET /health` gains three additive, saturating (never-wrap) `uint32` fields:
`serialRxImages` (incremented only after actual successful presentation of
an image frame), `serialRxControls` (claim/heartbeat/release) and
`serialRxRejected` (parse/decode/admission rejections and timeouts). All
previous fields are unchanged. The rejection counter is diagnostic-only: a
rejection still changes no frame, title, lease or ownership state, and the
transport reset policy remains owned by the serial multiplexer exactly as
before.

## Details

| Topic | Decision |
|-------|----------|
| Binary, not line-based | The console stays line-based; frames run only inside the explicit binary session entered with `EMOTE` and left with `EMTE`, so the two never mix. |
| No base64, no heap | The payload is raw PNG bytes into a caller-supplied static buffer; there is no frame queue because of stop-and-wait. |
| Metadata parity | Session identity, project-name matrix, clock state and PNG envelope checks reuse the HTTP parser's exact code paths. |
| Throughput | Native USB CDC throughput is a hardware measurement (T3), not a promised rate. |

## Checklist

- [ ] Sender can build all four operations with the layout table alone.
- [ ] Sender enters with `EMOTE`/`EMOTE_READY` and leaves with `EMTE`/`EMOTE_EXIT`.
- [ ] Receiver answers every frame with exactly one correlated reply.
- [ ] A corrupted, truncated or hostile frame changes no display state.
- [ ] The diagnostic triangle shows amber while bytes are pending, green
      only after an accepted binary operation, red on rejection or timeout,
      held ≥750 ms before amber may replace it.
- [ ] `/health` exposes `serialRxImages`, `serialRxControls`,
      `serialRxRejected` without changing any previous field.
- [ ] Wire logs contain no session ids or names.

## Next step

T3 runs functional checks on the device, assesses shared-contract risk and
records the sender handoff plus the hardware throughput measurement. The
device build already compiles the integrated loop (`python -m platformio run`);
host suites cover the parser, the multiplexer and the admission rules.
