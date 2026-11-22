# SC-06: title-screen semantic rendering capture

## Outcome against the acceptance criteria

| Criterion | Status |
| --- | --- |
| 1. Verified, reproducible title-screen launch and capture profile | **Met** |
| 2. Evidence-backed account of why SC-04/05 failed | **Met** |
| 3. Correct resource-pointer-to-GPU-address handling | **Met** |
| 4. Bounded observation of overlapping rendering work | **Met** |
| 5. One defensible RHI-to-GPU association, or a precise blocker | **Partly met.** RHI→GNM established; GPU half blocked with a precise cause |
| 6. Contract tests pass, default OFF unchanged, clean postflight | **Met** |
| 7. Committed and pushed on the dedicated branch | **Met** |

## The headline

The title screen is reachable, verified, and now the default research environment. SC-04 and
SC-05 were blocked by launcher configuration, not by the scene.

## Why SC-04 and SC-05 failed

Three differences.

**The save slot.** The M1 launcher stages `MGS_GameData_0`, `MGS_GameData_1`, `MGS_SystemData`
and `sce_sys` into the private lab. SC-04/05 built labs from `config.json` alone.

**The Move autopress.** The title is Move-driven. `move.cpp` presses Move button bit 2 with the
trigger at full. SC-04/05 used pad-only or no autopress.

**Two inert environment variables.** `SHADPS4_DBG_AUTOPRESS_TO` and `SHADPS4_DBG_AUTOPRESS_MENU`
are set by the M1 docs and launcher but read **nowhere** in the C++ tree. The schedule is fixed at
8-45 s in `autopress.h`. That is a documentation defect.

## The verified scene

| Item | Value |
| --- | --- |
| Eye size | 2880x3240 per eye |
| Worn lines | 0 |
| Front end | 472.7 draws, 535.2 Mpx, 86.4 passes |
| Scene | classroom with the main-menu overlay |
| Evidence | render-target dump at HMD frame 2400 |

`scene_signature.py` scores 0.442 against the 0.85 threshold. That is a **framing** difference,
not a scene difference: the reference is a single-eye 1280x720 crop and this is the packed stereo
pair at a different camera position. Brightness (36.35 vs 40.46) and contrast (35.09 vs 35.62)
match closely, and the side-by-side comparison shows the same room and the same menu.

The scene is identified visually, as the task directs. The numeric gate is reported as a framing
mismatch, not weakened.

## The capture profile

| Setting | Value |
| --- | --- |
| `SHADPS4_VR` / `OPENXR` / `VR_DEMO` | 1 / 0 / 0 |
| `SHADPS4_TITLE_EYE_WIDTH` | 2880 |
| `SHADPS4_VR_SCALE` | 0.5 |
| `SHADPS4_VR_DESKTOP_VIEW` | stereo |
| `SHADPS4_XR_PAUSE` | 0 |
| `SHADPS4_DBG_AUTOPRESS` | 1 |
| `SHADPS4_DERACINE_CMD_CAPTURE_AFTER` | 33 |
| `SHADPS4_EXIT_AFTER` / `CLEAN_EXIT` | 75 / 1 |

Two changes were required. The capture bound rose from 512 to **16384**, because at 512 it filled
during the health warning. And the export needs a **clean exit**, because the M1 launcher
force-kills and `Emulator::Shutdown` never runs.

Result: 5346 indexed draws across 63 distinct vertex-shader resources, against 156 draws and 2
resources before.

## The address-domain correction

The RHI `IndexBuffer` field and `regs.index_base_address` are **not comparable**.

| Layer | Value | Domain |
| --- | --- | --- |
| RHI resource | `0x10044E49E8` | guest heap |
| GNM `index_addr` | `0x2000042800` | GPU-visible |
| PM4 `index_base` | same as GNM | GPU-visible |

The translation is a function of the resource, not the draw: two resources map to two addresses
with **different** deltas (`0xFFBB5DE24` vs `0xFFBBD7978`), so it is not a constant bias.

`StartIndex` is a separate argument and is not folded into `index_addr`, so two draws can share an
address and differ only by start index.

This corrects SC-05's proposal. Adding `regs.index_base_address` and comparing it to the RHI
pointer would have compared `0x2000...` against `0x1004...` and produced zero matches — the
primitive-enum mistake in a different form.

## The observation windows

Both probes observed from boot with a 128-record cap, so they filled during the health warning.
Bounded start delays were added to the RHI probe and to the PM4 hook.

With all three gated at 33 s:

| Probe layer | Counts present in the capture |
| --- | --- |
| GNM `index_count` | **128 / 128** |
| PM4 `index_count` | **128 / 128** |

But GNM-to-PM4 joins give **0 unique, 0 ambiguous, 128 unmatched**, with zero shared addresses.

The cause is structural: the two threads consume the stream at different rates, so equal caps
land at different stream positions. The GNM records begin with `6@0x2000042800`; the PM4 records
begin with `11133@0x200bf50000`. Different draws.

## The association that works

The capture hooks the wrapper at RVA `0x13A68B0`. The RHI probe observes `0x0CBB9C0`, whose caller
is `0x13A68E8` — that is `0x13A68B0 + 0x38`, **inside the same wrapper**.

So capture draw *N* and probe event *N* are the same call. The join is by ordinal.

| Test | Result |
| --- | --- |
| Offset 0, index counts | **122 / 128** |
| Multiset over the first 128 | **identical** |

The six mismatches are adjacent swaps of the same two draws, because the GNM records come from
several `TaskGraphThreadNP` threads. That is a bounded limitation, not an identity failure.

### One associated draw

| Layer | Value |
| --- | --- |
| Scene | classroom with main-menu overlay |
| UE4 command | `CommandDrawIndexedPrimitive`, wrapper `0x13A68B0` |
| Topology | `PT_TriangleList`, `NumPrimitives=2` |
| Index count / instances | 6 / 1 |
| RHI index resource | `0x10044E49E8` |
| GNM `index_addr` | `0x2000042800` |
| GNM `cmdbuf` | `0x30000F0AE0` |
| VS resource | resolved via alternate slot `+0x1E0` |
| Correlation | ordinal within one wrapper, multiset-validated |

## The remaining blocker

The **GPU-side resolved draw** for that specific draw is not associated.

```
UE4 command --[ordinal, validated]--> RHI entry --[same thread, 1:1]--> GNM draw
                                                                          |
                                                                  [NOT JOINED]
                                                                          |
                                                                   PM4 / GPU draw
```

Parameter matching is ruled out (zero shared pairs, and `48@0x20000c0000` recurs). Equal record
caps are ruled out (different consumption rates).

The remaining mechanism is a **submission identity**: the GNM call receives `cmdbuf`, and the PM4
packet is written into that same buffer. Recording the packet's address within the command buffer
would join the two on real provenance. That is one field in an existing diagnostic line.

## Safety

| Check | Result |
| --- | --- |
| Default OFF | 0 capture records, 0 probe records |
| Contract tests | 12 capture + 13 probe, all pass |
| Clean exit | export fires on `Emulator::Shutdown` |
| Guest mutation | none; reads only |
| Per-draw disk I/O | none; export is post-guest |

## Files

| File | Purpose |
| --- | --- |
| `reports/SC06_phase1_title_screen.md` | scene verification and why SC-04/05 failed |
| `reports/SC06_phase3_address_domains.md` | the four address domains and the translation |
| `reports/SC06_phase4_observation_windows.md` | the overlap result and the thread offset |
| `reports/SC06_phase5_association.md` | the ordinal join and its validation |
| `src/src/core/rhi_command_capture_contract.h` | bound 16384, start delay |
| `src/src/core/rhi_entry_probe_contract.h` | probe start delay |
| `src/src/core/rhi_entry_probe.cpp` | probe and PM4 start gates |
| `src/tests/test_rhi_*_contract.cpp` | 25 tests |

## The next dependency

**A submission identity across the GNM-to-PM4 boundary.**

Everything downstream depends on it. Without it, a captured draw cannot be connected to its GPU
execution, and without that the VS/PS hash association and the Vulkan pipeline identity cannot be
attached to a specific draw.

The smallest experiment: record the PM4 packet's address within the command buffer, and the
`cmdbuf` pointer in the capture, then join on that pair.

## Longer-term direction

Once the association works, the title screen supports recovering vertex declarations, index
formats, uniform buffers, texture and sampler bindings, blend and depth state, render-target
attachments, stereo view state, and finally native Vulkan pipeline construction.

Gameplay coverage is deferred, as directed.
