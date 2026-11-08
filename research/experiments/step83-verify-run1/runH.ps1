param([int]$Att=1,[string]$AP='-Mode Kill -Total 420 -TargetColors green,yellow')
$X=$PSScriptRoot; $A='D:\acz-evidence-archive\step107'; $n="H$Att"; $E="$X\run$n"
New-Item -ItemType Directory -Force "$E\shots","$A" | Out-Null
Copy-Item "$X\cfg-H.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
Remove-Item "$env:LOCALAPPDATA\acz-vr\png-burst.now" -ea 0
if(-not (Test-Path "$E\steamvr.vrsettings.pre-run-backup")){ & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt" }
$vrs=Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe'
Start-Process $vrs
for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }
Start-Sleep 8
Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\killcap.ps1`"",'-E',"`"$E`"",'-MirrorDir',"`"$A\$n-mirror`"",'-FastFor','30','-MaxMin','14' -WindowStyle Hidden
Start-Sleep 3
& pwsh -NoProfile -File "$X\runK2.ps1" -E $E -AP $AP
Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force
New-Item -ItemType File "$E\stopN" -Force | Out-Null; Start-Sleep 3
New-Item -ItemType Directory -Force "$A\run$n-logs" | Out-Null
Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force | % { Copy-Item -LiteralPath $_.FullName -Destination "$A\run$n-logs" -Recurse -Force }
$p="$A\run$n-logs\step5-producer.log"; $ex=0; if(Test-Path $p){ $ex=(Select-String -Path $p -Pattern 'class=explosion' -ea 0).Count }
$fires=(Select-String -Path "$E\autopilot-1.log" -Pattern ' FIRE' -ea 0).Count; $guns=(Select-String -Path "$E\autopilot-1.log" -Pattern ' GUN' -ea 0).Count
"{0} attempt {1}: explosion-plan lines {2}, FIRE {3}, GUN {4}" -f (Get-Date -f HH:mm:ss),$n,$ex,$fires,$guns | Add-Content "$X\runHbatch.log"
Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
'done' | Add-Content "$E\steps.txt"
