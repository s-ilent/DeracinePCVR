# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Read-only postflight: recompute the preflight snapshot and refuse success when a protected file
# or this session's own worktree changed unexpectedly. Volatile user directories and other agents'
# worktrees are recorded, not failed: another agent may legitimately work while a benchmark runs.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Before,
    [Parameter(Mandatory)][string]$Out
)

$ErrorActionPreference = 'Stop'
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
    return [ordered]@{ exists = $true; bytes = $item.Length; sha256 = Get-Sha256 $Path }
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
        present      = $true
        head         = if ($head) { $head.Trim() } else { $null }
        branch       = if ($branch) { $branch.Trim() } else { $null }
        dirty_count  = $porcelain.Count
        dirty_sha256 = if ($porcelain.Count -gt 0) {
            $bytes = [Text.Encoding]::UTF8.GetBytes(($porcelain -join "`n"))
            ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
        } else { $null }
    }
}

if (-not (Test-Path -LiteralPath $Before -PathType Leaf)) {
    throw "preflight manifest not found: $Before"
}
$beforeDoc = Get-Content -LiteralPath $Before -Raw | ConvertFrom-Json

$unexpected = New-Object System.Collections.Generic.List[string]
$observed = New-Object System.Collections.Generic.List[string]
$treeResults = [System.Collections.Generic.List[object]]::new()

$immutable = $beforeDoc.immutable
foreach ($property in $immutable.PSObject.Properties) {
    $path = $property.Name
    $old = $property.Value
    $now = Get-FileRecord $path
    if ($old.exists -ne $now.exists) {
        $unexpected.Add("existence changed: $path")
    } elseif ($old.exists -and ($old.sha256 -ne $now.sha256 -or $old.bytes -ne $now.bytes)) {
        $unexpected.Add("content changed: $path")
    }
}

if ($beforeDoc.PSObject.Properties.Name -contains 'protected_trees') {
    foreach ($oldTree in @($beforeDoc.protected_trees)) {
        try {
            $nowTree = Get-ProtectedTreeSnapshot -Path ([string]$oldTree.root)
            $comparison = Compare-ProtectedTreeSnapshot -Before $oldTree -After $nowTree
            $treeResults.Add($comparison)
            if (-not $comparison.unchanged) {
                $unexpected.Add("protected tree changed: $($oldTree.root) " +
                    "files +$(@($comparison.added_files).Count)/-$(@($comparison.removed_files).Count)/changed $(@($comparison.changed_files).Count), " +
                    "directories +$(@($comparison.added_directories).Count)/-$(@($comparison.removed_directories).Count), " +
                    "reparse +$(@($comparison.added_reparse_points).Count)/-$(@($comparison.removed_reparse_points).Count)/changed $(@($comparison.changed_reparse_points).Count)")
            }
        } catch {
            $message = "protected tree could not be verified: $($oldTree.root) ($($_.Exception.Message))"
            $unexpected.Add($message)
            $treeResults.Add([ordered]@{
                root = [string]$oldTree.root
                unchanged = $false
                error = $_.Exception.Message
                before_tree_sha256 = [string]$oldTree.tree_sha256
                after_tree_sha256 = $null
            })
        }
    }
}

$volatile = $beforeDoc.volatile
foreach ($property in $volatile.PSObject.Properties) {
    $path = $property.Name
    $old = $property.Value
    $root = if ($path.StartsWith('pc-vr')) { $beforeDoc.protected.repo } else { $beforeDoc.protected.repo }
    $now = Get-DirectoryRecord (Join-Path $root $path)
    if ($old.files -ne $now.files -or $old.bytes -ne $now.bytes) {
        $observed.Add("volatile directory changed: $path ($($old.files) -> $($now.files) files)")
    }
}

$ownNow = Get-GitState $beforeDoc.protected.worktree
$ownBefore = $beforeDoc.own
if ($ownBefore.head -ne $ownNow.head) {
    $unexpected.Add("own worktree HEAD changed: $($ownBefore.head) -> $($ownNow.head)")
}
if ($ownBefore.dirty_sha256 -ne $ownNow.dirty_sha256) {
    $unexpected.Add("own worktree dirty set changed during the session")
}

$worktreesNow = @()
foreach ($sibling in $beforeDoc.worktrees) {
    $now = Get-GitState $sibling.path
    $now['path'] = $sibling.path
    $worktreesNow += $now
    if ($sibling.path -eq $beforeDoc.protected.worktree) { continue }
    if (-not $sibling.present) { continue }
    if ($sibling.head -ne $now.head -or $sibling.dirty_count -ne $now.dirty_count) {
        $observed.Add("another worktree changed (owned elsewhere, recorded only): $($sibling.path) " +
                      "HEAD $($sibling.head) -> $($now.head), dirty $($sibling.dirty_count) -> $($now.dirty_count)")
    }
}

$result = [ordered]@{
    schema_version     = 1
    checked_utc        = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    before             = $Before
    protected_unchanged = ($unexpected.Count -eq 0)
    unexpected         = @($unexpected)
    observed           = @($observed)
    protected_trees    = @($treeResults.ToArray())
    own_after          = $ownNow
    worktrees_after    = $worktreesNow
}

Write-JsonFile $Out $result
if ($unexpected.Count -gt 0) {
    Write-Output "postflight: FAIL - $($unexpected.Count) protected change(s)"
    $unexpected | ForEach-Object { Write-Output "  - $_" }
    exit 1
}
Write-Output "postflight: OK - protected files and $($treeResults.Count) protected trees unchanged ($($observed.Count) recorded observations)"
exit 0
