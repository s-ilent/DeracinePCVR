# C-022 — Resource descriptor fallback and CPU-side keyed duplicate lookup

**Input:** original Déraciné PS4 SELF 1.00 (81,506,767 bytes), SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed in this run. Addresses below are **inner ELF VAs**; verified SELF file offsets are ELF VA + `0x1FD30`. They are not guest loaded addresses, GPU VAs, shader hashes or Vulkan bindings.

## New finding

The C-021 second resource stream's direct callee `0x0CAC030` does not simply write a descriptor. Its decoded entry establishes a **three-way resource descriptor fallback** and then performs a **CPU-side keyed lookup in a 16-byte-entry chained table**, with a fast exit on a matching 64-bit key.

- At `0x0CAC05D..0x0CAC09D`, the non-null input object R14 selects descriptor source in priority order: `[R14+0x30]` (virtual `+0x38`, returned pointer +8), else `[R14+0x28]+8`, else `[R14+0x38]+0x20`. The selected DWORD is tested against 0 and 4, with further conditional handling for 2. The virtual return is **not** identified as a GPU address.
- At `0x0CAC151..0x0CAC16B`, the function compares state DWORDs `[R12+0x3E0]` and `[R12+0x40C]`, skipping the lookup if equal.
- At `0x0CAC16B..0x0CAC1F0`, it combines `[R14+0x14]` with the low twelve bits of `[R14+0x18]` into a 64-bit key and mixes that key with a fixed `0x9E3779B9` seed. This is a **CPU lookup key**, not a shader hash or GPU VA.
- At `0x0CAC1F0..0x0CAC23A`, table base is `[R12+0x418]` if nonzero, otherwise inline `R12+0x410`; bucket index uses `([R12+0x420]-1) & mixed_hash`. Bucket stores a signed 32-bit entry index, with -1 sentinel. Records at `[R12+0x3D8]+index*0x10` hold a 64-bit key at +0 and next index at +8. Matching key branches to `0x0CAC502` (a common exit). A second corresponding lookup exists at `0x0CAC270..0x0CAC336`.
- This establishes an original-target **resource identity/deduplication lookup**, not GPU buffer-cache versioning. Exact purpose of the keyed table (binding suppression, resource validation, or another CPU-side registry) remains unresolved; no live reachability is inferred.

**Instruction-boundary evidence:** decoded with `objdump -D -b binary -m i386:x86-64 --adjust-vma=0x0CAC030` starting at the function prologue. Window ELF `[0x0CAC030,0x0CAC23A)`, SELF `[0x0CCBD60,0x0CCBF6A)`, SHA-256 `6b3df5d65d173fc174c489b3c1da0c0baad9327b22e2d6ab36279d07d4dde4c2`. Lookup window ELF `[0x0CAC151,0x0CAC23A)`, SELF `[0x0CCBE81,0x0CCBF6A)`, SHA-256 `6ef67f7b05caac3d1f56db5dc619b6b213c6923ca705adec077691b4e4d75e70`. Second lookup ELF `[0x0CAC270,0x0CAC336)`, SELF `[0x0CCBFA0,0x0CCC066)`, SHA-256 `cfc1f51027aa3460b12edd65816b9fb04692bc26d2c2fdc30e53f0eb4cd600b0`. Direct producer callsite `0x0CC0222` is prior C-021 evidence, not a new discovery. FDE, table constructor and relocation edges not independently verified here.

**Confidence:** high for source priority, key arithmetic, bucket/chain field accesses and equality branch; medium for classifying the table as deduplication rather than validation or resource-state tracking. Exact UE class name unknown.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can distinguish this CPU-side key/table from the actual GPU buffer-cache version and from shader program identity; examine whether a shader-resource binding update with the same `(+0x14, +0x18 & 0xFFF)` key is suppressed by this lookup. This is a static hypothesis to test, not a runtime observation.

**Next precise target:** recover the constructor and mutation sites for context `+0x3D8/+0x410/+0x418/+0x420` to determine whether the keyed registry is a binding cache, hazard tracker, or generic resource registry.
