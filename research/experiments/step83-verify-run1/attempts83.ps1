param([string]$E,[int]$Max=4,[int]$QuietSec=60,[double]$StartGate=85,[double]$Total=420,[string]$PitchKey='',[double]$PitchLen=0.15,[string]$MissileAt='',[string]$BurstAt='',[string]$TabAt='',[string]$Extra83='',[string]$Route='route77.ps1')
# Retry loop (step79/81/82 pattern): quiet gate (no cl/link for $QuietSec s and commit < $StartGate), launch acz.exe directly (D-279),
# click Launch Game (launcher position verified), route77, V, run83. Closed-by-rule launches (GAMEOVER) are logged and retried.
$log="$env:LOCALAPPDATA\acz-vr\logs\xr-stereo.log"
Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;
public static class WR{
[StructLayout(LayoutKind.Sequential)]public struct RECT{public int L,T,R,B;}
[DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out RECT r);
[DllImport("user32.dll")]public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
}
'@
[void][WR]::SetProcessDPIAware()
function Commit { try { [double](((Get-Content "$E\sampler.csv" -Tail 1) -split ',')[1]) } catch { 0 } }
function L($m){ "{0} {1}" -f (Get-Date -f HH:mm:ss),$m | Add-Content "$E\route.txt" }
function CloseGame { Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; for($k=0;$k -lt 20 -and (Get-Process acz -ea 0);$k++){ Start-Sleep 1 }; Get-Process acz -ea 0 | Stop-Process -Force }
$x83=@{}; if($Extra83){ foreach($kv in $Extra83.Split(';')){ $k,$v=$kv.Split('=',2); $x83[$k]=$v } }
for($i=1;$i -le $Max;$i++){
  Remove-Item "$E\GAMEOVER","$E\NOMISSION" -ea 0
  $q0=[Diagnostics.Stopwatch]::StartNew(); $gw=[Diagnostics.Stopwatch]::StartNew()
  while($q0.Elapsed.TotalSeconds -lt $QuietSec -and $gw.Elapsed.TotalMinutes -lt 45){ $c=Commit; if((Get-Process cl,link,rc,mt -ea 0) -or $c -ge $StartGate -or $c -le 0){ $q0.Restart() }; Start-Sleep -Milliseconds 700 }
  if($q0.Elapsed.TotalSeconds -lt $QuietSec){ L "quiet gate NOT reached in 45 min (commit $(Commit)): abort attempts"; break }
  L "attempt $i start after $([int]$gw.Elapsed.TotalSeconds) s gate (commit $(Commit))"
  Start-Process 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero\acz.exe' -WorkingDirectory 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
  $p=$null; for($k=0;$k -lt 60 -and -not $p;$k++){ Start-Sleep -Milliseconds 500; $p=Get-Process acz -ea 0 | ? {$_.MainWindowTitle -eq 'ACE COMBAT ZERO THE BELKAN WAR'} | select -First 1 }
  if(-not $p){ L 'no launcher window'; CloseGame; continue }
  Start-Sleep 4
  $r=New-Object WR+RECT; [void][WR]::GetWindowRect($p.MainWindowHandle,[ref]$r)
  L ("launcher rect {0},{1},{2},{3}" -f $r.L,$r.T,$r.R,$r.B)
  [void][WR]::SetWindowPos($p.MainWindowHandle,[IntPtr]::new(-1),0,0,0,0,3); [void][WR]::SetForegroundWindow($p.MainWindowHandle); Start-Sleep -Milliseconds 800
  pwsh -NoProfile -File "$PSScriptRoot\shot.ps1" -Out "$E\shots\launcher-att$i.png" | Out-Null
  if([math]::Abs($r.L-880) -gt 12 -or [math]::Abs($r.T-380) -gt 12){ L 'launcher not at the verified position: abort attempts'; CloseGame; break }
  pwsh -NoProfile -File "$PSScriptRoot\shot.ps1" -ClickX 1368 -ClickY 886 | Out-Null
  L "attempt $i click Launch Game (T0)"
  $up=$false; for($k=0;$k -lt 80 -and -not $up;$k++){ Start-Sleep -Milliseconds 500; $pp=Get-Process acz -ea 0 | select -First 1; if($pp -and $pp.PrivateMemorySize64 -gt 600MB){$up=$true} }
  if(-not $up){ pwsh -NoProfile -File "$PSScriptRoot\shot.ps1" -Out "$E\shots\nostart-att$i.png" | Out-Null; L "attempt ${i}: game did not start 40 s after the click"; CloseGame; Start-Sleep 5; continue }
  & "$PSScriptRoot\$Route" $E
  if(Test-Path "$E\GAMEOVER"){ Copy-Item "$E\GAMEOVER" "$E\GAMEOVER-att$i.txt"; L "attempt $i closed by rule during the route: $(Get-Content "$E\GAMEOVER")"; CloseGame; Start-Sleep 5; continue }
  Start-Sleep 12
  if(-not (Get-Process acz -ea 0)){ L "attempt $i game gone before V"; continue }
  & "$PSScriptRoot\run83.ps1" -ShotDir "$E\archive\shots" -LogFile $log -Sampler "$E\sampler.csv" -Total $Total -PitchKey $PitchKey -PitchLen $PitchLen -MissileAt $MissileAt -BurstAt $BurstAt -TabAt $TabAt -ShotEvery 15 @x83 | Tee-Object "$E\run83-console-att$i.txt"
  if(Test-Path "$E\NOMISSION"){ L "attempt ${i}: no source-ready after V (route missed): closing and retrying"; CloseGame; Start-Sleep 8; continue }
  if(Test-Path "$E\GAMEOVER"){ Copy-Item "$E\GAMEOVER" "$E\GAMEOVER-att$i.txt"; L "attempt $i closed by rule in the run: $(Get-Content "$E\GAMEOVER")"; Start-Sleep 5; break }
  if(-not (Get-Process acz -ea 0)){ L "attempt $i game gone in the run"; break }
  L "attempt $i COMPLETED run (game still up)"
  'OK' | Set-Content "$E\ATTEMPT-OK"
  break
}
L 'attempt loop ended'
