# DEBRIEF-3D: the post-mission debriefing as a 3D table (user objective, headset report 2026-10-29 item 8)

Offline research 2026-10-29. Nothing here ran in the game or a headset. Tags: **[E]** evidence (file, log line), **[I]** inference,
**[U]** unknown. Companion of `brief3d.md` (the briefing table, D-372/D-389) and `briefing-3d-map.md` (the shaders and data).

## 0. Summary

**Update 2026-10-30 evening (section 8): stage 2 live glue implemented (offline-tested only, nothing ran in the game or a headset).** The
png-screen capture of the debrief shows that the census "map window" is the MISSION RESULT panel drawn over a perspective wireframe grid with
the flight paths. With `dbr3d:1` (and `ui3d:1`) every debrief frame becomes a UI3D frame: the map scene (the draws before the panel) is
re-rendered per eye on a world-locked wraparound 4 m away, the results panel and its text on the UI3D floating-panel layers in front of it.
The table of section 7.3 is not used live.

**Update 2026-10-30 (section 7):** the screen the user left after m03k (live-1508 census, 60 frames) IS the debrief, and it is pure ps2_v 2D
(no SelMap / ModelIcon / ACM / other key) = case B. Table core `dbr3d.h` + `dbr3d:1` option (parsed, live glue pending) and a census GEO
diagnostic for the indexed map draws are implemented with offline tests. The bullets below are the 2026-10-29 state.

* **What the debrief draws is NOT yet known.** No archived run had a draw histogram (`briefing-observe`) armed on the debrief, and
  no screenshot of it is archived. The one real debrief we have (user session `D:\acz-evidence-archive\headset-2026-10-29-session`,
  after m02a, confirmed by the user) was logged only by the brief3d producer, which counts nothing but the briefing map's own draw
  classes.
* **Evidence [E]: the debrief did NOT draw the briefing map's classified draws** (indexed `DrawSelMap_v` line grid with PS
  `9ebdf113`, indexed `DrawModelIcon_v` triangles): the producer's `selmap`/`icons`/`refused` counters stay flat for the whole
  150 s between the end of the m02a flight and the next map phase (section 2). Non-indexed or other-topology SelMap variants, and
  any other shader, are not excluded.
* The one post-flight map phase in that session (22 s, ticks 100688-100710 s) is, by its unit count and icon heights, **the next
  mission's (m03k "The Round Table") briefing, not the debrief** [I, strong; section 2.3]. A note elsewhere read it as the debrief;
  the data do not support that.
* Game data [E]: `data/debrief/` holds only 2D textures (buttons, a message sheet, a font, a 624 B icon sheet, a noise texture);
  the archive has **no debrief shader** (every `shader/*.cso` in the a.arc listing checked) and no debrief map or track file; `data/caption/debri/<lang>/
  mNNcap.bin` (one per mission and language) are the debrief subtitles (m17a/m18a are empty stubs, 176 B: the last missions go to
  the ending [I]). So any debrief map is drawn either by the briefing map shaders (`DrawSelMap_v` / `DrawModelIcon_v`, reusing
  `data/selmap/*`) or by the generic emulated-GS path (`ps2_v` / `ps2Line_v`, 2D pre-projected) [I]. Which one decides the route
  (section 4), and only a live histogram on the debrief can tell.
* Delivered now: a read-only extension of `briefing-observe` (every draw key of the window is now named, not only the top 8) and
  the capture request in section 5.

## 1. How the debrief is reached and what the logs show around it

Session 2026-10-29 (day install step123, `brief3d:1`, `vscreen:1;vsearly:1`; clocks: `xr-stereo.log` and `ee-camera.log` both use
GetTickCount ms) [E]:

| tick (s) | event | source |
| --- | --- | --- |
| 99551-99658 | m02a briefing (ROSELEIN overview + zoom): `brief3d-fit` at 99555.6, SelMap 1/frame, icons 63 then 2/frame | ee-camera `summary-brief3d` |
| 99671.6 | flight 1 installed (`stage=m02a`) | xr-stereo |
| ~100538 | last flight composite; `summary-vscreen;mode=flight` -> `mode=screen` by 100543.7 (virtual screen) | xr-stereo |
| 100546.4 | `flight-stage-ended;flight=1` (8 s after the last composite) | xr-stereo |
| 100538-100688 | **debrief / results / save / next-mission cinematics** (the user confirms the debrief is in this session); vscreen `mode=screen`, `content=1`, 60 fps; step5 producer silent (no lines 100536-100697); `present-stats moving=0` | all |
| 100688-100710 | the only post-flight map phase: `brief3d-fit` at 100692.6 (`screens=1`, a new screen), 898 frames published and blitted | ee-camera, xr-stereo |
| 100710-100765 | no map draws; mission menu / hangar (the user picked the F-5E) / loading | ee-camera |
| 100765.5 | flight 2 installed (`stage=m03k`) | xr-stereo |

## 2. What the producer saw (per 5 s window deltas of `summary-brief3d`)

### 2.1 The debrief window 100538-100688 s [E]
`frames=5884;selmap=6058;icons=36147;icons-unverified=870;refused=1044` in EVERY line from 100522.3 to 100687.6 (34 windows): zero
SelMap draws, zero ModelIcon draws, zero refusals. `Producer::draw` counts `selmap`/`icons` before every refusal
(`brief3d_live.h:153`) and `brief3d::classify` admits only the exact briefing classes, so: no indexed SelMap line draw with PS
`9ebdf113` and no indexed ModelIcon triangle draw occurred anywhere in the debrief/results screens. The table consumer's `no-image`
grew 4089 -> 15345 there because there was no map frame to show (no slot), not because a table was refused.

### 2.2 The map phase 100688-100710 s [E]
`selmap=68/301/300/296` per window (one grid per frame), icons per frame 5.1 / 6.5 / 6.1 / 5.5, `refused=341+394` with
`last-why=brief3d-eye-inside-volume` (whole frames refused while the head was inside the table volume, as in the m02a briefing),
content scale changes 26 -> 55 (`brief3d-content;top-units=39.66` -> `19.87` -> `0`). Consumer: `blits` 5884 -> 6782 (= every
published frame), `layers` 8916 -> 10397, `no-image` frozen at 15582: **the table was presented** during this phase.

### 2.3 Why the map phase is the m03k briefing, not the debrief [I, strong]
* Unit records in the briefing files [E: `D:\acz-extract\mis-src\data\brdata`, 52-byte records from `Cipher`]: `m02a.bfn` 3552 B =
  **63 records** = exactly the 63 icon draws per frame of the m02a overview; `m03k.bfn` 736 B = **10 records** (Cipher, Pixy, 2 F-4E,
  4 MiG-21bis, 2 unnamed) at altitudes 6000-20000 (m02a: 0-4000).
* Map phase: 5-6.5 icons per frame (<= 10), and the content top latched at 19.87 units (= 1.1 x ~18 raw) on the overview scale where
  m02a's 4000 is 3.16 units: ~18 units = ~23,000 altitude. m03k's aircraft at 20,000 (15.8 units) fit; nothing in m02a does.
* Route: the next mission's overview is always shown before its mission menu (D-89/D-90 route, m02a here); the map phase is the only
  SelMap phase before the m03k flight, and it starts right after a 2 s+ screen gap (`screens=1`). If it were the debrief, the m03k
  briefing would have drawn no map at all.
Conclusion: the debrief happened inside 100538-100688 and drew none of the briefing map classes. What it did draw is [U].

## 3. Briefing vs debrief

| aspect | briefing (known, brief3d.md) | debrief |
| --- | --- | --- |
| terrain | `DrawSelMap_v` indexed line grid 128x128, one draw per frame, 64-unit model box | [U]; not the briefing's classified grid [E] |
| units | `DrawModelIcon_v` lit icon models (63 on m02a), 4 matrices per record, matrix 0 = map camera | [U]; not classified ModelIcon draws [E]; unit markers may be 2D (`opm_icon.gim`, 624 B) [I] |
| flight path / kill markers | none | [U] (typical of the series' debriefs [I, not evidence]); candidates: line draws (`ps2Line_v`, `t2`) or 2D strips |
| text / panels | `ps2_v` panels on the virtual screen | `ps2_v` + the debrief font/button/message sheets [I] |
| camera | game camera orbits; the table ignores it after the fit | [U] |
| narration | `caption/...` | `caption/debri/<lang>/mNNcap.bin` subtitles [E file list] |
| our layer today | screen quad + 3D table (brief3d) | screen quad only (vscreen `mode=screen`), unchanged |

## 4. Reuse and new work (decided by the histogram)

**Case A: the debrief uses `DrawSelMap_v`/`DrawModelIcon_v` with another topology, PS or non-indexed** (the histogram's `window`
line already counts SelMap/ModelIcon draws by VS whatever the topology: `selmap=`/`modelicon=` > 0). Reuse almost everything:
`classify` gains the observed (topology, indexed, PS) tuple (one line + tests); root layout check, t10/b10 reads, the fit (box,
camera centre, handedness), placement, content-aware scaling, eye guard, replay, the 3-slot ring, consumer and all GPU lifetime
invariants 1-11 of brief3d.md section 8 unchanged. New: a non-indexed draw path in `brief3dDraw` (today it is called from
`DrawIndexedInstanced` only; `DrawInstanced` needs the same seam), and the fit for a grid that may not be 128x128.

**Case B: the debrief map is emulated-GS 2D (`ps2_v` / `ps2Line_v`, pre-projected by the emulated VU).** No matrix to replace: the
replay route does not apply. Route = build the table ourselves (briefing-3d-map.md option B): terrain from our baked stage
(`.zvrs`, already loaded per flight) rendered as a small relief; units from `.bfn`; **flight path and kill positions** from our own
record of the flight (the E6 camera/aircraft position the producer already reads every frame, stored per flight: a bounded ring of
positions + kill events from the existing marker/kill tracer) or from an EE structure the debrief reads (needs the EE snapshot
search, section 5 step 6). The placement, eye guard, ring/consumer and layer composition of brief3d are reused; the producer is new
(our own draws, not a replay), so its GPU lifetime rules must be re-derived (no game list involved: our own command list and fence).

**Case C: a mix** (3D terrain grid via SelMap, 2D path overlay via `ps2Line_v`): Case A for the grid, path lines would stay on the
virtual screen (flat) until a Case-B path source exists.

Do not choose before the histogram: the classifier must not be widened beyond what a live line shows.

## 5. Read-only capture for the user's debrief session

Diagnostic changed in this branch (default off, file-armed, bounded): `briefing_observe.h` `format_rest` + `ee_camera_probe_win.cpp`:
an armed window now logs, after `event=window` and `event=top` (top 8 keys), up to **3** `briefing-observe;event=rest;from=<rank>;
keys=<n>;overflow=<n>;items=<vs8/ps8/t<topo><i|n><g|->/draws/units/max[/name]>,...` lines (each < 448 B) with every other key of the
window (at most 32 keys kept; more = `overflow`). New names: `ps2` (534f29ca, ps2_v/ps2AW_v), `movie` (15c8c0bd), `extra2`
(a16573be). Offline test `briefing_observe_offline` (116 checks, incl. all 24 lower ranks printed exactly once, bounded lines, no
loop on tiny buffers). Needs the next install; with the CURRENT install (step123) the `window` + `top` lines already work.

Procedure (no config change; the day install as is):
1. Before finishing a mission, create the empty files `%LOCALAPPDATA%\acz-vr\briefing-observe.arm` and
   `%LOCALAPPDATA%\acz-vr\brief3d-capture.arm` (both read only; the second only logs if a SelMap draw appears).
2. Finish the mission. On EACH post-mission screen (debrief map, results/rank, save) stay **~30 s without input**, take a monitor
   screenshot (Win+Shift+S) at the start and the end, and write down the wall-clock time of each screen change.
3. While on the debrief, create `%LOCALAPPDATA%\acz-vr\png-screen.now` once (the XR screen PNG, vscreen.md 9).
4. If the debrief has an animated replay (a moving path), let it run to the end once.
5. Proceed or quit; delete both `.arm` files.
6. Optional (EE data for case B, large file): while the debrief is shown, Task Manager -> Details -> `acz.exe` -> right click ->
   "Create memory dump file" (read only; several GB in %TEMP%); keep it locally and tell us the path, do not upload.

Send back: `%LOCALAPPDATA%\acz-vr\logs\ee-camera.log` (the `briefing-observe;event=window|top|rest` lines and `brief3d-capture` /
`summary-brief3d` lines), `xr-stereo.log`, the `png` folder, the screenshots with their times.

Reading the result: windows during the debrief with `selmap>0` or `modelicon>0` = case A (record topology/indexed/PS from `top`/
`rest`, and the `brief3d-capture` line); otherwise case B: look for keys absent from the results/menu windows (a `t2` line key,
`ps2Line_v` hash, a debrief-only VS) and their draws per present (a growing count during a replay = the path).

## 6. Implementation plan

| stage | content | tests | live check |
| --- | --- | --- | --- |
| D0 (done) | this note; `briefing-observe` rest lines | `briefing_observe_offline` | the capture of section 5 |
| D1 | classify the debrief from the capture; decision entry (case A/B/C) | - | - |
| D2-A | `classify` + non-indexed seam (`DrawInstanced`) for the observed SelMap/ModelIcon variant; debrief screen latch (re-fit) | `brief3d_offline`, `brief3d_live_offline` with the observed tuple; refusals unchanged | null driver: `brief3d-fit` on the debrief, `summary-brief3d` selmap/frame; headset: table + path |
| D2-B | flight track recorder (bounded ring per flight of aircraft positions + kill events, CPU only), table builder from `.zvrs` + `.bfn` + track, own producer (one list, own fence) feeding the existing table consumer | offline: ring bounds, track decimation, table transform reuse (`place`, eye guard), budget | debrief: path and kills on the table match the 2D screen |
| D3 | lifetime review (Astra) of any new producer; cost in the menus | as brief3d.md 8 | exit from the debrief clean |

Risks: (1) the debrief may animate the path (draw counts change per frame): the replay must not latch a fit on a partial path (re-fit
rule); (2) a 2D debrief (case B) needs our own flight record; it can only show what WE recorded (same flight, same session), not
older saves; (3) the head-inside-volume refusals seen in both map phases (`last-why=brief3d-eye-inside-volume`, 341-684 refused
frames per 5 s while leaning in) also apply to a debrief table; (4) without a screenshot of the debrief, its content (path, kills,
replay) remains [U] and must not be assumed.

## 7. 2026-10-30: the post-m03k screen captured (live-1508 census) - it IS the debrief, it is 2D (case B); table core implemented, glue pending

Source (read only): `D:\acz-evidence-archive\headset-2026-10-30-session\live-1508\` (day install step134, real headset; cfg `...;brief3d:1;
fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1`). `ee-camera.log`: 2941 `event=ui3d-census` lines, 60 sampled frames at 1/s, ticks 167,655.4-167,715.8 s,
`latched;reason=samples` at 167,715,834 (`ui3d-census.arm` + `ui3d-census-acm.arm`). `xr-stereo.log` / `ee-camera.log` summaries for the
timeline. No screenshot or XR dump of this screen exists (the `png` folder is from 14:44). Tags as above.

### 7.1 Which screen it was: the post-mission DEBRIEF of m03k, not the next mission's briefing
* Timeline [E]: `flight-stage-ended;flight=2` (m03k) at 167,590.4 s; 167,590-167,643 `summary-ui3d` frozen with `held=1`,
  `last-refusal=ui3d-frame-has-3d|ui3d-frame-has-other-shader` (the post-flight sequence still drew 3D / other keys; not characterised);
  from ~167,643.3 s to the END of the log (168,049 s, ~406 s) one screen whose every frame is a pure ps2_v frame (UI3D frames +601 per 10 s,
  all refused, 7.2); the census sampled it 12-73 s after it appeared.
* Not a briefing [E]: the brief3d producer's `selmap=3799;icons=34809;refused=25322` are constant in EVERY `summary-brief3d-producer` line
  from 167,572 s to 168,048 s, and the table consumer's `blits=10126` likewise: no `DrawSelMap_v` and no `DrawModelIcon_v` draw at all after
  the flight. Every briefing overview captured so far draws the SelMap grid every frame (R4, K9, the m02a/m03k briefings of 2026-10-29).
  The census agrees: `other=0;other-keys=0;acm=0` in 60/60 frames.
* Debrief content [E layout, I meaning]: a large framed MAP WINDOW (60/60 frames, identical), a header bar and a corner piece on its border,
  a label inside its lower right, map content inside it, and ONE line of small glyphs (z 1) directly below it (GS y 2180-2204, starting at
  the window's left edge) = the narration subtitle line (`caption/debri/<lang>/mNNcap.bin`, section 0). The content animates (7.2) for
  400+ s. The user's "briefing screen" is this screen. [U]: whether the narration had finished; what the map shows (terrain, tracks, units).

### 7.2 What it draws [E: per-draw census lines; the census logs the first 48 ps2_v draws of frames holding 51-80]
Every draw: VS 534f29ca / PS d34ee153, target `0x1cf88df62b0` 2560x1440 fmt 10, PSO RTV 10, DSV 20 (view 19), viewport 0,0,2560,1440,
scissor 320,0,2240,1440, stencil enabled, 1 instance. Two cb0 mappings: GS `rc0:682,-448,2048,2048` (as the menus / hangar) and pixel-space
`rc0:2560,-1440,1280,720`. Blend codes are D3D12_BLEND (5 SRC_ALPHA, 6 INV_SRC_ALPHA, 2 ONE, 1 ZERO); depth 7 GREATER_EQUAL, 8 ALWAYS.

| order in frame | draws | mapping | idx | indices / vertices (VB view / 64) | blend / depth | reading |
| --- | --- | --- | --- | --- | --- | --- |
| first, 4-33 (varies) | fills | pixel | yes | 4 / 2 | off / GE + write | emulator sprite fills [I]; vertices unread |
| next | 1 | GS | yes | 219-985 / 214-976 = 3-11 strips of 75-121 vertices | 5,6 / GE + write | long strips, size changes every second, not monotonic [U: terrain rows? tracks?] |
| next | 1 | GS | yes | 49-247 / 40-245 = 10-61 quads | 5,2 / ALWAYS | sprite batch, count varies (blinking markers?) [U] |
| 11 | 1 | GS | no | quad x 1817..2278, y 1880..2168, z 16,777,216, q 1 | 5,6 / ALWAYS | **the map window** (px 412.9..2143.3 x 180..1105.7 = 90 % x 64 % of the 4:3 picture) |
| 12-31 | ~20 | GS + pixel | mixed | GS quad batches of 14 / 78-81 / 308-312 / 11 / 16 / 6 quads; pixel 5- and 4-vertex draws | 5,2 or 1,6 / ALWAYS | map content; the 308-312-quad batch (1,232-1,248 vertices) changes slowly (312 -> 308 -> 309) [U: map tiles? 24 x 13 = 312] |
| among them | 3 | GS | no | header 1817.4..2277.5 x 1880.5..1895.5; corner 1809..1841.5 x 1872..1904.5; label 2167.5..2267.5 x 2143.5..2157.5 (z 16,777,216) | 5,2 / ALWAYS | window decoration |
| last | 15-18 logged | GS | no | glyph quads at y 2180-2204, z 1, q 1 | 5,6 / ALWAYS | narration line |

* Topology [I, strong; the IA topology was not logged]: every GS indexed draw satisfies `indices = vertices + strips - 1` with whole strips of
  4 vertices for every batch (n = 4k + (k-1): 49/40, 69/56, 389/312, 1559/1248, 54/44, 79/64, 29/24, 9/8, ...) = triangle strips with one
  cut index per strip: sprite quads, plus the long-strip draw. The pixel-space draws (4/2, 7/5, 6/4, 8/4) do not fit and stay [U].
* Indexed vertex contents are unread (`v=?(u3c-indexed-unread)`): where the map content lies, whether its z and q are constant (a flat 2D map)
  or vary (an EE-projected perspective view), and which batch is the player's track are [U]. All non-indexed quads have q = 1.
* UI3D (ui3d:1) refused every frame of the screen (`summary-ui3d` frames 14,333 -> 38,376, refused 160 -> 24,203, `held=0`,
  `last-refusal=ui3d-pso-rtv`): the screen was shown on the flat vscreen (`mode=screen`), as designed (stage 2 replays no indexed draw).

### 7.3 Route: case B (pre-projected 2D) - lay the map window on a table and re-render its own draws per eye
The briefing-map classes are absent, so brief3d's matrix replay does not apply: there is no world/model matrix, only GS screen coordinates.
Chosen presentation (`dbr3d.h`): the map window becomes a horizontal TABLE in front of the user (placed like brief3d: `dbrw` wide, `dbrd`
ahead, `dbry` below the head at the screen's first frame; depth from the window's own aspect on the 16:9 image: 1.0 x 0.535 m by default).
Each ps2_v draw whose vertices lie wholly inside the window is re-rendered per eye from a vertex-buffer copy whose x, y, q are rewritten so
that the game's own shader, texture and blend put each vertex where its table point projects (the UI3D technique on a horizontal plane: NDC
from x, y; q' = q / w_eye for perspective-correct attributes). Small sprite batches (bbox <= 12 % of the window in both axes) are lifted
`dbrl` above the top (markers); the window quad itself is the table top; everything else (pixel-space draws, the narration line, the border
decorations) stays on the flat screen. Fail closed: another shader key, an unread or non-finite vertex, or a vertex with q != 1 (perspective
content cannot be laid flat) refuses the frame (flat screen only); a draw straddling the window border stays flat; a changed window rectangle
is a new screen (the table is placed again). The game's index buffer is reused; only vertices are copied (both eyes, at most 2 MiB per
frame; the live frame needs ~3,600 table vertices).
* Not chosen: a flight track recorded by us (section 4 case B: the debrief already draws whatever it shows; a recorder adds a second source
  of truth); un-projecting an EE perspective view (only if the GEO capture shows varying q, 7.5).

### 7.4 Implemented vs pending
* **Implemented (offline only; nothing ran in the game)**: `src/acz-vr/dbr3d.h` (options `dbr3d:<0|1>` default 0, `dbrw` 0.2..4 m default 1.0,
  `dbrd` 0.2..5 default 0.75, `dbry` -2.5..1 default -0.45, `dbrl` 0..0.3 default 0.03; window signature and latch; the online role assigner;
  table geometry; per-eye vertex rewrite; eye guard; upload budget; a `summary-dbr3d` line). `xr_stereo_proxy.cpp` removes the tokens before
  the other parsers and logs `xr-stereo;event=dbr3d-option;ok=1;dbr3d=1;...;prerequisites=ok;live=off(glue-pending)`: **`dbr3d:1` changes
  nothing live yet**. ctest `dbr3d_offline` (98 checks: options incl. the step134 day value + `;dbr3d:1` <= 255 characters; the window
  signature on the census values (window accepted; header, corner, glyph, top band, pixel-space, indexed, q 2, skewed, ACM rejected); latch
  and new screen; the assigner on a census-shaped frame; table aspect / orientation / never mirrored / head-yaw placement; rewrite: exact
  centre, perspective q, marker lift, stereo disparity, culling, every fail-closed reason; eye guard; budget; summary).
* **Pending (D2-B glue, after the GEO capture)**: a producer frame kind `debrief` in `brief3d_live.h` (the ring / slot / list / fence lifetime
  of the UI3D frames) that admits the INDEXED ps2_v draws (UI3D refuses them), reads each draw's referenced vertex range from the mapped upload
  VB (as the GEO reader does), rewrites it per eye with `dbr3d::rewrite_vertices` and replays the game's indexed draw with the copied VB and
  the game's own IB. The frame opens at the first draw of a debrief frame (the long-strip draw precedes the window quad: the latched rectangle
  is kept across frames); the slot is composited OPAQUE over the vscreen like the UI3D frames; GPU lifetime review (Astra) before any install.

### 7.5 Read-only diagnostic added, and the next capture
* `ui3d_census.h` / `ui3d_census_hooks.inl`: every `kind=draw` line now ends with `;ia=<D3D_PRIMITIVE_TOPOLOGY>` (5 = triangle strip expected),
  and the first 16 INDEXED menu draws of each sampled frame get a `kind=geo` line: `ia`, the PSO strip-cut enum (`cut=1` = 0xFFFF expected),
  `ibs`, `base`, `ni=<read>/<draw>`, `cuts` (marker count), `imin`/`imax`, `nv` (vertices read, `t` = capped at 2048), the x / y / z / q ranges
  of every referenced vertex, the pixel bbox and the first 3 vertices in index order; an unreadable case prints `geo=?(reason)`
  (`u3c-ib-address-unresolved`, `u3c-ib-range`, `u3c-vb-read-fault`, ...). Reads only through `step3MappedSpan_locked` + `guardedCpuRead`
  (one index copy <= 16 KiB into a static buffer under the hook-state mutex, then one 16-byte copy per vertex); nothing is written to a list,
  the GPU or the game. Armed with the census (no new file). ctest `ui3d_census_offline` 290 checks (`test_geo`: strip-cut scan, 32-bit markers,
  only markers, bad stride, non-finite vertex, the line with its exact pixel bbox, failure lines, bounded worst case, emitter placement and
  line budget).
* **Next capture (needs the next install; the user or the null driver):** finish a mission (m03k again is best), then on the debrief map
  screen create `%LOCALAPPDATA%\acz-vr\ui3d-census.arm` once (60 frames, ~1 min), take a Win+Shift+S screenshot at the start and at the end
  with wall-clock times, create `png-screen.now` once (the XR screen dump), then delete the arm file. Send `ee-camera.log`
  (`ui3d-census;kind=frame|draw|geo|other`), `xr-stereo.log`, the png folder and the screenshots. It decides: (1) `ia` / `cut` (strips with
  cuts?); (2) per batch, whether x / y lie inside the window 1817..2278 x 1880..2168 (on the table) and its size (the marker rule);
  (3) **q and z ranges: q = 1 and constant z = a flat map (7.3 as implemented); varying q = an EE-projected perspective view (the long-strip
  draw with depth write is the candidate): the frame then refuses (`dbr3d-perspective-content`) and that draw needs its own route**; (4) which
  batch moves like a replay (the track) over the 60 x 1 s samples; (5) the screenshot ties the batches to what is visible. The pixel-space
  fills' vertices stay unread by design (their GS-index ratio is not a strip pattern; they stay flat).

### 7.6 Null-run checks (for the next install; nothing below has run)
1. Day cfg + `;dbr3d:1`: `xr-stereo.log` shows `event=dbr3d-option;ok=1;dbr3d=1;dbrw=1.00;dbrd=0.75;dbry=-0.45;dbrl=0.030;prerequisites=ok;
   live=off(glue-pending)`, and every other option line is unchanged (brief3d / ui3d / pov / recenter parse as before).
2. A bad value (`;dbrl:1`) logs `dbr3d-option;ok=0;detail=bad value for dbrl (0..0.3 m);effect=ignored`; the rest of the stage still parses.
3. Census armed in a menu: `kind=draw` lines end with `;ia=<n>`; indexed draws are followed by `kind=geo` lines (values or `geo=?(reason)`);
   no frame-time spike beyond the census's own once-per-second sample (`present-stats max-gap-ms`), no `Log::fail`, latch after 60 frames.
4. Census not armed: no `ui3d-census` line and no change in `present-stats`.

## 8. 2026-10-30 (evening): stage 2 live glue - the results panel floats, the map scene wraps around (`dbr3d:1`)

Offline work only: nothing below ran in the game, SteamVR or a headset, and it was not compiled when this section was written (the build waits
for the main session's "build go"). Tags as above.

### 8.1 New evidence and what it changes
* [E] `D:\acz-evidence-archive\headset-2026-10-30-session\live-1936-step137\acz-vr-xr-screen-t1325274-screen.png` (png-screen of the XR flat
  screen, rotated 180; upright copy `debrief-upright-small.png`): a **MISSION RESULT** panel (MISSION 03K, rank, time 08'07", 10000 points,
  TARGET DESTROYED list, YELLOW TARGET, ACE STYLE bar), the button line `A OK | RB FAST-FORWARD | X DISPLAY OFF` above it and the narration
  line below it, all over a **perspective wireframe grid** (lines converging, clipped to the 4:3 picture) with a grey **flight-path ribbon** and
  the labels PIXY / PLAYER seen through the semi-transparent panel.
* [E, same image] the panel spans ~90 % x 64 % of the 4:3 picture: the census "map window" (7.1/7.2, GS 1817..2278 x 1880..2168) **is the
  results panel**, not a map. Hence [I, strong]: the draws before it in the census order (pixel-space fills, the long-strip draw with depth
  GREATER_EQUAL + write, the additive sprite batch) are the map scene (grid, paths, labels); the 308-312-quad indexed batch after it is the
  panel's text (its quad count matches the glyphs of the result table).
* [I] The grid is a perspective projection by the game's debrief camera. Laying that image flat on a table (7.3, stage 1) would paint a
  trapezoid on a table; stage 1 is therefore NOT used live (kept with its tests).
* [E] `live-2210-step143` (step143 day install, ended on the debrief) `summary-ui3d` during the debrief: frames / refused / rejected +300 per
  5 s, `last-why=ui3d-indexed-draw`, `last-refusal=ui3d-pso-rtv`: every debrief frame OPENS a UI3D frame and is rejected at its first indexed
  draw; [I] the pso-rtv refusals are the final pass to another target, met while no frame is open. No `kind=geo` line exists in live-1936 or
  live-2210 (the census was not armed on the debrief): the indexed vertex values are still [U].

### 8.2 Route (dbr3d.h stage 2, brief3d_live.h `Producer::dbr3d_draw`, ui3d_replay_hooks.inl)
| step | rule |
| --- | --- |
| arming | `ui3d_draw` (and `dbr3d_draw`) test every non-indexed 4-vertex ps2_v draw with the census window signature (`is_window_quad`: GS mapping, SRC_ALPHA / INV_SRC_ALPHA, z >= 1.6e7, q 1, >= 60 % x 40 % of the picture). Seen within `kArmMs` (1 s) = armed. The first debrief frame is still rejected as before (the window comes after the indexed draws); from the next frame on the route is active. |
| routing | armed: the glue reads every ps2_v draw with its vertices (non-indexed: its own range; indexed: the index range read twice, scanned with the PSO strip-cut value, then the referenced range [base + imin, base + imax] read twice) into a per-thread heap scratch and calls `dbr3d_draw`. Non-ps2_v draws still go to `ui3d_draw` (hold + rejection of the open frame). |
| frame | a DEBRIEF frame is a UI3D frame (`r.ui && r.dbr`): same ring / slot / list / fence lifetime, any refusal rejects it (flat screen for that frame), published OPAQUE at its list's Reset, consumed by the existing table consumer. It opens at the frame's first on-target ps2_v draw with a pose and a vscreen placement of the same space generation. |
| split | game order, no look-ahead (`dbr3d::Router`): indexed or pixel-space draws before the first non-indexed GS-mapped draw = MAP; that draw (the panel quad) and every later draw = PANEL. |
| map | re-rendered per eye on the WRAPAROUND: the unscaled vscreen placement with the cylinder rule (`ui3d::surface_point` with curve), horizontal radius `dbrr` (4 m) about the head at placement, angles `dbrk` (1.4) x the flat screen's; world-locked. The game's z is kept (its own GREATER_EQUAL depth test on our eye depth image, cleared to 0 as the game's); the game's scissor is carried onto the surface. |
| panel | on the UI3D panel layers at the u3s-scaled vscreen placement (as the menus): layer 0 = the panel quad / large draws, 1 = medium, 2 = text (a batch of more than 4 vertices is classed by its largest 4-vertex quad); the layer never decreases in a frame. Layer distances u3mn (1.1 m) .. min(u3mf, 0.75 dbrr) = 3 m: the panel always lies in front of the map it covers. |
| replay | the stage-2 sequence with our vertex copy as VB 0; indexed: `DrawIndexedInstanced(game count, 1, game start index, -imin, startInstance)` on the game's still-bound index buffer (vertex i = copy[i - imin]); a PSO with a DSV binds the slot's eye depth image. Copies use a new per-slot span (`kDbrUploadBase`, 2 MiB per slot; the upload buffer grows by 6 MiB). |
| fail closed | depth other than ALWAYS / GREATER / GREATER_EQUAL, depth without the DSV, stencil not ALWAYS, an unknown strip cut or one not matching the index format, > 8,192 indices, a referenced span > 4,096 vertices, a negative base, a torn / unreadable index or vertex range (default-heap buffers are unreadable), the 2 MiB span full, a non-finite / q <= 0 vertex: the frame is rejected (the flat screen, as today). |

### 8.3 Files
* `src/acz-vr/dbr3d.h`: options `dbrr` (1.5..20, default 4.0) and `dbrk` (0.5..2.5, default 1.4); `live_prerequisites` (adds `needs-ui3d:1`
  and the present hook); `window_from_vertices`, `scan_indices`, `Router` / `quad_max_dim`, `wrap_options`, `panel_options`, `rewrite_surface`
  (ui3d `rewrite_at` in 64-vertex chunks, up to 4,096 vertices), `LiveStats` / `format_live` (the `summary-dbr3d` line).
* `src/acz-vr/brief3d_live.h`: `DbrFacts` / `DbrPlan`, `configure_dbr3d`, `dbr3d_armed`, `dbr3d_draw`, `dbr3d_commit` / `dbr3d_cancel`,
  `format_dbr3d`; the window observation in `ui3d_draw`; debrief frames in `on_present`, `ui_reject_locked`, `settle_locked`; `UiFacts` gains
  ps8 / blend / depth_write; `kUploadTotal` = 3 x 192 KiB + 3 x 512 KiB + 3 x 2 MiB.
* `src/acz-vr/ui3d_replay_hooks.inl`: `DbrScratch`, `dbrFacts_locked` (read only: pinned mapped UPLOAD spans + two agreeing guarded copies),
  the debrief recording branch of `ui3dDraw`.
* `src/acz-vr/xr_stereo_proxy.cpp`: the `dbr3d-option` line now configures the producer (`live=producer-configured(wraparound+panel)` or
  `live=off`).
* `src/acz-vr/ee_camera_probe_win.cpp`: one `summary-dbr3d;...` line per 5 s window in ee-camera.log while configured.
* Tests: `dbr3d_offline` (stage 1 unchanged + stage 2: options on the step143 day value, the index scan, the window signature, the census
  frame through the router, quad sizes, wraparound geometry (cylinder radius / azimuth exact, stereo disparity, chunking identical to
  per-vertex, scissor, culling, failures), panel distances, summary bounds); new `dbr3d_live_offline` (the producer with fakes: arming and the
  first rejected frame, a census-shaped debrief frame published OPAQUE with map / panel / text plans, rebased indexed replays and upload
  offsets, nine refusal kinds each rejecting the frame, another list, a movie draw rejecting + holding, the 2 MiB budget, no pose / no
  placement); `brief3d_live_offline` updated for the larger upload buffer.

### 8.4 Still [U] or not done
* Not compiled or run when written; the offline tests are fixtures shaped on the census, not live values.
* The indexed vertex values (whether the grid / path strips have q = 1, where they lie) are unread live. The wraparound shows whatever the game
  drew (it does not need q = 1), but a long primitive (a grid line across the picture) is a straight chord of the cylinder, nearer in its
  middle than the strips around it [I: a visible depth artefact at most, cos(40.4 deg) = ~0.76 x dbrr at the middle of a full-width line with dbrk 1.4 and the live 3.20 m / 1.97 m screen].
* `DISPLAY OFF` (X: the panel hidden) has no window quad: the route disarms after 1 s and those frames return to the flat screen (stage-2
  rejection). FAST-FORWARD and the replay animation are redrawn draws, handled frame by frame.
* The pixel-space "fills" (4 indices / 2 vertices) and the strips' IA topology are [U]; they replay with the game's own PSO and topology.
* GPU lifetime review (Astra) of the indexed replay before any install; cost on the debrief (~3,600 vertices x 2 eyes rewritten on the CPU per
  frame, two guarded copies of each range) to be measured (`present-stats`).

### 8.5 Null-run cfg and checks (next install; nothing below has run)
cfg = the step143 day install (`live-2210-step143\runtime.cfg`) with only the terrain-stage line changed (188 characters; the 255-character
buffer holds it; `;dbrr:4;dbrk:1.4` = the defaults):
```
xr_terrain_stage=auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1;wcloud:1;fxtrail:8;dbr3d:1
```
All other keys unchanged (`xr_hud_layer=true`, `ee_camera_probe=true`, `xr_stereo_output=true`, ...).
1. `xr-stereo.log`: `xr-stereo;event=dbr3d-option;ok=1;dbr3d=1;dbrw=1.00;dbrd=0.75;dbry=-0.45;dbrl=0.030;dbrr=4.00;dbrk=1.40;prerequisites=ok;
   live=producer-configured(wraparound+panel)`; the brief3d / ui3d / pov lines unchanged. A bad `;dbrr:1` logs `ok=0;detail=bad value for dbrr
   (1.5..20 m);effect=ignored`.
2. Menus, hangar, briefing, flight: `summary-dbr3d;armed=0;arms=0;...` in ee-camera.log every 5 s; `summary-ui3d` and `present-stats` as before
   (no new rejections; one relaxed load more per draw while not armed).
3. On the debrief (finish a mission; m03k is the known case): `summary-dbr3d;armed=1;arms>=1;screens>=1`, `frames` / `published` growing
   ~60/s, `map-draws`, `panel-draws` and `indexed` > 0, `rejected` small (the first frame), `last-why` named if any; `summary-ui3d` `published`
   grows instead of `rejected`. Create `png-screen.now` (the flat image) and use `xr_png_once` (the eye images; swap `xr_edge_feather` as in the
   null-capture routine) to see the wraparound and the panel; also arm `ui3d-census.arm` once on the debrief for the still-missing GEO lines
   (7.5).
4. Press X (DISPLAY OFF): within ~1 s `armed=0` and the flat screen; X again: armed again. Leave the debrief: `armed=0` within 1 s, the next
   screens as before, no `poisoned=1`, no `Log::fail`.

## 9. 2026-11-01: why AGU1 never reached the debrief, and the null route (`runBatchAG.ps1 -Debrief`)

* [E] The mission DID end in step174 runAGU1 (`D:\acz-evidence-archive\step174\regression\runAGU1`, gameshots g0066..g0115): the Gun autopilot crashed at 03:49:18 ("Cipher's crashed!", the crash camera), then the game showed the crash REPLAY
  ("PRESS MENU BUTTON TO EXIT REPLAY", angles cut every ~3 s with black fades) until the run closed 150 s later. The old post-wait tapped ENTER (`confirm`) every 45 s; Enter never left the replay (three taps, the screens before and after are the replay).
  `summary-dbr3d` stayed `armed=0;arms=0;screens=0` for the whole run: the debrief was never drawn.
* [E] Pause menu items (step124 runJ3 g0048): RETURN TO GAME / REPLAY / RETRY / QUIT MISSION / FLIGHT OPTIONS. The cursor does not always start on the first row: in AGU1 it sat on RETRY (g0030) and on QUIT MISSION (g0032..g0044), because the
  autopilot's S/W taps move it while paused. RETRY restarts the mission, so any pause-menu navigation must read the highlighted row from the image first.
* [I] The menu button is Esc (`back`, the key that pauses in flight). Esc on the replay is UNVERIFIED until the first `-Debrief` run; what follows the replay (a results/debrief screen, or a failed-mission menu) is also unverified.
* Route (`-Debrief`, run DBR1; window-image classifier `research\experiments\step83-verify-run1\debrieflib.ps1`: BLACK / REPLAY / PAUSE + highlighted row / OTHER):
  1. fly the AGU1 flight; wait up to `-DebriefEndWait` s for the REPLAY screen (an unflown aircraft also crashes); fallback, or `-DebriefQuit`: Esc, move the highlight to QUIT MISSION (row 3, verified by image), one confirm (what follows is UNVERIFIED);
  2. on a verified REPLAY screen: Esc (max 3, 10 s apart); 3. wait `-DebriefWait` s for `summary-dbr3d;armed=1` (unidentified screens only get screenshots; `-DebriefProbeConfirm N` allows guarded confirms on a static unknown screen);
  4. hold `-DebriefHold` s (png-screen.now every `-DebriefPngEvery` s, one png-eye.now, census re-armed every 15 s), then page with confirm only while dbr3d is armed and fresh (`-DebriefMaxPages`, `-DebriefPageHold`); a new `flight-stage-detect` aborts.
  `pwsh -NoProfile -File research\experiments\step83-verify-run1\runBatchAG.ps1 -Debrief -ExpectDxgi <8 hex> -ExpectD3d12 <8 hex>` (usual preflight: no game / SteamVR running, mem-guard). Analysis: `-Analyze <runDir> -Kind debrief`
  (screen timeline from the gameshots, steps.txt DEBRIEF lines, `summary-dbr3d` / `summary-ui3d` / census / png counts). Offline check on runAGU1: 116 shots classified (REPLAY 23, PAUSE 12 incl. rows 0/2/3, BLACK 7), replay never left, dbr3d never armed.
