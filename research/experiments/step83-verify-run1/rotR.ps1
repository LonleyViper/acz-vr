param([string]$In,[string]$Out)
Add-Type -AssemblyName System.Drawing
$i=[Drawing.Bitmap]::FromFile($In); $i.RotateFlip([Drawing.RotateFlipType]::Rotate180FlipNone); $i.Save($Out,[Drawing.Imaging.ImageFormat]::Png); $i.Dispose()
