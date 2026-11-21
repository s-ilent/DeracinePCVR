# Headless gameplay benchmark (M2) — the default remains OpenXR-off and headset-free.
#
# One cell: build -> isolated lab -> launch -> scripted Move entry -> gameplay window -> normal quit
# -> gameplay-gate verdict. The scene gate and unique-FPS arithmetic live in gameplay_gate.py
# (unit-tested, no GPU); this script only orchestrates and records provenance. The guarded OpenXR
# mode is opt-in and may only be invoked by openxr-gameplay.ps1 with its preflight and path guard.
#
# Requirements, all committed to this repository:
#   - the executable under test (any build carrying the M2 commits)
#   - a game dump (an eboot.bin; a private shadow with its saved/engine.ini holds the 300% preset)
#   - a savedata directory matching tools/headless-vr/save-slot/MANIFEST.json (slot 1)
#   - tools/headless-vr/input/gameplay-entry.txt (the M2-proven scripted sequence)
#   - Python with the standard library (gameplay_gate.py uses nothing else)
#
# It does not touch pc-vr/, the active lab, SteamVR or any OpenXR runtime. It takes no GPU lock by
# itself: run it inside the machine-wide GPU lock, as the other headless tools do:
#   bash <worktree>/tools/headless-vr/gpu-lock.sh run --owner <you> --kind headless --reason "headless gameplay" \
#        --ttl 1800 --require-quiet -- pwsh.exe -NoProfile -File \
#        M:/.../wt-unified-headless/tools/benchmark/headless-gameplay.ps1 `
#        -Exe M:/.../build/shadPS4.exe -Eboot D:/Temp/Deracine/CUSA11919-qual/eboot.bin `
#        -SavesFrom M:/.../savedata/CUSA11919 -Out M:/.../_out/headless-gameplay-r1
#
# Exit codes: 0 valid, 2 invalid (the run happened but is not a gameplay cell), 1 execution error.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Eboot,
    [string]$SavesFrom = '',
    [string]$Out = '',
    [string]$Label = 'headless-gameplay',
    [string]$Lab = '',
    [string]$Template = '',
    [string]$InputScript = '',
    [string]$Profile = '',
    [string]$SaveManifest = '',
    [double]$Warmup = 10.0,
    [double]$Seconds = 40.0,
    [double]$StatsEvery = 0.5,
    [int]$ExitAfterSeconds = 180,
    # The camera/headset check needs the dev autopress; it must stop before the menu appears or it
    # walks the menu and races the scripted entry.
    [int]$AutopressMilliseconds = 45000,
    [int]$TimeoutSeconds = 300,
    [string]$Python = 'python',
    [switch]$EyeDump,
    # Gameplay entry is injected when the log shows a settled menu, then re-injected if the title
    # returns to it; this is what makes independently constructed labs (and slow cold shader
    # compilation) work with a script whose presses must land while the menu is up.
    [switch]$NoMenuGatedInput,
    [int]$MaxInputInjections = 3,
    [switch]$DryRun,
    [string]$ReplayLog = '',
    [switch]$OpenXR,
    [switch]$OpenXRStartupMenuOnly,
    [ValidateSet('off', 'on')][string]$RhiProbeMode = 'on',
    [switch]$EnableOpenXRTestInput,
    [string]$OpenXRGuardConfig = '',
    [string]$OpenXRPreflight = '',
    [string]$ShadowRoot = ''
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($OpenXR) { . (Join-Path $PSScriptRoot 'openxr_probe_parser.ps1') }
if ($Out -eq '') { $Out = Join-Path $Repo "_out\headless-gameplay\$Label" }
if ($Lab -eq '') { $Lab = Join-Path $Out 'lab' }
if ($Template -eq '') { $Template = Join-Path $Repo 'pc-vr' }
if ($InputScript -eq '' -and -not $OpenXRStartupMenuOnly) { $InputScript = Join-Path $Repo 'tools\headless-vr\input\gameplay-entry.txt' }
if ($Profile -eq '') { $Profile = Join-Path $Repo 'tools\benchmark\reference\scene-gameplay.json' }
if ($SaveManifest -eq '') { $SaveManifest = Join-Path $Repo 'tools\headless-vr\save-slot\MANIFEST.json' }

function Write-Failure([string]$Message) {
    Write-Output "headless-gameplay: $Message"
    exit 1
}

function Assert-File([string]$Path, [string]$What) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Write-Failure "$What not found: $Path"
    }
}

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Test-PathWithin([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Invoke-OpenXRGuard([string]$Op, [string]$Path) {
    if (-not $OpenXR) { return }
    if ([string]::IsNullOrWhiteSpace($OpenXRGuardConfig)) {
        Write-Failure 'OpenXR mode requires a path-guard config from openxr-gameplay.ps1'
    }
    $output = & $Python (Join-Path $PSScriptRoot 'path_guard.py') --config $OpenXRGuardConfig `
        --op $Op --path $Path 2>&1
    $guardExit = $LASTEXITCODE
    if ($guardExit -ne 0) {
        Write-Failure "path guard refused '$Op' on '$Path': $($output -join ' ')"
    }
}

function Get-OpenXRMoveEvents([string]$Path) {
    $events = [System.Collections.Generic.List[object]]::new()
    $linePattern = [regex]'^(?<start>[0-9]+(?:\.[0-9]+)?)\s+(?<hold>[0-9]+(?:\.[0-9]+)?)\s+(?<actions>.+)$'
    $movePattern = [regex]'(?i)\bmove[01]=[^\s]+'
    foreach ($line in Get-Content -LiteralPath $Path -ErrorAction Stop) {
        $text = $line.Trim()
        if (-not $text -or $text.StartsWith('#')) { continue }
        $match = $linePattern.Match($text)
        if (-not $match.Success) { continue }
        $moves = @($movePattern.Matches($match.Groups['actions'].Value) | ForEach-Object { $_.Value })
        if ($moves.Count -eq 0) { continue }
        $events.Add([ordered]@{
            start = [double]::Parse($match.Groups['start'].Value, [Globalization.CultureInfo]::InvariantCulture)
            hold = [double]::Parse($match.Groups['hold'].Value, [Globalization.CultureInfo]::InvariantCulture)
            actions = $moves -join ' '
        })
    }
    return $events.ToArray()
}

function Test-SkippedTemplatePath([string]$RelativePath, [switch]$NoSaveData) {
    $normalized = $RelativePath.Replace('/', '\')
    if ([IO.Path]::GetFileName($normalized).Equals('shadps4.exe', [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    if ($NoSaveData -and $normalized -match '(?i)^user\\home(?:\\|$)') { return $true }
    foreach ($part in $normalized.Split('\')) {
        if ($part -in @('log', 'temp', '_out')) { return $true }
    }
    return $false
}

function Guard-TreeCopyTargets([string]$SourceRoot, [string]$DestinationRoot, [switch]$TemplateRules, [switch]$NoSaveData) {
    if (-not $OpenXR) { return }
    Invoke-OpenXRGuard 'mkdir' $DestinationRoot
    $sourceFull = (Resolve-Path -LiteralPath $SourceRoot -ErrorAction Stop).Path
    $sourceItem = Get-Item -LiteralPath $sourceFull -Force -ErrorAction Stop
    if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Write-Failure "copy source root is a reparse point: $sourceFull"
    }
    $pendingDirectories = [System.Collections.Generic.Stack[string]]::new()
    $pendingDirectories.Push($sourceFull)
    while ($pendingDirectories.Count -gt 0) {
        $current = $pendingDirectories.Pop()
        foreach ($entry in @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)) {
            $relative = [IO.Path]::GetRelativePath($sourceFull, $entry.FullName)
            if ($TemplateRules -and (Test-SkippedTemplatePath -RelativePath $relative -NoSaveData:$NoSaveData)) { continue }
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Write-Failure "copy source contains an unapproved reparse point: $($entry.FullName)"
            }
            $destination = Join-Path $DestinationRoot $relative
            if ($entry.PSIsContainer) {
                Invoke-OpenXRGuard 'mkdir' $destination
                $pendingDirectories.Push($entry.FullName)
            } else {
                Invoke-OpenXRGuard 'copy_dest' $destination
            }
        }
    }
}

function Copy-SaveTree([string]$SourceRoot, [string]$DestinationRoot) {
    if ($OpenXR) { Invoke-OpenXRGuard 'mkdir' $DestinationRoot }
    New-Item -ItemType Directory -Force -Path $DestinationRoot | Out-Null
    $sourceFull = (Resolve-Path -LiteralPath $SourceRoot -ErrorAction Stop).Path
    foreach ($entry in @(Get-ChildItem -LiteralPath $sourceFull -Recurse -Force -ErrorAction Stop)) {
        if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Write-Failure "save source contains an unapproved reparse point: $($entry.FullName)"
        }
        $relative = [IO.Path]::GetRelativePath($sourceFull, $entry.FullName)
        $destination = Join-Path $DestinationRoot $relative
        if ($entry.PSIsContainer) {
            if ($OpenXR) { Invoke-OpenXRGuard 'mkdir' $destination }
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
        } else {
            if ($OpenXR) { Invoke-OpenXRGuard 'copy_dest' $destination }
            Copy-Item -LiteralPath $entry.FullName -Destination $destination -Force
        }
    }
}

function Format-OpenXRMoveScript($Events, [double]$BaseTime) {
    if (@($Events).Count -eq 0) { return '' }
    $first = [double]$Events[0].start
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('# OpenXR session and HMD frame observed; Move-only test input')
    foreach ($event in $Events) {
        $at = $BaseTime + 1.0 + ([double]$event.start - $first)
        $timeText = $at.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture)
        $holdText = ([double]$event.hold).ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
        $lines.Add("$timeText $holdText $($event.actions)")
    }
    return $lines -join "`r`n"
}

function Get-OpenXRLogEvidence([string]$Path) {
    $sessionLines = @(Select-String -LiteralPath $Path -Pattern 'Session with the headset made' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line })
    $frameLines = @(Select-String -LiteralPath $Path -Pattern 'HMD frame \d+ eye [01]: image \d+' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line })
    $controllerLines = @(Select-String -LiteralPath $Path -Pattern '(?i)controller' -ErrorAction SilentlyContinue |
        Select-Object -First 100 | ForEach-Object { $_.Line })
    return [ordered]@{
        session_created = ($sessionLines.Count -gt 0)
        session_lines = $sessionLines
        hmd_frame_log_count = $frameLines.Count
        first_hmd_frame_lines = @($frameLines | Select-Object -First 6)
        controller_related_lines = $controllerLines
        controller_tracking_samples = $null
        controller_tracking_note = 'the current Info log has no positive per-sample tracking counter; do not infer tracking from PnP or session creation'
    }
}

if ($OpenXRStartupMenuOnly -and -not $OpenXR) {
    Write-Failure 'OpenXRStartupMenuOnly is valid only together with OpenXR mode'
}
if ($OpenXR) {
    if ($ReplayLog -ne '') { Write-Failure 'OpenXR launch mode does not accept replay logs' }
    if ($NoMenuGatedInput -and -not $OpenXRStartupMenuOnly) {
        Write-Failure 'saved-game OpenXR mode requires menu-gated input after runtime session evidence'
    }
    if ($OpenXRStartupMenuOnly -and $EnableOpenXRTestInput) {
        Write-Failure 'startup/menu-only OpenXR mode never accepts scripted test input'
    }
    if ($OpenXRStartupMenuOnly -and -not [string]::IsNullOrWhiteSpace($SavesFrom)) {
        Write-Failure 'startup/menu-only OpenXR mode refuses any SaveData source path'
    }
    if (-not $OpenXRStartupMenuOnly -and [string]::IsNullOrWhiteSpace($SavesFrom)) {
        Write-Failure 'saved-game OpenXR mode requires a manifest-verified SavesFrom path'
    }
    if ($EyeDump) { Write-Failure 'OpenXR mode leaves eye dumping off to keep the capture bounded' }
    if ($MaxInputInjections -gt 1) { Write-Failure 'OpenXR probe permits at most one test-input injection' }
    if ($TimeoutSeconds -gt 180 -or $ExitAfterSeconds -gt 150) {
        Write-Failure 'OpenXR probe is bounded to at most 180 seconds overall and 150 seconds of emulator runtime'
    }
    if ([string]::IsNullOrWhiteSpace($ShadowRoot) -or
        [string]::IsNullOrWhiteSpace($OpenXRPreflight) -or
        [string]::IsNullOrWhiteSpace($OpenXRGuardConfig)) {
        Write-Failure 'OpenXR mode requires ShadowRoot, OpenXRPreflight, and OpenXRGuardConfig from openxr-gameplay.ps1'
    }
    $b3OutputRoot = Join-Path $Repo '_out\B3_semantic_capture'
    if (-not (Test-PathWithin $Out $b3OutputRoot)) {
        Write-Failure "OpenXR output must stay inside $b3OutputRoot"
    }
    if (-not (Test-PathWithin $OpenXRPreflight $Out) -or
        -not (Test-PathWithin $OpenXRGuardConfig $Out)) {
        Write-Failure 'OpenXR preflight and guard config must be inside the private cell output directory'
    }
    if (-not (Test-Path -LiteralPath $OpenXRPreflight -PathType Leaf) -or
        -not (Test-Path -LiteralPath $OpenXRGuardConfig -PathType Leaf)) {
        Write-Failure 'OpenXR preflight or guard config is missing'
    }
    $guardDoc = Get-Content -LiteralPath $OpenXRGuardConfig -Raw | ConvertFrom-Json
    $expectedWriteRoots = @([IO.Path]::GetFullPath($Out), [IO.Path]::GetFullPath($Lab))
    $actualWriteRoots = @($guardDoc.allow_write | ForEach-Object { [IO.Path]::GetFullPath([string]$_) })
    if ($actualWriteRoots.Count -ne $expectedWriteRoots.Count -or
        @($actualWriteRoots | Where-Object { $_ -notin $expectedWriteRoots }).Count -gt 0) {
        Write-Failure 'OpenXR guard config may write only to the private cell output and its lab'
    }
    $shadowFull = [IO.Path]::GetFullPath($ShadowRoot).TrimEnd('\')
    $ebootFull = [IO.Path]::GetFullPath($Eboot)
    if (-not $ebootFull.Equals((Join-Path $shadowFull 'eboot.bin'), [StringComparison]::OrdinalIgnoreCase)) {
        Write-Failure "OpenXR eboot must be the private shadow's eboot.bin: $shadowFull"
    }
    $preDoc = Get-Content -LiteralPath $OpenXRPreflight -Raw | ConvertFrom-Json
    $ebootActual = Get-Sha256 $Eboot
    $pinnedShadowEboot = '8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75'
    if (-not $preDoc.shadow.requested -or
        -not ([string]$preDoc.shadow.root).Equals($shadowFull, [StringComparison]::OrdinalIgnoreCase) -or
        -not ([string]$preDoc.shadow.eboot_sha256).Equals($ebootActual, [StringComparison]::OrdinalIgnoreCase) -or
        -not $ebootActual.Equals($pinnedShadowEboot, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Failure 'OpenXR preflight shadow manifest/eboot evidence does not match the pinned 1.01 shadow hash'
    }
    $protectedRoots = @($preDoc.protected.protected_tree_roots)
    if ($protectedRoots.Count -lt 6 -or
        -not (@($preDoc.protected_trees | ForEach-Object { [string]$_.root }) -contains 'D:\Temp\Deracine\CUSA11919-qual')) {
        Write-Failure 'OpenXR preflight does not include the shared root and all resolved junction target trees'
    }
    $originalEbootPath = 'D:\Temp\Deracine\1.0\Image0\eboot.bin'
    $originalEboot = $preDoc.immutable.PSObject.Properties[$originalEbootPath]
    if ($null -eq $originalEboot -or
        -not ([string]$originalEboot.Value.sha256).Equals('8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0', [StringComparison]::OrdinalIgnoreCase)) {
        Write-Failure 'OpenXR preflight did not preserve the exact protected 1.00 eboot hash record'
    }
    foreach ($root in @('D:\Temp\Deracine\CUSA11919-qual', 'D:\Temp\Deracine\1.0\Image0')) {
        foreach ($relative in @('eboot.bin', 'title.json', 'gameproject\saved\config\ps4\engine.ini')) {
            $path = Join-Path $root $relative
            $record = $preDoc.immutable.PSObject.Properties[$path]
            if ($null -eq $record -or -not [bool]$record.Value.exists) {
                Write-Failure "OpenXR preflight lacks a protected source hash record: $path"
            }
        }
    }
}

Assert-File $Exe 'executable'
Assert-File $Eboot 'eboot'
$manifest = $null
$saveSrc = $null
$saveHashes = [ordered]@{}
if ($OpenXRStartupMenuOnly) {
    Write-Output 'startup/menu-only mode: SaveData source and save manifest are not read or copied'
} else {
    Assert-File $InputScript 'input script'
    Assert-File $Profile 'gameplay profile'
    Assert-File $SaveManifest 'save manifest'

    # --- deterministic save slot --------------------------------------------------------------
    # The save is game/user data and is not committed; the manifest pins the exact bytes. Refuse
    # anything else, so a cell cannot silently measure a different stage.
    $manifest = Get-Content -LiteralPath $SaveManifest -Raw | ConvertFrom-Json
    $saveSrc = Join-Path $SavesFrom 'SaveData'
    if (-not (Test-Path -LiteralPath $saveSrc -PathType Container)) {
        # Accept either ...\savedata\CUSA11919 (containing SaveData) or the SaveData directory itself.
        if (Test-Path -LiteralPath (Join-Path $SavesFrom 'MGS_GameData_1') -PathType Leaf) {
            $saveSrc = $SavesFrom
        } else {
            Write-Failure "savedata not found under $SavesFrom (expected SaveData\MGS_GameData_1)"
        }
    }
    foreach ($name in $manifest.files.PSObject.Properties.Name) {
        $path = Join-Path $saveSrc $name
        Assert-File $path "save file $name"
        $hash = Get-Sha256 $path
        if ($hash -ne $manifest.files.$name.ToUpperInvariant()) {
            Write-Failure "save file $name has sha256 $hash, the manifest requires $($manifest.files.$name)"
        }
        $saveHashes[$name] = $hash
    }
    Write-Output "save slot verified: $($manifest.files.PSObject.Properties.Name.Count) files, slot $($manifest.slot), stage $($manifest.stage)"
}

$exeSha = Get-Sha256 $Exe
$ebootSha = Get-Sha256 $Eboot
$branch = (& git -C $Repo rev-parse --abbrev-ref HEAD 2>$null)
$head = (& git -C $Repo rev-parse HEAD 2>$null)
$dirty = @(& git -C $Repo status --porcelain 2>$null)

# --- pinned gameplay environment ----------------------------------------------------------------
# Default values reproduce M2 with OpenXR off. OpenXR mode removes synthetic hand/pad/autopress
# overrides and enables headset controllers; input remains neutral until session+frame log proof.
$envMap = [ordered]@{
    SHADPS4_VR                 = '1'
    SHADPS4_OPENXR             = '0'
    SHADPS4_VR_DEMO            = '0'
    SHADPS4_XR_PAUSE           = '0'
    SHADPS4_TITLE_EYE_WIDTH    = '2880'
    SHADPS4_TITLE_RESOLUTION   = '6'
    SHADPS4_VR_SCALE           = '0.5'
    SHADPS4_VR_FOV             = '100'
    SHADPS4_MAX_MSAA           = '2'
    SHADPS4_VR_DESKTOP_VIEW    = 'stereo'
    SHADPS4_VR_SHARPEN         = '0.25'
    SHADPS4_FORCE_ANISO        = '16'
    SHADPS4_VR_REFRESH_RATE    = '120'
    SHADPS4_VR_FPS_CAP         = '120'
    SHADPS4_VR_FASTEST_PACE    = '1'
    SHADPS4_CONSOLE_LANGUAGE   = 'en-US'
    SHADPS4_FRAME_STATS        = '1'
    SHADPS4_FRAME_STATS_EVERY  = "$StatsEvery"
    # M2 test-only input: synthetic tracked hands + Move input from the emulated pad + the existing
    # dev autopress for the controller-check screen, then the committed scripted sequence.
    SHADPS4_VR_HANDS           = 'synthetic'
    SHADPS4_VR_MOVE_FROM_PAD   = '1'
    SHADPS4_MOVE_PITCH         = '0'
    SHADPS4_MOVE_Y             = '0'
    SHADPS4_MOVE_Z             = '-0.135'
    SHADPS4_DBG_AUTOPRESS      = '1'
    SHADPS4_DBG_AUTOPRESS_TO   = "$AutopressMilliseconds"
    SHADPS4_INPUT_SCRIPT       = $(if ($OpenXRStartupMenuOnly) { '' } elseif ($NoMenuGatedInput) { (Resolve-Path -LiteralPath $InputScript).Path } else { Join-Path $Out 'input-initial.txt' })
    # Normal quit through the emulator's own path; clean presenter teardown (M2).
    SHADPS4_EXIT_AFTER         = "$ExitAfterSeconds"
    SHADPS4_CLEAN_EXIT         = '1'
}
if (-not $NoMenuGatedInput -and (-not $OpenXR -or $EnableOpenXRTestInput)) {
    $envMap['SHADPS4_LIVE_INPUT'] = (Join-Path $Out 'input-live.txt')
}
if ($OpenXR) {
    $envMap['SHADPS4_OPENXR'] = '1'
    $envMap['SHADPS4_XR_CONTROLLERS'] = '1'
    if ($RhiProbeMode -eq 'on') {
        $envMap['SHADPS4_DERACINE_RHI_PROBE'] = '1'
    } else {
        $envMap.Remove('SHADPS4_DERACINE_RHI_PROBE')
    }
    $envMap.Remove('SHADPS4_VR_HANDS')
    $envMap.Remove('SHADPS4_VR_MOVE_FROM_PAD')
    $envMap.Remove('SHADPS4_MOVE_PITCH')
    $envMap.Remove('SHADPS4_MOVE_Y')
    $envMap.Remove('SHADPS4_MOVE_Z')
    $envMap.Remove('SHADPS4_DBG_AUTOPRESS')
    $envMap.Remove('SHADPS4_DBG_AUTOPRESS_TO')
    if (-not $EnableOpenXRTestInput) { $envMap.Remove('SHADPS4_INPUT_SCRIPT') }
}
if ($OpenXR -and $EnableOpenXRTestInput) {
    $openXrMoveEvents = @(Get-OpenXRMoveEvents $InputScript)
    if ($openXrMoveEvents.Count -eq 0) {
        Write-Failure 'the existing input script has no Move-only events for OpenXR gating'
    }
    if ($openXrMoveEvents.Count -gt 10) {
        Write-Failure "OpenXR Move-only sequence exceeds the fixed 10-event cap ($($openXrMoveEvents.Count))"
    }
}
if ($EyeDump) {
    # Scene-gated eye dump at the gameplay gate; the folder is relative to the user directory.
    $envMap['SHADPS4_DBG_DUMP_HMD_AT_SCENE'] = '800,550,rt_dump-gameplay'
}

Write-Output "executable $Exe ($exeSha)"
Write-Output "eboot      $Eboot ($ebootSha)"
if ($OpenXRStartupMenuOnly) {
    Write-Output 'script     <none; startup/menu-only mode does not read or inject input scripts>'
} else {
    Write-Output "script     $InputScript"
}

# --- replay mode: evaluate an existing log without launching ----------------------------------
function Invoke-Gate([string]$LogPath, [string]$SaveDir) {
    $json = & $Python (Join-Path $Repo 'tools\benchmark\gameplay_gate.py') $LogPath `
        --profile $Profile --save-dir $SaveDir --save-manifest $SaveManifest --json 2>&1
    $code = $LASTEXITCODE
    try {
        return @{ code = $code; verdict = ($json | ConvertFrom-Json) }
    } catch {
        Write-Failure "gameplay_gate.py produced no JSON (exit $code): $json"
    }
}

if ($ReplayLog -ne '') {
    Assert-File $ReplayLog 'replay log'
    New-Item -ItemType Directory -Force -Path $Out | Out-Null
    $result = Invoke-Gate $ReplayLog $saveSrc
    $document = [ordered]@{
        schema_version = 1
        label          = $Label
        created_utc    = (Get-Date).ToUniversalTime().ToString('o')
        mode           = 'replay'
        replay_log     = $ReplayLog
        profile        = $Profile
        verdict        = $result.verdict
        limitations    = @('replay: the log was produced by another run; no launch was performed')
    }
    $document | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $Out 'run.json') -Encoding utf8
    $document.verdict | ConvertTo-Json -Depth 10
    exit $result.code
}

if ($DryRun) {
    Write-Output "dry run: lab would be $Lab"
    $envMap.GetEnumerator() | ForEach-Object { Write-Output ("  {0}={1}" -f $_.Key, $_.Value) }
    exit 0
}

# --- isolated lab -----------------------------------------------------------------------------
# Built from the tracked pc-vr template, never from the active runtime folder. The executable under
# test is copied in; the save is provisioned from the verified source above.
Invoke-OpenXRGuard 'mkdir' $Out
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$labExe = Join-Path $Lab 'shadPS4.exe'
if (-not (Test-Path -LiteralPath (Join-Path $Lab 'user\config.json'))) {
    Write-Output "creating isolated lab $Lab from $Template"
    if (-not (Test-Path -LiteralPath $Template -PathType Container)) {
        Write-Failure "template runtime not found: $Template"
    }
    Invoke-OpenXRGuard 'mkdir' $Lab
    New-Item -ItemType Directory -Force -Path $Lab | Out-Null
    $copyArgs = @('/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP',
                  '/XF', 'shadps4.exe', '/XD', 'log', 'temp', '_out')
    if ($OpenXR) {
        if ($OpenXRStartupMenuOnly) { $copyArgs += (Join-Path $Template 'user\home') }
        $copyArgs += '/XJ'
        Guard-TreeCopyTargets -SourceRoot $Template -DestinationRoot $Lab -TemplateRules `
            -NoSaveData:$OpenXRStartupMenuOnly
    }
    & robocopy $Template $Lab @copyArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { Write-Failure "robocopy of the template failed ($LASTEXITCODE)" }
    if (-not $OpenXRStartupMenuOnly) {
        $dstSave = Join-Path $Lab 'user\home\1000\savedata\CUSA11919\SaveData'
        if ($OpenXR) {
            Copy-SaveTree -SourceRoot $saveSrc -DestinationRoot $dstSave
        } else {
            New-Item -ItemType Directory -Force -Path $dstSave | Out-Null
            Copy-Item (Join-Path $saveSrc '*') $dstSave -Recurse -Force
        }
    }
    # Lossless log tail and the benchmark's GPU settings, as the M1 launcher does.
    $cfgPath = Join-Path $Lab 'user\config.json'
    if (Test-Path -LiteralPath $cfgPath) {
        $cfg = Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json
        $cfg.Log.type = 'file'
        $cfg.Log.filter = '*:Info'
        $cfg.Log.sync = $true
        $cfg.Log.skip_duplicate = $true
        $cfg.GPU.readbacks_mode = 1
        $cfg.GPU.copy_gpu_buffers = $false
        $cfg.GPU.readback_linear_images_enabled = $true
        $cfg.GPU.vblank_frequency = 120
        $cfg.Vulkan.pipeline_cache_enabled = $false
        Invoke-OpenXRGuard 'write' $cfgPath
        ($cfg | ConvertTo-Json -Depth 20) | Set-Content -LiteralPath $cfgPath -Encoding utf8
    }
} else {
    Write-Output "reusing lab $Lab (delete it to force a clean rebuild)"
}
Invoke-OpenXRGuard 'copy_dest' $labExe
Copy-Item -LiteralPath $Exe -Destination $labExe -Force
$logDir = Join-Path $Lab 'user\log'
Invoke-OpenXRGuard 'mkdir' $logDir
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
if ($OpenXR) {
    foreach ($oldLog in @(Get-ChildItem -LiteralPath $logDir -File -Force -ErrorAction SilentlyContinue)) {
        Invoke-OpenXRGuard 'delete' $oldLog.FullName
        Remove-Item -LiteralPath $oldLog.FullName -Force -ErrorAction SilentlyContinue
    }
    foreach ($dump in @(Get-ChildItem -LiteralPath $Lab -Directory -Filter 'rt_dump*' -Force -ErrorAction SilentlyContinue)) {
        Invoke-OpenXRGuard 'delete' $dump.FullName
        Remove-Item -LiteralPath $dump.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }
} else {
    Remove-Item (Join-Path $logDir '*') -Force -ErrorAction SilentlyContinue
    Get-ChildItem $Lab -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

# Never inherit the dev-only probe opt-in; OpenXR mode sets it explicitly below.
Remove-Item Env:SHADPS4_DERACINE_RHI_PROBE -ErrorAction SilentlyContinue
if ($OpenXR) {
    foreach ($name in @('SHADPS4_VR_HANDS', 'SHADPS4_VR_MOVE_FROM_PAD', 'SHADPS4_MOVE_PITCH',
                        'SHADPS4_MOVE_Y', 'SHADPS4_MOVE_Z', 'SHADPS4_DBG_AUTOPRESS',
                        'SHADPS4_DBG_AUTOPRESS_TO')) {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
    if (-not $EnableOpenXRTestInput) {
        Remove-Item Env:SHADPS4_INPUT_SCRIPT -ErrorAction SilentlyContinue
        Remove-Item Env:SHADPS4_LIVE_INPUT -ErrorAction SilentlyContinue
    }
}
foreach ($k in $envMap.Keys) { Set-Item -Path "Env:$k" -Value $envMap[$k] }
# Do not inherit a live script or replay overrides from the caller. The gated path sets its own
# live file only after the OpenXR session and HMD-frame markers have both been observed.
if ($NoMenuGatedInput -or ($OpenXR -and -not $EnableOpenXRTestInput)) {
    Remove-Item Env:SHADPS4_LIVE_INPUT -ErrorAction SilentlyContinue
}
Remove-Item Env:SHADPS4_DBG_HAND_POSE -ErrorAction SilentlyContinue
Remove-Item Env:SHADPS4_DBG_DUMP_HMD_FRAMES -ErrorAction SilentlyContinue

$useMenuInput = (-not $NoMenuGatedInput) -and (-not $OpenXR -or $EnableOpenXRTestInput)
if ($useMenuInput) {
    if ($OpenXR) {
        $initialText = '# neutral until OpenXR session and HMD frame are observed'
    } else {
        # Wait at arm's reach with no presses; default mode retains its proven head/pad pose.
        $initialText = '0 100000 head=0,0,0,0,0,0 pad=0,-0.17,-0.5,0,0,0'
    }
    $inputInitialPath = $envMap['SHADPS4_INPUT_SCRIPT']
    $inputLivePath = $envMap['SHADPS4_LIVE_INPUT']
    Invoke-OpenXRGuard 'write' $inputInitialPath
    Invoke-OpenXRGuard 'write' $inputLivePath
    [IO.File]::WriteAllText($inputInitialPath, $initialText + "`r`n")
    [IO.File]::WriteAllText($inputLivePath, $initialText + "`r`n")
}

# --- run --------------------------------------------------------------------------------------
$log = Join-Path $Lab 'user\log\shad_log.txt'
$stdout = Join-Path $Out 'stdout.txt'
$stderr = Join-Path $Out 'stderr.txt'
$started = Get-Date
Invoke-OpenXRGuard 'write' $log
Invoke-OpenXRGuard 'write' $stdout
Invoke-OpenXRGuard 'write' $stderr
Write-Output "launching $labExe -g $Eboot"
$p = Start-Process -FilePath $labExe -WorkingDirectory $Lab -ArgumentList @('-g', $Eboot) `
    -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$deadline = $started.AddSeconds($TimeoutSeconds)
# Menu-gated entry: independently constructed labs (and cold shader compilation) put the fixed
# script's presses in the loading screen. Watch the frame-stats windows instead; when a settled
# menu is measured, replace the live script with presses timed from that moment. Re-inject if the
# title returns to the menu after a failed load, up to -MaxInputInjections. A window is "menu" only
# inside the measured menu range, and gameplay is only a window inside the gameplay band, so a
# heavy load is not mistaken for either.
$statsRe = [regex]'frame stats at\s+(?<at>\d+)\s+s:.*?per frame\s+(?<passes>[0-9.]+)\s+passes\s+\([^)]*?(?<mpx>[0-9.]+)\s+Mpx\),\s+(?<draws>[0-9.]+)\s+draws'
$injections = 0
$lastInjectAt = -1000.0
$menuRun = 0
$heavyRun = 0
$gameplaySeen = $false
$openXrSessionSeen = $false
$openXrFrameSeen = $false
$openXrInputGatePassed = $false
$openXrMenuSeen = $false
$openXrMenuSamples = [System.Collections.Generic.List[object]]::new()
$lastOpenXrStatsAt = -1.0
$openXrScanPosition = [long]0
$openXrInjectedScriptPath = $null
$openXrInjectedScriptSha256 = $null
$openXrInjectionAtSeconds = $null
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if (-not (Test-Path -LiteralPath $log)) { continue }
    if ($OpenXR -and (-not $openXrSessionSeen -or -not $openXrFrameSeen)) {
        $scanStream = $null
        $scanReader = $null
        try {
            $scanStream = [IO.File]::Open($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            $null = $scanStream.Seek($openXrScanPosition, [IO.SeekOrigin]::Begin)
            $scanReader = [IO.StreamReader]::new($scanStream)
            while ($null -ne ($scanLine = $scanReader.ReadLine())) {
                if (-not $openXrSessionSeen -and $scanLine -match 'Session with the headset made') {
                    $openXrSessionSeen = $true
                }
                if (-not $openXrFrameSeen -and $scanLine -match 'HMD frame \d+ eye [01]: image \d+') {
                    $openXrFrameSeen = $true
                }
            }
            $openXrScanPosition = $scanStream.Position
        } catch {
            Write-Output "OpenXR evidence scan warning: $($_.Exception.Message)"
        } finally {
            if ($null -ne $scanReader) { $scanReader.Dispose() }
            elseif ($null -ne $scanStream) { $scanStream.Dispose() }
        }
        if ($openXrSessionSeen -and $openXrFrameSeen) {
            $openXrInputGatePassed = $true
            Write-Output 'OpenXR gate passed: headset session creation and Vulkan HMD-frame handoff observed'
        }
    }
    if ($OpenXR -and -not $openXrInputGatePassed) { continue }
    if (-not $useMenuInput -and -not $OpenXRStartupMenuOnly) { continue }
    try { $tail = Get-Content -LiteralPath $log -Tail 60 -ErrorAction Stop } catch { continue }
    $last = $null
    foreach ($line in $tail) { $m = $statsRe.Match($line); if ($m.Success) { $last = $m } }
    if ($null -eq $last) { continue }
    $at = [double]$last.Groups['at'].Value
    if ($OpenXRStartupMenuOnly -and $at -le $lastOpenXrStatsAt) { continue }
    if ($OpenXRStartupMenuOnly) { $lastOpenXrStatsAt = $at }
    $draws = [double]$last.Groups['draws'].Value
    $mpx = [double]$last.Groups['mpx'].Value
    $passes = [double]$last.Groups['passes'].Value
    $inBand = ($draws -ge 1500.0 -and $draws -le 2300.0 -and $mpx -ge 500.0 -and
               $mpx -le 1000.0 -and $passes -ge 25.0 -and $passes -le 80.0)
    if ($inBand) { $heavyRun++ } else { $heavyRun = 0 }
    if ($heavyRun -ge 2) { $gameplaySeen = $true }
    $isMenu = ($draws -ge 350.0 -and $draws -le 800.0 -and $mpx -ge 300.0 -and $mpx -le 750.0)
    if ($isMenu) { $menuRun++ } else { $menuRun = 0 }
    if ($OpenXRStartupMenuOnly) {
        $openXrMenuSamples.Add([ordered]@{ at_seconds = $at; draws = $draws; mpx = $mpx; passes = $passes; is_menu_band = $isMenu })
        if ($menuRun -ge 4 -and -not $openXrMenuSeen) {
            $openXrMenuSeen = $true
            Write-Output 'OpenXR startup menu gate passed: four distinct per-second samples in the measured menu band'
        }
    }
    if (-not $OpenXRStartupMenuOnly -and $isMenu -and $menuRun -ge 4 -and -not $gameplaySeen -and
        $injections -lt $MaxInputInjections -and ($at - $lastInjectAt) -ge 20.0) {
        $target = $at
        if ($OpenXR) {
            # Reuse only Move-button events from the committed test input. Head and pad pose commands
            # are intentionally excluded so OpenXR tracking remains authoritative.
            $scriptText = Format-OpenXRMoveScript -Events $openXrMoveEvents -BaseTime $target
        } else {
            # The M2 confirm sweep, shifted onto the detected menu. This is the unchanged default
            # path with synthetic pose, pad, cross/options and Move events.
            $scriptText = @(
                '0 100000 head=0,0,0,0,0,0 pad=0,-0.17,-0.5,0,0,0',
                ('{0:0.0} 100000 pad=0.30,-0.12,-0.5,0,0,0' -f ($target + 1.0)),
                ('{0:0.0} 0.4 cross' -f ($target + 2.0)),
                ('{0:0.0} 0.4 options' -f ($target + 4.0)),
                ('{0:0.0} 0.4 move1=2,255' -f ($target + 6.0)),
                ('{0:0.0} 0.4 move0=6,255' -f ($target + 8.0)),
                ('{0:0.0} 0.4 move1=6,255' -f ($target + 10.0)),
                ('{0:0.0} 0.4 move0=2,255' -f ($target + 12.0)),
                ('{0:0.0} 0.4 cross' -f ($target + 14.0)),
                ('{0:0.0} 0.4 move1=6,255' -f ($target + 16.0)),
                ('{0:0.0} 0.4 move0=6,255' -f ($target + 18.0))
            ) -join "`r`n"
        }
        Invoke-OpenXRGuard 'write' $envMap['SHADPS4_LIVE_INPUT']
        [IO.File]::WriteAllText($envMap['SHADPS4_LIVE_INPUT'], $scriptText + "`r`n")
        $openXrInjectedScriptPath = $envMap['SHADPS4_LIVE_INPUT']
        $openXrInjectedScriptSha256 = Get-Sha256 $openXrInjectedScriptPath
        $openXrInjectionAtSeconds = $target
        $injections++
        $lastInjectAt = $at
        $menuRun = 0
        $inputPathLabel = if ($OpenXR) { 'OpenXR-gated Move input' } else { 'menu-gated input' }
        Write-Output ("{0} injected at {1:N1} s (injection {2})" -f $inputPathLabel, $target, $injections)
    }
}
$timedOut = -not $p.HasExited
if ($timedOut) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Write-Output "the emulator did not exit within $TimeoutSeconds s; stopped"
}
$p.WaitForExit()
$exitCode = $p.ExitCode
$ranSeconds = ((Get-Date) - $started).TotalSeconds

if (-not (Test-Path -LiteralPath $log)) {
    Write-Failure "the run produced no log at $log"
}
$logCopy = Join-Path $Out 'shad_log.txt'
Invoke-OpenXRGuard 'copy_dest' $logCopy
Copy-Item -LiteralPath $log -Destination $logCopy -Force
$sourceLogSha = Get-Sha256 $log
$logSha = Get-Sha256 $logCopy
$logCopyMatchesSource = $sourceLogSha.Equals($logSha, [StringComparison]::OrdinalIgnoreCase)
$inputScriptSha256 = if ($OpenXRStartupMenuOnly) { $null } else { Get-Sha256 $InputScript }
$openXrLogEvidence = if ($OpenXR) { Get-OpenXRLogEvidence $logCopy } else { $null }
if ($OpenXRStartupMenuOnly -and $null -ne $openXrLogEvidence) {
    $openXrLogEvidence['startup_menu'] = [ordered]@{
        reached = $openXrMenuSeen
        stable_menu_sample_count = @($openXrMenuSamples.ToArray() | Where-Object { $_.is_menu_band }).Count
        samples = @($openXrMenuSamples.ToArray())
        contract = 'four distinct per-second frame-stats samples in the measured menu band; no SaveData source or scripted input'
    }
}
$pinnedOpenXrImageSha256 = '8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75'
$rhiProbeEvidence = if ($OpenXR -and $RhiProbeMode -eq 'on') {
    Get-DeracineRhiProbeEvidence -Paths @($stdout, $stderr, $log) -PinnedImageSha256 $pinnedOpenXrImageSha256
} else { $null }
if ($OpenXR -and $RhiProbeMode -eq 'on' -and -not $logCopyMatchesSource) {
    $rhiProbeEvidence.schema_valid = $false
    $rhiProbeEvidence.valid = $false
    $rhiProbeEvidence.errors = @($rhiProbeEvidence.errors) + @('captured shad_log.txt copy SHA-256 differs from the in-lab source log')
}
$probeLogSinks = if ($OpenXR) {
    @(
        [ordered]@{ channel = 'stdout'; path = $stdout; sha256 = Get-Sha256 $stdout; bytes = (Get-Item -LiteralPath $stdout).Length },
        [ordered]@{ channel = 'stderr'; path = $stderr; sha256 = Get-Sha256 $stderr; bytes = (Get-Item -LiteralPath $stderr).Length },
        [ordered]@{
            channel = 'shad_log'
            source_path = $log
            captured_copy_path = $logCopy
            source_sha256 = $sourceLogSha
            captured_copy_sha256 = $logSha
            bytes = (Get-Item -LiteralPath $log).Length
            copy_matches_source = $logCopyMatchesSource
        }
    )
} else { @() }

# --- verdict ----------------------------------------------------------------------------------
if ($OpenXRStartupMenuOnly) {
    $startupReasons = [System.Collections.Generic.List[string]]::new()
    if (-not $openXrSessionSeen) { $startupReasons.Add('no "Session with the headset made" marker was observed') }
    if (-not $openXrFrameSeen) { $startupReasons.Add('no Vulkan HMD frame/eye marker was observed') }
    if (-not $openXrMenuSeen) { $startupReasons.Add('four distinct menu-band frame-stats samples were not observed') }
    $startupValid = ($startupReasons.Count -eq 0)
    $result = @{
        code = if ($startupValid) { 0 } else { 2 }
        verdict = [pscustomobject]@{
            valid = $startupValid
            gate = 'openxr-startup-menu-only'
            reasons = @($startupReasons.ToArray())
        }
    }
} else {
    $result = Invoke-Gate $logCopy $saveSrc
}
$verdict = $result.verdict
if ($OpenXR -and $RhiProbeMode -eq 'on' -and -not $rhiProbeEvidence.valid) {
    $verdict.valid = $false
    $probeReason = if (-not $rhiProbeEvidence.schema_valid) {
        'OpenXR RHI probe schema validation failed; see openxr.rhi_probe.errors.'
    } elseif (-not $rhiProbeEvidence.trace_observed) {
        'OpenXR RHI probe emitted no event records; no caller trace is claimed.'
    } else {
        'OpenXR RHI probe evidence is invalid; see openxr.rhi_probe.errors.'
    }
    if ($verdict.PSObject.Properties['reasons']) {
        $verdict.reasons = @($verdict.reasons) + @($probeReason)
    } else {
        $verdict | Add-Member -NotePropertyName reasons -NotePropertyValue @($probeReason) -Force
    }
    $result.code = 2
}
if ($OpenXR -and -not $logCopyMatchesSource) {
    $verdict.valid = $false
    $copyReason = 'captured shad_log.txt copy SHA-256 differs from the in-lab source log'
    if ($verdict.PSObject.Properties['reasons']) {
        $verdict.reasons = @($verdict.reasons) + @($copyReason)
    } else {
        $verdict | Add-Member -NotePropertyName reasons -NotePropertyValue @($copyReason) -Force
    }
    $result.code = 2
}
$limitations = if ($OpenXRStartupMenuOnly) {
    @(
        'Startup/menu-only cell: no SaveData source is read, copied, or required; the template user/home tree is excluded.',
        'No scripted or Move input is used; menu reach is inferred from four distinct per-second frame-stats samples in the measured menu band.',
        'OpenXR session activity is claimed only when the headset-session and Vulkan HMD-frame log markers are present.',
        'PnP enumeration and session creation alone do not prove positive controller tracking; see openxr.controller_tracking_note.',
        'This startup/menu cell is not a gameplay or comparable M2 headless FPS benchmark.'
    )
} elseif ($OpenXR) {
    @(
        'OpenXR session activity is claimed only when the headset-session and Vulkan HMD-frame log markers are present.',
        'PnP enumeration and session creation alone do not prove positive controller tracking; see openxr.controller_tracking_note.',
        'No OS GUI or window automation is used; physical menu navigation may still be required if the gated test input cannot advance the title.',
        'This OpenXR cell is a distinct probe, not a comparable M2 headless FPS benchmark.',
        'The save is game/user data; the manifest pins the copied source bytes but the private lab may change them.'
    )
} else {
    @(
        'No physical headset: no latency, comfort, distortion or compositor claims.',
        'unique FPS is the emulator''s own rendered-frame series, not compositor cadence.',
        'The save is game/user data; the manifest pins it but the files are not committed.',
        'The band bounds draws tightly and Mpx/passes loosely because renderer state changes them at the same scene; re-derive it only from a live cell.'
    )
}
$document = [ordered]@{
    schema_version  = 1
    label           = $Label
    created_utc     = $started.ToUniversalTime().ToString('o')
    mode            = if ($OpenXRStartupMenuOnly) {
        'launch-openxr-startup-menu-probe'
    } elseif ($OpenXR) {
        if ($EnableOpenXRTestInput) { 'launch-openxr-gameplay-probe' } else { 'launch-openxr-session-probe' }
    } else { 'launch' }
    scenario        = [ordered]@{
        id      = if ($OpenXRStartupMenuOnly) { 'deracine-openxr-startup-menu-probe' } elseif ($OpenXR) { 'deracine-openxr-gated-probe' } else { 'deracine-headless-gameplay-300-msaa2-vblank120' }
        profile = if ($OpenXRStartupMenuOnly) { $null } else { $Profile }
        gate    = if ($OpenXRStartupMenuOnly) {
            [ordered]@{ kind = 'measured_menu_band'; consecutive_samples = 4; distinct_sample_interval = 'per second'; min_draws = 350; max_draws = 800; min_mpx = 300; max_mpx = 750 }
        } else { [ordered]@{ min_draws = 800; min_mpx = 450; consecutive_samples = 2 } }
        band    = if ($OpenXRStartupMenuOnly) { $null } else { [ordered]@{ draws = '1500-2300'; mpx = '550-750'; passes = '35-55' } }
        input   = if ($OpenXRStartupMenuOnly -or ($OpenXR -and -not $EnableOpenXRTestInput)) { $null } else { $InputScript }
        input_source_sha256 = $inputScriptSha256
        menu_gated_input = [ordered]@{
            enabled         = $useMenuInput
            injection_count = $injections
            max_injections  = if ($OpenXRStartupMenuOnly -or ($OpenXR -and -not $EnableOpenXRTestInput)) { 0 } else { $MaxInputInjections }
            openxr_test_input_enabled = ($OpenXR -and $EnableOpenXRTestInput)
            openxr_gate_requires = if ($OpenXR) { @('Session with the headset made', 'HMD frame <n> eye <0|1>') } else { @() }
            openxr_script_policy = if ($OpenXRStartupMenuOnly) { 'none; no scripted or Move input' } elseif ($OpenXR) { 'existing input script Move-button events only; head/pad pose commands excluded' } else { 'M2 menu-gated input' }
            press_schedule  = if ($OpenXRStartupMenuOnly) { 'none' } elseif ($OpenXR) { 'existing Move event schedule shifted to the first stable menu, after OpenXR session/frame proof' } else { 'pose +1 s; Move trigger at +14/+17/+20 s from the detected menu' }
        }
        save = if ($OpenXRStartupMenuOnly) {
            [ordered]@{ dependency = 'none'; source = $null; copied = $false; files = @(); note = 'no SaveData source was read or copied; template user/home excluded' }
        } else {
            [ordered]@{ dependency = 'manifest_verified'; source = (Resolve-Path $SavesFrom).Path; slot = $manifest.slot; stage = $manifest.stage; files = $saveHashes }
        }
    }
    source          = [ordered]@{
        repo       = $Repo
        branch     = if ($branch) { $branch.Trim() } else { 'detached' }
        head_sha   = if ($head) { $head.Trim() } else { '' }
        dirty_files = @($dirty)
    }
    executable      = [ordered]@{ path = (Resolve-Path $Exe).Path; sha256 = $exeSha; bytes = (Get-Item $Exe).Length }
    game            = [ordered]@{ eboot = (Resolve-Path $Eboot).Path; sha256 = $ebootSha }
    env             = $envMap
    openxr          = if ($OpenXR) {
        [ordered]@{
            requested = $true
            startup_menu_only = [bool]$OpenXRStartupMenuOnly
            rhi_probe_mode = $RhiProbeMode
            rhi_probe_control = if ($RhiProbeMode -eq 'on') { 'SHADPS4_DERACINE_RHI_PROBE=1' } else { 'unset' }
            rhi_probe_schema = if ($RhiProbeMode -eq 'on') { 'DERACINE_RHI_PROBE_INSTALL+DERACINE_RHI_PROBE_EVENT/v2+DERACINE_GNM_DRAW_INDEX' } else { $null }
            rhi_probe_log_sinks = $probeLogSinks
            rhi_probe = $rhiProbeEvidence
            startup_menu = if ($OpenXRStartupMenuOnly) { $openXrLogEvidence.startup_menu } else { $null }
            session_frame_gate_passed = $openXrInputGatePassed
            session_created = $openXrLogEvidence.session_created
            hmd_frame_log_count = $openXrLogEvidence.hmd_frame_log_count
            session_lines = $openXrLogEvidence.session_lines
            first_hmd_frame_lines = $openXrLogEvidence.first_hmd_frame_lines
            controller_related_lines = $openXrLogEvidence.controller_related_lines
            controller_tracking_samples = $openXrLogEvidence.controller_tracking_samples
            controller_tracking_note = $openXrLogEvidence.controller_tracking_note
            test_input_enabled = [bool]$EnableOpenXRTestInput
            test_input_injection_cap = if ($EnableOpenXRTestInput) { 1 } else { 0 }
            injected_script_path = $openXrInjectedScriptPath
            injected_script_sha256 = $openXrInjectedScriptSha256
            injected_at_seconds = $openXrInjectionAtSeconds
            move_event_cap = 10
            move_event_count = if ($EnableOpenXRTestInput) { $openXrMoveEvents.Count } else { 0 }
            move_events = if ($EnableOpenXRTestInput) { @($openXrMoveEvents) } else { @() }
            move_script_sha256 = if ($EnableOpenXRTestInput) { $inputScriptSha256 } else { $null }
        }
    } else { $null }
    log             = [ordered]@{
        path = $logCopy
        sha256 = $logSha
        source_path = $log
        source_sha256 = $sourceLogSha
        copy_matches_source = $logCopyMatchesSource
    }
    shutdown        = [ordered]@{ exit_code = $exitCode; seconds = [math]::Round($ranSeconds, 1); timed_out = $timedOut }
    verdict         = $verdict
    reproducibility = if ($OpenXR) {
        'OpenXR probe is a distinct path; no reproducibility claim until repeated on the pinned A3 binary and runtime.'
    } else {
        'two-lab: M2 gameplay entry reproduced in two independently constructed labs (m2-off-fixed-1, m3-portable-2)'
    }
    limitations     = $limitations
}
$runJson = Join-Path $Out 'run.json'
Invoke-OpenXRGuard 'write' $runJson
$document | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $runJson -Encoding utf8

Write-Output ''
$verdict | ConvertTo-Json -Depth 10
Write-Output "run written: $runJson"
if ($verdict.valid -eq $true) {
    if ($OpenXRStartupMenuOnly) {
        Write-Output 'VALID OpenXR startup/menu cell; no SaveData or scripted input was used.'
    } elseif ($OpenXR) {
        Write-Output 'VALID gameplay gate in the OpenXR probe; do not treat its FPS as a comparable M2 benchmark.'
    } else {
        Write-Output ("VALID gameplay cell: unique rendered FPS {0:N2}" -f [double]$verdict.unique_fps)
    }
    exit 0
}
Write-Output 'INVALID: the run did not satisfy its declared cell contract (see verdict.reasons)'
exit 2
