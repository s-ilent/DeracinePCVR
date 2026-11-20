# A2 — Déraciné UE4 internal RHI recovery

## Result

**Recovered:** one strong static candidate for the UE4 graphics command-context draw seam above GNM/PM4 generation: the function at inner-ELF VA `0x0cbb9c0`, likely the PS4 implementation of `IRHICommandContext::RHIDrawIndexedPrimitive`. The function is FDE-bounded, calls the imported `sceGnmDrawIndex` stub at `0x033c81a0`, is a target in a relocated function-pointer table, and contains a RIP-relative reference to the target's retained `UE4_18 ... PS4RHI\Private\GnmCommands.cpp` path literal.

The interface-method mapping is **high-confidence static attribution, not a recovered C++ symbol name**: the private UE4.18 PS4 implementation source and symbol/PDB data were not available. The likely class/method name is an inference from the exact UE4.18 public contract, this binary's argument setup/vtable evidence, and a separately version-labeled later PS4-RHI analogue.

The pinned target is **Déraciné 1.00**: `D:\Temp\Deracine\1.0\Image0\eboot.bin`, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. This is the 1.00 target; the 1.01 shadow-copy finding is not a target limitation.

## Primary static evidence: indexed draw

- The GNM import-caller scan finds `E8` at `0x0cbbd55` targeting the `sceGnmDrawIndex` import/PLT stub at `0x033c81a0`. The caller lies in FDE `[0x0cbb9c0, 0x0cbbee5)`.
- The call setup matches the imported API shape `(cmdbuf, size, index_count, index_addr, flags, type)`: `ESI=10` (decimal, `0x0a`), `EDX=R13D`, `RCX=RBX`, `R8D` masked to bit 0, `R9D=1`, and `RDI` loaded from `[RDI+0x10]`. The exact runtime values behind the dynamic count/address are not known.
- A relative ELF relocation at table slot `0x03ee0190` points to function VA `0x0cbb9c0`. The table base `0x03edfca0` is loaded at code VA `0x0cace4b`; the following instruction at `0x0cace52` stores that pointer through `[RBX]`, consistent with an object vptr assignment. The draw entry is at byte offset `+0x4f0` from that base. The table is vtable-shaped; RTTI/type name is not recovered.
- Code at `0x0cbba83`, inside the draw FDE, references the string at `0x035cae6c`: `W:\MGS\Source\UE4_18\Engine\Source\Runtime\PS4\PS4RHI\Private\GnmCommands.cpp`. This is a retained compilation-unit path clue, not a line table or a method-name proof.
- The exact UE4.18 public RHI command executor dispatches `FRHICommandDrawIndexedPrimitive::Execute` through `CmdList.GetContext().RHIDrawIndexedPrimitive(...)` in `RHICommandListCommandExecutes.inl`. The 4.18 `RHIContext.h` interface includes this semantic draw operation. Combined with the target-side draw signature setup, GNM import call, vtable entry, and `GnmCommands.cpp` path reference, this is the best-supported pre-PM4 operation recovered.
- As a callee-contract cross-check only, the local emulator's [`sceGnmDrawIndex` handler](../src/src/core/libraries/gnmdriver/gnmdriver.cpp#L459-L478) uses the same `(cmdbuf, size, index_count, index_addr, flags, type)` signature and writes a `DrawIndex2` PM4 packet. That handler is not the game's code and does not prove runtime reachability.

This creates a plausible internal hook point at function entry VA `0x0cbb9c0` or at the observed table entry slot `0x03ee0190`, before the GNM draw-emission call. Neither address has been demonstrated executing at runtime. A game-internal detour/vtable hook does not require the method to be a dynamic-library export; the import-only/export-only finding does not rule out this seam. B2's GR2port `FrameGraph` hooks are a separate game-specific analog only and were not used as evidence for Déraciné or this UE4.18 interface attribution.

## Related members and lower boundary

The same object-vptr table base `0x03edfca0` has additional relative relocation targets:

- Slot `0x03edfdb8` points to FDE start `0x0cadc00`; that FDE calls `sceGnmDispatchDirect` at `0x0cadda0`. Its argument setup matches `(cmdbuf, size=9, threads_x, threads_y, threads_z, flags)`. This is consistent with an `IRHIComputeContext::RHIDispatchComputeShader` implementation, but the private method name is not resolved.
- Slot `0x03edffe0` points to FDE start `0x0cb5130`. Calls inside that FDE include `sceGnmSetVsShader` (`0x0cb7075`) and `sceGnmSetPsShader350` (`0x0cb702b`). This is a graphics shader/pipeline-state operation candidate; the exact UE method name is unresolved.
- The separate submit FDE `[0x0cd69a0, 0x0cd6cd2)` calls `sceGnmSubmitCommandBuffers` at `0x0cd6bc4` and references the retained `...PS4RHI\Private\GnmManager.cpp` path. No relocation to its FDE start was found in the context table described above. Treat this as a separate command-buffer submission boundary, not as proof that it is a member of the same RHI context vtable.

The canonical candidate rows, including distinct inner-ELF VA, inner-ELF file-offset, original SELF container-offset, vtable, and source-string coordinates, are in [`A2_internal_rhi_candidates.tsv`](../maps/A2_internal_rhi_candidates.tsv). GPU virtual addresses and shader hashes are explicitly `not_observed`; they are not inferred from ELF addresses or register setup.

## Exact UE4.18 source comparison

The public source reference is the third-party mirror `aceyan/UE4.18.3_release`, pinned at commit `4c9fc5d3a2bf51029aa7d7295e09620d298739ef`. Its `Engine/Build/Build.version` reports UE `4.18.3` (major 4, minor 18, patch 3). This is a version-pinned public source mirror, not an Epic-signed upstream commit. The target's retained `UE4_18` path identifies the 4.18 family but does not independently establish that Déraciné used patch 4.18.3 or a particular Epic changelist; the mirror is the exact-version public API reference, not proof of the game's precise private source revision.

Relevant exact-version files:

- [`DynamicRHI.h`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/DynamicRHI.h): `FDynamicRHI` is the dynamically bound RHI backend interface; `IDynamicRHIModule::CreateRHI` returns an `FDynamicRHI*`. This is distinct from the per-command `IRHICommandContext` seam.
- [`RHIContext.h`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHIContext.h): declares semantic graphics/compute context operations, including indexed draw and direct compute dispatch, above the platform command-buffer writer.
- [`RHICommandListCommandExecutes.inl`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandListCommandExecutes.inl) and [`RHICommandList.h`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h): command-list execution forwards semantic RHI commands to the context.
- [`RHICommandList.cpp`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Private/RHICommandList.cpp): confirms command-list execution/bypass and RHI-thread dispatch infrastructure around those interface calls.
- [`Build.version`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Build/Build.version) and [pinned commit](https://github.com/aceyan/UE4.18.3_release/commit/4c9fc5d3a2bf51029aa7d7295e09620d298739ef) identify the source reference.

The public mirror does not expose the private UE4.18 PS4 RHI implementation files. The game binary's retained path strings identify the original build path, but do not restore the source body or exact C++ method names. A local UE4.25.0 PS4 RHI tree contains a structurally similar `FGnmCommandListContext::RHIDrawIndexedPrimitive` that prepares index/primitive/instance state before `Context.drawIndex`; because that tree's `Build.version` is `4.25.0`, it is recorded only as a later-version plausibility cross-check and is **not** used as exact UE4.18 proof.

## Coordinates and evidence classes

- **ELF VA**: virtual addresses in the embedded ELF loaded from the pinned SELF in memory.
- **ELF file offset**: offset in that embedded ELF coordinate system, mapped through its program headers.
- **SELF container offset**: payload offset in the original `eboot.bin` SELF container. The SELF was not written/unpacked to disk.
- **GPU VA**: none recovered. The static call setup does not reveal a runtime GPU address value.
- **Shader hash**: none recovered from this RHI call/vtable analysis.
- GNM import stubs and JUMP_SLOT/GOT locations are linkage coordinates. The final runtime-resolved function targets are not observed.

Static facts are import relocation + stub mapping, decoded direct `E8` sites, FDE containment, relative function-pointer relocations, object-vptr-like store, and source-path string references. These establish a credible internal virtual dispatch seam and its relation to a GNM emission call, but not that the call was reached during a real frame.

## Runtime status and blockers

No game, emulator, or target code was executed; no hook was installed, and no runtime trace was collected. B2's safe private-runner gate is blocked because its manifest is absent, `engine.ini` is writable, and the assigned worktree has no build. Therefore runtime validation was not attempted. These are safety/runtime-validation blockers only; they do not negate the static vtable/call-site evidence. The 1.01 shadow-copy hash was not used as the target identity.

Static work was limited to read-only inspection of the pinned 1.00 file, the in-memory SELF/ELF parser, public UE source, and existing source references. No SELF extraction, executable modification, build, test, or game/emulator run was performed.
