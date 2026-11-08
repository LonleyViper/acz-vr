# MARKERS-360: the game's FRND/TGT boxes around the whole sphere (`mk360:1|2`)

Headset test 19 (D-362, item 3): the boxes render only inside the rectangle in front of the aircraft; the user wants them ALWAYS when looking around.
The game draws its boxes only for its own forward view (pre-projected sprites on the flat marker quad, 500 m out, 63.20 x 38.17 deg; D-358). The
earlier synthetic `tacboxes:1` boxes were rejected in test 18 (D-355): they doubled the game's, stayed 15 s after kills, had wrong colours.

Offline work only: nothing here was run in the game or a headset. Tags: **[E]** evidence (log/code cited), **[I]** inference, **[G]** guess.

## 1. Switch

Inside the existing `xr_terrain_stage` value (no new config key; the installed config stays at its 32-key cap), independent of `tac:1`:

| option | meaning |
| --- | --- |
| `mk360:1` | ONE set: every game box sprite the producer READ completely is hidden and redrawn by us at its own direction; contacts outside the game window that the game boxed before are drawn by us at their true 3D position |
| `mk360:2` | the game's own boxes stay in the window (flat quad, unchanged); ours only for contacts outside the game window (fallback if `mk360:1` looks wrong) |
| `mk360:3` | NBOX (section 12, user-preferred): the GAME'S OWN box + label pixels (cut from the marker image, cached per contact) drawn per eye at each contact's direction, in and outside the window; nothing hidden by the producer; the flat quad keeps everything not stamped |
| `mk360:0` / absent | off (default): byte-identical behaviour, `producer_flags_for()` = 0 unless `tac:1` |
| `mk360u:1` | diagnostic only: moving contacts that never had a game box, outside the window, as dim grey unlabeled boxes |

`tacself` / `tacclu` (tac.md) also set the own-aircraft radius (80 u) and the contact cluster distance (60 u) for mk360. With `mk360` set, `tacboxes:1` is
ignored (never two sets of ours), and `tac:1` keeps only its tracers/radar. Needs the keys already in the day config: `gpu_copy_step5_world_objects`,
`gpu_copy_step5_hud`, `gpu_copy_step5_hud_markers`, `xr_hud_markers_layer`. Example: day terrain value + `;mk360:1`.

## 2. Sources evaluated

| source | what it gives | used |
| --- | --- | --- |
| game box sprites (`gpu_copy_step5_hud_markers` box/label draws, read by the tac producer: ps2_v GS x,y + vertex colour) | exact screen position, colour, label glyph count (4 = FRND, 3 = TGT), size: **only in the game window** | identity + in-window direction |
| replayed world objects (`gpu_copy_step5_world_objects` DrawACM_v records, +256 translation) | true 3D position of each drawn object, **also outside the game's view** [E, below] | 3D position outside the window |
| the frame's A (record +0, world -> GS screen) | the game camera the boxes were drawn with: sprite ray, window test, camera centre | yes |
| E6 / terrain camera | the head-free camera position (self exclusion, logs) | yes |
| game radar contacts (`gpu_copy_step5_hud_radar` EQUAL draws, replayed since D-357) | bearing + colour of every contact, 360 deg, heading-up | **not read** (section 5) |

Evidence that the game submits object draws outside its forward view [E, test-18 `pre-run-P` xr-stereo.log `tac-blips`]: tac listed unknown-team blips only for
tracks matched in the newest frame (`radar_blips`: `team_unknown && !seen_now` skipped); 58 of those 91 samples were at |bearing| > 35 deg, 31 at > 90 deg
(behind), 4 distinct contacts, ranges up to 67,206 u. So the game does not cull those draws to its window on the CPU (the same was found for smoke trails, D-361).

Why tac's identity was wrong [E, same run, `continuous-tac`]: 6,123 sprite->object matches out of ~69,000 box clusters (avg-markers 4.78 x 14,510 frames: ~9 %),
and tracks kept the colour of ANY one match for 15 s. The game's box colours seen there: yellow (1,0.85,0) 85, green (0.29,1,0.22) 55, red 23, blue 22 blip samples
(the game's own colours; mk360 copies them, it never maps colour -> team). Why so few matched: section 9 (live run S1: the contacts ARE projected 8..16
GS from every box, but each box is read as small pieces and the gate was relative to the piece size; fixed by MATCH-2).

## 3. Rules (marker360.h)

Per consumed producer frame, from `StereoFrame::tac` (the tac producer facts, collected with `mk360` alone):

1. Contacts = the frame's replayed world draws minus those within `tacself` of the game camera, clustered within `tacclu`; associated to tracks by greedy
   nearest pairs in a velocity-aware gate. **No coasting**: a track without a draw for > 0.25 s is dropped; a track without a draw THIS frame is not drawn.
2. In-window test with the frame's A (GS window 2048 +- 256 / 2048 +- 224, in front of the camera).
3. Sprites -> tracks (MATCH-2, section 9): sprites grouped into boxes; the producer's own per-draw match (frame time) first, else the nearest
   offset-corrected projected track (now or previous frame) within max(box half x 1.5 + 4, 20) GS; score quartered for the track that already has that
   colour; greedy one box group to one track.
4. Identity = the matched sprite's colour, glyph count and size, taken after 2 consecutive same-colour matches (a one-frame mismatch never sticks), refreshed every
   matched frame. Dropped when the track is inside 92 % of the window, the frame's box draws were all read, and no box matched it for >= 4 frames and >= 60 ms
   (the game stopped reporting it: kill, target change).
5. Drawn (eye `post` slot, after the cockpit, like the game's marker layer):
   * `mk360:1`: every suppressed sprite at the ray of its GS centre from A's camera centre, at its matched contact's distance (else 32,500 u, direction exact), in
     the sprite's own colour/label/size; plus every identified track seen now, outside the window, not drawn via a sprite, at its 3D position.
   * `mk360:2`: only the identified tracks outside the window.
   * Never: a track whose game box is drawn (unsuppressed) this frame; an in-window track without a box this frame; a track without a draw this frame; a label the
     game did not draw (glyph count other than 3/4 = no label).
6. Producer (one hunk, `step5TacMarker`): with `kProducerHideAll` (mk360:1) a box draw is not replayed when all its clusters were kept (pure rule
   `suppress_box_draw`, tactical_producer.h); its label draw follows. An unreadable or over-cap draw stays the game's, and the consumer draws none of its sprites.

Size: GS half extents x (0.6152 / 256, 0.3459 / 224) = tan; a 15-unit box = 0.0180 tan, the step62 shot's 0.0185 [E].

## 4. Unchanged / key off

`mk360` absent: `extract_options` returns the same rest string, `describe` the same line, `producer_flags_for` the same flags (tests), `tac_frame` is not reached
unless `tac:1`. With `tac:1` alone the tracker/tracers/radar are as before; the only tac change: a tacboxes draw whose clusters hit the frame cap is no longer
suppressed (it used to vanish), and `WorldMarker` carries the box half size (default = the old constant).

## 5. Limits (honest)

* Identity needs the contact to have been boxed IN the game window once, and the box matched to its world draws. Contacts never seen in the window are not drawn
  (`mk360u:1` grey for tests). The ~9 % match rate of test 18 [E] means many out-of-window boxes may be missing until section 6 explains it.
* Outside the window the identity is the last in-window one: a colour the game changes while the contact is out of view (e.g. the lock colour) is not followed.
* A killed contact OUTSIDE the window disappears only when its world draws stop (> 0.25 s); debris clustered at the same place may keep the box until then [I].
* Not implemented: radar-derived identity (read the replayed EQUAL contact draws' vertices = bearing + colour for all contacts; would also give a "reported" signal
  outside the window). Ground hexagons are redrawn as boxes (our stroke shapes: box + FRND/TGT only).
* No depth test: our boxes draw over the cockpit like the game's marker layer did. Positions use display-time object positions with `interp:1`, the sprite rays use
  frame-time A (sub-pixel at km range [I]).

## 6. Logs (`xr-stereo.log`, every summary period)

`event=summary-mk360;...;tracks=N(seen:,identified:,in-window:,identified-outside:);sprites=(suppressed:,matched:,producer:,consumer:);
sprite-to-nearest-contact-gs=lt8:,lt16:,lt32:,lt64:,ge64:,none:;identity=new:,recolour:,cleared-in-view:;lost=(identified:);created=;self-draws=;box-facts-unsafe=;
drawn=sprite:,outside:,unknown:;avg-drawn=;in-window-identified-without-box=;game-boxes=` (cumulative) and `event=mk360-contacts;contacts=id:win|out[+box]:r<range>:
b<bearing deg>:e<elev deg>:v<speed>:id<r,g,b>|none:g<glyphs>` (up to 8 seen contacts; bearing from the game camera's forward). Producer: the existing
`continuous-tac` line (`box-draws`, `suppressed=boxes:`, `unreadable=`).

## 7. Tests

`marker360_offline` (tests/acz-vr/marker360_offline_tests.cpp, links tactical.cpp only): options/flags (key off unchanged, mk360 replaces tacboxes, bad values);
`suppress_box_draw` table; A geometry (camera centre to 0.05 u, sprite ray = the point's direction to 1e-4, window edges at 30/40 deg, forward ray); a blue FRND
wingman identified in the window and boxed at its exact position with the nose turned 90/180/270 deg, drawn in the centre of an eye looking at it, not drawn
for an eye looking along the nose; gone the first frame its draws stop, dropped after 0.25 s; a red TGT killed in view (wreck drawn, no box) loses its identity at
frame 4 / 60 ms and is not drawn when behind; unread draws never doubled and never counted as misses; a one-frame red box on a blue contact does not recolour
it, two yellow frames do; never-boxed contacts not drawn (mk360u:1 grey for the moving one only); self draws excluded; two contacts 0.6 deg apart over a 360 deg
nose sweep: always exactly 2 boxes in mode 1, the out-of-window count in mode 2, identities never swapped; edge margin; log line formats.
Agent build (private Release dir, MSVC, -j2, one target at a time): `marker360_offline` 6373 checks pass; `tactical_offline` (also compiles the
`xr_stereo_output.cpp` hooks) 352 checks pass; `gpu_copy_step1_offline_tests` (compiles the producer hunk) fixtures `step5-tac`, `step5-hud-markers-off`,
`step5-hud-markers` pass. Full build / ctest: not run (the lead's integration build).

## 9. MATCH-2: why only ~1-9 % of the boxes matched, and the matcher fix

Evidence [E, `D:\acz-evidence-archive\step83\runS1-logs` (mk360:1, mission 02), shots `step94\runS1-archive\shots`]:

* Final `summary-mk360`: `sprites=44246(suppressed:44246,matched:604,producer:602,consumer:2)`, `sprite-to-nearest-contact-gs=lt8:845,lt16:41339,
  lt32:125,lt64:649,ge64:1288`, `identity=new:6,cleared-in-view:5`, final `identified-outside:0`. 1.4 % matched although **93 % of the sprites had a
  projected contact 8..16 GS units away**. S1b: 23146 sprites, 316 matched, lt16 20825.
* Per 5-s window T45..T70: 300 frames, 1800 sprites, all `lt16`, matched 0; 3 contacts in window. Producer: 4 box draws / 3 label draws per frame. The
  T55 game-window shot shows exactly three blue FRND boxes, and they sit on the directions `mk360-contacts t-ms=55023` gives (37 b-7.4 e11.5, 38 b-19.9 e11.2,
  39 b0.6 e14.7: shot box centres b-7.4/-20.0/+0.6 deg, elevation within 0.3-0.8 deg, pitch not known exactly). So camera (A), lens (63.2 x 38.2 deg) and
  coordinate system agree to ~1 deg; the contacts ARE the boxed wingmen.
* **6 sprites per frame for 3 boxes** = the producer turns each game box into TWO clusters. Matches happened only in windows whose nearest distance fell
  below 8 (T40: lt8 205 -> 316 matched; T105: lt8 64 -> 97; T135-145: lt8 576 -> 191): the effective gate (half x 1.5 + 4) was ~8 GS, i.e. the sprites'
  half extents are ~2-3 GS, NOT the 7.5 of a whole 15-GS box. A sprite centre 8..16 GS from the contact with a ~7-GS gate never matches.
* Cause [I, strong]: the gate is relative to the CLUSTER's size and each cluster is a box PIECE (or a small part offset from the box centre: corner
  pieces at +-7.5,+-7.5 give exactly 10.6 = lt16); the residual offset is constant over 80 s of steady flight (not a one-frame lag: that would vary with the
  turn rate; not a lens error: that would grow with the distance from the window centre). Which exact piece shape is not logged (no per-sprite dump); the
  new `summary-mk360-match` line settles it (sprite half sizes, mean signed residual).
* Also found: `feature_log::line_cap` = 480 B cuts `summary-mk360` after `avg-drawn=` (S1: the line ends `in-window-identi`): the last two fields were never
  in any log. They are repeated on the new line.

Fix (`marker360.h` `update()`, step 4; xr_stereo_output.cpp: one emit line):

1. **Box groups**: same-colour sprites of one frame whose rectangles are <= 14 GS apart are one game box while the union stays <= 14 GS half size
   (two neighbouring boxes 30 GS apart never merge, test). The group rectangle is the box (centre, size, max glyph count). In mode 1 a fully hidden group
   is drawn ONCE at its centre and size (before: every piece as a small box of its own).
2. **Gate** = max(group half x 1.5 + 4, 20 GS) around the **offset-corrected projection**: each track learns its GS offset (box centre - projection,
   EMA 0.3 of its matched residuals, <= 40 GS); tracks without one use the global offset (EMA 0.1 of unambiguous residuals: nearest <= 40 GS and the
   second contact >= 2x + 8 farther; applied after 6 samples). Anchoring differences (a box above the object, per class aircraft vs ground hex) are absorbed.
3. **Previous-frame prediction**: the track's previous-update position is projected too; the closer one counts (`prev-frame-closer`).
4. Unchanged: the producer's own per-draw match first, colour continuity, greedy one box <-> one contact, identity hysteresis/misses, no coasting.

New log line (every summary period): `event=summary-mk360-match;t-ms=;sprites=;matched=;groups=N(multi:);sprite-half-gs=lt2:,lt4:,lt8:,ge8:;
nearest-mean-gs=dx,dy(n:);offset-gs=gx,gy(n:);matched-beyond-raw-gate=;prev-frame-closer=;in-window-identified-without-box=`.

Tests (`marker360_offline`, `test_s1_replay`, `test_group_limits`): the S1 T55 geometry (the three logged contacts, 300 frames) with each box as two corner
pieces (half 2, +-7.5) and as one small piece 11 GS above the contact: the OLD rule (Params with MATCH-2 off) reproduces the log exactly (every sprite
lt16, 0 matched, no identity); MATCH-2 matches every box from frame 1, 3 FRND identities, one box per contact (whole-box size for the pieces), the learned
global offset = (0, -11) +-0.5, all three kept as FRND at their 3D positions after a 90 deg turn; two same-colour boxes 30 GS apart stay two.

## 8. Live check list (null driver, mission 02; the lead schedules it)

Config: day install + `;mk360:1` appended to the terrain value (run 1), then `;mk360:2` (run 2, short). Keep `tac` off. Fly the step40 route; after the first
wingman FRND boxes are visible in the game window, yaw/roll so the wingmen and targets leave the window; fire at one target in view and one after it left.

1. `xr-stereo;event=tac-armed;...;mk360=1;producer-flags=5` once (5 = On|HideAll; mode 2: 1).
2. Producer `continuous-tac`: `box-draws` grows, `unreadable=boxes:0`, `suppressed=boxes:` ~ `box-draws` (mode 1; mode 2: 0). If `unreadable` > 0: the reason.
3. `summary-mk360`: `sprites` ~ producer clusters, `suppressed` ~ `sprites` (mode 1), `drawn=sprite:` ~ `suppressed`; `identified` > 0 once wingmen were boxed;
   `drawn=outside:` > 0 while a boxed contact is out of the window; `cleared-in-view` grows with in-view kills; `in-window-identified-without-box` small.
4. **Match diagnosis** (MATCH-2, section 9): `summary-mk360-match`: `matched` ~ `sprites`, `groups=N(multi:M)` (M ~ N = each box read as pieces),
   `sprite-half-gs` (lt4 dominant = pieces), `nearest-mean-gs` / `offset-gs` (a constant offset = anchoring), `in-window-identified-without-box` small;
   `summary-mk360`: `identified-outside` > 0 once wingmen leave the window. Old reading: `sprite-to-nearest-contact-gs`: mostly `lt8`/`lt16` = contacts found; mostly `ge64`/`none` = the boxed contacts are not among the replayed
   draws (check `continuous-status-world before-frame`) or A/sprite coordinates disagree: report the histogram + one `mk360-contacts` line + a game-window shot.
5. PNG burst (`xr_png_once`) with the head turned ~90 deg toward a wingman outside the game window: one blue FRND box on it; in the window: boxes where the game
   window shows them, same colours (blue FRND, red TGT, yellow/green as the game), NO second flat-quad box (mode 1).
6. After a kill in view: the box is gone in the same 5 s window (`cleared-in-view` +1 or the track `lost`); after a kill out of view: gone within ~0.3 s of the
   wreck's draws stopping (contacts line no longer lists it).
7. Headset (user): exactly ONE set of boxes everywhere; colours = the game's; none lingering after kills; the edge of the game window shows no jump or double.
8. Stop and report if: boxes doubled in the window (mode 1: `suppressed` << `sprites` with our sprite boxes also drawn), boxes at wrong directions (sprite ray
   vs game shot), or `drawn=outside:0` with `identified-outside` > 0.

## 10. BOX-WARP: "TGT boxes look warped" (step103 N1, step104l L4)

Two causes, both ours (test-18 without mk360 showed the game's own boxes on the flat markers quad without complaint):

1. **Tangent-plane quad, rectilinear projection.** `build_marker` drew the box as a quad in the plane perpendicular to the eye->contact ray (axes
   `f x up`) and projected it with the eye's rectilinear matrix. Away from the view axis (canopy top, the sides) such a plane projects as a sheared /
   rotated parallelogram growing with the off-axis angle, and near the zenith `f x up` flips fast (the slanted blue / yellow / green boxes of step103 N1).
   Now `marker_screen_frame` measures the local NDC scale per tan along the eye's own right (x only) and up (y only) axes (`EyeView::right/fwd`, from
   the eye pose; fallback `f x up` when absent) and `ndc_seg` draws every stroke (box sides, FRND/TGT glyphs) AXIS-ALIGNED in the eye's NDC around the
   contact's projected centre: no roll, no shear, whatever the aircraft bank (the box follows the head, not the aircraft).
2. **Anamorphic size.** The game's window (512 x 448 GS) is stretched over the lens (tan 0.6152 x 0.3459): `kTanPerGsX / kTanPerGsY` = 1.56, so
   `hw * kTanPerGsX` x `hh * kTanPerGsY` turned the game's square 15 x 15 box into a 1.56:1 flat rectangle (the wide yellow rectangles of L4).
   `box_half_tan` now uses one angular scale (`kTanPerGsBox` = geometric mean, 0.001926) and the game's own aspect clamped to 0.8..1.25 (a
   mis-matched piece union can no longer give a wide flat box); a 15 GS box = 0.0144 tan half size, square.

Test: `marker360_offline` `test_box_not_warped` (rolls 0 / +-0.5 / 1.2 rad x five off-axis directions: every box side is an axis-aligned rectangle in
the eye's NDC, size = tan x local scale, square on axis; ratio clamp). Not checked live: the headset look is the next live run.

## 11. Lock and miss effects (LOCK-FX)

Question: does mk360 keep the game's lock-on / lock-lost / MISS look of a target box? Offline analysis only; **no run in the archive contains a lock or a missile
launch** [E: every `autopilot*.log` of step104k/step104l/step105/step83 ends `missile=0`; the L4 'acquired at 16 km' run never fired and no lock shot exists], so
what the game draws for a lock is **unknown [G]** (`J-hud-split-analysis.md` 3.6 already lists lock reticles / lock text as unverified).

What mk360 reads: box = EVERY depth-GREATER draw of the HUD tail (the only signature, `hud_classify`), label = the text-route indexed draw right after a box;
everything else (range / closure numbers, `MSSL` / `MISS` HUD text, the alert box) is an instrument draw and is never touched. The game's per-box facts
used are colour (cluster mean, normalised), rectangle (clusters merged within 14 GS), glyph count of the nearest label cluster.

| behaviour | state | why |
| --- | --- | --- |
| colour change of an in-window box (lock colour, flash, 1 frame) | preserved | mode 1 draws each hidden sprite with THIS frame's `s.rgb`; hysteresis (2 frames) only gates the stored identity (test `test_lock_fx`) |
| size change (tightening lock box) | preserved | this frame's group rectangle -> `box_half_tan` (one angular scale; aspect clamped 0.8..1.25; 0.006..0.08 tan) |
| blinking (box drawn on alternate frames) in the window | preserved | drawn exactly on the frames the game draws it; a blink does not drop identity any more (miss rule 4 fr / 60 ms -> 9 fr / 140 ms) |
| identity after a long blink (> 140 ms off) or a lost lock | dropped by design | indistinguishable from a kill; the box is redrawn when the game draws it again (new identity after 2 frames) |
| lock / miss text after a box, other than FRND/TGT (e.g. `LOCK`, `MISS`, ranges, 5-6 glyph names) | was lost, now kept | the producer hid every label draw after a hidden box; now only FRND (4 glyphs, blue) / TGT (3 glyphs) are hidden (`label_draw_redrawable`); other text stays the game's draw on the flat marker layer (window position, not world-anchored) |
| a 4-glyph non-blue label (`MISS`-like) | was mislabelled FRND, now no label | `label_of` needs the blue colour for FRND |
| lock reticle / diamond / bracket that is depth-GREATER | lost (shape) [G] | hidden and redrawn as a plain rectangle in its colour/size; only if it is GREATER-tested (unknown); a reticle wider than 14 GS half may draw as several small boxes (`sprite-half-gs ge8`, `groups multi` show it live) |
| lock reticle not GREATER-tested | preserved | instrument layer, untouched |
| colour/size/blink of a contact OUTSIDE the window | not followed | identity = last in-window box; world draws only. Acceptable: a lock needs the target in the reticle (inside the window); a lock held after the target leaves the window keeps the last colour. Following it needs the radar contact draws (not read, section 5) |
| MISS text / missile-lock tone | preserved | HUD text instrument draws and audio, not touched |

Live check (the lead schedules it; null driver, mission 02, `mk360:1`, `xr_png_once` bursts + mirror): approach a target, hold it in the reticle until the lock
tone and box change, fire a missile, keep it in the window through the hit AND once through a miss (turn away so the seeker loses it). Capture the game-window
shot (flat HUD) and the mirror frame every 0.5 s from 3 s before the lock to 5 s after the hit/miss. Compare per frame: box colour, size, blink rhythm, label
text, extra reticle shapes, MISS / range text present and not doubled. Log: `summary-mk360-match` (`sprite-half-gs`, `groups(multi)`), `continuous-tac`
`suppressed=labels:` vs `label-draws` (the difference now = non-FRND/TGT text kept), `mk360-contacts` glyph counts (g != 0/3/4 during the lock = lock text).
Stop and report if a lock shape shows as several boxes, text is doubled (ours and the game's), or the box colour lags the game's by > 1 frame.

Code: `label_text_redrawable` / `label_draw_redrawable` (tactical_producer.h), `step5TacMarker` label branch, `marker360.h` (`label_of`, `miss_frames` 9 / `miss_s` 0.14).

## 12. NBOX (`mk360:3`): the game's OWN box + label pixels, per eye, all around

User (headset test 2026-10-28, items 7/8/13/14; `2026-10-28-user-1/-2.webp`, original-game references `2026-10-28-explosion-targets-original-reference.webp`,
`2026-10-28-enemy-aircraft-box-original-reference.webp`): our stroke boxes (mode 1) doubled in the window and an enemy aircraft carried "FRND"; the user wants
the game's own look everywhere: green hexagons for ground targets with red TGT labels, a red lock reticle with its range number, a red square + diamond for
enemy aircraft with the GREEN type label ("F-4E") upper right and the green range below. Those shapes, colours and texts exist only as the game's pixels:
mode 3 replays THOSE pixels instead of synthesizing anything. Offline work only (no game, no headset).

### 12.1 How

* **Producer**: `producer_flags_for(mk360:3)` = `kProducerOn` only. No box or label draw is ever hidden: every marker draw is replayed into the marker image
  exactly as with `mk360` off (the tac facts are still collected: box clusters, and now the label rectangle `lx0..ly1` + the label's own colour `lrgb`).
* **Consumer plan** (`marker360.h` `plan_stamps`, after `update`): every MATCH-2 box group is a **stamp** = the marker-image pixel rectangle around the box
  and its associated label clusters (any glyph count: type names, ranges), + 2 GS margin, clipped to one atlas cell (box centre +- 64 x +- 32 GS). Stamps that
  overlap (boxes close together, the two "TGT" hexagons of the reference) are cut at the integer midline between their box centres: **no marker-image pixel
  is in two stamps**. Each live stamp is (a) copied into an atlas cell, (b) cleared (transparent) in the flat marker quad's swapchain image, (c) drawn in
  both eyes at the ray of its box centre through A, at its contact's distance (else 32,500 u), screen-aligned, at the flat quad's own pixel scale (the
  game's size and aspect). Exactly once: pixels in no stamp (unread box draws, text out of reach, other HUD) stay on the flat quad.
* **Cache**: a contact matched to a COMPLETE stamp (its box >= 1 GS inside the window: the game clips at the edge) owns its cell (24 cells, 6 x 4; LRU
  eviction of the contact out of view longest). A window-clipped box never overwrites a complete cached stamp (`edge-kept`). Outside the window an
  identified contact whose world draws the game submits THIS frame is drawn from its cell at its 3D position + its learned box offset. Gone when the game
  stops reporting it (the mode-1 rules: world draws stop > 0.25 s, or no box in view for 9 frames / 140 ms). Never drawn from a cell the backend did not
  fill (`stamps_not_copied`, image-size change).
* **Lifetime invariants** (Codex J-203, review-w5-1): (1) a cell is sampled only while its contact's `cell_ok` holds, and `cell_ok` is set only by a
  COMPLETE live stamp of a fresh plan; (2) the plan's cell writes are provisional until the frame is accepted: copies not recorded ->
  `stamps_not_copied`; copies recorded but the frame REJECTED after GPU completion because the producer rewrote its marker image after the pre-check
  (`fresh_rewritten_post`: the copies read the rewritten pixels) -> `stamps_rejected` = every cell this plan wrote is forgotten and the plan is emptied
  (never re-composited); cells the plan did not write keep their earlier accepted stamps; (3) every tac EyeView (fresh frame AND DECOUPLE
  re-composite) is built by ONE helper (`tac_eye_views` -> `tac::set_eye_frame`: eye + right/up/fwd), so a stamp's screen size at a given pose does not
  depend on the path; `build_stamps` draws nothing for an eye without its frame (a missing frame is a visible refusal, never a ~1.4x width jump
  off-axis, the J-203 finding 1 trigger).
* **Backend** (`xr_stereo_d3d12.cpp`): atlas = DEFAULT texture of the marker image's format (copy dest + SRV, created zeroed = transparent; 2560x1440:
  3840 x 824 x 8 B = 25 MB; budget 64 MB), stamp PSO (`xr_tactical.hlsl` `vs_stamp/ps_stamp`: the marker blit's HUD conversion, premultiplied, ONE /
  INV_SRC_ALPHA), SRV after the model table (heap + 1). Per fresh job: marker image -> PSR|COPY_SOURCE, copies in a PSR -> COPY_DEST -> PSR bracket of
  the atlas, clears right after the marker blit, the eye quads after the model cockpit (overlay slots `eye0/1_stamp`). A re-composite (DECOUPLE) draws the
  stamps again from the atlas, no copies/clears. If a fresh plan's copies cannot be recorded (marker image unreadable / other size, no atlas/PSO, overlay
  upload failed) NOTHING of the stamps runs (no clear: the flat quad keeps the game's boxes) and the consumer forgets those cells.
* **Labels**: never guessed. The label pixels are the game's own text (TGT, APC, AA GUN, F-4E, ranges) in the game's colour. Label association stays
  geometric (nearest box within 30 GS), not colour: the reference shows a red box with a green label. In the mode-1 fallback FRND now needs a BLUE LABEL
  (`lrgb`), never the box colour (a green-labelled enemy is never FRND).
* **Codex J-201 P1s** (review-w4-2): mode 3 hides nothing, so neither trigger exists there. Fixed for the mode-1 fallback too (`label_suppress_ok`): an
  unreadable label stays the game's draw; a read label is hidden only when every cluster is FRND/TGT text joined one-to-one to a hidden box of the preceding
  box draw (out-of-reach, two texts on one box, a dropped cluster, a type name: kept).

Switch: `;mk360:3` in the terrain value (no new config key). Needs `xr_hud_markers_layer` (+ `gpu_copy_step5_hud_markers`): without the marker image there
are no live stamps (`no-image`). Fallback: `mk360:1` (synthetic boxes) or `mk360:2`.

### 12.2 Limits (honest)

* Only text in the label draw that follows a box (the text-route draw) joins a stamp. If the lock range ("4717"/"3138") or the type name is an INSTRUMENT
  text draw, it stays on the flat quad at the window position (in view only) [G: unknown which]. Live check item 5 settles it.
* Outside the window the stamp is the last complete in-window picture (a lock colour / range number change out of view is not followed; the range number
  freezes). Label clusters further than 64 GS right / 32 GS up/down of the box centre are cut off.
* A lock reticle wider than 14 GS half may split into several box groups; their stamps are cut between the centres (still the game's pixels, no double).
  Superseded by section 13: same-colour groups of one contact are joined into its stamp (satellites), never cut against each other.
* Overlapping boxes at different distances: each stamp at its own contact's distance (stereo), cut at the midline; a box overlapping another one's label
  loses that part of the label.
* No depth test (as the marker layer and mode 1). Not built as a DLL, not run.

### 12.3 Logs

`event=summary-mk360-nbox;frames=;live=;cached=;copies=;cuts=;no-cell=;empty=;no-image=;evicted=;edge-kept=;invalidated=;no-atlas=;rejected=;cells=N(contacts:M)/24;
atlas=WxH(cell:wxh,img:wxh);last=draws(copies:)` and `event=summary-mk360-nbox-backend;used=;refused=;copies=;clears=` (what was recorded) plus `refused-view=;refused-atlas=;refused-copy=;refused-pipeline=` (step111 live: refused 32% was ALL `refused-view`: cached stamps whose boxes are outside both eye views = empty overlay, benign; `invalidated=0` shows no copy was ever refused; only the last three are defects), plus the
unchanged `summary-mk360` (`game-boxes=native-stamps`), `summary-mk360-match`, `mk360-contacts`.

### 12.4 Tests

`marker360_offline` (agent build, MSVC, private D:\b-nbox): 12,260 checks pass; new: `test_nbox_geometry_and_labels` (atlas sizes, GS<->pixel, red box +
green F-4E/range labels joined and inside the stamp, FRND only from a blue label), `test_nbox_wingman` (one live stamp per frame = copy = clear, box + label,
at the contact's range, its own cell; outside the window the cached stamp at the 3D position, eye quad at the game's scale, uv at texel centres; nothing once
the draws stop; log lines), `test_nbox_exactly_once_and_edge` (three boxes close together: disjoint stamps, each keeps its centre, distinct cells; a clipped
box never overwrites a complete cache), `test_nbox_failures_and_eviction` (copies not recorded -> not drawn; no marker image -> no live stamp, cached kept;
new image size forgets cells; 14 + 14 contacts -> 4 LRU evictions, 10 cached kept), `test_label_suppress_pairing` (both Codex J-201 triggers + the
one-replacement rules). J-203 (private D:-stm, 12,399 checks): `test_nbox_fresh_equals_recomposite` (off-axis 0/20/35/-30 deg: the fresh and
re-composite EyeViews give identical stamp vertices; the old eye/up-only EyeView measures another width, same height, and now draws nothing),
`test_nbox_rejected_after_completion` (frame rewritten after the pre-check: its written cell forgotten, an unwritten contact's cell kept, the plan
emptied; the next accepted frame outside the window never samples the rejected cell; a later complete box re-caches; `rejected=1`). `tactical_offline` 352 pass (compiles `xr_stereo_output.cpp`). Compiled only: `xr_stereo_d3d12.cpp`, `d3d12_hook.cpp` (producer
hooks); `xr_tactical.hlsl` vs/ps_stamp with fxc /WX. Not run: full build, ctest, `gpu_copy_step1_offline_tests` fixtures.

### 12.5 Live check list (null driver, then the user's headset; the lead schedules it)

Config: day install + `;mk360:3` appended to the terrain value (keep `tac` off). Mission 02 (wingmen + ground TGTs) and a mission with enemy aircraft.

1. `tac-config` shows `mk360=3`, producer flags 1; `continuous-tac` `suppressed=boxes:0` and `labels:0` (nothing hidden).
2. `summary-mk360-nbox`: `live` grows with boxes in view, `no-cell=0`, `no-atlas=0`, `atlas=3840x824(cell:640x206,img:2560x1440)`;
   `summary-mk360-nbox-backend`: `refused` ~0, `copies`/`clears` ~ the plan's `copies`. `invalidated` > 0 only after refusals.
3. PNG burst (`xr_png_once`) + mirror shot looking FORWARD: every game box exactly once in the game's shape/colour (green hexagons, red TGT text, red square +
   diamond, lock reticle); label text = the game's (F-4E / AA GUN / TGT; FRND only on blue wingmen); NO flat-quad copy beside it; nothing missing vs the
   game-window shot (box by box).
4. Head turned ~90 / 180 deg toward a wingman / enemy outside the game window: its native box + label there (`cached` grows), on the contact.
5. Record whether the range number and the type name travel WITH the box outside the window (label draw) or stay at the window position (instrument text).
6. Lock + fire (section 11 procedure): reticle + range in the window follow the game frame by frame; kill in view: stamp gone with the game's box; out of view:
   gone within ~0.3 s of the wreck draws stopping.
7. Two TGT hexagons close together: no doubled edge (`cuts` > 0), no visible gap beyond the cut line.
8. Window edge: a contact sliding out leaves no half box frozen outside (`edge-kept` grows while clipped).
9. Stop and report if: boxes doubled (flat quad + stamp: check `clears`), black/opaque squares (atlas format/alpha), stamps at wrong directions, perf
   (XR ms) or memory regress (atlas 25 MB VRAM).
Tests: `marker360_offline` `test_lock_fx` + adjusted `test_kill_in_view` (12094 checks pass); `gpu_copy_step1_offline_tests` builds (hook compiles; `step5-tac`, `step5-hud-markers(-off)` run).

## 13. LOCK GROUPING + ROLL: boxes outside the window, rolled boxes, the lock diamond (headset test 2026-10-29 item 3)

User (day install step123, `mk360:3`; screenshot `docs/headset-tests/2026-10-29-user-enemy-air-box-lock-diamond.png`): (a) TGT boxes do not render outside the
forward rectangle, or only bits do; (b) rolling the aircraft misaligns the box edges; (c) the lock DIAMOND (a separate red rotated square lower left of the
red target box, with "5" and the green range "3450"; "F-4E" upper right) is drawn in the forward HUD, not with the box. Offline work only (no game, no headset).

### 13.1 Evidence (archived `D:\acz-evidence-archive\headset-2026-10-29-session\xr-stereo.log`, last summary t-ms 1561446)

`summary-mk360-nbox`: frames 69958, live 148312, cached 469909, **cuts 16171 (10.9 % of the live stamps)**, empty 1445, edge-kept 813, no-cell 0,
invalidated 0, no-atlas 0, rejected 0; backend refused 10687 = all `refused-view` (nothing in view; benign). So cached stamps WERE drawn outside the window
(the Batch X null result holds) and nothing failed in the backend: the defect is WHAT the stamps held. `no-image` 35728 = whole summary periods with
`marker-skipped=300` (no marker image: menus / pauses), not a stamp fault. `mk360-contacts` shows the locked red contact with `g14` (box + diamond text).

### 13.2 Root causes (code, `marker360.h` before this change)

1. **The lock diamond is its own box group.** It is a depth-GREATER draw like the box (it reached the marker image and was stamped: the user saw it at the
   window position [inference: no per-draw evidence of its classification]), ~25 GS from the box centre: MATCH-2 never merges it (union half 16 GS >
   `group_half` 14) and it matches no contact (outside the 20 GS gate). `plan_stamps` made it a SECOND stamp, **cut at the midline** against the box's
   (the range "3450" and "5" went with it), track-less: drawn at `far_units` (32,500 u = the flat quad's 500 m) on its ray = visually "in the HUD",
   never cached, so absent outside the window. (c) + (a).
2. **Cut stamps were cached as complete.** `complete` only tested the box against the window edge; a stamp cut by a neighbour (diamond, a second
   contact) replaced the contact's whole cached picture: outside the window "only bits". (a)
3. **Stamps were axis-aligned in the eye's NDC.** `build_stamps` scaled the GS offsets with `marker_screen_frame` (x along the camera right, y along
   the camera up, cross terms dropped). The game laid the pixels out along ITS screen axes at that frame: a re-composite (DECOUPLE) at a later camera
   roll, a head rolled in the cockpit, and pieces of one picture anchored at different depths (box at the contact, diamond at 500 m) all broke the
   picture apart while rolling. (b)

### 13.3 Fix

* **Satellites** (`kSatReachGsX/Y` 64 / 48 GS): an unmatched box group of the same colour within reach of a contact's matched group, onto which no other
  seen contact projects (that would be ITS box), joins that contact. The contact's stamp = the union of its box(es) + every label, in ONE cell window
  (128 x 64 GS) that is shifted (no longer centred) to hold all boxes and, when they fit, all labels: one picture, one anchor (the matched box), the
  contact's distance, never cut against itself, cleared from the flat quad exactly once.
* **Second stamp**: a satellite too far for one window (e.g. a reticle 46 GS below) gets a second stamp of the same contact (same anchor / distance,
  disjoint from the box's), cached in the contact's second cell (`Track::cell2`), drawn with it outside the window and released when a new cached
  picture has no satellite.
* **Cache quality** (`c_q`: 2 whole, 1 cut by another contact, 0 window-clipped): a stamp replaces the cached picture only when it is at least as good,
  or the cached one is older than `kStampRecacheS` (2 s). Cut stamps no longer replace whole ones (`cut-kept`).
* **Roll** (`StampDraw::sr/su`, `stamp_frame`): every stamp carries the world directions of the game screen's right/up at its anchor: live = that frame's
  A at the box centre (`screen_axes`), cached = this frame's camera axes carried to the contact (`contact_axes`: up = the camera up perpendicular to
  the ray, right with the camera's handedness; straight above -> camera back). Per eye the local Jacobian of those axes is reduced (in the eye's
  conformal metric, `eye_ndc_per_tan`) to a ROTATION times the per-axis scales: the stamp turns with aircraft roll, the camera delta of a re-composite
  and head roll, stays a rectangle (the section 10 no-shear rule) and keeps the game's size; with no relative roll it is exactly the old quad.
* Logs: `summary-mk360-nbox` gains `sat=` (satellite groups joined), `second=` (second-cell stamps drawn), `cut-kept=`.

Not changed: mode 1 `build_marker` (synthetic boxes) is still axis-aligned in the eye; the D3D12 backend and shaders (same vertex/uv stream); the stamp
pixels outside the window are the last whole in-window picture (range number frozen); no depth test. If the lock diamond is an INSTRUMENT draw (HUD
image, not the marker image) none of this can move it: live item 1 below decides (expected `sat` > 0 while a lock diamond is visible).

### 13.4 Tests (`marker360_offline`, private D:\b-bx2, MSVC /W4, 13,009 checks pass; `tactical_offline` 352 pass, compiles `xr_stereo_output.cpp`)

`test_nbox_lock_diamond` (the screenshot's layout: two groups -> one stamp, no cut, box + diamond + F-4E + 5/3450 covered, at the contact's range,
cached whole and drawn whole at yaw 150; the same shape ON another contact stays that contact's), `test_nbox_lock_far_second_stamp` (reticle 46 GS below:
second stamp, disjoint, same anchor, cell2 cached/drawn outside, released after the lock is lost), `test_nbox_cut_never_replaces_whole` (cut -> c_q 1,
whole -> 2, cut kept out of a whole cache, replaced after 2 s), `test_nbox_roll` (camera roll 0/30/-60/90/180 deg x head roll 0/25/-40 deg, live in the
window and cached behind: the quad's right/down edges follow the game screen's axes as the eye sees them (< 1.2 deg), perpendicular, game size; an eye
rolled with the aircraft gives the old axis-aligned quad; handedness kept behind; the zenith fallback). Every earlier NBOX test unchanged and passing
(fresh == re-composite, rejected-after-completion, eviction). Not run: full build, ctest, the DLL, game, headset.

### 13.5 Live check list (null driver first, then the user's headset; the lead schedules it)

Config: day install + `;mk360:3` (as section 12.5). A mission with enemy aircraft (lock + missile) and mission 02 (ground TGTs, wingmen).

1. `summary-mk360-nbox`: `sat` grows while a lock diamond / reticle is on screen (0 then = the diamond is not a box draw: stop and report); `cuts` drops
   well below the 10.9 % of step123 during locks; `second` > 0 only with far reticles; `no-cell 0`, `invalidated 0`, `cut-kept` > 0 only with close pairs.
2. PNG burst + mirror looking FORWARD at a locked target: box, diamond, "5", range and type name exactly once (nothing left on the flat quad, no
   double), all at the target's depth (no diamond floating at the HUD plane: compare the eyes' disparity).
3. Head turned 90 / 180 deg toward the locked target outside the game window: the WHOLE picture (box + diamond + text) on the contact, not a fragment.
4. Roll the aircraft 360 deg slowly, then fast, with the target in the window and again outside it: the box + diamond rotate WITH the game's HUD / the
   world (edges parallel to the cockpit frame), never split apart, never sheared; tilt the head 30 deg: the stamps tilt with the cockpit, not the head.
5. Two ground TGTs close together (mission 02): still no doubled edge (`cuts` > 0 there), each cached whole when seen apart.
6. Stop and report if: the diamond stays at the window position after the head turns away, a stamp is mirrored or upside down behind the aircraft, perf
   (XR ms) or memory regress (atlas size unchanged: 25 MB at 2560x1440).

## 14. NBOX near-target policy (2026-10-30; headset report of the step134 null run)

User: "Tgt boxes now render in world, but they appear inside the cockpit when you fly close to the target; probably need to scale them smaller as you get closer."
Offline only (no game run, no headset). Cause [I from code, `marker360.h` `build_stamps` / `draw_stamps`]: a stamp is placed at C + dir x (the contact's true distance) and
drawn per eye AFTER the cockpit model with NO depth test (stamp PSO: no depth) at a CONSTANT angular size (one GS = kTanPerGsX/Y). At 5 u/m a contact 10-50 u away
(2-10 m) is inside or at the canopy: the two eyes' parallax (IPD ~0.32 u) is then 3-15 times that of a 150 u contact, so the box converges in front of the cockpit frame
and nothing hides it behind the frame. Far contacts are unaffected (parallax ~0).
Fix (per stamp, per eye, `stamp_near_policy`, applied in `build_stamps`; the plan carries the knobs and the game camera centre `ref`):
* distance floor `mk360min`: the stamp position is pushed OUT along the ray from the game camera centre (common to both eyes, so the disparity really shrinks) to at least that distance; the direction (screen position) is unchanged;
* size `mk360ref` / `mk360floor`: scale = clamp(d / ref, floor, 1) on the stamp's offsets about its anchor (box, label and lock diamond are one stamp group and scale together; the game's pixels are unchanged);
* Defaults `mk360min:150` (30 m at 5 u/m), `mk360ref:750` (150 m), `mk360floor:0.3` (units like tacself); `mk360min:0` + `mk360ref:0` = the old behaviour; the tac-config line shows them only when non-default.
Not done: true occlusion by the cockpit frame (needs the cockpit depth bound in the stamp pass with the model's reversed-Z projection: a GPU change that cannot be verified offline). The floor keeps the box well beyond the canopy instead; if it still shows over the frame in the headset, raise `mk360min`.
Tests: `marker360_offline` `test_nbox_near_policy` (13,062 checks pass, private D:\b-box): constant angular size with the policy off, scale 1 / 0.5 / floor at 750 / 375 / 100 / 10 u, the 3 u stamp's stereo disparity vs the old behaviour (> 20x smaller, still > 0), screen position unchanged, option parse/ranges/describe, `plan_stamps` passes the knobs and camera centre on. `tactical_offline` 352 pass.
Live check: fly at a TGT/enemy to < 100 m: the box shrinks smoothly and never floats in front of the canopy frame (compare `mk360min:0;mk360ref:0`).

## 15. BOX-LOCK: boxes drift off close targets, the diamond is off, boxes vanish outside the window (2026-10-30, headset report of step134)

User (step134, before the section 14 near policy; `docs/headset-tests/2026-10-30b-user-report.md` item 4, screenshot `2026-10-30b-user-4.png`: red box 327 below-left of
the small aircraft, inverted, pause menu up): "Tgt boxes improved. Diamond still off. As you get closer boxes move off of the target models (seen inverted). Occasionally a
tgt box would vanish outside the forward rectangle." Offline work only: no game, headset or DLL; the user's own 15:08 session log was not read (it lives under
%LOCALAPPDATA%\acz-vr); the evidence is the step133 null runs (batchAG runAGB1/AGB2) and the 10-30 morning session (`headset-2026-10-30-session\live-1144`).

### 15.1 Evidence

* **interp:1 is installed** (AGB run.cfg, live-1144 runtime.cfg-installed): `summary-interp` k-mean 0.54 / 0.60 / 0.73 game frames ahead, horizon 9-12 ms,
  camera shift 9.9 / 11.9 / 15.5 u per frame (AGB2 / AGB1 / live-1144). [E] Code: `step5TacClose` replaces every object's translation `t` by its DISPLAY-time
  `td` before publication, while `A` (record +0) and the box sprites are the FRAME's (the comment says so); the eyes are the display-time camera.
* Contact speeds in `mk360-contacts` 500-2600 u/s [E]: per display horizon a target moves 6-30 u, the eyes' camera 10-15 u.
* `summary-mk360-match`: `matched-beyond-raw-gate` = 1380/1380 (AGB1), 1302/1302 (AGB2), 18246/18553 (live-1144) [E]: the raw projection of the contact
  never lay on its box; the learned per-track / global screen offsets did all the matching.
* Every box is read as TWO groups [E: AGB1/AGB2/live-1144 `groups` = `sprites`, `multi:0`, every sprite half >= 8 GS, all 8-16 GS from the contact's projection,
  mean signed residual (-0.5,0.2) / (0.0,0.1) / (-0.0,0.1); `nbox sat` = `live` (1380/1380) in a flight without a lock]. Which half of the box is "the box
  group" is arbitrary; the box CENTRE (between the pieces) is on the contact.
* `identity=new:88 ... cleared-in-view:74` (live-1144) [E]: identities were dropped in view (>= 9 frames without a matched box) about as often as made. That run's
  camera was the junk E6 base (e6-anchor-free.md section 11: ranges ~140,000 in its contact lines), so its geometry is not usable; the counts are. At t 585 s a red
  g9 box was matched to contacts flying 3,400 u/s (our missiles near the target) [E]: a second vanish path, not changed here.
* Screenshot 4 is paused: the composite then re-draws the last plan (DECOUPLE), so the offset at the moment of the pause stays frozen [I].

### 15.2 Causes (code)

* **B1 (drift, all ranges, 1/range)**: MATCH-2 projected the display-time centroid through the frame-time A (the lag 6-30 u = 2-35 GS at 300 u, beyond the 20 GS
  gate at close range, absorbed by learned offsets while steady), and a live stamp was anchored at `C + ray(box group) x range` = where the target WAS at frame
  time, while the eyes see the model at its display-time position: the box trails the model by the target's motion per horizon (offline: 5.2 deg at 300 u,
  7.7 deg at 200 u, 1.0 deg at 1500 u for a target in formation at 1800 u/s).
* **B2 (inverted / outside the window)**: the cached picture was drawn at the contact + the learned GS offset in the game screen's axes of the CURRENT attitude.
  That offset held a box PIECE's offset plus the B1 lag (a world-space vector): after a roll it points the wrong way (offline: 2.2 deg at 400 u inverted).
  In view, a stale offset pushes the residual past the gate: unmatched frames (offline: 5 of 12 at 220 u crossing) feed the 9-frame in-view miss rule that
  DROPS the identity, and without identity nothing is drawn once the contact leaves the window (defect c).
* **B3 (near policy, merged after step134)**: (i) the push-out origin was the frame's game camera, the eyes are 10-15 u away (interp) + the head: a 60 u target
  pushed to 150 u moved 6.6 deg off for 12 u; (ii) the near SCALE shrank the picture about the matched GROUP = a piece 12 GS beside the target (1.1 deg at
  200 u). Both also make "closer = more off".
* **Diamond (a)**: if the diamond is a marker-image satellite (section 13; `sat` > 0 during a lock would show it), it is one picture with its box and had
  exactly the box's B1/B2/B3 error (offline: 5-8 deg off at 200-300 u). If it is instead a HUD INSTRUMENT draw, no marker change can move it: the HUD layer is
  scaled `xr_hud_scale` (0.8 in the user's install) about the gunsight-glass pivot (hud_menu.h), which pulls every world-registered HUD symbol 20 % of its
  angle toward the pivot - the 2026-10-29 shot (diamond lower left = toward the view centre, of a box up right) fits that too [I]. Live item 3 decides.

### 15.3 Fix (`marker360.h`, producer one line, `xr_stereo_output.cpp` two calls)

* Producer (`step5TacClose`): keeps the frame-time translation in `ObjDraw::tf` (`tf_valid`) when it replaces `t` by `td`. Without interp `tf_valid` = 0 and the
  consumer uses `t` (byte-identical behaviour).
* `update`: each track carries `posf` (frame-time centroid, same clustering); the window test, MATCH-2 projections (now and previous frame), offset learning and
  the satellite "another contact projects here" test use `posf` through A: same time as the sprites, no lag.
* `plan_stamps`, live: a matched contact's picture is anchored ON the contact: world point = its display-time centroid `pos`; pixel offsets (`ox/oy`) and the
  game screen axes measured at the contact's frame-time projection. Unmatched groups: unchanged (own ray, `far_units`). Cached: the stored offsets are already
  contact-relative; the learned screen offset is no longer added.
* `set_view_centre(plan, ev)`: the near policy's origin = the midpoint of this composite's two eyes (fresh frame and DECOUPLE re-composite); the scale centre is
  the anchor = the contact.
* Unchanged: matching rules, gates, identity/miss/lost rules, cache quality, cuts, satellites, backend, shaders, logs.

### 15.4 Tests (`marker360_offline`, private D:\b-box2, MSVC Release; `MK360_SOFT=1` lists every failing check)

Before = HEAD `marker360.h` with the new tests (15 of 13,374 checks fail); after: **13,374 pass**. `tactical_offline` 352 pass (compiles `xr_stereo_output.cpp`); `gpu_copy_step1_offline_tests` builds (producer hunk) and its
`step5-tac`, `step5-hud-markers(-off)` fixtures pass. Not run: full build, ctest, DLL, game, headset.

| test | before | after |
| --- | --- | --- |
| `test_boxlock_interp_close` (formation 1800 u/s, one frame horizon; box + lock diamond; 6000/1500/600/300/200 u) | box 0.26/1.04/2.74/5.19/7.72 deg off the model, diamond the same | 0.000 deg, diamond at the game's offset from the box |
| `test_boxlock_inverted` (400 u crossing 600 u/s, 0.6 frame; upright, rolled 180 deg, nose away) | cached box 2.19 deg off | 0.009 deg, matched every inverted frame, cached 10/10 |
| `test_boxlock_close_persist` (220 u crossing) | matched 7/12 in view | 12/12, cached 10/10 outside |
| `test_boxlock_near_push_origin` (eyes 12 u from the game camera, target 60 u) | 6.58 deg | 0.095 deg |
| `test_boxlock_near_scale_two_pieces` (box read as two bars, 200 u, floor scale) | 1.11 deg | 0.001 deg |

### 15.5 Live check list (null driver first, then the headset; the lead schedules it)

1. `summary-mk360-match`: `matched-beyond-raw-gate` drops from ~100 % to the two-piece share (each box's matched piece sits 8-16 GS from the contact, so up
   to all of them may still count "beyond raw"); `offset-gs` now pure piece geometry (no speed dependence). `summary-mk360` `cleared-in-view` far below `new`.
2. Fly in formation with a wingman / chase a target to < 300 u: mirror + `xr_png_once` bursts: the box stays on the model; roll 360 deg with the target in the
   window and once outside: stays on it.
3. Lock a target (missile): `summary-mk360-nbox` `sat` grows WITH the lock (pieces already give sat = live, so compare the rate before/after the lock tone). If the
   diamond still sits off the target while the box is on it, it is the HUD layer: A/B with `xr_hud_scale` 1 (the diamond then on the target = cause found; it
   needs a HUD-scale exception for world-registered symbols, a separate change).
4. Vanish: with a target circling outside the window, `mk360-contacts` keeps `id..` on it and `nbox cached` grows every period; a box that still disappears with
   `lost=` growing is the 0.25 s draw gap (not changed), with `cleared-in-view` growing a remaining match failure (report `summary-mk360-match`).

## 16. Headset test 5 (2026-10-30, step137): lock diamond, clipped labels, friendly boxes, blue flashes, radio subtitles

User report `docs/headset-tests/2026-10-30c-user-report.md` items 6-9; evidence `D:\acz-evidence-archive\headset-2026-10-30-session\live-1936-step137`
(`xr-stereo.log`, `step5-producer.log`, `user-shots\8.png`, `9.png`). Offline work only: no game, no headset, no build yet. Tags as above ([E] / [I] / [G]).

### 16.1 Label text "mirrored" and clipped (item 8, `9.png`): LABEL-WHOLE (fixed)

* **What the shot shows [E, 4x crop of 9.png]**: the label is NOT mirrored. The glyph ORDER reads left to right (`M I G - 2 1 B I S`, the hyphen is the 4th
  glyph; a mirror would put it 6th), the label sits upper right of the box and the range `774` lower right, exactly the game's layout, and `774` is upright.
  Each glyph of the name shows only its BOTTOM part (M = two legs, I = a foot, G = its lower bowl, 2 = diagonal + base bar, B = the lower loop): the top of
  the line is cut off along a straight edge that turns with the stamp. Half glyphs read as backwards letters.
* **Geometry cannot mirror [E, code]**: `stamp_frame` (`marker360.h`, the det < 0 branch) reduces the eye Jacobian to a ROTATION times positive scales, so
  every stamp quad has det > 0; one quad carries box + label, so a mirror would also flip the box/label layout. New test: the cached quad's orientation for
  contacts beside / behind / above / below with the aircraft upright, rolled 35 / -90 and inverted (>= 80 directions) is never mirrored.
* **Root cause [E code, I for the exact frame]**: `plan_stamps` cached a picture as WHOLE (`c_q` 2) when its BOX lay >= 1 GS inside the game window
  (`inside(q.box)`), without looking at its labels. The type name sits ABOVE / RIGHT of the box, so a contact leaving the window over its top (pull-up, the
  common case) or right edge keeps its box inside for some frames after the game has already clipped the label at the window edge; every such frame
  replaced the cached picture (q 2 >= q 2), and the last one before the exit is what is drawn outside the window: a label with its top cut away.
  The same happened when a label did not fit the 128 x 64 GS cell window (cut by the stamp).
* **Fix (`marker360.h` `plan_stamps`)**: a picture whose box is whole and not cut but whose labels are not wholly inside the window (>= 1 GS) and inside the
  stamp is quality 1 (`L::lab_whole`). q 1 is cached only when the contact has no whole picture (better than nothing), never replaces a whole picture unless
  that is older than `kStampRecacheClippedS` (8 s; was 2 s for any lesser picture), and is replaced by the next whole one. In-window (live) stamps are
  unchanged: they show this frame's game pixels (the game itself clips at its window edge). Counters: `label-clipped=` (live q-1 stamps), `cut-kept=` now
  also counts q-1 pictures kept out of a whole cache; `mk360-contacts` entries end with `:c2` / `:c1` / `:c-` (cached picture quality; none).

### 16.2 Friendly boxes "hit and miss" (item 6): IDENT-HOLD (fixed for the dropout case) + diagnostics

* **Not refused, not atlas, not colour [E]**: last `summary-mk360-nbox` `no-cell=0; evicted=0; invalidated=0; no-atlas=0; rejected=0`, backend
  `refused-view=refused` (all benign), `cells` 1-8 of 24; FRND colour stable `id0.21,0.23,1.00` in every `mk360-contacts` line (no recolour of the
  wingman). Per summary period, nbox `cached`/frame equals `summary-mk360 identified-outside` in every period but one (465 s, a red contact): every
  identified contact outside the window WAS drawn.
* **The misses are identity gaps [E]**:
  1. The wingman (contact 1, r 339-6700, bearings -128 / 166 / 176) was tracked from t 305 s but unidentified (`none1.00,1.00,1.00:g0`) until its first
     in-window box at t 685 s (`1:win+box:r757`). By design (D-355: never invented) a contact is boxed outside the window only after the game boxed it
     inside. Not changed; the radar-blip identity remains the only way around it (not implemented).
  2. A **dropout** dropped the identity: at t 945.9 -> 960.9 s the frames stopped (no summaries for ~10 s, `no-a` +43), `identified` 1 -> 0, every track
     lost (`lost_ident` +1); the wingman came back as a NEW contact 45 at the SAME place relative to the player (`1:out:r3210:b-175.8:e20.8` ->
     `45:out:r3136:b-174.1:e21.8`, unidentified) and had no box until it crossed the window again at 995.9 s. Again at 1031 -> 1036 s (`no-a` +112,
     contacts none -> 60).
* **Fix (`marker360.h` `update`, steps 6-7)**: a dropout = a frame without A, or frames stopped longer than `kDropoutGapS` (1 s; a shorter hitch is not one).
  An identified contact lost after a dropout (its last sighting before it) leaves a **ghost**: identity, colour, label, cached pictures (its cells stay
  owned) and its offset from the game camera. A contact first seen after that sighting, unidentified and not boxed this frame, whose camera offset
  matches within max(400 u, 15 % of the range), takes the ghost over (greedy nearest): the box is back the same frame. Ghosts expire after `kGhostS`
  (45 s; their cells are then freed), at most 8. Losses while frames keep arriving (a kill) never make a ghost, so a killed target is never revived.
* Counters: `summary-mk360-nbox` `ghosts=N(reacquired:M,held:K)`, `out-uncached=` (identified, outside, seen, but no cached picture: the remaining
  "never boxed whole" misses), and the `:c` suffix per contact.

### 16.3 Blue flashes (item 6 / d): NEAR-FADE (fixed)

* [I] The near policy (section 14) already applies to EVERY stamp, friendly or not (`build_stamps` calls `stamp_near_policy` for each draw, no colour
  test), so a wingman's box was pushed out to `mk360min` and shrunk to the floor scale. It still DREW: a wingman passing the canopy at 80-150 u (contacts
  inside `tacself` 80 u are the player's own aircraft) flashed a full-opacity blue box for a few frames. No per-frame near log exists to prove the flashes
  were that [G: the remaining candidate].
* **Fix**: `near_fade_of` (`marker360.h`): opacity 1 at d >= `mk360min`, linear to 0 at `mk360fade` (default 75 u = 15 m), not drawn at or below it
  (d = the TRUE distance from the eyes' centre, before the push-out). The opacity travels in the stamp vertex `col.z` (was a constant 1) and
  `xr_tactical.hlsl` `ps_stamp` multiplies the premultiplied texel by it. `mk360fade:0` = the old behaviour; `mk360fade` >= `mk360min` = hidden below it,
  no ramp. Counters `near=faded:N,hidden:M` (eye 0 per composite).

### 16.4 Lock diamond (item 7, `8.png`): design, not implemented

* **Root cause [E]**: the diamond is a HUD INSTRUMENT draw, not a marker-image draw. `hud_classify` (`gpu_copy_step5.h`) makes only depth-GREATER /
  GREATER_EQUAL draws boxes and only the indexed text-route draw right after a box a label; the diamond shape is neither, so it is replayed into the HUD
  image. That image is the HUD layer: flat, only in the window, and scaled about the gunsight glass: the log line
  `cockpit-hud-registration;hud-scale=0.700;pivot=glass-centre;registered=pivot-only;error-at-10deg=2.96deg;effect=...world-symbols-compressed-toward-the-pivot,markers-layer-unscaled`
  [E, both flights]. The NBOX box is world-anchored on the contact; the diamond is pulled 30 % of its angle toward the pivot: in `8.png` it sits left of
  the box (toward the view centre) at the same height. Had it been a marker-image draw, section 13's satellite rule would already have joined it to the
  box's stamp. There is no target identity on the HUD draw (no matrix, no contact; only its vertices).
* **Immediate, config only**: `xr_hud_scale=1` puts the diamond on the target IN the window (`exact=xr_hud_scale=1` in the same log line); it stays flat.
* **Design (LOCK-SHAPE)**: (1) one diagnostic run to name the draw: `fxmem:1;fxsig:d34ee153` (the HUD sprite pixel shader) during a lock; `fx-flare-draw`
  lines give each draw's GS `bbox`, `nv`, `cls`, `img=hud`: the diamond is the draw whose bbox is a ~+-12 GS square within ~30 GS of a red box group of
  the same frame (it moves to the box as the lock completes). (2) Producer rule: after a box draw in the frame, an instrument draw of that PSO class whose
  vertices (read like a box: `step5TacReadSprites`) form a 45-degree square in the box's colour, centred within `kSatReachGsX/Y` of a box of this frame,
  is classified `hud_marker_box` (replayed into the MARKER image, never the HUD). (3) Consumer: nothing new: it is a same-colour group in reach of the
  contact's box, so section 13 joins it as a satellite: one picture with the box, at the contact's depth, cached and drawn outside the window, roll-correct.
  Cost: one vertex read for the few draws following a box. Risk: a non-diamond red instrument shape near a box joins the stamp (bounded by the shape test).
  Not implemented here: the draw's signature is not on record, and a blind classifier rule would move HUD instruments.

### 16.5 Radio chatter subtitles (item 9): design, not implemented (superseded: see section 18.1, the radar-on evidence)

* **Root cause [E code + earlier evidence]**: the subtitle quads are drawn AFTER the HUD region has ended. The HUD replay region ends at the first scene
  barrier out of RENDER_TARGET (`hudReplayBarrier_locked`, `gpu_copy_step5_hooks.inl` ~931, `hudRegionEnd(...,"scene-barrier")`); this run: `continuous-hud
  end=rt-change:0,scene-barrier:30143,close:0` = every frame. After that `step5HudDraw` replays nothing (`hudRegionEnded`, ~2083 / ~2106). A scene
  barrier while not away is "the real tone-map hand-off" and is never resumed (`gpu_copy_step5.h` ~330; RADAR-SCENE-RESUME resumes only an excursion).
  The tone-map pass reads the scene (16x16 / 1x1 exposure targets), then the scene target comes BACK and gets 38-103 unindexed 4-vertex quads + 3 indexed
  of the HUD text PSO (VS 534f29ca / PS d34ee153, blend 5/6, depth ALWAYS, stencil ALWAYS/KEEP: `hud_decide`-clean), in scripted plateaus, then the output
  pass (`J-radar-analysis.md` 1.3 [E]); the AWACS subtitle is such a "tm-phase" draw (`J-smoke-trace.md` section 2 [E: PNG]). Not the capture rectangle
  (the HUD layer is the whole game image, `hud-swapchain=2400x1350`) and not the marker class gate (those draws never reach `hud_classify`).
* **Design (HUD-LATE)**: a producer flag from the terrain value (`hudlate:1`, through the existing `tac::producer_flags` path: no new config key; the
  installed config is at its key cap). With it, the transition of the SAME scene resource back INTO RENDER_TARGET after a non-away scene-barrier end
  (plain full barrier, once per frame) reopens the region in LATE mode: only draws of the HUD text class (above signature, or the frame's learned text
  route) are replayed, into the HUD instrument target without a clear (it already holds the frame's HUD), never classified as markers; anything else is
  skipped and counted; the next RT change / barrier / close ends it. `hudRegionWhy` must keep reporting "scene-barrier" for the effects layer (as
  `hudRegionResumed` does for the radar resume) and the scene-end copy must not fire twice. Caps: late frames use the radar caps (256 draws / 960 commands:
  86 + 103 fits). Logs: `continuous-hud` `late=resumes:,draws:,skipped:`. Consumer: unchanged; the subtitles then sit on the HUD layer exactly where the flat
  game shows them (above the HUD, scaled with it). Live check: `late draws` 38-103 during radio chatter, 0 in quiet plateaus, text visible, no doubled HUD.

### 16.6 Options (inside `xr_terrain_stage`, no new config key)

`mk360fade:<u>` (new, default 75, 0 = off; listed in `tac-config` only when not 75). Unchanged: `mk360:3`, `mk360min` 150, `mk360ref` 750, `mk360floor` 0.3.
Constants (not options): `kStampRecacheClippedS` 8 s, `kGhostS` 45 s, `kGhostGateMin` 400 u, `kGhostGateFrac` 0.15, `kDropoutGapS` 1 s, `kGhostCap` 8.

### 16.7 Tests (`marker360_offline`, registered in `src/acz-vr/CMakeLists.txt` as before)

`test_hs5_label_whole` (box whole at the window top, label clipped: q 1, the whole picture stays cached and is what is drawn outside; never mirrored from
>= 80 directions x 4 rolls; q 1 cached when nothing better, replaced by the next whole picture, replaces a whole one only after 8 s; a label wider than the
cell window = q 1; log), `test_hs5_ident_hold` (wingman boxed, 15 s dropout, re-tracked at the same offset: identity + cell taken over, drawn the same frame;
frames without A also a dropout; a kill makes no ghost and a later contact stays unidentified; another offset does not take it, the ghost keeps its cell then
expires and frees it; `out-uncached` and `:c-`), `test_hs5_near_fade` (opacity 1 / 0.5 / 0.2 / hidden at 150 / 112.5 / 90 / 75 u, off, fade >= min,
a BLUE and a red contact at 100 u through the plan path fade identically, option parse / describe / range). Written without a build (the lead's
"build go" gate): not yet compiled or run; `xr_tactical.hlsl` not yet compiled with fxc.

### 16.8 Live check list (null driver first, then the headset)

1. `summary-mk360-nbox`: `label-clipped` > 0 when targets leave over the window top; outside the window no half label (PNG burst + mirror, pull up past a
   target); `mk360-contacts` `:c2` for boxed contacts.
2. A radio cut-in / cutscene mid-mission: `ghosts` and `reacquired` grow, the wingman's box is back right after it (`:c2` on the new id); a kill never
   raises `reacquired`.
3. Fly formation close to the wingman (< 150 m): no full-opacity box at the canopy; `near=faded/hidden` grow; compare `mk360fade:0`.
4. Lock (16.4 step 1 diagnostic) and radio chatter (`continuous-hud` after HUD-LATE) as designed above.

## 17. Headset test 6 (2026-10-30, step143): labels cut, friendly boxes hit or miss, flashes, unboxed aircraft, lock diamond

User report `docs/headset-tests/2026-10-30d-user-report.md` item 7: "Labels still cut in half; friendly boxes still hit or miss; flashes still seen; some aircraft
get no box at all; lock diamond unchanged." Section 16's fixes were in the install. Evidence `D:\acz-evidence-archive\headset-2026-10-30-session\live-2210-step143`
(`xr-stereo.log`: 172 periods of `summary-mk360*` / `mk360-contacts`; `step5-producer.log`; `ee-camera.log`); earlier `live-1936-step137`. Offline work only (no game,
headset, DLL or build: the lead's "build go" gate). Tags: [E] evidence, [I] inference, [G] guess.

Run facts used below [E]: flight 235-795 s and 1010-1081 s, game paused 795-1005 s (every contact `v0`, identical samples); the E6 camera locked at t 263 s
(`e6-locate;event=locked` at 191883187) and was lost only at t 1114 s, after the last flight summary (no camera loss while boxes were drawn: A is not
suspect); `no-a` 71 (dropouts at 305 s and 705 s). Final `summary-mk360`: sprites 47992, matched 23913, `identity=new:129,recolour:27,cleared-in-view:110`
for `created=67` tracks; `summary-mk360-match`: groups 47772 (multi 207), every sprite half >= 8 GS, nearest contact 8-16 GS for 47642 of 47992, mean
residual (-0.3,-0.5); `summary-mk360-nbox`: live 25526, cached 64096, sat 22090, `label-clipped=196`, `cut-kept=91`, `edge-kept=503`, `out-uncached=0`,
`ghosts=6(reacquired:1,held:1)`, `near=faded:186,hidden:0`, no-cell / evicted / invalidated / rejected 0.

### 17.1 (a) Labels cut in half

* **The section 16.1 rule did run [E]**: `label-clipped=196`; `mk360-contacts` shows q-1 cached pictures in use: contact 16 `id0.29,1.00,0.22:g9:c1` at
  330-335 s (outside the window, r 19-29 km), contact 30 `id1.00,0.00,0.00:g4:c1` at 635 s. A q-1 picture was cached and DRAWN outside the window whenever no
  whole picture existed ("better than nothing", 16.1): a half label shown on purpose. Not the atlas cell (640 x 206 px = the 128 x 64 GS cell window; a label
  beyond it is q 1 too) and not the stamp quad's crop (texel-centre quad = texel-centre uv, `build_stamps`).
* **The label rectangle itself [I, strong]**: `step5TacReadSprites` divided every vertex's x, y by its stored w (`v.x /= raw[3]`, comment "w = 1 in every stored
  vertex"). VS 534f29ca outputs `((x-2048)*2/512, (y-2048)*2/-448, z*2^-24, 1) / q` (fx_sprite_replay.h, live constants): the division by q cancels in the
  perspective divide, so the game's pixel lies at the STORED x, y whatever q is. For a vertex with q != 1 the reader moved it by 2048 (1/q - 1) GS in x AND
  y (q = 0.998: +4 GS right and down). Boxes (untextured) match their contacts to ~0 GS (the mean residual above), so their q is 1; labels are textured glyphs
  whose q is the GS Q register (often left over from an earlier primitive). A label rectangle shifted DOWN-RIGHT by a few GS gives exactly 9.png: the stamp's
  top edge (label top - 2 GS margin) runs through the name's glyphs, its right and bottom edges still cover "MIG-21BIS" and "774", the box is whole; in the
  window the uncleared top halves stay on the flat quad (another depth), outside it only the bottom halves exist. Not proven: no log has recorded a glyph's
  q. The new `mk360-stamp` line records it.
* **Fixes**: (1) the reader keeps x, y as stored and records q (`SpriteVert::q`; `TacFrame::box_q / label_q`: min, max, vertices with q != 1);
  (2) LABEL-CORE: a q-1 picture is cached as its BOXES only (core = boxes + margin, a sub-rectangle of the same cell: `Track::c_u0/c_v0`, `StampDraw::u0/v0`;
  `build_stamps` offsets the uv and refuses a draw that would sample beyond its cell): outside the window a contact without a whole picture shows its box
  without the clipped label; the first whole picture replaces it. Live (in-window) stamps unchanged.
* Diagnostic `mk360-stamp` (one per summary period: the last label-clipped live stamp, else the last live stamp with a label): `id;q;label-whole;cut;complete;
  glyphs;box=x0,y0,x1,y1;label=x0,y0,x1,y1;px=x0,y0,x1,y1;box-q=lo,hi(off:n/N);label-q=lo,hi(off:n/N)`. `label-q off:0` on a run where labels are still cut
  refutes the q cause; then compare `label` with the shot.

### 17.2 (b)/(d) Friendly boxes hit or miss, aircraft without a box

* **Identity flip-flop [E]**: 129 identities made and 110 dropped in view for 67 tracks; contacts carried a whole cached picture (`:c2`) while UNidentified
  (= not drawn outside the window): 17 `none1.00,0.00,0.00:g9:c2` from 355 to 575 s (220 s outside the window without a box), 12 at 330-360 s, 16 at
  360-370 s, 29/30 at 665-670 s, 47 at 735-745 s, the friendly 59 `none0.21,0.23,1.00:g0:c2` at 1010-1015 s, 63 at 1045-1055 s.
* **Root cause [E + I]**: every game box is read as TWO same-colour pieces either side of its contact (groups = sprites, all halves >= 8 GS, all 8-16 GS from
  the contact, symmetric). MATCH-2 matched one piece to the contact; the other piece was free for ANY other track within the 20 GS gate: a missile, a ground
  object or a neighbour on nearly the same line of sight. That track took the contact's colour and label after 2 frames (`identity new`), the contact's learned
  offset pointed at the piece it kept, and when the other track moved off, the in-view miss rule dropped whichever identity no longer found a piece
  (`cleared-in-view`). A contact crossing another's box was RECOLOURED after 2 frames (recolour forgets the cached pictures, w12-2 finding 4) and then dropped:
  17 went `id0.29,1.00,0.22:g0` (345 s) -> `none1.00,0.00,0.00:g9` (355 s: 16's red MiG-21bis box; that period `recolour+2,cleared-in-view+2`). The g0
  identities (a box without a label = the label-less piece) on long-lived tracks (17, 19, 29, 41; the wingman 10 at 665-700 s) are the same split.
* **Not the cause [E]**: no cell / atlas / copy refusal (all 0), `out-uncached=0` (every identified contact outside the window had a picture), no `kMarkCap`
  pressure (avg 0.71 markers per frame, no `mark_dropped`), `cells` 1-8 of 24.
* **(d) never boxed [E]**: (i) contacts never in the game window (contact 1, 235-300 s, bearings -70..-177, r 200-6000: by design D-355, nothing invented);
  (ii) contacts beyond the game's box range: 34-37 / 39-42 in the window at r 55,000-200,000 u without a box, boxed from ~31,000 u (40 at r30970, 3 at
  r30852): the game draws no box there either; (iii) fast tracks (v 2700-4000 u/s: 9, 23, 31, 53, 54, 64: missiles) never boxed by the game; (iv) the
  identity drops above (the defect); (v) the pause (795-1005 s): 57/58/59 in the window without a box, `cleared-in-view+3` at 795 s. Ground targets ARE boxed
  (red g3 "TGT" identities: 20, 30, 41, 50).
* **Fixes (`marker360.h update`)**: PIECE-PAIR (`Params::pair_gate` 10 GS, `pair_half` 24 GS): before matching, two pieces of one colour (never a lock diamond)
  whose union is box sized and whose union CENTRE lies within 10 GS of a seen contact's frame-time projection, closer to it than either piece by 2 GS, become
  ONE group matched to that contact (greedy by that distance; a piece and a contact pair at most once): the whole box, centred on its contact, nothing left to
  steal, a learned offset of ~0. RECOLOUR-HOLD (`recolour_confirm` 6): an identified contact takes another colour only after 6 consecutive matches (a new
  identity still needs 2); a box of another colour is drawn live but never replaces the contact's cached picture.
* Limits [I]: two same-colour contacts closer than ~40 GS still have their inner pieces merged by the old `group_gap` rule (unchanged); a single unread piece
  matches as before; the pause and the box range (ii, v) are the game's.

### 17.3 (c) Flashes

* **Not the near fade [E]**: `near=faded:186,hidden:0` for the whole session (186 eye-0 stamp draws, ~3 s, none hidden): section 16.3's hypothesis cannot
  explain repeated flashes.
* **Causes [E counts, I mechanism]**: (1) live stamps of box groups matched to NO contact were drawn at `far_units` (32,500 u = the flat quad's depth) on their
  ray: live 25526 vs ~23,700 matched groups = ~1,800 one-frame depth pops of a whole box picture between the contact's range and 32,500 u; (2) the piece thefts
  of 17.2: another track drew the stolen piece (+ label) at ITS 3D position for a few frames, and outside the window a wrongly identified track carried the
  picture until lost. HUD-layer quads: the marker pixels never reached the HUD image (markers split, `producer-marker-refused=0`).
* **Fixes**: (1) DEPTH-HOLD (`plan_stamps`): an unmatched group borrows the range of the seen contact projecting nearest to it within twice its match gate (its
  own ray kept); none near: `far_units` as before. (2) PIECE-PAIR / RECOLOUR-HOLD (17.2).

### 17.4 (e) Lock diamond: LOCK-SHAPE (implemented) + the census

* **Can the logs name the draw? No [E]**: the only HUD draw census in step143 / step137 is the flare log (`fxsig:f1a4e000`, `fx-flare-draw`): 400 lines, all in
  frames 5355-~8000 (t 322-~370 s), only depth-ALWAYS additive / near-centre draws, no vertex colour, no relation to the frame's boxes; the first lock in
  `mk360-contacts` (g14 = type + range) is at 715 s, after the census was used up. The draw's signature is not on record.
* **One-shot census `mk360cen:1`** (any mk360 mode; producer flag `kProducerLockCensus`): 40 frames >= 1 s apart; in each, from the first box draw on, every HUD
  draw gets a `step5-producer.log` line `tac-lock-census;f;ord;after-box;cls(box|label|instrument|lock-moved);vs;ps;idx;cnt;nv;bbox;rgb;q=lo,hi(off);
  shape=<the diamond test's verdict>;box=<nearest box>(d:dx,dy);read` (<= 20 per frame, through the existing diagnostic line queue). Run it with a lock held.
* **LOCK-SHAPE (default on with `mk360:3`; `mk360lock:0` = off; producer flag `kProducerLockShape`)**: an INSTRUMENT draw (not a box, label or refused draw)
  after a box draw of the frame, of <= 48 indices (<= 24 reads per frame), whose vertices form a 45-degree square (`tactical_producer.h lock_shape_test`: half
  3-24 GS, aspect <= 2, every vertex on the rhombus |u|+|v| in 0.55..1.2 of its bounding box, all four tips present, no corner vertex) in the colour of a box
  of this frame (0.15) centred within 64 x 48 GS of it, is classified `hud_marker_box`: replayed into the MARKER image and added as a marker of shape 1
  (`add_lock_shape`; labels never join it). Consumer: a diamond group is never matched to a contact and never counts as another contact's box; it joins its
  box's stamp as a satellite (section 13): one picture at the contact's depth, cached whole, drawn outside the window, roll-correct, no longer scaled by
  `xr_hud_scale`. `continuous-tac` gains `lock-shape=moved:N,diamond-not-moved:M;census-frames=K`.
* Limits [G]: a diamond drawn BEFORE its box in the frame, of another colour than the box, or of more than 48 indices is not moved: the census decides (its
  `after-box`, `rgb`, `cnt` fields).

### 17.5 Options and logs

Options (inside `xr_terrain_stage`, no new config key): `mk360lock:0|1` (default 1 with mk360:3; in `tac-config` only when 0), `mk360cen:0|1` (default 0).
Constants: `pair_gate` 10 GS, `pair_half` 24 GS, `recolour_confirm` 6, `kLockReadsPerFrame` 24, census 40 frames / 1 s / 20 lines. New lines:
`summary-mk360-hs6;pairs;recolour-held;depth-held;core-cached;lock-shapes` and `mk360-stamp` (xr-stereo.log, every period), `tac-lock-census`
(step5-producer.log, with `mk360cen:1`). Producer flags in `tac-armed` become 9 with mk360:3 (1 + LOCK-SHAPE 8), 25 with `mk360cen:1`.

### 17.6 Tests (`marker360_offline`, written without a build: not yet compiled or run)

`test_hs6_piece_pair` (two pieces + a track 14 GS right of the contact, 3 km farther: before (pair_gate 0) that track is identified with the contact's colour
and label in >= 15 of 20 frames; after: never, the contact holds the whole box centred on it, offset 0, `pairs=20`; two same-colour contacts 50 GS apart pair
on their own contacts; a lone piece still matches), `test_hs6_recolour_hold` (5 frames of a crossing red box: still green, cached picture kept,
`recolour-held=1`; 6 frames: recoloured), `test_hs6_depth_hold` (an unmatched group 30 GS from a contact drawn at its range on its own ray; 120 GS away at
32,500 u), `test_hs6_label_core` (label clipped at the window top: the cached picture is the box core (95 x 61 px, offsets -9.5 GS), drawn outside the window
with the core's uv; the next whole picture replaces it from the cell origin; a draw beyond its cell is refused), `test_hs6_lock_shape` (diamond outline,
filled and anamorphic accepted; square, glyph, bar, other colour, far, tiny and vertex counts refused; the shape-1 marker gets no label; box pieces + diamond
= one stamp at the contact's depth even with another contact projecting onto the diamond; options, flags, describe; the census line < 426 bytes),
`test_hs6_stamp_line` (QStat, `mk360-stamp` content and reset, `summary-mk360-hs6`). Adjusted: `test_options` (mk360:3 flags include LOCK-SHAPE),
`legacy_params` (pair_gate 0), `test_unread_and_mismatch` and `test_w12_recolour_drops_cache` (6 frames to recolour). Not run: these tests,
`tactical_offline`, `gpu_copy_step1_offline_tests` (producer hunk), the DLL, the game, the headset.

### 17.7 Live check list (null driver first, then the headset)

Config: the step143 day install + `;mk360cen:1` for one run (remove afterwards).
1. `summary-mk360`: `identity new` close to the number of real aircraft and `cleared-in-view` far below 110 per flight; `summary-mk360-hs6 pairs` ~ the boxes
   per frame; `mk360-contacts`: no long `none...:c2` stretches for contacts that were boxed.
2. `mk360-stamp`: `label-q off` > 0 where labels were cut before = 17.1 confirmed; outside the window no half label (PNG burst + mirror, pull up past a
   target).
3. Flashes: `summary-mk360-hs6 depth-held` grows; mirror: no box jumping in depth for a frame.
4. Lock a target and hold it 10 s: `continuous-tac lock-shape=moved` > 0 and the diamond on the target in VR (also with the head turned away). `moved=0`: read
   the `tac-lock-census` lines of the lock frames (the diamond's `cls`, `after-box`, `cnt`, `rgb`, `shape=` verdict) and report them.

## 18. Headset tests 5/6 follow-up (2026-10-31): radio subtitles (HUD-LATE) and the lock-diamond census (CENSUS-AIM, LOCK-HOLD)

Offline work only (no game, headset, DLL build or install). Evidence: `D:\acz-evidence-archive\step146\batchAG-fx\run{AGB1,AGB2,AGC1,AGC2}` (null runs with
`mk360cen:1`, `gpu_copy_step5_hud_radar=true`). Tags: [E] evidence, [I] inference.

### 18.1 Radio subtitles: HUD-LATE (`hudlate:1`, default off) - section 16.5 corrected

* **16.5's premise does not hold for the installed config [E]**. With `gpu_copy_step5_hud_radar` the HUD region is RESUMED after the tone-map pass
  (RADAR-SCENE-RESUME), and the subtitle quads reach the HUD seam, where `hud_radar_decide` skips them as `late` (after an excursion, outside a stencil window,
  neither a REPLACE writer nor an EQUAL test). runAGB2 `hud-end` lines: quiet frames `scene-leaves-away=3;scene-resumes=2`, 0 late-skipped; subtitle frames
  `4 / 3` and a tail of late skips: frame 312 `skipped=late:87`, `hud-end-verdicts accept:89` (the plain `hud_decide` accepts them), stencil `A`(ALWAYS) and
  disposition `l` for the whole tail; frames 1212 (43), 2112, 5113 (58), 8113, 14414, 17415 alike. Session: `skipped=late:274611` over 20801 frames, `refused=0`.
  The final scene barrier of every frame is never followed by a barrier back (leaves - resumes = 20801 = 1 per frame), so a reopen at a "non-away scene-barrier
  end" (16.5) would never fire.
* **Fix (`hud_late.h`, `gpu_copy_step5_hooks.inl step5HudDraw`)**: with `hudlate:1`, a draw that `hud_radar_decide` skips as late and that is of the HUD TEXT
  class (VS 534f29ca, PS d34ee153, blend SRC_ALPHA/INV_SRC_ALPHA, no depth test or ALWAYS, no depth write, stencil off or ALWAYS/KEEP on both faces:
  `hud_late_text`) is replayed into the HUD INSTRUMENT image when the plain `hud_decide` accepts it and the frame's caps allow (radar caps 256 / 960). It opens
  no radar window, is never classified (no box / label / lock test / census), and a refusal or cap only skips it (counted; the frame's HUD stays valid). The
  gun tracer (blend 5/2), sun flare (ONE/ONE), firing wedge (GREATER) and radar mask / contact draws (stencil REPLACE / EQUAL / NOT_EQUAL) are not text.
  Consumer unchanged: the subtitles sit on the HUD layer where the flat game draws them (top of the image, scaled with `xr_hud_scale`).
* Switch: `hudlate:0|1` inside `xr_terrain_stage` (no new runtime.cfg key), carried like `hudrs` (`effects_stage_flags.h` -> `hud_late_flag()`); needs
  `gpu_copy_step5_hud` + `gpu_copy_step5_hud_radar` (without the radar key nothing changes). Logs: `xr-stereo;event=hud-late-option` once per install;
  step5-producer `continuous-hud-late;on=1;frames=N(text:F);text-draws=;max-per-frame=;refused=;capped=;late-other=;last-refusal=` (status cadence);
  the `hud-end-draws` disposition shows `T` for a replayed text draw.
* Limits [I]: if some subtitle glyph draws carry another blend or a stencil test they stay skipped (`late-other` counts the non-text late skips). The radar
  diagnostic's own `late` replay count includes the text draws.

### 18.2 Lock diamond: what the step146 census shows, and CENSUS-AIM

* **No lock happened in any step146 null run [E]**. runAGB1 Gun mode (`missile=0`, `gun=0`); runAGB2 Kill mode: every autopilot line `lock=0`, the green air
  targets stayed 100-180 degrees off the nose (`brgErr`), `missile=0`; runAGC1/AGC2 Cruise. `summary-mk360-hs6 lock-shapes=0`,
  `continuous-tac lock-shape=moved:0,diamond-not-moved:0` in all four.
* **The census never saw a target box either [E]**: all 40 census frames of each run fell in the first ~40 s of producer frames (runAGB1 ticks
  196254265-196293250, before the autopilot started at 196304015). Every frame's census "box" (160 of 160) was VS 534f29ca / PS 4caf6e2d, 274 indices / 220
  vertices, bbox ~1570-2580 x 1430-2100 GS, rgb 1.00,0.98,0.89, q ~1e-5: the GREATER cloud-sprite draw, which `hud_classify` calls a box (depth-tested) but
  which yields no marker; the remaining 19 lines per frame (2964) were the green HUD (PS d34ee153, rgb 0.29,1.00,0.22: pitch ladder, gunsight, scales),
  all `shape=not-tested` (no marker in the frame, so the diamond test never ran) and `box=-1`. So the diamond's signature is still not on record, and the
  shape test (17.4) is unchanged: nothing contradicts it, nothing confirms it.
* **CENSUS-AIM (`tactical_producer.h census_trigger / census_near_box / format_census_boxes`)**: a census frame starts only once the frame's tail holds a
  COLOURED box marker (not grey/white) within 160 x 120 GS of the screen centre, where the seeker diamond appears during a lock; its first line is
  `tac-lock-census-boxes;f;n;mK=cx,cy,hw,hh,r,g,b,sShape` (<= 6 markers); box / label / moved-diamond draws are logged as before; an instrument draw is logged
  only when its bbox centre lies within 128 x 96 GS (twice the satellite reach) of a box of the frame, others are counted:
  `continuous-tac ...;census-frames=K(far-skipped:N)`.

### 18.3 LOCK-HOLD (autopilot `-HoldLock <s>`, Kill / Chase)

`research/experiments/step83-verify-run1/autopilot.ps1`: the controller's lock model is "target inside `-LockDeg` (9) in bearing AND elevation, within
`-LockRange`, held `-LockSec`". With `-HoldLock 10`: the nearest target (`-PriWeight 0` unless given), pure pursuit (no lead), no gun and no missile until the
target was held in the reticle 10 s in one stretch (log ` HOLDLOCK=t/10`, then ` LOCK-HELD tick=<GetTickCount64>`, tap summary `lockheld=1`), then the
normal attack. A ground pass at ~1100 u/s crosses the 9 km window in ~6 s (sim: lock 3.1 s, 7.3 s with +1 km), so while the hold is pending the window and
the nose-on dive start `V*HoldLock + MissileMinGround + 4 km` out (14.4 km at the defaults). Sim: Kill static ground target LOCK-HELD at T=96 (14.0 -> 4.8 km,
min y 946 = the same floor as without the hold); Chase red air mover 700 u/s LOCK-HELD at T=140 (8.0 -> 4.8 km). Regression `test-autopilot-kill.ps1`:
two new LOCK-HOLD cases pass, all previous cases unchanged (the known 220 u/s mover fail is pre-existing). The game's own lock range and cone are not known
[I]: the census lines decide.

### 18.3b Subtitle brightness under a tone map or exposure (review F6)

The subtitles are drawn after the game's tone map (display values), but the HUD composite applies exposure and the tone map to every HUD texel: Reinhard
halved white text, exposure != 1 rescaled it. The HUD image has no per-pixel flag, so the late text carries one in its alpha. A late text draw is replayed
under a private clone of its PSO (created with the key; exact VS 534f29ca / PS d34ee153, blend SRC_ALPHA / INV_SRC_ALPHA, full write mask) whose alpha blend
is ONE / ONE / REV_SUBTRACT: RGB blends as the game's, alpha becomes `a_dst - A`, negative over the empty subtitle band. The consumer (`xr_stereo_blit.hlsl`
mode 1, `shade_hud`, the PNG mirror) reads alpha < 0 as a display value: clamp only, alpha = saturate(-alpha). The clone is used only while the consumer's
tone map is not clamp or the exposure is not 1 (`hud_late_marker_flag`); under the defaults the game's PSO is replayed and nothing changes bit for bit.
Limits: text over non-empty HUD texels loses the marker coverage; a (never seen) negative game alpha reads as a marker. Test: 18.4.

### 18.4 Tests

`hud_late_offline` (new, registered in CMakeLists; MSVC /W4 standalone in D:\b-radio: 40 checks pass): text class vs tracer / flare / wedge / radar mask,
contact, NOT_EQUAL / unknown shaders, the decision matrix, sums and the status line (<= 440 bytes: 396 max), stage switch plumbing, `hudlate` parse.
F6 (18.3b): `test_marker` (clone selection, D3D12 factor numerics, default gating; 312 checks in all) and `xr_stereo_output_offline` `hud_late_display_marker`
(Reinhard + exposure 0.8: white text composites to the game white within 1/255, unmarked texels still tone-mapped, coloured / edge / overlapping texels,
defaults unchanged).
`marker360_offline` + `test_census_aim` (cloud colour never triggers, zone edges, diamond is not a box, near-box filter on the step146 instrument centres, the
boxes line): 14607 checks pass (this also ran the section 17 tests for the first time). `tactical_offline` 352 pass. `d3d12_hook.cpp`,
`xr_flight_stage.cpp`, `xr_stereo_proxy.cpp` compile to objects (MSVC /W4, no new warnings). Not run: the DLL link, the game, the headset.

### 18.5 Null-run steps (the lead schedules them)

Config: the current day install + inside `xr_terrain_stage` `;hudlate:1;mk360cen:1` (remove `mk360cen` afterwards).
1. Mission 02 (ground targets): autopilot `-Mode Kill -HoldLock 10 -Total 400` (or m03k air start: `-Mode Chase -HoldLock 10`). The autopilot log must show
   `LOCK-HELD tick=...`; the `tac-lock-census-boxes` / `tac-lock-census` lines around that tick name the diamond (its `cls`, `cnt`, `rgb`, `shape=` verdict,
   `box=` distance). `continuous-tac lock-shape=moved` > 0 = LOCK-SHAPE works; `moved:0` with a census = report the lines for a matched fix.
2. Radio chatter (any mission with cut-ins): `continuous-hud-late frames=..(text:>0);text-draws` grows during chatter, `refused` / `capped` ~0, `hud-end-draws`
   shows `T` where it showed `l`; PNG burst / mirror: the subtitle text on the HUD layer at the top, no doubled HUD, quiet frames unchanged.
### 18.6 runLOCK1 (step155, m03k, Kill `-HoldLock 10`): why no lock, and the fixes

* **Evidence [E]** (`D:\acz-evidence-archive\pov1\runLOCK1`): `continuous-hud-late frames=12718(text:4776);text-draws=241816;refused=0` (HUD-LATE active; no eye
  dump of a subtitle). Autopilot: `lock=0 HOLDLOCK=0/10` throughout. The only identified contact was green (`id0.29,1.00,0.22:g0`), met HEAD-ON
  (T=52: rng 9162, brgErr 0.4, elvErr 0.6, one sample, then rng 1441 two seconds later at ~2000 u/s closing), then 120-180 degrees behind; the overflight
  BREAK (`near < 3500`, |he| > 1.6) and PULLUP alternated; the pitch was `CEIL pt=-0.050` for the whole flight (y 20000 -> 12000 > `AltCeil` 12000) so the
  target's 10-19 degree elevation error was never flown; from T=78 the aircraft struct read zero (`BLIND`, dead reckoning).
* **Colour [E + I]**: green is an enemy box colour, not a friendly: `team_of` makes blue the friend (FRND, the wingman `id0.21,0.23,1.00:g4`); in the step143
  headset log red identities sit near the nose (72 of 81 in-window red contacts at |bearing| < 10 deg) while green ones rarely do, and the user's 8.png shows
  the red box with the seeker diamond: the game draws an enemy box green and red while it is being locked [I]. Kill's `-TargetColors green,yellow,red,unknown`
  (white air contacts excluded) is right; the wingman at r 300 (white) was never picked.
* **Fix (autopilot `holdAir`)**: while a hold is pending on an AIR target: the Chase trail guidance (aim behind it inside 8 km, so the pass ends behind it, not
  head-on), no overflight BREAK, pitch on its elevation (`HOLD-elev`, -0.25..0.20 rad) even above `AltCeil` (FLOOR still first), 1 s decisions inside 20 km;
  the reticle window stays `-LockRange` (9 km) for air (the 14.4 km extension is for ground passes only). Sim (`-Stage m03k`, green air enemy): head-on then
  turning 0.03 rad/s LOCK-HELD T=108; crossing 0.02 rad/s T=165-207; flying away left T=143; a target flying straight out of the box at 900 u/s is not caught.
  Regression `test-autopilot-kill.ps1`: three m03k LOCK-HOLD cases added, exit 0 (31 pass; the 220 u/s mover is the known pre-existing fail).
* **Eye shots**: the live autopilot writes `%LOCALAPPDATA%\acz-vr\png-eye.now` + `png-screen.now` at 3 s of hold (diamond moving onto the box) and at
  `LOCK-HELD` (`LOCK-SHOT ...` log lines, tap summary `lockshots=N`; `-NoLockShots` = off). The producer requests one eye dump at a subtitle ONSET (>= 8 text
  draws after >= 30 frames with fewer; at most 3 per session, >= 20 s apart): `step5-producer` `hud-late-onset;frame;text-draws;shot`, consumer
  `png-eye-armed;...;trigger=hud-late-onset(subtitle)`. Both need `xr_png_once=true` (FLIGHT-DUMP re-arm; a request waits until the previous dump finished).
* Next null run: same cfg (`;hudlate:1;mk360cen:1`, `xr_png_once=true`), m03k, `-Mode Kill -HoldLock 10 -Total 400`. Check `LOCK-SHOT` / `LOCK-HELD` in the
  autopilot log, the eye PNGs at those ticks, `tac-lock-census-boxes` lines near them, `continuous-tac lock-shape=moved`, and the subtitle eye PNG after the
  first `hud-late-onset` line. The game's own lock range/cone is still unknown [I]: a hold counted by the controller is not proof of a game lock; the
  red box / diamond in the eye PNG is.
### 18.7 runLOCK2 (step156, m03k, Kill `-HoldLock 10`): the 12 census frames, the layer dump, the hold

* **Census [E]** (`D:\acz-evidence-archive\pov1\runLOCK2\logs\step5-producer.log`, 32 lines, ticks 212182140-212193093, i.e. the 11 s before `LOCK-SHOT` at
  212195000): every frame holds one or two 16 x 24 GS GREEN boxes (`m0/m1` = the two 16-GS edges 24 GS apart, rgb 0.29,1.00,0.22; contacts 7/8
  `win+box ... id0.29,1.00,0.22:g0`, 10.5-23 km, bearing 3.5-25 deg), their label draws, and in the last two frames (f 6255, 6315) one 3-vertex
  non-indexed green instrument at GS 1893-1964 x 1935-2001 (`shape=few-vertices`). That instrument maps to window pixels ~474-808 x 335-534 of the
  flat game: the large green TARGET-DIRECTION WEDGE that points from the SCORE box toward the game's SELECTED target when it is off-screen (flat shots
  `gameshots\g0018-035158.138.png`, `g0019-035201.155.png`). No red box, no 45-degree square, nothing else near the boxes: **the lock diamond is not in
  the census, and the game did not lock.** The box ahead has no type / range label (the selected target carries one, 8.png `F-4E 2185`) and the wedge
  pointed elsewhere: the target the autopilot held in the reticle was not the game's selected target, and ACZ locks only the selected target [I, strong].
  The shape test is unchanged (no evidence of the diamond's geometry yet). Census lines now also carry `topo=<primitive topology>;v=x,y/x,y/...` (first 4
  vertices) so the next small shape can be drawn from the log.
* **Colour [E]**: the whole HUD turns red during MISSILE ALERT (g0054 `MISSILE ALERT`, user shot 12.webp): red HUD colour is not a lock signal by itself.
* **LAYER-DUMP (`xr_png_once`)**: every eye dump now also writes `...-f<id>-hudlayer.png` and `...-f<id>-markerlayer.png` (the HUD quad image and the marker quad
  image of the same frame, rendered once more into a private target of the HUD layer's size and read back with the eye images; the marker image WITHOUT the
  NBOX stamp cut-outs, so every game box / diamond of the frame shows), and the `-final.png` eye image now includes the per-eye tactical markers and the NBOX
  stamps. The `png-once` line gains `layers=N(WxH)`. Same triggers as before (png-eye.now, LOCK-SHOT, hud-late-onset). The existing producer burst
  (`gpu_copy_step5_png_once` + `png-burst.now`: hud-on-black / hud-over-world) needs another runtime key and has no marker image.
* **Hold (autopilot)**: (1) target switch: once the held track has been in the reticle 1 s and is not red, tap `-SwitchKey` (default `I` = PCSX2 Pad1
  Triangle, ACZ "change target" [I: AC-series default control, not verified live]) at most 3 times per track, 4 s apart (`TSWITCH#n`, tap summary
  `tswitch=N`; `-NoSwitch` = off); (2) maintain cone: after 1 s inside `-LockDeg` the hold continues while the target stays within `-HoldDeg` 20 deg
  [I: ACZ keeps a lock inside its seeker area]; (3) `-HoldAspect 15000`: target score + 15 km x cos(aspect) for a track flying toward us (prefer one we can
  come up behind); (4) `-HoldBank` (default 0.45 = unchanged; 0.6-0.75 made the sim holds worse). Sim (`-Stage m03k`, green air enemy): head-on turning
  0.03 rad/s LOCK-HELD T=96 (was 108), crossing T=207, away T=143; the LOCK2 shape (head-on merge at ~1150 u/s, target turning 0.05 rad/s) peaks at 7.3 s
  (was 2.1) and is a KNOWN case in `test-autopilot-kill.ps1` (exit 0, 31 pass). A same-speed turning target is not caught in 400 s. A 10 s hold needs a
  geometry the AI may not offer; the census and the layer dump only need the game's lock (seconds).
* Next null run: same cfg + `xr_png_once=true`, m03k, `-Mode Kill -HoldLock 10 -Total 400`. Check `TSWITCH` lines and whether the held box gains a label /
  turns red after it (mk360-contacts `:g>0` / `id1.00,0.00,0.00`), `tac-lock-census` lines with `topo/v` near the lock, `continuous-tac lock-shape`, and the
  `-hudlayer` / `-markerlayer` PNGs at LOCK-SHOT and at each `hud-late-onset` (subtitle on the HUD layer). If `I` is not the switch key in this setup, the
  flat game shot shows it (a view change = Triangle held is the look-at-target camera) and `-SwitchKey` takes another letter.
## 19. PROV: provisional box for contacts the game never boxed (headset test 7, step160, 2026-10-31)
Evidence (headset-test7-20261031 xr-stereo.log, 47 summaries): tracks=5 (identified 3, identified-outside 3); the mk360-contacts lines show contacts 10 and 11 (v 1100-2000 u/s, r 6-7 km, a few degrees off the nose) as "none" (never matched to a game box: no identity, no stamp cell, c-) from t 160 s to the end. Mode 3 draws only the game's own pixels and collect() returned at once for mode 3, so a contact the game never boxed got nothing (the two circled aircraft; section 17 (d) lists these as by-design "never boxed"). Not the cause (current code re-checked): pairing (pairs=1519, multi=1519), budget (no-cell=0, no-atlas=0, rejected=0, cells=3/24), near fade (faded:10, hidden:0), census (off). The game was paused from t=175 s (all contacts frozen, v0), so the screenshot shows a frozen scene.
Fix (marker360.h collect_prov, every mode): a track seen now, never identified (Track::ever_ident false; set where an identity is confirmed or taken over from a ghost), tracked >= 10 frames, speed 75..2500 u/s (no static objects, no missiles: section 17 (d) iii), within 31,000 u of the camera (the game's measured box range, 17 (d) ii), with no game box matched this frame (sprite < 0), gets one unlabeled TGT-green stroke box at its true position. mk360p:0 disables it (tac-config shows mk360p=0 only then). mk360u:1 keeps owning the unidentified in modes 1/2. summary-mk360 drawn= gains prov:N. Team is unknown for these: a friendly the game never boxed also gets a green box (accepted). Test: marker360_offline test_provisional_box.

## 20. IFF for the provisional box: friendly, enemy, unknown (2026-10-31, follows section 19)

Offline work only (no game, headset, DLL build or install). Tags: [E] evidence, [I] inference, [G] guess.

### 20.1 What the logs can say about a contact's team [E]

Source: the `mk360-contacts` lines (id, in/out of window, range, bearing, elevation, speed, identity colour, glyphs) of `headset-test7-20261031` (xr-stereo.log + .prev),
`step164\batch\runAGB1` and `runAGB2`, plus `headset-2026-10-30-session\live-2210-step143` for volume (step137 has no contact lines). 128 tracks in all (t7 9, t7.prev 58, AGB1 7,
AGB2 9, step143 45): 10 boxed friendly, 41 boxed enemy, 2 whitish, 75 never boxed.

* **The game's box colour is a clean team predictor.** Blue (0.21,0.23,1.00) with the 4-glyph FRND label (220 samples; 26 more as the label-less piece g0) is the only friendly colour;
  green (0.29,1.00,0.22; ~250 samples, g0/g3/g4/g5/g9/g14) and red (1,0,0; ~75 samples) are enemy / target colours. Contacts that ever showed both a blue and a non-blue box: 0 of 51.
  (`tactical.cpp team_of` already implements this rule.)
* **Candidate sources the logs cannot test [E: absent from every log]:** `ObjDraw::sig` (mesh signature) and the aircraft records are never logged per contact (tactical.md section 6:
  models are not identified, only kinematics), and the radar blip colours are not read: the radar contacts are replayed only with `gpu_copy_step5_hud_radar`, never decoded per contact
  (radar-markers-game-original.md section 2). Neither is a usable team source today; a radar reader is a separate, live-verified piece of work.
* **Formation with the player.** 7 boxed friendlies with >= 5 samples (the wingman in each run: t7 1, AGB1 1, AGB2 1, step143 10 / 38 / 58 / 59) against 19 boxed enemies with >= 5 samples.
  Test = range <= 2500 u and range-rate < 200 u/s over consecutive 5 s samples (the logs carry no player velocity), >= 6 consecutive samples (30 s): 6 of 7 friendlies (the miss, step143
  58, is a friendly 10 km away) and 1 of 19 enemies (step143 17, a tail-chaser: 9 consecutive samples at r 684..2500 u). Per sample: friendly 165 of 320 pairs in formation, enemy 36 of 375.
  Range alone does not separate them (an enemy's closest pass was 300..587 u, the wingman's 122..353 u). An unboxed contact that is in formation: step143 1 (201 u minimum, 8 samples) = the
  wingman before its box was confirmed; t7 10 / 11 and the AGB2 contacts never were.
* **Class prior of the never-boxed rest [E]:** non-formation boxed contacts: 3 friendly (t7.prev 71 and 97, step143 58), ~38 enemy.

### 20.2 Rule (marker360.h `collect_prov`, `update` step 2b; `Params::prov_unknown`, `form_*`)

Team of a provisional box, in order:
1. **Game colour** (`Track::team`, set where an identity is confirmed or taken from a ghost = `team_of(box rgb)`): a contact the game boxed BLUE and then stopped boxing in view (the
   section 17.2 "hit or miss" miss) now gets a blue box, even though `ever_ident` is set. A boxed ENEMY that loses its box (kill / retarget) still gets none (section 19).
2. **Formation friend** (`form_friend`): dwell time `form_s` grows while the contact is within `form_range` 2500 u of the game camera AND its velocity differs from the camera's by <= `form_rel`
   300 u/s (the camera velocity is derived in `update`, `Tracker::cam_v`; invalid after a > 0.5 s gap, a > 12,000 u/s jump or a missing camera), falls at half rate otherwise, latches at
   `form_min_s` 30 s, unlatches only at 0.
3. **Unknown**: `form_s > 0` (a formation candidate, not yet confirmed) or no valid camera velocity. Default: a NEUTRAL unlabeled grey box (0.75), so the aircraft is marked but no team is
   claimed; `mk360pu:0` skips them. Evidence favours neutral: the user's complaint is wrong colours (D-355) and "aircraft with no box" (headset test 7) at once, and for a candidate the
   logs cannot tell a wingman from a tail-chasing enemy (step143 17) for its first 30 s.
4. **Presumed enemy**: every other never-boxed moving contact (the section 19 rule): the unlabeled TGT-green box. This is a prior, not a measurement: ~93% of boxed non-formation
   contacts were enemy-coloured; the 3 friendly ones are drawn green if the game never boxes them.

Friend box colour = the game's FRND blue, unlabeled (a "FRND" text would claim an IFF we do not have). The game's own colour always wins: a formation friend the game then boxes red becomes
an enemy. Not done: nothing is learned across runs, no radar reader, no mesh table.

### 20.3 Options, logs, tests

`mk360pu:0|1` (default 1 neutral; shown in `tac-config` only as `mk360pu=0`). New line `summary-mk360-iff` (next to `summary-mk360-hs6`, mk360:3): `prov-friend/enemy/unknown` (cumulative
boxes drawn), `unknown-policy`, `unknown-skipped`, `formation-confirmed`, `now=friend:,enemy:,unknown:` (tracks not boxed by the game), `cam-v=ok|none`.
Tests (`marker360_offline`): `test_iff_formation_friend` and `test_iff_learned_team` FAIL on the section 19 code (formation friend drew enemy green; a boxed-then-unboxed friend drew nothing),
`test_iff_unknown_candidate` (neutral / skip, 30 s latch, iff line), `test_iff_presumed_enemy` (head-on and 600 u/s fast contacts are never formation), `test_iff_box_overrides_formation`,
`test_iff_camera_velocity` (teleport, missing camera), `test_iff_options`; 15,255 checks pass.

### 20.4 Live check list

1. A mission with a wingman the game does not box: `summary-mk360-iff formation-confirmed` >= 1 and the wingman's provisional box blue (mirror / marker layer), grey for its first 30 s.
2. An enemy the game never boxes: green; a friendly that is not a wingman and is never boxed will still be green (known residual).
3. `cam-v=none` for long stretches means the camera velocity is unavailable (cuts / dropouts): everything provisional is grey then.

## 21. Headset test 8 (2026-11-01, step178): provisional boxes doubled / wrong team / on destroyed aircraft / in the cockpit, lock diamond

Offline work only (no game, headset, DLL build or install; `marker360_offline` 15,344 checks pass in D:\b-boxes, the `.inl` edit is NOT compiled). Evidence: `D:\acz-evidence-archive\headset-test8-20261101` (xr-stereo.log capped at 19:54 = t 1920 s, step5-producer.log, ee-camera.log). Tags [E] evidence, [I] inference, [G] guess.

### 21.1 What happened to Pixy [E + I]
The contact lines of the first 17 min: contact 1/2 (`out`, v 1171-1185 u/s = the player's own speed, r 264-416 u, bearing -128..-94, elevation 14..21) is the wingman, never game-boxed until t 615 s (`id0.21,0.23,1.00:g4`). Timeline in `summary-mk360-iff`: grey `unknown` from 135 s (a formation CANDIDATE, 30 s rule) -> r 1331 / 2561 u at 150-155 s (she broke away: the dwell decays at 0.5/s outside form_range 2500 u) -> `now=enemy:1` 165-225 s = **green** (the user's first sighting) -> back in range, `unknown` again 240-265 s = grey/**white** (and two contacts on her: the parts of one aircraft tracked twice) -> `formation-confirmed=1` 270 s = **blue stroke box, not the game's**. 635-1916 s the game was paused (all v0, nothing drawn); the game's own blue box is on her from 615 s ("showing correctly"). [E] No object-data team source exists: `ObjDraw` carries only the model translation, the 3x3 and the mesh signature (`tactical_frame.h`), the ee-camera logs are camera/region scans (`region-scan`, `scan-near`, `tracking`), and nothing the producer reads holds an ally flag, team byte or roster; the radar blip colours are not decoded (section 20.1). So friendliness still comes from kinematics, only earlier.

### 21.2 Rules (marker360.h `collect_prov`, `update`; all in `Params`)
1. **No doubles.** A provisional box is refused (`refused=box`) when any game box group of the frame (any colour, a lock diamond too; `prov_box_gate` 28 GS or 1.5 x its half size + 4) lies on the contact's frame-time projection, with and without the learned offset; refused for a contact in the window when a box draw was unreadable (`f.box_unreadable`: that box may be the contact's); refused (`refused=hold`) for `prov_hold_s` = 2 s after the game last boxed it (a matched sprite or a held identity: lock-box blink, pending identity, the miss rule's 9 frames). Before, only `tr.sprite < 0 && !tr.ident` counted, so a box the tracker could not match to the contact (a second track took it, the 20 GS gate, a recolour pending 6 frames) drew our box beside the game's.
2. **Never green for a wingman.** QUICK-FORMATION: a contact first seen within `form_first_range` 1,200 u of the game camera and co-moving (<= `form_rel`) dwells 15x faster for its first 10 s: friend after `form_quick_s` 2 s (a wingman spawns beside the player; every boxed enemy in the logs was first seen far away). The latch is permanent while the track lives (the old decay un-latched Pixy when she left 2,500 u). A friend lost for any reason is remembered 90 s as a friend ghost (`fghosts`) and re-latched on a new unboxed contact within max(500 u, 0.25 r) of its offset from the camera (`reacq`). A contact first seen farther away keeps the 30 s rule (grey meanwhile). The game's box colour still wins over all of it. A friend whose game box vanished in view draws our blue box after the 2 s hold (it cannot be the game's own pixels: the game is not drawing them).
3. **One box per aircraft** (`refused=dup`): two provisional contacts, or a provisional one and a game-boxed contact seen now, closer than `prov_dup_deg` 1.5 deg at the camera AND `prov_dup_dist` 250 u are one aircraft (parts further apart than the 60 u cluster radius): friend > enemy > unknown, then the older track keeps the box. 800 u apart = two aircraft.
4. **Destroyed aircraft** (`refused=wreck`): evidence is thin [I]. A game-boxed (ever identified) track lost while frames kept arriving and no dropout since its last sighting leaves a death record (8, 1.5 s); a track born within `heir_gate` 300 u of the dead-reckoned death is its heir, and at its third frame it is a WRECK if its velocity differs from the dead track's by more than 0.35 x the dead speed (debris falls; the same aircraft re-tracked after a flicker keeps its velocity and is not a wreck). A wreck never gets a box; a boxed enemy that lost its box never did (section 19). Not covered: a wreck that keeps the same track with continuous position and velocity, and never-boxed aircraft shot down (nothing says they died: their draws just stop). The logs hold no kill sequence to measure a better signal (no per-contact mesh signature, no explosion/fx link); `summary-mk360-iff wrecks=` counts hits.
5. **Cockpit guard.** [E] No contact sat at a stale or zero position in the log (every `mk360-contacts` range is 264+ u, none at the origin; `depth-held=0` all session, so not the stamp depth rule). [I] The stroke boxes are drawn after the cockpit with no depth test and had no near policy (the NBOX stamps had one, section 14): Pixy at 264-416 u behind/above/abeam put a grey box on the headrest and canopy rail, and a far contact behind the canopy is drawn over it. Now refused (`refused=bad`): non-finite or world-origin position, display vs frame position more than `prov_slip` 300 u apart (a wrong interpolation), a missing camera; refused (`refused=near`) within `near_fade` (75 u) of the game camera, with an alpha ramp 0..1 up to `near_min` (150 u) (`WorldMarker::alpha`; `mk360fade:0` turns the near refusal off). Not fixed [G]: a far contact whose direction lies behind cockpit structure is still drawn over it; that needs the eye coverage (depth > 0) as an occluder for the stroke layer, which the overlay slots do not have.
6. Log: `summary-mk360-iff` gains `refused=box:,hold:,near:,bad:,dup:,wreck:;quick=N,fghost:N,reacq:N,wrecks:N` (cumulative contact-frames; the line stays < 400 bytes).

### 21.3 Lock diamond: never moved, offset [E] / cause unproven
* `continuous-tac` the whole session: `frames=271027;flags=9` (On + LockShape, so the feature was ON); `lock-shape=moved:0,diamond-not-moved:0;census-frames=0` (mk360cen was not set). `moved` counts diamonds accepted AND added, `diamond-not-moved` counts diamonds that passed the geometry but could not be added (marker cap): both 0 means NO instrument draw ever passed `lock_shape_test`, whether or not a lock happened. The game did lock in that session (`win+box ... id1.00,0.00,0.00:g4` red boxes with a 4-glyph label at t 315-355 s), so the diamond was drawn on the HUD layer. The offset is section 17.4's known mechanism: a diamond left in the HUD layer is scaled by `xr_hud_scale` about the gunsight glass while its box is world-anchored.
* The evidence cannot say WHICH gate stopped it: the test only runs for an instrument draw after the frame's first box draw, not refused by the HUD replay, with >= 1 box marker, <= 48 indices, and then needs a 45-degree rhombus in the colour of a box within 64 x 48 GS. Also `summary-mk360` shows `box-facts-unsafe` on ~97 % of frames (the cloud-sprite draws classed as boxes overflow the per-draw cluster cap, `mark_dropped`, nearly every frame: that only disables the in-window miss rule, but it says the marker list carries junk clusters). Guessing the shape would repeat steps 143-156, so `lock_shape_test` is unchanged.
* **Added: LOCK-WHY** (`tactical_producer.h lock_why_index / format_lock_why`, wired in `gpu_copy_step5_hooks.inl`; not compiled here): a status line `...-tac-lock;tested=diamond:,few:,many:,offscreen:,size:,aspect:,corner:,inside:,tips:,nobox:,other:;not-tested=refused-draw:,no-marker:,too-many-indices:` beside `continuous-tac` whenever LOCK-SHAPE is on (no mk360cen needed). Read it after one held lock (>= 5 s, the red box on screen): `tested` dominated by `tips`/`inside`/`corner`/`size`/`aspect` = the diamond is drawn but is not our 45-degree outline (then run once with `mk360cen:1` and `xr_png_once=true` for its vertices (`topo=;v=`) and the `-hudlayer.png`); `nobox` = no box of its colour within reach; `no-marker` = no box marker in the frame when it was drawn (drawn before its box, or the box draw unread); `refused-draw` = the HUD replay refused it (`hud-markers refused=` says why); `too-many-indices` = more than 48 indices; `other` = vertices unreadable. What is needed to fix it: that one line and, if the shape is the cause, the census vertices.

### 21.4 Tests (`marker360_offline`, private D:\b-boxes; existing cases adapted: `test_iff_learned_team` runs 230 frames (the 2 s hold), `test_iff_unknown_candidate` starts beyond form_first_range)
`test8_no_double_beside_game_box` (an unmatched 24 GS box suppresses ours 30 of 30 frames, a 62 GS one does not, no box: 30 drawn), `test8_hold_after_game_box` (the blue box first appears 110-140 frames after the game's last, not before), `test8_quick_formation_never_green` (blue after 3 s with `quick=1`, never green over 90 s including drifting to 7,000 u; a first sighting at 2,100 u stays grey), `test8_friend_ghost` (lost 3 s, re-seen 90 u away: blue, `reacq=1`; a far contact is not), `test8_one_box_per_aircraft` (150 u apart: 1 box, 800 u: 2; a game-boxed neighbour takes the aircraft), `test8_wreck_gets_no_box` (debris 0 boxes, `wrecks=1`; a same-velocity heir is drawn), `test8_cockpit_guard` (origin, NaN, 500 u slip, 60/75 u hidden, 112 u alpha 0.5, `mk360fade:0`, iff line), `test8_lock_why` (every verdict text maps to its bucket, line format and truncation).

### 21.5 Live check list
1. A mission with Pixy: `summary-mk360-iff` `quick=1` within ~3 s of her first sighting, `now=friend:1`, never `enemy:1` for her; blue stroke box until the game boxes her, then only the game's (no second box: `refused=box/hold` rising at that moment).
2. Shoot down a boxed enemy: no grey/blue box on the wreck (`wrecks=` >= 1); report if a box still follows the debris (then the wreck kept its track).
3. A wingman or enemy passing the canopy: its box fades below 150 u and is gone below 75 u; far contacts behind the canopy frame still show over it (open).
4. Lock a target for >= 5 s and send `...-tac-lock` (section 21.3).

### 21.6 Lock diamond again (2026-11-01, step180 `boxes` null runs runAGB1/AGB2): the evidence has no diamond; LOCK-DUMP added (`mk360ld:1`)

Offline only (no game, no DLL build, `.inl` edit not compiled). `marker360_offline` 15,371 checks pass (D:\b-lock).
* **What the step180 logs say [E]**: `continuous-tac-lock;tested=diamond:0,few:3989,many:0,offscreen:6309,size:4209,aspect:963,corner:304,inside:37,tips:0,nobox:0,other:0;not-tested=...no-marker:721529...`. Neither run had `mk360cen` (cfg `mk360:3;fxmem:6;fxsig:f1a4e000`, no `xr_png_once`), so there is NO per-draw vertex data and no layer PNG. **No lock happened in either run [E]**: autopilot `lock=0` the whole flight (the green target stayed 150-180 deg off the nose, `brgErr`), the 4 salvo missiles were fired "WAITED (not level)" without a lock, no red box in any log, and the only red frames in `gameshots` (g0091-97, g0103-107, g0156-159) are the whole-HUD red of DAMAGE / MISSILE ALERT (section 18.7), no diamond. So `tested` is NOT a diamond rejection tally: `few:3989` = the 3-vertex target-direction wedge / HUD arrows, `offscreen`/`size` = frame-wide HUD pieces, `corner:304` axis-aligned HUD rectangles. Reading the buckets as "the diamond is rejected for size / aspect" is wrong; the diamond has not been seen.
* **What the code can say about the shape [E + I]**: the game's own box is ONE indexed draw, 9 indices, 8 vertices, topology 5 (strip), a 16 x 24 GS ring, outer corners first (`tac-lock-census` step143/146/156: `v=2092,2065/2108,2065/2092,2089/2108,2089`). The diamond in `docs/headset-tests/2026-10-29-user-enemy-air-box-lock-diamond.png` is the same thin red outline rotated 45 degrees and about the box's size [I]: the natural draw is the same primitive (8 vertices, outer tips + inner tips). `lock_shape_test` already accepts exactly that (new `test_lock_dump`: that ring rotated, half 8-16 GS, thickness 0.75-1.5 GS, accepted; the unrotated ring is the game's box, refused as `corner`). Not covered, unproven: a diamond drawn as 4 separate line / quad draws (each 2-4 vertices, would show as `few`), a diamond drawn BEFORE its red box in the frame (`no-marker`), one farther than 64 x 48 GS from its box during acquisition (`nobox`; the reference shot shows it 17 / 19 GS off the box when locked, the seeker may start farther). Changing the test blind would repeat steps 143-156, so `lock_shape_test` is unchanged.
* **Added: LOCK-DUMP `mk360ld:1`** (any mk360 mode; producer flag `kProducerLockDump` = 32; `tactical_producer.h lock_dump_*`, `format_lock_dump`; `gpu_copy_step5_hooks.inl step5TacLockDump`; described as `;mk360ld=1`). In a frame whose marker list holds a RED box (r >= 0.6, g, b <= 0.4: the game's targeted / locked colour; the whole-HUD red of a missile alert does not trigger it), every instrument draw that was not moved and whose bbox is 6..90 GS each way writes ONE `step5-producer.log` line per signature (reason, vertex count, index count, size/2): `tac-lock-dump;f;ord;why;after-box;vs;ps;idx;cnt;topo;nv;bbox;half;rgb;v=x,y/x,y/...` (all unique vertices up to 16), <= 60 lines in all, <= 24 reads per frame, plus `continuous-tac ...;lock-dump-lines=N`. `why` = the diamond test's verdict (`size`, `aspect`, `corner`, `inside`, `tips`, `few-vertices`, `no-box-in-reach`, ...) or `refused-draw` / `too-many-indices` (not tested). Limit: a diamond drawn before the red box of its frame is not dumped (the trigger needs the box already in the list); `mk360cen:1` still logs by position.
* **runBatchAG options for the next null run** (needs a game lock: use the Kill autopilot that held a target in the reticle, or the user's own flight): run.cfg `xr_terrain_stage` value `...;mk360:3;mk360ld:1;mk360cen:1` and `xr_png_once=true` (the `-hudlayer.png` / `-markerlayer.png` at LOCK-SHOT show where the diamond is); autopilot `-Mode Kill -HoldLock 10 -Total 400` on m03k as in 18.6 / 18.7 (`-NoSwitch` not set). Read: `grep tac-lock-dump step5-producer.log` (the diamond is the line near a red box with `half` about 8-16 GS and `nv` 4-16; its `why` names the gate), then `...-tac-lock;tested=` after the lock, then fix `lock_shape_test` from those vertices. Remove `mk360ld` / `mk360cen` afterwards.
* Tests: `test_lock_dump` (rotated box ring accepted at 3 sizes x 3 thicknesses, the unrotated ring `corner`, red-box trigger (green / empty / moved-diamond frames do not trigger), signature dedupe and table bound, line format < 426 bytes with 16 extreme vertices and a 40-byte buffer, option parse / flag bit / describe / mk360-off / bad value).

## 22. Headset test 9 (2026-11-02, step182): fewer boxes, one box outside the window, other-green far boxes, duplicates behind, boxes on wrecks, neutral boxes

Offline only (no game, headset or DLL build). Evidence: `D:\acz-evidence-archive\headset-test9-20261102` (xr-stereo.log, step5-producer.log; cfg `mk360:3`, no
`mk360p`, so PROV was on), test 8 `headset-test8-20261101`, null runs `step180\boxes\runAGB1` and `step182\eyeobjbox\runAGB1`. Tags [E] evidence, [I] inference.

### 22.1 Findings per complaint
* **"Regressed / not on all aircraft" [E]: no change in the game-box (NBOX) code since step160; the null runs step180 vs step182 give the same nbox counts
  (live 5517 / 5519, cached 27981 / 27247).** In test 9 every identified out-of-window contact with a cached picture was drawn each frame (`cached` per frame =
  the identified-outside count of `mk360-contacts`, e.g. 5.0-5.5 per frame at t 555-615 s with 5-6 such contacts). What was missing: (a) contacts the game
  never boxed in its window (44 aircraft-like samples; no native picture exists for them), (b) pairs of boxed contacts (below), (c) identities never cleared (below).
* **"Outside the rectangle it only renders one" [E]: two enemies close together in the window cut each other's stamps on every frame**, and a cut picture was
  never cached (w12-2 rule), so only the one seen alone at some time had a picture outside. t 125-170 s: contacts 5 and 6 (b -9.3 / -8.6, e -4.4 / -1.8: ~6 GS
  apart, ~30 GS one above the other) both `win+box`; 6 got `c2`, 5 stayed `c-` (identified, nothing drawn outside: `out-uncached` 56 -> 2570 in those 45 s).
* **"Far targets a different shade of green" [E + I]: our provisional stroke boxes** (PROV, flat TGT green 0.29,1,0.22 lines over contacts the game never boxed,
  out to 31,000 u; `prov-enemy=7826` contact-frames) next to the game's own pixel boxes. Plus [E] game pictures drawn far beyond the game's box range: 60 of
  323 identified out-of-window samples beyond 31,000 u, up to 204,993 u (contacts 16 / 56 yellow, 23 / 55 / 89 red); the in-window game boxes never exceeded
  32,354 u (110 samples, p99 28,212), and contact 55 sat IN the window at 192,650 u with no game box while keeping its red identity.
* **"Duplicates behind me" and "random neutral boxes" [E]: whitish groups became identities.** At the m03k event a group of 4 contacts at 3,200-5,900 u/s within
  ~200 u of each other (test 9 t 505-515 s, null step180 t 355-360 s) matched whitish groups (1.00,0.96,0.84 / 0.99,1.00,0.91); afterwards contacts 38-41 were
  four whitish boxes ~2,000 u BEHIND the player (b 165-177 / -165..-177), and contact 50 carried a whitish box 3,700-7,700 u away for 45 s (t 555-600 s). The
  game's box colours are blue, green, red, yellow (sections 2, 20.1); white has no team (`tac::team_of`). Plus [I] PROV boxes beside game boxes (the user's
  screenshot: a red "F-4E 260" game box with a dim green unlabeled box beside it, the stroke box of a second contact the dedup did not join).
* **"Destroyed planes suddenly get a green box" [E + I]**: (1) PROV: debris tracks are new never-boxed contacts = presumed-enemy green (`wrecks=0`: the
  heir test caught none). (2) The game's own picture followed wrecks because **the in-window miss rule never ran**: `box-facts-unsafe` 26,320 of 33,498 frames
  (test 9), 20,840 of 20,840 (step180 null), 13,118 of 13,835 (test 7), while the frames held ~1 marker on average (`sprites=34268` in 33,498 frames). Cause
  [E, code]: `step5TacMember` restarts the HUD tail (`nmark = 0`) but kept `mark_dropped` / `box_unreadable` from the discarded segment (a per-draw cluster
  overflow there fills the list to 16, so the drops cannot be in the final list). A contact the game stopped boxing in view (kill, retarget) kept its identity, and
  its cached box was drawn on the wreck once outside the window.

### 22.2 Fixes
1. **PROV off by default** (`mk360p:1` opts in; `tac-config` then shows `;mk360p=1`, the default line is unchanged; `mk360pu` applies only with it). Only the
   game's own pictures are drawn.
2. **Producer** (`tactical_producer.h restart_marker_tail`, called by `step5TacMember`): the tail restart also clears `mark_dropped`, `box_unreadable`,
   `label_unreadable` (the status totals keep the counts) and `lock_shapes`. The miss rule (9 frames / 140 ms without the game's box inside 92 % of the window)
   now runs on frames whose final marker list is complete: a kill or retarget in view drops the identity and the picture.
3. **Colour gate** (`box_colour_ok` = `team_of` not unknown): a whitish group never gives an identity, never recolours one and is never cached; in the window
   it stays the game's own live pixels (`pale` counts matches).
4. **Range gate** (`Params::box_range` 33,000 u, 0 = off): a cached picture is not drawn beyond it (the identity is kept for a return; `far` counts). Same in
   modes 1/2.
5. **CUT-CLEAN**: a cut stamp caches the part of its picture no other stamp's picture touches: box + whole label (q 2) or the box alone (q 1, LABEL-CORE),
   else nothing (a truncated label is still never cached). When two pictures overlap only in their 2 GS margins the cut runs in the gap between them instead of
   the midline, so both keep their whole picture (no pixel in two stamps, as before).
6. Log: `summary-mk360-hs6` gains `pale=;far=;cut-cached=;cleared-in-view=;unsafe=` (the last two repeat summary-mk360 fields that the line cap cuts).

### 22.3 Tests (`marker360_offline` 15,437 checks, private D:\b-t9; `tactical_offline` 352; `gpu_copy_step1_offline_tests` builds with the `.inl` change, fixtures `step5-tac`, `step5-hud-markers(-off)` pass)
`test9_prov_default_off` (default options: no box of ours on never-boxed contacts), `test9_pale_never_identity` (a whitish contact gets no identity / cache and
nothing behind the player; a red identity crossed by a whitish group stays red with its picture), `test9_far_cached_not_drawn` (no picture beyond 33,000 u,
back within range it returns; `box_range` 0 = old behaviour), `test9_cut_pair_both_cached` (the test-9 pair: both boxes cached q 1 without the other's pixels,
both drawn behind the player; labels clear of the other box: both whole q 2), `test9_restart_marker_tail` (counters; a kill in view with a pre-restart drop:
before = the box follows the wreck, after = identity cleared, nothing drawn). Adapted: `test_provisional_box` / `test_iff_options` (opt-in), `test_w12_cut_label_not_cached`
(the truncated label is still never cached; A's clean box may be).

### 22.4 Only the headset can confirm
1. Every boxed aircraft carries its game box (shape, colour, text) when the head turns, including pairs flying close together; `hs6 cut-cached` > 0.
2. No box on a wreck after a kill in view (`hs6 cleared-in-view` grows at kills; `unsafe` well below `frames`). Report a box that blinks off in view on a
   live target (the miss rule now runs: a target the matcher loses for 9 frames would lose its picture until boxed again).
3. No whitish / neutral boxes behind the player after the m03k event (`pale` > 0), no far boxes (`far` > 0 while targets fly away).
4. A kill OUTSIDE the window still keeps the box on the wreck until its draws stop (> 0.25 s): unchanged, nothing reports that kill.
