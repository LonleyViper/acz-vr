# Batch AF (2026-10-30 night): step126 (UI3D stage 2) with the null-vr showMirrorView fix. F1 3D menus (ui3d:1) + boxes/roll (Gun + LookAtContacts),
# F2 lens flare (SunZoom), F3 terrain baseline (cam pose), F4 terrain fxmem:0;gamecloud:0, F5 cloud-cut vertices (fxsig:4caf6e2d). Day cfg cfg-U restored at the end.
$X=$PSScriptRoot
"{0} [AF] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag F1 -Cfg 'cfg-UI.cfg' -AP '-Mode Gun -Total 360 -LookAtContacts -LookRange 14000'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag F2 -Cfg 'cfg-F2cam.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag F2b -Cfg 'cfg-F2cam-b.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag F3 -Cfg 'cfg-P0.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag F4 -Cfg 'cfg-P4.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag F5 -Cfg 'cfg-F5.cfg' -AP '-Mode Gun -Total 240'
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AF] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
