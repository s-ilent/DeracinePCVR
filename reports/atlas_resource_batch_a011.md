# A-011 — Resource stamping and two 16-byte context submission arrays

**Target:** original Déraciné 1.00 SELF (81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`). Static only; no runtime reachability established.

## New finding

The instruction-boundary-confirmed block **ELF VA 0x0CC2ADE–0x0CC2C48**, inside a larger graphics-context function, takes a nonzero resource count in `r14d` and iterates resource pointers from the array at `[rsp+0x30]`. At 0x0CC2AEB it snapshots the DWORD at **ELF VA 0x04496748** once into `[rsp+0x20]`; the loop uses that same snapshot for every resource. This strengthens the interpretation of this global as a batch-scoped resource stamp, but **does not prove a frame counter or identify its writer**.

For each non-null entry, the loop checks the pointer at **entry +0x28**, invokes its vtable **+0x38** method as many as three times (each guarded by a fresh null check), and then takes one of two metadata paths:
- fallback entry **+0x18** points to a resource object whose byte **+0x28** is set to 1 and DWORD **+0x24** is stamped;
- otherwise entry **+0x20** points to a different object whose byte **+0x10** is set to 1 and DWORD **+0x0C** is stamped.

The latter stamp location is identical to the A-009 backing-object offset, but **object identity is not yet established**. The triple virtual call could be an inlined repeated operation; its return values are not used here. No resource lifetime/ownership claim follows solely from these calls.

The loop then appends **two distinct 16-byte records** to context-owned dynamically grown arrays:

1. At context **+0x438**, with count **+0x440** and capacity **+0x444**, a record stores the loop index as a DWORD at record +0 and the entry pointer at +8. Capacity growth calls **0x0CC8780**.
2. At context **+0x428**, with count **+0x430** and capacity **+0x434**, a record stores a packed QWORD assembled from entry **+0x34** and low 12 bits of entry **+0x38** (shifted by 32), followed by a DWORD at +8 derived from bits 16–29 of entry +0x38 (with zero replaced by one), multiplied by entry **+0x3C**. Capacity growth calls **0x0CC8CE0**.

The two arrays are different submission/registration representations of the same input entry, not yet proven to be a draw list, render-target list or resource-lifetime list. Their pairing and geometry-like packed arithmetic provide a concrete new semantic boundary above the low-level command writer.

## Original-byte evidence and address domains

For these verified windows, **original SELF offset = ELF VA + 0x1FD30**. This mapping is local to these windows; it is not a loaded guest address, relocation slot, GPU VA or shader hash.

| ELF VA range (end exclusive) | Original SELF offset | Bytes | SHA-256 |
|---|---|---:|---|
| 0x0CC2ADE–0x0CC2B20 | 0x0CE280E | 66 | `25f757a6b9a4632175f13e0bab9500fb0ba03e6baa984d7f0c4938b6537844cf` |
| 0x0CC2B20–0x0CC2B99 | 0x0CE2850 | 121 | `785dca2d10be9b11822479e646a29502062449839a1d3926e26e104e9a2a7264` |
| 0x0CC2B99–0x0CC2C48 | 0x0CE28C9 | 175 | `6a5aa36e16fcaf5a4bd23e1e94d7c3fbf63540f3487ce27c5f40113f7f268856` |

Decoded independently using GNU objdump `-D -b binary -m i386:x86-64 -M intel --adjust-vma` on the exact original-byte windows, with the boundaries selected at decoded instruction starts. Reproduce digests and critical instruction/control-flow checks with `tools/atlas/verify_resource_batch_a011.py`.

**High confidence:** global load, snapshot reuse, two conditional metadata layouts, exact array strides/counters/capacities, growth calls and packed field arithmetic. **Moderate:** batch-level resource registration. **Unresolved:** owning function entry/complete FDE, entry and object C++ identities, meaning of bitfields, writer of global 0x04496748, whether either array is consumed by a draw/resolve operation, and runtime reachability.

**Next:** trace consumers of context +0x428/+0x438 arrays and identify the global 0x04496748 writer. Prioritize verified producer/consumer edges over speculative GPU packet naming.
