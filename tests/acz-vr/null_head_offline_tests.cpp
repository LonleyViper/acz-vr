// NULL-HEAD offline fixtures (docs/world-render/null-head-look.md; TEST-ONLY feature): the nh:/nhs: tokens inside xr_terrain_stage, the head.now
// command parser, the mailbox, the slew/hold/return controller (settle after a session start and a LOCAL generation change, stall clamp, summary
// cadence), the pose composition (sign convention against recenter.h / look_drive_keys.h head yaw, pivot, IPD kept, identity byte-identical) and the
// Owner seam (xr_owner::PoseFilter: the snapshot AND the consumer see the same turned views; attach refused after use). No OS object, no game.
#include "null_head.h"
#include "recenter.h"
#include "xr_owner.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace nh = acz::nullhead;
using namespace acz::xr_owner;
static int g_failures = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near_(double a, double b, double e = 1e-4) { return std::fabs(a - b) <= e; }

static void options() {
    std::string rest, why; nh::Options o;
    CHECK(nh::extract_options("auto;vrn:1;nh:1;rc:1", rest, o, why) && o.on && o.any_token && near_(o.slew_dps, 90.0) && rest == "auto;vrn:1;rc:1");
    CHECK(nh::extract_options("auto;nh:1;nhs:180", rest, o, why) && o.on && near_(o.slew_dps, 180.0) && rest == "auto");
    CHECK(nh::extract_options("auto;rc:1", rest, o, why) && !o.on && !o.any_token && rest == "auto;rc:1");   // absent = off, value unchanged
    CHECK(nh::extract_options("auto;nh:0", rest, o, why) && !o.on && o.any_token && rest == "auto");
    CHECK(nh::extract_options("nh:1", rest, o, why) && !o.on && rest == "nh:1");                              // the first item is the stage id
    CHECK(!nh::extract_options("auto;nh:2", rest, o, why) && !o.on && o.any_token && rest == "auto" && !why.empty());
    CHECK(!nh::extract_options("auto;nh:1;nhs:5", rest, o, why) && !o.on && rest == "auto");                 // too slow: the whole option off
    CHECK(!nh::extract_options("auto;nh:1;nhs:abc", rest, o, why) && !o.on);
    CHECK(nh::extract_options("auto;nhx:1", rest, o, why) && rest == "auto;nhx:1");                          // not ours: left for the other parsers
    CHECK(nh::describe(nh::Options{}) == "null-head=0;slew-dps=90");
    CHECK(nh::format_config(o, true, "").find("event=null-head-config;ok=1") != std::string::npos);
}

static void commands() {
    nh::Command c; std::string why;
    const char* a = "90 0 3";
    CHECK(nh::parse_command(a, std::strlen(a), c, why) && c.yaw == 90 && c.pitch == 0 && c.hold_s == 3 && c.roll == 0);
    const char* b = "-90,60,2.5,10\r\n";
    CHECK(nh::parse_command(b, std::strlen(b), c, why) && c.yaw == -90 && c.pitch == 60 && c.hold_s == 2.5 && c.roll == 10);
    const char* z = "0 0";
    CHECK(nh::parse_command(z, std::strlen(z), c, why) && c.yaw == 0 && c.hold_s == 0);
    const char* behind = "180 0 4";
    CHECK(nh::parse_command(behind, std::strlen(behind), c, why) && c.yaw == 180);
    const char* bad[] = {"", "90", "abc 0", "181 0", "0 90", "0 0 -1", "0 0 601", "0 0 1 181", "1 2 3 4 5", "90x 0"};
    for (const char* s : bad) { CHECK(!nh::parse_command(s, std::strlen(s), c, why)); CHECK(!why.empty()); }
    const char* nul = "45 10\0 99 99";   // stops at the first NUL
    CHECK(nh::parse_command(nul, 12, c, why) && c.yaw == 45 && c.pitch == 10);
    CHECK(nh::format_command(c, true, "", "deleted").find("stage=command;yaw=45.0;pitch=10.0") != std::string::npos);
    CHECK(nh::format_command(c, false, "pitch outside -89..89", "deleted").find("stage=refused;detail=pitch outside") != std::string::npos);
}

static void mailbox() {
    nh::Mailbox m; uint32_t seen = m.sequence(); nh::Command out;
    CHECK(!m.read(seen, out));
    nh::Command c; c.yaw = -37.5; c.pitch = 12.25; c.hold_s = 1.5; c.roll = 3;
    m.post(c);
    CHECK(m.read(seen, out) && near_(out.yaw, -37.5) && near_(out.pitch, 12.25) && near_(out.hold_s, 1.5) && near_(out.roll, 3));
    CHECK(!m.read(seen, out));   // consumed once
    m.post(c); m.post(nh::Command{});
    CHECK(m.read(seen, out) && out.yaw == 0 && out.hold_s == 0);   // the latest command wins
}

// Steps the controller at 10 ms until `until_ms`; returns the last output.
static nh::StepOut run_to(nh::Controller& ctl, int64_t& t, int64_t until_ms, uint32_t gen, const nh::Mailbox* m, unsigned* reached = nullptr, unsigned* home = nullptr, unsigned* returning = nullptr) {
    nh::StepOut o;
    for (; t <= until_ms; t += 10) { o = ctl.step(t, gen, m); if (reached && o.reached) ++*reached; if (home && o.home) ++*home; if (returning && o.returning) ++*returning; }
    t -= 10;
    return o;
}

static void slew() {
    nh::Mailbox m; nh::Controller ctl(90.0, m.sequence());
    int64_t t = 1000;
    nh::StepOut o = ctl.step(t, 0, &m);
    CHECK(o.settle_start && std::strcmp(o.settle_reason, "session-start") == 0 && o.settling && nh::is_zero(o.cur));
    nh::Command c; c.yaw = 90; c.hold_s = 2; m.post(c);
    t += 10; o = ctl.step(t, 0, &m);
    CHECK(o.command && o.settling && nh::is_zero(o.cur) && o.phase == nh::Phase::slewing && near_(o.target.yaw, 90));   // settle: identity first
    o = run_to(ctl, t, 1990, 0, &m);
    CHECK(o.settling && nh::is_zero(o.cur));
    o = run_to(ctl, t, 2500, 0, &m);   // ~0.5 s of slew at 90 deg/s
    CHECK(!o.settling && o.cur.yaw > 40 && o.cur.yaw < 50 && o.phase == nh::Phase::slewing);
    unsigned reached = 0, home = 0, ret = 0;
    o = run_to(ctl, t, 3100, 0, &m, &reached);
    CHECK(reached == 1 && o.phase == nh::Phase::holding && o.cur.yaw == 90);
    o = run_to(ctl, t, 4980, 0, &m, &reached, &home, &ret);
    CHECK(o.phase == nh::Phase::holding && ret == 0);   // hold counted from ARRIVAL (2.99 s + 2 s)
    o = run_to(ctl, t, 5100, 0, &m, &reached, &home, &ret);
    CHECK(ret == 1 && o.phase == nh::Phase::returning && nh::is_zero(o.target));
    o = run_to(ctl, t, 6200, 0, &m, &reached, &home, &ret);
    CHECK(home == 1 && o.phase == nh::Phase::idle && nh::is_zero(o.cur) && reached == 1);
    CHECK(ctl.commands() == 1 && ctl.turned_frames() > 100);

    // Combined yaw+pitch: both axes arrive together (the largest error sets the time).
    c = nh::Command{}; c.yaw = -90; c.pitch = 45; m.post(c);   // hold 0: stays until the next command
    o = run_to(ctl, t, t + 500, 0, &m);
    CHECK(near_(o.cur.pitch / o.cur.yaw, -0.5, 1e-6));
    o = run_to(ctl, t, t + 1000, 0, &m);
    CHECK(o.cur.yaw == -90 && o.cur.pitch == 45 && o.phase == nh::Phase::holding);
    o = run_to(ctl, t, t + 20000, 0, &m);
    CHECK(o.phase == nh::Phase::holding && o.cur.yaw == -90);   // indefinite hold

    // A LOCAL generation change: identity at once, settle, then back to the target.
    t += 10; o = ctl.step(t, 1, &m);
    CHECK(o.settle_start && std::strcmp(o.settle_reason, "local-generation-change") == 0 && nh::is_zero(o.cur) && ctl.generation_changes() == 1);
    o = run_to(ctl, t, t + 900, 1, &m);
    CHECK(o.settling && nh::is_zero(o.cur));
    o = run_to(ctl, t, t + 1300, 1, &m);
    CHECK(!o.settling && o.cur.yaw == -90 && o.cur.pitch == 45);

    // "0 0" returns; a stall of 5 s moves at most kMaxStepMs of slew.
    m.post(nh::Command{});
    t += 10; o = ctl.step(t, 1, &m);
    CHECK(o.command && o.phase == nh::Phase::returning);
    t += 5000; o = ctl.step(t, 1, &m);
    CHECK(near_(o.cur.yaw, -90 + 0.9 + 22.5, 1e-6));   // 10 ms on the command frame, then the 250 ms clamp (not 5 s)
    o = run_to(ctl, t, t + 2000, 1, &m);
    CHECK(nh::is_zero(o.cur) && o.phase == nh::Phase::idle);
}

static void summary_cadence() {
    nh::Controller ctl(90.0); int64_t t = 0; unsigned n = 0; nh::StepOut o;
    for (; t <= 20100; t += 11) { o = ctl.step(t, 0, nullptr); if (o.summary) { ++n; CHECK(nh::format_summary(o, ctl, 0).find("xr-stereo;event=summary-null-head;cur=0.0,0.0,0.0;") == 0); } }
    CHECK(n == 4);   // ~5, 10, 15, 20 s, also with zero counters
    CHECK(nh::format_change(o, t).empty());
}

static void make_view(ViewSample& v, HeadSample& h, double cx, double cy, double cz) {
    v = ViewSample{}; h = HeadSample{};
    v.view_count = 2; v.view_state_flags = valid_bits | tracked_bits;
    for (int e = 0; e < 2; ++e) {
        v.eyes[e].pose.position[0] = float(cx + (e ? 0.032 : -0.032)); v.eyes[e].pose.position[1] = float(cy); v.eyes[e].pose.position[2] = float(cz);
        v.eyes[e].fov = {-0.9f, 0.8f, 0.85f, -0.95f};
    }
    h.location_flags = valid_bits | tracked_bits;
    h.pose.position[0] = float(cx); h.pose.position[1] = float(cy); h.pose.position[2] = float(cz);
}
static void forward(const Pose& p, double f[3]) { const double q[4] = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]}; const double z[3] = {0, 0, -1}; nh::rotate(q, z, f); }

static void compose() {
    ViewSample v, v0; HeadSample h, h0;
    make_view(v, h, 0.1, 1.2, -0.3); v0 = v; h0 = h;
    CHECK(!nh::apply(nh::Angles{}, true, v, true, h) && std::memcmp(&v, &v0, sizeof v) == 0 && std::memcmp(&h, &h0, sizeof h) == 0);   // identity untouched
    // yaw +90 = LEFT: forward (0,0,-1) -> (-1,0,0); the left eye moves behind the head (+z), the right eye in front (-z); the head point stays.
    CHECK(nh::apply(nh::Angles{90, 0, 0}, true, v, true, h));
    double f[3]; forward(v.eyes[0].pose, f);
    CHECK(near_(f[0], -1) && near_(f[1], 0) && near_(f[2], 0));
    CHECK(near_(v.eyes[0].pose.position[0], 0.1) && near_(v.eyes[0].pose.position[2], -0.3 + 0.032));
    CHECK(near_(v.eyes[1].pose.position[0], 0.1) && near_(v.eyes[1].pose.position[2], -0.3 - 0.032));
    CHECK(near_(h.pose.position[0], 0.1) && near_(h.pose.position[1], 1.2) && near_(h.pose.position[2], -0.3));
    const double ipd = std::hypot(v.eyes[0].pose.position[0] - v.eyes[1].pose.position[0], v.eyes[0].pose.position[2] - v.eyes[1].pose.position[2]);
    CHECK(near_(ipd, 0.064));
    CHECK(v.eyes[0].fov.left == v0.eyes[0].fov.left && v.eyes[1].fov.down == v0.eyes[1].fov.down);   // FOV untouched
    // Same heading as the producer / recenter.h measure it: +90 deg.
    float pos[2][3], ori[2][4];
    for (int e = 0; e < 2; ++e) { for (int k = 0; k < 3; ++k) pos[e][k] = v.eyes[e].pose.position[k]; for (int k = 0; k < 4; ++k) ori[e][k] = v.eyes[e].pose.orientation[k]; }
    const acz::recenter::Head rh = acz::recenter::head_of(pos, ori);
    CHECK(rh.ok && near_(rh.yaw, 90 * nh::kDegToRad));
    // pitch +60 after yaw -90: looks up-right.
    make_view(v, h, 0, 1.2, 0);
    nh::apply(nh::Angles{-90, 60, 0}, true, v, true, h);
    forward(v.eyes[1].pose, f);
    CHECK(near_(f[0], std::cos(60 * nh::kDegToRad)) && near_(f[1], std::sin(60 * nh::kDegToRad)) && near_(f[2], 0));
    forward(h.pose, f);
    CHECK(near_(f[1], std::sin(60 * nh::kDegToRad)));
    // yaw 180 = behind.
    make_view(v, h, 0, 1.2, 0);
    nh::apply(nh::Angles{180, 0, 0}, true, v, true, h);
    forward(v.eyes[0].pose, f);
    CHECK(near_(f[2], 1) && near_(v.eyes[0].pose.position[0], 0.032));
    // roll + = left ear down: the head's right axis goes up.
    make_view(v, h, 0, 1.2, 0);
    nh::apply(nh::Angles{0, 0, 30}, true, v, true, h);
    CHECK(v.eyes[1].pose.position[1] > 1.2f && v.eyes[0].pose.position[1] < 1.2f);
    // Composition on an already turned real head: R_synthetic * q (left-multiplied in LOCAL).
    make_view(v, h, 0, 1.2, 0);
    const double s45 = std::sin(22.5 * nh::kDegToRad), c45 = std::cos(22.5 * nh::kDegToRad);
    for (int e = 0; e < 2; ++e) { v.eyes[e].pose.orientation[1] = float(s45); v.eyes[e].pose.orientation[3] = float(c45); }
    nh::apply(nh::Angles{45, 0, 0}, true, v, false, h);
    for (int e = 0; e < 2; ++e) { for (int k = 0; k < 3; ++k) pos[e][k] = v.eyes[e].pose.position[k]; for (int k = 0; k < 4; ++k) ori[e][k] = v.eyes[e].pose.orientation[k]; }
    CHECK(near_(acz::recenter::head_of(pos, ori).yaw, 90 * nh::kDegToRad));
    // No head: the pivot is the eye midpoint (the midpoint stays put).
    make_view(v, h, 0.5, 1.0, 2.0);
    CHECK(nh::apply(nh::Angles{70, 20, 0}, true, v, false, h));
    CHECK(near_(0.5 * (v.eyes[0].pose.position[0] + v.eyes[1].pose.position[0]), 0.5) && near_(0.5 * (v.eyes[0].pose.position[2] + v.eyes[1].pose.position[2]), 2.0));
    CHECK(h.pose.orientation[3] == 1.0f && h.pose.position[0] == 0.5f);   // head not located: untouched
    // Nothing usable: no change.
    make_view(v, h, 0, 0, 0); v.eyes[0].pose.position[0] = NAN; v0 = v;
    CHECK(!nh::apply(nh::Angles{90, 0, 0}, true, v, false, h));
    CHECK(!nh::apply(nh::Angles{90, 0, 0}, false, v, false, h));
    for (int e = 0; e < 2; ++e) for (int k = 0; k < 4; ++k) { const float a = v.eyes[e].pose.orientation[k], b = v0.eyes[e].pose.orientation[k]; CHECK(a == b); }
}

// ---- Owner seam -------------------------------------------------------------------------------------------------------------------------------
struct Clk final : Clock {
    std::atomic<int64_t> t{0};
    int64_t now_ms() override { return t.load(); }
    void sleep_ms(int64_t ms) override { t += ms; }
};
static Event state(SessionState s) { Event e; e.type = EventType::session_state; e.state = s; return e; }
struct Rt final : Runtime {
    Clk& clock; std::deque<Event> events; std::mutex m;
    ViewSample views; HeadSample head; unsigned waits = 0; int64_t frame_ms = 40;
    explicit Rt(Clk& c) : clock(c) { make_view(views, head, 0, 1.2, 0); }
    void push(const Event& e) { std::lock_guard<std::mutex> g(m); events.push_back(e); }
    Rc create_instance() override { return Rc::ok; }
    Rc query_system(SystemFacts& s) override { s.hmd_system = true; s.primary_stereo = true; s.view_count = 2; s.blend_count = 1; s.blend_modes[0] = blend_opaque; return Rc::ok; }
    Rc graphics_requirements(GraphicsRequirements& r) override { r = {7, 3, 0xb000}; return Rc::ok; }
    Rc query_binding(BindingFacts& b) override { b = {true, true, 7, 3, 0xc100, true, true}; return Rc::ok; }
    Rc create_session() override { push(state(SessionState::idle)); push(state(SessionState::ready)); return Rc::ok; }
    Rc query_spaces(SpaceFacts& s) override { s = {true, true}; return Rc::ok; }
    Rc create_local_space() override { return Rc::ok; }
    Rc create_view_space() override { return Rc::ok; }
    Rc poll_event(Event& e) override { std::lock_guard<std::mutex> g(m); if (events.empty()) return Rc::event_unavailable; e = events.front(); events.pop_front(); return Rc::ok; }
    Rc begin_session() override { push(state(SessionState::synchronized)); push(state(SessionState::visible)); return Rc::ok; }
    Rc wait_frame(FrameTiming& t) override { clock.t += frame_ms; ++waits; t.predicted_time = 1000000000 + int64_t(waits) * 11111111; t.predicted_period = 11111111; t.should_render = true; return Rc::ok; }
    Rc begin_frame() override { return Rc::ok; }
    Rc locate_views(int64_t, ViewSample& v) override { v = views; return Rc::ok; }
    Rc locate_head(int64_t, HeadSample& h) override { h = head; return Rc::ok; }
    Rc end_frame_zero_layers(int64_t, uint32_t) override { return Rc::ok; }
    Rc request_exit() override { push(state(SessionState::stopping)); return Rc::ok; }
    Rc end_session() override { return Rc::ok; }
    Rc destroy_view_space() override { return Rc::ok; }
    Rc destroy_local_space() override { return Rc::ok; }
    Rc destroy_session() override { return Rc::ok; }
    Rc release_binding() override { return Rc::ok; }
    Rc destroy_instance() override { return Rc::ok; }
};
struct Filter final : PoseFilter {
    nh::Mailbox& mb; nh::Controller ctl; unsigned calls = 0, rotated = 0; uint32_t last_gen = 99;
    explicit Filter(nh::Mailbox& m) : mb(m), ctl(90.0, m.sequence()) {}
    void apply(int64_t now, uint32_t gen, bool vok, ViewSample& v, bool hok, HeadSample& h) noexcept override {
        ++calls; last_gen = gen;
        const nh::StepOut o = ctl.step(now, gen, &mb);
        if (nh::apply(o.cur, vok, v, hok, h)) ++rotated;
    }
};
struct Consumer final : FrameConsumer {   // also reads the snapshot published for the same frame (the slot is retracted after the run)
    ViewSample last; unsigned frames = 0; Owner* owner = nullptr; PoseSnapshot snap; bool snap_ok = false;
    bool setup(Gate&) override { return true; }
    void frame(Gate&, const FrameInfo& i, LayerPlan&) override { last = i.views; ++frames; if (owner) snap_ok = owner->try_read_pose(snap); }
    void ended(Gate&, Rc, bool, int64_t) override {}
    bool retire(Gate&) override { return true; }
    bool failed() const noexcept override { return false; }
};

static void owner_seam() {
    auto clk = std::make_shared<Clk>(); auto rt = std::make_shared<Rt>(*clk);
    nh::Mailbox mb;
    auto filter = std::make_shared<Filter>(mb);
    auto consumer = std::make_shared<Consumer>();
    Config cfg; cfg.enabled = true;   // ladder mode: 120 frames x 40 ms = 4.8 s
    auto owner = std::make_shared<Owner>(rt, clk, cfg, Limits{});
    CHECK(owner->set_pose_filter(filter));
    CHECK(!owner->set_pose_filter(filter));   // only one
    CHECK(owner->set_consumer(consumer));
    consumer->owner = owner.get();
    nh::Command c; c.yaw = 90; mb.post(c);    // posted after the filter was made: taken on the first frame
    const Report r = owner->run();
    CHECK(r.complete && r.success && r.begun == 120);
    CHECK(filter->calls == r.begun && consumer->frames == r.begun && filter->last_gen == 0);
    CHECK(filter->rotated > 0 && filter->rotated < r.begun);   // identity while settling, then turned
    const PoseSnapshot& s = consumer->snap;
    CHECK(consumer->snap_ok && s.eyes_valid() && s.head_valid() && s.frame_index == r.begun);
    double f[3]; forward(s.eyes[0].pose, f);
    CHECK(near_(f[0], -1) && near_(f[2], 0));                  // the published snapshot is turned left 90 deg
    CHECK(std::memcmp(&consumer->last.eyes, &s.eyes, sizeof s.eyes) == 0);   // the consumer saw exactly the published views
    forward(s.head, f);
    CHECK(near_(f[0], -1));
    CHECK(near_(rt->views.eyes[0].pose.orientation[3], 1.0));  // the runtime's own sample is never touched
    CHECK(!owner->set_pose_filter(std::make_shared<Filter>(mb)));   // refused once used

    // No filter: byte-identical raw poses.
    auto clk2 = std::make_shared<Clk>(); auto rt2 = std::make_shared<Rt>(*clk2);
    auto owner2 = std::make_shared<Owner>(rt2, clk2, cfg, Limits{});
    auto consumer2 = std::make_shared<Consumer>();
    CHECK(owner2->set_consumer(consumer2));
    consumer2->owner = owner2.get();
    CHECK(owner2->run().success);
    CHECK(consumer2->snap_ok && std::memcmp(&consumer2->snap.eyes[0].pose, &rt2->views.eyes[0].pose, sizeof(Pose)) == 0 &&
          std::memcmp(&consumer2->snap.head, &rt2->head.pose, sizeof(Pose)) == 0);
}

int main() {
    options(); commands(); mailbox(); slew(); summary_cadence(); compose(); owner_seam();
    std::printf("null_head_offline: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
