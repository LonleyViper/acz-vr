# Offline profile-contract tests. They create temporary JSON only, use recorded
# PNG fixtures, and never open ACZ or modify its data.
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$profilePath = Join-Path $PSScriptRoot 'cockpit-profiles\capture-4132-stock-cockpit.json'
$renderer = Join-Path $PSScriptRoot 'render_cockpit_display_concept.ps1'
$temporaryProfiles = [System.Collections.Generic.List[string]]::new()

function Expect-Valid([string]$Profile) {
    $result = & $renderer -Profile $Profile -ValidateOnly 2>&1
    if ($LASTEXITCODE -ne 0 -or $result -notmatch '^VALID:') {
        throw "expected profile '$Profile' to validate; got: $result"
    }
}

function Expect-Rejected($Profile, [string]$Expected) {
    $name = '.test-' + [guid]::NewGuid().ToString('N') + '.json'
    $path = Join-Path (Split-Path -Parent $profilePath) $name
    $relative = "research/acz-arc-mpl/cockpit-profiles/$name"
    $Profile | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
    [void]$temporaryProfiles.Add($path)
    $threw = $false
    try {
        $result = & $renderer -Profile $relative -ValidateOnly 2>&1
        $exitCode = $LASTEXITCODE
    } catch {
        $threw = $true
        $exitCode = 1
        $result = $_.Exception.Message
    }
    if ((-not $threw -and $exitCode -eq 0) -or $result -notmatch [regex]::Escape($Expected)) {
        throw "expected rejection containing '$Expected'; got: $result"
    }
}

try {
    Expect-Valid 'research/acz-arc-mpl/cockpit-profiles/capture-4132-stock-cockpit.json'
    $base = Get-Content -Raw -LiteralPath $profilePath | ConvertFrom-Json -AsHashtable

    $overlap = $base | ConvertTo-Json -Depth 16 | ConvertFrom-Json -AsHashtable
    $overlap.console_displays[0].source_crop = @(800, 600, 100, 100)
    Expect-Rejected $overlap 'overlaps protected canopy region'

    $offscreen = $base | ConvertTo-Json -Depth 16 | ConvertFrom-Json -AsHashtable
    $offscreen.console_displays[0].surface_polygon[0] = @(-1, 0)
    Expect-Rejected $offscreen 'escapes cockpit image'

    $duplicate = $base | ConvertTo-Json -Depth 16 | ConvertFrom-Json -AsHashtable
    $duplicate.console_displays[1].id = $duplicate.console_displays[0].id
    Expect-Rejected $duplicate 'duplicate console display id'

    Write-Output 'PASS: cockpit display profile contract accepts the capture fixture and rejects protected, offscreen, and duplicate-display variants'
} finally {
    foreach ($path in $temporaryProfiles) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}
