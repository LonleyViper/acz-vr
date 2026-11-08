# Headset test 2026-10-28, items 3 / 4 / 9 / 10: offline analysis and capture plans

Offline, read-only analysis of docs, code and D:\acz-evidence-archive (step83, step97 shader dumps, step105-107 logs, headset-test9..17). No game, no build, no code change: the
evidence does not pin a root cause for 3, 4 or 10, so each ends in a capture plan. Tags: [E] measured, [I] inference, [U] unknown.

## Item 3: sun lens flare renders black with a dark ring (user-3.webp)

[E] The screenshot shows two different things. The cream dot at the top right is OUR sun disc (`env_sky`, terrain_stage.cpp: disc + glow at the mapset light direction). The dark
ring with a black core is centred near the screen centre, i.e. on the lens-flare ghost chain (mapset +0x270 chain, positions -0.8..0.8 along sun to centre; skytex 1 ring, 10 halo, 11 hexagon).
[E] The game's flare is a 2D depth-ALWAYS blended sprite draw. No flare draw is in the effects path. The only unclassified depth-ALWAYS effects shader seen, PS 7a3c9385
(step97 X2 disassembly: rgb = T1(0,0).x * COLOR0.rgb * T0.rgb, a = T0.a * COLOR0.a; non-indexed), appears ~95 times in total per run (flat counters in the step83 and headset-test13
candidate tables), so it is a one-shot overlay (fade/flash), NOT a per-frame flare. The flare most likely uses the generic HUD sprite PS d34ee153 (depth ALWAYS, 1.4M draws seen,
all `class-hud-sprite`, never replayed by the effects class).
[I, strongest hypothesis] Those depth-ALWAYS draws are replayed by the HUD layer. `hud_decide` (gpu_copy_step5.h:366) checks tracking, root, PSO, RT, depth and stencil but NOT the
blend tuple. The replay goes into a cleared transparent image that the consumer composites alpha-over (xr_stereo_blit.hlsl hud_alpha). An ADDITIVE flare (SRC_ALPHA/ONE or ONE/ONE)
replayed onto a transparent target has nothing to add to: its colour is dark/low-luminance with non-zero alpha, which alpha-over turns into a black ring/disc. Inside the game's own
window the same draw is bright. [U] The flare's real blend tuple and PS: nothing in the archive records the blend tuple of HUD-class draws (hud-end-draws has only a disposition string).
Not fixed: a blanket "skip additive HUD draws" could remove legitimate HUD glow, and the HUD tuples are unknown.
[E, live batch Y null run, 2026-10-29, `fxsig:f1a4e000`] Corrected cause. The flare is two large 4-vertex quads (signatures cb01abc5 and 1579d9a8, PS d34ee153, VS 534f29ca,
depth ALWAYS) with blend `bl=on:2,2,3/2,1,1` = colour ONE,ONE,**REV_SUBTRACT** (dest - src: the game DARKENS its scene there), alpha ONE,ZERO,ADD, admitted into the HUD image
(`img=hud;cls=unclassified;why=vertex-outside-view-part`). Not additive: the whole HUD uses 5,2,1/1,2,1 (SRC_ALPHA,ONE,ADD) and is correct in the eye (batch W). Replayed into the
transparent (0,0,0,0) HUD image, dest - src clamps to 0 while alpha = src alpha, and the consumer's alpha-over turns that into the opaque black ring/core of user-3.webp.
**Fix HUD-RS-SKIP (default ON; `hudrs:0` in `xr_terrain_stage` turns it off):** the PSO record now keeps render target 0's colour and alpha blend OP (`PsoOutput::blend_op0/blend_op_alpha0`,
in the padding of the 448-byte Draw-seam Shape). A HUD-region draw of exactly the observed flare class (`gpu_copy_step5.h hud_rs_subtract / hud_rs_skip`: blend on, colour op REV_SUBTRACT with colour factors ONE/ONE, alpha ONE/ZERO/ADD; `PsoOutput::rs_flare_factors` is set at PSO creation) is a SKIPPED class (W10-1 P1#1: plain SUBTRACT, src - dest, is positive content and is replayed, as is any REV_SUBTRACT with other factors or alpha op):
not replayed into the HUD or marker image, counted, NOT a refusal, so `hud_frame_valid` and every other draw of the frame are unchanged (the additive 5,2,1 HUD draws are still replayed;
SUBTRACT/ADD/MIN/MAX/blend-off and other tuples keep the old rules). W10-1 P1#2: a skipped draw that writes depth marks a region depth gap (`HudTarget::depth_gap`, `Step5Hud::depthGap`); later depth-TESTED (GREATER/GREATER_EQUAL) draws are then refused `depth-writer-skipped` (ALWAYS/NEVER draws unaffected; the observed flare is depth ALWAYS and not known to write depth); stencil writers use the radar stencil gap. The game's own draw (window/mirror) is untouched. Log: `gpu-copy-step1-status;event=continuous-hud-rs-skip;n=<skipped draws>;frames=;ds-writers=;on=`
with the status cadence, printed once a draw was skipped or when the switch is off; the flare diagnostic shows `hud=rs-skip;img=none` (so `adm=no:N` in `fx-flare-sig`); the radar
disposition string uses `s`. Tests: `step5-hud-rules` (class), `step5-hud-rs-skip` (flare tuple frames, pair of quads, SUBTRACT replayed, other-factor REV_SUBTRACT replayed, skipped depth writer then GREATER refused / non-writer not, blend off, a would-be-refused flare, hudrs:0),
`flight_stage_adapter_offline` (option + switch). Effect in the eye: the flare's darkening is simply absent (our eye has no ring and no game flare); the window keeps the game's flare.
Better treatment, not done: replay the REV_SUBTRACT quads per eye over the EYE scene (where dest is the real scene, as in the game) instead of into the HUD image; it needs a
scene-composite pass with the game's PSO against our eye colour target and screen-space placement of a sun-anchored sprite, i.e. a new GPU route. [U] Not verified live yet: the next
sun-in-view batch should show `continuous-hud-rs-skip;n>0` and no ring in the eye mirror.
Capture plan (one live batch, sun in the forward window, then 60 degrees off): (1) add a read-only per-frame HUD census line (per depth-ALWAYS blended draw in the HUD region: ps8,
vs8, blend tuple src/dst/op/srcA/dstA/opA/mask, vertices, replayed/refused). (2) png burst: compare `*-hud-over-world.png`, `*-effects-on-black.png` and the game mirror. If the ring is in
hud-over-world only, the cause is confirmed and the fix is a per-tuple route (additive HUD draws to the effects coverage-clone path, which exists for the tracer tuple 5,2,1/1,2,1) or an
ignore list of the flare draws found in the census. (3) If it is NOT in the HUD image, inspect the window composite: the game's alpha channel carries the flare and could mask the world.

## Item 4: colour flicker while pitching up

[E] No archived log or frame pairs a pitch-up with a colour change. Test 8 recorded 28/12394 refused frames "when pulling up", but that was the step34 pre-terrain renderer.
[E] The sky has no zenith seam (sky_at scales the azimuth term by cos(elevation), C1 smooth blends), so the obvious analytic candidate is already handled.
Remaining candidates [I]: (a) our single cloud sheet (layer 0, 16000 u; 5-tap blurred blob map + smoothstep, repeated every 65,536 u) shimmering as the ray hit distance changes quickly at
pitch-up; (b) the sun glow (pow 300 additive) and the moon sprite entering/leaving; (c) per-frame refusals in the world/effects layers (refused frames reuse the previous image via
xr_world_hold_last); (d) the game's own fade/fog change with altitude.
Capture plan: driver pitch-up (autopilot Climb) with png-burst at pitch 0, 30, 60 and consecutive-frame differences of `*-world.png` (terrain only) versus `*-final.png`, plus the per-second
refused counters. If the difference is in `world` alone it is (a)/(b) (bisect `env:1` vs `sky:1;light:1` without `cloud:1`); if only in `final` it is (c).

## Item 9: some clouds visible only inside the forward rectangle

[E] Day install (step105): `gamecloud` defaults to 0 because our clouds are drawn (xr-stereo.log `game-clouds;replay=off;reason=default(cloud:1-active)`), and the producer logs
`continuous-hud-effects-gamecloud;replay=off;ignored=27022` in step107 H1: the GREATER_EQUAL 4caf6e2d cloud cards ARE dropped. [E] The same PS with depth GREATER (4..19 quads per
draw, count changing every frame; ~54k of 81k seen replayed in H1) is still replayed, but only into the window effects image: fxmem:4 (day install) replays only SMOKE per eye.
[I] So the clouds still seen in the rectangle are the GREATER-class 4caf6e2d sprites (wisps, cards, specks). The game pre-projects them for its own ~38 degree window, so outside it
they do not exist; this is not "world objects not replayed per eye", it is the effects-outside-window gap of effects-outside-window.md.
Status: fxmem:6 (D-383, per-eye sprite replay, merged, in step108, NOT live-verified) is the existing fix path; our own cloud sheet is a separate layer.
Verification plan: fxmem:6 and gamecloud:0, effects-on-black PNG while looking 60 degrees off the window; the sprites should be present and world-anchored. If clouds still appear only in the
rectangle with the GREATER class replayed per eye, the source is a draw outside the effects classes: log `class-ignored` reasons per PS during the capture.

## Item 10: k00a ground/map brown

[E] The tiles are correct and dark brown at source (D-379: unique-tile blocks 256..611, "a dark brown aerial photo"). TAKEOFF-LOOK (ac52a2ac, in the day install since step96) uses the
neutral look gain 0.54 for stage id "k00a" and the mission env for the takeoff stage. D-381 still found the ground brownish vs the game's grey and asked for a registered ground-only
crop; that crop has never been taken. [E] The neutral gain is a single-shot fit (rms 7/255, +-0.06); near-ground ours 34,35,28 vs game 34,36,32: blue is 4 levels low, the residual
warm cast. [I] Contributors: (1) the game's grey comes from lighting/vertex colour on near-neutral texels while ours multiplies texel x gain then hazes toward the mission-adopted (greenish
m02a) fog colour; (2) after the takeoff switch to the mission stage the ground is m02a (gain 0.64,0.61,0.50), not k00a. [U] Which phase the user looked at (runway vs after the cut) and
whether "map textures" means the brief3d map (a separate pipeline).
Fragility noted, harmless today: `look_gain_for` uses `strncmp(id,"k00a",9)`, true only for the exact 4-char id.
Capture plan: one registered pair at k00a parked T10 with `look:1;toenv:mission`: game window crop + our XR eye dump at the same pose; fit a per-channel gain on the ground-only region
(D-381 crop method, detail_fit.py) to replace 0.54 by (g_r,g_g,g_b) and add the blue 4/255; verify on a second pose. Separate capture for the briefing map: brief3d overview png vs the
game's own briefing map.

## Item 3 live diagnostic: `fxsig:f1a4e000` (read-only, implemented, not yet run)
Arming (key-neutral): existing stage option `fxsig:<8 hex>` with the reserved value `f1a4e000`, e.g. `<stage>;fxmem:1;fxsig:f1a4e000` (needs fxmem >= 1 and the HUD capture on).
Code: `src/acz-vr/fx_diag_flare_air.h`, `gpu_copy_step5_fxdiag.inl` (`step5FlareLog`), call sites right after `hud_decide`/the HUD classifier in `step5HudDraw`.
Logged: every HUD-region draw that is depth-ALWAYS (or depth test off) AND (additive, i.e. blend on with dst factor ONE, OR bbox within 160 units of the screen centre in either raw GS (2048,2048) or
offset-free (320,224) coordinates). One `fx-flare-draw` line (< 426 bytes): `ps= vs= bl=on:src,dst,op/srcA,dstA,opA add= d=<func> cnt in idx nv bbox=minx,miny,maxx,maxy ctr= hud=<admitted|hud_decide reason>
img=<hud|markers|none> cls=<instrument|marker-box|marker-label> dt=<depth tested> why=<read status> sup=<dropped>`. bbox = raw position x,y of the first 48 vertices/indices. Limits: 400 vertex reads/s,
24 lines/s, 6 per signature/s (12 if additive), 400/run. Live-check list: (1) is there a draw with a flare-like bbox (centre or large) and what are its ps/vs/bl; (2) `add=1` and `img=hud` together confirms the
additive-onto-transparent hypothesis; `hud=` a refusal reason means it is not replayed (then the dark ring is something else); (3) does `bl` match 5,2,1 / 2,2,1; (4) lines with `why=` other than ok mean the vertices
were unreadable (blend still logged for additive draws); (5) fly with the sun in view, then away, compare line counts.

Start gate (batch K2): same as the aircraft diagnostic (aircraft-shading-reflection.md section 6): no line is counted against the 400/run cap until `%LOCALAPPDATA%cz-vrxdiag.arm` is created/touched after the first poll (the autopilot touches it at T0) or 90 s after the first HUD draw; arming resets the counters.

### Item 3 diagnostic, batch M rework (replaces the "Logged:"/"Limits:" rules and the 400-line run cap above; the start gate is unchanged)
Why: batch M live logged 400 `fx-flare-draw` lines that were all HUD instrument draws (398 instrument, 2 marker-label), so the cap was spent before the sun was ever in view, and flight
detection (ee-camera struct, `cam:` pose) was unreliable. The log is now usable in an uncontrolled headset session. Nothing depends on flight detection: it arms by the trigger file or 90 s after the
first HUD draw (`Limiter::kArmDelayMs`) as before. Code: `FlareTracker` in `fx_diag_flare_air.h`, `step5FlareLog` in `gpu_copy_step5_fxdiag.inl`; tests `fx_diag_flare_air_offline_tests.cpp`.
- Observed draws: every depth-ALWAYS (or depth-off) HUD-region draw within the 400 vertex reads/s budget (reads refused by the budget are counted as `unread=` in the summary).
- Signature = ps8, vs8, blend tuple, blend on/off, depth func/enable, coarse bbox bucket (`bk=` hex: size class 0..7 << 5 | 4x4 screen cell 0..15, 16 = off-screen centre, 31 = no bbox; raw GS
  space is normalised to screen space first). 64 signatures tracked (least recently seen non-candidate evicted when full; `evict=`, `ovf=` counted).
- Dedupe: the first sighting of a signature is ALWAYS logged. Repeats: once per 30 s, at most 3 lines per signature in total (candidates: once per 5 s, at most 12). A reappearance (run 2+) of a
  candidate is logged immediately within its total. Global caps: 24 lines/s, 250 non-candidate lines per run, 400 lines per run in total (candidates may use the remainder). Repeating instrument
  draws therefore cost at most 3 lines per signature however long the session runs.
- Candidate (`tag=flare-candidate` on the `fx-flare-draw` line, `rs=` reasons), tightened after live batch N (51 of 57 signatures were tagged, all HUD instrument / marker-label draws, because
  the HUD itself is drawn additive 5,2,1 with ps d34ee153 / vs 534f29ca): the draw is additive (`A`, blend on with dst factor ONE) OR depth-always, AND NOT classified by step5's HUD gate as
  `instrument`, `marker-label` or `hud-text` (`hud_chrome_class`; `marker-box` stays eligible), AND (large `L` [bbox area >= 1/16 of 640x448, or a side >= 320] OR near the screen centre `C`
  [within 160 units, either GS convention] OR intermittent `I` [the signature reappeared after >= 1 whole second of absence, i.e. appears/disappears with head motion; `runs=` counts presence
  runs]). Additive alone no longer qualifies; `A` still adds to the score and appears in `rs=`. Chrome-class signatures are never candidates, so they get only the 3-line allowance. Score (`sc=`) = A4 + L2 + C2 + I3 + N1 (`N` = first seen >= 30 s after arming).
  Other first sightings carry `tag=new`, later ones `tag=seen`. `sg=` is the low 32 bits of the signature key, shared with the summary lines.
- Periodic summary every 20 s (only when new draws were seen): `fx-flare-summary;t=<ms since arm>;draws;unread;sigs;cand;ovf;evict;lines;candlines;qdrop;seq`, followed by up to 8
  `fx-flare-sig;t=<ms since arm>;r=<rank>;sg;tag=<flare-candidate|other>;sc;n=<draws>;lg=<lines logged>;runs;ep=<seconds seen>;gone=<unseen >= 3 s>;ps;vs;bl;add;d;bk;lrg;ctr;bbox=<last>;adm=hud:N,mk:N,no:N;last=<last
  hud_decide verdict>;cls;seen=<first>-<last>s` lines, ranked candidates first, then score, then draw count. `adm=` is the admitted-image routing per signature (HUD image, marker image, or
  not replayed); `last=` is the latest verdict or refusal reason. A later analysis finds the flare signature from the user's log alone: take the top `flare-candidate` `fx-flare-sig` lines, check
  `add=1`, `adm=hud` > 0 (additive onto the transparent HUD image) versus `no` > 0 (refused, so the dark ring is something else), `runs` >= 2 / `gone` for head-motion appearance, and the `bl` tuple.
- HUD-RS-SKIP (item 3 fix above): a colour REV_SUBTRACT ONE/ONE draw (`bl=on:2,2,3/2,1,1`, the flare pair cb01abc5 / 1579d9a8) now logs `hud=rs-skip;img=none` and counts as `adm=no`
  with `last=rs-skip`; that is the fix working, not a refusal. Run with `hudrs:0` to see the pre-fix `adm=hud` routing again.
