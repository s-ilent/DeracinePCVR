# C-004 — Descriptor-format predicate controls full versus abbreviated shader-state emission

Target: original Déraciné 1.00 SELF, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly verified. Static only. For these executable regions, SELF offset = ELF VA + `0x1FD30`.

## Original-target finding

At ELF VA `0x03091BF0`, a descriptor-derived 32-bit mask is calculated and returned; it is **not** a Boolean format validator. The caller at `0x0CB5872` tests the returned mask and chooses 18 DWORDs when nonzero, otherwise three DWORDs (`0x0CB5877..0x0CB5883`). It calls the same helper again at `0x0CB58B4` to choose the packet body. The nonzero branch writes the previously identified C-003 72-byte state packet; the zero branch at `0x0CB5A1D` writes **12 bytes**, header `0x0000031CC0016900` and a zero DWORD. This resolves the abbreviated path's exact output, without assigning GPU register semantics.

The helper reads descriptor DWORD `+0x10` and extracts three fields with BEXTR controls `0x502`, `0x308`, `0x20B`, passing them to `0x0308F8F0`. It gates further processing on that callee's byte result, then checks low five bits of descriptor `+0x14` through `0x00000AB0`. If either predicate fails, the original DWORD `+0x00` is returned unchanged. On the accepted path it recomputes the format result, consults DWORD lookup table `0x03752FB0` when index <= 0x3C, derives a bit width, chooses one of `0x00003220` or `0x00003450` according to the sign of descriptor `+0x10`, and applies `andn r14d,edx,r14d` at `0x03091CFD` before returning. This operation clears a calculated field of descriptor `+0x00` rather than simply returning the predicate result. The helper includes a stack-canary comparison and failure call at `0x03091D1B`.

**Confidence:** high for byte-verified instruction sequence, field offsets, call edges, mask operation, 18/3-DWORD branch and abbreviated packet. Medium for generic format/descriptor interpretation. The exact semantics of `0x0308F8F0`, `0xAB0`, the lookup table and encoded register words remain unresolved. A nonzero returned mask is the actual full-packet criterion, not necessarily a successful validation. No live reachability, shader hash, GPU VA or Vulkan binding is inferred.

## Reproducibility

- ELF `[0x03091BF0,0x03091D30)`, SELF `[0x030B1920,0x030B1A60)`, SHA-256 `ef81479b562cad5aaa26bac08e38f89ad4ea96aa04fe0afbb4d46730164469a0`.
- ELF `[0x0CB5867,0x0CB590B)`, SELF `[0x0CD5597,0x0CD563B)`, SHA-256 `8e53e6f3515108b0559c3647d6e0c852bd128a4bd45cffc0d54ddad7d2841be8`.
- Independent instruction-boundary decode with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=<ELF VA>` on the extracted windows; inspect predecessor for the descriptor pointer provenance.
- Companion verifier rechecks original SELF SHA, both window digests and key instruction anchors without embedding proprietary bytes.

**Transfer to SC-09/SC-10/SC-11:** test whether a captured nested descriptor's `+0x00/+0x10/+0x14` fields produce a nonzero calculated mask and thus select the 72-byte state command versus the 12-byte no-payload form. This is a static packet-selection rule, not proof of GPU resource residency or buffer versioning.

**Next precise target:** decode `0x0308F8F0` and `0xAB0` with the table `0x03752FB0` to identify the three extracted format fields and determine the exact cleared-bit range in `+0x00`.
