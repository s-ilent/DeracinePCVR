# RHI-04: first original capture — preflight

## Status

**AWAITING_EXPLICIT_LAUNCH_PERMISSION**

The offline preflight is complete and every check passed. The emulator has not been launched. No
runtime result exists yet, and none is claimed.

## 1. Branch

| Item | Value |
| --- | --- |
| Experiment branch | `research/rhi04-first-original-capture-20261125` |
| Base | `fbe46f9ca341984386b0d614c14f112963d14e3b` (RHI-03P) |
| Integration branch | `integration/rhi03-unified-original-shader-renderer`, unchanged |
| `main` | `e2f981c40cba7c8490177c20c179cb3608cc1442`, unchanged |

The experiment branch is for analysis, diagnostics and reports. It is not merged into `main` or into
the integration branch.

## 2. Emulator identity

| Item | Value |
| --- | --- |
| Source commit | `fbe46f9ca341984386b0d614c14f112963d14e3b` |
| Binary | `build/shadPS4.exe` |
| SHA-256 | `87A8589C06F4A4FBCB81D873C5EEE31B45B4F7EA10C44BEFC43265B22D4EE6A7` |
| Size | 77,401,088 bytes |

Built from the RHI-03P source in this worktree. The build completed with no errors.

## 3. Game executable

| Item | Value |
| --- | --- |
| Path | the approved 1.00 SELF, `1.0/Image0/eboot.bin` under the local dump root |
| SHA-256 | `8CAEE78E40693B2E57C68CE87CE9507D907B824B6F6A4C481F414AC0627011A0` |
| Size | 81,506,767 bytes |
| Approved | **yes**, exact match |

The `CUSA11919-qual` image is present and hashes to
`8DA8E46AC5FDC48E4E9B59E459920A266039993DC13A2D92740F8803E46FDF75`. It is **rejected** and will not
be used. Four other Déraciné images exist locally (`CUSA11919-1.01`, `CUSA11919-color`,
`CUSA11919-guardrails-2026-11-17`, `Update`); none is used.

## 4. Capture directory

| Item | Value |
| --- | --- |
| Path | `_build/rhi04-capture-1` under the local work root |
| State | does not exist yet, so it is fresh |
| Free space | 117.2 GB |

The run refuses to proceed if the directory already contains capture artifacts. No SC-10, SC-11 or
RHI-02 capture is mixed into this execution.

## 5. Lab

| Item | Value |
| --- | --- |
| Path | `_build/headless/labs/rhi04-capture-1` under the local work root |
| Built from | `sc10s-stability`, a lab known to reach the capture point |
| Save slot | `MGS_GameData_1` present, plus `MGS_SystemData` and `sce_sys` |
| Stale logs | cleared |
| Stale render-target dumps | removed |

## 6. Flag verification

Every flag the run uses is checked against its source definition.

| Flag | Status |
| --- | --- |
| `SHADPS4_DERACINE_CAPTURE_SESSION` | present |
| `SHADPS4_DERACINE_CAPTURE_SESSION_DIR` | present |
| `SHADPS4_DERACINE_RHI_PROBE` | present |
| `SHADPS4_DERACINE_RHI_PROBE_AFTER` | present |
| `SHADPS4_DERACINE_DRAW_PROVENANCE` | present |
| `SHADPS4_DERACINE_INDEX_IDENTITY` | present |
| `SHADPS4_DERACINE_VERTEX_INPUT` | present |
| `SHADPS4_DERACINE_SHADER_PARAMS` | present |
| `SHADPS4_DERACINE_TEXTURE_CAPTURE` | present |
| `SHADPS4_DERACINE_SHADER_BINARY` | present, **singular** |
| `SHADPS4_DERACINE_SHADER_INTERFACE` | present |
| `SHADPS4_DERACINE_REFERENCE_OUTPUT` | present |
| `SHADPS4_DERACINE_DRAW_PACKAGE` | present |
| `SHADPS4_DERACINE_SEMANTIC_TAPE` | present |
| `SHADPS4_DERACINE_SEMANTIC_TAPE_DIR` | present |
| `SHADPS4_DERACINE_CMD_CAPTURE` | present |

`SHADPS4_DERACINE_SHADER_BINARIES` (plural) does not exist in the source. The singular form is the
only one that captures modules.

`SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` will **not** be set. RHI-02I-R3 made the package-selected
provenance authoritative, and pinning it would be a selection mechanism rather than evidence.

## 7. Default-OFF verification

Every capture observer gates on `value == nullptr || value != "1"`, so an unset variable disables it.
Outside this experiment no capture instrumentation is enabled.

## 8. Process ownership

| Item | Value |
| --- | --- |
| Running emulator instances | none |
| Other capture processes | none |

The run will record the exact PID of the process it starts. No broad process kill will be used. Only
that PID may be managed, and only within the approved scope.

## 9. Execution bounds

| Control | Value |
| --- | --- |
| `SHADPS4_EXIT_AFTER` | 75 seconds |
| `SHADPS4_CLEAN_EXIT` | 1, the normal quit path |
| Runs | one, no automatic retry |

The emulator exits through its documented clean-exit mechanism. If it crashes, hangs or fails to
reach the capture point, the failure evidence is preserved and the run stops.

## 10. Expected artifacts

```text
<capture-directory>/
    session.json          the selection and verification record
    package.json          schema 4
    tape.json             schema 2, session-bearing
    index_bridge.json     the original-object-to-guest-allocation bridge
    resources.bin         geometry and shader-buffer bytes
    textures.bin          texture bytes, if a texture was captured
    stage_*.spv           effective shader modules, if captured
    reference_post.bin    the post-draw reference, if captured
    reference_pre.bin     OPTIONAL: only when that capture succeeded
```

Any of the last four may be absent. Their absence is a result, not a failure of the run.

## 11. Known limitations

These are carried from RHI-03P and are not expected to be resolved by this capture.

| Limitation | Consequence |
| --- | --- |
| Colour attachment format is a GNM packed format | the replay renders into its own RGBA8 target |
| Blend factors and operations are not captured | a package with blend enable is refused |
| Effective shader state at the draw boundary is not recovered | the modules are captured, the selection is not |
| Texture and sampler descriptor writers are not established | a texture binding's declaration is unresolved |
| Render targets are substituted | the diagnostic target is the replay's |

The capture's purpose is to establish what the game actually provides, and to identify the first
unresolved relationship from real evidence.

## 12. What the run will establish

The research question is whether one original Déraciné indexed draw produces a semantic tape, an
original RHI index-resource bridge, a downstream resource package and captured effective shaders that
all correspond to the same execution and draw.

A successful shader replay is desirable but not required. A correctly diagnosed incomplete capture is
the preferred outcome over a fabricated success.

## 13. Request

Explicit permission is required to launch the emulator once, for the bounded run described above.
