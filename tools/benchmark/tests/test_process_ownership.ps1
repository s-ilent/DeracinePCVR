# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Automated tests for tools/benchmark/process_ownership.ps1.
#
# Plain PowerShell, no Pester: dot-source the module, feed SIMULATED process tables, and assert
# on the result. Nothing here starts or stops a real process: Stop-OwnedProcesses is always called
# with an injected -StopAction that only records the ids it was asked to stop.
#
# Run: pwsh -NoProfile -File tools\benchmark\tests\test_process_ownership.ps1
# Exit code 0 means every case passed.

$ErrorActionPreference = 'Stop'

$modulePath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'process_ownership.ps1'
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) {
    throw "module not found: $modulePath"
}
. $modulePath

$script:baseTime = [datetime]::new(2026, 11, 18, 2, 0, 0, [System.DateTimeKind]::Utc)
$script:labExe = 'C:\lab\wt-fix-procown\_lab\pc-vr\shadps4.exe'
$script:renderDocExe = 'C:\Program Files\RenderDoc\renderdoccmd.exe'
$script:caseCount = 0
$script:failures = 0

function New-TestTime([double]$OffsetSeconds) {
    return $script:baseTime.AddSeconds($OffsetSeconds)
}

function New-TestRecord {
    param(
        [int]$Id,
        [datetime]$StartTimeUtc = $script:baseTime,
        [string]$Path = $script:labExe,
        [string]$Role = 'emulator',
        [bool]$IdentityAvailable = $true,
        [string]$Reason = ''
    )
    return [pscustomobject]@{
        Id                = $Id
        StartTimeUtc      = $StartTimeUtc
        Path              = $Path
        Role              = $Role
        IdentityAvailable = $IdentityAvailable
        Reason            = $Reason
    }
}

function New-TestRow {
    param(
        [int]$ProcessId,
        [int]$ParentProcessId = 1,
        [datetime]$CreationDateUtc = $script:baseTime,
        [string]$ExecutablePath = $script:labExe,
        [string]$Name = 'shadps4.exe'
    )
    return [pscustomobject]@{
        ProcessId       = $ProcessId
        ParentProcessId = $ParentProcessId
        CreationDateUtc = $CreationDateUtc
        ExecutablePath  = $ExecutablePath
        Name            = $Name
    }
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw ("assertion failed: {0}" -f $Message) }
}

function Assert-Equal {
    param([AllowNull()]$Expected, [AllowNull()]$Actual, [string]$Message)
    if ("$Expected" -ne "$Actual") {
        throw ("assertion failed: {0} (expected '{1}', got '{2}')" -f $Message, $Expected, $Actual)
    }
}

function Assert-Ids {
    param(
        [AllowNull()]$Objects,
        [AllowEmptyCollection()][int[]]$Expected,
        [string]$Message
    )
    # Accepts objects with an Id property (owned/skipped entries) and bare ints (stop recorders).
    $actual = @(@($Objects) | Where-Object { $null -ne $_ } | ForEach-Object {
            if ($_ -is [int] -or $_ -is [long]) { [int]$_ } else { [int]$_.Id }
        } | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join ',') -ne ($wanted -join ',')) {
        throw ("assertion failed: {0} (expected ids [{1}], got [{2}])" -f $Message, ($wanted -join ','), ($actual -join ','))
    }
}

function Get-Codes {
    param([AllowNull()]$Ambiguities)
    return @(@($Ambiguities) | Where-Object { $null -ne $_ } | ForEach-Object { [string]$_.Code })
}

function Invoke-Case {
    param([string]$Name, [scriptblock]$Body)
    $script:caseCount++
    try {
        & $Body
        Write-Host ("PASS {0}: {1}" -f $script:caseCount, $Name)
    } catch {
        $script:failures++
        Write-Host ("FAIL {0}: {1} -- {2}" -f $script:caseCount, $Name, $_.Exception.Message)
    }
}

# --- case 1 -----------------------------------------------------------------------------------
Invoke-Case 'two independent sessions: only the owned root and its descendants stop' {
    $records = @(New-TestRecord -Id 1000 -StartTimeUtc (New-TestTime 0))
    $rows = @(
        New-TestRow -ProcessId 1000 -ParentProcessId 900 -CreationDateUtc (New-TestTime 0)
        New-TestRow -ProcessId 1004 -ParentProcessId 1000 -CreationDateUtc (New-TestTime 2)
        New-TestRow -ProcessId 1001 -ParentProcessId 900 -CreationDateUtc (New-TestTime 30)
        New-TestRow -ProcessId 1002 -ParentProcessId 1001 -CreationDateUtc (New-TestTime 31)
        New-TestRow -ProcessId 999 -ParentProcessId 1 -CreationDateUtc (New-TestTime -600)
    )

    $set = Get-OwnedProcessSet -Records $records -Table $rows
    Assert-Ids $set.Owned @(1000, 1004) 'owned set is the root plus its child only'
    Assert-Equal 0 @($set.Ambiguous).Count 'no ambiguity for a clean owned set'
    Assert-True (@($set.Ambiguous | Where-Object { [int]$_.Id -eq 1001 }).Count -eq 0) 'the other session is not in the ambiguous report'

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }
    $result = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop `
        -Log { param([string]$Message) }

    Assert-Ids $result.Stopped @(1000, 1004) 'stopped exactly the owned tree'
    Assert-Ids $recorded @(1000, 1004) 'Stop-Process was invoked only for owned ids'
    Assert-Equal $false $result.Ambiguous 'no ambiguity on a clean stop'
    Assert-True (-not $recorded.Contains(1001)) 'the other session (1001) was untouched'
    Assert-True (-not $recorded.Contains(1002)) 'the other session child (1002) was untouched'
    Assert-True (-not $recorded.Contains(999)) 'an older emulator predating the run (999) was untouched'
}

# --- case 2 -----------------------------------------------------------------------------------
Invoke-Case 'pid reuse: same pid with a later creation time is skipped and reported' {
    $records = @(New-TestRecord -Id 4242 -StartTimeUtc (New-TestTime 0))
    $rows = @(New-TestRow -ProcessId 4242 -ParentProcessId 1 -CreationDateUtc (New-TestTime 120))

    $set = Get-OwnedProcessSet -Records $records -Table $rows
    Assert-Equal 0 @($set.Owned).Count 'a reused pid is not owned'
    Assert-Equal 'identity' @(Get-Codes $set.Ambiguous)[0] 'the reused pid is reported as an identity ambiguity'
    Assert-True ([string]$set.Ambiguous[0].Reason -match 'reused') 'the reason names pid reuse'

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }
    $result = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop `
        -Log { param([string]$Message) }

    Assert-Equal 0 $recorded.Count 'the reused pid was not stopped'
    Assert-Equal 0 @($result.Stopped).Count 'nothing was reported as stopped'
    Assert-Equal $true $result.Ambiguous 'the stop reports ambiguity'
    Assert-Ids $result.Skipped @(4242) 'the reused pid is in the skipped report'
}

# --- case 3 -----------------------------------------------------------------------------------
Invoke-Case 'RenderDoc wrapper: wrapper and its emulator child stop, an unrelated session does not' {
    $records = @(New-TestRecord -Id 100 -StartTimeUtc (New-TestTime 0) -Path $script:renderDocExe -Role 'wrapper')
    $rows = @(
        New-TestRow -ProcessId 100 -ParentProcessId 900 -CreationDateUtc (New-TestTime 0) -ExecutablePath $script:renderDocExe -Name 'renderdoccmd.exe'
        New-TestRow -ProcessId 101 -ParentProcessId 100 -CreationDateUtc (New-TestTime 3)
        New-TestRow -ProcessId 200 -ParentProcessId 900 -CreationDateUtc (New-TestTime 20)
        New-TestRow -ProcessId 201 -ParentProcessId 200 -CreationDateUtc (New-TestTime 25)
    )

    $set = Get-OwnedProcessSet -Records $records -Table $rows
    Assert-Ids $set.Owned @(100, 101) 'the wrapper and the emulator it launched are owned'
    Assert-Equal 0 @($set.Ambiguous).Count 'no ambiguity'

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }
    $result = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop `
        -Log { param([string]$Message) }

    Assert-Ids $result.Stopped @(100, 101) 'both the wrapper and its child stopped'
    Assert-Ids $recorded @(100, 101) 'no other pid was passed to Stop-Process'
    Assert-Equal 101 $recorded[0] 'the child is stopped before its wrapper'
    Assert-True (-not $recorded.Contains(200)) 'the unrelated shadps4 (200) was untouched'
    Assert-True (-not $recorded.Contains(201)) 'the unrelated shadps4 child (201) was untouched'
    Assert-Equal $false $result.Ambiguous 'no ambiguity'
}

# --- case 4 -----------------------------------------------------------------------------------
Invoke-Case 'partial startup failure: unreadable identity and already-exited process are reported, nothing killed' {
    $records = @(
        New-TestRecord -Id 300 -IdentityAvailable $false -Reason 'executable path unreadable: Access is denied'
        New-TestRecord -Id 77 -StartTimeUtc (New-TestTime 0)
    )
    $rows = @(New-TestRow -ProcessId 88 -ParentProcessId 1 -CreationDateUtc (New-TestTime 5))

    $set = Get-OwnedProcessSet -Records $records -Table $rows
    Assert-Equal 0 @($set.Owned).Count 'no process is owned'
    Assert-True ((Get-Codes $set.Ambiguous) -contains 'identity') 'the unreadable record is an identity ambiguity'
    Assert-True ((Get-Codes $set.Ambiguous) -contains 'absent') 'the exited record is reported as absent'

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }
    $result = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop `
        -Log { param([string]$Message) }

    Assert-Equal 0 $recorded.Count 'nothing was stopped'
    Assert-Equal 0 @($result.Stopped).Count 'nothing was reported as stopped'
    Assert-Equal $true $result.Ambiguous 'identity unavailable makes the cleanup ambiguous'
    Assert-Ids $result.Skipped @(300, 77) 'both records are reported in skipped'
    Assert-Equal 0 @($result.Skipped | Where-Object { [int]$_.Id -eq 88 }).Count 'the unrelated live process is not reported'
}

# --- case 5 -----------------------------------------------------------------------------------
Invoke-Case 'path mismatch at the same pid and creation time is skipped' {
    $records = @(New-TestRecord -Id 500 -StartTimeUtc (New-TestTime 0) -Path 'C:\lab-a\shadps4.exe')
    $rows = @(New-TestRow -ProcessId 500 -ParentProcessId 1 -CreationDateUtc (New-TestTime 0) -ExecutablePath 'C:\lab-b\shadps4.exe')

    $set = Get-OwnedProcessSet -Records $records -Table $rows
    Assert-Equal 0 @($set.Owned).Count 'a different image on the same pid is not owned'
    Assert-Equal 'identity' @(Get-Codes $set.Ambiguous)[0] 'a path mismatch is an identity ambiguity'
    Assert-True ([string]$set.Ambiguous[0].Reason -match 'path') 'the reason names the executable path'

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }
    $result = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop `
        -Log { param([string]$Message) }

    Assert-Equal 0 $recorded.Count 'the process with a different path was not stopped'
    Assert-Equal $true $result.Ambiguous 'the stop reports ambiguity'
}

# --- case 6 -----------------------------------------------------------------------------------
Invoke-Case 'descendant created before its root is not adopted' {
    $records = @(New-TestRecord -Id 600 -StartTimeUtc (New-TestTime 10))
    $rows = @(
        New-TestRow -ProcessId 600 -ParentProcessId 1 -CreationDateUtc (New-TestTime 10)
        New-TestRow -ProcessId 601 -ParentProcessId 600 -CreationDateUtc (New-TestTime -30)
    )

    $set = Get-OwnedProcessSet -Records $records -Table $rows
    Assert-Ids $set.Owned @(600) 'only the root is owned'
    Assert-Equal 'rejected-child' @(Get-Codes $set.Ambiguous)[0] 'the impossible child is reported'
    Assert-True ([string]$set.Ambiguous[0].Reason -match 'before its parent') 'the reason names the creation order'

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }
    $result = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop `
        -Log { param([string]$Message) }

    Assert-Ids $result.Stopped @(600) 'only the root stopped'
    Assert-True (-not $recorded.Contains(601)) 'the earlier-created child was untouched'
    Assert-Equal $false $result.Ambiguous 'a definitive rejection is not an ambiguity'
}

# --- case 7 -----------------------------------------------------------------------------------
Invoke-Case 'identity comparison tolerates path casing and the extended-length prefix' {
    $record = New-TestRecord -Id 700 -StartTimeUtc (New-TestTime 0) -Path 'C:\Lab\ShadPS4.exe'

    $same = Test-ProcessIdentity -Owned $record -Candidate (New-TestRow -ProcessId 700 -CreationDateUtc (New-TestTime 1.5) `
            -ExecutablePath '\\?\c:\lab\shadps4.EXE')
    Assert-Equal $true $same.Confirmed 'same pid, time and path in a different form confirms'
    Assert-Equal 'pid, creation time and executable path match' $same.Reason 'the confirmation reason is explicit'

    $reused = Test-ProcessIdentity -Owned $record -Candidate (New-TestRow -ProcessId 700 -CreationDateUtc (New-TestTime 600))
    Assert-Equal $false $reused.Confirmed 'a later creation time is rejected'
    Assert-True ($reused.Reason -match 'reused') 'pid reuse names itself'

    $shifted = Test-ProcessIdentity -Owned $record -Candidate (New-TestRow -ProcessId 700 -CreationDateUtc (New-TestTime 1.5) `
            -ExecutablePath 'c:\lab\shadPS4.exe')
    Assert-Equal $true $shifted.Confirmed 'a difference inside the 2 s tolerance still confirms'

    $mismatch = Test-ProcessIdentity -Owned (New-TestRecord -Id 701 -IdentityAvailable $false -Reason 'access denied') `
        -Candidate (New-TestRow -ProcessId 701)
    Assert-Equal $false $mismatch.Confirmed 'an unavailable identity is never confirmed'
    Assert-True ($mismatch.Reason -match 'access denied') 'the launch-time reason is carried through'
}

# --- case 8 -----------------------------------------------------------------------------------
Invoke-Case 'repeated cleanup is safe: a stopped process is reported absent, not killed twice' {
    $records = @(New-TestRecord -Id 800 -StartTimeUtc (New-TestTime 0))
    $rows = @(New-TestRow -ProcessId 800 -ParentProcessId 1 -CreationDateUtc (New-TestTime 0))
    $afterExit = @(New-TestRow -ProcessId 900 -ParentProcessId 1 -CreationDateUtc (New-TestTime 60) -Name 'pwsh.exe' -ExecutablePath 'C:\Program Files\PowerShell\7\pwsh.exe')

    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }

    $first = Stop-OwnedProcesses -Records $records -TableProvider { $rows } -StopAction $simulatedStop -Log { param([string]$Message) }
    Assert-Ids $first.Stopped @(800) 'the first cleanup stops the owned process'

    $second = Stop-OwnedProcesses -Records $records -TableProvider { $afterExit } -StopAction $simulatedStop -Log { param([string]$Message) }
    Assert-Equal 0 @($second.Stopped).Count 'the second cleanup stops nothing'
    Assert-Equal $false $second.Ambiguous 'an already exited process is benign'
    Assert-Ids $second.Skipped @(800) 'the already exited process is reported'
    Assert-Equal 'absent' @(Get-Codes $second.Skipped)[0] 'the report code is absent'
    Assert-Equal 1 $recorded.Count 'Stop-Process ran once in total'
}

# --- case 9 -----------------------------------------------------------------------------------
Invoke-Case 'unreadable snapshot: nothing is stopped and the failure is reported' {
    $records = @(New-TestRecord -Id 9000 -StartTimeUtc (New-TestTime 0))
    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }

    $result = Stop-OwnedProcesses -Records $records -TableProvider { throw 'CIM service unavailable' } `
        -StopAction $simulatedStop -Log { param([string]$Message) }

    Assert-Equal 0 $recorded.Count 'nothing was stopped without a snapshot'
    Assert-Equal 0 @($result.Stopped).Count 'nothing was reported as stopped'
    Assert-Equal $true $result.Ambiguous 'the failure is ambiguous'
    Assert-Equal 'snapshot' @(Get-Codes $result.Skipped)[0] 'the report code is snapshot'
    Assert-True ([string]$result.Skipped[0].Reason -match 'CIM service unavailable') 'the snapshot error is carried through'
}

# --- case 10 ----------------------------------------------------------------------------------
Invoke-Case 'live identity capture: the current process round-trips through the real CIM table' {
    $current = Get-Process -Id $PID
    $record = New-OwnedProcessRecord -Process $current -Role 'harness' -ExpectedPath $current.Path

    Assert-Equal $true $record.IdentityAvailable 'the current process identity is readable'
    Assert-Equal $PID $record.Id 'the record keeps the pid'
    Assert-Equal $current.Path $record.Path 'the record keeps the executable path'
    Assert-Equal 'harness' $record.Role 'the record keeps the role'
    $drift = [math]::Abs(($record.StartTimeUtc - $current.StartTime.ToUniversalTime()).TotalSeconds)
    Assert-True ($drift -le 2.0) 'the captured start time is the process start time in UTC'

    $table = @(Get-ProcessTable)
    Assert-True ($table.Count -gt 1) 'the snapshot has rows'
    $row = $table | Where-Object { [int]$_.ProcessId -eq $PID } | Select-Object -First 1
    Assert-True ($null -ne $row) 'the current process is in the snapshot'
    Assert-True ($null -ne $row.CreationDateUtc) 'the snapshot row carries a UTC creation time'
    $identity = Test-ProcessIdentity -Owned $record -Candidate $row
    Assert-Equal $true $identity.Confirmed ('live identity round-trip: ' + $identity.Reason)

    $other = @($table | Where-Object { [int]$_.ProcessId -ne $PID }) | Select-Object -First 1
    if ($null -ne $other) {
        $wrong = Test-ProcessIdentity -Owned $record -Candidate $other
        Assert-Equal $false $wrong.Confirmed 'a different pid never confirms'
    }
}

# --- case 11 ----------------------------------------------------------------------------------
Invoke-Case 'no owned processes: cleanup is a no-op and never reads a snapshot' {
    $recorded = [System.Collections.Generic.List[int]]::new()
    $simulatedStop = { param([int]$ProcessId) $recorded.Add([int]$ProcessId) }

    $result = Stop-OwnedProcesses -Records @() -TableProvider { throw 'the snapshot must not be read' } `
        -StopAction $simulatedStop -Log { param([string]$Message) }

    Assert-Equal 0 $recorded.Count 'nothing was stopped'
    Assert-Equal 0 @($result.Stopped).Count 'nothing was reported as stopped'
    Assert-Equal 0 @($result.Skipped).Count 'nothing was reported as skipped'
    Assert-Equal $false $result.Ambiguous 'an empty record set is not ambiguous'
}

# --- case 12 ----------------------------------------------------------------------------------
Invoke-Case 'no global name sweep exists in the module or in these tests' {
    $moduleText = Get-Content -LiteralPath $modulePath -Raw
    Assert-True (-not ($moduleText -match 'Get-Process\s+-Name\s+shadps4')) 'the module never selects by process name'
    Assert-True (-not ($moduleText -match 'Stop-Process\s+-Name')) 'the module never stops by name'

    $testText = Get-Content -LiteralPath $PSCommandPath -Raw
    Assert-True (-not ($testText -match 'Get-Process\s+-Name\s+shadps4')) 'the tests never select by process name'
}

# --- summary ----------------------------------------------------------------------------------
Write-Host ''
if ($script:failures -gt 0) {
    Write-Host ("FAILED: {0} of {1} cases" -f $script:failures, $script:caseCount)
    exit 1
}
Write-Host ("PASSED: all {0} cases" -f $script:caseCount)
exit 0
