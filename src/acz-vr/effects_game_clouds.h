#pragma once
// effects_game_clouds.h -- EFFECTS-OUTSIDE (A), docs/world-render/effects-outside-window.md. Header-only, SDK-free.
// Process-wide switch between the XR proxy (writes it once per terrain load from the xr_terrain_stage option gamecloud:0|1, see
// terrain_stage.h drop_game_clouds) and the step-5 effects producer (reads it per effects candidate, gpu_copy_step5.h effects_decide).
// true = the game's own cloud billboards (effects PS 4caf6e2d with depth GREATER_EQUAL) are NOT replayed into the effects image, so they no
// longer draw a second cloud layer inside the game-window rectangle on top of our full-sphere environment clouds. Default false = unchanged.
#include <atomic>

namespace acz::gpu_copy::step5 {
inline std::atomic<bool>& effects_drop_game_clouds_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (C) fxmem:1 (fx_smoke.h; docs section 8.3 steps 1-2): the producer copies each missile-smoke draw's t10 record from the game's mapped
// UPLOAD memory (read-only, bounded) and logs `fx-smoke-frame` lines. Capture + log only: nothing is replayed. Default false = no read, no line.
inline std::atomic<bool>& effects_smoke_capture_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (C) fxmem:2 (docs section 8.6): additionally every captured smoke draw is PREPARED for both eyes on the CPU (fx_smoke.h ReplayStore, from the
// frame's world camera) and its trail points counted per eye: `fx-smoke-replay-plan` lines. A DRY RUN: nothing is recorded on the GPU, the window effects
// image keeps the smoke. Only read while effects_smoke_capture_flag is also set. Default false.
inline std::atomic<bool>& effects_smoke_replay_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (D) fxmem:3 (fx_census.h; docs section 9): the smoke capture of fxmem:1 PLUS a census of every non-member draw the effects seam sees
// (pipeline signature, counts per frame, onsets, gate verdict): fx-census / fx-census-onset / fx-census-window lines. Log only. Default false.
inline std::atomic<bool>& effects_census_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (E) fxmem:5 (docs section 10): fxmem:4 PLUS the explosion PS 542a11d7 (extra7_p) admitted into the window effects image through its own
// coverage clone (needs gpu_copy_step5_effects_dual), and its ps2_v sprites captured (bounded VB/IB read) and planned per eye on the CPU (fx-sprite-plan lines).
inline std::atomic<bool>& effects_explosion_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (C) fxmem:4 (fx_smoke_gpu.h; docs section 8.7): the capture + dry run of fxmem:2 and the prepared eye copies REPLAYED on the GPU per eye (eye
// smoke images depth-tested against the eye objects depth, composited by the consumer over the objects); the smoke is no longer replayed into the window effects
// image. NOT the census of fxmem:3 (effects_census_flag stays false). Read when the step-5 ring is created (the resources exist only if it was set then) and
// latched per frame. Needs effects_smoke_capture_flag + effects_smoke_replay_flag.
inline std::atomic<bool>& effects_smoke_gpu_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h; docs section 11): fxmem:5 PLUS the captured sprite draws (explosion PS 542a11d7, indexed GREATER 4caf6e2d)
// replayed on the GPU per eye from re-projected vertex-buffer copies into per-eye SPRITE images (depth-tested against the eye objects depth, composited by the
// consumer after the smoke) and no longer replayed into the window effects image. Resources at ring creation or late (worker tick), as the smoke. Default false.
inline std::atomic<bool>& effects_sprite_gpu_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (D) xr_terrain_stage fxsig:<8 hex> (docs section 12): the pixel-shader prefix whose draws are logged one line each (fx-sig-draw) for 3 s after an onset.
// 0 = off. Needs effects_smoke_capture_flag (fxmem >= 1) because the effects seam only runs then; independent of the census (fxmem:3) so it works under fxmem:5/6.
inline std::atomic<uint32_t>& effects_sig_filter() noexcept { static std::atomic<uint32_t> f{0}; return f; }
// HUD-RS-SKIP (gpu_copy_step5.h hud_rs_skip): the HUD replay skips the colour REV_SUBTRACT ONE/ONE (alpha ONE/ZERO/ADD) draws (the sun flare). Default TRUE (on without any
// option); xr_terrain_stage hudrs:0 turns it off on the next terrain load / per-flight install, clear_stage_effects restores the default.
inline std::atomic<bool>& hud_rs_skip_flag() noexcept { static std::atomic<bool> f{true}; return f; }
// HUD-LATE (hud_late.h; xr_terrain_stage hudlate:1): the HUD text drawn after the tone-map hand-off (radio subtitles) is replayed into the HUD image.
// Default FALSE (key off: the HUD region ends at the scene barrier exactly as before); clear_stage_effects restores it.
inline std::atomic<bool>& hud_late_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// WORLD-CLOUDS (world_clouds.h; xr_terrain_stage wcloud:1 + wcdrop:<u>, fxmem:6 only): the world edge (units) above which a class-1 GREATER 4caf6e2d quad is a
// cloud puff and is collapsed in the per-eye sprite copies (our world puff field replaces it). 0 = off (default).
inline std::atomic<float>& effects_world_cloud_drop_edge() noexcept { static std::atomic<float> f{0.0f}; return f; }
// WORLD-CLOUDS wcdiag:1 (fxmem:5/6): the read-only fx-cloud-puff census lines. Default false.
inline std::atomic<bool>& effects_cloud_puff_diag_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (M) xr_terrain_stage fxtrail:<s> (+ fxtmax:<u>), fxmem:6 only (fx_puff_trail.h; docs effects-outside-window.md section 19): the life in
// seconds of the world-space smoke-puff memory (the game's class-1 puffs kept and re-emitted per eye after the game camera left them). 0 = off (default).
inline std::atomic<float>& effects_puff_trail_life_s() noexcept { static std::atomic<float> f{0.0f}; return f; }
// (M) the largest world edge of a puff the memory keeps (default 1500: the cloud puffs, 3,800-7,500 units, stay out).
inline std::atomic<float>& effects_puff_trail_max_edge() noexcept { static std::atomic<float> f{1500.0f}; return f; }
// (M3) fxtcls:near|size (0 = near, default; 1 = size) and fxtnear:<u> (default 2500): the smoke classification of fx_puff_trail.h.
inline std::atomic<int>& effects_puff_trail_cls() noexcept { static std::atomic<int> f{0}; return f; }
inline std::atomic<float>& effects_puff_trail_near() noexcept { static std::atomic<float> f{2500.0f}; return f; }
// (M4, docs effects-outside-window.md section 20) fxcloud:<s> (0 = off, default; else 10..600 s): the cloud class of the same memory (the game's own cloud
// puffs kept at their world points); fxcfar:<u> (5000..200000, default 40000): the cache radius (distance fade, drop beyond 1.25 x); fxtxkey:0|1 (default 1):
// a stored card may be drawn inside another key's host draw (same game PSO and descriptor heaps, its key bound within 120 frames).
inline std::atomic<float>& effects_puff_cloud_life_s() noexcept { static std::atomic<float> f{0.0f}; return f; }
inline std::atomic<float>& effects_puff_cloud_far() noexcept { static std::atomic<float> f{40000.0f}; return f; }
inline std::atomic<bool>& effects_puff_trail_xkey() noexcept { static std::atomic<bool> f{true}; return f; }
// (M4c) fxtclip:off|fade|hide (0|1|2, default fade): a game card cut at the window border with no whole stored copy this frame (fx_puff_trail.h ClipMode).
inline std::atomic<int>& effects_puff_clip_mode() noexcept { static std::atomic<int> f{1}; return f; }
// (M6, docs effects-outside-window.md section 23) fxatlas:0|1 (default 0; fxmem:6 with fxtrail / fxcloud): the puff memory's cards are captured into our own
// texture atlas while the game binds their texture, and drawn from it in frames that do not bind it (fx_puff_atlas.h). 0 = the M5 key path exactly.
inline std::atomic<bool>& effects_puff_atlas_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// EFFECTS-OUTSIDE (P, docs effects-outside-window.md section 21) xr_terrain_stage fxadd:1, fxmem:6 only: the ADDITIVE 4caf6e2d family (blend 5,2,1/18,19,1; PSOs
// with additive_ready) is admitted into the effects layer through its private clone (alpha ZERO/ONE), and its one-vertex POINT lists are captured and replayed per
// eye as single pixels (behind-eye points culled). Default false: refused as dual-blend-unsupported exactly as before.
inline std::atomic<bool>& effects_additive_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// (P) ADDITIVE SHOT: the producer asks the consumer for one eye dump (xr_png_once re-arm, like png-eye.now) on the first frames whose per-eye sprite replay
// included an additive draw (at most 2 per session, >= 20 s apart). The consumer takes it with exchange once its previous dump is finished.
inline std::atomic<bool>& effects_additive_eye_request() noexcept { static std::atomic<bool> f{false}; return f; }
} // namespace acz::gpu_copy::step5
