# VSEARLY: XR from the menus on, and the stage detected per flight (roadmap R1 prerequisite)

Status: implemented behind `vsearly:1` (a sub-option of `xr_terrain_stage`, no new config key; default OFF), offline-tested only. No game,
SteamVR or headset run has exercised any of it. Code: `src/acz-vr/flight_cycle.h` (pure core), `xr_flight_stage.{h,cpp}` (DLL: worker,
loader, producer switches), small hooks in `xr_stereo_proxy.cpp` (monitor: start gate, `auto` deferral, Limits, retry policy),
`xr_stereo_output.{h,cpp}` (two calls: `FlightStageHook::frame` before each frame's work, `recorded` after each terrain job),
`xr_owner.{h,cpp}` (`Limits::frame_stall_ms`), `xr_start.{h,cpp}` (`RetryPolicy::set_early`, `Diagnosis::clean_after_start`).
Test: `tests/acz-vr/flight_cycle_offline_tests.cpp` (ctest `flight_cycle_offline`).

With the token absent (or `vsearly:0`) no new line is logged and every path is the previous one: the monitor parses the vscreen tokens once
more but acts only on `vsearly:1`; `Limits::frame_stall_ms` stays 0 (the 250 ms rule), `RetryPolicy` is not switched, `Settings::flight_stage`
stays null (the Output's two new calls are behind it), and `xr_terrain_stage=auto` still detects once at XR start.

## 1. Effective only with

Config size: `xr_terrain_stage` holds up to 255 characters (buffer 256, config line limit 288 bytes; it was 99 and the day value
plus `;vscreen:1;vsearly:1` is 109). The other limits are unchanged: 2048 bytes and 32 keys per file, 16-32 characters per option token.


`vsearly:1` AND `vscreen:1` (the menus need the screen layer) AND `xr_hud_layer` (the screen's swapchain) AND `ee_camera_probe` (the Present
hook that counts presents and captures the screen). Otherwise the monitor logs `vsearly-config;effective=0;reason=needs-...` once and starts XR
as before. Example: `xr_terrain_stage=auto;env:1;props:1;trees:1;vscreen:1;vsearly:1`.

## 2. Session start in the menus

* Start gate (monitor thread, 100 ms poll as before): the producer's first StereoFrame as before, OR (since 2026-11-01, `vsstart:first`,
  the default; docs/world-render/launch-mode.md 3) the game's FIRST counted Present (`capture_configure` is called early so the Present hook
  counts). `vsstart:steady` keeps the original rule: >= 120 presents and >= 3 s since `armed`; `vsstart:lazy` = vsearly:0; `ACZ_XR_START`
  overrides. Line: `source-ready;...;start=early-game-presents;...;start-mode=first`.
* Source extent: no StereoFrame exists, so the eye swapchains are sized for 2560x1440 (the game's swap chain in every log), unless a frame was
  already published (then its extent).
* The frame loop in the menus: no producer frame -> `Output::pace` -> the vscreen hook (vscreen.md 2): `flight_age = -1` -> screen mode,
  capture requested on the first frame. Until the first captured image was copied the frame has ZERO layers (a legal OpenXR frame: SteamVR
  shows its own void for those ~2-3 frames); then one opaque quad. No projection layer is needed in the menus.
* First-frame budget: unchanged (5 s, first frame included). The first frame does less than in flight (no producer frame, no terrain).
  The terrain/props pipelines are now created in the FIRST FLIGHT frame mid-session, i.e. under the frame-stall limit below, not the
  startup budget.

## 3. Surviving loads, cutscenes and missions

* Progress limit: `Limits::frame_stall_ms = 8000` for this session (`Limits::valid` allows only 0 or progress_ms..10000). It replaces the
  250 ms rule in the FRAMES phase after the first frame (owner self-check and watchdog). Startup, READY wait, first frame and cleanup keep
  their budgets. Test 19's attempts died at `frame-cpu-ms-max=1968` / `end-late-ms-max=1936` (a game load holding the shared queue); 8 s
  covers that with margin. A real runtime hang is detected after 8 s instead of 250 ms (the watchdog never cancels a call either way).
* There is no other "no frames" rule that retires a session: without producer frames the consumer submits zero layers (`stale-zero`) or the
  screen; nothing fails the session (checked in `Output::pace`/`no_new_frame`).
* Retry after a running session ended (`RetryPolicy::set_early`): an attempt that had hooked and ended with `progress-deadline` or a session/
  instance loss, retired cleanly with nothing held, is retried (`retry=yes-early(after-start)`, 3 s, then the normal 5 s/10 s/15 s steps; the
  6-per-minute cap still applies: `yes-early(rate-capped)`). The R-5 slow-down clock restarts at that end (test 19 waited 60 s because its
  clock counted the 674 s the session had RUN). Not retried: an orderly runtime stop (SteamVR asked the session to stop / user quit),
  game exit, unclean retirement, other causes, `ACZ_XR_RETRY=0`.
* UNCAP (`uncap:1`): its Present override applied for the whole XR session; with vsearly the monitor enables it only while flight
  composites arrive (checked every 100 ms), so the menus keep the game's own frame limiter.
* Session state: FOCUSED/VISIBLE changes are logged as before; `shouldRender=false` frames submit zero layers. Headset standby: SteamVR keeps
  the session (inference) and xrWaitFrame may throttle; a throttled first frame is covered by the startup budget, later throttling by the
  8 s stall limit.

## 4. Per-flight stage detection (`xr_terrain_stage=auto` only)

Nothing is detected or loaded at XR start (in the menus the stage files are not resident). The monitor keeps the parsed options as a template
(`flight-stage-config` line) and starts one worker thread.

| event (owner thread, every XR frame) | rule | effect |
|---|---|---|
| flight start | the first FRESH flight composite while not in flight (`Output::last_new_ms_` changed) | request detection for flight N (worker) |
| detection done | worker: `detect_auto_stage` (the same bounded read-only scan, `terrain-auto*` lines) + load (stage, sea, atlas, env, props/trees, takeoff stage) with the template's options; unique generations (1001, 1003, ...; takeoff = +1) | `flight-stage-loaded` |
| install | the result belongs to the flight that is still running | `s_.terrain` replaced between frames, TakeoffSelector re-armed (a runway start of mission 2 enters k00a again), camera stage epoch bumped, producer switches set exactly as the monitor's one-time load (props dedupe, tac/mk360 flags, interp, game clouds, fxmem:1..5 through the shared `effects_stage_flags.h`) with the same `props-dedupe`/`interp-armed`/`tac-armed`/`game-clouds`/`fx-memory` lines; `flight-stage-installed` |
| late result | the flight ended (or a newer flight asked) before it finished | discarded, never drawn |
| no stage | no match / tie / load failure | no terrain this flight (the game's world image), `flight-stage-none` |
| flight end | no fresh composite for 8000 ms (results, debrief, briefing, hangar; also a pause or a load longer than 8 s) | terrain cleared (`s_.terrain = null`: nothing drawn between flights), producer switches cleared (dedupe, tac, interp, clouds, fx); the stage is PARKED; `flight-stage-ended` |
| next flight, same stage | the detection names the parked stage | the parked stage is reused (no reload; the fresh load is dropped), epoch unchanged |
| parked too long | 60 s without reuse | released (subject to the retire rule) |

So the second mission is detected again instead of keeping the first map, and a pause longer than 8 s only costs one scan (~0.5-0.8 s
measured for m02a) and reuses the parked stage.

The first fresh composites of every flight (until the scan and load finish, ~1 s plus props load) show the game's world image without our
terrain. A fixed stage id (`m02a;...`) with vsearly keeps that stage for the process (loaded at XR start as before).

## 5. Memory and lifetime (for review)

* Owner thread only replaces/clears the drawn stage, always between frames; the D3D12 backend borrows the CPU stage only inside a record,
  except the props init worker (`PropsGpu::async_ready` holds a reference while it runs).
* A stage WITHOUT props is freed as soon as it is replaced. A stage WITH props is kept RETIRED until a recorded job of a NEWER stage reports
  its props settled (ready or failed): the backend starts the next props worker only after the previous one left its running state (its
  ready/failed state is published after its last access to the runtime) and joins it first, so that report shows no worker still uses
  the retired stage. Unique props generations rule out the backend's (pointer, generation) caches aliasing a freed stage.
* A new XR attempt (new Output + backend) starts a new controller session: the drawn stage is parked (reusable); stages the previous backend's
  worker could still borrow are kept for the process (`kept-for-process`), never freed under a possible reader.
* Cost: one stage runtime is a few MB (atlas, env) plus its props (m02a.zvro 28 MB on disk). Normally at most current + parked + 1 retired.
  `publish_dedupe` keeps every published placement set for the process (existing WORLD-4 rule: ~2 MB per m02a install).
* The `summary-flight-stage` line (every 10 s) reports flights, installs, reuses, parked, retired, released, kept.

## 6. Not verified (inference until a live run)

The 120-present gate firing in the title/menu screens; SteamVR accepting the zero-layer and screen-only frames from the menus on; the
compositor stalls during loads staying below 8 s; detection succeeding ~0.5 s into a flight (the stage files resident); the flight-end
threshold against real pauses and cutscenes inside a mission (a cutscene that stops the world draws > 8 s ends the flight: harmless, one
rescan); the cockpit auto detection mid-session (it already detects lazily from the E6 block); the cost of the install on the owner thread
(dedupe sort of ~170k placements, freeing a stage).

## 7. Live check list (null driver first; config: day install + `;vscreen:1;vsearly:1` with `xr_terrain_stage=auto;...`)

1. `xr-stereo.log`: `vsearly-config;effective=1;reason=ok` once; `flight-stage-config` once; NO `terrain-auto` line before the first flight.
2. Menu start: `source-ready;...;start=early-game-presents;presents=N` (N small with `vsstart:first`, >= 120 with `vsstart:steady`) within
   ~1 s of the Launch Game click (steady: ~3 s); `start-attempt;n=1;outcome=hooked`;
   `summary-vscreen;mode=screen;copies>0`; SteamVR shows acz as the running app; headset: the title/menu on the screen quad.
3. Load stall survival: go menu -> briefing -> mission load: no `attempt-ended` with `progress-deadline` while `frame-cpu-ms-max` reaches
   ~2000 in the summaries (if one ends: `retry=yes-early(after-start);next-attempt-in-ms=3000`, not `slow-60s`).
4. First mission (02, runway start): `flight-stage-detect;flight=1`, `terrain-auto;...;stage=m02a`, `flight-stage-loaded;flight=1;stage=m02a;
   ...;takeoff=k00a:loaded(...)`, `flight-stage-installed;flight=1;stage=m02a`, then `terrain-takeoff;drawn=k00a;switch=to-takeoff-base` and
   `switch=to-mission;reason=cut` as in takeoff-stage.md; record the time from the first composite to `installed`.
5. Mission end -> results/menu: `flight-stage-ended;flight=1;had-stage=1`; the screen quad shows results; `summary-flight-stage;...;stage=none;parked=m02a`.
6. Second mission (a DIFFERENT one, e.g. 05): `flight-stage-detect;flight=2`, `terrain-auto;...;stage=m05a`, `installed;flight=2;stage=m05a`;
   the headset shows m05a's landscape (not m02a); `retired`/`released` counts move after `props-ready`.
7. Replay mission 02 from the menu after a short break (< 60 s): `installed;...;reused=1`.
8. Pause > 8 s in flight: `flight-stage-ended` then on resume a new detect + `reused=1`; nothing else changes.
9. Headset off/standby for 1 min in the menus and back: the session stays or comes back (`yes-early`); record the session-state lines.
10. Game exit: NO exit lines are expected (any mode): production has no orderly teardown hook (J-xr-stereo-output-findings.md, Stop:
    `shutdownDiagnosticOwnership(false)` is never called; DllMain detach is never used), so the owner runs until the process ends and
    SteamVR sees the client disappear. V1/V1b (vsearly) and V1c (no vsearly) all end on periodic summaries; the only `retire` lines in the
    archive are progress-deadline retirements (test 19). Check instead that SteamVR drops the app and no acz process stays.

## 8. Step95 live batch V (D-367 notes 62665748) read against the logs

* V1 (menu walk only: main menu, campaign, cinematic, map, overview, briefing, hangar; NO mission flown): XR from the menus PASS; step 3
  `waiting;matched=0;polls=0` and E6 `locks=0` are what every run shows before a mission (no DrawCPT cockpit draw exists in the menus);
  `flights=0` is correct there.
* V1b (route to mission 02, Esc skip): `runV1b-logs` HAS `step5-producer.log` and step 3 `continuous` (published=302); the per-flight path
  ran: `flight-stage-detect;flight=1` 80 ms after the first producer frame (294925859), `terrain-auto;stage=m02a;best=24;second=4`
  (decisive, 11 MB), `flight-stage-loaded;...;takeoff=k00a:loaded(props-loaded);load-ms=187`, `flight-stage-installed;flight=1;stage=m02a`
  (294926046), `summary-terrain;terrain-frames=387`, `props-ready`, decouple 120 composites/s; the game exited ~12 s later. No
  `terrain-takeoff` switch: the flight started at (-16981, 3968, 124460) in the mission area (after the Esc skip), not on the runway.
* So the early start does not keep the capture from arming; it arms when a mission draws the cockpit, as without vsearly. Not yet seen:
  a second mission, a runway start with vsearly, a flight end.

## 9. Live batch Y2 (step99): fxmem with vsearly

Cause (verified in code): with vsearly + `auto` the monitor loads no stage, so its block that sets the stage-bound producer switches and logs
`fx-memory`/`interp-armed`/`tac-armed`/`game-clouds` never runs; the per-flight install had its own copy of the switches, written before
fxmem:5 existed: fxmem:4/5 only set the capture switch (hence only `fx-smoke-frame` lines), no explosion switch, and nothing was logged.
Fix: one switch table `effects_stage_flags.h` (stage_effects / apply_stage_effects / fx_memory_line) used by BOTH paths; the install logs the
same lines (`source=flight-stage` on interp/tac). The fxmem:4/5 smoke GPU resources then come from the producer's late creation on its first
worker tick after the switch (`fx-smoke-ring;...late`), once per process. Fixture `flight_stage_adapter_offline`: fxmem:4 and fxmem:5
installs set the switches and log `fx-memory;requested=4|5` once; a rollover clears them.
Live check: Y2 again with vsearly: `fx-memory;requested=5` right after `flight-stage-installed`, then `fx-smoke-ring` (late) and the
explosion candidates `542a11d7...rep>0` as in Y2b.
