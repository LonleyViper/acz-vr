# Batch AK2 (2026-10-30 ~04:30): step130 (ACM record census). Menu route with ui3d-census.arm AND ui3d-census-acm.arm re-armed every 15 s
# and png-screen.now every 8 s from launch until the autopilot log exists (hangar / aircraft-select / weapon screens); then 30 s of flight. cfg-UI.
$X=$PSScriptRoot; $arm="$env:LOCALAPPDATA\acz-vr\ui3d-census.arm"; $acm="$env:LOCALAPPDATA\acz-vr\ui3d-census-acm.arm"; $now="$env:LOCALAPPDATA\acz-vr\png-screen.now"
$E='D:\acz-evidence-archive\step124\runK10'; Remove-Item $E -Recurse -Force -ea 0
"{0} [AK2] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
$job=Start-Job { param($E,$arm,$acm,$now,$B)
  $t=0; while(-not (Test-Path "$E\autopilot-1.log") -and $t -lt 600){
    if(($t % 15) -eq 0){ Remove-Item $arm,$acm -ea 0; Start-Sleep 5; $t+=5; Set-Content $arm 'AK2' -Encoding ascii; Set-Content $acm 'AK2' -Encoding ascii; Add-Content $B ("{0} [K10] census re-armed" -f (Get-Date -f HH:mm:ss)) }
    if(($t % 8) -eq 0){ Set-Content $now 'AK2' -Encoding ascii }
    Start-Sleep 1; $t+=1 }
  Remove-Item $arm,$acm,$now -ea 0 } -ArgumentList $E,$arm,$acm,$now,"$X\runQbatch.log"
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag K10 -Cfg 'cfg-UI.cfg' -AP '-Mode Gun -Total 30'
Stop-Job $job -ea 0; Remove-Item $arm,$acm,$now -ea 0
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AK2] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
