# GPU cost of the world renderer (GPU-COST)

Status: measured offscreen on the real GPU (2026-10-27). Not measured inside a live game frame with a
headset. The benchmark's numbers are the composite's own cost on an otherwise idle GPU.

## 1. What was measured

- **Tool:** `tests/acz-vr/xr_gpu_bench.cpp` (target `xr_gpu_bench`). It is opt-in. It is
  **not** an `add_test`, because it needs a real GPU and the extracted game data.
- **What it runs:** the same `xr_stereo_d3d12.cpp` backend and the same embedded
  `xr_stereo_blit.hlsl`, `xr_props.hlsl` and `cockpit_model.hlsl` bytecode that the DLL uses.
- **Adapter:** the first hardware adapter by DXGI high-performance preference. `--warp`
  selects WARP instead, for a smoke check only.
- **Data:** loaded with the production loaders.
  - `D:\acz-extract\stage-all\m02a.zvrs`. This is byte-identical to the installed stage.
    It goes through `load_stage_file`, `apply_sea_convention`, `build_atlas` and
    `build_env_texture`, with look on and off.
  - `D:\acz-extract\stage-perf\m02a.zvro`. This is the installed v2 snapped bake with
    1,157,163 instances. It goes through `load_props_file`, `snap_to_terrain` and
    `build_grid`.
  - `D:\acz-extract\models-c4\j35jk_vrcockpit.zvrm`. It goes through `load_model_file` and
    `build_mesh` with the default options: interior, canopy glass, 5,793 triangles.
- **Per-pass timing:** `BlitJob::pass_timing` (new) writes D3D12 timestamps at these points:
  - after the props raster pass (both eyes);
  - for each eye, after the model depth pre-pass (zpre only), after the eye composite
    (terrain march + sky, clouds, water and lighting + props merge), after the model's opaque
    and blend passes, and after the canopy glass.

  `BlitBackend::pass_timing()` reads the timestamps once the recording has completed.
- **Statistics:** the median of 40 frames after 8 warm-up frames. The variants of one scene
  are interleaved frame by frame, so a GPU clock change affects all of them alike.
- **Setup:**
  - Per-eye target: 2400x1350 RGBA8 sRGB.
  - FOV: 100 deg horizontal per eye, with square pixels (67.7 deg vertical). IPD 64 mm.
  - The head is at the seat origin, looking forward.
  - The replayed cockpit is fully dropped (model cockpit) or absent (depth 0), so every eye
    pixel runs the world composite.
- **Location:** above the busiest tree area of m02a: cell (-116736, -116736), with 3,998
  trees within ±2 cells. The heading is toward the stage centre. The bake has no
  building props in the grid, so `props` without `trees` draws nothing here.
- **Poses (AGL = above the ground at the start point):**
  - `level-2500`: 2500 u AGL, pitch 0.
  - `low-300-forest`: 300 u AGL, pitch -8 deg.
  - `nose-down-30`: 2500 u AGL, pitch -30 deg.
  - `horizon-300`: 300 u AGL, pitch +1 deg. The horizon is mid-image and grazing rays run long.
- **Raw output:** `D:\acz-extract\gpu-bench\`
  - `baseline.csv` and `compare.csv`/`compare.txt`;
  - one PNG per pose and variant (full size and `_small` previews), for the full
    configuration.

**GPU:** NVIDIA GeForce RTX 4090 (24 GB). This is the user's machine. The headset runs from the
same GPU.

## 2. Before: per-pass cost (q:0, zpre:0 = the defaults), ms

Full live configuration: env:1, look:1, props:1, trees:1 and the model cockpit.

| pose | props pass (both eyes) | composite eye 0 / eye 1 | model opaque+blend (per eye) | canopy glass (per eye) | **frame (both eyes)** |
|---|---|---|---|---|---|
| level-2500 | 0.084 | 0.309 / 0.302 | 0.011 | 0.013 | **0.748** |
| low-300-forest | 0.103 | 0.307 / 0.302 | 0.010 | 0.012 | **0.759** |
| nose-down-30 | 0.092 | 0.344 / 0.339 | 0.010 | 0.012 | **0.822** |
| horizon-300 | 0.094 | 0.299 / 0.294 | 0.010 | 0.012 | **0.735** |

Breakdown of the eye composite. These are level-2500 eye 0 values, taken from the feature
variants with the cockpit on.

| features | composite ms |
|---|---|
| no env, props + trees | 0.205 |
| env (no look), props + trees | 0.274 |
| env + look, props + trees | 0.309 |
| env + look, no props/trees | 0.267 |

- Of the composite cost, the environment (sky, clouds, cloud shadows, water and light) is about
  a third (+0.07 ms).
- look:1 adds +0.035 ms.
- The props merge in the composite adds +0.04 ms, on top of the 0.08-0.10 ms raster pass.
- The rest is the terrain march and its texturing (about 0.19-0.26 ms per eye).
- Without the model cockpit the composite costs the same (0.27-0.35 ms).

**Dominant cost:** the per-eye composite (terrain march + environment) is **80-83% of the
frame**. The props raster is about 12%, and the model cockpit with its glass is about 3%.

In the default order the composite also shades every pixel that the opaque cockpit then
overwrites. In the J35 forward view that is about half the eye.

## 3. Optimisations

### zpre: model first, depth-tested composite

**Option:** `zpre:1` inside `xr_cockpit_model` (default 0). The bench sets
`BlitJob::cockpit_prepass` directly.

The order per eye with zpre:
1. Clear the model depth.
2. Draw the model's **opaque** pass (colour + depth).
3. Draw the eye composite (`ps_main`) with a second PSO that has a read-only depth test.
   The test is GREATER_EQUAL and the fullscreen triangle sits at z = 0, so only pixels the
   model left at the cleared 0 pass.
4. Draw the model's blend and canopy-glass passes over the result. They use the same depth,
   so they test exactly as before.

Why this is cheap and why the image does not change:
- `ps_main` writes no depth and never discards, so early-Z rejects the covered pixels before
  the ray march runs.
- The opaque pass outputs alpha = 1 with ONE / INV_SRC_ALPHA, so it fully overwrote the
  composite in the old order. The final image is therefore **byte-identical**.

Other cases:
- Debug views and the png copy keep the old order.
- If the PSO cannot be created, the old order is used.

### q:1 / q:2: march quality

**Option:** `q:1` or `q:2` inside `xr_terrain_stage` (default 0). This sets `t_bits` bits
9..10 for the shader.

- **q:1:**
  - Beyond 20,000 u the minimum step grows twice as fast; the near field is unchanged.
  - Bisection drops from 8 to 6 steps.
  - A descending ray that marches past the haze-saturation distance stops there and takes the
    haze colour. That distance is `L·ln 512` (haze ≥ 1-1/512), which is 134 km with look:1;
    with look:0 (L 55 km) it lies beyond the march end.
- **q:2:**
  - The far-field step grows three times as fast, beyond 10,000 u.
  - The step fraction is 0.5 instead of 0.4.
  - Bisection uses 5 steps.
- The CPU reference stays the q0 march.

### Candidates not implemented

- **Hierarchical max-height mip march.** Rays that are high above the terrain already take
  steps of 0.4·dy. Grazing far-field rays sit below the block maxima. q1 doubles the
  far-field step and saves only 2-6%, so a mip march would gain at most a fraction of the
  march for a new texture and traversal.
- **Half-resolution far field** and **cheaper cloud and sky terms.** The environment is about
  0.07 ms per eye after zpre roughly halves it. The effort and the visual risk are not
  justified at the current budget.
- **Props LOD.** About 3,000 instances are selected per frame. The raster pass costs about
  0.1 ms for both eyes.

## 4. After: same scenes (median ms; image error vs q:0 on eye 0, 8-bit sRGB)

Full live configuration. The columns are the same as in section 2.

| pose | variant | props | composite e0 / e1 | model+glass per eye | **frame** | vs q0 |
|---|---|---|---|---|---|---|
| level-2500 | q0 (before) | 0.084 | 0.309 / 0.302 | 0.024 | **0.748** | reference |
| level-2500 | **zpre** | 0.084 | 0.180 / 0.168 | 0.016 (+0.012 pre) | **0.486** | **identical (0/255)** |
| level-2500 | q1+zpre | 0.084 | 0.163 / 0.158 | 0.016 | **0.460** | max 19/255, 0.032% px > 2/255 |
| level-2500 | q2+zpre | 0.084 | 0.145 / 0.142 | 0.016 | **0.428** | max 39/255, 0.17% px |
| low-300-forest | q0 | 0.103 | 0.307 / 0.302 | 0.022 | **0.759** | reference |
| low-300-forest | **zpre** | 0.103 | 0.162 / 0.160 | 0.015 | **0.475** | **identical** |
| low-300-forest | q1+zpre | 0.103 | 0.153 / 0.152 | 0.015 | **0.457** | max 38/255, 0.011% |
| low-300-forest | q2+zpre | 0.103 | 0.141 / 0.140 | 0.015 | **0.435** | max 54/255, 0.074% |
| nose-down-30 | q0 | 0.092 | 0.344 / 0.339 | 0.022 | **0.822** | reference |
| nose-down-30 | **zpre** | 0.092 | 0.175 / 0.175 | 0.015 | **0.494** | **identical** |
| nose-down-30 | q1+zpre | 0.092 | 0.161 / 0.161 | 0.015 | **0.464** | max 23/255, 0.058% |
| nose-down-30 | q2+zpre | 0.092 | 0.144 / 0.144 | 0.015 | **0.431** | max 33/255, 0.25% |
| horizon-300 | q0 | 0.094 | 0.299 / 0.294 | 0.022 | **0.735** | reference |
| horizon-300 | **zpre** | 0.094 | 0.165 / 0.164 | 0.015 | **0.475** | **identical** |
| horizon-300 | q1+zpre | 0.094 | 0.157 / 0.158 | 0.015 | **0.461** | max 76/255, 0.013% |
| horizon-300 | q2+zpre | 0.094 | 0.146 / 0.147 | 0.015 | **0.438** | max 77/255, 0.085% |

- **zpre:** -35 to -42% of the frame, with byte-identical output on every pose and feature
  combination of the benchmark (28 scenes).
- **q1** on top of zpre: -3 to -6%. **q2** on top of zpre: -8 to -13%.
- The q differences are silhouette pixels where a coarser far-field step lands on another
  ridge, plus near-saturated haze. Without env, q2 reaches 1.1-3.9% of pixels over 2/255
  (`compare.csv`).

**Tolerance (documented budget):**
- q1 ≤ 0.5% of the pixels over 2/255 at 2400x1350. The worst measured case is 0.49% (noenv).
- WARP harness, against the CPU `trace_env`: ≤ 1% of the pixels over 0.05 linear for q1 and
  ≤ 3% for q2. The measured values are ≤ 0.01%.

## 5. Frame budget at 90 Hz (11.1 ms)

| item | ms (RTX 4090) | source |
|---|---|---|
| world composite + props + model cockpit, both eyes, default | 0.74-0.82 | this benchmark (isolated) |
| same with zpre:1 | 0.48-0.49 | this benchmark |
| same with zpre:1 q:2 | 0.43-0.44 | this benchmark |
| the whole consumer recording in the live game (null HMD, zpre off) | 1.8-2.1 | step71 `summary-timing gpu-blit-ms` (shared queue, game load) |
| the game's own frame + the producer's replays | not measured | — |
| SteamVR compositor | not measured | — |

- The world renderer uses about **4.3% of the 11.1 ms frame with zpre** (6.7-7.4% without).
- It is not the item that decides 90 Hz on this GPU. The game and the producer passes
  (unmeasured) and the compositor are.
- The live wall time is about 2.5x the isolated cost. That run also included the HUD and
  marker targets, the replay sources, and queue contention with the game.
- **Scaling:** the cost is roughly proportional to the eye pixel count. At 2880x1700 per eye
  (+51% pixels), expect about 0.73 ms with zpre.
- A GPU with a quarter of the 4090's throughput would need about 2 ms with zpre: still
  inside the budget.

## 6. Recommendation and how to reproduce

- **Next integrated or headset run:** set `xr_cockpit_model=<id>;...;zpre:1`. The image is
  identical, but the change is not yet verified live: the GPU order on the game's queue.
- Keep `q:0`. Use `q:1` only if a lower-end GPU is targeted.

Commands:
- Bench: `D:\b-gpu\xr_gpu_bench.exe --compare --png --frames 40 --csv D:\acz-extract\gpu-bench\compare.csv`
  - `--quick` runs the full configuration only.
  - Check `tasklist` for `acz.exe` / `vrserver.exe` first.
- Harness: `xr_composite_warp_harness` case `[g] GPU-COST` (in ctest `xr_composite_warp`). It
  covers:
  - zpre byte identity (terrain + env and world modes, three head poses, png and debug paths);
  - the q1/q2 budgets against `trace_env`;
  - monotone pass marks, and an unchanged image with `pass_timing`;
  - option parsing.
