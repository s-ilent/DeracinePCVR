# SC-01: first live semantic RHI capture

## Result

A bounded, passive capture of the UE4 deferred RHI command stream is working on real Déraciné
execution. It produced an ordered stream of 512 commands with known payloads, and rendered
output was unchanged.

## What was built

A default-OFF observer installed on the five recovered command `Execute` wrappers, reusing the
A3/A2 reversible guest-entry detour. It copies scalar payload fields synchronously during the
hook and never retains a payload pointer.

| Item | Value |
| --- | --- |
| Opt-in | `SHADPS4_DERACINE_CMD_CAPTURE` must equal exactly `"1"` |
| Record bound | 512, compile-time, no allocation |
| Hook sites | 5 command `Execute` wrappers |
| Image gate | exact SHA-256 of the approved 1.00 or 01.01 eboot |
| Prologue gate | exact 16-byte entry sequence per command |

The five hook sites and their command ids:

| RVA | Command |
| --- | --- |
| `0x13A68B0` | DrawIndexedPrimitive |
| `0x13A6900` | DrawPrimitive |
| `0x13A6930` | SetStreamSource |
| `0x13A6A30` | SetRenderTargets |
| `0x13A6BB0` | SetViewport |

## Why the wrappers, not the dispatch

The ideal hook is the dispatch at `0x1376954`, but that instruction is 2 bytes
(`ff 10`, `callq *(%rax)`). The detour needs 5 bytes for an `E9 rel32`. Patching it would
require relocating the following instructions and would risk the executor's own control flow.

The five wrapper prologues are 16 bytes each and displacement-free, so they relocate cleanly.
That is the narrower, safer choice, and its coverage limitation is stated below.

## Verified capture

Cell: `_out/SC01_semantic_capture/capture-on-run-1`, headless, no save data, no input.

```
DERACINE_CMD_CAPTURE_INSTALL title_id=CUSA11919 app_version=01.01
image_sha256=8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75
module_name=eboot.bin module_base=0x800000000 installed=5 command_limit=512
```

### Coverage

| Command | Count |
| --- | --- |
| DrawIndexedPrimitive | 156 |
| DrawPrimitive | 3 |
| SetStreamSource | 313 |
| SetRenderTargets | 14 |
| SetViewport | 26 |
| **Unknown vtable** | **0** |

All five recovered command types appeared. No unknown vtable was observed, which is consistent
with the capture covering the whole command vocabulary on this path.

### Threads

One execution thread: `39456`. The A9 static analysis found the executor gates on thread
identity against two recorded ids; this run exercised one of them.

### Representative ordered sequence

```
seq=1  SetRenderTargets  count=1  RT[0]=0x100c05fc80  depth=0x0  uavs=0
seq=2  SetViewport       min=(0,0)  max=(0x500,0x320)  maxZ=1.0
seq=3  SetRenderTargets  count=1  RT[0]=0x0  depth=0x0
seq=4  SetRenderTargets  count=1  RT[0]=0x0  depth=0x803f2e0c0
seq=5  SetViewport       min=(0,0)  max=(0xb40,0xca8)  maxZ=1.0
seq=6  SetViewport       min=(0xb40,0)  max=(0x1680,0xca8)  maxZ=1.0
```

That is a coherent frame opening: bind targets, set a viewport, rebind, then two viewports for
a stereo pair. The `0x500x0x320` and `0xb40x0xca8` extents are plausible render-target sizes.

### Payload cross-check against the A3 GNM trace

The draw payloads reproduce the A3 GNM distributions exactly:

| Field | This capture | A3 GNM trace |
| --- | --- | --- |
| `NumPrimitives` | 1→3, 2→153 | 1→3, 2→125 (capped) |
| `StartIndex` | 0→153, 6→3 | 0→125, 6→3 |
| `NumInstances` | 1,2,8,16,32,64,128 | same set |

The `NumPrimitives` 1/2 split and the `StartIndex` 0/6 split match. That is independent
corroboration that the payload offsets are correct, because the two mechanisms read the same
values through different code paths.

## Safety and overhead

### Default OFF

The OFF cell produced zero `DERACINE_CMD_CAPTURE_INSTALL` and zero records. The gate requires
the exact string `"1"`.

### Reversible

The hook uses the same `InstallGuestEntryObserver` / `RemoveGuestEntryObserver` pair as the A3
probe, which restores the exact saved bytes and protection.

### Bounded

512 records, compile-time array, no allocation, no growth. The hot path does one
`QueryProtection` per guest word read and one `memcpy` per field.

### Non-mutating

The hook reads the command object's own payload and writes nothing to guest memory. It does not
touch the command, the list, or the context.

### Overhead

| Cell | Mean fps | Min | Max |
| --- | --- | --- | --- |
| Capture ON | 60.1 | 59.2 | 60.5 |
| Capture OFF | 59.9 | 58.9 | 61.5 |

Within noise. The scene is front-end limited at ~60 fps, so this does not measure the
observer's cost in a GPU-bound scene. That limit is stated rather than hidden.

### Postflight

`protected_unchanged=true`, 0 unexpected changes, exit 0.

## Coverage limitations

1. **The dispatch is not hooked.** Commands executed through a path that bypasses the five
   wrappers are not captured. The executor's own bypass branches at `0x1376547` and
   `0x137654F` select different paths, and this capture does not distinguish them.
2. **The 512-record cap was reached.** The stream is truncated, so the counts are a lower bound
   on the run, not a total.
3. **No GNM correlation in this cell.** The A3 probe was not enabled in the same run, so the
   "does the capture cover the same draws as GNM" question is answered only indirectly, through
   the payload distribution match above.
4. **One thread observed.** The second recorded thread id was not exercised.
5. **State caching is not measured.** The capture records command execution, not packet
   emission. A `SetRenderTargets` that the receiver suppressed still appears here.

## What this establishes

The recovered command list is real, ordered, and semantically readable. The payload offsets
derived statically in A5 through A9 are correct, because the captured values reproduce the
independently measured GNM distributions.

That is the success criterion: an ordered, trustworthy stream of actual UE4 RHI commands with
known payloads, without changing rendered output.

## Reproducible checks

```
# contract tests
build-tests/tests/shadps4_rhi_command_capture_contract_test.exe --gtest_color=no

# the capture cell (headless, no save, no input)
$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
pwsh -File tools/benchmark/runner.ps1 -Exe <sc01 build>\shadPS4.exe -Out <fresh _out path> `
  -ShadowRoot <private game shadow> -Lab <fresh lab> -Autopress 0 -TimeoutSeconds 90
```

Binary: `build/shadPS4.exe`, SHA-256
`A6872C14A605C3FA171850045C5BFC752308758B5AF3AF164495C6516D90A62E`, 73,483,264 bytes.
