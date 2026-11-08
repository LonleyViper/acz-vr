// VSEARLY offline tests (docs/world-render/vsearly.md): the early start gate, flight start/end, the per-flight stage slot (request, late results,
// reuse, clearing, parking) and the retire rule for stages the props worker may borrow; the early retry policy (xr_start) and the owner's
// frame-stall limit validity. Fake stage type and clock; the loader runs synchronously or on a real second thread. No device, no headset.
#include "flight_cycle.h"
#include "vscreen.h"
#include "xr_owner.h"
#include "xr_start.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace xr_owner = acz::xr_owner;
namespace {
int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Atomic (Astra 6458816088 P2): stages are created on the loader thread and freed on the owner thread in test_threads.
struct FakeStage { std::string id; uint64_t gen = 0; static std::atomic<int> alive; FakeStage() { alive.fetch_add(1); } ~FakeStage() { alive.fetch_sub(1); } };
std::atomic<int> FakeStage::alive{0};
using FS = acz::flight::FlightStage<FakeStage>;
using L = acz::flight::Loaded<FakeStage>;

L make(const std::string& id, uint64_t gen, bool props) {
    L l; auto s = std::make_shared<FakeStage>(); s->id = id; s->gen = gen;
    l.rt = s; l.id = id; l.gens = {gen, gen + 1}; l.has_props = props; l.why = "ok"; return l;
}

void test_gate() {
    using namespace acz::flight;
    CHECK(std::strcmp(early_refusal(false, true, true, true), "off") == 0);
    CHECK(std::strcmp(early_refusal(true, false, true, true), "needs-vscreen:1") == 0);
    CHECK(std::strcmp(early_refusal(true, true, false, true), "needs-xr_hud_layer") == 0);
    CHECK(std::strcmp(early_refusal(true, true, true, false), "needs-ee_camera_probe(present-hook)") == 0);
    CHECK(early_refusal(true, true, true, true) == nullptr);
    CHECK(!early_start_ready(119, 10000));
    CHECK(!early_start_ready(500, 2999));
    CHECK(early_start_ready(120, 3000));
    // The option text: vsearly no longer reported as ignored; parsing unchanged.
    acz::vscreen::Options o; std::string rest, why;
    CHECK(acz::vscreen::extract_options("auto;env:1;vscreen:1;vsearly:1;props:1", rest, o, why));
    CHECK(o.on && o.early && rest == "auto;env:1;props:1");
    CHECK(acz::vscreen::describe(o).find("ignored") == std::string::npos);
    CHECK(acz::vscreen::extract_options("auto;vsearly:2", rest, o, why) == false);
}

void test_cycle() {
    acz::flight::FlightCycle c;   // end 8000
    using E = acz::flight::FlightCycle::Event;
    CHECK(c.step(0, -1) == E::none);          // menus: no composite ever
    CHECK(c.step(5000, -1) == E::none);
    CHECK(c.step(6000, 5990) == E::start && c.in_flight() && c.flight() == 1);
    CHECK(c.step(6011, 6000) == E::none);
    // a 2 s load stall (test 19) and a 7.9 s pause stay inside the flight
    CHECK(c.step(8100, 6000) == E::none);
    CHECK(c.step(13900, 6000) == E::none);
    CHECK(c.step(14000, 13990) == E::none);
    CHECK(c.step(22000, 13990) == E::end);    // 8010 ms without a fresh composite: the flight ended
    acz::flight::FlightCycle d;
    CHECK(d.step(0, 0) == E::start);
    CHECK(d.step(8000, 0) == E::none);        // == end_ms: not yet
    CHECK(d.step(8001, 0) == E::end && !d.in_flight());
    CHECK(d.step(9000, 0) == E::none);        // no new composite: stays ended
    CHECK(d.step(30000, 29990) == E::start && d.flight() == 2);
    d.reset_session();
    CHECK(!d.in_flight());
    CHECK(d.step(31000, -1) == E::none);      // a new consumer: no composite yet
    CHECK(d.step(31100, 31050) == E::start && d.flight() == 3);   // numbering continues
}

// One whole mission cycle with a synchronous loader: menus -> flight 1 (m02a) -> end -> flight 2 (m05a) -> end -> flight 3 (m05a again: reuse).
void test_stage_cycle() {
    FS fs;
    int installs = 0, ends = 0, nones = 0;
    FS::Hooks h;
    h.installed = [&](const L&, bool, uint64_t) { ++installs; };
    h.ended = [&](uint64_t, bool) { ++ends; };
    h.no_stage = [&](const L&, uint64_t) { ++nones; };
    fs.set_hooks(h);
    std::vector<std::string> plan = {"m02a", "m05a", "m05a", ""};
    uint64_t gen = 1001; unsigned calls = 0;
    auto loader = [&](uint64_t flight) -> L { ++calls; const std::string& id = plan[size_t(flight - 1)]; if (id.empty()) { L n; n.why = "no-match"; return n; } L l = make(id, gen, true); gen += 2; return l; };
    std::shared_ptr<const FakeStage> io;
    fs.begin_session(0);
    CHECK(!fs.frame(0, -1, io) && !io && !fs.pending());     // menus: no request, nothing drawn
    CHECK(!fs.frame(100, 90, io) && fs.pending());             // flight 1 starts: a request, nothing drawn yet
    CHECK(fs.work(loader) && !fs.pending() && calls == 1);
    CHECK(fs.frame(111, 100, io) && io && io->id == "m02a" && installs == 1);
    const uint64_t g1 = io->gen;
    CHECK(!fs.work(loader));                                    // nothing pending
    fs.recorded(g1, true, true);                                // settled props of the current stage (nothing retired yet)
    // flight end: cleared (not drawn), parked (not freed)
    CHECK(fs.frame(9000, 900, io) && !io && ends == 1 && fs.parked());
    CHECK(FakeStage::alive.load() == 1);
    // flight 2 = another stage: install, the parked m02a is retired (props: kept until a settled job of the new stage)
    CHECK(!fs.frame(20000, 19990, io) && fs.pending());
    CHECK(fs.work(loader));
    CHECK(fs.frame(20011, 20000, io) && io->id == "m05a" && installs == 2 && !fs.parked() && fs.retired() == 1);
    CHECK(FakeStage::alive.load() == 2);
    fs.recorded(g1, true, true);          // an old generation: not proof
    CHECK(fs.retired() == 1);
    fs.recorded(io->gen, true, false);    // props of the new stage still running
    CHECK(fs.retired() == 1);
    fs.recorded(io->gen, false, false);   // a job without props: not proof
    CHECK(fs.retired() == 1);
    fs.recorded(io->gen + 1, true, true); // the new stage's TAKEOFF props settled: the worker of the old stage was joined
    CHECK(fs.retired() == 0 && FakeStage::alive.load() == 1);
    // flight 3 = the same stage again: the parked one is reused, the fresh load is dropped unused
    CHECK(fs.frame(30000, 20000, io) && !io && ends == 2);
    CHECK(!fs.frame(31000, 30990, io));
    CHECK(fs.work(loader));
    const FakeStage* parked_ptr = fs.current().rt.get();
    CHECK(parked_ptr == nullptr);
    CHECK(fs.frame(31011, 31000, io) && io && io->id == "m05a" && installs == 3);
    CHECK(FakeStage::alive.load() == 1);         // the reload was freed, the parked object is drawn again
    CHECK(fs.summary().find("reused=1") != std::string::npos);
    // flight 4: detection finds nothing -> no terrain (game world image), nothing drawn
    CHECK(fs.frame(40000, 31000, io) && !io);
    CHECK(!fs.frame(41000, 40990, io));
    CHECK(fs.work(loader));
    CHECK(!fs.frame(41011, 41000, io) && !io && nones == 1);
    CHECK(!fs.parked() && fs.retired() == 1);   // the parked m05a (props) went to retired; no newer stage can prove it idle yet
}

// A result for a flight that already ended is never drawn; a request superseded while loading is discarded by the worker.
void test_late_results() {
    FS fs;
    std::shared_ptr<const FakeStage> io;
    auto loader = [&](uint64_t) { return make("m02a", 2001, false); };
    fs.begin_session(0);
    fs.frame(100, 90, io);                 // flight 1 starts
    fs.frame(9000, 90, io);                // ends before the load ran
    CHECK(fs.work(loader));                // loads for flight 1
    CHECK(!fs.frame(9100, 90, io) && !io); // discarded (late)
    CHECK(fs.summary().find("discarded=1") != std::string::npos);
    CHECK(FakeStage::alive.load() == 0);
    // superseded during the load: flight 2 requested, the worker loads 2, flight 3 starts meanwhile
    fs.frame(10000, 9990, io);             // flight 2
    std::atomic<int> gate{0};
    auto slow = [&](uint64_t f) { gate.store(1); while (gate.load() != 2) std::this_thread::yield(); return make("m0" + std::to_string(f) + "a", 3001 + f * 2, false); };
    std::thread w([&] { fs.work(slow); });
    while (gate.load() != 1) std::this_thread::yield();
    fs.frame(19000, 9990, io);             // flight 2 ends
    fs.frame(20000, 19990, io);            // flight 3 starts (new request while 2 is loading)
    gate.store(2);
    w.join();
    CHECK(fs.pending());                   // 3 still wanted
    CHECK(!fs.frame(20010, 20000, io) && !io);
    CHECK(fs.work(loader));
    CHECK(fs.frame(20020, 20010, io) && io);
    CHECK(fs.cycle().flight() == 3);
}

// A parked stage expires after park_ms; stages without props are freed at once; a new session keeps an old backend's props stages for the process.
void test_park_and_sessions() {
    acz::flight::StageConfig cfg; cfg.park_ms = 60000;
    FS fs(cfg);
    std::shared_ptr<const FakeStage> io;
    fs.begin_session(0);
    fs.frame(100, 90, io);
    fs.work([](uint64_t) { return make("m01a", 4001, false); });
    CHECK(fs.frame(110, 100, io) && io);
    fs.frame(9000, 100, io);               // end -> parked
    CHECK(fs.parked() && FakeStage::alive.load() == 1);
    fs.frame(69001, 100, io);              // 60 001 ms parked -> retired; no props -> freed at once
    CHECK(!fs.parked() && fs.retired() == 0 && FakeStage::alive.load() == 0);
    // with props, across a new session
    fs.frame(70000, 69990, io);
    fs.work([](uint64_t) { return make("m02a", 4003, true); });
    CHECK(fs.frame(70010, 70000, io) && io);
    io = nullptr;                          // the old consumer is gone
    fs.begin_session(80000);               // new attempt: the drawn stage is parked (reusable)
    CHECK(fs.parked());
    fs.frame(80100, 80090, io);            // flight in the new session: a different stage
    fs.work([](uint64_t) { return make("m07a", 4005, true); });
    CHECK(fs.frame(80110, 80100, io) && io && io->id == "m07a");
    CHECK(fs.kept() == 1 && fs.retired() == 0);   // m02a: the old backend's worker may borrow it -> kept for the process
    fs.recorded(io->gen, true, true);
    CHECK(fs.kept() == 1 && FakeStage::alive.load() == 2);
}

// P3 (Astra 6458816088): a stage parked in a LONG first session, then a new session whose owner clock starts near zero: the park expiry
// restarts at the rollover (not never, not the old elapsed + 60 s); the rollover hook reports a session lost in flight.
void test_park_rebase_and_rollover() {
    FS fs;
    unsigned rolls = 0; bool last_in_flight = false, last_had = false;
    FS::Hooks h; h.rollover = [&](uint64_t, bool f, bool had) { ++rolls; last_in_flight = f; last_had = had; };
    fs.set_hooks(h);
    std::shared_ptr<const FakeStage> io;
    fs.begin_session(0);
    CHECK(rolls == 1 && !last_in_flight && !last_had);
    fs.frame(5000000, 4999990, io);   // flight 1 at owner t = 5 000 s
    fs.work([](uint64_t) { return make("m02a", 6001, false); });
    CHECK(fs.frame(5000010, 5000000, io) && io);
    io = nullptr;                     // the session is lost IN FLIGHT (no ended callback)
    fs.begin_session(20);             // new owner clock near zero
    CHECK(rolls == 2 && last_in_flight && last_had && fs.parked());
    fs.frame(60000, -1, io);
    CHECK(fs.parked());               // 59 980 ms since the rollover
    fs.frame(60021, -1, io);
    CHECK(!fs.parked() && FakeStage::alive.load() == 0);   // 60 001 ms: expired in the new clock
}

void test_diagnose_effective_limit() {
    using namespace acz::xr_start;
    // A tolerated 2 s call (frame 10) and the 8 s call that broke the stall limit (frame 40): the latter is named.
    xr_owner::CallRecord rec[3] = {
        {xr_owner::Op::end_frame, xr_owner::Rc::ok, 3000, 5000, 0},
        {xr_owner::Op::wait_frame, xr_owner::Rc::ok, 6000, 6010, 0},
        {xr_owner::Op::end_frame, xr_owner::Rc::ok, 9000, 17100, 0}};
    xr_owner::Report r; r.first_frame_done_ms = 2000; r.begun = 900; r.first_cause = xr_owner::Cause::progress_deadline; r.complete = true; r.clean = true;
    xr_owner::Limits early; early.frame_stall_ms = 8000;
    Diagnosis d = diagnose(r, rec, 3, false, early);
    CHECK(d.has_call && d.call_entry_ms == 9000 && d.call_ms == 8100);
    Diagnosis legacy = diagnose(r, rec, 3, false);   // default limits: the first call >= 250 ms, exactly as before
    CHECK(legacy.has_call && legacy.call_entry_ms == 3000);
}

// The worker on a real second thread against frames on this thread (no lock order problem, results arrive, nothing double-installed).
void test_threads() {
    FS fs;
    std::shared_ptr<const FakeStage> io;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> loads{0};
    std::thread w([&] { while (!stop.load()) { if (!fs.work([&](uint64_t f) { ++loads; return make("s" + std::to_string(f % 3), 5001 + f * 2, (f & 1) != 0); })) std::this_thread::yield(); } });
    fs.begin_session(0);
    int64_t t = 0, last_new = -1; unsigned installed = 0;
    unsigned handshakes = 0;
    for (int flight = 0; flight < 200; ++flight) {
        for (int i = 0; i < 50; ++i) { t += 11; last_new = t; if (fs.frame(t, last_new, io) && io) ++installed; std::this_thread::yield(); }
        if (flight % 10 == 0) {   // bounded handshake: every 10th flight waits (<= 5 s wall) for its load and installs it while still in flight
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool got = bool(io);
            while (!got && std::chrono::steady_clock::now() < until) { t += 11; last_new = t; if (fs.frame(t, last_new, io) && io) { ++installed; got = true; } else std::this_thread::yield(); }
            CHECK(got);
            if (got) ++handshakes;
        }
        for (int i = 0; i < 2; ++i) { t += 9000; if (fs.frame(t, last_new, io)) CHECK(!io); }
    }
    stop.store(true); w.join();
    fs.frame(t + 1, last_new, io);   // drain a result that arrived after the last frame (discarded: not in flight)
    CHECK(fs.cycle().flight() == 200 && fs.cycle().ends() == 200);
    CHECK(handshakes == 20 && installed >= 20 && installed <= 200);
    CHECK(!io && size_t(FakeStage::alive.load()) == fs.retired() + fs.kept() + (fs.parked() ? 1u : 0u) + (fs.current().rt ? 1u : 0u));   // no leak outside the slot
    std::printf("threads: flights=%llu installs=%u loads=%llu %s\n", (unsigned long long)fs.cycle().flight(), installed, (unsigned long long)loads.load(), fs.summary().c_str());
}

void test_retry_policy() {
    using namespace acz::xr_start;
    xr_owner::Report r;
    r.first_frame_done_ms = 2247; r.begun = 60135; r.first_cause = xr_owner::Cause::progress_deadline; r.complete = true; r.clean = true;
    Diagnosis d = diagnose(r, nullptr, 0, false);
    CHECK(d.cls == Class::ended_after_start && d.clean_after_start);
    // default policy (vsearly off): unchanged -> no retry after start
    RetryPolicy off(true);
    off.attempt_started(0);
    CHECK(!off.decide(2, false, d, 674000).retry);
    // early: retried after 3 s, even after 10+ minutes since the first start (test 19 waited 60 s)
    RetryPolicy on(true); on.set_early(true);
    on.attempt_started(0);
    Decision x = on.decide(2, false, d, 674000);
    CHECK(x.retry && x.delay_ms == 3000 && std::strcmp(x.text, "yes-early(after-start)") == 0);
    // the slow-down clock restarted at that end: a transient failure 10 s later is NOT slow-60s
    on.attempt_started(677000);
    Diagnosis tr; tr.cls = Class::transient; tr.reason = "x";
    Decision y = on.decide(3, false, tr, 680000);
    CHECK(y.retry && y.delay_ms == 5000 && std::strcmp(y.text, "yes-persistent") == 0);
    // not retried: an orderly runtime stop, a game exit, an unclean end, ACZ_XR_RETRY=0
    xr_owner::Report ok = r; ok.success = true;
    CHECK(!on.decide(4, false, diagnose(ok, nullptr, 0, false), 700000).retry);
    xr_owner::Report unclean = r; unclean.clean = false;
    Diagnosis du = diagnose(unclean, nullptr, 0, false);
    CHECK(!du.clean_after_start && !on.decide(4, false, du, 700000).retry);
    xr_owner::Report held = r; held.session_held = true;
    CHECK(!diagnose(held, nullptr, 0, false).clean_after_start);
    xr_owner::Report other = r; other.first_cause = xr_owner::Cause::end_frame_failed;
    CHECK(!diagnose(other, nullptr, 0, false).clean_after_start);
    RetryPolicy noenv(false); noenv.set_early(true);
    CHECK(!noenv.decide(2, false, d, 1000).retry);
    // rate cap still applies
    RetryPolicy cap(true, 2); cap.set_early(true);
    cap.attempt_started(0); cap.attempt_started(100);
    Decision z = cap.decide(2, false, d, 200);
    CHECK(z.retry && z.delay_ms == 60000 - 200 && std::strcmp(z.text, "yes-early(rate-capped)") == 0);
}

void test_limits() {
    xr_owner::Limits l;
    CHECK(l.valid() && l.frame_progress_ms() == 250);
    l.frame_stall_ms = 8000;
    CHECK(l.valid() && l.frame_progress_ms() == 8000);
    l.frame_stall_ms = 10001;
    CHECK(!l.valid());
    l.frame_stall_ms = 100;   // below the progress rule: refused (it may only relax the frame phase)
    CHECK(!l.valid());
    l.frame_stall_ms = -1;
    CHECK(!l.valid());
}
} // namespace

int main() {
    test_gate();
    test_cycle();
    test_stage_cycle();
    test_late_results();
    test_park_and_sessions();
    test_park_rebase_and_rollover();
    test_diagnose_effective_limit();
    test_threads();
    test_retry_policy();
    test_limits();
    std::printf("flight_cycle_offline: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
