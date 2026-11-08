param([string]$d)
$out="$d\sampler.csv"; $stop="$d\stopN"; $g="$d\GAMEOVER"; $n=0
'time,commit_pct,commit_gb,acz_priv_mb,vrserver_priv_mb,vrcompositor_priv_mb,top1,top2,top3' | Set-Content $out
$base=@{}
Get-Process | % { try { $base["$($_.Id)"]=$_.PrivateMemorySize64 } catch {} }
while(-not (Test-Path $stop)){
  try { $cs=(Get-Counter '\Memory\% Committed Bytes In Use','\Memory\Committed Bytes').CounterSamples; $p=$cs[0].CookedValue; $gb=$cs[1].CookedValue/1GB } catch { $p=-1; $gb=-1 }
  $ps=Get-Process -ErrorAction SilentlyContinue
  $a=$ps | ? Name -eq 'acz'
  $f={ param($nm) $x=$ps | ? Name -eq $nm | select -First 1; if($x){[math]::Round($x.PrivateMemorySize64/1MB)}else{0} }
  $top=$ps | % { $b=0; if($base.ContainsKey("$($_.Id)")){$b=$base["$($_.Id)"]}; [pscustomobject]@{N=$_.Name;G=($_.PrivateMemorySize64-$b)} } | sort G -desc | select -First 3 | % { '{0}:{1}' -f $_.N,[math]::Round($_.G/1MB) }
  while($top.Count -lt 3){$top+=''}
  Add-Content $out ('{0},{1:N2},{2:N2},{3},{4},{5},{6},{7},{8}' -f (Get-Date -f HH:mm:ss.fff),$p,$gb,(& $f 'acz'),(& $f 'vrserver'),(& $f 'vrcompositor'),$top[0],$top[1],$top[2])
  if($p -ge 95){$n++}else{$n=0}
  if($a -and ($n -ge 2 -or $p -ge 97)){ Set-Content $g ('{0} {1}' -f (Get-Date -f HH:mm:ss.fff),$p); $a | % { $_.CloseMainWindow() | Out-Null } }
  Start-Sleep -Milliseconds 700
}

