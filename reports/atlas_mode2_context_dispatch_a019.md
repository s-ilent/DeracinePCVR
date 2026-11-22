# A-019 — Two alternate-viewport commands dispatch to different context interfaces

Original Déraciné 1.00 SELF: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly checked before decoding. All addresses here are **inner ELF virtual addresses**; original SELF code file offset = VA + `0x1FD30`, and RELRO table offset = VA + `0x21240`. Neither domain is a loaded guest address, GPU VA, or shader hash. No runtime reachability is claimed.

A-018 established that the owner mode-2 path at `0x1404710` enqueues a local 0x30-byte node with vptr `0x03EDFC00` and a separate global 0x18-byte node with vptr `0x03EDFC80`. This run resolved **both Execute dispatches** from original SELF bytes, not from source names.

## Original-byte evidence

RELRO table entries (little-endian QWORDs, unrelocated addends):

| ELF vtable VA | Execute target | destructor target |
| --- | --- | --- |
| `0x03EDFC00` | `0x0CACC90` | `0x0CACCC0` |
| `0x03EDFC80` | `0x0CACD70` | `0x0CACD90` |

`0x0CACC90` (entry verified with GNU objdump x86-64): loads context `[command_list+0x20]` into RAX, four node payload arguments `[node+0x20]` (ESI), `[node+0x24]` (EDX), `[node+0x10]` (R8D), `[node+0x28]` (R9), and `[node+0x18]` (RCX). It loads context vptr `[rax]`, target `[vptr+0x30]`, and tail-jumps there at `0x0CACCB5`. The adjacent second command variant `0x0CACCD0` uses **command_list+0x28** instead but otherwise the same vtable slot `+0x30` and payload layout.

`0x0CACD70`: loads context **`[command_list+0x28]`**, payload pointer `[node+0x10]` into RSI, context vptr `[rax]`, target `[vptr+0x00]`, and tail-jumps at `0x0CACD86`. Its adjacent alternate `0x0CACDA0` is **not** a trivial Execute: it accesses context `[command_list+0x20]`, node `+0x10/+0x18/+0x20`, nested resource `+0x10`, state `+0x460/+0x568/+0x928`, and calls `0x02F62440` and `0x0CAB2E0`; it is a distinct, longer path, not yet attributed to the `0x03EDFC80` table.

## Interpretation

**High confidence:** A-018's two mode-2 additions cross *different deferred execution contexts*: the local `0x03EDFC00` node targets context `+0x20` virtual method `+0x30`; the global `0x03EDFC80` node targets context `+0x28` virtual method `+0x00`. This is a concrete two-interface dispatch boundary above platform command generation, not merely two queue allocations. The paired variants support a shared command-node family with different context routing, but do not prove the interfaces' C++ names.

**Unresolved:** actual concrete vtables and methods behind the two context pointers; meaning of the mode-2 resource and whether it corresponds to graphics/async compute, cross-thread handoff, or another division. Do not infer eye/stereo semantics or runtime execution.

## Reproduction

1. Hash original private SELF and check expected size/hash.
2. Read QWORD pairs at SELF offsets `0x03EDFC00+0x21240` and `0x03EDFC80+0x21240`.
3. Read code windows `[0x0CACC90,0x0CACD10)` and `[0x0CACD70,0x0CACE80)` at SELF offsets VA+`0x1FD30`. Disassemble with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=<window-start>`.
4. Check boundaries `0x0CACC90`, `0x0CACCD0`, `0x0CACD70`, `0x0CACDA0`; verify tail-jump register origins rather than relying on raw opcode matches.

Next: locate concrete vtable owners for the two command-list context fields and decode their `+0x30` and `+0x00` targets.