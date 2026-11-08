param([ValidateSet('default','raw','own')][string]$V='default')
$X=$PSScriptRoot; $A='D:\acz-evidence-archive\step111'; $E="$X\runF2$V"
New-Item -ItemType Directory -Force "$E\shots","$A" | Out-Null
$opt=@{default='';raw=';tclut:raw';own=';toenv:own'}[$V]
(Get-Content "$X\cfg-I.cfg") | % { if($_ -match '^xr_hud_markers_layer='){ 'xr_png_once=true' } elseif($_ -match '^xr_terrain_stage='){ $_+$opt } else { $_ } } | Set-Content "$env:LOCALAPPDATA\acz-vr\runtime.cfg"
Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
if(-not (Test-Path "$E\steamvr.vrsettings.pre-run-backup")){ & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt" }
Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }; Start-Sleep 8
Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
& pwsh -NoProfile -File "$X\runF2.ps1" -E $E
Get-Process acz -ea 0 | Stop-Process -Force -ea 0; New-Item -ItemType File "$E\stopN" -Force | Out-Null; Start-Sleep 2
New-Item -ItemType Directory -Force "$A\runF2$V-logs" | Out-Null
Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force | % { Copy-Item -LiteralPath $_.FullName -Destination "$A\runF2$V-logs" -Recurse -Force }
Copy-Item "$E\shots" "$A\runF2$V-shots" -Recurse -Force
Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
