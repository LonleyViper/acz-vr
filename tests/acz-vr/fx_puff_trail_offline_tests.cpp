// EFFECTS-OUTSIDE (M) offline tests: fx_puff_trail.h, the world-space memory of the game's smoke puffs (docs/world-render/effects-outside-window.md section 19).
// Quad analysis (whole puff / clipped at the GS border / streak / cloud-sized / the live clipped quad of run AGC1), the memory rules (store, refresh, the
// hand-over once the game camera no longer shows the card whole (M4), select once per frame, eye billboard re-projection with the z form and the age fade,
// collapse of the game's clipped part, cull of a puff the game stopped drawing in view, expiry, eviction, behind the eye, unselect), plan_draw's extra_drop,
// the fx-puff-trail line, the options and the stage switches. The M4 rules (cloud class, dedupe, cross-key hosts, fades, nohost): fx_puff_cloud_offline.
#include "fx_puff_trail.h"
#include "terrain_stage.h"
#include "effects_stage_flags.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace P = acz::fxpuff;
namespace FS = acz::fxsprite;
namespace M = acz::fxmem;
namespace T = acz::terrain;
namespace s5 = acz::gpu_copy::step5;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// A game map (the form of fx_memory ScreenMap) for a camera at E: GS window 512 x 448 around 2048, half tangents 0.6152 / 0.346 (as world_clouds_offline).
static void make_a(const double E[3], const double right[3], const double up[3], const double fwd[3], double A[16]) {
    const double tx = 0.6152, ty = 0.346, b = M::kDepthB, z0 = M::kZ0;
    for (int r = 0; r < 3; ++r) {
        A[r * 4 + 0] = 2048.0 * fwd[r] + (256.0 / tx) * right[r];
        A[r * 4 + 1] = 2048.0 * fwd[r] - (224.0 / ty) * up[r];
        A[r * 4 + 3] = fwd[r];
        A[r * 4 + 2] = b * A[r * 4 + 3];
    }
    for (int c : {0, 1, 3}) { double s = 0; for (int r = 0; r < 3; ++r) s += E[r] * A[r * 4 + c]; A[12 + c] = -s; }
    A[14] = z0 + b * A[15];
}
struct Cam { double E[3], R[3], U[3], F[3]; };
static M::ScreenMap game_map(const Cam& c) { double A[16]; make_a(c.E, c.R, c.U, c.F, A); M::ScreenMap m; CHECK(FS::game_map_from_a(A, m)); return m; }
static M::ScreenMap eye_map(const Cam& c) { double A[16]; make_a(c.E, c.R, c.U, c.F, A); M::ScreenMap m; CHECK(FS::eye_map_from_a(A, m)); return m; }
static const Cam kFwdZ = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};    // looks +z
static const Cam kFwdX = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};   // looks +x (a 90 degree turn right)
static const Cam kBackZ = {{0, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}; // looks -z

static M::Vtx vtx_at(const M::ScreenMap& m, const double P3[3], float u, float v, float alpha = 0.5f) {
    double xs, ys, d; CHECK(FS::sp_project(m, P3, xs, ys, d));
    M::Vtx x{}; x.pos[0] = float(xs); x.pos[1] = float(ys); x.pos[2] = float(m.z0 / d + m.b); x.pos[3] = 1.0f; x.uv[0] = u; x.uv[1] = v;
    x.col[0] = x.col[1] = x.col[2] = 0.3f; x.col[3] = alpha;
    return x;
}
// A camera-facing card of `cam` at world centre c, half extents hx (along cam.R) / hy (along cam.U), strip order a b c d = top-left, top-right, bottom-left,
// bottom-right on the game screen, every vertex at the CENTRE's depth (a constant-depth card, as the game's puffs).
static void card(const M::ScreenMap& m, const Cam& cam, const double c[3], double hx, double hy, M::Vtx out[4]) {
    const double sx[4] = {-1, 1, -1, 1}, sy[4] = {1, 1, -1, -1};
    double xs, ys, dc; CHECK(FS::sp_project(m, c, xs, ys, dc));
    for (int k = 0; k < 4; ++k) {
        double w[3]; for (int i = 0; i < 3; ++i) w[i] = c[i] + sx[k] * hx * cam.R[i] + sy[k] * hy * cam.U[i];
        out[k] = vtx_at(m, w, sx[k] > 0 ? 1.0f : 0.0f, sy[k] > 0 ? 0.0f : 1.0f);
        out[k].pos[2] = float(m.z0 / dc + m.b);   // the card's one depth
    }
}
static P::Quad analyse(const M::ScreenMap& m, const M::Vtx v[4], const P::Options& o) { const M::Vtx* q[4] = {&v[0], &v[1], &v[2], &v[3]}; return P::analyse_quad(m, q, o); }
static double dist(const double* a, const double* b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

int main() {
    const P::Options o = P::options(8.0, 1500.0, P::kClsSize);   // the memory mechanics under the M1 size rule (M3 near rule: its own block)
    CHECK(o.on && o.life_ms == 8000.0 && o.max_edge == 1500.0);
    CHECK(!P::options(0.0, 1500.0).on && P::options(0.0, 0.0).max_edge == P::kDefMaxEdge);
    const M::ScreenMap g0 = game_map(kFwdZ), g1 = game_map(kFwdX);
    const double Pc[3] = {0, 0, 5000};

    // ---- one quad: whole puff, clipped, streak, cloud-sized, the live AGC1 clipped quad
    {
        M::Vtx v[4]; card(g0, kFwdZ, Pc, 100, 100, v);
        const P::Quad q = analyse(g0, v, o);
        CHECK(q.ok && q.constant && !q.clipped && q.puff && q.zform);
        CHECK(dist(q.c, Pc) < 0.5 && std::fabs(q.hu - 100) < 0.5 && std::fabs(q.hv - 100) < 0.5 && std::fabs(q.edge - 200) < 1.0);
        CHECK(q.sx[0] < 0 && q.sx[1] > 0 && q.sy[0] < 0 && q.sy[2] > 0);   // a = top-left, c = bottom-left (screen y down)
        M::Vtx s[4]; card(g0, kFwdZ, Pc, 300, 50, s);
        CHECK(!analyse(g0, s, o).puff);                                    // streak (aspect 6)
        M::Vtx big[4]; card(g0, kFwdZ, Pc, 1000, 1000, big);
        CHECK(analyse(g0, big, o).ok && !analyse(g0, big, o).puff);        // a cloud puff (edge 2000 > 1500)
        M::Vtx cl[4]; card(g0, kFwdZ, Pc, 100, 100, cl);
        for (auto& x : cl) x.pos[0] = std::fmax(x.pos[0] - 300.0f, 1792.0f);   // pushed against the left border and clipped there by the game
        const P::Quad qc = analyse(g0, cl, o);
        CHECK(qc.ok && qc.clipped && !qc.puff);
        // run AGC1 (step137) f=34 n=1: v0=1792,2204.16,278,1 v1=1854.64,2204.16 v2=1792,2272 v3=1854.64,2272 (fx-sig-draw): on the border
        M::Vtx live[4] = {};
        const float lx[4] = {1792.0f, 1854.64f, 1792.0f, 1854.64f}, ly[4] = {2204.16f, 2204.16f, 2272.0f, 2272.0f};
        for (int k = 0; k < 4; ++k) { live[k].pos[0] = lx[k]; live[k].pos[1] = ly[k]; live[k].pos[2] = 278.0f; live[k].pos[3] = 1.0f; }
        const P::Quad ql = analyse(g0, live, o);
        CHECK(ql.ok && ql.constant && ql.clipped && !ql.puff);
        CHECK(P::on_border(1792.0, 2000.0) && P::on_border(2000.0, 2272.0) && P::on_border(1700.0, 2000.0) && !P::on_border(1800.0, 2000.0));
    }
    // ---- the basis of a map (orthonormal; = the camera's right / up / forward)
    {
        double R[3], U[3], F[3];
        CHECK(P::basis(g1, R, U, F));
        for (int i = 0; i < 3; ++i) { CHECK(std::fabs(R[i] - kFwdX.R[i]) < 1e-9); CHECK(std::fabs(U[i] - kFwdX.U[i]) < 1e-9); CHECK(std::fabs(F[i] - kFwdX.F[i]) < 1e-9); }
    }
    // ---- the memory: store, hand-over, select once per frame, eye re-projection, collapse, cull, expiry
    {
        static P::Memory mem;
        const uint64_t K = 0x15678a0012a280ull, K2 = 0x15678a0012a2a0ull;
        const P::Host HK{K}, HK2{K2};
        M::Vtx v[4]; card(g0, kFwdZ, Pc, 100, 100, v);
        uint16_t idx[4] = {0, 1, 2, 3};
        uint8_t col[FS::kMaxVertices] = {};
        uint16_t sel[P::kMaxEmit];
        // key 0 = unknown binding: nothing stored
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, false, 0, 2, 1000, o, col) == 0 && mem.live() == 0);
        // frames 2 and 3: the game draws the puff whole (M4: seen whole on kMinSeen = 2 frames before it may be re-emitted)
        CHECK(mem.select(HK, 2, g0, nullptr, sel, P::kMaxEmit, o) == 0);
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, false, K, 2, 1000, o, col) == 0);
        P::Stats f2 = mem.end_frame(&g0, 2, 1000, o);
        CHECK(f2.observed == 1 && f2.stored == 1 && mem.live() == 1 && mem.p[0].seen_n == 1);
        CHECK(mem.select(HK, 3, g1, nullptr, sel, P::kMaxEmit, o) == 0);   // seen once only: not yet re-emitted
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, false, K, 3, 1000, o, col) == 0);
        P::Stats f3 = mem.end_frame(&g0, 3, 1000, o);
        CHECK(f3.refreshed == 1 && f3.stored == 0 && f3.culled == 0 && mem.live() == 1 && mem.p[0].seen_n == 2);
        // frame 4: the game camera turned 90 degrees right (the puff is outside its window): selected once; never for another key or a second time
        CHECK(mem.select(HK, 4, g0, nullptr, sel, P::kMaxEmit, o) == 0);   // a game camera that shows it whole: the game draws it itself
        CHECK(mem.select(HK, 4, g1, nullptr, sel, P::kMaxEmit, o) == 1);
        CHECK(mem.select(HK, 4, g1, nullptr, sel, P::kMaxEmit, o) == 0);
        CHECK(mem.select(HK2, 4, g1, nullptr, sel, P::kMaxEmit, o) == 0);
        // the eye still looks +z (head turned back to the left while the aircraft turned right): the puff is drawn where it is in the world
        const M::ScreenMap e0 = eye_map(kFwdZ);
        M::Vtx out[P::kMaxEmitVerts];
        const uint16_t one = sel[0];
        const unsigned n = mem.build_eye(e0, 1.0, 1.0, &one, 1, 1000 + 2000, o, out, P::kMaxEmitVerts);
        CHECK(n == 6);
        double cx = 0, cy = 0;
        for (int k : {0, 1, 2, 5}) { cx += 0.25 * out[k].pos[0]; cy += 0.25 * out[k].pos[1]; }
        double xs, ys, d; CHECK(FS::sp_project(e0, Pc, xs, ys, d));
        CHECK(std::fabs(cx - xs) < 1e-2 && std::fabs(cy - ys) < 1e-2);
        CHECK(std::fabs(out[1].pos[0] - out[0].pos[0] - (v[1].pos[0] - v[0].pos[0])) < 1e-2);    // same size as the game drew it (same distance)
        CHECK(std::fabs(out[2].pos[1] - out[0].pos[1] - (v[2].pos[1] - v[0].pos[1])) < 1e-2);
        CHECK(std::fabs(double(out[0].pos[2]) - (M::kZ0 / 5000.0 + M::kDepthB)) < 1e-3 * double(out[0].pos[2]));   // z form, depth scale 1
        CHECK(out[0].uv[0] == v[0].uv[0] && out[1].uv[0] == v[1].uv[0] && out[2].uv[1] == v[2].uv[1] && out[5].uv[1] == v[3].uv[1]);
        CHECK(std::fabs(out[0].col[3] - 0.5f * 0.75f) < 1e-6f);          // 2 s of an 8 s life: alpha x 0.75
        for (int c = 0; c < 4; ++c) { CHECK(out[3].pos[c] == out[2].pos[c]); CHECK(out[4].pos[c] == out[1].pos[c]); }   // (0,1,2), (2,1,3)
        // the depth scale applies to z (the objects' depth fix)
        { M::Vtx o2[6]; CHECK(mem.build_eye(e0, 0.5, 1.0, &one, 1, 3000, o, o2, 6) == 6); CHECK(std::fabs(double(o2[0].pos[2]) - 0.5 * double(out[0].pos[2])) < 1e-3 * double(out[0].pos[2])); }
        // an eye turned the other way: the puff is behind it, skipped
        { M::Vtx o2[6]; const M::ScreenMap eb = eye_map(kBackZ); CHECK(mem.build_eye(eb, 1.0, 1.0, &one, 1, 3000, o, o2, 6) == 0); }
        // the cap of the output
        { M::Vtx o2[5]; CHECK(mem.build_eye(e0, 1.0, 1.0, &one, 1, 3000, o, o2, 5) == 0); }
        // a billboard faces the EYE: seen from the side (an eye at (5000, 0, 5000) looking -x) it keeps its full width, centred
        {
            const Cam side = {{5000, 0, 5000}, {0, 0, 1}, {0, 1, 0}, {-1, 0, 0}};
            const M::ScreenMap es = eye_map(side);
            M::Vtx o2[6]; CHECK(mem.build_eye(es, 1.0, 1.0, &one, 1, 3000, o, o2, 6) == 6);
            CHECK(std::fabs(o2[1].pos[0] - o2[0].pos[0] - (v[1].pos[0] - v[0].pos[0])) < 1e-2);
            CHECK(std::fabs(0.5 * (o2[0].pos[0] + o2[1].pos[0]) - 2048.0) < 1e-2);
        }
        // the frame's statistics: one selection, three builds emitted, one behind the eye, one refusal (trail-eye-cap)
        P::Stats f4 = mem.end_frame(&g1, 4, 3000, o);
        CHECK(f4.selected == 1 && f4.emitted == 3 && f4.behind == 1 && f4.refused == 1 && std::strcmp(f4.last_why, "trail-eye-cap") == 0);
        CHECK(f4.hosts == 4 && f4.nomatch == 0 && f4.nohost == 0);   // four host draws; the K2 host came after the K puff was taken
        // frame 5: selected again; the game's camera sees the puff CLIPPED at its left border: that quad is collapsed (the stored card replaces it)
        const Cam turned = {{0, 0, 0}, {std::cos(0.55), 0, -std::sin(0.55)}, {0, 1, 0}, {std::sin(0.55), 0, std::cos(0.55)}};   // the puff near the left edge
        const M::ScreenMap gt = game_map(turned);
        CHECK(mem.select(HK, 5, gt, nullptr, sel, P::kMaxEmit, o) == 1);
        M::Vtx cv[4]; card(gt, turned, Pc, 100, 100, cv);
        CHECK(cv[0].pos[0] < 1792.0f && cv[1].pos[0] > 1792.0f);
        for (auto& x : cv) x.pos[0] = std::fmax(x.pos[0], 1792.0f);   // the game's clip
        std::memset(col, 0, sizeof col);
        CHECK(mem.observe(gt, cv, 4, idx, 4, 0, false, K, 5, 3100, o, col) == 1);
        CHECK(col[0] && col[1] && col[2] && col[3]);
        // a clipped quad of another key, or one far from the puff, is not collapsed
        std::memset(col, 0, sizeof col);
        CHECK(mem.observe(gt, cv, 4, idx, 4, 0, false, K2, 5, 3100, o, col) == 0 && !col[0]);
        P::Stats f5 = mem.end_frame(&gt, 5, 3100, o);
        CHECK(f5.collapsed == 1 && f5.clipped == 2 && mem.live() == 1);
        // frame 6: not selected (no replay of the key): the clipped quad stays the game's (nothing collapsed)
        std::memset(col, 0, sizeof col);
        CHECK(mem.observe(gt, cv, 4, idx, 4, 0, false, K, 6, 3200, o, col) == 0 && !col[0] && !col[1]);
        mem.end_frame(&gt, 6, 3200, o);
        // unselect: the plan could not take the selection -> selectable again in the same frame
        CHECK(mem.select(HK, 7, g1, nullptr, sel, P::kMaxEmit, o) == 1);
        mem.unselect(7, sel, 1, "trail-window-fallback");
        CHECK(mem.select(HK, 7, g1, nullptr, sel, P::kMaxEmit, o) == 1);
        mem.end_frame(&g1, 7, 3300, o);
        // the game looks at the puff (whole inside its window) but stops drawing it: dropped after kCullFrames frames
        for (uint64_t fr = 8; fr < 8 + P::kCullFrames - 1; ++fr) { mem.end_frame(&g0, fr, 3400, o); CHECK(mem.live() == 1); }
        P::Stats fc = mem.end_frame(&g0, 8 + P::kCullFrames - 1, 3400, o);
        CHECK(fc.culled == 1 && mem.live() == 0);
        // expiry after the life (the camera looks away: not culled)
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, false, K, 20, 10000, o, col) == 0);
        mem.end_frame(&g0, 20, 10000, o);
        CHECK(mem.live() == 1);
        mem.end_frame(&g1, 21, 10000 + 8000, o); CHECK(mem.live() == 1);
        P::Stats fe = mem.end_frame(&g1, 22, 10000 + 8001, o);
        CHECK(fe.expired == 1 && mem.live() == 0);
        // refresh: the same puff a little moved is one puff
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, false, K, 30, 20000, o, col) == 0);
        M::Vtx mv[4]; const double P2[3] = {20, 0, 5000}; card(g0, kFwdZ, P2, 100, 100, mv);
        CHECK(mem.observe(g0, mv, 4, idx, 4, 0, false, K, 31, 20016, o, col) == 0);
        CHECK(mem.live() == 1 && mem.p[0].c[0] > 19.0);
        // eviction: kCap + 1 distinct puffs -> the one seen longest ago leaves
        mem.end_frame(&g0, 31, 20016, o);
        unsigned stored = 0;
        for (unsigned i = 0; i < P::kCap + 1; ++i) {
            const double Q[3] = {-1500.0 + 25.0 * double(i % 120), -600.0 + 600.0 * double(i / 120), 5000};
            M::Vtx q[4]; card(g0, kFwdZ, Q, 5, 5, q);
            mem.observe(g0, q, 4, idx, 4, 0, false, K2, 40, 30000 + i, o, col);
            ++stored;
        }
        P::Stats fv = mem.end_frame(&g0, 40, 30200, o);
        CHECK(stored == P::kCap + 1 && fv.evicted == 2 && mem.live() == P::kCap);   // K's refreshed puff (seen at 20016) and the first K2 puff
        CHECK(P::kMaxEmitVerts * sizeof(M::Vtx) == 61440);   // (M4) 160 cards x 6 vertices x 64 B per eye
    }
    // ---- a non-quad run (whole_run) is never read as puffs
    {
        static P::Memory mem;
        M::Vtx v[4]; card(g0, kFwdZ, Pc, 100, 100, v);
        uint16_t idx[4] = {0, 1, 2, 3}; uint8_t col[8] = {};
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, true, 7, 2, 0, o, col) == 0 && mem.live() == 0);
        CHECK(mem.observe(g0, v, 4, idx, 4, 0, false, 7, 2, 0, P::options(0.0, 1500.0), col) == 0 && mem.live() == 0);   // off
    }
    // ---- plan_draw extra_drop: the marked quad is collapsed in both eye copies, the other one is not
    {
        M::Vtx v[8]; card(g0, kFwdZ, Pc, 100, 100, v); const double Q[3] = {300, 0, 5000}; card(g0, kFwdZ, Q, 100, 100, v + 4);
        uint16_t idx[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
        const M::ScreenMap e0 = eye_map(kFwdZ), e1 = eye_map(kFwdZ);
        const M::ScreenMap* eyes[2] = {&e0, &e1};
        M::Vtx scratch[8], k0[8], k1[8]; uint8_t bad[8]; FS::Plan pl;
        uint8_t drop[8] = {0, 0, 0, 0, 1, 1, 1, 1};
        FS::plan_draw(g0, eyes, v, 8, idx, 9, 0, 1.0, scratch, bad, pl, k0, k1, nullptr, 0.0, false, drop);
        CHECK(pl.prepared == 1 && pl.collapsed == 2);
        for (int c = 0; c < 2; ++c) { CHECK(k0[5].pos[c] == k0[4].pos[c] && k0[7].pos[c] == k0[4].pos[c] && k1[6].pos[c] == k1[4].pos[c]); }
        CHECK(k0[1].pos[0] != k0[0].pos[0]);
        FS::Plan pn;
        FS::plan_draw(g0, eyes, v, 8, idx, 9, 0, 1.0, scratch, bad, pn, k0, k1, nullptr, 0.0, false, nullptr);
        CHECK(pn.prepared == 1 && pn.collapsed == 0 && k0[5].pos[0] != k0[4].pos[0]);
    }
    // ---- the fx-puff-trail line (M2: written once per second while active, whatever happened)
    {
        P::Stats f, r;
        uint32_t* all[] = {&f.observed, &f.stored, &f.refreshed, &f.clipped, &f.collapsed, &f.culled, &f.expired, &f.evicted, &f.selected, &f.emitted, &f.behind,
                           &f.refused, &f.draws, &f.nokey, &f.runs, &f.quads, &f.nonconst, &f.whole, &f.big, &f.streak, &f.tiny};
        for (uint32_t* x : all) *x = 4000000000u;
        f.edge_max = 1e12; r = f; r.last_why = "trail-binding-key-unknown";
        char b[600]; P::format_line(b, sizeof b, ~0ull, 4000000000u, f, r, P::kCap, true, true);
        CHECK(std::strlen(b) + std::strlen("gpu-copy-step1;event=") < 448 && std::strncmp(b, "fx-puff-trail;frame=", 20) == 0 &&
              std::strstr(b, "last-why=trail-binding-key-unkno") && std::strstr(b, ";emax=99999;"));
        // the "nothing happened" window still makes a full line: frames=0 and the open state name a closed path
        P::Stats z; P::format_line(b, sizeof b, 5, 0, z, z, 0, false, false);
        CHECK(std::strstr(b, ";frames=0;open=g0,r0;") && std::strstr(b, ";emitted=0;") && std::strstr(b, ";stored=0;") && std::strstr(b, "last-why=none"));
        // line_due: the first tick while active, then every >= 1000 ms; never while the option is off
        CHECK(P::line_due(true, 5000, 0) && !P::line_due(true, 5999, 5000) && P::line_due(true, 6000, 5000) && !P::line_due(false, 99999, 0));
        // the worker writes one line per second over a run with NO stored puff (the step139 null-run case): 10 s of 100 ms ticks -> 10 lines
        uint64_t last = 0; unsigned lines = 0;
        for (uint64_t t = 1000; t < 11000; t += 100) if (P::line_due(true, t, last)) { last = t; ++lines; }
        CHECK(lines == 10);
        P::Stats w1, w2; w1.draws = 3; w1.edge_max = 4000; w2.draws = 2; w2.edge_max = 120; w1.add(w2);
        CHECK(w1.draws == 5 && w1.edge_max == 4000);
    }
    // ---- (M3) the near rule: smoke is born at an aircraft (step141: every whole card was 4,000-11,000 units, so size cannot tell smoke from cloud)
    {
        const P::Options on = P::options(8.0, 1500.0);
        CHECK(on.cls == P::kClsNear && on.near_units == P::kDefNear && on.size_cap() == P::kNearMaxEdge && std::strcmp(P::cls_name(on.cls), "near") == 0);
        static P::Memory mem;
        uint16_t idx[4] = {0, 1, 2, 3}; uint8_t col[8] = {};
        const double Ps[3] = {0, 0, 20000};
        const float obj[2][3] = {{1000.0f, 0.0f, 20000.0f}, {-90000.0f, 0.0f, 0.0f}};   // an aircraft next to the smoke card, one far away
        M::Vtx smoke[4]; card(g0, kFwdZ, Ps, 2500, 2500, smoke);                  // a 5,000-unit card at the aircraft (the step141 sizes)
        const double Cc[3] = {20000, 0, 40000}; M::Vtx cloud[4]; card(g0, kFwdZ, Cc, 2500, 2500, cloud);   // the same size, 21,000 units from any object
        CHECK(P::nearest_object(Ps, obj, 2) == 1000.0 && P::nearest_object(Ps, obj, 0) > 1e299);
        mem.observe(g0, smoke, 4, idx, 4, 0, false, 9, 2, 0, on, col, obj, 2);
        mem.observe(g0, cloud, 4, idx, 4, 0, false, 9, 2, 0, on, col, obj, 2);
        P::Stats s = mem.end_frame(&g0, 2, 0, on);
        CHECK(s.stored == 1 && s.observed == 1 && s.wnear == 1 && s.wfar == 1 && s.whole == 2 && s.big == 0 && s.objs == 4 && mem.live() == 1);
        CHECK(std::fabs(s.edge_min - 5000.0) < 5.0 && std::fabs(s.edge_max - 5000.0) < 5.0);
        // the stored puff drifts away from the aircraft: still refreshed (no object needed), no new puff
        const double P2[3] = {400, 0, 20000}; M::Vtx drift[4]; card(g0, kFwdZ, P2, 2500, 2500, drift);
        mem.observe(g0, drift, 4, idx, 4, 0, false, 9, 3, 16, on, col, nullptr, 0);
        s = mem.end_frame(&g0, 3, 16, on);
        CHECK(s.refreshed == 1 && s.stored == 0 && s.observed == 1 && s.wfar == 0 && mem.live() == 1);
        // the size rule (fxtcls:size) refuses the same card as big
        static P::Memory ms; const P::Options os = P::options(8.0, 1500.0, P::kClsSize);
        ms.observe(g0, smoke, 4, idx, 4, 0, false, 9, 2, 0, os, col, obj, 2);
        s = ms.end_frame(&g0, 2, 0, os);
        CHECK(s.big == 1 && s.stored == 0 && s.wnear == 1 && ms.live() == 0);
        // no world camera: counted as nocam, NOT a refusal
        mem.note_nocam(); s = mem.end_frame(&g0, 4, 32, on);
        CHECK(s.nocam == 1 && s.refused == 0 && s.last_why == nullptr);
        P::Stats a, b2; a.edge_min = 0; b2.edge_min = 300; a.add(b2); CHECK(a.edge_min == 300); P::Stats c3; c3.edge_min = 200; a.add(c3); CHECK(a.edge_min == 200);
        char line[600]; P::format_line(line, sizeof line, 7, 60, s, mem.run, mem.live(), true, true, on.cls);
        CHECK(std::strstr(line, ";cls=near;") && std::strstr(line, ";nocam=1;") && std::strstr(line, ";near=0;far=0;"));
    }
    // ---- (M2) the observe census: why nothing was stored is countable
    {
        static P::Memory mem;
        uint16_t idx[4] = {0, 1, 2, 3}; uint8_t col[8] = {};
        M::Vtx big[4]; card(g0, kFwdZ, Pc, 1000, 1000, big);       // cloud-sized whole card
        M::Vtx st[4]; card(g0, kFwdZ, Pc, 300, 50, st);            // streak
        M::Vtx ok[4]; card(g0, kFwdZ, Pc, 100, 100, ok);           // a puff
        M::Vtx cl[4]; card(g0, kFwdZ, Pc, 100, 100, cl); for (auto& x : cl) x.pos[0] = std::fmax(x.pos[0] - 300.0f, 1792.0f);   // clipped
        M::Vtx nc[4]; card(g0, kFwdZ, Pc, 100, 100, nc); nc[3].pos[2] = nc[3].pos[2] * 0.5f;     // not one depth
        mem.observe(g0, big, 4, idx, 4, 0, false, 9, 2, 0, o, col);
        mem.observe(g0, st, 4, idx, 4, 0, false, 9, 2, 0, o, col);
        mem.observe(g0, ok, 4, idx, 4, 0, false, 9, 2, 0, o, col);
        mem.observe(g0, cl, 4, idx, 4, 0, false, 9, 2, 0, o, col);
        mem.observe(g0, nc, 4, idx, 4, 0, false, 9, 2, 0, o, col);
        mem.observe(g0, ok, 4, idx, 4, 0, false, 0, 2, 0, o, col);   // no binding key
        mem.observe(g0, ok, 4, idx, 4, 0, true, 9, 2, 0, o, col);    // whole run
        const P::Stats s = mem.end_frame(&g0, 2, 0, o);
        CHECK(s.draws == 7 && s.nokey == 1 && s.runs == 1 && s.quads == 5 && s.nonconst == 1 && s.whole == 3 && s.big == 1 && s.streak == 1 && s.tiny == 0);
        CHECK(s.clipped == 1 && s.observed == 1 && s.stored == 1 && std::fabs(s.edge_max - 2000.0) < 2.0 && s.refused == 1);
        CHECK(std::strcmp(s.last_why, "trail-binding-key-unknown") == 0);
    }
    // ---- options and the stage switches
    {
        std::string id, why; T::Conventions c;
        CHECK(c.fx_trail_s == 0.0f && c.fx_trail_max_edge == 1500.0f);
        CHECK(T::parse_stage_option("m03k;fxmem:6;fxtrail:8", id, c, why) && c.fx_trail_s == 8.0f && c.fx_trail_max_edge == 1500.0f);
        { T::Conventions c2; CHECK(T::parse_stage_option("m03k;fxmem:6;fxtrail:12;fxtmax:800", id, c2, why) && c2.fx_trail_s == 12.0f && c2.fx_trail_max_edge == 800.0f); }
        { T::Conventions c2; CHECK(T::parse_stage_option("m03k;fxtrail:0", id, c2, why) && c2.fx_trail_s == 0.0f); }
        const char* bad[] = {"m03k;fxtrail:0.5", "m03k;fxtrail:31", "m03k;fxtrail:x", "m03k;fxtrail:-1", "m03k;fxtmax:10", "m03k;fxtmax:200000", "m03k;fxtmax:x"};
        for (const char* b : bad) { T::Conventions cc; CHECK(!T::parse_stage_option(b, id, cc, why)); }
        const auto e6 = s5::stage_effects(6, false, 0, true, 0.0f, false, 8.0f, 800.0f);
        CHECK(e6.puff_trail_s == 8.0f && e6.puff_trail_edge == 800.0f);
        for (int m = 0; m <= 5; ++m) CHECK(s5::stage_effects(m, false, 0, true, 0.0f, false, 8.0f).puff_trail_s == 0.0f);
        CHECK(s5::stage_effects(6, false).puff_trail_s == 0.0f);
        s5::apply_stage_effects(e6);
        CHECK(s5::effects_puff_trail_life_s().load() == 8.0f && s5::effects_puff_trail_max_edge().load() == 800.0f);
        s5::clear_stage_effects();
        CHECK(s5::effects_puff_trail_life_s().load() == 0.0f && s5::effects_puff_trail_max_edge().load() == 1500.0f);
        char b[400];
        CHECK(!s5::fx_puff_trail_line(b, sizeof b, 6, 0.0f, 1500.0f));
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 8.0f, 1500.0f) && std::strstr(b, "event=fx-puff-trail-option;life-s=8.0;max-edge=1500;cls=near;near=2500;active=1;"));
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 8.0f, 1500.0f, 1, 900.0f) && std::strstr(b, ";cls=size;near=900;"));
        // (M3) fxtcls / fxtnear
        { T::Conventions c3; CHECK(c3.fx_trail_cls == 0 && c3.fx_trail_near == 2500.0f); }
        { T::Conventions c3; CHECK(T::parse_stage_option("m03k;fxmem:6;fxtrail:8;fxtcls:size;fxtnear:4000", id, c3, why) && c3.fx_trail_cls == 1 && c3.fx_trail_near == 4000.0f); }
        { T::Conventions c3; CHECK(T::parse_stage_option("m03k;fxtcls:near", id, c3, why) && c3.fx_trail_cls == 0); }
        const char* bad3[] = {"m03k;fxtcls:cloud", "m03k;fxtnear:50", "m03k;fxtnear:60000", "m03k;fxtnear:x"};
        for (const char* b3 : bad3) { T::Conventions cc; CHECK(!T::parse_stage_option(b3, id, cc, why)); }
        const auto e3 = s5::stage_effects(6, false, 0, true, 0.0f, false, 8.0f, 1500.0f, 1, 4000.0f);
        CHECK(e3.puff_trail_cls == 1 && e3.puff_trail_near == 4000.0f);
        s5::apply_stage_effects(e3);
        CHECK(s5::effects_puff_trail_cls().load() == 1 && s5::effects_puff_trail_near().load() == 4000.0f);
        s5::clear_stage_effects();
        CHECK(s5::effects_puff_trail_cls().load() == 0 && s5::effects_puff_trail_near().load() == 2500.0f);
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 5, 8.0f, 1500.0f) && std::strstr(b, "active=0;reason=needs-fxmem-6"));
    }
    std::printf("fx_puff_trail_offline: %u checks passed\n", g_checks);
    return 0;
}
