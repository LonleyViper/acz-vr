// UI3D resample (2026-10-30b, docs/world-render/ui-3d.md section 11): the CPU mirror of the table blit's filter (xr_brief3d.hlsl), the b0 bit packing, the
// quality claims behind the choice (sharper than the old bilinear at the real 1440 -> 2400 upscale, exact at texel centres, constants preserved), and
// that the shader source still carries the same arithmetic.
#include "ui3d_resample.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace acz::ui3d::resample;

static unsigned g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static Image make(int w, int h, double v) { Image im; im.w = w; im.h = h; im.v.assign(size_t(w) * size_t(h), v); return im; }

static void test_weights() {
    for (double t = 0.0; t <= 1.0; t += 0.125) {
        double w[4]; catmull_rom(t, w);
        CHECK(std::fabs(w[0] + w[1] + w[2] + w[3] - 1.0) < 1e-12);   // partition of unity
    }
    double a[4], b[4]; catmull_rom(0.0, a); catmull_rom(1.0, b);
    CHECK(a[0] == 0 && a[1] == 1 && a[2] == 0 && a[3] == 0);         // interpolating: exact at the texel centre
    CHECK(std::fabs(b[0]) < 1e-15 && std::fabs(b[1]) < 1e-15 && std::fabs(b[2] - 1.0) < 1e-15 && std::fabs(b[3]) < 1e-15);
}

static void test_bits() {
    CHECK(filter_bits(kFilterBilinear, 0.35f) == 0u);                 // filter 0 = the old b0 value, byte for byte
    CHECK(filter_bits(7, 0.5f) == 0u);                                // out of range: off
    CHECK(filter_bits(kFilterCatmullRom, 0.9f) == (1u << 16));        // no unsharp amount without filter 2
    const uint32_t b = filter_bits(kFilterCatmullRomSharp, 0.35f);
    CHECK(bits_filter(b) == 2u && std::fabs(bits_sharpen(b) - 0.35f) < 0.003f);
    CHECK(bits_sharpen(filter_bits(2, 5.0f)) == 1.0f && bits_sharpen(filter_bits(2, -1.0f)) == 0.0f);
    CHECK((b & 0xffffu) == 0u);                                       // never touches the mode (0..7) / pause floor (8..15) bits
    const uint32_t pause = 2u | (77u << 8);
    CHECK(bits_filter(pause | b) == 2u && (pause | b & 0xffffu) == (pause | 0u));
}

static void test_identity_and_constants() {
    Image im = make(8, 6, 0.0);
    for (int y = 0; y < 6; ++y) for (int x = 0; x < 8; ++x) im.v[size_t(y) * 8 + size_t(x)] = 0.1 * x + 0.05 * y * y;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x) {   // at a texel centre every filter returns the texel (the unsharp term is zero: CR == bilinear there)
            const double u = (x + 0.5) / 8.0, v = (y + 0.5) / 6.0;
            const double t = im.v[size_t(y) * 8 + size_t(x)];
            CHECK(std::fabs(sample(im, u, v, kFilterBilinear, 0) - t) < 1e-12 && std::fabs(sample(im, u, v, kFilterCatmullRom, 0) - t) < 1e-12 &&
                  std::fabs(sample(im, u, v, kFilterCatmullRomSharp, 0.35) - t) < 1e-12);
        }
    const Image c = make(5, 5, 0.37);
    for (double u = 0.0; u <= 1.0; u += 0.1)
        for (double v = 0.0; v <= 1.0; v += 0.1)
            CHECK(std::fabs(sample(c, u, v, kFilterCatmullRomSharp, 0.35) - 0.37) < 1e-12);   // no ripple on flat areas (panel fills)
}

// The real case: a 1-texel-wide bright line / a hard step in the 1440-wide eye image, upscaled 2400/1440 = 1.667 into the swapchain.
static void test_sharper_than_bilinear_at_the_real_upscale() {
    const int W = 1440, N = 40;
    Image step = make(N, 1, 0.0);
    for (int x = N / 2; x < N; ++x) step.v[size_t(x)] = 1.0;
    auto edge_width = [&](unsigned f, double k) {   // 10..90 % rise of the step edge in eye-image TEXELS (a fine sub-texel scan; x 1.667 = output pixels)
        double lo = -1, hi = -1;
        for (double x = N / 2 - 3.0; x < N / 2 + 3.0; x += 0.005) {
            const double s = std::clamp(sample(step, x / double(N), 0.5, f, k), 0.0, 1.0);
            if (lo < 0 && s >= 0.1) lo = x;
            if (hi < 0 && s >= 0.9) hi = x;
        }
        return (hi - lo) * 2400.0 / double(W);
    };
    const double wb = edge_width(kFilterBilinear, 0), wc = edge_width(kFilterCatmullRom, 0), ws = edge_width(kFilterCatmullRomSharp, 0.35);
    std::printf("edge 10-90%% width (output px): bilinear %.1f  catmull-rom %.1f  +unsharp %.1f\n", wb, wc, ws);
    CHECK(wc < wb);                // Catmull-Rom is sharper than the old bilinear
    CHECK(ws <= wc);               // the unsharp mask only steepens it further
    // contrast of a thin bright line (the yellow frame lines are 1-2 texels wide): the line's peak after the resample, bilinear loses the most
    Image line = make(N, 1, 0.0); line.v[size_t(N / 2)] = 1.0;
    auto peak = [&](unsigned f, double k) {
        double p = 0;
        for (int ox = 0; ox < int(N * 2400.0 / W); ++ox) p = std::max(p, sample(line, (ox + 0.5) / (N * 2400.0 / W), 0.5, f, k));
        return p;
    };
    CHECK(peak(kFilterBilinear, 0) <= peak(kFilterCatmullRom, 0) + 1e-12);
    CHECK(peak(kFilterCatmullRomSharp, 0.35) >= peak(kFilterCatmullRom, 0));
    // overshoot at a hard step is bounded (the shader saturates): CR rings ~7 %, plus the unsharp ~0.35 x that
    double mx = 0;
    for (int ox = 0; ox < int(N * 2400.0 / W); ++ox) mx = std::max(mx, sample(step, (ox + 0.5) / (N * 2400.0 / W), 0.5, kFilterCatmullRomSharp, 0.35));
    CHECK(mx > 1.0 && mx < 1.15);
}

static void test_shader_source() {
    std::string path = __FILE__;   // tests/acz-vr/<this> -> src/acz-vr/xr_brief3d.hlsl
    for (int i = 0; i < 3; ++i) { const size_t p = path.find_last_of("/\\"); if (p == std::string::npos) break; path.resize(p); }
    std::ifstream f(path + "/src/acz-vr/xr_brief3d.hlsl");
    std::stringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    CHECK(!s.empty());
    CHECK(s.find("w[0] = -0.5 * t3 + t2 - 0.5 * t;") != std::string::npos);
    CHECK(s.find("w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;") != std::string::npos);
    CHECK(s.find("w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;") != std::string::npos);
    CHECK(s.find("w[3] = 0.5 * t3 - 0.5 * t2;") != std::string::npos);
    CHECK(s.find("(opaque >> 16) & 3u") != std::string::npos && s.find("(opaque >> 24) & 0xffu") != std::string::npos);
    CHECK(s.find("return cr + amount * (cr - bil);") != std::string::npos);
    CHECK(s.find("const float4 c = sample_eye(i.uv);") != std::string::npos);
}

int main() {
    test_weights();
    test_bits();
    test_identity_and_constants();
    test_sharper_than_bilinear_at_the_real_upscale();
    test_shader_source();
    std::printf("ui3d_resample_offline: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
