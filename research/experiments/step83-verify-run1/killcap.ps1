param([string]$E,[string]$MirrorDir,[double]$Slow=1.0,[double]$Fast=0.25,[double]$FastFor=20,[double]$GameSlow=5,[double]$GameFast=1,
      [string]$ApLog='autopilot-1.log',[string]$StartPattern='autopilot start',[double]$MaxMin=12,[double]$StartTimeout=240,[double]$WaitMax=900)
# Batch H capture helper (in-process PrintWindow, no focus change): mirror ('Headset Window') every $Slow s; switches to every $Fast s for $FastFor s
# after each trigger (new FIRE/GUN decision in $E\$ApLog, or a new explosion/sprite line in step5-producer.log). Game client shots every $GameSlow s
# (every $GameFast s in fast mode). Every trigger and shot is logged with TickCount64 (the log clock) in $E\killcap.log. Stops on $E\stopN, acz gone, MaxMin.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class KC{
[StructLayout(LayoutKind.Sequential)]public struct R{public int L,T,Rr,B;}
[DllImport("user32.dll")]public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
[DllImport("user32.dll")]public static extern bool GetClientRect(IntPtr h,out R r);
[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
}
'@
[void][KC]::SetProcessDPIAware()
# Batch K2 (step113): K1 fired a missile and scored an explosion plan but had no mirror/game shots because this helper never started or died silently.
# Now: it logs 'launched' at once (killcap.log missing = the helper was not run at all), logs every 30 s while waiting, STARTS ANYWAY (reason logged) when the
# steps.txt marker is not seen but the autopilot log was written after the launch, or acz is running and -StartTimeout s passed; exits with 'never started: <reason>'
# after -WaitMax s without acz; logs every capture error (first 5 distinct + a count) and any fatal error, so a missing capture always has a log line explaining it.
New-Item -ItemType Directory -Force $E | Out-Null
$log="$E\killcap.log"; function L($m){ try { "{0} tick={1} {2}" -f (Get-Date -f HH:mm:ss.fff),[Environment]::TickCount64,$m | Add-Content $log } catch {} }
L ("launched pid={0} E={1} mirror={2} pattern='{3}' startTimeout={4}s waitMax={5}s" -f $PID,$E,$MirrorDir,$StartPattern,$StartTimeout,$WaitMax)
trap { L ("FATAL: " + $_.Exception.Message + " at " + $_.InvocationInfo.ScriptLineNumber); break }
New-Item -ItemType Directory -Force $MirrorDir | Out-Null; $GD="$E\gameshots"; New-Item -ItemType Directory -Force $GD | Out-Null
function Cap($h,[string]$out,[switch]$Client){
  $r=New-Object KC+R; if($Client){ [void][KC]::GetClientRect($h,[ref]$r) } else { [void][KC]::GetWindowRect($h,[ref]$r); $r.Rr-=$r.L; $r.B-=$r.T }
  if($r.Rr -le 0 -or $r.B -le 0){ return }
  $b=New-Object System.Drawing.Bitmap $r.Rr,$r.B; $g=[System.Drawing.Graphics]::FromImage($b); $dc=$g.GetHdc(); [void][KC]::PrintWindow($h,$dc,2); $g.ReleaseHdc($dc); $g.Dispose()
  $b.Save($out,[System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose() }
# new lines of a growing file (shared read)
$pos=@{}
function NewLines([string]$f){
  if(-not (Test-Path $f)){ return @() }
  try { $fs=[IO.File]::Open($f,'Open','Read','ReadWrite,Delete') } catch { return @() }
  try { $p=0L; if($pos.ContainsKey($f)){ $p=$pos[$f] }; if($fs.Length -lt $p){ $p=0 }; [void]$fs.Seek($p,'Begin')
        $sr=New-Object IO.StreamReader($fs); $t=$sr.ReadToEnd(); $pos[$f]=$fs.Length } finally { $fs.Dispose() }
  if(-not $t){ return @() }; ,($t -split "`n") }
$prod="$env:LOCALAPPDATA\acz-vr\logs\step5-producer.log"; $ap="$E\$ApLog"; $gbl="$E\gunburst.log"
# Start detection ignores lines from earlier runs: steps.txt lines are stamped HH:mm:ss.fff (runK2 L), so only a matching line stamped at or after
# this helper's launch counts (batch H2: a stale 'autopilot start' line from a pre-seeded steps.txt ended the helper at once). Unstamped lines never count.
$launch=(Get-Date).TimeOfDay
function StartSeen { $f="$E\steps.txt"; if(-not (Test-Path $f)){ return $false }
  try { $ls=[IO.File]::ReadAllLines($f) } catch { return $false }
  foreach($l in $ls){ if($l -match $StartPattern -and $l -match '^(\d\d):(\d\d):(\d\d)\.(\d+)\s'){ $ts=New-TimeSpan -Hours $Matches[1] -Minutes $Matches[2] -Seconds $Matches[3]
      if($ts -ge $launch.Add([TimeSpan]::FromSeconds(-1)) -or ($launch.TotalHours -gt 23.9 -and $ts.TotalHours -lt 0.1)){ return $true } } }
  $false }
$wsw=[Diagnostics.Stopwatch]::StartNew(); $nextWaitLog=30.0; $startWhy='steps marker'; $launchDt=Get-Date
function ApLogFresh { $f="$E\$ApLog"; (Test-Path $f) -and ((Get-Item $f).LastWriteTime -ge $launchDt.AddSeconds(-1)) }
while($true){
  if(Test-Path "$E\stopN"){ L 'stopN seen before the start: exit'; exit 1 }
  if(StartSeen){ break }
  if(ApLogFresh){ $startWhy='autopilot log written after launch (steps marker not seen)'; break }
  $accRun=[bool](Get-Process acz -ea 0)
  if($accRun -and $wsw.Elapsed.TotalSeconds -ge $StartTimeout){ $startWhy="FALLBACK: no marker/autopilot log within $StartTimeout s, acz is running"; break }
  if(-not $accRun -and $wsw.Elapsed.TotalSeconds -ge $WaitMax){ L "never started: acz not running after $WaitMax s (no marker, no autopilot log)"; exit 2 }
  if($wsw.Elapsed.TotalSeconds -ge $nextWaitLog){ $nextWaitLog+=30; L ("waiting for '{0}' in steps.txt / {1}: {2:N0} s, acz={3}" -f $StartPattern,$ApLog,$wsw.Elapsed.TotalSeconds,$accRun) }
  Start-Sleep -Milliseconds 300 }
[void](NewLines $prod); [void](NewLines $ap); [void](NewLines $gbl)   # skip what was written before the flight
L "start ($startWhy) after $([int]$wsw.Elapsed.TotalSeconds) s of waiting"
$errs=@{}; function CapErr([string]$k,$e){ if(-not $errs.ContainsKey($k)){ $errs[$k]=0 }; $errs[$k]++; if($errs[$k] -le 5){ L ("capture error ($k #$($errs[$k])): " + $e.Exception.Message) } }
$sw=[Diagnostics.Stopwatch]::StartNew(); $fastUntil=-1.0; $nextM=0.0; $nextG=0.0; $nm=0; $ng=0; $lastScan=0.0
while($sw.Elapsed.TotalMinutes -lt $MaxMin -and -not (Test-Path "$E\stopN")){
  $t=$sw.Elapsed.TotalSeconds
  if($t -ge $lastScan+0.2){ $lastScan=$t
    foreach($l in (NewLines $ap)){ if($l -match '\b(FIRE|GUN)\b'){ $fastUntil=$t+$FastFor; L ("trigger autopilot: " + $l.Trim().Substring(0,[math]::Min(200,$l.Trim().Length))) } }
    foreach($l in (NewLines $gbl)){ if($l -match 'J DOWN'){ $fastUntil=$t+$FastFor; L ('trigger gunburst: ' + $l.Trim()) } }   # batch M: manual gun bursts (gunburst.ps1) start the fast mirror capture too
    foreach($l in (NewLines $prod)){ if($l -match 'fx-sprite-gpu|sprite-eye-composited|fx-sprite-plan;[^|]*class=explosion|fx-smoke-ring|542a11d7'){ if($t -ge $fastUntil-$FastFor+2){ L ("trigger producer: " + $l.Trim().Substring(0,[math]::Min(220,$l.Trim().Length))) }; $fastUntil=[math]::Max($fastUntil,$t+$FastFor) } } }
  $fast=($t -lt $fastUntil)
  $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1
  if(-not (Get-Process acz -ea 0)){ L 'acz gone'; break }
  if($t -ge $nextM){ $vrw=(Get-Process vrcompositor -ea 0 | ? { $_.MainWindowTitle -eq 'Headset Window' } | select -First 1).MainWindowHandle
    if($vrw){ try { [void][KC]::SetWindowPos($vrw,[IntPtr](-1),0,0,0,0,0x13); $rr=New-Object KC+R; [void][KC]::GetWindowRect($vrw,[ref]$rr); $bm=New-Object System.Drawing.Bitmap ($rr.Rr-$rr.L),($rr.B-$rr.T); $gg=[System.Drawing.Graphics]::FromImage($bm); $gg.CopyFromScreen($rr.L,$rr.T,0,0,$bm.Size); $gg.Dispose(); $bm.Save(("$MirrorDir\m{0:D4}-{1}{2}.png" -f $nm,(Get-Date -f HHmmss.fff),$(if($fast){'-F'}else{''})),[System.Drawing.Imaging.ImageFormat]::Png); $bm.Dispose() } catch { CapErr 'mirror' $_ } } else { if($nm -eq 0 -or ($nm % 60) -eq 0){ L 'no Headset Window yet (mirror not captured)' } }
    if($false){ try { Cap $vrw ("$MirrorDir\m{0:D4}-{1}{2}.png" -f $nm,(Get-Date -f HHmmss.fff),$(if($fast){'-F'}else{''})) } catch {} }; $nm++; $nextM=$t+$(if($fast){$Fast}else{$Slow}) }
  if($p -and $t -ge $nextG){ try { Cap $p.MainWindowHandle ("$GD\g{0:D4}-{1}{2}.png" -f $ng,(Get-Date -f HHmmss.fff),$(if($fast){'-F'}else{''})) -Client } catch { CapErr 'game' $_ }; $ng++; $nextG=$t+$(if($fast){$GameFast}else{$GameSlow}) }
  Start-Sleep -Milliseconds 20
}
L "done mirror=$nm game=$ng errors=" + (($errs.GetEnumerator() | % { "$($_.Key)=$($_.Value)" }) -join ',')
