# Passive ACZero UI evidence recorder. It never launches ACZero, sends input,
# changes settings, or modifies the game directory. It saves a time-stamped
# status/screenshot timeline only while the expected ACZero window exists.
[CmdletBinding()]
param(
    [ValidateRange(30, 1800)]
    [int]$DurationSeconds = 720,

    [ValidateRange(500, 10000)]
    [int]$IntervalMilliseconds = 1000,

    [switch]$CaptureAllowedKeys,

    [ValidateRange(10, 250)]
    [int]$KeyPollMilliseconds = 25,

    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('..\\..\\research\\experiments\\ui-route-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$helper = Join-Path $PSScriptRoot 'acz-window-driver.ps1'
if (-not (Test-Path -LiteralPath $helper)) { throw "Missing ACZero window helper: $helper" }

if ($CaptureAllowedKeys -and -not ('AczUiRecorder.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace AczUiRecorder {
    public static class Native {
        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll")]
        public static extern short GetAsyncKeyState(int virtualKey);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern int GetWindowText(IntPtr window, StringBuilder text, int maxCount);
    }
}
'@
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$timeline = Join-Path $OutputDirectory 'timeline.txt'
"ACZero passive UI record started $(Get-Date -Format o)" | Set-Content -LiteralPath $timeline -Encoding utf8
"DurationSeconds=$DurationSeconds; IntervalMilliseconds=$IntervalMilliseconds; CaptureAllowedKeys=$CaptureAllowedKeys" | Add-Content -LiteralPath $timeline -Encoding utf8

$allowedKeys = [ordered]@{
    confirm = 0x0D
    back = 0x1B
    view = 0x09
    accelerate = 0x20
    up = 0x57
    down = 0x53
    left = 0x41
    right = 0x44
    'look-up' = 0x68
    'look-down' = 0x62
    'look-left' = 0x64
    'look-right' = 0x66
}
$keyStates = @{}
$keyTimeline = Join-Path $OutputDirectory 'allowed-key-events.csv'
if ($CaptureAllowedKeys) {
    'timestamp,key,event' | Set-Content -LiteralPath $keyTimeline -Encoding utf8
    foreach ($key in $allowedKeys.Keys) { $keyStates[$key] = $false }
}

function Test-ExpectedAczForeground {
    $window = [AczUiRecorder.Native]::GetForegroundWindow()
    if ($window -eq [IntPtr]::Zero) { return $false }
    [uint32]$processId = 0
    [void][AczUiRecorder.Native]::GetWindowThreadProcessId($window, [ref]$processId)
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($null -eq $process -or $process.ProcessName -ne 'acz') { return $false }
    $title = [System.Text.StringBuilder]::new(512)
    [void][AczUiRecorder.Native]::GetWindowText($window, $title, $title.Capacity)
    return $title.ToString() -eq 'ACE COMBAT ZERO THE BELKAN WAR'
}

function Record-AllowedKeyTransitions {
    if (-not (Test-ExpectedAczForeground)) {
        foreach ($key in $allowedKeys.Keys) { $keyStates[$key] = $false }
        return
    }
    foreach ($key in $allowedKeys.Keys) {
        $down = (([AczUiRecorder.Native]::GetAsyncKeyState($allowedKeys[$key]) -band 0x8000) -ne 0)
        if ($down -ne $keyStates[$key]) {
            $event = if ($down) { 'down' } else { 'up' }
            "$(Get-Date -Format o),$key,$event" | Add-Content -LiteralPath $keyTimeline -Encoding utf8
            $keyStates[$key] = $down
        }
    }
}

$deadline = (Get-Date).AddSeconds($DurationSeconds)
$previousState = ''
$nextScreenshot = Get-Date
while ((Get-Date) -lt $deadline) {
    if ($CaptureAllowedKeys) { Record-AllowedKeyTransitions }
    if ((Get-Date) -ge $nextScreenshot) {
        $timestamp = Get-Date -Format o
        try {
            $status = (& $helper -Action status | Out-String).Trim()
            $screenshot = (& $helper -Action screenshot -ScreenshotDirectory $OutputDirectory | Select-Object -Last 1).Trim()
            "[$timestamp] WINDOW" | Add-Content -LiteralPath $timeline -Encoding utf8
            $status | Add-Content -LiteralPath $timeline -Encoding utf8
            "Screenshot=$screenshot" | Add-Content -LiteralPath $timeline -Encoding utf8
            $previousState = 'window-present'
        } catch {
            if ($previousState -ne 'window-absent') {
                "[$timestamp] WINDOW_ABSENT_OR_UNVERIFIED: $($_.Exception.Message)" | Add-Content -LiteralPath $timeline -Encoding utf8
                $previousState = 'window-absent'
            }
        }
        $nextScreenshot = (Get-Date).AddMilliseconds($IntervalMilliseconds)
    }
    Start-Sleep -Milliseconds $(if ($CaptureAllowedKeys) { $KeyPollMilliseconds } else { [Math]::Min($IntervalMilliseconds, 250) })
}

"ACZero passive UI record ended $(Get-Date -Format o)" | Add-Content -LiteralPath $timeline -Encoding utf8
Write-Output $OutputDirectory
