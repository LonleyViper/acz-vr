param([string]$Name,[string]$Cfg)
# One dtex A/B flight run: runtime.cfg = $Cfg, sampler, runD.ps1, collect logs/shots to D:\acz-evidence-archive\step104\run$Name-*.
$X=$PSScriptRoot; $E="$X\run$Name"; $A='D:\acz-evidence-archive\step104'
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
Copy-Item "$X\$Cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force; Copy-Item "$X\$Cfg" "$E\runtime.cfg"
Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden; Start-Sleep 3
& pwsh -NoProfile -File "$X\runD.ps1" -E $E
Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 5; Get-Process acz -ea 0 | Stop-Process -Force
New-Item -ItemType File "$E\stopN" -Force | Out-Null; Start-Sleep 3
New-Item -ItemType Directory -Force "$A\run$Name-logs" | Out-Null
Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force | % { Move-Item -LiteralPath $_.FullName -Destination "$A\run$Name-logs" -Force }
Move-Item "$E\shots" "$A\run$Name-shots"; New-Item -ItemType Junction -Path "$E\shots" -Target "$A\run$Name-shots" | Out-Null
"done $Name"
