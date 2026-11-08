// VSCREEN offline tests (docs/world-render/vscreen.md): option parsing/removal, mode decision, placement, follow, the capture ring (single
// thread + a two-thread writer/reader stress that checks no slot is ever rewritten while held or read before its capture completed), and the
// screen-only layer list. No device, no game, no headset.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vscreen.h"
#include "xr_stereo_output.h"
#include "xr_vscreen_core.h"
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace acz;

namespace {
unsigned checks = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { std::cerr << "FAIL " << what << '\n'; std::exit(1); }
}
bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
vscreen::Completion C(uint64_t v) { vscreen::Completion c; c.value = v; return c; }

void options() {
    using vscreen::Options;
    Options o; std::string rest, why;
    // A value without vscreen tokens is unchanged byte for byte (the existing day-install value).
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;tac:1;interp:1;decouple:1";
    check(vscreen::extract_options(day, rest, o, why), "day value parses");
    check(rest == day, "day value unchanged");
    check(!o.on && !o.any_token, "default off");
    check(vscreen::extract_options(day + ";vscreen:1;vsd:3.5;vsw=4;vsy:-0.2;vscurve:1;vsenter:800;vsfollow:1;vspov:flat;vsearly:1", rest, o, why), "all tokens");
    check(rest == day, "tokens removed");
    check(o.on && near(o.distance_m, 3.5f) && near(o.width_m, 4.0f) && near(o.y_m, -0.2f) && o.curve && o.enter_ms == 800 && o.follow &&
          o.pov == vscreen::Pov::flat && o.early && o.any_token, "values");
    check(vscreen::extract_options("auto;vscreen:1;env:1", rest, o, why) && rest == "auto;env:1", "token in the middle");
    check(vscreen::extract_options("vscreen:1", rest, o, why) && rest == "vscreen:1" && !o.on, "the first item is the stage id, never an option");
    check(!vscreen::extract_options("auto;vscreen:2", rest, o, why) && why.find("vscreen") != std::string::npos, "bad flag");
    check(!vscreen::extract_options("auto;vsd:0.1", rest, o, why), "distance range");
    check(!vscreen::extract_options("auto;vsw:x", rest, o, why), "width not a number");
    check(!vscreen::extract_options("auto;vsenter:10", rest, o, why), "enter range");
    check(!vscreen::extract_options("auto;vspov:chase", rest, o, why), "chase pov not offered");
    check(vscreen::extract_options("auto;vsdx:1", rest, o, why) && rest == "auto;vsdx:1", "unknown vs-like key left for the other parsers");
    const std::string d = vscreen::describe(o);
    check(d.find("vscreen=0") == 0, "describe starts with the flag");
    Options e; e.on = true; e.early = true; e.curve = true;
    const std::string de = vscreen::describe(e);
    check(de.find("early=see-vsearly-config") != std::string::npos && de.find("early=ignored") == std::string::npos && de.find("requested-flat") != std::string::npos, "describe says what is not implemented");
}

void mode_machine() {
    vscreen::Options o; o.on = true;   // enter 500
    vscreen::ModeMachine m(o);
    vscreen::ModeIn in;
    in.now_ms = 0; in.flight_age_ms = -1;
    auto r = m.step(in);
    check(r.mode == vscreen::Mode::screen && r.entered && r.want_capture, "no flight since start: screen at once");
    in.now_ms = 10; in.flight_age_ms = -1;
    r = m.step(in);
    check(r.mode == vscreen::Mode::screen && !r.entered, "stays, no re-entry");
    in.now_ms = 20; in.flight_age_ms = 5;
    r = m.step(in);
    check(r.mode == vscreen::Mode::flight && !r.want_capture, "a young flight composite: flight, no capture");
    in.now_ms = 200; in.flight_age_ms = 160;
    r = m.step(in);
    check(r.mode == vscreen::Mode::flight && r.want_capture, "capture requested before the takeover (lead)");
    in.now_ms = 600; in.flight_age_ms = 500;
    r = m.step(in);
    check(r.mode == vscreen::Mode::flight, "exactly the limit: still flight");
    in.now_ms = 601; in.flight_age_ms = 501;
    r = m.step(in);
    check(r.mode == vscreen::Mode::screen && r.entered, "older than vsenter: screen");
    check(m.transitions() == 3, "transitions counted");
    m.flight_frame(700);
    check(m.mode() == vscreen::Mode::flight && m.transitions() == 4, "a fresh flight frame leaves the screen");
    vscreen::Options f = o; f.pov = vscreen::Pov::flat;
    vscreen::ModeMachine mf(f);
    in.flight_age_ms = 1;
    r = mf.step(in);
    check(r.mode == vscreen::Mode::screen && r.want_capture, "vspov:flat: screen even in flight");
    mf.flight_frame(800);
    check(mf.mode() == vscreen::Mode::screen, "vspov:flat ignores flight frames");
}

void placement() {
    vscreen::Options o; o.on = true; o.distance_m = 2.0f; o.width_m = 3.2f; o.y_m = 0.1f;
    const float head[3] = {0.5f, 1.6f, -0.25f};
    const float ident[4] = {0, 0, 0, 1};
    auto p = vscreen::place(o, head, ident, 16.0f / 9.0f, 7);
    check(p.valid && p.generation == 7, "placed");
    check(near(p.pose.p[0], 0.5f) && near(p.pose.p[1], 1.7f) && near(p.pose.p[2], -2.25f), "2 m ahead along -Z, raised by vsy");
    check(near(p.width, 3.2f) && near(p.height, 1.8f), "16:9 height");
    check(near(p.pose.q[3], 1.0f) && near(p.pose.q[1], 0.0f), "facing the viewer");
    // Head turned 90 deg left (about +Y): forward = -X; pitched down 30 deg as well: the screen stays level at the head's yaw.
    const float s = std::sin(0.785398163f), c = std::cos(0.785398163f);   // 90 deg yaw
    const float yawq[4] = {0, s, 0, c};
    p = vscreen::place(o, head, yawq, 16.0f / 9.0f, 1);
    check(near(p.pose.p[0], -1.5f) && near(p.pose.p[2], -0.25f), "turned left: screen at -X");
    check(near(p.yaw, 1.5707963f, 1e-3f), "yaw 90 deg");
    const float ps = std::sin(-0.261799f), pc = std::cos(-0.261799f);   // -30 deg pitch about +X
    float q[4] = {c * ps, s * pc, -s * ps, c * pc};   // yaw * pitch
    p = vscreen::place(o, head, q, 16.0f / 9.0f, 1);
    check(near(p.pose.p[1], 1.7f) && near(p.yaw, 1.5707963f, 1e-3f), "pitch ignored");
    check(near(p.cyl_radius, 2.0f) && near(p.cyl_angle, 1.6f) && near(p.cyl_aspect, 16.0f / 9.0f), "cylinder parameters (not submitted yet)");
    const float bad[3] = {NAN, 0, 0};
    check(!vscreen::place(o, bad, ident, 1.7f, 1).valid, "non-finite head refused");
    p = vscreen::place(o, head, ident, 0.0f, 1);
    check(p.valid && near(p.height, 1.8f), "bad aspect falls back to 16:9");
    // Follow
    vscreen::Follow fw;
    check(!fw.step(true, 0.0f, 0.0f, 0), "looking at it");
    check(!fw.step(true, 1.2f, 0.0f, 100), "away: timer starts");
    check(!fw.step(true, 1.2f, 0.0f, 1500), "not yet 1.5 s");
    check(fw.step(true, 1.2f, 0.0f, 1600), "re-place after 1.5 s away");
    check(!fw.step(false, 3.0f, 0.0f, 9999), "follow off");
    check(!fw.step(true, 1.2f, 0.0f, 10000) && !fw.step(true, 0.1f, 0.0f, 12000) && !fw.step(true, 1.2f, 0.0f, 12100), "looking back resets");
    check(near(vscreen::wrap_pi(6.0f), 6.0f - 6.2831853f, 1e-4f), "wrap");
}

void ring_single() {
    vscreen::Ring r;
    vscreen::Acquired a;
    check(!r.acquire_latest(0, C(100), a), "empty ring");
    const int s0 = r.begin_write(C(0));
    check(s0 >= 0, "write slot");
    r.publish(s0, 1);
    check(!r.acquire_latest(0, C(0), a), "not acquired before its capture completed");
    check(r.acquire_latest(0, C(1), a) && a.slot == s0 && a.id == 1, "acquired after completion");
    check(!r.acquire_latest(1, C(1), a), "nothing newer");
    // While s0 is held the writer uses the others; it never picks the held slot.
    for (int k = 0; k < 10; ++k) {
        const int s = r.begin_write(C(100));
        check(s >= 0 && s != s0, "writer avoids the held slot");
        r.publish(s, 2 + uint64_t(k));
    }
    r.release(a);
    check(r.slot(unsigned(s0)).state == vscreen::slot_free, "released");
    // A slot whose previous capture has not completed is not re-recorded.
    vscreen::Ring r2;
    const int w0 = r2.begin_write(C(0)); r2.publish(w0, 5);
    const int w1 = r2.begin_write(C(0)); r2.publish(w1, 6);
    const int w2 = r2.begin_write(C(0)); r2.publish(w2, 7);
    check(w0 != w1 && w1 != w2 && w0 != w2, "three slots");
    check(r2.begin_write(C(4)) == -1, "every slot's capture in flight: skip");
    check(r2.begin_write(C(5)) == w0, "the oldest completed slot is reused");
    r2.abort(w0);
    check(r2.slot(unsigned(w0)).state == vscreen::slot_free && r2.slot(unsigned(w0)).value == 5, "abort keeps the previous value");
    // The latest ready image is never overwritten (the reader may be about to take it).
    vscreen::Ring r3;
    const int x0 = r3.begin_write(C(0)); r3.publish(x0, 1);
    vscreen::Acquired h; check(r3.acquire_latest(0, C(1), h), "hold x0");
    const int x1 = r3.begin_write(C(1)); r3.publish(x1, 2);
    const int x2 = r3.begin_write(C(2)); r3.publish(x2, 3);
    const int x3 = r3.begin_write(C(3));
    check(x3 == x1, "held and latest are never reused; the older ready slot is");
    r3.abort(x3);
    check(r3.slot(unsigned(x2)).state == vscreen::slot_ready, "the latest image is still acquirable");
    // retire_all refuses while a slot is held, then forgets every image.
    check(!r3.retire_all(C(10)), "retire refused while held");
    r3.release(h);
    check(r3.retire_all(C(10)), "retire when idle");
    check(!r3.acquire_latest(0, C(10), h), "nothing acquirable after retire");
    vscreen::Ring r4;
    const int y0 = r4.begin_write(C(0)); r4.publish(y0, 9);
    check(!r4.retire_all(C(8)), "retire refused while a capture is in flight");
    check(vscreen::capture_wanted(1000, 1000) && !vscreen::capture_wanted(1000, 1001) && !vscreen::capture_wanted(0, 0), "want window");
}

// Writer and reader threads with a fake GPU: the writer "completes" a capture value only some time later; the reader "copies" and completes
// its copy later. Invariants: a slot is never written while held; a held slot's capture value is always completed when acquired.
void ring_stress() {
    vscreen::Ring r;
    std::atomic<uint64_t> completed{0};
    std::atomic<int> held_slot{-1};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> violations{0};
    std::atomic<uint64_t> writes{0}, reads{0};
    std::thread writer([&] {
        uint64_t value = 0;
        for (int i = 0; i < 200000 && !stop.load(); ++i) {
            if ((i & 3) == 0 && completed.load() < value) completed.store(completed.load() + 1);   // the GPU catches up slowly
            const int s = r.begin_write(C(completed.load()));
            if (s < 0) continue;
            if (s == held_slot.load()) violations.fetch_add(1);
            if ((i % 7) == 0) { r.abort(s); continue; }
            r.publish(s, ++value);
            writes.fetch_add(1);
        }
        stop.store(true);
    });
    std::thread reader([&] {
        uint64_t last = 0;
        while (!stop.load()) {
            vscreen::Acquired a;
            const uint64_t c = completed.load();
            if (!r.acquire_latest(last, C(c), a)) { std::this_thread::yield(); continue; }
            if (a.value > c) violations.fetch_add(1);
            if (a.id <= last) violations.fetch_add(1);
            held_slot.store(a.slot);
            last = a.id;
            for (int k = 0; k < 50; ++k) std::this_thread::yield();   // the copy runs
            held_slot.store(-1);
            r.release(a);
            reads.fetch_add(1);
        }
    });
    writer.join(); reader.join();
    check(violations.load() == 0, "no slot written while held; nothing read before its capture completed");
    check(writes.load() > 100 && reads.load() > 10, "stress made progress (writes " + std::to_string(writes.load()) + ", reads " + std::to_string(reads.load()) + ")");
}

void layers() {
    xr_owner::ProjectionLayer l;
    l.screen_only = true;
    l.hud = true; l.quad = true; l.markers = true;   // ignored with screen_only
    const auto list = xr_stereo::compose_layers(l);
    check(list.count == 1, "screen-only: one layer");
    check(list.e[0].kind == xr_stereo::LayerKind::screen_quad && list.e[0].target == xr_stereo::kHud, "the HUD swapchain carries the screen");
    check(!list.e[0].source_alpha && !list.e[0].unpremultiplied && list.e[0].space == xr_stereo::LayerSpace::local, "opaque, LOCAL");
    xr_owner::ProjectionLayer d;   // default: exactly the previous single projection
    const auto dl = xr_stereo::compose_layers(d);
    check(dl.count == 1 && dl.e[0].kind == xr_stereo::LayerKind::projection, "default unchanged");
    xr_owner::ProjectionLayer h; h.hud = true; h.world_composite = true;
    const auto hl = xr_stereo::compose_layers(h);
    check(hl.count == 2 && hl.e[1].kind == xr_stereo::LayerKind::hud_quad, "flight layers unchanged");
}

// Astra finding 2 (ring side): a lost completion refuses reuse, acquisition and re-creation; an Execute-success/Signal-failure slot (left
// WRITING) is never reused or read, before or after a later loss.
void ring_loss() {
    vscreen::Completion lost; lost.lost = true;
    check(vscreen::completion_from_raw(UINT64_MAX).lost && !vscreen::completion_from_raw(7).lost && vscreen::completion_from_raw(7).value == 7,
          "UINT64_MAX is loss, never a value");
    check(!lost.reached(0) && C(5).reached(5) && !C(4).reached(5), "loss reaches nothing");
    vscreen::Ring r;
    const int a = r.begin_write(C(0)); r.publish(a, 1);
    const int b = r.begin_write(C(0));          // Execute succeeded, Signal failed: the writer never publishes nor aborts it
    check(a >= 0 && b >= 0 && a != b, "two slots");
    vscreen::Acquired h;
    check(!r.acquire_latest(0, lost, h), "no acquisition on loss");
    check(r.begin_write(lost) == -1, "no reuse on loss");
    check(!r.retire_all(lost), "no re-creation on loss");
    check(r.stats().lost_refused == 3, "loss refusals counted");
    check(r.slot(unsigned(b)).state == vscreen::slot_writing, "the signal-failure slot stays WRITING");
    for (int k = 0; k < 5; ++k) { const int s = r.begin_write(C(1000)); check(s != b, "never reused"); if (s >= 0) r.abort(s); }
    check(!r.retire_all(C(1000)), "re-creation refused while a slot is WRITING");
    check(r.acquire_latest(0, C(1), h) && h.slot == a, "the completed image is still acquirable when completion is valid");
    r.release(h);
    check(r.begin_write(lost) == -1 && !r.retire_all(lost), "a later loss still refuses everything");
}

// Astra finding 3: the queue comes ONLY from the creation association (the swap chain is never asked for a queue, so a swap chain that exposes
// its device but rejects the queue IID cannot matter), and it is re-validated against the recorded device on use.
void queue_association() {
    vscreen::QueueAssoc as;
    int sc1 = 0, sc2 = 0, q1 = 0, q2 = 0, dev = 0, other = 0;
    const char* why = "none";
    auto device_of = [&](void* q) -> void* { return q == &q1 || q == &q2 ? static_cast<void*>(&dev) : nullptr; };
    check(vscreen::resolve_queue(as, &sc1, device_of, why) == nullptr && std::string(why) == "no-swapchain-queue-association", "no association: no queue");
    as.note(&sc1, &q1, &dev);
    check(vscreen::resolve_queue(as, &sc1, device_of, why) == &q1, "the creation queue");
    check(vscreen::resolve_queue(as, &sc2, device_of, why) == nullptr, "another swap chain: no substitute queue of the same device");
    auto moved = [&](void*) -> void* { return &other; };
    check(vscreen::resolve_queue(as, &sc1, moved, why) == nullptr && std::string(why) == "queue-device-mismatch", "device identity re-checked on use");
    auto failing = [&](void*) -> void* { return nullptr; };
    check(vscreen::resolve_queue(as, &sc1, failing, why) == nullptr && std::string(why) == "queue-device-lookup-failed", "lookup failure refused");
    as.note(&sc1, &q2, &dev);
    check(vscreen::resolve_queue(as, &sc1, device_of, why) == &q2, "a re-created swap chain at the same address replaces the entry");
    as.note(nullptr, &q1, &dev); as.note(&sc2, nullptr, &dev);
    check(vscreen::resolve_queue(as, &sc2, device_of, why) == nullptr, "incomplete notes are ignored");
    int many[12] = {};
    for (auto& m : many) as.note(&m, &q1, &dev);
    check(vscreen::resolve_queue(as, &many[11], device_of, why) == &q1, "bounded table keeps the newest");
}

// ------------------------------------------------------------------ ScreenCore fixtures (findings 1, 2, 4)
struct FakeGate final : xr_owner::Gate {
    int64_t t = 0; bool lost_ = false;
    std::vector<xr_owner::Op> ops;
    int64_t now_ms() override { return t; }
    void sleep_ms(int64_t ms) override { t += ms; }
    bool lost() const override { return lost_; }
    bool stopping() const override { return false; }
protected:
    xr_owner::Rc invoke(xr_owner::Op op, xr_owner::Rc (*fn)(void*), void* p) override {
        ops.push_back(op);
        for (auto r : refuse) if (r == op) return xr_owner::Rc::refused;   // the owner's deadline admission: the callback is NOT invoked
        return fn(p);
    }
public:
    std::vector<xr_owner::Op> refuse;
};
struct FakeChains final : xr_stereo::SwapchainRuntime {
    std::vector<std::string> log;
    bool acquired = false, waited = false; uint32_t next = 0, index = 0, count = 3;
    std::deque<bool> timeouts; bool release_fails = false; unsigned violations = 0;
    xr_owner::Rc view_configs(std::array<xr_stereo::ViewConfig, 2>&) override { return xr_owner::Rc::ok; }
    xr_owner::Rc formats(std::array<int64_t, xr_stereo::max_formats>&, uint32_t& n) override { n = 0; return xr_owner::Rc::ok; }
    xr_owner::Rc create_swapchain(unsigned, int64_t, uint32_t, uint32_t, xr_stereo::SwapchainImages&) override { return xr_owner::Rc::ok; }
    xr_owner::Rc acquire(unsigned t, uint32_t& i) override {
        if (acquired) ++violations;
        acquired = true; waited = false; index = next; next = (next + 1) % count; i = index;
        log.push_back("acquire " + std::to_string(t) + " " + std::to_string(i)); return xr_owner::Rc::ok;
    }
    xr_owner::Rc wait(unsigned t, int64_t, bool& to) override {
        if (!acquired || waited) ++violations;
        to = false; if (!timeouts.empty()) { to = timeouts.front(); timeouts.pop_front(); }
        if (!to) waited = true;
        log.push_back("wait " + std::to_string(t) + (to ? " timeout" : " ok")); return xr_owner::Rc::ok;
    }
    xr_owner::Rc release(unsigned t) override {
        if (!acquired || !waited) ++violations;
        log.push_back("release " + std::to_string(t));
        if (release_fails) return xr_owner::Rc::error;
        acquired = false; waited = false; return xr_owner::Rc::ok;
    }
    xr_owner::Rc destroy_swapchain(unsigned) override { return xr_owner::Rc::ok; }
};
struct FakeGpu final : xr_stereo::ScreenGpu {
    FakeChains* chains = nullptr;
    uint64_t value = 0, completed_v = 0; bool lose = false, record_fails = false, signal_fails = false, destroyed = false;
    std::vector<std::pair<void*, uint64_t>> writes;   // (swapchain image, capture id) per executed copy
    unsigned writes_before_wait = 0, copy_calls = 0, destroys = 0;
    // Partial setup model (Astra follow-up P3): 0 = success; 1 = fails after the fence; 2 = after the first allocator; 3 = at the initial Close.
    int fail_setup_at = 0; unsigned created = 0, released = 0;
    bool setup(const xr_stereo::SwapchainImages&, const char*& why) override {
        ++created;                                   // fence
        if (fail_setup_at == 1) { why = "allocator"; return false; }
        ++created;                                   // allocator 0
        if (fail_setup_at == 2) { why = "allocator"; return false; }
        created += 2;                                // allocator 1 + list
        if (fail_setup_at == 3) { why = "list-close"; return false; }
        return true;
    }
    vscreen::Completion completed() override { vscreen::Completion c; if (lose) c.lost = true; else c.value = completed_v; return c; }
    Submit copy(unsigned, void* dst, const vscreen::CaptureImage& ci, xr_stereo::Extent) override {
        ++copy_calls;
        Submit s;
        if (record_fails) return s;
        if (!chains || !chains->waited) ++writes_before_wait;
        writes.emplace_back(dst, ci.held.id); s.executed = true;
        if (rb_want) { s.readback = !rb_fails && !rb_pending; rb_dst = dst; if (s.readback) { ++rb_recorded; rb_pending = true; } }   // like Readback::record: refused while pending_
        if (signal_fails) return s;
        s.signalled = true; s.value = ++value; return s;
    }
    // SCREEN-DUMP: a 4x2 image whose pixel i = (i, 10+i, 20+i, 255) as stored
    bool rb_want = false, rb_fails = false; void* rb_dst = nullptr; unsigned rb_recorded = 0, rb_taken = 0, rb_discarded = 0; bool rb_pending = false;
    void readback_discard() override { ++rb_discarded; rb_pending = false; }
    void readback_want(bool w) override { rb_want = w; }
    bool readback_take(std::vector<uint8_t>& v, uint32_t& w, uint32_t& h) override {
        ++rb_taken; rb_pending = false; w = 4; h = 2; v.clear();
        for (uint8_t i = 0; i < 8; ++i) { v.push_back(i); v.push_back(uint8_t(10 + i)); v.push_back(uint8_t(20 + i)); v.push_back(255); }
        return true;
    }
    void destroy() override { destroyed = true; ++destroys; released = created; }
};
struct FakeShots final : xr_stereo::ScreenSource {
    uint64_t next_id = 0; bool have = false; unsigned requests = 0, acquired = 0, released = 0, violation = 0; bool holding = false; uint64_t held_id = 0;
    void push() { ++next_id; have = true; }
    void request() override { ++requests; }
    void set_extent(uint32_t, uint32_t) override {}
    bool acquire(uint64_t after, vscreen::CaptureImage& out) override {
        if (!have || next_id <= after) return false;
        if (holding) { ++violation; return false; }
        out = vscreen::CaptureImage{}; out.resource = reinterpret_cast<void*>(uintptr_t(0x7000 + next_id)); out.width = 2400; out.height = 1350;
        out.held.slot = int(next_id % 3); out.held.id = next_id; holding = true; held_id = next_id; ++acquired; return true;
    }
    void release(const vscreen::CaptureImage& img) override { if (!holding || img.held.id != held_id) ++violation; holding = false; ++released; }
    std::string describe() override { return "cap=fake"; }
};
struct CoreRig {
    vscreen::Options o;
    FakeGate g; FakeChains xr;
    std::shared_ptr<FakeGpu> gpu = std::make_shared<FakeGpu>();
    std::shared_ptr<FakeShots> src = std::make_shared<FakeShots>();
    std::shared_ptr<xr_stereo::ScreenCore> core;
    xr_stereo::SwapchainImages imgs;
    xr_owner::FrameInfo info;
    CoreRig() {
        o.on = true; gpu->chains = &xr;
        core = std::make_shared<xr_stereo::ScreenCore>(o, gpu, src);
        imgs.count = 3; for (unsigned i = 0; i < 3; ++i) imgs.images[i] = reinterpret_cast<void*>(uintptr_t(0x3000 + i));
        info.views_valid = true; info.views.view_count = 2; info.space_generation = 1;
        for (int e = 0; e < 2; ++e) { info.views.eyes[e].pose.position[1] = 1.6f; info.views.eyes[e].pose.position[0] = e ? 0.03f : -0.03f; }
    }
    // One no-new-frame XR frame (flight_age -1 = no flight since start). true = the screen layer was planned.
    bool frame(int64_t flight_age = -1) {
        g.t += 11;
        xr_stereo::VScreenIn in; in.flight_age_ms = flight_age;
        xr_owner::LayerPlan plan;
        const bool shown = core->frame(g, info, xr, xr_stereo::kHud, imgs, xr_stereo::Extent{2400, 1350}, in, plan);
        check(shown == (plan.submit && plan.layer.screen_only), "frame() result matches the plan");
        if (shown) check(plan.layer.screen_width > 3.0f && !plan.layer.hud && !plan.layer.quad, "screen-only plan");
        return shown;
    }
};

void core_happy() {
    CoreRig r;
    check(!r.frame(), "no captured image yet: no layer (the old path continues)");
    check(r.src->requests == 1, "capture requested");
    r.src->push();
    check(r.frame(), "a copied and released screen is submitted");
    check(r.xr.log == std::vector<std::string>{"acquire 3 0", "wait 3 ok", "release 3"}, "exact swapchain sequence, image 0");
    check(r.gpu->writes.size() == 1 && r.gpu->writes[0].first == r.imgs.images[0] && r.gpu->writes[0].second == 1, "copy into the waited image 0");
    check(r.core->holding(), "capture slot held until the copy completed");
    check(r.frame() && r.xr.log.size() == 3, "no new capture: re-submitted without any acquire");
    r.gpu->completed_v = 1;
    r.frame();
    check(!r.core->holding() && r.src->released == 1, "released after its copy completed");
    r.src->push();
    check(r.frame() && r.xr.log.back() == "release 3" && r.gpu->writes.back().first == r.imgs.images[1], "next capture into image 1");
    r.gpu->completed_v = 2;
    check(r.core->retire(r.g) && r.gpu->destroyed && r.src->released == 2 && r.src->violation == 0 && r.xr.violations == 0, "clean retire");
}
// Finding 1: a wait that never succeeds poisons; nothing is written or released; retire refuses.
void core_wait_timeout() {
    CoreRig r; r.src->push(); r.xr.timeouts = {true, true};
    check(!r.frame(), "no layer on a wait timeout");
    check(r.core->poisoned(), "poisoned (the acquisition is unsettled)");
    check(r.gpu->writes.empty() && r.gpu->writes_before_wait == 0, "no write before a successful wait");
    check(r.xr.log == std::vector<std::string>{"acquire 3 0", "wait 3 timeout", "wait 3 timeout"}, "never released");
    r.src->push();
    check(!r.frame() && r.xr.log.size() == 3, "nothing touches the swapchain again");
    check(!r.core->retire(r.g) && !r.gpu->destroyed, "retire refuses (the caller retains)");
    check(r.src->released == 1 && r.src->violation == 0, "the unread capture slot was given back");
}
// Finding 1: a failed release poisons as well.
void core_release_failure() {
    CoreRig r; r.src->push(); r.xr.release_fails = true;
    check(!r.frame() && r.core->poisoned(), "release failure poisons");
    check(r.gpu->writes.size() == 1 && r.gpu->writes_before_wait == 0, "the copy followed the successful wait");
    check(!r.core->retire(r.g), "retained");
}
// Finding 4: a recording failure after a successful screen releases an image of unknown content: no layer until a new copy succeeds.
void core_record_failure() {
    CoreRig r; r.src->push();
    check(r.frame(), "screen shown");
    r.gpu->completed_v = 1; r.frame();
    r.gpu->record_fails = true; r.src->push();
    check(!r.frame(), "record failure: the released image is not the screen: no layer");
    check(!r.core->poisoned() && !r.core->content_ok(), "not poisoned, content invalid");
    check(r.xr.log.back() == "release 3" && r.xr.violations == 0, "the waited image was released");
    check(!r.frame(), "still no layer without a new successful copy");
    r.gpu->record_fails = false; r.src->push();
    check(r.frame(), "a new successful copy shows the screen again");
}
// Finding 4: screen -> flight HUD writes the swapchain -> back to screen while the capture is stalled: no stale HUD as the screen.
void core_flight_hud_then_stalled() {
    CoreRig r; r.src->push();
    check(r.frame(), "screen shown");
    r.gpu->completed_v = 1;
    check(!r.frame(10), "a young flight composite: flight mode, no screen");
    r.core->target_written(xr_stereo::kHud);   // Output acquired the HUD swapchain for the flight HUD
    check(!r.frame(900), "screen again but no new capture: the HUD image is NOT submitted as the screen");
    check(r.core->stats().invalidated == 1, "invalidation counted");
    r.src->push();
    check(r.frame(950), "a fresh capture restores the screen");
}
// Finding 2 (consumer side): loss never releases the held slot, never counts as completion, retire refuses.
void core_loss() {
    CoreRig r; r.src->push();
    check(r.frame(), "screen shown");
    r.gpu->lose = true;
    r.frame();
    check(r.core->holding() && r.src->released == 0, "held for ever on loss");
    r.src->push();
    const size_t n = r.xr.log.size();
    r.frame();
    check(r.xr.log.size() == n, "no new acquisition after loss");
    check(!r.core->retire(r.g) && !r.gpu->destroyed, "loss is not retirement");
}
// Finding 2: Execute success + Signal failure, then loss: quarantined for ever, poisoned, retire refuses.
void core_signal_failure_then_loss() {
    CoreRig r; r.src->push(); r.gpu->signal_fails = true;
    check(!r.frame(), "signal failure: no layer");
    check(r.core->quarantined() && r.core->holding() && r.core->poisoned(), "quarantined + poisoned");
    r.gpu->lose = true;
    r.frame(); r.frame();
    check(r.src->released == 0, "the quarantined slot is never released, also after loss");
    check(!r.core->retire(r.g), "retained");
}
// Astra follow-up P2: the owner refuses Op::d3d_execute after a successful wait (e.g. the startup deadline passed during the wait): no GPU
// work at all; the image is released when the release is admitted, otherwise the acquisition is retained and the hook poisoned.
void core_execute_refused(bool release_refused) {
    CoreRig r; r.src->push();
    r.g.refuse.push_back(xr_owner::Op::d3d_execute);
    if (release_refused) r.g.refuse.push_back(xr_owner::Op::release_image);
    const std::string what = release_refused ? "execute+release refused" : "execute refused";
    check(!r.frame(), what + ": no layer");
    check(r.gpu->copy_calls == 0 && r.gpu->writes.empty(), what + ": no GPU reset/record/execute (the callback never ran)");
    check(r.xr.log.size() >= 2 && r.xr.log[0] == "acquire 3 0" && r.xr.log[1] == "wait 3 ok", what + ": acquired and waited");
    check(r.src->released == 1 && !r.core->holding(), what + ": the unread capture slot was given back");
    bool saw_exec = false; for (auto op : r.g.ops) saw_exec = saw_exec || op == xr_owner::Op::d3d_execute;
    check(saw_exec, what + ": the execution went through the owner's guarded call");
    if (release_refused) {
        check(r.xr.acquired && r.core->poisoned(), "release refused: the acquisition is retained and the hook poisoned");
        check(!r.core->retire(r.g) && !r.gpu->destroyed, "release refused: retire refuses (retained)");
    } else {
        check(!r.xr.acquired && !r.core->poisoned() && !r.core->content_ok(), "release admitted: image released, not poisoned, content unknown");
        check(r.core->retire(r.g) && r.gpu->released == r.gpu->created, "clean retire, balanced");
    }
}
// Astra follow-up P3: a partial setup is destroyed at retire (balanced), never reported clean while objects leak.
void core_partial_setup(int at) {
    CoreRig r; r.gpu->fail_setup_at = at; r.src->push();
    check(!r.frame(), "setup failure: no layer");
    check(r.xr.log.empty() && r.gpu->copy_calls == 0, "setup failure: nothing acquired, no copy");
    check(r.core->dead() && r.src->released == 1, "dead; capture slot returned");
    check(r.core->retire(r.g) && r.gpu->destroys == 1 && r.gpu->released == r.gpu->created && r.gpu->created > 0,
          "partial setup (stage " + std::to_string(at) + ") released at retire, balanced");
    check(r.core->retire(r.g) && r.gpu->destroys == 1, "a second retire does not destroy twice");
}
// SCREEN-DUMP (xr_screen_dump.h): armed -> the next copy reads back the image it wrote -> taken after its fence value, rotated 180 degrees.
void core_dump() {
    CoreRig r;
    xr_stereo::DumpImage d;
    check(std::string(r.core->dump_arm(0)) == "no-screen-image" && !r.core->dump_busy(), "dump refused before a screen image exists");
    r.src->push();
    check(r.frame(), "screen shown");
    check(r.core->dump_arm(r.g.t) == nullptr && r.core->dump_busy(), "armed");
    check(std::string(r.core->dump_arm(r.g.t)) == "busy", "one dump at a time");
    r.gpu->completed_v = 1; r.frame();
    check(!r.core->dump_take(r.g, d) && r.gpu->rb_recorded == 0, "no new capture: no copy, nothing taken, still armed");
    r.src->push();
    check(r.frame() && r.gpu->rb_recorded == 1 && r.gpu->rb_dst == r.gpu->writes.back().first, "the next copy reads back the image it wrote");
    check(!r.core->dump_take(r.g, d) && r.gpu->rb_taken == 0, "pending: never mapped before its fence value completed");
    r.gpu->completed_v = 2;
    check(r.core->dump_take(r.g, d) && d.ok && d.part == "screen" && d.w == 4 && d.h == 2 && d.rgba.size() == 32, "taken once");
    // Orientation (Q1a): the SCREEN part (an upright quad image) is rotated 180 degrees so xr_dump_upright.py restores it: stored pixel i
    // (row-major, 4 x 2) lands at 7 - i; the tool's rotation brings pixel 0 (top-left) back to the top-left.
    check(d.rgba[0] == 7 && d.rgba[1] == 17 && d.rgba[2] == 27 && d.rgba[28] == 0 && d.rgba[29] == 10, "screen part rotated 180 degrees");
    { std::vector<uint8_t> up = d.rgba; xr_stereo::rotate180(up); check(up[0] == 0 && up[4] == 1 && up[16] == 4 && up[28] == 7, "upright after the tool's rotation"); }
    check(!r.core->dump_busy() && !r.core->dump_take(r.g, d), "idle after the take");
    r.frame(); r.src->push(); r.frame();
    check(r.gpu->rb_recorded == 1, "no readback without an armed dump");
    // given up: no new copy within kArmMs
    r.gpu->completed_v = 3; r.frame();
    check(r.core->dump_arm(r.g.t) == nullptr, "armed again");
    r.g.t += xr_stereo::DumpTrack::kArmMs + 10; r.frame();
    d = {}; check(r.core->dump_take(r.g, d) && !d.ok && std::string(d.why) == "no-new-copy" && !r.core->dump_busy(), "no new copy: given up");
    // the readback could not be recorded
    check(r.core->dump_arm(r.g.t) == nullptr, "armed (readback refused)");
    r.gpu->rb_fails = true; r.src->push(); r.frame(); r.gpu->rb_fails = false;
    d = {}; check(r.core->dump_take(r.g, d) && !d.ok && std::string(d.why) == "readback-setup", "readback refused: given up");
    // pending, then the private fence is lost
    r.gpu->completed_v = r.gpu->value; r.frame();
    check(r.core->dump_arm(r.g.t) == nullptr, "armed (loss)");
    r.src->push(); r.frame();
    r.gpu->lose = true;
    d = {}; check(r.core->dump_take(r.g, d) && !d.ok && std::string(d.why) == "fence-lost" && r.gpu->rb_taken == 1, "loss: never mapped");
    check(r.src->violation == 0 && r.xr.violations == 0, "no ownership violation");
}
// A pending readback whose fence value never completes is given up after kPendingMs (never waited for, never mapped).
void core_dump_pending_timeout() {
    CoreRig r; r.src->push(); r.frame(); r.gpu->completed_v = 1; r.frame();
    check(r.core->dump_arm(r.g.t) == nullptr, "armed");
    r.src->push(); r.frame();
    xr_stereo::DumpImage d;
    check(!r.core->dump_take(r.g, d), "pending");
    r.g.t += xr_stereo::DumpTrack::kPendingMs + 10; r.frame();
    check(r.core->dump_take(r.g, d) && !d.ok && std::string(d.why) == "readback-timeout" && r.gpu->rb_taken == 0, "timeout: given up unmapped");
    // SD-2: the cancelled readback is DRAINING (its fence value kept): never discarded while the GPU may still write it, and no fresh arm yet
    check(r.core->dump_busy() && std::string(r.core->dump_arm(r.g.t)) == "busy", "timeout: draining, a new trigger is refused (not readback-setup later)");
    check(!r.core->dump_take(r.g, d) && r.gpu->rb_discarded == 0 && r.core->dump_busy(), "draining: fence not complete -> not released");
    r.gpu->completed_v = 0; r.frame(); check(!r.core->dump_take(r.g, d) && r.gpu->rb_discarded == 0, "still not complete: still held");
    // late completion: reclaimed (once), then a second trigger records and is taken
    r.gpu->completed_v = r.gpu->value; r.frame();
    check(!r.core->dump_take(r.g, d) && r.gpu->rb_discarded == 1 && !r.core->dump_busy() && r.gpu->rb_taken == 0, "late completion: reclaimed once, never mapped");
    check(!r.core->dump_take(r.g, d) && r.gpu->rb_discarded == 1, "reclaimed only once");
    check(r.core->dump_arm(r.g.t) == nullptr, "fresh arm after reclamation");
    r.src->push(); check(r.frame() && r.gpu->rb_recorded == 2, "second trigger: the readback is recorded (no readback-setup)");
    r.gpu->completed_v = r.gpu->value;
    d = {}; check(r.core->dump_take(r.g, d) && d.ok && d.part == "screen" && r.gpu->rb_taken == 1, "second trigger: taken");
}
// A lost fence never proves completion: the readback stays draining (never released) until destroy().
void core_dump_lost_stays_draining() {
    CoreRig r; r.src->push(); r.frame(); r.gpu->completed_v = 1; r.frame();
    check(r.core->dump_arm(r.g.t) == nullptr, "armed");
    r.src->push(); r.frame();
    r.gpu->lose = true;
    xr_stereo::DumpImage d;
    check(r.core->dump_take(r.g, d) && !d.ok && std::string(d.why) == "fence-lost", "loss: given up");
    for (int i = 0; i < 3; ++i) check(!r.core->dump_take(r.g, d) && r.gpu->rb_discarded == 0 && r.core->dump_busy(), "lost: still draining, never released");
}
} // namespace

int main() {
    options();
    mode_machine();
    placement();
    ring_single();
    ring_stress();
    layers();
    ring_loss();
    queue_association();
    core_happy();
    core_wait_timeout();
    core_release_failure();
    core_record_failure();
    core_flight_hud_then_stalled();
    core_loss();
    core_signal_failure_then_loss();
    core_execute_refused(false);
    core_execute_refused(true);
    core_partial_setup(1);
    core_partial_setup(2);
    core_partial_setup(3);
    core_dump();
    core_dump_pending_timeout();
    core_dump_lost_stays_draining();
    std::cout << "vscreen_offline: " << checks << " checks passed\n";
    return 0;
}
