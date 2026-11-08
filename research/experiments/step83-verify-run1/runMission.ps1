param([string]$Mission='02',[string]$Tag='C1',[string]$Cfg='cfg-L1.cfg',[int]$Seconds=180,[int]$MaxMin=9,
      [string]$MissionKeys='',[switch]$ForceStage,[string]$FxSig='4caf6e2d',[int]$BurstEvery=20,[double]$Alt=0,[string]$AP='',
      [string]$Root='D:\acz-evidence-archive\clouds',[switch]$WhatIf)
# Cloud / other-mission capture route (item 9, docs/world-render/cloud-window-only-item9.md), templated on runL.ps1: null SteamVR on -> SteamVR, sampler, killcap
# (mirror 1 fps + game window 1 fps, never a faster mode), png-burst.now rewrite every -BurstEvery s, runK2 route, autopilot -Mode Cruise -Total $Seconds -> ALWAYS (finally)
# close game, restore vrsettings (byte-identical), copy logs. Evidence in $Root\run$Tag.
#
# MISSION REACHABILITY (evidence: notes.md 'Run M', 2026-10-27/28; stages-and-detection.md 2): the save only offers CAMPAIGN > Continue = mission 02 (m02a); ACE STYLE has
# no mission select; no live run has ever flown another mission. So:
#   -Mission 02 (default)      : the PROVEN route (runK2: R4 menu path, START MISSION, takeoff k00a, Esc skip, then the m02a flight).
#   -Mission <other>           : the SAME route plus -MissionKeys (UNVERIFIED keys tapped on the mission-menu screen before START MISSION, e.g. 'left,left'),
#                                every key screenshotted (04-mission-sel-*.png). The game almost certainly still starts mission 02: the script logs the
#                                detected stage (flight-stage-installed) and says MISMATCH when it is not the requested stage. A different mission needs the save
#                                to have it unlocked (finish m02 / a save from the user's PC), nothing here can unlock it.
#   -ForceStage                : writes the mission's stage id into xr_terrain_stage of the run cfg (instead of auto). Changes OUR terrain/sky/cloud sheet only, NEVER the
#                                game's own cloud draws (the game still flies mission 02), so it is no substitute for item 9; useful to look at another stage's look.
# CLOUD DECKS (docs/world-render/stage-table.md, 4 layer altitudes, game units): m02a 16000/15000/16500/18000; m07a 5000/5000/11500/10000 (the best match for the
# autopilot cruise altitude 5000); m03k/m/n 6000/6000/5000/5250; m01a 12900-14350; k00a 12000-18500. The default autopilot cruise (TargetAlt 5000, AltCeil 12000) stays
# 10 kilo-units BELOW the m02a decks, so for m02a use -Alt 13500 (cruise just under the lowest deck at 15000; AltCeil is set to Alt+3000): UNVERIFIED that the plant climbs
# there and that decks are visible from below. Use -Alt 0 to keep the autopilot defaults.
# -WhatIf: prints the resolved plan (stage, cfg, autopilot args, routes, paths) and exits; nothing is launched, written outside the scratchpad-free dry path, or restored.
$X=$PSScriptRoot; $n=$Tag; $E="$Root\run$n"; $B="$X\runMissionbatch.log"
$stageMap=@{'01'='m01a';'02'='m02a';'03'='m03k';'04'='m04k';'05'='m05a';'06'='m06a';'07'='m07a';'08'='m08k';'09'='m09a';'10'='m10k';'11'='m11a';'12'='m12a';'13'='m13a';'14'='m14k';'15'='m15a';'16'='m16k';'17'='m17a';'18'='m18a';'SP'='r01a'}
$Mission=$Mission.ToUpper() -replace '[KMN]$',''
if(-not $stageMap.ContainsKey($Mission)){ throw "-Mission '$Mission' is not 01..18 or SP" }
$stage=$stageMap[$Mission]
if(-not $AP){
  $AP="-Mode Cruise -Total $Seconds"
  if($Alt -gt 0){ $AP+=" -TargetAlt $Alt -AltCeil $($Alt+3000)" }
}
if($Mission -ne '02' -and -not $MissionKeys){ Write-Warning "mission $Mission requested without -MissionKeys: the route starts whatever 'Continue' offers (mission 02)." }
$cfgSrc="$X\$Cfg"; if(-not (Test-Path $cfgSrc)){ throw "cfg $cfgSrc not found" }
$cfgText=Get-Content $cfgSrc -Raw
$runCfg=$cfgText -replace 'fxsig:[0-9a-fA-F]+',"fxsig:$FxSig"
if($ForceStage){ $runCfg=$runCfg -replace '(xr_terrain_stage=)auto',"`${1}$stage" }
$vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
$plan=[ordered]@{mission=$Mission;expected_stage=$stage;force_stage=[bool]$ForceStage;cfg=$Cfg;fxsig=$FxSig;mission_keys=$MissionKeys;autopilot=$AP;mirror_1fps=$true;burst_every_s=$BurstEvery;evidence=$E;maxmin=$MaxMin}
if($WhatIf){
  Write-Host "WhatIf plan:"; $plan.GetEnumerator() | % { Write-Host ("  {0} = {1}" -f $_.Key,$_.Value) }
  $xl=($runCfg -split "`r?`n" | ? { $_ -match '^xr_terrain_stage=' }); Write-Host "  run cfg terrain line: $xl"
  foreach($f in 'null-vr.ps1','sampler.ps1','killcap.ps1','runK2.ps1','autopilot.ps1'){ Write-Host ("  helper {0}: {1}" -f $f,(Test-Path "$X\$f")) }
  Write-Host ("  vrsettings present: {0}" -f (Test-Path $vrset)); return
}
New-Item -ItemType Directory -Force "$E\shots" | Out-Null
function BL($m){ "{0} [{1}] {2}" -f (Get-Date -f HH:mm:ss),$n,$m | Add-Content $B }
try {
  BL "start mission=$Mission stage=$stage cfg=$Cfg force=$ForceStage keys=$MissionKeys AP=$AP"
  Set-Content "$E\run.cfg" $runCfg -Encoding ascii; Copy-Item "$E\run.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\png-burst.now","$env:LOCALAPPDATA\acz-vr\fxdiag.arm" -ea 0
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt"
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }
  Start-Sleep 8
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  # 1 fps mirror + game window (Slow=Fast=1, FastFor=0: no faster mode), triggered by 'autopilot start'
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\killcap.ps1`"",'-E',"`"$E`"",'-MirrorDir',"`"$E\mirror`"",'-Slow','1','-Fast','1','-FastFor','0','-GameSlow','1','-GameFast','1','-MaxMin',"$MaxMin" -WindowStyle Hidden
  if($BurstEvery -gt 0){ Start-Process pwsh -ArgumentList '-NoProfile','-Command',"while(-not (Select-String -Path '$E\steps.txt' -Pattern 'autopilot start' -Quiet -ea 0)){Start-Sleep 2}; while(-not (Test-Path '$E\stopN')){ Start-Sleep $BurstEvery; Set-Content (Join-Path `$env:LOCALAPPDATA 'acz-vr\png-burst.now') (Get-Date -f HH:mm:ss.fff) -Encoding ascii; Add-Content '$E\steps.txt' 'png-burst rewritten' }" -WindowStyle Hidden }
  Start-Sleep 3
  $k2=@('-E',$E,'-AP',$AP); if($MissionKeys){ $k2+=@('-MissionKeys',$MissionKeys) }
  & pwsh -NoProfile -File "$X\runK2.ps1" @k2
} catch { BL "ERROR $($_.Exception.Message)" }
finally {
  BL 'cleanup'
  New-Item -ItemType File "$E\stopN" -Force | Out-Null
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  New-Item -ItemType Directory -Force "$E\logs" | Out-Null
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$E\logs" -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\fxdiag.arm","$env:LOCALAPPDATA\acz-vr\png-burst.now" -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  $h=(Get-FileHash $vrset).Hash.Substring(0,8); BL "vrsettings restored hash $h"
  $xl="$E\logs\xr-stereo.log"; $det=''
  if(Test-Path $xl){ $m=Select-String -Path $xl -Pattern 'flight-stage-installed;flight=\d+;stage=(\w+)' -ea 0 | select -Last 1; if($m){ $det=$m.Matches[0].Groups[1].Value } }
  $verdict= if(-not $det){ 'no flight stage detected' } elseif($det -eq $stage -or ($stage -match '^m(\d\d)[kmn]$' -and $det -like "m$($Matches[1])*")){ 'stage OK' } else { "MISMATCH: flew $det, requested $stage" }
  BL "detected stage '$det' ($verdict); game gone; shots $((Get-ChildItem "$E\mirror" -ea 0).Count) mirror"
  # item 9 checks (cloud-window-only-item9.md step 4)
  $gc=(Select-String -Path $xl -Pattern 'game-clouds;replay=off' -ea 0).Count; $ig=(Select-String -Path "$E\logs\step5-producer.log" -Pattern 'effects-gamecloud;.*ignored' -ea 0).Count
  BL "game-clouds replay=off lines $gc, producer gamecloud ignored lines $ig"
}
