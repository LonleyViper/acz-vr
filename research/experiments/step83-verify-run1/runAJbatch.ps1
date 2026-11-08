# Batch AJ2 (2026-10-30 ~03:40): step129 (W13 fixes). J3 3D menus + in-flight pause menu (ui3d:1;u3p:1) with ui3d-census-tail.arm.
# A helper waits until the autopilot log exists in the fresh evidence dir (flight started), +40 s, then Esc pause 20 s / Esc resume, twice.
$X=$PSScriptRoot; $tail="$env:LOCALAPPDATA\acz-vr\ui3d-census-tail.arm"; $drv=(Resolve-Path "$X\..\..\..\tools\live\acz-window-driver.ps1").Path
$E='D:\acz-evidence-archive\step124\runJ3'; Remove-Item $E -Recurse -Force -ea 0
"{0} [AJ2] start" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
Set-Content $tail 'AJ2' -Encoding ascii
$job=Start-Job { param($E,$drv,$B)
  while(-not (Test-Path "$E\autopilot-1.log")){ Start-Sleep 2 }
  Add-Content $B ("{0} [J3] helper: autopilot log seen" -f (Get-Date -f HH:mm:ss)); Start-Sleep 40
  foreach($k in 1,2){
    & pwsh -NoProfile -File $drv -Action tap -Key back -AcknowledgeLiveInput | Out-Null; Add-Content $B ("{0} [J3] Esc pause {1}" -f (Get-Date -f HH:mm:ss),$k); Start-Sleep 20
    & pwsh -NoProfile -File $drv -Action tap -Key back -AcknowledgeLiveInput | Out-Null; Add-Content $B ("{0} [J3] Esc resume {1}" -f (Get-Date -f HH:mm:ss),$k); Start-Sleep 25 }
} -ArgumentList $E,$drv,"$X\runQbatch.log"
& pwsh -NoProfile -File "$X\runAA1.ps1" -Tag J3 -Cfg 'cfg-UIP.cfg' -AP '-Mode Gun -Total 200'
Stop-Job $job -ea 0; Remove-Item $tail -ea 0
Copy-Item "$X\cfg-U.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
"{0} [AJ2] done" -f (Get-Date -f HH:mm:ss) | Add-Content "$X\runQbatch.log"
