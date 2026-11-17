# Review — `6170df6`, recycle Vulkan images by creation parameters

Adversarial review of `6170df6` (`perf(video_core): recycle Vulkan images released with the same
creation parameters`) on `investigation/cross-fork-renderer`, worktree
`M:\Development\Deracine-work\wt-crossfork`, plus its neighbours `49ec266` (diagnostics) and
`fcdf3de` (`CoveringCopyOf`).

Method: read-only. `git show` of the three commits, then the code the recycler sits in — the image
destructor/creator (`texture_cache/image.{h,cpp}`), the texture-cache lifetime
(`texture_cache/texture_cache.cpp`), the scheduler's deferred-operation gate
(`renderer_vulkan/vk_scheduler.{h,cpp}`, `vk_master_semaphore.{h,cpp}`), the ownership chain
(`vk_presenter.h`, `vk_instance.cpp`, `emulator.cpp`, `gnmdriver.cpp`) and every other
`UniqueImage`/`vmaCreateImage` site in the tree. **No build, no emulator run, no GPU.** Everything
below is either quoted code or explicitly marked as unproven.

The question I asked of every line was: *what would have to be true for this to be wrong, and does
the code prove that it is not?* Two of the ten checklist items end in "wrong but inert" rather than
"PASS", and one ends in "unproven" rather than "wrong".

---

## 1. Findings

Severity: **blocker** = must not integrate as is; **major** = integrate only with a named condition
or an explicit, recorded risk acceptance; **minor** = fix in the same branch when convenient.

| id | sev. | what | evidence | suggested fix |
|---|---|---|---|---|
| R1 | major | `FsrPass::CreateImages` destroys `UniqueImage`s with `Destroy()`, which releases straight into the pool with **no tick gate**. The commit's stated invariant ("release happens in the image destructor, and the texture cache erases images from `scheduler.DeferOperation`") is not a property of `UniqueImage`; the FSR pass is a second destruction path that is not deferred. | `host_passes/fsr_pass.cpp:450-453` (`intermediary_image_view.reset(); output_image_view.reset(); intermediary_image.Destroy(); output_image.Destroy();`), called from `Build`-time resize (`fsr_pass.cpp:158-159` → `ResizeAndInvalidate` → `Render` → `if (img.dirty) CreateImages(img);`), reaching `image.cpp:114-129` → `vk_image_recycler.cpp:81-136`. The new image created at `fsr_pass.cpp:470` has exactly the create info of the one just released (`flags`=0, `view_formats` empty, same extent/format/usage), so the reuse is **guaranteed** — the image being handed out is the one whose last submission may still be in flight. | Defer the FSR release (`scheduler.DeferOperation([img]{ … })`) or add `UniqueImage::Retire(scheduler)` and use it here; alternatively make `Release` refuse an entry that was not produced by the deferred path. |
| R2 | major | No fallback when `vmaCreateImage` fails, and the pool makes failure more likely: the VMA create info carries `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT`, so an allocation that does not fit the current heap budget returns `VK_ERROR_OUT_OF_DEVICE_MEMORY`, and `ASSERT_MSG` then runs `Emulator::Shutdown()` + `int $3`. Between 0 and 768 MiB of image memory is held out of circulation by the pool. | `image.cpp:136-143` (alloc info), `image.cpp:180-181` (`ASSERT_MSG(result == VK_SUCCESS …)`), `common/assert.cpp:16-22` (`assert_fail_impl` → `Crash()`), bound at `vk_image_recycler.h:97-98` (`MaxImages = 16`, `MaxBytes = 768_MB`). | On failure: drain the pool (`ImageRecycler::Drain()`), retry once, and only then assert. Log a single warning with the pool state. Same for the presenter's own `vmaCreateImage` if it is to keep asserting. |
| R3 | major (unproven) | Recycling does not make old contents *valid*, but it changes the failure mode from "whatever the driver hands a fresh allocation" (on WDDM usually zeroed pages) to "the previous owner's pixels". Render passes load with `eLoad` whenever the guest does not clear, so any place where the tracker's model of an image's contents is wrong stops rendering as black/zeroes and starts rendering as the previous frame's image. No pixel-level A/B exists; the commit measured frame rate only. | `vk_scheduler.cpp:776-795` (`loadOp = cb.is_clear ? eClear : eLoad`, same for depth/stencil), `pp_pass.cpp:441`; first barrier of a fresh backing is from `eUndefined` (`image.h:198-202` `State{… layout = eUndefined}`, `image.cpp:585-602`), which legally discards. | Record one A/B pixel comparison per scene (recycler on vs off, identical guest frame): dump the presented frame in both runs and diff. If the images differ by more than noise on any scene, the tracker has an untracked read and the recycler is the instrument that found it. |
| R4 | minor | Statistics do not describe what the code does. `images_destroyed` misses two of the four destroy sites — the same-shape replacement and the cheapest-eviction both call `vmaDestroyImage` directly and never `NoteImageDestroyed` — so with the recycler on, `images_created` and `images_destroyed` differ by something that is not the pool size, which reads as a leak. `recycler_destroys` (cumulative) is printed among per-second rates. The `pool_images`/`pool_mb` gauge is only refreshed on a create-hit and in the destructor, never from `Destroy()` or `Shutdown()`. `recycler_misses` counts *attempts*, including shapes the pool can never hold. | `vk_image_recycler.cpp:102` and `:125` (bare `vmaDestroyImage`), vs `:138-145` (`DestroyReleased`, the only one that counts); `image_stats.h:157-172` (mix of `exchange(0)` and `.load()`); gauge set only at `image.cpp:109-110` and `168-169`. | Count in exactly one place (both bare sites go through `DestroyReleased`), split the line's per-second and cumulative fields, and update the gauge in `Release`/`Shutdown` as well. |
| R5 | minor | After `Shutdown()`, `Release` still calls `vmaDestroyImage` with the entry's allocator (the `!Enabled() \|\| shutdown` branch goes to `DestroyReleased`), i.e. the pool must be the last thing destroyed but its own "shut down" state does not make a late release safe. `shutdown` is never cleared, so a second `Instance` in the same process gets a recycler that silently pools nothing. | `vk_image_recycler.cpp:88-92`, `:138-145`; `shutdown` set at `:149` and only read; `Enabled()` is a one-shot `getenv` (`:12-19`), so nothing re-arms. | `Shutdown()` should make `Release` *drop* the entry (log once, do not touch the allocator), and `Instance` construction (or `Create`) should re-arm the pool for the new allocator. |
| R6 | minor | The teardown ordering the commit adds is correct but **never executed**: this build leaves through `std::quick_exit`, which does not destroy the `presenter` global, so `~Presenter` → `~Instance` → `ImageRecycler::Shutdown()` + `vmaDestroyAllocator` do not run. The invariant is correct by declaration order, not by a run. | `emulator.cpp:587` (and `326`, `543`, `690`) `std::quick_exit(0)`; `core/libraries/gnmdriver/gnmdriver.cpp:35` (`std::unique_ptr<Vulkan::Presenter> presenter;` never reset); `vk_instance.cpp:213-218`. | Keep the ordering (it is right), but say in the commit that it is unexercised; if the exit path is ever made clean, the sticky-`shutdown` hole (R5) becomes live at that moment. |
| R7 | minor | `Acquire` does not check that the entry belongs to this instance's device/allocator; `Create` overwrites `allocator` from the entry. The class comment justifies a process-wide pool with "there is one Vulkan device" — true today (one `vmaCreateAllocator`, `vk_instance.cpp:833`), but the invariant is not enforced where it is relied on. | `image.cpp:162-171` (`allocator = recycled.allocator;`), `vk_image_recycler.h:55-56` ("A process-wide pool is enough: there is one Vulkan device"). | Store the device/allocator in the pool at `Shutdown`/`Arm` time and refuse entries that do not match; at minimum assert in `Acquire`. |
| R8 | minor | `pNext` is excluded from the key by design, and its exclusion is safe only because no caller passes one — an assumption, not a check. `UniqueImage::Create` stores the caller's `image_ci` verbatim (including `pNext`), and `SameShape` ignores it; the first caller to pass `VkImageFormatListCreateInfo`, `VkImageCompressionControlEXT`, a DRM-modifier list or an external-memory chain would silently share images with a caller that passed nothing. | `vk_image_recycler.cpp:26-40` (the comment and the field list), `image.cpp:133` (`this->image_ci = image_ci;`), the five live call sites (`image.cpp:354`, `:381`, `:1332`, `fsr_pass.cpp:470`, `:476`) all build an aggregate with `pNext == nullptr`; the only chain in the tree is on a raw `device.createImage` (`vk_vr_exporter.cpp:185-202`). | `ASSERT(image_ci.pNext == nullptr)` in `Create`, or store `pNext == nullptr` in the key. Either way the assumption becomes a check. |
| R9 | minor | `SameShape` dereferences the **pooled** entry's `pQueueFamilyIndices` when the count matches. Nothing is pooled with a non-zero count (`Release` refuses them), so the dereference is unreachable — but only because of the order in which two branches of `Release`/`Acquire` happen to be written today. | `vk_image_recycler.cpp:43-49` (the loop), `:109-113` (the refusal, which is *after* the same-shape search at `:96-98`). | Return false (or assert) whenever either count is non-zero and delete the loop; the comparison that matters is `count != 0`. |
| R10 | minor | `UniqueImage`'s move **assignment** overwrites `image`/`allocation` without destroying (or pooling) what was there — a leak of a whole image plus its allocation. Not reachable today: nothing move-assigns over a live `UniqueImage` (every assignment target is freshly default-constructed), `std::deque<BackingImage>` is never `erase`d/`insert`ed into, and `SlotVector::Reserve` move-*constructs*. | `image.h:59-67` (no `Destroy()`/`Release` of the current image), targets at `image.cpp:353`, `:380`, `:1331`, `fsr_pass.cpp:139-140`; `slot_vector.h:230-256` (move-construct + destroy old). | `Destroy()` (i.e. release to the pool) at the top of `operator=`; the pattern already exists in `Destroy()`. |
| R11 | minor | One pooled slot per shape: a second release of the same shape *destroys* the pooled one instead of keeping both, so a double-buffered pair of one shape can only ever produce one hit per frame. No automated test covers the recycler (the only new test target is `shadps4_image_coverage_test`). | `vk_image_recycler.cpp:94-106` (the collision branch), `src/tests/CMakeLists.txt:1098-1121`; `src/AGENTS.md` "Add automated tests for behaviour you change". | Allow 2–4 per shape (a per-shape count in `Entry`/key), and extract `SameShape` + the bound arithmetic so they can be tested without a device. |
| R12 | minor | `MaxBytes = 768_MB` is a constant, not a fraction of the device budget, and pooled memory is **visible** to the texture cache's GC pressure signal (`GetDeviceMemoryUsage` reads the Vulkan heap budget, which includes VMA allocations that are still alive in the pool). On the measured machine that is 250 MiB against a ~4.4 GiB pressure threshold, so it is small — but "hold up to 768 MiB out of circulation" is a policy that should be derived from the budget it is spent against. | `vk_image_recycler.h:97-98`; `texture_cache.cpp:1705-1727` (`total_used_memory = instance.GetDeviceMemoryUsage()`), `vk_instance.cpp:986-1001`; thresholds `texture_cache.cpp:215-226`. | `MaxBytes = min(768 MiB, budget / 8)`, and drain the pool when the GC enters its critical band instead of letting the pool be part of the reason it entered it. |
| R13 | minor | `Enabled()` treats any value that does not start with `'0'` as "on", so `SHADPS4_DBG_IMAGE_RECYCLER=off` / `=false` / `=no` turn the recycler **on**. The header documents only "unset means on, `=0` turns it off", which reads as if other values were not meaningful; the sibling knobs in the same tree (`SHADPS4_IMAGE_FORMATS`, `SHADPS4_DBG_SAMPLES`, …) test for `'1'`, so this one is the odd spelling out. | `vk_image_recycler.cpp:12-19` (`value[0] != '0'`), vs `image.cpp:340-343`, `:633-636`, `:999-1000` (`value[0] == '1'`). | Parse both spellings explicitly, or default off and require `=1`. |
| R14 | info | `vk_runtime.cpp:640` is a sixth `UniqueImage::Create` caller that is **not compiled** (`vk_runtime.cpp` is not in `src/CMakeLists.txt`'s source list; only `vk_presenter.cpp` is, at `src/CMakeLists.txt:1222`). It would be safe if built (it copies `image_ci`, no `pNext`, no view formats), but it is dead source that will not be fixed by anything here. | `src/CMakeLists.txt` source list; `vk_runtime.cpp:621-654`. | Note it when the Bachata runtime is either revived or deleted. |

No finding here is "recycling hands out an image with a stale layout, stale views, or stale state"
— that specific class is closed, see item 2 below.

---

## 2. Per-checklist reasoning

### 2.1 Key completeness — **PASS, with R8 and R9**

`vk::ImageCreateInfo` has exactly these members: `sType`, `pNext`, `flags`, `imageType`, `format`,
`extent`, `mipLevels`, `arrayLayers`, `samples`, `tiling`, `usage`, `sharingMode`,
`queueFamilyIndexCount`, `pQueueFamilyIndices`, `initialLayout` (`vulkan.hpp` for this SDK). The key
(`vk_image_recycler.cpp:26-51`) compares every scalar field of the struct except `pNext` and `sType`
(`sType` is a constant of the type; `pNext` is R8), compares the queue-family *values* as well as the
count when the count is non-zero, and adds the view-format list, which is the only other thing
`UniqueImage::Create` puts into the actual create call (`image.cpp:145-154`:
`VkImageFormatListCreateInfo` chained in when `view_formats` is non-empty). That is a complete key for
what this tree creates.

Field by field, with the call sites:

- **`flags`**, **`imageType`**, **`format`**, **`extent`**, **`mipLevels`**, **`arrayLayers`**,
  **`samples`**, **`tiling`**, **`usage`**, **`initialLayout`** — compared at
  `vk_image_recycler.cpp:33-40`. All of them are live: `flags` gets `eMutableFormat | eExtendedUsage`
  plus per-property bits (`image.cpp:264-274`), `usage` is mutated by `SetHostSamples` when a storage
  image cannot be multisampled (`image.cpp:1275-1291`, `image.cpp:1298-1299`), `samples` by the same
  function, and `format`/`extent`/levels are per-image. Two images can legitimately differ in any of
  them for the same guest texture, so all of these being keyed is load-bearing, not defensive.
- **`sharingMode` / `queueFamilyIndexCount` / `pQueueFamilyIndices`** — `vk::ImageCreateInfo`'s
  aggregate default is `eExclusive`/0/`nullptr` and no caller sets them: the only
  `queueFamilyIndexCount` in the tree is the swapchain's own create (`vk_swapchain.cpp:89-90`), which
  is not a `UniqueImage`. The comparison of the counts is fine; the *value* comparison
  (`vk_image_recycler.cpp:46-49`) would dereference the stored pointer, which is why non-zero-count
  images are never pooled (`:109-113`). The pointer cannot dangle **as long as** nothing with a
  non-zero count is ever pooled — an invariant that is currently enforced in `Release`, after the
  same-shape search, and nowhere else. That is R9.
- **`pNext`** — excluded, with a comment that says why: "the only pNext an image is created with is a
  format list built on the stack for the create call, and the list itself is compared above"
  (`vk_image_recycler.cpp:30-32`). That statement is true of every live call site — I checked all
  five: `image.cpp:318-333` (aggregate, no chain), `image.cpp:376`/`:1298` (copy of an existing
  `image_ci`, which is itself chain-free), `fsr_pass.cpp:455-469`/`:473-475` (aggregate, no chain).
  A grep for `setPNext`/`pNext` around image creation finds only the exporter's
  `vk::ExternalMemoryImageCreateInfo` chain on a raw `device.createImage`
  (`vk_vr_exporter.cpp:185-202`), which never goes through `UniqueImage`. So no two create calls in
  this tree differ in a field the key ignores — but the reason is a *convention* ("nobody chains
  anything"), and the pool stores the caller's `pNext` pointer (`image.cpp:133`) for later
  comparisons that ignore it. That is R8: assert the convention.
- **`view_formats`** — in the key (`:27-29`) and stored per entry. This is the field that makes
  `EnsureViewFormat`'s re-creation (`image.cpp:365-445`) safe: it creates a new backing with *no*
  view-format list, and can therefore only be given an image that was itself created without one. If
  the list were not in the key, a recycled image created with `{unorm, srgb}` could be handed to an
  image that then views it in a third format, which the driver would be entitled to reject (and
  which the `eMutableFormat` flag makes a real constraint, not a hint).
- **Anything else** that decides what the driver object is: none. The allocation parameters are
  uniform (`image.cpp:136-143`: same `VmaAllocationCreateInfo` for every unique image), so two
  images with equal create info cannot differ in memory type policy.

Verdict: the key is as complete as the type allows. Two assumptions are unproven by construction and
should become assertions (R8, R9).

### 2.2 Invalid state carried over — **PASS**

A recycled image never lands in an existing `BackingImage`; it always goes into a *new* one whose
state is default-initialised, and `UniqueImage::Create` overwrites its create info and format list
before it returns (`image.cpp:133-134` runs before the hit branch at `:160-173`).

- **Layout.** `BackingImage::State` defaults to `{eAllCommands, eNone, eUndefined}`
  (`image.h:198-202`), and `BackingImage` is aggregate-initialised on `emplace_back`
  (`image.cpp:336-337`, `:378-379`, `:1330-1332`). The first `GetBarriers` for the new image
  therefore emits `oldLayout = eUndefined` (`image.cpp:585-602`, or `:536-553` for the partial case),
  which per the Vulkan spec discards the previous contents. Nothing reads the recycled layout.
- **Subresource states / settled requests / state version.** `subresource_states` is empty,
  `settled` is zeroed and `state_version` starts at 1 (`image.h:206-225`), so the settled-request
  fast path (`image.cpp:488-500`) cannot match a request against a *previous* image's transition: the
  version counter starts over with the new backing.
- **Image views.** Views of texture-cache images are only created through `Image::FindView`, which
  records every id in `backing->image_view_ids` (`image.cpp:462-466`), and `DeleteImage` erases all
  of them *before* erasing the image, inside the same deferred callback
  (`texture_cache.cpp:1818-1826`). So by the time an image can enter the pool, no view of it exists —
  and `FindView`'s "last view" cache (`image.h:209-211`) lives in the backing that is dying with it.
  The other view-creators in the tree (`blit_helper.cpp:64`, `:381`, `:493`; `vk_presenter.cpp:896`;
  `vk_foveation.cpp:138`) either build views of images they own or destroy their temporary views in a
  `DeferOperation` that is queued before the image's own (`blit_helper.cpp:86`, `:223`, `:403`,
  `:514`), and the scheduler drains its queue FIFO (`vk_scheduler.cpp:889-902`).
- **Descriptor sets.** They are written per draw and never cached against an image handle; the
  `Scheduler`'s only handle-keyed state is `IsAttachmentImage`, which reads the *current*
  `RenderState` (`vk_scheduler.h:538-554`), and no release can happen inside a pass —
  `PopPendingOperations` runs from `SubmitExecution` (`vk_scheduler.cpp:1033`), i.e. after
  `EndRendering()` and `current_cmdbuf.end()` (`:963-966`), and the next recording begins with
  `is_rendering == false`. So a recycled handle cannot collide with the pass being recorded.
- **`Image`-level state** (`flags`, `mip_hashes`, `hash`, `track_addr`, `lru_id`, …) belongs to the
  new `Image`, constructed fresh in `slot_images.insert(...)` (`texture_cache.cpp:958-959`). Note
  `mip_hashes.resize(levels)` in the constructor (`image.cpp:261`) zeroes the hashes, so the
  GPU-modified skip at `texture_cache.cpp:1472-1479` cannot believe a recycled image's pixels are
  already in guest memory.

The only thing carried across is what the driver object itself remembers internally (memory layout,
compression metadata, the last bound allocation). None of that is observable as contents — the
allocation is the same allocation, and the contents are exactly what the previous owner left, which
is item 3.

### 2.3 Does recycling ever imply old contents are valid — **PASS on the tracker, RISK on the failure mode (R3)**

The new owner's contents model is byte-for-byte the model a brand-new driver image has:

- `Image::flags = ImageFlagBits::Dirty` (`image.h:189`), so the first use goes through
  `RefreshImage`, which uploads guest memory unless the image is multisampled
  (`texture_cache.cpp:1414-1418`, `:1443-1505`) or the caller proved a copy will overwrite the level
  (`fcdf3de`, see §3).
- `ExpandImage` uploads, then copies the old image over (`texture_cache.cpp:1039-1048`), then clears
  `Dirty` (`:1057`) — the copy is of a *known* source, not of the recycled pixels.
- `Clear` writes (`image.cpp:1259-1273`), `Upload` writes (`:650-703`), `CopyImage` writes into the
  destination with a `TransferDstOptimal` transition that comes from `eUndefined`
  (`image.cpp:985`, `:536-553`), `Resolve` writes (`:1166-1257`), `SetBackingSamples`' new backing
  transitions from `eUndefined` with an explicit "not caring of previous contents" comment
  (`:1354-1375`).
- Every consumer of the contents either (a) uploaded them, (b) wrote them, or (c) is a render pass
  load — and (c) is the one honest exception, see below.

So: **no consumer ever reads recycled contents as if they were validated data.** That is the claim
the commit makes and it holds.

The residual risk is narrower but real: `loadOp = eLoad` when the guest does not clear
(`vk_scheduler.cpp:776-795`, `pp_pass.cpp:441`). A render target that the tracker believes is valid
will load whatever the allocation holds; with a fresh allocation that is typically zeroed pages from
WDDM, with a recycled allocation it is the previous image's pixels. If the tracker is right, both are
the same data and nothing changes. If the tracker is ever wrong, the bug changes character: black
smear becomes a coherent previous frame. That is not a defect the recycler introduces, but it is a
defect the recycler will *hide better*, and no measurement in the commit would catch it. Hence
"unproven" rather than "wrong" — and one pixel A/B would close it.

### 2.4 GPU completion before reuse — **PASS for the texture cache, FAIL for the FSR pass (R1)**

The texture-cache path, in full:

1. `TextureCache::DeleteImage` does not destroy anything synchronously; it queues the work:
   `scheduler.DeferOperation([this, image_id]{ … slot_images.erase(image_id); })`
   (`texture_cache.cpp:1818-1826`).
2. `DeferOperation` stamps the current tick: `pending_ops.emplace(std::move(func), CurrentTick())`
   (`vk_scheduler.h:623-625`).
3. `PopPendingOperations` only pops while the GPU is known to have reached that tick:
   `if (pending_ops.empty() || !master_semaphore.IsFree(pending_ops.front().gpu_tick)) break;`
   (`vk_scheduler.cpp:889-903`), with `IsFree(tick) = KnownGpuTick() >= tick`
   (`vk_master_semaphore.h:34-36`) and the tick refreshed from the timeline semaphore
   (`vk_scheduler.h:608-614`, `vk_master_semaphore.cpp:61-68`). `PopPendingOperations` is called at
   the end of every submit and in `Finish()` (`vk_scheduler.cpp:1033`, `:863`).
4. `slot_images.erase` runs `~Image` → `~UniqueImage` → `Release` **inside that callback**
   (`image.cpp:96-112`). So the earliest an image can enter the pool is the first submit after the
   GPU passed its tick.

`CurrentTick()` at defer time is the value the *next* submit will signal, and that submission
includes everything recorded so far (`vk_scheduler.cpp:950`, `vk_master_semaphore.h:30-32`,
`:43-46`), so `KnownGpuTick >= that` covers the image's last recorded use. With up to
`MAX_IN_FLIGHT_SUBMISSIONS = 8` submissions outstanding (`vk_scheduler.cpp:84`, `:951-953`) the
release is *late*, never early — the safe direction. Presentation composition happens on the same
`draw_scheduler` (`vk_presenter.cpp:1044-1104`), so the same timeline covers it; `present_scheduler`
is constructed and finished but does no image work (`vk_presenter.cpp:761`, `:813`); the presenter's
and the VR exporter's own images are created by raw `vmaCreateImage` and never reach the pool
(`vk_presenter.cpp:874`, `vk_vr_exporter.cpp:322`), which is why asynchronous presentation cannot
smuggle a pooled image onto another queue.

The exception: `FsrPass` destroys images itself (`fsr_pass.cpp:450-453`), outside the scheduler, with
no tick gate at all — and the very next create matches the released image's shape exactly. That is
the one path where the commit's invariant is not merely unproven but false. It is latent in the
shipped configuration (`fsr_enabled` defaults to `false`, `emulator_settings.h:379`), which is why it
is major and not a blocker, but a user ticking "FSR" in the UI walks straight into it. Pre-change the
same code destroyed an in-flight image (also UB); the recycler changes the symptom from a validation
error / device loss to silent aliasing. Fixing it is a three-line deferral.

Secondary check: are there other destroy paths? `UniqueImage::Destroy` has exactly one caller
(`fsr_pass.cpp:452-453`). `Image`s exist only in `slot_images` (no stack or vector `Image` elsewhere
— the only owner is `Common::SlotVector<Image> slot_images`, `texture_cache.h:350`), erased only in
that one deferred callback, and the `SlotVector` destructor at teardown runs after
`draw_scheduler.Finish()` (`vk_presenter.cpp:812`). `SetBackingSamples`/`EnsureViewFormat` never
destroy the old backing; they mark it `retired` (`image.cpp:443`, `image.h:229`) and it dies with the
image. Good — that is the design that makes the single gate sufficient.

### 2.5 Shutdown ordering — **PASS by construction, never exercised (R6), one hole in the mechanism (R5)**

Ownership chain on this platform: `Presenter` (a `std::unique_ptr` in `gnmdriver.cpp:35`) holds
`Instance instance` by value (`vk_presenter.h:154`) and a `std::unique_ptr<Rasterizer>` (`:167`)
whose `TextureCache texture_cache` member (`vk_rasterizer.h:186`) is the only owner of the images.
`FsrPass` is a member too (`vk_presenter.h:155`).

C++ destroys members in reverse declaration order, so `rasterizer` (167) and `fsr_pass` (155) die
before `instance` (154). Concretely: `texture_cache`'s images are released into the pool while the
allocator is alive, then `~Instance` runs `ImGui::Core::Shutdown`, then
`ImageRecycler::Get().Shutdown()` (which destroys every pooled image with each entry's own
allocator), then `vmaDestroyAllocator` (`vk_instance.cpp:213-218`). Order: correct. The only member
declared *before* `instance` is `window` (`vk_presenter.h:153`), destroyed after it, and it owns no
images.

Two caveats, both recorded above:

- The whole chain is dead code on the normal exit path: `Emulator::Run` ends in `std::quick_exit(0)`
  (`emulator.cpp:587`), which skips static/global destructors, so the `presenter` global is never
  destroyed, `~Instance` never runs, and the pool is simply leaked with the allocator and the
  process. Verified by search: nothing calls `presenter.reset()` anywhere. So the ordering is
  *correct* but *unvalidated by any run*; if the exit path is ever fixed, the invariant has to hold
  then, and R5 becomes live at that moment.
- `Release` after `Shutdown` calls `vmaDestroyImage` rather than dropping the entry. Given the
  ordering above nothing does that today; it is the mechanism by which a future long-lived
  `UniqueImage` (an ImGui texture, an OpenXR host resource — both currently raw `vk::Image`s) would
  become a use-after-free of the allocator rather than a leak.

### 2.6 VRAM pressure and eviction — **PASS on the bound, MAJOR on the failure path (R2), RISK on the budget (R12)**

The bound is enforced on every growth path and arithmetic is right:

- New shape: `if (entries.size() >= MaxImages || pooled_bytes + entry.bytes > MaxBytes)` → evict or
  destroy (`vk_image_recycler.cpp:114-133`), otherwise `pooled_bytes += entry.bytes` then
  `entries.push_back` (`:134-135`).
- Same shape: one-for-one replacement (`:99-106`), so no growth. (A theoretical exception: the
  replacement is not re-checked against `MaxBytes`. Two entries with *equal* key but different
  `bytes` should be impossible — same create info, same allocation policy — so this is not worth a
  fix, only a note.)
- Eviction: `pooled_bytes - smallest->bytes + entry.bytes <= MaxBytes && entry.bytes > smallest->bytes`
  (`:121-122`) — correct arithmetic, and it prefers keeping expensive shapes over cheap ones, which
  is what the commit's measurement (8 small images, zero hits) motivated. A single image larger than
  `MaxBytes` is simply never pooled (`:130-132`).
- Removal: `pooled_bytes -= out.bytes` before `erase` (`:71-72`), so the gauge and the counter track
  the vector.

So the pool cannot exceed 16 entries or 768 MiB. What it *can* do is make the device run out: the
`within-budget` allocation flag (`image.cpp:137`) plus up to 768 MiB of retained memory turns an
allocation that would previously have succeeded into `VK_ERROR_OUT_OF_DEVICE_MEMORY`, and there is no
recovery — `ASSERT_MSG` → `Crash()` (R2). The recycler should drain itself before the process dies;
that is the cheap, obvious fallback and its absence is the second-worst thing in the commit.

The interaction with the texture cache's GC is real but bounded: `RunGarbageCollector` reads
`instance.GetDeviceMemoryUsage()` (`texture_cache.cpp:1709-1711`), which is the Vulkan heap usage
*including* the pool's allocations (`vk_instance.cpp:986-1001`), against thresholds derived from the
total budget (`texture_cache.cpp:215-226`). On the measured machine the pool's ~250 MiB sits far
below the ~4.4 GiB pressure threshold, so no churn was observed — but a 768 MiB pool on a device that
sits near the threshold would make the GC more aggressive *and* feed the pool with what it evicts,
which is a loop worth bounding as a fraction of the budget rather than a constant (R12).

### 2.7 Failure and fallback paths — **PASS except R5/R7/R9/R13**

- **Pool full** → the incoming image is destroyed immediately (`vk_image_recycler.cpp:130-132`); no
  error, no unbounded work. Eviction scans at most 16 entries (`:119-120`) — trivial.
- **From another thread** → all pool state is behind one `std::mutex` (`vk_image_recycler.h:100`),
  taken in `Acquire`, `Release`, `Shutdown`, `Snapshot` (`vk_image_recycler.cpp:61`, `:87`, `:148`,
  `:159`). No lock nesting hazard: `PopPendingOperations` releases its own mutex before invoking a
  callback (`vk_scheduler.cpp:890-901`), so the recycler lock is never taken under the scheduler's.
  `vmaDestroyImage` is called *while* holding the recycler lock — VMA is internally synchronised, and
  no code path takes the recycler lock while holding a VMA lock, so no inversion. Thread-safety is
  fine; the only reason multiple threads exist here is `DeferPriorityOperation`'s runner thread
  (`vk_scheduler.cpp:1036-1060`), which the texture cache does not use for image release.
- **Same shape released twice** → the older pooled image is destroyed, the new one kept
  (`:99-106`). No double-free (the vector holds at most one, and the released `Entry` is moved into
  the slot). This is a policy choice that costs hits (R11), not a correctness bug.
- **`queueFamilyIndexCount != 0`** → destroyed, not pooled (`:109-113`). Safe today; the check's
  position is what R9 is about.
- **`Enabled()` flipping at runtime** → it cannot: one `static const bool` (`:12-19`). The pool and
  `Create` read the same value, so they cannot disagree. R13 is about the *parsing*, not a flip.
- **UB**: no pointer is stored that outlives its owner, except the two the caller already stored in
  `image_ci` (`pNext`, `pQueueFamilyIndices`), which are never dereferenced by the pool
  (`SameShape` compares counts first; the loop is unreachable today). `Entry::view_formats` is a
  value-type `static_vector` with no pointers. `Entry` copies are `std::move`s inside the vector; the
  `Entry` passed to `Release` is a reference to the caller's local, which is left empty and destroyed
  — no dangling reference escapes.

### 2.8 Move semantics — **FAIL, inert today (R10)**

```cpp
UniqueImage& operator=(UniqueImage&& other) {
    image = std::exchange(other.image, VK_NULL_HANDLE);
    allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
    allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
    allocation_bytes = std::exchange(other.allocation_bytes, 0);
    image_ci = std::move(other.image_ci);
    view_formats = std::move(other.view_formats);
    return *this;
}
```

(`image.h:59-67`.) The current `image`/`allocation` are overwritten without being destroyed or
pooled: a whole `vk::Image` plus its VMA allocation leaks. The destructor would have released it; the
move assignment does not.

Why the standard makes this matter even though nothing does it today: container move assignment is
specified as *element-wise relocation unless it can steal*, i.e. "all existing elements of `a` are
either move assigned to or destroyed" (C++20 [container.reqmts], the `a = rv` row of the container
requirements table), and `erase`/`insert` on a sequence container move-assign the surviving elements
(`[deque.modifiers]`, `[vector.modifiers]`). Any container of `UniqueImage` that is ever moved into a
non-empty container, or erased from the middle, therefore *requires* this operator to be able to
release what it overwrites. That is precisely why `std::unique_ptr` and every RAII type in the
standard library implements its move assignment as "reset then take".

Reachability in this tree: none. `std::deque<BackingImage>` is only ever appended to
(`image.cpp:336`, `:378`, `:1330`; no `erase`/`insert` anywhere — grep), so its elements are never
shifted; `Common::SlotVector::Reserve` move-*constructs* each element and destroys the old one
(`slot_vector.h:230-256`), which uses the (correct) move constructor; and the five explicit
assignments all target freshly default-constructed objects (`image.cpp:353`, `:380`, `:1331`,
`fsr_pass.cpp:139-140`). So it is a latent leak, not an active one: `git grep` proves no path, the
code proves it would be a leak the day one exists. Fix is two lines: `Destroy();` first, or better,
`ImageRecycler::Get().Release(...)` via the existing `Destroy()`.

### 2.9 Statistics honesty — **FAIL (R4)**

What the numbers actually mean with `SHADPS4_DBG_OVERLAP` set:

| field | source | honest? |
|---|---|---|
| `images_created`, `image_mb` | `NoteImageCreated` only on the real `vmaCreateImage` path (`image.cpp:182`), never on a hit | **yes** — it means "driver creations", which is the number the recycler exists to reduce |
| `images_destroyed` | `NoteImageDestroyed` only inside `DestroyReleased` (`vk_image_recycler.cpp:140`) | **no** — the same-shape replacement (`:102`) and the eviction (`:125`) destroy an image without counting it, so with the recycler on the two numbers no longer balance, and the header's claim that it "counts what the pool could not keep" (`image_stats.h:66-68`) is false for exactly the images it throws away |
| `recycler_hits` | `NoteRecyclerHit` on the hit branch (`image.cpp:167`) | yes |
| `recycler_misses` | `NoteRecyclerMiss` whenever a create is attempted with the recycler on (`image.cpp:172`) | yes as "attempts", but it includes shapes that can never be pooled (queue-family split, larger than `MaxBytes`) — hit rate = hits/(hits+misses) is therefore a lower bound on poolability, fine, but it should say "requests" |
| `pool_images`, `pool_mb` | gauge written only from a create-hit (`image.cpp:168-169`) and from the destructor (`:109-110`) | **stale**: `UniqueImage::Destroy()` and `Shutdown()` change the pool without updating it, so after an FSR resize the gauge is wrong until the next create |
| `recycler_destroys` | `pool.destroyed`, a *cumulative* counter | **misleading** beside `exchange(0)` fields in the same line: the reader has to know which columns are rates and which are totals |

With the recycler off (the measurement baseline) the line becomes straightforward: every create
counts, every destroy counts through `DestroyReleased`, hits/misses stay 0, and the line describes
the code. So the instrumentation is only honest in the configuration that does not use the feature,
which is the wrong way round for an A/B.

### 2.10 Default safety — **against default-on for the runtime branch, for it on the investigation branch**

The change is default-on (`vk_image_recycler.cpp:12-19`; `SHADPS4_DBG_IMAGE_RECYCLER=0` disables).
The evidence in the commit is: 4.2 → 0.5 driver creates per frame, 85% hit rate, create+destroy
4.15 → 0.29 ms of GPU-thread time, pool ~250 MiB — **and frame rate unchanged** (58.7 vs 58.5 fps,
both capped, GPU 53–57% busy). So the only end-to-end measurement is taken where the benefit cannot
appear.

For default-on:

- The mechanism is measured, not inferred: four creates and four destroys a frame at 0.78 + 2.5 ms
  are real numbers from `create_ns`/`destroy_ns` around the driver calls, and the 85% hit rate says
  the shape matching is finding the intended reuse.
- The pool is bounded (16 / 768 MiB) and the reuse is gated on a tick whose semantics are sound
  (§2.4).
- The objective this work exists for is a 120 fps budget of 8.3 ms, of which 3.9 ms is this; a
  default-off flag would leave the 120 fps cell measuring a different program from the one that
  ships.

Against default-on:

- A default is a promise to every user of the fork, including the ones on a 6 GB card, with FSR
  enabled, resizing the window. The two failure modes the change adds — retained memory turning a
  successful allocation into an `ASSERT` crash (R2), and stale pixels where an untracked load used to
  read zeroes (R3) — are not covered by any measurement in the commit.
- The commit itself says the frame rate did not move; a default-on justified by "CPU work removed"
  without a cell where that CPU work is on the critical path is a hypothesis, not a result.
- One of the four destroys per frame (the FSR case) is outside the safety gate when FSR is on (R1).

My position: **keep it default-on in the investigation branch** (it is a measurement vehicle and its
off switch is one environment variable away), but **default it off in the runtime branch** until two
things exist: the 120 fps A/B that shows the 3.9 ms turning into frames, and a pixel A/B that shows
the rendered output is identical with the recycler on and off. If the team would rather ship it on,
then R1 and R2 are conditions, not suggestions — a crash-on-OOM default is not acceptable for a
performance option.

---

## 3. Also checked

### 3.1 `fcdf3de` — `CoveringCopyOf` and the `ExpandImage` coupling

**The predicate is sound for everything it accepts.** I checked it against `Image::CopyImage`
(`image.cpp:860-1037`) and `SanitizeCopyLayers` (`:748-793`) clause by clause:

- `num_mips = min(src.levels, dst.levels)` (`image.cpp:895`) ↔ `coverage.mips` (`image_coverage.h:108`).
  exact.
- per level the copy writes dst `[0, src_mip_w) × [0, src_mip_h)` at offset (0,0) with
  `layerCount = min(src_layers, dst_layers)` for same-type 2D (`image.cpp:958-961`) and
  `extent.depth = src_mip_d`, `layerCount = 1` for same-type 3D (`:951-955`) ↔
  `coverage.layers = min(...)` / `= 1` (`image_coverage.h:109-117`). exact.
- "covers the whole destination level" needs `src_mip >= dst_mip` at every level, and the predicate
  gets it from `src.size.width/height >= dst.size.width/height` (`:103-105`) because `x >> m` is
  monotone — floor division preserves the order, including for block-compressed images where both
  sides are block-shaped (the predicate requires `is_block` to match, `:97-99`).
- every shape where the copy's region is *not* the source extent is refused: 1D (`:79`), 2D↔3D
  (`:79`), different texel size or sample count (`:85-87`), depth/stencil aspect (`:93-96`) — the
  last one for the right reason (HTILE/stencil live beside the texels; `ResolveDepthOverlap` handles
  this title's depth aliasing).
- the caller-side contract ("the copy follows and is authoritative") is honoured by `ExpandImage`,
  which computes the coverage and then uploads only `!CoversLevel` levels
  (`texture_cache.cpp:1013-1045`) before `new_image.CopyImage(src_image)` (`:1046-1048`).

**The `carries_contents` coupling is sound and complete for the #5444 port.** Today the copy is
unconditional, so the skip is sound; the copy call is itself gated on `carries_contents`
(`texture_cache.cpp:1046`, `const bool carries_contents = true;` at `:1005`) and the coverage
computation is gated on the same variable (`:1013`). Porting #5444 is then a one-line change to the
initialiser, and both the copy and the skip change together — which is exactly the property that
prevents the "skip an upload for a copy that no longer runs" corruption. The residual risk is
procedural: the coupling is a variable, not an enforced structure, so a future edit could add a
second condition at the copy site. That is worth one comment and nothing more; the design is right.

**The one caveat:** the predicate has no *independent* proof obligation — it is a model of
`CopyImage` living in a different file (`image_coverage.h:46-49` says so). The 18 tests
(`src/tests/video_core/test_image_coverage.cpp`) pin the model against the shapes the copy can
produce, but nothing pins the model to the code: if `CopyImage` gains a region shape (say, a cropped
copy), the predicate will keep answering. That is a maintenance risk, not a defect in this commit,
and it is the reason to keep the feature off by default (which it is: `SHADPS4_DBG_COVERED_REFRESH`
unset means off, `:1009-1012`).

**Interaction with the recycler:** none. `ExpandImage` copies into a freshly created destination,
so the destination's *old* contents (recycled or not) are never read — the copy writes, the upload
writes, and the skipping only removes writes. And on the measured scene the predicate returns
"CoversNothing" for both expand entries a frame (they expand 32×32 2D↔3D reinterpretations), so the
feature is inert for this title by its own measurement — correct, tested, and honestly reported.

### 3.2 `49ec266` — anything costing measurable time when disabled

Three things run unconditionally:

1. `ScopedTimer` around `vmaCreateImage`/`vmaDestroyImage`, `ResolveOverlap`, `ResolveDepthOverlap`,
   `ExpandImage`, `RefreshImage` (`image.cpp:98`, `:116`, `:176`; `texture_cache.cpp:371`, `:666`,
   `:941`, `:1415`). Two `steady_clock::now()` per scope (image_stats.h:18-37), and they are *not*
   conditional on `Enabled()`. On Windows that is `QueryPerformanceCounter`, ~20-25 ns each; with
   ~8 such scopes a frame it is a fraction of a microsecond — not measurable on a 16 ms budget, but
   it is not zero, and it is paid whether or not the diagnostics are on.
2. The counter calls themselves (`NoteImageCreated`, `NoteImageDestroyed`, `NoteCopy`,
   `NoteRefresh`, `NoteExpand`, `NoteRecyclerHit/Miss`) start with `if (!Enabled()) return;` on a
   cached `static const bool` — a predictable branch, free.
3. `Snapshot()` in `~UniqueImage`/on a hit takes the recycler mutex and copies six integers *before*
   `SetRecyclerPool` gets the chance to return early on `!Enabled()` (`image.cpp:109-110`,
   `:168-169`; `vk_image_recycler.cpp:158-166`). Two extra mutex acquisitions per create/destroy
   pair when the diagnostics are off. Also negligible, but it is a lock taken for a value nobody
   asked for.

Not introduced here but adjacent and worth naming, since the commit message says "everything here is
off unless the existing diagnostic variables are set": `NoteOverlap` calls
`std::getenv("SHADPS4_DBG_OVERLAP")` on every overlap/expand entry (`texture_cache.cpp:52-55`), and
`Image::CopyImage` calls `Core::Vr::FrameTimeline::MarkStr` unconditionally on every whole-image copy
(`image.cpp:868`) — both cheap (`MarkStr` returns on a cached bool, frame_timeline.h:53-56), both on
the draw path. The honest verdict for `49ec266`: no measurable cost when disabled, with two clock
reads per image create/destroy as the only thing a profiler could see.

A functional note from reading that diff: `ReadCopyTimings` prints `recorded` *after* resetting it to
zero only on the *next* report, and it resets `requested`/`recorded`/`dropped` on a successful read
while keeping the pending pairs on a failed one — the accounting is consistent, but the line's
`recorded` field is the count *since the last successful read*, not since the last request. Fine for
its purpose, worth a comment if anyone builds a trend on it.

---

## 4. Recommendation

**Approve with conditions** — do not veto; the mechanism is sound, the key is complete, the layout
and state story is correct, and the pool is bounded and thread-safe. Integrate `6170df6` into the
runtime branch only when all three conditions below hold.

1. **Close the FSR hole (R1).** Either defer the FSR release to the scheduler's tick or make
   `ImageRecycler::Release` refuse any entry that was not produced after the GPU passed its tick.
   Until then, the commit message's three-invariant claim is false as written, and an FSR resize can
   hand an image that a submission may still be reading to the next image created.
2. **Add the OOM fallback (R2).** Drain the pool and retry once before `ASSERT_MSG` takes the process
   down; the pool is the reason the allocation can fail, so it has to be the first thing to give.
3. **Produce the two missing measurements (R3, item 10).** A pixel A/B with the recycler on and off on
   the same guest frame, and the 120 fps cell that shows the 3.9 ms becoming frames. If the 120 fps
   cell cannot be produced, default the feature off in the runtime branch and keep it default-on only
   where it is being measured.

Ordered after those, in the same branch: R4 (make the RENDERSTAT line describe the code, or the A/B
cannot be read), R5/R6 (state in the commit that the shutdown ordering is unexercised, and make a
late `Release` a drop instead of a `vmaDestroyImage`), R8/R9 (turn the two unproven assumptions into
assertions), R10 (release in move assignment), R11 (allow more than one per shape; add a device-free
test), R12/R13 (derive the byte bound from the budget; parse the switch the way its siblings parse
theirs).

`fcdf3de` is approvable as it stands: the predicate refuses everything it cannot prove, the coupling
to the copy is expressed with one variable, and it is off by default with 18 tests behind it.

---

### Appendix — how to re-check what this review claims

| claim | command / place |
|---|---|
| every live `UniqueImage::Create` call site, and that none passes `pNext` or a view-format chain | `git grep -n "image.Create(\|image_ci = " -- src/src/video_core` |
| the only `queueFamilyIndexCount` in the tree is the swapchain's | `git grep -n "queueFamilyIndexCount\|pQueueFamilyIndices" -- src/src` |
| one VMA allocator, created once | `git grep -n "vmaCreateAllocator\|vmaDestroyAllocator" -- src/src` |
| `UniqueImage::Destroy` has exactly one caller (FSR) | `git grep -n "\.Destroy()" -- src/src/video_core` |
| no `backing_images.erase`/`insert` | `git grep -n "backing_images\." -- src/src` |
| images live only in `slot_images`, erased only from the deferred callback | `git grep -n "slot_images\.erase" -- src/src` |
| the process leaves via `quick_exit`, so the teardown chain never runs | `git grep -n "quick_exit" -- src/src/emulator.cpp`; `git grep -n "presenter\.reset\|presenter = nullptr" -- src/src` (empty) |
| the pool bound is enforced on every growth path | `vk_image_recycler.cpp:99-136` |
| the FSR image setup that makes the self-recycle exact | `fsr_pass.cpp:445-498` |
