param([string]$Tag='M4a',[string]$Cam='17110,1852,21312,135.7,53.1')
# Batch M4: fixed debug camera at the k00a runway pointing at the stage sun (kSun -> yaw 135.7, pitch 53.1 deg), cfg-M2 (fxsig:f1a4e000) + ;cam:<Cam>; fxdiag.arm touched 11 s after the flight is detected.
# try/finally: close game, restore vrsettings, copy logs; evidence D:\acz-evidence-archive\step114\run$Tag. The day cfg is restored by the caller.
$X=$PSScriptRoot; $E="D:\acz-evidence-archive\step114\run$Tag"; $n=$Tag; $B="$X\runLbatch2.log"; $vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
function BL($m){ "{0} [{1}] {2}" -f (Get-Date -f HH:mm:ss),$n,$m | Add-Content $B }
try {
  BL "start cam=$Cam"
  (Get-Content "$X\cfg-M2.cfg") | % { if($_ -match '^xr_terrain_stage='){ ($_ -replace '=auto;','=k00a;')+";cam:$Cam" } else { $_ } } | Set-Content "$env:LOCALAPPDATA\acz-vr\runtime.cfg"
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\fxdiag.arm" -ea 0
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt"
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }; Start-Sleep 8
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\killcap.ps1`"",'-E',"`"$E`"",'-MirrorDir',"`"$E\mirror`"",'-MaxMin','6','-StartTimeout','60' -WindowStyle Hidden
  & pwsh -NoProfile -File "$X\runM4route.ps1" -E $E
} catch { BL "ERROR $($_.Exception.Message)" }
finally {
  New-Item -ItemType File "$E\stopN" -Force | Out-Null
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3; New-Item -ItemType Directory -Force "$E\logs" | Out-Null
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$E\logs" -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\fxdiag.arm" -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  BL ("vrsettings restored hash " + (Get-FileHash $vrset).Hash.Substring(0,8))
  Copy-Item "$X\cfg-M1.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
}
