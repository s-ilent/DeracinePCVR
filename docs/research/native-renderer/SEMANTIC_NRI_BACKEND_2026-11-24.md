# Semantic RHI / NRI Renderer — End-to-End Implementation

**Date:** 2026-11-24  
**Branch:** `research/native-semantic-nri-backend-20261124`  
**Base:** `7047e906` (`research/native-renderer-harness-20261124`)  
**Status:** Working code. 8 tests, 123 assertions, all passing on an RTX 3090 Ti.

---

## 1. Executive verdict

**A complete rendering path now runs: semantic RHI → NRI → Vulkan → externally owned stereo image → GPU execution → verified readback.**

The decisive evidence, from `t06_semantic_nri_draw`:

```
[PASS] commands submitted through NRI, completion value 1
[PASS] GPU completion observed
[info] layer 0 centre RGBA = 255,0,0,255
[info] layer 1 centre RGBA = 0,0,255,255
[PASS] layer 0 centre is red
[PASS] layer 1 centre is blue
[PASS] layer 0 corner is untouched background
[PASS] no cross-layer contamination
[PASS] external Vulkan image still valid after backend teardown
```

The pixels were produced by an indexed draw recorded through the semantic interface, executed by NRI on the GPU. They were not manufactured by a Vulkan clear.

### 1.1 Test results

```
t01_bootstrap              15 passed, 0 failed
t02_external_image         17 passed, 0 failed
t03_depth_stencil_aspects  12 passed, 0 failed
t04_stereo                  9 passed, 0 failed
t05_submission_lifetime    15 passed, 0 failed
t06_semantic_nri_draw      17 passed, 0 failed
t07_resource_registry      26 passed, 0 failed
t08_lifetime_gpu           12 passed, 0 failed
```

Zero unexpected validation errors across the suite.

### 1.2 Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 3090 Ti, Vulkan 1.4.357, driver 621.1.0.0 |
| Vulkan SDK | 1.4.363.0 |
| NRI | `06ff6f643777e72eda88bc6fb8d56267b85d872f` (v192) |
| Compiler | Clang 23.1.5 |
| Validation | `VK_LAYER_KHRONOS_validation`, sync validation enabled |

---

## 2. The ownership correction

### 2.1 What was wrong

The previous `LifetimeTracker` in `t05_submission_lifetime.cpp` marked a resource dead whenever the submission referencing it completed. That is wrong for persistent resources.

**GPU submission completion releases a GPU reference. It does not destroy an application-owned resource.**

### 2.2 The corrected model

A resource has two independent reasons to stay alive:

```
  application ownership  ──┐
                            ├──► physical destruction only when BOTH are gone
  GPU submission refs    ──┘
```

State transitions:

```
  Create()                ──► kLive          (appOwned = true)
  RetainForGpu()          ──► kLive          (gpuRefs += 1)
  CompleteDomain()        ──► kLive          (gpuRefs -= 1, app still owns)
  Destroy()               ──► kPendingDestroy (appOwned = false)
  CompleteDomain()        ──► kDestroyed      (last gpuRef released)
```

`CanRecord()` returns false in `kPendingDestroy`, so a resource awaiting physical destruction cannot accept new GPU work while previously recorded commands still finish safely.

### 2.3 Completion domains

GPU references are keyed by `(domain, value)`. A resource referenced by submissions on two independently progressing queues is not destroyed until both domains have completed. `t07` verifies this directly.

---
## 3. Architecture and responsibility split

```
  semantic_rhi.h          backend-independent commands and resource identities
        │                   (no Vulkan or NRI type appears here)
        ▼
  backend/nri_device.*    NRI handles, barriers, descriptors, queues, fences
        │
        ▼
  NRI  ──►  Vulkan  ──►  GPU
```

| Responsibility | Owner |
|---|---|
| Command recording, pipeline binding, barriers, draws, submission | **NRI** |
| Device creation, externally owned images, test-only readback | **Vulkan** |
| Resource identity, ownership, deferred destruction | **ResourceRegistry** |
| Mapping recovered Déraciné operations onto semantic commands | *out of scope* |

The backend never renders through direct Vulkan. Readback is the only Vulkan operation on the verification path, and it is test-only.

### 3.1 File layout

```
native-renderer/src/
  common/
    resource_registry.{h,cpp}   corrected ownership model
    semantic_rhi.h              backend-independent interface
    diagnostics.{h,cpp}         status vocabulary + expected diagnostics
    vk_bootstrap, nri_adoption, image_io, shader_compile   (unchanged)
  backend/
    nri_device.{h,cpp}          the NRI implementation
  tests/
    t06_semantic_nri_draw.cpp   the decisive end-to-end test
    t07_resource_registry.cpp   deterministic CPU ownership test
    t08_lifetime_gpu.cpp        lifetime against real GPU completion
```

---

## 4. Implemented API surface

Only what the test exercises. Nothing is a silent no-op.

| Method | Implemented | Notes |
|---|---|---|
| `CreateBuffer` | Yes | `HOST_UPLOAD` memory so `UploadBuffer` can map it |
| `UploadBuffer` | Yes | Synchronous `MapBuffer`/`UnmapBuffer` |
| `CreateTexture` | Yes | NRI committed texture |
| `ImportExternalImage` | Yes | Wraps a caller-owned `VkImage`; never destroys it |
| `CreateTextureView` | Yes | `COLOR_ATTACHMENT` or `DEPTH_STENCIL_ATTACHMENT` view type |
| `CreateShader` | Yes | Original test SPIR-V, not PS4 shader code |
| `CreateGraphicsPipeline` | Yes | Root-constant block of 64 bytes |
| `BeginCommands` / `EndCommands` | Yes | Waits for the previous submission before resetting |
| `BeginRenderPass` / `EndRenderPass` | Yes | NRI `CmdBeginRendering` |
| `SetViewport` / `SetScissor` | Yes | NRI dynamic state |
| `BindPipeline` | Yes | Sets layout then pipeline |
| `SetVertexBuffer` | Yes | Forwards the stride (see 6.1) |
| `SetIndexBuffer` | Yes | UINT16 / UINT32 |
| `SetShaderParameters` | Yes | Byte-exact snapshot into backend-owned storage |
| `TransitionResource` | Yes | NRI `CmdBarrier` with layout+access+stage |
| `DrawIndexed` | Yes | NRI `CmdDrawIndexed` |
| `Submit` | Yes | NRI `QueueSubmit` with a timeline fence |
| `WaitForCompletion` | Yes | NRI `Wait` on the fence |
| `ReadbackTexture` | Yes | Direct Vulkan, test-only |
| `Destroy` | Yes | Revokes app ownership; defers physical destruction |

### 4.1 What is deliberately absent

No compute dispatch, no descriptor sets, no samplers, no MSAA resolve, no multiview pipeline. Each would be an unimplemented abstraction, and the milestone asked for a small working implementation instead.

---

## 5. External image lifetime rules

| Rule | Enforcement |
|---|---|
| The backend never destroys an imported `VkImage` | `TexturePayload::external` gates the destructor |
| The NRI wrapper is destroyed, the image is not | `DestroyTexture` only |
| The image outlives backend teardown | Verified in `t06` after `device.reset()` |
| No assumption that the image index is sequential | Layers are addressed by explicit `baseLayer` |
| No per-frame `vkQueueWaitIdle` | Completion is tracked by a timeline fence |

---

## 6. Validation errors discovered and corrected

The validation layer caught **five real bugs in my own implementation**. Each is a finding about the API contract.

### 6.1 Vertex stride must be forwarded

**Symptom:** the draw submitted successfully but every pixel stayed background.

**Cause:** NRI takes the vertex stride from `VertexBufferDesc::stride` when `features.extendedDynamicState` is supported, and ignores `VertexStreamDesc::stride`. I passed `0`, so every vertex read from offset 0 and the triangles were degenerate.

**Fix:** `SetVertexBuffer` now takes an explicit stride. This is a real trap for any host porting UE4 vertex declarations.

### 6.2 A command buffer must not be reset while pending

```
VUID-vkResetCommandPool-commandPool-00040: command buffer is in use
VUID-vkBeginCommandBuffer-commandBuffer-00049: still in use (pending state)
VUID-vkQueueSubmit2-commandBuffer-03875: already in use
```
**Cause:** `BeginCommands` reset the allocator while the previous submission was still in flight.

**Fix:** `BeginCommands` waits on the fence for the last submitted value first. This is exactly the lifetime class the milestone targets, caught in my own code.

### 6.3 A render-target view is not a shader-resource view

```
nri::DeviceVal::CreateDescriptor: 'viewType' is not supported by the texture (type or usage)
```
**Cause:** I created `TextureView::TEXTURE` for a color attachment.

**Fix:** color aspects use `COLOR_ATTACHMENT`, depth/stencil use `DEPTH_STENCIL_ATTACHMENT`.

### 6.4 The wrapper needs the real Vulkan format

```
nri::DeviceVal::CreateTexture: 'vkFormat' is invalid
```
**Cause:** `ImportExternalImage` passed format `0`.

**Fix:** the semantic format is mapped back to a `VkFormat`, and the semantic usage to `VkImageUsageFlags`.

### 6.5 Buffers that are uploaded into must be host-visible

**Symptom:** `MapBuffer failed`.

**Cause:** the buffer was created in `MemoryLocation::DEVICE`.

**Fix:** `HOST_UPLOAD` for buffers the caller uploads into.

---

## 7. Expected-diagnostic mechanism

The previous sRGB negative test observed a validation error and then manually subtracted it from a global counter. That is not auditable and can hide unrelated errors.

Replaced with an explicit mechanism:

```cpp
const uint64_t token = harness::BeginExpectedDiagnostic("VUID-VkImageViewCreateInfo-image-12397");
// ... provoke the error ...
harness::EndExpectedDiagnostic(token);
// at the end of the run:
if (!harness::AllExpectationsMet()) log.Fail("a declared expected diagnostic never fired");
```

Properties:

- Matches on the **VUID**, not on a count.

- Scoped to the negative test that declares it.

- **Fails the run if the diagnostic never fires** — a negative test that stops rejecting must not silently pass.

- Never suppresses unrelated errors: anything unmatched increments `UnexpectedErrorCount()`.

- Stays matchable for the whole run, because Vulkan delivers validation messages asynchronously.

- The summary reports **zero unexpected validation errors**, not "no errors were emitted".

Verified by a self-test: declaring a VUID that never fires is correctly rejected.

---

## 8. Recorded GPU test outcomes

### 8.1 t06 — the decisive draw

| Step | Result |
|---|---|
| Externally owned two-layer image | created, both layers cleared to black |
| Semantic NRI device | created over the adopted Vulkan device |
| External image import | wrapped, ownership preserved |
| Per-layer views | created |
| Vertex + index buffers | uploaded (4 vertices, 6 indices) |
| Graphics pipeline | created from original GLSL |
| Submission | through NRI, completion value 1 |
| GPU completion | observed via timeline fence |
| Layer 0 centre | **255,0,0,255** (red) |
| Layer 1 centre | **0,0,255,255** (blue) |
| Layer 0 corner | 0,0,0,255 (untouched background) |
| Cross-layer contamination | none |
| External image after teardown | still valid |

The corner check matters: it proves the draw rasterised a bounded quad rather than filling the target.

### 8.2 t08 — lifetime against real GPU completion

| Scenario | Result |
|---|---|
| Submission completes, app still owns | resource remains valid |
| Destroy before GPU completion | physical destruction deferred |
| Pending-destroy resource | rejects new work |
| Two in-flight submissions | retained until both complete |
| Stale generation | rejected |
| Parameter mutation after recording | recorded value unchanged |
| Shutdown with outstanding work | drains without crash |

---

## 9. Working code versus planned interfaces

| Item | Status |
|---|---|
| Resource registry with deferred destruction | **Working, tested** |
| Semantic RHI interface | **Working, exercised by t06** |
| NRI backend | **Working, renders on GPU** |
| Indexed stereo draw | **Working, verified by readback** |
| External image import | **Working, ownership verified** |
| GPU completion tracking | **Working, timeline fence** |
| Expected-diagnostic mechanism | **Working, self-tested** |
| Multiview through NRI | **Not implemented** — direct Vulkan only (t04) |
| Samplers, descriptor sets | **Not implemented** |
| Compute dispatch | **Not implemented** |
| MSAA resolve | **Not implemented** |
| OpenXR | **Deferred by design** |
| Game-specific adapter | **Out of scope** |

---

## 10. Remaining unsupported operations

1. **NRI multiview rendering.** `OutputMergerDesc::viewMask` exists and the backend forwards it, but no test exercises it through NRI. `t04` verifies multiview through direct Vulkan only.

2. **Descriptor sets and samplers.** The pipeline uses a single root-constant block. Texture binding would need `DescriptorSetDesc` and `CmdSetDescriptorSet`.

3. **Multiple queues.** The registry supports independent completion domains and `t07` proves the semantics, but the backend uses one graphics queue.

4. **Command buffer pools.** One command buffer is reused, with a fence wait in `BeginCommands`. A pool would avoid that stall.

---

## 11. Next concrete steps

1. **Add NRI multiview** to `t06` using `viewMask=0x3`, then `0x1` for isolation. The backend already forwards the mask.

2. **Add descriptor sets and a sampler** so a textured draw can be verified, which is the next operation a real UE4 draw needs.

3. **Pin the Khronos OpenXR SDK** (already built locally at `release-1.1.65`) and add the compositor submission test. The ownership boundary is already correct: external images are wrapped, never destroyed.

4. **Wire the semantic RHI to the recovered UE4 operations.** The mapping is documented in the harness report; the game adapter remains out of scope here.

---

## 12. Reproduction

```
# NRI (pinned)
git clone https://github.com/NVIDIA-RTX/NRI.git _deps/NRI
cd _deps/NRI && git checkout 06ff6f643777e72eda88bc6fb8d56267b85d872f
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNRI_ENABLE_D3D11_SUPPORT=OFF -DNRI_ENABLE_D3D12_SUPPORT=OFF -DNRI_ENABLE_VK_SUPPORT=ON -DNRI_ENABLE_NONE_SUPPORT=OFF -DNRI_STATIC_LIBRARY=ON
cmake --build build

# Harness
cd native-renderer
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNRI_ROOT=<path>/_deps/NRI
cmake --build build
cd build
./t01_bootstrap && ./t02_external_image && ./t03_depth_stencil_aspects && ./t04_stereo
./t05_submission_lifetime && ./t06_semantic_nri_draw && ./t07_resource_registry && ./t08_lifetime_gpu
```

---

## 13. Summary

**Proven:** a representative indexed draw is created, bound, recorded and submitted through the semantic RHI, executed by NRI on the GPU, into an externally owned Vulkan image, with per-layer output verified by readback. Application ownership and GPU submission retention are enforced independently. Multiple in-flight submissions cannot destroy a still-referenced resource. Generation-tagged IDs reject stale references. Zero unexpected validation errors.

**Corrected:** the previous lifetime model, which destroyed resources at GPU completion regardless of application ownership.

**Caught by validation:** five real bugs in my own code, including a command buffer reset while pending and a zero vertex stride that silently produced an empty draw.

**Not done:** NRI multiview, descriptor sets, samplers, compute, MSAA resolve, OpenXR. Each is named rather than stubbed.

**Unchanged:** the native shader execution blocker. This milestone renders with original test shaders, not PS4 shader binaries.

