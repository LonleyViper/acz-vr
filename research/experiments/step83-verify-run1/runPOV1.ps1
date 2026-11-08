param([string]$Root='D:\acz-evidence-archive\pov1',
      [string]$Tag='POV1',
      [string]$Append=';pov:1;povc:1;povrig:level',   # appended to xr_terrain_stage (docs/world-render/other-povs.md section 8)
      [string]$BaseCfg='installed',     # 'installed' = a byte copy of the installed %LOCALAPPDATA%\acz-vr\runtime.cfg (the day cfg), or the path of a cfg file (e.g. cfg-U.cfg)
      [string]$AP='-Mode Cruise -Total 150',
      [int]$PostWait=150,               # runK3ag -PostWait: the aircraft is left unflown after the autopilot: MISSION FAILED -> end orbit (cinematic route)
      [string]$MissionKeys='',
      [int]$Taps=3,                     # V taps (cockpit -> chase -> HUD-only -> cockpit)
      [int]$FirstDelay=15,              # s after 'autopilot start' in steps.txt before the first V tap
      [int]$TapGap=20,                  # s between V taps
      [int]$PngDelay=5,                 # s after each tap: png-screen.now trigger
      [string[]]$Views=@('chase','hud-only','cockpit'),   # names logged for tap 1, 2, 3 ...
      [string]$ExpectDxgi='',           # expected installed dxgi.dll hash prefix (take it from the build record); required for a live run
      [string]$ExpectD3d12='',          # expected installed d3d12.dll hash prefix
      [switch]$AllowOtherBuild,         # only to run on DLLs that do NOT match -ExpectDxgi/-ExpectD3d12
      [int]$MaxMemPct=86,               # commit % allowed before the launch (waits up to 5 min, then aborts); the per-run sampler stops a run at 95 % twice / 97 %
      [int]$CineVisits=3,               # (2026-10-31, pov:3) after the taps the side job watches xr-stereo.log: each new `pov-stage2;request=cinematic` (pov:3) or
                                        # `pov-census;...route=cinematic` (pov:1/2) line triggers png-eye.now + png-screen.now at +0.2 / +2 / +5 s; at most this many visits (0 = off)
      [int]$WatchMax=900,               # s the watcher keeps watching after the last tap (the post-flight wait ends earlier: the job is stopped in cleanup)
      [string]$Analyze,                 # analysis only (no launch): an existing run dir -> analyze-pov.py
      [switch]$WhatIf)
# runPOV1.ps1 (2026-10-30): ONE single-launch null-HMD live run of POV stage 1 (docs/world-render/other-povs.md section 8): cockpit -> chase -> HUD-only -> cockpit with the V key,
# per-view camera census (pov:1;povc:1;povrig:level). Modelled on runBatchAG.ps1 (cfg handling, mem gate, hash expectations, null SteamVR, sampler/killcap, byte-exact restores) and
# runK3ag.ps1 (the menu route to a mission 03 air start + autopilot + -PostWait). Nothing here edits an existing script.
#   side job: waits for 'autopilot start' in steps.txt, waits -FirstDelay s, taps V (acz-window-driver-combat.ps1 -Action tap -Key cockpit-view) -Taps times -TapGap s apart, and writes
#   %LOCALAPPDATA%\acz-vr\png-screen.now -PngDelay s after each tap. Every step is logged with a timestamp in $E\pov-side.log (and 'V -> <view>' lines in steps.txt).
#   afterwards: logs copied to $E\logs, analyze-pov.py run on $E (-> $E\analysis-pov.txt), runtime.cfg / last-aircraft.json / vrsettings restored byte-exactly (hashes checked).
# Usage:  pwsh -NoProfile -File runPOV1.ps1 -ExpectDxgi <8 hex> -ExpectD3d12 <8 hex>        (live run, ~10 min)
#         pwsh -NoProfile -File runPOV1.ps1 -WhatIf                                          (plan + cfg checks only: launches, writes and restores nothing)
#         pwsh -NoProfile -File runPOV1.ps1 -Analyze <runDir>                                (re-run the analysis only)
#   POV3 (2026-10-31, docs/world-render/other-povs.md section 11.6): -Tag POV3 -Append ';pov:3;povc:1;povrig:level' -AP '-Mode Cruise -Total 150 -TargetAlt 9000'
#         -PostWait 240: chase / HUD-only / cockpit 20 s each, then the aircraft unflown (from 9,000 units, not 5,000: runPOV2b descended into the
#         terrain, crashed and got the REPLAY instead of the end camera) -> MISSION FAILED -> the mission-end camera (the cinematic route); the watcher
#         dumps the eyes when the cinematic request appears.
$ErrorActionPreference='Stop'
$X=$PSScriptRoot; $Repo=(Resolve-Path (Join-Path $X '..\..\..')).Path
$LD="$env:LOCALAPPDATA\acz-vr"; $rt="$LD\runtime.cfg"
$vrset='C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
$GameDir='D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
$drvc=Join-Path $Repo 'tools\live\acz-window-driver-combat.ps1'
$analyzer=Join-Path $X 'analyze-pov.py'
$E="$Root\run$Tag"
function Hash8([string]$f){ if(Test-Path -LiteralPath $f){ (Get-FileHash -LiteralPath $f).Hash.Substring(0,8) } else { '--------' } }
$script:LogOk=$false
function BL([string]$m){ $line="{0} [POV1] {1}" -f (Get-Date -f HH:mm:ss),$m; Write-Host $line; if($script:LogOk){ Add-Content "$Root\runPOV1.log" $line } }

if($Analyze){ & python -I $analyzer $Analyze; exit $LASTEXITCODE }

# ------------------------------------------------------------------ cfg builder (New-RunCfg style): append to xr_terrain_stage, 250-character limit, 32-key limit, producer pin
function New-RunCfg([string]$Base,[string]$App){
  $t=$Base
  if($App){ $t=[regex]::Replace($t,'(?m)^(xr_terrain_stage=[^\r\n]*)',{ param($m) $m.Groups[1].Value+$App }) }
  # eye dumps per view (png-eye.now re-arms xr_png_once, effects-outside-window.md 20.8): swap the feather key so the key count stays the same
  if($t -match 'xr_edge_feather=0\.06' -and $t -notmatch 'xr_png_once=true'){ $t=$t -replace 'xr_edge_feather=0\.06','xr_png_once=true' }
  $terr=([regex]::Match($t,'(?m)^xr_terrain_stage=([^\r\n]*)')).Groups[1].Value
  if(-not $terr){ throw 'cfg has no xr_terrain_stage line' }
  if($terr.Length -gt 1000){ throw "terrain value $($terr.Length) chars > 1000 (buffer 1023 since step158)" }
  $keys=@($t -split "`r?`n" | ? { $_ -match '^[A-Za-z0-9_]+=' }).Count
  if($keys -gt 32){ throw "cfg has $keys keys > 32" }
  if($t -notmatch 'diagnostic_ownership_pins=true'){ throw 'diagnostic_ownership_pins missing (the producer needs it)' }
  $t }
$baseText = if($BaseCfg -eq 'installed'){ [IO.File]::ReadAllText($rt) } else { [IO.File]::ReadAllText((Resolve-Path $BaseCfg).Path) }
$cfg=New-RunCfg $baseText $Append
$terr=([regex]::Match($cfg,'(?m)^xr_terrain_stage=([^\r\n]*)')).Groups[1].Value
$nkeys=@($cfg -split "`r?`n" | ? { $_ -match '^[A-Za-z0-9_]+=' }).Count
$warn=@()
if($terr -notmatch '(^|;)vsearly:1'){ $warn+='terrain value has no vsearly:1' }
if($terr -notmatch '(^|;)pov:[123]'){ $warn+='terrain value has no pov:1|2|3' }
if($terr -notmatch '(^|;)povc:1'){ $warn+='terrain value has no povc:1 (no census)' }
if($terr -notmatch '^auto(;|$)'){ $warn+='terrain stage is not auto' }
if($cfg -notmatch '(?m)^ee_camera_probe=true'){ $warn+='ee_camera_probe=true missing' }
$taplist=@(1..[math]::Max($Taps,0) | % { $v=$Views[($_-1) % $Views.Count]; [pscustomobject]@{n=$_;view=$v;tap=$FirstDelay+($_-1)*$TapGap;png=$FirstDelay+($_-1)*$TapGap+$PngDelay} })

# ------------------------------------------------------------------ WhatIf: print the plan and the cfg, touch nothing
if($WhatIf){
  Write-Host 'WhatIf: runPOV1 plan (nothing is launched, written or restored)'
  Write-Host ("  repo {0}`n  evidence dir {1}" -f $Repo,$E)
  Write-Host ("  installed runtime.cfg {0}  base cfg '{1}'  installed dxgi {2} d3d12 {3}  vrsettings {4}" -f (Hash8 $rt),$BaseCfg,(Hash8 "$GameDir\dxgi.dll"),(Hash8 "$GameDir\d3d12.dll"),(Hash8 $vrset))
  Write-Host ("  expected dxgi '{0}' d3d12 '{1}' (AllowOtherBuild={2}); a live run needs both (or -AllowOtherBuild)" -f $ExpectDxgi,$ExpectD3d12,[bool]$AllowOtherBuild)
  Write-Host ("  cfg: terrain {0} chars (limit 1000), {1} keys (limit 32), append '{2}'" -f $terr.Length,$nkeys,$Append)
  Write-Host ("  terrain: {0}" -f $terr)
  foreach($w in $warn){ Write-Host "  CFG WARNING: $w" }
  Write-Host ("  route: runK3ag.ps1 -E {0} -AP '{1}' -PostWait {2} -PostTapMax 0{3}" -f $E,$AP,$PostWait,$(if($MissionKeys){" -MissionKeys '$MissionKeys'"}else{''}))
  Write-Host ("  side job: wait for 'autopilot start' in {0}\steps.txt, then (seconds after it) :" -f $E)
  foreach($t in $taplist){ Write-Host ("    +{0,3} s  tap cockpit-view (V) -> {1}   (tap {2} of {3});  +{4,3} s  png-screen.now" -f $t.tap,$t.view,$t.n,$Taps,$t.png) }
  Write-Host ("  cinematic watcher: {0} visit(s) max, {1} s after the last tap: each new request=cinematic / route=cinematic line in xr-stereo.log -> png-eye.now + png-screen.now at +0.2 / +2 / +5 s" -f $CineVisits,$WatchMax)
  Write-Host ("  restores: runtime.cfg byte copy (hash checked), last-aircraft.json, vrsettings (null-vr backup); logs -> {0}\logs; analysis -> {0}\analysis-pov.txt" -f $E)
  foreach($f in 'null-vr.ps1','sampler.ps1','killcap.ps1','runK3ag.ps1','autopilot.ps1','shot.ps1','analyze-pov.py'){ Write-Host ("  helper {0}: {1}" -f $f,(Test-Path "$X\$f")) }
  Write-Host ("  combat window driver {0}: {1}" -f $drvc,(Test-Path $drvc))
  Write-Host ("  mem-guard {0}: {1}" -f (Join-Path $Repo 'tools\mem-guard.ps1'),(Test-Path (Join-Path $Repo 'tools\mem-guard.ps1')))
  return }

# ------------------------------------------------------------------ preflight
if(-not $AllowOtherBuild -and (-not $ExpectDxgi -or -not $ExpectD3d12)){ throw 'pass -ExpectDxgi and -ExpectD3d12 (the build record hashes), or -AllowOtherBuild' }
New-Item -ItemType Directory -Force $Root | Out-Null; $script:LogOk=$true
BL "start; run dir $E; AP '$AP'; append '$Append'"
$busy=@(Get-Process acz,vrserver,vrcompositor,vrmonitor,vrstartup -ea 0)
if($busy){ throw "refusing to start: $(($busy | % { $_.Name }) -join ',') is running (one orchestrator, hands off while a game/SteamVR runs)" }
$ppid=(Get-CimInstance Win32_Process -Filter "ProcessId=$PID" -ea 0).ParentProcessId
$other=@(Get-CimInstance Win32_Process -Filter "Name='pwsh.exe' OR Name='powershell.exe'" -ea 0 | ? { $_.ProcessId -ne $PID -and $_.ProcessId -ne $ppid -and $_.CommandLine -match 'run[A-Za-z0-9]*batch\.ps1|runAA1\.ps1|runK3(ag)?\.ps1|runBatchAG\.ps1|runPOV1\.ps1|autopilot\.ps1|runMission\.ps1' })
if($other){ throw "refusing to start: another orchestrator/run script is alive: $(($other | % { $_.CommandLine.Substring(0,[math]::Min(120,$_.CommandLine.Length)) }) -join ' || ')" }
$dx=Hash8 "$GameDir\dxgi.dll"; $d3=Hash8 "$GameDir\d3d12.dll"; BL "installed dxgi $dx d3d12 $d3 (expected '$ExpectDxgi' / '$ExpectD3d12'); runtime.cfg $(Hash8 $rt); vrsettings $(Hash8 $vrset)"
if(-not $AllowOtherBuild){
  $ed=$ExpectDxgi.ToUpper().Substring(0,[math]::Min(8,$ExpectDxgi.Length)); $e3=$ExpectD3d12.ToUpper().Substring(0,[math]::Min(8,$ExpectD3d12.Length))
  if($dx -ne $ed -or $d3 -ne $e3){ throw "installed DLLs (dxgi $dx d3d12 $d3) do not match -ExpectDxgi $ed / -ExpectD3d12 $e3" } }
foreach($w in $warn){ BL "CFG WARNING: $w" }
$cFree=[math]::Round((Get-PSDrive C).Free/1GB,1); $dFree=[math]::Round((Get-PSDrive D).Free/1GB,1); BL "free space C: $cFree GB, D: $dFree GB"
if($dFree -lt 25){ throw "D: has only $dFree GB free (the run writes mirror frames)" }
if($cFree -lt 1.5){ throw "C: has only $cFree GB free (logs, png dumps and the pagefile live there)" }
function Commit-Pct { try { [double](Get-Counter '\Memory\% Committed Bytes In Use' -ea Stop).CounterSamples[0].CookedValue } catch { -1.0 } }
function Wait-Mem([string]$why){ $t0=Get-Date; while($true){ $p=Commit-Pct; if($p -lt 0){ BL "commit % unreadable ($why): continuing"; return }; if($p -lt $MaxMemPct){ BL ("commit {0:N1} % ok ({1})" -f $p,$why); return }
    if(((Get-Date)-$t0).TotalMinutes -ge 5){ throw ("commit {0:N1} % >= {1} % for 5 min before {2}: stop (memory stop rule)" -f $p,$MaxMemPct,$why) }; BL ("commit {0:N1} % >= {1} %: waiting ({2})" -f $p,$MaxMemPct,$why); Start-Sleep 20 } }
$mg=& pwsh -NoProfile -File (Join-Path $Repo 'tools\mem-guard.ps1') -Report 2>&1; $mgx=$LASTEXITCODE; BL "mem-guard -Report exit $mgx (2 = commit >= 88 %: abort)"; ($mg | Select-Object -Last 6) | % { BL "  mem-guard: $_" }
if($mgx -ge 2){ throw 'mem-guard reports commit >= 88 %: abort (close the ChatGPT app / Steam web helpers / qBittorrent, then rerun)' }

# byte copies of what is INSTALLED now, restored exactly in the finally block
$pre="$Root\runtime.cfg.pre-run"; if(Test-Path $pre){ $old="$pre.prev-$(Get-Date -f yyyyMMdd-HHmmss)"; Move-Item -LiteralPath $pre -Destination $old -Force; BL "an older $pre was kept as $old" }
Copy-Item -LiteralPath $rt -Destination $pre -Force; $preHash=(Get-FileHash $pre).Hash
$la=Join-Path (Split-Path $rt) 'last-aircraft.json'; $laPre="$Root\last-aircraft.json.pre-run"; if(Test-Path $la){ Copy-Item -LiteralPath $la -Destination $laPre -Force }
$vrPre=(Get-FileHash $vrset).Hash
BL "installed runtime.cfg saved: $pre hash $($preHash.Substring(0,8)); vrsettings pre-run $($vrPre.Substring(0,8))"

# ------------------------------------------------------------------ the run
$jobs=@(); $now="$LD\png-screen.now"; $arm="$LD\ui3d-census.arm"; $tail="$LD\ui3d-census-tail.arm"
$armFiles=@("$LD\png-burst.now","$LD\fxdiag.arm",$now,$arm,$tail,"$LD\briefing-observe.arm","$LD\brief3d-capture.arm","$LD\ui3d-census-acm.arm")
try {
  Remove-Item -LiteralPath $E -Recurse -Force -ea 0; New-Item -ItemType Directory -Force "$E\shots" | Out-Null
  Wait-Mem "before $Tag"
  [IO.File]::WriteAllText("$E\run.cfg",$cfg,[Text.Encoding]::ASCII); Copy-Item "$E\run.cfg" $rt -Force
  BL "run.cfg installed: terrain $($terr.Length) chars, $nkeys keys, hash $(Hash8 $rt)"
  Get-ChildItem $LD\logs -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Remove-Item $armFiles -ea 0
  & pwsh -NoProfile -File "$X\null-vr.ps1" -Mode on -E $E -NoDashboard | Add-Content "$E\steps.txt"
  Start-Process (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe')
  for($i=0;$i -lt 60 -and -not (Get-Process vrcompositor -ea 0);$i++){ Start-Sleep 1 }
  Start-Sleep 8
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  $apTotal=150; if($AP -match '-Total\s+(\d+)'){ $apTotal=[int]$Matches[1] }; $maxMin=[math]::Ceiling(($PostWait+$apTotal+420)/60)+4   # menu route ~6 min + flight + post-wait
  $kc=@('-NoProfile','-File',"`"$X\killcap.ps1`"",'-E',"`"$E`"",'-MirrorDir',"`"$E\mirror`"",'-Slow','1','-Fast','1','-FastFor','0','-GameSlow','3','-MaxMin',"$maxMin")
  Start-Process pwsh -ArgumentList $kc -WindowStyle Hidden
  # side job: V cycle + png-screen.now, logged with timestamps
  $jobs+=Start-Job { param($E,$drvc,$LD,$FirstDelay,$TapGap,$PngDelay,$Taps,$Views,$CineVisits,$WatchMax)
      # (2026-10-31) NOT 'SL': SL is PowerShell's built-in alias of Set-Location and aliases win over functions, so every log line of runPOV2b/runPOV3
      # went to Set-Location (no pov-side.log, no steps.txt lines); the taps and png triggers still ran.
      function PovSideLog([string]$m){ $l="{0} {1}" -f (Get-Date -f HH:mm:ss.fff),$m; Add-Content "$E\pov-side.log" $l
        try { Add-Content "$E\steps.txt" $l -ea Stop } catch { } }
      # (2026-10-31, runPOV3b) the wait matches ONLY runK3ag's own line '<HH:mm:ss.fff> autopilot start: <args>': this job's log lines also go to
      # steps.txt, and its first line used to contain the phrase itself, so the job saw its own line, tapped V in the menus and the V cycle never happened.
      PovSideLog "side job: waiting for the autopilot line in steps.txt (taps $Taps, first +$FirstDelay s, gap $TapGap s, png +$PngDelay s after each tap)"
      $w=[Diagnostics.Stopwatch]::StartNew()
      while(-not (Select-String -Path "$E\steps.txt" -Pattern '^\d\d:\d\d:\d\d\.\d+ autopilot start: ' -Quiet -ea 0)){
        if((Test-Path "$E\ROUTE-FAILED") -or (Test-Path "$E\GAMEOVER") -or $w.Elapsed.TotalSeconds -gt 1500){ PovSideLog 'side job: no autopilot start (route failed / GAMEOVER / timeout): exit'; return }
        Start-Sleep 2 }
      PovSideLog 'side job: autopilot line seen'
      $t0=[Diagnostics.Stopwatch]::StartNew()
      for($i=1;$i -le $Taps;$i++){
        $view=$Views[($i-1) % $Views.Count]; $tapAt=$FirstDelay+($i-1)*$TapGap
        while($t0.Elapsed.TotalSeconds -lt $tapAt){ Start-Sleep -Milliseconds 200 }
        if(Test-Path "$E\GAMEOVER"){ PovSideLog 'side job: GAMEOVER: stop'; return }
        $o=& pwsh -NoProfile -File $drvc -Action tap -Key cockpit-view -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" 2>&1; $ec=$LASTEXITCODE
        PovSideLog ("V tap {0}/{1} -> {2}: driver exit {3} {4}" -f $i,$Taps,$view,$ec,(($o | Out-String).Trim() -replace '\s+',' '))
        if($ec -ne 0){ Start-Sleep -Milliseconds 1500; $o=& pwsh -NoProfile -File $drvc -Action tap -Key cockpit-view -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots" 2>&1; $ec=$LASTEXITCODE
          PovSideLog ("V tap {0}/{1} -> {2}: RETRY driver exit {3} {4}" -f $i,$Taps,$view,$ec,(($o | Out-String).Trim() -replace '\s+',' ')) }
        while($t0.Elapsed.TotalSeconds -lt $tapAt+$PngDelay){ Start-Sleep -Milliseconds 200 }
        Set-Content "$LD\png-screen.now" 'POV1' -Encoding ascii; Set-Content "$LD\png-eye.now" 'POV1' -Encoding ascii; PovSideLog ("png-screen.now written (view {0}, +{1} s after the tap)" -f $view,$PngDelay)
      }
      PovSideLog 'side job: all taps done'
      # cinematic watcher (pov:3 stage 4 / pov:1-2 census): read only the NEW part of the live xr-stereo.log (shared read; the DLL keeps writing)
      if($CineVisits -le 0){ return }
      $xl="$LD\logs\xr-stereo.log"; $st=@{pos=0L}; $visits=0; $lastHit=$null; $plan=@()
      try { $st.pos=(Get-Item -LiteralPath $xl -ea Stop).Length } catch { $st.pos=0L }
      PovSideLog ("cinematic watcher: on ({0} visits max, {1} s), from byte {2} of {3}" -f $CineVisits,$WatchMax,$st.pos,$xl)
      $tw=[Diagnostics.Stopwatch]::StartNew()
      while($tw.Elapsed.TotalSeconds -lt $WatchMax){
        if(Test-Path "$E\GAMEOVER"){ PovSideLog 'cinematic watcher: GAMEOVER: stop'; break }
        $txt=''
        try { $fs=[IO.File]::Open($xl,'Open','Read','ReadWrite,Delete')
              try { if($fs.Length -lt $st.pos){ $st.pos=0L }; [void]$fs.Seek($st.pos,'Begin'); $sr=New-Object IO.StreamReader($fs); $txt=$sr.ReadToEnd(); $st.pos=$fs.Position } finally { $fs.Dispose() } } catch { }
        if($txt -and $visits -lt $CineVisits -and ($txt -match 'event=pov-stage2;request=cinematic' -or $txt -match 'event=pov-census;[^\r\n]*route=cinematic')){
          if(-not $lastHit -or ((Get-Date)-$lastHit).TotalSeconds -gt 15){
            $visits++; $lastHit=Get-Date; $t0=$tw.Elapsed.TotalSeconds
            $plan+=@(@{at=$t0+0.2;n=1},@{at=$t0+2;n=2},@{at=$t0+5;n=3})   # runPOV3: cinematic windows of 0.6 / 1.5 s; pov:3 also dumps in-DLL
            PovSideLog ("cinematic watcher: cinematic visit {0}/{1} seen ({2}); eye dumps at +0.2 / +2 / +5 s" -f $visits,$CineVisits,$(if($txt -match 'request=cinematic'){'pov-stage2 request=cinematic'}else{'pov-census route=cinematic'})) } }
        foreach($p in @($plan | ? { $_.at -le $tw.Elapsed.TotalSeconds })){
          Set-Content "$LD\png-eye.now" 'POV-CINE' -Encoding ascii; Set-Content "$LD\png-screen.now" 'POV-CINE' -Encoding ascii
          PovSideLog ("cinematic watcher: png-eye.now + png-screen.now written (dump {0} of 3)" -f $p.n) }
        $plan=@($plan | ? { $_.at -gt $tw.Elapsed.TotalSeconds })
        Start-Sleep -Milliseconds 250 }
      PovSideLog ("cinematic watcher: done ({0} visit(s))" -f $visits) } -ArgumentList $E,$drvc,$LD,$FirstDelay,$TapGap,$PngDelay,$Taps,$Views,$CineVisits,$WatchMax
  Start-Sleep 3
  $k3=@('-NoProfile','-File',"$X\runK3ag.ps1",'-E',$E,'-AP',$AP,'-PostWait',"$PostWait",'-PostTapMax','0')
  if($MissionKeys){ $k3+=@('-MissionKeys',$MissionKeys) }
  BL "runK3ag: $($k3 -join ' ')"
  & pwsh @k3
  BL "runK3ag exit $LASTEXITCODE"
} catch { BL "ERROR $($_.Exception.Message)" }
finally {
  BL 'cleanup'
  foreach($j in $jobs){ Stop-Job $j -ea 0; Remove-Job $j -Force -ea 0 }
  New-Item -ItemType Directory -Force $E | Out-Null
  New-Item -ItemType File "$E\stopN" -Force | Out-Null
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  New-Item -ItemType Directory -Force "$E\logs" | Out-Null
  Get-ChildItem "$LD\logs" -Force -ea 0 | % { Copy-Item -LiteralPath $_.FullName -Destination "$E\logs" -Recurse -Force -ea 0 }
  Remove-Item $armFiles -ea 0
  Get-Process vrmonitor,vrserver,vrcompositor,vrdashboard,vrwebhelper,vrstartup -ea 0 | Stop-Process -Force -ea 0
  Start-Sleep 3
  if(Test-Path "$E\steamvr.vrsettings.pre-run-backup"){ Copy-Item "$E\steamvr.vrsettings.pre-run-backup" $vrset -Force }
  Copy-Item -LiteralPath $pre -Destination $rt -Force
  if(Test-Path $laPre){ Copy-Item -LiteralPath $laPre -Destination $la -Force; BL "last-aircraft.json restored: $(Get-Content $la -Raw)" }
  BL ("installed runtime.cfg restored from byte copy: hash {0} identical={1}" -f (Hash8 $rt),((Get-FileHash $rt).Hash -eq $preHash))
  $vrNow=(Get-FileHash $vrset).Hash; BL ("vrsettings now {0} pre-run {1} identical={2}" -f $vrNow.Substring(0,8),$vrPre.Substring(0,8),($vrNow -eq $vrPre))
  try { & python -I $analyzer $E 2>&1 | Tee-Object -FilePath "$E\analysis-pov.txt" | Select-Object -Last 60 | % { BL "  analysis: $_" } } catch { BL "analysis failed: $($_.Exception.Message)" }
  BL "DONE; evidence $E"
}
