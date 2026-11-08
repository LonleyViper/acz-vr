# FPS investigation: "the frame rate is really bad" (headset test 17)

Offline only: no game, SteamVR or headset run (a driver agent runs the live A/B later). Evidence: `D:\acz-evidence-archive\headset-test9..17`,
`step41..step79` (xr-stereo.log, step5-producer.log, ee-camera.log, diagnostics.log). Tags: **[E]** measured/tested here, **[I]** inference.
Tools: `tools/live/fps-log-table.py` (section 1 table), `tools/live/fps-cost-table.py` (the new cost line, section 5).

## 1. What the logs show

### 1.1 It is the GAME's frame rate, and it decays with time in the mission [E]
The producer publishes one stereo frame per game frame (`skipped` stays 0 in test 17), so `published/s` in step5-producer.log is the
game's own frame rate. In headset test 17 it starts at 60.0/s and falls: 60 (0-60 s) -> 51 (75 s) -> 43 (85 s) -> 29 (95 s) -> 27-42 (100-130 s)
-> 47-52 (140-160 s, a partial recovery) -> 21-12 (190-230 s) -> 11-27 (230-365 s). The consumer's `new-per-s` (15.19 in the summary the user
saw) follows it; the XR layer keeps running at ~68/s (`xr-period-ms-mean` 14.6) by re-using the last frame, i.e. the headset shows a
judder of 4-5 repeats per new image. The renderer itself is not the limit: 0.75-0.8 ms per frame for both eyes on the RTX 4090
(gpu-cost.md); `gpu-blit-ms` rose 0.7 -> 2.5 ms during the run (still small), `frame-cpu-ms-max` of the XR owner thread 30-50 ms (a
different thread from the game's).

### 1.2 Every long run decays; every null run was short [E]
`python -I tools/live/fps-log-table.py ...` over all archived runs with an XR summary (H = real headset, null = SteamVR null driver):

| run | xr s | new/s first 20 s | new/s last 30 s | t(<40/s) s | game fps first | game fps last | avg-cmds | ee-camera MB | ee KB/s |
|---|---|---|---|---|---|---|---|---|---|
| H9 (10-26 15:23) | 211 | 52 | 19 | 30 | 59 | 12 | 583 | - | - |
| H10 | 376 | 47 | 13 | 50 | 59 | 16 | 482 | - | - |
| H11 | 206 | 50 | 22 | 30 | 59 | 30 | 536 | - | - |
| H13 | 85 | 51 | 39 | 70 | 59 | 56 | 502 | - | - |
| H14 | 80 | 52 | 44 | 75 | 59 | 58 | 393 | - | - |
| H15 | 406 | 45 | 12 | 20 | 58 | 14 | 528 | 0.67 | 1.7 |
| H16 | 65 | 57 | 50 | 60 | 60 | 22 | 524 | 0.96 | 14.7 |
| H17 (10-27 07:40) | 370 | 58 | 17 | 70 | 60 | 16 | 419 | 2.28 | 6.2 |
| step41 null | 140 | 44 | 37 | (20) | 51 | 52 | 437 | - | - |
| step53 null | 60 | 53 | 49 | - | 60 | 60 | 474 | 0.79 | 13.2 |
| step62a/b null | 50/30 | 55/56 | 51/51 | - | 59/60 | 59/60 | 491/470 | 0.71/0.54 | 14.1/18.0 |
| step74a/b null | 40/35 | 57/58 | 57/58 | - | 60/60 | 60/60 | 356/348 | 0.62/0.58 | 15.5/16.5 |
| step77 null | 95 | 54 | 54 | - | 60 | 59 | 411 | 0.95 | 10.0 |
| step79b null | 50 | 55 | 53 | - | 60 | 60 | 388 | 0.73 | 14.6 |

* All eight headset runs lose frame rate after 20-75 s, and the long ones (H9, H10, H15, H17) end at 12-16 game fps. The decay is
  present in the 10-26 afternoon builds (H9-H11, before the terrain, cockpit model, E6 locator and ee-camera logging existed), so none of
  the overnight features started it.
- Every null-driver run lasted 25-140 s, inside the window where headset runs are still at 50-60 fps, and they held 59-60 game fps.
  **The archive therefore cannot separate "real headset" from "time in the mission"**: no null run was long enough. This is the first thing
  the live A/B must fix (section 6).
- Logging volume does not track the frame rate: H16 wrote 14.7 KB/s of ee-camera log at 57 fps, H17 6 KB/s at 16 fps, H9-H14 had no
  ee-camera log at all and decayed the same way. The whole log output of a run is 0.2-1.3 MB of step5+xr text and 0.5-2.3 MB of ee-camera
  (2.3 MB in ~6 min = 6 KB/s, ~16 lines/s), and the diagnostics ring drops rather than blocks. Logging is **not** the cause [E for the
  volumes, I for "not the cause"].
- `avg-commands` (the producer's replay commands per frame, 340-580) and `avg-members` (44-65) move a little with the scene but do not
  explain a 4x drop (test 17: 361 commands at 60 fps at t=10-60 s, 366-371 at 27-42 fps at t=90-130 s) [E].
- `member-refused` 0, `fence lag` 0-1, `slots` never exhausted, `failures` 0 in test 17: the producer is never waiting on its own GPU
  work or slots [E].

### 1.3 Other counters [E]
* test 17 diagnostics summary: 5161 PSO events (1065 priority overflows), 431,919 descriptor events, 314,038 ExecuteCommandLists,
  3,416,217 requirements_metadata events (2,020,811 dropped by the ring), execute-interval max 109 ms.
* `world=refused` 326 of 12,004 frames (entry-state-unknown), `pose-valid` 97%: no correlation with the slow windows.

## 2. Suspected cost breakdown (evidence vs inference)

The producer's code runs INSIDE the game's recording calls: every Draw, every binding setter, every descriptor write, every
ExecuteCommandLists, and Present (ee-camera tick). Hundreds of draws and several thousand setter calls per frame pass through it. Per call
(read from the code, d3d12_hook.cpp, native_binding_observer.cpp, buffer_binding_registry.cpp; costs of the scans measured offline):

| hook / work | per frame (estimate) | per-call cost | grows with | tag |
|---|---|---|---|---|
| **setter IA/index/root-SRV address lookup** (`setVertices`/`setIndex`/`setAddress` -> `diagnostic_address` -> `BufferBindingRegistry::observed_address`): a WHOLE-MAP scan of every registered game buffer, on every list (L3 is on for all lists with `diagnostic_ownership_pins`) | ~1-3 per draw x 400-600 draws | 4.5 us at 1000 buffers, 19.5 us at 4000, 33.7 us at 8000 (std::map walk) | number of live game buffers; cap 8192 | E (cost), I (count) |
| **CBV descriptor writes / root CBV-SRV resolution** (`NativeBindingObserver::address_resource`): for EVERY tracked buffer, call `buffer_address`, which itself scans every buffer: O(N^2) | descriptor writes ~5/frame (431,919 per run) | ~2 ms per call at N = 1000 (pre-fix measurement), quadratic | buffers^2 | E (cost), I (count) |
| `cpu_heap` (every descriptor write/copy): every heap x `cpu_slot`, which scans every heap: O(H^2) | ~5/frame | small while H is small | heaps^2 | E (code) |
| Draw: `observeDraw` + `ConstantCall::drawReturned` + `promoteT0AtDraw` (hook mutex 3-4x, list-state map lookups, L3 34-source update, constant/table witness snapshots for DrawCPT draws, requirements sampler) | every draw | ~1-3 us (I) | flat | I |
| `step5Draw` (producer: classification, member capture, per-eye replay recording ~400 commands) | every draw | the replay is the producer's purpose | scene | I |
| Execute: list QI + vtable checks, L3 submission bookkeeping, 3 formatted diagnostics records + one `l3-link` per list (~576-byte snprintf each) | 3-4 Executes/frame | ~5-20 us (I) | flat | I |
| ee-camera Present tick (VirtualQuery range check, probe observation, E6 locator 92 bytes, look/lens/ps2/terrain ticks) + seam B frame sample | 1 + 1 | ~10-50 us (I) | flat | I |
| logging: diagnostics ring push (format + try-lock), feature-log queue copy, ee-camera fprintf + 1 s flush | tens/frame | ~1-2 us each (I) | flat | I |
| **diagnostics flush on the game thread**: `flush_after_execute()` at the end of the Execute hook drains the ring and APPENDS to diagnostics.log (open/write/fflush/close per batch, up to 32 KiB appends, rotations = renames) and overwrites evidence records, every 64 submissions or 250 ms | ~4/s | file-system latency, unbounded by design ("OS I/O duration is not bounded", diagnostics_log.h) | flat; spikes with disk/AV | E (code), I (cost) |

**Leading hypothesis [I]:** the decay is per-call bookkeeping whose cost grows with what the game has created so far. The only
super-linear paths found on the recording thread are the registry scans above; their per-call cost is linear (setters) or quadratic
(CBV/root resolution) in the number of live game buffers, so a mission that keeps creating buffers (combat effects, streaming) makes
every frame slower over minutes, and a short null run at mission start never gets there. J-130 already flagged "setter cost ... require
measurement" for exactly this scan. **Not proven:** the archive has no buffer count; the new `registry=res:N` field (section 5) gives it.
Competing explanations the A/B must rule out: GPU/CPU contention with the real SteamVR compositor (the null runs had no real compositor
load), game content (combat) and memory pressure (96-97% commit in headset runs, D-323/D-329: paging the game's working set).

## 3. Config keys: what is required, what is diagnostics

The morning install (31 keys, step78). Every key is either a **latch** the producer/consumer chain requires or a **render feature**;
none is diagnostics-only, so the lean profile cannot simply drop keys:

| key | role | needed by the shipped renderer? |
|---|---|---|
| log_enabled | latch: every producer/consumer key is effective only with logging (diagnostics_log.cpp) | yes |
| diagnostic_ownership_pins | latch: step-1..5 owner requires it (gpu_copy_step1_hooks.inl `step1Mode`); also root-constant tracking for the producer (step5 `root-constants-not-tracked` without it); turns on L3 observation (the setter scans) | yes |
| gpu_copy_witness_observation | latch: step1Mode | yes |
| gpu_copy_early_device_hook | latch: step-3 mode, xr_stereo_output, ee_camera_probe | yes |
| gpu_copy_step3_experiment, gpu_copy_step3_whole_set | latch: the step-5 producer runs as step-3 whole-set mode | yes |
| gpu_copy_step5_continuous, xr_stereo_output | the producer and the XR consumer | yes |
| members_by_class, hmd_fov, hud, xr_hud_layer, hud_markers, xr_hud_markers_layer, world_objects, effects, xr_effects_layer, effects_dual, world_in_projection, world_hold_last, world_objects_depth_fix, world_window_occlude, terrain_render/stage, cockpit_model, xr_edge_feather, the distances/scales/units | render features / their parameters | yes (they change the image) |
| ee_camera_probe | the E6 camera feed for terrain + aircraft auto | yes (with terrain) |

`gpu_copy_step5_world_observe`, `phase_observe`, `hud_observe`, `precockpit_observe`, `xr_png_once`, `xr_debug_view` are the
diagnostics-only keys; none of them is in the installed set.

## 4. What the lean profile changes

### 4.1 Always on (no key): identical-result algorithmic fixes [E]
* `BufferBindingRegistry` keeps a GPU-VA index (`base -> {id, bytes}` multimap + multiset of sizes) in step with `resources_`.
  `observed_address` and `buffer_address` count intersecting buffers only among bases in `(address - max_size, address + bytes)` and stop at
  the second match (only 0 / 1 / more matter). Same predicate, same 8192 cap, same rejection texts.
* `NativeBindingObserver::address_resource` / `cpu_heap`: `buffer_address` / `cpu_slot` can only succeed for the UNIQUE buffer/heap that
  intersects/contains the span/handle, so that candidate is computed once and confirmed by the same call: O(log N) / O(H) instead of
  O(N^2) / O(H^2).
* Offline proof (`fps_lean_offline`): randomised differential test against verbatim copies of the old loops (6 seeds, 400-2500 buffers incl.
  overlapping, adjacent, 64 MiB, top-of-address-space buffers, removals/re-adds, empty/overflowing spans, the producer's 416-byte t10 span;
  2 x 300 heaps incl. overlapping heaps, CPU and GPU handles). Every result, offset and rejection text is equal.
* Measured per call (fps_lean_offline timing, this machine, Release): see section 5.

### 4.2 `perf_lean=true` (vr-test-start.ps1 -Lean): diagnostic logging only (volume, and where the flush runs) [E]
Latched with logging; read only by logging code:
* ee-camera: the observation lines `obs`/`obs2`, `event=timeline`, `event=activity`, `event=region-scan`, `event=hexdump` are not formatted
  into the shared ring or the ee-camera log (e6-locate, summaries, arming, refusals and results stay). The probe's state machine is
  unchanged.
* ExecuteCommandLists: no per-list `l3-link` record, `l3-counts`/`l3-repeats` at most once per second instead of on every Execute (the L3
  bookkeeping itself is unchanged; those two records live in a latest-value slot).
* Diagnostics flush: the periodic flush (ring drain + file appends/rotations) runs on the step-5 producer worker (100 ms tick, same 250 ms
  clock limit, no hook or producer mutex held) instead of inside the game's ExecuteCommandLists; the game thread skips its flush while a
  worker flush happened within the last 2 s (so it still flushes if the worker is not running). Release checkpoints are unchanged.
  `diagnostics_activation_offline` covers: lean off unchanged; lean on: first flush on the game thread, worker flush, 250 ms limit on the
  worker, no game-thread I/O while the worker is alive, game-thread flush again after 2 s of worker silence; no mutex held during appends.

It is the 32nd key of the morning set (the cap is 32; fps_lean_offline parses exactly that config). By section 1.2 this is expected to
be a small win: the algorithmic fix is the one aimed at the decay.

### 4.3 In-DLL cost counters (always on) [E]
`perf_counters.h`: per category calls, TSC ticks and a racy maximum with relaxed atomics (two `__rdtsc` + three relaxed atomic adds per
measured scope; estimated ~0.05-0.1 ms per frame at a few thousand hooked calls [I]). Every 5 s the producer status tick writes two lines
to step5-producer.log (each field `<us per game frame>/<calls per game frame>/<max us of one call>`):

```
gpu-copy-step5;event=summary-producer-cost;part=1;window-ms=5000;frames=300;fps=60.0;hooks-us-per-frame=..;registry=res:N,heaps:H;
  tsc-mhz=..;u=us/n/maxus;setter=a/b/c;addr=..;draw=..;step5=..;draw-x=..;native=..
gpu-copy-step5;event=summary-producer-cost;part=2;frames=300;exec=..;descr=..;ee-pres=..;ee-seam=..;log=..;flush=..
```
* `setter` binding-setter observation (observeBinding), `addr` the registry lookups nested inside setter/descr, `draw` observeDraw +
  drawReturned, `step5` the producer's per-draw work incl. its replay recording, `draw-x` hud/phase observers (off), `native` the game's own
  Draw into the driver (reference), `exec` the Execute hook minus the native call, `descr` descriptor writes/copies/creations,
  `ee-pres`/`ee-seam` the ee-camera Present tick and render-thread sample, `log` ring pushes + feature-log writes, `flush` the diagnostics
  flush on the game thread (nested in exec).
* `hooks-us-per-frame` sums everything except `addr`, `flush` (nested) and `native` (the game's). At 60 fps the frame budget is 16,667 us; a value of
  several thousand us/frame means the hooks are the bottleneck. `registry=res:N` is the live game-buffer count the scans walked.

## 5. Results of this change (offline) [E]

Build `D:\b-fps` (Release, Ninja, -j2; dxgi.dll `D:\b-fps\bin\dxgi.dll` sha256 a43802dabac3..., after merging master ada510a; the d3d12.dll
forwarding proxy is not touched). Release ctest **311/311** after the merge (306/306 before it: 305 + fps_lean_offline).
* The producer's golden digests and fixtures (gpu_copy_step1_* incl. step5 continuous, HUD, radar, effects, world objects; e2e capture
  chain; native binding observer; registry) all pass unchanged with the index and the counters: the producer publishes the same frames
  with the same recorded command streams [E].
* fps_lean_offline: 166,833 checks (differential registry/observer, config, cost line). Timing per call (this machine):

| registered game buffers | setter `observed_address` old -> new | CBV / root-view `address_resource` old -> new |
|---|---|---|
| 1,000 | 4.1-5.2 us -> 0.064 us | 2.1 ms -> 0.26 us |
| 4,000 | 16.4 us -> 0.079 us | 34.9 ms -> 0.57 us |
| 8,000 | 32-46 us -> 0.085 us | 135-231 ms -> 1.3 us |

  At 500-1000 IA/root-view setter calls per frame, the old setter scan alone costs ~2-5 ms per frame at 1000 buffers and 16-45 ms at
  4000-8000 (= a 60 fps game dropping to ~15-25 fps); one CBV write at 4000 buffers costs a whole frame [E for the per-call cost, I for the
  per-frame call count and the game's buffer count: section 6 measures both].
* Not measured offline: the real buffer count of ACZ during a mission, the actual per-frame hook cost, the diagnostics flush latency on
  this disk, and whether the real SteamVR compositor contributes. Fixture success does not show the game's fps.

## 6. Live A/B plan for the driver (null driver only)

Preconditions: mem-guard start gate (commit <= 85%), a DLL set built from this branch (dxgi from `D:\b-fps\bin` or a fresh proxy build; d3d12.dll unchanged), `-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1'
-CockpitModel 'auto;anim:1'` plus the morning switches (same 31 keys as the install). Direct acz.exe launch (D-279), Mission 02 as usual.

1. **Run A (current config, new DLLs):** `vr-test-start.ps1 <morning switches>` (31 keys). Fly the mission start for **at least 6 minutes**
   (the archive has no null run longer than 140 s; headset runs decay after 20-75 s and reach 12-16 fps after 3-4 min). Keep flying
   (no pause; the null HMD window must not be in front, D-270).
2. **Run B (-Lean):** identical, plus `-Lean` (32 keys). Same route and duration.
3. Optional **Run C (old DLLs, step78)** for 6 min, to see whether the decay exists on the null driver at all with the pre-fix code
   (no cost line there; use fps-log-table.py).
4. Collect each run's `%LOCALAPPDATA%\acz-vr\logs` folder (step5-producer.log, xr-stereo.log, ee-camera.log, diagnostics*.log).
5. Tabulate: `python -I tools/live/fps-log-table.py "A=<dir>" "B=<dir>" "C=<dir>"` (game fps first/last, new/s) and
   `python -I tools/live/fps-cost-table.py "A=<dir>" "B=<dir>"` (per-window us/frame per category, fps, registry size).
6. Read the answer:
   * fps stays at 59-60 for 6 min in A and B, but C decays -> the registry scans were the cause (compare C's decay time with A's
     `registry=res:` growth);
   * A decays while `hooks-us-per-frame` grows with `registry=res:` -> a scan we did not fix; the category that grows names it;
   * A decays while `hooks-us-per-frame` stays flat (< 2,000 us) -> not the hooks: compare `native` (game's driver Draw cost) and the
     XR side (`gpu-blit-ms`, `blocked-ms-max`) and record commit % (memory pressure) over time from the sampler;
   * B vs A: the `log` and `exec` columns and fps quantify perf_lean (expected small).
7. Pass criteria: B and A hold >= 55 game fps for 6 min on the null driver; the stereo image (png-once) unchanged. Then a real-headset
   run decides the compositor question.

## 7. Growth root cause after D-336 (offline, step83 archive; D-347/D-349)

Evidence: `D:\acz-evidence-archive\step83\runA-logs` (step85 DLLs, XR on, ~400 s), `runC-step80-logs` (no XR, 50 s), the
`summary-producer-cost` lines (part 1: fps, hooks-us-per-frame, registry, setter/addr/draw/step5/draw-x/native; part 2: exec/descr/ee-pres/
ee-seam/log/flush, each `us per frame / calls per frame / max us`) and the diagnostics `summary` checkpoints (`pso` = one event per shader
stage of every created PSO, `execute_summary` = Executes).

### 7.1 What grows [E]
Per CALL, not per frame, independent of fps and with `registry=res:7` flat (so not the D-336 registry):

| run, t | exec minus flush, us per Execute | setter us per call | draw us per call | `pso` events (cumulative; checkpoint clock not aligned) |
|---|---|---|---|---|
| runA (XR) 10 s | 140 | 0.94 | 2.6 | 836 (first checkpoint) |
| runA 131 s | 271 | 1.67 | 3.9 | 1469 / 2150 (2nd / 3rd of 7 checkpoints) |
| runA 402 s | 616 | 4.32 | 3.0 | 4243 (last checkpoint) |
| runC-step80 (no XR) 0 s -> 50 s | 139 -> 195 | 0.70 -> 1.02 | 1.6 -> 1.9 | - |

`exec - flush` per call fits `a + b*N` with N = live PSO records (~events/2): 140 us at ~420 PSOs, 616 us at ~2100 -> b ~= 0.28 us per PSO per
Execute, a ~= 20 us. With XR the game thread also sees ~29 Executes per frame instead of 3 (exec 8.0 ms/frame at 10 s) and the diagnostics
flush runs on it (3.0-3.9 ms/frame; perf_lean was not set).

### 7.2 Root cause [E code, I for the live attribution]
`HookState::pipelines` gets one record per PSO the game creates; the game's pipeline cache is never released, so it only grows. Two lookups
walked every record whenever the address was not a direct map key:
* `pipelineFor_locked` (alias search over `canonical_address` + 6 `ancestor_addresses`): called by `hookCaptureCommandList`'s
  `metadataOwner` for the canonical + up to 11 graphics-list variants + 3 bases of EVERY list on EVERY ExecuteCommandLists (12-15 full walks
  per list; a list is never a PSO alias, so every walk runs to the end), and by `releaseMetadata` for every Release through a hooked
  root/heap/resource/PSO table - including the old root signature / descriptor heaps that `setRoot` / `setHeaps` release inside the setter's
  cost scope, and every Release by the XR runtime or producer on another thread while holding the hook mutex the game's setters wait on.
* `registeredOwnershipKey_locked` (map key + 20 `retirementKeys` per record): per key of a Release of an unregistered object on a hooked
  table.
PipelineRecord is ~1 KB, so each visited record is a cache miss; ~10-20 ns x 15 x N matches b above. Nothing in perf counters measured
`releaseMetadata` or the per-Execute list certification separately.

### 7.3 Fix [E offline only]
`PipelineKeyIndex` (d3d12_hook.cpp): per address, the number of records claiming it as alias / as key-or-retirement key, maintained at
the three production edit sites (PSO creation `try_emplace`, `invalidate()` erase, final-Release erase) and cleared by the shutdown drain;
an epoch + size check rebuilds it if the map was edited elsewhere (fixtures). "No record claims this address" is answered in O(1); any claim
still runs the original scan, so returned records (first in iteration order) and booleans are unchanged. Test builds cross-check every
index answer against the scan (`g_pipelineIndexMismatch`). `fps_growth_offline`: randomised differential (shared aliases, null ancestors,
replace/erase), incremental maintenance (one build), staleness, timing, cost line.

New counters (part 2 of the cost line, nested, not in hooks-us-per-frame): `rel` (releaseMetadata incl. native Release, any thread), `cert`
(hookCaptureCommandList at Execute), `pscan` (remaining whole-map walks + index rebuilds), and `objects=pso:N,lists:M`.

### 7.4 Not fixed / not verified
* Constant per-Execute cost: ~30 QIs + certification per list per Execute (`cert`) and the XR-side Executes on the game's hooked queue
  vtable; `CreationSightings::classify` walks 4096 entries per hooked Release (constant). Measure `cert`/`rel` before changing them.
* Not built or run here. The live run must show `pso:` growing while `exec`/`setter` per call stay flat, `pscan` ~0 calls per frame,
  and `rel` calls per frame (how many Releases the XR runtime drives through the hooks).

## 8. Residual fixed hook cost after D-350/D-351 (offline; step86 run X counters)
Evidence [E]: run X at ~395 s (`summary-producer-cost` part 2, us per frame / calls per frame): `rel=6111-6173/2497-2521`,
`cert=1640-1656/27`, `pscan=685-687/396-403`, `flush=3233-3270/27` (max 8.9-10.1 ms), `exec=5241-5296/27`; hooks-us-per-frame 8.6 ms.
Run Y (perf_lean) shows the flush is most of the X-Y difference.

### 8.1 Causes [E code, I attribution]
* `rel` ~2.45 us per Release: every Release through a hooked table of an object that is neither a PSO record nor a root creation
  (every list Release, untracked resources/heaps) ran `CreationSightings::classify`, a walk of all 4096 entries (64 KB). The ~30 QI
  outputs certification drops per list per Execute are list Releases, so certification alone drives ~27 x 30 = ~800 of the ~2500
  Releases per frame [I].
* `cert` 61 us per list: ~31 QueryInterface calls (canonical, 11 variants + identity, 3 bases + identity, stable, round trip) and their
  ~30 Releases through `hookedRelease` -> `releaseMetadata`, repeated on every Execute of an already certified list.
* `pscan` ~400 calls per frame at ~1.7 us: scans that STOP at a match, i.e. lookups of an address some record claims. The D-350 index
  only skipped unclaimed addresses; a claimed one still scanned to its record. Most likely caller [I]: `releaseMetadata` ->
  `pipelineFor_locked` for Releases through a split-identity PSO's canonical / ancestor address (not the map key), whose position in the
  iteration order moves back as the map grows (the creep 319 -> 677 us); `registeredOwnershipKey_locked` with a claimed key is the other.
* `flush` ~8 ms per actual flush (every 64 submissions; with XR's 27 Executes per frame that is every ~2.4 frames): the evidence file is
  opened, sought, written, flushed and closed once PER CHANGED reserved slot (reasons / outcomes / l3-counts) plus once per batch append.

### 8.2 Changes [E offline only; nothing built or run here]
1. `PipelineKeyIndex::Claim` (d3d12_hook.cpp): per address the number of claiming RECORDS (duplicates inside one record count once) and
   the XOR of their map keys. count 1 -> `pipelineFor_locked` returns that record directly (confirmed against the record; the scan's
   first-claimant answer is then the only claimant); `registeredOwnershipKey_locked` answers from the count without walking. Only
   addresses claimed by 2+ records (and index rebuilds) still scan. Test builds compare every answer with the scan.
2. (REMOVED, D-358) `ListState::CertCache`. The first version cached the interface set of the last successful certification and, on
   an Execute-seam hit, reused those addresses instead of the ~29 QIs + GetDevice. Astra's source review found it unsound: after
   certification `ListInterfaces` released its QI references, so the cache kept raw interface addresses it did not own and later
   dereferenced them (`vtable(c.variants[i])`) and used them after the cache-check mutex was dropped. COM allows independently
   reference-counted tear-off interfaces, so a fresh canonical IUnknown reference does not keep every earlier interface pointer alive.
   Every certification now again holds fresh owned QI references through every use (the pre-cache path, unchanged). The
   `fps_growth_offline` cert section checks the cache is absent: every successful Execute makes >= 29 QIs and one GetDevice, returns
   every reference, and each independently counted alias holds its own reference while it is used (including one released by its
   other owner between Executes).3. `CreationSightings::index`: open-addressing hash (8192 x uint16) over the append-only entries; observe/classify are O(1) with the
   same results (first free entry = `entries[filled]`); a broken append-only shape falls back to the walks; test builds compare.
4. `IOProvider::begin_batch/end_batch`, `FileIO` (diagnostics_log.cpp): one write session per `Log::flush`; consecutive
   append/overwrite calls on one file reuse one "r+b" handle (end-of-file seek for appends, fflush per call so results stay per call);
   size/rotate/reset/another path/end close it first. Same bytes, offsets and order; same records, same flush cadence and quotas, so
   the diagnostics are unchanged. perf_lean keeps its meaning (worker flush + volume cuts) and also benefits.

### 8.3 Expected (estimates from the counters, not measured)
| counter | run X | expected |
|---|---|---|
| pscan | 685 us / 400 calls per frame | ~0 (only shared addresses / rebuilds) |
| cert | 61 us per list | ~15-30 us per list (cache removed: same ~31 QIs + ~30 Releases, but each Release ~0.5-1 us after 1+3) |
| rel calls | ~2500 per frame | ~2500 (unchanged: certification still releases its ~30 QI outputs per list per Execute) |
| rel us per call | ~2.45 | ~0.5-1 (no 64 KB walk, no claimed-address scan) |
| flush | 3.2 ms per frame | lower by the evidence re-opens per flush (count unmeasured; FileIO.opens is test-visible only) |
| hooks-us-per-frame | 8.6-8.8 ms | ~5.5-7 ms (no cert cache: ~0.3-0.6 ms per frame more than the cached estimate) |

### 8.4 Not changed / live checks
* XR-thread Executes through the game's hooked queue (27 per frame) are unchanged; `flush` cadence is unchanged.
* Live run (null driver, step87+): pscan calls ~0, cert us per call, rel calls and us per call, flush us per frame and max, setter us per
  call over >= 6 min (does the 977 -> 1331 creep disappear?), hooks-us-per-frame with and without perf_lean, no new list refusals
  (`list_refused` outcome counts in diagnostics.evidence.log), VR image unchanged, diagnostics.log / evidence content still written.
* Risk class lifetime (board-policy section 2): the cert cache was removed after review (D-358); no interface address is retained across calls.
* `FileIO::held` (D-358): the path is copied before the open and the new handle is owned by RAII until a nonthrowing commit, so an
  exception between open and commit closes it; `FileIO.closes` balances `opens` (fault-injection check in `fps_growth_offline`).
