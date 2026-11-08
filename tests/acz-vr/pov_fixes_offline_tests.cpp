// POV fixes offline fixtures (docs/review/pov-fixes-impl.md): F-REF (orientation clusters + majority look reference + diagnostic line),
// F-LEAN (setup_poses neck-pivot compensation and the layer shift maths), F-HUD rate limiter, config keys. Nothing here touches the game,
// the GPU or XR; the consumer-side layer placement lives in xr_stereo_output_offline_tests --pov-fixes.
#include "gpu_copy_step5.h"
#include "look_drive_keys.h"
#include "pov_fixes.h"
#include "runtime_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace acz::look_drive;
namespace step5 = acz::gpu_copy::step5;
namespace pov = acz::pov;

static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static const double kDeg = kPi / 180.0;

// ------------------------------------------------------------------ rotation helpers (row-major 3x3, v' = R v)
static void rot_y(double a, double R[9]) { const double c = std::cos(a), s = std::sin(a); const double m[9] = {c, 0, s, 0, 1, 0, -s, 0, c}; std::memcpy(R, m, sizeof m); }
static void rot_x(double a, double R[9]) { const double c = std::cos(a), s = std::sin(a); const double m[9] = {1, 0, 0, 0, c, -s, 0, s, c}; std::memcpy(R, m, sizeof m); }
static void mul33(const double* A, const double* B, double* O) {
    double t[9];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { double s = 0; for (int k = 0; k < 3; ++k) s += A[i * 3 + k] * B[k * 3 + j]; t[i * 3 + j] = s; }
    std::memcpy(O, t, sizeof t);
}
static void quat_of(double yaw, double pitch, float q[4]) {
    const double qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, qx[4] = {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    double o[4]; quat_mul(qy, qx, o);
    for (int i = 0; i < 4; ++i) q[i] = float(o[i]);
}

// ================================================================== F-LEAN: setup_poses
struct Head { double pivot[3]; double lever; double ipd_half; };
// Eye poses of a head that rotates about `pivot` (yaw, pitch; R = Ry Rx) with the eye midpoint at pivot + R n, n = (0, 0.7 f, -f); `lean` shifts the pivot.
static void head_poses(const Head& h, double yaw, double pitch, const double lean[3], step5::EyePose out[2]) {
    double R[9], Ry[9], Rx[9]; rot_y(yaw, Ry); rot_x(pitch, Rx); mul33(Ry, Rx, R);
    const double n[3] = {0.0, 0.7 * h.lever, -h.lever};
    double mid[3];
    for (int i = 0; i < 3; ++i) mid[i] = h.pivot[i] + lean[i] + R[i * 3] * n[0] + R[i * 3 + 1] * n[1] + R[i * 3 + 2] * n[2];
    for (int e = 0; e < 2; ++e) {
        const double sg = e ? 1.0 : -1.0;
        for (int i = 0; i < 3; ++i) out[e].position[i] = float(mid[i] + sg * h.ipd_half * R[i * 3]);   // R (1,0,0) = first column
        quat_of(yaw, pitch, out[e].orientation);
        for (int i = 0; i < 4; ++i) out[e].fov[i] = (i == 0 || i == 3) ? -0.6f : 0.6f;
    }
}
static void fixture_camera(step5::PoseFrame& f) {
    f.cam = step5::Camera{};
    f.cam.right[0] = 1; f.cam.up[1] = 1; f.cam.fwd[2] = -1;
    f.cam.tanX = 0.6152; f.cam.tanY = 0.3460; f.cam.n0 = 1; f.cam.n1 = 1; f.cam.n3 = 1; f.cam.A = 1; f.cam.C = 0;
}
// The eye midpoint offset from the game camera eye, in METRES of head motion (eyeWorld = cam.eye + s * d).
static void centre_offset_m(const step5::PoseFrame& f, double s, double out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = (0.5 * (f.eyeWorld[0][k] + f.eyeWorld[1][k]) - f.cam.eye[k]) / s;
}
static double norm3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void lean_tests() {
    const Head head{{0.1, 1.2, -0.3}, 0.132, 0.032};   // test 14 fit: f = 0.132
    const double s = 39.6, none[3] = {0, 0, 0};
    const double yaw0 = -0.9;                           // about -51.5 deg like the test-14 neutral
    struct Case { double dyaw, pitch; };
    const Case cases[] = {{0.0, 0.0}, {0.5, 0.0}, {70 * kDeg, 0.0}, {-70 * kDeg, 0.0}, {1.2, 0.0}, {0.3, 40 * kDeg}, {-0.4, -40 * kDeg}, {70 * kDeg, 25 * kDeg}};
    // Neutral (first valid pose): yaw0 and pitch 0.
    step5::EyePose first[2]; head_poses(head, yaw0, 0.0, none, first);
    auto run = [&](const pov::Lean* lean, double dyaw, double pitch, const double* lean_m, double out[3], double* ipd) {
        step5::Neutral n; step5::PoseFrame f; step5::Scratch sc; fixture_camera(f);
        CHECK(step5::setup_poses(first, n, f, sc, false, s, nullptr, lean) == nullptr);                    // fixes p0 / yaw0
        step5::EyePose cur[2]; head_poses(head, yaw0 + dyaw, pitch, lean_m, cur);
        CHECK(step5::setup_poses(cur, n, f, sc, false, s, nullptr, lean) == nullptr);
        centre_offset_m(f, s, out);
        if (ipd) { double d[3]; for (int k = 0; k < 3; ++k) d[k] = (f.eyeWorld[1][k] - f.eyeWorld[0][k]) / s; *ipd = norm3(d); }
    };
    const pov::Lean exact = pov::make_lean(0.132, 1.0);
    for (const auto& c : cases) {
        double off[3], offLean[3];
        run(nullptr, c.dyaw, c.pitch, none, off, nullptr);
        run(&exact, c.dyaw, c.pitch, none, offLean, nullptr);
        // Pure rotation with the true lever: the submitted eye offset collapses to < 1 mm (the doc: 0.12 m lateral swing at 70 deg).
        CHECK(norm3(offLean) < 1e-3);
        if (std::fabs(c.dyaw) >= 69 * kDeg && c.pitch == 0.0) CHECK(norm3(off) > 0.10);                      // the swing without the fix (f sin 70 = 0.124 m)
        // 30 % lever error leaves about 30 % of the arc, 9 % (0.12 vs 0.132, the doc's suggested default) leaves < 3 cm at 70 deg.
        const pov::Lean low = pov::make_lean(0.0924, 1.0), dflt = pov::make_lean(0.12, 1.0);
        double a[3], b[3]; run(&low, c.dyaw, c.pitch, none, a, nullptr); run(&dflt, c.dyaw, c.pitch, none, b, nullptr);
        if (norm3(off) > 0.05) { const double r = norm3(a) / norm3(off); CHECK(r > 0.2 && r < 0.4); }
        CHECK(norm3(b) < 0.03);
    }
    // Real leaning is preserved: the pivot moves 0.10 m (+x) and 0.05 m up, the head also turned 70 deg; the recentred offset is the lean itself.
    {
        const double lean_m[3] = {0.10, 0.05, 0.0};
        step5::Neutral n; step5::PoseFrame f; step5::Scratch sc; fixture_camera(f);
        step5::setup_poses(first, n, f, sc, false, s, nullptr, &exact);
        step5::EyePose cur[2]; head_poses(head, yaw0 + 70 * kDeg, 0.0, lean_m, cur);
        step5::setup_poses(cur, n, f, sc, false, s, nullptr, &exact);
        double off[3]; centre_offset_m(f, s, off);
        const double c = std::cos(-yaw0), sv = std::sin(-yaw0);   // recentred: Ry(-yaw0) lean
        const double want[3] = {c * lean_m[0] + sv * lean_m[2], lean_m[1], -sv * lean_m[0] + c * lean_m[2]};
        // eyeWorld = eye + s * (d.x * right + d.y * up - d.z * fwd) and fwd = (0,0,-1): the offset equals d.
        CHECK(std::fabs(off[0] - want[0]) < 1e-3 && std::fabs(off[1] - want[1]) < 1e-3 && std::fabs(off[2] - want[2]) < 1e-3);
        // scale 0.5 halves the remaining translation; scale 0 removes it; the IPD is never scaled.
        for (double scale : {0.5, 0.0}) {
            const pov::Lean sl = pov::make_lean(0.132, scale);
            double o2[3], ipd = 0; run(&sl, 70 * kDeg, 0.0, lean_m, o2, &ipd);
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(o2[k] - scale * want[k]) < 1e-3);
            CHECK(std::fabs(ipd - 2 * head.ipd_half) < 1e-4);
        }
    }
    // Rotation-only (lever 0, scale 0): a pure rotation still moves nothing, a lean moves nothing, the IPD stays.
    {
        const pov::Lean ro = pov::make_lean(0.0, 0.0);
        CHECK(ro.active());
        const double lean_m[3] = {0.2, 0.0, -0.1};
        double o[3], ipd = 0; run(&ro, 1.0, 0.3, lean_m, o, &ipd);
        CHECK(norm3(o) < 1e-5 && std::fabs(ipd - 2 * head.ipd_half) < 1e-4);
    }
    // Keys off: nullptr and the inactive Lean{} are bit-identical to the original expression (no lean argument at all).
    {
        const pov::Lean off{};
        for (const auto& c : cases) {
            step5::Neutral n1, n2; step5::PoseFrame f1, f2; step5::Scratch s1, s2; fixture_camera(f1); fixture_camera(f2);
            step5::EyePose cur[2]; head_poses(head, yaw0 + c.dyaw, c.pitch, none, cur);
            step5::setup_poses(first, n1, f1, s1, false, s); step5::setup_poses(first, n2, f2, s2, false, s, nullptr, &off);
            step5::setup_poses(cur, n1, f1, s1, false, s); step5::setup_poses(cur, n2, f2, s2, false, s, nullptr, &off);
            CHECK(std::memcmp(f1.D, f2.D, sizeof f1.D) == 0 && std::memcmp(f1.eyeWorld, f2.eyeWorld, sizeof f1.eyeWorld) == 0);
        }
    }
    // Layer shift maths: for a pure rotation with the true lever, shift = m - p0 (the layers follow the real eye, the virtual eye stays at the neutral).
    {
        for (const auto& c : cases) {
            step5::EyePose cur[2]; head_poses(head, yaw0 + c.dyaw, c.pitch, none, cur);
            double m0[3], mc[3];
            for (int k = 0; k < 3; ++k) { m0[k] = 0.5 * (double(first[0].position[k]) + double(first[1].position[k])); mc[k] = 0.5 * (double(cur[0].position[k]) + double(cur[1].position[k])); }
            float p0[3] = {float(m0[0]), float(m0[1]), float(m0[2])};
            double sh[3];
            const bool ok = pov::frame_layer_shift(exact, cur[0].position, cur[1].position, cur[0].orientation, p0, float(yaw0), sh);
            CHECK(ok || norm3(sh) == 0.0);
            for (int k = 0; k < 3; ++k) CHECK(std::fabs(sh[k] - (mc[k] - m0[k])) < 2e-4);
        }
        double sh[3]; const float p[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
        CHECK(!pov::frame_layer_shift(pov::Lean{}, p, p, q, p, 0.0f, sh) && sh[0] == 0.0 && sh[1] == 0.0 && sh[2] == 0.0);
        const float bad[4] = {0, 0, 0, 0};
        CHECK(!pov::frame_layer_shift(exact, p, p, bad, p, 0.0f, sh));
    }
}

// ================================================================== F-HUD rate limiter
static void hud_follow_tests() {
    pov::HudFollow s;
    pov::hud_follow_step(s, 0.6, 0.2, 1000);                 // first call starts at alpha * look: no start-up sweep
    CHECK(std::fabs(s.yaw - 0.3) < 1e-12 && std::fabs(s.pitch - 0.1) < 1e-12);
    pov::HudFollow t;
    pov::hud_follow_step(t, 0.0, 0.0, 0);
    double prev = 0;
    for (int i = 1; i <= 40; ++i) {
        pov::hud_follow_step(t, 1.0, -0.4, i * 25);        // 25 ms frames
        CHECK(t.yaw - prev <= pov::kHudFollowRateRadS * 0.025 + 1e-9);
        CHECK(t.yaw >= prev - 1e-12);
        prev = t.yaw;
    }
    CHECK(std::fabs(t.yaw - 0.5) < 1e-9 && std::fabs(t.pitch + 0.2) < 1e-9);   // converged to alpha * look
    pov::HudFollow u; pov::hud_follow_step(u, 0.0, 0.0, 0); pov::hud_follow_step(u, 2.0, 0.0, 10000);   // a 10 s gap: dt capped at 0.25 s
    CHECK(std::fabs(u.yaw - pov::kHudFollowRateRadS * 0.25) < 1e-9);
    pov::HudFollow v; pov::hud_follow_step(v, 0.0, 0.0, 0); pov::hud_follow_step(v, std::nan(""), std::numeric_limits<double>::infinity(), 40);
    CHECK(std::isfinite(v.yaw) && std::isfinite(v.pitch));
}

// ================================================================== F-REF: clusters and the majority reference
// Aircraft axes: the images of the local axes under the aircraft rotation A = Ry(theta) Rx(ap) (rows of the row-vector M256).
struct V3 { double x, y, z; };
static void aircraft_rows(double theta, double ap, V3 e[3]) {
    double Ry[9], Rx[9], A[9]; rot_y(theta, Ry); rot_x(ap, Rx); mul33(Ry, Rx, A);
    for (int j = 0; j < 3; ++j) e[j] = {A[0 * 3 + j], A[1 * 3 + j], A[2 * 3 + j]};
}
// A member whose local frame is rotated by Q (row-major) relative to the aircraft: image of its local axis i is sum_j Q[j][i] e_air[j]; mirror flips its x.
static void member_m256(const V3 air[3], const double Q[9], bool mirror, double m[16]) {
    std::memset(m, 0, 16 * sizeof(double));
    for (int i = 0; i < 3; ++i) {
        V3 r{0, 0, 0};
        for (int j = 0; j < 3; ++j) { r.x += Q[j * 3 + i] * air[j].x; r.y += Q[j * 3 + i] * air[j].y; r.z += Q[j * 3 + i] * air[j].z; }
        const double sg = (mirror && i == 0) ? -16.0 : 16.0;      // scale 16 like the logged matrix
        m[i * 4 + 0] = sg * r.x; m[i * 4 + 1] = sg * r.y; m[i * 4 + 2] = sg * r.z;
    }
    m[15] = 1;
}
static void camera_for(const V3 air[3], double yaw, double pitch, double right[3], double up[3], double fwd[3]) {
    double R[9]; look_matrix(yaw, pitch, R);
    V3 a[3];
    for (int j = 0; j < 3; ++j) a[j] = {air[0].x * R[0 * 3 + j] + air[1].x * R[1 * 3 + j] + air[2].x * R[2 * 3 + j],
                                        air[0].y * R[0 * 3 + j] + air[1].y * R[1 * 3 + j] + air[2].y * R[2 * 3 + j],
                                        air[0].z * R[0 * 3 + j] + air[1].z * R[1 * 3 + j] + air[2].z * R[2 * 3 + j]};
    right[0] = a[0].x; right[1] = a[0].y; right[2] = a[0].z;
    up[0] = a[1].x; up[1] = a[1].y; up[2] = a[1].z;
    fwd[0] = -a[2].x; fwd[1] = -a[2].y; fwd[2] = -a[2].z;     // the camera looks along -z of its basis
}

struct Fleet { std::vector<std::vector<double>> m; };
static Fleet make_fleet(const V3 air[3], unsigned seed, int nAir, int nPitch10, int nYaw20, int nMirror) {
    // Members: aircraft-aligned, pitched 10.1 deg, yawed -19.7 deg, and mirrored aircraft-aligned (a reflection of the same orientation).
    Fleet f;
    double Q0[9], Qp[9], Qy[9];
    rot_y(0, Q0); rot_x(10.1 * kDeg, Qp); rot_y(-19.7 * kDeg, Qy);
    auto push = [&](const double* Q, bool mirror, int n) { for (int i = 0; i < n; ++i) { std::vector<double> v(16); member_m256(air, Q, mirror, v.data()); f.m.push_back(v); } };
    push(Q0, false, nAir); push(Qp, false, nPitch10); push(Qy, false, nYaw20); push(Q0, true, nMirror);
    unsigned r = seed * 2654435761u + 12345u;
    for (size_t i = f.m.size(); i > 1; --i) { r = r * 1664525u + 1013904223u; std::swap(f.m[i - 1], f.m[(r >> 8) % i]); }
    return f;
}

static void drive_frames(Driver& d, const Fleet& fl, const V3 air[3], const double right[3], const double up[3], const double fwd[3], unsigned frames,
                         bool first_is_offset, double out_yaw[], double out_pitch[]) {
    double Q0[9]; rot_y(0, Q0); double mair[16]; member_m256(air, Q0, false, mair);
    double Eair[9]; RefClusters::unit_rows(mair, Eair);
    for (unsigned k = 0; k < frames; ++k) {
        std::vector<std::vector<double>> order = fl.m;
        // Optionally bring a member that is NOT in the aircraft cluster to the front: the test-14 failure mode.
        if (first_is_offset) {
            for (size_t i = 0; i < order.size(); ++i) {
                double E[9]; RefClusters::unit_rows(order[i].data(), E);
                if (rotation_angle(Eair, E) > 0.1) { std::swap(order[0], order[i]); break; }
            }
        }
        const double now = 1000.0 + 16.0 * k;
        d.note_camera(right, up, fwd, order[0].data(), now);
        out_yaw[k] = d.raw_measured().yaw; out_pitch[k] = d.raw_measured().pitch;
        for (const auto& m : order) d.ref_member(77 + k, m.data());
        d.ref_close(77 + k, now);
    }
}

static void ref_cluster_tests() {
    V3 air[3]; aircraft_rows(0.7, 0.2, air);
    double right[3], up[3], fwd[3]; camera_for(air, 30 * kDeg, 5 * kDeg, right, up, fwd);
    // Cluster bookkeeping: counts, mirrored folding, ordering independence.
    for (unsigned seed = 1; seed <= 6; ++seed) {
        const Fleet fl = make_fleet(air, seed, 12, 5, 4, 3);
        RefClusters rc; rc.reset(1);
        for (const auto& m : fl.m) rc.add(m.data());
        CHECK(rc.members == 24 && rc.n == 3 && rc.rejected == 0);
        const int maj = rc.majority();
        CHECK(maj >= 0 && rc.c[maj].count == 15);                      // 12 aircraft + 3 mirrored (folded into the same cluster)
        unsigned total = 0; for (unsigned i = 0; i < rc.n; ++i) total += rc.c[i].count;
        CHECK(total == 24);
    }
    // Ambiguity / minimum size / bad input.
    {
        RefClusters rc; rc.reset(2);
        const Fleet tie = make_fleet(air, 3, 6, 6, 0, 0);
        for (const auto& m : tie.m) rc.add(m.data());
        CHECK(rc.n == 2 && rc.majority() < 0);                          // 6 vs 6: ambiguous
        RefClusters few; few.reset(3);
        const Fleet small = make_fleet(air, 3, 2, 1, 0, 0);
        for (const auto& m : small.m) few.add(m.data());
        CHECK(few.majority() < 0);                                      // largest cluster has 2 < 3 members
        RefClusters bad; bad.reset(4);
        double nan16[16]{}; nan16[0] = std::nan(""); double zero16[16]{}; double skew[16]{}; skew[0] = 1; skew[4] = 1; skew[10] = 1; skew[15] = 1;
        CHECK(bad.add(nan16) < 0 && bad.add(zero16) < 0 && bad.add(skew) < 0 && bad.rejected == 3 && bad.members == 0 && bad.majority() < 0);
        RefClusters ovf; ovf.reset(5);
        for (unsigned i = 0; i < RefClusters::kMax + 3; ++i) {
            double Q[9]; rot_y(0.1 * double(i + 1), Q); double m[16]; member_m256(air, Q, false, m); ovf.add(m);
        }
        CHECK(ovf.n == RefClusters::kMax && ovf.overflow == 3);
    }
    // The look reference. Key off: the first member (the polluted measurement); on: the previous frame's majority cluster.
    for (unsigned seed = 1; seed <= 6; ++seed) for (int offsetFirst = 0; offsetFirst < 2; ++offsetFirst) {
        const Fleet fl = make_fleet(air, seed, 12, 5, 4, 3);
        double yo[8], po[8], yn[8], pn[8];
        Driver off; off.configure(false, false, true, false, false);
        Driver on; on.configure(false, false, true, false, true);
        drive_frames(off, fl, air, right, up, fwd, 8, offsetFirst != 0, yo, po);
        drive_frames(on, fl, air, right, up, fwd, 8, offsetFirst != 0, yn, pn);
        for (int k = 1; k < 8; ++k) {
            // ON: exactly the true look (30, 5 deg) from the second frame on, whatever member comes first.
            CHECK(std::fabs(yn[k] - 30 * kDeg) < 0.1 * kDeg && std::fabs(pn[k] - 5 * kDeg) < 0.1 * kDeg);
        }
        if (offsetFirst) {
            // OFF with an offset first member: the measured look is polluted by that member's orientation (several degrees).
            bool polluted = false; for (int k = 0; k < 8; ++k) polluted = polluted || std::fabs(yo[k] - 30 * kDeg) > 2 * kDeg || std::fabs(po[k] - 5 * kDeg) > 2 * kDeg;
            CHECK(polluted);
        }
        CHECK(on.ref_majority_valid() && on.ref_used_frames() == 7 && on.ref_fallbacks() == 1);   // the first frame has no previous majority
        CHECK(!off.ref_cluster() && off.ref_used_frames() == 0 && off.ref_fallbacks() == 0);
    }
    // Key off is exactly measure_look() of the first member (bitwise), whatever the cluster bookkeeping saw.
    {
        const Fleet fl = make_fleet(air, 2, 12, 5, 4, 3);
        Driver d; d.configure(false, false, true, false, false);
        d.note_camera(right, up, fwd, fl.m[0].data(), 1000.0);
        LookMeasure ref; measure_look(right, up, fwd, fl.m[0].data(), ref);
        CHECK(ref.valid == d.raw_measured().valid && std::memcmp(ref.R, d.raw_measured().R, sizeof ref.R) == 0 && ref.yaw == d.raw_measured().yaw);
    }
    // Stale majority (> 250 ms): the first member again.
    {
        const Fleet fl = make_fleet(air, 4, 12, 5, 4, 3);
        Driver d; d.configure(false, false, true, false, true);
        d.note_camera(right, up, fwd, fl.m[0].data(), 1000.0);
        for (const auto& m : fl.m) d.ref_member(1, m.data());
        d.ref_close(1, 1000.0);
        d.note_camera(right, up, fwd, fl.m[0].data(), 1100.0);
        CHECK(d.ref_used_frames() == 1);
        d.note_camera(right, up, fwd, fl.m[0].data(), 1500.0);
        CHECK(d.ref_used_frames() == 1 && d.ref_fallbacks() == 2);
    }
    // No majority (ambiguous frame): the reference stays at the last good one (not cleared), and an unknown/never-closed frame is dropped.
    {
        Driver d; d.configure(false, false, true, false, true);
        const Fleet tie = make_fleet(air, 3, 6, 6, 0, 0);
        d.note_camera(right, up, fwd, tie.m[0].data(), 1000.0);
        for (const auto& m : tie.m) d.ref_member(1, m.data());
        d.ref_close(1, 1000.0);
        CHECK(!d.ref_majority_valid());
        for (const auto& m : tie.m) d.ref_member(2, m.data());        // frame 2 never closes ...
        d.ref_member(3, tie.m[0].data());                              // ... frame 3 starts: frame 2's partial clusters are dropped
        d.ref_close(2, 1016.0);                                        // closing a stale key does nothing
        CHECK(d.ref_frames() == 1);
    }
}

static void ref_diag_tests() {
    V3 air[3]; aircraft_rows(0.7, 0.2, air);
    double right[3], up[3], fwd[3]; camera_for(air, 30 * kDeg, 5 * kDeg, right, up, fwd);
    const Fleet fl = make_fleet(air, 5, 12, 5, 4, 3);
    Driver d; d.configure(false, false, true, false, false);      // the diagnostic does not need the behaviour key
    char buf[480]; unsigned lines = 0, maxlen = 0; bool sawFirst = false;
    for (unsigned k = 0; k < 400; ++k) {
        const double now = 1000.0 + 16.0 * k;
        d.note_camera(right, up, fwd, fl.m[k % fl.m.size()].data(), now);
        for (const auto& m : fl.m) d.ref_member(k, m.data());
        d.ref_close(k, now);
        if (d.take_ref_diag(buf, sizeof buf)) {
            ++lines; maxlen = std::max<unsigned>(maxlen, unsigned(std::strlen(buf)));
            if (lines == 1) {
                sawFirst = true;
                const std::string t(buf);
                CHECK(t.rfind("look-ref-diag;n=1;members=24;clusters=3;maj=0/15;", 0) == 0 || t.find(";maj=") != std::string::npos);
                CHECK(t.find("clusters=3;") != std::string::npos && t.find("maj=") != std::string::npos && t.find("ref=first;") != std::string::npos);
                CHECK(t.find(";c=") != std::string::npos);
                // The aircraft cluster (15 members) reports the true look 30.0/5.0.
                CHECK(t.find("15@30.0/5.0/") != std::string::npos);
            }
        }
    }
    CHECK(sawFirst);
    CHECK(maxlen < 480);
    // First 8 frames at once, then at most one per second of the 400 x 16 ms = 6.4 s run.
    CHECK(lines >= 8 && lines <= 8 + 8);
    // Not active: nothing at all.
    Driver idle;
    idle.ref_member(1, fl.m[0].data()); idle.ref_close(1, 1.0);
    CHECK(!idle.take_ref_diag(buf, sizeof buf) && idle.ref_frames() == 0);
}

// ================================================================== config keys
static void config_tests() {
    using namespace acz::diagnostics;
    {
        auto c = parse_config("log_enabled=true\n");
        CHECK(c.issue == ConfigIssue::none && !c.config.xr_look_ref_cluster() && c.config.neck_lever_m() == 0.0f && c.config.translation_scale() == 1.0f && c.config.xr_hud_follow() == 0);
    }
    CHECK(parse_config("xr_look_ref_cluster=true\n").config.xr_look_ref_cluster() && parse_config("xr_look_ref_cluster=1\n").config.xr_look_ref_cluster() &&
          !parse_config("xr_look_ref_cluster=0\n").config.xr_look_ref_cluster());
    CHECK(parse_config("xr_look_ref_cluster=yes\n").issue == ConfigIssue::out_of_range);
    CHECK(parse_config("xr_look_ref_cluster=true\nxr_look_ref_cluster=true\n").issue == ConfigIssue::duplicate);
    {
        auto a = parse_config("gpu_copy_step5_neck_lever_m=0.12\n");
        CHECK(a.issue == ConfigIssue::none && std::fabs(a.config.neck_lever_m() - 0.12f) < 1e-6f && a.config.translation_scale() == 1.0f);
        auto b = parse_config("gpu_copy_step5_neck_lever_m=0.12,0.5\n");
        CHECK(b.issue == ConfigIssue::none && std::fabs(b.config.neck_lever_m() - 0.12f) < 1e-6f && std::fabs(b.config.translation_scale() - 0.5f) < 1e-6f);
        auto c = parse_config("gpu_copy_step5_neck_lever_m=0,0\n");
        CHECK(c.issue == ConfigIssue::none && c.config.neck_lever_m() == 0.0f && c.config.translation_scale() == 0.0f);
        auto d = parse_config("gpu_copy_step5_neck_lever_m=5,7\n");                    // clamped
        CHECK(d.issue == ConfigIssue::none && std::fabs(d.config.neck_lever_m() - 0.30f) < 1e-6f && d.config.translation_scale() == 1.0f);
    }
    CHECK(parse_config("gpu_copy_step5_neck_lever_m=abc\n").issue == ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_neck_lever_m=0.1,\n").issue == ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_neck_lever_m=,0.5\n").issue == ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_neck_lever_m=-0.1\n").issue == ConfigIssue::out_of_range);
    CHECK(parse_config("gpu_copy_step5_neck_lever_m=0.1,0.2,0.3\n").issue == ConfigIssue::out_of_range);
    for (char v : {'0', '1', '2'}) CHECK(parse_config(std::string("xr_hud_follow=") + v + "\n").config.xr_hud_follow() == unsigned(v - '0'));
    CHECK(parse_config("xr_hud_follow=3\n").issue == ConfigIssue::out_of_range && parse_config("xr_hud_follow=12\n").issue == ConfigIssue::out_of_range &&
          parse_config("xr_hud_follow=true\n").issue == ConfigIssue::out_of_range);
    // Latch: the cluster key needs the look-drive chain; the others are plain values here.
    {
        const std::string base = "log_enabled=true\ngpu_copy_step5_continuous=true\nxr_stereo_output=true\nxr_look_drive_keys=true\n";
        CHECK(parse_config(base + "xr_look_ref_cluster=true\n").config.look_drive_latch(true).ref_cluster);
        CHECK(!parse_config(base).config.look_drive_latch(true).ref_cluster);
        CHECK(!parse_config("xr_look_ref_cluster=true\n").config.look_drive_latch(true).ref_cluster);
        CHECK(!parse_config(base + "xr_look_ref_cluster=true\n").config.look_drive_latch(false).ref_cluster);
    }
    // 26 keys of the headset set + the 3 new ones = 29 keys, < 2048 bytes.
    {
        const char* keys[] = {"log_enabled=true", "diagnostic_ownership_pins=true", "gpu_copy_witness_observation=true", "gpu_copy_early_device_hook=true",
            "gpu_copy_step3_experiment=true", "gpu_copy_step3_whole_set=true", "gpu_copy_step5_continuous=true", "xr_stereo_output=true", "xr_edge_feather=0.06",
            "gpu_copy_step5_members_by_class=true", "gpu_copy_step5_hmd_fov=true", "gpu_copy_step5_hud=true", "xr_hud_layer=true", "xr_hud_quad_distance_m=0.8",
            "xr_hud_scale=0.5", "xr_world_quad_distance_m=500", "gpu_copy_step5_units_per_metre=65", "gpu_copy_step5_hud_markers=true", "xr_hud_markers_layer=true",
            "xr_world_in_projection=true", "gpu_copy_step5_world_objects=true", "xr_world_hold_last=true", "xr_world_extend=true", "xr_look_drive_keys=true",
            "xr_look_drive_xinput=true", "xr_world_window_occlude=true",
            "xr_look_ref_cluster=true", "gpu_copy_step5_neck_lever_m=0.12,0.5", "xr_hud_follow=2"};
        std::string cfg; for (const char* k : keys) cfg += std::string(k) + "\n";
        const auto full = parse_config(cfg);
        CHECK(full.issue == ConfigIssue::none && std::size(keys) == 29 && std::size(keys) <= max_config_keys && cfg.size() <= max_config_bytes &&
              full.config.xr_look_ref_cluster() && full.config.xr_hud_follow() == 2);
    }
}

int main() {
    lean_tests();
    hud_follow_tests();
    ref_cluster_tests();
    ref_diag_tests();
    config_tests();
    std::printf("pov_fixes_offline_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
