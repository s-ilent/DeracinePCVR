# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# B3 OpenXR-only guarded probe wrapper. This is separate from the legacy headless defaults:
# OpenXR stays enabled, Vulkan is not switched, no OS GUI/window actions are synthesized, and the
# optional existing Move test input is disabled unless explicitly requested. Even when enabled, the
# child waits for both the headset-session and Vulkan HMD-frame log markers before writing input.
#
# A real launch additionally requires an explicit A3 worktree/HEAD/exe-hash handoff, an exact
# implementation-gate pin for the private 1.01 eboot hash, runtime/HMD/Vulkan readiness, and
# -ConfirmOpenXRLaunch. Until those are supplied, this wrapper can only dry-run or report a block.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$SavesFrom = '',
    [Parameter(Mandatory)][string]$Out,
    [string]$A3Worktree = '',
    [string]$A3Branch = '',
    [string]$A3Head = '',
    [string]$A3BuildConfiguration = '',
    [string]$ExpectedExeSha256 = '',
    [ValidateSet('off', 'on')][string]$RhiProbeMode = 'on',
    [string]$ProbeLogSchema = 'DERACINE_RHI_PROBE_INSTALL+DERACINE_RHI_PROBE_EVENT/v2+DERACINE_GNM_DRAW_INDEX',
    [string]$ProbeLogLocation = 'stdout.txt,stderr.txt,lab/user/log/shad_log.txt',
    [string]$OpenXRLaunchPlan = '',
    [string]$ImplementationGateEbootSha256 = '',
    [switch]$StartupMenuOnly,
    [switch]$EnableOpenXRTestInput,
    [switch]$ConfirmOpenXRLaunch,
    [switch]$DryRun,
    [string]$Python = 'python',
    [int]$TimeoutSeconds = 150,
    [int]$ExitAfterSeconds = 120
)

$ErrorActionPreference = 'Stop'
$ProbeControl = if ($RhiProbeMode -eq 'on') { 'SHADPS4_DERACINE_RHI_PROBE=1' } else { 'unset' }
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ShadowRoot = 'D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17'
$SharedGame = 'D:\Temp\Deracine\CUSA11919-qual'
$OriginalImage = 'D:\Temp\Deracine\1.0\Image0'
$OriginalVersionRoot = 'D:\Temp\Deracine\1.0'
$One01Root = 'D:\Temp\Deracine\CUSA11919-1.01'
$PinnedShadowEbootSha256 = '8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75'
$PinnedOriginalEbootSha256 = '8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0'
$Template = Join-Path $Repo 'pc-vr'
$InputScript = Join-Path $Repo 'tools\headless-vr\input\gameplay-entry.txt'
$Profile = Join-Path $Repo 'tools\benchmark\reference\scene-gameplay.json'
$SaveManifest = Join-Path $Repo 'tools\headless-vr\save-slot\MANIFEST.json'
$ChildScript = Join-Path $PSScriptRoot 'headless-gameplay.ps1'
$PreflightScript = Join-Path $PSScriptRoot 'preflight.ps1'
$PostflightScript = Join-Path $PSScriptRoot 'postflight.ps1'
$PathGuardScript = Join-Path $PSScriptRoot 'path_guard.py'
$ProbeParserScript = Join-Path $PSScriptRoot 'openxr_probe_parser.ps1'
$BootstrapGuard = Join-Path $Repo '_out\B3_semantic_capture\provision\guard-roots.json'
$OutFull = [IO.Path]::GetFullPath($Out)
$B3OutputRoot = [IO.Path]::GetFullPath((Join-Path $Repo '_out\B3_semantic_capture'))
$Lab = Join-Path $OutFull 'lab'
$PreflightPath = Join-Path $OutFull 'preflight.json'
$GuardConfigPath = Join-Path $OutFull 'path-guard.json'
$JunctionMapPath = Join-Path $OutFull 'junction-targets.json'
$ReadinessPath = Join-Path $OutFull 'readiness.json'
$PostflightPath = Join-Path $OutFull 'postflight.json'
$ControlPath = Join-Path $OutFull 'control.json'

$preflightCompleted = $false
$guardReady = $false
$preflightExit = $null
$postflightExit = $null
$childExit = $null
$blockedReason = ''
$wrapperError = ''
$runStatus = 'not_started'
$preflightDocument = $null
$readiness = $null
$junctionClosure = $null
$actualEbootSha256 = $null
$actualExeSha256 = $null
$a3BranchActual = $null
$a3HeadActual = $null
$a3WorktreeDirtyLineCount = $null
$a3HandoffIdentityVerified = $false

function Test-PathWithin([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    return $fullPath.Equals($fullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-JsonFile([string]$Path, $Object) {
    $json = $Object | ConvertTo-Json -Depth 24
    [IO.File]::WriteAllText($Path, $json + "`r`n", [System.Text.UTF8Encoding]::new($false))
}

function Invoke-GuardWithConfig([string]$Config, [string]$Operation, [string]$Path) {
    $output = & $Python $PathGuardScript --config $Config --op $Operation --path $Path 2>&1
    $guardExit = $LASTEXITCODE
    if ($guardExit -ne 0) {
        throw "path guard refused '$Operation' on '$Path': $($output -join ' ')"
    }
}

function Invoke-Guard([string]$Operation, [string]$Path) {
    if (-not $guardReady) { throw 'cell path guard is not ready' }
    Invoke-GuardWithConfig -Config $GuardConfigPath -Operation $Operation -Path $Path
}

function Resolve-FinalFilesystemPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    $visited = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    for ($hop = 0; $hop -lt 32; $hop++) {
        if (-not $visited.Add($current)) { throw "reparse-point cycle detected at '$current'" }
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            return [IO.Path]::GetFullPath($item.FullName)
        }
        $targets = @($item.Target)
        if ($targets.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$targets[0])) {
            throw "reparse point has no unique target: '$current'"
        }
        $target = [string]$targets[0]
        if (-not [IO.Path]::IsPathRooted($target)) {
            $target = Join-Path (Split-Path -Parent $current) $target
        }
        $current = [IO.Path]::GetFullPath($target)
    }
    throw "reparse chain exceeds 32 hops: '$Path'"
}

function Get-TreeClosure([string[]]$Seeds, [string[]]$AllowedTargetRoots) {
    $queue = [System.Collections.Generic.Queue[string]]::new()
    foreach ($seed in $Seeds) { $queue.Enqueue([IO.Path]::GetFullPath($seed)) }
    $seenScanRoots = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $treeRootSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $treeRoots = [System.Collections.Generic.List[string]]::new()
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($seed in $Seeds) {
        if (-not (Test-Path -LiteralPath $seed -PathType Container)) { throw "protected tree seed is missing: $seed" }
        $finalSeed = Resolve-FinalFilesystemPath $seed
        if ($treeRootSet.Add($finalSeed)) { $treeRoots.Add($finalSeed) }
    }
    while ($queue.Count -gt 0) {
        $requestedRoot = $queue.Dequeue()
        $scanRoot = Resolve-FinalFilesystemPath $requestedRoot
        if (-not $seenScanRoots.Add($scanRoot)) { continue }
        if (-not (Test-Path -LiteralPath $scanRoot -PathType Container)) {
            throw "resolved junction target is not an accessible directory: $scanRoot"
        }
        foreach ($entry in @(Get-ChildItem -LiteralPath $scanRoot -Recurse -Force -ErrorAction Stop)) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) { continue }
            $targetPath = Resolve-FinalFilesystemPath $entry.FullName
            if (-not (Test-Path -LiteralPath $targetPath -PathType Container)) {
                throw "junction target is not a directory: $($entry.FullName) -> $targetPath"
            }
            $approved = $false
            foreach ($allowedRoot in $AllowedTargetRoots) {
                if (Test-PathWithin $targetPath $allowedRoot) { $approved = $true; break }
            }
            if (-not $approved) { throw "junction target is outside approved protected roots: $($entry.FullName) -> $targetPath" }
            $rawTarget = @($entry.Target)
            $relative = [IO.Path]::GetRelativePath($scanRoot, $entry.FullName)
            $records.Add([ordered]@{
                scan_root = $scanRoot
                relative_path = $relative
                link_path = $entry.FullName
                link_type = [string]$entry.LinkType
                target_raw = if ($rawTarget.Count -gt 0) { [string]$rawTarget[0] } else { '' }
                target_resolved = $targetPath
            })
            if ($treeRootSet.Add($targetPath)) { $treeRoots.Add($targetPath) }
            $queue.Enqueue($targetPath)
        }
    }
    $sortedRoots = @($treeRoots | Sort-Object)
    $sortedRecords = @($records | Sort-Object -Property link_path)
    return [ordered]@{ tree_roots = $sortedRoots; junctions = $sortedRecords }
}

function Assert-SharedJunctionMap([string]$Root) {
    $expected = [System.Collections.Hashtable]::new([System.StringComparer]::OrdinalIgnoreCase)
    $expected['engine'] = 'D:\Temp\Deracine\1.0\Image0\engine'
    $expected['gameproject\Binaries'] = 'D:\Temp\Deracine\1.0\Image0\gameproject\Binaries'
    $expected['gameproject\content'] = 'D:\Temp\Deracine\1.0\Image0\gameproject\content'
    $expected['sce_module'] = 'D:\Temp\Deracine\CUSA11919-1.01\sce_module'
    $expected['sce_sys'] = 'D:\Temp\Deracine\CUSA11919-1.01\sce_sys'
    $actual = [System.Collections.Hashtable]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @(Get-ChildItem -LiteralPath $Root -Recurse -Force -ErrorAction Stop |
            Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })) {
        $relative = [IO.Path]::GetRelativePath($Root, $entry.FullName).Replace('/', '\')
        $actual[$relative] = Resolve-FinalFilesystemPath $entry.FullName
    }
    if ($actual.Count -ne $expected.Count) {
        throw "shared root has $($actual.Count) reparse point(s); expected exactly $($expected.Count) known junctions"
    }
    foreach ($relative in $expected.Keys) {
        if (-not $actual.ContainsKey($relative)) { throw "expected shared junction is missing: $relative" }
        $wanted = [IO.Path]::GetFullPath([string]$expected[$relative])
        $observed = [IO.Path]::GetFullPath([string]$actual[$relative])
        if (-not $observed.Equals($wanted, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "shared junction target mismatch for '$relative': expected '$wanted', observed '$observed'"
        }
    }
    return [ordered]@{
        root = [IO.Path]::GetFullPath($Root)
        count = $actual.Count
        targets = @($expected.Keys | Sort-Object | ForEach-Object {
            [ordered]@{ relative_path = $_; target = [IO.Path]::GetFullPath([string]$actual[$_]) }
        })
    }
}

function Get-ReadinessSnapshot {
    $runtimeCandidates = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @('HKLM:\SOFTWARE\Khronos\OpenXR\1', 'HKCU:\SOFTWARE\Khronos\OpenXR\1')) {
        try {
            $value = (Get-ItemProperty -LiteralPath $key -Name ActiveRuntime -ErrorAction Stop).ActiveRuntime
            if ($value) {
                $runtimeCandidates.Add([ordered]@{
                    registry_key = $key
                    active_runtime = [string]$value
                    exists = (Test-Path -LiteralPath ([string]$value) -PathType Leaf)
                })
            }
        } catch { }
    }
    $activeRuntime = @($runtimeCandidates | Where-Object { $_.exists } | Select-Object -First 1)
    $runtimePath = if ($activeRuntime.Count -gt 0) { [string]$activeRuntime[0].active_runtime } else { '' }
    $vrserver = @(Get-Process -Name 'vrserver' -ErrorAction SilentlyContinue)
    $devices = @()
    $pnpError = ''
    try { $devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop) }
    catch { $pnpError = $_.Exception.Message }
    $hmds = @($devices | Where-Object {
        ([string]$_.InstanceId -match '(?i)VID_B58E&PID_9E84') -or
        ([string]$_.FriendlyName -match '(?i)PSVR|PlayStation VR|HID-compliant headset')
    })
    $controllers = @($devices | Where-Object {
        ([string]$_.Class -eq 'HIDClass') -and ([string]$_.FriendlyName -match '(?i)controller|gamepad|motion')
    })
    $gpuDevices = @()
    $gpuError = ''
    try {
        $gpuDevices = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop | ForEach-Object {
            [ordered]@{ name = [string]$_.Name; driver_version = [string]$_.DriverVersion; status = [string]$_.Status }
        })
    } catch { $gpuError = $_.Exception.Message }
    $vulkanLoader = 'C:\Windows\System32\vulkan-1.dll'
    $vulkanLoaderExists = Test-Path -LiteralPath $vulkanLoader -PathType Leaf
    $vulkanLoaderVersion = if ($vulkanLoaderExists) { (Get-Item -LiteralPath $vulkanLoader).VersionInfo.FileVersion } else { '' }
    $runtimeReady = ($runtimePath -ne '' -and $vrserver.Count -gt 0 -and $hmds.Count -gt 0)
    $vulkanReady = ($vulkanLoaderExists -and $gpuDevices.Count -gt 0)
    return [ordered]@{
        sampled_utc = (Get-Date).ToUniversalTime().ToString('o')
        ready = ($runtimeReady -and $vulkanReady)
        openxr_runtime_candidates = @($runtimeCandidates.ToArray())
        active_runtime_path = $runtimePath
        vrserver_process_count = $vrserver.Count
        hmd_candidate_count = $hmds.Count
        hmd_candidates = @($hmds | ForEach-Object {
            [ordered]@{ name = [string]$_.FriendlyName; class = [string]$_.Class; status = [string]$_.Status }
        })
        controller_candidate_count = $controllers.Count
        controller_candidates = @($controllers | Select-Object -First 12 | ForEach-Object {
            [ordered]@{ name = [string]$_.FriendlyName; status = [string]$_.Status }
        })
        controller_tracking_confirmed = $false
        controller_tracking_note = 'PnP presence is not positive runtime tracking evidence; tracking is not asserted here.'
        vulkan_loader_path = $vulkanLoader
        vulkan_loader_exists = $vulkanLoaderExists
        vulkan_loader_file_version = [string]$vulkanLoaderVersion
        gpu_device_count = $gpuDevices.Count
        gpu_devices = @($gpuDevices)
        vulkan_ready = $vulkanReady
        runtime_ready = $runtimeReady
        pnp_error = $pnpError
        gpu_query_error = $gpuError
        precondition_note = 'loader+GPU presence are a prelaunch check only; an HMD-frame handoff in the emulator log is still required to claim active Vulkan/OpenXR frames.'
    }
}

function Write-ControlReport {
    param([string]$Status, [string]$ErrorText)
    $report = [ordered]@{
        schema_version = 1
        created_utc = (Get-Date).ToUniversalTime().ToString('o')
        status = $Status
        wrapper_error = $ErrorText
        launch_requested = [bool]$ConfirmOpenXRLaunch
        dry_run = [bool]$DryRun
        startup_menu_only = [bool]$StartupMenuOnly
        save_data_dependency = if ($StartupMenuOnly) { 'none' } else { 'manifest_verified' }
        rhi_probe_mode = $RhiProbeMode
        optional_test_input_enabled = [bool]$EnableOpenXRTestInput
        optional_test_input_note = if ($EnableOpenXRTestInput) {
            'Move-only input is gated on headset-session creation and Vulkan HMD-frame log markers.'
        } else {
            'disabled; no scripted input is passed to the emulator'
        }
        eboot = [ordered]@{
            path = (Join-Path $ShadowRoot 'eboot.bin')
            sha256 = $actualEbootSha256
            pinned_sha256 = $PinnedShadowEbootSha256
            implementation_gate_sha256 = $ImplementationGateEbootSha256
        }
        original_1_00_eboot_sha256 = $PinnedOriginalEbootSha256
        executable = [ordered]@{
            path = if (Test-Path -LiteralPath $Exe -PathType Leaf) { [IO.Path]::GetFullPath($Exe) } else { $Exe }
            sha256 = $actualExeSha256
            expected_sha256 = $ExpectedExeSha256
        }
        a3 = [ordered]@{
            worktree = $A3Worktree
            branch_expected = $A3Branch
            branch_observed = $a3BranchActual
            head_expected = $A3Head
            head_observed = $a3HeadActual
            worktree_dirty_status_count = $a3WorktreeDirtyLineCount
            handoff_identity_verified = $a3HandoffIdentityVerified
            build_configuration = $A3BuildConfiguration
            probe_mode = $RhiProbeMode
            probe_control = $ProbeControl
            probe_log_schema = if ($RhiProbeMode -eq 'on') { $ProbeLogSchema } else { $null }
            probe_log_location = $ProbeLogLocation
            openxr_launch_plan = $OpenXRLaunchPlan
        }
        shared_game = $SharedGame
        shared_game_original_1_00 = $OriginalImage
        protected_tree_roots = if ($junctionClosure) { @($junctionClosure.tree_roots) } else { @() }
        junction_map_path = if (Test-Path -LiteralPath $JunctionMapPath) { $JunctionMapPath } else { $null }
        readiness_path = if (Test-Path -LiteralPath $ReadinessPath) { $ReadinessPath } else { $null }
        preflight_path = if (Test-Path -LiteralPath $PreflightPath) { $PreflightPath } else { $null }
        preflight_exit = $preflightExit
        child_exit = $childExit
        postflight_path = if (Test-Path -LiteralPath $PostflightPath) { $PostflightPath } else { $null }
        postflight_exit = $postflightExit
        blocked_reason = $blockedReason
    }
    $configToUse = if ($guardReady) { $GuardConfigPath } else { $BootstrapGuard }
    Invoke-GuardWithConfig -Config $configToUse -Operation 'write' -Path $ControlPath
    Write-JsonFile -Path $ControlPath -Object $report
}

try {
    if (-not (Test-PathWithin $OutFull $B3OutputRoot) -or
        $OutFull.Equals($B3OutputRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "output must be a new child directory of '$B3OutputRoot'"
    }
    if (Test-Path -LiteralPath $OutFull) { throw "output path already exists; refusing to reuse evidence directory: $OutFull" }
    if ($StartupMenuOnly) {
        if (-not [string]::IsNullOrWhiteSpace($SavesFrom)) {
            throw 'startup/menu-only mode refuses a SaveData source path'
        }
        if ($EnableOpenXRTestInput) { throw 'startup/menu-only mode refuses scripted test input' }
    } elseif ([string]::IsNullOrWhiteSpace($SavesFrom)) {
        throw 'saved-game OpenXR mode requires a manifest-verified SavesFrom path'
    }
    $requiredPaths = @($ChildScript, $PreflightScript, $PostflightScript, $PathGuardScript,
                       $ProbeParserScript, $BootstrapGuard, $Template)
    if (-not $StartupMenuOnly) { $requiredPaths += @($InputScript, $Profile, $SaveManifest) }
    foreach ($required in $requiredPaths) {
        if (-not (Test-Path -LiteralPath $required)) { throw "required B3 path is missing: $required" }
    }
    if (-not (Test-Path -LiteralPath $ShadowRoot -PathType Container)) { throw "private shadow is missing: $ShadowRoot" }
    if (-not (Test-Path -LiteralPath $SharedGame -PathType Container)) { throw "required shared-game source is missing: $SharedGame" }
    if (-not (Test-Path -LiteralPath $OriginalImage -PathType Container)) { throw "protected 1.00 source is missing: $OriginalImage" }
    if (-not (Test-Path -LiteralPath (Join-Path $ShadowRoot 'benchmark-shadow.json') -PathType Leaf)) {
        throw 'private shadow manifest is missing'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $ShadowRoot 'eboot.bin') -PathType Leaf)) {
        throw 'private shadow eboot is missing'
    }
    $ExeFull = [IO.Path]::GetFullPath($Exe)
    if (-not (Test-Path -LiteralPath $ExeFull -PathType Leaf)) { throw "executable is missing: $ExeFull" }
    $actualEbootSha256 = Get-Sha256 (Join-Path $ShadowRoot 'eboot.bin')
    $actualExeSha256 = Get-Sha256 $ExeFull
    $handoffSpecified = [bool]($A3Worktree -or $A3Branch -or $A3Head -or $ExpectedExeSha256)
    if ($handoffSpecified) {
        if (-not $A3Worktree -or -not $A3Branch -or -not $A3Head -or -not $ExpectedExeSha256) {
            throw 'A3 handoff identity is partial; worktree, branch, HEAD, and executable SHA-256 are all required'
        }
        if ($ExpectedExeSha256 -cnotmatch '^[0-9a-fA-F]{64}$') {
            throw 'A3 executable SHA-256 handoff must contain exactly 64 hexadecimal characters'
        }
        $a3WorktreeFull = (Resolve-Path -LiteralPath $A3Worktree -ErrorAction Stop).Path
        if (-not (Test-PathWithin $ExeFull $a3WorktreeFull)) {
            throw 'handed-off executable is not inside the declared A3 worktree'
        }
        $a3BranchActual = (& git -C $a3WorktreeFull branch --show-current 2>$null)
        if ($LASTEXITCODE -ne 0) { throw 'could not read the A3 worktree branch' }
        $a3BranchActual = ([string]$a3BranchActual).Trim()
        $a3HeadActual = (& git -C $a3WorktreeFull rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -ne 0) { throw 'could not read the A3 worktree HEAD' }
        $a3HeadActual = ([string]$a3HeadActual).Trim()
        $a3WorktreeDirty = @(& git -C $a3WorktreeFull status --porcelain 2>$null)
        if ($LASTEXITCODE -ne 0) { throw 'could not read A3 worktree status' }
        $a3WorktreeDirtyLineCount = $a3WorktreeDirty.Count
        if (-not $a3BranchActual.Equals($A3Branch.Trim(), [System.StringComparison]::Ordinal)) {
            throw "A3 branch mismatch; expected '$A3Branch' observed '$a3BranchActual'"
        }
        if (-not $a3HeadActual.Equals($A3Head.Trim(), [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "A3 HEAD mismatch; expected $A3Head observed $a3HeadActual"
        }
        if (-not $actualExeSha256.Equals($ExpectedExeSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "A3 executable SHA-256 mismatch; expected $ExpectedExeSha256 observed $actualExeSha256"
        }
        $a3HandoffIdentityVerified = $true
    }
    if (-not $actualEbootSha256.Equals($PinnedShadowEbootSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "shadow eboot hash mismatch; launch is held. actual=$actualEbootSha256 pinned=$PinnedShadowEbootSha256"
    }

    $sharedJunctionMap = Assert-SharedJunctionMap -Root $SharedGame
    $allowedTargets = @($ShadowRoot, $SharedGame, $OriginalImage, $One01Root)
    $junctionClosure = Get-TreeClosure -Seeds @($ShadowRoot, $SharedGame) -AllowedTargetRoots $allowedTargets
    if ($junctionClosure.tree_roots.Count -lt 6) {
        throw "resolved protected tree set has only $($junctionClosure.tree_roots.Count) roots; expected shared root plus five junction targets"
    }

    $bootstrapOut = [IO.Path]::GetFullPath($OutFull)
    Invoke-GuardWithConfig -Config $BootstrapGuard -Operation 'mkdir' -Path $bootstrapOut
    New-Item -ItemType Directory -Force -Path $OutFull | Out-Null
    Invoke-GuardWithConfig -Config $BootstrapGuard -Operation 'write' -Path $PreflightPath
    $protectedTreeJson = ConvertTo-Json -InputObject ([string[]]@($junctionClosure.tree_roots)) -Compress
    $preflightArgs = @(
        '-NoProfile', '-File', $PreflightScript,
        '-Worktree', $Repo,
        '-Out', $PreflightPath,
        '-Game', $ShadowRoot,
        '-SharedGame', $SharedGame,
        '-AdditionalSharedGame', $OriginalImage,
        '-ShadowRoot', $ShadowRoot,
        '-Python', $Python,
        '-ProtectedTreeJson', $protectedTreeJson
    )
    & pwsh @preflightArgs
    $preflightExit = $LASTEXITCODE
    $preflightCompleted = Test-Path -LiteralPath $PreflightPath -PathType Leaf
    if ($preflightExit -ne 0 -or -not $preflightCompleted) {
        throw "preflight failed with exit $preflightExit; launch held"
    }
    $preflightDocument = Get-Content -LiteralPath $PreflightPath -Raw | ConvertFrom-Json

    $guardDeny = [System.Collections.Generic.List[string]]::new()
    foreach ($entry in @($preflightDocument.guard.deny)) {
        foreach ($item in @($entry)) {
            if ($item -and -not $guardDeny.Contains([string]$item)) { $guardDeny.Add([string]$item) }
        }
    }
    foreach ($root in @($junctionClosure.tree_roots) + @($ShadowRoot, $SharedGame, $OriginalVersionRoot, $One01Root)) {
        if (-not $guardDeny.Contains([string]$root)) { $guardDeny.Add([string]$root) }
    }
    $allowReadRoots = @($Repo, $Template, $ShadowRoot, $SharedGame, $OriginalImage, $ExeFull)
    if (-not $StartupMenuOnly) { $allowReadRoots += @($SavesFrom, $InputScript, $Profile, $SaveManifest) }
    $allowReadRoots = @($allowReadRoots | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
    $guardConfig = [ordered]@{
        schema_version = 1
        protected_policy = [string]$preflightDocument.guard.protected_policy
        allow_write = @([IO.Path]::GetFullPath($OutFull), [IO.Path]::GetFullPath($Lab))
        allow_read = $allowReadRoots
        deny = @($guardDeny.ToArray())
    }
    Invoke-GuardWithConfig -Config $BootstrapGuard -Operation 'write' -Path $GuardConfigPath
    Write-JsonFile -Path $GuardConfigPath -Object $guardConfig
    $guardReady = $true

    $expectedRoots = @($junctionClosure.tree_roots | Sort-Object)
    $actualRoots = @($preflightDocument.protected_trees | ForEach-Object { [string]$_.root } | Sort-Object)
    if ($expectedRoots.Count -ne $actualRoots.Count -or
        @($expectedRoots | Where-Object { $_ -notin $actualRoots }).Count -gt 0) {
        throw 'preflight protected-tree roots differ from the discovered junction closure; launch held'
    }
    if (-not ([string]$preflightDocument.protected.shared_game).Equals($SharedGame, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'preflight did not use the required CUSA11919-qual shared-game root'
    }
    if (-not (@($preflightDocument.protected.shared_game_additional) -contains $OriginalImage)) {
        throw 'preflight did not include the protected original 1.00 shared-game root'
    }
    if (-not ([string]$preflightDocument.shadow.eboot_sha256).Equals($PinnedShadowEbootSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'preflight did not record the pinned 1.01 shadow eboot SHA-256'
    }
    $originalEbootPath = Join-Path $OriginalImage 'eboot.bin'
    $originalRecord = $preflightDocument.immutable.PSObject.Properties[$originalEbootPath]
    if ($null -eq $originalRecord -or
        -not ([string]$originalRecord.Value.sha256).Equals($PinnedOriginalEbootSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'preflight did not preserve the exact protected 1.00 eboot SHA-256 record'
    }
    foreach ($root in @($SharedGame, $OriginalImage)) {
        foreach ($relative in @('eboot.bin', 'title.json', 'gameproject\saved\config\ps4\engine.ini')) {
            $recordPath = Join-Path $root $relative
            $record = $preflightDocument.immutable.PSObject.Properties[$recordPath]
            if ($null -eq $record -or -not [bool]$record.Value.exists -or -not $record.Value.sha256) {
                throw "preflight omitted required immutable file evidence: $recordPath"
            }
        }
    }
    $junctionReport = [ordered]@{
        schema_version = 1
        shared_game_expected = $sharedJunctionMap
        recursively_discovered = @($junctionClosure.junctions)
        protected_tree_roots = @($junctionClosure.tree_roots)
        resolved_targets_are_separate_snapshots = $true
        note = 'The snapshot helper records reparse points but never traverses them; every resolved target appears as its own protected tree.'
    }
    Invoke-Guard 'write' $JunctionMapPath
    Write-JsonFile -Path $JunctionMapPath -Object $junctionReport

    $readiness = Get-ReadinessSnapshot
    Invoke-Guard 'write' $ReadinessPath
    Write-JsonFile -Path $ReadinessPath -Object $readiness

    $runStatus = 'blocked_launch_held'
    if ($DryRun) {
        $runStatus = 'dry_run'
        $childArgs = @(
            '-NoProfile', '-File', $ChildScript,
            '-Exe', $ExeFull,
            '-Eboot', (Join-Path $ShadowRoot 'eboot.bin'),
            '-Out', $OutFull,
            '-Lab', $Lab,
            '-Template', $Template,
            '-TimeoutSeconds', "$TimeoutSeconds",
            '-ExitAfterSeconds', "$ExitAfterSeconds",
            '-MaxInputInjections', $(if ($StartupMenuOnly) { '0' } else { '1' }),
            '-RhiProbeMode', $RhiProbeMode,
            '-OpenXR',
            '-OpenXRPreflight', $PreflightPath,
            '-OpenXRGuardConfig', $GuardConfigPath,
            '-ShadowRoot', $ShadowRoot,
            '-DryRun'
        )
        if ($StartupMenuOnly) {
            $childArgs += @('-OpenXRStartupMenuOnly', '-NoMenuGatedInput')
        } else {
            $childArgs += @('-SavesFrom', $SavesFrom, '-InputScript', $InputScript,
                            '-Profile', $Profile, '-SaveManifest', $SaveManifest)
        }
        if ($EnableOpenXRTestInput) { $childArgs += '-EnableOpenXRTestInput' }
        if ($Python) { $childArgs += @('-Python', $Python) }
        & pwsh @childArgs
        $childExit = $LASTEXITCODE
        if ($childExit -ne 0) { $blockedReason = "OpenXR dry-run child returned $childExit" }
    } else {
        if (-not $ConfirmOpenXRLaunch) {
            $blockedReason = 'launch held: explicit -ConfirmOpenXRLaunch is required'
        } elseif (-not $readiness.ready) {
            $blockedReason = 'launch held: OpenXR runtime, HMD, Vulkan loader, and GPU preconditions are not all present'
            $runStatus = 'blocked_preconditions'
        } elseif (-not $A3Worktree -or -not $A3Branch -or -not $A3Head -or
                  $A3BuildConfiguration -cne 'Release' -or -not $ExpectedExeSha256 -or
                  -not $ImplementationGateEbootSha256 -or -not $OpenXRLaunchPlan -or -not $a3HandoffIdentityVerified) {
            $blockedReason = 'launch held: awaiting the complete A3 path/SHA/HEAD/build, probe, launch-plan, and implementation-gate handoff'
        } elseif (($RhiProbeMode -eq 'on' -and
                   ($ProbeControl -cne 'SHADPS4_DERACINE_RHI_PROBE=1' -or
                    $ProbeLogSchema -cne 'DERACINE_RHI_PROBE_INSTALL+DERACINE_RHI_PROBE_EVENT/v2+DERACINE_GNM_DRAW_INDEX')) -or
                  ($RhiProbeMode -eq 'off' -and $ProbeControl -cne 'unset') -or
                  $ProbeLogLocation -cne 'stdout.txt,stderr.txt,lab/user/log/shad_log.txt') {
            $blockedReason = 'launch held: A3 probe mode/schema/log sinks differ from the verified contract'
        } else {
            $a3WorktreeFull = (Resolve-Path -LiteralPath $A3Worktree -ErrorAction Stop).Path
            if (-not (Test-PathWithin $ExeFull $a3WorktreeFull)) {
                $blockedReason = 'launch held: executable is not inside the handed-off A3 worktree'
            } else {
                   $a3BranchActual = (& git -C $a3WorktreeFull branch --show-current 2>$null)
                $a3BranchActual = ([string]$a3BranchActual).Trim()
                $a3HeadActual = (& git -C $a3WorktreeFull rev-parse HEAD 2>$null)
                $a3HeadActual = ([string]$a3HeadActual).Trim()
                $actualExeSha256 = Get-Sha256 $ExeFull
                if (-not $a3BranchActual.Equals($A3Branch.Trim(), [System.StringComparison]::Ordinal)) {
                    $blockedReason = "launch held: A3 branch mismatch; expected $A3Branch observed $a3BranchActual"
                } elseif (-not $a3HeadActual.Equals($A3Head.Trim(), [System.StringComparison]::OrdinalIgnoreCase)) {
                    $blockedReason = "launch held: A3 HEAD mismatch; expected $A3Head observed $a3HeadActual"
                } elseif (-not $actualExeSha256.Equals($ExpectedExeSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $blockedReason = "launch held: A3 executable SHA-256 mismatch; expected $ExpectedExeSha256 observed $actualExeSha256"
                } elseif (-not $ImplementationGateEbootSha256.Equals($PinnedShadowEbootSha256, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $blockedReason = 'launch held: A3 implementation gate has not explicitly pinned the current 1.01 shadow eboot SHA-256'
                } else {
                    # Close the enumeration/preflight gap; any changed junction mapping aborts before copy or launch.
                    $closureNow = Get-TreeClosure -Seeds @($ShadowRoot, $SharedGame) -AllowedTargetRoots $allowedTargets
                    $newJunctionJson = @($closureNow.junctions | ConvertTo-Json -Depth 12 -Compress)
                    $oldJunctionJson = @($junctionClosure.junctions | ConvertTo-Json -Depth 12 -Compress)
                    if (($closureNow.tree_roots -join "`n") -cne ($junctionClosure.tree_roots -join "`n") -or
                        ($newJunctionJson -join '') -cne ($oldJunctionJson -join '')) {
                        $blockedReason = 'launch held: reparse-point targets changed after preflight'
                    } else {
                        $runStatus = 'launching_openxr'
                        $childArgs = @(
                            '-NoProfile', '-File', $ChildScript,
                            '-Exe', $ExeFull,
                            '-Eboot', (Join-Path $ShadowRoot 'eboot.bin'),
                            '-Out', $OutFull,
                            '-Lab', $Lab,
                            '-Template', $Template,
                            '-TimeoutSeconds', "$TimeoutSeconds",
                            '-ExitAfterSeconds', "$ExitAfterSeconds",
                            '-MaxInputInjections', $(if ($StartupMenuOnly) { '0' } else { '1' }),
                            '-RhiProbeMode', $RhiProbeMode,
                            '-OpenXR',
                            '-OpenXRPreflight', $PreflightPath,
                            '-OpenXRGuardConfig', $GuardConfigPath,
                            '-ShadowRoot', $ShadowRoot
                        )
                        if ($StartupMenuOnly) {
                            $childArgs += @('-OpenXRStartupMenuOnly', '-NoMenuGatedInput')
                        } else {
                            $childArgs += @('-SavesFrom', $SavesFrom, '-InputScript', $InputScript,
                                            '-Profile', $Profile, '-SaveManifest', $SaveManifest)
                        }
                        if ($EnableOpenXRTestInput) { $childArgs += '-EnableOpenXRTestInput' }
                        if ($Python) { $childArgs += @('-Python', $Python) }
                        & pwsh @childArgs
                        $childExit = $LASTEXITCODE
                        $runStatus = if ($childExit -eq 0) { 'completed' } else { 'child_returned_nonzero' }
                    }
                }
            }
        }
    }
} catch {
    $wrapperError = $_.Exception.Message
    $runStatus = 'error'
    Write-Output "openxr-gameplay: $wrapperError"
} finally {
    if ($preflightCompleted) {
        try {
            $configToUse = if ($guardReady) { $GuardConfigPath } else { $BootstrapGuard }
            Invoke-GuardWithConfig -Config $configToUse -Operation 'write' -Path $PostflightPath
            & pwsh -NoProfile -File $PostflightScript -Before $PreflightPath -Out $PostflightPath
            $postflightExit = $LASTEXITCODE
            if ($postflightExit -ne 0) {
                $runStatus = 'postflight_failed'
                if (-not $blockedReason) { $blockedReason = "postflight returned $postflightExit" }
            }
        } catch {
            $postflightExit = 1
            $runStatus = 'postflight_error'
            if (-not $blockedReason) { $blockedReason = $_.Exception.Message }
            Write-Output "openxr-gameplay postflight error: $($_.Exception.Message)"
        }
        try { Write-ControlReport -Status $runStatus -ErrorText $wrapperError }
        catch { Write-Output "openxr-gameplay control report error: $($_.Exception.Message)" }
    }
}

if ($wrapperError) { exit 1 }
if (-not $preflightCompleted) { exit 1 }
if ($null -ne $postflightExit -and $postflightExit -ne 0) { exit 1 }
if ($blockedReason) {
    Write-Output "OpenXR probe not launched: $blockedReason"
    exit 3
}
if ($null -ne $childExit) { exit $childExit }
exit 0
