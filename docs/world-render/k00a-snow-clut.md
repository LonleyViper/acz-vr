# K00A-SNOW: the airbase looks brown; the terrain CLUT decode was wrong (2026-10-28)

The user reported that k00a (the takeoff airbase) should be snowy/grey, not brown (headset report item 10, reference item 12:
`docs/headset-tests/2026-10-28-k00a-original-reference.webp`). This note corrects D-379's "tiles are dark brown at source" and the K00A-BROWN
diagnosis. [E] = evidence, [I] = inference, [U] = unknown.

## 1. Decode bug: the terrain `.mtc` CLUT is in PS2 CSM1 order [E]

Every 8-bit GIM in the game stores its CLUT in CSM1 order (index bits 3 and 4 swapped; `acm_lib.csm1`, `env_gim`). The terrain `.mtc` is the same,
but `build_atlas` indexed it directly. Evidence (all from `a.arc`, scripts in the agent scratchpad, reproduced by `terrain_offline_tests`):

- The palettes are sorted by brightness in blocks of 8; read raw, the blocks run 0-7, 16-23, 8-15, 24-31 (k00a entries 0-31: 16..28, 34..45,
  29..38, 38..46). The total luminance variation along the palette drops on **all 39 stages** after the swap (k00a 564 -> 382, m01a 771 -> 584,
  m02a 610 -> 460, m07a 583 -> 437, m15a 685 -> 479; minimum 4% on m13a, typically 25-35%). Neighbour-texel colour differences drop ~10% (k00a 23.6 -> 21.4).
- m02a's river key texel index 40 (D-299) read raw is the opaque red-brown (74,39,25); through CSM1 it is entry 48 = (24,52,33) with alpha 0x40, the only
  translucent entry: a water colour. m02a's "red speckles" index 111 (139,59,48, D-379) is entry 119 = (76,85,63), field olive.
- Visual: the 356-tile airbase block rendered raw shows bright/dark salt-and-pepper texels on the apron; CSM1 removes them.

Fix (this change): `build_atlas` maps texel -> `clut_entry(idx)` (CSM1). Option `tclut:raw` restores the old decode for an A/B capture; the
`terrain-loaded` log line now carries `clut=csm1|raw`. Bakes are unchanged (the `.zvrs` stores the CLUT as in `.mtc`); the water key stays a texel
index. Effect on means is small (k00a near the parked eye (36,33,29) -> (38,33,27); m02a whole set (67,67,48) -> (62,69,49)), so kLookGain's +-0.06 fit is
not invalidated, but it was fitted on the raw decode [I].

The CLUT order does NOT make k00a white: the airbase tiles themselves are a warm grey-brown aerial photo (base block mean (81,75,66)); the
mountains around it are the snow (first 256 tiles, mean ~(97,91,81) of a sepia-lit snow photo). There is no second texture bank or season set for k00a
in the archive (`data/map/k00a/*`: one `.mtd` with 772 tiles x 4 mips, one `.mtc`) [E].

## 2. What the game draws versus ours (side by side) [E]

Means of hand-picked regions (display values). Game PC = `D:\acz-evidence-archive\step104\best\07-P1-game-window-runway-fullres.png`; reference =
the user's original-game image; ours toenv:mission = `step104\runD0F-xr-final-upright.png` (the normal mission start, what the headset shows);
ours own env = `step104\best\06-P1-k00a-runway-D1-xr-NO-CHANGE.png`.

| region | reference (orig.) | game PC | ours toenv:mission | ours own env |
|---|---|---|---|---|
| sky above the runway | (101,115,143) | (97,109,131) | (54,89,101) teal | (86,87,76) brown |
| ground beside the runway | (51,49,48), (53,51,50) | (46,50,44)* | (51,46,36) | (50,43,34) |
| snowy mountain (left) | (98,97,102) | (95,95,100) | (56,66,65) | (41,39,34) |

\* HUD green bleeds into this crop. The texture structure (taxiway edge, grey band, streaks) is identical in game and ours: tiles and orientation
are right; only the colour transform differs.

- Ground: the game turns the same warm texels (36,33,29) into a NEUTRAL grey. A two-parameter fit game = texel x k + sky x f on the reference
  crop gives k = 1.09, f = 0.11 (raw) or k = 0.90, f = 0.17 (CSM1), G predicted within 1 level: the game keeps the texel near unity gain and adds
  ~11-17% of a blue-grey sky colour even at the near ground [I]. Ours keeps red and loses blue: R/B 1.42 vs the game's 1.06.
- Mountains: ours are ~40% darker and teal; the game's snow is bright neutral grey. kLookGainNeutral 0.54 halves the snow [I].
- Sky: the game's takeoff sky is blue-grey (101,115,143) in two independent sources. Neither env we can draw matches: k00a's own bake is brown in
  all 12 stops (R > B), toenv:mission adopts m02a's teal. [I] candidates: k00a's mapset/sph colours read R/B-swapped give a blue-grey sky
  ((81,105,106) .. (64,89,106)), ambient (87,111,130) and water (54,63,69); m01a's sky stop 1 (95,115,141) is also close. Not decided.

## 3. Other snow stages [I]

Texel means: m01a (154,156,156) and m15a (125,128,118) are snow stages. `look_gain_for` gives them kLookGain (0.64,0.61,0.50): white snow
(154,156,156) -> (99,95,78) khaki, the same brown-snow defect k00a had before TAKEOFF-LOOK. Not changed here (no game capture of m01a/m15a ground).

## 4. Live capture plan (driver; null driver is enough)

Goal: one registered pair per variant to fit the k00a look (gain per channel + haze colour/amount) and pick the takeoff sky.
1. Normal mission start (m02a, takeoff k00a), parked at T10 with the day config + `look:1`. Per variant, the game window full res AND our XR eye dump
   (png-once) within the same second: (a) default (`tclut:csm1`, `toenv:mission`), (b) `tclut:raw`, (c) `toenv:own`. Log lines to keep:
   `terrain-loaded;...;clut=`, `terrain-takeoff;drawn=k00a`, the eye pose.
2. Crops (2000-wide frame): sky x900-1300 y60-200; ground right of the runway x1450-1900 y650-720 and left x100-500 y650-720; mountain left
   x50-300 y380-440. Fit per channel: game = texel_csm1 x g + F x f (texel = k00a texels sampled around the logged eye, the `near.py` method in `D:\acz-evidence-archive\k00a-snow`, which also holds the mosaics, crops and probe scripts) -> replace
   kLookGainNeutral by (g_r,g_g,g_b) and set the takeoff haze colour F; verify on a second pose (T20 rolling).
3. Sky: compare the game's sky gradient rows (5 elevations) against k00a-own, k00a-own-R/B-swapped, m01a and m02a stops; the closest decides the
   takeoff env (a toenv value).
4. Optional census: the game's texture uploads during the takeoff (hash of the terrain tile / CLUT uploads) to confirm the CLUT is uploaded as stored
   (CSM1) and that no other terrain texture set is bound for k00a.
5. Snow stages: one mission-1 (m01a) and mission-15 (m15a) game-window + eye pair over snow, to decide whether they need the neutral gain.

## 5. SNOW-LOOK: batch I findings and the stage-aware look (2026-10-29)

Batch I (step111, `research/experiments/step83-verify-run1/notes.md` F2; frames `D:\acz-evidence-archive\step111\best\F2-*`). [E] unless marked.

- **"Dark green mountains" was a crop error.** The mountain crop x50-300 y380-440 lands on the cockpit CANOPY FRAME in our wider-FOV XR frame
  (35,44,28 in every variant). Registered by eye: our left ridge (x250-520 y515-560 of 2000) = (54,62,60) default, (45,45,39) toenv:own;
  the game's (94,94,98) / right (79,82,93). Ours are grey-teal snow, ~40% too dark, tinted by the haze target, not a wrong texture.
- **tclut:raw did reach the loader.** default vs raw XR frames differ only on the distant terrain/mountains (diff mask: the ridge band and
  the clouds); the near ground is bit-identical (mean |diff| 0.0015), so the near airbase ground in our frame is the PROPS (airbase models,
  propshade:game), not terrain tiles. The vsearly flight path never logged `terrain-loaded;clut=`; `flight-stage-loaded` now carries
  `clut=`, `look=`, `takeoff-look=`, `takeoff-env=`.
- **The sky colour is in k00a's own data.** `sphk00a.sph` has 6 colour rows at +0x10; the bake reads rows 0..2 (brown, e.g. (106,105,81)), but
  rows 3..4 hold (100,113,136) (+0x40, +0x44, +0x50); the game's sky is FLAT (98..105,107..113,129..133) from ~5 to ~25 deg (column profile
  x1080-1280 of F2-default-game) and the reference shows (101,115,143). What rows 3..5 mean in general is still [U] (on m02a they repeat rows 0..2).
  m01a and m15a rows 0..2 are already blue (B > R).
- **Fit** (game = ours-model lerp toward F = (100,113,136)): near props ground (49,45,35) -> game (57,53,52) needs f = 0.157/0.118/0.168 (R/G/B):
  a constant fog floor ~0.15 even at distance ~0 [I]. Snowy ridge: snow texel (97,91,81) x a + F x f = (94,94,98) -> a = 0.587, f = 0.371,
  G predicted 95; with a = g (1 - f), g = 0.93 [I, one crop]. The old kLookGainNeutral 0.54 had been fitted against brown/teal haze targets on
  pixels that are props.

Fix (`stage_look()`, terrain_stage.h): k00a = gain 0.90 neutral, every sky stop and the fog F = (100,113,136) (flat; applies with toenv:mission
and toenv:own), haze floor 0.15 (P[kPLookFog].w: terrain, props in both shading modes; CPU `look_haze` = HLSL `te_look_haze`); m01a, m15a =
neutral 0.61 (kLookGain's luminance; white snow R-B +20.6 -> -1.2), own sky, no floor; every other stage unchanged (m02a bit-identical).
CPU reference at the logged parked eye (terrain_offline_tests, real bakes): sky 15 deg (101,113,135) vs game (100,111,133); far ridges
(t > 3000, 0.5..5 deg, +-45 deg) mean (74,77,81) vs game ridges (94,94,98)/(79,82,93) (before: ours (54,62,60) live); terrain -3 deg (59,54,48).

### Live capture plan (SNOW-LOOK verification; null driver)
1. Build with this change; `xr_terrain_stage=auto;env:1;look:1;...` (day cfg), mission 02, park at T10 exactly as runF2batch.ps1 `default`.
   Log: `flight-stage-loaded;...;clut=csm1;look=...;takeoff-look=0.90/0.90/0.90,sky=profile,floor=0.15;takeoff-env=mission`.
2. Same-second game window + XR eye dump (png-once). Crops REGISTERED per frame (not shared coordinates; ours is wider FOV): game sky x1080-1280
   y60-300, game left ridge x30-280 y400-470, right ridge x1700-1950 y520-560, ground right x1500-1900 y650-700; ours: the same features found in
   the XR frame (left ridge ~x250-520 y515-560, right ridge ~x1680-1900 y545-570 of a 2000-wide frame, sky ABOVE the canopy x900-1300 y60-200 and
   through the window x1000-1150 y400-470). Pass: sky within 8/255 of the game; ridges B >= R and within 15/255; ground |R-B| <= 6.
3. Repeat with `toenv:own` (only the light/clouds differ now) and pick the default by the ridge shading.
4. If the ridges are still dark: refit the gain from the registered pair (`near.py` method), keeping the floor; if the near props ground is
   off: refit the floor alone.
5. Snow stages: mission 01 (m01a) and mission 15 (m15a), one game + eye pair each over snow, to replace the 0.61 [I] with a fit.

## 6. K00A-FIT: the K3 refit (2026-10-29, offline)

K3 (step113, `D:\acz-evidence-archive\step113\best\K3-game-G4.png` vs `K3-xr-eye0-final-upright.png`): ground ours (57,54,48) vs game (55,52,52) (R-B +9 vs +4), ridges ours ~15-20%
darker and 8-13 bluer, sky fine. [E] crops are only approximately registered (ours is a wider FOV; canopy frame and buildings intrude), so the targets are averages:
ground (55,52,51), ridge mean (88,89,93) (game ridges (94,94,98), (79,82,93)).
Model, fitted on the CPU reference (`ACZ_LOOKFIT=1 terrain_offline_tests` prints the unit-gain colour and haze fraction of ground and five ridge distance bins at the parked eye):
`display = lerp(g x lerp(c, lum(c), desat), F, floor + (1 - floor) f^pow)`. A per-channel gain alone cannot do it (the warm texel needs B gain 1.5 x R, then ground R is +6 high);
desaturation toward the luminance neutralises the warm texels and a haze exponent < 1 hazes the middle distance sooner. Grid fit sse over the 6 targets: per-channel gain + floor 73;
desat + g + floor + pow 35: g 1.12 (fit 1.20, lowered so the -3 deg ground does not exceed the game), desat 0.60, floor 0, pow 0.60, F = sky (100,113,136).
CPU at the parked eye (real bake): far ridges (84,88,96) vs game (86,88,95); ground -3 deg (60,57,53) vs (57,53,52); sky (101,113,135) vs (100,111,133). Before: ridges (74,77,81), ground (59,54,48).
m01a/m15a/m02a/default: StageLook desat and haze_pow are 0, so their env textures and shaders are unchanged (only P[29].y/.z are new). Not live-verified; no second registered pose.
If the ridges are still dark in the headset, raise g (ridge scales ~linearly), not the haze.

## 7. K00A-FIT2: live-calibrated refit from the step113 and step117 pairs (2026-10-29, offline)

Live crops (same registered crops, same T10 pose), game: sky (98,110,133), ground beside the runway (58,53,52), mountain snow slope (94,93,98).
step113 (g 0.90, floor 0.15, no desat): ground (58,55,49), mountains (70,76,84). step117 (g 1.12, desat 0.60, floor 0, pow 0.60): ground (53,48,39), mountains (78,85,97).
- **The ground is PROPS, not terrain.** The propshade:game path (xr_props_composite.hlsli) applied NO look gain, only the haze (te_look_haze of pf = tp/46080); K00A-FIT's gain and desat never touched it.
  Both live ground values solve with props albedo A = (50.6,44.8,33.6) and haze fraction h 0.15 (step113 floor) / 0.05 (step117: pf^0.6, pf 0.0063, tp ~290). A equals the CPU terrain unit colour there (ground-right bin (50.5,43,33)).
  The game's (58,53,52) needs h 0.15/0.12/0.18 per channel from A, so the look gain and desat must reach the props too.
- **The mountain crop is the 10-20k bin** (ACZ_LOOKFIT=1: 10000-20000 units, f 0.205, unit colour (65.6,63,55.7)): the CPU model of step113/117 gives (72,75,78)/(83,87,94) vs live (70,76,84)/(78,85,97).
- **New model**: per-channel gain (1.30,1.18,1.15), desat 0.80, floor 0, haze exponent 0.56, F = sky. Props (k00a only, P[29].w): look_gain x 0.92 trim (props fog is linear, ~2x the terrain fraction near the eye), then the same te_look_haze.
  CPU: near ground (58,53,51) vs game (58,53,52); mid mountains (90,91,100) vs (94,93,98) (the live pipeline read R ~6% below the CPU on this crop, so expect R ~85); sky unchanged.
- Not addressed: the sky reads 3% low live in both steps (95,106,127 vs 98,110,133).
- m02a/m01a/m15a/default: P[29].w = 0, desat/pow = 0, gains unchanged: bit-identical. Not live-verified.
