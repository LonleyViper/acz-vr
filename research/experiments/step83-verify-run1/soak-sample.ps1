param([string]$Out='D:\acz-evidence-archive\soak\U',[int]$Every=30,[string]$Stop='D:\acz-evidence-archive\soak\U\STOP')
# Batch U soak sampler: every $Every s while acz runs (survives game restarts) record commit %, acz private/WS/handles/threads, GPU dedicated memory (acz pid + adapter),
# C: free, log dir size, error-string counts and the last values of the unbounded-growth counters from the live logs. Stops on $Stop file.
# Safety stop: C: free < 1 GB closes the game (the sampler.ps1 of runMission.ps1 holds the commit 95%x2 / 97% rule).
New-Item -ItemType Directory -Force $Out | Out-Null
$csv="$Out\soak.csv"; $logd=Join-Path $env:LOCALAPPDATA 'acz-vr\logs'
$cols='time,epoch,acz_pid,commit_pct,acz_priv_mb,acz_ws_mb,acz_handles,acz_threads,gpu_acz_mb,gpu_adapter_mb,c_free_gb,logdir_mb,logdir_files,game_frame,xr_frames,wait_frame_ms_max,frame_cpu_ms_max,end_frame_ms_max,xr_period_ms_mean,tracer_frames,tracer_read,tracer_refused,sprite_refused_all,nbox_cells,nbox_rejected,nbox_evicted,nbox_backend_refused,nbox_used,cockpit_total_frames,cockpit_total_failed,cockpit_loads,cockpit_unloads,load_failures,flare_lines,err_leak,err_queue_full,err_device_removed,err_poisoned,err_quarantined,err_oom,err_hr_outofmem,vscreen_lost,vscreen_copy_failed,b3d_blit_failed,b3d_poisoned'
if(-not (Test-Path $csv)){ $cols | Set-Content $csv }
function Tail($f,[int]$kb=400){ try { $fs=[IO.File]::Open($f,'Open','Read','ReadWrite,Delete'); $n=[Math]::Min($fs.Length,$kb*1024); [void]$fs.Seek(-$n,'End'); $b=New-Object byte[] $n; [void]$fs.Read($b,0,$n); $fs.Close(); [Text.Encoding]::ASCII.GetString($b) } catch { '' } }
function Last($t,$pat,$key){ $m=[regex]::Matches($t,$pat+'[^\r\n]*'); if($m.Count -eq 0){ return '' }; $l=$m[$m.Count-1].Value; $k=[regex]::Match($l,'[;|]'+$key+'=([^;\r\n]*)'); if($k.Success){ $k.Groups[1].Value } else { '' } }
while(-not (Test-Path $Stop)){
  $a=Get-Process acz -ea 0 | select -First 1
  if($a){
    try { $a.Refresh() } catch {}
    try { $cp=(Get-Counter '\Memory\% Committed Bytes In Use').CounterSamples[0].CookedValue } catch { $cp=-1 }
    $gpuA=''; $gpuT=''
    try { $s=(Get-Counter '\GPU Process Memory(*)\Dedicated Usage' -ea Stop).CounterSamples; $x=$s | ? { $_.Path -match "pid_$($a.Id)_" } | measure CookedValue -Sum; $gpuA=[math]::Round($x.Sum/1MB) } catch {}
    try { $s=(Get-Counter '\GPU Adapter Memory(*)\Dedicated Usage' -ea Stop).CounterSamples; $gpuT=[math]::Round((($s | measure CookedValue -Maximum).Maximum)/1MB) } catch {}
    $cfree=[math]::Round((Get-PSDrive C).Free/1GB,2)
    $fl=Get-ChildItem $logd -File -Force -ea 0; $lm=[math]::Round((($fl | measure Length -Sum).Sum)/1MB,1)
    $xr=Tail "$logd\xr-stereo.log" 600; $s5=Tail "$logd\step5-producer.log" 600
    # error strings over all log files (rotated included)
    $cnt=@{}; foreach($p in 'leak','queue-full','device-removed','poisoned=[1-9]','quarantined=[1-9]','oom','E_OUTOFMEMORY|8007000e'){ $cnt[$p]=0 }
    # U-soak: NEVER Select-String -Path on live logs. It opens without FILE_SHARE_DELETE (blocks the DLL's rotation) and, before the
    # U-soak DLL fix, made the DLL's non-shareable appends fail so Log::fail() switched diagnostics (and with it the replay) off mid-flight.
    # Read each file once with ReadWrite,Delete sharing (Tail) and count matching lines.
    $flare=0; foreach($f in $fl){ if($f.Extension -eq '.bin'){ continue }; $t=Tail $f.FullName 65536; if(-not $t){ continue }; $ls=$t -split "`n"
      foreach($p in @($cnt.Keys)){ $cnt[$p]+=@($ls -match $p).Count }; $flare+=@($ls -match 'flare').Count }
    $tr=[regex]::Matches($s5+$xr,'event=fx-sprite-totals;class=tracer[^\r\n]*'); $trl=''; if($tr.Count){ $trl=$tr[$tr.Count-1].Value }
    $g=@{}; foreach($k in 'frames','read','refused'){ $m=[regex]::Match($trl,"[;]$k=(\d+)"); $g[$k]=$m.Groups[1].Value }
    $allref=0; $seen=@{}; foreach($m in [regex]::Matches($s5+$xr,'event=fx-sprite-totals;class=([^;]+);[^\r\n]*?refused=(\d+)')){ $seen[$m.Groups[1].Value]=[int64]$m.Groups[2].Value }; foreach($v in $seen.Values){ $allref+=$v }
    $gf=[regex]::Matches($s5,'event=fx-sprite-plan;frame=(\d+)'); $gframe=''; if($gf.Count){ $gframe=$gf[$gf.Count-1].Groups[1].Value }
    $row=@(
      (Get-Date -f HH:mm:ss),[DateTimeOffset]::Now.ToUnixTimeSeconds(),$a.Id,[math]::Round($cp,1),[math]::Round($a.PrivateMemorySize64/1MB),[math]::Round($a.WorkingSet64/1MB),$a.HandleCount,$a.Threads.Count,$gpuA,$gpuT,$cfree,$lm,$fl.Count,
      $gframe,(Last $xr 'event=summary;t-ms' 'frames'),(Last $xr 'event=summary-timing;t-ms' 'wait-frame-ms-max'),(Last $xr 'event=summary-timing;t-ms' 'frame-cpu-ms-max'),(Last $xr 'event=summary-timing;t-ms' 'end-frame-ms-max'),(Last $xr 'event=summary-timing;gpu' 'xr-period-ms-mean'),
      $g['frames'],$g['read'],$g['refused'],$allref,
      (Last $xr 'event=summary-mk360-nbox;t-ms' 'cells'),(Last $xr 'event=summary-mk360-nbox;t-ms' 'rejected'),(Last $xr 'event=summary-mk360-nbox;t-ms' 'evicted'),(Last $xr 'event=summary-mk360-nbox-backend' 'refused'),(Last $xr 'event=summary-mk360-nbox-backend' 'used'),
      (Last $xr 'event=summary-cockpit;t-ms' 'total-frames'),(Last $xr 'event=summary-cockpit;t-ms' 'total-failed'),(Last $xr 'event=summary-cockpit-auto;t-ms' 'loads'),(Last $xr 'event=summary-cockpit-auto;t-ms' 'unloads'),(Last $xr 'event=summary-cockpit-auto;t-ms' 'load-failures'),
      $flare,$cnt['leak'],$cnt['queue-full'],$cnt['device-removed'],$cnt['poisoned=[1-9]'],$cnt['quarantined=[1-9]'],$cnt['oom'],$cnt['E_OUTOFMEMORY|8007000e'],
      (Last $xr 'event=summary-vscreen;t-ms' 'lost'),(Last $xr 'event=summary-vscreen;t-ms' 'copy-failed'),(Last $xr 'event=summary-brief3d-table' 'blit-failed'),(Last $xr 'event=summary-brief3d-table' 'poisoned')
    )
    Add-Content $csv ($row -join ',')
    if($cfree -lt 1){ Add-Content "$Out\soak-events.txt" ("{0} C: free {1} GB < 1: closing game" -f (Get-Date -f HH:mm:ss),$cfree); $a.CloseMainWindow() | Out-Null }
  }
  Start-Sleep $Every
}
