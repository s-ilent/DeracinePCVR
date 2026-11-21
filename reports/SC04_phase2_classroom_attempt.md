# SC-04 Phase 2: the classroom scene was not reached

## Result

The classroom/title-menu scene was **not** reached in any SC-04 cell. The scene gate never
passed, and the plateau that looked like the classroom band is a **black screen**.

This is reported as unverified rather than guessed.

## What was tried

| Cell | Autopress | Capture | Outcome |
| --- | --- | --- | --- |
| `classroom-run-1` | 1 | ON | Guest crash `0xc0000005` at `0x800DFA8D0` |
| `classroom-off-run-1` | 1 | OFF | Reached 624 draws / 502.7 Mpx, screenshot similarity 0.4283 |
| `classroom-run-2` | 1 | ON | Completed, 4447 draws / 9707 Mpx, classification `unverified` |
| `classroom-run-3` | 1 | ON, 30 s delay | Completed, classification `unverified` |
| `plateau-run-1` | 1 | ON, 32 s delay | Dumped at 38 s, all images uniform black |

## The band mismatch

The classroom gate requires two consecutive windows with `draws >= 400` and `Mpx >= 500`.

The plateau that resembles the classroom in draw count sits at **446 draws and 469 Mpx**:

| Time | Draws | Mpx | Passes |
| --- | --- | --- | --- |
| 34 s | 446 | 469.1 | 80.6 |
| 35 s | 435 | 460.6 | 80.1 |
| 35 s | 458 | 448.4 | 79.6 |
| 36 s | 445 | 437.1 | 78.9 |
| 37 s | 447 | 439.3 | 79.6 |
| 38 s | 447 | 441.5 | 79.6 |

Across all 245 samples of `classroom-run-3`, **zero** satisfied both `draws >= 400` and
`Mpx >= 500`. The maximum Mpx in the 400-800 draw range was 469.1.

So the draw count matches the classroom's expected ~465-500, but the pixel load is ~7% below the
gate. Either the gate's Mpx floor is slightly high for this build, or this plateau is a different
scene with a similar draw count.

## The plateau is black

A render-target dump taken at 38 s, inside that plateau, produced uniform images:

```
008_2020000000_5760x3240_R8G8B8A8Unorm_s1.png: image is uniform (std=0.000000)
009_2010000000_5760x3240_R8G8B8A8Unorm_s1.png: image is uniform (std=0.000000)
010_2018000000_5760x3240_R8G8B8A8Unorm_s1.png: image is uniform (std=0.000000)
```

The scene-signature tool refuses to identify a uniform image, which is correct: a black frame
carries no scene identity.

So the 446-draw plateau is a **black transition screen** that happens to have a classroom-like
draw count. It is not the classroom.

## The one cell that did reach the gate

`classroom-off-run-1` reached 624 draws / 502.7 Mpx and passed the draw and Mpx gate. Its
screenshot similarity was **0.4283**, below the 0.85 threshold, so the scene gate still failed.

That is a real near-miss worth recording: the run reached a scene with the right workload but the
wrong picture. It is not the classroom either.

## Why the capture cells differ

`classroom-run-1` crashed with an access violation at `0x800DFA8D0` (guest function
`0x0DFA7B0`) after the capture buffer filled. `classroom-run-2` and `-run-3` did not crash once
the per-word mapping query was replaced with a single span validation.

So the crash was a capture-induced guest stall, and it is fixed. That is a real finding from this
phase even though the scene was not reached.

## What this means for the remaining phases

Phase 3 and Phase 4 require a scene-verified capture. Without the classroom, the only
scene-identified capture available is the **PSVR health warning** from Phase 1.

That is a legitimate, independently verified scene. It is not the classroom, and it must not be
presented as such.

## The bounded next step

The classroom needs `Mpx >= 500` at ~446 draws, and this build produces ~469.

A longer run was tried to test whether the classroom appears later. `long-run-1` ran 200 s with
dumps at 40, 70, 100 and 130 s. Every dump was a loading screen:

| Dump | Similarity to classroom |
| --- | --- |
| 40 s | -0.0768 |
| 70 s | -0.0358 |
| 100 s | -0.0259 |

Across 205 samples, exactly **one** reached the band: 34 s, 469 draws, 501.0 Mpx. One sample is
not two consecutive windows, so the gate still cannot pass.

So the scene is not reached later either. The classroom is not reachable in this configuration.

## What the evidence supports

Two candidate explanations remain, and this phase does not distinguish them:

1. The gate's Mpx floor was calibrated on a different build. The reference note cites branch
   `test/v0.21.0-plus-fixes`; this branch descends from `main` at `e2f981c4`. A renderer
   difference would change Mpx at the same draw count, and 469 versus 500 is a 6% gap.
2. The classroom requires input this harness does not supply. The autopress schedule advances
   past the health warning and into loading, but never settles on the menu.

Distinguishing them needs the reference build, which is outside this branch's scope.

## Reproducible checks

```
# the gate
Select-String -Path shad_log.txt -Pattern 'frame stats at'

# a dump inside a chosen interval
[IO.File]::WriteAllText("<run>/lab/user/dump_render_targets", "1`r`n")

# scene identity
python tools/benchmark/scene_signature.py compare <dump>/010_*5760x3240*.png `
  --reference tools/benchmark/reference/scene-classroom-menu.json
```

A uniform image raises `ValueError: image is uniform`, which is the tool correctly refusing to
identify a blank frame.
