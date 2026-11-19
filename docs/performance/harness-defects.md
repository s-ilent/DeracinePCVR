# Headless harness defects found during CPU-03 validation

Status: **OPEN — recorded, not fixed.** These are defects in the tracked benchmark harness
(`tools/benchmark/headless-gameplay.ps1` and the emulator's scripted-input diagnostics), not in the
CPU-03 image-reuse implementation. They are recorded separately so CPU-03 is not blocked on them and
its branch does not carry an unrelated fix. Nothing here was worked around in tracked code.

Found 2026-11-20 while trying to run an interleaved OFF/ON sweep for CPU-03. The same harness had
produced four valid gameplay cells earlier the same day, so the failures are not a configuration the
harness was never able to run.

## H1 — Intermittent heap corruption in the scripted live-input watcher

Symptom: the emulator process exits ~0.4 s after start with `0xC0000374` (`STATUS_HEAP_CORRUPTION`)
and never creates `user\log\shad_log.txt`. stdout stops immediately after
`scripted_input.cpp:336 Replay: Live script now has 1 steps`.

Trigger: `SHADPS4_LIVE_INPUT` set (the menu-gated re-injection path the runner always enables).
`SHADPS4_INPUT_SCRIPT` alone — one-shot script, same parser, same file content — does **not** crash
and replays normally.

Reproduction in this workspace:

- isolated direct launches with only `SHADPS4_INPUT_SCRIPT` + `SHADPS4_LIVE_INPUT` set: 4 of 4 crashed;
- full runner invocations: 6 of 7 crashed;
- the one runner invocation that survived (`_out\cpu03-sweep\backslash1`) produced a valid 182 s
  gameplay cell, so the fault is **intermittent, not deterministic**.

I reported to the user that forward-slash paths were the cause. **That is not established and is
probably wrong.** The single winning run differed in path form, but it was also a single sample of an
intermittent race, and an isolated test with backslash paths crashed on the same code path. Treat the
path-form difference as an unproven correlation, not a discriminator. The confirmed discriminator is
`SHADPS4_LIVE_INPUT` versus `SHADPS4_INPUT_SCRIPT` alone.

Not investigated further by design (CPU-03 is not a scripted-input change and this is harness code).
Next action for whoever owns it: bisect the live watcher's read/replace against the replayer's
concurrent use of the same script state; the crash is heap corruption, so the likely shape is a
container mutated from the watcher thread while the replay thread holds a reference to it.

## H2 — A dead child is reported as "the run produced no log", with the exit code discarded

Symptom: `headless-gameplay: the run produced no log at <lab>\user\log\shad_log.txt`, runner exit 1,
elapsed 1.7–14.5 s, `stderr.txt` empty.

Cause in the harness: `headless-gameplay.ps1` starts the emulator, waits for it to exit, and then
checks `Test-Path $log` before ever looking at `$p.ExitCode`. A child that dies during startup
produces no log, so every early-exit cause — crash, bad argument path, missing file — is reported as
the same "no log" message. The child's `ExitCode` is captured two lines earlier and then not used in
that message.

Impact: this cost hours. The `0xC0000374` signature in H1 was only visible by launching the emulator
directly; through the runner the crash is indistinguishable from a path typo (which is what I first
assumed it was).

Suggested fix (not applied): include `$p.ExitCode` in the failure message, and treat a non-zero exit
before the log exists as an execution error rather than a "no log" condition.

## H3 — Observed once: access violation during the menu → gameplay transition

Symptom: `0xC0000005`, at ~67 s into a run, using a fixed one-shot script that pressed the menu
recipe between 36 s and 53 s and then stopped. The run had reached the menu (draws ~485 / Mpx ~535)
and died around the load into gameplay.

Recorded as an observation only: it happened once, it is not reproducible on demand, and unlike H1 it
is a deep-run crash rather than a startup one. It is not attributed to CPU-03 — that run had the reuse
pool **disabled** (`SHADPS4_DBG_IMAGE_REUSE` unset) — but it is also not explained. If it recurs while
a pinned driver/binary is in use, it deserves its own investigation before any conclusion is drawn
from a cell that happened to survive it.

## What this means for CPU-03's numbers

The four cells CPU-03 relies on all reached gameplay, exited 0 and passed the tracked gate, so they
are valid cells on the harness's own terms. What the defects prevented is the *interleaved* sweep
(OFF → ON × 3 at 300 %, 200 % and 100 %) that would have settled the frame-rate magnitude. That gap
is stated as a limitation in `image-reuse-pool-validation.md`; it is not papered over.
