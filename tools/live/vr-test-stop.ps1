# One-shot cleanup after a headset test: closes the game if it is running, removes the proxy DLLs (hash-checked by l1.ps1),
# and puts runtime.cfg back to log_enabled=true. Logs are collected by l1.ps1 into research/experiments/L1-logs.
$ErrorActionPreference = 'Stop'
Get-Process acz -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.CloseMainWindow() }
Start-Sleep 4
Get-Process acz -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'l1.ps1') -Action remove
Set-Content -LiteralPath (Join-Path $env:LOCALAPPDATA 'acz-vr\runtime.cfg') -Value 'log_enabled=true' -Encoding ascii
'runtime.cfg reset to log_enabled=true'
