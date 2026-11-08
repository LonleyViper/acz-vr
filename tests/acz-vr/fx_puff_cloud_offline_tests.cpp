// EFFECTS-OUTSIDE (M4, headset test 6 2026-10-30d items 5-6) offline tests: fx_puff_trail.h's cloud class and the M4 rules that answer "smoke trails stop
// and start" (docs/world-render/effects-outside-window.md section 20): the cloud class with its own capacity, one card refreshes one stored card per frame
// (the M1 radius merged neighbouring trail puffs), duplicates, kMinSeen (a spark that never holds still is never re-emitted), the game-window hand-over (no
// ghost of a puff that dissipated in view), cross-key hosts (a stored card is drawn every frame while ANY class-1 draw of the same PSO / heaps is replayed,
// not only one of its own key), key aliveness, eye visibility, the emission cap (nearest kept), the key-ordered far-to-near selection and its groups, the
// fades (age, cache edge, near), the cache drop, nohost, the self exclusion of the near rule, the fx-puff-mem line, the options and the stage switches.
#include "fx_puff_trail.h"
#include "terrain_stage.h"
#include "effects_stage_flags.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace P = acz::fxpuff;
namespace FS = acz::fxsprite;
namespace M = acz::fxmem;
namespace T = acz::terrain;
namespace s5 = acz::gpu_copy::step5;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// A game / eye map for a camera at E (as fx_puff_trail_offline: GS window 512 x 448 around 2048, half tangents 0.6152 / 0.346).
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
static const Cam kFwdX = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};   // looks +x (the aircraft turned 90 degrees right)
static const Cam kBackZ = {{0, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}; // looks -z

// A camera-facing constant-depth card of `cam` at world centre c, half extents h (strip order: top-left, top-right, bottom-left, bottom-right).
static void card(const M::ScreenMap& m, const Cam& cam, const double c[3], double h, M::Vtx out[4], float alpha = 0.5f) {
    const double sx[4] = {-1, 1, -1, 1}, sy[4] = {1, 1, -1, -1};
    double xs, ys, dc; CHECK(FS::sp_project(m, c, xs, ys, dc));
    for (int k = 0; k < 4; ++k) {
        double w[3]; for (int i = 0; i < 3; ++i) w[i] = c[i] + sx[k] * h * cam.R[i] + sy[k] * h * cam.U[i];
        double x, y, d; CHECK(FS::sp_project(m, w, x, y, d));
        M::Vtx v{}; v.pos[0] = float(x); v.pos[1] = float(y); v.pos[2] = float(m.z0 / dc + m.b); v.pos[3] = 1.0f;
        v.uv[0] = sx[k] > 0 ? 1.0f : 0.0f; v.uv[1] = sy[k] > 0 ? 0.0f : 1.0f; v.col[0] = v.col[1] = v.col[2] = 0.8f; v.col[3] = alpha;
        out[k] = v;
    }
}
static const uint16_t kIdx[4] = {0, 1, 2, 3};
// One class-1 draw of one card.
static unsigned see(P::Memory& mem, const M::ScreenMap& g, const Cam& cam, const double c[3], double h, uint64_t key, uint64_t frame, uint64_t now,
                    const P::Options& o, const float (*obj)[3] = nullptr, unsigned nobj = 0, uint64_t pso = 0, uint64_t heap = 0, uint8_t* col = nullptr) {
    M::Vtx v[4]; card(g, cam, c, h, v);
    return mem.observe(g, v, 4, kIdx, 4, 0, false, key, frame, now, o, col, obj, nobj, pso, heap);
}
// (F1) a game class-1 draw of `key` in `frame` without a card of its own (a whole run: the key and its heaps are noted, no quad is analysed).
static void bind_key(P::Memory& mem, uint64_t key, uint64_t frame, const P::Options& o, uint64_t pso, uint64_t heap) {
    static const Cam kX = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
    const double W[3] = {20000, 0, 0}; const M::ScreenMap g = game_map(kX); M::Vtx v[4]; card(g, kX, W, 10, v);
    mem.observe(g, v, 4, kIdx, 4, 0, true, key, frame, frame * 16, o, nullptr, nullptr, 0, pso, heap);
}

// ---- (M7, headset test 8 2026-11-01, docs section 25) the orientation of re-emitted cards: world up, no aircraft roll, no HMD roll; the game's roll at the seam
static double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void unit(double* a) { const double l = std::sqrt(dot3(a, a)); for (int i = 0; i < 3; ++i) a[i] /= l; }
static const double kY[3] = {0, 1, 0};
// A camera at E looking along `fwd`, rolled by `roll_deg` about its forward (the test maps' handedness: R = U x F, as kFwdZ).
static Cam look(const double E[3], const double fwd[3], double roll_deg) {
    Cam c{}; double F[3] = {fwd[0], fwd[1], fwd[2]}; unit(F);
    double U0[3]; const double k = dot3(kY, F); for (int i = 0; i < 3; ++i) U0[i] = kY[i] - k * F[i]; unit(U0);
    const double R0[3] = {U0[1] * F[2] - U0[2] * F[1], U0[2] * F[0] - U0[0] * F[2], U0[0] * F[1] - U0[1] * F[0]};
    const double a = roll_deg * 3.14159265358979323846 / 180.0, ca = std::cos(a), sa = std::sin(a);
    for (int i = 0; i < 3; ++i) { c.E[i] = E[i]; c.F[i] = F[i]; c.R[i] = R0[i] * ca + U0[i] * sa; c.U[i] = -R0[i] * sa + U0[i] * ca; }
    return c;
}
// The world up perpendicular to the ray E -> C (what a world-up card facing the eye point must have).
static void world_up_perp(const double E[3], const double C[3], double u[3]) {
    double f[3] = {C[0] - E[0], C[1] - E[1], C[2] - E[2]}; unit(f);
    const double k = dot3(kY, f); for (int i = 0; i < 3; ++i) u[i] = kY[i] - k * f[i]; unit(u);
}
// The world axes of an emitted card (6 TRIANGLELIST vertices; corners 0, 1, 2, 5 = TL, TR, BL, BR), recovered by unprojecting with the eye map.
static bool emitted_axes(const M::ScreenMap& e, const M::Vtx* out, double up[3], double rt[3], double nrm[3] = nullptr) {
    const int id[4] = {0, 1, 2, 5}; double w[4][3];
    for (int k = 0; k < 4; ++k) {
        M::DepthSource src; const double d = FS::sp_vertex_depth(e, out[id[k]].pos[2], out[id[k]].pos[3], src);
        if (!(d > 0) || !FS::sp_unproject(e, out[id[k]].pos[0], out[id[k]].pos[1], d, w[k])) return false;
    }
    for (int i = 0; i < 3; ++i) { up[i] = (w[0][i] + w[1][i]) - (w[2][i] + w[3][i]); rt[i] = (w[1][i] + w[3][i]) - (w[0][i] + w[2][i]); }
    unit(up); unit(rt);
    if (nrm) { nrm[0] = up[1] * rt[2] - up[2] * rt[1]; nrm[1] = up[2] * rt[0] - up[0] * rt[2]; nrm[2] = up[0] * rt[1] - up[1] * rt[0]; unit(nrm); }
    return true;
}
// The signed roll (radians) of u about the ray f relative to the reference ref (both perpendicular to f).
static double roll_about(const double f[3], const double ref[3], const double u[3]) {
    const double c[3] = {ref[1] * u[2] - ref[2] * u[1], ref[2] * u[0] - ref[0] * u[2], ref[0] * u[1] - ref[1] * u[0]};
    return std::atan2(dot3(c, f), dot3(ref, u));
}

static void orientation_tests() {
    const uint64_t K = 0x15678a0012a280ull;
    const double O[3] = {0, 0, 0};
    const double fz[3] = {0, 0, 1}, fx[3] = {1, 0, 0};
    uint16_t sel[P::kMaxEmit];
    CHECK(P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0).up == P::kUpSeam);   // the DLL default
    // A stored card (cloud or smoke) seen whole twice by the level game camera looking +z, then re-emitted with the given game camera (the aircraft)
    // and eye (aircraft x head). Returns the emitted card's world up / right through `up` / `rt`; false = not emitted.
    struct Case { const double* C; double h; bool smoke; };
    auto run = [&](const Case& cs, const Cam& game, const Cam& eye, int mode, double up[3], double rt[3], double nrm[3]) -> bool {
        std::unique_ptr<P::Memory> mp(new P::Memory()); P::Memory& mem = *mp;
        P::Options o = cs.smoke ? P::options(8.0, 1500.0, P::kClsNear, 2500.0, 0.0) : P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0);
        o.up = mode;
        const M::ScreenMap g0 = game_map(kFwdZ);
        const float obj[1][3] = {{float(cs.C[0] + 200.0), float(cs.C[1]), float(cs.C[2])}};   // the smoking aircraft next to the smoke card
        const float (*op)[3] = cs.smoke ? &obj[0] : nullptr;
        for (uint64_t f = 2; f < 4; ++f) { see(mem, g0, kFwdZ, cs.C, cs.h, K, f, f * 16, o, op, cs.smoke ? 1u : 0u); mem.end_frame(&g0, f, f * 16, o); }
        if (mem.live_of(cs.smoke ? P::kSmoke : P::kCloud) != 1) return false;
        const M::ScreenMap g = game_map(game), e = eye_map(eye);
        const unsigned n = mem.select(P::Host{K}, 4, g, &e, sel, P::kMaxEmit, o);
        M::Vtx out[P::kMaxEmitVerts];
        const unsigned v = mem.build_eye(e, 1.0, 1.0, sel, n, 64, o, out, P::kMaxEmitVerts, nullptr, nullptr, &g);
        mem.end_frame(&g, 4, 64, o);
        return n == 1 && v == 6 && emitted_axes(e, out, up, rt, nrm);
    };
    const double Cc[3] = {0, 0, 20000}, Cs[3] = {0, 0, 15000}, Coff[3] = {6000, 3000, 20000}, Csoff[3] = {-3000, -2500, 12000};
    const Case cases[4] = {{Cc, 3000, false}, {Cs, 500, true}, {Coff, 3000, false}, {Csoff, 500, true}};
    // ---- 1. aircraft roll x HMD roll x head direction: the card's up is the world up perpendicular to the eye ray, its normal the ray, every time.
    //         The aircraft turned 90 degrees right (game camera +x, rolled by `air`): the cards lie outside the game window (seam weight 0).
    unsigned n_ok = 0;
    for (const Case& cs : cases)
        for (double air : {0.0, 30.0, 90.0, 170.0, -45.0})
            for (double hmd : {0.0, 20.0, -60.0}) {
                const Cam game = look(O, fx, air);
                double dir[3] = {cs.C[0], cs.C[1], cs.C[2]};
                const double head[3] = {dir[0] + 1500.0, dir[1] - 1000.0, dir[2]};   // the head looks near the card, not exactly at it
                for (const double* hd : {static_cast<const double*>(dir), head}) {
                    const Cam eye = look(O, hd, air + hmd);   // the eye carries the aircraft's roll and the head's roll
                    double up[3], rt[3], nm[3], exp[3];
                    CHECK(run(cs, game, eye, P::kUpSeam, up, rt, nm));
                    world_up_perp(O, cs.C, exp);
                    double f[3] = {cs.C[0], cs.C[1], cs.C[2]}; unit(f);
                    CHECK(dot3(up, exp) > 0.99999);              // world up, whatever the aircraft / head roll
                    CHECK(std::fabs(dot3(rt, kY)) < 1e-4);         // its right edge stays level
                    CHECK(std::fabs(std::fabs(dot3(nm, f)) - 1.0) < 1e-4);   // it faces the eye point
                    ++n_ok;
                }
            }
    CHECK(n_ok == 4 * 5 * 3 * 2);
    // the same orientation for every roll: the emitted up does not move between aircraft rolls 0 and 90 and HMD roll 0 and -60
    {
        double u0[3], r0[3], n0[3], u1[3], r1[3], n1[3];
        CHECK(run(cases[2], look(O, fx, 0.0), look(O, Coff, 0.0), P::kUpSeam, u0, r0, n0));
        CHECK(run(cases[2], look(O, fx, 90.0), look(O, Coff, 30.0), P::kUpSeam, u1, r1, n1));
        CHECK(dot3(u0, u1) > 0.999999 && dot3(r0, r1) > 0.999999);
    }
    // ---- 2. the M4..M6 behaviour (kUpEye) for contrast: the card took the eye's roll (aircraft + HMD), the bug of headset test 8
    for (double roll : {30.0, 90.0, -60.0}) {
        double up[3], rt[3], nm[3];
        CHECK(run(cases[0], look(O, fx, 0.0), look(O, fz, roll), P::kUpEye, up, rt, nm));
        CHECK(std::fabs(dot3(up, kY) - std::cos(roll * 3.14159265358979323846 / 180.0)) < 1e-4);
        CHECK(run(cases[0], look(O, fx, 0.0), look(O, fz, roll), P::kUpWorld, up, rt, nm));
        CHECK(dot3(up, kY) > 0.99999);
    }
    // ---- 3. the seam: inside / at the game window the card takes the GAME camera's roll (the aircraft's; never the head's), fading to world up over kSeamBand
    {
        const double air = 30.0;
        const Cam gc = look(O, fz, air);
        const M::ScreenMap g = game_map(gc);
        double gR[3], gU[3], gF[3]; CHECK(P::basis(g, gR, gU, gF));
        double prev = 0.0; bool first = true;
        for (double o = -0.2; o <= 0.8001; o += 0.05) {   // the card centre o half-widths beyond the right border of the game window
            const double xs = M::kGsCentre + M::kGsHalfW * (1.0 + o);
            double C[3]; CHECK(FS::sp_unproject(g, xs, M::kGsCentre, 20000.0, C));
            double f[3] = {C[0], C[1], C[2]}; unit(f);
            double ref[3]; world_up_perp(O, C, ref);
            double gp[3]; const double kg = dot3(gU, f); for (int i = 0; i < 3; ++i) gp[i] = gU[i] - kg * f[i]; unit(gp);
            const double full = roll_about(f, ref, gp);   // the game's roll of this card relative to world up
            CHECK(std::fabs(full) > 0.2);                   // the aircraft is rolled 30 degrees
            double wexp = 1.0 - P::smooth01(std::fmax(0.0, o) / P::kSeamBand);
            for (double hmd : {0.0, 25.0, -70.0}) {         // the head's roll never changes the result
                const Cam ec = look(O, C, air + hmd);
                const M::ScreenMap e = eye_map(ec);
                double eR[3], eU[3], eF[3], E[3]; CHECK(P::basis(e, eR, eU, eF) && FS::game_eye(e, E));
                double R[3], U[3], w = -1;
                CHECK(P::card_axes(C, E, eR, eU, eF, &g, gU, P::kUpSeam, R, U, &w));
                CHECK(std::fabs(w - wexp) < 1e-6);
                CHECK(std::fabs(roll_about(f, ref, U) - wexp * full) < 1e-6);
                CHECK(std::fabs(dot3(U, f)) < 1e-9 && std::fabs(dot3(R, f)) < 1e-9 && std::fabs(dot3(R, U)) < 1e-9);
                double hR[3] = {f[1] * U[2] - f[2] * U[1], f[2] * U[0] - f[0] * U[2], f[0] * U[1] - f[1] * U[0]};   // same handedness as the eye
                double heR[3] = {eF[1] * eU[2] - eF[2] * eU[1], eF[2] * eU[0] - eF[0] * eU[2], eF[0] * eU[1] - eF[1] * eU[0]};
                CHECK((dot3(hR, R) > 0) == (dot3(heR, eR) > 0));
                // kUpWorld ignores the game camera
                double R2[3], U2[3]; CHECK(P::card_axes(C, E, eR, eU, eF, &g, gU, P::kUpWorld, R2, U2) && dot3(U2, ref) > 0.999999);
            }
            if (o <= 0.0) CHECK(std::fabs(wexp - 1.0) < 1e-12);   // at / inside the border: the game's own roll (no jump at the hand-over)
            if (o >= P::kSeamBand) CHECK(wexp == 0.0);
            if (!first) CHECK(std::fabs(wexp - prev) < 0.25);     // continuous: no step between neighbouring positions
            prev = wexp; first = false;
        }
        // behind the game camera: weight 0
        const double Cb[3] = {0, 0, -20000}; CHECK(P::seam_weight(g, Cb) == 0.0);
        // end to end: a stored cloud just beyond the game window's right border (o = 0.1) is emitted with the game's roll x w, not the eye's
        static P::Memory mem; const P::Options oc = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0);
        double C[3]; CHECK(FS::sp_unproject(g, M::kGsCentre + M::kGsHalfW * 1.1, M::kGsCentre, 20000.0, C));
        // stored by a level camera turned toward it (wholly in that window)
        double cd[3] = {C[0], C[1], C[2]}; const Cam lv = look(O, cd, 0.0); const M::ScreenMap gl = game_map(lv);
        for (uint64_t f = 2; f < 4; ++f) { see(mem, gl, lv, C, 3000, K, f, f * 16, oc); mem.end_frame(&gl, f, f * 16, oc); }
        CHECK(mem.live_of(P::kCloud) == 1);
        const Cam ec = look(O, cd, air - 50.0); const M::ScreenMap e = eye_map(ec);
        const unsigned n = mem.select(P::Host{K}, 4, g, &e, sel, P::kMaxEmit, oc);
        M::Vtx out[P::kMaxEmitVerts];
        CHECK(n == 1 && mem.build_eye(e, 1.0, 1.0, sel, n, 64, oc, out, P::kMaxEmitVerts, nullptr, nullptr, &g) == 6);
        double up[3], rt[3]; CHECK(emitted_axes(e, out, up, rt));
        double f[3] = {C[0], C[1], C[2]}; unit(f);
        double ref[3]; world_up_perp(O, C, ref);
        double gp[3]; const double kg = dot3(gU, f); for (int i = 0; i < 3; ++i) gp[i] = gU[i] - kg * f[i]; unit(gp);
        const double w = P::seam_weight(g, mem.p[sel[0]].c);
        CHECK(w > 0.0 && w < 1.0);
        CHECK(std::fabs(roll_about(f, ref, up) - w * roll_about(f, ref, gp)) < 1e-3);
        // without the game map (nullptr): pure world up
        CHECK(mem.build_eye(e, 1.0, 1.0, sel, n, 64, oc, out, P::kMaxEmitVerts) == 6 && emitted_axes(e, out, up, rt) && dot3(up, ref) > 0.99999);
    }
    // ---- 4. the pole: a card straight overhead / near it stays finite, perpendicular to the ray, and continuous
    {
        const Cam ec = look(O, fz, 40.0); const M::ScreenMap e = eye_map(ec);
        double eR[3], eU[3], eF[3], E[3]; CHECK(P::basis(e, eR, eU, eF) && FS::game_eye(e, E));
        double prevU[3] = {}; bool have = false;
        for (double deg = 10.0; deg >= -0.001; deg -= 0.25) {   // from 10 degrees off vertical to straight up, tilting toward +z
            const double a = deg * 3.14159265358979323846 / 180.0;
            const double C[3] = {0, 20000.0 * std::cos(a), 20000.0 * std::sin(a)};
            double R[3], U[3];
            CHECK(P::card_axes(C, E, eR, eU, eF, nullptr, nullptr, P::kUpSeam, R, U));
            double f[3] = {C[0], C[1], C[2]}; unit(f);
            for (int i = 0; i < 3; ++i) CHECK(std::isfinite(R[i]) && std::isfinite(U[i]));
            CHECK(std::fabs(dot3(U, f)) < 1e-9 && std::fabs(dot3(U, U) - 1.0) < 1e-9 && std::fabs(dot3(R, U)) < 1e-9);
            if (have) CHECK(dot3(U, prevU) > 0.9);   // no flip between 0.25-degree steps
            for (int i = 0; i < 3; ++i) prevU[i] = U[i]; have = true;
        }
    }
}

int main() {
    const uint64_t K = 0x15678a0012a280ull, K2 = 0x15678a0012a2a0ull, PSO = 0x1b6d8fd2740ull, HEAP = 0x2a00ull;
    const M::ScreenMap g0 = game_map(kFwdZ), g1 = game_map(kFwdX), e0 = eye_map(kFwdZ), eb = eye_map(kBackZ);
    const P::Options oc = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0);            // clouds only (fxcloud:120)
    const P::Options ob = P::options(8.0, 1500.0, P::kClsNear, 2500.0, 120.0);            // smoke + clouds
    uint16_t sel[P::kMaxEmit];

    // ---- options
    {
        CHECK(oc.on && !oc.smoke && oc.cloud && oc.cloud_life_ms == 120000.0 && oc.cloud_far == P::kDefCloudFar && oc.xkey);
        CHECK(ob.on && ob.smoke && ob.cloud && ob.life_of(P::kSmoke) == 8000.0 && ob.life_of(P::kCloud) == 120000.0);
        const P::Options off = P::options(0.0, 1500.0); CHECK(!off.on && !off.smoke && !off.cloud);
        const P::Options x = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 60.0, 25000.0, false); CHECK(x.cloud_far == 25000.0 && !x.xkey && x.on);
        CHECK(P::options(0.0, 1500.0, P::kClsNear, 2500.0, 60.0, -1.0).cloud_far == P::kDefCloudFar);
        CHECK(P::kSlots == P::kCap + P::kCloudCap && P::kMaxEmitVerts == P::kMaxEmit * 6);
    }
    // ---- the cloud class: a whole cloud-sized card far from every object is stored as a CLOUD; a card near an aircraft is smoke (not stored with smoke off)
    {
        static P::Memory mem;
        const double C[3] = {0, 0, 20000}, S[3] = {2000, 1000, 15000};
        const float obj[1][3] = {{2500.0f, 1000.0f, 15000.0f}};   // an aircraft next to S
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc, obj, 1);
        see(mem, g0, kFwdZ, S, 2500, K, 2, 0, oc, obj, 1);
        const double small[3] = {-5000, 0, 20000}; see(mem, g0, kFwdZ, small, 300, K, 2, 0, oc, obj, 1);   // a 600-unit speck: not a cloud
        P::Stats s = mem.end_frame(&g0, 2, 0, oc);
        CHECK(s.stored == 1 && s.cstored == 1 && s.wnear == 1 && s.wfar == 2 && mem.live_of(P::kCloud) == 1 && mem.live_of(P::kSmoke) == 0);
        // both classes on: the near card is smoke, the far one (already stored) is refreshed as a cloud
        static P::Memory m2;
        see(m2, g0, kFwdZ, C, 3000, K, 2, 0, ob, obj, 1);
        see(m2, g0, kFwdZ, S, 2500, K, 2, 0, ob, obj, 1);
        s = m2.end_frame(&g0, 2, 0, ob);
        CHECK(s.stored == 2 && s.cstored == 1 && m2.live_of(P::kCloud) == 1 && m2.live_of(P::kSmoke) == 1);
        see(m2, g0, kFwdZ, C, 3000, K, 3, 16, ob, nullptr, 0);
        s = m2.end_frame(&g0, 3, 16, ob);
        CHECK(s.refreshed == 1 && s.crefreshed == 1 && s.stored == 0);
    }
    // ---- the self exclusion of the near rule (the player's aircraft sits at the game camera)
    {
        const double p[3] = {0, 0, 3000}, self[3] = {0, 0, 0};
        const float obj[2][3] = {{0.0f, 0.0f, 100.0f}, {0.0f, 0.0f, 9000.0f}};
        CHECK(P::nearest_object(p, obj, 2) == 2900.0);
        CHECK(P::nearest_object(p, obj, 2, self) == 6000.0);   // the object 100 units from the camera is the player's own aircraft: skipped
        CHECK(P::nearest_object(p, obj, 1, self) > 1e299);
    }
    // ---- separate capacities: kCloudCap + 1 clouds evict ONE cloud; the smoke puff stays
    {
        static P::Memory mem;
        const float obj[1][3] = {{0.0f, -4000.0f, 15000.0f}};
        const double S[3] = {0, -4000, 15000}; see(mem, g0, kFwdZ, S, 1000, K, 2, 0, ob, obj, 1);
        unsigned n = 0;
        for (unsigned i = 0; i < P::kCloudCap + 1; ++i) {
            const double C[3] = {-9000.0 + 450.0 * double(i % 40), 1000.0 + 450.0 * double(i / 40), 30000};   // 1,200-unit cards, 450 apart (> 0.25 x 1,200)
            see(mem, g0, kFwdZ, C, 600, K, 2, 1 + i, ob); ++n;
        }
        const P::Stats s = mem.end_frame(&g0, 2, 1000, ob);
        CHECK(n == P::kCloudCap + 1 && s.cstored == P::kCloudCap + 1 && s.evicted == 1);
        CHECK(mem.live_of(P::kCloud) == P::kCloudCap && mem.live_of(P::kSmoke) == 1);
    }
    // ---- one card refreshes ONE stored card per frame (M4): two trail puffs 1,000 apart with 5,000-unit cards stay two and do not hop; duplicates
    {
        static P::Memory mem;
        const P::Options o = P::options(8.0, 1500.0);
        const float obj[1][3] = {{500.0f, 0.0f, 20000.0f}};
        const double A[3] = {0, 0, 20000}, B[3] = {1000, 0, 20000};
        see(mem, g0, kFwdZ, A, 2500, K, 2, 0, o, obj, 1);
        see(mem, g0, kFwdZ, B, 2500, K, 2, 0, o, obj, 1);
        P::Stats s = mem.end_frame(&g0, 2, 0, o);
        CHECK(s.stored == 2 && mem.live() == 2);   // M1 (0.5 x edge = 2,500) refreshed A with B: one puff for the two
        int ia = -1, ib = -1;
        for (unsigned i = 0; i < P::kSlots; ++i) if (mem.p[i].used) { if (std::fabs(mem.p[i].c[0]) < 1.0) ia = int(i); else if (std::fabs(mem.p[i].c[0] - 1000.0) < 1.0) ib = int(i); }
        CHECK(ia >= 0 && ib >= 0);
        // frame 3, B first then A: each refreshes its own card
        see(mem, g0, kFwdZ, B, 2500, K, 3, 16, o, obj, 1);
        see(mem, g0, kFwdZ, A, 2500, K, 3, 16, o, obj, 1);
        s = mem.end_frame(&g0, 3, 16, o);
        CHECK(s.refreshed == 2 && s.stored == 0 && mem.live() == 2);
        CHECK(std::fabs(mem.p[ia].c[0]) < 1.0 && std::fabs(mem.p[ib].c[0] - 1000.0) < 1.0 && mem.p[ia].seen_n == 2 && mem.p[ib].seen_n == 2);
        // the same card drawn twice in one frame: a duplicate, nothing stored
        see(mem, g0, kFwdZ, A, 2500, K, 4, 32, o, obj, 1);
        see(mem, g0, kFwdZ, A, 2500, K, 4, 32, o, obj, 1);
        s = mem.end_frame(&g0, 4, 32, o);
        CHECK(s.dup == 1 && s.refreshed == 1 && s.observed == 1 && s.stored == 0 && mem.live() == 2);
    }
    // ---- kMinSeen and the spark churn of the live-2210 static block (stored 120/s, culled 120/s): a card that never holds still is never re-emitted
    {
        static P::Memory mem;
        const P::Options o = P::options(8.0, 1500.0);
        unsigned selected = 0;
        for (uint64_t f = 2; f < 62; ++f) {
            const double C[3] = {-3000.0 + 100.0 * double(f), 0, 8000};   // 100 units per frame, a 60-unit card
            const float obj[1][3] = {{float(C[0]), 0.0f, 8000.0f}};
            selected += mem.select(P::Host{K}, f, g1, &e0, sel, P::kMaxEmit, o);   // the game camera looks away: everything stored is "not shown"
            see(mem, g0, kFwdZ, C, 30, K, f, f * 16, o, obj, 1);
            mem.end_frame(&g0, f, f * 16, o);
        }
        CHECK(selected == 0 && mem.run.stored == 60 && mem.run.culled >= 55);
        // a card seen once and then left behind: not re-emitted (seen_n 1)
        static P::Memory m1;
        const double C[3] = {0, 0, 8000}; const float obj[1][3] = {{0.0f, 0.0f, 8000.0f}};
        see(m1, g0, kFwdZ, C, 30, K, 2, 0, o, obj, 1); m1.end_frame(&g0, 2, 0, o);
        CHECK(m1.select(P::Host{K}, 3, g1, nullptr, sel, P::kMaxEmit, o) == 0);
    }
    // ---- the hand-over: a puff that dissipates INSIDE the game window is never re-emitted (M1 re-emitted it as a ghost on the 2 frames before the cull)
    {
        static P::Memory mem;
        const double C[3] = {0, 0, 20000};
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc); mem.end_frame(&g0, 2, 0, oc);
        see(mem, g0, kFwdZ, C, 3000, K, 3, 16, oc); mem.end_frame(&g0, 3, 16, oc);
        unsigned selected = 0;
        for (uint64_t f = 4; f < 7; ++f) { selected += mem.select(P::Host{K}, f, g0, &e0, sel, P::kMaxEmit, oc); mem.end_frame(&g0, f, f * 16, oc); }
        CHECK(selected == 0 && mem.live() == 0 && mem.run.culled == 1);
    }
    // ---- cross-key hosts: the stored cloud of key K is drawn on EVERY frame while the game draws only key K2 (same PSO / heaps), until K is no longer alive
    // (review 2026-10-31 F1: "alive" = bound by a game class-1 draw EARLIER IN THE SAME FRAME under the same heaps; a key bound only in an earlier frame may
    // name a descriptor slot the game has since rewritten or freed, so it is never re-bound)
    {
        static P::Memory mem;
        const double C[3] = {0, 0, 20000};
        // a game class-1 draw that binds `key` this frame without a card of its own (a whole run: the key is noted, no quad analysed)
        auto bind = [&](uint64_t key, uint64_t f, uint64_t heap) {
            const double B[3] = {20000, 0, 0}; M::Vtx v[4]; card(g1, kFwdX, B, 10, v);   // (a whole run: its quad is not analysed)
            mem.observe(g1, v, 4, kIdx, 4, 0, true, key, f, f * 16, oc, nullptr, nullptr, 0, PSO, heap);
        };
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc, nullptr, 0, PSO, HEAP); mem.end_frame(&g0, 2, 0, oc);
        see(mem, g0, kFwdZ, C, 3000, K, 3, 16, oc, nullptr, 0, PSO, HEAP); mem.end_frame(&g0, 3, 16, oc);
        CHECK(P::kKeyAliveFrames == 0);
        CHECK(mem.key_alive(K, 3, HEAP) && !mem.key_alive(K, 4, HEAP) && !mem.key_alive(K, 3, HEAP + 1) && !mem.key_alive(K2, 3, HEAP));
        unsigned frames = 0, xk = 0;
        for (uint64_t f = 4; f < 104; ++f) {   // the aircraft turned right: the game camera never shows the cloud, the head (eye) still looks at it
            bind(K, f, HEAP);                        // ... and the game still binds K this frame (another draw of the same PSO, before the host)
            const unsigned n = mem.select(P::Host{K2, PSO, HEAP}, f, g1, &e0, sel, P::kMaxEmit, oc);
            M::Vtx out[P::kMaxEmitVerts]; P::Group gr[P::kMaxKeys]; unsigned ng = 0;
            const unsigned v = mem.build_eye(e0, 1.0, 1.0, sel, n, f * 16, oc, out, P::kMaxEmitVerts, gr, &ng);
            if (n == 1 && v == 6 && ng == 1 && gr[0].key == K && gr[0].first == 0 && gr[0].count == 6) ++frames;
            const P::Stats s = mem.end_frame(&g1, f, f * 16, oc);
            xk += s.xkey;
            CHECK(s.nohost == 0);
        }
        CHECK(frames == 100 && xk == 100 && mem.live_of(P::kCloud) == 1);   // no frame without the cloud: no "stop and start"
        // another PSO, other heaps, xkey off: not drawable under that host (nomatch)
        { bind(K, 104, HEAP); const unsigned n = mem.select(P::Host{K2, PSO + 8, HEAP}, 104, g1, &e0, sel, P::kMaxEmit, oc); const P::Stats s = mem.end_frame(&g1, 104, 104 * 16, oc); CHECK(n == 0 && s.nomatch == 1); }
        { bind(K, 105, HEAP); const unsigned n = mem.select(P::Host{K2, PSO, HEAP + 1}, 105, g1, &e0, sel, P::kMaxEmit, oc); const P::Stats s = mem.end_frame(&g1, 105, 105 * 16, oc); CHECK(n == 0 && s.nomatch == 1); }
        { bind(K, 106, HEAP); const P::Options nx = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, false);   // fxtxkey:0
          CHECK(mem.select(P::Host{K2, PSO, HEAP}, 106, g1, &e0, sel, P::kMaxEmit, nx) == 0); mem.end_frame(&g1, 106, 106 * 16, oc); }
        // its own key still hosts it without a PSO identity (fxtxkey:0 too: the own-key path never re-binds a table)
        CHECK(mem.select(P::Host{K}, 107, g1, &e0, sel, P::kMaxEmit, oc) == 1); mem.end_frame(&g1, 107, 107 * 16, oc);
        { const P::Options nx = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, false);
          CHECK(mem.select(P::Host{K}, 108, g1, &e0, sel, P::kMaxEmit, nx) == 1); mem.end_frame(&g1, 108, 108 * 16, oc); }
        // F1: K last bound by a game draw in frame 106 (the own-key hosts of 107 / 108 bind nothing new) and NOT in frame 109: a K2 host of frame 109 must not
        // re-bind K (the slot may hold another texture or a freed one by now). Before the fix (kKeyAliveFrames = 120) this selected the card.
        { const unsigned n = mem.select(P::Host{K2, PSO, HEAP}, 109, g1, &e0, sel, P::kMaxEmit, oc); const P::Stats s = mem.end_frame(&g1, 109, 109 * 16, oc);
          CHECK(n == 0 && s.nomatch == 1 && s.xkey == 0); }
        // ... nor at frames 110 / 180 (4 / 74 frames after K's last binding: inside the old 120-frame window)
        for (uint64_t f : {110ull, 180ull}) { CHECK(mem.select(P::Host{K2, PSO, HEAP}, f, g1, &e0, sel, P::kMaxEmit, oc) == 0); mem.end_frame(&g1, f, 2000, oc); }
        // K bound this frame but under OTHER heaps (another heap object: K names a slot of a different heap): refused
        { bind(K, 181, HEAP + 1); CHECK(mem.select(P::Host{K2, PSO, HEAP}, 181, g1, &e0, sel, P::kMaxEmit, oc) == 0); mem.end_frame(&g1, 181, 2000, oc); }
        // K bound AFTER the host in the same frame: not known at the host (the card waits for K's own draw, which hosts it as its own key)
        { CHECK(mem.select(P::Host{K2, PSO, HEAP}, 182, g1, &e0, sel, P::kMaxEmit, oc) == 0); bind(K, 182, HEAP);
          CHECK(mem.select(P::Host{K, PSO, HEAP}, 182, g1, &e0, sel, P::kMaxEmit, oc) == 1); mem.end_frame(&g1, 182, 2000, oc); }
        // a frame with a wanted card and no host draw at all: nohost
        const P::Stats s = mem.end_frame(&g1, 200, 3200, oc);
        CHECK(s.nohost == 1 && mem.run.nohost == 1);
        // the eye looking away: not visible, not selected (offeye)
        { const unsigned n = mem.select(P::Host{K}, 201, g1, &eb, sel, P::kMaxEmit, oc); const P::Stats s2 = mem.end_frame(&g1, 201, 3216, oc); CHECK(n == 0 && s2.offeye == 1 && s2.nohost == 0); }
    }
    // ---- the selection order (host key first, far to near) and the per-key groups of build_eye
    {
        static P::Memory mem;
        const double A[3] = {0, 0, 20000}, B[3] = {-4000, 2000, 30000}, C[3] = {5000, -1000, 25000};
        for (uint64_t f = 2; f < 4; ++f) {
            see(mem, g0, kFwdZ, A, 2000, K, f, 0, oc, nullptr, 0, PSO, HEAP);
            see(mem, g0, kFwdZ, B, 2000, K, f, 0, oc, nullptr, 0, PSO, HEAP);
            see(mem, g0, kFwdZ, C, 2000, K2, f, 0, oc, nullptr, 0, PSO, HEAP);
            mem.end_frame(&g0, f, 0, oc);
        }
        { const double W[3] = {20000, 0, 0}; M::Vtx v[4]; card(g1, kFwdX, W, 10, v);   // (F1) the game binds K in frame 4 before the K2 host (a whole run)
          mem.observe(g1, v, 4, kIdx, 4, 0, true, K, 4, 16, oc, nullptr, nullptr, 0, PSO, HEAP); }
        const unsigned n = mem.select(P::Host{K2, PSO, HEAP}, 4, g1, &e0, sel, P::kMaxEmit, oc);
        CHECK(n == 3);
        CHECK(mem.p[sel[0]].key == K2 && std::fabs(mem.p[sel[1]].c[2] - 30000.0) < 1.0 && std::fabs(mem.p[sel[2]].c[2] - 20000.0) < 1.0);
        M::Vtx out[P::kMaxEmitVerts]; P::Group gr[P::kMaxKeys]; unsigned ng = 0;
        CHECK(mem.build_eye(e0, 1.0, 1.0, sel, n, 16, oc, out, P::kMaxEmitVerts, gr, &ng) == 18);
        CHECK(ng == 2 && gr[0].key == K2 && gr[0].first == 0 && gr[0].count == 6 && gr[1].key == K && gr[1].first == 6 && gr[1].count == 12);
        const P::Stats s = mem.end_frame(&g1, 4, 16, oc);
        CHECK(s.xkey == 2 && s.cemitted == 3 && s.emitted == 3);
    }
    // ---- the emission cap: 200 wanted clouds, the nearest kMaxEmit are taken
    {
        static P::Memory mem;
        for (uint64_t f = 2; f < 4; ++f) {
            for (unsigned i = 0; i < 200; ++i) {
                const double C[3] = {-9500.0 + 1000.0 * double(i % 20), -4500.0 + 1000.0 * double(i / 20), 20000.0 + 10.0 * double(i)};
                see(mem, g0, kFwdZ, C, 600, K, f, 0, oc);   // 1,200-unit cloud cards 1,000 apart
            }
            mem.end_frame(&g0, f, 0, oc);
        }
        CHECK(mem.live_of(P::kCloud) == 200);
        const unsigned n = mem.select(P::Host{K}, 4, g1, &e0, sel, P::kMaxEmit, oc);
        CHECK(n == P::kMaxEmit);
        double dmax = 0; for (unsigned k = 0; k < n; ++k) dmax = std::fmax(dmax, mem.p[sel[k]].c[2]);
        CHECK(dmax < 20000.0 + 10.0 * 199.0 - 1.0);   // the farthest card is not among them
        const P::Stats s = mem.end_frame(&g1, 4, 16, oc);
        CHECK(s.selected == P::kMaxEmit);
    }
    // ---- the fades: smoke age, cloud age tail, cache edge (distance) and near
    {
        P::Puff x; x.used = true; x.kind = P::kCloud; x.edge = 6000; x.seen_ms = 0;
        const P::Options o = P::options(8.0, 1500.0, P::kClsNear, 2500.0, 100.0, 40000.0);
        CHECK(P::fade(x, 0, 20000, o) == 1.0);
        CHECK(std::fabs(P::fade(x, 0, 35000, o) - 0.5) < 1e-9);       // halfway through the cache edge band (30,000 .. 40,000)
        CHECK(P::fade(x, 0, 40000, o) == 0.0 && P::fade(x, 0, 39000, o) > 0.0 && P::fade(x, 0, 31000, o) < 1.0);
        CHECK(P::fade(x, 80000, 20000, o) == 1.0 && std::fabs(P::fade(x, 90000, 20000, o) - 0.5) < 1e-9 && P::fade(x, 100000, 20000, o) == 0.0);
        CHECK(P::fade(x, 0, 1000, o) == 0.0 && std::fabs(P::fade(x, 0, 3000, o) - 0.5) < 1e-9);   // near: 0 under 1,500, full over 4,500
        x.kind = P::kSmoke;
        CHECK(std::fabs(P::fade(x, 2000, 100, o) - 0.75) < 1e-12 && P::fade(x, 8000, 100, o) == 0.0);   // smoke: 1 - age / life, no distance fades
        // build_eye applies the fade: a cloud beyond the cache edge is not drawn at all
        static P::Memory mem;
        const P::Options of = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, 25000.0);
        const double C[3] = {0, 0, 30000};
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, of); mem.end_frame(&g0, 2, 0, of);
        see(mem, g0, kFwdZ, C, 3000, K, 3, 0, of); mem.end_frame(&g0, 3, 0, of);
        const unsigned n = mem.select(P::Host{K}, 4, g1, &e0, sel, P::kMaxEmit, of);
        M::Vtx out[12];
        CHECK(n == 1 && mem.build_eye(e0, 1.0, 1.0, sel, n, 0, of, out, 12) == 0);
        // closer (d 20,000 inside the 18,750 .. 25,000 edge band): drawn, alpha x the smooth cache-edge fade
        static P::Memory m2; const double D[3] = {0, 0, 20000};
        see(m2, g0, kFwdZ, D, 3000, K, 2, 0, of); m2.end_frame(&g0, 2, 0, of);
        see(m2, g0, kFwdZ, D, 3000, K, 3, 0, of); m2.end_frame(&g0, 3, 0, of);
        const unsigned n2 = m2.select(P::Host{K}, 4, g1, &e0, sel, P::kMaxEmit, of);
        CHECK(n2 == 1 && m2.build_eye(e0, 1.0, 1.0, sel, n2, 0, of, out, 12) == 6);
        const double want = 0.5 * P::smooth01((25000.0 - 20000.0) / (25000.0 - 0.75 * 25000.0));
        CHECK(std::fabs(double(out[0].col[3]) - want) < 1e-4);
    }
    // ---- the cache drop: a cloud farther than kCloudDropFar x fxcfar from the game camera is gone
    {
        static P::Memory mem;
        const double C[3] = {0, 0, 20000};
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc); mem.end_frame(&g0, 2, 0, oc);
        const Cam away = {{0, 0, -40000}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};   // 60,000 from the cloud, looking +x
        const M::ScreenMap ga = game_map(away);
        const P::Stats s = mem.end_frame(&ga, 3, 16, oc);
        CHECK(s.gone == 1 && mem.live() == 0);
        // expiry after the cloud life (looked away, never seen again)
        static P::Memory m2;
        see(m2, g0, kFwdZ, C, 3000, K, 2, 0, oc); m2.end_frame(&g0, 2, 0, oc);
        m2.end_frame(&g1, 3, 120000, oc); CHECK(m2.live() == 1);
        const P::Stats s2 = m2.end_frame(&g1, 4, 120001, oc); CHECK(s2.expired == 1 && s2.cexpired == 1 && m2.live() == 0);
    }
    // ---- the collapse of the game's clipped part works for a cloud re-emitted under another key's host
    {
        static P::Memory mem;
        const double C[3] = {0, 0, 20000};
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc, nullptr, 0, PSO, HEAP); mem.end_frame(&g0, 2, 0, oc);
        see(mem, g0, kFwdZ, C, 3000, K, 3, 0, oc, nullptr, 0, PSO, HEAP); mem.end_frame(&g0, 3, 0, oc);
        const Cam turned = {{0, 0, 0}, {std::cos(0.55), 0, -std::sin(0.55)}, {0, 1, 0}, {std::sin(0.55), 0, std::cos(0.55)}};
        const M::ScreenMap gt = game_map(turned);
        bind_key(mem, K, 4, oc, PSO, HEAP);   // (F1) the game binds K earlier in frame 4
        CHECK(mem.select(P::Host{K2, PSO, HEAP}, 4, gt, &e0, sel, P::kMaxEmit, oc) == 1);
        M::Vtx cv[4]; card(gt, turned, C, 3000, cv);
        for (auto& x : cv) x.pos[0] = std::fmax(x.pos[0], 1792.0f);   // the game's clip at its left border
        uint8_t col[8] = {};
        CHECK(mem.observe(gt, cv, 4, kIdx, 4, 0, false, K, 4, 0, oc, col, nullptr, 0, PSO, HEAP) == 1 && col[0] && col[3]);
        const P::Stats s = mem.end_frame(&gt, 4, 0, oc);
        CHECK(s.collapsed == 1 && s.clipped == 1);
    }
    // ---- (M4b, step146) cdrift: a world-static cloud refreshed for 30 frames stays at its birth centre; a card that follows the camera (200 units per
    // frame, below the 0.25 x edge refresh radius) is ALSO refreshed every frame (cref >> cst) but its drift grows: cdrift tells the two apart
    {
        static P::Memory ms, mm;
        double dmax = 0;
        for (uint64_t f = 2; f < 32; ++f) {
            const double S[3] = {0, 0, 20000}, Mv[3] = {200.0 * double(f - 2), 0, 20000};
            see(ms, g0, kFwdZ, S, 3000, K, f, f * 16, oc); const P::Stats a = ms.end_frame(&g0, f, f * 16, oc);
            see(mm, g0, kFwdZ, Mv, 3000, K, f, f * 16, oc); const P::Stats b = mm.end_frame(&g0, f, f * 16, oc);
            CHECK(a.stored + a.refreshed == 1 && b.stored + b.refreshed == 1);
            dmax = std::fmax(dmax, b.cdrift);
        }
        CHECK(ms.run.cstored == 1 && ms.run.crefreshed == 29 && ms.run.cdrift < 1.0);
        CHECK(mm.run.cstored == 1 && mm.run.crefreshed == 29 && std::fabs(mm.run.cdrift - 5800.0) < 2.0 && dmax == mm.run.cdrift);
        char b[600]; P::format_mem_line(b, sizeof b, 9, mm.run, mm.run, 0, 1, oc);
        CHECK(std::strstr(b, ";cdrift=5800;") && std::strstr(b, ",cdrift:5800"));
    }
    // ---- (M4b) a cross-key selection handed back by unselect leaves xkey (xkey = cross-key cards a host kept)
    {
        static P::Memory mem;
        const double C[3] = {0, 0, 20000};
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc, nullptr, 0, PSO, HEAP); mem.end_frame(&g0, 2, 0, oc);
        see(mem, g0, kFwdZ, C, 3000, K, 3, 0, oc, nullptr, 0, PSO, HEAP); mem.end_frame(&g0, 3, 0, oc);
        bind_key(mem, K, 4, oc, PSO, HEAP);   // (F1)
        CHECK(mem.select(P::Host{K2, PSO, HEAP}, 4, g1, &e0, sel, P::kMaxEmit, oc) == 1);
        mem.unselect(4, sel, 1, "trail-window-fallback");
        CHECK(mem.select(P::Host{K}, 4, g1, &e0, sel, P::kMaxEmit, oc) == 1);   // its own key takes it: not a cross-key draw
        const P::Stats s = mem.end_frame(&g1, 4, 16, oc);
        CHECK(s.xkey == 0 && s.selected == 1 && s.hosts == 2);
    }
    // ---- (M4c, POV runPOV2b f5207) a game card CUT at the window border with no whole stored copy: fade (default) its border vertices / hide / leave it
    {
        const Cam turned = {{0, 0, 0}, {std::cos(0.55), 0, -std::sin(0.55)}, {0, 1, 0}, {std::sin(0.55), 0, std::cos(0.55)}};   // the card at the left border
        const M::ScreenMap gt = game_map(turned);
        const double C[3] = {0, 0, 20000};
        M::Vtx cv[4]; card(gt, turned, C, 3000, cv);
        CHECK(cv[0].pos[0] < 1792.0f && cv[1].pos[0] > 1792.0f && cv[2].pos[0] < 1792.0f && cv[3].pos[0] > 1792.0f);
        for (auto& x : cv) x.pos[0] = std::fmax(x.pos[0], 1792.0f);   // the game's clip
        auto cut = [&](const P::Options& o, uint8_t* col, uint8_t* fade, const float (*obj)[3] = nullptr, unsigned nobj = 0) {
            auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;   // fresh (heap: ~230 KB)
            const unsigned r = mem.observe(gt, cv, 4, kIdx, 4, 0, false, K, 2, 0, o, col, obj, nobj, 0, 0, fade);
            const P::Stats s = mem.end_frame(&gt, 2, 0, o);
            return std::make_pair(r, s);
        };
        CHECK(oc.clip == P::kClipFade && std::strcmp(P::clip_name(P::kClipHide), "hide") == 0);
        {   // fade (default): the two border vertices, nothing collapsed
            uint8_t col[4] = {}, fade[4] = {};
            const auto rs = cut(oc, col, fade);
            CHECK(rs.first == 0 && rs.second.efade == 1 && rs.second.ehide == 0 && rs.second.clipped == 1 && rs.second.collapsed == 0);
            CHECK(fade[0] && !fade[1] && fade[2] && !fade[3] && !col[0] && !col[1] && !col[2] && !col[3]);
        }
        {   // hide: the whole cut card collapsed
            const P::Options oh = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, P::kClipHide);
            uint8_t col[4] = {}, fade[4] = {};
            const auto rs = cut(oh, col, fade);
            CHECK(rs.first == 1 && rs.second.ehide == 1 && rs.second.efade == 0 && col[0] && col[1] && col[2] && col[3] && !fade[0]);
        }
        {   // off: the game's cut card as drawn (M4)
            const P::Options of = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, P::kClipOff);
            uint8_t col[4] = {}, fade[4] = {};
            const auto rs = cut(of, col, fade);
            CHECK(rs.first == 0 && rs.second.efade == 0 && rs.second.ehide == 0 && !fade[0] && !col[0]);
            CHECK(P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, 7).clip == P::kClipFade);   // a bad mode: the default
        }
        {   // smoke only (fxtrail, near rule): a cut card near an aircraft is faded, the same card far from every aircraft is not (no fxcloud)
            const P::Options os = P::options(8.0, 1500.0);
            const float objNear[1][3] = {{0.0f, 0.0f, 21000.0f}}, objFar[1][3] = {{90000.0f, 0.0f, 0.0f}};
            uint8_t col[4] = {}, fade[4] = {};
            auto rs = cut(os, col, fade, objNear, 1);
            CHECK(rs.second.efade == 1 && fade[0] && fade[2]);
            uint8_t fade2[4] = {};
            rs = cut(os, col, fade2, objFar, 1);
            CHECK(rs.second.efade == 0 && !fade2[0] && !fade2[2]);
        }
        {   // a cut card below cloud size with only fxcloud: untouched; no fade array: counted nothing, nothing written
            M::Vtx sm[4]; card(gt, turned, C, 300, sm);
            for (auto& x : sm) x.pos[0] = std::fmax(x.pos[0], 1792.0f);
            static P::Memory mem; uint8_t col[4] = {}, fade[4] = {};
            mem.observe(gt, sm, 4, kIdx, 4, 0, false, K, 2, 0, oc, col, nullptr, 0, 0, 0, fade);
            const P::Stats s = mem.end_frame(&gt, 2, 0, oc);
            CHECK(s.clipped == 1 && s.efade == 0 && !fade[0] && !fade[2]);
            uint8_t col2[4] = {}; const auto rs = cut(oc, col2, nullptr);
            CHECK(rs.second.efade == 0 && !col2[0]);
        }
        {   // a whole stored copy re-emitted this frame still REPLACES the cut card (collapse), not fade
            static P::Memory mem;
            see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc); mem.end_frame(&g0, 2, 0, oc);
            see(mem, g0, kFwdZ, C, 3000, K, 3, 0, oc); mem.end_frame(&g0, 3, 0, oc);
            CHECK(mem.select(P::Host{K}, 4, gt, &e0, sel, P::kMaxEmit, oc) == 1);
            uint8_t col[4] = {}, fade[4] = {};
            CHECK(mem.observe(gt, cv, 4, kIdx, 4, 0, false, K, 4, 0, oc, col, nullptr, 0, 0, 0, fade) == 1);
            const P::Stats s = mem.end_frame(&gt, 4, 0, oc);
            CHECK(s.collapsed == 1 && s.efade == 0 && col[0] && col[3] && !fade[0]);
        }
        {   // the eye copies: fxSprVtx alpha 0 on the faded vertices gives a ramp across the visible part (plan_draw copies the colour)
            uint8_t fade[4] = {}; uint8_t col[4] = {};
            cut(oc, col, fade);
            M::Vtx v[4]; for (int k = 0; k < 4; ++k) { v[k] = cv[k]; if (fade[k]) v[k].col[3] = 0.0f; }
            const M::ScreenMap e1 = eye_map(turned);
            const M::ScreenMap* eyes[2] = {&e1, &e1};
            M::Vtx scratch[4], k0[4], k1[4]; uint8_t bad[4]; FS::Plan pl;
            FS::plan_draw(gt, eyes, v, 4, kIdx, 4, 0, 1.0, scratch, bad, pl, k0, k1, nullptr, 0.0, false, nullptr);
            CHECK(pl.prepared == 1 && k0[0].col[3] == 0.0f && k0[2].col[3] == 0.0f && k0[1].col[3] == 0.5f && k1[3].col[3] == 0.5f);
        }
    }
    // ---- (M5, headset test 7 2026-10-31) a cloud the game keeps drawing CUT at its window border stays alive: the cut sighting renews its life. M4 aged it
    // from its last WHOLE sighting only: the session's last 2 minutes had whole=0 with 6 cut cloud draws per frame and the 21 stored clouds expired (c21 -> c0,
    // cexp 21) 120 s after their last whole sighting, while the game was still drawing them.
    {
        auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;
        const Cam turned = {{0, 0, 0}, {std::cos(0.55), 0, -std::sin(0.55)}, {0, 1, 0}, {std::sin(0.55), 0, std::cos(0.55)}};   // the card at the left border
        const M::ScreenMap gt = game_map(turned);
        const double C[3] = {0, 0, 20000};
        see(mem, g0, kFwdZ, C, 3000, K, 2, 0, oc); mem.end_frame(&g0, 2, 0, oc);
        see(mem, g0, kFwdZ, C, 3000, K, 3, 16, oc); mem.end_frame(&g0, 3, 16, oc);
        M::Vtx cv[4]; card(gt, turned, C, 3000, cv);
        for (auto& x : cv) x.pos[0] = std::fmax(x.pos[0], 1792.0f);   // the game's clip at its left border
        uint32_t cexp = 0, renew = 0; uint64_t now = 16;
        for (uint64_t f = 4; f < 154; ++f) {   // 150 s of cut sightings, one per second (cloud life 120 s)
            now = 16 + (f - 3) * 1000;
            mem.observe(gt, cv, 4, kIdx, 4, 0, false, K, f, now, oc, nullptr);
            const P::Stats s = mem.end_frame(&gt, f, now, oc);
            cexp += s.cexpired; renew += s.renew;
        }
        CHECK(cexp == 0 && mem.live_of(P::kCloud) == 1);
        CHECK(renew == 150);
        // ... and drawn at full alpha (not in the age tail) where the game does not show it whole: the head looks at it, the nose has turned away
        const unsigned n = mem.select(P::Host{K}, 154, g1, &e0, sel, P::kMaxEmit, oc);
        M::Vtx out[P::kMaxEmitVerts];
        const unsigned v = mem.build_eye(e0, 1.0, 1.0, sel, n, now + 16, oc, out, P::kMaxEmitVerts);
        CHECK(n == 1 && v == 6 && std::fabs(out[0].col[3] - 0.5f) < 1e-3f);
        mem.end_frame(&g1, 154, now + 16, oc);
        // the cut sightings stop: it expires 120 s after the last one, as before
        P::Stats e = mem.end_frame(&g1, 155, now + 120000, oc); CHECK(e.cexpired == 0 && mem.live() == 1);
        e = mem.end_frame(&g1, 156, now + 120001, oc); CHECK(e.cexpired == 1 && mem.live() == 0);
    }
    // ---- (M5) a smoke puff that drifts 0.4 x its edge per frame is ONE stored puff, refreshed every frame and re-emitted once the nose has turned away.
    // M4 matched within 0.25 x the edge: headset test 7 stored 1,138 smoke cards, culled 1,090, and re-emitted NONE (run emitted 87,112 == cemit 87,112).
    {
        auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;
        const P::Options o = P::options(8.0, 1500.0);   // fxtrail:8 (near rule), no clouds
        for (uint64_t f = 2; f < 6; ++f) {
            const double C[3] = {40.0 * double(f), 0, 5000};   // a 100-unit puff drifting 40 units per frame
            const float obj[1][3] = {{float(C[0]) + 300.0f, 0.0f, 5000.0f}};   // the smoking aircraft beside it
            see(mem, g0, kFwdZ, C, 50, K, f, f * 16, o, obj, 1);
            mem.end_frame(&g0, f, f * 16, o);
        }
        CHECK(mem.run.stored == 1 && mem.run.refreshed == 3 && mem.run.culled == 0 && mem.live_of(P::kSmoke) == 1);
        CHECK(mem.run.swide == 3 && mem.run.scam == 0);
        const unsigned n = mem.select(P::Host{K}, 6, g1, &e0, sel, P::kMaxEmit, o);
        M::Vtx out[P::kMaxEmitVerts];
        CHECK(n == 1 && mem.build_eye(e0, 1.0, 1.0, sel, n, 6 * 16, o, out, P::kMaxEmitVerts) == 6);
        mem.end_frame(&g1, 6, 6 * 16, o);
        // two puffs of one trail half an edge apart still keep their own cards (the nearest unrefreshed card wins)
        auto m2p = std::make_unique<P::Memory>(); P::Memory& m2 = *m2p;
        const float obj[1][3] = {{0.0f, 300.0f, 5000.0f}};
        for (uint64_t f = 2; f < 5; ++f) {
            const double A[3] = {0, 0, 5000}, B[3] = {50, 0, 5000};
            if (f % 2) { see(m2, g0, kFwdZ, B, 50, K, f, f * 16, o, obj, 1); see(m2, g0, kFwdZ, A, 50, K, f, f * 16, o, obj, 1); }
            else { see(m2, g0, kFwdZ, A, 50, K, f, f * 16, o, obj, 1); see(m2, g0, kFwdZ, B, 50, K, f, f * 16, o, obj, 1); }
            m2.end_frame(&g0, f, f * 16, o);
        }
        CHECK(m2.run.stored == 2 && m2.run.refreshed == 4 && m2.run.dup == 0 && m2.live() == 2);
        for (const auto& x : m2.p) if (x.used) CHECK(x.seen_n == 3 && (std::fabs(x.c[0]) < 0.5 || std::fabs(x.c[0] - 50.0) < 0.5));
    }
    // ---- (M5) a world-static puff whose unprojection jitters by one camera step (every other frame drawn with the previous frame's camera; the game camera
    // moves 270 units per frame = 250 m/s at 65 units per metre) is still one puff: refreshed within 1.25 x the camera step
    {
        auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;
        const P::Options o = P::options(8.0, 1500.0);
        const double Pw[3] = {0, 0, 20000};
        const float obj[1][3] = {{300.0f, 0.0f, 20000.0f}};
        for (uint64_t f = 2; f < 8; ++f) {
            const Cam cf = {{0, 0, 270.0 * double(f)}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            const Cam cr = {{0, 0, 270.0 * double(f - (f % 2))}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // odd frames: drawn with the previous camera
            const M::ScreenMap gf = game_map(cf), gr = game_map(cr);
            M::Vtx v[4]; card(gr, cr, Pw, 50, v);
            mem.observe(gf, v, 4, kIdx, 4, 0, false, K, f, f * 16, o, nullptr, obj, 1);
            mem.end_frame(&gf, f, f * 16, o);
        }
        CHECK(std::fabs(mem.cam_step(7) - 270.0) < 1.0 && mem.cam_step(8) == 0.0);
        CHECK(mem.run.stored == 1 && mem.run.refreshed == 5 && mem.live_of(P::kSmoke) == 1 && mem.run.scam == 5);
        // a camera cut (a jump beyond kMaxCamStep) adds no slack: the jittered puff is a new card
        const Cam far1 = {{0, 0, -5000}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const M::ScreenMap gfar = game_map(far1);
        const double Q[3] = {0, 0, 2000}; M::Vtx v[4]; card(gfar, far1, Q, 50, v);
        mem.observe(gfar, v, 4, kIdx, 4, 0, false, K, 8, 8 * 16, o, nullptr, obj, 1);
        CHECK(mem.cam_step(8) == 0.0);
        mem.end_frame(&gfar, 8, 8 * 16, o);
    }
    // ---- (M5) the per-card-frame telemetry: want / unbound (F1: a wanted card whose key no game draw binds in the frame cannot be drawn), kchurn
    {
        auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;
        const double A[3] = {0, 0, 20000}, B[3] = {-6000, 0, 22000};
        for (uint64_t f = 2; f < 4; ++f) { see(mem, g0, kFwdZ, A, 2000, K, f, f * 16, oc); see(mem, g0, kFwdZ, B, 2000, K2, f, f * 16, oc); mem.end_frame(&g0, f, f * 16, oc); }
        // frame 4: the nose has turned away (g1); the game draws only key K (one host); the head looks at both clouds
        bind_key(mem, K, 4, oc, 0, 0);
        const unsigned n = mem.select(P::Host{K}, 4, g1, &e0, sel, P::kMaxEmit, oc);
        const P::Stats s = mem.end_frame(&g1, 4, 64, oc);
        CHECK(n == 1 && s.want == 2 && s.unbound == 1 && s.nomatch == 1);
        // frame 5: both keys bound, both drawn: nothing unbound
        bind_key(mem, K, 5, oc, 0, 0); bind_key(mem, K2, 5, oc, 0, 0);
        unsigned n5 = mem.select(P::Host{K}, 5, g1, &e0, sel, P::kMaxEmit, oc); n5 += mem.select(P::Host{K2}, 5, g1, &e0, sel, P::kMaxEmit, oc);
        const P::Stats s5 = mem.end_frame(&g1, 5, 80, oc);
        CHECK(n5 == 2 && s5.want == 2 && s5.unbound == 0 && mem.run.unbound == 1);
        // a whole card at A under a NEW key K3: no card of K3 to refresh, but A's card (K) lies there: key churn (a new card is stored as before)
        see(mem, g0, kFwdZ, A, 2000, 0x15678a0012a2c0ull, 6, 96, oc);
        const P::Stats s6 = mem.end_frame(&g0, 6, 96, oc);
        CHECK(s6.kchurn == 1 && s6.cstored == 1);
        see(mem, g0, kFwdZ, A, 2000, K, 7, 112, oc);   // its own key: a refresh, no churn
        CHECK(mem.end_frame(&g0, 7, 112, oc).kchurn == 0);
    }
    // ---- the fx-puff-mem line (< 448 bytes with the 21-byte prefix at every cap) and the fx-puff-trail line unchanged in length
    {
        P::Stats f;
        uint32_t* all[] = {&f.cstored, &f.crefreshed, &f.dup, &f.hosts, &f.nohost, &f.xkey, &f.nomatch, &f.offeye, &f.gone, &f.cexpired, &f.cemitted,
                           &f.efade, &f.ehide, &f.want, &f.unbound, &f.renew, &f.kchurn, &f.synth, &f.streams};
        for (uint32_t* x : all) *x = 4000000000u;
        f.cdrift = 1e12;   // capped at 999999
        char b[600];
        const P::Options o = P::options(30.0, 1500.0, P::kClsNear, 2500.0, 600.0);
        P::format_mem_line(b, sizeof b, ~0ull, f, f, P::kCap, P::kCloudCap, o);
        std::printf("  fx-puff-mem line at every cap: %zu bytes with the prefix\n", std::strlen(b) + std::strlen("gpu-copy-step1;event="));
        CHECK(std::strstr(b, ";want=99999;unb=99999;renew=99999;kch=99999;syn=99999;strm=99999;run=") && std::strstr(b, ",unb:99999"));
        CHECK(std::strlen(b) + std::strlen("gpu-copy-step1;event=") < 448 && std::strncmp(b, "fx-puff-mem;frame=", 18) == 0);
        CHECK(std::strstr(b, ";smoke=30;cloud=600;xk=1;live=s128,c384;cst=99999;") && std::strstr(b, ";nohost=99999;") && std::strstr(b, "run=cst:99999,"));
        P::Stats z; P::format_mem_line(b, sizeof b, 5, z, z, 0, 0, oc);
        CHECK(std::strstr(b, ";smoke=0;cloud=120;xk=1;live=s0,c0;cst=0;") && std::strstr(b, ",dup:0"));
        CHECK(std::strstr(b, ";clip=fade;efade=0;ehide=0;") && std::strstr(b, ",efade:0,ehide:0"));
    }
    // ---- options (fxcloud / fxcfar / fxtxkey) and the stage switches
    {
        std::string id, why;
        { T::Conventions c; CHECK(c.fx_cloud_s == 0.0f && c.fx_cloud_far == 40000.0f && c.fx_trail_xkey); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxmem:6;fxtrail:8;fxcloud:120", id, c, why) && c.fx_cloud_s == 120.0f && c.fx_trail_s == 8.0f); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxmem:6;fxcloud:300;fxcfar:60000;fxtxkey:0", id, c, why) && c.fx_cloud_s == 300.0f &&
                                  c.fx_cloud_far == 60000.0f && !c.fx_trail_xkey); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxcloud:0", id, c, why) && c.fx_cloud_s == 0.0f); }
        { T::Conventions c; CHECK(c.fx_trail_clip == 1); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxmem:6;fxcloud:120;fxtclip:hide", id, c, why) && c.fx_trail_clip == 2); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxtclip:off", id, c, why) && c.fx_trail_clip == 0); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxtclip:1", id, c, why) && c.fx_trail_clip == 1); }
        const char* bad[] = {"m03k;fxcloud:5", "m03k;fxcloud:601", "m03k;fxcloud:x", "m03k;fxcfar:4000", "m03k;fxcfar:300000", "m03k;fxcfar:x", "m03k;fxtxkey:2", "m03k;fxtclip:soft", "m03k;fxtclip:3"};
        for (const char* b : bad) { T::Conventions cc; CHECK(!T::parse_stage_option(b, id, cc, why)); }
        // EFFECTS-OUTSIDE (P) fxadd:0|1 (default 0); the producer switch only with fxmem:6
        { T::Conventions c; CHECK(!c.fx_add); CHECK(T::parse_stage_option("m03k;fxmem:6;fxadd:1", id, c, why) && c.fx_add); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxadd:0", id, c, why) && !c.fx_add); }
        { T::Conventions c; CHECK(!T::parse_stage_option("m03k;fxadd:2", id, c, why)); }
        CHECK(s5::stage_effects(6, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{}, false, true).additive);
        for (int m = 0; m <= 5; ++m) CHECK(!s5::stage_effects(m, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{}, false, true).additive);
        const s5::PuffCloud pc{120.0f, 60000.0f, false};
        const auto e6 = s5::stage_effects(6, false, 0, true, 0.0f, false, 8.0f, 1500.0f, 0, 2500.0f, pc);
        CHECK(e6.puff_cloud_s == 120.0f && e6.puff_cloud_far == 60000.0f && !e6.puff_xkey && e6.puff_trail_s == 8.0f);
        for (int m = 0; m <= 5; ++m) CHECK(s5::stage_effects(m, false, 0, true, 0.0f, false, 8.0f, 1500.0f, 0, 2500.0f, pc).puff_cloud_s == 0.0f);   // fxmem:6 only
        CHECK(s5::stage_effects(6, false).puff_cloud_s == 0.0f && s5::stage_effects(6, false).puff_xkey);
        s5::apply_stage_effects(e6);
        CHECK(s5::effects_puff_cloud_life_s().load() == 120.0f && s5::effects_puff_cloud_far().load() == 60000.0f && !s5::effects_puff_trail_xkey().load());
        s5::clear_stage_effects();
        CHECK(s5::effects_puff_cloud_life_s().load() == 0.0f && s5::effects_puff_cloud_far().load() == 40000.0f && s5::effects_puff_trail_xkey().load());
        { auto eh = s5::stage_effects(6, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{120.0f, 40000.0f, true, 2});
          CHECK(eh.puff_clip == 2); s5::apply_stage_effects(eh); CHECK(s5::effects_puff_clip_mode().load() == 2);
          s5::clear_stage_effects(); CHECK(s5::effects_puff_clip_mode().load() == 1);
          CHECK(s5::stage_effects(6, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{120.0f, 40000.0f, true, 9}).puff_clip == 1); }
        char b[420];
        CHECK(!s5::fx_puff_trail_line(b, sizeof b, 6, 0.0f, 1500.0f));
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{120.0f, 40000.0f, true}) &&
              std::strstr(b, ";active=1;reason=world-space-puff-memory-re-emitted-per-eye;cloud-s=120;cfar=40000;xkey=1;clip=fade;") && std::strstr(b, "#20"));
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 30.0f, 100000.0f, 1, 50000.0f, s5::PuffCloud{600.0f, 200000.0f, false}) && std::strlen(b) < 400);
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 5, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{120.0f}) && std::strstr(b, "active=0;reason=needs-fxmem-6"));
    }
    orientation_tests();
    // ---- (M7, headset test 8) plume continuation: an emitter that leaves the game window keeps a plume (docs section 25)
    {
        static const Cam kDiag = {{0, 0, 0}, {0.70710678, 0, -0.70710678}, {0, 1, 0}, {0.70710678, 0, 0.70710678}};
        const M::ScreenMap ed = eye_map(kDiag);
        static P::Memory memS; P::Memory& mem = memS; uint16_t selp[P::kMaxEmit];
        unsigned synthFrames = 0, selSynth = 0, verts = 0;
        double lastSynthX = 0;
        for (uint64_t f = 1; f <= 90; ++f) {
            const double x = 400.0 * double(f);
            const double w[3] = {x, 0, 15000.0};
            const float obj[1][3] = {{float(w[0]), 0.0f, 15000.0f}};
            const bool inGame = x <= 8800.0;
            if (inGame) see(mem, g0, kFwdZ, w, 30.0, K, f, f * 16, ob, obj, 1, PSO, HEAP);
            else bind_key(mem, K, f, ob, PSO, HEAP);           // the game still draws other cards of the key (a host draw)
            if (f > 40) {   // the plume has left the window: a host draw of the key replays per eye
                const unsigned n = mem.select(P::Host{K, PSO, HEAP}, f, g0, &ed, selp, P::kMaxEmit, ob);
                for (unsigned i = 0; i < n; ++i) if (mem.p[selp[i]].synth) { ++selSynth; lastSynthX = std::fmax(lastSynthX, mem.p[selp[i]].c[0]); }
                static P::Vtx out[P::kMaxEmitVerts]; P::Group gr[P::kMaxKeys]; unsigned ng = 0;
                verts += mem.build_eye(ed, 1.0, 1.0, selp, n, f * 16, ob, out, P::kMaxEmitVerts, gr, &ng);
            }
            const P::Stats s = mem.end_frame(&g0, f, f * 16, ob);
            if (s.synth) ++synthFrames;
            if (f == 10) CHECK(s.synth == 0 && s.streams == 1);                 // inside the window: a stream follows the emitter, nothing is spawned
        }
        CHECK(mem.run.synth > 30);                                               // one synthetic card per frame once the emitter left (frames 23..90)
        CHECK(synthFrames > 30 && synthFrames <= 68);
        CHECK(selSynth > 20 && verts > 6 * 20);                                  // the synthetic cards are selected and built for the eye
        CHECK(lastSynthX > 32000.0 && lastSynthX < 36400.0);           // they follow the extrapolated emitter, not the last real puff (x = 8,800)
        CHECK(mem.live_of(P::kSmoke) <= P::kCap);
        // a synthetic card is world-up like every card: the stored half extents are the template's (no roll state is added)
        for (const auto& x : mem.p) if (x.used && x.synth) { CHECK(x.hu > 0 && x.hv > 0 && x.kind == P::kSmoke && x.seen_n >= P::kMinSeen); break; }
    }
    // ---- (M8, headset test 9) the synthetic cards must be DRAWABLE where the game binds another key: they carry the atlas tile of the stream, which outlives
    // the real donor cards and the cap evictions (test 9: syn 60/s but unb ~ want, ~100 cards drawn of 8,000 wanted per second)
    {
        static const Cam kDiag = {{0, 0, 0}, {0.70710678, 0, -0.70710678}, {0, 1, 0}, {0.70710678, 0, 0.70710678}};
        const M::ScreenMap ed = eye_map(kDiag);
        const P::Options oa = P::options(8.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, P::kClipFade, true);
        static P::Memory memS; P::Memory& mem = memS; uint16_t selp[P::kMaxEmit];
        uint16_t tl[8];
        unsigned lateSynth = 0, lateAtlas = 0; unsigned tileless = 0, streamTile = 0;
        for (uint64_t f = 1; f <= 250; ++f) {
            const double x = 7000.0 + 120.0 * double(f), xp = x - 120.0;
            const double w[3] = {x, 0, 15000.0}, wp[3] = {xp, 0, 15000.0};
            const float obj[1][3] = {{float(w[0]), 0.0f, 15000.0f}};
            mem.atlas.slot_started(0); mem.set_slot(0);
            if (x <= 8800.0) {
                see(mem, g0, kFwdZ, w, 30.0, K, f, f * 16, oa, obj, 1, PSO, HEAP);
                { P::atlas::Rect rr[8]; mem.plan_captures(f, oa, tl, rr, 8); }
                if (f > 1) { see(mem, g0, kFwdZ, wp, 30.0, K, f, f * 16, oa, obj, 1, PSO, HEAP);   // the previous card again: seen whole twice, it gets a tile
                             P::atlas::Rect rr[8]; mem.plan_captures(f, oa, tl, rr, 8); }
            } else bind_key(mem, K2, f, oa, PSO, HEAP);        // the game draws only ANOTHER key: key K is never bound
            mem.atlas.slot_submitted(0);
            if (x > 8800.0) {
                P::Host h{K2, PSO, HEAP, true};                 // an atlas-capable host of the game PSO
                const unsigned n = mem.select(h, f, g0, &ed, selp, P::kMaxEmit, oa);
                unsigned ns = 0; for (unsigned i = 0; i < n; ++i) if (mem.p[selp[i]].synth && mem.p[selp[i]].emit_atlas) ++ns;
                if (f >= 200) { lateSynth += ns; ++lateAtlas; }
                static P::Vtx out[P::kMaxEmitVerts]; P::Group gr[P::kMaxKeys]; unsigned ng = 0;
                mem.build_eye(ed, 1.0, 1.0, selp, n, f * 16, oa, out, P::kMaxEmitVerts, gr, &ng, &g0);
            }
            mem.end_frame(&g0, f, f * 16, oa);
        }
        for (const auto& x : mem.p) if (x.used && x.synth) { ++streamTile; if (x.tile < 0) ++tileless; }
        CHECK(mem.run.synth > 200);
        CHECK(mem.live_of(P::kSmoke) == P::kCap);                 // the cap was reached: the evictions ran for ~100 frames
        CHECK(streamTile > 100 && tileless == 0);                 // every live synthetic card has a tile (M7: the late ones had none)
        CHECK(lateAtlas > 40 && lateSynth > 40 * 10);             // and they are drawn from the atlas, frame after frame, 140 frames after the real cards are gone
    }
    {   // the emitter stops (shot down / smoke ends) while still inside the window: no synthetic card, the stream ends
        static P::Memory memS; P::Memory& mem = memS;
        for (uint64_t f = 1; f <= 20; ++f) {
            const double w[3] = {200.0 * double(f), 0, 15000.0};
            const float obj[1][3] = {{float(w[0]), 0.0f, 15000.0f}};
            see(mem, g0, kFwdZ, w, 30.0, K, f, f * 16, ob, obj, 1, PSO, HEAP);
            mem.end_frame(&g0, f, f * 16, ob);
        }
        P::Stats last{};
        for (uint64_t f = 21; f <= 60; ++f) last = mem.end_frame(&g0, f, f * 16, ob);
        CHECK(mem.run.synth == 0 && last.streams == 0);
    }
    {   // a one-off spark (fewer than kStreamMinBirths births) is not a plume; a stream also ends kStreamMaxFrames after the last birth
        static P::Memory memS; P::Memory& mem = memS;
        for (uint64_t f = 1; f <= 4; ++f) {
            const double w[3] = {7700.0 + 300.0 * double(f), 0, 15000.0};
            const float obj[1][3] = {{float(w[0]), 0.0f, 15000.0f}};
            see(mem, g0, kFwdZ, w, 30.0, K, f, f * 16, ob, obj, 1, PSO, HEAP);
            mem.end_frame(&g0, f, f * 16, ob);
        }
        for (uint64_t f = 5; f <= 40; ++f) mem.end_frame(&g0, f, f * 16, ob);
        CHECK(mem.run.synth == 0);
        static P::Memory m2S; P::Memory& m2 = m2S;
        for (uint64_t f = 1; f <= 8; ++f) {
            const double w[3] = {7000.0 + 200.0 * double(f), 0, 15000.0};
            const float obj[1][3] = {{float(w[0]), 0.0f, 15000.0f}};
            see(m2, g0, kFwdZ, w, 30.0, K, f, f * 16, ob, obj, 1, PSO, HEAP);
            m2.end_frame(&g0, f, f * 16, ob);
        }
        P::Stats z{};
        for (uint64_t f = 9; f <= 8 + P::kStreamMaxFrames + 5; ++f) z = m2.end_frame(&g0, f, f * 16, ob);
        CHECK(z.streams == 0);                                                   // expired
    }
    {   // fxtrail off (clouds only): no stream, no synthetic card
        static P::Memory memS; P::Memory& mem = memS;
        for (uint64_t f = 1; f <= 30; ++f) {
            const double w[3] = {400.0 * double(f), 0, 15000.0};
            const float obj[1][3] = {{float(w[0]), 0.0f, 15000.0f}};
            if (w[0] <= 8800.0) see(mem, g0, kFwdZ, w, 30.0, K, f, f * 16, oc, obj, 1, PSO, HEAP);
            mem.end_frame(&g0, f, f * 16, oc);
        }
        CHECK(mem.run.synth == 0 && mem.run.streams == 0);
    }
    std::printf("fx_puff_cloud_offline: %u checks passed\n", g_checks);
    return 0;
}
