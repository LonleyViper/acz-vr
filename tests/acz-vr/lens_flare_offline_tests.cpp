// LENS-FLARE offline tests (lens_flare.h = the CPU mirror of xr_stereo_blit.hlsl lf_over; docs/world-render/lens-flare.md): options and the env texel,
// head-look projection of the sun, in-view gate, angle/horizon intensity, ghost line, terrain/depth occlusion, display-space composite. Synthetic data only.
#include "lens_flare.h"
#include "terrain_stage.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace LF = acz::lensflare;
namespace T = acz::terrain;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static const float kPi = 3.14159265358979f;
static float rad(float d) { return d * kPi / 180.0f; }

// A pinhole eye looking along the head direction (yaw about +Y, pitch about the head's right), anchor frame = stage frame (right +X, up +Y, fwd -Z).
// Ray map: d(u, v) = base + u*ux + v*vy with u right, v DOWN (as the eye targets), half-FOV tangents tx, ty.
struct Eye { float base[3], ux[3], vy[3]; };
static void rot(float yaw, float pitch, const float in[3], float out[3]) {   // Ry(yaw) * Rx(pitch)
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const float y1 = in[1] * cp - in[2] * sp, z1 = in[1] * sp + in[2] * cp, x1 = in[0];
    out[0] = x1 * cy + z1 * sy; out[1] = y1; out[2] = -x1 * sy + z1 * cy;
}
static Eye eye(float yaw_deg, float pitch_deg, float tx = 1.0f, float ty = 0.8f) {
    const float b[3] = {-tx, ty, -1.0f}, u[3] = {2.0f * tx, 0.0f, 0.0f}, v[3] = {0.0f, -2.0f * ty, 0.0f};
    Eye e; rot(rad(yaw_deg), rad(pitch_deg), b, e.base); rot(rad(yaw_deg), rad(pitch_deg), u, e.ux); rot(rad(yaw_deg), rad(pitch_deg), v, e.vy);
    return e;
}
static const float kR[3] = {1, 0, 0}, kU[3] = {0, 1, 0}, kF[3] = {0, 0, -1};
static void sun_dir(float heading_deg, float elev_deg, float L[3]) {   // the stage convention dir(a, e) = (-sin a cos e, sin e, -cos a cos e)
    const float a = rad(heading_deg), e = rad(elev_deg);
    L[0] = -std::sin(a) * std::cos(e); L[1] = std::sin(e); L[2] = -std::cos(a) * std::cos(e);
}
static LF::Frame frame(const Eye& e, const float L[3]) { return LF::make_frame(e.base, e.ux, e.vy, kR, kU, kF, L); }
static void ray(const Eye& e, float u, float v, float d[3]) { for (int i = 0; i < 3; ++i) d[i] = e.base[i] + u * e.ux[i] + v * e.vy[i]; }
static float lum(const LF::Frame& fr, const Eye& e, float u, float v) {
    const float tint[3] = {1.0f, 0.95f, 0.85f};
    float d[3], F[3]; ray(e, u, v, d); LF::flare_rgb(fr, d, tint, F);
    return F[0] + F[1] + F[2];
}

int main() {
    // ---- options: the parsed default is ON (k = 1), lflare:0 off, lflarek:<0..3>; a hand-built Conventions{} is OFF
    {
        std::string id, why; T::Conventions c;
        CHECK(!T::Conventions{}.lens_flare);
        CHECK(T::parse_stage_option("m02a;env:1", id, c, why) && c.lens_flare && c.lens_flare_k == 1.0f);
        CHECK(T::parse_stage_option("m02a;env:1;lflare:0", id, c, why) && !c.lens_flare);
        CHECK(T::parse_stage_option("m02a;lflare:1;lflarek:2.5", id, c, why) && c.lens_flare && c.lens_flare_k == 2.5f);
        CHECK(T::parse_stage_option("m02a;lflarek:0", id, c, why) && c.lens_flare_k == 0.0f);
        CHECK(T::parse_stage_option("m02a;lflarek:3", id, c, why) && c.lens_flare_k == 3.0f);
        CHECK(!T::parse_stage_option("m02a;lflarek:3.5", id, c, why));
        CHECK(!T::parse_stage_option("m02a;lflarek:-1", id, c, why));
        CHECK(!T::parse_stage_option("m02a;lflare:2", id, c, why));
        float p[4];
        LF::param_texel(true, 1.0f, p); CHECK(p[0] == 1.0f && p[3] == 1.0f);
        LF::param_texel(true, 0.0f, p); CHECK(p[0] == 0.0f && p[3] == 0.0f);
        LF::param_texel(false, 2.0f, p); CHECK(p[0] == 0.0f && p[3] == 0.0f);
        LF::param_texel(true, 9.0f, p); CHECK(p[0] == LF::kStrengthMax && p[3] == 1.0f);
    }
    // ---- the env texel P[32]: written only when on; every other float of the texture identical
    {
        T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 9000.0f);
        std::vector<float> off, on, k0;
        T::Conventions c0; T::build_env_texture(st, c0, off);
        T::Conventions c1; c1.lens_flare = true; c1.lens_flare_k = 1.5f; T::build_env_texture(st, c1, on);
        T::Conventions c2; c2.lens_flare = true; c2.lens_flare_k = 0.0f; T::build_env_texture(st, c2, k0);
        CHECK(!off.empty() && off.size() == on.size() && off.size() == k0.size());
        const size_t p = size_t(T::kPFlare) * 4;
        CHECK(off[p] == 0.0f && off[p + 3] == 0.0f && on[p] == 1.5f && on[p + 3] == 1.0f && k0[p + 3] == 0.0f);
        size_t diff = 0;
        for (size_t i = 0; i < on.size(); ++i) if (on[i] != off[i] && (i < p || i >= p + 4)) ++diff;
        CHECK(diff == 0);
        CHECK(off == k0);
        CHECK(T::kPFlare < T::kEnvTexW);
    }
    // ---- projection: the sun lands where the HEAD looks (sun at heading 0 = -Z, elevation 20 deg)
    float L[3]; sun_dir(0.0f, 20.0f, L);
    {
        const Eye e = eye(0.0f, 20.0f);   // head looks straight at the sun: centre of the image
        const LF::Frame fr = frame(e, L);
        CHECK(fr.ok && fr.s_uv && std::fabs(fr.su - 0.5f) < 1e-4f && std::fabs(fr.sv - 0.5f) < 1e-4f);
        CHECK(std::fabs(fr.sx) < 1e-4f && std::fabs(fr.sy) < 1e-4f && std::fabs(fr.cs - 1.0f) < 1e-4f && fr.inview == 1.0f);
        CHECK(lum(fr, e, 0.5f, 0.5f) > 1.0f);                          // bright core at the sun
        CHECK(lum(fr, e, 0.5f, 0.5f) > 5.0f * lum(fr, e, 0.9f, 0.85f)); // falls off away from it
        const Eye e2 = eye(0.0f, 0.0f);   // head level: the sun is 20 deg up: v = 0.5 - tan(20)/(2*0.8)
        const LF::Frame f2 = frame(e2, L);
        CHECK(f2.ok && std::fabs(f2.su - 0.5f) < 1e-4f && std::fabs(f2.sv - (0.5f - std::tan(rad(20.0f)) / 1.6f)) < 1e-4f);
        const Eye e3 = eye(-25.0f, 20.0f);   // head turned 25 deg right (yaw negative = toward +X): the sun moves LEFT in the image
        const LF::Frame f3 = frame(e3, L);
        CHECK(f3.ok && f3.su > 0.2f && f3.su < 0.35f && std::fabs(f3.sv - 0.5f) < 0.05f);
        // the peak follows the projected sun position (search a grid)
        float best = -1, bu = 0, bv = 0;
        for (int y = 0; y <= 80; ++y) for (int x = 0; x <= 100; ++x) { const float u = x / 100.0f, v = y / 80.0f, l = lum(f3, e3, u, v); if (l > best) { best = l; bu = u; bv = v; } }
        CHECK(std::fabs(bu - f3.su) < 0.011f && std::fabs(bv - f3.sv) < 0.013f);
        // the plane's heading alone (stage basis) does not matter once the head ray map is given: a yawed stage basis with a counter-rotated head
        // gives the same world rays and the same flare
        const float yaw = rad(40.0f);
        const float R2[3] = {std::cos(yaw), 0, -std::sin(yaw)}, F2[3] = {-std::sin(yaw), 0, -std::cos(yaw)};   // stage basis yawed by +40 (left)
        const Eye e4 = eye(-40.0f, 20.0f);   // head (in the anchor frame) turned back right by 40: world look = straight at the sun
        const LF::Frame f4 = LF::make_frame(e4.base, e4.ux, e4.vy, R2, kU, F2, L);
        CHECK(f4.ok && std::fabs(f4.su - 0.5f) < 1e-3f && std::fabs(f4.sv - 0.5f) < 1e-3f);
    }
    // ---- in view only (user 2026-10-29): hidden once the sun leaves the image; behind; below the horizon; angle falloff
    {
        const LF::Frame back = frame(eye(180.0f, 0.0f), L);
        CHECK(!back.ok && lum(back, eye(180.0f, 0.0f), 0.5f, 0.5f) == 0.0f);
        const LF::Frame side = frame(eye(-55.0f, 20.0f), L);   // 55 deg right: ~0.2 outside the image -> gate 0
        CHECK(!side.ok);
        LF::Frame edge; float edge_yaw = 0;   // turn right until the sun just leaves the image (u < 0): inside the margin, faded
        for (float y = -30.0f; y > -70.0f; y -= 0.25f) { edge = frame(eye(y, 20.0f), L); if (edge.s_uv && edge.su < -0.01f) { edge_yaw = y; break; } }
        CHECK(edge_yaw < 0.0f && edge.ok && edge.inview > 0.0f && edge.inview < 1.0f);
        float below[3]; sun_dir(0.0f, -5.0f, below);
        CHECK(!frame(eye(0.0f, -5.0f), below).ok);
        float low[3]; sun_dir(0.0f, 1.0f, low);                // at the horizon: partially faded
        const LF::Frame fl = frame(eye(0.0f, 1.0f), low);
        CHECK(fl.ok && fl.horizon > 0.0f && fl.horizon < 1.0f);
        // the closer the head looks at the sun, the brighter the flare at the sun (monotonic)
        float prev = 1e30f;
        for (int a = 0; a <= 40; a += 5) {
            const Eye e = eye(-float(a), 20.0f); const LF::Frame f = frame(e, L);
            CHECK(f.ok);
            float d[3], F[3]; const float tint[3] = {1, 1, 1};
            for (int i = 0; i < 3; ++i) d[i] = f.sa.x * (i == 0) + f.sa.y * (i == 1) + f.sa.z * (i == 2);
            LF::flare_rgb(f, d, tint, F);
            CHECK(F[0] <= prev + 1e-6f); prev = F[0];
        }
    }
    // ---- ghosts lie on the line through the lens centre (opposite the sun) and are visible there
    {
        const Eye e = eye(-15.0f, 28.0f); const LF::Frame f = frame(e, L);
        CHECK(f.ok && f.cs > LF::kGhostCos0);
        // ghost 3 centre (t = 1.75) on the tangent plane -> ray d = f + gx er + gy eu
        const float gx = f.sx * (1.0f - LF::kGhostT[2]), gy = f.sy * (1.0f - LF::kGhostT[2]);
        const float tint[3] = {1, 1, 1};
        float d[3] = {f.f.x + gx * f.er.x + gy * f.eu.x, f.f.y + gx * f.er.y + gy * f.eu.y, f.f.z + gx * f.er.z + gy * f.eu.z}, F[3];
        LF::flare_rgb(f, d, tint, F);
        // the same line, 2 ghost radii further out (same starburst angle, farther from the sun): much darker outside the ghost disc
        const float len = std::sqrt((gx - f.sx) * (gx - f.sx) + (gy - f.sy) * (gy - f.sy));
        const float ox = gx + (gx - f.sx) / len * 2.0f * LF::kGhostRad[2], oy = gy + (gy - f.sy) / len * 2.0f * LF::kGhostRad[2];
        float d2[3] = {f.f.x + ox * f.er.x + oy * f.eu.x, f.f.y + ox * f.er.y + oy * f.eu.y, f.f.z + ox * f.er.z + oy * f.eu.z}, F2[3];
        LF::flare_rgb(f, d2, tint, F2);
        std::printf("ghost centre %.4f outside %.4f\n", F[0] + F[1] + F[2], F2[0] + F2[1] + F2[2]);
        CHECK(F[0] + F[1] + F[2] > F2[0] + F2[1] + F2[2] + 0.01f && F[0] + F[1] + F[2] > 2.0f * (F2[0] + F2[1] + F2[2]));
    }
    // ---- terrain occlusion: flat low ground = visible; a wall in front = hidden; smooth (no popping) as the eye moves
    {
        const LF::V3 lw = LF::norm(LF::V3{L[0], L[1], L[2]});
        const float eye0[3] = {0, 100, 0};
        auto flat = [](float, float) { return 50.0f; };
        CHECK(LF::terrain_vis(eye0, lw, 200.0f, 160000.0f, flat) == 1.0f);
        auto wall = [](float, float z) { return z < -100.0f && z > -400.0f ? 5000.0f : 0.0f; };   // a ridge 100..400 units toward the sun
        CHECK(LF::terrain_vis(eye0, lw, 5000.0f, 160000.0f, wall) == 0.0f);
        const float above[3] = {0, 6000, 0};
        CHECK(LF::terrain_vis(above, lw, 5000.0f, 160000.0f, wall) == 1.0f);   // above the max terrain height: nothing to test
        auto ramp = [](float, float z) { return z < 0.0f ? -z * 0.36f : 0.0f; };   // a slope rising toward the sun at ~19.8 deg
        float prev = -1; float maxstep = 0;
        for (int i = 0; i <= 200; ++i) {
            const float e[3] = {0, 40.0f + float(i), 0};
            const float v = LF::terrain_vis(e, lw, 3000.0f, 160000.0f, ramp);
            if (prev >= 0) maxstep = std::fmax(maxstep, std::fabs(v - prev));
            prev = v;
        }
        CHECK(maxstep < 0.1f);
        float neg[3]; sun_dir(0, -3, neg);
        CHECK(LF::terrain_vis(eye0, LF::norm(LF::V3{neg[0], neg[1], neg[2]}), 200.0f, 160000.0f, flat) == 0.0f);
    }
    // ---- replay-depth occlusion (cockpit mask): taps at the sun's image position
    {
        const LF::Frame f = frame(eye(0.0f, 20.0f), L);
        CHECK(LF::depth_vis(f, [](float, float) { return false; }) == 1.0f);
        CHECK(LF::depth_vis(f, [](float, float) { return true; }) == 0.0f);
        CHECK(LF::depth_vis(f, [](float u, float) { return u > 0.5f; }) == 0.8f);   // the right tap only
        LF::Frame fo;
        for (float y = -30.0f; y > -70.0f; y -= 0.25f) { fo = frame(eye(y, 20.0f), L); if (fo.s_uv && fo.su < -0.01f) break; }
        CHECK(fo.ok && fo.su < 0.0f);   // sun just outside the image: nothing in this image covers it
        CHECK(LF::depth_vis(fo, [](float, float) { return true; }) == 1.0f);
    }
    // ---- eval + display-space composite
    {
        const Eye e = eye(0.0f, 20.0f); const LF::Frame f = frame(e, L);
        const float tint[3] = {1, 1, 1};
        float d[3], F[3]; ray(e, 0.5f, 0.5f, d);
        int calls = 0;
        LF::eval(f, d, tint, 2.0f, [&] { ++calls; return 0.5f; }, F);
        float raw[3]; LF::flare_rgb(f, d, tint, raw);
        CHECK(calls == 1 && std::fabs(F[0] - raw[0]) < 1e-6f);   // k 2 x vis 0.5
        float d2[3], F2[3]; ray(eye(180.0f, 0.0f), 0.5f, 0.5f, d2);
        calls = 0; LF::eval(frame(eye(180.0f, 0.0f), L), d2, tint, 1.0f, [&] { ++calls; return 1.0f; }, F2);
        CHECK(calls == 0 && F2[0] == 0.0f && F2[1] == 0.0f && F2[2] == 0.0f);   // no flare: the occlusion is not evaluated
        const float under[3] = {0.2f, 0.3f, 0.4f}; float o[3];
        LF::composite(under, F2, o); CHECK(o[0] == under[0] && o[1] == under[1] && o[2] == under[2]);   // identity
        const float Fs[3] = {0.1f, 0.1f, 0.1f};
        LF::composite(under, Fs, o);
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(LF::srgb_enc(o[c]) - (LF::srgb_enc(under[c]) + 0.1f)) < 1e-5f);   // + 0.1 in display space
        const float big[3] = {5, 5, 5}; LF::composite(under, big, o); CHECK(o[0] == 1.0f && o[2] == 1.0f);           // clamped at white
    }
    // ---- 2026-10-30 diagnostics: classify() = the shader's early-outs in order; the m03k stage light (mapset: elevation 50 deg, not the moon) with a game-camera basis
    {
        const float m03k_light[4] = {0.32139382f, 0.76604444f, 0.55667037f, 1.0f};   // research stage_sweep m03k.json: light_elev_deg 50, light_is_moon false
        const float no_moon[4] = {0, 0, -1, 0};
        float p32[4]; LF::param_texel(true, 1.0f, p32);
        // an eye looking at the sun: active; the in-view/horizon/angle fields behind peak_gate
        float yaw_deg = 0.0f, pitch_deg = 50.0f;   // find the head direction of the sun in the identity basis: sun = (0.32,0.77,0.56): fwd is -Z, so yaw 180-30 deg
        yaw_deg = std::atan2(-m03k_light[0], -m03k_light[2]) * 180.0f / kPi;
        const Eye at_sun = eye(yaw_deg, pitch_deg);
        const LF::Frame fa = LF::make_frame(at_sun.base, at_sun.ux, at_sun.vy, kR, kU, kF, m03k_light);
        CHECK(LF::classify(p32, m03k_light, no_moon, fa) == LF::Why::active && fa.ok);
        CHECK(LF::peak_gate(fa) > 0.9f);
        // looking the other way: behind; looking at the horizon 70 deg from the sun heading: behind; slightly off: still active (in-view margin)
        const Eye away = eye(yaw_deg + 180.0f, 0.0f);
        CHECK(LF::classify(p32, m03k_light, no_moon, LF::make_frame(away.base, away.ux, away.vy, kR, kU, kF, m03k_light)) == LF::Why::behind);
        const Eye near_sun = eye(yaw_deg + 20.0f, pitch_deg - 10.0f);
        const LF::Frame fn = LF::make_frame(near_sun.base, near_sun.ux, near_sun.vy, kR, kU, kF, m03k_light);
        CHECK(LF::classify(p32, m03k_light, no_moon, fn) == LF::Why::active && LF::peak_gate(fn) > 0.0f && LF::peak_gate(fn) < LF::peak_gate(fa));
        // inside the angle cone (cos > 0.57) but the sun outside the image + margin: out-of-view (narrow FOV eye)
        const Eye narrow = eye(yaw_deg + 40.0f, pitch_deg, 0.2f, 0.16f);
        CHECK(LF::classify(p32, m03k_light, no_moon, LF::make_frame(narrow.base, narrow.ux, narrow.vy, kR, kU, kF, m03k_light)) == LF::Why::out_of_view);
        // the shader's global early-outs
        float off[4]; LF::param_texel(false, 1.0f, off);
        CHECK(LF::classify(off, m03k_light, no_moon, fa) == LF::Why::off);
        float zk[4]; LF::param_texel(true, 0.0f, zk);
        CHECK(LF::classify(zk, m03k_light, no_moon, fa) == LF::Why::off);
        const float no_light[4] = {0, 1, 0, 0};
        CHECK(LF::classify(p32, no_light, no_moon, fa) == LF::Why::no_light);
        const float moon_is_light[4] = {m03k_light[0], m03k_light[1], m03k_light[2], 0.85f};
        CHECK(LF::classify(p32, m03k_light, moon_is_light, fa) == LF::Why::moon);
        const float moon_elsewhere[4] = {-0.8f, 0.17f, -0.57f, 0.85f};   // a moon far from the sun does not switch the flare off
        CHECK(LF::classify(p32, m03k_light, moon_elsewhere, fa) == LF::Why::active);
        float below[4]; sun_dir(0.0f, -10.0f, below); below[3] = 1.0f;
        const Eye at_below = eye(0.0f, -10.0f);
        CHECK(LF::classify(p32, below, no_moon, LF::make_frame(at_below.base, at_below.ux, at_below.vy, kR, kU, kF, below)) == LF::Why::below_horizon);
        CHECK(std::string(LF::why_name(LF::Why::active)) == "active" && std::string(LF::why_name(LF::Why::out_of_view)) == "out-of-view" && std::string(LF::why_name(LF::Why::count)) == "?");
        // robustness: the diagnostics read ONLY the env light, the switch and the eye basis (no E6 sample): the classification is a pure function of those
        const LF::Frame fa2 = LF::make_frame(at_sun.base, at_sun.ux, at_sun.vy, kR, kU, kF, m03k_light);
        CHECK(LF::classify(p32, m03k_light, no_moon, fa2) == LF::classify(p32, m03k_light, no_moon, fa));
    }
    // ---- W15 F3: `active` must mean the shader's minimum-output gate can pass before occlusion; a tiny strength (allowed by the 0..3 parse) or a gate at the very edge of the ramps cannot
    {
        const float m03k_light[4] = {0.32139382f, 0.76604444f, 0.55667037f, 1.0f};
        const float no_moon[4] = {0, 0, -1, 0};
        const float yaw_deg = std::atan2(-m03k_light[0], -m03k_light[2]) * 180.0f / kPi;
        const Eye at_sun = eye(yaw_deg, 50.0f);
        const LF::Frame fa = LF::make_frame(at_sun.base, at_sun.ux, at_sun.vy, kR, kU, kF, m03k_light);
        float tiny[4]; LF::param_texel(true, 0.000001f, tiny);
        CHECK(tiny[0] > 0.0f && tiny[3] == 1.0f);                                                  // the option parse really admits it
        CHECK(LF::classify(tiny, m03k_light, no_moon, fa) != LF::Why::active);                     // F3: was `active` with every pixel returned untouched
        CHECK(LF::classify(tiny, m03k_light, no_moon, fa) == LF::Why::below_min && std::string(LF::why_name(LF::Why::below_min)) == "below-min-output");
        float ok1[4]; LF::param_texel(true, 1.0f, ok1);
        CHECK(LF::classify(ok1, m03k_light, no_moon, fa) == LF::Why::active);
        // near-cutoff gates: the angle ramp is ~0 just above kCoreCos0 (k = 1 included)
        LF::Frame edge = fa; edge.cs = LF::kCoreCos0 + 0.00003f;
        CHECK(LF::classify(ok1, m03k_light, no_moon, edge) == LF::Why::below_min);
        edge = fa; edge.inview = 0.0003f;
        CHECK(LF::classify(ok1, m03k_light, no_moon, edge) == LF::Why::below_min);
        edge = fa; edge.horizon = 0.0003f;
        CHECK(LF::classify(ok1, m03k_light, no_moon, edge) == LF::Why::below_min);
        edge = fa; edge.cs = 0.8f; CHECK(LF::classify(ok1, m03k_light, no_moon, edge) == LF::Why::active);
        // soundness of the bound: whenever classify says below_min, no pixel of the eye reaches the shader's output gate (tints up to the normalised maximum, grid over the image)
        const Eye near_sun = eye(yaw_deg + 20.0f, 40.0f);
        const Eye eyes[2] = {at_sun, near_sun};
        const float tints[3][3] = {{1, 1, 1}, {1, 0.4f, 0.1f}, {0.2f, 0.5f, 1}};
        const float ks[] = {0.000001f, 0.0003f, 0.0007f, 0.0008f, 0.002f, 0.01f};
        unsigned below = 0, act = 0;
        for (const Eye& ey : eyes) {
            const LF::Frame fr = LF::make_frame(ey.base, ey.ux, ey.vy, kR, kU, kF, m03k_light);
            for (float k : ks) {
                float p[4]; LF::param_texel(true, k, p);
                const LF::Why w = LF::classify(p, m03k_light, no_moon, fr);
                if (w != LF::Why::below_min && w != LF::Why::active) continue;
                float worst = 0.0f;
                for (const auto& t : tints)
                    for (int iu = 0; iu <= 64; ++iu) for (int iv = 0; iv <= 64; ++iv) {
                        float d[3], F[3];
                        for (int c = 0; c < 3; ++c) d[c] = ey.base[c] + float(iu) / 64.0f * ey.ux[c] + float(iv) / 64.0f * ey.vy[c];
                        LF::flare_rgb(fr, d, t, F);
                        worst = std::fmax(worst, std::fmax(F[0], std::fmax(F[1], F[2])) * k);
                    }
                if (w == LF::Why::below_min) { ++below; CHECK(worst < LF::kMinOut); } else { ++act; (void)worst; }   // active is the conservative side: the bound may exceed the real maximum
            }
        }
        CHECK(below > 0 && act > 0);
    }
    std::printf("lens_flare_offline: %u checks passed\n", g_checks);
    return 0;
}
