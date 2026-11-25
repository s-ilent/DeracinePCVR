# C-003 — Conditional 72-byte resource-state emission within shader-state path

**Original target:** Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly checked. Static only. In this executable segment original SELF offset = ELF VA + `0x1FD30`.

## Independently verified new producer/consumer edge

Within the C-001 FDE starting at ELF VA `0x0CB5130`, the non-null resource path at `0x0CB55B1` calls the resource object's virtual `+0x38` method and obtains a returned object. This object's pointer `+0x20` is not always merely a flag: it is dereferenced as a second descriptor/state block. At `0x0CB55E5` the nested block's DWORD `+0x1C` must be nonzero; `0x0CB55EF` checks its byte `+0x11` bit `0x40`. The latter condition enters a distinct state-emission path at `0x0CB57CF`.

The branch at `0x0CB57D9` again invokes the resource vtable `+0x38`, then loads the returned object's `+0x20` as the source descriptor. It obtains the writer from context `+0x460` (`0x0CB57F1`), checks remaining DWORD capacity against `0x11`, and calls writer callback `+0x18` requesting `0x12` DWORDs if needed (`0x0CB5813..0x0CB5827`). An external helper at `0x033C8380` is called at `0x0CB5839` with a fixed table pointer `0x035CAF60`; when it returns zero, the cursor advances by `0x48` (`0x0CB5842`). This helper's identity/semantics remain unverified.

The same nested descriptor's `+0x11` bit `0x40` is tested again at `0x0CB585C`. The routine calls `0x03091BF0` with the nested block and selects **18 versus 3 DWORDs** based on its return (`0x0CB5867..0x0CB5883`). For the 18-DWORD case, `0x0CB58CB` stores 64-bit header `0x00000318C00E6900`; `0x0CB58D8..0x0CB58E8` copies two 32-byte AVX vectors from the nested descriptor at `+0x00` and `+0x18` into output `+0x08` and `+0x20` (the ranges overlap in source; do not describe this as a contiguous 64-byte copy). At `0x0CB58F7` it writes `0xC0001000` at output `+0x40`, and at `0x0CB58FE` it copies descriptor DWORD `+0x3C` to output `+0x44`. The resulting writer cursor advances to output `+0x48`.

This is a separate **nested descriptor → conditional multiword state packet** boundary, distinct from C-001's seven-DWORD `0xC0055800` resource packet. The 18-DWORD branch is not proven to represent shader user-data, a Vulkan descriptor, or a GPU address; its exact GNM register meanings and runtime reachability are unresolved. The 3-DWORD alternative and the meaning of helper `0x03091BF0` require further decoding.

## Verification and address provenance

- Original SELF window ELF `[0x0CB57CF,0x0CB59CF)`, SELF `[0x0CD54FF,0x0CD56FF)`, SHA-256 `a2162445681946519c8d2a46e05622bf8e62dd34c54996f066175fa1028f17e3`.
- Independently decoded using `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x0CB57CF` on that 512-byte window. Decode the earlier predecessor window around `0x0CB55B1` to confirm the two field predicates.
- ELF virtual addresses here are neither original SELF offsets nor loaded guest addresses; no PS4 GPU VA, shader hash, or relocated table slot has been inferred.
- Confidence **high** for nested-object field accesses, bit predicate, two direct helper calls, conditional 18/3-DWORD capacity, 72-byte writer and copied fields; **medium** for generic descriptor/state interpretation; **unknown** for shader stage, exact resource type, semantic hardware register and live use.

**Transfer to SC-09/SC-10/SC-11:** compare captured 72-byte command sequences and nested guest descriptor `+0x11` bit `0x40`, `+0x1C` and `+0x3C` against user-data/descriptor state, separately from C-001's 28-byte packet. Do not conflate CPU descriptor bytes, GPU VA and Vulkan buffer versions.

**Next precise target:** independently decode `0x03091BF0` and the three-DWORD branch at `0x0CB5A1D`, determining the predicate for full versus abbreviated state emission.
