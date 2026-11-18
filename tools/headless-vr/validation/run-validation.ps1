#requires -Version 7.0
<#
.SYNOPSIS
    A-E renderer validation for the cross-fork candidate, run against the headless gameplay build.

.DESCRIPTION
    Orchestrates the validation matrix by calling _build\headless\headless-vr-gameplay.ps1
    (the teammate harness; this script never launches the emulator itself):

      P  recycler pair: one OFF cell then one ON cell, with input-execution verification.
      A  depth/barrier configuration cells on the gameplay scene, recycler OFF: base (build
         defaults) twice for the noise floor, then legacy (SHADPS4_DBG_DEPTH_PASS=0 and
         SHADPS4_DBG_BARRIER_IN_PASS=0), each with -Capture for a post-window gameplay eye image.
      B  pixel content between the A arms: base/base floor first, then base vs legacy, compared
         with _build\bench\capture-compare.py and _build\bench\capture-diff.py.
      C  R6 normal shutdown with the recycler ON (needs a build with SHADPS4_EXIT_AFTER and
         SHADPS4_CLEAN_EXIT; the 1a53ceb candidate does not have them).
      D  RenderDoc scene-gated capture with the recycler off, validated with renderdoccmd.
      E  100 % render / 120 Hz configuration cell, reported separately from the 300 % table.

    Every emulator cell runs inside the machine-wide GPU lock
    (bash _build/team/gpu-lock.sh run --owner validate-eng --kind headless --require-quiet), one
    emulator at a time; the lock wrapper releases on failure. -Authorized is required for any
    emulator cell so a dry run cannot start the GPU.

    -DryRun processes existing evidence (run dirs, captures, raw logs) with the same parsers and
    writes results\dryrun\DYRUN.{json,md}; it needs no GPU.
    -Plan only prints the exact command set into results\COMMANDS.md.

.EXAMPLE
    # no-GPU parser proof over the existing evidence
    pwsh -File run-validation.ps1 -DryRun

    # print the exact A-E command set
    pwsh -File run-validation.ps1 -Plan

    # one gameplay arm + capture, as the Lead asked before the matrix
    pwsh -File run-validation.ps1 -Authorized -Modes A -OnlyCells A-off-1,A-on-1 -CaptureOn on

    # full matrix
    pwsh -File run-validation.ps1 -Authorized
#>
[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [string]$Exe = '',
    [string]$Harness = '',
    [string]$Root = 'M:\Development\Deracine-work',
    [string]$OutRoot = '',
    # A-E, any subset
    [ValidateSet('A', 'B', 'C', 'D', 'E')][string[]]$Modes = @('A', 'B', 'C', 'D', 'E'),
    # Refuse to start any emulator cell without this.
    [switch]$Authorized,
    # Parse existing evidence, no GPU.
    [switch]$DryRun,
    # Print the exact command set and exit (no GPU).
    [switch]$PrintPlan,
    # Exercise the B capture-comparison/verdict path on existing capture runs (no GPU).
    [switch]$SelfTestB,
    [string[]]$DryRunPaths = @(),
    # gameplay is the only acceptance-grade gate; menu is CALIBRATION-ONLY and refused unless
    # -AllowCalibration is given, so the front end cannot be substituted for gameplay by accident.
    [ValidateSet('gameplay', 'menu')][string]$Gate = 'gameplay',
    [switch]$AllowCalibration,
    [int]$Seconds = 40,
    [int]$Warmup = 10,
    [int]$Hz = 120,
    [int]$RenderPercent = 300,
    [int]$SceneMinDraws = 400,
    [double]$SceneMinMpx = 450,
    [int]$SceneSamples = 2,
    # Gameplay gate (in the harness): a settled heavy phase after the loading dip must average at
    # least this many draws. The menu is ~465-500 draws, the loading dip 42-55, gameplay ~1866.
    [int]$GameplayMinDraws = 800,
    # Measured-window band for a *gameplay* cell (M2-confirmed scene): 1866-1900 draws,
    # 620-720 Mpx, 42-46 passes. The menu (465-500 / 480 / 65) and the loading dip are outside it.
    [int]$BandMinDraws = 1500,
    [int]$BandMaxDraws = 2300,
    [double]$BandMinMpx = 550,
    [double]$BandMaxMpx = 750,
    [int]$BandMinPasses = 35,
    [int]$BandMaxPasses = 55,
    # C: SHADPS4_EXIT_AFTER seconds; must exceed the gameplay anchor + window.
    [int]$ExitAfterSeconds = 85,
    [int]$MaxSeconds = 420,
    # A cell order. off/off/on/off gives both the A/A noise floor and OFF->ON->OFF.
    [string]$ArmOrder = 'off,off,on,off',
    # B dump point:
    #   harness = the harness's own gameplay-gated menu-baseline + gameplay-candidate dumps
    #             (default; the only mode that is anchored on the harness's gameplay verdict)
    #   scene   = SHADPS4_DBG_DUMP_HMD_AT_SCENE (wt-headless; lower-bound gate, can fire on the menu)
    #   frames  = fixed HMD hand-off frame ids (works on 1a53ceb too)
    [ValidateSet('harness', 'frames', 'scene')][string]$BDumpMode = 'harness',
    [string]$BDumpFrames = '',
    [int]$BProbeMarginSeconds = 5,
    # D: RenderDoc scene gate "<draws>,<mpx>" and the optional time trigger. 800/550 is the
    # gameplay gate: the menu (~485 draws) cannot arm it, so the capture cannot land on the menu.
    [string]$BRdocScene = '1500,550',
    [double]$BRdocSeconds = 0,
    # M2 recipe input script; empty = the harness default (menu-continue-move.txt for -Gate gameplay).
    [string]$InputScript = '',
    # Gameplay eye reference for the harness image gate; empty = its default.
    [string]$GameplayReference = '',
    # One-time: copy an accepted gameplay eye image to the reference path (after a visual check).
    [switch]$SetGameplayReference,
    # Slot-1 save source; the classroom only loads from a lab carrying MGS_GameData_1
    # (stage m00_00_10_10). Empty = the harness default (_run/pc-vr-recycler-try/.../CUSA11919).
    [string]$SaveDataSource = '',
    # Record the save provenance but do not abort when slot 1 is missing (not for real cells).
    [switch]$SkipSaveCheck,
    [string]$LabelPrefix = '',
    [switch]$NoLock,
    # Step 2 of the Lead's order: run exactly one recycler-OFF cell then one recycler-ON cell and
    # classify. A stall on the ON arm is only reported after input execution is verified; the
    # earlier reports were a harness live-input bug, not renderer findings.
    [switch]$RecyclerPair,
    # Backward-compatible alias for -RecyclerPair (the old smoke-pair name).
    [switch]$SmokePair,
    # Record the recycler pair from two existing run labels (no GPU): "<off-label>,<on-label>".
    # Used for the Lead's OFF/ON cells so the matrix does not duplicate them.
    [string]$PairFromRuns = '',
    # B from existing -Capture runs (no GPU): "<off1>,<off2>,<on>"; off1 vs off2 is the pixel
    # floor, off1/off2 vs on are the targets. Used after the b-off-2 capture cell.
    [string]$BCaptureFromRuns = '',
    # A acceptance from existing cells (no GPU): "<base1>,<base2>,<legacy>".
    [string]$AFromRuns = '',
    # C from an existing clean-exit run label (no GPU).
    [string]$CFromRun = '',
    # D validation from existing RenderDoc run labels, comma-separated (no GPU).
    [string]$DFromRuns = '',
    # Harness attempts per cell (the title has a ~18-20 s startup crash; a crashed attempt is not
    # a measurement). The smoke pair uses this for OFF and 2 for ON to bound the stuck case.
    [int]$Attempts = 3,
    # Run only the listed cell labels (substring match).
    [string[]]$OnlyCells = @(),
    [int]$TimeoutSeconds = 1800,
    # Wait this long for a quiet machine before a cell (builds in another worktree otherwise
    # invalidate a cell: the harness flags "another benchmark process was seen").
    [int]$QuietWaitSeconds = 900,
    # Fail on the first invalid cell instead of continuing to the next mode.
    [switch]$StopOnInvalid
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

# --- constants --------------------------------------------------------------------------------
$script:UsePair = [bool]($RecyclerPair -or $SmokePair)
$script:CellBand = @{}
$script:ValidationDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
if ($OutRoot -eq '') { $OutRoot = Join-Path $script:ValidationDir 'results' }
$script:Out = $OutRoot
$script:CellsDir = Join-Path $script:Out 'cells'
$script:DryDir = Join-Path $script:Out 'dryrun'
$script:EvidenceDir = Join-Path $script:ValidationDir 'evidence'
if ($Exe -eq '') { $Exe = Join-Path $Root 'wt-headless\build\shadPS4.exe' }
if ($Harness -eq '') { $Harness = Join-Path $Root '_build\headless\headless-vr-gameplay.ps1' }
$script:LockScript = Join-Path $Root '_build\team\gpu-lock.sh'
$script:LockLinux = '/mnt/m/Development/Deracine-work/_build/team/gpu-lock.sh'
$script:RunsRoot = Join-Path $Root '_build\headless\runs'
if ($SaveDataSource -eq '') { $SaveDataSource = Join-Path $Root '_run\pc-vr-recycler-try\user\home\1000\savedata\CUSA11919' }
if ($GameplayReference -eq '') { $GameplayReference = Join-Path $Root '_build\headless\references\gameplay-eye.png' }
$script:CaptureCompare = Join-Path $Root '_build\bench\capture-compare.py'
$script:CaptureDiff = Join-Path $Root '_build\bench\capture-diff.py'
$script:EyeHelper = Join-Path $script:ValidationDir 'lib\eye-image.py'
if ($LabelPrefix -eq '') { $LabelPrefix = 'xfval-' + (Get-Date -Format 'yyyyMMdd-HHmm') }
$script:Prefix = $LabelPrefix
$script:Python = $null
foreach ($candidate in @('python', 'python3', 'py')) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $script:Python = $cmd.Source; break }
}
if ($null -eq $script:Python) { $script:Python = 'C:\Users\Arukas\AppData\Local\Programs\Python\Python314\python.exe' }

$script:HmdRegex = [regex]'PrepareHmdFrame: HMD frame\s+(?<frame>\d+)\s+eye\s+(?<eye>[01]): image\s+(?<image>\d+)\s+at\s+(?<addr>0x[0-9a-fA-F]+),\s+(?<w>\d+)x(?<h>\d+),\s+format\s+(?<fmt>\S+),\s+samples\s+(?<samples>\d+)'
$script:StatsRegex = [regex]'frame stats at\s+(?<at>[\d.]+)\s+s(?: \[(?<ab>[A-Z+]+)\])?:\s+(?<fps>[\d.]+)\s+fps,\s+per frame\s+(?<passes>[\d.]+)\s+passes\s+\((?<loading>[\d.]+)\s+loading their targets,\s+(?<mpx>[\d.]+)\s+Mpx\),\s+(?<draws>[\d.]+)\s+draws,\s+(?<dispatches>[\d.]+)\s+dispatches,\s+(?<submits>[\d.]+)\s+submits(?:,\s+GPU busy\s+(?<busy>\d+)%(?:\s+at\s+(?<mhz>\d+)\s+MHz)?)?'
# The file name may carry a harness dump prefix ("menu-baseline__008_...") or a plain
# "<seq>_<addr>_<w>x<h>_<fmt>_s<n>.png"; the identity part is the trailing sequence.
$script:HmdImageRegex = [regex]'(?:^|/)(?:[^/]*__)?(?<seq>\d+)_(?<addr>[0-9a-fA-F]+)_(?<w>\d+)x(?<h>\d+)_(?<fmt>[A-Za-z0-9]+)_s(?<samples>\d+)(?<alpha>_alpha)?\.png$'
$script:DumpNameRegex = [regex]'^rt_dump[-_](?<name>.+)$'
$script:RdocTriggerRegex = [regex]'RDOC capture triggered\s+(?<s>[\d.]+)\s+s after the first frame'
$script:RdocGateRegex = [regex]'RDOC scene gate met:\s+(?<draws>[\d.]+)\s+draws and\s+(?<mpx>[\d.]+)\s+Mpx'
$script:CleanMarkers = @(
    'SHADPS4_EXIT_AFTER=',
    'SHADPS4_CLEAN_EXIT: destroying the presenter before exiting',
    'SHADPS4_CLEAN_EXIT: stopping the GPU and present threads',
    'SHADPS4_CLEAN_EXIT: destroying the presenter',
    'SHADPS4_CLEAN_EXIT: the presenter is destroyed',
    'SHADPS4_CLEAN_EXIT: the presenter is gone'
)
$script:ValidationPatterns = @(
    'VUID-', 'Validation Error', 'VK_ERROR', 'Device lost', '<Critical>', 'Assertion Failed',
    'Unreachable code', 'vk::DeviceLostError', 'vk::OutOfDeviceMemoryError'
)
$script:CrashPatterns = @('Unhandled exception', 'access violation', 'Stack trace', 'Fatal error', 'Segmentation')
# The wt-headless clean-exit path refuses to destroy the presenter when the guest threads will not
# pause. That is not a pass: R6 stays unexercised and C must say so.
$script:R6BlockPatterns = @('the presenter was left alone', 'R6 blocked', 'did not pause')

# --- small helpers ----------------------------------------------------------------------------
function Round3([double]$v) { if ($null -eq $v) { return $null }; return [math]::Round($v, 3) }
function Round1([double]$v) { if ($null -eq $v) { return $null }; return [math]::Round($v, 1) }

function Write-JsonFile([string]$Path, $Object) {
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $json = $Object | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $json + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
}

function Write-TextFile([string]$Path, [string]$Text) {
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) } catch { return $null }
}

function Get-MeanOf($Values) {
    $v = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return $null }
    return ($v | Measure-Object -Average).Average
}

function Get-Percentile([double[]]$Values, [double]$P) {
    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 1) { return $sorted[0] }
    $rank = ($P / 100.0) * ($sorted.Count - 1)
    $lo = [int][math]::Floor($rank)
    $hi = [int][math]::Ceiling($rank)
    if ($lo -eq $hi) { return $sorted[$lo] }
    return $sorted[$lo] + ($sorted[$hi] - $sorted[$lo]) * ($rank - $lo)
}

function Get-Stats($Values) {
    $v = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return [ordered]@{ count = 0 } }
    $mean = ($v | Measure-Object -Average).Average
    return [ordered]@{
        count = $v.Count
        mean  = Round3 $mean
        p50   = Round3 (Get-Percentile $v 50)
        p90   = Round3 (Get-Percentile $v 90)
        p99   = Round3 (Get-Percentile $v 99)
        min   = Round3 (($v | Measure-Object -Minimum).Minimum)
        max   = Round3 (($v | Measure-Object -Maximum).Maximum)
    }
}

function Format-Num($v, [string]$fmt = '0.0') {
    if ($null -eq $v) { return '-' }
    return ([double]$v).ToString($fmt)
}

# --- log parsing ------------------------------------------------------------------------------
function Get-LogParse([string]$LogPath) {
    $parse = [ordered]@{
        path         = $LogPath
        exists       = (Test-Path -LiteralPath $LogPath)
        bytes        = $(if (Test-Path -LiteralPath $LogPath) { (Get-Item -LiteralPath $LogPath).Length } else { 0 })
        samples      = New-Object System.Collections.Generic.List[object]
        renderstat   = New-Object System.Collections.Generic.List[object]
        aliasstat    = New-Object System.Collections.Generic.List[object]
        hmd          = $null
        hmd_lines    = New-Object System.Collections.Generic.List[object]
        rdoc_gate    = $null
        rdoc_trigger = $null
        clean_markers = [ordered]@{}
        validation   = New-Object System.Collections.Generic.List[string]
        crashes      = New-Object System.Collections.Generic.List[string]
        r6_blocked   = New-Object System.Collections.Generic.List[string]
        log_lines    = 0
        autopress    = 0
        move_presses = 0
    }
    foreach ($m in $script:CleanMarkers) { $parse.clean_markers[$m] = $false }
    if (-not $parse.exists) { return $parse }

    $index = -1
    $reader = [IO.File]::OpenText($LogPath)
    try {
        while ($null -ne ($line = $reader.ReadLine())) {
            $parse.log_lines++
            if ($line.Contains('AUTOPRESS')) { $parse.autopress++ }
            if ($line.Contains('AUTOPRESS move')) { $parse.move_presses++ }
            $m = $script:StatsRegex.Match($line)
            if ($m.Success) {
                ++$index
                $parse.samples.Add([pscustomobject]@{
                        index = $index; at_s = [double]$m.Groups['at'].Value
                        fps = [double]$m.Groups['fps'].Value; passes = [double]$m.Groups['passes'].Value
                        loading = [double]$m.Groups['loading'].Value; mpx = [double]$m.Groups['mpx'].Value
                        draws = [double]$m.Groups['draws'].Value; submits = [double]$m.Groups['submits'].Value
                        busy = $(if ($m.Groups['busy'].Success) { [int]$m.Groups['busy'].Value } else { $null })
                    })
                continue
            }
            if ($line -match 'RENDERSTAT ') {
                $fields = [ordered]@{}
                foreach ($token in ($line -split 'RENDERSTAT ')[1].Split(' ')) {
                    $kv = $token.Split('=')
                    if ($kv.Count -eq 2 -and $kv[0] -ne '') { $fields[$kv[0]] = $kv[1] }
                }
                $parse.renderstat.Add([pscustomobject]@{ index = $index; fields = $fields })
                continue
            }
            if ($line -match 'ALIASSTAT ') {
                $fields = [ordered]@{}
                foreach ($token in ($line -split 'ALIASSTAT ')[1].Split(' ')) {
                    $kv = $token.Split('=')
                    if ($kv.Count -eq 2 -and $kv[0] -ne '') { $fields[$kv[0]] = $kv[1] }
                }
                $parse.aliasstat.Add([pscustomobject]@{ index = $index; fields = $fields })
                continue
            }
            if ($null -eq $parse.hmd -or $parse.hmd_lines.Count -lt 4) {
                $h = $script:HmdRegex.Match($line)
                if ($h.Success) {
                    $entry = [pscustomobject]@{
                        frame = [int]$h.Groups['frame'].Value; eye = [int]$h.Groups['eye'].Value
                        w = [int]$h.Groups['w'].Value; h = [int]$h.Groups['h'].Value
                        fmt = $h.Groups['fmt'].Value; samples = [int]$h.Groups['samples'].Value
                        addr = $h.Groups['addr'].Value
                    }
                    $parse.hmd_lines.Add($entry)
                    if ($null -eq $parse.hmd) { $parse.hmd = $entry }
                }
            }
            if ($null -eq $parse.rdoc_gate) {
                $g = $script:RdocGateRegex.Match($line)
                if ($g.Success) {
                    $parse.rdoc_gate = [ordered]@{ draws = [double]$g.Groups['draws'].Value; mpx = [double]$g.Groups['mpx'].Value }
                }
            }
            if ($null -eq $parse.rdoc_trigger) {
                $t = $script:RdocTriggerRegex.Match($line)
                if ($t.Success) { $parse.rdoc_trigger = [double]$t.Groups['s'].Value }
            }
            foreach ($marker in $script:CleanMarkers) {
                if (-not $parse.clean_markers[$marker] -and $line.Contains($marker)) { $parse.clean_markers[$marker] = $true }
            }
            foreach ($p in $script:ValidationPatterns) {
                if ($line.Contains($p) -and $parse.validation.Count -lt 40) {
                    $parse.validation.Add($line.Substring(0, [Math]::Min(300, $line.Length)))
                    break
                }
            }
            foreach ($p in $script:CrashPatterns) {
                if ($line.Contains($p) -and $parse.crashes.Count -lt 20) {
                    $parse.crashes.Add($line.Substring(0, [Math]::Min(300, $line.Length)))
                    break
                }
            }
            foreach ($p in $script:R6BlockPatterns) {
                if ($line.Contains($p) -and $parse.r6_blocked.Count -lt 10) {
                    $parse.r6_blocked.Add($line.Substring(0, [Math]::Min(300, $line.Length)))
                    break
                }
            }
        }
    } finally { $reader.Dispose() }
    return $parse
}

function Get-SceneWindow($Parse, [int]$MinDraws, [double]$MinMpx, [int]$Need, [int]$WarmupS, [int]$SecondsS) {
    $samples = $Parse.samples
    $runs = New-Object System.Collections.Generic.List[object]
    $run = New-Object System.Collections.Generic.List[object]
    foreach ($s in $samples) {
        if ($s.draws -ge $MinDraws -and $s.mpx -ge $MinMpx) { $run.Add($s) }
        else { if ($run.Count -gt 0) { $runs.Add([object[]]$run.ToArray()); $run = New-Object System.Collections.Generic.List[object] } }
    }
    if ($run.Count -gt 0) { $runs.Add([object[]]$run.ToArray()) }
    $qualified = @($runs | Where-Object { $_.Count -ge $Need } | Sort-Object { $_.Count } -Descending)
    if ($qualified.Count -eq 0) {
        $longest = 0
        foreach ($r in $runs) { if ($r.Count -gt $longest) { $longest = $r.Count } }
        return [ordered]@{ ok = $false; reason = "no $Need consecutive heavy windows with draws>=$MinDraws and Mpx>=$MinMpx (longest run $longest)"; anchor_s = $null; longest_run = $longest }
    }
    $anchor = $qualified[0][0]
    $anchorAt = [double]$anchor.at_s
    $window = @($samples | Where-Object { $_.at_s -ge ($anchorAt + $WarmupS) -and $_.at_s -lt ($anchorAt + $WarmupS + $SecondsS) })
    if ($window.Count -eq 0) { $window = @($samples | Where-Object { $_.at_s -ge ($anchorAt + $WarmupS) }) }
    $draws = @($window | ForEach-Object { $_.draws })
    $mpx = @($window | ForEach-Object { $_.mpx })
    $passes = @($window | ForEach-Object { $_.passes })
    $fps = @($window | ForEach-Object { $_.fps })
    $busy = @($window | Where-Object { $null -ne $_.busy } | ForEach-Object { $_.busy })
    $dMean = if ($draws.Count) { Get-MeanOf $draws } else { $null }
    $mMean = if ($mpx.Count) { Get-MeanOf $mpx } else { $null }
    $spreadPct = $null
    if ($null -ne $dMean -and $dMean -gt 0) {
        $spreadPct = [math]::Round(100.0 * ((($draws | Measure-Object -Maximum).Maximum - ($draws | Measure-Object -Minimum).Minimum) / $dMean), 2)
    }
    return [ordered]@{
        ok          = $true
        anchor_s    = $anchorAt
        anchor_index = $anchor.index
        first_s     = $(if ($window.Count) { [double]$window[0].at_s } else { $null })
        last_s      = $(if ($window.Count) { [double]$window[-1].at_s } else { $null })
        samples     = $window.Count
        draws_mean  = Round1 $dMean
        draws_min   = if ($draws.Count) { ($draws | Measure-Object -Minimum).Minimum } else { $null }
        draws_max   = if ($draws.Count) { ($draws | Measure-Object -Maximum).Maximum } else { $null }
        mpx_mean    = Round1 $mMean
        mpx_min     = if ($mpx.Count) { ($mpx | Measure-Object -Minimum).Minimum } else { $null }
        mpx_max     = if ($mpx.Count) { ($mpx | Measure-Object -Maximum).Maximum } else { $null }
        passes_mean = Round1 (Get-MeanOf $passes)
        fps         = Get-Stats $fps
        gpu_busy_mean = Round1 (Get-MeanOf $busy)
        draws_spread_pct = $spreadPct
        heavy_runs  = $runs.Count
    }
}

function Get-CounterMeans($Parse, $Window) {
    if ($null -eq $Window -or -not $Window.ok) { return $null }
    $from = $Window.anchor_index
    $to = $Window.anchor_index + $Window.samples
    $rs = @($Parse.renderstat | Where-Object { $_.index -ge $from -and $_.index -le $to })
    $as = @($Parse.aliasstat | Where-Object { $_.index -ge $from -and $_.index -le $to })
    $names = 'images_created', 'images_destroyed', 'create_ms', 'destroy_ms', 'recycler_hits',
    'recycler_misses', 'pool_mb', 'vram_mb', 'refreshes', 'copies', 'depth_ms', 'refresh_ms'
    $perSecond = [ordered]@{}
    foreach ($name in $names) {
        $values = @($rs | ForEach-Object { $_.fields[$name] } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
        $perSecond[$name] = $(if ($values.Count) { Round3 (Get-MeanOf $values) } else { $null })
    }
    foreach ($name in @('depth_copies', 'expands', 'recreations')) {
        $values = @($as | ForEach-Object { $_.fields[$name] } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
        $perSecond[$name] = $(if ($values.Count) { Round3 (Get-MeanOf $values) } else { $null })
    }
    $fps = if ($Window.fps.count -gt 0) { [double]$Window.fps.mean } else { 0 }
    $perFrame = [ordered]@{}
    foreach ($name in @('images_created', 'create_ms', 'destroy_ms', 'recycler_hits', 'recycler_misses', 'refreshes', 'depth_copies')) {
        $v = $perSecond[$name]
        $perFrame[$name] = $(if ($null -ne $v -and $fps -gt 0) { Round3 ([double]$v / $fps) } else { $null })
    }
    return [ordered]@{ renderstat_samples = $rs.Count; aliasstat_samples = $as.Count; per_s = $perSecond; per_frame = $perFrame }
}

# --- cell measurement -------------------------------------------------------------------------
function Get-StageString([string]$Path) {
    try {
        $bytes = [IO.File]::ReadAllBytes($Path)
        $text = [Text.Encoding]::ASCII.GetString($bytes)
        $m = [regex]::Match($text, 'm\d{2}_\d{2}_\d{2}_\d{2}')
        if ($m.Success) { return $m.Value }
    } catch {}
    return ''
}

# The classroom only loads from a lab whose save carries slot 1 (MGS_GameData_1, stage
# m00_00_10_10) plus MGS_SystemData. The quality-lab save (slot 0 only) sits on the menu forever,
# so every cell's provenance is recorded and compared with the source the harness copies.
function Get-SaveProvenance([string]$Source) {
    $dataDir = Join-Path $Source 'SaveData'
    $files = New-Object System.Collections.Generic.List[object]
    $slot1 = $false; $system = $false; $stage = ''
    if (Test-Path -LiteralPath $dataDir) {
        foreach ($f in @(Get-ChildItem -LiteralPath $dataDir -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
            $files.Add([ordered]@{ name = $f.Name; bytes = $f.Length; sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash })
            if ($f.Name -eq 'MGS_GameData_1') { $slot1 = $true; $stage = Get-StageString $f.FullName }
            if ($f.Name -eq 'MGS_SystemData') { $system = $true }
        }
    }
    return [ordered]@{
        source = $Source; save_data_dir = $dataDir
        slot1_present = $slot1; system_present = $system; stage = $stage
        files = [object[]]$files
    }
}

function Get-WindowMeans($Parse, [double]$From, [double]$To) {
    $w = @($Parse.samples | Where-Object { $_.at_s -ge $From -and $_.at_s -le $To })
    if ($w.Count -eq 0) { return $null }
    $draws = @($w | ForEach-Object { $_.draws })
    $mpx = @($w | ForEach-Object { $_.mpx })
    $passes = @($w | ForEach-Object { $_.passes })
    $fps = @($w | ForEach-Object { $_.fps })
    $busy = @($w | Where-Object { $null -ne $_.busy } | ForEach-Object { $_.busy })
    $dMean = Get-MeanOf $draws
    $spread = if ($dMean -gt 0) { [math]::Round(100.0 * ((($draws | Measure-Object -Maximum).Maximum - ($draws | Measure-Object -Minimum).Minimum) / $dMean), 2) } else { $null }
    return [ordered]@{
        samples = $w.Count; from_s = [double]$w[0].at_s; to_s = [double]$w[-1].at_s
        draws_mean = Round1 $dMean
        draws_min = ($draws | Measure-Object -Minimum).Minimum
        draws_max = ($draws | Measure-Object -Maximum).Maximum
        mpx_mean = Round1 (Get-MeanOf $mpx)
        mpx_min = ($mpx | Measure-Object -Minimum).Minimum
        mpx_max = ($mpx | Measure-Object -Maximum).Maximum
        passes_mean = Round1 (Get-MeanOf $passes)
        fps = Get-Stats $fps
        gpu_busy_mean = Round1 (Get-MeanOf $busy)
        draws_spread_pct = $spread
    }
}

function Measure-Cell([string]$Label) {
    $runDir = Join-Path $script:RunsRoot $Label
    $cell = [ordered]@{
        label = $Label
        run_dir = $runDir
        cell_exists = (Test-Path -LiteralPath $runDir)
        result = $null
        meta = $null
        log = $null
        scene = $null
        counters = $null
        hmd = $null
        validation = @()
        crashes = @()
        clean_markers = [ordered]@{}
        arm_evidence = $null
        measured = $null
        error = ''
    }
    if (-not $cell.cell_exists) { $cell.error = 'run directory missing'; return $cell }
    $cell.result = Read-JsonFile (Join-Path $runDir 'result.json')
    $cell.meta = Read-JsonFile (Join-Path $runDir 'meta.json')
    if ($null -eq $cell.result) { $cell.error = 'result.json missing or unreadable'; return $cell }
    $logPath = Join-Path $runDir 'shad_log.txt'
    $parse = Get-LogParse $logPath
    $cell.log = [ordered]@{ path = $logPath; exists = $parse.exists; bytes = $parse.bytes; lines = $parse.log_lines; samples = $parse.samples.Count; renderstat = $parse.renderstat.Count; aliasstat = $parse.aliasstat.Count }
    $r = $cell.result
    $cell.scene = Get-SceneWindow $parse $SceneMinDraws $SceneMinMpx $SceneSamples $Warmup $Seconds

    # The harness reports the measured window; read the same interval back from the log so the
    # band is verified independently of result.json. The menu is ~485 draws / 480 Mpx, the
    # loading dip 42-55, gameplay 1866-1900 / 620-720 / 42-46; the gate and band below are the
    # M2-confirmed gameplay values, not the brief's earlier 480-494.
    $cell.result_window = $null
    if ($null -ne $r.window -and $null -ne $r.window.from_s -and $null -ne $r.window.to_s) {
        $cell.result_window = Get-WindowMeans $parse ([double]$r.window.from_s) ([double]$r.window.to_s)
    }
    $cell.independent_source = if ($null -ne $cell.result_window) { 'result window read back from shad_log' } else { 'own heavy-phase gate (fallback)' }
    $bandWindow = if ($null -ne $cell.result_window) { $cell.result_window } elseif ($cell.scene.ok) { $cell.scene } else { $null }

    # A configuration arm may declare its own band: the legacy depth/barrier behaviour changes
    # passes and Mpx by design, while the scene identity (draws + gameplay phase + image gate) is
    # unchanged. Cells without an override use the candidate band.
    $bMinMpx = $BandMinMpx; $bMaxMpx = $BandMaxMpx; $bMinPasses = $BandMinPasses; $bMaxPasses = $BandMaxPasses
    if ($Label -like '*-A-base-*' -or $Label -like '*-A-legacy-*') {
        # Configuration arms: the legacy depth/barrier behaviour changes Mpx/passes by design.
        $bMinMpx = 400; $bMaxMpx = 1200; $bMinPasses = 30; $bMaxPasses = 75
    }
    if ($Label -like '*-E-*') {
        # 100 % render lowers Mpx by ~9x; draws (scene identity) are unchanged.
        $bMinMpx = 20; $bMaxMpx = 300; $bMinPasses = 20; $bMaxPasses = 75
    }
    if ($Label -like '*-C-*') {
        # C's window is shortened so the emulator's timed quit wins; keep the scene band wide.
        $bMinMpx = 450; $bMaxMpx = 800; $bMinPasses = 30; $bMaxPasses = 75
    }
    if ($script:CellBand.ContainsKey($Label) -and $null -ne $script:CellBand[$Label]) {
        $bMinMpx = $script:CellBand[$Label].min_mpx; $bMaxMpx = $script:CellBand[$Label].max_mpx
        $bMinPasses = $script:CellBand[$Label].min_passes; $bMaxPasses = $script:CellBand[$Label].max_passes
    }
    $bandChecks = [ordered]@{
        gameplay_min_draws = $GameplayMinDraws
        draws = $(if ($bandWindow) { $bandWindow.draws_mean } else { $null })
        mpx = $(if ($bandWindow) { $bandWindow.mpx_mean } else { $null })
        passes = $(if ($bandWindow) { $bandWindow.passes_mean } else { $null })
        draws_ok = ($null -ne $bandWindow -and [double]$bandWindow.draws_mean -ge $GameplayMinDraws)
        draws_in_band = ($null -ne $bandWindow -and [double]$bandWindow.draws_mean -ge $BandMinDraws -and [double]$bandWindow.draws_mean -le $BandMaxDraws)
        mpx_in_band = ($null -ne $bandWindow -and [double]$bandWindow.mpx_mean -ge $bMinMpx -and [double]$bandWindow.mpx_mean -le $bMaxMpx)
        passes_in_band = ($null -ne $bandWindow -and [double]$bandWindow.passes_mean -ge $bMinPasses -and [double]$bandWindow.passes_mean -le $bMaxPasses)
        band = ("draws {0}-{1}, Mpx {2}-{3}, passes {4}-{5}" -f $BandMinDraws, $BandMaxDraws, $bMinMpx, $bMaxMpx, $bMinPasses, $bMaxPasses)
    }
    $bandChecks['ok'] = ($bandChecks.draws_ok -and $bandChecks.draws_in_band -and $bandChecks.mpx_in_band -and $bandChecks.passes_in_band)
    $cell.gameplay_gate = $bandChecks

    $cell.counters = Get-CounterMeans $parse $cell.scene
    $cell.hmd = $parse.hmd
    $cell.validation = @($parse.validation)
    $cell.crashes = @($parse.crashes)
    $cell.r6_blocked = @($parse.r6_blocked)
    $cell.clean_markers = $parse.clean_markers
    $cell.rdoc_gate = $parse.rdoc_gate
    $cell.rdoc_trigger_s = $parse.rdoc_trigger

    # The measured window the harness itself reports (its own anchor and its own counters).
    $cell.measured = [ordered]@{
        valid = $r.valid; reason = $r.reason; phase = $r.phase; gameplay = $r.gameplay; arm = $r.arm
        fps_p50 = $r.fps_p50; fps_p90 = $r.fps_p90; fps_p99 = $r.fps_p99; fps_mean = $r.fps_mean
        frames = $r.frames
        draws_mean = $r.draws_mean; mpx_mean = $r.mpx_mean; passes_mean = $r.passes_mean
        gpu_busy_mean = $r.gpu_busy_mean
        images_created_per_frame = $r.images_created_per_frame
        create_ms_per_frame = $r.create_ms_per_frame
        destroy_ms_per_frame = $r.destroy_ms_per_frame
        recycler_hits_per_frame = $r.recycler_hits_per_frame
        recycler_misses_per_frame = $r.recycler_misses_per_frame
        pool_mb_mean = $r.pool_mb_mean; vram_mb_mean = $r.vram_mb_mean
        depth_copies = $r.depth_copies; refresh_uploads = $r.refresh_uploads
        eye_width = $r.eye_width; eye_height = $r.eye_height
        samples = $r.samples; format = $r.format
        scene_first_s = $r.scene_first_s; scene_last_s = $r.scene_last_s
        shutdown = $r.shutdown
        band = $r.band
        config = $r.config
        window = $r.window
    }

    # Did the arm switch actually take effect?
    $hits = [double]($r.recycler_hits_per_frame | ForEach-Object { if ($null -ne $_) { $_ } else { 0 } })
    $misses = [double]($r.recycler_misses_per_frame | ForEach-Object { if ($null -ne $_) { $_ } else { 0 } })
    $images = $r.images_created_per_frame
    $arm = $r.arm
    $cell.arm_evidence = [ordered]@{
        arm = $arm
        recycler_hits_per_frame = $hits
        recycler_misses_per_frame = $misses
        images_created_per_frame = $images
        off_arm_zero_hits = ($arm -eq 'off' -and $hits -eq 0 -and $misses -eq 0)
        on_arm_activity = ($arm -eq 'on' -and ($hits + $misses) -gt 0)
        ok = $(if ($arm -eq 'on') { ($hits + $misses) -gt 0 } elseif ($arm -eq 'off') { $hits -eq 0 -and $misses -eq 0 } else { $true })
        note = $(if ($arm -eq 'on' -and (($hits + $misses) -eq 0)) { 'ON arm shows no recycler activity: the env did not take effect' } else { '' })
    }

    # Stuck-menu evidence: a cell that never left the menu is not a timing result. Record what it
    # did show (menu band, whether a loading dip ever appeared, whether the recycler was active)
    # so an ON-arm stall can be reported as a concrete finding.
    $lastSample = $null
    if ($parse.samples.Count -gt 0) { $lastSample = $parse.samples[-1] }
    $menuRun = $cell.scene
    $sawDip = $false
    if ($null -ne $menuRun -and $menuRun.ok) {
        $sawDip = @($parse.samples | Where-Object {
                $_.index -gt $menuRun.anchor_index -and ($_.draws -le 200 -or $_.mpx -le 250) }).Count -gt 0
    }
    $cell.menu_stuck = [ordered]@{
        stayed_on_menu = ($r.phase -ne 'gameplay')
        phase = $r.phase; reason = $r.reason
        menu_draws_mean = $(if ($menuRun.ok) { $menuRun.draws_mean } else { $null })
        menu_mpx_mean = $(if ($menuRun.ok) { $menuRun.mpx_mean } else { $null })
        menu_passes_mean = $(if ($menuRun.ok) { $menuRun.passes_mean } else { $null })
        saw_loading_dip_after_menu = $sawDip
        frames_measured = $r.frames
        last_sample_at_s = $(if ($null -ne $lastSample) { $lastSample.at_s } else { $null })
        last_draws = $(if ($null -ne $lastSample) { $lastSample.draws } else { $null })
        recycler_active = $arm_evidence.ok
        # Input execution evidence: a menu stall is only a possible regression if the presses
        # actually ran. The stale live-input file bug removed the file at resolution time, so the
        # live file is rewritten during the run; its presence afterwards is expected.
        input_script = $(if ($null -ne $cell.meta -and $null -ne $cell.meta.lab) { $cell.meta.lab.input } else { $null })
        live_input = $(if ($null -ne $cell.meta -and $null -ne $cell.meta.lab) { $cell.meta.lab.live_input } else { $null })
        live_file_present_after = $(if ($null -ne $cell.meta -and $null -ne $cell.meta.lab -and $cell.meta.lab.live_input) { Test-Path -LiteralPath $cell.meta.lab.live_input } else { $null })
        autopress_lines = $parse.autopress
        move_autopress_lines = $parse.move_presses
        log = $logPath
    }

    # Save provenance: the classroom only loads from a lab carrying slot 1 (MGS_GameData_1,
    # stage m00_00_10_10). The harness records the lab save hashes in meta.save_data.
    $cell.save = [ordered]@{ source = $script:SaveSourceInfo.source; source_slot1 = $script:SaveSourceInfo.slot1_present; lab_slot1 = $null; matches_source = $null; stage = $script:SaveSourceInfo.stage }
    $labSave = $null
    if ($null -ne $cell.meta) {
        if ($null -ne $cell.meta.save_data) { $labSave = $cell.meta.save_data }
        elseif ($null -ne $cell.meta.lab -and $null -ne $cell.meta.lab.save_data) { $labSave = $cell.meta.lab.save_data }
    }
    if ($null -ne $labSave) {
        $labFiles = @($labSave.files)
        $cell.save.lab_slot1 = [bool]$labSave.slot1_present
        # Compare the slot files that matter; the harness also carries sce_sys/ (param.sfo, icon0).
        $same = $true
        foreach ($name in @('MGS_GameData_0', 'MGS_GameData_1', 'MGS_SystemData')) {
            $src = @($script:SaveSourceInfo.files | Where-Object { $_.name -eq $name }) | Select-Object -First 1
            $lab = @($labFiles | Where-Object { $_.name -eq $name }) | Select-Object -First 1
            if ($null -eq $src -or $null -eq $lab -or $src.sha256 -ne $lab.sha256) { $same = $false; break }
        }
        $cell.save.matches_source = $same
        $cell.save.extra_files = @($labFiles | Where-Object { $_.name -notin @('MGS_GameData_0', 'MGS_GameData_1', 'MGS_SystemData') } | ForEach-Object { $_.name })
    }

    # Independent scene identity: the window read back from the log must agree with result.json
    # AND satisfy the gameplay gate/band.
    $cell.independent_band_ok = $false
    if ($null -ne $bandWindow -and $null -ne $r.draws_mean -and $null -ne $r.mpx_mean) {
        $d = [math]::Abs([double]$bandWindow.draws_mean - [double]$r.draws_mean)
        $m = [math]::Abs([double]$bandWindow.mpx_mean - [double]$r.mpx_mean)
        $agree = ($d -le [math]::Max(10.0, 0.02 * [double]$r.draws_mean)) -and ($m -le [math]::Max(15.0, 0.03 * [double]$r.mpx_mean))
        $cell.independent_band_ok = ($agree -and $bandChecks.ok)
    }
    return $cell
}

# --- capture bundles --------------------------------------------------------------------------
function Join-BundlePath([string]$BundleDir, [string]$RelativeName) {
    return (Join-Path $BundleDir ('images\' + ($RelativeName -replace '/', '\')))
}

function Get-HmdImageEntry([string]$RelativeName) {
    $m = $script:HmdImageRegex.Match($RelativeName)
    if (-not $m.Success) { return $null }
    $w = [int]$m.Groups['w'].Value
    $h = [int]$m.Groups['h'].Value
    $fmt = $m.Groups['fmt'].Value
    $isHmd = (($w -eq 5760 -and $h -eq 3240) -or ($w -eq 2880 -and $h -eq 3240)) -and ($fmt -eq 'R8G8B8A8Unorm')
    return [ordered]@{
        name = $RelativeName; addr = $m.Groups['addr'].Value; width = $w; height = $h; format = $fmt
        samples = [int]$m.Groups['samples'].Value; alpha = $m.Groups['alpha'].Success
        hmd = $isHmd
    }
}

function New-CaptureBundle([string]$Label, $Cell) {
    $bundle = Join-Path (Join-Path $script:CellsDir $Label) 'capture-bundle'
    if (Test-Path $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Join-Path $bundle 'images') | Out-Null
    $captures = New-Object System.Collections.Generic.List[object]
    $folders = New-Object System.Collections.Generic.List[string]

    # 1) Dumps in the lab's user directory: rt_dump-scene (scene mode) or rt_dump-<frame> (frames).
    $labDir = $null
    if ($null -ne $Cell.meta -and $null -ne $Cell.meta.lab) { $labDir = $Cell.meta.lab.dir }
    $dumps = @()
    if ($labDir -and (Test-Path -LiteralPath $labDir)) {
        $userDir = Join-Path $labDir 'user'
        if (Test-Path -LiteralPath $userDir) {
            $dumps = @(Get-ChildItem -LiteralPath $userDir -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -ne 'rt_dump' })
        }
    }
    foreach ($dump in $dumps) {
        $captureDirs = @()
        $inner = @(Get-ChildItem -LiteralPath $dump.FullName -Directory -Filter 'capture_*' -ErrorAction SilentlyContinue)
        if ($inner.Count -gt 0) { $captureDirs = $inner } else { $captureDirs = @($dump) }
        foreach ($dir in $captureDirs) {
            $name = if ($dir.FullName -eq $dump.FullName) { $dump.Name } else { $dump.Name + '/' + $dir.Name }
            if ($folders.Contains($name)) { continue }
            $folders.Add($name)
            $images = New-Object System.Collections.Generic.List[object]
            foreach ($png in @(Get-ChildItem -LiteralPath $dir.FullName -File -Filter '*.png' -ErrorAction SilentlyContinue)) {
                $rel = $name + '/' + $png.Name
                $dest = Join-Path $bundle ('images/' + $rel.Replace('/', '\'))
                $destDir = Split-Path -Parent $dest
                New-Item -ItemType Directory -Force -Path $destDir | Out-Null
                Copy-Item -LiteralPath $png.FullName -Destination $dest -Force
                $images.Add([ordered]@{ name = $rel; bytes = $png.Length; sha256 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash })
            }
            if ($images.Count -gt 0) {
                $captures.Add([ordered]@{ name = $name; offset_from_scene_s = $null; phase = 'lab-dump'; folder = 'images'; images = [object[]]$images })
            }
        }
    }

    # 2) Captures the harness saved itself (result.captures, frames/...).
    if ($null -ne $Cell.result -and $null -ne $Cell.result.captures) {
        $i = 0
        foreach ($cap in @($Cell.result.captures)) {
            $i++
            $capName = if ($cap.name) { [string]$cap.name } else { 'harness-' + $i }
            if ($cap.kind -eq 'renderdoc') { continue }
            if ($folders.Contains($capName)) { continue }
            $folders.Add($capName)
            $images = New-Object System.Collections.Generic.List[object]
            foreach ($img in @($cap.images)) {
                $file = [string]$img.file
                if (-not $file) { continue }
                $src = Join-Path $Cell.run_dir ($file -replace '/', '\')
                if (-not (Test-Path -LiteralPath $src)) { continue }
                $rel = $capName + '/' + (Split-Path -Leaf $src)
                $dest = Join-Path $bundle ('images/' + $rel.Replace('/', '\'))
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
                Copy-Item -LiteralPath $src -Destination $dest -Force
                $images.Add([ordered]@{ name = $rel; bytes = (Get-Item -LiteralPath $dest).Length; sha256 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash })
            }
            if ($images.Count -gt 0) {
                $captures.Add([ordered]@{ name = $capName; offset_from_scene_s = $null; phase = [string]$cap.phase; folder = 'images'; images = [object[]]$images })
            }
        }
    }

    # 3) A bench-style capture run: captures.json next to the images (used when analysing existing
    #    capture runs, and by -SelfTestB).
    if ($captures.Count -eq 0) {
        $manifestPath = Join-Path $Cell.run_dir 'captures.json'
        $man = Read-JsonFile $manifestPath
        if ($null -ne $man) {
            foreach ($cap in @($man.captures)) {
                $capName = [string]$cap.name
                if ($folders.Contains($capName)) { continue }
                $folders.Add($capName)
                $images = New-Object System.Collections.Generic.List[object]
                foreach ($img in @($cap.images)) {
                    $src = Join-Path $Cell.run_dir (([string]$cap.folder -replace '/', '\') + '\' + [string]$img.name)
                    if (-not (Test-Path -LiteralPath $src)) { continue }
                    $rel = $capName + '/' + [string]$img.name
                    $dest = Join-Path $bundle ('images/' + $rel.Replace('/', '\'))
                    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
                    Copy-Item -LiteralPath $src -Destination $dest -Force
                    $images.Add([ordered]@{ name = $rel; bytes = (Get-Item -LiteralPath $dest).Length; sha256 = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash })
                }
                if ($images.Count -gt 0) {
                    $captures.Add([ordered]@{ name = $capName; offset_from_scene_s = $cap.offset_from_scene_s; phase = 'captures-json'; folder = 'images'; images = [object[]]$images })
                }
            }
        }
    }

    if ($captures.Count -eq 0) { return [ordered]@{ ok = $false; reason = 'no render-target dumps found in the lab, the run directory or captures.json'; dir = $bundle; captures = @() } }
    $manifest = [ordered]@{
        label = $Label; generated_by = 'run-validation.ps1'; dump_mode = $BDumpMode
        captures = $captures
    }
    Write-JsonFile (Join-Path $bundle 'captures.json') $manifest
    return [ordered]@{ ok = $true; dir = $bundle; captures = [object[]]$captures }
}

function Invoke-Python([string[]]$Argv) {
    $raw = & $script:Python @Argv 2>&1
    $text = ($raw | Out-String)
    return [ordered]@{ code = $LASTEXITCODE; text = $text }
}

function Compare-Bundles([string]$A, [string]$B, [string]$OutFile) {
    $res = Invoke-Python @($script:CaptureCompare, $A, $B, '--out', $OutFile)
    if ($res.code -ne 0) { return [ordered]@{ ok = $false; error = $res.text } }
    $json = Read-JsonFile $OutFile
    if ($null -eq $json) { return [ordered]@{ ok = $false; error = 'capture-compare wrote no parsable json' } }
    return [ordered]@{ ok = $true; data = $json }
}

function Get-PairImageMetrics($CompareData, [string]$DumpName, [string]$ImageName) {
    foreach ($entry in @($CompareData.compare)) {
        if ($entry.dump -ne $DumpName) { continue }
        if (@($entry.images_only_a) -contains $ImageName -or @($entry.images_only_b) -contains $ImageName) {
            return [ordered]@{ present = $false; only_one_side = $true }
        }
        foreach ($d in @($entry.differing)) {
            if ($d.image -eq $ImageName) {
                return [ordered]@{ present = $true; identical = $false; mean_abs = $d.mean_abs; rms = $d.rms; ssim = $d.ssim; masked_equal = $d.masked_equal }
            }
        }
        # Not in "differing" -> it was identical.
        return [ordered]@{ present = $true; identical = $true; mean_abs = 0.0; rms = 0.0; ssim = 1.0; masked_equal = $true }
    }
    return [ordered]@{ present = $false; missing_dump = $true }
}

function Select-HmdEyeImage($Names, [string]$Addr = '') {
    $hmd = @()
    foreach ($n in @($Names)) {
        $e = Get-HmdImageEntry ([string]$n)
        if ($null -ne $e -and $e.hmd -and -not $e.alpha) { $hmd += [string]$n }
    }
    if ($hmd.Count -eq 0) { return $null }
    if ($Addr) {
        $bare = $Addr -replace '^0x', ''
        $pref = @($hmd | Where-Object { $_ -like ('*_' + $bare + '_*') })
        if ($pref.Count -gt 0) { return $pref[0] }
    }
    $sbs = @($hmd | Where-Object { $_ -match '_5760x3240_' })
    if ($sbs.Count -gt 0) { return $sbs[0] }
    return $hmd[0]
}

function Get-ImagePairMetrics([string]$BundleA, [string]$FullA, [string]$BundleB, [string]$FullB) {
    $pa = Join-BundlePath $BundleA $FullA
    $pb = Join-BundlePath $BundleB $FullB
    if (-not ((Test-Path -LiteralPath $pa) -and (Test-Path -LiteralPath $pb))) {
        return [ordered]@{ present = $false; reason = 'file missing' }
    }
    $res = Invoke-Python @($script:CaptureDiff, $pa, $pb, '--grid', '3')
    if ($res.code -ne 0) { return [ordered]@{ present = $false; reason = 'capture-diff failed' } }
    try { $d = ($res.text | ConvertFrom-Json) } catch { return [ordered]@{ present = $false; reason = 'capture-diff output unparsable' } }
    return [ordered]@{
        present = $true; identical = [bool]$d.identical; mean_abs = $d.mean_abs_diff; rms = $d.rms_diff
        ssim = $d.ssim_global; masked_equal = [bool]$d.masked_hash.equal; paired_by_identity = $true
    }
}

function Get-EyeStats($Paths) {
    if (-not (Test-Path $script:EyeHelper)) { return @() }
    $res = Invoke-Python (@($script:EyeHelper, 'stats') + $Paths)
    try { return @($res.text | ConvertFrom-Json) } catch { return @() }
}

function Split-EyeImage([string]$Png, [string]$OutDir, [string]$Stem) {
    if (-not (Test-Path $script:EyeHelper)) { return $null }
    $res = Invoke-Python @($script:EyeHelper, 'split', $Png, $OutDir, '--stem', $Stem)
    try { return ($res.text | ConvertFrom-Json) } catch { return $null }
}

# --- harness invocation -----------------------------------------------------------------------
function Get-CellPlan {
    $plan = New-Object System.Collections.Generic.List[object]
    if ($script:UsePair) {
        $plan.Add([ordered]@{ mode = 'P'; label = ('{0}-P-off-1' -f $script:Prefix); arm = 'off'; cell_mode = 'cell'; gate = $Gate; extra = @(); attempts = $Attempts; why = 'smoke pair: OFF arm must reach gameplay' })
        $plan.Add([ordered]@{ mode = 'P'; label = ('{0}-P-on-1' -f $script:Prefix); arm = 'on'; cell_mode = 'cell'; gate = $Gate; extra = @(); attempts = [math]::Min($Attempts, 2); why = 'recycler pair: ON arm checked for a menu stall (input verified)' })
        return $plan
    }
    # A is now the depth/barrier configuration comparison on the gameplay scene, recycler OFF:
    # base = the build defaults (depth pass ON, barrier in pass ON), repeated for the noise floor;
    # legacy = SHADPS4_DBG_DEPTH_PASS=0 + SHADPS4_DBG_BARRIER_IN_PASS=0. Both arms capture the
    # post-window gameplay eye image for B.
    $aBand = @{ min_mpx = 400; max_mpx = 1200; min_passes = 30; max_passes = 75 }
    $plan.Add([ordered]@{ mode = 'A'; label = ('{0}-A-base-1' -f $script:Prefix); arm = 'off'; cell_mode = 'cell'; gate = $Gate; extra = @('-Capture'); env = @{}; band = $aBand; why = 'A base (defaults), repeat 1 for the pixel floor' })
    $plan.Add([ordered]@{ mode = 'A'; label = ('{0}-A-base-2' -f $script:Prefix); arm = 'off'; cell_mode = 'cell'; gate = $Gate; extra = @('-Capture'); env = @{}; band = $aBand; why = 'A base (defaults), repeat 2 for the pixel floor' })
    $plan.Add([ordered]@{ mode = 'A'; label = ('{0}-A-legacy-1' -f $script:Prefix); arm = 'off'; cell_mode = 'cell'; gate = $Gate; extra = @('-Capture'); env = @{ SHADPS4_DBG_DEPTH_PASS = '0'; SHADPS4_DBG_BARRIER_IN_PASS = '0' }; band = $aBand; why = 'A legacy (depth pass + barrier in pass off)' })
    if ($Modes -contains 'C') {
        $plan.Add([ordered]@{ mode = 'C'; label = ('{0}-C-on-1' -f $script:Prefix); arm = 'on'; cell_mode = 'cell'; gate = $Gate; extra = @('-ExitAfterSeconds', "$ExitAfterSeconds", '-CleanExit'); env = @{}; seconds = 20; band = @{ min_mpx = 450; max_mpx = 800; min_passes = 30; max_passes = 75 }; why = 'R6 clean shutdown, recycler on (short window so the timed quit wins)' })
    }
    if ($Modes -contains 'D') {
        $plan.Add([ordered]@{ mode = 'D'; label = ('{0}-D-off-rdoc' -f $script:Prefix); arm = 'off'; cell_mode = 'rdoc'; gate = $Gate; extra = @('-RdocScene', $BRdocScene); env = @{}; why = 'RenderDoc scene-gated capture, recycler off' })
    }
    if ($Modes -contains 'E') {
        # E uses the heavy gate, not the 300%-derived menu->loading->gameplay transition gate: the
        # 100% menu is ~465 draws, so SceneMinDraws 800 skips menu+loading and the first heavy
        # phase is gameplay. The captured image is checked before the numbers are accepted.
        $plan.Add([ordered]@{ mode = 'E'; label = ('{0}-E-off-lowres' -f $script:Prefix); arm = 'off'; cell_mode = 'lowres'; gate = 'heavy'; extra = @('-Capture', '-SyntheticHands', 'on', '-Autopress', '-AutopressSeconds', '45'); env = @{}
            band = @{ min_mpx = 55; max_mpx = 90; min_passes = 30; max_passes = 55 }
            scene_min_draws = 800
            scene_min_mpx = 50
            expect = @{ eye = '960x1080'; image = '1920x1080'; format = 'R8G8B8A8Unorm'; samples = 1 }
            why = '100 % render / 120 Hz cell, heavy gate (first heavy phase is gameplay)' })
    }
    return $plan
}

function Get-HarnessArgs($Item) {
    $percent = if ($Item.cell_mode -eq 'lowres') { 100 } else { $RenderPercent }
    $hz = if ($Item.cell_mode -eq 'lowres') { 120 } else { $Hz }
    $args = @('-NoProfile', '-File', $Harness, '-Exe', $Exe, '-Label', $Item.label,
        '-Arm', $Item.arm, '-Mode', $Item.cell_mode, '-Gate', $Item.gate,
        '-RenderPercent', "$percent", '-Hz', "$hz", '-Seconds', "$(if ($null -ne $Item.seconds) { $Item.seconds } else { $Seconds })", '-Warmup', "$Warmup",
        '-MaxSeconds', "$MaxSeconds", '-SceneMinDraws', "$(if ($null -ne $Item.scene_min_draws) { $Item.scene_min_draws } else { $SceneMinDraws })",
        '-SceneMinMpx', "$(if ($null -ne $Item.scene_min_mpx) { $Item.scene_min_mpx } else { $SceneMinMpx })",
        '-SceneSamples', "$SceneSamples", '-GameplayMinDraws', "$GameplayMinDraws",
        '-SettleTolerancePct', "$(if ($null -ne $Item.settle_tolerance) { $Item.settle_tolerance } else { 5 })",
        '-BandMinDraws', "$BandMinDraws", '-BandMaxDraws', "$BandMaxDraws",
        '-BandMinMpx', "$(if ($null -ne $Item.band) { $Item.band.min_mpx } else { $BandMinMpx })",
        '-BandMaxMpx', "$(if ($null -ne $Item.band) { $Item.band.max_mpx } else { $BandMaxMpx })",
        '-BandMinPasses', "$(if ($null -ne $Item.band) { $Item.band.min_passes } else { $BandMinPasses })",
        '-BandMaxPasses', "$(if ($null -ne $Item.band) { $Item.band.max_passes } else { $BandMaxPasses })")
    $attempts = if ($null -ne $Item.attempts) { [int]$Item.attempts } else { $Attempts }
    $args += @('-Attempts', "$attempts")
    if ($null -ne $Item.env -and $Item.env.Count -gt 0) {
        $pairs = @($Item.env.Keys | ForEach-Object { '{0}={1}' -f $_, $Item.env[$_] })
        $args += @('-Env', ($pairs -join ';'))
    }
    if ($null -ne $Item.expect) {
        $args += @('-ExpectEye', $Item.expect.eye, '-ExpectImage', $Item.expect.image, '-ExpectFormat', $Item.expect.format, '-ExpectSamples', "$($Item.expect.samples)")
    }
    if ($InputScript -ne '') { $args += @('-InputScript', $InputScript) }
    # Only pass the gameplay reference when it exists: the harness's image gate treats a missing
    # reference as "not yet validated" and still requires the gameplay image to differ from the
    # menu baseline. Once the reference is set, it is picked up automatically.
    if ($GameplayReference -ne '' -and (Test-Path -LiteralPath $GameplayReference)) { $args += @('-GameplayReference', $GameplayReference) }
    if ($SetGameplayReference) { $args += @('-SetGameplayReference') }
    if ($null -ne $Item.extra) { $args += @($Item.extra) }
    return $args
}

function Invoke-Cell($Item) {
    $label = $Item.label
    if ($null -ne $Item.band) { $script:CellBand[$label] = $Item.band }
    $logDir = Join-Path $script:CellsDir $label
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $stdout = Join-Path $logDir 'cell.stdout.txt'
    $hargs = Get-HarnessArgs $Item
    $envExtra = @{}
    if ($null -ne $Item.env) { $envExtra = $Item.env }
    if ($Item.mode -eq 'B') {
        if ($BDumpMode -eq 'scene') { $hargs += @('-HmdDumpAtScene', "$GameplayMinDraws,$BandMinMpx") }
        elseif ($BDumpFrames -ne '') { $hargs += @('-DumpFrames', $BDumpFrames) }
    }

    $started = Get-Date
    try {
        if ($NoLock) {
            & pwsh.exe @hargs 2>&1 | Tee-Object -FilePath $stdout | ForEach-Object { Write-Host $_ }
            $code = $LASTEXITCODE
        } else {
            $lockArgs = @('-e', 'bash', $script:LockLinux, 'run', '--owner', 'validate-eng', '--kind', 'headless',
                '--reason', ('validation ' + $Item.mode + ': ' + $Item.why), '--ttl', '1800', '--require-quiet', '--quiet-wait', "$QuietWaitSeconds", '--',
                'pwsh.exe') + $hargs
            & wsl.exe @lockArgs 2>&1 | Tee-Object -FilePath $stdout | ForEach-Object { Write-Host $_ }
            $code = $LASTEXITCODE
        }
    } catch {
        Write-Warning ("cell {0} failed to start: {1}" -f $label, $_.Exception.Message)
        $code = -1
    }
    $ended = Get-Date
    return [ordered]@{
        label = $label; mode = $Item.mode; arm = $Item.arm; cell_mode = $Item.cell_mode; gate = $Item.gate
        why = $Item.why; exit_code = $code; started = $started.ToString('o'); ended = $ended.ToString('o')
        wall_seconds = [math]::Round(($ended - $started).TotalSeconds, 1)
        harness_args = $hargs; env = $envExtra; stdout = $stdout
    }
}

# --- recycler pair: OFF gameplay vs ON checked for a regression --- -------------------------------------
function Invoke-RecyclerPair {
    $items = @(Get-CellPlan)
    $recs = New-Object System.Collections.Generic.List[object]
    foreach ($item in $items) {
        $run = Invoke-Cell $item
        $cell = Measure-Cell $item.label
        Write-JsonFile (Join-Path (Join-Path $script:CellsDir $item.label) 'cell.json') ([ordered]@{ run = $run; cell = $cell })
        $recs.Add([pscustomobject]@{ item = $item; run = $run; cell = $cell })
    }
    $off = $recs[0].cell; $on = $recs[1].cell
    $result = Get-RecyclerPairVerdict $off $on
    $result['generated'] = (Get-Date).ToString('o')
    $result['exit_codes'] = @($recs | ForEach-Object { $_.run.exit_code })
    Write-PairReport $result
    return $result
}

function Get-RecyclerPairVerdict($off, $on) {
    $offG = ($null -ne $off.measured -and $off.measured.phase -eq 'gameplay')
    $onG = ($null -ne $on.measured -and $on.measured.phase -eq 'gameplay')
    $status = 'INCONCLUSIVE'
    $finding = $null
    if ($offG -and -not $onG) {
        $status = 'RECYCLER-REGRESSION-SUSPECTED'
        $finding = 'the recycler-ON arm never left the menu while the recycler-OFF arm reached gameplay; reported as a possible correctness regression only with the input-execution evidence recorded in the ON cell (presses ran, live file rewritten, no loading dip). The recycler stays off; the ON cell is invalid for timing.'
    } elseif ($offG -and $onG) {
        $status = 'BOTH-REACHED'
    } elseif (-not $offG) {
        $status = 'OFF-DID-NOT-REACH'
        $finding = 'the recycler-OFF arm did not reach gameplay either: no conclusion about ON; fix the scene entry or retry before reading anything into the ON arm.'
    }
    return [ordered]@{
        generated = (Get-Date).ToString('o'); status = $status; finding = $finding
        off = [ordered]@{ label = $off.label; valid = $off.measured.valid; phase = $off.measured.phase; reason = $off.measured.reason
            draws = $(if ($off.gameplay_gate) { $off.gameplay_gate.draws } else { $null }); mpx = $(if ($off.gameplay_gate) { $off.gameplay_gate.mpx } else { $null })
            passes = $(if ($off.gameplay_gate) { $off.gameplay_gate.passes } else { $null }); fps_p50 = $off.measured.fps_p50; frames = $off.measured.frames
            recycler_hits = $off.measured.recycler_hits_per_frame; recycler_misses = $off.measured.recycler_misses_per_frame
            menu_stuck = $off.menu_stuck; save = $off.save; log = $off.menu_stuck.log }
        on = [ordered]@{ label = $on.label; valid = $on.measured.valid; phase = $on.measured.phase; reason = $on.measured.reason
            draws = $(if ($on.gameplay_gate) { $on.gameplay_gate.draws } else { $null }); mpx = $(if ($on.gameplay_gate) { $on.gameplay_gate.mpx } else { $null })
            passes = $(if ($on.gameplay_gate) { $on.gameplay_gate.passes } else { $null }); fps_p50 = $on.measured.fps_p50; frames = $on.measured.frames
            recycler_hits = $on.measured.recycler_hits_per_frame; recycler_misses = $on.measured.recycler_misses_per_frame
            menu_stuck = $on.menu_stuck; save = $on.save; log = $on.menu_stuck.log }
    }
}

function Write-PairReport($result) {
    Write-JsonFile (Join-Path $script:Out 'RECYCLER-PAIR.json') $result
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# Recycler pair — OFF must reach gameplay, ON checked for a regression')
    $lines.Add('')
    $lines.Add(('Generated {0}. Status: **{1}**. {2}' -f (Get-Date).ToString('o'), $status, $finding))
    $lines.Add('')
    $lines.Add('| arm | label | phase | valid | draws | Mpx | passes | fps p50 | frames | hits/f | misses/f | menu stuck | log |')
    $lines.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($r in @($result.off, $result.on)) {
        $lines.Add(('| {0} | `{1}` | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | `{12}` |' -f `
                    $(if ($r.label -like '*-on-*') { 'on' } else { 'off' }), $r.label, $r.phase, $r.valid, (Format-Num $r.draws '0'), (Format-Num $r.mpx '0'), (Format-Num $r.passes '0'), (Format-Num $r.fps_p50), $r.frames, (Format-Num $r.recycler_hits '0.000'), (Format-Num $r.recycler_misses '0.000'), $(if ($r.menu_stuck) { $r.menu_stuck.stayed_on_menu } else { $null }), $r.log))
    }
    $lines.Add('')
    $lines.Add('A menu-stuck ON cell is only reported as a possible regression after the input execution is verified: the input script copy, the AUTOPRESS/move log lines, the live file rewritten at the scene anchor, and the absence of a loading dip. Its menu fps is never quoted against gameplay. The earlier stuck reports were a harness live-input bug, not renderer findings. Save provenance is in `SAVE.json`; every cell records the lab save hashes in `meta.save_data`.')
    $lines.Add('')
    Write-TextFile (Join-Path $script:Out 'RECYCLER-PAIR.md') (($lines -join "`r`n") + "`r`n")
}

# --- verdicts ---------------------------------------------------------------------------------
function Test-ArmAgreement($Cells, [double]$TolerancePct = 5.0) {
    $rows = @($Cells | Where-Object { $null -ne $_ -and $_.measured -and $_.measured.valid })
    if ($rows.Count -lt 2) { return [ordered]@{ ok = $false; reason = 'fewer than two valid cells'; rows = @() } }
    $dMean = Get-MeanOf @($rows | ForEach-Object { $_.measured.draws_mean })
    $mMean = Get-MeanOf @($rows | ForEach-Object { $_.measured.mpx_mean })
    $bad = New-Object System.Collections.Generic.List[string]
    foreach ($c in $rows) {
        $dPct = if ($dMean -gt 0) { [math]::Abs([double]$c.measured.draws_mean - $dMean) / $dMean * 100.0 } else { 0 }
        $mPct = if ($mMean -gt 0) { [math]::Abs([double]$c.measured.mpx_mean - $mMean) / $mMean * 100.0 } else { 0 }
        if ($dPct -gt $TolerancePct) { $bad.Add(('{0}: draws {1:0.0} vs mean {2:0.0} ({3:0.0} %)' -f $c.label, $c.measured.draws_mean, $dMean, $dPct)) }
        if ($mPct -gt $TolerancePct) { $bad.Add(('{0}: Mpx {1:0.0} vs mean {2:0.0} ({3:0.0} %)' -f $c.label, $c.measured.mpx_mean, $mMean, $mPct)) }
    }
    return [ordered]@{ ok = ($bad.Count -eq 0); reason = ($bad -join '; '); draws_mean_all = Round1 $dMean; mpx_mean_all = Round1 $mMean; rows = $rows.Count }
}

function Get-Acceptance($Cells) {
    $valid = @($Cells | Where-Object { $null -ne $_ -and $_.measured -and $_.measured.valid })
    $base = @($valid | Where-Object { $_.label -like '*-A-base-*' })
    $legacy = @($valid | Where-Object { $_.label -like '*-A-legacy-*' })
    $result = [ordered]@{
        cells = @($Cells | ForEach-Object { $_.label })
        valid = @($valid | ForEach-Object { $_.label })
        discarded = @($Cells | Where-Object { $null -eq $_ -or $null -eq $_.measured -or -not $_.measured.valid } | ForEach-Object { $_.label })
        base_cells = $base.Count
        legacy_cells = $legacy.Count
        arm_agreement = Test-ArmAgreement $base
        floor = $null; fps = $null; alloc = $null; gameplay_gate = $null
        status = 'PENDING'; reasons = @()
    }
    $reasons = New-Object System.Collections.Generic.List[string]

    # Pixel/behaviour floor: two base cells of the same build and configuration.
    if ($base.Count -ge 2) {
        $a = $base[0]; $b = $base[1]
        $result.floor = [ordered]@{ pair = @($a.label, $b.label); fps_p50_delta = $null; fps_p50_spread_pct = $null; draws_delta_pct = $null; mpx_delta_pct = $null }
        $result.floor.fps_p50_delta = [math]::Round([math]::Abs([double]$a.measured.fps_p50 - [double]$b.measured.fps_p50), 2)
        if ([double]$a.measured.fps_p50 -gt 0) { $result.floor.fps_p50_spread_pct = [math]::Round(100.0 * $result.floor.fps_p50_delta / [double]$a.measured.fps_p50, 2) }
        $dM = Get-MeanOf @($a.measured.draws_mean, $b.measured.draws_mean)
        if ($dM -gt 0) { $result.floor.draws_delta_pct = [math]::Round([math]::Abs($a.measured.draws_mean - $b.measured.draws_mean) / $dM * 100.0, 2) }
        $mM = Get-MeanOf @($a.measured.mpx_mean, $b.measured.mpx_mean)
        if ($mM -gt 0) { $result.floor.mpx_delta_pct = [math]::Round([math]::Abs($a.measured.mpx_mean - $b.measured.mpx_mean) / $mM * 100.0, 2) }
    }

    # Configuration comparison: legacy (both switches =0) against the base mean.
    if ($legacy.Count -ge 1 -and $base.Count -ge 1) {
        $l = $legacy[0]
        $baseP50 = Get-MeanOf @($base | ForEach-Object { $_.measured.fps_p50 })
        $delta = [double]$l.measured.fps_p50 - [double]$baseP50
        $spread = if ($null -ne $result.floor -and $null -ne $result.floor.fps_p50_delta) { [double]$result.floor.fps_p50_delta } else { 0 }
        $gpuBound = ($null -ne $l.measured.gpu_busy_mean -and [double]$l.measured.gpu_busy_mean -ge 95)
        $result.fps = [ordered]@{
            base_mean_p50 = Round3 $baseP50; legacy_p50 = $l.measured.fps_p50
            delta_p50 = Round3 $delta
            delta_pct = $(if ([double]$baseP50 -gt 0) { [math]::Round(100.0 * $delta / [double]$baseP50, 2) } else { $null })
            floor_spread_p50 = Round3 $spread
            beyond_floor = ([math]::Abs($delta) -gt $spread)
            gpu_bound = $gpuBound
            note = $(if ($gpuBound) { 'GPU busy >= 95 %: a CPU-side difference is not expected to show as fps' } else { '' })
        }
        $result.alloc = [ordered]@{
            base_images_created_per_frame = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.images_created_per_frame }))
            legacy_images_created_per_frame = Round3 $l.measured.images_created_per_frame
            base_create_ms_per_frame = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.create_ms_per_frame }))
            legacy_create_ms_per_frame = Round3 $l.measured.create_ms_per_frame
            base_destroy_ms_per_frame = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.destroy_ms_per_frame }))
            legacy_destroy_ms_per_frame = Round3 $l.measured.destroy_ms_per_frame
            base_passes_mean = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.passes_mean }))
            legacy_passes_mean = Round3 $l.measured.passes_mean
            base_mpx_mean = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.mpx_mean }))
            legacy_mpx_mean = Round3 $l.measured.mpx_mean
            base_refresh_uploads = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.refresh_uploads }))
            legacy_refresh_uploads = Round3 $l.measured.refresh_uploads
            base_vram_mb = Round3 (Get-MeanOf @($base | ForEach-Object { $_.measured.vram_mb_mean }))
            legacy_vram_mb = Round3 $l.measured.vram_mb_mean
        }
    }

    if (-not $result.arm_agreement.ok) { $reasons.Add('base cells do not agree on draws/Mpx within 5 %: ' + $result.arm_agreement.reason) }
    # Scene identity across configurations: the draws must match even though the legacy config
    # changes Mpx/passes by design (that change is the effect being measured, not a mismatch).
    $result.scene_identity = $null
    if ($base.Count -ge 1 -and $legacy.Count -ge 1) {
        $bDraws = Get-MeanOf @($base | ForEach-Object { $_.measured.draws_mean })
        $lDraws = Get-MeanOf @($legacy | ForEach-Object { $_.measured.draws_mean })
        $dPct = if ($bDraws -gt 0) { [math]::Round(100.0 * [math]::Abs($lDraws - $bDraws) / $bDraws, 2) } else { $null }
        $result.scene_identity = [ordered]@{
            ok = ($null -ne $dPct -and $dPct -le 5.0)
            base_draws = Round1 $bDraws; legacy_draws = Round1 $lDraws; draws_delta_pct = $dPct
            note = 'draws are the scene identity; Mpx/passes differ by configuration by design'
        }
        if (-not $result.scene_identity.ok) { $reasons.Add(('legacy and base draws differ by {0} % (>5 %): not the same scene' -f $dPct)) }
    }
    $gateBad = @($valid | Where-Object { $null -eq $_.gameplay_gate -or -not $_.gameplay_gate.ok })
    foreach ($c in $gateBad) {
        $g = $c.gameplay_gate
        $reasons.Add(('{0}: not the gameplay signature (draws {1}, Mpx {2}, passes {3}; gate >= {4} draws, band {5})' -f $c.label, $g.draws, $g.mpx, $g.passes, $g.gameplay_min_draws, $g.band))
    }
    $result.gameplay_gate = [ordered]@{
        ok = ($gateBad.Count -eq 0)
        band = ("draws {0}-{1}, Mpx {2}-{3}, passes {4}-{5}" -f $BandMinDraws, $BandMaxDraws, $BandMinMpx, $BandMaxMpx, $BandMinPasses, $BandMaxPasses)
        gameplay_min_draws = $GameplayMinDraws
        cells = @($valid | ForEach-Object { [ordered]@{ label = $_.label; draws = $_.gameplay_gate.draws; mpx = $_.gameplay_gate.mpx; passes = $_.gameplay_gate.passes; ok = $_.gameplay_gate.ok } })
    }
    # Every A cell is recycler OFF; a non-zero recycler counter means the wrong arm ran.
    $badArms = @($valid | Where-Object { -not $_.arm_evidence.ok })
    foreach ($c in $badArms) { $reasons.Add(('{0}: arm evidence wrong ({1})' -f $c.label, $c.arm_evidence.note)) }
    $nonGameplay = @($valid | Where-Object { $_.measured.phase -ne 'gameplay' })
    foreach ($c in $nonGameplay) { $reasons.Add(('{0}: phase is {1}, not gameplay' -f $c.label, $_.measured.phase)) }
    if ($result.discarded.Count -gt 0) { $reasons.Add('discarded cells: ' + ($result.discarded -join ', ')) }
    if ($base.Count -lt 2) { $reasons.Add('fewer than two base cells: no pixel/fps floor') }
    if ($legacy.Count -lt 1) { $reasons.Add('no legacy (depth/barrier = 0) cell') }
    $result.reasons = @($reasons)
    if ($reasons.Count -eq 0) { $result.status = 'PASS' } else { $result.status = 'FAIL/INVALID' }
    return $result
}

function Test-BContent($Off1, $Off2, $On) {
    $result = [ordered]@{
        status = 'PENDING'; dump_mode = $BDumpMode; dump_frames = $BDumpFrames; reasons = @(); scene_unavailable = $false
        floor = $null; targets = @(); images = @(); verdicts = @(); evidence = [ordered]@{}
    }
    $reasons = New-Object System.Collections.Generic.List[string]
    $b1 = New-CaptureBundle $Off1.label $Off1
    $b2 = New-CaptureBundle $Off2.label $Off2
    $b3 = New-CaptureBundle $On.label $On
    $result.evidence = [ordered]@{ off1 = $b1.dir; off2 = $b2.dir; on = $b3.dir }
    if (-not ($b1.ok -and $b2.ok -and $b3.ok)) {
        $reasons.Add('no render-target dumps in at least one arm: ' + (@(@($b1.reason), @($b2.reason), @($b3.reason)) -join '; '))
        $result.reasons = @($reasons); $result.status = 'INCONCLUSIVE'
        return $result
    }
    $cmpOff = Compare-Bundles $b1.dir $b2.dir (Join-Path (Join-Path $script:CellsDir $Off1.label) 'compare-off1-off2.json')
    $cmpOn1 = Compare-Bundles $b1.dir $b3.dir (Join-Path (Join-Path $script:CellsDir $Off1.label) 'compare-off1-on.json')
    $cmpOn2 = Compare-Bundles $b2.dir $b3.dir (Join-Path (Join-Path $script:CellsDir $Off2.label) 'compare-off2-on.json')
    foreach ($pair in @(@('off1-vs-off2', $cmpOff), @('off1-vs-on', $cmpOn1), @('off2-vs-on', $cmpOn2))) {
        if (-not $pair[1].ok) { $reasons.Add($pair[0] + ' comparison failed: ' + ($pair[1].error -split "`n")[0]) }
    }
    if ($reasons.Count -gt 0) { $result.reasons = @($reasons); $result.status = 'INCONCLUSIVE'; return $result }
    $result.floor = [ordered]@{ a = $Off1.label; b = $Off2.label }
    $result.targets = @([ordered]@{ a = $Off1.label; b = $On.label }, [ordered]@{ a = $Off2.label; b = $On.label })

    # Which HMD images are present in all three bundles at the same dump and file name? The
    # capture-compare JSON only names the images that differ, so presence is read from the
    # bundles themselves and only then matched against the pair results.
    function Get-SelectedCapture($Bundle) {
        $caps = @($Bundle.captures)
        $g = @($caps | Where-Object { [string]$_.phase -eq 'gameplay' })
        $mode = 'dump'
        if ($g.Count -gt 0) { $caps = $g; $mode = 'gameplay' }
        if ($caps.Count -eq 0) { return [ordered]@{ ok = $false; mode = $mode; cap = $null } }
        return [ordered]@{ ok = $true; mode = $mode; cap = $caps[-1] }
    }
    function Get-CaptureImageEntries($Sel) {
        # Key by the image's identity (guest address, size, format, samples, alpha), not by the
        # dump order: the same target is index 010 in one run and 011 in another when a pass
        # creates one more image first. Pairing by file name would silently drop the HMD target.
        $m = @{}
        if (-not $Sel.ok) { return $m }
        foreach ($img in @($Sel.cap.images)) {
            $base = Split-Path -Leaf ([string]$img.name)
            $entry = Get-HmdImageEntry $base
            $key = if ($null -ne $entry) {
                '{0}|{1}x{2}|{3}|s{4}|a{5}' -f $entry.addr, $entry.width, $entry.height, $entry.format, $entry.samples, [int]$entry.alpha
            } else { $base }
            $m[$key] = [ordered]@{ dump = [string]$Sel.cap.name; full = [string]$img.name; entry = $entry }
        }
        return $m
    }
    $s1 = Get-SelectedCapture $b1
    $s2 = Get-SelectedCapture $b2
    $s3 = Get-SelectedCapture $b3
    $phaseMode = if ($s1.mode -eq 'gameplay' -and $s2.mode -eq 'gameplay' -and $s3.mode -eq 'gameplay') { 'gameplay' } else { 'dump' }
    $phases = [ordered]@{
        b1 = @($b1.captures | ForEach-Object { [string]$_.phase })
        b2 = @($b2.captures | ForEach-Object { [string]$_.phase })
        b3 = @($b3.captures | ForEach-Object { [string]$_.phase })
        compared = $phaseMode
    }
    $result.phases = $phases
    if (-not ($s1.ok -and $s2.ok -and $s3.ok)) {
        $reasons.Add('one or more capture bundles contain no dump at all')
        $result.reasons = @($reasons); $result.status = 'INCONCLUSIVE'
        return $result
    }
    if ($BDumpMode -eq 'harness' -and $phaseMode -ne 'gameplay') {
        $reasons.Add('the harness saved no gameplay-phase dump (only the menu baseline, if anything): the title never entered gameplay in these capture cells'); $result.scene_unavailable = $true
    }
    $e1 = Get-CaptureImageEntries $s1
    $e2 = Get-CaptureImageEntries $s2
    $e3 = Get-CaptureImageEntries $s3
    $map = @{}
    foreach ($key in $e1.Keys) {
        if (-not ($e2.ContainsKey($key) -and $e3.ContainsKey($key))) { continue }
        $entry = $e1[$key].entry
        if ($null -eq $entry -or -not $entry.hmd -or $entry.alpha) { continue }
        # The dump order can shift between configurations (an extra pass image), so pair by the
        # image identity and measure the two files directly; capture-compare is used for inventory.
        $f = Get-ImagePairMetrics $b1.dir $e1[$key].full $b2.dir $e2[$key].full
        $t1 = Get-ImagePairMetrics $b1.dir $e1[$key].full $b3.dir $e3[$key].full
        $t2 = Get-ImagePairMetrics $b2.dir $e2[$key].full $b3.dir $e3[$key].full
        if (-not ($f.present -and $t1.present -and $t2.present)) { continue }
        $map[$key] = [pscustomobject]@{ dump = $e1[$key].dump; image = $e1[$key].full; floor = $f; t1 = $t1; t2 = $t2 }
    }
    if ($map.Count -eq 0) {
        $reasons.Add('no HMD eye image (5760x3240 or 2880x3240 R8G8B8A8Unorm) is common to the off/off/on bundles: the dumps did not land in the same scene/frame')
        $result.reasons = @($reasons); $result.status = 'INCONCLUSIVE'
        return $result
    }

    $toleranceSsim = 0.002
    $toleranceMeanAbs = 0.002
    $pass = $true
    foreach ($key in ($map.Keys | Sort-Object)) {
        $row = $map[$key]
        $ok1 = ($row.t1.ssim -ge ($row.floor.ssim - $toleranceSsim)) -and ($row.t1.mean_abs -le ($row.floor.mean_abs + $toleranceMeanAbs))
        $ok2 = ($row.t2.ssim -ge ($row.floor.ssim - $toleranceSsim)) -and ($row.t2.mean_abs -le ($row.floor.mean_abs + $toleranceMeanAbs))
        $rowVerdict = if ($ok1 -and $ok2) { 'PASS' } else { 'FAIL' }
        if ($rowVerdict -eq 'FAIL') { $pass = $false }
        $result.images += [ordered]@{
            dump = $row.dump; image = $row.image
            floor_ssim = $row.floor.ssim; floor_mean_abs = $row.floor.mean_abs; floor_identical = $row.floor.identical
            on1_ssim = $row.t1.ssim; on1_mean_abs = $row.t1.mean_abs; on1_identical = $row.t1.identical; on1_masked_equal = $row.t1.masked_equal
            on2_ssim = $row.t2.ssim; on2_mean_abs = $row.t2.mean_abs; on2_masked_equal = $row.t2.masked_equal
            verdict = $rowVerdict
        }
        $result.verdicts += $rowVerdict
    }

    # Per-region SSIM + absolute luma stats for the eye image pairs (capture-diff.py).
    $regions = New-Object System.Collections.Generic.List[object]
    $mapKeys = @($map.Keys | Sort-Object)
    if ($mapKeys.Count -gt 0) {
        $addr = ''
        if ($null -ne $Off1.hmd -and $Off1.hmd.addr) { $addr = [string]$Off1.hmd.addr }
        $present = @($mapKeys | ForEach-Object { $map[$_].image })
        $eyeRel = Select-HmdEyeImage $present $addr
        if ($null -ne $eyeRel) {
            $stats = Get-EyeStats @((Join-BundlePath $b1.dir $eyeRel))
            if ($stats.Count -gt 0) { $result.eye_stats_off = $stats[0] }
            $splitDir = Join-Path $script:EvidenceDir 'eye-splits'
            $result.eye_split = Split-EyeImage (Join-BundlePath $b1.dir $eyeRel) $splitDir ('B-' + $Off1.label)
            $result.eye_image = $eyeRel
            # A black dump is the trap this whole comparison exists to avoid: identical black
            # images would read as a trivially perfect off/off floor and a perfect off/on match.
            # e3 is keyed by image identity, so find the common key whose side-a name is this image.
            $matchKey = $null
            foreach ($k in $mapKeys) { if ($e1[$k].full -eq $eyeRel) { $matchKey = $k; break } }
            $onBlack = $null
            if ($null -ne $matchKey -and $e3.ContainsKey($matchKey)) {
                $statsOn = Get-EyeStats @((Join-BundlePath $b3.dir $e3[$matchKey].full))
                if ($statsOn.Count -gt 0) { $result.eye_stats_on = $statsOn[0]; $onBlack = ($statsOn[0].black_frac -ge 0.99) }
            } else { $onBlack = $true }
            $offBlack = ($stats.Count -gt 0 -and $stats[0].black_frac -ge 0.99)
            if ($offBlack -or $onBlack) {
                $reasons.Add('the dumped eye image is black (black_frac >= 0.99) in at least one arm: an identical black dump is not pixel evidence for R3; the dump missed the scene')
                $result.status = 'INCONCLUSIVE'
                $result.reasons = @($reasons)
                foreach ($imgRow in @($result.images)) { $imgRow.verdict = 'INCONCLUSIVE-BLACK' }
                return $result
            }
        }
    }
    foreach ($key in $mapKeys) {
        $row = $map[$key]
        if ($row.image -match '_alpha\.png$') { continue }
        $pn1 = Join-BundlePath $b1.dir $row.image
        $pn3 = Join-BundlePath $b3.dir $row.image
        if (-not ((Test-Path $pn1) -and (Test-Path $pn3))) { continue }
        $diffOut = Join-Path (Join-Path $script:CellsDir $On.label) ('capture-diff-' + (($row.image -replace '[\\/]', '_') -replace '\.png$', '') + '.json')
        $res = Invoke-Python @($script:CaptureDiff, $pn1, $pn3, '--grid', '3')
        if ($res.code -eq 0) {
            try { $parsed = ($res.text | ConvertFrom-Json) } catch { $parsed = $null }
            if ($null -ne $parsed) {
                Write-JsonFile $diffOut $parsed
                $regions.Add([ordered]@{ image = $row.image; data = $parsed })
            }
        }
    }
    $result.region_diffs = [object[]]$regions

    if ($reasons.Count -gt 0) { $result.status = if ($result.scene_unavailable) { 'INCONCLUSIVE' } else { 'FAIL' } } elseif ($pass) { $result.status = 'PASS' } else { $result.status = 'FAIL' }
    $result.reasons = @($reasons)
    return $result
}

function Test-CShutdown($Cell) {
    $result = [ordered]@{ status = 'PENDING'; reasons = @(); checks = [ordered]@{} }
    $reasons = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Cell -or $null -eq $Cell.measured) {
        $result.status = 'BLOCKED'; $result.reasons = @('C cell missing')
        return $result
    }
    $m = $Cell.measured
    $shutdown = $m.shutdown
    $markers = $Cell.clean_markers
    $allMarkers = $true
    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($marker in $script:CleanMarkers) { if (-not $markers[$marker]) { $allMarkers = $false; $missing.Add($marker) } }
    $checks = [ordered]@{
        exit_code_zero = ($null -ne $shutdown.exit_code -and [int]$shutdown.exit_code -eq 0)
        exited_on_own = ($null -ne $shutdown.method -and [string]$shutdown.method -like '*exited on its own*')
        shutdown_ok = ($shutdown.ok -eq $true)
        within_bound = ($null -ne $shutdown.seconds -and [double]$shutdown.seconds -le 60)
        markers_present = $allMarkers
        missing_markers = @($missing)
        no_validation_errors = ($Cell.validation.Count -eq 0)
        no_crash_markers = ($Cell.crashes.Count -eq 0)
        r6_blocked = ($null -ne $Cell.r6_blocked -and @($Cell.r6_blocked | Where-Object { $_ }).Count -gt 0)
        assertion_failed = (@($Cell.validation | Where-Object { $_ -match 'Assertion Failed|assert_fail' }).Count -gt 0)
        teardown_documented = 'ImageRecycler::Shutdown() logs nothing; teardown is evidenced by the presenter- destruction markers (the destructor chain that calls it and vmaDestroyAllocator)'
        wall_seconds = $Cell.wall_seconds
    }
    $result.checks = $checks
    if ($checks.r6_blocked) {
        $reasons.Add('the build refused the clean teardown (guest threads did not pause / "R6 blocked"): the presenter was left alone, so ImageRecycler::Shutdown + vmaDestroyAllocator did not run and R6 remains unexercised')
        $result.status = 'BLOCKED'
    } elseif (-not $allMarkers -and $checks.exit_code_zero -and $checks.shutdown_ok) {
        $reasons.Add('the build ignored SHADPS4_CLEAN_EXIT / SHADPS4_EXIT_AFTER: no clean-exit markers in the log, so R6 (teardown ordering) cannot be assessed on this binary; this is a build-capability gap, not a crash')
        $result.status = 'BLOCKED'
    } elseif (-not ($checks.exit_code_zero -and $checks.shutdown_ok -and $checks.exited_on_own -and $checks.within_bound)) {
        $reasons.Add('shutdown did not complete cleanly: exit_code/shutdown_ok/exited_on_own/bound check failed')
        $result.status = 'FAIL'
    } elseif ($Cell.validation.Count -gt 0 -or $Cell.crashes.Count -gt 0) {
        $reasons.Add('validation errors or crash markers in the log: ' + (@($Cell.validation + $Cell.crashes) -join ' | '))
        $result.status = 'FAIL'
    } else {
        $result.status = 'PASS'
    }
    $result.reasons = @($reasons)
    return $result
}

function Test-DCapture($Cell, $GameplayAnchor) {
    $result = [ordered]@{ status = 'PENDING'; reasons = @(); capture = $null; checks = [ordered]@{} }
    $reasons = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Cell -or $null -eq $Cell.measured) {
        $result.status = 'BLOCKED'; $result.reasons = @('D cell missing')
        return $result
    }
    # The harness copies the .rdc while RenderDoc may still be writing it; the run's rdoc/ copy
    # is the complete one. Prefer the largest .rdc under the run directory.
    $rdcDir = Join-Path $Cell.run_dir 'rdoc'
    $rdc = $null
    if (Test-Path -LiteralPath $rdcDir) {
        $rdc = @(Get-ChildItem -LiteralPath $rdcDir -File -Filter '*.rdc' -ErrorAction SilentlyContinue | Sort-Object Length -Descending) | Select-Object -First 1
    }
    $checks = [ordered]@{
        rdc_dir = $rdcDir
        path = $(if ($rdc) { $rdc.FullName } else { $null })
        exists = ($null -ne $rdc)
        bytes = $(if ($rdc) { $rdc.Length } else { 0 })
        sha256 = $(if ($rdc) { (Get-FileHash -LiteralPath $rdc.FullName -Algorithm SHA256).Hash } else { $null })
        nontrivial_size = ($null -ne $rdc -and $rdc.Length -ge 10MB)
        renderdoc_thumb = $null
        rdoc_scene_gate = $Cell.rdoc_gate
        rdoc_trigger_s = $Cell.rdoc_trigger_s
        gameplay_anchor_s = $GameplayAnchor
        captured_after_gameplay_anchor = $null
        unreachable_code_in_log = (@($Cell.crashes | Where-Object { $_ -match 'Unreachable code|Protect' }).Count -gt 0)
    }
    if (-not $checks.exists) { $result.status = 'FAIL'; $result.reasons = @('no .rdc in the run''s rdoc/ directory'); return $result }
    if (-not $checks.nontrivial_size) { $reasons.Add(('capture is only {0} bytes (< 10 MiB)' -f $checks.bytes)) }
    $thumbDir = Join-Path (Join-Path $script:Out 'cells') $Cell.label
    [void][IO.Directory]::CreateDirectory($thumbDir)
    $thumb = Join-Path $thumbDir 'rdoc-thumbnail.png'
    if (-not (Test-Path -LiteralPath $thumbDir)) { $thumb = Join-Path $Cell.run_dir 'rdoc-thumbnail.png' }
    if (Test-Path 'C:\Program Files\RenderDoc\renderdoccmd.exe') {
        $thumbRes = & 'C:\Program Files\RenderDoc\renderdoccmd.exe' thumb ("--out=" + $thumb) $checks.path 2>&1
        $checks.renderdoc_thumb = [ordered]@{
            ok = ((Test-Path -LiteralPath $thumb) -and (Get-Item -LiteralPath $thumb).Length -gt 0)
            file = $thumb; bytes = $(if (Test-Path -LiteralPath $thumb) { (Get-Item -LiteralPath $thumb).Length } else { 0 })
            output = (($thumbRes | Out-String).Substring(0, [Math]::Min(400, ($thumbRes | Out-String).Length)))
        }
        if (-not $checks.renderdoc_thumb.ok) { $reasons.Add('renderdoccmd could not extract a thumbnail') }
    } else { $reasons.Add('renderdoccmd.exe not found at C:\Program Files\RenderDoc') }
    # Scene identity: the in-emulator gate must be above the menu (485 draws) — the instruction is
    # gate 1500,550, gameplay only — and the trigger must be at gameplay time.
    $gateOk = ($null -ne $checks.rdoc_scene_gate -and [double]$checks.rdoc_scene_gate.draws -ge 1500 -and [double]$checks.rdoc_scene_gate.mpx -ge 550)
    $checks.gate_above_menu = $gateOk
    if (-not $gateOk) { $reasons.Add(('RDOC scene gate {0}/{1} is below 1500/550 (or missing)' -f $checks.rdoc_scene_gate.draws, $checks.rdoc_scene_gate.mpx)) }
    if ($null -ne $checks.rdoc_trigger_s -and $null -ne $GameplayAnchor) {
        $checks.captured_after_gameplay_anchor = ([double]$checks.rdoc_trigger_s -ge [double]$GameplayAnchor)
        if (-not $checks.captured_after_gameplay_anchor) { $reasons.Add(('capture fired at {0:0.0} s, before the gameplay anchor {1:0.0} s' -f $checks.rdoc_trigger_s, $GameplayAnchor)) }
    } elseif ($null -eq $checks.rdoc_trigger_s) { $reasons.Add('the RDOC trigger line is missing from the log') }
    if ($checks.unreachable_code_in_log) { $checks['note'] = 'the log also contains an AddressSpace::Protect "Unreachable code" critical; it did not prevent the capture but is recorded' }
    $result.capture = $checks
    $result.checks = $checks
    $result.reasons = @($reasons)
    $result.status = if ($reasons.Count -eq 0) { 'PASS' } elseif ($gateOk -and $checks.nontrivial_size -and $checks.renderdoc_thumb.ok) { 'INVALID-SCENE' } else { 'FAIL' }
    return $result
}

function Test-CShutdown($Cell) {
    $result = [ordered]@{ status = 'PENDING'; reasons = @(); checks = [ordered]@{} }
    $reasons = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Cell -or $null -eq $Cell.measured) {
        $result.status = 'BLOCKED'; $result.reasons = @('C cell missing')
        return $result
    }
    $m = $Cell.measured
    $shutdown = $m.shutdown
    $markers = $Cell.clean_markers
    $allMarkers = $true
    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($marker in $script:CleanMarkers) { if (-not $markers[$marker]) { $allMarkers = $false; $missing.Add($marker) } }
    $checks = [ordered]@{
        exit_code_zero = ($null -ne $shutdown.exit_code -and [int]$shutdown.exit_code -eq 0)
        exited_on_own = ($null -ne $shutdown.method -and [string]$shutdown.method -like '*exited on its own*')
        shutdown_ok = ($shutdown.ok -eq $true)
        within_bound = ($null -ne $shutdown.seconds -and [double]$shutdown.seconds -le 60)
        markers_present = $allMarkers
        missing_markers = @($missing)
        no_validation_errors = ($Cell.validation.Count -eq 0)
        no_crash_markers = ($Cell.crashes.Count -eq 0)
        r6_blocked = ($null -ne $Cell.r6_blocked -and @($Cell.r6_blocked | Where-Object { $_ }).Count -gt 0)
        assertion_failed = (@($Cell.validation | Where-Object { $_ -match 'Assertion Failed|assert_fail' }).Count -gt 0)
        teardown_documented = 'ImageRecycler::Shutdown() logs nothing; teardown is evidenced by the presenter- destruction markers (the destructor chain that calls it and vmaDestroyAllocator)'
        wall_seconds = $Cell.wall_seconds
    }
    $result.checks = $checks
    if ($checks.r6_blocked) {
        $reasons.Add('the build refused the clean teardown (guest threads did not pause / "R6 blocked"): the presenter was left alone, so ImageRecycler::Shutdown + vmaDestroyAllocator did not run and R6 remains unexercised')
        $result.status = 'BLOCKED'
    } elseif (-not $allMarkers -and $checks.exit_code_zero -and $checks.shutdown_ok) {
        $reasons.Add('the build ignored SHADPS4_CLEAN_EXIT / SHADPS4_EXIT_AFTER: no clean-exit markers in the log, so R6 (teardown ordering) cannot be assessed on this binary; this is a build-capability gap, not a crash')
        $result.status = 'BLOCKED'
    } elseif (-not ($checks.exit_code_zero -and $checks.shutdown_ok -and $checks.exited_on_own -and $checks.within_bound)) {
        $reasons.Add('shutdown did not complete cleanly: exit_code/shutdown_ok/exited_on_own/bound check failed')
        $result.status = 'FAIL'
    } elseif ($Cell.validation.Count -gt 0 -or $Cell.crashes.Count -gt 0) {
        $reasons.Add('validation errors or crash markers in the log: ' + (@($Cell.validation + $Cell.crashes) -join ' | '))
        $result.status = 'FAIL'
    } else {
        $result.status = 'PASS'
    }
    $result.reasons = @($reasons)
    return $result
}

# --- markdown ---------------------------------------------------------------------------------
function Get-BuildIdentityTable {
    $lines = New-Object System.Collections.Generic.List[string]
    $headless = Join-Path $Root 'wt-headless'
    $wslHead = '/mnt/m/Development/Deracine-work/wt-headless'
    if (Test-Path $headless) {
        # The worktree's .git file contains a WSL path, so Windows git cannot open it; use WSL git.
        $head = (& wsl.exe -e git -C $wslHead rev-parse HEAD 2>&1 | Out-String).Trim()
        $dirty = @(& wsl.exe -e git -C $wslHead status --porcelain 2>&1).Count
        $changed = @(& wsl.exe -e git -C $wslHead diff 1a53ceb --name-only -- src/src/video_core 2>&1 | Where-Object { $_ -and $_ -notmatch '^fatal' })
        $recyclerFiles = @(
            'src/src/video_core/renderer_vulkan/vk_image_recycler.cpp',
            'src/src/video_core/renderer_vulkan/vk_image_recycler.h',
            'src/src/video_core/texture_cache/image.cpp',
            'src/src/video_core/texture_cache/image.h',
            'src/src/video_core/texture_cache/texture_cache.cpp',
            'src/src/video_core/texture_cache/texture_cache.h',
            'src/src/video_core/texture_cache/image_stats.h',
            'src/src/video_core/texture_cache/image_coverage.h',
            'src/src/video_core/renderer_vulkan/vk_instance.cpp'
        )
        $recyclerDiff = (& wsl.exe -e git -C $wslHead diff 1a53ceb -- @recyclerFiles 2>&1 | Out-String).Trim()
        $recyclerIdentical = ($recyclerDiff -eq '')
        $lines.Add('| build difference vs `1a53ceb` | evidence |')
        $lines.Add('|---|---|')
        $lines.Add(('| `wt-headless` head | `{0}`, {1} working-tree entries |' -f $head, $dirty))
        $lines.Add(('| renderer files changed | `{0}` |' -f ($changed -join '`, `')))
        $lines.Add(('| recycler code | **{0}**: `git diff 1a53ceb` over vk_image_recycler.cpp/.h, image.cpp/.h, texture_cache.cpp/.h, image_stats.h, image_coverage.h, vk_instance.cpp {1} |' -f $(if ($recyclerIdentical) { 'byte-identical' } else { 'DIFFERS' }), $(if ($recyclerIdentical) { 'is empty' } else { 'is NOT empty: ' + $recyclerDiff.Substring(0, [Math]::Min(300, $recyclerDiff.Length)) })))
        $lines.Add('| hooks are env-gated | `SHADPS4_DBG_DUMP_HMD_AT_SCENE` parses to `draws=0` when unset, so `NoteHmdDumpSceneWindow` returns immediately and `TakeHmdDumpRequest` returns false; the presenter dump call and the absolute-folder branch run only for an armed request; `SHADPS4_EXIT_AFTER`/`SHADPS4_CLEAN_EXIT` are `getenv` checks |')
        $lines.Add('| why A/B/A is valid | both arms run the same wt-headless binary, the same harness and the same env set; they differ **only** in `SHADPS4_DBG_IMAGE_RECYCLER` (`0` vs `1`) |')
    } else {
        $lines.Add('`wt-headless` not found; build-identity table unavailable.')
    }
    return ($lines -join "`r`n")
}

function Write-ResultsMarkdown($State) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# Cross-fork renderer validation — A–E results')
    $lines.Add('')
    $lines.Add(('Generated by `_build/headless/validation/run-validation.ps1` at {0}.' -f (Get-Date).ToString('o')))
    $lines.Add('')
    $lines.Add('Candidate: `integration/cross-fork-renderer` @ `1dd4eb4` (docs-only on top of `1a53ceb`), binary')
    $lines.Add('`wt-crossfork-int/build/shadPS4.exe` sha256 `7B658961…` (see [CANDIDATE-STATE.md](CANDIDATE-STATE.md)).')
    $lines.Add('Gameplay cells use the `wt-headless` build (same renderer, test-only input/exit/dump hooks).')
    $lines.Add('')
    $lines.Add('## Status')
    $lines.Add('')
    $lines.Add('| item | status | evidence |')
    $lines.Add('|---|---|---|')
    foreach ($row in @($State.status)) { $lines.Add(('| {0} | **{1}** | {2} |' -f $row.item, $row.status, $row.evidence)) }
    $lines.Add('')
    if ($null -ne $State.smoke) {
        $sm = $State.smoke
        $lines.Add('## Recycler pair — OFF must reach gameplay, ON checked for a regression')
        $lines.Add('')
        $lines.Add(('Status: **{0}**. {1}' -f $sm.status, $sm.finding))
        $lines.Add('')
        $lines.Add('| arm | label | phase | valid | draws | Mpx | passes | fps p50 | frames | hits/f | misses/f | menu stuck |')
        $lines.Add('|---|---|---|---|---|---|---|---|---|---|---|---|')
        foreach ($r in @($sm.off, $sm.on)) {
            $lines.Add(('| {0} | `{1}` | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} |' -f `
                        $(if ($r.label -like '*-on-*') { 'on' } else { 'off' }), $r.label, $r.phase, $r.valid, (Format-Num $r.draws '0'), (Format-Num $r.mpx '0'), (Format-Num $r.passes '0'), (Format-Num $r.fps_p50), $r.frames, (Format-Num $r.recycler_hits '0.000'), (Format-Num $r.recycler_misses '0.000'), $(if ($r.menu_stuck) { $r.menu_stuck.stayed_on_menu } else { $null })))
        }
        $lines.Add('')
        $lines.Add('A menu-stuck ON cell is reported only with verified input execution; its menu fps is never compared with gameplay. Full log/evidence paths are in `results/RECYCLER-PAIR.json` and each cell''s `cell.json`.')
        $lines.Add('')
    }
    if ($null -ne $State.save) {
        $sv = $State.save
        $lines.Add('## Save provenance (the classroom only loads from slot 1)')
        $lines.Add('')
        $lines.Add(('Source: `{0}` — slot 1 `MGS_GameData_1`: **{1}**, system data: {2}, stage `{3}`.' -f $sv.source, $sv.slot1_present, $sv.system_present, $sv.stage))
        $lines.Add('')
        $lines.Add('| file | bytes | sha256 |')
        $lines.Add('|---|---|---|')
        foreach ($f in @($sv.files)) { $lines.Add(('| `{0}` | {1} | `{2}` |' -f $f.name, $f.bytes, $f.sha256)) }
        $lines.Add('')
        $lines.Add('Every cell records the lab copy it ran with (`meta.save_data`) and the runner compares it with these hashes; a cell whose save does not match is not a classroom measurement.')
        $lines.Add('')
    }
    $lines.Add('## Build identity and why the A/B/A comparison is valid')
    $lines.Add('')
    $lines.Add((Get-BuildIdentityTable))
    $lines.Add('')
    $lines.Add('## Exact commands')
    $lines.Add('')
    $lines.Add('```powershell')
    foreach ($cmd in @($State.commands)) { $lines.Add($cmd) }
    $lines.Add('```')
    $lines.Add('')
    if ($null -ne $State.plan) {
        $lines.Add('## Command plan')
        $lines.Add('')
        $lines.Add('| mode | label | arm | harness mode | why |')
        $lines.Add('|---|---|---|---|---|')
        foreach ($p in @($State.plan)) { $lines.Add(('| {0} | `{1}` | {2} | {3} | {4} |' -f $p.mode, $p.label, $p.arm, $p.cell_mode, $p.why)) }
        $lines.Add('')
    }
    if ($null -ne $State.cells -and @($State.cells).Count -gt 0) {
        $lines.Add('## A — depth/barrier configuration cells (recycler off, 300 %, ' + $Hz + ' Hz)')
        $lines.Add('')
        $lines.Add('| label | arm | valid | phase | fps p50 | p90 | p99 | frames | draws | Mpx | passes | GPU % | img/f | create ms/f | destroy ms/f | hits/f | misses/f | pool MB | VRAM MB |')
        $lines.Add('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
        foreach ($c in @($State.cells)) {
            if ($null -eq $c -or $null -eq $c.measured) { $lines.Add(('| `{0}` | {1} | missing | - | - | - | - | - | - | - | - | - | - | - | - | - | - | - | - |' -f $c.label, $c.arm)); continue }
            $m = $c.measured
            $lines.Add(('| `{0}` | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16} | {17} | {18} |' -f `
                        $c.label, $m.arm, $m.valid, $m.phase, (Format-Num $m.fps_p50), (Format-Num $m.fps_p90), (Format-Num $m.fps_p99), $m.frames,
                    (Format-Num $m.draws_mean), (Format-Num $m.mpx_mean), (Format-Num $m.passes_mean), (Format-Num $m.gpu_busy_mean),
                    (Format-Num $m.images_created_per_frame '0.000'), (Format-Num $m.create_ms_per_frame '0.000'), (Format-Num $m.destroy_ms_per_frame '0.000'),
                    (Format-Num $m.recycler_hits_per_frame '0.000'), (Format-Num $m.recycler_misses_per_frame '0.000'),
                    (Format-Num $m.pool_mb_mean), (Format-Num $m.vram_mb_mean)))
        }
        $lines.Add('')
        $lines.Add('Actual gameplay signature read back from the log (the M2-confirmed scene is ~1866-1900 draws / 620-720 Mpx / 42-46 passes; the menu is ~485/480/65 and the loading dip 42-55):')
        $lines.Add('')
        $lines.Add('| label | window s | draws mean (min–max) | Mpx mean (min–max) | passes | fps p50 | draws spread % | gameplay gate ok |')
        $lines.Add('|---|---|---|---|---|---|---|---|')
        foreach ($c in @($State.cells)) {
            if ($null -eq $c) { continue }
            $rw = if ($null -ne $c.result_window) { $c.result_window } else { $c.scene }
            if ($null -eq $rw -or -not $rw.ok -and $null -eq $c.result_window) { $lines.Add(('| `{0}` | - | - | - | - | - | - | no |' -f $c.label)); continue }
            $g = $c.gameplay_gate
            $lines.Add(('| `{0}` | {1}–{2} | {3} ({4}–{5}) | {6} ({7}–{8}) | {9} | {10} | {11} | {12} |' -f `
                        $c.label, (Format-Num $rw.from_s), (Format-Num $rw.to_s), (Format-Num $rw.draws_mean), (Format-Num $rw.draws_min '0'), (Format-Num $rw.draws_max '0'),
                    (Format-Num $rw.mpx_mean), (Format-Num $rw.mpx_min '0'), (Format-Num $rw.mpx_max '0'), (Format-Num $rw.passes_mean),
                    (Format-Num $(if ($rw.fps) { $rw.fps.p50 } else { $null })), (Format-Num $rw.draws_spread_pct), $(if ($g.ok) { 'yes' } else { 'NO' })))
        }
        $lines.Add('')
        $lines.Add('Independent re-reading of the same logs (window read back from `frame stats at …`, not from result.json):')
        $lines.Add('')
        $lines.Add('| label | log scene samples | anchor s | window s | draws mean (min–max) | Mpx mean (min–max) | passes | GPU busy | draws spread % | result band agrees |')
        $lines.Add('|---|---|---|---|---|---|---|---|---|---|')
        foreach ($c in @($State.cells)) {
            if ($null -eq $c) { continue }
            $s = if ($null -ne $c.result_window) { $c.result_window } else { $c.scene }
            if ($null -eq $s) { continue }
            if ($null -eq $c.result_window -and -not $s.ok) { $lines.Add(('| `{0}` | - | - | - | - | - | - | - | - | no ({1}) |' -f $c.label, $s.reason)); continue }
            $lines.Add(('| `{0}` | {1} | {2} | {3}–{4} | {5} ({6}–{7}) | {8} ({9}–{10}) | {11} | {12} | {13} | {14} |' -f `
                        $c.label, $s.samples, (Format-Num $(if ($s.anchor_s) { $s.anchor_s } else { $s.from_s })), (Format-Num $s.from_s), (Format-Num $s.to_s),
                    (Format-Num $s.draws_mean), (Format-Num $s.draws_min '0'), (Format-Num $s.draws_max '0'),
                    (Format-Num $s.mpx_mean), (Format-Num $s.mpx_min '0'), (Format-Num $s.mpx_max '0'),
                    (Format-Num $s.passes_mean), (Format-Num $s.gpu_busy_mean), (Format-Num $s.draws_spread_pct), $(if ($c.independent_band_ok) { 'yes' } else { 'no' })))
        }
        $lines.Add('')
        $lines.Add('Arm-effect check (did the env switch take effect?):')
        $lines.Add('')
        $lines.Add('| label | arm | hits/f | misses/f | off-arm all-zero | on-arm activity | ok |')
        $lines.Add('|---|---|---|---|---|---|---|')
        foreach ($c in @($State.cells)) {
            if ($null -eq $c -or $null -eq $c.arm_evidence) { continue }
            $a = $c.arm_evidence
            $lines.Add(('| `{0}` | {1} | {2} | {3} | {4} | {5} | {6} |' -f $c.label, $a.arm, (Format-Num $a.recycler_hits_per_frame '0.000'), (Format-Num $a.recycler_misses_per_frame '0.000'), $a.off_arm_zero_hits, $a.on_arm_activity, $a.ok))
        }
        $lines.Add('')
    }
    if ($null -ne $State.acceptance) {
        $a = $State.acceptance
        $lines.Add('### A acceptance (candidate rules)')
        $lines.Add('')
        $lines.Add('| rule | result |')
        $lines.Add('|---|---|')
        $lines.Add(('| arms match draws/Mpx within 5 % | {0} |' -f $(if ($null -ne $a.arm_agreement) { if ($a.arm_agreement.ok) { 'yes' } else { 'NO: ' + $a.arm_agreement.reason } } else { 'not evaluated' })))
        if ($null -ne $a.floor) {
            $lines.Add(('| A/A floor: fps p50 spread | {0} fps ({1} %); p99 spread {2} fps |' -f $a.floor.fps_p50_delta, $a.floor.fps_p50_spread_pct, $a.floor.worst_fps_p99_delta))
        }
        if ($null -ne $a.fps) {
            $lines.Add(('| legacy vs base fps p50 | {0} fps ({1} %), base/base spread {2} fps, beyond floor: {3}{4} |' -f $a.fps.delta_p50, $a.fps.delta_pct, $a.fps.floor_spread_p50, $a.fps.beyond_floor, $(if ($a.fps.note) { ' — ' + $a.fps.note } else { '' })))
        }
        if ($null -ne $a.alloc) {
            $lines.Add(('| depth/barrier effect | images/frame {0} -> {1}; create ms/frame {2} -> {3}; destroy ms/frame {4} -> {5}; passes {6} -> {7}; Mpx {8} -> {9} |' -f $a.alloc.base_images_created_per_frame, $a.alloc.legacy_images_created_per_frame, $a.alloc.base_create_ms_per_frame, $a.alloc.legacy_create_ms_per_frame, $a.alloc.base_destroy_ms_per_frame, $a.alloc.legacy_destroy_ms_per_frame, $a.alloc.base_passes_mean, $a.alloc.legacy_passes_mean, $a.alloc.base_mpx_mean, $a.alloc.legacy_mpx_mean))
            
            $lines.Add(('| device memory | VRAM {0} -> {1} MB; refresh uploads/frame {2} -> {3} |' -f $a.alloc.base_vram_mb, $a.alloc.legacy_vram_mb, $a.alloc.base_refresh_uploads, $a.alloc.legacy_refresh_uploads))
        }
        $lines.Add(('| cells discarded | {0} |' -f $(if ($a.discarded.Count) { ($a.discarded -join ', ') } else { 'none' })))
        $lines.Add(('| verdict | **{0}** {1} |' -f $a.status, $(if ($a.reasons.Count) { '— ' + ($a.reasons -join '; ') } else { '' })))
        $lines.Add('')
    }
    if ($null -ne $State.b) {
        $b = $State.b
        $lines.Add('## B — pixel content between the A arms (base/base floor, then base vs legacy)')
        $lines.Add('')
        $lines.Add(('Dump mode: **{0}**{1}.' -f $b.dump_mode, $(if ($b.dump_frames) { ', frame ids ' + $b.dump_frames } else { '' })))
        $lines.Add('')
        if ($null -ne $b.images -and @($b.images).Count -gt 0) {
            $lines.Add('| dump | image | floor SSIM (off/off) | on SSIM | floor mean abs | on mean abs | masked equal (on) | verdict |')
            $lines.Add('|---|---|---|---|---|---|---|---|')
            foreach ($i in @($b.images)) {
                $lines.Add(('| {0} | `{1}` | {2} | {3} | {4} | {5} | {6} | {7} |' -f $i.dump, $i.image, (Format-Num $i.floor_ssim '0.0000'), (Format-Num $i.on1_ssim '0.0000'), (Format-Num $i.floor_mean_abs '0.00000'), (Format-Num $i.on1_mean_abs '0.00000'), $i.on1_masked_equal, $i.verdict))
            }
            $lines.Add('')
        }
        $lines.Add(('Verdict: **{0}** {1}' -f $b.status, $(if ($b.reasons.Count) { '— ' + ($b.reasons -join '; ') } else { '' })))
        $lines.Add('')
        $lines.Add('Region SSIM (3x3, from `capture-diff.py`) is in `results/cells/*/capture-diff-*.json`; left/right eye splits are under `evidence\eye-splits\`.')
        $lines.Add('')
    }
    if ($null -ne $State.c) {
        $c = $State.c
        $lines.Add('## C — R6 clean shutdown with the recycler on')
        $lines.Add('')
        $lines.Add(('Verdict: **{0}** {1}' -f $c.status, $(if ($c.reasons.Count) { '— ' + ($c.reasons -join '; ') } else { '' })))
        $lines.Add('')
        if ($null -ne $c.checks) {
            $lines.Add('| check | value |')
            $lines.Add('|---|---|')
            foreach ($k in $c.checks.Keys) { $lines.Add(('| {0} | `{1}` |' -f $k, $c.checks[$k])) }
            $lines.Add('')
        }
    }
    if ($null -ne $State.d) {
        $d = $State.d
        $lines.Add('## D — RenderDoc scene-gated capture (recycler off)')
        $lines.Add('')
        $lines.Add(('Verdict: **{0}** {1}' -f $d.status, $(if ($d.reasons.Count) { '— ' + ($d.reasons -join '; ') } else { '' })))
        $lines.Add('')
        if ($null -ne $d.checks) {
            $lines.Add('| check | value |')
            $lines.Add('|---|---|')
            foreach ($k in $d.checks.Keys) {
                $v = $d.checks[$k]
                if ($v -is [System.Collections.IDictionary]) { $v = ($v | ConvertTo-Json -Compress) }
                $lines.Add(('| {0} | `{1}` |' -f $k, $v))
            }
            $lines.Add('')
        }
    }
    if ($null -ne $State.e) {
        $e = $State.e
        $lines.Add('## E — 100 % render / 120 Hz configuration cell (separate from the 300 % table)')
        $lines.Add('')
        if ($null -ne $e.measured) {
            $lines.Add('| field | value |')
            $lines.Add('|---|---|')
            $lines.Add(('| render percent / Hz | {0} / {1} |' -f $e.measured.config.render_percent, $e.measured.config.hz))
            $lines.Add(('| phase / valid | {0} / {1} |' -f $e.measured.phase, $e.measured.valid))
            $lines.Add(('| fps p50 / p90 / p99 | {0} / {1} / {2} |' -f (Format-Num $e.measured.fps_p50), (Format-Num $e.measured.fps_p90), (Format-Num $e.measured.fps_p99)))
            $lines.Add(('| draws / Mpx / passes | {0} / {1} / {2} |' -f (Format-Num $e.measured.draws_mean), (Format-Num $e.measured.mpx_mean), (Format-Num $e.measured.passes_mean)))
            $lines.Add(('| GPU busy | {0} % |' -f (Format-Num $e.measured.gpu_busy_mean)))
            $lines.Add(('| recycler | {0} |' -f $e.measured.arm))
            $lines.Add('')
        } else { $lines.Add('not run'); $lines.Add('') }
    }
    $lines.Add('## Limitations')
    $lines.Add('')
    $lines.Add('* No physical headset: no latency, comfort, tracking-quality or display-power claim can be made from these cells.')
    $lines.Add('* `depth_copies` needs `SHADPS4_TRACE_ADDR` (the ALIASSTAT line); otherwise it is null in `result.json`.')
    $lines.Add('* The candidate (1a53ceb/1dd4eb4) has no clean-exit path; R6 needs the wt-headless build with `SHADPS4_EXIT_AFTER` + `SHADPS4_CLEAN_EXIT`.')
    $lines.Add('* Frame identity across arms is not proven: the base/base floor bounds animation/pose noise, so a base-vs-legacy difference is only read against that floor, never against zero.')
    $lines.Add('* RenderDoc extraction of individual HMD textures would need the GUI; `renderdoccmd` validates the capture and its thumbnail only.')
    $lines.Add('')
    Write-TextFile (Join-Path $script:Out 'RESULTS.md') (($lines -join "`r`n") + "`r`n")
}

# --- dry run ----------------------------------------------------------------------------------
function Invoke-DryRun {
    New-Item -ItemType Directory -Force -Path $script:DryDir | Out-Null
    if ($DryRunPaths.Count -eq 0) {
        $DryRunPaths = @(
            (Join-Path $Root '_build\bench\runs\xf-a2-1'),
            (Join-Path $Root '_build\bench\runs\xf-a2-0-r2'),
            (Join-Path $Root '_build\bench\runs\xf-a2-1-r3'),
            (Join-Path $Root '_build\bench\runs\xf-rec-on'),
            (Join-Path $Root '_build\bench\runs\xf-rec2-on'),
            (Join-Path $Root '_build\bench\runs\b-on1'),
            (Join-Path $Root '_build\bench\runs\ab-integ-A-r1'),
            (Join-Path $Root '_build\bench\runs\xf-a2-1\shad_log.txt'),
            (Join-Path $Root '_build\bench\runs\cap-A'),
            (Join-Path $Root '_build\bench\runs\cap-B'),
            (Join-Path $Root '_build\bench\runs\capA2'),
            (Join-Path $Root '_build\bench\runs\capB2'),
            (Join-Path $Root '_build\crossfork\gameplay\gp-dryrun'),
            (Join-Path $Root '_build\crossfork\runs'),
            (Join-Path $Root '_run\pc-vr-recycler-try\user\log\shad_log.txt')
        )
    }
    $entries = New-Object System.Collections.Generic.List[object]
    $selfChecks = [ordered]@{ frame_stats_parser = 0; renderstat_parser = 0; hmd_parser = 0; scene_gate_parser = 0; capture_compare = $false; eye_stats = $false }
    foreach ($path in $DryRunPaths) {
        if (-not (Test-Path -LiteralPath $path)) {
            $entries.Add([ordered]@{ path = $path; kind = 'missing'; ok = $false; note = 'path does not exist' })
            continue
        }
        $isContainer = [System.IO.Directory]::Exists($path)
        if ($isContainer) {
            $resultPath = Join-Path $path 'result.json'
            $metaPath = Join-Path $path 'meta.json'
            $capturesPath = Join-Path $path 'captures.json'
            if (Test-Path $resultPath) {
                # A run dir in the harness / gameplay-cell shape.
                $label = (Read-JsonFile $resultPath).label
                if (-not $label) { $label = Split-Path -Leaf $path }
                $cell = [ordered]@{ label = $label; arm = (Read-JsonFile $resultPath).arm }
                $tmpRun = Join-Path $script:RunsRoot $label
                # Parse this directory directly, not the harness runs root.
                $result = Read-JsonFile $resultPath
                $meta = Read-JsonFile $metaPath
                $logPath = Join-Path $path 'shad_log.txt'
                if (-not (Test-Path $logPath) -and $result -and $result.log) { $logPath = [string]$result.log }
                $parse = Get-LogParse $logPath
                $scene = Get-SceneWindow $parse $SceneMinDraws $SceneMinMpx $SceneSamples $Warmup $Seconds
                $counters = Get-CounterMeans $parse $scene
                $entries.Add([ordered]@{
                    path = $path; kind = 'run-dir'; ok = $true; label = $label
                    result_valid = $(if ($result) { $result.valid } else { $null })
                    log = [ordered]@{ exists = $parse.exists; bytes = $parse.bytes; samples = $parse.samples.Count; renderstat = $parse.renderstat.Count; aliasstat = $parse.aliasstat.Count }
                    scene = $scene
                    counters = $counters
                    hmd = $parse.hmd
                    result_counters = $(if ($result) { [ordered]@{ images_created_per_frame = $result.images_created_per_frame; create_ms_per_frame = $result.create_ms_per_frame; destroy_ms_per_frame = $result.destroy_ms_per_frame; hits = $result.recycler_hits_per_frame; misses = $result.recycler_misses_per_frame } } else { $null })
                })
                if ($parse.samples.Count -gt 0) { $selfChecks.frame_stats_parser++ }
                if ($parse.renderstat.Count -gt 0) { $selfChecks.renderstat_parser++ }
                if ($null -ne $parse.hmd) { $selfChecks.hmd_parser++ }
                if ($scene.ok) { $selfChecks.scene_gate_parser++ }
            } elseif (Test-Path $capturesPath) {
                $cap = Read-JsonFile $capturesPath
                $entries.Add([ordered]@{ path = $path; kind = 'capture-run'; ok = $true; captures = @($cap.captures).Count; images = @(@($cap.captures) | ForEach-Object { @($_.images) }).Count })
            } elseif (Test-Path $metaPath) {
                $meta = Read-JsonFile $metaPath
                $logPath = Join-Path $path 'shad_log.txt'
                $parse = Get-LogParse $logPath
                $scene = Get-SceneWindow $parse $SceneMinDraws $SceneMinMpx $SceneSamples $Warmup $Seconds
                $entries.Add([ordered]@{
                    path = $path; kind = 'bench-run'; ok = $true; label = $meta.label
                    exe_sha256 = $meta.exe.sha256
                    scene_declared = $meta.timing.scene
                    log = [ordered]@{ exists = $parse.exists; bytes = $parse.bytes; samples = $parse.samples.Count; renderstat = $parse.renderstat.Count }
                    scene = $scene
                })
                if ($parse.samples.Count -gt 0) { $selfChecks.frame_stats_parser++ }
                if ($parse.renderstat.Count -gt 0) { $selfChecks.renderstat_parser++ }
                if ($scene.ok) { $selfChecks.scene_gate_parser++ }
            } else {
                $entries.Add([ordered]@{ path = $path; kind = 'unknown-dir'; ok = $false; note = 'no result.json, meta.json or captures.json' })
            }
            continue
        }
        # A raw log file.
        $parse = Get-LogParse $path
        $scene = Get-SceneWindow $parse $SceneMinDraws $SceneMinMpx $SceneSamples $Warmup $Seconds
        $counters = Get-CounterMeans $parse $scene
        $entries.Add([ordered]@{
            path = $path; kind = 'raw-log'; ok = $true
            log = [ordered]@{ bytes = $parse.bytes; lines = $parse.log_lines; samples = $parse.samples.Count; renderstat = $parse.renderstat.Count; aliasstat = $parse.aliasstat.Count }
            scene = $scene; counters = $counters; hmd = $parse.hmd
            validation_errors = @($parse.validation).Count
            rdoc_gate = $parse.rdoc_gate; rdoc_trigger_s = $parse.rdoc_trigger
        })
        if ($parse.samples.Count -gt 0) { $selfChecks.frame_stats_parser++ }
        if ($parse.renderstat.Count -gt 0) { $selfChecks.renderstat_parser++ }
        if ($null -ne $parse.hmd) { $selfChecks.hmd_parser++ }
        if ($scene.ok) { $selfChecks.scene_gate_parser++ }
    }

    # Pair-compare the existing captures: proves capture-compare + the HMD-image filter end to end.
    $pairResult = $null
    $capA = Join-Path $Root '_build\bench\runs\cap-A'
    $capB = Join-Path $Root '_build\bench\runs\cap-B'
    if ((Test-Path $capA) -and (Test-Path $capB)) {
        $out = Join-Path $script:DryDir 'compare-cap-A-cap-B.json'
        $cmp = Compare-Bundles $capA $capB $out
        if ($cmp.ok) {
            $hmdCommon = New-Object System.Collections.Generic.List[object]
            foreach ($dump in @($cmp.data.compare)) {
                foreach ($d in @($dump.differing)) {
                    $e = Get-HmdImageEntry ([string]$d.image)
                    if ($null -ne $e -and $e.hmd) { $hmdCommon.Add([ordered]@{ dump = $dump.dump; image = $d.image; ssim = $d.ssim; mean_abs = $d.mean_abs; masked_equal = $d.masked_equal; identical = $false }) }
                }
                $identicalCount = [int]$dump.identical
                if ($identicalCount -gt 0) {
                    $hmdCommon.Add([ordered]@{ dump = $dump.dump; image = "(and $identicalCount bit-identical image(s))"; ssim = 1.0; mean_abs = 0.0; masked_equal = $true; identical = $true })
                }
            }
            $pairResult = [ordered]@{ ok = $true; out = $out; dumps_a = @($cmp.data.dumps_a).Count; dumps_b = @($cmp.data.dumps_b).Count; common_dumps = @($cmp.data.compare).Count; hmd_images = [object[]]$hmdCommon }
            $selfChecks.capture_compare = $true
            # Eye-image stats + split on the HMD side-by-side image of cap-A, addressed by the
            # guest address the HMD line names (0x2018000000), not by size alone.
            $capManifest = Read-JsonFile (Join-Path $capA 'captures.json')
            $eyePng = $null
            if ($null -ne $capManifest) {
                $items = New-Object System.Collections.Generic.List[object]
                foreach ($cap in @($capManifest.captures)) {
                    foreach ($img in @($cap.images)) { $items.Add([pscustomobject]@{ folder = [string]$cap.folder; name = [string]$img.name }) }
                }
                $address = ''
                $capLog = Get-LogParse (Join-Path $capA 'shad_log.txt')
                if ($null -ne $capLog.hmd -and $capLog.hmd.addr) { $address = [string]$capLog.hmd.addr }
                $eyeName = Select-HmdEyeImage @($items | ForEach-Object { $_.name }) $address
                if ($null -ne $eyeName) {
                    $match = $items | Where-Object { $_.name -eq $eyeName } | Select-Object -First 1
                    $eyePath = Join-Path $capA (($match.folder -replace '/', '\') + '\' + $match.name)
                    if (Test-Path -LiteralPath $eyePath) { $eyePng = Get-Item -LiteralPath $eyePath }
                    $pairResult['eye_address'] = $address
                }
            }
            if ($null -ne $eyePng) {
                $stats = Get-EyeStats @($eyePng.FullName)
                $split = Split-EyeImage $eyePng.FullName (Join-Path $script:EvidenceDir 'eye-splits') ('dryrun-' + $eyePng.BaseName)
                $pairResult['eye_stats'] = if ($stats.Count) { $stats[0] } else { $null }
                $pairResult['eye_split'] = $split
                $selfChecks.eye_stats = ($stats.Count -gt 0)
            }
        } else { $pairResult = [ordered]@{ ok = $false; error = $cmp.error } }
    }

    # A second, cross-run pair (cap-A vs capA2): the differing-image metrics path, not just the
    # bit-identical one. Both are the same build's capture cells.
    $pairCross = $null
    $capA2 = Join-Path $Root '_build\bench\runs\capA2'
    if ((Test-Path $capA) -and (Test-Path $capA2)) {
        $out2 = Join-Path $script:DryDir 'compare-cap-A-capA2.json'
        $cmp2 = Compare-Bundles $capA $capA2 $out2
        if ($cmp2.ok) {
            $rows = New-Object System.Collections.Generic.List[object]
            foreach ($dump in @($cmp2.data.compare)) {
                foreach ($d in @($dump.differing)) {
                    $e = Get-HmdImageEntry ([string]$d.image)
                    if ($null -ne $e -and $e.hmd) { $rows.Add([ordered]@{ dump = $dump.dump; image = $d.image; ssim = $d.ssim; mean_abs = $d.mean_abs; masked_equal = $d.masked_equal }) }
                }
            }
            $pairCross = [ordered]@{ ok = $true; out = $out2; differing = $rows.Count; rows = [object[]]$rows }
        } else { $pairCross = [ordered]@{ ok = $false; error = $cmp2.error } }
    }

    $report = [ordered]@{
        generated = (Get-Date).ToString('o'); gate = $Gate; scene_min_draws = $SceneMinDraws; scene_min_mpx = $SceneMinMpx
        warmup_s = $Warmup; seconds_s = $Seconds
        self_checks = $selfChecks
        entries = [object[]]$entries
        capture_pair = $pairResult
        capture_pair_cross = $pairCross
    }
    Write-JsonFile (Join-Path $script:DryDir 'DYRUN.json') $report

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# Dry-run parser report (no GPU)')
    $lines.Add('')
    $lines.Add(('Generated {0}. Parsed {1} existing evidence item(s) with the same parsers the A–E runner uses.' -f (Get-Date).ToString('o'), $entries.Count))
    $lines.Add('')
    $lines.Add('## Parser self-checks')
    $lines.Add('')
    $lines.Add('| parser | items parsed |')
    $lines.Add('|---|---|')
    foreach ($k in $selfChecks.Keys) { $lines.Add(('| {0} | `{1}` |' -f $k, $selfChecks[$k])) }
    $lines.Add('')
    $lines.Add('## Evidence items')
    $lines.Add('')
    $lines.Add('| path | kind | samples | RENDERSTAT | anchor s | draws | Mpx | passes | GPU % | HMD line | notes |')
    $lines.Add('|---|---|---|---|---|---|---|---|---|---|---|')
    foreach ($e in ([object[]]$entries)) {
        if ($e.kind -eq 'capture-run') { $lines.Add(('| `{0}` | capture-run | - | - | - | - | - | - | - | - | {1} captures, {2} images |' -f $e.path, $e.captures, $e.images)); continue }
        if ($e.kind -eq 'missing' -or $e.kind -eq 'unknown-dir') { $lines.Add(('| `{0}` | {1} | - | - | - | - | - | - | - | - | {2} |' -f $e.path, $e.kind, $e.note)); continue }
        $log = $e.log
        $scene = $e.scene
        $hmd = if ($null -ne $e.hmd) { ('{0}x{1} {2} s{3}' -f $e.hmd.w, $e.hmd.h, $e.hmd.fmt, $e.hmd.samples) } else { '-' }
        $note = if ($e.kind -eq 'bench-run') { 'bench meta; declared scene ' + ($e.scene_declared.source) } elseif ($e.kind -eq 'raw-log') { 'raw log; validation errors ' + $e.validation_errors + $(if ($log.samples -eq 0) { '; no frame-stats lines (rotated/truncated or SHADPS4_FRAME_STATS off)' } else { '' }) } else { '' }
        $lines.Add(('| `{0}` | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |' -f `
                    $e.path, $e.kind, $log.samples, $log.renderstat, $(if ($scene.ok) { Format-Num $scene.anchor_s } else { '-' }),
                $(if ($scene.ok) { Format-Num $scene.draws_mean } else { '-' }), $(if ($scene.ok) { Format-Num $scene.mpx_mean } else { '-' }),
                $(if ($scene.ok) { Format-Num $scene.passes_mean } else { '-' }), $(if ($scene.ok) { Format-Num $scene.gpu_busy_mean } else { '-' }), $hmd, $note))
    }
    $lines.Add('')
    if ($null -ne $pairResult) {
        $lines.Add('## capture-compare proof (existing cap-A vs cap-B)')
        $lines.Add('')
        if ($pairResult.ok) {
            $lines.Add(('capture-compare ran: {0} dump(s) each side, {1} common; JSON `{2}`.' -f $pairResult.dumps_a, $pairResult.common_dumps, $pairResult.out))
            $lines.Add('')
            $lines.Add('| dump | HMD image | SSIM | mean abs | masked equal | identical |')
            $lines.Add('|---|---|---|---|---|---|')
            foreach ($h in @($pairResult.hmd_images)) { $lines.Add(('| {0} | `{1}` | {2} | {3} | {4} | {5} |' -f $h.dump, $h.image, (Format-Num $h.ssim '0.0000'), (Format-Num $h.mean_abs '0.00000'), $h.masked_equal, $h.identical)) }
            if ($null -ne $pairResult.eye_stats) {
                $lines.Add('')
                $lines.Add(('Eye stats on `{0}`: {1}x{2}, luma mean {3}, black fraction {4}.' -f $pairResult.eye_stats.path, $pairResult.eye_stats.width, $pairResult.eye_stats.height, $pairResult.eye_stats.mean, $pairResult.eye_stats.black_frac))
                if ([double]$pairResult.eye_stats.black_frac -ge 0.99) {
                    $lines.Add('')
                    $lines.Add('Note: this cap-A dump is the HMD target 3 s into the **light** phase and is entirely black. That is real (the dump landed before the guest wrote the eye image), and it is exactly why B needs a **scene-gated** gameplay dump instead of a fixed light-phase offset; the split path is still exercised end to end.')
                }
            }
            if ($null -ne $pairResult.eye_split -and $pairResult.eye_split.ok) {
                $lines.Add(('Eye split written: `{0}` + `{1}`.' -f $pairResult.eye_split.left, $pairResult.eye_split.right))
            }
        } else {
            $lines.Add('capture-compare failed: ' + $pairResult.error)
        }
        $lines.Add('')
    }
    if ($null -ne $pairCross) {
        $lines.Add('## capture-compare proof, cross-run pair (cap-A vs capA2)')
        $lines.Add('')
        if ($pairCross.ok) {
            $lines.Add(('Ran on two different capture cells; {0} HMD image(s) differed enough to be measured (the identical-image shortcut does not hide them). JSON `{1}`.' -f $pairCross.differing, $pairCross.out))
            $lines.Add('')
            if ($pairCross.differing -gt 0) {
                $lines.Add('| dump | HMD image | SSIM | mean abs | masked equal |')
                $lines.Add('|---|---|---|---|---|')
                foreach ($h in @($pairCross.rows)) { $lines.Add(('| {0} | `{1}` | {2} | {3} | {4} |' -f $h.dump, $h.image, (Format-Num $h.ssim '0.0000'), (Format-Num $h.mean_abs '0.00000'), $h.masked_equal)) }
            } else {
                $lines.Add('Both runs'' `light+3` eye targets are black, so every image is trivially bit-identical. This is **not** evidence of anything about the renderer; it is the concrete reason a fixed light-phase offset cannot serve R3 and B must dump at the gameplay scene gate.')
            }
        } else { $lines.Add('cross-run capture-compare failed: ' + $pairCross.error) }
        $lines.Add('')
    }
    $lines.Add('## What this proves and what it does not')
    $lines.Add('')
    $lines.Add('* Proves: the frame-stats, RENDERSTAT/ALIASSTAT, HMD-line, scene-gate and capture-comparison parsers work on real evidence, including the front-end-vs-gameplay ambiguity (a window with 535 draws / ~460 Mpx is accepted by a draws>=400/Mpx>=450 gate and is still the front end).')
    $lines.Add('* Does not prove: any A–E result. Those need the gameplay scene and the GPU lock, and are not claimed until the cells run.')
    $lines.Add('* `depth_copies` stays null in these logs because `SHADPS4_TRACE_ADDR` was not set; `RENDERSTAT` carries no depth-copy field.')
    Write-TextFile (Join-Path $script:DryDir 'DYRUN.md') (($lines -join "`r`n") + "`r`n")
    Write-Host ("dry run written: {0}" -f (Join-Path $script:DryDir 'DYRUN.md'))
    return 0
}

# --- main -------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $script:Out | Out-Null
New-Item -ItemType Directory -Force -Path $script:CellsDir | Out-Null

$script:SaveSourceInfo = Get-SaveProvenance $SaveDataSource
Write-JsonFile (Join-Path $script:Out 'SAVE.json') $script:SaveSourceInfo

$plan = Get-CellPlan
if ($OnlyCells.Count -gt 0) {
    $plan = @($plan | Where-Object { $hit = $false; foreach ($needle in $OnlyCells) { if ($_.label -like ('*' + $needle + '*')) { $hit = $true } }; $hit })
}

function Get-Commands {
    $cmds = New-Object System.Collections.Generic.List[string]
    $cmds.Add('# one lock hold per cell; the harness is called with -Exe <build> and never launches outside the lock')
    function Add-CellCommand($item) {
        $hargs = Get-HarnessArgs $item
        $envPairs = New-Object System.Collections.Generic.List[string]
        if ($null -ne $item.env) { foreach ($k in $item.env.Keys) { $envPairs.Add(("`$env:{0}='{1}'" -f $k, $item.env[$k])) } }
        if ($envPairs.Count -gt 0) {
            $cmds.Add(('# env must be set inside the harness process (a var set before wsl.exe does not survive to the emulator); the runner writes results\cells\<label>\invoke.ps1 with: ' + ($envPairs -join '; ')))
        }
        $inner = (($hargs | ForEach-Object { if ($_ -match '[\s;]') { "'$_'" } else { $_ } }) -join ' ')
        $cmds.Add(('bash /mnt/m/Development/Deracine-work/_build/team/gpu-lock.sh run --owner validate-eng --kind headless --reason "validation ' + $item.mode + '" --ttl 1800 --require-quiet -- pwsh.exe ' + $inner))
    }
    if (-not $script:UsePair) {
        $cmds.Add('# SMOKE PAIR FIRST (Lead gate): OFF must reach gameplay, then the ON arm is checked for the')
        $cmds.Add('# suspected menu stall. Do not start the matrix until the OFF cell is confirmed.')
        Add-CellCommand ([ordered]@{ mode = 'P'; label = ($script:Prefix + '-P-off-1'); arm = 'off'; cell_mode = 'cell'; gate = 'gameplay'; extra = @(); attempts = $Attempts })
        Add-CellCommand ([ordered]@{ mode = 'P'; label = ($script:Prefix + '-P-on-1'); arm = 'on'; cell_mode = 'cell'; gate = 'gameplay'; extra = @(); attempts = [math]::Min($Attempts, 2) })
        $cmds.Add('# Then the A/E matrix below.')
    }
    foreach ($item in ([object[]]$plan)) {
        $hargs = Get-HarnessArgs $item
        $envPairs = New-Object System.Collections.Generic.List[string]
        if ($null -ne $item.env) { foreach ($k in $item.env.Keys) { $envPairs.Add(("`$env:{0}='{1}'" -f $k, $item.env[$k])) } }
        if ($envPairs.Count -gt 0) {
            $cmds.Add(('# env must be set inside the harness process (a var set before wsl.exe does not survive to the emulator); the runner writes results\cells\<label>\invoke.ps1 with: ' + ($envPairs -join '; ')))
        }
        $inner = (($hargs | ForEach-Object { if ($_ -match '[\s;]') { "'$_'" } else { $_ } }) -join ' ')
        $cmds.Add(('bash /mnt/m/Development/Deracine-work/_build/team/gpu-lock.sh run --owner validate-eng --kind headless --reason "validation ' + $item.mode + '" --ttl 1800 --require-quiet -- pwsh.exe ' + $inner))
    }
    $cmds.Add('# B comparisons (per pair, after the capture cells)')
    $cmds.Add('python _build\bench\capture-compare.py _build\headless\validation\results\cells\<B-off-1>\capture-bundle _build\headless\validation\results\cells\<B-off-2>\capture-bundle --out compare-off1-off2.json')
    $cmds.Add('python _build\bench\capture-diff.py <hmd-off.png> <hmd-on.png> --grid 3 --out capture-diff.json')
    $cmds.Add('# D capture validation')
    $cmds.Add('"C:\Program Files\RenderDoc\renderdoccmd.exe" thumb --out=rdoc-thumbnail.png <capture>.rdc')
    $cmds.Add('# build-identity proof (WSL git; the worktree .git uses a POSIX path, Windows git cannot open it)')
    $cmds.Add('wsl.exe -e git -C /mnt/m/Development/Deracine-work/wt-headless diff 1a53ceb --name-only -- src/src/video_core')
    $cmds.Add('wsl.exe -e git -C /mnt/m/Development/Deracine-work/wt-headless diff 1a53ceb -- src/src/video_core/renderer_vulkan/vk_image_recycler.cpp src/src/video_core/renderer_vulkan/vk_image_recycler.h')
    return $cmds
}

if ($SelfTestB) {
    $dir = Join-Path $script:Out 'selftest'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $defs = @(
        @{ path = (Join-Path $Root '_build\bench\runs\cap-A'); label = 'selftest-off-1' },
        @{ path = (Join-Path $Root '_build\bench\runs\cap-B'); label = 'selftest-off-2' },
        @{ path = (Join-Path $Root '_build\bench\runs\capA2'); label = 'selftest-on' }
    )
    $fake = New-Object System.Collections.Generic.List[object]
    foreach ($d in $defs) {
        $fake.Add([ordered]@{ label = $d.label; run_dir = $d.path; meta = $null; result = $null; hmd = $null })
    }
    $r = Test-BContent $fake[0] $fake[1] $fake[2]
    Write-JsonFile (Join-Path $dir 'B-selftest.json') $r
    Write-Host ('B self-test status: {0}' -f $r.status)
    foreach ($reason in @($r.reasons)) { Write-Host ('  reason: {0}' -f $reason) }
    Write-Host ('  images compared: {0}; region diffs: {1}' -f @($r.images | Where-Object { $_ }).Count, @($r.region_diffs | Where-Object { $_ }).Count)
    exit 0
}

if ($DryRun) {
    $code = Invoke-DryRun
    Write-Host 'dry run: no emulator, no GPU lock, no capture started.'
    exit $code
}

if ($PrintPlan) {
    # Only the command list is written: RESULTS.md is a result, not a plan, and must not be
    # replaced by a "PLANNED" placeholder each time the plan is printed.
    Write-TextFile (Join-Path $script:Out 'COMMANDS.md') ((@(Get-Commands) -join "`r`n") + "`r`n")
    Write-Host ('plan written: {0}' -f (Join-Path $script:Out 'COMMANDS.md'))
    exit 0
}

if ($PairFromRuns -ne '') {
    $labels = @($PairFromRuns -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($labels.Count -ne 2) { Write-Host '-PairFromRuns needs "<off-label>,<on-label>"'; exit 2 }
    $offCell = Measure-Cell $labels[0]
    $onCell = Measure-Cell $labels[1]
    $pair = Get-RecyclerPairVerdict $offCell $onCell
    $pair['generated'] = (Get-Date).ToString('o')
    $pair['labels'] = $labels
    Write-PairReport $pair
    $state = [ordered]@{
        status = @([ordered]@{ item = ('recycler pair from runs: ' + ($labels -join ' / ')); status = $pair.status; evidence = 'results\RECYCLER-PAIR.json / .md' })
        commands = @(); plan = @(); save = $script:SaveSourceInfo
        cells = @(); acceptance = $null; b = $null; c = $null; d = $null; e = $null; smoke = $pair
    }
    Write-ResultsMarkdown $state
    Write-Host ('recycler pair: {0} (off {1}: {2}, on {3}: {4})' -f $pair.status, $offCell.label, $offCell.measured.phase, $onCell.label, $onCell.measured.phase)
    $exit = if ($pair.status -eq 'RECYCLER-REGRESSION-SUSPECTED') { 1 } else { 0 }
    exit $exit
}

if ($CFromRun -ne '') {
    $cCell = Measure-Cell $CFromRun
    $cres = Test-CShutdown $cCell
    $cres['label'] = $CFromRun
    Write-JsonFile (Join-Path $script:Out 'C-shutdown.json') $cres
    $state = [ordered]@{
        status = @([ordered]@{ item = 'C — clean exit (recycler on)'; status = $cres.status; evidence = ('results\C-shutdown.json; runs\' + $CFromRun) })
        commands = @(); plan = @(); save = $script:SaveSourceInfo
        cells = @($cCell); acceptance = $null; b = $null; c = $cres; d = $null; e = $null; smoke = $null
    }
    Write-ResultsMarkdown $state
    Write-Host ('C: {0}' -f $cres.status)
    foreach ($reason in @($cres.reasons)) { Write-Host ('  reason: {0}' -f $reason) }
    exit $(if ($cres.status -eq 'PASS') { 0 } else { 1 })
}

if ($DFromRuns -ne '') {
    $labels = @($DFromRuns -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    $resultsD = @()
    $best = $null
    foreach ($lbl in $labels) {
        $c = Measure-Cell $lbl
        $r = Test-DCapture $c 60.0
        $r['label'] = $lbl
        $resultsD += $r
        if ($null -eq $best -or ($r.status -eq 'PASS' -and $best.status -ne 'PASS')) { $best = $r }
    }
    Write-JsonFile (Join-Path $script:Out 'D-renderdoc.json') ([ordered]@{ chosen = $best; runs = [object[]]$resultsD })
    $state = [ordered]@{
        status = @([ordered]@{ item = 'D — RenderDoc scene-gated capture'; status = $best.status; evidence = 'results\D-renderdoc.json' })
        commands = @(); plan = @(); save = $script:SaveSourceInfo
        cells = @(); acceptance = $null; b = $null; c = $null; d = $best; e = $null; smoke = $null
    }
    Write-ResultsMarkdown $state
    Write-Host ('D: {0} ({1})' -f $best.status, $best.label)
    foreach ($reason in @($best.reasons)) { Write-Host ('  reason: {0}' -f $reason) }
    exit $(if ($best.status -eq 'PASS') { 0 } else { 1 })
}

if ($AFromRuns -ne '') {
    $labels = @($AFromRuns -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($labels.Count -ne 3) { Write-Host '-AFromRuns needs "<base1>,<base2>,<legacy>"'; exit 2 }
    $cells3 = @($labels | ForEach-Object { Measure-Cell $_ })
    $acc = Get-Acceptance $cells3
    Write-JsonFile (Join-Path $script:Out 'A-acceptance.json') $acc
    $state = [ordered]@{
        status = @([ordered]@{ item = 'A — depth/barrier configuration arms'; status = $acc.status; evidence = 'results\A-acceptance.json' })
        commands = @(); plan = @(); save = $script:SaveSourceInfo
        cells = $cells3; acceptance = $acc; b = $null; c = $null; d = $null; e = $null; smoke = $null
    }
    Write-ResultsMarkdown $state
    Write-Host ('A acceptance: {0}' -f $acc.status)
    foreach ($reason in @($acc.reasons)) { Write-Host ('  reason: {0}' -f $reason) }
    exit $(if ($acc.status -eq 'PASS') { 0 } else { 1 })
}

if ($BCaptureFromRuns -ne '') {
    $labels = @($BCaptureFromRuns -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($labels.Count -ne 3) { Write-Host '-BCaptureFromRuns needs "<off1>,<off2>,<on>"'; exit 2 }
    $cells3 = @($labels | ForEach-Object { Measure-Cell $_ })
    $b = Test-BContent $cells3[0] $cells3[1] $cells3[2]
    $b['labels'] = $labels
    Write-JsonFile (Join-Path $script:Out 'B-content.json') $b
    $state = [ordered]@{
        status = @([ordered]@{ item = 'B — pixel content (OFF/OFF floor, OFF vs ON)'; status = $b.status; evidence = 'results\B-content.json; results\cells\*\compare-*.json' })
        commands = @(); plan = @(); save = $script:SaveSourceInfo
        cells = @(); acceptance = $null; b = $b; c = $null; d = $null; e = $null; smoke = $null
    }
    Write-ResultsMarkdown $state
    Write-Host ('B from captures: {0} (images compared {1})' -f $b.status, @($b.images | Where-Object { $_ }).Count)
    foreach ($reason in @($b.reasons)) { Write-Host ('  reason: {0}' -f $reason) }
    exit $(if ($b.status -eq 'PASS') { 0 } else { 1 })
}

if (-not $Authorized) {
    Write-Host 'refusing to start emulator cells without -Authorized (the Lead gates the GPU). Use -DryRun or -Plan.'
    exit 2
}
if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host ('executable not found: {0}' -f $Exe)
    exit 2
}
if (-not (Test-Path -LiteralPath $Harness)) {
    Write-Host ('harness not found: {0}' -f $Harness)
    exit 2
}
if (-not (Test-Path -LiteralPath $script:LockScript)) {
    Write-Host ('GPU lock not found: {0}' -f $script:LockScript)
    exit 2
}
if (-not (Test-Path -LiteralPath $script:SaveSourceInfo.save_data_dir)) {
    Write-Host ('save data not found: {0} (the classroom needs slot 1 MGS_GameData_1)' -f $script:SaveSourceInfo.save_data_dir)
    exit 3
}
if (-not $script:SaveSourceInfo.slot1_present -and -not $SkipSaveCheck) {
    Write-Host ('the save source has no MGS_GameData_1 (stage m00_00_10_10): {0}' -f $script:SaveSourceInfo.source)
    Write-Host 'the title would sit on the menu forever; refusing to run cells (use -SkipSaveCheck only for non-gameplay calibration)'
    exit 3
}

if ($script:UsePair) {
    $smoke = Invoke-RecyclerPair
    $state = [ordered]@{
        status = @([ordered]@{ item = 'recycler pair (off then on)'; status = $smoke.status; evidence = 'results\RECYCLER-PAIR.json / .md' })
        commands = @(Get-Commands); plan = [object[]]$plan; save = $script:SaveSourceInfo
        cells = @(); acceptance = $null; b = $null; c = $null; d = $null; e = $null; smoke = $smoke
    }
    Write-ResultsMarkdown $state
    $exit = if ($smoke.status -in @('RECYCLER-REGRESSION-SUSPECTED', 'OFF-DID-NOT-REACH')) { 1 } else { 0 }
    exit $exit
}

$state = [ordered]@{
    status = @(); commands = @(Get-Commands); plan = [object[]]$plan; save = $script:SaveSourceInfo
    cells = @(); acceptance = $null; b = $null; c = $null; d = $null; e = $null; smoke = $null
}
$cells = @{}
$cellRecords = New-Object System.Collections.Generic.List[object]

# A cells
if ($Modes -contains 'A') {
    $aPlan = @($plan | Where-Object { $_.mode -eq 'A' })
    foreach ($item in $aPlan) {
        $run = Invoke-Cell $item
        $cell = Measure-Cell $item.label
        $cellRecords.Add($cell)
        $cells[$item.label] = $cell
        Write-JsonFile (Join-Path (Join-Path $script:CellsDir $item.label) 'cell.json') ([ordered]@{ run = $run; cell = $cell })
        if ($StopOnInvalid -and ($null -eq $cell.measured -or -not $cell.measured.valid)) { break }
    }
    $aCells = @($aPlan | ForEach-Object { $cells[$_.label] })
    $state.cells = @($aCells)
    $state.acceptance = Get-Acceptance $aCells
    $state.status += [ordered]@{ item = 'A — recycler off/off/on/off'; status = $state.acceptance.status; evidence = 'results\cells\<label>\cell.json' }
}

# B: pixel compare between the A arms (base/base floor, then base vs legacy)
if ($Modes -contains 'B') {
    $aCellsAll = @($state.cells | Where-Object { $null -ne $_ })
    $baseCells = @($aCellsAll | Where-Object { $_.label -like '*-A-base-*' -and $null -ne $_.measured -and $_.measured.valid })
    $legacyCells = @($aCellsAll | Where-Object { $_.label -like '*-A-legacy-*' -and $null -ne $_.measured -and $_.measured.valid })
    if ($baseCells.Count -lt 2 -or $legacyCells.Count -lt 1) {
        $state.b = [ordered]@{ status = 'INVALID'; reasons = @(('B needs two valid base cells and one valid legacy cell; have base={0} legacy={1}. Run A first (same invocation).' -f $baseCells.Count, $legacyCells.Count)); images = @(); dump_mode = 'harness -Capture' }
    } else {
        $state.b = Test-BContent $baseCells[0] $baseCells[1] $legacyCells[0]
    }
    Write-JsonFile (Join-Path $script:Out 'B-content.json') $state.b
    $state.status += [ordered]@{ item = 'B — depth/barrier pixel content'; status = $state.b.status; evidence = 'results\B-content.json; results\cells\*\compare-*.json' }
}

# C
if ($Modes -contains 'C') {
    $cItem = @($plan | Where-Object { $_.mode -eq 'C' })[0]
    $run = Invoke-Cell $cItem
    $cell = Measure-Cell $cItem.label
    $cellRecords.Add($cell)
    $state.c = Test-CShutdown $cell
    $state.c['cell'] = $cell
    Write-JsonFile (Join-Path $script:Out 'C-shutdown.json') $state.c
    $state.status += [ordered]@{ item = 'C — R6 clean shutdown'; status = $state.c.status; evidence = 'results\C-shutdown.json' }
}

# D
if ($Modes -contains 'D') {
    $dItem = @($plan | Where-Object { $_.mode -eq 'D' })[0]
    if ($BRdocSeconds -le 0) {
        # Place the capture a few seconds after the gameplay anchor of the first valid A cell.
        $anchor = $null
        foreach ($c in @($state.cells)) { if ($null -ne $c -and $null -ne $c.measured -and $c.measured.valid -and $null -ne $c.measured.scene_first_s) { $anchor = $c.measured.scene_first_s; break } }
        if ($null -ne $anchor) { $script:BRdocSeconds = [math]::Round([double]$anchor + 5.0, 1); $dItem.extra += @('-RdocSeconds', "$script:BRdocSeconds") }
    }
    $run = Invoke-Cell $dItem
    $cell = Measure-Cell $dItem.label
    $cellRecords.Add($cell)
    $meta = $cell.meta
    $gameplayAnchor = $null
    if ($null -ne $meta -and $null -ne $meta.scene -and $null -ne $meta.scene.gameplay) { $gameplayAnchor = $meta.scene.gameplay.first_s }
    $state.d = Test-DCapture $cell $gameplayAnchor
    $state.d['cell'] = $cell
    Write-JsonFile (Join-Path $script:Out 'D-renderdoc.json') $state.d
    $state.status += [ordered]@{ item = 'D — RenderDoc capture'; status = $state.d.status; evidence = 'results\D-renderdoc.json' }
}

# E
if ($Modes -contains 'E') {
    $eItem = @($plan | Where-Object { $_.mode -eq 'E' })[0]
    $run = Invoke-Cell $eItem
    $cell = Measure-Cell $eItem.label
    $cellRecords.Add($cell)
    $state.e = $cell
    Write-JsonFile (Join-Path $script:Out 'E-lowres.json') $cell
    $eStatus = if ($null -ne $cell.measured -and $cell.measured.valid) { 'PASS' } else { 'INVALID' }
    $state.status += [ordered]@{ item = 'E — 100 %/120 Hz'; status = $eStatus; evidence = 'results\E-lowres.json' }
}

# Per-cell summary + RESULTS.md
$allCells = [object[]]$cellRecords
Write-JsonFile (Join-Path $script:Out 'cells.json') @($allCells)
if ($state.status.Count -eq 0) {
    $state.status += [ordered]@{ item = 'A–E'; status = 'NOT RUN'; evidence = '-'; }
}
Write-ResultsMarkdown $state
Write-Host ('results written: {0}' -f (Join-Path $script:Out 'RESULTS.md'))
$exit = 0
foreach ($row in @($state.status)) { if ($row.status -in @('FAIL', 'FAIL/INVALID', 'INVALID')) { $exit = 1 } }
exit $exit
