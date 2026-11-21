# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Read-only preflight: snapshot the protected installation, the shared harness and the evidence
# store before a benchmark session. It writes only inside the private output root.
#
# The snapshot is detection, not prevention: this process holds the same Windows token as every
# other process, so it cannot stop a write. It makes an unexpected write visible, and
# postflight.ps1 refuses to report success when one happened.
#
# Optional game-shadow proof (the runner passes both once it has a declared private shadow):
#   -ShadowRoot <dir>       the declared private game shadow root; game_shadow.py must accept it
#                           against $Game before any protected file is hashed.
#   -ShadowManifest <file>  the declaration itself; may be given instead of -ShadowRoot
#                           (the root is then its parent). It must be named benchmark-shadow.json
#                           and sit directly in the root.
#   -AdditionalSharedGame <dir> repeats the shared-game file hashes for another protected source.
#   -ProtectedTree <dir[]>  recursively hashes files and records directories/reparse points without
#                           following links; pass every resolved junction target as its own tree.
#   -ProtectedTreeJson <json> transports the same string[] through a native PowerShell child process.
#   -Python <exe>           interpreter for game_shadow.py (standard library only; default python).
# When neither shadow parameter is given the preflight behaves exactly as before. When either is
# given, an invalid shadow is a terminating error (non-zero exit): the session must not write
# <Game>\gameproject\saved\config\ps4\engine.ini in a directory it cannot prove is private.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Worktree,
    [Parameter(Mandatory)][string]$Out,
    [string]$Repo = 'M:\Development\Deracine-work\DeracinePCVR',
    [string]$Recovery = 'M:\Development\Deracine-recovery-2026-11-17',
    [string]$SharedBench = 'M:\Development\Deracine-work\_build\bench',
    [string]$SharedLab = 'M:\Development\Deracine-work\lab\pc-vr-quality',
    [string]$Game = 'D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17',
    [string]$SharedGame = 'D:\Temp\Deracine\CUSA11919-qual',
    [string[]]$AdditionalSharedGame = @(),
    [string[]]$ProtectedTree = @(),
    [string]$ProtectedTreeJson = '',
    [string]$ShadowRoot = '',
    [string]$ShadowManifest = '',
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
if (-not [string]::IsNullOrWhiteSpace($ProtectedTreeJson)) {
    if (@($ProtectedTree).Count -gt 0) { throw 'pass either ProtectedTree or ProtectedTreeJson, not both' }
    try { $decodedProtectedTrees = @(ConvertFrom-Json -InputObject $ProtectedTreeJson -ErrorAction Stop) }
    catch { throw "ProtectedTreeJson must be a JSON array of paths: $($_.Exception.Message)" }
    if ($decodedProtectedTrees.Count -eq 0 -or
        @($decodedProtectedTrees | Where-Object { $_ -isnot [string] -or [string]::IsNullOrWhiteSpace([string]$_) }).Count -gt 0) {
        throw 'ProtectedTreeJson must contain one or more non-empty string paths'
    }
    $ProtectedTree = @($decodedProtectedTrees)
}
$inv = [System.Globalization.CultureInfo]::InvariantCulture
. (Join-Path $PSScriptRoot 'protected_tree_snapshot.ps1')

function Write-JsonFile([string]$Path, $Object) {
    $json = $Object | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText($Path, $json + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FileRecord([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return [ordered]@{ exists = $false; bytes = $null; sha256 = $null }
    }
    $item = Get-Item -LiteralPath $Path -Force
    return [ordered]@{
        exists = $true
        bytes  = $item.Length
        sha256 = Get-Sha256 $Path
    }
}

function Get-DirectoryRecord([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return [ordered]@{ exists = $false; files = 0; bytes = 0 }
    }
    $files = Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue
    $sum = ($files | Measure-Object -Property Length -Sum).Sum
    return [ordered]@{ exists = $true; files = @($files).Count; bytes = [long]($sum) }
}

function Get-GitState([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return [ordered]@{ present = $false }
    }
    $head = (git -C $Path rev-parse HEAD 2>$null)
    $branch = (git -C $Path rev-parse --abbrev-ref HEAD 2>$null)
    $porcelain = @(git -C $Path status --porcelain 2>$null)
    return [ordered]@{
        present       = $true
        head          = if ($head) { $head.Trim() } else { $null }
        branch        = if ($branch) { $branch.Trim() } else { $null }
        dirty_count   = $porcelain.Count
        dirty_sha256  = if ($porcelain.Count -gt 0) {
            $bytes = [Text.Encoding]::UTF8.GetBytes(($porcelain -join "`n"))
            ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
        } else { $null }
    }
}

function Normalize-Path([string]$Path) {
    return ([IO.Path]::GetFullPath($Path) -replace '/', '\').TrimEnd('\')
}

# The output root must be private: inside the assigned worktree's _out, or the private test root.
$outFull = [IO.Path]::GetFullPath($Out)
$allowOut = [IO.Path]::GetFullPath((Join-Path $Worktree '_out'))
if (-not ($outFull -eq $allowOut -or $outFull.StartsWith($allowOut + [IO.Path]::DirectorySeparatorChar))) {
    throw "preflight output '$outFull' is not inside the private root '$allowOut'"
}
New-Item -ItemType Directory -Force -Path (Split-Path $outFull) | Out-Null

$worktreeFull = Normalize-Path $Worktree
$repoFull = Normalize-Path $Repo
if ($worktreeFull.TrimEnd('\') -eq $repoFull.TrimEnd('\')) {
    throw "the active runtime repository cannot be the benchmark worktree"
}

# Prove the game shadow before hashing anything: the session rewrites engine.ini inside $Game, so
# a $Game that is really the active installation or a shared copy must fail here, not afterwards.
$shadow = [ordered]@{ requested = $false }
if ($ShadowRoot -or $ShadowManifest) {
    if (-not $ShadowRoot) {
        $ShadowManifest = [IO.Path]::GetFullPath($ShadowManifest)
        $ShadowRoot = Split-Path -Parent $ShadowManifest
    }
    $ShadowRoot = [IO.Path]::GetFullPath($ShadowRoot)
    if (-not $ShadowManifest) { $ShadowManifest = Join-Path $ShadowRoot 'benchmark-shadow.json' }
    $ShadowManifest = [IO.Path]::GetFullPath($ShadowManifest)
    if ((Split-Path -Leaf $ShadowManifest) -ne 'benchmark-shadow.json') {
        throw "the shadow manifest must be named benchmark-shadow.json: $ShadowManifest"
    }
    $shadowParent = (Split-Path -Parent $ShadowManifest).TrimEnd('\')
    if ($shadowParent -ne $ShadowRoot.TrimEnd('\')) {
        throw "the shadow manifest '$ShadowManifest' is not directly inside '$ShadowRoot'"
    }

    $shadowGame = [IO.Path]::GetFullPath($Game)
    $shadowEboot = Join-Path $shadowGame 'eboot.bin'
    $shadowOutput = & $Python (Join-Path $PSScriptRoot 'game_shadow.py') check `
        --game $shadowGame --shadow-root $ShadowRoot --eboot $shadowEboot --json 2>&1
    $shadowExit = $LASTEXITCODE
    $shadowDoc = $null
    try { $shadowDoc = ($shadowOutput -join "`n") | ConvertFrom-Json } catch { }
    if ($shadowExit -ne 0 -or $null -eq $shadowDoc -or -not [bool]$shadowDoc.valid) {
        $shadowReasons = if ($null -ne $shadowDoc -and $shadowDoc.reasons) {
            @($shadowDoc.reasons) -join '; '
        } else {
            ($shadowOutput -join ' ')
        }
        throw "game shadow '$ShadowRoot' is not valid (exit $shadowExit): $shadowReasons"
    }
    $shadow = [ordered]@{
        requested     = $true
        checker       = (Join-Path $PSScriptRoot 'game_shadow.py')
        root          = $shadowDoc.shadow_root
        game_realpath = $shadowDoc.game_realpath
        manifest_path = $shadowDoc.manifest_path
        manifest      = $shadowDoc.manifest
        eboot_sha256  = $shadowDoc.eboot_sha256
        engine_ini    = $shadowDoc.engine_ini
        policy        = $shadowDoc.policy
    }
}

# Immutable protected files: hash everything whose change would mean the session damaged the
# user's installation, a shared harness or the recovery evidence.
$immutablePaths = [ordered]@{}
$repoFiles = @(
    'pc-vr\shadps4.exe',
    'pc-vr\launch.ps1',
    'pc-vr\settings.txt',
    'pc-vr\user\config.json',
    'pc-vr\user\users.json',
    'pc-vr\user\patches\CUSA11919.xml',
    'Play Deracine VR.bat'
)
foreach ($relative in $repoFiles) {
    $immutablePaths[(Join-Path $Repo $relative)] = Get-FileRecord (Join-Path $Repo $relative)
}
foreach ($relative in @('pc-vr\shadps4.exe.before-r58', 'pc-vr\shadps4.v0.21.0-bundled.bak',
                        'pc-vr\render-config.ps1')) {
    $immutablePaths[(Join-Path $Repo $relative)] = Get-FileRecord (Join-Path $Repo $relative)
}
$recoveryFiles = @(
    'manifest\MANIFEST.csv',
    'bench\bench.ps1',
    'artifacts\rdc-frame532-window.png',
    'artifacts\bin\recovery-baseline-acda49e.exe',
    'artifacts\bin\recovery-renderdoc.exe'
)
foreach ($relative in $recoveryFiles) {
    $immutablePaths[(Join-Path $Recovery $relative)] = Get-FileRecord (Join-Path $Recovery $relative)
}
Get-ChildItem -LiteralPath (Join-Path $Recovery 'reports') -File -ErrorAction SilentlyContinue |
    ForEach-Object { $immutablePaths[$_.FullName] = Get-FileRecord $_.FullName }

foreach ($relative in @('bench.ps1', 'README.md', 'bin\baseline.json')) {
    $immutablePaths[(Join-Path $SharedBench $relative)] = Get-FileRecord (Join-Path $SharedBench $relative)
}
foreach ($relative in @('settings.txt', 'launch.ps1', 'shadps4.exe')) {
    $immutablePaths[(Join-Path $SharedLab $relative)] = Get-FileRecord (Join-Path $SharedLab $relative)
}
$sharedGameRoots = @($SharedGame) + @($AdditionalSharedGame)
foreach ($sharedRoot in $sharedGameRoots) {
    foreach ($relative in @('eboot.bin', 'title.json', 'gameproject\saved\config\ps4\engine.ini')) {
        $path = Join-Path $sharedRoot $relative
        $immutablePaths[$path] = Get-FileRecord $path
    }
}
$protectedTreeRecords = [System.Collections.Generic.List[object]]::new()
$protectedTreeRoots = [System.Collections.Generic.List[string]]::new()
$seenProtectedTreeRoots = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($treePath in $ProtectedTree) {
    $snapshot = Get-ProtectedTreeSnapshot -Path $treePath
    if (-not $seenProtectedTreeRoots.Add([string]$snapshot.root)) {
        throw "duplicate protected tree root: $($snapshot.root)"
    }
    $protectedTreeRecords.Add($snapshot)
    $protectedTreeRoots.Add([string]$snapshot.root)
}
$immutablePaths[[string](Join-Path $Game 'eboot.bin')] = Get-FileRecord (Join-Path $Game 'eboot.bin')
# The private shadow's engine.ini is this harness's own input, rewritten per session; hashing it
# would make a fresh shadow fail postflight. The shared copy above stays protected.

# Volatile directories: a running user session legitimately writes logs, caches and screenshots.
# Their counts are recorded so a change can be told apart from a changed binary; they never fail.
$volatile = [ordered]@{}
foreach ($relative in @('pc-vr\user\log', 'pc-vr\user\cache', 'pc-vr\user\screenshots',
                        'pc-vr\user\crash_dumps', 'pc-vr\user\rt_dump')) {
    $volatile[$relative] = Get-DirectoryRecord (Join-Path $Repo $relative)
}

# Every worktree is owned by somebody; record its HEAD and dirt without touching it.
$worktrees = @()
$porcelain = @(git -C $Repo worktree list --porcelain 2>$null)
$currentPath = $null
foreach ($line in $porcelain) {
    if ($line -like 'worktree *') {
        if ($currentPath) {
            $state = Get-GitState $currentPath
            $state['path'] = $currentPath
            $worktrees += $state
        }
        $currentPath = Normalize-Path $line.Substring(9)
    }
}
if ($currentPath) {
    $state = Get-GitState $currentPath
    $state['path'] = $currentPath
    $worktrees += $state
}

$protected = [ordered]@{
    worktree = $worktreeFull
    repo     = $repoFull
    recovery = [IO.Path]::GetFullPath($Recovery)
    shared_bench = [IO.Path]::GetFullPath($SharedBench)
    shared_lab = [IO.Path]::GetFullPath($SharedLab)
    shared_game = [IO.Path]::GetFullPath($SharedGame)
    shared_game_additional = @($AdditionalSharedGame | ForEach-Object { [IO.Path]::GetFullPath($_) })
    protected_tree_roots = @($protectedTreeRoots)
    game     = [IO.Path]::GetFullPath($Game)
}

$guard = [ordered]@{
    schema_version = 1
    # path_guard.py reads this policy itself and merges it into the deny roots for mutations.
    protected_policy = (Join-Path $PSScriptRoot 'shadow-policy.json')
    allow_write = @(
        (Join-Path $worktreeFull '_out'),
        (Join-Path $worktreeFull '_lab'),
        (Join-Path $worktreeFull 'build'),
        (Join-Path $worktreeFull 'build-tests'),
        [IO.Path]::GetFullPath($Game)
    )
    allow_read = @(
        $worktreeFull,
        $repoFull,
        [IO.Path]::GetFullPath($Recovery),
        [IO.Path]::GetFullPath($SharedBench),
        [IO.Path]::GetFullPath($SharedLab),
        'C:\Program Files\RenderDoc'
    )
    # Concatenate the variable-length groups instead of nesting them: an array literal keeps a
    # nested empty array as a literal element, which path_guard.py then rejects as a non-string.
    deny = @(
        (Join-Path $repoFull 'pc-vr'),
        [IO.Path]::GetFullPath($Recovery),
        [IO.Path]::GetFullPath($SharedBench),
        [IO.Path]::GetFullPath($SharedLab),
        [IO.Path]::GetFullPath($SharedGame),
        [IO.Path]::GetFullPath('D:\Temp\Deracine\1.0'),
        [IO.Path]::GetFullPath('D:\Temp\Deracine\CUSA11919-1.01'),
        [IO.Path]::GetFullPath('D:\Temp\Deracine\CUSA11919-color'),
        [IO.Path]::GetFullPath('D:\Temp\Deracine\Update')
    ) + @($AdditionalSharedGame | ForEach-Object { [IO.Path]::GetFullPath($_) }) +
        @($protectedTreeRoots) + @($worktrees | Where-Object {
        $_.present -and (Normalize-Path $_.path) -ne $worktreeFull
    } | ForEach-Object { $_.path })
}

$manifest = [ordered]@{
    schema_version = 1
    created_utc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    protected      = $protected
    own            = (Get-GitState $worktreeFull)
    worktrees      = $worktrees
    immutable      = $immutablePaths
    protected_trees = @($protectedTreeRecords.ToArray())
    volatile       = $volatile
    guard          = $guard
    shadow         = $shadow
}

Write-JsonFile $outFull $manifest
$treeFileCount = [long]0
foreach ($treeRecord in $protectedTreeRecords) { $treeFileCount += [long]$treeRecord.file_count }
Write-Output "preflight: $($immutablePaths.Count) protected files, $($protectedTreeRecords.Count) protected trees/$treeFileCount tree files, $($worktrees.Count) worktrees -> $outFull"
exit 0
