# Writes the settings Déraciné reads out of its own Unreal saved configuration, and puts them back.
#
# The game reads <game>\gameproject\saved\config\ps4\engine.ini when it starts (the file-system log
# shows it opening that exact path), so this is the effective layer for the console build's own
# settings: the same keys in the same sections that the UE4 shipped in the pak reads and overrides.
# It is applied on the next start of the game, never while it runs.
#
# What it manages, and nothing else:
#
#   render  (r.ScreenPercentage, [SystemSettings]): how much of a PlayStation VR's 960x1080 an eye
#           the scene is drawn at. 100 = 960x1080, 130 = the game's own 1248x1404, 200 = 1920x2160,
#           300 = 2880x3240. The engine's own clamp is 300 (a single `vminss 300.0f` in the Morpheus
#           HMD size path, guest 0x13f43ac, constant 0x8035b030c), so 400 is accepted as an input and
#           reported as out of range rather than written: raising it needs the optional clamp patch,
#           which is not this script's business. See docs/psvr-morpheus-ue4-settings.md.
#
#   frames  (PSVRFrameSequence, [/Script/MorpheusEditor.MorpheusRuntimeSettings]): which PSVR frame
#           sequence the title asks its headset path for. "120" writes the tested native-render mode
#           (Render120Scanout120, with bSmoothFrameRate=False, the 22..250 smoothed range, t.MaxFPS=0
#           and r.FinishCurrentFrame=0 that were measured with it); "60" writes nothing and leaves the
#           game's own sequence alone. This is the setting that made the title submit 120 unique
#           frames a second instead of 60 (measured 2026-11-17: 1932 unique frames in 16.1 s, vblank
#           waits per frame 1.19 against 2.03); see the document for what was and was not verified.
#
#   msaa    (r.MSAACount, [SystemSettings]): the samples the game's own scene targets are made
#           with. The game ships with 2. Measured with the emulator's quality diagnostics at 1, 2
#           and 4: the guest's scene colour and depth buffers and the bound pipeline all carry that
#           many samples. This is NOT the emulator's sample cap (SHADPS4_MAX_MSAA, `msaa=` in
#           settings.txt), which limits what the host pictures may use.
#
# Every value is produced from the file as it was before DéracinePCVR first changed it (kept beside
# it as engine.ini.deracine-original), so switching a mode on and off is idempotent and no unrelated
# key or section is ever rewritten, reordered or dropped.
#
# Nothing here touches the game's own assets: not eboot.bin, not the pak, not the configs inside it.
# The saved configuration is a file the game itself writes at runtime, and it is backed up here
# before it is changed. Testing belongs on a private copy of the game, never on the original.
[CmdletBinding()]
param(
    # The game's eboot.bin, or the folder holding it.
    [string]$Eboot = "",
    # Render scale, 100 to 300 percent of a PlayStation VR's 960x1080 an eye.
    [string]$Percent = "",
    # Native frame mode: "60" (the game's own sequence) or "120" (Render120Scanout120).
    [string]$FrameMode = "",
    # The game's own MSAA samples: 1, 2, 4. Empty leaves what the game asks for.
    [string]$GuestMsaa = "",
    # Put the copy kept before the first change back, and forget it.
    [switch]$Restore,
    # Say what is managed and what it is set to now, and change nothing.
    [switch]$List,
    [switch]$Quiet
)

$ErrorActionPreference = "Continue"
$BackupSuffix = ".deracine-original"
$MinPercent = 100
$MaxPercent = 300
# The engine's own ceiling on r.ScreenPercentage, as an instruction: anything above this needs the
# optional clamp patch and is refused here rather than written and silently ignored.
$EngineClampPercent = 300
$FrameModes = @("60", "120")
$GuestMsaaValues = @(1, 2, 4)
# The 120 Hz block exactly as measured (2026-11-17), section by section. Written together because it
# was verified together; r.FinishCurrentFrame=0 is separately measured as a real gain over the
# shipped 1 (cold-state 48.7 against 44.0 frames a second).
$FrameMode120 = @(
    @{ Section = "SystemSettings"; Key = "r.FinishCurrentFrame"; Value = "0" },
    @{ Section = "SystemSettings"; Key = "t.MaxFPS"; Value = "0" },
    @{ Section = "/Script/Engine.Engine"; Key = "bSmoothFrameRate"; Value = "False" },
    @{ Section = "/Script/Engine.Engine"; Key = "SmoothedFrameRateRange"
       Value = "(LowerBound=(Type=Inclusive,Value=22),UpperBound=(Type=Exclusive,Value=250))" }
)
$MorpheusSection = "/Script/MorpheusEditor.MorpheusRuntimeSettings"
$MorpheusKey = "PSVRFrameSequence"
$FrameSequence120 = "Render120Scanout120"

function Say([string]$text) {
    if (-not $Quiet) { Write-Output $text }
}

# One machine-readable line per outcome, for the launcher to turn into a sentence.
function Report([string]$status, [string]$detail) {
    Write-Output ("render-config: {0} {1}" -f $status, $detail)
}

function Get-IniPath([string]$eboot) {
    if ([string]::IsNullOrWhiteSpace($eboot)) { return "" }
    # A folder holding a game, or the game's eboot.bin. Anything else is refused rather than
    # guessed at: a path that is neither must never end up writing into its parent by accident.
    $folder = ""
    if ([System.IO.Directory]::Exists($eboot)) {
        $folder = $eboot
    } elseif ([System.IO.File]::Exists($eboot)) {
        $folder = [System.IO.Path]::GetDirectoryName($eboot)
    } else {
        return ""
    }
    if ([string]::IsNullOrWhiteSpace($folder)) { return "" }
    return [System.IO.Path]::Combine($folder, "gameproject", "saved", "config", "ps4", "engine.ini")
}

# Splits an INI into lines, keeping the section each line belongs to. Section headers are "[]".
# (Callers wrap the result in @(): a single-entry result would otherwise come back as a scalar.)
function Get-IniLines([string[]]$lines) {
    $parsed = New-Object System.Collections.Generic.List[object]
    $section = ""
    foreach ($line in $lines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]$') { $section = $matches[1] }
        $parsed.Add([pscustomobject]@{ Text = $line; Section = $section })
    }
    return $parsed
}

function Get-ManagedValue($parsed, [string]$section, [string]$key) {
    $inSection = $false
    foreach ($entry in $parsed) {
        $trimmed = $entry.Text.Trim()
        if ($trimmed -match '^\[(.+)\]$') {
            $inSection = ($matches[1] -ieq $section)
            continue
        }
        if ($inSection -and $trimmed -match ('^\s*' + [regex]::Escape($key) + '\s*=\s*(.*)$')) {
            return $matches[1].Trim()
        }
    }
    return ""
}

# Sets key=value inside one section: in place of the first entry that sets it, dropping any later
# duplicates of it in that section, or at the end of the section (before any blank lines that
# separate it from the next one), or in a new section at the end of the file if the section is not
# there at all.
function Set-IniValue($parsed, [string]$section, [string]$key, [string]$value) {
    $result = New-Object System.Collections.Generic.List[object]
    $inSection = $false
    $wrote = $false
    $dropped = 0
    for ($i = 0; $i -lt $parsed.Count; $i++) {
        $entry = $parsed[$i]
        $trimmed = $entry.Text.Trim()
        if ($trimmed -match '^\[(.+)\]$') {
            if ($inSection -and -not $wrote) {
                # The end of the section: before the blank lines that close it.
                $at = $result.Count
                while ($at -gt 0 -and $result[$at - 1].Text.Trim() -eq "") { $at-- }
                $result.Insert($at, [pscustomobject]@{ Text = "$key=$value"; Section = $section })
                $wrote = $true
            }
            $inSection = ($matches[1] -ieq $section)
            $result.Add($entry)
            continue
        }
        if ($inSection -and $trimmed -match ('^\s*' + [regex]::Escape($key) + '\s*=')) {
            if (-not $wrote) {
                $result.Add([pscustomobject]@{ Text = "$key=$value"; Section = $section })
                $wrote = $true
            } else {
                $dropped++
            }
            continue
        }
        $result.Add($entry)
    }
    if ($inSection -and -not $wrote) {
        $at = $result.Count
        while ($at -gt 0 -and $result[$at - 1].Text.Trim() -eq "") { $at-- }
        $result.Insert($at, [pscustomobject]@{ Text = "$key=$value"; Section = $section })
        $wrote = $true
    }
    if (-not $wrote) {
        if ($result.Count -gt 0 -and $result[$result.Count - 1].Text.Trim() -ne "") {
            $result.Add([pscustomobject]@{ Text = ""; Section = "" })
        }
        $result.Add([pscustomobject]@{ Text = "[$section]"; Section = "" })
        $result.Add([pscustomobject]@{ Text = "$key=$value"; Section = $section })
    }
    return [pscustomobject]@{ Lines = $result; Dropped = $dropped }
}

# The game's file is written by the guest, which may use bare newlines. Whatever it uses is what a
# changed file keeps: only the settings are ours to touch.
function Get-Newline([string]$text) {
    if ($text.Contains("`r`n")) { return "`r`n" }
    return "`n"
}

# The same text whatever newlines it uses, for deciding whether anything really changed.
function Get-ComparableText([string]$text) {
    if ($null -eq $text) { return "" }
    return $text.Replace("`r`n", "`n")
}

function Read-IniLines([string]$path, [bool]$existed) {
    if (-not $existed) { return @() }
    return @([System.IO.File]::ReadAllLines($path))
}

# --- the settings asked for ---------------------------------------------------------------------
$ini = Get-IniPath $Eboot
if ($ini -eq "") {
    Report "error" "the game's folder is not known: pass -Eboot <eboot.bin or its folder>"
    exit 2
}
$backup = "$ini$BackupSuffix"
$existed = [System.IO.File]::Exists($ini)

function Get-OriginalLines() {
    if ([System.IO.File]::Exists($backup)) { return @([System.IO.File]::ReadAllLines($backup)) }
    return Read-IniLines $ini $existed
}

if ($Restore) {
    if (-not [System.IO.File]::Exists($backup)) {
        Report "nothing-to-restore" "no copy of $ini was kept"
        exit 0
    }
    try {
        Copy-Item -LiteralPath $backup -Destination $ini -Force -ErrorAction Stop
        Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
        Report "restored" $ini
        exit 0
    } catch {
        Report "error" ("could not put the game's own settings back: " + $_.Exception.Message)
        exit 3
    }
}

$showPercent = $Percent.Trim()
$showFrame = $FrameMode.Trim()
$showMsaa = $GuestMsaa.Trim()

if ($List) {
    $parsed = @(Get-IniLines (Get-OriginalLines))
    $current = @(Get-IniLines (Read-IniLines $ini $existed))
    $pct = Get-ManagedValue $parsed "SystemSettings" "r.ScreenPercentage"
    $seq = Get-ManagedValue $parsed $MorpheusSection $MorpheusKey
    $msaa = Get-ManagedValue $parsed "SystemSettings" "r.MSAACount"
    # What the game is actually running with now, which is the original plus whatever was written.
    $pctNow = Get-ManagedValue $current "SystemSettings" "r.ScreenPercentage"
    $seqNow = Get-ManagedValue $current $MorpheusSection $MorpheusKey
    $msaaNow = Get-ManagedValue $current "SystemSettings" "r.MSAACount"
    Report "list" ("$ini backup=" + ([System.IO.File]::Exists($backup)) +
        " original: render=$pct frame=$seq msaa=$msaa" +
        " effective: render=$pctNow frame=$seqNow msaa=$msaaNow")
    exit 0
}

if ($showPercent -eq "" -and $showFrame -eq "" -and $showMsaa -eq "") {
    Report "unchanged" "nothing asked for: the game's own settings are kept"
    exit 0
}

# --- check what was asked for before anything is written ------------------------------------------
$percent = 0
if ($showPercent -ne "") {
    if (-not [int]::TryParse($showPercent, [ref]$percent)) {
        Report "error" "render_percent is not a number: $showPercent"
        exit 4
    }
    if ($percent -lt $MinPercent -or $percent -gt $MaxPercent) {
        $what = if ($percent -gt $MaxPercent) {
            "above the engine's own $EngineClampPercent percent ceiling (a $percent " +
            "setting needs the optional clamp patch, see docs/psvr-morpheus-ue4-settings.md)"
        } else {
            "below $MinPercent percent"
        }
        Report "out-of-range" "render_percent $percent is ${what}: nothing was written"
        exit 4
    }
}
$frameMode = ""
if ($showFrame -ne "") {
    if ($FrameModes -notcontains $showFrame) {
        Report "error" ("frame_mode is neither " + ($FrameModes -join " nor ") + ": $showFrame")
        exit 5
    }
    $frameMode = $showFrame
}
$guestMsaa = 0
if ($showMsaa -ne "") {
    $parsedMsaa = 0
    if (-not [int]::TryParse($showMsaa, [ref]$parsedMsaa) -or $GuestMsaaValues -notcontains $parsedMsaa) {
        Report "error" ("guest_msaa is not one of " + ($GuestMsaaValues -join ", ") + ": $showMsaa")
        exit 6
    }
    $guestMsaa = $parsedMsaa
}

# --- the file the game reads, from what it was before we ever changed it ---------------------------
$original = Get-OriginalLines
$parsed = @(Get-IniLines $original)
$notes = @()
$dropped = 0

if ($percent -gt 0) {
    $set = Set-IniValue $parsed "SystemSettings" "r.ScreenPercentage" "$percent"
    $parsed = [object[]]$set.Lines
    $dropped += $set.Dropped
}
if ($guestMsaa -gt 0) {
    $set = Set-IniValue $parsed "SystemSettings" "r.MSAACount" "$guestMsaa"
    $parsed = [object[]]$set.Lines
    $dropped += $set.Dropped
}
if ($frameMode -eq "120") {
    foreach ($entry in $FrameMode120) {
        $set = Set-IniValue $parsed $entry.Section $entry.Key $entry.Value
        $parsed = [object[]]$set.Lines
        $dropped += $set.Dropped
    }
    $set = Set-IniValue $parsed $MorpheusSection $MorpheusKey $FrameSequence120
    $parsed = [object[]]$set.Lines
    $dropped += $set.Dropped
    $notes += "native 120: $FrameSequence120 with smoothing off, t.MaxFPS=0 and r.FinishCurrentFrame=0"
}
if ($frameMode -eq "60") {
    # The game's own sequence: whatever the untouched file says (the key is not in the shipped saved
    # configuration at all), and no companion keys either.
    $seqInOriginal = Get-ManagedValue (Get-IniLines $original) $MorpheusSection $MorpheusKey
    if ($seqInOriginal -eq $FrameSequence120) {
        Report "own-frame-sequence" ("the game's own file already asks for $seqInOriginal, which " +
            "frame_mode=60 does not change: use render_restore=1 to put that file back, or " +
            "frame_mode=120 to keep it deliberately")
    }
    $notes += "native 60: the game's own frame sequence, no change to smoothing or frame finishing"
}
$originalText = if ($existed) { [System.IO.File]::ReadAllText($ini) } else { "" }
$newline = Get-Newline $originalText
$text = (($parsed | ForEach-Object { $_.Text }) -join $newline)
# ReadAllLines drops the file's last line terminator; a file that ended with one keeps it, so what is
# written back is the same file with the settings changed and nothing else about it different.
if ($originalText.EndsWith("`n") -and $parsed.Count -gt 0) { $text += $newline }
$current = $originalText
if ((Get-ComparableText $current) -eq (Get-ComparableText $text)) {
    Report "unchanged" ("$ini already holds what was asked for")
    exit 0
}

# A file that is there is kept as it was, once, before the first change.
if ($existed -and -not [System.IO.File]::Exists($backup)) {
    try {
        Copy-Item -LiteralPath $ini -Destination $backup -Force -ErrorAction Stop
    } catch {
        Report "error" ("could not keep a copy of " + $ini + ": " + $_.Exception.Message)
        exit 7
    }
}

try {
    $directory = [System.IO.Path]::GetDirectoryName($ini)
    if (-not [System.IO.Directory]::Exists($directory)) {
        [void][System.IO.Directory]::CreateDirectory($directory)
    }
    [System.IO.File]::WriteAllText($ini, $text)
} catch {
    # A game folder that cannot be written (a mounted image, a locked install) is reported, not
    # worked around: the game still runs, with its own settings.
    Report "read-only" ("could not write " + $ini + ": " + $_.Exception.Message)
    exit 8
}

$what = @()
if ($percent -gt 0) { $what += "render=${percent}%" }
if ($guestMsaa -gt 0) { $what += "msaa=${guestMsaa}x" }
if ($frameMode -ne "") { $what += "frame_mode=$frameMode" }
$note = if ($dropped -gt 0) { " (" + $dropped + " duplicate entries removed)" } else { "" }
$tail = if ($notes.Count -gt 0) { "; " + ($notes -join "; ") } else { "" }
Report "applied" (($what -join ", ") + " in " + $ini + $note + $tail)
exit 0
