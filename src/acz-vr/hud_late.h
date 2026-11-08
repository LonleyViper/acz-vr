#pragma once
// hud_late.h -- HUD-LATE (xr_terrain_stage hudlate:1, default off; docs/world-render/markers-360.md section 18). Header-only, SDK-free.
//
// The radio-chatter subtitles ("Pixy << Hey, you all right Buddy? >>") are drawn on the scene target AFTER the tone-map pass: the scene leaves
// RENDER_TARGET for the exposure passes, comes back, and receives 38-103 unindexed 4-vertex quads + a few indexed draws of the HUD text pipeline
// (VS 534f29ca / PS d34ee153, blend SRC_ALPHA / INV_SRC_ALPHA, depth ALWAYS, stencil ALWAYS/KEEP; J-radar-analysis.md 1.3 [E]), then the output pass.
// With the installed config (gpu_copy_step5_hud_radar) the HUD region IS resumed there (RADAR-SCENE-RESUME), but every draw after an excursion that is
// not in a radar stencil window and is neither a REPLACE writer nor an EQUAL test is skipped as "late" (gpu_copy_step5.h hud_radar_decide,
// radar_skip_late). Evidence (step146 runAGB2 step5-producer.log, hud-end lines): quiet frames have 0 late-skipped draws; subtitle frames have 30-87
// (frame 312: `skipped=late:87`, stencil ALWAYS, hud_decide `accept:89`, disposition `l` for the whole tail; scene-leaves 4 / resumes 3 vs 3 / 2 quiet).
//
// HUD-LATE: a draw that hud_radar_decide would skip as late is replayed into the HUD INSTRUMENT image when it is of the HUD text class below and the
// plain hud_decide accepts it. It never opens a radar window, is never classified as a marker (no box / label / lock test), and a cap hit or a refusal
// only skips it (counted; the frame's HUD stays valid). Key off or without the radar key: nothing changes.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace acz::gpu_copy::step5 {

// The HUD text pipeline (J-radar-analysis.md 1.3).
constexpr uint32_t kHudLateVs = 0x534f29cau, kHudLatePs = 0xd34ee153u;
constexpr uint32_t kBlendSrcAlpha = 5u, kBlendInvSrcAlpha = 6u;      // D3D12_BLEND
constexpr uint32_t kCmpAlways = 8u;                                  // D3D12_COMPARISON_FUNC_ALWAYS

// One draw's identity, as the Draw seam knows it (vs8 / ps8 = the first 8 hex digits of the creation shader hashes; 0 = unknown).
struct HudLateDraw {
    uint32_t vs8 = 0, ps8 = 0;
    bool blend = false; uint32_t src = 0, dst = 0;
    bool depth = false, depth_write = false; uint32_t depth_func = 0;
    bool stencil = false; uint32_t front_func = 0, back_func = 0; bool stencil_ops_keep = true;
};
// nullptr = a HUD text-class draw, else why not. The gun tracer shares the shaders but blends SRC_ALPHA / ONE (dst 2); the sun flare is REV_SUBTRACT
// ONE / ONE; the firing wedge is depth GREATER; the radar mask / contact draws write or test the stencil: none of them is text.
inline const char* hud_late_text(const HudLateDraw& d) noexcept {
    if (!d.vs8 || !d.ps8) return "shader-unknown";
    if (d.vs8 != kHudLateVs || d.ps8 != kHudLatePs) return "not-text-shader";
    if (!d.blend || d.src != kBlendSrcAlpha || d.dst != kBlendInvSrcAlpha) return "not-text-blend";
    if (d.depth_write || (d.depth && d.depth_func != kCmpAlways)) return "depth-tested";
    if (d.stencil && (d.front_func != kCmpAlways || d.back_func != kCmpAlways || !d.stencil_ops_keep)) return "stencil";
    return nullptr;
}
// DISPLAY-VALUE MARKER (review F6). The late text is drawn after the game's tone map, so its values are display values, but the HUD composite
// (xr_stereo_blit.hlsl mode 1) applies exposure and the tone map to every HUD pixel: Reinhard halves white text, exposure != 1 rescales it. The HUD
// image has no per-pixel flag, so a late text draw is replayed under a private clone of its PSO whose ALPHA blend is ONE / ONE / REV_SUBTRACT
// (a_out = a_dst - a_src): every pixel a late draw touched ends with a NEGATIVE alpha (-coverage while the layer under it is empty, as it is under
// the subtitle band). RGB is blended exactly as the game does (SRC_ALPHA / INV_SRC_ALPHA). The consumer reads alpha < 0 as "display value":
// colour = saturate(rgb) (no exposure, no tone map), alpha = saturate(-alpha) before the usual max(alpha, max rgb). A game alpha is never negative
// (UNORM-range blend factors), so the other HUD pixels are untouched. The clone is replayed only while the consumer's tone map is not clamp or its
// exposure is not 1 (hud_late_marker_flag); under the defaults the game's own PSO is used and the output is bit for bit the old one (and there
// clamp(rgb * 1) == saturate(rgb), so even a stale marked frame shades the same colour).
constexpr uint32_t kBlendOne = 2u;                                   // D3D12_BLEND_ONE
constexpr uint32_t kBlendOpRevSubtract = 3u;                         // D3D12_BLEND_OP_REV_SUBTRACT: dst - src
constexpr uint32_t kLateCloneSrcAlpha = kBlendOne, kLateCloneDstAlpha = kBlendOne, kLateCloneOpAlpha = kBlendOpRevSubtract;
// Creation time: is this PSO worth a marker clone (the text pipeline, blend RT0 SRC_ALPHA / INV_SRC_ALPHA, all four channels written)?
inline bool hud_late_clone_wanted(uint32_t vs8, uint32_t ps8, bool blend0, uint32_t src, uint32_t dst, uint32_t write_mask) noexcept {
    return vs8 == kHudLateVs && ps8 == kHudLatePs && blend0 && src == kBlendSrcAlpha && dst == kBlendInvSrcAlpha && (write_mask & 0xFu) == 0xFu;
}
// The consumer's tone map / exposure are not the identity-on-display-values defaults (clamp, exposure 1): the marker clone is needed.
inline bool hud_late_needs_marker(unsigned tonemap, float exposure) noexcept { return tonemap != 0u || !(exposure == 1.0f); }
// Published by the consumer every frame (xr_stereo_output.cpp), read by the producer's Draw seam. Default false = the game's PSO is replayed.
inline std::atomic<bool>& hud_late_marker_flag() noexcept { static std::atomic<bool> f{false}; return f; }
// The consumer's reading of a stored HUD alpha (HLSL mirror: `s.a < 0`; NaN is not marked).
inline bool hud_late_is_display(float a) noexcept { return a < 0.0f; }
inline float hud_late_coverage(float a) noexcept { const float c = -a; return c < 1.0f ? c : 1.0f; }

// The decision for a draw hud_radar_decide skipped as late. on = hudlate:1; text = hud_late_text(...) == nullptr; refused = the plain hud_decide's
// reason (nullptr = accepted); capped = the frame's draw / command caps are reached.
enum HudLateAction : int { hud_late_skip = 0, hud_late_replay = 1, hud_late_skip_refused = 2, hud_late_skip_capped = 3 };
inline int hud_late_decide(bool on, bool text, const char* refused, bool capped) noexcept {
    if (!on || !text) return hud_late_skip;
    if (refused) return hud_late_skip_refused;
    if (capped) return hud_late_skip_capped;
    return hud_late_replay;
}

// Per-frame counters (Step5Hud) and their sums over the armed radar frames (Step5State).
struct HudLateFrame { unsigned draws = 0, refused = 0, capped = 0, other = 0; const char* lastRefusal = nullptr; };
struct HudLateSums {
    uint64_t frames = 0, framesWithText = 0, draws = 0, refused = 0, capped = 0, other = 0, maxDraws = 0;
    const char* lastRefusal = nullptr;
    void add(const HudLateFrame& f) noexcept {
        ++frames; framesWithText += f.draws > 0; draws += f.draws; refused += f.refused; capped += f.capped; other += f.other;
        if (f.draws > maxDraws) maxDraws = f.draws;
        if (f.lastRefusal) lastRefusal = f.lastRefusal;
    }
};
// The status line (step5-producer.log, with the -hud line cadence; only while hudlate:1). <= 440 bytes for any counter values.
inline int format_hud_late(char* b, size_t n, const char* prefix, const HudLateSums& s) noexcept {
    if (!b || n < 2) return 0;
    const int k = std::snprintf(b, n, "%.40s-hud-late;on=1;frames=%llu(text:%llu);text-draws=%llu;max-per-frame=%llu;refused=%llu;capped=%llu;"
        "late-other=%llu;last-refusal=%.40s;rule=radar-late-skip-replays-hud-text(vs534f29ca,psd34ee153,blend5/6,depth-always)",
        prefix ? prefix : "continuous", static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.framesWithText),
        static_cast<unsigned long long>(s.draws), static_cast<unsigned long long>(s.maxDraws), static_cast<unsigned long long>(s.refused),
        static_cast<unsigned long long>(s.capped), static_cast<unsigned long long>(s.other), s.lastRefusal ? s.lastRefusal : "none");
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}
// SUBTITLE SHOT (null-run evidence for HUD-LATE): the producer asks the consumer for one eye dump (xr_png_once re-arm, like png-eye.now) at a subtitle ONSET:
// a frame with >= kOnsetMinDraws replayed text draws after >= kOnsetQuietFrames frames with fewer; at most kOnsetShots per session, >= kOnsetGapMs apart.
// Process-wide request flag (producer stores true, the consumer takes it with exchange once its previous dump is finished; xr_png_once off: never taken).
inline std::atomic<bool>& hud_late_eye_request() noexcept { static std::atomic<bool> f{false}; return f; }
constexpr unsigned kOnsetQuietFrames = 30, kOnsetMinDraws = 8, kOnsetShots = 3;
constexpr uint64_t kOnsetGapMs = 20000;
struct HudLateOnset {
    unsigned quiet = kOnsetQuietFrames, shots = 0; uint64_t lastMs = 0;
    // One armed radar frame with `draws` replayed text draws at nowMs; true = request a dump now.
    bool step(unsigned draws, uint64_t nowMs) noexcept {
        if (draws < kOnsetMinDraws) { if (quiet < 1000000u) ++quiet; return false; }
        const bool onset = quiet >= kOnsetQuietFrames;
        quiet = 0;
        if (!onset || shots >= kOnsetShots || (shots && nowMs - lastMs < kOnsetGapMs)) return false;
        ++shots; lastMs = nowMs;
        return true;
    }
};
// The option line (xr-stereo.log) when hudlate:1 is set. HUD-LATE acts only through the radar region (gpu_copy_step5_hud_radar): without that key the
// producer's continuous-hud-late line stays at frames=0.
inline const char* hud_late_option_line() noexcept {
    return "xr-stereo;event=hud-late-option;on=1;effect=post-tonemap-hud-text(radio-subtitles)-replayed-into-the-hud-layer;needs=gpu_copy_step5_hud+"
           "gpu_copy_step5_hud_radar;log=step5-producer.log(continuous-hud-late);doc=docs/world-render/markers-360.md#18";
}

} // namespace acz::gpu_copy::step5
