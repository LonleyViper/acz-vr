# Game-original markers and radar in VR (D-355 follow-up)

Headset test 18 (D-355): with `tac:1` the FRND/TGT boxes were doubled, outlived killed targets and sometimes had the wrong colour; the radar
showed blips but the wrong ones, also on the expanded big-map view. The user wants the GAME'S own boxes and radar. Offline work only: nothing here
was run in the game or a headset. Tags: **[E]** evidence (log/code line cited), **[I]** inference, **[G]** guess.

## 1. Which layer draws what (day config, tac off)

| element | producer | XR layer | state |
| --- | --- | --- | --- |
| FRND/TGT boxes + labels | HUD replay, draws classified `box`/`label` by `hud_classify` (`gpu_copy_step5_hud_markers`) | marker quad (`xr_hud_markers_layer`), pose = the world quad's | game's own draws: one per box, game colour, gone when the game stops drawing it |
| radar frame (rings, cross, cone, background) | HUD replay up to the end of the HUD region | HUD quad (`xr_hud_layer`) | drawn; background is an OPAQUE black square (mask quads write alpha, J-radar-analysis 3.5; fix `radarbg:1`: radar-background.md) |
| radar contacts (blips), expanded map contacts | NOT replayed | none | **missing** (section 2) |
| our boxes / blips / tracers | consumer `tac_frame` (`tactical.cpp`) | eye `post` slot / HUD `hud` slot / eye `pre` slot | only with `tac:1` (removed from the day config, D-355) |

Evidence for the boxes [E, headset-test-18 logs, `%LOCALAPPDATA%\acz-vr\logs` 18:48-18:53]: `continuous-hud-markers;frames=valid:9222,invalid:0,
absent:5304;draws=57639(boxes:40165,labels:17474);refused=0`: the game's boxes reach the marker layer with no refusal. With `tac:1` the same run had
`continuous-tac;...box-draws=54372;suppressed=boxes:635` (98.8 % of the game's box draws were NOT suppressed) while `summary-tac` drew `avg-markers=5.66`
of our own per frame (only 3776 game-marker skips in 13,654 frames): **that is the double**. Our tracks coast 15 s after the game stops drawing a
contact (`kCoastTeamed`), so our box outlives a kill; team came from the colour of whatever box was matched (`tracks=21(friend:1,enemy:10)` with three
wingmen: mismatches), hence wrong colours. Our blips used a GUESSED range (`tacr`, tactical.md 4) and are drawn at the small-radar position even when the
game shows its expanded map: wrong blips in both views. With tac off none of this exists (`tac_frame` is not reached, producer flags 0).

## 2. Why the game's radar has no blips in the VR HUD

The radar group at the end of the HUD region [E, J-radar-analysis 1.1, step37 per-draw dumps]: rings/cone (ALWAYS) -> background -> stencil mask
quads (REPLACE) -> an a3050-shaped triangle (no colour target) -> NOT_EQUAL quad -> mask quads again -> a3050 again -> **contacts: stencil EQUAL draws**
-> closing line. Our HUD replay stops before the contacts:

* Key off (day config): `continuous-hud;...avg-draws=55.4;refused=0;end=rt-change:14526,scene-barrier:0` [E, test-18 log]: every frame ends at the
  first bind of another render target, which sits just before the a3050 triangle (replayed 58 vs 65-74 observer draws per frame, J-radar-analysis 2.1).
* Radar key on (`gpu_copy_step5_hud_radar`, step39): `hud-end;end=scene-barrier;first-excursion=none;excursions=1;returns=0;post-excursion-draws=0;
  replayed=replace:2` in every retained frame, `continuous-hud-radar;...excursions=2738;returns=0` [E, docs/review/evidence/step39-run1-step5-producer.excerpt.log
  L148, L569]. `first-excursion=none` with `excursions=1` and `post-excursion-draws=0` means the excursion came from the OMSetRenderTargets HOOK (the
  draw seam records its own cause) and NO draw was seen between it and the end [E, code: `step5RadarAway` is the only writer of `dg.haveEnd`].
  The region therefore ended at a scene-target barrier (RENDER_TARGET -> something else) taken right after the bind. The phase observer, which does not
  end at barriers, still sees the contacts drawn ON the scene target afterwards (`on-scene-rt=hud:66` of 68) [E, L320], which D3D12 only allows after
  the scene is transitioned back into RENDER_TARGET. **[I, strong]**: the radar's mid pass (the a3050 copy/stencil triangle) reads the scene as a
  shader resource; the scene-barrier rule (`hudReplayBarrier_locked`) ends the region for good there, so the contacts are never replayed.
  This is the code path that loses them. The expanded big map is drawn by the same HUD tail [G: no dump of the map view exists]; it is lost the same way.

## 3. The fix (radar key only; key off byte-identical)

RADAR-SCENE-RESUME (`gpu_copy_step5_hooks.inl` `hudRegionEnd` / `hudRegionSceneBack` / `hudReplayBarrier_locked`, pure rule `radar_scene_back` in
`gpu_copy_step5.h`):

1. A scene barrier out of RENDER_TARGET **while the region is away on an excursion** ends the region as before (scene-end copy, effects layer closed)
   and marks it resumable (`sceneLeftAway`).
2. The SAME resource transitioned back INTO RENDER_TARGET by a plain full barrier (flags NONE, subresource 0/all; at most 4 per region) reopens the
   region, still AWAY: only a proven scene bind or scene draw returns, and everything after it is "late", replayed only inside a radar window
   (opened by a REPLACE mask writer or a proven EQUAL draw; the J-hud-radar stencil proof is unchanged). Split / partial barriers refuse (counted).
3. A scene barrier while NOT away (the tone-map hand-off) still ends the region for good (test (f)).
4. A resumed region reports `end=scene-barrier` at close, keeps one scene-end copy per region, and keeps the effects layer closed (`hudRegionResumed`).

Logs (dedicated `step5-producer.log`): `...-hud-radar-resume;scene-leaves-away=;resumes=(frames:);back-refused=` per status period and
`event=hud-end-resume;frame=;scene-leaves-away=;scene-resumes=;scene-back-refused=` with each `hud-end` diagnostic.

Use: `vr-test-start.ps1 ... -HudRadar` adds `gpu_copy_step5_hud_radar=true` (32 keys with the 31-key day set: not with `-Lean`). Markers need nothing new:
keep `gpu_copy_step5_hud_markers` + `xr_hud_markers_layer` and tac off.

Assumptions the live run decides (J-hud-radar-findings A1-A7 still apply): the barrier back comes before the contacts on the same list (legacy
ResourceBarrier, not enhanced barriers); the a3050 triangles' stencil ZERO lands in another depth-stencil (A1); the stencil reference persists (A2).
The radar background stays an opaque black square by default; the consumer-only fix `radarbg:1` (radar-background.md, offline-tested, not run live) caps the square's alpha to the game's measured ~0.56.

## 3b. "marker-frames=0" with tac off is NOT a broken marker layer (step83 run P / run X re-check, 2026-10-27)

Report: tac off, producer `continuous-hud-markers;...draws=31615(boxes:18866);refused=0` but XR `summary-markers;marker-frames=0;producer-marker-draws=0`.
Re-check of the archived logs (D:\acz-evidence-archive\step83) [E]:

* `continuous-hud-markers` is a CUMULATIVE status line (whole run); `summary-markers` is per 5 s window and `producer-marker-draws=` there is the
  marker draw count of the LAST consumed frame only. The zero lines are windows in which the game drew no box at all.
* Run P (tac off, step86 day keys): 59 summary windows, 34 with `marker-frames=0`. Consumer `marker-skipped` summed = 11424 = the producer's `absent`
  growth over the XR period (11424) exactly; consumer `marker-frames` summed = 6123 vs producer `valid` +5728 (+ the first window). `marker-invalid=0`,
  `marker-lens-invalid=0` everywhere. Every consumed frame whose producer image held boxes was blitted and submitted (`marker-layers` ~ 2x frames: the
  decouple re-composites re-submit the quad).
* Game-window shots of the zero windows show no box: T100 (contacts behind/left, radar only), T160, T235. Non-zero windows show them: T35 (FRND + 2
  TGT; producer avg-boxes 3.00; window 300 frames, draws 6), T295 (TGT + ground hexes; window 301 frames, draws 10).
* The same match (within the first/last window edges) in run X (19623 skipped vs 19724 absent), Y, E10 and headset test 18 (`pre-run-P`, 8591 marker frames). Test 18 is also the live proof
  that the layer is visible: the user saw the game's boxes AND ours (the "double", D-355).
* First marker layer in run P: `order=projection-then-markers-then-hud;marker-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);marker-distance-m=500.000;
  marker-angular-deg=63.20x38.17;marker-pose-equals-world-quad=world-composite-plane`: unchanged since the J-hud-markers merge.

No commit between the overnight "markers layer working" builds and step89 changed this path: CAM-EPOCH (D-341) and DECOUPLE (D-343) leave the
marker decision on fresh frames (`xr_stereo_output.cpp` `use_markers`, `if (s_.hud_markers_layer && fresh)`) and re-submit the fresh composite's quad
on re-composites; the producer split (`h.mSplit`, `gpu_copy_step5_hooks.inl` `step5HudMember_locked`) depends only on `gpu_copy_step5_hud_markers`,
the HUD arm and the marker target, never on `tac` (with tac off `producer_flags_for()` = 0, so nothing is suppressed). Removal on kill is inherent: a
frame without marker draws has `hud_markers_valid=0` and its layers carry no marker quad (never an older image).

Pinned offline by `xr_stereo_decouple_offline` (F2): the day shape (world in projection, decouple:1, HUD + marker layer, 90 XR frames over 60 Hz)
with boxes on every frame and on every 3rd frame: each XR layer carries exactly its game frame's markers (fresh and re-composite), one marker acquire
per game frame with boxes, `marker-skipped` = frames without boxes, the re-composite quad = the fresh composite's pose/size, layer order
projection -> markers -> HUD with source alpha.

Spatial registration (unchanged, documented only): the marker quad is a flat MONO quad at the world plane (500 m, the world quad's pose/size), so a
box sits on the original image ray of its target but has no stereo parallax or occlusion against the newly rendered terrain/cockpit (D-337 caveat);
it draws above the cockpit model (accepted, J-hud-markers-findings "Merge with master" 6). Targets are km away, so the missing disparity is ~0.

Live run must check (tac off; logs `xr-stereo.log`, `step5-producer.log`): with a box on screen in the game window, the matching 5 s window shows
`summary-markers;marker-frames>0;...;marker-invalid=0;marker-lens-invalid=0` and producer `continuous-hud-markers` `valid` growing with
`refused=0`; with none on screen `marker-skipped` grows by the same amount as producer `absent`; in the headset ONE set of boxes in the game colours
(blue FRND, red TGT) that vanish the frame the game stops drawing them. A window with `marker-frames=0` while the game window shows a box (or
`marker-invalid`/`marker-lens-invalid` > 0) would be a real defect.

## 4. tac sub-options (D-355)

`tac:1` now means tracers only. Our boxes and blips are individually opt-in inside `xr_terrain_stage` (no new config key): `tacboxes:1` (alias
`tacmark:1`) draws our world-anchored boxes AND enables suppression of matched game boxes (`tacsup:0` keeps the game's); `tacradar:1` draws our
blips. Defaults off. `producer_flags_for()` gives 0 with tac off and never `kProducerSuppress` without `tacboxes:1`, so the game's markers are never
removed unless we draw them. With tac off nothing of it runs (tac_frame not reached, flags 0, `StereoFrame::tac == nullptr`).

## 5. Tests (offline; build/ctest by the lead)

* `gpu_copy_step1_step5-hud-radar-region_offline` (k): the step39 shape `R k | o | barrier out | z T | barrier back | @ N R E6 k7 | output` replays
  `R k R E6 k7`, 1 NOT_EQUAL skip, 1 window, 3 late replays, end bucket scene-barrier, 2 leaves / 2 resumes; key off: the same shape ends at the bind
  (`R k` only), region never resumed. Test (f) (barrier while not away) unchanged.
* `step5-hud-radar-rules`: `radar_scene_back` table (full barrier ok, BEGIN/END_ONLY `split-barrier`, subresource 1 `partial-subresource`, 5th `resume-cap`).
* `step5-hud-radar`: the two new log lines present, dedicated log only.
* `tactical_offline` `test_options`: defaults off, `tacboxes`/`tacradar`/`tacmark` forms, `producer_flags_for` (0 with tac off, no suppression without boxes).

## 6. Live test: what to check

Null run first (no headset), mission 02, `-HudRadar`, with `-PngOnce`/`png-burst.now` while contacts are visible, plus a game-window screenshot:

1. `step5-producer.log`: `...-hud-radar-resume;scene-leaves-away=` ~ 1+ per frame and `resumes=` ~ the same (if `back-refused` grows, the game uses
   split/partial barriers: report the reason); `continuous-hud-radar`: `returns` > 0 now, `replayed=equal:` > 0 with contacts, `equal(indices:)`
   comparable with the contact count, `refused=equal:0,writer-skipped:0`; `continuous-hud;refused=0`, `end=` mostly `scene-barrier`.
2. `hud-end` + `hud-end-resume` lines: `scene-resumes>=1`, `post-excursion-draws` > 0, the `hud-end-draws` disposition string shows `r` for EQUAL (`E`) draws.
3. PNG `...-hud-over-world.png` radar crop vs the game shot: contacts at the same places/colours, nothing outside the radar square; open the big map:
   its contacts present too. Effects PNG unchanged (effects layer still closed after the barrier).
4. Headset: ONE set of FRND/TGT boxes (the game's) in game colours, gone with the kill; radar blips in the lower-left radar matching the 2D game
   window; the expanded map shows the game's own contacts. Known: opaque black radar background (with `radarbg:1`: translucent, radar-background.md 6).
5. If contacts are still missing with `resumes>0` and `equal:0`: read `hud-end-verdicts` / dispositions (A1/A2/A7 of J-hud-radar-findings).
