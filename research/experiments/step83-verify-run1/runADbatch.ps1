# Batch AD (2026-10-29 night): step125 (UI3D census merged). D1 menu census (ui3d-census.arm from launch) + boxes/roll (Gun + LookAtContacts, m03k bounds,
# cutscene wait), D2 lens flare (SunZoom), D3 terrain P0 baseline rerun, D4 terrain P4 (fxmem:0;gamecloud:0). Day cfg cfg-U restored at the end.
$X=$PSScriptRoot; $arm="$env:LOCALAPPDATA\acz-vr\ui3d-census.arm"
"{0} [AD] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
Set-Content $arm 'armed by runADbatch' -Encoding ascii
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag D1 -Cfg 'cfg-U.cfg' -AP '-Mode Gun -Total 360 -LookAtContacts -LookRange 14000'
Remove-Item $arm -ea 0
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag D2 -Cfg 'cfg-U.cfg' -AP '-Mode Sun -SunZoom -SunHdg -2.269 -ZoomPitch 0.7 -ZoomTop 3300 -ZoomBase 1900 -AltCeil 9000 -NoArmFx -Total 420'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag D3 -Cfg 'cfg-P0.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag D4 -Cfg 'cfg-P4.cfg' -AP '-Mode Gun -Total 60'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag D5 -Cfg 'cfg-F5.cfg' -AP '-Mode Gun -Total 240'   # cloud-cut vertices (fxsig:4caf6e2d, armed at T0)
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AD] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
