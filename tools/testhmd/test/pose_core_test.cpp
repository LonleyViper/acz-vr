// Offline unit test for the acz_testhmd pose-file parser and slew maths. Exit code 0 = all pass.
#include <cmath>
#include <cstdio>
#include <string>

#include "pose_core.h"

using namespace testhmd;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) ++g_pass;                                                \
        else { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
               std::printf(__VA_ARGS__); std::printf("\n"); }              \
    } while (0)

static bool near_eq(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

static void test_parser() {
    PoseTarget t;
    Vec3 keep{ 0.1, 1.2, -0.3 };
    CHECK(parse_pose_text("90 0 0", 120, keep, t), "3 fields");
    CHECK(near_eq(t.yaw, 90) && t.pitch == 0 && t.roll == 0 && near_eq(t.slew, 120) && !t.has_pos && !t.has_slew, "3 fields values");
    CHECK(near_eq(t.pos.y, 1.2), "3 fields keep the position");
    CHECK(parse_pose_text("-90, 10, 5, 45", 120, keep, t) && near_eq(t.yaw, -90) && near_eq(t.slew, 45) && t.has_slew, "4 fields, commas");
    CHECK(parse_pose_text("0 0 0 0.1 0.2 0.3", 120, keep, t) && t.has_pos && near_eq(t.pos.z, 0.3) && near_eq(t.slew, 120), "6 fields");
    CHECK(parse_pose_text("0 0 0 0.1 0.2 0.3 30", 120, keep, t) && near_eq(t.slew, 30), "7 fields");
    CHECK(parse_pose_text("# comment\r\n\r\n  45 -20 0   # trailing\r\n99 99 99", 120, keep, t) && near_eq(t.yaw, 45) && near_eq(t.pitch, -20),
          "comments, CRLF, first data line wins");
    CHECK(parse_pose_text("\xEF\xBB\xBF" "30 0 0\n", 120, keep, t) && near_eq(t.yaw, 30), "UTF-8 BOM");
    CHECK(parse_pose_text("0 0 0 0", 120, keep, t) && t.slew == 0, "slew 0 accepted (snap)");

    PoseTarget before = t;
    std::string why;
    CHECK(!parse_pose_text("", 120, keep, t, &why), "empty rejected");
    CHECK(!parse_pose_text("# only a comment\n", 120, keep, t, &why), "comment-only rejected");
    CHECK(!parse_pose_text("1 2", 120, keep, t, &why), "2 fields rejected");
    CHECK(!parse_pose_text("1 2 3 4 5", 120, keep, t, &why), "5 fields rejected");
    CHECK(!parse_pose_text("1 2 3 4 5 6 7 8", 120, keep, t, &why), "8 fields rejected");
    CHECK(!parse_pose_text("90 abc 0", 120, keep, t, &why) && why.find("abc") != std::string::npos, "non-number rejected: %s", why.c_str());
    CHECK(!parse_pose_text("90 0x 0", 120, keep, t, &why), "trailing junk rejected");
    CHECK(!parse_pose_text("nan 0 0", 120, keep, t, &why), "nan rejected");
    CHECK(!parse_pose_text("inf 0 0", 120, keep, t, &why), "inf rejected");
    CHECK(!parse_pose_text("90 0 0 0 50 0", 120, keep, t, &why), "position out of range rejected");
    CHECK(!parse_pose_text("9000 0 0", 120, keep, t, &why), "angle out of range rejected");
    CHECK(!parse_pose_text("90 0", 120, keep, t, &why), "truncated write rejected");
    CHECK(same_target(t, before), "rejected parses leave the output untouched");
}

static void test_conventions() {
    Quat q = quat_from_ypr_deg(90, 0, 0);
    Vec3 f = qrot(q, { 0, 0, -1 });
    CHECK(near_eq(f.x, -1, 1e-9) && near_eq(f.z, 0, 1e-9), "+yaw 90 turns LEFT: forward = (%.3f,%.3f,%.3f)", f.x, f.y, f.z);
    q = quat_from_ypr_deg(0, 30, 0);
    f = qrot(q, { 0, 0, -1 });
    CHECK(near_eq(f.y, 0.5, 1e-9), "+pitch looks UP: forward.y = %.3f", f.y);
    q = quat_from_ypr_deg(0, 0, 90);
    Vec3 u = qrot(q, { 0, 1, 0 });
    CHECK(near_eq(u.x, -1, 1e-9), "+roll tilts LEFT: up = (%.3f,%.3f,%.3f)", u.x, u.y, u.z);

    const double cases[][3] = { { 0, 0, 0 }, { 90, 0, 0 }, { -90, 0, 0 }, { 45, 30, 0 }, { -120, -45, 20 }, { 170, 60, -30 }, { 10, -80, 5 } };
    for (auto& c : cases) {
        double y, p, r;
        ypr_deg_from_quat(quat_from_ypr_deg(c[0], c[1], c[2]), y, p, r);
        CHECK(near_eq(y, c[0], 1e-7) && near_eq(p, c[1], 1e-7) && near_eq(r, c[2], 1e-7), "ypr round trip %g %g %g -> %g %g %g", c[0], c[1], c[2], y, p, r);
    }
}

// LOCAL-ROLL FIX (testhmd-first-flights.md section 3): model of the observed chain head_local = Rz(180) * q_reported (frame-side roll).
static void test_local_roll_fix() {
    const Quat rz = qaxis(0, 0, 1, kPi);
    const double cases[][3] = { { 90, 0, 0 }, { -90, 0, 0 }, { -90.6, 53.3, 0 }, { -111.8, 34, 0 }, { 30, -20, 10 }, { 0, 0, 0 } };
    for (auto& c : cases) {
        const Quat cmd = quat_from_ypr_deg(c[0], c[1], c[2]);
        const Vec3 want_f = qrot(cmd, { 0, 0, -1 }), want_u = qrot(cmd, { 0, 1, 0 });
        // without the fix: the look is mirrored (yaw and pitch negated) - what the step185 flights showed
        const Quat raw = qnorm(qmul(rz, cmd));
        const Vec3 rf = qrot(raw, { 0, 0, -1 });
        CHECK(near_eq(rf.x, -want_f.x, 1e-9) && near_eq(rf.y, -want_f.y, 1e-9) && near_eq(rf.z, want_f.z, 1e-9),
              "unfixed %g/%g: forward mirrored (%.3f,%.3f,%.3f)", c[0], c[1], rf.x, rf.y, rf.z);
        // with the fix: the commanded look direction exactly, the rest roll (up = -up) kept
        const Quat fixed = qnorm(qmul(rz, local_roll_fix(cmd)));
        const Vec3 f = qrot(fixed, { 0, 0, -1 }), u = qrot(fixed, { 0, 1, 0 });
        CHECK(near_eq(f.x, want_f.x, 1e-9) && near_eq(f.y, want_f.y, 1e-9) && near_eq(f.z, want_f.z, 1e-9),
              "fixed %g/%g: forward (%.3f,%.3f,%.3f) want (%.3f,%.3f,%.3f)", c[0], c[1], f.x, f.y, f.z, want_f.x, want_f.y, want_f.z);
        CHECK(near_eq(u.x, -want_u.x, 1e-9) && near_eq(u.y, -want_u.y, 1e-9) && near_eq(u.z, -want_u.z, 1e-9), "fixed %g/%g: image roll 180 kept", c[0], c[1]);
        // vectors: Rz(180) * fix(v) == v (position offsets and velocities land where the pose file says)
        const Vec3 v{ 0.1 * c[0], 0.2, -0.3 }, vf = qrot(rz, local_roll_fix(v));
        CHECK(near_eq(vf.x, v.x, 1e-9) && near_eq(vf.y, v.y, 1e-9) && near_eq(vf.z, v.z, 1e-9), "vector map %g", c[0]);
    }
    // +pitch is still "look up" in the game's LOCAL frame after the fix
    const Vec3 up = qrot(qnorm(qmul(rz, local_roll_fix(quat_from_ypr_deg(0, 30, 0)))), { 0, 0, -1 });
    CHECK(near_eq(up.y, 0.5, 1e-9), "+pitch 30 looks up after the fix: %.3f", up.y);
}

static void test_slew() {
    SlewState s;
    PoseTarget t;
    t.yaw = 90; t.slew = 120;
    set_target(s, t);
    // 90 deg at 120 deg/s = 0.75 s. Step in 4 ms ticks.
    double tsec = 0, rem = 1;
    bool w_ok = true;
    int ticks = 0;
    while (rem > 0 && ticks < 10000) {
        rem = step(s, 0.004); tsec += 0.004; ++ticks;
        if (rem > 0) {
            // turning LEFT about +Y at 120 deg/s: world angular velocity +Y 2.0944 rad/s
            if (!near_eq(s.ang_vel_world.y, 120 * kDeg, 1e-6) || !near_eq(s.ang_vel_world.x, 0, 1e-9) || !near_eq(s.ang_vel_world.z, 0, 1e-9)) w_ok = false;
            if (!near_eq(s.ang_vel_local.y, 120 * kDeg, 1e-6)) w_ok = false;   // pure yaw: the same in the head frame
        }
        if (ticks == 63) {   // 0.252 s -> 30.24 deg
            double y, p, r; ypr_deg_from_quat(s.q, y, p, r);
            CHECK(near_eq(y, 30.24, 1e-6), "after 0.252 s yaw %.4f (expected 30.24)", y);
        }
    }
    CHECK(w_ok, "angular velocity +Y 120 deg/s during the slew");
    CHECK(near_eq(tsec, 0.752, 0.0041), "90 deg at 120 deg/s took %.3f s (expected ~0.75)", tsec);
    double y, p, r; ypr_deg_from_quat(s.q, y, p, r);
    CHECK(near_eq(y, 90, 1e-7) && near_eq(p, 0, 1e-7), "ends exactly at the target: %g %g", y, p);
    CHECK(s.ang_vel_world.y == 0 && s.ang_vel_local.y == 0, "angular velocity zero at rest");
    step(s, 0.004);
    CHECK(s.ang_vel_world.y == 0, "stays at rest");

    // Right turn reports negative Y.
    t.yaw = 0; set_target(s, t);
    step(s, 0.01);
    CHECK(s.ang_vel_world.y < 0, "turning right: w.y %.3f < 0", s.ang_vel_world.y);

    // Shortest arc: 170 -> -170 crosses 180 (20 deg), not 340 deg.
    s = SlewState{}; t = PoseTarget{}; t.yaw = 170; t.slew = 0; set_target(s, t); step(s, 0.004);
    t.yaw = -170; t.slew = 100; set_target(s, t);
    tsec = 0; rem = 1; ticks = 0;
    while (rem > 0 && ticks < 10000) { rem = step(s, 0.001); tsec += 0.001; ++ticks; }
    CHECK(near_eq(tsec, 0.2, 0.0015), "170 -> -170 took %.3f s (expected 0.2: shortest 20 deg arc)", tsec);
    ypr_deg_from_quat(s.q, y, p, r);
    CHECK(near_eq(std::fabs(y), 170, 1e-6) && y < 0, "ends at -170: %g", y);

    // Snap (slew 0).
    s = SlewState{}; t = PoseTarget{}; t.yaw = -90; t.pitch = 20; t.slew = 0; set_target(s, t);
    rem = step(s, 0.004);
    ypr_deg_from_quat(s.q, y, p, r);
    CHECK(rem == 0 && near_eq(y, -90, 1e-7) && near_eq(p, 20, 1e-7) && s.ang_vel_world.y == 0, "slew 0 snaps");

    // Combined yaw+pitch: constant rate along one axis, unit quaternion throughout.
    s = SlewState{}; t = PoseTarget{}; t.yaw = 60; t.pitch = 30; t.slew = 90; set_target(s, t);
    bool unit = true, rate_ok = true;
    for (int i = 0; i < 500; ++i) {
        rem = step(s, 0.004);
        if (!near_eq(qdot(s.q, s.q), 1, 1e-9)) unit = false;
        if (rem > 0) {
            double wm = std::sqrt(s.ang_vel_world.x * s.ang_vel_world.x + s.ang_vel_world.y * s.ang_vel_world.y + s.ang_vel_world.z * s.ang_vel_world.z);
            double lm = std::sqrt(s.ang_vel_local.x * s.ang_vel_local.x + s.ang_vel_local.y * s.ang_vel_local.y + s.ang_vel_local.z * s.ang_vel_local.z);
            if (!near_eq(wm, 90 * kDeg, 1e-6) || !near_eq(lm, 90 * kDeg, 1e-6)) rate_ok = false;
        }
    }
    ypr_deg_from_quat(s.q, y, p, r);
    CHECK(unit, "quaternion stays unit length");
    CHECK(rate_ok, "|w| = slew rate in both frames");
    CHECK(near_eq(y, 60, 1e-6) && near_eq(p, 30, 1e-6) && near_eq(r, 0, 1e-6), "yaw+pitch target reached: %g %g %g", y, p, r);

    // World vs local angular velocity differ when the start pose is rotated: start yaw 90, then pitch up.
    s = SlewState{}; t = PoseTarget{}; t.yaw = 90; t.slew = 0; set_target(s, t); step(s, 0.004);
    t.pitch = 30; t.slew = 60; set_target(s, t); step(s, 0.004);
    CHECK(near_eq(s.ang_vel_local.x, 60 * kDeg, 1e-6), "local w is +X (pitch up) in the head frame: %.4f", s.ang_vel_local.x);
    CHECK(near_eq(s.ang_vel_world.z, -60 * kDeg, 1e-6), "world w: head +X maps to world -Z after a 90 deg left yaw: %.4f", s.ang_vel_world.z);

    // Position slews at pos_rate.
    s = SlewState{}; s.pos_rate = 0.5; t = PoseTarget{}; t.pos = { 0, 0.5, 0 }; set_target(s, t);
    step(s, 0.5);
    CHECK(near_eq(s.pos.y, 0.25, 1e-9) && near_eq(s.lin_vel.y, 0.5, 1e-9), "position half way after 0.5 s: %.3f", s.pos.y);
    step(s, 0.6);
    CHECK(near_eq(s.pos.y, 0.5, 1e-12) && s.lin_vel.y == 0, "position reached");

    // A garbage dt never moves backward or produces NaN.
    step(s, -1.0);
    step(s, std::nan(""));
    CHECK(std::isfinite(s.q.w) && std::isfinite(s.pos.y), "negative / NaN dt are harmless");
}

int main() {
    test_parser();
    test_conventions();
    test_local_roll_fix();
    test_slew();
    std::printf("testhmd pose_core: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
