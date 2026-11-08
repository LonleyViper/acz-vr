param([string]$E)
# Z3 guard (uncap.md section 6): end the run if present-stats shows uncap-active=1 with fps > 63 (game fast-forwarding) twice in a row, or warn= set.
$log="$env:LOCALAPPDATA\acz-vr\logs\ee-camera.log"; $n=0; $seen=0
while(-not (Test-Path "$E\RUN-DONE")){
  Start-Sleep 2
  try { $l=Get-Content $log -Tail 400 -ErrorAction Stop | Select-String 'event=present-stats' } catch { continue }
  if(-not $l -or $l.Count -le $seen){ $seen=[math]::Min($seen,($l | measure).Count); continue }
  $new=$l[$seen..($l.Count-1)]; $seen=$l.Count
  foreach($x in $new){ $s=$x.Line; if($s -match 'fps=([\d.]+)'){ $f=[double]$Matches[1] } else { continue }
    $bad = ($s -match 'uncap-active=1') -and (($f -gt 63) -or ($s -match ';warn=(?!none)[a-z]'))
    if($bad){ $n++ } else { $n=0 }
    if($n -ge 2){ "{0} uncap guard: {1}" -f (Get-Date -f HH:mm:ss),$s.Substring(0,[Math]::Min(250,$s.Length)) | Set-Content "$E\GAMEOVER"; Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; exit } }
}
