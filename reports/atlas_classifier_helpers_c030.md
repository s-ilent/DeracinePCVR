# C-030 — Two original classifier width helpers share an indexed CPU table

**Target:** Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` freshly verified. Addresses are inner ELF VAs; original SELF file offset = ELF VA + `0x1FD30` in the verified windows. Loaded guest address, PS4 GPU VA, shader hash and Vulkan binding remain separate unknown domains.

## New independently verified finding

The C-029 classifier `0x0308FE70` invokes either `0x03450` (at `0x0308FF23`) or `0x03220` (at `0x0308FF35`). Both helpers accept a DWORD output pointer in RDI, call `0x0240` with a local stack DWORD destination, and **only when this call returns zero** use that index to read the shared DWORD table at ELF `0x033CA910`.

- `0x03220`: `bextr ecx,[table+index*4],0x206` at `0x325D`, extracting **bits 6–7** (two bits); stores to `[rdi]` at `0x3263`.
- `0x03450`: `bextr ecx,[table+index*4],0x20C` at `0x348D`, extracting **bits 12–13** (two bits); stores at `0x3493`.

If `0x0240` returns nonzero, the output DWORD is not written by this path. Both helpers load a common guard pointer from `0x04380AE0` and invoke `0x033C88A0` if the guard changes. A null output pointer follows a separate error path. The classifier uses the output as a bit-clear mask-width input. Thus the masked CPU registry key depends on a **shared indexed table-derived property**, not solely on descriptor bytes. Neither the table's semantic meaning nor shader stage is proven.

## Original byte evidence

| ELF VA window | SELF offset window | SHA-256 |
|---|---|---|
| `0x3220..0x3283` | `0x22F50..0x22FB3` | `27d8849207e5c99787adec58cd550efc42ea7ed971f852801f822323b6732feb` |
| `0x3450..0x34B3` | `0x23180..0x231E3` | `e1b80bbcc9b52947a82ae91c2af274b6a3d8ee1019149b1d3fee14a33941d97a` |
| `0x0308FE70..0x0308FF75` | `0x030AFBA0..0x030AFCA5` | `449647116815bce4fcb71bc71429712c1643a3c90cd1a405d679afefc6da45c7` |

The shared table ELF `0x033CA910` maps to SELF `0x033EA640`. Independently instruction-boundary decoded with x86-64 objdump; companion `tools/atlas/verify_classifier_helpers_c030.py` checks full SELF digest, region digests, direct calls and exact bit extraction. Confidence **high** for call/data flow and two-bit extraction, **medium** for width interpretation, **unresolved** for table ownership, `0x0240` index bounds, FDEs, shader identity and runtime use. Competing interpretation: generalized device/configuration format properties rather than a shader binding field.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 must not interpret the C-028 registry key as a raw shader hash or GPU buffer version; its value can depend on a separate indexed CPU table property.

**Next precise target:** determine `0x0240` index-acquisition contract and table `0x033CA910` bounds/producer, then establish whether the property joins shader resource ownership.
