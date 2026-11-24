# NRI Lifetime P0 Corrections

**Date:** 2026-11-24  
**Branch:** `research/native-nri-p0-corrections-20261124`  
**Base:** `06bb1f06` (`research/native-nri-lifetime-hardening-20261124`)  
**Status:** Working code. 13 tests, 217 assertions, all passing on an RTX 3090 Ti.

---

## 1. Executive verdict

**Three P0 defects are corrected, and two of them were reproduced against the original code before being fixed.**

The renderer is unchanged. Layer 0 still reads back red, layer 1 blue, the corner stays background, and the external image survives teardown.

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
t13_p0_lifetime_corrections      25 passed, 0 failed   (new)
```

217 assertions, zero unexpected validation errors. Baseline before the patch was 12 tests and 192 assertions, reproduced first.

### 1.2 Environment

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 3090 Ti, Vulkan 1.4.357, driver 621.1.0.0 |
| Vulkan SDK | 1.4.363.0 |
| NRI | `06ff6f643777e72eda88bc6fb8d56267b85d872f` (unchanged) |
| Compiler | Clang 23.1.5 |
| Validation | `VK_LAYER_KHRONOS_validation`, sync validation enabled |

---

## 2. Defect 1 — dependency edges released too early

### 2.1 Location

`resource_registry.cpp`, `ResourceRegistry::Destroy()`. It released the dependency edge as soon as application ownership was revoked:

```cpp
if (r->hasDependency) {
    const ResourceId parent = r->dependency;
    r->hasDependency = false;
    ReleaseDependency(id, parent);   // <-- too early
}
```

`nri_device.cpp`, `NriDevice::Destroy()` did the same thing a second time for texture views.

### 2.2 Failure mechanism

Revoking application ownership does not make the dependent object disappear. A view can still be referenced by a recording or by submitted GPU work, and its NRI descriptor still points at the parent texture. Releasing the edge at `Destroy` let the parent retire while the view was still alive and still in use.

### 2.3 Reproduction

**Dynamically reproduced** against the original `resource_registry.cpp` from `06bb1f06`, in a standalone probe:

```
[FAIL] D1: parent texture alive while GPU-retained view exists
```

The probe created T and V, registered V → T, retained V for GPU work, destroyed both, and found T already gone. No invalid GPU command was submitted to establish this.

### 2.4 Correction

The edge is now released in `MaybeDestroy()`, immediately before the dependent is physically destroyed, and after its backend object has been destroyed:

```cpp
// Destroy the backend object first. Only then is it safe to release the
// parent, because the object's descriptor referenced the parent.
if (r.destroyFn && r.backendObject != nullptr) r.destroyFn(r.backendObject);
r.backendObject = nullptr;
r.state = ResourceState::kDestroyed;
r.id.generation++;
if (hadDependency) { ... ReleaseDependency(...); }
```

The duplicate release in `NriDevice::Destroy()` is removed. The registry is the single owner of that decision.

---

## 3. Defect 2 — executable context could be overwritten

### 3.1 Location

`nri_device.cpp`, `NriDevice::BeginCommands()`. It guarded only `recording_`:

```cpp
if (recording_) { error = "already recording"; return false; }
```

An ended-but-unsubmitted context has `recording_ == false`, so a second `BeginCommands()` replaced `active_` and stranded the first context's references.

Separately, `ctx->recordingToken = nextSubmitValue_` derived the token from the submission counter, which only advances at submission. Two executable recordings could therefore share a token.

### 3.2 Failure mechanism

`BeginCommands` → `EndCommands` → `BeginCommands` overwrote `active_`. The first context stayed `kExecutable` with its recording references held, but nothing could reach it to submit or abandon it. Its references were stranded permanently.

### 3.3 Reproduction

**Dynamically reproduced.** The first version of the new regression crashed against the unfixed code:

```
[NRI ERROR] CommandBufferVal::End: not in the recording state
```

That crash was itself a third defect: `AbandonRecording()` called `EndCommandBuffer` unconditionally, so abandoning an already-ended executable context ended it twice. The pre-fix code could not survive the sequence at all.

### 3.4 Correction

- `BeginCommands()` rejects when an executable context is pending, with a reason naming `Submit` or `AbandonRecording`.

- Recording tokens come from a dedicated `nextRecordingToken_` counter, independent of submission values.

- `AbandonRecording()` ends the NRI command buffer only when the context is still `kRecording`.

---
## 4. Defect 3 — pending-destroy textures accepted new views

### 4.1 Location

`nri_device.cpp`, `NriDevice::CreateTextureView()`. It used a typed payload lookup, which validates identity and type but not lifecycle state:

```cpp
detail::TexturePayload* tp =
    registry_.Payload<detail::TexturePayload>(desc.texture, ResourceType::kTexture, &err);
if (tp == nullptr) { ... }   // no state check
```

`resource_registry.cpp`, `ResourceRegistry::AddDependency()` likewise accepted any resolvable parent, including one in `kPendingDestroy`.

### 4.2 Failure mechanism

A texture whose application ownership was revoked could still gain new dependents. Each new view extended the texture's life, so a texture the caller had destroyed could never retire.

### 4.3 Reproduction

**Dynamically reproduced** against the original registry. The probe kept the texture physically alive with a GPU reference, destroyed it so it entered `kPendingDestroy`, then attempted to add a dependency:

```
[info] parent alive=1 pending=1
[FAIL] D3: AddDependency rejects a pending-destroy parent
```

### 4.4 Correction

The check now exists at the ownership boundary, not only in the caller:

```cpp
// In AddDependency:
if (p->state != ResourceState::kLive) return false;
if (d->state != ResourceState::kLive) return false;
```

`CreateTextureView` additionally resolves the parent record and requires `kLive` before creating any NRI descriptor, so a rejected call never reaches NRI.

---

## 5. Corrected dependency lifetime

```
  Create(T), Create(V), AddDependency(V, T)
        │
        ▼
  Destroy(T)   ──► T: kPendingDestroy, edge V→T still active
  Destroy(V)   ──► V: kPendingDestroy, edge V→T still active
        │
        │  V still has a GPU reference, so V cannot die
        │  and T cannot die because V depends on it
        ▼
  GPU completes
        │
        ▼
  MaybeDestroy(V): destroy V's descriptor, THEN release edge V→T
        │
        ▼
  MaybeDestroy(T): no dependents remain, destroy T
```

The invariant: **if a dependent backend object remains physically alive, every resource it requires also remains physically alive.**

### 5.1 Physical destruction ordering

Order within one resource's destruction:

1. Destroy the backend object (the NRI descriptor, which references the parent).

2. Mark the record destroyed and bump its generation.

3. Release the dependency edge, which may make the parent destroyable.

The edge is captured into locals before the destructor runs, because a destructor may re-enter the registry.

### 5.2 DestroyAll ordering

A plain map iteration would invert the order. `DestroyAll` now sorts by dependency depth: it repeatedly takes resources that no live resource depends on, so dependents are destroyed before their parents. A cycle falls through to the remaining resources rather than looping forever.

---

## 6. Corrected command-context transitions

```
  available ──BeginCommands──► recording
                                  │
                                  │ EndCommands
                                  ▼
                              executable
                                  │
                    ┌─────────────┼─────────────┐
                    │             │             │
              Submit()      AbandonRecording()   │
                    │             │             │
                    ▼             ▼             │
                submitted     available          │
                    │                            │
              completion                       │
                    ▼                            │
                available ◄──────────────────────┘
```

Rules now enforced:

- `BeginCommands()` succeeds only when no recording and no executable context is active.

- A rejected `BeginCommands()` does not touch `active_`, does not reset an allocator, and does not invalidate the original recording.

- `AbandonRecording()` discards either a recording or an executable context, ending the NRI command buffer only when it is still recording.

### 6.1 Recording tokens

Recording tokens come from `nextRecordingToken_`, a counter that advances at recording creation. They are distinct from GPU completion values, which come from `nextSubmitValue_` and advance at submission. `CommitRecording` transfers only the references tagged with that recording's token, so a submission cannot pick up another recording's resources.

---

## 7. New regression test

`t13_p0_lifetime_corrections` — 25 assertions, split into a CPU half and a real-backend half.

| Assertion | What it establishes |
|---|---|
| A1–A2 | Neither destructor runs while the view is GPU-retained; the parent stays alive |
| A3–A4 | Both destructors run after completion, view before texture |
| A5–A6 | Each destructor runs exactly once; both ids invalid |
| A7–A8 | `DestroyAll` drains both and destroys the view first |
| A9 | `AddDependency` rejects a pending-destroy parent |
| B1 | Pending-destroy texture rejects a new view |
| B2–B3 | Second `BeginCommands` rejected with a reason; A still submits |
| B4–B5 | Submitted work retains X; X retires after completion |
| B6 | Begin succeeds after abandoning an executable context |
| B7–B8 | Recording reference keeps X alive; abandon releases it |
| B9 | Recording tokens unique across abandoned recordings |
| B10–B11 | View and parent both stay alive while submitted work references the view |
| B12–B13 | Both retire after completion; no reference leaks |

The backend half uses a real texture, a real view, a real pipeline and a real submission. It destroys both semantic handles before the GPU has necessarily finished, then waits on the fence. No sleeps and no assumed timing.

---

## 8. Full-suite regression

All 13 tests pass, 217 assertions, zero unexpected validation errors.

The indexed stereo renderer is unchanged:

```
[info] layer 0 centre RGBA = 255,0,0,255
[info] layer 1 centre RGBA = 0,0,255,255
[PASS] layer 0 corner is untouched background
[PASS] no cross-layer contamination
[PASS] external Vulkan image still valid after backend teardown
```

Multiple outstanding submissions still work (`t11`: 12 assertions).

---

## 9. Remaining related risks

1. **The dependency graph is one level deep.** Views depend on textures. A deeper chain would need transitive retention, which is not implemented. `DestroyAll`'s depth sort handles arbitrary depth, but `MaybeDestroy` only checks direct dependents.

2. **`DestroyAll` cycle handling is a fallback.** A dependency cycle would destroy in arbitrary order rather than deadlock. No cycle is currently constructible through the public API.

3. **Device loss is still not distinguished** from other submission failures.

4. **One queue.** Cross-queue ownership transfer remains untested.

5. **Descriptor sets, samplers, NRI multiview and OpenXR remain unimplemented**, as scoped out of this patch.

---

## 10. Reproduction

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
         t11_multi_submission t12_expected_diagnostic_scope \
         t13_p0_lifetime_corrections; do ./$t || exit 1; done
```

---

## 11. Summary

**Fixed:** dependency edges survive until the dependent is physically destroyed. Executable contexts cannot be overwritten. Recording tokens are unique and independent of submission values. Pending-destroy textures reject new views, enforced at the registry boundary. `DestroyAll` respects dependency ordering.

**Reproduced before fixing:** defect 1 and defect 3, against the original registry, without submitting invalid GPU work. Defect 2 surfaced as a crash in the new test against the unfixed backend.

**Preserved:** the indexed stereo renderer, multiple outstanding submissions, and all 192 pre-existing assertions.

**Not done:** descriptor sets, samplers, NRI multiview, OpenXR. Those are separate milestones.

