# UNCAP: how ACZ limits its frame rate, the uncap:1 option, the sim-speed guard and extrap:1 smoothing

User requirement (2026-10-27): "I want an UNCAPPED frame rate for the game so it runs smoothly in VR."
This was offline work only. Nobody launched the game, SteamVR or a headset. A driver agent runs the live plan in section 6.
Tags: **[E]** = measured here (logs, screenshots, binary, offline tests). **[I]** = inference.
Code: `src/acz-vr/frame_pacing.h` (header only), the existing Present hook in `ee_camera_probe_win.cpp`, `xr_stereo_proxy.cpp`, `xr_stereo_output.cpp`.
Tests: `tests/acz-vr/frame_pacing_offline_tests.cpp` (`frame_pacing_offline`).
Analysis scripts used for section 1: `period.py` and `fixedstep.py` (scratch scripts, described in section 1). They read the
`ee-camera;obs` lines, which carry a microsecond Present timestamp `A=` and the E6 aircraft position, per frame for the first 300 frames of each series.

## 1. Findings

### 1.1 The simulation advances a FIXED STEP per rendered frame [E]
For every pair of consecutive game frames in the archive (23 distinct ee-camera series, 6,717 moving frame pairs, headset tests 15-17
and null runs step50-step79), we compared how far the aircraft moved (E6 ax/ay/az) with how long that frame took between Presents.
Each displacement was normalised by the median of its ±5 neighbours, because speed changes slowly.

| frame duration | frames | displacement / local median (p10..p90) |
|---|---|---|
| < 10 ms (catch-up frames) | 743 | 1.000 (0.996..1.000) |
| 10-14 ms | 1,164 | 1.000 (0.996..1.000) |
| 14-19 ms | 2,780 | 1.000 (0.996..1.000) |
| 19-25 ms | 1,624 | 1.000 (0.996..1.000) |
| >= 25 ms (stalls) | 406 | 1.000 (0.996..1.000) |

A game that integrated wall-clock delta time would move ~0.35x on the 6 ms frames and ~2x on the 34 ms ones. ACZ moves exactly the
same distance per frame: 18.5 units per frame in step50, whatever the frame's duration. This is the PS2 design: one guest frame is one
simulation step. Consequences:
* **Game speed = frame rate / 60.** In headset test 17 the game averaged 12,119 frames in 371 s (32.6 fps), so it ran at **54% speed**.
  During the 12-16 fps stretches it ran at about **25% speed** (slow motion). The "really bad frame rate" in D-330 was also slow-motion gameplay.
* **Rendering faster than 60 fps would make the game run faster.** No real "uncapped" frame rate can exist for this game without
  changing its simulation. That needs game-code patching (not allowed) or rendering interpolated in-between frames (section 4).

### 1.2 The 60 fps cap is the game's own timer, not the monitor [E for the numbers, I for the mechanism]
* Long-run Present period, from the Present timestamps of the stall-free runs: 16.6668 ms (step50, 9,779 frames, 163 s), 16.6651
  (step54, 98 s), 16.6683 (step75, 67 s), 16.6686 (step65). That is **60.000 fps within 0.01%** [E].
* The desktop runs at 119-120 Hz (Win32_VideoController CurrentRefreshRate=119, max 143; RTX 4090 + "Virtual Desktop Monitor") [E].
  Vsync at interval 1 would give ~120 fps. Interval 2 on a 119.88 Hz mode would give 59.94 fps = 16.683 ms, which is 0.1% off the
  measurement; the logs resolve 0.01%. The mode that was active during the archived runs is not recorded [I].
* `acz.exe` imports `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `timeBeginPeriod`, `timeGetTime`, `Sleep` and
  `WaitForSingleObject`. It imports no waitable timer, and the strings contain no `vsync` or `FrameLatencyWaitable` (a read-only string
  check of the binary) [E]. The skip-intro mod's log shows the game calls QPC (`game called QueryPerformanceCounter`); timeGetTime was
  never logged as called. Running that QPC 8x fast-forwards the intro (D-270) [E].
* Present gaps after a stall catch up to an absolute schedule: 34 + 6 + 10 ms = 3 frames; 44 + 6 + 6 + 10 ms = 4 frames [E]. Both a
  deadline-based limiter and a queued swap chain produce this pattern.
* **Inference:** an internal QPC + Sleep limiter (timeBeginPeriod 1 ms, absolute 1/60 s deadlines, catch-up) sets the 60 fps.
  The swap-chain sync interval the game passes, its swap-chain flags and its frame-latency setting have never been logged.
  This branch adds the logging (section 2.2), and the first live run answers the question (section 6, run U0).

### 1.3 Game time runs 1:1 with wall time at 60 fps [E]
step74 run A (59.9 fps): the HUD TIME reads 19'53"56 in `shots/acz-20261027-040004-952-T0.00.png` and 19'33"76 in
`...-040024-748-T19.82.png`. That is 19.80 s of mission clock in 19.796 s of wall time (file-name timestamps in ms; capture jitter
about ±50 ms). This matches 1.1: at 60 fps the fixed step equals real time.

### 1.4 Producer/consumer pacing at higher game rates [E, code reading]
* The producer never waits on the game thread. The slot pick is try-only (`step5PickSlot_locked`, `gpu_copy_step5_hooks.inl`): it
  uses a free slot or a published slot that a newer frame has superseded and whose GPU work has completed. Otherwise it skips the frame
  and counts it (`skipped`, kinds `no-free-slot` / `slot-gpu-not-complete` / `slot-metadata-busy`). The metadata mutex is
  `try_lock` on the game side.
* The consumer always takes the newest published frame (`try_acquire_latest` picks the highest frame id above the last one). Frames it
  never consumed are recycled.
* With 3 slots and a 90 Hz consumer, at > 90 game fps one slot is held, one holds the newest frame and one is being produced. A
  superseded slot whose GPU replay is still running is skipped (counted), so the game is never stalled. No change was needed.
  `frame_id_gaps` and `skipped` in the summaries show this.

## 2. uncap:1 (default OFF)

### 2.1 What it does
* Where: inside **`xr_terrain_stage`**, for example `-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1;uncap:1'`. No
  new config key: the morning set has 31 of 32 keys and `perf_lean` uses the last one. The option is removed before the terrain parser
  sees the value, like `tac:` and `e6loc:`. The value must stay under 256 characters (99 before D-367): the installed value plus `;uncap:1;extrap:1` is 63.
* Needs the existing Present vtable hook, which is armed only with `ee_camera_probe` (written by `-TerrainRender` unless `cam:` is
  given). Without it the proxy logs `present-hook=absent(...)` and does nothing.
* It acts only while an XR session runs (`pacing::uncap_active()` = option parsed AND the attempt's first XR frame done; cleared when
  the attempt ends). During that time the game's `Present`/`Present1` goes out with **SyncInterval 0**.
  `DXGI_PRESENT_ALLOW_TEARING` is added only if the GAME created its swap chain with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`, the factory
  reports tearing support and the swap chain is windowed. The flag is never added to a swap chain created without it (that would fail),
  and swap-chain creation and ResizeBuffers are not changed.
* Safety: TEST presents and nested presents are never changed. If the changed call returns `DXGI_ERROR_INVALID_CALL`, the game's
  original call is issued at once, and the override is switched off for the rest of the process (`pacing;event=uncap-refused`).
* The producer/consumer handshake, fence waits and counters are untouched: Present is not part of the producer's publication. The
  ee-camera Present tick and seam A still run after the native call, as before.
* Not done (hard rule): no patching of the game's limiter (its QPC/Sleep loop) and no QPC/Sleep import hooks.

### 2.2 Always-on observation (with the probe)
* `pacing;event=swapchain-desc` (once per swap chain): size, format, buffers, swap effect, flags, waitable, max frame latency, the
  allow-tearing flag, factory tearing support, windowed, refresh rate.
* `pacing;event=present-stats` (every 5 s, ee-camera.log + diagnostics):
  * `game-sync=0:a,1:b,2:c,3+:d`: the sync interval the GAME passes;
  * `game-flags-or`, `fps`, `max-gap-ms`;
  * `uncap-requested`, `xr-active`, `uncap-active`, `overridden`, `tearing-added`;
  * the sim-rate guard (2.3).

### 2.3 Simulation-speed guard [E offline]
The guard runs on every Present with the wall time and the E6 camera position, so the terrain key must be on. It reports:
* `units-per-frame` and `units-per-s`;
* a **live fixed-step test**: `slope` compares the relative displacement of long frames (> 1.4x the running mean) and short ones
  (< 0.7x). The value is ~0 for a fixed step and ~1 for delta time, and it labels the window `sim=fixed-step|delta-time|unclear|unknown`;
* `game-speed-if-fixed` = fps / 60.

`warn=game-faster-than-60-fixed-step-sim-runs-fast` is set when fps > 63 and the simulation is not delta-time. Offline cases: a
fixed-step series with catch-up frames gives slope < 0.1, a delta-time series gives slope > 0.9, and 120 fps fixed-step gives warn with speed 2.0.
The mission timer is not readable from memory, so the authoritative check is HUD TIME vs wall clock over 60 s (section 6).

### 2.4 What to expect from uncap:1 [I]
* If the internal limiter caps the game (1.2), uncap:1 keeps the game at 60 fps. At most it removes the time Present spends waiting for
  the display, a few ms of latency and jitter on the game thread. No speed change.
* If the game is in fact vsync-paced (for example interval 2 on 120 Hz), uncap:1 raises its frame rate **and makes the game run fast**
  (fixed step). The guard warns. Then switch it off.
* uncap:1 cannot fix the main problem of D-330/D-336: the game falls to 12-16 fps (slow motion) after minutes in a mission. That is the
  FPS track's hook-cost fix (D-336) and its live A/B.

## 3. extrap:1 (default OFF): smoothing for content we draw ourselves

### 3.1 Why only our content can move
The consumer composes an XR frame only when a NEW producer frame arrives (`transact`). A re-submitted XR frame reuses the last layers
unchanged (`pace`: `plan = last_plan_`), and SteamVR reprojects them for head motion. The replayed game images (objects, effects, HUD,
replayed cockpit) are pixels from the game frame and cannot be moved. What we render from a camera can be moved: the stage terrain,
props, trees, sky/clouds/water and the TACTICAL overlay.

### 3.2 What extrap:1 does
* Once per consumed producer frame, the chosen terrain camera (the game camera, else E6) feeds `pacing::MotionExtrap` with its frame
  id and publication time. The velocity is measured **per game frame** from the last two frames; skipped ids are divided out. The
  rotation rate is measured per game frame as an axis-angle of `M_cur * M_prev^T`. Each is multiplied by the measured game frame rate
  (EMA over windows of 250 ms or more). Because of 1.1, this is the true wall velocity even at 15 fps, without the noise of 6 ms
  catch-up frames.
* The camera moves forward by the **select age**: this XR frame's predicted display time minus the frame's own predicted display time
  (`select-age-ms`, 44 ms in test 17), clamped to `extrapms` (default 33 ms, range 5-100).
* No overshoot. A frame is not extrapolated when:
  * the step direction turned more than ~45 deg, or the step length changed by more than 2x (reversal, stop, braking, launch);
  * the step was larger than 400 units, or rotated more than 10 deg (cut/respawn); it resumes after two normal frames;
  * fewer than two steps or no frame-rate estimate exist yet.
  Extrapolated rotation is clamped to 6 deg. `extrapatt:0` extrapolates translation only.
* TACTICAL markers lead by their own tracked velocity over the same horizon, clamped to 400 units.
* Log lines: `xr-stereo;event=extrap-config` and, every 5 s, `xr-stereo;event=summary-extrap`: frames, translated/rotated/clamped/
  refused, no-horizon, horizon mean, camera shift (mean/max), game fps, markers led, last reason.

### 3.3 Trade-off (why it is opt-in) [I]
With extrap:1 the terrain is drawn at the display-time position while the replayed objects stay at frame time. The relative slip is
`v_aircraft x horizon`. At 18.5 u/frame x 60 = 1,110 u/s and a 16 ms horizon that is ~18 units (~1.5 m at ~12 u/m): invisible for
ground objects kilometres away, possibly visible for an object close beside the aircraft. Rolling at 180 deg/s, the terrain horizon
moves ~3 deg ahead of the replayed HUD pitch ladder for that frame. Without extrap:1 everything lags together by the select age. Decide
in the headset.

### 3.4 Tests (`frame_pacing_offline`) [E]
* A constant-velocity camera (18.5 u/frame, 60 fps) extrapolated by +8 ms lands 0.48 frames ahead (8.88 units) within 0.01 units.
* A 100 ms horizon clamps to 33 ms (1.98 frames).
* Reversal and stop: exactly the current position, no overshoot; extrapolation resumes after two consistent steps.
* A 90 deg turn and a 30% speed drop: refused.
* A steady 1 deg/frame turn: position within 0.32 units of the true arc, heading within 0.05 deg, axes orthonormal, right x up = -fwd.
* Rotation clamp to 6 deg; 15 deg/frame treated as a cut.
* Teleport refused, then resumes.
* Skipped frame ids keep the per-game-frame velocity; a repeated id changes nothing.
* A 15 fps slow-motion game: a 44 ms horizon clamps to 0.495 frames.
* No extrapolation before the rate is known.
* `lead_point` clamp.
* Option parsing, the Present override and its gate, Present statistics and the sim-rate guard are covered as well.

### 3.5 Next step that would give real 90 Hz terrain (proposal, not built) [I]
Re-render our own layers (terrain/props/clouds) on re-submitted XR frames with the camera extrapolated to that XR frame's display time,
and keep compositing the last game images on top. That changes the reuse path (`pace`), costs one terrain pass per XR frame (~0.75 ms
for both eyes, gpu-cost.md) and needs a WARP harness case. It is the only way to get content above 60 Hz from a fixed-step 60 Hz game.

### 3.6 Build [E]
`D:\b-uc` (Release, Ninja, -j2). dxgi.dll `D:\b-uc\bin\dxgi.dll` sha256 b5a0da55a3ac... `frame_pacing_offline`: 76 checks.
Release ctest **312/312**: 311 on master dfe7f31 plus the new test. After that, master 26b58ff was merged; it changed only DECISIONS.md.

## 4. Summary for the user
* The game is built to run at exactly 60 fps, and its simulation advances one fixed step per frame. A truly "uncapped" game would run
  faster than real time, so none is possible without patching the game, which this project does not do.
* The real problem in your headset run was the game dropping to 12-16 fps after a few minutes, which also put it in slow motion.
  The FPS fix (D-336, live A/B pending) addresses that.
* For smoothness above 60: extrap:1 (opt-in) moves the world we draw to the display time. Re-rendering the terrain on every headset
  frame (3.5) would be the next step.
* uncap:1 exists, is off by default and is safe to try. Expect no change; if the frame rate does rise, the game speeds up and the log
  warns.

## 5. Risks
* uncap:1: if the game's pace actually comes from vsync, the game speeds up (fixed step): watch `warn=` and HUD TIME. Tearing on the
  desktop window does not matter for VR. A third-party overlay that patched Present after us is handled: the original call is reissued
  on INVALID_CALL. CPU use rises if the game spins faster.
* extrap:1: terrain/object slip (3.3). A wrong velocity right after a respawn or a camera cut is guarded (cut, warming-up, clamps), but
  game camera switches the guards do not catch (for example a view toggle that moves the eye by less than 400 units) extrapolate for
  one frame (at most 33 ms).
* Neither option changes anything with the option off: `uncap_active()` stays false (one atomic load per Present), and
  `extrap_camera` is not reached.
* Always on with the probe: one `present-stats` line per 5 s and one `swapchain-desc` line per swap chain. Per Present this costs one
  QPC read, a try-read of the camera mailbox and some integer arithmetic.

## 6. Live test plan (null driver; the driver agent)
Preconditions: `tools/mem-guard.ps1 -Report` start gate (commit <= 85%). Install a DLL set built from this branch (the dxgi proxy; the
d3d12.dll forwarding proxy is unchanged). Use the morning key set (31 keys, or 32 with -Lean). Launch acz.exe directly (D-279) and fly
Mission 02 with the skip-intro protocol. Exclude the first 15 s, when the skip-intro clock runs 8x.
1. **U0 baseline (2 min, no new options).** Collect `pacing;event=swapchain-desc` and `present-stats` from ee-camera.log. Record the
   `game-sync=` distribution (the sync interval the game uses), `fps`, `sim=` (expect `fixed-step`, slope ~0 while flying) and
   `units-per-frame`.
2. **U1 uncap (3 min):** `-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1;uncap:1'`.
   * Check `xr-stereo;event=pacing-config;...;uncap-armed=1`, then `present-stats` with `uncap-active=1` and `overridden` > 0 (unless
     the game already passes 0), and `uncap-refused` absent.
   * Compare `fps` and `summary-selection new-per-s` with U0.
   * **Speed check:** take two game-window screenshots exactly 60 s apart with ms timestamps in the names (the screenshot helper names
     them `acz-YYYYMMDD-HHMMSS-mmm-...`). Read HUD TIME on both: mission seconds elapsed / 60 = game speed (expect 1.00 ±0.01).
     Cross-check `game-speed-if-fixed`.
   * If fps > 63 or `warn=` is set: note the numbers, end the run (the game is fast-forwarding) and leave uncap:1 off.
3. **E1 extrap (3 min, with tac:1):** `...;tac:1;extrap:1`.
   * `xr-stereo;event=extrap-config` once; `summary-extrap` every 5 s with `translated` close to the frame count while flying straight,
     `refused` in turns/rolls, `horizon-ms-mean` close to `select-age-ms`, `shift-units-mean` about 1,110 u/s x horizon.
   * No new failures in summary-selection (end-frame, stale counts).
   * Visual: an xr_png_once frame is fine; smoothness itself needs the real headset.
4. Optional long U1+E1 run (>= 6 min) together with the FPS A/B (fps-investigation.md section 6). Watch whether `fps` and `sim=` hold.
5. Report: U0 sync interval and swap-chain desc (settles 1.2), U1 fps/speed (settles whether uncap:1 has any effect), E1 counters.
   Fixture/offline success does not show headset smoothness.
