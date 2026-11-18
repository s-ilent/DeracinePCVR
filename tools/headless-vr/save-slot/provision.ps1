# Provision the deterministic M2 save slot into a lab.
#
# The save is game/user data and is not committed. This script copies a save you own and refuses
# any file whose sha256/size does not match tools/headless-vr/save-slot/MANIFEST.json, so a run can
# never silently measure a different stage.
#
# Usage:
#   pwsh -File provision.ps1 -From D:\saves\CUSA11919 -To M:\...\lab\user\home\1000\savedata\CUSA11919\SaveData
# `-From` may point at the ...\CUSA11919 directory (containing SaveData) or at SaveData itself.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$From,
    [Parameter(Mandatory)][string]$To,
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'MANIFEST.json')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { throw "manifest not found: $ManifestPath" }
$manifestDoc = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json

$source = $From
if (-not (Test-Path -LiteralPath (Join-Path $source 'MGS_GameData_1') -PathType Leaf)) {
    $nested = Join-Path $From 'SaveData'
    if (Test-Path -LiteralPath (Join-Path $nested 'MGS_GameData_1') -PathType Leaf) {
        $source = $nested
    } else {
        throw "no SaveData with MGS_GameData_1 under $From"
    }
}

foreach ($name in $manifestDoc.files.PSObject.Properties.Name) {
    $path = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing save file: $path" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
    $expected = $manifestDoc.files.$name.ToUpperInvariant()
    if ($actual -ne $expected) {
        throw "save file $name has sha256 $actual, the manifest requires $expected"
    }
    if ($manifestDoc.sizes.PSObject.Properties.Name -contains $name) {
        $size = (Get-Item -LiteralPath $path).Length
        if ($size -ne [int]$manifestDoc.sizes.$name) {
            throw "save file $name is $size bytes, the manifest requires $($manifestDoc.sizes.$name)"
        }
    }
}

New-Item -ItemType Directory -Force -Path $To | Out-Null
foreach ($name in $manifestDoc.files.PSObject.Properties.Name) {
    $destination = Join-Path $To $name
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $destination -Force
}
Write-Output "provisioned slot $($manifestDoc.slot) (stage $($manifestDoc.stage)) into $To"
foreach ($name in $manifestDoc.files.PSObject.Properties.Name) {
    Write-Output ("  {0}  {1}" -f $name, $manifestDoc.files.$name)
}
