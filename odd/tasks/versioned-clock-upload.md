# Versioned clock firmware upload
Add an identifiable waiting screen and prepare a safe clock firmware reload.
Preserve existing configuration, recovery artifacts, and unrelated edits. Forecast: ~140 authored lines; delivery: ask-on-risk.

## Specs
S1. User request: "pensaba que si, pero lo cargamos de nuevo, pon debajo del send png un numero de version"

## Tasks
T1 | S1 | context-backstop worker: add version below SEND PNG, tests and docs | done | commit: this work unit
T2 | S1 | worker: run suite, build and identify new artifact | done | commit: this work unit
T3 | S1 | parent-only upload after validated recovery, live partition/OTA proof and exact write approval; independent procedure verification | blocked: needs bounded live-preflight authorization and subsequent exact write approval | commit: n/a

## Log
L1. User original request: "pensaba que si, pero lo cargamos de nuevo, pon debajo del send png un numero de version"
L2. Selected initial visible release v1.0.0 (no existing firmware version convention found). Version is shown only on READY / SEND PNG; preserve clock precedence and other diagnostics.
L3. Existing odd/tasks/macos-firmware-upgrade-preparation.md has unrelated uncommitted edits: preserve. Recovery-local contains three logs but no full-flash backup. Previous frozen binary is preserved. No serial/reset/flash performed in this session. Historical authorization is not substituted for current exact write approval.
L4. Worker observed RED (2 failures: missing helper/integration), then GREEN (2 focused tests). Applicable suite: 23 discovered, 22 passed, 1 explicit skip of external sibling-checkout integration without accessing it. PlatformIO build exit 0, vendored Protomatter pointer-comparison warning. Artifact .pio/build/matrixportal_s3/firmware.bin: 874032 bytes, SHA256 36dab0d2af4f731ca7b86024f06c01f7caa5dee892283c469851035e0ac1d8ea. Hardware/visibility/NTP not verified.
L5. Parent structural readback: no shared rendering refactor, only READY footer helper. Native assessment initially unassessable due to untracked scope; no independent verifier required by returned plan. T3 cannot execute: backup not validated, live partition/OTA identity unproven, concrete write/recovery plan not independently verified or approved. Risk for code label: none; future upload: item 5 (delivery/environment) and item 1 (preservation of stored flash).
