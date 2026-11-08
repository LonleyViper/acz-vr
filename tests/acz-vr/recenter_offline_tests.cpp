// SEATED RECENTER offline fixtures (docs/world-render/recenter.md): the option tokens inside xr_terrain_stage, the forbidden-key list, the pose math
// (head centre / heading identical to the producer's Neutral in gpu_copy_step5.h, yaw-only recenter keeps pitch and roll), the key debounce and the
// controller (request, applied, hold, min interval, focus-gated key as an input, auto-flight trigger, runtime space change, pending timeout, off).
// No OS object, no key state, no game.
#include "recenter.h"
#include "gpu_copy_step5.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace rc = acz::recenter;
static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near_(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }
static const double kPi = 3.14159265358979323846;
static const double kDeg = kPi / 180.0;

// Quaternion (x,y,z,w) of yaw (about +Y, positive turns left like OpenXR), pitch (about +X), roll (about -Z... any fixed axis), composed Ry*Rx*Rz.
static void quat_mul(const double a[4], const double b[4], double o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
static void ypr(double yaw, double pitch, double roll, float q[4]) {
    const double qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, qx[4] = {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)}, qz[4] = {0, 0, std::sin(roll / 2), std::cos(roll / 2)};
    double t[4], r[4]; quat_mul(qy, qx, t); quat_mul(t, qz, r);
    for (int i = 0; i < 4; ++i) q[i] = float(r[i]);
}
// Both eyes at the same orientation, +-ipd/2 along the head's own x axis, centre c.
struct Pose { float pos[2][3]; float ori[2][4]; };
static Pose make_pose(const double c[3], double yaw, double pitch, double roll, double ipd = 0.064) {
    Pose p; float q[4]; ypr(yaw, pitch, roll, q);
    double R[9]; const float qq[4] = {q[0], q[1], q[2], q[3]}; acz::gpu_copy::step5::quat_rot(qq, R);
    for (int e = 0; e < 2; ++e) {
        const double s = e ? 0.5 * ipd : -0.5 * ipd;
        for (int k = 0; k < 3; ++k) p.pos[e][k] = float(c[k] + s * R[k * 3 + 0]);
        for (int k = 0; k < 4; ++k) p.ori[e][k] = q[k];
    }
    return p;
}
static rc::Head head_from(const Pose& p) { return rc::head_of(p.pos, p.ori); }

static void test_options() {
    std::string rest, why; rc::Options o;
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1";
    CHECK(rc::extract_options(day, rest, o, why) && rest == day && o.on && o.vk == 0x24 && o.auto_cm == 0 && o.focus && !o.any_token);   // day value untouched
    CHECK(rc::extract_options("auto;rc:0;env:1", rest, o, why) && rest == "auto;env:1" && !o.on && o.any_token);
    CHECK(rc::extract_options("auto;rckey:End;rcauto:25;rcyaw:45;rcfocus:0", rest, o, why) && rest == "auto" && o.vk == 0x23 && o.auto_cm == 25 && o.auto_yaw_deg == 45 && !o.focus);
    CHECK(rc::extract_options("auto;rckey=f9;x:1", rest, o, why) && rest == "auto;x:1" && o.vk == 0x78);
    CHECK(rc::extract_options("auto;rckey:0x2D", rest, o, why) && o.vk == 0x2D);
    CHECK(rc::extract_options("auto;rckey:19", rest, o, why) && o.vk == 19);                                   // Pause by number
    CHECK(rc::extract_options("auto;rcauto:0", rest, o, why) && o.auto_cm == 0);
    CHECK(rc::extract_options("auto;rc:0;rc:1", rest, o, why) && o.on);                                         // last wins
    // the first item is the stage id, never an option; similar names are not ours
    CHECK(rc::extract_options("rc:0", rest, o, why) && rest == "rc:0" && o.on);
    CHECK(rc::extract_options("auto;rcx:1;rcauto2:3;lookstick:game", rest, o, why) && rest == "auto;rcx:1;rcauto2:3;lookstick:game" && !o.any_token);
    // bad values: refused (defaults), tokens still removed, the reason says which
    why.clear();
    CHECK(!rc::extract_options("auto;rckey:v;env:1", rest, o, why) && rest == "auto;env:1" && o.on && o.vk == 0x24 && o.any_token && !why.empty());
    CHECK(!rc::extract_options("auto;rckey:esc", rest, o, why));
    CHECK(!rc::extract_options("auto;rckey:0x68", rest, o, why));          // numpad 8 (look)
    CHECK(!rc::extract_options("auto;rckey:s", rest, o, why));
    CHECK(!rc::extract_options("auto;rckey:f25", rest, o, why));
    CHECK(!rc::extract_options("auto;rcauto:500", rest, o, why));
    CHECK(!rc::extract_options("auto;rcauto:-3", rest, o, why));
    CHECK(!rc::extract_options("auto;rcyaw:1", rest, o, why));
    CHECK(!rc::extract_options("auto;rc:2", rest, o, why));
    CHECK(!rc::extract_options("auto;rcfocus:x", rest, o, why));
    CHECK(!rc::extract_options("auto;rckey:", rest, o, why));
    CHECK(!rc::extract_options("auto;rcauto:nan", rest, o, why));
    // a bad token mixed with good ones: everything falls back to the defaults
    CHECK(!rc::extract_options("auto;rcauto:25;rckey:q", rest, o, why) && o.auto_cm == 0 && rest == "auto");
    // forbidden keys
    const unsigned bad[] = {0x01, 0x02, 0x08, 0x09, 0x0D, 0x10, 0x11, 0x12, 0x1B, 0x20, 0x25, 0x26, 0x27, 0x28, 0x30, 0x39, 0x41, 0x53, 0x56, 0x5A, 0x60, 0x64, 0x66, 0x68, 0x6F, 0x5B, 0x5C, 0, 255};
    for (unsigned v : bad) CHECK(rc::key_forbidden(v));
    const unsigned good[] = {0x13, 0x21, 0x22, 0x23, 0x24, 0x2D, 0x2E, 0x70, 0x7B, 0x87, 0x91};
    for (unsigned v : good) CHECK(!rc::key_forbidden(v));
    CHECK(std::strstr(rc::describe(rc::Options{}).c_str(), "key=0x24") != nullptr);
}

static void test_pose_math() {
    // identity pose: centre = eye midpoint, heading 0
    const double c0[3] = {0.1, 1.2, -0.3};
    rc::Head h = head_from(make_pose(c0, 0, 0, 0));
    CHECK(h.ok && near_(h.p[0], 0.1, 1e-5) && near_(h.p[1], 1.2, 1e-5) && near_(h.p[2], -0.3, 1e-5) && near_(h.yaw, 0, 1e-6));
    // heading = yaw for any yaw, independent of pitch and roll (gravity-aligned: only the heading is captured)
    for (double yd : {-170.0, -90.0, -30.0, 0.0, 30.0, 90.0, 170.0}) {
        for (double pd : {0.0, 25.0, -40.0}) {
            h = head_from(make_pose(c0, yd * kDeg, pd * kDeg, 0.2));
            CHECK(h.ok && near_(rc::wrap_pi(h.yaw - yd * kDeg), 0, 2e-5));
        }
    }
    // identical to the producer's neutral (gpu_copy_step5.h setup_poses) for a turned, pitched, rolled, offset head
    {
        const double c[3] = {0.31, 1.07, -0.22};
        Pose p = make_pose(c, 37 * kDeg, -12 * kDeg, 0.1);
        acz::gpu_copy::step5::EyePose ep[2]{};
        for (int e = 0; e < 2; ++e) { std::memcpy(ep[e].position, p.pos[e], sizeof ep[e].position); std::memcpy(ep[e].orientation, p.ori[e], sizeof ep[e].orientation); for (int k = 0; k < 4; ++k) ep[e].fov[k] = (k % 2 ? 0.8f : -0.8f); }
        acz::gpu_copy::step5::Neutral n{}; acz::gpu_copy::step5::PoseFrame f{}; acz::gpu_copy::step5::Scratch sc{};
        // the camera must be valid for setup_poses: a unit lens at the origin
        auto& cam = f.cam; for (int k = 0; k < 3; ++k) { cam.eye[k] = 0; cam.right[k] = k == 0; cam.up[k] = k == 1; cam.fwd[k] = k == 2; } cam.tanX = 1; cam.tanY = 0.6;
        const char* why = acz::gpu_copy::step5::setup_poses(ep, n, f, sc);
        (void)why;   // the neutral is fixed before any camera-dependent failure; what matters is the capture below
        rc::Head m = head_from(p);
        CHECK(n.set && m.ok);
        for (int k = 0; k < 3; ++k) CHECK(near_(m.p[k], n.p0[k], 1e-6));
        CHECK(near_(m.yaw, n.yaw0, 1e-9));
    }
    // vertical gaze: no heading -> 0, still ok (the producer's rule)
    {
        float q[4]; ypr(0, 89.9999 * kDeg, 0, q);
        const float pos[2][3] = {{0, 1, 0}, {0.06f, 1, 0}}; const float ori[2][4] = {{q[0], q[1], q[2], q[3]}, {q[0], q[1], q[2], q[3]}};
        h = rc::head_of(pos, ori);
        CHECK(h.ok && std::isfinite(h.yaw));
    }
    // invalid input is refused: non-finite, far, zero quaternion
    {
        Pose p = make_pose(c0, 0, 0, 0);
        Pose a = p; a.pos[0][1] = NAN; CHECK(!head_from(a).ok);
        a = p; a.pos[1][0] = 2000.0f; CHECK(!head_from(a).ok);
        a = p; for (int k = 0; k < 4; ++k) a.ori[1][k] = 0; CHECK(!head_from(a).ok);
        a = p; a.ori[0][3] = 3.0f; CHECK(!head_from(a).ok);
    }
    // offset: after a recenter (neutral = the current head) the offset is zero for any yaw; later movement is expressed in the cockpit frame
    for (double yd : {0.0, 30.0, 90.0, -135.0}) {
        const double c[3] = {0.5, 1.3, 0.4};
        rc::Head n = head_from(make_pose(c, yd * kDeg, 0, 0));
        rc::Offset z = rc::offset_from(n, n);
        CHECK(z.dist() < 1e-9 && near_(z.dyaw, 0));
        // the head leans 10 cm toward its own forward and 5 cm up, turned 10 degrees further
        const double fwd[3] = {-std::sin(yd * kDeg), 0, -std::cos(yd * kDeg)};
        const double c2[3] = {c[0] + 0.10 * fwd[0], c[1] + 0.05, c[2] + 0.10 * fwd[2]};
        rc::Head h2 = head_from(make_pose(c2, (yd + 10) * kDeg, 0, 0));
        rc::Offset o = rc::offset_from(h2, n);
        CHECK(near_(o.d[0], 0, 1e-5) && near_(o.d[1], 0.05, 1e-5) && near_(o.d[2], -0.10, 1e-5) && near_(o.dyaw, 10 * kDeg, 1e-5));
    }
    // yaw-only: the recentered frame turns by the heading only; a head pitched down 30 degrees after the recenter keeps its pitch (the offset has no
    // pitch term at all and the heading difference ignores pitch)
    {
        const double c[3] = {0, 1.2, 0};
        rc::Head n = head_from(make_pose(c, 60 * kDeg, 0, 0));
        rc::Head h2 = head_from(make_pose(c, 60 * kDeg, -30 * kDeg, 0.3));
        CHECK(near_(rc::offset_from(h2, n).dyaw, 0, 2e-5));
    }
    CHECK(near_(rc::wrap_pi(3 * kPi), -kPi, 1e-9) || near_(rc::wrap_pi(3 * kPi), kPi, 1e-9));
    CHECK(near_(rc::wrap_pi(-kPi / 2 - 2 * kPi), -kPi / 2, 1e-9));
}

static void test_debounce() {
    rc::Debounce d;
    CHECK(!d.update(true, 0));            // held at start: never fires
    CHECK(!d.update(true, 100));          // still held
    CHECK(!d.update(false, 200));         // released: armed
    CHECK(d.update(true, 300));           // press fires once
    CHECK(!d.update(true, 320) && !d.update(true, 2000));   // hold never repeats
    CHECK(!d.update(false, 2100));
    CHECK(d.update(true, 2200));          // next press after the min interval
    CHECK(!d.update(false, 2210));
    CHECK(!d.update(true, 2300));         // bounce 100 ms later: inside kMinIntervalMs, swallowed
    CHECK(!d.update(false, 2310));
    CHECK(d.update(true, 2200 + rc::kMinIntervalMs));   // exactly at the interval: fires
    rc::Debounce e;
    CHECK(!e.update(false, 0) && e.update(true, 10));   // first press at once after one up sample
    CHECK(!e.update(false, 20) && !e.update(true, 30)); // released and re-pressed inside 1 s: swallowed (and consumed)
}

struct Rig {
    rc::Options o; rc::Controller c; int64_t t = 0; uint64_t fc = 0; uint32_t gen = 0; rc::Head head; bool have = true; bool key = false;
    explicit Rig(const rc::Options& op) : o(op), c(op) {}
    rc::Output step(int64_t dt = 20) {
        t += dt;
        rc::Input in; in.key_down = key; in.now_ms = t; in.flight_composites = fc; in.generation = gen; in.head = have ? &head : nullptr;
        return c.step(in);
    }
};

static void test_controller() {
    const double c0[3] = {0.0, 1.2, 0.0};
    // --- key request, applied, no repeat ---
    {
        Rig r(rc::Options{});
        r.head = head_from(make_pose(c0, 0, 0, 0));
        auto o = r.step();
        CHECK(!o.request && !o.applied && r.c.have_neutral());                     // the first neutral is not a "recenter"
        // user moved: 20 cm right, 8 cm up, turned 40 degrees
        const double c1[3] = {0.20, 1.28, -0.05};
        r.head = head_from(make_pose(c1, 40 * kDeg, -10 * kDeg, 0));
        for (int i = 0; i < 5; ++i) { o = r.step(); CHECK(!o.request); }
        r.key = true; o = r.step();                                                // the key was seen up before: the press fires at once
        CHECK(o.request && o.request_reason == rc::Reason::key);
        CHECK(near_(o.request_offset.d[1], 0.08, 1e-4) && near_(o.request_offset.dyaw, 40 * kDeg, 1e-4));
        for (int i = 0; i < 20; ++i) { auto q = r.step(); CHECK(!q.request); }     // held / pending: no second request
        // owner publishes the new generation; the next sample is the new neutral
        r.gen = 1; o = r.step();
        CHECK(o.applied && o.applied_reason == rc::Reason::key && o.generation == 1 && o.count == 1);
        CHECK(near_(o.after.dist(), 0) && near_(o.after.dyaw, 0));
        CHECK(near_(o.before.d[1], 0.08, 1e-4) && near_(o.before.dyaw, 40 * kDeg, 1e-4));
        CHECK(near_(o.shift[0], 0.20, 1e-5) && near_(o.shift[1], 0.08, 1e-5) && near_(o.shift[2], -0.05, 1e-5));
        CHECK(near_(o.yaw_from, 0, 1e-6) && near_(o.yaw_to, 40 * kDeg, 1e-5));
        CHECK(r.c.applied_count() == 1 && near_(r.c.neutral().yaw, 40 * kDeg, 1e-5));
        // after the recenter a head at the neutral measures zero offset
        CHECK(rc::offset_from(r.head, r.c.neutral()).dist() < 1e-9);
        CHECK(rc::format_applied(o).find("event=recenter;stage=applied;reason=key;gen=1;n=1") != std::string::npos);
        CHECK(rc::format_request(r.step(), 0).size() > 0);
    }
    // --- the request is refused/dropped when no pose: nothing fires, nothing is lost silently ---
    {
        Rig r(rc::Options{}); r.have = false;
        r.key = false; r.step(); r.key = true; auto o = r.step();
        CHECK(!o.request && !r.c.have_neutral());
    }
    // --- min interval: a second press 300 ms after the applied one is swallowed; one after a second works ---
    {
        Rig r(rc::Options{}); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        r.key = false; r.step(); r.key = true; auto o = r.step(); CHECK(o.request);
        r.key = false; r.gen = 1; o = r.step(); CHECK(o.applied);
        r.key = true; o = r.step(150); CHECK(!o.request);       // 170 ms after the first press
        r.key = false; r.step(100); r.key = true; o = r.step(1000); CHECK(o.request);
    }
    // --- pending timeout: no generation arrives (paused session): the pending flag clears and a later press works ---
    {
        Rig r(rc::Options{}); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        r.key = false; r.step(); r.key = true; auto o = r.step(); CHECK(o.request);
        r.key = false; r.step(rc::kPendingTimeoutMs + 100);
        r.key = true; o = r.step(10);
        CHECK(o.request);                                        // pending expired, the debounce interval long passed: a new press works
    }
    // --- rc:0: nothing ever fires and the key is not needed ---
    {
        rc::Options op; op.on = false; Rig r(op); r.head = head_from(make_pose(c0, 0, 0, 0));
        r.key = false; r.step(); r.key = true; auto o = r.step();
        CHECK(!o.request && !o.applied && !r.c.have_neutral());
    }
    // --- a runtime space change nobody requested (SteamVR reset seated position) re-captures and reports reason runtime ---
    {
        Rig r(rc::Options{}); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        const double c1[3] = {0.05, 1.1, 0.1}; r.head = head_from(make_pose(c1, 20 * kDeg, 0, 0)); r.gen = 3;
        auto o = r.step();
        CHECK(o.applied && o.applied_reason == rc::Reason::runtime && o.count == 1 && !o.request);
        CHECK(rc::offset_from(r.head, r.c.neutral()).dist() < 1e-9);
    }
    // --- auto-flight: off by default ---
    {
        Rig r(rc::Options{}); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        const double far_[3] = {0.8, 1.2, 0.0}; r.head = head_from(make_pose(far_, 0, 0, 0));
        r.fc = 10; auto o = r.step(); CHECK(!o.request);
    }
    // --- auto-flight on: far head at the first flight sample fires once; near head does not; once per flight start ---
    {
        rc::Options op; op.auto_cm = 25; op.auto_yaw_deg = 30;
        Rig r(op); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        // menus: composites do not move, even a far head does nothing
        const double far_[3] = {0.40, 1.2, 0.0}; r.head = head_from(make_pose(far_, 0, 0, 0));
        for (int i = 0; i < 10; ++i) CHECK(!r.step(100).request);
        r.fc = 1; auto o = r.step();                              // first flight sample
        CHECK(o.request && o.request_reason == rc::Reason::autoflight && o.request_offset.dist() > 0.39);
        r.gen = 1; o = r.step(); CHECK(o.applied && o.applied_reason == rc::Reason::autoflight);
        // the flight continues: no further auto request
        for (int i = 0; i < 20; ++i) { ++r.fc; CHECK(!r.step(20).request); }
        // the flight ends (no composites for > 500 ms) and a new one starts with the head back at the neutral: below threshold, no request
        r.head = head_from(make_pose(far_, 0, 0, 0));             // neutral == far_ now
        r.step(rc::kFlightGapMs + 100);
        ++r.fc; CHECK(!r.step().request);
    }
    {   // yaw trigger: head position at the eye point but turned 50 degrees
        rc::Options op; op.auto_cm = 25; op.auto_yaw_deg = 30;
        Rig r(op); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        r.head = head_from(make_pose(c0, -50 * kDeg, 0, 0)); r.fc = 5;
        auto o = r.step(); CHECK(o.request && o.request_reason == rc::Reason::autoflight);
    }
    {   // within both thresholds: 10 cm, 10 degrees: nothing, and the flight edge is consumed (no later firing while the head drifts further)
        rc::Options op; op.auto_cm = 25; op.auto_yaw_deg = 30;
        Rig r(op); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        const double n1[3] = {0.10, 1.2, 0.0}; r.head = head_from(make_pose(n1, 10 * kDeg, 0, 0)); r.fc = 5;
        CHECK(!r.step().request);
        const double f2[3] = {0.60, 1.2, 0.0}; r.head = head_from(make_pose(f2, 0, 0, 0));
        for (int i = 0; i < 10; ++i) { ++r.fc; CHECK(!r.step().request); }
    }
    {   // auto fires only when a pose is available: the flight edge waits for the first usable sample
        rc::Options op; op.auto_cm = 25;
        Rig r(op); r.head = head_from(make_pose(c0, 0, 0, 0)); r.step();
        const double far_[3] = {0.5, 1.2, 0.0}; r.head = head_from(make_pose(far_, 0, 0, 0));
        r.have = false; r.fc = 1; CHECK(!r.step().request);
        r.have = true; ++r.fc; CHECK(r.step().request);
    }
}

int main() {
    test_options();
    test_pose_math();
    test_debounce();
    test_controller();
    std::printf("recenter_offline_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
