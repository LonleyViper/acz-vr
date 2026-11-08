# debrieflib.ps1 (2026-11-01, -Debrief route): window-image classifier shared by runK3ag.ps1 (live) and runBatchAG.ps1 (-Analyze -Kind debrief over archived gameshots).
# Pure image statistics of the GAME WINDOW (title bar included, as in the killcap gameshots, 2405 x 1353 at the 2560 x 1440 output). No taps, no files written.
#   BLACK   mean luma < 14                                (the fades between replay cuts and the screen changes)
#   REPLAY  >= 5 % gold pixels in the top-left "REPLAY" box (the crash replay: "PRESS MENU BUTTON TO EXIT REPLAY"; evidence step174 runAGU1 gameshots g0066..g0115)
#   PAUSE   a gold horizontal rule across the top of the pause panel (y .148..156) AND one highlighted menu row;  row 0..4 = RETURN TO GAME / REPLAY / RETRY / QUIT MISSION / FLIGHT OPTIONS
#           (list read from step124 runJ3 gameshots g0048; the highlight is a gold box whose left edge is the strip x .1175..1235)
#   OTHER   anything else (flight, banners, menus, results, ...)
# Gold = R >= 190, G >= 150, B <= 110 (the HUD/menu text is 245,212,58).
Add-Type -AssemblyName System.Drawing
if(-not ('DbrImg' -as [type])){
Add-Type -TypeDefinition @'
using System;
public static class DbrImg {
  static bool Gold(byte r, byte g, byte b) { return r >= 190 && g >= 150 && b <= 110; }
  // a = BGRA bytes, stride bytes per row. Returns { meanLuma, replayGoldFrac, ruleGoldFrac, row0..row4 (gold pixels on the left-edge strip of each menu row band) }
  public static double[] Stats(byte[] a, int w, int h, int stride) {
    double sum = 0; int n = 0;
    for (int yy = 0; yy < 24; yy++) for (int xx = 0; xx < 40; xx++) { int x = (int)((xx + 0.5) * w / 40), y = (int)((yy + 0.5) * h / 24); int o = y * stride + x * 4; sum += 0.2126 * a[o + 2] + 0.7152 * a[o + 1] + 0.0722 * a[o]; n++; }
    double[] r = new double[8]; r[0] = sum / n;
    int g = 0, t = 0;
    for (int y = (int)(0.070 * h); y < (int)(0.126 * h); y += 2) for (int x = (int)(0.052 * w); x < (int)(0.215 * w); x += 2) { int o = y * stride + x * 4; if (Gold(a[o + 2], a[o + 1], a[o])) g++; t++; }
    r[1] = t > 0 ? (double)g / t : 0;
    double best = 0;   // the pause panel's top rule: the best single pixel row in y .145..160 that is gold along most of x .11..0.90
    for (int y = (int)(0.145 * h); y < (int)(0.160 * h); y += 1) { g = 0; t = 0; for (int x = (int)(0.11 * w); x < (int)(0.90 * w); x += 4) { int o = y * stride + x * 4; if (Gold(a[o + 2], a[o + 1], a[o])) g++; t++; } double f = t > 0 ? (double)g / t : 0; if (f > best) best = f; }
    r[2] = best;
    double[] cy = { 0.236, 0.291, 0.347, 0.403, 0.459 };
    for (int k = 0; k < 5; k++) { int c = 0; for (int y = (int)((cy[k] - 0.026) * h); y < (int)((cy[k] + 0.026) * h); y += 1) for (int x = (int)(0.1175 * w); x < (int)(0.1235 * w); x += 1) { int o = y * stride + x * 4; if (Gold(a[o + 2], a[o + 1], a[o])) c++; } r[3 + k] = c; }
    return r;
  }
}
'@
}
function DbrBytes([Drawing.Bitmap]$bm){ $rc=New-Object Drawing.Rectangle 0,0,$bm.Width,$bm.Height; $d=$bm.LockBits($rc,[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
  try { $buf=New-Object byte[] ($d.Stride*$bm.Height); [Runtime.InteropServices.Marshal]::Copy($d.Scan0,$buf,0,$buf.Length); ,@($buf,$d.Stride) } finally { $bm.UnlockBits($d) } }
# class of a bitmap: @{cls='BLACK|REPLAY|PAUSE|OTHER'; row=-1..4; luma; replay; rule}
function DbrClassify([Drawing.Bitmap]$bm){
  $bs=DbrBytes $bm; $s=[DbrImg]::Stats($bs[0],$bm.Width,$bm.Height,$bs[1])
  $row=-1; $mx=0; for($k=0;$k -lt 5;$k++){ if($s[3+$k] -gt $mx){ $mx=$s[3+$k]; $row=$k } }
  $cls='OTHER'
  if($s[0] -lt 14){ $cls='BLACK' }   # the title bar (white) lifts an all-black window to ~10
  elseif($s[1] -ge 0.05){ $cls='REPLAY' }
  elseif($s[2] -ge 0.60 -and $mx -ge 40){ $cls='PAUSE' }
  else { $row=-1 }
  [pscustomobject]@{cls=$cls;row=$row;luma=[math]::Round($s[0],1);replay=[math]::Round($s[1],3);rule=[math]::Round($s[2],3);rowpx=$mx} }
function DbrClassifyFile([string]$p){ $bm=[Drawing.Bitmap]::FromFile($p); try { DbrClassify $bm } finally { $bm.Dispose() } }
