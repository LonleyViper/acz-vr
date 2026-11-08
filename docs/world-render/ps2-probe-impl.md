# PS2 address probe (`ee_ps2_probe`), read-only implementation

Status: code written, built (Release, D:\b-pp) and fixture-tested offline; NOT yet run against the game. Nothing here writes game memory.
Why: D-293 / docs/world-render/ps2-free-camera-cheats.md (web data, unverified for this port) says the ACZGCT cheat addresses are offsets in the PCSX2 EE
image and that our E6 camera fields (+0xFDD410..+0xFDD878) lie inside the cheat's camera-struct range. This probe tests that on the real 64 MiB block and
logs everything an offline analysis needs to correlate the cheat struct with the E6 fields and to find a stage id. Block-relative offset = assumed PS2 address.

## Files
- `src/acz-vr/ee_ps2_probe.h/.cpp`: SDK-free core `Ps2Prober` (reuses `LensMem`, `LensRegion`, `lens_classify`, `Sha256`, `Sink`, `LensOut`, `LensIn` from ee_lens_scan.h / ee_look_write.h).
- Glue in `ee_camera_probe_win.cpp`: `ps2_tick` (Present, only stores the tracks-look anchor + stop flag), `ps2_background` (probe worker: all reads), `ps2_shutdown`
  (exit paths), own allocation excluded from the wide/diff scans (`exclLo[7]`), `LensFileOut` now takes a file stem (`ps2-probe`).
- Config: new key `ee_ps2_probe` (default off, `Config::ps2_probe_latch(gate)`), `Log::ee_ps2_probe()`. Independent of `ee_lens_scan` (a different key, so a lens run is unchanged).
- `tools/live/vr-test-start.ps1 -Ps2Probe`; test `tests/acz-vr/ee_ps2_probe_offline_tests.cpp` (ctest `ee_ps2_probe_offline`, 118 checks).

## Latch and keys
`ee_ps2_probe=true` latches only with the opt-in gate, `log_enabled`, `ee_camera_probe` (Present clock, tracks-look anchor, ee-camera log) and none of
`ee_camera_write_trials`, `ee_camera_look_write_trial`, `ee_camera_look_drive` (same rule as the lens scan). Probe-only set: 10 keys (8 gpu_copy/log keys,
`ee_camera_probe`, `ee_ps2_probe`), no `xr_stereo_output`. The full 24-key headset set plus `ee_camera_probe` plus `ee_ps2_probe` is 26 of 32 keys, under 2048 bytes (tested).

## What it does (all offsets block-relative, base = the tracks-look anchor block `lo`)
Start gate: the same tracks-look anchor `lo` present for 60 distinct Present frames (restarts if `lo` changes); no anchor within 300 s: aborted `never-anchor` (a report is still written).
Every read: `query` of the address, committed readable DATA page (`lens_classify`), window wholly inside ONE queried region, allocation base equals the one seen at start, checked
arithmetic, all-or-nothing guarded copy. Anything else is `UNREAD` / an unreadable or excluded chunk (never a partial read).
1. Scan A, `+0x800000..+0x1F00000` (23 MiB): float32 `512.0, 448.0` (0x44000000, 0x43E00000) at +0/+4, 4-byte aligned. A hit exactly 0x1C0 after the previous hit is the
   struct's own background pair (the cheat doc puts 512/448 at +0x1C0 too) and is merged (`bg-pair-hits`), not counted as a second struct. Max 4 structs kept (`camera-hits=kept/total`).
2. Scan B, `+0x300000..+0x4000000` (61 MiB): game ids `SLUS|SLES|SLPM|SLPS|SCUS|SCES|SCPS` + `_` or `-` + exactly five digits (optional `.` after the third, e.g. `SLUS_213.46`),
   max 6 kept, plus the cheat's 16-byte game-id pattern (max 2 kept).
3. Samples, starting when the scans end, one tick per second for 60 s (`stage` lines), `fixed` every 2 s for 20 samples, `cam`/`e6` every second for 10 samples:
   - `fixed`: mission `+0x3FFD1C`, `+0x3ACEA0`, `+0x76587C`, `+0x765320` (float draw-distance candidate) and each game-id hit: u8, u32, f32 at the point and 32 bytes of context (`ctx-off` = start of the 32 bytes, 16 before the value).
   - `cam` per struct hit: sig (should stay 512,448), `+0x198/+0x19C` (`dist198`), `+0x1C0/+0x1C4` (`bg1C0`), `+0xB30..+0xB50` (`pos`, 9 floats), `+0xC20..+0xC28` (`rot`), `rot-eq-e6` (rot within 1e-3 of the E6 rotation, not all-zero), `pos-eq-e6` (any of the three position triples equals the E6 position).
   - `e6`: look yaw `+0xFDD410`, aircraft position `+0xFDD42C..34` (`air`), heading `+0xFDD448`, camera position `+0xFDD860..68` (`campos`), composed rotation `+0xFDD870..78` (`rot`), read the same second.
   - `stage` (every second): mission byte, `+0x3ACEA0`, `+0x76587C`, the first struct hit's `+0xB30` and `+0xB48` triples (`cam-b30`, `cam-b48`) and E6 `campos`.

## Caps (every one explicit; any cap hit makes coverage UNKNOWN)
Scan bytes 128 MiB (needs ~84 MiB), scan time 60 s, chunks 256 KiB (+16 B overlap so matches straddling a chunk edge are seen), 8 chunks per worker call, struct hits 4, id hits 6, aob hits 2,
samples 20 / 10 / 60, log lines 600 (extra non-essential lines are dropped from the log only, counted in `log-lines-dropped`; essential lines always pass), report file 96 KiB (the last 3 KiB are reserved
for summary lines; `file-truncated=1` when the body was cut). `coverage=UNKNOWN` also for: unreadable/excluded chunks, identity change, unreadable sampled values, run ended early (stop file / process exit / abort),
dropped hits, scan not finished, `anchor-moved` (the probe re-located the block; the probe keeps the one pinned at start). A missing struct is only worded "not found within this signature/range/sampling" when coverage is complete.
The verdict line is numerical only (mission values seen, whether game id / struct were found, rot/pos agreement counts per struct) and carries `statement="values only; ..."`: whether block offsets are PS2 addresses and how the struct relates to E6 is for the offline analysis.

## Output
- `%LOCALAPPDATA%\acz-vr\logs\ee-camera.log` (and diagnostics): line family `ps2-probe;event=armed|wait|start|scan-start|scan-done|first-reject|hit|sampling-start|e6|stage|fixed|cam|anchor-moved|summary|coverage|verdict|file`.
- `%LOCALAPPDATA%\acz-vr\logs\ps2-probe.txt` (previous kept as `ps2-probe.prev.txt`): the same lines; the LAST line is `sha256=<hex of every byte before it>`, repeated in `event=file`. An evidence card pins that hash (and the DLL set hash).
- Kill: create `%LOCALAPPDATA%\acz-vr\lens-scan.stop` (shared with the lens scan; `look-write.stop` also works); a partial report is still written. Process exit ends a running probe as partial (`reason=process-exit`).

## Exact live recipe (NULL SteamVR driver, no headset; run only when the user is off the PC)
1. Preconditions as the lens scan: no build/`multi run`, commit below 85 %, never `steam.exe -applaunch`, no other probe keys. Delete any old `lens-scan.stop`.
2. `pwsh -NoProfile -File tools/live/vr-test-start.ps1 -DllDir <dll set built from this commit> -Ps2Probe` (installs the proxy DLLs, writes the 10-key runtime.cfg; every other switch is ignored).
3. Launch ACZ per the live protocol; reach a mission COCKPIT view (route `research/experiments/step40-live-run1/route.txt`), level flight. Keep the aircraft flying (the 60 s sampling is the camera-position-per-second series; fly gently so the camera position changes), do not touch the look keys. The probe starts about 1 s after the tracks-look anchor has been stable for 60 Present frames; scans take seconds, the run ends about 60 s later.
   For a stage-id comparison repeat in a second mission (restart the game: the block base moves per launch).
4. Wait for `ps2-probe;event=file` in `ee-camera.log` (or `logs\ps2-probe.txt` whose last line is `sha256=`), then `vr-test-stop.ps1` / `l1.ps1 -Action remove`.
5. Collect `ps2-probe.txt`, record the sha256, DLL set hash, `event=coverage` and `event=verdict`. Offline analysis: (a) game-id hit and mission byte plausible (3/255 etc.) = block offsets look like PS2 addresses; (b) `rot-eq-e6=1` on all samples for a struct = the cheat struct's rotation is the E6 composed rotation (and `pos-eq-e6` which position triple); (c) `dist198`/`bg1C0` static or per-frame; (d) `draw765320` value; (e) mission byte vs the stage heightfield set.

## Test coverage (fake 64 MiB image, `ee_ps2_probe_offline_tests`)
Planted mission/gates/draw float/game id/two structs/E6 fields (values, line counts, 32-byte context, rot/pos correlation, hash-pinned file); drifting position and a mission change; signatures and ids straddling chunk edges, at the range ends and just outside, near-miss ids rejected; unreadable range, mid-chunk region split and non-data pages (rejected, UNKNOWN, first-reject line, no absence claim); unreadable sampled values; allocation change mid-run; byte cap, time cap, struct/id/aob hit caps, log cap, file cap (truncation flagged, summary kept, hash valid); failing file write; no anchor (abort), unstable anchor, unreadable anchor block, stop file, finish()+drain() exit path, disabled = inert, deterministic hash; `write_float` never called and image checksum unchanged; config latch and key budget (10 keys, 26 of 32, below 2048 bytes).

## Unverified
Windows glue compiled but never run in the game; whether the real block contains `SLUS_...` text or the cheat's AOB; whether block offsets equal PS2 addresses; how many extra 512/448 pairs real memory holds; whether the E6 offsets and the struct share the 64 MiB block base; page protection of the real block (the `lens_classify` WRITECOMBINE-on-private tolerance is inherited from step 47); worker cost of ~84 MiB of guarded copies during gameplay.
