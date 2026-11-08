# OBJECT-INTERP: replayed world objects (and the terrain camera) at the headset's display time (interp:1)

User requirement (2026-10-27, D-340): "I don't care about patching the code: do it to give us better frame rates."
D-339 showed that a patch-based uncap makes the GAME run faster (fixed step, one step per frame), not smoother. This track does the
alternative D-340 names: smooth the game-sourced 3D content between the game's 60 Hz states.
This was offline work only. Nobody launched the game, SteamVR or a headset. A driver agent runs the live plan in section 7.
Tags: **[E]** = established here (code reading, offline tests, WARP). **[I]** = inference, not verified in the game.
Code: `src/acz-vr/object_interp.h` (header only), `gpu_copy_step5_hooks.inl` (`step5InterpFrame`, `step5InterpObject`), `xr_stereo_output.cpp`
(`Output::interp_camera`), `xr_stereo_proxy.cpp` (arming), `frame_pacing.h` (options). Tests: `object_interp_offline`, `frame_pacing_offline`,
`xr_stereo_output_offline` (terrain case 9), WARP harness `case_object_interp`.

## 1. Where the replayed objects are drawn, and what that allows [E, code reading]

The brief assumed the consumer re-renders the recorded object draws for every headset frame. It does not:
* The DrawACM_v world objects (aircraft, ground units, missiles, buildings) are replayed by the **producer**, on the game's render thread, inside the
  game's own command list. Each accepted draw is recorded again per eye with a substituted copy of its 416-byte record: `+0` A (world -> PS2 screen) is
  replaced by the eye's matrix (`gpu_copy_step5_world.h`), everything else is the game's. The result lands in the slot's private eye images, **once per
  game frame**.
* The consumer only composites those images (with our terrain, props, clouds, cockpit model and tactical overlay) when a NEW producer frame arrives
  (`Output::transact`). A headset frame without a new producer frame re-submits the last layers unchanged (`Output::pace`, `plan = last_plan_`), and
  SteamVR reprojects them for head rotation. The terrain is not re-rendered on those frames either (uncap.md 3.5 is a proposal, not built).
* The draws cannot be re-executed later from the consumer: they use the game's PSOs, root signatures, descriptor tables, vertex/index buffers and an
  UPLOAD record that the game rewrites in the next frame. Re-executing them after the game has moved on is unsafe (stale descriptors / records) and
  cannot be verified offline. Not attempted.

So a model matrix can be changed **once per game frame**, not once per headset frame. interp:1 uses that one chance to draw every object, and the
camera it is seen with, at the time the frame will actually be on the display.

## 2. What interp:1 does

Option: inside `xr_terrain_stage` (no new config key; 31 of 32 keys stay). `interp:1` (default off), `interpx:<0..3>` (extrapolation clamp in game
frames, default 1.5). It shares `extrapms` (camera horizon clamp, default 33 ms) and `extrapatt` with extrap:1. It is removed before the terrain
parser sees the value, like `extrap:`. Example (the value is 61 characters, under the 100-character limit):
`-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1;interp:1'`.
The proxy arms the producer only when a terrain stage loaded (without the terrain the eye images keep the game's own world, and objects drawn at
display time would slip against it). Needs `gpu_copy_step5_world_objects` (on in the default key set).

Per producer frame (the first accepted world draw recovers the frame's world camera from A; `step5InterpFrame`):
1. **Horizon.** The consumer measures each frame's select age (its XR predicted display time minus the frame's own predicted display time, the
   extrap:1 horizon) and publishes an EMA (0.8/0.2) through a process-wide atomic. The producer reads it: `t_display = frame time + horizon`.
2. **Camera.** The frame's game camera (A-derived eye + axes, the same camera the terrain uses through `game_eye`/`game_cam`) feeds the extrap:1
   dead-reckoning model (`pacing::MotionExtrap`: velocity and turn rate per game frame x the measured game frame rate; no extrapolation through
   reversals, stops, sharp speed changes, cuts; rotation clamp 6 deg). The horizon is clamped to `extrapms` and to `interpx` game frames. The result
   is the rigid delta `dt`, `dR` (frame camera -> display camera). If the camera cannot be extrapolated (and is not standing still) nothing moves this
   frame (`k = 0`, identity delta): objects and terrain stay together at frame time.
3. **k** = horizon x game frame rate (game frames ahead of the newest state).

Per accepted world draw (`step5InterpObject`, after the per-eye A substitution):
4. **Identity** (`ObjectTracker::observe`). Key = mesh signature (index/vertex count, stride, topology: the TACTICAL signature) + IA buffer identity
   (VB/IB GPU VA) + ordinal among this frame's draws of that signature. Candidates are the previous frame's unclaimed tracks of the same signature;
   the error against each track's prediction (position + one frame of its velocity) must be within `20 + 1.0 x |v|` units (no velocity yet: 400 units,
   the camera's cut threshold). Best score = error^2, +4 for other buffers, +1 for another ordinal. No candidate in the gate but the same buffers and
   ordinal: a **teleport** (a pooled object reused elsewhere): the track continues without history. A rotation above 30 deg in one frame is a
   teleport too. A row-scale change above 2% (size animation, LOD by scale) or a change of signature (LOD by mesh) restarts the history. Tracks not
   seen in a frame are lost; a producer frame-sequence gap or a wall gap above 4 game frames drops all history.
5. **Evaluation** (`ObjectTracker::evaluate`). B's 3x3 is split into a rotation (orthonormalised rows, quaternion) and per-row scales (a mirror is
   kept in the sign of the third scale; a sheared matrix is translated only). `-1 <= k < 0` interpolates between the previous and the newest state
   (translation lerp, rotation slerp, scale of the newest state). `k > 0` extrapolates with the last per-frame velocity and rotation rate, clamped to
   `interpx` frames and 10 deg, and only when the last two steps agree (direction within ~45 deg, length ratio 0.5..2; rotation-rate change below half
   the rate): never through a reversal, stop or launch. Otherwise the object stays at its game position (reason counted). In production k >= 0
   (t_display is never before the newest state); the k < 0 branch is the general interpolation path and is tested.
6. **Camera delta into the model matrix.** `w' = Cf + dR^T (w - Cf - dt)` on the translation, `r_i' = dR^T r_i` on the rows. Drawn through the frame's
   own per-eye A (head offset included), the object appears exactly as the display camera's eye sees it (`Md^T (w - he_d)`, derived in the WARP case
   comment). This is applied to EVERY replayed object, also those that cannot be interpolated (new, teleported, untracked beyond the cap): they are
   at their frame-time world position, seen from the display camera, so a building stays on the terrain.
7. Only the REPLAYED copies of the record change (+256..+319, both eye copies: B is the same for both eyes). The game's own record and draw are never
   touched. With k = 0 and an identity delta the bytes are not written at all.

Consumer (`Output::interp_camera`): the terrain camera (game camera or the E6 fallback) takes the frame's published `dt`, `dR` (StereoFrame::interp)
instead of running extrap:1 for that frame, so terrain, props, clouds and objects share one camera and one t_display. A frame without a plan
(world objects off, no world camera) falls back to extrap:1 when that is on, else to the frame camera.
TACTICAL (tac:1): the producer matches the game's marker sprites with the frame-time position (the sprites' own time), then replaces each tracked
draw's position by its display-time position when the frame closes; the consumer's tactical tracker, markers and radar use those, with the moved
camera. The extrap:1 marker lead is switched off for such frames (it would lead twice).

## 3. Evidence and inference

| statement | tag |
|---|---|
| Objects are replayed once per game frame into the slot images; XR reuse frames re-submit the same layers | [E] code reading (`step5WobjWhy`, `Output::pace`) |
| The game simulation is fixed-step, one step per rendered frame; 60 fps from its own limiter | [E] D-339 (6,717 frame pairs), limiter mechanism [I] |
| B (+256) is the model -> world matrix, rows = local axes, translation at floats 76..78; the VS reads A and B only for position | [E] acm-shader-dataflow.md, world_dedupe (0.49 u match to .pdl placements) |
| The world camera from A is the terrain's game camera (same axes to ~4e-7, same world space as E6) | [E] world1-impl.md section 3 |
| A replacement B gives the same pixels as moving the object and the camera rigidly | [E] algebra + WARP `case_object_interp` (real terrain shader, hand projection) |
| One part keeps its mesh signature and buffers from frame to frame; draw order among same-signature parts is mostly stable | [I] (TACTICAL already assumes the signature; the buffers are unmeasured: the live trace shows it) |
| Game objects move smoothly enough that 1.5 frames of dead reckoning is better than none | [I] (aircraft/missiles: straight segments dominate; jinking is refused by the stability rules) |
| The select-age EMA is a good predictor of the next frame's select age | [I] (test 17 select age 44 ms at 30 fps; never measured with a steady 60) |
| What the headset shows looks smoother | NOT shown. Needs the headset. |

## 4. Limits (what interp:1 cannot do)

* **No object motion above the game rate.** The objects change once per game frame. At 90 Hz the 60 Hz content is shown 2-1-2-1... headset frames
  (3:2 cadence); interp:1 removes the LATENCY (objects and the camera at the display time of the first headset frame that shows them) and the
  terrain/object slip of extrap:1 (uncap.md 3.3), not the cadence judder. SteamVR's reprojection corrects head rotation only.
* **At the game rate, unchanged:** the HUD (pitch ladder, gauges), effects sprites (smoke, explosions, clouds the game draws), the game's own
  screen-space marker sprites (boxes/labels drawn at frame time: for a near, fast target they can sit up to v x k off the interpolated object; with
  `tac:1` + suppression our display-time markers replace the matched boxes), and the replayed cockpit (camera-relative: it moves with the camera by
  construction). Objects that are not replayed (offscreen passes, refused draws) are not in the eye images at all.
* **Prediction error.** Extrapolation over k frames is wrong by the change of velocity during those frames: a 7 g turn at 300 m/s bends the path by
  ~0.5 x a x t^2 = 0.5 x 69 x 0.025^2 = 0.02 m at k = 1.5: invisible. A missile launch, an explosion pushing debris, or a target reversing course is
  held (no extrapolation) for one or two frames: the object then lags by its own k frames of motion for that frame (at most 1.5 x its per-frame step).
* **Identity heuristics.** Two same-mesh objects crossing within the gate can swap identities for a frame (the error stays inside the gate, so the
  result is still near both paths). An object pooled and re-used within the gate of its old position is treated as continuous motion.
* **Skipped producer frames** (no free slot) count as one step: the object and the camera both extrapolate a 2-step velocity for that frame
  (bounded by the ratio rule 0.5..2, which refuses the next frame instead).
* **The camera refusal couples everything.** In a hard reversal of the camera's path, the whole frame stays at frame time (that is the point: never
  slip objects against the terrain).
* **Cost.** Producer: one 96 x 96 signature-filtered scan per frame worst case, a quaternion decomposition per draw, ~80 KB of heap state; no
  allocation, no locks beyond the step-1 mutex already held. Consumer: a few float operations per frame and bounded log lines.

## 5. What real above-60 object motion would need (proposal, not built) [I]

1. Re-render our own layers on every headset frame (uncap.md 3.5): terrain/props/clouds with the camera extrapolated to that frame's display time.
2. **Phase images**: the producer replays the world draws TWICE per game frame (k and k + 0.67 for 60 -> 90 Hz) into two object image sets (one more
   pair of eye targets per slot), and the consumer composites the one whose phase matches each headset frame. Costs: the member and command caps
   (2 x 96 world members + the 16-member cockpit reserve exceed today's class_member_cap 128 and class_command_cap 1,100: both must grow, with the
   per-slot UPLOAD), a second record copy per draw and phase, and twice the object GPU time (not measured). This is the only way to give objects their own motion at the
   headset rate without making the game run faster; the tracker and planner of this track are the pieces it needs (`ObjectTracker::sample` evaluates
   a track at any k).
3. Not an option: re-executing the game's draws from the consumer later (section 1), or running the game faster than 60 (D-339).

## 6. Logs

* `xr-stereo;event=interp-armed;...;needs=gpu_copy_step5_world_objects;...` (proxy, once).
* `xr-stereo;event=interp-config;...` (consumer, first planned frame).
* `xr-stereo;event=summary-interp` every 5 s: `frames`, `planned`, `no-plan`, `k-positive`, `k-mean`, `horizon-ms-mean`, `horizon-ema-ms`,
  `cam-moved`, `cam-shift-mean/max`, `objects`, `moved`, `held`, `appeared`, `teleports`, `scale-changes`, `lost`, `write-fail`, `last-why`.
* `xr-stereo;event=interp-trace` (bounded: 120 consecutive frames every 20 s): `frame_id`, `publish-us`, `k`, `horizon-ms`, `why`, `cam-dt`, and up to
  4 objects `oN=id:kind:moved:game-position:drawn-display-position` (kind 1 appeared, 2 matched, 3 teleport, 4 scale change).

## 7. Live test plan (null driver; the driver agent)

Preconditions: `tools/mem-guard.ps1 -Report` start gate (commit <= 85%; the live XR run needs idle commit <= ~82%, D-338). Install a DLL set built
from this branch. Launch acz.exe directly (D-279), Mission 02 with the skip-intro protocol; exclude the first 15 s (8x intro clock).
1. **I0 (2 min):** the morning key set with `-TerrainRender 'auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1'` (no interp). Baseline counters.
2. **I1 (3 min):** the same + `;interp:1`. Fly straight and level for 30 s, then gentle turns, with the wingmen in view (mission 02 has the
   wingmen alongside after the start and enemies ahead).
   * Expect `interp-armed`, `interp-config`, and `summary-interp` with `planned` ~ the consumed frames, `k-positive` > 0 once the select age is known,
     `k-mean` between 0.5 and 1.5, `cam-shift-mean` ~ speed per frame x k (~18.5 x k units at cruise), `write-fail=0`, `teleports` and `appeared` a
     small fraction of `objects`. `last-why` in turns may read `direction-change` (refused frames: expected).
   * No change in summary-selection failures, end-frame or stale counts against I0; `summary-producer-cost` us/frame up by less than ~5%.
3. **Frame-by-frame comparison (offline, from the I1 log):** take an `interp-trace` window. For each object id present in consecutive frames:
   * the game positions (`oN` first triple) against `publish-us`: the per-frame step should be near constant while flying straight (fixed step);
   * the drawn display position of frame N (second triple) should match the game position interpolated at `k` frames after frame N, i.e. the
     straight-line position between the game positions of frames N+1 and N+2 at fraction k: report the mean and max error in units and as a
     fraction of the per-frame step (expect < 0.1 of a step on straight segments, larger only right after a refusal);
   * ids should persist for wingmen (no `appeared` while they stay in view).
4. **Registration:** one `xr_png_once` (or burst) frame with a wingman close beside the cockpit and the terrain below: the wingman must not sit ahead
   of or behind its expected place by ~v x k (compare with an extrap:1-only run, where uncap.md 3.3 predicts ~1.5 m slip at cruise). Smoothness
   itself needs the real headset.
5. Report: the counters, the trace error table, the cost delta, and whether any `write-fail` or unexpected `teleports` occurred. Fixture/offline
   success does not show headset smoothness.

## 8. Tests [E]

* `object_interp_offline` (CPU): options and the process-wide switch; constant velocity at 60 Hz states sampled at 90 Hz display times (exactly on
  the line for interpolation and extrapolation, clamp at 1.5 frames); reversal / stop / launch (no overshoot, resumes after two consistent steps);
  speed change beyond 2x refused; identity across swapped draw order with two same-mesh instances; disappear/re-appear; LOD by signature; scale
  change; the 96-draw cap; sequence and wall gaps; teleport gate and pooled reuse (resumes after two steps); a jump just outside / inside the gate;
  rotation teleport; slerp exact on the rotation path (yaw and a general axis, with scale 2.5 and a mirror; determinant sign kept); extrapolated
  rotation clamp; rate change refused; shear refused; the camera delta (an object compensated for the frame camera is seen exactly as the display
  camera sees it, model points too; the consumer's `apply_to_camera` turns the frame camera into the display camera; identical cameras: a no-op);
  the planner (wingman co-moving: no slip; ground object seen by the display camera; enemy at its own t_display; no horizon / no rate / camera
  reversal: nothing moves, nothing written; stationary camera: objects still extrapolate); record bytes (only B touched; unchanged transform = the
  identical bytes; non-finite: nothing written).
* `frame_pacing_offline`: the option string gains `interp`/`interpx` (describe line updated).
* `xr_stereo_output_offline` terrain case 9: the consumer applies the producer's delta to the terrain eyes; no plan -> frame camera; interp off ->
  the facts are ignored; `interp-config` / `interp-trace` lines.
* WARP `case_object_interp`: the producer planner on a moving + turning camera (150 u/frame, 3 deg/frame) and a ground vehicle (100 u/frame); the
  object drawn through the FRAME camera with the compensated matrix; the real terrain shader with the consumer camera: the landmark under the
  vehicle's display-time position lands on the object in both eyes; without the delta it does not (the check discriminates); the painted object is
  composited at its projection.

Build: `D:\b-oi` (Release, Ninja, -j2), no C1060. `object_interp_offline` 525 checks; WARP harness 879 checks (object case: landmark centroid within
0.0003 uv of the replayed object in both eyes; 0.032 uv off without the delta); Release ctest **313/313** on master 7625010 (312 + the new test),
then **315/315** after merging master 753aade (CAM-EPOCH, 314 + the new test). The terrain-camera hook sits after the CAM-EPOCH admission/outcome
block, so the delta moves whichever camera CAM-EPOCH admitted.
