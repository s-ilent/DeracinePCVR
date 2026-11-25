# C-016 — Paired resource descriptor comparison after shader-state setup

**Source:** original Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed. Static analysis only. Executable ELF VA to original SELF offset in this region: **+0x1FD30**.

## Original-byte finding

A separate function-shaped entry at ELF VA `0x0CB4330` (original SELF offset `0x0CD4060`) **couples** the C-001/C-014 shader-state routine to a *two-resource descriptor comparison and conditional resource-side state update*. It is not merely another emission helper.

* At `0x0CB434B..0x0CB4351`, input RSI and RDX become RBX and R14, respectively; RDI becomes R13. Both resource inputs are checked for null (`0x0CB4354`, `0x0CB436B`).
* `0x0CB4374..0x0CB4391`: pass `RDI=R13`, `ESI=0`, `ECX=1`, `RDX=&[RBP-0x88]` where the stack slot contains original RSI/RBX; direct call at `0x0CB438C` targets `0x0CB5130`. The caller's R8 is saved to `[RBP-0xC8]` before this call. The numeric mode passed as ESI is zero; the meaning of ECX=1 is not established.
* `0x0CB4391..0x0CB43A6`: **two independent** virtual calls at `[input1-vptr+0x38]` and `[input2-vptr+0x38]`, producing RBX and R12. These calls happen *after* shader-state setup and use the same virtual slot previously observed inside `0x0CB5130`.
* `0x0CB43B0..0x0CB43D9`: load `[result1+0x18]` and `[result2+0x18]`, call `0x03092D20` on each, shift each 32-bit return left by eight, and compare. Equal results branch to `0x0CB510F`. This is an explicit comparison of **transformed descriptors**, not proven shader hashes or GPU virtual addresses.
* On the unequal path, caller R8 points to a structure whose signed DWORD fields `+0x04/+0x08/+0x0C/+0x10` are validated as a bounded rectangle-like range (`0x0CB43DF..0x0CB440D`); invalid ranges fall back to dimensions derived from `[result1+0x18]+0x08` (`0x0CB441C..0x0CB4438`).
* At `0x0CB443D..0x0CB445E`, if `[result2+0x140]` is non-null, it stores QWORD 1 through that pointer, then copies global DWORD `[0x04496748]` into `[result2+0x148]`. This is a **conditional guest-object side effect** accompanying a descriptor mismatch. It is not evidence that the global is a GPU buffer-cache version or that the indirect pointer owns GPU memory.

The paired `+0x38` calls and common `+0x18` descriptor consumption independently strengthen the object interface identified in C-001. The new edge is **producer/consumer at a call boundary**, not an identified concrete virtual implementation. No direct E8 caller to `0x0CB4330` was found in a bounded scan of ELF VA `0x00400000..0x01800000`; this does not establish non-reachability or exclude indirect dispatch.

## Evidence and limits

The contiguous instruction-boundary decoded region `[0x0CB4330,0x0CB4460)` is 304 bytes, SELF `[0x0CD4060,0x0CD4190)`, SHA-256 `1bda546a3d09a8ce90ca62409e0a79bd424983d2f88cdecabec7caa54a8805c0`. Nested region `[0x0CB4374,0x0CB43DF)` SHA-256 `66a1359a1ff64614ef0f67cd8a99c1b1fc9fb97b0ba701b19994c5de9c138c90`. Direct relative calls at `0x0CB438C`, `0x0CB43B4`, `0x0CB43C4` independently decode to `0x0CB5130` and twice `0x03092D20`. Entry prologue is visible; enclosing FDE and precise function end remain to be established.

**Confidence:** high for call targets, two virtual-slot calls, field accesses, equality gate and conditional writes; medium for interpretation as resource-transition/stamp logic. Alternatives: descriptor-format compatibility test and invalidation marker, rather than shader binding/version update. No runtime execution, class identity, GPU address, shader hash or Vulkan binding is inferred.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can test whether paired resource transitions compare the `0x03092D20`-normalized `+0x18` descriptors and whether `+0x140/+0x148` changes accompany a changed binding; do not equate the global stamp with cache generation. **Next precise target:** find original relocations/vtable implementation(s) of virtual `+0x38` on the two input resource objects and prove their constructor/ownership path; independently establish the FDE for `0x0CB4330`.
