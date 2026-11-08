# DTEX: the per-stage detail texture (`<stage>_dt.gim`)

Status: implemented behind `xr_terrain_stage ...;dtex:1` (default OFF). Offline tests and the WARP shader-vs-CPU harness pass.
It has NOT been checked live or in a headset. Evidence tags: [E] measured, [I] inference, [U] unknown.

## Problem

Up close, the VR ground is blurry. k00a's tiles are 32 world units per texel (D-378), and the cockpit eye sits about 12 units above the runway. The same
magnification applies on m02a at low altitude. The game window shows a fine grain on the near ground that our terrain does not draw. The game's own tiles
are just as smeared (D-378 K00A-BROWN: the tiles and their orientation are right). The missing feature is the per-stage detail texture. Before this
change, no code in `src/acz-vr` read it (terrain-data-inventory.md listed it as "not examined").

## The data [E]

- Every stage in `a.arc` has `data/map/<s>/<s>_dt.gim` (39 files, all 8304 bytes): 128x128, 4-bit, CLUT flag 1 (the CSM1 swap applies to 8-bit images only).
  Decoded with `env_gim.parse` and baked by `research/world-render-data/detail_baker.py`.
- The 16-entry CLUT is grey and DARK. The table below gives the values (one channel, 0..255) and the raw PS2 CLUT alpha (0x80 = 1.0) for a few stages:

| stage | mean | sd | max | CLUT alpha |
| --- | --- | --- | --- | --- |
| k00a | 4.8 | 4.0 | 20 | 0x40 |
| m02a | 15.1 | 9.4 | 35 | 0x80 |
| m03k/m03m/m03n/r01a/v05a | 15.5 | 6.6 | 39..45 | 0x40 |
| m11a | 0 | 0 | 0 | 0x40 (no detail) |
| v02a, v03a | 26.6, 9.9 | | 70, 35 | 0x04 |

- Content: a fine noise. k00a has sparse bright diagonal streaks (asphalt grain); m02a has a denser field/grass grain.

## How the game applies it

- **Blend [I].** The PS2 terrain draw itself was not captured. The dumped shaders in step94 S3 and step97 X2 (`ps2AtestCA`, 4caf6e2d, 534f29ca, 542a11d7,
  7a3c9385) are the effects/cloud pipelines, not the ground. So the blend comes from the data. A texture of values 0..45 with a CLUT alpha is the operand
  of an ADDITIVE GS pass, Cv = Cs*As + Cd. A "2x modulate" by values around 15/255 would turn the ground almost black, which the game does not show.
  - The shot's grain skewness was inconclusive (-0.1..+0.27). The sign could also be subtractive (Cd - Cs*As).
  - A zero-mean term makes both readings differ only in the grain's polarity, and it keeps the average ground colour fitted in WORLD-4 / TAKEOFF-LOOK.
- **Tiling and amplitude, FITTED [E on one shot].** `detail_fit.py` fitted these on the game-window shot `step103/runN1-shots/13-runway.png`: k00a parked,
  horizon (HUD 0 line) at y 385, f = 977 px, eye 12 units, right-hand ground x 870..990, y 430..478.
  - Method: per band, a high-pass residual sd and its lag-1 autocorrelation. These are compared with synthetic renders of k00a_dt on the ground plane at
    candidate repeats, averaged over 4 headings.
  - Repeat: 48 and 64 units per 128 texels fit equally well (ac1 error 0.031 / 0.023). 32 (0.40) and 96 (0.27) fit clearly worse.
  - Chosen: **64 units = 1/32 of a 2048-unit tile, 0.5 units (10 cm) per texel**.
  - Amplitude at S = 64: the shot's grain sd is 0.78 x the CLUT value. With k00a's alpha 0.5, that is **kDtGain = 1.5 (+-0.3)** times the Cs*As term.
  - Shot residual sd is 0.4..1.0 / 255. The grain is subtle in the game too.
- **Distance fade and LOD [I].** The game's LOD for the detail pass is unknown. The grain is measurable in the shot out to at least ~650 units.
  - Here, trilinear mips of the zero-mean term average it out with distance. Detail lod = log2(t * kLodScale * 64), the tile rule at the detail's texel size.
  - An explicit smoothstep fade runs from 1500 to 3000 units. Beyond 3000 the term is not computed.
- **Which tiles [U].** The term is applied to all LAND hits, before the D-299 river blend and the haze. It is not applied to sea-plane water. Runway and
  field tiles are not distinguished. The game may restrict the pass to some tile classes.

## Implementation

- **Asset.** `<stage>.zvdt` sits next to `<stage>.zvrs` (a separate file, so every existing bake stays valid).
  - Header (128 bytes): `ZVDT`, v1, 128, 128, id[8], SHA-256 of the payload, SHA-256 of the source .gim. The rest of the header is zero.
  - Payload: RGBA8 128x128, decoded CLUT rgb plus the RAW PS2 alpha.
  - Bake: `python -I research/world-render-data/detail_baker.py all [--out <dir>]` (default `%LOCALAPPDATA%\acz-vr\stage`). It reads `a.arc` read-only.
  - `--selftest` covers the GIM decode and the format.
- **Runtime.** With `dtex:1`, `acz::terrain::attach_detail` runs at every stage load: the mission stage, the takeoff stage (its own detail) and each flight
  stage. It loads and validates the file, the id must match the stage, then `build_detail` encodes it.
  - Encoding: D = amp * rgb / 255 - mean, amp = alpha/0x80 * kDtGain. Mips are box means of the float term, quantised per level to
    byte = 128 + 255 * D. That is 8 levels and about 21.8 KB.
  - Log line: `xr-stereo;event=terrain-detail;stage=<s>;role=mission|takeoff;state=loaded|off|load-failed(<status>)|stage-mismatch|bad-content`.
    The mission line also carries the constants.
  - Any failure means no detail and the previous picture.
- **GPU.** No root-signature or SRV change. The detail rides in 5 extra slices of the existing tile atlas (t9):
  - Slices ntiles+0..3, mip 0: the 128 level as 64x64 quadrants.
  - Slice ntiles+4, mips 0..6: levels 1..7.
  - `t_bits` bit 14 is set only when the atlas carries them.
  - `t_detail()` in `xr_stereo_blit.hlsl` does wrapped bilinear `Load`s for 2 levels (8 loads) on near land hits only.
  - Memory: 5 x 32 KB of upload/atlas.
- **CPU reference.** `detail_term()` and the land branch of `trace_env()` in `terrain_stage.cpp`. The shader mirrors them operation for operation (constants `kDt*` / `kTDt*`).
- **Tests.**
  - `terrain_offline_tests` `test_detail`: option parsing, format round trip and every refusal, encoding (zero mean, the 1x1 level = 128, known texels),
    periodicity in kDtRepeat, fade, and NaN. It also checks `trace()` = the old colour + the clamped term near, and is byte-identical beyond the fade.
    It checks a real `k00a.zvdt` when one is installed (verified with a scratch LOCALAPPDATA).
  - `xr_composite_warp_harness` `case_terrain_detail`: the real shader vs `trace()` with the detail. Near and fade views both eyes: 0 bad pixels,
    max error 0.0047 / 0.0098. The detail changes the picture. A detail-less atlas is unchanged and deterministic.

## Uncertainty and the next captures

The blend sign, the exact repeat (48 vs 64), the fade distance and the tile classes are not established. They come from one 1280x720 desktop shot
(downsampled) at one pose. Constants live in `terrain_stage.h` (`kDtRepeat`, `kDtGain`, `kDtFadeNear/Far`) and are mirrored in the shader.

Live check list (needs the user; nothing below has been run):

1. Bake and install: `python -I research/world-render-data/detail_baker.py all`. This writes `%LOCALAPPDATA%\acz-vr\stage\*.zvdt`.
   Expect k00a `mean 4.8 sd 3.99 alpha [64]`.
2. Add `;dtex:1` to the existing `xr_terrain_stage` value (keep it within the 255-character buffer).
   The log must show `terrain-detail;...;state=loaded` for the mission stage and for the takeoff stage.
3. **Registered pair at k00a parked (T10).** Take a game-window shot (full resolution, not the desktop-scaled window) and an XR eye dump of the same parked
   moment, with `look:1` and `toenv:mission`.
   - Run `detail_fit.py` on both. Our grain's repeat and sd should match the game's within the fit spread (48..64 units, sd ratio 1 +- 0.3).
   - Also compare the grain polarity (bright vs dark streaks), which settles additive vs subtractive.
4. **m02a 60 degrees down.** Use the debug camera over the m02a start area (step103 N2 `09-N2-down60` pose), plus the game window at a comparable low pass
   if one can be flown.
   - Check that the grain shows at m02a's amplitude (alpha 0x80: about 2x k00a per CLUT unit) and does not swim.
   - Check that the red speckle in `09-N2-down60` is unchanged: it is in the tiles, not the detail.
5. Look for a visible fade ring at 1500..3000 units, and for shimmer while moving (the mip choice). If either shows, adjust `kDtFadeNear/Far` or the
   lod rule and log the change.
