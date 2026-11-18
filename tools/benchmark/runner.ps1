# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# One pinned, headless Déraciné classroom/menu benchmark session.
#
# The session is defined by: the executable under test, the private lab runtime, the private game
# shadow, the pinned guest/presentation/vblank/autopress configuration, the frame-statistics
# timing series, and a screenshot of the classroom matched against a reviewed reference. See
# tools/benchmark/README.md. Anything that changes those makes runs incomparable.
#
# Fail closed: a run that does not reach the scene, does not produce a matching screenshot, does
# not collect 80 valid 0.5 s windows or loses a protected file writes INVALID.txt and no run.json.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Out,
    # The explicitly designated private game shadow. Required: the runner writes the pinned
    # engine.ini into it, so it must never be the active installation or a shared directory.
    [Parameter(Mandatory)][string]$ShadowRoot,
    [string]$Worktree = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$Lab = '',
    [string]$Game = '',
    [string]$Label = '',
    [int]$Vblank = 60,
    [double]$Warmup = 10.0,
    [double]$Seconds = 40.0,
    [double]$StatsEvery = 0.5,
    [double]$VrScale = 0.5,
    [string]$Autopress = '1',
    [int]$SceneMinDraws = 400,
    [double]$SceneMinMpx = 500,
    [int]$SceneSamples = 2,
    [int]$DumpWidth = 1280,
    [switch]$Capture,
    [int[]]$CaptureOffsets = @(12, 25),
    [int]$TimeoutSeconds = 320,
    [string]$Python = 'python',
    [string]$RdocCapture = '',
    [string]$RenderDocCaptureDir = '',
    [string]$RenderDocCmd = 'C:\Program Files\RenderDoc\renderdoccmd.exe',
    [string]$BuiltFromCommit = ''
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture
$script:emulator = $null
$script:runStart = $null
if ([string]::IsNullOrWhiteSpace($Game)) { $Game = $ShadowRoot }

# --- positive process ownership (no name-based sweep) ----------------------------------------
. (Join-Path $PSScriptRoot 'process_ownership.ps1')
$script:ownedProcesses = [System.Collections.Generic.List[object]]::new()

# Defined before the trap: a trap cannot call a function that is defined later in the script body.
function Stop-LabGame {
    try {
        $result = Stop-OwnedProcesses -Records $script:ownedProcesses -TableProvider { Get-ProcessTable }
        foreach ($entry in @($result.Stopped)) {
            Write-Output ('cleanup: stopped pid {0} ({1})' -f $entry.Id, $entry.Role)
        }
        foreach ($entry in @($result.Skipped)) {
            Write-Output ('cleanup: skipped pid {0} ({1}): {2}' -f $entry.Id, $entry.Code, $entry.Reason)
        }
        if ($result.Ambiguous) {
            Write-Output 'cleanup: the owned process set could not be fully resolved; see the skips above'
        }
    } catch {
        Write-Output ('cleanup failed: {0}' -f $_.Exception.Message)
    }
}

trap {
    Write-Output ("runner failed: {0}" -f $_.Exception.Message)
    if ($_.InvocationInfo -and $_.InvocationInfo.PositionMessage) {
        Write-Output $_.InvocationInfo.PositionMessage
    }
    Stop-LabGame
    exit 4
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Round3([double]$Value) {
    return [math]::Round($Value, 3)
}

function Write-JsonFile([string]$Path, $Object) {
    $json = $Object | ConvertTo-Json -Depth 24
    [IO.File]::WriteAllText($Path, $json + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
}

function Get-Percentile([double[]]$Values, [double]$Fraction) {
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) { throw 'percentile of an empty series' }
    if ($ordered.Count -eq 1) { return $ordered[0] }
    $position = ($ordered.Count - 1) * $Fraction
    $lower = [math]::Floor($position)
    $upper = [math]::Ceiling($position)
    if ($lower -eq $upper) { return $ordered[[int]$lower] }
    return $ordered[[int]$lower] + ($ordered[[int]$upper] - $ordered[[int]$lower]) * ($position - $lower)
}

$Worktree = [IO.Path]::GetFullPath($Worktree)
if (-not $Lab) { $Lab = Join-Path $Worktree '_lab\pc-vr' }
$Lab = [IO.Path]::GetFullPath($Lab)
$Out = [IO.Path]::GetFullPath($Out)
$privateRoot = [IO.Path]::GetFullPath((Join-Path $Worktree '_out'))
if (-not $Out.StartsWith($privateRoot + [IO.Path]::DirectorySeparatorChar)) {
    throw "run directory '$Out' is not inside the private output root '$privateRoot'"
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$Exe = [IO.Path]::GetFullPath($Exe)
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "executable not found: $Exe" }
if (-not (Test-Path -LiteralPath (Join-Path $Lab 'user\config.json'))) {
    throw "private lab runtime is missing: $Lab"
}
$gameEboot = Join-Path $Game 'eboot.bin'
if (-not (Test-Path -LiteralPath $gameEboot -PathType Leaf)) { throw "game shadow eboot not found: $gameEboot" }
$runName = Split-Path $Out -Leaf
if (-not $Label) { $Label = $runName }

# --- preflight and the guard ----------------------------------------------------------------
$preflightPath = Join-Path $Out 'preflight.json'
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'preflight.ps1') -Worktree $Worktree `
    -Out $preflightPath -Game $Game -ShadowRoot $ShadowRoot -Python $Python
if ($LASTEXITCODE -ne 0) { throw "preflight failed with exit $LASTEXITCODE" }
$preDoc = Get-Content -LiteralPath $preflightPath -Raw | ConvertFrom-Json

$guardConfig = Join-Path $Out 'guard-roots.json'
$guard = [ordered]@{
    schema_version = 1
    allow_write = @($privateRoot, $Lab, $Game)
    allow_read  = @($preDoc.guard.allow_read) + @($Game)
    deny        = @($preDoc.guard.deny)
}
Write-JsonFile $guardConfig $guard

function Invoke-Guard([string]$Op, [string]$Path) {
    $output = & $Python (Join-Path $PSScriptRoot 'path_guard.py') --config $guardConfig `
        --op $Op --path $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "path guard refused '$Op' on '$Path': $($output -join ' ')"
    }
}

function Fail-Run([string]$Reason) {
    Stop-LabGame
    # Keep the evidence of a failed session: without the log, the next attempt has to guess.
    try {
        $labLogPath = Join-Path $Lab 'user\log\shad_log.txt'
        if (Test-Path -LiteralPath $labLogPath) {
            Copy-Item -LiteralPath $labLogPath -Destination (Join-Path $Out 'shad_log.txt') -Force `
                -ErrorAction SilentlyContinue
        }
    } catch { }
    Remove-Item -LiteralPath (Join-Path $Out 'run.json') -Force -ErrorAction SilentlyContinue
    $text = @(
        "INVALID RUN - $Reason",
        "run: $Out",
        "label: $Label",
        "exe: $Exe",
        "exe_sha256: $(Get-Sha256 $Exe)",
        "started_utc: $startedUtc",
        "autopress: $Autopress",
        "vblank: $Vblank",
        "vr_scale: $VrScale",
        "warmup_s: $Warmup",
        "window_s: $Seconds"
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $Out 'INVALID.txt'), $text + "`r`n",
        (New-Object System.Text.UTF8Encoding($false)))
    Write-Output "INVALID: $Reason"
    exit 2
}

$startedUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

# --- provenance of the source and the executable --------------------------------------------
$head = (git -C $Worktree rev-parse HEAD 2>$null)
$branch = (git -C $Worktree rev-parse --abbrev-ref HEAD 2>$null)
$dirtyLines = @(git -C $Worktree status --porcelain 2>$null)
$dirtyBytes = [Text.Encoding]::UTF8.GetBytes(($dirtyLines -join "`n"))
$dirtySha = if ($dirtyLines.Count -gt 0) {
    ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($dirtyBytes)) `
        -replace '-', '').ToLowerInvariant()
} else { $null }
$exeHash = Get-Sha256 $Exe
$exeBytes = (Get-Item -LiteralPath $Exe).Length

# --- host ------------------------------------------------------------------------------------
$video = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)
$gpu = if ($video.Count -gt 0) { ($video | ForEach-Object { $_.Name }) -join '; ' } else { 'unidentified' }
$driver = if ($video.Count -gt 0) {
    ($video | ForEach-Object { "$($_.Name) $($_.DriverVersion)" }) -join '; '
} else { 'unidentified' }
$cpu = (Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1).Name
$os = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).Caption
$vrProcesses = @(Get-Process vrserver, vrmonitor -ErrorAction SilentlyContinue)
$xrRuntime = if ($vrProcesses.Count -gt 0) {
    'SteamVR (' + (($vrProcesses | ForEach-Object { $_.ProcessName }) -join ', ') + ' running)'
} else { 'no OpenXR runtime process detected' }

# --- the lab session -------------------------------------------------------------------------
$labExe = Join-Path $Lab 'shadps4.exe'
$labLog = Join-Path $Lab 'user\log\shad_log.txt'
$labConfig = Join-Path $Lab 'user\config.json'
$labSettings = Join-Path $Lab 'settings.txt'
$labUser = Join-Path $Lab 'user'
$dumpFlag = Join-Path $labUser 'dump_render_targets'
$dumpRoot = Join-Path $labUser 'rt_dump'
$engineIni = Join-Path $Game 'gameproject\saved\config\ps4\engine.ini'

# --- private game-shadow proof, before anything is written into the game shadow ---------------
# The shadow checker resolves junctions and reparse points, rejects the active installation,
# shared directories, protected labs and path escapes, and verifies the eboot against the
# shadow's own manifest. It fails closed; preflight records the same proof.
$shadowEvidence = Join-Path $Out 'shadow.json'
& $Python (Join-Path $PSScriptRoot 'game_shadow.py') check --game $Game --shadow-root $ShadowRoot `
    --eboot $gameEboot --json 2>&1 | Set-Content -LiteralPath $shadowEvidence -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    throw "the selected game path is not a private benchmark shadow; refusing to write engine.ini (see $shadowEvidence)"
}
$engineIniBefore = if (Test-Path -LiteralPath $engineIni) { Get-Sha256 $engineIni } else { $null }

Invoke-Guard 'copy_dest' $labExe
Invoke-Guard 'write' $labSettings
Invoke-Guard 'write' $labConfig
Invoke-Guard 'write' $engineIni
Invoke-Guard 'delete' $labLog
Invoke-Guard 'delete' $dumpFlag

# Pinned guest configuration, written into the private game shadow.
[IO.File]::WriteAllText($engineIni, "[SystemSettings]`r`nr.ScreenPercentage=300`r`nr.MSAACount=2`r`n",
    (New-Object System.Text.UTF8Encoding($false)))

# Pinned settings, for the record: the emulator is launched directly with the env the launcher
# would derive, so these lines are evidence, not the operative configuration.
$settingsText = @(
    '# Written by tools/benchmark/runner.ps1 for one benchmark session.',
    'fps=120', 'resolution=2880', 'dynamic=0', 'fov=100', 'sharpen=0.25', 'aniso=16',
    'antialias=1', 'desktop_view=stereo', 'desktop_crop=0', 'readbacks=relaxed',
    'readback_linear_images=1', 'copy_gpu_buffers=0', 'move_pitch=-45', 'move_y=0.000',
    'move_z=-0.135', 'swap_hands=1', 'hands=1', 'language=en-US', "game=$gameEboot",
    'render_percent=300', 'menu=0', 'pause=0', 'msaa=2'
) -join "`r`n"
[IO.File]::WriteAllText($labSettings, $settingsText + "`r`n",
    (New-Object System.Text.UTF8Encoding($false)))

$config = Get-Content -LiteralPath $labConfig -Raw | ConvertFrom-Json
$config.Log.type = 'file'
$config.Log.filter = '*:Info'
$config.Log.sync = $true
$config.Log.skip_duplicate = $true
$config.GPU.readbacks_mode = 1
$config.GPU.copy_gpu_buffers = $false
$config.GPU.readback_linear_images_enabled = $true
$config.GPU.vblank_frequency = $Vblank
$config.Vulkan.pipeline_cache_enabled = $false
Write-JsonFile $labConfig $config
$configActual = Get-Content -LiteralPath $labConfig -Raw | ConvertFrom-Json
$vblankActual = [int]$configActual.GPU.vblank_frequency

Copy-Item -LiteralPath $Exe -Destination $labExe -Force
if ((Get-Sha256 $labExe) -ne $exeHash) { Fail-Run 'the executable was not copied into the lab correctly' }

# A clean log and clean dump directory make this session's evidence unambiguous.
if (Test-Path -LiteralPath $labLog) {
    $deadline = (Get-Date).AddSeconds(15)
    while ((Test-Path -LiteralPath $labLog) -and (Get-Date) -lt $deadline) {
        try { Remove-Item -LiteralPath $labLog -Force -ErrorAction Stop; break }
        catch { Start-Sleep -Milliseconds 300 }
    }
}
Get-ChildItem -LiteralPath $labUser -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
    ForEach-Object { Invoke-Guard 'delete' $_.FullName; Remove-Item -LiteralPath $_.FullName -Recurse -Force }
Invoke-Guard 'delete' $dumpFlag
Remove-Item -LiteralPath $dumpFlag -Force -ErrorAction SilentlyContinue

# A pipeline cache written under a previous executable, or after a device loss, can make a
# session fail for reasons unrelated to the build under test; the old harness cleared it too.
$gameCache = Join-Path $labUser 'cache\CUSA11919'
if (Test-Path -LiteralPath $gameCache) {
    Invoke-Guard 'delete' $gameCache
    Remove-Item -LiteralPath $gameCache -Recurse -Force -ErrorAction SilentlyContinue
}

$envMap = [ordered]@{
    'SHADPS4_TITLE_EYE_WIDTH'    = '2880'
    'SHADPS4_TITLE_RESOLUTION'   = '6'
    'SHADPS4_VR_SHARPEN'         = '0.25'
    'SHADPS4_FORCE_ANISO'        = '16'
    'SHADPS4_VR_DESKTOP_VIEW'    = 'stereo'
    'SHADPS4_VR_DESKTOP_CROP'    = '0'
    'SHADPS4_VR_FOV_OF'          = 'headset'
    'SHADPS4_MAX_MSAA'           = '2'
    'SHADPS4_VR_REFRESH_RATE'    = '120'
    'SHADPS4_VR_FPS_CAP'         = '120'
    'SHADPS4_VR_FASTEST_PACE'    = '1'
    'SHADPS4_XR_PAUSE'           = '0'
    'SHADPS4_XR_WAIT'            = '60'
    'SHADPS4_CONSOLE_LANGUAGE'   = 'en-US'
    'SHADPS4_MOVE_PITCH'         = '-45'
    'SHADPS4_MOVE_Y'             = '0.000'
    'SHADPS4_MOVE_Z'             = '-0.135'
    'SHADPS4_SWAP_HANDS'         = '1'
    'SHADPS4_VR_SCALE'           = $VrScale.ToString($inv)
    'SHADPS4_FRAME_STATS'        = '1'
    'SHADPS4_FRAME_STATS_EVERY'  = $StatsEvery.ToString($inv)
    'SHADPS4_DBG_AUTOPRESS'      = $Autopress
}
if ($Capture) { $envMap['SHADPS4_DUMP_WIDTH'] = "$DumpWidth" }
if ($RdocCapture) { $envMap['SHADPS4_DBG_RDOC_CAPTURE'] = $RdocCapture }
foreach ($key in $envMap.Keys) { Set-Item -Path ('Env:' + $key) -Value $envMap[$key] }

$gameArgs = @('-g', $gameEboot)
$patchFile = Join-Path $labUser 'patches\CUSA11919.xml'
if (Test-Path -LiteralPath $patchFile) { $gameArgs += @('-p', $patchFile) }

$consoleOut = Join-Path $Out 'console.txt'
$consoleErr = Join-Path $Out 'console-errors.txt'
$runStart = Get-Date
$script:runStart = $runStart
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
if ($RenderDocCaptureDir) {
    $RenderDocCaptureDir = [IO.Path]::GetFullPath($RenderDocCaptureDir)
    if (-not $RenderDocCaptureDir.StartsWith($Out + [IO.Path]::DirectorySeparatorChar)) {
        throw "RenderDoc capture directory must be inside the run directory: $RenderDocCaptureDir"
    }
    Invoke-Guard 'mkdir' $RenderDocCaptureDir
    New-Item -ItemType Directory -Force -Path $RenderDocCaptureDir | Out-Null
    $captureTemplate = Join-Path $RenderDocCaptureDir 'shadps4-capture'
    $launchArgs = @('capture', '-c', $captureTemplate, '-w', '-d', $Lab, $labExe) + $gameArgs
    $emulator = Start-Process -FilePath $RenderDocCmd -ArgumentList $launchArgs -WorkingDirectory $Lab `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $consoleOut -RedirectStandardError $consoleErr
} else {
    $emulator = Start-Process -FilePath $labExe -ArgumentList $gameArgs -WorkingDirectory $Lab `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $consoleOut -RedirectStandardError $consoleErr
}
$script:emulator = $emulator
# Record exactly what this run launched. A RenderDoc run's emulator is the wrapper's child, which
# the ownership module resolves from the live parent links; never a process matched by name.
$launchRole = if ($RenderDocCaptureDir) { 'wrapper' } else { 'emulator' }
$launchExe = if ($RenderDocCaptureDir) { $RenderDocCmd } else { $labExe }
Register-LaunchedProcess -Process $emulator -Role $launchRole -ExpectedPath $launchExe `
    -Into $script:ownedProcesses | Out-Null

# --- log following --------------------------------------------------------------------------
$logPos = 0
$logLines = New-Object System.Collections.Generic.List[string]
$samples = New-Object System.Collections.Generic.List[object]
$statsCursor = 0
$script:incompleteStats = 0

function Read-NewLogLines {
    if (-not (Test-Path -LiteralPath $labLog)) { return }
    $stream = $null
    try { $stream = [IO.File]::Open($labLog, 'Open', 'Read', 'ReadWrite') } catch { return }
    try {
        if ($stream.Length -lt $script:logPos) { $script:logPos = 0 }
        [void]$stream.Seek($script:logPos, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream)
        $line = $reader.ReadLine()
        while ($null -ne $line) {
            $script:logLines.Add($line)
            $line = $reader.ReadLine()
        }
        $script:logPos = $stream.Position
    } finally { $stream.Dispose() }
}

$statsRegex = [regex]'frame stats at\s+(?<at>\d+)\s+s(?:\s+\[(?<ab>[A-Z+]+)\])?:\s+(?<fps>[0-9.]+)\s+fps,\s+per frame\s+(?<passes>[0-9.]+)\s+passes\s+\((?<loading>[0-9.]+)\s+loading their targets,\s+(?<mpx>[0-9.]+)\s+Mpx\),\s+(?<draws>[0-9.]+)\s+draws,\s+(?<dispatches>[0-9.]+)\s+dispatches,\s+(?<submits>[0-9.]+)\s+submits(?:,\s+GPU busy\s+(?<busy>[0-9]+)%(?:\s+at\s+(?<mhz>[0-9]+)\s+MHz)?)?(?:,\s+(?<frames>[0-9]+)\s+frames)?'

function Update-Samples {
    for ($i = $script:statsCursor; $i -lt $script:logLines.Count; $i++) {
        $match = $statsRegex.Match($script:logLines[$i])
        if (-not $match.Success) {
            # A line that announced a stats report but could not be read whole is counted, not
            # silently dropped: the manifest should say when the series had holes.
            if ($script:logLines[$i] -match 'frame stats at') { $script:incompleteStats++ }
            continue
        }
        # A stats line that was cut short (no trailing frame count) is not a sample: skipping it
        # keeps the frame-count-derived duration honest instead of mixing in an unverifiable row.
        if (-not $match.Groups['frames'].Success) { $script:incompleteStats++; continue }
        $script:samples.Add([ordered]@{
            sample     = $script:samples.Count
            at_s       = [double]$match.Groups['at'].Value
            fps        = [double]$match.Groups['fps'].Value
            frames     = [int]$match.Groups['frames'].Value
            window_avg_frame_ms = Round3 (1000.0 / [double]$match.Groups['fps'].Value)
            draws      = [double]$match.Groups['draws'].Value
            mpx        = [double]$match.Groups['mpx'].Value
            passes     = [double]$match.Groups['passes'].Value
            loading    = [double]$match.Groups['loading'].Value
            dispatches = [double]$match.Groups['dispatches'].Value
            submits    = [double]$match.Groups['submits'].Value
            gpu_busy   = $(if ($match.Groups['busy'].Success) { [double]$match.Groups['busy'].Value } else { -1.0 })
            gpu_mhz    = $(if ($match.Groups['mhz'].Success) { [double]$match.Groups['mhz'].Value } else { -1.0 })
            in_window  = $false
        })
    }
    $script:statsCursor = $script:logLines.Count
}

function Find-SceneAnchor {
    $run = 0
    for ($i = 0; $i -lt $samples.Count; $i++) {
        $s = $samples[$i]
        if ($s.draws -ge $SceneMinDraws -and $s.mpx -ge $SceneMinMpx -and $s.fps -gt 1.0) {
            $run++
            if ($run -ge $SceneSamples) { return $i - $SceneSamples + 1 }
        } else { $run = 0 }
    }
    return -1
}

function Get-SceneSimilarity([string]$Image) {
    $text = & $Python (Join-Path $PSScriptRoot 'scene_signature.py') compare $Image `
        --reference (Join-Path $PSScriptRoot 'reference\scene-classroom-menu.json') 2>&1
    if ($text) {
        try { return [double]($text | ConvertFrom-Json).similarity } catch { }
    }
    return 0.0
}

# --- reach the scene ------------------------------------------------------------------------
$anchorIndex = -1
$anchorElapsed = 0.0
while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    Read-NewLogLines
    Update-Samples
    $anchorIndex = Find-SceneAnchor
    if ($anchorIndex -ge 0) { $anchorElapsed = $stopwatch.Elapsed.TotalSeconds; break }
    if ($emulator.HasExited) { Fail-Run "the emulator ended before the classroom scene (exit $($emulator.ExitCode))" }
    Start-Sleep -Milliseconds 250
}
if ($anchorIndex -lt 0) { Fail-Run 'the classroom scene was not reached before the timeout' }
$anchor = $samples[$anchorIndex]
Write-Output ("scene anchor: sample #{0}, {1:N1} draws, {2:N1} Mpx, {3:N1} fps at {4:N1} s" -f `
        $anchor.sample, $anchor.draws, $anchor.mpx, $anchor.fps, $stopwatch.Elapsed.TotalSeconds)

# --- screenshot of the scene, retried at bounded offsets ------------------------------------
$sceneShot = $null
$shotTakenAt = 0.0
$shotSimilarity = 0.0
$shotWidth = 0
$shotHeight = 0
$shotSha = $null
$dumpEvidence = New-Object System.Collections.Generic.List[object]
if ($Capture) {
    foreach ($offset in @($CaptureOffsets | Sort-Object)) {
        $target = $anchorElapsed + $offset
        while ($stopwatch.Elapsed.TotalSeconds -lt $target) {
            Read-NewLogLines
            Update-Samples
            if ($emulator.HasExited) { Fail-Run 'the emulator ended while waiting for the scene screenshot' }
            Start-Sleep -Milliseconds 200
        }
        $beforeDirs = @()
        if (Test-Path -LiteralPath $dumpRoot) {
            $beforeDirs = @(Get-ChildItem -LiteralPath $dumpRoot -Directory | ForEach-Object { $_.Name })
        }
        Invoke-Guard 'write' $dumpFlag
        [IO.File]::WriteAllText($dumpFlag, "1`r`n")
        $dumpDir = $null
        $deadline = (Get-Date).AddSeconds(120)
        $stable = 0
        $lastCount = -1
        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $dumpRoot) {
                $newDirs = @(Get-ChildItem -LiteralPath $dumpRoot -Directory |
                    Where-Object { $beforeDirs -notcontains $_.Name } | Sort-Object Name)
                if ($newDirs.Count -gt 0) {
                    $candidate = $newDirs[-1]
                    $pngs = @(Get-ChildItem -LiteralPath $candidate.FullName -File -Filter '*.png')
                    if ($pngs.Count -gt 0 -and $pngs.Count -eq $lastCount) { $stable++ } else { $stable = 0 }
                    $lastCount = $pngs.Count
                    if ($stable -ge 3) { $dumpDir = $candidate; break }
                }
            }
            if ($emulator.HasExited) { Fail-Run 'the emulator ended while taking the scene screenshot' }
            Start-Sleep -Milliseconds 250
        }
        Invoke-Guard 'delete' $dumpFlag
        Remove-Item -LiteralPath $dumpFlag -Force -ErrorAction SilentlyContinue
        if ($null -eq $dumpDir) { Fail-Run 'the render-target dump for the scene screenshot did not appear' }

        $eyePattern = '*_5760x3240_R8G8B8A8Unorm_s1.png'
        $candidates = @(Get-ChildItem -LiteralPath $dumpDir.FullName -File -Filter $eyePattern |
            Where-Object { $_.Name -notlike '*_alpha.png' })
        if ($candidates.Count -eq 0) {
            $candidates = @(Get-ChildItem -LiteralPath $dumpDir.FullName -File -Filter '*R8G8B8A8Unorm_s1.png' |
                Where-Object { $_.Name -notlike '*_alpha.png' })
        }
        if ($candidates.Count -eq 0) {
            $candidates = @(Get-ChildItem -LiteralPath $dumpDir.FullName -File -Filter '*.png')
        }
        $best = $null
        $bestScore = -1.0
        foreach ($candidate in @($candidates | Sort-Object Length -Descending | Select-Object -First 4)) {
            $score = Get-SceneSimilarity $candidate.FullName
            if ($score -gt $bestScore) { $bestScore = $score; $best = $candidate }
        }
        $dumpEvidence.Add([ordered]@{
            offset_s        = $offset
            folder          = $dumpDir.Name
            best_image      = $(if ($best) { $best.Name } else { $null })
            best_similarity = Round3 $bestScore
            files           = @($candidates | Sort-Object Name | ForEach-Object {
                [ordered]@{ name = $_.Name; bytes = $_.Length; sha256 = Get-Sha256 $_.FullName }
            })
        })
        if ($best -and $bestScore -ge $shotSimilarity) {
            Invoke-Guard 'write' (Join-Path $Out 'scene.png')
            Copy-Item -LiteralPath $best.FullName -Destination (Join-Path $Out 'scene.png') -Force
            $sceneShot = Join-Path $Out 'scene.png'
            $shotSimilarity = $bestScore
            $shotTakenAt = $stopwatch.Elapsed.TotalSeconds
            Write-Output ("scene screenshot: {0} at anchor+{1} s (similarity {2:N4})" -f `
                    $best.Name, $offset, $bestScore)
        }
        if ($bestScore -ge 0.85) { break }
    }
    if ($sceneShot) {
        $shotSha = Get-Sha256 $sceneShot
        $sizeText = & $Python (Join-Path $PSScriptRoot 'scene_signature.py') size $sceneShot 2>&1
        if ($LASTEXITCODE -eq 0) {
            $size = $sizeText | ConvertFrom-Json
            $shotWidth = [int]$size.width
            $shotHeight = [int]$size.height
        }
    }
    if ($shotSimilarity -lt 0.85) {
        Write-Output ("scene screenshot did not match the reviewed reference (best similarity {0:N4})" -f $shotSimilarity)
    }
    if (Test-Path -LiteralPath (Join-Path $Out 'captures.json')) {
        Invoke-Guard 'delete' (Join-Path $Out 'captures.json')
        Remove-Item -LiteralPath (Join-Path $Out 'captures.json') -Force
    }
    Invoke-Guard 'write' (Join-Path $Out 'captures.json')
    Write-JsonFile (Join-Path $Out 'captures.json') $dumpEvidence.ToArray()
    if ($emulator.HasExited) { Fail-Run 'the emulator ended after the scene screenshot' }
}

# --- warmup and measured window -------------------------------------------------------------
$warmupTarget = $samples.Count + [int][math]::Round($Warmup / $StatsEvery)
$windowTarget = $warmupTarget + [int][math]::Round($Seconds / $StatsEvery)
$windowFirstObs = $null
$windowLastObs = $null
while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    Read-NewLogLines
    Update-Samples
    if ($null -eq $windowFirstObs -and $samples.Count -ge $warmupTarget) {
        $windowFirstObs = Get-Date
    }
    if ($null -ne $windowFirstObs -and $samples.Count -ge $windowTarget) {
        $windowLastObs = Get-Date
        break
    }
    if ($emulator.HasExited) { Fail-Run 'the emulator ended before the measured window completed' }
    Start-Sleep -Milliseconds 250
}
if ($null -eq $windowLastObs) { Fail-Run 'the measured window did not complete before the timeout' }
# A poll can see several new lines at once; samples that arrived after the window ended are not
# part of it and would break the "in-window rows are the tail" invariant.
if ($samples.Count -gt $windowTarget) {
    $samples.RemoveRange($windowTarget, $samples.Count - $windowTarget)
}
$samplesInWindow = $windowTarget - $warmupTarget
for ($i = $warmupTarget; $i -lt $windowTarget; $i++) { $samples[$i].in_window = $true }
$windowObserved = ($windowLastObs - $windowFirstObs).TotalSeconds + $StatsEvery

# --- stop and collect -----------------------------------------------------------------------
try { Stop-LabGame } catch { }
Start-Sleep -Milliseconds 1500
Read-NewLogLines
try { Read-NewLogLines } catch { }

Invoke-Guard 'copy_dest' (Join-Path $Out 'shad_log.txt')
Copy-Item -LiteralPath $labLog -Destination (Join-Path $Out 'shad_log.txt') -Force -ErrorAction SilentlyContinue
if (-not (Test-Path -LiteralPath (Join-Path $Out 'shad_log.txt'))) {
    Fail-Run 'the emulator log was not collected'
}

# --- analysis --------------------------------------------------------------------------------
$window = $samples.ToArray() | Where-Object { $_.in_window }
if ($window.Count -ne $samplesInWindow) { Fail-Run "only $($window.Count) in-window samples" }
$windowAvgMs = @($window | ForEach-Object { [double]$_.window_avg_frame_ms })
$totalFrames = ($window | ForEach-Object { [int]$_.frames } | Measure-Object -Sum).Sum
$totalTime = ($window | ForEach-Object { [double]$_.frames / [double]$_.fps } |
    Measure-Object -Sum).Sum
$uniqueFps = if ($totalTime -gt 0) { $totalFrames / $totalTime } else { 0.0 }
$frameStats = [ordered]@{
    p50  = Round3 (Get-Percentile $windowAvgMs 0.50)
    p90  = Round3 (Get-Percentile $windowAvgMs 0.90)
    p95  = Round3 (Get-Percentile $windowAvgMs 0.95)
    p99  = Round3 (Get-Percentile $windowAvgMs 0.99)
    mean = Round3 (($windowAvgMs | Measure-Object -Average).Average)
    min  = Round3 (($windowAvgMs | Measure-Object -Minimum).Minimum)
    max  = Round3 (($windowAvgMs | Measure-Object -Maximum).Maximum)
}
$windowStart = $samples[$warmupTarget].at_s
$windowEnd = $samples[$windowTarget - 1].at_s
$framesCovered = @($window | Where-Object { [int]$_.frames -gt 0 }).Count
if ($framesCovered -eq $window.Count) {
    # The emulator's own window duration: frames of the window / window fps. This is measured by
    # the emulator's steady_clock, not inferred from the runner's polling.
    $windowElapsed = ($window | ForEach-Object { [double]$_.frames / [double]$_.fps } |
        Measure-Object -Sum).Sum
} else {
    $windowElapsed = $windowObserved
}

$framesCsv = New-Object System.Collections.Generic.List[string]
$framesCsv.Add('sample,at_s,fps,frames,window_avg_frame_ms,passes,loading_passes,mpx,draws,dispatches,submits,gpu_busy_pct,in_window')
foreach ($s in $samples) {
    $framesCsv.Add(('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12}' -f $s.sample, $s.at_s,
            $s.fps.ToString($inv), $s.frames, $s.window_avg_frame_ms.ToString($inv), $s.passes, $s.loading,
            $s.mpx, $s.draws, $s.dispatches, $s.submits, $s.gpu_busy, [int]$s.in_window))
}
Invoke-Guard 'write' (Join-Path $Out 'frames.csv')
[IO.File]::WriteAllText((Join-Path $Out 'frames.csv'), (($framesCsv -join "`r`n") + "`r`n"),
    (New-Object System.Text.UTF8Encoding($false)))

$allLines = $logLines.ToArray()
$diag = @($allLines | Where-Object {
        $_ -match 'frame stats at|frame path:|HMD frame |OpenXR swapchain|QUALITYDIAG|Device lost|DEVICE_LOST|Assertion Failed|Unreachable code|AUTOPRESS|RDOC capture'
    })
Invoke-Guard 'write' (Join-Path $Out 'diag.txt')
[IO.File]::WriteAllText((Join-Path $Out 'diag.txt'), (($diag -join "`r`n") + "`r`n"),
    (New-Object System.Text.UTF8Encoding($false)))

# Actual presentation/eye size, when the session logged it.
$actualPresentation = $null
foreach ($line in $allLines) {
    if ($line -match '(?<w>\d+)x(?<h>\d+) \[per eye: (?<pw>\d+)x(?<ph>\d+), base eye: (?<bw>\d+)x(?<bh>\d+), scale: (?<s>[0-9.]+)x\]') {
        $actualPresentation = "$($Matches['w'])x$($Matches['h']) window, per eye $($Matches['pw'])x$($Matches['ph']), base eye $($Matches['bw'])x$($Matches['bh']), scale: $($Matches['s'])x"
        break
    }
}
$stereoActual = $null
foreach ($line in $allLines) {
    if ($line -match 'HMD frame \d+ eye \d+: image \d+ at 0x[0-9a-fA-F]+, (?<w>\d+)x(?<h>\d+)') {
        $stereoActual = "$($Matches['w'])x$($Matches['h'])"
        break
    }
}
$autopressBursts = @($allLines | Where-Object { $_ -match 'AUTOPRESS' }).Count

# --- RenderDoc capture evidence, when this session was launched under RenderDoc -------------
$rdcRelative = $null
$rdcPngRelative = $null
$rdcSha = $null
$rdcPngSimilarity = $null
if ($RenderDocCaptureDir) {
    # shadps4's RenderDoc integration sets its own capture file template to
    # user\captures\CUSA11919, so the capture lands in the lab, not where renderdoccmd was told.
    # Search both places, take the newest capture of this session, and copy it into the run.
    $searchDirs = @($RenderDocCaptureDir, (Join-Path $labUser 'captures')) |
        Select-Object -Unique
    $rdc = $null
    foreach ($dir in $searchDirs) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        $found = Get-ChildItem -LiteralPath $dir -File -Filter '*.rdc' -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTime -ge $runStart } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($found -and ($null -eq $rdc -or $found.LastWriteTime -gt $rdc.LastWriteTime)) { $rdc = $found }
    }
    if ($null -eq $rdc) { Fail-Run 'the RenderDoc capture did not appear; the scene step still ran but the capture is missing' }
    $rdcInRun = Join-Path $RenderDocCaptureDir $rdc.Name
    Invoke-Guard 'write' $rdcInRun
    Copy-Item -LiteralPath $rdc.FullName -Destination $rdcInRun -Force
    $rdcSha = Get-Sha256 $rdcInRun
    $rdcRelative = [IO.Path]::GetRelativePath($Out, $rdcInRun)
    $pngPath = Join-Path $RenderDocCaptureDir ($rdc.BaseName + '-final.png')
    & $RenderDocCmd thumb --out $pngPath --max-size 0 $rdcInRun | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $pngPath)) {
        Fail-Run 'renderdoccmd could not read the capture back into a thumbnail'
    }
    $rdcPngRelative = [IO.Path]::GetRelativePath($Out, $pngPath)
    $rdcPngSimilarity = Round3 (Get-SceneSimilarity $pngPath)
    Write-Output ("RenderDoc capture: {0} ({1:N0} bytes), final image similarity {2:N4}" -f `
            $rdc.Name, $rdc.Length, $rdcPngSimilarity)
    # A RenderDoc session may die during the readback-heavy render-target dump before the harness
    # screenshot exists. The capture's own final image is then the scene evidence: use it as the
    # screenshot, and let the same independent comparison decide whether it is the classroom.
    if (-not $sceneShot -and $rdcPngSimilarity -ge 0.85) {
        Invoke-Guard 'write' (Join-Path $Out 'scene.png')
        Copy-Item -LiteralPath $pngPath -Destination (Join-Path $Out 'scene.png') -Force
        $sceneShot = Join-Path $Out 'scene.png'
        $shotSha = Get-Sha256 $sceneShot
        $shotSimilarity = $rdcPngSimilarity
        $shotTakenAt = $stopwatch.Elapsed.TotalSeconds
        $sizeText = & $Python (Join-Path $PSScriptRoot 'scene_signature.py') size $sceneShot 2>&1
        if ($LASTEXITCODE -eq 0) {
            $size = $sizeText | ConvertFrom-Json
            $shotWidth = [int]$size.width
            $shotHeight = [int]$size.height
        }
        Write-Output 'scene screenshot taken from the RenderDoc capture final image'
    }
}

# Scene identity for the run: the screenshot must match the reviewed reference.
$similarity = if ($sceneShot) { [double]$shotSimilarity } else { 0.0 }
$classification = if ($similarity -ge 0.85) { 'classroom-main-menu-overlay' } else { 'unverified' }
$overlayPresent = $similarity -ge 0.85
if (-not $sceneShot) { $classification = 'unverified'; $overlayPresent = $false }

# --- postflight -----------------------------------------------------------------------------
$postflightPath = Join-Path $Out 'postflight.json'
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'postflight.ps1') -Before $preflightPath -Out $postflightPath
$postflightExit = $LASTEXITCODE
$postDoc = Get-Content -LiteralPath $postflightPath -Raw | ConvertFrom-Json
$protectedUnchanged = [bool]$postDoc.protected_unchanged
$drift = @($postDoc.unexpected)
$observed = @($postDoc.observed)

$screenshots = @()
if ($sceneShot) { $screenshots += 'scene.png' }

$document = [ordered]@{
    schema_version = 1
    run_id         = $runName
    label          = $Label
    created_utc    = $startedUtc
    runner_command = [Environment]::CommandLine
    scenario       = [ordered]@{
        id       = 'deracine-classroom-menu-300-msaa2-vblank60'
        game     = [ordered]@{
            title_id       = 'CUSA11919'
            eboot          = $gameEboot
            eboot_sha256   = Get-Sha256 $gameEboot
            root           = [IO.Path]::GetFullPath($Game)
            shadow_root    = [IO.Path]::GetFullPath($ShadowRoot)
            shadow_evidence = $shadowEvidence
            content_source = 'private shadow: eboot copy plus junctions to the shared read-only content'
            engine_ini     = $engineIni
            engine_ini_sha256_before = $engineIniBefore
            engine_ini_sha256_after  = (Get-Sha256 $engineIni)
        }
        guest    = [ordered]@{
            screen_percentage = 300
            msaa_requested    = 2
            eye_width         = 2880
            eye_height        = 3240
            stereo_width      = 5760
            stereo_actual     = $stereoActual
        }
        presentation = [ordered]@{
            vr_scale  = $VrScale
            guest_eye = '2880x3240 per eye'
            actual    = $actualPresentation
        }
        vblank_hz        = $Vblank
        vblank_actual_hz = $vblankActual
        autopress        = [ordered]@{
            enabled       = -not ($Autopress.Trim().ToLowerInvariant() -in @('', '0', 'none'))
            value         = $Autopress
            devices       = $(if ($Autopress -eq '1' -or $Autopress -eq 'all') { 'pad,move' } else { $Autopress })
            schedule      = '8000-45000 ms, 600 ms every 3000 ms'
            bursts_logged = $autopressBursts
        }
        warmup_s       = $Warmup
        window_s       = $Seconds
        stats_every_s  = $StatsEvery
        scene_gate     = [ordered]@{
            min_draws           = $SceneMinDraws
            min_mpx             = $SceneMinMpx
            consecutive_samples = $SceneSamples
        }
        renderdoc      = [ordered]@{
            requested_seconds = $(if ($RdocCapture) { $RdocCapture } else { $null })
            capture_dir       = $(if ($RenderDocCaptureDir) { $RenderDocCaptureDir } else { $null })
            backend           = $(if ($RenderDocCaptureDir) { 'renderdoccmd capture' } else { $null })
        }
    }
    source         = [ordered]@{
        worktree                = $Worktree
        branch                  = if ($branch) { $branch.Trim() } else { 'detached' }
        head_sha                = if ($head) { $head.Trim() } else { '' }
        dirty_files             = @($dirtyLines)
        dirty_count             = $dirtyLines.Count
        status_porcelain_sha256 = $dirtySha
    }
    executable     = [ordered]@{
        path            = $Exe
        sha256          = $exeHash
        bytes           = $exeBytes
        lab_path        = $labExe
        lab_sha256      = Get-Sha256 $labExe
        built_from_head = $(if ($BuiltFromCommit) { $BuiltFromCommit } else { $null })
        build_commit_source = $(if ($BuiltFromCommit) { 'declared by the session' } else { 'not recorded' })
    }
    host           = [ordered]@{
        gpu        = $gpu
        driver     = $driver
        os         = $os
        cpu        = $cpu
        xr_runtime = $xrRuntime
        headset    = 'not queried; headless session (SHADPS4_XR_PAUSE=0), classroom front end only'
    }
    env            = [ordered]@{}
    timing         = [ordered]@{
        source             = 'framestats'
        source_id          = 'SHADPS4_FRAME_STATS windows: per-window frames / steady_clock seconds computed by the emulator draw scheduler'
        samples_total      = $samples.Count
        samples_window     = $window.Count
        warmup_samples     = $warmupTarget
        window_start_s     = $windowStart
        window_end_s       = $windowEnd
        window_elapsed_s   = Round3 $windowElapsed
        window_observed_s  = Round3 $windowObserved
        warmup_start_s     = $samples[0].at_s
        warmup_end_s       = $samples[$warmupTarget - 1].at_s
        unique_fps         = Round3 $uniqueFps
        window_avg_frame_ms = $frameStats
        frame_time_statistic = 'window-average'
        frame_time_derived_from = 'one mean-FPS value per stats window (1000/fps); not per-frame timing'
        per_frame_timing = $false
        compositor_cadence = $null
        incomplete_stats_lines = $script:incompleteStats
        per_sample         = $samples.ToArray()
    }
    scene          = [ordered]@{
        classification    = $classification
        overlay_present   = $overlayPresent
        gameplay_reached  = $false
        anchor_sample     = $anchor.sample
        anchor_at_s       = $anchor.at_s
        draws             = $anchor.draws
        mpx               = $anchor.mpx
        passes            = $anchor.passes
        dumps             = $dumpEvidence.ToArray()
        screenshot        = [ordered]@{
            path                 = $(if ($sceneShot) { 'scene.png' } else { '' })
            sha256               = $(if ($shotSha) { $shotSha } else { '' })
            width                = $shotWidth
            height               = $shotHeight
            taken_at_s           = Round3 $shotTakenAt
            from_scene_anchor_s  = Round3 ($shotTakenAt - $anchorElapsed)
            role                 = 'scene'
            signature_similarity = Round3 $similarity
            reference            = 'reference/scene-classroom-menu.json'
        }
    }
    artifacts      = [ordered]@{
        log             = 'shad_log.txt'
        log_sha256      = Get-Sha256 (Join-Path $Out 'shad_log.txt')
        frames_csv      = 'frames.csv'
        console         = 'console.txt'
        screenshots     = $screenshots
        rdc             = $rdcRelative
        rdc_sha256      = $rdcSha
        rdc_png         = $rdcPngRelative
        rdc_png_similarity = $rdcPngSimilarity
    }
    provenance     = [ordered]@{
        preflight_manifest  = 'preflight.json'
        protected_unchanged = $protectedUnchanged
        protected_drift     = @($drift)
        protected_observed  = @($observed)
        validator_version   = 1
    }
}
foreach ($key in $envMap.Keys) { $document.env[$key] = [string]$envMap[$key] }

Invoke-Guard 'write' (Join-Path $Out 'run.json')
Remove-Item -LiteralPath (Join-Path $Out 'INVALID.txt') -Force -ErrorAction SilentlyContinue
Write-JsonFile (Join-Path $Out 'run.json') $document
Write-Output ("run written: {0}" -f (Join-Path $Out 'run.json'))

if ($postflightExit -ne 0) {
    Write-Output 'postflight FAILED: protected files changed during the session'
    exit 3
}

# --- validity gate ---------------------------------------------------------------------------
& $Python (Join-Path $PSScriptRoot 'validator.py') run $Out
exit $LASTEXITCODE
