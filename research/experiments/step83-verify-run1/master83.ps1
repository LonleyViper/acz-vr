param([string]$Run,[string]$Terrain,[string]$Cockpit='auto;anim:1;default:j35jk_vrcockpit;zpre:1',[double]$Total=420,[int]$Max=4,[string]$Dll='D:\acz-dll-step83',[double]$StartGate=85,[string]$DebugView='0',[string]$PitchKey='',[double]$PitchLen=0.15,[switch]$Lean,[switch]$NoPng,[string]$MissileAt='',[string]$BurstAt='',[string]$TabAt='',[string[]]$ExtraSwitch=@(),[string]$CfgSwap='',[string]$Extra83='',[string]$Route='route77.ps1')
# One step83 live run: install $Dll with the vr-test-start pattern (PngOnce + the day switches), clean logs, sample, attempts, close, collect.
# Null SteamVR must already be on (null-vr.ps1 on). Logs go to D:\acz-evidence-archive\step83\run$Run-logs (not committed).
$ErrorActionPreference='Continue'
$X='<repo>\research\experiments\step83-verify-run1'
$repo='<repo>'
$arch='D:\acz-evidence-archive\step83'
$logs="$env:LOCALAPPDATA\acz-vr\logs"
$game='D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
New-Item -ItemType Directory -Force $arch | Out-Null
function M($m){ "{0} {1}" -f (Get-Date -f 'HH:mm:ss'),$m | Add-Content "$X\master.log" }
$E="$X\run$Run"; New-Item -ItemType Directory -Force "$E\shots","$E\archive\shots" | Out-Null
M "=== run $Run ($Dll) terrain '$Terrain' cockpit '$Cockpit' total $Total ==="
Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 5; Get-Process acz -ea 0 | Stop-Process -Force
$want=(Get-FileHash "$Dll\dxgi.dll").Hash; $have=(Get-FileHash "$game\dxgi.dll" -ea 0).Hash
if($have -and $have -ne $want){ M "installed dxgi $($have.Substring(0,8)) != $($want.Substring(0,8)): vr-test-stop"; & pwsh -NoProfile -File "$repo\tools\live\vr-test-stop.ps1" *>> "$X\master.log" }
$ln=@(); if($Lean){ $ln=@('-Lean') }; if(-not $NoPng){ $ln+='-PngOnce' }; $ln+=$ExtraSwitch; $cm=@(); if($Cockpit){ $cm=@('-CockpitModel',$Cockpit) }
& pwsh -NoProfile -File "$repo\tools\live\vr-test-start.ps1" -DllDir $Dll -NoWorldExtend -DepthFix -EffectsDual -OcclusionFix -TerrainRender $Terrain @cm -DebugView $DebugView @ln *> "$E\vr-test-start-out.txt"
$have=(Get-FileHash "$game\dxgi.dll" -ea 0).Hash; M "dxgi installed $($have.Substring(0,8)) want $($want.Substring(0,8))"
if($have -ne $want){ M 'DLL mismatch: abort'; exit 1 }
$rc="$env:LOCALAPPDATA\acz-vr\runtime.cfg"
if($CfgSwap){ foreach($pair in $CfgSwap.Split('|')){ $a,$b=$pair -split '=>'; $c0=Get-Content $rc; if($c0 -notcontains $a){ M "CfgSwap: '$a' not in cfg: abort"; exit 1 }; ($c0 | % { if($_ -eq $a){$b}else{$_} }) | Set-Content $rc -Encoding ascii; M "cfg swap $a -> $b" } }
$cfg=Get-Content "$env:LOCALAPPDATA\acz-vr\runtime.cfg"; Copy-Item "$env:LOCALAPPDATA\acz-vr\runtime.cfg" "$E\runtime.cfg"
M "cfg keys: $(@($cfg | ? { $_.Trim() }).Count), bytes $((Get-Item "$env:LOCALAPPDATA\acz-vr\runtime.cfg").Length)"
$pre="$arch\pre-run-$Run"; New-Item -ItemType Directory -Force $pre | Out-Null
Get-ChildItem $logs -Force -ea 0 | % { Move-Item -LiteralPath $_.FullName -Destination $pre -Force -ea 0 }
Remove-Item "$E\stopN","$E\GAMEOVER","$E\ATTEMPT-OK","$E\NOMISSION" -ea 0
Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
Start-Sleep 5
& pwsh -NoProfile -File "$X\attempts83.ps1" -E $E -Max $Max -Total $Total -StartGate $StartGate -PitchKey $PitchKey -PitchLen $PitchLen -MissileAt $MissileAt -BurstAt $BurstAt -TabAt $TabAt -Extra83 $Extra83 -Route $Route *> "$E\attempts-out.txt"
M "attempts ended; ATTEMPT-OK=$(Test-Path "$E\ATTEMPT-OK")"
Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }
for($k=0;$k -lt 30 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }
Get-Process acz -ea 0 | Stop-Process -Force
Start-Sleep 5
New-Item -ItemType File "$E\stopN" -Force | Out-Null
Start-Sleep 4
$dst="$arch\run$Run-logs"; New-Item -ItemType Directory -Force $dst | Out-Null
Get-ChildItem $logs -Force -ea 0 | % { Move-Item -LiteralPath $_.FullName -Destination $dst -Force -ea 0 }
M "run $Run logs moved to $dst"
'DONE' | Set-Content "$E\RUN-DONE"
