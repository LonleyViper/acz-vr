# Batch AH (2026-10-30 ~00:45): lens flare at a fixed sun-facing cam (m03k sun yaw -150 / pitch 50; both yaw signs) with xr_png_once eye dumps.
$X=$PSScriptRoot
"{0} [AH] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag FLa -Cfg 'cfg-F2cam-png.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag FLb -Cfg 'cfg-F2cam-b-png.cfg' -AP '-Mode Gun -Total 60'
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AH] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
