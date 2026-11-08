# POV: the game's HUD-only and chase views in VR (ROADMAP R5, user wish D-362)

Status: offline research + phase 1 code (`pov:1`, default OFF). Nothing here is live-verified. Code: `src/acz-vr/view_mode.h`,
keep-alive in `flight_cycle.h` / `xr_flight_stage.cpp`, wiring in `xr_stereo_proxy.cpp` (option), `ee_camera_probe_win.cpp` (view mailbox),
`xr_stereo_output.cpp` (one block before the flight-stage call). Tests: `tests/acz-vr/view_mode_offline_tests.cpp` (ctest `view_mode_offline`).

## 1. Sources

Live batch W1 (step96, D-369; `D:\acz-evidence-archive\step96\runW1-logs`, `runW1-shots`; notes.md 344e667): three flights, the driver pressed
the game's view key V several times. Logs used: `ee-camera.log` (1 Hz `obs` lines: look p/y/r, heading, aircraft x/y/z, camera x/y/z, camera
rot p/y/r; `timeline`; `e6-locate`; `camera-epoch`), `step5-producer.log` (`summary-producer-cost`, `world-evidence decision-summary`),
`xr-stereo.log` (`flight-stage-*`, `look-stick-gate`, `summary-vscreen`, `summary-hud`, `summary-cockpit`, `summary-cockpit-auto`,
`summary-camera-admit`). Wall clock = log ms aligned on the memory-stop line (03:00:31.7). Analysis scripts were throwaway stdlib Python.

## 2. What the game does per view (evidence)

The view key cycles **cockpit -> chase -> HUD-only -> cockpit** (W1: flight 1 HUD-only at the takeoff (shot 14) -> V -> cockpit 02:48:19;
flight 2 cockpit on the runway 02:58:22 -> V -> chase 02:58:35 (shot 23) -> takeoff skip 02:59:02 (still chase) -> V -> HUD-only 02:59:13
(shot 24, 03:00:14) -> V -> cockpit 03:00:29). The game keeps the chosen view across a RESTART; the takeoff intro (02:58:16) uses the chase rig.

| signal | cockpit | HUD-only | chase | mission-end orbit / menus |
|---|---|---|---|---|
| E6 camera - aircraft (aircraft frame: ahead, up) | +36.3, +3.575 (j35j eye, `cockpit-model eye=0,3.575,-36.3`) | **identical to cockpit** (03:00:13: +36.3, +3.575) | **-76.0, +17.0** parked (02:58:35), -79.5/+16.5 intro, **-83.4, +17.0** at speed (02:59:02): a spring rig, 15-17 m behind, 3.4 m above | orbit 111 units, look 3.14 (02:52:43, 02:58:00); menus: all-zero block |
| E6 camera rotation | aircraft attitude + look | the same | = aircraft attitude (pitch 0.040 parked like the cockpit; yaw = heading) | orbits |
| E6 locator (`ee_e6_locator`) | valid | valid (relock 02:59:14 in HUD-only; flight 1 locked 02:48:08.7 in HUD-only) | **rejected `eye-distance`** (77.9..85.7 > 60; also eye-direction): 3196 -> 3541 rejections during chase | rejected (`eye-distance`, `aircraft-at-origin`) |
| producer frames (`summary-producer-cost frames=`) | 300 / 5 s | **0** | **0** (150 in the window of the switch) | 0 |
| why | a frame starts at the first DrawCPT draw (`members-by-class-definitions start=first-DrawCPT-draw`) | no DrawCPT draws -> no frame; the world draws are seen and refused (`world-early-backoff-hold` +500k, `world-record-depth-scale` growing) | same | - |
| HUD (`summary-hud hud-frames`) | 300 / 5 s | 0 (HUD is only captured inside a frame) | 0 | 0 |
| cockpit model (`summary-cockpit model-frames`) | 600 / 5 s | 0 (no composite) | 0 | 0 |
| cockpit-auto (`samples=`) | grows | frozen (fed per composite) | frozen | frozen |
| camera admission (`summary-camera-admit admitted-frame`) | 300 / 5 s, lock/stage epoch current | 0 (nothing to admit) | 0; the lock is LOST after 1 s (a relock = new lock epoch) | 0 |
| look-stick gate | block | pass (gate follows composites) | pass | pass |
| screen quad (`summary-vscreen mode=`) | flight | **screen** after vsenter 500 ms (the game's flat image) | **screen** | screen |
| flight stage | in flight | **ended 8 s after the last composite** | **ended 8 s after the V** | ended (correct) |

Correction to the W1 notes: `flight-stage-ended;flight=2` (02:58:43.28) was NOT the takeoff skip: the V into chase came at 02:58:35 while still
parked on the runway (`look-stick-gate;state=pass` 02:58:35.70, producer frames stop, chase geometry); the skip came at 02:59:02. Flight 3 (03:00:29)
was a full re-detection + reload (187 ms) caused by the HUD-only stretch, not a new flight.

Signatures used by `view_mode.h` (`classify`, `Machine`): fresh composite (age <= 250 ms) = **cockpit**; else E6 eye geometry (15..60 units, along
the aircraft forward within 25 deg, yaw relation <= 0.6 rad) = **HUD-only**; else camera 40..260 units behind and 0..80 above, lateral <= 35 % + 10,
look centred, camera yaw = heading = **chase**; else **other**. Non-cockpit candidates must hold 300 ms. Unknown: the chase camera while looking
around (classified other; not observed) and the E6 block after a quit to the menus from a non-cockpit view.

## 3. Phase 1 (implemented, `pov:1`, default off)

* Option: `pov:<0|1>` inside `xr_terrain_stage` (no new key), removed before the other parsers (a bad value is logged and removed too); the day
  value + `;pov:1` is 135 characters (limit 255). Line `pov-config` (prerequisites: `vsearly:1` + stage `auto` (the flight stage), `e6loc:auto|content`).
* Probe (Present tick, e6loc auto/content): with pov on, the RAW E6 sample of the locked block (else the last locked block, so chase still
  reports while the locator rejects it) goes into `view_mailbox()` (92 bytes per tick, the locator's guarded read; read-only).
* Consumer (every XR frame, owner thread): `Tracker` reads it (stale after 500 ms = no sample), runs the machine with the composite age, logs
  `pov-mode;from=..;to=..;geom=..;ahead/up/side/dist;composite-age-ms;keep-alive;cockpit-model` on every change and `summary-pov` every 10 s.
* Flight stage keep-alive: while the mode is HUD-only or chase, `alive_ms` = now; the 8 s end rule counts from max(last composite, alive). It
  never STARTS a flight. The raw fallback sample is not check_sample'd, so `classify` itself rejects a heading outside the locator's domain (`heading-range`; a huge finite heading used to hang the wrap loop; review w2-3 finding 1) and `wrap` is a bounded fmod reduction. Frozen cap: no aircraft motion for 90 s (parked/paused; the last position and motion deadline survive missing/invalid samples, only a real position change advances it; review w2-3 finding 2) -> no keep-alive -> the old 8 s rule. Orbit/menus/zero -> other ->
  unchanged. Effect expected live: W1 flights 2+3 become one flight; no reload when returning to the cockpit.
* Gates for phase 2: `draw_cockpit_model(mode)` (false in HUD-only/chase), `flight_view(mode)`. Not wired: today no composite exists in those
  views, so there is nothing to gate (a gate in the composite path would be dead code).
* Rendering is unchanged: HUD-only and chase still show the game's flat image on the screen quad (vscreen) after 500 ms.

## 4. Design: VR rendering of the two views (phase 2, not implemented)

Superseded in detail by `other-povs.md` (2026-10-30: the camera source per view, the rigs, routes incl. cinematics, the stage plan, and the
stage-1 census `povc:1` / rig dry run `povrig:`).

The blocker is the producer: no DrawCPT draw -> no frame -> no HUD capture, no world-object replay, no camera admission.

**P2-a producer frame without the cockpit (both views).** With pov:1 and the consumer's view mode HUD-only/chase (a producer flag, like the
stage-bound switches), open a by-class frame at the first admitted DrawACM (world object) draw of a generation when no DrawCPT draw came for
N generations; its camera = the record's C matrix (already validated per member: `camera=from-C`), cockpit members = 0, HUD capture as today
(scene-barrier end). The world-object gate `camera must equal cockpit axes and lens` becomes `equal to the frame's first ACM C`. Evidence needed
first (live check 4): the per-generation DrawACM count and the HUD draw count in both views (a read-only `world_observe` window).

**HUD-only (P2-b).** Camera = the E6 eye (identical to cockpit: the existing terrain/props/clouds/objects path at 5 u/m, `world_scale.h`);
`draw_cockpit_model=false`; the game's replayed cockpit band is absent anyway; HUD on the existing quad (scale/placement unchanged). Comfort =
cockpit (the head moves about the eye point, 6DoF, IPD at 5 u/m).

**Chase (P2-c).** The game's chase rig is a spring 15.2..16.7 m behind and 3.4 m above, rotating with the aircraft. In VR:
* Seat reference: a chase seat = aircraft position + offset (0, +17, +80 units = 3.4 m up, 16 m back) in a heading-only frame, the head pose
  added on top (6DoF bubble, radius clamp ~0.5 m before fading to the HUD quad). Default `level` rig: yaw follows the heading through a
  critically damped spring (time constant ~0.4 s), pitch and roll NOT applied (horizon stays level; the aircraft banks in front of the user).
  Option `game` = the E6 camera pose exactly (the game's own rig incl. roll; for users who want it). No forced translation beyond the
  aircraft's own forward motion (same as the cockpit); the spring removes the camera's speed-dependent stretch jumps.
* World: terrain/props/clouds/sky from the seat camera (any camera works: our bakes); world objects replayed with substituted cameras
  (P2-a); the game culls to its own chase frustum, which is wider than the cockpit's around the aircraft but objects behind the seat are missing [I].
* Player aircraft: in chase the game draws the player's aircraft as a world object (it is visible in shot 23) [I: DrawACM, not DrawCPT, not
  verified]; if P2-a replays it, nothing else is needed; fallback: draw the baked exterior model (`aircraft-auto.md`, the type is known from the
  cockpit-view eye offset) at the E6 aircraft pose (position + heading/pitch/roll from rot), scale 5 u/m (Draken 76.7 u = 15.35 m).
* HUD: the game's chase HUD (reduced, no pitch ladder) on the quad; markers/mk360 unchanged.

## 5. Live check list (exact)

Config: the day install + `;pov:1` appended to `xr_terrain_stage` (needs `vsearly:1` and stage `auto`). Null driver, SteamVR first.
1. `xr-stereo.log`: `vscreen-option` / `vsearly-config` as before; `pov-config;ok=1;pov=1;detail=ok` once; no `terrain-disabled`, no `bad-stage-option`.
2. Start a mission, in the cockpit view: `pov-mode;from=unknown|other;to=cockpit` within 1 s of the first composite; `summary-pov` every 10 s with
   `frames=cockpit:` growing, `geom=eye:` growing, `no-sample` not growing (else the probe feed is absent: e6loc:probe or no E6 block).
3. Press V once (chase): `pov-mode;from=cockpit;to=chase;geom=chase;why=behind-above;ahead=-76..-85;up=~17` ~0.55 s later; the screen quad
   shows the game's chase view (`summary-vscreen mode=screen`); `e6-locate` may log `lost ... eye-distance` (expected).
   Stay > 15 s: NO `flight-stage-ended` (W1 logged one after 8 s); `summary-pov keep-alive-frames` growing; `alive-ms` advancing.
4. Press V again (HUD-only): `pov-mode;from=chase;to=hud-only;geom=eye`; again no `flight-stage-ended` for > 15 s. Optional read-only evidence for
   P2-a: arm `world_observe` and record `acm`/`cpt`/HUD draw counts per list in both views.
5. Press V again (cockpit): `pov-mode;to=cockpit` on the first composite; NO new `flight-stage-detect`/`flight-stage-loaded` (the same flight
   continues; W1 logged flight 3 here); terrain drawn immediately (`summary-terrain terrain-frames`).
6. Mission end (MISSION FAILED orbit or results): `pov-mode;to=other` and `flight-stage-ended` ~8 s after the last composite (unchanged).
7. Parked on the runway in chase for > 100 s (takeoff loop): `flight-stage-ended` ~98 s after the last aircraft motion (frozen cap).
8. Pause (Esc) in chase for > 100 s: the same frozen-cap end; resume -> cockpit composites start the next flight (stage re-detected).
9. Quit to the menus from the chase view: `pov-mode;to=other` (zero block) within ~1 s and `flight-stage-ended` 8 s later; record what E6 holds.
10. Costs: hooks us/frame unchanged within noise (one 92-byte read per Present tick); no new errors.
Not shown by any of this: stereo rendering of HUD-only or chase (phase 2).
