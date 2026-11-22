# C-001 — Resource-object to seven-DWORD command emission in shader-state candidate

**Original target:** Déraciné 1.00 SELF, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`; rehashed 2026-11-22 UTC. Static only. Executable mapping in inspected segment: original SELF offset = inner ELF VA + `0x1FD30`. No runtime guest/GPU address or shader hash inferred.

## Independently decoded finding

At ELF VA `0x0CB5130` (SELF `0x0CD4E60`), a candidate shader/pipeline state routine accepts `RDI=context, ESI=mode, RDX=pointer array, ECX=count`. Its instruction-boundary decode shows `cmp r13d,5` at `0x0CB51BF`, followed by a six-entry relative jump table at `0x035C3980`. The **mode-zero path** beginning `0x0CB51E0` iterates an eight-byte-stride array (`0x0CB537D`). A non-null element is dereferenced as an object, then `call [vptr+0x38]` at `0x0CB5237` returns a second object in RBX. The returned object has a DWORD at `+0x08` overwritten with mode zero; a pointer at `+0x18` passed to direct callee `0x03092D20`; a nullable pointer at `+0x20` used to choose flags; and a DWORD at `+0x88` shifted right by eight bits for the command payload. This establishes a concrete **producer/consumer resource-object interface**, without identifying the C++ types or claiming the pointer at +0x18 is a GPU address.

The writer comes from `[context+0x460]` at `0x0CB529C`. Capacity is checked by `([writer+8]-[writer+0x10])/4`; at six or fewer DWORDs remaining, `[writer-vptr+0x18]` is called requesting seven DWORDs (`0x0CB52B2..0x0CB52D8`). Successful emission writes command header `0xC0055800`, composed flags, a shifted resource DWORD, zero, the return of `0x03092D20`, and final QWORD `0x0000000A00000000` into **28 bytes**, then advances writer cursor by `0x1C` at `0x0CB5353`. This is a fixed-size writer operation with a resource-derived payload, not proof of semantic Vulkan descriptor binding. The branch beginning `0x0CB53A0` independently repeats the same virtual +0x38, +0x18/+0x20/+0x88 field access and seven-DWORD capacity pattern while clearing returned-object +0x10 and +0x08, corroborating shared layout across two mode-zero subpaths.

## Evidence anchors

- `0x0CB5130..0x0CB5560` (SELF `0x0CD4E60`, 1072 bytes): SHA-256 `19e0b129ad7016de5b6794339c3abf7c5e50ebb59d47f447ccee237f245138fa`.
- `0x0CB5220..0x0CB5378` (SELF `0x0CD4F50`, 344 bytes): SHA-256 `e1aabbfc58d7129b07c8a1c8aa4ce0e2c4337ed10b511fa99bf2de9939748f35`.
- `0x0CB53A0..0x0CB54F0` (SELF `0x0CD50D0`, 336 bytes): SHA-256 `274d335ccd1631cb00a76935e96a932df0df72f92688e6a6a5a9f46c0edd6bf5`.
- Direct call `0x0CB524B -> 0x03092D20`; indirect `0x0CB5237 -> [resource-vptr+0x38]`. These are decoded call edges, not byte-pattern guesses.

**Confidence:** high for instruction sequence, shared field offsets, 28-byte writer contract and direct/indirect edges; medium for a shader-resource-binding interpretation. Competing interpretation: the mode-zero routine encodes PS4-specific resource state or cache commands rather than shader descriptor tables. The meaning of `0xC0055800`, the callee `0x03092D20`, six mode values, object constructors and GPU address translation remain unresolved. No live reachability is established.

**Transfer to SC-09/SC-10/SC-11:** test whether a resource whose virtual +0x38 produces this object correlates with sampled user-data/resource state; compare the seven-DWORD packet's resource-derived fields without assuming GPU/Vulkan address equivalence. **Next precise static target:** disassemble `0x03092D20` and identify its input `[returned-object+0x18]` transformation, then recover the producer of the resource vtable +0x38 target and its allocation/ownership.