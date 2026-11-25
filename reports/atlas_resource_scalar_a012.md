# A-012 — Conditional resource scalar state emission in the attachment context

**Source:** original 1.00 PS4 SELF, size 81,506,767, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Independently reverified for this run. **Static evidence only**; no live reachability claimed.

## New finding

Within the same attachment context method that feeds A-011, instruction-boundary-confirmed code at **ELF VA 0x0CC2766–0x0CC27F9** implements a **conditional 12-byte graphics-state emission driven by a resource scalar**, not a blind command write:

1. At `0x0CC2766`, it tests `DWORD [r15+0x1C]`; zero skips the path.
2. At `0x0CC2771`, it reads cached context `DWORD [r13+0x51C]`. At `0x0CC2778`, it reads a **float** from `[r15+0x20]`; it converts the cached integer to float (`vcvtsi2ss`) and compares with `vucomiss`. The equal, ordered case skips emission.
3. When unequal/unordered, `0x0CC278B` converts the resource float to a signed integer with truncation (`vcvttss2si`), and `0x0CC2790` updates context cache `+0x51C`.
4. Through context writer pointer `[r13+0x460]`, it checks available DWORD capacity. If fewer than three DWORDs remain, it invokes writer callback `+0x18` requesting three DWORDs, and checks its success byte.
5. At `0x0CC27DF`, it places the 64-bit command constant `0x0000000BC0016900` at the write cursor, then stores the **original resource float** at cursor `+0x08`; advances the cursor by **12 bytes**.

This is a concrete **resource-derived scalar → cached graphics context → conditional command writer** connection above packet submission. The cache is integer-valued even though the emitted payload is float; therefore a float-to-int conversion and exact float comparison coexist. It is unsafe to infer that `+0x51C` holds the same bit pattern as the emitted payload. NaN and out-of-range conversion behavior are not characterized here.

An adjacent block `0x0CC26AD–0x0CC2766` performs a pointer-keyed hash lookup against context `+0x2C8/+0x300/+0x308/+0x310`, using 16-byte records with pointer at `+0` and chain index at `+8`. Its observed branches converge on `0x0CC2766`; **no persistent insertion or semantic effect is proven**, so this is recorded only as structural context, not a cache-ownership attribution.

## Reproducibility and address domains

These windows use the **original SELF file offset = ELF VA + 0x1FD30** mapping, verified locally; this is not a guest loaded VA, relocated pointer, GPU VA or shader hash.

| ELF VA interval (exclusive end) | SELF file offset | Length | SHA-256 |
|---|---|---:|---|
| `0x0CC26AD–0x0CC2766` | `0x0CE23DD` | 185 | `965fc6bb0670d23c8b60db5b1eeacf39ae18b7650a0618c862348c73413496d5` |
| `0x0CC2766–0x0CC27FF` | `0x0CE2496` | 153 | `5d7191a2c9b3a52523506ffb1ff204e5aa739098217b749136c632d70e257c2d` |
| `0x0CC28DF–0x0CC2923` | `0x0CE260F` | 68 | `30df0dc52a20b273ad886bd45a78e2ddb9940879647fce017928859f0d5f9949` |

Disassembled with GNU `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma` on windows cut at independently checked instruction starts. The third window shows the nearby call `0x0CC28E9 → 0x03090310` and context `+0x318/+0x32C/+0x350` updates; it does **not** establish the callee's semantic name.

**High confidence:** exact resource/context field accesses, compare/convert control flow, capacity check and 12-byte command payload. **Moderate:** this is a state-cache optimization. **Unresolved:** resource C++ type, physical GPU register corresponding to command constant, hash-table purpose, owning function FDE, and actual runtime execution.

**Next:** trace upstream producers of resource float `+0x20` and consumers of context `+0x51C`, or independently decode `0x03090310` to determine the immediately adjacent graphics-state transition. Avoid naming the state as depth bias, sample mask, or viewport parameter without additional evidence.
