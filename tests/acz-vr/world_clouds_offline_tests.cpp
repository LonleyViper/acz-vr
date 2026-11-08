// WORLD-CLOUDS offline tests (world_clouds.h = the CPU mirror of xr_stereo_blit.hlsl wc_accum / wc_over; docs/world-render/world-clouds.md): options and env
// texels, the hashed puff grid (deterministic, each puff inside its cell column), the 2D DDA against a brute-force over every nearby cell, world anchoring
// (two eyes / two frames see the same puff at the same world point), depth splits (terrain/object limit, front + back = whole), the near fade, the CPU
// trace path with a synthetic environment, and the producer side (game puff quads collapsed per eye, the read-only puff census line, stage switches).
#include "world_clouds.h"
#include "terrain_stage.h"
#include "fx_sprite_replay.h"
#include "effects_stage_flags.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace W = acz::wcloud;
namespace T = acz::terrain;
namespace FS = acz::fxsprite;
namespace s5 = acz::gpu_copy::step5;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static uint32_t g_rng = 12345u;
static float frand() { g_rng = g_rng * 1664525u + 1013904223u; return float(g_rng >> 8) * (1.0f / 16777216.0f); }
static void norm(float d[3]) { const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); for (int i = 0; i < 3; ++i) d[i] /= l; }

static W::Params params(float alt = 9000.0f) { W::Params p; p.on = true; p.alt = alt; p.thick = 3000.0f; p.cell = 12000.0f; p.radius = 3000.0f; p.opacity = 0.6f; p.fill = W::kDefFill; return p; }
// WCLOUD-FIX: the first cell (row k0) that holds a puff under the fill gate.
static void occupied_cell(const W::Params& p, int k0, int& ci, int& ck) { ck = k0; for (ci = 0; !(W::unit(W::hash(ci, ck, 5u)) < p.fill); ++ci) {} }
static float cov_fn(float x, float z) { return 0.5f + 0.5f * std::sin(x * 0.00011f) * std::cos(z * 0.00013f); }
static float noise_fn(float u, float v) { return 0.5f + 0.5f * std::sin(u * 0.37f + v * 0.21f); }
static float haze_fn(float t) { return T::haze_fraction(t, 55000.0f, 160000.0f); }
static const float kHz[3] = {0.5f, 0.55f, 0.53f}, kLit[3] = {155 / 255.0f, 153 / 255.0f, 123 / 255.0f}, kDark[3] = {64 / 255.0f, 83 / 255.0f, 75 / 255.0f};

static W::Accum dda(const W::Params& p, const float o[3], const float d[3], float t0, float t1) {
    W::Accum a; W::accumulate(p, o, d, t0, t1, 160000.0f, cov_fn, noise_fn, haze_fn, kHz, kLit, kDark, a); return a;
}
// Brute force: every cell within reach, the puffs the ray meets sorted by closest approach, composited front to back with the same per-puff terms.
static W::Accum brute(const W::Params& p, const float o[3], const float d[3], float t0, float t1) {
    W::Accum acc;
    const float tlo = std::max(t0, 0.0f), thi = std::min(t1, 160000.0f);
    struct Hit { float tc, a, col[3]; };
    std::vector<Hit> hits;
    const int ci0 = int(std::floor(o[0] / p.cell)), ck0 = int(std::floor(o[2] / p.cell));
    for (int di = -16; di <= 16; ++di) for (int dk = -16; dk <= 16; ++dk) {
        const W::Puff q = W::puff_in_cell(p, ci0 + di, ck0 + dk);
        const float dens = W::density(p, ci0 + di, ck0 + dk, cov_fn(q.c[0], q.c[2]));
        if (!(dens > 0)) continue;
        const float oc[3] = {q.c[0] - o[0], q.c[1] - o[1], q.c[2] - o[2]};
        const float tc = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const float b2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - tc * tc;
        if (!(tc > tlo && tc < thi && b2 < q.r * q.r)) continue;
        Hit h; h.tc = tc;
        const float pp[3] = {o[0] + d[0] * tc, o[1] + d[1] * tc, o[2] + d[2] * tc};
        const int ci = ci0 + di, ck = ck0 + dk;   // WCLOUD-LOOK: the lumpy outline (two map octaves, per-cell offset) and the lit gain / haze scale
        const float h1 = W::unit(W::hash(ci, ck, 1u)) * W::kMapSize, h2 = W::unit(W::hash(ci, ck, 2u)) * W::kMapSize;
        const float e = W::kEdgeW1 * noise_fn(pp[0] / W::kEdgeRep1 * W::kMapSize + h1, (pp[2] + pp[1]) / W::kEdgeRep1 * W::kMapSize + h2) +
                        (1.0f - W::kEdgeW1) * noise_fn(pp[0] / W::kEdgeRep2 * W::kMapSize + h2, (pp[2] + pp[1]) / W::kEdgeRep2 * W::kMapSize + h1);
        const float r2 = q.r * q.r;
        float a = dens * p.opacity * std::pow(W::sat(((1.0f - b2 / r2) - W::kEdge * (1.0f - e)) / (1.0f - W::kEdge)), W::kShapePow);
        a *= (1.0f - W::kNoise) + W::kNoise * noise_fn(pp[0] / W::kNoiseRep * W::kMapSize, (pp[2] + pp[1]) / W::kNoiseRep * W::kMapSize);
        a *= W::smooth(W::kNear0, W::kNear1, tc);
        const float ty = o[1] + d[1] * (tc - std::sqrt(q.r * q.r - b2));   // WCLOUD-FIX2: entry point height; the alpha is not hazed
        const float s = W::sat(0.5f + 0.5f * (ty - q.c[1]) / q.r), f = haze_fn(tc * W::kHazeScale);
        for (int c = 0; c < 3; ++c) { const float lt = std::fmin(kLit[c] * W::kLitGain, 1.0f), cc = kDark[c] + (lt - kDark[c]) * s; h.col[c] = cc + (kHz[c] - cc) * f; }
        h.a = a;
        hits.push_back(h);
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.tc < b.tc; });
    for (const Hit& h : hits) {
        const float w = (1.0f - acc.a) * h.a;
        for (int c = 0; c < 3; ++c) acc.c[c] += w * h.col[c];
        acc.a += w;
        if (acc.a > W::kAlphaStop) break;
    }
    return acc;
}

// A game map (fx_memory ScreenMap via game_map_from_a) for a camera at E looking along fwd: GS window 512 x 448 around 2048, half tangents 0.6152 / 0.346.
static void make_a(const double E[3], const double right[3], const double up[3], const double fwd[3], double A[16]) {
    const double tx = 0.6152, ty = 0.346, b = acz::fxmem::kDepthB, z0 = acz::fxmem::kZ0;
    for (int r = 0; r < 3; ++r) {
        A[r * 4 + 0] = 2048.0 * fwd[r] + (256.0 / tx) * right[r];
        A[r * 4 + 1] = 2048.0 * fwd[r] - (224.0 / ty) * up[r];
        A[r * 4 + 3] = fwd[r];
        A[r * 4 + 2] = b * A[r * 4 + 3];
    }
    for (int c : {0, 1, 3}) { double s = 0; for (int r = 0; r < 3; ++r) s += E[r] * A[r * 4 + c]; A[12 + c] = -s; }
    A[14] = z0 + b * A[15];
}
static acz::fxmem::Vtx vtx_at(const acz::fxmem::ScreenMap& m, const double P[3], float u, float v) {
    double xs, ys, d; FS::sp_project(m, P, xs, ys, d);
    acz::fxmem::Vtx x{}; x.pos[0] = float(xs); x.pos[1] = float(ys); x.pos[2] = float(m.z0 / d + m.b); x.pos[3] = 1.0f; x.uv[0] = u; x.uv[1] = v;
    for (int c = 0; c < 4; ++c) x.col[c] = 0.5f;
    return x;
}

int main() {
    // ---- options (parse_stage_option) and the env texels (build_env_texture)
    {
        std::string id, why; T::Conventions c;
        CHECK(!T::Conventions{}.wcloud);
        CHECK(T::parse_stage_option("m02a;env:1", id, c, why) && !c.wcloud);
        CHECK(T::parse_stage_option("m02a;env:1;wcloud:1", id, c, why) && c.wcloud && c.wc_alt < -1e8f && c.wc_size == W::kDefSize && c.wc_cell == W::kDefCell &&
              c.wc_thick < 0.0f && c.wc_opacity == W::kDefOpacity && c.wc_fill == W::kDefFill && c.wc_drop_edge == W::kDefDropEdge && !c.wc_diag);
        { T::Conventions c2; CHECK(T::parse_stage_option("m02a;env:1;wcloud:1;wcsize:4000", id, c2, why) && c2.wc_cell == 10000.0f); }   // default cell = 2.5 x wcsize
        { T::Conventions c2; CHECK(T::parse_stage_option("m02a;env:1;wcloud:1;wcfill:0.5", id, c2, why) && c2.wc_fill == 0.5f); }
        CHECK(T::parse_stage_option("m03k;env:1;wcloud:1;wcalt:5500;wcthk:2000;wcsize:2500;wccell:9000;wcop:0.8;wcdrop:0;wcdiag:1", id, c, why) && c.wc_alt == 5500.0f &&
              c.wc_thick == 2000.0f && c.wc_size == 2500.0f && c.wc_cell == 9000.0f && c.wc_opacity == 0.8f && c.wc_drop_edge == 0.0f && c.wc_diag);
        const char* bad[] = {"m02a;wcloud:2", "m02a;wcalt:x", "m02a;wcalt:90000", "m02a;wcsize:100", "m02a;wccell:500", "m02a;wcop:0", "m02a;wcop:1.5",
                             "m02a;wcdrop:50", "m02a;wcthk:-1", "m02a;wcsize:6000;wccell:12000", "m02a;wccell:7000", "m02a;wcdiag:x",
                             "m02a;wcfill:0", "m02a;wcfill:1.5", "m02a;wcfill:x"};
        for (const char* b : bad) { T::Conventions cc; CHECK(!T::parse_stage_option(b, id, cc, why)); }
        T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 9000.0f);
        std::vector<float> e0, e1, e2;
        T::Conventions off; T::build_env_texture(st, off, e0);
        T::Conventions on; on.wcloud = true; T::build_env_texture(st, on, e1);
        CHECK(e0.size() == e1.size() && !e0.empty());
        size_t diff = 0, first = 0;
        for (size_t i = 0; i < e0.size(); ++i) if (e0[i] != e1[i]) { if (!diff) first = i; ++diff; }
        CHECK(diff > 0 && first >= size_t(W::kPWc) * 4 && first < size_t(W::kPWc2 + 1) * 4);   // only texels 33..34 differ
        for (size_t i = size_t(W::kPWc2 + 1) * 4; i < e0.size(); ++i) CHECK(e0[i] == e1[i]);
        for (int i = 0; i < 8; ++i) CHECK(e0[size_t(W::kPWc) * 4 + size_t(i)] == 0.0f);
        const float* p = &e1[size_t(W::kPWc) * 4];
        CHECK(p[0] == 9000.0f && p[1] == W::kDefThick && p[2] == W::kDefCell && p[3] == 1.0f && p[4] == W::kDefSize && p[5] == W::kDefOpacity && p[6] == W::kDefFill);
        on.wc_alt = 5500.0f; T::build_env_texture(st, on, e2);
        CHECK(e2[size_t(W::kPWc) * 4] == 5500.0f);
        const W::Params pp = W::params_from(p, p + 4);
        CHECK(pp.on && pp.alt == 9000.0f && pp.radius == W::kDefSize);
        CHECK(!W::params_from(&e0[size_t(W::kPWc) * 4], &e0[size_t(W::kPWc2) * 4]).on);
        float bad8[8] = {9000, 3000, 4000, 1, 3000, 0.6f, 0, 0};   // cell < 2 R: refused
        CHECK(!W::params_from(bad8, bad8 + 4).on);
    }
    // ---- the hashed grid: deterministic, in [0,1), every puff inside its own cell column, centres spread over the band
    {
        const W::Params p = params();
        float ymin = 1e9f, ymax = -1e9f;
        for (int n = 0; n < 20000; ++n) {
            const int i = int(frand() * 2000.0f) - 1000, k = int(frand() * 2000.0f) - 1000;
            const float u = W::unit(W::hash(i, k, 1u));
            CHECK(u >= 0.0f && u < 1.0f && W::hash(i, k, 1u) == W::hash(i, k, 1u));
            const W::Puff q = W::puff_in_cell(p, i, k);
            CHECK(q.r >= W::kRadiusMin * p.radius - 1e-3f && q.r <= p.radius + 1e-3f);
            CHECK(q.c[0] - q.r >= float(i) * p.cell - 0.5f && q.c[0] + q.r <= float(i + 1) * p.cell + 0.5f);
            CHECK(q.c[2] - q.r >= float(k) * p.cell - 0.5f && q.c[2] + q.r <= float(k + 1) * p.cell + 0.5f);
            CHECK(std::fabs(q.c[1] - p.alt) <= 0.5f * p.thick + 1e-2f);
            ymin = std::min(ymin, q.c[1]); ymax = std::max(ymax, q.c[1]);
        }
        CHECK(ymax - ymin > 0.9f * p.thick);
        CHECK(W::hash(1, 2, 1u) != W::hash(2, 1, 1u) && W::hash(-1, 0, 1u) != W::hash(1, 0, 1u));
    }
    // ---- DDA vs brute force (ordering, cell coverage, early out) on random rays: below / inside / above the band, steep and level, limited t1
    {
        const W::Params p = params();
        unsigned hit = 0, n = 0; float worst = 0;
        for (int r = 0; r < 4000; ++r) {
            const float o[3] = {(frand() - 0.5f) * 200000.0f, 1000.0f + frand() * 16000.0f, (frand() - 0.5f) * 200000.0f};
            float d[3] = {frand() - 0.5f, (frand() - 0.5f) * (r % 3 == 0 ? 0.05f : 1.2f), frand() - 0.5f}; norm(d);
            const float t1 = r % 5 == 0 ? 5000.0f + frand() * 60000.0f : 3e38f;
            const W::Accum a = dda(p, o, d, 0.0f, t1), b = brute(p, o, d, 0.0f, t1);
            const float e = std::fabs(a.a - b.a) + std::fabs(a.c[0] - b.c[0]) + std::fabs(a.c[1] - b.c[1]) + std::fabs(a.c[2] - b.c[2]);
            worst = std::max(worst, e);
            CHECK(e < 2e-4f);
            CHECK(a.cells <= unsigned(W::kMaxCells));
            ++n; if (a.a > 0.01f) ++hit;
        }
        std::printf("dda vs brute: %u rays, %u with clouds, worst |diff| %.2e\n", n, hit, double(worst));
        CHECK(hit > 200);
    }
    // ---- world anchoring: a puff seen from two eyes (stereo baseline) and from a moved camera is at the same world point
    {
        const W::Params p = params();
        int found = 0;
        for (int i = 0; i < 50 && found < 10; ++i) for (int k = 0; k < 50 && found < 10; ++k) {
            const W::Puff q = W::puff_in_cell(p, i, k);
            if (W::density(p, i, k, cov_fn(q.c[0], q.c[2])) < 0.5f) continue;
            ++found;
            const float eyes[3][3] = {{q.c[0] - 9000.0f, q.c[1] - 2000.0f, q.c[2] + 1000.0f}, {q.c[0] - 9000.0f + 0.33f, q.c[1] - 2000.0f, q.c[2] + 1000.0f},
                                      {q.c[0] - 6000.0f, q.c[1] - 500.0f, q.c[2] - 3000.0f}};   // left eye, right eye (0.065 m x 5 u/m), a later frame
            float amax[3] = {};
            for (int e = 0; e < 3; ++e) {
                float d[3] = {q.c[0] - eyes[e][0], q.c[1] - eyes[e][1], q.c[2] - eyes[e][2]}; norm(d);
                amax[e] = dda(p, eyes[e], d, 0.0f, 3e38f).a;
            }
            CHECK(amax[0] > 0.0f && std::fabs(amax[0] - amax[1]) < 1e-3f);   // the two eyes agree on the puff at the same world centre
            CHECK(amax[2] > 0.0f);                                            // still there from another position
            // determinism: the same world ray gives the same bytes
            float d[3] = {q.c[0] - eyes[0][0], q.c[1] - eyes[0][1], q.c[2] - eyes[0][2]}; norm(d);
            const W::Accum a1 = dda(p, eyes[0], d, 0.0f, 3e38f), a2 = dda(p, eyes[0], d, 0.0f, 3e38f);
            CHECK(std::memcmp(&a1, &a2, sizeof a1) == 0);
        }
        CHECK(found == 10);
    }
    // ---- depth: a limit nearer than the puff hides it; the object split (front [0, r] over back [r, inf)) equals the whole ray; the near fade
    {
        const W::Params p = params();
        int tested = 0;
        for (int r = 0; r < 3000 && tested < 200; ++r) {
            const float o[3] = {(frand() - 0.5f) * 100000.0f, 5000.0f + frand() * 3000.0f, (frand() - 0.5f) * 100000.0f};
            float d[3] = {frand() - 0.5f, 0.05f + frand() * 0.3f, frand() - 0.5f}; norm(d);
            const W::Accum whole = dda(p, o, d, 0.0f, 3e38f);
            if (!(whole.a > 0.05f)) continue;
            ++tested;
            const float rr = 2000.0f + frand() * 40000.0f;
            const W::Accum f = dda(p, o, d, 0.0f, rr), b = dda(p, o, d, rr, 3e38f);
            float c[3]; const float a = f.a + (1.0f - f.a) * b.a;
            for (int i = 0; i < 3; ++i) c[i] = f.c[i] + (1.0f - f.a) * b.c[i];
            CHECK(std::fabs(a - whole.a) < 2e-4f && std::fabs(c[0] - whole.c[0]) < 2e-4f && std::fabs(c[2] - whole.c[2]) < 2e-4f);
            CHECK(dda(p, o, d, 0.0f, 100.0f).a == 0.0f);   // a terrain hit / object 100 units ahead hides every puff (all lie beyond the near band here)
        }
        CHECK(tested == 200);
        int qi = 0, qk = 0; occupied_cell(p, 4, qi, qk);
        const W::Puff q = W::puff_in_cell(p, qi, qk);
        const float o[3] = {q.c[0] - 100.0f, q.c[1], q.c[2]}; float d[3] = {1, 0, 0};
        W::Params pn = p; W::Accum a;
        W::accumulate(pn, o, d, 0.0f, 1000.0f, 160000.0f, [](float, float) { return 1.0f; }, noise_fn, haze_fn, kHz, kLit, kDark, a);
        CHECK(a.a < 1e-6f);   // closest approach 100 units ahead (inside the puff): faded out, no whiteout pop
        W::Params poff = p; poff.on = false; W::Accum z;
        W::accumulate(poff, o, d, 0.0f, 3e38f, 160000.0f, cov_fn, noise_fn, haze_fn, kHz, kLit, kDark, z);
        CHECK(z.a == 0.0f && z.cells == 0);
        // a ray far above the band never walks a cell
        const float oh[3] = {0, 40000.0f, 0}; float du[3] = {0.3f, 0.5f, 0.1f}; norm(du);
        CHECK(dda(p, oh, du, 0.0f, 3e38f).cells == 0);
        // colour: lit above, dark below (no haze close by)
        W::Accum top, bot;
        const float ot[3] = {q.c[0] - 3000.0f, q.c[1] + 0.6f * q.r, q.c[2]}, ob[3] = {q.c[0] - 3000.0f, q.c[1] - 0.6f * q.r, q.c[2]};
        W::accumulate(p, ot, d, 0.0f, 3e38f, 160000.0f, [](float, float) { return 1.0f; }, [](float, float) { return 1.0f; }, [](float) { return 0.0f; }, kHz, kLit, kDark, top);
        W::accumulate(p, ob, d, 0.0f, 3e38f, 160000.0f, [](float, float) { return 1.0f; }, [](float, float) { return 1.0f; }, [](float) { return 0.0f; }, kHz, kLit, kDark, bot);
        CHECK(top.a > 0.0f && bot.a > 0.0f && top.c[0] / top.a > bot.c[0] / bot.a);
    }
    // ---- WCLOUD-FIX (step138; world-clouds.md section 6). The step137 null runs runAGC1/runAGC2 (m03k, wcloud:1, active=yes) drew no puff: (1) the band
    // came from the layer-0 field (alt=6000) while the game's own puffs on m03k sit at 8.8 k..13.2 k (wcdiag census), and (2) the field was near-empty: m03k's
    // coverage map is binary with 18.8 % of its texels set, and the old law (density = smoothstep(0.2, 0.6, map at the centre)) left ~22 % of the 12 km cells
    // with a puff, ~3 % of the view.
    {
        // (a) the band: wcalt/wcthk absent = the stage census table; the options override; an unmeasured stage = layer 0 + 3000; a takeoff stage that adopted
        // its mission's environment takes the mission's band.
        T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 6000.0f);
        std::memcpy(st.id, "m03k", 5);
        std::string id, why; T::Conventions on;
        CHECK(T::parse_stage_option("m03k;env:1;wcloud:1", id, on, why));
        std::vector<float> e;
        T::build_env_texture(st, on, e);
        W::Params p = W::params_from(&e[size_t(W::kPWc) * 4], &e[size_t(W::kPWc2) * 4]);
        CHECK(p.on && p.alt == 10600.0f && p.thick == 4400.0f && p.cell == W::kDefCell && p.radius == W::kDefSize && p.opacity == W::kDefOpacity && p.fill == W::kDefFill);
        CHECK(std::string(W::band_source("m03k", on.wc_alt)) == "stage-census" && std::string(W::band_source("synth", -1e9f)) == "layer0" &&
              std::string(W::band_source("m03k", 5500.0f)) == "option");
        W::Band b; CHECK(W::stage_band("m02a", b) && b.alt == 9500.0f && b.thick == 8000.0f && !W::stage_band("m03", b) && !W::stage_band("m03kx", b) && !W::stage_band(nullptr, b));
        // the census band lies inside the measured puff centres (p10 8.8 k .. p90 13.2 k) and NOT around the layer-0 field
        CHECK(p.alt - 0.5f * p.thick >= 8000.0f && p.alt + 0.5f * p.thick <= 13500.0f && std::fabs(p.alt - 6000.0f) > 4000.0f);
        T::Conventions o2; CHECK(T::parse_stage_option("m03k;env:1;wcloud:1;wcalt:5500", id, o2, why)); T::build_env_texture(st, o2, e);
        p = W::params_from(&e[size_t(W::kPWc) * 4], &e[size_t(W::kPWc2) * 4]);
        CHECK(p.alt == 5500.0f && p.thick == 4400.0f);
        T::Conventions o3; CHECK(T::parse_stage_option("m03k;env:1;wcloud:1;wcthk:2000;wcfill:0.5;wcop:0.6", id, o3, why)); T::build_env_texture(st, o3, e);
        p = W::params_from(&e[size_t(W::kPWc) * 4], &e[size_t(W::kPWc2) * 4]);
        CHECK(p.alt == 10600.0f && p.thick == 2000.0f && p.fill == 0.5f && p.opacity == 0.6f);
        T::StageData un = st; std::memcpy(un.id, "zz9z", 5);
        T::build_env_texture(un, on, e);
        p = W::params_from(&e[size_t(W::kPWc) * 4], &e[size_t(W::kPWc2) * 4]);
        CHECK(p.alt == 6000.0f && p.thick == W::kDefThick);   // unmeasured stage: the layer-0 field, as before
        T::StageData tk = T::make_synthetic_stage(4, false); std::memcpy(tk.id, "k00a", 5);
        T::Conventions toenv; CHECK(T::parse_stage_option("k00a;env:1;wcloud:1;toenv:mission", id, toenv, why));
        CHECK(T::adopt_mission_env(tk, st, toenv) && std::string(tk.env_id) == "m03k");
        T::build_env_texture(tk, toenv, e);
        p = W::params_from(&e[size_t(W::kPWc) * 4], &e[size_t(W::kPWc2) * 4]);
        CHECK(p.alt == 10600.0f && p.thick == 4400.0f);
        // the config line carries the fill and the band source (fits the 256-byte minimum buffer)
        char line[256];
        W::config_line(line, sizeof line, true, true, p, 2000.0f, true, W::band_source(tk.env_id, toenv.wc_alt));
        CHECK(std::strstr(line, ";alt=10600;thick=4400;cell=7500;radius=3000;opacity=0.90;fill=0.80;alt-src=stage-census;") != nullptr && std::strlen(line) < 250);
        W::config_line(line, sizeof line, true, false, W::Params{}, 0.0f, false, "layer0");
        CHECK(std::strstr(line, "doc=docs/world-render/world-clouds.md") != nullptr);
        // (b) the fill gate: the occupied fraction follows wcfill and NOT the coverage map
        W::Params g = params(); unsigned occ = 0, n = 0, occ_old = 0;
        auto cov_bin = [](float x, float z) {   // an m03k-like binary map: 18.8 % of the 256 x 256 texels set, repeat 65536 units, bilinear + wrap
            const float u = x / 65536.0f * 256.0f - 0.5f, v = z / 65536.0f * 256.0f - 0.5f;
            const float fx = std::floor(u), fy = std::floor(v), wx = u - fx, wy = v - fy;
            float a = 0.0f;
            for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
                const int xx = (int(fx) + i) & 255, yy = (int(fy) + j) & 255;
                const float t = W::unit(W::hash(xx, yy, 9u)) < 0.188f ? 1.0f : 0.0f;
                a += (i ? wx : 1.0f - wx) * (j ? wy : 1.0f - wy) * t;
            }
            return a;
        };
        for (int i = -60; i < 60; ++i) for (int k = -60; k < 60; ++k) {
            const W::Puff q = W::puff_in_cell(g, i, k);
            const float cv = cov_bin(q.c[0], q.c[2]);
            ++n; if (W::density(g, i, k, cv) > 0.0f) ++occ;
            if (W::smooth(W::kCovLo, W::kCovHi, cv) > 0.0f) ++occ_old;
            CHECK(W::density(g, i, k, cv) == 0.0f || (W::density(g, i, k, cv) >= W::kDensFloor && W::density(g, i, k, cv) <= 1.0f));
        }
        const double fo = double(occ) / n, fold = double(occ_old) / n;
        std::printf("fill gate: occupied %.3f (fill %.2f); the old coverage gate on the m03k-like map: %.3f\n", fo, double(g.fill), fold);
        CHECK(std::fabs(fo - g.fill) < 0.03 && fold < fo);   // the old gate kept fewer cells (real m03k map: 22 %)
        // (c) the AGC2 pose (eye y 20000, pitched 35 deg down onto the m03k deck, terrain at y 0): the new defaults show a deck. The step137 parameters
        // (layer-0 band, 12 km cells, the coverage gate, op 0.6, falloff 1.5) on the REAL m03k map gave 0.2 % of rays > 0.05 (doc section 6, offline replay).
        W::Params nw; nw.on = true; nw.alt = 10600.0f; nw.thick = 4400.0f; nw.cell = W::kDefCell; nw.radius = W::kDefSize; nw.opacity = W::kDefOpacity; nw.fill = W::kDefFill;
        const float eye[3] = {101000.0f, 20000.0f, 101500.0f};
        unsigned rays = 0, vis = 0; double sum = 0;
        for (int yd = 0; yd < 360; yd += 3) for (int pj = -5; pj <= 5; ++pj) {
            const float pr = (-35.0f + float(pj)) * 3.14159265f / 180.0f, yr = float(yd) * 3.14159265f / 180.0f;
            const float d[3] = {std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
            W::Accum a; W::accumulate(nw, eye, d, 0.0f, eye[1] / -d[1], 160000.0f, cov_bin, noise_fn, haze_fn, kHz, kLit, kDark, a);
            ++rays; sum += a.a; if (a.a > 0.05f) ++vis;
            CHECK(a.cells <= unsigned(W::kMaxCells) && a.a <= 1.0f);
        }
        std::printf("AGC2 pose (m03k band, defaults): %.3f of rays > 0.05, mean coverage %.3f\n", double(vis) / rays, sum / rays);
        CHECK(double(vis) / rays > 0.25 && sum / rays > 0.04);
        // the same pose with the step137 band (alt 6000, thick 3000) is still a deck below (the fix does not depend on the band alone) ...
        W::Params lo = nw; lo.alt = 6000.0f; lo.thick = 3000.0f; unsigned vis_lo = 0;
        for (int yd = 0; yd < 360; yd += 3) {
            const float pr = -35.0f * 3.14159265f / 180.0f, yr = float(yd) * 3.14159265f / 180.0f;
            const float d[3] = {std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
            W::Accum a; W::accumulate(lo, eye, d, 0.0f, eye[1] / -d[1], 160000.0f, cov_bin, noise_fn, haze_fn, kHz, kLit, kDark, a);
            if (a.a > 0.05f) ++vis_lo;
        }
        CHECK(vis_lo > 0);
        // ... and flying INSIDE the census band level, puffs are all around (the AGC1 cruise at y ~11 k)
        const float ein[3] = {101000.0f, 11000.0f, 101500.0f}; unsigned vin = 0, nin = 0;
        for (int yd = 0; yd < 360; yd += 3) {
            const float yr = float(yd) * 3.14159265f / 180.0f; const float d[3] = {std::sin(yr), 0.0f, std::cos(yr)};
            W::Accum a; W::accumulate(nw, ein, d, 0.0f, 3e38f, 160000.0f, cov_bin, noise_fn, haze_fn, kHz, kLit, kDark, a);
            ++nin; if (a.a > 0.05f) ++vin;
        }
        std::printf("inside the band level: %u of %u azimuths > 0.05\n", vin, nin);
        CHECK(double(vin) / nin > 0.7);
        // (d) WCLOUD-FIX2 (step138 null runs: the band and the field were right but the eye dumps showed nothing). What a viewer sees is the display
        // luminance change, not the coverage. With look:1 (live day cfg) the haze length is kLookHazeL = 21500, and the step138 law hazed each puff twice
        // (colour toward the haze AND alpha x (1 - f)), shaded it by the closest-approach height (mid-grey from above, ~ the haze colour) and thinned it
        // (falloff 0.75, noise 0.5). Measured over the step138 AGC2 dump's own colours (haze below the horizon ~(67,85,105), ground ~(80,82,87)):
        const float hzL[3] = {0.26f, 0.33f, 0.41f}, under[3] = {0.31f, 0.32f, 0.34f};
        const float litM[3] = {0.671f, 0.643f, 0.584f}, darkM[3] = {0.384f, 0.361f, 0.306f};   // m03k P[21] / P[22]
        auto haze_live = [](float t) { return T::haze_fraction(t, T::kLookHazeL, 160000.0f); };
        auto lum = [](const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
        // the step138 per-puff law, brute force over the cells around the eye (front-to-back by closest approach)
        auto law138 = [&](const float o[3], const float d[3], float t1) {
            struct Hit { float tc, a, col[3]; };
            std::vector<Hit> hits;
            const int ci0 = int(std::floor(o[0] / nw.cell)), ck0 = int(std::floor(o[2] / nw.cell));
            for (int di = -12; di <= 12; ++di) for (int dk = -12; dk <= 12; ++dk) {
                const W::Puff q = W::puff_in_cell(nw, ci0 + di, ck0 + dk);
                const float dens = W::density(nw, ci0 + di, ck0 + dk, cov_bin(q.c[0], q.c[2]));
                if (!(dens > 0)) continue;
                const float oc[3] = {q.c[0] - o[0], q.c[1] - o[1], q.c[2] - o[2]};
                const float tc = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2], b2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - tc * tc;
                if (!(tc > 0.0f && tc < t1 && b2 < q.r * q.r)) continue;
                const float pp[3] = {o[0] + d[0] * tc, o[1] + d[1] * tc, o[2] + d[2] * tc};
                float a = dens * nw.opacity * std::pow(1.0f - b2 / (q.r * q.r), 0.75f);
                a *= 0.5f + 0.5f * noise_fn(pp[0] / W::kNoiseRep * W::kMapSize, (pp[2] + pp[1]) / W::kNoiseRep * W::kMapSize);
                a *= W::smooth(W::kNear0, W::kNear1, tc);
                const float sh = W::sat(0.5f + 0.5f * (pp[1] - q.c[1]) / q.r), f = haze_live(tc);
                Hit h; h.tc = tc; h.a = a * (1.0f - f);
                for (int c = 0; c < 3; ++c) { const float cc = darkM[c] + (litM[c] - darkM[c]) * sh; h.col[c] = cc + (hzL[c] - cc) * f; }
                hits.push_back(h);
            }
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.tc < b.tc; });
            W::Accum acc;
            for (const Hit& h : hits) { const float w = (1.0f - acc.a) * h.a; for (int c = 0; c < 3; ++c) acc.c[c] += w * h.col[c]; acc.a += w; }
            return acc;
        };
        unsigned n138 = 0, v138 = 0, vnew = 0; double dl138 = 0, dlnew = 0;
        for (int yd = 1; yd < 360; yd += 4) for (int pj = -4; pj <= 4; pj += 2) {
            const float pr = (-35.0f + float(pj)) * 3.14159265f / 180.0f, yr = float(yd) * 3.14159265f / 180.0f;
            const float d[3] = {std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
            const float t1 = eye[1] / -d[1];
            const W::Accum a0 = law138(eye, d, t1);
            W::Accum a1; W::accumulate(nw, eye, d, 0.0f, t1, 160000.0f, cov_bin, noise_fn, haze_live, hzL, litM, darkM, a1);
            float o0[3], o1[3];
            for (int c = 0; c < 3; ++c) { o0[c] = a0.c[c] + (1.0f - a0.a) * under[c]; o1[c] = a1.c[c] + (1.0f - a1.a) * under[c]; }
            const float d0 = std::fabs(lum(o0) - lum(under)) * 255.0f, d1 = std::fabs(lum(o1) - lum(under)) * 255.0f;
            ++n138; dl138 += d0; dlnew += d1; if (d0 >= 8.0f) ++v138; if (d1 >= 8.0f) ++vnew;
        }
        std::printf("AGC2 pose, live haze 21500: step138 law mean dL %.2f/255, %.3f of rays >= 8/255; WCLOUD-FIX2 mean dL %.2f/255, %.3f >= 8/255\n",
                    dl138 / n138, double(v138) / n138, dlnew / n138, double(vnew) / n138);
        CHECK(double(v138) / n138 < 0.1 && dl138 / n138 < 3.0);         // the step138 dump: next to nothing a viewer could see (Python replay: 0.067, 2.09)
        CHECK(double(vnew) / n138 > 0.2 && dlnew / n138 > 2.0 * dl138 / n138);
        // shading by the entry point: seen from straight above a puff is at least as light as its mid colour; from straight below at most
        {
            int qi = 0, qk = 0; occupied_cell(nw, 3, qi, qk);
            const W::Puff q = W::puff_in_cell(nw, qi, qk);
            const float oa[3] = {q.c[0] + 10.0f, q.c[1] + 5000.0f, q.c[2]}, ob[3] = {q.c[0] + 10.0f, q.c[1] - 5000.0f, q.c[2]};
            const float dn[3] = {0, -1, 0}, upd[3] = {0, 1, 0};
            auto one = [](float, float) { return 1.0f; };
            W::Accum ta, tb;
            W::accumulate(nw, oa, dn, 0.0f, 3e38f, 160000.0f, one, one, [](float) { return 0.0f; }, hzL, litM, darkM, ta);
            W::accumulate(nw, ob, upd, 0.0f, 3e38f, 160000.0f, one, one, [](float) { return 0.0f; }, hzL, litM, darkM, tb);
            CHECK(ta.a > 0.3f && tb.a > 0.3f && ta.c[0] / ta.a > 0.98f * litM[0] && tb.c[0] / tb.a < 1.02f * darkM[0]);
        }
        // (e) WCLOUD-LOOK (step140 magenta proof: puffs everywhere but hard round discs; summary-wcloud mean-dl 3.58, max-dl 16.6). With the m03k-like binary
        // map as the noise (as live: te_map channel 0), from the AGC2 pose: where a ray has coverage, the change must be plainly visible, the brightest
        // well above 60/255 (Python replay of this law: mean 30.5, max 76.7; with the real m03k map 29.9 / 72.5).
        {
            unsigned with = 0; double sdl = 0; float mx = 0;
            for (int yd = 1; yd < 360; yd += 4) for (int pj = -4; pj <= 4; pj += 2) {
                const float pr = (-35.0f + float(pj)) * 3.14159265f / 180.0f, yr = float(yd) * 3.14159265f / 180.0f;
                const float d[3] = {std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
                W::Accum a; W::accumulate(nw, eye, d, 0.0f, eye[1] / -d[1], 160000.0f, cov_bin, [&](float u, float v) { return cov_bin(u * 256.0f, v * 256.0f); },
                                          haze_live, hzL, litM, darkM, a);
                float o1[3]; for (int c = 0; c < 3; ++c) o1[c] = a.c[c] + (1.0f - a.a) * under[c];
                const float dl = std::fabs(lum(o1) - lum(under)) * 255.0f;
                mx = std::max(mx, dl);
                if (a.a > 0.05f) { ++with; sdl += dl; }
            }
            std::printf("WCLOUD-LOOK AGC2 pose: %u rays with coverage, their mean dL %.1f/255, max dL %.1f/255\n", with, with ? sdl / with : 0.0, double(mx));
            CHECK(with > 50 && sdl / with > 15.0 && mx > 60.0f);
            // the haze still fades them: the same puffs seen 4x farther contribute less
            W::Params pfar = nw; unsigned wf = 0; double sf = 0;
            const float efar[3] = {101000.0f, 10600.0f + 4.0f * (eye[1] - 10600.0f), 101500.0f};
            for (int yd = 1; yd < 360; yd += 4) for (int pj = -4; pj <= 4; pj += 2) {
                const float pr = (-35.0f + float(pj)) * 3.14159265f / 180.0f, yr = float(yd) * 3.14159265f / 180.0f;
                const float d[3] = {std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
                W::Accum a; W::accumulate(pfar, efar, d, 0.0f, efar[1] / -d[1], 160000.0f, cov_bin, [&](float u, float v) { return cov_bin(u * 256.0f, v * 256.0f); },
                                          haze_live, hzL, litM, darkM, a);
                float o1[3]; for (int c = 0; c < 3; ++c) o1[c] = a.c[c] + (1.0f - a.a) * under[c];
                if (a.a > 0.05f) { ++wf; sf += std::fabs(lum(o1) - lum(under)) * 255.0f; }
            }
            CHECK(wf == 0 || sf / wf < 0.8 * sdl / with);
            // not discs: from straight above, the outline radius of a puff varies with the azimuth (a sphere law alone gives one radius)
            for (int k0 = 0; k0 < 3; ++k0) {
                int qi = k0, qk = 3;
                while (!(W::unit(W::hash(qi, qk, 5u)) < nw.fill)) ++qi;
                const W::Puff q = W::puff_in_cell(nw, qi, qk);
                float rmin = 2.0f, rmax = 0.0f;
                for (int k = 0; k < 16; ++k) {
                    const float th = 6.2831853f * float(k) / 16.0f; float last = 0.0f;
                    for (int m = 0; m < 100; ++m) {
                        const float rho = q.r * float(m) / 100.0f;
                        const float oa[3] = {q.c[0] + rho * std::cos(th), q.c[1] + 6000.0f, q.c[2] + rho * std::sin(th)}, dn[3] = {0, -1, 0};
                        W::Accum a; W::accumulate(nw, oa, dn, 0.0f, 3e38f, 160000.0f, [](float, float) { return 1.0f; },
                                                  [&](float u, float v) { return cov_bin(u * 256.0f, v * 256.0f); }, [](float) { return 0.0f; }, hzL, litM, darkM, a);
                        if (a.a > 0.0f) last = float(m) / 100.0f;
                    }
                    rmin = std::min(rmin, last); rmax = std::max(rmax, last);
                }
                std::printf("WCLOUD-LOOK outline of puff (%d,%d): radius %.2f..%.2f r\n", qi, qk, double(rmin), double(rmax));
                CHECK(rmin >= 0.7f && rmax - rmin >= 0.08f);
            }
        }
    }
    // ---- WCLOUD-FIX2 telemetry: wcdiag:2 (magenta proof tint, P[34].w) and the CPU probe behind event=summary-wcloud
    {
        std::string id, why;
        T::Conventions c1, c2;
        CHECK(T::parse_stage_option("m03k;env:1;wcloud:1;wcdiag:1", id, c1, why) && c1.wc_diag && !c1.wc_tint);
        CHECK(T::parse_stage_option("m03k;env:1;wcloud:1;wcdiag:2", id, c2, why) && c2.wc_diag && c2.wc_tint);
        { T::Conventions c3; CHECK(!T::parse_stage_option("m03k;wcdiag:3", id, c3, why)); }
        T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 9000.0f);
        std::vector<float> e1, e2, e0;
        T::build_env_texture(st, c1, e1); T::build_env_texture(st, c2, e2);
        CHECK(e1[size_t(W::kPWc2) * 4 + 3] == 0.0f && e2[size_t(W::kPWc2) * 4 + 3] == 1.0f);
        for (size_t i = 0; i < e1.size(); ++i) if (i != size_t(W::kPWc2) * 4 + 3) CHECK(e1[i] == e2[i]);   // the tint is the only difference
        CHECK(W::params_from(&e2[size_t(W::kPWc) * 4], &e2[size_t(W::kPWc2) * 4]).tint && !W::params_from(&e1[size_t(W::kPWc) * 4], &e1[size_t(W::kPWc2) * 4]).tint);
        T::WcAccum w; w.a = 0.2f; w.c[0] = 0.1f; w.c[1] = 0.1f; w.c[2] = 0.1f; w.tint = true;
        float rgb[3] = {0.1f, 0.1f, 0.1f};
        T::world_clouds_apply(w, rgb);
        CHECK(rgb[0] > 0.6f && rgb[2] > 0.6f && rgb[1] < 0.02f);   // A = 0.2 -> 80 % magenta: plainly visible
        T::WcAccum z; z.tint = true; float rgb0[3] = {0.1f, 0.2f, 0.3f}; T::world_clouds_apply(z, rgb0);
        CHECK(rgb0[0] == 0.1f && rgb0[1] == 0.2f && rgb0[2] == 0.3f);   // no coverage: unchanged even with the tint
        // the probe: eye above the synthetic deck looking 53 deg down (fwd (0, -0.8, -0.6)); off = nothing
        T::Atlas at; T::build_atlas(st, at);
        const T::EnvView ev{e1.data(), T::kEnvSky | T::kEnvCloud};
        const float peye[3] = {8192.0f, 16000.0f, 2000.0f}, base[3] = {-0.6f, 0.45f, -1.0f}, ux[3] = {1.2f, 0.0f, 0.0f}, vy[3] = {0.0f, -0.9f, 0.0f};
        const float right[3] = {1, 0, 0}, up[3] = {0, 0.6f, -0.8f}, fwd[3] = {0, -0.8f, -0.6f};
        const T::WcProbe pr = T::world_clouds_probe(st, at, ev, peye, base, ux, vy, right, up, fwd, T::kLookHazeL, 160000.0f);
        std::printf("probe (synthetic deck from y 16000, 53 deg down): rays %u with %u mean-a %.3f mean-dl %.2f vis8 %u\n", pr.rays, pr.with, double(pr.mean_a),
                    double(pr.mean_dl), pr.vis8);
        CHECK(pr.on && !pr.tint && pr.rays == 48 && pr.with > 0 && pr.max_a > 0.05f && pr.mean_dl > 0.0f && pr.alt == 9000.0f);
        T::build_env_texture(st, T::Conventions{}, e0);
        const T::EnvView ev0{e0.data(), T::kEnvSky | T::kEnvCloud};
        const T::WcProbe p0 = T::world_clouds_probe(st, at, ev0, peye, base, ux, vy, right, up, fwd, T::kLookHazeL, 160000.0f);
        CHECK(!p0.on && p0.rays == 0);
    }
    // ---- the CPU trace path with the synthetic environment (what the WARP harness compares the shader against)
    {
        T::StageData st = T::make_synthetic_stage(3, false); T::add_synthetic_env(st, 9000.0f);
        T::Conventions on; on.wcloud = true;
        std::vector<float> env; T::build_env_texture(st, on, env);
        const T::EnvView ev{env.data(), T::kEnvSky | T::kEnvCloud};
        unsigned with = 0; float maxa = 0;
        for (int r = 0; r < 2000; ++r) {
            const float o[3] = {8192.0f + (frand() - 0.5f) * 50000.0f, 6000.0f, 2000.0f + (frand() - 0.5f) * 50000.0f};
            float d[3] = {frand() - 0.5f, 0.05f + frand() * 0.5f, frand() - 0.5f}; norm(d);
            T::WcAccum a; T::world_clouds(ev, o, d, 0.0f, 3e38f, 55000.0f, 160000.0f, a);
            if (a.a > 0.0f) ++with;
            maxa = std::max(maxa, a.a);
            float rgb[3] = {0.2f, 0.3f, 0.4f}, rgb0[3] = {0.2f, 0.3f, 0.4f};
            T::world_clouds_apply(a, rgb);
            if (a.a == 0.0f) CHECK(std::memcmp(rgb, rgb0, sizeof rgb) == 0);
            else CHECK(rgb[0] != rgb0[0] || rgb[1] != rgb0[1]);
        }
        std::printf("cpu trace path: %u of 2000 rays below the deck meet puffs, max coverage %.3f\n", with, double(maxa));
        CHECK(with > 50 && maxa <= 1.0f);
        std::vector<float> env0; T::build_env_texture(st, T::Conventions{}, env0);
        const T::EnvView e0{env0.data(), T::kEnvSky | T::kEnvCloud};
        const float o[3] = {8192.0f, 6000.0f, 2000.0f}; float d[3] = {0.2f, 0.4f, 0.1f}; norm(d);
        T::WcAccum z; T::world_clouds(e0, o, d, 0.0f, 3e38f, 55000.0f, 160000.0f, z);
        CHECK(z.a == 0.0f && z.cells == 0);   // wcloud off: no code runs
    }
    // ---- producer: the game's puff quads (world edge >= the drop edge) are collapsed in the eye copies; debris is kept; the census line
    {
        const double E[3] = {1000.0, 5000.0, 2000.0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1};
        double A[16]; make_a(E, right, up, fwd, A);
        acz::fxmem::ScreenMap game; CHECK(FS::game_map_from_a(A, game));
        double eye[3]; CHECK(FS::game_eye(game, eye) && std::fabs(eye[0] - E[0]) < 1e-3 && std::fabs(eye[1] - E[1]) < 1e-3 && std::fabs(eye[2] - E[2]) < 1e-3);
        // quad 0: a 6000-unit puff 20,000 ahead; quad 1: a 40-unit speck 800 ahead
        const double P0[4][3] = {{-2000, 7000, -18000}, {4000, 7000, -18000}, {-2000, 1000, -18000}, {4000, 1000, -18000}};
        const double P1[4][3] = {{1000, 5000, 1200}, {1040, 5000, 1200}, {1000, 4960, 1200}, {1040, 4960, 1200}};
        acz::fxmem::Vtx v[8];
        for (int k = 0; k < 4; ++k) { v[k] = vtx_at(game, P0[k], float(k & 1), float(k >> 1)); v[4 + k] = vtx_at(game, P1[k], float(k & 1), float(k >> 1)); }
        const uint16_t idx[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
        uint8_t big[16] = {};
        CHECK(FS::mark_puff_quads(game, v, 8, idx, 9, 0, 2000.0, big) == 1u);
        CHECK(big[0] && big[1] && big[2] && big[3] && !big[4] && !big[7]);
        CHECK(FS::run_world_edge(game, v, 8, idx, 0, 4, 0) > 5990.0 && FS::run_world_edge(game, v, 8, idx, 5, 9, 0) < 41.0);
        const acz::fxmem::ScreenMap* eyes[2] = {&game, &game};
        acz::fxmem::Vtx scratch[8], k0[8], k1[8]; uint8_t bad[8];
        FS::Plan pl; pl.begin();
        FS::plan_draw(game, eyes, v, 8, idx, 9, 0, 1.0, scratch, bad, pl, k0, k1, nullptr, 2000.0);
        CHECK(pl.prepared == 1 && pl.wc_dropped == 1u);
        auto same = [](const acz::fxmem::Vtx& a, const acz::fxmem::Vtx& b) { return a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1]; };
        CHECK(same(k0[0], k0[3]) && same(k1[1], k1[2]));          // the puff: zero area in both eyes
        CHECK(!same(k0[4], k0[7]) && !same(k1[5], k1[6]));        // the speck: kept
        FS::Plan pk; pk.begin();
        FS::plan_draw(game, eyes, v, 8, idx, 9, 0, 1.0, scratch, bad, pk, k0, k1, nullptr, 0.0);
        CHECK(pk.prepared == 1 && pk.wc_dropped == 0u && !same(k0[0], k0[3]));   // drop edge 0: the previous behaviour
        char line[448];
        std::snprintf(line, sizeof line, "fx-sprite-totals;class=x"); FS::append_world_cloud_dropped(line, sizeof line, pl);
        CHECK(std::strstr(line, ";wcd=1") != nullptr);
        std::snprintf(line, sizeof line, "fx-sprite-totals;class=x"); FS::append_world_cloud_dropped(line, sizeof line, pk);
        CHECK(std::strcmp(line, "fx-sprite-totals;class=x") == 0);
        // census: one puff sample (world centre, edges, depth, uv box, no border), the speck is below 1000 units
        FS::PuffSample ps[FS::kPuffPerLine]; unsigned seen = 0;
        const unsigned n = FS::puff_samples(game, v, 8, idx, 9, 0, 1000.0, ps, FS::kPuffPerLine, seen);
        CHECK(n == 1 && seen == 1);
        CHECK(std::fabs(ps[0].c[0] - 1000.0f) < 1.0f && std::fabs(ps[0].c[1] - 4000.0f) < 1.0f && std::fabs(ps[0].c[2] + 18000.0f) < 1.0f);
        CHECK(std::fabs(ps[0].eu - 6000.0f) < 2.0f && std::fabs(ps[0].ev - 6000.0f) < 2.0f && std::fabs(ps[0].depth - 20000.0f) < 2.0f);
        CHECK(ps[0].uv[0] == 0.0f && ps[0].uv[1] == 1.0f && ps[0].border == 0);
        FS::format_puff_line(line, 426, 77, eye, ps, n, seen);
        CHECK(std::strcmp(line, "fx-cloud-puff;frame=77;eye=1000,5000,2000;n=1;p0=1000,4000,-18000,6000,6000,20000,uv:0.00-1.00/0.00-1.00,b0") == 0);
        // extreme values stay inside the step-1 text budget (426 bytes after the prefix)
        FS::PuffSample big4[FS::kPuffPerLine];
        for (auto& q : big4) { for (float& c : q.c) c = -8.9e9f; q.eu = q.ev = 1e30f; q.depth = -1e30f; for (float& u : q.uv) u = -1e9f; q.border = 1; }
        const double ex[3] = {-1e300, 1e300, NAN};
        char lb[426]; FS::format_puff_line(lb, sizeof lb, ~0ull, ex, big4, FS::kPuffPerLine, 123456u);
        CHECK(std::strlen(lb) < 426 && std::strstr(lb, ";p3=") != nullptr);
        // a quad clipped at the window border is flagged
        acz::fxmem::Vtx vc[4] = {v[0], v[1], v[2], v[3]}; vc[0].pos[0] = 1792.0f;
        FS::PuffSample pc[1]; unsigned sc = 0;
        CHECK(FS::puff_samples(game, vc, 4, idx, 4, 0, 1000.0, pc, 1, sc) == 1 && pc[0].border == 1);
    }
    // ---- W15 (w15-2 F1, w15-3 F1): a non-indexed class-1 triangle list / strip is ONE continuous GPU assembly. The collapse (a vertex behind one eye) and the puff
    // marking never infer 4-vertex groups from the length: a list of 12 is not three quads; DrawInstanced still draws triangle (3,4,5) across a group boundary.
    {
        const double E[3] = {1000.0, 5000.0, 2000.0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1};
        double A[16]; make_a(E, right, up, fwd, A);
        acz::fxmem::ScreenMap game; CHECK(FS::game_map_from_a(A, game));
        const double E0[3] = {1000.0, 5000.0, 900.0};   // eye 0 moved 1100 units forward: a vertex 1000 ahead of the game camera is BEHIND it
        double A0[16]; make_a(E0, right, up, fwd, A0);
        acz::fxmem::ScreenMap eye0; CHECK(FS::game_map_from_a(A0, eye0));
        // vertex k of a draw: a distinct, well spread world point 5000 ahead of the game camera (3900 ahead of eye 0); `edge` scales the spread
        auto build = [&](unsigned n, double edge, int behind, std::vector<acz::fxmem::Vtx>& v) {
            v.clear();
            for (unsigned k = 0; k < n; ++k) {
                const double P[3] = {1000.0 + edge * (0.31 * double(k % 5) + 0.17 * double(k)), 5000.0 + edge * (0.23 * double((k * 7) % 5) + 0.05 * double(k % 3)), -3000.0};
                double Q[3] = {P[0], P[1], int(k) == behind ? 1000.0 : P[2]};
                v.push_back(vtx_at(game, Q, float(k & 1), float((k >> 1) & 1)));
            }
        };
        auto same = [](const acz::fxmem::Vtx& a, const acz::fxmem::Vtx& b) { return a.pos[0] == b.pos[0] && a.pos[1] == b.pos[1] && a.pos[2] == b.pos[2] && a.pos[3] == b.pos[3]; };
        // twice the signed area of one assembled triangle (screen xy)
        auto area2 = [](const acz::fxmem::Vtx& a, const acz::fxmem::Vtx& b, const acz::fxmem::Vtx& c) {
            return double(b.pos[0] - a.pos[0]) * double(c.pos[1] - a.pos[1]) - double(c.pos[0] - a.pos[0]) * double(b.pos[1] - a.pos[1]);
        };
        // every triangle the GPU assembles from the copy under the game's topology: list (3i, 3i+1, 3i+2) or strip (i, i+1, i+2)
        auto triangles = [&](const std::vector<acz::fxmem::Vtx>& k, bool list, unsigned n, std::vector<double>& areas) {
            areas.clear();
            if (list) for (unsigned i = 0; i + 2 < n; i += 3) areas.push_back(area2(k[i], k[i + 1], k[i + 2]));
            else for (unsigned i = 0; i + 2 < n; ++i) areas.push_back(area2(k[i], k[i + 1], k[i + 2]));
        };
        struct Case { bool list; unsigned n; int behind; };
        const Case cases[] = {{true, 12, 3}, {true, 24, 3}, {true, 24, 7}, {false, 8, 3}, {false, 12, 3}, {false, 12, 7}};
        for (const Case& cs : cases) {
            std::vector<acz::fxmem::Vtx> v, scratch(cs.n), k0(cs.n), k1(cs.n); std::vector<uint8_t> bad(cs.n);
            std::vector<uint16_t> idx; for (unsigned i = 0; i < cs.n; ++i) idx.push_back(uint16_t(i));   // ONE run, no restarts (what the capture synthesises with niQuads == false)
            build(cs.n, 600.0, cs.behind, v);
            const acz::fxmem::ScreenMap* eyes[2] = {&eye0, &game};
            FS::Plan pl; pl.begin();
            FS::plan_draw(game, eyes, v.data(), cs.n, idx.data(), idx.size(), 0, 1.0, scratch.data(), bad.data(), pl, k0.data(), k1.data(), nullptr, 0.0, true);
            CHECK(pl.prepared == 1 && pl.refused == 0 && pl.behind >= 1 && pl.quads == 0);
            // eye 0 (vertex behind it): the WHOLE draw is zero area: every assembled triangle degenerate, nothing of the game's triangles left (incl. the one across the group boundary)
            std::vector<double> ar; triangles(k0, cs.list, cs.n, ar);
            bool all0 = true; for (double a : ar) all0 = all0 && a == 0.0;
            bool same0 = true; for (unsigned i = 1; i < cs.n; ++i) same0 = same0 && same(k0[i], k0[0]);
            CHECK(all0 && same0);
            // eye 1 (nothing behind it): the draw is untouched, every triangle keeps its area
            triangles(k1, cs.list, cs.n, ar);
            bool live = true; for (double a : ar) live = live && a != 0.0;
            CHECK(live && !same(k1[3], k1[0]));
        }
        // the puff drop (wcdrop) on the same shapes: ONE group [0..3] has a >= 2000 edge, the rest are small: the whole list/strip is one puff run -> collapsed in BOTH eyes
        for (const Case& cs : cases) {
            std::vector<acz::fxmem::Vtx> v, scratch(cs.n), k0(cs.n), k1(cs.n), small(cs.n); std::vector<uint8_t> bad(cs.n), big(cs.n);
            std::vector<uint16_t> idx; for (unsigned i = 0; i < cs.n; ++i) idx.push_back(uint16_t(i));
            build(cs.n, 40.0, -1, v);
            // group 0 spread over 8000 units, so only 0..3 carry a large edge; vertices 4.. keep the 40-unit spread
            for (unsigned k = 0; k < 4; ++k) { const double P[3] = {1000.0 + 6000.0 * double(k & 1) - 2000.0, 5000.0 + 5000.0 * double(k >> 1), -18000.0}; v[k] = vtx_at(game, P, float(k & 1), float(k >> 1)); }
            small = v; for (unsigned k = 0; k < 4; ++k) small[k] = vtx_at(game, std::array<double, 3>{1000.0 + 40.0 * k, 5000.0 + 20.0 * k, -3000.0}.data(), float(k & 1), float(k >> 1));
            const acz::fxmem::ScreenMap* eyes[2] = {&game, &game};
            CHECK(FS::mark_puff_quads(game, v.data(), cs.n, idx.data(), idx.size(), 0, 2000.0, big.data(), true) == 1u);
            bool allbig = true; for (unsigned i = 0; i < cs.n; ++i) allbig = allbig && big[i] == 1; CHECK(allbig);   // the whole run, never just [0..3]
            FS::Plan pl; pl.begin();
            FS::plan_draw(game, eyes, v.data(), cs.n, idx.data(), idx.size(), 0, 1.0, scratch.data(), bad.data(), pl, k0.data(), k1.data(), nullptr, 2000.0, true);
            CHECK(pl.prepared == 1 && pl.wc_dropped == 1u);
            std::vector<double> ar; bool all0 = true;
            triangles(k0, cs.list, cs.n, ar); for (double a : ar) all0 = all0 && a == 0.0;
            triangles(k1, cs.list, cs.n, ar); for (double a : ar) all0 = all0 && a == 0.0;
            CHECK(all0);
            // a small-edged draw of the same shape is kept (no puff): positions equal the plain re-projection, nothing dropped
            FS::Plan ps; ps.begin(); std::vector<acz::fxmem::Vtx> s0(cs.n), s1(cs.n);
            FS::plan_draw(game, eyes, small.data(), cs.n, idx.data(), idx.size(), 0, 1.0, scratch.data(), bad.data(), ps, s0.data(), s1.data(), nullptr, 2000.0, true);
            CHECK(ps.prepared == 1 && ps.wc_dropped == 0u && !same(s0[3], s0[0]) && !same(s1[5], s1[4]));
        }
        // indexed strips keep the per-quad rule (the game's own restarts): the whole_run switch is off for them
        {
            std::vector<acz::fxmem::Vtx> v; build(8, 600.0, -1, v);
            const uint16_t idx[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
            uint8_t bad[8] = {0, 0, 0, 1, 0, 0, 0, 0}; acz::fxmem::Vtx out[8]; for (int i = 0; i < 8; ++i) out[i] = v[size_t(i)];
            FS::ReprojectStats st; FS::collapse_quads(out, 8, bad, idx, 9, st);
            CHECK(st.quads_collapsed == 1 && same(out[1], out[0]) && !same(out[5], out[4]));
        }
    }
    // ---- W15 w15-3 F2: the puff suppression ends when the replacement GPU terrain is dead for the generation (the consumer calls withdraw_world_cloud_drop)
    {
        s5::apply_stage_effects(s5::stage_effects(6, true, 0, true, 2000.0f, false));
        CHECK(s5::effects_world_cloud_drop_edge().load() == 2000.0f);
        CHECK(s5::withdraw_world_cloud_drop() && s5::effects_world_cloud_drop_edge().load() == 0.0f);
        CHECK(!s5::withdraw_world_cloud_drop());   // idempotent
        // the producer reads the flag per captured draw: a puff quad is now kept (not collapsed)
        const double E[3] = {1000.0, 5000.0, 2000.0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1};
        double A[16]; make_a(E, right, up, fwd, A);
        acz::fxmem::ScreenMap game; CHECK(FS::game_map_from_a(A, game));
        const double P0[4][3] = {{-2000, 7000, -18000}, {4000, 7000, -18000}, {-2000, 1000, -18000}, {4000, 1000, -18000}};
        acz::fxmem::Vtx v[4], scratch[4], k0[4], k1[4]; uint8_t bad[4];
        for (int k = 0; k < 4; ++k) v[k] = vtx_at(game, P0[k], float(k & 1), float(k >> 1));
        const uint16_t idx[4] = {0, 1, 2, 3}; const acz::fxmem::ScreenMap* eyes[2] = {&game, &game};
        FS::Plan pl; pl.begin();
        FS::plan_draw(game, eyes, v, 4, idx, 4, 0, 1.0, scratch, bad, pl, k0, k1, nullptr, double(s5::effects_world_cloud_drop_edge().load()));
        CHECK(pl.prepared == 1 && pl.wc_dropped == 0u && !(k0[0].pos[0] == k0[3].pos[0] && k0[0].pos[1] == k0[3].pos[1]));
        // a stage re-install re-applies the edge (a healthy new generation suppresses again)
        s5::apply_stage_effects(s5::stage_effects(6, true, 0, true, 2000.0f, false));
        CHECK(s5::effects_world_cloud_drop_edge().load() == 2000.0f);
        s5::clear_stage_effects();
    }
    // ---- stage switches: the drop edge only with wcloud + env + fxmem:6; the census with fxmem:5/6; apply/clear
    {
        CHECK(W::producer_drop_edge(true, true, 6, 2000.0f) == 2000.0f);
        CHECK(W::producer_drop_edge(false, true, 6, 2000.0f) == 0.0f && W::producer_drop_edge(true, false, 6, 2000.0f) == 0.0f);
        CHECK(W::producer_drop_edge(true, true, 5, 2000.0f) == 0.0f && W::producer_drop_edge(true, true, 6, 0.0f) == 0.0f);
        auto e = s5::stage_effects(6, true, 0, true, 2000.0f, true);
        CHECK(e.wc_drop_edge == 2000.0f && e.wc_diag && e.sprite_gpu);
        CHECK(s5::stage_effects(5, true, 0, true, 2000.0f, true).wc_drop_edge == 0.0f && s5::stage_effects(5, true, 0, true, 2000.0f, true).wc_diag);
        CHECK(!s5::stage_effects(4, true, 0, true, 2000.0f, true).wc_diag);
        CHECK(s5::stage_effects(6, true).wc_drop_edge == 0.0f && !s5::stage_effects(6, true).wc_diag);   // the old call: unchanged
        s5::apply_stage_effects(e);
        CHECK(s5::effects_world_cloud_drop_edge().load() == 2000.0f && s5::effects_cloud_puff_diag_flag().load());
        s5::clear_stage_effects();
        CHECK(s5::effects_world_cloud_drop_edge().load() == 0.0f && !s5::effects_cloud_puff_diag_flag().load());
        char b[300];
        W::config_line(b, sizeof b, true, true, params(), 2000.0f, false);
        CHECK(std::strstr(b, "event=world-clouds;requested=1;active=yes;alt=9000;thick=3000;cell=12000;radius=3000;opacity=0.60;fill=0.80;alt-src=-;game-puff-drop-edge=2000") != nullptr);
        W::config_line(b, sizeof b, true, false, W::Params{}, 0.0f, false);
        CHECK(std::strstr(b, "active=no(needs-env:1-and-a-v3-stage)") != nullptr);
        W::config_line(b, sizeof b, false, true, params(), 0.0f, false);
        CHECK(b[0] == 0);
    }
    std::printf("world_clouds_offline: %u checks PASS\n", g_checks);
    return 0;
}
