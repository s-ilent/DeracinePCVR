# A-016 — Viewport producer's source and preceding arena operation

**Status:** original 1.00 SELF static finding; no runtime reachability or per-eye attribution. Original SELF size 81,506,767, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` independently checked in this run. Previous A-015 established viewport command construction at 0x1400ACF–0x1400B5B; this finding concerns its **upstream input contract and a separate preceding arena operation**.

## Independently decoded evidence

The function starts at ELF VA `0x1400A40` (original SELF offset `0x14207870`, executable PT_LOAD#0). Its prologue preserves `rsi` in `rbx`, `rcx` in `r14`, and `rdx` on stack. At `0x1400A76`, it takes `rbx+0x28`; `0x1400A8A` dereferences that pointer to get an arena-like object. `0x1400A93` loads the qword pointed to by the incoming `rdx`; `0x1400A96` clears `edx`; `0x1400A98` calls `0x13D67E0` with `rdi=*(rbx+0x28), rsi=*(incoming_rdx), rdx=0, cl=0`. This call **precedes** the viewport command allocation and is not the viewport Execute function.

The helper `0x13D67E0` is a genuine arena/list operation, not a GPU draw or viewport setter: `0x13D681D` selects `r12=rbx+0x30` when `cl=0`; the alternative path at `0x13D683A–0x13D68B1` reserves space for up to two qwords and copies pointer values; `0x13D68BD–0x13D68E8` aligns a cursor and reserves a further `0x28`-byte node, using `0x0E3EF80` for growth. Because the caller passes `cl=0`, the conditional copy branch is **not** asserted to execute at this callsite. This distinguishes an upstream arena-preparation operation from the later direct viewport node construction.

At `0x1400A9D` the producer reads the first 64-bit word from incoming `rcx`, and at `0x1400AA5` reads the second at `rcx+8`. It splits both into low/high dwords and **also writes the four 32-bit components to the owning `rbx` object**: `0x1400AAC` -> `rbx+0x30`, `0x1400ABB` -> `rbx+0x34`, `0x1400ABF` -> `rbx+0x38`, `0x1400AC2` -> `rbx+0x3C`. A-015 already verified that the same components become the deferred viewport payload at node offsets +0x10/+0x14/+0x1C/+0x20, with fixed depth 0 and 1. This establishes a **dual destination** for the viewport coordinates: a four-dword owner cache and the deferred command. The cache's exact consumer and C++ class are unresolved.

## Address domains and confidence

All addresses above are **ELF VAs**. For the inspected executable segment only, original SELF file offset = VA + `0x1FD30`. The producer entry offset is `0x14207870`; helper entry `0x13F6510`. Neither is a loaded guest VA, GPU VA, shader hash or relocated vtable address. SHA-256 of original SELF windows: producer `[0x1400A40, +0x120)` = `1a68db2ecfffc94953c98dbeb892765e5cd15b9cd905968f24f9feb478ff7398`; helper `[0x13D67E0, +0x110)` = `8da0217846989e6c6bd64350846a89fc6eb0f4006bcd6682d054442bc29f69ad`. Disassembly: `objdump -D -b binary -m i386:x86-64 --adjust-vma=<VA> <window>`; windows extracted with seek at VA+0x1FD30.

**High confidence** in the call edge, two-word input, four cache writes, and conditional helper branch; **low confidence** in any semantic naming beyond arena preparation/viewport-coordinate cache. A possible interpretation is an owner-side viewport cache used for subsequent comparisons; another is a pass-setup state snapshot. No evidence here identifies stereo eye or runtime execution.

**Next:** trace readers of owner +0x30..+0x3C and callers passing the 16-byte coordinate pair into `0x1400A40`; seek a second independently decoded owner-use path before assigning a view/eye type.
