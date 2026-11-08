#pragma once
// fx_smoke_gpu.h -- EFFECTS-OUTSIDE (C) step 3: the GPU smoke replay per eye (fxmem:4; docs/world-render/effects-outside-window.md section 8.7).
// SDK-free policy for the producer (gpu_copy_step5_hooks.inl step5FxSmokeGpu*) and the CPU mirror of the consumer composite (xr_stereo_blit.hlsl smoke_over).
//
// What the GPU replay does (one captured + prepared smoke draw, fx_smoke.h prepare_eye_records already wrote both eye copies of the record):
//   the two eye copies go to the frame slot's UPLOAD block (this slot's span only, written while the slot is PRODUCING), then, inline before the game's own
//   smoke draw and under the SAME predicate word the effects layer opened for that draw (speculative like every effects replay: executed only when a later
//   cockpit member commits the epoch), the draw is recorded twice with the coverage_v1 clone PSO: render target = the slot's per-eye SMOKE image, depth =
//   that eye's objects depth-stencil (test only: the smoke PSO writes no depth), root SRV t10 = the eye's copy. The game's t10 VA, PSO, predication and render
//   targets are bound back before the game's draw. The smoke draw is then NOT replayed into the window effects image (it would show twice inside the window).
// Why this fixes "vehicles render through the smoke" (headset test 19, D-362): the window effects image was replayed against the HUD depth-stencil (cleared
//   to 0, holds nothing) and composited UNDER the eye image (xr_stereo_blit.hlsl: o + (1 - o.a) * world_layer, the effects being part of world_layer), so
//   every replayed object pixel covered the smoke whatever its depth. The eye smoke image is depth-tested against the eye's OWN objects depth (same eye
//   matrices, same depth scale: substitute applies the objects' depth scale to A's z column) and composited OVER the objects: smoke behind an object is
//   discarded by the depth test, smoke in front of it covers it.
//
// Lifetime (held-slot rules, reviewed with the rest of the ring): the per-eye smoke images and the UPLOAD block are created at most ONCE per process, only
// when fxmem:4 asks for it (after the effects layer is ready): at ring creation, or late by the worker tick under the step-1 mutex when the option arrives
// after the ring (live run R2), published to the frames by smokeReady set last; kept for the process lifetime like every other slot resource; nothing is
// ever released or recreated. A slot's images and its UPLOAD span are written only by a recording into that slot, i.e. while the slot is
// PRODUCING (never HELD by the consumer, never owned by another live recording: step5PickSlot_locked, J-180 owner table); the consumer reads them only while
// it HOLDS the slot (published -> held compare-exchange), after its queue waited on the slot's fence value. A re-execution of the owning recording rewrites
// exactly the same bytes and is quarantined like every other slot write (DECOUPLE review 3). Invariant text kept in the docs section 8.7.
#include "fx_smoke.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::fxsmoke::gpu {

// ---- memory (charged like step3::finalize: 64 KiB aligned resource + one 64 KiB descriptor heap per target)
inline constexpr uint64_t kAlign = 65536;
inline constexpr uint64_t kImageCap = 32ull << 20;    // one eye smoke image (2560x1440 RGBA16F = 29.5 MB)
inline constexpr uint64_t kTotalCap = 200ull << 20;   // all slots x 2 eyes + the UPLOAD block (3 x 2 x 29.6 MB + 768 KiB = 178 MB at 2560x1440)
inline constexpr uint64_t align_up(uint64_t v, uint64_t a) noexcept { return (v + a - 1) / a * a; }
inline constexpr uint64_t kSlotUpload = align_up(ReplayStore::kBytes, kAlign);   // 204,800 B of eye copies per slot (64 draws) -> one 256 KiB span
static_assert(kSlotUpload == 262144, "one 256 KiB UPLOAD span per slot (64 smoke draws x 2 eyes x 1,600 B)");
// nullptr = within the caps (`charged` set); else why. bytes_per_pixel = 0: unsupported format.
// EYE-RING (eye_ring.h): on the eye ring the images are the eye swapchain size (2688x2688 RGBA16F = 55 MiB each, 331 MiB + upload for 3 slots x 2 eyes);
// the producer passes kEyeImageCap / kEyeTotalCap then. Every other caller: the game-size caps above, unchanged.
inline constexpr uint64_t kEyeImageCap = 4096ull * 4096ull * 8ull;   // 128 MiB: one RGBA16F image at eyeres::kPixelBudget
inline constexpr uint64_t kEyeTotalCap = 800ull << 20;               // 3 slots x 2 eyes at the pixel budget (774 MiB) + the UPLOAD block
inline const char* budget(unsigned bytes_per_pixel, uint64_t width, uint32_t height, unsigned slots, uint64_t& charged,
                          uint64_t image_cap = kImageCap, uint64_t total_cap = kTotalCap) noexcept {
    charged = 0;
    if (!bytes_per_pixel) return "smoke-format-unsupported";
    if (!width || !height || width > 16384 || height > 16384 || !slots) return "smoke-size-unsupported";
    const uint64_t bytes = width * height * bytes_per_pixel;
    if (bytes > image_cap) return "smoke-over-image-cap";
    const uint64_t total = (align_up(bytes, kAlign) + kAlign) * 2 * slots + kSlotUpload * slots;
    if (total > total_cap) return "smoke-over-total-cap";
    charged = total;
    return nullptr;
}

// ---- per-draw gate (every input is a fact the producer already holds at the smoke draw; nullptr = replay per eye, else the named refusal and the
// window effects replay of the draw runs exactly as before = the in-window smoke of fxmem:0/1/2).
inline constexpr uint32_t kCmpGreater = 5, kCmpGreaterEqual = 7;   // D3D12_COMPARISON_FUNC (gpu_copy_step3.h cmp)
struct GateIn {
    bool ready = false;            // the slot's two eye smoke images + its UPLOAD span exist (owner context, ring creation)
    bool eye_depth = false;        // the frame's eye targets have a depth-stencil (the objects depth the smoke is tested against)
    bool dual = false;             // effects_dual on: the coverage_v1 clone exists for this PSO (the consumer composites coverage_v1 only)
    bool effects_recorded = false; // the effects layer accepted this draw (predicate slot opened, clears scheduled)
    bool prepared = false;         // the record was prepared for both eyes THIS frame (ReplayStore index valid)
    bool t10_root_srv = false;     // t10 is one root SRV parameter (the capture's test: not a table / vertex binding, trackers agree)
    uint32_t pso_dsv = 0, eye_dsv = 0, pso_rtv0 = 0, image_format = 0, pso_samples = 0, pso_rt_count = 0;
    bool depth_enable = false, depth_write = false, stencil = false; uint32_t depth_func = 0;
    bool stencil_noop = false;     // stencil on but provably inert: front/back ALWAYS and (no write mask or every op KEEP) -- the eye stencil content cannot matter
    bool vp_known = false; unsigned viewports = 0; float vp[4] = {}; uint32_t image_w = 0, image_h = 0;
};
inline const char* gate(const GateIn& g) noexcept {
    if (!g.ready) return "smoke-gpu-not-ready";
    if (!g.effects_recorded) return "smoke-effects-not-recorded";
    if (!g.dual) return "smoke-needs-effects-dual";
    if (!g.prepared) return "smoke-not-prepared";
    if (!g.t10_root_srv) return "smoke-t10-not-root-srv";
    if (!g.eye_depth || !g.eye_dsv) return "smoke-eye-depth-missing";
    // The open point of section 8.6: the smoke PSO's DSV format / sample count must be the eye objects DSV's, else the DSV cannot be bound with it.
    if (g.pso_dsv != g.eye_dsv) return "smoke-dsv-format-differs";
    if (g.pso_samples != 1) return "smoke-pso-multisample";
    if (g.pso_rt_count != 1 || g.pso_rtv0 != g.image_format) return "smoke-rt-format-differs";
    if (!g.depth_enable || !(g.depth_func == kCmpGreater || g.depth_func == kCmpGreaterEqual)) return "smoke-pso-depth-test";
    if (g.depth_write) return "smoke-pso-depth-write";       // would write the eye's objects depth
    if (g.stencil && !g.stencil_noop) return "smoke-pso-stencil";   // a real stencil test: the eye stencil holds the cockpit replay's values, not the game's
    if (!g.vp_known || g.viewports != 1) return "smoke-viewport-unknown";
    if (g.vp[0] != 0.0f || g.vp[1] != 0.0f || g.vp[2] != float(g.image_w) || g.vp[3] != float(g.image_h)) return "smoke-viewport-not-image";
    return nullptr;
}

// ---- the recorded sequence (the GPU path records exactly this, native vtable calls; offline-checked against the fake list)
// [ClearRTV(effects) / ClearDSV(HUD) as the effects call scheduled them] [ClearRTV(smoke eye 0), ClearRTV(smoke eye 1): first replay of the frame]
// [SetPredication(pred, op)] then replay_commands(swap, dual=true) = SetPipelineState(clone) {OMSet(smoke e, eye objects DSV e), RootSRV(t10, eye e copy), Draw} x 2,
// RootSRV(t10, game VA), SetPipelineState(game), OMSet(the game's targets) -- with SetPredication(null) recorded right BEFORE the final OMSet. The game's own
// draw follows, forwarded by the caller, under exactly its own state.
inline constexpr unsigned kCmdsPerDraw = 12;   // pred on, PSO clone, 2 x (OM, SRV, Draw), SRV restore, PSO game, pred off, OM restore
inline constexpr unsigned kCmdsClear = 2;      // the two smoke image clears (once per frame)

// ---- per-frame state (inside the frame's effects bookkeeping: reset when the effects layer arms)
template <unsigned Cap>
struct BasicFrameState {
    static constexpr unsigned kCap = Cap;
    bool on = false;            // fxmem:4 latched for this frame and the GPU resources ready
    bool cleared = false;       // both eye smoke images were cleared in this frame's recording (unpredicated)
    unsigned replays = 0, refused = 0, committed = 0;
    const char* last_why = nullptr;
    uint64_t pred_offset[kCap] = {}; bool predicated[kCap] = {};
    void begin(bool enabled) noexcept { *this = BasicFrameState{}; on = enabled; }
    void note_refused(const char* why) noexcept { ++refused; last_why = why ? why : "smoke-refused"; }
    // One recorded replay pair (both eyes) under the predicate word at pred_offset (predicated = false: predication mode off, always executes).
    void note_replay(bool pred, uint64_t offset) noexcept {
        if (replays < kCap) { pred_offset[replays] = offset; predicated[replays] = pred; }
        ++replays;
    }
    // At the list's Close (the CPU commits are final): how many replays the GPU will execute. mode: 0 = eq0 (word != 0 executes), 1 = ne0 (word == 0
    // executes), 2 = predication off (all execute) -- gpu_copy::step5 effects_pred_eq0 / _ne0 / _off.
    void close(const uint8_t* pred_base, int mode) noexcept {
        committed = 0;
        for (unsigned i = 0; i < replays && i < kCap; ++i) {
            if (!predicated[i] || mode == 2) { ++committed; continue; }
            if (!pred_base) continue;
            uint64_t w = 0; std::memcpy(&w, pred_base + pred_offset[i], sizeof w);
            if (mode == 1 ? w == 0 : w != 0) ++committed;
        }
    }
    // Published as StereoFrame::smoke_valid: cleared this frame AND at least one replay executes (else the image holds nothing: not sampled).
    bool valid() const noexcept { return on && cleared && committed > 0; }
};
// The smoke store holds ReplayStore::kCap (64) draws; the sprite path (fx_sprite_gpu.h) sizes its own to kMaxDraws (W5-2 P2).
using FrameState = BasicFrameState<ReplayStore::kCap>;

// ---- consumer composite, CPU mirror of xr_stereo_blit.hlsl smoke_layer / smoke_over (mode_bits bit 18)
inline constexpr float kObjectBand = 0.0009765625f;   // kOccludeObjectDepth: replayed objects 0 < dz < 2^-10 (xr_world_objects_depth_fix), cockpit above
struct Px { float r = 0, g = 0, b = 0, a = 0; };
// The smoke texel (coverage_v1: P premultiplied colour, A coverage) as a layer: un-premultiply, exposure, tonemap (0 clamp, 1 Reinhard), premultiply, times
// the decouple edge fade of the eye image (1 = no re-projection).
inline Px smoke_layer(const float e[4], float exposure, int tonemap, float fade) noexcept {
    const float a = e[3] > 0.0f ? (e[3] < 1.0f ? e[3] : 1.0f) : 0.0f;
    float c[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
        float v = a > 0.0f ? (e[k] / a) * exposure : 0.0f;
        v = v > 0.0f ? v : 0.0f;
        c[k] = tonemap == 1 ? v / (1.0f + v) : (v < 1.0f ? v : 1.0f);
    }
    Px p; p.r = c[0] * a * fade; p.g = c[1] * a * fade; p.b = c[2] * a * fade; p.a = a * fade;
    return p;
}
// `under` = the eye pixel as composited without smoke (objects / terrain / world, premultiplied); dz = the eye depth at the pixel (0 = nothing replayed).
// The replayed COCKPIT band (dz >= 2^-10) keeps its pixel when the replayed cockpit is shown (smoke drawn before a later cockpit member is not occluded by
// it in the eye depth); with the model cockpit (COCKPIT-2) that band is transparent in `under` and the model is drawn over the result anyway.
inline bool smoke_applies(float dz, bool model_cockpit) noexcept { return model_cockpit || !(dz >= kObjectBand); }
inline Px smoke_over(const Px& under, const Px& smoke, float dz, bool model_cockpit) noexcept {
    if (!smoke_applies(dz, model_cockpit)) return under;
    Px o; const float k = 1.0f - smoke.a;
    o.r = smoke.r + k * under.r; o.g = smoke.g + k * under.g; o.b = smoke.b + k * under.b; o.a = smoke.a + k * under.a;
    return o;
}
// The eye depth a replayed object / smoke puff writes / tests at view depth w (the A depth model z_s = Z0 + b w over w, times the objects' depth scale).
inline double eye_depth(double w, double depth_scale) noexcept { return depth_scale * (acz::fxmem::kZ0 / w + acz::fxmem::kDepthB) * kDepthUnit; }
// The same for the view's own depth model (b, Z0 from the frame's A: chase / HUD-only differ from the cockpit, effects-outside-window.md 22.3).
inline double eye_depth(double w, double depth_scale, double b, double z0) noexcept { return depth_scale * (z0 / w + b) * kDepthUnit; }
// The D3D12 depth test of the smoke PSO (GREATER / GREATER_EQUAL against the stored eye depth; 0 = cleared, nothing drawn).
inline bool depth_passes(double smoke, double stored, uint32_t func) noexcept { return func == kCmpGreaterEqual ? smoke >= stored : smoke > stored; }

// ---- log lines (step5-producer.log; < kLineLimit - kLinePrefix bytes for any values, offline-tested)
inline int format_frame(char* buf, size_t n, uint64_t frame, const FrameState& s, unsigned fallback_window) noexcept {
    auto cu = [](unsigned v) { return v > 99999u ? 99999u : v; };
    return std::snprintf(buf, n, "fx-smoke-gpu;frame=%llu;replays=%u;committed=%u;refused=%u;window-fallback=%u;cleared=%d;valid=%d;last-why=%.24s;"
        "mode=gpu-replay(per-eye,eye-objects-depth-test,effects-predicate,window-smoke-suppressed)",
        static_cast<unsigned long long>(frame), cu(s.replays), cu(s.committed), cu(s.refused), cu(fallback_window), s.cleared ? 1 : 0, s.valid() ? 1 : 0,
        s.last_why ? s.last_why : "none");
}
inline int format_ring(char* buf, size_t n, bool ready, const char* why, unsigned slots, uint32_t format, uint64_t w, uint32_t h, uint64_t charged) noexcept {
    return std::snprintf(buf, n, "fx-smoke-ring;outcome=%s;reason=%.32s;slots=%u;format=%u;size=%llux%u;charged=%llu;caps=image:%llu,total:%llu;"
        "per-slot=2x(smoke-rt)+upload:%llu;dsv=eye-objects-depth(test-only);per-draw=%u(+%u-clears-once);lifetime=process(ring)",
        ready ? "ready" : "refused", why ? why : "none", slots, format, static_cast<unsigned long long>(w), h, static_cast<unsigned long long>(charged),
        static_cast<unsigned long long>(kImageCap), static_cast<unsigned long long>(kTotalCap), static_cast<unsigned long long>(kSlotUpload), kCmdsPerDraw, kCmdsClear);
}

} // namespace acz::fxsmoke::gpu
