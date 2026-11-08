#pragma once
// world_clouds.h -- WORLD-CLOUDS (xr_terrain_stage wcloud:1, default OFF; docs/world-render/world-clouds.md). Header-only, SDK-free CPU reference of the
// shader's wc_* functions in xr_stereo_blit.hlsl (constants mirrored 1:1 as kTWc*; operations in the same order).
//
// Why: the game submits its GREATER 4caf6e2d cloud puffs PRE-PROJECTED and ALREADY CLIPPED to its own window (effects-outside-window.md section 17.2, [E]
// clouds\runT2: 1,027 of 1,027 dumped puff draws touch the window border), so no per-eye replay of the game's draws can show a cloud outside the forward
// rectangle. WORLD-CLOUDS draws our OWN puff field in world space, per eye, in the terrain ray (depth-correct against the terrain, the props, the replayed
// objects and the cockpit), from the stage's environment data (cloud colours, coverage map, layer altitude) with every unknown named as an option.
//
// The field: a world grid of square cells (side `cell`, xz). Cell (i, k) holds at most ONE spherical puff whose centre/radius come from an integer hash of
// (i, k) (deterministic, the same on CPU and GPU, the same for both eyes and every frame: world-anchored). A cell holds a puff when its hash is below the fill
// fraction; the stage's coverage map at the centre (the WORLD-2 blob map, skytex 6) modulates its density (WCLOUD-FIX).
// Each puff lies wholly inside its cell's column (margin = its radius), so a ray that walks the cells it crosses in
// xz order (2D DDA) meets the puffs strictly front to back: the accumulation is exact over-compositing, no sorting. A ray only walks the cells while it is
// inside the altitude band that can hold a puff and before `t1` (the terrain hit, a nearer prop or a replayed object's ray distance).
// Per puff: alpha = dens * opacity * (1 - (b/r)^2)^kShapePow (b = the ray's closest approach to the centre), x a coverage-map noise at the closest point,
// x a near fade (no pop when flying through), coloured lit (top) to dark (bottom) with the stage's cloud colours by the height of the ray's ENTRY point into
// the sphere (WCLOUD-FIX2: a deck seen from above shows its lit tops), its colour hazed with WORLD-1's haze law at its distance (the alpha is NOT reduced by the
// haze a second time: WCLOUD-FIX2). Output: premultiplied DISPLAY colour C and coverage A, composited as decode(C + (1 - A) encode(under)).
// wcdiag:2 (P[kPWc2].w = 1) paints the coverage magenta instead (proves in one eye dump that the GPU branch runs: world_clouds_apply / wc_over).
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace acz::wcloud {

// Environment-texture texels (row 0; build_env_texture writes them only with wcloud:1, else zero = off):
//   P[kPWc]  = (altitude H, band thickness, cell side, 1 = on)
//   P[kPWc2] = (puff radius R, opacity, fill = the fraction of cells holding a puff, 0)
inline constexpr uint32_t kPWc = 33, kPWc2 = 34;

// Shape constants (mirrored as kTWc* in xr_stereo_blit.hlsl; INFERENCE, tuned to look like soft puffs; see the doc's calibration list).
inline constexpr float kCovLo = 0.2f, kCovHi = 0.6f;          // coverage map value at the centre -> density (smoothstep)
inline constexpr float kShapePow = 0.5f;                       // radial falloff exponent (WCLOUD-FIX: 1.5 -> 0.75; WCLOUD-FIX2: 0.5, a fuller body; doc sections 6-7)
inline constexpr float kDensFloor = 0.7f;                      // WCLOUD-FIX: an occupied cell's density = kDensFloor + (1 - kDensFloor) x smoothstep(coverage)
inline constexpr float kNoise = 0.3f, kNoiseRep = 8000.0f;     // WCLOUD-FIX2: 0.5 -> 0.3. Coverage-map noise at the closest point: alpha x ((1 - kNoise) + kNoise n), map repeat in units
inline constexpr float kNear0 = 250.0f, kNear1 = 1500.0f;      // near fade (ray distance of the closest approach)
inline constexpr float kAlphaStop = 0.996f;                    // front-to-back early out
// WCLOUD-LOOK (step140 magenta proof: hard discs, mean-dl 3.6/255). Irregular cumulus outline: body = sat(((1 - (b/r)^2) - kEdge (1 - e)) / (1 - kEdge)),
// e = two octaves of the coverage map at the closest point (repeats kEdgeRep1/2 units, weights kEdgeW1 / 1 - kEdgeW1, offset per cell by hashes 1/2): the rim
// lies between 0.74 r (e = 0) and r (e = 1), in lumps. Lit tops = the stage's lit colour x kLitGain (clamped to 1). Puffs haze at kHazeScale x their distance
// (bright clouds keep contrast through the look:1 haze, L = 21500; the haze still takes them toward the horizon colour with distance).
inline constexpr float kEdge = 0.45f, kEdgeRep1 = 60000.0f, kEdgeRep2 = 21000.0f, kEdgeW1 = 0.6f, kLitGain = 1.3f, kHazeScale = 0.5f;
inline constexpr float kRadiusMin = 0.6f;                      // r = R (kRadiusMin + (1 - kRadiusMin) h)
inline constexpr int kMaxCells = 40;                           // DDA cap per ray (a level ray inside the band crosses ~14-20 cells to the march end)
inline constexpr float kMapSize = 256.0f;                      // coverage map texels per repeat

// Option defaults and limits (Conventions wc_*; parse_stage_option).
// WCLOUD-FIX (step138, doc section 6): cell 12000 -> 7500 (= 2.5 x wcsize, the densest legal grid), opacity 0.6 -> 0.9, and a fill fraction 0.8 that no longer
// depends on the binary coverage map (m03k: 18.8 % of its texels set, so only ~22 % of the cells held a puff and the field covered < 3 % of the view).
inline constexpr float kDefThick = 3000.0f, kDefSize = 3000.0f, kDefCell = 7500.0f, kDefOpacity = 0.9f, kDefDropEdge = 2000.0f, kDefFill = 0.8f;
inline constexpr float kCellPerSize = 2.5f;                    // cell >= 2.5 R (the puff must fit its cell with a margin)

inline uint32_t hash(int32_t x, int32_t z, uint32_t k) noexcept {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(z) * 0xd8163841u ^ k * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
inline float unit(uint32_t h) noexcept { return float(h >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
inline float smooth(float a, float b, float x) noexcept { float t = (x - a) / (b - a); t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); return t * t * (3.0f - 2.0f * t); }
inline float sat(float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

struct Params {
    bool on = false;
    float alt = 0, thick = 0, cell = 0, radius = 0, opacity = 0, fill = 0;
    bool tint = false;   // wcdiag:2: the diagnostic magenta coverage (P[kPWc2].w)
};
// From the two texels (nullptr / w == 0 / non-finite or degenerate = off).
inline Params params_from(const float* p, const float* p2) noexcept {
    Params r;
    if (!p || !p2 || !(p[3] > 0.5f)) return r;
    r.alt = p[0]; r.thick = p[1]; r.cell = p[2]; r.radius = p2[0]; r.opacity = p2[1]; r.fill = p2[2]; r.tint = p2[3] > 0.5f;
    r.on = std::isfinite(r.alt) && std::isfinite(r.thick) && r.thick >= 0.0f && r.cell > 0.0f && r.radius > 0.0f && r.cell >= 2.0f * r.radius && r.opacity > 0.0f &&
           r.fill > 0.0f && r.fill <= 1.0f;
    return r;
}
// WCLOUD-FIX: the puff band the game's OWN clouds use, per stage, measured by the wcdiag census (fx-cloud-puff lines: the unprojected centres of the GREATER
// 4caf6e2d puff quads whose atlas v range ends <= 0.8). The layer-0 field P[kPCloudAlt].x is NOT that altitude [E]: m03k's reads 6000 while its puffs sit at
// 8.8 k..13.2 k; m02a's reads 16000 while its puffs sit at 5.5 k..13.6 k. Band = (p10 + p90) / 2, thickness = p90 - p10 of the centres.
struct Band { float alt = 0, thick = 0; };
inline bool stage_band(const char* stage_id, Band& out) noexcept {
    if (!stage_id) return false;
    struct Row { const char* id; float alt, thick; };
    static const Row kRows[] = {
        {"m03k", 10600.0f, 4400.0f},   // [E] step137 batchAG runAGC1 + runAGC2 census, 918 puffs: p10 8.8 k, p50 10.6 k, p90 13.2 k (eye 12 k..20 k)
        {"m02a", 9500.0f, 8000.0f},    // [E] clouds/runT2 fxsig unprojection, 1,027 puffs: p10 5,530, p90 13,586 (world-clouds.md section 1)
    };
    for (const Row& r : kRows) {
        size_t i = 0;
        while (r.id[i] && stage_id[i] == r.id[i]) ++i;
        if (!r.id[i] && !stage_id[i]) { out.alt = r.alt; out.thick = r.thick; return true; }
    }
    return false;
}
// Which source sets the band (logged as alt-src=): the wcalt option, the stage census table, or the layer-0 field (the old default; an unmeasured stage).
inline const char* band_source(const char* stage_id, float alt_option) noexcept {
    Band b;
    return alt_option >= -1e8f ? "option" : (stage_band(stage_id, b) ? "stage-census" : "layer0");
}
// Writes the two texels (8 floats) for build_env_texture. on = wcloud:1 AND the stage has an environment. alt < -1e8 = the stage band (stage_band of
// `stage_id`), else `layer_alt` (P[kPCloudAlt].x); thick < 0 = the stage band's thickness, else kDefThick.
inline void param_texels(bool on, const char* stage_id, float alt, float layer_alt, float thick, float cell, float radius, float opacity, float fill,
                         float* out8, bool tint = false) noexcept {
    for (int i = 0; i < 8; ++i) out8[i] = 0.0f;
    if (!on) return;
    Band b;
    const bool known = stage_band(stage_id, b);
    out8[0] = alt >= -1e8f ? alt : (known ? b.alt : layer_alt);
    out8[1] = thick >= 0.0f ? thick : (known ? b.thick : kDefThick);
    out8[2] = cell; out8[3] = 1.0f;
    out8[4] = radius; out8[5] = opacity; out8[6] = fill; out8[7] = tint ? 1.0f : 0.0f;
}

struct Puff { float c[3] = {}; float r = 0; };
// WCLOUD-FIX: the density of cell (i, k)'s puff from the coverage map value `cov` at its centre: 0 for an empty cell (hash 5 >= fill), else
// kDensFloor .. 1 (the map only modulates; it no longer decides which cells hold a puff).
inline float density(const Params& p, int32_t i, int32_t k, float cov) noexcept {
    return unit(hash(i, k, 5u)) < p.fill ? kDensFloor + (1.0f - kDensFloor) * smooth(kCovLo, kCovHi, cov) : 0.0f;
}
// Geometry of cell (i, k)'s puff (density not included).
inline Puff puff_in_cell(const Params& p, int32_t i, int32_t k) noexcept {
    Puff q;
    const float f1 = unit(hash(i, k, 1u)), f2 = unit(hash(i, k, 2u)), f3 = unit(hash(i, k, 3u)), f4 = unit(hash(i, k, 4u));
    q.r = p.radius * (kRadiusMin + (1.0f - kRadiusMin) * f4);
    const float span = p.cell - 2.0f * q.r;
    q.c[0] = float(i) * p.cell + q.r + f1 * span;
    q.c[2] = float(k) * p.cell + q.r + f2 * span;
    q.c[1] = p.alt + (f3 - 0.5f) * p.thick;
    return q;
}

struct Accum { float c[3] = {0, 0, 0}; float a = 0; unsigned cells = 0, puffs = 0; };
// The puffs met by the ray o + t d (d normalised), t in [max(t0, 0), min(t1, far)], front to back.
//   cov(x, z)          -> coverage map value (0..1) at world (x, z)       (bilinear, wrapping: the shader's te_map channel 0 at u = x / rep * 256)
//   noise(u, v)        -> the same map at texel coordinates (u, v)        (bilinear, wrapping)
//   haze(t)            -> haze fraction at distance t (haze_fraction)
//   hz[3]              -> the haze colour along d (display)
//   lit[3], dark[3]    -> cloud colours (display, P[kPCloudCol + 1] / + 2)
template <class Cov, class Noise, class Haze>
inline void accumulate(const Params& p, const float o[3], const float d[3], float t0, float t1, float far_dist, Cov&& cov, Noise&& noise, Haze&& haze,
                       const float hz[3], const float lit[3], const float dark[3], Accum& acc) noexcept {
    if (!p.on) return;
    const float tlo = t0 > 0.0f ? t0 : 0.0f, thi = t1 < far_dist ? t1 : far_dist;
    if (!(thi > tlo)) return;
    const float ylo = p.alt - 0.5f * p.thick - p.radius, yhi = p.alt + 0.5f * p.thick + p.radius;
    float ts = tlo, te = thi;
    if (std::fabs(d[1]) > 1e-6f) {
        const float ta = (ylo - o[1]) / d[1], tb = (yhi - o[1]) / d[1];
        ts = std::fmax(ts, std::fmin(ta, tb)); te = std::fmin(te, std::fmax(ta, tb));
    } else if (o[1] < ylo || o[1] > yhi) return;
    if (!(te > ts)) return;
    const float C = p.cell;
    const float px = o[0] + d[0] * ts, pz = o[2] + d[2] * ts;
    int32_t ci = int32_t(std::floor(px / C)), ck = int32_t(std::floor(pz / C));
    const int32_t si = d[0] > 0.0f ? 1 : -1, sk = d[2] > 0.0f ? 1 : -1;
    const bool mx = std::fabs(d[0]) > 1e-9f, mz = std::fabs(d[2]) > 1e-9f;
    const float tdi = mx ? C / std::fabs(d[0]) : 1e30f, tdk = mz ? C / std::fabs(d[2]) : 1e30f;
    float tmi = mx ? ts + (float(ci + (d[0] > 0.0f ? 1 : 0)) * C - px) / d[0] : 1e30f;
    float tmk = mz ? ts + (float(ck + (d[2] > 0.0f ? 1 : 0)) * C - pz) / d[2] : 1e30f;
    for (int n = 0; n < kMaxCells; ++n) {
        ++acc.cells;
        const Puff q = puff_in_cell(p, ci, ck);
        const float dens = density(p, ci, ck, cov(q.c[0], q.c[2]));
        if (dens > 0.0f) {
            const float oc[3] = {q.c[0] - o[0], q.c[1] - o[1], q.c[2] - o[2]};
            const float tc = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
            const float b2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - tc * tc;
            const float r2 = q.r * q.r;
            if (tc > tlo && tc < thi && b2 < r2) {
                const float pp[3] = {o[0] + d[0] * tc, o[1] + d[1] * tc, o[2] + d[2] * tc};
                const float h1 = unit(hash(ci, ck, 1u)) * kMapSize, h2 = unit(hash(ci, ck, 2u)) * kMapSize;   // WCLOUD-LOOK: per-cell noise offset
                const float e = kEdgeW1 * noise(pp[0] / kEdgeRep1 * kMapSize + h1, (pp[2] + pp[1]) / kEdgeRep1 * kMapSize + h2) +
                                (1.0f - kEdgeW1) * noise(pp[0] / kEdgeRep2 * kMapSize + h2, (pp[2] + pp[1]) / kEdgeRep2 * kMapSize + h1);
                const float body = sat(((1.0f - b2 / r2) - kEdge * (1.0f - e)) / (1.0f - kEdge));
                float a = dens * p.opacity * std::pow(body, kShapePow);
                const float nn = noise(pp[0] / kNoiseRep * kMapSize, (pp[2] + pp[1]) / kNoiseRep * kMapSize);
                a *= (1.0f - kNoise) + kNoise * nn;
                a *= smooth(kNear0, kNear1, tc);
                const float ty = o[1] + d[1] * (tc - std::sqrt(r2 - b2));   // WCLOUD-FIX2: the entry point's height
                const float s = sat(0.5f + 0.5f * (ty - q.c[1]) / q.r);
                const float f = haze(tc * kHazeScale);   // WCLOUD-LOOK
                float col[3];
                for (int c = 0; c < 3; ++c) { const float lt = std::fmin(lit[c] * kLitGain, 1.0f), cc = dark[c] + (lt - dark[c]) * s; col[c] = cc + (hz[c] - cc) * f; }
                const float w = (1.0f - acc.a) * a;
                for (int c = 0; c < 3; ++c) acc.c[c] += w * col[c];
                acc.a += w;
                if (a > 0.0f) ++acc.puffs;
                if (acc.a > kAlphaStop) break;
            }
        }
        const float tn = tmi < tmk ? tmi : tmk;
        if (!(tn < te)) break;
        if (tmi < tmk) { ci += si; tmi += tdi; } else { ck += sk; tmk += tdk; }
    }
}

// ---- producer side (fxmem:6): the game's own puffs leave the per-eye sprite replay while our field is drawn (no double, no rectangle edge).
// The edge (world units) above which a GREATER 4caf6e2d class-1 quad counts as a cloud puff (section 17.2: puffs 3,800-7,500; debris tens) and is collapsed
// in the eye copies. 0 = off. Only with wcloud:1 AND an environment AND fxmem:6 (the per-eye replay exists only then).
inline float producer_drop_edge(bool wcloud, bool env_present, int fx_memory, float drop_edge) noexcept {
    return (wcloud && env_present && fx_memory == 6 && drop_edge > 0.0f) ? drop_edge : 0.0f;
}
// The xr-stereo log line for one stage install (buf >= 256).
// alt_src (band_source) is appended as ;alt-src= when given.
inline void config_line(char* b, size_t n, bool wcloud, bool env_present, const Params& p, float drop_edge, bool diag, const char* alt_src = nullptr) noexcept {
    if (!b || !n) return;
    if (!wcloud) { b[0] = 0; return; }
    std::snprintf(b, n, "xr-stereo;event=world-clouds;requested=1;active=%s;alt=%.0f;thick=%.0f;cell=%.0f;radius=%.0f;opacity=%.2f;fill=%.2f;alt-src=%s;"
        "game-puff-drop-edge=%.0f;diag=%d;doc=docs/world-render/world-clouds.md", env_present && p.on ? "yes" : (env_present ? "no(bad-params)" : "no(needs-env:1-and-a-v3-stage)"),
        double(p.alt), double(p.thick), double(p.cell), double(p.radius), double(p.opacity), double(p.fill), alt_src ? alt_src : "-", double(drop_edge), diag ? 1 : 0);
}

} // namespace acz::wcloud
