param([string]$F,[string]$Name)
Add-Type -AssemblyName System.Drawing
$RG=[ordered]@{ sky=@(900,60,400,140); groundR=@(1450,650,400,60); mountSlope=@(250,510,300,40) }
if($Name -eq 'game'){ $RG=[ordered]@{ sky=@(900,60,400,140); groundR=@(1450,650,450,70); mountSlope=@(50,380,250,60) } }
$i=[Drawing.Bitmap]::FromFile($F); $s=2000.0/$i.Width; $o=@()
foreach($k in $RG.Keys){ $r=$RG[$k]; $x0=[int]($r[0]/$s); $y0=[int]($r[1]/$s); $w=[int]($r[2]/$s); $h=[int]($r[3]/$s); $sr=0;$sg=0;$sb=0;$n=0
 for($y=$y0;$y -lt $y0+$h;$y+=3){ for($x=$x0;$x -lt $x0+$w;$x+=3){ $p=$i.GetPixel($x,$y); $sr+=$p.R;$sg+=$p.G;$sb+=$p.B;$n++ } }
 $o+=("{0}={1:N0},{2:N0},{3:N0}" -f $k,($sr/$n),($sg/$n),($sb/$n)) }
"{0}: {1}" -f (Split-Path $F -Leaf),($o -join '  '); $i.Dispose()
