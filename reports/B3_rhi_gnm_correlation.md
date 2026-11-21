# Déraciné: UE4 RHI entry to GNM draw correlation

Runtime evidence, headless, no save data, no input, no HMD.

## Result

The UE4 RHI candidate at `eboot.bin + 0x0CBB9C0` is the immediate caller of
`sceGnmDrawIndex`. Every observed entry produced exactly one indexed draw, on the same
thread, within microseconds. The resulting `PM4CmdDrawIndex2` packet carries the same index
count and index address that the GNM call supplied.

## Run

- Binary: `wt-ue4-rhi-a2/build/shadPS4.exe`, SHA-256
  `bf72bac0ba1e227c2b824af72b045b3520508404662f832b063510f430eb4001`, 73,466,368 bytes.
- Environment: `SHADPS4_VR=1`, `SHADPS4_OPENXR=0`, `SHADPS4_VR_DEMO=0`,
  `SHADPS4_DERACINE_RHI_PROBE=1`.
- Runner: `tools/benchmark/runner.ps1 -Autopress 0 -TimeoutSeconds 90`.
- Output: `_out/B3_semantic_capture/headless-rhi-probe-on-20261121-run-3`.
- The classroom scene gate timed out, as expected without autopress. That is not a failure of
  this experiment: the front end renders continuously and the trace was captured there.
- Postflight: `protected_unchanged=true`, 0 unexpected changes, exit 0.

## Install

```
DERACINE_RHI_PROBE_INSTALL title_id=CUSA11919 app_version=01.01
image_sha256=8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75
module_name=eboot.bin module_base=0x800000000 target_rva=0xcbb9c0 target=0x800cbb9c0
exec_pt_load_rva=0x0 exec_pt_load_filesz=0x3e86d30 exec_pt_load_memsz=0x3e86d30 event_limit=128
```

The image hash is the pinned CUSA11919 01.01 EBOOT. The target is inside the file-backed
executable PT_LOAD.

## Counts

| Item | Value |
| --- | --- |
| RHI entries | 128 (cap reached) |
| GNM draw records | 128 |
| PM4 DrawIndex2 records | 128 (cap reached) |
| Paired RHI→GNM 1:1 | 128 |
| RHI thread | 15136 |
| GNM thread | 15136 |
| PM4 thread | 25652 (GpuCommandProcessor) |
| Pairing delta | 1–68 µs, mean 3.3 µs |
| Caller | `0x8013a68e8` (RVA `0x13A68E8`), all entries |

Pairing is same-thread and strictly ordered: RHI sequences 1..128 map to GNM sequences 1..128.
PM4 decode runs on the GPU thread, so it is paired by value rather than by sequence.

## Argument correspondence

The entry stack words are `StartIndex`, `NumPrimitives`, `NumInstances` at RSP `+0x8`,
`+0x10`, `+0x18`. The GNM call receives `(cmdbuf, size, index_count, index_addr, flags, type)`.

The decisive relation holds for all 128 pairs:

```
NumPrimitives * 3 == index_count
```

| NumPrimitives | index_count | Count |
| --- | --- | --- |
| 1 | 3 | 3 |
| 2 | 6 | 125 |

So the UE4 entry's primitive count is the GNM index count divided by three, i.e. the draw is a
triangle list. `index_count` is derived from the primitive count, not passed through.

Other observed values:

- `StartIndex`: 0 (125 entries), 6 (3 entries).
- `NumInstances`: 1, 2, 8, 16, 32, 64, 128. Distribution: 32→50, 8→30, 16→15, 64→13, 128→11,
  2→6, 1→3.
- `gpr_rdi`: constant `0x1004650020` across all entries.
- `gpr_rsi`: two values, `0x10044e49e8` (3 entries) and `0x10044e8688` (125 entries).
- `gpr_r9`: 3 when `NumPrimitives` is 1, 4 when it is 2. It tracks the primitive topology, not
  the count.
- GNM `flags`: `0x0` for all records. GNM `type`: `0x1` for all records.
- GNM `size`: 10 for all records, matching the `PM4CmdDrawIndex2` packet size.
- `index_addr`: `0x200004280c` (3 entries) and `0x20000c0000` (125 entries).

## PM4 packet correspondence

The decoded `PM4CmdDrawIndex2` packets carry the GNM-supplied values unchanged:

| GNM `index_count` / `index_addr` | GNM count | PM4 `index_count` / `index_base` | PM4 count |
| --- | --- | --- | --- |
| 3 / `0x200004280c` | 3 | 3 / `0x200004280c` | 3 |
| 6 / `0x20000c0000` | 125 | 6 / `0x20000c0000` | 121 |

Both distinct GNM argument pairs appear in the PM4 stream with identical index count and index
base. The 4-record shortfall in the second row is the PM4 cap: the GPU thread decoded 128
packets, and the remaining 4 of the 125 six-index draws fell outside the captured window. The
PM4 stream also contains draws with counts 78 and 84 that never appear in the GNM records,
because those were issued before the RHI probe's first captured entry.

`draw_initiator` is `0x0` for all captured packets, consistent with the guest passing
`flags=0x0`.

## What this establishes

- The RHI candidate is a real per-draw UE4-side function, not a one-off or an init path.
- It calls `sceGnmDrawIndex` directly, once per draw, on the RHI thread.
- Its primitive count determines the GNM index count by a factor of three.
- The GNM call happens before packet construction, so this is a pre-packet observation.
- The packet that reaches the GPU carries the same index count and index base, so the draw is
  not rewritten between the GNM call and decode.

## What this does not establish

- The UE4 symbol name of the caller. The executable is stripped; the identity is by RVA and
  call relation only.
- That every GNM call produced a packet. The PM4 record is capped at 128 and the GPU thread
  decodes a superset of the captured GNM calls.
- Anything about OpenXR or VR presentation. This run used the headless path.
- Gameplay behaviour. No save data and no input were used.

## Reproduction

```
$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_DERACINE_RHI_PROBE='1'
pwsh -File tools/benchmark/runner.ps1 -Exe <a2 build>\shadPS4.exe -Out <fresh _out path> `
  -ShadowRoot D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17 -Lab <fresh lab> `
  -Autopress 0 -TimeoutSeconds 90
```

The scene gate is expected to time out. Read `shad_log.txt` for the trace.
