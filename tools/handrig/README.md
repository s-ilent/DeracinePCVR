# handrig — offline Déraciné hand-rig investigation

Read-only pak analysis used for `REPORT.md`. It answers: does the game have an articulated hand
rig, which skeletons/meshes/pose assets implement it, and how would OpenXR hand joints map onto it.

## Requirements

- .NET SDK 10 (CUE4Parse 1.2.2 via NuGet, restored from the local package cache)
- `oo2core_9_win64.dll` (Oodle) present at `C:\Users\Arukas\Downloads\`
- Déraciné pak at `D:\Temp\Deracine\1.0\Image0\gameproject\content\paks\gameproject-ps4.pak`
- Output directory `M:\Development\Deracine-work\_build\handrig` (outside the repository)

## Run

```powershell
dotnet build -c Release
.\bin\Release\net10.0\handrig.exe inventory
.\bin\Release\net10.0\handrig.exe skeleton "GameProject/Content/model/chr/c0000/model/c0000_Skeleton.uasset"
.\bin\Release\net10.0\handrig.exe skeleton "GameProject/Content/model/chr/c0100/model/c0100_Skeleton.uasset"
.\bin\Release\net10.0\handrig.exe mesh     "GameProject/Content/model/chr/c0000/model/c0000.uasset"
.\bin\Release\net10.0\handrig.exe mesh     "GameProject/Content/model/chr/c0100/model/c0100.uasset"
.\bin\Release\net10.0\handrig.exe poses
.\bin\Release\net10.0\handrig.exe posefinger "GameProject/Content/model/chr/c0000/motion/RightHand.uasset"
.\bin\Release\net10.0\handrig.exe posefinger "GameProject/Content/model/chr/c0100/motion/LeftHand.uasset"
.\bin\Release\net10.0\handrig.exe posefinger "GameProject/Content/model/chr/c0000/motion/Item_grasp.uasset"
.\bin\Release\net10.0\handrig.exe posefinger "GameProject/Content/model/chr/c0100/motion/Item_grasp.uasset"
.\bin\Release\net10.0\handrig.exe anims
python anatomy.py
python manifests.py
```

Set `UEVERSION=419` (or `420`) to try another version container; the 4.18 default parsed every
package without error.

## Modes

| Mode | Output |
|---|---|
| `inventory` | `inventory-exports.csv`, `inventory-references.csv` for every `GameProject` package |
| `skeleton <path>` | bone tree, reference pose (local + world), finger chains with measured thumb evidence, sockets, helper/virtual bones |
| `mesh <path>` | skeleton reference, bound and skinned bones, materials, morphs, sockets |
| `anim <path>` | frames, raw + compressed track-to-bone maps, finger tracks, curve names |
| `anims [filter]` | `animations.csv` over every animation sequence |
| `poses` | pose-asset inventory |
| `posefinger <path>` | per-finger rotation variation across a pose asset |
| `names [filter]` | matching asset paths |

`anatomy.py` and `manifests.py` post-process the dumps into the committed manifests. Absolute
output paths are deliberately fixed to this workstation; change `OUT` to relocate.

## Licensing

CUE4Parse is used as a NuGet dependency of a development-time tool; it is not linked into the
emulator. No game asset bytes are stored, and the committed manifests contain only counts, bone
names, mapping tables and measured angles.