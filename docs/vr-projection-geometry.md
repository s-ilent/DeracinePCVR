# PSVR → OpenXR projection geometry (Déraciné)

How the title's PlayStation VR eye images reach an OpenXR projection layer, what that mapping is
mathematically, where it is provably correct, where it is not, and how to move from the fixed
Steam Frame dimensions to runtime-provided ones without changing the geometry.

Evidence in this document comes from three places, and each claim says which:

- **code** — the tree at branch `investigation/psvr-projection-geometry` (`acda49e` + this work),
  cited as `file:line`;
- **live** — one recorded session, `pc-vr/user/log/shad_log.txt` (2026‑11‑17 04:23, SteamVR/OpenXR
  2.19.3, Steam Frame, `render_percent=300`, `resolution=2880`, `fov=100`, headset IPD);
- **test** — `shadps4_vr_projection_math_test`, targets in §7 below, whose numbers this document
  quotes verbatim.

---

## 1. The mapping that exists today

### 1.1 The path

```
sceHmdGetFieldOfView            guest: 4 tangents, one set for both eyes          (hmd.cpp:192-217)
  ↑ TitleFov()                  what the title is told to draw for                (vr_runtime.cpp:820-879)
UE4 builds its asymmetric frustum from them, renders side-by-side stereo, and hands the frame back:
sceHmdReprojectionStart         guest: 2 texture descriptors, 2 UV mappings, pose  (hmd_reprojection.cpp:204-293)
  → FovFromUv(uv[0])            the eye image's own frustum, recovered from UVs   (hmd_reprojection.cpp:64-86)
  → SubmitHmdFrame → driver     copy, ToHostSpace(render pose)                    (videoout/driver.cpp:490-580)
  → vk_presenter PrepareHmdFrame  eye size, target size, pp pass                  (vk_presenter.cpp:1105-1276)
  → pp_pass                     one full-rect bilinear stretch per eye            (pp_pass.cpp:327-357)
  → OpenXrHost CopyFrame        copy/blit into the swapchain's eye halves         (openxr_host.cpp:2512-2701)
  → FillProjection              per-eye XrFovf + sub-image rect                   (openxr_host.cpp:2705-2744)
```

The double arrow is the important part: the FOV goes *out* to the title, and the title's own
UV mapping is then inverted to recover the FOV it actually drew. Nothing else about the guest
image is trusted.

### 1.2 The conventions, stated once

`Core::Vr::Fov` (`vr_runtime.h:67-74`) holds tangents of four half-angles: `tan_out` (temple side),
`tan_in` (nose side), `tan_top`, `tan_bottom`, all magnitudes. The frustum is

```
x ∈ [−tan_out, +tan_in]     y ∈ [−tan_bottom, +tan_top]     z = −1      (eye space, +x right, +y up)
```

so one set of numbers describes both eyes, the right being the mirror of the left. This is how
`libSceHmd` reports it and how `FillProjection` mirrors it back:

```cpp
views[0].fov.angleLeft = -atan(tan_out);  views[0].fov.angleRight = +atan(tan_in);   // left eye
views[1].fov.angleLeft = -atan(tan_in);   views[1].fov.angleRight = +atan(tan_out);  // mirror
views[*].fov.angleUp   = +atan(tan_top);  views[*].fov.angleDown  = -atan(tan_bottom);
```

The guest's reprojection UVs are `uv = tan · scale + offset` per eye, in the space of the texture
that eye samples (`hmd.h:81-87`). Inverting at the eye's own edges is exactly the inverse map:
`tan_out = offset_x / scale_x`, `tan_in = (edge − offset_x) / scale_x`, `tan_top = offset_y /
scale_y`, `tan_bottom = (1 − offset_y) / scale_y`.

### 1.3 The mapping is affine in tangent, and that is the whole theorem

A perspective projection divides by depth, so the position in the image is an affine function of
the **tangent** of the viewing angle, not of the angle:

```
tanX(u) = −tan_out + u · (tan_out + tan_in)
tanY(v) = +tan_top − v · (tan_top + tan_bottom)          (v grows downwards)
dir(u, v) = normalize(tanX, tanY, −1)
```

OpenXR's projection layer consumes a rectangle the same way: NDC −1…1 maps affinely onto the
frustum advertised in `XrFovf`. Since `FillProjection` advertises *exactly* the frustum that the
guest drew for, and the sub-image rectangle covers *exactly* the whole eye image, the source
pixel and the destination pixel represent the same ray. The composed map is the identity — not
approximately, exactly, for any rectangle size or aspect.

Both halves of that claim are testable and tested:

- **Guest-side round trip (live evidence).** The title was told `1.20743 / 1.181346 / 1.262872 /
  1.262872` (50.368 / 49.752 / 51.626 / 51.626°). The UVs it handed back for that frame were
  `scale 0.209312 / 0.395923, offset 0.252730 / 0.500000`. Inverting them gives `1.207432 /
  1.181347 / 1.262872 / 1.262872` — agreement to **2e‑6 in tangent**, i.e. under 1e‑4 degrees, which
  is the log's own rounding. The same UVs put that eye's picture exactly at `U ∈ [0, 0.5]`,
  `V ∈ [0, 1]` of the side-by-side texture (`u_left = 0.000000, u_right = 0.500000, v_top =
  0.000000, v_bottom = 1.000000`), so the half image *is* the frustum, edge to edge.
- **Resample exactness (test evidence).** `VrProjectionMath.Identity_FullRectIsExact`,
  `…EightNineSource_SquareDestination_MatchingFov_IsExact` and
  `…LiveConfiguration_TodayAndAtTheRuntimesRecommendation` measure **0.000000°** maximum and mean
  angular error over a 129×129 grid of rays for the live numbers and for an 8:9 source stretched
  into a square rect.

### 1.4 The live configuration, as measured

| quantity | value | source |
|---|---|---|
| guest eye render target | 2880×3240 per eye (8:9), side-by-side in one 5760×3240 texture | live |
| frustum the title draws for | 50.368 / 49.752 / 51.626 / 51.626° (100.12° × 103.25°) | live |
| title's reprojection UV | `scale_x 0.209312, scale_y 0.395923, offset_x 0.252730, offset_y 0.5` | live |
| host buffer (presenter target) | 5760×3248, per eye 2880×3248 | live (`vk_presenter.cpp:1263-1274` log) |
| XR swapchain | 5760×3248, 3 images, `R8G8B8A8Srgb`, `arraySize 1`, two half-rects | live, code |
| declared `XrFovf` per eye | left `[−50.368°, +49.752°]`, right `[−49.752°, +50.368°]`, up `+51.626°`, down `−51.626°` | code + live |
| runtime recommendation | 3036×3036 per eye; maxima 8192×8192 | live (`openxr_host.cpp:712` log) |
| headset's own per-eye envelope | 59.0 / 50.7 / 49.8 / 59.9° (110° × 110°) | live (`vr_headset_fov.json`) |
| `xrEndFrame` failures | none logged | live |

The presenter target is `2 × eye_width × eye_height` with each axis rounded up to a multiple of
16 (`vk_presenter.cpp:1260-1264`), so the only difference between the guest image and the eye
rectangle is 3240 → 3248 rows: a 0.25 % vertical stretch of the sampling grid, not a geometric
change, because the rectangle is still shown with the guest's frustum.

### 1.5 What pixel aspect ratio does and does not do

It does not carry geometry, and the code is right not to use it. In the live frame the vertical
angular density is higher than the horizontal one (28.77 px/° across, 31.39 px/° up); that is
normal for an asymmetric frustum and says nothing about correctness. Correspondingly:

- 2880×3240 → 2880×3248 (today): 28.77 px/° × 31.46 px/° — exact geometry, no error;
- 2880×3240 → 3036×3036 (the runtime's recommendation): 30.32 px/° × 29.41 px/° — exact geometry,
  +5.4 % horizontal sampling, −6.3 % vertical against the source's 3240 rows (−6.5 % against
  today's 3248), **0.000000°** of angular error;
- 8:9 content pillarboxed inside the square: **3.372287°** maximum, **2.392688°** mean error — the
  bars *are* the error; it is zero on the centre column and 3.35–3.37° at the edges, so a still
  frame with its subject in the middle looks fine.

The vertical FOV is symmetric today (`tan_top == tan_bottom`), which is why a top/bottom mix-up
would be invisible; §5.3 turns that into a testable item.

---

## 2. When a full-rectangle resample is exact, and when it is not

**Theorem.** A full-rectangle resample from source rect `R_s` to destination rect `R_d` is
geometrically exact **iff** the frustum covered by `R_s` equals the frustum claimed for `R_d`,
component by component. Both sides interpolate tangents linearly with their own UV, and tangents
are separable per axis, so equality of the four bounds forces the composed pixel → ray map to be
the identity at every sample. Pixel counts, aspect ratios and resolutions are absent from the
condition (`vr_projection_math.h`, `ResampleIsExact`; tests 1–2, 9, 15).

Consequences worth stating, because each is a way to get this wrong:

1. **Rect size and aspect never matter.** Any rectangle can carry any frustum. Choosing 3036×3036
   for an 8:9 source is safe *as long as the declared FOV stays the source's*. Letterboxing is
   never required and always harmful here.
2. **FOV is the only thing that matters**, and it is declared per eye by the application. An
   OpenXR app is allowed to hand in a frustum narrower than `xrEnumerateViewConfigurationViews`
   recommends; the runtime warps it correctly and shows nothing outside it. That is the current
   behaviour, and it is conformant.
3. **When the two frusta differ**, an exact mapping still exists as long as they overlap: take the
   intersection of the tangent intervals and use it on both sides. Because the mapping is
   separable, each axis is a one-dimensional interval intersection. `PlanCrop` implements it and
   reports the cost:

   | situation | action | cost (test 5) |
   |---|---|---|
   | destination frustum wider than source | pad: inset the destination rect (bars outside the content) | content keeps its scale, bars are unavoidable |
   | destination frustum narrower than source | crop: shrink the source rect to the shared interval | 231.697 px per side for a 10° narrower destination, 83.91 % of area kept, residual 1.9e‑14° |
   | frusta disjoint | impossible without re-rendering | `possible == false` |

   What must *not* happen is a full-rect resample with mismatched declarations: that is a smooth
   angular distortion, zero on the axis where the frusta agree and largest at the edges —
   5.019148° maximum, 3.340783° mean for a destination 10° narrower, and 5.4464° for declaring
   the headset's 110°×110° while the source is the title's 100.12°×103.25°.
4. **Crop that is not accompanied by a FOV fix is a stereo bug, not a resolution change.** The same
   world direction must land on the pixel that its own eye's frustum implies. A 5 % one-sided crop
   per eye moves straight-ahead from `u = 0.5` to `u = 0.473684` (left) and `u = 0.526316` (right)
   — **159.789 px of spurious crossed disparity** at 3036 px width for a point at infinity, on top
   of the real IPD disparity (81.520 px at 1 m). Mirrored UVs are equally unsafe: with a 10° per-eye
   envelope offset the same direction sits at `u = 0.500000` and `u = 0.326352`, and assuming
   `1 − u` is wrong by **24.0596°**.
5. **`u = 0.5` is not the optical axis** for an asymmetric frustum: the angular bisector sits at
   `u = 0.456256` (125.98 px off at 2880 wide) and the forward direction (`tan = 0`) at
   `u = 0.413176` (250.05 px off). Any code that centres, mirrors or symmetrises about 0.5 inherits
   that offset. The live frustum is asymmetric by 0.62°, which puts its bisector 9.0 px from the
   centre of a 2880-wide image — small, but the mechanism matters once headset-derived FOVs make it
   large (the headset's own is asymmetric by 8.3° horizontally and 10.1° vertically).
6. **Isotropy is an assumption, not a property.** An equal-diameter angular circle images as an
   ellipse whose axis ratio is `(r−l)/(t−b)` — 1.420277:1 under a 100°×80° frustum. A “square
   looks square” check cannot validate this mapping; only angles can.
7. **Interpolating angles instead of tangents is not an alternative.** It bends straight world
   lines (2.395e‑04 UV collinearity deviation vs −1.0e‑17) and peaks at 7.25° error under an
   asymmetric frustum. The harness keeps `RayFromSourceUvSpherical` precisely to measure how wrong
   it is.

---

## 3. Confirmed issues

Ranked by consequence. “Confirmed” means established from code plus either the live log or a
passing test; no item here rests on appearance in the headset.

### C1. The FOV is recovered from the UVs by guessing where the eye image ends — fixed in this branch

`hmd_reprojection.cpp:53-55` decided whether the title's UVs describe the whole texture or one half
of a side-by-side pair by thresholding their magnitudes (`offset_x < 0.5 && scale_x < 0.35`). For a
side-by-side pair `scale_x = 0.5 / (tan_out + tan_in)`, so the guess flips at
`tan_out + tan_in = 1.4286`, i.e. **71° of horizontal FOV**: above it the nose side is read at the
middle of the picture, below it at the end. Below the flip the nose-side tangent jumps by
`(1 − offset_x)/(0.5 − offset_x)` — a factor of 3 for a centred pair — so at 70° total it recovers
`2.101` where the title drew for `0.700`, a **+29.5°** nose-side error, and the whole image is
stretched with it. Test
`GuestUv_MagnitudeHeuristicBreaksOnlyBelowSeventyOneDegrees` reproduces this at 71/60/45°.

The live session sits comfortably above the cliff (100.12°), which is why Déraciné is correct
today; nothing in the code guarantees it stays there. `SHADPS4_VR_FOV` reaches 50 %, and it is
exactly the mode in which the FOV is no longer the PSVR default. Measured on a build of this branch
with `fov=55` (§5.2): the title returned `scale_x = 0.380568`, above 0.35, so the old rule would have
declared 63.0° of nose-side field of view where the title drew for 33.0° — a 30° error and a
threefold nasal tangent, i.e. a picture visibly stretched across its nasal half. The fixed build
declares 33.0°. The fix (§6) replaces the guess with the layout fact and does not change Déraciné's
numbers at 100 %.

### C2. The runtime's recommended per-eye size is fetched, logged, and discarded

`openxr_host.cpp:698-715` calls `xrEnumerateViewConfigurationViews`, logs
`recommendedImageRectWidth/Height` and `maxImageRectWidth/Height`, and stores nothing. The string
`recommendedImageRect` appears nowhere else in the tree, and `3036` appears in no source file. The
swapchain size is instead derived from the guest's eye size and one scale factor
(`vk_presenter.cpp:1249-1264`), so the app *cannot* express the runtime's recommendation: asking
for `resolution=3036` produces 3040×3424 an eye (16-px alignment, height from the guest's aspect),
which is 12.9 % more pixels than the recommendation and still 8:9. This is a dimension-policy
defect, not a geometry defect — but it is the thing §8 replaces.

### C3. Only the left eye's UVs become geometry

`hmd_reprojection.cpp:232` inverts `param->uv[0]`; `uv[1]` is logged (`:262-267`) and dropped. The
right eye is the mirror by assumption. Déraciné's right-eye UV is the exact mirror
(`offset_x 0.747270 = 1 − 0.252730`, same scale — live), so there is no live error, but any title
with a genuinely per-eye rect or per-eye FOV would be described to the compositor with the left
eye's numbers on both sides. Fixing it properly means a per-eye `Fov` end to end
(`HmdFrame`, the protocol, `FillProjection`); §8 keeps the door open without doing it now.

### C4. Overlay UVs are carried and never used

`overlay_uv[2]` reaches `HmdFrame` (`hmd_reprojection.cpp:226-229`, `video_out.h:164`) and is never
read: `overlay_regions_for` (`vk_presenter.cpp:1416-1438`) hardcodes the two output halves. Harmless
today because the overlays are drawn to full halves, wrong for any title that composites its HUD
with its own rect.

### C5. Non-geometric but worth having on the list

- **Two axes coupled by one scale factor.** Any width request rescales the height by the source
  aspect and then rounds each axis up to 16 independently (`vk_presenter.cpp:1256-1261`), which is
  the only way the axes ever drift (≤ 15 px, ≤ 0.5 %). Geometrically harmless (§1.5), but it means
  the eye rectangle's aspect is a property of the guest image rather than of the headset.
- **The declared frustum is always the PSVR/guest one.** With the shipped settings the image covers
  100.12° × 103.25° of the headset's 110° × 110°, so ~9 % of the horizontal and ~6 % of the vertical
  field is black. That is *not* unnecessary letterboxing: the guest rendered no more. Rendering more
  is what `fov_of=headset` is for, and it works by telling the title a wider frustum before it
  renders.

---

## 4. Suspected issues, each with the experiment that settles it

### S1. Does the title's UV follow the FOV it was told? — resolved, it does

The round trip was exact on the PSVR default, where "UE4 derives its UV from the frustum it queried"
and "UE4 hardcodes the PSVR UV" predict the same numbers. Measured at 70 % and 55 % of the PSVR
field of view (§5.2): the returned `scale_x` moves to 0.299017 and 0.380568, which is
`0.5/(tan_out+tan_in)` for the frustum the title was told, to 0.01 %. The UV follows the query, so
the round trip is self-correcting and changing the reported field of view cannot open a mismatch on
this path.

### S2. Does the vertical axis point the way the reconstruction assumes? — resolved, it does

`FovFromUv` makes `v = 0` the top edge and `tan_top` the up half-angle. The PSVR numbers cannot show
it (`tan_top == tan_bottom`), so the test was the headset's own asymmetric frustum: told
49.8° up / 59.9° down, the title returned UVs that put 49.8° at `v = 0`, and the reconstruction gives
`tan_top = 1.1847` / `tan_bottom = 1.7219` back — up = +49.8°, down = −59.9° (§5.2). A flipped axis
on either side would have returned the pair swapped. There is no host-side convention error here, and
no guest-side limitation either: the title follows the vertical asymmetry it is told.

### S3. What does the 2-D VR branch actually declare?

`hmd_reprojection.cpp:340-352` hardcodes a 16:9 cinema frustum (`tan_h = 0.70`, `tan_v = 0.70·9/16`)
for UVs with `scale_x ∈ (0.4, 0.6)`, on the theory that each eye receives one half of a side-by-side
16:9 picture. The per-eye buffer is 8:9, so declaring 16:9 implies angular pixels stretched ≈1.8:1
between the axes — unless the guest's 2-D content is itself anamorphic inside its half. Which branch
runs for Déraciné's 2-D frames is also unverified: `DEVELOPMENT.md:422` records `scale_x ≈ 0.225`,
which would *not* take the 16:9 branch at all. **Test:** log `param->uv` once in
`sceHmdReprojectionStart2dVr` (it is not logged today) and compare the recovered frustum with the
hardcoded one; then check a circular menu element.

### S4. Is the IPD the guest used the IPD the layer declares?

The guest is told `±config.ipd/2` (`hmd.cpp:134-141`, tracker eye poses `vr_tracker.cpp:457-469`)
and the layer declares `±ipd/2` from the runtime's measured baseline (`openxr_host.cpp:2222-2236`,
`:2725-2730`), which also writes back `config.ipd` (`vr_runtime.cpp:758-762`). The two agree only if
the title re-reads the eye offset after the measurement arrives; nothing waits for it the way
`TitleFov` waits up to 10 s for the FOV. A 2 mm residual is ~0.06° of disparity error at 2 m —
small, systematic, and cheap to remove by having the guest-facing getter read the measured value.

### S5. `fov_of=headset` gives the title the envelope, not each eye's frustum

`ParallelStereoFov` (`openxr_view.h:55-73`) takes `max` of the mirrored pairs, so the title renders
a superset of both eyes' frusta (118° × 119° for the live headset, from 59.0/50.7/49.8/59.9) and the
compositor clips it. Correct, but over-renders by roughly a fifth of the pixels; and `fov_scale` is a
multiplier on tangents, so a “90 %” setting is not a uniform 10 % angular shrink (it is 8.6° of
horizontal at the live frustum). No error in the composited geometry — the declaration follows the
render — but the setting does not mean what its help text implies.

### S6. Out-of-tree VR host

`vk_vr_exporter` forwards the raw tangents, eye size and pose over the socket (`vr_protocol.h:48-67`,
`vk_vr_exporter.cpp:441-458`). Whether that application letterboxes 8:9 into its own swapchain is not
in this repository and cannot be checked here; the OpenXR path in-process does not.

---

## 5. Validation

### 5.1 Automated: `shadps4_vr_projection_math_test`, 25 cases

Pure math, no emulator, Vulkan or OpenXR headers (`src/tests/vr_projection_math.h`,
`src/tests/test_vr_projection_math.cpp`, target added in `src/tests/CMakeLists.txt`). The full
emulator build is clean with it in the tree (§5.3).

The work is on branch `investigation/psvr-projection-geometry`, checked out in
`M:\Development\Deracine-work\wt-projection` so the main worktree stays as it was. That worktree
needs the external sources once before it can configure (`python tools/fetch-submodules.py` from
its root).

```
pwsh M:\Development\Deracine-work\_build\build.ps1 -Tests -Root M:\Development\Deracine-work\wt-projection -Target shadps4_vr_projection_math_test
M:\Development\Deracine-work\wt-projection\build-tests\tests\shadps4_vr_projection_math_test.exe   # 25 passed
ctest -R VrProjectionMath --output-on-failure                                                      # 25/25
```

What each group proves:

| group | cases | what it establishes |
|---|---|---|
| exactness | `Identity_FullRectIsExact`, `EightNineSource_SquareDestination_MatchingFov_IsExact`, `LiveConfiguration_TodayAndAtTheRuntimesRecommendation` | a full-rect resample is exact at 0.000000° for matching frusta, including 8:9 → 1:1 and the live 2880×3240 → 2880×3248 |
| letterboxing | `…_PillarboxIsWrong` | bars inside the rectangle inject 3.372287° max / 2.392688° mean; zero on the shared axis |
| mismatch | `DifferingFov_…`, `DifferingFov_MinimalCropRestoresExactness` | error is smooth, edge-peaked, 5.019148° for 10°; `PlanCrop` removes it to 1.9e‑14° at 83.91 % kept area |
| asymmetry | `AsymmetricFov_CornerTangentsMatchHandComputation`, `…_ImageCentreIsNotTheFrustumAxis`, `SameTotalFov_ShiftedEnvelopeIsADifferentRaySet` | corners match hand-computed tangents to 1e‑12; bisector at u = 0.456256; equal totals with a shifted envelope are 12.129205° apart |
| rects & crops | `RectOffsetDoesNotChangeWhichRayAPixelRepresents`, `NaiveResampleOfCroppedSourceShiftsTheFanHorizontally` | a pixel's ray is invariant under window moves; a 5 % crop per eye is 159.789 px of spurious disparity |
| stereo | `Stereo_PointsAtInfinityHaveZeroDisparity`, `…_FiniteDepthDisparityMatchesClosedForm`, `…_AngularCircleBecomesAnEllipse`, `…_BinocularOverlapUsesEachEyesOwnFov` | 0 px at infinity; −81.520 px at 1 m against closed form; 1.420277:1 ellipse; overlap from tangent intervals, mirrored UVs 24.0596° wrong |
| rays & shapes | `GridOfRaysRoundTripsExactly`, `StraightLinesInAngleSpaceMapToStraightLinesInUv`, `MatchedObjectsAtSeveralDepths` | 17×17 UV round trip < 1e‑13; straight lines stay straight in tangent space and do not in angle space |
| guest UV recovery | `GuestUv_*` (6 cases) | the live UV round-trips to 2e‑6; the magnitude heuristic agrees above 71° and triples the nose side below it; a whole-texture eye must end at U = 1; the layout decision table covers one shared image, a texture each, contradictory offsets and offsets that fit neither reading |

The harness header is reusable by the emulator: `PlanCrop` is the function a future
“declare the runtime's FOV” mode would need, and `Compare` is the numerical oracle any shader-side
change should be checked against.

### 5.2 In the headset, on a build of this branch

Four sessions of the title, on `_validate\shadps4.exe` built from this branch, each log kept as
`run-<label>-log.txt`. The title renders 5760×3240 side-by-side (2880×3240 an eye) and the swapchain
is 5760×3248 in every run.

| run | told to the title (out/in/up/down) | UVs returned (`scale_x/scale_y, offset_x/offset_y`) | recovered tangents | declared to the compositor |
|---|---|---|---|---|
| 100 % (shipped settings) | 50.4/49.8/51.6/51.6° | 0.209312/0.395923, 0.252730/0.500000 | 1.2074/1.1813/1.2629/1.2629 | 50.4/49.8/51.6/51.6° |
| `fov=70` | 40.2/39.6/41.5/41.5° | 0.299017/0.565604, 0.252730/0.500000 | 0.8452/0.8269/0.8840/0.8840 | 40.2/39.6/41.5/41.5° |
| `fov=55` | 33.6/33.0/34.8/34.8° | 0.380568/0.719860, 0.252730/0.500000 | 0.6641/0.6497/0.6946/0.6946 | 33.6/33.0/34.8/34.8° |
| `fov_of=headset` | 59.0/50.7/49.8/59.9° | 0.173247/0.344041, 0.288335/0.407581 | 1.6643/1.2218/1.1847/1.7219 | 59.0/50.7/49.8/59.9° |

What these settle:

1. **The title derives its UVs from the frustum it was told** (§4 S1). `scale_x` is 0.299017 at 70 %
   and 0.380568 at 55 %, against `0.5/(tan_out+tan_in)` = 0.299043 and 0.380568 predicted for the
   told frusta: agreement to 0.01 %. `scale_y` likewise (0.565604 against 0.565606). The round trip
   is self-correcting, and the title is not hardcoding PSVR UVs.
2. **The 71° cliff was real, and it is gone** (§3 C1). At 55 % the returned `scale_x` is 0.380568,
   above the old rule's 0.35, so the old code would have taken U = 1.0 as the nose edge and declared
   `(1 − 0.252730)/0.380568 = 1.964` — 63.0° where the title drew for 33.0°, a 30° error and a
   threefold nasal tangent. The build declares 33.0°.
3. **The vertical convention is right, including the asymmetry** (§4 S2). Told 49.8° up / 59.9°
   down, the title returned UVs whose edges land at `v = 0` and `v = 1` exactly, and the inverse
   gives `tan_top = 1.1847` / `tan_bottom = 1.7219` back: up = +49.8°, down = −59.9°. A flipped axis
   on either side would have returned 59.9/49.8. There is neither a host-side convention error nor a
   guest-side limitation on this path.
4. **Stereo stays aligned across the change.** In every run the right eye's offset is the exact
   mirror of the left's (`0.747270 = 1 − 0.252730` at 70 % and 55 %, `0.711665 = 1 − 0.288335` at
   the headset's frustum), both eyes' pictures sit at `[0, 0.5]` and `[0.5, 1]` of the one image, and
   the compositor is told the same frustum mirrored — so there is no spurious horizontal disparity
   and no per-eye mismatch. The picture at 55 % is the same scene in a narrower window; the only
   visible change is the field of view.
5. **The layout was read unambiguously in every run.** The log says “the eyes' UVs are the halves of
   one picture” once per session, and no ambiguity or fallback warning fired.

### 5.3 Build

The full emulator builds from this branch with the tree's own script and flags:

```
python tools/fetch-submodules.py                                  # 50 externals, from the worktree root
pwsh _build\build.ps1 -Root M:\Development\Deracine-work\wt-projection   # exit 0, shadps4.exe
```

One environment note: `fetch-submodules.py` clones each external at its default branch, not at the
revision this tree builds against. `magic_enum` therefore arrives newer than v0.9.7 and
`magic_enum::customize` rejects `Core::MemoryProt` (“detected flags from operator| and operator&, but
a named value has more than one bit set”); pin it back with
`git -C src/externals/magic_enum fetch --depth 1 origin tag v0.9.7 && git -C src/externals/magic_enum checkout v0.9.7`.
The 25 tests build and pass in the same worktree (§5.1).

---

## 6. The minimal fix in this branch

`src/src/core/libraries/hmd/hmd_reprojection.cpp` only. The integration branch
`fix/hmd-eye-frustum-from-layout` carries this file alone (+102 −10), with no swapchain or test
changes, so it can be taken without the investigation branch.

```cpp
// before (hmd_reprojection.cpp:50-62)
const float right_edge = (left_eye.offset_x < 0.5f && left_eye.scale_x < 0.35f) ? 0.5f : 1.0f;

// after
EyeLayout EyeLayoutOf(left, right, left_eye, right_eye) {   // .halves, .ambiguous
    const float mirrored = std::abs(right_eye.offset_x - (1.0f - left_eye.offset_x));
    const float same = std::abs(right_eye.offset_x - left_eye.offset_x);
    const bool offsets_say_halves = mirrored < same;
    const bool shared = left.Address() == right.Address();
    const bool offsets_fit_neither = same >= 0.02f && mirrored >= 0.02f;
    return {.halves = shared && offsets_say_halves,
            .ambiguous = offsets_fit_neither || (!shared && offsets_say_halves)};
}
```

and `FovFromUv(uv, halves)` takes its far edge from that decision, a `UsableFov` guard catches a
frustum that comes out inside out, and `NoteEyeLayout` reports the reading once per session.

Why this is the right minimal fix:

- **It cannot change Déraciné.** The live frame has both eyes on one image address and
  `offset[1] = 0.747270 = 1 − 0.252730` exactly, so `halves == true` and the recovered frustum is
  bit-identical to today's (`1.207432 / 1.181347 / 1.262872 / 1.262872`).
- **It removes the only way the mapping can be provably wrong today**: the 71° cliff (C1), measured
  in the headset at 55 % (§5.2), with the arithmetic in
  `GuestUv_MagnitudeHeuristicBreaksOnlyBelowSeventyOneDegrees`.
- **It is layout-general and it does not guess.** A texture each is the whole picture whatever the
  offsets look like; one shared image is decided by the offsets; offsets that fit neither reading, or
  that contradict the textures, are marked ambiguous and named in the log with the angles actually
  handed to the compositor, together with the 2 % threshold that decided it. The decision table is
  pinned by `GuestUv_LayoutDecisionTable`.
- **The fallback cannot hide a mismatch.** It is taken only when the inversion produces no usable
  frustum, and it logs at WARNING with the raw UVs and the substituted angles, so a screen that then
  looks stretched is traceable to that line rather than silently “valid”.
- It touches no FPS, memory-pool or guest-engine configuration code.

Verification in this branch: the full emulator builds (§5.3); the 25 tests pass (§5.1); four
in-headset sessions at 100 %, 70 %, 55 % and the headset's own frustum show the recovered tangents
matching what the title was told every time (§5.2).

---

## 7. Where the geometry is *not* at risk (so nobody "fixes" it)

Worth recording because each of these looks like a bug and is not:

- The eye rectangle is **2880×3248** while the source is **2880×3240**. The 8 extra rows are 16-px
  alignment, and they change sampling density by 0.25 %, not angles.
- The source is **8:9** and the runtime recommends **1:1**. Any rectangle can carry the source's
  frustum (§2); no letterbox, no crop, no reprojection is needed to move to a square target.
- The declared FOV is **narrower than the headset's** 110°×110°. That is conformant and correct;
  the alternative — declaring the headset's frustum while the guest drew for its own — is the
  5.4464° error in §2.3.
- The image is **not** resampled by a shader with per-eye UV offsets. `pp_pass` is a plain
  full-0…1 bilinear stretch per half (`fs_tri.vert`, `post_process.frag:123`), which is exactly the
  operation §2 shows to be exact when the FOV is preserved. A per-eye `offset/scale` uniform would
  be needed only to *crop*, which is what §2.3 reserves for a mismatched-FOV mode.
- **`u = 0.5` is not used as an optical axis anywhere in the OpenXR path.** The sub-image split at
  `swapchain_width / 2` is a packing split, not a projection axis, and the frustum is declared
  independently per eye.

---

## 8. Design: from fixed dimensions to OpenXR-provided ones

### 8.1 Keep four numbers apart

| concept | owner | today | should come from |
|---|---|---|---|
| guest render resolution | the title (`r.ScreenPercentage`), guided by `render_percent` | 2880×3240 | unchanged; it is the source |
| host buffer (presenter target) | presenter | 5760×3248 | §8.2 policy |
| XR swapchain size | `EnsureSwapchain` | same as the host buffer | same policy, per view, clamped by `maxImageRect*` |
| **projection frustum** | the guest's `Fov`, declared per eye | PSVR 100.12° × 103.25° | **never the recommendation** — it must equal what was rendered |

The invariant to preserve: *the swapchain size is a sampling decision and must never enter the FOV
path.* `FillProjection` already satisfies it (it reads `shown.fov` and the eye rect only,
`openxr_host.cpp:2705-2744`); the design keeps it that way.

### 8.2 Obtaining and retaining the recommendation

- Store the full `XrViewConfigurationView` array per view at session creation: not just
  `recommendedImageRectWidth/Height` but `maxImageRectWidth/Height`, `recommendedSwapchainSampleCount`
  and `maxSwapchainSampleCount`. Keep **width and height as separate values** and never derive one
  from the other or from the guest aspect.
- Re-read when the session is recreated (runtime restart, headset change, refresh change) — the same
  place that already recomputes `max_swapchain_width/height` (`openxr_host.cpp:641-650`).
- Per-eye sizes are allowed to differ (`PRIMARY_STEREO` only promises equal sizes for identical
  views, and runtimes may report asymmetric rects); index by view from the start.

### 8.3 Size policy

```
per_view(v):  w = recommended[v].width  * supersample
              h = recommended[v].height * supersample
              w, h rounded up to the alignment the runtime/encoder prefers (16)
              clamped to min(maxImageRect[v], maxSwapchainImage)
```

with one addition so the guest's detail is not thrown away vertically: since the source is 8:9 and
the recommendation is 1:1, a square target *loses* 6.3 % of the vertical samples. A useful default
is `h = max(recommended.height, round(guest_eye_height * recommended.width / guest_eye_width))` —
for the live numbers, 3036 × 3240 instead of 3036 × 3036, which keeps the vertical density and costs
2.5 % more pixels than today (19.2 vs 18.7 Mpx/frame). Either is geometrically exact; this is purely
about where the pixels are spent. Percentage-based supersampling (`supersample ∈ {1.0, 1.25, 1.5, 2.0}`)
then multiplies both axes, so the aspect of the recommendation is preserved.

Current costs for comparison, per frame both eyes:

| target per eye | pixels/frame | px/° across × up |
|---|---|---|
| 2880×3248 (today) | 18.71 M | 28.77 × 31.46 |
| 3036×3036 (recommendation) | 18.43 M | 30.32 × 29.41 |
| 3036×3240 (recommendation width, guest height) | 19.67 M | 30.32 × 31.38 |
| 3795×3795 (1.25×) | 28.80 M | 37.90 × 36.76 |

### 8.4 Per-view rectangles and array layers, before per-view swapchains

One side-by-side swapchain with two half-rects is what the code does and what the tests model, and it
is not obviously wrong for asymmetric views. The options, in the order they should be tried rather
than assuming the last one:

1. **One swapchain, per-view `imageRect` (today).** `XrSwapchainSubImage` carries its own
   `imageRect` per view, so two different rectangles inside one image already express two different
   per-eye sizes — as long as both fit inside the swapchain. What this cannot express is a per-eye
   size *larger* than the shared image, or per-view formats. Cost to change: none.
2. **One swapchain, `arraySize = view count`, one layer per view.** `subImage.imageArrayIndex`
   selects the slice, and each slice may carry its own `imageRect`. This is the shape OpenXR
   runtimes expect for stereo, it removes the side-by-side packing from the copy path (each eye is
   copied into its own layer), and all slices share one size — so per-view recommendations are
   honoured by sizing the common slice to the largest recommendation and giving each view its own
   rectangle. Cost to change: `CopyFrame` writes two layers instead of two halves, `FillProjection`
   sets `imageArrayIndex = view`. This is the recommended target.
3. **One swapchain per view.** Needed only when per-view *pixel sizes* must differ beyond what a
   common slice allows, or when per-view formats are wanted. Cost: more handles, per-view recreation.

All three keep the property that matters: a rectangle and the frustum declared for it are
independent, so the swapchain can be recreated at any size without touching the geometry — a resize
can only change sampling density, never introduce distortion. Whichever is chosen, the projected
frustum stays what the guest drew for; only the pixel budget per axis changes.

### 8.5 Guest-facing knobs

`render_percent` keeps its meaning (the title's own render target). The headset-side setting should
become a policy, not a width: `resolution=auto` (recommendation × 1.0), `resolution=<width>` (legacy,
aspect from the source, as today), plus `supersample=<factor>`. That keeps existing settings working
and gives the OpenXR-native path a name.

### 8.6 What should *not* be built

- **No “fit 8:9 into the swapchain” step.** It is provably wrong (§2) and it is the one change that
  would turn today's exact mapping into a 3.37° distortion.
- **No FOV derived from the swapchain size** — that inverts the whole dependency.
- **No dependence on the recommendation for the FOV.** The recommendation describes the headset's
  field, not the guest's render; the two are only equal for a title that renders the whole field.

---

## 9. Summary

- The mapping from the guest's eye image to the OpenXR view is **geometrically exact today**, and
  the live numbers prove it: the FOV told to the title, the UVs the title returns and the frustum
  declared to the runtime agree to 2e‑6 in tangent, the eye rectangle covers exactly the guest
  image, and the resample in between is a plain full-rect stretch. Maximum angular error
  0.000000°, for the current 8:9-shaped target *and* for the runtime's 1:1 recommendation.
- **Pixel aspect ratio is not geometry.** Moving to 3036×3036 needs no letterbox, no crop and no
  reprojection; it only redistributes sampling density (+5.4 % across, −6.3 % up). Adding bars to
  “preserve” the aspect would introduce 3.372287° of error, and centring them makes it look right.
- One **real defect** is fixed, and was reproduced in the headset: the source frustum was recovered
  from the UVs by a magnitude guess that inverts the nose side below 71° of horizontal field of view.
  At 55 % the old code would have declared 63.0° where the title drew for 33.0° (§5.2); the fix reads
  the layout instead of guessing, is bit-identical for Déraciné at 100 %, and says in the log which
  reading it took — including when the offsets fit neither reading, and when it has to fall back.
- Four in-headset sessions (100 %, 70 %, 55 %, and the headset's own 59.0/50.7/49.8/59.9° frustum)
  show the title's UVs tracking what it was told to 0.01 %, the recovered tangents matching it in all
  four, the vertical asymmetry landing the right way up, and the two eyes' offsets mirrored exactly —
  which is the stereo alignment. Nothing on this path is left to appearance.
- The **runtime's recommendation is fetched and thrown away**; the swapchain size is derived from the
  guest's aspect, so 3036×3036 cannot be expressed. §8 replaces this with per-view recommendations,
  independent width/height and a supersampling factor, and prefers per-view `imageRect` and array
  layers over one swapchain per view — while keeping the projection frustum owned by the guest.
