# C-028 — second independently gated CPU registry key producer

Original Déraciné PS4 SELF 1.00: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, freshly verified. Addresses are **inner ELF virtual addresses**; in these executable windows, original SELF offset = ELF VA + `0x1FD30`. No runtime execution, guest loaded VA, PS4 GPU VA, shader hash or Vulkan binding is inferred.

## New original-byte finding

A **second, conditionally gated key producer** calls the previously characterized CPU keyed-registry insertion `0x0CC93D0`, independently of C-025's `0x0CC8558` producer.

At instruction boundaries `0x0CC27F9..0x0CC2843`, the original target:
- reads byte global `0x047B438A` and branches away if nonzero (`0x0CC27F9..0x0CC2806`);
- requires `r12d == 2` (`0x0CC280C..0x0CC2810`);
- loads `rdi=[r8+0x28]`, directly calls `0x0308FE70` at `0x0CC2821`;
- zero-extends EAX, shifts it left 8, writes that 64-bit value to stack `+0x38`;
- passes `rdi=r13+0x3D8`, `rsi=&stack[0x38]` and calls `0x0CC93D0` at `0x0CC2839`; then jumps to `0x0CC28DF`.

C-025's *other* insertion producer at `0x0CC8558` calls **different** classifier `0x03091BF0` on a nested `+0x20` object, also shifts EAX left eight, then inserts into context `+0x3D8`. Therefore two distinct producers converge on the same CPU registry ABI, with a common 8-bit shift but different source-object and classifier paths. The second producer is explicitly mode/global gated. This establishes multiple key-generation contexts, **not** GPU address translation or resource-version identity.

Independent disassembly from `0x0308FE70` confirms a real prologue, input `rdi` preserved as `rbx`, DWORD loads `[rbx]` and `[rbx+8]`, branch on `[rbx]&3`, and further table-driven classification; its full return semantics and FDE are unresolved. Do not equate it with `0x03091BF0` or claim a shader-stage classification.

## Reproduction and confidence

ELF `[0x0CC27F9,0x0CC2843)` = SELF `[0x0CE2529,0x0CE2573)`; SHA-256 `2c6164a02562cbfe0d83c170b8a0c70837330af29dd29bcda919ae309452f03b`. Classifier entry ELF `[0x0308FE70,0x0308FEFB)` = SELF `[0x030AFBA0,0x030AFC2B)`; SHA-256 `c0610d31c4ed03372167494df22409491448dd46b48789322a5d331b1f86e0b2`. Decode with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=<ELF-start>`. Direct E8 displacements resolve to `0x0308FE70` and `0x0CC93D0`; verifier independently checks digests, offsets, instruction bytes and displacements. Confidence **high** for the second producer, gating, classifier distinction and registry join; **low** for exact key semantic identity, caller class, shader binding or GPU relevance. Competing interpretations: general resource validation, deduplication or binding-state registry. No exact FDE established.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can distinguish two CPU-side registry key sources and avoid treating registry changes as GPU buffer-cache versions. A targeted static/live comparison can ask whether the `r12d==2` path is reached during shader-state transitions; this static finding alone cannot answer it.

**Next precise target:** trace the original-byte producer of `[r8+0x28]` at `0x0CC2816` and classify the complete `0x0308FE70` return contract, then relate both classifier outputs to registry lookup `0x0CAC030`. Alternatively prioritize C-001's `+0x218` descriptor table producer if this path proves unrelated to shader resources.
