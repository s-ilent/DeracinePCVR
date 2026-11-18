# Tests the Morpheus/Unreal configuration controls: pc-vr/render-config.ps1 (the game's own
# Engine.ini) and the parts of pc-vr/launch.ps1 that drive it. No emulator and no game are needed,
# and no game folder is touched: the layouts are made up in build/morpheus-config-test.
#   powershell -ExecutionPolicy Bypass -File tools/tests/morpheus-config-test.ps1
$top = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$launcher = Join-Path $top "pc-vr\launch.ps1"
$renderConfig = Join-Path $top "pc-vr\render-config.ps1"
$shell = (Get-Process -Id $PID).Path
$base = Join-Path $top "build\morpheus-config-test"
if ([System.IO.Directory]::Exists($base)) { [System.IO.Directory]::Delete($base, $true) }
$failed = 0
function Check([string]$what, $got, $want) {
    if ("$got" -eq "$want") { Write-Host "ok    $what" }
    else { $script:failed++; Write-Host "FAIL  $what`n      got:  $got`n      want: $want" }
}
function Check-True([string]$what, $got) { Check $what ([bool]$got) $true }
function Check-False([string]$what, $got) { Check $what ([bool]$got) $false }

# --- a game folder made up for the purpose ---------------------------------------------------------
# What the shipped saved configuration looks like: the game's own values, no Morpheus frame
# sequence (that key is nowhere in the 46 configs extracted from the shipped pak), and a device
# profile section that also sets r.ScreenPercentage and must be left alone.
$original = @(
    "[SystemSettings]"
    "r.ScreenPercentage=130"
    "r.MSAACount=2"
    "r.FinishCurrentFrame=1"
    ""
    "[/Script/Engine.RendererSettings]"
    "r.DefaultFeature.AntiAliasing=3"
    "r.ForwardShading=True"
    ""
    "[/Script/Engine.Engine]"
    "bSmoothFrameRate=True"
    "SmoothedFrameRateRange=(LowerBound=(Type=Inclusive,Value=22),UpperBound=(Type=Exclusive,Value=62))"
    ""
    "[/Script/MorpheusEditor.MorpheusRuntimeSettings]"
    "bEnableMorpheus=True"
    "vr.InstancedStereo=True"
    ""
    "[PS4 DeviceProfile]"
    "r.ScreenPercentage=130"
)
function New-Game([string]$name) {
    $folder = Join-Path $base $name
    $ini = Join-Path $folder "gameproject\saved\config\ps4\engine.ini"
    [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($ini))
    [System.IO.File]::WriteAllText((Join-Path $folder "eboot.bin"), "x")
    [System.IO.File]::WriteAllText($ini, ($original -join "`n"))
    return $folder
}
function Get-IniText([string]$folder) {
    return [System.IO.File]::ReadAllText((Join-Path $folder "gameproject\saved\config\ps4\engine.ini"))
}
function Get-IniValue([string]$folder, [string]$section, [string]$key) {
    $inSection = $false
    foreach ($line in [System.IO.File]::ReadAllLines((Join-Path $folder "gameproject\saved\config\ps4\engine.ini"))) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]$') { $inSection = ($matches[1] -ieq $section); continue }
        if ($inSection -and $trimmed -match ('^' + [regex]::Escape($key) + '\s*=\s*(.*)$')) { return $matches[1] }
    }
    return ""
}
function Invoke-RenderConfig([string]$folder, [string[]]$arguments) {
    $all = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $renderConfig, "-Eboot", $folder) + $arguments
    return @(& $shell @all)
}
function Get-Report([string[]]$lines) {
    foreach ($line in $lines) { if ($line -match '^render-config: (\S+) (.*)$') { return $matches[1] } }
    return ""
}
function Get-IniSections([string]$text) {
    return (@([regex]::Matches($text, '(?m)^\[(.+)\]$') | ForEach-Object { $_.Groups[1].Value }) -join ",")
}

# --- the 120 Hz configuration, exactly as it was measured ------------------------------------------
$game = New-Game "native120"
$out = Invoke-RenderConfig $game @("-Percent", "200", "-FrameMode", "120", "-GuestMsaa", "4", "-Quiet")
Check "120 mode is reported as applied" (Get-Report $out) "applied"
Check "the Morpheus frame sequence is in the game's saved config" (Get-IniValue $game "/Script/MorpheusEditor.MorpheusRuntimeSettings" "PSVRFrameSequence") "Render120Scanout120"
Check "the frame sequence's own section is used" (Get-IniValue $game "/Script/MorpheusEditor.MorpheusRuntimeSettings" "bEnableMorpheus") "True"
Check "smoothing is off in the engine section" (Get-IniValue $game "/Script/Engine.Engine" "bSmoothFrameRate") "False"
Check "the smoothed range is the tested one" (Get-IniValue $game "/Script/Engine.Engine" "SmoothedFrameRateRange") "(LowerBound=(Type=Inclusive,Value=22),UpperBound=(Type=Exclusive,Value=250))"
Check "t.MaxFPS is 0" (Get-IniValue $game "SystemSettings" "t.MaxFPS") "0"
Check "the frame is not finished early" (Get-IniValue $game "SystemSettings" "r.FinishCurrentFrame") "0"
Check "the render scale is the one asked for" (Get-IniValue $game "SystemSettings" "r.ScreenPercentage") "200"
Check "the game's MSAA is the one asked for" (Get-IniValue $game "SystemSettings" "r.MSAACount") "4"
Check "a copy of the game's own file is kept" ([System.IO.File]::Exists((Join-Path $game "gameproject\saved\config\ps4\engine.ini.deracine-original"))) $true
Check "the game's own file is the copy, unchanged" ([System.IO.File]::ReadAllText((Join-Path $game "gameproject\saved\config\ps4\engine.ini.deracine-original"))) ($original -join "`n")
Check "no section was added" (Get-IniSections (Get-IniText $game)) "SystemSettings,/Script/Engine.RendererSettings,/Script/Engine.Engine,/Script/MorpheusEditor.MorpheusRuntimeSettings,PS4 DeviceProfile"
Check "another section's r.ScreenPercentage is left alone" (Get-IniValue $game "PS4 DeviceProfile" "r.ScreenPercentage") "130"
Check "an unrelated key is left alone" (Get-IniValue $game "/Script/Engine.RendererSettings" "r.ForwardShading") "True"
Check-True "the guest's values are read back out of the file" (((Invoke-RenderConfig $game @("-List", "-Quiet")) -join " ") -match 'effective: render=200 frame=Render120Scanout120 msaa=4')

# A second identical run changes nothing and keeps the copy it made first.
$before = Get-IniText $game
$out = Invoke-RenderConfig $game @("-Percent", "200", "-FrameMode", "120", "-GuestMsaa", "4", "-Quiet")
Check "asking for the same thing twice writes nothing" (Get-Report $out) "unchanged"
Check "the file is the same afterwards" (Get-IniText $game) $before

# --- back to the game's own --------------------------------------------------------------------
$out = Invoke-RenderConfig $game @("-Percent", "130", "-FrameMode", "60", "-GuestMsaa", "2", "-Quiet")
Check "60 mode is reported as applied" (Get-Report $out) "applied"
Check "the frame sequence is not in the file" (Get-IniValue $game "/Script/MorpheusEditor.MorpheusRuntimeSettings" "PSVRFrameSequence") ""
Check "smoothing is the game's own again" (Get-IniValue $game "/Script/Engine.Engine" "bSmoothFrameRate") "True"
Check "the smoothed range is the game's own again" (Get-IniValue $game "/Script/Engine.Engine" "SmoothedFrameRateRange") "(LowerBound=(Type=Inclusive,Value=22),UpperBound=(Type=Exclusive,Value=62))"
Check "the frame is finished as the game asks" (Get-IniValue $game "SystemSettings" "r.FinishCurrentFrame") "1"
Check "t.MaxFPS is gone" (Get-IniValue $game "SystemSettings" "t.MaxFPS") ""
Check "60 mode returns the file to the game's own" (Get-IniText $game) ($original -join "`n")

# --- restore, and the file's own newlines ------------------------------------------------------
$out = Invoke-RenderConfig $game @("-Percent", "300", "-Quiet")
Check "300 is accepted" (Get-Report $out) "applied"
$out = Invoke-RenderConfig $game @("-Restore", "-Quiet")
Check "restoring is reported" (Get-Report $out) "restored"
Check "the game's own file is back exactly" (Get-IniText $game) ($original -join "`n")
Check "the copy is gone" ([System.IO.File]::Exists((Join-Path $game "gameproject\saved\config\ps4\engine.ini.deracine-original"))) $false
$out = Invoke-RenderConfig $game @("-Restore", "-Quiet")
Check "there is nothing to restore twice" (Get-Report $out) "nothing-to-restore"

# A file with Windows newlines keeps them; a file with bare newlines keeps those.
$crlf = New-Game "crlf"
[System.IO.File]::WriteAllText((Join-Path $crlf "gameproject\saved\config\ps4\engine.ini"), ($original -join "`r`n"))
[void](Invoke-RenderConfig $crlf @("-Percent", "200", "-Quiet"))
Check-True "a Windows-newline file keeps Windows newlines" ((Get-IniText $crlf).Contains("`r`n"))
Check-False "a Windows-newline file gains no bare newlines" ((Get-IniText $crlf).Replace("`r`n", "").Contains("`n"))
[void](Invoke-RenderConfig $game @("-Percent", "200", "-Quiet"))
Check-False "a bare-newline file gains no Windows newlines" ((Get-IniText $game).Contains("`r`n"))

# --- refusals ---------------------------------------------------------------------------------
$beforeRefusal = Get-IniValue $game "SystemSettings" "r.ScreenPercentage"
$out = Invoke-RenderConfig $game @("-Percent", "400", "-Quiet")
Check "400 is refused, not written" (Get-Report $out) "out-of-range"
Check "the refusal says what the ceiling is" (($out -join " ") -match 'clamp') $true
Check "and nothing was written" (Get-IniValue $game "SystemSettings" "r.ScreenPercentage") $beforeRefusal
Check "too small a render scale is refused" (Get-Report (Invoke-RenderConfig $game @("-Percent", "50", "-Quiet"))) "out-of-range"
Check "a frame mode that is neither 60 nor 120 is refused" (Get-Report (Invoke-RenderConfig $game @("-FrameMode", "90", "-Quiet"))) "error"
Check "an MSAA count the game does not have is refused" (Get-Report (Invoke-RenderConfig $game @("-GuestMsaa", "3", "-Quiet"))) "error"
Check "a render scale that is not a number is refused" (Get-Report (Invoke-RenderConfig $game @("-Percent", "lots", "-Quiet"))) "error"
Check "an unknown game folder is refused" (Get-Report (Invoke-RenderConfig (Join-Path $base "nowhere") @("-Percent", "200", "-Quiet"))) "error"
Check "nothing asked for leaves the file alone" (Get-Report (Invoke-RenderConfig $game @("-Quiet"))) "unchanged"

# A game's own file that already asks for 120 is not silently changed by frame_mode=60, and says so.
$already = New-Game "already120"
$ini = Join-Path $already "gameproject\saved\config\ps4\engine.ini"
$doctor = ($original -join "`n").Replace("bEnableMorpheus=True", "bEnableMorpheus=True`nPSVRFrameSequence=Render120Scanout120")
[System.IO.File]::WriteAllText($ini, $doctor)
Check "the made-up game already asks for the tested sequence" (Get-IniValue $already "/Script/MorpheusEditor.MorpheusRuntimeSettings" "PSVRFrameSequence") "Render120Scanout120"
$out = Invoke-RenderConfig $already @("-FrameMode", "60", "-Quiet")
Check "a sequence already in the game's own file is reported" (Get-Report $out) "own-frame-sequence"
Check "and the sequence is left as the game's own file has it" (Get-IniValue $already "/Script/MorpheusEditor.MorpheusRuntimeSettings" "PSVRFrameSequence") "Render120Scanout120"
$out = Invoke-RenderConfig $already @("-FrameMode", "120", "-Quiet")
Check "120 mode completes the block on a file that already asks for the sequence" (Get-Report $out) "applied"
Check "and it is still the tested sequence" (Get-IniValue $already "/Script/MorpheusEditor.MorpheusRuntimeSettings" "PSVRFrameSequence") "Render120Scanout120"
Check "and the companions the mode needs are written too" (Get-IniValue $already "/Script/Engine.Engine" "bSmoothFrameRate") "False"
Check "and the frame sequence is not duplicated" (@([regex]::Matches([System.IO.File]::ReadAllText($ini), 'PSVRFrameSequence')).Count) 1

# --- the launcher's side ------------------------------------------------------------------------
# The launcher's functions, not its main flow, exactly as tools/tests/launcher-test.ps1 loads them.
$ast = [System.Management.Automation.Language.Parser]::ParseFile($launcher, [ref]$null, [ref]$null)
foreach ($function in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($function.Extent.Text))
}

# Not one assignment anywhere in the launcher may set the Astro Bot frame cap, nor the emulated
# display's vblank: SHADPS4_VR_FPS_CAP is read only by the Astro Bot known-title governor, and
# PresentThread takes the VR refresh rate whenever a headset is connected.
$assigned = @()
foreach ($assignment in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.AssignmentStatementAst] }, $true)) {
    $assigned += $assignment.Left.Extent.Text
}
Check-False "the launcher sets no Astro Bot frame cap" (@($assigned | Where-Object { $_ -match 'SHADPS4_VR_FPS_CAP' }).Count)
Check-False "the launcher sets no frame pace of its own" (@($assigned | Where-Object { $_ -match 'SHADPS4_VR_(PACE|FASTEST_PACE|STEADY_FPS)' }).Count)
Check-False "the launcher rewrites no vblank frequency" (@($assigned | Where-Object { $_ -match 'vblank_frequency' }).Count)
Check-True "the launcher does set the emulated display's refresh" (@($assigned | Where-Object { $_ -match 'SHADPS4_VR_REFRESH_RATE' }).Count)

# refresh=, evaluated from the launcher's own statements. Statements are picked by their AST shape,
# not by searching their text: a comment inside a large block mentions the setting's name, and a
# text search would run that whole block.
function Get-StatementIf($ast, [string]$condition) {
    return @($ast.EndBlock.Statements | Where-Object {
        $_.GetType().Name -eq 'IfStatementAst' -and $_.Clauses[0].Item1.Extent.Text -match $condition
    })
}
function Get-StatementAssign($ast, [string[]]$names) {
    return @($ast.EndBlock.Statements | Where-Object {
        $_.GetType().Name -eq 'AssignmentStatementAst' -and $_.Left.Extent.Text -in $names
    })
}
$refreshStatements = @()
$refreshStatements += Get-StatementAssign $ast @('$refresh')
$refreshStatements += Get-StatementIf $ast '^\$refresh'
$refreshStatements += Get-StatementAssign $ast @('$env:SHADPS4_VR_REFRESH_RATE')
Check "the refresh setting is one assignment, one check and one export" $refreshStatements.Count 3
function Try-Refresh([string]$value) {
    $script:settings = if ($value -eq "") { [ordered]@{} } else { [ordered]@{ refresh = $value } }
    $env:SHADPS4_VR_REFRESH_RATE = ""
    foreach ($statement in $refreshStatements) { . ([scriptblock]::Create($statement.Extent.Text)) }
    return $env:SHADPS4_VR_REFRESH_RATE
}
Check "refresh is 120 unless it is set" (Try-Refresh "") "120"
Check "refresh=120 is 120" (Try-Refresh "120") "120"
Check "refresh=90 is 90" (Try-Refresh "90") "90"
Check "a refresh the emulator has not got falls back to 120" (Try-Refresh "144") "120"
$env:SHADPS4_VR_FPS_CAP = "60"
[void](Try-Refresh "120")
Check "running the launcher's refresh settings leaves the Astro Bot frame cap untouched" $env:SHADPS4_VR_FPS_CAP "60"
$env:SHADPS4_VR_FPS_CAP = ""

# The launcher's guest-configuration block, run against a made-up game with its own settings file.
$guest = New-Game "launcher"
$here = Join-Path $top "pc-vr"
function Say([string]$text, [string]$color = "Gray") { }
$guestStatements = @()
$guestStatements += Get-StatementAssign $ast @('$renderPercent', '$frameMode', '$guestMsaa',
                                               '$restoreRender', '$renderState', '$renderConfig')
$guestStatements += Get-StatementIf $ast '\$renderConfig'
$guestStatements += Get-StatementIf $ast '\$renderPercent'
Check "the guest settings are read, applied and read back" $guestStatements.Count 7
function Invoke-GuestBlock([hashtable]$values) {
    $script:settings = [ordered]@{}
    foreach ($key in $values.Keys) { $script:settings[$key] = $values[$key] }
    $game = $guest
    foreach ($statement in $guestStatements) { . ([scriptblock]::Create($statement.Extent.Text)) }
    return $renderState
}Check "the launcher applies frame_mode=120 to the game's config" ((Invoke-GuestBlock @{ frame_mode = "120"; render_percent = "200" }) -match "Render120Scanout120") $true
Check "the launcher reads the render scale back" ((Invoke-GuestBlock @{ frame_mode = "60"; render_percent = "300" }) -match "render 300%") $true
Check "the launcher reports the game's own MSAA" ((Invoke-GuestBlock @{ frame_mode = "60" }) -match "msaa 2x") $true
Check "frame_mode=60 leaves the tested sequence out" ((Invoke-GuestBlock @{ frame_mode = "60" }) -match "Render120Scanout120") $false
Check "the launcher restores on render_restore=1" ((Invoke-GuestBlock @{ render_restore = "1" }) -match "frame sequence the game's own") $true
Check "the game's own render scale is back after the restore" (Get-IniValue $guest "SystemSettings" "r.ScreenPercentage") "130"
Check "the game's own MSAA is back after the restore" (Get-IniValue $guest "SystemSettings" "r.MSAACount") "2"
Check "the game's own frame finishing is back after the restore" (Get-IniValue $guest "SystemSettings" "r.FinishCurrentFrame") "1"

[System.IO.Directory]::Delete($base, $true)
"failed: $failed"
if ($failed -gt 0) { exit 1 }
