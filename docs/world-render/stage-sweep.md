# STAGE-SWEEP: offline look at all 39 baked stages before the live runs

Offline only: no game, SteamVR, headset or C++ build. The baked stages in `D:\acz-extract\stage-ps-all` (the set D-333 says to install) and the
game archive were only read, except for the 12 header-only sea-flag fixes in section 4 (originals in `D:\acz-extract\stage-ps-all.bak`).
Tags: **[E]** measured here, **[I]** inference. The world renderer has been verified live only on m02a (D-313/D-316). Every other stage below is
a CPU render of the shipped look, not a comparison with the game.

## 1. Method
* `research/world-render-data/stage_sweep.py` is a numpy port of the C++ CPU reference `trace_env()` (terrain_stage.cpp). It runs with
  **env:1 look:1** exactly as shipped: the kLook* constants, haze length 21,500, far 160,000, sun disc, moon x0.11, cloud sheet, sea and river
  water, light shading and look gain. It reads the **baked `.zvrs`** (the loader's hash, env-finite, misc and stop checks are re-done) and the
  `.zvro` (prop and tree counts per model kind). The output is the display-domain colour, i.e. what a game-window screenshot would show. Props, trees,
  canopy glass and HUD are not drawn. Selftest: flat synthetic stage with a hand-computed nadir colour.
* Lens: the game's 63.2 x 38.2 deg (tan 0.6152 x 0.3460), 480x270 per image.
* Representative point P: for m02a, the mission-02 E6 start (25137, 4000, 82532). For other stages, the centroid of the .pdl placements, or the
  centroid of the non-filler .mha height chunks where the stage has no .pdl (m01a, the T4 group, v01a, v02a).
* There are 3 poses. Headings are chosen so that the three sky rows (toward / side / away from the moon or light azimuth) all appear:
  * `hi`: ground(P) + 2500, level, heading = moon (else light) azimuth (sky row 0)
  * `low`: ground(P) + 300, level, heading + 180 deg (row 2)
  * `down`: ground(P) + 2500, pitch -30 deg, heading + 90 deg
* Commands:
  * `python -I stage_sweep.py all` renders one child process per stage and waits 60 s while system commit is >= 88%. Peak RSS is about 0.3 GB.
  * `python -I stage_sweep.py sheets` builds the contact sheets.
  * `python -I stage_sweep.py summary` writes the flags and `docs/world-render/stage-sweep-metrics.json`.
  * `--fog stage|stagelight` renders the proposal variants (section 5).

Contact sheets: 6 stages per sheet; rows = stages, columns = hi / low / down. They are in `D:\acz-extract\out\sweep\` and are not committed.
* Shipped look: `sheet_1_k00a-m03n.png`, `sheet_2_m04k-m07a.png`, `sheet_3_m08k-m10m.png`, `sheet_4_m10n-m14m.png`, `sheet_5_m14n-m17a.png`,
  `sheet_6_m18a-v04a.png`, `sheet_7_v05a-v08a.png`.
* Proposal variants: `sheet_fogstage_*.png` (F from the stage data) and `sheet_fogstagelight_*.png` (that, plus ground gain = light colour / 128).
* Single images: `<stage>_<pose>[_fog<variant>].png`. Investigation images: `inv_sheet.png` (m16n / m06a / v03a with and without the sea plane)
  and `inv_m06a_sea.png`.

## 2. Automatic checks [E] (all 39 stages; the flags column of the table)
* All 39 `.zvrs` and `.zvro` load. All 39 env sections are present and valid. There are 0 non-finite colours.
* No pose has an all-black or all-white sky, and no image is blown out.
* Mean sky luminance relative to m02a: m11a 0.25x (**NIGHT**); m07a 0.61x, m16k/m/n 0.64x, v02a 0.64x, v08a 0.63x and v01a 0.74x are dark or
  dusk skies.
* **HAZE_TARGET_MISMATCH: 25 of 39 stages.** The look:1 fog colour F (85,105,101) is a constant fitted on m02a. On most stages it is far from
  that stage's own sph row-2 horizon colour, which on m02a is (85,102,98), the doc's data candidate. Examples:
  * m11a night: (24,15,12)
  * m07a: (28,37,45)
  * m16: (30,40,49)
  * m05a dusk: (44,44,58)
  * v04a / v08a ocean: (34,77,104) / (37,81,116)
* **DARK_HORIZON_BAND** (k00a, m08k/m/n, m09a, m13a, m14k/m/n, m15a, m18a, v03a): the stage's sph row 0 is darker at the horizon than at
  mid elevation, which is a plausible overcast pattern. With the look:1 stops (fitted on m02a's bright horizon glow) this gives a dark ring
  0..8 deg above the horizon. It is very visible in m09a low and m12a low.
* **Sea:** see section 4.
* Trees and props:
  * NO_TREES on 13 stages: k00a, m04x3, m07a, m11a, m13a, m17a, m18a (empty `.wpd`); v01a, v02a, v06a ('dummy' files); v08a (1,785
    placements, tree model ACM v3 unsupported).
  * NO_PROPS on 14 stages: m01a, the T4 group (m03x3, m10x3, m16x3, r01a, v05a), v01a and v02a (empty .pdl).
  * No stage drops props: drawn = .pdl minus locator-only models (m02a 100 of 105).
* Camera altitude: the ground at P is >= 4000 on k00a (4761), m01a (6488), m07a (6346) and v02a (4428). Terrain reaches above 4000 within
  10 km of P on the T4 group (6456), m05a (5152), m17a (4440) and m18a (4417). A fixed `fixy:4000` camera would be inside the hills there.
* Tiles:
  * Flag bit 11 (meaning unknown) is set on m09a (4 words), m17a/m18a (92) and v04a (5).
  * One tile covers 51-98% of the map on v01a, v03a, v04a and v08a. That is the sea placeholder tile, which is expected.
  * There are no flat or empty tiles.
  * 14-23% near-black texels on m17a/m18a and m11a (night textures with painted lights).
  * Near-white (snow) texels are 21-49% on k00a, m01a, m15a and v02a. The look gain (0.64, 0.61, **0.50**) turns them khaki.

## 3. Stage table (shipped look, after the sea fix; flags abbreviated, the full set is in the JSON)
| stage | P source (ground at P) | trees / props | flags | verdict |
|---|---|---|---|---|
| k00a | pdl (4761) | 0 / 62 | haze mismatch (brown sky, teal fog band), dark band, snow khaki, ground>4000 | suspicious |
| m01a | chunks (6488) | 722,053 / 0 | snow 49% khaki, ground>4000, no props | suspicious |
| m02a | E6 start (0) | 174,302 / 100 | reference; see risk R6 (forest) | OK |
| m03k, m03m, m03n | chunks (1384) | 60,354 / 0 | FALSE SEA removed; terrain >4000 near | OK (was broken) |
| m04k, m04m, m04n | pdl (0) | 0 / 40 | sea (bit13 0.96): plausible, but flat placeholder sea tiles show as blocks/stripes | suspicious |
| m05a | pdl (2412) | 24,407 / 34 | dusk: teal fog under a pink sky | suspicious |
| m06a | pdl (80) | 286,899 / 265 | FALSE SEA removed (city was sparkling water); sunset fog band; river not water-shaded | OK (was broken) |
| m07a | pdl (6346) | 0 / 452 | dark sky, fog mismatch, ground>4000 | suspicious |
| m08k, m08m, m08n | pdl (887) | 4,251 / 26 | dark horizon band, light == moon direction | suspicious |
| m09a | pdl (452) | 494,133 / 69 | strong dark horizon band (bright ring in low), bit 11 | suspicious |
| m10k, m10m, m10n | chunks (1384) | 60,354 / 0 | FALSE SEA removed | OK (was broken) |
| **m11a** | pdl (137) | 0 / 61 | **NIGHT: far ground fades to the day fog colour, bright teal mountains under a black sky** | **broken** |
| m12a | pdl (0) | 234,374 / 85 | fog mismatch, dark band in low | suspicious |
| m13a | pdl (320) | 0 / 62 | dark horizon band | suspicious |
| m14k, m14m, m14n | pdl (0) | 63,941 / 70 | sunset; sea + glint plausible; teal fog under orange sky; placeholder sea blocks in down | suspicious |
| m15a | pdl (1200) | 5,870 / 60 | snow tinted khaki by the gain, mild dark band | suspicious |
| m16k, m16m, m16n | chunks (1384) | 60,354 / 0 | FALSE SEA removed; dark sky, fog much brighter than the stage | suspicious |
| m17a, m18a | pdl (1678 / 1648) | 0 / 193, 177 | fog mismatch (lavender sky), bit 11, terrain >4000 near | OK / suspicious |
| r01a, v05a | chunks (1384) | 60,354 / 0 | FALSE SEA removed (= T4 group) | OK (was broken) |
| v01a | chunks (0) | 0 / 0 | sea in 70% byte-0 valleys (bit13 0.90); placeholder sea blocks (checkerboard in down) | suspicious |
| v02a | chunks (4428) | 0 / 0 | snow khaki, ground>4000, dummy trees | suspicious |
| **v03a** | pdl (0) | 1,218 / 207 | **flat city, sea tiles on only 50%: the y=0 sea plane turns the whole city into water (glints at low altitude)** | **broken** |
| v04a | pdl (0) | 3,426 / 122 | ocean + islands plausible (bit13 0.99); small facets at the coast | OK |
| v06a | pdl (2223) | 0 / 40 | brown fog band, dark flat square on the river (not investigated) | suspicious |
| v08a | pdl (0) | 0 / 12 | ocean with painted reef plausible; trees unsupported (ACM v3) | OK |

The k/m/n variants are byte-identical (D-310) and render identically. The P coordinates for every stage are in the JSON (`rep`).

## 4. Sea level: the 'y=0' guess, investigated and fixed in the baker [E]
The WORLD-2 rule put a sea plane at height 0 when more than 10% of the height cells are at byte 0. That flagged **22 stages (10 distinct
heightfields)**: m03x3, m04x3, m06a, m10x3, m14x3, m16x3, r01a, v01a, v03a, v04a, v05a, v08a.

**`.mti` tile-word bit 13 is the SEA tile flag.** It appears at real scale only on m04, m14, v01a, v03a, v04a and v08a. On those stages it
covers 90-99% of the byte-0 tile cells and 0.0-0.1% of the other cells. The sea tiles themselves are flat placeholder colours: one tile covers
51-98% of the map.

The T4 heightfield (m03/m10/m16/r01a/v05a) and m06a have about 16% byte-0 cells, **no** bit 13, and a land-coloured texture at byte 0. Examples:
* T4 at byte 0: (145,113,89) vs land (135,105,82)
* m06a at byte 0: (87,72,33) vs land (76,63,28)

On those stages the plane drew translucent bluish rectangles over hills (m16n) and turned m06a's city and river valley into sparkling water
(`inv_sheet.png`, `inv_m06a_sea.png`).

**Fix (baker, generic):** `stage_baker.sea_rule()` puts a sea plane at 0 (still flagged as a guess) only when at least 1% of the tile words
carry bit 13 **and** at least 80% of those tiles have all four corner heights at byte 0. The old rule is still available as
`--sea-level height`, and the selftest covers both cases.

**Regenerated:** m03k, m03m, m03n, m06a, m10k, m10m, m10n, m16k, m16m, m16n, r01a, v05a were rebaked from `a.arc` with the new rule. The new
file differs from the old one **only in header byte 16** (flags 11 -> 8; checked byte by byte). The payload SHA-256 is unchanged, so `.zvro` v2
snapping and stage detection (raw game files) are unaffected. `stage-ps-all/bake-results.json`, `stage-ps-all/stage-table.md` and
`docs/world-render/stage-table.md` were updated. Originals are in `D:\acz-extract\stage-ps-all.bak`.

The 10 stages that keep the sea are m04x3, m14x3, v01a, v03a, v04a and v08a:
* m04, m14, v04a and v08a render a plausible sea.
* v01a renders a plausible sea, though only 90% of its byte-0 cells are sea tiles.
* v03a is wrong (C1 below).

## 5. Causes and proposed fixes for the C++ side (not edited: no build allowed)
* **C1: per-tile sea mask.** The water test is `sea_present && hraw <= sea + 0.5`. On a flat stage (v03a: all heights 0, half the tiles land)
  that makes everything water.
  * Fix: also require tile-word bit 13. The runtime `StageData::words` already keeps the flag bits, and the shader already reads the word for
    the tile id.
  * Gate it with a new header flag (for example bit 4 "sea tiles masked", written by the baker when `sea_rule` holds) so old files keep their
    behaviour.
  * Mirror the change in `trace_env()` and the HLSL water branch, plus one WARP case.
* **C2: per-stage fog colour F.** `build_env_texture` writes the constant kLookFog.
  * Fix: use the stage's sph row-2 horizon colour `P[kPSky+8]`. On m02a that is (85,102,98) vs the fitted (85,105,101), which is inside the fit's
    +-4, so m02a is unchanged within tolerance.
  * One line, plus `test_look`'s F expectation.
  * Effect (`sheet_fogstage_*`): m11a becomes a dark night with no glowing teal horizon; the m05a dusk and m06a/m14 sunset bands match their
    skies; m02a looks the same. This is the most important fix before flying anything but m02a. **[E render / I game]**
* **C3: ground gain per stage (hypothesis).** The fitted gain (0.64, 0.61, 0.50) +-0.06 equals the m02a mapset light colour / 128:
  (83,83,68)/128 = (0.65, 0.65, 0.53). That is consistent with PS2 vertex colours where 0x80 = 1.0 **[I]**.
  * Variant `sheet_fogstagelight_*`: orange sunset ground on m14, dark night on m11a/m16, still khaki snow on m15a.
  * Adopt only after a game shot of a second stage.
* **C4: sea tile colour.** Under the game's own sea surface the bit-13 tiles are flat placeholder blocks. Our sea base keeps 75% of the tile
  texel (kSeaTint 0.25), so the blocks show (m04 stripes, m14/v01a/v03a rectangles).
  * Candidate: on bit-13 tiles, kSeaTint toward 1 (stage water colour plus waves).
  * But the v08a reef is painted in the sea tiles, so check a game shot first.
* **C5: horizon stops on overcast stages.** The look:1 stops come from m02a's glow. Where sph row 0 is darker at the horizon, a dark ring appears.
  This needs a game shot of m08/m09/m12/m13 before any change.

### 5a. C1 and C2 implemented (C++/HLSL; written without a build, see "not verified" below)
* **C1** (`terrain_stage.h/.cpp`, `xr_stereo_blit.hlsl`, `xr_stereo_d3d12.cpp`):
  * `kTileSeaBit = 0x2000` and `sea_tiles_flagged()`: at least 1% of the tile words carry bit 13 (the `sea_rule` threshold).
  * `StageData::sea_masked` is **derived at load** (`parse_stage`) and re-derived by `apply_sea_convention`. Deviation from the proposal above:
    there is no new header flag and no rebake. The bit-13 words are already in every `.zvrs`, so the mask applies to existing files. A stage
    without bit 13 (below 1%) keeps the old height-only rule exactly. That covers m02a, the T4 group and m06a, whose sea plane the baker
    already removed.
  * `sea_water_at()` is the water test in `trace_env()`, mirrored by `t_sea_water()` in the shader. It requires the plane, `hraw <= sea + kSeaEps`
    and, when masked, bit 13 in the tile word at the hit. The shader gets the mask as `t_bits` bit 13 (8192).
  * The march itself (`height_effective` / `t_h_eff`) is unchanged. Ground below the plane on a land tile still stops at the plane height but is
    shaded as land. That never happens on v03a, where the ground is at the plane.
  * Option `seamask:0` turns the old rule back on (default `seamask:1`). `sea=off` is unchanged. The proxy log shows `sea=plane-sea-tiles` when
    the mask is active.
* **C2** (`look_fog_color()`, `build_env_texture`):
  * look:1 F = `P[kPSky + 8]`, the stage's sph row-2 horizon colour.
  * Option `lookfog:fit` restores the constant kLookFog (A/B).
  * On the m02a values (85,102,98) every look:1 sky or haze colour moves by at most 3/255. That is inside the fit's +-4/255, which
    `test_look` asserts for sky, haze and a hazed ground trace.
* **Tests:**
  * `terrain_offline_tests`: `test_sea_mask` (new) covers the v03a-like flat stage with half its tiles sea, the tile boundary, the env water
    branch, `seamask:0`, `sea=off`, ground below the plane on a land tile, and the 655/656-word threshold. `test_look` now expects the stage F
    and covers `lookfog`, the m02a tolerance and a night F.
  * WARP: `t_terrain_sea_masked` in `xr_composite_warp_harness` checks that the shader matches `trace()` on a checkerboard sea mask.
* **Not verified:** nothing was compiled and no test was run (build gate: the lead builds after merge). No game, stage file or headset was
  used. `stage_sweep.py` still models the shipped look before this change: kLookFog by default, `--fog stage` = C2, and no C1 mask.

## 6. Per-stage risks for the live runs
* **R1:** Night (m11a), dusk (m05a), sunset (k00a, m06a, m14x3) and dark (m07a, m16x3) stages need C2 before a live run. Otherwise the far
  terrain glows in the day fog colour.
  * The moon on night stages is x0.11 (fitted on a daytime crescent): unverified.
  * The ground gain at night is unknown (C3).
* **R2:** Install with `install-assets.ps1 -StageDir stage-ps-all` to get the sea-flag fixes. Any older stage set still has the false seas on the
  T4 group and m06a. `sea=off` (an existing option) is the runtime workaround on a stage that shows rectangles of 'water' on land.
* **R3:** v03a at low altitude looks like an ocean until C1 lands. `sea=off` there leaves the sea part as flat painted tiles, which is the lesser
  evil for a city mission.
* **R4:** Altitude. On k00a, m01a, m07a and v02a the ground at the play area is above 4000. Use the live E6 altitude, never `fixy:4000`, and
  watch for the camera going underground at start.
* **R5:** Missing objects (by design, not bugs):
  * No buildings from our pass on m01a, the T4 group, v01a and v02a.
  * No trees on 13 stages, including v08a's 1,785 unsupported ACM-v3 trees.
  * The game's own replay still draws its objects.
  * The forest fill (`--forest 3`) exists only for m02a.
* **R6:** The m02a object file differs between sets:
  * `stage-ps-all\m02a.zvro`: 174,302 trees (no forest fill, v1, fixed tree colours).
  * `stage-w4` / `stage-perf`: 1,157,063 trees, with the pre-D-333 R/B-swapped tree texels; stage-perf is snapped v2.
  * Installing stage-ps-all therefore drops the forest fill used in D-316. Rebake with
    `objects_baker.py m02a --forest 3 --stage <stage-ps-all\m02a.zvrs>` if the forest is wanted.
  * **Rebaked and installed 2026-10-27 (handoff item 3).** Command:
    `python -I objects_baker.py m02a --forest 3 --stage %LOCALAPPDATA%\acz-vr\stage\m02a.zvrs --out <temp>`. The installed .zvrs is the
    stage-ps-all one (sha256 D5A9A0A6...). Bake time 2.9 s, commit peak 81% (start 81.8%).
    * Result: v2 (snapped, flags 1). Stage sha 2845a07b... equals the installed .zvrs payload. 47 models, 8 textures, 2,484 vertices,
      3,450 indices, 1,157,163 instances (100 props + 1,157,063 trees, of which 982,761 are forest fill). 28,391,120 bytes, payload
      sha256 OK, no ground-relative flags left, y 0..1751, no NaN.
    * Before: the lean v1 file. 174,402 instances (174,302 trees), 4,804,856 bytes, sha256 223FA8BD... It is kept as
      `%LOCALAPPDATA%\acz-vr\stage\m02a.zvro.lean-174k.bak`.
    * The new file is byte-identical to `D:\acz-extract\stage-perf\m02a.zvro` (sha256 B9FC27D5...). By section, its texture block equals
      stage-ps-all's fixed tree colours (and differs from stage-all's swapped one). stage-w4 and stage-perf therefore already carried the
      fixed tree texels for m02a, which corrects the bullet above.
    * The existing `props_offline_tests.exe` (step build, not rebuilt) was run with ACZ_PERF_STAGE_DIR set to the new file and
      ACZ_TERRAIN_W2_DIR set to the installed stage. It passed 1,158,764 checks. Python snap vs C++ snap: 1,157,163/1,157,163 y values
      bit-identical. Parse took 77 ms. Selection at step56 drew 926 instances (2,357 considered, cap not hit).
    * NOT verified live: not yet seen in the headset or game window. Re-running `install-assets.ps1 -StageDir stage-ps-all` would put the
      lean file back.
* **R7:** Stage-detection ties remain for B7R and m17a/m18a (D-310). m17a and m18a also share tile flag bit 11 (meaning unknown).
* **R8:** m06a's river (598 bit-12 words) has no dominant key colour, so it is drawn as painted texture without water shading.
* **R9:** Sun disc and glow on low-sun stages (k00a 8 deg, m06a 8 deg, m14 10 deg) is a large bright blob. No game shot shows the sun.
* **R10:** Snow stages (m01a, m15a, v02a, k00a) look khaki: the per-channel gain was fitted on green fields.

## 7. Files
* `research/world-render-data/stage_sweep.py`: numpy trace_env port, poses, metrics, flags, sheets; selftest.
* `research/world-render-data/stage_baker.py`: `sea_rule()` (bit-13 sea), `--sea-level height` keeps the old rule; selftest extended.
* `docs/world-render/stage-sweep-metrics.json`: per-stage metrics and flags, one line per stage (shipped look).
