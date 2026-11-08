# Other POVs in VR: chase, HUD-only, in-engine cinematics (design + stage 1)

Status (2026-10-31): stage 2 (pov:2) verified in the step149 null run (section 10); stages 3 + 4 (pov:3: HUD-only, in-engine cinematics, the drawn
vignette) implemented with offline tests only (section 11). Earlier status (2026-10-30): design + stage 1 code, offline tests only. Nothing here ran in the game, SteamVR or a headset, and nothing was built
in this task (tests written, not yet compiled or run). Stage 1 changes NO rendering: it adds the camera rig, the route table, the comfort
vignette maths and a one-shot census, all behind sub-options of `xr_terrain_stage` (default off). The stereo render of the chase view is stage 2
and is blocked on the census evidence (section 5).

Code: `src/acz-vr/pov_render.h` (pure), glue in `xr_stereo_output.cpp` (`frame()`, next to the pov:1 tracker), `xr_stereo_proxy.cpp` (options),
`ee_camera_probe_win.cpp` (ACM census request poll), `ui3d_census.h` (one `acm-a` line per distinct camera), `view_mode.h` (the tracker step
carries its sample). Tests: `tests/acz-vr/pov_render_offline_tests.cpp` (ctest `pov_render_offline`), `ui3d_census_offline`
(`test_acm_distinct_cameras`). Builds on `pov.md` (the classifier, pov:1) and follows D-291: only genuine 3D world render routes, no
stick-view/HMD look mapping.

Labels: **[E]** read from an archived log, **[I]** inference, **[T]** shown by an offline test only, **[U]** unknown.

## 1. What the game camera does in each view (evidence)

Sources: `D:\acz-evidence-archive\step96\runW1-logs` (V pressed several times; `pov.md` section 2), `step83\runY3-logs` (pov:1 PASS: cockpit ->
chase -> HUD-only -> cockpit), `headset-2026-10-30-session\live-2210-step143` (day config without pov:1: no pov lines, no non-cockpit evidence).

| view | how it is entered | E6 camera vs aircraft [E] | E6 rot [E] | producer frames today [E] | today in VR |
|---|---|---|---|---|---|
| cockpit | V cycle | 36.3 ahead, 3.575 up (j35j eye) | aircraft attitude + look | yes (DrawCPT opens) | stereo world + cockpit model |
| chase | V from cockpit | 76.0 behind / 17.0 up parked, 83.4 / 17.0 at speed (a spring), 85.7 in Y3 | = aircraft attitude: camera axis PARALLEL to the aircraft (runway: pitch 2.3 deg), not aimed at it: the aircraft is 10.3 (runway) / 11.5 (flight) deg below the view centre, 12.6 deg off the camera axis | none | flat vscreen after 500 ms |
| HUD-only | V from chase | identical to the cockpit eye | identical to cockpit | none | flat vscreen |
| takeoff intro (m02 runway) | mission start | 79.5 behind / 16.5 up (the chase rig) | pitch 0.065 | none | flat |
| mission-end orbit | MISSION FAILED / ACCOMPLISHED | 111 units from the aircraft, orbiting, look 3.14 rad | orbiting | none | flat |
| menus / hangar | - | zero block (`aircraft-at-origin`) | - | none (UI3D has its own path) | vscreen, UI3D layers, u3a room |
| pre-rendered FMV | story movies | [U] (the block holds the last values or zeros) | [U] | none (movie shader key) | flat |

Producer side in chase [E, step96 `step5-producer.log` `world-evidence decision-summary`, cumulative counts every 5 s]: the scene DrawACM_v
draws are still recorded by the game in chase and reach the early-open path, but (a) `world-early-backoff-hold` grows ~100,000 per 5 s (frames
opened by a world draw are never adopted by a DrawCPT member, abandoned, and the opener backs off: `step5WobjEarlyOpen` / the
`f.early&&!f.adopted` branch of the submission), and (b) `world-record-depth-scale` early refusals grew 39,740 -> 106,733 between 296,828 and
296,863 s (the takeoff-intro + chase window [I: attribution by time only]): records with `+360 != 1.0` reach the opener there, which the cockpit
view never shows. Which draws those are (the player's own aircraft? an exterior LOD with the cockpit's 16.0?) is [U]: section 5's census answers it.

## 2. Getting the game's actual camera for any POV

Two sources exist; the design uses both, with distinct roles.

**(a) The draw stream: the DrawACM_v A matrix (authoritative render camera).** Every 3D world draw carries the 416-byte SrtData record with
A (+0) = world -> PS2 screen (view x projection), shared bit-identically by the scene draws of a frame (flight [E], hangar [E: 620/640 `A=same`]).
`gpu_copy_step5_world.h` already recovers the camera from A alone (pseudo clip `Vp = [(A0-2048 A3)/258.048 | (A1-2048 A3)/225.792 | A2 | A3]`,
eye by Cramer on columns 0/1/3, orientation and lens tanX/tanY like the cockpit VP, depth `zs = Z0 + b w`), and the early-open path does it
WITHOUT a cockpit (`camera_setup(raw, nullptr, ...)`, `f.gameEye`); `ui3d_hangar.h decompose()` does the same for the hangar's k400 682.
This is the camera the game actually rendered the frame with, frame-correlated, including the chase spring, cinematic cuts and the lens. It is the
camera for replayed objects (they ARE these draws) and the reference for the per-eye substitution `A'_e = A + D_e x S`.

**(b) The E6 memory camera (classifier, cross-check, terrain fallback).** `cam` + `rot` (pitch/yaw/roll) of the E6 block, read every Present tick.
The locator rejects non-eye geometry (`eye-distance`), so pov:1 publishes the RAW sample of the last locked block to `view_mailbox()`; that feed
exists for chase, HUD-only and the orbit [E: Y3]. It needs a lock first: a session that enters flight already in chase (the game keeps the view
across a restart) has no lock and no E6 camera until the first cockpit/HUD-only moment [I from the locator rules]. E6 carries no lens and no
proof that it equals the rendered camera; the census compares it with A.

**Rule (stage 2):** render camera = A of the frame's first scene-target ACM draw (decomposed); E6 cross-checks it (position within a few units,
axes within `game_cam_max_deg`) and feeds the classifier; when a frame has no scene ACM draw (empty sky) the terrain uses the E6 camera of the same
frame (the cockpit path's existing hybrid rule, `CamMode`). Cinematic views use the same rule; FMV has neither and stays flat.

## 3. Rendering the stereo world around that camera (head pose on top)

The cockpit path already renders "game camera + head": `eye_world(cf, head_origin, upm)` puts each eye at the camera plus the head offset in the
camera's axes (5 units per metre, `world_scale.h`), with the head orientation on top; the producer does the same for objects (`D_e = VP_e -
VP_ref`). For other views the only new decision is WHICH camera frame (`cf`) the head rides on. `pov_render.h` defines it as a **rig** in the
E6 Euler terms the terrain already consumes (`camera_frame(campos, pitch, yaw, roll)`), so the terrain, props, clouds, markers and the per-eye
object substitution can all take the same rig sample:

| rig (`povrig:`) | position | orientation | use | comfort |
|---|---|---|---|---|
| `game` | the game camera | the game camera (incl. its pitch and any roll) | exactly the flat picture, in stereo | worst: the horizon pitches/rolls with the aircraft, the chase spring's jerks |
| `level` (recommended for chase) | the game camera (the game's own spring) | yaw = critically damped spring to the camera yaw (`povtc`, 400 ms); pitch = roll = 0 | chase | horizon always level; the aircraft pitches/banks in front of the user, who looks up/down with the head; hard turns lag ~0.4 s |
| `anchor` (always for cinematics unless `game`) | a world point latched at entry that follows the game camera with a 2 s time constant; re-latched (blink) when the camera is > 400 units (80 m) away | yaw spring 1 s, level | orbit, intros, scripted cameras | the user stands still in the world; scripted camera moves become slow drifts or blinks |

Cuts: a camera move > 20 u/ms x dt + 200 units or a yaw step > 45 deg in one step re-latches the rig at the new camera (no spring through a cut);
a route change or a gap > 250 ms re-enters. Every entry/cut/re-latch gives a 150 ms full **blink** (vignette 1), hiding the teleport.

Comfort vignette (`povv`, default 0.6): strength = povv x smoothstep(30, 120 deg/s) of the rig's own rotation rate, immediate attack, 250 ms
release; the cockpit is unaffected. Stage 2 shader: a radial darkening of the eye image outside tan radius `r = lerp(1.0, 0.45, v)` (head-locked,
in the composite pass after the world). Further options (not implemented): chase dolly (`povd`, move the rig forward along the camera axis so the
aircraft is bigger/closer, the u3a precedent), a fixed "level + no yaw spring" world-locked chase for the strongest stomachs.

The HUD in chase is the game's reduced chase HUD; if the producer's HUD capture runs in no-cockpit frames it stays on the existing HUD quad.
The player aircraft in chase is drawn by the game [E: shot 23] as world geometry [I: DrawACM_v; not proven]; if it is one of the scene ACM draws,
the per-eye replay draws it in true stereo at its real position with no extra work. Fallback if not: the baked exterior model at the E6 aircraft
pose (`aircraft-auto.md`), not planned unless the census says so.

## 4. How the classifier switches (routes)

`view_mode.h` decides the MODE (cockpit by a fresh composite <= 250 ms, immediately; HUD-only / chase / other by the E6 geometry held 300 ms).
`pov_render.h Router` maps mode + geometry to a ROUTE:

| mode | condition | route | renders (stage) |
|---|---|---|---|
| cockpit | - | `cockpit` | existing path |
| hud-only | E6 sample with eye geometry | `hud-only` | stereo world from the E6 eye, no cockpit model (stage 3) |
| chase | E6 sample with chase geometry | `chase` | stereo world from the rig (stage 2) |
| other | E6 `other` geometry, camera <= 2,000 units (400 m) from the aircraft, block MOVING within the last 1 s | `cinematic` | anchor rig (stage 4) |
| other / any | no sample, zero block, invalid, far camera, block frozen > 1 s, unknown | `flat` | the game's picture on the vscreen (unchanged) |

Stage-2 caveat (must be handled when no-cockpit frames are published): the mode machine treats ANY fresh composite as the cockpit. Published
chase frames would flip the mode back to cockpit. The producer must tag no-cockpit frames (e.g. `diag_start` 3 = opened by a world draw, never
adopted, or a new `no_cockpit` flag) and the machine must use "fresh composite WITH cockpit members" for the cockpit rule. Also: the vscreen's
`vsenter` (500 ms without a flight frame) keeps covering every `flat` route and every gap; a route change chase -> flat never shows a black frame.

## 5. Stage plan

**Stage 1 (this task, implemented, dry run):** options, route table, rigs, vignette, census, ACM request; logs only.

**Stage 2 (chase in stereo; after the census):** in `step5WobjEarlyOpen` + the submission:
1. A producer flag (atomic, set by the consumer from the route, like the stage-bound switches) `pov_frames = chase`. While set: no early-open
   back-off hold for the scene list; an early-opened frame that no DrawCPT member adopted is PUBLISHED instead of abandoned, tagged no-cockpit,
   with `lens` = the A camera's tanX/tanY, `gameCam` axes + `gameEye` from A, no cockpit members, no cockpit FOV rule beyond the HMD FOV.
2. The `world-record-depth-scale` refusals: admit the chase records the census identifies (e.g. a second accepted +360 value with its own depth
   model) or keep refusing them (then those draws are missing; visible in the census as which B groups refuse).
3. Rig substitution: the consumer publishes the rig sample (seqlock, with a sequence) every XR frame; the producer's early open reads it once per
   frame and builds `VP_e` from rig + head instead of game camera + head (`D_e = VP_e - VP_ref` with `VP_ref` = A's camera: unchanged maths);
   the frame carries the rig sample it used, and the consumer's terrain uses THAT copy (one camera for terrain and objects, the CAM-EPOCH rule).
4. Consumer: route `chase` composes the published frame without the cockpit model (`draw_cockpit_model(chase) = false` already exists), the
   vignette in the composite pass, the HUD quad as today. The world image (the game's own chase picture) is NOT used as the window plane (it
   contains the flat aircraft); with terrain on it is fully covered.
Checks: `xr-stereo` `summary-pov-rig` + new `pov-frames=` counts, the producer's `world-early` reasons, XR png dumps in chase.

**Stage 3 (HUD-only):** the same publication with the E6 eye camera (A = the cockpit eye already [E: identical geometry]); `level` is not needed
(head 6DoF about the eye, like the cockpit).

**Stage 4 (cinematics):** the same publication for `cinematic` routes with the anchor rig; any refused frame falls back to flat for 1 s.

## 6. What cannot work (stays flat or degraded)

* **Pre-rendered FMV** (story movies; the movie shader key, no ACM draws, no E6 motion): a 2D video. Stays on the vscreen (it may be made
  bigger/curved; never 3D). The route is `flat` by construction (zero/frozen block, no scene draws).
* **Screen-space effects pre-projected by the EE** (ps2_v quads with real depth: glows, lens-flare sprites, the hangar floor shadow): a camera
  substitution cannot move them; only the fx_sprite_replay route (re-projection through the recovered camera) can, per effect class.
* **Geometry the game culled for its own camera**: the game culls to the flat chase frustum; turning the head shows our own terrain/props/clouds
  (camera-independent bakes) but no objects outside that frustum (other aircraft, ground targets behind the user) [I].
* **The game's chase spring and roll** are only kept with `povrig:game`; `level` and `anchor` deliberately drop them.
* **Cinematic cameras far from the aircraft (> 400 m)**, letterboxed sequences, fades through the cb10 fade bit (refused by `cb10_check`): flat.
* **A session that starts in chase** has no E6 lock (section 2b) until the first cockpit/HUD-only moment.
* **Menus** are not this path (UI3D layers / u3a room).

## 7. Stage 1 implemented

Options (inside `xr_terrain_stage`, removed before the other parsers; a bad value is logged and the whole set ignored; need `pov:1`):
`povc:<0|1>` census, `povrig:<off|game|level|anchor>` rig dry run, `povv:<0..1>` vignette (0.60), `povtc:<50..3000>` level yaw spring ms (400).
Config line: `xr-stereo;event=pov-render-config;ok=1;povc=1;povrig=level;povv=0.60;povtc=400;detail=ok;prerequisites=ok;render=unchanged(stage-1-dry-run);census=...`
(`prerequisites=needs-pov:1` and everything off without pov:1).

Every XR frame while pov:1 runs (`Output::frame`): `Driver::step` = Router + Rig (+ census). Lines (`xr-stereo.log`):
* `pov-census;n;mode;visit-ms;route;why;sample;geom;look;air;hdg;cam;rot;ahead;up;side;dist;fwd-to-air-deg;air-pitch-deg;rig=<mode>:<why>;rig-cam;rig-rot;vig;acm=requested|no`
  at every mode change and at 1 Hz for the first 6 s of a visit, then every 10 s; at most 400 per session.
* `summary-pov-rig;rig;route;route-frames=cockpit,hud-only,chase,cinematic,flat;rig-valid;enters;cuts;relatches;rate-max-dps;vig-max;vig-frames;povv;povtc;render=unchanged(stage-1-dry-run)` every 10 s with `povrig`.
* ACM census: the first visit of each mode (cockpit as the reference, chase, HUD-only, other; at most 4) increments `acm_request()`; the probe's
  next 5 s window logs `ee-camera;event=pov-census-acm;armed=1;request;arms;windows=14;source=povc:1;read-only` and arms the existing READ-ONLY
  ui3d ACM census (ui-3d.md 8.4: one sampled frame per second, 60 frames, `kind=frame|acm|acm-a|acm-b|other` in `ee-camera.log`) for 14 windows
  (~70 s). The census now prints `acm-a` for the first draw of every distinct camera of a frame (<= 4), so an offscreen pass first no longer hides
  the scene camera. The arm files work as before; with povc off the poll is exactly the previous rule.
Tests [T] (`pov_render_offline`): options (day value + tokens fit 255; byte-identical without them; bad values), routes on the real W1 samples
(cockpit, HUD-only, runway/flight chase, orbit moving vs frozen, zero block, far camera), the game rig exact, the level rig (no overshoot, the
closed-form spring at 396 ms, the +-180 deg seam, a slower povtc, gap re-entry), cuts (position and 60 deg yaw), blink, anchor follow (63 % in
2 s) and re-latch, the vignette (0.452 at 91 deg/s, none at 20 deg/s, povv:0), census lines per visit / requests per view / the 400 cap / line
length, the runway-chase line (`ahead=-76.0;up=17.0`, `fwd-to-air-deg=12.61`, `air-pitch-deg=-10.30`), the probe poll, the tracker's new fields.

## 8. Null-run census (next run; exact steps)

Config: the day install's `xr_terrain_stage` value + `;pov:1;povc:1;povrig:level` (206 characters with the step143 value; limit 255). Needs the
day keys `ee_camera_probe=true`, stage `auto`, `vsearly:1` (all in the day config). Null driver, SteamVR first, memory gates as usual.

Run `runK3ag.ps1` (mission 03 air start, EnsureCockpit taps V until a cockpit flight is detected, then the autopilot) with
`-AP '-Mode Cruise -Total 150' -PostWait 150 -PostTapMax 0` and a side job (the runAA1 `-ChaseV` pattern) that waits for `autopilot start` in
`steps.txt` and taps the combat driver's `cockpit-view` key (V, 0x56):
```powershell
# side job: V cycle cockpit -> chase -> HUD-only -> cockpit, 20 s each, logged in steps.txt
$drvc='<repo>\tools\live\acz-window-driver-combat.ps1'
while(-not (Select-String -Path "$E\steps.txt" -Pattern 'autopilot start' -Quiet -ea 0)){ Start-Sleep 2 }
Start-Sleep 15
foreach($v in 'chase','hud-only','cockpit'){ pwsh -NoProfile -File $drvc -Action tap -Key cockpit-view -AcknowledgeLiveInput -ScreenshotDirectory "$E\shots"; Add-Content "$E\steps.txt" "V -> $v"; Start-Sleep 20 }
```
Then the autopilot ends at 150 s and the post-wait leaves the aircraft unflown: off course -> MISSION FAILED -> the end orbit (the `cinematic`
route) [I: in m02 this took ~35-60 s; m03 is unverified]. Optional, for the takeoff-intro chase rig: the runW1 route (mission 02 runway) instead.
Screenshot (`png-screen.now`) once in each view.

What to read:
1. `pov-config;ok=1;pov=1` and `pov-render-config;ok=1;povc=1;povrig=level;...;prerequisites=ok`.
2. Per V: `pov-mode;from=cockpit;to=chase` then a `pov-census;...;mode=chase;visit-ms=0;route=chase;...;acm=requested` line and 5 more at 1 Hz;
   `ee-camera;event=pov-census-acm;armed=1` within 5 s; `ui3d-census;kind=frame` lines at 1 Hz with `acm=` counts and `kind=other` keys
   (does DrawCPT vanish in chase? how many ACM draws?), `kind=acm` (k360, k400, A=same|differs, rt), `kind=acm-a` (A in full per camera).
   Same for HUD-only and back to cockpit (no new request: the cockpit was requested at the first census line of the run).
3. `summary-pov-rig`: `route-frames=chase:` growing during chase, `cuts=0` in steady chase, `rate-max-dps` in turns, `vig-max`.
4. Mission end: `pov-mode;to=other`, `pov-census;route=cinematic` (moving orbit) or `flat;why=frozen...`, one more ACM request (other).
5. Autopilot: it reads the aircraft state from the logs and stops on a frozen state; if it stops at the chase V, note it (the chase E6 rejection
   might be the reason) [U].

Offline analysis (next step, a script, not written yet): for each `acm-a` line with k400 512 decompose A (section 2a) into eye, axes, lens; pair it
by log tick with the nearest `pov-census` line of `ee-camera`/`xr-stereo` (same steady-clock ms); report |eye - E6 cam|, the axis angle to the
E6 rot frame, the lens per view, which B groups (acm-b translation) sit at the E6 aircraft position (the player aircraft in chase?), and the k360
values of the chase draws (the depth-scale refusals). That settles the stage-2 items 1-2.

## 9. Open / risks

* [U] Whether E6 rot equals the rendered chase camera (the 12.6 deg camera-axis-vs-aircraft offset is consistent with a level look over the
  aircraft, but only A proves it). [U] the chase lens (the game may widen it). [U] which chase ACM records carry +360 != 1.
* [I] The level rig's yaw lag in hard turns moves the aircraft sideways in view by up to the spring lag (~10 deg at 25 deg/s); povtc tunes it.
* The census adds `acm-a` lines only in frames with more than one camera (flight frames with offscreen passes): more log lines in an armed census,
  never in an unarmed run.
* The stage-1 glue (Output, proxy, probe) is not covered by an offline test; `pov_render_offline` covers the pure header only.

## 10. POV1 null run findings (step147, 2026-10-31) and stage 2 (pov:2, IMPLEMENTED, offline tests only)

Evidence: `D:\acz-evidence-archive\pov1\runPOV1` (`analysis-pov.txt`, `logs\ee-camera.log`, `logs\xr-stereo.log`, `logs\step5-producer.log`),
console `D:\acz-evidence-archive\step147-pov.out`. Analysis tick zero = 198,837,125 ms.

### 10.1 What happened in the V cycle [E]
* The V taps worked: `pov-mode;cockpit->chase` at 199,049,484 (t 212.4 s; E6 85.7 units, 84.0 behind / 16.7 above), the real HUD-only at t 235.7
  (eye 36.3 / 3.6), cockpit at 251.8. The game stayed in CHASE for ~23 s, not 3.3 s.
* Why only 95 chase route frames: 1 s after the V the locator lost the real structure (`e6-locate;event=lost;base=1836242bd10;reason=eye-distance`,
  199,049,903) and 2.6 s later LOCKED a shifted alias of the same memory, base `...bd0c` = the real base - 4 bytes (`event=locked;base=1836242bd0c;
  kind=s;air=0.00,90290.37,19199.09;cam=1.00,90345.59,19218.49;rot=2.00000,-0.02897,0.73294;eye-dist=58.546;eye-angle-deg=5.48`): read 4 bytes
  early, air = (pad, x, y), cam = (1.0, x, y), rot = (the 2.0 word, pitch, yaw), and |cam - air| = |(1, 55, 19.6)| = 58.5 passes the 15..60 eye
  rule. The raw pov feed followed the new lock, so the classifier saw a 58-unit "eye": `chase->hud-only` (t 215.7), `->other` (camera-ahead),
  `->hud-only`, ... until the real HUD-only. These flips are the analysis' first FAIL (V-cycle order). The taps did NOT land in HUD-only. The
  producer published no chase frame at all (stage 1 changes nothing there): `world-early-backoff-hold` +27k / 5 s and `world-vp-differs` +~500 / 5 s
  in the chase window (`step5-producer.log` 199,052,984..199,068,078): the early frames were abandoned (no DrawCPT) and backed off.
* hud-only#4 (t 300.2..312.5): E6 cam AND aircraft bit-constant for 12 s with no cockpit composites (a game stall or pause) [U, not a V tap].
* Fix (E6-ALIAS, `ee_e6_locator.*`): a candidate within 0x470 bytes of the last locked base (and not that base) is refused
  (`alias-of-locked-base`) while the home structure still reads as a MOVING camera refused only for its geometry (eye-distance / eye-direction /
  yaw-relation). A dead home (unreadable, zero, junk, frozen) does not block. `e6_locator_offline` `test_alias_guard` rebuilds the logged layout.

### 10.2 The true chase camera: A vs E6 [E, census frames s=1 / s=2 at 199,052,054 / 199,053,070]
* Chase frames: 93 DrawACM_v draws, no DrawCPT key; k400 512 x29 + 128 x3 (offscreen); every logged k360 = 1 (32 of 93 per frame logged): the W1
  `world-record-depth-scale` refusals are NOT reproduced in m03 (draws 33..93 [U]).
* Four distinct cameras per frame (the per-camera `acm-a`): `all=5`/`all=7` offscreen (k400 128), `all=19` = the ABSOLUTE scene camera, `all=34` =
  rows 0..2 bit-identical to all=19 with its own translation row (172032, 184232, 96573408, w = 84.0) = the player's aircraft drawn camera-relative:
  A34 = T(p) x A19 with p = (90629.5, 19214.8, 89429.0) (column-2 residual 8.5), 85.7 units in front of the eye = the chase distance. The producer
  refused this draw as `world-vp-differs`.
* all=19 decomposed (flight screen map): eye (90684.4, 19234.4, 89491.8), yaw 41.8, pitch -1.8, roll 2.9 deg, lens tan 0.560 x 0.315 (58.5 x 35.0
  deg; analyze-pov.py's own scale printed 58.1 vs 62.8 for the cockpit: the chase lens is narrower). E6 at the same moment (x/y interpolated between
  the census line 199,051,500 and the alias line 199,052,515): (90667, 19239) vs A (90684, 19234) = ~18 units = one frame of motion at ~1,100 u/s;
  orientation within ~1 deg. E6 IS the rendered chase camera. The analysis' second FAIL (`|A eye - E6| median 134,964`) compared A with the shifted
  alias E6 values and counted the camera-relative all=34 draw as a camera: an artefact, not a disagreement.
* Depth model of the chase A: b = -1474.5, Z0 ~ 9.67e7 (Z0/b ~ -65,580, the cockpit's far plane; another near plane).

### 10.3 Stage 2 (pov:2): the chase view in stereo
Option: `pov:2` inside `xr_terrain_stage` (= pov:1 + stage 2; `povrig:game` keeps the game camera, anything else = the level rig; `povtc`).
Needs `gpu_copy_step5_world_objects=true` and `gpu_copy_step5_members_by_class=true` (both in the day config). Code: `pov_render.h` (stage-2 section),
`gpu_copy_step5_world.h` (`relative_offset`, `substitute_relative`), `gpu_copy_step5_hooks.inl` (early open, adopt, submission, publication, status
line), `stereo_frame_source.h` (`no_cockpit`), `xr_stereo_output.*` (request, classifier evidence, cockpit-model gate), `view_mode.h` (pov:2).
1. Consumer, every XR frame: route `chase` -> `producer_request` = chase | level | tc (else 0; cleared at retire). Line `xr-stereo;event=pov-stage2;
   request=chase|off;route;why;rig;tc-ms;no-cockpit-frames` on every change.
2. Producer, the first scene DrawACM_v draw of a list (`step5WobjEarlyOpen`) while requested and not in the cockpit hold: no early back-off; the
   opener's A camera must lie within 400 units of the raw E6 chase camera (`opener_check`; a camera-relative first draw is refused without blocking
   the list); the level rig yaw (critically damped, povtc) gives `lookR` = camera-local -> rig-local, so `setup_poses` builds the eyes around the
   game camera but turned onto the level rig (head pose on top, 5 u/m); the frame is tagged no-cockpit with lens = A's lens, game-camera axes = the
   RIG axes (terrain and objects use one camera), the eyes' HMD FOV, world snapshot + HUD region armed at the opener.
3. Later scene draws: absolute ones as before; camera-relative ones (A = T(p) x A_ref) admitted in no-cockpit frames only (`substitute_relative`:
   A' = A + T(p) x D_e S).
4. Submission: a no-cockpit frame is PUBLISHED (StereoFrame::no_cockpit, diag_start 4), never abandoned. A DrawCPT draw inside one (V back to the
   cockpit) poisons it (`pov-no-cockpit-frame-saw-cockpit`) and holds no-cockpit opens for 1 s, so ordinary cockpit frames resume.
5. Consumer: no cockpit model for no-cockpit frames; the terrain uses the frame's game camera (= the rig; E6-free, CamMode game_only); the
   classifier's cockpit evidence is the age of the last frame WITH cockpit members (identical without pov:2), so chase frames never flip it back.
6. Producer status line (5 s): `<prefix>-world-early-pov;key=xr_terrain_stage(pov:2);no-cockpit-opened;opener-refused;cockpit-poisoned;
   relative-admitted;relative-refused;rig-enters;rig-yaw-deg;last`.
Not in stage 2: the comfort vignette shader (the rig computes the value; nothing draws it), HUD-only (stage 3), cinematics (stage 4).

Tests [T]: `pov_render_offline` (2,971 checks: stage 1 + request/route coupling, the opener check incl. the relative "eye", level-axes handedness,
the producer spring, the rig look inside the REAL `setup_poses` (level horizon under a pitched/rolled POV1 camera, IPD along the rig's right axis,
head yaw 30 deg, povrig:game = identity), the POV1 A19/A34 records through the REAL `camera_setup` / `relative_offset` / `substitute_relative`),
`view_mode_offline` (pov:2), `e6_locator_offline` (`test_alias_guard`), `ui3d_census_offline`. Compiled and run standalone (cl /std:c++17, these four
test files only); the producer/consumer glue was only syntax-checked (/Zs). Nothing ran in the game.

Risks: (a) with terrain off, the game's world snapshot of a chase frame (the flat chase picture) would be composited as the window plane [I];
(b) HUD capture in chase is unproven (armed at the opener); (c) chase draws 33..93 were never logged: other camera-relative or depth-scale records
are refused (world-incomplete, counted), never mis-drawn; (d) the game culls to its chase frustum: turning the head shows our terrain but no
objects behind; (e) with the alias guard the locator keeps searching during chase (the A camera is used, no E6 terrain fallback); (f) a frame opened
while the classifier still says chase but the game is already in HUD-only is published around the HUD-only camera on the level rig until the
classifier flips (~0.3-0.5 s; the opener check refuses it once E6 shows the eye geometry).

### 10.4 Next null run (pov:2)
Config: the day `xr_terrain_stage` + `;pov:2;povc:1;povrig:level` (the POV1 value with pov:2). Same runPOV1.ps1 flow (runK3ag, V cycle 20 s each,
PostWait 150). Read: `pov-config;pov=2;...;renders=chase-in-stereo(...)`; at the chase V `pov-stage2;request=chase` within ~0.6 s, then
`-world-early-pov` `no-cockpit-opened` growing ~60/s with `opener-refused` small and `relative-admitted` ~1-2 per frame; `summary-pov-rig` chase
frames growing for the whole ~20 s (no alias flips; `e6-locate` summary reasons show `alias-of-locked-base`); `png-eye.now` / `png-screen.now` in
chase: the aircraft in stereo ahead, level horizon, terrain; at the HUD-only V `pov-stage2;request=off` and the vscreen; at the cockpit V normal
frames (`cockpit-poisoned` 0..2). Abort if `cockpit-poisoned` keeps growing in the cockpit view or no cockpit composites return after V.

## 11. Stages 3 + 4 (pov:3, 2026-10-31; IMPLEMENTED, offline tests only)

Evidence: `D:\acz-evidence-archive\pov1\runPOV2` / `runPOV2b` (step149, master 16d756cf; the pov:2 verification: no-cockpit-opened 1,167,
cockpit-poisoned 0, relative-admitted 88,616, chase held ~20 s, eye dump f4269 = the aircraft from behind with a level horizon) and the stage-1 run
`runPOV1`. Nothing in this section ran in the game, SteamVR or a headset.

### 11.1 Findings [E]
* **f5207 cloud card (runPOV2b eye dump at 201,848,656, a cockpit frame).** The puff has a hard vertical edge at x = 232 of the 1,200-pixel
  upright dump and a hard horizontal edge across its lower-left part. x = 232 is the game window's left border in a 90-degree HMD eye: 600 - 0.61 x
  600 = 234 (cockpit lens tan 0.61). It is NOT in the world layer (`-worldfx.png` shows terrain and sky only), so it is one of the game's class-1 puff
  quads replayed per eye, which the game submits already clipped to its own window (`fx_puff_trail.h`: "every constant-depth puff quad already
  CLIPPED"). It is a window-border cut that was not collapsed. The frame is the 2nd producer frame after the 20 s HUD-only visit (cockpit composites
  resumed at 201,848,609; the producer published no frame during HUD-only under pov:2). The `fx-puff-trail` window that contains it (frames
  5205..5249) shows clipped=161, coll=32, cst=5 new stores. The collapse rule only collapses a clipped quad when a stored WHOLE card of the same key is
  re-emitted in that frame, and the memory had no whole card for this puff yet. In steady flight the ratio stays clipped ~240 / collapsed ~60 per
  second (`fx-puff-mem nomatch` ~450/s), so cut puffs at the window border are expected whenever the memory has no match. A possible fix belongs to
  the effects track, not POV: with `fxcloud` on, collapse every clipped cloud-size quad (edge in the cloud band) whether or not a stored card replaces
  it. That hides the cut half instead of showing a hard edge. Not done here. Stage 3 removes the cause that made this frame stale: HUD-only now
  publishes frames, so the puff memory keeps running.
* **The runPOV2b end was a crash, not the end camera.** After the autopilot (150 s at 5,000 units) the unflown aircraft descended (`PULL UP` at
  01:02:11) and hit the terrain. The game then showed the crash site and the REPLAY ("PRESS MENU BUTTON TO EXIT REPLAY", external angles that cut
  every ~3 s; gameshots g0074..g0099). E6 kept showing the eye (hud-only, t 417..469 s), then a far 'other' camera (8,934 units, flat), then a chase
  geometry, while the game rendered the replay cameras. The opener check refused 46,628 openers (`pov-camera-not-the-e6-chase-camera`,
  step5-producer 202,072,750..202,087,812) and opened 31 frames while one replay angle sat near the E6 camera. **E6 is not the render camera in the
  replay.** The check keeps the wrong camera out, but sporadic frames would make the view flicker between the vscreen and stereo (pov:3 NcGate below).
* **The cinematic route frames of runPOV1.** Census n=40..52 (`route=cinematic`, 199,053,968..199,072,281) were the E6 ALIAS artefact (cam = 1.0,
  x, y; rot = 2.0, ...; fixed in stage 2 by the alias guard), not cinematics. The real mission-end camera is at n=97..100 (199,317,296..):
  geometry `other`, 111.2 units from the aircraft (99.0-99.7 ahead, 49.3-50.5 up), looking back at it (look yaw 3.13, pitch -0.39 rad,
  fwd-to-air 3.8 deg). It **travels with the aircraft at ~1,115 u/s** (z +1,113 per second). The stage-1 anchor (a pure world point following with
  tc 2 s) lagged 206 / 378 units in consecutive census lines and re-latched about every 0.4 s, each time with a 150 ms blink. The W1 m02 end camera
  (`kOrbitFail`: look 3.135, pitch -0.392) has the same form [I].
* [U] Whether the rendered A camera equals the E6 camera at the mission-end camera: no ACM census covered it (the 4 census requests went to the
  earlier views). The opener check settles it in the next run (`kinds` cin > 0, or refusals with `pov-camera-not-the-e6-cinematic-camera`).

### 11.2 What pov:3 does
Option `pov:3` inside `xr_terrain_stage` = pov:2 + HUD-only + cinematics + the drawn vignette. pov:1 and pov:2 are unchanged. Without `povrig`,
pov:3 runs the consumer rig as `level`, because the drawn vignette and the blink come from it.
1. **Routing** (unchanged Router, `pov_render.h`): mode `other` + E6 geometry `other` + camera <= 2,000 units (400 m) from the aircraft + E6 block
   moving within the last 1 s -> `cinematic`; anything else -> `flat`. Pre-rendered FMV has a zero/frozen block and no scene ACM draws, so it stays flat.
2. **Request kinds** (`NcKind`, request bits 2..3; kind chase = 0, so every pov:2 request value is unchanged): route chase -> chase (level rig or
   povrig:game); route hud-only -> hud; route cinematic -> cinematic (anchor rig, or povrig:game). `request_for_stage3`.
3. **NcGate** (consumer): a requested view that gets no no-cockpit frame within 600 ms, or a gap > 300 ms once frames came, withdraws the request
   for 1 s (flat on the vscreen), then retries. A route change re-arms it at once. This implements "a refused frame falls back to flat for 1 s" and
   stops the replay-style flicker. The log line is `pov-stage2;request=off;why=pov3-no-first-frame(flat-1s)|pov3-no-frames-gap(flat-1s);gate-fallbacks=N`.
4. **Producer opener** (`step5WobjEarlyOpen`): `opener_check(eye, e6, kind)`. hud needs E6 geometry `eye`; cinematic needs E6 `other` within
   2,000 units of the aircraft; every kind needs |A eye - E6 cam| <= 400 units. The refusal reasons are `pov-e6-not-eye`, `pov-e6-not-other`,
   `pov-e6-camera-far-from-aircraft`, `pov-camera-not-the-e6-eye`, `pov-camera-not-the-e6-cinematic-camera`.
   * **Stage 3 (HUD-only):** the eyes are built around the game camera exactly (lookR = none, no rig, no vignette). The head is 6DoF about the
     eye, as in the cockpit. The frame is tagged no-cockpit, so no cockpit model is drawn; lens = A's.
   * **Stage 4 (cinematic):** `ProducerAnchor`: the eyes stand at the ANCHOR on a LEVEL yaw. `setup_poses(..., eyeOffset)` takes the new
     `eyeOffset = anchor - A eye` (nullptr = byte-identical). VP_ref stays the game camera, so D_e carries the offset into every substituted record.
     The frame's game eye = the anchor and its game-camera axes = the level rig, so terrain, objects and markers share one camera.
5. **Anchor follower** (`AnchorCore`, shared by the producer anchor and the consumer rig that drives the vignette and census). The evidence above
   rules out a pure world point. The anchor carries the camera's LOW-PASSED VELOCITY: tc 1 s, and 250 ms for the first second after a latch so a
   moving entry does not run away. Per-step velocity samples are clamped to 4,000 u/s, so a jump below the cut rule adds at most 4,000 u/s x dt. The
   anchor also pulls toward the camera with tc 2 s. Effect: a steadily travelling camera is followed with zero steady-state lag (a smooth translation,
   no rotation), and swings, orbits and bobbing are smoothed. Re-latches with a 150 ms blink: beyond 400 units from the anchor, or on a camera cut
   (a move > 20 u/ms x dt + 200 units, or a heading step > 45 deg). A gap > 250 ms re-enters. The yaw is a 1 s spring toward the camera heading;
   pitch and roll are 0.
6. **The vignette, drawn.** `vignette_mode_bits` packs it into the eye targets' `mode_bits`: bits 24..31 = strength q, bit 20 = blink. These bits
   are free on the eye targets; on the HUD target they carry the radar background / alpha floor, and the root signature is full at 64 DWORDs.
   `xr_stereo_blit.hlsl` `ps_main` now wraps `ps_main_body`. Blink = rgb x 0. Otherwise rgb x (1 - smoothstep(inner, inner + 0.35, |2uv - 1|)) with
   inner = 1.45 - q/255. Alpha is kept (premultiplied). The CPU mirror is `vignette_gain`. It is drawn only with pov:3, on a chase/cinematic route
   with a valid rig, over a NO-COCKPIT frame, on the final eye image (not the png alpha/world debug views). Strength = povv x smoothstep(30, 120 deg/s)
   of the rig rotation (stage 1). The blink lasts the 150 ms after a rig entry, cut or re-latch.
7. **Logs:** `pov-stage2;request=chase|hud-only|cinematic|off;...;rig=level|game|game(eye)|anchor;...;gate-fallbacks=N` (on every request or kind
   change); `summary-pov-stage3;request;route;req-frames=chase:,hud-only:,cinematic:;no-cockpit-frames;gate-fallbacks;vig-frames;blink-frames;vig-max;povv`
   (every 10 s); the producer status `-world-early-pov` gains `;kinds=chase:N,hud:N,cin:N;anchor=e:N,c:N,r:N,last:<event>`;
   `pov-config;pov=3;...;renders=chase+hud-only+cinematic-in-stereo(...)`.

Code: `pov_render.h` (AnchorCore, NcKind, request_for_stage3, opener_check(kind), ProducerAnchor, NcGate, vignette_*), `view_mode.h` (pov:3),
`gpu_copy_step5.h` (setup_poses eyeOffset), `gpu_copy_step5_hooks.inl` (kinds, anchor, eye offset, status), `xr_stereo_output.*` (request + gate,
vignette job bits, summary), `xr_stereo_d3d12.cpp` (eye constants), `xr_stereo_blit.hlsl` (ps_main wrapper), `xr_stereo_proxy.cpp` (pov:3,
default level rig).

### 11.3 Tests [T]
* `pov_render_offline`: 8,587 checks. Stage 1-2 unchanged except `test_rig_anchor`, rewritten for the follower: a 100-unit step settles with
  < 45 units overshoot and no re-latch; a 9,000 u/s sprint re-latches with a blink. New tests:
  * request kinds: pov:2 values unchanged;
  * the end-camera route (cinematic while moving, flat when frozen);
  * openers per kind, including the runPOV2b replay and post-crash refusals;
  * the anchor on the REAL end-camera motion: the stage-1 rule re-latches >= 15 times in 10 s; the follower does not re-latch in 20 s, max lag
    < 260, final lag < 10, bob smoothed. Also a 30 deg/s orbit, cuts, heading cut, gap re-entry;
  * eyeOffset through the REAL `setup_poses` / `substitute` / `camera_setup` on the POV1 A19 record (zero offset bit-identical; the substituted
    record decomposes to the anchor eyes, level);
  * NcGate (first-frame and gap fallbacks, 1 s hold, kind change);
  * the vignette (packing never touches bits 0..19, shader-mirror values, monotonic, continuous at 0, gating, blink 150 ms).
* `view_mode_offline` 72 checks (pov:3; pov:4 bad), `ui3d_census_offline` 294 (unchanged), `test_analyze_pov.py` 20 tests (new
  `TestStageRender`: the pov:3 checks pass/fail, format keys present in the sources).
* Compiled and run standalone (`cl /std:c++17 /W4`), with no warnings. The blit shader compiles with fxc (`/T ps_5_0 /E ps_main /WX /O3`). The glue
  (`xr_stereo_output.cpp`, `xr_stereo_proxy.cpp`, `xr_stereo_d3d12.cpp`, `d3d12_hook.cpp` with the hooks) was only syntax-checked (`/Zs`, stub shader
  headers). No full build, no install.

### 11.4 Limits / risks
* A replay, or any camera the E6 block does not show, stays flat by design (opener refusals, then the gate's 1 s fallbacks).
* With `interp:1`, the producer dead-reckons the GAME camera and folds its delta into the objects; the consumer folds the same delta into the
  terrain camera (here the anchor). Both shift together (one camera), but the anchor gets one frame of extrapolated camera motion on top [I].
* The vignette darkens the composite pass (world, terrain, objects, effects, props). The tactical markers / NBOX stamps drawn after it are not
  darkened, and in a blink they stay visible over black [I].
* The vignette is head-locked in eye-image coordinates (radius in normalised eye-image units, not tangent units: an asymmetric HMD FOV puts the
  ring slightly off-centre) [I].
* The consumer rig (vignette) and the producer anchor run the same rule on E6 vs A samples (~1 frame apart). A blink can start up to one frame
  before or after the producer's re-latch [I].
* The takeoff intro uses the chase rig geometry (section 1), so it routes `chase` (level rig), not cinematic [I]. Event cameras route cinematic
  only when E6 shows them and they are within 400 m of the aircraft.
* HUD-only uses the game camera with the head on top. If the look drive (parked, D-291) were on, the game look would be in A and the head would
  add to it (lookR is not used for HUD-only).

### 11.5 Config
The day `xr_terrain_stage` + `;pov:3;povc:1;povrig:level`. With the current installed value this is 238 characters (limit 250) and 32 keys
(runPOV1.ps1 -WhatIf). It needs the day keys `gpu_copy_step5_world_objects=true`, `gpu_copy_step5_members_by_class=true`, `ee_camera_probe=true`,
stage `auto`, `vsearly:1`. Leave `povv` at its default (0.60), or `povv:0` to keep only the blink.

### 11.6 Null run (pov:3; exact steps)
Null driver, SteamVR first, memory gates as usual (the runner does them). From the main checkout after the merge and a day build/install:
```powershell
pwsh -NoProfile -File research\experiments\step83-verify-run1\runPOV1.ps1 -Tag POV3 -Append ';pov:3;povc:1;povrig:level' `
     -AP '-Mode Cruise -Total 150 -TargetAlt 9000' -PostWait 240 -ExpectDxgi <8 hex> -ExpectD3d12 <8 hex>
```
Flow: m03 air start, autopilot, V taps at +15 / +35 / +55 s (chase, HUD-only, cockpit, 20 s each, png-screen + png-eye 5 s after each). Then the
aircraft is left unflown from 9,000 units instead of 5,000 (runPOV2b crashed into the replay): MISSION FAILED -> the mission-end camera. The side
job's new **cinematic watcher** reads the live xr-stereo.log. Each new `pov-stage2;request=cinematic` triggers png-eye.now + png-screen.now at
+1.5 / +5 / +10 s (at most 3 visits, 15 s apart; `-CineVisits 0` turns it off). `analysis-pov.txt` section 6b checks:
1. `pov-config;ok=1;pov=3` and `pov-render-config;...;povrig=level;prerequisites=ok`.
2. Chase: `pov-stage2;request=chase;rig=level` (as pov:2), `no-cockpit-opened` growing, f-dump = the aircraft from behind, level horizon,
   darkened corners while turning (vignette).
3. HUD-only: `pov-stage2;request=hud-only;rig=game(eye)`, producer `kinds=...hud:N` growing ~60/s, an eye dump with the HUD and the world in
   stereo and no cockpit. Back to cockpit: `request=off`, `cockpit-poisoned` 0..2.
4. End: `pov-mode;to=other`, `pov-stage2;request=cinematic;rig=anchor`, producer `kinds=...cin:N` growing and `anchor=e:1,c:0,r:0` (or few
   re-latches), cinematic eye dumps showing the aircraft from the end camera's side with terrain, horizon level.
   If instead `pov-stage2;request=off;why=pov3-no-first-frame(flat-1s)` repeats with `last=pov-camera-not-the-e6-cinematic-camera`, the end camera's
   A is not the E6 camera ([U] above): stage 4 then stays flat for that camera, and the census (povc) `acm-a` lines of the 'other' visit tell why.
5. `summary-pov-stage3`: `vig-frames` / `blink-frames` > 0, `gate-fallbacks` small (0 in steady views).
Abort if `cockpit-poisoned` keeps growing in the cockpit view or no cockpit composites return after V (as pov:2).

### 11.7 runPOV3 (step151, master 13345fb5, dxgi B764CA4A; 2026-10-31) findings and the A-only fix
Evidence: `D:\acz-evidence-archive\pov1\runPOV3` (`analysis-pov.txt`, `logs\xr-stereo.log`, `logs\step5-producer.log`, gameshots), console
`D:\acz-evidence-archive\step151-pov3.out`. Analysis time zero = 203,857,937 ms (wall clock ~01:32:58).
* [E] **What worked.** Chase: 1,182 frames. HUD-only (the V tap): 1,180 frames; f5353 is HUD-only terrain in stereo.
* [E] **There was no mission-end camera in this run either.** At 01:39:41..53 the aircraft was low over the desert (despite `-TargetAlt 9000`). It
  crashed (hud-only from t 414.1 s, cockpit composites gone), and from 01:40:05 to the end the game showed the REPLAY (gameshots g0076..g0128,
  "PRESS MENU BUTTON TO EXIT REPLAY").
* [E] **The fallback cycling (gate-fallbacks 0 -> 109) happened in two kinds of window, both cameras that E6 does not show:**
  * **An in-mission event cutscene**, t 295.6..307.8 s (01:37:53..01:38:05, gameshots g0035..g0038). The game played the "INDIGO, Belkan Air Force
    7th Air Division" introduction with external shots of the enemy flight and "Target spotted". E6 stayed at the eye, so the classifier said
    hud-only. The producer refused 17,544 openers with `pov-camera-not-the-e6-eye` (status 291.2 -> 311.2 s) and opened none, so the gate cycled
    flat 1 s / retry 0.6 s eight times.
  * **The crash and the replay**, t 414.1..576.1 s. E6 showed the eye (hud-only), then a chase geometry (chase#2, 117 s) that is not the replay's
    render camera: 140,000+ refusals (`pov-camera-not-the-e6-chase-camera`), with only 10 + 6 chase frames opened when a replay angle sat near it.
    The gate cycled ~100 more times, with two `pov3-no-frames-gap` after those sporadic frames.
* [E] **The two cinematic requests DID render.** At 457.9 s (`other`, 852 units: E6 inside the replay transition) 31 cinematic frames were
  opened (no-cockpit-frames 1257 -> 1273) and there were 0 cinematic opener refusals. At 577.1 s, 27 frames (1283 -> 1331). Anchor e:2, c:0, r:0.
  Both ended because E6 changed geometry to chase after 0.6 s and 1.5 s (the route changed); they were not refused.
* **Answers.** (1) Is the mission-end camera the E6 camera? [U] still: no end camera occurred (crash -> replay). The cinematic windows that
  did occur passed the 400-unit E6 check every time. (2) Why the fallback keeps firing: the game rendered cameras that E6 does not follow (an
  event cutscene, the replay). The E6 opener check correctly refused them, so no frame ever came. (3) Is 400 units too tight? Not for the
  windows where E6 was the camera. Where E6 was not the camera, the distance is unbounded, so no radius around E6 would fix it. The ACM census
  could not measure the A-vs-E6 distance: its 4 requests went to the earlier views. The analysis' `A vs E6` numbers pair census frames with
  E6 lines up to 2.5 s apart, i.e. ~1,100 u/s x the time offset, so they are not a camera disagreement.
* [E] **pov-side.log missing.** The side job's logger was named `SL`, which is PowerShell's built-in alias for Set-Location, and aliases win
  over functions. Every log line became a Set-Location call. The taps and the png-eye/png-screen triggers still ran, but nothing was written to
  `pov-side.log` or `steps.txt` (runPOV2b had the same problem). The logger is renamed `PovSideLog`.
* [E] **No cinematic eye dump.** The cinematic windows were 0.6 s and 1.5 s long. The watcher's first dump at +1.5 s came too late, and the
  pending png-eye.now trigger then dumped the next fresh frame (f15580 at 473.6 s, a replay chase frame). The run ended 4 s after the second
  window.

**Fix (pov:3, offline tests only):**
1. **A-only escalation** (`NcGate`, `pov_render.h`). The producer counts `pov-camera-not-the-e6-*` refusals in `producer_e6_mismatch()`.
   Escalation happens when a gate fallback ends a SHORT attempt (<= 3 s: no frame, or only a few sporadic frames) during which the producer
   refused >= 30 openers for that reason. The gate then retries at once with an **A-only cinematic request** (kind cinematic + `kReqAOnly`; the
   anchor rig, or povrig:game). This lasts until the route changes (a cockpit frame, another view, flat). A long-running view that hiccups
   (a loading stall) and FMV/pause (no draws, hence no refusals) keep the plain 1 s fallback.
2. **Producer A-only opener** (`opener_check_a_only`). The frame's first scene-target camera A alone (D-291: the game's own render camera)
   with the anchor rig. Refused when its decomposed eye is < 3,000 units from the world origin: a camera-relative draw decomposes to
   camera - model, e.g. POV1 all=34 -> (55, 19.6, 62.8). Also refused when it is > 60,000 units from the E6 aircraft (the replay spans ~30 s of
   flight) or when the E6 block is invalid (menus / FMV zero block).
3. **A-only vignette.** The consumer rig follows E6, which is wrong there, so only the producer anchor's 150 ms blink is drawn, from
   `producer_anchor_events()` (enter / cut / re-latch). The replay's camera cuts every ~3 s each get a blink.
4. **In-DLL cinematic eye dump** (`CineDump`). The consumer re-arms xr_png_once itself on the 4th fresh no-cockpit frame of each cinematic or
   A-only visit, at most 4 per session (`png-eye-armed;trigger=pov3-cinematic(e6|a-only)`). The runner's watcher now fires at +0.2 / +2 / +5 s.
5. **Logs.** `pov-stage2;...;rig=anchor(a-only);...;gate-fallbacks=N;escalations=N;e6-mismatch=N`;
   `summary-pov-stage3;...;escalations;a-only-frames;cine-dumps`; producer status `;a-only=N`. analyze-pov.py section 6b prints them.

Tests [T]: `pov_render_offline` 10,577 checks (new: `test_aonly_escalation` replays the cutscene / replay / normal-chase-hiccup / FMV / povrig:game
timelines, `test_aonly_opener` with the POV1 cameras, `test_event_blink_and_cine_dump`), `view_mode_offline` 72, `test_analyze_pov.py` 20. Compiled
and run standalone (cl /std:c++17 /W4, no warnings). The glue was syntax-checked with /Zs. Not run in the game.

Risks: [I] the A-only anchor rides the cutscene camera, which can be a fast fly-by. Each cut blinks and each relatch beyond 400 units blinks,
but a smooth fast swing is followed (velocity feed-forward). [U] a cutscene drawn with an unusual lens or offscreen passes only; the opener
still requires the flight scene target.

**Null run (pov:3, after merge + day build/install):** the same command as 11.6. To also get the true mission-end camera, avoid the
crash: the autopilot ended low over the desert at `-TargetAlt 9000`, so use `-AP '-Mode Cruise -Total 300 -TargetAlt 9000'` (the autopilot
keeps flying, so MISSION FAILED / time-out can come before any unflown descent) and `-PostWait 240`:
```powershell
pwsh -NoProfile -File research\experiments\step83-verify-run1\runPOV1.ps1 -Tag POV3b -Append ';pov:3;povc:1;povrig:level' `
     -AP '-Mode Cruise -Total 300 -TargetAlt 9000' -PostWait 240 -ExpectDxgi <8 hex> -ExpectD3d12 <8 hex>
```
Read in `analysis-pov.txt` 6b:
* during the INDIGO cutscene (runPOV3: 01:37:53 wall clock, ~103 s after the autopilot start): `pov-stage2;request=cinematic;...;rig=anchor(a-only);...;escalations=1`
  ~0.6 s after `pov-mode;to=hud-only`, then no `pov3-no-first-frame` cycling; the producer `a-only=` count growing; a `png-eye-armed;trigger=pov3-cinematic(a-only)`
  dump showing the enemy flight in stereo;
* in a replay (if the aircraft crashes): the same escalation, frames, and blinks at the replay's camera cuts;
* at the mission-end camera (if reached): `request=cinematic;rig=anchor` (E6 route) with `kinds ... cin:N` growing and a `trigger=pov3-cinematic(e6)` dump;
  if instead it escalates, E6 is not that camera either (then answered [E]);
* `pov-side.log` present, with V taps and `cinematic watcher` lines;
* abort rule as before (cockpit-poisoned growing in the cockpit, or no cockpit composites after V).

### 11.8 runPOV3b (step153, master 4f376fc7 / aa8f2684; 2026-10-31) findings and fixes
Evidence: `D:\acz-evidence-archive\pov1\runPOV3b` (`pov-side.log` now written, `steps.txt`, `logs\xr-stereo.log`, `logs\step5-producer.log`).
* [E] **Cinematic works.** 13 cinematic requests, kinds cin:2541, anchor e:6 c:8, in-DLL dumps f24400..f24707; f24707 = terrain + game clouds in stereo.
  The A-only escalation rendered the post-crash cutscene / replay (566.7 s: 0 -> 1,113 frames; 581.4 s and 716.5 s chase-geometry replay windows).
* [E] **The chase:0 / hud:1 "regression" is the RUNNER, not the gate.** With the side-job logger fixed (11.7), the job's first line
  "waiting for 'autopilot start' in steps.txt" went into steps.txt and matched its own wait pattern: `side job: autopilot start seen` at 01:59:24.969,
  V taps at 01:59:40 / 02:00:00 / 02:00:20 (menus / loading), the real `autopilot start` at 02:02:57.850. No V cycle happened, so there was no chase
  and no V HUD-only visit to render. No normal chase / HUD-only visit was escalated: all 4 escalations are the event cutscene (300.7 s) and the
  crash cutscene / replay (566.1, 580.7, 715.9 s), where E6 is not the camera. Fix: the job waits for runK3ag's own line only
  (`^HH:mm:ss.fff autopilot start: `) and no longer writes the phrase. `test_vcycle_never_escalates` pins the gate side (a V cycle with frames
  flowing and a growing mismatch counter: 0 escalations, 0 fallbacks, the kinds kept).
* [E] **The INDIGO event cutscene (300.7..313.0 s) escalated but rendered nothing.** All 20,953 A-only openers were refused with
  `pov-a-only-camera-far-from-aircraft`: the cutscene films the enemy flight more than 60,000 units from the player. Fix: the aircraft bound is now
  only a sanity limit of 500,000 units. The camera-relative rule (|eye| >= 3,000) is what keeps a wrong camera out.
* [E] **f24400 black = a blink frame, not a vignette bug.** The in-DLL dump was armed at 566.8 s on the 4th frame of the A-only visit, inside the
  producer anchor's entry blink (EventBlink 150 ms -> mode_bits blink -> rgb x 0). Fix: `CineDump` counts only non-blink frames, and the consumer
  never captures a png on a blink frame (`job.png_capture` waits for the next fresh non-blink frame; this covers png-eye.now too).
* Tests [T]: `pov_render_offline` 23,000 checks (the cutscene camera accepted; the dump on the 4th non-blink frame; the V-cycle gate), `view_mode_offline`
  72, `test_analyze_pov.py` 20; standalone cl /W4, glue /Zs. Not run in the game.

Null run: as 11.7 (`-Tag POV3c`, the same arguments). Expect, in order:
1. `pov-side.log`: `autopilot line seen` AFTER `autopilot start:` in steps.txt.
2. A V cycle with chase / HUD-only kinds growing as in runPOV3.
3. At the INDIGO cutscene, `rig=anchor(a-only)` with the producer `a-only=` count growing, then a non-black `trigger=pov3-cinematic(a-only)` dump.
