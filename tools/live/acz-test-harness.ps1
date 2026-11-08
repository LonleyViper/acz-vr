# Guarded ACZero mission-test harness.  Job-board workers may create and
# validate cards; only an interactive controller with current user approval may
# execute one.  The harness never launches ACZero or changes game files.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('validate', 'execute')]
    [string]$Action,

    [Parameter(Mandatory)]
    [string]$TestCard,

    [switch]$AcknowledgeLiveRun,

    [string]$EvidenceRoot = (Join-Path $PSScriptRoot '..\..\research\experiments\harness-runs')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path.TrimEnd('\')
$driver = Join-Path $PSScriptRoot 'acz-window-driver.ps1'
$allowedKeys = @('confirm', 'back', 'view', 'accelerate', 'up', 'down', 'left', 'right', 'look-up', 'look-down', 'look-left', 'look-right', 'cockpit-view')

function Resolve-ProjectFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label does not exist: $Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $prefix = $projectRoot + '\'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must be inside the project root."
    }
    return $resolved
}

function Assert-NonEmptyString($Value, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace([string]$Value)) { throw "$Label is required." }
}

function Read-AndValidateCard([string]$Path) {
    $resolved = Resolve-ProjectFile $Path 'Test card'
    try { $card = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json } catch {
        throw "Test card is not valid JSON: $($_.Exception.Message)"
    }
    if ($null -eq $card) { throw 'Test card is empty.' }
    if ($card.schemaVersion -ne 1) { throw 'Test card schemaVersion must be 1.' }
    Assert-NonEmptyString $card.id 'Test card id'
    if ([string]$card.id -notmatch '^[a-z0-9][a-z0-9._-]{0,79}$') {
        throw 'Test card id must use lowercase letters, digits, dot, underscore, or hyphen.'
    }
    Assert-NonEmptyString $card.purpose 'Test card purpose'
    Assert-NonEmptyString $card.expectedStartCheckpoint 'expectedStartCheckpoint'
    Assert-NonEmptyString $card.stopCondition 'stopCondition'
    $executionMode = if ($null -eq $card.PSObject.Properties['executionMode'] -or [string]::IsNullOrWhiteSpace([string]$card.executionMode)) { 'checkpointed' } else { [string]$card.executionMode }
    if ($executionMode -notin @('checkpointed', 'timed-macro')) { throw 'executionMode must be checkpointed or timed-macro.' }
    if ($card.requiresCurrentUserApproval -ne $true) {
        throw 'Test card must set requiresCurrentUserApproval to true.'
    }
    $maximumActions = if ($executionMode -eq 'timed-macro') { 48 } else { 12 }
    if ($null -eq $card.maxActions -or [int]$card.maxActions -lt 1 -or [int]$card.maxActions -gt $maximumActions) {
        throw "maxActions must be between 1 and $maximumActions for $executionMode cards."
    }
    if ($executionMode -eq 'timed-macro') { Assert-NonEmptyString $card.expectedEndCheckpoint 'expectedEndCheckpoint' }

    $actions = @($card.actions)
    if ($actions.Count -lt 1 -or $actions.Count -gt [int]$card.maxActions) {
        throw 'actions must contain between 1 and maxActions entries.'
    }

    $normalized = @()
    foreach ($item in $actions) {
        Assert-NonEmptyString $item.type 'Action type'
        if ($executionMode -eq 'timed-macro') {
            if ($item.type -ne 'tap') { throw 'timed-macro cards permit tap actions only.' }
            Assert-NonEmptyString $item.key 'Action key'
            if ($item.key -notin $allowedKeys) { throw "Action key '$($item.key)' is not permitted." }
            $delay = [int]$item.delayBeforeMilliseconds
            if ($delay -lt 0 -or $delay -gt 30000) { throw 'timed-macro delayBeforeMilliseconds must be between 0 and 30000.' }
            $normalized += [pscustomobject]@{ type = 'tap'; key = [string]$item.key; delayBeforeMilliseconds = $delay }
            continue
        }
        if ($item.type -notin @('tap', 'hold', 'wait')) {
            throw "Unsupported action type '$($item.type)'."
        }
        Assert-NonEmptyString $item.expectedAfter 'Action expectedAfter'
        $entry = [ordered]@{ type = [string]$item.type; expectedAfter = [string]$item.expectedAfter }
        if ($item.type -eq 'wait') {
            $duration = [int]$item.durationMilliseconds
            if ($duration -lt 100 -or $duration -gt 10000) {
                throw 'wait durationMilliseconds must be between 100 and 10000.'
            }
            $entry.durationMilliseconds = $duration
        } else {
            Assert-NonEmptyString $item.key 'Action key'
            if ($item.key -notin $allowedKeys) {
                throw "Action key '$($item.key)' is not permitted."
            }
            $entry.key = [string]$item.key
            if ($item.type -eq 'hold') {
                $duration = [int]$item.durationMilliseconds
                if ($duration -lt 50 -or $duration -gt 1000) {
                    throw 'hold durationMilliseconds must be between 50 and 1000.'
                }
                $entry.durationMilliseconds = $duration
                $entry.captureDuringHold = ($item.PSObject.Properties.Name -contains 'captureDuringHold' -and $item.captureDuringHold -eq $true)
            }
        }
        $normalized += [pscustomobject]$entry
    }
    return [pscustomobject]@{ Source = $resolved; Card = $card; Actions = $normalized; ExecutionMode = $executionMode }
}

function Invoke-Driver([string[]]$Arguments, [string]$Label) {
    # Run the driver as its own process: in-process array splatting binds '-Action' as a value, not a parameter name.
    $output = & pwsh -NoProfile -File $driver @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($output | Out-String)" }
    return $output
}

function Save-Checkpoint([string]$Name, [string]$Directory) {
    $status = Invoke-Driver @('-Action', 'status') "Status checkpoint '$Name'"
    $status | Set-Content -LiteralPath (Join-Path $Directory "$Name-status.txt") -Encoding utf8
    $screenshot = Invoke-Driver @('-Action', 'screenshot', '-ScreenshotDirectory', $Directory) "Screenshot checkpoint '$Name'"
    $screenshotPath = ($screenshot | Select-Object -Last 1).ToString().Trim()
    [pscustomobject]@{ name = $Name; screenshot = $screenshotPath }
}

$validated = Read-AndValidateCard $TestCard
if ($Action -eq 'validate') {
    [pscustomobject]@{
        Valid = $true
        Id = $validated.Card.id
        ExecutionMode = $validated.ExecutionMode
        ExpectedStartCheckpoint = $validated.Card.expectedStartCheckpoint
        Actions = $validated.Actions.Count
        Source = $validated.Source
    } | Format-List
    exit 0
}

if (-not $AcknowledgeLiveRun) {
    throw 'Live execution is disabled. Re-run with -AcknowledgeLiveRun only after a current user approval for this specific card.'
}
if (-not (Test-Path -LiteralPath $driver -PathType Leaf)) { throw "Missing window driver: $driver" }

$session = Join-Path $EvidenceRoot ("{0}-{1}" -f $validated.Card.id, (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $session | Out-Null
Copy-Item -LiteralPath $validated.Source -Destination (Join-Path $session 'test-card.json')
$run = [ordered]@{
    cardId = $validated.Card.id
    purpose = $validated.Card.purpose
    expectedStartCheckpoint = $validated.Card.expectedStartCheckpoint
    expectedEndCheckpoint = if ($null -eq $validated.Card.PSObject.Properties['expectedEndCheckpoint']) { $null } else { $validated.Card.expectedEndCheckpoint }
    executionMode = $validated.ExecutionMode
    stopCondition = $validated.Card.stopCondition
    startedAt = (Get-Date).ToString('o')
    steps = @()
}

$run.steps += Save-Checkpoint '00-before' $session
if ($validated.ExecutionMode -eq 'timed-macro') {
    $sequencePath = Join-Path $session 'timed-sequence.json'
    [pscustomobject]@{ schemaVersion = 1; actions = $validated.Actions } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $sequencePath -Encoding utf8
    Invoke-Driver @('-Action', 'sequence', '-SequencePath', $sequencePath, '-AcknowledgeLiveInput') 'Timed macro' | Set-Content -LiteralPath (Join-Path $session '01-timed-macro-output.json') -Encoding utf8
    $checkpoint = Save-Checkpoint '01-after-timed-macro' $session
    $checkpoint | Add-Member -NotePropertyName expectedAfter -NotePropertyValue $validated.Card.expectedEndCheckpoint
    $run.steps += $checkpoint
} else {
    for ($index = 0; $index -lt $validated.Actions.Count; $index++) {
        $step = $validated.Actions[$index]
        $name = '{0:D2}-{1}' -f ($index + 1), $step.type
        if ($step.type -eq 'wait') {
            Start-Sleep -Milliseconds $step.durationMilliseconds
        } elseif ($step.type -eq 'tap') {
            Invoke-Driver @('-Action', 'tap', '-Key', $step.key, '-AcknowledgeLiveInput') "Action $name" | Set-Content -LiteralPath (Join-Path $session "$name-output.txt") -Encoding utf8
        } else {
            $holdArgs = @('-Action', 'hold', '-Key', $step.key, '-HoldMilliseconds', [string]$step.durationMilliseconds, '-AcknowledgeLiveInput')
            if ($step.captureDuringHold) { $holdArgs += @('-CaptureDuringHold', '-ScreenshotDirectory', $session) }
            Invoke-Driver $holdArgs "Action $name" | Set-Content -LiteralPath (Join-Path $session "$name-output.txt") -Encoding utf8
        }
        $checkpoint = Save-Checkpoint ("{0}-after" -f $name) $session
        $checkpoint | Add-Member -NotePropertyName expectedAfter -NotePropertyValue $step.expectedAfter
        $run.steps += $checkpoint
    }
}
$run.endedAt = (Get-Date).ToString('o')
$run | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $session 'run.json') -Encoding utf8
# Periodic cleanse of old raw screenshots; never fails the run.
try { & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'prune-screenshots.ps1') | Out-Null } catch { }
$session
