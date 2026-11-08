# mem-guard: commit-charge diagnosis and strict orphan cleanup (see docs/mem-guard.md).
#   pwsh -NoProfile -File tools/mem-guard.ps1 [-Report]          read-only table + verdict (default)
#   pwsh -NoProfile -File tools/mem-guard.ps1 -Apply             kill ONLY class (A) orphans (strict allowlist)
#   pwsh -NoProfile -File tools/mem-guard.ps1 -Watch 30          poll every 30 s and warn
#   pwsh -NoProfile -File tools/mem-guard.ps1 -SelfTest          classification tests on fake process lists
# Exit: 0 ok (<80%), 1 warn (>=80%), 2 fail (>=88%). Self-test: 0 pass / 1 fail.
# Classes: A = provably ours/agent-spawned orphan (the only thing -Apply kills), B = the user's
# applications (never touched), C = system. Imported guidance: the commit % is committed bytes /
# commit limit (RAM + pagefile), NOT RAM use.
param([switch]$Report,
      [switch]$Apply,
      [ValidateRange(0, 86400)][int]$Watch = 0,
      [ValidateRange(0, 1000000)][int]$WatchCount = 0,   # 0 = until Ctrl-C (tests use a small count)
      [switch]$SelfTest,
      [ValidateRange(1, 720)][double]$StaleHours = 12,   # N: minimum age of an orphaned interpreter/build tool
      [int]$HandleLimit = 100000,                        # find/rg/fd above this are runaway scans
      [double]$WarnPercent = 80,
      [double]$FailPercent = 88,
      [double]$LiveRunDelta = 15,                        # projected extra commit of null SteamVR + game
      [string]$LogPath)
$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
if (-not $LogPath) { $LogPath = Join-Path $repoRoot '.orchestration\mem-guard.log' }

# ---- pure classification (tested by -SelfTest) ----
$script:NeverKill = '^(claude|chatgpt|codex|steam.*|vr.*|acz|aceCombat8|explorer|dwm|csrss|winlogon|lsass|services|svchost|system|registry|memory compression|windowsterminal|openconsole|conhost|sihost|taskhostw|msedge.*|chrome.*|firefox.*|brave.*|opera.*|qbittorrent|jellyfin|sonarr|radarr|tailscale.*|msmpeng|voicemeeter.*|beyondhid)$'
$script:ProtectedAncestor = '^(chatgpt|codex.*|steam.*|windowsterminal|explorer|services|svchost|sihost|winlogon|wininit|jellyfin|sonarr|radarr|qbittorrent|cmd)$'
$script:ScanTools = '^(find|rg|fd)$'
$script:BuildTools = '^(cmake|ninja|cl|link)$'
$script:Interpreters = '^(python|python3|node|pwsh|powershell|bash|sh)$'
$script:OwnEvidence = 'PS2 VR Support|[\\/]\.claude[\\/]|shell-snapshots|\.orchestration|acz-vr|jobboard|tools[\\/]live|aczarc|orchestrat'
$script:KeepAlive = 'github-watch|jobboard\.mjs|mem-guard'      # intentional long-lived helpers of this project

# $P: array of hashtables {Id, Name, ParentId, ParentStart, Start, CmdLine, PrivMB, WSMB, Handles, HasWindow, Session}
# $Self: pid of the current process. Returns one row per process with Class (A/B/C) and Reason.
function Get-MemGuardClassification([object[]]$P, [int]$Self, [double]$StaleHours, [int]$HandleLimit, [datetime]$Now) {
    $by = @{}; foreach ($x in $P) { $by[[int]$x.Id] = $x }
    # current process tree = self + ancestors + all descendants
    $tree = @{}
    $i = $Self; $guard = 0
    while ($i -and $by.ContainsKey([int]$i) -and $guard++ -lt 40 -and -not $tree.ContainsKey([int]$i)) { $tree[[int]$i] = $true; $i = $by[[int]$i].ParentId }
    $tree[[int]$Self] = $true
    $desc = @{ [int]$Self = $true }     # descendants of self only (not of ancestors)
    $grew = $true
    while ($grew) { $grew = $false; foreach ($x in $P) { if (-not $desc.ContainsKey([int]$x.Id) -and $desc.ContainsKey([int]$x.ParentId)) { $desc[[int]$x.Id] = $true; $tree[[int]$x.Id] = $true; $grew = $true } } }
    $rows = foreach ($x in $P) {
        $name = ($x.Name -replace '\.exe$', '')
        $lname = $name.ToLowerInvariant()
        $age = if ($x.Start) { ($Now - $x.Start).TotalHours } else { 0 }
        # ancestor walk: orphan = chain ends at a missing/recycled parent; live claude / protected ancestor = not ours to kill
        $liveClaude = $false; $protAnc = $null; $orphan = $false
        $cur = $x; $depth = 0
        while ($true) {
            $pp = $by[[int]$cur.ParentId]
            if (-not $pp -or ($pp.Start -and $cur.Start -and $pp.Start -gt $cur.Start)) { $orphan = $true; break }
            if ($pp.Name -replace '\.exe$', '' -match '^claude$') { $liveClaude = $true; break }
            if (($pp.Name -replace '\.exe$', '') -match $script:ProtectedAncestor) { $protAnc = $pp.Name; break }
            if (++$depth -gt 40) { break }
            $cur = $pp
        }
        $class = 'B'; $reason = 'user application / unknown owner'
        $cmd = [string]$x.CmdLine
        if ($x.Session -eq 0 -or $lname -match '^(system|registry|memory compression|svchost|services|lsass|csrss|wininit|winlogon|smss|dwm|fontdrvhost|nvdisplay\.container|msmpeng|tailscaled)$') { $class = 'C'; $reason = 'system' }
        $kill = $false
        if ($tree.ContainsKey([int]$x.Id)) { $reason = 'current process tree' }
        elseif ($lname -match $script:NeverKill) { if ($class -ne 'C') { $reason = 'protected name' } }
        elseif ($x.HasWindow) { $reason = 'has a visible window' }
        elseif ($cmd -match $script:KeepAlive) { $reason = 'intentional project helper' }
        elseif ($x.Session -eq 0) { }
        elseif ($lname -match $script:ScanTools) {
            if ($x.Handles -gt $HandleLimit) { $kill = $true; $reason = "runaway scan: $($x.Handles) handles > $HandleLimit" }
            elseif ($orphan -and $age -gt $StaleHours) { $kill = $true; $reason = "orphaned scan, age {0:N1} h" -f $age }
            else { $reason = 'scan tool, not runaway' }
        }
        elseif ($lname -match $script:BuildTools -or $lname -match $script:Interpreters) {
            $isBuild = $lname -match $script:BuildTools
            if ($age -le $StaleHours) { $reason = 'younger than StaleHours' }
            elseif ($liveClaude) { $reason = 'live claude ancestor' }
            elseif ($protAnc) { $reason = "started by protected ancestor $protAnc" }
            elseif (-not $orphan) { $reason = 'parent chain unresolved' }
            elseif (-not $isBuild -and $cmd -notmatch $script:OwnEvidence) { $reason = 'orphan but no evidence it is ours' }
            else { $kill = $true; $reason = "orphan, age {0:N1} h, no live claude chain" -f $age }
        }
        if ($kill) { $class = 'A' }
        [pscustomobject]@{ Id = $x.Id; Name = $name; Class = $class; Reason = $reason; AgeH = [math]::Round($age, 1); PrivMB = $x.PrivMB; WSMB = $x.WSMB; Handles = $x.Handles; Parent = $x.ParentId; Orphan = $orphan }
    }
    return , @($rows)
}

function Get-MemGuardVerdict([double]$Percent, [double]$Warn, [double]$Fail) {
    if ($Percent -ge $Fail) { return 2 } elseif ($Percent -ge $Warn) { return 1 } else { return 0 }
}

if ($SelfTest) {
    $now = [datetime]'2026-10-26T20:00:00'
    function P($id, $name, $par, $ageH, [hashtable]$o = @{}) {
        $h = @{ Id = $id; Name = $name; ParentId = $par; Start = $now.AddHours(-$ageH); CmdLine = ''; PrivMB = 50; WSMB = 20; Handles = 200; HasWindow = $false; Session = 1 }
        foreach ($k in $o.Keys) { $h[$k] = $o[$k] }
        $h
    }
    $base = @(
        (P 1 'explorer.exe' 0 100), (P 2 'claude.exe' 1 50 @{ CmdLine = 'claude.exe' }), (P 3 'ChatGPT.exe' 1 50),
        (P 4 'pwsh.exe' 2 1 @{ CmdLine = 'pwsh mem-guard.ps1' }),        # SELF, child of live claude
        (P 5 'svchost.exe' 0 100 @{ Session = 0 })
    )
    $cases = @(
        @{ n = 'runaway find under live claude'; add = @((P 10 'find.exe' 2 1 @{ Handles = 200000 })); kill = @(10) },
        @{ n = 'ordinary fresh find'; add = @((P 11 'find.exe' 2 1)); kill = @() },
        @{ n = 'rg 150k handles'; add = @((P 12 'rg.exe' 99 1 @{ Handles = 150000 })); kill = @(12) },
        @{ n = 'old orphan find'; add = @((P 13 'find.exe' 99 30)); kill = @(13) },
        @{ n = 'old orphan python with evidence'; add = @((P 14 'python.exe' 99 30 @{ CmdLine = 'python <repo>\tools\x.py' })); kill = @(14) },
        @{ n = 'old orphan python no evidence'; add = @((P 15 'python.exe' 99 30)); kill = @() },
        @{ n = 'old python under live claude'; add = @((P 16 'python.exe' 2 30 @{ CmdLine = 'PS2 VR Support' })); kill = @() },
        @{ n = 'old node under ChatGPT'; add = @((P 17 'node.exe' 3 30 @{ CmdLine = 'PS2 VR Support' })); kill = @() },
        @{ n = 'old node under cmd/services'; add = @((P 18 'cmd.exe' 5 30), (P 19 'node.exe' 18 30 @{ CmdLine = 'PS2 VR Support' })); kill = @() },
        @{ n = 'orphan python tree (parent also orphan)'; add = @((P 20 'python.exe' 99 30 @{ CmdLine = 'C:\x\.claude\y' }), (P 21 'python.exe' 20 30 @{ CmdLine = 'C:\x\.claude\y' })); kill = @(20, 21) },
        @{ n = 'pid reuse counts as orphan'; add = @((P 22 'bash.exe' 23 30 @{ CmdLine = 'shell-snapshots' }), (P 23 'sh.exe' 1 0.1)); kill = @(22) },
        @{ n = 'young orphan'; add = @((P 24 'node.exe' 99 2 @{ CmdLine = 'PS2 VR Support' })); kill = @() },
        @{ n = 'visible window'; add = @((P 25 'pwsh.exe' 99 30 @{ CmdLine = 'PS2 VR Support'; HasWindow = $true })); kill = @() },
        @{ n = 'project helper github-watch'; add = @((P 26 'pwsh.exe' 99 30 @{ CmdLine = 'github-watch.ps1 run PS2 VR Support' })); kill = @() },
        @{ n = 'jobboard MCP server'; add = @((P 27 'node.exe' 99 30 @{ CmdLine = 'jobboard.mjs serve' })); kill = @() },
        @{ n = 'orphan cmake'; add = @((P 28 'cmake.exe' 99 30)); kill = @(28) },
        @{ n = 'protected names never die'; add = @((P 29 'acz.exe' 99 99 @{ Handles = 900000 }), (P 30 'vrserver.exe' 99 99 @{ Handles = 900000 }), (P 31 'steamwebhelper.exe' 99 99 @{ Handles = 900000 }), (P 32 'firefox.exe' 99 99), (P 33 'dwm.exe' 99 99), (P 34 'claude.exe' 99 99 @{ Handles = 900000 })); kill = @() },
        @{ n = 'find with window never dies'; add = @((P 35 'find.exe' 99 99 @{ Handles = 900000; HasWindow = $true })); kill = @() },
        @{ n = 'system session find'; add = @((P 36 'find.exe' 99 99 @{ Handles = 900000; Session = 0 })); kill = @() },
        @{ n = 'current tree child'; add = @((P 37 'find.exe' 4 5 @{ Handles = 900000 }), (P 38 'python.exe' 37 99 @{ CmdLine = 'C:\x\.claude\y' })); kill = @() },
        @{ n = 'unreadable cmdline interpreter'; add = @((P 39 'pwsh.exe' 99 53 @{ CmdLine = $null })); kill = @() }
    )
    $errors = @()
    foreach ($c in $cases) {
        $list = @($base) + @($c.add)
        $rows = Get-MemGuardClassification $list 4 12 100000 $now
        $got = @($rows | Where-Object Class -eq 'A' | ForEach-Object Id | Sort-Object) -join ','
        $exp = @($c.kill | Sort-Object) -join ','
        if ($got -ne $exp) { $errors += "$($c.n): killed [$got] expected [$exp]" }
        if (@($rows | Where-Object { $_.Class -eq 'A' -and $_.Id -in 1, 2, 3, 4, 5 }).Count) { $errors += "$($c.n): killed a base process" }
    }
    $v = @((Get-MemGuardVerdict 72.7 80 88), (Get-MemGuardVerdict 80 80 88), (Get-MemGuardVerdict 87.9 80 88), (Get-MemGuardVerdict 88 80 88))
    if (($v -join ',') -ne '0,1,1,2') { $errors += "verdict [$($v -join ',')]" }
    if ($errors) { $errors | ForEach-Object { "SELF-TEST FAIL $_" }; exit 1 }
    "SELF-TEST PASS ($($cases.Count) classification cases + verdict)"
    exit 0
}

# ---- fact gathering (read-only) ----
function Get-CommitFacts {
    $os = Get-CimInstance Win32_OperatingSystem
    $limit = $os.TotalVirtualMemorySize / 1MB; $free = $os.FreeVirtualMemory / 1MB
    $ctr = Get-Counter '\Memory\Pool Paged Bytes', '\Memory\Pool Nonpaged Bytes' -ErrorAction SilentlyContinue
    $paged = ($ctr.CounterSamples | Where-Object Path -match 'paged bytes' | Where-Object Path -notmatch 'nonpaged' | Select-Object -First 1).CookedValue / 1GB
    $nonpaged = ($ctr.CounterSamples | Where-Object Path -match 'nonpaged bytes' | Select-Object -First 1).CookedValue / 1GB
    $pf = @(Get-CimInstance Win32_PageFileUsage)
    [pscustomobject]@{ LimitGB = $limit; UsedGB = $limit - $free; Percent = 100 * ($limit - $free) / $limit; PhysFreeGB = $os.FreePhysicalMemory / 1MB
                       PagedGB = $paged; NonpagedGB = $nonpaged
                       PagefileUsedGB = ($pf | Measure-Object CurrentUsage -Sum).Sum / 1KB; PagefileSizeGB = ($pf | Measure-Object AllocatedBaseSize -Sum).Sum / 1KB }
}
function Get-ProcFacts {
    $cim = @(Get-CimInstance Win32_Process)
    $win = @{}; foreach ($gp in Get-Process -ErrorAction SilentlyContinue) { if ($gp.MainWindowHandle -ne 0) { $win[[int]$gp.Id] = $true } }
    foreach ($c in $cim) {
        @{ Id = [int]$c.ProcessId; Name = $c.Name; ParentId = [int]$c.ParentProcessId; Start = $c.CreationDate; CmdLine = $c.CommandLine
           PrivMB = [int]($c.PrivatePageCount / 1MB); WSMB = [int]($c.WorkingSetSize / 1MB); Handles = [int]$c.HandleCount
           HasWindow = $win.ContainsKey([int]$c.ProcessId); Session = [int]$c.SessionId }
    }
}
function Write-GuardLog([string]$Line) {
    $dir = Split-Path $LogPath
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Add-Content -LiteralPath $LogPath -Value ('{0:s} {1}' -f (Get-Date), $Line)
}

function Invoke-GuardOnce([switch]$DoApply) {
    $cf = Get-CommitFacts
    $procs = @(Get-ProcFacts)
    $rows = Get-MemGuardClassification $procs $PID $StaleHours $HandleLimit (Get-Date)
    $privGB = ($rows | Measure-Object PrivMB -Sum).Sum / 1KB
    $other = $cf.UsedGB - $privGB - $cf.PagedGB - $cf.NonpagedGB
    '== commit: {0:N1} / {1:N1} GB = {2:N1}%   (physical free {3:N1} GB; pagefile {4:N1}/{5:N1} GB used)' -f $cf.UsedGB, $cf.LimitGB, $cf.Percent, $cf.PhysFreeGB, $cf.PagefileUsedGB, $cf.PagefileSizeGB
    '   process private {0:N1} GB | paged pool {1:N1} GB | nonpaged pool {2:N1} GB | other (shared sections, page tables, kernel stacks, drivers) {3:N1} GB' -f $privGB, $cf.PagedGB, $cf.NonpagedGB, $other
    '   handles total {0}; paged pool per 1000 handles {1:N1} MB' -f (($rows | Measure-Object Handles -Sum).Sum), (1024 * $cf.PagedGB / [math]::Max(1, ($rows | Measure-Object Handles -Sum).Sum / 1000))
    '== top families by private commit'
    $rows | Group-Object { $_.Name.ToLowerInvariant() } | ForEach-Object {
        [pscustomobject]@{ Family = $_.Name; N = $_.Count; PrivGB = [math]::Round(($_.Group | Measure-Object PrivMB -Sum).Sum / 1KB, 2); WSGB = [math]::Round(($_.Group | Measure-Object WSMB -Sum).Sum / 1KB, 2)
                           Handles = ($_.Group | Measure-Object Handles -Sum).Sum; Class = (($_.Group.Class | Select-Object -Unique) -join '/') }
    } | Sort-Object PrivGB -Descending | Select-Object -First 10 | Format-Table -AutoSize | Out-String -Width 200 | ForEach-Object { $_.TrimEnd() }
    $idle = @($rows | Where-Object { $_.PrivMB -gt 300 -and $_.WSMB -lt 0.1 * $_.PrivMB })
    if ($idle.Count) { '== reserved/idle commit (private > 300 MB, working set < 10%): RECOMMENDATION only, not killed'
        $idle | ForEach-Object { '   {0} pid {1} class {2}: private {3} MB, WS {4} MB, age {5} h' -f $_.Name, $_.Id, $_.Class, $_.PrivMB, $_.WSMB, $_.AgeH } }
    $big = @($rows | Where-Object { $_.Handles -gt 20000 })
    if ($big.Count) { '== high handle holders'; $big | ForEach-Object { '   {0} pid {1}: {2} handles' -f $_.Name, $_.Id, $_.Handles } }
    $a = @($rows | Where-Object Class -eq 'A')
    '== class (A) orphans safe to kill: {0}' -f $a.Count
    $a | ForEach-Object { '   KILL {0} pid {1}  private {2} MB, handles {3}: {4}' -f $_.Name, $_.Id, $_.PrivMB, $_.Handles, $_.Reason }
    $suspects = @($rows | Where-Object { $_.Class -ne 'A' -and $_.Orphan -and $_.AgeH -gt $StaleHours -and $_.Name -match '^(python|node|pwsh|powershell|bash|sh|find|rg|fd|cmake|ninja|cl|link)$' })
    if ($suspects.Count) { '== old orphans NOT killed (no proof they are ours; review manually)'
        $suspects | ForEach-Object { '   {0} pid {1} age {2} h private {3} MB: {4}' -f $_.Name, $_.Id, $_.AgeH, $_.PrivMB, $_.Reason } }
    if ($DoApply) {
        if (-not $a.Count) { 'APPLY: nothing to kill.'; Write-GuardLog "apply: nothing to kill, commit $([math]::Round($cf.Percent,1))%" }
        foreach ($k in $a) {
            $line = 'kill {0} pid {1} (private {2} MB, handles {3}): {4}' -f $k.Name, $k.Id, $k.PrivMB, $k.Handles, $k.Reason
            try { Stop-Process -Id $k.Id -Force -ErrorAction Stop; "APPLY: $line"; Write-GuardLog $line }
            catch { "APPLY FAILED pid $($k.Id): $($_.Exception.Message)"; Write-GuardLog "FAILED $line :: $($_.Exception.Message)" }
        }
        if ($a.Count) { Start-Sleep -Seconds 3; $cf2 = Get-CommitFacts; 'commit after apply: {0:N1}% (was {1:N1}%)' -f $cf2.Percent, $cf.Percent; Write-GuardLog ('commit after apply {0:N1}% (was {1:N1}%)' -f $cf2.Percent, $cf.Percent); $cf = $cf2 }
    }
    $proj = $cf.Percent + $LiveRunDelta
    $code = Get-MemGuardVerdict $cf.Percent $WarnPercent $FailPercent
    $gate = if ($cf.Percent -le 85) { 'start gate (<=85) OK' } else { 'start gate (<=85) FAILS' }
    $live = if ($proj -le 90) { 'XR live run projected within 90%' } elseif ($proj -le 92) { 'XR live run projected above the 90% stop but below 92%' } else { 'XR live run projected over 92%: will abort' }
    $word = @('OK', 'WARN', 'FAIL')[$code]
    'VERDICT {0}: commit {1:N1}%, projected live run {2:N1}% (+{3:N0}); {4}; {5}; class A pending: {6}' -f $word, $cf.Percent, $proj, $LiveRunDelta, $gate, $live, $a.Count
    return $code
}

try {
    if ($Watch -gt 0) {
        $n = 0; $worst = 0
        while ($true) {
            $cf = Get-CommitFacts
            $code = Get-MemGuardVerdict $cf.Percent $WarnPercent $FailPercent
            $worst = [Math]::Max($worst, $code)
            $top = Get-Process | Sort-Object PrivateMemorySize64 -Descending | Select-Object -First 1
            '{0:HH:mm:ss} commit {1:N1}% (paged pool {2:N1} GB) {3}  top: {4} {5:N1} GB' -f (Get-Date), $cf.Percent, $cf.PagedGB, @('ok', 'WARN', 'FAIL')[$code], $top.Name, ($top.PrivateMemorySize64 / 1GB)
            if ($code -ge 1) { Write-Warning ('commit {0:N1}% >= {1}%; run tools/mem-guard.ps1 -Report' -f $cf.Percent, $(if ($code -eq 2) { $FailPercent } else { $WarnPercent })) }
            if ($WatchCount -gt 0 -and ++$n -ge $WatchCount) { break }
            Start-Sleep -Seconds $Watch
        }
        exit $worst
    }
    $out = @(Invoke-GuardOnce -DoApply:$Apply)
    $code = [int]$out[-1]
    $out[0..($out.Count - 2)] | ForEach-Object { "$_" }
    exit $code
} catch {
    "mem-guard error: $($_.Exception.Message)"
    exit 2
}
