# Provision the manifest that identifies a private benchmark game shadow.
#
# The benchmark runner refuses to write the pinned engine.ini into a game directory until that
# directory proves it is a private shadow: it must contain benchmark-shadow.json naming the shadow
# private and pinning its eboot. This script creates that file once per shadow. It is the only
# supported way to designate a shadow; do not point it at the active installation.
#
# Usage:
#   pwsh -File new-shadow-manifest.ps1 -ShadowRoot D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17
#   pwsh -File new-shadow-manifest.ps1 -ShadowRoot <dir> -Force -Verify
#
# -Verify runs game_shadow.py check afterwards (exit 0 valid). Without -Force an existing manifest
# is left untouched.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ShadowRoot,
    [string]$Owner = 'benchmark-private-shadow',
    [switch]$Force,
    [switch]$Verify,
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $ShadowRoot -PathType Container)) {
    throw "shadow root not found: $ShadowRoot"
}
$root = (Resolve-Path -LiteralPath $ShadowRoot).Path
$eboot = Join-Path $root 'eboot.bin'
if (-not (Test-Path -LiteralPath $eboot -PathType Leaf)) {
    throw "shadow root has no eboot.bin: $root"
}
if ([string]::IsNullOrWhiteSpace($Owner)) {
    throw 'owner must not be empty'
}

$manifestPath = Join-Path $root 'benchmark-shadow.json'
if ((Test-Path -LiteralPath $manifestPath) -and -not $Force) {
    throw "manifest already exists: $manifestPath (use -Force to replace it)"
}

$hash = (Get-FileHash -LiteralPath $eboot -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [ordered]@{
    private      = $true
    eboot_sha256 = $hash
    owner        = $Owner
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8
Write-Output "wrote $manifestPath"
Write-Output "  private=true owner=$Owner eboot_sha256=$hash"

if ($Verify) {
    & $Python (Join-Path $PSScriptRoot 'game_shadow.py') check --game $root --shadow-root $root `
        --eboot $eboot --json
    exit $LASTEXITCODE
}
