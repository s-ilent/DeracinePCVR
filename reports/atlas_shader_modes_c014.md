# C-014 — Six-mode shader-resource dispatch, update and emission split

Original Déraciné 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed 2026-11-23. Static only. For the verified executable segment, original SELF offset = ELF VA + 0x1FD30. All addresses below are ELF VAs, not loaded guest addresses, PS4 GPU addresses, or shader hashes.

## New finding

At 0x0CB51BF the candidate shader-state routine compares mode (R13D) with 5, rejects values >5 to 0x0CB8637, and uses a six-entry **signed relative** jump table based at 0x035C3980. Independently read and decoded all six original table entries:

| Mode | ELF target | Distinct behavior |
|---|---|---|
| 0 | 0x0CB51E0 | resource iteration and seven-DWORD state emission (C-001) |
| 1 | 0x0CB82B4 | resource virtual +0x38 iteration, returned-object DWORD +0x08 updated with mode 1; six-DWORD writer |
| 2 | 0x0CB51E0 | same entry as 0; mode-sensitive branch, not necessarily same payload |
| 3 | 0x0CB8637 | immediate common epilogue, no mode-specific resource loop or writer |
| 4 | 0x0CB51E0 | same entry as 0 and 2; mode-sensitive branch |
| 5 | 0x0CB846C | six-DWORD writer, with no resource-array iteration at entry |

At 0x0CB82C0..0x0CB82D3 mode 1 walks eight-byte-stride resource pointers, skips nulls, calls `[resource-vptr+0x38]` and writes mode (R13D) to the returned object's DWORD +0x08. At 0x0CB82DC..0x0CB8306 it retrieves context writer +0x460 and reserves **six DWORDs** when free space <=5. Mode 5 starts at 0x0CB846C with the same writer +0x460 and six-DWORD capacity test, but does not perform the mode-1 virtual resource loop at its entry. Mode 3 and out-of-range values reach 0x0CB8637, which checks the stack canary and returns. These are instruction-boundary-confirmed paths, not E8-pattern matches.

This materially refines C-001: the entry at 0x0CB51E0 is shared by three legal mode values, so calling the whole path “mode-zero only” was too restrictive. The six-mode API contains a resource-object update path (1), shared resource state paths (0/2/4), an explicit no-op (3), and a separate writer path (5). Exact UE4 enum names, shader stages, packet/register semantics, and live reachability are **unresolved**. In particular, a six-DWORD writer operation is not automatically a Vulkan descriptor update.

## Reproducible evidence

Original SELF file offsets: table 0x035E36B0; dispatch 0x0CD4EEF; mode-1 0x0CD7FE4; mode-5 0x0CD819C; epilogue 0x0CD8367. Region SHA-256 digests and signed-table decoding are asserted by `tools/atlas/verify_shader_modes_c014.py`. Disassembly used x86-64 instruction boundaries at 0x0CB51BF, 0x0CB82B4, 0x0CB846C and 0x0CB8637. No relocation is required to interpret the relative jump table. The enclosing function entry 0x0CB5130 is already established in C-001; this investigation does not newly attribute a C++ class.

**Confidence:** high for six dispatch targets, mode-1 object mutation, mode-1/mode-5 six-DWORD writer paths, and mode-3 early return; medium for broader shader-resource semantic interpretation. Alternative: this routine selects PS4 graphics-state command formats rather than directly binding user-data/SGPR descriptors.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can classify sampled shader-state invocations by exact mode and distinguish resource mutation (1), shared resource encoding (0/2/4), no-op (3), and writer-only entry (5) before correlating any packet with user-data bindings. No FD4 ownership conclusion follows.

**Next precise target:** recover the producer and concrete vtable implementation of the resource virtual +0x38 called at 0x0CB5237 and 0x0CB82CC, including allocation and multiple field uses, rather than continuing neighboring transfer helpers.
