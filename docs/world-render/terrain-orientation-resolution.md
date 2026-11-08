# Terrain orientation: which convention is right? (offline, step53 + step54 evidence)

Tags: **[E]** measured here, **[I]** inference. Scripts: `research/world-render-data/orient_scan.py` (analytic river projection scan), `orient_render.py`
(contact sheet of all 8 conventions vs the screenshot). Images in `D:\acz-extract\out` (not committed): `orient_top3.png` (screenshot / RH+0 / mirror+0 renders),
`orient_sheet.png`, `eyes_pair.png` (step53 mirror:0 eye image over step54 mirror:1), `river_mask.png`, `m02a_mosaic.png`.

## 1. Pose at the capture moment [E]
The full E6 log of step54 run B exists (`D:\acz-evidence-archive\step54\20261026-222519\ee-camera.log`, `event=timeline;slot=0`, ~0.26 s cadence, same clock as the
xr-stereo log: png-once frame 413 = 193981171 = wall 22:24:15.186). Samples bracketing it: 193981029.9 air=(-7102.0, 3876.36, 114763.3); 193981286.2 air=(-6892.7, 3875.64, 114554.0).
Interpolated pose at the capture: **air = (-6987, 3876, 114648)**, camera = air + (25.7, 3.5, -25.7), heading -0.785398 rad (NE, constant), pitch -0.0022 rad, roll ~0.
No extrapolation from the step50 start pose was needed (that run's clock is unrelated; in this run the aircraft was ~48 km *behind* (25137, 82532) on the same NE track).
Along-track uncertainty: a clock mismatch of +-0.5 s is +-400 u, +-1.5 s is +-1200 u (790 u/s); I scanned +-30 km anyway. y falls 4000 -> 3876 (descending, as the HUD ALT 2563 -> 2482 says);
terrain under it = 480 u. Step53's png (frame 450, 193064640) was at air=(-19900, 4000, 127560), heading -0.785, **before** the first look event (193065771), so the look holds did not turn that capture.

## 2. Independent check from the tile data: where is the river? [E]
The "river" is the set of red-brown tiles (8x8-mip mean colour r-g > 12: 370 of 65 536 map tiles, 72 tile indices, a thin connected polyline in tile space, see `river_mask.png`).
Data frame: +X = +col, +Z = +row, no flips (D-294). Relative to the capture pose, in the data frame (x east, -z north, forward NE = (+0.707,-0.707)): the river runs just W/NW of the aircraft
(nearest river tile ~9 km west, same row) and continues NE; the nearest visible river tiles are at **forward 26-28 km, lateral 13-17 km to the LEFT** of the NE heading (right-handed frame).
At the step50 start pose the visible river is also mostly left (13 tiles left, 6 right); at step53's pose all 16 visible river points are left. A mirrored frame puts every one of them on the right.
Game screenshot (222415, same second): thin teal river band at the left edge, x 10..145, y ~975 -> 967 (rising to the right), ~7.3 deg below the horizon, i.e. ~30 km ahead and ~18 km left
(flat-ground estimate, camera 3876 u up). The right-handed prediction (river 28 km ahead, 13-17 km left) agrees; the mirrored one does not.

## 3. Scan results [E]
Camera model: level, FOV 63.2 x 38.1, game view 2421x1362 (title bar 31 px). Score = |y of predicted band at x=80 - 973| + 1000*|slope - (-0.06)| + 400*(river points predicted in the right strip x>2030, y 860-1050).
Convention = forward azimuth 135 deg + yaw offset, RH or mirrored right axis. "mirror+0" is the shipped `mirror:1` option (same forward, negated right).

| convention | at logged pose (shift 0, fine 0): nearest to observed band | band (y@x80, slope) | best score within +-1.5 km along track, +-10 deg | river pts in right strip |
|---|---|---|---|---|
| **RH +0 (shipped default geometry)** | 45 px | (980, -0.087) | **19** | 0 |
| mirror +0 (`mirror:1`) | 312 px | none | 1521 | 3 (any shift -30..+30 km: >= 243 px) |
| RH +90 | 720 px | none | 148 | 1 |
| mirror +90 | 146 px | (1025, +0.038) | 27 | 0 |
| RH +180 / mirror +180 | 1322 / 65 px | none / (913, -0.058) | 1884 / 54 | 2 / 0 |
| RH +270 / mirror +270 | 3082 / 5339 px | none | 2173 / 4425 | 0 |

Rendered contact sheet (`orient_top3.png`): RH+0 draws the thin red river entering at the left edge and rising to the right, like the game; mirror+0 draws it entering from the right edge.
The wide scan alone (+-30 km free along-track shift) is NOT discriminating (several conventions find some river point near the target at some shift); with the pose pinned by the log, RH+0 wins clearly over
mirror+0 and the 90/270 offsets. Yaw offsets 90/180/270 are also excluded physically: level flight along (+x,-z) means the view looks along (+0.707,-0.707); the only free choice is handedness.
mirror+90/+180 survive the single-band metric but contradict the flight direction, so they are not credible. Fine yaw +-10 deg does not change the ranking.

## 4. Verdict
* **Correct world convention: right-handed, +X east, -Z north, forward = (-sin h, 0, -cos h) (h = E6 rot[1]), screen-right = forward x up, yaw offset 0, river on the LEFT** [E, strong: data geometry, the
  band position/slope fit (4-45 px with no free parameters at the logged pose) and the render agree; the C++ `camera_frame()` maths is this convention].
* **But the shipped renderer with mirror:0 does not produce it** [E]: the step53 eye image (mirror:0) shows the river on the RIGHT, while the numpy RH model predicts it on the LEFT and `camera_frame`
  is RH (right = (-fz,0,fx)). The step54 eye image (mirror:1) shows a thin red line at the LEFT edge (red-pixel x percentiles 5/25/50 = 94/266/910 vs step53 median 1408).
  So on screen `mirror:1` is the right-handed view and `mirror:0` its mirror image: some stage between `camera_frame` and the pixel (anchor-frame ray x sign in `WorldComposite`/`t_right * d.x`, or a horizontal flip
  of the world window/capture) adds one extra horizontal mirror. I did not locate it (shader tests compare shader vs CPU reference, so both share it). Recommendation: make `mirror:1` the effective default
  (or fix the sign) after one confirming run; do not use yaw offsets. The earlier D-296 'river right vs lower-left' was a real mirror, not a pose-timing artefact (both poses are on the left in RH).
* Confidence: high (~90%) that RH, forward-NE is the game's convention; medium (~70%) that "mirror:1 as implemented" is the correct on-screen fix, because the step54 eye evidence is one faint far-field line and
  the central blend window hides the middle. The river is the only unambiguous landmark: the renders look kaleidoscopic/symmetric (tile flag bits 1/4/8 ignored, D-294 'tile flags unknown'), so field-pattern
  matching is meaningless until those are solved. Also open: river width/colour (the red tile is water; the game river is teal and far thinner than a 2048 u tile) and haze (game ground fades at ~56 km).

## 5. If a further capture is wanted: nose-down calibration run
Same route as step54, `-TerrainRender 'm02a;blend:1;mirror:1'` (then again with `mirror:0`).
1. After the cockpit view is up, hold the stick forward (pitch down) for **1.2 s**, release; never more than ~2 s. At 790 u/s and ~3400 u above ground, a 1.2-2 s hold at ~20 deg loses < 500 u of height: safe.
   An optional second run with a 1.2 s roll right separates left/right unambiguously (landmarks must move the right way).
2. Game-window screenshots at 5 Hz (not 1 Hz) and png-once fired inside the hold (tune png-after-new-frames so the capture is ~0.6 s after the hold starts).
3. E6 pose in the same second: the `timeline slot=0` lines are 0.26 s apart; run the `obs` series every frame (or every 4th) around the hold (the series is 'first 300 frames then every 10', so start the hold within
   300 frames of probe start or lower the cadence), and write wall-clock <-> tick mapping to `pngonce-wall-clock.txt` so each screenshot ties to a sample within +-0.1 s.
4. Score with `orient_scan.py` using the logged pitch (`rp`) and match the water band, forest patches and field corners across the pitched view.

## 6. RESOLVED (D-298): there is no extra mirror; the xr_png_once dumps are rotated 180 degrees [E]
Offline investigation (this pass). The step53/step54 conclusion "mirror:0 renders the river on the right" came from reading the dumps with only a VERTICAL un-flip.
* The dump is the whole eye image rotated by 180 degrees (rows AND columns). Proof from the cockpit, which has known asymmetry: after a vertical un-flip only, the numbered
  gauge (10/20/30) is on the RIGHT with mirrored digits and the ball handle is on the LEFT strut; in the game window (`acz-20261026-222415.png`) the gauge is on the LEFT
  and the handle on the RIGHT strut. After a 180 degree rotation (`xr_dump_upright.py`) the digits read correctly and both match the game (`D:\acz-extract\out\step53_upright.png`).
* Rotating the step53 dump (mirror:0) by 180 degrees puts the river on the LEFT, entering at the left edge, as the game and the right-handed model predict; a CPU render of the
  shipped `camera_frame` + ray construction + `trace` (real m02a.zvrs, pose from section 1, mirror:0) agrees, and mirror:1 puts it on the right. The step54 (mirror:1) dump read
  the same way has the faint river on the RIGHT, as the mirror:1 CPU render says. So the earlier "extra mirror" was an interpretation error (and mirror:1 is the WRONG default).
* Pipeline proof (deterministic): `xr_composite_warp_harness` case `terrain orientation` runs the real shader on WARP with a synthetic flat stage whose red tile block lies
  LEFT and green block RIGHT of the heading (positions derived by hand: left of f=(fx,fz) is (fz,-fx), not from `camera_frame`), for headings 0/-45/90/170 deg, with head yaw =
  anchor yaw 0/25/-40 deg: red is only in the left half of the eye image, green only in the right half; with `mirror:1` the sides swap. 503 checks, 0 failed.
* Why the dump is rotated is not established here (the harness readback of the same private-target path is upright); the previous finding (J-hud-colour-findings.md C) saw the same
  180 degree rotation in the SteamVR null-driver window. Whatever it is, it rotates cockpit and terrain alike, so the terrain is registered correctly relative to the cockpit.
* Decision: keep the right-handed default (`mirror:0`); `mirror:1` stays as an override. Read dumps with `research/world-render-data/xr_dump_upright.py` (180 degrees).
