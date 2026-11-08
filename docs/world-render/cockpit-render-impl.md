# Model-based cockpit/aircraft renderer (COCKPIT-2): loader, 6DoF per-eye pass, replay replacement, HUD anchoring

Track COCKPIT-2 (D-302 overnight mandate, design in docs/world-render/model-decode.md section 6). Status: code written, built (Release, D:\b-c2), tested
offline incl. the real shader on WARP; first integrated null-driver run in step62 (D-309). **COCKPIT-3 (D-311, sections 10-14)** fixed the colour
pipeline, the floor/nose, the HUD scale and added the stage light, measured offline against the step62 images (built D:\b-c3, not yet run live).
Tags: **[V]** verified by a test or a measurement in this repo, **[I]** inference / unverified.
**COCKPIT-4** (cockpit4-impl.md): canopy glass transmission pass (`canopy:`, `tint:`, `refl:`, `dirt:`), procedural interior (`interior:`, `stick:`;
replaces `floor:1` by default), control-surface/stick animation (`anim:1`), 48 root constants, `.zvrm` flag 8; sections below describe COCKPIT-2/3.

## 1. What it is
Default OFF. With `xr_cockpit_model=<id>[;opt:val...]` (and `xr_world_in_projection`), each eye of the projection layer gets the baked aircraft model
(`%LOCALAPPDATA%\acz-vr\models\<id>.zvrm`, the `vrcockpit` bake: detailed cockpit part 2 + exterior airframe part 0) drawn over the eye composite,
per eye, from the tracked head with full 6DoF, depth-tested, back-face culled, alpha-tested and with translucent glass. The game's replayed cockpit is
no longer composited (its depth band is dropped) while the replayed world objects, the world/terrain layer, the effects layer, the HUD quad and the
marker quad are unchanged. Looking left/right/back/down shows the canopy frame, wings, spine and fin of the player's own aircraft (exterior part 0);
the exterior pilot/seat/low-detail tub are hidden (they would sit in the viewer's face). Any failure (bad option, missing/corrupt file, mesh refusal,
GPU resource/PSO failure) keeps the replayed cockpit and logs `xr-stereo;event=cockpit-model-disabled;reason=...` (once).

Layer order per eye (inside the one projection image): world/terrain -> effects (mode 3/4, window rays) -> replayed objects (object depth band)
-> **model cockpit/airframe** (opaque, then glass) ; then the HUD quad layer and the marker quad layer are composited by the XR runtime on top, as before.

## 2. Files
| file | role |
|---|---|
| `src/acz-vr/cockpit_model.h/.cpp` | SDK-free: `.zvrm` v1 loader/validator + serializer (layout = `model_baker.py`), option parser, visible-mesh build (GPU vertex stream, texture slots, blend order, HUD-glass locator), `eye_camera` (pose composition + projection), CPU reference rasteriser, synthetic test model |
| `src/acz-vr/cockpit_model.hlsl` | `vs_model` / `ps_model` (new file; fxc at build time -> `cockpit_model_vs.h` / `cockpit_model_ps.h`) |
| `src/acz-vr/xr_stereo_d3d12.cpp` | lazily created model pipeline (own root signature, opaque + blend PSOs, D32 depth, raw vertex buffer, RGBA8 textures, one-time upload), `draw_model` after each eye's composite and in the png_once capture |
| `src/acz-vr/xr_stereo_blit.hlsl` | 3 lines: `mode_bits` bit 17 (`kModeBitModelCockpit`) drops the replayed cockpit band (CPU mirror `model_cockpit_cover`) |
| `src/acz-vr/xr_stereo_output.{h,cpp}` | `CockpitRuntime`, `Settings::cockpit`, `BlitJob::cockpit*`, per-eye constants from the composite's views, counters, log lines |
| `src/acz-vr/xr_stereo_proxy.cpp` | parse + load + build + log; `hudglass:1` HUD distance |
| `src/acz-vr/runtime_config.*`, `diagnostics_log.*` | the key `xr_cockpit_model` and its latch (with `xr_world_in_projection`) |
| `tests/acz-vr/cockpit_model_offline_tests.cpp` | loader/options/mesh/camera/reference/config tests; `--load <zvrm> [--png out yaw pitch roll]` evidence mode |
| `tests/acz-vr/xr_stereo_output_offline_tests.cpp --cockpit`, `xr_composite_warp_harness.cpp case_cockpit_model` | consumer and GPU tests |
| `tools/live/install-assets.ps1` | copies `D:\acz-extract\stage\*.zvrs` and `models\*.zvrm` into `%LOCALAPPDATA%\acz-vr\{stage,models}` (magic + SHA-256; game data never in git) |
| `tools/live/vr-test-start.ps1 -CockpitModel '<id>[;opt:val]'` | writes the key (and warns when the file is missing / without `-DepthFix`) |

## 3. Config: one new key (justification)
`xr_cockpit_model=<id>[;opt:val...]`, id = the `.zvrm` file stem (`[A-Za-z0-9_]{1,24}`, e.g. `j35jk_vrcockpit`), value < 100 bytes, `off`/`0`/`false`
= disabled, absent = OFF. ONE key, no separate bool (a non-empty value enables it). Not folded into `xr_terrain_stage`: the terrain parser refuses unknown
options (a cockpit option would disable the terrain), the two must fail independently (a missing model must not cost the terrain and vice versa), the
cockpit also works over the game's own world image, and the WORLD tracks edit the terrain option parser in parallel. Budget: the launcher's full set with
terrain + depth fix + png once + this key is 30 of 32 keys and ~835 of 2048 bytes [V, computed from the script's key list; the launcher now also checks
the 2048-byte limit].

| option | default | meaning |
|---|---|---|
| `upm:<u/m>` | 5.0 | head translation (metres) -> model units (model-decode.md: Draken 5.00 u/m) |
| `gain:`, `amb:` | 1.0, 0.6 | colour gain (display domain) / ambient share of `amb + (1-amb)*max(0, n.L)` (COCKPIT-3: fitted to the game window, section 10; COCKPIT-2 used 1.15 in the raw domain) |
| `dom:raw` | `display` | COCKPIT-3: `display` shades in the game's display domain and applies the sRGB EOTF once; `raw` = the COCKPIT-2 path (A/B) |
| `light:fixed` | `stage` | COCKPIT-3: the stage sun + light colour in the cockpit frame (needs the terrain with a v3 env section); `fixed` = kLight, white |
| `floor:0` | 1 | COCKPIT-3: the synthetic floor closure (section 12) off |
| `cut:` | 0.5 | alpha-test threshold (cockpit atlas texels) |
| `near:` | 0.05 | near plane in model units (1 cm) |
| `ext:0` | 1 | hide every exterior (airframe) part |
| `glass:0` | 1 | hide glass parts |
| `hudglass:1` | 0 | put the world-quad HUD at the model's HUD-glass distance (section 6) |
| `replay:1` | 0 | A/B: keep the replayed cockpit UNDER the model (registration check) |
| `hide:i,j` / `show:i` | - | part indices (as printed by `--load` / `model_baker.py info`) |
| `eye:x,y,z` | the file's pvb eye | override the cockpit eye point |
| `cscale:<0.8..1.5>` | 1.0 | COCKPIT-SCALE: uniform cockpit scale about the pvb eye (section 15); 1.0 = unscaled, bit-identical; out of range/NaN refused. Recommended in the headset: 1.15 (D-362) |
| `clip:0` | 1 | COCKPIT-CLIP (section 16): fit the procedural interior into the airframe + clip the cockpit to the type's clip volume; `clip:0` = off (the pre-D-362 mesh) |
| `clipbox:<ahead>,<behind>,<below>,<above>,<half>` | the type's table row | COCKPIT-CLIP: explicit clip volume in metres from the pvb eye (each 0.05..5); per model, refused with `auto` |
| `seatback:<0..0.8>` | 0.30 | SEAT-CLEARANCE (cockpit-fidelity.md section 5): model metres from the eye to the procedural headrest front, unscaled model (physical = x cscale); the seat only moves back; 0 = the COCKPIT-4 placement (0.21 m), bit-identical |

## 4. Pose composition (the cockpit is rigid in the aircraft frame; no game look/camera enters)
Per eye, with exactly the pose and FOV the projection layer submits for that eye (`composite_views`), the seat anchor `p0` (the producer's
seated-recentering neutral, `neutral_`) and its heading `yaw0`:
```
rel        = Ry(-yaw0) (eye_pos_LOCAL - p0)                  (metres, cockpit axes: +X right, +Y up, -Z nose)
eye_model  = pvb_eye + upm * rel                              (pvb_eye = .pvb bytes 40..52; j35j (0, 3.575, -36.3))
R_model    = Ry(-yaw0) * R_head                               (full 6DoF orientation)
clip       = P(tan l, r, u, d; reversed-Z infinite, near) * V(R_model, eye_model) * (p, 1)
```
`P` rows: `x = (2 x_v + (r+l) z_v)/(r-l)`, `y = (2 y_v + (u+d) z_v)/(u-d)`, `z = near`, `w = -z_v` (depth = near/w, GREATER, cleared to 0). The lean
(`gpu_copy_step5_neck_lever_m`) and the look-drive look are NOT applied to the cockpit (it is fixed to the seat). [V] tests: pvb eye at the seat origin,
FOV edges map to +-1 for an asymmetric FOV, 10 cm head offset = 0.5 u, seat yaw 90 deg maps LOCAL -X to cockpit forward, orthonormal basis under
yaw/pitch/roll; the consumer test checks every job's eye = `pvb + upm * world_eye[e].origin` (the SAME anchor-frame origin the world composite uses).

## 5. Rendering
* Vertex stream: one 32-byte corner per triangle corner (non-indexed; `pos | uv | nrm i16x3 | mat u16 | rgba`), opaque triangles first, then the blend
  parts sorted back-to-front by part centroid from the pvb eye (static order; head motion of a few cm cannot reorder parts meaningfully [I]). Read in the VS
  as a raw buffer with `base_vertex + SV_VertexID` (no input layout, no index buffer). j35jk_vrcockpit: 53 drawn parts, 5,625 triangles, 540 kB [V].
* Pixel (COCKPIT-3): `rgb = srgb_to_linear(saturate(tex * vcol * (amb + (1-amb) max(0, n.L)) * gain * tint)) * exposure`, the consumer tonemap, output
  premultiplied, blended ONE/INV_SRC_ALPHA into the sRGB RTV (linear blend, one sRGB encode) - the game's DISPLAY domain like the terrain/props. COCKPIT-2
  wrote the display value as linear (the replayed cockpit's convention, which is itself 4.5x too bright in the dark tones: section 10); `dom:raw` keeps it. Opaque draw: alpha test `tex.a < cut`
  on cockpit-atlas parts, alpha 1 (the exterior skin's alpha is a reflection mask and is ignored). Blend draw: alpha = `tex.a * vcol.a`, no depth write.
  Light (COCKPIT-3, section 13): the stage sun in the cockpit frame of the frame's game camera (moves with the attitude) and the stage light colour; fixed
  direction (0.35, 0.8, 0.45), white, without a v3 stage or with `light:fixed`. Root constants 32 DWORDs (tint added).
* Textures: RGBA8 UNORM, 1 mip, bilinear WRAP (the 512x512 exterior skin may alias at distance [I]). Max 4 per model (vrcockpit uses 2).
* Replay replacement: `mode_bits` bit 17 makes `ps_main` keep only replay pixels with `0 < depth < 2^-10` (the replayed world OBJECT band of
  `xr_world_objects_depth_fix`); everything else of the replayed eye image gets coverage 0 (no depth plane: all of it). The bit is only set when the backend
  actually drew the model in that recording, so a GPU failure shows the replay exactly as with the key off [V WARP].
* The `xr_png_once` final image includes the model [V WARP: identical to the submitted eye 0]; debug views 1-4 skip the model.

## 6. HUD relation (docs/review/pov-hud-lean-analysis.md)
The game's HUD stays a separate quad layer composited over the projection. In world-quad mode (`hud_follow 0`, no lean) its pose is
`p0 + Ry(yaw0) * (...)`: it is already fixed in the cockpit frame, at the same anchor as the model's pvb eye, so the HUD and the model cockpit move together
with no extra code [V by construction: same `neutral_`/`neutral_yaw_`]. The model's HUD glass is located from the mesh (drawn, non-exterior glass part
nearest the forward axis, 1..40 u ahead): **j35j: part 4 (the reflector-sight glass), centre (0.055, 3.500, -38.272), 1.974 u = 0.39 m ahead, 12.6 / 12.8
deg left/right, 8.5 / 9.3 deg up/down** [V `--load`; identification as the gunsight glass I, from position/size]. Log: `xr-stereo;event=cockpit-hud-glass`.
Placement choice: a reflector sight is collimated, so symbology at a far distance (same angular size) has no neck parallax against the world, while a quad
at 0.4-0.5 m reproduces the "HUD moves when I look around" parallax of test 14 [I from the analysis]. Default therefore leaves the configured HUD distance;
`hudglass:1` puts the HUD quad exactly at the glass distance (event `cockpit-hud-anchor`) for an A/B. Not done: clipping the HUD to the glass outline.

## 7. Evidence (what is verified) and what is not
[V] loader: round trip byte-identical with the serializer, 39 refusal cases plus a truncation sweep (every 97th byte; magic/version/header/size/stride/flags/counts,
SHA-256, and content errors behind a resealed hash: index ranges, vertex part ids, NaN/huge positions, texture size/format/overlap, part parents, names),
missing file, on-disk corruption; the real bakes load (j35jk/m21bk/f16ck `vrcockpit`, `j35jk_cockpit`; j35jk sha256 89d0c9fc...7774, 90 parts, 5,929 tris).
CPU reference on the synthetic model: red LEFT / green RIGHT / blue UP (no mirror), back face culled, near quad over far quad independent of draw order,
alpha-test hole, glass translucent, exterior tail visible only looking back and only with `ext:1`, roll/pitch/yaw move the landmarks the right way, per-eye
disparity 4.0 px vs 3.7 px expected, lean parallax near > far, deterministic. **WARP**: the real `vs_model`/`ps_model` + the real backend vs the CPU
reference, both eyes, forward / 6DoF (yaw -10, pitch 6, roll 25, translated) / look back / `replay:1`: max error 0.006, at most 1 edge pixel of 57,600 > 0.04;
object band kept and cockpit band dropped; disparity of the 8-u quad 5.7 px (5.4 expected); png capture = eye 0; deterministic; key off and a refused mesh are
byte-identical to the no-cockpit job; re-upload after a refused generation. CPU preview of the real Draken from the pvb eye (`--load ... --png`, not
committed: game-derived) shows the canopy bow, posts, reflector glass and panel as in model-decode.md; looking left shows the canopy frame and wing;
looking back the spine and fin with an empty (dark) seat area. Release ctest 299/299.
[I] not verified: anything in the game or headset; registration of the model with the replayed cockpit / game camera at neutral head (the preview matched
the game image within a few %, model-decode.md); the head-translation scale (5 u/m) in the headset; the colour/brightness match; the HUD-glass identity;
GPU cost on the real GPU (5,625 triangles x 2 eyes, expected negligible); behaviour with `gpu_copy_step5_hmd_fov` off (the model is only visible inside
the 63x38 lens rectangle then).

## 8. Risks / known limits
1. Replayed objects without `xr_world_objects_depth_fix` (`-DepthFix`): object depth ~1/w, so objects nearer than ~1,000 units (~200 m) fall in the
   cockpit band and are dropped with the replayed cockpit (missiles/aircraft very close). With the fix all objects are in the band.
2. The terrain eye uses its own head-translation scale (WORLD-1 default: the producer's units per metre) while the cockpit uses 5 u/m: harmless for km-distant
   terrain, but near replayed objects (rendered by the producer at its scale) get a different translation parallax than the cockpit [I].
3. No animation: control surfaces/canopy/airbrakes at rest, gear not drawn (part 1 not in the bake), no stick/throttle, gauges are texture-only, no damage
   state. The seat area is empty (the game has no seat model); looking straight down shows the fuselage interior.
4. The game's cockpit lights/HUD glass effects (0x2000 overlay) are approximated by a straight-alpha tint; the exterior canopy glass uses the skin alpha as
   glass alpha (could look too opaque/milky) - `glass:0` or `hide:` for A/B.
5. Aircraft type is not detected: the id must match the mission's aircraft (mission 02 = Draken `j35jk_vrcockpit`); a wrong id shows the wrong cockpit.
6. The model draw follows the eye composite; in debug views it is skipped by design.

## 9. Live recipe (null driver first; a driver runs it)
COCKPIT-3: rerun `install-assets.ps1` first (the rebaked RGBA-CLUT `j35jk_vrcockpit.zvrm`, sha256 713f52e2...; an old bake still loads and is fixed at load,
logged `clut=legacy-bgra-bake`); `-CockpitModel` now defaults `xr_hud_scale` to 1. Expect `event=cockpit-model;...;dom=display(srgb-eotf-once);
light=stage-sun(else-fixed);floor=closed`, `summary-cockpit;...;stage-light-frames=` rising, no `event=cockpit-hud-registration` (else the HUD scale is not 1).
1. `pwsh -NoProfile -File tools/live/install-assets.ps1` (copies the baked `j35jk_vrcockpit.zvrm` etc. into `%LOCALAPPDATA%\acz-vr\models`).
2. `pwsh -NoProfile -File tools/live/vr-test-start.ps1 -DllDir <D:\b-c2 dll set> -CockpitModel j35jk_vrcockpit -TerrainRender m02a -DepthFix -PngOnce`
   (A/B registration: `-CockpitModel 'j35jk_vrcockpit;replay:1'`; HUD at the glass: `'j35jk_vrcockpit;hudglass:1'`).
3. Mission 02, cockpit view. Log (`xr-stereo`): `event=cockpit-model-loaded` (sha256 89d0c9fc...), `event=cockpit-hud-glass`, `event=cockpit-model;...`,
   `summary-cockpit` with `model-frames` rising and `no-view=0`, `backend-failed=0`; no `cockpit-model-disabled`.
4. PNG (`logs\png`, read with the 180-degree rotation, D-298): the model cockpit fills the forward view like the replayed one (with `replay:1` both should
   overlap at the neutral head; a constant offset = eye point/FOV issue, a scale difference = FOV, a growing offset with head motion = `upm:`).
5. Headset: look around/back/down (airframe visible, canopy frame continuous), lean (the cockpit parallax should feel 1:1 at `upm:5`), check the HUD
   with and without `hudglass:1`.

## 10. COCKPIT-3 colour: what was wrong and the measured match (step62 images; tools `research/world-render-data/cockpit3_fit_colour.py`)
Reference images: the game window `step62-integrated-run1/shots/acz-20261027-004711-116-T6.75.png` (client area = crop x 10, y 31, 2400x1350, the game's
63.2x38.2 deg lens) and our eye dump `xr-f403-final-upright-2400.png` at the same moment; `step56-terrain-run4/xr-f417-final-upright.png` = the game's own
cockpit REPLAYED into the XR eye.
1. **The replayed cockpit (and the COCKPIT-2 model) are sRGB-encoded twice.** Game window sight housing (16,16,15), centre console (22,23,22), canopy bow
   (26,33,24), strut (9,11,11); the step56 replay at the same features (75,78,80), (77,81,81), (80,92,74), (51,55,56) = exactly `srgb_encode(game/255)`
   (sRGB EOTF of the replay values: (18,19,20), (19,21,21), (20,27,17), (8,10,10)) [V by these 7 landmarks]. The producer's FP16 replay holds the game's
   display values, the RTV is sRGB, so dark tones come out ~4.5x too bright. The terrain/props (WORLD-1/3) already shade in the display domain and convert
   once; the model now does the same (`dom:display`). The replayed world image (`world_image`, non-terrain mode) keeps the old convention (not this track).
2. **The GIM CLUT is RGBA, not BGRA.** COCKPIT-1 read the appended GIM palette as BGRA (`model_baker.gim_image`, `acm_lib.decode_gim`). Evidence: the sign of
   R-B flips between our model and the game's replay on every landmark (gunsight housing ours R-B +7 / replay -7, bow -5 / +15, panel -4 / +8); the m02a
   tree GIMs read RGBA are olive (33,36,29) vs teal (29,36,33) as BGRA; WORLD-2's `env_gim.py` and the pmtex prop CLUT read RGBA; PS2 CT32 CLUT entries are
   R,G,B,A [V]. This made the pink-brown gunsight and the cyan-green panel. Fixed in the baker (header flag 4 `kZvrmRgbaClut`); the loader swaps R/B of
   unflagged mpl/acm bakes (kind 1/3) and logs it.
3. **Result** (CPU reference = the shader, game lens, stage light at the capture attitude, `--light 0.640 0.766 -0.056 --tint 1.016 1.074 0.911`), over
   1,070,456 interior cockpit pixels (HUD symbology and glass excluded): mean game (22.0, 25.6, 21.0) vs model (21.9, 25.5, 20.2), mean |diff| 6.4 levels
   [V]. Regions (game / model): sight face (18,20,21)/(15,17,18), alpha-gauge bezel (20,21,19)/(17,17,16), canopy bow (27,35,25)/(32,41,26), left strut
   (9,11,11)/(12,13,13), right console (10,11,11)/(14,15,16). A free least-squares fit gives gain 1.19 / ambient 0.47 (residual 6.15 vs 6.43): the defaults
   (1.0 / 0.6 = the m02a ambient share 0.59) are kept. Before (step62 eye): sight housing displayed (86,80,79), panel (94,105,98) - 4-5x too bright, R/B swapped.
   Side by side: `D:\acz-extract\out\cockpit3\game_vs_model_colour.png` (game-derived, not committed).

## 11. COCKPIT-3 position/scale: angular registration (tools `research/world-render-data/cockpit3_fit_camera.py`)
Chamfer fit of the model's silhouette edges (CPU reference) against the edge map of each image [V]:
* **Game window**, game lens tan (0.6152, 0.6152, 0.3460, 0.3460) from the pvb eye (0, 3.575, -36.3), no offset, no pitch: mean edge distance 0.37 px at 1/3
  scale (~1 px of 2400, ~0.03 deg); dz +-0.1 u or dy +-0.1 u (2 cm) already scores >= 2.4 px. The model, its scale (1 u = 1 world unit), the pvb eye and the
  lens are exactly the game's cockpit camera. `fit_game_lens_chamfer.png`.
* **Our step62 eye 0**: null-driver FOV +-45 deg on BOTH axes (`eye0-fov=-45.00,45.00,45.00,-45.00`) on a 2400x1350 image: best at eye = pvb + (+0.16 u = 3.2 cm,
  half the null IPD), pitch 0-0.5 deg, 1.07 px; the eye point and orientation the consumer built are the designed ones. `fit_xr_eye0_chamfer.png`.
* So "lower and larger" is the projection, not the model: the null eye maps tan 1 to 1200 px horizontally but 675 px vertically (anisotropic pixels, 1.78:1)
  vs the game lens' 1958 px per tan on both axes. Same angles in both: e.g. the alpha gauge centre at tan (-0.288, -0.12) is pixel (636, 948) in the game
  lens and (854, 756) in the dump. Near plane 0.05 u (1 cm) clips nothing (the nearest drawn geometry is the glass at 1.97 u). No change needed [V].

## 12. COCKPIT-3 the white "nose tip" and the floor closure
The white shape at the bottom centre was NOT the nose cone: ray picks from the pvb eye below the panel (tan y < -0.75, |tan x| < 0.45) hit up-facing
triangles of the exterior fuselage `p0/n0` and `p0/n55` at y -2.1..-2.9, z -40..-43 (the nose-gear-well roof, inside the airframe) seen through the open
bottom of the game's cockpit part, which has no floor (the game's 19 deg-down lens never looks there); the terrain also showed through the same opening.
Fix (`floor:1`, default): an opaque, untextured, up-facing quad at the lowest point of the drawn cockpit geometry spanning its footprint (j35j: y 1.460,
x -1.983..1.983, z -41.775..-32.131: 0.42 m below the eye), colour (40,42,40) under the cockpit light. Everything below it - gear well, fuselage interior,
terrain - is depth-occluded; the airframe stays visible looking out sideways/back (rays that leave the cockpit above the floor or outside its footprint).
Synthetic geometry [I: a real floor/seat/stick would need models the game does not have].

## 13. COCKPIT-3 cockpit lighting
With the terrain running and a v3 stage env section, every frame lights the model with the stage sun (env P[1], stage space) expressed in the cockpit frame
of the frame's game camera: `L_model = (L.right, L.up, -L.fwd)` (the same camera axes the terrain rays use, so cockpit and terrain shading agree and the sun
moves across the cockpit when the aircraft banks/turns). Light colour: `tint = (ambient + sun colour) / mean` (m02a (1.016, 1.074, 0.911)), clamped
[0.5, 1.5]. Ambient share stays `amb:` (0.6 ~ m02a's 0.59). Counters: `summary-cockpit;stage-light-frames`. At the step62 capture (heading -45, level) the
m02a sun (elev 50, heading -130) is (0.640, 0.766, -0.056) in the cockpit: up and to the right [V by the formula; the game's own cockpit lighting model
is unknown, I].

## 14. COCKPIT-3 HUD
The world-quad HUD and the model share the seat anchor and heading (section 6). The step62 run had `xr_hud_scale=0.5` (the launcher default from test 5
with the replayed cockpit): the HUD was HALF the game's angular size and could not sit on the gunsight glass. In the game window the HUD symbology frames the
reflector glass, and our model glass registers with the game image (section 11) - so the HUD registers with the model at scale 1 at any distance.
`vr-test-start -CockpitModel` now defaults `-HudScale` to 1.0 (an explicit `-HudScale` wins) and the consumer logs `event=cockpit-hud-registration` when
the scale is not 1. Harness (`xr_stereo_output_offline_tests --cockpit`, `cockpit_hud_registration`) [V]: the `hud_world_quad` corners lie on the game-lens
rays from the pvb eye in the model frame (1e-4); for head rotations up to 25 deg yaw/10 pitch/20 roll the HUD centre stays on the model glass point (0
NDC); with `hudglass:1` (HUD at the glass distance 0.395 m; the HUD distance clamp minimum is now 0.3 m, was 0.5 m and silently moved it to 0.5 m) also for
leans up to 6 cm (0 NDC); at 0.5 m a 5 cm lean shows 0.026 NDC of HUD-vs-glass parallax. Not done: clipping the HUD to the glass outline.
HUD-MENU (D-362, `docs/world-render/hud-menu.md`): `-CockpitModel` now defaults `xr_hud_scale` to 0.8, pivoted about the glass's angular centre
(only the pivot stays registered; 2 deg error at 10 deg off it), and a frozen game camera (pause menu) switches the HUD layer to menu mode.

### COCKPIT-3 tests
`cockpit_model_offline` (387 checks: legacy CLUT swap per source kind, flag refusal, options dom/light/floor, display-domain shading incl. exposure order,
tint, stage light frames, floor geometry/winding/placement), `xr_composite_warp` case 11 (`c_model_cockpit3`, `_down`: the real shader with display
domain + stage light + tint + floor vs the CPU reference, max error 0.006, 0 bad pixels), `xr_stereo_cockpit_model_offline` (HUD registration),
`test_model_baker.py` (RGBA CLUT, flag 7). Release ctest 299/299 after merging master with STAGES (D:\b-c3; before that 298/298, gpu_copy_step1_step5-threads_offline once hit its known parallel-run 0xc0000409 and passed alone, as in D-307). Not verified: anything live (game, null driver, headset).

## 15. COCKPIT-SCALE `cscale:` (D-355 headset feedback: the cockpit feels "a little small")
**Semantics.** `cscale:s` scales the WHOLE model cockpit (game parts, canopy, floor closure, procedural interior, animated surfaces/stick/throttle/pedals,
HUD glass) uniformly about the pvb EYE point: `p' = pvb + s (p - pvb)`. From the centred eye every ray keeps its direction, so everything section 11/14
registers ANGULARLY stays registered at any `s` (the game lens / gunsight glass, the world-quad HUD on the glass, the world through the canopy; the
default world-quad HUD keeps its lens-derived angular size). What changes is only distance, which the eye reads from stereo disparity (IPD) and head-motion
parallax: `s > 1` = a physically bigger cockpit around the same eye. A scale about any other point (the model origin, the cockpit centre) would move the
glass and the HUD against each other and the world, so it is not offered.

**Where head motion and IPD enter (section 4).** Both enter only through `eye_model = pvb + upm * rel` (each eye's submitted LOCAL position, the IPD is
the difference of the two). A model scaled by `s` about pvb seen from `pvb + upm * rel` is exactly the unscaled model seen from `pvb + (upm/s) * rel`
with near `near/s`: `p' - E = s (p - E_s)`, so clip x, y, w scale by `s` and `z = near` matches `s * near/s` -> identical NDC and reversed-Z depth
(`near/w`). The implementation therefore leaves the mesh untouched (floor/interior/anim/zpre/glass are consistent automatically: they are part of the
same rigid model) and the consumer calls `eye_camera(..., view_upm(o) = upm/cscale, view_near(o) = near/cscale)`. The canopy Fresnel view vector
uses that camera's model-space eye (same direction as the scaled case). `hudglass:1` converts the glass distance with `view_upm`
(`distance_m = cscale * distance_u / upm`: the scaled glass is that much farther, same angular size). The world terrain's own head scale is untouched.
At `cscale:1` `view_upm == upm` and `view_near == near` exactly (division by 1.0f), so the camera and mesh are bit-identical to before [V].

**Log.** `event=cockpit-model;...;upm=5.00;cscale=1.080(about-pvb-eye);...`; the `cockpit-hud-glass` event's `distance-m` is the scaled one.

**Tests** (`cockpit_model_offline` `test_cscale`, `aircraft_auto_offline`) [V]: default 1.0, accepted 0.8..1.5, refused 0.79/1.51/nan/inf/-1/0/empty/text
(also through `auto`); cscale 1 -> `memcmp`-identical `EyeCam` and mesh; for s in {0.8, 1.08, 1.25, 1.5} at a centred eye, a half-IPD eye and a
leaned/rotated head in a yawed seat, every vertex of the COCKPIT-4 synthetic model gives `clip_impl(p) * s == clip_ref(pvb + s (p - pvb))` (NDC and depth
equal) against the physically scaled reference; from the centred eye every vertex ray keeps its direction (1e-5) while its length scales by `s`, the
image NDC of the model is unchanged by cscale and its reversed-Z depth is divided by `s`; the stereo eye separation in model units is `0.064 * upm / s`;
the hudglass distance in metres is `s` times the unscaled one. Not verified: anything live (headset impression).

**Use.** "A little bigger": start with `cscale:1.08` (8 %), e.g. `-CockpitModel 'auto;cscale:1.08'`. In the headset: the cockpit should look ~8 %
farther/larger WITHOUT anything shifting in the view at the neutral head (HUD still on the gunsight glass, canopy frame and horizon where they were); lean
and it should parallax like a slightly bigger cockpit (panel slides a little less per cm of lean). If the HUD or panel appears to MOVE relative to the
glass/world when only cscale changed, that is a bug (registration must be scale-invariant). Too big/far: step back to 1.04; still small: 1.12-1.15.
**Recommendation since headset test 19 (D-362: 1.08 "could be a little larger still"): `cscale:1.15`** (`auto;anim:1;default:j35jk_vrcockpit;zpre:1;cscale:1.15`).
The clip of section 16 is applied to the mesh in model space and cscale only to the camera, so cscale 1.15 with clipping keeps the angular registration
(`test_clip`: the clipped/fitted mesh is byte-identical at cscale 1.15 and every vertex keeps its centred-eye NDC, depth / 1.15) [V offline].

## 16. COCKPIT-CLIP `clip:` (D-362 headset test 19: "the cockpit model protrudes out of the nose and behind the cockpit")
**What protrudes [V offline, `research/world-render-data/cockpit_clip_survey.py`].** The survey renders the drawn opaque mesh (back faces culled, glass
excluded) from the pvb eye and two 15 cm side leans, 12 yaws x 3 pitches (108 views, 240x180, 100 deg), takes every visible pixel whose surface is NOT the
exterior airframe and tests its 3D point against the airframe envelope (the drawn exterior skin incl. the canopy glass, sampled on 0.1 u grids: half-width
per side from the centre plane, top and bottom per column, slack 0.15 u). On the Draken (the test-19 aircraft, `cockpit-auto;type=j35j`) the outside
surfaces are (1) **our procedural interior** (COCKPIT-4, made up): the tub is a box over the game cockpit part's footprint (x +-1.98 u, z -41.78..-32.13),
but the nose narrows to a half-width of 1.1 u at the panel front (z -41.8, y 1.6) and the canopy/spine to 1.1-1.4 u at eye height behind the seat, so
the **front-wall corners stick out of both nose sides**, the **rear bulkhead (up to the spine antenna, y 4.85) sticks out above and beside the spine**,
the seat rails (eye + 0.24 m) poke through the canopy top, the side walls leave the nose ahead of the sill; (2) the game's own cockpit part is wider than
the exterior canopy (side rails n12/n19 to x 1.84 u, rear side pieces n22/n25 to 1.98 u vs 1.1-1.4 u), and the glareshield/panel top lies above the
exterior nose skin: that is the game's cockpit (the panels, HUD glass and canopy frame must stay), so it is NOT clipped sideways/upwards. Across the 36
bakes the opaque game cockpit triangles lie inside the airframe except stray far parts behind the seat (f16x n49/n70 2.5-2.7 m aft, zoef, f15c/e/s rear
pieces).

**Fix (default ON, `clip:0` = the previous mesh).** In `build_mesh`, model space, before the camera:
1. **Fit** (made-up geometry only): the procedural tub's five large faces (floor, side walls, front wall, rear bulkhead; or the COCKPIT-3 floor quad with
   `interior:0`) are tessellated 8x8, then every static interior vertex (tub, seat, consoles, rails, quadrant; distinct positions once) is pulled inside the
   exterior skin: sideways to the outermost skin crossing on its side of the centre plane at its height, then vertically between the lowest and highest skin
   crossing of its column (only for a closed column: a single sheet such as an elevator plate moves nothing), 3 iterations, margin 2 cm
   (`kClipMarginM`); flat normals are re-derived and collapsed cells dropped. The skin = the drawn exterior parts incl. the canopy glass in the cockpit's z
   slice. Animated interior pieces (stick, throttle lever, pedals) are not moved (they sit inside the tub). No airframe around the cockpit (the synthetic test
   models) = nothing tessellated or moved: byte-identical to `clip:0`.
2. **Clip volume** (data: the aircraft table): every NON-exterior, non-glass static triangle (the game's cockpit part and the interior) is clipped exactly
   (Sutherland-Hodgman per triangle, attributes interpolated, winding kept) against a box around the pvb eye `{ahead, behind, below, above, half}` metres =
   `kClipVolumes` in `cockpit_model.cpp`, generated by `cockpit_clip_survey.py table` into `research/world-render-data/cockpit_clip_volumes.json` (rule:
   ahead/below/above/half = the extent of the opaque game cockpit triangles, behind = the farthest of those whose centroid lies INSIDE the airframe, each
   +0.02 m; below >= 1.12 m for the tub floor). The interior is built inside the box (footprint and floor clamped to it). The exterior airframe, every glass
   part (HUD glass, gauge glasses, canopy) and the animated pieces are never clipped; the HUD-glass locator still reads the unclipped model. `apply_clip_table`
   (proxy for a fixed id, `load_auto_model` for `auto`) sets the box by the model id unless `clipbox:` was given; `clipbox:` is per model (refused with
   `auto`); an unknown id gets the fit only.
3. **Log**: `xr-stereo;event=cockpit-model-clip;clip=on(fit+box);box-m=...;game-removed=;game-cut=;interior-removed=;interior-cut=;fit-moved=;fit-skin=`.

**cscale.** The clip/fit changes only the mesh, in model space; cscale changes only the camera (section 15), so the clipped mesh is byte-identical at
any cscale and the angular registration (HUD glass, panel, horizon) is unchanged [V `test_clip`].

**Live check list (headset; nothing here is verified live).**
1. Build + install the DLLs carrying this change; day config cockpit value `auto;anim:1;default:j35jk_vrcockpit;zpre:1;cscale:1.15` (clip is default ON; no new
   key, no key count change).
2. Log (`xr-stereo.log`): `event=cockpit-model-clip;clip=on(fit+box);box-m=1.12,0.84,1.12,0.18,0.42(...)` for j35j with `fit-moved` > 0 and `fit-skin` > 0
   (offline j35j: fit-moved=495, fit-skin=713, interior-cut=3, game-removed=0, game-cut=0); `event=cockpit-model;...;cscale=1.150(about-pvb-eye)`; no
   `cockpit-model-disabled`.
3. Headset, Draken m02a: look down-left/down-right past the panel and over the sills toward the nose: no dark slab sticking out of the nose sides; look back
   over both shoulders (lean up/sideways 10-15 cm): no dark wall above or beside the spine behind the canopy, seat rails end below the canopy glass.
4. Must stay: panel, gunsight/HUD glass with the HUD on it, canopy bow/frame, side rails, the seat/headrest, the stick/throttle; the HUD and panel must NOT
   shift when toggling cscale 1.08 -> 1.15 (only bigger/farther) or clip on/off.
5. A/B: `...;clip:0` restores the pre-D-362 mesh (the protrusions should come back). If something the user wants is missing, a per-model `clipbox:` (fixed
   id only) or a new table row (`cockpit_clip_survey.py table`) adjusts it.
6. Not covered: the game cockpit part is wider than the exterior canopy on the Draken (rails to 0.37 m vs canopy 0.22-0.28 m) and its glareshield top sits
   above the exterior nose skin; these are the game's own cockpit and are kept. If the user still reports "behind the cockpit", look at the rear side pieces
   n22/n25 first (a `clipbox:` behind of ~0.72 m trims their last 10 cm).

**Evidence [V offline, 2026-10-27; meshes dumped by `cockpit_model_offline_tests --load <zvrm> --opts 'anim:1[;clip:0]' --dump`, scored by
`cockpit_clip_survey.py survey --mesh-dir`; 108 head poses per type; images not committed (game-derived)].** Visible synthetic-interior pixels outside the
airframe, before (clip:0) -> after (default): **j35j 31,373 -> 0** (4 -> 0 outside triangles; the front-wall slabs out of the nose, the rear bulkhead above
the spine and the seat rails through the canopy are gone; forward pilot view unchanged), f16c 24,359 -> 0, f16x 24,054 -> 370, zoef 470,084 -> 4,708,
f17a 42,089 -> 16,367, e18g 7,454 -> 445, su27 8,761 -> 447, su37 8,841 -> 355, m31b 3,663 -> 46, a10a 3,775 -> 3, typn 5,067 -> 0, x02a 3,169 -> 0,
mr2d 2,879 -> 252, m21b 2,380 -> 16, j39c/m29a/rm01/yf23/f35c/f05e/su47 -> 0..9; f15c/e/s 1.5-1.8k -> 2.2-2.9k (small, tessellated walls follow the skin
edge). **Not fixed: e06b (569k -> 561k), su32 (352k -> 360k), tnd4 (50.9k -> 50.6k), f14d (12.2k -> 12.0k)**: side-by-side / two-seat cockpits whose
procedural tub does not fit the airframe (the fit needs a closed skin column around each vertex); use `interior:0` there until a per-type interior exists.
Clip-table cuts of the game's own cockpit (`--load` counts, triangles removed/cut): f16x 65/20, zoef 58/38, f15c 16/20, f15e/s 0/18, f14d 0/15,
f05e 6/18, f20a 4/20 (the last two: blended decal parts behind the seat), all other types 0/0; glass (HUD glass) is never cut. Fit statistics per type
(`fit-moved`, `fit-skin`): j35j 495/713, a10a 345/272, f16c 459/517.

## SKIN-REFL (option srefl:1 / sreflk:<0..4>, default off)
The engine's cb10-bit-0 reflection pass on the exterior skin (stage `<stage>_ref` hemisphere map, world-normal uv, up-facing mask, x skin alpha), constants 56
DWORDs, t5 map, sidecar `<stage>_ref.zvrt`: design, evidence, tests and live checks in docs/world-render/aircraft-shading-reflection.md sections 7-9.
