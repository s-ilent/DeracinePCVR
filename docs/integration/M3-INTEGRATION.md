# M3 harness integration (2026-11-18)

M3 tooling from the headless agent is integrated beside the unified M2 benchmark; M2, scripted-input,
R6 and process-ownership work are unchanged and authoritative.

## Source commits

| M3 branch | In this branch |
|---|---|
| `8ff9827` | cherry-picked: gameplay runner, inputs, A–E validation, handoff |
| `b9261bc` | cherry-picked: exit-after wait |
| `b0ada73` | cherry-picked: 100 % profile, portable launch parameters |
| `f81153a` | cherry-picked: report, save manifest (`MANIFEST-M3.json`), handoff |
| `bc524dd` (R6) | `9b918eb`, already published in `6cab0fe` |

## Safety hardening applied

The imported runner's three `Get-Process -Name shadps4` sweeps now filter by the runner's own lab
executable path. Before writing `engine.ini` it requires a private shadow with
`benchmark-shadow.json` (`private: true`, matching eboot hash) and refuses missing portable paths.
The integrated benchmark's `process_ownership.ps1`/`game_shadow.py` remain the policy of record.

## Validation status

| item | result |
|---|---|
| R6 clean shutdown (unified build) | **PASS** — 4/4 guard tests; live exit 0, teardown markers, no assertion, no orphan |
| 300 % unified gameplay cell | **VALID** — second lab, 324/324 windows in band, 34.14 unique FPS, draws 1716–1943, Mpx 873.8, passes 56.5, save verified (`_build/unified/accept-lab2-sfo`) |
| 300 % unified band | draws 1500–2300, Mpx 500–1100, passes 25–80 |
| 100 % candidate reference | 960×1080/eye, 1866 draws, 69.3 Mpx, 40.5 passes, 46.2 fps; band Mpx 55–90, passes 30–55 |
| 100 % unified cell | **BLOCKED / not claimed** — the imported runner hung before producing a result; its orphan lab process was stopped by path (verified) and the GPU lock released |

All headless switches remain off by default; the recycler stays excluded/default-off; depth/barrier
remain opt-in; no LUT work; no generated evidence, saves or captures are committed; no deployment.
