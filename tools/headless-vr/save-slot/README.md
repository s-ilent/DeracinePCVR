# Save slot for headless gameplay (M2)

Déraciné only loads the classroom from **savedata slot 1** (stage `m00_00_10_10`). Slot 0
(`m00_00_80_10`) stays on the menu forever, which is why a "Continue" without the right slot never
reaches gameplay. The M2 validation record pins three files:

| file | bytes | sha256 |
|---|---|---|
| `MGS_GameData_0` | 17,047 | `366B88B24B59A4D77BEEBFC72A1848724B36DC53DBC83A5E3FD29474FCE49D9F` |
| `MGS_GameData_1` | 17,313 | `C30D33846D35B090B3F89D4D1E6AD8C73B6B6DA264852BFEC9ECF8CD4214F93C` |
| `MGS_SystemData` | 2,897 | `EAB7FE140A57988167D45D8ABCB38C4AA0F518062CE26CD77D2BF027FD48745D` |
| `sce_sys/param.sfo` | 1,596 | `A5081D3FC574F20A7DB84E6208097720FB094A9B37ACDE4A3C43D5788B111926` |

The four files are save data (game/user content), so they are **not committed**; `sce_sys/param.sfo` is required, or the menu never loads Continue. `MANIFEST.json` pins
the bytes and `provision.ps1` refuses anything that does not match; the benchmark calls the same
check on every run.

```powershell
pwsh -NoProfile -File tools\headless-vr\save-slot\provision.ps1 `
  -From D:\saves\CUSA11919 `
  -To M:\Development\Deracine-work\wt-unified-headless\_out\headless-gameplay\r1\lab\user\home\1000\savedata\CUSA11919\SaveData
```

`-From` may be the `...\CUSA11919` directory (containing `SaveData`) or `SaveData` itself. A
different save is allowed only after updating `MANIFEST.json` deliberately, with the hashes recorded
in the run's `run.json` — the point is that a cell never silently measures a different stage.
