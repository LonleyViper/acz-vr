#pragma once
// world_scale.h -- WORLD-SCALE (headset test 19: "close things look smaller than they should"). Header-only, SDK-free.
// Derivation and numbers: docs/world-render/world-scale.md.
//
// The game's WORLD units (E6 camera/aircraft positions, stage heightfield, DrawACM_v world-object VP, stage props, aircraft models: 1 model
// unit == 1 world unit) are ~5.0 units per metre (Draken 76.7 u / 15.35 m = 5.00; MiG-21 5.00; F-16C 5.26 incl. probe; HUD ALT 2563 ft at
// y 4000 = 5.12; |E6 cam - air| = 36.48 u == the j35j.pvb eye). Before this change every world layer moved the eye by the producer's
// gpu_copy_step5_units_per_metre (65 in the day config, chosen for the REPLAYED cockpit's camera space), i.e. 13x the head translation and
// 13x the IPD: a 0.9 m stereo baseline that makes the world look 1/13 size (hyper-stereo miniature).
//
// One world scale now: kWorldUnitsPerMetre (or xr_terrain_stage upm:) times wscale:<f> (xr_terrain_stage option, default 1). The XR proxy
// publishes the result here when the terrain option parses; the step-5 producer uses it for the world-object / smoke eye offsets (the
// replayed cockpit and its IPD row-3 fallback keep gpu_copy_step5_units_per_metre). 0 = not published = the previous behaviour.
#include <atomic>

namespace acz::world_scale {
inline constexpr float kWorldUnitsPerMetre = 5.0f;          // evidenced (docs/world-render/world-scale.md section 2)
inline constexpr float kWScaleMin = 0.25f, kWScaleMax = 4.0f; // wscale:<f> range; >1 = larger stereo baseline = world looks SMALLER
// The supported range of the FINAL scale (upm x wscale), shared by the terrain parser (rejects a product outside it) and the producer (setup_poses accepts
// the same range), so both consumers always apply the same translation scale. 0 stays the "unpublished" sentinel (outside the range on purpose).
inline constexpr float kFinalScaleMin = 1.0f, kFinalScaleMax = 100.0f;
inline bool final_scale_supported(float v) noexcept { return v >= kFinalScaleMin && v <= kFinalScaleMax; }
// World head-translation scale (game units per metre of head/eye offset) for the world layers.
// upmOption > 0: the explicit xr_terrain_stage upm: value; otherwise the evidenced kWorldUnitsPerMetre. Times wscale.
inline float world_head_units_per_metre(float upmOption, float wscale) noexcept {
    const float base = upmOption > 0.0f ? upmOption : kWorldUnitsPerMetre;
    return base * wscale;
}
// Process-wide value from the XR proxy (written when xr_terrain_stage parses) to the step-5 world-object path. 0 = unset.
inline std::atomic<float>& published_world_units_per_metre() noexcept { static std::atomic<float> v{0.0f}; return v; }
// The producer's world eye scale: the published world value when set, else `fallback` (gpu_copy_step5_units_per_metre: unchanged behaviour).
inline double producer_world_units_per_metre(double fallback) noexcept {
    const float v = published_world_units_per_metre().load(std::memory_order_relaxed);
    return final_scale_supported(v) ? double(v) : fallback;
}
} // namespace acz::world_scale
