param([string]$Dir,[double]$IntervalSec=20,[double]$MaxMin=15)
# Periodic mirror shots of the null 'Headset Window' (compositor output) into $Dir until $Dir\mirror.stop exists or MaxMin passes.
New-Item -ItemType Directory -Force $Dir | Out-Null
$sw=[Diagnostics.Stopwatch]::StartNew(); $n=0
while(-not (Test-Path "$Dir\mirror.stop") -and $sw.Elapsed.TotalMinutes -lt $MaxMin){
  $f=Join-Path $Dir ("mirror-{0}-n{1:D3}.png" -f (Get-Date -Format 'HHmmss'),$n)
  pwsh -NoProfile -File "$PSScriptRoot\mirror-shot.ps1" -Out $f -TitleMatch 'Headset Window' -Log "$Dir\mirror.log" | Out-Null
  $n++; $t=$sw.Elapsed.TotalSeconds + $IntervalSec; while($sw.Elapsed.TotalSeconds -lt $t -and -not (Test-Path "$Dir\mirror.stop")){ Start-Sleep -Milliseconds 500 }
}
