# C-013 — Exhaustive selector semantics for paired CPU transfer normalizers

Original PS4 SELF 1.00 SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed this run. Static-only. Original SELF file offset = ELF VA + `0x1FD30` for these windows. ELF VA is **not** loaded guest VA, GPU VA, shader hash, or Vulkan binding.

## New original-byte finding

Both previously verified C-012 transfer-normalizing wrappers use **the same exhaustive 16-case selector partition**, although their separate signed-relative jump tables point into different executable functions. Incoming `[RDX+0x2C]` gates the switch; when zero, it bypasses selection and copies dimensions unchanged. Otherwise `[RDX+0x28]` is decremented and unsigned range-checked against 15, then a 32-bit signed offset is loaded from the table and added to its **table base** (not the individual entry address).

| Original selector (before decrement) | 0x9460 wrapper target | 0x97D0 wrapper target | Original instruction-level effect |
|---|---|---|---|
| 1 | 0x94F1 | 0x9861 | `(width+7)>>3`; height unchanged |
| 4, 8 | 0x94C8 | 0x9838 | `(width+3)>>2`, `(height+3)>>2` |
| 16 | 0x9504 | 0x9874 | dimensions unchanged |
| 2, 3, 5, 6, 7, 9–15 | 0x9511 | 0x9881 | return status `0x80D30000` without calling transfer |
| 0 or >16, if gate nonzero | range failure | range failure | same status, without transfer |
| any selector, if gate zero | 0x9504 | 0x9874 | dimensions unchanged |

All successful cases multiply `R9D *= R8D` at 0x9504 / 0x9874 and forward the stack-local region descriptor as RCX to 0x91C0 / 0x9530. This is a stronger restriction than a generic 16-format converter: **12 of the 16 in-range selectors reject**. The two original tables are structurally equivalent under a constant +0x370 shift of their target instruction addresses (including failure paths), not necessarily proof that both external resource directions have identical higher-level semantics.

## Evidence and limitations

- Table 0x033CAD10, original SELF offset 0x035CAA40, 64-byte SHA-256 `bb1753f7f8e6086d74826edfb1b38af75f7b306bee3557a38ae6ab85bfb2c889`.
- Table 0x033CAD90, original SELF offset 0x035CAAC0, 64-byte SHA-256 `c75bab0c8df9cddff9e8e9397b4e86a876b93505321262332dd2a92a458d69f0`.
- Instruction-boundary disassembly of wrapper entries 0x9460 and 0x97D0 confirms `dec ecx; cmp ecx,0xf; ja fail; lea rbx,[rip+table]; movsxd rcx,[rbx+rcx*4]; add rcx,rbx; jmp rcx`. Confirmed targets enter aligned instructions at 0x94C8/0x94F1/0x9504/0x9511 and counterparts 0x9838/0x9861/0x9874/0x9881. The original C-012 direct call edges remain 0x950C→0x91C0 and 0x987C→0x9530.
- Confidence **high** for exact branch partition and arithmetic; **low** for identifying selector values as specific texture formats, compression schemes, shaders or stage resources. The rounding suggests block/tile units but the format identity remains unproven. No FDE or relocation attribution for these tables; no runtime reachability claim.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-10/11 can use this precise 1/4/8/16 selector acceptance and gate behavior to validate CPU-side region layouts and reject mistaken bytes-per-pixel interpretations. SC-09 should not treat these as SGPR descriptor tables or Vulkan bindings.

**Next precise target:** trace the original producer of descriptor `+0x28/+0x2C` into the paired wrappers, or pivot to the shader resource virtual `+0x38` target at ELF 0x0CB5130; prioritize the latter if it yields a shader ownership edge.
