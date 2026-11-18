# The 32³ LUT at 0x2008f80000: its sampler state, and whether the 2D↔3D conversions preserve it

Guest resource: colour-grading LUT, `0x2008f80000`, 128 KiB, `A2B10G10R10UnormPack32`, produced as a
32-layer 2D array and sampled as a 32³ volume.

- Sampler state: **pending one run** (§4). The diagnostic that reports it is in the tree (§1).
- 2D↔3D conversions: **preserve the texels and the slice order** — 1:1, ordered, no truncation, no
  stale-data path for this resource (§3).

## 1. The diagnostic

`SHADPS4_DBG_SAMPLER=1` enables `SamplerStateDiag`
(`src/src/video_core/renderer_vulkan/sampler_state_diag.h`), called from the single place samplers
are bound, `Rasterizer::BindTextures` (`src/src/video_core/renderer_vulkan/vk_rasterizer.cpp`). It
prints one line per unique binding — stage, program hash, binding index, S# words, and the paired T#
— capped at 400 lines, and is off by default. Its value picks the scope:

- `SHADPS4_DBG_SAMPLER=1` — every unique binding, capped.
- `SHADPS4_DBG_SAMPLER=<guest address>` — only bindings whose paired texture is at that address, so
  one resource can be asked about without depending on the cap.

```
SAMPLER_STATE shader=<pgm_hash> stage=<stage> binding=<n> origin=inline|sgpr:<i> aniso_off=<0|1>
  raw0=<hex> raw1=<hex> clamp_x=<n> clamp_y=<n> clamp_z=<n> names=<X>/<Y>/<Z>
  vulkan=<U>/<V>/<W> tex=addr=<hex> type=<ImageType> <w>x<h>x<d> layers=<n> levels=<n> volume=<0|1>
```

- `raw*` is the guest S# as uploaded; `clamp_x/y/z` are its 3-bit clamp fields (word 0 bits 0-2 /
  3-5 / 6-8), `names` is the `AmdGpu::ClampMode` each decodes to, and `vulkan` is what
  `LiverpoolToVK::ClampMode` passes as `addressModeU/V/W`.
- For a volume the third coordinate is the one `addressModeW` transforms, so **`clamp_z` / the W
  entry** is the value that decides the answer.
- `origin` says whether the words came from the shader's inline S# or from a user-data pair
  (`sgpr:<index>`), which is how the consumer's `fs_sampsgpr_48` reads.

For this resource, look for `type=Color3D` or `tex=addr=0x2008f80000`.

## 2. Existing instrumentation used for the conversions

No new tracing framework was added for this; the main-branch hooks are sufficient:

- `SHADPS4_DBG_TRACE_IMAGES=1` — every image the cache takes on or lets go of
  (`IMAGES … at addr+size WxH fmt levels N layers N`). The LUT appears as `32x32 … layers 32`
  (2D array) and `32x32 … layers 1` (3D volume), so the two conversions show up as the layer count
  flipping at that address.
- `SHADPS4_DBG_OVERLAP=1` — the per-second image/copy/refresh counters (`ImageStats`).
- `SHADPS4_DBG_XFER=1` — GPU timestamps around the full-surface transfers.
- `SHADPS4_DBG_RDOC_CAPTURE=1` — RenderDoc capture, for reading the volume's slices out of the
  frame.

The old line's per-request `ALIAS seq=` trace is **not** on main (it lives on the pre-rewrite
`diag/lut-dump` branch) and was deliberately not ported.

## 3. Do the conversions preserve the LUT's texels and slice order?

**Yes.** The conversion is an identity mapping over all 32 slices, and nothing on this path can skip
or reorder them.

- **Region mapping** (`Image::CopyImage`, `src/src/video_core/texture_cache/image.cpp:882-892`):
  - 2D array → 3D volume: `src.layerCount = 32`, `dst.layerCount = 1`, `extent = (32, 32, 32)`,
    `baseArrayLayer = 0` on both sides — layer *L* becomes slice *L*.
  - 3D volume → 2D array: the mirror (`src.layerCount = 1`, `dst.layerCount = 32`,
    `extent.depth = 32`).
  - `base_depth` takes the 3D side's depth whichever way the copy runs
    (`image.cpp:826-827`), so the depth is the volume's 32 in both directions.
  - Vulkan requires `src.layerCount == extent.depth` for a 2D→3D copy and
    `dst.layerCount == extent.depth` for the reverse; both hold here, which is what makes the
    count check below meaningful.
- **No coercion fired for this resource.** `SanitizeCopyLayers` (`image.cpp:698-743`) warns whenever
  it has to force the counts to agree. The preserved classroom run has 11 `Coercing copy` warnings
  and all 11 are the *same-type* "source layers N and destination layers M to minimum" variant with
  N/M between 1 and 8 — array/cube resources, not this one. There is no 2D↔3D depth coercion at all,
  so the LUT took the exact-match branch (`32 == 32`): nothing padded, nothing dropped.
- **Ordering is safe.** `ExpandImage` (`texture_cache.cpp:796-808`) refreshes the new image *first*
  and copies *after*, so the copied GPU contents win over the guest-memory upload, and the copy only
  runs at all when the source really holds GPU-written data (`carries_contents` — `SafeToDownload()`
  or multisampled), which a slice-rendered LUT does. The coverage skip that could leave a level
  un-uploaded is opt-in and off by default (`SHADPS4_DBG_COVERED_REFRESH`).
- **Independent confirmation of the direction the consumer reads.** The earlier full 32-slice dump of
  the volume showed 32/32 unique, smooth slices with no truncation or duplication
  (`LUT-CLOSEOUT.md`).

Residual, stated plainly: this is code + log + earlier-dump evidence for the 2D→3D direction, which
is the one the sampler reads. The reverse copy is the mirror of the same region math, and its result
is overwritten by the producer pass, which is configured for all 32 layers (the `SHADPS4_DBG_LUT_AXIS`
flip changed the scene, so the slices are written rather than only slice 0). A capture that hashes the
volume's slices before and after a round trip would close that last gap; it is not needed to explain
the observed colours.

## 4. What is still needed

One run with `SHADPS4_DBG_SAMPLER=0x2008f80000` (add `SHADPS4_DBG_TRACE_IMAGES=1` if the
conversion sequence should be visible beside it), then:

```
Select-String -Path <log> -Pattern 'SAMPLER_STATE'
```

The three numbers it prints settle the remaining question: the LUT coordinate is
`31/32 · c + 0.5/32`, which leaves `[0,1]` as soon as `c > 1`, and one of the two branches feeding
`c` in the consumer shader (`consumer-29090b93.spvasm`) is not clamped — so `clamp_z` decides
whether such a lookup wraps to the opposite slice or clamps to the last one.
