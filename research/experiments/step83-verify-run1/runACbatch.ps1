# Batch AC (2026-10-29 night): step124 null verification. AC1 boxes outside the rectangle / roll / lock diamond / clouds (Gun + LookAtContacts),
# AC2 lens flare (SunZoom on the SE sun heading, Y2 parameters). Day cfg cfg-U restored at the end.
$X=$PSScriptRoot
while(-not (Select-String -Path "$X\runQbatch.log" -Pattern '\[AB\] done' -Quiet)){ Start-Sleep 10 }; Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag C1 -Cfg 'cfg-U.cfg' -AP '-Mode Gun -Total 300 -LookAtContacts -LookRange 14000'
Start-Sleep 20
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag C2 -Cfg 'cfg-U.cfg' -AP '-Mode Sun -SunZoom -SunHdg -2.269 -ZoomPitch 0.7 -ZoomTop 3300 -ZoomBase 1900 -AltCeil 9000 -NoArmFx -Total 420'
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AC] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
