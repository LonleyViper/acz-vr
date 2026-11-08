# Stages for every mission and runtime stage detection (STAGES track)

Tags: **[E]** evidence (measured / read from data), **[I]** inference, **[U]** unknown. Offline work only: no game or SteamVR launch, no network,
read-only on the installed archives; game-derived outputs live in `D:\acz-extract` (never in git).

## 1. All 39 stages baked

`research/world-render-data/bake_all_stages.py` bakes every `data/map/<id>/` that has a heightfield (39 ids) in its own child process
(`stage_baker.assemble` with env + `objects_baker.assemble`), re-runs the loader checks on both outputs and writes
`D:\acz-extract\stage-all\<id>.zvrs|.zvro` + `bake-results.json` and the table `docs/world-render/stage-table.md`.

- [E] 39/39 stages bake; all 39 `.zvrs` are v3 with the env section; 39 `.zvro`. The m02a output is byte-identical to the earlier
  `stage-w2` / `stage` bakes (determinism check).
- [E] Two baker failures found and fixed (`objects_baker.py`): v01a/v02a/v06a ship 5-byte placeholder files whose content is the ASCII text
  `dummy` (`.wad .wpd .wbd`, all `_treeN.acm/.gim`) -> treated as absent (no trees); v08a ships ACM **version 3** tree models
  (all other 105 ACM files are v5) -> that tree type is skipped (1785 v08a tree placements have no model) instead of failing the stage.
  Selftests cover both.
- [E] Empty (4-byte) `.pdl` = no static props: m01a, m03*, m10*, m16*, r01a, v01a, v02a, v05a. Empty `.wpd` = no trees: k00a, m04*, m07a,
  m11a, m13a, m17a, m18a.
- [E] Identity classes (table column `ident`): the k/m/n variants of missions 03, 04, 08, 10, 14, 16 are byte-identical in terrain, env and
  objects. m03*, m10*, m16*, r01a and v05a share ONE heightfield (the B7R area); m03*, r01a and v05a also share tiles, palette and objects but
  differ in env. m17a/m18a share tile words, tiles and palette (heights differ slightly). 24 distinct terrains, 27 distinct envs.
- [E] v03a and v08a heightfields are all zero (ocean stages: deep blue tiles with painted islands); 12 stages get the baker's sea plane at
  y = 0, which is a **guess** (byte-0 cells > 10%, flagged in the file) [I]. No sea-level field was identified in the stage files [U].
- Previews (4x4 texels per tile): `D:\acz-extract\stage-all\preview\*.png`, contact sheet `_sheet.png`.

## 2. Mission -> stage mapping

Table (also machine-readable: `research/world-render-data/mission_stages.json`):

| # | title | operation | area (title card) | stage(s) |
|---|---|---|---|---|
| 01 | Glacial Skies | Crossbow | Valais Air Base | m01a |
| 02 | Annex | Roselein | Arlon | m02a |
| 03 | The Round Table | Choker One | B7R | m03k / m03m / m03n |
| 04 | Juggernaut | Offensive Campaign No.4101 | Futuro Canal | m04k / m04m / m04n |
| 05 | Flicker of Hope | Varsity | Solis Ortus | m05a |
| 06 | Diapason | Constantine | Directus | m06a |
| 07 | Bastion | Hell Bound | Mt. Ivrea | m07a |
| 08 | Merlon | Dynamo | Schayne Plains | m08k / m08m / m08n |
| 09 | Sword of Annihilation | Judgment | Tauberg | m09a |
| 10 | Mayhem | Battle-Axe | B7R | m10k / m10m / m10n |
| 11 | The Inferno | Cannibal | Hoffnung | m11a |
| 12 | The Stage of Apocalypse | Ravage | Waldreich | m12a |
| 13 | Lying in Deceit | Stone Age | Mt. Schirm | m13a |
| 14 | The Final Overture | Broom | Anfang | m14k / m14m / m14n |
| 15 | The Talon of Ruin | Valkyrie | ---- | m15a |
| 16 | The Demon of the Round Table | Thunderbolt | B7R | m16k / m16m / m16n |
| 17 | The Valley of Kings | Point Blank | Avalon Dam | m17a |
| 18 | Zero | ---- | Avalon Dam | m18a |
| SP | The Gauntlet | ---- | B7R | r01a |

Other stages: k00a (no title card, no briefing id) = the **airbase map of the takeoff segment** [E for mission 02: headset test 19 rolled
6.9 units above its flat 1840-unit runway plateau before the game cut to m02a; docs/world-render/takeoff-stage.md], drawn by the TAKEOFF
selector (option `takeoff:`, default k00a); v01a..v08a (no v07a) versus stages [I] (`VSstage.swa`, `v*` packs).
`data/mis/pack/f01a..f18a, fr1a` exist without a `data/map` entry: free-mission packs that reuse the stage of the same number [I].

Aircraft / player model: NOT a property of the mission data. The player picks the aircraft in the hangar (`data/selaircraft`, `data/mis/mpl/<type>.mpl/.pvb`);
the mission-02 runs used the J35J Draken (D-303). The cockpit model therefore stays a separate choice (`xr_cockpit_model`).

How the mapping was established:
- [E] One id namespace: every one of the 39 stage ids has `data/map/<id>/`, `data/mapset/map/mapset<id>.bin`, `data/effect/<id>/` and
  `data/mis/pack/<id>/`; the mission ids also have `data/radio/<id>/` (m01a..m18a, r01a). The game loads all of them by the same id. The
  mission pack objects are named after it (`m02a.mmb`: `m02a_GLUECK_5`, `m02a_KRITIKER_6`, ...).
- [E] Mission titles from the game's own title cards `data/mistitle/m01..m19.gim` (decoded to `D:\acz-extract\mis-src\mistitles-text.png`).
- [E] `data/brdata/BriefingID.bin`: 19 records (one per title card), each `(type, id0, id1, id2)`: type 0 = one briefing id, types 1/2 = three ids.
  The three-id records are exactly missions 03, 04, 08, 10, 14, 16 (the missions with k/m/n stages) and the ids run 0..30 = the 31 stages
  m01a..m18a + r01a in name order; record 19 (#SP) -> id 30 = r01a.
- [E] Independent geometry check: the four title cards that say area "B7R" (#03, #10, #16, #SP) are exactly the mission stages that share
  one byte-identical heightfield (section 1); the two "Avalon Dam" cards (#17, #18) are the two stages that share tiles and palette.
- [E] Screenshot verification of m02a: `research/experiments/step50-ps2-probe-run1/briefing-roselein.png` shows MISSION ROSELEIN, DATA NO. 02,
  1995/04/15, 10:20, AREA ARLON = title card #02 (Operation Roselein, 1020hrs 15 April 1995, Arlon); stage m02a then registered with the
  game's own view in flight (D-296, D-300). No screenshot of another mission exists in research/experiments (every live run so far was
  mission 02), so the second verification is the data-only B7R/Avalon check above, not a screenshot [E].
- [U] Which of k/m/n is which ace style (the briefing shows ACE STYLE SOLDIER on the single-variant mission 02). It does not matter for
  rendering: the variants are byte-identical.

## 3. Runtime stage detection (`xr_terrain_stage=auto`)

Constraint: read-only, no game-memory writes, no code patching, no new config key (the config is at 30 of 32 keys).

### 3.1 Candidates

(a) **D3D12 resource creations / texture uploads.** What the proxy sees today [E]: creation seams (CreateCommitted/Placed/Reserved Resource:
descriptions only, behind `diagnostic_ownership_pins`), Map/Unmap/WriteToSubresource on already-tracked buffers, CopyBufferRegion and
CopyResource; it does **not** hook `CopyTextureRegion` (the normal texture upload path). The archived logs (`D:\acz-evidence-archive\step62`:
diagnostics, step5-producer, xr-stereo) contain no texture creation or upload records: only render-target slots (2560x1440, 128x128).
A texture fingerprint would need a new list hook, reads of write-combined upload heaps, and knowledge of the format the PC port converts
the 8-bit PS2 tiles/palettes into (unknown; uploads are also 256-byte row-pitch padded). None of it can be verified offline [U].

(b) **A stage/mission index near the E6 camera fields.** Every live run so far was mission 02, so there is no second mission's dump to compare
(`ee-camera.log` hexdumps; the step50 PS2 probe values at +0x3FFD1C etc. are constant and random-looking, D-295) [E]. Untestable offline.

(c) **Config fallback.** `xr_terrain_stage=<id>` keeps working unchanged; `tools/live/vr-test-start.ps1 -Mission NN[k|m|n]|SP` replaces the
stage id of `-TerrainRender` from the mission table (section 2), e.g. `-Mission 07 -TerrainRender 'auto;env:1'` -> `m07a;env:1`.

**Chosen and implemented: a read-only scan of the game process's memory for the RAW stage files.** The game must hold the current stage's
data in CPU memory while flying [I: the heightfield for ground collision / altitude, palette and tiles for its own GPU uploads, mapset/sph for
its lighting], and raw file bytes do not depend on how the port converts textures. The scan follows the same rules as the existing E6/lens/PS2
scans (VirtualQuery, SEH-guarded copies, never a write). It is the most robust candidate that can be verified offline. Whether the PC port
keeps the files resident in raw form is **not yet observed** [U]; the first live `auto` run logs it (per-source hit counts).

### 3.2 Fingerprints (offline)

`research/world-render-data/stage_fingerprints.py` -> `D:\acz-extract\stage-all\fingerprints.json` (93,758 bytes; 638 signatures, 27 groups).
- Identity groups from `bake-results.json`: stages whose baked terrain + env + objects are byte-identical form one group (the k/m/n variants);
  the group representative (first id) is the file that is loaded (`m03k.zvrs` for m03m too: identical bytes).
- Signatures: 32-byte windows cut from `.mhd` (height-chunk rows), `.mti`, `.mtc`, `.mtd` (tile texel rows), `mapset<id>.bin`, `sph<id>.sph`;
  high-variety windows only (>= 6..10 distinct byte values), never crossing a 33-byte height row or a 64-byte texel row; 4 per source per
  group, the ones owned by the fewest groups first. Each signature lists every group whose files contain it (owners, found by searching all
  39 stages).
- Decision: each distinct signature found counts once for every owning group; the best group wins with >= 3 hits AND strictly more hits than
  any other group; otherwise "below-min-hits" / "tie" -> no terrain.

Offline verification (`stage_fingerprint_offline_tests --scan-files`: the real C++ Matcher over fake memory images made of a stage's raw files
separated by random bytes, `D:\acz-extract\stage-all\rawcheck`) [E]:
- all six sources present: **39/39** images select the right group (k/m/n -> the representative), e.g. m02a 24 hits vs 0; m03k 70 vs 66
  (r01a/v05a share its terrain and differ only in mapset/sph); m17a 40 vs 31 (m18a).
- heights + tiles only (`.mhd` + `.mtd`): 33/39 still match; the B7R group (m03*, r01a, v05a) and m18a-vs-m17a are refused as ties (identical
  or nearly identical terrain; only mapset/sph separate them).

### 3.3 Runtime (C++)

- `src/acz-vr/stage_fingerprint.{h,cpp}` (SDK-free): table parser (all-or-nothing; refuses unknown format/version, window != 32, bad hex,
  duplicates, bad or repeated ids, group index out of range, deep nesting) and the `Matcher` (8-byte-prefix hash table, ~500 MiB/s offline;
  windows across chunk boundaries are found, separate memory ranges never join). Signature bytes are stored XOR-masked, so a scan of the
  process cannot find the table itself.
- `src/acz-vr/stage_detect_win.{h,cpp}`: walks committed, readable regions (skips write-combined/uncached GPU mappings, executable images,
  this DLL and its own scan buffer) and hands them to the bounded scan core `stage_scan.{h,cpp}` (section 3.5). Before R-3 the walk read every
  region in address order with a budget of 8 GiB / 20 s.
- `xr_stereo_proxy.cpp`: when the stage id is `auto`, before loading: read `%LOCALAPPDATA%\acz-vr\stage\fingerprints.json`, scan (up to 3
  attempts 3 s apart while nothing at all matched), log `xr-stereo;event=terrain-auto;attempt=..;result=..;best=..;second=..;sigs-seen=..;
  sources=mhd=..,mtd=..;top=<stage:hits,...>;regions=..;bytes=..;ms=..`, then either `terrain-auto-selected;stage=..;identical-members=..`
  followed by the normal load of `<stage>.zvrs` (+ `.zvro` for props/trees) with all options of the value, or
  `terrain-disabled;reason=auto-no-match|auto-ambiguous|auto-table;...;fallback=game-world-image`. A loaded `.zvrs` whose payload hash differs
  from the table logs `terrain-auto-warning`.
- Timing: detection runs once, on the consumer thread at XR start (the first published frame, i.e. in a mission), before the terrain is set
  up. A mission change later in the same game session is NOT re-detected (restart the game) [limitation]. The takeoff
  segment is handled without re-detection: the airbase stage (k00a) is loaded next to the detected stage and selected by position
  (takeoff-stage.md); in test 19 the scan ran on the runway and still ranked the mission stage first (m02a 24 vs k00a 4).
- Tests: ctest `stage_fingerprint_offline` (the Python-built synthetic fixture `tests/acz-vr/fixtures/stage_fingerprints_synth.json` + fake
  streams: per-group selection, chunk-boundary windows at every split point, separate streams, family-only tie, family + one unique, stale
  A+B tie, min-hits, repeated copies, 31-byte partial windows, a scan of the masked table itself, parser refusals).

### 3.5 Scan cost bounds (R-3, docs/review/overnight-runtime-review.md)

Measured before the change [E]: every live `auto` run (step65..step77, mission 02) read the whole eligible process memory, 360-369 MB in
588-612 regions, 500-797 ms, `truncated=0`, result m02a 24 vs 7 (39 of 638 signatures: mapset 4, mhd 8, mtc 4, mtd 4, mti 15, sph 4). The logs
do **not** say which regions held the hits (no addresses were logged) [U], so the region priority below is an inference, and the scan now
logs where its hits were found so the next live run can confirm or correct it.

- **Order** (`order_regions`): (0) `ee64`: private read-write regions of a 64 MiB allocation, the shape of the block the E6 locator finds
  (e6-anchor-free.md; the PS2 game loads its files into EE RAM [I]); (1) `prw`: private read-write allocations of 1..256 MiB, largest first;
  (2) `prw-x`: other private read-write; (3) `priv`: other private readable; (4) `map`: mapped views. Address order inside (0), (2)..(4).
- **Decisive early stop:** after any chunk that found a new signature, stop when best >= 16, best - second >= 8 and best >= 2 x second
  (m02a's 24 vs 7 qualifies; close families such as m03k 70 vs r01a 66 or m17a 40 vs m18a 31 never stop early and are read in full).
  A partial count can only grow, and a wrong group gains hits only through signatures it shares with the right one, so the margin rule
  protects against stopping on shared (family) signatures.
- **Byte caps:** 1 GiB read per attempt (2 GiB on attempt 3, which only runs when the first two found no signature at all); once a candidate
  leads (best >= min_hits and best > second) at most 384 MiB more; 20 s wall clock.
- **Resident pages only:** before each chunk `QueryWorkingSetEx` (reads the working-set database, not the pages) says which pages are in
  this process's working set; only those are copied. Paged-out, standby and never-touched (demand-zero) pages are skipped and counted, so
  the scan never pages anything in and never materialises zero pages. A gap breaks the matcher stream (no window spans unread bytes). A
  failed query skips the chunk. Cost: a stage file that the game has let page out is not seen (logged as `nonresident-skipped`).
- **Pacing:** 4 MiB copies; `Sleep(1)` after every 4 MiB read (about 90 yields for the measured 360 MB).
- **Stop:** an orderly stop (game exit drain) ends the scan before the next chunk and the 3 s wait between attempts early (R-11).
- **Log:** a second line per attempt, `xr-stereo;event=terrain-auto-cost;attempt=;stop=complete|decisive|byte-cap|post-candidate-cap|
  time-cap|stop;bytes-read=;nonresident-skipped=;eligible=;regions=<visited>/<eligible>;cap=;candidate-at=;decisive-at=;ms=;yields=;
  ws-query-failures=;skipped-own=;hit-regions=<class>@<rank>:<alloc MiB>M/<region KiB>K:<new signatures>,...` (top 4 regions by hits).
- Tests (offline, `stage_fingerprint_offline`): a fake address space with a fake page-state provider: order/classes, decisive stop inside the
  first region (4 MiB of 272 MiB read), a tie read in full, default thresholds not reached, hard byte cap, post-candidate cap, time cap,
  chunk size and yields, non-resident pages never read (signatures on them and a window across the boundary stay unseen, skipped bytes
  counted), a failed residency query, residency off, orderly stop, `lead()` == `decide()`. `runtime_safety_review` runs the real scan on the
  test process (guard/no-access pages, churn) and checks that `QueryWorkingSetEx` works.
- Not verified live: the order, the decisive stop and the resident-only rule on the game (none of it has run in acz.exe yet).

### 3.4 Install / run (for the driver)

```
python -I research/world-render-data/bake_all_stages.py --out D:\acz-extract\stage-all --table docs/world-render/stage-table.md
python -I research/world-render-data/stage_fingerprints.py                 # -> D:\acz-extract\stage-all\fingerprints.json
pwsh -NoProfile -File tools/live/install-assets.ps1 -StageDir stage-all    # *.zvrs, *.zvro, fingerprints.json -> %LOCALAPPDATA%\acz-vr\stage
tools/live/vr-test-start.ps1 ... -TerrainRender 'auto;env:1;props:1;trees:1'   # or -Mission 02 -TerrainRender '<anything>;...' for a fixed stage
```
Read the `terrain-auto` line in xr-stereo.log: `sources=` shows which raw files are resident in the PC port's memory. If nothing matches at
all, the raw-file hypothesis is wrong for this port and candidate (a) (texture uploads) is next. Not yet run live: nothing here shows that
the port keeps the files in raw form, and fixture success is not game behaviour.
