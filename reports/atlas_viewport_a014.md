# A-014: deferred viewport command and cached PS4 context state

Status: VERIFIED_STATIC_NEW, original 1.00 SELF. No runtime reachability claim.

Original SELF size 81,506,767; SHA-256 8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0 independently verified. For inspected executable code only, SELF offset = ELF VA + 0x1FD30; for inspected RELRO vtable, SELF offset = ELF VA + 0x21240. These are not loaded guest/GPU addresses.

* RELRO vtable address point ELF VA 0x03F26A08 (SELF offset 0x03F47C48) contains Execute 0x013A6BB0 and destructor 0x013A6BE0.
* Execute at ELF VA 0x013A6BB0 (SELF offset 0x013C68E0) loads context from command list +0x20, and six payload fields at command +0x10/+0x14/+0x18/+0x1C/+0x20/+0x24. Four are integers, two are floats. Instruction-boundary decoded tail jump at 0x013A6BD3 reaches context method 0x00CB8AE0.
* Context method 0x00CB8AE0 (SELF offset 0x00CD8810) converts integer coordinates to floats and compares six inputs against cached context +0x80/+0x84/+0x88/+0x8C/+0xA0/+0xA4. Equal values branch toward 0x00CB8FC9 without following changed-state emission.
* Changed-state path 0x00CB8BC2 loads writer from context +0x460; 0x00CB8C1D checks available DWORD capacity against 11 and 0x00CB8C22 requests growth for 12 DWORDs. This is capacity management, not a semantic shader callback. Nearby constants and computed dimensions are emitted to the writer, but exact GPU register identities are not established.

Interpretation: strong SetViewport-style command boundary above PM4, with six scalar payload fields and context-level state deduplication. C++ name, coordinate edge convention, command constructor, cache writeback, complete FDEs, and per-eye producer remain unverified. No assumption about OpenXR resolution or runtime execution.

Reproduce with tools/atlas/verify_viewport_a014.py on the original private SELF; separately decode bounded executable windows with objdump -D -b binary -m i386:x86-64 -M intel. The checker validates SHA, vtable pair, Execute tail jump, context prologue, writer access and capacity gate. No proprietary bytes beyond short instruction assertions are published.
