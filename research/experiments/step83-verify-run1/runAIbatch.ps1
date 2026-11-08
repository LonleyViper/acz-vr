# Batch AI (2026-10-30 night): step127 (W12 fix rounds). I1 3D menus (ui3d:1) with png-screen.now every 12 s through the menus + flight;
# I2 m03k terrain at the fixed pose with an eye dump (sea fix check; keeps diagnostic_ownership_pins); I3 day cfg flight 240 s (boxes/look).
$X=$PSScriptRoot; $now="$env:LOCALAPPDATA\acz-vr\png-screen.now"
"{0} [AI] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
$job=Start-Job { param($n) Start-Sleep 25; for($i=0;$i -lt 18;$i++){ Set-Content $n 'I1' -Encoding ascii; Start-Sleep 12 } } -ArgumentList $now
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag I1 -Cfg 'cfg-UI.cfg' -AP '-Mode Gun -Total 120'
Stop-Job $job -ea 0; Remove-Item $now -ea 0
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag I2 -Cfg 'cfg-P0-png2.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag I3 -Cfg 'cfg-U.cfg' -AP '-Mode Gun -Total 240 -LookAtContacts -LookRange 14000'
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AI] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
