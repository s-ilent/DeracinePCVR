# C-023 — Original CPU resource-key registry reset path

Original Déraciné 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly verified. Inner ELF VA = original SELF offset minus `0x1FD30` in this verified window; no guest loaded VA, GPU VA, shader hash, or Vulkan descriptor is inferred.

## New independently verified finding

A distinct function starting at **ELF VA 0x0CB87D0** resets the exact keyed CPU registry consulted by C-022's `0x0CAC030` resource lookup. Instruction-boundary disassembly from the function prologue through tail jump proves:

- `0x0CB87D9`: clear `[RBX+0x3E0]` (current registry item count/state counter); if `[RBX+0x3E4]` nonzero, pass `RBX+0x3D8` and zero to direct `0x0CCBBC0` at `0x0CB87F5`.
- `0x0CB87FA`: set qword `[RBX+0x408]` to `0xFFFFFFFF`; `0x0CB8806`: clear `[RBX+0x400]`; if `[RBX+0x404]` nonzero, clear it and call `0x0C7C380` with `RBX+0x3E8` and zero at `0x0CB882C`.
- `0x0CB8831..0x0CB8888`: when signed `[RBX+0x420] > 0`, select bucket storage `[RBX+0x418]` if non-null, otherwise inline `RBX+0x410`. Write signed `-1` to the first bucket and iterate indices `i & (capacity-1)` for `i=1..capacity-1`, resetting every bucket sentinel. `0x0CB8890` tail-jumps to `0x0D014A0`.

This directly connects the **same four fields** `+0x3D8/+0x410/+0x418/+0x420` in C-022's keyed chained lookup to a concrete registry reset/clear routine, independently of any shader-stage or GPU address interpretation. The power-of-two capacity is **suggested** by mask indexing, not proven by this function alone. No insert/constructor or runtime execution is established.

Evidence: original SELF file window `[0x0CD8500,0x0CD85C5)`, ELF `[0x0CB87D0,0x0CB8895)`, SHA-256 `c02ccffb4bd519a3249d5dc2ddcec8518e3d2e710d12b15d7e95136fa0f1ec02`. Independently decoded with `objdump -D -b binary -m i386:x86-64 --adjust-vma=0xCB87D0`. The function is bracketed by prior `ret`/alignment and next entry at `0x0CB88A0`; FDE not independently checked. Confidence high for reset and field association; medium for interpreting `+0x3E0` as count; exact ownership and C++ identity unresolved.

Competing interpretations: generic hash-set reset, resource-binding deduplication cache reset, or resource validation/hazard registry reset. CPU refcounts and GPU buffer-cache versions are **not** established.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can treat `0x0CB87D0` as a concrete CPU registry invalidation/reset candidate, separate from GPU resource versioning and descriptor binding. Do not infer live use.

**Next precise target:** identify a write/insert producer for `+0x3D8` 16-byte key/next records and `+0x418/+0x420` bucket capacity, and recover its caller/constructor/FDE to distinguish cache from registry.
