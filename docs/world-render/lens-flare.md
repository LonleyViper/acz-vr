# LENS-FLARE: our own head-look lens flare (2026-10-29)

## Why
Headset test 2026-10-29 item 1: looking at the sun shows NO lens flare. The game's flare follows the GAME camera (plane heading) and its
REV_SUBTRACT quads are skipped in the HUD replay since D-391 (they turned black there). The user wants a flare that follows where the HEAD points
and only while the sun is in view (confirmed 2026-10-29), and accepted our own effect.

## What
`lf_over` in `src/acz-vr/xr_stereo_blit.hlsl`, applied to the final colour of every composite eye pixel (modes 2/3/4) after the eye effects
(smoke/sprites). CPU mirror: `src/acz-vr/lens_flare.h` (same constants, same operations).

- **Sun:** the stage light `P[kPLight]` (world/stage space, the direction of our sun disc `te_sky`). No flare when the stage has no light,
  when the light is the moon (`kTESunMoonSame`), or below the horizon (smooth fade over sin(elevation) -0.02..0.06).
- **Head look:** the sun is taken into each eye's anchor frame through the eye's stage basis (`t_right/t_up/t_fwd`) and projected with that eye's
  own ray map (`w_base/w_ux/w_vy`), the same rays the terrain/sky march uses. The geometry lives on the eye's tangent plane at unit distance
  along its optical axis (the ray-map normal), so it is independent of FOV and aspect and moves with the head, not with the plane.
- **In view only:** gate = 1 while the sun's image position is inside the eye image, smooth to 0 at 0.1 uv outside (`kViewMargin`); plus angle
  terms of cos(sun, optical axis): core/rays/ring from 55 deg off axis (linear ramp), ghosts from 45 deg (squared ramp).
- **Elements:** gaussian core + exponential bloom, 6 long rays + 6 thin rays rotated 30 deg, a faint chromatic halo ring (radius 0.40 on the
  tangent plane, about 22 deg), 4 coloured ghost discs at `s * (1 - t)`, t = 0.55/1.35/1.75/2.30, on the line through the lens centre.
  Core colour = lerp(white, stage light colour, 0.5); ring/ghosts take the light colour.
- **Colour space:** added in DISPLAY space like the sprites: `out = decode(min(1, encode(under) + F))`; alpha unchanged. A pixel whose
  flare is below 1/1024 is returned untouched (no occlusion work there).
- **Occlusion (no flicker: continuous, no history):** terrain: 12 samples along the sun ray from the eye (quadratic spacing up to the stage's max
  height, capped at the march end), soft clearance 20 units + 2 % of the distance. Cockpit/objects: 5 replay-depth taps at the sun's image
  position (centre, +-0.006 uv), each covered tap removes 20 %; this is the view-space cockpit mask. Clouds do not occlude.
- **Memory:** none. The switch and strength are the env texture's unused row-0 texel `P[32]` (`kPFlare` = (k, 0, 0, on)), written by
  `build_env_texture`.

## Options (inside the existing `xr_terrain_stage` value; no new config key)
- `lflare:0|1` - default 1 for every parsed value (`parse_stage_option`); `lflare:0` turns it off (P[32] = 0: `lf_over` is the identity).
  The struct default is 0 so hand-built test conventions keep every existing image.
- `lflarek:<0..3>` - strength (default 1; 0 = off).
- Needs the terrain with `sky:1` or `env:1` (the env texture and the sun disc). Without them nothing changes.

## Limits
- The model cockpit (COCKPIT-2) is drawn after this pass: it covers the flare where it is opaque. Its occlusion of the sun is approximated by the
  replayed cockpit's depth band.
- The HUD and marker layers are composited by the OpenXR runtime above the eye image (the flare is under them).
- Cost: the per-pixel term is ~60 ALU plus, where the flare is visible, the 12-sample terrain test (48 height loads, cache-uniform) and 5 depth
  loads. Not measured on the real GPU yet (`xr_gpu_bench`).

## Tests
- `lens_flare_offline` (68 checks): options and the env texel (only texel 32 differs), projection (centre, pitch, head yaw, peak = projected sun,
  plane yaw compensated by the head = same flare), in-view gate, behind/below horizon, monotonic angle falloff, ghost on the centre line,
  terrain occlusion (flat, wall, above max height, continuity < 0.1 per unit of eye height), depth taps, eval/composite.
- WARP harness `t_env_flare` (real shader vs the CPU mirror over trace_env): max_err 0.004, 0 bad pixels per eye; a turned head matches the
  mirror; flare on vs off differs; a view away from the sun is byte-identical with the flare on.

## Live check list (headset)
1. Look straight at the sun (m02a, light el 50 deg): bright core with a 6-ray starburst and a faint ring; both eyes agree (no double image).
2. Turn the head slowly while the plane flies straight: the flare stays on the sun and the ghosts swing through the view centre.
3. Turn the head away: the flare fades as the sun nears the image edge and is gone once it leaves the view.
4. Pitch/roll the plane with the head still: the flare follows the sun in the world, never the plane's nose.
5. Sun behind the canopy frame/airframe or a mountain: the flare fades out smoothly, no flicker at the edge.
6. Night/moon stages and sun below the horizon: no flare.
7. `lflare:0`: no flare (the eye image as before); `lflarek:2`: twice as strong.
8. Frame time looking at the sun vs away (perf HUD / xr-stereo log): no visible drop.

## 2026-10-30 headset session (step133, m03k): why no flare, why a black world, and the run proof

Evidence: `D:\acz-evidence-archive\headset-2026-10-30-session\live-1144` (xr-stereo.log; times are GetTickCount ms).

**Was the flare active?** By configuration yes: `lflare` defaults to 1 (`parse_stage_option`), the cfg has `env:1`, the stage loaded `env=yes`, and m03k's baked light is
50 deg elevation, not the moon (research `stage_sweep` metrics: `light_elev_deg 50`, `light_is_moon false`). The flare needs no E6 sample or lock: it reads the env light,
the P[32] switch and each eye's terrain-camera basis (`te.right/up/fwd`). The camera basis was the problem: from 154209.6 s to the end of the log (about 470 s) every
terrain frame ran in `mode=e6-only-game-refused` with a CONSTANT E6 sample (eye 2,2,2, constant orientation), because the angle cross-check refused the moving game
camera (`summary-terrain-camera` game-vs-e6 144..179 deg, `game-refused=300` per window). The flare's sun position therefore came from a frozen, wrong camera
(the sun is about 64 deg off that camera's axis, outside the 55 deg cone), so a head turn could not bring it in view, and no log line could have said so.
No flare telemetry existed, so the logs neither prove nor disprove a flare frame; `summary-lensflare` below fixes that.

**Black world (same cause).** The E6 block went stale around 154209 s (`e6-locate: aircraft-at-origin`; ee-camera `air=2,6.59e-10,45.3 cam=2,2,2 rot=2,2,2`). The sample still
passed the sanity limits and the game camera was refused (2400 of 2400 logged frames). In 73 healthy hybrid frames the game eye and the E6 eye are the same point
(distance 0.00-0.07 units); in the refused frames they are 125,606-164,613 units apart. The terrain camera was then at (2, 2, 2): the m03k height at (2, 2) is **2641**
(`stage_sweep.Stage.height_eff`), i.e. the eye was buried about 2639 units inside the map-corner mountain. The CPU reference trace (`stage_sweep.Stage.trace`) from that
eye gives a uniform (9, 9, 13) image for every look direction (the same directions from the real aircraft position give luminance 70-110 with sky and land); from an eye
above that ground it renders a normal sky/land view. No compositor-level failure exists in the log: `world-hold` held=0 expired=0 capture-failed=0, `world-composite` refused=0,
`summary-terrain` no-camera=0 backend-failed=0, `summary-decouple` reuse-fallback=0 no-views=0 (all windows), so the black frames were rendered content, not a hold/fallback.
The hud-menu enters (154192, 154210, 154213-154316, 154352, 154361, 154363-154570, 154679) are pauses with a frozen game camera (objects static in `interp-trace`) and do
not black the world. The "aircraft samples zero 154501-154603" window is the same stale block, not a separate event. Not proven from logs: the exact second the user saw
black (no per-frame luminance telemetry); the buried camera makes everything from 154209 on black or wrong, which matches "went black" after a normal hybrid start.

**Fix (E6-PLAUSIBLE, `terrain_camera.h e6_eye_agrees`, `xr_stereo_output.cpp`).** The E6 sample is the same aircraft as the game camera's eye: when the game eye is valid and the
E6 eye is farther than `kE6EyeAgreeUnits` (2000 units; real data 0.07 vs 125,606) the E6 sample is implausible and may not veto the game camera (mode game-camera-only, objects
registered again, no cockpit-anim update from the garbage sample). A plausible E6 sample keeps the old angle cross-check unchanged. Events: `event=e6-implausible;state=enter|exit`
(one line per transition) and `e6-implausible=<n>` in `summary-terrain-camera`. Not covered: E6-only frames with no game camera at all (still the E6 eye).

**Flare telemetry.** `event=lensflare-config` (per stage generation): env, sky bit, switch, k, light, light-is-moon, sun elevation. `event=summary-lensflare` (every summary window):
`frames` (terrain composites), then the per-frame reason from the shader's early-outs (CPU mirror `lens_flare.h classify`): `active` (some eye has a non-zero flare term,
pre-occlusion), `off`, `no-env`, `no-light`, `moon`, `below-horizon`, `behind` (sun more than ~55 deg off both eyes' axes), `out-of-view`; plus `peak-gate` (0..1, pre-occlusion,
pre-k), `best-cos`, `sun-sin-el`, and totals. A run proves the flare fired when `total-active > 0` while looking at the sun; `behind` all flight means the head never faced the sun
(m03k: sun at elevation 50, heading -130). The terrain/cockpit occlusion on the GPU is not mirrored.

**Tests.** `lens_flare_offline` (classify/peak_gate cases incl. the m03k light), `cam_epoch_offline` (`test_e6_plausible`: six real camlag rows, the limit, NaN).

**Live checks.** (1) xr-stereo.log has `lensflare-config;...sun-elevation-deg=50.0` after the stage install and `summary-lensflare` lines; (2) no long `mode=e6-only-game-refused`
stretch while `e6-implausible` counts (mode shows game-camera-only); (3) look at the sun: `active` > 0 and the flare is visible; (4) the world follows the plane all flight.
