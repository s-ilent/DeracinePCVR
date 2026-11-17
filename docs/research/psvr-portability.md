# PSVR1 PC port portability review

Read-only comparison of five native PlayStation VR PC ports, against the already-working VR
behaviour of `DeracinePCVR`. Written 2026-11-17. No build was run, no emulator or GPU/HMD tool was
launched, and no repository other than the five listed was modified.

Audience: whoever designs Déraciné's future PSVR work and asks "what can we reuse from the other
five ports, and what must stay ours?".

---

## 0. Provenance and method

All five repositories were reachable and are public, GPL-2.0 (the same licence as the shadPS4 tree
in `DeracinePCVR/src/`, so code movement is licence-compatible but attribution obligations apply).

| Port | Repository | Revision inspected | Notes |
|---|---|---|---|
| Wipeport | <https://github.com/shipa-2/Wipeport> | `main` @ `8fa75683474ffbfe0ae72da747326a805ec120a6` (2026-10-19) | Deepest inspection: docs plus source |
| AstroQuest | <https://github.com/bigmak94/AstroQuest> | `main` @ `a56f5b1405acddb9935bdd3c8430fb16013399e8` (2026-11-14) | Origin of the shared PSVR libraries |
| revive-psvr1-wipeout | <https://github.com/jshodanVR/revive-psvr1-wipeout> | `main` — commit not resolved; root tree not fetched (repo is ~122 MB) | Docs (`README.md`, `docs/PROVENANCE.md`) and top-level tree only |
| Farpoint | <https://github.com/evertec82/Farpoint> | `main`, root tree `de6d8df1165aac8e09d90febfb4fb6a30384f48c` | Docs plus tree; source tree not line-audited |
| BloodTruth | <https://github.com/evertec82/BloodTruth> | `main`, root tree `af0dd8d14b4069f5499c933650d12a398f695a1e` | Docs plus tree; source tree not line-audited |

Verified by direct reading of file contents (not by inference from README claims alone): Wipeport's
`docs/VR.md`, `docs/GAME_CODE.md`, `core/vr/vr_runtime.{h,cpp}`, `core/vr/openxr_host.cpp`,
`core/libraries/hmd/hmd.{h,cpp}`-adjacent files, `core/libraries/hmd/hmd_reprojection.cpp`,
`core/libraries/vr_tracker/vr_tracker.h`, `video_core/renderer_vulkan/vk_vr_exporter.{h,cpp}`;
AstroQuest's `src/core/vr/` tree and `late_frame_wait.h`; and the local
`DeracinePCVR/src/src/` files named below.

Two structural facts worth stating up front, because they change how the comparison reads:

1. **These are not five independent implementations.** Wipeport's own header comment says its VR
   layer is "AstroQuest's VrExporter", and its `docs/VR.md` says the PSVR libraries come "from
   AstroQuest". Farpoint derives from AstroQuest commit `3a75299f`; BloodTruth derives from
   "AstroQuest and Farpoint"; revive-psvr1-wipeout's `docs/PROVENANCE.md` names AstroQuest, shadPS4
   and shadps4-arm64 as its ancestry. Déraciné is the same lineage again: its `core/vr/` contains
   `openxr_host.cpp`, `vr_runtime.cpp`, `vr_host_link.cpp` and `headset_fov_cache.h` under the same
   names and with the same `Config`/`Fov`/`PresentedFrame` vocabulary. **The comparison is therefore
   mostly a comparison of divergences from one common ancestor, not of five designs.**
2. **The "array-layer presentation" premise in the brief does not hold for Wipeport.** See §2.3 and
   §3.3. The OpenXR layer is a single packed side-by-side image (`arraySize = 1`) addressed by
   per-view `imageRect`. This is the single most consequential correction in this document.

---

## 1. Compatibility matrix

Legend: **✓** verified in source or authoritative in-repo doc · **~** partially verified (doc-backed,
source not line-read) · **?** not verified · **—** not applicable to that title.

| Axis | Wipeport (CUSA05670) | AstroQuest (AstroBot) | revive-psvr1-wipeout (CUSA05670) | Farpoint (CUSA04508) | BloodTruth (CUSA11108) |
|---|---|---|---|---|---|
| **1. HMD/tracker interface** | ✓ Re-implements `libSceHmd` + `libSceVrTracker` in-process; backend is OpenXR, never USB/HID | ✓ Origin of those libraries | ~ Same lineage (AstroQuest/shadPS4/shadps4-arm64) | ~ AstroQuest libraries, "fix tracker initialization" | ~ AstroQuest + Farpoint lineage |
| **2. Eye/frustum conversion** | ✓ **Symmetric enclosing** FOV told to the title; lens frusta restored per view at submit | ~ `fov_from_headset` / `fov_symmetric` switches live in the shared runtime it authored | ? | ~ "Correct packed stereo submission and per-eye sampling" | ? |
| **3. Eye-array vs SBS** | ✓ SBS packed, `arraySize = 1`, per-view `imageRect` (not array layers) | ? | ? | ~ "packed stereo submission" | ? |
| **4. Image ownership / copy** | ✓ Host-owned 4-slot pool, guest draws into it, **GPU copy** into swapchain | ✓ Two models: local host + Quest app owning images over a socket (`vr_host_link`) | ? | ? | ? |
| **5. Frame readiness / GPU sync** | ✓ Explicit CPU wait on `ready_tick` in `VrExporter::Deliver()` | ✓ `late_frame_wait.h` — but it is a **CPU** deadline wait, not a GPU fence | ? | ~ "readback … before signaling GPU completion" | ? |
| **6. 60/120 timing + reprojection** | ✓ Flips on every refresh; guest counts 2 flips/frame; 120 Hz via **binary patch** | ✓ (Astro Bot "60 fps reprojected to 120 Hz") | ? | ~ "full-refresh pacing instead of the default half-refresh wait" | ? |
| **7. Controllers / hands / input** | ✓ OpenXR hands+grips machinery present but **disabled**; input via its own `runtime_pad.c` | ✓ Hand-tracking EXT, grips, palm sampling; controllers stand in for a gamepad | ~ "PC gamepad and VR controller gamepad input" | ~ Tracked **Aim** + gamepad modes; mapping documented | ~ Two tracked controllers, documented Move mapping |
| **8. Per-title vs reusable** | ✓ Reusable `Core::Vr::Runtime` + libs; per-title = binary patches, ini, env | ✓ Reusable libs + settings; per-title = launcher/settings | ~ Per-title launcher/config | ✓ Per-title launcher, profiles, metadata, memory budgets | ✓ Per-title launcher, resolution pack |

Déraciné, for reference, sits on the same axes as: (1) full re-implementation ✓, (2) asymmetric
per-eye FOV **recovered from the title's reprojection UVs** ✓, (3) SBS packed ✓, (4) zero-copy
guest→presenter with one GPU copy to the swapchain ✓, (5) explicit CPU wait on `ready_tick` ✓,
(6) flips + reprojection lifecycle ✓, (7) Move/DS4 semantics ✓, (8) reusable runtime + per-title
layout inference ✓.

---

## 2. Per-axis notes

### 2.1 HMD and tracker service interfaces

**Wipeport** re-implements the two PSVR system libraries and puts OpenXR behind them. There is no
USB, HID or `libusb` PSVR transport anywhere in the VR path; nothing in these five repositories
talks to PSVR1 hardware.

- `gpu/shadps4/core/libraries/hmd/hmd.h` declares the full `libSceHmd` surface — `sceHmdOpen`,
  `sceHmdGetFieldOfView`, `sceHmdGetDeviceInformation`, `sceHmdGetEyeOffset`, `sceHmdClose`, plus
  the entire `sceHmdInternal*` and `sceHmdDistortion*` sets, the `OrbisHmdReprojection*` family, and
  the recovered-layout structs (`OrbisHmdReprojectionParam`, `OrbisHmdReprojectionEyeUv`,
  `OrbisHmdReprojectionTrackerState`). Comments there state the reprojection structs are
  "not documented anywhere public" and were recovered from how titles fill them in — the same
  provenance as Déraciné's own `hmd.h`.
- `gpu/shadps4/core/libraries/vr_tracker/vr_tracker.h` is a faithful `libSceVrTracker` surface:
  `sceVrTrackerQueryMemory`, `Init`, `RegisterDevice`, `GpuSubmit`, `GpuWait`,
  `GpuWaitAndCpuProcess`, `CpuProcess`, `GetResult`, `Recalibrate`, `SetLEDBrightness`, etc., with
  the real memory constants (`BASE_ONION_SIZE = 0x400000`, `GARLIC_SIZE = 0x3000000`,
  `WORK_SIZE = 0x1000000`) and the real device enum
  (`HMD`, `DUALSHEK4`, `MOVE`, `GUN`). This is the interface a shared runtime should own: the
  guest-visible ABI is identical across all five ports because they all descend from this file.
- What changes per port is only **where poses come from**: Wipeport feeds them from OpenXR through
  `Core::Vr::Runtime`; AstroQuest additionally feeds them from a Quest host app over IPC.

**Implication.** Axis 1 is already solved and already shared. It is not where the divergence or the
risk is.

### 2.2 OpenXR eye/frustum conversion — and Wipeport's symmetric enclosing FOV

This is Wipeport's most distinctive and most transferable idea, and it is worth stating exactly.

**The PSVR value.** `gpu/shadps4/core/vr/vr_runtime.h` defines:

```cpp
struct Fov {
    float tan_out{1.20743f};
    float tan_in{1.181346f};
    float tan_top{1.262872f};
    float tan_bottom{1.262872f};
};
```

Those tangents are PlayStation VR's per-eye half-angles: `atan(1.20743) ≈ 50.36°`,
`atan(1.181346) ≈ 49.75°`, `atan(1.262872) ≈ 51.63°` — i.e. ≈100.1° horizontal by ≈103.3° vertical,
matching `docs/VR.md`'s "100°×103° an eye". Note the asymmetry is horizontal (temple vs nose) and
the field is symmetric vertically.

**The conversion.** `Core::Vr::Runtime::TitleFov()` in `gpu/shadps4/core/vr/vr_runtime.cpp` is what
`sceHmdGetFieldOfView` returns. When `Config::fov_symmetric` is on it does:

```cpp
if (config.fov_symmetric) {
    // Scaling asymmetric per-eye bounds also shrinks the binocular overlap: directions
    // visible on one eye's temple side become black on the other's nose side. Render an
    // encompassing, centred frustum in both eyes instead. At 100% it covers the entire
    // real headset; the compositor clips the extra area to the actual lens bounds.
    base.tan_out = base.tan_in = std::max(base.tan_out, base.tan_in);
    base.tan_top = base.tan_bottom = std::max(base.tan_top, base.tan_bottom);
}
```

So the title is told a **symmetric frustum built from the per-axis maximum** of the headset's real
asymmetric frusta — an enclosing envelope — and `fov_scale` (`SHADPS4_VR_FOV`/`vr` percentage)
scales it before it is handed over. `TitleFov()` then caches the result in `config.fov`, because
"the title asks once, when it opens the headset".

**The payoff is recovered at submit, not at draw.** `Impl::FillProjection()` in
`gpu/shadps4/core/vr/openxr_host.cpp` takes the envelope the frame was drawn with (`shown.fov`) and
the lens field of view sampled by `xrLocateViews` (`lens_fov`, via `ParallelStereoFov`), intersects
them, and hands the compositor the lens's own field of view plus a cropped `imageRect`:

```cpp
const Fov& drawn = shown.fov;
Fov fov = drawn;
if (lens_fov) {
    fov = {std::min(lens_fov->tan_out, drawn.tan_out), std::min(lens_fov->tan_in, drawn.tan_in),
           std::min(lens_fov->tan_top, drawn.tan_top), std::min(lens_fov->tan_bottom, drawn.tan_bottom)};
}
...
views[eye].fov.angleLeft  = eye == 0 ? -angle_out : -angle_in;
views[eye].fov.angleRight = eye == 0 ?  angle_in  :  angle_out;
...
const auto left  = static_cast<s32>(std::lround((drawn_left - shown_left) / drawn_width * width));
const auto right = static_cast<s32>(std::lround((drawn_left + shown_right) / drawn_width * width));
views[eye].subImage.imageRect.offset = {eye * eye_width + left, top};
views[eye].subImage.imageRect.extent = {right - left, bottom - top};
```

The comment states the invariant: "Cutting it out of the planar picture is exact". That is correct —
cropping a planar projection to a narrower frustum expressed in the *same* projection is a pure
sub-rectangle, no resampling.

**Why they did it (their own evidence).** `docs/VR.md`: with a Quest 2 under SteamVR reporting
49°/45°/48°/50° (temple, nose, up, down), the port tells the title 49°/49°/50°/50°. Two failures
drove this: "Drawn with a PS VR's field of view, objects did not line up between the eyes in a Quest
2; drawn with the Quest's asymmetric one, the right eye lost most of its picture on some tracks."
The `Config` comment in `vr_runtime.h` repeats it: "WipEout drawn with a Quest 2's asymmetric field of
view (45 degrees on the nose side, 49 on the temple's) lost most of the right eye's picture on some
tracks".

**What is genuinely reusable here** is the *concept* — ask the compositor what the lenses show,
intersect, submit a sub-rect with the lens frustum — not necessarily the default.

**IPD is applied on the compositor side, not in the guest.** In `FillProjection()`, each view's pose
is the head pose offset by `±ipd/2` rotated by head orientation:

```cpp
const XrVector3f offset = Rotate(orientation, {side * ipd, 0.0f, 0.0f});   // side = ∓0.5
views[eye].pose.position = {shown.render_pose.position.x + offset.x, ...};
```

`docs/VR.md` confirms: "The game positions both eyes from the eye poses the tracker library reports
(`sceVrTrackerGetResult`). The port passes it the headset's own IPD from OpenXR", overridable with
`{"ipd_mm": 63}` in `user/vr.json`. So the title's own `hmd_info.left_eye_pose`/`right_eye_pose` are
**not** the authority for the submitted projection — an important divergence from a title that
derives its own eye separation.

### 2.3 Eye-array vs side-by-side presentation — **the brief's premise is wrong for Wipeport**

`docs/VR.md` says: "The game renders both eyes into one texture array: layer 0 is the left eye,
layer 1 the right. The port copies each layer to its eye's OpenXR swapchain." Read literally that
describes an `arraySize = 2` swapchain with two array-layer slices. **The code does not do that.**

`Impl::EnsureSwapchain()` in `gpu/shadps4/core/vr/openxr_host.cpp` creates exactly one swapchain
whose image is the *whole packed frame*:

```cpp
XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                  XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
info.format = static_cast<...>(swapchain_format);
info.sampleCount = 1;
info.width = width;         // the packed frame width, i.e. 2 × eye width
info.height = height;
info.faceCount = 1;
info.arraySize = 1;         // <-- one layer
info.mipCount = 1;
```

and `FillProjection()` addresses the eyes as two halves of that one image — `eye_width =
swapchain_width / 2`, `imageRect.offset = {eye * eye_width + left, top}`. The slot images the guest
draws into are also single-layer (`Impl::Create()`: `.arrayLayers = 1`), and `Impl::CopyFrame()`
copies `ColorLayer` (`layerCount = 1`) from slot to swapchain.

So the real model in Wipeport is: **one packed side-by-side image, one swapchain, `arraySize = 1`,
two `XrCompositionLayerProjectionView`s with per-view `imageRect`** (each further cropped by the lens
FOV). The prose in `docs/VR.md` describing "each layer … its eye's swapchain" is loose wording for
"each eye's sub-rect of the packed image"; the code is unambiguous.

**Déraciné uses the identical presentation model.** In
`DeracinePCVR/src/src/core/vr/openxr_host.cpp:2704` the swapchain is created with
`info.arraySize = 1`, and at `:2964-2965`:

```cpp
views[eye].subImage.imageRect.offset = {eye * eye_width, 0};
views[eye].subImage.imageRect.extent = {eye_width, ...};
```

Déraciné's presenter makes the side-by-side contract explicit and *detects* it rather than assuming
it — `DeracinePCVR/src/src/video_core/renderer_vulkan/vk_presenter.cpp:1218-1220`:

```cpp
const bool is_sbs = eye_textures[0].Address() == eye_textures[1].Address();
eye_width  = is_sbs ? (left_info.size.width / 2) : left_info.size.width;
eye_height = left_info.size.height;
```

**Conclusion for the matrix.** Axis 3 is aligned, not divergent: Wipeport, Farpoint ("correct packed
stereo submission and per-eye sampling") and Déraciné all present packed SBS with per-view
`imageRect`. Whatever "array-layer presentation" the brief expected, there is no such design in these
repositories to learn from, and therefore nothing on this axis to import.

### 2.4 Image ownership, zero-copy, and who synchronises

**Wipeport.** A four-deep pool of host-owned images ("one being drawn, one waiting to be shown, one
being copied, and one to spare", `constexpr u32 NumSlots = 4`). The guest renderer draws *directly*
into those images — `VrExporter::AcquireLocal()` maps `OpenXrHost::BeginFrame()`'s
`Target{index, image, view, width, height}` onto a `Frame`, with the comment "The image is the host's,
and may be another one than the last time". Then `Impl::CopyFrame()` performs one GPU-side copy or
blit into the runtime's swapchain image, bracketed by explicit layout transitions:

- source image: `eGeneral → eTransferSrcOptimal → back to eGeneral` (the frame is left in `General`
  by "whoever drew it");
- target: `eColorAttachmentOptimal → eTransferDstOptimal → eColorAttachmentOptimal`, because "the
  runtime hands its images over, and wants them back, as colour attachments";
- `copyImage` **only** when `swapchain_format == FrameFormat && width/height match`, otherwise
  `blitImage` with `vk::Filter::eLinear`.

The swapchain image is acquired with `xrAcquireSwapchainImage`, waited with
`xrWaitSwapchainImage(timeout = XR_INFINITE_DURATION)`, filled, submitted on
`graphics.queue.submit(submit_info, fences[buffer])`, and released with `xrReleaseSwapchainImage`.
Both `xrCreateSwapchain`/`xrEnumerateSwapchainImages` and the submit/release pair are taken under
`Vulkan::Scheduler::submit_mutex`, "because creating the images, and listing them, has some runtimes
prepare them on the queue".

**AstroQuest.** Owns two distinct models, and this is the clearest ownership split among the five:

- the local OpenXR host (host-owned images, as above);
- a Quest app that owns the images and hands them over a socket: `core/vr/vr_host_link.{h,cpp}`,
  `core/vr/vr_protocol.h`, `TakeBuffers()` / `SendFrame()`. Wipeport explicitly did *not* port this
  path — its `vk_vr_exporter.h` says "AstroQuest's other path, a Quest app owning the images over a
  socket, is not ported", and `vr_runtime.cpp` says "no SHADPS4_VR_SOCKET host (AstroQuest's Quest
  link is not ported)".

**Déraciné.** Keeps both paths and is the only one of the five with a *zero-copy* import path on the
host-link side: `DeracinePCVR/src/src/video_core/renderer_vulkan/vk_vr_exporter.cpp:32-48` documents
the slot state ("Zero copy: the image is the host's buffer" via `imported_memory`) versus "Otherwise
the image is ours, and its pixels travel through a buffer the CPU can read". `Setup()` is
all-or-nothing across slots. On the OpenXR side, `Finalize()` returns immediately for local frames —
"Drawn into the host's own image, which it reads on the same device" (`:364-368`) — and
`AcquireLocal()` hands out the OpenXR host's own image (`:92-113`).

**Who synchronises.** In all three, ownership of the *swapchain* images stays with the OpenXR runtime
(it hands them out on acquire and takes them back on release), and the app owns the intermediate
pool. Nobody here uses `VK_KHR_external_memory` for the OpenXR path; Wipeport enables the external
memory/semaphore/fence extension sets only because "what runtimes are known to want of a Vulkan
device they share images with".

### 2.5 Frame readiness and GPU synchronisation — the explicit wait

This is the axis where the brief asked for care, so it is worth separating three different things
that all look like "a wait".

**(a) The GPU-completion wait on the present path — it exists, in both Wipeport and Déraciné.**

`VrExporter::Deliver()` in Wipeport
`gpu/shadps4/video_core/renderer_vulkan/vk_vr_exporter.cpp`:

```cpp
void VrExporter::Deliver(Frame* frame, const Core::Vr::PresentedFrame& info) {
    // The host copies the image as soon as it hears about the frame.
    scheduler.Wait(frame->ready_tick);
    Core::Vr::OpenXrHost::Instance().EndFrame(static_cast<u32>(frame->host_buffer), info);
}
```

and the header's contract: "Tells the host the frame is ready. **Blocks until the GPU is done with
it.**"

Déraciné has the same wait, in the same function, on both paths
(`DeracinePCVR/src/src/video_core/renderer_vulkan/vk_vr_exporter.cpp:407-420`):

```cpp
if (IsLocal(frame)) {
    // The host copies the image as soon as it hears about the frame.
    scheduler.GetMasterSemaphore()->Wait(frame->ready_tick);
    Core::Vr::OpenXrHost::Instance().EndFrame(static_cast<u32>(frame->host_buffer), info,
                                              frame->ready_tick);
    return;
}
...
// The host samples the buffer as soon as it hears about the frame.
scheduler.GetMasterSemaphore()->Wait(frame->ready_tick);
```

Note the divergence: Déraciné also passes `ready_tick` *into* `EndFrame`, i.e. it keeps the tick
available to the host, where Wipeport drops it after waiting.

**What it buys.** Correctness, given the design: `EndFrame` only marks a slot `Ready` and notifies
the frame thread; the frame thread's `CopyFrame()` then reads that slot on the GPU. The wait is what
guarantees the copy cannot sample an image the renderer is still writing. The comment says it
plainly: the host copies as soon as it hears, so the CPU must not let it hear too early. Déraciné's
`OpenXrHost::EndFrame` also now takes the tick, which suggests the intent to *move* that dependency
into the host rather than block the caller.

**What it costs.** The calling (presenting) thread blocks on GPU completion before it can hand the
frame over, once per frame. At 120 Hz that is an 8.3 ms budget; a full stall per frame removes
CPU/GPU overlap for the remainder of the frame. Wipeport's own instrumentation measures exactly this
cost and logs it every ten seconds — "handing one over took {:.2f} ms, ending a picture {:.2f} ms
({:.1f} at worst)" — but **neither repository publishes a comparison against a non-waiting
variant**, so there is no evidence in these trees that the wait is *better* than an alternative; only
that it is necessary for the current copy-immediately design.

**(b) The swapchain waits.** `xrWaitSwapchainImage` with `XR_INFINITE_DURATION`, plus
`device.waitForFences(fences[buffer], true, 1'000'000'000)` (a 1 s timeout) before reusing a copy
command buffer. The infinite wait is a compositor-paced block and is unavoidable; the 1 s fence
timeout is a debugging guard that will silently allow reuse after a second.

**(c) A CPU deadline wait — AstroQuest's `late_frame_wait.h`, which is *not* a GPU wait.**
`shadps4-arm64-main/src/core/vr/late_frame_wait.h` is a condition-variable/event wait for the
*producer* to deliver a frame before the compositor deadline:

```cpp
// Budget begins at the runtime wakeup. The predicate is always checked first so a
// delivery between the initial TakeFrame and taking the mutex is not lost.
template <typename Ready>
LateFrameResult WaitForLateFrame(LateFrameSignal& delivered, ...);
```

On Windows it uses `CreateEventW` plus `Common::WindowsSleepTimer` specifically to avoid "the coarse
timeout of `SleepConditionVariableSRW`". It returns `Skipped`/`Ready`/`TimedOut` and skips entirely
when the last delivery is older than two refresh periods. This is frame *pacing*, not GPU
synchronisation, and conflating the two is the easiest way to import the wrong thing. AstroQuest also
carries `mirror_limiter.h` alongside it.

**One colour-contract hazard in Wipeport's copy.** The fallback path blits with
`vk::Filter::eLinear` whenever format or size differs. For Déraciné's stated contract — "the guest eye
picture is display-encoded bytes" — resampling *encoded* values is not a neutral operation: it
interpolates post-transfer-function codes and softens edge detail. Wipeport gets away with it because
it only triggers on a resolution/format mismatch; it is not a pattern to copy into a path whose
contract is byte-exact.

### 2.6 60/120 Hz timing and reprojection emulation

The PSVR1 model these ports reproduce: the panel scans at **120 Hz**; the title renders at **60 fps**
and its frames are shown for two refreshes each; the PSVR processing unit warps the last frame to the
current head pose on every scan-out ("reprojection"). None of the five emulate the warp — the OpenXR
compositor does it — so what is emulated is only the **guest-visible timing**, and the trigger for it
is the vblank/flip.

**Wipeport**, `gpu/shadps4/core/libraries/hmd/hmd_reprojection.cpp`, `OnVblank()`:

```cpp
// On real hardware a system thread warps the latest submitted frame to the current head pose
// and scans it out on every panel refresh. Here the host compositor does the warping, so this
// only has to keep the guest-visible timing alive and pass submitted frames along.
```

The `Reprojection` struct holds `flips`, `flip_arg`, `display_index[2]` and two user events, and
`OnVblank()` alternates the display buffer (`display_index[flips++ % 2]`), calls
`Libraries::VideoOut::SubmitHmdFlip(handle, index, flip_arg)` between the title's start and end
equeue events. `docs/VR.md`: "the display buffers are flipped on every headset refresh, and the
game's tracker thread waits for its number of flips per frame (2 on a PS4: 60 FPS …)".

The "number of flips per frame" is the pacing knob, and here it is **inside the guest binary**, so
Wipeport patches it: `docs/GAME_CODE.md` documents the `VRTrackerCntrTh` load at `0x658e14`
(`mov r14b, [rip+…]`, 2 = 60 fps) replaced with `mov r14b, 1` plus a 4-byte `nop` for `vr_fps=120`,
applied by `scripts/wipeout_patches.py` and compiled into `out/patches.bin`. It also notes "The
game's simulation follows its timer, so the game speed stays right at 120 FPS (checked)".

Emulated refresh rate: `Config::refresh_rate{120}` (`vr_runtime.h`), clamped to 60..120 in
`LoadFileConfig()`, and `Runtime::HeadsetRefreshRate()` prefers the host display's rate when the host
reports it (`NoteDisplayRefresh`, gated by `SHADPS4_VR_FOLLOW_DISPLAY`).

**Déraciné** has the same vblank entry point (`hmd_reprojection.cpp:148 OnVblank`) and the same
`refresh_rate{120}` default (`vr_runtime.h:130`), but its `core/vr/` does not carry AstroQuest's
`late_frame_wait.h` or `mirror_limiter.h`; it has `core/vr/frame_timeline.h` instead — a diagnostic
timeline (`SHADPS4_FRAME_TIMELINE=1`, "TIMELINE <ns> <event> <detail>") for "lining up the guest, the
GPU thread and the OpenXR host on a monotonic clock". That is an instrumentation choice, not a pacing
policy.

**Farpoint** is the interesting one because it changed the policy rather than the binary: its README
lists "Enable full-refresh Farpoint pacing instead of the default half-refresh wait", and warns "The
compositor still handles its own reprojection; fresh frames at headset refresh are not guaranteed in
every scene". So it moved from 2 flips/frame to 1 flip/frame in the emulator, and is honest that this
does not guarantee fresh frames.

### 2.7 Controller mapping, hand tracking, input semantics

**Wipeport** carries the complete AstroQuest OpenXR input machinery and then **turns it off**. The
`Impl` struct holds an `XrActionSet` with actions for `move`, `finger`, `cross`, `square`, `circle`,
`triangle`, `l1`, `r1`, `l2`, `r2`, `options`, `l3`, `finger_press`, `pose`, `grip`, `rumble`; hand
trackers `hand_trackers[2]` plus `xrLocateHandJointsEXT`; `use_grips`; `pad_source` (0 nothing,
1 hand joints, 2 grips); palm-distance sampling; and a full gyro-derived attitude estimator in
`Runtime::UpdatePadGyro()` (tilt gain 2.0, yaw gain 1.5, yaw lean from host reference within 500 ms).

But `OpenXrHost::Connect()` sets `impl->use_controllers = false;` with the comment "wocport: the
headset's controllers are not a gamepad here (see UpdateControllers)", and the `Impl` comment notes
"wocport: no Libraries::Pad here (the C runtime's pad, `src/runtime_pad.c`)". So Wipeport drives the
guest through its own runtime pad layer and keeps the OpenXR action set dormant. WipEout Omega
Collection's VR mode is a DualShock 4 title, so it does not need tracked hands.

**AstroQuest** is where the hand-tracking semantics actually run: controllers stand in for a gamepad
"where the PC has none, or has one that is not the one in use", hands placing a gamepad via grip
spaces ("a runtime that makes controllers up from the hands it sees (Virtual Desktop does) says where
the hands are, which is where the gamepad is"), and `Runtime::GetPad()` synthesising a controller
pose from head position plus a smoothed `pad_offset` when nothing tracks it — including a
`pad_follows_head` mode and a player-adjustable "own place" (`user/vr_controller.json`). Astro Bot
Rescue Mission is a DualShock 4 + camera title, which is why this fits.

**Farpoint** is the Aim Controller title, and its README says so: "Fix tracker initialization and
controller routing; tracked Aim and gamepad modes are available" and "The right controller supplies
the experimental Aim pose". Mapping: right A = Cross, right B = Square, left X/Y = Circle/Triangle,
left stick moves, right stick turns, triggers = L2/R2, grips = L1/R1, left menu = Options, both stick
clicks recenter.

**BloodTruth** is the two-Move title and documents a *corrected* hand order: "Both controllers provide
independent tracked poses and triggers. Trigger maps to Move T; grip to the Move button; lower face
button (A/X) to Cross; upper face button (B/Y) to Circle; stick click to Square; stick up to Triangle;
stick down to Select. Left menu maps to Start. Both stick clicks recenter." It keeps a
`-OriginalMoveOrder` switch "to restore the original order for comparisons" — direct evidence that
Move hand order is a per-title empirical question, not a property of the interface.

Déraciné's equivalent is `src/src/core/libraries/move/` (`libSceMove`) plus `swap_hands=1` in
`pc-vr/settings.txt`, which its `AGENTS.md` says must be kept because "the game's Move mapping depends
on it". Same class of problem as BloodTruth's flag, solved by a setting rather than a code branch.

### 2.8 What is per-title configuration versus a reusable service

This is the axis the brief calls key, and the five ports answer it consistently.

**Reusable across all five** (and already shared by inheritance): the `libSceHmd` and
`libSceVrTracker` ABI and structs; the reprojection lifecycle (`Start`/`SetDisplayBuffers`/
`SetUserEvent*`/vblank flip); `Core::Vr::Runtime` (seat/recenter/origin, `Fov`, `Config`, `DeviceState`,
`PresentedFrame`, pad feedback); `Core::Vr::OpenXrHost` (instance, system, session, swapchain, view
localisation, projection submission); the headset FOV cache; the env-var and `user/vr.json`
configuration surface.

**Per title, without exception:**

| Port | Per-title surface |
|---|---|
| Wipeport | `scripts/wipeout_patches.py` (eboot byte patches incl. the flip count), `gpu/games/wipeout.cpp` + `game_profile`, `wipeport.ini` (`vr_scale`, `vr_fps`, `vr_dynres`), `WOC_*` env vars, game memory pool size |
| AstroQuest | `pc-vr/settings.txt`, `pc-vr/launch.ps1`, title settings in `emulator_settings` |
| revive-psvr1-wipeout | `config/`, `games/`, launcher, CUSA05670 + update 01.07 package handling |
| Farpoint | `launch-farpoint.ps1`, `initialize-farpoint.ps1`, `initialize-metadata.ps1`, `FarpointProfiles.cs`, `config-default.json`, per-eye resolution ladder, +2–6 GiB guest direct memory, Aim-vs-gamepad routing, executable SHA-256 gate |
| BloodTruth | `bloodtruth-launcher/`, resolution options, `-OriginalMoveOrder`, exact CUSA11108 v1.00 gate |

The pattern: **the emulator-side service is general; everything that knows a title's name is a
launcher script, a profile table, a patch list, or a documented memory/pacing override.** Wipeport is
the outlier that pushes title knowledge into *guest binary patches*, which is exactly the thing a
maintained emulator fork should be most reluctant to copy.

---

## 3. What a shared PSVR runtime would own, and what stays per title

### 3.1 A shared runtime should own

1. **The guest ABI**: `libSceHmd` (including the `sceHmdInternal*` stubs and the recovered
   `OrbisHmdReprojectionParam`/`EyeUv`/`TrackerState` layouts), `libSceVrTracker` (memory sizes,
   device enums, `GpuSubmit`/`GpuWait`), `libSceHmdReprojection*`, `libSceHmdDistortion*`.
2. **The reprojection lifecycle as timing, not as rendering**: initialise, display buffers, user
   events, vblank flip, and the flip counter the title paces itself against.
3. **Pose space semantics**: seat/recenter, origin offset, tracker-space ↔ host-space conversion,
   `ToHostSpace` with the previous-seat disambiguation Wipeport implements in
   `Runtime::ToHostSpace()`.
4. **OpenXR session and presentation**: instance/system/session, swapchain creation policy
   (`arraySize = 1`, `TRANSFER_DST`), per-view `imageRect`, lens-FOV intersection, IPD application in
   view poses, layout transitions around the copy.
5. **Frame ownership and handoff**: the image pool, the acquire/end/drop protocol, and — crucially —
   *the sync contract between the renderer and the host*, whichever side owns the wait.
6. **Device abstraction**: head, one or two pads, `DeviceState` (position, orientation, linear and
   angular velocity, sequence, tracked), `PadFeedback` (rumble, light), and the "nothing tracks it,
   place it before the player" fallback.
7. **Configuration and observability**: `user/vr.json`, env vars, the headset FOV cache keyed by
   runtime+system+vendor, and periodic logs that separate delivered frames, shown frames, copy cost,
   end-frame cost, tracking coverage and pose correction (Wipeport's `Report()` is a good template).

### 3.2 What must stay per title

1. **Eye layout inference** — whether the two `OrbisHmdReprojectionEyeUv` describe halves of one
   image or two separate images, and whether the offsets are mirrored. Déraciné's `EyeLayoutOf()`
   already treats this as a per-title empirical inference with an explicit "ambiguous" outcome and a
   warning. Nothing about this generalises.
2. **Flip count / pacing policy** — 2 flips per frame versus 1 is a property of the title's loop.
3. **Which frustum to tell the title** — symmetric enclosing envelope, the headset's own asymmetric
   frusta, or a PSVR's nominal values. Wipeout needed symmetric; Déraciné needs the title's own UVs.
4. **Resolution ladder and guest memory budget** (Farpoint's +2–6 GiB, Wipeport's `game_pool_mb`).
5. **Controller identity and hand order** (Aim vs Move vs DS4; BloodTruth's hand order).
6. **Title binary patches and executable gates** — per-title by definition.
7. **Readback-ordering hazards** that a particular engine has (Farpoint's sky-brightness readback).

---

## 4. Recommendations for Déraciné's future PSVR work

Each is labelled by how safe it is to act on without new evidence.

### 4.1 Safe

**S1. Keep the asymmetric, UV-derived per-eye FOV as the default, and protect it with a test.**
Déraciné's `FovFromUv(left_eye, halves)` (`src/src/core/libraries/hmd/hmd_reprojection.cpp:80-90`)
and `EyeLayoutOf()` (`:61-78`) recover the frusta from the title's own reprojection UVs, and
`NoteEyeLayout()` (`:104-139`) refuses to guess when the offsets say neither reading. Wipeport's
symmetric envelope exists because *WipEout* mishandled an asymmetric host frustum; that is evidence
about WipEout, not about Déraciné. Reason: this is the stated preserved behaviour, and no repository
in this set shows a case where the title's own UVs were wrong and a host-side envelope was right.

**S2. Do not move to array-layer swapchains.** `arraySize = 1` with per-view `imageRect` is what
Wipeport (`EnsureSwapchain`) and Déraciné (`openxr_host.cpp:2704`, `:2964-2965`) both do, and it is
what the zero-copy presenter already feeds. Reason: there is no working array-layer design in these
repositories to copy, and changing the presentation shape would touch the one path the brief says
must not break.

**S3. Adopt Wipeport's lens-FOV intersection *as an available pathology fix*, keeping it off by
default.** The mechanism (`FillProjection()`: intersect `shown.fov` with `xrLocateViews`'s
`lens_fov`, then shrink `imageRect`) is exact cropping of a planar projection. Reason: it is additive,
it cannot change what the title draws, and it gives a documented answer for a headset whose lens FOV
is narrower than the envelope the title drew into. Keeping it behind the existing
`SHADPS4_VR_FOV_SYMMETRIC`-style switch keeps the preserved path untouched.

**S4. Adopt Wipeport's `Report()` instrumentation shape.** It logs, per ten seconds: frames delivered
vs shown, longest wait for a frame and count of >50 ms waits, copy cost, end-frame cost (average and
worst), head-tracked percentage, headset FOV, and "pictures were shown X degrees from where they were
drawn for". Reason: it is diagnostic only, and Déraciné already has the matching vocabulary
(`PresentedFrame`, `frame_timeline.h`) — this makes the copy/wait costs measurable *before* anyone
acts on §4.2.

**S5. Keep the display-encoded colour contract explicit and tested.** Déraciné's host-link exporter
already restricts itself to a channel swap (`swap_red_blue = format == eB8G8R8A8Unorm ||
eB8G8R8A8Srgb`, `vk_vr_exporter.cpp:141`) rather than a conversion. Reason: it is the stated contract,
and it is the property most easily broken by borrowing a copy path from elsewhere (see D3).

### 4.2 Needs measurement

**M1. Symmetric enclosing FOV as a real A/B, not a new default.** Déraciné *already contains* both
switches, defaulted off: `fov_from_headset{false}` and `fov_symmetric{false}`
(`src/src/core/vr/vr_runtime.h:120,123`), with the implementation at
`src/src/core/vr/vr_runtime.cpp:820-875`. Wipeport defaults both **on** (`fov_from_headset{true}`,
`fov_symmetric{true}`). Reason it needs measurement: turning them on changes what the title is told
→ changes what it draws → changes both the pixel budget (a symmetric envelope wastes area on the
narrow side) and the submitted frusta. Their Quest 2 evidence ("the right eye lost most of its
picture") is a strong hypothesis for Déraciné's asymmetric headset cases, but it is WipEout's
evidence. Measure: binocular coverage per eye, angular resolution (pixels per degree), and whether
`EyeLayoutOf()`'s `halves` reading still holds when the title is fed a symmetric frustum.

**M2. Whether the present-path GPU wait can move off the CPU.** Both Wipeport and Déraciné block the
presenting thread on `ready_tick` before handing the frame over
(`vk_vr_exporter.cpp:407-420`). Déraciné *already* passes `ready_tick` into `EndFrame` — the
scaffolding to let the host (or a timeline semaphore) own the dependency is present. Reason it needs
measurement: the wait exists because the host copies immediately on notification, so removing it
without moving the dependency is a use-after-write; and **no repository here publishes evidence that
the blocking form is faster or slower than the deferred form**. Measure: `Deliver()` stall time (S4
gives it to you), frame-to-photon latency, and dropped-frame count at 120 Hz, before and after.

**M3. Full-refresh (one flip per frame) pacing.** Farpoint moved to "full-refresh pacing instead of
the default half-refresh wait" and explicitly warns that fresh frames at headset refresh are "not
guaranteed in every scene". Reason it needs measurement: the flip count is guest-visible pacing; a
title whose simulation follows its timer (Wipeport verified WipEout's speed stays right at 120 FPS,
with a binary patch) may behave differently from one that assumes two refreshes per frame. Déraciné
must not change a guest-visible timing constant without checking the title's loop and reprojection
assumptions.

**M4. Per-eye resolution and IPD overrides as first-class settings.** Wipeport's `vr_scale`
(960×1080 × scale, default 1.4 = 1344×1512) and Farpoint's ladder up to 3072×3456 per eye, each with
a guest-memory budget, are the same knob Déraciné would need. Reason it needs measurement: it trades
resolution against the title's dynamic-resolution behaviour and guest memory; Wipeport needed
`game_pool_mb` raised (and 896 MiB more direct memory reported) before larger eyes stopped crashing
on heavy tracks.

### 4.3 Do not import

**D1. Do not import Wipeport's binary-patch pacing.** `scripts/wipeout_patches.py` rewrites guest
immediates (the `VRTrackerCntrTh` flip count at `0x658e14`) and refuses to start when the original
bytes do not match. Reason: it is the most brittle possible expression of a per-title timing change,
it is tied to one eboot revision (CUSA05670 update 1.07), and it directly contradicts
`DeracinePCVR/src/AGENTS.md`'s rule that title-specific work must be justified as a last resort
beside a general fix. If Déraciné needs different pacing, express it in the emulator's timing
(§M3), not in the guest image.

**D2. Do not import Wipeport's decision to bypass OpenXR controllers as a gamepad.**
`OpenXrHost::Connect()` hard-sets `impl->use_controllers = false` and routes input through its own
`src/runtime_pad.c`. Reason: Déraciné's product requirement is *PlayStation Move emulation*, i.e. the
opposite direction — tracked device poses and Move semantics going *into* the guest. Adopting the
bypass would fight the Move model and discard the hand/grip/pose machinery that the shared runtime
already has.

**D3. Do not import Wipeport's `blitImage(..., eLinear)` copy fallback into the eye path.** Reason:
Déraciné's contract is that the guest eye picture is display-encoded bytes; a linear blit resamples
post-transfer-function values and softens edges. Wipeport only takes that branch on a format/size
mismatch, which is a different premise. If Déraciné ever needs a resize, do it with an explicit,
colour-aware step, or keep the exact `copyImage`.

**D4. Do not import AstroQuest's `late_frame_wait.h` / `mirror_limiter.h` as a synchronisation
fix.** Reason: they are CPU deadline waits for a producer (a condition variable / `CreateEventW`
plus a high-resolution timer), not GPU fences. Installing them "to make the present path wait" would
add latency without ordering anything against the GPU. Déraciné already dropped them in favour of
`frame_timeline.h`; that is the right direction.

**D5. Do not import Farpoint's "readback before signaling GPU completion" as a general present-path
rule.** Its README describes completing "the game's sky-brightness readback before signaling GPU
completion" to fix excessively bright terrain — a title-specific readback-ordering hazard. Reason:
it is a corrective for one engine's readback dependency, not evidence that a global present-path wait
improves anything. Treat it as a debugging hypothesis if Déraciné ever shows a similar
brightness/latency artifact.

**D6. Do not adopt a per-eye-frustum policy from any of them without Déraciné's own layout evidence.**
Wipeport's envelope and a PSVR-nominal fallback are both defensible for *their* titles; Déraciné's
authority is the title's reprojection UVs. Reason: this is precisely the "already-working VR
behaviour that MUST be preserved".

---

## 5. Claims I could not verify from source

Listed so that nothing above is mistaken for checked fact.

1. **AstroQuest's own FOV defaults.** `fov_from_headset` / `fov_symmetric` appear in the shared
   `Core::Vr::Runtime` that AstroQuest authored, and Wipeport annotates its deviations with
   `wocport:` comments, but I did not read AstroQuest's `vr_runtime.h` line by line to confirm its
   default values or whether its `TitleFov()` matches Wipeport's.
2. **AstroQuest's OpenXR presentation shape.** `openxr_host.cpp` there is 135,980 bytes and I only
   read its tree entry, the `late_frame_wait.h` file, and the neighbouring file list. Whether its
   swapchain is `arraySize = 1` with `imageRect` (as Wipeport and Déraciné) or array layers, I did
   **not** verify.
3. **AstroQuest's `Deliver()`/`VrExporter` GPU wait.** I did not read its `vk_vr_exporter.*`. The wait
   is assumed to be inherited from the same ancestor; unverified.
4. **AstroQuest's Quest-side ownership path in detail.** I know `vr_host_link.{h,cpp}` and
   `vr_protocol.h` exist and that Wipeport describes it as "a Quest app owning the images over a
   socket". I did not read the protocol, the dmabuf handshake, or who synchronises on that path.
5. **All of revive-psvr1-wipeout's source.** I read its `README.md` and `docs/PROVENANCE.md` only.
   Every "?" cell in that row of the matrix is genuinely unchecked. In particular I did **not**
   verify: whether its swapchain is packed or arrayed, how it converts frusta, whether it keeps the
   GPU wait, how it paces 60→120, or whether it re-implements `libSceHmd` itself versus importing it.
   Note also that despite the "revive" name I found **no evidence of PSVR1 hardware support** (no
   USB/HID transport) — its own README describes OpenXR output. That is a documentation-level
   observation, not a source-level disproof.
6. **All of Farpoint's and BloodTruth's VR source.** Their root trees reveal `shadps4-arm64-main/`
   (Farpoint) and `src/` (BloodTruth), but I audited their READMEs, not their C++. So: "correct packed
   stereo submission", "tracked Aim", "full-refresh pacing", "readback before signaling GPU
   completion", and the Move mappings are **author claims I did not verify at source**. Farpoint's
   `shadps4-arm64-main/src/core/vr/` exists but its contents were not enumerated (a `contents` fetch
   of `src/core/vr` at the repo root 404'd because the tree lives under `shadps4-arm64-main/`).
7. **Whether any of these ports clicks with a PSVR1 headset directly.** None of the ones I read show
   PSVR USB/HID code; all route to OpenXR. I did not exhaustively grep for `libusb`/HID across five
   trees, so this is a strong impression from the files read, not a proven negative.
8. **Any performance number.** Wipeport's `docs/VR.md` reports "about 117 FPS … on an RX 6800 XT,
   1920×2160 per eye at 120 FPS without dynamic resolution … measured without a headset"; Farpoint
   reports "approximately 89.6–90 game submissions per second on a 90 Hz headset" at 3072×3456. Both
   are author-reported single-scene numbers, not measurements I made or can check.
9. **Whether the explicit present-path GPU wait helps or hurts on Déraciné specifically.** No
   repository here contains a controlled comparison. Wipeport only *measures* the cost (its `Report()`
   logs "handing one over took … ms"). This is why §M2 asks for measurement rather than a change.
10. **Line numbers inside the Wipeport sources.** The raw files were fetched whole and the surrounding
    text was truncated/spilled by the fetch tool, so I cite **function names and quoted code** for
    Wipeport rather than line numbers. The Déraciné line numbers in §4 and §2 are from the local
    files and are exact.

---

## 6. Bottom line

Three things transfer, and they all transfer as *mechanisms with switches*, not as defaults:
**(a)** ask the compositor what the lenses show and intersect the drawn frustum with it, cropping
`imageRect` exactly (Wipeport `FillProjection()`); **(b)** treat the OpenXR layer as one packed
side-by-side image with per-view `imageRect` and never as array layers; **(c)** keep the per-title
knowledge in launchers, profiles and documented overrides, and keep the runtime itself title-blind.

Three things must not cross the boundary: binary patches into the guest for pacing, a gamepad bypass
that replaces tracked controllers, and any "wait" borrowed from another codebase without knowing
whether it is a GPU fence or a CPU deadline.

The single most useful next step is not a code change. It is instrumenting Déraciné's present path
the way Wipeport's `Report()` does — delivered vs shown frames, copy cost, end-frame cost, tracking
coverage, pose correction — so that the two questions that actually matter (§M1 symmetric envelope,
§M2 where the GPU wait lives) can be answered with numbers from this title rather than from Wipeout's.
