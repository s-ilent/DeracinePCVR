# RHI-03: single-execution capture runbook

## Status

**Prepared, not executed.** This requires explicit authorisation to launch the emulator. No part of
it has been run.

It exists to resolve the remaining RHI-03 proof obligations: that one bounded Déraciné execution
produces a semantic tape, a resource package and an index bridge that describe the same draw, and
that the unified renderer can replay it with its own captured shaders.

## Why one execution

Earlier designs used two runs, one for resources and one for semantics. That cannot establish that
the two describe the same draw: provenance ids are per-process, so a tape from run A and a package
from run B carry ids that mean nothing across runs.

The capture session identity exists to make this checkable. One run produces one session id, and
every artifact carries it. A join across sessions is refused.

## Prerequisites

| Item | Value |
| --- | --- |
| Emulator build | `build/shadps4.exe` from this branch |
| Approved game image | the 1.00 SELF, identified by hash |
| Game SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Title id | `CUSA11919` |

Verify before running:

```powershell
(Get-FileHash '<path to the approved eboot.bin>' -Algorithm SHA256).Hash
# must be 8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0
```

**Do not use the `CUSA11919-qual` image.** It hashes to `8da8e46a...`, which is a different build.
Static addresses and runtime observations from different builds must not be joined.

## The run

```powershell
# Set these to your own paths. The lab must already contain the save slot and
# sce_sys/param.sfo the title needs, as described in the headless harness notes.
$lab = '<lab directory containing shadPS4.exe and user/>'
$out = '<output directory for the capture>'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$env:SHADPS4_VR = '1'
$env:SHADPS4_OPENXR = '0'
$env:SHADPS4_VR_DEMO = '0'
$env:SHADPS4_TITLE_EYE_WIDTH = '2880'
$env:SHADPS4_TITLE_RESOLUTION = '6'
$env:SHADPS4_VR_SCALE = '0.5'
$env:SHADPS4_VR_DESKTOP_VIEW = 'stereo'
$env:SHADPS4_VR_REFRESH_RATE = '120'
$env:SHADPS4_XR_PAUSE = '0'
$env:SHADPS4_DBG_AUTOPRESS = '1'

# Provenance and the identity bridge. The bridge is what connects the original UE4
# index-buffer object to the guest allocation, and without it the join cannot be made.
$env:SHADPS4_DERACINE_RHI_PROBE = '1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER = '33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE = '1'
$env:SHADPS4_DERACINE_INDEX_IDENTITY = '1'

# Geometry, shader parameters and textures.
$env:SHADPS4_DERACINE_VERTEX_INPUT = '1'
$env:SHADPS4_DERACINE_SHADER_PARAMS = '1'
$env:SHADPS4_DERACINE_TEXTURE_CAPTURE = '1'
$env:SHADPS4_DERACINE_CMD_CAPTURE = '1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER = '33'

# The effective shader binaries and their descriptor interfaces.
$env:SHADPS4_DERACINE_SHADER_BINARY = '1'
$env:SHADPS4_DERACINE_SHADER_INTERFACE = '1'

# The reference image around the draw.
$env:SHADPS4_DERACINE_REFERENCE_OUTPUT = '1'

# The package and the tape, into the same directory.
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out
$env:SHADPS4_DERACINE_SEMANTIC_TAPE = '1'
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR = $out

$env:SHADPS4_EXIT_AFTER = '75'
$env:SHADPS4_CLEAN_EXIT = '1'

Start-Process -FilePath "$lab\shadPS4.exe" -WorkingDirectory $lab `
    -ArgumentList @('-g', '<path to the approved eboot.bin>') -Wait
```

### Do not pin the draw

`SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` must be left unset. RHI-02I-R3 made the package-selected
provenance authoritative, so the tape follows the package. Pinning it would be a selection
mechanism, not evidence, and a conflict is logged as `DERACINE_SEMANTIC_TAPE_MANUAL_CONFLICT`.

### Do not enable the synthetic bypass

`--synthetic-fixture` must not be passed when verifying a genuine capture. It exists for fixtures
built from a package, and using it here would conceal exactly the failure this run exists to detect.

## Expected artifacts

| File | Content |
| --- | --- |
| `package.json` | schema 4 package |
| `resources.bin` | geometry and shader-buffer bytes |
| `textures.bin` | captured texture bytes |
| `tape.json` | schema 2 semantic tape with session identity |
| `index_bridge.json` | the original-object-to-guest-allocation bridge |
| `manifest.json` | schema 2 selection and verification record |
| `stage_*.spv` | effective shader modules |
| `reference_post.bin` | the post-draw reference image |

## Validation

```powershell
# The package must validate.
python tools\validate_draw_package.py $out

# The adversarial suite must reject every case. It mutates a copy, so the original is unchanged.
python tools\test_validator_adversarial.py $out
```

## The join

```powershell
.\build-geometry-replay\deracine_tape_replay.exe $out --shader-mode effective
```

**Do not pass `--synthetic-fixture`.**

### Reading the outcome

| Output | Meaning | Next |
| --- | --- | --- |
| `provenance: original_tape_matched` | the join succeeded on a real identity field | proceed |
| `provenance: insufficient_evidence` with `index_domain ... DISAGREE` | the two sides record different namespaces | the expected outcome if the bridge is absent |
| `provenance: rejected` with `index_guest_base ... DISAGREE` | the two sides name different resources | investigate; the exporters may have selected different draws |
| `identity rejected: ... different draws` | provenance ids disagree | the tape and package are from different runs |
| `effective shader resolution failed` | the package cannot supply its own shaders | read the named problem |
| `draw is out of bounds` | the captured draw exceeds its buffers | the capture is inconsistent |
| `reference_status: exact_match` | the render matched the captured reference | the strongest available result |

## What a successful run would prove

That one original UE4 indexed draw's semantic payload, its resource package and its index bridge
describe the same operation, and that the unified renderer executed that draw with the game's own
effective shader modules, its captured descriptors, its captured push constants and its captured
pipeline state, producing an image that matches the captured reference.

## What it would not prove

Whole-frame execution, original material reproduction beyond the captured state, OpenXR integration,
or that the renderer is correct for draws other than the one captured.

## Evidence to preserve

Keep these privately, outside the repository. Do not commit game bytes.

| Artifact | Why |
| --- | --- |
| `package.json`, `tape.json`, `index_bridge.json`, `manifest.json` | the join |
| `shad_log.txt` | the exporter's own record of what it selected |
| replay stdout | the join outcome and the executed draw |
| `reference_post.bin` | the comparison target |

Record the SHA-256 of each. `resources.bin`, `textures.bin` and the `.spv` sidecars contain game data
and must stay private.

## Cleanup

```powershell
Get-Process shadPS4 -ErrorAction SilentlyContinue | Stop-Process -Force
foreach ($name in 'SHADPS4_DERACINE_SEMANTIC_TAPE', 'SHADPS4_DERACINE_SEMANTIC_TAPE_DIR',
                 'SHADPS4_DERACINE_DRAW_PACKAGE', 'SHADPS4_DERACINE_INDEX_IDENTITY',
                 'SHADPS4_DERACINE_SHADER_BINARY', 'SHADPS4_DERACINE_SHADER_INTERFACE',
                 'SHADPS4_DERACINE_REFERENCE_OUTPUT', 'DERACINE_REPLAY_VALIDATION') {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}
```

All capture instrumentation is default OFF. Nothing in this runbook changes that.
