# C-027 — shared pointer-slot array allocator behind CPU registry bucket rebuild

Original PS4 SELF 1.00: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly checked. Addresses below are inner ELF virtual addresses. In these verified executable intervals original SELF file offset = ELF VA + `0x1FD30`. Loaded guest addresses, GPU virtual addresses, shader hashes, and Vulkan bindings are **not** inferred.

## New original-byte finding

Instruction-boundary disassembly from entry `0x0C7BD40` shows a **shared pointer-slot allocation/reallocation adapter**, not a shader binding writer or an owner of the inline/heap selection. The caller supplies RDI pointing to an 8-byte pointer slot and ESI as an element count; `mov r15,[r14+8]` at `0x0C7BD57` reads the current heap pointer. `cmp esi,1; ja 0x0C7BDEB` at `0x0C7BD54` divides zero/one-element disposal from allocation. On the allocation side `movsxd rbx,esi; shl rbx,2` at `0x0C7BDF2..0x0C7BDF9` converts element count to **4-byte element bytes**. The fallback allocator is a virtual call `[rax+0x20]` at `0x0C7BF1F` after loading global `0x044CFF60`; the other fallback is a direct call `0x0D6EB50` at `0x0C7C141`. The result is stored to the pointer slot at `0x0C7C149` (`mov [r14+8],r13`). The fast paths use global small-block structures `0x044AB298` and `0x044AB2A0`; do not assume a particular allocator implementation.

Two distinct, decoded callsites demonstrate the contract:

* `0x0CC9767`: registry rehash caller forms `r14=r12+0x38`, passes zero then bucket capacity `[r12+0x48]` to `0x0C7BD40` at `0x0CC9776` and `0x0CC978D`. Therefore the helper's stored pointer at argument `+0x08` is registry-local `+0x40`, context `+0x418`. Registry-local `+0x38` remains the **inline bucket storage** chosen by the caller, not the pointer that the helper overwrites.
* `0x0C8F000`: an independent global-array caller uses `lea rdi,[rip+0x381e018]` = `0x044AD018`, calls helper with ESI=0 at `0x0C8F00B`, then reloads count from `0x044AD028` and calls with that count at `0x0C8F02A`. Its pointer slot is `0x044AD020`. This independently establishes a reusable 4-byte-array allocation pattern, not a shader-specific routine.

The helper's `0x0C7BF99..0x0C7C309` small-block path includes linked free-list and copy operations; the exact small-block policy, alignment, and error behavior remain unresolved. Its complete FDE was not independently checked, so `0x0C7C15B` is a verified return site, **not** an asserted FDE end. The original instruction stream continues at `0x0C7C15C` into a branch target of the same routine. Do not truncate the function at its first return.

## Verification

Verified ELF interval `[0x0C7BD40,0x0C7C15C)` = original SELF `[0x0C9BA70,0x0C9BE8C)`, SHA-256 `330b4fd659c5358c0243a5a3ffa7fd32bd15b51db23bd978103e38823972b06a`. Decode from exact entry with `objdump -D -b binary -m i386:x86-64 --adjust-vma=0xC7BD40 -M intel`. Both callsites independently disassembled from original SELF; direct E8 targets checked by signed displacement and instruction boundaries. Verifier checks digest and key instruction windows. Confidence **high** for pointer-slot, count-times-four, two caller joins and global allocator fallback; **medium** for exact allocator ownership, because the function has additional paths after the first return. No live reachability claimed.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can classify context `+0x418` as CPU heap bucket-array backing managed through a generic allocator, not GPU buffer version or shader descriptor table. A context `+0x420` capacity change can reallocate this CPU array without any shader resource mutation.

**Next precise target:** find the registry/context constructor that initializes context `+0x410/+0x418/+0x420`, verify its allocation and reset paths, and compare with the `0x0CB87D0` registry reset. Separately complete `0x0C7BD40` FDE bounds and post-return branch targets before naming allocator policy.
