// VIEW-MODE offline tests (src/acz-vr/view_mode.h, flight_cycle.h keep-alive; docs/world-render/pov.md): the pov: option, the E6 geometry classifier
// on the EXACT samples logged in live batch W1 (step96 ee-camera.log `obs` lines: cockpit, runway cockpit, runway chase, takeoff-intro chase, chase at
// speed, HUD-only, mission-failed orbit, zero block), the mode machine and the flight-stage keep-alive on a replay of W1 flights 2/3 (cockpit -> V
// chase while parked -> takeoff skip -> V HUD-only -> V cockpit), the frozen cap, the mailbox and the tracker's lines. No D3D12, no game, no
// headset; fixture success says nothing about the live game.
#include "view_mode.h"
#include "flight_cycle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace acz::viewmode;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near_eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

namespace {

Geo mk(float lp, float ly, float lr, float hdg, float ax, float ay, float az, float cx, float cy, float cz, float rp, float ry, float rr) {
    Geo g;
    g.look[0] = lp; g.look[1] = ly; g.look[2] = lr; g.heading = hdg;
    g.air[0] = ax; g.air[1] = ay; g.air[2] = az; g.cam[0] = cx; g.cam[1] = cy; g.cam[2] = cz;
    g.rot[0] = rp; g.rot[1] = ry; g.rot[2] = rr;
    return g;
}
// W1 (step96) ee-camera.log obs lines, fields lp,ly,lr,hdg,ax,ay,az,cx,cy,cz,rp,ry,rr (wall clock in the comment).
const Geo kCockpit = mk(0, 0, 0, -0.785398f, -22813.07f, 4000, 130474, -22787.4f, 4003.575f, 130448.3f, -6.3988e-06f, -0.785398f, 3.97407e-13f);   // 02:48:25
const Geo kRunwayCockpit = mk(0, 0, 0, 0, 17110.4f, 1846.879f, 21348, 17110.4f, 1851.911f, 21311.87f, 0.0402351f, 0, 4.02245e-08f);            // 02:58:22
const Geo kRunwayChase = mk(0, 0, 0, 0, 17110.4f, 1846.879f, 21348, 17110.4f, 1860.808f, 21424.62f, 0.0402351f, 0, 4.02245e-08f);              // 02:58:35
const Geo kIntroChase = mk(0, 0, 0, 0, 17110.4f, 1846.879f, 21348, 17110.4f, 1858.203f, 21428.43f, 0.0651828f, 1.00554e-09f, 4.02775e-08f);    // 02:58:16
const Geo kFlightChase = mk(0, 0, 0, -0.785398f, -35754.57f, 4000, 143415.6f, -35813.58f, 4017.001f, 143474.6f, -8.04603e-06f, -0.785398f, 4.75906e-15f);   // 02:59:02
const Geo kHudOnly = mk(0, 0, 0, -0.785398f, 20067.89f, 4000, 87600.61f, 20093.56f, 4003.575f, 87574.94f, -2.85415e-06f, -0.785398f, 4.89429e-13f);   // 03:00:13
const Geo kOrbitFail = mk(-0.391883f, 3.13506f, 0, -0.785398f, 179729.5f, 4000, -72080.6f, 179800.3f, 4049.23f, -72150.76f, -0.392005f, 2.35064f, -9.89876e-10f);   // 02:52:43
const Geo kOrbitFrozen = mk(-0.392699f, 3.14159f, 0, -0.785398f, 184706.3f, 4000, -77057.41f, 184776.8f, 4049.282f, -77127.88f, -0.392699f, 2.35619f, -4.55448e-13f);   // 02:58:00

void test_options() {
    std::string rest, why; Options o;
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:1;vscreen:1;vsearly:1;fxmem:4";
    CHECK(extract_options(day, rest, o, why) && rest == day && !o.on && !o.any_token);           // byte-identical without the token
    CHECK(extract_options(day + ";pov:1", rest, o, why) && rest == day && o.on && o.any_token);
    CHECK(extract_options("auto;pov=1;vscreen:1", rest, o, why) && rest == "auto;vscreen:1" && o.on);
    CHECK(extract_options("auto;pov:0", rest, o, why) && rest == "auto" && !o.on && o.any_token);
    CHECK(!extract_options("auto;pov:4;env:1", rest, o, why) && rest == "auto;env:1" && !o.on && !o.stage2 && !o.stage3 && why.find("pov") != std::string::npos);   // removed anyway
    CHECK(extract_options("auto;pov:2;env:1", rest, o, why) && rest == "auto;env:1" && o.on && o.stage2 && !o.stage3 && describe(o) == "pov=2");   // POV-RENDER stage 2
    CHECK(extract_options("auto;pov:3;env:1", rest, o, why) && rest == "auto;env:1" && o.on && o.stage2 && o.stage3 && describe(o) == "pov=3");   // stages 3 + 4
    CHECK(extract_options("auto;pov:3;pov:2", rest, o, why) && o.on && o.stage2 && !o.stage3 && describe(o) == "pov=2");                         // last wins
    CHECK(extract_options("auto;pov:2;pov:1", rest, o, why) && o.on && !o.stage2 && describe(o) == "pov=1");                         // last wins
    CHECK(extract_options("pov:1", rest, o, why) && rest == "pov:1" && !o.on);                   // the first item is the stage id, never an option
    CHECK(extract_options("auto;povx:1;vspov:flat", rest, o, why) && rest == "auto;povx:1;vspov:flat" && !o.any_token);   // other keys untouched
    CHECK(describe(o) == "pov=0");
    CHECK(day.size() + 6 <= 255);   // the day value + ";pov:1" fits the 255-character terrain option (D-368)
}

void test_geometry() {
    GeomOut g = classify(kCockpit);
    CHECK(g.g == Geom::eye && near_eq(g.ahead, 36.30, 0.2) && near_eq(g.up, 3.575, 0.05) && near_eq(g.side, 0, 0.2) && near_eq(g.dist, 36.48, 0.1));
    g = classify(kRunwayCockpit);   // nose 2.3 deg up on the runway: the same body-frame eye offset (j35j 0,3.575,-36.3)
    CHECK(g.g == Geom::eye && near_eq(g.ahead, 36.30, 0.2) && near_eq(g.up, 3.575, 0.05));
    g = classify(kHudOnly);         // HUD-only = the cockpit eye exactly: only the missing composites tell them apart
    CHECK(g.g == Geom::eye && near_eq(g.ahead, 36.30, 0.2) && near_eq(g.up, 3.575, 0.05));
    g = classify(kRunwayChase);
    CHECK(g.g == Geom::chase && near_eq(g.ahead, -76.0, 0.3) && near_eq(g.up, 17.0, 0.2) && near_eq(g.side, 0, 0.1));
    g = classify(kIntroChase);      // the takeoff intro camera (pitch 3.7 deg, aircraft 2.3) is the chase rig too
    CHECK(g.g == Geom::chase && near_eq(g.ahead, -79.5, 0.5) && near_eq(g.up, 16.5, 0.5));
    g = classify(kFlightChase);     // at speed the chase spring stretches: 83.4 behind, 17 above
    CHECK(g.g == Geom::chase && near_eq(g.ahead, -83.4, 0.3) && near_eq(g.up, 17.0, 0.2) && near_eq(g.side, 0, 0.2));
    CHECK(classify(kOrbitFail).g == Geom::other);      // mission-failed orbit: look 3.14, camera 111 units away
    CHECK(classify(kOrbitFrozen).g == Geom::other);
    CHECK(std::strcmp(classify(kOrbitFrozen).why, "look-not-centred") == 0);
    CHECK(classify(Geo{}).g == Geom::invalid && std::strcmp(classify(Geo{}).why, "aircraft-at-origin") == 0);   // the all-zero block (menus)
    Geo nan = kCockpit; nan.cam[1] = std::nanf(""); CHECK(classify(nan).g == Geom::invalid);
    Geo ahead = kRunwayChase; ahead.cam[2] = 21348 - 80.0f; CHECK(classify(ahead).g == Geom::other);   // camera far AHEAD: not chase, not eye
    Geo turned = kRunwayChase; turned.rot[1] = 1.0f; CHECK(classify(turned).g == Geom::other);       // camera yaw far from the heading
}

// One W1 flight-2/3 replay frame (60 Hz). composite = a fresh producer frame this frame.
struct Replay {
    Machine m;
    acz::flight::FlightCycle with, without;
    int64_t last_new = -1;
    Out last{};
    bool ended_with = false, ended_without = false;
    int64_t end_with_ms = -1, end_without_ms = -1;
    void frame(int64_t now, bool composite, const Geo* s) {
        if (composite) last_new = now;
        In in; in.now_ms = now; in.composite_age_ms = last_new >= 0 ? now - last_new : -1; in.sample = s != nullptr; if (s) in.s = *s;
        last = m.step(in);
        if (with.step(now, last_new, last.alive_ms) == acz::flight::FlightCycle::Event::end && !ended_with) { ended_with = true; end_with_ms = now; }
        if (without.step(now, last_new) == acz::flight::FlightCycle::Event::end && !ended_without) { ended_without = true; end_without_ms = now; }
    }
};
Geo moved(Geo g, int64_t t) { const float k = float(t) * 0.8f; g.air[0] += k; g.cam[0] += k; g.air[2] -= k; g.cam[2] -= k; return g; }

void test_replay_w1() {
    Replay r;
    int64_t t = 0;
    int64_t chase_at = -1, hud_at = -1, cockpit_again = -1;
    for (; t < 13000; t += 16) r.frame(t, true, &kRunwayCockpit);                     // 02:58:22..35 cockpit on the runway (parked)
    CHECK(r.last.mode == Mode::cockpit && r.with.in_flight() && r.with.flight() == 1);
    for (; t < 40000; t += 16) { r.frame(t, false, &kRunwayChase); if (chase_at < 0 && r.last.mode == Mode::chase) chase_at = t; }   // V: chase, parked
    CHECK(chase_at >= 13000 + kCompositeFreshMs + kConfirmMs - 32 && chase_at <= 13000 + kCompositeFreshMs + kConfirmMs + 48);
    CHECK(r.ended_without && r.end_without_ms > 21000 && r.end_without_ms <= 21040);   // the logged misfire: flight-stage-ended 8 s after the V (02:58:43)
    CHECK(!r.ended_with && r.with.in_flight());                                          // pov:1 keeps it
    for (; t < 51000; t += 16) { const Geo c = moved(kFlightChase, t); r.frame(t, false, &c); }   // takeoff skip: chase at speed
    CHECK(r.last.mode == Mode::chase && !r.ended_with);
    for (; t < 127000; t += 16) { const Geo h = moved(kHudOnly, t); r.frame(t, false, &h); if (hud_at < 0 && r.last.mode == Mode::hud_only) hud_at = t; }
    CHECK(hud_at >= 51000 + kConfirmMs - 16 && hud_at <= 51000 + kConfirmMs + 48);
    CHECK(!r.ended_with && r.with.in_flight() && r.with.flight() == 1);
    for (; t < 130000; t += 16) { r.frame(t, true, &kHudOnly); if (cockpit_again < 0 && r.last.mode == Mode::cockpit) cockpit_again = t; }
    CHECK(cockpit_again >= 127000 && cockpit_again < 127016);   // a composite = cockpit at once
    CHECK(r.with.flight() == 1 && r.with.starts() == 1 && r.with.ends() == 0);           // ONE flight (W1 logged flights 2 and 3 + a reload)
    CHECK(r.without.flight() == 2);                                                       // without pov: ended + a second start
    CHECK(r.m.transitions() >= 4);
    CHECK(draw_cockpit_model(Mode::cockpit) && draw_cockpit_model(Mode::other) && draw_cockpit_model(Mode::unknown));
    CHECK(!draw_cockpit_model(Mode::hud_only) && !draw_cockpit_model(Mode::chase));
    CHECK(flight_view(Mode::hud_only) && flight_view(Mode::chase) && flight_view(Mode::cockpit) && !flight_view(Mode::other));
}

void test_end_rules() {
    {   // mission end: the orbit camera is 'other' -> the 8 s rule as before
        Replay r; int64_t t = 0;
        for (; t < 5000; t += 16) { const Geo c = moved(kCockpit, t); r.frame(t, true, &c); }
        for (; t < 20000; t += 16) r.frame(t, false, &kOrbitFail);
        CHECK(r.ended_with && r.end_with_ms == r.end_without_ms && r.last.mode == Mode::other);
    }
    {   // the all-zero block (menus / load) -> no keep-alive
        Replay r; int64_t t = 0;
        for (; t < 5000; t += 16) { const Geo c = moved(kCockpit, t); r.frame(t, true, &c); }
        for (; t < 20000; t += 16) { const Geo z{}; r.frame(t, false, &z); }
        CHECK(r.ended_with && r.end_with_ms == r.end_without_ms);
    }
    {   // no sample at all (pov feed absent: e6loc:probe) -> unchanged
        Replay r; int64_t t = 0;
        for (; t < 5000; t += 16) r.frame(t, true, &kCockpit);
        for (; t < 20000; t += 16) r.frame(t, false, nullptr);
        CHECK(r.ended_with && r.end_with_ms == r.end_without_ms && r.last.alive_ms < 0);
    }
    {   // frozen chase (parked or paused) is kept at most kFrozenKeepMs after the last aircraft motion (here: the first sample), then the 8 s rule
        Replay r; int64_t t = 0;
        for (; t < 5000; t += 16) r.frame(t, true, &kRunwayCockpit);
        for (; t < 120000; t += 16) r.frame(t, false, &kRunwayChase);
        CHECK(r.ended_with && r.end_with_ms > kFrozenKeepMs + 8000 - 32 && r.end_with_ms <= kFrozenKeepMs + 8000 + 32);
        CHECK(r.end_without_ms > 5000 + 8000 - 32 && r.end_without_ms <= 5000 + 8000 + 32);
    }
    {   // a short look-around in chase ('other' < 8 s) does not end the flight: the end rule counts from the last qualifying sample
        Replay r; int64_t t = 0;
        for (; t < 5000; t += 16) r.frame(t, true, &kCockpit);
        for (; t < 15000; t += 16) { const Geo c = moved(kFlightChase, t); r.frame(t, false, &c); }
        for (; t < 20000; t += 16) { Geo c = moved(kFlightChase, t); c.look[1] = 1.0f; r.frame(t, false, &c); }
        for (; t < 30000; t += 16) { const Geo c = moved(kFlightChase, t); r.frame(t, false, &c); }
        CHECK(!r.ended_with && r.ended_without);
    }
    {   // keep-alive never STARTS a flight (only a fresh composite does)
        acz::flight::FlightCycle c;
        CHECK(c.step(0, -1, 0) == acz::flight::FlightCycle::Event::none && c.step(100, -1, 100) == acz::flight::FlightCycle::Event::none && !c.in_flight());
        CHECK(c.step(200, 200, -1) == acz::flight::FlightCycle::Event::start);
        CHECK(c.step(9000, 200, 8000) == acz::flight::FlightCycle::Event::none);     // alive 8000 -> no end at 9000
        CHECK(c.step(16100, 200, 8000) == acz::flight::FlightCycle::Event::end);     // 8.1 s after the last alive
    }
}

void test_review_w2_pov() {
    // POV-1: a large finite heading (raw E6 fallback, never check_sample'd) returns promptly with invalid geometry (the wrap loop used to never end)
    for (float h : {1e30f, -1e30f, 1e10f, -1e10f, 7.0f, -7.0f, 3.4e38f}) {
        Geo g = kRunwayChase; g.heading = h;
        const GeomOut o = classify(g);
        CHECK(o.g == Geom::invalid && std::strcmp(o.why, "heading-range") == 0);
    }
    CHECK(classify(kCockpit).g == Geom::eye);
    CHECK(near_eq(detail::wrap(1e30), detail::wrap(1e30), 0) && std::fabs(detail::wrap(1e30)) <= 3.1416 && std::fabs(detail::wrap(-1e300)) <= 3.1416);
    CHECK(near_eq(detail::wrap(7.0), 7.0 - 6.283185307179586, 1e-12) && near_eq(detail::wrap(-7.0), -7.0 + 6.283185307179586, 1e-12));
    {   // through the machine: the huge heading is just an invalid sample
        Machine m; In in; in.now_ms = 0; in.sample = true; in.s = kRunwayChase; in.s.heading = 1e30f;
        CHECK(m.step(in).geom.g == Geom::invalid);
    }
    // POV-2: repeated short invalidations of an unchanged sample do not extend the frozen deadline
    {
        Machine m; int64_t t = 0; bool alive_after = false;
        In in; in.composite_age_ms = -1;
        for (; t < 5000; t += 16) { in.now_ms = t; in.sample = true; in.s = kRunwayChase; m.step(in); }
        int64_t next_gap = 60000;
        for (; t < 200000; t += 16) {
            in.now_ms = t;
            in.sample = !(t >= next_gap && t < next_gap + 48);   // 3 missing frames every 60 s
            if (t >= next_gap + 48) next_gap += 60000;
            in.s = kRunwayChase;
            const Out o = m.step(in);
            if (t > kFrozenKeepMs + 2000 && o.alive_ms > kFrozenKeepMs + 1000) alive_after = true;
        }
        CHECK(!alive_after);   // the deadline stays at the first sample (t=0) + kFrozenKeepMs
    }
    {   // a real position change after an outage does advance it
        Machine m; In in; in.composite_age_ms = -1; in.sample = true; in.s = kRunwayChase; in.now_ms = 0;
        for (int64_t t = 0; t < 5000; t += 16) { in.now_ms = t; m.step(in); }
        in.now_ms = 5016; in.sample = false; m.step(in);
        in.now_ms = 80000; in.sample = true; in.s = moved(kRunwayChase, 100); const Out o = m.step(in);
        CHECK(o.alive_ms == 80000);
    }
}

void test_mailbox_and_tracker() {
    ViewMailbox mb; Geo g; uint64_t n = 0;
    CHECK(!mb.read(g, n));
    mb.publish(kRunwayChase);
    CHECK(mb.read(g, n) && n == 1 && std::memcmp(&g, &kRunwayChase, sizeof g) == 0);
    mb.invalidate();
    CHECK(!mb.read(g, n) && n == 2 && mb.published() == 2);
    Tracker tr; ViewMailbox box;
    Tracker::Step s = tr.step(0, -1, box);
    CHECK(s.line && std::strstr(s.text, "event=summary-pov;") && s.alive_ms < 0);   // the first call writes the summary
    for (int64_t t = 16; t < 2000; t += 16) { box.publish(kRunwayCockpit); s = tr.step(t, 0, box); }
    CHECK(s.mode == Mode::cockpit);
    bool saw = false;
    for (int64_t t = 2000; t < 3000; t += 16) {
        box.publish(kRunwayChase); s = tr.step(t, t - 1990, box);
        if (s.line && std::strstr(s.text, "event=pov-mode;from=cockpit;to=chase;geom=chase;")) { saw = true; CHECK(s.important && std::strstr(s.text, "cockpit-model=gated")); }
    }
    CHECK(saw && s.mode == Mode::chase && s.alive_ms >= 2900);
    for (int64_t t = 3000; t < 4000; t += 16) s = tr.step(t, t - 1990, box);   // no new publication: stale after 500 ms -> no sample -> other
    CHECK(tr.machine().mode() == Mode::other);
    CHECK(std::string(s.text).size() < sizeof s.text - 1);
}

} // namespace

int main() {
    test_options();
    test_geometry();
    test_replay_w1();
    test_end_rules();
    test_review_w2_pov();
    test_mailbox_and_tracker();
    std::printf("view_mode_offline: %u checks passed\n", g_checks);
    return 0;
}
