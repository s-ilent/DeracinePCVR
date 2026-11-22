# C-002 — Descriptor-word sanitization before seven-DWORD resource command

**Target:** original Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed this run. Static only. Executable original SELF offset = ELF VA + `0x1FD30` for these windows.

## Original-byte finding

The direct callee of C-001's `0x0CB524B` (`0x03092D20`, SELF `0x030B2A50`) is **not a GPU-address translator**. It accepts a descriptor-like pointer in RDI, reads DWORD `+0x00`, WORD `+0x06`, DWORD `+0x04`, DWORD `+0x0C`, and DWORD `+0x18`, then returns a modified copy of DWORD `+0x00` in EAX. It does not write the input descriptor.

Instruction-boundary anchors: `0x03092D40 movzx eax,word [rbx+6]`; `0x03092D44 mov r14d,[rbx]`; `0x03092D47 test ax,0x3f0`; `0x03092D57 bextr edi,[rbx+0xc],r12d` with R12D=0x514 (extract five bits at bit 20); `0x03092D5D call 0x00000AB0`; `0x03092D6A mov eax,[rbx+4]`; `0x03092D6D mov r8d,[rbx+0x18]`; `0x03092D7B bextr ecx,eax,0x614` (six bits at bit 20); `0x03092D87 lea rdx,[rip+0x6c0222]` resolves to table ELF VA `0x03752FB0`; `0x03092D91 mov esi,[rdx+rcx*4]`; `0x03092DB7 imul edx,esi`; `0x03092DE0 test r8d,0x01000000`; `0x03092DEB call 0x00003220` or `0x03092DF7 call 0x00003450`; `0x03092E13 andn r14d,edx,r14d`; `0x03092E21 mov eax,r14d`; `0x03092E30 ret`. Both early branches to `0x03092E18` return the original DWORD. A stack-canary check uses `0x04380AE0`.

The 61-DWORD lookup table at `0x03752FB0` (SELF `0x03772CE0`) has entries including 0, 8, 16, 32, 64, 96, 128 and `0xffffffff`. A six-bit index is range checked against 60; out-of-range bypasses the table-derived multiplier. Depending on the format/flags, the function computes a bitmask from extracted descriptor fields and clears those bits from its first DWORD. The resulting value is consumed by C-001's 28-byte packet writer. This is a **descriptor-word normalization/masking boundary**, not a proven GPU address conversion or Vulkan binding.

## Reproducible checks

- Function `0x03092D20..0x03092E40` (288 bytes), SELF `0x030B2A50`: SHA-256 `e907f47a0571fe80f893d16ba8cfd81747050db6a845a303f0b9568483494a93`.
- Lookup `0x03752FB0..0x037530A4` (244 bytes), SELF `0x03772CE0`: SHA-256 `1bea6c958db92ad22542248fe1c2b09730e7a6e4708ca7a698d9b48dc4eefb96`.
- Independent disassembly: `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x3092d20` over the first window; verify branch targets and direct calls. The first bytes are `55 48 89 e5 41 57 41 56 41 54 53`.

**Confidence:** high for read-only descriptor fields, table access, return-value masking and C-001 producer/consumer edge; medium for descriptor-format interpretation. The call at `0xAB0` is not identified; `0x3220`/`0x3450` output semantics, exact PS4 register identity, vtable producer, GPU VA and runtime reachability remain unknown.

**Transfer to SC-09/SC-10/SC-11:** the C-001 packet's resource-derived DWORD is a **masked descriptor word**, not necessarily a resource pointer. Compare packet field changes against source descriptor `+0,+4,+6,+0xC,+0x18` and shader user-data captures, avoiding pointer-domain conflation.

**Next precise target:** resolve the `0x00000AB0` predicate and the producer of the virtual `+0x38` resource object to establish which descriptor types reach this mask.
