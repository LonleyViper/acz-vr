param([int]$Max=4,[string]$AP='-Mode Kill -Total 420 -TargetColors green,yellow')
# Batch L: up to $Max kill attempts (runK2.ps1 menu route + autopilot Kill), each with sampler + 1 s mirror loop; stops after the first attempt with an explosion.
$X=$PSScriptRoot; $A='D:\acz-evidence-archive\step104l'
for($i=1;$i -le $Max;$i++){
  $n="L$i"; $E="$X\run$n"; New-Item -ItemType Directory -Force "$E\shots" | Out-Null
  Copy-Item "$X\cfg-L.cfg" "$env:LOCALAPPDATA\acz-vr\runtime.cfg" -Force
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force -ea 0 | % { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ea 0 }
  Remove-Item "$env:LOCALAPPDATA\acz-vr\png-burst.now" -ea 0
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\sampler.ps1`"","`"$E`"" -WindowStyle Hidden
  Start-Process pwsh -ArgumentList '-NoProfile','-File',"`"$X\mirror-loop.ps1`"",'-Dir',"$A\$n-mirror",'-IntervalSec','1','-MaxMin','12' -WindowStyle Hidden
  Start-Sleep 3
  & pwsh -NoProfile -File "$X\runK2.ps1" -E $E -AP $AP
  Get-Process acz -ea 0 | % { $_.CloseMainWindow() | Out-Null }; Start-Sleep 6; Get-Process acz -ea 0 | Stop-Process -Force
  New-Item -ItemType File "$E\stopN" -Force | Out-Null; New-Item -ItemType File "$A\$n-mirror\mirror.stop" -Force | Out-Null; Start-Sleep 3
  New-Item -ItemType Directory -Force "$A\run$n-logs" | Out-Null
  Get-ChildItem "$env:LOCALAPPDATA\acz-vr\logs" -Force | % { Move-Item -LiteralPath $_.FullName -Destination "$A\run$n-logs" -Force }
  Move-Item "$E\shots" "$A\run$n-shots"; New-Item -ItemType Junction -Path "$E\shots" -Target "$A\run$n-shots" | Out-Null
  $p="$A\run$n-logs\step5-producer.log"; $ex=0; if(Test-Path $p){ $ex=(Select-String -Path $p -Pattern 'class=explosion' -ea 0).Count }
  $fires=(Select-String -Path "$E\autopilot-1.log" -Pattern ' FIRE' -ea 0).Count
  "{0} attempt {1}: explosion-plan lines {2}, FIRE decisions {3}" -f (Get-Date -f HH:mm:ss),$n,$ex,$fires | Add-Content "$X\runLbatch.log"
  if($ex -gt 0){ break }
}
