# C-025 — CPU keyed registry insertion and record allocation

Original PS4 SELF 1.00, size 81,506,767, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed before analysis. **All addresses are original inner ELF VAs**, original SELF file offset = ELF VA + `0x1FD30` for the verified intervals. No guest loaded address, GPU VA, shader hash or Vulkan descriptor binding is inferred.

## New byte-verified producer/consumer edge

At ELF `0x0CC8558`, a caller takes the address of enclosing context `+0x3D8` into RBX. It calls `0x03091BF0` with a nested object `+0x20` (`0x0CC8560..0x0CC8564`), shifts the resulting DWORD left by eight, stores the resulting 64-bit key at stack `+0x58`, and passes **RCX-independent arguments** RDI=&context+0x3D8 and RSI=&stack key to direct callee `0x0CC93D0` at callsite `0x0CC857C`. This is an actual producer of entries for the CPU keyed registry, not merely another lookup.

The instruction-boundary-decoded `0x0CC93D0` entry:
- `0x0CC93E4..0x0CC941B`: when local `+0x34` is nonzero, pops a recycled index from `+0x30` using 16-byte records at `+0x00`; otherwise `0x0CC941D..0x0CC9441` increments `+0x08` and grows capacity `+0x0C` via `0x0CC1EC0` if needed.
- `0x0CC9446..0x0CC949A`: chooses heap or inline bitset at local `+0x20/+0x10`, marks the allocated index, writes the incoming 64-bit key to record `+0x00`, and initializes signed next index `+0x08` to -1.
- `0x0CC94A3..0x0CC9575`: hashes the key with seed `0x9E3779B9`, chooses heap/inline buckets at local `+0x40/+0x38`, masks by `+0x48-1`, traverses signed 16-byte record links and compares the stored 64-bit key. The duplicate-key branch updates the existing record and enters recycling logic; **it is not safe to call the table a unique resource owner without the remaining CFG**.

Because RDI is `context+0x3D8`, the local offsets map to context: records `+0x3D8`, record count `+0x3E0`, capacity `+0x3E4`, bitset `+0x3E8/+0x3F8`, free head `+0x408`, free count `+0x40C`, buckets `+0x410/+0x418`, bucket capacity `+0x420`. This independently joins C-022 lookup, C-023 reset, C-024 removal with a **concrete insertion/allocator**. It also explains why C-024 increments `+0x40C` on removal: this is a recycled/free-index count, not a live-entry count.

**Reproducibility:** verified caller interval ELF `[0x0CC8558,0x0CC8581)`, SELF `[0x0CE8288,0x0CE82B1)`, SHA-256 `7d7d41460b940e0ef93b78da0e0c62dd09395d549ecf8537e2721f77672af7ed`; verified exact callee interval ELF `[0x0CC93D0,0x0CC95B8)`, SELF `[0x0CE9100,0x0CE92E8)`, SHA-256 `70f50e7c6fa211b9374705d3254521a2aa2b70864b034219430e20e926f8710f`. Decode from entry with `objdump -D -b binary -m i386:x86-64 --adjust-vma=0xCC93D0`. Caller instruction range `0x0CC8558..0x0CC8581` independently decoded in original bytes. Exact FDE and constructor/initial bucket allocation not yet established. Confidence high for insertion and 16-byte record/free-list/bitset semantics, medium for resource-deduplication interpretation. No runtime execution claimed.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can now distinguish a CPU descriptor-derived key insertion from GPU descriptor versions, shader identity and guest resource byte backing. Key generation uses `0x03091BF0` output shifted eight bits, not a GPU virtual address. Test whether this path is reached by shader-state changes; do not assume it.

**Next precise target:** decode the remainder of `0x0CC93D0` through its return, especially duplicate-key handling and bucket rehash/growth, then identify the FDE and constructor initializing context `+0x410/+0x420`.
