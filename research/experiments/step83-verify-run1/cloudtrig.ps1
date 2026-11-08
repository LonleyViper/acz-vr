# cloudtrig.ps1 (2026-10-31): telemetry-triggered eye dumps for the null cloud flights (FX-PUFF-MEM / fxcloud / fxatlas:1).
# Problem it fixes: time-based dumps (cloudshots.ps1, flight frame 240) kept landing on moments with no cloud in view although the producer counted thousands of
# off-window cloud draws. This watcher tails the LIVE step5-producer.log (shared read) and the run's autopilot-1.log, and writes %LOCALAPPDATA%\acz-vr\png-eye.now
# (the DLL re-arms xr_png_once for one fresh flight frame; needs xr_png_once=true in the run cfg = the batch's -Png runs) paired with png-screen.now (the playbook
# pairing; in flight png-screen.now dumps the virtual screen, so the eye PNG is the evidence) when a log line proves that cloud cards are drawn OFF the window:
#   fx-puff-mem  line: live=s<S>,c<C> with C > 0  AND hosts > 0  AND offeye > 0
#   fx-puff-atlas line (fxatlas:1): drawn > 0 AND the latest fx-puff-mem line (|frame delta| <= 180) has C > 0 and offeye > 0
# Gate: only DURING or just AFTER a look-left/right tap (autopilot-1.log 'LOOK'/'LOOKC' line + hold + -LookGrace s), because the forward window hides the off-eye cards
# unless the head turns; -NoLookGate for held-camera runs (AGC2). At most -Max dumps per flight, at least -MinGap s apart.
# Output: <RunDir>\cloudtrig.log (human log) and <RunDir>\cloudtrig.tsv (one row per dump: n, wall time, flight T, look, trigger kind, the producer line(s), eye PNG names).
# The PNG files themselves are copied by the batch with logs\png (and upright\ by Analyze-Common); this script only records their names.
# Modes:  live (default, started by runBatchAG for AGC1/AGC2)   -Analyze <RunDir>  list each dump with its trigger line (+ replay of what the rule WOULD have fired on that
#         run's logs, with the pngs it already has matched by frame)   -WhatIf  print the plan and the replay, touch nothing.
# Example (existing evidence): pwsh -NoProfile -File cloudtrig.ps1 -RunDir D:\acz-evidence-archive\step166\atlas\runAGC1 -Analyze
param([Parameter(Mandatory)][string]$RunDir,
      [int]$Max=6, [double]$MinGap=10, [double]$LookGrace=2.0, [switch]$NoLookGate,
      [double]$WaitFileS=900,           # live: give up if no autopilot log appears within this many seconds
      [string]$LogDir=(Join-Path $env:LOCALAPPDATA 'acz-vr\logs'),
      [string]$FlagDir=(Join-Path $env:LOCALAPPDATA 'acz-vr'),   # where png-eye.now / png-screen.now go (tests point it at a temp dir)
      [switch]$Analyze, [switch]$WhatIf)
$ErrorActionPreference='Stop'
$inv=[Globalization.CultureInfo]::InvariantCulture
function KvOf([string]$line,[string]$k){ $m=[regex]::Match($line,"(?:^|[;|])$k=(-?[\d.]+)"); if($m.Success){ [double]::Parse($m.Groups[1].Value,$inv) } else { -1.0 } }
function Tick([string]$line){ $m=[regex]::Match($line,'^(\d+)\|'); if($m.Success){ [int64]$m.Groups[1].Value } else { -1 } }
# Classify ONE producer line against the latest mem state $st (hashtable: c, off, frame, line). Returns $null or @{kind; why}.
function Judge([string]$l,$st){
  if($l -match 'event=fx-puff-mem;'){
    $m=[regex]::Match($l,'live=s(\d+),c(\d+)'); $c=$(if($m.Success){ [int]$m.Groups[2].Value } else { 0 }); $h=KvOf $l 'hosts'; $o=KvOf $l 'offeye'; $f=KvOf $l 'frame'
    $st.c=$c; $st.off=$o; $st.frame=$f; $st.line=$l
    if($c -gt 0 -and $h -gt 0 -and $o -gt 0){ return @{kind='mem'; why="live c=$c hosts=$([int]$h) offeye=$([int]$o)"} } }
  elseif($l -match 'event=fx-puff-atlas;'){
    $d=KvOf $l 'drawn'; $f=KvOf $l 'frame'
    if($d -gt 0 -and $st.c -gt 0 -and $st.off -gt 0 -and [math]::Abs($f-$st.frame) -le 180){ return @{kind='atlas'; why="atlas drawn=$([int]$d) with live c=$($st.c) offeye=$([int]$st.off) (mem frame $([int]$st.frame))"} } }
  $null }
function EyeFrames([string]$dir){ if(-not (Test-Path $dir)){ return @() }; @(Get-ChildItem $dir -Filter 'acz-vr-xr-eye0-f*-final.png' -ea 0 | % { [pscustomobject]@{frame=[int](([regex]::Match($_.Name,'-f(\d+)-final')).Groups[1].Value); name=$_.Name} } | sort frame) }

# ----------------------------------------------------------------------------------------------------------------------------------------------------------- analysis / WhatIf
function Show-Analysis {
  $pl=Join-Path $RunDir 'logs\step5-producer.log'; $tsv=Join-Path $RunDir 'cloudtrig.tsv'
  Write-Output "  --- CLOUD-TRIGGER dumps (cloudtrig.ps1: eye dump when the producer shows off-window cloud cards; rule above)"
  $png=@(); foreach($d in 'upright','logs\png'){ $p=EyeFrames (Join-Path $RunDir $d); if($p.Count){ $png=$p; Write-Output "  [INFO] eye dumps found in $d : $($p.Count) ($(($p | % { $_.frame }) -join ','))"; break } }
  if(-not $png.Count){ Write-Output '  [INFO] no eye-dump PNGs in upright\ or logs\png of this run dir' }
  # producer index: per frame the mem/atlas telemetry (for matching dumps to their moment)
  $mem=@{}; $atl=@{}; $ring=New-Object System.Collections.Generic.List[object]
  if(Test-Path $pl){
    $st=@{c=0;off=0;frame=-1;line=''}
    foreach($l in [IO.File]::ReadLines($pl)){
      if($l -notmatch 'event=fx-puff-(mem|atlas);'){ continue }
      $f=[int](KvOf $l 'frame'); if($l -match 'event=fx-puff-mem;'){ $mem[$f]=$l } else { $atl[$f]=$l }
      $j=Judge $l $st; if($j){ $ring.Add([pscustomobject]@{tick=(Tick $l);frame=$f;kind=$j.kind;why=$j.why;line=$l}) } }
  } else { Write-Output "  [WARN] no logs\step5-producer.log in $RunDir" }
  function Near($h,[int]$f){ $best=$null; foreach($k in $h.Keys){ if($null -eq $best -or [math]::Abs($k-$f) -lt [math]::Abs($best-$f)){ $best=$k } }; if($null -ne $best -and [math]::Abs($best-$f) -le 130){ $h[$best] } else { $null } }
  function Short([string]$l){ if(-not $l){ return '(no producer line within 130 frames)' }; $x=[regex]::Match($l,'event=fx-puff-(mem|atlas);frame=\d+;.*?(live=s\d+,c\d+|on=1)'); ($l -replace '^\d+\|gpu-copy-step1;','') -replace ';(nohost|xkey|nomatch|gone|cexp|cemit|cdrift|clip|ehide|unb|renew|kch|sw|sc|run|dup|cst|cref|amiss|hostno|hwhy|lay|capno|cwhy|ccap|nor|dead|rdy|rec|ev|full|shr|ref|cap|of)=[^;]*','' }
  if(Test-Path $tsv){
    $rows=@(Import-Csv $tsv -Delimiter "`t")
    Write-Output "  [INFO] LIVE dumps written by the trigger: $($rows.Count) (cap $Max)"
    foreach($r in $rows){
      Write-Output ("  dump {0}  wall {1}  flight T={2}  look={3}  trigger={4}" -f $r.n,$r.wall,$r.flightT,$r.look,$r.kind)
      Write-Output ("      trigger line: {0}" -f $r.line)
      if($r.companion){ Write-Output ("      paired line : {0}" -f $r.companion) }
      Write-Output ("      eye pngs: {0}   png-screen.now written: {1}" -f $r.pngs,$r.screen) }
    $ok=@($rows | ? { $_.pngs -and $_.pngs -ne '-' }).Count
    Write-Output ("  [{0}] cloud-trigger dumps with an eye PNG {1} of {2}" -f $(if($rows.Count -ge 1 -and $ok -eq $rows.Count){'PASS'}elseif($ok){'WARN'}else{'FAIL'}),$ok,$rows.Count)
    Write-Output "  [LOOK] upright\*-final.png of each listed frame: PASS = cloud puffs/smoke cards visible away from the forward window, no straight cut at the window border."
  } else {
    Write-Output '  [INFO] no cloudtrig.tsv (this run did not use the trigger): REPLAY of the rule on its producer log (no look gate: the autopilot log has flight seconds, not ticks)'
    $n=0; $last=-1; $pick=@()
    foreach($m in $ring){ if($n -ge $Max){ break }; if($last -ge 0 -and ($m.tick-$last) -lt ($MinGap*1000)){ continue }; $last=$m.tick; $n++; $pick+=$m }
    Write-Output ("  [{0}] qualifying producer lines (live c>0, hosts/drawn>0, offeye>0): {1} total; the rule would write {2} dump(s) (max {3}, gap {4} s)" -f $(if($ring.Count){'INFO'}else{'WARN'}),$ring.Count,$pick.Count,$Max,$MinGap)
    $i=0; foreach($m in $pick){ $i++; Write-Output ("  would-dump {0}  tick {1}  frame {2}  kind={3}  ({4})" -f $i,$m.tick,$m.frame,$m.kind,$m.why); Write-Output ("      trigger line: {0}" -f (Short $m.line)) }
    if($png.Count){
      Write-Output '  [INFO] the dumps this run DID take (time-based), matched by frame to the producer telemetry:'
      foreach($p in $png){ $ml=Near $mem $p.frame; $al=Near $atl $p.frame
        $c=0;$h=0;$o=0; if($ml){ $mm=[regex]::Match($ml,'live=s(\d+),c(\d+)'); $c=[int]$mm.Groups[2].Value; $h=[int](KvOf $ml 'hosts'); $o=[int](KvOf $ml 'offeye') }
        $hit=($c -gt 0 -and $o -gt 0)
        Write-Output ("  dump f{0}: live c={1} hosts={2} offeye={3} -> {4}" -f $p.frame,$c,$h,$o,$(if($hit){'cloud cards off-eye at this moment'}else{'NO live off-eye cloud card: expect plain sky/terrain'})); Write-Output ("      mem : {0}" -f (Short $ml)); if($al){ Write-Output ("      atlas: {0}" -f (Short $al)) } } }
  } }

if($Analyze -or $WhatIf){
  if($WhatIf){ Write-Output ("WhatIf: cloudtrig plan: tail {0}\step5-producer.log + {1}\autopilot-1.log; max {2} dumps, gap {3} s, look gate {4} (grace {5} s); writes png-eye.now then png-screen.now; nothing is written now" -f $LogDir,$RunDir,$Max,$MinGap,$(if($NoLookGate){'OFF'}else{'ON'}),$LookGrace) }
  Show-Analysis; return }

# ----------------------------------------------------------------------------------------------------------------------------------------------------------- live watcher
$dir=$FlagDir; $nowEye=Join-Path $dir 'png-eye.now'; $nowScr=Join-Path $dir 'png-screen.now'; $pd=Join-Path $LogDir 'png'
$pl=Join-Path $LogDir 'step5-producer.log'; $al=Join-Path $RunDir 'autopilot-1.log'; $stop=Join-Path $RunDir 'stopN'
$logf=Join-Path $RunDir 'cloudtrig.log'; $tsv=Join-Path $RunDir 'cloudtrig.tsv'
function Log($m){ $l=('{0} {1}' -f (Get-Date -Format 'HH:mm:ss.fff'),$m); Write-Host $l; try { Add-Content $logf $l } catch {} }
function Open-Tail([string]$p){ $share=[IO.FileShare]([int][IO.FileShare]::ReadWrite + [int][IO.FileShare]::Delete)   # NOT '-bor' inside a New-Object arg list: that parsed as an array and killed the 2026-10-31 live run
  $fs=New-Object IO.FileStream -ArgumentList $p,([IO.FileMode]::Open),([IO.FileAccess]::Read),$share
  [pscustomobject]@{fs=$fs;sr=(New-Object IO.StreamReader($fs,[Text.Encoding]::UTF8));path=$p;created=(Get-Item -LiteralPath $p).CreationTimeUtc} }
function Next-Line($t){ if($t.fs.Length -lt $t.fs.Position){ $t.fs.Position=0; $t.sr.DiscardBufferedData() }; $t.sr.ReadLine() }
trap { Log ("FATAL: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"); exit 2 }   # a hidden window swallowed the first live failure
Log ("cloudtrig start: max {0}, gap {1} s, look gate {2} (grace {3} s)" -f $Max,$MinGap,$(if($NoLookGate){'OFF'}else{'ON'}),$LookGrace)
$t0=Get-Date; while(-not (Test-Path $al)){ if((Test-Path $stop) -or ((Get-Date)-$t0).TotalSeconds -gt $WaitFileS){ Log 'no autopilot log: stop'; exit 1 }; Start-Sleep -Milliseconds 500 }
Log 'autopilot log seen'
Remove-Item $nowEye -Force -ea 0
$seen=@{}; Get-ChildItem $pd -Filter 'acz-vr-*.png' -ea 0 | % { $seen[$_.Name]=$true }
$ap=Open-Tail $al; $pt=$null; $st=@{c=0;off=0;frame=-1;line=''}; $lastAtlas=''; $lookUntil=-1.0; $lookDesc='-'; $flightT=0.0
$lnP=0; $lnA=0; $qual=0; $memN=0; $atlN=0; $lastL=''; $lastDiag=-99.0; $lastErr=''
$n=0; $lastDump=[datetime]::MinValue; $clock=[Diagnostics.Stopwatch]::StartNew()
if(-not (Test-Path $tsv)){ Set-Content $tsv ("n`twall`tflightT`tlook`tkind`tline`tcompanion`tpngs`tscreen") -Encoding ascii }
function Clean($s){ ($s -replace "[`t`r`n]",' ') }
try {
  while($n -lt $Max){
    if(Test-Path $stop){ Log 'stopN: stop'; break }
    if(-not $pt){ if(Test-Path $pl){ try { $pt=Open-Tail $pl; Log ("producer log opened: $pl size $($pt.fs.Length) (tailing from the start of the file)") } catch { if($_.Exception.Message -ne $lastErr){ $lastErr=$_.Exception.Message; Log "producer log open FAILED: $lastErr" } } } }
    # autopilot: flight T and look windows
    while($null -ne ($a=Next-Line $ap)){ $lnA++
      $m=[regex]::Match($a,'^T=\s*([\d.]+)'); if($m.Success){ $flightT=[double]::Parse($m.Groups[1].Value,$inv) }
      $lm=[regex]::Match($a,'LOOKC? (lookleft|lookright) hold ([\d.]+)'); if($lm.Success){ $lookUntil=$clock.Elapsed.TotalSeconds+[double]::Parse($lm.Groups[2].Value,$inv)+$LookGrace; $lookDesc=("{0} T={1:N1}" -f $lm.Groups[1].Value,$flightT) } }
    $any=$false
    if($pt){ while($null -ne ($l=Next-Line $pt)){ $any=$true; $lnP++; $lastL=$l; if($l -match 'event=fx-puff-mem;'){ $memN++ } elseif($l -match 'event=fx-puff-atlas;'){ $atlN++ }
        if($l -match 'event=fx-puff-atlas;'){ $lastAtlas=$l }
        $j=Judge $l $st; if(-not $j){ continue }; $qual++
        $nowS=$clock.Elapsed.TotalSeconds; $inLook=($NoLookGate -or $nowS -le $lookUntil)
        if(-not $inLook){ continue }
        if(((Get-Date)-$lastDump).TotalSeconds -lt $MinGap){ continue }
        $n++; $lastDump=Get-Date
        Set-Content $nowEye (Get-Date -Format o); Set-Content $nowScr 'AG' -Encoding ascii
        Log ("dump {0}: png-eye.now + png-screen.now written at flight T={1:N1} ({2}; {3}) look={4}" -f $n,$flightT,$j.kind,$j.why,$(if($NoLookGate){'(gate off)'}else{$lookDesc}))
        Log ("  trigger line: {0}" -f ($l -replace '^\d+\|gpu-copy-step1;',''))
        $w=Get-Date; while((Test-Path $nowEye) -and ((Get-Date)-$w).TotalSeconds -lt 8){ Start-Sleep -Milliseconds 200 }
        $cons=-not (Test-Path $nowEye); if(-not $cons){ Log '  png-eye.now not consumed within 8 s (no fresh flight frame, xr_png_once off, or no FLIGHT-DUMP DLL): removed'; Remove-Item $nowEye -Force -ea 0 }
        Start-Sleep 3; Remove-Item $nowScr -Force -ea 0
        $new=@(Get-ChildItem $pd -Filter 'acz-vr-xr-eye0-f*-final.png' -ea 0 | ? { -not $seen[$_.Name] }); Get-ChildItem $pd -Filter 'acz-vr-*.png' -ea 0 | % { $seen[$_.Name]=$true }
        $names=$(if($new.Count){ ($new | % { $_.Name }) -join ',' } else { '-' }); Log "  eye pngs: $names"
        $comp=$(if($j.kind -eq 'atlas'){ $st.line } else { $lastAtlas })
        Add-Content $tsv ("{0}`t{1}`t{2:N1}`t{3}`t{4}`t{5}`t{6}`t{7}`t{8}" -f $n,(Get-Date -Format 'HH:mm:ss'),$flightT,(Clean $lookDesc),$j.kind,(Clean ($l -replace '^\d+\|gpu-copy-step1;','')),(Clean ($comp -replace '^\d+\|gpu-copy-step1;','')),$names,$cons) -Encoding ascii
        if($n -ge $Max){ break } } }
    $es=$clock.Elapsed.TotalSeconds
    if($es-$lastDiag -ge 10){ $lastDiag=$es
      if($pt -and (Test-Path $pl)){ $ci=Get-Item -LiteralPath $pl; if($ci.CreationTimeUtc -ne $pt.created){ Log 'producer log was re-created: reopening'; try { $pt.sr.Dispose() } catch {}; $pt=$null } }
      $lk=$(if($NoLookGate){'gate off'}elseif($es -le $lookUntil){'IN look window'}else{'no look window'})
      Log ("diag: producer {0} size {1} lines read {2} (mem {3}, atlas {4}) qualifying {5}; autopilot lines {6} flightT {7:N1}; {8}; dumps {9}; last producer line: {10}" -f $(if($pt){'open'}else{'NOT open (exists='+(Test-Path $pl)+')'}),$(if($pt){$pt.fs.Length}else{-1}),$lnP,$memN,$atlN,$qual,$lnA,$flightT,$lk,$n,$(if($lastL.Length -gt 140){$lastL.Substring(0,140)}else{$lastL})) }
    if(-not $any){ if(-not (Get-Process acz -ea 0) -and $clock.Elapsed.TotalSeconds -gt 30 -and $flightT -gt 0){ Log 'acz gone: stop'; break }; Start-Sleep -Milliseconds 250 }
  }
} catch { Log ("ERROR in watcher loop: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)") } finally { Remove-Item $nowEye,$nowScr -Force -ea 0; Log "cloudtrig done: $n dump(s)" }
