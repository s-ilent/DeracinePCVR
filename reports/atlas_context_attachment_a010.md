# A-010 — Context-local attachment binding from a global resource table

**Original target:** Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Static-only, no live-reachability claim.

## New original-byte finding

A complete function starts at **ELF VA 0x0D049F0** (prologue `push rbp; mov rbp,rsp; push r15,r14,r13,r12,rbx`) and returns at 0x0D04DE1. Its first argument is preserved in `r14` at 0x0D04A08. It obtains a command-writer pointer from **context +0x460** at 0x0D04A27 and obtains a resource pointer from a globally indexed pointer table at **ELF VA 0x044962C8**, using global index **0x044965FC**, at 0x0D04A19–0x0D04A35. The resource's DWORDs **+0x58 and +0x5C** are loaded at 0x0D04A36–0x0D04A3B and converted to floating-point values; these feed several capacity-checked command-buffer writes, including floating-point state fields. This is consistent with dimension-derived viewport/scissor state, but the exact PS4 register meaning is not established.

The same function later initializes the selected resource's **+0x68 DWORD to 1** (0x0D04D71), **+0x70 BYTE to 1** (0x0D04D9A), and **+0x6C DWORD from global 0x04496748** (0x0D04D94–0x0D04DA5). It builds a stack-local descriptor whose first QWORD is the resource pointer and whose next QWORD is `0xffffffff00000000`; it writes a WORD `0x0101` at the subsequent stack location and directly calls **0x0CC2420** at **0x0D04DBE** with context `r14` in `rdi`, `esi=1`, and the descriptor pointer in `rdx`. The callee is the previously verified A-005 attachment-binding context method. The stack descriptor's full ABI remains unresolved.

This is a **new direct context-to-attachment binding callsite**, independent of A-005's deferred RHI command Execute wrapper. It also connects the global epoch-like value 0x04496748 to a distinct resource metadata layout (+0x68/+0x6C/+0x70), in addition to A-009's backing-object +0x08/+0x0C/+0x10 fields. **Do not assume these are the same object type.** No global writer has yet been established; the word “epoch” is descriptive speculation, not a verified frame counter.

## Independent byte provenance

ELF VA → original SELF offset for these instruction windows is **VA + 0x1FD30**. This does not give a loaded guest address or GPU VA. Three verified windows:

| ELF VA range | SELF offset start | Length | SHA-256 of original bytes |
|---|---|---:|---|
| 0x0D049F0–0x0D04A40 | 0x0D24720 | 80 | `da21b77be4759a5e47540595de550e22e00a5ac9890d235dbb36400c7a78b795` |
| 0x0D04A83–0x0D04B19 | 0x0D247B3 | 150 | `13922b2025329ab4f83d6f90bcb7fc6df957679c99444061de304360cccf604e` |
| 0x0D04D71–0x0D04DC3 | 0x0D24AA1 | 82 | `5fb860d74b03de174566afaf6843e20b14f33fd8e2d629a7e095617bfa3b9892` |

Instruction boundaries and direct-call displacement were independently decoded with GNU objdump in Intel x86-64 mode, on original-byte windows. Reproduce with `tools/atlas/verify_context_attachment_a010.py` and objdump. The verifier checks original SELF digest, exact window digests, RIP-relative global references and direct call target from the original bytes; no proprietary bytes are published.

## Confidence and next question

**High:** function boundary/prologue and return, context writer +0x460, global indexed resource selection, dimension-field reads, three metadata writes, stack descriptor, and direct call to 0x0CC2420. **Moderate:** viewport/scissor semantics. **Unknown:** resource C++ type, table ownership, global 0x04496748 producer, descriptor ABI, runtime execution. The direct context call is an architectural alternative to the deferred command Execute path, not evidence that one path supersedes the other.

**Next:** trace the producer of the selected global table resource and writes to 0x04496748, then test whether +0x58/+0x5C represent render dimensions and whether the resource can be identified with A-009's distinguished backing object.