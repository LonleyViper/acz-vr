// object_interp_offline_tests.cpp -- OBJECT-INTERP (xr_terrain_stage option interp:1; object_interp.h, docs/world-render/interp.md).
// CPU-only: option extraction, constant-velocity objects at 60 Hz game states sampled at 90 Hz display times (exactly on the line), reversals /
// stops (no overshoot), appear / disappear, the teleport gate, pooled-object reuse, LOD / scale changes, identity across draw-order swaps, rotation
// slerp (angle, axis, orthonormal rows, scale and mirror kept), the extrapolation clamps, the rigid camera delta (objects registered with the
// moved terrain camera, co-moving wingman does not slip) and interp-off / nothing-to-do byte identity of the record. No GPU, no game.
#include "object_interp.h"
#include "frame_pacing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace acz::interp;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static constexpr double kPi = 3.14159265358979323846;
static constexpr int64_t kFrameUs = 16667;   // 60 Hz game frame

static Xform at(double x, double y, double z) { Xform f; f.t[0] = x; f.t[1] = y; f.t[2] = z; return f; }
// Rows of a rotation about +y by a (row-vector convention: row i = image of local axis i), scaled by s.
static Xform yaw_xf(double a, double s, double x, double y, double z) {
    Xform f = at(x, y, z);
    const double c = std::cos(a), sn = std::sin(a);
    const double r[9] = {c, 0, -sn, 0, 1, 0, sn, 0, c};
    for (int i = 0; i < 9; ++i) f.r[i] = r[i] * s;
    return f;
}
static double row_angle_y(const Xform& f) { return std::atan2(-f.r[2], f.r[0]); }   // inverse of yaw_xf for row 0
static double dist(const double a[3], const double b[3]) { double s = 0; for (int i = 0; i < 3; ++i) s += (a[i] - b[i]) * (a[i] - b[i]); return std::sqrt(s); }
static bool orthonormal_rows(const Xform& f, double s, double tol) {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double d = 0; for (int k = 0; k < 3; ++k) d += f.r[i * 3 + k] * f.r[j * 3 + k];
        if (!near(d, i == j ? s * s : 0.0, tol)) return false;
    }
    return true;
}

static void test_options() {
    acz::pacing::Options o; std::string rest, why;
    CHECK(acz::pacing::extract_options("auto;env:1;tac:1;interp:1;interpx:2", rest, o, why));
    CHECK(rest == "auto;env:1;tac:1" && o.interp && near(o.interp_max_frames, 2.0, 1e-6) && o.any() && !o.extrap && !o.uncap);
    CHECK(acz::pacing::extract_options("m02a;interp:0", rest, o, why) && rest == "m02a" && !o.interp && !o.any());
    CHECK(!acz::pacing::extract_options("m02a;interp:2", rest, o, why));
    CHECK(!acz::pacing::extract_options("m02a;interpx:4", rest, o, why));
    CHECK(!acz::pacing::extract_options("m02a;interpx:-1", rest, o, why));
    CHECK(acz::pacing::extract_options("m02a;interpx:0", rest, o, why) && near(o.interp_max_frames, 0.0, 0));
    const std::string installed = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1;extrap:1;interp:1";
    CHECK(installed.size() < 100);
    // process-wide switch: off by default, armed values round trip, horizon unknown until published
    CHECK(!armed());
    arm(true, 1.5f, 33.0f, true);
    CHECK(armed() && horizon_ms() < 0);
    publish_horizon_ms(22.5); CHECK(near(horizon_ms(), 22.5, 1e-3));
    publish_horizon_ms(-3);   CHECK(near(horizon_ms(), 22.5, 1e-3));   // refused, kept
    arm(false, 1.5f, 33.0f, true);
    CHECK(!armed());
    HorizonEma e; e.feed(10); CHECK(near(e.v, 10, 0)); e.feed(20); CHECK(near(e.v, 12, 1e-12)); e.feed(std::nan("")); CHECK(near(e.v, 12, 1e-12));
}

// Constant velocity at 60 Hz states, sampled at 90 Hz display times: every sample exactly on the line, for interpolation AND extrapolation.
static void test_constant_velocity_90hz() {
    ObjectTracker tr;
    const double v[3] = {18.5, -0.75, 3.25};   // units per game frame (aircraft cruise, step50)
    const DrawKey key{0xabcdu, 0x1000, 0x2000};
    uint32_t id = 0;
    for (int f = 0; f < 6; ++f) {
        tr.begin_frame(uint64_t(f + 1), f * kFrameUs);
        Xform out; const Result r = tr.observe(key, at(100 + v[0] * f, 50 + v[1] * f, -20 + v[2] * f), 0.0, out);
        if (f == 0) { CHECK(r.kind == kind_appeared); id = r.id; } else { CHECK(r.kind == kind_matched && r.id == id); }
        CHECK(!r.moved);   // k = 0: the newest state, bit-for-bit
    }
    // newest state = frame 5 (t5), previous = frame 4 (t4). 90 Hz display times 11.111 ms apart, from t4 to t5 + 1.5 frames.
    const double t4 = 4.0 * kFrameUs, t5 = 5.0 * kFrameUs;
    int samples = 0;
    for (double td = t4; td <= t5 + 1.5 * kFrameUs + 1; td += 1e6 / 90.0) {
        const double k = frames_from_times(td, t4, t5);
        Xform out; Result r;
        CHECK(tr.sample(id, k, out, r));
        const double frame = 5.0 + k;   // exact game-frame position on the line
        const double want[3] = {100 + v[0] * frame, 50 + v[1] * frame, -20 + v[2] * frame};
        CHECK(dist(out.t, want) < 1e-9);
        if (k < 0) CHECK(r.interpolated); else if (k > 0) CHECK(r.moved && !r.interpolated);
        CHECK(orthonormal_rows(out, 1.0, 1e-12));
        ++samples;
    }
    CHECK(samples >= 4);
    // the clamp: 3 frames ahead is evaluated at 1.5
    { Xform out; Result r; CHECK(tr.sample(id, 3.0, out, r)); const double want[3] = {100 + v[0] * 6.5, 50 + v[1] * 6.5, -20 + v[2] * 6.5};
      CHECK(r.clamped && dist(out.t, want) < 1e-9); }
    // interpolation below -1 holds the previous state
    { Xform out; Result r; CHECK(tr.sample(id, -2.0, out, r)); const double want[3] = {100 + v[0] * 4, 50 + v[1] * 4, -20 + v[2] * 4}; CHECK(dist(out.t, want) < 1e-9); }
    // k from display times: 60 Hz states 16.667 ms apart, 90 Hz display: k steps of 2/3 frame
    CHECK(near(frames_from_times(t5 + 1e6 / 90.0, t4, t5), 0.6666, 1e-3));
    CHECK(std::isnan(frames_from_times(1, 5, 5)));
}

// Reversal, stop and launch: extrapolation never overshoots (exactly the newest state); interpolation still works between the two states.
static void test_reversal_stop() {
    ObjectTracker tr; const DrawKey key{7, 0, 0};
    const double xs[] = {0, 10, 20, 30, 25, 20, 20, 20, 20, 35};
    uint32_t id = 0;
    for (int f = 0; f < 10; ++f) {
        tr.begin_frame(uint64_t(f + 1), f * kFrameUs);
        Xform out; const Result r = tr.observe(key, at(xs[f], 0, 0), 1.0, out);
        if (f == 0) id = r.id;
        CHECK(r.id == id);
        if (f == 1) CHECK(!r.moved && std::strcmp(r.why, "warming-up") == 0);
        if (f == 2 || f == 3) CHECK(r.moved && near(out.t[0], xs[f] + 10, 1e-12));     // steady +10/frame: one frame ahead
        if (f == 4) CHECK(!r.moved && near(out.t[0], 25, 0) && std::strcmp(r.why, "direction-change") == 0);   // reversal: no overshoot
        if (f == 5) CHECK(r.moved && near(out.t[0], 15, 1e-12));                         // two consistent steps backwards: resumes
        if (f == 6) CHECK(!r.moved && near(out.t[0], 20, 0));                            // stop
        if (f == 7 || f == 8) CHECK(!r.moved && std::strcmp(r.why, "stopped") == 0);
        if (f == 9) CHECK(!r.moved && near(out.t[0], 35, 0));                            // launch from rest: waits for a second step
        if (f == 4) { Xform o2; Result r2; CHECK(tr.sample(id, -0.5, o2, r2) && r2.interpolated && near(o2.t[0], 27.5, 1e-12)); }
    }
    // speed change beyond 2x: refused (braking / afterburner jump), within 2x: extrapolated
    ObjectTracker t2;
    const double ys[] = {0, 10, 20, 45, 70};
    for (int f = 0; f < 5; ++f) {
        t2.begin_frame(uint64_t(f + 1), f * kFrameUs);
        Xform out; const Result r = t2.observe(key, at(0, ys[f], 0), 1.0, out);
        if (f == 3) CHECK(!r.moved);
        if (f == 4) CHECK(r.moved && near(out.t[1], 95, 1e-12));
    }
}

// Appear / disappear / identity across a swapped draw order / LOD (signature) switch / scale change / cap.
static void test_identity() {
    ObjectTracker tr; const DrawKey a{1, 0x10, 0x20}, b{1, 0x10, 0x20};   // two instances of one mesh (same buffers): proximity decides
    uint32_t ida = 0, idb = 0;
    for (int f = 0; f < 5; ++f) {
        tr.begin_frame(uint64_t(f + 1), f * kFrameUs);
        Xform o1, o2;
        // the draw order swaps on odd frames; object A moves +x, object B moves -z, 500 units apart
        const Xform xa = at(10.0 * f, 0, 0), xb = at(500, 0, -10.0 * f);
        Result ra, rb;
        if (f % 2) { rb = tr.observe(b, xb, 0.5, o2); ra = tr.observe(a, xa, 0.5, o1); }
        else { ra = tr.observe(a, xa, 0.5, o1); rb = tr.observe(b, xb, 0.5, o2); }
        if (f == 0) { ida = ra.id; idb = rb.id; CHECK(ida != idb && ra.kind == kind_appeared && rb.kind == kind_appeared); }
        else { CHECK(ra.id == ida && rb.id == idb && ra.kind == kind_matched && rb.kind == kind_matched); }
        if (f >= 2) { CHECK(near(o1.t[0], 10.0 * f + 5, 1e-12) && near(o2.t[2], -10.0 * f - 5, 1e-12)); }
    }
    CHECK(tr.counters().appeared == 2 && tr.counters().lost == 0);
    // B disappears (not drawn in frame 6): one track lost at the next frame start; it reappears as a NEW track (no stale velocity)
    tr.begin_frame(6, 5 * kFrameUs); { Xform o; tr.observe(a, at(50, 0, 0), 0.5, o); }
    tr.begin_frame(7, 6 * kFrameUs); CHECK(tr.lost_last_frame() == 1 && tr.counters().lost == 1);
    { Xform o; CHECK(tr.observe(a, at(60, 0, 0), 0.5, o).id == ida); }
    { Xform o; const Result r = tr.observe(b, at(500, 0, -70), 0.5, o); CHECK(r.kind == kind_appeared && r.id != idb && !r.moved); }
    // LOD switch: another mesh signature at the same place is a new track (the old one is lost), no interpolation across it
    ObjectTracker t2; const DrawKey hi{2, 1, 1}, lo{3, 1, 1};
    for (int f = 0; f < 4; ++f) { t2.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; t2.observe(hi, at(f * 5.0, 0, 0), 1, o); }
    t2.begin_frame(5, 4 * kFrameUs);
    { Xform o; const Result r = t2.observe(lo, at(20, 0, 0), 1, o); CHECK(r.kind == kind_appeared && !r.moved && near(o.t[0], 20, 0)); }
    // scale change (size animation / LOD by scale): matched, not interpolated this frame
    ObjectTracker t3; uint32_t id3 = 0;
    for (int f = 0; f < 3; ++f) { t3.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; id3 = t3.observe(hi, yaw_xf(0, 1.0, f, 0, 0), 1, o).id; }
    t3.begin_frame(4, 3 * kFrameUs);
    { Xform o; const Result r = t3.observe(hi, yaw_xf(0, 1.10, 3, 0, 0), 1, o); CHECK(r.kind == kind_scale && r.id == id3 && !r.moved); }
    // the cap: draw 97 of a frame is untracked (drawn as recorded)
    ObjectTracker t4; t4.begin_frame(1, 0);
    for (unsigned i = 0; i < kCap; ++i) { Xform o; CHECK(t4.observe(DrawKey{i, 0, 0}, at(i, 0, 0), 1, o).kind == kind_appeared); }
    { Xform o; const Result r = t4.observe(DrawKey{999, 0, 0}, at(1, 2, 3), 1, o); CHECK(r.kind == kind_untracked && near(o.t[2], 3, 0)); }
    // a sequence gap (skipped producer frames) drops the history
    ObjectTracker t5;
    for (int f = 0; f < 3; ++f) { t5.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; t5.observe(hi, at(f, 0, 0), 1, o); }
    t5.begin_frame(10, 9 * kFrameUs); { Xform o; CHECK(t5.observe(hi, at(9, 0, 0), 1, o).kind == kind_appeared); }
    CHECK(t5.counters().gaps == 1);
    // a wall-time gap (game paused 200 ms) drops the history too
    ObjectTracker t6;
    for (int f = 0; f < 4; ++f) { t6.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; t6.observe(hi, at(f, 0, 0), 1, o); }
    t6.begin_frame(5, 3 * kFrameUs + 200000); { Xform o; CHECK(t6.observe(hi, at(4, 0, 0), 1, o).kind == kind_appeared); }
}

// Teleport gate and pooled objects reused for different things.
static void test_teleport_pool() {
    ObjectTracker tr; const DrawKey pooled{5, 0x500, 0x600};
    uint32_t id = 0;
    for (int f = 0; f < 4; ++f) { tr.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; id = tr.observe(pooled, at(20.0 * f, 0, 0), 1, o).id; }
    // the pooled missile slot is reused far away (5,000 units: far beyond speed x dt = 20 + 20 units): same buffers + ordinal -> teleport
    tr.begin_frame(5, 4 * kFrameUs);
    { Xform o; const Result r = tr.observe(pooled, at(5000, 300, 0), 1, o); CHECK(r.kind == kind_teleport && r.id == id && !r.moved && near(o.t[0], 5000, 0)); }
    // first step after the teleport: velocity measured but not yet confirmed -> held; the second confirms -> extrapolated
    tr.begin_frame(6, 5 * kFrameUs); { Xform o; const Result r = tr.observe(pooled, at(5030, 300, 0), 1, o); CHECK(r.kind == kind_matched && !r.moved); }
    tr.begin_frame(7, 6 * kFrameUs); { Xform o; const Result r = tr.observe(pooled, at(5060, 300, 0), 1, o); CHECK(r.moved && near(o.t[0], 5090, 1e-12)); }
    CHECK(tr.counters().teleports == 1);
    // a jump just outside the gate with OTHER buffers: not the same object (appeared), the old track is lost
    ObjectTracker t2; const DrawKey k1{6, 0x1, 0x2}, k2{6, 0x3, 0x4};
    for (int f = 0; f < 3; ++f) { t2.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; t2.observe(k1, at(10.0 * f, 0, 0), 1, o); }
    t2.begin_frame(4, 3 * kFrameUs);
    { Xform o; const Result r = t2.observe(k2, at(30 + 10 + 20 + 10 + 1, 0, 0), 1, o); CHECK(r.kind == kind_appeared); }   // predicted 30, gate 20 + 10
    { Xform o; const Result r = t2.observe(k1, at(30 + 29, 0, 0), 1, o); CHECK(r.kind == kind_matched); }                  // inside the gate: still matched
    // a 90-degree turn in one frame (pooled model re-posed): teleport by rotation
    ObjectTracker t3; const DrawKey k3{8, 9, 9};
    for (int f = 0; f < 3; ++f) { t3.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; t3.observe(k3, yaw_xf(0.01 * f, 1, f, 0, 0), 1, o); }
    t3.begin_frame(4, 3 * kFrameUs);
    { Xform o; const Result r = t3.observe(k3, yaw_xf(kPi / 2, 1, 3, 0, 0), 1, o); CHECK(r.kind == kind_teleport && std::strcmp(r.why, "teleport-rotation") == 0 && !r.moved); }
}

// Rotation: slerp on the orthonormalised 3x3, scale (and a mirror) kept, extrapolated rotation clamp.
static void test_rotation() {
    for (int mirror = 0; mirror < 2; ++mirror) {
        ObjectTracker tr; const DrawKey key{11, 0, 0};
        const double step = 3.0 * kPi / 180.0, s = 2.5;
        uint32_t id = 0;
        for (int f = 0; f < 4; ++f) {
            tr.begin_frame(uint64_t(f + 1), f * kFrameUs);
            Xform x = yaw_xf(step * f, s, 0, 0, 0);
            if (mirror) for (int c = 0; c < 3; ++c) x.r[6 + c] = -x.r[6 + c];
            Xform o; id = tr.observe(key, x, 0, o).id;
        }
        for (double k : {-0.75, -0.5, -0.25, 0.5, 1.0, 1.5}) {
            Xform o; Result r; CHECK(tr.sample(id, k, o, r));
            CHECK(r.rotated);
            Xform m = o; if (mirror) for (int c = 0; c < 3; ++c) m.r[6 + c] = -m.r[6 + c];
            CHECK(orthonormal_rows(m, s, 1e-9));
            CHECK(near(row_angle_y(m), step * (3 + k), 1e-9));        // exactly on the rotation path
            CHECK(near(m.r[4], s, 1e-9));                             // the axis stays +y
        }
        // determinant sign kept (mirror)
        { Xform o; Result r; tr.sample(id, 0.5, o, r);
          const double* a = &o.r[0]; const double* b = &o.r[3]; const double* c = &o.r[6];
          const double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
          CHECK(mirror ? det < 0 : det > 0); }
    }
    // general axis: rotation about (1,1,1)/sqrt3 at 4 deg/frame; interpolated half-way = 2 deg from the previous state about the same axis
    {
        auto axis_rot = [](double ang, Xform& x) {
            const double u[3] = {1 / std::sqrt(3.0), 1 / std::sqrt(3.0), 1 / std::sqrt(3.0)}, c = std::cos(ang), sn = std::sin(ang);
            double M[9];   // column convention
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) M[i * 3 + j] = (i == j ? c : 0) + u[i] * u[j] * (1 - c);
            M[1] -= u[2] * sn; M[2] += u[1] * sn; M[3] += u[2] * sn; M[5] -= u[0] * sn; M[6] -= u[1] * sn; M[7] += u[0] * sn;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) x.r[i * 3 + j] = M[j * 3 + i];   // rows = M^T
        };
        ObjectTracker tr; const DrawKey key{12, 0, 0}; uint32_t id = 0;
        const double st = 4.0 * kPi / 180.0;
        for (int f = 0; f < 3; ++f) { tr.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform x; axis_rot(st * f, x); Xform o; id = tr.observe(key, x, 0, o).id; }
        Xform o, want; Result r; CHECK(tr.sample(id, -0.5, o, r)); axis_rot(st * 1.5, want);
        double e = 0; for (int i = 0; i < 9; ++i) e = std::fmax(e, std::fabs(o.r[i] - want.r[i]));
        CHECK(e < 1e-12);
        CHECK(tr.sample(id, 1.0, o, r)); axis_rot(st * 3.0, want);
        e = 0; for (int i = 0; i < 9; ++i) e = std::fmax(e, std::fabs(o.r[i] - want.r[i]));
        CHECK(e < 1e-12);
    }
    // extrapolated rotation clamp (10 deg): 9 deg/frame x 1.5 frames = 13.5 -> 10
    {
        ObjectTracker tr; const DrawKey key{13, 0, 0}; uint32_t id = 0;
        const double st = 9.0 * kPi / 180.0;
        for (int f = 0; f < 3; ++f) { tr.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; id = tr.observe(key, yaw_xf(st * f, 1, 0, 0, 0), 0, o).id; }
        Xform o; Result r; CHECK(tr.sample(id, 1.5, o, r) && r.clamped && near(row_angle_y(o), st * 2 + 10.0 * kPi / 180.0, 1e-9));
    }
    // a rate change (spinning up): rotation not extrapolated; translation unaffected
    {
        ObjectTracker tr; const DrawKey key{14, 0, 0}; uint32_t id = 0;
        const double angs[] = {0, 0.01, 0.02, 0.08};
        for (int f = 0; f < 4; ++f) { tr.begin_frame(uint64_t(f + 1), f * kFrameUs); Xform o; id = tr.observe(key, yaw_xf(angs[f], 1, 10.0 * f, 0, 0), 0, o).id; }
        Xform o; Result r; CHECK(tr.sample(id, 1.0, o, r) && !r.rotated && r.translated && near(o.t[0], 40, 1e-12) && near(row_angle_y(o), 0.08, 1e-7));
    }
    // decompose refuses shear; a sheared object is still translated
    { Xform x = at(0, 0, 0); x.r[1] = 0.3; Decomp d; CHECK(!decompose(x, d)); }
}

// The rigid camera delta: an object compensated for the FRAME camera is seen exactly as the DISPLAY camera sees the uncompensated object;
// the consumer's apply_to_camera turns the frame camera into the display camera.
static void cam_coords(const acz::pacing::Frame& c, const double w[3], double out[3]) {
    const float* ax[3] = {c.right, c.up, c.fwd};
    for (int k = 0; k < 3; ++k) out[k] = (w[0] - c.eye[0]) * ax[k][0] + (w[1] - c.eye[1]) * ax[k][1] + (w[2] - c.eye[2]) * ax[k][2];
}
static acz::pacing::Frame cam_level(float x, float y, float z, double h) {
    acz::pacing::Frame f;
    f.eye[0] = x; f.eye[1] = y; f.eye[2] = z;
    f.fwd[0] = float(-std::sin(h)); f.fwd[1] = 0; f.fwd[2] = float(-std::cos(h));
    f.up[0] = 0; f.up[1] = 1; f.up[2] = 0;
    f.right[0] = float(std::cos(h)); f.right[1] = 0; f.right[2] = float(-std::sin(h));
    return f;
}
static void test_camera_delta() {
    const acz::pacing::Frame fc = cam_level(1000, 500, -2000, 0.3), dc = cam_level(1012, 499, -2015, 0.33);
    const CamDelta d = cam_delta(fc, dc);
    CHECK(!d.identity);
    Xform obj = yaw_xf(0.7, 1.5, 1500, 480, -2600);
    Xform comp = obj; compensate(d, comp);
    double a[3], b[3];
    cam_coords(dc, obj.t, a); cam_coords(fc, comp.t, b);
    CHECK(dist(a, b) < 1e-3);   // float camera axes: ~1e-7 relative on 700 units
    // a model-space point (local (1,2,3)) as well
    double wl[3], wc[3];
    for (int k = 0; k < 3; ++k) { wl[k] = obj.t[k] + 1 * obj.r[k] + 2 * obj.r[3 + k] + 3 * obj.r[6 + k]; wc[k] = comp.t[k] + 1 * comp.r[k] + 2 * comp.r[3 + k] + 3 * comp.r[6 + k]; }
    cam_coords(dc, wl, a); cam_coords(fc, wc, b); CHECK(dist(a, b) < 1e-3);
    CHECK(orthonormal_rows(comp, 1.5, 1e-5));
    // consumer: the frame camera moved by the published delta is the display camera
    float dt[3], dR[9]; for (int i = 0; i < 3; ++i) dt[i] = float(d.dt[i]); for (int i = 0; i < 9; ++i) dR[i] = float(d.dR[i]);
    acz::pacing::Frame m = fc; apply_to_camera(dt, dR, m.eye, m.right, m.up, m.fwd);
    for (int i = 0; i < 3; ++i) { CHECK(near(m.eye[i], dc.eye[i], 1e-3)); CHECK(near(m.right[i], dc.right[i], 1e-6)); CHECK(near(m.up[i], dc.up[i], 1e-6)); CHECK(near(m.fwd[i], dc.fwd[i], 1e-6)); }
    // identical cameras: identity, compensate is a no-op (the exact same doubles)
    const CamDelta id = cam_delta(fc, fc);
    CHECK(id.identity);
    Xform c2 = obj; compensate(id, c2); CHECK(std::memcmp(&c2, &obj, sizeof c2) == 0);
}

// The planner (what the producer runs): camera and objects to the same t_display.
static void test_planner() {
    FramePlanner p;
    Config oc; acz::pacing::ExtrapConfig cc;
    p.configure(oc, cc);
    const double v[3] = {-5.0, 0.0, -18.0};   // player velocity per frame
    const DrawKey wing{21, 0xa, 0xb}, ground{22, 0xc, 0xd}, enemy{23, 0xe, 0xf};
    uint32_t moved_frames = 0;
    for (int f = 0; f < 40; ++f) {
        const int64_t t = int64_t(f) * kFrameUs;
        acz::pacing::Frame cam = cam_level(float(v[0] * f), 1500, float(v[2] * f), 0.27);   // heading constant: translation only
        const double horizon = f < 3 ? -1.0 : 22.0;   // consumer select age known from frame 3
        const FramePlan plan = p.begin(uint64_t(f + 1), t, cam, horizon);
        if (f < 3) { CHECK(plan.k == 0 && plan.delta.identity); }
        // wingman: 60 units right of the camera, same velocity; ground object: fixed; enemy: crossing at 25 units/frame along +x
        const Xform w = at(cam.eye[0] + 60 * cam.right[0], cam.eye[1] - 5, cam.eye[2] + 60 * cam.right[2]);
        const Xform g = at(200, 0, -3000);
        const Xform e = at(-800 + 25.0 * f, 1400, -1500);
        Xform ow, og, oe; double dw[3], dg[3], de[3]; Result rw, rg, re;
        const bool ww = p.object(wing, w, ow, dw, rw), wg = p.object(ground, g, og, dg, rg), we = p.object(enemy, e, oe, de, re);
        if (f < 15) CHECK(plan.k == 0);   // the game frame rate is measured over >= 250 ms first
        if (f >= 16) {
            CHECK(plan.valid && std::strcmp(plan.why, "ok") == 0 && plan.cam_translated);
            const double k = plan.k;
            CHECK(near(k, 22.0 * 1e-3 * p.fps(), 1e-9) && near(p.fps(), 60.0, 0.5) && k > 1.0 && k <= 1.5);
            CHECK(ww && wg && we);
            // display camera = frame camera + v k
            acz::pacing::Frame dcam = cam; for (int i = 0; i < 3; ++i) dcam.eye[i] = float(cam.eye[i] + v[i] * k);
            double a[3], b[3];
            // the wingman keeps its frame-time position relative to the camera (no slip)
            cam_coords(cam, w.t, a); cam_coords(cam, ow.t, b); CHECK(dist(a, b) < 2e-3);
            // the ground object is drawn as the display camera sees it
            cam_coords(dcam, g.t, a); cam_coords(cam, og.t, b); CHECK(dist(a, b) < 2e-3);
            // the enemy at its own t_display position, seen by the display camera; display_t = the world position at t_display
            const double ed[3] = {e.t[0] + 25.0 * k, e.t[1], e.t[2]};
            CHECK(dist(de, ed) < 1e-9);
            cam_coords(dcam, ed, a); cam_coords(cam, oe.t, b); CHECK(dist(a, b) < 2e-3);
            CHECK(dist(dg, g.t) < 1e-12);   // ground: display position = its position
            ++moved_frames;
        }
    }
    CHECK(moved_frames == 24);
    // no horizon / no rate: nothing moves, nothing written (byte identity of the replayed record)
    FramePlanner q; q.configure(oc, cc);
    for (int f = 0; f < 5; ++f) {
        const FramePlan plan = q.begin(uint64_t(f + 1), int64_t(f) * kFrameUs, cam_level(float(10 * f), 0, 0, 0), -1.0);
        CHECK(plan.k == 0 && plan.delta.identity && std::strcmp(plan.why, "no-horizon") == 0);
        Xform o; double d[3]; Result r; CHECK(!q.object(wing, at(10.0 * f, 0, 0), o, d, r));
    }
    // a camera reversal (cut-like turn of the track): the camera refuses -> objects stay at frame time too (no relative slip)
    FramePlanner s; s.configure(oc, cc);
    for (int f = 0; f < 30; ++f) {
        const float x = f < 25 ? float(10 * f) : float(250 - 10 * (f - 25));
        const FramePlan plan = s.begin(uint64_t(f + 1), int64_t(f) * kFrameUs, cam_level(x, 0, 0, 0), 20.0);
        if (f == 24) CHECK(plan.k > 0 && !plan.delta.identity);
        if (f == 26) { CHECK(plan.k == 0 && plan.delta.identity && std::strcmp(plan.why, "direction-change") == 0); }
    }
    // a stationary camera (on the runway): objects still interpolate, no camera delta
    FramePlanner st; st.configure(oc, cc);
    for (int f = 0; f < 30; ++f) {
        const FramePlan plan = st.begin(uint64_t(f + 1), int64_t(f) * kFrameUs, cam_level(5, 5, 5, 0), 16.0);
        Xform o; double d[3]; Result r; const bool w = st.object(enemy, at(10.0 * f, 0, 0), o, d, r);
        if (f >= 16) { CHECK(plan.k > 0.9 && plan.delta.identity && w && near(o.t[0], 10.0 * f + 10.0 * plan.k, 1e-9)); }
    }
}

// Record round trip: B rows / translation at floats 64..78; nothing else touched; an unchanged transform writes the identical bytes.
static void test_record_bytes() {
    uint8_t raw[416], out[416];
    for (unsigned i = 0; i < 416; ++i) raw[i] = uint8_t(i * 37 + 11);
    const float B[16] = {0.8f, 0.0f, -0.6f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.6f, 0.0f, 0.8f, 0.0f, 12120.0f, 512.5f, 132150.25f, 1.0f};
    std::memcpy(raw + 256, B, 64);
    Xform x; xform_from_record(raw, x);
    CHECK(near(x.t[2], 132150.25, 0) && near(x.r[2], -0.6000000238418579, 1e-12));
    std::memcpy(out, raw, 416);
    CHECK(xform_to_record(x, out));
    CHECK(std::memcmp(out, raw, 416) == 0);   // float -> double -> float is exact
    x.t[0] += 10.0; CHECK(xform_to_record(x, out));
    float t0; std::memcpy(&t0, out + 4 * 76, 4); CHECK(t0 == 12130.0f);
    CHECK(std::memcmp(out, raw, 304) == 0 && std::memcmp(out + 308, raw + 308, 416 - 308) == 0);   // only float 76 changed
    x.t[1] = std::nan(""); uint8_t keep[416]; std::memcpy(keep, out, 416);
    CHECK(!xform_to_record(x, out) && std::memcmp(keep, out, 416) == 0);   // non-finite: nothing written
}

int main() {
    test_options();
    test_constant_velocity_90hz();
    test_reversal_stop();
    test_identity();
    test_teleport_pool();
    test_rotation();
    test_camera_delta();
    test_planner();
    test_record_bytes();
    std::printf("object_interp_offline: %u checks passed\n", g_checks);
    return 0;
}
