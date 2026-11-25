# C-024 — Keyed CPU registry removal and free-list mutation

Original Déraciné 1.00 PS4 SELF (81,506,767 bytes), SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently verified. Addresses below are **inner ELF VAs**, not loaded guest, GPU or Vulkan addresses. In this window original SELF offset = ELF VA + `0x1FD30`.

## New original-byte finding

An instruction-boundary-decoded block `0x0CB80D0..0x0CB828B` implements **removal from the same CPU-side keyed registry** whose lookup is established by C-022 and whose reset is established by C-023. This is a mutation/unlink path, **not** the requested insertion producer. Its enclosing function entry/FDE remains unresolved: branches back to `0x0CB556A` and `0x0CB5590` prove that treating `0x0CB80D0` as a function entry would be incorrect.

- `0x0CB80F7..0x0CB8105`: compares `[r15+0x40C]` against `[r15+0x3E0]`; equal counts bypass this removal.
- `0x0CB810B..0x0CB8174`: hashes a saved key with seed `0x9E3779B9`; `0x0CB8176..0x0CB819A` selects heap bucket pointer `+0x418` or inline bucket storage and masks the index by `+0x420-1`.
- `0x0CB81A0..0x0CB81C6`: follows `+0x3D8` records of stride 16, compares the 64-bit key, and follows signed next indices at record `+0x08`.
- `0x0CB81CB..0x0CB81FC`: locates the matching entry in a second bucket chain (using record `+0x0C` as a bucket-mask input); branch `0x0CB81E2 -> 0x0CB556A` leaves the bounded window.
- `0x0CB81FC..0x0CB8240`: on the local unlink path, updates the prior chain/free-list link via `+0x408`, writes sentinel `-1` into record `+0x00`, stores a free-list link at record `+0x04`, assigns removed index to `+0x408`, and **increments** `+0x40C`.
- `0x0CB8247..0x0CB8278`: selects `+0x3F8` or inline bitset storage, derives a 32-element word index, and clears a bit via an AND mask.

The `+0x40C` increment on removal strongly argues that this field is **not a simple live-entry count**; it may track free entries or another availability state. The two bucket traversals and the branch to the earlier block need whole-function CFG inspection before exact chain invariants are claimed.

## Reproduction and limits

Verified SELF file interval `[0x0CD7E00,0x0CD7FBB)`, ELF `[0x0CB80D0,0x0CB828B)`, SHA-256 `b61b2c4cc5e2461401b002b86327dc4530a530fb62e33f91549f3806aeab60e9`. Decode from this known instruction boundary with `objdump -D -b binary -m i386:x86-64 --adjust-vma=0xCB80D0`. This block was independently checked against the original SELF, not copied from C-022/C-023. Confidence **high** for record unlink, free-list and bitset mutation; **medium** for field semantic labels; exact enclosing FDE, caller, allocation and runtime use **unverified**.

Competing interpretations: CPU resource deduplication table, binding-state suppression registry, validation/hazard tracking. No GPU buffer-cache version, shader hash or GPU virtual address is established.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 now has three distinct CPU-registry operations (C-022 lookup, C-023 reset, C-024 removal) to keep separate from GPU descriptor/resource-version tracking. This block is not evidence of live execution.

**Next precise target:** find the **insertion/allocator producer** of the 16-byte records at `+0x3D8`, bucket table `+0x418/+0x420`, and free-list head `+0x408`; resolve the enclosing FDE/CFG before naming the owner.
