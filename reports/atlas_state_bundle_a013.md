# A-013 — Conditional graphics-state bundle writer at 0x03090310

**Original target:** Déraciné PS4 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Verified in this investigation. **Static evidence only**; no claim of live execution.

## New original-byte finding

The A-012 adjacent direct call at ELF VA `0x0CC28E9` passes `[context+0x460]` as RDI (writer) and RBX as RSI (possibly null resource/state descriptor) into `0x03090310`. Independent x86-64 disassembly now establishes that the callee is a **conditional multi-register graphics-state bundle writer**, not merely an unknown helper.

* The callee preserves writer in R14 and descriptor in RBX. Null descriptor selects **6 DWORDs**; nonnull descriptor is tested by two different direct predicate calls, `0x0308FE70` and `0x03090090`. If either returns nonzero, it selects **24 DWORDs**; otherwise it selects **6 DWORDs**. The first predicate checks descriptor `+0x00` and `+0x08`; the second checks `+0x04` and `+0x0C`, including bitfield/table calculations. These are actual target function bodies, not inferred source symbols.
* It computes writer remaining capacity from `[writer+0x08] - [writer+0x10]` divided by four, then calls writer callback `+0x18` with requested DWORD count and `[writer+0x20]` if insufficient. Callback failure returns without emission.
* When either predicate succeeds, it emits exactly **96 bytes**: a first 40-byte block with 8-byte command `0x00000010C0086900` and 32 descriptor bytes, then four 12-byte command/value blocks (values from descriptor `+0x2C`, `+0x20`, `+0x24`, `+0x28`), then an 8-byte final command/value pair using descriptor `+0x30`. The explicit cursor ends at `+0x60`.
* On null descriptor or two false predicates, it emits a **24-byte fallback**: a 16-byte constant copied from ELF VA `0x03590EA0` and a following 8-byte immediate `0x11`. The cursor ends at `+0x18`.

The four 12-byte commands are `0x0000000FC0016900`, `0x00000002C0016900`, `0x00000005C0016900`, `0x000002AFC0016900`. These are original CPU command-writer values, **not** assigned PS4 register names. Descriptor bytes `+0x00..+0x1F` are transferred by AVX as one 32-byte block. This is a strong static connection between higher-level descriptor state and a group of graphics-state emissions. The predicate tests are not yet semantically named; graphics pipeline state (possibly target format/layout) is a hypothesis.

## Reproducibility and address provenance

The listed file offsets are **original SELF offsets**, derived using the locally validated code-region mapping `SELF offset = ELF VA + 0x1FD30`; neither addresses nor offsets represent loaded guest addresses, relocated table slots, PS4 GPU VAs, or shader hashes.

| ELF VA interval (exclusive end) | Original SELF offset | Bytes | SHA-256 |
|---|---|---:|---|
| `0x03090310–0x03090450` | `0x030B0040` | 320 | `e35d804f8350bb045e6911cf54accca9133c234138e19e5d6c20fc9f025316cc` |
| `0x0308FE70–0x0308FF20` | `0x030AFBA0` | 176 | `631bef171c277bfeb77eae95f819f3b1d498bea300bb3e260472b2c6f0cebc17` |
| `0x03090090–0x03090130` | `0x030AFDC0` | 160 | `40b3256cb1c5bc06086f9b78caef27476f843009914f0d1f6dd6957ed7610ae3` |
| `0x0CC28DF–0x0CC28EE` | `0x0CE260F` | 15 | `7a51ea6d2972e9171ede0c45572f0043b6a2db221758ef1bf47e320dcd7faf87` |

Instruction-boundary-confirmed GNU objdump x86-64 Intel disassembly starts at the prologue `0x03090310`, and shows direct call edges `0x03090325→0x0308FE70`, `0x03090336→0x03090090`, `0x03090380→0x0308FE70`, `0x0309038C→0x03090090`, and caller `0x0CC28E9→0x03090310`. Conditional branches reach `0x03090422` (fallback) and `0x0309044A` (return). `0x0309044E` is RET and `0x03090450` begins another prologue.

**High confidence:** binary-backed function boundaries, direct edges, predicate-controlled 6/24-DWORD selection, writer capacity/growth ABI, exact payload construction. **Unresolved:** descriptor C++ class and ownership, predicate meanings, register identity, GPU behavior and runtime reachability. The output should not be treated as Vulkan pipeline state without semantic recovery.

**Next:** trace construction/field producers of the descriptor at `+0x00..+0x30` and caller RBX provenance; independently resolve constant table `0x03590EA0` and predicate classification before naming the emitted state.