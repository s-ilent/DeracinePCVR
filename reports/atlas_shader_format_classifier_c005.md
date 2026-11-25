# C-005 — Original PS4 descriptor format classifier and low-five-bit gate

Target: original 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Static only. In examined executable/data regions original SELF offset = ELF VA + `0x1FD30`; no loaded guest address, GPU VA or shader hash is asserted.

## New original-byte finding

The C-004 descriptor mask function at ELF `0x03091BF0` calls `0x0308F8F0` at `0x03091C34` and `0x03091C79`. The callee is an instruction-confirmed **three-input format classifier/packer**, not a simple validation predicate. The caller extracts from descriptor DWORD `+0x10` three fields using BEXTR controls `0x502`, `0x308`, `0x20B` (respectively bits 2..6, 8..10, 11..12) and passes them as EDI, ESI, EDX. It tests only AL after the first call; after the second call it zero-extends AL to select the 61-DWORD width table `0x03752FB0`.

At `0x0308F8F0` the first input selects one of four disjoint, literal 64-bit bitset families, tested in order by `bt` and `jb`. Family I mask `0x1800004000000016` selects the tables at `0x035AF3A0..0x035AF3D0`; family II `0x03FFF08000700828` selects `0x035AF3E0/3F0/400`; family III `0x04000107000120C0` selects `0x035AF410/420/430`; family IV `0x00000238000E5700` selects `0x035AF440/450/460/470`. The second extracted input (ESI) contributes `(ESI & 0xF)<<8`, except ESI=6 forces `0x900`. The third input (EDX) indexes four-element DWORD tables when EDX < 4; otherwise the family uses explicit default values. The output combines the first input's low byte, table words and the second-input field via OR at `0x0308FA32..0x0308FA56`. Thus AL is the low byte of a packed return value, and a nonzero AL does not imply any particular shader stage or resource validity.

The separate `0x00000AB0` callee is an exact **bitset membership test**: `mov eax,0x07F7DCDF; bt rax,rdi; setb al; ret`. The C-004 caller masks descriptor `+0x14` to five bits before this call (`0x03091C41..0x03091C47`). Accepted values of that five-bit field are exactly `{0,1,2,3,4,6,7,10,11,12,14,15,16,17,18,20,21,22,23,24,25,26}`. All other 0..31 values bypass descriptor-mask clearing. This is a stronger original-target constraint than the previous unknown predicate.

The four-family lookup tables were independently read from the original SELF; representative family-IV DWORDs at `0x035AF440` are `0,0x100000,0x140000,0x1C0000`, and the adjacent `0x035AF470` words are `0xE00000,0xE00000,0x800000,0x800000`. The classifier itself ends in `ret` at `0x0308FA56`. This maps an actual producer of the format-derived byte used in C-004's 18/3-DWORD selection, without claiming its hardware enum name.

## Reproduction and limits

- Classifier ELF `0x0308F8F0..0x0308FA60`, original SELF `0x030AF620..0x030AF790`, SHA-256 of 0x170 bytes: verify using companion script (the larger 0x190 inspection window SHA-256 is `73901cdca9bb7fd28fabc6ee1bdabb24897910c29d55441cf26f60514f175b49`).
- Bitset helper ELF `0x00000AB0..0x00000AC0`, original SELF `0x000207E0..0x000207F0`. Inspect instructions at `0xAB0..0xABC`.
- Tables ELF `0x035AF3A0..0x035AF480`, original SELF `0x035CF0D0..0x035CF1B0`; lookup `0x03752FB0` is already verified in C-002.
- Reproduce using `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=<ELF VA>`; inspect real original bytes, branch boundaries, table targets and return flow. Companion verifier checks original full SHA and independently mapped windows.

**Confidence:** high for exact instruction-level classification, family dispatch, table addressing, low-five-bit membership and C-004 consumer edge; medium for generic format-class interpretation. No proof of C++ class name, GPU register identity, shader hash, runtime execution or Vulkan resource binding.

**Transfer to SC-09/SC-10/SC-11:** captured guest descriptor `+0x10/+0x14` can now be checked against exact format-family membership and the 22 accepted low-five-bit values before comparing C-004's 72/12-byte state packet choice. Do not equate CPU descriptor classification with GPU buffer version or residency.

**Next precise target:** independently decode `0x00003220` and `0x00003450` and their stack-output contract to resolve C-004's final cleared-bit range in descriptor `+0x00`.
