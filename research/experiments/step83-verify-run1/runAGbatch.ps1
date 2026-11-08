# Batch AG (2026-10-30 night): eye-image evidence (pre-compositor dumps; the null mirror is unreliable tonight). G1 3D menus (ui3d:1) with
# png-screen.now every 12 s through the menus; G2/G3 flare cam (both yaw signs) + xr_png_once; G4/G5 terrain baseline / fxmem:0;gamecloud:0 + xr_png_once.
$X=$PSScriptRoot; $now="$env:LOCALAPPDATA\acz-vr\png-screen.now"
while(-not (Select-String -Path "$X\runQbatch.log" -Pattern '\[AF\] done' -Quiet)){ Start-Sleep 10 }; Start-Sleep 20
"{0} [AG] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
$job=Start-Job { param($n) Start-Sleep 25; for($i=0;$i -lt 18;$i++){ Set-Content $n 'G1' -Encoding ascii; Start-Sleep 12 } } -ArgumentList $now
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag G1 -Cfg 'cfg-UI.cfg' -AP '-Mode Gun -Total 120'
Stop-Job $job -ea 0; Remove-Item $now -ea 0
foreach($p in @(@('G2','cfg-F2cam-png.cfg'),@('G3','cfg-F2cam-b-png.cfg'),@('G4','cfg-P0-png.cfg'),@('G5','cfg-P4-png.cfg'))){
  Start-Sleep 20; & pwsh -NoProfile -File "$X\runAA1.ps1" -Tag $p[0] -Cfg $p[1] -AP '-Mode Gun -Total 60' }
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AG] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
