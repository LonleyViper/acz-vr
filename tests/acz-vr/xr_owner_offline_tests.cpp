// Offline tests for the in-proxy XR frame owner. Fully injected fake runtime
// and fake clock: no OpenXR loader, no D3D12 device, no game, no headset.
#include "xr_owner.h"
#include "xr_start.h"
#include "xr_owner_registry.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <malloc.h>
#include <new>

// R-5 measurement: live heap bytes of this test process (counting global operator new/delete; _msize of the malloc block).
std::atomic<int64_t> g_live_bytes{0};
void* operator new(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    g_live_bytes.fetch_add(int64_t(_msize(p)), std::memory_order_relaxed);
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return operator new(n); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return operator new(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { if (!p) return; g_live_bytes.fetch_sub(int64_t(_msize(p)), std::memory_order_relaxed); std::free(p); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }

using namespace acz::xr_owner;

namespace {
unsigned checks = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { std::cerr << "FAIL " << what << '\n'; std::exit(1); }
}

struct FakeClock final : Clock {
    std::atomic<int64_t> t{0};
    int64_t now_ms() override { return t.load(); }
    void sleep_ms(int64_t ms) override {
        check(ms >= 0 && ms <= Limits{}.poll_ms, "sleep bounded to [0, poll_ms]");
        t += ms;
    }
};

Event state_event(SessionState s, bool same = true) {
    Event e; e.type = EventType::session_state; e.state = s; e.same_session = same; return e;
}

// Scripted fake runtime. Every call is logged under a mutex so a test thread
// can observe it while the owner thread is blocked inside another call.
struct Fake final : Runtime {
    FakeClock& clock;
    explicit Fake(FakeClock& c) : clock(c) {
        sys.hmd_system = true; sys.primary_stereo = true; sys.view_count = 2;
        sys.blend_count = 1; sys.blend_modes[0] = blend_opaque;
        req = {7, 3, 0xb000};
        bind = {true, true, 7, 3, 0xc100, true, true};
        spaces = {true, true};
        views.view_count = 2; views.view_state_flags = valid_bits | tracked_bits;
        for (int i = 0; i < 2; ++i) {
            views.eyes[i].pose.position[0] = i ? 0.032f : -0.032f;
            views.eyes[i].pose.position[1] = 1.2f;
            views.eyes[i].fov = {-0.9f, 0.8f, 0.85f, -0.95f};
        }
        head.location_flags = valid_bits | tracked_bits;
        head.pose.position[1] = 1.2f;
        head.pose.orientation[1] = 0.38268343f; head.pose.orientation[3] = 0.92387953f; // 45 deg yaw
    }
    mutable std::mutex m;
    std::vector<std::string> log;
    std::map<std::string, std::deque<Rc>> script; // per-op scripted results, consumed in order
    std::map<std::string, std::deque<int32_t>> xr_script; // XR-START: per-op raw XrResult, consumed in order
    int32_t last_xr = kNoXrResult;
    unsigned takes = 0;
    int32_t take_last_xr_result() override { std::lock_guard<std::mutex> g(m); ++takes; const int32_t r = last_xr; last_xr = kNoXrResult; return r; }
    std::map<std::string, std::function<void()>> hook; // runs inside the call
    std::map<std::string, std::string> throws;
    std::deque<Event> events;
    SystemFacts sys; GraphicsRequirements req; BindingFacts bind; SpaceFacts spaces;
    ViewSample views; HeadSample head;
    std::vector<FrameTiming> timings; // per wait index; default below
    bool ready_after_session = true, stopping_after_exit = true;
    int64_t frame_ms = 11, period_ns = 11111111, time_ns = 1000000000;
    bool should_render = true;
    unsigned waits = 0;

    unsigned count(const std::string& op) const {
        std::lock_guard<std::mutex> g(m);
        unsigned n = 0; for (auto& s : log) n += s == op; return n;
    }
    std::vector<std::string> calls() const { std::lock_guard<std::mutex> g(m); return log; }
    Rc enter(const std::string& op) {
        std::function<void()> h; Rc rc = Rc::ok;
        {
            std::lock_guard<std::mutex> g(m);
            log.push_back(op);
            auto it = script.find(op);
            if (it != script.end() && !it->second.empty()) { rc = it->second.front(); it->second.pop_front(); }
            auto xi = xr_script.find(op);
            if (xi != xr_script.end() && !xi->second.empty()) { last_xr = xi->second.front(); xi->second.pop_front(); }
            else last_xr = rc == Rc::ok ? 0 : kNoXrResult;
            auto hi = hook.find(op);
            if (hi != hook.end()) h = hi->second;
            if (throws.count(op)) throw std::runtime_error(op);
        }
        if (h) h();
        return rc;
    }
    Rc create_instance() override { return enter("create_instance"); }
    Rc query_system(SystemFacts& o) override { Rc rc = enter("query_system"); o = sys; return rc; }
    Rc graphics_requirements(GraphicsRequirements& o) override { Rc rc = enter("graphics_requirements"); o = req; return rc; }
    Rc query_binding(BindingFacts& o) override { Rc rc = enter("query_binding"); o = bind; return rc; }
    Rc create_session() override {
        Rc rc = enter("create_session");
        if (succeeded(rc) && ready_after_session) {
            std::lock_guard<std::mutex> g(m);
            events.push_back(state_event(SessionState::idle));
            events.push_back(state_event(SessionState::ready));
        }
        return rc;
    }
    Rc query_spaces(SpaceFacts& o) override { Rc rc = enter("query_spaces"); o = spaces; return rc; }
    Rc create_local_space() override { return enter("create_local_space"); }
    Rc create_view_space() override { return enter("create_view_space"); }
    Rc poll_event(Event& e) override {
        Rc rc = enter("poll_event");
        if (rc != Rc::ok) return rc;
        std::lock_guard<std::mutex> g(m);
        if (events.empty()) return Rc::event_unavailable;
        e = events.front(); events.pop_front();
        return Rc::ok;
    }
    Rc begin_session() override {
        Rc rc = enter("begin_session");
        if (succeeded(rc)) {
            std::lock_guard<std::mutex> g(m);
            events.push_back(state_event(SessionState::synchronized));
            events.push_back(state_event(SessionState::visible));
        }
        return rc;
    }
    Rc wait_frame(FrameTiming& t) override {
        Rc rc = enter("wait_frame");
        clock.t += frame_ms;
        unsigned i = waits++;
        if (i < timings.size()) t = timings[i];
        else { t.predicted_time = time_ns + int64_t(i + 1) * period_ns; t.predicted_period = period_ns; t.should_render = should_render; }
        return rc;
    }
    Rc begin_frame() override { return enter("begin_frame"); }
    Rc locate_views(int64_t, ViewSample& v) override { Rc rc = enter("locate_views"); v = views; return rc; }
    Rc locate_head(int64_t, HeadSample& h) override { Rc rc = enter("locate_head"); h = head; return rc; }
    std::vector<std::pair<int64_t, uint32_t>> ends;
    Rc end_frame_zero_layers(int64_t t, uint32_t blend) override {
        Rc rc = enter("end_frame");
        std::lock_guard<std::mutex> g(m);
        ends.push_back({t, blend});
        return rc;
    }
    Rc request_exit() override {
        Rc rc = enter("request_exit");
        if (succeeded(rc) && stopping_after_exit) { std::lock_guard<std::mutex> g(m); events.push_back(state_event(SessionState::stopping)); }
        return rc;
    }
    Rc end_session() override { return enter("end_session"); }
    Rc destroy_view_space() override { return enter("destroy_view_space"); }
    Rc destroy_local_space() override { return enter("destroy_local_space"); }
    Rc destroy_session() override { return enter("destroy_session"); }
    Rc release_binding() override { return enter("release_binding"); }
    Rc destroy_instance() override { return enter("destroy_instance"); }
};

struct Rig {
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::shared_ptr<Fake> rt = std::make_shared<Fake>(*clock);
    Limits lim{};
    std::shared_ptr<Owner> make(bool enabled = true) {
        Config c; c.enabled = enabled;
        return std::make_shared<Owner>(rt, clock, c, lim);
    }
};

const std::vector<std::string> teardown_order{"destroy_view_space", "destroy_local_space", "destroy_session",
    "release_binding", "destroy_instance"};

bool ends_with_teardown(const std::vector<std::string>& calls) {
    if (calls.size() < teardown_order.size()) return false;
    return std::equal(teardown_order.begin(), teardown_order.end(), calls.end() - teardown_order.size());
}
bool all_released(const Report& r) {
    return !r.instance_held && !r.session_held && !r.session_running && !r.local_space_held && !r.view_space_held && !r.binding_held;
}
void forbid_destroy(Fake& f, const std::string& what) {
    for (auto& d : teardown_order) check(f.count(d) == 0, what + ": no " + d);
    check(f.count("end_session") == 0, what + ": no end_session");
}

// ------------------------------------------------------------------ tests ---
void disabled_default() {
    Config defaults;
    check(!defaults.enabled, "Config default OFF");
    Rig g;
    auto o = g.make(false);
    check(launch(o) == Launch::disabled, "launch refuses when OFF");
    auto r = o->run();
    check(g.rt->calls().empty(), "OFF: zero runtime calls");
    check(g.clock->t == 0, "OFF: clock untouched");
    check(r.complete && !r.success && r.first_cause == Cause::disabled && !r.failure_observed, "OFF report");
    PoseSnapshot s;
    check(!o->try_read_pose(s), "OFF: no pose");
    check(!o->watchdog_tick(), "OFF: watchdog inert");
    check(launch(nullptr) == Launch::disabled, "null owner refused");
}

void invalid_limits() {
    Rig g; g.lim.absolute_ms = 20000; // widening refused
    auto r = g.make()->run();
    check(g.rt->calls().empty() && r.first_cause == Cause::invalid_limits && !r.success, "widened limits refused, zero calls");
    Rig h; h.lim.events_per_batch = 64;
    check(h.make()->run().first_cause == Cause::invalid_limits, "event batch cannot widen");
    Limits tight; tight.max_frames = 10; tight.absolute_ms = 10000; // card (b)-style tightening
    check(tight.valid(), "tightened limits accepted");
}

void happy_path() {
    Rig g;
    auto o = g.make();
    auto r = o->run();
    check(r.complete && r.success && r.clean, "frame-cap run succeeds cleanly");
    check(r.stop == StopReason::frame_cap && r.first_cause == Cause::none, "frame cap reason");
    check(r.waited == 120 && r.begun == 120 && r.ended == 120 && r.waited_unbegun == 0, "balanced 120 frames");
    check(r.located == 120 && r.head_qualified == 120 && r.published == 120 && r.publish_dropped == 0, "poses published each frame");
    check(r.selected_blend == blend_opaque, "enumerated OPAQUE selected");
    check(all_released(r), "all handles retired");
    auto c = g.rt->calls();
    const std::vector<std::string> prefix{"create_instance", "query_system", "graphics_requirements", "query_binding",
        "create_session", "query_spaces", "create_local_space", "create_view_space"};
    check(std::equal(prefix.begin(), prefix.end(), c.begin()), "startup order; requirements before session");
    check(g.rt->count("begin_session") == 1 && g.rt->count("request_exit") == 1 && g.rt->count("end_session") == 1, "lifecycle once");
    check(ends_with_teardown(c), "teardown order space->session->binding->instance");
    // end_session precedes destruction, and request_exit precedes end_session
    auto pos = [&](const std::string& s) { return std::find(c.begin(), c.end(), s) - c.begin(); };
    check(pos("request_exit") < pos("end_session") && pos("end_session") < pos("destroy_view_space"), "exit handshake before destroy");
    for (auto& e : g.rt->ends) check(e.second == blend_opaque, "end uses selected blend");
    check(g.rt->ends.size() == 120 && g.rt->ends.back().first == g.rt->time_ns + 120 * g.rt->period_ns, "end displayTime = predicted");
    PoseSnapshot s;
    check(!o->try_read_pose(s), "publication retracted after stop");
    check(o->record_count() == r.runtime_calls && r.records_lost == 0, "every call recorded");
    check(o->records()[0].op == Op::create_instance, "first record");
    check(o->run().first_cause == Cause::already_used && g.rt->calls().size() == c.size(), "single attempt: rerun makes zero calls");
    check(launch(o) == Launch::already_used, "launch after run refused");
    check(!o->watchdog_tick(), "watchdog inert after finish");
}

void pose_content() {
    Rig g; g.lim.max_frames = 3;
    PoseSnapshot seen; bool got = false;
    std::shared_ptr<Owner> o;
    g.rt->hook["end_frame"] = [&] { if (!got) got = o->try_read_pose(seen); };
    o = g.make();
    auto r = o->run();
    check(r.success && got, "snapshot readable during frames");
    check(seen.sequence == 1 && seen.frame_index == 1 && seen.predicted_time == g.rt->time_ns + g.rt->period_ns, "snapshot identity/time");
    // Eye poses are raw LocateViews output: head (45 deg yaw) is NOT composed onto them.
    for (int i = 0; i < 2; ++i) {
        for (int k = 0; k < 3; ++k) check(seen.eyes[i].pose.position[k] == g.rt->views.eyes[i].pose.position[k], "eye position raw");
        for (int k = 0; k < 4; ++k) check(seen.eyes[i].pose.orientation[k] == g.rt->views.eyes[i].pose.orientation[k], "eye orientation raw");
    }
    check(seen.head.orientation[1] == g.rt->head.pose.orientation[1], "head pose separate, raw");
    check(seen.view_state_flags == (valid_bits | tracked_bits) && seen.head_flags == (valid_bits | tracked_bits), "raw flags preserved");
    check(seen.eyes_tracked() && seen.head_centre_qualified(), "qualified sample");
}

void pose_qualification() {
    struct Case { const char* name; std::function<void(Fake&)> mutate; bool eyes_valid, eyes_tracked, head_valid, head_qualified; };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Case> cases{
        {"eye position invalid", [](Fake& f) { f.views.view_state_flags = orientation_valid | tracked_bits; }, false, false, true, true},
        {"eye valid untracked", [](Fake& f) { f.views.view_state_flags = valid_bits; }, true, false, true, true},
        {"head valid untracked", [](Fake& f) { f.head.location_flags = valid_bits | orientation_tracked; }, true, true, true, false},
        {"head orientation invalid", [](Fake& f) { f.head.location_flags = position_valid | tracked_bits; }, true, true, false, false},
        {"head NaN", [nan](Fake& f) { f.head.pose.position[0] = nan; }, true, true, false, false},
        {"eye NaN fov", [nan](Fake& f) { f.views.eyes[1].fov.up = nan; }, false, false, true, true},
        {"head unnormalized", [](Fake& f) { f.head.pose.orientation[3] = 2.f; }, true, true, false, false},
        {"three views", [](Fake& f) { f.views.view_count = 3; }, false, false, true, true},
    };
    for (auto& c : cases) {
        Rig g; g.lim.max_frames = 2;
        c.mutate(*g.rt);
        PoseSnapshot s; bool got = false; std::shared_ptr<Owner> o;
        g.rt->hook["end_frame"] = [&] { if (!got) got = o->try_read_pose(s); };
        o = g.make();
        auto r = o->run();
        std::string n = c.name;
        check(r.success && got, n + ": lifecycle unaffected, still published");
        check(s.eyes_valid() == c.eyes_valid && s.eyes_tracked() == c.eyes_tracked, n + ": eye flags");
        check(s.head_valid() == c.head_valid && s.head_centre_qualified() == c.head_qualified, n + ": head flags");
        check(r.head_qualified == (c.head_qualified ? 2u : 0u), n + ": qualified counter");
    }
}

void should_render_false() {
    Rig g; g.lim.max_frames = 5; g.rt->should_render = false;
    PoseSnapshot s; bool got = false; std::shared_ptr<Owner> o;
    g.rt->hook["end_frame"] = [&] { if (!got) got = o->try_read_pose(s); };
    o = g.make();
    auto r = o->run();
    check(r.success && r.begun == 5 && r.ended == 5, "shouldRender=false keeps the frame loop running");
    check(g.rt->count("locate_views") == 5 && g.rt->count("locate_head") == 5, "readout independent of shouldRender");
    check(got && !s.should_render && s.head_centre_qualified(), "snapshot records shouldRender=false");
}

void partial_init() {
    // Fail each startup stage; only created handles are destroyed, in reverse order.
    struct Stage { const char* op; std::vector<std::string> expect_destroy; };
    std::vector<Stage> stages{
        {"create_instance", {"release_binding"}},
        {"query_system", {"release_binding", "destroy_instance"}},
        {"graphics_requirements", {"release_binding", "destroy_instance"}},
        {"query_binding", {"release_binding", "destroy_instance"}},
        {"create_session", {"release_binding", "destroy_instance"}},
        {"query_spaces", {"destroy_session", "release_binding", "destroy_instance"}},
        {"create_local_space", {"destroy_session", "release_binding", "destroy_instance"}},
        {"create_view_space", {"destroy_local_space", "destroy_session", "release_binding", "destroy_instance"}},
        {"begin_session", teardown_order},
    };
    for (auto& st : stages) {
        Rig g;
        g.rt->script[st.op] = {Rc::error};
        auto r = g.make()->run();
        std::string n = st.op;
        check(!r.success && r.failure_observed && r.first_cause == Cause::startup_call, n + ": startup failure");
        check(r.clean && all_released(r), n + ": partial init retired");
        check(g.rt->count("begin_session") == (n == "begin_session" ? 1u : 0u), n + ": no BeginSession after failure");
        check(g.rt->count("wait_frame") == 0 && g.rt->count("request_exit") == 0 && g.rt->count("end_session") == 0, n + ": no frames/exit");
        auto c = g.rt->calls();
        std::vector<std::string> destroys;
        for (auto& s : c) if (std::find(teardown_order.begin(), teardown_order.end(), s) != teardown_order.end()) destroys.push_back(s);
        check(destroys == st.expect_destroy, n + ": exact destroy set/order");
    }
    // Qualified success on creation keeps the handle (and is still a loss).
    Rig q; q.rt->script["create_session"] = {Rc::loss_pending};
    auto r = q.make()->run();
    check(!r.success && r.loss_observed && r.clean && q.rt->count("destroy_session") == 1, "qualified create keeps and retires handle");
    // Exception from the runtime is contained and teardown still runs.
    Rig x; x.rt->throws["query_spaces"] = "boom";
    r = x.make()->run();
    check(!r.success && r.first_cause == Cause::exception && r.clean && x.rt->count("destroy_session") == 1, "exception contained");
}

void compatibility() {
    struct Case { const char* name; std::function<void(Fake&)> mutate; bool reaches_session; };
    std::vector<Case> cases{
        {"wrong LUID low", [](Fake& f) { f.bind.luid_low = 8; }, false},
        {"wrong LUID high", [](Fake& f) { f.bind.luid_high = 4; }, false},
        {"feature level too low", [](Fake& f) { f.bind.max_feature_level = 0xa100; }, false},
        {"zero required feature level", [](Fake& f) { f.req.min_feature_level = 0; }, false},
        {"queue not DIRECT", [](Fake& f) { f.bind.queue_direct = false; }, false},
        {"queue on other device", [](Fake& f) { f.bind.queue_on_device = false; }, false},
        {"no device", [](Fake& f) { f.bind.device_present = false; }, false},
        {"no queue", [](Fake& f) { f.bind.queue_present = false; }, false},
        {"no HMD", [](Fake& f) { f.sys.hmd_system = false; }, false},
        {"no primary stereo", [](Fake& f) { f.sys.primary_stereo = false; }, false},
        {"three views", [](Fake& f) { f.sys.view_count = 3; }, false},
        {"only additive blend", [](Fake& f) { f.sys.blend_modes[0] = 2; }, false},
        {"no blend modes", [](Fake& f) { f.sys.blend_count = 0; }, false},
        {"blend over capacity", [](Fake& f) { f.sys.blend_count = 9; }, false},
        {"no LOCAL", [](Fake& f) { f.spaces.local = false; }, true},
        {"no VIEW", [](Fake& f) { f.spaces.view = false; }, true},
    };
    for (auto& c : cases) {
        Rig g; c.mutate(*g.rt);
        auto r = g.make()->run();
        std::string n = c.name;
        check(!r.success && r.first_cause == Cause::compatibility, n + ": refused");
        check(g.rt->count("create_session") == (c.reaches_session ? 1u : 0u), n + ": session creation gate");
        check(g.rt->count("begin_session") == 0 && r.clean && all_released(r), n + ": retired, never begun");
    }
    // Enumerated selection among several modes.
    Rig g; g.rt->sys.blend_count = 3; g.rt->sys.blend_modes = {3, 2, 1};
    g.lim.max_frames = 1;
    auto r = g.make()->run();
    check(r.success && r.selected_blend == blend_opaque, "OPAQUE selected from {3,2,1}");
    uint32_t b = 0;
    SystemFacts s; s.blend_count = 2; s.blend_modes = {2, 3};
    check(!select_blend(s, b), "pure: no OPAQUE refused");
}

// XR-START diagnostics: a compatibility refusal records the refusing gate and,
// for the device gate, every compared field of both sides; the log line names them.
void compatibility_refusal_facts() {
    auto has = [](const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; };
    // Device gate: LUID mismatch (runtime LUID zero, as a runtime without an
    // adapter would report) plus a low feature level.
    Rig g; g.rt->req = {0, 0, 0xb000};
    g.rt->bind = {true, true, 0x1a2b3, 2, 0xa100, true, true};
    auto r = g.make()->run();
    check(!r.success && r.first_cause == Cause::compatibility, "facts: refused");
    check(r.compat_stage == CompatStage::binding, "facts: binding stage");
    check(r.compat_req.luid_low == 0 && r.compat_req.luid_high == 0 && r.compat_req.min_feature_level == 0xb000, "facts: req kept");
    check(r.compat_bind.device_present && r.compat_bind.queue_present && r.compat_bind.luid_low == 0x1a2b3 &&
        r.compat_bind.luid_high == 2 && r.compat_bind.max_feature_level == 0xa100 && r.compat_bind.queue_direct &&
        r.compat_bind.queue_on_device, "facts: bind kept");
    check(compat_mismatch(r.compat_req, r.compat_bind) == (compat_luid_low | compat_luid_high | compat_feature_level), "facts: mismatch bits");
    char line[480];
    const int n = format_compat_refusal(line, sizeof line, 1, r);
    const std::string s = line;
    check(n > 0 && static_cast<size_t>(n) < 448 && s.size() == static_cast<size_t>(n), "facts: line fits text capacity");
    for (const char* part : {"event=compat-refusal;n=1;stage=binding;", "req-luid=0x00000000:0x00000000;", "req-min-fl=0xb000;",
            "bind-device=1;", "bind-queue=1;", "bind-luid=0x00000002:0x0001a2b3;", "bind-max-fl=0xa100;",
            "bind-queue-direct=1;", "bind-queue-on-device=1;", "failed=luid-low,luid-high,feature-level"})
        check(has(s, part), std::string("facts: line has ") + part);
    // Every term failing still fits and lists all eight names.
    Report all; all.compat_stage = CompatStage::binding;
    all.compat_req = {0xffffffffu, -1, 0};
    all.compat_bind = {false, false, 0, 0, 0, false, false};
    // min FL 0: min-fl-zero fails but 0 >= 0 passes the FL term.
    check(compat_mismatch(all.compat_req, all.compat_bind) == (0xffu & ~unsigned(compat_feature_level)), "facts: zero min FL terms");
    all.compat_req = {0xffffffffu, -1, 0xc100}; // FL term now fails; min-fl-zero passes
    check(compat_mismatch(all.compat_req, all.compat_bind) == (0xffu & ~unsigned(compat_min_fl_zero)), "facts: seven terms");
    const int na = format_compat_refusal(line, sizeof line, 4294967295u, all);
    check(na > 0 && static_cast<size_t>(na) < 448, "facts: worst-case line fits");
    check(has(line, "req-luid=0xffffffff:0xffffffff;") && has(line, "failed=device-absent,queue-absent,luid-low,luid-high,feature-level,queue-not-direct,queue-off-device"),
        "facts: worst-case names");
    // A compatible pair reports no failing terms.
    const GraphicsRequirements good_req{7, 3, 0xb000};
    const BindingFacts good_bind{true, true, 7, 3, 0xc100, true, true};
    check(compat_mismatch(good_req, good_bind) == 0 && compatible(good_req, good_bind), "facts: default pair compatible");
    // Other gates name only the stage.
    Rig sg; sg.rt->sys.hmd_system = false;
    r = sg.make()->run();
    check(r.compat_stage == CompatStage::system, "facts: system stage");
    format_compat_refusal(line, sizeof line, 2, r);
    check(std::string(line) == "xr-stereo;event=compat-refusal;n=2;stage=system", "facts: system line");
    Rig sp; sp.rt->spaces.view = false;
    r = sp.make()->run();
    check(r.compat_stage == CompatStage::spaces, "facts: spaces stage");
    // A successful run records no stage.
    Rig ok; ok.lim.max_frames = 1;
    r = ok.make()->run();
    check(r.success && r.compat_stage == CompatStage::none, "facts: success has no stage");
    // Null/zero buffers are safe.
    check(format_compat_refusal(nullptr, 0, 1, r) == 0, "facts: null buffer");
}

void ready_handling() {
    Rig g; g.rt->ready_after_session = false;
    auto r = g.make()->run();
    check(!r.success && r.first_cause == Cause::ready_timeout && g.rt->count("begin_session") == 0, "READY timeout: never begun");
    check(r.clean && g.clock->t >= 5000 && g.clock->t <= 5000 + Limits{}.cleanup_ms, "bounded at startup cap");

    Rig s; s.rt->ready_after_session = false;
    std::shared_ptr<Owner> o;
    s.rt->hook["poll_event"] = [&] { if (s.clock->t >= 100) o->request_stop(); };
    o = s.make();
    r = o->run();
    check(!r.success && r.stop == StopReason::requested && r.first_cause == Cause::operator_stop_before_lifecycle, "stop before READY is not success");
    check(s.rt->count("begin_session") == 0 && r.clean, "stop before READY retires without session begin");

    Rig x; x.rt->ready_after_session = false;
    x.rt->events = {state_event(SessionState::idle), state_event(SessionState::exiting)};
    r = x.make()->run();
    check(!r.success && r.first_cause == Cause::exiting && x.rt->count("begin_session") == 0, "EXITING before READY refused");

    Rig m; m.rt->ready_after_session = false;
    m.rt->events = {state_event(SessionState::ready, false)};
    r = m.make()->run();
    check(!r.success && r.first_cause == Cause::session_mismatch, "foreign-session READY refused");
}

void runtime_stopping() {
    Rig g; g.lim.max_frames = 50;
    g.rt->hook["end_frame"] = [&] { if (g.rt->ends.size() == 9) g.rt->events.push_back(state_event(SessionState::stopping)); };
    auto r = g.make()->run();
    check(r.success && r.stop == StopReason::runtime_stopping && r.begun == 10 && r.ended == 10, "runtime STOPPING ends loop");
    check(g.rt->count("request_exit") == 0 && g.rt->count("end_session") == 1 && ends_with_teardown(g.rt->calls()), "STOPPING: EndSession without request");
}

void exiting_and_loss() {
    Rig e;
    e.rt->hook["end_frame"] = [&] { if (e.rt->ends.size() == 3) e.rt->events.push_back(state_event(SessionState::exiting)); };
    auto r = e.make()->run();
    check(!r.success && r.first_cause == Cause::exiting && r.stop == StopReason::failure, "EXITING while running is failure");
    check(r.clean, "EXITING: still retired");

    Rig l; std::shared_ptr<Owner> o;
    bool readable_after = true;
    l.rt->hook["end_frame"] = [&] { if (l.rt->ends.size() == 3) l.rt->events.push_back(state_event(SessionState::loss_pending)); };
    l.rt->hook["destroy_session"] = [&] { PoseSnapshot s; readable_after = o->try_read_pose(s); };
    o = l.make();
    r = o->run();
    check(!r.success && r.loss_observed && r.first_cause == Cause::loss, "LOSS_PENDING event: sticky loss");
    check(l.rt->count("request_exit") == 0 && l.rt->count("end_session") == 0, "LOSS_PENDING: no exit handshake");
    check(l.rt->count("destroy_session") == 1 && r.clean && ends_with_teardown(l.rt->calls()), "LOSS_PENDING: destroyed");
    check(!readable_after, "loss retracts pose publication");
    check(r.begun == r.ended, "frames balanced at loss");

    Rig i;
    i.rt->hook["end_frame"] = [&] { if (i.rt->ends.size() == 2) { Event ev; ev.type = EventType::instance_loss_pending; i.rt->events.push_back(ev); } };
    r = i.make()->run();
    check(!r.success && r.loss_observed && i.rt->count("end_session") == 0 && r.clean, "instance loss pending");

    Rig h; h.rt->script["wait_frame"] = {Rc::ok, Rc::session_lost};
    r = h.make()->run();
    check(!r.success && r.loss_observed && r.waited == 1 && r.begun == 1 && r.ended == 1 && h.rt->count("begin_frame") == 1,
        "SESSION_LOST on wait: no begin");
    check(h.rt->count("request_exit") == 0 && h.rt->count("destroy_session") == 1, "lost session destroyed, no handshake");
}

void qualified_success() {
    Rig b; b.rt->script["begin_frame"] = {Rc::ok, Rc::loss_pending};
    auto r = b.make()->run();
    check(!r.success && r.loss_observed && r.begun == 2 && r.ended == 2, "begin LOSS_PENDING: frame still ended once");
    check(b.rt->count("end_frame") == 2 && b.rt->count("request_exit") == 0, "begin loss: stop, no further frames");

    Rig es; es.lim.max_frames = 4; es.rt->script["end_session"] = {Rc::loss_pending};
    r = es.make()->run();
    check(!r.success && r.loss_observed && r.clean && r.stop == StopReason::frame_cap, "EndSession LOSS_PENDING after balanced cap is failure");
    check(es.rt->count("destroy_session") == 1 && ends_with_teardown(es.rt->calls()), "EndSession loss: legal teardown continues");

    Rig rq; rq.lim.max_frames = 4; rq.rt->script["request_exit"] = {Rc::loss_pending};
    r = rq.make()->run();
    check(!r.success && r.loss_observed && rq.rt->count("end_session") == 0 && r.clean, "request_exit loss: destroy path");

    Rig ef; ef.lim.max_frames = 4; ef.rt->script["end_frame"] = {Rc::ok, Rc::loss_pending};
    r = ef.make()->run();
    check(!r.success && r.loss_observed && r.ended == 2 && ef.rt->count("end_frame") == 2, "end LOSS_PENDING: ended counted, stop");

    Rig d; d.lim.max_frames = 4; d.rt->script["begin_frame"] = {Rc::ok, Rc::frame_discarded};
    r = d.make()->run();
    check(r.success && r.discarded == 1 && r.begun == 4 && r.ended == 4, "FRAME_DISCARDED: begun, ended once, not failure");
}

void frame_call_failures() {
    Rig e; e.rt->script["end_frame"] = {Rc::ok, Rc::ok, Rc::error};
    auto r = e.make()->run();
    check(!r.success && r.first_cause == Cause::end_frame_failed && e.rt->count("end_frame") == 3 && r.ended == 2, "end failure not retried");
    check(r.clean && e.rt->count("request_exit") == 1, "end failure: orderly exit handshake");

    Rig b; b.rt->script["begin_frame"] = {Rc::ok, Rc::error};
    r = b.make()->run();
    check(!r.success && r.waited == 2 && r.begun == 1 && r.waited_unbegun == 1 && b.rt->count("end_frame") == 1, "begin error: no end for unbegun frame");

    Rig w; w.rt->script["wait_frame"] = {Rc::error};
    r = w.make()->run();
    check(!r.success && r.waited == 0 && w.rt->count("begin_frame") == 0 && r.clean, "wait error: no begin");

    Rig l; l.rt->script["locate_views"] = {Rc::ok, Rc::error};
    r = l.make()->run();
    check(!r.success && r.first_cause == Cause::frame_call && r.begun == 2 && r.ended == 2, "locate error: frame still ended, then stop");
}

void stop_between_calls() {
    const char* points[] = {"wait_frame", "begin_frame", "locate_views", "end_frame", "poll_event"};
    for (const char* p : points) {
        Rig g; std::shared_ptr<Owner> o;
        std::string point = p;
        g.rt->hook[point] = [&] { if (g.rt->count("wait_frame") >= 3) o->request_stop(); };
        o = g.make();
        auto r = o->run();
        check(r.success && r.stop == StopReason::requested, point + ": requested stop is orderly");
        check(r.waited == r.begun && r.begun == r.ended && r.waited_unbegun == 0, point + ": accepted wait/begin not erased");
        check(r.begun == 3, point + ": no frame admitted after stop");
        check(g.rt->count("request_exit") == 1 && g.rt->count("end_session") == 1 && r.clean, point + ": requested-exit teardown");
    }
}

void event_flood_and_lost() {
    Rig f; std::shared_ptr<Owner> o;
    f.rt->hook["end_frame"] = [&] {
        if (f.rt->ends.size() == 1) for (int i = 0; i < 16; ++i) { Event e; e.type = EventType::other; f.rt->events.push_back(e); }
    };
    auto r = f.make()->run();
    check(!r.success && r.first_cause == Cause::event_flood && r.begun == 2 && r.ended == 2, "16-event batch is a flood: admission stops");
    check(r.clean, "flood: teardown completes (cleanup budget, bounded batches)");

    Rig ok; ok.lim.max_frames = 3;
    ok.rt->hook["end_frame"] = [&] {
        if (ok.rt->ends.size() == 1) for (int i = 0; i < 15; ++i) { Event e; e.type = EventType::other; ok.rt->events.push_back(e); }
    };
    r = ok.make()->run();
    check(r.success && r.events >= 15, "15 events drained in one batch");

    Rig l;
    l.rt->hook["end_frame"] = [&] { if (l.rt->ends.size() == 2) { Event e; e.type = EventType::events_lost; l.rt->events.push_back(e); } };
    r = l.make()->run();
    check(!r.success && r.first_cause == Cause::events_lost && r.clean, "EVENTS_LOST is failure");

    Rig m;
    m.rt->hook["end_frame"] = [&] { if (m.rt->ends.size() == 2) m.rt->events.push_back(state_event(SessionState::focused, false)); };
    r = m.make()->run();
    check(!r.success && r.first_cause == Cause::session_mismatch, "foreign session event is failure");

    Rig p; p.rt->script["poll_event"] = {Rc::ok, Rc::ok, Rc::ok, Rc::error};
    r = p.make()->run();
    check(!r.success && r.failure_observed, "poll error refused");
}

void space_change() {
    Rig g; g.lim.max_frames = 6;
    const int64_t change = g.rt->time_ns + 4 * g.rt->period_ns; // frame 4's predicted time
    std::vector<PoseSnapshot> seen; std::shared_ptr<Owner> o;
    g.rt->hook["end_frame"] = [&] {
        PoseSnapshot s; if (o->try_read_pose(s)) seen.push_back(s);
        if (g.rt->ends.size() == 1) {
            Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local; e.change_time = change;
            g.rt->events.push_back(e);
            Event v = e; v.space = SpaceType::stage; g.rt->events.push_back(v); // not our space
        }
    };
    o = g.make();
    auto r = o->run();
    check(r.success && r.space_changes == 1 && seen.size() == 6, "LOCAL change honoured, run continues");
    for (auto& s : seen) check(s.space_generation == (s.predicted_time >= change ? 1u : 0u), "generation by changeTime, never mixed");

    Rig ov;
    ov.rt->hook["end_frame"] = [&] {
        if (ov.rt->ends.size() == 1) for (int i = 0; i < 2; ++i) {
            Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local; e.change_time = ov.rt->time_ns + 50 * ov.rt->period_ns;
            ov.rt->events.push_back(e);
        }
    };
    r = ov.make()->run();
    check(!r.success && r.first_cause == Cause::space_change_overlap && r.clean, "overlapping LOCAL changes stop conservatively");
}

void software_recenter() {
    // SEATED RECENTER: request_recenter() starts a new LOCAL generation at the next sample, with no runtime event and no changeTime.
    Rig g; g.lim.max_frames = 8;
    std::vector<PoseSnapshot> seen; std::shared_ptr<Owner> o;
    g.rt->hook["end_frame"] = [&] {
        PoseSnapshot s; if (o->try_read_pose(s)) seen.push_back(s);
        if (g.rt->ends.size() == 2 || g.rt->ends.size() == 3) o->request_recenter();   // two requests inside one frame window = two generations (each is honoured)
    };
    o = g.make();
    auto r = o->run();
    check(r.success && r.recenters == 2 && r.space_changes == 0 && seen.size() == 8, "software recenter honoured, run continues");
    uint32_t prev = 0; bool mono = true;
    for (auto& s : seen) { mono = mono && s.space_generation >= prev; prev = s.space_generation; }
    check(mono && seen.back().space_generation == 2 && seen.front().space_generation == 0, "generation 0 -> 2, never going back");

    // a request coalesced between two samples is ONE generation per request counter step seen by the owner thread
    Rig c; c.lim.max_frames = 6;
    std::vector<PoseSnapshot> cs; std::shared_ptr<Owner> oc;
    c.rt->hook["end_frame"] = [&] {
        PoseSnapshot s; if (oc->try_read_pose(s)) cs.push_back(s);
        if (c.rt->ends.size() == 2) { oc->request_recenter(); oc->request_recenter(); oc->request_recenter(); }
    };
    oc = c.make();
    r = oc->run();
    check(r.success && r.recenters == 1 && cs.back().space_generation == 1, "burst of requests inside one sample window = one recenter");

    // with a runtime change pending in the future the recenter keeps the ordering: recenter first, the runtime change after its changeTime
    Rig m; m.lim.max_frames = 8;
    const int64_t change = m.rt->time_ns + 6 * m.rt->period_ns;
    std::vector<PoseSnapshot> ms; std::shared_ptr<Owner> om;
    m.rt->hook["end_frame"] = [&] {
        PoseSnapshot s; if (om->try_read_pose(s)) ms.push_back(s);
        if (m.rt->ends.size() == 1) {
            Event e; e.type = EventType::reference_space_change; e.space = SpaceType::local; e.change_time = change;
            m.rt->events.push_back(e);
        }
        if (m.rt->ends.size() == 3) om->request_recenter();
    };
    om = m.make();
    r = om->run();
    check(r.success && r.recenters == 1 && r.space_changes == 1, "recenter + pending runtime change: both counted");
    bool ordered = true; uint32_t last = 0;
    for (auto& s : ms) { ordered = ordered && s.space_generation >= last; last = s.space_generation; }
    check(ordered && ms.back().space_generation == 2, "recenter then runtime change = generation 2, monotonic");
}

void deadlines() {
    // Startup deadline: a slow session creation consumes the 5 s budget.
    Rig s; s.rt->hook["create_session"] = [&] { s.clock->t += 5000; };
    auto r = s.make()->run();
    check(!r.success && r.policy_failure && r.first_cause == Cause::startup_deadline, "startup deadline refuses further calls");
    check(s.rt->count("query_spaces") == 0 && r.clean, "startup expiry: retired in cleanup budget");

    // Frame-phase progress: one call taking >= 250 ms is a policy failure.
    Rig p; p.rt->hook["end_frame"] = [&] { if (p.rt->ends.size() == 5) p.clock->t += 300; };
    r = p.make()->run();
    check(!r.success && r.policy_failure && r.first_cause == Cause::progress_deadline && r.begun == 6 && r.ended == 6, "slow end: progress failure");
    check(r.clean && p.rt->count("request_exit") == 1, "progress failure: orderly exit still attempted");

    // VSEARLY: Limits::frame_stall_ms relaxes ONLY the frame phase: a 2 s stall (test 19: 1968 ms) survives with 8000; 8001 ms does not;
    // the startup budget is unchanged by it.
    Rig v; v.lim.frame_stall_ms = 8000; v.rt->hook["end_frame"] = [&] { if (v.rt->ends.size() == 5) v.clock->t += 2000; };
    r = v.make()->run();
    check(r.first_cause != Cause::progress_deadline && !r.policy_failure && r.ended > 6, "vsearly: a 2 s frame stall survives frame_stall_ms 8000");
    Rig w; w.lim.frame_stall_ms = 8000; w.rt->hook["end_frame"] = [&] { if (w.rt->ends.size() == 5) w.clock->t += 8001; };
    r = w.make()->run();
    check(!r.success && r.first_cause == Cause::progress_deadline && r.begun == 6 && r.clean, "vsearly: a stall past frame_stall_ms still fails");
    Rig vs; vs.lim.frame_stall_ms = 8000; vs.rt->hook["create_session"] = [&] { vs.clock->t += 5000; };
    r = vs.make()->run();
    check(!r.success && r.first_cause == Cause::startup_deadline, "vsearly: the startup budget is unchanged");

    // Admission closes at 13 s: orderly time cap, already-waited frame finished.
    Rig a; a.lim.max_frames = 120; a.rt->frame_ms = 120; // 120 frames would take 14.4 s
    r = a.make()->run();
    check(r.success && r.stop == StopReason::time_cap && r.begun == r.ended && r.begun < 120, "13 s admission: time cap");
    check(a.clock->t <= Limits{}.absolute_ms, "finished inside absolute cap");

    // Cleanup budget: STOPPING never arrives -> no EndSession, no destruction.
    Rig c; c.lim.max_frames = 3; c.rt->stopping_after_exit = false;
    r = c.make()->run();
    check(!r.success && !r.clean && r.first_cause == Cause::cleanup_deadline && r.session_running, "STOPPING timeout: retained");
    forbid_destroy(*c.rt, "STOPPING timeout");
    check(r.instance_held && r.session_held && r.binding_held, "STOPPING timeout: owners retained");

    // Absolute cap reached during cleanup: remaining destruction refused.
    Rig x; x.lim.max_frames = 3;
    x.rt->hook["end_session"] = [&] { x.clock->t = 15000; };
    r = x.make()->run();
    check(!r.success && !r.clean && r.first_cause == Cause::absolute_cap, "absolute cap refuses cleanup calls");
    check(x.rt->count("destroy_view_space") == 0 && r.view_space_held && r.instance_held && !r.session_running, "ended but undestroyed handles retained");
}

void failed_destruction() {
    struct Case { const char* op; bool view, local, session, binding, instance; };
    std::vector<Case> cases{
        {"destroy_view_space", true, true, true, true, true},
        {"destroy_local_space", false, true, true, true, true},
        {"destroy_session", false, false, true, true, true},
        {"release_binding", false, false, false, true, true},
        {"destroy_instance", false, false, false, false, true},
    };
    for (auto& c : cases) {
        Rig g; g.lim.max_frames = 2;
        g.rt->script[c.op] = {Rc::error};
        auto o = g.make();
        auto r = o->run();
        std::string n = c.op;
        check(!r.success && !r.clean && r.first_cause == Cause::teardown_incomplete, n + ": failed destruction is failure");
        check(r.view_space_held == c.view && r.local_space_held == c.local && r.session_held == c.session &&
            r.binding_held == c.binding && r.instance_held == c.instance, n + ": exact retention, parents kept");
        const auto before = g.rt->calls().size();
        o.reset(); // destructor makes no foreign call
        check(g.rt->calls().size() == before, n + ": destructor inert");
    }
    Rig rq; rq.lim.max_frames = 2; rq.rt->script["request_exit"] = {Rc::error};
    auto r = rq.make()->run();
    check(!r.clean && r.session_running && r.session_held, "request_exit failure: running session retained");
    forbid_destroy(*rq.rt, "request_exit failure");
    Rig es; es.lim.max_frames = 2; es.rt->script["end_session"] = {Rc::error};
    r = es.make()->run();
    check(!r.clean && r.session_running && es.rt->count("destroy_session") == 0, "EndSession failure: never destroy running session");
}

// A blocked foreign call: the watchdog detects it, requests stop, and NOTHING
// is destroyed while it is in flight. Released later, the owner retires legally.
void blocked_call_watchdog(bool past_absolute) {
    Rig g;
    std::mutex m; std::condition_variable cv; bool release = false, entered = false;
    g.rt->hook["wait_frame"] = [&] {
        if (g.rt->count("wait_frame") != 4) return;
        std::unique_lock<std::mutex> lk(m);
        entered = true; cv.notify_all();
        cv.wait(lk, [&] { return release; });
    };
    auto o = g.make();
    std::thread worker([o] { o->run(); });
    {
        std::unique_lock<std::mutex> lk(m);
        check(cv.wait_for(lk, std::chrono::seconds(5), [&] { return entered; }), "owner entered blocked call");
    }
    check(o->phase() == Phase::frames && !o->watchdog_tick(), "within progress budget: no latch");
    g.clock->t += 249;
    check(!o->watchdog_tick(), "249 ms: still within policy");
    g.clock->t += past_absolute ? 15000 : 1;
    check(o->watchdog_tick(), "watchdog latches on blocked call");
    check(o->first_cause() == (past_absolute ? Cause::absolute_cap : Cause::progress_deadline), "first cause recorded");
    for (int i = 0; i < 5; ++i) o->watchdog_tick();
    check(!o->finished(), "owner still blocked; watchdog did not unblock it");
    forbid_destroy(*g.rt, "blocked call");
    check(g.rt->count("request_exit") == 0, "no concurrent exit while blocked");
    PoseSnapshot s; (void)o->try_read_pose(s); // reader never blocks while owner is stuck
    { std::lock_guard<std::mutex> lk(m); release = true; }
    cv.notify_all();
    worker.join();
    auto r = o->report();
    check(r.complete && !r.success && r.policy_failure, "late return: failure outcome");
    check(r.waited == 4, "returned wait is recorded as accepted");
    if (past_absolute) {
        check(r.begun == 3 && r.waited_unbegun == 1, "past cap: begin refused, frame left unbegun");
        check(!r.clean && r.instance_held && r.session_held && r.session_running, "past cap: everything retained");
        forbid_destroy(*g.rt, "past absolute cap");
    } else {
        check(r.clean && ends_with_teardown(g.rt->calls()) && g.rt->count("request_exit") == 1, "after return: legal requested-exit teardown");
        check(r.waited == 4 && r.begun == 4 && r.ended == 4, "returned wait completed with begin/end");
    }
}

void watchdog_phases() {
    // Startup watchdog: blocked create_instance past 5 s.
    Rig g;
    std::mutex m; std::condition_variable cv; bool release = false, entered = false;
    g.rt->hook["create_instance"] = [&] {
        std::unique_lock<std::mutex> lk(m); entered = true; cv.notify_all(); cv.wait(lk, [&] { return release; });
    };
    auto o = g.make();
    std::thread worker([o] { o->run(); });
    { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::seconds(5), [&] { return entered; }); }
    g.clock->t = 4999;
    check(!o->watchdog_tick(), "startup: 4999 ms ok (no 250 ms rule in startup)");
    g.clock->t = 5000;
    check(o->watchdog_tick() && o->first_cause() == Cause::startup_deadline, "startup deadline latched");
    { std::lock_guard<std::mutex> lk(m); release = true; }
    cv.notify_all();
    worker.join();
    auto r = o->report();
    check(!r.success && g.rt->count("query_system") == 0, "no further startup call after latch");
    check(r.clean && g.rt->count("destroy_instance") == 1, "instance created late is still retired in cleanup budget");

    // Time-cap request from the watchdog is orderly.
    Rig t; t.rt->frame_ms = 120; std::shared_ptr<Owner> ot;
    bool ticked = false, latched = true;
    t.rt->hook["end_frame"] = [&] { if (!ticked && t.clock->t >= 13000) { ticked = true; latched = ot->watchdog_tick(); } };
    ot = t.make();
    r = ot->run();
    check(ticked && !latched, "admission close is a stop request, not a policy failure");
    check(r.success && r.stop == StopReason::time_cap && r.begun == r.ended, "watchdog time-cap stop is orderly");
}

void slot_concurrency() {
    SnapshotSlot slot;
    PoseSnapshot s;
    check(!slot.try_read(s), "empty slot unreadable");
    std::atomic<bool> done{false};
    std::atomic<uint64_t> reads{0}, torn{0}, misses{0};
    std::thread reader([&] {
        while (!done.load()) {
            PoseSnapshot v;
            if (slot.try_read(v)) {
                ++reads;
                // Every field derives from the sequence: detects torn copies.
                if (v.frame_index != v.sequence || v.predicted_time != int64_t(v.sequence) * 7 ||
                    v.head.position[0] != float(v.sequence % 1000)) ++torn;
            } else ++misses;
        }
    });
    uint64_t published = 0;
    const uint64_t attempts = 200000;
    for (uint64_t i = 1; i <= attempts; ++i) {
        PoseSnapshot v; v.sequence = i; v.frame_index = i; v.predicted_time = int64_t(i) * 7; v.head.position[0] = float(i % 1000);
        published += slot.try_publish(v);
    }
    done = true;
    reader.join();
    check(torn == 0, "no torn snapshot under contention");
    check(published + slot.dropped() == attempts, "every publish is either stored or counted dropped");
    check(slot.try_read(s) && s.sequence <= attempts, "latest snapshot readable");
    slot.retract();
    check(!slot.try_read(s) && !slot.live(), "retraction is immediate");
}

void threaded_launch() {
    // Real threads and a real clock; fake runtime returns immediately.
    auto clock = make_steady_clock();
    FakeClock unused;
    auto rt = std::make_shared<Fake>(unused);
    rt->frame_ms = 0;
    Config c; c.enabled = true;
    Limits lim; lim.max_frames = 120;
    auto o = std::make_shared<Owner>(rt, clock, c, lim);
    check(launch(o) == Launch::started, "launch started");
    check(launch(o) == Launch::already_used, "second launch refused");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!o->finished() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(o->finished(), "owner thread finished within bound");
    auto r = o->report();
    check(r.success && r.clean && r.begun == 120, "threaded run succeeds");
    // A requested stop from another thread.
    auto rt2 = std::make_shared<Fake>(unused);
    rt2->frame_ms = 0;
    std::atomic<bool> go{false};
    rt2->hook["wait_frame"] = [&] { if (rt2->count("wait_frame") == 5) { go = true; std::this_thread::sleep_for(std::chrono::milliseconds(100)); } };
    auto o2 = std::make_shared<Owner>(rt2, clock, c, lim);
    check(launch(o2) == Launch::started, "second owner started");
    const auto until2 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!go.load() && std::chrono::steady_clock::now() < until2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    o2->request_stop();
    while (!o2->finished() && std::chrono::steady_clock::now() < until2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r = o2->report();
    check(o2->finished() && r.success && r.stop == StopReason::requested && r.begun == r.ended, "cross-thread stop is orderly");
}

void report_before_finish() {
    Rig g;
    auto o = g.make();
    check(!o->report().complete && o->record_count() == 0 && o->records() == nullptr, "no report/records before finish");
}
// ------------------------------------------------- XR-START diagnostics ---
struct ObservedCall { CallRecord rec; Phase phase; bool first_frame; };
struct RecordingObserver final : CallObserver {
    std::vector<ObservedCall> calls;
    void call(const CallRecord& c, Phase p, bool first) noexcept override { calls.push_back({c, p, first}); }
    unsigned count(Op op) const { unsigned n = 0; for (auto& c : calls) n += c.rec.op == op; return n; }
};

void xr_start_names() {
    check(std::string(xr_result_name(kNoXrResult)) == "none", "no result name");
    check(std::string(xr_result_name(0)) == "XR_SUCCESS", "success name");
    check(std::string(xr_result_name(-35)) == "XR_ERROR_FORM_FACTOR_UNAVAILABLE", "form factor name");
    check(std::string(xr_result_name(-51)) == "XR_ERROR_RUNTIME_UNAVAILABLE", "runtime unavailable name");
    check(std::string(xr_result_name(-9999)) == "XR_UNKNOWN_FAILURE" && std::string(xr_result_name(77)) == "XR_UNKNOWN_SUCCESS", "unknown names");
    check(std::string(session_state_name(SessionState::ready)) == "READY" && std::string(session_state_name(SessionState::idle)) == "IDLE", "state names");
    check(acz::xr_start::transient_xr_result(-35) && acz::xr_start::transient_xr_result(-2) && !acz::xr_start::transient_xr_result(-51) &&
        !acz::xr_start::transient_xr_result(-38) && !acz::xr_start::transient_xr_result(0), "transient XrResult set");
    check(std::string(acz::xr_start::class_name(acz::xr_start::Class::transient)) == "failed-transient", "class name");
}

void xr_start_diagnostics() {
    using namespace acz::xr_start;
    {   // HMD in standby: xrGetSystem -> XR_ERROR_FORM_FACTOR_UNAVAILABLE. Recorded with its XrResult, transient, clean.
        Rig g;
        g.rt->script["query_system"] = {Rc::error};
        g.rt->xr_script["query_system"] = {-35};
        auto o = g.make();
        auto obs = std::make_shared<RecordingObserver>();
        check(o->set_call_observer(obs) && !o->set_call_observer(obs), "one observer, attached before run");
        auto r = o->run();
        check(!r.success && r.first_cause == Cause::startup_call && r.clean, "form factor unavailable: clean startup failure");
        check(o->record_count() >= 2 && o->records()[0].op == Op::create_instance && o->records()[0].xr == 0, "create_instance recorded with XR_SUCCESS");
        check(o->records()[1].op == Op::query_system && o->records()[1].xr == -35 && o->records()[1].rc == Rc::error, "xrGetSystem result recorded");
        check(obs->calls.size() >= 2 && obs->calls[0].rec.op == Op::create_instance && obs->calls[0].phase == Phase::startup && !obs->calls[0].first_frame,
            "observer sees create_instance in startup");
        check(obs->calls[1].rec.xr == -35, "observer sees the XrResult");
        check(obs->count(Op::destroy_instance) == 1, "observer sees cleanup calls");
        const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
        check(d.cls == Class::transient && d.has_call && d.call == Op::query_system && d.xr == -35 &&
            std::string(d.reason) == "hmd-unavailable(form-factor-unavailable)", "diagnosis: HMD unavailable is transient");
        check(o->first_frame_done_ms() == -1, "no first frame");
        o->release_records();
        check(o->records() == nullptr && o->record_count() == 0 && o->report().complete, "records released, report kept");
    }
    {   // No OpenXR runtime: xrCreateInstance fails -> permanent.
        Rig g;
        g.rt->script["create_instance"] = {Rc::error};
        g.rt->xr_script["create_instance"] = {-51};
        auto o = g.make();
        auto r = o->run();
        const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
        check(d.cls == Class::permanent && d.call == Op::create_instance && std::string(d.reason) == "runtime-unavailable(no-openxr-runtime)",
            "diagnosis: no runtime is permanent");
        g = Rig{};
        g.rt->script["create_instance"] = {Rc::error};
        g.rt->xr_script["create_instance"] = {-2};
        auto o2 = g.make(); r = o2->run();
        check(diagnose(r, o2->records(), o2->record_count(), false).cls == Class::permanent, "instance creation refused: permanent (any XrResult)");
    }
    {   // Session never READY (idle/off-face headset): transient, no failing call.
        Rig g; g.rt->ready_after_session = false;
        auto o = g.make();
        auto obs = std::make_shared<RecordingObserver>();
        o->set_call_observer(obs);
        auto r = o->run();
        const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
        check(d.cls == Class::transient && std::string(d.reason) == "ready-timeout(session-never-READY)" && !d.has_call, "diagnosis: READY timeout");
        check(obs->count(Op::poll_event) == 0, "successful xrPollEvent calls are not reported");
    }
    {   // Graphics device mismatch error from xrCreateSession: permanent runtime error.
        Rig g;
        g.rt->script["create_session"] = {Rc::error};
        g.rt->xr_script["create_session"] = {-38};
        auto o = g.make(); auto r = o->run();
        const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
        check(d.cls == Class::permanent && d.call == Op::create_session && d.xr == -38 && std::string(d.reason) == "runtime-error", "diagnosis: device invalid permanent");
    }
    {   // Slow runtime start: xrCreateInstance takes 6 s -> startup deadline, transient, the slow call named.
        Rig g;
        g.rt->hook["create_instance"] = [&] { g.clock->t += 6000; };
        auto o = g.make(); auto r = o->run();
        const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
        check(r.first_cause == Cause::startup_deadline && d.cls == Class::transient && d.call == Op::create_instance && d.call_ms == 6000 &&
            std::string(d.reason) == "startup-deadline(runtime-slow-to-start)", "diagnosis: slow runtime start");
    }
    {   // Continuous run: first frame reported to the observer as first-frame, then hooked; a stop is ended-after-start.
        Rig g;
        Config c; c.enabled = true; c.continuous = true;
        auto o = std::make_shared<Owner>(g.rt, g.clock, c, g.lim);
        auto obs = std::make_shared<RecordingObserver>();
        o->set_call_observer(obs);
        g.rt->hook["end_frame"] = [&] { if (g.rt->ends.size() == 19) o->request_stop(); };
        auto r = o->run();
        check(r.first_frame_done_ms >= 0 && o->first_frame_done_ms() == r.first_frame_done_ms, "first frame done published");
        unsigned first = 0, later = 0;
        for (auto& x : obs->calls) { first += x.first_frame; later += x.phase == Phase::frames && !x.first_frame; }
        check(first >= 4 && later == 0, "only the first frame's calls are observed in the frame loop");
        const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
        check(d.cls == Class::ended_after_start, "diagnosis: hooked attempt ends after start");
    }
}
// ------------------------------------------------- XR-START persistent retry ---
// The monitor's loop (xr_start::run_attempts) against ONE fake runtime shared by every attempt, so
// its call counters prove that each attempt retired what it created before the next one started.
struct LoopRig {
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::shared_ptr<Fake> rt = std::make_shared<Fake>(*clock);
    int64_t mono = 0;                       // the monitor's own clock (GetTickCount64 stand-in)
    std::vector<int64_t> waits;
    std::vector<std::string> decisions;
    std::vector<acz::xr_start::Class> classes;
    unsigned frames_before_stop = 10;
    bool stop_in_wait_after = false; unsigned stop_wait_index = 0;
    std::function<void(unsigned)> before_attempt;
    std::function<bool(unsigned, const Report&, const acz::xr_start::Diagnosis&)> legacy; // default: none
    unsigned run(acz::xr_start::RetryPolicy& policy) {
        acz::xr_start::LoopHooks h;
        h.now_ms = [&] { return mono; };
        h.wait = [&](int64_t ms) {
            waits.push_back(ms); mono += ms;
            return !(stop_in_wait_after && waits.size() > stop_wait_index);
        };
        h.attempt = [&](unsigned n, acz::xr_start::AttemptOutcome& out) {
            if (before_attempt) before_attempt(n);
            Config c; c.enabled = true; c.continuous = true;
            auto o = std::make_shared<Owner>(rt, clock, c, Limits{});
            const size_t ends_before = rt->ends.size();
            rt->hook["end_frame"] = [this, o, ends_before] { if (rt->ends.size() + 1 - ends_before >= frames_before_stop) o->request_stop(); };
            const int64_t t0 = clock->t;
            out.report = o->run();
            mono += clock->t - t0;
            out.diag = acz::xr_start::diagnose(out.report, o->records(), o->record_count(), false);
            out.legacy_retry = legacy ? legacy(n, out.report, out.diag) : false;
            o->release_records();
            rt->hook.erase("end_frame");
            check(all_released(out.report) && out.report.clean, "attempt " + std::to_string(n) + " retired every handle");
            return true;
        };
        h.ended = [&](unsigned, const acz::xr_start::AttemptOutcome& a, const acz::xr_start::Decision& d) {
            decisions.push_back(d.text); classes.push_back(a.diag.cls);
        };
        return acz::xr_start::run_attempts(policy, h);
    }
    // Every created handle was destroyed exactly once across ALL attempts.
    void balanced(const std::string& what) const {
        check(rt->count("create_instance") == rt->count("destroy_instance"), what + ": instances balanced");
        check(rt->count("create_session") == rt->count("destroy_session"), what + ": sessions balanced");
        check(rt->count("create_local_space") == rt->count("destroy_local_space"), what + ": LOCAL spaces balanced");
        check(rt->count("create_view_space") == rt->count("destroy_view_space"), what + ": VIEW spaces balanced");
        check(rt->count("release_binding") == rt->count("create_instance"), what + ": one binding release per attempt");
        check(rt->count("begin_session") == rt->count("end_session"), what + ": begun sessions ended");
    }
};

void xr_start_policy_units() {
    using namespace acz::xr_start;
    check(persistent_retry_enabled(nullptr) && persistent_retry_enabled("") && persistent_retry_enabled("1") && persistent_retry_enabled("on"),
        "persistent retry default ON");
    check(!persistent_retry_enabled("0") && !persistent_retry_enabled("off") && !persistent_retry_enabled("FALSE") && !persistent_retry_enabled("No"),
        "ACZ_XR_RETRY=0/off/false/no disables");
    check(backoff_ms(0) == 3000 && backoff_ms(1) == 5000 && backoff_ms(2) == 10000 && backoff_ms(3) == 15000 && backoff_ms(40) == 15000, "backoff 3/5/10/15 s");
    Diagnosis transient; transient.cls = Class::transient;
    Diagnosis permanent; permanent.cls = Class::permanent;
    Diagnosis after; after.cls = Class::ended_after_start;
    {
        RetryPolicy p(true);
        p.attempt_started(0);
        Decision d = p.decide(1, true, transient, 100);
        check(d.retry && d.delay_ms == 0 && std::string(d.text) == "yes-cold-start", "legacy cold-start retry first and immediate");
        p.attempt_started(100);
        d = p.decide(2, true, transient, 200);
        check(d.retry && d.delay_ms == 3000 && std::string(d.text) == "yes-persistent", "attempt 2: legacy never again, persistent 3 s");
        check(!p.decide(3, false, permanent, 300).retry && std::string(p.decide(3, false, permanent, 300).text) == "no(permanent)", "permanent stops");
        check(!p.decide(3, false, after, 300).retry && std::string(p.decide(3, false, after, 300).text) == "no(after-start)", "after-start not retried");
    }
    {
        RetryPolicy off(false);
        off.attempt_started(0);
        Decision d = off.decide(1, true, transient, 0);
        check(d.retry && d.delay_ms == 0 && std::string(d.text) == "yes-cold-start", "off: legacy retry unchanged");
        d = off.decide(2, false, transient, 0);
        check(!d.retry && d.delay_ms == -1 && std::string(d.text) == "no(persistent-retry-off:ACZ_XR_RETRY=0)", "off: no persistent retry");
        RetryPolicy off1(false);
        check(!off1.decide(1, false, transient, 0).retry, "off: no legacy reason, no retry");
    }
    {   // Rate cap (cap 3 for the test): starts at 0, 3000, 8000 -> the 4th may not start before 60000.
        RetryPolicy p(true, 3);
        p.attempt_started(0);
        Decision d = p.decide(1, false, transient, 0);         // 3000
        p.attempt_started(3000);
        d = p.decide(2, false, transient, 3000);                // 5000
        p.attempt_started(8000);
        d = p.decide(3, false, transient, 8000);                // 10000 -> 18000 < 60000: capped
        check(d.retry && d.delay_ms == 52000 && std::string(d.text) == "yes-persistent(rate-capped)", "rate cap stretches the wait");
        RetryPolicy q(true);                                    // default cap 6: the 3/5/10/15 s schedule never hits it
        int64_t now = 0;
        for (unsigned n = 1; n <= 20; ++n) {
            q.attempt_started(now);
            const Decision e = q.decide(n, false, transient, now);
            check(e.retry && std::string(e.text) == "yes-persistent", "default schedule under the cap");
            now += e.delay_ms;
        }
    }
}

void xr_start_retry_loop() {
    using namespace acz::xr_start;
    {   // HMD unavailable (standby) for 3 attempts, then available: attempt 4 hooks; every attempt clean.
        LoopRig g;
        g.rt->script["query_system"] = {Rc::error, Rc::error, Rc::error};
        g.rt->xr_script["query_system"] = {-35, -35, -35};
        RetryPolicy p(true);
        const unsigned n = g.run(p);
        check(n == 4, "HMD unavailable x3 then available: 4 attempts");
        check(g.waits == std::vector<int64_t>({3000, 5000, 10000}), "backoff 3/5/10 s between attempts");
        check(g.classes[0] == Class::transient && g.classes[3] == Class::ended_after_start, "transient x3, then hooked and ended orderly");
        check(g.decisions.back() == "no(after-start)", "a hooked attempt is never retried");
        check(g.rt->count("create_session") == 1 && g.rt->count("begin_session") == 1, "only the hooking attempt created a session");
        g.balanced("hmd-unavailable loop");
    }
    {   // Session never READY twice (idle headset), then READY: sessions created and destroyed each time.
        LoopRig g;
        g.before_attempt = [&](unsigned n) { g.rt->ready_after_session = n >= 3; };
        RetryPolicy p(true);
        check(g.run(p) == 3 && g.rt->count("create_session") == 3 && g.rt->count("begin_session") == 1, "READY timeout x2 then hooks");
        check(g.rt->count("destroy_session") == 3, "no session leaked across READY-timeout retries");
        g.balanced("ready-timeout loop");
    }
    {   // Permanent: no OpenXR runtime (xrCreateInstance -> XR_ERROR_RUNTIME_UNAVAILABLE): one attempt, stops.
        LoopRig g;
        g.rt->script["create_instance"] = {Rc::error, Rc::error};
        g.rt->xr_script["create_instance"] = {-51, -51};
        RetryPolicy p(true);
        check(g.run(p) == 1 && g.decisions[0] == "no(permanent)" && g.waits.empty(), "permanent error: stops after one attempt");
        check(g.rt->count("create_instance") == 1, "no second instance request");
    }
    {   // Permanent mid-way: transient twice, then an incompatible device error -> stops.
        LoopRig g;
        g.rt->script["query_system"] = {Rc::error, Rc::error};
        g.rt->xr_script["query_system"] = {-35, -35};
        g.rt->script["create_session"] = {Rc::error};
        g.rt->xr_script["create_session"] = {-38};
        RetryPolicy p(true);
        check(g.run(p) == 3 && g.decisions.back() == "no(permanent)", "transient, transient, permanent: stops");
        check(g.rt->count("create_instance") == 3 && g.rt->count("destroy_instance") == 3 && g.rt->count("release_binding") == 3,
            "every instance destroyed, every binding released");
        check(g.rt->count("create_session") == 1 && g.rt->count("destroy_session") == 0, "the failed xrCreateSession left no session to destroy");
    }
    {   // Escape ACZ_XR_RETRY=0: the previous policy exactly (attempt 1, one legacy cold-start retry, stop).
        LoopRig g;
        g.rt->ready_after_session = false;
        g.legacy = [](unsigned n, const Report& r, const Diagnosis&) {   // xr_stereo::retry_allowed's rule (tested in xr_stereo_output_offline)
            return n == 1 && r.complete && !r.success && r.clean && !r.loss_observed && r.first_cause == Cause::ready_timeout;
        };
        RetryPolicy off(false);
        check(g.run(off) == 2 && g.decisions[0] == "yes-cold-start" && g.decisions[1] == "no(persistent-retry-off:ACZ_XR_RETRY=0)",
            "off: two attempts as before");
        check(g.waits.empty(), "off: the legacy retry is immediate");
        RetryPolicy on(true);
        LoopRig h; h.rt->ready_after_session = false; h.legacy = g.legacy;
        h.stop_in_wait_after = true; h.stop_wait_index = 2;   // the game exits during the 3rd wait
        check(h.run(on) == 4 && h.decisions[0] == "yes-cold-start" && h.decisions[1] == "yes-persistent", "on: legacy retry then persistent");
        check(h.waits == std::vector<int64_t>({3000, 5000, 10000}), "on: waits 3/5/10 s; stop during a wait ends the loop");
        h.balanced("stop-during-wait loop");
    }
    {   // First attempt succeeds: one attempt, the watchdog/limits path unchanged, no retry.
        LoopRig g;
        RetryPolicy p(true);
        check(g.run(p) == 1 && g.classes[0] == Class::ended_after_start && g.waits.empty(), "successful first attempt: no retry");
        g.balanced("success loop");
    }
}
// Runtime-safety review (docs/review/overnight-runtime-review.md): the monitor's loop with REAL owner + watchdog threads (launch()), a real
// clock and a game-exit stop arriving at a random moment (before, during or between attempts). Every round must end (no hang), every
// attempt's owner must finish, and every handle created across the attempts must be retired exactly once.
void xr_start_threaded_stop_stress() {
    using namespace acz::xr_start;
    auto clock = make_steady_clock();
    std::mt19937 rng(2026);
    unsigned rounds_with_retry = 0, stopped_in_wait = 0;
    for (unsigned round = 0; round < 60; ++round) {
        FakeClock unused;
        auto rt = std::make_shared<Fake>(unused);
        rt->frame_ms = 0;
        const unsigned transient = round % 4;   // HMD unavailable for the first `transient` attempts
        for (unsigned i = 0; i < transient; ++i) { rt->script["query_system"].push_back(Rc::error); rt->xr_script["query_system"].push_back(-35); }
        std::atomic<bool> stop{false};
        std::mutex om; std::vector<std::shared_ptr<Owner>> owners;   // retained like publish_owner (never destroyed while threads may run)
        const unsigned delay_us = unsigned(rng() % 20000);
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
            stop = true;
            std::lock_guard<std::mutex> l(om);
            for (auto& o : owners) o->request_stop();
        });
        RetryPolicy policy(true);
        int64_t mono = 0;
        LoopHooks h;
        h.now_ms = [&] { return mono; };
        h.wait = [&](int64_t ms) { mono += ms; if (stop.load()) { ++stopped_in_wait; return false; } return true; };
        h.attempt = [&](unsigned n, AttemptOutcome& out) -> bool {
            Config c; c.enabled = true; c.continuous = true;
            auto o = std::make_shared<Owner>(rt, clock, c, Limits{});
            { std::lock_guard<std::mutex> l(om); owners.push_back(o); }
            if (stop.load()) o->request_stop();
            if (launch(o) != Launch::started) return false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!o->finished() && std::chrono::steady_clock::now() < until) {
                if (stop.load()) o->request_stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(o->finished(), "threaded stress: attempt " + std::to_string(n) + " finished within bound");
            out.report = o->report();
            out.diag = diagnose(out.report, o->records(), o->record_count(), false);
            out.stop = stop.load();
            o->release_records();
            check(out.report.clean && all_released(out.report), "threaded stress: attempt retired every handle");
            return true;
        };
        const unsigned n = run_attempts(policy, h);
        stopper.join();
        check(n >= 1 && n <= transient + 1, "threaded stress: attempts bounded by the transient failures + the hooking one");
        rounds_with_retry += n > 1;
        check(rt->count("create_instance") == rt->count("destroy_instance"), "threaded stress: instances balanced");
        check(rt->count("create_session") == rt->count("destroy_session"), "threaded stress: sessions balanced");
        check(rt->count("create_local_space") == rt->count("destroy_local_space") && rt->count("create_view_space") == rt->count("destroy_view_space"),
            "threaded stress: spaces balanced");
        // The adopted device/queue binding is released once per attempt that ran, including one stopped before xrCreateInstance.
        check(rt->count("release_binding") == n, "threaded stress: one binding release per attempt");
        check(rt->count("begin_session") == rt->count("end_session"), "threaded stress: begun sessions ended");
        // Owners are retained; their watchdog threads exit on finished(): give them one poll period, then nothing may still be running.
        std::this_thread::sleep_for(std::chrono::milliseconds(Limits{}.poll_ms * 2));
        for (auto& o : owners) check(o->finished(), "threaded stress: every retained owner finished");
    }
    check(rounds_with_retry > 0, "threaded stress: some rounds retried before the stop");
    std::cout << "xr_start threaded stop stress: rounds=60 with-retry=" << rounds_with_retry << " stop-in-wait=" << stopped_in_wait << '\n';
}
// ------------------------------------------------- R-5: bounded retry + owner release ---
// Slow-down tiers: normal backoff for 10 min, then >= 60 s, after 60 min >= 5 min; never a permanent stop; each step flagged once.
void xr_start_bounded_policy() {
    using namespace acz::xr_start;
    check(slow_tier(0) == 0 && slow_tier(kSlowAfterMs - 1) == 0 && slow_tier(kSlowAfterMs) == 1 && slow_tier(kSlowerAfterMs - 1) == 1 &&
        slow_tier(kSlowerAfterMs) == 2 && tier_interval_ms(0) == 0 && tier_interval_ms(1) == 60000 && tier_interval_ms(2) == 300000, "slow tiers");
    Diagnosis transient; transient.cls = Class::transient;
    RetryPolicy p(true);
    int64_t now = 0;
    unsigned attempts = 0, changes = 0, in10 = 0, in60 = 0;
    std::vector<unsigned> change_tiers;
    while (now < 3 * 3600 * 1000LL) {   // 3 hours with the HMD never available
        p.attempt_started(now);
        ++attempts;
        if (now < kSlowAfterMs) ++in10; else if (now < kSlowerAfterMs) ++in60;
        const Decision d = p.decide(attempts, false, transient, now);
        check(d.retry, "bounded policy never stops for good");
        const int64_t floor = tier_interval_ms(slow_tier(now));
        check(d.delay_ms >= floor && d.delay_ms >= 3000, "delay respects the tier floor");
        if (now >= kSlowerAfterMs) check(d.delay_ms == 300000 && std::string(d.text) == "yes-persistent(slow-5min)", "after 60 min: every 5 min");
        else if (now >= kSlowAfterMs) check(d.delay_ms == 60000 && std::string(d.text) == "yes-persistent(slow-60s)", "after 10 min: every 60 s");
        if (d.tier_changed) { ++changes; change_tiers.push_back(d.tier); }
        now += d.delay_ms;
    }
    check(changes == 2 && change_tiers == std::vector<unsigned>({1, 2}), "each slow-down step is flagged exactly once");
    std::cout << "R-5 bounded retry: 3 h without HMD -> attempts=" << attempts << " (first 10 min " << in10 << ", 10-60 min " << in60
              << ", 60-180 min " << (attempts - in10 - in60) << ")\n";
    check(in10 <= 45 && in60 <= 51 && attempts - in10 - in60 <= 25, "attempt counts bounded per phase");
    // ACZ_XR_RETRY=0 is unchanged by the tiers: still two attempts at most.
    RetryPolicy off(false);
    off.attempt_started(0);
    check(!off.decide(2, false, transient, kSlowerAfterMs + 1).retry, "off: no persistent retry at any elapsed time");
}

// Owner registry: publication, reader quiescence and release (xr_owner_registry.h). Real Owners on a fake runtime.
void owner_registry() {
    using namespace acz::xr_start;
    auto clock = std::make_shared<FakeClock>();
    auto rt = std::make_shared<Fake>(*clock);
    Config c; c.enabled = true; c.continuous = true;
    OwnerRegistry reg;
    PoseSnapshot snap;
    check(!reg.try_read_pose(snap) && reg.current() == nullptr, "empty registry: no pose");
    // Not finished -> kept; not published -> not found.
    auto idle = std::make_shared<Owner>(rt, clock, c, Limits{});
    check(reg.publish(idle) && reg.current() == idle.get() && reg.retained() == 1, "publish");
    check(reg.release(idle, 10) == OwnerRegistry::Release::not_finished && reg.retained() == 1 && reg.current() == idle.get(), "unfinished owner is kept");
    auto other = std::make_shared<Owner>(rt, clock, c, Limits{});
    rt->script["query_system"] = {Rc::error}; rt->xr_script["query_system"] = {-35};
    (void)other->run();
    check(other->finished() && reg.release(other, 10) == OwnerRegistry::Release::not_found, "unpublished owner: not found");
    // A finished, superseded owner is released and destroyed once the last outside reference goes.
    rt->script["query_system"] = {Rc::error}; rt->xr_script["query_system"] = {-35};
    auto failed = std::make_shared<Owner>(rt, clock, c, Limits{});
    check(reg.publish(failed), "publish failed attempt");
    (void)failed->run();
    std::weak_ptr<Owner> w = failed;
    check(reg.release(failed, 100) == OwnerRegistry::Release::released && reg.current() == nullptr && reg.retained() == 1 && reg.released() == 1,
        "finished failed owner released; current cleared");
    failed.reset();
    check(w.expired(), "released owner destroyed with the last outside reference");
    check(!reg.try_read_pose(snap), "no owner published: reads return false");
    reg.request_stop_current();   // no current owner: no-op
    // Concurrent readers: 4 threads hammer try_read_pose/request_stop_current while 300 failed attempts publish+release. Every released owner
    // must be destroyed (no reader can keep a raw pointer: the registry is the only long-lived reference).
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] { PoseSnapshot s; while (!stop.load(std::memory_order_relaxed)) { reg.try_read_pose(s); ++reads; } });
    unsigned released = 0, busy = 0, destroyed = 0;
    for (int i = 0; i < 300; ++i) {
        rt->script["query_system"] = {Rc::error}; rt->xr_script["query_system"] = {-35};
        auto o = std::make_shared<Owner>(rt, clock, c, Limits{});
        reg.publish(o);
        (void)o->run();
        o->release_records();
        std::weak_ptr<Owner> wo = o;
        const auto r = reg.release(o, 200);
        released += r == OwnerRegistry::Release::released;
        busy += r == OwnerRegistry::Release::readers_busy;
        o.reset();
        destroyed += wo.expired();
    }
    stop = true;
    for (auto& t : readers) t.join();
    check(released + busy == 300 && destroyed == released && released >= 295, "concurrent readers: released owners destroyed (" +
        std::to_string(released) + " released, " + std::to_string(busy) + " busy)");
    check(reg.retained() == 1 + busy, "registry keeps only the unfinished owner and busy ones");
    std::cout << "owner registry: 300 attempts under 4 hammering readers: released=" << released << " busy=" << busy << " reads=" << reads.load() << '\n';
}

// Measured retention per failed attempt (the review's ~10 MB/hour estimate): live heap with every owner retained (old behaviour) vs released.
// Only the Owner and what it owns is measured here (fake runtime); the proxy's Output/OpenXr/Blit objects die with the Owner reference chain.
void owner_retention_measure() {
    using namespace acz::xr_start;
    auto run_loop = [](bool release_owners, unsigned attempts, size_t& retained_after) -> int64_t {
        auto clock = std::make_shared<FakeClock>();
        auto rt = std::make_shared<Fake>(*clock);
        auto reg = std::make_unique<OwnerRegistry>();
        Config c; c.enabled = true; c.continuous = true;
        const int64_t before = g_live_bytes.load();
        for (unsigned i = 0; i < attempts; ++i) {
            rt->script["query_system"] = {Rc::error}; rt->xr_script["query_system"] = {-35};
            auto o = std::make_shared<Owner>(rt, clock, c, Limits{});
            reg->publish(o);
            const Report r = o->run();
            const Diagnosis d = diagnose(r, o->records(), o->record_count(), false);
            o->release_records();
            check(d.cls == Class::transient, "measure: transient failure");
            if (release_owners) check(reg->release(o, 100) == OwnerRegistry::Release::released, "measure: released");
        }
        rt->log.clear(); rt->log.shrink_to_fit();   // the fake's own call log is not retention
        const int64_t grown = g_live_bytes.load() - before;
        retained_after = reg->retained();
        return grown;
    };
    constexpr unsigned kAttempts = 100;
    size_t kept_old = 0, kept_new = 0;
    const int64_t old_growth = run_loop(false, kAttempts, kept_old);
    const int64_t new_growth = run_loop(true, kAttempts, kept_new);
    const double per_old = double(old_growth) / kAttempts, per_new = double(new_growth) / kAttempts;
    std::cout << "R-5 retention: per failed attempt retained " << int64_t(per_old) << " B before (owners kept), " << int64_t(per_new)
              << " B after (released); at the 3/5/10/15 s schedule (~240 attempts/h) = " << int64_t(per_old * 240 / 1024) << " KiB/h before, "
              << int64_t(per_new * 240 / 1024) << " KiB/h after\n";
    check(kept_old == kAttempts && kept_new == 0, "retained counts: all kept before, none after");
    check(per_old > 200.0, "the old behaviour retains a measurable owner per attempt");
    check(per_new < per_old / 4, "releasing reduces the retained memory per attempt");
}
} // namespace

int main() {
    disabled_default();
    invalid_limits();
    report_before_finish();
    happy_path();
    pose_content();
    pose_qualification();
    should_render_false();
    partial_init();
    compatibility();
    compatibility_refusal_facts();
    ready_handling();
    runtime_stopping();
    exiting_and_loss();
    qualified_success();
    frame_call_failures();
    stop_between_calls();
    event_flood_and_lost();
    space_change();
    software_recenter();
    deadlines();
    failed_destruction();
    blocked_call_watchdog(false);
    blocked_call_watchdog(true);
    watchdog_phases();
    slot_concurrency();
    threaded_launch();
    xr_start_names();
    xr_start_diagnostics();
    xr_start_policy_units();
    xr_start_retry_loop();
    xr_start_threaded_stop_stress();
    xr_start_bounded_policy();
    owner_registry();
    owner_retention_measure();
    std::cout << "xr_owner offline: " << checks << " checks passed\n";
    return 0;
}
