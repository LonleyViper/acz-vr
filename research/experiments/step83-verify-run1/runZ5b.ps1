param([string]$Tag='N1',[string]$AP='-Mode Gun -Total 300',[string]$Cfg='cfg-N.cfg',[int]$FastFor=40,[int]$MaxMin=14,[string]$GunTimes='',[switch]$ChaseV,[switch]$ArmAtSun)
# Batch L wrapper: null SteamVR on -> run (SteamVR, killcap, sampler, runK2 with autopilot [+gunburst]) -> ALWAYS (finally) close game, restore vrsettings,
# copy logs, move shots to D. Evidence in D:\acz-evidence-archive\step123\run$Tag.
# Batch M: -AP '-Mode Gun -Total 300' (gun strike) or -AP '-Mode Sun -Total 260' (sun look; use a cfg with fxsig:f1a4e000); Kill gates now default to LockDeg 9 / LockSec 0.5 / DiveFrom 9000.
# -GunTimes only fires manual J bursts; its own mirror capture is off (black frames): killcap.ps1 captures the mirror (view with mirror-sheet.ps1 -NoRotate).
$X=$PSScriptRoot; $A='D:\acz-evidence-archive\step123'; $n=$Tag; $E="$A\run$n"; $B="$X\runQbatch.log"
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
function BL($m){ "{0} [{1}] {2}" -f (Get-Date -f HH:mm:ss),$n,$m | Add-Content $B }
$vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
try {
  BL "start cfg=$Cfg AP=$AP gun=$GunTimes"
  Copy-Item "$X\$Cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\png-burst.now","$env:LOCALAPPDATA\acz-vr\fxdiag.arm" -ea 0
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt"
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }
  Start-Sleep 8
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\killcap.ps1`"",'-E',"`"$E`"",'-MirrorDir',"`"$E\mirror`"",'-FastFor',"$FastFor",'-MaxMin',"$MaxMin" -WindowStyle Hidden
  if($GunTimes){ Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\gunburst.ps1`"",'-E',"`"$E`"",'-Times',$GunTimes,'-MirrorDir',"`"$E\gun-mirror`"" -WindowStyle Hidden }
  if($ChaseV){ Start-Process pwsh -ArgumentList '-NoProfile','-Command',"while(-not (Select-String -Path '$E\steps.txt' -Pattern 'autopilot start' -Quiet -ea 0)){Start-Sleep 2}; Start-Sleep 15; pwsh -NoProfile -File '<repo>\tools\live\acz-window-driver-combat.ps1' -Action tap -Key cockpit-view -AcknowledgeLiveInput -ScreenshotDirectory '$E\shots'; Add-Content '$E\steps.txt' 'chase V tapped'" -WindowStyle Hidden }
  Start-Sleep 3
  if($ArmAtSun){ Start-Process pwsh -ArgumentList '-NoProfile','-Command',"while(-not (Select-String -Path '$E\autopilot-1.log' -Pattern 'ph=1 al=[1-9]' -Quiet -ea 0)){Start-Sleep 1}; Set-Content (Join-Path `$env:LOCALAPPDATA 'acz-vr\fxdiag.arm') ('sun aligned '+(Get-Date -f HH:mm:ss.fff)) -Encoding ascii; Add-Content '$E\steps.txt' 'fxdiag armed at sun alignment'" -WindowStyle Hidden }
  & pwsh -NoProfile -File "$X\runK2.ps1" -E $E -AP $AP
} catch { BL "ERROR $($_.Exception.Message)" }
finally {
  BL 'cleanup'
  New-Item -ItemType File "$E\stopN" -Force | Out-Null
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  New-Item -ItemType Directory -Force "$E\logs" | Out-Null
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$E\logs" -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\fxdiag.arm" -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  $h=(Get-FileHash $vrset).Hash.Substring(0,8); BL "vrsettings restored hash $h"
  $p="$E\logs\step5-producer.log"; $ex=0; if(Test-Path $p){ $ex=(Select-String -Path $p -Pattern 'class=explosion' -ea 0).Count }
  $fires=(Select-String -Path "$E\autopilot-1.log" -Pattern ' FIRE' -ea 0).Count; $guns=(Select-String -Path "$E\autopilot-1.log" -Pattern ' GUN' -ea 0).Count
  BL "explosion-plan lines $ex, FIRE $fires, GUN $guns"
}
