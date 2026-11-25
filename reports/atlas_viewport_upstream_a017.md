# A-017 — Packed viewport coordinate derivation and branch ownership

**Original-target finding, static only.** Déraciné 1.00 SELF, 81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` rechecked in this run. All addresses below are **inner ELF virtual addresses**, not loaded guest addresses, SELF offsets, GPU VAs, or shader hashes. For this executable PT_LOAD#0 only, original SELF offset = VA + `0x1FD30`.

## New independently decoded upstream edge

Function entry `0x14002A0` (SELF `0x142FF D0` without space: `0x142FFD0`) saves incoming `rdi` in `r15` and `rsi` in `r14`. At `0x140036A` and `0x1400370` it reads two packed 32-bit coordinate pairs from `[r13+0x78]` and `[r13+0x70]`. The two lanes of the latter are separately divided by `esi` (derived from an earlier integer division), then repacked at `0x1400385..0x1400389` into `rdi`, saved at `[rbp-0x48]` by `0x14003A0`. The former pair is independently transformed using `(scale + coordinate - 1) / scale` at `0x140038C..0x14003A8`, repacked in `rcx`, and stored at `[rbp-0x40]` by `0x14003AB`. This is integer downscaling of two packed coordinate pairs, with ceiling-like rounding on the second pair when operands are positive; signed division means general signed rounding semantics must not be assumed.

The same caller at `0x140042E` tests `dword [r15+0xD8]` and **selects two distinct setup paths**:
- nonzero: `0x1400438..0x1400456` passes `rcx=&[rbp-0x48]`, `r8=&[rbp-0x38]`, `rdi=r15`, `rsi=r14`, `rdx=r10` and calls `0x1404710`;
- zero: `0x140045D..0x140047B` sets up the same argument register shapes and calls `0x1400A40` (A-016's verified viewport producer).

The **new direct call** is at ELF VA `0x140047B`, SELF offset `0x14201AB`, bytes `E8 C0 05 00 00`; decoded target `0x1400A40`. A bounded scan of `E8 rel32` candidates over executable SELF PT_LOAD#0 returned this single direct caller; that scan is discovery only, with the instruction boundary and surrounding argument setup independently decoded from `0x14002A0`.

At `0x14003AF..0x140042B` a separate float is compared against four constants at `0x35B0328..0x35B0334` to compute an integer tier in `r9d`; this is not sufficient to name a render-quality or eye-resolution setting.

## Interpretation, limits, and next work

**High confidence:** producer `0x14002A0` owns a scale-adjusted packed coordinate pair, passes it by address to A-016's `0x1400A40`, and chooses an alternate path `0x1404710` based on owner `+0xD8`. The original source of the `r13` object, scaling divisor, and the semantic distinction of the alternate path are not established. This is a concrete new command-list *upstream producer edge*, not proof of a stereo-eye viewport or of runtime reachability. Competing interpretations include resolution scaling, render-target subregion adjustment, or a generic rect/viewport quantization layer.

**Next:** decode `0x1404710` and compare its payload/owner-cache behavior to `0x1400A40`; trace the producer of `[r15+0xD8]` and of packed input `[r13+0x70/+0x78]`.

Reproduction: verify SELF SHA-256; seek executable VA + `0x1FD30`; disassemble `[0x14002A0,0x14004A4)` using `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x14002A0`; inspect the exact call `0x140047B` and argument-defining basic blocks. No proprietary bytes or paths are committed.
