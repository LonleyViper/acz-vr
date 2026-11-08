# Batch AL (2026-10-30 ~05:00): step131 (UI3D stage 3b hangar room). L1 menu route with ;ui3d:1;u3a:1, png-screen.now every 8 s from launch until
# the autopilot log exists (aircraft select / weapon screens), then 60 s of flight (no-regression check).
$X=$PSScriptRoot; $now="$env:LOCALAPPDATA\acz-vr\png-screen.now"
$E='D:\acz-evidence-archive\step124\runL11'; Remove-Item $E -Recurse -Force -ea 0
"{0} [AL] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
$job=Start-Job { param($E,$now) $t=0; while(-not (Test-Path "$E\autopilot-1.log") -and $t -lt 600){ if(($t % 8) -eq 0){ Set-Content $now 'AL' -Encoding ascii }; Start-Sleep 1; $t+=1 }; Remove-Item $now -ea 0 } -ArgumentList $E,$now
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag L11 -Cfg 'cfg-UIA.cfg' -AP '-Mode Gun -Total 60'
Stop-Job $job -ea 0; Remove-Item $now -ea 0
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AL] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
