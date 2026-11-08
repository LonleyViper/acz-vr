#pragma once
// effects_stage_flags.h -- the stage-bound EFFECTS-OUTSIDE producer switches (effects_game_clouds.h) for one terrain stage, in ONE place for both stage
// paths: the monitor's one-time load (xr_stereo_proxy.cpp) and the VSEARLY per-flight install (xr_flight_stage.cpp). Live batch Y (Y2): the per-flight
// copy had drifted (fxmem:5 never set the GPU smoke / explosion switches and no fx-memory line was logged). Header-only, SDK-free.
#include "effects_game_clouds.h"
#include "hud_late.h"

#include <cstddef>
#include <cstdio>

namespace acz::gpu_copy::step5 {

struct StageEffects {
    bool drop_game_clouds = false, smoke_capture = false, smoke_replay = false, smoke_gpu = false, explosion = false, census = false, sprite_gpu = false;
    uint32_t fx_sig = 0;
    bool hud_rs_skip = true;   // HUD-RS-SKIP (hudrs:0 = false); default on
    float wc_drop_edge = 0.0f;  // WORLD-CLOUDS (world_clouds.h producer_drop_edge): 0 = the game's puff quads stay in the per-eye replay
    bool wc_diag = false;       // WORLD-CLOUDS wcdiag:1 (fxmem:5/6): fx-cloud-puff census lines
    float puff_trail_s = 0.0f;  // EFFECTS-OUTSIDE (M) fxtrail:<s> (fxmem:6 only; fx_puff_trail.h): 0 = off
    float puff_trail_edge = 1500.0f;   // (M) fxtmax:<u>
    int puff_trail_cls = 0;            // (M3) fxtcls: 0 near (default), 1 size
    float puff_trail_near = 2500.0f;   // (M3) fxtnear:<u>
    float puff_cloud_s = 0.0f;         // (M4) fxcloud:<s> (fxmem:6 only): 0 = off
    float puff_cloud_far = 40000.0f;   // (M4) fxcfar:<u>
    bool puff_xkey = true;             // (M4) fxtxkey:0|1
    int puff_clip = 1;                 // (M4c) fxtclip: 0 off, 1 fade, 2 hide
    bool puff_atlas = false;           // (M6) fxatlas:0|1 (fxmem:6 only; fx_puff_atlas.h)
    bool hud_late = false;             // HUD-LATE hudlate:1 (hud_late.h): post-tone-map HUD text (radio subtitles) into the HUD image; default off
    bool additive = false;             // EFFECTS-OUTSIDE (P) fxadd:1 (fxmem:6 only): the additive 4caf6e2d family + its point lists per eye; default off
};
// (M4) the cloud class / cross-key switches of the puff memory (one argument: stage_effects keeps its positional tail).
struct PuffCloud { float life_s = 0.0f, far_units = 40000.0f; bool xkey = true; int clip = 1; bool atlas = false; };   // (M6) atlas = fxatlas:1
// fx_memory 0..6 (xr_terrain_stage fxmem:), drop = acz::terrain::drop_game_clouds(conv, our clouds active), hud_rs = hudrs: (default 1),
// hud_late = hudlate: (default 0; independent of fxmem).
inline StageEffects stage_effects(int fx_memory, bool drop, uint32_t fx_sig = 0, bool hud_rs = true, float wc_drop_edge = 0.0f, bool wc_diag = false,
                                  float puff_trail_s = 0.0f, float puff_trail_edge = 1500.0f, int puff_trail_cls = 0, float puff_trail_near = 2500.0f,
                                  PuffCloud cloud = PuffCloud{}, bool hud_late = false, bool additive = false) noexcept {
    StageEffects e;
    e.hud_late = hud_late;
    e.additive = fx_memory == 6 && additive;   // (P) needs the per-eye sprite replay (fxmem:6)
    e.puff_cloud_s = fx_memory == 6 && cloud.life_s > 0.0f ? cloud.life_s : 0.0f;   // (M4) needs the per-eye sprite replay (fxmem:6)
    e.puff_cloud_far = cloud.far_units > 0.0f ? cloud.far_units : 40000.0f;
    e.puff_xkey = cloud.xkey;
    e.puff_clip = cloud.clip == 0 || cloud.clip == 2 ? cloud.clip : 1;
    e.puff_atlas = fx_memory == 6 && cloud.atlas;   // (M6) needs the per-eye sprite replay (fxmem:6)
    e.puff_trail_cls = puff_trail_cls == 1 ? 1 : 0;
    e.puff_trail_near = puff_trail_near > 0.0f ? puff_trail_near : 2500.0f;
    e.puff_trail_s = fx_memory == 6 && puff_trail_s > 0.0f ? puff_trail_s : 0.0f;   // (M) needs the per-eye sprite replay (fxmem:6)
    e.puff_trail_edge = puff_trail_edge > 0.0f ? puff_trail_edge : 1500.0f;
    e.wc_drop_edge = fx_memory == 6 && wc_drop_edge > 0.0f ? wc_drop_edge : 0.0f;
    e.wc_diag = (fx_memory == 5 || fx_memory == 6) && wc_diag;
    e.hud_rs_skip = hud_rs;
    e.fx_sig = fx_memory >= 1 ? fx_sig : 0;
    e.drop_game_clouds = drop;
    e.smoke_gpu = fx_memory == 4 || fx_memory == 5 || fx_memory == 6;     // fxmem:4/5/6 (fx_smoke_gpu.h)
    e.smoke_replay = fx_memory == 2 || fx_memory == 4 || fx_memory == 5 || fx_memory == 6;
    e.explosion = fx_memory == 5 || fx_memory == 6;                        // EFFECTS-OUTSIDE (E)
    e.sprite_gpu = fx_memory == 6;                                         // EFFECTS-OUTSIDE (F) fx_sprite_gpu.h
    e.smoke_capture = fx_memory >= 1;
    e.census = fx_memory == 3;                                             // EFFECTS-OUTSIDE (D)
    return e;
}
inline void apply_stage_effects(const StageEffects& e) noexcept {
    effects_drop_game_clouds_flag().store(e.drop_game_clouds, std::memory_order_release);
    effects_smoke_gpu_flag().store(e.smoke_gpu, std::memory_order_release);
    effects_smoke_replay_flag().store(e.smoke_replay, std::memory_order_release);
    effects_explosion_flag().store(e.explosion, std::memory_order_release);
    effects_smoke_capture_flag().store(e.smoke_capture, std::memory_order_release);
    effects_census_flag().store(e.census, std::memory_order_release);
    effects_sprite_gpu_flag().store(e.sprite_gpu, std::memory_order_release);
    effects_sig_filter().store(e.fx_sig, std::memory_order_release);
    hud_rs_skip_flag().store(e.hud_rs_skip, std::memory_order_release);
    hud_late_flag().store(e.hud_late, std::memory_order_release);
    effects_world_cloud_drop_edge().store(e.wc_drop_edge, std::memory_order_release);
    effects_cloud_puff_diag_flag().store(e.wc_diag, std::memory_order_release);
    effects_puff_trail_max_edge().store(e.puff_trail_edge, std::memory_order_release);
    effects_puff_trail_cls().store(e.puff_trail_cls, std::memory_order_release);
    effects_puff_trail_near().store(e.puff_trail_near, std::memory_order_release);
    effects_puff_cloud_far().store(e.puff_cloud_far, std::memory_order_release);
    effects_puff_trail_xkey().store(e.puff_xkey, std::memory_order_release);
    effects_puff_clip_mode().store(e.puff_clip, std::memory_order_release);
    effects_puff_atlas_flag().store(e.puff_atlas, std::memory_order_release);
    effects_puff_cloud_life_s().store(e.puff_cloud_s, std::memory_order_release);
    effects_puff_trail_life_s().store(e.puff_trail_s, std::memory_order_release);
    effects_additive_flag().store(e.additive, std::memory_order_release);
}
// EFFECTS-OUTSIDE (P) the option line for fxadd:1 (false = not requested: no line). active=0 names why (fxmem:6 is required).
inline bool fx_additive_line(char* b, size_t n, int fx_memory, bool requested) noexcept {
    if (!b || !n || !requested) return false;
    std::snprintf(b, n, "xr-stereo;event=fx-additive-option;requested=1;active=%d;reason=%s;tuple=5,2,1/18,19,1;clone-alpha=1,2;points=%s;"
        "log=step5-producer.log(continuous-hud-effects-additive,fx-sprite-totals;pts=);doc=docs/world-render/effects-outside-window.md#21",
        fx_memory == 6 ? 1 : 0, fx_memory == 6 ? "additive-4caf6e2d-family-admitted-with-clone+point-lists-per-eye" : "needs-fxmem-6",
        fx_memory == 6 ? "per-eye(one-pixel,behind-eye-culled)" : "off");
    return true;
}
// EFFECTS-OUTSIDE (M) the option line for fxtrail:<s> / (M4) fxcloud:<s> (false = neither requested: no line). active=0 names why (fxmem:6 is required).
inline bool fx_puff_trail_line(char* b, size_t n, int fx_memory, float life_s, float max_edge, int cls = 0, float near_units = 2500.0f,
                               PuffCloud cloud = PuffCloud{}) noexcept {
    if (!b || !n || !(life_s > 0.0f || cloud.life_s > 0.0f)) return false;
    std::snprintf(b, n, "xr-stereo;event=fx-puff-trail-option;life-s=%.1f;max-edge=%.0f;cls=%s;near=%.0f;active=%d;reason=%s;cloud-s=%.0f;cfar=%.0f;xkey=%d;clip=%s;"
        "atlas=%d;log=step5-producer.log(fx-puff-trail,fx-puff-mem,fx-puff-atlas);doc=docs/world-render/effects-outside-window.md#20", double(life_s), double(max_edge),
        cls == 1 ? "size" : "near", double(near_units), fx_memory == 6 ? 1 : 0,
        fx_memory == 6 ? "world-space-puff-memory-re-emitted-per-eye" : "needs-fxmem-6", double(cloud.life_s > 0.0f ? cloud.life_s : 0.0f),
        double(cloud.far_units), cloud.xkey ? 1 : 0, cloud.clip == 0 ? "off" : cloud.clip == 2 ? "hide" : "fade", fx_memory == 6 && cloud.atlas ? 1 : 0);
    return true;
}
inline void clear_stage_effects() noexcept { apply_stage_effects(StageEffects{}); }
// W15 w15-3 F2: the game's puff quads are collapsed in the producer's eye copies ONLY while our replacement cloud field can be drawn. The replacement needs the GPU
// terrain resources; when the consumer marks them dead for the stage generation (e.g. the environment texture allocation fails) it calls this: the producer stops
// collapsing puffs from the next captured draw on (a re-install of the stage re-applies the edge through apply_stage_effects). Returns whether suppression was on.
// Frames already prepared with collapsed copies keep them until they retire (the producer cannot restore a vertex copy it has already replaced).
inline bool withdraw_world_cloud_drop() noexcept { return effects_world_cloud_drop_edge().exchange(0.0f, std::memory_order_acq_rel) > 0.0f; }
// The `fx-memory` line for fxmem:1..6 (nullptr for 0).
inline const char* fx_memory_line(int fx_memory) noexcept {
    switch (fx_memory) {
    case 1: return "xr-stereo;event=fx-memory;requested=1;active=capture-log-only;reason=smoke-record-capture+fx-smoke-frame-lines(no-replay,"
                   "sprite-memory-not-built);needs=gpu_copy_step5_effects+hud+continuous;log=step5-producer.log;doc=docs/world-render/effects-outside-window.md";
    case 2: return "xr-stereo;event=fx-memory;requested=2;active=capture-log+replay-dry-run;reason=smoke-records-prepared-per-eye-on-cpu+"
                   "fx-smoke-replay-plan-lines(no-gpu-replay,window-effects-unchanged);needs=gpu_copy_step5_effects+hud+continuous+"
                   "world_objects;log=step5-producer.log;doc=docs/world-render/effects-outside-window.md";
    case 3: return "xr-stereo;event=fx-memory;requested=3;active=capture-log+draw-census;reason=smoke-record-capture+fx-census-lines(explosion/"
                   "tracer-identification,log-only,no-replay);needs=gpu_copy_step5_effects+hud+continuous;log=step5-producer.log;"
                   "doc=docs/world-render/effects-outside-window.md#9";
    case 4: return "xr-stereo;event=fx-memory;requested=4;active=capture+replay-dry-run+replay-per-eye-gpu(no-census);reason=eye-smoke-images-depth-tested-against-eye-objects+"
                   "composited-over-objects(window-effects-without-smoke);resources=at-step5-ring-creation(fx-smoke-ring-line);needs=gpu_copy_step5_effects+"
                   "effects_dual+hud+continuous+world_objects;log=step5-producer.log;doc=docs/world-render/effects-outside-window.md";
    case 5: return "xr-stereo;event=fx-memory;requested=5;active=4+explosion-window-effects+explosion-sprite-plan;reason=ps542a11d7(extra7_p)-admitted-with-"
                   "coverage-clone(5,5,1/18,19,1->alpha-zero/one)+fx-sprite-plan-lines(cpu-dry-run,no-eye-replay-yet);needs=gpu_copy_step5_effects+"
                   "effects_dual+hud+continuous+world_objects;log=step5-producer.log;doc=docs/world-render/effects-outside-window.md#10";
    case 6: return "xr-stereo;event=fx-memory;requested=6;active=5+sprite-replay-per-eye-gpu;reason=explosion(ps542a11d7)+greater-4caf6e2d-sprites-re-projected-"
                   "per-eye(vb-copy)+eye-sprite-images-depth-tested-against-eye-objects(window-effects-without-them);resources=fx-sprite-ring-line;needs="
                   "gpu_copy_step5_effects+effects_dual+hud+continuous+world_objects;log=step5-producer.log;doc=docs/world-render/effects-outside-window.md#11";
    default: return nullptr;
    }
}

} // namespace acz::gpu_copy::step5
