# Batch AK (2026-10-30 ~03:50): step129. UI3D census of EVERY menu screen on the route (main menu, campaign, mission select, briefing,
# START MISSION, aircraft select / hangar, loading): ui3d-census.arm re-armed every 15 s (delete, 5 s, recreate) and png-screen.now every 8 s
# from launch until the autopilot log exists; then 30 s of flight. cfg-UI (ui3d:1) so the 3D menus run too.
$X=$PSScriptRoot; $arm="$env:LOCALAPPDATA\acz-vr\ui3d-census.arm"; $now="$env:LOCALAPPDATA\acz-vr\png-screen.now"
$E='D:\acz-evidence-archive\step124\runK9'; Remove-Item $E -Recurse -Force -ea 0
"{0} [AK] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
$job=Start-Job { param($E,$arm,$now,$B)
  $t=0; while(-not (Test-Path "$E\autopilot-1.log") -and $t -lt 600){
    if(($t % 15) -eq 0){ Remove-Item $arm -ea 0; Start-Sleep 5; $t+=5; Set-Content $arm 'AK' -Encoding ascii; Add-Content $B ("{0} [K9] census re-armed" -f (Get-Date -f HH:mm:ss)) }
    if(($t % 8) -eq 0){ Set-Content $now 'AK' -Encoding ascii }
    Start-Sleep 1; $t+=1 }
  Remove-Item $arm,$now -ea 0 } -ArgumentList $E,$arm,$now,"$X\runQbatch.log"
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag K9 -Cfg 'cfg-UI.cfg' -AP '-Mode Gun -Total 30'
Stop-Job $job -ea 0; Remove-Item $arm,$now -ea 0
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AK] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
