# Deracine RHI probe: address map and trace index

Derived from the preserved raw logs. All values are copied from the logs, not inferred.

The raw logs are not committed: `_out/` is gitignored. They are preserved on disk under
`_out/B3_semantic_capture/evidence/`, together with the matching preflight and postflight
manifests. The SHA-256 of each preserved file is listed at the end of this document.

## Module and target

| Item | Value |
| --- | --- |
| Title | CUSA11919, app_version 01.01 |
| Image SHA-256 | `8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` |
| Module | `eboot.bin` |
| Module base | `0x800000000` |
| Target RVA | `0x0CBB9C0` |
| Target VA | `0x800CBB9C0` |
| Executable PT_LOAD | rva `0x0`, filesz `0x3e86d30`, memsz `0x3e86d30` |
| Caller VA | `0x8013A68E8` |
| Caller RVA | `0x13A68E8` |

## Threads

| Role | Thread id |
| --- | --- |
| RHI entry and GNM call | 15136 |
| PM4 decode (GpuCommandProcessor) | 25652 |

## Observed argument values

| Field | Values |
| --- | --- |
| `gpr_rdi` | `0x1004650020` (constant) |
| `gpr_rsi` | `0x10044e49e8` (3), `0x10044e8688` (125) |
| `gpr_r9` | `0x3` (prim=1), `0x4` (prim=2) |
| `StartIndex` | `0x0` (125), `0x6` (3) |
| `NumPrimitives` | `0x1` (3), `0x2` (125) |
| `NumInstances` | 1, 2, 8, 16, 32, 64, 128 |
| GNM `index_addr` | `0x200004280c` (3), `0x20000c0000` (125) |
| GNM `flags` / `type` | `0x0` / `0x1` |
| PM4 `draw_initiator` | `0x0` |

## Raw logs

Preserved on disk under `_out/B3_semantic_capture/evidence/` (gitignored, not committed):

- `headless-rhi-probe-on-20261121-run-2-shad_log.txt` - RHI and GNM records.
- `headless-rhi-probe-on-20261121-run-3-shad_log.txt` - RHI, GNM and PM4 records.
- Matching `-preflight.json` and `-postflight.json` for each run.

| File | SHA-256 |
| --- | --- |
| `run-2-shad_log.txt` | `00FD901E024AA3335D6752AC1CB89CA081E293E827B44839EDEB0C705EC8EDE1` |
| `run-2-preflight.json` | `E85E1BC479D2AE567890DC86161F7801E3974D171081B736E83F021366779709` |
| `run-2-postflight.json` | `9EFF2CC7B6100E1D737F3E9115D8588BEC2093D24B1A48A714CB83B87FB98F21` |
| `run-3-shad_log.txt` | `D8691376922463D730000ED6ECF03767DCA36470E671719E19F22BE41973FF32` |
| `run-3-preflight.json` | `A4A0B87CE676BC02CB56C21DB7EE4C39387FB31A551C42938BC2F75D25E6F27C` |
| `run-3-postflight.json` | `00BD76598221B451B619BD40DF4197D8D65EA12980390C1464A37BE1E975A916` |

## Not established

The UE4 symbol name of the caller is not established. The executable is stripped, so the
identity rests on the RVA above and the observed call relation.
