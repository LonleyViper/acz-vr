# Terrain renderer calibration against the step50 live run (m02a, "Roselein / Arlon")

Offline only. Inputs: `research/experiments/step50-ps2-probe-run1/` (`ps2-probe.txt` E6 lines, `cockpit.png` 2421x1392 window capture,
`notes.md`; untracked in the main checkout, not in this branch) and stages extracted to `D:\acz-extract\a\data\map` (m01a m02a m07a m10m m16n).
Tags: **[E]** measured here, **[I]** inference, **[U]** unknown. Scripts: `research/world-render-data/terrain_calibrate.py` (render with a
chosen convention + side-by-side with the screenshot), `terrain_calibrate_search.py` (brute-force metric). Images (not committed):
`D:\acz-extract\out\cal_s0_RH.png`, `cal_s9_RH.png`, `cal_s0_mirror.png`, `cal_s0_RH_pitch4.png` (render on top, cockpit.png below).

## 1. Stage identification

- Only m02a fits [E]: at the logged air position (25137, 82532) m02a ground is **0** units (the 480 in the older doc was the terrain at world (0,0));
  m01a (3893) and m07a (5593) put the 4003 u camera underground; m10m and m16n are sea/islands (ground 544/0, relief 8960) while the screenshot
  shows farmland, a river and low hills. m02a: relief 0..1760 u (22 height bytes), 105 pdl objects. So mission 02 = `m02a` by elimination
  plus landscape style (field patchwork, woods, river, low hills). It is not proven by a registered match (see section 3).
- Aircraft is **4000 u above ground at the start** (y=4000.0 exactly in all 10 samples), camera = aircraft + (25.67, +3.57, -25.67) (36.3 u ahead
  along the heading, 3.57 u up; identical for all samples) [E]. Under later samples ground rises 0 -> 458 u (sample 9), i.e. aircraft ~3540 u above ground.
- HUD ALT 2563 vs y 4000: ratio 0.6407. If the HUD altitude is in feet, **1 world unit = 0.64 ft = 0.195 m (about 5.1 u/m)** [I]. Cross-check: along-track speed
  is ~790 u/s (340 u per 430 ms between samples 1..9) = 506 ft/s = 300 kt vs HUD SPEED 324 (kt) [I, 7% off, speed varies]. This contradicts the earlier
  "65 u/m" guess; the cell (1024 u) would be 200 m and the stage 102 km. y stays exactly 4000 while the aircraft moves 9.8 km, so y may be a fixed/initial
  value rather than live altitude [U]; the screenshot is not time-aligned to the samples (taken ~10-60 s earlier [U]).

## 2. Calibration record (derived from compass + motion, NOT from imagery)

| item | value | basis |
|---|---|---|
| E6 `heading` / rot[1] | -0.7853982 rad (-45 deg), constant for all samples | log |
| rot[0] (pitch), rot[2] (roll) | ~ -2.4e-6, ~4e-13 rad = level flight; the `yaw=-4.2e-45` field is a denormal/garbage, ignore | log |
| forward axis | horizontal, **(fx, fz) = (-sin h, -cos h)**; h=0 points to -Z; for h=-45 deg forward = (+0.707, -0.707) | position delta over samples = (+x, -z) for constant h; HUD compass reads NE |
| yaw sign | positive yaw turns forward toward -X (counter-clockwise seen from +Y, standard right-handed rotation about +Y) | formula above (offset 180 deg: renderer azimuth a = yaw + 180 deg) |
| handedness | **right-handed**, Y up: +X east, -Z north, +Z south. Screen-right = forward x up = (+0.707,+0.707) for h=-45 | NE compass + motion (+x,-z) is only consistent with a right-handed frame; a mirrored frame would give SE/NW |
| up axis / height | +Y, height byte * 80 u, ground at y=0..1760 | D-292, section 1 |
| camera pitch/roll | pitch about the right axis (positive up), roll about forward; both ~0 in the log, sign unverified [U] | |
| camera position | E6 `campos` is used as the eye directly (aircraft + 36 u forward, 3.6 u up) | log |

The old `terrain_render.py` default camera (forward=(sin a,0,cos a), right=(cos a,0,-sin a)) is a **left-handed mirror of this**; use
`terrain_calibrate.py` defaults (`--a-off-deg 180 --sgn 1 --hand -1`) for registered work.
Dependence on the terrain data axes: it assumes tile/height rows (+Z = +row) are drawn as stored, unflipped [E, D-293 coastline check at chunk level].
So the cockpit view is "x east, z south" only up to a rotation of 90 deg about Y (x north / z west gives the same compass reading); the data cannot
separate those two by compass alone. Imagery should decide, see section 3.

## 3. Screenshot vs render [E, honest]

- FOV 63.2 x 38.1 matches the capture: the game view (2421 x 1362 below the title bar) has aspect 1.777 = tan(31.6)/tan(19.05) [E].
- Qualitative: m02a renders the same scene type: green/brown field patchwork, dark woods, hazy low hills, a river. The in-game picture is
  far hazier: the ground fades into grey-blue haze about 4 deg below the screen centre (~58 000 u ahead if flat; rough estimate) [I], i.e. the game's draw
  distance/fog end is probably ~55-60 cells, much shorter than the 250-cell window and the lighter fog of the preview. The river renders **red-brown**
  (water tiles in the CLUT/flagged tiles are not water in-game) [E]; real colours also more saturated green in game.
- Quantitative: `terrain_calibrate_search.py` computes the high-pass NCC over the four unobstructed ground regions (right of the right strut, below
  the HUD right panel, left rim, above radar), sweeping 4 conventions x 21 positions backwards along the track (0..40 km) x 5 pitches.
  Best scores 0.12-0.16 for every convention (RH 0.138, mirror 0.118, yaw-negated 0.149, left-handed default 0.156; median 0.006). **The metric does not
  discriminate conventions**: the visible ground is a few hundred hazy low-contrast pixels and the true screenshot pose is unknown. Anything claimed from the
  images beyond "same landscape type" would be over-reading.
- Side-by-side checks: the screenshot shows a pale river band entering at the left edge below the radar; the RH render at sample 0/9 shows the river crossing
  the field of view from far left to the right foreground, mirrored version the reverse. Not decisive (pose offset unknown).

## 4. Later samples (translation sanity)

Samples 1..9 keep heading and camera offset and move 9.83 km along (+x,-z) in 8.87 s wall (sample 0 -> 1 moves 4.1 km in 3.7 s, catch-up/clock mismatch [U]).
`cal_s9_RH.png` vs `cal_s0_RH.png`: the river and wooded ridge shift consistently (river nearer/right, village appears at bottom centre; terrain
height under the camera rises 0 -> 458 u), as expected for forward travel. Only one screenshot exists and it is not time-registered to any sample, so
this is a self-consistency check of the renderer, not a validation against the game. **Verdict: unregistered**.

## 5. What remains uncertain / to do for a real registration

1. A screenshot with an E6 sample at the same instant (log E6 + screenshot within one frame, e.g. via `-Ps2Probe` capturing a screen grab each sample) and a landmark: the
   river bend, the village/road junction, pdl tower. Needed to fix the 90-degree rotation ambiguity, yaw offset, pitch zero and the altitude/unit scale.
2. Altitude scale (HUD ft?) and whether y=4000 is live altitude.
3. Water/sea tiles (river renders red), sky colour/gradient, haze colour and fog range, tile flag bits (bit 10/12/13), `_dt` detail tint.
4. Camera pitch/roll signs under manoeuvring; cockpit head offset (E6 campos already includes a 3.6 u eye height, not a pilot head model).
