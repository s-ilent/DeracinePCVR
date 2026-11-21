# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Strict parser for the A3 Déraciné RHI-probe and correlated A2 GNM draw-index line contracts.
# It is independent of runtime execution so schema, pairing, and cap checks need no emulator/headset.

function Get-RhiProbePayload {
    param(
        [Parameter(Mandatory)][string]$Line,
        [Parameter(Mandatory)][string]$Prefix,
        [Parameter(Mandatory)][string[]]$ExpectedFields,
        [Parameter(Mandatory)][string]$SourcePath,
        [Parameter(Mandatory)][int]$LineNumber
    )
    $result = [ordered]@{ valid = $false; values = $null; error = $null }
    $first = $Line.IndexOf($Prefix, [System.StringComparison]::Ordinal)
    if ($first -lt 0 -or $first -ne $Line.LastIndexOf($Prefix, [System.StringComparison]::Ordinal)) {
        $result.error = "${SourcePath}:$LineNumber has an ambiguous $Prefix marker"
        return $result
    }
    $payloadStart = $first + $Prefix.Length
    if ($payloadStart -ge $Line.Length -or -not [char]::IsWhiteSpace($Line[$payloadStart])) {
        $result.error = "${SourcePath}:$LineNumber must place whitespace-separated fields after $Prefix"
        return $result
    }
    $payload = $Line.Substring($payloadStart).Trim()
    $tokens = if ($payload.Length -gt 0) { @($payload -split '\s+') } else { @() }
    if ($tokens.Count -ne $ExpectedFields.Count) {
        $result.error = "${SourcePath}:$LineNumber $Prefix has $($tokens.Count) token(s); expected $($ExpectedFields.Count)"
        return $result
    }
    $values = [ordered]@{}
    for ($i = 0; $i -lt $ExpectedFields.Count; $i++) {
        $token = [string]$tokens[$i]
        $equals = $token.IndexOf('=')
        if ($equals -le 0 -or $equals -eq ($token.Length - 1)) {
            $result.error = "${SourcePath}:$LineNumber token $($i + 1) is not a non-empty key=value pair"
            return $result
        }
        $name = $token.Substring(0, $equals)
        $value = $token.Substring($equals + 1)
        if ($name -cne $ExpectedFields[$i]) {
            $result.error = "${SourcePath}:$LineNumber field $($i + 1) is '$name'; expected '$($ExpectedFields[$i])'"
            return $result
        }
        if ($value.Contains(' ') -or $value.Contains("`t")) {
            $result.error = "${SourcePath}:$LineNumber field '$name' contains whitespace"
            return $result
        }
        $values[$name] = $value
    }
    $result.valid = $true
    $result.values = $values
    return $result
}

function Test-RhiHex([string]$Value) {
    return $Value -cmatch '^0x[0-9a-f]+$'
}

function Test-RhiUInt64Decimal([string]$Value, [switch]$AllowZero) {
    $pattern = if ($AllowZero) { '^(0|[1-9][0-9]*)$' } else { '^[1-9][0-9]*$' }
    if ($Value -cnotmatch $pattern) { return $false }
    [UInt64]$number = 0
    return [UInt64]::TryParse(
        $Value,
        [System.Globalization.NumberStyles]::None,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [ref]$number
    )
}

function Test-RhiUInt32Decimal([string]$Value) {
    if ($Value -cnotmatch '^(0|[1-9][0-9]*)$') { return $false }
    [UInt32]$number = 0
    return [UInt32]::TryParse(
        $Value,
        [System.Globalization.NumberStyles]::None,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [ref]$number
    )
}

function Test-RhiUInt32Hex([string]$Value) {
    if (-not (Test-RhiHex $Value)) { return $false }
    try { return ((ConvertFrom-RhiHex $Value) -le [UInt32]::MaxValue) }
    catch { return $false }
}

function ConvertFrom-RhiHex([string]$Value) {
    if (-not (Test-RhiHex $Value)) { throw "not a lowercase 0x hex value: $Value" }
    return [Convert]::ToUInt64($Value.Substring(2), 16)
}

function ConvertFrom-RhiHexArray([string]$Value, [int]$ExpectedCount, [string]$FieldName) {
    if ($Value -cnotmatch '^\[(?:0x[0-9a-f]+(?:,0x[0-9a-f]+)*)?\]$') {
        throw "$FieldName must be a bracketed comma-separated lowercase 0x array with no spaces"
    }
    $body = $Value.Substring(1, $Value.Length - 2)
    $items = if ($body.Length -eq 0) { @() } else { @($body -split ',') }
    if ($items.Count -ne $ExpectedCount) {
        throw "$FieldName contains $($items.Count) qword(s); expected $ExpectedCount"
    }
    foreach ($item in $items) {
        try { [void](ConvertFrom-RhiHex ([string]$item)) }
        catch { throw "$FieldName contains a value outside UInt64 range: $item" }
    }
    return ,@($items)
}

function Get-DeracineRhiProbeEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string[]]$Paths,
        [Parameter(Mandatory)][string]$PinnedImageSha256
    )

    $installFields = @(
        'title_id', 'app_version', 'image_sha256', 'module_name', 'module_base', 'target_rva',
        'target', 'exec_pt_load_rva', 'exec_pt_load_filesz', 'exec_pt_load_memsz', 'event_limit'
    )
    $eventFields = @(
        'sequence', 'thread_id', 'module_base', 'target', 'caller', 'entry_rsp', 'gpr_rdi',
        'gpr_rsi', 'gpr_rdx', 'gpr_rcx', 'gpr_r8', 'gpr_r9', 'ymm_args', 'stack_validated',
        'stack_count', 'stack_args'
    )
    $gnmFields = @(
        'gnm_sequence', 'rhi_sequence', 'thread_id', 'delta_us', 'rhi_module_base', 'rhi_target',
        'rhi_caller', 'cmdbuf', 'size', 'index_count', 'index_addr', 'flags', 'type'
    )
    $errors = [System.Collections.Generic.List[string]]::new()
    $install = $null
    $installCanonical = $null
    $installSources = [System.Collections.Generic.List[string]]::new()
    $eventsBySequence = [System.Collections.Generic.Dictionary[long, object]]::new()
    $gnmBySequence = [System.Collections.Generic.Dictionary[long, object]]::new()
    $recognizedRecords = 0
    $addressRelationChecked = $false
    $addressRelationPassed = $false

    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $errors.Add("probe log sink is missing: $path")
            continue
        }
        $lineNumber = 0
        foreach ($line in [IO.File]::ReadLines($path)) {
            $lineNumber++
            $lineText = [string]$line
            $prefix = $null
            $expected = $null
            if ($lineText.Contains('DERACINE_RHI_PROBE_INSTALL')) {
                $prefix = 'DERACINE_RHI_PROBE_INSTALL'
                $expected = $installFields
            } elseif ($lineText.Contains('DERACINE_RHI_PROBE_EVENT')) {
                $prefix = 'DERACINE_RHI_PROBE_EVENT'
                $expected = $eventFields
            } elseif ($lineText.Contains('DERACINE_GNM_DRAW_INDEX')) {
                $prefix = 'DERACINE_GNM_DRAW_INDEX'
                $expected = $gnmFields
            } else {
                continue
            }
            $recognizedRecords++
            $parsed = Get-RhiProbePayload -Line $lineText -Prefix $prefix -ExpectedFields $expected `
                -SourcePath $path -LineNumber $lineNumber
            if (-not $parsed.valid) {
                $errors.Add([string]$parsed.error)
                continue
            }
            $canonical = $parsed.values | ConvertTo-Json -Depth 8 -Compress
            if ($prefix -eq 'DERACINE_RHI_PROBE_INSTALL') {
                if ($null -eq $install) {
                    $install = $parsed.values
                    $installCanonical = $canonical
                    $installSources.Add($path)
                } elseif ($canonical -ceq $installCanonical) {
                    if (-not $installSources.Contains($path)) { $installSources.Add($path) }
                } else {
                    $errors.Add("$path`:$lineNumber has a conflicting second install record")
                }
                continue
            }
            if ($prefix -eq 'DERACINE_RHI_PROBE_EVENT') {
                $sequenceText = [string]$parsed.values.sequence
                if ($sequenceText -cnotmatch '^[1-9][0-9]*$') {
                    $errors.Add("$path`:$lineNumber sequence must be a positive decimal integer")
                    continue
                }
                $sequence = 0L
                if (-not [long]::TryParse($sequenceText, [ref]$sequence) -or $sequence -gt 128) {
                    $errors.Add("$path`:$lineNumber sequence must be in the inclusive range 1..128")
                    continue
                }
                if ($eventsBySequence.ContainsKey($sequence)) {
                    $existing = $eventsBySequence[$sequence]
                    if ($canonical -cne $existing.canonical) {
                        $errors.Add("$path`:$lineNumber conflicts with another event having sequence $sequence")
                    } elseif (-not $existing.sources.Contains($path)) {
                        $existing.sources.Add($path)
                    }
                    continue
                }
                if ($eventsBySequence.Count -ge 128) {
                    $errors.Add("$path`:$lineNumber exceeds the hard cap of 128 unique probe events")
                    continue
                }
                $event = [ordered]@{
                    sequence = $sequence
                    fields = $parsed.values
                    canonical = $canonical
                    sources = [System.Collections.Generic.List[string]]::new()
                }
                $event.sources.Add($path)
                $eventsBySequence.Add($sequence, $event)
                continue
            }

            $gnmSequenceText = [string]$parsed.values.gnm_sequence
            if ($gnmSequenceText -cnotmatch '^[1-9][0-9]*$') {
                $errors.Add("$path`:$lineNumber gnm_sequence must be a positive decimal integer")
                continue
            }
            $gnmSequence = 0L
            if (-not [long]::TryParse($gnmSequenceText, [ref]$gnmSequence) -or $gnmSequence -gt 128) {
                $errors.Add("$path`:$lineNumber gnm_sequence must be in the inclusive range 1..128")
                continue
            }
            if ($gnmBySequence.ContainsKey($gnmSequence)) {
                $existing = $gnmBySequence[$gnmSequence]
                if ($canonical -cne $existing.canonical) {
                    $errors.Add("$path`:$lineNumber conflicts with another GNM draw-index record having gnm_sequence $gnmSequence")
                } elseif (-not $existing.sources.Contains($path)) {
                    $existing.sources.Add($path)
                }
                continue
            }
            if ($gnmBySequence.Count -ge 128) {
                $errors.Add("$path`:$lineNumber exceeds the hard cap of 128 unique GNM draw-index records")
                continue
            }
            $gnmRecord = [ordered]@{
                gnm_sequence = $gnmSequence
                fields = $parsed.values
                canonical = $canonical
                sources = [System.Collections.Generic.List[string]]::new()
            }
            $gnmRecord.sources.Add($path)
            $gnmBySequence.Add($gnmSequence, $gnmRecord)
        }
    }

    if ($null -eq $install) {
        $errors.Add('no DERACINE_RHI_PROBE_INSTALL record was found')
    } else {
        if ($install.title_id -cne 'CUSA11919') { $errors.Add("install title_id is '$($install.title_id)', expected CUSA11919") }
        if ($install.app_version -cne '01.01') { $errors.Add("install app_version is '$($install.app_version)', expected 01.01") }
        if ($install.image_sha256 -cne $PinnedImageSha256.ToLowerInvariant()) {
            $errors.Add("install image_sha256 is '$($install.image_sha256)', expected pinned shadow hash '$($PinnedImageSha256.ToLowerInvariant())'")
        }
        if ($install.module_name -cne 'eboot.bin') { $errors.Add("install module_name is '$($install.module_name)', expected eboot.bin") }
        if ($install.target_rva -cne '0xcbb9c0') { $errors.Add("install target_rva is '$($install.target_rva)', expected 0xcbb9c0") }
        if ($install.event_limit -cne '128') { $errors.Add("install event_limit is '$($install.event_limit)', expected 128") }
        foreach ($field in @('module_base', 'target_rva', 'target', 'exec_pt_load_rva', 'exec_pt_load_filesz', 'exec_pt_load_memsz')) {
            if (-not (Test-RhiHex ([string]$install[$field]))) { $errors.Add("install $field is not lowercase 0x hex") }
        }
        if ($install.image_sha256 -cnotmatch '^[0-9a-f]{64}$') { $errors.Add('install image_sha256 is not a lowercase 64-character SHA-256') }
        if ($install.module_base -cmatch '^0x[0-9a-f]+$' -and
            $install.target_rva -cmatch '^0x[0-9a-f]+$' -and
            $install.target -cmatch '^0x[0-9a-f]+$') {
            try {
                $baseValue = ConvertFrom-RhiHex $install.module_base
                $rvaValue = ConvertFrom-RhiHex $install.target_rva
                $targetValue = ConvertFrom-RhiHex $install.target
                $maxValue = [UInt64]::MaxValue
                $addressRelationChecked = $true
                if ($baseValue -gt ($maxValue - $rvaValue)) {
                    $errors.Add('install module_base + target_rva overflows UInt64')
                } else {
                    $expectedTarget = [UInt64]($baseValue + $rvaValue)
                    if ($targetValue -ne $expectedTarget) {
                        $errors.Add('install target does not equal module_base + target_rva')
                    } else {
                        $addressRelationPassed = $true
                    }
                }
            } catch { $errors.Add("install address relation could not be checked: $($_.Exception.Message)") }
        }
        if ($install.exec_pt_load_rva -cmatch '^0x[0-9a-f]+$' -and
            $install.exec_pt_load_filesz -cmatch '^0x[0-9a-f]+$' -and
            $install.exec_pt_load_memsz -cmatch '^0x[0-9a-f]+$') {
            try {
                $fileSize = ConvertFrom-RhiHex $install.exec_pt_load_filesz
                $memorySize = ConvertFrom-RhiHex $install.exec_pt_load_memsz
                if ($fileSize -eq 0 -or $memorySize -lt $fileSize) {
                    $errors.Add('install PT_LOAD sizes are zero or memsz is smaller than filesz')
                }
            } catch { $errors.Add("install PT_LOAD size parse failed: $($_.Exception.Message)") }
        }
    }

    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($sequence in @($eventsBySequence.Keys | Sort-Object)) {
        $event = $eventsBySequence[$sequence]
        $fields = $event.fields
        $eventErrorsBefore = $errors.Count
        foreach ($field in @('module_base', 'target', 'caller', 'entry_rsp', 'gpr_rdi', 'gpr_rsi', 'gpr_rdx', 'gpr_rcx', 'gpr_r8', 'gpr_r9')) {
            if (-not (Test-RhiHex ([string]$fields[$field]))) { $errors.Add("event $sequence $field is not lowercase 0x hex") }
        }
        if (-not (Test-RhiUInt64Decimal ([string]$fields.thread_id))) {
            $errors.Add("event $sequence thread_id must be a positive UInt64 decimal integer")
        }
        if ($fields.stack_validated -cnotmatch '^[01]$') { $errors.Add("event $sequence stack_validated must be 0 or 1") }
        $stackCount = -1
        if ($fields.stack_count -cnotmatch '^[0-9]+$' -or -not [int]::TryParse([string]$fields.stack_count, [ref]$stackCount) -or $stackCount -lt 0 -or $stackCount -gt 8) {
            $errors.Add("event $sequence stack_count must be in the inclusive range 0..8")
        }
        $ymmQwords = @()
        $stackQwords = @()
        try { $ymmQwords = ConvertFrom-RhiHexArray ([string]$fields.ymm_args) 32 'ymm_args' }
        catch { $errors.Add("event $sequence $($_.Exception.Message)") }
        try { $stackQwords = ConvertFrom-RhiHexArray ([string]$fields.stack_args) 8 'stack_args' }
        catch { $errors.Add("event $sequence $($_.Exception.Message)") }
        if ($null -ne $install -and
            ($fields.module_base -cne $install.module_base -or $fields.target -cne $install.target)) {
            $errors.Add("event $sequence module_base/target do not match the install record")
        }
        $stackTrusted = ($fields.stack_validated -ceq '1' -and $stackCount -ge 0 -and $stackQwords.Count -eq 8)
        $validatedStack = @()
        if ($stackTrusted -and $stackCount -gt 0) { $validatedStack = @($stackQwords | Select-Object -First $stackCount) }
        $stackInterpretation = @(
            [ordered]@{ index = 0; name = 'StartIndex'; rbp_offset = '0x10'; entry_rsp_offset = '0x8' },
            [ordered]@{ index = 1; name = 'NumPrimitives'; rbp_offset = '0x18'; entry_rsp_offset = '0x10' },
            [ordered]@{ index = 2; name = 'NumInstances'; rbp_offset = '0x20'; entry_rsp_offset = '0x18' }
        )
        $stackValues = [System.Collections.Generic.List[object]]::new()
        foreach ($meaning in $stackInterpretation) {
            $index = [int]$meaning.index
            $isValid = ($stackTrusted -and $stackCount -gt $index)
            $stackValues.Add([ordered]@{
                index = $index
                name = $meaning.name
                raw_value = if ($index -lt $stackQwords.Count) { $stackQwords[$index] } else { $null }
                valid = $isValid
                value = if ($isValid) { $stackQwords[$index] } else { $null }
                rbp_offset = $meaning.rbp_offset
                entry_rsp_offset = $meaning.entry_rsp_offset
            })
        }
        $events.Add([ordered]@{
            sequence = $sequence
            fields = $fields
            ymm_qwords_low_to_high_register_order = @($ymmQwords)
            stack_args_raw = @($stackQwords)
            stack_args_trusted = $stackTrusted
            validated_stack_args = @($validatedStack)
            stack_arguments_interpreted = @($stackValues.ToArray())
            sources = @($event.sources.ToArray())
            record_valid = ($errors.Count -eq $eventErrorsBefore)
        })
    }

    if ($events.Count -gt 128) { $errors.Add('unique event count exceeded the hard cap of 128') }
    if ($gnmBySequence.Count -eq 0) {
        $errors.Add('no DERACINE_GNM_DRAW_INDEX record was found; indexed-draw evidence is required')
    }

    $gnmEvents = [System.Collections.Generic.List[object]]::new()
    foreach ($gnmSequence in @($gnmBySequence.Keys | Sort-Object)) {
        $gnmRecord = $gnmBySequence[$gnmSequence]
        $fields = $gnmRecord.fields
        $gnmErrorsBefore = $errors.Count
        foreach ($field in @('rhi_module_base', 'rhi_target', 'rhi_caller', 'cmdbuf', 'index_addr')) {
            if (-not (Test-RhiHex ([string]$fields[$field]))) {
                $errors.Add("GNM draw-index $gnmSequence $field is not lowercase 0x hex")
            } else {
                try { [void](ConvertFrom-RhiHex ([string]$fields[$field])) }
                catch { $errors.Add("GNM draw-index $gnmSequence $field is outside UInt64 range") }
            }
        }
        if (-not (Test-RhiUInt64Decimal ([string]$fields.thread_id))) {
            $errors.Add("GNM draw-index $gnmSequence thread_id must be a positive UInt64 decimal integer")
        }
        if (-not (Test-RhiUInt64Decimal ([string]$fields.rhi_sequence))) {
            $errors.Add("GNM draw-index $gnmSequence rhi_sequence must be a positive UInt64 decimal integer")
        }
        $rhiSequence = 0L
        $rhiSequenceValid = ([long]::TryParse([string]$fields.rhi_sequence, [ref]$rhiSequence) -and
            $rhiSequence -ge 1 -and $rhiSequence -le 128)
        if (-not $rhiSequenceValid) {
            $errors.Add("GNM draw-index $gnmSequence rhi_sequence must be in the inclusive range 1..128")
        }
        $deltaUs = [UInt64]0
        $deltaValid = (Test-RhiUInt64Decimal ([string]$fields.delta_us) -AllowZero) -and
            [UInt64]::TryParse([string]$fields.delta_us, [ref]$deltaUs)
        if (-not $deltaValid) {
            $errors.Add("GNM draw-index $gnmSequence delta_us must be a UInt64 decimal integer")
        } elseif ($deltaUs -gt 250000) {
            $errors.Add("GNM draw-index $gnmSequence delta_us exceeds the 250000 microsecond correlation window")
        }
        foreach ($field in @('size', 'index_count')) {
            if (-not (Test-RhiUInt32Decimal ([string]$fields[$field]))) {
                $errors.Add("GNM draw-index $gnmSequence $field must be a UInt32 decimal integer")
            }
        }
        foreach ($field in @('flags', 'type')) {
            if (-not (Test-RhiUInt32Hex ([string]$fields[$field]))) {
                $errors.Add("GNM draw-index $gnmSequence $field must be lowercase 0x hex within UInt32 range")
            }
        }

        $pairedRhi = $null
        if ($rhiSequenceValid -and $eventsBySequence.ContainsKey($rhiSequence)) {
            $pairedRhi = $eventsBySequence[$rhiSequence]
            $rhiFields = $pairedRhi.fields
            if ([string]$fields.thread_id -cne [string]$rhiFields.thread_id) {
                $errors.Add("GNM draw-index $gnmSequence thread_id does not match RHI event $rhiSequence")
            }
            foreach ($pair in @(
                @{ gnm = 'rhi_module_base'; rhi = 'module_base' },
                @{ gnm = 'rhi_target'; rhi = 'target' },
                @{ gnm = 'rhi_caller'; rhi = 'caller' }
            )) {
                if ([string]$fields[$pair.gnm] -cne [string]$rhiFields[$pair.rhi]) {
                    $errors.Add("GNM draw-index $gnmSequence $($pair.gnm) does not match RHI event $rhiSequence $($pair.rhi)")
                }
            }
        } elseif ($rhiSequenceValid) {
            $errors.Add("GNM draw-index $gnmSequence references missing RHI event sequence $rhiSequence")
        }

        $pairedRhiSummary = if ($null -eq $pairedRhi) { $null } else {
            [ordered]@{
                sequence = $rhiSequence
                thread_id = $pairedRhi.fields.thread_id
                module_base = $pairedRhi.fields.module_base
                target = $pairedRhi.fields.target
                caller = $pairedRhi.fields.caller
                sources = @($pairedRhi.sources.ToArray())
            }
        }
        $gnmEvents.Add([ordered]@{
            gnm_sequence = $gnmSequence
            pair_key = [ordered]@{ rhi_sequence = $fields.rhi_sequence; thread_id = $fields.thread_id }
            fields = $fields
            paired_rhi_event = $pairedRhiSummary
            sources = @($gnmRecord.sources.ToArray())
            record_valid = ($errors.Count -eq $gnmErrorsBefore)
        })
    }
    if ($gnmEvents.Count -gt 128) { $errors.Add('unique GNM draw-index count exceeded the hard cap of 128') }

    $schemaValid = ($errors.Count -eq 0)
    $traceObserved = ($events.Count -gt 0)
    $gnmTraceObserved = ($gnmEvents.Count -gt 0)
    return [ordered]@{
        schema = 'DERACINE_RHI_PROBE_INSTALL+DERACINE_RHI_PROBE_EVENT/v2+DERACINE_GNM_DRAW_INDEX'
        schema_valid = $schemaValid
        trace_observed = $traceObserved
        valid = ($schemaValid -and $traceObserved -and $gnmTraceObserved)
        install_record = if ($null -eq $install) { $null } else {
            [ordered]@{ fields = $install; sources = @($installSources.ToArray()) }
        }
        event_count = $events.Count
        event_limit = 128
        event_cap_reached = ($events.Count -eq 128)
        gnm_draw_index = [ordered]@{
            schema = 'DERACINE_GNM_DRAW_INDEX'
            required = $true
            trace_observed = $gnmTraceObserved
            event_count = $gnmEvents.Count
            event_limit = 128
            event_cap_reached = ($gnmEvents.Count -eq 128)
            pairing_key = @('rhi_sequence', 'thread_id')
            max_delta_us = 250000
            events = @($gnmEvents.ToArray())
        }
        recognized_record_count = $recognizedRecords
        install_address_relation = [ordered]@{
            equation = 'target == module_base + target_rva'
            overflow_checked = $true
            checked = $addressRelationChecked
            passed = $addressRelationPassed
        }
        events = @($events.ToArray())
        errors = @($errors.ToArray())
        identity_claim = 'RHI target/caller addresses are raw trace evidence; GNM records identify the observed sceGnmDrawIndex HLE entry only when paired by RHI sequence and thread.'
        gnm_observation_note = 'The GNM hook observes arguments before packet writes and does not dereference pointer values or prove packet-write success.'
        stack_argument_interpretation_source = 'prior A3 disassembly handoff; interpretation only, raw values remain authoritative'
        stack_argument_interpretation = @(
            [ordered]@{ index = 0; name = 'StartIndex'; rbp_offset = '0x10'; entry_rsp_offset = '0x8' },
            [ordered]@{ index = 1; name = 'NumPrimitives'; rbp_offset = '0x18'; entry_rsp_offset = '0x10' },
            [ordered]@{ index = 2; name = 'NumInstances'; rbp_offset = '0x20'; entry_rsp_offset = '0x18' }
        )
        gpr_arguments_note = 'gpr_rdi, gpr_rsi, gpr_rdx, gpr_rcx, gpr_r8, and gpr_r9 are preserved separately as call-register values; they are not the UE entry stack arguments above.'
        stack_args_note = 'All eight qwords are retained raw; only the first stack_count are valid when stack_validated=1. If stack_validated=0, none are trusted.'
        ymm_args_note = 'Exactly 32 qwords: four 64-bit chunks per register, low-to-high for YMM0 through YMM7 in register order.'
    }
}
