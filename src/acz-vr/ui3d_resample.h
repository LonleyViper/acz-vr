#pragma once
// UI3D resample (2026-10-30b, docs/world-render/ui-3d.md section 11): the CPU mirror of the filter in xr_brief3d.hlsl (ps_table, b0 bits 16..17 and
// 24..31) and the root-constant bit packing. Pure; the HLSL must keep the same arithmetic (ctest ui3d_resample_offline checks the mirror and that
// the shader source still carries the same weights).
//
// Why: the producer's eye image is 1440 x 1440 (kEyeW/kEyeH) and the eye swapchain is 2400 x 1350 (xr-stereo.log `swapchains;eye0=2400x1350`,
// policy source-aspect-width-min): the table blit UPSCALES by 1.67 horizontally (0.94 vertically). The old blit sampled that with the hardware
// bilinear filter (the whole panel text looked blurred; the user: "very blurry"). The game's glyphs were already minified once into the
// 1440 image, so the cure is the best reconstruction at the upscale, not more samples:
//   filter 1  Catmull-Rom (a = -0.5 cubic convolution, 4 x 4 point taps): interpolating (exact at texel centres), C1, sharper than bilinear
//             (measured by the offline test at the real 1.667x scale: 10-90 % step edge 1.3 -> 1.2 output pixels, a 1-texel line keeps more
//             of its peak), small overshoot (~7 %, clamped by the saturate of the shader). A modest gain on its own.
//   filter 2  Catmull-Rom + unsharp mask using the old bilinear as the low-pass: out = CR + k (CR - bilinear), k = u3k (default 0.35).
//             Restores the glyph contrast the two resamples (game -> 1440 -> 2400) took away; one extra bilinear tap. The unsharp term is what
//             visibly crisps thin strokes (text, the 1-2 texel frame lines); the edge width alone barely changes (1.2 -> 1.1 px).
// Rejected: mipmapped / anisotropic sampling (the slot has one mip and the minification already happened in the game's replayed draws);
// Lanczos-3 (6 x 6 taps, ringing visible on the 1-pixel yellow frame lines for no gain at a 1.67x upscale); a bigger eye image (memory: the
// slots are the commit limit's biggest consumers, 192 MiB at 1440 x 1440 x 3 slots x 2 eyes).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace acz::ui3d::resample {

inline constexpr unsigned kFilterBilinear = 0, kFilterCatmullRom = 1, kFilterCatmullRomSharp = 2;

// b0 of the table blit: bits 0..7 mode, 8..15 pause floor (ui3d_pause.h blit_mode_bits), 16..17 filter, 24..31 sharpen amount x 255.
inline uint32_t filter_bits(unsigned filter, float sharpen) noexcept {
    if (filter == kFilterBilinear || filter > kFilterCatmullRomSharp) return 0u;
    uint32_t amount = 0;
    if (filter == kFilterCatmullRomSharp) {
        const float k = std::isfinite(sharpen) ? std::clamp(sharpen, 0.0f, 1.0f) : 0.0f;
        amount = uint32_t(std::lround(double(k) * 255.0));
    }
    return (uint32_t(filter) << 16) | (amount << 24);
}
inline unsigned bits_filter(uint32_t b0) noexcept { return (b0 >> 16) & 3u; }
inline float bits_sharpen(uint32_t b0) noexcept { return float((b0 >> 24) & 0xffu) / 255.0f; }

// Catmull-Rom weights for the four taps at offsets -1, 0, +1, +2 around floor(p); t = frac(p). Sum = 1; w(t=0) = (0,1,0,0); w(t=1) = (0,0,1,0).
inline void catmull_rom(double t, double w[4]) noexcept {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}

// A single-channel image (row-major, top row first) sampled like the shader does: normalised uv, texel centres at (i + 0.5) / size, clamp to edge.
struct Image {
    int w = 0, h = 0;
    std::vector<double> v;
    double at(int x, int y) const noexcept { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return v[size_t(y) * size_t(w) + size_t(x)]; }
};
inline double bilinear(const Image& im, double u, double vv) noexcept {
    const double px = u * im.w - 0.5, py = vv * im.h - 0.5;
    const double fx = std::floor(px), fy = std::floor(py), tx = px - fx, ty = py - fy;
    const int x = int(fx), y = int(fy);
    const double a = im.at(x, y) * (1 - tx) + im.at(x + 1, y) * tx, b = im.at(x, y + 1) * (1 - tx) + im.at(x + 1, y + 1) * tx;
    return a * (1 - ty) + b * ty;
}
inline double bicubic(const Image& im, double u, double vv) noexcept {
    const double px = u * im.w - 0.5, py = vv * im.h - 0.5;
    const double fx = std::floor(px), fy = std::floor(py);
    double wx[4], wy[4];
    catmull_rom(px - fx, wx);
    catmull_rom(py - fy, wy);
    const int x = int(fx), y = int(fy);
    double s = 0;
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) s += wx[i] * wy[j] * im.at(x + i - 1, y + j - 1);
    return s;
}
// The shader's pixel for `filter` / `sharpen` (before the final saturate of the caller).
inline double sample(const Image& im, double u, double vv, unsigned filter, double sharpen) noexcept {
    if (filter == kFilterBilinear) return bilinear(im, u, vv);
    const double cr = bicubic(im, u, vv);
    if (filter == kFilterCatmullRom) return cr;
    return cr + sharpen * (cr - bilinear(im, u, vv));
}

} // namespace acz::ui3d::resample
