# Evidence provenance / triage — initial bootstrap only

> **Historical bootstrap record:** The entries below describe the earlier metadata-only run, before the original SELF and DSH ZIP were directly attached. They are not the current research status. The inputs have since been independently SHA-256 verified, all archive member CRCs passed, and the first accepted original-target finding is [A-002 indexed-indirect RHI emitter](../../reports/atlas_indirect_indexed_draw_a002.md). Current state: [state.json](state.json); new source/event provenance: [session-provenance-20261121.md](session-provenance-20261121.md).


| ID | Classification | Evidence source at inspected revision | Address domain | Validation status | Next |
|---|---|---|---|---|---|
| RHI-DRAW-A2 | ALREADY_VERIFIED *in earlier A2 report only* | `A2_internal_rhi_candidates.tsv`, blob `30551f900e52a1b685592dad2232e59e24f91cf8`; `reports/A2_internal_rhi_recovery.md`, blob `1d403c7934712d6958dd747cc79c5581364436bc` | inner ELF VA `0x0CBB9C0`; vtable slot `0x03EE0190` | NOT reverified against original SELF in this task | Don't repeat proof; investigate adjacent slot |
| RHI-COMPUTE-A2 | ALREADY_VERIFIED *in earlier A2 report only* | same A2 map/report | ELF VA `0x0CADC00`, table slot `0x03EDFDB8` | NOT reverified this run | Find new related owner/member |
| RHI-SHADER-A2 | ALREADY_VERIFIED *in earlier A2 report only* | same A2 map/report | ELF VA `0x0CB5130`, slot `0x03EDFFE0` | NOT reverified this run | Decode resource-field flow |
| RHI-CALLER-B3 | CANDIDATE_VERIFY (dispatch mode) | `reports/B3_rhi_gnm_address_map.md`, blob `967512ebf080ebfd847709ddd3346d1652f5ee66` | 01.01 runtime caller RVA `0x13A68E8`; loaded VA `0x8013A68E8` | Runtime caller **reported**, raw logs uninspected; caller instruction and virtual/direct dispatch undecoded | Obtain matching 01.01 bytes; confirm exact instruction |
| DSH-INDEX | CANDIDATE_VERIFY (event-level) | private sanitized session index digest `1f661e29…` | 33 JSONL stream metadata | Index inspected structurally; raw ZIP **not** verified | Verify zip digest, member inventory, then process bounded agent event windows |

No transcript tool events, raw disassembly, secrets or proprietary content were ingested during this retry. *“Already verified” records inherited from the earlier A2 report are NOT new findings of this run.* No item is promoted to NEW_READY_STATIC until source and candidate evidence are accessible and original-byte checked.
