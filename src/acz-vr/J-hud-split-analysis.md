# J-hud-split-analysis: target markers vs instruments, radar blips, two-layer HUD design

Read-only analysis (no source edited, nothing built or run). Every claim is tagged
**[E]** established from a log/image/code line cited here, **[I]** inference from several [E] facts,
or **[G]** guess / unverified. Primary evidence:

* run 7 observer lines: `research/experiments/step14-live-run7-hud/logs-final/step5-producer.log`
  (`hud;` / `hud-state;` records, frames 1,2,3,600,1200,...,9000; 64-record cap per frame, see J-171).
* run 11 PNGs: `research/experiments/step14-live-run11-full/png/*-s1/-s2-*` (hud-on-black, hud-alpha-raw,
  hud-over-world) and `NOTES.txt`; HUD counters in the producer logs of runs 8-11 and the L1-logs
  copies (`continuous-hud` lines).
* code: `gpu_copy_step5.h` (`hud_decide`), `gpu_copy_step5_hooks.inl` (`step5HudDraw`, `hudRegion*`,
  `step5RecordHud`), `stereo_frame_source.h`, `xr_stereo_output.{h,cpp}`, `runtime_config.{h,cpp}`.
* Caveat on "test-5 logs": `research/experiments/L1-logs/20261024-194004/png/` is a STALE COPY of run 11's
  PNGs (identical md5 of `*-s1-hud-on-black.png`, file times 12:52, pid 4132, clock 19'37"78). Test 5 did not
  run `png_once`; there is no PNG and no observer (`gpu_copy_step5_hud_observe`) output from test 5. Only its
  `continuous-hud` counters are real test-5 evidence.

## 1. Which HUD draws are the target markers

### 1.1 What the observer shows (run 7, frame 1; D-258 key set)
The HUD region (after the last cockpit member until the first RT change) is draw tail_seq 8..65 = 58 draws
(`hud-frame draws=58`, `end=rt-change`, run 8-11 all frames) [E]. All HUD draws share one VS/PS pair
(vs 534f29ca..., ps d34ee153...), depth func ALWAYS, depth write 0, blend enabled, write mask 15, viewport
2560x1440 [E]. They differ only in PSO (id) and count/indexed:

| tail_seq (frame 1) | PSO (id) | count | note |
| --- | --- | --- | --- |
| 8-12, 35, 37, 41 ... | cb80e0 (13219), indexed | 89/44/89/39/59, 84/69/79 ... | triangle-list text/panels [I] (large counts) |
| 13,14,16-18,20,21,23,... | f4efd0 (188624), indexed | 4,4,4,5,5,7,8 ... | line strips/boxes (instrument ticks, frames) [I] |
| 45 | 7e6ab0 (622686) | 24 | one-off, before the markers [E] |
| **47, 49, 51** | **07a9c0 (597979), indexed, depth func 5 = GREATER** | **9 each** | **target boxes [I]** |
| **48, 50, 52** | cb80e0 (text PSO), indexed, depth ALWAYS | 19 each | **"FRND" labels [I]** |
| 53 | f4efd0 | 6 | radar start [I] |
| 54 | 6c8620 (419), non-indexed, 4 verts | 4 | one-off radar-area quad [I] |
| 55-58 | f4efd0 | 4,4,4,4 | radar cross + two cone lines [I] |
| 59-63 | f4efd0 | 35 x 5 | five radar rings (35-vertex closed strips) [I] |
| 64-65 | 1dedc0 (604464), stencil front pass op 3 (REPLACE) | N4, I6 | stencil mask quads, last two HUD draws [E for the state; role I] |

[E] Frame-1 sequence is box(9) label(19) box label box label at 47-52, between the "TIME/ALT" text group
(<=46) and the radar group (>=53).

Why the GREATER draws are the markers:
* [E] The only HUD draws with depth func GREATER (5) are PSO id 597979, count 9, three per frame in frames
  1,2,3,600 (and 2 at 1200, 6 at 1800, 3 at 2400, 1 at 3000, 0 from 3600 on). No other pipeline has func 5.
* [E] Run 11 `continuous-hud`: `depth-tested=24126` over 8042 valid frames = 3.000 per frame, and both
  run-11 PNG samples (frame 2 and frame 69) show exactly three blue "FRND" boxes. Run 7 frame 1 also has exactly
  three. The three FRND boxes move between s1 and s2 (80 ms apart) and the labels move with them.
* [E] Other runs: `depth-tested` per valid frame = 0.19 (run 183702), 0.22 (173656), 1.84 (test 5, 194004),
  3.0 (runs 8/11). The count is target dependent, as markers must be; fixed instruments would give a constant.
* [I] The blue label ("FRND") follows its box immediately, drawn by the shared text PSO with 19 indices. Frame
  1800 (6 GREATER draws) shows sequence B L B L B L B B L(54 idx) B, so a box is not always followed by an
  own label, and labels can be longer text (the 54-index draw). The label rule is therefore heuristic.
* [G] Enemy / lock-on boxes use the same PSO (counts in test 5 average 1.84 with 0 refusals) but no enemy
  image or observer record exists. Whether the red lock-on reticle (the one with lock/range text) is also
  GREATER-tested is unknown.

Everything else in the region is a fixed instrument [I]: the large cb80e0 text/panel draws (SCORE, TIME, SPEED,
ALT, GUN/MSSL/SAAM/DMG, COVER), f4efd0 line groups (attitude ladder 4/5/7/8-index pieces, heading ticks,
speed/altitude tapes), the radar group 53-63, and the closing stencil quads 64/65.

Ordinal stability: tail_seq is NOT stable (first marker at 47 in frames 1-600, 48 from 1800; the instrument draws
before it are identical in all frames 8..46). PSO ids (597979 etc.) are stable only within a process. The
only process-independent signature on record is depth func GREATER on a HUD-region draw.

### 1.2 The 3 m vs 40 m symptom matches
[E] Test 5 config: `hud-quad-distance-m=0.80;hud-scale=0.850` (xr-stereo.log `hud-config` / `first-hud-layer`
55.2x32.8 deg). The marker boxes sit on a 0.8 m plane while the world quad is at the world distance. Each eye's
parallax between the marker and the distant target is about `0.032 m * (1/0.8 - 1/D)` rad = 2.3 deg per eye at
D = 40 m (and 4.6 deg between the eyes). Also scale 0.85 breaks angular registration by itself [E, J-hud-placement].

## 2. Why no blips are visible on the radar

Established facts:
* [E] All HUD counters show 0 refusals (`refused=0(state:0,descriptor:0,format:0,cap:0)`) in runs 8, 9, 10A/B,
  11 and test-5-era logs. `hud_valid` frames = published frames (run 11: 8042/8042; test 5: 10110 valid, 163
  invalid with refused=0, so those are draws==0/other, not blip refusals). So "radar blips were refused" is
  ruled out for the draws that ARE in the region.
* [E] The captured radar has: green outline rings, white cross + cone lines, one small hollow triangle at the
  upper-left (identical position in s1 and s2 of run 11), no dots/arrows/squares. The game's own radar in the
  pause screenshot `step14-live-run11-full/game-window-shots/acz-20261024-125357.png` shows the same hollow
  triangle at the same place, the same rings, and a translucent (not black) background, plus a faint player
  triangle in the centre. The friendlies do not appear on the game's own radar in that shot either, so "no blips"
  on the radar in run 11 is NOT proof of a capture loss. The user reports it with enemies (test 5): no
  enemy-containing image exists to check.
* [E] The radar group (53-63) is the same in every frame regardless of the number of boxes (frame 1: 3
  targets, frame 3600: none); its constant draw list contains no per-contact draw [I].

Leading hypothesis (H-A, the strongest, [I] with [G] parts): the radar blips are drawn after the HUD
region ends and are never replayed.
* [E] Region end is latched at the FIRST RT change (or scene transition out of RENDER_TARGET):
  `hudRegionEnd(... "rt-change")`; `hudRegionEnded()` then makes `step5HudDraw` return. It is reopened only by a
  later cockpit MEMBER (`hudRegionReopen()`), never by a draw returning to the scene target. `end=rt-change` is
  recorded in 100% of frames (run 11: 8042/8042).
* [E] The observer keeps recording after the region end and shows a second tail: in frames 3000/3600/9000
  (tail_seq at/after `rt_change`): `a3050` (VS 127819e3 / PS 7e94d4fc, 3 verts, viewport 2405x1352, depth LESS
  with depth write, no blend) = the tone-map/copy pass on the R10 target, then draws on a target that the observer
  labels `rt_class=scene` (FP16 20a024ccbc0): `04830` N4 (stencil NOT_EQUAL), `dedc0` N4, another `a3050` N3, then
  `03510` (I14 / I99 / I19, varying frame to frame) and `ad6c0` (I5 x1-3, one run of 3 in frame 3000), all with
  **stencil func 3 = EQUAL**, depth ALWAYS, blend on. At frame 9000 the last observed record is `4efd0 I7`.
* [I] The varying-count, stencil-EQUAL draws (03510, ad6c0) clip to a stencil mask (written by the replayed `dedc0`
  REPLACE quads at 58-61) and are the best candidates for the radar contacts (small squares: I5 = closed
  5-vertex strips; and the varying I14/I19/I99). They are the only draws whose index count changes with game state.
* [E] Even if the region continued, `hud_decide` would refuse them: stencil func EQUAL gives
  `stencil-test-not-always` (state category) and `a3050` has a smaller viewport and LESS depth write
  (`depth-test-not-replayable`), which would invalidate the frame. So today the late tail is cut by design.
* [G] Whether the post-`a3050` draws really render into the scene target again (the observer's `rt_class=scene`
  may be a stale cache; the a3050 record itself has `rt=0 unknown`), or into the R10 output after a copy, is
  unproven. If they draw into the output (swapchain format), they are not part of any scene-target HUD and a
  different capture would be needed (replay into the HUD target, with the output pass's own blend).
* [G] A mid-frame order dependency: the first radar-group draws happen before the dedc0 stencil quads; the
  stencil written by replayed dedc0 goes to the private DS, not the game's, so a replay of the late EQUAL draws
  would work only if the private DS keeps that stencil (it is the same DS, cleared once per frame).

Secondary hypotheses:
* H-B (the radar is opaque black): [E] the stored alpha over the whole radar square is ~1 (`hud-alpha-raw.png`:
  a solid white 313x320 square at the radar, nothing else visible at this scale) while the game shows it translucent.
  [G] constant blend factor / PS2 FIX-style alpha written to alpha, as J-hud-colour hypothesised (the PSO's alpha
  blend factors and the blend factor are not recorded). This is a coverage problem and explains the black
  square, not missing blips; if the blips were drawn they would sit on top of it. It does hide any blip drawn
  BEFORE the background (the hollow triangle is seen, so the order of the bg vs triangle is: triangle visible on
  top) [I].
* H-C (alpha rule): blips are small and dark/low-luminance; the consumer alpha max(a, max(rgb)) would keep
  coloured ones visible; red/green blips are bright, so H-C is unlikely [I]. Not testable offline.
* H-D (depth/scissor): no scissor or depth state of HUD draws fails: depth ALWAYS for all but the boxes, the viewport is full
  for all region draws, none refused [E]. Ruled out for in-region draws.
* H-E (cap): `cap-frames=0` in every run [E]; the observer's 64-record cap affected only the observer. In
  frame 1 the observer cut the list at seq 67 (`truncated` record), so the late tail of the first frames was not seen.
* H-F (blips are in a different list/generation or an indirect draw): [G]; the observer covers only the
  active list/generation (J-171 H3 caveat).

What would settle it (all read-only/default-off additions, no game change):
1. Extend the observer to run to Close (lift the 64-record cap for the tail; add RT identity per record, topology,
   `depth_enable`, blend factors (src/dst/alpha ops), and the current blend factor + stencil ref), in a run with
   enemies visible and radar contacts (test-5 scenario).
2. A `png_once` variant that captures the game's FINAL output (after tone map) so blips can be seen where they
   land; or compare a readback of the scene target after the late tail.
3. A counter that logs `late-scene-draws-after-region-end` (count, PSO ids) per frame in `continuous-hud` so
   H-A is checkable in every normal run.

## 3. Minimal design: split the HUD into an instrument layer and a marker layer

Goal: instruments on the cockpit glass (`xr_hud_quad_distance_m`, e.g. 0.8 m, scale as wanted) and the markers on a
second quad at the world distance with the SAME angular placement and scale 1, so they register with the world
quad (and with distant targets) and have almost no disparity.

### 3.1 Producer (`gpu_copy_step5*`)
New key `gpu_copy_step5_hud_markers` (default off, latched like `gpu_copy_step5_hud`; requires it). Cost [I]: one more
private FP16 2560x1440 RT per slot (29.5 MB; +88 MB for 3 slots), the existing HUD DS can be shared because the
marker boxes do not write depth/stencil (depth write 0, stencil ALWAYS/KEEP in frame 1); a separate DS is the safe
variant (+88 MB again). Run 8 measured +188 MB commit for the whole HUD, so this is the same order.

Classification in `step5HudDraw` after `hud_decide` accepts a draw (all state already known: `d.pso`'s depth func
via `v.depth_tested`):
* marker box: `v.depth_tested` (depth func GREATER/GREATER_EQUAL, the existing counter) [E: 3/frame in run 11].
* marker label: the first draw after a marker box that (a) is indexed, (b) is not depth-tested, and (c) has the same
  PSO route as the first such draw following the first box seen in this frame (the text PSO), AND the next
  draw before it was a marker box. Anything else is an instrument. [G] heuristic, validated only on frames 1 and
  1800 of run 7: the 54-index text after the 7th box is attributed to a box that has no label of its own and
  could be an instrument line.
* Mutually exclusive: a draw goes to exactly one target. Region logic, caps, refusal categories and the clear
  (one ClearRTV [+ClearDSV] per target per frame, `h.cleared` becomes per-target) are unchanged. Counters:
  `marker_draws`, `marker_refused`; a refused marker draw invalidates only the marker layer (`hud_markers_valid=0`),
  instrument validity (`hud_valid`) is computed from instrument draws only; a refused instrument draw still
  invalidates `hud_valid` as today.
* `step5RecordHud` takes the target RTV/DSV from the class; everything else (OMSet(private), game Draw, OMSet(restore))
  is identical. The ClearRTV/ClearDSV of the marker target are emitted lazily on the first marker draw, so frames
  without markers cost nothing and produce `hud_markers_valid=0` (no layer).
* Slot lifecycle: `hudMarkerRt/hudMarkerRtv/heap` per slot, created in `step5CreateHud` next to `hudRt`, same
  fence/quarantine/discard handling as `hudRt`, returned in RENDER_TARGET. PNG once: add `hud-markers` image
  (png-once footprints 3 -> 4) to get ground truth.

### 3.2 `StereoFrame` (additive, after the existing fields)
```
ID3D12Resource* hud_markers = nullptr;  // private target of the marker draws; nullptr unless hud_markers_valid
bool hud_markers_valid = false;         // >=1 marker draw replayed and 0 marker refusals
uint32_t hud_marker_draws = 0, hud_marker_refused = 0;
```
Same size/format as `hud`, premultiplied, RENDER_TARGET at publish/release, covered by the frame fence. `hud`
then contains only the instruments when the split key is on (marker draws are NOT also drawn into `hud`).
Frames with the key off: unchanged (marker draws stay in `hud`). Consumers that do not know the field keep a
HUD without markers.

### 3.3 Consumer (`xr_stereo_output.{h,cpp}`, `xr_stereo_d3d12.cpp`, owner layer list)
* New `Settings::hud_markers_layer` (key `xr_hud_markers_layer`, default off, requires `xr_hud_layer`), new
  target index `kMarkers = 4` (kTargets 5), a 5th swapchain of the world-quad extent created only with the key;
  `BlitJob` gets `markers` + `use_markers` (same mode-1 premultiplied shader as the HUD, no shader change, same
  clear-to-transparent policy on unreadable source).
* `use_markers` = `s_.hud_markers_layer && f.hud_markers_valid` plus the same validity checks as the HUD
  (format, not aliasing eye/world/hud, lens valid). Frames without it submit no marker layer.
* Geometry: `Quad q = hud_world_quad(neutral_, f.lens_tan, s_.quad_distance_m, s_.quad_distance_m /*same distance*/, 1.0f, neutral_yaw_)`
  i.e. exactly `world_quad` (pose-equal to the world quad; `hud-pose-equals-world-quad` becomes a marker log token).
  Optional `xr_marker_quad_distance_m` (default = world distance; clamp [5, 2000]) for a slightly nearer plane.
  The instrument HUD keeps `xr_hud_quad_distance_m` / `xr_hud_scale`; scale of the markers is forced to 1 (any other
  scale breaks registration; do not apply `xr_hud_scale`).
* `LayerKind::marker_quad`; `compose_layers` order: `[world_quad, markers?, projection, hud_quad]` or
  `[world_quad, projection, markers, hud]`. Recommended: world quad, then marker quad, then the cockpit projection,
  then the instrument HUD (markers behind the cockpit canopy frame, instruments on the glass); `LayerList::e`
  grows from 3 to 4 entries (`std::array<LayerEntry, 4>`), `ProjectionLayer` gets `markers`, `marker_pose`,
  `marker_width/height`; SOURCE_ALPHA premultiplied exactly as the HUD (never UNPREMULTIPLIED_ALPHA). The layer
  order and layer-count (`2u + use_world + use_hud + use_markers`) summary lines in `xr_stereo_output.cpp` (first-layer,
  first-hud-layer, hud-result) need an extra token each; every line must stay under the router's 480-byte cap.
* Counters / evidence: `marker_frames`, `marker_layers`, `marker_skipped`, `marker_invalid` and a
  `first-marker-layer` line with `marker-distance-m`, `marker-angular-deg` (must equal the world quad's 63.2x38.2),
  `marker-pose-equals-world-quad=1`. Key off: no 5th swapchain, no acquire/blit/layer, every line unchanged.
* Depth ordering: OpenXR layers have no depth test; submission order decides. With the marker quad coplanar to the
  world quad the order (world, markers) is unambiguous; no z-fighting concern across layers.

### 3.4 Config keys (cap = 24 keys AND `max_config_bytes = 2048`, `runtime_config.h`)
Two keys (producer, consumer) plus the optional distance key. Test-5 used 15 keys (~16 with png_once), so there is
room; check the 2048-byte total as well: the new names add about 90 bytes. Parsing follows the existing patterns:
`gpu_copy_step5_hud_markers` (bool, like `gpu_copy_step5_hud`: `runtime_config.cpp` line 87), `xr_hud_markers_layer`
(bool, same as `xr_hud_layer`), `xr_marker_quad_distance_m` (decimal like `xr_hud_quad_distance_m`). Latch in
`diagnostics_log.cpp` only together with their prerequisites (opt-in+logging; xr_stereo_output + xr_hud_layer) so
key-off lines remain byte-identical.

### 3.5 Tests (offline, follow existing fixtures)
* Producer (`gpu_copy_step1_offline_tests`): `step5-hud-markers-off` (key off: the instrument stream and
  all HUD fields op-for-op unchanged); `step5-hud-markers` (a synthetic frame with GREATER box + label draws
  among instrument draws: exact command stream: marker draws land in the marker target, instrument draws in the
  hud target, one clear per target, restore OM after each; counts; fake GPU rasterises into both; no extra
  allocations on the Draw path, refcounts unchanged); label heuristic table (B L B L B B L B, box with no
  label, text PSO after a non-box); `step5-hud-markers-refused` (a refused marker draw invalidates only the
  marker layer; a refused instrument draw invalidates only the HUD; both valid/invalid combinations in the frame
  fields); slot rotation, fence epoch, device loss as the existing `hud-faults`; config parsing of the new key
  (dup/invalid rejected) and the 24-key / 2048-byte limits.
* Consumer (`xr_stereo_output_offline`, new `--hud-markers` ctest like `--hud-placement`): layer list order and
  flags with every combination of world/markers/hud; marker quad pose equals the world quad pose bitwise (with
  yaw/neutral), size/direction match for lens sets and world distances (5, 40, 500, 2000 m); `xr_hud_scale` and the
  HUD distance do not affect the marker quad; frames without `hud_markers_valid` submit no marker layer and never reuse
  an older marker image; backend refusal path (`markers_used=false`); every log line fits 480 bytes; key off:
  identical journal and lines; a mutation (marker quad uses the HUD distance) must fail.
* Live check (null HMD): `png_once` with the 4th image; the marker target contains only the blue boxes and labels,
  `hud-on-black` loses them; `marker_draws` 3.0/frame in the run-11 scenario with `refused=0`.

### 3.6 Known limits of the split
* [G] The label/box heuristic is based on one scenario (3 friendlies). Enemy lock-on reticles, missile-lock tones and
  "TGT" text may use other PSOs; unclassified marker-like draws stay on the instrument layer at 0.8 m (offset).
* [E] Markers are drawn into a zero-cleared target: the translucent dark boxes (colour ~0.15-0.4 blue) have low alpha with the
  `max(a,max(rgb))` rule and look faint over the sky (visible in `hud-over-world.png`); the split does not improve
  that. A marker alpha fix (white-replay coverage) would apply to both targets.
* The marker layer is still a flat mono quad at one distance; targets at other depths still carry 0.1-2 deg of
  residual disparity at 40 m (500 m default makes it < 0.01 deg).
* The radar blips (Section 2) are not solved by the split. If H-A holds, a separate late-tail replay (EQUAL-stencil
  draws after the tone-map pass, into the instrument target, with the private stencil retained) is needed first,
  and it needs a design of its own: relax `hud_decide` for EQUAL stencil, reopen the region on return to the scene
  target (or process the segments between RT changes), and handle the a3050 passes (do not replay).
