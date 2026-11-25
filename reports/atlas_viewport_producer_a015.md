# A-015 — Deferred viewport command construction from packed coordinates

**Status:** original-target static finding; no runtime reachability established.

Input: original 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly verified before inspection. Git baseline: atlas branch after A-014; A4 corrected command identities are prior evidence, not new discoveries.

## New evidence

At ELF VA `0x1400ACF–0x1400B5B` (original SELF offset = ELF VA + `0x1FD30`, for this inspected code segment), a producer reserves a 0x28-byte aligned command node in a command-list arena:

- `[r13+0x30]` is an arena cursor; it is rounded up to eight-byte alignment, and the requested end is `aligned_cursor+0x28`. `[r13+0x38]` bounds the reservation; the slow path invokes `0x0E3EF80` with size `0x30` before reloading the cursor. `[r13+0x14]` increments after reservation. These are *observed field uses*, not a proven complete arena class.
- At `0x1400B1E`, a RIP-relative LEA resolves **ELF VA** `0x03F26A08`, the previously verified SetViewport-style command vtable. The producer writes `[rax]=vtable`, `[rax+0x08]=0`, appends `rax` through the tail pointer `[r13+0x08]`, and updates the tail pointer to `rax+0x08`. This independently connects the producer to the linked command execution model.
- Payload fields: `[rax+0x10]=r12d`, `+0x14=r14d`, `+0x18=0`, `+0x1C=edx`, `+0x20=r15d`, `+0x24=0x3F800000` (float 1.0). Earlier instructions at `0x1400A9D–0x1400AC2` split two 64-bit source words into four 32-bit halves: `r12d/r14d` from one and `edx/r15d` from the other. Thus the producer constructs a six-component viewport with depth interval **[0.0, 1.0]**, while four integer coordinates are supplied from two packed words.
- Prior A-014 independently identified vtable Execute `0x13A6BB0`, its six-field forwarding, and context method `0x0CB8AE0` with state-cache comparison and conditional writer emission. A-015 adds the **previously missing construction and packed-coordinate provenance**; it does not repeat the context writer as a new result.

## Address provenance

| Object | ELF VA | Original SELF offset |
|---|---|---|
| packed-coordinate split | `0x1400A9D` | `0x14207CD` |
| arena reservation | `0x1400ACF` | `0x14207FF` |
| vtable LEA | `0x1400B1E` | `0x142084E` |
| vtable address point | `0x03F26A08` | `0x03F47C48` (RELRO segment) |
| payload writes | `0x1400B3F–0x1400B55` | code VA + `0x1FD30` |

The source words are CPU-side values, not PS4 GPU virtual addresses or shader hashes. Loaded guest addresses require relocation/base treatment. **Note:** use the verifier's exact arithmetic for the LEA original offset; never apply the code-segment delta to the RELRO vtable.

## Interpretation and limits

High confidence for command-node allocation, linked-list insertion, viewport payload, and fixed depth interval. The packed coordinate source's owning C++ type, the caller's eye/view identity, and whether the operation runs at runtime remain unknown. The producer could be a generic viewport setup or a pass-specific reset; no per-eye inference is justified. The allocator's `0x30` request versus `0x28` node extent may reflect alignment/slack, not a 0x30-byte payload.

Reproduce: `python tools/atlas/verify_viewport_producer_a015.py <private-original-SELF>`. Decode the original bytes around ELF VA `0x1400A9D–0x1400B5B` on x86-64 instruction boundaries. The verifier checks the source hash, packed-word splits, arena operations, LEA target, list linkage, payload stores and pre-existing Execute target. No original executable is committed.

Next: trace callers of this producer and the two packed coordinate source words, then determine whether separate per-eye viewport producers exist without inferring from the presence of VR code.
