#pragma once
// EYE-RING (docs/world-render/eye-resolution.md section 5.1 (a)): the per-eye PRODUCER targets (the replayed world objects / cockpit members, their depth,
// and the per-eye smoke t13 / sprite t14 images) at the EYE SWAPCHAIN size instead of the game's frame size. Only with eyeres > 0; sub-option eyeobj:<0|1>
// of xr_terrain_stage (default 1 while eyeres is on; eyeobj:0 keeps the producer at the game size = the eyeres step-2 behaviour).
//
// Why it is exact: every replayed draw writes NDC through the game's own GS-unit constants (+400 = 512, -448, 2048, 2048; acm-shader-dataflow.md) and the
// eye matrices (HMD FOV), so the target size enters ONLY through the viewport. The game's viewport and scissor are scaled per axis by (eye / game) for
// the replayed draws and the game's own are set back before its draw follows. No game shader reads SV_Position or a pixel-size constant (DrawACM_p /
// DrawCPT_ps read COLOR/TEXCOORD only; dataflow doc section 2). The consumer samples every per-eye image by uv over the same FOV (full-rect).
// The window images (world copy, HUD, markers, the mono effects image) stay at the game size: they are game-screen images.
//
// Channel: the proxy publishes the REQUEST at cfg time (eyeres > 0 and the eyeobj flag), Output::setup publishes the EXTENT (the two eye swapchains,
// the larger of each axis) once the runtime's recommended size is known. The producer reads both ONCE, when it creates its ring (process lifetime).
// eyeres:0 / no token: nothing is ever published, choose() returns the game size with why "off" and the producer never logs, records or creates
// anything new (byte-identical).
// SDK-free, header-only; tests/acz-vr/eye_res_offline_tests.cpp (math, choice, fallback) + gpu_copy_step1_offline_tests step5-eye-ring* (recordings).
#include <atomic>
#include <cstdint>

namespace acz::eyering {

// ---- the channel (one per module; the proxy, Output and the producer live in the same DLL)
inline std::atomic<uint32_t>& request_word() noexcept { static std::atomic<uint32_t> v{0}; return v; }   // bit0 eyeres on, bit1 eyeobj on, bit31 set
inline std::atomic<uint64_t>& extent_word() noexcept { static std::atomic<uint64_t> v{0}; return v; }    // w | h << 24 | 1 << 63
inline void request(bool eyeres_on, bool objects) noexcept {
    request_word().store(0x80000000u | (eyeres_on ? 1u : 0u) | (objects ? 2u : 0u), std::memory_order_release);
}
inline void publish_extent(uint32_t w, uint32_t h) noexcept {
    if (!w || !h || w >= (1u << 24) || h >= (1u << 24)) return;
    extent_word().store(uint64_t(w) | (uint64_t(h) << 24) | (1ull << 63), std::memory_order_release);
}
inline void reset_for_tests() noexcept { request_word().store(0); extent_word().store(0); }
struct Channel { bool requested = false, objects = false, extent_known = false; uint32_t w = 0, h = 0; };
inline Channel read() noexcept {
    Channel c;
    const uint32_t r = request_word().load(std::memory_order_acquire);
    c.requested = (r & 0x80000000u) && (r & 1u);
    c.objects = c.requested && (r & 2u);
    const uint64_t e = extent_word().load(std::memory_order_acquire);
    if (e >> 63) { c.extent_known = true; c.w = uint32_t(e & 0xffffffu); c.h = uint32_t((e >> 24) & 0xffffffu); }
    return c;
}

// ---- caps (the eye extent is already clamped to eyeres::kPixelBudget = 4096 x 4096 by Output)
inline constexpr uint32_t kMaxSide = 4096;
inline constexpr uint64_t kImageCap = 4096ull * 4096ull * 8ull;   // one FP16 / D32S8X24 eye image at the pixel budget: 128 MiB
// Commands one replayed member / per-eye effects replay adds on the eye ring: RSSetViewports + RSSetScissorRects (eye), the same two (game) after.
inline constexpr unsigned kExtraCommands = 4;

// ---- the ring size decision (owner context, once)
struct Choice { bool eye = false; uint32_t w = 0, h = 0; const char* why = "off"; };
// game_w/h: the game's viewport extents (step3 Targets); bpp: bytes per pixel of the ring RT and DS (0 = no depth).
inline Choice choose(const Channel& c, uint32_t game_w, uint32_t game_h, unsigned rt_bpp, unsigned ds_bpp) noexcept {
    Choice r; r.w = game_w; r.h = game_h;
    if (!c.requested) { r.why = "off"; return r; }                         // eyeres:0 / absent: never logged
    if (!c.objects) { r.why = "eyeobj-off"; return r; }
    if (!c.extent_known) { r.why = "no-eye-extent"; return r; }             // XR swapchains not created before the ring (no vsearly): game size, process lifetime
    if (!game_w || !game_h) { r.why = "game-size-unknown"; return r; }
    if (!c.w || !c.h || c.w > kMaxSide || c.h > kMaxSide) { r.why = "eye-size-refused"; return r; }
    if (uint64_t(c.w) * c.h * rt_bpp > kImageCap || uint64_t(c.w) * c.h * ds_bpp > kImageCap) { r.why = "eye-over-image-cap"; return r; }
    if (c.w == game_w && c.h == game_h) { r.why = "eye-equals-game"; return r; }   // nothing to rewrite: the old path
    r.eye = true; r.w = c.w; r.h = c.h; r.why = "eye";
    return r;
}

// ---- viewport / scissor substitution (per replayed draw; the game's own state is set back after it)
struct Viewport { float x = 0, y = 0, w = 0, h = 0, min_depth = 0, max_depth = 1; };
struct Rect { int32_t l = 0, t = 0, r = 0, b = 0; };
// The game's viewport scaled per axis by (eye / game); depth range unchanged. NDC -> the same relative place of the eye image.
inline Viewport scale_viewport(const Viewport& g, uint32_t game_w, uint32_t game_h, uint32_t eye_w, uint32_t eye_h) noexcept {
    Viewport v = g;
    if (!game_w || !game_h) return v;
    const double sx = double(eye_w) / double(game_w), sy = double(eye_h) / double(game_h);
    v.x = float(double(g.x) * sx); v.w = float(double(g.w) * sx);
    v.y = float(double(g.y) * sy); v.h = float(double(g.h) * sy);
    return v;
}
// The game's scissor scaled per axis and rounded OUTWARD (floor the left/top, ceil the right/bottom: no covered eye pixel is cut), clamped to the eye
// image. An empty game scissor stays empty. Integer arithmetic (exact for every int32 input).
inline Rect scale_scissor(const Rect& g, uint32_t game_w, uint32_t game_h, uint32_t eye_w, uint32_t eye_h) noexcept {
    Rect s;
    if (!game_w || !game_h) return s;
    auto lo = [](int64_t v, uint32_t gs, uint32_t es) -> int32_t {
        if (v <= 0) return 0;
        const int64_t q = v * int64_t(es) / int64_t(gs);
        return int32_t(q > int64_t(es) ? int64_t(es) : q);
    };
    auto hi = [](int64_t v, uint32_t gs, uint32_t es) -> int32_t {
        if (v <= 0) return 0;
        const int64_t q = (v * int64_t(es) + int64_t(gs) - 1) / int64_t(gs);
        return int32_t(q > int64_t(es) ? int64_t(es) : q);
    };
    s.l = lo(g.l, game_w, eye_w); s.t = lo(g.t, game_h, eye_h);
    s.r = hi(g.r, game_w, eye_w); s.b = hi(g.b, game_h, eye_h);
    if (g.r <= g.l || s.r < s.l) s.r = s.l;   // empty stays empty
    if (g.b <= g.t || s.b < s.t) s.b = s.t;
    return s;
}
// The member path can only set the game's RS state back exactly when the game has ONE viewport and ONE scissor bound (and the step-3 RS tracking is
// complete). nullptr = substitutable, else the refusal (the member is refused alone, as any other member refusal).
inline const char* rs_restorable(bool rs_known, bool rs_gap, uint32_t viewports, uint32_t scissors) noexcept {
    if (!rs_known || rs_gap) return "eye-ring-rs-unknown";
    if (viewports != 1) return "eye-ring-viewport-count";
    if (scissors != 1) return "eye-ring-scissor-count";
    return nullptr;
}

// ---- memory (DEFAULT heap; logged once at ring creation and in eye-resolution.md section 5.1)
inline uint64_t image_bytes(uint32_t w, uint32_t h, unsigned bpp) noexcept { return uint64_t(w) * h * bpp; }
// The object ring: 2 eyes x slots x (RT + DS).
inline uint64_t ring_bytes(uint32_t w, uint32_t h, unsigned rt_bpp, unsigned ds_bpp, unsigned slots) noexcept {
    return (image_bytes(w, h, rt_bpp) + image_bytes(w, h, ds_bpp)) * 2u * slots;
}
// One per-eye image kind (smoke or sprite): 2 eyes x slots.
inline uint64_t per_eye_kind_bytes(uint32_t w, uint32_t h, unsigned bpp, unsigned slots) noexcept { return image_bytes(w, h, bpp) * 2u * slots; }

}  // namespace acz::eyering
