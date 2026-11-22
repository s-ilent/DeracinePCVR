# A-021 — Construction and reference ownership of the 0x03EDFCA0 command node

**Status:** verified original SELF static finding; no runtime reachability. Source: Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed 2026-11-22 UTC. Code SELF offset = ELF VA + `0x1FD30` only in the inspected executable segment; RELRO table SELF offset = VA + `0x21240`. No loaded guest or GPU address implied.

## New construction evidence

A complete instruction-boundary decode of the producer block `0x1975841–0x19758D7` establishes a deferred **command-list node** using the previously known table `0x03EDFCA0`. This is a stronger ownership attribution than the A-020 cleanup analysis, and corrects any interpretation of that table as necessarily a live graphics-context object's own vtable.

- At `0x1975841–0x1975883`, the producer obtains an aligned arena allocation and checks `object+0x30` against `object+0x38`; if insufficient it calls `0x0E3EF80` requesting `0x38` bytes. The aligned node advances the arena cursor by `0x30` bytes, so **0x30 is the observed node footprint** and 0x38 is the growth reservation including alignment slack.
- At `0x1975886`, `lea [rax+8]` forms the new link-tail address; `0x1975891` reads the current tail slot, `0x1975895` writes the new node into it, and `0x1975898` sets the tail to `&node.Next`. At `0x197589C`, `[node+0x08]=0`; `0x197588A` resolves vtable VA `0x03EDFCA0` via RIP-relative LEA, and `0x19758A4` writes that vptr to `[node+0x00]`. This is the same `{vptr,Next}` command-header convention as A4's deferred RHI commands.
- The producer sets `node+0x10` from local `[rbp-0x70]`, `node+0x18` from a 16-bit input `[rdx+0xC0]`, `node+0x20` from `[r13+0x08]`, and `node+0x28` from `[r13+0x10]` (`0x19758A7–0x19758C2`).
- When `node+0x28` is non-null, `lock incl [node+0x28+0x08]` at `0x19758C8`. Independently, `0x19758D0–0x19758D4` loads `[[node+0x20]+0x10]` and increments its `+0x08` count. These two increments corroborate A-020's later decrement/release operations on nested references. They do **not** by themselves establish whether the counts are shared_ptr, custom intrusive handles, or another lifetime protocol.
- RELRO address point `0x03EDFCA0` (SELF `0x03F00EE0`) holds `0x0CACDA0`; its +8 slot holds `0x0CACE70`. A-020 decoded the first function's conditional graphics-state emission, cache update and nested releases. **This new producer demonstrates that the table is installed on a queued node**, not merely a context object. The first method's precise C++ destructor/Execute name remains unresolved.

## Byte-level anchors

| ELF VA | Original SELF offset | Checked bytes | Meaning |
| --- | --- | --- | --- |
| `0x197588A` | `0x19955BA` | `48 8D 15 0F A4 56 02` | RIP LEA => `0x03EDFCA0` |
| `0x19758A4` | `0x19955D4` | `48 89 10` | write vptr |
| `0x19758B7` | `0x19955E7` | `48 89 48 20` | write node +0x20 |
| `0x19758C8` | `0x19955F8` | `F0 FF 42 08` | atomic reference increment |
| `0x0CACE4B` | `0x0CCCB7B` | `48 8D 05 4E 2E 23 03` | previous A-020 cleanup vptr restoration |

**Confidence:** high for queue insertion, node size/field offsets and two reference increments; medium for matching source-level command class and whether `0x0CACDA0` is an Execute/destructor hybrid. No runtime trace of this node. A competing interpretation is a generic queued cleanup command rather than a graphics-state command proper.

## Reproduce

Hash the private SELF, then read code at `ELF_VA+0x1FD30` for `0x1975841..0x19758D7`, disassemble from the stated instruction boundary with x86-64 `objdump -D -b binary -m i386:x86-64 --adjust-vma=0x1975841`. Read RELRO qwords at `0x03EDFCA0+0x21240` and `+8`; confirm `0x0CACDA0,0x0CACE70`. Cross-check the A-020 report on the same research branch for the complementary release path. No game bytes or proprietary logs are included here.

**Next:** recover the enclosing producer FDE, the origin of `r13+0x08/+0x10`, and determine whether the node's first virtual method executes during list traversal or is invoked only during teardown. The node's `+0x10` pointer and `+0x18` slot remain untyped.
