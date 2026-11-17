# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Positive process ownership for benchmark cleanup.
#
# The runner used to clean up by enumerating every process named shadps4 and stopping the ones
# started after the run began - a global sweep that kills every emulator on the machine, including
# a session another agent started. This module records the processes the run itself launched and
# stops only those, after re-reading a fresh process table and re-verifying identity.
#
# Dot-source it; it has no load-time side effects and never kills anything by name:
#     . (Join-Path $PSScriptRoot 'process_ownership.ps1')
#
# The invariants:
#   * Ownership is (pid, creation time, executable path) captured at launch - never a process name.
#   * A process whose identity could not be captured is reported with a reason, never stopped.
#   * A pid whose creation time or executable path does not match the launch-time capture (pid
#     reuse, a different image, a different lab) is reported, never stopped.
#   * Descendants are adopted through live parent links only, and only when a child was created at
#     or after its parent.
#   * Every timestamp in a record, a table row and a result is UTC.
#
# The module holds no state: the caller owns the record collection (see Register-LaunchedProcess).
#
# Host limitations, all of which fail closed (report, kill nothing) rather than guess:
#   * Win32_Process needs a working CIM/WMI service. If the snapshot cannot be read,
#     Stop-OwnedProcesses reports ambiguity and stops nothing.
#   * ExecutablePath is blank for some protected or exited processes; such a process is not
#     confirmed.
#   * Win32_Process.CreationDate carries no timezone. Get-ProcessTable probes the reported
#     interpretation against .NET StartTime and applies the matching one, so a misreported
#     timezone makes identities unconfirmed instead of confirming the wrong process.
#   * If a wrapper (renderdoccmd) exits before its child is read, the child link is gone and the
#     child is reported as ambiguous instead of being guessed at.

# --- internal helpers -----------------------------------------------------------------------

function Get-ProcessOwnershipField {
    # Tolerant property read: records and snapshot rows may be PSCustomObjects or hashtables, and
    # a missing field must be distinguishable from a false value.
    [CmdletBinding()]
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory)][string]$Name
    )
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function ConvertTo-ProcessOwnershipUtc {
    # Normalise a captured timestamp. Records and table rows carry UTC, so an Unspecified value
    # (a caller-built test row, for instance) is read as UTC rather than as local time.
    param([AllowNull()]$Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [datetimeoffset]) { return $Value.UtcDateTime }
    if ($Value -isnot [datetime]) { return $null }
    $stamp = [datetime]$Value
    if ($stamp.Kind -eq [System.DateTimeKind]::Utc) { return $stamp }
    if ($stamp.Kind -eq [System.DateTimeKind]::Local) { return $stamp.ToUniversalTime() }
    return [datetime]::SpecifyKind($stamp, [System.DateTimeKind]::Utc)
}

function ConvertTo-ProcessOwnershipPathKey {
    # Canonical form used only for comparison: absolute path, no extended-length prefix, no
    # trailing separator. Casing is handled by the comparison itself (Windows is case-insensitive).
    param([AllowNull()][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    $value = $Path.Trim().Trim('"')
    try { $value = [System.IO.Path]::GetFullPath($value) } catch { }
    $value = $value -replace '^\\\\\?\\', ''
    return $value.TrimEnd([char]'\', [char]'/')
}

function Test-ProcessOwnershipPathEqual {
    param([AllowNull()][string]$Left, [AllowNull()][string]$Right)
    $leftKey = ConvertTo-ProcessOwnershipPathKey $Left
    $rightKey = ConvertTo-ProcessOwnershipPathKey $Right
    if ($null -eq $leftKey -or $null -eq $rightKey) { return $false }
    return [string]::Equals($leftKey, $rightKey, [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-ProcessOwnershipMedian {
    param([System.Collections.Generic.List[double]]$Values)
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count
    if ($count -eq 0) { return [double]::MaxValue }
    if ($count % 2 -eq 1) { return [double]$sorted[[int](($count - 1) / 2)] }
    return ([double]$sorted[$count / 2 - 1] + [double]$sorted[$count / 2]) / 2.0
}

function New-ProcessOwnershipIdentityResult {
    param([bool]$Confirmed, [string]$Reason)
    return [pscustomobject]@{ Confirmed = $Confirmed; Reason = $Reason }
}

function New-ProcessOwnershipAmbiguity {
    # Code is part of the report so a caller can tell "nothing left to stop" from "could not
    # establish ownership": absent / rejected-child / duplicate-root are benign, identity and
    # chain mean the owned set could not be established, and snapshot/snapshot-empty mean the
    # table itself was unusable.
    param([int]$Id, [string]$Role, [string]$Code, [string]$Reason)
    return [pscustomobject]@{ Id = $Id; Role = $Role; Code = $Code; Reason = $Reason }
}

function New-ProcessOwnershipEntry {
    param(
        [int]$Id,
        [AllowNull()]$StartTimeUtc,
        [AllowNull()][string]$Path,
        [string]$Role,
        [string]$RootRole,
        [int]$ParentProcessId,
        [int]$RootId,
        [int]$Depth,
        [string]$Origin,
        [AllowNull()]$RootRecord,
        [int[]]$Chain
    )
    return [pscustomobject]@{
        Id              = $Id
        StartTimeUtc    = $StartTimeUtc
        Path            = $Path
        Role            = $Role
        RootRole        = $RootRole
        ParentProcessId = $ParentProcessId
        RootId          = $RootId
        Depth           = $Depth
        Origin          = $Origin
        RootRecord      = $RootRecord
        Chain           = $Chain
    }
}

function Get-ProcessOwnershipRowMap {
    # pid -> first row for that pid. Duplicate pids make a link ambiguous; callers of the owned-set
    # resolver reject those identities rather than picking one.
    param([AllowNull()]$Table)
    $map = @{}
    foreach ($row in @($Table)) {
        if ($null -eq $row) { continue }
        $id = [int](Get-ProcessOwnershipField $row 'ProcessId')
        if (-not $map.ContainsKey($id)) { $map[$id] = $row }
    }
    return $map
}

function Get-ProcessOwnershipChainProblem {
    # Re-verifies the parent chain of an owned entry against the snapshot that is about to be
    # acted on. Returns an empty string when the chain is intact, otherwise the reason.
    param($Entry, [hashtable]$RowMap)
    $chain = @($Entry.Chain)
    if ($chain.Count -eq 0) { $chain = @($Entry.Id) }
    $previous = $null
    foreach ($id in $chain) {
        $row = $RowMap[[int]$id]
        if ($null -eq $row) {
            return ("process {0} left the snapshot before it could be stopped" -f $id)
        }
        if ($null -ne $previous) {
            $expectedParent = [int](Get-ProcessOwnershipField $previous 'ProcessId')
            $actualParent = [int](Get-ProcessOwnershipField $row 'ParentProcessId')
            if ($actualParent -ne $expectedParent) {
                return ("process {0} is no longer a child of {1}" -f $id, $expectedParent)
            }
            $childStart = ConvertTo-ProcessOwnershipUtc (Get-ProcessOwnershipField $row 'CreationDateUtc')
            $parentStart = ConvertTo-ProcessOwnershipUtc (Get-ProcessOwnershipField $previous 'CreationDateUtc')
            if ($null -eq $childStart -or $null -eq $parentStart -or $childStart -lt $parentStart) {
                return ("process {0} no longer starts after its parent" -f $id)
            }
        }
        $previous = $row
    }
    return ''
}

# --- public contract ------------------------------------------------------------------------

function Get-ProcessTable {
    # One Win32_Process snapshot, UTC creation times. Used to resolve descendants and to
    # re-verify identity at stop time. Callers wrap the result in @() when they need array
    # semantics for a single-process table.
    [CmdletBinding()]
    param()

    try {
        $snapshot = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)
    } catch {
        throw "process snapshot unavailable: $($_.Exception.Message)"
    }
    if ($snapshot.Count -eq 0) { throw 'process snapshot returned no rows' }

    # CreationDate is a wall clock with no timezone. Probe both readings against .NET StartTime
    # (the same kernel creation instant) and apply whichever fits; an unverifiable reading is
    # still used, but it can only fail identity checks, never confirm a process we did not start.
    $localDeltas = [System.Collections.Generic.List[double]]::new()
    $utcDeltas = [System.Collections.Generic.List[double]]::new()
    $probed = @{}
    $probeRows = @($snapshot | Where-Object { [int]$_.ProcessId -eq $PID }) + @($snapshot | Select-Object -First 8)
    foreach ($row in $probeRows) {
        if ($null -eq $row) { continue }
        $rowId = [int]$row.ProcessId
        if ($probed.ContainsKey($rowId)) { continue }
        if ($row.CreationDate -isnot [datetime]) { continue }
        try { $startUtc = (Get-Process -Id $rowId -ErrorAction Stop).StartTime.ToUniversalTime() } catch { continue }
        $probed[$rowId] = $true
        $wall = [datetime]::SpecifyKind([datetime]$row.CreationDate, [System.DateTimeKind]::Unspecified)
        $localDeltas.Add([math]::Abs(($wall.ToUniversalTime() - $startUtc).TotalSeconds))
        $utcDeltas.Add([math]::Abs(([datetime]::SpecifyKind($wall, [System.DateTimeKind]::Utc) - $startUtc).TotalSeconds))
    }
    $timeBase = 'local'
    if ($localDeltas.Count -gt 0) {
        $localMedian = Get-ProcessOwnershipMedian $localDeltas
        $utcMedian = Get-ProcessOwnershipMedian $utcDeltas
        if ($utcMedian -lt $localMedian) { $timeBase = 'utc' }
        $best = [math]::Min($localMedian, $utcMedian)
        if ($best -gt 5.0) {
            Write-Verbose ("process table creation-time base is unverified (best fit {0:N1} s); identities will fail closed" -f $best)
        }
    }
    Write-Verbose ("process table creation-time base: {0}" -f $timeBase)

    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $snapshot) {
        if ($null -eq $row) { continue }
        $created = $null
        if ($row.CreationDate -is [datetime]) {
            $wall = [datetime]::SpecifyKind([datetime]$row.CreationDate, [System.DateTimeKind]::Unspecified)
            if ($timeBase -eq 'utc') {
                $created = [datetime]::SpecifyKind($wall, [System.DateTimeKind]::Utc)
            } else {
                $created = $wall.ToUniversalTime()
            }
        }
        $executablePath = $null
        if ($null -ne $row.ExecutablePath -and -not [string]::IsNullOrWhiteSpace([string]$row.ExecutablePath)) {
            $executablePath = [string]$row.ExecutablePath
        }
        $rows.Add([pscustomobject]@{
            ProcessId       = [int]$row.ProcessId
            ParentProcessId = [int]$row.ParentProcessId
            CreationDateUtc = $created
            ExecutablePath  = $executablePath
            Name            = $(if ($null -ne $row.Name) { [string]$row.Name } else { '' })
        })
    }
    return $rows.ToArray()
}

function New-OwnedProcessRecord {
    # Captures identity at launch time. Returns the record with IdentityAvailable=$false and a
    # Reason when the creation time or the executable path cannot be read, so cleanup reports the
    # process instead of guessing.
    [CmdletBinding(DefaultParameterSetName = 'FromProcess')]
    param(
        [Parameter(Mandatory, ParameterSetName = 'FromProcess', Position = 0)]
        [System.Diagnostics.Process]$Process,

        [Parameter(Mandatory, ParameterSetName = 'FromIdentity')]
        [int]$ProcessId,

        [Parameter(ParameterSetName = 'FromIdentity')]
        [datetime]$CreationDateUtc,

        [Parameter(ParameterSetName = 'FromIdentity')]
        [string]$Path = '',

        [Parameter(Mandatory)][string]$Role,
        [string]$ExpectedPath = ''
    )

    $identityAvailable = $true
    $reasons = [System.Collections.Generic.List[string]]::new()
    $id = 0
    $started = $null
    $capturedPath = $null

    if ($PSCmdlet.ParameterSetName -eq 'FromProcess') {
        $id = [int]$Process.Id
        $exited = $false
        try { $exited = $Process.HasExited } catch { }
        if ($exited) {
            $identityAvailable = $false
            $reasons.Add('process has already exited, so its identity cannot be re-verified')
        }
        try {
            $started = ConvertTo-ProcessOwnershipUtc $Process.StartTime
        } catch {
            $identityAvailable = $false
            $reasons.Add("creation time unreadable: $($_.Exception.Message)")
        }
        try {
            $capturedPath = [string]$Process.Path
        } catch {
            $identityAvailable = $false
            $reasons.Add("executable path unreadable: $($_.Exception.Message)")
        }
    } else {
        $id = $ProcessId
        if ($PSBoundParameters.ContainsKey('CreationDateUtc')) {
            $started = ConvertTo-ProcessOwnershipUtc $CreationDateUtc
            if ($null -eq $started) {
                $identityAvailable = $false
                $reasons.Add('creation time could not be normalised to UTC')
            }
        } else {
            $identityAvailable = $false
            $reasons.Add('no creation time was supplied')
        }
        $capturedPath = $Path
    }

    if ($null -eq $started) {
        $identityAvailable = $false
        if ($reasons.Count -eq 0) { $reasons.Add('creation time unreadable') }
    }
    if ([string]::IsNullOrWhiteSpace($capturedPath)) {
        if (-not [string]::IsNullOrWhiteSpace($ExpectedPath)) {
            $capturedPath = $ExpectedPath
        } else {
            $identityAvailable = $false
            if ($reasons.Count -eq 0) { $reasons.Add('executable path unreadable') }
        }
    }

    return [pscustomobject]@{
        Id                = $id
        StartTimeUtc      = $started
        Path              = $capturedPath
        ExpectedPath      = $ExpectedPath
        Role              = $Role
        IdentityAvailable = [bool]$identityAvailable
        Reason            = ($reasons -join '; ')
    }
}

function Register-LaunchedProcess {
    # Records a just-launched process. Pass a mutable list through -Into (for example
    # [System.Collections.Generic.List[object]]::new()); without -Into the record is only returned,
    # which keeps the module free of state.
    [CmdletBinding()]
    param(
        [Parameter(Mandatory, Position = 0)]
        [System.Diagnostics.Process]$Process,

        [Parameter(Mandatory)][string]$Role,
        [string]$ExpectedPath = '',
        [System.Collections.IList]$Into
    )

    $record = New-OwnedProcessRecord -Process $Process -Role $Role -ExpectedPath $ExpectedPath
    if ($null -ne $Into) {
        try {
            [void]$Into.Add($record)
        } catch {
            throw ("cannot append an owned-process record to '{0}': {1}. Pass a mutable list (List[object]) through -Into." -f `
                    $Into.GetType().FullName, $_.Exception.Message)
        }
    }
    return $record
}

function Get-OwnedProcessSet {
    # Resolves roots plus descendants through live parent links. Roots must match their launch-time
    # identity; descendants are adopted only when created at or after their parent and reachable
    # through a single unambiguous parent row.
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowNull()]$Records,
        [Parameter(Mandatory)][AllowNull()]$Table
    )

    $owned = [System.Collections.Generic.List[object]]::new()
    $ambiguous = [System.Collections.Generic.List[object]]::new()
    $recordsArray = @($Records)
    $tableRows = @($Table)

    $rowsById = @{}
    $duplicateIds = @{}
    $childrenByParent = @{}
    foreach ($row in $tableRows) {
        if ($null -eq $row) { continue }
        $rowId = [int](Get-ProcessOwnershipField $row 'ProcessId')
        if ($rowsById.ContainsKey($rowId)) {
            $duplicateIds[$rowId] = $true
            continue
        }
        $rowsById[$rowId] = $row
        $parentId = [int](Get-ProcessOwnershipField $row 'ParentProcessId')
        if (-not $childrenByParent.ContainsKey($parentId)) {
            $childrenByParent[$parentId] = [System.Collections.Generic.List[object]]::new()
        }
        $childrenByParent[$parentId].Add($row)
    }

    $claimed = @{}
    $queue = [System.Collections.Generic.Queue[object]]::new()

    foreach ($record in $recordsArray) {
        if ($null -eq $record) { continue }
        $recordId = [int](Get-ProcessOwnershipField $record 'Id')
        $role = [string](Get-ProcessOwnershipField $record 'Role')

        if ((Get-ProcessOwnershipField $record 'IdentityAvailable') -eq $false) {
            $reason = [string](Get-ProcessOwnershipField $record 'Reason')
            if ([string]::IsNullOrWhiteSpace($reason)) { $reason = 'identity was not captured at launch' }
            [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $recordId -Role $role -Code 'identity' `
                        -Reason ("identity unavailable at launch: {0}" -f $reason)))
            continue
        }
        if (-not $rowsById.ContainsKey($recordId)) {
            [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $recordId -Role $role -Code 'absent' `
                        -Reason 'process is not in the snapshot (it has already exited)'))
            continue
        }
        if ($claimed.ContainsKey($recordId)) {
            [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $recordId -Role $role -Code 'duplicate-root' `
                        -Reason 'process is already owned by another record'))
            continue
        }
        if ($duplicateIds.ContainsKey($recordId)) {
            [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $recordId -Role $role -Code 'identity' `
                        -Reason 'process id is not unique in the snapshot'))
            continue
        }

        $row = $rowsById[$recordId]
        $identity = Test-ProcessIdentity -Owned $record -Candidate $row
        if (-not $identity.Confirmed) {
            [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $recordId -Role $role -Code 'identity' -Reason $identity.Reason))
            continue
        }

        $claimed[$recordId] = $recordId
        $entry = New-ProcessOwnershipEntry -Id $recordId `
            -StartTimeUtc (ConvertTo-ProcessOwnershipUtc (Get-ProcessOwnershipField $row 'CreationDateUtc')) `
            -Path ([string](Get-ProcessOwnershipField $row 'ExecutablePath')) `
            -Role $role -RootRole $role `
            -ParentProcessId ([int](Get-ProcessOwnershipField $row 'ParentProcessId')) `
            -RootId $recordId -Depth 0 -Origin 'record' -RootRecord $record -Chain @($recordId)
        [void]$owned.Add($entry)
        $queue.Enqueue($entry)
    }

    while ($queue.Count -gt 0) {
        $parent = $queue.Dequeue()
        $parentId = [int]$parent.Id
        if (-not $childrenByParent.ContainsKey($parentId)) { continue }
        foreach ($child in $childrenByParent[$parentId]) {
            $childId = [int](Get-ProcessOwnershipField $child 'ProcessId')
            if ($claimed.ContainsKey($childId)) { continue }
            if ($duplicateIds.ContainsKey($childId)) {
                [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $childId -Role $parent.RootRole -Code 'identity' `
                            -Reason 'process id is not unique in the snapshot'))
                continue
            }
            $childStart = ConvertTo-ProcessOwnershipUtc (Get-ProcessOwnershipField $child 'CreationDateUtc')
            $parentStart = ConvertTo-ProcessOwnershipUtc $parent.StartTimeUtc
            if ($null -eq $childStart -or $null -eq $parentStart -or $childStart -lt $parentStart) {
                [void]$ambiguous.Add((New-ProcessOwnershipAmbiguity -Id $childId -Role $parent.RootRole -Code 'rejected-child' `
                            -Reason ("process {0} starts before its parent {1}; not adopted" -f $childId, $parentId)))
                continue
            }
            $claimed[$childId] = $parent.RootId
            $chain = @($parent.Chain) + @($childId)
            $entry = New-ProcessOwnershipEntry -Id $childId -StartTimeUtc $childStart `
                -Path ([string](Get-ProcessOwnershipField $child 'ExecutablePath')) `
                -Role 'descendant' -RootRole $parent.RootRole `
                -ParentProcessId $parentId -RootId ([int]$parent.RootId) -Depth ([int]$parent.Depth + 1) `
                -Origin 'descendant' -RootRecord $parent.RootRecord -Chain $chain
            [void]$owned.Add($entry)
            $queue.Enqueue($entry)
        }
    }

    return [pscustomobject]@{
        Owned     = @($owned.ToArray())
        Ambiguous = @($ambiguous.ToArray())
    }
}

function Test-ProcessIdentity {
    # Confirms that a snapshot row is the process the record was captured from. Confirmation needs
    # all three of pid, creation time (within ToleranceSeconds) and executable path; anything less
    # is reported with a reason and the caller must not stop it.
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowNull()]$Owned,
        [Parameter(Mandatory)][AllowNull()]$Candidate,
        [double]$ToleranceSeconds = 2.0
    )

    if ($null -eq $Owned) {
        return New-ProcessOwnershipIdentityResult $false 'no owned record was supplied'
    }
    if ((Get-ProcessOwnershipField $Owned 'IdentityAvailable') -eq $false) {
        $reason = [string](Get-ProcessOwnershipField $Owned 'Reason')
        if ([string]::IsNullOrWhiteSpace($reason)) { $reason = 'identity was not captured at launch' }
        return New-ProcessOwnershipIdentityResult $false ("identity unavailable at launch: {0}" -f $reason)
    }
    if ($null -eq $Candidate) {
        return New-ProcessOwnershipIdentityResult $false 'process is not present in the snapshot'
    }

    $ownedId = [int](Get-ProcessOwnershipField $Owned 'Id')
    $candidateId = [int](Get-ProcessOwnershipField $Candidate 'ProcessId')
    if ($ownedId -ne $candidateId) {
        return New-ProcessOwnershipIdentityResult $false ("process id {0} does not match owned id {1}" -f $candidateId, $ownedId)
    }

    $ownedStart = ConvertTo-ProcessOwnershipUtc (Get-ProcessOwnershipField $Owned 'StartTimeUtc')
    $candidateStart = ConvertTo-ProcessOwnershipUtc (Get-ProcessOwnershipField $Candidate 'CreationDateUtc')
    if ($null -eq $ownedStart) {
        return New-ProcessOwnershipIdentityResult $false 'owned record has no creation time'
    }
    if ($null -eq $candidateStart) {
        return New-ProcessOwnershipIdentityResult $false ("snapshot has no creation time for pid {0}" -f $ownedId)
    }
    $deltaSeconds = ($candidateStart - $ownedStart).TotalSeconds
    if ([math]::Abs($deltaSeconds) -gt $ToleranceSeconds) {
        if ($deltaSeconds -gt 0) {
            return New-ProcessOwnershipIdentityResult $false ("pid {0} was reused by a later process (snapshot {1:o}, launch {2:o})" -f `
                    $ownedId, $candidateStart, $ownedStart)
        }
        return New-ProcessOwnershipIdentityResult $false ("creation time differs (snapshot {0:o}, launch {1:o})" -f $candidateStart, $ownedStart)
    }

    $ownedPath = [string](Get-ProcessOwnershipField $Owned 'Path')
    $candidatePath = [string](Get-ProcessOwnershipField $Candidate 'ExecutablePath')
    if ([string]::IsNullOrWhiteSpace($ownedPath)) {
        return New-ProcessOwnershipIdentityResult $false 'owned record has no executable path'
    }
    if ([string]::IsNullOrWhiteSpace($candidatePath)) {
        return New-ProcessOwnershipIdentityResult $false ("snapshot has no executable path for pid {0}" -f $ownedId)
    }
    if (-not (Test-ProcessOwnershipPathEqual $ownedPath $candidatePath)) {
        return New-ProcessOwnershipIdentityResult $false ("executable path differs: snapshot '{0}', owned '{1}'" -f $candidatePath, $ownedPath)
    }

    return New-ProcessOwnershipIdentityResult $true 'pid, creation time and executable path match'
}

function Stop-OwnedProcesses {
    # Stops the owned set and nothing else. Reads a fresh table, re-derives the owned set,
    # re-verifies every candidate's root identity and parent chain against that same table, and
    # only then calls Stop-Process by verified id. Safe to call again (trap + Fail-Run + normal
    # cleanup): already exited processes are reported as absent, not as failures.
    #
    # -TableProvider : scriptblock returning the fresh table (normally { Get-ProcessTable }).
    # -Log           : scriptblock invoked for progress messages as a side effect. Its pipeline
    #                  output is discarded so the structured report stays the function's only return
    #                  value: use Write-Host/Write-Verbose, or render the returned report.
    # -StopAction    : optional scriptblock receiving the process id, used by tests to simulate
    #                  stops. When supplied, exit verification is skipped because no real stop
    #                  happened.
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowNull()]$Records,
        [Parameter(Mandatory)][scriptblock]$TableProvider,
        [scriptblock]$Log = { param([string]$Message) Write-Verbose $Message },
        [scriptblock]$StopAction,
        [int]$ExitWaitSeconds = 3
    )

    $stopped = [System.Collections.Generic.List[object]]::new()
    $skipped = [System.Collections.Generic.List[object]]::new()
    $ambiguous = $false

    if (@($Records).Count -eq 0) {
        # Nothing was launched (a trap before the Start-Process, for instance): no snapshot and no
        # stop, so cleanup is safe even when CIM is unavailable.
        & $Log 'owned cleanup: no owned processes were recorded, nothing to stop' | Out-Null
        return [pscustomobject]@{ Stopped = @(); Skipped = @(); Ambiguous = $false }
    }

    $table = $null
    try {
        $table = & $TableProvider
    } catch {
        & $Log ("owned cleanup: no process snapshot, nothing was stopped: {0}" -f $_.Exception.Message) | Out-Null
        [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id 0 -Role '' -Code 'snapshot' `
                    -Reason ("process snapshot failed: {0}" -f $_.Exception.Message)))
        return [pscustomobject]@{ Stopped = @(); Skipped = @($skipped.ToArray()); Ambiguous = $true }
    }
    if ($null -eq $table) {
        & $Log 'owned cleanup: the process snapshot was empty, nothing was stopped' | Out-Null
        [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id 0 -Role '' -Code 'snapshot' `
                    -Reason 'process snapshot returned no rows'))
        return [pscustomobject]@{ Stopped = @(); Skipped = @($skipped.ToArray()); Ambiguous = $true }
    }

    $rows = @($table)
    $rowMap = Get-ProcessOwnershipRowMap $rows
    $set = Get-OwnedProcessSet -Records $Records -Table $rows

    foreach ($ambiguity in @($set.Ambiguous)) {
        if ($null -eq $ambiguity) { continue }
        [void]$skipped.Add($ambiguity)
        if ($ambiguity.Code -eq 'identity') { $ambiguous = $true }
    }

    $customStop = $PSBoundParameters.ContainsKey('StopAction') -and $null -ne $StopAction
    if (-not $customStop) {
        $StopAction = { param([int]$ProcessId) Stop-Process -Id $ProcessId -Force -ErrorAction Stop }
    }

    # Deepest first: a child goes before its parent, so stopping the wrapper cannot orphan the
    # emulator behind a parent link the chain check depends on.
    $ordered = @($set.Owned | Sort-Object -Property Depth -Descending)
    foreach ($entry in $ordered) {
        $entryId = [int]$entry.Id
        $rootRow = $rowMap[[int]$entry.RootId]
        $identity = Test-ProcessIdentity -Owned $entry.RootRecord -Candidate $rootRow
        if (-not $identity.Confirmed) {
            [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id $entryId -Role $entry.Role -Code 'identity' `
                        -Reason ("root {0}: {1}" -f $entry.RootId, $identity.Reason)))
            $ambiguous = $true
            continue
        }
        $chainProblem = Get-ProcessOwnershipChainProblem -Entry $entry -RowMap $rowMap
        if (-not [string]::IsNullOrEmpty($chainProblem)) {
            [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id $entryId -Role $entry.Role -Code 'chain' -Reason $chainProblem))
            $ambiguous = $true
            continue
        }
        if ($entryId -eq $PID) {
            [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id $entryId -Role $entry.Role -Code 'self' `
                        -Reason 'refusing to stop the benchmark harness process itself'))
            continue
        }
        try {
            & $StopAction $entryId
        } catch {
            [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id $entryId -Role $entry.Role -Code 'stop-failed' `
                        -Reason ("Stop-Process failed: {0}" -f $_.Exception.Message)))
            $ambiguous = $true
            continue
        }
        [void]$stopped.Add([pscustomobject]@{
                Id       = $entryId
                Role     = $entry.Role
                RootRole = $entry.RootRole
                Path     = $entry.Path
                Origin   = $entry.Origin
            })
        & $Log ("owned cleanup: stopped {0} '{1}' (pid {2})" -f $entry.Origin, $entry.Role, $entryId) | Out-Null
    }

    if (-not $customStop -and $ExitWaitSeconds -gt 0 -and $stopped.Count -gt 0) {
        $deadline = [datetime]::UtcNow.AddSeconds($ExitWaitSeconds)
        foreach ($record in @($stopped.ToArray())) {
            while ($true) {
                $alive = $null -ne (Get-Process -Id ([int]$record.Id) -ErrorAction SilentlyContinue)
                if (-not $alive) { break }
                if ([datetime]::UtcNow -ge $deadline) {
                    [void]$skipped.Add((New-ProcessOwnershipAmbiguity -Id ([int]$record.Id) -Role $record.Role -Code 'still-running' `
                                -Reason 'process is still alive after Stop-Process'))
                    [void]$stopped.Remove($record)
                    $ambiguous = $true
                    break
                }
                Start-Sleep -Milliseconds 100
            }
        }
    }

    & $Log ("owned cleanup: stopped {0}, skipped {1}, ambiguous {2}" -f $stopped.Count, $skipped.Count, ([bool]$ambiguous)) | Out-Null
    return [pscustomobject]@{
        Stopped   = @($stopped.ToArray())
        Skipped   = @($skipped.ToArray())
        Ambiguous = [bool]$ambiguous
    }
}
