# Headless VR launch — milestone 1
#
# Boots Deracine in emulated-PSVR mode with NO OpenXR host and NO physical headset, in a private
# portable lab, and proves the front end was reached:
#   - SHADPS4_VR=1 SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0
#   - no UpdateWorn line (nothing can take the headset off)
#   - both eyes at 2880x3240 from the guest's own 5760x3240 eye picture
#   - a heavy front-end frame-stats phase (draws/Mpx recorded, never used as a gameplay claim)
#
# It does not move the Move controller and does not enter gameplay: that is milestone 2
# (scripted Move movement and inputs), see _build/team/HEADLESS-ONLY.md.
#
# Run it inside the machine-wide GPU lock:
#   bash _build/team/gpu-lock.sh run --owner me --kind headless --reason "headless M1 launch" \
#        --ttl 900 --require-quiet -- pwsh.exe -NoProfile -File \
#        M:/Development/Deracine-work/_build/headless/headless-vr-launch.ps1 -Exe M:/.../shadps4.exe
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$Label = 'm1-launch',
    [string]$Lab = '',
    [string]$Eboot = 'D:\Temp\Deracine\CUSA11919-qual\eboot.bin',
    [string]$TemplateLab = 'M:\Development\Deracine-work\lab\pc-vr-quality',
    [string]$SavesFrom = 'M:\Development\Deracine-work\_run\pc-vr-recycler-try\user\home\1000\savedata\CUSA11919',
    [int]$Seconds = 100,
    [int]$DumpWidth = 1920,
    # Guest HMD frame to dump as evidence; 0 = no dump (the M1 proof is the log + result.json).
    # Eye captures for deliverables are taken scene-gated on the M2/M3 build.
    [int]$DumpFrame = 0,
    [int]$ExpectEyeWidth = 2880,
    [int]$ExpectEyeHeight = 3240,
    [int]$HeavyDraws = 400,
    [double]$HeavyMpx = 450,
    # The known guest-memory startup crash ("address_space.cpp Protect: Unreachable code!")
    # kills a run at random before the front end; retry a fresh session for it.
    [int]$Attempts = 3,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
$Root = 'M:\Development\Deracine-work'
$RunDir = Join-Path $Root "_build\headless\runs\$Label"
if ($Lab -eq '') { $Lab = Join-Path $Root "_build\headless\labs\$Label" }
New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

if (-not (Test-Path $Exe)) { throw "executable not found: $Exe" }
if (-not (Test-Path $Eboot)) { throw "eboot not found: $Eboot" }

# --- isolated lab -----------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $Lab 'user\config.json'))) {
    Write-Host "creating private lab $Lab from $TemplateLab"
    if (-not (Test-Path $TemplateLab)) { throw "template lab not found: $TemplateLab" }
    New-Item -ItemType Directory -Force -Path $Lab | Out-Null
    Copy-Item (Join-Path $TemplateLab '*') $Lab -Recurse -Force -Exclude 'log', 'temp', 'rt_dump*'
    $srcSave = Join-Path $SavesFrom 'SaveData'
    if (Test-Path $srcSave) {
        $dstSave = Join-Path $Lab 'user\home\1000\savedata\CUSA11919\SaveData'
        New-Item -ItemType Directory -Force -Path $dstSave | Out-Null
        Copy-Item (Join-Path $srcSave '*') $dstSave -Recurse -Force
    }
}
Copy-Item $Exe (Join-Path $Lab 'shadPS4.exe') -Force
New-Item -ItemType Directory -Force -Path (Join-Path $Lab 'user\log') | Out-Null
Remove-Item (Join-Path $Lab 'user\log\*') -Force -ErrorAction SilentlyContinue
Get-ChildItem $Lab -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

# Lossless log tail; the benchmark's GPU settings.
$cfgPath = Join-Path $Lab 'user\config.json'
$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
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

# --- environment: headless only ---------------------------------------------------------------
$envMap = [ordered]@{
    SHADPS4_VR                 = '1'
    SHADPS4_OPENXR             = '0'
    SHADPS4_VR_DEMO            = '0'
    SHADPS4_TITLE_EYE_WIDTH    = "$ExpectEyeWidth"
    SHADPS4_TITLE_RESOLUTION   = '6'
    SHADPS4_VR_SCALE           = '0.5'
    SHADPS4_VR_DESKTOP_VIEW    = 'stereo'
    SHADPS4_VR_DESKTOP_CROP    = '0'
    SHADPS4_VR_SHARPEN         = '0.25'
    SHADPS4_FORCE_ANISO        = '16'
    SHADPS4_MAX_MSAA           = '2'
    SHADPS4_VR_FOV             = '100'
    SHADPS4_VR_REFRESH_RATE    = '120'
    SHADPS4_VR_FPS_CAP         = '120'
    SHADPS4_VR_FASTEST_PACE    = '1'
    SHADPS4_XR_PAUSE           = '0'
    SHADPS4_CONSOLE_LANGUAGE   = 'en-US'
    SHADPS4_FRAME_STATS        = '1'
    SHADPS4_FRAME_STATS_EVERY  = '1'
    SHADPS4_DUMP_WIDTH         = "$DumpWidth"
    # The verified recipe for reaching the classroom front end: the existing Move autopress
    # advances the controller-check screen (the title is Move-driven; this is not new support).
    SHADPS4_DBG_AUTOPRESS      = '1'
    SHADPS4_DBG_AUTOPRESS_TO   = "$([int]([math]::Max(30, $Seconds - 15) * 1000))"
    SHADPS4_DBG_AUTOPRESS_MENU = '1'
}
if ($DumpFrame -gt 0) { $envMap['SHADPS4_DBG_DUMP_HMD_FRAMES'] = "$DumpFrame" }
foreach ($k in $envMap.Keys) { Set-Item -Path "Env:$k" -Value $envMap[$k] }
foreach ($k in @('SHADPS4_INPUT_SCRIPT', 'SHADPS4_LIVE_INPUT', 'SHADPS4_DBG_HAND_POSE',
                 'SHADPS4_VR_HANDS', 'SHADPS4_VR_MOVE_FROM_PAD', 'SHADPS4_EXIT_AFTER',
                 'SHADPS4_CLEAN_EXIT')) {
    Remove-Item "Env:$k" -ErrorAction SilentlyContinue
}

$exeSha = (Get-FileHash -Algorithm SHA256 $Exe).Hash.ToUpperInvariant()
$started = Get-Date
$stdout = Join-Path $RunDir 'stdout.txt'
$stderr = Join-Path $RunDir 'stderr.txt'
Write-Host "launching headless: $Exe"
$p = Start-Process -FilePath (Join-Path $Lab 'shadPS4.exe') -WorkingDirectory $Lab `
    -ArgumentList @('-g', $Eboot) -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$log = Join-Path $Lab 'user\log\shad_log.txt'

# --- watch the log for the front-end proof ----------------------------------------------------
$eyeLine = $null
$titleDraws = $null
$hmdLines = New-Object System.Collections.Generic.List[string]
$samples = New-Object System.Collections.Generic.List[object]
$wornLines = New-Object System.Collections.Generic.List[string]
$seen = New-Object System.Collections.Generic.HashSet[string]
$deadline = (Get-Date).AddSeconds($Seconds)
$done = $false
$exitedEarly = $false
while (-not $done -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 700
    if ($p.HasExited) { Write-Host "emulator exited early: $($p.ExitCode)"; $exitedEarly = $true; break }
    if (-not (Test-Path $log)) { continue }
    foreach ($line in (Get-Content $log -Tail 500 -ErrorAction SilentlyContinue)) {
        if (-not $seen.Add($line)) { continue }
        if ($line -match 'UpdateWorn|The headset is off the head') { $wornLines.Add($line) | Out-Null }
        if ($line -match 'PrepareHmdFrame: HMD frame (\d+) eye (\d)') { $hmdLines.Add($line) | Out-Null }
        if ($line -match 'PrepareHmdFrame: VR: the title draws (\d+)x(\d+) an eye') {
            $titleDraws = [pscustomobject]@{ W = [int]$Matches[1]; H = [int]$Matches[2] }
        }
        if ($line -match 'frame stats at [\d.]+ s: ([\d.]+) fps, per frame ([\d.]+) passes \(([\d.]+) loading their targets, ([\d.]+) Mpx\), (\d+) draws') {
            $samples.Add([pscustomobject]@{
                fps = [double]$Matches[1]; passes = [double]$Matches[2]; mpx = [double]$Matches[4]
                draws = [int]$Matches[5] }) | Out-Null
        }
    }
    $heavy = @($samples | Where-Object { $_.draws -ge $HeavyDraws })
    $eyes = @($hmdLines | Where-Object { $_ -match "eye 0" }).Count
    if ($null -ne $titleDraws -and $eyes -ge 2 -and $heavy.Count -ge 3) { $done = $true }
}

if (-not $p.HasExited -and -not $KeepRunning) {
    # The eye dump is requested at a fixed guest frame; give it a bounded moment to land before
    # the session is stopped, so the M1 evidence includes the picture.
    if ($done -and $DumpFrame -gt 0) {
        $waitUntil = (Get-Date).AddSeconds(45)
        while ((Get-Date) -lt $waitUntil) {
            $have = Get-ChildItem $Lab -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
                ForEach-Object { Get-ChildItem $_.FullName -Recurse -Filter '*5760x3240_R8G8B8A8Unorm*.png' -ErrorAction SilentlyContinue |
                    Select-Object -First 1 } | Select-Object -First 1
            if ($have) { Start-Sleep -Seconds 2; break }
            if ($p.HasExited) { break }
            Start-Sleep -Milliseconds 500
        }
    }
    $stoppedByUs = $true
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
$shutdown = if ($stoppedByUs) { [pscustomobject]@{ kind = 'hard_stop'; code = $null } }
            elseif ($p.HasExited) { [pscustomobject]@{ kind = 'exited'; code = $p.ExitCode } }
            else { [pscustomobject]@{ kind = 'left_running'; code = $null } }

# --- result -----------------------------------------------------------------------------------
$lastHeavy = @($samples | Where-Object { $_.draws -ge $HeavyDraws } | Select-Object -Last 20)
$mean = { param($v) if (@($v).Count -eq 0) { $null } else { [math]::Round((@($v) | Measure-Object -Average).Average, 3) } }
$eyeOk = ($null -ne $titleDraws) -and ($titleDraws.W -eq $ExpectEyeWidth) -and ($titleDraws.H -eq $ExpectEyeHeight)
$valid = $done -and $eyeOk -and ($wornLines.Count -eq 0) -and ($lastHeavy.Count -ge 1)

# The known guest-memory startup crash is random; retry a fresh session when the run died
# before the front end was reached.
if (-not $valid -and -not $KeepRunning -and $Attempts -gt 1 -and $exitedEarly -and -not $done) {
    $next = "$Label-r$Attempts"
    Write-Host "startup crash (exit $($p.ExitCode)); retrying as $next ($($Attempts - 1) left)"
    & $PSCommandPath -Exe $Exe -Label $next -Lab (Join-Path $Root "_build\headless\labs\$next") `
        -Eboot $Eboot -Seconds $Seconds -DumpWidth $DumpWidth -DumpFrame $DumpFrame `
        -Attempts ($Attempts - 1) -ExpectEyeWidth $ExpectEyeWidth -ExpectEyeHeight $ExpectEyeHeight `
        -HeavyDraws $HeavyDraws -HeavyMpx $HeavyMpx
    exit $LASTEXITCODE
}

$framesDir = Join-Path $RunDir 'frames'
New-Item -ItemType Directory -Force -Path $framesDir | Out-Null
$eyeFile = $null
if ($DumpFrame -gt 0) {
    $png = Get-ChildItem $Lab -Directory -Filter 'rt_dump*' -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Recurse -Filter '*5760x3240_R8G8B8A8Unorm*.png' -ErrorAction SilentlyContinue } |
        Sort-Object Length -Descending | Select-Object -First 1
    if ($png) {
        $eyeFile = Join-Path $framesDir 'front-end-eye.png'
        Copy-Item $png.FullName $eyeFile -Force
    }
}

$result = [ordered]@{
    milestone      = 'M1-headless-launch'
    valid          = [bool]$valid
    label          = $Label
    exe            = $Exe
    exe_sha256     = $exeSha
    lab            = $Lab
    eboot          = $Eboot
    started        = $started.ToString('o')
    seconds        = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    headless       = [ordered]@{ vr = 1; openxr = 0; demo_motion = 0; physical_headset = $false }
    eye            = [ordered]@{
        width = $(if ($titleDraws) { $titleDraws.W } else { $null })
        height = $(if ($titleDraws) { $titleDraws.H } else { $null })
        expect_width = $ExpectEyeWidth; expect_height = $ExpectEyeHeight
        image = '5760x3240 R8G8B8A8Unorm samples 1'
        hmd_frame_lines = $hmdLines.Count
        ok = [bool]$eyeOk
    }
    front_end      = [ordered]@{
        reached = [bool]$done
        heavy_windows = $lastHeavy.Count
        draws_mean = (& $mean ($lastHeavy | ForEach-Object { $_.draws }))
        mpx_mean = (& $mean ($lastHeavy | ForEach-Object { $_.mpx }))
        passes_mean = (& $mean ($lastHeavy | ForEach-Object { $_.passes }))
        fps_mean = (& $mean ($lastHeavy | ForEach-Object { $_.fps }))
    }
    worn_lines     = $wornLines.Count
    gameplay       = $false
    note           = 'Front end only. Menu/gameplay is milestone 2 (scripted Move movement and inputs).'
    shutdown       = $shutdown
    log            = $log
    eye_png        = $eyeFile
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $RunDir 'result.json') -Encoding utf8
$envMap.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" } |
    Set-Content -LiteralPath (Join-Path $RunDir 'env.txt') -Encoding utf8
Copy-Item $log (Join-Path $RunDir 'shad_log.txt') -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '=== M1 headless launch ==='
Write-Host ("valid: {0}  eye: {1}x{2}  hmd frames: {3}  worn lines: {4}" -f $valid,
    $titleDraws.W, $titleDraws.H, $hmdLines.Count, $wornLines.Count)
if ($lastHeavy.Count -gt 0) {
    Write-Host ("front end: draws {0}  mpx {1}  passes {2}  fps {3}" -f `
        (& $mean ($lastHeavy | ForEach-Object { $_.draws })), `
        (& $mean ($lastHeavy | ForEach-Object { $_.mpx })), `
        (& $mean ($lastHeavy | ForEach-Object { $_.passes })), `
        (& $mean ($lastHeavy | ForEach-Object { $_.fps })))
}
Write-Host "result: $(Join-Path $RunDir 'result.json')"
exit $(if ($valid) { 0 } else { 2 })
