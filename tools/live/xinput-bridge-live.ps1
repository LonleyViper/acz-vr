# Guarded deploy/rollback helper for the per-game XInput test bridge.
# Requires current approval for gates 1, 3 and 6. Never launches the game.
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('status', 'install', 'remove')]
    [string]$Action,
    [string]$Proxy = (Join-Path $PSScriptRoot '..\..\build\bin\Release\XINPUT1_4.dll')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$game = 'D:\SteamLibrary\steamapps\common\ACE COMBAT 8\ACZero'
$target = Join-Path $game 'XINPUT1_4.dll'
$baseline = @{
  'acz.exe'='8aa8ffc9a185c58d45b34bc1116d8e1880ea785cc6fa3fa7d6a421fffeb9c739'
  'a.arc'='82c05ed08ca584b551776f916ba06e9bfd21c01e0cfede8afe6ffbd276745f80'
  'b.arc'='4c4a97f5783cc925002d642306d3b25de2fa87e4e10a4f5a82e0cc96ec00d7ff'
  'c.arc'='a84efdbba33046105134111197d1aa7babecb16dc3f15f158f7689dd25299bce'
  'd.arc'='c28e41acdc6ba6699481169e168381234b920191f0189384fca751c04538d0e2'
  'e.arc'='63d8f45fc98cc5fbeca71de8e98727f8662a1e7d6aeb291b305d5503bc6de3d5'
  'steam_api64.dll'='e3fe06e2d802c2f753aeb46869e2af898f34715ce85f08001456fd8a80ecd21e'
}

function Test-Baseline {
  $bad = @()
  foreach ($file in $baseline.Keys) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $game $file)).Hash.ToLower()
    if ($hash -ne $baseline[$file]) { $bad += "$file $hash" }
  }
  if ($bad) { throw "Baseline mismatch: $($bad -join '; ')" }
  'Baseline OK (7 files).'
}

function Assert-GameClosed {
  if (Get-Process acz -ErrorAction SilentlyContinue) {
    throw 'acz.exe is running; close it normally before changing the game directory.'
  }
}

switch ($Action) {
  'status' {
    Test-Baseline
    [pscustomobject]@{
      GameRunning = [bool](Get-Process acz -ErrorAction SilentlyContinue)
      ProxyPresent = Test-Path -LiteralPath $target
      ProxyHash = if (Test-Path -LiteralPath $target) { (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash.ToLower() } else { $null }
      BuildHash = if (Test-Path -LiteralPath $Proxy) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Proxy).Hash.ToLower() } else { $null }
    } | Format-List
  }
  'install' {
    Assert-GameClosed
    Test-Baseline
    if (-not (Test-Path -LiteralPath $Proxy)) { throw "Built XInput proxy is missing: $Proxy" }
    if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite existing $target" }
    Copy-Item -LiteralPath $Proxy -Destination $target
    "Installed XINPUT1_4.dll sha256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash.ToLower())."
  }
  'remove' {
    Assert-GameClosed
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target; 'XInput proxy removed.' } else { 'No XInput proxy present.' }
    Test-Baseline
  }
}
