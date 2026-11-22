# Independent Binary Atlas A-024 — Stage-indexed shader map provenance

**Static original-target finding; no runtime reachability claim.** Déraciné 1.00 original SELF: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. All addresses inner ELF VAs, executable file offset VA+`0x1FD30`. Locally verified **62/62 SHA-gated instruction, direct-call, stage-register table and pointer-arithmetic checks**. No game data distributed.

## Result

A-023 proved the source of stage-0 user-data map at `graphics_state+0xC468`. A-024 independently recovers **stage indices 1–4** and shows their pointer sources are *not homogeneous*. Some point into shader-resource metadata; others into **writer-owned scratch state**.

The GCN user-data helper `0x02F62440` retrieves `map=[writer+0xC180+8*stage]` at `0x02F6246E` and a map entry at `[map+0xC2+2*slot]` (`0x02F624B2`). With `writer=graphics_state+0x2E8`, the stage pointers live at `graphics_state+0xC468+8*stage`. The original seven-word register-base array at `0x037530B0` is CS,PS,VS,GS,ES,HS,LS (`0x2E40,0x2C0C,0x2C4C,0x2C8C,0x2CCC,0x2D0C,0x2D4C`). These names refer to GCN register banks, not verified gameplay stage reachability.

| Index / bank | Pointer slot | Verified source / setup |
|---|---|---|
| 0 / CS | `state+0xC468` | Active shader resource `+0xB0`, direct assignments `0x0CB132A`, `0x0CB4B7A`, `0x0CC372B` |
| 1 / PS | `state+0xC470` | Resource `[shader_state+0x1E8]+0xB0` via `0x0CBEC21 -> 0x02F62350`; **explicit null clear** via `0x0CBED3B`; helper writes `writer+0xC188` at `0x02F62423` |
| 2 / VS | `state+0xC478` | Fallback `[shader_state+0x1E0]+0xB0` at `0x0CBE611`; otherwise helper-selected **writer-internal scratch-map pointer** at `0x02F61EB3` or `0x02F6214E` |
| 3 / GS | `state+0xC480` | Resource `[shader_state+0x200]+0xB0` via helper stores `writer+0xC198` at `0x02F61E00` / `0x02F62095` |
| 4 / ES | `state+0xC488` | Resource `[shader_state+0x1E0]+0x268` via `0x0CBE41A` / `0x0CBE70D` |

## Conditional source selection

- `0x0CBE39D` reads shader resource A from `shader_state+0x200`; `0x0CBE3AD` reads B from `shader_state+0x1E0`. Absent prerequisites (`A` or `B+0x260`) take a fallback where `state+0xC478=B+0xB0` at `0x0CBE611`.
- Active mode reads program `[A+0x1D8]`, its selector `program[3]`, and bit 1 at `program[0x5E+4*selector]` (`0x0CBE3CC..0x0CBE3D5`).
- With bit clear, `0x0CBE44F` calls `0x02F61D50(writer, program, A+0xB0)`. It sets stage 3 to A's metadata and stage 2 to `writer+0xC380` or `writer+0xC268`, selected by a program indexed-word test (compare `0x01000000`).
- With bit set, `0x0CBE73E` calls `0x02F61FE0` using the same A metadata. Stage 3 again receives A's metadata, but stage 2 uses `writer+0xC5B0` or `writer+0xC498`.
- In either active path, stage 4 receives `B+0x268`. The stage-1 resource at `shader_state+0x1E8` is set or cleared separately.

**Practical SC-series warning:** do **not** reconstruct all stage map pointers as `shader_resource+0xB0`. Instead read the effective stage-indexed map pointer at `writer+0xC180+8*stage` during capture, then resolve its slot map using the actual source and lifetime. Program flags can change the stage-2 pointer between different writer-local regions; a pointer-only shader association will miss that distinction.

**Unresolved:** source-level class names, construction of the map bytes within the shader resource, stage-5/6 pointer producers, runtime reachability, and the meanings of the alternate shader setup modes. No claim that these branches necessarily implement tessellation.

**Verification:** `tools/atlas/verify_independent_a024.py <private-original-1.00-eboot.bin>`. A full standalone evidence report and verifier were also generated as local research artifacts. This PR remains separate from runtime implementation and the scheduled Atlas branch.