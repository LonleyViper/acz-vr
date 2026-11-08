param([string]$E,[string]$AP='-Mode Cruise -Total 120',[string]$AP2='',[string]$MissionKeys='')
# K2 (autopilot run, R4 menu route: runway, skip, skip, 90 s flight): launch, route83-noskip to mission 02 (no menu walk) with XR already running (R4 path), start mission 02, takeoff 25 s, Esc skip, V, fly without input until
# MISSION FAILED (off course), RESTART (flight 2), 60 s, close. Screenshots + step times in $E. Stops early if GAMEOVER (sampler stop rule) appears.
$X=$PSScriptRoot; $drv=Join-Path (Split-Path (Split-Path $X)) '..\tools\live\acz-window-driver.ps1'; $drv=(Resolve-Path $drv).Path
$cdrv=$drv -replace 'acz-window-driver.ps1','acz-window-driver-combat.ps1'
function L($m){ "{0} {1}" -f (Get-Date -f HH:mm:ss.fff),$m | Add-Content "$E\steps.txt" }
function Shot($n){ pwsh -NoProfile -File "$X\shot.ps1" -Out "$E\shots\$n.png" | Out-Null; L "shot $n" }
function Stop?{ if(Test-Path "$E\GAMEOVER"){ L 'GAMEOVER: stop'; exit 3 }; if(-not (Get-Process acz -ea 0)){ L 'acz gone'; exit 4 } }
function T($k){ Stop?; pwsh -NoProfile -File $drv -Action tap -Key $k -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" *> $null; L "tap $k" }
function W($s){ $t=[Diagnostics.Stopwatch]::StartNew(); while($t.Elapsed.TotalSeconds -lt $s){ Stop?; Start-Sleep -Milliseconds 250 } }
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class WV{[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);}'
[void][WV]::SetProcessDPIAware()
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
Start-Process 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\acz.exe' -WorkingDirectory 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
$p=$null; for($k=0;$k -lt 60 -and -not $p;$k++){ Start-Sleep -Milliseconds 500; $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1 }
if(-not $p){ L 'no launcher'; exit 1 }
Start-Sleep 4; $r=New-Object WV+RECT; [void][WV]::GetWindowRect($p.MainWindowHandle,[ref]$r); [void][WV]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 800
if([math]::Abs($r.L-880) -gt 12 -or [math]::Abs($r.T-380) -gt 12){ L "launcher rect $($r.L),$($r.T): abort"; exit 1 }
pwsh -NoProfile -File "$X\shot.ps1" -ClickX 1368 -ClickY 886 | Out-Null; L 'clicked Launch Game'
L 'menu route (R4 path)'; W 40; Shot '01-mainmenu'; W 4
T 'accelerate'; W 2; T 'accelerate'; W 6; Shot '02-cinematic'
T 'back'; W 6; T 'back'; W 6; Shot '03-overview'
T 'back'; W 4; Shot '04-mission-menu'
if($MissionKeys){ $mi=0; foreach($mk in ($MissionKeys -split '[,; ]+' | ? {$_})){ $mi++; T $mk; W 2; Shot ("04-mission-sel-$mi-$mk") }; L "mission-select keys: $MissionKeys" }   # runMission.ps1: UNVERIFIED mission-select keys before START MISSION; screenshots show where they landed
T 'accelerate'; L 'START MISSION'; W 6; T 'confirm'; W 7; T 'confirm'; W 6; T 'confirm'; W 8; T 'confirm'; L 'deploy confirmed'; W 20; Shot '05-loaded'
W 8
function DetN { try { (Select-String -Path "$env:LOCALAPPDATA\acz-vr\logs\xr-stereo.log" -Pattern 'flight-stage-detect' -ea Stop).Count } catch { 0 } }
function EnsureCockpit { $b0=DetN; if($b0 -gt $script:det0){ L 'flight already detected'; $script:det0=$b0; return }; for($v=1;$v -le 3;$v++){ Stop?; pwsh -NoProfile -File $cdrv -Action tap -Key cockpit-view -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" *> $null; L "V try $v"; W 4; $n1=DetN; if($n1 -gt $script:det0){ L "flight detected after V try $v"; $script:det0=$n1; return } }; L 'no flight detected after 3 V' }
$script:det0=0; EnsureCockpit
W 15; Shot '13-runway'; W 25; Shot '14-roll'
L 'm03 air start: no takeoff skip'; W 5
$script:det0=0; EnsureCockpit
L "autopilot start: $AP"; $apl="$E\autopilot-1.log"; Invoke-Expression "& pwsh -NoProfile -File `"$X\autopilot.ps1`" $AP -DecisionLog `"$apl`"" *> "$E\autopilot-1-console.txt"; L "autopilot 1 exit=$LASTEXITCODE"
if($AP2 -and (Get-Process acz -ea 0)){ L "autopilot 2 start: $AP2"; $apl="$E\autopilot-2.log"; Invoke-Expression "& pwsh -NoProfile -File `"$X\autopilot.ps1`" $AP2 -DecisionLog `"$apl`"" *> "$E\autopilot-2-console.txt"; L "autopilot 2 exit=$LASTEXITCODE" }

L 'closing game'; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; for($k=0;$k -lt 20 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }
L 'done'
