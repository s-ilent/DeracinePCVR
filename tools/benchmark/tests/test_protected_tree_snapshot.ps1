$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\protected_tree_snapshot.ps1')

function Copy-Snapshot($Snapshot) {
    $files = [ordered]@{}
    foreach ($name in $Snapshot.files.Keys) {
        $record = $Snapshot.files[$name]
        $files[$name] = [ordered]@{ bytes = $record.bytes; sha256 = $record.sha256 }
    }
    $links = [ordered]@{}
    foreach ($name in $Snapshot.reparse_points.Keys) {
        $record = $Snapshot.reparse_points[$name]
        $links[$name] = [ordered]@{
            link_type = $record.link_type
            target = $record.target
            is_directory = $record.is_directory
        }
    }
    return [ordered]@{
        root = $Snapshot.root
        file_count = $Snapshot.file_count
        total_bytes = $Snapshot.total_bytes
        directory_count = $Snapshot.directory_count
        reparse_point_count = $Snapshot.reparse_point_count
        tree_sha256 = $Snapshot.tree_sha256
        files = $files
        directories = @($Snapshot.directories)
        reparse_points = $links
    }
}

$before = Get-ProtectedTreeSnapshot -Path $PSScriptRoot
$after = Get-ProtectedTreeSnapshot -Path $PSScriptRoot
if (-not (Compare-ProtectedTreeSnapshot -Before $before -After $after).unchanged) {
    throw 'identical tree snapshots should compare unchanged'
}

$addedTree = Copy-Snapshot $before
$addedTree.files['__synthetic_added__'] = [ordered]@{ bytes = 1; sha256 = ('0' * 64) }
$added = Compare-ProtectedTreeSnapshot -Before $before -After $addedTree
if ($added.unchanged -or @($added.added_files) -notcontains '__synthetic_added__') {
    throw 'added-file detection failed'
}

$firstFile = @($before.files.Keys | Select-Object -First 1)[0]
if (-not $firstFile) { throw 'test tree unexpectedly contains no files' }
$removedTree = Copy-Snapshot $before
$removedTree.files.Remove($firstFile)
$removed = Compare-ProtectedTreeSnapshot -Before $before -After $removedTree
if ($removed.unchanged -or @($removed.removed_files) -notcontains $firstFile) {
    throw 'removed-file detection failed'
}

$changedTree = Copy-Snapshot $before
$oldRecord = $changedTree.files[$firstFile]
$changedTree.files[$firstFile] = [ordered]@{ bytes = ([long]$oldRecord.bytes + 1); sha256 = ('f' * 64) }
$changed = Compare-ProtectedTreeSnapshot -Before $before -After $changedTree
if ($changed.unchanged -or @($changed.changed_files).Count -ne 1) {
    throw 'content-change detection failed'
}

$directoryTree = Copy-Snapshot $before
$directoryTree.directories = @($directoryTree.directories) + @('__synthetic_dir__')
$directory = Compare-ProtectedTreeSnapshot -Before $before -After $directoryTree
if ($directory.unchanged -or @($directory.added_directories) -notcontains '__synthetic_dir__') {
    throw 'directory-set detection failed'
}

$linkTree = Copy-Snapshot $before
$linkTree.reparse_points['__synthetic_link__'] = [ordered]@{
    link_type = 'Junction'
    target = 'C:\\synthetic-target'
    is_directory = $true
}
$link = Compare-ProtectedTreeSnapshot -Before $before -After $linkTree
if ($link.unchanged -or @($link.added_reparse_points) -notcontains '__synthetic_link__') {
    throw 'reparse-point detection failed'
}

Write-Output ("protected tree snapshot tests passed: {0} file(s), {1} byte(s)" -f $before.file_count, $before.total_bytes)
