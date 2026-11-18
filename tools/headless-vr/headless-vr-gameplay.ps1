# Headless VR gameplay harness: one bounded, evidence-producing emulator session with the
# emulated PSVR headset and no physical headset.
#
# The session is defined by the pinned benchmark configuration in _build\bench\README.md plus the
# headless switches (SHADPS4_VR=1 SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0). It runs in an isolated
# portable lab, launches the emulator directly (never pc-vr\launch.ps1: that rewrites
# config.json and loses the flushed log tail), walks the log live, and stops at a point of the
# *scene*, never of the clock and never on FPS.
#
# Two scene gates:
#   -Gate menu     first heavy phase (the classroom main menu). Not gameplay; a calibration and
#                  smoke-test gate. result.json says phase="menu", gameplay=false.
#   -Gate heavy    first heavy phase, no band or image checks (calibration only).
#   -Gate gameplay (default) menu phase -> loading dip -> next settled heavy phase whose eye
#                  image differs from the menu baseline. Draws alone are ambiguous: the menu is
#                  ~485 draws and ~480 Mpx, which is inside the same band as a gameplay phase.
#
# Usage (inside the GPU lock; see _build\headless\gpu-lock-protocol.md):
#   bash gpu-lock.sh run --owner harness-eng --kind headless --reason "menu smoke" --ttl 1800 \
#        --require-quiet -- pwsh -File M:/Development/Deracine-work/_build/headless/headless-vr-gameplay.ps1 \
#        -Exe M:/Development/Deracine-work/DeracinePCVR/build/shadPS4.exe -Label smoke-menu-1 -Gate menu
#
#   # A/B/A of the recycler arm, same executable
#   pwsh -File headless-vr-gameplay.ps1 -Exe <exe> -Label ab-recycler -Mode aba
#   # RenderDoc scene-gated capture (needs the scene-gated capture block in the build)
#   pwsh -File headless-vr-gameplay.ps1 -Exe <exe> -Label rdoc-menu -Mode rdoc -RdocScene "400,450"
#   # 100 % render at 120 Hz, reported separately
#   pwsh -File headless-vr-gameplay.ps1 -Exe <exe> -Label lowres-100 -Mode lowres
#   # parser/gate self-test on an existing log, no emulator
#   pwsh -File headless-vr-gameplay.ps1 -SelfTest -SelfTestLog <log> -SelfTestWrite
[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    # The executable to measure. Copied to <lab>\shadps4.exe for the session.
    [Parameter(Mandatory, ParameterSetName = 'Run', Position = 0)][string]$Exe,
    # Self-test: one or more existing emulator logs to parse instead of running the emulator.
    [Parameter(Mandatory, ParameterSetName = 'SelfTest')][string[]]$SelfTestLog,
    [Parameter(ParameterSetName = 'SelfTest')][switch]$SelfTest,
    # Self-test: write <Root>\_build\headless\selftest\<name>.json next to the printed result.
    [Parameter(ParameterSetName = 'SelfTest')][switch]$SelfTestWrite,
    # Name of this cell; runs\<label>\ holds its evidence. Default: derived from mode/arm/time.
    [string]$Label = '',
    # Lab directory. Default: <Root>\_build\headless\labs\<label> (aba cells share one lab).
    [string]$Lab = '',
    # Arm: on/off set the arm variable to 1/0; baseline leaves it unset.
    [ValidateSet('on', 'off', 'baseline')][string]$Arm = 'on',
    # The variable the arms differ in. The recycler is the historical arm.
    [string]$ArmVariable = 'SHADPS4_DBG_IMAGE_RECYCLER',
    # Extra environment for the emulator: VAR=VALUE entries (a ';'-joined string or a list).
    # Applied after the pinned set, so an entry wins over a pin; recorded in session/meta.
    [string[]]$Env = @(),
    # Synthetic tracked Move hands (SHADPS4_VR_HANDS=synthetic, SHADPS4_VR_MOVE_FROM_PAD=1).
    # auto = on only for -Gate gameplay: M2 is proven with them on, together with AUTOPRESS
    # (without AUTOPRESS they stall on the camera/headset check screen). M1 menu cells leave them
    # off so they do not depend on the patched build.
    [ValidateSet('auto', 'on', 'off')][string]$SyntheticHands = 'auto',
    # cell = one session; aba = on/off/on with the same executable; rdoc = RenderDoc capture;
    # lowres = 100 % render at 120 Hz (a separate configuration, not comparable with 300 % cells).
    [ValidateSet('cell', 'aba', 'rdoc', 'lowres')][string]$Mode = 'cell',
    # What the measured window is gated on: gameplay, menu (calibration/smoke) or heavy.
    [ValidateSet('gameplay', 'menu', 'heavy')][string]$Gate = 'gameplay',
    # Resolution-specific gameplay profile: auto derives it from -RenderPercent. 100 has its own
    # eye/image size, scene anchor and workload band; the menu/gameplay classification stays
    # separate (draws are resolution-independent, so 800 excludes the ~465-draw menu).
    [ValidateSet('auto', '300', '100')][string]$Profile = 'auto',
    # The guest's own render size, in percent of a PSVR's 960x1080 an eye (300 = 2880x3240).
    [int]$RenderPercent = 300,
    # The emulated display's refresh rate; the title paces itself to it.
    [int]$Hz = 120,
    # Seconds of the measured window, after the warmup is discarded.
    [int]$Seconds = 40,
    # Seconds discarded after the scene anchor.
    [int]$Warmup = 10,
    # Scene gate: consecutive frame-stats windows with at least this many draws per frame and
    # this much drawn area per frame. 400/450 matches the classroom heavy phase; the light
    # front end is 22-38 draws / 150-240 Mpx.
    [int]$SceneMinDraws = 400,
    [double]$SceneMinMpx = 450,
    # Consecutive windows the gate needs.
    [int]$SceneSamples = 2,
    # The gameplay phase must be at least this heavy (draws per frame): the menu is ~465-500 and
    # the loading screen 42-55, the confirmed gameplay phase ~1866-1900 draws.
    [int]$GameplayMinDraws = 800,
    # Documented band for the measured window's means. 0 disables a bound. The band is the
    # broad heavy-phase band, not the gameplay discriminator (the menu is inside it too).
    [int]$BandMinDraws = 400,
    [int]$BandMaxDraws = 620,
    [double]$BandMinMpx = 400,
    [double]$BandMaxMpx = 650,
    [int]$BandMinPasses = 0,
    [int]$BandMaxPasses = 0,
    # Gameplay gate: a window with draws <= LoadingMaxDraws or Mpx <= LoadingMaxMpx counts as the
    # loading transition between the menu and the next heavy phase.
    [int]$LoadingMaxDraws = 200,
    [double]$LoadingMaxMpx = 250,
    # Gameplay gate: the heavy phase after the transition must be this many windows long and its
    # draws/Mpx spread (max-min over mean) must be at most 2x SettleTolerancePct percent.
    [int]$SettleWindows = 4,
    [double]$SettleTolerancePct = 5,
    # Gameplay gate: the candidate eye picture must differ from the menu baseline: SSIM (percent)
    # at most this, or a different masked hash. 95 is capture-diff.py's "different picture".
    [ValidateSet('on', 'off')][string]$ImageGate = 'on',
    [double]$ImageDiffMaxSsim = 95,
    # Gameplay gate: when a validated gameplay reference exists, the candidate must also be close
    # to it (SSIM percent at least this). Animated gameplay does not repeat exactly; the default
    # is provisional until the first gameplay reference is measured.
    [double]$GameplayMinSsim = 80,
    # The stored menu eye picture (front-end, invalid). Every cell records its path and hash.
    [string]$MenuReference = '',
    # The stored gameplay eye picture (validated once against docs\screenshots\deracine_vr_gameplay.jpg).
    [string]$GameplayReference = '',
    # Copy this cell's accepted gameplay eye picture to the gameplay reference path (use once,
    # after checking the picture looks like the gameplay screenshot).
    [switch]$SetGameplayReference,
    # What the HMD eye lines must say. Mismatches invalidate the cell (they are not warnings).
    [string]$ExpectEye = '2880x3240',
    [string]$ExpectImage = '5760x3240',
    [string]$ExpectFormat = 'R8G8B8A8Unorm',
    [int]$ExpectSamples = 1,
    # Capture mode: one render-target dump after the measured window (outside the timed window).
    [switch]$Capture,
    # SHADPS4_DUMP_WIDTH: how wide a dumped image is at most.
    [int]$DumpWidth = 1920,
    # Optional SHADPS4_DBG_DUMP_HMD_FRAMES list (frame numbers). These dumps land inside the run
    # and are marked as such; prefer -Capture for the timed path.
    [string]$DumpFrames = '',
    # RenderDoc scene gate "<draws>,<mpx>" (SHADPS4_DBG_RDOC_CAPTURE_SCENE) for -Mode rdoc.
    [string]$RdocScene = '400,500',
    # RenderDoc time trigger (SHADPS4_DBG_RDOC_CAPTURE); 0 = scene gate only.
    [double]$RdocSeconds = 0,
    # The session is abandoned after this long.
    [int]$MaxSeconds = 420,
    # Attempts for one cell: the title has a known ~18-20 s startup crash (0xC0000005), and a
    # crashed attempt is not a measurement. Each attempt keeps its own run directory.
    [int]$Attempts = 3,
    # Name recorded as the GPU lock owner. Default: headless-vr-gameplay:<label>.
    [string]$LockOwner = '',
    # Do not acquire the machine-wide GPU lock (the caller already holds it, or this is a
    # deliberate single-session run).
    [switch]$NoLock,
    # Refuse to start when another GPU/CPU benchmark process is running (the lock has the same
    # audit; this one also protects a -NoLock run).
    [switch]$RequireQuiet,
    # Seconds to wait for a quiet machine when -RequireQuiet is given.
    [int]$QuietWait = 900,
    # Leave the lab in place (it is reused by default, so this is the default behaviour; the
    # switch is kept for symmetry with the old harness).
    [switch]$KeepLab,
    # What SHADPS4_VR_FOV (100) is a percent of: "psvr" (the console headset's own field of view),
    # "headset" (only meaningful with an OpenXR host), or empty = leave the emulator's default.
    # With OPENXR=0 there is no host to ask, so the default is unset.
    [ValidateSet('', 'psvr', 'headset')][string]$FovOf = '',
    # The build's own pad automation (AUTOPRESS): it presses pad down/cross and the Move button
    # every few seconds. Default: on for -Gate gameplay (it is what passes the camera/headset
    # check screen; it stops at -AutopressSeconds, 45 s, before the menu), off for -Gate
    # menu/heavy. -NoAutopress forces it off, -Autopress forces it on.
    [switch]$Autopress,
    [int]$AutopressSeconds = 45,
    # AUTOPRESS's menu cycle (down/cross after frame 1800). Off: the confirmed M2 winner does not
    # use it, and it would fight the scripted Move aim on the menu.
    [switch]$AutopressMenu,
    # SHADPS4_CLEAN_EXIT=1 plus -ExitAfterSeconds: only a build with the clean-exit path uses it;
    # the emulator then exits by itself and shutdown.clean_exit is reported.
    [switch]$CleanExit,
    # Capture mode only: SHADPS4_DBG_DUMP_HMD_AT_SCENE="<draws>,<mpx>" lets the emulator dump the
    # HMD frame on the first frame-stats window that matches, so a picture is scene-anchored
    # rather than taken at a fixed frame. Such a cell is capture-only (no timed window).
    [string]$HmdDumpAtScene = '',
    # Fixed input script (SHADPS4_INPUT_SCRIPT). Default: inputs\menu-continue.txt.
    [string]$InputScript = '',
    # Force the build's own pad automation off (it is off by default).
    [switch]$NoAutopress,
    # SHADPS4_EXIT_AFTER=<seconds>: only a build with the clean-exit path uses it. 0 = off.
    [int]$ExitAfterSeconds = 0,
    # Frame-stats window, seconds (SHADPS4_FRAME_STATS_EVERY).
    [double]$StatsEvery = 0.5,
    # The game: the shadow app with the 300 % render configuration.
    [string]$Eboot = 'D:\Temp\Deracine\CUSA11919-qual\eboot.bin',
    # Empty = derive from the script location; pass it when the layout differs.
    # Fixture locations; empty = the conventional place under -Root.
    [string]$SourceLab = '',
    [string]$SavedataSource = '',
    [string]$Shadow = '',
    [string]$Root = '',
    # The lock script (Windows path); converted to its WSL path for bash.
    [string]$LockScript = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

# --- paths and constants ----------------------------------------------------------------------
$script:ScriptDir = Split-Path -Parent $PSCommandPath
if ($Root -eq '') {
    # <root>/_build/headless/... or <repo>/tools/headless-vr/...
    if ((Split-Path -Leaf $script:ScriptDir) -in @('headless', 'headless-vr')) {
        $Root = Split-Path -Parent (Split-Path -Parent $script:ScriptDir)
    } else {
        $Root = (Get-Location).Path
    }
}
if ($Shadow -eq '') { $Shadow = Split-Path -Parent $Eboot }
if ($SourceLab -eq '') { $SourceLab = Join-Path $Root 'lab\pc-vr-quality' }
if ($SavedataSource -eq '') { $SavedataSource = Join-Path $Root '_run\pc-vr-recycler-try\user\home\1000\savedata\CUSA11919' }
$script:HeadlessDir = Join-Path $Root '_build\headless'
$script:RunsRoot = Join-Path $script:HeadlessDir 'runs'
$script:LabsRoot = Join-Path $script:HeadlessDir 'labs'
$script:TemplateDir = Join-Path $script:HeadlessDir 'template'
$script:InputsDir = if (Test-Path (Join-Path $script:ScriptDir 'inputs')) { Join-Path $script:ScriptDir 'inputs' } else { Join-Path $script:HeadlessDir 'inputs' }
$script:ReferencesDir = if (Test-Path (Join-Path $script:ScriptDir 'references')) { Join-Path $script:ScriptDir 'references' } else { Join-Path $script:HeadlessDir 'references' }
$script:SelfTestDir = Join-Path $script:HeadlessDir 'selftest'
if ($MenuReference -eq '') { $MenuReference = Join-Path $script:ReferencesDir 'menu-eye.png' }
if ($GameplayReference -eq '') { $GameplayReference = Join-Path $script:ReferencesDir 'gameplay-eye.png' }
$script:Shadow = $Shadow
$script:IniPath = Join-Path $script:Shadow 'gameproject\saved\config\ps4\engine.ini'
# Refuse to write engine.ini outside a designated private shadow. This is the same rule the
# integrated runner enforces in game_shadow.py; it keeps this harness from touching the active
# installation or a shared game directory.
$shadowManifest = Join-Path $script:Shadow 'benchmark-shadow.json'
if (-not (Test-Path -LiteralPath $shadowManifest -PathType Leaf)) {
    throw "refusing to write engine.ini: '$($script:Shadow)' has no benchmark-shadow.json (private shadow required)"
}
$shadowDoc = Get-Content -LiteralPath $shadowManifest -Raw | ConvertFrom-Json
if ($shadowDoc.private -ne $true) {
    throw "refusing: $shadowManifest does not mark the shadow private"
}
$ebootHash = (Get-FileHash -LiteralPath $Eboot -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ebootHash -ne ([string]$shadowDoc.eboot_sha256).ToLowerInvariant()) {
    throw "refusing: the eboot hash does not match $shadowManifest"
}
foreach ($required in @(@('SourceLab', $SourceLab), @('SavedataSource', $SavedataSource), @('Shadow', $Shadow), @('Eboot', $Eboot))) {
    if (-not (Test-Path -LiteralPath $required[1])) {
        throw "missing -$($required[0]): $($required[1]); all portable paths must be supplied (no private _build defaults)"
    }
}
$script:SavedataSource = $SavedataSource
$script:SourceLab = $SourceLab
$script:RenderDocCmd = 'C:\Program Files\RenderDoc\renderdoccmd.exe'
$script:CaptureDiff = Join-Path $Root '_build\bench\capture-diff.py'
if ($LockScript -eq '') { $LockScript = Join-Path $Root '_build\team\gpu-lock.sh' }
$script:LockScript = $LockScript
$script:Inv = [System.Globalization.CultureInfo]::InvariantCulture
$script:StatsRegex = [regex]'frame stats at\s+(?<at>\d+)\s+s(?:\s+\[(?<ab>[A-Z+]+)\])?:\s+(?<fps>[0-9.]+)\s+fps,\s+per frame\s+(?<passes>[0-9.]+)\s+passes\s+\((?<loading>[0-9.]+)\s+loading their targets,\s+(?<mpx>[0-9.]+)\s+Mpx\),\s+(?<draws>[0-9.]+)\s+draws,\s+(?<dispatches>[0-9.]+)\s+dispatches,\s+(?<submits>[0-9.]+)\s+submits(?:,\s+GPU busy\s+(?<busy>[0-9]+)%(?:\s+at\s+(?<mhz>[0-9]+)\s+MHz)?)?'
$script:HmdRegex = [regex]'PrepareHmdFrame: HMD frame\s+(?<frame>\d+)\s+eye\s+(?<eye>[01]): image\s+(?<image>\d+)\s+at\s+(?<addr>0x[0-9a-fA-F]+),\s+(?<w>\d+)x(?<h>\d+),\s+format\s+(?<fmt>\S+),\s+samples\s+(?<samples>\d+)'
$script:VrRegex = [regex]'PrepareHmdFrame: VR: the title draws\s+(?<w>\d+)x(?<h>\d+)\s+an eye;\s+the headset is shown\s+(?<pw>\d+)x(?<ph>\d+)\s+an eye[^,]*,\s+(?<sw>\d+)x(?<sh>\d+)\s+for both'
$script:SwapRegex = [regex]'EnsureSwapchain: Frames are shown through\s+(?<n>\d+)\s+images of\s+(?<w>\d+)x(?<h>\d+)'
$script:SelfTestMode = $false
if ($PSCmdlet.ParameterSetName -eq 'SelfTest' -or $SelfTest -or ($SelfTestLog.Count -gt 0)) {
    $script:SelfTestMode = $true
}

# --- small helpers ----------------------------------------------------------------------------
function Round3([double]$v) { return [math]::Round($v, 3) }
function Round1([double]$v) { return [math]::Round($v, 1) }

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

function Get-Stats([double[]]$Values) {
    if ($null -eq $Values -or $Values.Count -eq 0) { return [ordered]@{ count = 0 } }
    $mean = ($Values | Measure-Object -Average).Average
    $sd = 0.0
    if ($Values.Count -gt 1) {
        $sum = 0.0
        foreach ($v in $Values) { $sum += ($v - $mean) * ($v - $mean) }
        $sd = [math]::Sqrt($sum / ($Values.Count - 1))
    }
    return [ordered]@{
        count = $Values.Count
        mean  = Round3 $mean
        p50   = Round3 (Get-Percentile $Values 50)
        p90   = Round3 (Get-Percentile $Values 90)
        p99   = Round3 (Get-Percentile $Values 99)
        min   = Round3 (($Values | Measure-Object -Minimum).Minimum)
        max   = Round3 (($Values | Measure-Object -Maximum).Maximum)
        stdev = Round3 $sd
    }
}

function Get-MeanOf($Values) {
    $v = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return $null }
    return ($v | Measure-Object -Average).Average
}

function Get-FileSha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Get-FileSize([string]$Path) { return (Get-Item -LiteralPath $Path).Length }

function Write-JsonFile([string]$Path, $Object) {
    $json = $Object | ConvertTo-Json -Depth 14
    [IO.File]::WriteAllText($Path, $json + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
}

function Write-TextFile([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-PngSize([string]$Path) {
    try {
        $b = New-Object byte[] 24
        $fs = [IO.File]::OpenRead($Path)
        try { [void]$fs.Read($b, 0, 24) } finally { $fs.Dispose() }
        if ($b[0] -ne 0x89 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47) { return $null }
        $w = ([int]$b[16] * 16777216) + ([int]$b[17] * 65536) + ([int]$b[18] * 256) + [int]$b[19]
        $h = ([int]$b[20] * 16777216) + ([int]$b[21] * 65536) + ([int]$b[22] * 256) + [int]$b[23]
        return [ordered]@{ width = $w; height = $h }
    } catch { return $null }
}

function Get-GpuState {
    $state = [ordered]@{}
    $smi = 'C:\Windows\system32\nvidia-smi.exe'
    if (Test-Path $smi) {
        $query = & $smi --query-gpu=name,driver_version,temperature.gpu,clocks.sm,utilization.gpu,power.draw,memory.used --format=csv,noheader 2>$null
        if ($LASTEXITCODE -eq 0 -and $query) {
            $parts = ($query -join '') -split ',\s*'
            $state.name = $parts[0]
            $state.driver = $parts[1]
            $state.temperature_c = $parts[2]
            $state.sm_clock_mhz = $parts[3]
            $state.utilization_pct = $parts[4]
            $state.power_w = $parts[5]
            $state.memory_used_mib = $parts[6]
        }
    }
    return $state
}

# A frame-rate cell is only comparable when no other GPU/CPU benchmark work is running. The
# audit records what it saw; it never stops anything it did not start.
$script:QuietNames = @('shadps4', 'ninja', 'clang-cl', 'link', 'msbuild', 'renderdoccmd', 'qrenderdoc')
function Get-ProcessAudit {
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
        $n = $p.ProcessName.ToLowerInvariant()
        if ($script:QuietNames -contains $n) {
            $path = $null
            try { $path = $p.Path } catch {}
            $list.Add(('{0}:{1}:{2}' -f $n, $p.Id, $path))
        }
    }
    return , $list.ToArray()
}

function Get-OtherEmulators([string]$OurExe) {
    $list = New-Object System.Collections.Generic.List[string]
    foreach ($p in @(Get-Process -Name 'shadps4' -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq (Join-Path $Lab 'shadps4.exe') } catch { $false } })) {
        $path = $null
        try { $path = $p.Path } catch {}
        if ($path -ne $OurExe) { $list.Add(('{0}:{1}' -f $p.Id, $path)) }
    }
    return , $list.ToArray()
}

function Convert-ToWslPath([string]$Path) {
    $p = $Path -replace '\\', '/'
    if ($p -match '^([A-Za-z]):/(.*)$') { return '/mnt/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2] }
    return $p
}

function ConvertTo-SingleQuoted([string]$Text) { return "'" + ($Text -replace "'", "'\''") + "'" }

# Runs a bash command through WSL and returns stdout/stderr/exit code. Used for the GPU lock so
# that old testslot.sh users and this harness share one lock protocol. The command is passed as
# one double-quoted argument because Start-Process does not quote an argument list for us.
function Invoke-WslBash([string]$Command) {
    $outFile = [IO.Path]::GetTempFileName()
    $errFile = [IO.Path]::GetTempFileName()
    try {
        $argLine = 'bash -lc "' + ($Command -replace '"', '\"') + '"'
        $p = Start-Process -FilePath 'wsl.exe' -ArgumentList $argLine -PassThru `
            -WindowStyle Hidden -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        [void]$p.WaitForExit(120000)
        $stdout = ''
        $stderr = ''
        try { $stdout = [IO.File]::ReadAllText($outFile) } catch {}
        try { $stderr = [IO.File]::ReadAllText($errFile) } catch {}
        return [pscustomobject]@{ exit_code = $p.ExitCode; stdout = $stdout.Trim(); stderr = $stderr.Trim() }
    } finally {
        Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
    }
}

# --- the GPU lock -----------------------------------------------------------------------------
# Two ways in: the caller already holds the slot through `gpu-lock.sh run` (the wrapper exports
# GPU_LOCK_TOKEN, and the harness then must not acquire or release), or the harness acquires the
# slot itself and heartbeats inline from its own polling loop. There is deliberately no detached
# heartbeat process: if the harness dies, the lock must expire on its own TTL.
$script:Lock = [ordered]@{ source = 'none'; owner = ''; token = ''; acquired = $false }
$script:LastHeartbeat = Get-Date

function Start-LockHold([string]$Owner, [string]$Reason, [int]$Ttl) {
    $envToken = $env:GPU_LOCK_TOKEN
    if ($envToken) {
        $script:Lock.source = 'wrapper'
        $script:Lock.owner = $(if ($env:GPU_LOCK_OWNER) { $env:GPU_LOCK_OWNER } else { $Owner })
        $script:Lock.token = $envToken
        Write-Host ("gpu lock: held by the wrapper as {0} (token {1})" -f $script:Lock.owner, $envToken)
        return
    }
    $lockWsl = Convert-ToWslPath $script:LockScript
    $cmd = "bash $(ConvertTo-SingleQuoted $lockWsl) acquire --owner $(ConvertTo-SingleQuoted $Owner) " +
    "--reason $(ConvertTo-SingleQuoted $Reason) --ttl $Ttl --kind headless"
    if ($RequireQuiet) { $cmd += " --require-quiet --quiet-wait $QuietWait" }
    $r = Invoke-WslBash $cmd
    if ($r.exit_code -ne 0) {
        Write-Host ("gpu lock: acquire failed (exit {0}): {1}" -f $r.exit_code, $r.stderr)
        exit $r.exit_code
    }
    $script:Lock.source = 'self'
    $script:Lock.owner = $Owner
    $script:Lock.token = $r.stdout
    $script:Lock.acquired = $true
    Write-Host ("gpu lock: acquired as {0} (token {1})" -f $Owner, $r.stdout)
}

function Send-LockHeartbeat {
    if ($script:Lock.source -ne 'self') { return }
    $now = Get-Date
    if (($now - $script:LastHeartbeat).TotalSeconds -lt 15) { return }
    $script:LastHeartbeat = $now
    $lockWsl = Convert-ToWslPath $script:LockScript
    [void](Invoke-WslBash ("bash $(ConvertTo-SingleQuoted $lockWsl) heartbeat --token $(ConvertTo-SingleQuoted $script:Lock.token)"))
}

function Stop-LockHold {
    if ($script:Lock.source -ne 'self') { return }
    $lockWsl = Convert-ToWslPath $script:LockScript
    [void](Invoke-WslBash ("bash $(ConvertTo-SingleQuoted $lockWsl) release --token $(ConvertTo-SingleQuoted $script:Lock.token)"))
    $script:Lock.source = 'released'
}

# --- the lab ----------------------------------------------------------------------------------
# The template is one copy of the quality lab's portable runtime with the log, captures, caches
# and dumps left out, and the real save data put in. Per-cell labs are copied from it, so a cell
# can never write into lab\pc-vr-quality or another agent's lab.
function New-LabTemplate {
    if (Test-Path $script:TemplateDir) { return }
    Write-Host ("creating the lab template from {0}" -f $script:SourceLab)
    New-Item -ItemType Directory -Path $script:TemplateDir -Force | Out-Null
    $exclude = @('captures', 'log', 'temp', 'cache', 'rt_dump', 'rt_dump*')
    Copy-Item -Path (Join-Path $script:SourceLab '*') -Destination $script:TemplateDir -Recurse -Force -Exclude $exclude
    $tplUser = Join-Path $script:TemplateDir 'user'
    Get-ChildItem -LiteralPath $script:TemplateDir -Force -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    # Copy-Item -Exclude does not reliably skip nested directories, so the heavy per-session state
    # is removed explicitly: captures (tens of MB), caches, temp, dumps and config backups.
    foreach ($name in @('captures', 'cache', 'temp', 'screenshots', 'log')) {
        Remove-Item -LiteralPath (Join-Path $tplUser $name) -Recurse -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $tplUser -Force -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    # Note: -Filter 'config.json.*' also matches config.json itself on Windows (DOS wildcard
    # semantics); match the backup names explicitly so the lab's real config is kept.
    Get-ChildItem -LiteralPath $tplUser -Force -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'config.json.*' -and $_.Name -ne 'config.json' } |
        Remove-Item -Force -ErrorAction SilentlyContinue
    # The save data with real progress (Stage m00_00_10_10). Copied, never moved: the source lab
    # belongs to another investigation.
    $dst = Join-Path $script:TemplateDir 'user\home\1000\savedata\CUSA11919'
    if (Test-Path $script:SavedataSource) {
        Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $script:SavedataSource -Destination $dst -Recurse -Force
        Write-Host ("save data copied into the template: {0}" -f $dst)
    } else {
        Write-Warning ("save data not found: {0} (the menu will have no Continue)" -f $script:SavedataSource)
    }
    Write-Host ("template ready: {0}" -f $script:TemplateDir)
}

function Initialize-Lab([string]$LabDir) {
    New-LabTemplate
    if (-not (Test-Path $LabDir)) {
        Write-Host ("creating the lab from the template: {0}" -f $LabDir)
        New-Item -ItemType Directory -Path $LabDir -Force | Out-Null
        Copy-Item -Path (Join-Path $script:TemplateDir '*') -Destination $LabDir -Recurse -Force
    }
    # Per-cell clean state. The save data is refreshed from the source each time so a session
    # that advances the game cannot make the next cell start from a different place.
    $user = Join-Path $LabDir 'user'
    # A reused lab must start from the same state every time: no log, no dumps, no captures, no
    # shader cache and no config backups from an earlier cell.
    foreach ($name in @('captures', 'cache', 'temp')) {
        Remove-Item -LiteralPath (Join-Path $user $name) -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Path (Join-Path $user 'log') -Force | Out-Null
    Remove-Item -LiteralPath (Join-Path $user 'log\shad_log.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $user 'log\shad_log.prev.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $user 'dump_render_targets') -Force -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $user -Force -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $user -Force -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'config.json.*' -and $_.Name -ne 'config.json' } |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $gameCache = Join-Path $user 'cache\CUSA11919'
    if (Test-Path $gameCache) { Remove-Item -LiteralPath $gameCache -Recurse -Force -ErrorAction SilentlyContinue }
    $save = Join-Path $user 'home\1000\savedata\CUSA11919'
    if (Test-Path $script:SavedataSource) {
        Remove-Item -LiteralPath $save -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path (Split-Path $save -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $script:SavedataSource -Destination $save -Recurse -Force
    }
    return $LabDir
}

function Get-PinnedSettingsText([int]$Percent, [int]$RefreshHz, [string]$Arm, [hashtable]$EnvMap) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# Derived by _build\headless\headless-vr-gameplay.ps1. Not edited by hand.')
    $lines.Add('# Fixed render configuration of the headless VR gameplay cell.')
    $lines.Add('fps=' + $RefreshHz)
    $lines.Add('resolution=2880')
    $lines.Add('dynamic=0')
    $lines.Add('fov=100')
    $lines.Add('sharpen=0.25')
    $lines.Add('aniso=16')
    $lines.Add('antialias=1')
    $lines.Add('desktop_view=stereo')
    $lines.Add('desktop_crop=0')
    $lines.Add('readbacks=relaxed')
    $lines.Add('readback_linear_images=1')
    $lines.Add('copy_gpu_buffers=0')
    $lines.Add('move_pitch=-45')
    $lines.Add('move_y=0.000')
    $lines.Add('move_z=-0.135')
    $lines.Add('swap_hands=1')
    $lines.Add('hands=1')
    $lines.Add('language=en-US')
    $lines.Add('game=' + $Eboot)
    $lines.Add('render_percent=' + $Percent)
    $lines.Add('menu=0')
    $lines.Add('pause=0')
    $lines.Add('msaa=2')
    foreach ($key in $EnvMap.Keys) { $lines.Add('env=' + $key + '=' + $EnvMap[$key]) }
    return ($lines -join "`r`n") + "`r`n"
}

function Set-LabConfig([string]$LabDir, [int]$RefreshHz) {
    $cfgPath = Join-Path $LabDir 'user\config.json'
    $cfg = Get-Content -LiteralPath $cfgPath -Raw | ConvertFrom-Json
    $cfg.Log.type = 'file'
    $cfg.Log.filter = '*:Info'
    $cfg.Log.sync = $true
    $cfg.Log.skip_duplicate = $true
    $cfg.GPU.readbacks_mode = 1
    $cfg.GPU.copy_gpu_buffers = $false
    $cfg.GPU.readback_linear_images_enabled = $true
    $cfg.GPU.vblank_frequency = $RefreshHz
    $cfg.Vulkan.pipeline_cache_enabled = $false
    ($cfg | ConvertTo-Json -Depth 20) | Set-Content -LiteralPath $cfgPath -Encoding utf8
}

# The game shadow's Engine.ini is shared with every other lab (the render size is the game's own
# setting, not an emulator setting), so it is saved and put back exactly. The GPU lock excludes
# another emulator session for the duration.
$script:IniBefore = $null
$script:IniExisted = $false
function Save-ShadowIni([string]$RunDir) {
    $script:IniExisted = Test-Path $script:IniPath
    if ($script:IniExisted) {
        $script:IniBefore = [IO.File]::ReadAllBytes($script:IniPath)
        try { [IO.File]::WriteAllBytes((Join-Path $RunDir 'shadow-engine.ini.before'), $script:IniBefore) } catch {}
    }
}
$script:IniWritten = $false
function Write-ShadowIni([int]$Percent) {
    $desired = "[SystemSettings]`r`nr.ScreenPercentage=$Percent`r`nr.MSAACount=2`r`n"
    # The file is shared with every other lab. A 300 % cell is the common case and the file is
    # usually already 300/2, so it is left alone instead of rewritten; only a real change (for
    # example the 100 % lowres cell) writes it and is put back afterwards.
    if ($script:IniExisted) {
        try {
            $current = [IO.File]::ReadAllText($script:IniPath)
            if ($current -match ('r\.ScreenPercentage\s*=\s*' + $Percent + '\b') -and $current -match 'r\.MSAACount\s*=\s*2\b') {
                $script:IniWritten = $false
                return
            }
        } catch {}
    }
    $dir = Split-Path $script:IniPath -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Write-TextFile $script:IniPath $desired
    $script:IniWritten = $true
}
function Restore-ShadowIni {
    if (-not $script:IniWritten) { return }
    try {
        if ($script:IniExisted -and $null -ne $script:IniBefore) {
            [IO.File]::WriteAllBytes($script:IniPath, $script:IniBefore)
        } elseif (-not $script:IniExisted) {
            Remove-Item -LiteralPath $script:IniPath -Force -ErrorAction SilentlyContinue
        }
    } catch { Write-Warning ("the game's Engine.ini was not put back: {0}" -f $_) }
    $script:IniWritten = $false
}

# --- the environment the emulator inherits ----------------------------------------------------
function Get-BenchEnvironment([int]$Percent, [int]$RefreshHz, [double]$Every, [string]$Arm,
    [string]$InputScriptPath, [string]$LiveInputPath, [string]$DumpFrames, [int]$DumpWidth,
    [int]$ExitAfterSeconds, [int]$MaxSeconds, [string]$ArmVariable, [string]$FovOf,
    [bool]$Autopress, [int]$AutopressSeconds, [bool]$AutopressMenu, [bool]$CleanExit,
    [string]$HmdDumpAtScene, [bool]$SyntheticHands, [string[]]$ExtraEnv) {
    $env = [ordered]@{}
    # Headless: the emulated PSVR is the headset, no OpenXR host is looked for, and the title is
    # never told the headset was taken off (no UpdateWorn wait screen).
    $env['SHADPS4_VR'] = '1'
    $env['SHADPS4_OPENXR'] = '0'
    $env['SHADPS4_VR_DEMO'] = '0'
    # Pinned settings (the same mapping bench.ps1 applies to its settings.txt).
    $env['SHADPS4_TITLE_EYE_WIDTH'] = '2880'
    $env['SHADPS4_TITLE_RESOLUTION'] = '6'
    $env['SHADPS4_VR_SCALE'] = '0.5'
    $env['SHADPS4_VR_DESKTOP_VIEW'] = 'stereo'
    $env['SHADPS4_VR_DESKTOP_CROP'] = '0'
    $env['SHADPS4_VR_SHARPEN'] = '0.25'
    $env['SHADPS4_FORCE_ANISO'] = '16'
    $env['SHADPS4_MAX_MSAA'] = '2'
    $env['SHADPS4_VR_FOV'] = '100'
    if ($FovOf -ne '') { $env['SHADPS4_VR_FOV_OF'] = $FovOf }
    # The synthetic tracked Move hands: without these the headless build cannot aim the Move
    # pointer at "Continue", and the title never leaves its Move-driven menu. They are ignored by
    # a build that predates them, but M1 menu cells deliberately leave them off so they do not
    # depend on the patched build.
    if ($SyntheticHands) {
        $env['SHADPS4_VR_HANDS'] = 'synthetic'
        $env['SHADPS4_VR_MOVE_FROM_PAD'] = '1'
    }
    $env['SHADPS4_VR_REFRESH_RATE'] = "$RefreshHz"
    $env['SHADPS4_VR_FPS_CAP'] = "$RefreshHz"
    $env['SHADPS4_VR_FASTEST_PACE'] = '1'
    $env['SHADPS4_XR_PAUSE'] = '0'
    $env['SHADPS4_XR_WAIT'] = '60'
    $env['SHADPS4_CONSOLE_LANGUAGE'] = 'en-US'
    # The synthetic-hand aim assumes the wand's identity orientation points straight ahead
    # (MOVE_PITCH=0, scene-eng's confirmed M2 recipe). The physical Move calibration (-45) only
    # applies when no synthetic hand is driving the pointer.
    $env['SHADPS4_MOVE_PITCH'] = if ($SyntheticHands) { '0' } else { '-45' }
    $env['SHADPS4_MOVE_Y'] = '0.000'
    $env['SHADPS4_MOVE_Z'] = '-0.135'
    # Only affects the OpenXR controller mapping, not the synthetic Move hands; kept for parity
    # with the benchmark definition.
    $env['SHADPS4_SWAP_HANDS'] = '1'
    # Measurement plumbing.
    $env['SHADPS4_FRAME_STATS'] = '1'
    $env['SHADPS4_FRAME_STATS_EVERY'] = $Every.ToString($script:Inv)
    $env['SHADPS4_FRAME_TIMELINE'] = '1'
    # The per-second texture-cache counters (RENDERSTAT) the result.json carries. They cost the
    # run a little time, and both arms of an A/B pay it.
    $env['SHADPS4_DBG_OVERLAP'] = '1'
    $env['SHADPS4_DUMP_WIDTH'] = "$DumpWidth"
    if ($DumpFrames -ne '') { $env['SHADPS4_DBG_DUMP_HMD_FRAMES'] = $DumpFrames }
    if ($HmdDumpAtScene -ne '') { $env['SHADPS4_DBG_DUMP_HMD_AT_SCENE'] = $HmdDumpAtScene }
    if ($CleanExit) { $env['SHADPS4_CLEAN_EXIT'] = '1' }
    if ($ExitAfterSeconds -gt 0) { $env['SHADPS4_EXIT_AFTER'] = "$ExitAfterSeconds" }
    if ($InputScriptPath -ne '') { $env['SHADPS4_INPUT_SCRIPT'] = $InputScriptPath }
    if ($LiveInputPath -ne '') { $env['SHADPS4_LIVE_INPUT'] = $LiveInputPath }
    if ($Autopress) {
        $env['SHADPS4_DBG_AUTOPRESS'] = '1'
        $env['SHADPS4_DBG_AUTOPRESS_TO'] = "$([math]::Max(1, $AutopressSeconds) * 1000)"
        if ($AutopressMenu) { $env['SHADPS4_DBG_AUTOPRESS_MENU'] = '1' }
    }
    switch ($Arm) {
        'on' { $env[$ArmVariable] = '1' }
        'off' { $env[$ArmVariable] = '0' }
        'baseline' { }
    }
    foreach ($pair in @($ExtraEnv)) {
        if ([string]::IsNullOrWhiteSpace($pair)) { continue }
        $kv = $pair -split '=', 2
        if ($kv.Count -ne 2) { throw ("-Env entry '{0}' is not VAR=VALUE" -f $pair) }
        $env[$kv[0]] = $kv[1]
    }
    return $env
}

# --- log parsing ------------------------------------------------------------------------------
# One parser state per session: the log lines, the frame-stats samples, the per-second
# RENDERSTAT/ALIASSTAT counters (attached to the last sample seen, since they carry no time),
# and the HMD facts. The same code parses a live tail and an existing log in -SelfTest.
function New-ParseState {
    return [pscustomobject]@{
        lines      = New-Object System.Collections.Generic.List[string]
        samples    = New-Object System.Collections.Generic.List[object]
        renderstat = New-Object System.Collections.Generic.List[object]
        aliasstat  = New-Object System.Collections.Generic.List[object]
        hmd        = [ordered]@{
            hmd_lines           = 0
            eye_frames          = New-Object System.Collections.Generic.List[int]
            eyes_seen           = @{}
            image_width         = $null
            image_height        = $null
            eye_width           = $null
            eye_height          = $null
            format              = $null
            image_samples       = $null
            eye_image_addr      = $null
            vr_line             = $null
            presented_eye       = $null
            presented_both      = $null
            swapchain_images    = 0
            swapchain_w         = $null
            swapchain_h         = $null
            openxr_disabled     = $false
            headless_configured = $false
            update_worn_lines   = 0
            error_lines         = New-Object System.Collections.Generic.List[string]
            critical_errors     = New-Object System.Collections.Generic.List[string]
            input_steps         = 0
            live_updates        = 0
            rdoc_events         = New-Object System.Collections.Generic.List[string]
            dumps               = New-Object System.Collections.Generic.List[string]
            title_fov           = $null
            clean_exit_markers  = New-Object System.Collections.Generic.List[string]
            exit_after_markers  = New-Object System.Collections.Generic.List[string]
            autopress_lines     = 0
            build_info          = New-Object System.Collections.Generic.List[string]
            lines_total         = 0
        }
    }
}

function Add-ParseLines($State, [string[]]$NewLines) {
    $hmd = $State.hmd
    foreach ($line in $NewLines) {
        $State.lines.Add($line)
        $hmd.lines_total = $State.lines.Count
        $m = $script:StatsRegex.Match($line)
        if ($m.Success) {
            $State.samples.Add([pscustomobject]@{
                    index      = $State.samples.Count
                    at_s       = [double]$m.Groups['at'].Value
                    fps        = [double]$m.Groups['fps'].Value
                    passes     = [double]$m.Groups['passes'].Value
                    loading    = [double]$m.Groups['loading'].Value
                    mpx        = [double]$m.Groups['mpx'].Value
                    draws      = [double]$m.Groups['draws'].Value
                    dispatches = [double]$m.Groups['dispatches'].Value
                    submits    = [double]$m.Groups['submits'].Value
                    busy       = $(if ($m.Groups['busy'].Success) { [int]$m.Groups['busy'].Value } else { -1 })
                    mhz        = $(if ($m.Groups['mhz'].Success) { [int]$m.Groups['mhz'].Value } else { -1 })
                })
            continue
        }
        if ($line -match 'RENDERSTAT\s+(?<rest>.*)$') {
            $fields = @{}
            foreach ($token in ($Matches['rest'] -split ' ')) {
                $kv = $token.Split('=')
                if ($kv.Count -eq 2) { $fields[$kv[0]] = $kv[1] }
            }
            $State.renderstat.Add([pscustomobject]@{ sample_index = $State.samples.Count - 1; fields = $fields })
            continue
        }
        if ($line -match 'ALIASSTAT\s+(?<rest>.*)$') {
            $fields = @{}
            foreach ($token in ($Matches['rest'] -split ' ')) {
                $kv = $token.Split('=')
                if ($kv.Count -eq 2) { $fields[$kv[0]] = $kv[1] }
            }
            $State.aliasstat.Add([pscustomobject]@{ sample_index = $State.samples.Count - 1; fields = $fields })
            continue
        }
        $h = $script:HmdRegex.Match($line)
        if ($h.Success) {
            $hmd.hmd_lines++
            $hmd.eye_frames.Add([int]$h.Groups['frame'].Value)
            $hmd.eyes_seen[[int]$h.Groups['eye'].Value] = $true
            $hmd.image_width = [int]$h.Groups['w'].Value
            $hmd.image_height = [int]$h.Groups['h'].Value
            $hmd.eye_width = [int]([int]$h.Groups['w'].Value / 2)
            $hmd.eye_height = [int]$h.Groups['h'].Value
            $hmd.format = $h.Groups['fmt'].Value
            $hmd.image_samples = [int]$h.Groups['samples'].Value
            $hmd.eye_image_addr = $h.Groups['addr'].Value
            continue
        }
        $v = $script:VrRegex.Match($line)
        if ($v.Success) {
            $hmd.vr_line = $line
            $hmd.eye_width = [int]$v.Groups['w'].Value
            $hmd.eye_height = [int]$v.Groups['h'].Value
            $hmd.presented_eye = ('{0}x{1}' -f $v.Groups['pw'].Value, $v.Groups['ph'].Value)
            $hmd.presented_both = ('{0}x{1}' -f $v.Groups['sw'].Value, $v.Groups['sh'].Value)
            continue
        }
        $s = $script:SwapRegex.Match($line)
        if ($s.Success) {
            $hmd.swapchain_images = [int]$s.Groups['n'].Value
            $hmd.swapchain_w = [int]$s.Groups['w'].Value
            $hmd.swapchain_h = [int]$s.Groups['h'].Value
            continue
        }
        if ($line -match 'The headset of this machine is not looked for \(SHADPS4_OPENXR=0\)') {
            $hmd.openxr_disabled = $true
        }
        if ($line -match 'Configure: Virtual headset connected') { $hmd.headless_configured = $true }
        if ($line -match 'UpdateWorn') { $hmd.update_worn_lines++ }
        if ($line -match 'Replaying (?<n>\d+) input steps') { $hmd.input_steps = [int]$Matches['n'] }
        if ($line -match 'Live script now has (?<n>\d+) steps') { $hmd.live_updates++ }
        if ($line -match 'AUTOPRESS') { $hmd.autopress_lines++ }
        if ($line -match 'TitleFov:') { $hmd.title_fov = $line }
        if ($line -match 'CLEAN_EXIT|CleanExit|clean exit|exiting cleanly|Exiting cleanly') { $hmd.clean_exit_markers.Add($line) }
        if ($line -match 'SHADPS4_EXIT_AFTER') { $hmd.exit_after_markers.Add($line) }
        if ($line -match 'RDOC (capture triggered|scene gate met)') { $hmd.rdoc_events.Add($line) }
        if ($line -match 'DumpGpuImages: Dumped (?<n>\d+) render targets to (?<dir>.+)$') {
            $hmd.dumps.Add($Matches['dir'].Trim())
        }
        if ($line -match 'Starting shadps4 emulator|Revision |Branch |Description ') {
            $hmd.build_info.Add($line)
        }
        if ($line -match '<(Error|Critical)>') {
            $hmd.error_lines.Add($line)
            if ($line -match 'Device lost|DEVICE_LOST|Unreachable code|Assertion Failed|VK_ERROR_DEVICE_LOST') {
                $hmd.critical_errors.Add($line)
            }
        }
    }
}

function Get-ParseStateFromFile([string]$Path) {
    $state = New-ParseState
    foreach ($line in [IO.File]::ReadLines($Path)) {
        Add-ParseLines $state @($line)
    }
    return $state
}

# --- the scene gate ---------------------------------------------------------------------------
# The frame-stats window is the only per-session timing series both builds have. Each window is
# ~StatsEvery seconds of presented frames (a long stall can make one window longer), so windows
# are counted, not timed; `at_s` is the emulator's own monotonic session clock and is used only
# to place the measurement window in seconds.
function Get-SceneAnalysis {
    param($Parse, $Opt)
    $samples = $Parse.samples
    $result = [ordered]@{
        gate         = $Opt.Gate
        heavy_runs   = New-Object System.Collections.Generic.List[object]
        phases       = New-Object System.Collections.Generic.List[object]
        menu         = $null
        loading      = $null
        gameplay     = $null
        anchor_index = -1
        anchor_s     = $null
        phase        = 'none'
        reason       = ''
    }
    if ($samples.Count -eq 0) {
        $result.reason = 'no frame-stats samples in the log (SHADPS4_FRAME_STATS not set?)'
        return $result
    }

    $isHeavy = {
        param($s)
        return ($s.draws -ge $Opt.SceneMinDraws -and $s.mpx -ge $Opt.SceneMinMpx -and $s.fps -gt 1.0)
    }
    $isLight = {
        param($s)
        return ($s.draws -le $Opt.LoadingMaxDraws -or $s.mpx -le $Opt.LoadingMaxMpx)
    }

    # Contiguous runs of heavy windows.
    $run = $null
    $phases = New-Object System.Collections.Generic.List[object]
    $phaseKind = $null
    $phaseFirst = 0
    for ($i = 0; $i -lt $samples.Count; $i++) {
        $s = $samples[$i]
        $heavy = & $isHeavy $s
        $light = & $isLight $s
        $kind = if ($heavy) { 'heavy' } elseif ($light) { 'light' } else { 'transition' }
        if ($null -eq $phaseKind) { $phaseKind = $kind; $phaseFirst = $i }
        elseif ($kind -ne $phaseKind) {
            $phases.Add((New-PhaseRecord $samples $phaseKind $phaseFirst ($i - 1) $Opt))
            $phaseKind = $kind; $phaseFirst = $i
        }
        if ($heavy) {
            if ($null -eq $run) { $run = [pscustomobject]@{ first = $i; last = $i } }
            else { $run.last = $i }
        } elseif ($null -ne $run) {
            $result.heavy_runs.Add((New-PhaseRecord $samples 'heavy' $run.first $run.last $Opt))
            $run = $null
        }
    }
    if ($null -ne $phaseKind) { $phases.Add((New-PhaseRecord $samples $phaseKind $phaseFirst ($samples.Count - 1) $Opt)) }
    if ($null -ne $run) { $result.heavy_runs.Add((New-PhaseRecord $samples 'heavy' $run.first $run.last $Opt)) }
    $result.phases = $phases

    $qualified = @($result.heavy_runs | Where-Object { $_.count -ge $Opt.SceneSamples })
    if ($qualified.Count -gt 0) { $result.menu = $qualified[0] }

    switch ($Opt.Gate) {
        'menu' {
            if ($null -ne $result.menu) {
                $result.anchor_index = $result.menu.first_index
                $result.anchor_s = $result.menu.first_s
                $result.phase = 'menu'
                $result.reason = 'menu/front-end heavy phase matched (calibration gate; not gameplay)'
            } else {
                $result.reason = ("no {0} consecutive heavy windows with draws>={1} and Mpx>={2}" -f $Opt.SceneSamples, $Opt.SceneMinDraws, $Opt.SceneMinMpx)
            }
        }
        'heavy' {
            if ($null -ne $result.menu) {
                $result.anchor_index = $result.menu.first_index
                $result.anchor_s = $result.menu.first_s
                $result.phase = 'heavy'
                $result.reason = 'first heavy phase matched (calibration gate; no gameplay claim)'
            } else {
                $result.reason = ("no {0} consecutive heavy windows with draws>={1} and Mpx>={2}" -f $Opt.SceneSamples, $Opt.SceneMinDraws, $Opt.SceneMinMpx)
            }
        }
        'gameplay' {
            if ($null -eq $result.menu) {
                $result.reason = ("the menu/classroom heavy phase was never reached: no {0} consecutive windows with draws>={1} and Mpx>={2}" -f $Opt.SceneSamples, $Opt.SceneMinDraws, $Opt.SceneMinMpx)
                break
            }
            # The loading transition: after the menu leaves the screen, the title shows a light
            # window (or several) before the gameplay phase's heavy windows start.
            $dip = $null
            for ($i = $result.menu.last_index + 1; $i -lt $samples.Count; $i++) {
                if (& $isLight $samples[$i]) {
                    if ($null -eq $dip) { $dip = [pscustomobject]@{ first = $i; last = $i } }
                    else { $dip.last = $i }
                } elseif ($null -ne $dip) { break }
            }
            if ($null -eq $dip) {
                $result.reason = 'the menu phase was reached but no loading transition (light window) followed it; the title is still on the menu (Move-pointer driven)'
                break
            }
            $result.loading = New-PhaseRecord $samples 'light' $dip.first $dip.last $Opt
            $wanted = [math]::Max($Opt.SceneSamples, $Opt.SettleWindows)
            $candidates = @($result.heavy_runs | Where-Object {
                    $_.first_index -gt $result.loading.last_index -and $_.count -ge $wanted -and
                    $_.settled -and $_.draws_mean -ge $Opt.GameplayMinDraws
                })
            if ($candidates.Count -eq 0) {
                $result.reason = ("no settled heavy phase after the loading transition (need {0} windows, draws/Mpx spread <= {1} %)" -f $wanted, (2 * $Opt.SettleTolerancePct))
                break
            }
            $result.gameplay = $candidates[0]
            $result.anchor_index = $(if ($null -ne $result.gameplay.settle_index) { $result.gameplay.settle_index } else { $result.gameplay.first_index })
            $result.anchor_s = $Samples[$result.anchor_index].at_s
            $result.phase = 'gameplay'
            $result.reason = 'menu -> loading transition -> settled heavy phase matched'
        }
    }
    return $result
}

function New-PhaseRecord($Samples, [string]$Kind, [int]$First, [int]$Last, $Opt) {
    $window = @($Samples[$First..$Last])
    $draws = @($window | ForEach-Object { [double]$_.draws })
    $mpx = @($window | ForEach-Object { [double]$_.mpx })
    $passes = @($window | ForEach-Object { [double]$_.passes })
    $fps = @($window | ForEach-Object { [double]$_.fps })
    $busy = @($window | Where-Object { $_.busy -ge 0 } | ForEach-Object { [double]$_.busy })
    $drawsMean = Get-MeanOf $draws
    $mpxMean = Get-MeanOf $mpx
    $passesMean = Get-MeanOf $passes
    $inBand = $true
    if ($Opt.BandMinDraws -gt 0 -and $drawsMean -lt $Opt.BandMinDraws) { $inBand = $false }
    if ($Opt.BandMaxDraws -gt 0 -and $drawsMean -gt $Opt.BandMaxDraws) { $inBand = $false }
    if ($Opt.BandMinMpx -gt 0 -and $mpxMean -lt $Opt.BandMinMpx) { $inBand = $false }
    if ($Opt.BandMaxMpx -gt 0 -and $mpxMean -gt $Opt.BandMaxMpx) { $inBand = $false }
    if ($Opt.BandMinPasses -gt 0 -and $passesMean -lt $Opt.BandMinPasses) { $inBand = $false }
    if ($Opt.BandMaxPasses -gt 0 -and $passesMean -gt $Opt.BandMaxPasses) { $inBand = $false }

    # The settle check slides over the run: the first window after a loading transition can be a
    # spike, so "settled" means the first SettleWindows consecutive windows whose draws and Mpx
    # spreads are both within 2 x SettleTolerancePct. The anchor is the start of that segment, so
    # the warmup is discarded after the curve has settled, not from the run's first window.
    $settleIndex = $null
    $drawsSpread = 0.0
    $mpxSpread = 0.0
    $need = [math]::Max($Opt.SettleWindows, 1)
    if ($window.Count -ge $need) {
        for ($j = 0; $j + $need -le $window.Count; $j++) {
            $segD = @($window[$j..($j + $need - 1)] | ForEach-Object { [double]$_.draws })
            $segM = @($window[$j..($j + $need - 1)] | ForEach-Object { [double]$_.mpx })
            $dMean = Get-MeanOf $segD
            $mMean = Get-MeanOf $segM
            $dSpread = if ($null -ne $dMean -and $dMean -gt 0) { 100.0 * (($segD | Measure-Object -Maximum).Maximum - ($segD | Measure-Object -Minimum).Minimum) / $dMean } else { 0.0 }
            $mSpread = if ($null -ne $mMean -and $mMean -gt 0) { 100.0 * (($segM | Measure-Object -Maximum).Maximum - ($segM | Measure-Object -Minimum).Minimum) / $mMean } else { 0.0 }
            if ($dSpread -le (2 * $Opt.SettleTolerancePct) -and $mSpread -le (2 * $Opt.SettleTolerancePct)) {
                $settleIndex = $First + $j
                $drawsSpread = $dSpread
                $mpxSpread = $mSpread
                break
            }
        }
    }
    return [ordered]@{
        kind             = $Kind
        first_index      = $First
        last_index       = $Last
        count            = $window.Count
        first_s          = $Samples[$First].at_s
        last_s           = $Samples[$Last].at_s
        draws_mean       = $(if ($null -ne $drawsMean) { Round3 $drawsMean } else { $null })
        mpx_mean         = $(if ($null -ne $mpxMean) { Round3 $mpxMean } else { $null })
        passes_mean      = $(if ($null -ne $passesMean) { Round3 $passesMean } else { $null })
        fps_mean         = $(if ($fps.Count -gt 0) { Round3 (Get-MeanOf $fps) } else { $null })
        gpu_busy_mean    = $(if ($busy.Count -gt 0) { Round1 (Get-MeanOf $busy) } else { $null })
        draws_spread_pct = Round1 $drawsSpread
        mpx_spread_pct   = Round1 $mpxSpread
        settle_windows   = $need
        settle_index     = $settleIndex
        settle_s         = $(if ($null -ne $settleIndex) { $Samples[$settleIndex].at_s } else { $null })
        in_band          = $inBand
        settled          = ($null -ne $settleIndex)
    }
}

function Get-MeasuredWindow($Samples, [int]$AnchorIndex, [int]$Warmup, [int]$Seconds) {
    $anchorS = $Samples[$AnchorIndex].at_s
    $from = $anchorS + $Warmup
    $to = $from + $Seconds
    $measured = @($Samples | Where-Object { $_.at_s -ge $from -and $_.at_s -lt $to })
    return [ordered]@{
        from_s      = $from
        to_s        = $to
        anchor_s    = $anchorS
        count       = $measured.Count
        first_index = $(if ($measured.Count -gt 0) { $measured[0].index } else { -1 })
        last_index  = $(if ($measured.Count -gt 0) { $measured[-1].index } else { -1 })
        samples     = $measured
    }
}

function Test-Band($Measured, $Opt) {
    $draws = Get-MeanOf ($Measured.samples | ForEach-Object { $_.draws })
    $mpx = Get-MeanOf ($Measured.samples | ForEach-Object { $_.mpx })
    $passes = Get-MeanOf ($Measured.samples | ForEach-Object { $_.passes })
    $checks = [ordered]@{}
    $ok = $true
    $rule = {
        param($name, $value, $min, $max)
        $good = $true
        if ($min -gt 0 -and ($null -eq $value -or $value -lt $min)) { $good = $false }
        if ($max -gt 0 -and ($null -eq $value -or $value -gt $max)) { $good = $false }
        return [ordered]@{ value = $value; min = $min; max = $max; ok = $good }
    }
    $checks.draws = & $rule 'draws' $draws $Opt.BandMinDraws $Opt.BandMaxDraws
    $checks.mpx = & $rule 'mpx' $mpx $Opt.BandMinMpx $Opt.BandMaxMpx
    $checks.passes = & $rule 'passes' $passes $Opt.BandMinPasses $Opt.BandMaxPasses
    foreach ($k in $checks.Keys) { if (-not $checks[$k].ok) { $ok = $false } }
    return [ordered]@{
        ok         = $ok
        draws_mean = $(if ($null -ne $draws) { Round3 $draws } else { $null })
        mpx_mean   = $(if ($null -ne $mpx) { Round3 $mpx } else { $null })
        passes_mean = $(if ($null -ne $passes) { Round3 $passes } else { $null })
        checks     = $checks
        rule       = ("draws in [{0},{1}], Mpx in [{2},{3}], passes in [{4},{5}] (0 = unbounded)" -f $Opt.BandMinDraws, $Opt.BandMaxDraws, $Opt.BandMinMpx, $Opt.BandMaxMpx, $Opt.BandMinPasses, $Opt.BandMaxPasses)
    }
}

function Test-Hmd($Parse, $Opt) {
    $hmd = $Parse.hmd
    $actualEye = $null
    if ($null -ne $hmd.eye_width -and $null -ne $hmd.eye_height) { $actualEye = ('{0}x{1}' -f $hmd.eye_width, $hmd.eye_height) }
    $actualImage = $null
    if ($null -ne $hmd.image_width -and $null -ne $hmd.image_height) { $actualImage = ('{0}x{1}' -f $hmd.image_width, $hmd.image_height) }
    $checks = [ordered]@{
        eye_line   = ($hmd.hmd_lines -gt 0)
        both_eyes  = ($hmd.eyes_seen.ContainsKey(0) -and $hmd.eyes_seen.ContainsKey(1))
        eye_size   = ($actualEye -eq $Opt.ExpectEye)
        image_size = ($actualImage -eq $Opt.ExpectImage)
        format     = ($hmd.format -eq $Opt.ExpectFormat)
        samples    = ($hmd.image_samples -eq $Opt.ExpectSamples)
    }
    $ok = $true
    foreach ($k in $checks.Keys) { if (-not $checks[$k]) { $ok = $false } }
    $reason = ''
    if (-not $checks.eye_line) { $reason = 'no PrepareHmdFrame: HMD frame eye line in the log (both eyes must be rendered)' }
    elseif (-not $checks.both_eyes) { $reason = 'the HMD eye lines do not cover both eyes' }
    elseif (-not $checks.eye_size) { $reason = ("the title drew {0} an eye, expected {1}" -f $actualEye, $Opt.ExpectEye) }
    elseif (-not $checks.image_size) { $reason = ("the eye image is {0}, expected {1}" -f $actualImage, $Opt.ExpectImage) }
    elseif (-not $checks.format) { $reason = ("the eye image format is {0}, expected {1}" -f $hmd.format, $Opt.ExpectFormat) }
    elseif (-not $checks.samples) { $reason = ("the eye image has {0} sample(s), expected {1}" -f $hmd.image_samples, $Opt.ExpectSamples) }
    return [ordered]@{
        ok            = $ok
        reason        = $reason
        eye_width     = $hmd.eye_width
        eye_height    = $hmd.eye_height
        image         = $actualImage
        format        = $hmd.format
        samples       = $hmd.image_samples
        hmd_lines     = $hmd.hmd_lines
        presented_eye = $hmd.presented_eye
        presented_both = $hmd.presented_both
        title_fov     = $hmd.title_fov
        swapchain     = ('{0}x{1} x{2}' -f $hmd.swapchain_w, $hmd.swapchain_h, $hmd.swapchain_images)
        openxr_disabled = $hmd.openxr_disabled
        headless_configured = $hmd.headless_configured
        update_worn_lines = $hmd.update_worn_lines
        checks        = $checks
    }
}

# --- counters ---------------------------------------------------------------------------------
function Get-CounterSummary($Parse, $Measured, [double]$FpsMean) {
    $from = $Measured.first_index
    $to = $Measured.last_index
    $rs = @($Parse.renderstat | Where-Object { $_.sample_index -ge $from -and $_.sample_index -le $to })
    $as = @($Parse.aliasstat | Where-Object { $_.sample_index -ge $from -and $_.sample_index -le $to })
    $out = [ordered]@{
        renderstat_samples = $rs.Count
        aliasstat_samples  = $as.Count
        per_s = [ordered]@{}
        per_frame = [ordered]@{}
    }
    $names = 'images_created', 'images_destroyed', 'create_ms', 'destroy_ms', 'recycler_hits',
    'recycler_misses', 'pool_mb', 'vram_mb', 'refreshes', 'copies', 'depth_ms', 'refresh_ms'
    foreach ($name in $names) {
        $values = @($rs | ForEach-Object { $_.fields[$name] } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
        $out.per_s[$name] = $(if ($values.Count -gt 0) { Round3 (Get-MeanOf $values) } else { $null })
    }
    $aliasNames = 'depth_copies', 'expands', 'recreations'
    foreach ($name in $aliasNames) {
        $values = @($as | ForEach-Object { $_.fields[$name] } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
        $out.per_s[$name] = $(if ($values.Count -gt 0) { Round3 (Get-MeanOf $values) } else { $null })
    }
    $perFrame = {
        param($name)
        $v = $out.per_s[$name]
        if ($null -eq $v -or $FpsMean -le 0) { return $null }
        return Round3 ($v / $FpsMean)
    }
    foreach ($name in @('images_created', 'create_ms', 'destroy_ms', 'recycler_hits', 'recycler_misses', 'refreshes', 'depth_copies')) {
        $out.per_frame[$name] = & $perFrame $name
    }
    return $out
}

# --- captures ---------------------------------------------------------------------------------
# The dump is triggered by the file <UserDir>\dump_render_targets, which the presenter picks up
# within five frames, so a picture can be taken at a chosen scene point without any input. The
# dump writes every registered GPU image (PNGs capped at SHADPS4_DUMP_WIDTH) plus one log line
# per image; depth images are listed but not written.
function Invoke-RtDump {
    param([string]$UserDir, [string]$Name, [int]$TimeoutMs = 90000)
    $base = Join-Path $UserDir 'rt_dump'
    $flag = Join-Path $UserDir 'dump_render_targets'
    $before = @()
    if (Test-Path $base) { $before = @(Get-ChildItem -LiteralPath $base -Directory | ForEach-Object { $_.Name }) }
    Write-TextFile $flag "1`r`n"
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    $stable = 0
    $lastCount = -1
    while ((Get-Date) -lt $deadline) {
        $dirs = @()
        if (Test-Path $base) {
            $dirs = @(Get-ChildItem -LiteralPath $base -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object Name)
        }
        if ($dirs.Count -gt 0) {
            $dir = $dirs[-1]
            $pngs = @(Get-ChildItem -LiteralPath $dir.FullName -File -Filter '*.png' -ErrorAction SilentlyContinue)
            if ($pngs.Count -gt 0 -and $pngs.Count -eq $lastCount) { $stable++ } else { $stable = 0 }
            $lastCount = $pngs.Count
            if ($stable -ge 3) {
                return [pscustomobject]@{ name = $Name; dir = $dir.FullName; folder = $dir.Name; files = @($pngs | Sort-Object Name) }
            }
        }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

# The dumped eye picture: its file name carries the guest address and the *real* image size
# (<seq>_<addr>_<w>x<h>_<format>_s<n>.png), while the PNG itself is capped at DumpWidth.
function Find-EyePng($Dump, $Hmd) {
    if ($null -eq $Dump) { return $null }
    $candidates = @($Dump.files)
    if ($null -ne $Hmd.eye_image_addr) {
        $addr = ($Hmd.eye_image_addr -replace '^0x', '')
        $exact = @($candidates | Where-Object { $_.Name -match ('_' + [regex]::Escape($addr) + '_') })
        if ($exact.Count -gt 0) { $candidates = $exact }
    }
    $sizeTag = ('{0}x{1}' -f $Hmd.image_width, $Hmd.image_height)
    $sized = @($candidates | Where-Object { $_.Name -match [regex]::Escape($sizeTag) })
    if ($sized.Count -gt 0) { $candidates = $sized }
    if ($candidates.Count -eq 0) { return $null }
    # The dump writes an _alpha companion next to some images; the eye picture is the plain one.
    $plain = @($candidates | Where-Object { $_.Name -notmatch '_alpha\.png$' })
    if ($plain.Count -gt 0) { $candidates = $plain }
    return $candidates[0]
}

function Save-Capture($Dump, [string]$DestDir, [string]$RunDir, [string]$Phase, [bool]$BeforeWindow, $Hmd) {
    if ($null -eq $Dump) { return $null }
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
    $eye = Find-EyePng $Dump $Hmd
    $saved = New-Object System.Collections.Generic.List[object]
    foreach ($f in $Dump.files) {
        $isEye = ($null -ne $eye -and $f.FullName -eq $eye.FullName)
        $dest = Join-Path $DestDir (('{0}__{1}' -f $Dump.name, $f.Name))
        Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
        $size = Get-PngSize $dest
        $saved.Add([ordered]@{
                file          = ('frames/' + (Split-Path $dest -Leaf))
                source_name   = $f.Name
                eye_image     = $isEye
                width         = $(if ($size) { $size.width } else { $null })
                height        = $(if ($size) { $size.height } else { $null })
                bytes         = (Get-Item -LiteralPath $dest).Length
                sha256        = (Get-FileSha256 $dest)
            })
    }
    return [ordered]@{
        name          = $Dump.name
        phase         = $Phase
        before_window = $BeforeWindow
        dump_dir      = $Dump.dir
        eye_file      = $(if ($null -ne $eye) { 'frames/' + $Dump.name + '__' + $eye.Name } else { $null })
        images        = $saved
    }
}

function Invoke-ImageDiff([string]$MenuPng, [string]$GamePng) {
    if (-not (Test-Path $script:CaptureDiff)) { return $null }
    $python = (Get-Command python -ErrorAction SilentlyContinue)
    if ($null -eq $python) { return $null }
    $raw = & $python.Source $script:CaptureDiff $MenuPng $GamePng 2>&1
    $text = ($raw -join "`n")
    try { return ($text | ConvertFrom-Json) } catch { return $null }
}

# --- one cell ---------------------------------------------------------------------------------
$script:Opt = [ordered]@{}
function Set-Options {
    $script:Opt = [ordered]@{
        Gate              = $Gate
        SceneMinDraws     = $SceneMinDraws
        SceneMinMpx       = $SceneMinMpx
        GameplayMinDraws  = $GameplayMinDraws
        SceneSamples      = $SceneSamples
        BandMinDraws      = $BandMinDraws
        BandMaxDraws      = $BandMaxDraws
        BandMinMpx        = $BandMinMpx
        BandMaxMpx        = $BandMaxMpx
        BandMinPasses     = $BandMinPasses
        BandMaxPasses     = $BandMaxPasses
        LoadingMaxDraws   = $LoadingMaxDraws
        LoadingMaxMpx     = $LoadingMaxMpx
        SettleWindows     = $SettleWindows
        SettleTolerancePct = $SettleTolerancePct
        ExpectEye         = $ExpectEye
        ExpectImage       = $ExpectImage
        ExpectFormat      = $ExpectFormat
        ExpectSamples     = $ExpectSamples
    }
}

function Resolve-InputScript([string]$LabDir) {
    $src = $InputScript
    if ($src -eq '') {
        $defaultScript = if ($Gate -eq 'gameplay') { 'menu-continue-move.txt' } else { 'menu-continue.txt' }
        $src = Join-Path $script:InputsDir $defaultScript
    }
    elseif (-not [IO.Path]::IsPathRooted($src)) { $src = Join-Path $Root $src }
    if (-not (Test-Path $src)) { throw "input script not found: $src" }
    $dst = Join-Path $LabDir 'input-script.txt'
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $live = Join-Path $LabDir 'input-live.txt'
    # The live script is what the replay thread re-reads when its mtime changes. At the scene
    # anchor the harness rewrites it as a pose-only script, so nothing is pressed during the
    # measured window. The pose is taken from the input script (the last head=/pad= lines), so a
    # custom script keeps its own fixed pose; the verified headless pose is the fallback.
    $text = [IO.File]::ReadAllText($dst)
    $headToken = $null
    $padToken = $null
    foreach ($line in ($text -split "`r?`n")) {
        foreach ($token in ($line -split '\s+')) {
            if ($token.StartsWith('head=')) { $headToken = $token }
            elseif ($token.StartsWith('pad=')) { $padToken = $token }
        }
    }
    if (-not $headToken) { $headToken = 'head=0,0,0,0,0,0' }
    if (-not $padToken) { $padToken = 'pad=0,-0.17,-0.5,0,-45,0' }
    $freeze = "# Fixed pose, no buttons (written by the harness at the scene anchor).`r`n0 100000 $headToken $padToken`r`n"
    # The live file must NOT exist before the run: the replay thread replaces its whole step list
    # when the file's mtime changes, so a pose-only live file written at setup would silently
    # discard the script's Move-trigger presses (confirmed: "Live script now has 1 steps" and the
    # menu never advanced). It is created only at the scene anchor, when freezing is intended.
    Remove-Item -LiteralPath $live -Force -ErrorAction SilentlyContinue
    return [ordered]@{
        source = $src; lab_copy = $dst; live = $live; text = $text
        freeze_text = $freeze; head_pose = $headToken; pad_pose = $padToken
    }
}

function Read-NewLogLines {
    # Reads only what the emulator appended since the last call. The position is script state on
    # purpose: a scriptblock invoked with & gets its own scope, so an assignment inside it would
    # never update the caller and the whole log would be re-parsed on every poll.
    $path = $script:CellLogPath
    if (-not $path -or -not (Test-Path $path)) { return , @() }
    $fs = $null
    try { $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite') } catch { return , @() }
    try {
        if ($fs.Length -lt $script:CellLogPos) { $script:CellLogPos = 0 }
        [void]$fs.Seek($script:CellLogPos, 'Begin')
        $reader = New-Object System.IO.StreamReader($fs)
        $new = New-Object System.Collections.Generic.List[string]
        $line = $reader.ReadLine()
        while ($null -ne $line) { $new.Add($line); $line = $reader.ReadLine() }
        $script:CellLogPos = $fs.Position
        return , $new.ToArray()
    } finally { $fs.Dispose() }
}

# The gameplay load depends on which save the lab carries: slot 1 (MGS_GameData_1, stage
# m00_00_10_10) is what makes Continue load the classroom; with only slot 0 the title sits on the
# menu forever. The lab is always refreshed from the one known-good save, and the hashes are
# recorded so a cell can be traced to the exact save it ran with.
function Get-SaveDataInfo([string]$LabDir) {
    $saveRoot = Join-Path $LabDir 'user\home\1000\savedata\CUSA11919'
    $dataDir = Join-Path $saveRoot 'SaveData'
    $files = New-Object System.Collections.Generic.List[object]
    $slot1 = $false
    $system = $false
    $paramSfo = $null
    if (Test-Path $dataDir) {
        foreach ($f in @(Get-ChildItem -LiteralPath $dataDir -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
            $files.Add([ordered]@{
                    name = $f.Name; bytes = $f.Length; sha256 = (Get-FileSha256 $f.FullName)
                })
            if ($f.Name -eq 'MGS_GameData_1') { $slot1 = $true }
            if ($f.Name -eq 'MGS_SystemData') { $system = $true }
        }
        # sce_sys/param.sfo is part of the save directory: a SaveData with only the data files and
        # a foreign param.sfo did not load the classroom (Lead, 2026-11-18).
        $sceDir = Join-Path $dataDir 'sce_sys'
        if (Test-Path $sceDir) {
            foreach ($f in @(Get-ChildItem -LiteralPath $sceDir -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
                $entry = [ordered]@{
                    name = 'sce_sys/' + $f.Name; bytes = $f.Length; sha256 = (Get-FileSha256 $f.FullName)
                }
                $files.Add($entry)
                if ($f.Name -eq 'param.sfo') { $paramSfo = $entry }
            }
        }
    }
    return [ordered]@{
        lab_dir           = $saveRoot
        source            = $script:SavedataSource
        slot1_present     = $slot1
        system_present    = $system
        param_sfo_present = ($null -ne $paramSfo)
        param_sfo         = $paramSfo
        files             = $files.ToArray()
    }
}

function Get-ReferenceInfo([string]$Path) {
    if ($Path -eq '' -or -not (Test-Path $Path)) {
        return [ordered]@{ path = $Path; exists = $false; sha256 = $null; width = $null; height = $null }
    }
    $size = Get-PngSize $Path
    return [ordered]@{
        path   = (Resolve-Path -LiteralPath $Path).Path
        exists = $true
        sha256 = (Get-FileSha256 $Path)
        width  = $(if ($size) { $size.width } else { $null })
        height = $(if ($size) { $size.height } else { $null })
    }
}

function Invoke-CellSession {
    param(
        [string]$CellLabel, [string]$CellArm, [string]$CellMode,
        [string]$ExePath, [string]$ExeHash, [int]$Percent, [int]$RefreshHz
    )
    $runDir = Join-Path $script:RunsRoot $CellLabel
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null
    $labDir = if ($Lab -ne '') { $Lab } else { Join-Path $script:LabsRoot $Label }
    [void](Initialize-Lab $labDir)
    $labExe = Join-Path $labDir 'shadps4.exe'
    Copy-Item -LiteralPath $ExePath -Destination $labExe -Force
    if ((Get-FileSha256 $labExe) -ne $ExeHash) { throw "the executable was not copied into the lab correctly: $labExe" }
    $logPath = Join-Path $labDir 'user\log\shad_log.txt'
    $script:CellLogPath = $logPath
    $script:CellLogPos = 0
    $saveInfo = Get-SaveDataInfo $labDir
    if (-not $saveInfo.slot1_present) {
        throw ("the lab's save has no MGS_GameData_1 (stage m00_00_10_10): the menu cannot Continue " +
            "and the gameplay gate can never match. Source: {0}" -f $script:SavedataSource)
    }
    if (-not $saveInfo.param_sfo_present) {
        throw ("the lab's SaveData has no sce_sys\param.sfo: a SaveData without it (or with a foreign " +
            "one) did not load the classroom. Source: {0}" -f $script:SavedataSource)
    }

    $inputInfo = Resolve-InputScript $labDir
    $useAutopress = if ($NoAutopress) { $false } elseif ($Autopress) { $true } else { $Gate -eq 'gameplay' }
    $useSyntheticHands = if ($SyntheticHands -eq 'on') { $true } elseif ($SyntheticHands -eq 'off') { $false } else { $Gate -eq 'gameplay' }
    $envMap = Get-BenchEnvironment -Percent $Percent -RefreshHz $RefreshHz -Every $StatsEvery -Arm $CellArm `
        -InputScriptPath $inputInfo.lab_copy -LiveInputPath $inputInfo.live -DumpFrames $DumpFrames `
        -DumpWidth $DumpWidth -ExitAfterSeconds $ExitAfterSeconds -MaxSeconds $MaxSeconds -ArmVariable $ArmVariable `
        -FovOf $FovOf -Autopress $useAutopress -AutopressSeconds $AutopressSeconds -AutopressMenu ([bool]$AutopressMenu) `
        -CleanExit ([bool]$CleanExit) -HmdDumpAtScene $HmdDumpAtScene `
        -SyntheticHands $useSyntheticHands `
        -ExtraEnv (($Env | ForEach-Object { $_ -split ';' }) | Where-Object { $_ -ne '' })
    Set-LabConfig $labDir $RefreshHz
    $settingsText = Get-PinnedSettingsText $Percent $RefreshHz $CellArm $envMap
    Write-TextFile (Join-Path $labDir 'settings.txt') $settingsText
    Write-JsonFile (Join-Path $runDir 'env.json') ([ordered]@{ env = $envMap; settings_txt = 'settings.txt'; lab = $labDir })

    # The stored scene references are recorded for every cell, whether or not it captures.
    $menuRef = Get-ReferenceInfo $MenuReference
    $gameRef = Get-ReferenceInfo $GameplayReference

    Save-ShadowIni $runDir
    Write-ShadowIni $Percent
    # A killed wrapper does not kill its Windows child. Only this lab's own executable is ever
    # stopped here; another lab's leftover is recorded and left alone.
    $leftovers = @(Get-Process -Name 'shadps4' -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq (Join-Path $Lab 'shadps4.exe') } catch { $false } } | Where-Object {
            try { $_.Path -eq $labExe } catch { $false }
        })
    foreach ($p in $leftovers) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch {} }
    if ($leftovers.Count -gt 0) {
        Start-Sleep -Milliseconds 1200
        Write-Warning ("stopped {0} leftover emulator process(es) of this lab" -f $leftovers.Count)
    }
    $auditBefore = Get-ProcessAudit
    $othersBefore = Get-OtherEmulators $labExe
    $gpuBefore = Get-GpuState
    if ($auditBefore.Count -gt 0) { Write-Warning ('another benchmark process is running: ' + ($auditBefore -join ', ')) }
    if ($othersBefore.Count -gt 0) { Write-Warning ('another emulator is running: ' + ($othersBefore -join ', ')) }

    $gameArgs = @('-g', $Eboot)
    $patchFile = Join-Path $labDir 'user\patches\CUSA11919.xml'
    if (Test-Path $patchFile) { $gameArgs += @('-p', $patchFile) }
    # Optional switches must not leak from the previous cell of an A/B/A.
    foreach ($key in @('SHADPS4_DBG_DUMP_HMD_FRAMES', 'SHADPS4_EXIT_AFTER', 'SHADPS4_DBG_RDOC_CAPTURE',
            'SHADPS4_DBG_RDOC_CAPTURE_SCENE', $ArmVariable)) {
        Remove-Item -Path ('Env:' + $key) -ErrorAction SilentlyContinue
    }
    foreach ($key in $envMap.Keys) { Set-Item -Path ('Env:' + $key) -Value $envMap[$key] }

    $parse = New-ParseState
    $captureOnly = ($HmdDumpAtScene -ne '')
    $dumpsBefore = @()
    if ($captureOnly -and (Test-Path (Join-Path $labDir 'user'))) {
        $dumpsBefore = @(Get-ChildItem -LiteralPath (Join-Path $labDir 'user') -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    }
    $emu = $null
    $startedAt = Get-Date
    $sceneAnchorLive = -1
    $menuCapture = $null
    $gameplayCapture = $null
    $imageGate = $null
    $triedCandidates = @{}
    $liveNotes = New-Object System.Collections.Generic.List[string]
    $endedEarly = $null
    $stopReason = $null
    $fatalError = $null
    $captures = New-Object System.Collections.Generic.List[object]
    $launchCommand = ''
    $shutdown = [ordered]@{ exit_code = $null; seconds = $null; ok = $false; method = ''; reason = '' }

    try {
        if ($CellMode -eq 'rdoc') {
            if (-not (Test-Path $script:RenderDocCmd)) { throw "renderdoccmd not found: $($script:RenderDocCmd)" }
            $rdocArgs = @('capture', '-w', '-d', $labDir, $labExe, '-g', $Eboot)
            if (Test-Path $patchFile) { $rdocArgs += @('-p', $patchFile) }
            $env:SHADPS4_DBG_RDOC_CAPTURE_SCENE = $RdocScene
            if ($RdocSeconds -gt 0) { $env:SHADPS4_DBG_RDOC_CAPTURE = "$RdocSeconds" }
            $launchCommand = 'renderdoccmd ' + ($rdocArgs -join ' ')
            $emu = Start-Process -FilePath $script:RenderDocCmd -ArgumentList $rdocArgs -WorkingDirectory $labDir -PassThru `
                -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runDir 'console.txt') `
                -RedirectStandardError (Join-Path $runDir 'console-errors.txt')
            Write-Host ("launched under RenderDoc: renderdoccmd pid {0}" -f $emu.Id)
        } else {
            $launchCommand = $labExe + ' ' + ($gameArgs -join ' ')
            $emu = Start-Process -FilePath $labExe -ArgumentList $gameArgs -WorkingDirectory $labDir -PassThru `
                -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runDir 'console.txt') `
                -RedirectStandardError (Join-Path $runDir 'console-errors.txt')
            Write-Host ("launched: pid {0}  {1}" -f $emu.Id, $launchCommand)
            # A hard-killed harness (tool timeout, SIGKILL) cannot run its finally. The pid file
            # lets the next harness start stop exactly the emulator it started, even for another
            # label; it is removed on a clean stop.
            Write-JsonFile (Join-Path $runDir 'emulator.pid') ([ordered]@{
                    pid = $emu.Id; exe = $labExe; label = $CellLabel; started = (Get-Date).ToString('o')
                })
        }

        $deadline = $startedAt.AddSeconds($MaxSeconds)
        $captureDir = Join-Path $labDir 'user\captures'
        $rdocBefore = @()
        if ($CellMode -eq 'rdoc' -and (Test-Path $captureDir)) {
            $rdocBefore = @(Get-ChildItem -LiteralPath $captureDir -File -Filter '*.rdc' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
        }

        while ($true) {
            $newLines = Read-NewLogLines
            if ($newLines.Count -gt 0) { Add-ParseLines $parse $newLines }
            Send-LockHeartbeat

            if ($CellMode -eq 'rdoc') {
                # renderdoccmd is the parent; the emulator is its child and writes the capture.
                if ((Get-Date) -gt $deadline) { $stopReason = 'timeout'; break }
                if (Test-Path $captureDir) {
                    $newRdc = @(Get-ChildItem -LiteralPath $captureDir -File -Filter '*.rdc' -ErrorAction SilentlyContinue |
                        Where-Object { $rdocBefore -notcontains $_.FullName })
                    if ($newRdc.Count -gt 0) {
                        $rdc = $newRdc[-1]
                        Write-Host ("RenderDoc capture appeared: {0} ({1} bytes)" -f $rdc.Name, $rdc.Length)
                        # Let RenderDoc finish writing before the file is copied or the process stopped.
                        Start-Sleep -Seconds 4
                        $rdocDir = Join-Path $runDir 'rdoc'
                        New-Item -ItemType Directory -Path $rdocDir -Force | Out-Null
                        $dest = Join-Path $rdocDir $rdc.Name
                        Copy-Item -LiteralPath $rdc.FullName -Destination $dest -Force
                        $captures.Add([ordered]@{
                                name = $rdc.Name; kind = 'renderdoc'; file = 'rdoc/' + $rdc.Name
                                bytes = $rdc.Length; sha256 = (Get-FileSha256 $dest); before_window = $true
                            })
                        $stopReason = 'capture'
                        break
                    }
                }
                if ($emu.HasExited) { $endedEarly = 'renderdoccmd ended before a capture appeared'; break }
                Start-Sleep -Milliseconds 500
                continue
            }

            if ($emu.HasExited) {
                # A build with SHADPS4_CLEAN_EXIT / SHADPS4_EXIT_AFTER ends by itself; that is not
                # an early end when the measured window (or the capture) is already complete.
                $complete = $false
                if ($parse.samples.Count -gt 0) {
                    if ($captureOnly) {
                        $complete = @(Get-ChildItem -LiteralPath (Join-Path $labDir 'user') -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
                            Where-Object { $dumpsBefore -notcontains $_.Name }).Count -gt 0
                    } elseif ($sceneAnchorLive -ge 0) {
                        $anchorAt = $parse.samples[$sceneAnchorLive].at_s
                        $complete = ($parse.samples[-1].at_s -ge ($anchorAt + $Warmup + $Seconds))
                    }
                }
                if ($complete) { $stopReason = $(if ($captureOnly) { 'capture' } else { 'window-complete' }); break }
                $endedEarly = ("the emulator ended on its own (exit {0})" -f $emu.ExitCode)
                break
            }
            if ((Get-Date) -gt $deadline) { $stopReason = 'timeout'; break }

            $analysis = Get-SceneAnalysis $parse $script:Opt
            if ($sceneAnchorLive -lt 0) {
                if ($Gate -eq 'gameplay' -and $ImageGate -eq 'on') {
                    if ($null -ne $analysis.menu -and $null -eq $menuCapture) {
                        Write-Host ("menu phase at {0:N1} s: {1} draws, {2} Mpx, {3} passes - capturing the baseline" -f `
                                $analysis.menu.first_s, $analysis.menu.draws_mean, $analysis.menu.mpx_mean, $analysis.menu.passes_mean)
                        $dump = Invoke-RtDump -UserDir (Join-Path $labDir 'user') -Name 'menu-baseline'
                        if ($null -ne $dump) {
                            $menuCapture = Save-Capture $dump (Join-Path $runDir 'frames') $runDir 'menu' $true $parse.hmd
                            if ($null -ne $menuCapture) {
                                $captures.Add($menuCapture)
                                if ($menuRef.exists -and $menuCapture.eye_file) {
                                    $menuPng = Join-Path $runDir ($menuCapture.eye_file -replace '/', '\')
                                    $md = Invoke-ImageDiff $menuRef.path $menuPng
                                    if ($null -ne $md -and $null -ne $md.ssim_global) {
                                        $menuCapture.ssim_vs_reference = [math]::Round([double]$md.ssim_global * 100.0, 2)
                                    }
                                }
                                Write-Host ("menu baseline captured: {0}" -f $menuCapture.eye_file)
                            }
                        } else {
                            $liveNotes.Add('the menu baseline dump did not appear')
                        }
                    }
                    if ($null -ne $analysis.gameplay -and -not $triedCandidates.ContainsKey($analysis.gameplay.first_index)) {
                        $triedCandidates[$analysis.gameplay.first_index] = $true
                        Write-Host ("gameplay candidate at {0:N1} s: {1} draws, {2} Mpx, {3} passes - capturing and comparing" -f `
                                $analysis.gameplay.first_s, $analysis.gameplay.draws_mean, $analysis.gameplay.mpx_mean, $analysis.gameplay.passes_mean)
                        $dump = Invoke-RtDump -UserDir (Join-Path $labDir 'user') -Name ('gameplay-candidate-{0}' -f $analysis.gameplay.first_index)
                        if ($null -ne $dump) {
                            $gameplayCapture = Save-Capture $dump (Join-Path $runDir 'frames') $runDir 'gameplay' $true $parse.hmd
                            if ($null -ne $gameplayCapture) { $captures.Add($gameplayCapture) }
                            $menuPng = $null
                            if ($null -ne $menuCapture -and $menuCapture.eye_file) { $menuPng = Join-Path $runDir ($menuCapture.eye_file -replace '/', '\') }
                            $gamePng = $null
                            if ($null -ne $gameplayCapture -and $gameplayCapture.eye_file) { $gamePng = Join-Path $runDir ($gameplayCapture.eye_file -replace '/', '\') }
                            if ($null -ne $menuPng -and $null -ne $gamePng) {
                                $diff = Invoke-ImageDiff $menuPng $gamePng
                                $ssimVsMenu = $null
                                if ($null -ne $diff -and $null -ne $diff.ssim_global) { $ssimVsMenu = [math]::Round([double]$diff.ssim_global * 100.0, 2) }
                                $differsFromMenu = ($null -ne $ssimVsMenu -and $ssimVsMenu -le $ImageDiffMaxSsim)
                                if ($null -ne $diff -and $null -ne $diff.masked_hash -and -not $diff.masked_hash.equal) { $differsFromMenu = $true }
                                $ssimVsGame = $null
                                if ($gameRef.exists -and $gameRef.path -ne $gamePng) {
                                    $gd = Invoke-ImageDiff $gameRef.path $gamePng
                                    if ($null -ne $gd -and $null -ne $gd.ssim_global) { $ssimVsGame = [math]::Round([double]$gd.ssim_global * 100.0, 2) }
                                }
                                $matchesGameRef = $null
                                if ($gameRef.exists) { $matchesGameRef = ($null -ne $ssimVsGame -and $ssimVsGame -ge $GameplayMinSsim) }
                                $ok = $differsFromMenu -and ($null -eq $matchesGameRef -or $matchesGameRef)
                                $imageGate = [ordered]@{
                                    menu_reference       = $menuRef
                                    gameplay_reference   = $gameRef
                                    menu_file            = $menuCapture.eye_file
                                    menu_sha256          = $(if ($null -ne $menuCapture -and $menuCapture.eye_file) { Get-FileSha256 (Join-Path $runDir ($menuCapture.eye_file -replace '/', '\')) } else { $null })
                                    menu_ssim_vs_reference = $(if ($null -ne $menuCapture -and $menuCapture.Contains('ssim_vs_reference')) { $menuCapture.ssim_vs_reference } else { $null })
                                    gameplay_file        = $gameplayCapture.eye_file
                                    gameplay_sha256      = $(if ($null -ne $gameplayCapture -and $gameplayCapture.eye_file) { Get-FileSha256 (Join-Path $runDir ($gameplayCapture.eye_file -replace '/', '\')) } else { $null })
                                    ssim_vs_menu_pct     = $ssimVsMenu
                                    ssim_vs_gameplay_pct = $ssimVsGame
                                    max_ssim_vs_menu_pct = $ImageDiffMaxSsim
                                    min_ssim_vs_gameplay_pct = $GameplayMinSsim
                                    differs_from_menu    = $differsFromMenu
                                    matches_gameplay_reference = $matchesGameRef
                                    verdict              = $(if ($null -ne $diff) { $diff.verdict } else { $null })
                                    ok                   = $ok
                                }
                                if ($ok -and $SetGameplayReference -and $null -ne $gamePng) {
                                    New-Item -ItemType Directory -Path $script:ReferencesDir -Force | Out-Null
                                    Copy-Item -LiteralPath $gamePng -Destination $GameplayReference -Force
                                    $imageGate.gameplay_reference_written = (Resolve-Path -LiteralPath $GameplayReference).Path
                                    $imageGate.gameplay_reference = Get-ReferenceInfo $GameplayReference
                                    $liveNotes.Add('the accepted gameplay eye picture was stored as the gameplay reference')
                                    Write-Host ("gameplay reference written: {0}" -f $GameplayReference)
                                }
                                if ($ok) {
                                    $sceneAnchorLive = $analysis.gameplay.first_index
                                    Write-Host ("gameplay gate matched: SSIM vs menu {0} %, vs gameplay reference {1} %" -f $ssimVsMenu, $ssimVsGame)
                                } else {
                                    Write-Host ("candidate rejected: SSIM vs menu {0} % (must be <= {1}), vs gameplay reference {2} % (must be >= {3})" -f `
                                            $ssimVsMenu, $ImageDiffMaxSsim, $ssimVsGame, $GameplayMinSsim)
                                }
                            } else {
                                $imageGate = [ordered]@{ ok = $false; reason = 'no eye PNG in the menu or gameplay dump'; ssim_vs_menu_pct = $null; menu_reference = $menuRef; gameplay_reference = $gameRef }
                            }
                        } else {
                            $liveNotes.Add('the gameplay candidate dump did not appear')
                        }
                    }
                } else {
                    # Without the image gate, the scene analysis alone decides: a gameplay cell
                    # anchors on the settled heavy phase after the loading transition, a menu/
                    # heavy calibration cell anchors on the first heavy phase.
                    $anchor = $null
                    if ($Gate -eq 'gameplay') {
                        if ($null -ne $analysis.gameplay) { $anchor = $analysis.gameplay }
                    } elseif ($null -ne $analysis.menu) {
                        $anchor = $analysis.menu
                    } elseif ($null -ne $analysis.gameplay) {
                        $anchor = $analysis.gameplay
                    }
                    if ($null -ne $anchor) {
                        $sceneAnchorLive = $anchor.first_index
                        Write-Host ("scene gate matched ({0}) at {1:N1} s: {2} draws, {3} Mpx" -f $analysis.phase, $anchor.first_s, $anchor.draws_mean, $anchor.mpx_mean)
                    }
                }
                if ($sceneAnchorLive -ge 0) {
                    # Freeze the input for the measured window: the live script replaces the whole
                    # step list, so a pose-only script stops every button press at once.
                    Write-TextFile $inputInfo.live $inputInfo.freeze_text
                    $liveNotes.Add(('input frozen at the scene anchor ({0:HH:mm:ss})' -f (Get-Date)))
                }
            } else {
                if ($captureOnly) {
                    # Capture-only cell: wait for the scene-anchored HMD dump the build writes when
                    # its frame-stats window matches SHADPS4_DBG_DUMP_HMD_AT_SCENE, then stop.
                    $newDumps = @(Get-ChildItem -LiteralPath (Join-Path $labDir 'user') -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
                        Where-Object { $dumpsBefore -notcontains $_.Name })
                    if ($newDumps.Count -gt 0) { $stopReason = 'capture'; break }
                } else {
                    $anchorAt = $parse.samples[$sceneAnchorLive].at_s
                    $need = $anchorAt + $Warmup + $Seconds + 1
                    if ($parse.samples.Count -gt 0 -and $parse.samples[-1].at_s -ge $need) { $stopReason = 'window-complete'; break }
                }
            }
            Start-Sleep -Milliseconds 250
        }

        # Capture mode: one dump after the measured window, before the emulator is stopped, so
        # readbacks never pay for a frame the result quotes. It is marked outside the window.
        if ($Capture -and -not $captureOnly -and $CellMode -ne 'rdoc') {
            $liveAnalysis = Get-SceneAnalysis $parse $script:Opt
            $captureName = if ($liveAnalysis.phase -eq 'gameplay') { 'gameplay-postwindow' } else { 'postwindow' }
            $dump = Invoke-RtDump -UserDir (Join-Path $labDir 'user') -Name $captureName
            if ($null -ne $dump) {
                $cap = Save-Capture $dump (Join-Path $runDir 'frames') $runDir $liveAnalysis.phase $false $parse.hmd
                if ($null -ne $cap) { $captures.Add($cap) }
            } else {
                $liveNotes.Add('the post-window dump did not appear')
            }
        }
    } catch {
        $fatalError = $_
        Write-Warning ("the session failed: {0}" -f $_)
    } finally {
        # Stop only what this session started. The emulator is stopped the way bench.ps1 stops its
        # lab emulator (Stop-Process -Force): a hard stop, recorded as such; the log is flushed by
        # Log.sync=true, so its tail survives.
        $stopAt = Get-Date
        try {
            # A clean-exit cell must be allowed to quit by itself: SHADPS4_EXIT_AFTER fires on
            # the emulator's clock, which is after the measured window. Waiting here is what
            # makes C an actual clean-shutdown test instead of a hard stop.
            if ($CleanExit -and $ExitAfterSeconds -gt 0 -and $null -ne $emu -and -not $emu.HasExited) {
                try {
                    $selfExitBy = $emu.StartTime.AddSeconds($ExitAfterSeconds + 30)
                    $waitMs = [int][Math]::Max(0, ($selfExitBy - (Get-Date)).TotalMilliseconds)
                    if ($waitMs -gt 0) { [void]$emu.WaitForExit($waitMs) }
                } catch {}
            }
            if ($CellMode -eq 'rdoc') {
                $emus = @(Get-Process -Name 'shadps4' -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq (Join-Path $Lab 'shadps4.exe') } catch { $false } } | Where-Object {
                        try { $_.Path -eq $labExe } catch { $false }
                    })
                foreach ($p in $emus) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch {} }
                $shutdown.method = 'Stop-Process -Force on the emulator child (bench.ps1 mechanism)'
            } elseif ($null -ne $emu -and -not $emu.HasExited) {
                Stop-Process -Id $emu.Id -Force -ErrorAction SilentlyContinue
                $shutdown.method = 'Stop-Process -Force (bench.ps1 mechanism)'
            } elseif ($null -ne $emu) {
                $shutdown.method = 'the emulator exited on its own'
                $shutdown.ok = $true
            }
            if ($null -ne $emu) {
                try { [void]$emu.WaitForExit(20000) } catch {}
                try { $shutdown.exit_code = $emu.ExitCode } catch {}
            }
            if ($CellMode -eq 'rdoc' -and $null -ne $emu -and -not $emu.HasExited) {
                Start-Sleep -Seconds 2
                try { Stop-Process -Id $emu.Id -Force -ErrorAction SilentlyContinue } catch {}
            }
            $shutdown.seconds = Round3 ((Get-Date) - $stopAt).TotalSeconds
            if ($shutdown.method -like 'Stop-Process*') {
                $shutdown.ok = $false
                $shutdown.reason = 'hard stop: this build has no clean-exit path in use (Log.sync=true keeps the log tail)'
            }
            Start-Sleep -Milliseconds 1200
        } catch { Write-Warning ("stopping the emulator failed: {0}" -f $_) }
        $script:IniWasWritten = $script:IniWritten
        Restore-ShadowIni
        Remove-Item -LiteralPath (Join-Path $runDir 'emulator.pid') -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $labDir 'user\dump_render_targets') -Force -ErrorAction SilentlyContinue
        # Last read after the stop: the process wrote its tail while exiting.
        try { $tail = Read-NewLogLines; if ($tail.Count -gt 0) { Add-ParseLines $parse $tail } } catch {}
        if (Test-Path $logPath) { Copy-Item -LiteralPath $logPath -Destination (Join-Path $runDir 'shad_log.txt') -Force -ErrorAction SilentlyContinue }
        $auditAfter = Get-ProcessAudit
        $othersAfter = Get-OtherEmulators $labExe
        $gpuAfter = Get-GpuState
        $endedAt = Get-Date
    }

    # --- final, offline analysis of the complete log ---------------------------------------
    # The live loop only decides when to stop; this pass is the evidence and the validity.
    $analysis = Get-SceneAnalysis $parse $script:Opt
    $hmd = Test-Hmd $parse $script:Opt
    $measured = $null
    $band = $null
    $counters = $null
    $fpsStats = $null
    if (-not $captureOnly -and $analysis.anchor_index -ge 0 -and $analysis.anchor_index -lt $parse.samples.Count) {
        $measured = Get-MeasuredWindow $parse.samples $analysis.anchor_index $Warmup $Seconds
        if ($measured.count -gt 0) {
            $band = Test-Band $measured $script:Opt
            $fpsStats = Get-Stats @($measured.samples | ForEach-Object { [double]$_.fps })
            $counters = Get-CounterSummary $parse $measured ([double]$fpsStats.mean)
        }
    }

    # Capture-only cell (-HmdDumpAtScene): the build dumped the eye picture at the scene gate.
    # There is deliberately no timed window here: the dump stalls frames.
    if ($captureOnly) {
        # The dump directory layout is <user>/rt_dump[-<frame>]/capture_NN/*.png (the emulator
        # numbers its dumps inside the folder it was given), so the PNGs are one level down.
        $newDumps = @(Get-ChildItem -LiteralPath (Join-Path $labDir 'user') -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
            Where-Object { $dumpsBefore -notcontains $_.Name } | Sort-Object Name)
        foreach ($dir in $newDumps) {
            $capDirs = New-Object System.Collections.Generic.List[object]
            $direct = @(Get-ChildItem -LiteralPath $dir.FullName -File -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
            if ($direct.Count -gt 0) {
                $capDirs.Add([pscustomobject]@{ name = $dir.Name; dir = $dir.FullName; folder = $dir.Name; files = $direct })
            } else {
                foreach ($cd in @(Get-ChildItem -LiteralPath $dir.FullName -Directory -Filter 'capture_*' -ErrorAction SilentlyContinue | Sort-Object Name)) {
                    $pngs = @(Get-ChildItem -LiteralPath $cd.FullName -File -Filter '*.png' -ErrorAction SilentlyContinue | Sort-Object Name)
                    if ($pngs.Count -gt 0) {
                        $capDirs.Add([pscustomobject]@{ name = ($dir.Name + '-' + $cd.Name); dir = $cd.FullName; folder = $cd.Name; files = $pngs })
                    }
                }
            }
            foreach ($dump in $capDirs) {
                $cap = Save-Capture $dump (Join-Path $runDir 'frames') $runDir $analysis.phase $true $parse.hmd
                if ($null -ne $cap) { $cap.scene_anchored = $true; $captures.Add($cap) }
            }
        }
    }

    # Every cell records the menu baseline it was compared against (or would have been).
    if ($null -eq $imageGate) {
        $imageGate = [ordered]@{
            menu_reference = $menuRef; gameplay_reference = $gameRef
            menu_file = $null; menu_sha256 = $null
            gameplay_file = $null; gameplay_sha256 = $null
            ssim_vs_menu_pct = $null; ssim_vs_gameplay_pct = $null
            ok = $null; reason = $(if ($Gate -eq 'gameplay' -and $ImageGate -eq 'on') { 'the gameplay gate did not run' } else { 'image gate not used for this gate/mode' })
        }
    } else {
        $imageGate.menu_reference = $menuRef
        if ($null -ne $menuCapture -and $menuCapture.eye_file -and -not $imageGate.Contains('menu_file')) {
            $imageGate.menu_file = $menuCapture.eye_file
            $imageGate.menu_sha256 = Get-FileSha256 (Join-Path $runDir ($menuCapture.eye_file -replace '/', '\'))
        }
        $imageGate.gameplay_reference = $gameRef
    }

    $valid = $true
    $reasons = New-Object System.Collections.Generic.List[string]
    if ($fatalError) { $valid = $false; $reasons.Add(('the session failed: ' + $fatalError)) }
    if ($parse.samples.Count -eq 0) { $valid = $false; $reasons.Add('no frame-stats samples in the log') }
    if ($analysis.anchor_index -lt 0) { $valid = $false; $reasons.Add($analysis.reason) }
    if (-not $hmd.ok) { $valid = $false; $reasons.Add($hmd.reason) }
    if (-not $captureOnly -and ($null -eq $measured -or $measured.count -lt 5)) {
        $valid = $false
        $reasons.Add(("the measured window has {0} sample(s); at least 5 are needed" -f $(if ($null -ne $measured) { $measured.count } else { 0 })))
    }
    if ($captureOnly -and $captures.Count -eq 0) {
        $valid = $false
        $reasons.Add('no scene-anchored HMD dump appeared (SHADPS4_DBG_DUMP_HMD_AT_SCENE)')
    }
    if ($null -ne $band -and -not $band.ok) {
        $valid = $false
        $reasons.Add(("scene band mismatch: draws {0} (band {1}..{2}), Mpx {3} (band {4}..{5}), passes {6}" -f `
                    $band.draws_mean, $BandMinDraws, $BandMaxDraws, $band.mpx_mean, $BandMinMpx, $BandMaxMpx, $band.passes_mean))
    }
    if ($parse.hmd.critical_errors.Count -gt 0) { $valid = $false; $reasons.Add(('critical error in the log: ' + $parse.hmd.critical_errors[0])) }
    if ($othersBefore.Count -gt 0 -or $othersAfter.Count -gt 0) { $valid = $false; $reasons.Add('another emulator was running (timing is not comparable)') }
    if ($auditBefore.Count -gt 0 -or $auditAfter.Count -gt 0) { $reasons.Add('another benchmark process was seen; see host.quiet') }
    if ($endedEarly) { $valid = $false; $reasons.Add($endedEarly) }
    if ($stopReason -eq 'timeout' -and $analysis.anchor_index -lt 0) { $reasons.Add(("the session timed out after {0} s before the gate matched" -f $MaxSeconds)) }
    if ($Gate -eq 'gameplay' -and $ImageGate -eq 'on' -and $CellMode -ne 'rdoc') {
        if ($null -eq $imageGate -or $null -eq $imageGate.ok) {
            $valid = $false
            $reasons.Add('the gameplay gate was never reached, so the eye picture could not be compared with the menu baseline')
        } elseif (-not $imageGate.ok) {
            $valid = $false
            $reasons.Add('the eye picture did not differ from the menu baseline (the title did not leave the menu)')
        }
    }
    if ($CellMode -eq 'rdoc' -and $captures.Count -eq 0) { $valid = $false; $reasons.Add('no RenderDoc capture appeared') }

    # --- evidence ---------------------------------------------------------------------------
    $diagPattern = 'frame stats at|TIMELINE |PrepareHmdFrame|VR HMD|EnsureSwapchain|RENDERSTAT|ALIASSTAT|RDOC |DumpGpuImages|AUTOPRESS|Live script|Replaying |<Error>|<Critical>|Device lost|Unreachable code|Assertion Failed'
    $diag = @($parse.lines | Where-Object { $_ -match $diagPattern })
    Write-TextFile (Join-Path $runDir 'diag.txt') (($diag -join "`r`n") + "`r`n")

    $csv = New-Object System.Collections.Generic.List[string]
    $csv.Add('sample,at_s,fps,frame_ms,passes,loading_passes,mpx,draws,dispatches,submits,gpu_busy_pct,gpu_mhz,in_window,phase')
    $phaseOf = @{}
    foreach ($ph in $analysis.phases) {
        for ($i = $ph.first_index; $i -le $ph.last_index; $i++) { $phaseOf[$i] = $ph.kind }
    }
    for ($i = 0; $i -lt $parse.samples.Count; $i++) {
        $s = $parse.samples[$i]
        $inWindow = $false
        if ($null -ne $measured) { $inWindow = ($i -ge $measured.first_index -and $i -le $measured.last_index) }
        $frameMs = if ($s.fps -gt 0) { 1000.0 / $s.fps } else { 0.0 }
        $csv.Add(('{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13}' -f
                $s.index, $s.at_s, $s.fps, (Round3 $frameMs), $s.passes, $s.loading, $s.mpx,
                $s.draws, $s.dispatches, $s.submits, $s.busy, $s.mhz, [int]$inWindow,
                $(if ($phaseOf.ContainsKey($i)) { $phaseOf[$i] } else { '' })))
    }
    Write-TextFile (Join-Path $runDir 'timings.csv') (($csv -join "`r`n") + "`r`n")

    $cleanBlocked = @($parse.hmd.clean_exit_markers | Where-Object { $_ -match 'left alone|R6 blocked|did not pause' }).Count -gt 0
    $cleanGone = @($parse.hmd.clean_exit_markers | Where-Object { $_ -match 'presenter is gone|presenter was destroyed|presenter destroyed' }).Count -gt 0
    $cleanExit = (($shutdown.method -eq 'the emulator exited on its own') -and $cleanGone -and -not $cleanBlocked)
    $shutdownForResult = [ordered]@{
        exit_code = $shutdown.exit_code; seconds = $shutdown.seconds
        ok = $shutdown.ok; method = $shutdown.method; reason = $shutdown.reason
        clean_exit = $cleanExit
        clean_exit_blocked = $cleanBlocked
        exit_after_markers = @($parse.hmd.exit_after_markers)
    }
    $result = [ordered]@{
        valid                = $valid
        reason               = $(if ($valid) { 'ok' } else { ($reasons -join '; ') })
        label                = $CellLabel
        phase                = $analysis.phase
        gameplay             = ($analysis.phase -eq 'gameplay')
        gate                 = $Gate
        mode                 = $CellMode
        capture_only         = $captureOnly
        timing_valid         = (-not $captureOnly)
        arm                  = $CellArm
        exe                  = [ordered]@{ path = $ExePath; sha256 = $ExeHash; bytes = (Get-FileSize $ExePath) }
        config               = [ordered]@{
            render_percent = $Percent; hz = $RefreshHz; fps_cap = $RefreshHz
            msaa = 2; eye_width = 2880; vr_scale = 0.5; stats_every_s = $StatsEvery
        }
        fps_p50              = $(if ($null -ne $fpsStats) { $fpsStats.p50 } else { $null })
        fps_p90              = $(if ($null -ne $fpsStats) { $fpsStats.p90 } else { $null })
        fps_p99              = $(if ($null -ne $fpsStats) { $fpsStats.p99 } else { $null })
        fps_mean             = $(if ($null -ne $fpsStats) { $fpsStats.mean } else { $null })
        frames               = $(if ($null -ne $measured) { $measured.count } else { 0 })
        draws_mean           = $(if ($null -ne $band) { $band.draws_mean } else { $null })
        mpx_mean             = $(if ($null -ne $band) { $band.mpx_mean } else { $null })
        passes_mean          = $(if ($null -ne $band) { $band.passes_mean } else { $null })
        gpu_busy_mean        = $(if ($null -ne $measured -and $measured.count -gt 0) {
                $b = @($measured.samples | Where-Object { $_.busy -ge 0 } | ForEach-Object { [double]$_.busy })
                if ($b.Count -gt 0) { Round1 (Get-MeanOf $b) } else { $null }
            } else { $null })
        images_created_per_frame = $(if ($null -ne $counters) { $counters.per_frame['images_created'] } else { $null })
        create_ms_per_frame  = $(if ($null -ne $counters) { $counters.per_frame['create_ms'] } else { $null })
        destroy_ms_per_frame = $(if ($null -ne $counters) { $counters.per_frame['destroy_ms'] } else { $null })
        recycler_hits_per_frame = $(if ($null -ne $counters) { $counters.per_frame['recycler_hits'] } else { $null })
        recycler_misses_per_frame = $(if ($null -ne $counters) { $counters.per_frame['recycler_misses'] } else { $null })
        pool_mb_mean         = $(if ($null -ne $counters) { $counters.per_s['pool_mb'] } else { $null })
        vram_mb_mean         = $(if ($null -ne $counters) { $counters.per_s['vram_mb'] } else { $null })
        depth_copies         = $(if ($null -ne $counters) { $counters.per_frame['depth_copies'] } else { $null })
        refresh_uploads      = $(if ($null -ne $counters) { $counters.per_frame['refreshes'] } else { $null })
        eye_width            = $hmd.eye_width
        eye_height           = $hmd.eye_height
        samples              = $hmd.samples
        format               = $hmd.format
        scene_first_s        = $analysis.anchor_s
        scene_last_s         = $(if ($null -ne $measured -and $measured.count -gt 0) { $measured.samples[-1].at_s } else { $null })
        shutdown             = $shutdownForResult
        captures             = $captures.ToArray()
        window               = [ordered]@{
            warmup_s = $Warmup; seconds_s = $Seconds
            from_s = $(if ($null -ne $measured) { $measured.from_s } else { $null })
            to_s = $(if ($null -ne $measured) { $measured.to_s } else { $null })
            anchor_index = $analysis.anchor_index
        }
        band                 = $band
        image_gate           = $imageGate
        hmd                  = $hmd
    }

    $meta = [ordered]@{
        label        = $CellLabel
        harness      = 'headless-vr-gameplay v1'
        mode         = $CellMode
        gate         = $Gate
        phase        = $analysis.phase
        exe          = [ordered]@{
            path = $ExePath; sha256 = $ExeHash; bytes = (Get-FileSize $ExePath)
            lab_copy = $labExe; build_info = $parse.hmd.build_info.ToArray()
        }
        lab          = [ordered]@{
            dir = $labDir; log = $logPath; eboot = $Eboot
            template = $script:TemplateDir; savedata = $script:SavedataSource
            save_data = $saveInfo
            shadow_ini = $script:IniPath; shadow_ini_written = $script:IniWasWritten
            input = $inputInfo.source; input_lab_copy = $inputInfo.lab_copy; live_input = $inputInfo.live
        }
        launch       = [ordered]@{ command = $launchCommand; working_dir = $labDir }
        settings     = $settingsText
        env          = $envMap
        references   = [ordered]@{ menu = $menuRef; gameplay = $gameRef }
        scene        = [ordered]@{
            anchor_s = $analysis.anchor_s; anchor_index = $analysis.anchor_index
            phase = $analysis.phase; reason = $analysis.reason
            menu = $analysis.menu; loading = $analysis.loading; gameplay = $analysis.gameplay
            heavy_runs = $analysis.heavy_runs.ToArray(); phases = $analysis.phases.ToArray()
            gate_options = $script:Opt
        }
        window       = $result.window
        content      = [ordered]@{
            draws_mean = $result.draws_mean; mpx_mean = $result.mpx_mean
            passes_mean = $result.passes_mean; gpu_busy_mean = $result.gpu_busy_mean
            fps = $fpsStats
        }
        counters     = $counters
        hmd          = $hmd
        host         = [ordered]@{
            started = $startedAt.ToString('o'); ended = $endedAt.ToString('o')
            wall_seconds = Round3 ($endedAt - $startedAt).TotalSeconds
            quiet_before = $auditBefore; quiet_after = $auditAfter
            lab_leftovers_stopped = @($leftovers | ForEach-Object { $_.Id })
            other_emulators_before = $othersBefore; other_emulators_after = $othersAfter
            gpu_before = $gpuBefore; gpu_after = $gpuAfter
        }
        lock         = [ordered]@{
            source = $script:Lock.source; owner = $script:Lock.owner; token = $script:Lock.token
            script = $script:LockScript
        }
        shutdown     = $shutdownForResult
        captures     = $captures.ToArray()
        image_gate   = $imageGate
        log          = [ordered]@{
            lines = $parse.lines.Count; file = 'shad_log.txt'; diag = 'diag.txt'
            error_lines = $parse.hmd.error_lines.Count
            critical_errors = $parse.hmd.critical_errors.ToArray()
            update_worn_lines = $parse.hmd.update_worn_lines
            title_fov = $parse.hmd.title_fov
            clean_exit_markers = $parse.hmd.clean_exit_markers.ToArray()
            exit_after_markers = $parse.hmd.exit_after_markers.ToArray()
            rdoc_events = $parse.hmd.rdoc_events.ToArray()
            dumps = $parse.hmd.dumps.ToArray()
            live_notes = $liveNotes.ToArray()
        }
        result       = $result
        notes        = @()
    }
    if ($endedEarly) { $meta.notes += $endedEarly }
    if ($stopReason -eq 'timeout') { $meta.notes += ("the session hit the {0} s cap" -f $MaxSeconds) }
    if ($script:Lock.source -eq 'none') { $meta.notes += 'the GPU lock was not held (NoLock)' }
    if ($Gate -eq 'menu' -or $Gate -eq 'heavy') { $meta.notes += 'calibration gate: this cell is not a gameplay measurement' }
    Write-JsonFile (Join-Path $runDir 'session.json') ([ordered]@{
            label = $CellLabel; mode = $CellMode; arm = $CellArm; gate = $Gate
            exe = $ExePath; sha256 = $ExeHash; lab = $labDir; eboot = $Eboot
            launch = $launchCommand; env = $envMap; settings = $settingsText
            input_script = $inputInfo.text; started = $startedAt.ToString('o')
            ended = $endedAt.ToString('o'); lock = $meta.lock
        })
    Write-JsonFile (Join-Path $runDir 'meta.json') $meta
    Write-JsonFile (Join-Path $runDir 'result.json') $result
    if ($captures.Count -gt 0) {
        Write-JsonFile (Join-Path $runDir 'captures.json') ([ordered]@{
                label = $CellLabel; dump_width = $DumpWidth; captures = $captures.ToArray()
            })
    }
    Write-Host ''
    Write-Host ("--- {0}: valid={1} phase={2} fps p50={3} mean={4} frames={5} draws={6} mpx={7} passes={8} gpu={9}% ---" -f `
            $CellLabel, $valid, $analysis.phase, $result.fps_p50, $result.fps_mean, $result.frames,
            $result.draws_mean, $result.mpx_mean, $result.passes_mean, $result.gpu_busy_mean)
    if (-not $valid) { Write-Host ("reason: {0}" -f $result.reason) }
    Write-Host ("evidence: {0}" -f $runDir)
    return [pscustomobject]@{ result = $result; meta = $meta; run_dir = $runDir }
}
# --- self-test --------------------------------------------------------------------------------
function Invoke-SelfTestOnLog([string]$LogPath) {
    if (-not (Test-Path $LogPath)) { throw "log not found: $LogPath" }
    $item = Get-Item -LiteralPath $LogPath
    if ($item.PSIsContainer) {
        $candidate = Join-Path $LogPath 'shad_log.txt'
        if (-not (Test-Path $candidate)) { throw "no shad_log.txt in $LogPath" }
        $LogPath = $candidate
    }
    Write-Host ''
    Write-Host ("=== self-test: {0}" -f $LogPath)
    $parse = Get-ParseStateFromFile $LogPath
    $analysis = Get-SceneAnalysis $parse $script:Opt
    $hmd = Test-Hmd $parse $script:Opt
    $measured = $null
    $band = $null
    $counters = $null
    $fpsStats = $null
    if ($analysis.anchor_index -ge 0 -and $analysis.anchor_index -lt $parse.samples.Count) {
        $measured = Get-MeasuredWindow $parse.samples $analysis.anchor_index $Warmup $Seconds
        if ($measured.count -gt 0) {
            $band = Test-Band $measured $script:Opt
            $fpsStats = Get-Stats @($measured.samples | ForEach-Object { [double]$_.fps })
            $counters = Get-CounterSummary $parse $measured ([double]$fpsStats.mean)
        }
    }
    $summary = [ordered]@{
        log = (Resolve-Path -LiteralPath $LogPath).Path
        lines = $parse.lines.Count
        samples = $parse.samples.Count
        gate = $Gate
        phase = $analysis.phase
        reason = $analysis.reason
        anchor_s = $analysis.anchor_s
        menu = $analysis.menu
        loading = $analysis.loading
        gameplay = $analysis.gameplay
        heavy_runs = $analysis.heavy_runs.ToArray()
        phases = $analysis.phases.ToArray()
        hmd_ok = $hmd.ok
        hmd = $hmd
        measured = $(if ($null -ne $measured) { [ordered]@{ count = $measured.count; from_s = $measured.from_s; to_s = $measured.to_s } } else { $null })
        band = $band
        fps = $fpsStats
        counters = $counters
        errors = $parse.hmd.error_lines.ToArray()
    }
    Write-Host ("samples={0}  heavy_runs={1}  menu={2}  loading={3}  gameplay={4}" -f `
            $parse.samples.Count, $analysis.heavy_runs.Count,
        $(if ($null -ne $analysis.menu) { ('{0} draws / {1} Mpx @ {2}s' -f $analysis.menu.draws_mean, $analysis.menu.mpx_mean, $analysis.menu.first_s) } else { 'none' }),
        $(if ($null -ne $analysis.loading) { ('{0}s..{1}s' -f $analysis.loading.first_s, $analysis.loading.last_s) } else { 'none' }),
        $(if ($null -ne $analysis.gameplay) { ('{0} draws / {1} Mpx @ {2}s' -f $analysis.gameplay.draws_mean, $analysis.gameplay.mpx_mean, $analysis.gameplay.first_s) } else { 'none' }))
    Write-Host ("gate: {0}  phase={1}  anchor={2}s  reason={3}" -f $Gate, $analysis.phase, $analysis.anchor_s, $analysis.reason)
    if ($null -ne $fpsStats) {
        Write-Host ("window: {0} samples, fps p50={1} p90={2} p99={3} mean={4}; draws={5} mpx={6} passes={7}; band_ok={8}" -f `
                $measured.count, $fpsStats.p50, $fpsStats.p90, $fpsStats.p99, $fpsStats.mean,
            $band.draws_mean, $band.mpx_mean, $band.passes_mean, $band.ok)
    }
    Write-Host ("hmd: ok={0} eye={1}x{2} image={3} format={4} samples={5} worn_lines={6} openxr_disabled={7}" -f `
            $hmd.ok, $hmd.eye_width, $hmd.eye_height, $hmd.image, $hmd.format, $hmd.samples,
        $hmd.update_worn_lines, $hmd.openxr_disabled)
    if ($counters) { Write-Host ("counters: renderstat={0} aliasstat={1}" -f $counters.renderstat_samples, $counters.aliasstat_samples) }
    if ($SelfTestWrite) {
        New-Item -ItemType Directory -Path $script:SelfTestDir -Force | Out-Null
        $name = $LogPath
        try {
            $full = (Resolve-Path -LiteralPath $LogPath).Path
            $rootFull = (Resolve-Path -LiteralPath $Root).Path
            if ($full.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
                $name = $full.Substring($rootFull.Length).TrimStart('\', '/')
            } else { $name = Split-Path $full -Leaf }
        } catch {}
        $name = $name -replace '[\\/:]', '_' -replace '[^0-9A-Za-z._-]', '_'
        $out = Join-Path $script:SelfTestDir ($name + '.json')
        Write-JsonFile $out $summary
        Write-Host ("self-test result: {0}" -f $out)
    }
    return $summary
}

# A harness that was killed hard (tool timeout, SIGKILL) left its emulator running and its pid
# record behind. Only processes this harness itself recorded are stopped here: the recorded exe
# must match the live process's path and live under _build\headless\labs. Another agent's lab is
# never touched.
function Stop-OrphanedHarnessEmulators {
    $stopped = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path $script:RunsRoot)) { return , $stopped.ToArray() }
    foreach ($dir in @(Get-ChildItem -LiteralPath $script:RunsRoot -Directory -ErrorAction SilentlyContinue)) {
        $pidFile = Join-Path $dir.FullName 'emulator.pid'
        if (-not (Test-Path $pidFile)) { continue }
        $info = $null
        try { $info = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json } catch {}
        if ($null -ne $info -and $null -ne $info.pid) {
            $p = Get-Process -Id ([int]$info.pid) -ErrorAction SilentlyContinue
            if ($null -ne $p) {
                $path = $null
                try { $path = $p.Path } catch {}
                if ($path -and $info.exe -and $path -ieq [string]$info.exe -and $path -like '*_build\headless\labs\*') {
                    try {
                        Stop-Process -Id ([int]$info.pid) -Force -ErrorAction Stop
                        $stopped.Add([ordered]@{ pid = [int]$info.pid; exe = $path; label = $info.label })
                        Write-Host ("stopped a leftover emulator of an earlier killed cell: pid {0} ({1})" -f $info.pid, $info.label)
                    } catch {}
                }
            }
        }
        Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    }
    return , $stopped.ToArray()
}

# --- main -------------------------------------------------------------------------------------
Set-Options

# Mode-dependent defaults (lowres render size, rdoc gate, per-gate band) are applied before the
# self-test branch so -SelfTest uses the same gate and band as a real run.
if ($Mode -eq 'lowres') {
    $RenderPercent = 100
    $Hz = 120
    # 100 % of a PSVR panel is 960x1080 an eye, 1920x1080 side by side; the 300 % eye/image
    # expectation must not reject a correct lowres cell.
    if (-not $PSBoundParameters.ContainsKey('ExpectEye')) { $ExpectEye = '960x1080' }
    if (-not $PSBoundParameters.ContainsKey('ExpectImage')) { $ExpectImage = '1920x1080' }
}
if ($Mode -eq 'rdoc' -and -not $PSBoundParameters.ContainsKey('Gate')) {
    # A RenderDoc capture is of the front-end/classroom phase the scene gate matches; requiring
    # a gameplay transition would never let it fire before the capture stops the session.
    $Gate = 'menu'
}
# The documented band is per phase: the classroom menu is ~465-490 draws / ~480 Mpx / 65 passes,
# the confirmed gameplay phase is ~1870 draws / ~625 Mpx / 30-35 fps (scene-eng, m2-confirm-1).
# A -Gate menu cell measures the menu, a -Gate gameplay cell measures gameplay; each is checked
# against its own band unless the caller gave one explicitly.
if (-not $PSBoundParameters.ContainsKey('BandMinDraws') -and -not $PSBoundParameters.ContainsKey('BandMaxDraws')) {
    if ($Gate -eq 'gameplay') { $BandMinDraws = 1500; $BandMaxDraws = 2300 }
    else { $BandMinDraws = 400; $BandMaxDraws = 620 }
}
if (-not $PSBoundParameters.ContainsKey('BandMinMpx') -and -not $PSBoundParameters.ContainsKey('BandMaxMpx')) {
    if ($Gate -eq 'gameplay') { $BandMinMpx = 550; $BandMaxMpx = 750 }
    else { $BandMinMpx = 400; $BandMaxMpx = 560 }
}
if (-not $PSBoundParameters.ContainsKey('BandMinPasses') -and -not $PSBoundParameters.ContainsKey('BandMaxPasses')) {
    if ($Gate -eq 'gameplay') { $BandMinPasses = 35; $BandMaxPasses = 55 }
    else { $BandMinPasses = 55; $BandMaxPasses = 80 }
}
Set-Options
# The Mpx gate and band are defined at 300 % (2880x3240 an eye). A different render size draws a
# proportional area, so the Mpx thresholds scale with the square of the size unless given
# explicitly. Draw counts are geometry, not pixels, and do not scale.
if ($RenderPercent -ne 300) {
    $areaScale = [math]::Pow($RenderPercent / 300.0, 2)
    if (-not $PSBoundParameters.ContainsKey('SceneMinMpx')) { $SceneMinMpx = [math]::Round($SceneMinMpx * $areaScale, 1) }
    if (-not $PSBoundParameters.ContainsKey('BandMinMpx')) { $BandMinMpx = [math]::Round($BandMinMpx * $areaScale, 1) }
    if (-not $PSBoundParameters.ContainsKey('BandMaxMpx')) { $BandMaxMpx = [math]::Round($BandMaxMpx * $areaScale, 1) }
    Set-Options
}

# --- resolution-specific gameplay profile -------------------------------------------------
# A 100 % cell draws 960x1080 an eye (1920x1080 side by side), so its target writes are roughly
# a ninth of the 300 % cell's; a shared absolute Mpx threshold cannot classify both. Draws are
# geometry and do not scale, so the 800-draw anchor separates gameplay (~1860) from the menu
# (~465) and the loading screen (42-55) at every resolution. The Mpx/pass band below was
# calibrated from a valid 100 % capture (classroom image verified), not from the menu.
$profileSize = if ($Profile -ne 'auto') { [int]$Profile } else { $RenderPercent }
if ($profileSize -eq 100) {
    if (-not $PSBoundParameters.ContainsKey('ExpectEye')) { $ExpectEye = '960x1080' }
    if (-not $PSBoundParameters.ContainsKey('ExpectImage')) { $ExpectImage = '1920x1080' }
    if (-not $PSBoundParameters.ContainsKey('SceneMinDraws')) { $SceneMinDraws = 800 }
    if (-not $PSBoundParameters.ContainsKey('SceneMinMpx')) { $SceneMinMpx = 50 }
    if (-not $PSBoundParameters.ContainsKey('BandMinDraws')) { $BandMinDraws = 1500 }
    if (-not $PSBoundParameters.ContainsKey('BandMaxDraws')) { $BandMaxDraws = 2300 }
    if (-not $PSBoundParameters.ContainsKey('BandMinMpx')) { $BandMinMpx = 55 }
    if (-not $PSBoundParameters.ContainsKey('BandMaxMpx')) { $BandMaxMpx = 90 }
    if (-not $PSBoundParameters.ContainsKey('BandMinPasses')) { $BandMinPasses = 30 }
    if (-not $PSBoundParameters.ContainsKey('BandMaxPasses')) { $BandMaxPasses = 55 }
    if (-not $PSBoundParameters.ContainsKey('SettleTolerancePct')) { $SettleTolerancePct = 15 }
    Set-Options
}

if ($script:SelfTestMode) {
    $results = @()
    foreach ($path in $SelfTestLog) {
        $results += Invoke-SelfTestOnLog $path
    }
    Write-Host ''
    Write-Host ("self-test: {0} log(s) parsed" -f $results.Count)
    exit 0
}

# A single-session run. The lock is held for the whole cell (and for all cells of an A/B/A), so
# an old testslot.sh user cannot start an emulator in the middle of a measurement.
if (-not (Test-Path $Exe)) { throw "executable not found: $Exe" }
if (-not (Test-Path $Eboot)) { throw "game not found: $Eboot" }
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$exeHash = Get-FileSha256 $exePath

if ($HmdDumpAtScene -ne '' -and -not $Capture) { throw '-HmdDumpAtScene needs -Capture (it is a capture-only cell, never a timed one)' }
if ($CleanExit -and $ExitAfterSeconds -le 0) { throw '-CleanExit needs -ExitAfterSeconds > 0' }
if ($Capture -and $DumpFrames -ne '') { throw '-Capture and -DumpFrames both dump the run; use -Capture (post-window) or -DumpFrames (fixed frames), not both' }
if ($Label -eq '') {
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $Label = ('headless-{0}-{1}-{2}' -f $Mode, $Arm, $stamp)
    if ($Mode -eq 'lowres') { $Label = ('headless-lowres100-{0}' -f $stamp) }
}
New-Item -ItemType Directory -Path $script:RunsRoot, $script:LabsRoot, $script:InputsDir -Force | Out-Null

$orphansStopped = Stop-OrphanedHarnessEmulators
if ($orphansStopped.Count -gt 0) { Start-Sleep -Milliseconds 1200 }
if ($LockOwner -eq '') { $LockOwner = 'headless-vr-gameplay:' + $Label }
$ttl = [math]::Max(600, $MaxSeconds + 300)
if (-not $NoLock) {
    Start-LockHold $LockOwner ('headless VR cell ' + $Label) $ttl
} else {
    Write-Host 'gpu lock: skipped (-NoLock)'
}
if ($RequireQuiet -and $script:Lock.source -eq 'none') {
    $busy = Get-ProcessAudit
    if ($busy.Count -gt 0) {
        Write-Host ("quiet check: another benchmark process is running: {0}" -f ($busy -join ', '))
        exit 7
    }
}

# One cell, retried when the attempt died before it could measure (the known startup crash) and
# not when the scene/band/quiet result is a real answer.
function Invoke-CellWithRetry {
    param([string]$CellLabel, [string]$CellArm, [string]$CellMode, [string]$ExePath, [string]$ExeHash,
          [int]$Percent, [int]$RefreshHz)
    $last = $null
    $attempts = [math]::Max(1, $Attempts)
    for ($i = 1; $i -le $attempts; $i++) {
        $attemptLabel = if ($i -eq 1) { $CellLabel } else { '{0}-try{1}' -f $CellLabel, $i }
        if ($attempts -gt 1) { Write-Host ("=== attempt {0}/{1}: {2}" -f $i, $attempts, $attemptLabel) }
        $cell = Invoke-CellSession -CellLabel $attemptLabel -CellArm $CellArm -CellMode $CellMode `
            -ExePath $ExePath -ExeHash $ExeHash -Percent $Percent -RefreshHz $RefreshHz
        $last = $cell
        if ($cell.result.valid) { return $cell }
        # Only a crash/short death is retried; a gate that never matched after the full cap is a
        # real answer (for example the menu never loading), not a startup crash.
        $retryable = ($cell.result.reason -match 'emulator ended on its own|critical error|no frame-stats samples|no PrepareHmdFrame|the measured window has 0')
        if (-not $retryable) { return $cell }
        Write-Host ("attempt {0} is not a measurement ({1}); retrying" -f $i, $cell.result.reason)
    }
    return $last
}

$cellResults = New-Object System.Collections.Generic.List[object]
try {
    if ($Mode -eq 'aba') {
        $arms = @('on', 'off', 'on')
        $round = 0
        foreach ($arm in $arms) {
            $round++
            $cellLabel = ('{0}-{1}-r{2}' -f $Label, $arm, $round)
            Write-Host ("=== cell {0}/{1}: {2}={3} -> {4}" -f $round, $arms.Count, $ArmVariable, $arm, $cellLabel)
            $cell = Invoke-CellWithRetry -CellLabel $cellLabel -CellArm $arm -CellMode 'cell' `
                -ExePath $exePath -ExeHash $exeHash -Percent $RenderPercent -RefreshHz $Hz
            $cellResults.Add($cell)
            Send-LockHeartbeat
        }
        # --- the A/B/A report -------------------------------------------------------------
        $rows = @($cellResults | ForEach-Object { $_.result })
        $on = @($rows | Where-Object { $_.arm -eq 'on' })
        $off = @($rows | Where-Object { $_.arm -eq 'off' })
        $med = {
            param($values)
            $v = @($values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
            if ($v.Count -eq 0) { return $null }
            return Round3 (Get-Percentile $v 50)
        }
        $onFps = & $med @($on | ForEach-Object { $_.fps_p50 })
        $offFps = & $med @($off | ForEach-Object { $_.fps_p50 })
        $onSpread = @($on | ForEach-Object { [double]$_.fps_p50 })
        $offSpread = @($off | ForEach-Object { [double]$_.fps_p50 })
        $spreadOn = if ($onSpread.Count -gt 1) { Round3 (($onSpread | Measure-Object -Maximum).Maximum - ($onSpread | Measure-Object -Minimum).Minimum) } else { $null }
        $spreadOff = if ($offSpread.Count -gt 1) { Round3 (($offSpread | Measure-Object -Maximum).Maximum - ($offSpread | Measure-Object -Minimum).Minimum) } else { $null }
        $delta = if ($null -ne $onFps -and $null -ne $offFps) { Round3 ($onFps - $offFps) } else { $null }
        $deltaPct = if ($null -ne $delta -and $null -ne $offFps -and $offFps -ne 0) { Round1 (100.0 * $delta / $offFps) } else { $null }
        $spreadMax = @(@($spreadOn, $spreadOff) | Where-Object { $null -ne $_ })
        $spreadWorst = if ($spreadMax.Count -gt 0) { ($spreadMax | Measure-Object -Maximum).Maximum } else { $null }
        $verdict = 'not enough valid cells'
        if ($null -ne $delta -and $null -ne $spreadWorst) {
            $verdict = if ([math]::Abs($delta) -gt $spreadWorst) { 'distinguishable' } else { 'not distinguishable (the difference is inside the run-to-run spread)' }
        }
        $ab = [ordered]@{
            tag = $Label; variable = $ArmVariable; order = @('on', 'off', 'on')
            exe = $exePath; sha256 = $exeHash; started = (Get-Date).ToString('o')
            cells = @($rows | ForEach-Object {
                    [ordered]@{
                        label = $_.label; arm = $_.arm; valid = $_.valid; phase = $_.phase
                        fps_p50 = $_.fps_p50; fps_mean = $_.fps_mean; draws_mean = $_.draws_mean
                        mpx_mean = $_.mpx_mean; passes_mean = $_.passes_mean; gpu_busy_mean = $_.gpu_busy_mean
                        shutdown = $_.shutdown; reason = $_.reason
                    }
                })
            on = [ordered]@{ fps_p50_median = $onFps; spread = $spreadOn; cells = @($on | ForEach-Object { $_.label }) }
            off = [ordered]@{ fps_p50_median = $offFps; spread = $spreadOff; cells = @($off | ForEach-Object { $_.label }) }
            delta = [ordered]@{ on_minus_off_fps = $delta; relative_pct = $deltaPct }
            acceptance = [ordered]@{
                rule = 'a cell counts only when valid; the arms differ only when |delta| exceeds the larger run-to-run spread of the two arms'
                spread_worst = $spreadWorst; verdict = $verdict
            }
        }
        New-Item -ItemType Directory -Path (Join-Path $script:RunsRoot $Label) -Force | Out-Null
        Write-JsonFile (Join-Path $script:RunsRoot ($Label + '\ab.json')) $ab
        $md = New-Object System.Collections.Generic.List[string]
        $md.Add("# A/B/A $Label")
        $md.Add('')
        $md.Add(("Variable: {0}; order on, off, on; executable {1}" -f $ArmVariable, (Split-Path $exePath -Leaf)))
        $md.Add('')
        $md.Add('| cell | arm | valid | phase | fps p50 | fps mean | draws | Mpx | passes | GPU % | shutdown |')
        $md.Add('|---|---|---|---|---|---|---|---|---|---|---|')
        foreach ($c in $ab.cells) {
            $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |' -f
                    $c.label, $c.arm, $c.valid, $c.phase, $c.fps_p50, $c.fps_mean, $c.draws_mean,
                    $c.mpx_mean, $c.passes_mean, $c.gpu_busy_mean, $c.shutdown.method))
        }
        $md.Add('')
        $md.Add(("on median {0} fps, off median {1} fps, delta {2} fps ({3} %); worst spread {4} fps -> **{5}**" -f $onFps, $offFps, $delta, $deltaPct, $spreadWorst, $verdict))
        Write-TextFile (Join-Path $script:RunsRoot ($Label + '\ab.md')) (($md -join "`r`n") + "`r`n")
        Write-Host ''
        Write-Host ("A/B/A {0}: on {1} fps, off {2} fps, delta {3} fps ({4} %), spread {5} -> {6}" -f $Label, $onFps, $offFps, $delta, $deltaPct, $spreadWorst, $verdict)
        $allValid = @($rows | Where-Object { -not $_.valid }).Count -eq 0
        if (-not $allValid) { Write-Host 'A/B/A: at least one cell is not valid' }
        exit $(if ($allValid) { 0 } else { 2 })
    } else {
        $cell = Invoke-CellWithRetry -CellLabel $Label -CellArm $Arm -CellMode $Mode `
            -ExePath $exePath -ExeHash $exeHash -Percent $RenderPercent -RefreshHz $Hz
        $cellResults.Add($cell)
        exit $(if ($cell.result.valid) { 0 } else { 2 })
    }
} finally {
    Stop-LockHold
}
