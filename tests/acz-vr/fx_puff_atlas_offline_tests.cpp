// EFFECTS-OUTSIDE (M6, headset test 7 2026-10-31 "clouds popping in and out", fxatlas:1) offline tests: fx_puff_atlas.h (the tile table, its lifetime
// rules, the uv maths of the capture and of the draw, the host's root-table check, the fx-puff-atlas line) and its use by fx_puff_trail.h (a cached card
// whose key no game draw binds this frame is drawn from its atlas tile under any host of the same game PSO; `unb` drops to 0).
// docs/world-render/effects-outside-window.md section 23.
// "Before" (the M5 key path, fxatlas:0 -- the code path every existing build runs) is checked side by side in the same scenarios: the test-7 frames with
// only another key bound select NOTHING and count every wanted card-frame as unbound; with fxatlas:1 the same frames draw the card and count 0.
#include "fx_puff_trail.h"
#define NOMINMAX
#include <windows.h>
#include "fx_puff_atlas_d3d12.h"
#include "terrain_stage.h"
#include "effects_stage_flags.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace P = acz::fxpuff;
namespace A = acz::fxpuff::atlas;
namespace FS = acz::fxsprite;
namespace M = acz::fxmem;
namespace T = acz::terrain;
namespace s5 = acz::gpu_copy::step5;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// ---- the game / eye maps and cards of fx_puff_cloud_offline_tests (GS window 512 x 448 around 2048)
static void make_a(const double E[3], const double right[3], const double up[3], const double fwd[3], double Am[16]) {
    const double tx = 0.6152, ty = 0.346, b = M::kDepthB, z0 = M::kZ0;
    for (int r = 0; r < 3; ++r) {
        Am[r * 4 + 0] = 2048.0 * fwd[r] + (256.0 / tx) * right[r];
        Am[r * 4 + 1] = 2048.0 * fwd[r] - (224.0 / ty) * up[r];
        Am[r * 4 + 3] = fwd[r];
        Am[r * 4 + 2] = b * Am[r * 4 + 3];
    }
    for (int c : {0, 1, 3}) { double s = 0; for (int r = 0; r < 3; ++r) s += E[r] * Am[r * 4 + c]; Am[12 + c] = -s; }
    Am[14] = z0 + b * Am[15];
}
struct Cam { double E[3], R[3], U[3], F[3]; };
static M::ScreenMap game_map(const Cam& c) { double Am[16]; make_a(c.E, c.R, c.U, c.F, Am); M::ScreenMap m; CHECK(FS::game_map_from_a(Am, m)); return m; }
static M::ScreenMap eye_map(const Cam& c) { double Am[16]; make_a(c.E, c.R, c.U, c.F, Am); M::ScreenMap m; CHECK(FS::eye_map_from_a(Am, m)); return m; }
static const Cam kFwdZ = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};    // looks +z
static const Cam kFwdX = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};   // looks +x (the nose turned 90 degrees right; the head still looks +z)
static void card(const M::ScreenMap& m, const Cam& cam, const double c[3], double h, M::Vtx out[4], float u0 = 0.0f, float u1 = 1.0f) {
    const double sx[4] = {-1, 1, -1, 1}, sy[4] = {1, 1, -1, -1};
    double xs, ys, dc; CHECK(FS::sp_project(m, c, xs, ys, dc));
    for (int k = 0; k < 4; ++k) {
        double w[3]; for (int i = 0; i < 3; ++i) w[i] = c[i] + sx[k] * h * cam.R[i] + sy[k] * h * cam.U[i];
        double x, y, d; CHECK(FS::sp_project(m, w, x, y, d));
        M::Vtx v{}; v.pos[0] = float(x); v.pos[1] = float(y); v.pos[2] = float(m.z0 / dc + m.b); v.pos[3] = 1.0f;
        v.uv[0] = sx[k] > 0 ? u1 : u0; v.uv[1] = sy[k] > 0 ? 0.0f : 1.0f; v.col[0] = v.col[1] = v.col[2] = 0.8f; v.col[3] = 0.5f;
        out[k] = v;
    }
}
static const uint16_t kIdx[4] = {0, 1, 2, 3};
static void see(P::Memory& mem, const M::ScreenMap& g, const Cam& cam, const double c[3], double h, uint64_t key, uint64_t frame, uint64_t now,
                const P::Options& o, uint64_t pso, uint64_t heap, float u0 = 0.0f, float u1 = 1.0f) {
    M::Vtx v[4]; card(g, cam, c, h, v, u0, u1);
    mem.observe(g, v, 4, kIdx, 4, 0, false, key, frame, now, o, nullptr, nullptr, 0, pso, heap);
}

// ---- a CPU texture + bilinear sampler (clamp or wrap), the stand-in for the game's static s0 and for the atlas sampling of the game's PS
struct Tex {
    unsigned w = 0, h = 0; std::vector<float> t;   // one channel is enough for the maths
    float at(int x, int y, bool wrap) const {
        if (wrap) { x = ((x % int(w)) + int(w)) % int(w); y = ((y % int(h)) + int(h)) % int(h); }
        else { x = x < 0 ? 0 : x >= int(w) ? int(w) - 1 : x; y = y < 0 ? 0 : y >= int(h) ? int(h) - 1 : y; }
        return t[size_t(y) * w + size_t(x)];
    }
    float sample(double u, double v, bool wrap) const {
        const double x = u * w - 0.5, y = v * h - 0.5;
        const double fx = std::floor(x), fy = std::floor(y); const double ax = x - fx, ay = y - fy;
        const int ix = int(fx), iy = int(fy);
        return float((1 - ay) * ((1 - ax) * at(ix, iy, wrap) + ax * at(ix + 1, iy, wrap)) + ay * ((1 - ax) * at(ix, iy + 1, wrap) + ax * at(ix + 1, iy + 1, wrap)));
    }
};
// The capture pass on the CPU: rasterise tile i's copy quad at texel centres (the vertex uv interpolated linearly across the pitch square, as the GPU's
// perspective-correct interpolation of a w = 1 quad does) and sample the source there into the atlas texture.
static void capture(Tex& atlas, unsigned i, const A::Rect& r, const Tex& src, bool wrap) {
    A::CopyVtx q[A::kCopyVerts]; A::copy_quad(i, r, q);
    // corners: q[0] = top-left, q[1] = top-right, q[2] = bottom-left, q[5] = bottom-right
    const double px0 = (double(q[0].x) + 1.0) * 0.5 * A::kWidth, px1 = (double(q[1].x) + 1.0) * 0.5 * A::kWidth;
    const double py0 = (1.0 - double(q[0].y)) * 0.5 * A::kHeight, py1 = (1.0 - double(q[2].y)) * 0.5 * A::kHeight;
    for (int y = int(py0); y < int(py1 + 0.5); ++y) for (int x = int(px0); x < int(px1 + 0.5); ++x) {
        const double fx = (x + 0.5 - px0) / (px1 - px0), fy = (y + 0.5 - py0) / (py1 - py0);
        const double u = q[0].u + fx * (double(q[1].u) - q[0].u), v = q[0].v + fy * (double(q[2].v) - q[0].v);
        atlas.t[size_t(y) * atlas.w + size_t(x)] = src.sample(u, v, wrap);
    }
}

int main() {
    // ---------------------------------------------------------------- fx_puff_atlas.h: geometry
    {
        CHECK(A::kPitch == 132 && A::kWidth == 2112 && A::kHeight == 1056 && A::kTiles == 128 && A::kBytes == 2112ull * 1056 * 8 && A::kBytes <= A::kBudget);
        CHECK(A::kCopyStride == 16 && A::kCopyBytes == 96 && A::kCopyBytes * A::kCapPerFrame <= A::kCopyRegion);
        unsigned x = 0, y = 0; A::tile_origin(0, x, y); CHECK(x == 0 && y == 0);
        A::tile_origin(17, x, y); CHECK(x == 132 && y == 132);
        A::tile_origin(127, x, y); CHECK(x == 15 * 132 && y == 7 * 132 && x + A::kPitch == A::kWidth && y + A::kPitch == A::kHeight);
        M::Vtx t[4]{}; const float uv[4][2] = {{0.25f, 0.5f}, {0.75f, 0.5f}, {0.25f, 1.0f}, {0.75f, 1.0f}};
        for (int k = 0; k < 4; ++k) { t[k].uv[0] = uv[k][0]; t[k].uv[1] = uv[k][1]; }
        A::Rect r; CHECK(A::rect_of(t, r) && r.u0 == 0.25f && r.u1 == 0.75f && r.v0 == 0.5f && r.v1 == 1.0f);
        A::Rect rb;
        M::Vtx bad[4]; std::memcpy(bad, t, sizeof bad); bad[2].uv[0] = std::nanf(""); CHECK(!A::rect_of(bad, rb));
        std::memcpy(bad, t, sizeof bad); for (auto& v : bad) v.uv[0] = 0.5f; CHECK(!A::rect_of(bad, rb));   // zero width
        std::memcpy(bad, t, sizeof bad); bad[1].uv[0] = 100.0f; CHECK(!A::rect_of(bad, rb));               // not a texture rectangle
        // map_uv: the rectangle's edges land kGutter texels inside the tile, its far edge kGutter before the next tile
        const A::Rect full{0, 0, 1, 1}; float au = 0, av = 0;
        A::map_uv(17, full, 0.0f, 0.0f, au, av); CHECK(std::fabs(au * A::kWidth - 134.0) < 1e-3 && std::fabs(av * A::kHeight - 134.0) < 1e-3);
        A::map_uv(17, full, 1.0f, 1.0f, au, av); CHECK(std::fabs(au * A::kWidth - 262.0) < 1e-3 && std::fabs(av * A::kHeight - 262.0) < 1e-3);
        // copy_quad covers exactly the pitch square of the tile (clip space over the atlas viewport); its uv extends the rectangle by the gutter
        A::CopyVtx q[A::kCopyVerts]; A::copy_quad(17, r, q);
        CHECK(std::fabs((q[0].x + 1.0) * 0.5 * A::kWidth - 132.0) < 1e-3 && std::fabs((q[1].x + 1.0) * 0.5 * A::kWidth - 264.0) < 1e-3);
        CHECK(std::fabs((1.0 - q[0].y) * 0.5 * A::kHeight - 132.0) < 1e-3 && std::fabs((1.0 - q[2].y) * 0.5 * A::kHeight - 264.0) < 1e-3);
        const double du = 0.5 / 128.0, dv = 0.5 / 128.0;
        CHECK(std::fabs(q[0].u - (0.25 - 2 * du)) < 1e-6 && std::fabs(q[1].u - (0.75 + 2 * du)) < 1e-6 && std::fabs(q[0].v - (0.5 - 2 * dv)) < 1e-6 &&
              std::fabs(q[2].v - (1.0 + 2 * dv)) < 1e-6);
        CHECK(q[3].x == q[2].x && q[3].y == q[2].y && q[4].x == q[1].x && q[5].x == q[1].x && q[5].y == q[2].y);   // (0,1,2),(2,1,3)
        // the inverse: the texel centre j + 0.5 of the tile holds the source uv that map_uv sends to that texel centre
        for (int j = 0; j < int(A::kPitch); ++j) {
            const double u = q[0].u + (j + 0.5) / A::kPitch * (double(q[1].u) - q[0].u);
            float mu = 0, mv = 0; A::map_uv(17, r, float(u), 0.75f, mu, mv);
            CHECK(std::fabs(mu * A::kWidth - (132.0 + j + 0.5)) < 2e-3);
        }
    }
    // ---------------------------------------------------------------- the round trip on the CPU: capture, then the game's bilinear read of the atlas
    // (1:1, a 128-texel rectangle: the same texels; magnified, a 64-texel rectangle of a smooth texture: within 1e-3; the gutter: clamp and wrap edges)
    {
        Tex atlas; atlas.w = A::kWidth; atlas.h = A::kHeight; atlas.t.assign(size_t(atlas.w) * atlas.h, -7.0f);
        unsigned seed = 12345u; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return float((seed >> 8) & 0xffff) / 65535.0f; };
        Tex noise; noise.w = noise.h = 128; noise.t.resize(128 * 128); for (auto& v : noise.t) v = rnd();
        Tex smooth; smooth.w = smooth.h = 64; smooth.t.resize(64 * 64);
        for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) smooth.t[y * 64 + x] = float(0.5 + 0.4 * std::sin(x * 0.11) * std::cos(y * 0.07));
        const A::Rect full{0, 0, 1, 1};
        for (int wrap = 0; wrap < 2; ++wrap) {
            capture(atlas, 5, full, noise, wrap != 0);
            capture(atlas, 6, full, smooth, wrap != 0);
            double e1 = 0, e2 = 0, e2in = 0;
            for (int s = 0; s < 4000; ++s) {
                const double u = rnd(), v = rnd();   // anywhere on the card, edges included
                float au, av;
                A::map_uv(5, full, float(u), float(v), au, av);
                e1 = std::fmax(e1, std::fabs(double(atlas.sample(au, av, false)) - noise.sample(u, v, wrap != 0)));
                A::map_uv(6, full, float(u), float(v), au, av);
                const double d2 = std::fabs(double(atlas.sample(au, av, false)) - smooth.sample(u, v, wrap != 0));
                e2 = std::fmax(e2, d2);
                if (u > 0.02 && u < 0.98 && v > 0.02 && v < 0.98) e2in = std::fmax(e2in, d2);
            }
            std::printf("  round trip (%s): 1:1 noise max |d| = %.2e, 2x smooth max |d| = %.2e (inside %.2e)\n", wrap ? "wrap" : "clamp", e1, e2, e2in);
            // 1:1: the same texels. Magnified 2x: bilinear of the bilinear upsample differs by (slope change at a source texel centre) / 8, largest at an
            // edge where the sampler's addressing turns the slope (clamp: 0.044 / texel -> 0.0055) or jumps (wrap across a non-periodic texture: the
            // seam value step / 8 -> 0.054); inside a smooth texture well below 1/255. (Cloud / smoke cards fade to alpha 0 at their edges.)
            CHECK(e1 < 1e-4 && e2in < 1e-3 && e2 < (wrap ? 0.1 : 1e-2));
            // texel centres of the 1:1 tile are the source texels exactly
            unsigned x0, y0; A::tile_origin(5, x0, y0);
            for (unsigned j = 0; j < 128; j += 13) CHECK(atlas.t[size_t(y0 + A::kGutter + j) * atlas.w + x0 + A::kGutter + j] == noise.t[size_t(j) * 128 + j]);
        }
        // a sub-rectangle (one frame of a sheet): only that rectangle is the card's image
        const A::Rect sub{0.5f, 0.25f, 0.75f, 0.5f};
        capture(atlas, 9, sub, noise, false);
        double e = 0;
        for (int s = 0; s < 2000; ++s) {
            const double u = 0.5 + 0.25 * rnd(), v = 0.25 + 0.25 * rnd(); float au, av; A::map_uv(9, sub, float(u), float(v), au, av);
            e = std::fmax(e, std::fabs(double(atlas.sample(au, av, false)) - noise.sample(u, v, false)));
        }
        std::printf("  sub-rectangle 32x32 texels -> 128: max |d| = %.3f (noise; magnified 4x)\n", e);
        CHECK(e < 0.25);   // bilinear of a 4x bilinear upsample of white noise: bounded, not exact (docs 23.6 limits)
        // neighbouring tiles never bleed: tile 5's last content texel and tile 6's first gutter texel are independent writes
        CHECK(atlas.t[size_t(A::kGutter) * atlas.w + 6 * A::kPitch] != -7.0f);
    }
    // ---------------------------------------------------------------- the tile table: lifetime rules
    {
        auto at = std::make_unique<A::Atlas>(); A::Atlas& a = *at;
        const uint64_t K = 0x15678a0012a280ull, K2 = 0x15678a0012a2a0ull; const A::Rect r{0, 0, 1, 1}, r2{0, 0, 0.5f, 1};
        a.slot_started(0);                                   // recording 1 in slot 0
        bool cap = false;
        int t = a.request(-1, K, r, 0, 10, true, cap);
        CHECK(t == 0 && cap && a.t[0].pending && !a.t[0].ready && !a.drawable(t, K, r) && a.t[0].refs == 1);
        int t2 = a.request(-1, K, r, 0, 10, true, cap);      // a second card of the same key / rectangle in the same recording: shares, no capture
        CHECK(t2 == 0 && !cap && a.t[0].refs == 2 && a.c.shared == 1 && a.c.captures == 1);
        CHECK(a.request(t, K, r, 0, 11, true, cap) == 0 && !cap);   // captured in this recording already
        a.slot_submitted(1);                                 // another slot's submission: nothing
        CHECK(!a.drawable(t, K, r));
        a.slot_submitted(0);
        CHECK(a.drawable(t, K, r) && !a.drawable(t, K2, r) && !a.drawable(t, K, r2) && a.c.readied == 1);
        // ready and fresh: no capture; stale (kRefreshFrames later, recording 2): an in-place refresh that keeps it drawable
        a.slot_started(0);
        CHECK(a.request(t, K, r, 0, 10 + A::kRefreshFrames - 1, true, cap) == 0 && !cap);
        CHECK(a.request(t, K, r, 0, 10 + A::kRefreshFrames, true, cap) == 0 && cap && a.c.refreshes == 1 && a.drawable(t, K, r));
        // a rectangle change (the card's template moved to another frame of the sheet): the old reference goes, a new tile is captured
        int t3 = a.request(t2, K, r2, 0, 200, true, cap);
        CHECK(t3 == 1 && cap && a.t[0].refs == 1 && a.t[1].refs == 1);
        // the recording is DISCARDED (Reset unsubmitted): slot 0 picked again; tile 1 never became ready and is recaptured (dead)
        a.slot_started(0);
        CHECK(a.dead_capture(a.t[1]) && !a.drawable(t3, K, r2));
        CHECK(a.request(t3, K, r2, 0, 201, true, cap) == 1 && cap && a.c.dead == 1);
        a.slot_submitted(0); CHECK(a.drawable(t3, K, r2));
        // the capture cap: no new tile, an existing match kept
        CHECK(a.request(-1, K2, r, 0, 202, false, cap) == -1 && !cap && a.c.capcap == 1 && a.live() == 2);
        // reuse only after retirement: tile 1 (captured in recording 3 of slot 0) read by a recording of slot 1
        a.slot_started(1); a.use(1, 1);
        a.release(t3); CHECK(t3 == -1 && a.t[1].refs == 0 && !a.retired(a.t[1]) && a.stuck() == 1);
        a.slot_started(0); CHECK(!a.retired(a.t[1]));       // slot 0 retired, slot 1 (the read) not yet
        a.slot_started(1); CHECK(a.retired(a.t[1]) && a.stuck() == 0);
        // fill the atlas: every tile referenced -> full; an unreferenced but unretired tile is never taken
        std::vector<int> refs;
        for (unsigned i = 0; i < A::kTiles + 4; ++i) {
            A::Rect ri{0, 0, 1.0f / float(i + 2), 1};
            const int ti = a.request(-1, K + 0x20 * (i + 1), ri, 1, 300, true, cap);
            if (ti >= 0) refs.push_back(ti);
        }
        // 132 requests: tiles 2..127 free (126), tile 1 (retired, unreferenced) recycled, tile 0 still held by a card: the last 5 refused
        CHECK(a.live() == A::kTiles && refs.size() == 127 && a.c.full == 5 && a.c.recycled == 1);
        for (int& ti : refs) a.release(ti);
        // all unreferenced now, but captured by the still-open recording of slot 1: none reusable yet
        CHECK(a.request(-1, 0x999, r, 1, 301, true, cap) == -1 && a.c.full == 6 && a.stuck() == 127);
        a.slot_started(1);
        const int tr = a.request(-1, 0x999, r, 1, 302, true, cap);
        CHECK(tr >= 0 && cap && a.c.recycled == 2 && a.t[tr].key == 0x999 && a.t[tr].pending && a.stuck() == 0);
        // a slot that is never picked again (quarantined) keeps its tiles unreusable (stuck), fail closed
        a.slot_started(2); int tq = a.request(-1, 0x777, r, 2, 400, true, cap); CHECK(tq >= 0); a.release(tq);
        for (int k = 0; k < 5; ++k) { a.slot_started(0); a.slot_started(1); }
        CHECK(a.stuck() == 1);
        bool found = false; for (const auto& x : a.t) if (x.used && x.key == 0x777) found = !a.retired(x) && x.refs == 0; CHECK(found);
        const A::Counters c = a.take(); CHECK(c.captures > 100 && a.c.captures == 0);
        // an out-of-range slot never gets a tile
        CHECK(a.request(-1, K, r, A::kSlotCap, 500, true, cap) == -1 && !cap);
    }
    // ---------------------------------------------------------------- review 2026-11-01 F1: no card joins a stale tile without a capture
    {
        auto at = std::make_unique<A::Atlas>(); A::Atlas& a = *at;
        const uint64_t K = 0x15678a0012a280ull; const A::Rect r{0, 0, 0.5f, 0.5f};
        bool cap = false;
        a.slot_started(0);
        int t = a.request(-1, K, r, 0, 10, true, cap); CHECK(t == 0 && cap);
        a.slot_submitted(0); CHECK(a.drawable(t, K, r));
        a.release(t);                                        // its card expires: the tile is used, unreferenced
        CHECK(!a.t[0].ready && a.t[0].used && a.ready_n() == 0);   // failing before: still ready (joinable as is)
        for (int k = 0; k < 3; ++k) { a.slot_started(0); a.slot_submitted(0); }
        // 200 frames later (a mission restart: K now names another texture) a new card of (K, r) is sighted: it must capture
        int n = a.request(-1, K, r, 0, 210, true, cap);
        CHECK(n >= 0 && cap && a.t[n].refs == 1);            // failing before: n == 0, cap == false (old pixels for the card's whole life)
        a.slot_submitted(0); CHECK(a.drawable(n, K, r));
        // a REFERENCED ready tile whose cards are not sighted (no own refresh) gets old: a new card refreshes it in place in its draw
        a.slot_started(0);
        int m = a.request(-1, K, r, 0, 210 + A::kRefreshFrames + 5, true, cap);
        CHECK(m == n && cap && a.c.stale == 1 && a.t[n].refs == 2 && a.drawable(m, K, r) && a.t[n].cap_frame == 210 + A::kRefreshFrames + 5);
        // ... and a FRESH ready tile is still joined without a capture (the sharing that makes the atlas fit)
        a.slot_submitted(0); a.slot_started(0);
        int f = a.request(-1, K, r, 0, 210 + A::kRefreshFrames + 50, true, cap);
        CHECK(f == n && !cap && a.t[n].refs == 3);
        // stale and the capture cap spent: no tile (the card keeps the key path), never the old content
        a.slot_started(0);
        const uint32_t cc = a.c.capcap;
        CHECK(a.request(-1, K, r, 0, 210 + 3 * A::kRefreshFrames, false, cap) == -1 && !cap && a.c.capcap == cc + 1 && a.t[n].refs == 3);
        // a capture of THIS recording whose card left before the submission: never made ready unreferenced; a sibling card of the same recording that
        // joins it gets it ready at the submission
        a.slot_started(1);
        const A::Rect r3{0.5f, 0.5f, 1, 1};
        int p = a.request(-1, K, r3, 1, 900, true, cap); CHECK(p >= 0 && cap);
        int pc = p; a.release(pc); CHECK(a.t[p].refs == 0 && a.t[p].pending);
        a.slot_submitted(1); CHECK(!a.t[p].ready && !a.t[p].pending);
        a.slot_started(1);
        int q = a.request(-1, K, r3, 1, 901, true, cap); CHECK(q >= 0 && cap);   // recaptured (no unreferenced ready tile to join)
        int q2 = a.request(-1, K, r3, 1, 901, true, cap); CHECK(q2 == q && !cap);   // same recording: shared
        { int qq = q; a.release(qq); } CHECK(a.t[q].refs == 1);
        a.slot_submitted(1); CHECK(a.drawable(q2, K, r3));
    }
    // ---------------------------------------------------------------- review 2026-11-01 F3: a quarantined slot's pending captures release their cards
    {
        const uint64_t K = 0x2222ull; const A::Rect r{0, 0, 1, 1};
        bool cap = false;
        // BEFORE (no slot_quarantined call, as every build so far): the card waits on the pending tile of a slot that is never picked again, forever
        {
            auto bt = std::make_unique<A::Atlas>(); A::Atlas& b = *bt;
            b.slot_started(2); int t = b.request(-1, K, r, 2, 10, true, cap); CHECK(t == 0 && cap);
            for (int k = 0; k < 5; ++k) { b.slot_started(0); b.slot_submitted(0); }
            CHECK(b.request(t, K, r, 0, 500, true, cap) == t && !cap && !b.drawable(t, K, r));   // stuck: never drawable, never recaptured
        }
        auto at = std::make_unique<A::Atlas>(); A::Atlas& a = *at;
        a.slot_started(2);
        int t = a.request(-1, K, r, 2, 10, true, cap); CHECK(t == 0 && cap);
        int t2 = a.request(-1, K, r, 2, 10, true, cap); CHECK(t2 == 0 && !cap && a.t[0].refs == 2);
        a.slot_started(1); a.use(0, 1);                       // another recording read nothing ready; just a second pin source
        CHECK(a.qpinned() == 0);
        a.slot_quarantined(2);                                 // step5SetSlot(quarantined): a fault after the captures were recorded
        CHECK(a.t[0].quarantined && a.t[0].refs == 0 && !a.t[0].ready && a.c.qrel == 1 && a.qpinned() == 1);
        a.slot_submitted(2); CHECK(!a.t[0].ready);            // never readied, even if the slot's submission is reported later
        // the cards take a fresh tile in the next recording (of another slot)
        a.slot_started(0);
        int n = a.request(t, K, r, 0, 20, true, cap);
        CHECK(n == 1 && cap && t == 0 && a.t[1].refs == 1);
        int n2 = a.request(t2, K, r, 0, 20, true, cap);
        CHECK(n2 == 1 && !cap && a.t[1].refs == 2);           // shared with the fresh capture, never the quarantined tile
        a.slot_submitted(0); CHECK(a.drawable(n, K, r));
        // the quarantined tile stays pinned: never recycled, never evicted, never joined
        for (int k = 0; k < 5; ++k) { a.slot_started(0); a.slot_started(1); }
        CHECK(!a.retired(a.t[0]) && a.evict_candidate(100000) != 0 && a.qpinned() == 1);
        // a ready tile READ by the quarantined recording is pinned too (qpin), its cards keep drawing it
        a.slot_started(3); a.use(1, 3); a.slot_quarantined(3);
        CHECK(a.qpinned() == 2 && a.drawable(1, K, r) && a.t[1].refs == 2 && !a.t[1].quarantined);
        a.slot_quarantined(A::kSlotCap);                      // out of range: nothing
    }
    // ---------------------------------------------------------------- the host's root-table check
    {
        unsigned kc = 9;
        // TableDesc{param, srv_only, unbounded, span, value_known, sampler_only}
        // (M6b) THE LIVE LAYOUT, root signature 310 (docs/interception/stored-draw-manifest-1.md 3.2; step164 null run: hwhy=atlas-other-table on every host):
        // param0 VS b0 x4, param1 PS b1 x4, param2 PS SRV table (t0 = the key), param3 PS SAMPLER table (s0), no static samplers.
        // FAILING BEFORE: M6 (98ddef4d) refused it as "atlas-other-table" (it allowed one table only), so the atlas never drew.
        A::TableDesc rs310[2] = {{2, true, false, 1, true, false}, {3, false, false, 1, true, true}};
        CHECK(A::host_why(rs310, 2, 2, 2, kc) == nullptr && kc == 1);
        CHECK(std::string(A::host_why(rs310, 2, 2, 1, kc)) == "atlas-sampler-heap");    // the sampler table needs the game's sampler heap bound
        A::TableDesc rs310u[2] = {{2, true, false, 1, true, false}, {3, false, false, 1, false, true}};
        CHECK(std::string(A::host_why(rs310u, 2, 2, 2, kc)) == "atlas-table-unknown");   // the sampler table's value must be known to set it back
        A::TableDesc live[1] = {{2, true, false, 1, true, false}};   // a static-s0 variant: one table
        CHECK(A::host_why(live, 1, 2, 1, kc) == nullptr && kc == 1);
        CHECK(std::string(A::host_why(live, 1, 3, 1, kc)) == "atlas-key-param");
        A::TableDesc two[2] = {{2, true, false, 1, true, false}, {3, false, false, 1, true, false}};   // another CBV / SRV table: not supported, named
        CHECK(std::string(A::host_why(two, 2, 2, 2, kc)) == "atlas-other-view-table");
        A::TableDesc unk[1] = {{2, true, false, 1, false}}; CHECK(std::string(A::host_why(unk, 1, 2, 1, kc)) == "atlas-table-unknown");
        A::TableDesc cbv[1] = {{2, false, false, 1, true}}; CHECK(std::string(A::host_why(cbv, 1, 2, 1, kc)) == "atlas-key-table-not-srv");
        A::TableDesc big[1] = {{2, true, false, A::kTableMax + 1, true}}; CHECK(std::string(A::host_why(big, 1, 2, 1, kc)) == "atlas-key-table-size");
        A::TableDesc unb[1] = {{2, true, true, 1, true}}; CHECK(std::string(A::host_why(unb, 1, 2, 1, kc)) == "atlas-key-table-size");
        CHECK(std::string(A::host_why(nullptr, 0, 2, 1, kc)) == "atlas-no-table");
        // the layout text logged with a refusal
        {
            A::ParamDesc pd[4] = {{'C', 0, 4, {}, 0, 'v'}, {'C', 1, 4, {}, 0, 'p'}, {'T', 0, 0, {'s'}, 1, 'p'}, {'T', 0, 0, {'z'}, 1, 'p'}};
            char lay[72]; A::layout_text(pd, 4, lay, sizeof lay);
            CHECK(std::string(lay) == "0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p");
            char tiny[12]; A::layout_text(pd, 4, tiny, sizeof tiny); CHECK(std::strlen(tiny) < sizeof tiny);
        }
        // the recorded atlas group for root signature 310 (fx_puff_atlas_d3d12.h record_group, the sequence the proxy and the WARP harness record)
        {
            auto* ours = reinterpret_cast<ID3D12DescriptorHeap*>(0x100); auto* gv = reinterpret_cast<ID3D12DescriptorHeap*>(0x200);
            auto* gs = reinterpret_cast<ID3D12DescriptorHeap*>(0x300);
            A::GroupBind gb; gb.ours = ours; gb.our_srv.ptr = 0xAA00; gb.game[0] = gv; gb.game[1] = gs; gb.ngame = 2; gb.game_sampler = gs;
            gb.key_param = 2; gb.host_key = 0x15678a0012a280ull; gb.ntables = 2; gb.tparam[0] = 2; gb.tvalue[0] = gb.host_key; gb.tparam[1] = 3; gb.tvalue[1] = 0x5A00;
            CHECK(A::group_why(gb) == nullptr);
            std::string seq;
            const unsigned n = A::record_group(gb,
                [&](unsigned c, ID3D12DescriptorHeap* const* h) { char t[64]; std::snprintf(t, sizeof t, "H%u:%llx,%llx;", c, (unsigned long long)(uintptr_t)h[0], c > 1 ? (unsigned long long)(uintptr_t)h[1] : 0ull); seq += t; },
                [&](unsigned p, D3D12_GPU_DESCRIPTOR_HANDLE h) { char t[48]; std::snprintf(t, sizeof t, "T%u=%llx;", p, (unsigned long long)h.ptr); seq += t; },
                [&] { seq += "D;"; });
            CHECK(seq == "H2:100,300;T3=5a00;T2=aa00;D;H2:200,300;T2=15678a0012a280;T3=5a00;");
            CHECK(n == A::group_commands(2) && n == 7);
            A::GroupBind nos = gb; nos.game_sampler = nullptr; CHECK(std::string(A::group_why(nos)) == "atlas-sampler-heap");
            A::GroupBind one = gb; one.ntables = 1; one.game_sampler = nullptr; one.ngame = 1;   // a static-s0 signature: our heap alone
            seq.clear(); CHECK(A::group_why(one) == nullptr);
            CHECK(A::record_group(one, [&](unsigned c, ID3D12DescriptorHeap* const*) { seq += "H" + std::to_string(c) + ";"; },
                                  [&](unsigned p, D3D12_GPU_DESCRIPTOR_HANDLE) { seq += "T" + std::to_string(p) + ";"; }, [&] { seq += "D;"; }) == 5);
            CHECK(seq == "H1;T2;D;H1;T2;");
            A::GroupBind nokey = gb; nokey.key_param = 7; CHECK(std::string(A::group_why(nokey)) == "atlas-key-param");
            A::GroupBind noheap = gb; noheap.ours = nullptr; CHECK(std::string(A::group_why(noheap)) == "atlas-not-ready");
        }
        uint32_t span = 0;
        { const uint32_t cnt[1] = {1}, off[1] = {A::kAppend}; CHECK(A::table_span(cnt, off, 1, span) && span == 1); }
        { const uint32_t cnt[2] = {2, 3}, off[2] = {A::kAppend, A::kAppend}; CHECK(A::table_span(cnt, off, 2, span) && span == 5); }
        { const uint32_t cnt[2] = {1, 1}, off[2] = {0, 6}; CHECK(A::table_span(cnt, off, 2, span) && span == 7); }
        { const uint32_t cnt[1] = {0xffffffffu}, off[1] = {A::kAppend}; CHECK(!A::table_span(cnt, off, 1, span)); }
        CHECK(A::group_commands(1) == 5 && A::kCaptureCommands == 12);
    }
    // ---------------------------------------------------------------- the memory: test 7's popping and its fix
    const uint64_t K = 0x15678a0012a280ull, K2 = 0x15678a0012a2a0ull, PSO = 0x1b6d8fd2740ull, HEAP = 0x2a00ull;
    const M::ScreenMap g0 = game_map(kFwdZ), g1 = game_map(kFwdX), e0 = eye_map(kFwdZ);
    uint16_t sel[P::kMaxEmit];
    {
        CHECK(!P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0).atlas);
        CHECK(P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, P::kClipFade, true).atlas);
        CHECK(!P::options(0.0, 1500.0, P::kClsNear, 2500.0, 0.0, P::kDefCloudFar, true, P::kClipFade, true).atlas);   // memory off: no atlas
    }
    // The scenario of headset test 7 (22.1 item 1; prev ms 231208296: live c15, one host draw per frame, sel 0, nomatch 14 / frame): a cloud of key K
    // seen whole twice; then the nose turns away (g1) and the game binds ONLY key K2 (one host of the same PSO); the head still looks at the cloud (e0).
    for (int withAtlas = 0; withAtlas < 2; ++withAtlas) {
        auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;
        const P::Options o = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, P::kClipFade, withAtlas != 0);
        const double C[3] = {0, 0, 20000};
        unsigned slot = 0, caps = 0;
        auto start = [&](unsigned s) { mem.atlas.slot_started(s); mem.set_slot(s); };
        uint16_t tiles[A::kCapPerDraw]; A::Rect rects[A::kCapPerDraw];
        for (uint64_t f = 2; f < 4; ++f) {
            start(slot);
            see(mem, g0, kFwdZ, C, 3000, K, f, f * 16, o, PSO, HEAP);
            caps += mem.plan_captures(f, o, tiles, rects, A::kCapPerDraw);   // the GPU plan of that draw took it (pRecord)
            mem.end_frame(&g0, f, f * 16, o);
            mem.atlas.slot_submitted(slot); slot = (slot + 1) % 3;           // Execute returned
        }
        CHECK(caps == (withAtlas ? 1u : 0u));
        if (withAtlas) { CHECK(tiles[0] == 0 && rects[0] == (A::Rect{0, 0, 1, 1})); }
        unsigned drawn = 0, unb = 0, want = 0, nomatch = 0, groupOk = 0;
        for (uint64_t f = 4; f < 104; ++f) {
            start(slot);
            const unsigned n = mem.select(P::Host{K2, PSO, HEAP, true}, f, g1, &e0, sel, P::kMaxEmit, o);
            M::Vtx out[P::kMaxEmitVerts]; P::Group gr[P::kMaxKeys]; unsigned ng = 0;
            const unsigned v = mem.build_eye(e0, 1.0, 1.0, sel, n, f * 16, o, out, P::kMaxEmitVerts, gr, &ng);
            if (n == 1 && v == 6 && ng == 1 && gr[0].key == A::kGroupKey && gr[0].first == 0 && gr[0].count == 6) {
                // the eye copy samples the tile: uv 0 -> 2 texels in, uv 1 -> 130 texels in (tile 0)
                bool uvOk = true;
                for (unsigned k = 0; k < 6; ++k) {
                    const double px = out[k].uv[0] * A::kWidth, py = out[k].uv[1] * A::kHeight;
                    uvOk = uvOk && (std::fabs(px - 2.0) < 1e-3 || std::fabs(px - 130.0) < 1e-3) && (std::fabs(py - 2.0) < 1e-3 || std::fabs(py - 130.0) < 1e-3);
                }
                groupOk += uvOk;
            }
            const P::Stats s = mem.end_frame(&g1, f, f * 16, o);
            drawn += s.adrawn; unb += s.unbound; want += s.want; nomatch += s.nomatch;
            mem.atlas.slot_submitted(slot); slot = (slot + 1) % 3;
        }
        std::printf("  test-7 scenario fxatlas:%d: want=%u unb=%u drawn-from-atlas=%u nomatch=%u\n", withAtlas, want, unb, drawn, nomatch);
        CHECK(want == 100);
        if (!withAtlas) CHECK(drawn == 0 && unb == 100 && nomatch == 100 && groupOk == 0);   // BEFORE (M5): 100 frames without the cloud (the popping)
        else CHECK(drawn == 100 && unb == 0 && nomatch == 0 && groupOk == 100);           // AFTER: drawn every frame, unb 0
        if (withAtlas) {
            // the tile is referenced by the card and read by recent recordings: never reused while either holds
            int ti = -1; for (const auto& x : mem.p) if (x.used) ti = x.tile;
            CHECK(ti == 0 && mem.atlas.t[0].refs == 1 && mem.atlas.t[0].ready);
            // a host of ANOTHER game PSO cannot draw it (other blend / shaders): nomatch, and amiss (a ready tile no atlas host took)
            start(slot);
            CHECK(mem.select(P::Host{K2, PSO + 8, HEAP, true}, 104, g1, &e0, sel, P::kMaxEmit, o) == 0);
            P::Stats s = mem.end_frame(&g1, 104, 104 * 16, o);
            CHECK(s.nomatch == 1 && s.amiss == 1 && s.unbound == 1);   // (M6b) a miss is unb whatever its tile (M6 counted 0: step164 unb=0, drawn=0)
            // a host that cannot draw the atlas (host_why refused, the atlas not ready): the same
            CHECK(mem.select(P::Host{K2, PSO, HEAP, false}, 105, g1, &e0, sel, P::kMaxEmit, o) == 0);
            s = mem.end_frame(&g1, 105, 105 * 16, o); CHECK(s.amiss == 1 && s.unbound == 1);
            // the card's own key bound again: the own-key path (no atlas, no heap switch) is preferred
            CHECK(mem.select(P::Host{K, PSO, HEAP, true}, 106, g1, &e0, sel, P::kMaxEmit, o) == 1 && !mem.p[sel[0]].emit_atlas);
            s = mem.end_frame(&g1, 106, 106 * 16, o); CHECK(s.adrawn == 0);
            // unselect (window fallback) hands the atlas selection back
            CHECK(mem.select(P::Host{K2, PSO, HEAP, true}, 107, g1, &e0, sel, P::kMaxEmit, o) == 1 && mem.p[sel[0]].emit_atlas);
            mem.unselect(107, sel, 1, "trail-window-fallback");
            s = mem.end_frame(&g1, 107, 107 * 16, o); CHECK(s.adrawn == 0 && !mem.p[sel[0]].emit_atlas && s.amiss == 1 && s.unbound == 1);
            // the card expires: its tile reference goes with it (the tile is reusable once its recordings retired)
            mem.end_frame(&g1, 108, 108 * 16 + 130000, o);
            CHECK(mem.live() == 0 && mem.atlas.t[0].refs == 0);
        }
    }
    // A mixed frame: the host's own key first, another key bound in the frame (xkey) next, the atlas group last; capture cap per draw.
    {
        auto memp = std::make_unique<P::Memory>(); P::Memory& mem = *memp;
        const P::Options o = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0, P::kDefCloudFar, true, P::kClipFade, true);
        const uint64_t K3 = 0x15678a0012a2c0ull;
        const double A1[3] = {0, 0, 20000}, B1[3] = {-6000, 2000, 30000}, C1[3] = {6000, -1500, 25000};
        mem.atlas.slot_started(0); mem.set_slot(0);
        uint16_t tiles[A::kCapPerDraw]; A::Rect rects[A::kCapPerDraw];
        for (uint64_t f = 2; f < 4; ++f) {
            see(mem, g0, kFwdZ, A1, 2000, K, f, f * 16, o, PSO, HEAP); mem.plan_captures(f, o, tiles, rects, A::kCapPerDraw);
            see(mem, g0, kFwdZ, B1, 2000, K2, f, f * 16, o, PSO, HEAP); mem.plan_captures(f, o, tiles, rects, A::kCapPerDraw);
            see(mem, g0, kFwdZ, C1, 2000, K3, f, f * 16, o, PSO, HEAP); mem.plan_captures(f, o, tiles, rects, A::kCapPerDraw);
            mem.end_frame(&g0, f, f * 16, o);
        }
        mem.atlas.slot_submitted(0);
        CHECK(mem.atlas.ready_n() == 3);
        mem.atlas.slot_started(1); mem.set_slot(1);
        // frame 4: K3 is bound by an earlier game draw of the frame (xkey), K is the host, K2 is bound by nothing (atlas)
        { const double W[3] = {20000, 0, 0}; M::Vtx v[4]; card(g1, kFwdX, W, 10, v); mem.observe(g1, v, 4, kIdx, 4, 0, true, K3, 4, 64, o, nullptr, nullptr, 0, PSO, HEAP); }
        const unsigned n = mem.select(P::Host{K, PSO, HEAP, true}, 4, g1, &e0, sel, P::kMaxEmit, o);
        M::Vtx out[P::kMaxEmitVerts]; P::Group gr[P::kMaxKeys]; unsigned ng = 0;
        CHECK(n == 3 && mem.build_eye(e0, 1.0, 1.0, sel, n, 64, o, out, P::kMaxEmitVerts, gr, &ng) == 18);
        CHECK(ng == 3 && gr[0].key == K && gr[1].key == K3 && gr[2].key == A::kGroupKey && gr[2].first == 12 && gr[2].count == 6);
        const P::Stats s = mem.end_frame(&g1, 4, 64, o);
        CHECK(s.xkey == 1 && s.adrawn == 1 && s.unbound == 0 && s.want == 3);
        // capture cap: 10 cards sighted in one draw, room for 8: 8 captures, 2 postponed (capcap) and captured by the next sighting
        auto m2p = std::make_unique<P::Memory>(); P::Memory& m2 = *m2p;
        m2.atlas.slot_started(0); m2.set_slot(0);
        M::Vtx v[40]; uint16_t idx[49]; size_t ni = 0;
        for (int i = 0; i < 10; ++i) {
            const double Ci[3] = {-9000.0 + 2000.0 * i, 0, 30000};
            card(g0, kFwdZ, Ci, 600, v + 4 * i, float(i) * 0.05f, float(i) * 0.05f + 0.05f);   // ten frames of one sheet: ten rectangles
            if (i) idx[ni++] = 0xFFFF;
            for (int k = 0; k < 4; ++k) idx[ni++] = uint16_t(4 * i + k);
        }
        m2.observe(g0, v, 40, idx, ni, 0, false, K, 2, 32, o, nullptr, nullptr, 0, PSO, HEAP);
        CHECK(m2.seen_n_ == 10);
        // (M6b) a first sighting takes no tile (a card is re-emitted only after kMinSeen sightings: sparks never cost a tile)
        CHECK(m2.plan_captures(2, o, tiles, rects, A::kCapPerDraw) == 0 && m2.atlas.live() == 0 && m2.seen_n_ == 0);
        m2.observe(g0, v, 40, idx, ni, 0, false, K, 3, 48, o, nullptr, nullptr, 0, PSO, HEAP);
        CHECK(m2.plan_captures(3, o, tiles, rects, A::kCapPerDraw) == 8 && m2.atlas.c.capcap == 2 && m2.seen_n_ == 0);
        m2.observe(g0, v, 40, idx, ni, 0, false, K, 4, 64, o, nullptr, nullptr, 0, PSO, HEAP);
        CHECK(m2.plan_captures(4, o, tiles, rects, A::kCapPerDraw) == 2 && m2.atlas.live() == 10);
        // fxatlas off: plan_captures never requests anything
        const P::Options off = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 120.0);
        m2.observe(g0, v, 40, idx, ni, 0, false, K, 5, 80, off, nullptr, nullptr, 0, PSO, HEAP);
        CHECK(m2.plan_captures(5, off, tiles, rects, A::kCapPerDraw) == 0);
    }
    // (M6b) Tile churn, step164 AGC1 (tiles=128/128 the whole flight, run cap 1573, drawn 0): more distinct (key, rectangle) cards than tiles. FAILING
    // BEFORE: M6 never took a tile from a card, so once 128 cards held tiles every later card was refused for the rest of the flight. Now a full atlas evicts
    // the tile least recently sighted / drawn (>= kEvictAge frames ago, its recordings retired) and the new card gets it.
    {
        auto mp = std::make_unique<P::Memory>(); P::Memory& m = *mp;
        const P::Options o = P::options(0.0, 1500.0, P::kClsNear, 2500.0, 600.0, P::kDefCloudFar, true, P::kClipFade, true);
        uint16_t tiles[A::kCapPerDraw]; A::Rect rects[A::kCapPerDraw];
        unsigned slot = 0; uint64_t frame = 10;
        // `count` new cloud cards (each its own sheet cell) seen whole on two consecutive frames, eight per draw (the per-draw cap); each frame is one
        // recording of the next slot. The frame's end looks away (g1): stored cards are not "shown by the game", so none is culled while it waits.
        auto sightTwice = [&](int first, int count) {
            for (int pass = 0; pass < 2; ++pass) {
                m.atlas.slot_started(slot); m.set_slot(slot);
                for (int i = first; i < first + count; i += 8) {
                    for (int j = i; j < i + 8 && j < first + count; ++j) {
                        const double Cj[3] = {-12000.0 + 1500.0 * (j % 16), -5000.0 + 1500.0 * ((j / 16) % 8), 40000.0 + 3000.0 * (j / 128)};
                        const float u0 = float(j % 64) / 64.0f, u1 = u0 + 1.0f / 64.0f;
                        see(m, g0, kFwdZ, Cj, 600, 0x15678a0012a280ull + 0x20ull * uint64_t(j / 64), frame, frame * 16, o, PSO, HEAP, u0, u1);
                        m.plan_captures(frame, o, tiles, rects, A::kCapPerDraw);   // the GPU plan of that draw (candidates are per draw)
                    }
                }
                m.end_frame(&g1, frame, frame * 16, o);
                m.atlas.slot_submitted(slot); slot = (slot + 1) % 3; ++frame;
            }
        };
        (void)tiles; (void)rects;
        sightTwice(0, 128);
        std::printf("  churn: 128 cards: live cards %u, tiles %u, ready %u, captures %u, full %u, capcap %u\n", m.live(), m.atlas.live(), m.atlas.ready_n(),
                    m.atlas.c.captures, m.atlas.c.full, m.atlas.c.capcap);
        CHECK(m.atlas.live() == 128 && m.atlas.ready_n() == 128);
        frame += A::kEvictAge + 5;                          // the first 128 are not sighted / drawn for a while ...
        for (int k = 0; k < 3; ++k) { m.atlas.slot_started(slot); m.atlas.slot_submitted(slot); slot = (slot + 1) % 3; }   // ... and every slot recorded again
        const A::Counters before = m.atlas.c;
        sightTwice(128, 16);                                // 16 new cards
        unsigned newTiles = 0; for (unsigned i = 0; i < P::kSlots; ++i) if (m.p[i].used && m.p[i].tile >= 0 && m.p[i].c[2] > 42000.0) ++newTiles;
        std::printf("  churn: 16 new cards on a full atlas: %u got tiles, evicted %u, full %u\n", newTiles, m.atlas.c.evicted - before.evicted, m.atlas.c.full - before.full);
        CHECK(newTiles == 16 && m.atlas.c.evicted - before.evicted == 16 && m.atlas.c.full == before.full && m.atlas.live() == 128);
        // an evicted card has no tile any more (it keeps the key path) and nothing points at a reassigned tile
        unsigned detached = 0, mismatched = 0;
        for (unsigned i = 0; i < P::kSlots; ++i) {
            const P::Puff& x = m.p[i]; if (!x.used) continue;
            if (x.tile < 0) ++detached; else if (m.atlas.t[x.tile].key != x.key) ++mismatched;
        }
        CHECK(detached == 16 && mismatched == 0);
        // a recently drawn tile is not evicted: all 128 used this frame -> a new card is refused (full), nobody loses its tile
        for (auto& t : m.atlas.t) t.last_frame = frame;
        const uint32_t full0 = m.atlas.c.full, ev0 = m.atlas.c.evicted;
        sightTwice(144, 1);
        CHECK(m.atlas.c.full > full0 && m.atlas.c.evicted == ev0);
    }
    // ---------------------------------------------------------------- the fx-puff-atlas line (< 448 bytes with the 21-byte prefix at every cap)
    {
        A::LineIn li; li.frame = ~0ull; li.on = true; li.res = "atlas-texture-create-failed"; li.live = 128; li.ready = 128; li.stuck = 128;
        uint32_t* w[] = {&li.w.captures, &li.w.refreshes, &li.w.shared, &li.w.full, &li.w.recycled, &li.w.readied, &li.w.dead, &li.w.capcap, &li.w.norect,
                         &li.w.stale, &li.run.captures, &li.run.full, &li.drawn, &li.amiss, &li.unb, &li.want, &li.hostno, &li.capno, &li.run_drawn, &li.run_unb, &li.run_want};
        for (uint32_t* x : w) *x = 4000000000u;
        li.hwhy = "atlas-key-table-not-srv-and-more"; li.cwhy = "atlas-no-copy-pso-and-more";
        li.lay = "0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p"; li.w.evicted = 4000000000u; li.qpin = 4000000000u;
        char b[600]; A::format_line(b, sizeof b, li);
        const size_t len = std::strlen(b) + std::strlen("gpu-copy-step1;event=");
        std::printf("  fx-puff-atlas line at every cap: %zu bytes with the prefix\n", len);
        CHECK(len < 448 && std::strncmp(b, "fx-puff-atlas;frame=", 20) == 0);
        CHECK(std::strstr(b, ";on=1;res=atlas-texture-create-failed;tiles=128/128/128;of=128;cap=99999;") && std::strstr(b, ";unb=99999;want=99999;"));
        CHECK(std::strstr(b, ";run=cap:9999999,drawn:9999999,unb:9999999,want:9999999") && std::strstr(b, ";ev=99999;") && std::strstr(b, ";lay=0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p;"));   // (M6b) run 7 digits (step164 saturated want at 99999), the layout
        CHECK(std::strstr(b, ";nor=99999;stl=9999;qpin=999;drawn="));   // review 2026-11-01 F1 / F3 counters
        A::LineIn z; A::format_line(b, sizeof b, z);
        CHECK(std::strstr(b, ";stl=0;qpin=0;"));
        // review 2026-11-01 F2: the read-only compute census line (run totals)
        {
            char cl[200];
            const int nc = A::format_compute_line(cl, sizeof cl, ~0ull, 7, 4000000000u, 4000000000u, 4000000000u, 4000000000u, 4000000000u);
            CHECK(nc > 0 && std::strlen(cl) + std::strlen("gpu-copy-step1;event=") < 160);
            CHECK(std::strstr(cl, "fx-puff-atlas-compute;frame=") == cl && std::strstr(cl, ";hook=1;groups=4000000000;dispatch=4000000000;after=4000000000;"));
            A::format_compute_line(cl, sizeof cl, 5, -1, 3, 0, 0, 0, 0);
            CHECK(std::strcmp(cl, "fx-puff-atlas-compute;frame=5;hook=-1;groups=3;dispatch=0;after=0;ind=0;lists=0") == 0);
        }
        CHECK(std::strstr(b, ";on=0;res=not-requested;tiles=0/0/0;of=128;cap=0;") && std::strstr(b, ";hwhy=none;lay=-;capno=0;cwhy=none;"));
        { char h[300]; A::format_host_line(h, sizeof h, "0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p", nullptr, 2, true);
          CHECK(std::string(h).find("fx-puff-atlas-host;why=ok;heaps=2;sampler-heap=1;layout=0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p;legend=") == 0 && std::strlen(h) + 21 < 448); }
    }
    // ---------------------------------------------------------------- option, stage switches, option line
    {
        std::string id, why;
        { T::Conventions c; CHECK(!c.fx_atlas); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxmem:6;fxcloud:120;fxatlas:1", id, c, why) && c.fx_atlas); }
        { T::Conventions c; CHECK(T::parse_stage_option("m03k;fxatlas:0", id, c, why) && !c.fx_atlas); }
        { T::Conventions c; CHECK(!T::parse_stage_option("m03k;fxatlas:2", id, c, why)); }
        { T::Conventions c; CHECK(!T::parse_stage_option("m03k;fxatlas:on", id, c, why)); }
        s5::PuffCloud pc{120.0f, 40000.0f, true, 1, true};
        CHECK(s5::stage_effects(6, false, 0, true, 0.0f, false, 8.0f, 1500.0f, 0, 2500.0f, pc).puff_atlas);
        for (int m = 0; m <= 5; ++m) CHECK(!s5::stage_effects(m, false, 0, true, 0.0f, false, 8.0f, 1500.0f, 0, 2500.0f, pc).puff_atlas);   // fxmem:6 only
        CHECK(!s5::stage_effects(6, false).puff_atlas && !s5::effects_puff_atlas_flag().load());
        s5::apply_stage_effects(s5::stage_effects(6, false, 0, true, 0.0f, false, 8.0f, 1500.0f, 0, 2500.0f, pc));
        CHECK(s5::effects_puff_atlas_flag().load());
        s5::clear_stage_effects(); CHECK(!s5::effects_puff_atlas_flag().load());
        char b[460];
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 8.0f, 1500.0f, 0, 2500.0f, pc) && std::strstr(b, ";xkey=1;clip=fade;atlas=1;log=") && std::strstr(b, "fx-puff-atlas"));
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 5, 8.0f, 1500.0f, 0, 2500.0f, pc) && std::strstr(b, ";atlas=0;"));
        pc.atlas = false; CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 8.0f, 1500.0f, 0, 2500.0f, pc) && std::strstr(b, ";atlas=0;"));
        CHECK(s5::fx_puff_trail_line(b, sizeof b, 6, 30.0f, 100000.0f, 1, 50000.0f, s5::PuffCloud{600.0f, 200000.0f, false, 2, true}) && std::strlen(b) < 448);
    }
    std::printf("fx_puff_atlas_offline: %u checks passed\n", g_checks);
    return 0;
}
