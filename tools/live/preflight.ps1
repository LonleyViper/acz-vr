# Live-run pre-flight (D-283 B, D-284). Read-only: run it BEFORE spawning a live driver
# (docs/live-driver-brief-template.md step 1). It launches nothing, sends no input, changes nothing.
#   pwsh -NoProfile -File tools/live/preflight.ps1 [-Xr] [-ExpectedCfgKeys N]
#   pwsh -NoProfile -File tools/live/preflight.ps1 -SelfTest
# Checks: no acz/AceCombat8 or `steam -applaunch` leftovers; memory gate on the projected peak
# (-Xr SteamVR run: commit + 10 (SteamVR, unless vrserver already runs) + 8 (game) <= 90;
#  non-XR: commit + 8 <= 86); C: and D: free >= 2 GB; desktop 2560x1440 (else remote/not comparable);
# user idle > 120 s (else 'user present: do not drive'); no SteamVR mirror window in front; Steam not
# stuck at 'Starting game'; l1.ps1 status baseline OK; runtime.cfg key count.
# Prints PASS/FAIL/INFO lines and a verdict. Exit 0 = PASS, 1 = FAIL.
param([switch]$Xr,
      [int]$ExpectedCfgKeys = -1,           # -1: report the count only
      [ValidateRange(0, 3600)][int]$MinIdleSeconds = 120,
      [string]$ExpectedResolution = '2560x1440',
      [double]$MinFreeGB = 2,
      [switch]$SkipBaseline,                # skip the l1.ps1 hash check (reported as INFO, not PASS)
      [switch]$MemGuard,                    # opt-in pre-step: tools/mem-guard.ps1 -Report (docs/mem-guard.md)
      [switch]$MemGuardApply,               # opt-in: mem-guard -Apply (kills only class A orphans), then the checks below
      [switch]$SelfTest)
$ErrorActionPreference = 'Stop'

# ---- pure evaluation: facts -> results (tested by -SelfTest) ----
function Test-Preflight([hashtable]$F, [switch]$Xr, [int]$ExpectedCfgKeys, [int]$MinIdleSeconds, [string]$ExpectedResolution, [double]$MinFreeGB) {
    $r = [System.Collections.Generic.List[object]]::new()
    function AddR([string]$State, [string]$Name, [string]$Detail) { $r.Add([pscustomobject]@{ State = $State; Name = $Name; Detail = $Detail }) }
    if ($F.GameProcesses.Count) { AddR FAIL leftovers "game process still running: $($F.GameProcesses -join ', ') (stop it with tools/live/vr-test-stop.ps1 or ask the user)" }
    elseif ($F.AppLaunch.Count) { AddR FAIL leftovers "steam -applaunch process present (pid $($F.AppLaunch -join ', ')): never -applaunch; clear it first" }
    else { AddR PASS leftovers 'no acz/AceCombat8/-applaunch process' }
    $vr = if ($Xr -and -not $F.VrServerRunning) { 10 } else { 0 }
    $limit = if ($Xr) { 90 } else { 86 }
    $peak = $F.CommitPercent + $vr + 8
    $what = ('commit {0:N1}% + {1} (SteamVR{2}) + 8 (game) = {3:N1}% vs <= {4}%' -f $F.CommitPercent, $vr, $(if ($Xr -and $F.VrServerRunning) { ' already running' } elseif ($Xr) { '' } else { ' not used' }), $peak, $limit)
    if ($peak -le $limit) { AddR PASS memory $what } else { AddR FAIL memory "$what - close apps or wait" }
    foreach ($d in 'C', 'D') {
        $free = $F.FreeGB[$d]
        if ($null -eq $free) { AddR FAIL "disk-$d" "drive $d`: not found" }
        elseif ($free -ge $MinFreeGB) { AddR PASS "disk-$d" ('{0:N1} GB free' -f $free) }
        else { AddR FAIL "disk-$d" ('{0:N1} GB free < {1} GB' -f $free, $MinFreeGB) }
    }
    if ($F.Resolution -eq $ExpectedResolution) { AddR PASS resolution $F.Resolution }
    else { AddR FAIL resolution "desktop $($F.Resolution) != $ExpectedResolution`: remote/not comparable" }
    if ($F.IdleSeconds -gt $MinIdleSeconds) { AddR PASS user-idle "idle $([int]$F.IdleSeconds) s" }
    else { AddR FAIL user-idle "last input $([int]$F.IdleSeconds) s ago: user present: do not drive" }
    if ($F.ForegroundTitle -match 'VR View|SteamVR|Headset Window|Mirror') { AddR FAIL mirror "foreground window '$($F.ForegroundTitle)': SteamVR mirror in front" }
    else { AddR PASS mirror "foreground '$($F.ForegroundTitle)'" }
    $stuck = @($F.SteamTitles | Where-Object { $_ -match 'Starting game|Preparing to launch|Launching' })
    if ($stuck.Count) { AddR FAIL steam "Steam window '$($stuck[0])': stuck launch state" } else { AddR PASS steam 'no Steam launch dialog' }
    if ($null -eq $F.Baseline) { AddR INFO baseline 'skipped (-SkipBaseline)' }
    elseif ($F.Baseline -match 'Baseline OK') { AddR PASS baseline ($F.Baseline -split "\r?\n")[0] }
    else { AddR FAIL baseline "l1.ps1 status: $(($F.Baseline -split "\r?\n")[0])" }
    if ($F.CfgKeys -lt 0) { AddR INFO runtime-cfg 'runtime.cfg missing' }
    elseif ($ExpectedCfgKeys -ge 0 -and $F.CfgKeys -ne $ExpectedCfgKeys) { AddR FAIL runtime-cfg "$($F.CfgKeys) keys, expected $ExpectedCfgKeys" }
    else { AddR $(if ($ExpectedCfgKeys -ge 0) { 'PASS' } else { 'INFO' }) runtime-cfg "$($F.CfgKeys) keys" }
    return , $r
}

if ($SelfTest) {
    $good = @{ GameProcesses = @(); AppLaunch = @(); CommitPercent = 60; VrServerRunning = $false; FreeGB = @{ C = 10; D = 100 }
               Resolution = '2560x1440'; IdleSeconds = 600; ForegroundTitle = 'Windows Terminal'; SteamTitles = @('Steam'); Baseline = 'Baseline OK (7 files).'; CfgKeys = 12 }
    $p = @{ ExpectedCfgKeys = -1; MinIdleSeconds = 120; ExpectedResolution = '2560x1440'; MinFreeGB = 2 }
    $cases = @(
        @{ n = 'clean non-XR'; f = @{}; xr = $false; fail = @() },
        @{ n = 'clean XR'; f = @{ CommitPercent = 72 }; xr = $true; fail = @() },
        @{ n = 'XR peak 91'; f = @{ CommitPercent = 73 }; xr = $true; fail = @('memory') },
        @{ n = 'XR vrserver up'; f = @{ CommitPercent = 82; VrServerRunning = $true }; xr = $true; fail = @() },
        @{ n = 'non-XR 79+8'; f = @{ CommitPercent = 79 }; xr = $false; fail = @('memory') },
        @{ n = 'leftover game'; f = @{ GameProcesses = @('acz') }; xr = $false; fail = @('leftovers') },
        @{ n = 'applaunch'; f = @{ AppLaunch = @(42) }; xr = $false; fail = @('leftovers') },
        @{ n = 'low C'; f = @{ FreeGB = @{ C = 1.5; D = 100 } }; xr = $false; fail = @('disk-C') },
        @{ n = 'no D'; f = @{ FreeGB = @{ C = 10 } }; xr = $false; fail = @('disk-D') },
        @{ n = 'remote res'; f = @{ Resolution = '1920x1080' }; xr = $false; fail = @('resolution') },
        @{ n = 'user typing'; f = @{ IdleSeconds = 30 }; xr = $false; fail = @('user-idle') },
        @{ n = 'mirror'; f = @{ ForegroundTitle = 'VR View' }; xr = $false; fail = @('mirror') },
        @{ n = 'steam stuck'; f = @{ SteamTitles = @('Steam', 'Starting game...') }; xr = $false; fail = @('steam') },
        @{ n = 'baseline bad'; f = @{ Baseline = 'Baseline mismatch: acz.exe 00' }; xr = $false; fail = @('baseline') },
        @{ n = 'cfg mismatch'; f = @{ CfgKeys = 9 }; xr = $false; fail = @('runtime-cfg'); cfg = 12 }
    )
    $errors = @()
    foreach ($c in $cases) {
        $f = $good.Clone(); foreach ($k in $c.f.Keys) { $f[$k] = $c.f[$k] }
        $q = $p.Clone(); if ($c.cfg) { $q.ExpectedCfgKeys = $c.cfg }
        $res = Test-Preflight $f -Xr:$c.xr @q
        $got = @($res | Where-Object State -eq 'FAIL' | ForEach-Object Name)
        if (($got -join ',') -ne ($c.fail -join ',')) { $errors += "$($c.n): failed [$($got -join ',')] expected [$($c.fail -join ',')]" }
    }
    if (($res | Where-Object Name -eq 'user-idle').Detail -notmatch 'idle') { $errors += 'idle detail' }
    if ($errors) { $errors | ForEach-Object { "SELF-TEST FAIL $_" }; exit 1 }
    "SELF-TEST PASS ($($cases.Count) cases)"
    exit 0
}

# ---- optional memory pre-step (off by default; its exit code is informational, the memory gate below decides) ----
if ($MemGuard -or $MemGuardApply) {
    $mg = Join-Path $PSScriptRoot '..\mem-guard.ps1'
    '--- mem-guard' + $(if ($MemGuardApply) { ' -Apply' } else { ' -Report' })
    if ($MemGuardApply) { & pwsh -NoProfile -File $mg -Apply } else { & pwsh -NoProfile -File $mg -Report }
    '--- end mem-guard (exit ' + $LASTEXITCODE + ')'
}

# ---- fact gathering (read-only) ----
if (-not ('AczPreflight.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace AczPreflight {
  public static class Native {
    [StructLayout(LayoutKind.Sequential)] struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
    [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
    [DllImport("kernel32.dll")] static extern ulong GetTickCount64();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE {
      [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
      public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
      public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
      public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
      [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
      public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
      public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    public static double IdleSeconds() {
      LASTINPUTINFO i = new LASTINPUTINFO(); i.cbSize = (uint)Marshal.SizeOf(i);
      if (!GetLastInputInfo(ref i)) return -1;
      return ((GetTickCount64() & 0xFFFFFFFFUL) - i.dwTime & 0xFFFFFFFFUL) / 1000.0;
    }
    public static string Title(IntPtr h) { var b = new StringBuilder(512); GetWindowText(h, b, b.Capacity); return b.ToString(); }
    public static string PrimaryMode() {
      DEVMODE d = new DEVMODE(); d.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
      return EnumDisplaySettings(null, -1, ref d) ? d.dmPelsWidth + "x" + d.dmPelsHeight : "unknown";
    }
    public static string[] VisibleTitles(uint[] pids) {
      var set = new HashSet<uint>(pids); var o = new List<string>();
      EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
        if (set.Contains(p) && IsWindowVisible(h)) { var t = Title(h); if (t.Length > 0) o.Add(t); } return true; }, IntPtr.Zero);
      return o.ToArray();
    }
  }
}
'@
}
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$os = Get-CimInstance Win32_OperatingSystem
$steamPids = @(Get-Process -Name steam, steamwebhelper -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
$free = @{}; foreach ($d in 'C', 'D') { $drive = Get-PSDrive -Name $d -PSProvider FileSystem -ErrorAction SilentlyContinue; if ($drive) { $free[$d] = $drive.Free / 1GB } }
$cfg = Join-Path $env:LOCALAPPDATA 'acz-vr\runtime.cfg'
$baseline = $null
if (-not $SkipBaseline) {
    $baseline = (& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'l1.ps1') -Action status 2>&1 | Out-String).Trim()
    if (-not $baseline) { $baseline = 'no output' }
}
$facts = @{
    GameProcesses   = @(Get-Process -Name acz, AceCombat8 -ErrorAction SilentlyContinue | ForEach-Object Name | Select-Object -Unique)
    AppLaunch       = @(Get-CimInstance Win32_Process -Filter "Name='steam.exe'" | Where-Object { $_.CommandLine -match '-applaunch' } | ForEach-Object ProcessId)
    CommitPercent   = 100.0 * ($os.TotalVirtualMemorySize - $os.FreeVirtualMemory) / $os.TotalVirtualMemorySize
    VrServerRunning = [bool](Get-Process -Name vrserver -ErrorAction SilentlyContinue)
    FreeGB          = $free
    Resolution      = [AczPreflight.Native]::PrimaryMode()
    IdleSeconds     = [AczPreflight.Native]::IdleSeconds()
    ForegroundTitle = [AczPreflight.Native]::Title([AczPreflight.Native]::GetForegroundWindow())
    SteamTitles     = @(if ($steamPids.Count) { [AczPreflight.Native]::VisibleTitles([uint32[]]$steamPids) })
    Baseline        = $baseline
    CfgKeys         = if (Test-Path -LiteralPath $cfg) { @(Get-Content -LiteralPath $cfg | Where-Object { $_ -match '^\s*[^#;\s][^=]*=' }).Count } else { -1 }
}
$results = Test-Preflight $facts -Xr:$Xr -ExpectedCfgKeys $ExpectedCfgKeys -MinIdleSeconds $MinIdleSeconds -ExpectedResolution $ExpectedResolution -MinFreeGB $MinFreeGB
foreach ($x in $results) { '{0,-4} {1,-11} {2}' -f $x.State, $x.Name, $x.Detail }
$failed = @($results | Where-Object State -eq 'FAIL')
if ($failed.Count) { "PREFLIGHT FAIL ($($failed.Count)): $(($failed | ForEach-Object Name) -join ', '). Do not spawn the live driver."; exit 1 }
"PREFLIGHT PASS ($(if ($Xr) { 'XR' } else { 'non-XR' }))"
exit 0
