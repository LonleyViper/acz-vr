# WORLD-SCALE: one evidenced metres-to-world-units factor for the world layers

Headset test 19 (D-362): "world scale feels wrong: close things (buildings, ground, runway, other aircraft) look smaller than they
should". Tags: **[E]** measured, **[I]** inference, **[U]** unknown. Offline only; nothing below was checked in the headset.

## 1. Mechanism

In VR the perceived size of an object follows from the stereo baseline (IPD) and head-motion parallax *in world units*. If the
eye offset (metres from OpenXR) is converted with `k` units per metre while the world is really `w` units per metre, the viewer
triangulates every point at `1/(k/w)` of its true distance and therefore sees it `k/w` times too small (hyper-stereo miniature).
Perceived length = `L_units / k` metres. Far objects (km) have ~zero disparity either way, so only CLOSE objects look wrong:
exactly the test-19 report.

## 2. True world units per metre (evidence)

| source | numbers | u/m | tag |
|---|---|---|---|
| Draken model (j35jk_exterior.zvrm bbox, 1 model unit = 1 world unit) | length 76.70 u / 15.35 m; span 46.84 u / 9.42 m | 5.00 / 4.97 | E |
| MiG-21 (m21bk_exterior) | 73.47 u / 14.7 m; span 34.70 u / 7.15 m | 5.00 / 4.85 | E |
| F-16C (f16ck_exterior, probe/tip rails included) | 79.16 u / 15.06 m; span 48.58 u / 9.45 m | 5.26 / 5.14 | E |
| Draken cockpit part (j35jk_cockpit) | width 3.97 u = 0.79 m at 5 u/m (6 cm at 65 u/m) | ~5 | I |
| E6 camera vs aircraft (test-19 ee-camera.log, 344 samples) | `|cam - air|` = 36.48 u = `|j35j.pvb eye|` = 36.476 u (same units) | (consistent) | E |
| HUD altitude (step50, terrain-calibration.md) | y 4000 u, ground 0, ALT 2563 ft = 781 m | 5.12 | E/I |
| HUD speed (step50) | ~790 u/s vs SPEED 324 kt (166.7 m/s) | 4.74 | I (speed varies) |
| E6 speed, test 19 (`ee-camera.log`, 90 sample pairs) | median 1378 u/s: 536 kt at 5 u/m, **41 kt at 65 u/m** (below any jet's stall speed) | ~5 plausible, 65 impossible | E/I |
| stage (m02a) | cell 1024 u, 512 cells: 102 km at 5 u/m (8 km at 65); relief 1760 u = 345 m hills | ~5 plausible | I |

**Result: world units = 5.0 per metre** (models within real-aircraft tolerance, cross-checked by altitude and speed).

## 3. Which consumer used which scale (before)

| layer | head/IPD scale | where | test-19 log |
|---|---|---|---|
| terrain, stage props, trees, our clouds, tac markers | `Conventions::head_units_per_metre` = producer's `gpu_copy_step5_units_per_metre` (WORLD-1) | `xr_stereo_proxy.cpp`, `eye_world()` in `xr_stereo_output.cpp` | `head-units-per-metre=65.00(producer-units-per-metre)` |
| replayed world objects (DrawACM_v; absolute world units, M256 row length 1) and the smoke replay | `gpu_copy_step5_units_per_metre` via `setup_poses` | `gpu_copy_step5_hooks.inl` world path (`w.pf`) | `units-per-metre=65.0;eye-offset-units=2.243,2.243` |
| model cockpit (COCKPIT-2) | `upm:5 / cscale` (1.08 in the day install) | `cockpit_model.h view_upm` | correct |
| replayed cockpit (DrawCPT_v, scale-16 M256 rows, camera-relative) and its IPD row-3 fallback | `gpu_copy_step5_units_per_metre` (65; 39.6 E9 assumption) | `setup_poses` cockpit path | its own magnified camera space |
| sky / environment | direction only (no translation) | | n/a |

So every WORLD layer moved the eye by 65 u per metre in a 5 u/m world: **13x** the IPD and head translation. A 63 mm IPD became a
0.82 m baseline (test 19: 2.243 u per eye = 0.45 m at 5 u/m), and the Draken (76.7 u) is perceived as 76.7/65 = **1.18 m** instead of
15.35 m. The 65 was tuned (test 4) for the replayed cockpit, whose draws are magnified (scale-16 M256) in camera space; it never applied
to the world.

## 4. Change

- `src/acz-vr/world_scale.h`: `kWorldUnitsPerMetre = 5.0`; `world_head_units_per_metre(upm, wscale)`; a process-wide published value.
- `xr_terrain_stage` option **`wscale:<0.25..4>`** (default 1): multiplies the world stereo baseline and head translation. >1 = larger
  baseline = the world looks SMALLER; <1 = bigger. `upm:` still overrides the base; the result is `(upm: or 5.0) * wscale`
  (`parse_stage_option`, idempotent).
- The proxy no longer copies the producer's units per metre into the terrain; it publishes the world scale when the option parses
  (log line `xr-stereo;event=world-scale;head-units-per-metre=5.000;source=evidenced;wscale=1.000;...`).
- The producer's world-object and smoke eye offsets use the published world scale (`worldUnitsPerMetreConfigured()`); without a terrain
  option nothing is published and the old value is kept. The replayed cockpit, its IPD fallback and `gpu_copy_step5_units_per_metre`
  are unchanged; the model cockpit (`upm:5/cscale`) is unchanged, so cockpit registration is untouched.
- `setup_poses` accepts [1, 100] u/m (was [20, 100]: 5 would have fallen back to 39.6). The config key stays clamped to [20, 100].

Day-install effect: terrain/objects head scale 65 -> 5.0 (no config change needed; `wscale:` only if the headset says otherwise).
The cockpit model (5/1.08 = 4.63 u/m) and the world (5.0) now agree within cscale.

## 5. Tests (offline)

`tests/acz-vr/world_scale_offline_tests.cpp` (target `world_scale_offline_tests`): option parsing/refusals/idempotence; the perceived
length of the Draken triangulated through `eye_world` with the real IPD at 10 m..1 km = 15.34 m at 5 u/m vs 1.18 m at 65 u/m;
`setup_poses` half-baseline = 0.0315 m x scale at 5 and 65, fallback outside [1, 100]; publish/fallback.
Run 2026-10-27 (private Debug build): **PASS, 43 checks** (Draken at 200 u: 15.27 m at 5 u/m, 1.94 m at 39.6, 1.18 m at 65).
Also compiled and linked: `gpu_copy_step1_offline_tests` (includes the changed step-5 hooks). Its scenarios step5-world-keys,
-world-rules and -world-stored pass; every scenario that drives draws (including step5-continuous, which never reaches the
changed world path) stops at `line 2039: allocations==before` ("Draw-path allocation of 16 bytes") in this private Debug
build, and `terrain_offline_tests` hits a CRT "vector subscript out of range" assert within 2 s (before the option tests).
Another agent's independent build shows the same terrain assert. Neither failure was confirmed against a master build here.

## 6. Live check list (headset; not run)

1. Log: `event=world-scale;head-units-per-metre=5.000`, `event=terrain;...head-units-per-metre=5.00(world-evidenced-5u/m*wscale)`,
   `world-objects;outcome=first-published;...eye-offset-units=~0.17,~0.17;units-per-metre=5.0`.
2. On the runway at mission start (taxi/take-off): runway width (~45-60 m) and hangars/buildings should look full size, a hangar
   door taller than the aircraft; lean 20 cm sideways: near ground objects shift like real objects metres away, not like a table model.
3. Wingman in close formation (20-50 m): the aircraft should read as a ~15 m jet, not a toy; compare its size against your own
   wing seen over the shoulder (cockpit model, same 5 u/m).
4. Low pass over buildings/trees (<100 m): no "model railway" look; trees ~10-20 m tall.
5. Cockpit unchanged: canopy, panel and HUD glass the same size as in test 19 (cscale 1.08).
6. Close smoke trails/missiles: depth consistent with the aircraft that made them.
7. If still too small/large, try `wscale:0.8` / `wscale:1.25` (append to `xr_terrain_stage`; the string is limited to 255 chars since D-367; 99 before)
   and report which is right; do not change `gpu_copy_step5_units_per_metre` for this (it is the replayed cockpit's scale).

## 7. Unknowns

- Real-size figures are public airframe data; model tolerances +-5% (probe, rails). The altitude/speed checks rely on HUD units
  (ft, kt) [I] and an unregistered screenshot.
- Objects replayed before the terrain option is parsed (first frames) use the old scale; with no terrain option (game world image)
  world objects keep `gpu_copy_step5_units_per_metre`.
- Whether perceived size feels right also depends on the HMD FOV mapping and the game lens on the replayed objects [U].
