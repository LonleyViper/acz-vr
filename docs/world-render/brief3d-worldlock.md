# Briefing table: world lock and icon height (headset test 8, 2026-11-01)

User, verbatim: "Briefing 3d render isnt fixed properly int he world and moves when i move my head. Also the icons floated a little higher after a few seconds."
Evidence: `D:\acz-evidence-archive\headset-test8-20261101` (xr-stereo.log, ee-camera.log, step5-producer.log). Fixture success says nothing about the
headset: the checklist at the end is the acceptance test.

## What the logs prove

* The table is placed ONCE: `summary-brief3d-table placements=1 fits=1` from t=20 s to the end; `summary-brief3d-latch gen-refused=0 alloc-busy=0
  resubmits=0 lost=0 render-failed=0`, 450 layers per 5 s (90 Hz, one render per XR frame). There is exactly one LOCAL space change in the whole
  run (`recenter ... reason=runtime-space-change gen=1` at t=90 s), 5 s AFTER the table had left the briefing (`stale` counting from t=85 s), so
  neither the early XR start (`vsstart=first`: session and LOCAL space exist 2 s into the game) nor a recentre re-placed the table while it was shown.
* So the table's placement and the per-eye matrices are not what moved. The offline proof is old and still holds (`test_world_locked_across_head_motion`
  and the new `test_worldlock_quad_depth_and_icons`): decoded from the float32 record the GPU reads and un-projected with an independent OpenXR view
  model, every grid vertex AND the icon land on the same LOCAL point in every frame and both eyes while the head walks, turns and nods (max error 7e-7 m).
* `no-slot=29018` in the final `summary-brief3d-table` is the producer's SLOT ring counting map draws that found no slot: with `b3l:1` the table never
  uses a slot (the latch path), so it is the unused replay path asking on every map draw, not a placement failure. `stale=647` is the table core
  counting frames after the map screen was left.
* `ee-camera.log brief3d-content` shows the icon heights moving: `scale=0.6155 -> 0.7999 -> 0.6804 -> 0.6155 -> 1 -> 0.6485 -> 0.5889`,
  `height-m=0.35 -> 0.185 -> 0.232 -> 0.35`, `changes=249` in 70 s, while the measured content top went 36 -> 15 -> 33 -> 36 units.

## Cause 1 (certain): the icons' vertical scale relaxed upward

Icons stand at `(model y - origin) x metres-per-unit x vexag x content_k`. `content_k` (`brief3d.h squashed_fit`, per scene) tightens at once when the
highest icon would pass the 0.35 m cap and, since the W8-1 fix, RELAXED back at 0.2 per second once the measured top fell (the game omits or moves its
tall aircraft, or zooms out). Every relax lifted every icon above the table a little ("floated a little higher after a few seconds") and the next tall
icon snapped them down. Fix: `kContentRelaxPerS = 0`: within one briefing page `content_k` is MONOTONE (tightens only). A new page (no map draw for 2 s,
`kScreenGapMs`) still starts from 1, and the re-latch (30 s gap) is unchanged. Cost: after a game zoom-out inside one page the icons stay compressed
until the page changes (a few mm at overview scale).

## Cause 2 (likely, not provable offline): swim of the table quad against the world

With `u3q:1` (default) the table is submitted as ONE QUAD PER EYE (`table_front`: SteamVR draws quads above projection layers) at the located eye pose
and a fixed plane distance (`kTableQuadDistanceM` = 1.5 m). The picture is drawn for exactly that eye pose, so with a perfect pose it is the table.
With a real pose error `delta` (prediction, frames the compositor holds) the flat picture is seen against the world shifted by
`delta x (1/content_depth - 1/plane_depth)`. The table is ~0.75 m away: with the 1.5 m plane that is 0.67 rad per metre of pose error (1 cm = 6.7 mrad,
0.4 degrees), growing with head speed: "moves when I move my head". Fix: the plane of each eye's quad is placed at the table's own depth
(`LatchCore` -> `ProjectionLayer::table_quad_distance_m`, `table_axial_depth`: depth of the table centre along that eye's view axis, clamped to
[0.4, 4] m by `xr_stereo::table_quad_distance`; resubmitted frames keep the plane of the render they show). The picture is unchanged (the quad still
subtends exactly the eye's FOV), only the plane moves; for the table centre the parallax error is zero (offline: 2.6e-8 rad against 13 mrad for the
1.5 m plane at 1 cm of pose error). UI3D menus and the hangar keep 1.5 m (their content is drawn at the quad's own distance).

If the table still crawls in the headset, the remaining suspects are: (a) the compositor holding frames (`xr-stereo.log` layers per 5 s below 450);
(b) the quad being reprojected on its own; (c) the eye swapchain image read by the runtime before the render on the game's direct queue finished
(`alloc-busy`/`resubmits` stayed 0 in test 8, so the CPU saw completion, but cross-queue visibility is not logged). Cheap discriminator: one flight with
`u3q:0` (projection layer) to see whether the swim follows the layer type.

## Debrief 3D (dbr3d:1)

The wraparound is anchored on the vscreen placement (`vscreen.h place`, `xr_vscreen_core.h`), which is world-locked: re-placed only on entry to a
screen mode (`m.entered`), a LOCAL generation change or `vsfollow:1` (off). No per-frame head dependence was found. It uses the older slot path
(TableCore: rendered per GAME frame with the producer's pose). CORRECTION 2026-11-01 (follow-up): its slots are OPAQUE (`SlotMeta::opaque`), and
an opaque `table_front` slot is submitted as ONE projection layer (`compose_layers`), not as per-eye quads, so the 1.5 m / table-depth quad plane does not
exist for the debrief (nor for UI3D menus and the hangar) and no plane fix applies; the earlier 0.42 rad/m estimate was wrong. Any debrief swim is the
compositor's rotation-only reprojection of a picture up to a game frame old (head TRANSLATION error uncorrectable). The real fix would be a per-XR-frame
re-draw as the briefing latch does (large: not started). Covered by `brief3d_table_offline` (opaque list = one projection).

## Tests

* `brief3d_live_offline test_icons_rigid_over_time`: 60 s of 16 ms map frames, a fixed aircraft plus a tall aircraft present 5 s / absent 5 s; the fixed
  aircraft never rises above a height it had had and is constant after the first 10 s (the old relax fails it). `test_zoom_content_scale` and
  `test_table_stable_under_jitter` now assert no relax and a reset only after a 2 s gap.
* `brief3d_latch_offline test_worldlock_quad_depth_and_icons`: 5400 XR frames (60 s at 90 Hz), head walking/turning/nodding, 45 Hz game map with an
  orbiting camera: placement never re-latched, fit unchanged, the air icon decoded per eye from the GPU record at the same LOCAL point (error < 5e-4 m)
  and at a constant 0.1875 m above the table top, quad plane at the table depth (parallax at 1 cm pose error < 0.2 mrad; the 1.5 m plane > 4 mrad).
  `test_busy_resubmits` also checks that a resubmitted frame keeps its render's plane.

## Headset check list (one briefing, about 60 s)

1. Enter the briefing map. The table appears about 0.75 m in front of you at table height and stays there. Walk or lean 30 cm each way, nod, turn 90
   degrees and back: it must stay fixed in the room like a real object; watch for a slow crawl or lag when you stop moving (the reported fault).
2. Watch one aircraft icon for 30 s without moving the head: its height above the table must not change (no slow rise, no snap down).
3. Zoom the game map in and out once: icons may follow the game's own heights; after zooming back out they may stay slightly compressed (known; resets on
   the next page). Report any creep after the zoom stopped.
4. Press Home (recentre) during the briefing: the table may re-place once in front of you, then must stay put.
5. Open the debrief 3D map: turn and lean; report whether the wraparound swims (follow-up above).
6. Logs to send: `xr-stereo.log` (`summary-brief3d-latch`: layers per 5 s about 450, `alloc-busy`, `resubmits`; `summary-brief3d-table placements=1`) and
   `ee-camera.log` (`brief3d-content`: `scale` only falls within one page, `changes` small).

## Headset test 9 (2026-11-02, step182): still moving with the head, low fps

User, verbatim: "Briefing table render still not locked in position. Moves when i move my head. Renders at low fps".
Evidence: `D:\acz-evidence-archive\headset-test9-20261102` (xr-stereo.log, ee-camera.log), compared with `headset-test8-20261101` and the null run
`step180\brief\runAGM1`.

### What draws the table, and what the logs rule out

* The picture is the LATCHED 3D render, not a 2D path: `summary-brief3d-latch` renders 116 -> 4266 between t=10 s and t=62 s, ~450 per 5 s
  (one per XR frame), `layers` = `renders`, `render-failed=0 alloc-busy=0 resubmits=0 lost=0 gen-refused=0`, `age-max-ms=31`. `no-scene=1027` are
  the menu frames before the first briefing scene. The slot path's `blits` (391 -> 1109) only run before t=10 s and after t=62 s (menus, `content=1`
  only there). `live=producer-configured(table-on-the-screen-layer)` in `brief3d-option` is only the layering label: the table is submitted ON TOP OF
  the vscreen quad (`summary-vscreen mode=screen placements=1 screen=3.20x1.80m@-0.03,0.09,-2.46`: placed once, world-locked).
* Placement and fit are latched once (`brief3d-fit outcome=ok` once, producer `fits=1 placements=1`, one LOCAL generation). The game camera does not
  follow the head in the briefing: `summary-pov mode=other route=flat`, the look-stick gate only acts in flight, and the latched draw keeps only
  model-space geometry and icon positions (the game's camera matrix is replaced per eye; `test_world_locked_across_head_motion` orbits it). The grid
  content stopped changing after the intro animation (`brief3d-latch miss=399/..` frozen from t=35 s).
* The app delivers 90 Hz in the briefing: `summary-timing xr-period-ms-mean=11.11`, `xr-period-ms-max=11.1` in most briefing windows (occasional
  22-55 ms hitches as in every other screen), scenes at the game's 60 Hz (`scenes` +300 per 5 s), producer `lag-max=2`. So "low fps" is not a slow
  render, a busy allocator or a read-rate stall (test 7's cause; `rate` no longer rises).
* `clipped` (grid segments behind an eye's 5 cm plane) rose by 0.4-3.4 million per 5 s (null run: 0): the user turned the head far enough for the
  table to leave the view while it was drawn, i.e. the head moved a lot and the table was re-drawn for each pose.

### Cause (strong, not provable offline): the per-eye quad posed at the eye

History: tests 1-4 (2026-10-28..30b, the table as a projection layer under the screen) never reported shaking. `u3q:1` (aa7da7ab, 2026-10-30 15:42)
made the table one quad PER EYE posed at that eye's located pose; test 5 (step137, the same evening) reported "works but shakes a lot", test 6 "shakes
like crazy", and tests 8 and 9 the same, through the per-XR-frame latch render (b3l:1), the table-depth plane and a 90 Hz render with no busy frame.
The common factor is the head-posed quad. Its picture depends on the head ORIENTATION, so anything SteamVR does to a quad layer between our
submission and the display (its own pose / timing for quads, which it composites as overlays above the projection, a texture from another frame than
the pose) moves the whole table by the head rotation of that interval: offline, a picture one frame old in a 150 deg/s turn is 25-34 mrad (1.5-2
degrees) off, the "moves when I move my head", and the error changes with every frame and head speed, which reads as judder ("low fps").

### Fix (b3p:1, default)

The quad is WORLD-FIXED: posed once per placement (`latch::TablePlane`: through the table centre at half the grid relief, facing the seated head,
its rectangle the table volume up to the content cap seen from that head, +10 % + 3 cm, each half extent capped at 1.2 m; default table: 2.4 x 1.17 m
at ~0.85 m). Every XR frame each eye's picture is the central projection of the latched scene onto that plane from the eye's POSITION
(`latch::plane_view`: the plane's orientation, the eye's position, the off-axis FOV whose image rectangle is the quad); the GPU path is unchanged
(it already takes any pose + FOV). Head rotation does not change the picture at all; a picture one frame old is wrong only by the parallax of that
frame's eye translation for content off the plane (offline, the same 150 deg/s turn: < 6 mrad max, ~0.7 mrad median). An eye within 15 cm of the
plane / behind it or a view grazing it by more than ~80 degrees keeps that frame on the per-eye quad (`plane-off`). Busy frames resubmit the fixed
quad of the shown render. `u3q:0` (projection layer) ignores the plane.

* Files: `brief3d.h` (option `b3p`), `brief3d_latch.h` (`TablePlane`, `table_plane`, `plane_view`, `Scene::plane`), `brief3d_live.h` (plane computed
  with each placement), `xr_brief3d_core.h` (`LatchCore`: plane views, `table_quad_fixed`, `plane` / `plane-off` counters in
  `summary-brief3d-latch`), `xr_owner.h` / `xr_owner_openxr.cpp` (`ProjectionLayer::table_quad_fixed/pose/width/height`: both eye quads at that pose).
* Tests: `brief3d_latch_offline test_fixed_plane_world_locked` (60 s of walking / turning / nodding at 90 Hz with a 45 Hz game map: the submitted
  quad never moves; every grid vertex and the air icon, decoded from the float32 record and put on the submitted quad, lies on the ray from the
  located eye to its latched LOCAL point (< 2e-4 rad); rotation with the eyes in place gives byte-identical records; the stale-picture comparison
  above; plane-off fallback; resubmit keeps the quad; b3p:0 and u3q:0 keep the per-eye path). `brief3d_offline` checks the option.
* Toggle: `b3p:0` in `xr_terrain_stage` restores the per-eye quads (A/B in the headset). Logs: `summary-brief3d-latch plane=` should equal the
  new renders and `plane-off` stay small; `clipped` should drop to about 0 (culling now runs along the plane normal, not the gaze).
* Costs: the table's horizontal sampling is ~90 % of the per-eye image at the centre (the rectangle is wider than the view); tall content at the
  NEAR corners (projected beyond 1.2 m) is cut at the quad's side edges; a null-run `eye0-table` dump now shows the picture on the plane (skewed
  against an eye view): use the mirror or `b3p:0` for eye-view dumps.

Headset-only: whether the table now stays put when turning / leaning, and that it still looks 3D. With b3p:1 both eye quads share one pose, so if
SteamVR ignored `eyeVisibility` each eye would show the right eye's picture (a flat or doubled table): report that, and try `b3p:0` once to compare.
