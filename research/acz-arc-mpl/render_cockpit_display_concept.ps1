# Offline concept render only.  It reads recorded PNGs and writes a derived PNG;
# it neither opens ACZ nor modifies an archive, DLL, or captured game resource.
[CmdletBinding()]
param(
    [string]$Cockpit = 'research/experiments/step14-live-run11-full/png/acz-vr-png-4132-875315921-s2-eye0.png',
    [string]$Hud = 'research/experiments/step14-live-run11-full/png/acz-vr-png-4132-875315921-s2-hud-on-black.png',
    [string]$Profile = 'research/acz-arc-mpl/cockpit-profiles/capture-4132-stock-cockpit.json',
    [string]$Out = 'research/acz-arc-mpl/cockpit-display-concept.png',
    [string]$CanopyHudOut = 'research/acz-arc-mpl/cockpit-canopy-hud-only.png',
    [string]$CalibrationOverlayOut = 'research/acz-arc-mpl/cockpit-console-calibration-overlay.png',
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Copy-Crop([System.Drawing.Graphics] $Graphics, [System.Drawing.Bitmap] $Source,
                   [System.Drawing.Rectangle] $SourceRect, [System.Drawing.Rectangle] $Destination,
                   [System.Drawing.Point[]] $Clip) {
    $state = $Graphics.Save()
    try {
        $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $path.AddPolygon($Clip)
        $Graphics.SetClip($path)
        $Graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $Graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $Graphics.DrawImage($Source, $Destination, $SourceRect.X, $SourceRect.Y, $SourceRect.Width, $SourceRect.Height,
            [System.Drawing.GraphicsUnit]::Pixel)
    } finally {
        $Graphics.Restore($state)
    }
}

function Require($Condition, [string]$Message) {
    if (-not $Condition) { throw "invalid cockpit display profile: $Message" }
}

function To-Rectangle($Values, [string]$Name) {
    Require ($null -ne $Values -and $Values.Count -eq 4) "$Name must be [x,y,width,height]"
    $ints = @($Values | ForEach-Object { [int]$_ })
    Require ($ints[2] -gt 0 -and $ints[3] -gt 0) "$Name must have positive width and height"
    return [System.Drawing.Rectangle]::new($ints[0], $ints[1], $ints[2], $ints[3])
}

function To-Polygon($Values, [string]$Name) {
    Require ($null -ne $Values -and $Values.Count -eq 4) "$Name must contain exactly four points"
    $points = [System.Collections.Generic.List[System.Drawing.Point]]::new()
    foreach ($point in $Values) {
        Require ($null -ne $point -and $point.Count -eq 2) "$Name points must be [x,y]"
        [void]$points.Add([System.Drawing.Point]::new([int]$point[0], [int]$point[1]))
    }
    return $points.ToArray()
}

function Get-Bounds([System.Drawing.Point[]]$Points) {
    $minX = ($Points | Measure-Object -Property X -Minimum).Minimum
    $maxX = ($Points | Measure-Object -Property X -Maximum).Maximum
    $minY = ($Points | Measure-Object -Property Y -Minimum).Minimum
    $maxY = ($Points | Measure-Object -Property Y -Maximum).Maximum
    return [System.Drawing.Rectangle]::FromLTRB([int]$minX, [int]$minY, [int]$maxX, [int]$maxY)
}

function Get-PolygonTwiceArea([System.Drawing.Point[]]$Points) {
    [long]$twiceArea = 0
    for ($index = 0; $index -lt $Points.Length; ++$index) {
        $next = $Points[($index + 1) % $Points.Length]
        $twiceArea += [long]$Points[$index].X * $next.Y - [long]$next.X * $Points[$index].Y
    }
    return [Math]::Abs($twiceArea)
}

function Require-Within([System.Drawing.Rectangle]$Rectangle, [int]$Width, [int]$Height, [string]$Name) {
    Require ($Rectangle.X -ge 0 -and $Rectangle.Y -ge 0 -and $Rectangle.Right -le $Width -and $Rectangle.Bottom -le $Height) "$Name escapes its declared image"
}

function Assert-Profile($Data, [System.Drawing.Bitmap]$CockpitImage, [System.Drawing.Bitmap]$HudImage) {
    Require ($Data.schema -eq 'acz-vr-cockpit-display-profile/v1') 'unsupported schema'
    Require ($Data.id -is [string] -and $Data.id.Length -gt 0) 'missing id'
    Require ($Data.capture.width -eq $CockpitImage.Width -and $Data.capture.height -eq $CockpitImage.Height) 'cockpit image size does not match profile capture'
    Require ($Data.hud_texture.width -eq $HudImage.Width -and $Data.hud_texture.height -eq $HudImage.Height) 'HUD image size does not match profile'
    foreach ($required in @('flight_reticle', 'attitude_ladder', 'targeting_cues')) {
        Require ($Data.canopy_hud.protected -contains $required) "$required must remain on canopy HUD"
    }
    Require ($null -ne $Data.console_displays -and $Data.console_displays.Count -gt 0) 'at least one console display is required'

    $displayIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($display in $Data.console_displays) {
        Require ($display.id -is [string] -and $display.id.Length -gt 0) 'console display is missing an id'
        Require ($displayIds.Add($display.id)) "duplicate console display id '$($display.id)'"
        Require ($display.kind -in @('radar', 'stores_status')) "unsupported console display kind '$($display.kind)'"
        $crop = To-Rectangle $display.source_crop "source crop for $($display.id)"
        Require-Within $crop $HudImage.Width $HudImage.Height "source crop for $($display.id)"
        $surface = To-Polygon $display.surface_polygon "surface polygon for $($display.id)"
        foreach ($point in $surface) {
            Require ($point.X -ge 0 -and $point.X -lt $CockpitImage.Width -and $point.Y -ge 0 -and $point.Y -lt $CockpitImage.Height) "surface polygon for $($display.id) escapes cockpit image"
        }
        Require ((Get-PolygonTwiceArea $surface) -gt 0) "surface polygon for $($display.id) is degenerate"
        if ($null -ne $display.alignment) {
            Require ($null -ne $display.alignment.centreline_x -and $null -ne $display.alignment.centroid_tolerance_px) "alignment for $($display.id) requires centreline_x and centroid_tolerance_px"
            $centroidX = ($surface | Measure-Object -Property X -Average).Average
            Require ([Math]::Abs($centroidX - [double]$display.alignment.centreline_x) -le [double]$display.alignment.centroid_tolerance_px) "surface polygon for $($display.id) is off its declared centreline"
        }
    }

    foreach ($protected in $Data.canopy_hud.protected_regions) {
        Require ($protected.id -is [string] -and $Data.canopy_hud.protected -contains $protected.id) "protected canopy region has an undeclared id '$($protected.id)'"
        $protectedRect = To-Rectangle $protected.rect "protected canopy region $($protected.id)"
        Require-Within $protectedRect $HudImage.Width $HudImage.Height "protected canopy region $($protected.id)"
        foreach ($display in $Data.console_displays) {
            $crop = To-Rectangle $display.source_crop "source crop for $($display.id)"
            if ($crop.IntersectsWith($protectedRect)) {
                throw "invalid cockpit display profile: source crop '$($display.id)' overlaps protected canopy region '$($protected.id)'"
            }
        }
    }
}

$cockpitPath = Join-Path $PSScriptRoot '..\..\' $Cockpit
$hudPath = Join-Path $PSScriptRoot '..\..\' $Hud
$profilePath = Join-Path $PSScriptRoot '..\..\' $Profile
$outPath = Join-Path $PSScriptRoot '..\..\' $Out
$canopyHudOutPath = Join-Path $PSScriptRoot '..\..\' $CanopyHudOut
$calibrationOverlayOutPath = Join-Path $PSScriptRoot '..\..\' $CalibrationOverlayOut
$outDir = Split-Path -Parent $outPath
$canopyHudOutDir = Split-Path -Parent $canopyHudOutPath
$calibrationOverlayOutDir = Split-Path -Parent $calibrationOverlayOutPath
if (-not (Test-Path -LiteralPath $cockpitPath)) { throw "cockpit input not found: $cockpitPath" }
if (-not (Test-Path -LiteralPath $hudPath)) { throw "HUD input not found: $hudPath" }
if (-not (Test-Path -LiteralPath $profilePath)) { throw "profile not found: $profilePath" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
New-Item -ItemType Directory -Force -Path $canopyHudOutDir | Out-Null
New-Item -ItemType Directory -Force -Path $calibrationOverlayOutDir | Out-Null

$cockpitImage = [System.Drawing.Bitmap]::new($cockpitPath)
$hudImage = [System.Drawing.Bitmap]::new($hudPath)
$profileData = Get-Content -Raw -LiteralPath $profilePath | ConvertFrom-Json -AsHashtable
Assert-Profile $profileData $cockpitImage $hudImage

if ($ValidateOnly) {
    Write-Output "VALID: $($profileData.id) ($($profileData.console_displays.Count) console displays; canopy HUD protected)"
    $cockpitImage.Dispose(); $hudImage.Dispose()
    exit 0
}
$output = [System.Drawing.Bitmap]::new($cockpitImage.Width, $cockpitImage.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$canopyHud = [System.Drawing.Bitmap]::new($hudImage)
$graphics = [System.Drawing.Graphics]::FromImage($output)
try {
    $graphics.Clear([System.Drawing.Color]::Black)
    $graphics.DrawImageUnscaled($cockpitImage, 0, 0)

    foreach ($display in $profileData.console_displays) {
        $source = To-Rectangle $display.source_crop "source crop for $($display.id)"
        $surface = To-Polygon $display.surface_polygon "surface polygon for $($display.id)"
        $destination = Get-Bounds $surface
        Copy-Crop $graphics $hudImage $source $destination $surface
    }

    # The crop list is also removed from the canopy texture.  This mirrors the
    # target runtime routing: radar/stores have one console destination, while
    # flight/attitude/targeting remain on a transparent canopy-HUD layer.
    $mask = [System.Drawing.Graphics]::FromImage($canopyHud)
    try {
        $mask.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $transparent = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
        try {
            foreach ($display in $profileData.console_displays) {
                $crop = To-Rectangle $display.source_crop "source crop for $($display.id)"
                $mask.FillRectangle($transparent, $crop)
            }
        } finally {
            $transparent.Dispose()
        }
    } finally {
        $mask.Dispose()
    }
    $canopyHud.Save($canopyHudOutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    # The diagnostic PNG is named hud-on-black and encodes its transparent
    # background as opaque black.  Colour-key black for this offline preview;
    # the runtime must instead preserve the producer's premultiplied alpha.
    $attributes = [System.Drawing.Imaging.ImageAttributes]::new()
    try {
        $attributes.SetColorKey([System.Drawing.Color]::Black, [System.Drawing.Color]::Black)
        $graphics.DrawImage($canopyHud, [System.Drawing.Rectangle]::new(0, 0, $canopyHud.Width, $canopyHud.Height),
            0, 0, $canopyHud.Width, $canopyHud.Height, [System.Drawing.GraphicsUnit]::Pixel, $attributes)
    } finally {
        $attributes.Dispose()
    }

    $font = [System.Drawing.Font]::new('Consolas', 18, [System.Drawing.FontStyle]::Bold)
    $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(235, 180, 255, 160))
    $graphics.DrawString("$($profileData.id) — OFFLINE DISPLAY-PLACEMENT CONCEPT — CALIBRATION REQUIRED", $font, $brush, 30, 28)
    $brush.Dispose(); $font.Dispose()
    $output.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)

    # A content-free image-space calibration reference. These are capture pixels
    # only: they are not model UVs, runtime coordinates, or a stereo solution.
    $overlay = [System.Drawing.Bitmap]::new($cockpitImage.Width, $cockpitImage.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $overlayGraphics = [System.Drawing.Graphics]::FromImage($overlay)
    try {
        $overlayGraphics.DrawImageUnscaled($cockpitImage, 0, 0)
        $font = [System.Drawing.Font]::new('Consolas', 22, [System.Drawing.FontStyle]::Bold)
        try {
            foreach ($display in $profileData.console_displays) {
                $surface = To-Polygon $display.surface_polygon "surface polygon for $($display.id)"
                [System.Drawing.Point[]]$surfacePoints = $surface
                $colour = if ($display.kind -eq 'radar') { [System.Drawing.Color]::FromArgb(255, 80, 230, 255) } else { [System.Drawing.Color]::FromArgb(255, 255, 190, 80) }
                $pen = [System.Drawing.Pen]::new($colour, 6)
                $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(170, $colour))
                try {
                    $overlayGraphics.DrawPolygon($pen, $surfacePoints)
                    $overlayGraphics.DrawString($display.id, $font, $brush, $surface[0].X + 8, $surface[0].Y + 8)
                } finally {
                    $pen.Dispose(); $brush.Dispose()
                }
            }
        } finally {
            $font.Dispose()
        }
        $overlay.Save($calibrationOverlayOutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $overlayGraphics.Dispose(); $overlay.Dispose()
    }
} finally {
    $graphics.Dispose(); $output.Dispose(); $canopyHud.Dispose(); $cockpitImage.Dispose(); $hudImage.Dispose()
}
