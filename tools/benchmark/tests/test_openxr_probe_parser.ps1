$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\openxr_probe_parser.ps1')

$root = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('deracine-rhi-probe-test-' + [Guid]::NewGuid().ToString('N'))))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $root.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing test fixture outside the OS temp root: $root"
}
New-Item -ItemType Directory -Path $root | Out-Null

$hash = '8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75'
$ymm = @()
for ($i = 0; $i -lt 32; $i++) { $ymm += ('0x{0:x}' -f $i) }
$stack = @('0x10','0x11','0x12','0x13','0x14','0x15','0x16','0x17')
$install = "DERACINE_RHI_PROBE_INSTALL title_id=CUSA11919 app_version=01.01 image_sha256=$hash module_name=eboot.bin module_base=0x800000000 target_rva=0xcbb9c0 target=0x800cbb9c0 exec_pt_load_rva=0xc00000 exec_pt_load_filesz=0x100000 exec_pt_load_memsz=0x100000 event_limit=128"

function New-EventLine([int]$Sequence, [int]$StackValidated = 1) {
    $seq = $Sequence.ToString([Globalization.CultureInfo]::InvariantCulture)
    return "DERACINE_RHI_PROBE_EVENT sequence=$seq thread_id=12345 module_base=0x800000000 target=0x800cbb9c0 caller=0x80cbb9c1 entry_rsp=0x7fff0000 gpr_rdi=0x1 gpr_rsi=0x2 gpr_rdx=0x3 gpr_rcx=0x4 gpr_r8=0x5 gpr_r9=0x6 ymm_args=[$($ymm -join ',')] stack_validated=$StackValidated stack_count=3 stack_args=[$($stack -join ',')]"
}

function New-GnmLine([int]$GnmSequence, [int]$RhiSequence = 1, [string]$ThreadId = '12345', [int]$DeltaUs = 123) {
    $gnm = $GnmSequence.ToString([Globalization.CultureInfo]::InvariantCulture)
    $rhi = $RhiSequence.ToString([Globalization.CultureInfo]::InvariantCulture)
    $delta = $DeltaUs.ToString([Globalization.CultureInfo]::InvariantCulture)
    return "DERACINE_GNM_DRAW_INDEX gnm_sequence=$gnm rhi_sequence=$rhi thread_id=$ThreadId delta_us=$delta rhi_module_base=0x800000000 rhi_target=0x800cbb9c0 rhi_caller=0x80cbb9c1 cmdbuf=0x1000 size=10 index_count=36 index_addr=0x2000 flags=0x0 type=0x2"
}

function Write-Fixture([string]$Name, [string[]]$Lines) {
    $path = Join-Path $root $Name
    [IO.File]::WriteAllLines($path, $Lines, [Text.UTF8Encoding]::new($false))
    return $path
}

try {
    $installLine = "[Core_Linker] $install"
    $eventLine = "[Core_Linker] $(New-EventLine 1)"
    $gnmLine = "[Core_Linker] $(New-GnmLine 1)"
    $stdout = Write-Fixture 'stdout.txt' @($installLine)
    $stderr = Write-Fixture 'stderr.txt' @($eventLine, $gnmLine)
    $shadLog = Write-Fixture 'shad_log.txt' @($installLine, $eventLine, $gnmLine)

    $evidence = Get-DeracineRhiProbeEvidence -Paths @($stdout, $stderr, $shadLog) -PinnedImageSha256 $hash
    if (-not $evidence.valid -or $evidence.event_count -ne 1) { throw "valid duplicated sinks failed: $($evidence.errors -join '; ')" }
    if (@($evidence.events[0].sources).Count -ne 2) { throw 'duplicate event records were not merged across their two captured sinks' }
    if ($evidence.gnm_draw_index.event_count -ne 1 -or
        @($evidence.gnm_draw_index.events[0].sources).Count -ne 2 -or
        $evidence.gnm_draw_index.events[0].paired_rhi_event.sequence -ne 1 -or
        $evidence.gnm_draw_index.events[0].paired_rhi_event.thread_id -cne '12345' -or
        $evidence.gnm_draw_index.events[0].fields.index_count -cne '36') {
        throw 'GNM draw-index records were not deduplicated and paired to the matching RHI sequence/thread'
    }
    if (-not $evidence.events[0].stack_args_trusted -or @($evidence.events[0].validated_stack_args).Count -ne 3) {
        throw 'validated stack arguments were not limited to stack_count'
    }
    if ($evidence.schema -cne 'DERACINE_RHI_PROBE_INSTALL+DERACINE_RHI_PROBE_EVENT/v2+DERACINE_GNM_DRAW_INDEX' -or
        @($evidence.events[0].ymm_qwords_low_to_high_register_order).Count -ne 32 -or
        $evidence.events[0].ymm_qwords_low_to_high_register_order[0] -cne '0x0' -or
        $evidence.events[0].ymm_qwords_low_to_high_register_order[31] -cne '0x1f') {
        throw 'YMM v2 qword ordering/count was not retained'
    }
    if (@($evidence.events[0].stack_arguments_interpreted).Count -ne 3 -or
        $evidence.events[0].stack_arguments_interpreted[0].name -cne 'StartIndex' -or
        $evidence.events[0].stack_arguments_interpreted[0].raw_value -cne '0x10') {
        throw 'interpretation-only stack mapping did not retain the raw StartIndex value'
    }
    if (-not $evidence.install_address_relation.passed) { throw 'target/base/RVA arithmetic relation was not verified' }

    $outOfOrderPath = Write-Fixture 'out-of-order.txt' @(
        $installLine, (New-EventLine 2), (New-EventLine 1), (New-GnmLine 2 2), (New-GnmLine 1 1)
    )
    $outOfOrder = Get-DeracineRhiProbeEvidence -Paths @($outOfOrderPath) -PinnedImageSha256 $hash
    if (-not $outOfOrder.valid -or $outOfOrder.events[0].sequence -ne 1 -or $outOfOrder.events[1].sequence -ne 2 -or
        $outOfOrder.gnm_draw_index.events[0].gnm_sequence -ne 1 -or
        $outOfOrder.gnm_draw_index.events[0].paired_rhi_event.sequence -ne 1 -or
        $outOfOrder.gnm_draw_index.events[1].paired_rhi_event.sequence -ne 2) {
        throw 'out-of-order RHI and GNM records were not accepted and paired by sequence/thread'
    }

    $badTargetInstall = $installLine.Replace('target=0x800cbb9c0', 'target=0x800cbb9c1')
    $badTargetPath = Write-Fixture 'bad-target.txt' @($badTargetInstall, (New-EventLine 1), (New-GnmLine 1))
    $badTarget = Get-DeracineRhiProbeEvidence -Paths @($badTargetPath) -PinnedImageSha256 $hash
    if ($badTarget.schema_valid -or @($badTarget.errors | Where-Object { $_ -match 'target does not equal' }).Count -eq 0) {
        throw 'incorrect target == module_base + target_rva relation was not rejected'
    }

    $overflowInstall = $installLine.Replace('module_base=0x800000000 target_rva=0xcbb9c0 target=0x800cbb9c0',
        'module_base=0xffffffffffffffff target_rva=0xcbb9c0 target=0xffffffffffffffff')
    $overflowPath = Write-Fixture 'overflow.txt' @($overflowInstall)
    $overflow = Get-DeracineRhiProbeEvidence -Paths @($overflowPath) -PinnedImageSha256 $hash
    if ($overflow.schema_valid -or @($overflow.errors | Where-Object { $_ -match 'overflows UInt64' }).Count -eq 0) {
        throw 'UInt64 target-address overflow was not rejected'
    }

    $stackOff = Write-Fixture 'stack-off.txt' @($installLine, (New-EventLine 2 0), (New-GnmLine 1 2))
    $stackOffEvidence = Get-DeracineRhiProbeEvidence -Paths @($stackOff) -PinnedImageSha256 $hash
    if (-not $stackOffEvidence.valid -or $stackOffEvidence.events[0].stack_args_trusted -or
        @($stackOffEvidence.events[0].validated_stack_args).Count -ne 0) {
        throw 'stack_validated=0 did not invalidate all stack arguments'
    }

    $wrongOrder = $installLine.Replace('title_id=CUSA11919 app_version=01.01', 'app_version=01.01 title_id=CUSA11919')
    $badOrderPath = Write-Fixture 'bad-order.txt' @($wrongOrder, (New-EventLine 1))
    $badOrder = Get-DeracineRhiProbeEvidence -Paths @($badOrderPath) -PinnedImageSha256 $hash
    if ($badOrder.schema_valid -or @($badOrder.errors | Where-Object { $_ -match 'field 1' }).Count -eq 0) {
        throw 'out-of-order install fields were not rejected'
    }

    $shortYmm = $ymm[0..30] -join ','
    $badYmm = (New-EventLine 1).Replace("ymm_args=[$($ymm -join ',')]", "ymm_args=[$shortYmm]")
    $badYmmPath = Write-Fixture 'bad-ymm.txt' @($installLine, $badYmm)
    $badYmmEvidence = Get-DeracineRhiProbeEvidence -Paths @($badYmmPath) -PinnedImageSha256 $hash
    if ($badYmmEvidence.schema_valid -or @($badYmmEvidence.errors | Where-Object { $_ -match 'ymm_args contains 31 qword\(s\); expected 32' }).Count -eq 0) {
        throw 'incorrect YMM qword count was not rejected'
    }

    $legacyXmm = (New-EventLine 1).Replace("ymm_args=[$($ymm -join ',')]", "xmm_args=[$($ymm -join ',')]")
    $legacyXmmPath = Write-Fixture 'legacy-xmm.txt' @($installLine, $legacyXmm)
    $legacyXmmEvidence = Get-DeracineRhiProbeEvidence -Paths @($legacyXmmPath) -PinnedImageSha256 $hash
    if ($legacyXmmEvidence.schema_valid -or @($legacyXmmEvidence.errors | Where-Object { $_ -match "expected 'ymm_args'" }).Count -eq 0) {
        throw 'legacy xmm_args fallback was not rejected'
    }

    $conflictPath = Write-Fixture 'conflict.txt' @($installLine, (New-EventLine 1), (New-EventLine 1).Replace('gpr_rdi=0x1', 'gpr_rdi=0x9'), (New-GnmLine 1))
    $conflict = Get-DeracineRhiProbeEvidence -Paths @($conflictPath) -PinnedImageSha256 $hash
    if ($conflict.schema_valid -or @($conflict.errors | Where-Object { $_ -match 'conflicts with another event' }).Count -eq 0) {
        throw 'conflicting duplicate event sequence was not rejected'
    }

    $tooMany = @($installLine)
    for ($i = 1; $i -le 129; $i++) { $tooMany += (New-EventLine $i) }
    $tooMany += (New-GnmLine 1 1)
    $tooManyPath = Write-Fixture 'too-many.txt' $tooMany
    $tooManyEvidence = Get-DeracineRhiProbeEvidence -Paths @($tooManyPath) -PinnedImageSha256 $hash
    if ($tooManyEvidence.event_count -gt 128 -or $tooManyEvidence.schema_valid) {
        throw 'the 128-event hard cap was not enforced'
    }

    $missingGnmPath = Write-Fixture 'missing-gnm.txt' @($installLine, (New-EventLine 1))
    $missingGnm = Get-DeracineRhiProbeEvidence -Paths @($missingGnmPath) -PinnedImageSha256 $hash
    if ($missingGnm.valid -or @($missingGnm.errors | Where-Object { $_ -match 'no DERACINE_GNM_DRAW_INDEX record' }).Count -eq 0) {
        throw 'missing indexed-draw evidence was accepted through a compatibility fallback'
    }

    $wrongThreadPath = Write-Fixture 'wrong-thread.txt' @($installLine, (New-EventLine 1), (New-GnmLine 1 1 '12346'))
    $wrongThread = Get-DeracineRhiProbeEvidence -Paths @($wrongThreadPath) -PinnedImageSha256 $hash
    if ($wrongThread.schema_valid -or @($wrongThread.errors | Where-Object { $_ -match 'thread_id does not match RHI event 1' }).Count -eq 0) {
        throw 'GNM record with a different thread was not rejected'
    }

    $wrongRhiSequencePath = Write-Fixture 'wrong-rhi-sequence.txt' @($installLine, (New-EventLine 1), (New-GnmLine 1 2))
    $wrongRhiSequence = Get-DeracineRhiProbeEvidence -Paths @($wrongRhiSequencePath) -PinnedImageSha256 $hash
    if ($wrongRhiSequence.schema_valid -or @($wrongRhiSequence.errors | Where-Object { $_ -match 'references missing RHI event sequence 2' }).Count -eq 0) {
        throw 'GNM record referencing an absent RHI sequence was not rejected'
    }

    $staleGnmPath = Write-Fixture 'stale-gnm.txt' @($installLine, (New-EventLine 1), (New-GnmLine 1 1 '12345' 250001))
    $staleGnm = Get-DeracineRhiProbeEvidence -Paths @($staleGnmPath) -PinnedImageSha256 $hash
    if ($staleGnm.schema_valid -or @($staleGnm.errors | Where-Object { $_ -match 'exceeds the 250000 microsecond correlation window' }).Count -eq 0) {
        throw 'GNM record older than the correlation window was not rejected'
    }

    $badEcho = (New-GnmLine 1).Replace('rhi_caller=0x80cbb9c1', 'rhi_caller=0x80cbb9c2')
    $badEchoPath = Write-Fixture 'bad-rhi-echo.txt' @($installLine, (New-EventLine 1), $badEcho)
    $badEchoEvidence = Get-DeracineRhiProbeEvidence -Paths @($badEchoPath) -PinnedImageSha256 $hash
    if ($badEchoEvidence.schema_valid -or @($badEchoEvidence.errors | Where-Object { $_ -match 'rhi_caller does not match RHI event 1 caller' }).Count -eq 0) {
        throw 'GNM record with mismatched RHI identity echo was not rejected'
    }

    $gnmWrongOrder = (New-GnmLine 1).Replace('gnm_sequence=1 rhi_sequence=1', 'rhi_sequence=1 gnm_sequence=1')
    $gnmWrongOrderPath = Write-Fixture 'gnm-wrong-order.txt' @($installLine, (New-EventLine 1), $gnmWrongOrder)
    $gnmWrongOrderEvidence = Get-DeracineRhiProbeEvidence -Paths @($gnmWrongOrderPath) -PinnedImageSha256 $hash
    if ($gnmWrongOrderEvidence.schema_valid -or @($gnmWrongOrderEvidence.errors | Where-Object { $_ -match "field 1 is 'rhi_sequence'; expected 'gnm_sequence'" }).Count -eq 0) {
        throw 'out-of-order GNM fields were not rejected'
    }

    $badWidths = (New-GnmLine 1).Replace('size=10', 'size=4294967296').Replace('flags=0x0', 'flags=0x100000000')
    $badWidthsPath = Write-Fixture 'gnm-bad-widths.txt' @($installLine, (New-EventLine 1), $badWidths)
    $badWidthsEvidence = Get-DeracineRhiProbeEvidence -Paths @($badWidthsPath) -PinnedImageSha256 $hash
    if ($badWidthsEvidence.schema_valid -or
        @($badWidthsEvidence.errors | Where-Object { $_ -match 'size must be a UInt32 decimal integer' }).Count -eq 0 -or
        @($badWidthsEvidence.errors | Where-Object { $_ -match 'flags must be lowercase 0x hex within UInt32 range' }).Count -eq 0) {
        throw 'GNM draw-index UInt32 argument widths were not validated'
    }

    $gnmConflictPath = Write-Fixture 'gnm-conflict.txt' @(
        $installLine, (New-EventLine 1), (New-GnmLine 1), (New-GnmLine 1).Replace('index_count=36', 'index_count=37')
    )
    $gnmConflict = Get-DeracineRhiProbeEvidence -Paths @($gnmConflictPath) -PinnedImageSha256 $hash
    if ($gnmConflict.schema_valid -or @($gnmConflict.errors | Where-Object { $_ -match 'conflicts with another GNM draw-index record' }).Count -eq 0) {
        throw 'conflicting GNM duplicate sequence was not rejected'
    }

    $tooManyGnm = @($installLine, (New-EventLine 1))
    for ($i = 1; $i -le 129; $i++) { $tooManyGnm += (New-GnmLine $i 1) }
    $tooManyGnmPath = Write-Fixture 'too-many-gnm.txt' $tooManyGnm
    $tooManyGnmEvidence = Get-DeracineRhiProbeEvidence -Paths @($tooManyGnmPath) -PinnedImageSha256 $hash
    if ($tooManyGnmEvidence.gnm_draw_index.event_count -gt 128 -or $tooManyGnmEvidence.schema_valid) {
        throw 'the 128-record GNM draw-index cap was not enforced'
    }

    $badImagePath = Write-Fixture 'bad-image.txt' @($installLine.Replace($hash, ('0' * 64)), (New-EventLine 1), (New-GnmLine 1))
    $badImage = Get-DeracineRhiProbeEvidence -Paths @($badImagePath) -PinnedImageSha256 $hash
    if ($badImage.schema_valid -or @($badImage.errors | Where-Object { $_ -match 'image_sha256' }).Count -eq 0) {
        throw 'wrong image SHA-256 was not rejected'
    }

    Write-Output 'OpenXR RHI/GNM probe parser tests passed: v2 YMM fields, required draw-index records, strict sequence/thread pairing, echo/window checks, deduplication, conflicts, and 128-record caps'
} finally {
    if (Test-Path -LiteralPath $root -PathType Container) {
        $resolvedRoot = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $root).Path)
        if ($resolvedRoot.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -and
            $resolvedRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        } else {
            throw "test fixture cleanup path changed unexpectedly: $resolvedRoot"
        }
    }
}
