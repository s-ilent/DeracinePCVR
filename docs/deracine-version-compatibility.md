# Déraciné (CUSA11919) version compatibility

This note records what the emulator actually does with the Déraciné versions that exist locally.
It replaces the inherited assumption that the game is only usable at versions 01.00 and 01.04.

## Short version

The declared `APP_VER` of Déraciné is metadata. It selects optional executable patches; it is
not a gate on loading the game. Nothing in the emulator refuses CUSA11919 at 01.00 or 01.01.

The launcher no longer warns that a version is unknown, and no longer repeats the two symptoms
("slow motion", "console resolution") that belong to Astro Bot's unknown-build path. It prints
the detected title and version, says that the executable is loaded, and reports the state of
each optional patch in `user/patches/<TITLE_ID>.xml`.

## Installed copies (inspected 2026-11-18)

| | 1.00 | 1.01 (installed) |
|---|---|---|
| Path | `D:\Temp\Deracine\1.0\Image0` | `D:\Temp\Deracine\CUSA11919-1.01` |
| `param.sfo` `APP_VER` | `01.00` | `01.01` |
| `param.sfo` `VERSION` | `01.00` | `01.00` |
| `eboot.bin` size | 81,506,767 | 81,506,767 |
| `eboot.bin` SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` | `8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` |

`D:\Temp\Deracine\Update\Image0\eboot.bin` is byte-identical to the installed 1.01 eboot.
There are no modded executables in the installed folders. The `engine` and `gameproject`
entries in the 1.01 folder are symlinks into the 1.00 image; `ue4commandline.txt` and
`title.json` are normal game files, not version spoofing.

### What differs between the two executables

Both files are SCE SELF containers with plaintext segments. Whole-file comparison finds 362
different bytes, none of them code:

- 32 bytes in the SELF control block at `0x430`;
- 2 bytes in the mapped RW process-parameter block at `0x43A41F1`: the SDK stamp
  (`0x05508001` versus `0x05050001`, guest address `0x4384011`);
- 328 bytes in the never-loaded `PT_SCE_LIBVERSION` trailer at `0x4DBA1A7..0x4DBB1CD`.

The executable `PT_LOAD` segment (65,563,952 bytes, SHA-256 `4112d957…`), the RELRO segment and
the dynlibdata segment are byte-identical; the only mapped difference is the 2-byte SDK stamp
above. **1.01 is a re-sign/metadata update, not a code change.** Therefore any claim that 1.01
has different code, different audio entry points, or a different SNDX implementation is
unsupported.

## The SNDX "Bypass Compressor Crash" patch

`pc-vr/user/patches/CUSA11919.xml` (and its `community` copy) contains one optional patch:

- `AppVer="01.00"`, a fixed `bytes` line at `00c465b0`, value `31c0c39090`, enabled.
- The address convention subtracts `0x400000`, so the write lands at guest
  `0x8008465B0`: ELF vaddr `0x8465B0`, SELF file offset `0x8662E0`.
- The original function entry there is `55 48 89 e5 41 56 53 48 89 fb …` (both versions).
  It is an adapter that tail-jumps to `0x893BF0`; that routine reads the NGS2 effect's
  detector (sidechain) pointer bank at `+0x40` and faults at guest `0x894060` when the bank is
  short. The five patch bytes replace the prologue with `xor eax,eax; ret; nop; nop`, cutting
  the only path to the effect. It is a bypass, not a repair.
- Commit `acda49e` (`fix(ngs2): give the user effect its detector pointers`) fixes the cause in
  the emulator by mirroring the channel pointers into the detector bank. With that commit, the
  effect runs normally and the patch is redundant on **both** versions. Without that commit,
  the identical code means 1.01 would fault at the same place, so the workaround would still be
  needed there — the `AppVer="01.00"` gate would be the only reason it did not run.

On the installed 1.01 the legacy patch loader compares `AppVer` with `APP_VER` exactly, so the
fixed-address line is skipped. It used to be skipped silently; the loader now logs the skip and
the launcher reports it before launch. No bytes are written.

The patch file now carries `Expected="554889e54156534889fb"`. The patch manager verifies those
bytes before any fixed-address write and refuses the patch, leaving the executable unchanged, if
the build at that address is not the verified one. Do not change `AppVer` to `01.01` to force the
patch: it is not needed on current builds, and forcing it would bypass the NGS2 effect again.

## Why the old warning existed and why it was wrong

`pc-vr/launch.ps1` had this branch (root commit `e7d5bcf`, 2026-10-20, "Déraciné PC VR port
based on AstroQuest"):

```powershell
} elseif (@("01.00", "01.04") -notcontains $info["APP_VER"]) {
    Say ("... DeracinePCVR knows versions 01.00 and 01.04 from inside: another plays in slow
          motion where frames take long, and at the console's resolution.") "Yellow"
}
```

- The `01.00`/`01.04` list is Astro Bot Rescue Mission's: `known_title_builds.h` names
  CUSA12392 and those two builds, and `known_title.cpp` limits every fix to CUSA12392/CUSA12431.
  Déraciné never enters that code.
- The branch only called `Say` (`Write-Host`); it never exited.
- The "slow motion / console resolution" text is the unknown-build fallback for Astro Bot, not a
  behaviour of Déraciné. Déraciné's scene size comes from `render_percent` and its engine's
  `Engine.ini`; `SHADPS4_TITLE_RESOLUTION`, `SHADPS4_TITLE_TIMESTEP`, the frame-pace logic and
  the known-build size tables are all inert for it.

No other version gate exists: `src/src` compares `APP_VER` only when selecting patches. PSVR
support is read from `param.sfo` attributes, not from the version.

## Adding a version-specific patch safely

1. Verify the function in the actual build: extract the program segment (SELF segment table,
   not ELF `p_offset`) and disassemble the exact address. Remember the `0x400000` convention in
   XML addresses.
2. Put the original bytes in `Expected`, in memory order. The write is refused on any mismatch.
3. Gate with `AppVer` only when the patch really is version-specific; otherwise prefer a unique
   `mask` signature.
4. Add a unit test with a matching and a mismatching image, and run
   `shadps4_memory_patcher_test`.

## Validation status

- Patch-manager tests: 23/23 pass in the isolated worktree (`fix/deracine-version-compat`).
- Launcher tests: 134 pass, including six new version/patch-report checks. The 15 failures are
  identical on the pinned baseline (menu/instruction fixtures), not caused by this work.
- Launcher smoke test with the real 1.01 `param.sfo`: prints version `01.01`, states that the
  executable is loaded, and reports the SNDX patch as skipped on 01.01. No warning, no exit.
- A full 01.01 run already on disk (`pc-vr/user/log/shad_log.txt`, revision `acda49e-dirty`)
  shows APP_VER 01.01, PSVR supported, OpenXR connected, NGS2/Audio3d initialized, the
  `sndx_audio3d_out_thread` running, and rendering for over 50 seconds with no exception.
- Not re-run live here: the game/emulator and SteamVR were deliberately left alone, per the
  standing rule and the HMD-slot protocol. The 1.00 versus 1.01 comparison above is static
  (hash/segment/disassembly), because the two builds are byte-identical in code.
