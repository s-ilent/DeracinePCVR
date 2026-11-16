# OpenXR-native presentation resolution

How big the picture handed to the headset is, separately from how big the title draws it. The
immediate target is a Steam Frame runtime's 3036×3036 an eye; the mechanism is the runtime's own
per-view recommendation, so any headset works.

Branch `feature/openxr-native-resolution` (on top of `fix/hmd-eye-frustum-from-layout`) in
`M:\Development\Deracine-work\wt-projection`.

---

## 1. What was wrong before

`vk_presenter.cpp` derived the OpenXR target from the *title's* eye size:

```
target = guest;
if (SHADPS4_VR_SCALE)        target = round(guest * scale);
else if (SHADPS4_TITLE_EYE_WIDTH > guest) target = {width, guest.height * width / guest.width};
target = align16(target);
```

so the headset was shown the guest's own picture, or a uniformly scaled copy of it, and its shape
was always the guest's 8:9. A runtime asking for 3036×3036 square could not be honoured: asking for
`resolution=3036` produced 3040×3424 an eye (12.9 % more pixels, still 8:9). The recommendation was
read and thrown away.

## 2. What it is now

```
target = PlanPresentationSize(guest, recommended, limits, request);
```

- **Recommended mode (default).** The runtime's `recommendedImageRectWidth/Height` from
  `xrEnumerateViewConfigurationViews`, used **verbatim** — no 16-pixel rounding, because 3036 is what
  Steam Frame asks for and 3040 would be asking for more than it recommended.
- **Explicit mode.** A width and/or height chosen by the user, rounded up to 16 as before; a width
  alone keeps the title's aspect.
- **Guest mode.** The title's own eye size, for when the runtime recommends nothing.
- **Supersampling** multiplies whichever of the above was chosen, on both axes, and never changes
  the mode.
- **Limits.** `XrViewConfigurationView::maxImageRectWidth/Height` and
  `XrSystemGraphicsProperties::maxSwapchainImageWidth/Height` cut the result down. The two eyes
  travel side by side in one image, so an eye may be at most half the image width. Clamping is
  reported in the log.

The title's render resolution is untouched by all of it: `render_percent` / the game's own
`r.ScreenPercentage` decide what the guest draws, and the presentation size only decides how many
pixels carry that picture to the compositor.

### 2.1 Interface

| variable | meaning |
|---|---|
| `SHADPS4_VR_RESOLUTION` | `auto` (default), `guest`, `<width>`, or `<width>x<height>` |
| `SHADPS4_VR_SUPERSAMPLE` | multiplier on the chosen size, 0.1 … 8 (default 1.0) |
| `SHADPS4_TITLE_EYE_WIDTH` | legacy: an explicit per-eye width, still only ever enlarging (kept) |
| `SHADPS4_VR_SCALE` | legacy: multiplier on the title's own size, floor 0.5 (kept) |

Precedence: a set `SHADPS4_VR_RESOLUTION` wins, then the legacy pair (`VR_SCALE` before
`TITLE_EYE_WIDTH`, as before), then `auto`. `SHADPS4_VR_SUPERSAMPLE` overrides the legacy scale as
the multiplier. The launcher's `resolution=<width>` keeps producing `SHADPS4_TITLE_EYE_WIDTH`, so
existing settings behave exactly as they did; `resolution=auto` would mean "let the headset
decide".

The recommendation is re-read whenever the runtime re-acquires its system (headset change, runtime
restart), the same place the swapchain maxima were already refreshed.

### 2.2 Logging

One line whenever either size changes, naming both unambiguously:

```
VR: the title draws 2880x3240 an eye; the headset is shown 3036x3036 an eye (the runtime's size), 6072x3036 for both
```

and, from the runtime: `The runtime asks for 3036x3036 an eye (up to 8192x8192, 1 samples down to
1), 2 views; an image of up to 8192x8192 holds both eyes`.

## 3. Acceptance test, measured

Same scene, same build, three internal sizes, one output. The guest's internal size is its own
`r.ScreenPercentage` (see §5); the presentation size is `SHADPS4_VR_RESOLUTION=auto`.

| internal render (guest, per eye) | OpenXR presentation (per eye) | swapchain image | log |
|---|---|---|---|
| 2880×3240 (SBS 5760×3240) | 3036×3036 | 6072×3036 | `run-res300b` |
| 3072×3456 (SBS 6144×3456) | 3036×3036 | 6072×3036 | `run-res320` |
| 3840×4320 (SBS 7680×4320) | 3036×3036 | 6072×3036 | `run-res400` |

Verbatim from the three runs:

```
VR: the title draws 2880x3240 an eye; the headset is shown 3036x3036 an eye (the runtime's size), 6072x3036 for both
Frames are shown through 3 images of 6072x3036, R8G8B8A8Srgb
The title's first picture handed to the headset: 3036x3036 an eye, field of view 50.4/49.8/51.6/51.6 degrees (out, in, up, down)

VR: the title draws 3072x3456 an eye; the headset is shown 3036x3036 an eye (the runtime's size), 6072x3036 for both
frame 0: eye 6144x3456 at 0x2018000000 / 0x2018000000, tan out/in/top/bottom = 1.2074/1.1813/1.2629/1.2629

VR: the title draws 3840x4320 an eye; the headset is shown 3036x3036 an eye (the runtime's size), 6072x3036 for both
frame 0: eye 7680x4320 at 0x2018000000 / 0x2018000000, tan out/in/top/bottom = 1.2074/1.1813/1.2629/1.2629
The title's first picture handed to the headset: 3036x3036 an eye, field of view 50.4/49.8/51.6/51.6 degrees (out, in, up, down)
```

Reading it: the guest's eye size moves (5760/6144/7680 across, 3240/3456/4320 up), the swapchain
does not (6072×3036), and the declared field of view is byte-identical in all three — the frustum
comes from the title's reprojection UVs (`hmd_reprojection.cpp`), not from the output size. The
layer's sub-image covers the whole eye rectangle, so there is no letterbox by construction; the
projection suite pins that a full-rect resample is exact whatever the rectangle's size or aspect
(`shadps4_vr_projection_math_test`, and `VrPresentationSize.EveryAcceptedPresentationSizeKeepsTheGeometryExact`).

### 3.1 Overrides and limits, also measured

```
VR: the title draws 2880x3240 an eye; the headset is shown 3600x3600 an eye (the size asked for), 7200x3600 for both
Frames are shown through 3 images of 7200x3600, R8G8B8A8Srgb
The title's first picture handed to the headset: 3600x3600 an eye, field of view 50.4/49.8/51.6/51.6 degrees

VR: the title draws 2880x3240 an eye; the headset is shown 2880x3248 an eye (the size asked for), 5760x3248 for both
Frames are shown through 3 images of 5760x3248, R8G8B8A8Srgb        <- legacy TITLE_EYE_WIDTH=2880: unchanged

VR: the title draws 2880x3240 an eye; the headset is shown 4096x4554 an eye (the runtime's size, 1.50x, cut to the runtime's maximum), 8192x4554 for both
Frames are shown through 3 images of 8192x4554, R8G8B8A8Srgb        <- 1.5x of 3036 would need 9108: cut to 8192
```

The last one is the requirement that nothing exceeds the runtime's maxima: 8192 is exactly
`maxSwapchainImageWidth`, and 4554 stays under `maxSwapchainImageHeight`/`maxImageRectHeight`.

## 4. Tests

`shadps4_vr_presentation_size_test` — 10 cases, no GPU (`src/tests/test_vr_presentation_size.cpp`,
`src/src/core/vr/presentation_size.h`):

- the three acceptance internal sizes all yield 3036×3036 and are **not** derived from the guest
  aspect (a square output from an 8:9 source);
- guest mode keeps the old 16-pixel alignment (2880×3240 → 2880×3248), explicit mode keeps the
  title's aspect from a width alone, explicit both-axes is aligned;
- supersampling multiplies the choice and keeps its mode; a 0.5× shrink aligns as it always did;
- a recommendation nobody made falls back to the guest size and reports mode `Guest`;
- the runtime maxima cut the request and set `clamped` (both the per-eye and the half-image-width
  cases);
- for every accepted pair, the resample is exact (0.0°), while the sampling densities are
  redistributed by the ratio of the two aspects.

## 5. How the internal sizes were produced (and one negative result)

The guest chooses its own eye size; nothing in this change touches it. To reach 3072×3456 and
3840×4320 the title's `r.ScreenPercentage` must go above 300, which the guest clamps with a
`vminss 300.0f` — the investigation patch from the render-quality workstream
(`D:\Temp\Deracine\CUSA11919-qual\quality-cap400.xml`, "Render scale cap 300 percent") lifts it to
400. The acceptance runs used that patch, the quality lab's shadow game (whose `gameproject\saved`
is its own, so no real game file was touched) and its `engine.ini` set to 300/320/400.

**Negative result, worth recording:** `SHADPS4_DBG_HMD_PANEL` (reporting a different panel size to
the guest) does **not** change the title's eye buffers — 1024×1152 and 1280×1440 both still drew
2880×3240. The guest sizes its Morpheus path from its own constants, not from the emulated
`sceHmdGetDeviceInformation` panel. That answers the question the probe was added for
(`hmd.cpp:46-57`).

## 6. Sampling: the interface with the downsampling work

This change sizes the picture and resamples nothing new: when the guest is larger than the output
(3840×4320 → 3036×3036, a 0.70× vertical minification) the existing post-processing pass does the
minification with its bilinear sampler. The render-quality workstream
(`investigation/render-quality`) owns the exact area filter behind `SHADPS4_VR_DOWNSAMPLE=1`; this
branch deliberately does not duplicate it.

The interface between the two is one sentence: **the filter applies when the source eye image is
larger than the presentation rectangle, and the rectangle is now independent of the source**, so the
filter's trigger must be `guest_width > target_eye_width` (not "presentation cap is on"). When both
branches land, the cases to check are 320 % and 400 % internal with `auto` presentation, where the
minification is 1.01×–1.27× across and 1.14×–1.42× up — the first time the area filter would have a
real job.

## 7. Asymmetric configurations (investigated, not implemented)

Nothing here assumes the two eyes are the same size except one place: the copy path writes both
eyes into one side-by-side image and `FillProjection` splits it in half. For asymmetric
configurations the order to try, before reaching for per-view swapchains:

1. **Per-view `imageRect` in one swapchain** — already expressible; the only change would be
   computing two rectangles instead of half-width halves.
2. **One swapchain with `arraySize = viewCount`**, one layer per view, `subImage.imageArrayIndex`
   selecting it — the shape runtimes expect, and it lets each view carry its own `imageRect` inside
   a common slice size.
3. **One swapchain per view** — only when per-view *pixel* sizes must genuinely differ.

`XrViewConfigurationView` is already stored per view in `OpenXrHost::ViewSizes` (recommended and
maximum rectangle, sample counts), so any of the three can be built on it.

## 8. Files

| file | change |
|---|---|
| `src/src/core/vr/presentation_size.h` | new: the size policy, standard library only |
| `src/src/core/vr/openxr_host.h/.cpp` | new: `EyeSizes()` — recommendation, maxima, sample counts, per view |
| `src/src/video_core/renderer_vulkan/vk_presenter.cpp` | the sizing block: modes, legacy knobs, limits, the log line |
| `src/tests/test_vr_presentation_size.cpp` | new: 10 policy cases |
| `src/tests/CMakeLists.txt` | one new target |
