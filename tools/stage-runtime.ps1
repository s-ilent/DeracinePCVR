<#
.SYNOPSIS
    Stages a built emulator binary into pc-vr/ and reports what the play folder is missing.

.DESCRIPTION
    Runtime binaries are not tracked by git (see .gitignore). This script copies a build output
    to pc-vr/shadps4.exe, checks that it really is a Windows executable, and prints the SHA-256 of
    what it staged, so a test session can record exactly which binary it ran.

    With -Check it changes nothing: it reports whether pc-vr/shadps4.exe and the PkgTool files
    are present, prints their hashes, and exits non-zero when the play folder is incomplete.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\stage-runtime.ps1 -FromBuild build\shadPS4.exe

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\stage-runtime.ps1 -Check
#>
[CmdletBinding()]
param(
    [string] $FromBuild = '',
    [switch] $Check,
    [switch] $Force,
    [string] $RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$PcVr = Join-Path $RepoRoot 'pc-vr'
$Target = Join-Path $PcVr 'shadps4.exe'
$PkgToolExe = Join-Path $PcVr 'pkgtool\PkgTool.exe'
$LibOrbisDll = Join-Path $PcVr 'pkgtool\LibOrbisPkg.dll'

function Test-PeBinary([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $head = New-Object byte[] 4096
        $read = $stream.Read($head, 0, $head.Length)
        if ($read -lt 0x40) { return $false }
        if ($head[0] -ne 0x4D -or $head[1] -ne 0x5A) { return $false }   # 'MZ'
        $lfanew = [BitConverter]::ToInt32($head, 0x3C)
        if ($lfanew -lt 0 -or ($lfanew + 4) -ge $read) { return $false }
        return $head[$lfanew] -eq 0x50 -and $head[$lfanew + 1] -eq 0x45 -and `
               $head[$lfanew + 2] -eq 0x00 -and $head[$lfanew + 3] -eq 0x00   # 'PE\0\0'
    } finally {
        $stream.Dispose()
    }
}

function Get-Sha256([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Show-State {
    $complete = $true
    Write-Host ''
    Write-Host ('Play folder: ' + $PcVr)
    $rows = @(
        @{ What = 'emulator';    Path = $Target;      Hint = 'tools\stage-runtime.ps1 -FromBuild <build output>' },
        @{ What = 'PkgTool';     Path = $PkgToolExe;  Hint = 'tools\pkgtool.ps1 -Mode Build (or -Mode Release)' },
        @{ What = 'LibOrbisPkg'; Path = $LibOrbisDll; Hint = 'tools\pkgtool.ps1 -Mode Build (or -Mode Release)' }
    )
    foreach ($row in $rows) {
        if (-not (Test-Path -LiteralPath $row.Path -PathType Leaf)) {
            $complete = $false
            Write-Host ('  MISSING  {0,-12} {1}' -f $row.What, $row.Path)
            Write-Host ('           obtain it with: ' + $row.Hint)
        } elseif (-not (Test-PeBinary $row.Path)) {
            $complete = $false
            Write-Host ('  INVALID  {0,-12} {1} (not a Windows PE file)' -f $row.What, $row.Path)
        } else {
            Write-Host ('  ok       {0,-12} {1}  sha256 {2}' -f $row.What, $row.Path, (Get-Sha256 $row.Path))
        }
    }
    if (-not $complete) {
        Write-Host ''
        Write-Host 'The play folder is incomplete; the launcher needs all three files.'
        return $false
    }
    Write-Host ''
    Write-Host 'The play folder is complete.'
    return $true
}

if ($Check -or [string]::IsNullOrWhiteSpace($FromBuild)) {
    if (-not $Check) {
        Write-Output 'Pass -FromBuild <path to a built shadPS4.exe> to stage the emulator, or -Check to report only.'
    }
    $complete = Show-State
    if (-not $complete) { exit 1 }
    exit 0
}

$Source = $FromBuild
if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    # A relative path is taken from the repository root, so the examples work from anywhere.
    $Source = Join-Path $RepoRoot $FromBuild
}
if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    throw ('No such build output: ' + $FromBuild)
}
if (-not (Test-PeBinary $Source)) {
    throw ('Not a Windows executable (no MZ/PE header): ' + $Source)
}

$sourceHash = Get-Sha256 $Source
if (Test-Path -LiteralPath $Target -PathType Leaf) {
    $targetHash = Get-Sha256 $Target
    if ($targetHash -eq $sourceHash) {
        Write-Output ('pc-vr\shadps4.exe already holds this build (sha256 ' + $sourceHash + ').')
        exit 0
    }
    if (-not $Force) {
        throw ('pc-vr\shadps4.exe exists with sha256 ' + $targetHash + '; pass -Force to replace it with ' + $sourceHash + '.')
    }
}

Copy-Item -LiteralPath $Source -Destination $Target -Force
if (-not (Test-PeBinary $Target)) {
    throw ('Staged file is not a Windows executable: ' + $Target)
}
$stagedHash = Get-Sha256 $Target
$size = (Get-Item -LiteralPath $Target).Length
Write-Output ('Staged ' + $Target)
Write-Output ('  size   ' + $size + ' bytes')
Write-Output ('  sha256 ' + $stagedHash)
Write-Output ''
Write-Output ('Record that hash with any test result; the binary is not in version control.')
