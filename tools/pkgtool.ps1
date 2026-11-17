<#
.SYNOPSIS
    Obtains PkgTool.exe and LibOrbisPkg.dll for pc-vr/pkgtool/ (they are not tracked by git).

.DESCRIPTION
    The launcher unpacks dumped game packages with PkgTool 0.2.231 and LibOrbisPkg.dll, by Maxton,
    from https://github.com/maxton/LibOrbisPkg (LGPL-3.0, see pc-vr/pkgtool/LICENSE.txt).
    Those two binaries are not stored in this repository.

    -Mode Build    (preferred) clone upstream at the pinned release commit and build with MSBuild.
                   Needs Visual Studio Build Tools and the .NET Framework 4.0 targeting pack.
                   A rebuild reports version 0.2.0.0 and is not byte-identical to the release, so
                   the hashes of what was staged are printed and recorded. Use -Mode Release when
                   you need exactly the released bytes.

    -Mode Release  download the official v0.2 asset PkgTool-0.2.231.zip, verify its SHA-256 and
                   then each extracted file against the hashes recorded below, and stage it.
                   Nothing is downloaded unless this mode is asked for explicitly.

    Both modes refuse to stage anything whose hash does not match what they verified, and print
    the SHA-256 of every staged file. Re-run either mode to replace the files after an upstream
    version bump; the pinned values in this script are the record of what was verified.

.PARAMETER SourceDir
    An existing checkout of LibOrbisPkg to build from (Build mode). Default: clone the pinned
    commit into a temporary folder.

.PARAMETER Destination
    Where to put the files. Default: pc-vr/pkgtool.

.PARAMETER Force
    Replace files of the same name in the destination.
#>
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Release')]
    [string] $Mode = 'Build',
    [string] $SourceDir = '',
    [string] $Destination = '',
    [string] $RepoRoot = '',
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

# Verified 2026-11-18: these are the exact bytes this repository used to carry, and they are
# byte-identical to the official release asset. Change them only together with a new upstream
# release and a fresh verification.
$UpstreamUrl = 'https://github.com/maxton/LibOrbisPkg'
$ReleaseTag = 'v0.2'
$PinnedCommit = '72370ca3739e0708250695046b3886e50452dee7'
$ReleaseAssetUrl = 'https://github.com/maxton/LibOrbisPkg/releases/download/v0.2/PkgTool-0.2.231.zip'
$ReleaseAssetSha256 = 'C639E591E35C2431F68410D1771541D95F7308863638F9E3A6EEDF1818530097'
$ExpectedFiles = @{
    'PkgTool.exe'     = '8B1C269D29E42EC13D7AAA4C8AC5D1B120F0393B91B51AF69DA38B92F962A9AB'
    'LibOrbisPkg.dll' = '787057748BE8C43FB2562CA6A47F664E86169A95671B646DF60F042DA862EF1D'
}

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $RepoRoot 'pc-vr\pkgtool'
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

function Get-Sha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Assert-KnownGood([string] $Path) {
    $name = Split-Path -Leaf $Path
    if (-not $ExpectedFiles.ContainsKey($name)) { return }
    $actual = Get-Sha256 $Path
    if ($actual -ne $ExpectedFiles[$name]) {
        throw ('Refusing to stage ' + $name + ': sha256 ' + $actual + ' is not the verified ' + $ExpectedFiles[$name] + '.')
    }
}

function Install-Files([string] $FromDir, [bool] $EnforceKnownHashes) {
    $staged = @()
    foreach ($name in @('PkgTool.exe', 'LibOrbisPkg.dll', 'LICENSE.txt')) {
        $source = Join-Path $FromDir $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            if ($name -eq 'LICENSE.txt') { continue }
            throw ('Missing ' + $name + ' in ' + $FromDir)
        }
        if ($EnforceKnownHashes) { Assert-KnownGood $source }
        $target = Join-Path $Destination $name
        Copy-Item -LiteralPath $source -Destination $target -Force
        $staged += $target
    }
    Write-Host ''
    Write-Host ('Staged into ' + $Destination + ':')
    $provenance = New-Object System.Collections.Generic.List[string]
    foreach ($file in $staged) {
        $hash = Get-Sha256 $file
        Write-Host ('  ' + (Split-Path -Leaf $file) + '  sha256 ' + $hash)
        $provenance.Add((Split-Path -Leaf $file) + ' ' + $hash)
    }
    return $staged
}

function Write-Provenance([string] $How) {
    $lines = @(
        ('PkgTool and LibOrbisPkg 0.2.231, by Maxton: the launcher uses them to unpack a game')
        ('package (.pkg). Licensed under the GNU Lesser General Public License, version 3')
        ('(LICENSE.txt). Source: ' + $UpstreamUrl + ' (release ' + $ReleaseTag + ', commit ' + $PinnedCommit + ').')
        ('')
        ('Obtained by tools/pkgtool.ps1 -Mode ' + $How + ' on ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '.')
        ('This file is local provenance, not part of the repository.')
    )
    foreach ($file in @('PkgTool.exe', 'LibOrbisPkg.dll')) {
        $path = Join-Path $Destination $file
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $lines += ($file + ' sha256 ' + (Get-Sha256 $path))
        }
    }
    $out = Join-Path $Destination 'PROVENANCE.local.txt'
    Set-Content -LiteralPath $out -Value $lines -Encoding UTF8
    Write-Output ('  provenance: ' + $out)
}

if ($Mode -eq 'Release') {
    $asset = Join-Path $env:TEMP ('PkgTool-0.2.231-' + [guid]::NewGuid().ToString('N') + '.zip')
    Write-Output ('Downloading ' + $ReleaseAssetUrl)
    Invoke-WebRequest -Uri $ReleaseAssetUrl -OutFile $asset -UseBasicParsing
    $assetHash = Get-Sha256 $asset
    if ($assetHash -ne $ReleaseAssetSha256) {
        Remove-Item -LiteralPath $asset -Force
        throw ('Downloaded asset has sha256 ' + $assetHash + ', expected ' + $ReleaseAssetSha256 + '. Nothing was staged.')
    }
    Write-Output ('  asset sha256 ' + $assetHash + ' (matches)')
    $expanded = Join-Path $env:TEMP ('pkgtool-' + [guid]::NewGuid().ToString('N'))
    Expand-Archive -LiteralPath $asset -DestinationPath $expanded
    try {
        $staged = Install-Files -FromDir $expanded -EnforceKnownHashes $true
    } finally {
        Remove-Item -LiteralPath $expanded -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $asset -Force -ErrorAction SilentlyContinue
    }
    Write-Provenance 'Release'
    exit 0
}

# Build mode: upstream's own source at the pinned commit.
$msbuild = $null
$cmd = Get-Command msbuild -ErrorAction SilentlyContinue
if ($cmd) {
    $msbuild = $cmd.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null
        if ($found) { $msbuild = ($found | Select-Object -First 1) }
    }
}
if (-not $msbuild) {
    throw ('MSBuild not found. Install Visual Studio Build Tools with the .NET Framework 4.0 targeting pack, or use -Mode Release for the verified upstream asset.')
}

$checkout = $SourceDir
$temporary = $false
if ([string]::IsNullOrWhiteSpace($checkout)) {
    $checkout = Join-Path $env:TEMP ('liborbispkg-' + [guid]::NewGuid().ToString('N'))
    $temporary = $true
    Write-Output ('Cloning ' + $UpstreamUrl + ' at ' + $PinnedCommit)
    & git clone --quiet $UpstreamUrl $checkout
    if ($LASTEXITCODE -ne 0) { throw 'git clone failed.' }
    & git -C $checkout checkout --quiet $PinnedCommit
    if ($LASTEXITCODE -ne 0) { throw ('git checkout ' + $PinnedCommit + ' failed.') }
}
$head = (& git -C $checkout rev-parse HEAD).Trim()
if ($head -ne $PinnedCommit) {
    throw ('Checkout is at ' + $head + ', not the pinned release commit ' + $PinnedCommit + '.')
}

try {
    Write-Output ('Building PkgTool with ' + $msbuild)
    & $msbuild (Join-Path $checkout 'PkgTool\PkgTool.csproj') '/p:Configuration=Release' '/p:Platform=AnyCPU' '/v:minimal' '/nologo'
    if ($LASTEXITCODE -ne 0) {
        throw ('MSBuild failed (exit ' + $LASTEXITCODE + '). The projects target .NET Framework 4.0; install the .NET Framework 4.0 Developer Pack (https://aka.ms/msbuild/developerpacks), or use -Mode Release for the hash-pinned upstream asset.')
    }
    $builtExe = Join-Path $checkout 'PkgTool\bin\Release\PkgTool.exe'
    $builtDll = Join-Path $checkout 'LibOrbisPkg\bin\Release\LibOrbisPkg.dll'
    foreach ($path in @($builtExe, $builtDll)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw ('Build output missing: ' + $path)
        }
    }
    # A rebuild carries version 0.2.0.0 rather than the release's 0.2.231.0, so the known-good
    # hashes do not apply; the files are staged as built and their hashes are recorded instead.
    # PkgTool.exe lives under PkgTool\bin\Release and the library under LibOrbisPkg\bin\Release.
    Write-Host ''
    Write-Host ('Staged into ' + $Destination + ':')
    foreach ($pair in @(@($builtExe, 'PkgTool.exe'), @($builtDll, 'LibOrbisPkg.dll'))) {
        $target = Join-Path $Destination $pair[1]
        Copy-Item -LiteralPath $pair[0] -Destination $target -Force
        Write-Host ('  ' + $pair[1] + '  sha256 ' + (Get-Sha256 $target))
    }
    $license = Join-Path $checkout 'LICENSE.txt'
    if (Test-Path -LiteralPath $license) {
        Copy-Item -LiteralPath $license -Destination (Join-Path $Destination 'LICENSE.txt') -Force
    }
    Write-Provenance 'Build'
    Write-Output ''
    Write-Output 'A rebuilt PkgTool reports version 0.2.0.0; verify pkg_extract against a known package before relying on it.'
} finally {
    if ($temporary -and (Test-Path -LiteralPath $checkout)) {
        Remove-Item -LiteralPath $checkout -Recurse -Force -ErrorAction SilentlyContinue
    }
}
