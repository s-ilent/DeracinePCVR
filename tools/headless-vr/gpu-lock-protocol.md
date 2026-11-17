# GPU benchmark lock (`gpu-lock.sh`)

One machine, one GPU-bound emulator session at a time. This is the same slot the HMD protocol
(`testslot.sh`, `HMD-SLOT-PROTOCOL.md`) uses — `_build/team/hmd-slot/lock` — so a headless
harness user and an old testslot.sh user exclude each other. `testslot.sh` is not modified.

The lock is **exclusion, not authorization**: holding it does not grant permission to launch
the game; the task owner still decides that. There is **no headset prerequisite** — a headless
session (`SHADPS4_VR=1 SHADPS4_OPENXR=0`) needs the GPU and nothing else.

## Headless-only directive (2026-11-18 01:07)

The user ended headset use for routine work; `_build/team/HEADLESS-ONLY.md` is the directive and
`_build/team/hmd-slot/HEADLESS-ONLY` is the marker. While the marker exists, `--kind hmd` (the
default) is refused with exit **8** and a `NOTE ... refused: HEADLESS-ONLY marker is present` line
in the slot log; `--kind headless` is required, and `testslot.sh` applies the same rule to its
reason text. A physical-headset cell is an occasional final integration check only, and only when
the user explicitly asks for it.

## Commands

Run through WSL (paths are Linux paths there; the slot lives on `/mnt/m`):

```bash
L=/mnt/m/Development/Deracine-work/_build/team/gpu-lock.sh

bash $L status
bash $L acquire --owner harness-eng --kind headless --reason "menu cell" --ttl 1800 \
                --require-quiet --quiet-wait 900
bash $L wait    --owner harness-eng --kind headless --reason "menu cell" --timeout 1800 \
                --require-quiet --quiet-wait 900
bash $L run     --owner harness-eng --kind headless --reason "smoke" --ttl 1800 \
                --require-quiet -- pwsh -File M:/Development/Deracine-work/_build/headless/headless-vr-gameplay.ps1 -Exe ...
bash $L heartbeat --token <token>
bash $L release   --token <token>
bash $L reclaim   [--as NAME] [--reason TEXT] [--force]
bash $L log 30
```

`acquire`/`wait` print the token on stdout and diagnostics on stderr:

```bash
TOKEN=$(bash $L acquire --owner me --reason "cell" --require-quiet) || exit $?
```

`run` waits for the slot, heartbeats every 15 s in the background, exports
`GPU_LOCK_TOKEN`/`GPU_LOCK_OWNER` to the child, runs the command, releases on normal exit,
non-zero exit, Ctrl-C and SIGTERM. Prefer it for anything longer than a minute.

## Quiet-machine audit

`--require-quiet` audits the process list before the hold is taken (and again after) for
`shadps4`, `ninja`, `clang-cl`, `link`, `msbuild`, `renderdoccmd`, `qrenderdoc`. If any is
running, the lock is **not taken**: it refuses with exit 7 and writes
`QUIET who=<owner> phase=acquire busy=<list>` to the slot log. With `--quiet-wait SECONDS` it
retries every 10 s until the deadline. The audit never stops a process it did not start.

The harness also records its own before/after audit (same process names plus the other
emulator paths it can see) in `meta.json`, because the lock only proves the moment of
acquisition, not the whole session.

## Files and compatibility

```text
_build/team/hmd-slot/
  lock/          present == HELD; meta (token, owner, reason, ttl, kind, pid, host, acquired) + hb
  queue/         FIFO waiter entries
  archive/       broken locks, kept for audit
  log            append-only; ACQUIRE/RELEASE/WAIT_*/BREAK_*/QUIET lines
  USER-HOLD      human reservation (blocks everyone)
  STALE-POLICY   any (default) | lead-only
```

`meta` keeps the testslot.sh keys and adds `kind=hmd|headless`; log lines keep the old
`ACQUIRE owner= token= ttl= reason=` shape with `kind=` appended, so the old protocol document
and any log reader stay true.

TTL: `--ttl` (default 600 s). The holder must heartbeat at least every TTL; `run` does it
automatically, the harness does it inline every ~15 s. A dead holder goes stale and any agent
may `reclaim` it after the TTL (`STALE-POLICY=lead-only` restricts this to a Lead with
`HMD_SLOT_LEAD=1`).

Exit codes: `0` ok · `3` busy · `4` stale but not breakable · `5` user hold · `6` wait timeout ·
`7` quiet check failed · `8` headless-only (a `--kind hmd` hold while the marker exists).

## Limitations

- Atomicity comes from `mkdir` on the filesystem backing `M:`; a half-written lock goes stale
  after 30 s.
- The lock is a *claim*, not a sandbox: the user launching shadps4 by hand, or a process that
  starts after the audit, is outside it.
- `heartbeat` proves a wrapper is alive, not that the emulator is healthy.
- Killing the `run` wrapper with SIGKILL falls back to the TTL; Ctrl-C releases at once.
