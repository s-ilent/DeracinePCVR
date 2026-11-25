# C-026 — CPU keyed-registry bucket growth and rehash

Original Déraciné PS4 SELF 1.00, 81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` freshly checked. Address domain throughout: **inner ELF virtual address**; in the checked region original SELF file offset = ELF VA + `0x1FD30`. No loaded guest address, GPU virtual address, shader hash or Vulkan binding is inferred.

## Independently decoded new finding

The insertion function entered at `0x0CC93D0` (C-025) continues through `0x0CC99CB` (return). Its nonduplicate path has an explicit bucket-capacity decision and **rehashes existing active records**. This is not merely a record insertion.

* `0x0CC95B8..0x0CC96AF`: computes a candidate power-of-two bucket capacity (using the current record count, conditional shifts and `shl r10d,cl`). The exact load-factor heuristic is not yet named.
* `0x0CC96AF..0x0CC96CA`: compares local bucket capacity `+0x48` to the candidate; an adequate nonzero capacity branches to the direct insertion path.
* `0x0CC96CA..0x0CC9762`: the direct path hashes the new key using `0x9E3779B9`, stores the masked bucket number into record `+0x0C`, links the record's `+0x08` to the prior bucket head and replaces the bucket head with the new index. The `+0x0C` field is therefore a bucket index, not an object size or GPU binding.
* `0x0CC9767..0x0CC9792`: the growth path writes candidate capacity to local `+0x48`, calls `0x0C7BD40` twice on local `+0x38` with zero then requested capacity, selecting/reallocating bucket storage. The precise helper's allocator contract is not independently established.
* `0x0CC9796..0x0CC97C7`: initializes all new bucket heads to signed `-1`, selecting heap pointer `+0x40` or inline `+0x38`.
* `0x0CC97C9..0x0CC99B7`: scans active-index bitset local `+0x20/+0x28`, uses `blsi` and `lzcnt` to enumerate occupied slots, hashes each 16-byte record's key, stores its new masked bucket in record `+0x0C`, updates record `+0x08` next-link, and replaces the new bucket head. Thus the growth branch **rebuilds bucket chains from occupied record indices**, not merely copies an old bucket array.

With caller `0x0CC8558` passing context `+0x3D8` as the local base (C-025), the mapped fields are context `+0x410` inline buckets, `+0x418` heap buckets, `+0x420` bucket capacity, `+0x3F8` heap bitset, and record base `+0x3D8`. This independently strengthens the CPU hash-table interpretation of C-022/C-023/C-024/C-025. It **does not** establish that the registry represents GPU buffer-cache versions, resource lifetime, shader identity or live execution.

## Reproduction and limits

Verified interval ELF `[0x0CC95B8,0x0CC99CC)` corresponds to original SELF `[0x0CE92E8,0x0CE96FC)`; SHA-256 `a176e9789800c5fc563f352f540c71ae703dcab8defefaad5378f033757a8a84`. The specific growth/initialization block ELF `[0x0CC9767,0x0CC97C9)`, SELF `[0x0CE9497,0x0CE94F9)`, SHA-256 `309fc024ea0ef063490b4cc6198562ca9ba2b9a672e752cc4151c1ae1e6b6a2b`. Instruction-boundary disassembly: `objdump -D -b binary -m i386:x86-64 --adjust-vma=0xCC95B8` on the first interval; cross-check branches against entry-disassembly beginning `0x0CC93D0`. The fallthrough at `0x0CC99CB` is `ret`, followed by a new prologue at `0x0CC99D0`; no exact FDE was independently confirmed.

Confidence **high** for bucket-capacity update, -1 initialization, bitset-driven rehash and per-record chain writes; **medium** for exact allocation policy and duplicate-key ownership. Competing interpretations for the enclosing registry remain resource deduplication, binding-state suppression, and CPU validation tracking.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can treat `+0x420` changes as **CPU hash-bucket rehashes**, not shader buffer version increments. The original ELF exposes stable record/key/bucket semantics for targeted cross-checking, without implying title-screen reachability.

**Next precise target:** decode `0x0C7BD40` with its FDE and at least two callers to determine the `+0x38/+0x40` inline-to-heap allocation contract; then identify the constructor initializing the registry in its enclosing context.
