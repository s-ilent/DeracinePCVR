# Déraciné PS4 SELF / UE4–FD4 Binary Atlas

Independent **static** research track for the shipped UE4 4.18-derived PS4 executable, focusing on semantic RHI, command-list, shader and FD4 interfaces above GNM/PM4 emission.

**Status: ACTIVE; one accepted static original-target finding (A-002).** The initial bootstrap was metadata-only, but direct user attachments subsequently supplied the exact-hash 1.00 SELF and DSH ZIP. Both hashes match; ZIP member CRC check passed. [A-002 indexed-indirect draw](../../reports/atlas_indirect_indexed_draw_a002.md) independently verifies the +0x4F8 RHI vtable method, FDE, original target bytes, GNM import and draw-argument contract. No runtime execution or complete C++ class name is claimed.

## Boundaries

- Research only; do not modify `main`, runtime/Codex branches, worktrees, renderer, benchmark runner, or DSH workspace.
- This repository is currently reported **public** by GitHub; commit only original sanitized reports, verification scripts, address maps and metadata.
- Keep ELF VA, ELF file offset, SELF container offset, guest mapped VA, vtable/relocated slot, PS4 GPU VA and shader hash separate. Do not treat runtime reachability as a static property.
- Verify original SELF SHA-256 before decoding it. No proprietary binaries, decompiled/extracted game assets, raw DSH transcripts or sensitive local paths in Git.
- One **NEW target-byte-verified** function, structure, or call-graph finding per accepted research run, with exact disassembly/relocation/FDE evidence, counterhypotheses, confidence and repro steps. Metadata-only and tooling changes are not findings.

## Inputs / baselines

- Verified original SELF size `81,506,767`, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` (independently read and hashed in A-002 run; no bytes committed).
- Verified private transcript archive size `92,544,428`, SHA-256 `336d0746d64915c988fa985a92ce2eb8ea1ecf328034385c8d055766a93c984c`; 33 members / 272,185,371 uncompressed bytes, all CRCs passed. See [sanitized session index](session-provenance-20261121.md).
- Verified transcript index SHA-256 `1f661e2956ef8c34e75c83ace75823149a9e1dee10045fa96536a15697f4b772`; 11 non-memory streams selectively indexed, 22 mnemon streams skipped. Full raw logs and credentials remain private.
- GitHub main examined: `e2f981c40cba7c8490177c20c179cb3608cc1442`.
- Existing research source: [A2 RHI recovery](../../reports/A2_internal_rhi_recovery.md) on `research/ue4-internal-rhi-a2-20261121` (blob `1d403c7934712d6958dd747cc79c5581364436bc`) and [A2 candidate map](https://github.com/s-ilent/DeracinePCVR/blob/research/ue4-internal-rhi-a2-20261121/maps/A2_internal_rhi_candidates.tsv) (blob `30551f900e52a1b685592dad2232e59e24f91cf8`).
- B3 runtime report lives on `research/semantic-capture-b2-20261121` ([address map](https://github.com/s-ilent/DeracinePCVR/blob/research/semantic-capture-b2-20261121/reports/B3_rhi_gnm_address_map.md), blob `967512ebf080ebfd847709ddd3346d1652f5ee66`). Runtime trace claims are **reported evidence**, not independently checked from the protected raw run logs here.

## Already reported, not newly discovered

A2 associates inner ELF VA `0x0CBB9C0` with indexed draw, table base `0x03EDFCA0`, slot `0x03EE0190` (+`0x4F0`), compute FDE `0x0CADC00` at +`0x118`, and shader-state FDE `0x0CB5130` at +`0x340`. These are **previously reported static facts**, not reverified this run, and the exact private C++ names remain inferred. B3 reports runtime caller RVA `0x13A68E8` on a separately hashed 01.01 image; direct/virtual dispatch at that caller remains undetermined. Cross-image address equality must not be presumed without validation.

## Current evidence limits and next steps

1. Original 1.00 SELF and transcript archive access **resolved** by direct user attachments. Earlier Library-only `BLOCKED_WITH_RESOURCES` records are historical; consult `state.json` for current status.
2. GitHub research branch and [draft PR #1](https://github.com/s-ilent/DeracinePCVR/pull/1) are open; keep draft and **do not merge**. Only sanitized research files, not the original binary or raw transcripts, are committed.
3. A-002 finding is *indexed-indirect GNM emission*, not proven UE4 C++ type, runtime reachability, or complete Vulkan renderer seam. Next priority A-003: trace the caller/producer and object ownership above this method on the original 1.00 image. Reported A3/B3 caller RVA `0x13A68E8` used a separately hashed 01.01 image, whose matching original bytes are not available in this research run. Do not silently combine address domains or assume exact instruction identity.

See [queue.yaml](queue.yaml), [state.json](state.json), and [evidence-provenance.md](evidence-provenance.md).