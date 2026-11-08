# WORLD-1: terrain render correct and registered (D-302 overnight track)

Tags: **[E]** evidence measured or tested in this pass, **[I]** inference not verified in the game. Offline only: no game, SteamVR or headset was run.
Inputs: `terrain-issues-test16.md` (diagnosis), `D:\acz-evidence-archive\headset-test16` (ee-camera / step5-producer / xr-stereo logs), the step56 game-shot table.
Build `D:\b-w1` (Release), ctest 295/295, WARP harness 594 checks, terrain_offline 706 checks, consumer `--terrain` 719 checks.

## 1. Roll sign (commit "step 1")
* `Conventions::roll_sign` default **-1** (`rsgn:1` restores the old sense). A positive E6 roll is a LEFT bank.
* [E] Regression: 9 logged test-16 rows (`campos - air - 36.3 fwd` = K x real up axis): worst error 0.018 against `camera_frame().up` with -1; the old +1 is off by >= 0.74 on the
  same rows. Hand-derived sign case (roll +0.5 at heading 0: up leans to -X, right tilts up).
* [E] WARP harness `terrain roll/pitch/heading registration`: red/green landmarks left/right of the heading; their image positions are predicted with an independent Euler model
  (`M = Ry(h) Rx(p) Rz(r)`, Rz positive = left bank) and the eye's ray map; the real shader puts them within 0.025 uv for 7 attitudes (roll +-0.5, 1.0, -1.6, 3.0 inverted,
  head yaw 25); roll +0.5: left marker above the right one; inverted: the left marker in the right half. `rsgn:+1` is shown to fail the same check.

## 2. Colour, haze, sky (commit "step 2")
* [E] Cause: the shader wrote display-domain values (CLUT/255, sky constants) as linear and the sRGB RTV encoded them again (test-16 dump top row = sRGB of the constants).
* Fix: the terrain is shaded in the game's DISPLAY domain (as the PS2 composes it: texel x light, haze mix, sky, water) and `trace()` / `terrain_trace` convert the final colour
  ONCE with the exact sRGB EOTF. CPU (`terrain_stage.cpp`) and HLSL mirror each other; WARP shader-vs-CPU tolerances unchanged.
* Haze: `f = 1 - exp(-t / 55000)` toward the measured horizon colour (130,148,141), mixed in the display domain (how the fit was measured), no start distance. The march ends at
  `far` = 160000 and the haze reaches 1 smoothly over its last 20% (`kFarFade`), so a ray that leaves the march and the last terrain sample are both the haze colour: the flat
  58 km band is gone. `kStepGrow` 0.004 -> 0.008 so 320 steps reach ~190 km (brute-force agreement still 576/576, worst 3.9 u).
* Sky: the measured elevation table (0..16.6 deg, display values) piecewise linear; 30 deg (45,90,100) is an EXTRAPOLATION [I]; constant above; below the horizon = haze.
* [E] Tests: sRGB round trip; every table row reproduced on screen within 0.5/255; haze vs formula (3 depressions) and vs the game's measured haze fractions (within 0.08);
  march end vs missed ray < 2/255; just-above-horizon sky = haze within 0.5/255.
* [E] Validation on the real m02a stage (step54 level pose, median G over azimuth 25..65 deg) vs the game's step56 rows: 1.5 deg 132 vs 138; 2.2: 134 vs 129; 2.9: 128 vs 122;
  4.4: 114 vs 111; 5.8: 115 vs 104; horizon 148 vs 148. Near ground (30 deg down, ~7 km): ours G 57 vs the game's lower ground 69..83 at a DIFFERENT pose (step56 matched view).
* [I] Ground gain left at 1 (`kAmbient/kDiffuse` unchanged): the near-ground pair suggests ~1.3x brighter, the 4-6 deg rows suggest we are already brighter; the pairs are not
  the same terrain patches. Needs one registered pair (eye dump + game shot of the same frame).
* Options: `fogl:<L>` (default 55000), `far:<u>` (160000; `fog1:` = legacy alias), `fog0:` refused with a reason.

## 3. Registration: one camera (commit "steps 3-4")
* Producer (`gpu_copy_step5_hooks.inl`) publishes per frame (`StereoFrame` additive, invalid by default):
  `game_cam_*` = the cockpit camera decomposed from this frame's first member (orientation of the game camera the objects and the marker HUD are rendered with),
  `game_eye` = the absolute camera recovered from the frame's first DrawACM_v record (`world::camera_setup` eyeAbs), `e6_*` = the E6 sample read at seam B (the frame's first
  cockpit member) into a second single-writer mailbox (`frame_cam_mailbox`, `ee_camera_probe_win.cpp terrain_frame_tick`, read-only, VirtualQuery + SEH guarded).
* [E] The decomposed game axes ARE the terrain convention: stored ACM draw 3 gives right (0.691, 0.023, 0.723), up (-0.013, 1.000, -0.019), fwd (0.723, -0.004, -0.691),
  right x up = -fwd, i.e. right-handed, NE track (test `step5-world-stored`). [E] Test-16 logs: the first published world camera (17110.4, 1851.9, 21311.9) and the E6 `cam` at
  the same moment (17110.4, 1851.9, 20879) share x/y: E6 and the ACM camera are the same world space.
* Consumer (`xr_stereo_output.cpp`): `csrc:game` (default) builds the terrain frame with `camera_frame_axes` from the frame's game camera, position = `game_eye`, else the
  E6 campos. It is cross-checked against the E6 Euler frame; beyond `gmax` (15 deg) the frame uses E6 and counts `game-refused` (catches a convention error live).
  `csrc:e6` = the E6 path only. `e6:frame` (default) uses the frame's own E6 sample instead of the newest Present-tick sample (the test-16 1-3 frame skew); `e6:latest` restores it.
  This direct per-frame copy replaces the planned ring buffer keyed by frame id: the frame carries the sample it belongs to.
  **CAM-EPOCH update (e6-anchor-free.md section 10):** with admission on (default for parsed values) a frame WITHOUT an admissible own sample no longer
  falls through to the newest Present-tick sample: only to the Present-history publication nearest to its recording start within `camwin` (50 ms), else
  no E6 camera; samples carry time/lock/stage epochs and the recording-generation tag; the gmax cross-check and the E6 eye for the game camera use only the
  SAME frame's sample; each frame records its mode (`correlated-hybrid`, `game-camera-only`, `e6-*`, `none`) and terrain-occludes-objects is refused on
  E6-only frames. "Terrain, objects and markers share one camera by construction" holds only for the correlated modes [I: phase still unproven live].
* Head translation: without `upm:` the terrain takes the producer's `gpu_copy_step5_units_per_metre` (test 16: 65 objects vs 39.6 terrain) -> one scale.
* Markers: the FRND/target marker layer is the game's own 2D pass on the world quad (game camera, game lens): with the terrain on the game camera, terrain, objects and markers
  share one camera by construction [I: needs the live check].
* [E] Tests: WARP `game` cases build the terrain from the hand camera's axes via `camera_frame_axes` with large head translations (20,-5,10 m and -15,8,0 m at 65 u/m) and the
  landmarks land where the hand camera moved by the same head offset projects them (< 0.025 uv); consumer tests: game camera + game eye used, game axes + E6 eye, 30 deg
  disagreement refused (E6 used, angle logged), `csrc:e6`, `e6:frame` vs `e6:latest`, provenance lines `summary-terrain-camera` / `event=terrain;...cam-source=`.

## 4. "Dropped objects" (commit "steps 3-4")
* [E] Test-16 producer decision summaries split by time: the 109,737 `world-record-depth-scale` early refusals (records with +360 = 1.03, objects at (17568, 1840, 21509))
  all happened BEFORE the mission (they stop at the first mission sample); during the 45 s of flight (ticks 197177234..197222390) the DrawACM_v decisions were +65,096 accepted,
  +29,785 offscreen-target (other render targets: out of scope), 0 cap / list-blocked / backoff / depth-scale. `before-frame` grew by only ~4% of seen draws in flight.
  So the "52% never replayed" is the briefing scene and the post-crash phase; in flight every eligible scene-target object draw was replayed. Accepted records include ground
  objects (model translation (12120, 512, 132150), y = ground) and the player aircraft.
* [I] Hence buildings in flight are replayed objects in the eye images (the user SAW them, rotated by the roll error); what the dropped game window still holds is the game's own
  terrain, sky and any non-ACM scenery (clouds, sun, possibly tree sprites), owned by WORLD-2 / not identified offline.
* Implemented the missing correctness piece instead: **terrain occludes replayed objects**. With the producer's `xr_world_objects_depth_fix` (object depth = 2^-10 x ACM depth,
  float depth formats) an eye pixel in the object depth band is decoded to its view depth `w = 1/(dz*1024 + 255*2^-24)`, converted to ray distance with the eye's forward axis
  (normal of the planar ray map), and hidden when the terrain hit is nearer by > 3% + 50 u (the baked heightfield is not the game mesh). Shader bit 3 of `t_bits`, option
  `occl:0/1` (default 1), active only with `xr_world_objects_depth_fix` latched (`Settings::objects_depth_fix`) and a 32-bit float depth format. Objects keep the game's own fog
  (they run the game's PSO), which now matches the terrain haze model.
* [E] WARP `terrain occludes replayed world objects`: object nearer than the ground stays (324/324 px), object behind the ground is hidden (pixels identical to the terrain-only
  image), switch off = drawn as before.

## 5. Live switches and what a driver should check
* `tools/live/vr-test-start.ps1 -TerrainRender 'm02a' -DepthFix` (the new defaults need no options; `fog0:` is refused; a note is printed without `-DepthFix`).
* Look for: `xr-stereo;event=terrain;...roll-sign=-1;head-units-per-metre=65.00(producer-units-per-metre);haze=exp(-t/55000),far=160000;cam-source=game-camera(e6-fallback);
  e6-sample=frame;...;occlude-objects=1` and every 5 s `event=summary-terrain-camera;orient-game=..;orient-e6=..;game-refused=..;eye-game=..;e6-frame-sample=..;game-vs-e6-max-deg=..`.
  Expected: orient-game ~ all frames, game-refused 0, game-vs-e6-max-deg a few degrees in rolls (E6 latency); a large value or many refusals = the game camera convention
  differs live (then `csrc:e6`).
* Not verified [I]: anything in the headset; E6 at seam B belonging to the recorded frame (EE threading); the sky above 17 deg; ground gain; object occlusion thresholds on real
  geometry; cost of the longer march (320 steps unchanged, more of them used near the horizon).
