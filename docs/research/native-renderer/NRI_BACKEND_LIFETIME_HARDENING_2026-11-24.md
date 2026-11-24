# NRI Backend Lifetime and Asynchronous Submission Hardening

**Date:** 2026-11-24  
**Branch:** `research/native-nri-lifetime-hardening-20261124`  
**Base:** `ed2abd30` (`research/native-semantic-nri-backend-20261124`)  
**Status:** Working code. 12 tests, 192 assertions, all passing on an RTX 3090 Ti.

---

## 1. Executive verdict

**The registry is now authoritative and the backend enforces it.**

Before this pass the backend kept two representations of resource state. The registry tracked generation, ownership and GPU references; five separate maps tracked backend payloads by index alone. Most backend methods consulted the maps and never asked the registry, so a stale id could reach a freed NRI pointer.

That is now impossible. Every lookup validates index, generation, type and lifecycle state. Payloads live behind the registry and are deleted by its destruction callback. There is no second representation to bypass.

### 1.1 Test results

```
t01_bootstrap                    15 passed, 0 failed
t02_external_image               17 passed, 0 failed
t03_depth_stencil_aspects        12 passed, 0 failed
t04_stereo                        9 passed, 0 failed
t05_submission_lifetime          15 passed, 0 failed
t06_semantic_nri_draw            17 passed, 0 failed
t07_resource_registry            38 passed, 0 failed
t08_lifetime_gpu                 13 passed, 0 failed
t09_stale_resource_handles       18 passed, 0 failed
t10_recording_lifetime           19 passed, 0 failed
t11_multi_submission             12 passed, 0 failed
t12_expected_diagnostic_scope     7 passed, 0 failed
```

192 assertions, zero unexpected validation errors.

The indexed stereo renderer still produces correct pixels: layer 0 red, layer 1 blue, corner untouched.

### 1.2 Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 3090 Ti, Vulkan 1.4.357, driver 621.1.0.0 |
| Vulkan SDK | 1.4.363.0 |
| NRI | `06ff6f643777e72eda88bc6fb8d56267b85d872f` (v192) |
| Compiler | Clang 23.1.5 |
| Validation | `VK_LAYER_KHRONOS_validation`, sync validation enabled |

---

## 2. Defects, locations and mechanisms

### 2.1 The registry was bypassed

**Established by code inspection.** The pre-fix `SetVertexBuffer` read:

```cpp
auto it = buffers_.find(buffer.index);   // index only
if (it == buffers_.end()) return;        // silent no-op
vbd.buffer = it->second.buffer;          // possibly freed
```

No generation check, no type check, no lifecycle check, and a silent return on failure. The same pattern appeared in `SetIndexBuffer`, `BindPipeline`, `CreateTextureView`, `CreateGraphicsPipeline`, `UploadBuffer`, `TransitionResource` and `ReadbackTexture`.

**Mechanism:** destroy a resource, keep its id, call any of those methods. The registry correctly reports the id as dead, but the backend never asks it. The map still holds the payload, so the freed NRI pointer is used.

### 2.2 Recording did not own its resources

**Established by code inspection.** The pre-fix retention was:

```cpp
void NriDevice::RetainForCurrentRecording(ResourceId id) {
    if (!recording_) return;
    for (const auto& r : recordingRefs_) { if (r == id) return; }
    recordingRefs_.push_back(id);   // a list, not a reference
}
```

It appended an id to a vector. It did not acquire a registry reference. GPU references were only taken later, inside `Submit()`.

**Mechanism:** record a command referencing A, call `Destroy(A)`, and the registry physically destroys A because nothing references it. `Submit()` then submits a command buffer containing a freed resource. `Submit()` also ignored the return value of `RetainForGpu()`.

### 2.3 Views did not retain their parent textures

**Established by code inspection.** A view's NRI descriptor references its parent texture, but the registry treated the two as independent. Destroying the texture while a view existed freed the texture under the descriptor.

### 2.4 One command buffer serialised every submission

**Established by code inspection.** `BeginCommands()` waited on the fence for the previous submission before resetting the single allocator. Correct for avoiding a reset-while-pending, but it meant two submissions could never be outstanding. The previous `t08` two-submission test therefore did not establish what its name implied.

### 2.5 A closed expectation could be satisfied later

**Established by code inspection.** `ConsumeExpectedDiagnostic` skipped only expectations that had already fired, not ones that had been closed. A closed expectation stayed matchable for the whole run, so a later unrelated error with the same VUID could satisfy it.

### 2.6 What was dynamically reproduced

The previous suite was reproduced first: 8 tests, 123 assertions, zero failures, matching the prior report. The defects above were **not** dynamically reproduced against the old code, because doing so requires deliberately submitting use-after-free work. They were established by reading the exact source, and the new tests demonstrate the corrected behaviour. I am not claiming a dynamic pre-fix reproduction.

---
## 3. Corrected resource lifecycle

```
  Create()
     │  appOwned = true
     ▼
  ┌────────┐   RetainForRecording()   ┌───────────────────┐
  │  kLive │ ───────────────────────► │ refs += (recording)│
  └────────┘                          └───────────────────┘
     │                                          │
     │ Destroy()                                │ CommitRecording()
     ▼                                          ▼
  ┌──────────────────┐                  refs += (gpu, value)
  │ kPendingDestroy  │  appOwned = false
  └──────────────────┘
     │
     │  last reference released AND no dependents
     ▼
  ┌────────────┐
  │ kDestroyed │  generation++
  └────────────┘
```

A resource is physically destroyed only when **all** of these hold:

1. application ownership is revoked (`appOwned == false`),

2. no recording reference remains,

3. no GPU submission reference remains, and

4. no dependent resource exists.

### 3.1 Dependency graph

```
  texture  ◄──── depends on ────  texture-view
     │                                │
     │ cannot retire while             │ releases the edge on Destroy()
     │ a view exists                   │
     ▼                                ▼
  retires when the last view is gone
```

The edge is stored on the dependent's record, so `Destroy()` releases it automatically. Callers cannot forget, and the backend needs no special case.

**Pipelines do not depend on shaders.** NRI copies the shader bytecode during pipeline creation, so the shader objects can be destroyed immediately afterwards. Retaining them would be an unnecessary dependency, and the code says so.

**External images are never owned.** Destroying the wrapper destroys only the NRI texture object. The `VkImage` belongs to the caller and survives, verified in `t10`.

---

## 4. Recording → submission → completion

```
  BeginCommands()
     │  acquire a context whose command buffer is complete
     ▼
  kRecording ──── each command ────► RetainForRecording()
     │                                (before any NRI handle is used)
     │
     ├── invalid operation ──► poisoned = true
     │                              │
     │                              └── EndCommands() fails, AbandonRecording()
     │
     ▼
  kExecutable
     │
     ├── Submit() ok ──► CommitRecording() ──► kSubmitted
     │                    (gap-free handoff)
     │
     └── Submit() fails ──► ReleaseRecording() ──► kAvailable
```

The handoff is atomic within `CommitRecording`: the recording reference is removed and the GPU reference added in the same step, so there is no window in which the resource is unreferenced.

---

## 5. Multi-context ownership and reuse

Four command contexts, each with its own allocator and command buffer.

```
  available ──► recording ──► executable ──► submitted ──► completed ──► available
```

Rules enforced:

- A context is handed out only in `available`.

- `available` is reached only after the fence value for that context has been observed.

- An allocator is never reset while its command buffer is pending.

- When all four are in flight, `BeginCommands` waits for the **oldest** submission. That is backpressure, not a per-frame stall.

- There is no `vkQueueWaitIdle` or `DeviceWaitIdle` on the recording path. `DeviceWaitIdle` appears only in `Shutdown`.

`t11` demonstrates three submissions outstanding at once, and eight sequential submissions through a four-context pool with backpressure and no failure.

**This is backend bookkeeping, not a claim about simultaneous GPU execution.** The submissions may execute sequentially on the single graphics queue. What is established is that the backend accepts and tracks them without an intervening wait.

---

## 6. Error handling and cancellation

| Situation | Behaviour |
|---|---|
| Invalid resource in a command | Rejected, recording poisoned, reason recorded |
| Poisoned recording | `EndCommands` fails and abandons it |
| Abandoned recording | References released, context returned |
| Failed submission | Recording references released, no resubmission |
| Partial init failure | Allocated contexts destroyed |
| Shutdown with work outstanding | `DeviceWaitIdle`, then drain |

Rejections are observable through `LastRejection()`, which names the failure: `stale-generation`, `wrong-type`, `pending-destroy`, `invalid-id`. A rejected call is never a silent no-op.

Distinctions kept explicit:

- **Recoverable rejection** — the recording is poisoned; the caller can abandon and retry.

- **Unsupported capability** — reported as `UNSUPPORTED`, never as a pass.

- **Failed submission** — `Submit` returns false with a message.

- **Device loss** — not distinguished from other submission failures in this pass; noted as a limitation.

- **Programming error** — a poisoned recording cannot be submitted.

---

## 7. Expected-diagnostic semantics

An expectation now has a bounded window:

```
  BeginExpectedDiagnostic(vuid)   window opens
        │
        │  matching messages are attributed here
        ▼
  EndExpectedDiagnostic(token)    intent to close
        │
        │  still open: a message already in flight is attributed
        ▼
  SynchronizeDiagnostics()        window definitively shut
        │
        │  a later message with the same VUID is UNEXPECTED
        ▼
  ExpectationMet(token) / AllExpectationsMet()
```

`SynchronizeDiagnostics()` is the boundary. The validation layer delivers messages on the calling thread, so once control returns from it, every message provoked before that point has been observed. Finalizing there means a closed expectation can never consume a later error.

`t12` proves the properties directly:

| Property | Result |
|---|---|
| A missing expected diagnostic is unmet | pass |
| A closed expectation does not consume a later message | pass |
| An open expectation does consume a matching message | pass |
| An unrelated VUID is never suppressed | pass |
| Two expectations for the same VUID do not satisfy each other | pass |
| Unexpected errors are counted, not hidden | pass |

The summary phrasing remains **zero unexpected validation errors**.

---

## 8. Regression matrix

| Test | Focus | Assertions | GPU |
|---|---|---|---|
| t01 | device, features, NRI adoption | 15 | yes |
| t02 | external image interop | 17 | yes |
| t03 | depth/stencil aspects | 12 | yes |
| t04 | stereo, direct Vulkan | 9 | yes |
| t05 | submission lifetime model | 15 | yes |
| t06 | **indexed stereo through semantic RHI** | 17 | yes |
| t07 | registry semantics, CPU only | 38 | no |
| t08 | lifetime against real completion | 13 | yes |
| t09 | **stale handles rejected** | 18 | yes |
| t10 | **recording ownership, dependencies** | 19 | yes |
| t11 | **multiple outstanding submissions** | 12 | yes |
| t12 | **expectation scoping, CPU only** | 7 | no |

### 8.1 Actual outputs

`t09` — stale handles:

```
[PASS] UploadBuffer rejected a destroyed buffer
[PASS] rejection reason recorded: SetVertexBuffer: stale-generation
[PASS] poisoned recording cannot be ended successfully
[PASS] BindPipeline rejected a destroyed pipeline
[PASS] CreateTextureView rejected a destroyed texture
[PASS] CreateGraphicsPipeline rejected a destroyed shader
[PASS] rejection names wrong-type
[PASS] a valid resource records cleanly
```

`t10` — recording ownership:

```
[PASS] recording reference kept the resource alive after Destroy
[PASS] submission succeeded after destroy-before-submit
[PASS] new use of a pending-destroy resource rejected
[PASS] abandon released the recording reference
[PASS] no duplicate-release bug on completion
[PASS] parent texture survives while a view depends on it
[PASS] texture survives while the second view exists
[PASS] external VkImage survives destruction of its NRI wrapper
```

`t11` — multiple submissions:

```
[PASS] second submission accepted without waiting for the first
[PASS] both submissions outstanding in backend bookkeeping
[PASS] shared resource retained by two in-flight submissions
[PASS] three submissions outstanding simultaneously
[PASS] context pool returned to its initial size
[PASS] pool exhaustion applied backpressure, all 8 submissions accepted
```

`t06` — the renderer still works:

```
[info] layer 0 centre RGBA = 255,0,0,255
[info] layer 1 centre RGBA = 0,0,255,255
[PASS] layer 0 corner is untouched background
[PASS] external Vulkan image still valid after backend teardown
```

---

## 9. Bugs found while hardening

Three real bugs surfaced in my own new code, each caught by the validation layer or by a test:

1. **`AbandonRecording` left the NRI command buffer in the recording state.** The next `BeginCommandBuffer` failed with `already in the recording state`. NRI requires `EndCommandBuffer` even when the work is discarded.

2. **A pipeline whose shader declared a vertex input with no matching attribute.** `VUID-VkGraphicsPipelineCreateInfo-Input-07904`. A test shader declared `location 0` but the pipeline declared no attributes.

3. **Readback on an image still in `UNDEFINED`.** `ReadbackColor` assumes `COLOR_ATTACHMENT_OPTIMAL`; a freshly created image is not.

---

## 10. Remaining limitations and risks

1. **Device loss is not distinguished.** A lost device surfaces as a generic submission failure. A production renderer needs `VK_ERROR_DEVICE_LOST` handling and a recovery path.

2. **One queue.** The registry supports independent completion domains and `t07` proves the semantics, but the backend uses a single graphics queue. Cross-queue ownership transfer is untested.

3. **Four contexts is a fixed pool.** Adequate here; a real renderer would size it from the swapchain image count.

4. **Backpressure waits on the oldest submission.** Correct, but it can stall a frame. A production path would prefer to grow the pool or defer recording.

5. **No descriptor sets or samplers.** Texture binding is still unimplemented, so a textured draw cannot yet be expressed.

6. **NRI multiview is forwarded but untested.** `viewMask` reaches `OutputMergerDesc`; no test exercises it through NRI.

7. **The dependency graph is one level deep.** Views depend on textures. A deeper chain would need transitive retention, which is not implemented.

---

## 11. Next recommended milestone

**Descriptor sets and a sampled texture, then OpenXR swapchain submission.**

The lifetime foundation is now sound enough for a swapchain: external images are wrapped without ownership transfer, destruction is deferred correctly, and multiple submissions can be outstanding. The next real risk is descriptor management, which is the last piece a UE4-style draw needs before the recovered RHI operations can be mapped onto this interface.

---

## 12. Reproduction

```
git clone https://github.com/NVIDIA-RTX/NRI.git _deps/NRI
cd _deps/NRI && git checkout 06ff6f643777e72eda88bc6fb8d56267b85d872f
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNRI_ENABLE_D3D11_SUPPORT=OFF -DNRI_ENABLE_D3D12_SUPPORT=OFF -DNRI_ENABLE_VK_SUPPORT=ON -DNRI_ENABLE_NONE_SUPPORT=OFF -DNRI_STATIC_LIBRARY=ON
cmake --build build

cd native-renderer
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNRI_ROOT=<path>/_deps/NRI
cmake --build build
cd build
for t in t01_bootstrap t02_external_image t03_depth_stencil_aspects t04_stereo \
         t05_submission_lifetime t06_semantic_nri_draw t07_resource_registry \
         t08_lifetime_gpu t09_stale_resource_handles t10_recording_lifetime \
         t11_multi_submission t12_expected_diagnostic_scope; do ./$t || exit 1; done
```

---

## 13. Summary

**Fixed:** the registry is authoritative. Every lookup validates index, generation, type and state. Payloads live behind the registry, so there is no second representation to bypass. Recording acquires real references before any NRI handle is used. Views retain their parent textures. Four command contexts allow multiple outstanding submissions. Rejections are structured and observable. Expectations have a bounded window.

**Preserved:** the indexed stereo renderer still produces red on layer 0 and blue on layer 1, with the corner untouched and the external image surviving teardown.

**Not claimed:** simultaneous GPU execution. The submissions may run sequentially; what is established is that the backend tracks them without an intervening wait.

**Not done:** descriptor sets, samplers, NRI multiview, device-loss recovery, cross-queue transfer.

