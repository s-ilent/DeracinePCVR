# C-006 — Original descriptor mask's architecture-dependent field selector

Original Déraciné 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly verified against 81,506,767 bytes. Static only. For the examined executable regions, original SELF file offset = ELF VA + `0x1FD30`. No guest loaded address, GPU VA, relocated slot or shader hash is asserted.

## Independently decoded finding

The two calls from C-004's `0x03091CD5` and `0x03091CE1` are **not interchangeable predicates**. They invoke two near-identical, but distinctly encoded, output decoders:

| ELF entry | SELF offset | table extract after common call `0x240` | caller-selected mask shift |
| --- | --- | --- | --- |
| `0x00003220` | `0x00022F50` | `bextr ecx,[table+index*4],0x206` = table bits 6–7 | 3 |
| `0x00003450` | `0x00023180` | `bextr ecx,[table+index*4],0x20C` = table bits 12–13 | 4 |

Both functions accept a pointer in RDI, preserve it in RBX, call `0x240` with `RDI=&[rbp-0x1c]`, then, **only when EAX == 0**, read the resulting index from that local and fetch a DWORD from table ELF `0x033CA910`. They write the two-bit extraction to `[RBX]` at `0x3263` / `0x3493`. A null output pointer takes an alternate path and does not write. The table's entry is an encoded metadata word, not an established shader program or GPU descriptor.

The parent `0x03091BF0` derives `ESI = descriptor[+0x14] & 0x1F`, `ECX = 1 << ((descriptor[+0x14] >> 15) & 3)`, and EDX from the earlier format-width calculation; `RDI=&[rbp-0x2c]`. It chooses `0x3220` when descriptor DWORD `+0x10` is nonnegative and `0x3450` when negative. On return it loads the local output, increments it, computes `(1 << (output+1)) - 1` using x86 32-bit shift semantics, shifts that mask left by 3 or 4, then executes `andn r14d,edx,r14d` at `0x03091CFD`. Thus the sign bit selects **which pair of metadata bits determines the mask width** and simultaneously selects the final field's starting bit (3 versus 4).

This is a genuine consumer/producer edge: descriptor `+0x14` and format-derived width -> common index resolver `0x240` -> table `0x033CA910` -> two-bit width -> cleared range in descriptor `+0x00` -> C-004 full/abbreviated packet selection. The common resolver is not a trivial pure table lookup: `0x240` calls `0x2EC0`, `0x3020`, and on one path `0x2FD0/0x2FF0/0x30B0/0x3080/0x2F90`, with failure status `0x80D30000`. Its full index algorithm and table bounds remain unproven.

**Important limitation:** the parent does not visibly test the decoder's return status before reading `[rbp-0x2c]`; the local is only written on successful resolver return. The formula therefore describes the **successful output-written path**, not a guaranteed valid mask for arbitrary descriptor inputs. Shifts are x86 modulo-32, so do not translate this to an unbounded mathematical shift without guards. Do not infer shader stage, GPU register, buffer lifetime, live execution or Vulkan descriptor identity.

## Reproducibility

- ELF `0x3220..0x3290` -> SELF `0x22F50..0x22FC0`; SHA-256 `01619330eec6eff7b322087c1c8716ddbda134eb084dce073eecd71c318c97c9`.
- ELF `0x3450..0x34C0` -> SELF `0x23180..0x231F0`; SHA-256 `03f6955fa6a07b57d302da84c9409c2d49e571d0724ff6e948cbbb1f59d8d987`.
- ELF `0x240..0x380` -> SELF `0x1FF70..0x200B0`; SHA-256 `843119267478bd061e4acc9eb629ec301f91a47e51c80c743275ccdd4524e6f9`.
- ELF `0x03091CB6..0x03091D06` -> SELF `0x030B19E6..0x030B1A36`; SHA-256 `198dd1c78b3cecbb243c656410aee52ec024427564cbc9036cded2e71d035e73`.
- Decode with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=<ELF VA>` on exact extracted windows. Companion verifier validates original SHA, four window digests, callsite and BEXTR control bytes.

**Confidence:** high for instruction-boundary decoded branch, distinct BEXTR bit ranges, output writes, mask construction and common resolver call. Medium for generic encoded-format metadata interpretation. Resolver index validity and caller failure behavior unresolved.

**Transfer to SC-09/SC-10/SC-11:** test captured guest descriptor `+0x10/+0x14` against the correct sign-selected two-bit table extraction before predicting the C-004 packet; log resolver failure separately, and never equate the table index with a GPU virtual address or shader hash.

**Next precise target:** decode `0x00000240`'s `0x2EC0/0x3020` success path and `0x033CA910` bounds/producer to establish which descriptor combinations yield a valid output index.
