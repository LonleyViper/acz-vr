// EYE-RES offline tests (src/acz-vr/eye_res.h, docs/world-render/eye-resolution.md): the eyeres: option inside xr_terrain_stage, the recommended-size
// eye extent (scale, runtime max, pixel budget, aspect), the density numbers of headset test 8 (layer-fov line: 1030.1 x 587.3 px/tan at 2400x1350)
// and the byte estimate. No D3D12, no runtime, no headset: fixture success says nothing about the image in the headset.
#include "eye_res.h"
#include "eye_ring.h"
#include "fx_sprite_gpu.h"   // EYE-RING: the smoke / sprite budgets at the eye size (fx_smoke_gpu.h through it)
#include "terrain_preset.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace acz::eyeres;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near_eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

namespace {

void test_option() {
    std::string rest, why; Options o;
    // absent: byte-identical, off
    const std::string user = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;vrn:1";
    CHECK(extract_options(user, rest, o, why) && rest == user && o.pct == 0 && !o.any_token);
    // present: removed, value taken, every other token in order
    CHECK(extract_options("auto;env:1;eyeres:100;vrn:1", rest, o, why) && rest == "auto;env:1;vrn:1" && o.pct == 100 && o.any_token);
    CHECK(extract_options("auto;eyeres=150", rest, o, why) && rest == "auto" && o.pct == 150);
    CHECK(extract_options("auto;eyeres:0;env:1", rest, o, why) && rest == "auto;env:1" && o.pct == 0 && o.any_token);
    CHECK(extract_options("auto;eyeres:50", rest, o, why) && o.pct == 50);
    CHECK(extract_options("auto;eyeres:200", rest, o, why) && o.pct == 200);
    // the stage id is never an option
    CHECK(extract_options("eyeres:100;env:1", rest, o, why) && rest == "eyeres:100;env:1" && o.pct == 0 && !o.any_token);
    // other keys sharing the prefix stay
    CHECK(extract_options("auto;eyeresx:1;eye:1", rest, o, why) && rest == "auto;eyeresx:1;eye:1" && !o.any_token);
    // bad values: refused, removed, off
    for (const char* bad : {"auto;eyeres:49", "auto;eyeres:201", "auto;eyeres:", "auto;eyeres:1x0", "auto;eyeres:-1", "auto;eyeres:1000", "auto;eyeres:on"}) {
        why.clear();
        CHECK(!extract_options(bad, rest, o, why) && rest == "auto" && o.pct == 0 && o.any_token && !why.empty());
    }
    // last token wins (like the other sub-options)
    CHECK(extract_options("auto;eyeres:100;eyeres:120", rest, o, why) && o.pct == 120 && rest == "auto");
    CHECK(describe(o) == "eyeres=120");
    // the vrn preset neither adds nor drops it
    std::string ex; bool applied = false;
    CHECK(acz::preset::expand("auto;eyeres:100;vrn:1", ex, applied, why) && applied && ex.find("eyeres:100") != std::string::npos);
    CHECK(acz::preset::expand(user, ex, applied, why) && ex.find("eyeres") == std::string::npos);
    // the user's value + the token stays far inside the 1023-character capacity (runtime_config.h D-400)
    CHECK((user + ";eyeres:100").size() < 255);
}

void test_extent() {
    // test 8: Bigscreen Beyond via SteamVR, recommended 2688x2688, runtime max 8192
    Size s = recommended_extent(2688, 2688, 8192, 8192, 100);
    CHECK(s.w == 2688 && s.h == 2688 && std::string(s.clamp) == "none");
    s = recommended_extent(2688, 2688, 8192, 8192, 150);
    CHECK(s.w == 4032 && s.h == 4032 && std::string(s.clamp) == "none");         // 16.26 Mpx, inside the 16.78 Mpx budget
    s = recommended_extent(2688, 2688, 8192, 8192, 200);
    CHECK(s.w == 4096 && s.h == 4096 && std::string(s.clamp) == "budget");       // 5376^2 over budget -> exactly 4096^2
    CHECK(uint64_t(s.w) * s.h <= kPixelBudget);
    s = recommended_extent(2688, 2688, 8192, 8192, 75);
    CHECK(s.w == 2016 && s.h == 2016);
    // 1512 * 1.25 = 1890, 1680 * 1.25 = 2100
    s = recommended_extent(1512, 1680, 8192, 8192, 125);
    CHECK(s.w == 1890 && s.h == 2100);
    s = recommended_extent(1001, 1001, 8192, 8192, 50);   // 500.5 -> 501
    CHECK(s.w == 501 && s.h == 501);
    // runtime max: aspect kept, the tighter axis decides
    s = recommended_extent(2688, 2688, 4096, 3000, 150);
    CHECK(s.w == 3000 && s.h == 3000 && std::string(s.clamp) == "max");
    s = recommended_extent(2000, 1000, 2500, 8192, 200);  // 4000x2000 -> 2500x1250
    CHECK(s.w == 2500 && s.h == 1250 && std::string(s.clamp) == "max");
    // budget with a non-square view keeps the aspect within a pixel
    s = recommended_extent(4000, 3000, 8192, 8192, 200);  // 8000x6000 = 48 Mpx -> about 4730x3547
    CHECK(uint64_t(s.w) * s.h <= kPixelBudget && near_eq(double(s.w) / double(s.h), 4.0 / 3.0, 2e-3) && std::string(s.clamp) == "budget");
    // a custom (smaller) budget
    s = recommended_extent(2688, 2688, 8192, 8192, 100, 2048ull * 2048ull);
    CHECK(s.w == 2048 && s.h == 2048);
    // refusals: zero inputs, pct out of range (the caller keeps the old policy)
    CHECK(recommended_extent(0, 2688, 8192, 8192, 100).w == 0);
    CHECK(recommended_extent(2688, 0, 8192, 8192, 100).w == 0);
    CHECK(recommended_extent(2688, 2688, 0, 8192, 100).w == 0);
    CHECK(recommended_extent(2688, 2688, 8192, 8192, 0).w == 0);
    CHECK(recommended_extent(2688, 2688, 8192, 8192, 49).w == 0);
    CHECK(recommended_extent(2688, 2688, 8192, 8192, 201).w == 0);
    CHECK(recommended_extent(2688, 2688, 8192, 8192, 100, 0).w == 0);
}

void test_density_test8() {
    // headset test 8 eye 0 FOV (radians, xr-stereo.log layer-fov: -54.95, 42.12, 43.88, -53.21 deg)
    const double d2r = 3.14159265358979323846 / 180.0;
    const double l = -54.95 * d2r, r = 42.12 * d2r, u = 43.88 * d2r, dn = -53.21 * d2r;
    const double ox = px_per_tan(2400, l, r), oy = px_per_tan(1350, dn, u);
    CHECK(near_eq(ox, 1030.1, 0.5) && near_eq(oy, 587.3, 0.5));                     // the logged eye0-px-per-tan=1030.1x587.3
    const double nx = px_per_tan(2688, l, r), ny = px_per_tan(2688, dn, u);
    CHECK(near_eq(nx, 1153.7, 0.6) && near_eq(ny, 1169.4, 0.6));                     // isotropic within 1.4 %
    CHECK(ny / oy > 1.98 && nx / ox > 1.11);                                         // 2x vertical, +12 % horizontal
    CHECK(near_eq(mpix(2688, 2688) / mpix(2400, 1350), 2.23, 0.01));
    // the game's own world window: 2560x1440 over the lens tan 0.61517 / 0.34604 (continuous-first lens-tan) = 2081 px/tan both axes
    CHECK(near_eq(2560.0 / (2 * 0.61517), 2080.7, 0.5) && near_eq(1440.0 / (2 * 0.34604), 2080.7, 0.5));
    // the producer eye targets (2560x1440 over the same HMD FOV): 1099 x 626 px/tan, the second-largest loss after the swapchain
    CHECK(near_eq(px_per_tan(2560, l, r), 1098.8, 0.6) && near_eq(px_per_tan(1440, dn, u), 626.5, 0.6));
    CHECK(px_per_tan(100, 0.5, 0.5) == 0.0 && px_per_tan(100, 0.5, 0.1) == 0.0);
}

void test_bytes() {
    const Bytes o = eye_bytes(2400, 1350, 2400, 1350, 3), n = eye_bytes(2688, 2688, 2688, 2688, 3);
    CHECK((o.swapchains >> 20) == 74 && (n.swapchains >> 20) == 165);
    CHECK((o.props >> 20) == 123 && (n.props >> 20) == 275);
    CHECK((o.depth >> 20) == 12 && (n.depth >> 20) == 27);
    const Bytes b = eye_bytes(4096, 4096, 4096, 4096, 3);
    CHECK((b.swapchains >> 20) == 384 && (b.props >> 20) == 640 && (b.depth >> 20) == 64);
    const Bytes a = eye_bytes(100, 50, 200, 40, 2);   // depth sized to the larger extent per axis
    CHECK(a.depth == 200ull * 50ull * 4ull && a.swapchains == (5000ull + 8000ull) * 8ull);
}

}  // namespace

// WORLD-WINDOW FILTER (eye_res.h filter_select / filter_offset; the HLSL world_rgb mirrors them).
// 1-D shimmer model: a white-noise row of world texels, an eye pixel minifying r texels, sampled by `g` equal bilinear taps over its footprint
// (filter_offset), shifted in 1/16-pixel steps (head motion); the mean std dev of the pixel over the shifts is the shimmer.
double shimmer_1d(float r, uint32_t g) {
    static double tex[4096]; static bool init = false;
    if (!init) { uint32_t x = 12345u; for (double& t : tex) { x = x * 1664525u + 1013904223u; t = double(x >> 8) / double(1u << 24); } init = true; }
    auto bil = [&](double p) { const double q = p - 0.5, i = std::floor(q), f = q - i; const int k = int(i);
        return tex[unsigned(k) % 4096u] * (1.0 - f) + tex[unsigned(k + 1) % 4096u] * f; };
    FilterGrid fg; fg.gu = g; fg.gv = 1;
    const float ju[2] = {r, 0.0f}, jv[2] = {0.0f, 0.0f};   // offsets in texels here (the selection uses uv x size; same thing)
    double tot = 0.0;
    for (int x = 0; x < 200; ++x) {
        double v[16], m = 0.0;
        for (int s = 0; s < 16; ++s) {
            const double c = (x + 0.5 + s / 16.0) * r + 100.0;
            double acc = 0.0;
            for (uint32_t i = 0; i < g; ++i) { float o[2]; filter_offset(fg, i, 0, ju, jv, o); acc += bil(c + o[0]); }
            v[s] = acc / g; m += v[s];
        }
        m /= 16.0; double var = 0.0; for (double t : v) var += (t - m) * (t - m);
        tot += std::sqrt(var / 16.0);
    }
    return tot / 200.0;
}
void test_filter() {
    // grid threshold (1x1 at or below 1.25 texels, else 2x2)
    CHECK(filter_grid(0.5f) == 1 && filter_grid(1.0f) == 1 && filter_grid(1.25f) == 1 && filter_grid(1.26f) == 2 && filter_grid(3.5f) == 2 && filter_grid(100.0f) == 2);
    CHECK(filter_grid(std::nanf("")) == 1 && filter_grid(-3.0f) == 1 && filter_grid(INFINITY) == 2 && kFilterMaxTaps == 4);
    // footprint length in texels (uv per eye pixel x world size)
    CHECK(near_eq(footprint_texels(3.0f / 2560.0f, 0.0f, 2560, 1440), 3.0, 1e-5) && near_eq(footprint_texels(3.0f / 2560.0f, 4.0f / 1440.0f, 2560, 1440), 5.0, 1e-5));
    // test 8 at the centre: window 2081 px/tan (lens tan 0.61517 + 0.61517 over 2560 px), eye tan span 2.330 x 2.299
    const double win_span_x = 2560.0 / 2081.0, win_span_y = 1440.0 / 2081.0;
    const float old_x = centre_ratio(2560, win_span_x, 2400, 2.330), old_y = centre_ratio(1440, win_span_y, 1350, 2.299);
    const float new_x = centre_ratio(2560, win_span_x, 2688, 2.330), new_y = centre_ratio(1440, win_span_y, 2688, 2.299);
    CHECK(near_eq(old_x, 2.02, 0.02) && near_eq(old_y, 3.54, 0.03) && near_eq(new_x, 1.80, 0.02) && near_eq(new_y, 1.78, 0.02));
    CHECK(filter_grid(new_x) == 2 && filter_grid(new_y) == 2 && filter_grid(old_y) == 2);   // eyeres:100 -> 4 taps
    // eyeres:200 on a 4096 eye: 1.18 x 1.17 texels per pixel -> the old single tap (no blur when the eye out-resolves the window)
    CHECK(filter_grid(centre_ratio(2560, win_span_x, 4096, 2.330)) == 1 && filter_grid(centre_ratio(1440, win_span_y, 4096, 2.299)) == 1);
    CHECK(centre_ratio(2560, 0.0, 2688, 2.3) == 0.0f && centre_ratio(2560, 1.0, 0, 2.3) == 0.0f);
    // selection from the Jacobian columns (uv per eye pixel): the larger axis decides, both axes get the grid
    const float ju[2] = {1.0f / 2560.0f, 0.0f}, jv[2] = {0.0f, 3.5f / 1440.0f};
    const FilterGrid g = filter_select(ju, jv, 2560, 1440);
    CHECK(g.gu == 2 && g.gv == 2 && g.taps() == 4);
    const float mag[2] = {0.5f / 2560.0f, 0.0f}, magv[2] = {0.0f, 1.2f / 1440.0f};
    CHECK(filter_select(mag, magv, 2560, 1440).taps() == 1);   // magnified / about 1:1: the old single tap
    const float nanj[2] = {std::nanf(""), 0.0f};
    CHECK(filter_select(nanj, nanj, 2560, 1440).taps() == 1);
    // offsets: a 1x1 grid is the old tap (zero offset); the 2x2 grid is +-(ju + jv)/4, +-(ju - jv)/4 (the shader's o1 / o2)
    { FilterGrid one; float o[2]; filter_offset(one, 0, 0, ju, jv, o); CHECK(o[0] == 0.0f && o[1] == 0.0f); }
    {
        const float a[2] = {0.004f, 0.001f}, b[2] = {-0.0005f, 0.003f};
        FilterGrid fg; fg.gu = fg.gv = 2; double su = 0.0, sv = 0.0; unsigned matched = 0;
        const float o1[2] = {0.25f * (a[0] + b[0]), 0.25f * (a[1] + b[1])}, o2[2] = {0.25f * (a[0] - b[0]), 0.25f * (a[1] - b[1])};
        for (uint32_t j = 0; j < 2; ++j) for (uint32_t i = 0; i < 2; ++i) {
            float o[2]; filter_offset(fg, i, j, a, b, o); su += o[0]; sv += o[1];
            for (int s = -1; s <= 1; s += 2) {
                if (near_eq(o[0], s * o1[0], 1e-9) && near_eq(o[1], s * o1[1], 1e-9)) ++matched;
                if (near_eq(o[0], s * o2[0], 1e-9) && near_eq(o[1], s * o2[1], 1e-9)) ++matched;
            }
        }
        CHECK(near_eq(su, 0.0, 1e-9) && near_eq(sv, 0.0, 1e-9) && matched == 4);
    }
    // shimmer model: the 2x2 grid cuts the shimmer at both test-8 ratios; r = 1: exactly the old tap
    const double s18_1 = shimmer_1d(1.8f, 1), s18_g = shimmer_1d(1.8f, filter_grid(1.8f));
    const double s35_1 = shimmer_1d(3.54f, 1), s35_g = shimmer_1d(3.54f, filter_grid(3.54f)), s35_3 = shimmer_1d(3.54f, 3);
    std::printf("shimmer model: r=1.8 1 tap %.4f, 2 taps %.4f; r=3.54 1 tap %.4f, 2 taps %.4f (3 taps %.4f, not used)\n", s18_1, s18_g, s35_1, s35_g, s35_3);
    CHECK(s18_g < 0.75 * s18_1 && s35_g < 0.65 * s35_1);
    CHECK(near_eq(shimmer_1d(1.0f, 1), shimmer_1d(1.0f, filter_grid(1.0f)), 0.0));
}

// ---- EYE-RING (eye_ring.h, eye-resolution.md 5.1 (a)): the eyeobj option, the request/extent channel, the ring decision and its fallbacks, the
// viewport/scissor substitution math and the memory / effect-cap budgets at the user's 2688x2688.
namespace er = acz::eyering;

void test_eyeobj_option() {
    std::string rest, why; Options o;
    CHECK(extract_options("auto;eyeres:100;vrn:1", rest, o, why) && o.objects && !o.any_obj_token);   // default on
    CHECK(describe_objects(o) == ";eyeobj=1");
    CHECK(extract_options("auto;eyeres:100;eyeobj:0;vrn:1", rest, o, why) && rest == "auto;vrn:1" && o.pct == 100 && !o.objects && o.any_obj_token);
    CHECK(describe_objects(o) == ";eyeobj=0");
    CHECK(extract_options("auto;eyeobj=1;eyeres:120", rest, o, why) && rest == "auto" && o.pct == 120 && o.objects && o.any_obj_token);
    CHECK(extract_options("auto;eyeobj:0", rest, o, why) && rest == "auto" && o.pct == 0 && !o.objects);
    CHECK(describe_objects(o) == ";eyeobj=0(inactive:eyeres-off)");
    CHECK(extract_options("auto;env:1", rest, o, why) && describe_objects(o).empty());   // eyeres off, no eyeobj: no trailing token (the old line)
    for (const char* bad : {"auto;eyeres:100;eyeobj:2", "auto;eyeres:100;eyeobj:", "auto;eyeres:100;eyeobj:on"}) {
        why.clear();
        CHECK(!extract_options(bad, rest, o, why) && rest == "auto" && o.pct == 100 && !o.objects && !why.empty());   // removed; producer part off
    }
    CHECK(extract_options("eyeobj:0;env:1", rest, o, why) && rest == "eyeobj:0;env:1" && o.objects && !o.any_obj_token);   // the stage id is no option
    CHECK(extract_options("auto;eyeobjx:0", rest, o, why) && rest == "auto;eyeobjx:0" && !o.any_obj_token);
    const std::string user = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;vrn:1";
    CHECK((user + ";eyeres:100;eyeobj:0").size() < 255);   // the fallback value still fits the 255 playbook budget (capacity 1023)
}

void test_eyering_channel() {
    er::reset_for_tests();
    er::Channel c = er::read();
    CHECK(!c.requested && !c.objects && !c.extent_known);
    er::Choice ch = er::choose(c, 2560, 1440, 8, 8);
    CHECK(!ch.eye && ch.w == 2560 && ch.h == 1440 && std::string(ch.why) == "off");   // eyeres off: the old ring, never logged
    er::publish_extent(2688, 2688);                                                   // an extent without a request (cannot happen: Output publishes only with eyeres)
    CHECK(std::string(er::choose(er::read(), 2560, 1440, 8, 8).why) == "off");
    er::reset_for_tests();
    er::request(true, true);
    c = er::read();
    CHECK(c.requested && c.objects && !c.extent_known);
    ch = er::choose(c, 2560, 1440, 8, 8);
    CHECK(!ch.eye && ch.w == 2560 && std::string(ch.why) == "no-eye-extent");         // ring before the XR swapchains (no vsearly)
    er::publish_extent(2688, 2688);
    c = er::read();
    CHECK(c.extent_known && c.w == 2688 && c.h == 2688);
    ch = er::choose(c, 2560, 1440, 8, 8);
    CHECK(ch.eye && ch.w == 2688 && ch.h == 2688 && std::string(ch.why) == "eye");
    CHECK(std::string(er::choose(c, 2688, 2688, 8, 8).why) == "eye-equals-game" && !er::choose(c, 2688, 2688, 8, 8).eye);
    CHECK(std::string(er::choose(c, 0, 1440, 8, 8).why) == "game-size-unknown");
    // the fallback: eyeres:100;eyeobj:0
    er::request(true, false);
    ch = er::choose(er::read(), 2560, 1440, 8, 8);
    CHECK(!ch.eye && ch.w == 2560 && ch.h == 1440 && std::string(ch.why) == "eyeobj-off");
    er::request(true, true);
    er::publish_extent(5000, 5000);
    CHECK(std::string(er::choose(er::read(), 2560, 1440, 8, 8).why) == "eye-size-refused");
    er::publish_extent(4096, 4096);
    CHECK(er::choose(er::read(), 2560, 1440, 8, 8).eye);                                // at the pixel budget, FP16 + D32S8X24: 128 MiB each, inside
    CHECK(std::string(er::choose(er::read(), 2560, 1440, 16, 8).why) == "eye-over-image-cap");   // a 16 B/px RT at 4096^2 is over the cap
    er::publish_extent(0, 10);   // refused at the source: the earlier extent stays
    CHECK(er::read().w == 4096);
    er::reset_for_tests();
}

void test_eyering_viewport_scissor() {
    // the user's case: game 2560x1440 (full viewport and scissor) -> eye 2688x2688
    er::Viewport v = er::scale_viewport({0, 0, 2560, 1440, 0, 1}, 2560, 1440, 2688, 2688);
    CHECK(v.x == 0.0f && v.y == 0.0f && near_eq(v.w, 2688.0, 1e-3) && near_eq(v.h, 2688.0, 1e-3) && v.min_depth == 0.0f && v.max_depth == 1.0f);
    er::Rect r = er::scale_scissor({0, 0, 2560, 1440}, 2560, 1440, 2688, 2688);
    CHECK(r.l == 0 && r.t == 0 && r.r == 2688 && r.b == 2688);
    // identity (eye == game): exactly the game's values
    v = er::scale_viewport({12.5f, 7.0f, 640, 360, 0.25f, 0.75f}, 1280, 720, 1280, 720);
    CHECK(v.x == 12.5f && v.y == 7.0f && v.w == 640.0f && v.h == 360.0f && v.min_depth == 0.25f && v.max_depth == 0.75f);
    r = er::scale_scissor({3, 4, 500, 600}, 1280, 720, 1280, 720);
    CHECK(r.l == 3 && r.t == 4 && r.r == 500 && r.b == 600);
    // a sub-rect viewport: NDC lands on the same RELATIVE place of the eye image (what the full-rect consumer samples)
    const er::Viewport g{100, 50, 1280, 720, 0, 1};
    v = er::scale_viewport(g, 2560, 1440, 2688, 2688);
    CHECK(near_eq(v.x, 105.0, 1e-3) && near_eq(v.y, 93.3333, 1e-3) && near_eq(v.w, 1344.0, 1e-3) && near_eq(v.h, 1344.0, 1e-3));
    for (double ndc : {-1.0, -0.37, 0.0, 0.5, 1.0}) {
        const double gx = g.x + (ndc + 1.0) * 0.5 * g.w, gy = g.y + (1.0 - ndc) * 0.5 * g.h;
        const double ex = v.x + (ndc + 1.0) * 0.5 * v.w, ey = v.y + (1.0 - ndc) * 0.5 * v.h;
        CHECK(near_eq(gx / 2560.0, ex / 2688.0, 1e-6) && near_eq(gy / 1440.0, ey / 2688.0, 1e-6));
    }
    // a scissor smaller than the viewport, rounded OUTWARD: l,t floored, r,b ceiled
    r = er::scale_scissor({10, 20, 1000, 700}, 2560, 1440, 2688, 2688);
    CHECK(r.l == 10 && r.t == 37 && r.r == 1050 && r.b == 1307);   // 10.5 -> 10, 37.33 -> 37, 1050.0 -> 1050, 1306.67 -> 1307
    // odd sizes: the full rect maps exactly onto the full eye
    r = er::scale_scissor({0, 0, 1279, 719}, 1279, 719, 1001, 1003);
    CHECK(r.l == 0 && r.t == 0 && r.r == 1001 && r.b == 1003);
    v = er::scale_viewport({0, 0, 1279, 719, 0, 1}, 1279, 719, 1001, 1003);
    CHECK(near_eq(v.w, 1001.0, 1e-3) && near_eq(v.h, 1003.0, 1e-3));
    // the outward property on a grid of rects (the continuous image of the game rect is always inside the integer eye rect)
    unsigned n = 0;
    for (int l = 0; l < 1279; l += 97) for (int t = 0; t < 719; t += 71) for (int w = 1; l + w <= 1279; w += 211) for (int h = 1; t + h <= 719; h += 157) {
        const er::Rect s = er::scale_scissor({l, t, l + w, t + h}, 1279, 719, 1001, 1003);
        const double el = l * 1001.0 / 1279.0, et = t * 1003.0 / 719.0, eR = (l + w) * 1001.0 / 1279.0, eB = (t + h) * 1003.0 / 719.0;
        CHECK(s.l <= el + 1e-9 && s.t <= et + 1e-9 && s.r >= eR - 1e-9 && s.b >= eB - 1e-9);   // covers
        CHECK(el - s.l < 1.0 && et - s.t < 1.0 && s.r - eR < 1.0 && s.b - eB < 1.0);           // by less than one pixel
        CHECK(s.l >= 0 && s.t >= 0 && s.r <= 1001 && s.b <= 1003 && s.l < s.r && s.t < s.b);
        ++n;
    }
    CHECK(n > 100);
    // clamps: an 'infinite' game scissor, a negative origin, an empty rect stays empty, a degenerate game size is a no-op
    r = er::scale_scissor({-50, -10, 16384, 16384}, 2560, 1440, 2688, 2688);
    CHECK(r.l == 0 && r.t == 0 && r.r == 2688 && r.b == 2688);
    r = er::scale_scissor({300, 200, 300, 200}, 2560, 1440, 2688, 2688);
    CHECK(r.l == r.r && r.t == r.b);
    r = er::scale_scissor({300, 200, 100, 100}, 2560, 1440, 2688, 2688);
    CHECK(r.l == r.r && r.t == r.b);
    r = er::scale_scissor({1, 2, 3, 4}, 0, 1440, 2688, 2688);
    CHECK(r.l == 0 && r.r == 0);
    // the RS state must be restorable exactly: one viewport, one scissor, tracked
    CHECK(er::rs_restorable(true, false, 1, 1) == nullptr);
    CHECK(std::string(er::rs_restorable(false, false, 1, 1)) == "eye-ring-rs-unknown");
    CHECK(std::string(er::rs_restorable(true, true, 1, 1)) == "eye-ring-rs-unknown");
    CHECK(std::string(er::rs_restorable(true, false, 2, 1)) == "eye-ring-viewport-count");
    CHECK(std::string(er::rs_restorable(true, false, 1, 0)) == "eye-ring-scissor-count");
    // no constant term: the game's position path is GS units -> NDC (+400 = 512, -448, 2048, 2048) -> viewport. The same GS point gives the same
    // relative eye position at any target size (the shader never sees the target size).
    const double gs_x = 2048.0 + 117.0, ndc = (gs_x - 2048.0) * 2.0 / 512.0;
    for (uint32_t ew : {2560u, 2688u, 1001u, 4096u}) {
        const er::Viewport e = er::scale_viewport({0, 0, 2560, 1440, 0, 1}, 2560, 1440, ew, ew);
        CHECK(near_eq((e.x + (ndc + 1.0) * 0.5 * e.w) / ew, (ndc + 1.0) * 0.5, 1e-6));
    }
}

void test_eyering_memory() {
    // eye-resolution.md 5.1 (f): FP16 ring RT (8 B) + D32_S8X24 DS (8 B), 3 slots x 2 eyes
    const uint64_t game = er::ring_bytes(2560, 1440, 8, 8, 3), eye = er::ring_bytes(2688, 2688, 8, 8, 3);
    CHECK(game == 353894400ull && (game >> 20) == 337);
    CHECK(eye == 693633024ull && (eye >> 20) == 661);
    CHECK(((eye - game) >> 20) == 324);
    const uint64_t kg = er::per_eye_kind_bytes(2560, 1440, 8, 3), ke = er::per_eye_kind_bytes(2688, 2688, 8, 3);
    CHECK((kg >> 20) == 168 && (ke >> 20) == 330);   // per kind (smoke t13, sprite t14): +162 MiB each
    const uint64_t total_extra = (eye - game) + 2 * (ke - kg);
    std::printf("eye-ring memory at 2688x2688: ring %llu MiB (game %llu), smoke/sprite %llu MiB each (game %llu), extra %llu MiB\n",
        (unsigned long long)(eye >> 20), (unsigned long long)(game >> 20), (unsigned long long)(ke >> 20), (unsigned long long)(kg >> 20),
        (unsigned long long)(total_extra >> 20));
    CHECK((total_extra >> 20) == 648);
    // effect caps: the game-size caps refuse a 2688^2 eye image (55 MiB > 32 MiB); the eye caps accept it and the 4096^2 budget
    uint64_t charged = 0;
    CHECK(std::string(acz::fxsmoke::gpu::budget(8, 2688, 2688, 3, charged)) == "smoke-over-image-cap");
    CHECK(std::string(acz::fxsprite::gpu::budget(8, 2688, 2688, 3, charged)) == "sprite-over-image-cap");
    CHECK(acz::fxsmoke::gpu::budget(8, 2560, 1440, 3, charged) == nullptr);   // the game size: unchanged
    CHECK(acz::fxsmoke::gpu::budget(8, 2688, 2688, 3, charged, acz::fxsmoke::gpu::kEyeImageCap, acz::fxsmoke::gpu::kEyeTotalCap) == nullptr &&
          charged == 347996160ull);
    CHECK(acz::fxsprite::gpu::budget(8, 2688, 2688, 3, charged, acz::fxsprite::gpu::kEyeImageCap, acz::fxsprite::gpu::kEyeTotalCap) == nullptr &&
          charged == 351141888ull);
    CHECK(acz::fxsmoke::gpu::budget(8, 4096, 4096, 3, charged, acz::fxsmoke::gpu::kEyeImageCap, acz::fxsmoke::gpu::kEyeTotalCap) == nullptr);
    CHECK(acz::fxsprite::gpu::budget(8, 4096, 4096, 3, charged, acz::fxsprite::gpu::kEyeImageCap, acz::fxsprite::gpu::kEyeTotalCap) == nullptr);
    CHECK(acz::fxsmoke::gpu::budget(8, 4097, 4096, 3, charged, acz::fxsmoke::gpu::kEyeImageCap, acz::fxsmoke::gpu::kEyeTotalCap) != nullptr);
}

int main() {
    test_option();
    test_extent();
    test_density_test8();
    test_bytes();
    test_filter();
    test_eyeobj_option();
    test_eyering_channel();
    test_eyering_viewport_scissor();
    test_eyering_memory();
    std::printf("eye_res_offline: %u checks passed\n", g_checks);
    return 0;
}
