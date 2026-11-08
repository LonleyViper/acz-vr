# AIRCRAFT-SHADING: why our own airframe/cockpit model has no shadows/reflections (headset report items 15/16)

Offline investigation only (no game, no build, no job board). Tags: [E] evidence, [I] inference, [U] unknown.

## 1. What our model renderer does [E, code]
`cockpit_model.hlsl` `ps_model`: `rgb = tex * vertex_colour * (ambient + (1-ambient) * max(n.L,0)) * gain * tint`, one fixed/stage sun, alpha 1 for opaque
parts, `tex.a` used only for alpha test on cockpit-atlas parts (`MF_ALPHA_TEST`) and for blend parts. No specular, no environment/sphere-map term, no shadow
term. The only reflective term is the canopy-glass Fresnel sky pass (`ps_glass`, cockpit4-impl.md). The exterior skin alpha is dropped on purpose
(`MF_TEX_ALPHA_NOT_COVERAGE`, model_baker.py "alpha = NOT transparency: reflection mask, ignored").

## 2. What the engine does that we drop
1. **Reflection (sphere-map) VS path [E DXIL].** `tools/evidence/e11-drawcpt-dxil/drawcpt-vs.dxil.txt` (DrawCPT_v, label 143-171) and the DrawACM_v dataflow
   (acm-shader-dataflow.md s1: cb10 bit 0 = the +352.w/+364-gated reflection-vector path, +320 reference point): when cb10 bit0 is set the VS replaces the
   mesh UV by a view-space-normal sphere-map UV, `uv = (n'.x*0.5+0.5, n'.z*0.5+0.5)` (n' = normal through a record matrix /16), and replaces the vertex colour
   by a facing mask `saturate(10 * n'.y)` on rgb. So the engine draws a SECOND pass of the same mesh with an environment texture (matcap) gated by facing.
   The captured aircraft draws include flags 7 (bits 0,1,2 = reflect, int normals, no fog): 4 of the 17 pass-6 ACM draws (acm-shader-dataflow.md s5); the
   other 13 are flags 258 (plain lit + fog). [E that the path exists and aircraft use it; U which texture, blend mode, and which parts.]
2. **Skin alpha as a mask [E data, I role].** j35jk_vrcockpit skin (GIM part 8, 512x512): alpha 11..191 (/128), median 61 (0.48), p5 13, p95 109: a real
   spatially varying mask (panel lines, grime), not noise. Our opaque pass discards it. [I] it is the per-texel strength of the reflection/spec pass.
3. **Shadows [I].** The PS2 title has no shadow maps. On the airframe "shadows" are (a) the baked vertex colours (we use them) and (b) the lit-branch light
   matrix (+352.x=1, three directional terms + ambient, squared remap when +356==0, clamp 1.99): we use ONE Lambert sun, so wing-on-fuselage and
   under-surface darkening differ. Offscreen passes 1-3 (128x-128 targets, 11 draws, flags 2) are probably projected shadow/blob passes [U].
4. **Node flag 0x2000 overlays [E data].** The baker maps them to `MF_GLASS|MF_BLEND` plain alpha. The Draken exterior has NONE (j35jk exterior parts are
   g0000/g0008/g000a/g000b; 0 triangles with 0x2000); only cockpit-tub gauge glass (169 tris) has it. So the Draken's reflection cannot come from node flags:
   it is the per-draw cb10 bit0 pass (a runtime flag, not stored in the .mpl). [E counts]

## 3. Root cause
Our model path is a single lit pass; the engine's reflection is a separate draw (cb10 bit0, matcap UV + facing-mask colour, modulated by skin alpha [I]) and
its lighting is multi-term. Both are absent. Not a filter bug: no flagged exterior part is dropped (hidden = pilot/tub duplicates only).

## 4. Why no fix was committed at first (superseded for the reflection by section 7 after the KA7 diagnostic)
The formula in step 2 is known only on the VS side. Unknown without a capture: the sphere-map texture, the PS blend (add vs lerp vs multiply by skin alpha),
the cb10 alpha reference, and which nodes get bit0. Inventing them would be the stopgap the user rejected (D-291, real render only).

## 5. Capture plan (live driver, one mirror capture batch)
1. Fly the cockpit with the exterior wing visible (look left/right 60-90 deg) in daylight m02a, and the chase view if reachable (the user's reference frame
   is a chase view). Record the game window and our eye frame at the same instants (>= 3 pairs, steady attitude).
2. DrawACM/DrawCPT census with cb10 (root constants), PSO blend desc and SRV list per draw of the player's mesh: log per draw `flags`, alpha ref, blend
   state (SrcBlend/DestBlend/op), t0 texture resource id + size + format, depth state. The flags-7 draws are the reflection pass: dump their t0 texture
   (expect a small sphere map) and record bytes +240..+255, +320..+335, +352..+367.
3. Dump the t10 record of one flags-7 and one flags-258 draw of the SAME mesh (same B): confirms the same geometry is drawn twice.
4. Dump the offscreen pass 1-3 RTVs (128x128) once per second to see whether they hold a shadow/blob projection.
5. With texture and blend known: add a `refl_air:` pass to `cockpit_model.hlsl`: second draw of exterior parts, matcap uv from the view-space normal,
   mask `saturate(10*n'.y)` * skin alpha/128, blend per step 2; plus a 3-light lit term from the stage light matrix replacing the single sun. Tests: CPU
   mirror in `shade_fragment` + WARP case, as for COCKPIT-4 glass.

## 6. Live diagnostic: `fxsig:ac0f1a60` (read-only, implemented, not yet run)
Arming (key-neutral): the existing stage option `fxsig:<8 hex>` with the reserved value `ac0f1a60`, e.g. `<stage>;fxmem:1;fxsig:ac0f1a60` (needs fxmem >= 1, the step-5 producer and the
root-constant witness / ownership pins for the flags). Code: `src/acz-vr/fx_diag_flare_air.h` (formatter, limiter), `gpu_copy_step5_hooks.inl` `step5AirDiagDraw`.
Output: one `fx-air-draw` line (< 426 bytes) per DrawCPT/DrawACM-identity draw of the continuous phase, at most 24 lines/s, 400/run (`sup=` = dropped since the last line):
`k=cpt|acm; flags=<cb10 dword0>; refl=<bit0>; rcm=<written-dword mask>; rc1..3; ps/vs; bl=on|off:src,dst,op/srcA,dstA,opA:m<write mask>; d=f<func>:w<write>; cnt/in/idx; rt=<res>(WxH:fmt); rtv; vp; roots=<param:kind reg=value,...>`.
Live-check list: (1) flags 7 (refl=1) and 258 lines both present for the same frame; (2) bl/depth of the flags-7 draws (add vs lerp vs multiply); (3) `roots=` T-slot handles
differ between the flags-7 and flags-258 draws (different bound texture table); (4) lines with `flags=unknown` mean the root-constant witness is off. NOT delivered: texture resource id/size
and the single-PNG dump of the flags-set draw's texture, and the +240/+320/+352 record bytes: the proxy tracks no SRV descriptor-to-resource map and has no read-only GPU texture
readback for arbitrary game resources (png_dump_queue.h only writes pixels already on the CPU), so that needs a separate descriptor-tracking feature; the table handles in `roots=` are the key to resolve it from a RenderDoc capture.

Start gate (batch K2): the 400-line run cap was used up on the runway before takeoff (step113 KA7). The limiter now counts and logs NOTHING until armed: (a) create or touch `%LOCALAPPDATA%cz-vrxdiag.arm` after the game started (the autopilot does it at T0; a file already present at the first poll is ignored), or (b) 90 s after the first considered draw (fallback). Arming resets the run/window counters, so the full 400 lines (24/s) are available in flight. Unit tests: `fx_diag_flare_air_offline_tests` (63 checks).
**Update CFID (2026-10-29, docs/world-render/cockpit-fidelity.md):** `srefl` now defaults ON through `cfid:1` (default); the j35jk vertex colours are all 128
(no baked shading), and the lit branch's 2 d^3 response is used on the skin plus a cockpit-rim sun shadow.
## 7. SKIN-REFL: the reflection pass, implemented (cockpit option `srefl:1`, default off until CFID; offline-tested, NOT live-verified)
### 7.1 Evidence (KA7 `fx-air-draw`, D:\acz-evidence-archive\step113\runKA7-logs\step5-producer.log, plus offline reads)
1. **Which mesh [E].** The flags-7 draws' index counts 9, 29, 9, 34, 22, in that order, are exactly `sum(strip vertices) + restarts` of j35jk.mpl **part 3** nodes
   n7..n11 (groups 6, 6, 5, 5, 53 = right/left elevon pieces and the grp-53 part; scratch `nodes.py` over `acm_lib.decode_acm`; the same rule gives the
   cockpit atlas draws 3354/477 = part 2 n3/n5). So DrawCPT draws the cockpit-view exterior LOD (part 3, skin texture table `...110a80`) twice: flags 2
   (blend off, writes `dst.a = skin texel alpha`: DrawCPT_p outputs `a = tex.a`, rgb = tex.rgb * colour) then flags 7. The cockpit interior (part 2) never
   gets flags 7. Earlier draws of each frame were suppressed by the line limiter (`sup=73/156`), so whether part 3 n2/n5 (fuselage/wing, 314/325 indices)
   also get the pass is not observed [U, INFERENCE: yes, every logged skin draw had it].
2. **Blend/depth [E].** flags 7: `bl=on:7,2,1` = SrcBlend DEST_ALPHA, DestBlend ONE, ADD: `dst.rgb += dst.a * src.rgb`; depth GREATER_EQUAL, write on;
   alpha `ONE/ONE` (irrelevant here). With DrawCPT_p `src.rgb = env.rgb * colour` and the VS colour replaced by the mask: **`dst.rgb += skin_alpha * env.rgb *
   saturate(10 n'.y)`**, no lighting, no vertex colour.
3. **VS maths [E DXIL]** (drawcpt-vs.dxil.txt label 143): `n' = (rows +256/+272/+288 of the t10 record / 16) applied to the int normal / 4096`;
   `u = saturate(n'.x/2 + 1/2)`, `v = saturate(n'.z/2 + 1/2)`, colour = `saturate(10 n'.y)`; per vertex (Gouraud), not normalised.
4. **Which texture [E data + I role].** Not in the .mpl (its only GIMs are part 8 skin, part 9 cockpit atlas). The per-stage **`data/map/<stage>/<stage>_ref.gim`**
   (32x32, 39 stages) are sky hemisphere maps: dark disc, bright horizon rim, one sun highlight. Their highlight sits on the **stage sun's azimuth** in every
   stage when the map is read with u = game world +X, v = game world +Z (sign of (2u-1, 2v-1) = sign of (sun.x, sun.z) for all 39, e.g. k00a sun
   (-0.70, 0.14, +0.70) el 8 deg -> highlight on the rim at (-0.79, +0.17); m05a (+0.99, 0.07, 0.14) -> (+0.87, +0.01); v08a z -0.06 -> -0.17; scratch `sunref.py`;
   low suns sit on the rim, high suns inside). So the record matrix maps the normal into the **game world frame** and `n'.y` is world UP: the mask is an
   **up-facing (sky-facing) mask**, not a view-facing one, and the map is the stage's sky reflection. The descriptor table `...11efe0` itself is not resolved
   to a resource (no descriptor map), so "flags-7 binds `<stage>_ref.gim`" is INFERENCE from the matching data; `data/effect/<stage>_ref.gim` (identical in 47/48
   stages, a radial glow) is not it.
5. **Domain [I].** The game adds in its display-domain RT (R16G16B16A16_FLOAT, fmt 10). The texel alpha scale (0x80 = 1.0) of the PC textures is assumed like
   our baker's (`alpha * 2`, clamped): skin alpha median 0.48.

### 7.2 Design (as implemented)
* **Data.** `research/world-render-data/refl_map.py <data_root> <stage_dir> [stage..]` writes `<stage>_ref.zvrt` (48-byte header ZVRT v1, w, h, kind 1, SHA-256;
  RGBA8, alpha 0x80 -> 255) **next to the .zvrs** (default `%LOCALAPPDATA%\acz-vr\stage\`). `load_stage_file` reads it after the stage (StageData::refl_*,
  not serialised; absent/malformed = no map, `refl_status` says why, the stage loads unchanged). No .zvrs format change, no rebake of stages.
* **Shader** (`cockpit_model.hlsl`; CPU mirror `refl_vertex` + `shade_fragment` + `render_reference`): vs_model computes per vertex `n_w = (dot(n, rx),
  dot(n, ry), dot(n, rz))`, `r = (saturate(n_w.x/2+1/2), saturate(n_w.z/2+1/2), saturate(10 n_w.y))`; ps_model, opaque pass, `kMatTexAlphaNotCov` (= the
  exterior skin) fragments only: `c_display += sreflk * tex.a * r.z * refl_map(r.xy).rgb` BEFORE the single sRGB->linear conversion. Folding the engine's
  second draw into the first fragment is exact for the engine's rule (same geometry, GREATER_EQUAL depth = the same surviving fragments, dst.a = this
  fragment's skin alpha) and keeps the add in the display domain as the game does (a separate additive draw into our sRGB eye target would add in linear).
* **Constants** 48 -> 56 DWORDs (`rx/refl_k`, `ry`, `rz`; root signature 57 of 64 DWORDs); `kCFlagRefl` = cflags bit 1. Basis = the game's world axes in model
  space from the same terrain camera frame as the COCKPIT-3 stage light (`refl_basis(cf.right, cf.up, cf.fwd, conv.yaw_sign, conv.yaw_offset_deg)`, the inverse of
  build_env_texture's heading convention).
* **Backend** (`xr_stereo_d3d12.cpp`): model table t0..t5 (`kModelSlots` 6); t5 = a w x h RGBA8 texture + persistently mapped UPLOAD buffer created on first use,
  copied only when the key (`BlitJob::cockpit_refl_gen` = the drawn TerrainRuntime's generation, so the TAKEOFF airbase uses its own k00a map) changes;
  `kCFlagRefl` is cleared in the eye constants unless the map is resident. Facts `cockpit_refl_uploaded/used`; summary-cockpit gains
  `refl=on|off;refl-frames;refl-uploads;refl-nomap;refl-map=present|<status>`.
* **Option** in the existing `xr_cockpit_model` value (no new key, budget unchanged): `srefl:0|1` (default 0), `sreflk:<0..4>` (default 1 = the engine's add),
  e.g. `auto;anim:1;srefl:1` (+9 chars). Part policy = every exterior skin part (INFERENCE from item 1; our vrcockpit draws part 0, the full exterior, not part 3).
* **Tests**: cockpit_model_offline_tests `test_skin_refl` (options, layout, basis incl. yaw convention, per-vertex term, fragment add, reference raster:
  analytic tail value, no map / option off / level flight / forward view = byte-identical to before), terrain_offline_tests `test_refl_sidecar` (format,
  refusals, path, optional load), `research/world-render-data/test_refl_map.py`, WARP harness case 13 (`c_model_refl_*`: real vs_model/ps_model vs CPU
  reference with a gradient map and two bases, max error 0.0017, upload once per key, no map / srefl:0 = the COCKPIT-3 image).

## 8. Shadows / three-light term: NOT implemented (data does not identify the lights)
The DrawCPT/DrawACM lit branch uses three directional lights (+128..+160 directions, +192/+208/+224 colours, +240 ambient, 2x^3 remap when +356 == 0). The KA7
`acm-record-hex` dumps (24 records, all early-ordinal prop draws with **+352.x = 0, lighting off**) carry colours light1 (74,74,70), light2 (46,46,39),
light3 0, ambient (100,100,105) /255 but garbage direction rows, and no aircraft (part 3 / DrawCPT) record was dumped. The stage mapset holds one sun
(0x58 el/az) + colour/ambient and a second el/az block at 0x88 (k00a: same angles): not enough to place three lights. Per the brief, the multi-light term stays
the single stage sun (COCKPIT-3). Baked vertex colours are already used. Needed: one DrawCPT part-3 t10 record with +352.x != 0 (live-check 6).

## 9. Live-check list (SKIN-REFL)
Prerequisite (executor, offline): `python -I research/world-render-data/refl_map.py <extract>\data %LOCALAPPDATA%\acz-vr\stage m02a k00a` (or all stages), then
`xr_cockpit_model=auto;anim:1;srefl:1` (keep the rest of the day value; check the 255-char limit).
1. `summary-cockpit` shows `refl=on`, `refl-map=present`, `refl-frames` = model-frames, `refl-uploads` 1 per stage switch (k00a takeoff -> mission = 2), `refl-nomap` 0.
2. Mirror pair (game window vs our eye, same instant, daylight m02a): look at the wings/elevons from the cockpit at level flight, banked 60 deg each way, and
   pitched up: the upper skin brightens toward the sun's azimuth with the stage's sky tint; under-surfaces never get it (mask = world up). Compare with the game's own
   cockpit-view exterior and the user's reference (docs/headset-tests/2026-10-28-aircraft-shading-explosion-original-reference.webp).
3. A/B `srefl:0` vs `srefl:1` with the same attitude: only skin pixels change; cockpit interior, canopy, HUD byte-identical.
4. Orientation sanity: heading toward the sun vs away (same bank): the highlight moves with the world, not with the head (world-frame map). If it instead follows
   the head, the record matrix is view space: swap the basis for the eye camera axes (one-line change in stage_lighting) and re-run.
5. Strength: if the game's skin looks brighter/duller, adjust `sreflk` (0.5..2) and record the value; if the whole wing glows, the PC texel alpha scale is
   0..0.5 (not 0..1): `sreflk:0.5`.
6. Diagnostic follow-up (needs a new read-only option): arm `fxsig:ac0f1a60` AFTER takeoff (the 400-line cap was spent on the runway) to see flags-7 on part 3
   n2/n5 and on DrawACM in chase view; dump one DrawCPT part-3 t10 record (+128..+367) to read the record matrix (+256..+288, expect world rotation x16) and the
   three lights (+128..+240, +352) for section 8.
