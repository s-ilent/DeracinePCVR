# Read-only recursive snapshot for protected source trees. Reparse points are recorded but never
# followed; callers must add each resolved target as a separate protected tree.
Set-StrictMode -Version Latest

function Get-ProtectedTreeSnapshot {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $root = Get-Item -LiteralPath $resolved -Force -ErrorAction Stop
    if (-not $root.PSIsContainer) {
        throw "protected tree is not a directory: $resolved"
    }
    if (($root.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "protected tree root is a reparse point; pass its resolved target: $resolved"
    }

    $files = [ordered]@{}
    $directories = [System.Collections.Generic.List[string]]::new()
    $reparsePoints = [ordered]@{}
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $visited = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $pending.Push($resolved)
    [long]$totalBytes = 0

    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        if (-not $visited.Add($directory)) { continue }
        foreach ($entry in @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)) {
            $relative = [IO.Path]::GetRelativePath($resolved, $entry.FullName).Replace('\', '/')
            $isReparse = ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            if ($isReparse) {
                $target = ''
                if ($entry.PSObject.Properties.Name -contains 'Target') {
                    $target = (@($entry.Target) | Select-Object -First 1)
                }
                $reparsePoints[$relative] = [ordered]@{
                    link_type = [string]$entry.LinkType
                    target = [string]$target
                    is_directory = [bool]$entry.PSIsContainer
                }
                continue
            }
            if ($entry.PSIsContainer) {
                $directories.Add($relative)
                $pending.Push($entry.FullName)
                continue
            }

            $hash = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
            [long]$length = $entry.Length
            $files[$relative] = [ordered]@{ bytes = $length; sha256 = $hash }
            $totalBytes += $length
        }
    }

    $canonical = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @($directories | Sort-Object -Unique)) {
        $canonical.Add("D`t$name")
    }
    foreach ($name in @($files.Keys | Sort-Object)) {
        $record = $files[$name]
        $canonical.Add("F`t$name`t$($record.bytes)`t$($record.sha256)")
    }
    foreach ($name in @($reparsePoints.Keys | Sort-Object)) {
        $record = $reparsePoints[$name]
        $canonical.Add("R`t$name`t$($record.link_type)`t$($record.is_directory)`t$($record.target)")
    }
    $payload = [Text.Encoding]::UTF8.GetBytes(($canonical -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $treeHash = ([BitConverter]::ToString($sha.ComputeHash($payload)) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }

    return [ordered]@{
        root = $resolved
        file_count = $files.Count
        total_bytes = $totalBytes
        directory_count = $directories.Count
        reparse_point_count = $reparsePoints.Count
        tree_sha256 = $treeHash
        files = $files
        directories = @($directories | Sort-Object -Unique)
        reparse_points = $reparsePoints
    }
}

function Get-ProtectedTreeMapKeys($Map) {
    if ($null -eq $Map) { return @() }
    if ($Map -is [System.Collections.IDictionary]) { return @($Map.Keys | ForEach-Object { [string]$_ }) }
    return @($Map.PSObject.Properties | ForEach-Object { $_.Name })
}

function Get-ProtectedTreeMapValue($Map, [string]$Name) {
    if ($null -eq $Map) { return $null }
    if ($Map -is [System.Collections.IDictionary]) {
        if ($Map.Contains($Name)) { return $Map[$Name] }
        return $null
    }
    $property = $Map.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Compare-ProtectedTreeSnapshot {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Before, [Parameter(Mandatory)]$After)

    $beforeFiles = $Before.files
    $afterFiles = $After.files
    $oldFileKeys = @(Get-ProtectedTreeMapKeys $beforeFiles)
    $newFileKeys = @(Get-ProtectedTreeMapKeys $afterFiles)
    $oldFileSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $newFileSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $oldFileKeys) { [void]$oldFileSet.Add($name) }
    foreach ($name in $newFileKeys) { [void]$newFileSet.Add($name) }

    $addedFiles = @($newFileKeys | Where-Object { -not $oldFileSet.Contains($_) } | Sort-Object)
    $removedFiles = @($oldFileKeys | Where-Object { -not $newFileSet.Contains($_) } | Sort-Object)
    $changedFiles = [System.Collections.Generic.List[object]]::new()
    foreach ($name in $oldFileKeys) {
        if (-not $newFileSet.Contains($name)) { continue }
        $oldRecord = Get-ProtectedTreeMapValue $beforeFiles $name
        $newRecord = Get-ProtectedTreeMapValue $afterFiles $name
        if ([long]$oldRecord.bytes -ne [long]$newRecord.bytes -or
            [string]$oldRecord.sha256 -ne [string]$newRecord.sha256) {
            $changedFiles.Add([ordered]@{
                path = $name
                before_bytes = $oldRecord.bytes
                after_bytes = $newRecord.bytes
                before_sha256 = $oldRecord.sha256
                after_sha256 = $newRecord.sha256
            })
        }
    }

    $oldDirs = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $newDirs = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @($Before.directories)) { [void]$oldDirs.Add([string]$name) }
    foreach ($name in @($After.directories)) { [void]$newDirs.Add([string]$name) }
    $addedDirectories = @($After.directories | Where-Object { -not $oldDirs.Contains([string]$_) } | Sort-Object)
    $removedDirectories = @($Before.directories | Where-Object { -not $newDirs.Contains([string]$_) } | Sort-Object)

    $oldLinks = $Before.reparse_points
    $newLinks = $After.reparse_points
    $oldLinkKeys = @(Get-ProtectedTreeMapKeys $oldLinks)
    $newLinkKeys = @(Get-ProtectedTreeMapKeys $newLinks)
    $oldLinkSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $newLinkSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $oldLinkKeys) { [void]$oldLinkSet.Add($name) }
    foreach ($name in $newLinkKeys) { [void]$newLinkSet.Add($name) }
    $addedLinks = @($newLinkKeys | Where-Object { -not $oldLinkSet.Contains($_) } | Sort-Object)
    $removedLinks = @($oldLinkKeys | Where-Object { -not $newLinkSet.Contains($_) } | Sort-Object)
    $changedLinks = [System.Collections.Generic.List[object]]::new()
    foreach ($name in $oldLinkKeys) {
        if (-not $newLinkSet.Contains($name)) { continue }
        $oldRecord = Get-ProtectedTreeMapValue $oldLinks $name
        $newRecord = Get-ProtectedTreeMapValue $newLinks $name
        if ([string]$oldRecord.link_type -ne [string]$newRecord.link_type -or
            [string]$oldRecord.target -ne [string]$newRecord.target -or
            [bool]$oldRecord.is_directory -ne [bool]$newRecord.is_directory) {
            $changedLinks.Add([ordered]@{ path = $name; before = $oldRecord; after = $newRecord })
        }
    }

    $rootMatches = [string]$Before.root -eq [string]$After.root
    $unchanged = $rootMatches -and $addedFiles.Count -eq 0 -and $removedFiles.Count -eq 0 -and
        $changedFiles.Count -eq 0 -and $addedDirectories.Count -eq 0 -and
        $removedDirectories.Count -eq 0 -and $addedLinks.Count -eq 0 -and
        $removedLinks.Count -eq 0 -and $changedLinks.Count -eq 0 -and
        [string]$Before.tree_sha256 -eq [string]$After.tree_sha256

    return [ordered]@{
        root = [string]$Before.root
        unchanged = $unchanged
        before_tree_sha256 = [string]$Before.tree_sha256
        after_tree_sha256 = [string]$After.tree_sha256
        before_file_count = [int]$Before.file_count
        after_file_count = [int]$After.file_count
        before_total_bytes = [long]$Before.total_bytes
        after_total_bytes = [long]$After.total_bytes
        added_files = $addedFiles
        removed_files = $removedFiles
        changed_files = @($changedFiles)
        added_directories = $addedDirectories
        removed_directories = $removedDirectories
        added_reparse_points = $addedLinks
        removed_reparse_points = $removedLinks
        changed_reparse_points = @($changedLinks)
    }
}
