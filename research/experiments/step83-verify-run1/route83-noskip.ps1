param([string]$Dir)
$drv='<repo>\tools\live\acz-window-driver.ps1'
$steps=@(@(9,'accelerate'),@(26,'accelerate'),@(31,'accelerate'),@(36,'accelerate'),@(40,'back'),@(44,'back'),@(48,'back'),@(52,'back'),@(56,'accelerate'),@(62,'confirm'),@(69,'confirm'),@(75,'confirm'),@(83,'confirm'))
$sw=[Diagnostics.Stopwatch]::StartNew()
foreach($s in $steps){
  while($sw.Elapsed.TotalSeconds -lt $s[0]){ if(-not (Get-Process acz -ea 0)){ "{0} acz gone, route aborted" -f (Get-Date -f HH:mm:ss) | Add-Content "$Dir\route.txt"; exit 1 }; Start-Sleep -Milliseconds 100 }
  & pwsh -NoProfile -File $drv -Action tap -Key $s[1] -AcknowledgeLiveInput -ScreenshotDirectory "$Dir\shots" *> $null
  "{0} {1} (+{2:N0}s) exit={3}" -f (Get-Date -f HH:mm:ss),$s[1],$s[0],$LASTEXITCODE | Add-Content "$Dir\route.txt"
}
"{0} route done (Esc skip opening sent)" -f (Get-Date -f HH:mm:ss) | Add-Content "$Dir\route.txt"
