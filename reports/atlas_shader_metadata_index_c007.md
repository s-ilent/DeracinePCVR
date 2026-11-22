# C-007 — Bounded descriptor metadata index from original SELF

Target: Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` independently verified in this run. Static only. In these windows original SELF offset = ELF VA + `0x1FD30`. No loaded guest VA, GPU VA, shader hash, or runtime reachability inferred.

## Original-target result

The C-006 common resolver at ELF `0x240` is a **bounded metadata-index generator**, not an arbitrary descriptor index. Its entry checks output RDI non-null, power-of-two `ECX` via direct call `0x2EC0`, and `ECX <= 8`. Its direct call `0x3020` accepts an input selector `ESI <= 31` and extracts four bits (BEXTR control `0x402`: start 2, length 4) from the 32-entry DWORD table `0x033CA890`, writing the result to a local output only on success. The subsequent path at `0x2b8` validates further inputs and computes a bounded scale.

The success path at `0x391..0x3c5` requires a power-of-two scaled quantity in `[0x40,0x1000]`. It calculates `x = scaled >> 6` (1..64), `b = 31 - lzcnt(x | 1)` (0..6), then calls `0x2fe0` on a previously extracted local format selector. `0x2fe0` is a bit-test predicate over immediate `0x8e60`; its result selects `index = b + 8` or `index = b`. Thus **successful resolver output is within 0..14**, and the C-006 consumer's `0x033CA910[index]` access is bounded to its 16-word metadata region. This is a bound on successful outputs, NOT proof that every input succeeds or that the caller handles failures safely.

The original table at ELF `0x033CA910` (SELF `0x033EA640`) has 16 DWORDs: `26e8,26d4,21d0,21d0,2080,2040,1000,0000,36ec,26e8,21d4,20d0,1080,1040,0000,0000` (hex). The next DWORD at `0x033CA950` is `0xFCC38FB9`, apparently the beginning of different relative-offset data; do not treat the following words as descriptor metadata. This makes C-006's 2-bit extracts from bits 6–7 versus 12–13 finite and reproducible.

## Evidence and limits

- Entry `0x240` SELF `0x1FF70`: direct calls `0x2EC0` at `0x279`, `0x3020` at `0x28F`, and later `0x2fe0` at `0x3b3`; instruction-boundary decoded with GNU objdump.
- `0x2EC0` SELF `0x22BF0`: nonzero power-of-two test `(x & (x-1)) == 0`.
- `0x3020` SELF `0x22D50`: null pointer and selector >31 rejected with `0x80D30000`, BEXTR `0x402` of `0x33CA890[selector]`, writes output and returns zero.
- `0x2fe0` SELF `0x22D10`: `bt 0x8e60,edi; setb al`; does not itself identify a shader stage.
- The `0x391` branch additionally requires earlier calls `0x2fd0/0x2ff0/0x30b0/0x3080/0x2f90` to succeed. Their detailed semantic meaning remains unresolved. A null output, invalid selector, or failed validation leaves no valid new index. No assumption about shader identities or resource residency is justified.

**Confidence:** high for the success-path index bound, bitfield extraction, calls, branch conditions, and static table contents; medium for generic descriptor-format interpretation. No claim about dynamic use.

**Transfer to SC-09/SC-10/SC-11:** model the two metadata table halves as finite 0..14 success-path indices; distinguish failed resolution from a zero-valued metadata word. This narrows capture assertions without equating index with a shader hash or GPU buffer address.

**Next precise target:** identify the producer and semantic role of the 32-entry table `0x033CA890`, particularly how selector `ESI` originates in the C-004 descriptor `+0x14` path, and verify whether all 32 selectors are reachable.
