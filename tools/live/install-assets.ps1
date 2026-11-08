# install-assets.ps1 -- copy the offline-baked render assets into the runtime folders the proxy reads (COCKPIT-2 / terrain).
#   <Source>\<StageDir>\*.zvrs, *.zvro, fingerprints.json -> %LOCALAPPDATA%\acz-vr\stage   (stage_baker.py / objects_baker.py / bake_all_stages.py;
#                           fingerprints.json = stage_fingerprints.py, for xr_terrain_stage=auto). -StageDir stage-all = all 39 stages (default 'stage').
#   <Source>\models\*.zvrm  -> %LOCALAPPDATA%\acz-vr\models   (research/world-render-data/model_baker.py; -Props also copies models\props_*\*.zvrm)
# Game-derived files never go into git: they stay under D:\acz-extract (the default source) and the per-user LOCALAPPDATA folder.
# Read-only on the source; only the two destination folders are written; an unchanged file (same SHA-256) is skipped; a file without the
# expected magic ('ZVRS' / 'ZVRM') is refused (not copied). No network, no game launch.
#   pwsh -NoProfile -File tools/live/install-assets.ps1 [-Source D:\acz-extract] [-StageDir stage-all] [-ModelDir models-c4] [-Props] [-DryRun]
param(
  [string]$Source = 'D:\acz-extract',
  [string]$StageDir = 'stage',   # STAGES (docs/world-render/stages-and-detection.md): subfolder of -Source holding the baked stages
  [string]$ModelDir = 'models',  # COCKPIT-4: subfolder of -Source holding the baked aircraft models (models-c4 = the zvrm flag-8 bakes with fitted hinges;
                                 # those need a COCKPIT-4 DLL: an older loader refuses the unknown header flag and keeps the replayed cockpit)
  [switch]$Props,     # also copy the stage prop models (models\props_<stage>\*.zvrm) into models\props_<stage>
  [switch]$AllBakes,  # AIRCRAFT-AUTO: with -ModelDir models-all also copy the <id>_cockpit / <id>_exterior research bakes (default: only the runtime *_vrcockpit.zvrm)
  [switch]$DryRun     # list what would be copied, write nothing
)
# AIRCRAFT-AUTO (docs/world-render/aircraft-auto.md): -ModelDir models-all = every player aircraft baked by research/world-render-data/bake_all_aircraft.py
# (<type><skin>_{cockpit,exterior,vrcockpit}.zvrm); the runtime (xr_cockpit_model=auto or =<id>) reads only *_vrcockpit.zvrm, so only those are copied
# unless -AllBakes. Afterwards the 36 table entries (research/world-render-data/aircraft_cockpits.json) are checked against the destination.
$modelFilter = if ($ModelDir -eq 'models-all' -and -not $AllBakes) { '*_vrcockpit.zvrm' } else { '*.zvrm' }
$ErrorActionPreference = 'Stop'
if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set' }
$root = Join-Path $env:LOCALAPPDATA 'acz-vr'

function Test-Magic([string]$path, [string]$magic) {
  $fs = [System.IO.File]::OpenRead($path)
  try { $len = $magic.Length; $b = New-Object byte[] $len; $n = $fs.Read($b, 0, $len); return ($n -eq $len) -and ([System.Text.Encoding]::ASCII.GetString($b) -eq $magic) }
  finally { $fs.Dispose() }
}
function Copy-Set([string]$from, [string]$filter, [string]$to, [string]$magic) {
  $out = @()
  if (-not (Test-Path -LiteralPath $from)) { Write-Warning "source folder not found: $from"; return $out }
  $files = @(Get-ChildItem -LiteralPath $from -Filter $filter -File)
  if (-not $files.Count) { Write-Warning "no $filter files in $from"; return $out }
  if (-not $DryRun -and -not (Test-Path -LiteralPath $to)) { New-Item -ItemType Directory -Path $to | Out-Null }
  foreach ($f in $files) {
    $dst = Join-Path $to $f.Name
    if (-not (Test-Magic $f.FullName $magic)) { Write-Warning "refused (no $magic magic): $($f.FullName)"; $out += [pscustomobject]@{ File = $f.Name; Action = 'refused-magic'; Bytes = $f.Length; Sha256 = '' }; continue }
    $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $same = (Test-Path -LiteralPath $dst) -and ((Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLowerInvariant() -eq $h)
    $action = if ($same) { 'unchanged' } elseif ($DryRun) { 'would-copy' } else { 'copied' }
    if (-not $same -and -not $DryRun) { Copy-Item -LiteralPath $f.FullName -Destination $dst -Force }
    $out += [pscustomobject]@{ File = $f.Name; Action = $action; Bytes = $f.Length; Sha256 = $h.Substring(0, 16) }
  }
  return $out
}

$rows = @()
$rows += Copy-Set (Join-Path $Source $StageDir) '*.zvrs' (Join-Path $root 'stage') 'ZVRS'
$rows += Copy-Set (Join-Path $Source $StageDir) '*.zvro' (Join-Path $root 'stage') 'ZVRO'
$fpDir = Join-Path $Source $StageDir   # STAGES: the auto-detection table (optional; its 'magic' is the JSON format line)
if (Test-Path -LiteralPath (Join-Path $fpDir 'fingerprints.json')) { $rows += Copy-Set $fpDir 'fingerprints.json' (Join-Path $root 'stage') ("{`n" + '"format": "acz-stage-fingerprints"') }
$rows += Copy-Set (Join-Path $Source $ModelDir) $modelFilter (Join-Path $root 'models') 'ZVRM'
if ($Props) {
  foreach ($d in @(Get-ChildItem -LiteralPath (Join-Path $Source 'models') -Directory -Filter 'props_*' -ErrorAction SilentlyContinue)) {
    $rows += Copy-Set $d.FullName '*.zvrm' (Join-Path (Join-Path $root 'models') $d.Name) 'ZVRM'
  }
}
$rows | Format-Table -AutoSize | Out-String -Width 160
"destination: $root$(if ($DryRun) { ' (dry run: nothing written)' })"
'cockpit model: config key xr_cockpit_model=<file stem>[;opt:val] (e.g. j35jk_vrcockpit), or tools/live/vr-test-start.ps1 -CockpitModel j35jk_vrcockpit'
# AIRCRAFT-AUTO: which of the 36 player aircraft can xr_cockpit_model=auto draw (a missing file = the game's replayed cockpit for that type, logged)
$table = Join-Path $PSScriptRoot '..\..\research\world-render-data\aircraft_cockpits.json'
if (Test-Path -LiteralPath $table) {
  $ids = @((Get-Content -LiteralPath $table -Raw | ConvertFrom-Json).aircraft | ForEach-Object { $_.model_id })
  $have = @($ids | Where-Object { Test-Path -LiteralPath (Join-Path (Join-Path $root 'models') ($_ + '.zvrm')) })
  $src = @($ids | Where-Object { Test-Path -LiteralPath (Join-Path (Join-Path $Source $ModelDir) ($_ + '.zvrm')) })
  $missing = @($ids | Where-Object { $have -notcontains $_ })
  "xr_cockpit_model=auto: $($have.Count)/$($ids.Count) player-aircraft models installed$(if ($DryRun) { " ($($src.Count) available in $ModelDir)" })" + $(if ($missing.Count) { "; missing: $($missing -join ', ')" } else { '' })
}
