# WORLD-2: environment visuals (sky, sun/moon, lighting, clouds, water) for the terrain renderer (D-302 overnight track)

Tags: **[E]** evidence measured or tested in this pass, **[I]** inference not verified in the game. Offline only: no game, SteamVR or headset was run; the installed
archives were read read-only with `arcread.py`; extracts and renders are in `D:\acz-extract\w2` (not committed). Build `D:\b-w2` (Release): ctest 295/295,
terrain_offline 812 checks, WARP harness 632 checks (14 new WORLD-2 shader-vs-CPU image comparisons), all after merging WORLD-1 (master `cdfdc58`).

## 1. Research findings (what the game files say)

### 1.1 Per-stage environment block: `data/mapset/map/mapset<stage>.bin` (864 B) [E unless marked]
Same layout in all 39 stages; values vary per stage in ways that match the mission mood (m05a dusk pink sky, m11a night, m14 orange sunset light).
Offsets used (f32 = float, rgb = 3 bytes, 0..255 = display colour /255; the file itself stores the light colour once more as floats at +0x2F0 = 83/255, 83/255, 68/255 for
m02a, which is how the /255 scale was established):

| offset | m02a value | meaning | evidence |
|---|---|---|---|
| +0x58, +0x5C | 0.8727, -2.269 | directional light elevation 50 deg, heading -130 deg | [I] light (sun); same layout as the moon entry; the old hand-picked `kSun` (-0.42,0.80,0.43) is this direction within ~5 deg |
| +0x68 | (83,83,68) | light colour | [I] |
| +0x80, +0x84 | (19,21,21), (110,121,105) | dark/shadow colour?, ambient | [I] (+0x84 used as ambient) |
| +0x88.. | second light (same angles, (96,96,79)) | second light set | [U] not used |
| **+0xBC, +0xC0, +0xC4** | 0.1745, -0.925, 0.85 | **moon sprite** elevation 10 deg, heading -53 deg, strength | **[E]** the pre-V game shot shows the crescent 8.0 deg right of the NE heading (bearing 53 deg, exact) and ~8 deg above the horizon (data 10 deg, ~2 deg off) |
| +0x1F4 | (70,89,84) | **water colour** | **[E]** equals the game's measured far river (70,88,82) |
| +0x1F8 | (224,225,217) | water highlight colour | [I] |
| +0x210 + 16 i, +0x21C + 16 i | 16000/15000/16500/18000, 1/1/1/0 | 4 cloud layers: altitude, enabled | [I] altitudes 5000..21000 across stages, all above the usual 4000-unit flight level |
| +0x260..+0x26C | (145,142,121), (155,153,123), (64,83,75), (66,81,75) | cloud colours (lit / dark) | [I] |
| +0x270..+0x2EF | 8 x {1, size 32..84, grey, position -0.8..0.8} | lens-flare ghost chain | [I] (matches the flare textures below) |
Not decoded: +0x00..+0x4F (16, 6144, 8192, 0.04, -0.02, 16000, 10000, (103,103,92)), +0x190..+0x1EF, +0x250, +0x300.., the second block +0xF0..+0x18F (same layout as
+0x50..+0xEF with the light heading +90 deg). `data/mapset/dd/mapset<stage>.bin` is a variant that differs in 2..289 bytes for 30 of 39 stages (identical for m02a);
which one the PC port loads is unknown [U]; `map/` is used.

### 1.2 Sky colours: `data/spheredata/map/sph<stage>.sph` (352 B = 2 blocks of 176) [E for the m02a fit]
Block 0: f32 16896, 40960 (m02a; per stage 0..40960 / 19968..51200: dome radii or fog distances [U]), then 6 rows x 4 rgbx colours at +0x10, then 2 more colour rows
(earth/fog tones), then floats 0.61, 0.02, (0.35,0.18,0.05), (0.37,0.18,0.05) [U]. Row 0 of m02a, read from the last column to the first, is the game's sky from the horizon up:
(135,156,148), (140,164,155) (a brighter glow band just above the horizon), (92,132,141), (65,100,112). With stop elevations fitted to the measured step56 profile
(0, 1.5, 6.5, 23 deg; the top segment eased out) the row reproduces all 6 measured sky rows (5.6..16.6 deg) within 8/255 (test). Rows 1 and 2 are progressively
darker versions; they are used as the sky AWAY from the moon/light azimuth (row 0 toward, 1 at 90 deg, 2 opposite) [I: no game shot looks away from the moon]. Rows 3..5
and block 1 [U] (block 1 = same with small changes; an alternate weather/time?).

### 1.3 Sky textures: `data/map/<stage>/skytex<stage>.bin` (12 GIMs, same layout in all 39 stages) [E for the images]
`0` empty, `1` flare ring, `2` sun-ray streak, `3` radial glow, **`4` the MOON (32x32, 4-bit, CLUT alpha 0x43 = translucent; a different phase per stage: crescent
m02a/m07a/m08k/m11a/m12a, half m05a, full m14k/m16n/v08a)**, `5` small dot (star/flare core), **`6` 256x256 binary blob map** (white blobs on black; m07a holds a grid
of small crescents instead [U]) = used as the cloud coverage map [I], **`7`/`8` cloud puff sprites** (64x64 / 64x128 with alpha, identical in all stages), `9` 64x32 vertical
gradient, `10` flare halo, `11` flare hexagon. Decoder: `research/world-render-data/env_gim.py` (8-bit/4-bit GIM, byte 0x12 = 1 -> PS2 CSM1 CLUT swap; v01a's 8-bit moon still looks
scrambled with it [U]).

### 1.4 Water [E/I]
`<stage>_sea.gim` 128x128 4-bit, CLUT alpha 0x0b..0x2f: a translucent wave/noise overlay [I], used as a static wave-normal map. `_ref.gim` 32x32: a sphere reflection map
(environment highlight) [I], not used. **m16n's sea is painted in the terrain tiles** (`D:\acz-extract\out\mosaic_m16n.png`: wavy grey sea texture below the coast) [E], so the
game's sea is the tile texture plus overlays, not a flat colour. `.wad` (u32 offset table + 0xFF blocks), `.wbd` (2-bit-per-cell masks in 1024-byte blocks), `.wpd`
(ascending u16 lists) look like water/shore region masks; not decoded [U]. Sea level: m16n has 16% of its height cells at byte 0 (its coast); the baker's auto rule now
flags a sea plane at 0 from 10% (was 30%) [I].

### 1.5 Other effects (report only)
`<stage>.efa` (16 KB, 0xFF/0x00 blocks: an area mask) and `.efd` (empty for m02a/m16n) = effect placements; no smoke/fog-column data found for these stages [U].
Ground darkening patches in the game shot may be cloud shadows (consistent with the blob map) or forest; not separable from one frame [I].

## 2. Implementation (default OFF; key-off and option-off output unchanged)

* **Format .zvrs v3** (`stage_baker.py`, `terrain_stage.h/.cpp`): optional env section after the water flags (header +88 offset, +92 size 84 488, flag bit 3):
  `ENV1`, 32 parameter float4s (`EnvParam` in `terrain_stage.h`: moon dir+strength, light dir, light colour, ambient, 3x4 sky stops, water, water highlight, cloud altitudes/flags/
  colours, stop elevations, misc), cloud map 256x256, wave map 128x128, moon grey/alpha 32x32. Directions are baked as unit vectors in the game heading convention
  (`dir(a,e) = (-sin a cos e, sin e, -cos a cos e)`) and rotated by the camera yaw convention (`sgn`/`yaw`) at load. A stage without the inputs (or `--env 0`) is
  written as **v2 byte-identical to before**; the C++ serializer writes v2 unless the section is present (the baked v2 files still round-trip). Loader refusals tested:
  magic, size, alignment, non-finite parameter, offsets without the flag, v2 carrying env.
* **Options** inside `xr_terrain_stage` (no new key; budget unchanged): `sky:1`, `light:1`, `cloud:1` (`clouds:1`), `water:1`, `env:1` (all). Any other value is refused.
  Log line at load: `xr-stereo;event=terrain-env;stage=..;requested=..;env-section=present|absent(rebake-v3);active=yes|no(legacy-shading)`.
* **GPU**: one more SRV `t10` (RGBA32F 256x257: row 0 = parameters, rows 1.. = maps; `kSlots` 10 -> 11), uploaded once with the stage; `t_bits` bits **4..7**
  (bit 3 is WORLD-1's object occlusion). A missing/malformed texture = the bits are not set (tested). CPU reference `trace_env()` mirrors `terrain_trace_t()` operation
  for operation (constants `k*` / `kTE*`).
* **sky:1** sky dome from the sph rows (stops above, azimuth blend C1 and fading to row 1 at the zenith so there is no seam), the **moon** sprite (additive, alpha x strength,
  half-size tan 0.8 deg) and a **sun disc + glow** at the light direction (disc ~0.45 deg, glow pow 300 x 0.25 in the light's normalised colour; skipped when the moon and
  the light coincide, i.e. night stages) [I: no game shot shows the sun]; the haze target becomes the env horizon colour along the view azimuth (WORLD-1's haze law,
  `haze_fraction`, is unchanged; only its colour).
* **light:1** terrain normal from the heightfield (central differences), `shade = (ambient + light * max(N.L,0) * cloudshadow) / (ambient + light * L.y)` per channel,
  clamped 0.4..1.4: flat ground keeps exactly its baked texel colour (the tiles already carry baked shading/shadows), slopes toward/away from the stage light get +/- ~5-15%.
* **cloud:1** one cloud sheet at layer 0's altitude: coverage = the blob map (5-tap blurred, smoothstep, opacity 0.5) repeated every 65 536 units [I], lit colour
  (+0x264) seen from above, 40% toward the dark colour seen from below, hazed by distance with WORLD-1's law and faded out with it; composited over the sky or the terrain
  in front of the hit. With light:1 the same map gives cloud shadows on the ground along the light (strength 0.3).
* **water:1** river (D-299 key texels) base = the stage water colour; sea plane base = the tile texel tinted 25% toward the water colour (the sea is painted in the tiles).
  Then a static wave normal from the `_sea.gim` map (amplitude 0.12, fading out by 20 km), Schlick Fresnel (F0 0.02) toward the env sky reflection and a specular glint of
  the highlight colour along the light (pow 256 x 0.8).
* All shading is in WORLD-1's display domain and leaves through its single sRGB conversion.

## 3. Verification [E]
* `terrain_offline_tests`: options, v3 round trip + 6 refusals, env texture layout + yaw rotation, `trace_env` with bits 0 == `trace` on 40 rays, the m02a sky row vs the 6
  measured game rows (< 8/255), horizon = haze colour (no seam), darker away from the moon, moon and sun visibility, light normalisation (flat = texel within 0.3%) and
  slope response, river colour close to (70,89,84) looking down, clouds: blob vs gap, cloud shadow darkens the ground; real v3 bakes of m02a/m16n/m07a load and re-serialise
  byte-identically (if present in `D:\acz-extract\stage-w2`).
* WARP harness `terrain environment`: the real shader vs `trace_env` for 7 cases (moon view, sun view, light, water+sea, clouds from below, clouds from above with shadows,
  all) on both eyes: max error 0.0045, 0 bad pixels; every feature changes the image; env bits 0 / no texture / malformed texture = byte-identical to the terrain without env.
* Visual (CPU reference, game lens, `ACZ_W2_RENDER_DIR` + `w2_sheet.py`): m02a pre-V pose with env:1: sky top 10% (72,109,120) vs the game (68,106,117); moon crescent
  where the game shows it (~2 deg high); m16n sea: tile texture with a slight water tint instead of WORLD-1/legacy's flat blue rectangles on the byte-0 cells.
  Sheets: `D:\acz-extract\w2\sheet-preV.png`, `sheet-views.png`; harness sheets `D:\acz-extract\w2\warp-composite-out\t_env_*.png`.

## 4. How to try it (driver)
1. Bake v3: `python -I research/world-render-data/stage_baker.py m02a` (writes `%LOCALAPPDATA%\acz-vr\stage\m02a.zvrs`; v3 needs THIS build: older DLLs refuse v3, so keep
   a copy of the v2 file for them). Pre-baked: `D:\acz-extract\stage-w2\{m02a,m16n,m07a}.zvrs`.
2. `-TerrainRender 'm02a;env:1'` (or any subset `sky:1;light:1;cloud:1;water:1`). Check `event=terrain-env ... active=yes`.
3. Compare: sky colour away from the moon (rows 1/2 are inference), clouds (altitude/size/opacity are inference; the game uses sprite puffs 7/8 at positions from map 6
   probably), the sun (position = light direction, inference), water near the river at low altitude, cost of the extra texel loads (cloud: 5 bilinear taps per pixel
   that crosses the sheet; water: 4 taps).

## 5. Not done / open
* Cloud sprites (skytex 7/8 billboards) instead of a flat sheet; layers 1..3; cloud motion (no time constant in b1); water animation.
* Lens flare (skytex 1/2/3/5/10/11 + mapset +0x270 chain) - a screen-space effect, not a world one.
* sph rows 3..5, block 1, the sph floats; mapset +0x00..0x4F, +0x190..0x1EF, `dd` vs `map`; `.wad/.wbd/.wpd`; `_ref.gim`; m07a's skytex[6].
* Moon elevation is ~2 deg higher than the game shot (dome geometry/radius effect?); ground gain/lighting magnitude needs a registered game/eye pair (as WORLD-1 notes).
