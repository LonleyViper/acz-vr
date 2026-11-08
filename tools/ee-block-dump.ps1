# ee-block-dump.ps1 - READ-ONLY snapshot of the PC port's 64 MiB emulated-memory block(s) during a flight, for offline
# signature searches (docs/research/ac0vr-lessons.md lead 1: unit table, 0x4A0 stride, position +0x260).
# Opens acz.exe with PROCESS_QUERY_INFORMATION | PROCESS_VM_READ only. Never writes to the game.
# A block is "live" when the E6 aircraft position (+0xFDD42C, 3 floats) changes between two reads 1 s apart.
# Waits -Settle s after the first live read, then dumps every live block at each offset in -At (s after the settle).
param([string]$Out='D:\acz-evidence-archive\step187-units', [int]$Settle=45, [int[]]$At=@(0,1,5,30),
      [int]$WaitMax=900, [string]$Root='')
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$log=Join-Path $Out 'dump.log'
function L($m){ "$(Get-Date -f HH:mm:ss.fff) $m" | Tee-Object -FilePath $log -Append }
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class EeRead {
  [StructLayout(LayoutKind.Sequential)] public struct MBI { public IntPtr Base; public IntPtr AllocBase; public uint AllocProt; public uint pad0;
    public IntPtr Size; public uint State; public uint Protect; public uint Type; public uint pad1; }
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool i, int pid);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr VirtualQueryEx(IntPtr h, IntPtr a, out MBI m, IntPtr l);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr r);
  public static List<long> Blocks(IntPtr h) {
    var r = new List<long>(); long a = 0; MBI m;
    while (VirtualQueryEx(h, new IntPtr(a), out m, new IntPtr(Marshal.SizeOf(typeof(MBI)))) != IntPtr.Zero) {
      long s = m.Size.ToInt64();
      if (m.State == 0x1000 && m.Type == 0x20000 && m.Protect == 0x04 && s == 64L << 20) r.Add(m.Base.ToInt64());
      a = m.Base.ToInt64() + s; if (s <= 0) break; }
    return r; }
  public static byte[] Read(IntPtr h, long a, int n) { var b = new byte[n]; IntPtr got;
    if (!ReadProcessMemory(h, new IntPtr(a), b, new IntPtr(n), out got) || got.ToInt64() != n) return null; return b; }
}
'@
$sw=[Diagnostics.Stopwatch]::StartNew()
$p=$null; while(-not $p -and $sw.Elapsed.TotalSeconds -lt $WaitMax){ $p=Get-Process acz -ea 0 | select -First 1; if(-not $p){ Start-Sleep 2 } }
if(-not $p){ L 'no acz.exe'; exit 2 }
$h=[EeRead]::OpenProcess(0x0410,$false,$p.Id); if($h -eq [IntPtr]::Zero){ L "OpenProcess failed pid=$($p.Id)"; exit 2 }
L "pid=$($p.Id) opened read-only"
function Pos($b){ $x=[EeRead]::Read($h,$b+0xFDD42C,12); if(-not $x){ return $null }; ,@([BitConverter]::ToSingle($x,0),[BitConverter]::ToSingle($x,4),[BitConverter]::ToSingle($x,8)) }
$live=@()
while(-not $live -and $sw.Elapsed.TotalSeconds -lt $WaitMax){
  if(-not (Get-Process -Id $p.Id -ea 0)){ L 'acz gone'; exit 4 }
  $bl=[EeRead]::Blocks($h); $p0=@{}; foreach($b in $bl){ $p0[$b]=Pos $b }
  Start-Sleep 1
  foreach($b in $bl){ $q=Pos $b; if($p0[$b] -and $q -and ($q -join ',') -ne ($p0[$b] -join ',') -and [math]::Abs($q[0])+[math]::Abs($q[2]) -gt 1){ $live+=$b } }
  if(-not $live -and $Root -and (Get-ChildItem $Root -Recurse -Filter 'autopilot-1.log' -ea 0)){
    # the fixed E6 offset may not be relative to the region start (effective base): the flight has started, take all blocks
    L "autopilot log seen, no fixed-offset motion: dumping all $($bl.Count) blocks"; $live=$bl; $allBlocks=$true }
  if(-not $live -and [int]$sw.Elapsed.TotalSeconds % 30 -lt 2){ L "waiting: blocks=$($bl.Count)" }
}
if(-not $live){ L 'no live block'; exit 3 }
L ("live blocks: {0}" -f (($live|%{'0x{0:X}' -f $_}) -join ' '))
Start-Sleep $Settle
$t0=$sw.Elapsed.TotalSeconds
foreach($t in $At){
  while($sw.Elapsed.TotalSeconds -lt $t0+$t){ Start-Sleep -Milliseconds 50 }
  foreach($b in $live){
    $buf=[EeRead]::Read($h,$b,64MB); if(-not $buf){ L ("read failed block 0x{0:X} t={1}" -f $b,$t); continue }
    $f=Join-Path $Out ("blk{0:X}-t{1:000}.bin" -f $b,$t); [IO.File]::WriteAllBytes($f,$buf)
    $ap=@(0..2|%{[BitConverter]::ToSingle($buf,0xFDD42C+4*$_)}); $cp=@(0..2|%{[BitConverter]::ToSingle($buf,0xFDD860+4*$_)})
    L ("dump t={0} block=0x{1:X} air=({2}) cam=({3}) hdg={4}" -f $t,$b,($ap -join ','),($cp -join ','),[BitConverter]::ToSingle($buf,0xFDD448))
  }
}
[void][EeRead]::CloseHandle($h); L 'done'
