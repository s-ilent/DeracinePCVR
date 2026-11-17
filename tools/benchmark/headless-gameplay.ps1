# Headless gameplay benchmark (M2) — Déraciné in gameplay with no OpenXR host and no headset.
#
# One cell: build -> isolated lab -> headless launch -> scripted Move entry -> gameplay window ->
# normal quit -> unique-rendered-FPS verdict. The scene gate and the unique-FPS arithmetic live in
# gameplay_gate.py (unit-tested, no GPU); this script only orchestrates and records provenance.
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
#   bash _build/team/gpu-lock.sh run --owner <you> --kind headless --reason "headless gameplay" \
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
    [Parameter(Mandatory)][string]$SavesFrom,
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
    [int]$TimeoutSeconds = 300,
    [string]$Python = 'python',
    [switch]$EyeDump,
    [switch]$DryRun,
    [string]$ReplayLog = ''
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($Out -eq '') { $Out = Join-Path $Repo "_out\headless-gameplay\$Label" }
if ($Lab -eq '') { $Lab = Join-Path $Out 'lab' }
if ($Template -eq '') { $Template = Join-Path $Repo 'pc-vr' }
if ($InputScript -eq '') { $InputScript = Join-Path $Repo 'tools\headless-vr\input\gameplay-entry.txt' }
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

Assert-File $Exe 'executable'
Assert-File $Eboot 'eboot'
Assert-File $InputScript 'input script'
Assert-File $Profile 'gameplay profile'
Assert-File $SaveManifest 'save manifest'

# --- deterministic save slot ------------------------------------------------------------------
# The save is game/user data and is not committed; the manifest pins the exact bytes. Refuse to run
# on anything else, so a cell can never silently measure a different stage.
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
$saveHashes = [ordered]@{}
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

$exeSha = Get-Sha256 $Exe
$ebootSha = Get-Sha256 $Eboot
$branch = (& git -C $Repo rev-parse --abbrev-ref HEAD 2>$null)
$head = (& git -C $Repo rev-parse HEAD 2>$null)
$dirty = @(& git -C $Repo status --porcelain 2>$null)

# --- proven headless gameplay environment -----------------------------------------------------
# From the M2 confirm run (_build/headless/probe-scene/evidence/m2-confirm-1/run.cmd, 2026-11-18)
# plus the HC stats cadence. Every game-affecting switch is explicit; the synthetic input switches
# are the only reason a run without a headset can reach gameplay.
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
    SHADPS4_DBG_AUTOPRESS_TO   = '45000'
    SHADPS4_INPUT_SCRIPT       = (Resolve-Path -LiteralPath $InputScript).Path
    # Normal quit through the emulator's own path; clean presenter teardown (M2).
    SHADPS4_EXIT_AFTER         = "$ExitAfterSeconds"
    SHADPS4_CLEAN_EXIT         = '1'
}
if ($EyeDump) {
    # Scene-gated eye dump at the gameplay gate; the folder is relative to the user directory.
    $envMap['SHADPS4_DBG_DUMP_HMD_AT_SCENE'] = '800,550,rt_dump-gameplay'
}

Write-Output "executable $Exe ($exeSha)"
Write-Output "eboot      $Eboot ($ebootSha)"
Write-Output "script     $InputScript"

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
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$labExe = Join-Path $Lab 'shadPS4.exe'
if (-not (Test-Path -LiteralPath (Join-Path $Lab 'user\config.json'))) {
    Write-Output "creating isolated lab $Lab from $Template"
    if (-not (Test-Path -LiteralPath $Template -PathType Container)) {
        Write-Failure "template runtime not found: $Template"
    }
    New-Item -ItemType Directory -Force -Path $Lab | Out-Null
    $copyArgs = @('/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP',
                  '/XF', 'shadps4.exe', '/XD', 'log', 'temp', '_out')
    & robocopy $Template $Lab @copyArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { Write-Failure "robocopy of the template failed ($LASTEXITCODE)" }
    $dstSave = Join-Path $Lab 'user\home\1000\savedata\CUSA11919\SaveData'
    New-Item -ItemType Directory -Force -Path $dstSave | Out-Null
    Copy-Item (Join-Path $saveSrc '*') $dstSave -Recurse -Force
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
        ($cfg | ConvertTo-Json -Depth 20) | Set-Content -LiteralPath $cfgPath -Encoding utf8
    }
} else {
    Write-Output "reusing lab $Lab (delete it to force a clean rebuild)"
}
Copy-Item -LiteralPath $Exe -Destination $labExe -Force
New-Item -ItemType Directory -Force -Path (Join-Path $Lab 'user\log') | Out-Null
Remove-Item (Join-Path $Lab 'user\log\*') -Force -ErrorAction SilentlyContinue
Get-ChildItem $Lab -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

foreach ($k in $envMap.Keys) { Set-Item -Path "Env:$k" -Value $envMap[$k] }
# Do not inherit a live script or replay overrides from the caller.
Remove-Item Env:SHADPS4_LIVE_INPUT -ErrorAction SilentlyContinue
Remove-Item Env:SHADPS4_DBG_HAND_POSE -ErrorAction SilentlyContinue
Remove-Item Env:SHADPS4_DBG_DUMP_HMD_FRAMES -ErrorAction SilentlyContinue

# --- run --------------------------------------------------------------------------------------
$log = Join-Path $Lab 'user\log\shad_log.txt'
$stdout = Join-Path $Out 'stdout.txt'
$stderr = Join-Path $Out 'stderr.txt'
$started = Get-Date
Write-Output "launching $labExe -g $Eboot"
$p = Start-Process -FilePath $labExe -WorkingDirectory $Lab -ArgumentList @('-g', $Eboot) `
    -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$deadline = $started.AddSeconds($TimeoutSeconds)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
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
Copy-Item -LiteralPath $log -Destination $logCopy -Force
$logSha = Get-Sha256 $logCopy

# --- verdict ----------------------------------------------------------------------------------
$result = Invoke-Gate $logCopy $saveSrc
$verdict = $result.verdict
$document = [ordered]@{
    schema_version  = 1
    label           = $Label
    created_utc     = $started.ToUniversalTime().ToString('o')
    mode            = 'launch'
    scenario        = [ordered]@{
        id      = 'deracine-headless-gameplay-300-msaa2-vblank120'
        profile = $Profile
        gate    = [ordered]@{ min_draws = 800; min_mpx = 450; consecutive_samples = 2 }
        band    = [ordered]@{ draws = '1500-2300'; mpx = '550-750'; passes = '35-55' }
        input   = $InputScript
        save    = [ordered]@{
            source = (Resolve-Path $SavesFrom).Path
            slot   = $manifest.slot
            stage  = $manifest.stage
            files  = $saveHashes
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
    log             = [ordered]@{ path = $logCopy; sha256 = $logSha }
    shutdown        = [ordered]@{ exit_code = $exitCode; seconds = [math]::Round($ranSeconds, 1); timed_out = $timedOut }
    verdict         = $verdict
    reproducibility = 'single-lab: M2 has been proven in one independently constructed lab only'
    limitations     = @(
        'No physical headset: no latency, comfort, distortion or compositor claims.',
        'unique FPS is the emulator''s own rendered-frame series, not compositor cadence.',
        'The save is game/user data; the manifest pins it but the files are not committed.',
        'M2 is not yet reproducible from a second independently constructed lab.'
    )
}
$document | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $Out 'run.json') -Encoding utf8

Write-Output ''
$verdict | ConvertTo-Json -Depth 10
Write-Output "run written: $(Join-Path $Out 'run.json')"
if ($verdict.valid -eq $true) {
    Write-Output ("VALID gameplay cell: unique rendered FPS {0:N2}" -f [double]$verdict.unique_fps)
    exit 0
}
Write-Output 'INVALID: this run is not a gameplay measurement (see verdict.reasons)'
exit 2
