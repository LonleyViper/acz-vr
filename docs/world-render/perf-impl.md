# PERF: startup budget with the forest, and the xr_png_once memory spike (overnight track, D-302)

Offline only: no game, SteamVR or headset run (a driver verifies later). Evidence: `research/experiments/step65-integrated-run2`
(`notes.md`, `launch1-xr-stereo.log`, `launch2-xr-stereo.log`, `sampler.csv`) and `D:\acz-evidence-archive\step65` (L1 logs, png folder),
`D:\acz-evidence-archive\step62\runA/runB-xr-stereo.log`. Game-derived outputs only in `D:\acz-extract\stage-perf`. Tags: **[E]** measured /
tested here, **[I]** inference. Build `D:\b-pf2` (Release, Ninja, -j2).

## 1. Bug 1: "insufficient first-frame completion budget" with trees:1

### 1.1 Where the 4242 ms went [E]
The owner's startup clock starts with the owner thread (`event=owner-thread`). Owner timelines of every archived run (offsets from that line):

| run | trees | owner -> `formats` | `formats` -> `swapchains` (blit `setup()`: root signature + PSOs) | session READY (owner-t-ms) |
|---|---|---|---|---|
| step65 launch 1 (first launch of a new DLL) | 1 (1,157,163 inst.) | **3219** | **875** | **4091** -> refused |
| step65 launch 2 (same DLL, 3 min later) | 0 (same 1,157,163 loaded) | 2219 | 31 | 2255 |
| step62 runA (first launch of that DLL) | 1 (174,402) | **2719** | **625** | 3348 |
| step62 runB (same DLL) | 1 (174,402) | 2203 | 16 | 2220 |
| step56 / step40 / headset-test16 | - | 2203-2219 | 16-31 | 2220-2249 |
| step41 | - | 3172 | 78 | 3257 |

* Nothing of the props/trees runs on the owner thread before frame 1: the `.zvro` is loaded, snapped and gridded on the proxy's monitor thread
  BEFORE `event=launch` (launch 1: `props-loaded` 171 ms after `terrain-loaded`, launch 2 the same), and the props GPU objects were created inside
  frame 1's recording, which launch 1 never reached (the refusal is logged at owner-t 4091, before any GPU work).
* The extra ~1.8 s of launch 1 is the **cold start**: ~1 s more in xrCreateInstance/system/session (`formats`) and ~850 ms in the blit `setup()`,
  whose big ray-march pixel shader is compiled by the driver without a shader-cache entry the first time a new DLL build runs (the same pattern in
  step62 runA vs runB, both trees:1). trees:1 vs trees:0 is not the cause [E for the timeline; I for "driver shader cache"].
* The consumer then refused the frame (`completion_boundary - now < completion_window`), reported `Cause::consumer`, and the cold-start retry
  policy (startup deadline / READY timeout / progress deadline only) said `retry=no`.

### 1.2 CPU cost of the props/trees initialisation [E] (`props_offline_tests` benchmark, soft thresholds)
| phase | synthetic 1.2M | real m02a v1 (1,157,163) | real m02a v2 (baked snap) |
|---|---|---|---|
| parse (incl. sha256 of 28 MB) | 78-87 ms | 77-91 ms | 78 ms |
| of which sha256 | 70-78 ms | 70-72 ms | 72 ms |
| ground snap | 6 ms | 5.3-5.6 ms | **0** |
| grid build | 7-9 ms | 7-8 ms | 8 ms |
| one selection (props+trees, step56 pose) | 2.35 -> **0.02** ms | 0.21 -> **0.07** ms (considered 40,945 -> **2,357**) | 0.07 ms |

So the whole CPU initialisation is ~90-100 ms on the monitor thread (logged now as `props-loaded;...;snap=baked|runtime;load-ms=`), and the
per-frame selection is well under a millisecond. None of it is near the 5000 ms budget.

### 1.3 Changes
1. **Cold-start retry for the first-frame budget refusal** (`xr_stereo_output.{h,cpp}`, `xr_stereo_proxy.cpp`). `Output` records
   `startup_budget_refused()` when it refuses frame 1 for the startup budget with nothing submitted; `retry_allowed(attempt, report, flag)` treats
   exactly that consumer failure as a cold-start timeout. All other conditions are unchanged (attempt 1 only, complete, clean, no loss, nothing
   held, not a success; the flag never widens any other cause). The second attempt runs with warm runtime/shader caches (launch 2 / runB: READY at
   ~2.2 s). Log: `attempt-ended;...;retry=yes-cold-start(first-frame-budget)`.
2. **Props GPU init never inside a frame** (`xr_props_d3d12.{h,cpp}`, `xr_stereo_d3d12.cpp`, `BlitJob::props_async`, set by the consumer). The
   props root signature + PSO and the static set (VB/IB, texture array + mips, SRV heap) are built by ONE worker thread (inside a proxy creation
   scope, device calls only) started by the first job that needs them; until it finishes every job draws the terrain WITHOUT props (byte-identical
   to the props-off picture) and never waits. A new runtime/generation restarts it once the previous worker finished; `release()` and the
   destructor join it. `RecordFacts::props_pending/props_ready/props_failed/props_init_ms`; the consumer logs once
   `event=props-ready;init-ms=;frame=;owner-t-ms=;worker=async` (or `props-init-failed;fallback=terrain-only`).
3. **Per-kind spatial grid** (`world_props.{h,cpp}`): one CSR grid per kind; a frame scans only the cells within that kind's own range + reach, a
   disabled kind is never touched (trees:0 scans the 100 props, not 1.16M trees), locator-only models are not entered. The selection result is
   identical to WORLD-3's (tested against a brute-force copy of the old algorithm on 24 random poses/options/caps); the 16,384 cap is unchanged.
4. **Bake-time ground snap, `.zvro` v2** (`objects_baker.py --stage <the stage's .zvrs>`, `world_props.{h,cpp}`). v2 = v1 + header flag bit0
   (SNAPPED), bytes 68..83 = the first 16 bytes of that `.zvrs` payload sha256 (84..95 zero), every y absolute, instance flags 0. The loader accepts
   v1 and v2; `snap_to_terrain` does no CPU work for v2 and returns false unless the stage's payload sha matches (the proxy then logs
   `props-disabled;reason=snapped-against-another-stage-bake` and keeps the terrain). Refusals: unknown flag bits, stray reserved bytes, a
   ground-relative instance in a snapped file, versions other than 1/2. Without `--stage` the baker writes v1 byte for byte (fixture unchanged).
   The Python snap is a float32 copy of `height_bilinear`: on the real m02a forest bake **1,157,163 / 1,157,163 y are bit-identical** to the C++
   runtime snap [E]. `D:\acz-extract\stage-perf\m02a.zvro` (v2, payload sha `d866aaa4...`) and `stage-perf\v1\m02a.zvro` (v1, `9954728d...` = the
   installed WORLD-4 file) were baked from the archive with `--forest 3`. Installing v2 is optional (v1 keeps working).

Key off / trees:0 output: `props_async` only defers the props pass; with props off or trees:0 the jobs, constants and pictures are unchanged
(harness: key off, out of range and pending jobs are byte-identical to terrain-only).

## 2. Bug 2: commit 94/98.3/99.8 % right after the png-once write

### 2.1 What the png path allocated [E] (`png_dump_offline_tests`, counting operator new, 2400x1350 = 12.96 MB per image)
* Consumer: `png_take` copies the three images out of the 3-image readback buffer (38.9 MB upload-heap commit) into three vectors (38.9 MB).
* Proxy sink (before): per image a detached thread with a COPY of the pixels, each running `png_encode`, which holds the filtered rows, the zlib
  stream and the growing file image: **58.3 MB peak per image (4.5 x)**; three such threads at once: **194 MB peak above the three images (15 x)**.
* Total before: ~270 MB transient (0.7 % of the 40.8 GB commit limit).

### 2.2 Changes
* `gpu_copy::step3::png_stream` (`gpu_copy_step3.h`): the same bytes as `png_encode` (stored deflate, filter 0, one IDAT; tested identical for
  1x1, odd widths, exactly one/two 65535-byte blocks, padded pitch, BGRA, 2400x1350) streamed row by row through a 64 KiB buffer: **75 KB peak**.
* `png_dump::Queue` (`png_dump_queue.h`): the sink TAKES the pixels (swap, no copy; `Settings::png_sink` now passes a mutable vector); ONE worker
  writes the files sequentially in push order and frees each image after its file; image size cap (8192 per side, 64 MB); a failed write deletes
  the partial file. Measured: **75.5 KB above the three images**, max one writer, all three images released afterwards, file CRCs unchanged.
* The readback buffer stays the single one created for the capture and is released by `png_take`.
* Peak now: readback 38.9 MB + the three images 38.9 MB during `png_take`, then <= 38.9 MB + 75 KB while writing.

### 2.3 What this does NOT explain [E/I]
The sampler shows +9.3 % of a 40.8 GB commit limit (~3.8 GB) within ~2 s after the png event (90.5 -> 99.8 %), and launch 2 kept logging for
>= 3 s after `png-once` (xr summary at +3.2 s, producer status at +2.4 s). The old png path's ~270 MB cannot account for 3.8 GB: the png path is
reduced as asked, but the main source of that spike is **unidentified** (candidates not examined offline: the game/driver, SteamVR null,
the evidence logger; the files themselves were complete). A live run with the sampler plus a per-process private-bytes sample of acz.exe and
vrserver is needed to attribute it.

## 3. Tests (all offline)
* `props_offline_tests`: PERF benchmark (synthetic 1.2M + real v1/v2 when present; soft limits printed, never failing), per-kind grid ==
  brute-force reference (24 poses), v2 round trip / stage binding / refusals / v1 compatibility, Python-vs-C++ snap on the real bake (hard
  |dy| <= 0.01, measured 0).
* `xr_stereo_output_offline_tests`: retry policy with the budget flag (allowed once; never with loss, unclean retirement, held binding, attempt 2,
  or for any other cause); the reserve-capped first frame now reports the flag.
* `xr_composite_warp_harness` case 6: async props: the first job is pending and byte-identical to terrain-only, the worker ran inside the proxy
  creation scope, props appear after ready and equal the synchronous picture; a 1.2M-instance forest: first job pending (record time printed, hard
  < 1000 ms, soft 50 ms), trees drawn after ready within the 16,384 cap.
* `png_dump_offline_tests`: identity, peaks (hard: png_stream < 256 KiB, queue < 1 MiB above the images), sequential writer, refusals.
* `objects_baker.py --selftest`: snapped v2 (float32 reference per tree), stage mismatch refused, reserved bytes refused, v1 unchanged.

Not verified: any live behaviour (first-frame time with trees in the game, the retry on a real cold start, the props-ready time on the GPU,
the commit curve after png-once).
