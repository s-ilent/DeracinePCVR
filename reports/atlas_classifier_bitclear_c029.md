# C-029 — original classifier 0x0308FE70 clears a derived bitfield mask

Original Déraciné 1.00 SELF: 81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` independently verified in this run. All addresses below are inner ELF VAs; in these executable/table windows, original SELF offset = ELF VA + `0x1FD30`. No runtime reachability, guest loaded address, PS4 GPU address, shader hash, or Vulkan binding is implied.

## Independently decoded finding

The C-028 producer at `0x0CC2821` calls `0x0308FE70` on `[r8+0x28]` and shifts its return left eight before insertion into the CPU registry at context `+0x3D8`. The **new result** is that `0x0308FE70` is not an opaque key classifier: it returns a **modified bit mask derived from input DWORD +0x08**, optionally clearing a contiguous bit range computed from input descriptor fields.

Instruction-boundary decoding of `[0x0308FE70,0x0308FF75)` establishes:
- `mov r14d,[rbx+8]` seeds the eventual return; `test byte [rbx],3` skips mutation when the low two bits are zero.
- `bextr edi,eax,0x314` extracts a 3-bit field starting at bit 20, passed to predicate `0x0AB0`; false also preserves the original DWORD.
- Table `0x035AF480` indexed by `[rbx]&3` has DWORD values `[0,2,0,4]`; its low byte indexes another DWORD table `0x03752FB0`. The latter table is not exhaustively characterized here.
- `[rbx+0x2C]&0x1F00 == 0x1200` chooses helper `0x03450` and shift 4; other values choose `0x03220` and shift 3. Both helpers receive a stack output pointer and a single-bit value computed from bits 2–3 of input DWORD 0.
- The returned stack DWORD supplies the bit-mask width: `((1 << (width+1))-1) << shift`, with x86 shift-count masking, is removed by `andn r14d,edx,r14d`. Final `mov eax,r14d` returns the surviving bits.
- The separate `0x0308FF80` routine has a different contract (conditional extraction from `[rbx+8]`, then `shr eax,4`); do not conflate adjacent routines.

The 0x0308FE70 return is therefore a **masked DWORD**, not a stable resource identity by itself. Its shifted result is inserted as a CPU registry key by C-028; this does not establish what the original resource or shader stage represents. The mask's exact width depends on helper outputs; no complete range or shader-stage identity is asserted.

## Reproduction

ELF `[0x0308FE70,0x0308FF75)` = original SELF `[0x030AFBA0,0x030AFCA5)`, SHA-256 `449647116815bce4fcb71bc71429712c1643a3c90cd1a405d679afefc6da45c7`. Four-DWORD selector table at ELF `0x035AF480`, SELF `0x035CF1B0`, SHA-256 `c175f659bff12175bcddd6acf5cd0ed07b0c95d85a9b7032eaf87309d02ff504`. The first eight DWORDs of secondary table at ELF `0x03752FB0` (SELF `0x03772CE0`) have SHA-256 `99b8b86019a500cd5528f571eafa811cfded3c10001ff9f565c78fb4319fa8ce`.

Reproduce with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x308fe70` on the private 0x105-byte slice. The read-only verifier checks SHA, table values, instruction anchors, and the three direct call targets. Confidence high for conditional bit-clear and the CPU registry join; medium for width-helper interpretation; low for shader/material meaning. Alternative interpretation: generalized packed format/usage-bit validation rather than shader-stage binding.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 should not interpret the C-028 registry key as a shader hash or GPU resource version; it is a shifted, masked CPU-side descriptor DWORD. This supplies a concrete static falsification check for proposed shader-resource identity joins.

**Next precise target:** decode the input object producer of `[r8+0x28]` at `0x0CC2816` and independently determine the output-width contracts of `0x03220/0x03450`, then test whether the resulting bit-clearing contract corresponds to a shader descriptor usage mask.
