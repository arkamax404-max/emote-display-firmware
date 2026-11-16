# Integrate open firmware branches
Objective: integrate existing branches into main and publish without losing local work.
Scope: Git integration, README conflict resolution, existing tests and target build; no hardware writes or branch deletion. New authored changes forecast under 100 lines; preserve existing commits.

## Specs
- S1: "integra las ramas que tengamos abiertas y deja todo en main"
- S2: "deje el borrado de ramas para después de que lo verifiques"

## Tasks
- T1 | S1 | parent Git inventory/preservation | done | commit: none (read-only)
- T2 | S1 | parent merge + README resolution | done | commit: 84c4717
- T3 | S1 | parent verification: target build + available suite | done | commit: 84c4717
- T4 | S1 | parent push branch, merge into main, publish, switch | pending | commit: pending
- T5 | S2 | branch deletion (remote + local) | blocked: awaits user verification of main | commit: pending

## Log
- L1: User request verbatim: "integra las ramas que tengamos abiertas y deja todo en main"
- L2: Fetch succeeded. main aec717e; local clock b0b2554; remote clock 776843b; integration 4de1798; counters 3a54cfe. merge-tree predicts README-only conflict, automatic src/main.cpp integration. Preserve pre-existing AM odd/tasks/autonomous-clock-upload.md and untracked .pi/, d.las, lemmings_reference.png, odd/tasks/clock-vertical-offset.md; do not publish them. Do not delete branches or force push. No meaningful RED for integrating existing tested behavior: run merged suite and target build.
- L3: User request verbatim: "revisa las ramas que tenemos abiertas para ver si las temenos ya integradas y se puede eliminar"
- L4: Inventory result: no branch was integrated into main; main is a strict ancestor of all three feature branches. origin/feat/sender-ip-counters (1 commit) is contained in origin/feat/clock-sender-integration. The two feature lines diverge: 776843b (clock vertical offset) only on the clock branch; a7e4a56 plus six firmware/documentation commits only on the integration branch. Nothing was deletable before the merge.
- L5: User authorization verbatim: "si" (continue the integration; deletion deferred).
- L6: README.md conflict resolved as a union: kept the local acceptance state and added the verified v1.0.0 recording state, keeping the physical-validation caveat and the warning not to re-run the generic upload. The "Continuar el reloj desde el Mac" section now links the v1.0.0 record, the continuation guide and the previous ROM preparation. 22 authored lines net.
- L7: Merge commit 84c4717. Containment re-checked: origin/main, origin/feat/sender-ip-counters, origin/feat/clock-sender-integration and origin/feat/autonomous-idle-clock are all ancestors of HEAD. The src/main.cpp auto-merge was reviewed seam by seam: the local side keeps esp_timer tick, full-canvas clock drawing and frameIsClock classification, while the incoming side adds firmware::drawVersion, SenderStats/SenderConnection and the sender IP capture.
- L8: Verification: target build `pio run` exit 0 (RAM 47.2%, 154688/327680 bytes; flash 41.7%, 874445/2097152 bytes). The native suite is unavailable on this host: 16 of 17 test cases fail with FileNotFoundError because clang++ is absent (only ESP32 cross-toolchains are installed); no test failed on an assertion. The single compiler-independent test, test_version_is_only_integrated_in_diagnostic_branch, passed and covers the src/main.cpp ordering seam that the merge could have broken.
