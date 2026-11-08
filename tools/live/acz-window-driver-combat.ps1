# Combat variant of tools/live/acz-window-driver.ps1 (D-284: promoted from the
# session scratchpad so live drivers stop using the key-less repo driver).
# Local, ACZero-only fallback when the Computer Use bridge cannot enumerate
# native windows.  This tool deliberately does not launch the game, edit its
# files, or change Steam/Windows settings.
#
# KEY ALLOWLIST (the only keys this script can send; everything else is refused
# by ValidateSet and by the timed-sequence check):
#   menus/flight : confirm Enter, back Esc, view Tab, accelerate Space,
#                  up W, down S, left A, right D, cockpit-view V
#   camera look  : look-up Num8, look-down Num2, look-left Num4, look-right Num6
#   combat       : fire-gun J (machine gun), fire-missile L (missile/special weapon)
#                  (bindings read from the in-game Controls screen)
# HOLD LIMIT: -HoldMilliseconds 50..4000 (holds <= 4 s; the repo driver caps at 1 s).
# Timed sequences: taps only, 1..48 actions, delays <= 30 s, file inside the project.
# Run tools/live/preflight.ps1 first (docs/live-driver-brief-template.md).
#
# Read-only use:
#   pwsh -NoProfile -File tools/live/acz-window-driver-combat.ps1 -Action status
#   pwsh -NoProfile -File tools/live/acz-window-driver-combat.ps1 -Action screenshot
#
# Live input (requires a separate, current user approval):
#   pwsh -NoProfile -File tools/live/acz-window-driver-combat.ps1 -Action tap -Key fire-missile -AcknowledgeLiveInput
#   pwsh -NoProfile -File tools/live/acz-window-driver-combat.ps1 -Action hold -Key fire-gun -HoldMilliseconds 2000 -AcknowledgeLiveInput
#
# The input path accepts only the declared game controls, and only after the
# foreground window has been checked by handle and exact title.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('status', 'screenshot', 'tap', 'hold', 'sequence')]
    [string]$Action,

    [ValidateSet('fire-gun','fire-missile','confirm', 'back', 'view', 'accelerate', 'up', 'down', 'left', 'right', 'look-up', 'look-down', 'look-left', 'look-right', 'cockpit-view')]
    [string]$Key,

    [ValidateRange(50, 4000)]
    [int]$HoldMilliseconds = 250,

    # hold only: capture a screenshot while the key is still down (camera-look views snap back on release).
    [switch]$CaptureDuringHold,

    [switch]$AcknowledgeLiveInput,

    [string]$SequencePath,

    [string]$ScreenshotDirectory = (Join-Path $PSScriptRoot '..\..\research\experiments\L1b-screenshots')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedTitle = 'ACE COMBAT ZERO THE BELKAN WAR'
$keyCodes = @{
    confirm = 0x0D # Enter
    back = 0x1B # Escape
    view = 0x09 # Tab (verified on the live Controls screen)
    accelerate = 0x20 # Space
    up = 0x57 # W
    down = 0x53 # S
    left = 0x41 # A
    right = 0x44 # D
    'look-up' = 0x68 # Numpad 8: move camera up
    'look-down' = 0x62 # Numpad 2: move camera down
    'look-left' = 0x64 # Numpad 4: move camera left
    'look-right' = 0x66 # Numpad 6: move camera right
    'cockpit-view' = 0x56 # V: in-mission Change view
    'fire-gun' = 0x4A # J: Fire machine gun (Controls screen)
    'fire-missile' = 0x4C # L: Fire missile/special weapon
}

if (-not ('AczWindowDriver.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace AczWindowDriver {
    public static class Native {
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        public struct INPUT { public uint type; public InputUnion U; }

        [StructLayout(LayoutKind.Explicit)]
        public struct InputUnion {
            [FieldOffset(0)] public MOUSEINPUT mi;
            [FieldOffset(0)] public KEYBDINPUT ki;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct MOUSEINPUT {
            public int dx;
            public int dy;
            public uint mouseData;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct KEYBDINPUT {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool GetWindowRect(IntPtr hWnd, out RECT rectangle);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern uint SendInput(uint count, INPUT[] inputs, int inputSize);

        public const uint INPUT_KEYBOARD = 1;
        public const uint KEYEVENTF_KEYUP = 0x0002;

        [DllImport("user32.dll")]
        public static extern uint MapVirtualKey(uint code, uint mapType);

        public static uint SendKeyboard(ushort virtualKey, bool keyUp) {
            INPUT input = new INPUT();
            input.type = INPUT_KEYBOARD;
            input.U.ki.wVk = virtualKey;
            // Include the hardware scan code: games that read lParam scan codes ignore wVk-only events.
            input.U.ki.wScan = (ushort)MapVirtualKey(virtualKey, 0); // MAPVK_VK_TO_VSC
            input.U.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
            return SendInput(1, new INPUT[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }
    }
}
'@
}

function Get-AczWindow {
    $matches = @(Get-Process -Name acz -ErrorAction SilentlyContinue | Where-Object {
        $_.MainWindowHandle -ne [IntPtr]::Zero -and $_.MainWindowTitle -eq $expectedTitle
    })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one running ACZero window titled '$expectedTitle'; found $($matches.Count)."
    }
    return $matches[0]
}

function Get-WindowTitle([IntPtr]$Handle) {
    $buffer = [System.Text.StringBuilder]::new(512)
    [void][AczWindowDriver.Native]::GetWindowText($Handle, $buffer, $buffer.Capacity)
    return $buffer.ToString()
}

function Get-AczStatus {
    $process = Get-AczWindow
    $rect = [AczWindowDriver.Native+RECT]::new()
    if (-not [AczWindowDriver.Native]::GetWindowRect($process.MainWindowHandle, [ref]$rect)) {
        throw 'Could not read the ACZero window rectangle.'
    }
    [pscustomobject]@{
        ProcessId = $process.Id
        SessionId = $process.SessionId
        WindowHandle = ('0x{0:X}' -f $process.MainWindowHandle.ToInt64())
        Title = $process.MainWindowTitle
        Foreground = ([AczWindowDriver.Native]::GetForegroundWindow() -eq $process.MainWindowHandle)
        Width = $rect.Right - $rect.Left
        Height = $rect.Bottom - $rect.Top
        NativeInputSize = [Runtime.InteropServices.Marshal]::SizeOf([type][AczWindowDriver.Native+INPUT])
        InputKeys = ($keyCodes.Keys | Sort-Object) -join ', '
    }
}

function Assert-LiveInput([System.Diagnostics.Process]$Process) {
    if (-not $AcknowledgeLiveInput) {
        throw 'Live input is disabled. Re-run with -AcknowledgeLiveInput only after a current user approval.'
    }
    [void][AczWindowDriver.Native]::SetForegroundWindow($Process.MainWindowHandle)
    Start-Sleep -Milliseconds 100
    if ([AczWindowDriver.Native]::GetForegroundWindow() -ne $Process.MainWindowHandle) {
        throw 'Refusing input: ACZero did not become the foreground window.'
    }
    if ((Get-WindowTitle $Process.MainWindowHandle) -ne $expectedTitle) {
        throw 'Refusing input: the checked window title changed.'
    }
}

function Send-AczKey([int]$VirtualKey, [int]$DurationMilliseconds) {
    if ([AczWindowDriver.Native]::SendKeyboard([uint16]$VirtualKey, $false) -ne 1) {
        $error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Keyboard-down injection failed (Win32 error $error)."
    }
    Start-Sleep -Milliseconds $DurationMilliseconds
    if ([AczWindowDriver.Native]::SendKeyboard([uint16]$VirtualKey, $true) -ne 1) {
        $error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Keyboard-up injection failed (Win32 error $error)."
    }
}

function Invoke-AczTimedSequence([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw '-SequencePath is required for sequence.' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Sequence file does not exist: $Path" }
    $projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path.TrimEnd('\')
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (-not $resolved.StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Sequence file must be inside the project root.'
    }
    try { $sequence = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json } catch {
        throw "Sequence file is not valid JSON: $($_.Exception.Message)"
    }
    if ($sequence.schemaVersion -ne 1) { throw 'Sequence schemaVersion must be 1.' }
    $actions = @($sequence.actions)
    if ($actions.Count -lt 1 -or $actions.Count -gt 48) { throw 'Sequence must contain between 1 and 48 actions.' }

    # Validate the whole sequence before any input is sent.
    foreach ($item in $actions) {
        if ($item.type -ne 'tap') { throw 'Timed sequences permit tap actions only.' }
        if ([string]::IsNullOrWhiteSpace([string]$item.key) -or -not $keyCodes.ContainsKey([string]$item.key)) {
            throw "Timed sequence key '$($item.key)' is not permitted."
        }
        $delay = [int]$item.delayBeforeMilliseconds
        if ($delay -lt 0 -or $delay -gt 30000) { throw 'delayBeforeMilliseconds must be between 0 and 30000.' }
    }

    $process = Get-AczWindow
    Assert-LiveInput $process
    $started = [Diagnostics.Stopwatch]::StartNew()
    $record = @()
    for ($index = 0; $index -lt $actions.Count; $index++) {
        $item = $actions[$index]
        $delay = [int]$item.delayBeforeMilliseconds
        Start-Sleep -Milliseconds $delay
        # SendInput goes to whatever window has focus, so re-check before every key and abort rather than
        # steal focus back. Re-resolving tolerates the launcher-to-game window swap under the same title.
        $reason = $null
        try { $current = Get-AczWindow } catch { $reason = $_.Exception.Message }
        if (-not $reason) {
            if ([AczWindowDriver.Native]::GetForegroundWindow() -ne $current.MainWindowHandle) { $reason = 'ACZero is no longer the foreground window' }
            elseif ((Get-WindowTitle $current.MainWindowHandle) -ne $expectedTitle) { $reason = 'the ACZero window title changed' }
        }
        if ($reason) {
            throw "Timed sequence aborted before action $($index + 1) of $($actions.Count) ($($item.key)): $reason. Keys sent: $($record.Count)."
        }
        Send-AczKey -VirtualKey $keyCodes[[string]$item.key] -DurationMilliseconds 50
        $record += [pscustomobject]@{ key = [string]$item.key; delayBeforeMilliseconds = $delay; elapsedMilliseconds = $started.ElapsedMilliseconds }
    }
    [pscustomobject]@{ sequence = $resolved; actions = $record; elapsedMilliseconds = $started.ElapsedMilliseconds }
}

function Save-AczScreenshot([System.Diagnostics.Process]$Process, [string]$Suffix = '') {
    $status = Get-AczStatus
    New-Item -ItemType Directory -Force -Path $ScreenshotDirectory | Out-Null
    $path = Join-Path $ScreenshotDirectory ("acz-{0}{1}.png" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Suffix)
    Add-Type -AssemblyName System.Drawing
    $bitmap = [System.Drawing.Bitmap]::new($status.Width, $status.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $rect = [AczWindowDriver.Native+RECT]::new()
        [void][AczWindowDriver.Native]::GetWindowRect($Process.MainWindowHandle, [ref]$rect)
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    return $path
}

$process = Get-AczWindow
switch ($Action) {
    'status' { Get-AczStatus | Format-List }
    'screenshot' { Save-AczScreenshot $process }
    { $_ -in @('tap', 'hold') } {
        if ([string]::IsNullOrWhiteSpace($Key)) { throw '-Key is required for live input.' }
        Assert-LiveInput $process
        $duration = if ($Action -eq 'tap') { 50 } else { $HoldMilliseconds }
        if ($Action -eq 'hold' -and $CaptureDuringHold) {
            $vk = [uint16]$keyCodes[$Key]
            if ([AczWindowDriver.Native]::SendKeyboard($vk, $false) -ne 1) { throw 'Keyboard-down injection failed.' }
            try {
                Start-Sleep -Milliseconds $duration
                $held = Save-AczScreenshot $process '-held'
            } finally {
                # Always release the key, even if the capture fails.
                [void][AczWindowDriver.Native]::SendKeyboard($vk, $true)
            }
            "Sent '$Key' to the verified ACZero window for $duration ms; captured while held: $held"
        } else {
            Send-AczKey -VirtualKey $keyCodes[$Key] -DurationMilliseconds $duration
            "Sent '$Key' to the verified ACZero window for $duration ms."
        }
    }
    'sequence' { Invoke-AczTimedSequence $SequencePath | ConvertTo-Json -Depth 5 }
}
