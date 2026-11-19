# PlayStation VR / Unreal configuration for Déraciné (CUSA11919)

What this document is for: **keeping the Déraciné-specific Unreal and Morpheus settings that were
already found, and separating the ones that were measured from the ones that were only read out of
the game's files.** Every claim below is either (a) a value read out of the game's shipped
configuration, (b) a value measured at runtime on this project, or (c) an open question. Anything in
(c) is called out as such; nothing here is presented as proven that was not.

It is the guest-side companion to `docs/vr-presentation-resolution.md`, which owns what the headset
is shown. The two meet at exactly one sentence: **the guest decides what it draws (`r.ScreenPercentage`),
and the presentation size decides how many pixels that picture is sent at.** A change of one is not a
change of the other.

- Controls and where they live: §1
- The 120 Hz configuration that was tested: §2
- Where the game reads its own settings: §3
- What enables and what limits 120 unique frames: §4
- Render scale, the 300 % clamp and the 400 % patch: §5
- MSAA: the guest's and the emulator's: §6
- The rest of the shipped VR configuration: §7
- Every configuration source, and how far it can be trusted: §8
- Historical versus proven on current `main`: §9
- How the controls are validated: §10

---

## 1. The controls, and what each one is

Four separate things were conflated in one place or another before this document existed. They are
not the same control and must not be presented as one:

| Concept | Setting (`pc-vr/settings.txt`) | What it actually changes | Mechanism |
|---|---|---|---|
| **The game's native frame rate** | `frame_mode=60\|120` | How many unique frames the title renders per second | The title's own Morpheus frame sequence, in its effective saved `engine.ini` |
| **The game's render scale** | `render_percent=100..300` | The size the title draws its scene at, per eye | `r.ScreenPercentage`, same file |
| **The game's own MSAA** | `guest_msaa=1\|2\|4` | The sample count of the title's scene colour and depth targets | `r.MSAACount`, same file |
| **The emulated display** | `refresh=90\|120` | The rate the emulated PlayStation VR display refreshes at, which the title keeps time by | `SHADPS4_VR_REFRESH_RATE` |
| **The emulator's sample cap** | `msaa=1\|2\|4\|8` | The most samples the *host* pictures may be made with | `SHADPS4_MAX_MSAA` |
| **What the headset is shown** | `present=guest\|auto\|WxH`, `resolution=` | The OpenXR presentation size | `SHADPS4_VR_RESOLUTION`, legacy `SHADPS4_VR_SCALE` / `SHADPS4_TITLE_EYE_WIDTH` |

Not among them: the headset's own panel refresh. That is chosen by the headset's own software (SteamVR
Video settings, Virtual Desktop Streaming settings) and the runtime's OpenXR extension; the emulator
only reports it (`SHADPS4_VR_FOLLOW_DISPLAY`, on by default, follows the host's own display rate —
`src/src/core/vr/vr_runtime.cpp:891-914`).

The five guest and emulator keys are applied by `pc-vr/render-config.ps1` (guest, through the game's
own file) and by `pc-vr/launch.ps1` (environment). All of them take effect on the next start of the
game, never while it runs.

**Removed controls.** `fps=` and `pace=` no longer do anything and the launcher says so when it finds
them. `fps=` was written by the old settings window and read by the launcher, which then ignored it and
exported `SHADPS4_VR_FPS_CAP=60` regardless. `SHADPS4_VR_FPS_CAP`, `SHADPS4_VR_PACE`,
`SHADPS4_VR_FASTEST_PACE` and `SHADPS4_VR_STEADY_FPS` are parsed only by
`src/src/core/known_title.cpp` (`GetSettings()`, lines 205-300), whose every consumer runs behind
`known_build != nullptr`, which only the Astro Bot builds set (`IsThisTitle()`, lines 49-52;
`OnGameLoaded()`, lines 1394-1465). **No Déraciné build ever reaches that code, so those variables
cannot cap, pace or smooth this title.** The launcher also no longer rewrites `GPU.vblank_frequency`.

---

## 2. The tested 120 Hz configuration

This is the block that was measured. It is written into the game's own saved configuration
(`<game>\gameproject\saved\config\ps4\engine.ini`) — the file the game itself opens at start
(confirmed in the file-system log: `/app0/gameproject/saved/config/ps4/engine.ini`).

```ini
[/Script/MorpheusEditor.MorpheusRuntimeSettings]
PSVRFrameSequence=Render120Scanout120

[/Script/Engine.Engine]
bSmoothFrameRate=False
SmoothedFrameRateRange=(LowerBound=(Type=Inclusive,Value=22),UpperBound=(Type=Exclusive,Value=250))

[SystemSettings]
t.MaxFPS=0
r.FinishCurrentFrame=0
```

What it is for: the title's Morpheus path asks its headset for a frame sequence, and the shipped
configuration never says which one — `PSVRFrameSequence` appears in **none** of the 46 config files
extracted from `gameproject-ps4.pak`, and `[/Script/MorpheusEditor.MorpheusRuntimeSettings]` appears
exactly once, holding only `bEnableMorpheus=True` (`GameProject/Config/DefaultEngine.ini:57-58`).
Left to itself the title rendered about 60 unique frames a second, taking two vblank waits per frame.
`Render120Scanout120` is the engine's native 120 mode; the same key with `Render90Scanout90` is the
documented 90 mode of another UE4 PlayStation VR title, [StatikQuest's launcher](https://github.com/Jerware/StatikQuest/blob/main/pc-vr/launch.ps1),
which is where the setting was found.

Measured on 2026-11-17 **in a session with the headset** (100 % render scale, OpenXR at 120 Hz, warm —
the cold state has shader compilation in it and is not comparable). The workload matters and is in §9:
this was the 2D calibration/menu screen, not gameplay.

| metric | value |
|---|---|
| unique guest frames | **120.0 fps** (1,932 frames in 16.1 s) |
| median frame | 8.52 ms |
| p90 / p99 | 9.07 / 9.70 ms |
| frames over 100 ms | 0 |
| vblank waits per frame | **1.19** (was 2.03) |

So the engine consumes the key, the cadence really does change from two vblank waits to one, and the
title can produce 120 unique rendered frames a second **with no executable patch**. The source of the
figures is the pre-ledger investigation log, `_handoff/2026-11-17/(dead) Framerate Investigation #1.md`
(written after an earlier draft got it wrong — see §9).

Two more measured points about that block:

- `r.FinishCurrentFrame=0` is an **independent** gain, not part of the frame-sequence change: in the
  cold-state matrix it gave 48.7 against 44.0 frames a second for the shipped `1`
  (`GameProject/Config/DefaultEngine.ini:65`). It is grouped with `frame_mode=120` here because the
  tested winning configuration contains it, and it is not applied in `frame_mode=60`.
- `t.MaxFPS=0` is the engine's own default (the key is absent from all 46 configs), so it is written
  for completeness and changes nothing by itself.

---

## 3. Where the game reads its own settings

The guest build resolves configuration in the usual Unreal order. For this title, in the pak:

| Layer | File | What it holds |
|---|---|---|
| Stock engine defaults | `Engine/Config/BaseEngine.ini` | `bSmoothFrameRate=True`, smoothed range 22..62, `MinDesiredFrameRate=35` |
| PS4 platform engine defaults | `Engine/Config/PS4/PS4Engine.ini` | `r.setres=1920x1080`, `r.VSync=1`, `bSmoothFrameRate=false` |
| Project config | `GameProject/Config/DefaultEngine.ini` | the VR/render file: MSAA, forward shading, stereo, post-process, `r.FinishCurrentFrame=1` |
| Project PS4 overrides | `GameProject/Config/PS4/PS4Engine.ini` | memory pools, PSN subsystems |
| Device profiles | `GameProject/Config/PS4/PS4DeviceProfiles.ini` | the **only** shipped `r.ScreenPercentage`: 130 (PS4), 150 (PS4_Neo) |
| **Saved (writable)** | `<game>\gameproject\saved\config\ps4\engine.ini` | what the game writes at runtime; **the layer the launcher manages** |

The saved layer is the one that is both effective and ours to write. Reading it is not optional: the
game's log shows it opening that exact path on every run, and the measured mapping in §5 is only
explainable if that file's `[SystemSettings]` value is what the Morpheus path ends up using.

`render-config.ps1` therefore:

- makes every change to the file **as it is at that moment**, so anything written into it since the
  last run — by the game, or by hand — is still there afterwards, along with section order, unrelated
  keys and their duplicates, the file's line endings and its last newline;
- sets only the keys in the table in §1, inside their own sections, and collapses a duplicate of a key
  it manages rather than adding another;
- restores **only the frame-mode keys** when `frame_mode` goes back to `60`: their values from before
  this project first wrote them, and nothing else. A section it created for them and no longer needs
  goes with them; a section the game's own file had is never dropped;
- keeps the copy taken before the first change (`engine.ini.deracine-original`) immutable. It is read
  for exactly two things: knowing those earlier frame-mode values, and `render_restore=1`, which puts
  the whole file back and is the one operation that also discards later edits;
- refuses `render_percent` above 300 instead of writing a value the engine will clamp anyway (§5).

Nothing in it touches `eboot.bin`, the pak, or any config inside the pak. The saved configuration is a
file the game writes while it runs; it is backed up before the first change.

---

## 4. What enables and what limits 120 unique frames

### 4.1 What enables it

**The guest's own Morpheus frame sequence.** Nothing in the emulator enables it and nothing in the
emulator was found to be capable of enabling it: `frame_mode=120` changes a setting inside the game,
and the game then submits twice as many frames. The measured change in the emulator's own view of the
same scene is exactly the cadence (§2): 2.03 vblank waits per frame → 1.19.

### 4.2 What does **not** control it

Both of these were believed to control it at some point, and neither does:

- **`SHADPS4_VR_FPS_CAP`** — parsed only by the Astro Bot known-title governor (§1). Setting it to 60,
  or to 120, or not at all, changes nothing about Déraciné. The launcher used to export
  `SHADPS4_VR_FPS_CAP=60` unconditionally while reading an `fps=` setting it then ignored: that is the
  single most misleading thing this document replaces.
- **`GPU.vblank_frequency`** — the emulated display's rate only when **no** headset is connected.
  `VideoOutDriver::PresentThread` takes the VR refresh rate whenever one is
  (`src/src/core/libraries/videoout/driver.cpp:730-731`), which comes from `refresh=` through
  `SHADPS4_VR_REFRESH_RATE` (`src/src/core/vr/vr_runtime.cpp:163-168`). The launcher used to rewrite
  this key to 60 on every start, which never held the game to 60 and only contradicted `refresh=`; the
  emulator's own settings window still owns the value for the desktop path.

### 4.3 What limits it

The frame's cost, at the size it is drawn. Measured on the heavy scene (GPU busy × frame time, at a
60 fps front-end ceiling so the numbers are work-per-frame and not a frame rate):

| render | Mpx a frame | GPU work | fps while capped |
|---|---|---|---|
| 320 % | 529 | 13.8 ms | 56.7 |
| 210 % | 231 | 6.5 ms | 59.3 |
| 180 % | 179 | 5.2 ms | 59.6 |

That is **0.028 ms per megapixel with no measurable intercept**. A 120 fps frame has 8.33 ms, of
which roughly 1.8 ms is host time, so the scene has to be drawn at about **210 % (2016×2268 an eye)**
for 120 fps to fit on the reference card, with the presentation left at the runtime's own size
(`present=auto`) so that what is given up is sharpness, not size.

### 4.4 The evidence on the front end, and what it does not settle

The front end held **58.7 fps at vblank 120** and **58.6 fps with `SHADPS4_VR_PACE=1`**, with the GPU
only 37.5–38 % busy at 210 %. The conclusion drawn at the time was that "the 60 fps ceiling is the
title's own front end, not the emulated vblank and not the governor's pace".

**That conclusion is weaker than it reads, for three reasons.** First, §4.2: `SHADPS4_VR_PACE` is
inert for this title, so those two arms were the same experiment twice. Second, every front-end
measurement then was taken **before** `PSVRFrameSequence=Render120Scanout120` was known, i.e. with the
title asking for its default sequence. Third, when the configuration *was* re-baselined on current
`main` (2026-11-19, `docs/performance/target-ledger.md` §4.6), the key **did not raise front-end
pacing**:

| | menu-band maximum, no headset worn |
|---|---|
| private shadow carrying the 120 configuration | 55.3 / 57.3 fps |
| the shadow every earlier benchmark cell used (key absent) | 60.6 / 61.7 fps |

The same cells measured the gameplay band at **32.80 and 34.45 unique fps** (≈876 Mpx, GPU 48.9 / 51.0 %
busy, run-to-run spread 4.8 %), and the ledger records them as their own configuration — **not** a
matched A/B, and not to be pooled with the earlier arms. Note also that those cells do *not* follow
§4.3's per-megapixel line: at 876 Mpx the budget would predict far more than 33 fps, so that scene is
limited by something else (the ledger's CPU targets), which is exactly why the budget above is scoped
to the GPU-bound heavy scene.

So the honest state is: **the 120 configuration has been used with the headset, and the 120.0 unique fps
in §2 came from such a session — but in a lighter workload.** What was never verified is 120 unique
frames **in gameplay**; there gameplay was the ~52 fps quoted in §9. The **headless** re-baseline cells
above add that, with nobody wearing the headset, this key does not lift pacing and is slightly worse —
consistent with the XR present path stalling when the headset is asleep — so headless arms cannot
substitute for a session.

That leaves the one measurement this configuration exists to make possible: same scene, same build,
same size, `frame_mode=60` against `frame_mode=120`, comparing **unique rendered fps** in gameplay
rather than the compositor's or the headset's cadence.

---

## 5. Render scale: `r.ScreenPercentage`

The title draws its scene at `r.ScreenPercentage` percent of a PlayStation VR's 960×1080 an eye. The
mapping is exact and linear, and was measured at runtime, per eye:

| `r.ScreenPercentage` | render per eye | measured evidence |
|---|---|---|
| 100 | 960×1080 | PSVR native |
| 130 *(the game's own)* | 1248×1404 | scene target SBS 2560×1408, eye image 2496×1404 |
| 200 | 1920×2160 | — |
| 300 | **2880×3240** | scene target SBS 5760×3264, eye image 5760×3240 |
| 400 | 2880×3240 *(clamped at 300)* | 400 % requested, 300 % rendered |

300 % was validated as a standing configuration: 2880×3240 per eye, 100 seconds, 3.3 GB, zero crashes
— a 5.3× pixel increase over the shipped 1248×1404. The precedence question the pak extraction left
open (device-profile cvar against `[SystemSettings]`) is settled *for this layer* by those numbers:
the saved-config value is what the Morpheus path used, so it wins over the profile's 130/150. The
engine's internal ordering was not disassembled and is not claimed.

### 5.1 The 300 % ceiling

It is **one guest instruction**, in the Morpheus HMD size path, which computes the eye render target
size roughly as `ceil(panel * clamp(r.ScreenPercentage, 30, 300) * 0.01)`:

- `vminss 300.0f` at guest `0x013f43ac`, the constant at eboot VA `0x35b030c` (guest
  `0x8035b030c`), adjacent to the `30.0f` and `0.01f` of the same clamp;
- the cvar itself is read-only for this title: all seven references to its name are reads.

So the ceiling is a guest-side clamp on the *multiplier*, not a cvar limit and not a fixed render
buffer. `pc-vr/render-config.ps1` refuses a `render_percent` above 300 rather than writing a value the
guest will clamp: silently accepting 400 and rendering 300 would be the kind of control this document
exists to prevent.

### 5.2 The experimental 400 % patch

The investigation's way past the clamp is an emulator **executable patch**, not a memory write and not
a modification of the game:

- file: `D:\Temp\Deracine\CUSA11919-qual\quality-cap400.xml` (outside the repository — a lab artifact),
  metadata name `Render scale cap 300 percent`, `AppVer="01.01"`, so version-gated and skipped on any
  other build;
- what it does: a `Type="mask"` line matching the 12-byte constant triple
  `00 00 96 43 00 00 f0 41 0a d7 23 3c` (300.0f, 30.0f, 0.01f) and replacing the first four bytes with
  `00 00 c8 43` (400.0f). The triple occurs once in the image;
- verified at runtime: 400 % renders **3840×4320** an eye; 320 % renders **3072×3456** an eye; with the
  patch off, both stop at 2880×3240.

`eboot.bin` is untouched in both cases. 320 % is the smallest setting that genuinely exceeds the
runtime's 3036×3036 per-eye recommendation in **both** axes, and costs only ~13.8 % more scene pixels
than 300 %. It is experimental: it is an executable patch, gated to one title version, and this
launcher does not install it. Setting `render_percent=320` or `400` without the patch is refused with
a message naming the patch.

---

## 6. MSAA: the guest's and the emulator's

Two different things, and for this title the difference is destructive:

- **The guest's MSAA** is `r.MSAACount` under `[SystemSettings]` — the sample count of the title's own
  scene colour and depth targets. The game ships with **2** (`GameProject/Config/DefaultEngine.ini:30`,
  with `r.DefaultFeature.AntiAliasing=3` on line 29 and `r.ForwardShading=True` on line 42, which is
  what makes MSAA applicable at all).
- **The emulator's MSAA cap** is `SHADPS4_MAX_MSAA` (`msaa=` in `settings.txt`), which sets a host
  `sample_count_cap` (`src/src/video_core/renderer_vulkan/vk_instance.cpp:190-193`) that the
  Liverpool→Vulkan sample translation then respects.

Measured with `SHADPS4_QUALITY_DIAG=1`, on the same scene at 130 %:

| run | `r.MSAACount` | scene colour (guest/Vulkan) | scene depth | pipeline | guest resolves | HMD eye image |
|---|---|---|---|---|---|---|
| `r1-130-msaa1` | 1 | `2560x1408 s1/1` | `s1/1` | 1/1 | 0 | 2496×1404 s1 |
| `r1-130-msaa2` | 2 | `2560x1408 s2/2` | `s2/2` | 2/2 | 0 | 2496×1404 s1 |
| `r3-130-msaa4` | 4 | `2560x1408 s4/4` | `s4/4` | 4/4 | 0 | 2496×1404 s1 |
| `r7-130-msaa2-hostcap1` | 2 (`SHADPS4_MAX_MSAA=1`) | `2560x1408 s2/1` | `s2/1` | 1/1 | 0 | 2496×1404 s1 |

`s<guest>/<image>` is the guest descriptor's sample count against the Vulkan image's. So:

1. **The guest's MSAA is real and follows the guest's own setting** — 1, 2 and 4 samples, in the guest
   descriptor and the Vulkan image and the bound pipeline alike, at 130 % and at 300 %.
2. **The title never asks the emulator for a colour resolve** (`resolves=0` in every window of every
   run). It resolves internally — every eye image handed to reprojection is single-sample and
   side-by-side — and its only visible MSAA trace is `FmaskDecompress` draws. `SHADPS4_RESOLVE_AA` is
   therefore **unreachable** for Déraciné; it is not what is smoothing anything.
3. **Capping the host samples below the guest's is a silent quality loss.** With
   `SHADPS4_MAX_MSAA=1` the guest still says 2, the host image is made at 1, and because there is no
   resolve and no substitute smoothing, MSAA is simply gone with nothing in the log to say so. The
   emulator's cap is only safe at or above the guest's own count; `settings.txt` says this next to
   `msaa=`.

`guest_msaa` exists because of exactly this: the setting that changes the real MSAA is the guest's, and
it was unreachable from the launcher. 4× is structurally working (see the table) but experimental: the
4× run died on the pre-existing `address_space.cpp Protect: Unreachable code!` crash that also occurs
without MSAA.

---

## 7. The rest of the shipped VR configuration

Values **read out of the shipped pak** (extracted from `gameproject-ps4.pak`; exact files and line
numbers in §8). These are configuration, not runtime verification: the emulator has no way to observe
`vr.InstancedStereo` or `vr.MultiView`, and does not need to.

| Setting | Value | File:line | Note |
|---|---|---|---|
| `bEnableMorpheus` | `True` | `DefaultEngine.ini:58` | the title's master PSVR switch |
| `vr.InstancedStereo` | `True` | `DefaultEngine.ini:37` | single-pass instanced stereo |
| `vr.MultiView` | `True` | `DefaultEngine.ini:41` | a PS4 GPU feature; under emulation it only shapes the guest's own targets, it is not a PC multi-view path |
| `r.ForwardShading` | `True` | `DefaultEngine.ini:42` | prerequisite for MSAA in VR; consistent with the measured MSAA |
| `r.DefaultFeature.AntiAliasing` | `3` (MSAA) | `DefaultEngine.ini:29` | |
| `r.MSAACount` | `2` | `DefaultEngine.ini:30` | **verified at runtime** — see §6 |
| `r.BasePassOutputsVelocity` | `False` | `DefaultEngine.ini:35` | no velocity buffer, so no TAA ghosting |
| `GlobalDefaultGameMode` | `.../VR_Game.VR_Game_C` | `DefaultEngine.ini:54` | the VR game mode is the default |
| `r.ScreenPercentage` | `130` / `150` | `PS4DeviceProfiles.ini:5,10` | superseded by the saved layer (§5) |
| `NearClipPlane` | `4.0` | `DefaultEngine.ini:102` | 4 cm near plane |
| `r.FinishCurrentFrame` | `1` | `DefaultEngine.ini:65` | the tested 120 mode sets it to 0 (§2) |
| `r.HZBOcclusion` | `0` | `DefaultEngine.ini:64` | |
| `r.EarlyZPass` / `r.EarlyZPassMovable` / `r.EarlyZPassOnlyMaterialMasking` | `2` / `True` / `True` | `DefaultEngine.ini:31,32,43` | |
| `r.CustomDepth` | `3` | `DefaultEngine.ini:22` | |
| post-process | bloom, AO, motion blur, lens flare, auto-exposure all `False`; `sg.PostProcessQuality=0` | `DefaultEngine.ini:23-28,85` | already pared back for VR |

**Historical guest value, not a current one:** the `r.MSAACount=2` in the table above is both the
shipped value *and* the value measured at runtime, which is why it is trustworthy. Treat the rest of
the table as "what the game was built with" until a run measures it.

---

## 8. Every configuration source, and how far it can be trusted

| Source | Path | What it holds | Trust |
|---|---|---|---|
| Shipped pak configs, extracted | `_build/pakout/cfg/` (parent worktree, **not in the repository** — 46 `.ini` files, byte-exact, extracted with CUE4Parse; report: `_build/PAK-CONFIG-VR-REPORT.md`) | everything in §7 | **read-out values.** Not runtime-verified except where §6 says so |
| The game's own saved config | `<game>\gameproject\saved\config\ps4\engine.ini` | the effective writable layer | **the layer the launcher manages**; the game's log shows it opening this path every run |
| The 120 Hz artifact | `D:\Temp\Deracine\1.0\Image0\...\gameproject\saved\config\ps4\engine.ini` | the §2 block plus `r.ScreenPercentage=300` | **not part of the game.** Corrected on 2026-11-19 (see §9): it is an artifact the earlier frametime investigation added. It is the provenance of §2, not a shipped default |
| The private shadow that reproduces it | `_out/gpu01-depthpass-2026-11-19/shadow-morpheus120/gameproject/saved/config/ps4/engine.ini` (parent worktree, not in the repository) | the §2 block verbatim, plus `r.MSAACount=2`; `engine.ini` hash `26C43971…` | the two 2026-11-19 cells in ledger §4.6 were run on it; §4.3's figures were not |
| The 60-mode shadows | `_build/frame-time-profile-full-2026-11-18/...`, `D:\Temp\Deracine\CUSA11919-qual\...` | `[SystemSettings] r.ScreenPercentage=300`, `r.MSAACount=2` only | the configuration every M2 benchmark cell ran with; **no Morpheus key** (ledger §4.5) |
| The clamp patch | `D:\Temp\Deracine\CUSA11919-qual\quality-cap400.xml` (outside the repository) | the §5.2 pattern patch | verified at runtime (320 %, 400 %) |
| The measurements | `_handoff/2026-11-17/(dead) Framerate Investigation #1.md` (parent worktree) | §2, §5, §6, §7 numbers | a dated investigation log; its own corrections are part of it (§9) |
| The front-end ceiling and the fps budget | `_build/PASSES-AND-PACING.md` §3, §11, §18, §19, §20 (parent worktree) | §4.3, §4.4 | measured, but **before** the Morpheus key was known |
| MSAA / clamp / downsampling detail | `_build/quality/RENDER-QUALITY-REPORT.md`, `.../msaa/MSAA-ANALYSIS.md`, `.../clamp/CLAMP-ANALYSIS.md` (parent worktree) | §5, §6 | measured with `SHADPS4_QUALITY_DIAG=1` |
| Presentation size | `docs/vr-presentation-resolution.md` (**in the repository**) | §1's last row, and §6 there for the sampling interface | measured; the authoritative source for the presentation side |
| The ledger's record of the tweak and its re-baseline | `docs/performance/target-ledger.md` §4.5–4.6 (**in the repository**) | the tweak's provenance as an agent-added artifact, and two 2026-11-19 cells on it | the ledger owns the *benchmark* consequences; this document owns the configuration. §4.4 here and §4.6 there are the same evidence |
| The emulator's behaviour | `src/src/core/known_title.cpp`, `src/src/core/libraries/videoout/driver.cpp`, `src/src/video_core/renderer_vulkan/vk_instance.cpp`, `src/src/video_core/renderer_vulkan/vk_presenter.cpp`, `src/src/core/vr/vr_runtime.cpp` (**in the repository**) | §1, §4.2, §5.1, §6 | source-verified on current `main` |

Note on what is *not* here: no extracted game config, no dump, no screenshot and no log from the game
is committed to this repository. The paths above marked "not in the repository" are the lab artifacts
they live in; they are cited, not copied.

---

## 9. Historical versus proven on current `main`

**Proven on current `main`** (source-verified, and covered by the tests in §10):

- `SHADPS4_VR_FPS_CAP`, `SHADPS4_VR_PACE`, `SHADPS4_VR_FASTEST_PACE` and `SHADPS4_VR_STEADY_FPS` are
  reachable only through the Astro Bot known-title path and cannot affect Déraciné (§1, §4.2).
- `PresentThread` uses the VR refresh rate whenever a headset is connected, and
  `GPU.vblank_frequency` otherwise (§4.2).
- The presentation-size chain and its precedence (`docs/vr-presentation-resolution.md` §2.1).
- `SHADPS4_MAX_MSAA` sets a host sample cap that is applied after the guest's own request (§6).
- All of `render-config.ps1`'s file behaviour: which keys it writes, into which sections; that an
  unrelated key written into the file afterwards survives a later mode or scale change; that the kept
  original is never written to and is read only for the frame-mode keys and `render_restore=1`; that
  switching modes is idempotent and leaves a file the mode created sections in byte for byte as it was;
  that line endings and the last newline are untouched; and that out-of-range or unknown values are
  refused (§10).

**Measured on 2026-11-17 with the headset, on the then-current tree and binary, not since reproduced on
`main`:**

- 120.0 unique frames a second with `Render120Scanout120` + the §2 block, at 100 % render scale
  (§2). **The workload is what limits this claim, not the session:** the 16.1-second window that
  produced it was the **2D calibration/menu screen, not the 3D scene**. The investigation says so
  itself ("The window I measured '120.0 fps' in is the 2D calibration/menu screen, not the 3D scene"),
  and in the same breath records gameplay at 960×1080 running at ~19.2 ms (~52 fps) at that time. A
  later round, on the same stationary classroom viewpoint and a different build, measured **119.5 fps
  mean (p50 8.34 ms) at 100 %** and **32.3 fps at 300 %**. So "120 unique frames at 100 % in a lighter
  workload" is a real headset-session measurement; **120 fps in gameplay has never been verified.**
- The `r.ScreenPercentage` mapping, and 300 % as a standing configuration (§5).
- The 300 % clamp instruction and the 400 % patch's effect (§5.1, §5.2).
- The MSAA sample counts and the host-cap loss (§6).
- The front-end ~60 fps ceiling, and the 0.028 ms/Mpx frame budget (§4.3, §4.4) — the budget measured
  **without** the Morpheus key, and the ceiling's attribution weakened by §4.4.
- The 4× MSAA crash on `address_space.cpp Protect: Unreachable code!` (§6) — pre-existing, not caused
  by MSAA, and not re-confirmed on `main`.

**Measured on current `main`, 2026-11-19** (`docs/performance/target-ledger.md` §4.6 — not by this
document, and not a 120 fps result): two cells on a private shadow carrying the §2 block, gameplay
band, 32.80 and 34.45 unique fps at ≈876 Mpx with the GPU under half busy, run-to-run spread 4.8 %.
This is the first current-`main` measurement of the configuration. It does **not** show the
configuration producing 120 unique frames — the scene is not GPU-bound at that size — and it records
that the front-end pacing did **not** improve with the key present in a headless session (§4.4).

**Recorded corrections** (kept because they are the kind of mistake this document exists to prevent):

- The 120 Hz block was once described as the game's shipped configuration. It is not: it is an artifact
  the earlier frametime investigation wrote into one lab copy. Corrected 2026-11-19 and recorded in
  `docs/performance/target-ledger.md`.
- "120 unique frames are not unlockable without title-specific modification" was concluded and then
  withdrawn in the same investigation, once the Morpheus route worked.
- The 400 % "success" in one early screenshot was crop/resample of a 2880×3240 image, not a larger
  render; the real 400 % needed the clamp patch.

---

## 10. How the controls are validated

Two suites, both runnable without a game and without an emulator, both green:

```powershell
powershell -ExecutionPolicy Bypass -File tools/tests/launcher-test.ps1
powershell -ExecutionPolicy Bypass -File tools/tests/morpheus-config-test.ps1
```

`tools/tests/morpheus-config-test.ps1` is the one that covers this document. It makes up game folders
under `build/morpheus-config-test`, and checks, among others:

- `frame_mode=120` writes `PSVRFrameSequence=Render120Scanout120` into
  `[/Script/MorpheusEditor.MorpheusRuntimeSettings]`, and the companion keys into
  `[/Script/Engine.Engine]` and `[SystemSettings]`, and nothing else;
- a `r.ScreenPercentage` in another section (the device profile) is left alone, an unrelated key is
  left alone, and no section is added or reordered;
- asking for the same thing twice writes nothing the second time;
- `frame_mode=60` returns the frame-mode keys **and only those** to the game's own, and a file the mode
  created sections in comes back byte for byte — with and without a trailing newline;
- a key the game or the player writes into the file **after** a mode has been applied survives the next
  frame-mode or render-scale change, while the frame-mode keys still go back to their earlier values;
- the kept original is byte-identical after every one of those operations, and is only discarded by
  `render_restore=1`;
- an unrelated key written twice is left twice;
- `render_restore=1` puts the kept copy back byte-for-byte and forgets it;
- a file with bare newlines keeps bare newlines and a file with Windows newlines keeps those;
- 400 % is refused, with a message naming the clamp patch, and nothing is written;
- an unknown frame mode, an MSAA count the game does not have, a non-numeric render scale and an
  unknown game folder are each refused;
- a game whose own file already asks for a frame sequence is not silently changed by `frame_mode=60`,
  and says so;
- the launcher sets no `SHADPS4_VR_FPS_CAP`, no pace and no `vblank_frequency` anywhere, and does set
  `SHADPS4_VR_REFRESH_RATE`;
- the launcher's own `refresh=` values map as documented, and running them leaves a pre-set
  `SHADPS4_VR_FPS_CAP` untouched;
- the launcher's guest-configuration block, run against a made-up game, applies `frame_mode` and
  `render_percent`, reads the effective values back out of the file, and restores on `render_restore=1`.

Both suites are run on Windows PowerShell 5.1 and on PowerShell 7; the launcher's guest-configuration
call uses a **hash** splat, because an array splat is expanded positionally and would pass
`-Percent` as an argument rather than as a parameter name.

What the suites cannot check is the part that needs the game: that the guest actually consumes
`PSVRFrameSequence` and renders more frames. Two headless cells on the 120 configuration already exist
(`docs/performance/target-ledger.md` §4.6) and they were run with nobody wearing the headset, which the
ledger itself records as the reason they cannot judge this key (§4.4). The procedure for the
measurement that would settle it is:

1. Copy the game to a **private shadow** (never the original). Set the shadow's
   `gameproject\saved\config\ps4\engine.ini` to the state under test, or point `render-config.ps1` at
   the shadow with `-Eboot`.
2. Fix everything else between the arms: same build, same scene, same `render_percent`, same `refresh`,
   same presentation size.
3. Run one arm with `frame_mode=60` and one with `frame_mode=120` and compare **unique rendered
   frames** — the emulator's own frame-stat series and draw/Mpx counts — never the compositor's or the
   headset's cadence. `tools/benchmark/headless-gameplay.ps1` reaches the gameplay band (≈1,868 draws,
   ≈876 Mpx) without a headset, which is where the two configurations can be compared directly; read
   §4.4 before treating an unattended arm as a statement about a worn session.
   `docs/integration/HEADSET-ACCEPTANCE.md` §7 defines what to record.
4. Read the effective guest values back with `pwsh -File pc-vr/render-config.ps1 -Eboot <shadow> -List`,
   and confirm them against the render-target dimensions and the Vulkan sample counts the emulator
   reports (`SHADPS4_QUALITY_DIAG=1`), not against the settings file alone.

This is deliberately separate from the renderer and benchmark work: no renderer optimisation, image
alias change or performance-ledger experiment belongs in the same run as a configuration comparison.

---

## 11. References

In the repository:

- `docs/vr-presentation-resolution.md` — the presentation side, the sizing policy and its measured
  acceptance (`SHADPS4_VR_RESOLUTION`, legacy `SHADPS4_VR_SCALE` / `SHADPS4_TITLE_EYE_WIDTH`).
- `docs/performance/target-ledger.md` §4.5–4.6 and the 2026-11-19 entries — the tweak's provenance,
  its re-baseline cells, and the record that the front-end pacing did not improve headless.
- `docs/integration/HEADSET-ACCEPTANCE.md` §7 — what a headset session must record.
- `pc-vr/render-config.ps1`, `pc-vr/launch.ps1`, `pc-vr/settings.txt` — the controls themselves.
- `src/src/core/known_title.cpp` — the Astro Bot known-title governor those frame-cap variables belong
  to; `src/src/core/libraries/videoout/driver.cpp` — vblank and the headset refresh clock;
  `src/src/video_core/renderer_vulkan/vk_instance.cpp` — the host sample cap;
  `src/src/core/vr/vr_runtime.cpp` — the emulated refresh rate and display following;
  `src/src/video_core/renderer_vulkan/vk_presenter.cpp` — the presentation-size chain.

Lab artifacts outside the repository (cited, not committed):

- `_build/pakout/cfg/` + `_build/PAK-CONFIG-VR-REPORT.md` — the 46 configs extracted from the pak.
- `_build/quality/RENDER-QUALITY-REPORT.md` (+ `msaa/`, `clamp/`, `filters/`) — MSAA, the clamp and
  downsampling.
- `_build/PASSES-AND-PACING.md` — the frame budget and the front-end ceiling.
- `_handoff/2026-11-17/(dead) Framerate Investigation #1.md` — the measurements in §2, §5 and §6.
- `_out/gpu01-depthpass-2026-11-19/shadow-morpheus120/` — the private shadow reproducing the §2 block.

Outside this project:

- [StatikQuest](https://github.com/Jerware/StatikQuest) — another UE4 PlayStation VR title whose
  launcher switches its own frame rate through `Engine.ini`; the source of the `PSVRFrameSequence`
  route and the 90 Hz variant (`Render90Scanout90`).
