param([string]$E)
# P1 (step116 zoom check): overview + BRIEFING-3D; dump before zoom, accelerate, 3 s, dump, accelerate, dump.
$X=$PSScriptRoot; $drv=(Resolve-Path (Join-Path $X '..\..\..\tools\live\acz-window-driver.ps1')).Path
function L($m){ "{0} tick={1} {2}" -f (Get-Date -f HH:mm:ss.fff),[Environment]::TickCount,$m | Add-Content "$E\steps.txt" }
function Shot($n){ pwsh -NoProfile -File "$X\shot.ps1" -Out "$E\shots\$n.png" | Out-Null; L "shot $n" }
function Stop?{ if(Test-Path "$E\GAMEOVER"){ L 'GAMEOVER: stop'; exit 3 }; if(-not (Get-Process acz -ea 0)){ L 'acz gone'; exit 4 } }
function T($k){ Stop?; pwsh -NoProfile -File $drv -Action tap -Key $k -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" *> $null; L "tap $k" }
function W($s){ $t=[Diagnostics.Stopwatch]::StartNew(); while($t.Elapsed.TotalSeconds -lt $s){ Stop?; Start-Sleep -Milliseconds 250 } }
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class WX{[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);}'
[void][WX]::SetProcessDPIAware(); New-Item -ItemType Directory -Force "$E\shots" | Out-Null
Start-Process 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\acz.exe' -WorkingDirectory 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
$p=$null; for($k=0;$k -lt 60 -and -not $p;$k++){ Start-Sleep -Milliseconds 500; $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1 }
if(-not $p){ L 'no launcher'; exit 1 }
Start-Sleep 4; $r=New-Object WX+RECT; [void][WX]::GetWindowRect($p.MainWindowHandle,[ref]$r); [void][WX]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 800
if([math]::Abs($r.L-880) -gt 12 -or [math]::Abs($r.T-380) -gt 12){ L "launcher rect $($r.L),$($r.T): abort"; exit 1 }
pwsh -NoProfile -File "$X\shot.ps1" -ClickX 1368 -ClickY 886 | Out-Null; L 'clicked Launch Game'
W 40; Shot '01-mainmenu'; W 6
T 'accelerate'; W 2; T 'accelerate'; W 5; Shot '02-cinematic'; W 6
T 'back'; W 6; Shot '03-mappres'; W 6
T 'back'; W 5; Shot '04-overview-0'; L 'OVERVIEW start'; W 10; Set-Content (Join-Path $env:LOCALAPPDATA 'acz-vr\png-screen.now') (Get-Date -Format o); L 'png-screen.now written'; W 5; Shot '05-overview-15'; W 15; Shot '06-overview-30'; W 5; L 'OVERVIEW end'
T 'back'; W 4; Shot '07-mission-menu'
T 'down'; W 1; T 'down'; W 1; Shot '08-briefing-sel'
T 'confirm'; W 5; Shot '09-briefing-mappres'; W 8
T 'back'; W 5; Shot '10-briefing-3d'; L 'BRIEFING-3D start (idle baseline 15 s)'; W 15; Shot '11-baseline'
function Dump($n){ Set-Content (Join-Path $env:LOCALAPPDATA 'acz-vr\png-screen.now') (Get-Date -Format o); Start-Sleep -Milliseconds 2500; Shot $n; pwsh -NoProfile -File "$X\mirror-shot.ps1" -Out "$E\shots\$n-mirror.png" | Out-Null; L "dump $n done" }
Dump 'p-before'; Dump 'p-before2'
L 'ZOOM accelerate tap 1'; T 'accelerate'; W 3; Dump 'p-zoomed'; W 3; Dump 'p-zoomed2'
L 'ZOOM accelerate tap 2 (un-zoom)'; T 'accelerate'; W 3; Dump 'p-unzoomed'; W 2; Dump 'p-unzoomed2'
L 'BRIEFING-3D end'
L 'closing game'; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; for($k=0;$k -lt 20 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }
L 'done'
