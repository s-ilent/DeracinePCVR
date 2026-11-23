# C-015 — Mode-dependent three-phase command sequence in shader-state candidate

**Target:** Original Déraciné 1.00 SELF, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed on 2026-11-23. Static only. For both code windows, original SELF offset = ELF VA + `0x1FD30`. No loaded guest address, GPU virtual address, Vulkan descriptor or shader hash is inferred.

## New independently decoded finding

The previously identified mode-1 and mode-5 entries in `0x0CB5130` do **not** simply emit one six-DWORD command. Each constructs an aligned/variable-length first record, a fixed 24-byte second record, and a 28-byte third record. The second record has a **mode-specific embedded 64-bit constant**, providing a concrete discriminator that SC-09 can compare against shader-state capture without assigning a PS4 register name.

- Mode 1 at `0x0CB82B4`: first iterates resource pointers in eight-byte steps, invokes `[resource-vptr+0x38]`, and stores mode 1 to returned-object `+0x08` (`0x0CB82C0..0x0CB82D3`). It then reads context writer `+0x460` at `0x0CB82DC`.
- First emission at `0x0CB830E..0x0CB8361`: computes 16-byte alignment of writer cursor, writes a header with `0xC0001000` flags and `0x68753000` second DWORD, and updates the cursor using computed DWORD count. It is **not** a fixed six-DWORD packet; the six-DWORD threshold is the growth/reservation request.
- Second emission at `0x0CB8398..0x0CB83D8`: writes 24 bytes, starting with `0x00038528C0044700`, followed by aligned address-derived fields and terminal QWORD 1. Its address-derived value comes from the writer's aligned cursor `R14`, **not a demonstrated GPU address**.
- Third emission at `0x0CB8409..0x0CB8459`: writes `0xC0053C00`, conditional 16/32-bit splits of `R14`, QWORD `0xFFFFFFFF00000001`, DWORD `0xA`, and advances writer cursor by 28 bytes.
- Mode 5 starts at `0x0CB846C` without the mode-1 resource loop, but repeats the same three-phase shape. At `0x0CB8528..0x0CB856B`, its second 24-byte record starts with `0x00038504C0044700`. The mode-1/mode-5 constant XOR is exactly `0x0000002C00000000`. Mode-5 third emission `0x0CB859C..0x0CB85F4` uses the same `0xC0053C00` header and 28-byte cursor increment.

Both paths check remaining DWORD capacity and call writer callback at **object offset** `+0x18` with requests of 6, 6, and 7 DWORDs as appropriate. This is a capacity-growth callback, not a semantic shader writer vtable. Mode 5 does not imply absence of resource state from earlier invocations; only its local entry skips the resource iteration.

## Evidence and confidence

Original ELF windows `[0x0CB82B4,0x0CB846C)` and `[0x0CB846C,0x0CB8637)` map to SELF offsets `0x0CD7FE4` and `0x0CD819C`, with lengths 440 and 459 bytes and SHA-256 `1437fa3650508a99054da139adf7165aef7f20e9f23b7070de736d9f176107dc` and `160b6070efe7ce8d8eb426602578e51f056e97f85a7db740257d6319e4e1f220`. Instruction-boundary x86-64 disassembly confirms the cursor arithmetic, immediate constants, stores and conditional growth calls. The enclosing FDE/function and mode table were previously established in C-001/C-014; this report does not reattribute them.

**High confidence** in the instruction-level two-mode distinction, three sequential emission phases and cursor increments. **Unresolved:** meaning of packet constants, whether R14 represents a CPU command-buffer pointer or an encoded address, which shader stage is affected, and whether either path executes in the sampled scene. Competing interpretation: these are PS4 command-stream setup/relocation records rather than shader descriptor-table bindings. Do not interpret `0x38528` / `0x38504` as verified hardware register IDs without independent packet-definition joins.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can use the mode-dependent 64-bit constant and three-phase emission pattern to distinguish shader-state writer paths and avoid mistaking capacity callback `+0x18` for a resource binding. This does not establish a Vulkan descriptor or GPU address. **Next precise target:** identify a concrete vtable target and producer for resource virtual `+0x38` invoked at `0x0CB82CC` and `0x0CB5237`, then prove returned-object `+0x08/+0x18/+0x20/+0x88` ownership through multiple original-byte accesses.
