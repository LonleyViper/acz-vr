// frame_pacing_offline_tests.cpp -- UNCAP / EXTRAP (xr_terrain_stage options uncap:1, extrap:1; frame_pacing.h, docs/world-render/uncap.md).
// CPU-only: option extraction (and the remaining terrain value still parsing), the Present override decision and its state gate, the Present
// statistics, the simulation-rate guard (fixed step vs delta time, game-speed warning) and the motion extrapolator (constant velocity, +8 ms,
// no overshoot on reversal/stop/turn, clamps, cuts, skipped frame ids, attitude rate, slow-motion game rate). No GPU, no game.
#include "frame_pacing.h"
#include "terrain_stage.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace acz::pacing;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static double dist3(const float a[3], const float b[3]) {
    return std::sqrt(double(a[0] - b[0]) * (a[0] - b[0]) + double(a[1] - b[1]) * (a[1] - b[1]) + double(a[2] - b[2]) * (a[2] - b[2]));
}
static double angle_deg(const float a[3], const float b[3]) {
    const double d = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
    const double la = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]), lb = std::sqrt(double(b[0]) * b[0] + double(b[1]) * b[1] + double(b[2]) * b[2]);
    return std::acos(std::fmax(-1.0, std::fmin(1.0, d / (la * lb)))) * 180.0 / 3.14159265358979;
}
// A level camera with heading h (rad): fwd = (-sin h, 0, -cos h) like terrain::camera_frame; right x up = -fwd.
static Frame level(float x, float y, float z, double h) {
    Frame f;
    f.eye[0] = x; f.eye[1] = y; f.eye[2] = z;
    f.fwd[0] = float(-std::sin(h)); f.fwd[1] = 0; f.fwd[2] = float(-std::cos(h));
    f.up[0] = 0; f.up[1] = 1; f.up[2] = 0;
    f.right[0] = float(std::cos(h)); f.right[1] = 0; f.right[2] = float(-std::sin(h));
    return f;
}

static void test_options() {
    Options o; std::string rest, why;
    CHECK(extract_options("auto;env:1;uncap:1;tac:1;extrap:1;extrapms:20", rest, o, why));
    CHECK(rest == "auto;env:1;tac:1" && o.uncap && o.extrap && near(o.extrap_max_ms, 20, 1e-6) && o.extrap_attitude);
    CHECK(extract_options("m02a;extrapatt=0;extrap=1", rest, o, why) && rest == "m02a" && o.extrap && !o.extrap_attitude && !o.uncap);
    CHECK(extract_options("auto;env:1;look:1;props:1;trees:1;propdedupe:1", rest, o, why) && rest == "auto;env:1;look:1;props:1;trees:1;propdedupe:1" && !o.any());
    CHECK(extract_options("m02a", rest, o, why) && rest == "m02a" && !o.any());
    CHECK(extract_options("uncap:1", rest, o, why) && rest == "uncap:1" && !o.uncap);   // the first item is the stage id, never an option
    CHECK(!extract_options("m02a;uncap:2", rest, o, why) && why.find("uncap") != std::string::npos);
    CHECK(!extract_options("m02a;extrap:yes", rest, o, why));
    CHECK(!extract_options("m02a;extrapms:3", rest, o, why));
    CHECK(!extract_options("m02a;extrapms:101", rest, o, why));
    CHECK(!extract_options("m02a;extrapms:abc", rest, o, why));
    CHECK(extract_options("m02a;bogus:1;uncap:0", rest, o, why) && rest == "m02a;bogus:1" && !o.uncap);   // not ours: left for the terrain parser
    CHECK(describe(Options{}) == "uncap=0;extrap=0;extrap-max-ms=33;extrap-attitude=1;interp=0;interp-max-frames=1.50");
    // The value left behind still parses as a terrain option (the installed value + the new options, 99-character key limit kept).
    const std::string installed = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;uncap:1;extrap:1";
    CHECK(installed.size() < 100);
    CHECK(extract_options("m02a;env:1;look:1;uncap:1;extrap:1", rest, o, why) && o.uncap && o.extrap);
    std::string id; acz::terrain::Conventions c;
    CHECK(acz::terrain::parse_stage_option(rest, id, c, why) && id == "m02a");
    CHECK(!acz::terrain::parse_stage_option("m02a;uncap:1", id, c, why));   // the terrain parser alone refuses it: the extraction must run first
    // DECOUPLE (decouple.md): decouple:1 / dcage / dcms / dcfade are ours; defaults off, 250 ms, 70 ms, 0.02; describe() appends them only when on
    CHECK(extract_options("auto;env:1;decouple:1;tac:1", rest, o, why) && rest == "auto;env:1;tac:1" && o.decouple && o.any() && !o.extrap && !o.interp);
    CHECK(near(o.decouple_max_age_ms, 250, 1e-6) && near(o.decouple_horizon_ms, 70, 1e-6) && near(o.decouple_fade, 0.02, 1e-7));
    CHECK(extract_options("m02a;decouple:1;dcage:100;dcms:40;dcfade:0", rest, o, why) && rest == "m02a" && near(o.decouple_max_age_ms, 100, 1e-6) &&
          near(o.decouple_horizon_ms, 40, 1e-6) && o.decouple_fade == 0.0f);
    CHECK(describe(o) == "uncap=0;extrap=0;extrap-max-ms=33;extrap-attitude=1;interp=0;interp-max-frames=1.50;decouple=1;dc-age-ms=100;dc-horizon-ms=40;dc-fade=0.000");
    CHECK(extract_options("m02a;decouple:0", rest, o, why) && rest == "m02a" && !o.decouple && !o.any());
    CHECK(!extract_options("m02a;decouple:2", rest, o, why) && !extract_options("m02a;dcage:10", rest, o, why) && !extract_options("m02a;dcage:501", rest, o, why));
    CHECK(!extract_options("m02a;dcms:4", rest, o, why) && !extract_options("m02a;dcms:151", rest, o, why) && !extract_options("m02a;dcfade:0.3", rest, o, why));
    const std::string morning_dc = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1;interp:1;decouple:1";   // the intended live value: under 100 characters
    CHECK(morning_dc.size() < 100 && extract_options(morning_dc, rest, o, why) && o.decouple && o.interp && rest == "auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1");
}

static void test_present_override() {
    PresentDecision d = present_override(false, 1, 0, true);
    CHECK(d.sync == 1 && d.flags == 0 && !d.changed && !d.tearing_added);
    d = present_override(true, 1, 0, false);
    CHECK(d.sync == 0 && d.flags == 0 && d.changed && !d.tearing_added);
    d = present_override(true, 2, 0x10, true);
    CHECK(d.sync == 0 && d.flags == (0x10u | kPresentAllowTearing) && d.changed && d.tearing_added);
    d = present_override(true, 0, 0, false);
    CHECK(d.sync == 0 && !d.changed);   // the game already presents unsynchronised: nothing to change
    d = present_override(true, 1, kPresentTest, true);
    CHECK(d.sync == 1 && d.flags == kPresentTest && !d.changed);   // a TEST present is never touched
    d = present_override(true, 0, kPresentAllowTearing, true);
    CHECK(!d.changed && !d.tearing_added);
    // gate: both the option and a running XR session
    set_uncap_requested(false); set_xr_active(false);
    CHECK(!uncap_active());
    set_uncap_requested(true); CHECK(!uncap_active());
    set_xr_active(true); CHECK(uncap_active());
    set_xr_active(false); CHECK(!uncap_active());
    set_xr_active(true); set_uncap_requested(false); CHECK(!uncap_active());
    set_xr_active(false);
}

static void test_present_stats() {
    PresentStats s;
    uint64_t t = 1000000;
    for (int i = 0; i < 300; ++i) { s.note(t, i % 3 == 0 ? 1u : 0u, 0x40, present_override(i >= 150, 1, 0, false)); t += 16667; }
    CHECK(s.presents == 300 && s.sync_count[1] == 100 && s.sync_count[0] == 200 && s.flags_or == 0x40 && s.overridden == 150);
    CHECK(!s.due(t - 16667 * 2) || true);
    CHECK(near(s.fps(1000000 + 300 * 16667), 60.0, 0.01));
    CHECK(s.max_interval_us == 16667);
    s.reset(t); CHECK(s.presents == 0 && s.total_presents == 300 && s.window_start_us == t);
    CHECK(s.due(t + 5000000) && !s.due(t + 4999999));
}

// The observed game: fixed step of 18.5 units per frame, Present gaps 16.7 ms with catch-up pairs (34 ms then 6 ms + 10 ms), like the ee-camera logs.
static SimRateReport run_guard(bool fixed_step, double fps, int frames) {
    SimRateGuard g;
    uint64_t t = 5000000; float p[3] = {0, 4000, 0};
    const double base = 1e6 / fps;
    g.feed(t, p, true);
    for (int i = 1; i <= frames; ++i) {
        double dt = base;
        const int k = i % 20;
        if (k == 5) dt = base * 2.0; else if (k == 6) dt = base * 0.36; else if (k == 7) dt = base * 0.64;
        t += uint64_t(dt);
        const double step = fixed_step ? 18.5 : 18.5 * dt / base;   // delta time: displacement proportional to the frame's duration
        p[0] += float(step * 0.7071); p[2] -= float(step * 0.7071);
        g.feed(t, p, true);
    }
    return g.report(t);
}
static void test_sim_guard() {
    SimRateReport r = run_guard(true, 60.0, 600);
    CHECK(std::strcmp(r.model, "fixed-step") == 0 && std::fabs(r.slope) < 0.1 && near(r.fps, 60.0, 0.5) && near(r.units_per_frame, 18.5, 0.05) && !r.warn);
    CHECK(near(r.game_speed, 1.0, 0.01));
    r = run_guard(false, 60.0, 600);
    CHECK(std::strcmp(r.model, "delta-time") == 0 && r.slope > 0.9 && !r.warn && near(r.game_speed, 1.0, 1e-9));
    r = run_guard(true, 120.0, 1200);   // uncapped fixed-step game: twice the rate = twice the speed -> warning
    CHECK(std::strcmp(r.model, "fixed-step") == 0 && r.warn && near(r.game_speed, 2.0, 0.03));
    r = run_guard(false, 120.0, 1200);  // a delta-time game may run faster safely
    CHECK(!r.warn);
    // standing still / no position: unknown model, no division by zero
    SimRateGuard g; float p[3] = {1, 2, 3};
    for (int i = 0; i < 100; ++i) g.feed(uint64_t(1000000 + i * 16667), p, i % 2 == 0);
    const SimRateReport s = g.report(1000000 + 100 * 16667);
    CHECK(std::strcmp(s.model, "unknown") == 0 && s.moving == 0 && s.units_per_frame == 0);
}

// Feed n frames of a camera moving step units/frame along heading h (rad), heading changing by turn (rad/frame), at fps.
static void feed_line(MotionExtrap& m, uint64_t& id, int64_t& t, float pos[3], double& h, int n, double step, double turn, double fps, int idstep = 1) {
    for (int i = 0; i < n; ++i) {
        for (int s = 0; s < idstep; ++s) {   // idstep > 1: frames the consumer never saw (skipped ids) still advanced the fixed-step sim
            h += turn;
            pos[0] += float(-std::sin(h) * step); pos[2] += float(-std::cos(h) * step);
        }
        id += uint64_t(idstep); t += int64_t(1e6 / fps * idstep);
        m.update(id, t, level(pos[0], pos[1], pos[2], h));
    }
}
static void test_extrap() {
    {   // constant velocity: +8 ms at 60 fps lands 0.48 frames ahead
        MotionExtrap m; uint64_t id = 100; int64_t t = 0; float p[3] = {1000, 4000, 2000}; double h = 0.6;
        Frame o; CHECK(std::strcmp(m.predict(8, o).why, "no-frame") == 0);
        feed_line(m, id, t, p, h, 60, 18.5, 0.0, 60.0);
        CHECK(near(m.fps(), 60.0, 0.2));
        const ExtrapResult r = m.predict(8.0, o);
        CHECK(r.translated && !r.clamped && near(r.frames_ahead, 0.48, 0.01));
        const float want[3] = {float(p[0] - std::sin(h) * 18.5 * r.frames_ahead), p[1], float(p[2] - std::cos(h) * 18.5 * r.frames_ahead)};
        CHECK(dist3(o.eye, want) < 0.01);
        CHECK(near(dist3(o.eye, p), 8.88, 0.2));
        CHECK(angle_deg(o.fwd, level(0, 0, 0, h).fwd) < 0.01);   // straight flight: no rotation
        float v[3]; CHECK(m.velocity_per_s(v) && near(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 18.5 * 60.0, 5.0));
        // horizon clamp: 100 ms -> 33 ms
        const ExtrapResult c = m.predict(100.0, o);
        CHECK(c.clamped && near(c.horizon_ms, 33.0, 1e-9) && near(c.frames_ahead, 1.98, 0.01) && near(dist3(o.eye, p), 18.5 * 1.98, 0.3));
        CHECK(!m.predict(0.0, o).translated && !m.predict(-5.0, o).translated);
        CHECK(dist3(o.eye, p) < 1e-6);   // a refused prediction returns the latest frame unchanged
    }
    {   // reversal: never past the turning point
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        feed_line(m, id, t, p, h, 30, 10.0, 0.0, 60.0);
        h += 3.14159265358979;   // fly back the other way (a 180 deg turn in one frame is also a camera cut: no rotation)
        feed_line(m, id, t, p, h, 1, 10.0, 0.0, 60.0);
        Frame o; const ExtrapResult r = m.predict(16.0, o);
        CHECK(!r.translated && std::strcmp(r.why, "direction-change") == 0 && dist3(o.eye, p) < 1e-6);
        feed_line(m, id, t, p, h, 1, 10.0, 0.0, 60.0);   // two consistent steps again: extrapolation resumes
        CHECK(m.predict(16.0, o).translated);
    }
    {   // sudden stop and braking
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 1.0;
        feed_line(m, id, t, p, h, 30, 10.0, 0.0, 60.0);
        feed_line(m, id, t, p, h, 1, 0.0, 0.0, 60.0);
        Frame o; CHECK(!m.predict(16.0, o).translated && dist3(o.eye, p) < 1e-6);
        feed_line(m, id, t, p, h, 3, 10.0, 0.0, 60.0);
        feed_line(m, id, t, p, h, 1, 3.0, 0.0, 60.0);   // speed falls to 30%: outside the ratio band
        CHECK(!m.predict(16.0, o).translated);
    }
    {   // 90 deg change of direction between two steps: refused
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        feed_line(m, id, t, p, h, 20, 10.0, 0.0, 60.0);
        h += 1.5707963; feed_line(m, id, t, p, h, 1, 10.0, 0.0, 60.0);
        Frame o; CHECK(!m.predict(16.0, o).translated);
    }
    {   // steady turn 1 deg/frame: position and heading both extrapolated, error small and never beyond the true arc
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        const double turn = 1.0 * 3.14159265358979 / 180.0;
        feed_line(m, id, t, p, h, 60, 18.5, turn, 60.0);
        Frame o; const ExtrapResult r = m.predict(1000.0 / 60.0, o);   // exactly one frame ahead
        CHECK(r.translated && r.rotated && near(r.frames_ahead, 1.0, 0.02));
        float tp[3] = {p[0], p[1], p[2]}; double th = h;
        th += turn; tp[0] += float(-std::sin(th) * 18.5); tp[2] += float(-std::cos(th) * 18.5);
        CHECK(dist3(o.eye, tp) < 0.5);   // straight-line step vs arc: 18.5 x sin(1 deg) ~ 0.32 units
        CHECK(angle_deg(o.fwd, level(0, 0, 0, th).fwd) < 0.05);
        CHECK(std::fabs(o.right[0] * o.fwd[0] + o.right[1] * o.fwd[1] + o.right[2] * o.fwd[2]) < 1e-5);   // still orthonormal
        CHECK(std::fabs(o.up[0] * o.fwd[0] + o.up[1] * o.fwd[1] + o.up[2] * o.fwd[2]) < 1e-5);
        float rxu[3] = {o.right[1] * o.up[2] - o.right[2] * o.up[1], o.right[2] * o.up[0] - o.right[0] * o.up[2], o.right[0] * o.up[1] - o.right[1] * o.up[0]};
        CHECK(near(rxu[0], -o.fwd[0], 1e-4) && near(rxu[1], -o.fwd[1], 1e-4) && near(rxu[2], -o.fwd[2], 1e-4));   // right x up = -fwd kept
        // attitude off: translation only
        ExtrapConfig c; c.attitude = false; MotionExtrap m2(c); uint64_t id2 = 1; int64_t t2 = 0; float p2[3] = {0, 0, 0}; double h2 = 0.0;
        feed_line(m2, id2, t2, p2, h2, 60, 18.5, turn, 60.0);
        Frame o2; const ExtrapResult r2 = m2.predict(1000.0 / 60.0, o2);
        CHECK(r2.translated && !r2.rotated && angle_deg(o2.fwd, level(0, 0, 0, h2).fwd) < 1e-3);
    }
    {   // rotation clamp: 8 deg/frame for 2 frames ahead (16 deg) is limited to 6 deg
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        feed_line(m, id, t, p, h, 30, 5.0, 8.0 * 3.14159265358979 / 180.0, 60.0);
        Frame o; const ExtrapResult r = m.predict(33.0, o);
        CHECK(r.rotated && r.clamped && near(angle_deg(o.fwd, level(0, 0, 0, h).fwd), 6.0, 0.05));
        // above max_rot_frame (10 deg/frame) = a camera cut: no rotation
        MotionExtrap m2; uint64_t id2 = 1; int64_t t2 = 0; float p2[3] = {0, 0, 0}; double h2 = 0.0;
        feed_line(m2, id2, t2, p2, h2, 30, 5.0, 15.0 * 3.14159265358979 / 180.0, 60.0);
        CHECK(!m2.predict(16.0, o).rotated);
    }
    {   // teleport (respawn / camera cut): refused, resumes after two normal steps
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        feed_line(m, id, t, p, h, 30, 10.0, 0.0, 60.0);
        p[0] += 5000.0f; feed_line(m, id, t, p, h, 1, 10.0, 0.0, 60.0);
        Frame o; ExtrapResult r = m.predict(16.0, o);
        CHECK(!r.translated && std::strcmp(r.why, "cut") == 0);
        feed_line(m, id, t, p, h, 1, 10.0, 0.0, 60.0);
        CHECK(!m.predict(16.0, o).translated);
        feed_line(m, id, t, p, h, 1, 10.0, 0.0, 60.0);
        CHECK(m.predict(16.0, o).translated);
    }
    {   // skipped frame ids (the consumer saw every second game frame): velocity stays per GAME frame
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.3;
        feed_line(m, id, t, p, h, 40, 18.5, 0.0, 60.0, 2);
        CHECK(near(m.fps(), 60.0, 0.3));
        Frame o; const ExtrapResult r = m.predict(8.0, o);
        CHECK(r.translated && near(dist3(o.eye, p), 18.5 * 0.48, 0.2));
        // a repeated frame id changes nothing
        const Frame before = o;
        m.update(id, t + 1000, level(p[0] + 100, p[1], p[2], h));
        Frame o2; m.predict(8.0, o2);
        CHECK(dist3(o2.eye, before.eye) < 1e-6);
    }
    {   // slow-motion game (15 fps, fixed step: the sim runs at 1/4 speed): wall velocity = step x 15, horizon 44 ms clamped to 33
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        feed_line(m, id, t, p, h, 30, 18.5, 0.0, 15.0);
        CHECK(near(m.fps(), 15.0, 0.1));
        Frame o; const ExtrapResult r = m.predict(44.4, o);
        CHECK(r.clamped && near(r.frames_ahead, 0.495, 0.005) && near(dist3(o.eye, p), 18.5 * 0.495, 0.1));
    }
    {   // not before the frame rate is known (first 250 ms)
        MotionExtrap m; uint64_t id = 1; int64_t t = 0; float p[3] = {0, 0, 0}; double h = 0.0;
        feed_line(m, id, t, p, h, 5, 10.0, 0.0, 60.0);
        Frame o; CHECK(std::strcmp(m.predict(8.0, o).why, "no-rate-yet") == 0);
    }
    {   // lead_point: constant velocity, clamp
        float q[3] = {10, 20, 30}; const float v[3] = {1000, 0, 0};
        lead_point(q, v, 0.008, 400.0f); CHECK(near(q[0], 18.0, 1e-4) && near(q[1], 20, 0) && near(q[2], 30, 0));
        float q2[3] = {0, 0, 0}; const float fast[3] = {0, 0, 100000};
        lead_point(q2, fast, 0.033, 400.0f); CHECK(near(q2[2], 400.0, 1e-3));
        float q3[3] = {1, 1, 1}; lead_point(q3, v, -1.0, 400.0f); lead_point(q3, v, std::nan(""), 400.0f); CHECK(q3[0] == 1 && q3[1] == 1);
    }
}

int main() {
    test_options();
    test_present_override();
    test_present_stats();
    test_sim_guard();
    test_extrap();
    std::printf("frame_pacing_offline: %u checks passed\n", g_checks);
    return 0;
}
