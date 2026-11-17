# AGENTS.md

Agent notes for **Déraciné PCVR** — a fork of shadPS4 that adds native OpenXR output and
PlayStation Move emulation for *Déraciné* (CUSA11919). Windows x86_64 only. Engineering rules
for the C++ tree live in `src/AGENTS.md` and override inherited upstream guidance.

- Player-facing docs: `README.md`
- Architecture, PSVR library mapping, memory offsets, porting history: `DEVELOPMENT.md`
- v0.21.0 merge log and resolved build/runtime errors: `HANDOFF_V0.21.0.md`

## Layout

- `pc-vr/` — runtime folder: `shadps4.exe`, `launch.ps1`, `settings.txt` (user config), and
  `user/` (config, patches, input maps).
- `Play Deracine VR.bat` — end-user launcher; reads `pc-vr/settings.txt`.
- `src/` — the shadPS4 tree; the CMake root is `src/`, not the repo root. Sources are one level
  deeper, in `src/src/`:
  - `src/src/core/vr/` — OpenXR session and host link (`openxr_host.cpp`, `vr_runtime.cpp`,
    `vr_host_link.cpp`).
  - `src/src/core/libraries/move/` — `libSceMove` (`move.cpp`).
  - `src/src/core/libraries/vr_tracker/` — `libSceVrTracker` (`vr_tracker.cpp`).
  - `src/src/video_core/renderer_vulkan/` — `vk_presenter.cpp`, `vk_vr_exporter.cpp`.
- `tools/` — helper scripts, including `download-artifact.py` and `fetch-submodules.py`.
- `.github/workflows/build-windows.yml` — Windows x86_64 CI build.

## Rules

- Keep `swap_hands=1` in `pc-vr/settings.txt`; the game's Move mapping depends on it.
- Engineering rules for the C++ tree are in `src/AGENTS.md`: title-specific compatibility work is
  allowed there when justified, and automated tests are expected.
- Match the surrounding code and `src/CONTRIBUTING.md` style; avoid new external dependencies in
  Core. The VR/OpenXR and Move code is deliberately platform-specific and lives under
  `src/src/core/`.
- Keep changes focused. Do not refactor or reformat unrelated code.
- Treat `optimizacion-graficos` as read-only: do not push to or rewrite it unless asked.

## Building

- Externals: run `python tools/fetch-submodules.py` from the repo root. `git submodule update`
  does nothing here; `src/.gitmodules` is only the fetch list.
- CI: `.github/workflows/build-windows.yml` builds with LLVM/Clang or MSVC;
  `python tools/download-artifact.py` pulls the result into `pc-vr/`.
- Local Windows build quirks (clang-cl, LLVM 23, `-DHAVE_ENDIAN_H:INTERNAL=`, pinned
  `magic_enum` v0.9.7, `ENABLE_TESTS=ON` replacing the emulator target) are documented in
  `_build/SETUP.md` in the parent worktree, which is not tracked here.

## Running

`Play Deracine VR.bat` reads `pc-vr/settings.txt` and launches `pc-vr/shadps4.exe`. The game dump
lives in `games/` or is selected in the launcher.
