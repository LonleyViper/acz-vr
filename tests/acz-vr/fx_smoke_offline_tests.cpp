// EFFECTS-OUTSIDE (C) offline tests: missile smoke replayed from an eye camera (src/acz-vr/fx_smoke.h; docs/world-render/effects-outside-window.md section 8).
// A synthetic smoke record in the layout read by the dumped GS (EffDrawMissileSmoke_g, D-352) holds a 20-point trail on an arc from straight ahead of the
// game camera to 76 deg right of it. The CPU emulation of the GS shows: with the game's own matrices only the part inside the game window produces puffs;
// with +0/+64 substituted for a head looking 60 deg right (100 deg eye) the off-window part produces puffs at the eye-projected positions of the trail
// points; byte identity when nothing changes; depth; lens rescale; trail behind the game camera seen by a head looking back; record checks; bounded
// capture; the live summary line. The random table is constant so the GPU/CPU sin() difference in the hash cannot matter.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fx_smoke.h"
#include "fx_smoke_gpu.h"   // fxmem:4 (the tests at the end)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace acz::fxsmoke;
using acz::fxmem::kDepthB;
using acz::fxmem::kGsCentre;
using acz::fxmem::kGsScaleX;
using acz::fxmem::kGsScaleY;
using acz::fxmem::kZ0;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTanX = 0.6152, kTanY = 0.3460;   // the game lens (63.2 x 38.1 deg)
constexpr double kEyeTan = 1.2;                    // a ~100 deg headset eye
double deg(double d) { return d * kPi / 180.0; }

struct Cam { double e[3]; double yaw, pitch; };
void axes(double yaw, double pitch, double f[3], double r[3], double u[3]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    f[0] = -sy * cp; f[1] = sp; f[2] = -cy * cp; r[0] = cy; r[1] = 0; r[2] = -sy;
    u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
}
// The world -> PS2-screen matrix A of a camera with lens tangents (tx, ty), in the stored-frame form (fx_memory_offline_tests make_a).
void make_a(const Cam& c, double tx, double ty, double a[16]) {
    double f[3], r[3], u[3]; axes(c.yaw, c.pitch, f, r, u);
    auto lin = [&](const double v[3], double s, double col[4]) {
        for (int i = 0; i < 3; ++i) col[i] = s * v[i];
        col[3] = -s * (v[0] * c.e[0] + v[1] * c.e[1] + v[2] * c.e[2]);
    };
    double w[4], cx[4], cy[4];
    lin(f, 1.0, w); lin(r, 1.0 / (1.008 * tx), cx); lin(u, -1.0 / (1.008 * ty), cy);
    for (int k = 0; k < 4; ++k) {
        a[k * 4 + 0] = kGsScaleX * cx[k] + kGsCentre * w[k];
        a[k * 4 + 1] = kGsScaleY * cy[k] + kGsCentre * w[k];
        a[k * 4 + 2] = kDepthB * w[k] + (k == 3 ? kZ0 : 0.0);
        a[k * 4 + 3] = w[k];
    }
}
// A as the record holds it (float) and back.
void a_float_roundtrip(const double in[16], double out[16]) { for (int i = 0; i < 16; ++i) out[i] = double(float(in[i])); }

const Cam kGame{{0.0, 1000.0, 0.0}, 0.0, 0.0};
constexpr double kRadius = 100.0;  // world puff radius (size curve value; the GS drops puffs with curve / w < 1/16, i.e. beyond 1,600 units here)

// A smoke record: game A at +0, the game-window clip (guard 1, stand-in for the game's own clip matrix) at +64, constant curves (alpha 128 = output 1.0,
// size kRadius), lens = A's focal lengths, no drift unless given, every point's seed 0.
std::vector<uint8_t> make_record(const Cam& game, const std::vector<std::vector<double>>& pts, const double drift[3] = nullptr, double gain = 1.0) {
    std::vector<uint8_t> b(kRecordBytes, 0);
    uint8_t* r = b.data();
    double a0[16], a[16]; make_a(game, kTanX, kTanY, a0); a_float_roundtrip(a0, a);
    double c[16]; clip_from_a(a, 1.0, c);
    for (unsigned i = 0; i < 16; ++i) { wf(r, kOffA + 4 * i, float(a[i])); wf(r, kOffClip + 4 * i, float(c[i])); }
    double fx = 0, fy = 0; CHECK(screen_focal(a, fx, fy));
    wf(r, kOffAlphaDist, 1.0f);
    const float lx = float(gain * fx), ly = float(gain * fy);   // the live form (D-361): (g fx, g fy, 1/(g fx), 1/(g fy))
    wf(r, kOffLens, lx); wf(r, kOffLens + 4, ly); wf(r, kOffLens + 8, float(1.0 / double(lx))); wf(r, kOffLens + 12, float(1.0 / double(ly)));
    wf(r, kOffScale, 255.0f); wf(r, kOffScale + 4, 255.0f); wf(r, kOffScale + 8, 1.0f); wf(r, kOffScale + 12, 0.0f);
    wf(r, kOffAge + 12, 0.5f);                              // age 0.5 for every point (.x = .y = .z = 0)
    wf(r, kOffNearFade, 1.0f);                              // w > 1: no near fade
    const int32_t one = 128;
    for (unsigned k = 0; k < 16; ++k) std::memcpy(r + kOffColour + 4 * k, &one, 4);
    wf(r, kOffUv, 8.0f); wf(r, kOffUv + 4, 8.0f); wf(r, kOffUv + 8, 1.0f); wf(r, kOffUv + 12, 0.0f);
    for (unsigned i = 0; i < 8; ++i) {
        wf(r, kOffCurveAKeys + 4 * i, float(32 * i)); wf(r, kOffCurveAVals + 4 * i, 128.0f);
        wf(r, kOffCurveBKeys + 4 * i, float(32 * i)); wf(r, kOffCurveBVals + 4 * i, float(kRadius));
    }
    for (unsigned n = 0; n < pts.size() && n < kMaxPoints; ++n) {
        const unsigned o = kOffPoints + n * kPointStride;
        for (unsigned k = 0; k < 3; ++k) {
            wf(r, o + 4 * k, float(pts[n][k]));
            wf(r, o + 16 + 4 * k, drift ? float(drift[k]) : 0.0f);
        }
    }
    return b;
}
RandomTable const_table() { RandomTable t; for (auto& e : t.v) { e[0] = 0.5f; e[1] = 0.25f; e[2] = 0.0f; e[3] = 0.0f; } return t; }
// The trail: 20 points on an arc of radius 800 around the game camera, 0 .. 76 deg to its right, at the camera's height.
std::vector<std::vector<double>> arc_points() {
    std::vector<std::vector<double>> p;
    for (int n = 0; n < 20; ++n) { const double t = deg(4.0 * n); p.push_back({kGame.e[0] + 800.0 * std::sin(t), kGame.e[1], kGame.e[2] - 800.0 * std::cos(t)}); }
    return p;
}
DrawArgs full_draw() { DrawArgs d; d.vertex_count = kMaxSegments; return d; }
bool proj(const double a[16], const std::vector<double>& p, double& x, double& y, double& w) {
    double h[4]; xform(a, p.data(), h); if (!(h[3] > 0)) return false; x = h[0] / h[3]; y = h[1] / h[3]; w = h[3]; return true;
}

void test_curve_and_layout() {
    std::vector<uint8_t> b(kRecordBytes, 0); uint8_t* r = b.data();
    const float keys[8] = {10, 20, 40, 80, 120, 160, 200, 240}, vals[8] = {1, 3, 5, 9, 13, 17, 21, 25};
    for (unsigned i = 0; i < 8; ++i) { wf(r, kOffCurveAKeys + 4 * i, keys[i]); wf(r, kOffCurveAVals + 4 * i, vals[i]); }
    CHECK(curve(r, kOffCurveAKeys, kOffCurveAVals, 5.0) == 1.0);               // below key0: value0
    CHECK(std::fabs(curve(r, kOffCurveAKeys, kOffCurveAVals, 15.0) - 2.0) < 1e-12);
    CHECK(std::fabs(curve(r, kOffCurveAKeys, kOffCurveAVals, 60.0) - 7.0) < 1e-12);
    CHECK(std::fabs(curve(r, kOffCurveAKeys, kOffCurveAVals, 220.0) - 23.0) < 1e-12);
    CHECK(std::fabs(curve(r, kOffCurveAKeys, kOffCurveAVals, 250.0) - 26.0) < 1e-12);  // beyond key7: the last piece extrapolated
    CHECK(kOffPoints + kMaxPoints * kPointStride == 1584);
    // a constant table makes the hash irrelevant
    const RandomTable t = const_table();
    for (double s : {0.0, 1.0, 53.0, 1e4, -7.5}) { const unsigned i = hash_index(s); CHECK(i < 127); CHECK(t.v[i][0] == 0.5f); }
}

void test_game_view_only_inside_window() {
    const auto rec = make_record(kGame, arc_points());
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    CHECK(record_check(rec.data(), rec.size(), full_draw()) == nullptr);
    std::vector<Puff> out; const GsStats st = emulate_draw(rec.data(), rnd, sc, full_draw(), out);
    CHECK(st.segments == 19);
    CHECK(st.puffs > 0);
    CHECK(st.culled_outside >= 10);           // segments wholly right of the 31.6 deg window edge
    unsigned maxSeg = 0;
    for (const Puff& p : out) {
        CHECK(std::fabs(p.screen[0] - kGsCentre) <= 256.0 + 1e-3);
        CHECK(std::fabs(p.screen[1] - kGsCentre) <= 224.0 + 1e-3);
        CHECK(p.sv[0][3] == 1.0f);
        maxSeg = p.segment > maxSeg ? p.segment : maxSeg;
    }
    CHECK(maxSeg <= 7);                       // point 7 = 28 deg (inside), point 8 = 32 deg (outside)
    std::printf("  game view: %u puffs from %u segments, %u culled, last segment %u\n", st.puffs, st.segments, st.culled_outside, maxSeg);
}

// The core claim: the same record with +0/+64 for a head 60 deg to the right shows the trail beyond the game window, registered.
void test_eye_view_outside_window() {
    const auto pts = arc_points();
    const auto rec = make_record(kGame, pts);
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    const Cam eye{{kGame.e[0], kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0};
    double ae[16]; make_a(eye, kEyeTan, kEyeTan, ae);
    std::vector<uint8_t> sub(kRecordBytes);
    SubstCfg cfg;   // guard 1.25, no lens rescale, depth scale 1
    CHECK(substitute(rec.data(), ae, cfg, sub.data()) == nullptr);
    CHECK(std::memcmp(sub.data() + 128, rec.data() + 128, kRecordBytes - 128) == 0);   // only +0..+127 change
    double aeF[16]; load_m(sub.data(), kOffA, aeF);
    std::vector<Puff> out; const GsStats st = emulate_draw(sub.data(), rnd, sc, full_draw(), out);
    CHECK(st.puffs > 0);
    unsigned outsideGame = 0, registered = 0; double worst = 0;
    double ag[16]; load_m(rec.data(), kOffA, ag);
    for (const Puff& p : out) {
        CHECK(std::fabs(p.screen[0] - kGsCentre) <= 256.0 * 1.25 + 1e-3);
        CHECK(std::fabs(p.screen[1] - kGsCentre) <= 224.0 * 1.25 + 1e-3);
        if (p.segment >= 8) ++outsideGame;
        if (p.u == 0.0) {   // the puff at point `segment` itself: its eye projection
            double x, y, w; CHECK(proj(aeF, pts[p.segment], x, y, w));
            const double e = std::fmax(std::fabs(x - p.screen[0]), std::fabs(y - p.screen[1]));
            worst = std::fmax(worst, e); ++registered;
            double gx, gy, gw;   // and the game would not have shown it when it lies outside its window
            if (p.segment >= 8) CHECK(!proj(ag, pts[p.segment], gx, gy, gw) || !acz::fxmem::in_window(gx, gy));
            CHECK(std::fabs(double(p.sv[0][2]) - (kZ0 / w + kDepthB) * kDepthUnit) <= 1e-6 * ((kZ0 / w) * kDepthUnit));
        }
    }
    CHECK(outsideGame > 0);
    CHECK(registered >= 10);
    CHECK(worst < 1e-3);
    std::printf("  eye 60 deg right: %u puffs, %u from trail segments the game window never shows, %u point puffs registered within %.2g GS px\n",
                st.puffs, outsideGame, registered, worst);
}

void test_identity_bytes() {
    const auto rec = make_record(kGame, arc_points());
    double a[16]; load_m(rec.data(), kOffA, a);
    std::vector<uint8_t> sub(kRecordBytes);
    SubstCfg cfg; cfg.guard = 1.0;
    CHECK(substitute(rec.data(), a, cfg, sub.data()) == nullptr);
    CHECK(std::memcmp(sub.data(), rec.data(), kRecordBytes) == 0);   // the game's camera and window: the raw bytes
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    std::vector<Puff> p0, p1; emulate_draw(rec.data(), rnd, sc, full_draw(), p0); emulate_draw(sub.data(), rnd, sc, full_draw(), p1);
    CHECK(p0.size() == p1.size());
    for (size_t i = 0; i < p0.size() && i < p1.size(); ++i) CHECK(std::memcmp(p0[i].sv, p1[i].sv, sizeof p0[i].sv) == 0);
    // the guard band only adds puffs at the edge; every in-window puff is unchanged
    SubstCfg g; std::vector<uint8_t> sg(kRecordBytes);
    CHECK(substitute(rec.data(), a, g, sg.data()) == nullptr);
    std::vector<Puff> p2; emulate_draw(sg.data(), rnd, sc, full_draw(), p2);
    CHECK(p2.size() >= p0.size());
}

void test_depth_scale() {
    const auto rec = make_record(kGame, arc_points());
    const Cam eye{{kGame.e[0], kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0};
    double ae[16]; make_a(eye, kEyeTan, kEyeTan, ae);
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    std::vector<uint8_t> s1(kRecordBytes), s2(kRecordBytes);
    SubstCfg c1, c2; c2.depth_scale = 1.0f / 1024.0f;
    CHECK(substitute(rec.data(), ae, c1, s1.data()) == nullptr);
    CHECK(substitute(rec.data(), ae, c2, s2.data()) == nullptr);
    CHECK(std::memcmp(s1.data() + kOffClip, s2.data() + kOffClip, 64) == 0);   // the clip never sees the depth scale
    std::vector<Puff> p1, p2; emulate_draw(s1.data(), rnd, sc, full_draw(), p1); emulate_draw(s2.data(), rnd, sc, full_draw(), p2);
    CHECK(!p1.empty() && p1.size() == p2.size());
    for (size_t i = 0; i < p1.size() && i < p2.size(); ++i) {
        CHECK(std::fabs(double(p2[i].sv[0][2]) - double(p1[i].sv[0][2]) / 1024.0) <= 1e-6 * double(p1[i].sv[0][2]) / 1024.0 + 1e-12);
        CHECK(p1[i].sv[0][0] == p2[i].sv[0][0] && p1[i].sv[0][1] == p2[i].sv[0][1]);
        CHECK(p1[i].sv[0][2] > 0.0f && p1[i].sv[0][2] <= 1.0f);
    }
}

// The puff's world radius as the eye sees it: half size x w / f. Correct only with the lens field rescaled to the eye.
void test_lens_rescale() {
    const auto pts = arc_points();
    const auto rec = make_record(kGame, pts);
    const LensFit L = lens_fit(rec.data());
    CHECK(L.proven);
    const Cam eye{{kGame.e[0], kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0};
    double ae[16]; make_a(eye, kEyeTan, kEyeTan, ae);
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    double fe = 0, fey = 0; CHECK(screen_focal(ae, fe, fey));
    CHECK(std::fabs(fe - 256.0 / kEyeTan) < 1e-6 * fe);
    for (int rescale = 0; rescale < 2; ++rescale) {
        SubstCfg cfg; cfg.rescale_lens = rescale != 0;
        std::vector<uint8_t> s(kRecordBytes); CHECK(substitute(rec.data(), ae, cfg, s.data()) == nullptr);
        double aeF[16]; load_m(s.data(), kOffA, aeF);
        std::vector<Puff> out; emulate_draw(s.data(), rnd, sc, full_draw(), out);
        unsigned n = 0;
        for (const Puff& p : out) {
            if (p.u != 0.0) continue;
            double x, y, w; CHECK(proj(aeF, pts[p.segment], x, y, w));
            const double radius = p.half[0] * w / fe, radiusY = std::fabs(p.half[1]) * w / fey;
            const double expect = rescale ? kRadius : kRadius * L.fx / fe;   // without the rescale: the game's focal length on the eye's screen
            CHECK(std::fabs(radius - expect) < 1e-4 * expect);
            if (rescale) CHECK(std::fabs(radiusY - kRadius) < 1e-4 * kRadius);
            ++n;
        }
        CHECK(n > 0);
    }
    // a record whose +144 is not A's focal length: the rescale is refused (fail closed), the plain substitution still works
    auto bad = rec; wf(bad.data(), kOffLens, float(L.fx * 1.5));
    CHECK(!lens_fit(bad.data()).proven);
    std::vector<uint8_t> s(kRecordBytes); SubstCfg cfg; cfg.rescale_lens = true;
    CHECK(std::strcmp(substitute(bad.data(), ae, cfg, s.data()), "smoke-lens-field-unproven") == 0);
    cfg.rescale_lens = false;
    CHECK(substitute(bad.data(), ae, cfg, s.data()) == nullptr);
}

// A trail BEHIND the game camera (the aircraft flew through it): the game shows nothing, a head looking back sees it; nothing behind the eye is emitted.
void test_trail_behind() {
    std::vector<std::vector<double>> pts;
    for (int n = 0; n < 20; ++n) pts.push_back({kGame.e[0] + 3.0, kGame.e[1] - 5.0, kGame.e[2] - 200.0 + 60.0 * n});   // from 200 ahead to 940 behind
    const auto rec = make_record(kGame, pts);
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    std::vector<Puff> g; emulate_draw(rec.data(), rnd, sc, full_draw(), g);
    for (const Puff& p : g) CHECK(p.segment <= 3);   // only the part ahead of the game camera (points 0..3 are at -200..-20)
    const Cam back{{kGame.e[0], kGame.e[1], kGame.e[2]}, deg(180.0), 0.0};
    double ab[16]; make_a(back, kEyeTan, kEyeTan, ab);
    std::vector<uint8_t> s(kRecordBytes); SubstCfg cfg; CHECK(substitute(rec.data(), ab, cfg, s.data()) == nullptr);
    std::vector<Puff> e; const GsStats st = emulate_draw(s.data(), rnd, sc, full_draw(), e);
    unsigned behindGame = 0;
    for (const Puff& p : e) {
        CHECK(p.sv[0][2] >= 0.0f && p.sv[0][2] <= 1.0f);   // never behind the eye (near clip at ~1 unit)
        if (p.segment >= 4) ++behindGame;
    }
    CHECK(behindGame > 0);
    CHECK(st.culled_outside >= 1);   // the segments ahead of the game camera are behind the looking-back eye
    std::printf("  looking back: %u puffs (game view: %zu), %u culled\n", st.puffs, g.size(), st.culled_outside);
}

void test_record_check() {
    const auto good = make_record(kGame, arc_points());
    DrawArgs d = full_draw();
    CHECK(record_check(good.data(), good.size(), d) == nullptr);
    CHECK(std::strcmp(record_check(good.data(), 100, d), "smoke-record-size") == 0);
    CHECK(std::strcmp(record_check(nullptr, kRecordBytes, d), "smoke-record-size") == 0);
    DrawArgs d2 = d; d2.vertex_count = 20; CHECK(std::strcmp(record_check(good.data(), good.size(), d2), "smoke-draw-segments") == 0);
    d2 = d; d2.vertex_count = 0; CHECK(std::strcmp(record_check(good.data(), good.size(), d2), "smoke-draw-segments") == 0);
    d2 = d; d2.vertex_count = 5; d2.start_vertex = 15; CHECK(std::strcmp(record_check(good.data(), good.size(), d2), "smoke-draw-segments") == 0);
    d2 = d; d2.instance_count = 2; CHECK(std::strcmp(record_check(good.data(), good.size(), d2), "smoke-draw-instanced") == 0);
    auto r = good; wf(r.data(), kOffA + 4, std::numeric_limits<float>::quiet_NaN());
    CHECK(std::strcmp(record_check(r.data(), r.size(), d), "smoke-record-nonfinite") == 0);
    r = good; wf(r.data(), kOffClip + 20, std::numeric_limits<float>::infinity());
    CHECK(std::strcmp(record_check(r.data(), r.size(), d), "smoke-record-nonfinite") == 0);
    r = good; wf(r.data(), kOffA + 8, rf(r.data(), kOffA + 8) + 50.0f);   // z column no longer b x w column
    CHECK(std::strcmp(record_check(r.data(), r.size(), d), "smoke-a-depth-column") == 0);
    r = good; wf(r.data(), kOffCurveAVals + 8, std::numeric_limits<float>::quiet_NaN());
    CHECK(std::strcmp(record_check(r.data(), r.size(), d), "smoke-params-nonfinite") == 0);
    r = good; const int32_t nanBits = 0x7fc00000; std::memcpy(r.data() + kOffInt208, &nanBits, 4);   // an int the GS never reads
    CHECK(record_check(r.data(), r.size(), d) == nullptr);
    r = good; wf(r.data(), kOffPoints + 19 * kPointStride, std::numeric_limits<float>::quiet_NaN());
    CHECK(std::strcmp(record_check(r.data(), r.size(), d), "smoke-point-nonfinite") == 0);
    d2 = d; d2.vertex_count = 5;   // the NaN point 19 is not read by a 5-segment draw
    CHECK(record_check(r.data(), r.size(), d2) == nullptr);
    // substitution input errors
    double a[16]; load_m(good.data(), kOffA, a);
    std::vector<uint8_t> s(kRecordBytes); SubstCfg cfg;
    cfg.guard = 0.5; CHECK(std::strcmp(substitute(good.data(), a, cfg, s.data()), "smoke-subst-guard") == 0);
    cfg = SubstCfg{}; cfg.depth_scale = 0.0f; CHECK(std::strcmp(substitute(good.data(), a, cfg, s.data()), "smoke-subst-depth-scale") == 0);
    cfg = SubstCfg{}; double z[16] = {}; CHECK(std::strcmp(substitute(good.data(), z, cfg, s.data()), "smoke-subst-depth-column") == 0);
    double nanA[16]; std::memcpy(nanA, a, sizeof a); nanA[5] = std::numeric_limits<double>::quiet_NaN();
    CHECK(std::strcmp(substitute(good.data(), nanA, cfg, s.data()), "smoke-subst-nonfinite") == 0);
}

void test_capture_and_summary() {
    const auto arc = make_record(kGame, arc_points());
    std::vector<std::vector<double>> beh;
    for (int n = 0; n < 6; ++n) beh.push_back({0.0, 1000.0, 100.0 + 50.0 * n});   // 6 points behind the game camera
    const auto back = make_record(kGame, beh);
    static FrameCapture fc;   // ~105 KB: static, not on the stack
    fc.begin(42);
    const float roots[6] = {512.0f, -448.0f, 2048.0f, 2048.0f, 0.31294f, 0.0f};   // live run Q order: cb10 = scale, cb11 = centre, cb12 = alpha ref
    CHECK(fc.add(arc.data(), arc.size(), full_draw(), 0x1000, 0x2000, 7, roots) == nullptr);
    DrawArgs d5; d5.vertex_count = 5;
    CHECK(fc.add(back.data(), back.size(), d5, 0x1700, 0x2000, 7, nullptr) == nullptr);
    CHECK(fc.add(arc.data(), 10, full_draw(), 0, 0, 0, nullptr) != nullptr);   // refused, counted
    CHECK(fc.count() == 2 && fc.refused() == 1 && fc.frame() == 42);
    CHECK(std::memcmp(fc.at(0).rec, arc.data(), kRecordBytes) == 0 && fc.at(0).root_known && fc.at(0).roots[0][1] == -448.0f && fc.at(0).roots[1][1] == 2048.0f && !fc.at(1).root_known);
    const FrameSummary s = summarise(fc);
    CHECK(s.draws == 2 && s.segments == 19 + 5 && s.points == 20 + 6);
    CHECK(s.in_window == 8 && s.outside_window == 12 && s.behind == 6);   // arc points 0..7 inside (<= 28 deg), 8..19 outside
    CHECK(s.a_differs == 0 && s.lens_proven && s.roots && s.cb10[0] == 512.0f && s.cb11[0] == 2048.0f);
    char line[512]; const int n = format_summary(line, sizeof line, fc.frame(), s);
    CHECK(n > 0 && n < int(sizeof line));
    const std::string l(line);
    CHECK(l.find("fx-smoke-frame;frame=42;draws=2;segments=24;points=26;pts-in-window=8;pts-outside-window=12;pts-behind=6;a-differs=0;") == 0);
    CHECK(l.find(";lens=1,1,1.0000,1.0000(proven);roots=known;cb10=512,-448;cb11=2048,2048;cb12=0.31294,0;") != std::string::npos);
    // (section 18) the live m03k peak: 27 smoke draws in one frame (16 stored + 11 over the old cap) are now ALL stored: no window-only trail
    fc.begin(45);
    for (int i = 0; i < 27; ++i) CHECK(fc.add(arc.data(), arc.size(), full_draw(), 0, 0, 0, nullptr) == nullptr);
    CHECK(fc.count() == 27 && fc.overflow() == 0 && fc.refused() == 0 && fc.last_why() == nullptr && !fc.full());
    CHECK(FrameCapture::kCap == 64 && FrameCapture::kCap >= 2 * 27);
    // the cap: kCap stored, the rest counted
    fc.begin(43);
    for (unsigned i = 0; i < FrameCapture::kCap + 4; ++i) fc.add(arc.data(), arc.size(), full_draw(), 0, 0, 0, nullptr);
    CHECK(fc.count() == FrameCapture::kCap && fc.overflow() == 4 && fc.refused() == 0);
    CHECK(std::strcmp(fc.last_why(), "smoke-capture-cap") == 0);
    fc.begin(44);
    CHECK(fc.count() == 0 && fc.overflow() == 0 && summarise(fc).draws == 0);
}

// The live producer's sequence (gpu_copy_step5_hooks.inl step5FxSmokeCapture / step5FxSmokeAccount, fxmem:1): the cap tested before any read, unreadable
// records counted with their reason, the roots' reason kept, +0 compared with the same frame's DrawACM_v A.
void test_live_capture_path() {
    const auto arc = make_record(kGame, arc_points());
    static FrameCapture fc;
    fc.begin(0);
    CHECK(!fc.full());
    fc.note_refused("not-mapped");                                        // step3MappedSpan_locked refused the span: nothing stored
    CHECK(fc.count() == 0 && fc.refused() == 1 && std::strcmp(fc.last_why(), "not-mapped") == 0);
    CHECK(fc.add(arc.data(), arc.size(), full_draw(), 0x1000, 0, 7, nullptr, "absent") == nullptr);   // roots unknown: reason kept
    CHECK(fc.count() == 1 && !fc.at(0).root_known && std::strcmp(fc.at(0).roots_why, "absent") == 0);
    // +0 vs the frame's object A: identical floats -> 0; one element off by 0.5 -> 0.5
    float objA[16]; for (unsigned k = 0; k < 16; ++k) objA[k] = rf(arc.data(), kOffA + 4 * k);
    double diff = -1, rel = -1;
    CHECK(compare_object_a(fc, objA, diff, rel) && diff == 0.0 && rel == 0.0);
    FrameSummary s = summarise(fc, objA);
    CHECK(s.obj_a && s.obj_a_diff == 0.0 && !s.roots && std::strcmp(s.roots_why, "absent") == 0 && s.refused == 1);
    char line[1024]; format_summary(line, sizeof line, 7, s, 2);
    std::string l(line);
    CHECK(l.find(";roots=unknown;") != std::string::npos && l.find(";a-vs-obj=0(rel=0);roots-why=absent;last-why=not-mapped;skipped=2") != std::string::npos);
    float objB[16]; std::memcpy(objB, objA, sizeof objA); objB[3] = float(double(objB[3]) + 0.5);   // row 0, w column = the view direction's x (|.| <= 1)
    CHECK(compare_object_a(fc, objB, diff, rel) && std::fabs(diff - std::fabs(double(objB[3]) - double(objA[3]))) < 1e-9 && diff > 0.25 && rel > 0 && rel < 1);
    s = summarise(fc, objB);
    format_summary(line, sizeof line, 7, s); l = line;
    CHECK(l.find(";a-vs-obj=0.") != std::string::npos);
    s = summarise(fc, nullptr);   // no world-objects record this frame
    format_summary(line, sizeof line, 7, s); l = line;
    CHECK(!s.obj_a && l.find(";a-vs-obj=none;") != std::string::npos);
    float bad[16]; std::memcpy(bad, objA, sizeof objA); bad[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!compare_object_a(fc, bad, diff, rel));
    // the cap: once full, the producer counts an overflow WITHOUT reading the record
    for (unsigned i = 1; i < FrameCapture::kCap; ++i) CHECK(fc.add(arc.data(), arc.size(), full_draw(), 0, 0, 0, nullptr) == nullptr);
    CHECK(fc.full());
    fc.note_overflow();
    CHECK(fc.count() == FrameCapture::kCap && fc.overflow() == 1 && std::strcmp(fc.last_why(), "smoke-capture-cap") == 0);
    // a new frame starts empty (the previous frame's records are never summarised again)
    fc.begin(0);
    CHECK(!fc.full() && fc.count() == 0 && fc.refused() == 0 && fc.overflow() == 0);
}

// The line stays inside the feature log's 480-byte line (with the 21-byte "gpu-copy-step1;event=" prefix) for any values.
void test_line_limit() {
    FrameSummary s;
    s.draws = s.segments = s.points = s.in_window = s.outside_window = s.behind = s.a_differs = s.overflow = s.refused = 0xffffffffu;
    s.lens_rx = s.lens_ry = s.lens_rzx = s.lens_rwy = -1e300;
    const float tiny = -1.17549435e-38f;
    for (int k = 0; k < 2; ++k) { s.cb10[k] = tiny; s.cb11[k] = tiny; s.cb12[k] = tiny; }
    s.obj_a = true; s.obj_a_diff = 1e300; s.obj_a_rel = -1e300;
    s.roots_why = "a-very-long-reason-that-would-not-fit-the-line-at-all-xxxxxxxxxxxxxxxxxxxx";
    s.last_why = s.roots_why;
    char line[1024];
    const int n = format_summary(line, sizeof line, ~0ull, s, 0xffffffffu);
    CHECK(n > 0 && size_t(n) + kLinePrefix < kLineLimit);
    s.lens_rx = std::numeric_limits<double>::quiet_NaN(); s.cb10[0] = std::numeric_limits<float>::infinity();
    const int m = format_summary(line, sizeof line, ~0ull, s, 0xffffffffu);
    CHECK(m > 0 && size_t(m) + kLinePrefix < kLineLimit);
    // the producer's real buffer: a truncated write still terminates (snprintf), never overruns
    char shortbuf[64]; CHECK(format_summary(shortbuf, sizeof shortbuf, 1, s) > 63 && std::strlen(shortbuf) == 63);
}

// The rate limit: the first 3 smoke frames, then at most one line per second; the frames in between are reported as skipped.
void test_log_gate() {
    LogGate g; unsigned sk = 99;
    CHECK(g.admit(0, sk) && sk == 0);
    CHECK(g.admit(1, sk) && sk == 0);
    CHECK(g.admit(2, sk) && sk == 0);
    CHECK(!g.admit(3, sk) && sk == 0);
    CHECK(!g.admit(500, sk));
    CHECK(g.admit(1003, sk) && sk == 2);
    CHECK(!g.admit(1500, sk));
    CHECK(g.admit(2003, sk) && sk == 1);
    CHECK(g.emitted == 5 && g.skipped == 0);
}

// cb10 / cb11 (GS) and cb12 (PS) only from root constants of the bound layout; anything else is named, never read.
void test_root_constants() {
    RootParamView v[6];
    v[0].kind = 0;                                                   // a table (SRV only)
    v[1].kind = 1; v[1].visibility = 0; v[1].reg = 10; v[1].constants = 2;   // cb10: root constants, ALL
    v[2].kind = 1; v[2].visibility = 4; v[2].reg = 11; v[2].constants = 2;   // cb11: root constants, GEOMETRY
    v[3].kind = 1; v[3].visibility = 5; v[3].reg = 12; v[3].constants = 1;   // cb12: one word only
    unsigned idx = 99;
    CHECK(find_root_constants(v, 4, 0, 10, idx) == nullptr && idx == 1);
    CHECK(find_root_constants(v, 4, 0, 11, idx) == nullptr && idx == 2);
    CHECK(std::strcmp(find_root_constants(v, 4, 0, 12, idx), "constants-short") == 0);
    CHECK(std::strcmp(find_root_constants(v, 4, kDenyGs, 10, idx), "stage-denied") == 0);
    CHECK(std::strcmp(find_root_constants(v, 4, kDenyPs, 12, idx), "stage-denied") == 0 && find_root_constants(v, 4, kDenyPs, 10, idx) == nullptr);
    CHECK(std::strcmp(find_root_constants(v, 4, 0, 9, idx), "register") == 0);
    RootParamView w[3] = {v[0], v[1], v[2]};
    w[2].visibility = 1;                                                     // VERTEX only: the GS cannot see it
    CHECK(std::strcmp(find_root_constants(w, 3, 0, 11, idx), "absent") == 0);
    w[2].visibility = 4; w[2].kind = 2;                                      // a root CBV: reported, never dereferenced
    CHECK(std::strcmp(find_root_constants(w, 3, 0, 11, idx), "root-cbv") == 0);
    w[2].kind = 1; w[0].table_cbv[1] = true;                                 // a table also covers b11: ambiguous
    CHECK(std::strcmp(find_root_constants(w, 3, 0, 11, idx), "ambiguous") == 0);
    w[2].kind = 3;                                                           // only the table: not readable
    CHECK(std::strcmp(find_root_constants(w, 3, 0, 11, idx), "descriptor-table") == 0);
    w[0].table_cbv[1] = false; w[2] = w[1];                                  // two root-constant parameters for b10
    CHECK(std::strcmp(find_root_constants(w, 3, 0, 10, idx), "ambiguous") == 0);
    w[2].space = 1;                                                          // another space: not b10 of space 0
    CHECK(find_root_constants(w, 3, 0, 10, idx) == nullptr && idx == 1);
    CHECK(std::strcmp(find_root_constants(w, 0, 0, 10, idx), "absent") == 0);
    // the words: both written in this binding generation
    const float f2[2] = {512.0f, -448.0f}; uint32_t words[2]; std::memcpy(words, f2, sizeof f2);
    bool wr[2] = {true, true}; float out[2] = {};
    CHECK(root_pair(words, wr, 2, out) == nullptr && out[0] == 512.0f && out[1] == -448.0f);
    wr[1] = false; CHECK(std::strcmp(root_pair(words, wr, 2, out), "unwritten") == 0);
    CHECK(std::strcmp(root_pair(words, wr, 1, out), "unwritten") == 0 && std::strcmp(root_pair(nullptr, wr, 2, out), "unwritten") == 0);
}

// ================================================================================== replay core (D-361 facts)
const float kLiveRoots[6] = {512.0f, -448.0f, 2048.0f, 2048.0f, 0.31294f, 0.0f};   // run Q: cb10, cb11, cb12
constexpr double kLiveGain = 0.0019;

// SV_Position.xy = (X - cb11) * 2 / cb10 (DXIL handle order); the screen constants the replay requires are exactly the live ones.
void test_screen_consts() {
    const auto rec = make_record(kGame, arc_points());
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    CHECK(sc.cb10[0] == 512.0f && sc.cb10[1] == -448.0f && sc.cb11[0] == 2048.0f && sc.cb11[1] == 2048.0f);
    std::vector<Puff> out; emulate_draw(rec.data(), rnd, sc, full_draw(), out);
    CHECK(!out.empty());
    for (const Puff& p : out) {
        const double X = p.screen[0] + p.half[0] * -1.0, Y = p.screen[1];   // vertex 0: offset (-1, 0)
        CHECK(std::fabs(double(p.sv[0][0]) - (X - 2048.0) / 256.0) < 1e-5 && std::fabs(double(p.sv[0][1]) - (Y - 2048.0) / -224.0) < 1e-5);
    }
    CHECK(screen_consts_check(kLiveRoots) == nullptr);
    CHECK(std::strcmp(screen_consts_check(nullptr), "smoke-roots-unknown") == 0);
    const float swapped[6] = {2048.0f, 2048.0f, 512.0f, -448.0f, 0.0f, 0.0f};   // the pre-D-361 inferred order
    CHECK(std::strcmp(screen_consts_check(swapped), "smoke-screen-consts") == 0);
    float other[6]; std::memcpy(other, kLiveRoots, sizeof other); other[1] = 448.0f;
    CHECK(std::strcmp(screen_consts_check(other), "smoke-screen-consts") == 0);
    other[1] = -448.0f; other[4] = 7.0f;   // cb12 (the PS alpha reference) is the game's own: not part of the check
    CHECK(screen_consts_check(other) == nullptr);
}

// The live lens form (g fx, g fy, 1/(g fx), 1/(g fy)), g = 0.0019: proven; a puff's world radius is kRadius x g in the game view AND, rescaled, in the eye.
void test_lens_gain_live_form() {
    const auto pts = arc_points();
    const auto rec = make_record(kGame, pts, nullptr, kLiveGain);
    const LensFit L = lens_fit(rec.data());
    CHECK(L.proven && std::fabs(L.gain - kLiveGain) < 1e-6 && std::fabs(L.rx - kLiveGain) < 1e-6 && std::fabs(L.rzx - 1.0) < 1e-6);
    // the pre-D-361 rule (rx = 1) would have refused it; the summary line now prints the gain with 4 significant digits
    static FrameCapture fc; fc.begin(1);
    CHECK(fc.add(rec.data(), rec.size(), full_draw(), 1, 0, 0, kLiveRoots) == nullptr);
    char line[512]; format_summary(line, sizeof line, 1, summarise(fc));
    CHECK(std::string(line).find(";lens=0.0019,0.0019,1.0000,1.0000(proven);") != std::string::npos);
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    // the game view: world radius = half x w / fx = kRadius x g. The size curve of make_record is kRadius x 255 / 255 x 255 (+160.y): scale it so the
    // puffs pass the GS's 1/16 size cut: use a size curve of kRadius / g.
    auto big = rec; for (unsigned i = 0; i < 8; ++i) wf(big.data(), kOffCurveBVals + 4 * i, float(kRadius / kLiveGain));
    double ag[16]; load_m(big.data(), kOffA, ag);
    std::vector<Puff> g; emulate_draw(big.data(), rnd, sc, full_draw(), g);
    unsigned ng = 0;
    for (const Puff& p : g) if (p.u == 0.0) {
        double x, y, w; CHECK(proj(ag, pts[p.segment], x, y, w));
        CHECK(std::fabs(p.half[0] * w / L.fx - kRadius) < 1e-3 * kRadius); ++ng;
    }
    CHECK(ng > 0);
    const Cam eye{{kGame.e[0], kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0};
    double ae[16]; make_a(eye, kEyeTan, kEyeTan, ae);
    double fe = 0, fey = 0; CHECK(screen_focal(ae, fe, fey));
    SubstCfg cfg; cfg.rescale_lens = true;
    std::vector<uint8_t> s(kRecordBytes); CHECK(substitute(big.data(), ae, cfg, s.data()) == nullptr);
    CHECK(lens_fit(s.data()).proven && std::fabs(lens_fit(s.data()).gain - kLiveGain) < 1e-6);   // still the same gain against the EYE's focal lengths
    double aeF[16]; load_m(s.data(), kOffA, aeF);
    std::vector<Puff> e; emulate_draw(s.data(), rnd, sc, full_draw(), e);
    unsigned ne = 0;
    for (const Puff& p : e) if (p.u == 0.0) {
        double x, y, w; CHECK(proj(aeF, pts[p.segment], x, y, w));
        CHECK(std::fabs(p.half[0] * w / fe - kRadius) < 1e-3 * kRadius && std::fabs(std::fabs(p.half[1]) * w / fey - kRadius) < 1e-3 * kRadius); ++ne;
    }
    CHECK(ne > 0);
    // not the form: unequal gains, or .z not the inverse of .x, or a negative .x
    auto bad = rec; wf(bad.data(), kOffLens, float(rf(rec.data(), kOffLens) * 1.1)); wf(bad.data(), kOffLens + 8, 1.0f / rf(bad.data(), kOffLens));
    CHECK(!lens_fit(bad.data()).proven);
    bad = rec; wf(bad.data(), kOffLens + 8, rf(rec.data(), kOffLens + 8) * 1.05f); CHECK(!lens_fit(bad.data()).proven);
    bad = rec; wf(bad.data(), kOffLens, -rf(rec.data(), kOffLens)); wf(bad.data(), kOffLens + 8, -rf(rec.data(), kOffLens + 8)); CHECK(!lens_fit(bad.data()).proven);
    bad = rec; wf(bad.data(), kOffLens, 0.0f); CHECK(!lens_fit(bad.data()).proven && lens_fit(bad.data()).gain == 0.0);
}

// The world camera's pseudo clip matrix and S exactly as world::camera_setup builds them (gpu_copy_step5_world.h): D = vp(eye) - vp(game).
void vp_of(const double a[16], double vp[16]) {
    for (int r = 0; r < 4; ++r) {
        vp[r * 4 + 0] = (a[r * 4 + 0] - kGsCentre * a[r * 4 + 3]) / kGsScaleX;
        vp[r * 4 + 1] = (a[r * 4 + 1] - kGsCentre * a[r * 4 + 3]) / kGsScaleY;
        vp[r * 4 + 2] = a[r * 4 + 2]; vp[r * 4 + 3] = a[r * 4 + 3];
    }
}
void s_of(double S[16]) { for (int i = 0; i < 16; ++i) S[i] = 0; S[0] = kGsScaleX; S[5] = kGsScaleY; S[12] = kGsCentre; S[13] = kGsCentre; S[14] = kDepthB; S[15] = 1.0; }
struct Eyes { double D[2][16]; double S[16]; double truth[2][16]; };
Eyes make_eyes(const uint8_t* rec, const Cam eyes[2], double tan) {
    Eyes E; s_of(E.S);
    double ag[16]; load_m(rec, kOffA, ag); double vg[16]; vp_of(ag, vg);
    for (int e = 0; e < 2; ++e) {
        make_a(eyes[e], tan, tan, E.truth[e]);
        double ve[16]; vp_of(E.truth[e], ve);
        for (int i = 0; i < 16; ++i) E.D[e][i] = ve[i] - vg[i];
    }
    return E;
}
void test_eye_a_from_delta() {
    const auto rec = make_record(kGame, arc_points(), nullptr, kLiveGain);
    double zero[16] = {}, S[16]; s_of(S);
    double out[16];
    auto negz = rec; wf(negz.data(), kOffA + 4 * 6, -0.0f);   // a -0.0 element survives a zero delta bit for bit (as world::substitute)
    CHECK(eye_a_from_delta(negz.data(), zero, S, out));
    for (int i = 0; i < 16; ++i) { const float f = float(out[i]); CHECK(std::memcmp(&f, negz.data() + kOffA + 4 * i, 4) == 0); }
    const Cam eyes[2] = {{{kGame.e[0] - 2.0, kGame.e[1] + 1.0, kGame.e[2]}, deg(-60.0), deg(10.0)}, {{kGame.e[0] + 2.0, kGame.e[1] + 1.0, kGame.e[2]}, deg(-60.0), deg(10.0)}};
    const Eyes E = make_eyes(rec.data(), eyes, kEyeTan);
    for (int e = 0; e < 2; ++e) {
        CHECK(eye_a_from_delta(rec.data(), E.D[e], E.S, out));
        CHECK(depth_column_ok(out));
        // equal to the eye's own A up to float32 rounding of the 1.6e8-scale translation row (as world::substitute)
        for (int i = 0; i < 16; ++i) CHECK(std::fabs(out[i] - E.truth[e][i]) <= 1e-5 * std::fmax(std::fabs(E.truth[e][i]), 1.0) + 64.0 * (i >= 12));
        double fx, fy, tx, ty; CHECK(screen_focal(out, fx, fy) && screen_focal(E.truth[e], tx, ty) && std::fabs(fx - tx) < 1e-4 * tx);
    }
    double nan[16]; std::memcpy(nan, E.D[0], sizeof nan); nan[3] = std::numeric_limits<double>::infinity();
    CHECK(!eye_a_from_delta(rec.data(), nan, E.S, out) && !eye_a_from_delta(nullptr, zero, S, out));
}

// prepare_eye_records: the full preparation (registered puffs outside the game window, world-sized, depth scaled) and every refusal.
void test_prepare_eye_records() {
    const auto pts = arc_points();
    auto rec = make_record(kGame, pts, nullptr, kLiveGain);
    for (unsigned i = 0; i < 8; ++i) wf(rec.data(), kOffCurveBVals + 4 * i, float(kRadius / kLiveGain));
    const Cam eyes[2] = {{{kGame.e[0] - 0.03, kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0}, {{kGame.e[0] + 0.03, kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0}};
    const Eyes E = make_eyes(rec.data(), eyes, kEyeTan);
    EyeDelta ed; ed.D[0] = E.D[0]; ed.D[1] = E.D[1]; ed.S = E.S; ed.world_ref64 = rec.data() + kOffA;
    ReplayCfg cfg; cfg.depth_scale = 1.0f / 1024.0f;
    std::vector<uint8_t> o0(kRecordBytes), o1(kRecordBytes);
    CHECK(prepare_eye_records(rec.data(), rec.size(), full_draw(), kLiveRoots, ed, cfg, o0.data(), o1.data()) == nullptr);
    CHECK(std::memcmp(o0.data() + 160, rec.data() + 160, kRecordBytes - 160) == 0);   // only A, the clip and the lens field change
    CHECK(std::memcmp(o0.data() + kOffA, o1.data() + kOffA, 64) != 0);                 // two eyes, two matrices
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    unsigned outside = 0, reg = 0;
    for (const auto* o : {&o0, &o1}) {
        double ae[16]; load_m(o->data(), kOffA, ae);
        double fe, fey; CHECK(screen_focal(ae, fe, fey));
        std::vector<Puff> out; emulate_draw(o->data(), rnd, sc, full_draw(), out);
        for (const Puff& p : out) {
            if (p.segment >= 8) ++outside;
            if (p.u != 0.0) continue;
            double x, y, w; CHECK(proj(ae, pts[p.segment], x, y, w));
            CHECK(std::fabs(x - p.screen[0]) < 1e-3 && std::fabs(y - p.screen[1]) < 1e-3);
            CHECK(std::fabs(p.half[0] * w / fe - kRadius) < 1e-3 * kRadius);
            CHECK(std::fabs(double(p.sv[0][2]) - (kZ0 / w + kDepthB) * kDepthUnit / 1024.0) <= 1e-5 * (kZ0 / w) * kDepthUnit / 1024.0);
            ++reg;
        }
    }
    CHECK(outside > 0 && reg >= 20);
    std::printf("  prepared eyes: %u puffs from off-window segments, %u point puffs registered, world-sized, depth x 2^-10\n", outside, reg);
    auto why = [&](const uint8_t* r, const float* roots, const EyeDelta& d) {
        const char* w = prepare_eye_records(r, kRecordBytes, full_draw(), roots, d, cfg, o0.data(), o1.data()); return std::string(w ? w : "ok");
    };
    CHECK(why(rec.data(), kLiveRoots, ed) == "ok");
    CHECK(why(rec.data(), nullptr, ed) == "smoke-roots-unknown");
    const float swapped[6] = {2048.0f, 2048.0f, 512.0f, -448.0f, 0.0f, 0.0f};
    CHECK(why(rec.data(), swapped, ed) == "smoke-screen-consts");
    auto bad = rec; wf(bad.data(), kOffLens, rf(bad.data(), kOffLens) * 1.5f);
    CHECK(why(bad.data(), kLiveRoots, ed) == "smoke-lens-field-unproven");
    EyeDelta nd = ed; nd.D[1] = nullptr; CHECK(why(rec.data(), kLiveRoots, nd) == "smoke-world-pose-missing");
    nd = ed; nd.world_ref64 = nullptr; CHECK(why(rec.data(), kLiveRoots, nd) == "smoke-world-a-missing");
    std::vector<uint8_t> otherA(64); std::memcpy(otherA.data(), rec.data(), 64); wf(otherA.data(), 12, rf(otherA.data(), 12) + 0.5f);
    nd = ed; nd.world_ref64 = otherA.data(); CHECK(why(rec.data(), kLiveRoots, nd) == "smoke-a-differs-from-world");
    bad = rec; wf(bad.data(), kOffPoints + 4, std::numeric_limits<float>::quiet_NaN()); CHECK(why(bad.data(), kLiveRoots, ed) == "smoke-point-nonfinite");
    double inf[16]; std::memcpy(inf, E.D[0], sizeof inf); inf[0] = std::numeric_limits<double>::infinity();
    nd = ed; nd.D[0] = inf; CHECK(why(rec.data(), kLiveRoots, nd) == "smoke-eye-a-nonfinite");
}

void test_replay_store_and_root_swap() {
    auto rec = make_record(kGame, arc_points(), nullptr, kLiveGain);
    const Cam eyes[2] = {{{kGame.e[0] - 0.03, kGame.e[1], kGame.e[2]}, deg(30.0), 0.0}, {{kGame.e[0] + 0.03, kGame.e[1], kGame.e[2]}, deg(30.0), 0.0}};
    const Eyes E = make_eyes(rec.data(), eyes, kEyeTan);
    EyeDelta ed; ed.D[0] = E.D[0]; ed.D[1] = E.D[1]; ed.S = E.S; ed.world_ref64 = rec.data() + kOffA;
    static ReplayStore st; st.begin();
    const ReplayCfg cfg;
    unsigned idx = 99;
    std::vector<uint8_t> o0(kRecordBytes), o1(kRecordBytes);
    CHECK(prepare_eye_records(rec.data(), rec.size(), full_draw(), kLiveRoots, ed, cfg, o0.data(), o1.data()) == nullptr);
    for (unsigned i = 0; i < ReplayStore::kCap; ++i) { CHECK(st.put(rec.data(), rec.size(), full_draw(), kLiveRoots, ed, cfg, idx) == nullptr && idx == i); }
    CHECK(std::strcmp(st.put(rec.data(), rec.size(), full_draw(), kLiveRoots, ed, cfg, idx), "smoke-replay-cap") == 0 && st.refused() == 1);
    CHECK(std::memcmp(st.eye(5, 0), o0.data(), kRecordBytes) == 0 && std::memcmp(st.eye(5, 1), o1.data(), kRecordBytes) == 0);
    CHECK(st.eye(ReplayStore::kCap, 0) == nullptr && st.eye(0, 2) == nullptr);
    for (unsigned i = 0; i < ReplayStore::kCap; ++i) for (unsigned e = 0; e < 2; ++e) CHECK(ReplayStore::offset(i, e) % 16 == 0);
    CHECK(ReplayStore::offset(ReplayStore::kCap - 1, 1) + kRecordStride == ReplayStore::kBytes && ReplayStore::kBytes == 204800);
    st.begin(); CHECK(st.count() == 0 && st.put(rec.data(), rec.size(), full_draw(), nullptr, ed, cfg, idx) != nullptr && st.count() == 0 && st.refused() == 1);
    // root swap
    RootSwap r; const uint64_t up = 0x10000, game = 0x7700;
    CHECK(plan_root_swap(3, true, game, up, ReplayStore::kBytes, 15, r) == nullptr);
    CHECK(r.parameter == 3 && r.restore_va == game && r.eye_va[0] == up + 30u * kRecordStride && r.eye_va[1] == up + 31u * kRecordStride);
    CHECK(std::strcmp(plan_root_swap(UINT32_MAX, true, game, up, ReplayStore::kBytes, 0, r), "smoke-t10-parameter") == 0 && r.parameter == UINT32_MAX);
    CHECK(std::strcmp(plan_root_swap(3, false, game, up, ReplayStore::kBytes, 0, r), "smoke-t10-not-root-srv") == 0);
    CHECK(std::strcmp(plan_root_swap(3, true, 0, up, ReplayStore::kBytes, 0, r), "smoke-t10-game-va") == 0);
    CHECK(std::strcmp(plan_root_swap(3, true, game, up + 4, ReplayStore::kBytes, 0, r), "smoke-upload-va") == 0);
    CHECK(std::strcmp(plan_root_swap(3, true, game, up, ReplayStore::kBytes, ReplayStore::kCap, r), "smoke-replay-cap") == 0);
    CHECK(std::strcmp(plan_root_swap(3, true, game, up, ReplayStore::kBytes - 1, ReplayStore::kCap - 1, r), "smoke-upload-span") == 0);
    CHECK(std::strcmp(plan_root_swap(3, true, game, UINT64_MAX - 15, ReplayStore::kBytes, 0, r), "smoke-upload-span") == 0);
    Cmd c[kMaxReplayCmds]; CHECK(replay_commands(RootSwap{}, true, c) == 0);
    // a fake recorder: the eye draws see their own copies, and afterwards the game's root argument, PSO and targets are back
    CHECK(plan_root_swap(3, true, game, up, ReplayStore::kBytes, 2, r) == nullptr);
    for (int dual = 0; dual < 2; ++dual) {
        const unsigned n = replay_commands(r, dual != 0, c);
        CHECK(n == (dual ? 10u : 8u));
        uint64_t root[8] = {}; root[3] = game; int pso = 0 /*game*/, om = -1 /*game targets*/; unsigned draws = 0;
        for (unsigned i = 0; i < n; ++i) {
            switch (c[i].kind) {
            case CmdKind::set_pso_clone: pso = 1; break;
            case CmdKind::set_pso_game: pso = 0; break;
            case CmdKind::om_eye: om = c[i].eye; break;
            case CmdKind::om_restore: om = -1; break;
            case CmdKind::root_srv: CHECK(c[i].parameter == 3); root[c[i].parameter] = c[i].va; break;
            case CmdKind::draw:
                CHECK(om == int(c[i].eye) && root[3] == r.eye_va[c[i].eye] && pso == dual); ++draws; break;
            }
        }
        CHECK(draws == 2 && root[3] == game && pso == 0 && om == -1);
    }
}

void test_dry_run() {
    const auto pts = arc_points();
    const auto rec = make_record(kGame, pts, nullptr, kLiveGain);
    const Cam eyes[2] = {{{kGame.e[0] - 0.03, kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0}, {{kGame.e[0] + 0.03, kGame.e[1], kGame.e[2]}, deg(-60.0), 0.0}};
    const Eyes E = make_eyes(rec.data(), eyes, kEyeTan);
    EyeDelta ed; ed.D[0] = E.D[0]; ed.D[1] = E.D[1]; ed.S = E.S; ed.world_ref64 = rec.data() + kOffA;
    static ReplayStore st; st.begin();
    ReplayDryRun dr; dr.begin(); unsigned idx = 0;
    ++dr.draws;
    CHECK(st.put(rec.data(), rec.size(), full_draw(), kLiveRoots, ed, ReplayCfg{}, idx) == nullptr); ++dr.prepared;
    count_eye_points(rec.data(), st.eye(idx, 0), st.eye(idx, 1), full_draw(), dr);
    // arc points 0..19 at 0..76 deg right; eyes look 60 deg right with a 100 deg field (50 deg half): points 3..19 (12..76 deg); the game shows 0..7
    CHECK(dr.points == 20 && dr.eye_visible[0] == dr.eye_visible[1] && dr.eye_visible[0] >= 14 && dr.eye_only == 12);
    dr.gain = lens_fit(rec.data()).gain; dr.depth_scale = 1.0f / 1024.0f;
    char line[512]; const int n = format_dry_run(line, sizeof line, 9, dr);
    const std::string l(line);
    CHECK(n > 0 && l.find("fx-smoke-replay-plan;frame=9;draws=1;prepared=1;refused=0;points=20;") == 0 && l.find(";eye-only=12;lens-gain=0.0019;depth-scale=0.0009766;world=ok;last-why=none;mode=dry-run(cpu-only,no-gpu-replay)") != std::string::npos);
    std::printf("  dry run: eye0 sees %u of %u trail points, %u of them outside the game window\n", dr.eye_visible[0], dr.points, dr.eye_only);
    // worst case under the feature log's line limit
    ReplayDryRun w; w.draws = w.prepared = w.refused = w.points = w.eye_visible[0] = w.eye_visible[1] = w.eye_only = 0xffffffffu; w.gain = 1e300; w.depth_scale = -1e30f;
    w.last_why = w.world_why = "a-very-long-reason-that-would-not-fit-the-line-at-all-xxxxxxxxxxxxxxxxxxxx";
    const int m = format_dry_run(line, sizeof line, ~0ull, w);
    CHECK(m > 0 && size_t(m) + kLinePrefix < kLineLimit);
}

// ================================================================== fx_smoke_gpu.h (fxmem:4: the GPU replay per eye; docs section 8.7)
namespace g = acz::fxsmoke::gpu;
void test_gpu_budget() {
    uint64_t c = 1;
    CHECK(g::budget(8, 2560, 1440, 3, c) == nullptr);
    const uint64_t img = g::align_up(2560ull * 1440 * 8, g::kAlign) + g::kAlign;
    CHECK(c == img * 2 * 3 + 3 * g::kSlotUpload && c < g::kTotalCap);   // ~178 MB at the live size (D-361 ring: 2560x1440 RGBA16F, 3 slots)
    CHECK(std::strcmp(g::budget(0, 2560, 1440, 3, c), "smoke-format-unsupported") == 0 && c == 0);
    CHECK(std::strcmp(g::budget(8, 0, 1440, 3, c), "smoke-size-unsupported") == 0);
    CHECK(std::strcmp(g::budget(8, 3840, 2160, 3, c), "smoke-over-image-cap") == 0);    // 66 MB per image
    CHECK(std::strcmp(g::budget(8, 2560, 1440, 4, c), "smoke-over-total-cap") == 0 && c == 0);
    CHECK(g::kSlotUpload >= ReplayStore::kBytes && g::kSlotUpload % 65536 == 0);
    // every eye copy of every slot's span stays inside that span and is 16-byte aligned
    for (unsigned s = 0; s < 3; ++s) for (unsigned i = 0; i < ReplayStore::kCap; ++i) for (unsigned e = 0; e < 2; ++e) {
        const uint64_t o = s * g::kSlotUpload + ReplayStore::offset(i, e);
        CHECK(o % 16 == 0 && o + kRecordBytes <= (s + 1) * g::kSlotUpload);
    }
}
g::GateIn good_gate() {
    g::GateIn in; in.ready = in.eye_depth = in.dual = in.effects_recorded = in.prepared = in.t10_root_srv = true;
    in.pso_dsv = in.eye_dsv = 20; in.pso_rtv0 = in.image_format = 10; in.pso_samples = 1; in.pso_rt_count = 1;
    in.depth_enable = true; in.depth_func = g::kCmpGreater; in.depth_write = false; in.stencil = true; in.stencil_noop = true;
    in.vp_known = true; in.viewports = 1; in.vp[2] = 2560.0f; in.vp[3] = 1440.0f; in.image_w = 2560; in.image_h = 1440;
    return in;
}
void test_gpu_gate() {
    CHECK(g::gate(good_gate()) == nullptr);
    { auto x = good_gate(); x.depth_func = g::kCmpGreaterEqual; CHECK(g::gate(x) == nullptr); }
    { auto x = good_gate(); x.stencil = false; x.stencil_noop = false; CHECK(g::gate(x) == nullptr); }
    auto why = [](void (*edit)(g::GateIn&)) { auto x = good_gate(); edit(x); const char* w = g::gate(x); return std::string(w ? w : "ok"); };
    CHECK(why([](g::GateIn& x) { x.ready = false; }) == "smoke-gpu-not-ready");
    CHECK(why([](g::GateIn& x) { x.effects_recorded = false; }) == "smoke-effects-not-recorded");
    CHECK(why([](g::GateIn& x) { x.dual = false; }) == "smoke-needs-effects-dual");
    CHECK(why([](g::GateIn& x) { x.prepared = false; }) == "smoke-not-prepared");
    CHECK(why([](g::GateIn& x) { x.t10_root_srv = false; }) == "smoke-t10-not-root-srv");
    CHECK(why([](g::GateIn& x) { x.eye_depth = false; }) == "smoke-eye-depth-missing");
    CHECK(why([](g::GateIn& x) { x.eye_dsv = 0; }) == "smoke-eye-depth-missing");
    CHECK(why([](g::GateIn& x) { x.pso_dsv = 45; }) == "smoke-dsv-format-differs");        // the open point: D24S8 smoke PSO vs D32S8X24 eye DSV
    CHECK(why([](g::GateIn& x) { x.pso_samples = 4; }) == "smoke-pso-multisample");
    CHECK(why([](g::GateIn& x) { x.pso_rtv0 = 24; }) == "smoke-rt-format-differs");
    CHECK(why([](g::GateIn& x) { x.pso_rt_count = 2; }) == "smoke-rt-format-differs");
    CHECK(why([](g::GateIn& x) { x.depth_enable = false; }) == "smoke-pso-depth-test");
    CHECK(why([](g::GateIn& x) { x.depth_func = 8; }) == "smoke-pso-depth-test");          // ALWAYS: would draw over nearer objects
    CHECK(why([](g::GateIn& x) { x.depth_write = true; }) == "smoke-pso-depth-write");
    CHECK(why([](g::GateIn& x) { x.stencil_noop = false; }) == "smoke-pso-stencil");
    CHECK(why([](g::GateIn& x) { x.vp_known = false; }) == "smoke-viewport-unknown");
    CHECK(why([](g::GateIn& x) { x.viewports = 2; }) == "smoke-viewport-unknown");
    CHECK(why([](g::GateIn& x) { x.vp[2] = 1280.0f; }) == "smoke-viewport-not-image");
    CHECK(why([](g::GateIn& x) { x.vp[0] = 4.0f; }) == "smoke-viewport-not-image");
}
void test_gpu_frame_state() {
    g::FrameState s; s.begin(false); CHECK(!s.on && !s.valid());
    s.begin(true); CHECK(s.on && !s.cleared && s.replays == 0 && !s.valid());
    // predicate words as the effects layer leaves them at the Close: epoch 0 committed (1), epoch 1 the tail (0)
    uint8_t words[64] = {}; const uint64_t one = 1; std::memcpy(words + 0, &one, 8);
    s.cleared = true; s.note_replay(true, 0); s.note_replay(true, 8); s.note_replay(true, 0);
    s.close(words, 0); CHECK(s.committed == 2 && s.valid());                               // eq0 (default): word 1 executes
    s.close(words, 1); CHECK(s.committed == 1 && s.valid());                               // ne0: word 0 executes
    s.close(words, 2); CHECK(s.committed == 3);                                            // predication off: all execute
    s.close(nullptr, 0); CHECK(s.committed == 0 && !s.valid());                           // no predicate memory: nothing provable
    g::FrameState t; t.begin(true); t.cleared = true; t.note_replay(true, 8); t.close(words, 0);
    CHECK(t.committed == 0 && !t.valid());                                                 // only tail smoke: cleared but empty -> not published
    g::FrameState u; u.begin(true); u.note_replay(false, 0); u.close(words, 0); CHECK(u.committed == 1 && !u.valid());   // never cleared -> never valid
    u.note_refused(nullptr); CHECK(u.refused == 1 && std::strcmp(u.last_why, "smoke-refused") == 0);
    g::FrameState v; v.begin(true); v.cleared = true;
    for (unsigned i = 0; i < g::FrameState::kCap + 3; ++i) v.note_replay(true, 0);
    v.close(words, 0); CHECK(v.replays == g::FrameState::kCap + 3 && v.committed == g::FrameState::kCap);   // bounded bookkeeping (the store caps replays at ReplayStore::kCap anyway)
}
// The recorded sequence: the per-draw command count, the predication bracket around every eye draw, and the game's state restored before its own draw.
void test_gpu_sequence() {
    RootSwap r; const uint64_t span = 0x40000 + 2 * g::kSlotUpload, game = 0x7700;
    CHECK(plan_root_swap(3, true, game, span, g::kSlotUpload, 15, r) == nullptr && r.eye_va[1] + kRecordBytes <= span + g::kSlotUpload);
    {   // (section 18) the last draw of the raised store (index kCap - 1 = 63) still fits this slot's span; index kCap is refused by name
        RootSwap r2; CHECK(plan_root_swap(3, true, game, span, g::kSlotUpload, ReplayStore::kCap - 1, r2) == nullptr && r2.eye_va[1] + kRecordBytes <= span + g::kSlotUpload);
        CHECK(std::strcmp(plan_root_swap(3, true, game, span, g::kSlotUpload, ReplayStore::kCap, r2), "smoke-replay-cap") == 0);
        CHECK(std::strcmp(plan_root_swap(3, true, game, span, 65536, ReplayStore::kCap - 1, r2), "smoke-upload-span") == 0);   // the old 64 KiB span is too small
    }
    Cmd c[kMaxReplayCmds]; const unsigned n = replay_commands(r, true, c);
    CHECK(n + 2 == g::kCmdsPerDraw);                                                        // + SetPredication(on) before, SetPredication(null) before the OM restore
    bool pred = false; int pso = 0, om = -1; uint64_t root = game; unsigned draws = 0, predOffBeforeRestore = 0;
    pred = true;                                                                            // SetPredication(word of the effects epoch)
    for (unsigned i = 0; i < n; ++i) {
        switch (c[i].kind) {
        case CmdKind::set_pso_clone: pso = 1; break;
        case CmdKind::set_pso_game: pso = 0; break;
        case CmdKind::om_eye: om = c[i].eye; break;
        case CmdKind::root_srv: root = c[i].va; break;
        case CmdKind::draw: CHECK(pred && pso == 1 && om == int(c[i].eye) && root == r.eye_va[c[i].eye]); ++draws; break;
        case CmdKind::om_restore: if (pred) { pred = false; ++predOffBeforeRestore; } om = -1; break;
        }
    }
    CHECK(draws == 2 && !pred && predOffBeforeRestore == 1 && pso == 0 && om == -1 && root == game);
}
// The core of headset test 19's "vehicles render through the smoke": the smoke puff depth an eye copy writes is the SAME depth model the replayed objects use
// in that eye (objects' depth scale), so the D3D12 GREATER test against the eye objects depth keeps exactly the puffs in front of an object; composited over
// the objects, a puff in front covers the object and a puff behind it leaves the object untouched. The old path (window effects image under the eye image)
// shows the object over the smoke whatever the depths.
void test_gpu_depth_order_and_composite() {
    const auto pts = arc_points();
    auto rec = make_record(kGame, pts, nullptr, kLiveGain);
    for (unsigned i = 0; i < 8; ++i) wf(rec.data(), kOffCurveBVals + 4 * i, float(kRadius / kLiveGain));
    const Cam eyes[2] = {{{kGame.e[0] - 0.03, kGame.e[1], kGame.e[2]}, deg(-30.0), 0.0}, {{kGame.e[0] + 0.03, kGame.e[1], kGame.e[2]}, deg(-30.0), 0.0}};
    const Eyes E = make_eyes(rec.data(), eyes, kEyeTan);
    EyeDelta ed; ed.D[0] = E.D[0]; ed.D[1] = E.D[1]; ed.S = E.S; ed.world_ref64 = rec.data() + kOffA;
    const double scale = 1.0 / 1024.0;                       // xr_world_objects_depth_fix (live: depth-fix-frames > 0 in test 19)
    ReplayCfg cfg; cfg.depth_scale = float(scale);
    std::vector<uint8_t> o0(kRecordBytes), o1(kRecordBytes);
    CHECK(prepare_eye_records(rec.data(), rec.size(), full_draw(), kLiveRoots, ed, cfg, o0.data(), o1.data()) == nullptr);
    const RandomTable rnd = const_table(); const ScreenConsts sc;
    double ae[16]; load_m(o0.data(), kOffA, ae);
    std::vector<Puff> out; emulate_draw(o0.data(), rnd, sc, full_draw(), out);
    unsigned hidden = 0, shown = 0, checked = 0;
    const float smokeTexel[4] = {0.3f, 0.3f, 0.3f, 0.6f};    // coverage_v1: premultiplied P, coverage A
    const g::Px object{0.2f, 0.3f, 0.4f, 1.0f};
    for (const Puff& p : out) {
        if (p.u != 0.0) continue;
        double x, y, w; CHECK(proj(ae, pts[p.segment], x, y, w));
        const double ds = double(p.sv[0][2]);
        CHECK(std::fabs(ds - g::eye_depth(w, scale)) <= 1e-5 * g::eye_depth(w, scale));   // one depth model for puffs and objects
        CHECK(ds > 0.0 && ds < double(g::kObjectBand));                                     // the smoke lands in the object band, below the cockpit band
        const double front = g::eye_depth(0.5 * w, scale), back = g::eye_depth(2.0 * w, scale);
        const bool passFront = g::depth_passes(ds, front, g::kCmpGreater), passBack = g::depth_passes(ds, back, g::kCmpGreater);
        CHECK(!passFront && passBack);
        CHECK(g::depth_passes(ds, 0.0, g::kCmpGreater));                                     // nothing replayed at the pixel (cleared 0): drawn
        // composite: the GPU wrote the texel only where the test passed
        const float none[4] = {0, 0, 0, 0};
        const g::Px sBack = g::smoke_layer(passBack ? smokeTexel : none, 1.0f, 0, 1.0f), sFront = g::smoke_layer(passFront ? smokeTexel : none, 1.0f, 0, 1.0f);
        const g::Px atBack = g::smoke_over(object, sBack, float(back), false);              // object BEHIND the puff: smoke over it
        const g::Px atFront = g::smoke_over(object, sFront, float(front), false);           // object IN FRONT of the puff: untouched
        CHECK(std::fabs(atBack.r - (0.3f + 0.4f * 0.2f)) < 1e-6f && std::fabs(atBack.a - 1.0f) < 1e-6f);
        CHECK(atFront.r == object.r && atFront.g == object.g && atFront.b == object.b && atFront.a == object.a);
        // the pre-fix composite: effects (here the smoke) are part of the world layer UNDER the eye image; an opaque object pixel always wins
        const g::Px oldPath = g::smoke_over(sBack, object, 0.0f, true);                      // object (a = 1) over the smoke layer
        CHECK(oldPath.r == object.r && oldPath.g == object.g && oldPath.b == object.b);
        if (passBack) ++shown;
        if (!passFront) ++hidden;
        ++checked;
    }
    CHECK(checked >= 10 && shown == checked && hidden == checked);
    std::printf("  depth order: %u point puffs, each hidden by a nearer object and covering a farther one (same eye depth model, depth x 2^-10)\n", checked);
    // the cockpit band and the model cockpit; the sky (nothing replayed); the decouple edge fade; exposure / tonemap like the effects mode 4
    const g::Px s = g::smoke_layer(smokeTexel, 1.0f, 0, 1.0f);
    CHECK(std::fabs(s.r - 0.3f) < 1e-6f && std::fabs(s.a - 0.6f) < 1e-6f);
    const g::Px cockpit{0.1f, 0.1f, 0.1f, 1.0f}, sky{0.5f, 0.6f, 0.7f, 1.0f};
    { const g::Px r = g::smoke_over(cockpit, s, 0.5f, false); CHECK(r.r == cockpit.r && r.a == cockpit.a); }   // replayed cockpit band kept
    { const g::Px r = g::smoke_over(sky, s, 0.5f, true); CHECK(std::fabs(r.r - (0.3f + 0.4f * 0.5f)) < 1e-6f); }   // model cockpit: band transparent, smoke applies
    { const g::Px r = g::smoke_over(sky, s, 0.0f, false); CHECK(std::fabs(r.g - (0.3f + 0.4f * 0.6f)) < 1e-6f); }  // the sky
    { const g::Px f = g::smoke_layer(smokeTexel, 1.0f, 0, 0.5f); CHECK(std::fabs(f.a - 0.3f) < 1e-6f && std::fabs(f.r - 0.15f) < 1e-6f); }
    { const float hot[4] = {1.2f, 0.6f, 0.0f, 0.6f}; const g::Px h = g::smoke_layer(hot, 1.0f, 1, 1.0f);
      CHECK(std::fabs(h.r - 0.6f * (2.0f / 3.0f)) < 1e-6f && std::fabs(h.g - 0.6f * 0.5f) < 1e-6f && h.b == 0.0f); }   // Reinhard on P / A
    { const float neg[4] = {0.5f, 0.5f, 0.5f, -1.0f}; const g::Px z = g::smoke_layer(neg, 1.0f, 0, 1.0f); CHECK(z.r == 0.0f && z.a == 0.0f); }
}
void test_gpu_lines() {
    char line[600];
    g::FrameState s; s.begin(true); s.cleared = true; s.note_replay(true, 0); s.committed = 1;
    int n = g::format_frame(line, sizeof line, 42, s, 0);
    CHECK(n > 0 && std::string(line) == "fx-smoke-gpu;frame=42;replays=1;committed=1;refused=0;window-fallback=0;cleared=1;valid=1;last-why=none;"
        "mode=gpu-replay(per-eye,eye-objects-depth-test,effects-predicate,window-smoke-suppressed)");
    g::FrameState w; w.begin(true); w.replays = w.committed = w.refused = 0xffffffffu; w.last_why = "a-very-long-reason-that-would-not-fit-the-line-at-all-xxxxxxxxxxxxxxxxxxxx";
    n = g::format_frame(line, sizeof line, ~0ull, w, 0xffffffffu);
    CHECK(n > 0 && size_t(n) + kLinePrefix < kLineLimit);
    n = g::format_ring(line, sizeof line, true, nullptr, 3, 10, 2560, 1440, 186908672ull);
    CHECK(n > 0 && size_t(n) + kLinePrefix < kLineLimit && std::string(line).find("fx-smoke-ring;outcome=ready;reason=none;slots=3;format=10;size=2560x1440;") == 0);
    n = g::format_ring(line, sizeof line, false, "a-very-long-reason-that-would-not-fit-the-line-at-all-xxxxxxxxxxxxxxxxxxxx", 99, ~0u, ~0ull, ~0u, ~0ull);
    CHECK(n > 0 && size_t(n) + kLinePrefix < kLineLimit);
}
} // namespace

// HUD-only / chase depth model (headset test 7, effects-outside-window.md 22.3): the smoke record's A of the HUD-only view (logged verbatim,
// pov1\runPOV3\logs\ee-camera.log 204084889.040) has b = -1341.44, Z0 = 8.80e7. The old depth_column_ok (fixed b = -255.0078) refused it:
// `smoke-a-depth-column` / `smoke-subst-depth-column` for the whole view.
void test_hudonly_depth_model() {
    const double A[16] = {-1036.37085,-1319.10754,884.882751,-0.659652293,-147.345123,-798.73999,96.8959885,-0.072232917,-1809.55432,-1497.34668,1003.51904,-0.748091936,219116288,229474528,-57887428,108737.133};
    double bOld = 0, mag = 0;   // the old rule: z - kDepthB w on rows 0..2
    for (int r = 0; r < 3; ++r) { const double d = A[r * 4 + 2] - kDepthB * A[r * 4 + 3]; bOld += d * d; mag += A[r * 4 + 2] * A[r * 4 + 2]; }
    CHECK(std::sqrt(bOld) > 1e-4 * std::sqrt(mag));   // failing-before: the fixed-b rule refuses this A
    double b = 0, z0 = 0;
    CHECK(depth_column_ok(A, &b, &z0) && std::fabs(b + 1341.44) < 0.1 && std::fabs(z0 - 87976709.0) < 2000.0);
    const double cockpit[16] = {-1357.64526,-1448.15491,180.317764,-0.707106888,3.58666803e-05,-127.999977,-4.46595823e-06,1.75130275e-08,-1538.66455,-1448.15491,180.317764,-0.707106888,100844.141,103620.5,16764887,49.3459473};
    CHECK(depth_column_ok(cockpit, &b, &z0) && std::fabs(b + 255.01) < 0.01);
    double bad[16]; std::memcpy(bad, A, sizeof bad); bad[14] += 5.0e7; CHECK(!depth_column_ok(bad));   // not the PS2 depth model
    bad[2] += 50.0; bad[14] = A[14]; CHECK(!depth_column_ok(bad));                                    // the depth column is not b x w
    // a record carrying that A is replayable, and substitute() accepts it as an eye matrix
    auto rec = make_record(kGame, arc_points());
    for (unsigned i = 0; i < 16; ++i) wf(rec.data(), kOffA + 4 * i, float(A[i]));
    CHECK(record_check(rec.data(), rec.size(), full_draw()) == nullptr);
    std::vector<uint8_t> sub(kRecordBytes); SubstCfg cfg;
    CHECK(substitute(rec.data(), A, cfg, sub.data()) == nullptr);
    // one depth model for clip and eye depth: a world point's clip z / w equals 2 x eye_depth x 2^24 - 1 with the view's own (b, Z0)
    const double P[3] = {1000.0, 2000.0, 3000.0}; double h[4], c[16];
    for (int k = 0; k < 4; ++k) h[k] = P[0] * A[k] + P[1] * A[4 + k] + P[2] * A[8 + k] + A[12 + k];
    clip_from_a(A, 1.0, c);
    double cz = 0, cw = 0;
    cz = P[0] * c[2] + P[1] * c[6] + P[2] * c[10] + c[14]; cw = P[0] * c[3] + P[1] * c[7] + P[2] * c[11] + c[15];
    double bm = 0, zm = 0; CHECK(depth_column_ok(A, &bm, &zm));
    const double ed = g::eye_depth(h[3], 1.0, bm, zm);
    CHECK(std::fabs(cz / cw - (2.0 * ed - 1.0)) < 1e-9);
    CHECK(std::fabs(ed - (zm / h[3] + bm) * kDepthUnit) < 1e-15);
    CHECK(std::fabs(g::eye_depth(h[3], 1.0, bm, zm) - g::eye_depth(h[3], 1.0)) > 1e-3 * ed);   // the cockpit constants would give another depth
    std::printf("  hud-only smoke depth model: b %.2f Z0 %.0f accepted\n", bm, zm);
}
int main() {
    test_curve_and_layout();
    test_game_view_only_inside_window();
    test_eye_view_outside_window();
    test_identity_bytes();
    test_depth_scale();
    test_lens_rescale();
    test_trail_behind();
    test_record_check();
    test_hudonly_depth_model();
    test_capture_and_summary();
    test_live_capture_path();
    test_line_limit();
    test_log_gate();
    test_root_constants();
    test_screen_consts();
    test_lens_gain_live_form();
    test_eye_a_from_delta();
    test_prepare_eye_records();
    test_replay_store_and_root_swap();
    test_dry_run();
    test_gpu_budget();
    test_gpu_gate();
    test_gpu_frame_state();
    test_gpu_sequence();
    test_gpu_depth_order_and_composite();
    test_gpu_lines();
    std::printf("fx_smoke_offline: %u checks passed\n", g_checks);
    return 0;
}
