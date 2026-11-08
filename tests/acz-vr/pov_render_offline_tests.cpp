// POV-RENDER stage 1-4 offline tests (src/acz-vr/pov_render.h; docs/world-render/other-povs.md): the povc / povrig / povv / povtc options, the route
// per view on the EXACT E6 samples logged in live batch W1 (step96 ee-camera.log `obs` lines, the same fixtures as view_mode_offline), the three rigs
// (game exact, level spring, anchor follow / re-latch), camera cuts, the comfort vignette, the census lines and their ACM census requests, the probe's
// request poll, and the tracker's new sample/geometry fields. No D3D12, no game, no headset; fixture success says nothing about the live game.
#include "pov_render.h"
#include "gpu_copy_step5_world.h"   // POV-RENDER stage 2: setup_poses with the rig look, world::relative_offset / substitute_relative (header only)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace acz::povr;
namespace vm = acz::viewmode;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near_eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static constexpr double kDeg = 3.141592653589793 / 180.0;

namespace {

vm::Geo mk(float lp, float ly, float lr, float hdg, float ax, float ay, float az, float cx, float cy, float cz, float rp, float ry, float rr) {
    vm::Geo g;
    g.look[0] = lp; g.look[1] = ly; g.look[2] = lr; g.heading = hdg;
    g.air[0] = ax; g.air[1] = ay; g.air[2] = az; g.cam[0] = cx; g.cam[1] = cy; g.cam[2] = cz;
    g.rot[0] = rp; g.rot[1] = ry; g.rot[2] = rr;
    return g;
}
// W1 (step96) ee-camera.log obs lines (= tests/acz-vr/view_mode_offline_tests.cpp), fields lp,ly,lr,hdg,ax,ay,az,cx,cy,cz,rp,ry,rr.
const vm::Geo kCockpit = mk(0, 0, 0, -0.785398f, -22813.07f, 4000, 130474, -22787.4f, 4003.575f, 130448.3f, -6.3988e-06f, -0.785398f, 3.97407e-13f);
const vm::Geo kRunwayChase = mk(0, 0, 0, 0, 17110.4f, 1846.879f, 21348, 17110.4f, 1860.808f, 21424.62f, 0.0402351f, 0, 4.02245e-08f);
const vm::Geo kFlightChase = mk(0, 0, 0, -0.785398f, -35754.57f, 4000, 143415.6f, -35813.58f, 4017.001f, 143474.6f, -8.04603e-06f, -0.785398f, 4.75906e-15f);
const vm::Geo kHudOnly = mk(0, 0, 0, -0.785398f, 20067.89f, 4000, 87600.61f, 20093.56f, 4003.575f, 87574.94f, -2.85415e-06f, -0.785398f, 4.89429e-13f);
const vm::Geo kOrbitFail = mk(-0.391883f, 3.13506f, 0, -0.785398f, 179729.5f, 4000, -72080.6f, 179800.3f, 4049.23f, -72150.76f, -0.392005f, 2.35064f, -9.89876e-10f);
const vm::Geo kZero{};

vm::Tracker::Step pv_of(vm::Mode m, const vm::Geo& s, bool sample = true) {
    vm::Tracker::Step st;
    st.mode = m; st.sample = sample; st.s = s; st.geom = sample ? vm::classify(s) : vm::GeomOut{};
    return st;
}
Options opt(RigMode r, bool census = false) { Options o; o.rig = r; o.census = census; return o; }

void test_options() {
    std::string rest, why; Options o;
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;"
                            "brief3d:1;fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1;wcloud:1;fxtrail:8;pov:1";
    CHECK(extract_options(day, rest, o, why) && rest == day && !o.any_token && !o.active());   // byte-identical without the tokens (pov:1 is not ours)
    CHECK(extract_options(day + ";povc:1;povrig:level", rest, o, why) && rest == day && o.census && o.rig == RigMode::level && o.any_token);
    CHECK(o.vignette == kVignetteDefault && o.yaw_tc_ms == kYawTcDefaultMs && o.active());
    CHECK(extract_options("auto;povrig=game;env:1;povv:0.25;povtc=900", rest, o, why) && rest == "auto;env:1" && o.rig == RigMode::game &&
          near_eq(o.vignette, 0.25, 1e-6) && o.yaw_tc_ms == 900 && !o.census);
    CHECK(extract_options("auto;povrig:anchor;povrig:off", rest, o, why) && o.rig == RigMode::off && o.any_token && !o.active());   // last wins
    CHECK(!extract_options("auto;povc:1;povrig:spin;env:1", rest, o, why) && rest == "auto;env:1" && !o.census && o.rig == RigMode::off &&
          o.any_token && why.find("povrig") != std::string::npos);   // a bad value: removed, the WHOLE set ignored
    CHECK(!extract_options("auto;povv:1.5", rest, o, why) && rest == "auto" && why.find("povv") != std::string::npos);
    CHECK(!extract_options("auto;povv:abc", rest, o, why) && rest == "auto");
    CHECK(!extract_options("auto;povtc:20", rest, o, why) && rest == "auto" && why.find("povtc") != std::string::npos);
    CHECK(!extract_options("auto;povtc:-5", rest, o, why) && rest == "auto");
    CHECK(!extract_options("auto;povc:yes", rest, o, why) && rest == "auto");
    CHECK(extract_options("povc:1", rest, o, why) && rest == "povc:1" && !o.census);            // the first item is the stage id, never an option
    CHECK(extract_options("auto;povx:1;pov:1;vspov:flat", rest, o, why) && rest == "auto;povx:1;pov:1;vspov:flat" && !o.any_token);   // other keys untouched
    Options d; d.census = true; d.rig = RigMode::level;
    CHECK(describe(d) == "povc=1;povrig=level;povv=0.60;povtc=400");
    CHECK(prerequisites(d, true) == nullptr && std::strcmp(prerequisites(d, false), "needs-pov:1") == 0 && prerequisites(Options{}, false) == nullptr);
    CHECK(day.size() + std::strlen(";povc:1;povrig:level") <= 255);   // the day value + pov:1 + the stage-1 options fit the 255-character terrain option
}

void test_routes() {
    Router r;
    CHECK(r.step(0, vm::Mode::cockpit, vm::classify(kCockpit), true, kCockpit).r == Route::cockpit);
    CHECK(r.step(11, vm::Mode::hud_only, vm::classify(kHudOnly), true, kHudOnly).r == Route::hud_only);
    CHECK(r.step(22, vm::Mode::chase, vm::classify(kRunwayChase), true, kRunwayChase).r == Route::chase);
    CHECK(r.step(33, vm::Mode::chase, vm::classify(kFlightChase), true, kFlightChase).r == Route::chase);
    CHECK(r.step(44, vm::Mode::chase, vm::GeomOut{}, false, kFlightChase).r == Route::flat);           // chase without a current sample: flat
    CHECK(r.step(55, vm::Mode::hud_only, vm::classify(kFlightChase), true, kFlightChase).r == Route::flat);   // mode/geometry disagree: flat
    // the mission-end orbit (111 units from the aircraft): cinematic while the block moves, flat once it is frozen for > 1 s (FMV / pause / menu)
    Router o;
    vm::Geo s = kOrbitFail;
    RouteOut ro{};
    for (int64_t t = 0; t <= 2000; t += 11) { s.cam[0] = kOrbitFail.cam[0] + float(t) * 0.01f; ro = o.step(t, vm::Mode::other, vm::classify(s), true, s); }
    CHECK(vm::classify(kOrbitFail).g == vm::Geom::other && ro.r == Route::cinematic);
    for (int64_t t = 2011; t <= 3100; t += 11) ro = o.step(t, vm::Mode::other, vm::classify(s), true, s);
    CHECK(ro.r == Route::flat && std::strstr(ro.why, "frozen") != nullptr);
    s.cam[1] += 1.0f;
    CHECK(o.step(3111, vm::Mode::other, vm::classify(s), true, s).r == Route::cinematic);             // moving again
    // the zero block (menus) and no sample: flat; a camera 3 km from the aircraft: flat
    CHECK(o.step(3122, vm::Mode::other, vm::classify(kZero), true, kZero).r == Route::flat);
    CHECK(o.step(3133, vm::Mode::other, vm::GeomOut{}, false, kZero).r == Route::flat);
    vm::Geo far = kOrbitFail; far.cam[0] += 3000.0f;
    CHECK(o.step(3144, vm::Mode::other, vm::classify(far), true, far).r == Route::flat);
    CHECK(o.step(3155, vm::Mode::unknown, vm::GeomOut{}, false, kZero).r == Route::flat);
}

void test_rig_game() {
    Rig g; const Options o = opt(RigMode::game);
    RigOut r = g.step(0, Route::chase, kFlightChase, true, o);
    CHECK(r.valid && r.entered && r.mode == RigMode::game && r.vignette == 1.0f);   // entry: a blink
    for (int i = 0; i < 3; ++i) CHECK(r.campos[i] == kFlightChase.cam[i]);
    CHECK(r.pitch == kFlightChase.rot[0] && r.yaw == kFlightChase.rot[1] && r.roll == kFlightChase.rot[2]);
    r = g.step(11, Route::chase, kRunwayChase, true, o);   // 76,000-unit jump in 11 ms: a cut
    CHECK(r.valid && r.cut && r.campos[2] == kRunwayChase.cam[2] && r.pitch == kRunwayChase.rot[0] && g.cuts() == 1);
    // rig off / route not rigged / no sample: invalid
    CHECK(!g.step(22, Route::chase, kFlightChase, true, opt(RigMode::off)).valid);
    CHECK(!g.step(33, Route::cockpit, kCockpit, true, o).valid);
    CHECK(!g.step(44, Route::hud_only, kHudOnly, true, o).valid);
    CHECK(!g.step(55, Route::chase, kFlightChase, false, o).valid);
    vm::Geo bad = kFlightChase; bad.rot[1] = NAN;
    CHECK(!g.step(66, Route::chase, bad, true, o).valid);
    // cinematic with povrig:game stays game; with level/anchor it is the anchor rig
    CHECK(Rig::effective(Route::cinematic, RigMode::game) == RigMode::game && Rig::effective(Route::cinematic, RigMode::level) == RigMode::anchor &&
          Rig::effective(Route::chase, RigMode::level) == RigMode::level && Rig::effective(Route::chase, RigMode::off) == RigMode::off);
}

void test_rig_level_spring() {
    Rig g; const Options o = opt(RigMode::level);   // tc 400 ms
    vm::Geo s = kFlightChase; s.rot[0] = 0.3f; s.rot[2] = 0.5f;   // a climbing, banked game camera
    RigOut r = g.step(0, Route::chase, s, true, o);
    CHECK(r.valid && r.mode == RigMode::level && r.pitch == 0.0f && r.roll == 0.0f && near_eq(r.yaw, s.rot[1], 1e-6));   // level horizon, game yaw
    for (int i = 0; i < 3; ++i) CHECK(r.campos[i] == s.cam[i]);
    // the game camera turns 40 degrees at once (below the 45-degree cut rule): the rig follows without overshoot, ~10.4 deg after ~one tc,
    // < 0.5 deg left after 3 s
    const double y0 = s.rot[1];
    s.rot[1] = float(y0 + 40.0 * kDeg);
    double prev = y0;
    bool mono = true, over = false;
    double at_tc = 0;
    for (int64_t t = 11; t <= 3003; t += 11) {
        s.cam[0] += 1.0f;   // the camera keeps moving (no cut)
        r = g.step(t, Route::chase, s, true, o);
        CHECK(r.valid && !r.cut);
        const double y = r.yaw;
        if (y < prev - 1e-7) mono = false;
        if (y > s.rot[1] + 1e-6) over = true;
        prev = y;
        if (t == 396) at_tc = (y - y0) / kDeg;   // 36 steps = 396 ms
        CHECK(r.campos[0] == s.cam[0] && r.pitch == 0.0f && r.roll == 0.0f);
    }
    CHECK(mono && !over);
    CHECK(near_eq(at_tc, 40.0 - 40.0 * (1.0 + 0.99) * std::exp(-0.99), 0.1));   // the exact critically damped solution at 396 ms (~10.4 deg)
    CHECK(std::fabs(prev - s.rot[1]) / kDeg < 0.5);
    // wrap: from +179 to -179 degrees is a 2-degree turn, never 358
    Rig w; vm::Geo a = kFlightChase; a.rot[1] = float(179.0 * kDeg);
    w.step(0, Route::chase, a, true, o);
    a.rot[1] = float(-179.0 * kDeg);
    double maxdev = 0;
    for (int64_t t = 11; t <= 2002; t += 11) {
        r = w.step(t, Route::chase, a, true, o);
        const double d = std::fabs(wrap_pi(double(r.yaw) - 180.0 * kDeg)) / kDeg;   // distance from the 180-degree seam
        if (d > maxdev) maxdev = d;
    }
    CHECK(maxdev <= 1.0 + 1e-3 && std::fabs(wrap_pi(double(r.yaw) - a.rot[1])) / kDeg < 0.1);
    // a slower spring (povtc:1200) is slower
    Rig sl; Options o2 = o; o2.yaw_tc_ms = 1200;
    vm::Geo b = kFlightChase; sl.step(0, Route::chase, b, true, o2);
    b.rot[1] = float(b.rot[1] + 40.0 * kDeg);
    for (int64_t t = 11; t <= 396; t += 11) r = sl.step(t, Route::chase, b, true, o2);
    CHECK(!r.cut && (double(r.yaw) - kFlightChase.rot[1]) / kDeg < at_tc - 5.0);   // ~1.75 deg
    // a gap > 250 ms restarts the rig at the game yaw
    r = sl.step(1000, Route::chase, b, true, o2);
    CHECK(r.entered && near_eq(r.yaw, b.rot[1], 1e-6));
}

void test_rig_cut() {
    Rig g; const Options o = opt(RigMode::level);
    vm::Geo s = kFlightChase;
    g.step(0, Route::chase, s, true, o);
    for (int64_t t = 11; t <= 500; t += 11) { s.cam[0] += 3.0f; g.step(t, Route::chase, s, true, o); }   // 273 u/s: no cut
    CHECK(g.cuts() == 0);
    s.cam[2] += 1000.0f;   // > 20 u/ms * 11 ms + 200
    RigOut r = g.step(511, Route::chase, s, true, o);
    CHECK(r.cut && r.vignette == 1.0f && g.cuts() == 1);
    s.rot[1] += float(60.0 * kDeg);   // a 60-degree yaw step in one frame: a cut too (the yaw is taken at once)
    r = g.step(522, Route::chase, s, true, o);
    CHECK(r.cut && near_eq(r.yaw, s.rot[1], 1e-6) && g.cuts() == 2);
    r = g.step(533, Route::chase, s, true, o);
    CHECK(!r.cut && r.vignette == 1.0f);   // still inside the 150 ms blink
    for (int64_t t = 544; t <= 2000; t += 11) { s.cam[0] += 1.0f; r = g.step(t, Route::chase, s, true, o); }
    CHECK(r.vignette < 0.02f);             // after the blink, a still rig releases the vignette (250 ms time constant)
    // a route change chase -> cinematic re-enters (another rig)
    r = g.step(2011, Route::cinematic, s, true, o);
    CHECK(r.entered && r.mode == RigMode::anchor);
}

void test_rig_anchor() {
    Rig g; const Options o = opt(RigMode::level);
    vm::Geo s = kOrbitFail;
    RigOut r = g.step(0, Route::cinematic, s, true, o);
    CHECK(r.valid && r.mode == RigMode::anchor && r.pitch == 0.0f && r.roll == 0.0f && r.campos[0] == s.cam[0] && r.blink);
    // stage 4: the game camera jumps 100 units (not a cut) and stays: the anchor follows (2 s pull + the clamped velocity feed-forward of that one
    // step, <= 4,000 u/s x 11 ms = 44 units), overshoots by less than that, settles, never re-latches
    s.cam[0] += 100.0f;
    double maxpos = 0, at2 = 0;
    for (int64_t t = 11; t <= 12001; t += 11) {
        r = g.step(t, Route::cinematic, s, true, o);
        const double m = double(r.campos[0]) - kOrbitFail.cam[0];
        if (m > maxpos) maxpos = m;
        if (t == 2002) at2 = m;
        CHECK(!r.relatched && !r.cut);
    }
    CHECK(at2 >= 100.0 * (1.0 - std::exp(-2.002 / 2.0)) - 0.5 && maxpos <= 100.0 + 45.0);
    CHECK(std::fabs(double(r.campos[0]) - s.cam[0]) < 6.0 && g.relatches() == 0);
    // the camera sprints at 9,000 u/s (99 units per 11 ms step: below the 420-unit cut rule, above the 4,000 u/s velocity clamp): > 400 units
    // behind -> re-latched with a blink
    bool relatched = false;
    for (int64_t t = 12012; t <= 16000 && !relatched; t += 11) { s.cam[2] += 99.0f; r = g.step(t, Route::cinematic, s, true, o); relatched = r.relatched; }
    CHECK(relatched && g.relatches() == 1 && r.campos[2] == s.cam[2] && r.vignette == 1.0f && r.blink);
    // yaw: slow spring (1 s), level
    vm::Geo y = kOrbitFail; Rig h;
    h.step(0, Route::cinematic, y, true, o);
    y.rot[1] = float(y.rot[1] + 30.0 * kDeg);
    for (int64_t t = 11; t <= 990; t += 11) r = h.step(t, Route::cinematic, y, true, o);
    const double turned = wrap_pi(double(r.yaw) - kOrbitFail.rot[1]) / kDeg;
    CHECK(near_eq(turned, 30.0 - 30.0 * (1.0 + 0.99) * std::exp(-0.99), 0.3));   // ~7.9 deg after 0.99 s
}

void test_vignette() {
    Rig g; Options o = opt(RigMode::game);
    vm::Geo s = kFlightChase;
    RigOut r{};
    for (int64_t t = 0; t <= 2002; t += 11) { s.rot[1] = float(kFlightChase.rot[1] + double(t / 11) * kDeg); r = g.step(t, Route::chase, s, true, o); }
    CHECK(near_eq(r.rate_dps, 90.909, 0.05));
    const double x = (90.909 - 30.0) / 90.0, expect = 0.6 * x * x * (3 - 2 * x);
    CHECK(near_eq(r.vignette, expect, 0.01));   // ~0.452 at 91 deg/s
    for (int64_t t = 2013; t <= 3003; t += 11) r = g.step(t, Route::chase, s, true, o);   // the rotation stops
    CHECK(r.rate_dps == 0.0f && r.vignette < 0.02f);
    // a slow turn (20 deg/s) gives no vignette; povv:0 disables the motion vignette (the cut blink stays)
    Rig q; vm::Geo u = kFlightChase;
    for (int64_t t = 0; t <= 2002; t += 11) { u.rot[1] = float(kFlightChase.rot[1] + double(t) * 0.02 * kDeg); r = q.step(t, Route::chase, u, true, o); }
    CHECK(near_eq(r.rate_dps, 20.0, 0.1) && r.vignette < 0.002f);   // only the decayed entry blink is left
    Rig z; Options o0 = o; o0.vignette = 0.0f; vm::Geo v = kFlightChase;
    r = z.step(0, Route::chase, v, true, o0);
    CHECK(r.vignette == 1.0f);
    for (int64_t t = 11; t <= 1100; t += 11) { v.rot[1] = float(kFlightChase.rot[1] + double(t / 11) * 2.0 * kDeg); r = z.step(t, Route::chase, v, true, o0); }
    CHECK(r.rate_dps > 150.0f && r.vignette == 0.0f);
}

void test_census_driver() {
    // the real step96 runway chase geometry: the game camera looks along the aircraft attitude (2.3 deg up), the aircraft 10.3 deg below
    double ang = 0, tp = 0;
    CHECK(fwd_to_air(kRunwayChase, ang, tp) && near_eq(ang, 12.62, 0.05) && near_eq(tp, -10.30, 0.05));
    CHECK(fwd_to_air(kFlightChase, ang, tp) && near_eq(tp, -11.5, 0.2));
    CHECK(fwd_to_air(kCockpit, ang, tp) && ang > 170.0);   // cockpit: the aircraft origin is BEHIND the eye
    CHECK(!fwd_to_air(kZero, ang, tp));

    Driver d; const Options o = opt(RigMode::level, true);
    struct Seg { vm::Mode m; const vm::Geo* s; int64_t ms; };
    const Seg segs[] = {{vm::Mode::cockpit, &kCockpit, 2000}, {vm::Mode::chase, &kRunwayChase, 8000}, {vm::Mode::hud_only, &kHudOnly, 3000},
                        {vm::Mode::chase, &kFlightChase, 3000}, {vm::Mode::cockpit, &kCockpit, 1000}, {vm::Mode::other, &kOrbitFail, 2000}};
    int64_t t = 0;
    unsigned census = 0, summary = 0, requests = 0, first_lines = 0;
    unsigned per_seg[6] = {};
    std::string chase_first;
    for (unsigned k = 0; k < 6; ++k) {
        const int64_t end = t + segs[k].ms;
        bool first = true;
        for (; t < end; t += 11) {
            vm::Geo s = *segs[k].s;
            s.cam[0] += float(t) * 0.05f; s.air[0] += float(t) * 0.05f;   // moving
            const auto st = d.step(t, pv_of(segs[k].m, s), o);
            CHECK(st.lines <= 2);
            for (unsigned i = 0; i < st.lines; ++i) {
                CHECK(std::strlen(st.text[i]) < kLine - 1);   // never truncated
                if (std::strstr(st.text[i], "event=pov-census")) {
                    ++census; ++per_seg[k];
                    if (first) {
                        ++first_lines;
                        CHECK(std::strstr(st.text[i], (std::string("mode=") + vm::mode_name(segs[k].m) + ";").c_str()) != nullptr);
                        CHECK(std::strstr(st.text[i], "visit-ms=0;") != nullptr);
                        if (k == 1) chase_first = st.text[i];
                    }
                } else if (std::strstr(st.text[i], "event=summary-pov-rig")) ++summary;
            }
            if (st.acm_request) { ++requests; CHECK(first); }
            if (first) {
                // a request only at the FIRST visit of each view: cockpit, chase, hud-only, other (chase and cockpit again: none)
                const bool want = k == 0 || k == 1 || k == 2 || k == 5;
                CHECK(st.acm_request == want);
                if (k == 1) CHECK(st.route.r == Route::chase && st.rig.valid && st.rig.mode == RigMode::level && st.rig.entered);
                if (k == 5) CHECK(st.route.r == Route::cinematic && st.rig.mode == RigMode::anchor);
            }
            first = false;
        }
    }
    CHECK(first_lines == 6 && requests == 4 && d.acm_requests() == 4);
    CHECK(per_seg[0] == 2 && per_seg[1] == 6 && per_seg[2] == 3 && per_seg[3] == 3 && per_seg[4] == 1 && per_seg[5] == 2);   // 1 Hz, 6 per visit max
    CHECK(census == d.census_lines() && census == 17);
    CHECK(summary == 2);   // every 10 s with povrig (19 s run)
    CHECK(chase_first.find("route=chase;why=e6-chase-camera;sample=1;geom=chase;") != std::string::npos);
    CHECK(chase_first.find("fwd-to-air-deg=12.6") != std::string::npos && chase_first.find("air-pitch-deg=-10.3") != std::string::npos);
    CHECK(chase_first.find("rig=level:entered;") != std::string::npos && chase_first.find(";acm=requested") != std::string::npos);
    CHECK(chase_first.find("ahead=-76.0;up=17.0;") != std::string::npos);

    // the session cap: 400 census lines, at most 4 ACM requests, whatever the view churn
    Driver c; const Options oc = opt(RigMode::off, true);
    unsigned lines = 0, reqs = 0;
    for (int64_t u = 0; u < 20000 * 11; u += 11) {
        const vm::Mode m = (u / 22) % 2 ? vm::Mode::chase : vm::Mode::hud_only;
        const auto st = c.step(u, pv_of(m, m == vm::Mode::chase ? kFlightChase : kHudOnly), oc);
        for (unsigned i = 0; i < st.lines; ++i) { if (std::strstr(st.text[i], "event=pov-census")) ++lines; CHECK(!std::strstr(st.text[i], "summary-pov-rig")); }
        reqs += st.acm_request ? 1u : 0u;
    }
    CHECK(lines == kCensusCap && c.census_lines() == kCensusCap && reqs == 2);
    // census off, rig off: nothing at all
    Driver n; const auto st = n.step(0, pv_of(vm::Mode::chase, kFlightChase), Options{});
    CHECK(st.lines == 0 && !st.acm_request && !st.rig.valid && st.route.r == Route::chase);
}

void test_acm_poll() {
    AcmPoll p; bool rearm = true;
    CHECK(!p.poll(0, rearm) && !rearm);            // no request ever: the previous rule exactly
    CHECK(p.poll(1, rearm) && rearm && p.arms == 1);
    unsigned active = 1;
    for (int i = 0; i < 30; ++i) { if (p.poll(1, rearm)) ++active; CHECK(!rearm); }
    CHECK(active == kAcmWindows);                  // ~70 s of 5 s windows
    CHECK(p.poll(2, rearm) && rearm && p.arms == 2);
    CHECK(p.poll(2, rearm) && !rearm);
    CHECK(p.poll(3, rearm) && rearm && p.left == kAcmWindows - 1);   // a newer request mid-window: re-armed, fresh window
    acm_request().store(0);
    acm_request().fetch_add(1);
    CHECK(acm_request().load() == 1);
}

void test_tracker_fields() {
    // the tracker now carries its sample and geometry (read by the driver only); a published chase sample with stale composites
    vm::ViewMailbox mb; vm::Tracker tr;
    vm::Tracker::Step st{};
    for (int64_t t = 0; t <= 600; t += 11) { mb.publish(kRunwayChase); st = tr.step(t, 2000, mb); }
    CHECK(st.mode == vm::Mode::chase && st.sample && st.geom.g == vm::Geom::chase && near_eq(st.geom.ahead, -76.0, 0.3));
    CHECK(st.s.cam[2] == kRunwayChase.cam[2] && st.s.rot[0] == kRunwayChase.rot[0]);
    mb.invalidate();
    st = tr.step(611, 2000, mb);
    CHECK(!st.sample && st.geom.g == vm::Geom::invalid);
    Driver d;
    const auto ds = d.step(611, st, opt(RigMode::level));
    CHECK(ds.route.r == Route::flat && !ds.rig.valid);
}

// ================================================================================ STAGE 2 (pov:2)
namespace s5 = acz::gpu_copy::step5;
namespace w5 = acz::gpu_copy::step5::world;

void test_stage2_request() {
    Options o = opt(RigMode::level);
    CHECK(request_for(false, Route::chase, o) == 0u);                       // pov:1 or off: never a request
    for (Route r : {Route::none, Route::cockpit, Route::hud_only, Route::cinematic, Route::flat}) CHECK(request_for(true, r, o) == 0u);
    uint32_t q = request_for(true, Route::chase, o);
    CHECK(req_chase(q) && req_level(q) && req_tc_ms(q) == 400);
    o.yaw_tc_ms = 1200; q = request_for(true, Route::chase, o); CHECK(req_tc_ms(q) == 1200);
    q = request_for(true, Route::chase, opt(RigMode::game)); CHECK(req_chase(q) && !req_level(q));
    q = request_for(true, Route::chase, opt(RigMode::off));  CHECK(req_chase(q) && req_level(q));   // stage 2 without povrig: the level rig
    CHECK(req_tc_ms(encode_request(true, true, 5)) == kYawTcMinMs && req_tc_ms(encode_request(true, true, 99999)) == kYawTcMaxMs);
    CHECK(encode_request(false, true, 400) == 0u && req_tc_ms(0) == kYawTcDefaultMs);
    // the producer's per-opener decision and the cockpit hold
    CHECK(std::strcmp(no_cockpit_why(0u, 5000000, 0), "pov-not-requested") == 0);
    CHECK(no_cockpit_why(q, 5000000, 0) == nullptr);
    CHECK(std::strcmp(no_cockpit_why(q, 5000000, 5000000 + kCockpitHoldUs), "pov-cockpit-seen-hold") == 0);
    CHECK(no_cockpit_why(q, 6000001, 5000000 + kCockpitHoldUs) == nullptr);
    // the classifier's cockpit evidence: fresh cockpit frames only
    CHECK(cockpit_composite(true, false) && !cockpit_composite(true, true) && !cockpit_composite(false, false));
    // the mode machine never sees a no-cockpit chase frame as the cockpit (the consumer feeds it the cockpit-only age)
    vm::Machine m; vm::In in; in.sample = true; in.s = kFlightChase;
    int64_t last_cockpit = 0;
    for (int64_t t = 0; t <= 2000; t += 11) {
        const bool chase_frame = t > 300;                                  // the producer publishes chase frames from 0.3 s on
        if (cockpit_composite(true, chase_frame)) last_cockpit = t;
        in.now_ms = t; in.composite_age_ms = t - last_cockpit;
        m.step(in);
    }
    CHECK(m.mode() == vm::Mode::chase);
}

void test_stage2_opener() {
    const double near_[3] = {kFlightChase.cam[0] + 3.0, kFlightChase.cam[1] - 2.0, kFlightChase.cam[2] + 5.0};
    CHECK(opener_check(near_, &kFlightChase) == nullptr);
    const double far_[3] = {kFlightChase.cam[0] + 500.0, kFlightChase.cam[1], kFlightChase.cam[2]};
    CHECK(std::strcmp(opener_check(far_, &kFlightChase), "pov-camera-not-the-e6-chase-camera") == 0);
    const double rel[3] = {55.0, 19.6, 62.8};   // POV1 census draw all=34: the camera-relative player-aircraft A decomposes to this "eye"
    CHECK(std::strcmp(opener_check(rel, &kFlightChase), "pov-camera-not-the-e6-chase-camera") == 0);
    CHECK(std::strcmp(opener_check(near_, nullptr), "pov-no-e6-sample") == 0);
    CHECK(std::strcmp(opener_check(near_, &kCockpit), "pov-e6-not-chase") == 0);
}

// A camera in the step-5 convention (right x up = -forward) from yaw / pitch / roll.
s5::Camera make_cam(double yaw, double pitch, double roll, const double eye[3], double tanX, double tanY) {
    s5::Camera c;
    const double f[3] = {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    double r[3] = {std::cos(yaw), 0.0, -std::sin(yaw)};
    double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (int i = 0; i < 3; ++i) {
        c.right[i] = r[i] * std::cos(roll) - u[i] * std::sin(roll);
        c.up[i] = u[i] * std::cos(roll) + r[i] * std::sin(roll);
        c.fwd[i] = f[i]; c.eye[i] = eye[i];
    }
    c.tanX = tanX; c.tanY = tanY; c.n3 = 1.0; c.A = -1474.5; c.C = 96696540.0;
    return c;
}
double dot3d(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void test_stage2_rig_math() {
    for (double yaw : {0.0, 0.70978, -2.5, 3.1}) {
        double r[3], u[3], f[3], h = 0;
        level_axes(yaw, r, u, f);
        const double x[3] = {r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0]};
        CHECK(near_eq(x[0], -f[0], 1e-12) && near_eq(x[1], -f[1], 1e-12) && near_eq(x[2], -f[2], 1e-12));   // right x up = -forward
        CHECK(near_eq(dot3d(r, r), 1, 1e-12) && near_eq(dot3d(f, f), 1, 1e-12) && near_eq(dot3d(r, f), 0, 1e-12) && f[1] == 0.0 && u[1] == 1.0);
        CHECK(heading_of(f, h) && std::fabs(wrap_pi(h - yaw)) < 1e-12);
    }
    const double up_[3] = {0, 1, 0}; double h = 0;
    CHECK(!heading_of(up_, h));                                             // straight up: no heading
    // the producer rig: entry at the heading, a 20-degree turn followed by the critically damped spring (60 Hz game frames), cut, gap
    ProducerRig pr; double y = 0, r[3], u[3], f[3];
    level_axes(0.5, r, u, f);
    CHECK(pr.step(f, 1000000, 400, y) && near_eq(y, 0.5, 1e-12) && pr.enters() == 1);
    level_axes(0.5 + 20.0 * kDeg, r, u, f);
    int64_t t = 1000000;
    for (int k = 0; k < 24; ++k) { t += 16667; CHECK(pr.step(f, t, 400, y)); }   // 0.4 s
    const double turned = (y - 0.5) / kDeg, wt = 24 * 0.016667 / 0.4;
    CHECK(near_eq(turned, 20.0 - 20.0 * (1.0 + wt) * std::exp(-wt), 0.05) && pr.enters() == 1);
    level_axes(0.5 + 80.0 * kDeg, r, u, f); t += 16667;                     // a 60-degree heading step: a camera cut, the rig snaps
    CHECK(pr.step(f, t, 400, y) && near_eq(y, 0.5 + 80.0 * kDeg, 1e-12) && pr.enters() == 2);
    t += 400000;                                                             // a 0.4 s gap: restart at the heading
    CHECK(pr.step(f, t, 400, y) && pr.enters() == 3);

    // rig_look in setup_poses: the eyes ride on the LEVEL rig although the matrices are built around a pitched, rolled game camera
    // (the POV1 chase camera: yaw 41.8, pitch -1.8, roll 2.9 deg; lens tan 0.555 x 0.312)
    const double eye[3] = {90684.4, 19234.4, 89491.8};
    s5::PoseFrame pf; pf.cam = make_cam(41.8 * kDeg, -1.8 * kDeg, 2.9 * kDeg, eye, 0.555, 0.312);
    double cam_yaw = 0; CHECK(heading_of(pf.cam.fwd, cam_yaw) && near_eq(cam_yaw, 41.8 * kDeg, 1e-9));
    double rr[3], ru[3], rf[3], look[9];
    level_axes(cam_yaw, rr, ru, rf);
    rig_look(pf.cam.right, pf.cam.up, pf.cam.fwd, rr, ru, rf, look);
    for (int i = 0; i < 3; ++i) {   // a rotation: orthonormal rows
        for (int j = 0; j < 3; ++j) {
            double d = 0; for (int k = 0; k < 3; ++k) d += look[i * 3 + k] * look[j * 3 + k];
            CHECK(near_eq(d, i == j ? 1.0 : 0.0, 1e-9));
        }
    }
    auto run_pose = [&](double head_yaw_deg, s5::Camera eyes[2], double eye_world[2][3]) {
        acz::stereo::EyePose poses[2]{};
        const double hy = head_yaw_deg * kDeg;
        for (int e = 0; e < 2; ++e) {
            poses[e].position[0] = e ? 0.032f : -0.032f; poses[e].position[1] = 1.2f; poses[e].position[2] = 0.0f;
            poses[e].orientation[0] = 0; poses[e].orientation[1] = float(std::sin(hy / 2)); poses[e].orientation[2] = 0; poses[e].orientation[3] = float(std::cos(hy / 2));
        }
        s5::Neutral n; n.set = true; n.p0[0] = 0; n.p0[1] = 1.2; n.p0[2] = 0; n.yaw0 = 0;   // recentred straight ahead
        s5::Scratch sc; s5::PoseFrame p = pf;
        CHECK(s5::setup_poses(poses, n, p, sc, false, 5.0, look) == nullptr && p.valid);
        for (int e = 0; e < 2; ++e) { CHECK(s5::decompose(p.vpEye[e], eyes[e])); for (int k = 0; k < 3; ++k) eye_world[e][k] = p.eyeWorld[e][k]; }
    };
    s5::Camera ec[2]; double ew[2][3];
    run_pose(0.0, ec, ew);
    for (int e = 0; e < 2; ++e) {
        CHECK(dot3d(ec[e].fwd, rf) > 1.0 - 1e-9 && dot3d(ec[e].up, ru) > 1.0 - 1e-9 && dot3d(ec[e].right, rr) > 1.0 - 1e-9);   // head ahead = the rig
        CHECK(std::fabs(ec[e].fwd[1]) < 1e-9);                                                 // a level horizon (the game camera pitches/rolls)
        CHECK(near_eq(ec[e].tanX, 0.555, 1e-9) && near_eq(ec[e].tanY, 0.312, 1e-9));           // the game lens
    }
    const double ipd = std::sqrt((ew[1][0] - ew[0][0]) * (ew[1][0] - ew[0][0]) + (ew[1][1] - ew[0][1]) * (ew[1][1] - ew[0][1]) + (ew[1][2] - ew[0][2]) * (ew[1][2] - ew[0][2]));
    CHECK(near_eq(ipd, 0.064 * 5.0, 1e-4));                                                     // 64 mm at 5 units per metre
    const double sep[3] = {ew[1][0] - ew[0][0], ew[1][1] - ew[0][1], ew[1][2] - ew[0][2]};
    CHECK(near_eq(dot3d(sep, rr) / ipd, 1.0, 1e-6));                                            // along the RIG's right axis (level)
    CHECK(near_eq(0.5 * (ew[0][0] + ew[1][0]), eye[0], 1e-6) && near_eq(0.5 * (ew[0][2] + ew[1][2]), eye[2], 1e-6));   // at the game camera
    run_pose(30.0, ec, ew);                                                                     // head turned 30 degrees left
    double hy2 = 0; CHECK(heading_of(ec[0].fwd, hy2) && near_eq(wrap_pi(hy2 - cam_yaw) / kDeg, 30.0, 1e-6) && std::fabs(ec[0].fwd[1]) < 1e-9);
    // povrig:game (rig = the camera): lookR = identity, the eyes = the game camera (the stage-1 behaviour)
    rig_look(pf.cam.right, pf.cam.up, pf.cam.fwd, pf.cam.right, pf.cam.up, pf.cam.fwd, look);
    for (int i = 0; i < 9; ++i) CHECK(near_eq(look[i], i % 4 == 0 ? 1.0 : 0.0, 1e-12));
}

// The POV1 chase census records (ee-camera.log 199052054, s=1): draw all=19 = the absolute camera, all=34 = the same rows 0..2 with the player
// aircraft folded into row 3 (w = 84.0 = the chase distance).
const float kA19[16] = {-1021.34326f, -1322.57727f, 982.172607f, -0.666103005f, -41.1409988f, -780.0578f, 46.1609306f, -0.0313060395f,
                        -1833.58557f, -1533.28406f, 1098.80554f, -0.745202661f, 257502144.0f, 272157504.0f, -91592480.0f, 127696.859f};
const float kA34[16] = {-1021.34326f, -1322.57727f, 982.172607f, -0.666103005f, -41.1409988f, -780.0578f, 46.1609306f, -0.0313060395f,
                        -1833.58557f, -1533.28406f, 1098.80554f, -0.745202661f, 172032.031f, 184231.594f, 96573408.0f, 84.0000153f};
void make_record(const float a[16], uint8_t out[416]) {
    std::memset(out, 0, 416);
    std::memcpy(out, a, 64);
    const float B[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(out + 256, B, 64);
    const float k360 = 1.0f; std::memcpy(out + 360, &k360, 4);
    const float k400[4] = {512.0f, -448.0f, 2048.0f, 2048.0f}; std::memcpy(out + 400, k400, 16);
}
void test_stage2_relative() {
    uint8_t r19[416], r34[416];
    make_record(kA19, r19); make_record(kA34, r34);
    CHECK(w5::record_check(r19) == nullptr && w5::record_check(r34) == nullptr);
    w5::Frame w; s5::Scratch sc;
    CHECK(w5::camera_setup(r19, nullptr, w, sc) == nullptr && w.camera);
    // the absolute camera = the POV1 analysis eye (90684.4, 19234.4, 89491.8); the chase lens through the flight screen map: tan 0.560 x 0.315
    // (58.5 x 35.0 deg; analyze-pov.py's own scale printed 58.1 vs 62.8 for the cockpit: the chase lens is narrower)
    CHECK(near_eq(w.eyeAbs[0], 90684.4, 2.0) && near_eq(w.eyeAbs[1], 19234.4, 2.0) && near_eq(w.eyeAbs[2], 89491.8, 2.0));
    CHECK(near_eq(w.pf.cam.tanX, 0.5597, 0.001) && near_eq(w.pf.cam.tanY, 0.3148, 0.001));
    CHECK(std::strcmp(w5::record_matches(r34, w), "world-vp-differs") == 0);   // today: refused (POV1 world-vp-differs in chase)
    double p[3];
    CHECK(w5::relative_offset(r34, w, p) == nullptr);
    CHECK(near_eq(p[0], 90629.47, 0.5) && near_eq(p[1], 19214.83, 0.5) && near_eq(p[2], 89429.04, 0.5));   // the aircraft, 85.7 units ahead of the eye
    const double d[3] = {w.eyeAbs[0] - p[0], w.eyeAbs[1] - p[1], w.eyeAbs[2] - p[2]};
    CHECK(near_eq(std::sqrt(dot3d(d, d)), 85.7, 0.5));
    CHECK(w5::relative_offset(r19, w, p) == nullptr && std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) < 1e-3);   // itself: p = 0
    float bad[16]; std::memcpy(bad, kA34, 64); bad[1] *= 1.01f;
    uint8_t rb[416]; make_record(bad, rb);
    CHECK(std::strcmp(w5::relative_offset(rb, w, p), "world-vp-differs") == 0);   // another rotation: never relative
    std::memcpy(bad, kA34, 64); bad[14] += 1.0e6f; make_record(bad, rb);          // a translation row that is not T(p) x A (column 2 disagrees)
    CHECK(std::strcmp(w5::relative_offset(rb, w, p), "world-relative-depth-column") == 0);
    w5::Frame none;
    CHECK(std::strcmp(w5::relative_offset(r34, none, p), "world-relative-no-camera") == 0);
    // substitution: zero delta keeps the raw bytes; a delta gives T(p) x (A_ref + D S), i.e. the substituted absolute camera applied at p
    CHECK(w5::relative_offset(r34, w, p) == nullptr);
    w.pf.valid = true;
    uint8_t o19[512], o34[512];
    CHECK(w5::substitute_relative(r34, w.pf, 0, p, o34) == nullptr && std::memcmp(o34, r34, 416) == 0);
    for (int i = 0; i < 16; ++i) w.pf.D[0][i] = 0.0;
    w.pf.D[0][12] = 0.8; w.pf.D[0][15] = 0.02; w.pf.D[0][0] = 1e-4;   // an eye offset + a tiny rotation in clip terms
    CHECK(w5::substitute(r19, w.pf, 0, o19) == nullptr && w5::substitute_relative(r34, w.pf, 0, p, o34) == nullptr);
    float a19[16], a34[16]; std::memcpy(a19, o19, 64); std::memcpy(a34, o34, 64);
    for (int i = 0; i < 12; ++i) CHECK(a34[i] == a19[i]);                                    // rows 0..2: the substituted camera
    for (int c = 0; c < 4; ++c) {
        const double want = p[0] * a19[c] + p[1] * a19[4 + c] + p[2] * a19[8 + c] + double(a19[12 + c]);
        CHECK(std::fabs(double(a34[12 + c]) - want) <= 1e-6 * std::fabs(double(a19[12 + c])) + 64.0);   // float rows of ~2.5e8
    }
    CHECK(std::memcmp(o34 + 64, r34 + 64, 416 - 64) == 0);                                    // every other byte untouched
}

// ================================================================================ STAGES 3 + 4 (pov:3)
// runPOV1 xr-stereo.log pov-census n=97 (199,317,296): the m03 mission-end camera, 111.2 units from the aircraft (99.0 ahead, 50.5 up), looking back
// at it (look yaw 3.13), moving with it at ~1,100 u/s (n=98..100: +1,115 u/s along z).
const vm::Geo kEndCam = mk(-0.3916f, 3.1326f, 0, -2.94982f, -31281.5f, 4014.0f, 179912.2f, -31263.4f, 4063.9f, 180010.0f, -0.39828f, 0.18233f, 0.00458f);

void test_stage3_request() {
    Options o = opt(RigMode::level);
    // pov:2 requests are unchanged by the kind field (kind chase = 0)
    CHECK(encode_request(true, true, 400) == (kReqChase | kReqLevel | (400u << 8)) && req_kind(encode_request(true, true, 400)) == NcKind::chase);
    CHECK(request_for(true, Route::hud_only, o) == 0u && request_for(true, Route::cinematic, o) == 0u);   // pov:2: chase only
    uint32_t q = request_for_stage3(Route::chase, o);
    CHECK(q == request_for(true, Route::chase, o) && req_kind(q) == NcKind::chase && req_level(q));
    q = request_for_stage3(Route::hud_only, o);
    CHECK(req_chase(q) && req_kind(q) == NcKind::hud && !req_level(q));                 // HUD-only: the game camera, never a rig
    q = request_for_stage3(Route::cinematic, o);
    CHECK(req_chase(q) && req_kind(q) == NcKind::cinematic && req_level(q) && req_tc_ms(q) == 400);   // the anchor rig
    q = request_for_stage3(Route::cinematic, opt(RigMode::game));
    CHECK(req_kind(q) == NcKind::cinematic && !req_level(q));                           // povrig:game: the game camera
    for (Route r : {Route::none, Route::cockpit, Route::flat}) CHECK(request_for_stage3(r, o) == 0u);
    CHECK(no_cockpit_why(request_for_stage3(Route::hud_only, o), 1, 0) == nullptr);      // the producer opens any requested kind
    CHECK(std::strcmp(nc_kind_name(NcKind::hud), "hud-only") == 0 && std::strcmp(nc_kind_name(NcKind::cinematic), "cinematic") == 0);
}

void test_stage3_routes_and_openers() {
    // the mission-end camera: geometry other, 111 units -> cinematic while it moves, flat once frozen > 1 s (FMV / pause / menu)
    const vm::GeomOut ge = vm::classify(kEndCam);
    CHECK(ge.g == vm::Geom::other && near_eq(ge.dist, 111.2, 0.3));
    Router r; vm::Geo s = kEndCam; RouteOut ro{};
    for (int64_t t = 0; t <= 1000; t += 11) {
        s.cam[2] = kEndCam.cam[2] + float(t) * 1.115f; s.air[2] = kEndCam.air[2] + float(t) * 1.115f;
        ro = r.step(t, vm::Mode::other, vm::classify(s), true, s);
    }
    CHECK(ro.r == Route::cinematic);
    for (int64_t t = 1011; t <= 2100; t += 11) ro = r.step(t, vm::Mode::other, vm::classify(s), true, s);
    CHECK(ro.r == Route::flat);
    // openers per kind: HUD-only = the E6 eye; cinematic = the E6 'other' camera; the stage-2 chase check unchanged
    const double hud_eye[3] = {kHudOnly.cam[0] + 10.0, kHudOnly.cam[1], kHudOnly.cam[2] - 12.0};
    CHECK(opener_check(hud_eye, &kHudOnly, NcKind::hud) == nullptr);
    CHECK(std::strcmp(opener_check(hud_eye, &kFlightChase, NcKind::hud), "pov-e6-not-eye") == 0);
    const double hud_far[3] = {kHudOnly.cam[0] + 900.0, kHudOnly.cam[1], kHudOnly.cam[2]};   // the post-crash camera of runPOV2b (E6 still the eye)
    CHECK(std::strcmp(opener_check(hud_far, &kHudOnly, NcKind::hud), "pov-camera-not-the-e6-eye") == 0);
    const double end_eye[3] = {kEndCam.cam[0] + 17.0, kEndCam.cam[1], kEndCam.cam[2] + 18.0};   // one frame of motion
    CHECK(opener_check(end_eye, &kEndCam, NcKind::cinematic) == nullptr);
    CHECK(std::strcmp(opener_check(end_eye, &kHudOnly, NcKind::cinematic), "pov-e6-not-other") == 0);
    vm::Geo farcam = kEndCam; farcam.cam[0] += 3000.0f;
    CHECK(std::strcmp(opener_check(end_eye, &farcam, NcKind::cinematic), "pov-e6-camera-far-from-aircraft") == 0);
    const double rel[3] = {55.0, 19.6, 62.8};   // a camera-relative first draw
    CHECK(std::strcmp(opener_check(rel, &kEndCam, NcKind::cinematic), "pov-camera-not-the-e6-cinematic-camera") == 0);
    // runPOV2b replay: E6 shows a chase geometry, the rendered replay camera is elsewhere -> refused (the replay stays flat)
    const double replay[3] = {kFlightChase.cam[0] + 2500.0, kFlightChase.cam[1] - 300.0, kFlightChase.cam[2]};
    CHECK(std::strcmp(opener_check(replay, &kFlightChase, NcKind::chase), "pov-camera-not-the-e6-chase-camera") == 0);
}

// The anchor on the REAL mission-end camera motion (1,115 u/s along z, 60 Hz game frames, the camera bobbing +-6 units at 0.5 Hz):
// the stage-1 world point (no feed-forward) re-latched every ~0.4-0.6 s; the stage-4 follower carries the velocity: no re-latch after the entry,
// bounded lag, zero steady-state lag.
void test_stage4_anchor_travel() {
    // reference: the stage-1 rule (pure 2 s follow) on the same motion, as logged in runPOV1 (rig-cam 206 / 378 units behind, then re-latched)
    unsigned old_relatch = 0; double oldp = 0;
    for (int k = 1; k <= 600; ++k) {   // 10 s at 60 Hz
        const double cam = 1115.0 * k / 60.0;
        oldp += (1.0 - std::exp(-(1.0 / 60.0) / 2.0)) * (cam - oldp);
        if (cam - oldp > kAnchorRadius) { oldp = cam; ++old_relatch; }
    }
    CHECK(old_relatch >= 15);
    ProducerAnchor a; double pos[3], yaw = 0; const char* ev = nullptr;
    const double fwd[3] = {-std::sin(double(kEndCam.rot[1])), 0.0, -std::cos(double(kEndCam.rot[1]))};
    double maxlag = 0, lag = 0, maxbob = 0, maxbob_late = 0;
    int64_t t = 5000000;
    for (int k = 0; k <= 1200; ++k, t += 16667) {   // 20 s
        const double ts = double(k) / 60.0;
        const double eye[3] = {kEndCam.cam[0], kEndCam.cam[1] + 6.0 * std::sin(3.14159 * ts), kEndCam.cam[2] + 1115.0 * ts};
        CHECK(a.step(eye, fwd, t, pos, yaw, ev));
        if (k == 0) CHECK(std::strcmp(ev, "entered") == 0 && pos[2] == eye[2]);
        else CHECK(std::strcmp(ev, "follow") == 0);
        lag = std::sqrt((eye[0] - pos[0]) * (eye[0] - pos[0]) + (eye[1] - pos[1]) * (eye[1] - pos[1]) + (eye[2] - pos[2]) * (eye[2] - pos[2]));
        if (lag > maxlag) maxlag = lag;
        const double by = std::fabs(pos[1] - kEndCam.cam[1]);   // the +-6 bob: an entry transient of < 8, then smoothed (|H| ~0.45 at 0.5 Hz)
        if (by > maxbob) maxbob = by;
        if (k > 360 && by > maxbob_late) maxbob_late = by;
    }
    CHECK(a.enters() == 1 && a.relatches() == 0 && a.cuts() == 0);
    CHECK(maxbob < 8.0 && maxbob_late < 4.0);
    CHECK(maxlag < 260.0 && lag < 10.0);                  // the entry transient stays inside the 400-unit leash; then it rides along
    CHECK(near_eq(wrap_pi(yaw - kEndCam.rot[1]), 0.0, 1e-6));   // level yaw = the camera heading
    // a camera cut (a 3 km jump) re-latches at once; a 60-degree heading step too; a 0.4 s gap re-enters
    t += 16667;
    const double cut_eye[3] = {kEndCam.cam[0] + 3000.0, kEndCam.cam[1], kEndCam.cam[2] + 1115.0 * 20.0};
    CHECK(a.step(cut_eye, fwd, t, pos, yaw, ev) && std::strcmp(ev, "cut") == 0 && pos[0] == cut_eye[0] && a.cuts() == 1);
    const double f60[3] = {-std::sin(double(kEndCam.rot[1]) + 60.0 * kDeg), 0.0, -std::cos(double(kEndCam.rot[1]) + 60.0 * kDeg)};
    t += 16667;
    CHECK(a.step(cut_eye, f60, t, pos, yaw, ev) && std::strcmp(ev, "cut") == 0 && a.cuts() == 2);
    t += 400000;
    CHECK(a.step(cut_eye, f60, t, pos, yaw, ev) && std::strcmp(ev, "entered") == 0 && a.enters() == 2);
    // an orbit (radius 111 around a moving aircraft, 30 deg/s): no re-latch, the anchor stays near the orbit centre's track
    ProducerAnchor o; int64_t u = 0; double worst = 0;
    for (int k = 0; k <= 1800; ++k, u += 16667) {
        const double ts = double(k) / 60.0, ang = 30.0 * kDeg * ts;
        const double centre[3] = {0.0, 4000.0, 1115.0 * ts};
        const double eye[3] = {centre[0] + 111.0 * std::sin(ang), 4050.0, centre[2] + 111.0 * std::cos(ang)};
        const double f[3] = {-std::sin(ang), 0.0, -std::cos(ang)};
        CHECK(o.step(eye, f, u, pos, yaw, ev));
        const double dc = std::sqrt((pos[0] - centre[0]) * (pos[0] - centre[0]) + (pos[2] - centre[2]) * (pos[2] - centre[2]));
        if (k > 300 && dc > worst) worst = dc;
    }
    CHECK(o.relatches() == 0 && worst < 260.0);
    double h = 0;
    const double up_[3] = {0, 1, 0};
    CHECK(!heading_of(up_, h) && !o.step(up_, up_, u, pos, yaw, ev));   // no heading: no step
}

// The anchor eyes in the REAL setup_poses + substitute: the eyes stand at the anchor (the game camera + eyeOffset) on the level rig; the
// substituted POV1 record A19 decomposes to exactly those eyes; a zero offset = no offset bit for bit.
void test_stage4_eye_offset() {
    uint8_t r19[416]; make_record(kA19, r19);
    w5::Frame w; s5::Scratch sc;
    CHECK(w5::camera_setup(r19, nullptr, w, sc) == nullptr && w.camera);
    double cy = 0; CHECK(heading_of(w.pf.cam.fwd, cy));
    double rr[3], ru[3], rf[3], look[9];
    level_axes(cy, rr, ru, rf);
    rig_look(w.pf.cam.right, w.pf.cam.up, w.pf.cam.fwd, rr, ru, rf, look);
    acz::stereo::EyePose poses[2]{};
    for (int e = 0; e < 2; ++e) { poses[e].position[0] = e ? 0.032f : -0.032f; poses[e].position[1] = 1.2f; poses[e].orientation[3] = 1.0f; }
    auto run = [&](const double* off, s5::PoseFrame& p) {
        s5::Neutral n; n.set = true; n.p0[1] = 1.2;
        p = w.pf;
        CHECK(s5::setup_poses(poses, n, p, sc, false, 5.0, look, nullptr, off) == nullptr && p.valid);
    };
    s5::PoseFrame p0, pz, po;
    run(nullptr, p0);
    const double zero[3] = {0, 0, 0};
    run(zero, pz);
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < 3; ++k) CHECK(pz.eyeWorld[e][k] == p0.eyeWorld[e][k]);
        for (int i = 0; i < 16; ++i) CHECK(pz.D[e][i] == p0.D[e][i]);
    }
    const double off[3] = {-180.0, 25.0, 140.0};   // the anchor 230 units from the game camera
    run(off, po);
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < 3; ++k) CHECK(near_eq(po.eyeWorld[e][k], p0.eyeWorld[e][k] + off[k], 1e-6));
        s5::Camera c0, c1; CHECK(s5::decompose(p0.vpEye[e], c0) && s5::decompose(po.vpEye[e], c1));
        for (int k = 0; k < 3; ++k) CHECK(near_eq(c1.fwd[k], c0.fwd[k], 1e-12) && near_eq(c1.eye[k], c0.eye[k] + off[k], 1e-4));   // same axes, moved
        CHECK(std::fabs(c1.fwd[1]) < 1e-6);   // level (the A19 axes are float-derived, orthonormal to ~1e-7)
        // the substituted record: its recovered camera is the anchor eye
        uint8_t out[512];
        CHECK(w5::substitute(r19, po, unsigned(e), out) == nullptr);
        w5::Frame chk; s5::Scratch sc2;
        CHECK(w5::camera_setup(out, nullptr, chk, sc2) == nullptr);
        for (int k = 0; k < 3; ++k) CHECK(near_eq(chk.eyeAbs[k], po.eyeWorld[e][k], 2.0));   // float records of ~2.5e8: a few units
    }
}

void test_stage3_gate() {
    NcGate g;
    const uint32_t cin = request_for_stage3(Route::cinematic, opt(RigMode::level)), chase = request_for_stage3(Route::chase, opt(RigMode::level));
    CHECK(g.step(0, 0u, -1) == 0u);
    // frames come: the request stays
    int64_t last = -1;
    for (int64_t t = 0; t <= 2000; t += 11) { if (t >= 50) last = t; CHECK(g.step(t, cin, last) == cin); }
    // no first frame (the opener check refuses the camera): withdrawn after 600 ms for 1 s, then tried again
    NcGate n; uint32_t q = 0; int64_t off_at = -1, back_at = -1;
    for (int64_t t = 10000; t <= 13000; t += 11) {
        q = n.step(t, cin, 5000);
        if (!q && off_at < 0) off_at = t;
        if (q && off_at >= 0 && back_at < 0) back_at = t;
    }
    CHECK(off_at >= 10600 && off_at <= 10612 && back_at >= off_at + 1000 && back_at <= off_at + 1011 && n.fallbacks() >= 2 && !n.last_fallback_after_frames());
    // frames stop (a gap > 300 ms): withdrawn; a route change (chase) is not held back by the cinematic hold
    NcGate p;
    for (int64_t t = 0; t <= 1000; t += 11) p.step(t, cin, t);
    CHECK(p.step(1350, cin, 990) == 0u && p.fallbacks() == 1 && p.last_fallback_after_frames());
    CHECK(p.step(1361, chase, 990) == chase);   // another kind: tried at once
    CHECK(p.step(1372, 0u, 990) == 0u && p.step(1383, chase, 990) == chase);
}

void test_stage3_vignette() {
    // packing (eye-target mode_bits 20, 24..31) and the shader mirror
    CHECK(vignette_mode_bits(0.0f, false) == 0u && vignette_mode_bits(NAN, false) == 0u);
    CHECK(vignette_mode_bits(0.6f, false) == (153u << 24) && vignette_mode_bits(1.5f, false) == (255u << 24));
    CHECK(vignette_mode_bits(0.3f, true) == kVigBlinkBit);
    const uint32_t b6 = vignette_mode_bits(0.6f, false);
    CHECK((b6 & 0x000FFFFFu) == 0u);   // never touches mode / debug / occlude / model / smoke / sprite bits (0..19)
    CHECK(vignette_gain(0u, 0.0f, 0.0f) == 1.0f && vignette_gain(0u, 0.5f, 0.5f) == 1.0f);   // off: unchanged everywhere, corners too
    CHECK(vignette_gain(kVigBlinkBit, 0.5f, 0.5f) == 0.0f);                                  // blink: black
    CHECK(vignette_gain(b6, 0.5f, 0.5f) == 1.0f && vignette_gain(b6, 0.9f, 0.5f) == 1.0f);   // centre and 80 % out: clear
    CHECK(vignette_gain(b6, 0.0f, 0.0f) == 0.0f && vignette_gain(b6, 1.0f, 0.5f) < 0.75f);   // corner black, edge middle darkened
    const uint32_t b1 = vignette_mode_bits(1.0f, false);
    CHECK(vignette_gain(b1, 0.72f, 0.5f) == 1.0f && vignette_gain(b1, 0.95f, 0.5f) == 0.0f);  // full strength: a clear disc of radius 0.45
    const uint32_t b01 = vignette_mode_bits(0.01f, false);
    CHECK(vignette_gain(b01, 0.0f, 0.0f) > 0.97f);                                            // continuous at 0 (no sudden corner black)
    float prev = 1.0f; bool mono = true;
    for (int i = 0; i <= 100; ++i) {
        const float gq = vignette_gain(b6, 0.5f + 0.005f * float(i), 0.5f + 0.005f * float(i));
        if (gq > prev + 1e-6f) mono = false;
        prev = gq;
    }
    CHECK(mono);
    // who gets it: pov:3, a chase / cinematic route, a valid rig, a NO-COCKPIT composed frame
    RigOut rig; rig.valid = true; rig.vignette = 0.45f;
    CHECK(vignette_for(true, Route::chase, rig, true) == vignette_mode_bits(0.45f, false));
    CHECK(vignette_for(true, Route::cinematic, rig, true) != 0u);
    CHECK(vignette_for(false, Route::chase, rig, true) == 0u);                  // pov:2: computed, not drawn
    CHECK(vignette_for(true, Route::chase, rig, false) == 0u);                  // a cockpit frame is never darkened
    CHECK(vignette_for(true, Route::hud_only, rig, true) == 0u);                // HUD-only = the cockpit eye: no vignette
    rig.blink = true; CHECK(vignette_for(true, Route::chase, rig, true) == kVigBlinkBit);
    rig.valid = false; CHECK(vignette_for(true, Route::chase, rig, true) == 0u);
    // the rig's blink flag lasts the 150 ms after an entry
    Rig g; const Options o = opt(RigMode::level);
    vm::Geo s = kFlightChase;
    RigOut r = g.step(0, Route::chase, s, true, o);
    CHECK(r.blink);
    for (int64_t t = 11; t <= 143; t += 11) { s.cam[0] += 1.0f; r = g.step(t, Route::chase, s, true, o); CHECK(r.blink); }
    s.cam[0] += 1.0f; r = g.step(154, Route::chase, s, true, o);
    CHECK(!r.blink);
}

// ================================================================================ pov:3 A-ONLY escalation (runPOV3 / step151)
// The runPOV3 timeline: the in-mission event cutscene (E6 frozen at the eye, hud-only route, the producer refusing every opener because A is the
// cutscene camera) and the post-crash replay (E6 chase geometry, a few sporadic frames). Before: the gate cycled flat/retry 107 times. Now the
// first fallback with >= 30 E6-mismatch refusals escalates the visit to an A-only cinematic request that then holds while frames come.
void test_aonly_escalation() {
    const Options o = opt(RigMode::level);
    const uint32_t hud = request_for_stage3(Route::hud_only, o), chase = request_for_stage3(Route::chase, o);
    // the cutscene: ~6 refusals per 16.7 ms game frame (POV3: 17,544 in 12.3 s ~ 1,430/s); frames only once the producer is asked A-only
    {
        NcGate g; uint64_t mm = 0; int64_t last = -1; uint32_t q = 0; unsigned aonly = 0, off = 0;
        for (int64_t t = 295600; t <= 307800; t += 11) {
            q = g.step(t, hud, last, mm, o);
            if (req_a_only(q)) { ++aonly; last = t - 5; }   // A-only openers pass: a frame (5 ms old) every XR frame
            else { mm += 16; if (!q) ++off; }                                      // the E6 check refuses (no frame)
            if (req_a_only(q)) CHECK(req_chase(q) && req_kind(q) == NcKind::cinematic && req_level(q) && req_tc_ms(q) == 400);   // the anchor rig
        }
        CHECK(g.escalations() == 1 && g.escalated() && g.fallbacks() == 1 && off == 0 && req_a_only(q));
        CHECK(aonly > 1000);                                                      // the whole 12 s cutscene in stereo (not 107 flat/retry cycles)
        // the view ends (a cockpit frame -> route cockpit -> want 0): the escalation is dropped; the next HUD-only V tap is the E6 eye again
        CHECK(g.step(307811, 0u, last, mm, o) == 0u && !g.escalated());
        CHECK(g.step(307822, hud, last, mm, o) == hud);
    }
    // the replay: sporadic frames (an angle near the E6 chase camera) then a gap, with refusals: escalated after the short attempt
    {
        NcGate g; uint64_t mm = 0; int64_t last = -1; uint32_t q = 0;
        for (int64_t t = 458800; t <= 459500; t += 11) {
            q = g.step(t, chase, last, mm, o);
            if (req_a_only(q)) { last = t; continue; }
            mm += 10;
            if (t >= 459000 && t <= 459100) last = t;   // 10 sporadic frames, then nothing
        }
        CHECK(g.escalations() == 1 && req_a_only(q) && g.last_fallback_after_frames());
    }
    // a NORMAL chase: relative first draws are refused every frame (mismatch grows) but frames flow: never escalated; a loading hiccup after
    // 10 s (a gap) is an ordinary 1 s fallback, not an escalation
    {
        NcGate g; uint64_t mm = 0; int64_t last = -1; uint32_t q = 0;
        for (int64_t t = 0; t <= 10000; t += 11) { mm += 2; last = t; q = g.step(t, chase, last, mm, o); CHECK(q == chase); }
        for (int64_t t = 10011; t <= 10400; t += 11) { mm += 2; q = g.step(t, chase, last, mm, o); }
        CHECK(q == 0u && g.fallbacks() == 1 && g.escalations() == 0);
    }
    // pre-rendered FMV / a pause: no scene draws, so no refusals and no frames: the plain 1 s fallback cycle, never escalated (stays flat)
    {
        NcGate g; uint32_t q = 0;
        for (int64_t t = 0; t <= 8000; t += 11) q = g.step(t, hud, -1, 0, o);
        CHECK(!req_a_only(q) && g.escalations() == 0 && g.fallbacks() >= 4);
    }
    // povrig:game: the A-only request keeps the game camera
    {
        NcGate g; uint64_t mm = 0; uint32_t q = 0;
        for (int64_t t = 0; t <= 700; t += 11) { mm += 5; q = g.step(t, hud, -1, mm, opt(RigMode::game)); }
        CHECK(req_a_only(q) && req_kind(q) == NcKind::cinematic && !req_level(q));
    }
    CHECK(is_e6_mismatch("pov-camera-not-the-e6-eye") && is_e6_mismatch("pov-camera-not-the-e6-chase-camera") &&
          is_e6_mismatch("pov-camera-not-the-e6-cinematic-camera") && !is_e6_mismatch("pov-e6-not-eye") && !is_e6_mismatch(nullptr));
}

void test_aonly_opener() {
    // POV1 census: the absolute chase camera A19 eye and the E6 aircraft 85.7 units ahead of it
    const double eye[3] = {90684.4, 19234.4, 89491.8};
    vm::Geo e6 = kFlightChase;
    e6.air[0] = 90629.5f; e6.air[1] = 19214.8f; e6.air[2] = 89429.0f; e6.cam[0] = 90667.0f; e6.cam[1] = 19239.0f; e6.cam[2] = 89480.0f;
    CHECK(opener_check_a_only(eye, &e6) == nullptr);
    vm::Geo frozen_eye = kHudOnly;   // the cutscene: E6 frozen at the eye 30 km away from the cutscene camera -> still accepted (A alone)
    for (int i = 0; i < 3; ++i) {   // the HUD-only eye geometry moved next to the cutscene camera (+30 km in x)
        const float d = float(eye[i] + (i == 0 ? 30000.0 : 0.0)) - kHudOnly.air[i];
        frozen_eye.air[i] += d; frozen_eye.cam[i] += d;
    }
    CHECK(vm::classify(frozen_eye).g == vm::Geom::eye);
    CHECK(opener_check_a_only(eye, &frozen_eye) == nullptr);
    const double rel[3] = {55.0, 19.6, 62.8};   // the camera-relative player-aircraft draw (POV1 all=34)
    CHECK(std::strcmp(opener_check_a_only(rel, &e6), "pov-a-only-camera-relative") == 0);
    // runPOV3b: the INDIGO cutscene films the enemy flight far from the player (every opener was refused by the old 60,000-unit bound): accepted
    const double cutscene[3] = {eye[0] + 70000.0, eye[1] + 2000.0, eye[2] - 45000.0};
    CHECK(opener_check_a_only(cutscene, &e6) == nullptr);
    const double far_[3] = {eye[0] + 600000.0, eye[1], eye[2]};   // beyond any map: still a sanity refusal
    CHECK(std::strcmp(opener_check_a_only(far_, &e6), "pov-a-only-camera-far-from-aircraft") == 0);
    const double nan_[3] = {NAN, eye[1], eye[2]};
    CHECK(opener_check_a_only(nan_, &e6) != nullptr);
    CHECK(std::strcmp(opener_check_a_only(eye, &kZero), "pov-a-only-e6-invalid") == 0);   // menus / FMV zero block
    CHECK(std::strcmp(opener_check_a_only(eye, nullptr), "pov-no-e6-sample") == 0);
    // the same camera through the E6 check (the POV3 refusals): refused against the frozen eye
    CHECK(std::strcmp(opener_check(eye, &frozen_eye, NcKind::hud), "pov-camera-not-the-e6-eye") == 0);
}

void test_event_blink_and_cine_dump() {
    EventBlink b;
    CHECK(!b.step(0, 5));                         // the first value is the baseline
    CHECK(b.step(10, 6) && b.step(150, 6) && !b.step(161, 6));   // 150 ms after a producer anchor event
    CHECK(b.step(200, 8) && !b.step(400, 8));
    // the in-DLL cinematic dump: the 4th fresh no-cockpit frame of each cinematic visit, at most 4 per session; never chase / HUD-only
    CineDump d;
    const Options o = opt(RigMode::level);
    const uint32_t cin = request_for_stage3(Route::cinematic, o), chase = request_for_stage3(Route::chase, o);
    unsigned fired = 0;
    for (int i = 0; i < 10; ++i) fired += d.step(chase, true) ? 1u : 0u;
    CHECK(fired == 0);
    for (int i = 0; i < 10; ++i) { const bool f = d.step(cin, i % 2 == 0); if (f) { ++fired; CHECK(i == 6); } }   // fresh frames 0,2,4,6 -> the 4th
    CHECK(fired == 1);
    const uint32_t aonly = cin | kReqAOnly;
    for (int i = 0; i < 6; ++i) fired += d.step(aonly, true) ? 1u : 0u;   // an A-only visit is a new visit
    CHECK(fired == 2);
    for (int v = 0; v < 5; ++v) { d.step(0u, false); for (int i = 0; i < 6; ++i) fired += d.step(cin, true) ? 1u : 0u; }
    CHECK(fired == kCineDumpMax && d.dumps() == kCineDumpMax);
    // runPOV3b f24400 (black): the dump must never be armed on a blink frame; blink frames do not count toward the 4th frame
    CineDump e; unsigned at = 0, n = 0;
    for (int i = 0; i < 20; ++i) {
        const bool blink = i < 9;   // the producer anchor's entry blink lasts ~150 ms = 9 XR frames
        if (e.step(cin | kReqAOnly, true, blink)) { ++n; at = unsigned(i); CHECK(!blink); }
    }
    CHECK(n == 1 && at == 12);   // the 4th NON-blink frame
}

// runPOV3b: the "chase:0, hud:1" regression was the runner (the V taps fired in the menus), not the gate: a normal V cycle with frames flowing
// never escalates, whatever the E6-mismatch counter does (relative first draws are refused every frame in a real chase), and keeps its own kind.
void test_vcycle_never_escalates() {
    const Options o = opt(RigMode::level);
    const uint32_t chase = request_for_stage3(Route::chase, o), hud = request_for_stage3(Route::hud_only, o);
    NcGate g; uint64_t mm = 0; int64_t last = -1; int64_t t = 0;
    for (const uint32_t want : {chase, hud, 0u, chase}) {
        for (int k = 0; k < 1800; ++k, t += 11) {   // 20 s per view
            mm += want ? 3 : 0;
            if (want && k > 20) last = t - 4;         // the first frame ~0.2 s after the request, then every XR frame
            const uint32_t q = g.step(t, want, last, mm, o);
            if (want) { CHECK(!req_a_only(q)); if (k > 60) CHECK(q == want); }
            else CHECK(q == 0u);
        }
    }
    CHECK(g.escalations() == 0 && g.fallbacks() == 0);
    CHECK(req_kind(chase) == NcKind::chase && req_level(chase) && req_kind(hud) == NcKind::hud && !req_level(hud));
}

} // namespace

int main() {
    test_options();
    test_routes();
    test_rig_game();
    test_rig_level_spring();
    test_rig_cut();
    test_rig_anchor();
    test_vignette();
    test_census_driver();
    test_acm_poll();
    test_tracker_fields();
    test_stage2_request();
    test_stage2_opener();
    test_stage2_rig_math();
    test_stage2_relative();
    test_stage3_request();
    test_stage3_routes_and_openers();
    test_stage4_anchor_travel();
    test_stage4_eye_offset();
    test_stage3_gate();
    test_stage3_vignette();
    test_aonly_escalation();
    test_aonly_opener();
    test_event_blink_and_cine_dump();
    test_vcycle_never_escalates();
    std::printf("pov_render_offline: %u checks passed\n", g_checks);
    return 0;
}
