# TACTICAL: radar blips, markers outside the window, cosmetic gun tracers (`tac:1`)

Headset test 17 (D-330/D-331): (a) the HUD radar has no blips, (b) no machine-gun fire is visible, (c) FRND/TGT markers exist only inside the
original 63x38 deg game window. This track builds a tactical display from data the mod already has, behind default-off options INSIDE
`xr_terrain_stage` (no new config key; the installed config stays at 31 of 32 keys).

Tags: **[E]** evidence (measured, logged or tested here, with the source), **[I]** inference, **[G]** guess. Offline only: no game, SteamVR or
headset was run for this work. Nothing here is verified in the game.

## 1. Switches

**D-355 change (headset test 18):** our boxes doubled the game's, outlived kills (15 s coasting) and had wrong colours; our blips were wrong (guessed
range, also over the expanded map). `tac:1` alone now gives the cosmetic tracers ONLY; our boxes and blips are opt-in and the game's markers are never
suppressed unless our boxes are on. The game's own radar contacts: `docs/world-render/radar-markers-game-original.md` (`gpu_copy_step5_hud_radar` +
scene-barrier resume).

**D-362 follow-up:** the game's boxes around the whole sphere are `mk360:1|2` (independent of `tac:1`, replaces `tacboxes:1`; no coasting, identity only from
the game's own boxes): `docs/world-render/markers-360.md`.

`xr_terrain_stage=<stage>;...;tac:1` turns the tactical producer and the tracers on. Sub-options (all optional):

| option | default | meaning |
| --- | --- | --- |
| `tac:1` | off | the tactical display (needs the terrain render: it uses the terrain camera) |
| `tacradar:1` | 0 | our radar blips (off by default since D-355) |
| `tacboxes:1` (alias `tacmark:1`) | 0 | our world-anchored markers AND suppression of matched game marker sprites (off by default since D-355) |
| `tacgun:0` | 1 | no synthetic tracers |
| `tacsup:0` | 1 | with `tacboxes:1`: never suppress the game's own marker sprites (doubles possible); without `tacboxes:1` nothing is suppressed anyway |
| `tacr:<u>` | 10000 | world units at the radar's THIRD ring: **a guess** (section 4) |
| `tacself:<u>` | 80 | world draws within this of the game camera are the player's own aircraft |
| `tacclu:<u>` | 60 | world draws within this of each other form one contact |

Needs the producer keys already in the installed config: `gpu_copy_step5_world_objects` (the replayed object draws), `gpu_copy_step5_hud`
(HUD target, shader identities) and `gpu_copy_step5_hud_markers` (box/label classification). Example (driver): append `;tac:1` to the
`-TerrainRender` value of `tools/live/vr-test-start.ps1`.

Key off (no `tac` item): `acz::tac::extract_options` returns the value unchanged [E test], the producer flags stay 0 so no producer function
does anything [E fixture `step5-tac` and every existing producer fixture], `StereoFrame::tac` is nullptr, `BlitJob::tac` is nullptr and the backend
creates nothing [E tests]. 305 pre-existing tests unchanged.

## 2. Data flow

Producer (game render thread, `gpu_copy_step5_hooks.inl`, functions `step5Tac*`; pure helpers in `tactical_producer.h`), per producer frame:

* **Objects**: every ACCEPTED (= replayed) DrawACM_v world draw appends `{t = record +256 B translation (floats 76..78), 3x3 rows, mesh
  signature}`; the frame's `A` (record +0, world -> PS2 screen) is kept from the first one. Same read as `world_dedupe.h`, whose translation
  matched a .pdl building to 0.49 u [E world4-impl.md]. The draws are exactly the ones the user sees outside the window (WORLD-1 shared camera).
* **Markers**: each HUD-tail draw classified `box` / `label` by `gpu_copy_step5_hud_markers` is READ (vertex buffer and, if indexed, index buffer
  of the draw, through `step3MappedSpan_locked` + SEH-guarded copies: only pinned, mapped UPLOAD memory, at most 512 vertices). ps2_v vertices
  are 4 x float4: ATTRIBUTE0 = pre-projected GS screen x, y (2048 +- 256 / 2048 +- 224), ATTRIBUTE1 = colour [E stored-draw-manifest-1.md 3.1].
  Box vertices are clustered (single linkage, 20 GS units; one box = one marker [I]); label vertices (10 GS units) give a glyph count per box
  (4 quads = "FRND", 3 = "TGT" [I]). Each box is matched to the nearest replayed draw whose translation, projected with `A`, lies inside the
  box (x1.5 + 4 GS units) [I: the box is centred on its object].
* **Suppression**: when EVERY marker of a box draw matched a replayed draw (and `tacsup` is on), that box draw and the label draw after it are
  not replayed into the marker layer: the consumer draws those markers itself. Otherwise the game's draw is replayed unchanged (one game draw
  holds several boxes, so suppression is all-or-nothing per draw) and the consumer does NOT draw its own marker for those contacts this frame.
* **Firing wedge**: a draw with VS 534f29ca, PS d34ee153, non-indexed 4 vertices, blend on SRC_ALPHA/ONE/ADD, depth GREATER without write.
  [E D-318 step69: exactly this key appears while the gun is held, one per frame, and ALSO while a missile is fired.] [E this work, step69
  run1+run4 draw dumps: among ~5,500 dumped draws of these shaders with 4 vertices, the additive + GREATER state appears ONLY in the four
  firing-window frames 2339, 3559, 4071, 6833; the ordinary quads of the same shaders are blend 5/6.] Counted between members (`wedge`) and after
  the last member (`wedge_tail`); firing uses either.
* Status line in `step5-producer.log` every status period: `...-tac;frames=;avg-objects=;avg-markers=;markers-matched=;box-draws=;label-draws=;
  suppressed=boxes:,labels:;unreadable=boxes:,labels:(last=<why>);wedge-frames=;wedge-tail-draws=;flags=`.

Consumer (XR owner thread, `Output::tac_frame`, `tactical.cpp`), per consumed producer frame, only with the terrain camera of that frame:

* **Tracker**: draws within `tacself` of the camera are dropped (own aircraft); the rest are clustered (`tacclu`) into contacts; contacts are
  associated to tracks by greedy nearest pairs within a velocity-aware gate; velocity = smoothed displacement / time (frame publish time).
  A matched marker sets the track's team (blue-dominant colour = friend, other saturated colour = enemy, grey = unknown), its colour (the
  marker's own vertex colour) and label. Unseen tracks are dead-reckoned ("coasting", drawn dimmer) for 15 s (teamed) / 2 s (unknown).
* **Radar** (HUD image, slot `hud`): heading-up, centre = the player (game camera position), forward = the camera forward projected on the
  ground. Shown: every teamed contact (marker colour; arrow when moving, square when not) and moving unknown contacts (white arrows);
  static unknown contacts (buildings, props) are not shown. Beyond the box: a chevron on the border.
* **Markers** (eyes, slot `post`, drawn over the model cockpit like the game's markers): every teamed contact, world-anchored at its position,
  per eye, in every view direction where it is in front of the eye (no clamping; no off-screen indicator), box + stroke-font "FRND"/"TGT" at the
  game's angular size (box half 0.0185 tan, line 0.0016 tan, glyph 0.0135 tan, from the step62 shot), skipped when the game's own marker for it
  is drawn this frame (no doubles).
* **Tracers** (eyes, slot `pre`, drawn after the world composite and BEFORE the model cockpit so the opaque cockpit covers them): see 5.
* The backend (`xr_stereo_d3d12.cpp`, `ensure_tac`/`draw_tac`, shader `xr_tactical.hlsl`) draws the clip-space triangles from a persistently
  mapped UPLOAD buffer (24,576 vertices, root SRV, no input layout, no depth, premultiplied ONE/INV_SRC_ALPHA into the sRGB targets).
* Logs (`xr-stereo.log`): `tac-armed` (parse), `tac-config` (first frame), every 5 s `summary-tac` (tracks/teamed/friend/enemy/moving/coasting,
  self-draws, marker matches, blips, markers, game-marker skips, wedge/firing frames, fire events, tracers, overlay size, backend used/refused)
  and `tac-blips` (up to 6 blips: id, range, bearing, radar uv, shape, colour) for the range calibration.

## 3. Radar widget geometry (MEASURED)

`research/world-render-data/tactical_radar_measure.py` on three game-window shots (step62 `flight/...T0.16.png`, step71 `...T1.23.png`,
`...T4.83.png`; client 2404x1352 inside the window chrome; identical to 0.0002) [E]:

| quantity | client px | fraction |
| --- | --- | --- |
| box | (144, 873) .. (527, 1268) | u 0.0599..0.2192, v 0.6457..0.9379 |
| centre (box centre = ring centre = player arrow) | (335.6, 1071.4) | (0.13960, 0.79250) |
| rings (radius) | 29.2, 58.2, 116.0, 174.3 | 0.0216, 0.0430, 0.0858, 0.1289 of the height (ratio 1 : 2 : 4 : 6) |

Heading-up and the bearing convention are checked on two shots (`tactical_radar_measure.py --bearings`) [E]: the friendlies' radar arrows sit at
bearings -6.4, +6.5, +16.7 deg (step62 T0.16) and -12.3, -12.0, -23.3 deg (step71 T1.23) from the radar's up direction, against FRND box azimuths
of about -7.0, +6.4, +17.3 and -12.9, -13.1, -25.6 deg (box centre estimated as the blue "FRND" label's right end + 56 px; azimuth from the
0.6152 lens) [I for the box centre]: agreement within ~0.6 deg / ~2.3 deg.
The 90-degree wedge opens upward. Blips are drawn into OUR HUD layer (the game's rings/box are already in it from the HUD replay; the game's
own contacts never reach it: radar-region-end.md).

## 4. Radar range scale: NOT measured (guess)

The ring spacing is measured (above); the world distance per ring is not: no evidence holds a contact's world position together with its
game-radar blip. `tacr` (default 10000 u ~ 2 km at ~5.1 u/m [I terrain-calibration.md] at the third ring) is a GUESS. Calibration for the driver:
in one null run with `tac:1`, take a game-window screenshot while `tac-blips` lines are written; for a teamed contact compare its game blip
radius (px from the centre, /1352 = fraction of the height) with our logged range: `tacr = range x 0.0858 / fraction`.

## 5. Synthetic tracers: COSMETIC

Firing = the wedge present in >= 3 consecutive producer frames spanning >= 45 ms; released after >= 4 frames AND >= 150 ms without it [E test:
a single stray frame never fires; at 60 fps ON at the 4th frame, OFF after ~10; at the 15 fps of headset test 17 ON at the 3rd, OFF after 4].
While firing, two guns (illustrative offsets in the aircraft frame: +-6 u lateral, 5 u below, 10 u ahead of the eye) emit a visible tracer
every 60 ms each at the aircraft velocity + 5100 u/s (~1000 m/s) along the nose, gravity 50 u/s^2, life 1.1 s, drawn as ~70-unit yellow streaks
fading with age, world-anchored (they stay where they are when the head turns). They are NOT the game's bullets: no hit data, rate, dispersion
or ballistics from the game. Known false positive: a missile launch shows the same wedge [E D-318], so tracers also appear for ~3 s after a
missile launch.

## 6. What is classified how

| thing | how | evidence |
| --- | --- | --- |
| own aircraft | world draws within `tacself` (80 u ~ 16 m) of the game camera | [I] (the player's exterior may not be drawn in the cockpit view at all) |
| contact | draws within `tacclu` (60 u ~ 12 m) of each other | [I] aircraft parts carry their own matrices near the object |
| aircraft vs ground unit vs building | NOT identified by model; kinematics only: moving (>= 75 u/s ~ 15 m/s) vs static | [I] |
| team (friend / enemy) | the colour of the game's marker box matched to the contact | [E] FRND blue (step62); enemy colours [I] |
| label | glyph count of the label cluster (4 = FRND, 3 = TGT), else FRND for friends | [I] |
| radar colour | the matched marker's own colour (so whatever the game uses); unknown = white | design |

Model identification by fingerprint was NOT implemented: the baked models (`models-all`) are the 36 PLAYER cockpits; enemy/ally exteriors and
ground units are not baked, so there is nothing to compare the draws' vertex counts against. The producer records a mesh signature per draw
(`ObjDraw::sig`) for a later table.

## 7. Limits

* Team learning needs the game's marker for that object to have been visible INSIDE the game window once (and matched). Until then the contact
  is a white arrow on the radar (if moving) and has no marker.
* Contacts exist only while the game draws them (its own culling); behind the player they coast for 15 s, then vanish from radar and markers.
* Static enemy ground targets that never had a marker in view are not shown (indistinguishable from buildings without a team).
* Suppression is per game draw: when one box of the draw cannot be matched, every box of that draw stays the game's (in-window only), and
  ours are skipped for those contacts in that frame.
* Marker learning needs the sprite vertices to be readable (pinned, mapped UPLOAD); otherwise `unreadable=` grows, no team is learned and
  no suppression happens (radar: white moving contacts only; tracers unaffected).
* Range scale guessed (section 4); radar box/rings assume the 16:9 game image (HUD image = scene RT).
* No live verification of anything: the producer hooks only compile and pass fixtures; the GPU pass compiles (fxc) but the WARP harness does not
  draw it.

## 8. Tests

`tactical_offline` (341 checks): option extraction / key-off identity; marker clustering, labels, matching through a synthetic game `A`, the
wedge key against its look-alikes; tracker (self, clusters, association, velocity, team learning, coasting); radar pixels at 2560x1440
(contact at 10045 u north = 123.6 px above the centre (357.4, 1141.2); 5000 u east = 61.8 px right; heading east puts a northern contact on the
left with its arrow pointing left; beyond the box -> border chevron); markers for a head turned 40 deg off the window (a contact 40 deg left
lands in the eye centre to 1e-3, one straight ahead at 0.5 + 0.5 tan40/tan(0.87), behind = not drawn; box size and label placement);
firing debounce on synthetic draw logs (60 fps and 15 fps); tracers; no double markers; overlay cap. Producer fixture `step5-tac`
(gpu_copy_step1 fakes): the sprite read path runs on the fixture's pinned UPLOAD buffer without a fault (its vertices are not GS coordinates:
no cluster), the replay stream with `tac:1` equals the key-off stream when nothing is matched, no heap allocation on the HUD Draw path, the status
line, and the flags back to 0 give `tac == nullptr`; `step5-hud-markers-off` also checks `tac == nullptr`. Release build `D:\b-tc`, ctest 307/307 before and 310/310 after merging master 3fd6ab3
(305 baseline + `tactical_offline` + `gpu_copy_step1_step5-tac_offline`).

## 9. Driver checklist (null run)

1. `xr-stereo;event=tac-armed;tac=1;...;producer-flags=3` and `tac-config` once.
2. `step5-producer.log ...-tac;`: `avg-objects` ~ the world-member count, `box-draws` > 0 while FRND boxes are visible, `unreadable` ~ 0 (else
   the reason), `markers-matched` > 0, `suppressed=boxes:` > 0, `wedge-frames` > 0 only during gun/missile.
3. `summary-tac`: `teamed` > 0 after the wingmen's FRND boxes were in view, `avg-blips` ~ the game's radar contact count, `backend=used:` ~ frames.
4. Eye PNG (`xr_png_once`): blue FRND boxes on the wingmen outside the 63x38 window; radar arrows in the lower-left HUD circle.
5. Range: section 4 with a game-window shot.
