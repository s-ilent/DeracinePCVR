# DSH session archive — first-pass sanitized evidence index (2026-11-21)

The original private archive and index were verified **outside Git**, against the uploaded files. Archive SHA-256 `336d0746d64915c988fa985a92ce2eb8ea1ecf328034385c8d055766a93c984c`; index SHA-256 `1f661e2956ef8c34e75c83ace75823149a9e1dee10045fa96536a15697f4b772`. ZIP inventory: 33 members, 272,185,371 bytes total uncompressed, CRC check of **every member passed**. Index metadata: one lead stream, ten non-memory research/audit streams, 22 mnemon streams. The first-pass keyword/provenance scan visited all **11** non-memory streams; none of the raw JSONL content is in Git. The 22 mnemon streams are intentionally deferred. Future passes must read selected windows only.

## Relevant previously seen events

Event numbers below are **ordinal positions within the indicated JSONL stream** (line 1 is the session header). `seq` is the stream's own event sequence. These are **untrusted transcript leads**, not a substitute for original binary bytes or committed source. All address claims promoted to the A-002 report were independently checked against the exact-hash SELF.

| Stream role | Event ordinal / seq | Classification | Earlier evidence | Current result |
|---|---|---|---|---|
| First-round A / RHI | 237 / 235 | ALREADY_VERIFIED (candidate recorded earlier) | Tool result lists direct GNM import call candidate `0x0CBC359 -> 0x033C8390` and identifies `sceGnmDrawIndexIndirect` | Independently decoded, NID / GOT and FDE checked in new A-002 finding |
| First-round E / runtime | 253 / 251 | ALREADY_VERIFIED (import inventory recorded earlier) | Tool result lists NID `ED9-Fjr8Ta4`, JUMP_SLOT `0x04381DA8`, call candidates `0x0CBB841` and `0x0CBC359` | Original JUMP_SLOT record 451 and symbol verified |
| A2 / internal UE4 RHI | 292 / 290 | ALREADY_VERIFIED (table enumerated earlier) | Tool result includes `0x03EE0198 -> 0x0CBBEF0` | Table slot original bytes, RELATIVE record 37982 verified |
| A2 / internal UE4 RHI | 936 / 934 | ALREADY_VERIFIED (table enumerated earlier) | Relocation/table enumeration includes the same adjacent slot | No new claim for slot enumeration |
| A2 / internal UE4 RHI | 944 / 942 | ALREADY_VERIFIED (table enumerated earlier) | Rela type 8 and target `0x0CBBEF0` listed | Original RELA checked |
| A2 / internal UE4 RHI | 1019 / 1017 | ALREADY_VERIFIED (table bytes inspected earlier) | Raw qwords showing `0x0CBB9C0, 0x0CBBEF0` neighbors | Independent SELF mapping verified |
| A-002 new analysis | N/A (this research turn) | **NEW VERIFIED_STATIC** | Exact FDE at `0x038F7A50`; call-argument setup, two writer/state field paths, branch on import success, indexed-indirect API linkage | New semantic vtable-member attribution, [report](../../reports/atlas_indirect_indexed_draw_a002.md) |

## Candidate queue from this pass

- **NEW_READY_STATIC:** caller/producer of the new `+0x4F8` indirect-indexed table method, plus fourth-argument writer ownership. Prior archive sessions did not promote a proven method-name or owner for this new attribution.
- **CANDIDATE_VERIFY:** nearby `+0x4E8 -> 0x0CBB410` also calls `sceGnmDrawIndexIndirect` at `0x0CBB841`. Confirm vtable family semantics and differences without equating parameter layouts.
- **CANDIDATE_VERIFY:** A2's separate `+0x340` shader-state FDE `0x0CB5130` to identify specific resource ownership, shader register descriptor lifetime.
- **BLOCKED_WITH_RESOURCES:** reported 01.01 runtime caller RVA `0x13A68E8` must be checked against matching 01.01 bytes. Only 1.00 original SELF was independently inspected in this turn.
- **KNOWN_FALSE:** no new proven false hypotheses in the sampled event windows.

## Disclosure / resumption rules

No raw logs, tools' verbatim output, original SELF bytes, derived stripped ELF, proprietary assets, passwords, private development paths, frame dumps or agent transcripts are published here. Keep ZIP hash, completed non-memory stream coverage and event ordinals in `state.json` to avoid repeatedly streaming all 272 MB. Research continues independently of runtime instrumentation. For a new research result, use original binary bytes, not transcript summaries alone.
