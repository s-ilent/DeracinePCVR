# C-008 — Original SELF resource-layout producer with three distinct consumers

**Target:** Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly checked. Static only. For all windows below, original SELF offset = ELF VA + `0x1FD30`. No loaded guest address, relocated slot, PS4 GPU VA, shader hash or runtime reachability is inferred.

## New original-target producer/consumer relationship

ELF `0x5760` is a **resource-layout derivation function**, with an input descriptor in RSI and output record in RDI (preserved in R14 and RBX). It checks input non-null, calls validator `0xAD0` with a zero-initialized local and the source descriptor, and branches on the validator status before writing the output. Its fields are *not* yet attributed to a UE4 FRHI resource or PS4 GPU buffer.

The verified successful path copies source `+0x04` to output `+0x00`, source `+0x00` to output `+0x04`, and source `+0x08/+0x0C/+0x10/+0x14/+0x28` to output `+0x0C/+0x10/+0x14/+0x44/+0x24` respectively, with conditional transformations. The output `+0x08` comes from validator-produced local `[rsp+0x38]`; it is not a direct source copy. For `output[+0x08]-4 <= 11`, a 12-entry lookup at ELF `0x033CE400` supplies output `+0x60`.

The input byte `+0x2C` gates a 16-case jump-table path (`0x33CAAB0`, index `source[+0x28]-1` with unsigned limit 15). Some cases modify output dimensions. The common path at `0x58AF..0x58C7` computes an integer size from dimensions, an additional factor and rounding: `((r13d * r8d * eax * r9d * r11d) + 7) >> 3` in 32-bit arithmetic, stored as a zero-extended QWORD at output `+0x28`. This is a calculated size-like quantity, **not** a proven allocated byte count or GPU residency size. The input source also supplies a QWORD through a stack temporary to output `+0x30`.

The branch at `0x58DB` selects `0x3340 -> 0x34C0` when `source[+0x04]==1`, or `0x3050 -> 0x3290` otherwise. These are real direct-call edges. Both paths converge at `0x5944` and construct four power-of-two output fields `+0x48/+0x4C/+0x50/+0x54` from returned local byte shift counts; `+0x50` uses base 2, the other three base 1. Later `0x59D7..0x59E4` clamps a calculated quantity to at most `0x400` in output `+0x58`; `+0x6C` is assigned `0x100`. This establishes a multi-field layout builder, not merely a format lookup.

Three independently instruction-boundary-decoded callers consume its output differently:

| Callsite ELF VA (SELF offset) | Source descriptor | Output record | Immediate next consumer |
|---|---|---|---|
| `0x92CA` (`0x290FA`) | `[rsp+0x60]` | `[rsp+0xA0]` | `0x6190` at `0x92E5` |
| `0x963A` (`0x2936A`) | `[rsp+0x60]` | `[rsp+0xA0]` | `0x6F40` at `0x9655` |
| `0x999A` (`0x296CA`) | `[rsp+0x20]` | `[rsp+0x60]` | `0x5B90` at `0x99BF` |

Each caller has an earlier `0x3020` extraction and checks its status. The first two use 16-case dispatch tables `0x33CACD0` and `0x33CAD50`; the third checks extracted format index `[4,15]` before invoking the builder. These are **three static construction-to-consumption edges**, not evidence of three live resource classes.

## Reproduction, confidence and limits

Original-SELF windows SHA-256:
- ELF `0x5760` length `0x360`, SELF `0x25490`: `d357d8abda4311c9257b796ba7f59e10e198b1099e888d97532abeed77494fc6`
- ELF `0x9274` length `0x75`, SELF `0x28FA4`: `c30c3b7f01c55b02d23b32956bf17e9646c2d0d726e6cf7f164126a112cc444a`
- ELF `0x95E4` length `0x76`, SELF `0x29314`: `2303236efac21fc7fd139078ed8bc4e1401c56b444aef06b92e5bb8f382656bd`
- ELF `0x994A` length `0x86`, SELF `0x2967A`: `68daf4bb2da19a77e7c647c2cd886c7213b6b24c5d33b28432dc148b2e296863`

Verifier: `tools/atlas/verify_resource_layout_c008.py`. Original SHA, six window digests and six relative-call targets checked; GNU objdump independently confirms instruction boundaries. FDE coverage and relocation semantics have **not** yet been established. Confidence **high** for the field accesses, branches, direct call edges and arithmetic; **medium** for generic layout/size terminology; **unresolved** for ownership, lifetime, UE4 identity, stage and GPU meaning. The three callsites could be different operations on the same layout type.

**Transfer to SC-09/SC-10/SC-11 or FD4:** if SC-09 sees a candidate guest resource descriptor with matching input fields, test the `0x5760` output layout and `0x6190/0x6F40/0x5B90` consumers separately. Do not equate `+0x28` or `+0x58` with actual resident GPU bytes, a shader hash, or a Vulkan binding.

**Next precise target:** decode the three immediate consumers `0x6190`, `0x6F40`, `0x5B90` sufficiently to establish whether output `+0x28` and `+0x48..+0x58` control guest buffer sizing, transfer addressing or a pure validation/layout calculation; confirm FDE boundary of `0x5760`.
