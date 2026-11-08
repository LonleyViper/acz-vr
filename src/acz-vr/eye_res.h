#pragma once
// EYE-RES (headset test 8, docs/world-render/eye-resolution.md): the size policy of the two EYE swapchains. Sub-option of the existing key
// xr_terrain_stage (the user's cfg already has the 32-key maximum, so no new key):
//   eyeres:0        (default, also when the token is absent) the old policy: source aspect, width min(source, cap 2400, runtime max)
//                   -> 2400x1350 for a 2560x1440 game. Every line and swapchain exactly as before.
//   eyeres:<50..200> the eye swapchains are the runtime's RECOMMENDED per-eye size (xrEnumerateViewConfigurationViews, i.e. SteamVR's own
//                   render resolution for this headset, the size SteamVR's cinema screen is rendered at) times <pct>/100 per axis, clamped to the
//                   runtime's max (aspect kept) and to kPixelBudget per eye (aspect kept). 100 = exactly the recommended size.
// Why: test 8 logged eye0=2400x1350 (16:9) against recommended=2688x2688 for a ~97x97 deg (tan 2.33 x 2.30) eye FOV. Full-rect mapping spreads
// 1350 rows over the whole vertical FOV: 587 px/tan vertically and 1030 px/tan horizontally against 1154 px/tan recommended (layer-fov line),
// i.e. half the vertical density and 45 % of the pixels. Everything drawn natively into the eye (terrain, sky, clouds, props, the model cockpit,
// the UI3D / briefing tables, the minified game world window) is limited by it.
// Only the two eye swapchains change; the world-quad / HUD / marker quad swapchains keep the source-aspect policy (their content is a 16:9 quad).
// Prerequisite (checked by the proxy): gpu_copy_step5_hmd_fov, i.e. the projection layer is submitted with the headset's own FOV. In the lens
// mode the layer FOV is the game lens (63x38 deg, 16:9) and a square swapchain would only waste rows.
// SDK-free, header-only; tests/acz-vr/eye_res_offline_tests.cpp (+ xr_stereo_output_offline_tests --eye-res for the Output).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace acz::eyeres {

inline constexpr uint32_t kMinPct = 50, kMaxPct = 200;
// Per-eye pixel budget: 4096 x 4096. At the budget one eye costs 64 MiB per RGBA8 swapchain image (x3 images) plus the props targets
// (RGBA8 + RGBA32F = 20 B/px, 320 MiB) and the model-cockpit depth (4 B/px); see eye-resolution.md section 4.
inline constexpr uint64_t kPixelBudget = 4096ull * 4096ull;

struct Options {
    uint32_t pct = 0;      // 0 = off (old policy)
    bool any_token = false;
    // EYE-RING (eye_ring.h, eye-resolution.md 5.1 (a)): eyeobj:<0|1>, the per-eye producer targets at the eye size. Default 1; only acts with pct > 0.
    // eyeobj:0 = the fallback: the producer stays at the game size (the eyeres step-2 behaviour), the eye swapchains still follow eyeres.
    bool objects = true;
    bool any_obj_token = false;
};

// Removes every eyeres:<v> and eyeobj:<v> token (':' or '=') from a "<id>;opt:val;..." value (the first item, the stage id, is never an option). A
// value without them is returned byte-identical. A bad value: false + why, the token is STILL removed (the caller logs it; the terrain parser never
// sees it) and the policy stays off (eyeobj: a bad value turns the producer part off, eyeres keeps its own value).
inline bool extract_options(const std::string& value, std::string& rest, Options& o, std::string& why) {
    o = Options{};
    rest.clear();
    bool ok = true;
    size_t pos = 0; bool first = true;
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        bool ours = false;
        if (!first) {
            const size_t eq = item.find_first_of(":=");
            if (eq == 6 && item.compare(0, 6, "eyeres") == 0) {
                ours = true; o.any_token = true;
                const std::string v = item.substr(eq + 1);
                uint32_t n = 0; bool digits = !v.empty() && v.size() <= 3;
                for (char c : v) { if (c < '0' || c > '9') { digits = false; break; } n = n * 10u + uint32_t(c - '0'); }
                if (digits && (n == 0 || (n >= kMinPct && n <= kMaxPct))) o.pct = n;
                else { ok = false; o.pct = 0; why = "bad value for eyeres (0 or 50..200)"; }
            } else if (eq == 6 && item.compare(0, 6, "eyeobj") == 0) {
                ours = true; o.any_obj_token = true;
                const std::string v = item.substr(eq + 1);
                if (v == "0") o.objects = false;
                else if (v == "1") o.objects = true;
                else { ok = false; o.objects = false; why = "bad value for eyeobj (0 or 1)"; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return ok;
}
inline std::string describe(const Options& o) { return "eyeres=" + std::to_string(o.pct); }
// EYE-RING: the trailing token of the eye-res-config line (only when an eyeobj token was given or eyeres is on; eyeres off and no eyeobj: "").
inline std::string describe_objects(const Options& o) {
    if (!o.pct && !o.any_obj_token) return "";
    return std::string(";eyeobj=") + (o.objects ? "1" : "0") + (o.pct ? "" : "(inactive:eyeres-off)");
}

// The eye swapchain size of the eyeres policy. clamp: "none", "max" (runtime / extent limit) or "budget" (kPixelBudget).
// A zero input, or pct outside 50..200, returns 0x0 (the caller keeps the old policy).
struct Size { uint32_t w = 0, h = 0; const char* clamp = "none"; };
inline Size recommended_extent(uint32_t rec_w, uint32_t rec_h, uint32_t lim_w, uint32_t lim_h, uint32_t pct, uint64_t budget = kPixelBudget) noexcept {
    Size s;
    if (!rec_w || !rec_h || !lim_w || !lim_h || pct < kMinPct || pct > kMaxPct || !budget) return s;
    // per-axis scale, half-up rounding in integers: round(rec * pct / 100)
    uint64_t w = (uint64_t(rec_w) * pct * 2 + 100) / 200, h = (uint64_t(rec_h) * pct * 2 + 100) / 200;
    if (w > lim_w || h > lim_h) {   // shrink both axes by the tighter limit (aspect kept)
        const double k = std::fmin(double(lim_w) / double(w), double(lim_h) / double(h));
        w = uint64_t(std::floor(double(w) * k + 1e-6)); h = uint64_t(std::floor(double(h) * k + 1e-6));
        s.clamp = "max";
    }
    if (w * h > budget) {           // shrink both axes by sqrt(budget / pixels) (aspect kept), then step down until inside
        const double k = std::sqrt(double(budget) / double(w * h));
        w = uint64_t(std::floor(double(w) * k + 1e-6)); h = uint64_t(std::floor(double(h) * k + 1e-6));
        while (w * h > budget && w > 1 && h > 1) { --w; --h; }
        s.clamp = "budget";
    }
    s.w = uint32_t(w < 1 ? 1 : w > lim_w ? lim_w : w);
    s.h = uint32_t(h < 1 ? 1 : h > lim_h ? lim_h : h);
    return s;
}

// Pixel density of an image of `px` pixels spread (full-rect) over the tangent span [lo, hi] (XrFovf angles in radians, lo < 0 < hi usually).
// Centre density in px per degree = px_per_tan * pi / 180.
inline double px_per_tan(uint32_t px, double lo, double hi) noexcept {
    const double span = std::tan(hi) - std::tan(lo);
    return span > 0.0 && std::isfinite(span) ? double(px) / span : 0.0;
}
inline double mpix(uint32_t w, uint32_t h) noexcept { return double(w) * double(h) / 1.0e6; }
// Bytes the eye size drives (both eyes): the swapchain images (RGBA8, `images` each), the props targets (RGBA8 + RGBA32F) and the model-cockpit
// depth (D32, one, sized to the larger eye). An estimate for the log; the runtime may add its own copies.
struct Bytes { uint64_t swapchains = 0, props = 0, depth = 0; };
inline Bytes eye_bytes(uint32_t w0, uint32_t h0, uint32_t w1, uint32_t h1, uint32_t images) noexcept {
    Bytes b;
    const uint64_t p0 = uint64_t(w0) * h0, p1 = uint64_t(w1) * h1;
    b.swapchains = (p0 + p1) * 4u * images;
    b.props = (p0 + p1) * 20u;
    b.depth = uint64_t(w0 > w1 ? w0 : w1) * uint64_t(h0 > h1 ? h0 : h1) * 4u;
    return b;
}

// ---- WORLD-WINDOW FILTER (eyeres > 0 only; eye-resolution.md section 3b). CPU mirror of world_rgb() in xr_stereo_blit.hlsl.
// The game's world image (2560x1440, one mip level) is shown as a plane covering the game lens; each eye pixel samples it once (bilinear), which
// minifies 1.8x (eyeres:100) to 3.5x (old 2400x1350 eyes) and aliases. With the filter, an eye pixel whose footprint is larger than kFilterOneTap
// world texels on either axis takes a 2x2 grid of bilinear taps at +-1/4 of its footprint (a box over the footprint, equal weights). The footprint
// is the Jacobian of the eye-uv -> world-uv map, measured per pixel by two extra ray/plane hits one eye pixel to the right and one down (ju, jv in
// world-image uv per eye pixel). Otherwise (magnified or about 1:1): the old single tap.
// Why 2x2 only: per-axis 1..4 grids (loops or an unrolled 4x4) passed fxc but broke unrelated shader modes on WARP; the straight-line 2x2 does not.
// 1-D shimmer model (white-noise texture, eye pixel shifted in 1/16 steps, std dev of the pixel; eye_res_offline prints it): r = 1.8 (eyeres:100,
// test 8): 1 tap 0.132 -> 2 taps 0.079; r = 3.54 (old eyes, vertical): 0.174 -> 0.100 (3 taps would reach 0.069; not used).
// Chosen over a per-frame mip chain: no new resource, no pass, no state change on the producer-owned world image (0 MiB); the extra cost is on
// window pixels only (section 3b). mode_bits bit 21 (kModeBitWorldFilter).
inline constexpr float kFilterOneTap = 1.25f;
inline constexpr uint32_t kFilterMaxGrid = 2, kFilterMaxTaps = kFilterMaxGrid * kFilterMaxGrid;
// Grid along one footprint axis of r world texels per eye pixel (a lone axis; filter_select takes the larger axis). NaN / negative -> 1.
inline uint32_t filter_grid(float r) noexcept { return r > kFilterOneTap ? kFilterMaxGrid : 1u; }
// Footprint axis length in world texels: |j * (W, H)| with j in world uv per eye pixel.
inline float footprint_texels(float ju, float jv, uint32_t world_w, uint32_t world_h) noexcept {
    const float a = ju * float(world_w), b = jv * float(world_h);
    return std::sqrt(a * a + b * b);
}
struct FilterGrid { uint32_t gu = 1, gv = 1; uint32_t taps() const noexcept { return gu * gv; } };
// 2x2 when either footprint axis exceeds kFilterOneTap texels, else 1x1 (the shader: k = 0.25 or 0).
inline FilterGrid filter_select(const float ju[2], const float jv[2], uint32_t world_w, uint32_t world_h) noexcept {
    FilterGrid g;
    const float r = std::fmax(footprint_texels(ju[0], ju[1], world_w, world_h), footprint_texels(jv[0], jv[1], world_w, world_h));
    g.gu = g.gv = filter_grid(r);
    return g;
}
// The uv offset of tap (i, j) of the grid: ((i + 0.5)/gu - 0.5) * ju + ((j + 0.5)/gv - 0.5) * jv. A 1x1 grid has offset 0 (the old tap); the 2x2
// offsets are +-(ju + jv)/4 and +-(ju - jv)/4, exactly the shader's o1 / o2.
inline void filter_offset(const FilterGrid& g, uint32_t i, uint32_t j, const float ju[2], const float jv[2], float out[2]) noexcept {
    const float a = (float(i) + 0.5f) / float(g.gu) - 0.5f, b = (float(j) + 0.5f) / float(g.gv) - 0.5f;
    out[0] = a * ju[0] + b * jv[0];
    out[1] = a * ju[1] + b * jv[1];
}
// Footprint (world texels per eye pixel) at the centre for a pinhole eye of `eye_px` pixels over a tan span `eye_span` looking straight at a window
// of `win_px` texels over a tan span `win_span` (one axis): r = (win_px / win_span) / (eye_px / eye_span). Tests (test 8: 2081 px/tan window).
inline float centre_ratio(uint32_t win_px, double win_span, uint32_t eye_px, double eye_span) noexcept {
    if (!(win_span > 0.0) || !(eye_span > 0.0) || !eye_px) return 0.0f;
    return float((double(win_px) / win_span) / (double(eye_px) / eye_span));
}

}  // namespace acz::eyeres
