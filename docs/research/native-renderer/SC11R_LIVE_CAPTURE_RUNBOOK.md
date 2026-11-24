# SC-11R live capture runbook

**Status: prepared, not executed.** No emulator run has been performed. This document is the exact
procedure for a later bounded capture, and the checks that decide whether a candidate draw is
suitable.

## 1. Permission boundary

Launching Déraciné or shadPS4 requires explicit user permission. Nothing in this runbook is
authorised to run on its own.

## 2. Prerequisites

- A game dump at `D:\Temp\Deracine\CUSA11919-qual` (or the lab's configured shadow).
- A private lab directory, so the run does not touch shared settings.
- The SC-11R build: `shadps4.exe` from `wt-sc11r`.

## 3. Capture flags

All are opt-in and default OFF. Every one is required for a complete original-shader package.

```powershell
# Provenance: the draw identity the package is keyed by.
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'

# Geometry and shader parameters.
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'

# The package itself.
$env:SHADPS4_DERACINE_DRAW_PACKAGE='<output dir>'

# SC-11: the effective shader binaries.
$env:SHADPS4_DERACINE_SHADER_BINARY='1'

# SC-11R: the descriptor interface of each permutation.
$env:SHADPS4_DERACINE_SHADER_INTERFACE='1'

# SC-11R: the pre/post reference oracle.
$env:SHADPS4_DERACINE_REFERENCE_OUTPUT='1'
# A 2880x3240 RGBA8 target is 35.6 MiB, which exceeds the 64 MiB default only if doubled.
# Raise it only if the selected draw's attachment is larger.
# $env:SHADPS4_DERACINE_REFERENCE_OUTPUT_MAX_BYTES='134217728'

# Bounded run.
$env:SHADPS4_EXIT_AFTER='75'
$env:SHADPS4_CLEAN_EXIT='1'
```

## 4. Expected log lines

| Line | Meaning |
| --- | --- |
| `DERACINE_SHADER_BINARY_SUMMARY` | how many permutations were retained |
| `DERACINE_SHADER_INTERFACE_SUMMARY` | how many interfaces were retained |
| `DERACINE_REFERENCE_OUTPUT_SUMMARY` | references captured, pending, failed |
| `DERACINE_DRAW_PACKAGE_REQUESTED` | the package directory |
| `DERACINE_DRAW_PACKAGE_CANDIDATE` | which draws were rejected and why |

A `DERACINE_REFERENCE_OUTPUT` line with `post_status=pending` means the readback did not complete
before export. That is a failure, not a partial success: the package must not claim
`reference_output_ready`.

## 5. Candidate acceptance checks

Run these against the produced package before treating it as a valid original-shader capture.

```powershell
python tools/validate_draw_package.py <output dir>
python tools/test_validator_adversarial.py <output dir>
.\build-geometry-replay-rel\deracine_original_shader_replay.exe <output dir>
```

The package is suitable only when the validator reports:

- `shader_binaries_ready: true` with two stages
- `shader_bindings_ready: true` with an interface per stage
- `shader_parameters_ready: true`
- `pipeline_state_ready_original: true`
- `reference_output_ready: true` with a non-empty post hash
- `original_shader_replay_ready: true`

## 6. Rejection criteria

Reject the candidate and select another when:

| Condition | Why |
| --- | --- |
| `reference_output` reports `oversize` | the attachment exceeds the configured bound |
| `reference_output` reports `unsupported_format` | the attachment format is not one the capture reads |
| `reference_output` reports `multisampled` | the capture does not resolve MSAA |
| the pre-draw reference is missing because the pass clears | the draw inherits the clear value, not a capturable image |
| a stage has no retained interface | the permutation was not compiled in this run |
| the draw uses tessellation or geometry stages | out of scope for this phase |
| the draw writes a UAV | out of scope for this phase |

## 7. Selecting an alternative draw

The SC-10S draw (provenance 40) is the current best candidate: 1101 indices, 4 vertex streams, 6
shader buffers, one 64x64 BC3 texture, vertex and fragment stages only.

If it is rejected, prefer a draw with fewer resources and a single colour attachment. The
`DERACINE_DRAW_PACKAGE_CANDIDATE` lines name the binding that failed, so the reason is actionable.

## 8. What the capture does not establish

A successful capture establishes that the package carries the inputs. It does not establish that the
standalone pipeline reproduces the draw, because the pipeline's captured-package path is not yet
wired. That is the next step after a live capture exists.

## 9. Artifact handling

The package contains proprietary game-derived shader code, GCN words and texture bytes. It stays in
a private local directory and is never committed. Only the schema, validator and documentation are
committed.
