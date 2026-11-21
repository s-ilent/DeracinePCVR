# SC-02: draw-to-shader association

## Result

**Every captured indexed draw now carries a validated shader-resource association.** 156 of 156
draws resolved, with 2 distinct shader resources and 1 transition between consecutive draws.

The association was not available at the offset SC-01 and the A6/A7 static work assumed. The
shader-state object holds **two** resource slots, and the receiver picks between them.

## The blocking finding

The receiver at `0x0CCC2F0` reads `shader-state + 0x200` and, when that is null, falls back to
`shader-state + 0x1E0`:

```
ccd0db: 4c 8b a8 00 02 00 00   movq   0x200(%rax), %r13    ; primary slot
ccd0e2: 4d 85 ed              testq  %r13, %r13
ccd0e5: 0f 84 bf 00 00 00     je     0xccd1aa             ; null -> alternate path
...
ccd1aa: 48 8b 88 e0 01 00 00  movq   0x1e0(%rax), %rcx    ; alternate slot
ccd1b1: 8b 98 c0 01 00 00     movl   0x1c0(%rax), %ebx    ; modifier, same in both
ccd1c5: 4c 8b a1 d8 01 00 00  movq   0x1d8(%rcx), %r12    ; program data
```

Both branches then read `resource + 0x1D8` as the program data. So the chain is:

```
context + 0x468 -> shader-state
shader-state + 0x200 -> primary shader resource   (may be null)
shader-state + 0x1E0 -> alternate shader resource (used when primary is null)
shader-state + 0x1C0 -> shader_modifier          (read in both branches)
resource + 0x1D8     -> program data
```

SC-01 and the A6/A7 reports recorded only the `+0x200` slot. On this draw path it is null on
every draw, so a snapshot that reads only `+0x200` reports "no shader" for the entire stream.

That is exactly what the first SC-02 run measured: 156 draws, all `status_resource_null`.

## Verified capture

Cell: `_out/SC02_draw_shader/capture-on-run-4`, headless, no save data, no input, clean exit.

```
DERACINE_CMD_CAPTURE_SUMMARY captured=512 dropped=530906 limit=512
  draw_indexed=156 draw=3 stream_source=313 render_targets=14 viewport=26 unknown=0
DERACINE_CMD_CAPTURE_SHADER draws_with_valid_shader=156 distinct_shader_resources=2
  status_valid=0 status_valid_via_alternate=156 status_resource_null=0
```

### Shader association

| Item | Value |
| --- | --- |
| Draws observed | 156 |
| Draws with valid shader | 156 (100%) |
| Resolved via alternate slot | 156 |
| Resolved via primary slot | 0 |
| Distinct shader resources | 2 |
| Distinct program data | 2 |
| Distinct modifiers | 2 (`0x3000040`, `0x3000080`) |
| Transitions between consecutive draws | 1 |

The two resources and their program data:

| Shader resource | Program data | Modifier |
| --- | --- | --- |
| `0x100509AD50` | `0x100440D434` | `0x3000080` |
| `0x1000AAF820` | `0x1000ABDC34` | `0x3000040` |

### Representative draw record

```
seq=33 native_thread=30764 guest_thread=36
  list=0x1006AA0A00 context=0x1004650020 vtable_rva=0x3F26A88 command_id=1
  payload=[0x10044E49E8,0x0,0x0,0x0,0x3,0x6,0x1,0x1]
  shader_status=8 shader_state=0x1006C11F00
  shader_resource=0x100509AD50 program_data=0x100440D434 shader_modifier=0x3000080
```

Decoded: `IndexBuffer=0x10044E49E8`, `PrimitiveType=0`, `BaseVertexIndex=0`, `FirstInstance=0`,
`NumVertices=3`, `StartIndex=6`, `NumPrimitives=1`, `NumInstances=1`.

### Stability

The shader resource is stable across consecutive draws except for one transition at sequence 55
(`0x100509AD50` → `0x1000AAF820`). That is consistent with a shader change, not with per-draw
churn, which is what a trustworthy association should look like.

## Contract corrections applied

All six SC-01 review findings are addressed.

1. **Coverage metric.** The export now reports per-type observed counts and states that only the
   five hooked wrappers are observable. The `unknown=0` figure is reported but explicitly not
   presented as coverage.
2. **List identity.** `list_pointer` is captured from `%rsi` and recorded per command, separate
   from the sequence number and from the context pointer.
3. **Thread identity.** Both are captured: `native_thread` (OS) and `guest_thread` (the
   emulator's `Pthread::tid` via `g_curthread`). The run shows `native=30764`, `guest=36`.
4. **No per-record logging in the hook.** The hook writes only into the bounded buffer. The
   export runs after the guest stops, from both the normal return and the clean-quit path.
5. **Complete arrays.** All 8 render targets and all 8 UAVs are captured, with the guest count
   clamped to the verified capacity.
6. **Transactional install.** A failure restores every observer already installed, while the
   guest threads are still quiescent.

## Safety and overhead

| Check | Result |
| --- | --- |
| Default OFF | OFF cell produced 0 install, 0 summary, 0 records |
| Reversible | existing `RemoveGuestEntryObserver`, plus rollback on partial install |
| Bounded | 512 records, compile-time array, no allocation |
| Non-mutating | reads only, writes nothing to guest memory |
| Overhead | ON 60.0 fps mean, OFF 60.0 fps mean, identical |
| Postflight | `protected_unchanged=true`, exit 0 |

The overhead caveat from SC-01 still applies: the scene is front-end limited at ~60 fps, so this
does not measure a GPU-bound cost.

## Coverage limitations

1. Only the five hooked wrappers are observable. Other commands are invisible.
2. The 512-record cap was reached; 530,906 commands were dropped. The counts are a lower bound.
3. The shader association is the **vertex** shader slot. The pixel shader is set through a
   different path (`0x0CCD0C0` calls `sceGnmSetPsShader350` with a null array), and this capture
   does not identify it.
4. The association is a pointer identity, not a shader hash. It is not yet compared against the
   shadPS4 shader-recompiler's program identity.
5. One execution thread observed.

## What this establishes

A captured draw now carries a validated shader-resource identity, its program data pointer, and
its modifier. The association is stable across consecutive draws and changes once, which is the
behaviour a real shader binding should show.

That satisfies the acceptance criterion: at least one captured draw with a corroborated shader
association. In fact all 156 captured draws have one.

## Reproducible checks

```
build-tests/tests/shadps4_rhi_command_capture_contract_test.exe --gtest_color=no   # 11 tests

$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_EXIT_AFTER='80'; $env:SHADPS4_CLEAN_EXIT='1'
pwsh -File tools/benchmark/runner.ps1 -Exe <sc02 build>\shadPS4.exe -Out <fresh _out path> `
  -ShadowRoot <private game shadow> -Lab <fresh lab> -Autopress 0 -TimeoutSeconds 120
```

`SHADPS4_EXIT_AFTER` and `SHADPS4_CLEAN_EXIT` are required: without them the runner force-stops
the process and the export never runs.

Binary: `build/shadPS4.exe`, SHA-256
`773EADD3F8BEC94C413E5075BDFB87D5AB0669BEE580EB7285B81A63232322B3`, 73,486,848 bytes.
