# C-012 — Paired descriptor-normalizing callers of CPU transfer entries

Original Déraciné 1.00 SELF: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed. Static-only original-byte analysis. For these executable windows SELF file offset = ELF VA + `0x1FD30`; neither offset is a loaded guest address, GPU VA, shader hash or Vulkan binding.

## New finding

Two *separate, instruction-boundary-confirmed* entry points, ELF `0x9460` and `0x97D0`, normalize a region descriptor from incoming `RDX` before calling the previously identified opposing-direction transfer APIs `0x91C0` and `0x9530`. These are the only direct E8 callers of those two entry points found by a bounded scan of mapped executable-address candidates through ELF VA `0x03400000`; this negative result does not exclude indirect calls or code outside the scan.

The first wrapper reads descriptor DWORDs `[RDX+0x08]`, `+0x0C`, `+0x10`, byte `+0x2C`, and selector DWORD `+0x28`. It constructs a local descriptor at `[RBP-0x30]`: zeroes its first eight bytes, places source +0x08/+0x0C/+0x10 into local +0x0C/+0x10/+0x14, respectively. If the byte at +0x2C is nonzero, the selector is decremented and range-checked against 15, then an original relative jump table at ELF `0x33CAD10` selects normalization. Confirmed branches round the two dimensions upward to multiples of four (`0x94C8..0x94E8`) or the first dimension upward to multiples of eight (`0x94F1..0x9504`). Both paths calculate `R9D=R8D*R9D` at `0x9504`, put `RCX=&local` at `0x9508`, then call `0x91C0` at `0x950C`.

The second wrapper repeats the same field access and layout at `0x97FD..0x9817`, with a *different* relative jump table `0x33CAD90`, the corresponding four/eight normalization at `0x9838..0x9874`, `R9D=R8D*R9D` at `0x9874`, `RCX=&local` at `0x9878`, and direct call `0x987C -> 0x9530`. Both wrappers check incoming RDX for null and return status `0x80D30000` on that path, and both snapshot/check the same global guard pointer through RIP-relative slot `0x4380AE0`; the guard's actual identity is unknown. Both include an out-of-range selector path returning the same status.

The *new* producer/consumer fact is that an original incoming descriptor's +0x08/+0x0C/+0x10 and format/normalization selector +0x28/+0x2C are converted into a stack-local region descriptor and forwarded as **fourth argument RCX** to the C-011 transfer entry, while RDX still supplies the original layout descriptor. This does not establish whether the first two pointers are CPU source/destination, guest buffers, texture storage or GPU addresses. The two wrappers' distinct jump tables must not be assumed identical in every selector case without enumerating all 16 entries.

## Reproducible evidence

- Entry `0x9460`, SELF `0x29190`, 200-byte window SHA-256 `6807f100194df049271939d4d92026d1a92bbcfaada1b2a7108a00fd9c70e4b4`.
- Entry `0x97D0`, SELF `0x29500`, 200-byte window SHA-256 `5a31822fba57362a4b65ee594364e8e1248a8ab2e7649f63e54d0c8bf8245c6f`.
- Decoded with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma` from both entry boundaries; direct E8 targets and paired field accesses checked by `tools/atlas/verify_transfer_wrappers_c012.py`. FDE boundaries and relocation records for the guard slot are **not** established here. The jump-table addresses are RIP-relative instruction results, not independently verified relocations.
- Confidence **high** for entry/field accesses, normalized local descriptor and paired direct calls; **medium** for interpreting normalization as region/format conversion. Competing interpretation: generic tiled/swizzled CPU image conversion rather than shader-resource or UE4 RHI buffer ownership. No runtime reachability claim.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-10/SC-11 can distinguish the original descriptor in RDX from the normalized local region descriptor in RCX and check +0x28/+0x2C selection when investigating resource byte layout. SC-09 must not equate this with shader SGPR user data or a Vulkan descriptor binding.

**Next precise target:** enumerate all 16 branch targets of the two original relative tables `0x33CAD10` and `0x33CAD90`, then identify the producer/owner of descriptor +0x28/+0x2C or pivot to the shader-resource virtual +0x38 producer from C-001.
