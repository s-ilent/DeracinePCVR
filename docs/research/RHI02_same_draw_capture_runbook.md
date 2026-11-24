# RHI-02: genuine same-draw capture runbook

## Status

**Prepared, not executed.** This runbook requires explicit authorisation to launch the emulator. No
part of it has been run.

It exists to resolve the one remaining RHI-02 proof obligation: that a genuinely captured UE4
semantic tape and its corresponding resource package describe the same draw.

## Why this is not already answered

Two findings from the capture-path audit make the naive configuration insufficient.

### Finding 1: the two exporters select their draw independently

| Exporter | Selection |
| --- | --- |
| Semantic tape | `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` if set, otherwise `RhiEntryProbe::LastRecordedProvenanceId()` |
| Resource package | Scores every captured draw on geometry completeness and writes the first that validates; `SHADPS4_DERACINE_DRAW_PACKAGE_SELECT` filters by vertex-shader hash |

Setting both output flags to one directory does **not** guarantee they describe the same draw. The
tape defaults to the last recorded draw; the package writes the first draw that validates.

**Consequence for the runbook:** an explicit `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` is required, and
it must be the provenance id the package actually wrote. That is only knowable after the package
exists, so the run is two-phase: capture, read the package's provenance id, then export the tape for
that id.

### Finding 2: the two sides record the index resource in different namespaces

| Side | Value | Namespace |
| --- | --- | --- |
| Tape | `rhi_index_resource_lo/hi` from `event.integer_arguments[1]` | original UE4 `FRHIIndexBuffer` object pointer |
| Package | `index_binding.guest_base` | guest GPU virtual address |

Their numeric equality or inequality is not evidence either way. `JoinTapeToPackage` now refuses to
compare across namespaces and reports `insufficient_evidence` with both domains named.

**Consequence for the runbook:** the identity check cannot establish the join from the index resource
alone. A genuine capture needs an independently established object-to-buffer relationship, which is
the missing evidence described in section 6.

## Prerequisites

| Item | Value |
| --- | --- |
| Emulator build | `build/shadPS4.exe` from this branch |
| Emulator SHA-256 | `0918EA5615A31A72201A4415EBE52410D6A882C721090ECB793132F3CEA828CE` |
| Approved game image | the 1.00 SELF, identified by hash rather than by path |
| Game SHA-256 | `8CAEE78E40693B2E57C68CE87CE9507D907B824B6F6A4C481F414AC0627011A0` |
| Title id | `CUSA11919` |

**Do not use the `CUSA11919-qual` image.** It hashes to
`8DA8E46AC5FDC48E4E9B59E459920A266039993DC13A2D92740F8803E46FDF75`, which is not the approved
1.00 image. A-028R's runner used it, and A-038 recorded the discrepancy. Static addresses and
runtime observations from different builds must not be joined.

Verify before running:

```powershell
(Get-FileHash '<path to the approved eboot.bin>' -Algorithm SHA256).Hash
# must be 8CAEE78E40693B2E57C68CE87CE9507D907B824B6F6A4C481F414AC0627011A0
```

## Phase 1: capture the resource package

The package exporter selects its own draw. Run it first, then read which draw it chose.

```powershell
# Set these to your own paths. The lab must already contain the save slot and
# sce_sys/param.sfo the title needs, as described in the headless harness notes.
$lab = '<lab directory containing shadPS4.exe and user/>'
$out = '<output directory for the package and tape>'
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

# Provenance and resource capture. All of these are required: the tape needs the
# provenance record, the package needs the vertex input and shader parameters, and
# the texture needs the texture capture.
$env:SHADPS4_DERACINE_RHI_PROBE = '1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER = '33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE = '1'
$env:SHADPS4_DERACINE_VERTEX_INPUT = '1'
$env:SHADPS4_DERACINE_SHADER_PARAMS = '1'
$env:SHADPS4_DERACINE_TEXTURE_CAPTURE = '1'
$env:SHADPS4_DERACINE_CMD_CAPTURE = '1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER = '33'

# The package only. The tape is exported in phase 2, once the draw is known.
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out
$env:SHADPS4_DERACINE_SEMANTIC_TAPE = '0'

$env:SHADPS4_EXIT_AFTER = '75'
$env:SHADPS4_CLEAN_EXIT = '1'

Start-Process -FilePath "$lab\shadPS4.exe" -WorkingDirectory $lab `
    -ArgumentList @('-g', '<path to the approved eboot.bin>') -Wait
```

### Expected output

| File | Content |
| --- | --- |
| `package.json` | schema 2 package with `provenance_id` |
| `resources.bin` | geometry and shader-buffer bytes |
| `textures.bin` | captured texture bytes |

### Read the selected draw

```powershell
python -c "import json; d=json.load(open(r'$out\package.json',encoding='utf-8')); print('provenance_id', d['provenance_id']); print('index', d['index_binding']['index_count'], 'indices,', d['index_binding']['index_size'], 'bytes each'); print('textures', len(d.get('textures',[])))"
```

Record the `provenance_id`. It is the value phase 2 needs.

If the package is absent, the log says why:

```powershell
Select-String -LiteralPath "$lab\user\log\shad_log.txt" -Pattern 'DRAW_PACKAGE_NONE|refused'
```

## Phase 2: export the semantic tape for that draw

Re-run with the tape enabled and the draw pinned to the package's provenance id.

```powershell
$draw = '<provenance_id from phase 1>'
$env:SHADPS4_DERACINE_SEMANTIC_TAPE = '1'
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR = $out
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW = $draw
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out

Start-Process -FilePath "$lab\shadPS4.exe" -WorkingDirectory $lab `
    -ArgumentList @('-g', '<path to the approved eboot.bin>') -Wait
```

### Why the draw must be pinned

Without `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` the exporter uses the last recorded draw, which is not
necessarily the draw the package describes. Pinning it is a **selection mechanism**, not evidence:
it makes the two exporters agree on which draw to describe, and it does not establish that they
observed the same operation.

### Expected output

| File | Content |
| --- | --- |
| `tape.json` | schema 1 tape with one `DrawIndexed` operation |

### Verify the tape came from the command observation

The tape's draw payload must come from the deferred UE4 command record, not from the package. Check
that the tape carries a command list and an index resource:

```powershell
python -c "import json; t=json.load(open(r'$out\tape.json',encoding='utf-8')); op=[o for o in t['operations'] if o['kind']=='DrawIndexed'][0]; print('command_list', op['command_list']); print('index_resource', op['draw']['index_buffer_resource']); print('primitives', op['draw']['num_primitives']); print('evidence', op['evidence'])"
```

A tape whose `command_list` is zero and whose payload matches the package exactly is a
package-derived tape, not a capture. The exporter records `evidence` per operation; a genuine
capture carries `runtime_confirmed` or `verified_target_binary`.

## Phase 3: validate both artifacts

```powershell
python tools\validate_draw_package.py $out
python tools\test_validator_adversarial.py $out
```

The package validator must report `VALID`. The adversarial suite must reject all cases; it mutates a
copy, so the original is unchanged.

## Phase 4: attempt the same-draw join

This is the step that decides whether RHI-02's central claim holds.

```powershell
.\build-geometry-replay\deracine_tape_replay.exe $out --substitute-render-targets
```

**Do not pass `--synthetic-fixture`.** That flag exists for fixtures built from a package, and using
it here would conceal exactly the failure this runbook exists to detect.

### Reading the outcome

| Output | Meaning | Next |
| --- | --- | --- |
| `provenance: original_tape_matched` | The join succeeded on a real identity field | Proceed to phase 5 |
| `provenance: insufficient_evidence` with `index_domain ... DISAGREE` | The two sides record different namespaces | This is the expected outcome; see section 6 |
| `provenance: rejected` with `index_guest_base ... DISAGREE` | The two sides name different resources | Investigate; the exporters may have selected different draws |
| `identity rejected: ... different draws` | Provenance ids disagree | Phase 2 pinned the wrong draw |
| `draw is out of bounds` | The captured draw exceeds its buffers | The capture is inconsistent; do not proceed |
| `tape read failed` / `package read failed` | A file is missing or malformed | Re-run the capture |

## Phase 5: replay, only if the join succeeded

```powershell
$env:DERACINE_REPLAY_VALIDATION = '1'
.\build-geometry-replay\deracine_tape_replay.exe $out --substitute-render-targets
```

Expected on success:

```
identity: joined
provenance: original_tape_matched
mode: textured
draw_from_tape: index_count=<n> first_index=<n> vertex_offset=<n> instance_count=<n>
bounds: ok max_index=<n> vertex_range=[<n>,<n>]
validation: errors=0
```

The `draw_from_tape` values must equal the tape's semantic payload derivation. The `bounds` line must
report `ok`. The validation count must be zero.

## Evidence to preserve

Keep these privately, outside the repository. Do not commit game bytes.

| Artifact | Why |
| --- | --- |
| `package.json` | the resource side of the join |
| `tape.json` | the semantic side of the join |
| `shad_log.txt` | the exporter's own record of what it selected |
| replay stdout | the join outcome and the executed draw |
| `mutations.json` | only if the mutation matrix was rerun |

Record the SHA-256 of each. The package and tape contain no game executables, but `resources.bin`
and `textures.bin` contain game resource bytes and must stay private.

## Cleanup

```powershell
Get-Process shadPS4 -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item Env:SHADPS4_DERACINE_SEMANTIC_TAPE
Remove-Item Env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR
Remove-Item Env:SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW
Remove-Item Env:SHADPS4_DERACINE_DRAW_PACKAGE
Remove-Item Env:DERACINE_REPLAY_VALIDATION
```

All capture instrumentation is default OFF. Nothing in this runbook changes that.

## 6. The evidence this runbook cannot produce

The join needs an independently established relationship between the original UE4 index-buffer
object and the guest GPU address the draw consumed. Neither exporter records it today.

What exists:

| Fact | Where |
| --- | --- |
| The tape records the RHI invocation's index-buffer argument | `rhi_entry_probe.cpp`, `event.integer_arguments[1]` |
| The package records the guest GPU address | `shader_param_capture.cpp`, `index_binding.guest_base` |
| The RHI context holds the bound index buffer | not yet located |

What is missing:

1. **The original UE4 index-buffer object's guest allocation.** The RHI resource object is a host
   structure; the buffer it describes lives at a guest address. The mapping between them is what
   turns an object pointer into a comparable address.
2. **A capture of that mapping at the draw boundary.** It must be recorded for the same draw, not
   inferred from a nearby observation.

Until both exist, a genuine capture will report `insufficient_evidence`, and that is the correct
outcome. It is not a failure of the matcher.

### The smallest implementation that would close it

Record, at the RHI draw entry, the guest address of the index buffer the invocation consumed. The
RHI entry already reads the index-buffer argument; the missing step is resolving that object to its
guest allocation through the RHI's own resource tracking, and recording the result alongside the
provenance record.

That is a capture-side change in `rhi_entry_probe.cpp`, testable offline against a synthetic
provenance record. It is not implemented, and this runbook does not assume it.

## 7. What a successful run would and would not prove

**Would prove:** one original UE4 indexed draw's semantic payload and its resource package describe
the same operation, and an independent Vulkan backend executed that draw using the captured geometry,
texture and sampler.

**Would not prove:** original shader execution, original material reproduction, whole-frame
execution, or OpenXR integration. The diagnostic SPIR-V shader remains a substitute, and the
render targets remain ours.
