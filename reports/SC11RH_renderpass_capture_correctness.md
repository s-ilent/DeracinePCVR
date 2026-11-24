# SC-11R-H: reference capture render-pass lifecycle correctness

## Result

The defect is real, reproduced, and fixed. A pre-draw capture ended the Vulkan rendering scope and
nothing re-opened it, so the draw it was supposed to observe executed outside any scope.

| Item | Value |
| --- | --- |
| Base | `92820981` (SC-11R) |
| Branch | `research/sc11r-renderpass-hotfix-20261124` |
| GPU fixture verdicts | 7/7 pass |
| Validation errors | 0 |
| Capture OFF vs ON | bit-identical |
| Package tests | 49 pass |
| Reference sync test | pass |

## 1. The defect

`Rasterizer::Draw` recorded this sequence:

```
scheduler.BeginRendering(state, ...)     // opens the scope
CaptureColorTarget(..., is_pre=true)     // -> Image::DownloadScoped
scheduler.BindGraphicsPipeline(...)
cmdbuf.drawIndexed(...)                  // the draw
```

`Image::DownloadScoped` calls `scheduler->EndRendering()`, which records `endRendering()` and clears
`is_rendering`. `BindGraphicsPipeline` does not restart rendering. Nothing between the capture and
the draw re-opened the scope.

The consequence is not subtle. A draw recorded outside a rendering scope is invalid, and the
negative control shows exactly what happens: the draw does not execute, and the attachment keeps its
clear colour.

The post-draw capture has the same shape. It ends the scope after the draw, so a later draw in the
same guest pass would also execute outside one.

## 2. The fix

`Scheduler::OpenRenderingScope` is extracted from `BeginRendering` so a resumed scope can reuse the
same attachment construction with one difference: every attachment opens with a load operation
instead of the clear the state asks for.

`Scheduler::ResumeRendering` re-opens the tracked scope with that load behaviour. It returns false
when no pass has been opened yet, which is the case before the first pass of a frame.

Three properties matter and are deliberate:

**The tracked render state is left unchanged.** A later draw in the same guest pass compares against
it. Rewriting it to "no clears" would make that draw start a fresh pass and clear the target again,
which is the failure the load operation exists to avoid.

**The pipeline binding is invalidated.** `bound_pipeline` and `last_push_layout` are cleared, so the
caller rebinds. The rasterizer binds the pipeline after the capture, so this is already correct, but
the invalidation makes it explicit rather than incidental.

**The resume is unconditional on the capture's status.** A capture that failed after ending the
scope would otherwise leave the draw outside one. The resume runs whenever the capture path was
entered, not only when it succeeded.

Both capture sites call it: the pre-draw capture before the draw, and the post-draw capture after it.

### Why not simply reuse `BeginRendering(state)`

Because `state` still requests `eClear` for a clearing pass. Reusing it would repeat the clear and
discard the contents the draw is meant to inherit. The load operation is the whole point.

### What is not done

No device-wide idle, no `vkDeviceWaitIdle`, no synchronous wait is added to the draw path. The
resume records a `beginRendering` and nothing else.

## 3. The GPU fixture

`tools/geometry_replay/renderpass_capture_test.cpp` reproduces the scheduler's sequence with real
SPIR-V and validation layers enabled. It does not use the emulator.

| Scenario | What it establishes |
| --- | --- |
| A loaded target, one draw, no capture | the baseline image |
| B same draw with pre/post capture | capture ON matches capture OFF |
| C clearing target | clear semantics still correct |
| D two consecutive draws | both land in one target |
| E draw after a capture-induced break | the draw executes inside a scope |
| F depth-tested draw with colour | depth attachment preserved |
| G MSAA configuration | reported unsupported, not copied |
| H partial coverage, capture OFF | reference for the load test |
| I partial coverage, capture ON | the resumed scope loaded, not cleared |

### The negative control

The same sequence with the resume removed:

```
NEGATIVE CONTROL: capture without resuming the scope
    hash=11248824735641314085 distinct=1 covered=4096 draw=no scope_at_draw=no
  draw executed: no, scope open at draw: no
```

`distinct=1` is the clear colour and nothing else. The draw did not execute. This is the defect, and
the fixture sees it.

### Verdicts

```
PASS  the draw executes inside a rendering scope
PASS  the negative control reproduces the defect
PASS  capture OFF and ON produce identical output
PASS  the captured image is not the clear colour
PASS  no validation errors in the corrected path
PASS  a resumed scope loads rather than clears
PASS  an MSAA attachment is reported unsupported rather than copied
0 failure(s)
validation errors total: 0
```

### Why the partial-coverage pair exists

A full-screen triangle covers every pixel, so a clear and a load are indistinguishable in the final
image. Scenarios A, B and C all hash identically at `11479051890106863333` for that reason, which
proves nothing about load semantics.

H and I draw into a quarter of the target. The uncovered pixels keep whatever the pass loaded. Both
hash `8058767532727310245` with 1025 distinct colours, so the resumed scope loaded rather than
cleared. Without this pair the load claim would be untested.

## 4. Fixture defects found while building it

Three, all caught by validation layers rather than by the image comparison:

| Defect | Root cause |
| --- | --- |
| Depth image used as an attachment while still `UNDEFINED` | the fixture transitioned the colour image but not the depth image |
| MSAA attachment copied | a copy requires a single-sample image; the fixture issued the copy anyway |
| Dynamic viewport and scissor not set | the pipeline declared them dynamic but the fixture only set them for the partial case |

All three are fixed. The MSAA case now reports `unsupported` and issues no copy, which is what the
emulator's `CaptureColorTarget` does.

## 5. Deferred readback lifetime

Preserved. The SC-11R dedicated staging buffers and `DeferPriorityOperation` synchronization are
untouched by this hotfix.

`deracine_reference_sync_test` still passes:

```
negative control (read before completion): PASS  saw the staging buffer's prior contents
deferred read after completion: PASS  the read saw the copied contents
separate staging buffers: PASS  the second capture did not overwrite the first
validation errors: 0
```

The pre-draw and post-draw captures use separate allocations, so neither overwrites the other. A
pending or failed reference does not set `reference_output_ready`; that logic is unchanged.

## 6. Unsupported configurations

| Configuration | Behaviour |
| --- | --- |
| Multisampled colour attachment | reported `Multisampled`, no copy issued |
| Format outside the supported set | reported `UnsupportedFormat` |
| Capture larger than the byte bound | reported `Oversize` |
| Pass that clears its colour attachment | reported `UnsupportedFormat` for the pre-draw capture |

The last one is unchanged from SC-11R and is still correct: the clear is already recorded into the
same command buffer, so a copy after it reads the cleared image and a copy before it reads the wrong
state. That configuration has no capturable pre-draw image.

## 7. Runbook correction

`docs/research/native-renderer/SC11R_LIVE_CAPTURE_RUNBOOK.md` referenced the `CUSA11919-qual` dump.
That build hashes to `8da8e46a...`, not the approved `8caee78e...`. The runbook now states the
approved hash as a requirement, names the qualification build explicitly as not interchangeable, and
records what remains unverified until an authorised runtime capture.

## 8. Remaining risks

**The fixture models the scheduler, not the game.** It reproduces the sequence the rasterizer
records. Whether a real Déraciné pass reaches the capture path with the state the fixture models is
unverified.

**A resumed scope costs a pass break.** Every capture now splits the guest's pass into two Vulkan
passes. On a tiled GPU that is not free. The capture is opt-in and default OFF, so production
rendering is unaffected, but a capture run's frame timing is not representative.

**The load operation depends on the attachment contents surviving the break.** They do, because the
scope's store operation is `eStore` and the image stays in its layout. A future change to either
would break this.

**Only the first colour attachment is captured.** A draw with several targets would need each
captured, which this does not attempt.

## 9. Reproducing

```powershell
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay
.\build-geometry-replay\deracine_renderpass_capture_test.exe --verbose
.\build-geometry-replay\deracine_reference_sync_test.exe
```

The fixture enables validation layers itself and reports whether they were available. A run without
them still executes and says so, but the verdicts that depend on validation would be weaker.
