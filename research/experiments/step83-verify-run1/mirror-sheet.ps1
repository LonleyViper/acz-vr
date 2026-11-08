param([string]$FileList,[string]$Out,[int]$Cols=2,[int]$W=640,[int]$H=360,[switch]$NoRotate,[string]$Crop='')
# Contact sheet of mirror PNGs, each rotated back 180 deg (the null Headset Window shows our layers rotated 180; see notes batch M).
# -Crop 'x,y,w,h' crops each (after rotation) before scaling (e.g. one eye: 0,0,640,720). Labels = file base names.
Add-Type -AssemblyName System.Drawing
$Files=$FileList.Split(";") | ? { $_ }
$rows=[math]::Ceiling($Files.Count/$Cols); $b=New-Object System.Drawing.Bitmap ($W*$Cols),($H*$rows); $g=[System.Drawing.Graphics]::FromImage($b); $g.Clear([System.Drawing.Color]::Black); $k=0
foreach($f in $Files){
  $i=[System.Drawing.Image]::FromFile($f); if(-not $NoRotate){ $i.RotateFlip([System.Drawing.RotateFlipType]::Rotate180FlipNone) }
  $dst=New-Object System.Drawing.Rectangle (($k%$Cols)*$W),([math]::Floor($k/$Cols)*$H),$W,$H
  if($Crop){ $c=$Crop.Split(',') | % { [int]$_ }; $g.DrawImage($i,$dst,(New-Object System.Drawing.Rectangle $c[0],$c[1],$c[2],$c[3]),[System.Drawing.GraphicsUnit]::Pixel) } else { $g.DrawImage($i,$dst) }
  $g.DrawString([IO.Path]::GetFileNameWithoutExtension($f),(New-Object System.Drawing.Font 'Arial',12),[System.Drawing.Brushes]::Yellow,$dst.X+4,$dst.Y+$H-22); $i.Dispose(); $k++ }
$b.Save($Out,[System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $b.Dispose()
