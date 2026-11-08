// LENS-FLARE (xr_terrain_stage lflare:0|1 (default 1), lflarek:<0..3> (default 1); docs/world-render/lens-flare.md).
// Our own head-look lens flare, drawn by the eye compositor (xr_stereo_blit.hlsl lf_over) over each composited eye pixel. The game's flare follows the
// GAME camera (plane heading; its REV_SUBTRACT quads are skipped in the HUD replay, D-391); this one follows where the HEAD points: the sun direction is
// the stage light P[kPLight] (world space), its position in each eye comes from that eye's own ray map (w_base/w_ux/w_vy) and the eye's stage basis
// (t_right/t_up/t_fwd), the same rays the terrain/sky march uses.
// This header is the CPU mirror of the shader (same constants, same operations, float math) used by the offline tests and the WARP harness.
// Geometry: every direction x is projected onto the eye's tangent plane at unit distance along the optical axis f: p(x) = (x.er, x.eu) / (x.f).
// The flare is a function of the pixel's p and the sun's s = p(sun) only (FOV/aspect independent, ~radians near the axis):
//   core glow + 6-ray starburst + a ring (halo) around s, and 4 ghost discs at s * (1 - t_i) on the line through the lens centre (p = 0).
// Intensity: k (lflarek) x vis (terrain march along the sun ray x replay-depth taps at the sun) x horizon fade (sun elevation) x in-view gate (the sun
// inside this eye's image, smooth over kViewMargin) x angle terms of cos(sun, f): the flare shows only while the sun is in view. The result F is ADDED in display space (the effects convention, sprite_over): out = decode(min(1, encode(under) + F)).
// Memory: no resources; the switch and strength travel in the env texture's unused row-0 texel P[32] (kPFlare), written by build_env_texture.
// Limits: the model cockpit (COCKPIT-2) is drawn AFTER this pass, so it covers the flare where it is opaque; its occlusion of the sun is approximated by
// the replayed cockpit's depth band (the view-space cockpit mask). Clouds do not occlude the sun.
#pragma once

#include <cmath>
#include <cstdint>

namespace acz::lensflare {

// ---- constants (mirrored by xr_stereo_blit.hlsl kLF*)
inline constexpr float kStrengthMax = 3.0f;          // lflarek:<0..3>
inline constexpr float kCoreCos0 = 0.57f;            // core/starburst/ring start (sun ~55 deg off the head's optical axis); below: no flare at all
inline constexpr float kGhostCos0 = 0.71f;           // ghosts start at 45 deg off axis; full toward the centre (squared ramp)
inline constexpr float kViewMargin = 0.1f;           // IN-VIEW gate (user, 2026-10-29): 1 while the sun is inside this eye's image, 0 once it is
                                                     // kViewMargin (image uv) outside it, smooth in between; not on the image plane = 0
inline constexpr float kHorizonLo = -0.02f, kHorizonHi = 0.06f;   // sin(sun elevation): smooth fade at the horizon (below: off)
inline constexpr float kCoreA = 0.55f, kCoreR = 0.035f;           // gaussian core: a * exp(-(r/R)^2)
inline constexpr float kBloomA = 0.16f, kBloomR = 0.12f;          // exponential bloom: a * exp(-r/R)
inline constexpr float kRayA = 0.30f, kRayR = 0.22f, kRayPow = 48.0f;   // 6 rays: a * |cos 3phi|^pow * exp(-r/R)
inline constexpr float kRay2A = 0.12f, kRay2R = 0.10f;                  // 6 thin rays rotated 30 deg: |sin 3phi|^pow, faster falloff
inline constexpr float kRingA = 0.05f, kRingR = 0.40f, kRingW = 0.03f, kRingChroma = 0.025f;   // halo ring (R/B radii x (1 +- chroma))
inline constexpr int kGhosts = 4;
inline constexpr float kGhostT[kGhosts] = {0.55f, 1.35f, 1.75f, 2.30f};   // centre = s * (1 - t)
inline constexpr float kGhostRad[kGhosts] = {0.035f, 0.060f, 0.110f, 0.050f};
inline constexpr float kGhostCol[kGhosts][3] = {{0.25f, 0.45f, 0.35f}, {0.20f, 0.30f, 0.55f}, {0.35f, 0.25f, 0.15f}, {0.30f, 0.45f, 0.25f}};
inline constexpr float kGhostA = 0.10f;
inline constexpr float kCoreTint = 0.5f;             // core colour = lerp(white, stage light tint, kCoreTint)
inline constexpr int kVisSteps = 12;                 // terrain occlusion march samples (quadratic spacing up to the max terrain height)
inline constexpr float kVisSoft0 = 20.0f, kVisSoft1 = 0.02f;   // soft clearance (units): 20 + 2% of the distance (no popping)
inline constexpr float kDepthTap = 0.006f;           // replay depth taps at the sun: centre and +-kDepthTap in u and v (5 taps)
inline constexpr float kMinOut = 1.0f / 1024.0f;     // F below this everywhere: the pixel is returned untouched (and the occlusion is not evaluated)

struct V3 { float x, y, z; };
inline V3 v3(const float a[3]) noexcept { return {a[0], a[1], a[2]}; }
inline float dot(V3 a, V3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 add(V3 a, V3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 mul(V3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline V3 norm(V3 a) noexcept { const float l = std::sqrt(dot(a, a)); return l > 0.0f ? mul(a, 1.0f / l) : a; }
inline float sat(float x) noexcept { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
inline float smooth01(float e0, float e1, float x) noexcept { const float t = sat((x - e0) / (e1 - e0)); return t * t * (3.0f - 2.0f * t); }

// The option values as the env texel P[32] carries them: (k, 0, 0, on). Off (or k == 0): w = 0 and the shader runs no flare code.
inline void param_texel(bool on, float k, float out[4]) noexcept {
    const bool live = on && k > 0.0f && std::isfinite(k);
    out[0] = live ? (k > kStrengthMax ? kStrengthMax : k) : 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = live ? 1.0f : 0.0f;
}

// Per-eye flare frame. ray map d(u,v) = base + u*ux + v*vy (the eye's anchor-frame ray, xr_stereo world composite); stage basis right/up/fwd
// (orthonormal; world ray = right*d.x + up*d.y - fwd*d.z). L = the stage light direction (world, any length).
struct Frame {
    bool ok = false;        // false: no flare for this eye (sun behind/too far off axis, below the horizon or degenerate)
    V3 f{}, er{}, eu{};     // optical axis and tangent basis (anchor frame)
    V3 sa{};                // sun direction (anchor frame, unit)
    V3 lw{};                // sun direction (world, unit)
    float cs = 0;           // cos(sun, optical axis)
    float sx = 0, sy = 0;   // sun position on the tangent plane
    float horizon = 0;      // horizon fade
    float inview = 0;       // in-view gate (kViewMargin)
    float su = 0, sv = 0; bool s_uv = false;   // sun position in the eye image (for the depth taps); s_uv false = not on the ray plane
};
inline Frame make_frame(const float base[3], const float ux[3], const float vy[3], const float right[3], const float up[3], const float fwd[3], const float L[3]) noexcept {
    Frame fr;
    const V3 b = v3(base), u = v3(ux), v = v3(vy);
    V3 f = norm(cross(u, v));
    if (dot(f, add(b, add(mul(u, 0.5f), mul(v, 0.5f)))) < 0.0f) f = mul(f, -1.0f);
    const V3 er = norm(add(u, mul(f, -dot(u, f))));
    const V3 eu = cross(f, er);
    const V3 lw = norm(v3(L));
    const V3 sa = {dot(lw, v3(right)), dot(lw, v3(up)), -dot(lw, v3(fwd))};
    fr.f = f; fr.er = er; fr.eu = eu; fr.sa = sa; fr.lw = lw;
    fr.cs = dot(sa, f);
    fr.horizon = smooth01(kHorizonLo, kHorizonHi, lw.y);
    if (!(fr.cs > kCoreCos0) || !(fr.horizon > 0.0f)) return fr;
    fr.sx = dot(sa, er) / fr.cs; fr.sy = dot(sa, eu) / fr.cs;
    // the sun on the image plane: u*ux + v*vy - l*sa = -base (Cramer), l > 0
    const V3 a3 = mul(sa, -1.0f), rb = mul(b, -1.0f);
    const float D = dot(u, cross(v, a3));
    if (std::fabs(D) > 1e-12f) {
        const float x1 = dot(rb, cross(v, a3)) / D, x2 = dot(u, cross(rb, a3)) / D, x3 = dot(u, cross(v, rb)) / D;
        if (x3 > 0.0f) { fr.su = x1; fr.sv = x2; fr.s_uv = true; }
    }
    if (!fr.s_uv) return fr;
    const float out_u = std::fmax(-fr.su, fr.su - 1.0f), out_v = std::fmax(-fr.sv, fr.sv - 1.0f);
    fr.inview = smooth01(kViewMargin, 0.0f, std::fmax(std::fmax(out_u, out_v), 0.0f));
    if (!(fr.inview > 0.0f)) return fr;
    fr.ok = true;
    return fr;
}

// The flare's display-space colour at the eye ray d (anchor frame) BEFORE occlusion and strength (the horizon fade and the in-view gate are included): tint = the stage light colour (any scale; normalised by
// its max channel like the sun disc).
inline void flare_rgb(const Frame& fr, const float d[3], const float tint_in[3], float out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0f;
    if (!fr.ok) return;
    const V3 dv = v3(d);
    const float pd = dot(dv, fr.f);
    if (!(pd > 1e-6f)) return;
    const float px = dot(dv, fr.er) / pd, py = dot(dv, fr.eu) / pd;
    const float m = std::fmax(std::fmax(tint_in[0], tint_in[1]), std::fmax(tint_in[2], 1e-3f));
    const float tint[3] = {tint_in[0] / m, tint_in[1] / m, tint_in[2] / m};
    const float dx = px - fr.sx, dy = py - fr.sy;
    const float r = std::sqrt(dx * dx + dy * dy);
    float ac = sat((fr.cs - kCoreCos0) / (1.0f - kCoreCos0));       // core/rays/ring angle term (linear)
    float ag = sat((fr.cs - kGhostCos0) / (1.0f - kGhostCos0)); ag *= ag; // ghost angle term (squared)
    // core + bloom + rays (white-ish: lerp(1, tint, kCoreTint))
    float core = kCoreA * std::exp(-(r / kCoreR) * (r / kCoreR)) + kBloomA * std::exp(-r / kBloomR);
    if (r > 1e-5f) {
        const float phi = std::atan2(dy, dx);
        const float c3 = std::fabs(std::cos(3.0f * phi)), s3 = std::fabs(std::sin(3.0f * phi));
        core += kRayA * std::pow(c3, kRayPow) * std::exp(-r / kRayR) + kRay2A * std::pow(s3, kRayPow) * std::exp(-r / kRay2R);
    }
    const float gate = fr.horizon * fr.inview;
    ac *= gate; ag *= gate;
    core *= ac;
    // halo ring (chromatic: red outside, blue inside)
    float ring[3];
    for (int c = 0; c < 3; ++c) {
        const float rr = kRingR * (1.0f + kRingChroma * float(1 - c));
        const float q = (r - rr) / kRingW;
        ring[c] = kRingA * ac * std::exp(-q * q);
    }
    // ghosts
    float gh[3] = {0.0f, 0.0f, 0.0f};
    if (ag > 0.0f)
        for (int i = 0; i < kGhosts; ++i) {
            const float gx = fr.sx * (1.0f - kGhostT[i]), gy = fr.sy * (1.0f - kGhostT[i]);
            const float rg = std::sqrt((px - gx) * (px - gx) + (py - gy) * (py - gy));
            const float rad = kGhostRad[i];
            const float disc = smooth01(rad, 0.75f * rad, rg) * (0.6f + 0.4f * sat(rg / rad));   // soft disc, brighter rim
            for (int c = 0; c < 3; ++c) gh[c] += kGhostA * ag * disc * kGhostCol[i][c];
        }
    for (int c = 0; c < 3; ++c) {
        const float ct = 1.0f + (tint[c] - 1.0f) * kCoreTint;
        out[c] = core * ct + ring[c] * tint[c] + gh[c] * tint[c];
    }
}

// Terrain occlusion along the sun ray from the eye: soft clearance minimum over kVisSteps samples up to the stage's max terrain height (t_hmax), capped
// at the march end `far_dist`. height(x, z) = the effective height (sea plane included), e.g. acz::terrain::height_effective.
template <class H>
inline float terrain_vis(const float eye[3], const V3& lw, float hmax, float far_dist, H&& height) noexcept {
    if (!(lw.y > 1e-3f)) return 0.0f;
    float tmax = (hmax - eye[1]) / lw.y;
    if (!(tmax > 0.0f)) return 1.0f;
    tmax = std::fmin(tmax, far_dist);
    float vis = 1.0f;
    for (int k = 0; k < kVisSteps; ++k) {
        const float fk = float(k + 1) / float(kVisSteps);
        const float t = tmax * fk * fk;
        const float qx = eye[0] + t * lw.x, qy = eye[1] + t * lw.y, qz = eye[2] + t * lw.z;
        const float h = height(qx, qz);
        vis = std::fmin(vis, sat(0.5f + (qy - h) / (kVisSoft0 + kVisSoft1 * t)));
    }
    return vis;
}

// Replay-depth occlusion (the cheap view-space cockpit/object mask): 5 taps at the sun's image position; covered(u, v) = the replay depth > 0 there
// (cockpit or object). Not on the image plane / outside the image: 1 (nothing in this eye's image covers it).
template <class C>
inline float depth_vis(const Frame& fr, C&& covered) noexcept {
    if (!fr.s_uv || !(fr.su >= 0.0f && fr.su <= 1.0f && fr.sv >= 0.0f && fr.sv <= 1.0f)) return 1.0f;
    const float o[5][2] = {{0.0f, 0.0f}, {kDepthTap, 0.0f}, {-kDepthTap, 0.0f}, {0.0f, kDepthTap}, {0.0f, -kDepthTap}};
    int n = 0;
    for (const auto& t : o) {
        const float u = sat(fr.su + t[0]), v = sat(fr.sv + t[1]);
        if (covered(u, v)) ++n;
    }
    return 1.0f - float(n) / 5.0f;
}

// The shader's full term (lf_over): F = flare_rgb x k x vis, where vis() (terrain x depth) is evaluated only when max(flare_rgb) x k >= kMinOut;
// below that F = 0 (the pixel stays untouched).
template <class Vis>
inline void eval(const Frame& fr, const float d[3], const float tint[3], float k, Vis&& vis, float F[3]) noexcept {
    flare_rgb(fr, d, tint, F);
    if (!(std::fmax(F[0], std::fmax(F[1], F[2])) * k >= kMinOut)) { F[0] = F[1] = F[2] = 0.0f; return; }
    const float s = k * vis();
    for (int c = 0; c < 3; ++c) F[c] *= s;
}

// Display-space additive composite (exact sRGB, as sprite_over): under = linear [0,1]; F = display-space light.
inline float srgb_enc(float x) noexcept { x = sat(x); return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f; }
inline float srgb_dec(float x) noexcept { x = sat(x); return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f); }
inline void composite(const float under[3], const float F[3], float out[3]) noexcept {
    if (!(std::fmax(F[0], std::fmax(F[1], F[2])) > 0.0f)) { for (int c = 0; c < 3; ++c) out[c] = under[c]; return; }   // shader: `under` untouched
    for (int c = 0; c < 3; ++c) out[c] = srgb_dec(std::fmin(srgb_enc(under[c]) + F[c], 1.0f));
}

// ---- 2026-10-30 diagnostics: WHY a frame shows (or cannot show) the flare, as the shader decides it (pre-occlusion; the GPU terrain/depth occlusion is not mirrored).
// Order = the shader's early-outs: switch/strength (P[32]) -> env light -> moon -> horizon -> angle -> in view. `behind` = the sun is more than ~55 deg off this eye's optical
// axis; `out_of_view` = within the angle but outside the image (+ kViewMargin). active = the pre-occlusion flare term can reach the minimum-output gate for some pixel of this eye (below_min otherwise, a conservative bound).
// W15 F3: below_min = every pixel of this eye is below the shader's minimum-output gate (kMinOut) before occlusion, so the shader returns it untouched.
enum class Why : uint8_t { active = 0, off, no_light, moon, below_horizon, behind, out_of_view, no_env, below_min, count };
inline const char* why_name(Why w) noexcept {
    static const char* const n[] = {"active", "off", "no-light", "moon", "below-horizon", "behind", "out-of-view", "no-env", "below-min-output"};
    return unsigned(w) < unsigned(Why::count) ? n[unsigned(w)] : "?";
}
// W15 F3: a conservative upper bound of max(flare_rgb) per unit of peak_gate for a tint normalised to <= 1 per channel: core+bloom+rays <= 0.55+0.16+0.30+0.12,
// ring <= kRingA, the four ghosts together <= kGhostA x max channel sum of kGhostCol (1.45); every term scales with ac or ag, and ag <= ac (the squared, later ramp).
inline constexpr float kFlareRgbBound = (kCoreA + kBloomA + kRayA + kRay2A) + kRingA + kGhostA * 1.45f;
// The pre-occlusion strength of the flare core in this eye (0..1): angle ramp x horizon fade x in-view gate (what `ac` is in the shader before k and vis).
inline float peak_gate(const Frame& fr) noexcept {
    if (!fr.ok) return 0.0f;
    return sat((fr.cs - kCoreCos0) / (1.0f - kCoreCos0)) * fr.horizon * fr.inview;
}
inline constexpr float kSunMoonSameCos = 0.99f;   // = terrain_stage.h kSunMoonSame (the light IS the moon)
// p32 = env P[32], light = P[1] (xyz dir, w), moon = P[0]; the frame comes from make_frame for the eye.
inline Why classify(const float p32[4], const float light[4], const float moon[4], const Frame& fr) noexcept {
    if (!(p32[3] > 0.0f) || !(p32[0] > 0.0f)) return Why::off;
    if (!(light[3] > 0.0f)) return Why::no_light;
    if (moon[3] > 0.0f && moon[0] * light[0] + moon[1] * light[1] + moon[2] * light[2] > kSunMoonSameCos) return Why::moon;
    if (!(fr.horizon > 0.0f)) return Why::below_horizon;
    if (!(fr.cs > kCoreCos0)) return Why::behind;
    if (!fr.ok) return Why::out_of_view;
    // W15 F3: active means the shader's minimum-output gate (F x k >= kMinOut, before occlusion) can pass for some pixel; a tiny strength (lflarek:0.000001)
    // or a gate at the very edge of the angle/horizon/view ramps cannot, however large the eye, so it is not reported as active.
    if (!(p32[0] * kFlareRgbBound * peak_gate(fr) >= kMinOut)) return Why::below_min;
    return Why::active;
}

} // namespace acz::lensflare
