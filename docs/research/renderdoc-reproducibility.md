# RenderDoc reproducibility — the one successful shadPS4 capture (CUSA11919, frame 532)

Forensic reconstruction from files only. Nothing here was run except `renderdoccmd capture --help`
and read-only queries (`Get-FileHash`, `git show/log`, file reads). No emulator was launched, no
injection was performed, no `.rdc` was touched.

Date of the recorded session: 2026-11-17 (Australia/Adelaide, UTC+10:30).
Document written: 2026-11-17.

Legend used throughout: **proven** = a file or log states it directly; **plausible** = consistent
with evidence but not established; **unknown** = no file can settle it.

---

## 1. Provenance table

| item | value | evidence |
|---|---|---|
| Executable used | `M:\Development\Deracine-work\_build\bench\bin\rdoc.exe` | `win32_process.cpp(526)` — `Running process M:\Development\Deracine-work\_build\bench\bin\rdoc.exe` |
| Size / mtime | 73,011,200 B / 2026-11-17 10:38:47 | `Get-Item`; file still present today |
| SHA-256 | `F061B0624849B8B1AD1CD174E81C64E6DBE856CF1BFF4D2331413A7473A56EEF` | computed this session |
| Signature | not signed | `Get-AuthenticodeSignature` → `NotSigned` |
| Source commit | `633d212ff5c8def6431bf6729a41a89ac22a314f` — "wip(diagnostics): alias/representation trace, coalesced per-second reports, RenderDoc trigger", Tue Nov 17 10:39:29 2026 +1030 | `git -C M:\Development\Deracine-work\DeracinePCVR log -1 633d212`; binary built 10:38:47, commit created 42 s later from that working tree |
| Commit ancestry | `633d212` **is** an ancestor of `de9c78c` (`merge-base --is-ancestor` → true) | verified this session |
| RenderDoc version | `v1.47 Windows 64-bit Release (b0e1370309f7d7bf986a73830446ca39d9b8c1fd)` | `core.cpp(719)`, first line of every session log |
| Installed files | `renderdoc.dll` 27,706,656 B, FileVersion 1.47.0.0, ProductVersion v1.47, dated 2026-10-31 09:38:24 | `Get-Item` / `VersionInfo` |
| Injector | `C:\Program Files\RenderDoc\renderdoccmd.exe` started as RenderDoc's "replay application" | `core.cpp(848) - Replay application 'C:\Program Files\RenderDoc\renderdoccmd.exe' launched`, then `core.cpp(852) - Parameter [0]: --crash` (its crash-handler child), then `win32_process.cpp(610) - Injecting renderdoc into process 44992` |
| Capture file | `M:\Development\Deracine-work\lab\pc-vr-quality\user\captures\CUSA11919_frame532.rdc`, 65,702,449 B, mtime 10:42:22 | `core.cpp(2231) - Written to disk: ...`; `Get-Item` |
| Capture SHA-256 | `479C633580CA2A1DD9295624BF21C2EA6EA3851D0D0BA424D439351E9B7DB0A9` | computed for both existing copies; identical |
| Second copy | `M:\Development\Deracine-work\wt-autonomous-guardrails\_lab\pc-vr\user\captures\CUSA11919_frame532.rdc` — same size, same mtime 10:42:22, same SHA-256 | it is a copy of the 10:42 file, not a later capture |
| Capture file template (runtime) | `M:\Development\Deracine-work\lab\pc-vr-quality\user\captures\CUSA11919` | `app_api.cpp(67) - Using capture file template ...` at 10:42:09 |
| Template in config | **none** — `renderdoc.conf` has no `Core.*` section and no capture-path/template key at all (see Appendix A) | file read; only `Capture.IncludeExtendedThumbnail=false` and `Capture.Debug.SnapshotDiagnosticLog=false` exist under `Capture` |
| User directory | `M:\Development\Deracine-work\lab\pc-vr-quality\user` | derived from the template; equals `<cwd>\user` (`PORTABLE_DIR`) |
| Working directory of the run | `M:\Development\Deracine-work\lab\pc-vr-quality` | proven indirectly: `user_dir = current_path()/user` and the template is under exactly that path |
| emulator user dir rule | `std::filesystem::current_path() / PORTABLE_DIR` when it exists, else `%APPDATA%\shadPS4` on Windows | source, §1.1 below |
| Game | `-g D:\Temp\Deracine\CUSA11919-qual\eboot.bin` | `D:\Temp\rdoc\probe.txt` + the emulator's own argv echo `main.cpp:351` |
| Timestamp | trigger 10:42:20, capture finished 10:42:21, file on disk 10:42:22 | RenderDoc log |
| Contents | Vulkan, 233 actions (60 draws / 30 dispatches / 26 clears), 1280×720 window, 5760×3240 render targets, "Sony Interactive Entertainment presents" splash | `_handoff\2026-11-17\RENDERDOC-RECOVERY.md` |

### 1.1 How the capture path becomes `<user>/captures/<serial>`

The path is assembled entirely inside the emulator, in two steps, and handed to RenderDoc as a
filename template (RenderDoc appends `_frame<N>` and `.rdc` itself).

Step 1 — `src/src/emulator.cpp:496-500`, verbatim:

```cpp
    const auto& mount_captures_dir = Common::FS::GetUserPath(Common::FS::PathType::CapturesDir);
    if (!std::filesystem::exists(mount_captures_dir)) {
        std::filesystem::create_directory(mount_captures_dir);
    }
    VideoCore::SetOutputDir(mount_captures_dir, id);
```

`id` is the title serial parsed from the dump — `CUSA11919` here.

Step 2 — `src/src/video_core/renderdoc.cpp:124-130`, verbatim:

```cpp
void SetOutputDir(const std::filesystem::path& path, const std::string& prefix) {
    if (!rdoc_api) {
        return;
    }
    LOG_WARNING(Common, "RenderDoc capture path: {}", (path / prefix).string());
    rdoc_api->SetCaptureFilePathTemplate(fmt::UTF((path / prefix).u8string()).data.data());
}
```

`SetOutputDir` has exactly one caller in the tree (`emulator.cpp:500`; `git grep SetOutputDir de9c78c`
also returns only its declaration in `renderdoc.h:25`). Its `LOG_WARNING(Common, ...)` goes to the
emulator's own `shad_log.txt`, not to RenderDoc's log; only `app_api.cpp(67)` in RenderDoc's log
records the result.

Step 3 — what `<user>` is, `src/src/common/path_util.cpp:87-115`, verbatim (abridged only where
marked by `…`):

```cpp
static auto UserPaths = [] {
    // Try the portable user directory first.
    auto user_dir = std::filesystem::current_path() / PORTABLE_DIR;
    if (!std::filesystem::exists(user_dir)) {
        // If it doesn't exist, use the standard path for the platform instead.
        // NOTE: On Windows we currently just create the portable directory instead.
#ifdef __APPLE__
        …
#elif defined(__linux__)
        …
#elif _WIN32
        TCHAR appdata[MAX_PATH] = {0};
        SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata);
        user_dir = std::filesystem::path(appdata) / "shadPS4";
#endif
    }
```

with

```cpp
constexpr auto PORTABLE_DIR = "user";
constexpr auto CAPTURES_DIR = "captures";
```

(`src/src/common/path_util.h:33,43`), and `create_path(PathType::CapturesDir, user_dir / CAPTURES_DIR);`
at `path_util.cpp:137`.

Therefore, on Windows:

* The user directory is the **working directory** of the emulator process plus `\user` — **not** the
  executable's directory, and **not** an environment variable. In the 10:42 run the working directory
  was `M:\Development\Deracine-work\lab\pc-vr-quality`, so the user dir was
  `…\lab\pc-vr-quality\user` and the template `…\user\captures\CUSA11919`. Note that the executable
  lived somewhere else entirely (`_build\bench\bin\rdoc.exe`), which is why the working directory had
  to be set explicitly.
* If `<cwd>\user` does not exist, the portable rule does not apply and the fallback is
  `%APPDATA%\shadPS4` (i.e. `C:\Users\<user>\AppData\Roaming\shadPS4`), a path that would silently
  move the capture out of the lab.
* There is **no override**: `SetUserPath` is defined at `path_util.cpp:200` and has no caller anywhere
  in `src/src`, and `main.cpp` declares no user-dir/portable option (only `-g`, `-p`, `-i`, …). The
  only way to choose the user directory is the working directory.

### 1.2 The emulator's RenderDoc integration — injected vs. `renderdoc_enabled`

`src/src/video_core/renderdoc.cpp:33-94`, verbatim (the `#else` non-Windows branch elided):

```cpp
void LoadRenderDoc() {
#ifdef WIN32

    // Check if we are running by RDoc GUI
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod && EmulatorSettings.IsRenderdocEnabled()) {
        // If enabled in config, try to load RDoc runtime in offline mode
        HKEY h_reg_key;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                    L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0,
                                    KEY_READ, &h_reg_key);
        if (result != ERROR_SUCCESS) {
            return;
        }
        std::array<wchar_t, MAX_PATH> key_str{};
        DWORD str_sz_out{key_str.size()};
        result = RegQueryValueExW(h_reg_key, L"", 0, NULL, (LPBYTE)key_str.data(), &str_sz_out);
        if (result != ERROR_SUCCESS) {
            return;
        }

        std::filesystem::path path{key_str.cbegin(), key_str.cend()};
        path = path.parent_path().append("renderdoc.dll");
        const auto path_to_lib = path.generic_string();
        mod = LoadLibraryA(path_to_lib.c_str());
    }

    if (mod) {
        const auto RENDERDOC_GetAPI =
            reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
        const s32 ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc_api);
        ASSERT(ret == 1);
    }
#else
    …
#endif
    if (rdoc_api) {
        // Disable default capture keys as they suppose to trigger present-to-present capturing
        // and it is not what we want
        rdoc_api->SetCaptureKeys(nullptr, 0);

        // Also remove rdoc crash handler
        rdoc_api->UnloadCrashHandler();
    }
}
```

The state machine that the emulator's own F12 path uses (`renderdoc.cpp:96-122`) is separate from the
scheduler trigger:

```cpp
void StartCapture() {
    if (!rdoc_api) {
        return;
    }

    if (capture_state == CaptureState::Triggered) {
        rdoc_api->StartFrameCapture(nullptr, nullptr);
        capture_state = CaptureState::InProgress;
    }
}

void EndCapture() {
    if (!rdoc_api) {
        return;
    }

    if (capture_state == CaptureState::InProgress) {
        rdoc_api->EndFrameCapture(nullptr, nullptr);
        capture_state = CaptureState::Idle;
    }
}

void TriggerCapture() {
    if (capture_state == CaptureState::Idle) {
        capture_state = CaptureState::Triggered;
    }
}
```

driven from `src/src/video_core/amdgpu/liverpool.cpp:130` (`VideoCore::StartCapture();` at the top of
each GPU submission batch) and `:173` (`VideoCore::EndCapture();` when `submit_done`), via
`SDL_EVENT_RDOC_CAPTURE` in `sdl_window.cpp:443-451`:

```cpp
    case SDL_EVENT_RDOC_CAPTURE:
        if (VideoCore::IsRenderDocLoaded()) {
            VideoCore::TriggerCapture();
        } else {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        }
        break;
```

which is produced by `HOTKEY_RENDERDOC` (`input_handler.cpp:780`), default bound to `f12`
(`input_handler.cpp:170`: `{"hotkey_capture_frame", "f12"},`).

What `LoadRenderDoc()` does differently, case by case:

| situation | behaviour | consequence |
|---|---|---|
| **Injected** (`renderdoccmd capture` put `renderdoc.dll` into the process) | `GetModuleHandleA("renderdoc.dll")` succeeds, so the whole registry/`LoadLibraryA` branch is skipped; `RENDERDOC_GetAPI(1.6.0)` returns the API; then `SetCaptureKeys(nullptr, 0)` and `UnloadCrashHandler()`. | `renderdoc_enabled` is **irrelevant**. This is the path the 10:42 session took, with `renderdoc_enabled=false`. RenderDoc's F12 is removed, the emulator's own F12 still works, and RenderDoc no longer writes crash dumps for the process. |
| **Not injected, `renderdoc_enabled=true`** ("offline mode") | reads `HKLM\SOFTWARE\Classes\RenderDoc.RDCCapture.1\DefaultIcon` (on this machine the value is `C:\Program Files\RenderDoc\qrenderdoc.exe`), takes its parent directory, appends `renderdoc.dll` → `C:\Program Files\RenderDoc\renderdoc.dll`, and `LoadLibraryA`s it into a process RenderDoc is not otherwise attached to. Same API binding, same key removal, same crash-handler removal. | a capture can be triggered from inside the app with no injector — but there is no `.rdc` writing until a capture is requested, and the scheduler trigger will find `renderdoc.dll` loaded. |
| **Not injected, `renderdoc_enabled=false`** | `mod` stays null, `rdoc_api` stays null. Every API function returns immediately; `SetOutputDir` logs nothing; `IsRenderDocLoaded()` is false so F12 takes a PNG screenshot instead. | no capture of any kind is possible; the scheduler's DLL probe fails and logs one INFO line (§4.1). |

The setting is therefore about **how RenderDoc gets into the process**, never about whether a capture
is allowed. Two config values confirm the state today: `renderdoc_enabled=false` in both configs
(§5.4), and `vkvalidation_core_enabled=true` (§4.2 #7).


### Environment variables that matter

| variable | role | value in the successful run |
|---|---|---|
| `SHADPS4_DBG_RDOC_CAPTURE` | arms the one-shot trigger in `vk_scheduler.cpp`; seconds measured from the **first `FrameStats::EndFrame()`** | set and > 0 — **proven**, see below; value ≈ 10 (reconstructed, not recorded) |
| `SHADPS4_TITLE_EYE_WIDTH`, `SHADPS4_TITLE_RESOLUTION`, `SHADPS4_VR_SCALE`, `SHADPS4_MAX_MSAA`, `SHADPS4_FORCE_ANISO`, `SHADPS4_VR_SHARPEN`, `SHADPS4_VR_DESKTOP_VIEW`, … | render configuration that produces the 5760×3240 targets | not recorded for 10:42; the only surviving complete set is the bench meta (`_build\bench\runs\alias-trace-1\meta.json`) and `_build\crossfork\rdoc-run.ps1` |
| `SHADPS4_DBG_AUTOPRESS`, `SHADPS4_DBG_AUTOPRESS_TO` | pad automation (menu → classroom) | **no evidence** it was set in the 10:42 run; absent from `probe.txt`, which is the emulator's argv only |
| `RENDERDOC_*` | none needed | not used; capture path came from the emulator, not from RenderDoc env |

`SHADPS4_DBG_RDOC_CAPTURE` being set in the 10:42 run is provable from the log, not assumed: the
only caller that requests RenderDoc API **1.4.0** is that function in the `633d212` build, and the
log shows `app_api.cpp(448) - Initialising RenderDoc API version 1.7.0 for requested version 10400`
at 10:42:10. The emulator's own integration (`renderdoc.cpp`) requests 1.6.0 and logs 10600 at
10:42:07. The two requests are distinguishable, and the 10400 one can only come from the trigger.

---

## 2. Exact reproduction procedure

The 10:42 session was launched by `renderdoccmd capture`, not by the RenderDoc UI and not by
double-clicking. That is proven by `D:\Temp\rdoc\probe.txt`, whose first line is
`Launching 'M:\Development\Deracine-work\_build\bench\bin\rdoc.exe' with params: -g D:\Temp\Deracine\CUSA11919-qual\eboot.bin`
— and those two strings ("`Launching '` … `with params: `" and "`' finished executing.`") are
literal string constants inside `renderdoccmd.exe` (verified by scanning the binary). The same
message shape appears in `D:\Temp\rdoc\class-out.txt` for the second attempt.

### 2.1 Reconstructed command line

```powershell
# working directory for the shell: M:\Development\Deracine-work\lab\pc-vr-quality
& 'C:\Program Files\RenderDoc\renderdoccmd.exe' capture -w `
    -d 'M:\Development\Deracine-work\lab\pc-vr-quality' `
    'M:\Development\Deracine-work\_build\bench\bin\rdoc.exe' `
    -g 'D:\Temp\Deracine\CUSA11919-qual\eboot.bin'
```

Flag by flag, and why:

* `capture` — `renderdoccmd`'s launch-and-inject subcommand (`renderdoccmd capture --help`).
* `-w, --wait-for-exit` — **required for the observed artefacts**: only with `-w` does
  `renderdoccmd` keep the child and print `'…rdoc.exe' finished executing.` after it exits, which is
  in `probe.txt`. Without `-w` it detaches and returns immediately.
* `-d, --working-dir` — sets the child's working directory to `…\lab\pc-vr-quality`, which is what
  makes the emulator's portable user dir `…\lab\pc-vr-quality\user` and therefore the capture
  template `…\user\captures\CUSA11919`. The log proves the *effective* working directory was that
  path; whether it was passed with `-d` or simply inherited from the launching shell cannot be told
  apart from the files. `-d` is the explicit, reproducible form.
* **no `-c, --capture-file`** — the only template assignment in the log is
  `app_api.cpp(67)` at 10:42:09, i.e. the emulator's own `SetCaptureFilePathTemplate`. A `-c`
  template would have been installed before the app ran.
* `<executable>` — `rdoc.exe` itself, exactly as logged by `win32_process.cpp(526)`.
* `-g …` — everything after the executable is the guest's argv. The emulator's own echo
  (`main.cpp:351`) records the full argv as `["…\rdoc.exe", "-g", "D:\\Temp\\Deracine\\CUSA11919-qual\\eboot.bin"]`,
  so **no `-p` patch file was passed** in the 10:42 run (a patch file does exist at
  `lab\pc-vr-quality\user\patches\CUSA11919.xml`; it was not used).

### 2.2 Environment, in the shell that calls `renderdoccmd`

`renderdoccmd` does not read the emulator's `settings.txt`; it only inherits the parent environment
and passes it to the child. So the render configuration has to be exported before the call. The
only complete, recorded set that reproduces the capture's 300 % / `VR_SCALE=0.5` stereo layout is
`_build\crossfork\rdoc-run.ps1`'s `$envMap` (identical to `_build\bench\runs\alias-trace-1\meta.json`
`env`):

```powershell
$env:SHADPS4_DBG_RDOC_CAPTURE = '10'     # ← the trigger; ≈10 s after the first presented frame
$env:SHADPS4_TITLE_EYE_WIDTH  = '2880'
$env:SHADPS4_TITLE_RESOLUTION = '6'
$env:SHADPS4_VR_SHARPEN       = '0.25'
$env:SHADPS4_FORCE_ANISO      = '16'
$env:SHADPS4_VR_DESKTOP_VIEW  = 'stereo'
$env:SHADPS4_VR_DESKTOP_CROP  = '0'
$env:SHADPS4_VR_FOV_OF        = 'headset'
$env:SHADPS4_MAX_MSAA         = '2'
$env:SHADPS4_VR_REFRESH_RATE  = '120'
$env:SHADPS4_VR_FPS_CAP       = '120'
$env:SHADPS4_VR_FASTEST_PACE  = '1'
$env:SHADPS4_XR_PAUSE         = '0'
$env:SHADPS4_XR_WAIT          = '60'
$env:SHADPS4_CONSOLE_LANGUAGE = 'en-US'
$env:SHADPS4_MOVE_PITCH       = '-45'
$env:SHADPS4_MOVE_Y           = '0.000'
$env:SHADPS4_MOVE_Z           = '-0.135'
$env:SHADPS4_SWAP_HANDS       = '1'
$env:SHADPS4_VR_SCALE         = '0.5'
$env:SHADPS4_FRAME_STATS      = '1'
$env:SHADPS4_FRAME_STATS_EVERY= '0.5'
```

Why each group is needed:

* `SHADPS4_DBG_RDOC_CAPTURE` — nothing else asks RenderDoc for a capture. RenderDoc's own F12 is
  disabled by the emulator (`SetCaptureKeys(nullptr, 0)`, §1.2), so without this variable a run that
  nobody touches with a keyboard produces **no** capture, which is exactly what is observed for the
  10:43 and 20:23 sessions.
* `SHADPS4_TITLE_EYE_WIDTH` / `_RESOLUTION` / `VR_SCALE` / `MAX_MSAA` / `FORCE_ANISO` /
  `VR_SHARPEN` / `VR_DESKTOP_VIEW` — the render configuration: these are what make the eye targets
  5760×3240 side-by-side at `VR_SCALE=0.5` instead of the default. They are a property of the scene
  you want in the capture, not of RenderDoc.
* `SHADPS4_MOVE_*` / `SWAP_HANDS` / `XR_WAIT` / `XR_PAUSE` / `VR_FPS_CAP` / `VR_FASTEST_PACE` —
  keep the headset slot and the Move mapping as the benchmark assumes; a paused or capped session
  changes how many frames pass before the timer expires.
* `SHADPS4_FRAME_STATS*` — harmless here, but present in every recorded run and needed if you want
  to correlate frames with the emulator log.
* `SHADPS4_DBG_AUTOPRESS=1` + `SHADPS4_DBG_AUTOPRESS_TO=150000` — **only** if you want to leave the
  front-end and reach the classroom. It was *not* in the successful run as far as any file shows,
  and frame 532 is the SIE splash exactly because nothing pressed anything.

### 2.3 Numbered procedure

1. Confirm RenderDoc 1.47 is installed: `C:\Program Files\RenderDoc\renderdoc.dll` (27,706,656 B)
   and `renderdoccmd.exe` exist.
2. Confirm the binary is the exact one: `Get-FileHash M:\Development\Deracine-work\_build\bench\bin\rdoc.exe -Algorithm SHA256`
   must be `F061B0624849B8B1AD1CD174E81C64E6DBE856CF1BFF4D2331413A7473A56EEF`. A rebuild from the
   same commit will **not** match byte-for-byte.
3. Confirm the game dump: `D:\Temp\Deracine\CUSA11919-qual\eboot.bin` exists.
4. Record the pre-existing captures so nothing is mistaken for the new one:
   `Get-ChildItem M:\Development\Deracine-work\lab\pc-vr-quality\user\captures\*.rdc`.
5. Open a PowerShell window **in** `M:\Development\Deracine-work\lab\pc-vr-quality` and export the
   environment block from §2.2.
6. Run the command line from §2.1 (with `-d` pointing at the same directory).
7. Wait for `'…rdoc.exe' finished executing.` — that is `renderdoccmd` reporting the child's exit,
   not a success message.
8. Check the new file: `…\user\captures\CUSA11919_frame<N>.rdc` with a byte size in the tens of MB
   and an mtime inside the session window. The capture itself is confirmed by the RenderDoc log for
   that session (`Starting capture` → `Finished capture, Frame N` → `Written to disk`).
9. If no file appeared, read the session log in `%LOCALAPPDATA%\Temp\RenderDoc\RenderDoc_<date>_<time>.log`
   before re-running: the presence of `requested version 10400` proves the trigger was armed, its
   absence proves the binary or the variable was wrong.

`renderdoccmd capture --help` (run this session; only prints help) documents the relevant flags:

```
usage: renderdoccmd capture [options ...] <executable> [program arguments]

options:
  -d, --working-dir                            Set the working directory of the program, if launched. (optional string)
  -c, --capture-file                           Set the filename template for new captures. Frame number will be automatically appended. (optional string)
  -w, --wait-for-exit                          Wait for the target program to exit, before returning.
      --opt-disallow-vsync                     Capturing Option: Disallow the application from enabling vsync.
      --opt-api-validation                     Capturing Option: Record API debugging events and messages.
      --opt-capture-callstacks                 Capturing Option: Capture CPU callstacks for API events.
      --opt-delay-for-debugger                 Capturing Option: Specify a delay in seconds to wait for a debugger to attach. (optional int=0) Must be within [0, 10000]
```

(There is no `-p` option; a `-p` in the runner script below lands on the emulator's own
`-p,--patch` argument because it follows the executable.)

### 2.4 The newer, isolated runner (do not confuse it with the 10:42 command)

`_build\crossfork\rdoc-run.ps1` is a later, safer wrapper written after the incident. It builds
exactly the shape above and adds bookkeeping:

```powershell
$arguments = @('capture', '-w', '-d', $Lab, $Exe, '-g', $Eboot)
if (Test-Path $patch) { $arguments += @('-p', $patch) }
...
$job = Start-Job -ScriptBlock { param($cmd, $args2) & $cmd @args2 2>&1 } -ArgumentList $RenderDocCmd, $arguments
```

with `$Lab = <root>\_build\crossfork\rdoc-lab`, a **private** portable dir it creates by copying
`lab\pc-vr-quality\user` (excluding `captures`). Two differences from the 10:42 session: the capture
lands in `_build\crossfork\rdoc-lab\user\captures` instead of the shared lab, and the runner
`Stop-Process`es the emulator after its capture timeout. It also supports
`SHADPS4_DBG_RDOC_CAPTURE_SCENE`, a scene-gate variant that the `633d212` / `wt-crossfork`
trigger does **not** implement (grep finds the variable only in this script).

---

## 3. Timeline of the successful session

From `C:\Users\Arukas\AppData\Local\Temp\RenderDoc\RenderDoc_2026.11.17_10.42.07.log`, verbatim
lines (timestamps are RenderDoc's, second resolution). PIDs: 24276 = `renderdoccmd` control,
35808 = its `--crash` helper, 44992 = the emulator.

| time | elapsed from launch | line |
|---|---|---|
| 10:42:07 | 0 s | `core.cpp(719) - RenderDoc v1.47 Windows 64-bit Release (b0e1370309f7d7bf986a73830446ca39d9b8c1fd) loaded in replay application` |
| 10:42:07 | 0 s | `settings.cpp(471) - Loading config from C:\Users\Arukas\AppData\Roaming\renderdoc\renderdoc.conf` |
| 10:42:07 | 0 s | `core.cpp(848) - Replay application 'C:\Program Files\RenderDoc\renderdoccmd.exe' launched` |
| 10:42:07 | 0 s | `win32_process.cpp(526) - Running process M:\Development\Deracine-work\_build\bench\bin\rdoc.exe` |
| 10:42:07 | 0 s | `win32_process.cpp(610) - Injecting renderdoc into process 44992` |
| 10:42:07 | 0 s | *(PID 35808)* `core.cpp(852) - Parameter [0]: --crash` |
| 10:42:07 | 0 s | `core.cpp(719) - RenderDoc v1.47 … loaded in capturing application` |
| 10:42:07 | 0 s | `crash_handler.h(156) - Created crash-handling server \\.\pipe\RenderDocBreakpadServer573294938728` |
| 10:42:07 | 0 s | `vk_layer.cpp(110) - Registering Vulkan hooks` |
| 10:42:07 | 0 s | `app_api.cpp(448) - Initialising RenderDoc API version 1.7.0 for requested version 10600` ← emulator's `LoadRenderDoc()` |
| 10:42:09 | +2 s | `app_api.cpp(67) - Using capture file template M:\Development\Deracine-work\lab\pc-vr-quality\user\captures\CUSA11919` ← emulator's `SetOutputDir()` |
| 10:42:09 | +2 s | `vk_device_funcs.cpp(1562) - physical device 0: NVIDIA GeForce RTX 3090 Ti (ver 621.1 patch 0x0) - 10de:2203` |
| 10:42:09 | +2 s | `vk_device_funcs.cpp(5107) - Created capture device from physical device 0` |
| 10:42:09 | +2 s | `vk_core.cpp(5253) - RegisterSurface() window 0000000000382428` |
| 10:42:10 | +3 s | `app_api.cpp(448) - Initialising RenderDoc API version 1.7.0 for requested version 10400` ← **trigger armed here** |
| 10:42:10 | +3 s | `d3d11_device.cpp(163) - New D3D11 device created: nVidia / NVIDIA GeForce RTX 3090 Ti 32.0.16.2101` |
| **10:42:20** | **+13 s** | `vk_core.cpp(2770) - Starting capture` |
| 10:42:20 | +13 s | `resource_manager.h(1452) - Preparing up to 358 potentially dirty resources` |
| 10:42:21 | +14 s | `resource_manager.h(1509) - Prepared 347 dirty resources, postponed 11, skipped 0` |
| 10:42:21 | +14 s | `vk_queue_funcs.cpp(1254) - Persistent map flush forced for ResourceId::224 (0 -> 33554432)` … (5 more flush lines) |
| 10:42:21 | +14 s | `vk_debug.cpp(2724) - Allocating readback window of 33554432 bytes` / `… of 536870912 bytes` |
| 10:42:21 | +14 s | `vk_core.cpp(2923) - Finished capture, Frame 532` |
| 10:42:21 | +14 s | `resource_manager.h(1628) - Serialised 305 resources, skipped 36 unreferenced` |
| 10:42:22 | +15 s | `rdcfile.cpp(1307) - Finishing write to section 1 (renderdoc/internal/framecapture). Compressed from 2769125760 bytes to 65676841 (2.37 %)` |
| 10:42:22 | +15 s | `vk_core.cpp(3389) - Captured Vulkan frame with 2640.844116 MB capture section in 1.877286 seconds` |
| **10:42:22** | **+15 s** | `core.cpp(2231) - Written to disk: M:\Development\Deracine-work\lab\pc-vr-quality\user\captures\CUSA11919_frame532.rdc` |
| 10:42:47 | +40 s | last line of the session log (`vk_image_states.cpp(620)` warning); `probe.txt` gets `'…rdoc.exe' finished executing.` |

Gaps:

* **launch → trigger: 13 s.** Of that, 3 s is process start-up up to the first presented frame
  (the 10400 request is made on the first `FrameStats::EndFrame()`), so the timer ran about
  **10 s**, i.e. `SHADPS4_DBG_RDOC_CAPTURE` was almost certainly `10` (it cannot be read exactly:
  RenderDoc timestamps have 1 s resolution, so the defensible bracket is 9.0–10.9 s).
* **trigger → capture start: under a second.** `TriggerCapture()` takes effect at the next frame
  boundary; RenderDoc logs `Starting capture` and then captures frame 532.
* **capture start → finished: ~1 s**, **→ file on disk: ~2 s** (1.877 s serialisation of a 2.77 GB
  section, compressed to 65.7 MB, 2.37 %).
* **frames per second implied:** 532 frames in ≈10 s ≈ 53 fps.

Warnings/errors in the successful session, all boot-time or benign, none capture-related:
`vk_get_funcs.cpp(937..954)` "Forcibly disabling support for physical device feature …" ×12;
`streamio.cpp(149) - Error - Stream created with invalid file handle` ×3;
`nvapi_hooks.cpp(628) - Warning - NvAPI disabled: Returning NULL for nvapi_QueryInterface(...)`;
`dxgi_common.cpp(1688) - Warning - Querying IDXGIAdapter for unsupported/undocumented interface: IDXGIAdapterInternal2`;
`d3d11_device.cpp(797) - Warning - Trying to get ID3D11On12Device. Not supported at this time.`;
and from 10:42:2x onward a flood of
`vk_image_states.cpp(620) - Warning - baseSlice (0) + sliceCount (32) is greater than the image sliceCount (1)`
(3,900+ lines: the session log is 3,995 lines, of which only 84 are not this warning). No `Error` about the capture, no device loss, no crash.

### 3.1 The second session (10:43:49) — armed but never fired

`RenderDoc_2026.11.17_10.43.49.log` (494,810 B, 3,612 lines) is the "classroom" attempt:

| time | line |
|---|---|
| 10:43:49 | same launch sequence; `Running process …\rdoc.exe`; `Injecting renderdoc into process 42264` |
| 10:43:50 | `app_api.cpp(448) … requested version 10600` (emulator) |
| 10:43:52 | `app_api.cpp(67) - Using capture file template …\lab\pc-vr-quality\user\captures\CUSA11919` |
| 10:43:52 | physical device / capture device / surface registered |
| 10:43:53 | `app_api.cpp(448) … requested version 10400` ← **trigger armed** |
| 10:43:54 → 10:44:27 | nothing but `vk_image_states.cpp(620)` warnings |
| 10:44:27 | last line |

No `Starting capture`. `D:\Temp\rdoc\class-out.txt` and `class-err.txt` (mtime 10:44:28) show the
child ran and then `'…\rdoc.exe' finished executing.` The trigger was armed at 10:43:53 and the
session ended **34 s** later without it firing — so this run's `SHADPS4_DBG_RDOC_CAPTURE` was
**greater than ~34 s** (a value of 45 is consistent with the handoff's advice and with the process
lifetime; it is not proven). This is the cleanest proof of the failure mode "attached, then ended
before the trigger".

### 3.2 The third session (20:23:21) — later, different worktree, different binary

`RenderDoc_2026.11.17_20.23.21.log` (428,628 B, 3,128 lines):

| time | line |
|---|---|
| 20:23:21 | `Running process M:\Development\Deracine-work\wt-autonomous-guardrails\_lab\pc-vr\shadps4.exe` (72,777,216 B, SHA-256 `B6830480CBE33FD5E2DBF374ED213234C7E09792BAF8F9814E7781D890AFD774`, built 19:58) |
| 20:23:22 | `Injecting renderdoc into process 31268` |
| 20:23:23 | `app_api.cpp(448) … requested version 10600` (emulator) |
| 20:23:24 | `Using capture file template M:\Development\Deracine-work\wt-autonomous-guardrails\_lab\pc-vr\user\captures\CUSA11919` |
| 20:23:26 | `app_api.cpp(448) … requested version 10600` ← **second request, also 1.6.0** |
| 20:23:26 → 20:24:11 | only the `vk_image_states` warnings |
| 20:24:11 | last line |

Two things follow. First, this run is **not** the 10:42 binary: both API requests are 10600, i.e. the
scheduler here asks for `eRENDERDOC_API_Version_1_6_0`, which is the *later salvaged* variant (the
current `wt-crossfork` source), not `633d212`'s 1.4.0. Second, the trigger was armed at 20:23:26 and
the session ended 45 s later with no capture — 20:24:11 is exactly `20:23:26 + 45 s`, so a value of
45 s would have fired in the same second the log stops. Whether the process was killed a few
milliseconds before the trigger or just after cannot be resolved from the log. **unknown**.

---

## 4. Why later attempts could end before the trigger

### 4.1 The trigger itself (identical in `633d212` and today)

Current `wt-crossfork\src\src\video_core\renderer_vulkan\vk_scheduler.cpp` lines 34–80, verbatim:

```cpp
namespace {
/// SHADPS4_DBG_RDOC_CAPTURE=<seconds>: with the process started under RenderDoc, asks it for one
/// capture of the first frame presented that long after the first one. A scene can then be captured
/// without a key being pressed, which the frame a benchmark measures needs.
void MaybeTriggerRenderDocCapture() {
    static const double at_seconds = [] {
        const char* value = std::getenv("SHADPS4_DBG_RDOC_CAPTURE");
        return value != nullptr ? std::atof(value) : 0.0;
    }();
    if (at_seconds <= 0.0) {
        return;
    }
    static RENDERDOC_API_1_6_0* api = []() -> RENDERDOC_API_1_6_0* {
        const HMODULE module = GetModuleHandleA("renderdoc.dll");
        if (module == nullptr) {
            LOG_INFO(Render_Vulkan, "RDOC capture was asked for but renderdoc.dll is not loaded");
            return nullptr;
        }
        const auto get_api =
            reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
        if (get_api == nullptr) {
            return nullptr;
        }
        void* out = nullptr;
        if (get_api(eRENDERDOC_API_Version_1_6_0, &out) != 1) {
            return nullptr;
        }
        return static_cast<RENDERDOC_API_1_6_0*>(out);
    }();
    if (api == nullptr) {
        return;
    }
    static const auto started = std::chrono::steady_clock::now();
    static bool fired = false;
    if (fired) {
        return;
    }
    const double since =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (since < at_seconds) {
        return;
    }
    api->TriggerCapture();
    fired = true;
    LOG_INFO(Render_Vulkan, "RDOC capture triggered {:.1f} s after the first frame", since);
}
} // Anonymous namespace
```

`git show 633d212 -- src/src/video_core/renderer_vulkan/vk_scheduler.cpp` adds exactly this
function (plus two `FrameTimeline` marks) at the top of `vk_scheduler.cpp`, with two differences from
today's file:

```diff
+#include "video_core/renderer_vulkan/renderdoc_app.h"
...
+    static RENDERDOC_API_1_4_0* api = []() -> RENDERDOC_API_1_4_0* {
...
+        if (get_api(eRENDERDOC_API_Version_1_4_0, &out) != 1) {
...
+        return static_cast<RENDERDOC_API_1_4_0*>(out);
```

and it is called from the first line of `FrameStats::EndFrame()`:

```cpp
void FrameStats::EndFrame() {
    MaybeTriggerRenderDocCapture();
```

Today's file uses `#include <renderdoc_app.h>`. Everything else — the env parse, the one-shot
`fired` flag, the `TriggerCapture()` call, the log message — is byte-identical.

**When it fires, precisely.**

* Called by: `Vulkan::FrameStats::EndFrame()`, whose only caller is
  `Core::Libraries::VideoOut::VideoOutDriver::Flip()` (`src/src/core/libraries/videoout/driver.cpp:316`,
  HMD path; the window path reaches it further down) — i.e. once per **guest videoout flip**, which
  is the "presented frame".
* The clock is `static const auto started = std::chrono::steady_clock::now();` — initialised on the
  **first call of the function**, which is the first `EndFrame()` after the env variable and the DLL
  are both satisfied. It is **not** process start, not the first Vulkan frame, not the first present
  of a window. In the 10:42 log the corresponding moment is the 10400 API request at 10:42:10.
* The comparison is `since < at_seconds` → return, then `TriggerCapture()`. `TriggerCapture()` is
  RenderDoc's "capture the next frame" request, so the captured frame is the frame *after* the one
  whose `EndFrame` fired the trigger — frame 532 here.
* One-shot: `fired = true` immediately after `TriggerCapture()`, unconditionally. There is no retry,
  no second attempt, and no check that RenderDoc actually started a capture.
* If `renderdoc.dll` is not loaded: the lambda's `GetModuleHandleA` returns `nullptr`, one
  `LOG_INFO(Render_Vulkan, "RDOC capture was asked for but renderdoc.dll is not loaded")` is emitted,
  `api` stays `nullptr` for the life of the process, and every later call returns at
  `if (api == nullptr)`. The clock is never even created. No capture, one log line.
* If the variable is unset or ≤ 0: the function returns before the API probe, so `renderdoc.dll` is
  never looked for and nothing is logged.
* On non-Windows: the function is **not** guarded by `#ifdef _WIN32` (the include of `<windows.h>` is,
  the function is not), and it uses `HMODULE` / `GetModuleHandleA` / `GetProcAddress`. A Linux build
  of this file therefore cannot compile as written. The `_handoff` note that "non-Windows builds
  compile a no-op" is **not supported by the source** — no such branch exists in `633d212` or in the
  current file. (No Linux build was attempted here; the finding is a source reading, not a build
  result.)

### 4.2 Concrete mechanisms that can end a run before the trigger

| # | mechanism | label | evidence |
|---|---|---|---|
| 1 | **Timer starts at the first flip, and the process does not live long enough.** `at_seconds` counts from the first `EndFrame()`, not from launch; a 45 s request on a process that lives ~38–50 s is a coin flip. | **proven** (code) + **proven** for session 2 (armed 10:43:53, dead by 10:44:27, no capture) | §4.1, §3.1 |
| 2 | **Process lifetime is short and bounded by the harness, not by the game.** Session 1 ran 10:42:07→10:42:47 (40 s), session 2 10:43:49→10:44:27 (38 s), session 3 20:23:21→20:24:11 (50 s); `rdoc-run.ps1`-style wrappers `Stop-Process` the emulator after a capture timeout. | **proven** (log first/last timestamps) | log spans |
| 3 | **RenderDoc's own capture keys are dead.** `LoadRenderDoc()` calls `SetCaptureKeys(nullptr, 0)` whenever the API binds, so F12 does not reach RenderDoc's default present-to-present capture. Only the emulator's own hotkey path (`hotkey_capture_frame` = `f12` → `SDL_EVENT_RDOC_CAPTURE` → `VideoCore::TriggerCapture()` → `StartFrameCapture` at the next submission) or the timer can capture. | **proven** (code) | `renderdoc.cpp:86-93`, `input_handler.cpp:170`, `sdl_window.cpp:443-450`, `liverpool.cpp:130,173` |
| 4 | **The one-shot flag can be spent while RenderDoc cannot honour it.** `fired = true` is set regardless of outcome; if `TriggerCapture()` lands while RenderDoc is busy or the frame loop is already tearing down, the only request of the session is lost. | **proven** that the flag is unconditional; **plausible** that it was hit | code; no `Starting capture` in sessions 2 and 3 |
| 5 | **Capture is expensive and can push the device over.** 2.77 GB of section, 1.877 s of serialisation, plus readback windows of 512 MB / 64 MB / 32 MB allocated at capture time on a 23.7 GB device-local heap. | **proven** (numbers) | `vk_core.cpp(3389)`, `vk_debug.cpp(2724)`, `vk_memory.cpp(179)` |
| 6 | **The app passes an invalid image subresource range and RenderDoc tracks it anyway.** `baseSlice (0) + sliceCount (32) is greater than the image sliceCount (1)` floods the log (≈3,900 of 3,995 lines in session 1, ≈3,550 of 3,612 in session 2). A slice count larger than the image is an application bug; under capture its consequences (corruption, hang, device loss) become more likely. | **plausible** (warning is proven, the consequence is not) | `vk_image_states.cpp(620)` in all three logs |
| 7 | **Core validation is on.** `lab\pc-vr-quality\user\config.json` has `"vkvalidation_core_enabled": true` (`vkvalidation_enabled` is `false`). Validation inside a captured frame adds CPU work and can turn a marginal timeout into a device loss. | value **proven**, effect **plausible** | config.json:117 |
| 8 | **No crash evidence survives.** `LoadRenderDoc()` also calls `UnloadCrashHandler()`, so the injected process's crash reporting is removed after the app binds the API; `%LOCALAPPDATA%\Temp\RenderDoc\dumps\` exists (created 10:39:06) and is **empty**. A crash in `AddressSpace::Protect` therefore leaves no RenderDoc minidump. | **proven** (code, empty dumps dir); the claimed `Protect` crash itself is **unknown** | `renderdoc.cpp:92`, dumps dir listing |
| 9 | **Wrong binary / trigger absent.** A build without the `633d212` hunk never requests 1.4.0 and never captures, while still showing "attached" in RenderDoc's log. | **proven** mechanism (code); not the case for sessions 1–2, and session 3 had a 1.6.0 trigger | API-version signature in each log |
| 10 | **`renderdoc_enabled` is irrelevant when injected** (`false` in both configs today) but decisive when *not* injected: with no injection and the setting off, `rdoc_api` stays null, `SetCaptureKeys` is never called, and the trigger's DLL probe fails. | **proven** (code) | §1.2 |

Three most likely, in order: **#1/#2** (the process dies before a long timer expires — the only
failure actually demonstrated), then **#3** (someone expecting F12 to work under an injected
session, where the emulator has deliberately removed that key), then **#5/#6** (capture-time
resource pressure / image-range bug turning a successful start into a device loss).

---

## 5. What is different today

1. **The tree and the binary.** The successful binary was built from the working tree that became
   `633d212` and is still on disk (`rdoc.exe`, `F061B062…`). Today's checkouts are *rebuilt* from
   later commits; `_build\bench\bin` holds 31 executables but only `rdoc.exe` is the 10:42 one.
   Source-wise nothing is missing: `633d212` is an ancestor of `de9c78c`
   (`integration/r58-presentsize`), and the current `wt-crossfork` HEAD `e676a43`
   (`investigation/cross-fork-renderer`) still contains `MaybeTriggerRenderDocCapture()` and the
   `FrameStats::EndFrame()` call.
2. **API version drift 1.4.0 → 1.6.0.** `633d212` asked for `eRENDERDOC_API_Version_1_4_0`; the
   current tree asks for `eRENDERDOC_API_Version_1_6_0` and stores a `RENDERDOC_API_1_6_0*`. With
   RenderDoc 1.47 (which serves up to 1.7.0) both work, but the log signature changes from
   `requested version 10400` to `requested version 10600` — which is exactly how the 20:23 session
   proves it ran a different build.
3. **Include path.** `633d212`: `#include "video_core/renderer_vulkan/renderdoc_app.h"` (the header
   was tracked by follow-up commits titled "build: track renderdoc_app.h needed by the 633d212
   diagnostics build"). Current: `#include <renderdoc_app.h>`.
4. **`renderdoc_enabled` is off in both configs, verbatim:**
   * `M:\Development\Deracine-work\lab\pc-vr-quality\user\config.json:113` → `    "renderdoc_enabled": false,`
   * `M:\Development\Deracine-work\DeracinePCVR\pc-vr\user\config.json:113` → `    "renderdoc_enabled": false,`

   This is **not** a blocker for an injected run (§1.2), and it was equally false at 10:42. It matters
   only for the non-injected "offline mode" route.
5. **Working directory.** The successful run's user dir was the shared lab
   (`lab\pc-vr-quality\user`). The current runner defaults to a private lab
   (`_build\crossfork\rdoc-lab\user`) which **does not exist yet** — the first run would copy
   `lab\pc-vr-quality\user` there, and therefore inherit its `config.json` (`renderdoc_enabled=false`,
   `vkvalidation_core_enabled=true`) and its `patches\CUSA11919.xml`.
6. **The game dump path moved for the newest lab.** `lab\pc-vr-quality\settings.txt` still points at
   `game=D:\Temp\Deracine\CUSA11919-qual\eboot.bin` (the 10:42 dump), while
   `wt-autonomous-guardrails\_lab\pc-vr\settings.txt` points at
   `D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17\eboot.bin`.
7. **Missing variables.** Nothing in the current tree defaults `SHADPS4_DBG_RDOC_CAPTURE`; it is off
   unless exported (`at_seconds <= 0.0` → immediate return). The 10:42 environment as a whole was
   never recorded — only the render half survives in `_build\bench\runs\alias-trace-1\meta.json` and
   `_build\crossfork\rdoc-run.ps1`.
8. **`SHADPS4_DBG_RDOC_CAPTURE_SCENE` exists only in the runner**, not in the emulator: the
   scene-gated variant the assignment asks for (`DERACINE_CODEX_FIRST_ASSIGNMENT.md:111`) is not
   implemented in `wt-crossfork`'s trigger.
9. **The trigger's non-Windows story is unchanged and still broken** (no `#ifdef _WIN32` guard), see
   §4.1.
10. **The `.rdc` itself is duplicated.** Two byte-identical copies exist (the lab and the
    guardrails `_lab`). Any cleanup that removes one must not be mistaken for "the capture was
    regenerated"; both carry the 10:42:22 mtime.

---

## 6. What cannot be determined from files alone

1. **The exact value of `SHADPS4_DBG_RDOC_CAPTURE` in the successful run.** Bounded to ≈9.0–10.9 s
   by the log; `10` is the natural reading, not a recorded fact.
2. **Whether the timer or a manual F12 fired the 10:42 capture.** The variable was certainly set
   (10400 request), and RenderDoc's log records `Starting capture` identically for
   `RENDERDOC_TriggerCapture` and for the emulator's `StartFrameCapture`. Only the emulator's
   `shad_log.txt` line `RDOC capture triggered {:.1f} s after the first frame` would settle it, and
   that log was overwritten (`lab\pc-vr-quality\user\log\shad_log.txt` now ends 20:34; the two
   surviving logs there contain no RDOC line at all, only the config echo
   `Vulkan rdocEnable: false`).
3. **Whether the working directory was passed as `-d` or merely inherited** by `renderdoccmd`.
4. **Why the emulator process ended at 40 s / 38 s / 50 s** in the three sessions. Normal exit and
   `Stop-Process` from a wrapper are indistinguishable: both leave `'…rdoc.exe' finished executing.`
   in `renderdoccmd`'s stdout. No emulator log and no RenderDoc crash dump survives (§4.2 #8).
5. **Whether the `AddressSpace::Protect` crash attributed to the failed attempt actually happened.**
   Nothing in the three RenderDoc logs reports a device loss or a crash; the dumps directory is
   empty; the claim comes from the earlier agent's audit, not from an artefact.
6. **The exact environment of the 10:42 run** — in particular whether `SHADPS4_DBG_AUTOPRESS` or a
   `-p` patch was used (argv proves no `-p`; the env is unrecoverable).
7. **How the 5760×3240 targets are derived** from `TITLE_EYE_WIDTH=2880` / `TITLE_RESOLUTION=6` /
   `VR_SCALE=0.5`. The recorded set is the one that produced them; the arithmetic is not established
   here (the recovery record's `2880x1632` / `2880x3240` naming does not close on 3240 by doubling).
8. **Whether a fresh run would capture the same frame 532.** Frame number depends on how many flips
   happen in the timer window, on the headset being present (`req.is_hmd` path in `Flip`), on the
   OpenXR wait and on the frame cap; none of that is deterministic across sessions.
9. **Whether the capture would be classroom content.** It would not, by default: frame 532 is the
   "Sony Interactive Entertainment presents" splash, and the classroom needs pad automation
   (`SHADPS4_DBG_AUTOPRESS`) and a scene-synchronised gate, neither of which the current trigger
   implements.
10. **Whether RenderDoc 1.47 + driver 621.01 is still the installed state** at the time of any rerun
    (measured here: `renderdoc.dll` 1.47.0.0, dated 2026-10-31; GPU `NVIDIA GeForce RTX 3090 Ti`,
    driver `621.1` per the log, `621.01` per the bench meta).

---

## Appendix A — `C:\Users\Arukas\AppData\Roaming\renderdoc\renderdoc.conf`, verbatim

17,491 bytes, 525 lines. It is the **complete default settings dump** (every option at its default),
not a small hand-written file: there is no `Core` section, hence no capture path, no capture file
template, no capture keys and no "capture on launch" setting stored here. The only capture-related
entries are `Capture.IncludeExtendedThumbnail=false` and `Capture.Debug.SnapshotDiagnosticLog=false`;
everything that shaped the 10:42 session came from the emulator's runtime API calls, not from this
file.

```xml
<?xml version="1.0"?>
<config version="1">
  <AMD>
    <ISA>
      <!-- AMD.ISA.ShowLegacyASICs

Show legacy ASICs for AMD shader disassembly targets. Note that depending on the environment if driver support is required, these may not be available.

Default value: 'False' -->
      <ShowLegacyASICs type="Boolean">false</ShowLegacyASICs>
    </ISA>
    <RGP>
      <!-- AMD.RGP.Enable

Enable integration with AMD's RGP tool.

Default value: 'False' -->
      <Enable type="Boolean">false</Enable>
    </RGP>
    <ags>
      <!-- AMD.ags.AllowUnknownExtensions

Allow extensions that we may not support. This could crash or cause crashes on replay.

Default value: 'False' -->
      <AllowUnknownExtensions type="Boolean">false</AllowUnknownExtensions>
    </ags>
  </AMD>
  <Android>
    <Debug>
      <!-- Android.Debug.ProcessLaunch

Output verbose debug logging messages when launching android apps.

Default value: 'False' -->
      <ProcessLaunch type="Boolean">false</ProcessLaunch>
    </Debug>
    <!-- Android.MaxConnectTimeout

Maximum time in seconds to try connecting to the target app before giving up. Useful primarily for apps that take a very long time to start up.

Default value: '30' -->
    <MaxConnectTimeout type="UnsignedInteger">30</MaxConnectTimeout>
    <!-- Android.SDKDirPath

The location of the root of the Android SDK. This path should contain folders such as build-tools and platform-tools.

Default value: '' -->
    <SDKDirPath type="String"></SDKDirPath>
    <!-- Android.JDKDirPath

The location of the root of the Java JDK. This path should contain folders such as bin and lib.

Default value: '' -->
    <JDKDirPath type="String"></JDKDirPath>
    <!-- Android.AllowAllEGLExtensions

Normally certain extensions are removed from the EGL extension string for compatibility, but with this option that behaviour can be overridden and all extensions will be reported.

Default value: 'False' -->
    <AllowAllEGLExtensions type="Boolean">false</AllowAllEGLExtensions>
  </Android>
  <Capture>
    <Debug>
      <!-- Capture.Debug.SnapshotDiagnosticLog

Snapshot the diagnostic log at capture time and embed in the capture.

Default value: 'False'
DEBUG VARIABLE: Read-only in stable builds. -->
      <SnapshotDiagnosticLog type="Boolean">false</SnapshotDiagnosticLog>
    </Debug>
    <!-- Capture.IncludeExtendedThumbnail

Save the thumbnail unresized and losslessly encoded during capture.

Default value: 'False' -->
    <IncludeExtendedThumbnail type="Boolean">false</IncludeExtendedThumbnail>
  </Capture>
  <D3D11>
    <!-- D3D11.HardwareCounters

Enable support for IHV-specific hardware counters on D3D11.

Default value: 'True' -->
    <HardwareCounters type="Boolean">true</HardwareCounters>
  </D3D11>
  <D3D12>
    <DXILShaderDebugger>
      <!-- D3D12.DXILShaderDebugger.Logging

Debug logging for the DXIL shader debugger

Default value: 'False' -->
      <Logging type="Boolean">false</Logging>
      <!-- D3D12.DXILShaderDebugger.EnableMT

Use multiple threads to run the shader debugger simulation.

Default value: 'True' -->
      <EnableMT type="Boolean">true</EnableMT>
    </DXILShaderDebugger>
    <Debug>
      <!-- D3D12.Debug.EnableGPUVA

Enable GPU Validation when enabling D3D12 validation.

Default value: 'False' -->
      <EnableGPUVA type="Boolean">false</EnableGPUVA>
      <!-- D3D12.Debug.EnableDRED

Enable DRED when enabling D3D12 validation.

Default value: 'False' -->
      <EnableDRED type="Boolean">false</EnableDRED>
      <RT>
        <!-- D3D12.Debug.RT.Overlay

Add some RT tracking to the overlay.

Default value: 'False'
DEBUG VARIABLE: Read-only in stable builds. -->
        <Overlay type="Boolean">false</Overlay>
        <!-- D3D12.Debug.RT.IndirectEstimateOverride

Override how many bytes are reserved for shader tables in each indirect ray dispatch (will be multiplied by MaxCommandCount)

Default value: '0' -->
        <IndirectEstimateOverride type="UnsignedInteger">0</IndirectEstimateOverride>
        <!-- D3D12.Debug.RT.MaxVertexIncrement

Amount to add to the API-provided max vertex when building a BLAS with an index buffer, to account for incorrectly set values by application.

Default value: '1000' -->
        <MaxVertexIncrement type="UnsignedInteger">1000</MaxVertexIncrement>
        <!-- D3D12.Debug.RT.MaxVertexPercentIncrease

Percentage increase for the API-provided max vertex when building a BLAS with an index buffer, to account for incorrectly set values by application.

Default value: '10' -->
        <MaxVertexPercentIncrease type="UnsignedInteger">10</MaxVertexPercentIncrease>
        <!-- D3D12.Debug.RT.ASCacheThreshold

How many milliseconds to wait before caching an AS to disk if it has been unmodified for that long

Default value: '5000' -->
        <ASCacheThreshold type="UnsignedInteger">5000</ASCacheThreshold>
        <!-- D3D12.Debug.RT.Auditing

Audit RT work during capture and replay.

Default value: 'False' -->
        <Auditing type="Boolean">false</Auditing>
        <!-- D3D12.Debug.RT.ASCacheBatchSize

The maximum number of ASs to cache to disk in a single batch (batch processing happens at indeterminate intervals but no more than once per submission

Default value: '50' -->
        <ASCacheBatchSize type="UnsignedInteger">50</ASCacheBatchSize>
      </RT>
      <!-- D3D12.Debug.SingleSubmitFlushing

Every command buffer is submitted and fully flushed to the GPU, to narrow down the source of problems.

Default value: 'False'
DEBUG VARIABLE: Read-only in stable builds. -->
      <SingleSubmitFlushing type="Boolean">false</SingleSubmitFlushing>
      <!-- D3D12.Debug.DriverASSerialisation

Use driver-side serialisation for saving and restoring ASs

Default value: 'False' -->
      <DriverASSerialisation type="Boolean">false</DriverASSerialisation>
      <!-- D3D12.Debug.OverlayDumpDirPath

Path to dump quad overdraw patched DXIL files.

Default value: '' -->
      <OverlayDumpDirPath type="String"></OverlayDumpDirPath>
      <!-- D3D12.Debug.PostVSDumpDirPath

Path to dump post mesh shader patched DXIL files.

Default value: '' -->
      <PostVSDumpDirPath type="String"></PostVSDumpDirPath>
      <!-- D3D12.Debug.IgnoreSignatureCheck

Whether to ignore digital signature check for dll's embedded in capture file

Default value: 'False' -->
      <IgnoreSignatureCheck type="Boolean">false</IgnoreSignatureCheck>
      <!-- D3D12.Debug.FeedbackDumpDirPath

Path to dump bindless feedback annotation generated DXBC/DXIL files.

Default value: '' -->
      <FeedbackDumpDirPath type="String"></FeedbackDumpDirPath>
    </Debug>
    <!-- D3D12.HardwareCounters

Enable support for IHV-specific hardware counters on D3D12.

Default value: 'True' -->
    <HardwareCounters type="Boolean">true</HardwareCounters>
    <!-- D3D12.AttemptExperimentalGPUUploadEnable

Try to enable 'experimental' support for GPU upload heaps on D3D12.

Default value: 'True' -->
    <AttemptExperimentalGPUUploadEnable type="Boolean">true</AttemptExperimentalGPUUploadEnable>
    <!-- D3D12.D3D12CoreDirPath

The location of the D3D12Core library. This path should be the directory that contains the D3D12Core.dll that you want to use.

Default value: '' -->
    <D3D12CoreDirPath type="String"></D3D12CoreDirPath>
    <!-- D3D12.DXCPath

The location of the dxcompiler.dll library to use.

Default value: '' -->
    <DXCPath type="String"></DXCPath>
    <!-- D3D12.BindlessFeedback

Enable fetching from GPU which descriptors were dynamically used in descriptor arrays.

Default value: 'True' -->
    <BindlessFeedback type="Boolean">true</BindlessFeedback>
  </D3D12>
  <DXBC>
    <Debug>
      <!-- DXBC.Debug.SearchDirPaths

Paths to search for separated shader debug PDBs, including all types of paths.

Default value: '[]' -->
      <SearchDirPaths type="Array" elemtype=""></SearchDirPaths>
    </Debug>
    <Disassembly>
      <!-- DXBC.Disassembly.FriendlyNaming

Where possible (i.e. it is completely unambiguous) replace register names with high-level variable names.

Default value: 'True' -->
      <FriendlyNaming type="Boolean">true</FriendlyNaming>
      <!-- DXBC.Disassembly.ProcessVendorShaderExts

Process vendor shader extensions from magic UAV encoded instructions into the real operations.
If this is disabled, shader debugging won't produce correct results.

Default value: 'True' -->
      <ProcessVendorShaderExts type="Boolean">true</ProcessVendorShaderExts>
    </Disassembly>
  </DXBC>
  <NV>
    <!-- NV.BlockNVAPI

Completely block nvapi from activating, pretend no NV GPU is present

Default value: 'False' -->
    <BlockNVAPI type="Boolean">false</BlockNVAPI>
  </NV>
  <OpenGL>
    <Debug>
      <!-- OpenGL.Debug.ShaderDebugDumpDirPath

Path to dump shader debugging generated SPIR-V files.

Default value: '' -->
      <ShaderDebugDumpDirPath type="String"></ShaderDebugDumpDirPath>
      <!-- OpenGL.Debug.ShaderDebugLogging

Output verbose debug logging messages when debugging shaders.

Default value: 'False' -->
      <ShaderDebugLogging type="Boolean">false</ShaderDebugLogging>
    </Debug>
    <!-- OpenGL.HardwareCounters

Enable support for IHV-specific hardware counters on OpenGL.

Default value: 'True' -->
    <HardwareCounters type="Boolean">true</HardwareCounters>
  </OpenGL>
  <RemoteServer>
    <!-- RemoteServer.TimeoutMS

Timeout in milliseconds for remote server operations.

Default value: '5000' -->
    <TimeoutMS type="UnsignedInteger">5000</TimeoutMS>
    <!-- RemoteServer.DebugLogging

Output a verbose logging file in the system's temporary folder containing the traffic to and from the remote server.

Default value: 'False' -->
    <DebugLogging type="Boolean">false</DebugLogging>
  </RemoteServer>
  <Replay>
    <Debug>
      <!-- Replay.Debug.PrintChunkTimings

Print stats of chunk processing times

Default value: 'False' -->
      <PrintChunkTimings type="Boolean">false</PrintChunkTimings>
      <!-- Replay.Debug.SingleThreadedCompilation

Compile all shaders and PSOs single-threaded.

Default value: 'False' -->
      <SingleThreadedCompilation type="Boolean">false</SingleThreadedCompilation>
      <!-- Replay.Debug.EnableAftermath

Enable nvidia Aftermath for diagnosing GPU crashes or failures on D3D12 and Vulkan.

Default value: 'False' -->
      <EnableAftermath type="Boolean">false</EnableAftermath>
      <!-- Replay.Debug.EnableNVRTValidation

Enable nvidia Raytracing validation on D3D12 and Vulkan.

Default value: 'False' -->
      <EnableNVRTValidation type="Boolean">false</EnableNVRTValidation>
    </Debug>
    <Shader>
      <!-- Replay.Shader.LimitedSearchDirPaths

Companion array to DXBC.Debug.SearchDirPaths - listing paths which should not be searched exhaustively but only used for simple lookups.

Default value: '[]' -->
      <LimitedSearchDirPaths type="Array" elemtype=""></LimitedSearchDirPaths>
    </Shader>
  </Replay>
  <Shader>
    <Debug>
      <ControlFlow>
        <!-- Shader.Debug.ControlFlow.Logging

Debug logging for shader debugger controlflow

Default value: 'False' -->
        <Logging type="Boolean">false</Logging>
      </ControlFlow>
      <!-- Shader.Debug.SPIRVUseDebugColumnInformation

Control whether column information should be read from vulkan debug info.

Default value: 'False' -->
      <SPIRVUseDebugColumnInformation type="Boolean">false</SPIRVUseDebugColumnInformation>
      <!-- Shader.Debug.UseJobSystemJobs

Use individual job system jobs to run shader debugging simulation.

Default value: 'False'
DEBUG VARIABLE: Read-only in stable builds. -->
      <UseJobSystemJobs type="Boolean">false</UseJobSystemJobs>
    </Debug>
  </Shader>
  <Vulkan>
    <Debug>
      <!-- Vulkan.Debug.FeedbackDumpDirPath

Path to dump bindless feedback annotation generated SPIR-V files.

Default value: '' -->
      <FeedbackDumpDirPath type="String"></FeedbackDumpDirPath>
      <!-- Vulkan.Debug.PrintfBufferSize

How many bytes to reserve for a printf output buffer.

Default value: '65536' -->
      <PrintfBufferSize type="UnsignedInteger">65536</PrintfBufferSize>
      <!-- Vulkan.Debug.PostVSDumpDirPath

Path to dump gnerated SPIR-V compute shaders for fetching post-vs.

Default value: '' -->
      <PostVSDumpDirPath type="String"></PostVSDumpDirPath>
      <!-- Vulkan.Debug.PSDebugDumpDirPath

Path to dump shader debugging generated SPIR-V files.

Default value: '' -->
      <PSDebugDumpDirPath type="String"></PSDebugDumpDirPath>
      <RT>
        <!-- Vulkan.Debug.RT.MaxVertexIncrement

Amount to add to the API-provided max vertex when building a BLAS with an index buffer, to account for incorrectly set values by application.

Default value: '1000' -->
        <MaxVertexIncrement type="UnsignedInteger">1000</MaxVertexIncrement>
        <!-- Vulkan.Debug.RT.MaxVertexPercentIncrease

Percentage increase for the API-provided max vertex when building a BLAS with an index buffer, to account for incorrectly set values by application.

Default value: '10' -->
        <MaxVertexPercentIncrease type="UnsignedInteger">10</MaxVertexPercentIncrease>
      </RT>
      <!-- Vulkan.Debug.ShaderDebugLogging

Output verbose debug logging messages when debugging shaders.

Default value: 'False' -->
      <ShaderDebugLogging type="Boolean">false</ShaderDebugLogging>
      <!-- Vulkan.Debug.EnableShaderDebugMT

Use multiple threads to run the shader debugger simulation.

Default value: 'True' -->
      <EnableShaderDebugMT type="Boolean">true</EnableShaderDebugMT>
      <!-- Vulkan.Debug.MemoryAllocationLogging

Output verbose debug logging messages when allocating internal memory.

Default value: 'False' -->
      <MemoryAllocationLogging type="Boolean">false</MemoryAllocationLogging>
      <!-- Vulkan.Debug.SingleSubmitFlushing

Every command buffer is submitted and fully flushed to the GPU, to narrow down the source of problems.

Default value: 'False'
DEBUG VARIABLE: Read-only in stable builds. -->
      <SingleSubmitFlushing type="Boolean">false</SingleSubmitFlushing>
      <!-- Vulkan.Debug.DisableBufferDeviceAddress

Disable use of buffer device address for PS Input fetch.

Default value: 'False' -->
      <DisableBufferDeviceAddress type="Boolean">false</DisableBufferDeviceAddress>
      <!-- Vulkan.Debug.VerboseCommandRecording

Add verbose logging around recording and submission of command buffers in vulkan.

Default value: 'False'
DEBUG VARIABLE: Read-only in stable builds. -->
      <VerboseCommandRecording type="Boolean">false</VerboseCommandRecording>
      <!-- Vulkan.Debug.AllowDescriptorSetReuse

Allow the re-use of descriptor sets via vkResetDescriptorPool.

Default value: 'True'
DEBUG VARIABLE: Read-only in stable builds. -->
      <AllowDescriptorSetReuse type="Boolean">true</AllowDescriptorSetReuse>
      <!-- Vulkan.Debug.DangerousDescriptorSerialisation

DANGEROUS, MAY CAUSE CAPTURE FAILURES: Disable serialising plain untyped buffer descriptors.

Default value: '0' -->
      <DangerousDescriptorSerialisation type="UnsignedInteger">0</DangerousDescriptorSerialisation>
      <!-- Vulkan.Debug.UseFastDescriptorLookup

Use fast pattern-matching lookup to try to identify descriptors before falling back to trie lookup.

Default value: 'True' -->
      <UseFastDescriptorLookup type="Boolean">true</UseFastDescriptorLookup>
      <!-- Vulkan.Debug.ReplaceAppInfo

By default we have no choice but to replace VkApplicationInfo to safely work on all drivers. This behaviour can be disabled with this flag, which lets it through both during capture and on replay.

Default value: 'True' -->
      <ReplaceAppInfo type="Boolean">true</ReplaceAppInfo>
      <!-- Vulkan.Debug.EnableGPUVA

Enable GPU Validation when enabling Vulkan validation.

Default value: 'False' -->
      <EnableGPUVA type="Boolean">false</EnableGPUVA>
      <!-- Vulkan.Debug.UsePipelineCacheForReplay

Use application-provided pipeline cache when compiling shaders on replay

Default value: 'True' -->
      <UsePipelineCacheForReplay type="Boolean">true</UsePipelineCacheForReplay>
    </Debug>
    <!-- Vulkan.BindlessFeedback

Enable fetching from GPU which descriptors were dynamically used in descriptor arrays.

Default value: 'True' -->
    <BindlessFeedback type="Boolean">true</BindlessFeedback>
    <Hack>
      <!-- Vulkan.Hack.DisableRPNormalisation

Disable default behaviour to normalise renderpasses to be more consistent and debuggable.

Default value: 'False' -->
      <DisableRPNormalisation type="Boolean">false</DisableRPNormalisation>
    </Hack>
    <!-- Vulkan.PrintfFetch

Enable fetching printf messages from GPU.

Default value: 'True' -->
    <PrintfFetch type="Boolean">true</PrintfFetch>
    <!-- Vulkan.HardwareCounters

Enable support for IHV-specific hardware counters on Vulkan.

Default value: 'True' -->
    <HardwareCounters type="Boolean">true</HardwareCounters>
    <!-- Vulkan.GPUReadbackDeviceLocal

When reading back mapped device-local memory, use a GPU copy instead of a CPU side comparison directly to mapped memory.

Default value: 'True' -->
    <GPUReadbackDeviceLocal type="Boolean">true</GPUReadbackDeviceLocal>
  </Vulkan>
  <Win32>
    <Callstacks>
      <!-- Win32.Callstacks.IgnoreList

Modules which are ignored when resolving callstacks on Windows.

Default value: '[]' -->
      <IgnoreList type="Array" elemtype=""></IgnoreList>
      <!-- Win32.Callstacks.MSDIAPath

The path to the msdia dll.

Default value: ':uninit:var:' -->
      <MSDIAPath type="String">:uninit:var:</MSDIAPath>
    </Callstacks>
  </Win32>
</config>
```

---

## Addendum from the lead (same branch, `integration/cross-fork-renderer`)

Three corrections/updates to the archaeology above, so the next session starts from the current
state rather than from the state at the time of writing:

1. **The scene gate now exists.** `SHADPS4_DBG_RDOC_CAPTURE_SCENE=<draws>,<mpx>` is implemented in
   this branch (`3d375a4`) in `vk_scheduler.cpp`: the frame-stats window arms the same one-shot
   `TriggerCapture()` once a window reports at least that many draws and that much target area per
   frame. `PeekWindow()` reads the counters without consuming them, so the gate sees exactly what
   the log line reports. It has not been exercised live yet.
2. **Injection still works on this machine today.** `RenderDoc_2026.11.17_20.23.21.log` is a second,
   independent successful injection - RenderDoc v1.47 into
   `wt-autonomous-guardrails\_lab\pc-vr\shadps4.exe` - so the "RenderDoc cannot attach here"
   classification is not merely unproven, it is contradicted by a session later the same day.
3. **No capture run has been made in this phase.** The headset is in use, so the control run
   (old procedure, and then the scene-gated classroom capture) is deferred. Both commands are ready
   in `_build\crossfork\rdoc-run.ps1` and the private portable directory keeps captures out of the
   shared lab; the existing `CUSA11919_frame532.rdc` is preserved in three places and was not read
   destructively.

The classification this document supports, and which the deferral does not change: the one
*demonstrated* cause of a failed attempt is a trigger delay longer than the process lifetime
(item 11), i.e. procedure, not capability. The remaining candidates (capture cost, validation
layers, the `sliceCount` log flood) are labelled plausible and are testable by the control run.
