# CFID: cockpit/aircraft model fidelity (headset test 2026-10-29 item 6)

User finding: the game's chase-view plane has far more fidelity, reflections, shadows and lighting than our cockpit/aircraft model in VR.
The user's headset day cfg did NOT have `srefl:1`, so the comparison was against our model with no reflection pass at all.
This work was offline only: no game, no install. Tags: [E] evidence, [I] inference, [U] unknown.

## 1. What the game's aircraft draw does (quantified from the step113 KA7 / step114 L1a-c `fx-air-draw` logs, the DXIL and the data)

| Feature | Game (DrawCPT = own cockpit-view LOD, DrawACM = chase/other aircraft) | Ours before CFID | Source |
|---|---|---|---|
| Passes per skin mesh | 2: flags 2 (blend off, writes dst.a = skin alpha), then flags 7 (reflection). KA7: 98 cpt flags-2, 83 flags-7 lines; L1a-c: 81/81 | 1 | [E] logs |
| Reflection blend | CPT: `DEST_ALPHA, ONE, ADD` (dst += skin_alpha * env * mask). ACM (chase plane): `BLEND_FACTOR, ONE, ADD` (34 lines/run, 70 indices, constant factor, value not logged) | none (srefl off by default and not in the day cfg) | [E] logs |
| Reflection texture | the same descriptor table `...11d4e0` for CPT and ACM flags 7 in L1a-c: one per-stage sky map (`<stage>_ref.gim`, INFERENCE from data, section 7 of aircraft-shading-reflection.md) | none | [E] table / [I] texture |
| Lit branch | `colour = min(vcol/128 * max(0, A + sum_i C_i * f(n.L_i)), 1.99)`, `f(d) = 2 d^3` when +356 == 0, else `d` (then max 0); three lights at +128/+144/+160 (columns), colours +192/+208/+224, ambient +240 | `amb + (1-amb) * max(0, n.L)`, ONE stage sun, no clamp > 1 | [E] DXIL drawcpt-vs label 88..137 |
| Light values | prop records (lighting off): light1 (74,74,70), light2 (46,46,39), light3 0, ambient (100,100,105)/255. No aircraft (+352.x != 0) record dumped: directions and the aircraft's +356 mode unknown | stage sun direction + (ambient+sun) tint | [E] values / [U] aircraft |
| Baked vertex colour | ALL 128 (= 1.0) in j35jk.mpl parts 0, 2, 3 (4723 + 6592 + 631 vertices; no AO, no baked shadow) | used (neutral) | [E] decode_acm survey |
| Shadows | no shadow maps; no cast shadows on the airframe. The chase plane's "shadow" is the lit-branch terminator (2 d^3 contrast) + the up-facing reflection mask. Offscreen 128x128 R8G8B8A8 targets: 6 ACM flags-2 draws/frame (9..19 indices, blend off, own tables 115340/115360) = small meshes, not the player's airframe: probably HUD/weapon icons, NOT a shadow projection | none | [E] logs / [I] role |
| Output domain | display domain (R16G16B16A16_FLOAT RT), colour may exceed 1 before the texture multiply | display domain, saturate | [E] |

## 2. Implemented (option `cfid:1`, DEFAULT ON; `cfid:0` inside `xr_cockpit_model` = the pre-CFID image, bit-identical)
No new config key and no change to the day value needed (the 32-key / 255-char budgets are untouched); `cfid:0` adds 7 characters.
1. **Reflection pass on by default.** `srefl` now defaults to `cfid` (an explicit `srefl:0|1` wins). Needs the `<stage>_ref.zvrt` sidecars: all 39 are
   present in `%LOCALAPPDATA%\acz-vr\stage` on this machine (2026-10-29 00:44). Without a map the term is skipped (`refl-nomap`).
2. **Engine sun response on the exterior skin** (`kMatTexAlphaNotCov` fragments): `s = max(0, 2 (n.L)^3)` instead of `max(0, n.L)`; light clamp 1.99
   like DrawCPT_v. The mean of `2 d^3` over d in [0,1] equals the mean of `d` (0.5), so the COCKPIT-3 calibrated average level stays; contrast rises
   (n.L = 1 -> 2x the sun term, 0.5 -> 0.25). Which mode the aircraft records use (+356) is [U]: the brighter, harder-edged chase plane is the reason
   for choosing it [I]. The cockpit interior keeps the calibrated Lambert term (its fit to the game's cockpit window, cockpit-render-impl section 10).
3. **Cockpit-rim sun shadow** on the interior (non-skin fragments inside the opening footprint): the light ray to the sill plane must leave through the
   opening `{|x| <= half_x, z_front <= z <= z_back}`; soft edge `smoothstep(-0.25 u, 0.25 u)` (5 cm); sun below the sill plane = shadow. Only the sun term
   is removed (ambient stays). Box from the game's cockpit part per model (`cockpit_shadow_box`): z_front = the glare shield's aft edge (vertices above the
   eye, ahead of it, |x| < 0.5 u), sill = highest side vertex between it and the eye (>= eye height), half_x = 0.9 * max |x| there, z_back = max z of the
   cockpit part. j35jk: sill 4.271, half 1.656, z -37.533..-32.131 (the eye is 3.575/-36.3). 35 of 36 bakes get a box; zoefk none. Only drawn when the
   stage sun is live (a fixed kLight shadow would be made up). INFERENCE: a single horizontal opening plane, no glare-shield height step.
Not done (no data): the second/third light (direction unknown; mapset +0x88 block has the sun's angles), a sky/ground hemisphere ambient (the game has a
constant ambient), the ACM blend-factor value (only matters for other aircraft, which we do not draw).

Code: `cockpit_model.h/.cpp` (`Options::fidelity`, `cockpit_shadow_box`, `shadow_visibility`, `sun_term`, `Constants::shb/shs`, 56 -> 60 root constants,
root signature 61 of 64 DWORDs, flags `kCFlagFid` 4 / `kCFlagShadow` 8), `cockpit_model.hlsl` (`shadow_vis`, ps_model), `xr_stereo_output.cpp`
(box passed only with the stage light; `summary-cockpit-fid` line).

## 3. Tests (offline)
* `cockpit_model_offline_tests` `test_cfid`: options (default on, srefl follows, explicit srefl wins, refusals), constants layout/flags, the 2 d^3
  response and its equal mean, shadow visibility cases (penumbra, glare shield ahead, lit behind, side rails, below the plane, hard edge), the box
  derivation on a synthetic glare shield/rails model (exterior and glass ignored), fragment colours with/without position, cfid:0 = old colour, the
  reference raster forward (identical) / back at the skin (differs). All 287100 checks pass.
* WARP harness case 14 (`c_model_cfid_*`): real vs_model/ps_model vs render_reference with a glare shield + rails + knee panel: shadow (sun ahead-high,
  max err 0.0056), lit (sun behind-high, 0.0091), 17857 panel pixels darker in shadow, cfid:0 differs, the tail skin 0.413 vs 0.305 green. 11462 checks, 0 failed.
* `xr_stereo_output_offline_tests` 278516 passed. CPU previews of j35jk (`--load ... --opts cfid:0`): a sun from the left-rear at 27 deg darkens ~4% of the
  cockpit pixels (the left console/floor under the rail); a sun ahead changes nothing visible (the panel faces aft: Lambert is already 0).

## 4. Live-check list (headset or null mirror; day cfg unchanged = cfid on)
1. Log: `summary-cockpit-fid` shows `cfid=on;srefl=on;shadow-box=yes` and `fid-frames`/`shadow-frames` rising with the stage running; `summary-cockpit`
   `refl=on;refl-map=present;refl-nomap` 0.
2. Mirror pairs, daylight m02a: wings/elevons from the cockpit at level, banked 60 deg both ways, pitched up: the upper skin brightens toward the sun with
   the sky tint, the sun-facing surfaces are visibly brighter and the terminator darker than with `cfid:0`. Compare with the game's chase view (reference
   2026-10-28-aircraft-shading-explosion-original-reference.webp).
3. If the skin is too bright/blown: `sreflk:0.5` (reflection) and/or record that the aircraft's lit mode is Lambert (+356 != 0): then the 2 d^3 response is
   wrong and should become an option default 0.
4. Shadow: turn so the sun is low on one side (bank 30-60 deg): the console under the sun-side rail darkens with a soft edge that moves with the bank; with
   the sun high behind, the panel is lit; sun ahead-high: the knees/panel top area dark. A shadow that does NOT move with the aircraft = wrong light frame.
5. A/B `cfid:0` at the same attitude: only skin (exterior) and sun-lit interior pixels change; HUD, canopy, world byte-identical.
6. Diagnostic still wanted: one DrawCPT part-3 t10 record with +352.x != 0 (the aircraft's three light directions/colours and +356 mode) to replace the
   single-sun inference.

## COCKPIT-5: F-5E cockpit parts missing in the headset (headset test 5, item 4) [E offline]

Root cause: not dropped geometry. The F-5E source has 50/50 cockpit nodes with geometry and 3382 strip triangles (baked 3380, 2 degenerate); the
exterior 46/46 and 1869/1869. The baker alpha-tested EVERY cockpit-atlas part (MF_ALPHA_TEST, runtime t.a < cut 0.5 discard). The J35 atlas has alpha
1.0 under its lit panels, so it looked complete; the F-5E atlas stores 0.1..0.5 under its main panels (nodes 7, 36, 37, 14, 42, 51 ...), so the
discard removed 3012 of 3244 drawn cockpit triangles (92.8%, 7.2% survived). 14 more types are hit (a10a 75%, f16c 71%, f16x 68%, f17a 80%, f20a 83%,
mr2d 75%, rm01 72%, x29a 83%, su32 54%, fa22 41%, f18c 35%, e18g 24%, typn 21%, ...).
Fix (model_baker.cockpit_alpha_test): texel alpha is coverage only on nodes whose game flags blend (bit0) or glass (0x2000), plus unlit nodes
(bit3 clear, unchanged). A lit non-blend node (flags 0x0008) writes alpha as a mask (the exterior-skin rule, aircraft-shading-reflection.md), so it
stays opaque. Check: research/world-render-data/check_cockpit_parts.py <mpl> <zvrm> [--old <zvrm>]. F-5E: 7.2% -> 94.5% of drawn cockpit
triangles survive; the rest is glass/blend overlays and unlit decals (alpha is real coverage there). Existing models in the acz-vr models folder must be
re-baked (bake_all_aircraft.py) to pick the fix up; the DLL needs no change. Headset confirmation pending.
## 5. 2026-10-30 SEAT-CLEARANCE: the seat was too far forward (headset test 6, `docs/headset-tests/2026-10-30d-user-report.md` item 4)

User: the F-5E cockpit is complete, but "the seat we rendered in is too far forward so when I look up my head clips into it".

**Cause [V, from the code and all 36 bakes].** The viewer's eye is the game's own pvb eye (`f05e` (0, 3.97, -21.9), no `eye:` override; HMD pose enters as
`pvb + upm * rel`, scaled about the eye by `cscale 1.15`; `zpre` is only a GPU-cost option and does not move anything). No game triangle of the F-5E lies within 0.41 m (model) behind the eye, so the seat the pilot sits against is the made-up COCKPIT-4 interior (cockpit4-impl.md section 2): `back_max = clamp((rear of the cockpit
footprint - eye) - 0.02, 0.10, 0.36)` m and the headrest front is `back_max - 0.15`. The 0.36 cap is hit on every aircraft whose cockpit part reaches 0.38 m behind the
eye (all 36 do, the shortest 0.68 m), so the headrest front is 0.21 m behind the eye in model metres = 0.24 m physical at `cscale 1.15` (0.21 m at 1.0)
on EVERY aircraft, identical for all of them. A head puts the back of the skull about 0.17 m behind the eye [I, anthropometric] and, pitching up about the neck
(pivot ~0.12 m below / 0.10 m behind the eye [I]), the occiput / back of the head swings aft to ~0.30 m. So at rest only 7 cm separate the head from the
headrest and the look-up arc enters the headrest / seat back by 5.8 cm (the head clips into the seat, as reported). The F-5E's own cockpit parts behind the head
(n7 etc., 548 triangles) start 0.50 m (model) behind the eye (`game` column) and are not the cause; the eye is correct (it is the game's point and the HUD/world
registration was fitted to it), so the seat moves, not the eye.

**Fix.** `Options::seat_back` (`seatback:<m>`, default 0.30, 0 = off): `seat_back_max(back_max_old, seatback)` = `max(back_max_old, seatback + 0.15)`: the seat is pushed back
so the headrest front is at least 0.30 model metres behind the eye (0.345 m physical at the headset's `cscale 1.15`: 0.17 m skull + 17.5 cm clearance; 12.5 cm
more than needed for the neutral head so that the look-up arc stays clear), never forward of the old placement, whole seat (back, headrest, rails, consoles' aft end)
moves as one. Option units are the UNSCALED model (like `eye:`/`clipbox:`), so the mesh stays independent of `cscale` (bit-identical across cscale, tested) and the
physical distance is `seatback * cscale`: at `cscale 1.0` use `seatback:0.34`. The interior is still clipped to the aircraft clip box (behind >= 0.68 m) and fitted
inside the skin. Log: `cockpit-model-c4 ... seatback=0.300m(headrest-front-model;physical=0.345m-at-cscale)`. Tune in the headset config, e.g.
`xr_cockpit_model=auto;...;seatback:0.36` (further back) or `seatback:0` (the old seat).

**Follow-up (step144 `xr_composite_warp` "looking down: the interior is opaque a=0.000").** On the synthetic c4 model the cockpit footprint ends 1.6 m AHEAD of the eye, so the old seat back (0.1 m under the eye) was what the centre ray hit; with the seat moved aft nothing was below the eye. The tub now follows the seat: when `seatback` pushes the seat beyond the footprint rear, the floor, side walls and rear bulkhead extend to the seat's rear + 0.02 m (`zr` in `build_mesh`; never on a real model, footprints end >= 0.68 m behind the eye, and never with `seatback:0`). Verified: the centre ray now hits the floor, `cockpit_model_offline_tests` 281843 checks and `xr_composite_warp_harness` 11475 checks pass (built by hand with cl against the worktree sources).

**Check (offline, read-only on `%LOCALAPPDATA%cz-vr\models`):** `python -I research/world-render-data/cockpit_seat_clearance.py [--cscale 1.15] [--seatback 0.30] [--strict]`
(unit test `test_cockpit_seat_clearance.py`; the interior port is `cockpit_clip_survey.interior(..., seatback=, clipbox=)`; the skin fit is not ported). Physical metres at
`cscale 1.15`: game = nearest game cockpit triangle behind the head, aft = headrest front, clear = min(aft, game) - 0.17, look = worst signed distance of
occiput/crown/back-top over a 0..90 deg look-up (negative = inside the seat). PASS = clear >= 0.10 and look >= 0.

| model(s) | before aft / clear / look | after aft / clear / look | verdict |
|---|---|---|---|
| a10a e06b e18g f01a f02a f04e **f05e** f14d f15c/e/s f16c f16x f18c f20a f35c fa22 j35j j39c m21b m29a mr2d mrgn rm01 su27 su32 su37 su47 tnd4 typn x02a x29a yf23 zoef (35) | 0.241-0.242 / 0.071-0.072 / -0.058 | 0.345 / 0.175 / +0.021 | FAIL -> pass |
| f17a (game seat parts 0.33 m from the eye) | 0.241 / 0.071 / -0.058 | 0.345 / 0.158 / +0.021 | FAIL -> pass |
| m31b (a game triangle 0.27 m from the eye, overhead-behind) | 0.241 / 0.071 / -0.058 | 0.345 / 0.099 / +0.021 | FAIL -> game-limited (the procedural seat clears; the game's own part is 1 mm under the 10 cm target, not fixable by the seat) |

At `cscale 1.0`: before 0.210 / 0.040 / -0.053; after (`seatback 0.30`) 0.300 / 0.130 / -0.017, i.e. use `seatback:0.34` there. 36 models, 0 FAIL after, 1 game-limited.
Tests: `cockpit_model_offline_tests` `test_seat` (option parse/refusals, the rule, mesh before/after, only aft movement, cscale independence, clip box still applies; NOT yet run: build
withheld by the orchestrator), `test_cockpit_seat_clearance.py` 7 tests pass.

**Live check.** Sit upright, recentre, look straight up (and up-left/right) with the head against where a headrest would be: the seat back/headrest must stay behind the head
(a dark box visible only when looking back). If it still touches, raise `seatback` (0.36, 0.40); if the seat now looks too far from the shoulders, lower it (0.26). The head
model constants (0.17 m, neck pivot) are estimates: the headset is the judge. Not touched: the eye point, the HUD anchor, the game's own cockpit geometry.
