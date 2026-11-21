# Déraciné PS4 SELF / UE4–FD4 Binary Atlas

Independent **static** research track for the shipped UE4 4.18-derived PS4 executable, focusing on semantic RHI, command-list, shader and FD4 interfaces above GNM/PM4 emission.

**Status: BLOCKED_WITH_RESOURCES. This bootstrap is NOT an accepted original-target finding.** No new original-SELF disassembly was independently verified in this run. The source SELF and DSH ZIP appear in the private Library but Files raw-byte materialization denied access to both, so neither payload SHA-256 was checked. No raw archive streams were inspected.

## Boundaries

- Research only; do not modify `main`, runtime/Codex branches, worktrees, renderer, benchmark runner, or DSH workspace.
- This repository is currently reported **public** by GitHub; commit only original sanitized reports, verification scripts, address maps and metadata.
- Keep ELF VA, ELF file offset, SELF container offset, guest mapped VA, vtable/relocated slot, PS4 GPU VA and shader hash separate. Do not treat runtime reachability as a static property.
- Verify original SELF SHA-256 before decoding it. No proprietary binaries, decompiled/extracted game assets, raw DSH transcripts or sensitive local paths in Git.
- One **NEW target-byte-verified** function, structure, or call-graph finding per accepted research run, with exact disassembly/relocation/FDE evidence, counterhypotheses, confidence and repro steps. Metadata-only and tooling changes are not findings.

## Inputs / baselines

- Expected SELF size `81,506,767`, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` (unverified **this run**).
- Expected private transcript archive size `92,544,428`, SHA-256 `336d0746d64915c988fa985a92ce2eb8ea1ecf328034385c8d055766a93c984c` (unverified against archive bytes).
- Available transcript index: SHA-256 `1f661e2956ef8c34e75c83ace75823149a9e1dee10045fa96536a15697f4b772`; parsed and structurally checked previously: 33 streams (lead + 10 research agents + 22 memory-maintenance), 272,185,371 declared uncompressed bytes. **Not an archive integrity check.**
- GitHub main examined: `e2f981c40cba7c8490177c20c179cb3608cc1442`.
- Existing research source: [A2 RHI recovery](../../reports/A2_internal_rhi_recovery.md) on `research/ue4-internal-rhi-a2-20261121` (blob `1d403c7934712d6958dd747cc79c5581364436bc`) and [A2 candidate map](https://github.com/s-ilent/DeracinePCVR/blob/research/ue4-internal-rhi-a2-20261121/maps/A2_internal_rhi_candidates.tsv) (blob `30551f900e52a1b685592dad2232e59e24f91cf8`).
- B3 runtime report lives on `research/semantic-capture-b2-20261121` ([address map](https://github.com/s-ilent/DeracinePCVR/blob/research/semantic-capture-b2-20261121/reports/B3_rhi_gnm_address_map.md), blob `967512ebf080ebfd847709ddd3346d1652f5ee66`). Runtime trace claims are **reported evidence**, not independently checked from the protected raw run logs here.

## Already reported, not newly discovered

A2 associates inner ELF VA `0x0CBB9C0` with indexed draw, table base `0x03EDFCA0`, slot `0x03EE0190` (+`0x4F0`), compute FDE `0x0CADC00` at +`0x118`, and shader-state FDE `0x0CB5130` at +`0x340`. These are **previously reported static facts**, not reverified this run, and the exact private C++ names remain inferred. B3 reports runtime caller RVA `0x13A68E8` on a separately hashed 01.01 image; direct/virtual dispatch at that caller remains undetermined. Cross-image address equality must not be presumed without validation.

## Current blockers

1. Both authorized private file copies returned `This Project file does not have an authorized raw-byte materialization path`; metadata/list access alone does not provide executable bytes for a valid analysis. Binary and ZIP checks remain blocked.
2. GitHub research-branch creation **succeeded on the retry**, superseding the earlier HTTP 403. Publish only to `research/deracine-self-atlas`; leave the PR open as a draft.
3. The target is explicitly static research; do not substitute a live GPU/headset test for original-byte verification.

See [queue.yaml](queue.yaml), [state.json](state.json), and [evidence-provenance.md](evidence-provenance.md).