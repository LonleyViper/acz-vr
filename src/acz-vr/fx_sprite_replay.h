#pragma once
// fx_sprite_replay.h -- EFFECTS-OUTSIDE (D) CPU core of the head-pose replay of the game's ps2_v SPRITE effects (explosion candidate PS 542a11d7, the
// GREATER 4caf6e2d sprites incl. tracer-like streaks); docs/world-render/effects-outside-window.md section 9.3. Header only (needs fx_memory.cpp). No D3D12.
//
// Route "CPU-expanded" (the sprite analogue of the smoke route S2): the game submits these effects as PRE-PROJECTED ps2_v quads (64-byte vertices
// x_s, y_s, z_s, q | COLOR0 | TEXCOORD | COLOR1, VS 534f29ca: SV_Position = ((x-2048)*2/512, (y-2048)*2/-448, z*2^-24, 1) / q). There is no world data to
// substitute, so each vertex is turned back into a world point with the frame's A (fx_memory unproject: z form d = Z0/(z-b), q form d = 1/q) and projected
// again with the EYE's A'_e (the same A + D_e x S the world objects and the smoke use, which maps the eye frustum onto the same GS window). The replay then
// re-issues the game's draw (its PSO, root arguments, index buffer, textures) with a VERTEX BUFFER COPY holding the re-projected vertices, into a per-eye
// FX image tested against that eye's objects depth. Colour, uv, the second colour and the PS are the game's: the explosion keeps its own look.
//
// Rules (fail closed; every refusal is named):
//  * a vertex without a world depth (z = 0 and q not in (0,1): a flat 2D overlay) makes the whole draw refused ("sprite-no-depth"): it is not a world effect;
//  * z form: z' = depth_scale x (Z0 / d' + b) (the objects' depth with the depth fix); q is perspective only and is scaled by d / d' (q' = q d / d');
//    q form: z' stays 0, q' = 1 / d';
//  * a vertex behind the eye or nearer than `near` collapses EVERY 4-vertex quad that uses it to a point (zero area: the rasteriser draws nothing),
//    since the replay keeps the game's index buffer; counted;
//  * everything outside the eye frustum is clipped by the rasteriser (the eye A'_e maps the eye frustum onto the GS window, viewport = the eye FX image).
// Limits: the game only draws sprites inside ITS window, so an explosion the aircraft's camera never saw cannot be shown (route + the particle memory of
// section 4 for persistence); each quad stays a planar card facing the GAME camera (correct position, size and depth; for the small eye offsets of a
// head turn inside the game's view it is indistinguishable from a billboard).
#include "fx_memory.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::fxsprite {

// ---- header-only copies of the fx_memory.cpp maths this module needs (the DLL does not link fx_memory.cpp; fx_census_offline cross-checks them against it)
inline bool solve3(const double* a, const double r[3], double x[3]) noexcept {   // rows = A columns 0, 1, 3 over the xyz rows
    const int col[3] = {0, 1, 3}; double n[3][3];
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) n[i][k] = a[k * 4 + col[i]];
    auto det3 = [](const double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                                                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
    const double det = det3(n);
    if (!(std::fabs(det) > 1e-300) || !std::isfinite(det)) return false;
    for (int k = 0; k < 3; ++k) { double m[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[i][j] = j == k ? r[i] : n[i][j]; x[k] = det3(m) / det; }
    return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}
// The game map from the frame's A (same checks as fxmem::screen_map_from_a: finite, depth column = b x w column, a plausible per-view depth model b / Z0); eye/fwd are not filled.
inline bool game_map_from_a(const double a16[16], fxmem::ScreenMap& m) noexcept {
    m = fxmem::ScreenMap{};
    for (int i = 0; i < 16; ++i) { if (!std::isfinite(a16[i])) return false; m.a[i] = a16[i]; }
    double bs = 0, ws = 0;
    for (int r = 0; r < 3; ++r) { bs += m.a[r * 4 + 2] * m.a[r * 4 + 3]; ws += m.a[r * 4 + 3] * m.a[r * 4 + 3]; }
    if (!(ws > 1e-30)) return false;
    const double b = bs / ws; double res = 0, ref = 0;
    for (int r = 0; r < 3; ++r) { const double e = m.a[r * 4 + 2] - b * m.a[r * 4 + 3]; res += e * e; ref += m.a[r * 4 + 2] * m.a[r * 4 + 2]; }
    if (res > 1e-8 * (ref + 1e-30)) return false;
    const double z0 = m.a[14] - b * m.a[15];
    if (!fxmem::depth_model_ok(b, z0)) return false;   // per-view depth model (cockpit b -255 / Z0 2^24; chase + HUD-only b -1341..-1475 / Z0 8.8e7..9.7e7)
    m.b = b; m.z0 = z0; m.ok = true;
    return true;
}
inline bool sp_project(const fxmem::ScreenMap& m, const double p[3], double& xs, double& ys, double& depth) noexcept {
    double h[4];
    for (int c = 0; c < 4; ++c) h[c] = p[0] * m.a[c] + p[1] * m.a[4 + c] + p[2] * m.a[8 + c] + m.a[12 + c];
    depth = h[3];
    if (!(h[3] > 1e-9)) return false;
    xs = h[0] / h[3]; ys = h[1] / h[3];
    return std::isfinite(xs) && std::isfinite(ys);
}
inline bool sp_unproject(const fxmem::ScreenMap& m, double xs, double ys, double depth, double p[3]) noexcept {
    if (!m.ok || !(depth > 0) || !std::isfinite(depth) || !std::isfinite(xs) || !std::isfinite(ys)) return false;
    const double r[3] = {xs * depth - m.a[12], ys * depth - m.a[13], depth - m.a[15]};
    return solve3(m.a, r, p);
}
inline double sp_vertex_depth(const fxmem::ScreenMap& m, float z, float w, fxmem::DepthSource& src) noexcept {
    src = fxmem::depth_none;
    if (std::isfinite(z) && z > 0.0f) { const double d = m.z0 / (double(z) - m.b); if (d > 0 && std::isfinite(d)) { src = fxmem::depth_z; return d; } return 0; }
    if (std::isfinite(w) && w > 0.0f && w < 1.0f) { src = fxmem::depth_q; return 1.0 / double(w); }
    return 0;
}

struct ReprojectStats {
    uint32_t vertices = 0, z_form = 0, q_form = 0, no_depth = 0, unproject_fail = 0, behind_eye = 0, quads = 0, quads_collapsed = 0, bad_index = 0;
    uint32_t runs_collapsed = 0;   // (K) non-quad runs (length not a multiple of 4) collapsed WHOLE because one of their vertices is unusable
};

// Re-project `n` ps2_v vertices from the game map to the eye map into `out` (may not alias `in`). `bad` (n flags, may be nullptr) marks vertices behind
// the eye. nullptr = done (call collapse_quads next when stats.behind_eye > 0); else the refusal (out undefined).
inline const char* reproject(const fxmem::ScreenMap& game, const fxmem::ScreenMap& eye, const fxmem::Vtx* in, size_t n, fxmem::Vtx* out, double depth_scale,
                             double near_units, ReprojectStats& st, uint8_t* bad) noexcept {
    st = ReprojectStats{};
    if (!game.ok) return "sprite-game-map-invalid";
    if (!eye.ok) return "sprite-eye-map-invalid";
    if (!in || !out || n == 0) return "sprite-no-vertices";
    if (!(depth_scale > 0) || !std::isfinite(depth_scale)) return "sprite-depth-scale";
    for (size_t i = 0; i < n; ++i) {
        const fxmem::Vtx& v = in[i];
        fxmem::Vtx& o = out[i];
        o = v;
        if (bad) bad[i] = 0;
        ++st.vertices;
        fxmem::DepthSource src;
        const double d = sp_vertex_depth(game, v.pos[2], v.pos[3], src);
        if (src == fxmem::depth_none || !(d > 0)) { ++st.no_depth; return "sprite-no-depth"; }
        src == fxmem::depth_z ? ++st.z_form : ++st.q_form;
        double p[3];
        if (!sp_unproject(game, v.pos[0], v.pos[1], d, p)) { ++st.unproject_fail; return "sprite-unproject"; }
        double xs = 0, ys = 0, d2 = 0;
        if (!sp_project(eye, p, xs, ys, d2) || !(d2 > near_units)) {
            ++st.behind_eye; if (bad) bad[i] = 1;
            continue;   // position left as the game's; the quad is collapsed below
        }
        o.pos[0] = float(xs); o.pos[1] = float(ys);
        if (src == fxmem::depth_z) {
            o.pos[2] = float(depth_scale * fxmem::game_z_for_depth(eye, d2));   // the EYE map's depth model (= the game view's b / Z0)
            if (std::isfinite(v.pos[3]) && v.pos[3] > 0.0f) o.pos[3] = float(double(v.pos[3]) * d / d2);
        } else {
            o.pos[2] = 0.0f; o.pos[3] = float(1.0 / d2);
        }
        if (!std::isfinite(o.pos[0]) || !std::isfinite(o.pos[1]) || !std::isfinite(o.pos[2]) || !std::isfinite(o.pos[3])) { ++st.unproject_fail; return "sprite-non-finite"; }
    }
    return nullptr;
}

// Collapse every 4-vertex strip quad (indices with 0xFFFF restarts, or a non-indexed list of consecutive 4-vertex strips when idx == nullptr) that uses a
// `bad` vertex: all its vertices get the first vertex's position (zero area). Counts quads / collapsed / bad indices into st.
// (K, docs section 17) A run whose length is not a multiple of 4 (a plain strip of 3, 5, 6 .. vertices, or the one run of a non-indexed class-1 list) is
// still counted in bad_index (it is not a quad list) and, when ANY of its vertices is bad, the WHOLE run collapses to its first vertex (zero area: nothing
// drawn) instead of being left with a vertex behind the eye (before (K) such a run kept its bad vertices: a stretched triangle across the view).
// W15 (w15-2 F1 / w15-3 F1): `whole_run` = the index list is ONE continuous GPU assembly (a non-indexed class-1 triangle list or strip drawn by DrawInstanced from
// vertex 0, no real restarts): the length is never read as "groups of 4" (a list of 12 or a strip of 8 is not three / two quads: the GPU draws triangle (3,4,5)
// across the group boundary), every run is the whole-run rule: any bad vertex collapses the WHOLE draw.
inline void collapse_quads(fxmem::Vtx* out, size_t n, const uint8_t* bad, const uint16_t* idx, size_t ni, ReprojectStats& st, bool whole_run = false) noexcept {
    if (!out || !bad) return;
    auto quad = [&](const uint32_t q[4]) {
        ++st.quads;
        for (int k = 0; k < 4; ++k) if (q[k] >= n) { ++st.bad_index; return; }
        if (!(bad[q[0]] || bad[q[1]] || bad[q[2]] || bad[q[3]])) return;
        ++st.quads_collapsed;
        for (int k = 1; k < 4; ++k) for (int c = 0; c < 4; ++c) out[q[k]].pos[c] = out[q[0]].pos[c];
    };
    if (!idx) { for (size_t i = 0; i + 3 < n; i += 4) { const uint32_t q[4] = {uint32_t(i), uint32_t(i + 1), uint32_t(i + 2), uint32_t(i + 3)}; quad(q); } return; }
    size_t s = 0;
    while (s < ni) {
        size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e;
        const size_t len = e - s;
        if (!whole_run && len % 4 == 0) for (size_t k = s; k + 3 < e; k += 4) { const uint32_t q[4] = {idx[k], idx[k + 1], idx[k + 2], idx[k + 3]}; quad(q); }
        else if (len) {
            ++st.bad_index;
            bool anyBad = false, inRange = true;
            for (size_t k = s; k < e; ++k) { if (idx[k] >= n) { inRange = false; break; } if (bad[idx[k]]) anyBad = true; }
            if (inRange && anyBad) {
                ++st.runs_collapsed;
                for (size_t k = s + 1; k < e; ++k) for (int c = 0; c < 4; ++c) out[idx[k]].pos[c] = out[idx[s]].pos[c];
            }
        }
        s = e + 1;
    }
}

// (P, docs section 21) a POINT-list run (D3D topology 1, no GS: one pixel per vertex) has no area to collapse: a point left at the game's position would still
// light a pixel there. Every `bad` point (behind the eye / a dropped vertex) is moved to GS (0, 0): ps2_v maps it to NDC (-8, +9.1), outside every viewport, so
// the rasteriser clips it. Colour, uv, z and q are untouched. Returns the culled count. Never applied to strips / lists (their collapse is collapse_quads).
inline constexpr float kPointCullX = 0.0f, kPointCullY = 0.0f;
inline unsigned cull_points(fxmem::Vtx* out, size_t n, const uint8_t* bad) noexcept {
    if (!out || !bad) return 0;
    unsigned c = 0;
    for (size_t i = 0; i < n; ++i) if (bad[i]) { out[i].pos[0] = kPointCullX; out[i].pos[1] = kPointCullY; ++c; }
    return c;
}

// ------------------------------------------------------------------ minimum width of thin strip quads (gun tracers; docs section 12.4)
// The gun tracer is a thin far-away streak: re-projected to the eye image (2560 x 1440 for the 512 x 448 GS window: 5 x 3.2 eye pixels per GS pixel) a quad
// only ~0.2 GS px wide covers no pixel centre and the rasteriser draws nothing. widen_thin_quads grows such a quad ACROSS its short side to `min_px` eye pixels
// (about its centre line; length, depth, q, colours and uv untouched), when the maths allow: the quad needs a long side (>= min_px, else it is a speck, not a
// streak: left alone) and finite coordinates. Collapsed quads (zero area) stay collapsed. 4-vertex strip order: a=(0,0) b=(1,0) c=(0,1) d=(1,1); U = ab / cd, V = ac / bd.
struct Widen {
    double gx = 0.0, gy = 0.0, min_px = 1.0;   // GS units per eye pixel (x, y) = 512 / image_w, 448 / image_h; the minimum width in eye pixels
    bool on() const noexcept { return gx > 0.0 && gy > 0.0 && std::isfinite(gx) && std::isfinite(gy) && min_px > 0.0; }
};
inline unsigned widen_thin_quads(fxmem::Vtx* v, size_t n, const uint16_t* idx, size_t ni, const Widen& w) noexcept {
    if (!v || !idx || !w.on()) return 0;
    unsigned widened = 0;
    auto widen_quad = [&](const uint32_t q[4]) {
        for (int k = 0; k < 4; ++k) if (q[k] >= n) return;
        fxmem::Vtx* p[4] = {&v[q[0]], &v[q[1]], &v[q[2]], &v[q[3]]};
        double px[4], py[4];
        for (int k = 0; k < 4; ++k) {
            px[k] = double(p[k]->pos[0]) / w.gx; py[k] = double(p[k]->pos[1]) / w.gy;
            if (!std::isfinite(px[k]) || !std::isfinite(py[k])) return;
        }
        auto len = [&](int i, int j) { return std::hypot(px[j] - px[i], py[j] - py[i]); };
        const double lu = 0.5 * (len(0, 1) + len(2, 3)), lv = 0.5 * (len(0, 2) + len(1, 3));
        const bool shortU = lu <= lv;                    // the short side pair is U (a-b, c-d): the quad extends along V
        const double llong = shortU ? lv : lu;
        if (!(llong >= w.min_px)) return;   // a speck (no long side): left alone
        // long direction from the sum of the two long edges; the width direction is its perpendicular
        const double lx = shortU ? (px[2] - px[0]) + (px[3] - px[1]) : (px[1] - px[0]) + (px[3] - px[2]);
        const double ly = shortU ? (py[2] - py[0]) + (py[3] - py[1]) : (py[1] - py[0]) + (py[3] - py[2]);
        const double ln = std::hypot(lx, ly);
        if (!(ln > 1e-12)) return;
        const double wx = -ly / ln, wy = lx / ln;
        // the two sides of the short pair: (a,c) vs (b,d) when the short pair is U, (a,b) vs (c,d) when it is V; signed separation along (wx, wy)
        const int s0a = 0, s0b = shortU ? 2 : 1, s1a = shortU ? 1 : 2, s1b = 3;
        // each END of the quad (a pair across the short side) gets at least min_px of width: perspective makes the far end narrower than the near one
        const double sepA = (px[s1a] - px[s0a]) * wx + (py[s1a] - py[s0a]) * wy, sepB = (px[s1b] - px[s0b]) * wx + (py[s1b] - py[s0b]) * wy;
        const double sgn = sepA + sepB < 0.0 ? -1.0 : 1.0;
        bool did = false;
        auto grow = [&](int i0, int i1, double sep) {
            if (sgn * sep >= w.min_px * 0.995) return;   // already wide enough (0.5% slack: float rounding keeps this idempotent)
            const double delta = 0.5 * (w.min_px * sgn - sep);   // push each vertex of the pair outwards by this many pixels along (wx, wy)
            const double dx = wx * delta * w.gx, dy = wy * delta * w.gy;
            p[i0]->pos[0] -= float(dx); p[i0]->pos[1] -= float(dy); p[i1]->pos[0] += float(dx); p[i1]->pos[1] += float(dy);
            did = true;
        };
        grow(s0a, s1a, sepA); grow(s0b, s1b, sepB);
        if (did) ++widened;
    };
    size_t s = 0;
    while (s < ni) {
        size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e;
        if ((e - s) % 4 == 0) for (size_t k = s; k + 3 < e; k += 4) { const uint32_t q[4] = {idx[k], idx[k + 1], idx[k + 2], idx[k + 3]}; widen_quad(q); }
        s = e + 1;
    }
    return widened;
}

// ------------------------------------------------------------------ fxmem:5 capture + CPU dry run (docs section 10)
// The eye matrix A'_e = A + D_e x S (fx_smoke.h eye_a_from_delta) keeps A's x / y / w columns in the screen form but its z column is not checked (the eye
// depth is recomputed from w); an eye map therefore only needs finite values and a non-zero w column.
// `like` = the game map of the same frame: the eye shares its depth model (b, Z0: the eye A' = A + D x S keeps the z column b x w and the Z0 translation), which
// is per view (HUD-only / chase differ from the cockpit's 2^24); nullptr = the cockpit constants.
inline bool eye_map_from_a(const double a16[16], fxmem::ScreenMap& m, const fxmem::ScreenMap* like = nullptr) noexcept {
    m = fxmem::ScreenMap{};
    if (like && like->ok) { m.b = like->b; m.z0 = like->z0; }
    for (int i = 0; i < 16; ++i) { if (!std::isfinite(a16[i])) return false; m.a[i] = a16[i]; }
    if (!(std::fabs(m.a[3]) + std::fabs(m.a[7]) + std::fabs(m.a[11]) > 1e-12)) return false;
    m.ok = true;
    return true;
}
// Bounds of one captured draw (the producer refuses anything larger BEFORE reading: counted, the game's draw untouched).
inline constexpr uint32_t kMaxIndices = 1536, kMaxVertices = 512, kVertexStride = 64;
// The referenced vertex range of an indexed strip draw (16-bit indices, 0xFFFF = restart). false = no index / all restarts.
inline bool index_range(const uint16_t* idx, size_t n, uint32_t& lo, uint32_t& hi) noexcept {
    lo = 0xFFFFFFFFu; hi = 0;
    for (size_t i = 0; i < n; ++i) { if (idx[i] == 0xFFFF) continue; if (idx[i] < lo) lo = idx[i]; if (idx[i] > hi) hi = idx[i]; }
    return lo != 0xFFFFFFFFu;
}
// One frame's dry-run totals over every captured draw of one class (explosion = PS 542a11d7; sprite = the indexed GREATER 4caf6e2d draws, the tracer candidates).
// Quad shape (tracer discrimination, section 10.4): each 4-vertex quad is unprojected with the game map; aspect = longer / shorter world edge (v0-v1 vs
// v0-v2). A STREAK is a quad with aspect >= kStreakAspect (the stored-frame tracer segments: 152 units long, a few units wide); its mean COLOR0, its view
// depth and its centre's lateral offset from the view axis are accumulated (a gun tracer starts near the aircraft and runs along the view axis).
inline constexpr double kStreakAspect = 3.0;
struct Plan {
    uint32_t draws = 0, read = 0, refused = 0, prepared = 0, vertices = 0, quads = 0, in_window = 0, outside_window = 0, z_form = 0, q_form = 0, no_depth = 0;
    uint32_t eye_visible[2] = {}, eye_only = 0, behind = 0, collapsed = 0, widened = 0;   // widened: tracer quads grown to the minimum width (both eyes)
    uint32_t nonindexed = 0; double quad_len_max = 0;   // (J) non-indexed draws captured (class 1/2) and the longest world quad edge (cloud puff vs speck): `;ni=N;qm=L` on the plan line
    // (K) non-indexed draws refused by the shape / topology rule (gpu_copy_step5.h effects_sprite_nonindexed_shape) and the last one's vertex count and
    // D3D topology: `;nir=N(c:C,t:T)` on the class's fx-sprite-totals line (only when N > 0), so a live line names the shape that stays out of the per-eye replay.
    uint32_t ni_refused = 0, ni_last_count = 0, ni_last_topology = 0;
    void note_nonindexed_refused(uint32_t count, uint32_t topology) noexcept { ++ni_refused; ni_last_count = count; ni_last_topology = topology; }
    uint32_t streaks = 0; double streak_rgb[3] = {}; double near_depth = 0, streak_near = 0, streak_len_max = 0;
    uint32_t points = 0, points_culled = 0;   // (P) fxadd:1: point-list vertices prepared / culled behind an eye (both eyes): `;pts=N(cull:C)` on fx-sprite-totals
    uint32_t wc_dropped = 0;   // WORLD-CLOUDS (world_clouds.h): quads/runs of this frame collapsed in the eye copies as cloud puffs (world edge >= the drop edge)
    const char* last_why = nullptr; const char* world_why = nullptr;
    void begin() noexcept { *this = Plan{}; }
};
// Shape statistics of the draw's quads (needs the game map; vertices without a depth are skipped).
inline void quad_shapes(const fxmem::ScreenMap& game, const fxmem::Vtx* v, size_t n, const uint16_t* idx, size_t ni, uint32_t lo, Plan& p) noexcept {
    if (!game.ok) return;
    auto world = [&](const fxmem::Vtx& x, double w[3], double& d) {
        fxmem::DepthSource s; d = sp_vertex_depth(game, x.pos[2], x.pos[3], s);
        return s != fxmem::depth_none && sp_unproject(game, x.pos[0], x.pos[1], d, w);
    };
    size_t s = 0;
    while (s < ni) {
        size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e;
        if ((e - s) % 4 == 0) for (size_t k = s; k + 3 < e; k += 4) {
            const uint32_t q[3] = {uint32_t(idx[k] - lo), uint32_t(idx[k + 1] - lo), uint32_t(idx[k + 2] - lo)};
            if (q[0] >= n || q[1] >= n || q[2] >= n) continue;
            double w0[3], w1[3], w2[3], d0, d1, d2;
            if (!world(v[q[0]], w0, d0) || !world(v[q[1]], w1, d1) || !world(v[q[2]], w2, d2)) continue;
            auto len = [](const double* a, const double* b) { const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return std::sqrt(x * x + y * y + z * z); };
            const double l1 = len(w0, w1), l2 = len(w0, w2), lo2 = std::fmin(l1, l2), hi2 = std::fmax(l1, l2);
            const double d = (d0 + d1 + d2) / 3.0;
            if (p.near_depth == 0 || d < p.near_depth) p.near_depth = d;
            if (hi2 > p.quad_len_max) p.quad_len_max = hi2;
            if (!(lo2 > 0) || hi2 / lo2 < kStreakAspect) continue;
            ++p.streaks;
            for (int c = 0; c < 3; ++c) p.streak_rgb[c] += (v[q[0]].col[c] + v[q[1]].col[c] + v[q[2]].col[c]) / 3.0;
            if (p.streak_near == 0 || d < p.streak_near) p.streak_near = d;
            if (hi2 > p.streak_len_max) p.streak_len_max = hi2;
        }
        s = e + 1;
    }
}
// ---- WORLD-CLOUDS (world_clouds.h, docs/world-render/world-clouds.md): the game's cloud-puff quads among the class-1 (GREATER 4caf6e2d) draws.
// The longest world edge of a run (a 4-vertex quad: v0-v1 / v0-v2 like quad_shapes; any other run: consecutive vertices), unprojected with the game map.
// < 0 when a vertex has no world point. idx are the draw's indices (not re-based), lo the first referenced vertex.
inline double run_world_edge(const fxmem::ScreenMap& game, const fxmem::Vtx* v, size_t n, const uint16_t* idx, size_t s, size_t e, uint32_t lo) noexcept {
    double w[3][3]; double best = 0;
    auto world = [&](uint32_t q, double out[3]) {
        if (q < lo || q - lo >= n) return false;
        fxmem::DepthSource src; const fxmem::Vtx& x = v[q - lo]; const double d = sp_vertex_depth(game, x.pos[2], x.pos[3], src);
        return src != fxmem::depth_none && sp_unproject(game, x.pos[0], x.pos[1], d, out);
    };
    auto len = [](const double* a, const double* b) { const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return std::sqrt(x * x + y * y + z * z); };
    if (e - s == 4) {
        if (!world(idx[s], w[0]) || !world(idx[s + 1], w[1]) || !world(idx[s + 2], w[2])) return -1.0;
        return std::fmax(len(w[0], w[1]), len(w[0], w[2]));
    }
    if (e - s < 2 || !world(idx[s], w[0])) return -1.0;
    for (size_t k = s + 1; k < e; ++k) {
        if (!world(idx[k], w[1])) return -1.0;
        best = std::fmax(best, len(w[0], w[1]));
        for (int c = 0; c < 3; ++c) w[0][c] = w[1][c];
    }
    return best;
}
// Marks (big[re-based vertex] = 1) every quad / run whose longest world edge >= min_edge; returns the number of marked quads/runs. Needs the game map.
inline uint32_t mark_puff_quads(const fxmem::ScreenMap& game, const fxmem::Vtx* v, size_t n, const uint16_t* idx, size_t ni, uint32_t lo, double min_edge,
                                uint8_t* big, bool whole_run = false) noexcept {   // whole_run: see collapse_quads (never infer 4-vertex groups from the length)
    if (!game.ok || !v || !idx || !big || !(min_edge > 0.0)) return 0;
    uint32_t marked = 0;
    auto mark = [&](size_t s, size_t e) {
        const double L = run_world_edge(game, v, n, idx, s, e, lo);
        if (!(L >= min_edge)) return;
        ++marked;
        for (size_t k = s; k < e; ++k) if (idx[k] >= lo && idx[k] - lo < n && idx[k] - lo < kMaxVertices) big[idx[k] - lo] = 1;
    };
    size_t s = 0;
    while (s < ni) {
        size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e;
        if (e > s && !whole_run && (e - s) % 4 == 0) for (size_t k = s; k < e; k += 4) mark(k, k + 4);
        else if (e > s) mark(s, e);
        s = e + 1;
    }
    return marked;
}
// The game camera position from the map (the point with h.x = h.y = h.w = 0).
inline bool game_eye(const fxmem::ScreenMap& m, double eye[3]) noexcept {
    if (!m.ok) return false;
    const double r[3] = {-m.a[12], -m.a[13], -m.a[15]};
    return solve3(m.a, r, eye);
}
// The read-only puff census (wcdiag:1, `fx-cloud-puff` lines): the game's puff quads with their WORLD centre, edges, view depth, uv box and whether the game
// clipped them at its window (a vertex on the GS border x 1792/2304, y 1824/2272). Decides live (a) whether the game's puffs are static in the world (a
// persistence route), (b) their altitude / size / spacing per stage (the calibration of wcloud's wcalt/wcthk/wcsize/wccell), (c) whether the clip moved the uv.
struct PuffSample { float c[3] = {}; float eu = 0, ev = 0, depth = 0; float uv[4] = {}; uint8_t border = 0; };
inline constexpr unsigned kPuffPerLine = 4;
inline unsigned puff_samples(const fxmem::ScreenMap& game, const fxmem::Vtx* v, size_t n, const uint16_t* idx, size_t ni, uint32_t lo, double min_edge,
                             PuffSample* out, unsigned cap, unsigned& seen, bool whole_run = false) noexcept {
    if (!game.ok || !v || !idx || !out) return 0;
    unsigned k = 0;
    size_t s = 0;
    auto len = [](const double* a, const double* b) { const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return std::sqrt(x * x + y * y + z * z); };
    while (s < ni) {
        size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e;
        for (size_t q = s; e > s && !whole_run && (e - s) % 4 == 0 && q < e; q += 4) {
            double w[4][3]; double dsum = 0; bool ok = true; float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f; uint8_t border = 0;
            for (int j = 0; j < 4 && ok; ++j) {
                const uint16_t ix = idx[q + size_t(j)];
                if (ix < lo || ix - lo >= n) { ok = false; break; }
                const fxmem::Vtx& x = v[ix - lo];
                fxmem::DepthSource src; const double d = sp_vertex_depth(game, x.pos[2], x.pos[3], src);
                ok = src != fxmem::depth_none && sp_unproject(game, x.pos[0], x.pos[1], d, w[j]);
                dsum += d;
                u0 = std::fmin(u0, x.uv[0]); u1 = std::fmax(u1, x.uv[0]); v0 = std::fmin(v0, x.uv[1]); v1 = std::fmax(v1, x.uv[1]);
                if (std::fabs(x.pos[0] - 1792.0f) < 0.01f || std::fabs(x.pos[0] - 2304.0f) < 0.01f || std::fabs(x.pos[1] - 1824.0f) < 0.01f || std::fabs(x.pos[1] - 2272.0f) < 0.01f) border = 1;
            }
            if (!ok) continue;
            const double eu = len(w[0], w[1]), ev = len(w[0], w[2]);
            if (!(std::fmax(eu, ev) >= min_edge)) continue;
            ++seen;
            if (k >= cap) continue;
            PuffSample& o = out[k++];
            for (int c = 0; c < 3; ++c) o.c[c] = float((w[0][c] + w[1][c] + w[2][c] + w[3][c]) * 0.25);
            o.eu = float(eu); o.ev = float(ev); o.depth = float(dsum * 0.25);
            o.uv[0] = u0; o.uv[1] = u1; o.uv[2] = v0; o.uv[3] = v1; o.border = border;
        }
        s = e + 1;
    }
    return k;
}
// One line (< 426 bytes for kPuffPerLine samples with any finite values; offline-tested): eye = the game camera.
inline void format_puff_line(char* b, size_t n, uint64_t frame, const double eye[3], const PuffSample* p, unsigned count, unsigned seen) noexcept {
    if (!b || !n) return;
    auto cl = [](double x, double lim) { return std::isfinite(x) ? (x > lim ? lim : (x < -lim ? -lim : x)) : 0.0; };
    int k = std::snprintf(b, n, "fx-cloud-puff;frame=%llu;eye=%.0f,%.0f,%.0f;n=%u", static_cast<unsigned long long>(frame), cl(eye[0], 9e6), cl(eye[1], 9e6), cl(eye[2], 9e6),
                          seen < 9999u ? seen : 9999u);
    if (k < 0 || size_t(k) >= n) { b[n - 1] = 0; return; }
    size_t len = size_t(k);
    for (unsigned i = 0; i < count && i < kPuffPerLine; ++i) {
        const PuffSample& q = p[i];
        char t[96];
        const int r = std::snprintf(t, sizeof t, ";p%u=%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,uv:%.2f-%.2f/%.2f-%.2f,b%u", i, cl(q.c[0], 9e6), cl(q.c[1], 9e6), cl(q.c[2], 9e6),
                                    cl(q.eu, 999999), cl(q.ev, 999999), cl(q.depth, 9e6), cl(q.uv[0], 9), cl(q.uv[1], 9), cl(q.uv[2], 9), cl(q.uv[3], 9), unsigned(q.border));
        if (r <= 0 || size_t(r) >= sizeof t || len + size_t(r) >= n) break;
        std::memcpy(b + len, t, size_t(r) + 1); len += size_t(r);
    }
}
// `;wcd=N` on the class's fx-sprite-totals line (absent when nothing was dropped).
inline void append_world_cloud_dropped(char* b, size_t n, const Plan& p) noexcept {
    if (!b || !n || !p.wc_dropped) return;
    const size_t l = std::strlen(b);
    if (l + 12 < n) std::snprintf(b + l, n - l, ";wcd=%u", p.wc_dropped < 9999u ? p.wc_dropped : 9999u);
}
// One captured draw: `v` = the referenced vertices (index lo .. hi, re-based so that idx[i] - lo addresses v), `idx` the draw's indices. eyes[e] may be
// nullptr (no world camera: counted as refused with `world_why`). `scratch` holds n vertices (the eye copy; it is what the GPU replay would upload).
// fxmem:6 (fx_sprite_gpu.h): keep0 / keep1 (n vertices each, may be nullptr) receive eye 0 / eye 1's final copy (re-projected + collapsed): what the GPU
// replay uploads. They are written only for a draw that ends PREPARED (p.prepared incremented); on a refusal their content is undefined.
inline void plan_draw(const fxmem::ScreenMap& game, const fxmem::ScreenMap* const eyes[2], const fxmem::Vtx* v, size_t n, const uint16_t* idx, size_t ni,
                      uint32_t lo, double depth_scale, fxmem::Vtx* scratch, uint8_t* bad, Plan& p, fxmem::Vtx* keep0 = nullptr,
                      fxmem::Vtx* keep1 = nullptr, const Widen* widen = nullptr, double drop_edge = 0.0, bool whole_run = false,
                      const uint8_t* extra_drop = nullptr,   // (M) extra_drop: n flags, vertices of game quads a stored puff replaces (fx_puff_trail.h)
                      bool points = false) noexcept {        // (P) a POINT-list run: no quads, no puff marking, no widening; bad points culled (cull_points)
    if (points) { whole_run = true; drop_edge = 0.0; extra_drop = nullptr; widen = nullptr; }
    ++p.draws;
    if (!v || !n || !scratch || !bad) { ++p.refused; p.last_why = "sprite-no-vertices"; return; }
    p.vertices += uint32_t(n);
    for (size_t i = 0; i < n; ++i) {
        fxmem::in_window(v[i].pos[0], v[i].pos[1]) ? ++p.in_window : ++p.outside_window;
        fxmem::DepthSource s; sp_vertex_depth(game, v[i].pos[2], v[i].pos[3], s);
        if (s == fxmem::depth_z) ++p.z_form; else if (s == fxmem::depth_q) ++p.q_form; else ++p.no_depth;
    }
    // quads of the draw (indices re-based to the copied range)
    // (whole_run: ONE continuous assembly, no quads: the quad statistics and the streak shapes do not apply to it)
    uint32_t quads = 0; if (!whole_run) { size_t s = 0; while (s < ni) { size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e; if ((e - s) % 4 == 0) quads += uint32_t((e - s) / 4); s = e + 1; } }
    p.quads += quads;
    if (!whole_run) quad_shapes(game, v, n, idx, ni, lo, p);
    if (!eyes[0] || !eyes[1]) { ++p.refused; p.last_why = p.world_why ? p.world_why : "sprite-world-camera-unavailable"; return; }
    bool eyeOnly[kMaxVertices] = {};
    uint8_t big[kMaxVertices] = {};   // WORLD-CLOUDS: vertices of the game's cloud-puff quads (drop_edge > 0 only), collapsed in both eye copies
    const uint32_t dropped = drop_edge > 0.0 ? mark_puff_quads(game, v, n, idx, ni, lo, drop_edge, big, whole_run) : 0u;
    bool anyDrop = dropped != 0;
    if (extra_drop) for (size_t i = 0; i < n && i < kMaxVertices; ++i) if (extra_drop[i]) { big[i] = 1; anyDrop = true; }
    for (int e = 0; e < 2; ++e) {
        ReprojectStats st;
        if (const char* why = reproject(game, *eyes[e], v, n, scratch, depth_scale, 1.0, st, bad)) { ++p.refused; p.last_why = why; return; }
        if (anyDrop) for (size_t i = 0; i < n && i < kMaxVertices; ++i) bad[i] = uint8_t(bad[i] | big[i]);
        // re-based indices for the collapse
        uint16_t loc[kMaxIndices]; const size_t m = ni < kMaxIndices ? ni : kMaxIndices;
        for (size_t i = 0; i < m; ++i) loc[i] = idx[i] == 0xFFFF ? uint16_t(0xFFFF) : uint16_t(idx[i] - lo);
        if (points) p.points_culled += cull_points(scratch, n, bad);   // (P) no area: move the bad points off every viewport instead
        else collapse_quads(scratch, n, bad, loc, m, st, whole_run);
        p.behind += st.behind_eye; p.collapsed += st.quads_collapsed + st.runs_collapsed;
        if (widen && widen->on()) p.widened += widen_thin_quads(scratch, n, loc, m, *widen);   // (G) tracer: keep >= min_px eye pixels of width
        if (fxmem::Vtx* keep = e == 0 ? keep0 : keep1) for (size_t i = 0; i < n && i < kMaxVertices; ++i) keep[i] = scratch[i];
        for (size_t i = 0; i < n && i < kMaxVertices; ++i) {
            if (bad[i] || !fxmem::in_window(scratch[i].pos[0], scratch[i].pos[1])) continue;
            ++p.eye_visible[e];
            if (!fxmem::in_window(v[i].pos[0], v[i].pos[1])) eyeOnly[i] = true;
        }
    }
    for (size_t i = 0; i < n && i < kMaxVertices; ++i) p.eye_only += eyeOnly[i];
    p.wc_dropped += dropped;
    if (points) p.points += uint32_t(n);
    ++p.prepared;
}
inline void format_plan(char* b, size_t n, uint64_t frame, const char* cls, const Plan& p, unsigned skipped) noexcept {
    if (!b || !n) return;
    char why[32]; std::snprintf(why, sizeof why, "%s", p.last_why ? p.last_why : "none");
    const double k = p.streaks ? 1.0 / p.streaks : 0.0;
    std::snprintf(b, n, "fx-sprite-plan;frame=%llu;class=%s;draws=%u;read=%u;prep=%u;refused=%u;vtx=%u;quads=%u;in-win=%u;out-win=%u;z=%u;q=%u;no-depth=%u;"
        "eye-vis=%u,%u;eye-only=%u;behind=%u;collapsed=%u;near=%.0f;streaks=%u(rgb:%.2f,%.2f,%.2f;near:%.0f;len:%.0f);last-why=%s;skipped=%u",
        static_cast<unsigned long long>(frame), cls ? cls : "?", p.draws, p.read, p.prepared, p.refused, p.vertices, p.quads, p.in_window, p.outside_window, p.z_form,
        p.q_form, p.no_depth, p.eye_visible[0], p.eye_visible[1], p.eye_only, p.behind, p.collapsed, p.near_depth, p.streaks, p.streak_rgb[0] * k, p.streak_rgb[1] * k,
        p.streak_rgb[2] * k, p.streak_near, p.streak_len_max, why, skipped);
    if (p.widened) { const size_t l = std::strlen(b); if (l + 16 < n) std::snprintf(b + l, n - l, ";wide=%u", p.widened); }
    if (p.nonindexed) { const size_t l = std::strlen(b); if (l + 32 < n) std::snprintf(b + l, n - l, ";ni=%u;qm=%.0f", p.nonindexed < 9999u ? p.nonindexed : 9999u, p.quad_len_max < 999999.0 ? p.quad_len_max : 999999.0); }
}
// (K) `;nir=N(c:C,t:T)` appended to the class's fx-sprite-totals line (the plan line is already at the text cap): non-indexed draws of the pending frame refused by
// the shape / topology rule and the last one's count / D3D topology. Nothing is appended when none was refused (the line stays byte-identical).
inline void append_nonindexed_refused(char* b, size_t n, const Plan& p) noexcept {
    if (!b || !n || !p.ni_refused) return;
    const size_t l = std::strlen(b);
    if (l + 28 < n) std::snprintf(b + l, n - l, ";nir=%u(c:%u,t:%u)", p.ni_refused < 9999u ? p.ni_refused : 9999u, p.ni_last_count < 99999u ? p.ni_last_count : 99999u,
                                  p.ni_last_topology < 99u ? p.ni_last_topology : 99u);
}
// (P) `;pts=N(cull:C)` appended to the class's fx-sprite-totals line: point-list vertices of the pending frame prepared per eye and culled behind an eye (both
// eyes counted). Nothing is appended when the frame had no point run (fxadd off: always absent, the line stays byte-identical).
inline void append_points(char* b, size_t n, const Plan& p) noexcept {
    if (!b || !n || !p.points) return;
    const size_t l = std::strlen(b);
    if (l + 26 < n) std::snprintf(b + l, n - l, ";pts=%u(cull:%u)", p.points < 99999u ? p.points : 99999u, p.points_culled < 99999u ? p.points_culled : 99999u);
}

} // namespace acz::fxsprite
