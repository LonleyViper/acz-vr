param([string]$Tag='Q1a')
# Batch Q (step123): k00a T10 F2 pair (runF2.ps1: game window shots + XR eye dump xr_png_once + png-screen.now) with cfg-N.cfg (day cfg + fxsig:f1a4e000).
# try/finally: close game, restore vrsettings (byte-identical), copy logs/shots to D:\acz-evidence-archive\step123\run$Tag.
$X=$PSScriptRoot; $A='D:\acz-evidence-archive\step123'; $E="$X\run$Tag"; $B="$X\runQbatch.log"
New-Item -ItemType Directory -Force "$E\shots","$A" | Out-Null
function BL($m){ "{0} [{1}] {2}" -f (Get-Date -f HH:mm:ss),$Tag,$m | Add-Content $B }
$vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
try {
  BL 'start'
  (Get-Content "$X\cfg-N.cfg") | % { if($_ -match '^xr_hud_markers_layer='){ 'xr_png_once=true' } else { $_ } } | Set-Content "$env:LOCALAPPDATA\acz-vr\runtime.cfg"
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt"
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }; Start-Sleep 8
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  & pwsh -NoProfile -File "$X\runF2.ps1" -E $E
} catch { BL "ERROR $($_.Exception.Message)" }
finally {
  BL 'cleanup'
  New-Item -ItemType File "$E\stopN" -Force | Out-Null
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 5; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  New-Item -ItemType Directory -Force "$A\run$Tag-logs" | Out-Null
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$A\run$Tag-logs" -Recurse -Force -ea 0 }
  Copy-Item "$E\shots" "$A\run$Tag-shots" -Recurse -Force -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  BL ("vrsettings restored hash {0}" -f (Get-FileHash $vrset).Hash.Substring(0,8))
}
