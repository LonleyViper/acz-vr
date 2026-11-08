# COCKPIT-4: canopy glass, procedural interior, control-surface/stick animation, gauges, other aircraft

Track COCKPIT-4 (D-302 overnight mandate: "cockpit and aircraft fully rendered with 6DoF"). Builds on COCKPIT-2/3 (cockpit-render-impl.md) and the
model decode (model-decode.md). Offline work only: no game, SteamVR or headset run. Built in `D:\b-c4` (Release, Ninja, tests on); ctest 300/300 after merging master (PERF).
Tags: **[V]** verified here by a test or a measurement, **[I]** inference / unverified, **[M]** made up (no game data behind it).

## 0. What changed (all inside the existing key `xr_cockpit_model=<id>[;opt:val]`; key off = unchanged)

| option | default | meaning |
|---|---|---|
| `canopy:0\|1` | 1 | canopy glass = per-channel TRANSMISSION of the world behind it + a Fresnel sky reflection (dual-source blend pass); 0 = COCKPIT-2 straight-alpha glass |
| `tint:r,g,b` | 0.951,0.940,0.935 | canopy transmission in the DISPLAY domain (WORLD-4 measurement); applied as `tint^2.2` in linear |
| `refl:<0..1>` | 0.2 | scale of the Schlick reflectance (F0 0.04) of the sky colour [M strength] |
| `dirt:<0..0.5>` | 0 | uniform sky-coloured veil (dust/scratches) [M] |
| `interior:0\|1` | 1 | procedural tub, seat, headrest, rails, side consoles, stick, throttle quadrant, rudder pedals [M]; replaces the COCKPIT-3 `floor:1` quad (0 = floor quad again) |
| `stick:centre\|side\|none` | centre | interior stick: centre (Draken, MiG-21), right-console side-stick (F-16 etc., see section 5 table), none |
| `anim:0\|1` | 0 | drive the control surfaces + stick/throttle/pedals from the E6 attitude rates (needs the terrain key for E6, else neutral) |

Files: `src/acz-vr/cockpit_model.{h,cpp}` (options, loader fields, `build_mesh` classification/interior/ranges, `shade_glass`, `pose_anim`, `anim_update`,
CPU reference pass 2), `cockpit_model.hlsl` (`ps_glass`, model-space position varying, 48 root constants), `xr_stereo_d3d12.cpp` (glass PSO with
`SrcBlend ONE / DestBlend SRC1_COLOR`, persistently mapped UPLOAD buffer for the animated range + per-recording copy into the VB), `xr_stereo_output.*`
(stage sky colour for the reflection, E6 sample -> `anim_update`, posed range on the job, `event=cockpit-model-c4` line, `summary-cockpit` anim tokens),
`CMakeLists.txt` (fxc `ps_glass`), baker `research/world-render-data/{acm_lib,model_baker}.py` (fitted hinge rest poses, `.zvrm` flag 8),
`aircraft_cockpits.py` + `aircraft_cockpits.json` (section 5), `tools/live/install-assets.ps1 -ModelDir`.

Mesh vertex order is now `[opaque static][opaque animated][blend][canopy glass]`; the backend refuses a mesh whose ranges do not add up (as before).

## 1. Canopy glass

**Measurement it reproduces** (WORLD-4, world4-impl.md 2.2) [V there]: the game's cockpit view tints the sky by (0.951, 0.940, 0.935) relative to the HUD view.

**Which geometry.** The Draken cockpit part (p2) has one big 0x2000 glass node (n1, 34 tris, atlas tint corner, texel (121,120,119) a 0.15), but it covers
only the overhead/side canopy: rendering a glass mask from the pvb eye shows the whole windscreen area inside the bow is NOT covered [V,
`D:\acz-extract\out\cockpit4\j35j_fwd_glassmask.png` first version]. The exterior model's canopy (p0 n10, 36 tris) + windscreen (n11, 40 tris) enclose the
cockpit completely, but seen from inside they are back faces (culled). `build_mesh` therefore takes, in order: exterior blend parts around the eye
(extend > 1 unit left and right of the eye, above the eye's height - 0, within 15 units fore/aft) emitted FLIPPED (corner order 0,2,1, normals negated) so
they face the eye; when such parts exist the cockpit-part canopy glass (n1) is not drawn (it would tint the overhead view twice); otherwise the
cockpit-part glass (wide, top > eye + 0.5) is used. Gauge glasses and the reflector sight stay ordinary straight-alpha glass, and the HUD-glass
locator is unchanged (part 4 on the Draken). F-16C / MiG-21: exterior canopy glass found too (2 parts each).

**Shading** (`shade_glass`, mirrored exactly by `ps_glass`): `c = |n.v|`, `F = F0 + (1-F0)(1-c)^5` (F0 0.04), `veil = sat(refl F + dirt)`,
`trans = tint^2.2 (1 - veil)` per channel, `add = tonemap(srgb_to_linear(sky) exposure) * veil`; blend `dst = add + trans * dst` (dual source, SRC1_COLOR),
`alpha = a + (1 - a) dst.a` with `a = 1 - mean(trans)`; depth test on, no depth write, drawn after the blend range. `sky` = the stage's sky row 1 stop 1
(WORLD-2 env, display) when the terrain runs, else (0.62, 0.68, 0.74) [I].
Why `^2.2`: the blend runs in linear (sRGB RTV) while the measured ratio is of display values; a linear factor t^2.2 keeps the display ratio within 0.005 of
t for display levels 0.55-0.80 (the sky rows) [V test `test_c4_glass`]. A sky-coloured reflection over a sky background is invisible (it replaces
transmitted light of the same colour); over dark ground it brightens at grazing angles - the intended "slight reflection".

**Result** [V offline, CPU reference of the real Draken bake from the pvb eye over a flat sky]: median display ratio through the glass (0.951, 0.941, 0.933)
vs the target (0.951, 0.940, 0.935) over 239,056 sky pixels (ratio of two previews, canopy on vs all glass off; images
`D:\acz-extract\out\cockpit4\j35j_fwd*.png`). The COCKPIT-2 straight-alpha path gave (0.922, 0.932, 0.933) where n1 covers the view and 1.0 (no glass) in the windscreen.
Not modelled: edge darkening (no per-fragment distance to the frame is available without new vertex data; `dirt:` is uniform), frame reflections, the
game's possible environment map behind the 0x2000 flag.

## 2. Interior (procedural, [M])

The game has no seat, stick, throttle, floor or rear cockpit for any player type [V survey of all 36 player types, `aircraft_cockpits.py`]: part 2 (cockpit)
has no animated nodes except on the fictional `zoef` (group 0x1b/0x14 nodes, see section 4); part 5 is an ACM with one node and no mesh on 34 of the 38
model types in plmodel (3 nodes/0-2 meshes on f14d, itesu, tnd4, x02a: not cockpit furniture); part 3 = loose wing/fin pieces, part 6 = low LOD exterior, part 7 = stores.
So `build_mesh` generates, at load, from the eye point E and the cockpit part's footprint (x/z extent and lowest point of the drawn opaque cockpit
geometry), in metres at 5 u/m (`kModelUnitsPerMetre`, independent of the head scale `upm:`):

| piece | geometry (metres rel. to E; x right, y up, z aft) | colour (display RGBA, untextured) |
|---|---|---|
| tub floor | y = E - 1.05 (never above the old floor closure), whole footprint, faces up | (34,36,34) |
| side walls | footprint x edges, floor .. E - 0.30, face inward | (38,40,37) |
| front wall | footprint front, floor .. min(E - 0.25, the line of sight over the glareshield - 0.02 tan), faces aft | (38,40,37) |
| rear bulkhead | footprint rear, floor .. the exterior airframe's highest point within 0.2 m before / 0.4 m behind it (clamped E-0.2..E+0.5): from inside the canopy the spine skin is back-facing, a lower bulkhead left a hole into the empty fuselage [V render] | (42,44,40) |
| seat | base, pan (top E - 0.80, z -0.22..), back, headrest (E -0.06..+0.16, front 0.11-0.20 behind E), ejection rails; clamped inside the footprint | (58,60,50) / (38,40,38) |
| side consoles | both walls, floor .. E - 0.47, inboard edge 0.245 from the centre line | (34,36,34) |
| stick (centre) | boot + shaft + grip, pivot near the floor 0.40 ahead (clamped), top E - 0.52 | (52,52,50), grip (22,22,22) |
| side-stick (`stick:side`) | right console top, x +0.31, 0.12 ahead | grip (22,22,22) |
| throttle | quadrant on the left console (x -0.30) + lever and handle (pivot at the quadrant top) | (34,36,34) / (26,26,26) |
| rudder pedals | two plates x +-0.12, 0.85 ahead (clamped), hanging from a pivot 0.30 above | (46,46,44) |

Lit like the rest of the model (stage sun, ambient share, display domain). Every triangle is
CCW-front on its normal's side [V winding test over the whole synthetic mesh]. j35j: 202 interior triangles (118 static, 84 animated). Previews (CPU reference = the shader, `cockpit_model_offline_tests
--load ... --png`, not committed): `j35j_down.png` (stick, pedals, consoles, floor; no more sky/fuselage holes), `j35j_left.png` (throttle on the console),
`j35j_back_lean.png` (seat + bulkhead closing the spine), `f16ck_right.png` (side-stick), `m21bk_*`. **Default on**: looking down/back is no longer an
empty void or the inside of the airframe, the forward view is unchanged except a 2-row sky slit under the Draken glareshield that is now closed (56 px of
480,000) [V diff]. It is plainly boxy and untextured [I: acceptable for "not a void"; judge in the headset; `interior:0` restores COCKPIT-3].
(SEAT-CLEARANCE 2026-10-30: the headrest front is now >= `seatback` 0.30 m behind the eye, cockpit-fidelity.md section 5; the 0.11 m below is the COCKPIT-4 original.) Risks: the headrest is ~0.11 m behind the eye; leaning back further puts the eye inside the box (its inner faces are culled: the box disappears).

## 3. Animation (`anim:1`)

**Hinge rest poses (decode correction).** For animated nodes, COCKPIT-1 ignored rot[0] (rest = 0, "20 deg = deflection range"). That holds for most
surfaces but not all: the stored rot[0] is `rest + range`. A grid search of rot[0] against the static geometry (median nearest-vertex distance) gives on
the Draken: inner/mid elevons rest 0 (stored 20), OUTER elevons rest 180 (stored 159.9: the mirrored part; COCKPIT-1 bakes put them 0.32 units off),
rudder rest -90 (stored -105; 0.63 off before), upper-right airbrake rest 180 (stored 180) [V, 0.000-0.08 unit fits]. Naive "round to 90 deg" over all
plmodels is better on 162 and worse on 255 nodes (mostly landing-gear part 1), so the baker FITS per node: candidates {0, stored, stored rounded to 90}
are tried against the static vertices, the best wins if its median distance <= 0.15 units (`acm_lib.fit_rest_angles`); otherwise rest 0 and "not
fitted". `.zvrm` header flag 8 (`kZvrmAnimRest`): part record +120 f32 rest, +124 u32 flags (bit 0 = fitted); older loaders refuse flag 8 (so a
COCKPIT-4 bake needs a COCKPIT-4 DLL), the new loader accepts older files (rest 0, nothing fitted) and refuses rest/flags in a file without flag 8.
Rebaked (game-derived, not committed): `D:\acz-extract\models-c4\{j35jk,m21bk,f16ck}_vrcockpit.zvrm`; install with
`install-assets.ps1 -ModelDir models-c4`.

**Roles from the node group id** [V on j35j/m21b/f16c by position; I for other types]: 1/2 = left/right elevator (on the Draken both INNER elevons carry 1),
3 = rudder, 5/6 = left/right aileron (Draken outer elevons), 7 = flaps, 0x11 = leading-edge flaps, 0xf = airbrakes, 0x17 = fin (static), 0x1d = small
spine part. Driven: 1/2 (pitch + 0.5 x roll, taileron share), 5/6 (roll), 3 (yaw). A part is driven when its rest was fitted OR its rest is 0 with a
stored 5-35 deg range (the F-16/MiG tails and rudders do not touch the static mesh, so the fit cannot confirm them), AND its hinge axis (local X) is
lateral (|x| > 0.85) for elevators/ailerons or in the symmetry plane (|x| < 0.3) for rudders (the MiG-21 stabilator nodes have a 50 deg swept local X
and are left static). Drawn result: j35j 7 surfaces (2 inner elevons, 4 outer, rudder), f16c 5 (flaperons, tails, rudder), m21b 3 (ailerons, rudder);
airbrakes/flaps/LE flaps/the 0x1d part stay at rest (no data source). Gear: part 1 is not in the bake (gear-down pose; in flight it is retracted).
Canopy: closed (no state source).

**Deflection sign**: positive command = trailing edge UP: `gain = sign((axis x (centroid - pivot)).y) * |range|` (rudder: `.x`, positive = trailing edge
to +X = nose right). Pose: `v' = pivot + R(axis, angle) (v - pivot)`, normals rotated; descendants of a driven part move with it.

**Commands from the attitude** (`anim_update`; APPROXIMATION - the game's stick/throttle are not read): consecutive E6 samples (new sequence only, time =
the frame's publication time or the consumer clock) -> body angular velocity from `R_prev^T R_cur` (axis-angle / dt) -> pitch = w_x / 0.61 rad/s
(35 deg/s), roll = -w_z / 2.62 rad/s (150 deg/s; D-306: 1 s tap ~145 deg), yaw = -w_y / 0.26 rad/s, clamped to +-1, first-order lag tau 0.12 s;
throttle = 0.5 + 0.5 x along-track acceleration / 25 u/s^2 (5 m/s^2), lag 0.48 s. A gap > 0.5 s or a non-finite sample restarts the differentiation.
Without E6 (no terrain key) the commands stay neutral (rest pose).
Interior: centre stick +-17 deg pitch (pull = top aft) / +-15 deg roll; side-stick +-4 deg; throttle lever +-30 deg (forward = more); pedals +-10 deg
(right pedal forward for nose right).

**GPU path**: the animated range is posed on the CPU each frame (j35j: 561 corners, 18 kB) and copied into the default-heap VB from a persistently
mapped UPLOAD buffer at the start of the recording (NON_PIXEL_SHADER_RESOURCE -> COPY_DEST -> back), after the allocator reset proved the previous
recording complete; a count mismatch skips the copy (rest pose). No shader change for animation.
Tests [V]: `test_c4_anim` (rest = neutral within 1e-5; elevator TE up by sin 20 deg about the hinge, hinge line fixed, normal tilt; taileron share +-10 deg;
stick/throttle/pedal directions; rigidity 1e-4; apply_pose), `anim_update` on synthetic 60 Hz flights (pitch 0.3 rad/s -> 0.492 = 0.3/0.61, right roll,
nose-right yaw, +-acceleration -> throttle >0.7/<0.3, saturation, gap/NaN resets), WARP case 12 (`c_model_c4_anim`, `_anim_6dof`: the real VB copy +
shaders vs the CPU reference with the same posed mesh, max error 0.008, <= 3 edge pixels), consumer test (anim:1 without E6 -> neutral posed range on
every job). Chase previews `j35j_chase_{rest,rollR,pull_yawR}.png` show the right/left elevon split and the inner elevons up on a pull.

## 4. Gauges

No separable needle/ball geometry exists for any player type except the fictional ADF-01 (`zoef`: part 2 has 22 animated nodes, group 0x1b, and a 0.02-unit
node in group 0x14 - not decoded) [V survey]. The Draken's instruments (alpha gauge, tape instruments, side gauges, ADI area) are texture-only in the
128x128 cockpit atlas, so nothing is driven. Limitation documented; a later option could draw procedural needles over registered gauge centres (needs a
manual gauge map per aircraft) from E6 data: altitude = campos.y, airspeed = |d pos / dt|, attitude = the E6 rotation [I].

## 5. Other aircraft, model table and player-aircraft detection

`research/world-render-data/aircraft_cockpits.py <a/data> aircraft_cockpits.json --zvrm-dir D:\acz-extract\models-c4` writes the table (committed:
type ids, model ids, pvb eye points and small facts; no geometry): all 36 types of `plmodel/playerlist.txt` -> skins present, runtime model id
`<type><skin>_vrcockpit` (skin preference k, e, l, m, n, u, v, o: parts 0-7 are identical across skins, model-decode.md), the `.pvb` eye point (bytes
40-52), `.pvb` +56 (90 on every type except su32 = 50: plausibly the game's cockpit FOV [I]), the cockpit glass count, the exterior animated groups, and the
interior stick style (side-stick for f16c, f16x, f35c, fa22, rm01, su37, su47, yf23 [I: real aircraft]). Eye points: 33 of 36 are unique; f15c/f15e/f15s
share (0, 5.63, -49.05). Two seats side by side: e06b (x -1.885) and su32 (x -2.1).

F-16C and MiG-21bis vrcockpits render cleanly with the COCKPIT-4 defaults (`f16ck_{fwd,down,right}.png`, `m21bk_{fwd,down,left}.png`): exterior canopy
glass found, interior fits the footprint (the F-16's lower front gap is closed by the front wall), F-16 with `stick:side`.
Runtime use today: `xr_cockpit_model=f16ck_vrcockpit;stick:side`. A future `auto` value would map the detected type through this table.

**Detecting the player's aircraft (research note, read-only; nothing implemented):**
1. **E6 eye offset (cheapest, no memory scan) [I, strong]**: the E6 camera position minus the aircraft position, rotated back by the aircraft attitude,
   equals the type's `.pvb` eye point (verified for the Draken, D-295/D-303: (0, 3.575, -36.3)). One frame in the cockpit view identifies 33 of 36 types
   (the F-15 family ties). Needs the E6 aircraft position field next to campos (already read by the probe) and the cockpit view (not the HUD/chase view,
   where the camera offset differs).
2. **Memory fingerprints (like the stage detection, D-310/D-313) [I, proven pattern]**: the PC port keeps stage files raw in memory; if the aircraft
   `.mpl` is also resident, 32-byte pieces of part 2 (cockpit, per type) and part 8 (skin, per skin letter) identify the type AND the skin. Reuses
   `stage_fingerprints.py` / the `auto` scanner; resolves the F-15 tie.
3. **Replayed cockpit draws [I]**: the producer already captures the game's cockpit draw (part 2 + the 128x128 atlas); hashing the atlas upload or the
   cockpit vertex buffer per draw and matching against offline hashes of each type's part 9/part 2 also works, at the cost of a hook in the replay path.
Recommended order: 1 (zero cost) with 2 as the tie-breaker; both read-only.

## 6. Evidence, tests, build

* `cockpit_model_offline` 505 checks (COCKPIT-4: options/loader fields and refusals, canopy selection incl. exterior flip and no-double-tint, glass rule
  exactness and the display-ratio test, interior presence/winding/footprint clamp, stick variants, anim roles/locating rules, pose transforms, rate ->
  command); `xr_composite_warp` case 12 on WARP with the REAL shaders and backend: canopy glass per pixel = `add + trans * background` over 1,198 lit
  pixels, worst 0.0071 (8-bit); c4 forward/down/anim/anim_6dof vs the CPU reference max error <= 0.008, <= 3 edge pixels; count-mismatch copy refused;
  `xr_stereo_cockpit_model_offline` 464 checks (c4 config line, posed range on every job); `test_model_baker.py` 6 tests (fit_rest_angles, part fields).
* Release ctest **300/300** after merging master with PERF (299/299 before) in `D:\b-c4` (configuration: tests on, d3d12 proxy option off; the "301" baseline in the mandate presumably counts a
  configuration with more targets [I]).
* Not verified: anything live (game, null driver, headset); the glass look in the headset (only the forward ratio is measured); the interior's scale/feel;
  whether the E6 rotation rate is smooth enough at 60 Hz for a calm stick (lag tau 0.12 s, untested live); GPU cost (561 posed corners per frame, one
  extra draw per eye).

## 7. Live recipe (driver; null first)

1. Build/copy the COCKPIT-4 DLL set (an older DLL refuses the flag-8 bakes and keeps the replayed cockpit, logged `cockpit-model-disabled`).
2. `pwsh -NoProfile -File tools/live/install-assets.ps1 -ModelDir models-c4`
3. `vr-test-start.ps1 ... -TerrainRender 'auto;env:1;look:1;props:1;trees:0' -DepthFix -CockpitModel 'j35jk_vrcockpit;anim:1' -PngOnce`
4. Log: `event=cockpit-model-c4;canopy=exterior-flipped;canopy-parts=2;...;interior=procedural;...;anim=on;anim-surfaces=7;anim-not-driven=5;
   anim-pieces=11;anim-vertices=561`; `summary-cockpit;...;anim-frames=` rising, `anim-updates=` rising (E6 samples), `stick=p,r;pedal=y;throttle=t`
   following the flying. A/B: `canopy:0`, `interior:0`, `anim:0`.

## 8. Proposed decision entry (for the coordinator)

D-3xx - COCKPIT-4: canopy glass (exterior canopy+windscreen seen from inside, dual-source transmission tint (0.951,0.940,0.935)^2.2 + Fresnel sky;
offline forward ratio (0.951,0.941,0.933)), procedural interior (made up; default on), anim:1 (fitted hinge rest poses: Draken outer elevons 180,
rudder -90 were mis-posed in COCKPIT-1 bakes; group-id roles; commands from E6 body rates), gauges texture-only (no needles in any player type),
36-type cockpit table + detection plan (pvb eye offset from E6 identifies 33/36 types). ctest 300/300 (D:\b-c4, master with PERF merged). Nothing live.
