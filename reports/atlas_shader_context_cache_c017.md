# C-017 — Shader resource transition reaches a cached writer and two descriptor consumers

**Source:** original 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed in this run. **Address domain:** all code addresses below are ELF virtual addresses; original SELF file offsets in this mapped segment equal ELF VA **+0x1FD30**. These are neither loaded guest addresses nor GPU VAs. No runtime reachability is asserted.

## New original-byte finding

The C-016 descriptor-mismatch path continues into a **resource-backed state selection, context binding-cache clear, writer-state update and two separately parameterized descriptor-consumer calls**. This extends the previously analyzed C-016 window (ending 0x0CB4460) rather than relabeling its paired virtual calls.

* `0x0CB4AF3..0x0CB4B0E`: after an indexed 24-byte-entry lookup and key comparison, load a selected entry's `+0x08` pointer, follow `+0x48`, and test byte `+0x28`. If zero, call `0x013B8490` with that object as RDI. This is conditional object preparation, not a proven shader compilation step.
* `0x0CB4B0E..0x0CB4B21`: read selected object's `+0x60` into R14, call `0x0CC7500` with `RDI=context+0x940`, `RSI=R14`.
* `0x0CB4B21..0x0CB4B47`: read signed count at context `+0x450` and clear exactly that many QWORD entries through array pointer at context `+0x448`, when count is positive. This is a **CPU-side context array reset**, not proof of GPU descriptor invalidation.
* `0x0CB4B47..0x0CB4B93`: fetch writer pointer from context `+0x460`, compare writer `+0xC388` with selected resource `[R14+0x1D8]`, OR mismatch into writer flag `+0xC530`, set flags `+0xC538` and `+0xC54E`, store `R14+0xB0` to writer `+0xC468`, and cache the `+0x1D8` pointer at `+0xC388`. Also extract two low bits of `[pointer+0x24]` to writer `+0xC9C1`. This is a concrete **pointer-identity-dependent dirty flag** and cache update; it does not identify shader hash, GPU address, or Vulkan binding.
* `0x0CB4B93..0x0CB4C01`: if WORD `[resource+0x1D8]+0x06` is nonzero, a writer-capacity check can invoke callback `[writer+0x18]`; on success, emit a 12-byte record beginning with constant `0x000218C0017602` and an encoded DWORD derived from that WORD. The callback is capacity-related, not a semantic shader-state writer.
* `0x0CB4C1F..0x0CB4C6B`: pass `writer+0x2E8` as RDI to `0x02F62A40` and `0x02F62BD0`. Both receive ESI=0, ECX=1 and a stack-copied 32-byte descriptor in R8; the former uses WORD `[selected-entry+0xBC]` as EDX, the latter `[selected-entry+0xC0]`. The two descriptor sources are copied from the paired resource `+0x18` objects at `0x0CB445F..0x0CB447A`.

**Interpretation:** this is a static bridge from a paired resource-descriptor comparison to a context-local binding cache and writer state. The pair of consumers may correspond to different resource roles or stages, but neither stage identity nor SGPR/user-data register meaning is established. The `+0xC530` flag tracks pointer inequality, not proven GPU buffer-cache versioning.

## Reproducibility

Contiguous instruction-boundary region `[0x0CB4AF3,0x0CB4C6B)` (SELF `[0x0CB6E23,0x0CB6F9B)`) SHA-256 `612b3d61d99cb34b0a50b50e2655ec0bf64e4782c165dd0c749fb8ef56772081`. Subwindows `[0x0CB4B21,0x0CB4B88)` and `[0x0CB4C1F,0x0CB4C6B)` SHA-256 `e073d3988bc6021f6cfb0abcae6055ab0e18f575634991a30d96c7153ebf8868` and `c5495fcfe5956b22f0bf2f1eb105f2cd0d2806e4a20753981d04ef9fabe0cb92`. Direct E8 calls are independently checked by `tools/atlas/verify_shader_context_cache_c017.py`. Decode with x86-64 instruction boundaries from `0x0CB4330`, not raw E8 scans. Enclosing FDE and concrete class identities are unresolved.

**Confidence:** high for exact byte windows, branch conditions, field accesses, direct calls and cache-clear loop; medium for shader-resource semantic classification. Competing interpretation: generalized descriptor/resource transition and writer cache preparation, rather than per-stage shader binding. No live usage claim.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can specifically compare a shader/resource transition against context `+0x448/+0x450`, writer `+0xC388/+0xC530`, and the two WORD-selected descriptor consumers `0x02F62A40/0x02F62BD0`; these are original-SELF test candidates, not Vulkan binding assignments.

**Next precise target:** independently decode the producer and callee behavior of `0x02F62A40` versus `0x02F62BD0` to determine the meaning of the two WORD selectors at selected-entry `+0xBC/+0xC0` and whether either reaches stage-specific user-data registers.
