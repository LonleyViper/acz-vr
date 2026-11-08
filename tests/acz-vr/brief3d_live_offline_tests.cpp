// BRIEF3D live wiring offline tests: the PRODUCER core (src/acz-vr/brief3d_live.h) driven with fake lists, queues and fence completions.
// D-370 facts as inputs; the split-phase sequences of Astra review 6460640585 (P2-1..P2-3, P2-5) as deterministic fixtures. Nothing here touches
// D3D12; fixture success says nothing about the game or a headset.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "brief3d_live.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace acz::brief3d;
using namespace acz::brief3d::live;
using acz::vscreen::Completion;

static std::atomic<unsigned> g_checks{0};   // CHECK runs on worker threads too (P2-5)
#define CHECK(c) do { g_checks.fetch_add(1, std::memory_order_relaxed); if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

namespace {

const float kB10[2] = {682.0f, -448.0f};   // D-370

EyeView make_eye(double px, double py, double pz, double yaw, double pitch, double ipd_x) {
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    EyeView e;
    e.orientation[0] = cy * sp; e.orientation[1] = sy * cp; e.orientation[2] = -sy * sp; e.orientation[3] = cy * cp;
    const double off[3] = {ipd_x, 0, 0}; double r[3]; quat_rotate(e.orientation, off, r);
    e.position[0] = px + r[0]; e.position[1] = py + r[1]; e.position[2] = pz + r[2];
    e.fov[0] = -0.82; e.fov[1] = 0.74; e.fov[2] = 0.79; e.fov[3] = -0.87;
    return e;
}
struct Scene { Box box; uint8_t grid[64]; uint8_t icon[272]; };
void set_icon_height(Scene& s, float y);
Scene make_scene(double cam_yaw) {
    Scene s;
    s.box.valid = true; s.box.vertices = 32512;
    s.box.lo[0] = -60; s.box.lo[1] = 0; s.box.lo[2] = -60; s.box.hi[0] = 60; s.box.hi[1] = 6; s.box.hi[2] = 60;
    EyeView cam = make_eye(std::sin(cam_yaw) * 60.0, 60.0, std::cos(cam_yaw) * 60.0, cam_yaw, -0.78, 0.0);
    cam.fov[0] = -0.6; cam.fov[1] = 0.6; cam.fov[2] = 0.35; cam.fov[3] = -0.35;
    Mat4 P; ps2_projection(cam.fov, kB10, 0.5, DepthMode::reverse, P);
    store(mul(view_from_eye(cam), P), s.grid);
    for (unsigned i = 0; i < 272; ++i) s.icon[i] = uint8_t(i * 13 + 1);
    std::memcpy(s.icon, s.grid, 64);   // matrix 0 = the grid matrix (D-370 eq=63.0.0.0)
    for (int k = 1; k < 4; ++k) { float v[16]; for (int i = 0; i < 16; ++i) v[i] = float(k * 10 + i); std::memcpy(s.icon + 64 * k, v, 64); }
    set_icon_height(s, 2.0f);   // matrix 3 translation y: an overview aircraft (live: 1.6..3.2 units over a 64-unit grid)
    return s;
}
void set_icon_height(Scene& s, float y) { std::memcpy(s.icon + 64 * kIconModelMatrix + 4 * 13, &y, 4); }
DrawFacts facts(Kind k, const void* list, int64_t now, const Scene& sc) {
    DrawFacts f;
    f.kind = k; f.list = list; f.now_ms = now;
    f.root_why = nullptr; f.t10_param = 0; f.t10_va = 0x7000100; f.t10_why = nullptr;
    if (k == Kind::selmap) std::memcpy(f.rec, sc.grid, 64); else std::memcpy(f.rec, sc.icon, 272);
    f.b10_why = nullptr; f.b10[0] = kB10[0]; f.b10[1] = kB10[1];
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_write = true; f.depth_func = kCmpGreaterEqual; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.om_known = true;
    return f;
}
PoseIn pose(uint32_t gen = 1) {
    PoseIn p; p.ok = true; p.space_generation = gen;
    p.eyes[0] = make_eye(0.0, 1.2, 0.0, 0.1, -0.6, -0.032); p.eyes[1] = make_eye(0.0, 1.2, 0.0, 0.1, -0.6, 0.032);
    return p;
}
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
Options on_opts() { Options o; o.on = true; return o; }
const uint64_t kUploadVa = 0x40000000ull;

Producer& fresh(Producer& p) { p.configure(on_opts()); p.gpu_ready(kUploadVa); return p; }
// grant only (the reservation stays open)
Plan grant(Producer& p, Kind k, const void* list, int64_t now, const Scene& sc, uint64_t completed = 0, uint32_t gen = 1) {
    const DrawFacts f = facts(k, list, now, sc);
    const PoseIn ps = p.wants_poses(now) ? pose(gen) : PoseIn{};
    const bool box = p.wants_box(now) && k == Kind::selmap;
    return p.draw(f, ps, box ? &sc.box : nullptr, nullptr, done(completed));
}
// grant + commit
Plan step(Producer& p, Kind k, const void* list, int64_t now, const Scene& sc, uint64_t completed = 0, uint32_t gen = 1) {
    Plan pl = grant(p, k, list, now, sc, completed, gen);
    if (pl.record) p.commit(pl);
    return pl;
}
Producer::ExecToken execute(Producer& p, const void* q, const void* list, bool signalled = true) {
    const void* ls[1] = {list};
    const auto t = p.before_execute(q, ls, 1);
    p.after_execute(t, signalled);
    return t;
}
bool can_acquire(Producer& p, uint64_t completed, Held* out = nullptr) {
    Held h; if (!p.acquire(0, done(completed), h)) return false;
    if (out) *out = h; else p.release(h);
    return true;
}

void test_refusals_before_ready() {
    Producer p; const Scene sc = make_scene(0.3); int L = 0;
    CHECK(streq(step(p, Kind::selmap, &L, 10, sc).why, "brief3d-off"));
    p.configure(on_opts());
    CHECK(streq(step(p, Kind::selmap, &L, 10, sc).why, "brief3d-gpu-not-ready"));
    p.gpu_ready(kUploadVa);
    DrawFacts f = facts(Kind::selmap, &L, 10, sc); f.t10_why = "b3cap-read-fault";
    CHECK(streq(p.draw(f, pose(), &sc.box, nullptr, done(0)).why, "b3cap-read-fault"));
    CHECK(streq(step(p, Kind::modelicon, &L, 10, sc).why, "brief3d-no-fit-yet"));
    CHECK(p.stats().frames == 0 && p.tracked_lists() == 0);
}

void test_happy_frame_publishes_at_reset() {
    Producer p; fresh(p);
    const Scene sc = make_scene(0.3); int L = 0, Q = 0;
    const Plan g = step(p, Kind::selmap, &L, 100, sc);
    CHECK(g.record && g.slot >= 0 && g.clear && g.index == 0 && g.rec_bytes == 64 && g.clear_depth == 0.0f);
    char line[600]; CHECK(p.take_fit_line(line, sizeof line) && std::string(line).find("up=1+") != std::string::npos);
    CHECK(g.swap.eye_va[0] == kUploadVa + g.upload_offset[0] && g.swap.restore_va == 0x7000100);
    for (int i = 0; i < 63; ++i) {
        const Plan ic = step(p, Kind::modelicon, &L, 101, sc);
        CHECK(ic.record && !ic.clear && ic.slot == g.slot && ic.index == unsigned(i));
        for (unsigned e = 0; e < 2; ++e) CHECK(std::memcmp(ic.eye_rec[e], g.eye_rec[e], 64) == 0 && std::memcmp(ic.eye_rec[e][0] + 64, sc.icon + 64, 208) == 0 && ic.copies == 1);
    }
    execute(p, &Q, &L);
    p.on_present(120);
    CHECK(!can_acquire(p, 100));   // executed and presented but NOT retired: the recording could still execute again
    p.on_reset(&L, false);         // a failed Reset is no retirement witness
    CHECK(!can_acquire(p, 100));
    p.on_reset(&L, true);
    CHECK(!can_acquire(p, 0));     // retired, value 1 not completed
    Held h; CHECK(can_acquire(p, 1, &h) && h.slot == g.slot && h.meta.replays == 64 && h.meta.space_generation == 1);
    p.release(h);
    CHECK(p.stats().published == 1 && p.tracked_lists() == 0 && !p.poisoned());
    char sum[800]; CHECK(p.format_summary(sum, sizeof sum) > 0 && std::strlen(sum) < kLineLimit - kLinePrefix);
    // the xr-stereo lines: lag at the acquire (no newer producer frame; published and acquired at the same Present tick)
    char tail[kLineLimit], xr[kLineLimit]; p.format_xr(tail, sizeof tail, xr, sizeof xr);
    const std::string xs = xr;
    CHECK(xs.find("xr-stereo;event=summary-brief3d-producer;frames=1;published=1;") == 0 && xs.find(";lag-frames=0;lag-max=0;age-ms=0;") != std::string::npos);
    CHECK(xs.find(";other-list=0;") != std::string::npos && xs.find(";quarantined=0;") != std::string::npos && xs.find(";poisoned=0;why=none") != std::string::npos);
    CHECK(std::string(tail).find("fits=1;fit-refused=0;placements=1;") == 0);
}

// The split xr-stereo lines stay under the log capacity for ANY values (the combined line was cut at 480 bytes in batch Z).
void test_xr_lines_worst_case() {
    Stats s; uint64_t* f = &s.frames;
    for (uint64_t* q = f; q <= &s.age_max_ms; ++q) *q = ~0ull;   // every counter at its maximum
    const char* why = "brief3d-a-very-long-reason-string-that-is-cut-here";
    char tail[kLineLimit], xr[kLineLimit];
    const int a = format_xr_producer(xr, sizeof xr, s, true, why), b = format_xr_tail(tail, sizeof tail, s, ~0ull, why);
    CHECK(a > 0 && size_t(a) < kLineLimit && b > 0 && size_t(b) < kLineLimit);
    CHECK(std::string(xr).find(";replays=999999999;") != std::string::npos && std::string(xr).find(";quarantined=9999999;") != std::string::npos);
    CHECK(std::string(xr).find(";age-max-ms=9999999;poisoned=1;") != std::string::npos);
    std::printf("xr producer line worst case %d bytes, tail %d bytes\n", a, b);
}

// D-384: icons whose PSO pairs the icon VS with another pixel shader are replayed in the same frame/list/slot (no lifetime rule is touched) and counted.
void test_icons_with_other_pixel_shader() {
    Producer p; fresh(p);
    const Scene sc = make_scene(0.3); int L = 0, Q = 0;
    const Plan g = step(p, Kind::selmap, &L, 100, sc);
    CHECK(g.record);
    DrawFacts a = facts(Kind::modelicon, &L, 101, sc); a.ps8 = 0x1234abcdu;
    const Plan ic = p.draw(a, pose(), &sc.box, nullptr, done(0));
    CHECK(ic.record && ic.slot == g.slot && ic.index == 0 && !ic.clear);
    p.commit(ic);
    const Plan ic2 = step(p, Kind::modelicon, &L, 101, sc);   // the standard pixel shader
    CHECK(ic2.record && ic2.index == 1);
    int other = 0; DrawFacts b = facts(Kind::modelicon, &other, 101, sc); b.ps8 = 0x1234abcdu;   // another list while the frame is open: still refused
    CHECK(streq(p.draw(b, pose(), &sc.box, nullptr, done(0)).why, "brief3d-frame-other-list"));
    CHECK(p.stats().icons_other_ps == 2 && p.stats().other_list == 1);
    execute(p, &Q, &L);
    char sum[800]; CHECK(p.format_summary(sum, sizeof sum) > 0 && std::string(sum).find(";icons-other-ps=2;") != std::string::npos);
}

// Lag: the consumer acquires a slot after newer producer frames were opened and Present ticks passed.
void test_acquire_lag() {
    Producer p; fresh(p);
    const Scene sc = make_scene(0.3); int L = 0, M = 0, Q = 0;
    CHECK(step(p, Kind::selmap, &L, 100, sc).record);
    execute(p, &Q, &L); p.on_present(100); p.on_reset(&L, true);              // frame 1 published at t=100
    CHECK(step(p, Kind::selmap, &M, 116, sc).record); p.on_present(150);      // frame 2 opened, present clock 150
    Held h; CHECK(can_acquire(p, 1, &h) && h.meta.frame == 1); p.release(h);
    const Stats s = p.stats();
    CHECK(s.lag_frames == 1 && s.lag_max == 1 && s.age_ms == 50 && s.age_max_ms == 50);
}

// P2-1a: Present / Reset between permission and commit: no settlement while the recording reservation is open.
void test_recording_reservation() {
    Producer p; fresh(p); const Scene sc = make_scene(0.3); int L = 0, Q = 0;
    CHECK(step(p, Kind::selmap, &L, 10, sc).record);
    const Plan open = grant(p, Kind::modelicon, &L, 11, sc);      // the glue is still writing / recording
    CHECK(open.record);
    execute(p, &Q, &L);
    p.on_present(12);
    p.on_reset(&L, true);
    CHECK(!can_acquire(p, 99) && p.stats().published == 0 && p.stats().aborted == 0);   // deferred
    p.commit(open);
    CHECK(can_acquire(p, 1) && p.stats().published == 1);
    // the same with a never-executed list: the abort also waits for the reservation
    Producer q; fresh(q); int M = 0;
    CHECK(step(q, Kind::selmap, &M, 10, sc).record);
    const Plan o2 = grant(q, Kind::modelicon, &M, 11, sc);
    q.on_present(12); q.on_reset(&M, true);
    CHECK(q.stats().aborted == 0);
    q.commit(o2);
    CHECK(q.stats().aborted == 1 && q.stats().published == 0);
    // a stale commit (another frame) is ignored
    q.commit(o2);
    CHECK(q.stats().replays == 2);
}

// P2-1b: the last list's submission is reserved until its Signal outcome; Present/Reset in between cannot publish an earlier value.
void test_submission_reservation() {
    Producer p; fresh(p); const Scene sc = make_scene(0.3); int L = 0, Q = 0;
    CHECK(step(p, Kind::selmap, &L, 10, sc).record);
    const auto t1 = execute(p, &Q, &L);                         // first execution, signalled 1
    const void* ls[1] = {&L};
    const auto t2 = p.before_execute(&Q, ls, 1);                // second execution reserved (native Execute + Signal not yet done)
    CHECK(t2.signal && t2.value == t1.value + 1);
    p.on_present(20);
    p.on_reset(&L, true);
    CHECK(!can_acquire(p, t1.value) && p.stats().published == 0);   // not with A's earlier value
    p.after_execute(t2, true);
    CHECK(!can_acquire(p, t1.value));
    CHECK(can_acquire(p, t2.value) && p.stats().reexecutes == 1);
}

// P2-2: re-execution before retirement raises the value; after retirement the list no longer targets the slot; held/recycled slots stay safe.
void test_reexecution_and_reuse() {
    Producer p; fresh(p); const Scene sc = make_scene(0.3); int L = 0, Q = 0;
    const Plan g = step(p, Kind::selmap, &L, 10, sc);
    execute(p, &Q, &L); p.on_present(11);
    const auto t2 = execute(p, &Q, &L);                          // legal re-execution after the first completed
    p.on_reset(&L, true);
    Held h; CHECK(!can_acquire(p, t2.value - 1) && can_acquire(p, t2.value, &h) && h.slot == g.slot);
    // HELD: the retired list executed again (re-recorded by the game without map draws) is not ours any more: no Signal requested
    const auto t3 = execute(p, &Q, &L);
    CHECK(!t3.signal && !p.poisoned());
    // while HELD the slot is never taken for writing: three more frames use the other slots / wait
    std::vector<int> used;
    for (int f = 0; f < 3; ++f) {
        const Plan n = step(p, Kind::selmap, &L, 20 + f, sc, 1000);
        if (n.record) { used.push_back(n.slot); CHECK(n.slot != h.slot); execute(p, &Q, &L); p.on_present(20 + f); p.on_reset(&L, true); }
    }
    CHECK(!used.empty());
    p.release(h);
    // recycled only after release + completion
    const Plan r = step(p, Kind::selmap, &L, 40, sc, 1000);
    CHECK(r.record);
}

// P2-2: second queue, also for a RE-execution with Q1 work pending: no Signal on the shared fence, slot quarantined for ever, poison blocks admission.
void test_second_queue() {
    const Scene sc = make_scene(0.3);
    {
        Producer p; fresh(p); int L = 0, Q1 = 0, Q2 = 0;
        const Plan g = step(p, Kind::selmap, &L, 10, sc);
        const void* ls[1] = {&L};
        const auto t1 = p.before_execute(&Q1, ls, 1);            // Q1 work pending (reserved)
        const auto tq2 = p.before_execute(&Q2, ls, 1);           // re-execution on Q2
        CHECK(t1.signal && !tq2.signal && p.poisoned() && streq(p.poison_why(), "brief3d-second-queue"));
        p.after_execute(t1, true);
        p.on_present(20); p.on_reset(&L, true);
        CHECK(p.slot_quarantined(unsigned(g.slot)) && p.ring_slot(unsigned(g.slot)).state == acz::vscreen::slot_writing);
        CHECK(!can_acquire(p, 1000) && p.stats().published == 0);
    }
    {   // poison after a publication: the READY slot is no longer admitted (atomic with admission)
        Producer p; fresh(p); int L = 0, M = 0, Q1 = 0, Q2 = 0;
        CHECK(step(p, Kind::selmap, &L, 10, sc).record);
        execute(p, &Q1, &L); p.on_present(11); p.on_reset(&L, true);
        CHECK(can_acquire(p, 1));
        CHECK(step(p, Kind::selmap, &M, 12, sc, 1).record);
        execute(p, &Q2, &M);
        CHECK(p.poisoned() && !can_acquire(p, 1000));
    }
}

// P2-3: one list per frame, clear first, a cancelled clear invalidates, frames never share a slot (per-slot depth).
void test_clear_and_lists() {
    const Scene sc = make_scene(0.3);
    {   // other list while the frame is open: refused (no reversed/interleaved execution across lists can exist)
        Producer p; fresh(p); int A = 0, B = 0, Q = 0;
        const Plan a1 = step(p, Kind::selmap, &A, 10, sc);
        CHECK(a1.record && a1.clear);
        const Plan b1 = step(p, Kind::modelicon, &B, 10, sc);
        CHECK(!b1.record && streq(b1.why, "brief3d-frame-other-list") && p.stats().other_list == 1);
        p.on_present(11);
        const Plan b2 = step(p, Kind::selmap, &B, 12, sc);         // the next frame in B: its own slot, its own clear
        CHECK(b2.record && b2.clear && b2.slot != a1.slot);
        // A still pending: its list may not start another frame
        p.on_present(13);
        const Plan a2 = step(p, Kind::selmap, &A, 14, sc);
        CHECK(!a2.record && streq(a2.why, "brief3d-list-holds-previous-frame"));
        // execute B before A (reversed): each frame's slot only depends on its own list
        execute(p, &Q, &B); p.on_reset(&B, true);
        execute(p, &Q, &A); p.on_reset(&A, true);
        CHECK(p.stats().published == 2);
    }
    {   // discarded initializer: the clear-bearing plan cancelled, the frame executed: never published (quarantined slot)
        Producer p; fresh(p); int A = 0, Q = 0;
        const Plan c = grant(p, Kind::selmap, &A, 10, sc);
        CHECK(c.record && c.clear);
        p.cancel(c);
        const Plan i = step(p, Kind::modelicon, &A, 10, sc);
        CHECK(i.record && !i.clear);                               // the clear is not granted twice
        execute(p, &Q, &A); p.on_present(11); p.on_reset(&A, true);
        CHECK(p.stats().invalidated == 1 && p.stats().published == 0 && p.slot_quarantined(unsigned(c.slot)) && !can_acquire(p, 100));
    }
    {   // cancelled clear, never executed: aborted (no GPU work), slot reusable
        Producer p; fresh(p); int A = 0;
        const Plan c = grant(p, Kind::selmap, &A, 10, sc);
        p.cancel(c); p.on_present(11); p.on_reset(&A, true);
        CHECK(p.stats().aborted == 1 && !p.slot_quarantined(unsigned(c.slot)));
    }
}

void test_signal_failure_and_discard() {
    const Scene sc = make_scene(2.0);
    {
        Producer p; fresh(p); int L = 0, Q = 0;
        const Plan g = step(p, Kind::selmap, &L, 10, sc);
        execute(p, &Q, &L, false);
        p.on_present(20); p.on_reset(&L, true);
        CHECK(!can_acquire(p, 1000) && p.slot_quarantined(unsigned(g.slot)) && p.ring_slot(unsigned(g.slot)).state == acz::vscreen::slot_writing);
        CHECK(p.poisoned() && streq(p.poison_why(), "brief3d-signal-failed"));
    }
    {
        Producer p; fresh(p); int L = 0;
        CHECK(step(p, Kind::selmap, &L, 10, sc).record);
        p.on_reset(&L, true);                                      // reset before execution: discarded, the frame closes with it
        CHECK(p.stats().discarded == 1 && p.stats().aborted == 1 && p.tracked_lists() == 0);
        CHECK(step(p, Kind::selmap, &L, 11, sc).record);           // the reset list may start a new frame
    }
    {
        Producer p; fresh(p); p.gpu_failed("brief3d-gpu-create-rt");
        int L = 0; CHECK(streq(step(p, Kind::selmap, &L, 10, sc).why, "brief3d-gpu-create-rt"));
    }
}

void test_ring_backpressure() {
    Producer p; fresh(p); const Scene sc = make_scene(0.7); int L = 0, Q = 0;
    for (int f = 0; f < 3; ++f) { CHECK(step(p, Kind::selmap, &L, 10 + f * 10, sc, 0).record); execute(p, &Q, &L); p.on_present(15 + f * 10); p.on_reset(&L, true); }
    const Plan g4 = step(p, Kind::selmap, &L, 50, sc, 0);
    CHECK(!g4.record && streq(g4.why, "brief3d-no-free-slot"));
    CHECK(step(p, Kind::selmap, &L, 60, sc, 3).record);
    Producer q; fresh(q);
    Completion lost; lost.lost = true;
    CHECK(streq(q.draw(facts(Kind::selmap, &L, 10, sc), pose(), &sc.box, nullptr, lost).why, "brief3d-fence-lost"));
}

void test_fit_and_screens() {
    Producer p; fresh(p); const Scene sc = make_scene(0.3); int L = 0;
    Box tall = sc.box; tall.hi[1] = 200;
    DrawFacts f = facts(Kind::selmap, &L, 1000, sc);
    CHECK(streq(p.draw(f, pose(), &tall, nullptr, done(0)).why, "brief3d-box-not-flat"));
    CHECK(!p.wants_box(1500) && p.wants_box(2000));
    f.now_ms = 2000;
    CHECK(streq(p.draw(f, pose(), nullptr, "b3cap-record-torn", done(0)).why, "b3cap-record-torn"));
    f.now_ms = 3000;
    const Plan g = p.draw(f, pose(), &sc.box, nullptr, done(0));
    CHECK(g.record); p.commit(g); p.on_present(3010); p.on_reset(&L, true);
    CHECK(!p.wants_box(6000) && p.wants_box(40000));   // brief3d.md 10: fit and placement survive a 3 s gap, not a 37 s one
    const Stats a = p.stats();
    CHECK(step(p, Kind::selmap, &L, 40000, sc).record);
    CHECK(p.stats().fits == a.fits + 1 && p.stats().placements == a.placements + 1);
    p.on_present(40010); p.on_reset(&L, true);
    CHECK(step(p, Kind::selmap, &L, 40020, sc, 9, 2).record && p.stats().placements == a.placements + 2);   // recentre: re-placed
}

void test_gate_with_live_facts() {
    const Scene sc = make_scene(0.3); int L = 0;
    struct Case { void (*mut)(DrawFacts&); const char* why; };
    const Case cases[] = {
        {[](DrawFacts& f) { f.stencil_always = false; }, "brief3d-pso-stencil-test"},
        {[](DrawFacts& f) { f.rtv0 = 24; }, "brief3d-rt-format-differs"},
        {[](DrawFacts& f) { f.dsv = 44; }, "brief3d-dsv-format-differs"},
        {[](DrawFacts& f) { f.depth_func = kCmpNotEqual; }, "brief3d-pso-depth-func"},
        {[](DrawFacts& f) { f.rs_known = false; }, "brief3d-viewport-unknown"},
        {[](DrawFacts& f) { f.om_known = false; }, "brief3d-om-unknown"},
        {[](DrawFacts& f) { f.pso_known = false; }, "brief3d-pso-unknown"},
    };
    for (const auto& c : cases) {
        Producer p; fresh(p);
        DrawFacts f = facts(Kind::selmap, &L, 10, sc); c.mut(f);
        const Plan g = p.draw(f, pose(), &sc.box, nullptr, done(0));
        CHECK(!g.record && streq(g.why, c.why));
    }
    Producer p; fresh(p);
    CHECK(streq(p.draw(facts(Kind::selmap, &L, 10, sc), PoseIn{}, &sc.box, nullptr, done(0)).why, "brief3d-no-pose"));
    CHECK(step(p, Kind::selmap, &L, 15, sc).record);
    p.on_present(16); p.on_reset(&L, true);
    PoseIn in = pose();
    for (auto& e : in.eyes) { e.position[1] = 0.76; e.position[2] = -0.74; }
    CHECK(streq(p.draw(facts(Kind::selmap, &L, 20, sc), in, nullptr, nullptr, done(0)).why, "brief3d-eye-inside-volume"));
    CHECK(step(p, Kind::selmap, &L, 30, sc).record);
    const Scene other = make_scene(2.5);
    CHECK(streq(p.draw(facts(Kind::modelicon, &L, 30, other), PoseIn{}, nullptr, nullptr, done(0)).why, "brief3d-icon-camera-unmatched"));
}

void test_threads() {
    // two recording threads (only the frame's list is admitted), a present thread and an execute thread racing; the counter is atomic (P2-5)
    Producer p; fresh(p); const Scene sc = make_scene(0.3); int A = 0, B = 0, Q = 0;
    CHECK(step(p, Kind::selmap, &A, 10, sc).record);
    std::atomic<unsigned> okA{0}, okB{0};
    std::thread t1([&] { for (int i = 0; i < 60; ++i) if (step(p, Kind::modelicon, &A, 10, sc).record) ++okA; });
    std::thread t2([&] { for (int i = 0; i < 60; ++i) if (step(p, Kind::modelicon, &B, 10, sc).record) ++okB; });
    t1.join(); t2.join();
    CHECK(okA == 60 && okB == 0 && p.stats().replays == 61 && p.stats().other_list == 60);
    std::thread e([&] { execute(p, &Q, &A); });
    std::thread pr([&] { p.on_present(20); });
    e.join(); pr.join();
    p.on_reset(&A, true);
    Held h; CHECK(can_acquire(p, 1, &h) && h.meta.replays == 61); p.release(h);
}

void test_zoom_content_scale() {
    // brief3d.md 9.1: the game's zoom draws the remaining icons ~15x higher in the grid's model space. The table scales uniformly (from the next
    // opened frame) so that the content top stays content_max_height() = 0.35 m above the table top; un-zoom restores scale 1.
    Producer p; fresh(p); Scene sc = make_scene(0.3); int L = 0, Q = 0;   // box -60..60: 1/120 m per unit at b3w 1
    auto frame = [&](int64_t t, float icon_y, int icons, Plan* sel_out) {
        set_icon_height(sc, icon_y);
        const Plan s = step(p, Kind::selmap, &L, t, sc, 1000000);
        CHECK(s.record);
        if (sel_out) *sel_out = s;
        for (int i = 0; i < icons; ++i) CHECK(step(p, Kind::modelicon, &L, t, sc, 1000000).record);
        execute(p, &Q, &L); p.on_present(t + 1); p.on_reset(&L, true);
        Held h; while (p.acquire(0, done(1000000), h)) p.release(h);   // keep slots free
    };
    char line[kLineLimit];
    Plan a, b, c, d, e;
    frame(100, 2.0f, 63, &a);                                // overview
    frame(120, 2.0f, 63, &b);                                // 2.2 units latched = 0.018 m: unchanged
    CHECK(p.content_scale_now() == 1.0 && !p.take_content_line(line, sizeof line));
    CHECK(std::memcmp(a.eye_rec[0], b.eye_rec[0], 64) == 0);
    frame(140, 48.0f, 2, nullptr);                           // the zoom: 2 icons at 48 units (0.40 m) drawn after this frame's grid
    frame(160, 48.0f, 2, &c);                                // opened with latched 52.8 units = 0.44 m > 0.35: scale 0.795
    const double k = p.content_scale_now();
    CHECK(std::fabs(k - content_max_height(on_opts()) / (48.0 * 1.1 / 120.0)) < 1e-9 && std::fabs(k - 0.7955) < 1e-4);
    CHECK(p.take_content_line(line, sizeof line) && std::string(line).find("brief3d-content;top-units=52.8;scale=0.7955;mode=vertical;height-m=0.35;width-m=1;max-m=0.35;changes=1") == 0);
    CHECK(std::strlen(line) < kLineLimit - kLinePrefix && !p.take_content_line(line, sizeof line));
    CHECK(std::memcmp(a.eye_rec[0], c.eye_rec[0], 64) != 0);   // the scaled table (the geometry is checked in brief3d_offline)
    frame(180, 50.0f, 2, nullptr);                           // within the hysteresis (50 < 52.8): no change
    frame(200, 50.0f, 2, nullptr);
    CHECK(p.content_scale_now() == k && !p.take_content_line(line, sizeof line));
    // brief3d.md 10: the un-zoom (2 < 26.4) must PERSIST kContentHoldMs before the latch shrinks (icon omissions are not an un-zoom)
    frame(220, 2.0f, 63, nullptr); frame(240, 2.0f, 63, nullptr); frame(1700, 2.0f, 63, nullptr); frame(3200, 2.0f, 63, nullptr);
    CHECK(p.content_scale_now() == k && !p.take_content_line(line, sizeof line));
    frame(4700, 2.0f, 63, nullptr); frame(6200, 2.0f, 63, nullptr);   // held 4 s: the latch shrinks to 2.2, but the scale must NOT relax
    // headset test 8 ("the icons floated a little higher after a few seconds"): within one page the vertical scale is monotone (tightens only)
    CHECK(p.content_scale_now() == k && !p.take_content_line(line, sizeof line));
    frame(9000, 2.0f, 63, &d);   // > 2 s without map draws (a new page): the scale starts from 1 again, the overview is byte-identical
    CHECK(p.content_scale_now() == 1.0);
    CHECK(std::memcmp(a.eye_rec[0], d.eye_rec[0], 64) == 0);   // the same poses: byte-identical to the overview frame again
    // an unusable icon position (non-finite) is ignored; a new screen resets the latch
    { float nan = NAN; std::memcpy(sc.icon + 64 * kIconModelMatrix + 4 * 13, &nan, 4); }
    { const Plan s = step(p, Kind::selmap, &L, 9060, sc, 1000000); CHECK(s.record); CHECK(step(p, Kind::modelicon, &L, 9060, sc, 1000000).record);
      execute(p, &Q, &L); p.on_present(9061); p.on_reset(&L, true); Held h; while (p.acquire(0, done(1000000), h)) p.release(h); }
    frame(9080, 2.0f, 1, &e);
    CHECK(p.content_scale_now() == 1.0);
    frame(9100, 48.0f, 2, nullptr); frame(9120, 48.0f, 2, nullptr);
    CHECK(p.content_scale_now() < 1.0);
    frame(14000, 2.0f, 63, nullptr);                         // > 2 s without map draws: a new screen, latch reset, scale 1 at once
    CHECK(p.content_scale_now() == 1.0);
    CHECK(p.stats().refused == 0 && !p.poisoned());
}


void test_zoom_air_icons_stalks() {
    // brief3d.md 9.2 (headset 2026-10-29 item 4): in the zoom the 2 aircraft are AIR icons: their plan carries the icon + kStalkDots dots (one reservation),
    // the dots stand on the table top under the aircraft, the aircraft never above the cap, the table keeps its width (vertical-only scale).
    Producer p; fresh(p); Scene sc = make_scene(0.3); int L = 0, Q = 0;   // up = model y, 1/120 m per unit
    auto frame = [&](int64_t t, float icon_y, int icons, std::vector<Plan>* out) {
        set_icon_height(sc, icon_y);
        const Plan s = step(p, Kind::selmap, &L, t, sc, 1000000); CHECK(s.record);
        if (out) out->push_back(s);
        for (int i = 0; i < icons; ++i) { const Plan ic = step(p, Kind::modelicon, &L, t, sc, 1000000); CHECK(ic.record); if (out) out->push_back(ic); }
        execute(p, &Q, &L); p.on_present(t + 1); p.on_reset(&L, true);
        Held h; while (p.acquire(0, done(1000000), h)) p.release(h);
    };
    std::vector<Plan> ov, zm;
    frame(100, 2.0f, 63, &ov);   // overview: no air icon (2 units = 0.017 m), one copy each, no counters
    for (size_t i = 1; i < ov.size(); ++i) CHECK(ov[i].copies == 1 && ov[i].index == unsigned(i - 1));
    CHECK(p.stats().air_icons == 0 && p.stats().stalk_dots == 0);
    frame(120, 48.0f, 2, nullptr); frame(140, 48.0f, 2, &zm);
    const double kv = p.content_scale_now(); CHECK(kv < 1.0);
    CHECK(zm.size() == 3 && zm[1].copies == 1 + kStalkDots && zm[1].index == 0 && zm[2].index == 1 + kStalkDots);
    CHECK(zm[1].swap.copies == 1 + kStalkDots && zm[1].swap.step == 2ull * kCopyStrideIcon);
    // the copies' upload offsets stay inside the slot span; every copy keeps the eye matrix 0 and the game's matrices 1, 2 and the vector
    for (unsigned c = 0; c < zm[2].copies; ++c) {
        const uint64_t off = zm[2].upload_offset[1] + uint64_t(c) * zm[2].swap.step;
        CHECK(off + kCopyStrideIcon <= uint64_t(zm[2].slot + 1) * kSlotUploadBytes && off >= uint64_t(zm[2].slot) * kSlotUploadBytes);
        CHECK(std::memcmp(zm[2].eye_rec[0][c], zm[0].eye_rec[0][0], 64) == 0 && std::memcmp(zm[2].eye_rec[0][c] + 64, sc.icon + 64, 128) == 0 &&
              std::memcmp(zm[2].eye_rec[0][c] + 256, sc.icon + 256, 16) == 0);
    }
    // dot heights: strictly increasing, all below the aircraft; translation y of the aircraft = 48 units (0.4 m unsquashed; table 0.35 cap)
    float yi = 0, yp = -1;
    std::memcpy(&yi, zm[1].eye_rec[0][0] + 64 * kIconModelMatrix + 4 * 13, 4);
    for (unsigned c = 1; c < zm[1].copies; ++c) { float y; std::memcpy(&y, zm[1].eye_rec[0][c] + 64 * kIconModelMatrix + 4 * 13, 4); CHECK(y > yp && y < yi && y > 0); yp = y; }
    CHECK(yi > 0 && std::fabs(double(yi) * (1.0 / 120.0) * kv - 48.0 / 120.0 * kv) < 1e-6);   // below the cap: the aircraft keeps its height
    CHECK(p.stats().air_icons >= 4 && p.stats().stalk_dots >= 4ull * kStalkDots && p.stats().refused == 0);
    char sum[800]; CHECK(p.format_summary(sum, sizeof sum) > 0 && std::string(sum).find(";air=") != std::string::npos &&
                         std::strlen(sum) < kLineLimit - kLinePrefix);
    // command sequence of an air plan: per eye OM/VP/SC once, then copies x (root SRV at the copy VA, draw)
    Cmd cm[kMaxCmds]; const unsigned n = replay_commands(zm[1].swap, false, true, cm);
    unsigned draws = 0; for (unsigned i = 0; i < n; ++i) if (cm[i].kind == CmdKind::draw) ++draws;
    CHECK(n == 2 * (3 + 2 * zm[1].copies) + 4 && n <= kMaxCmds && draws == 2 * zm[1].copies);
    CHECK(cm[3].kind == CmdKind::root_srv && cm[3].va == zm[1].swap.eye_va[0] && cm[5].va == zm[1].swap.eye_va[0] + zm[1].swap.step);
}

void test_icons_rigid_over_time() {
    // Headset test 8 (2026-11-01): "the icons floated a little higher after a few seconds" (ee-camera.log brief3d-content: scale 0.6155 -> 0.80 ->
    // 0.68 -> 0.6155 -> 1.0 while the measured content top went 36 -> 15 -> 33 -> 36 units). The icons are part of the table: a fixed aircraft
    // (model y = 10 units, 0.083 m on the table at scale 1) must never rise above where it first stood, however the tall aircraft elsewhere
    // appears and disappears (5 s present, 5 s absent: far longer than the 4 s hold, so the latch itself does shrink), for 60 s.
    Producer p; fresh(p); Scene sc = make_scene(0.3); int L = 0, Q = 0;
    auto height_m = [&](const Plan& ref) {   // the reference icon's height above the table top: its model y (copy 0) x metres per unit x the scale
        float y = 0; std::memcpy(&y, ref.eye_rec[0][0] + 64 * kIconModelMatrix + 4 * 13, 4);
        return double(y) / 120.0 * p.content_scale_now();
    };
    double first = -1, lowest = 1e9, highest_after_settle = 0, settled_k = 0;
    unsigned risen = 0, frames = 0;
    for (int64_t t = 100; t < 60100; t += 16) {
        const bool tall = ((t - 100) / 1000) % 10 < 5;
        set_icon_height(sc, 10.0f);
        const Plan s = step(p, Kind::selmap, &L, t, sc, 1000000); CHECK(s.record);
        Plan ref = step(p, Kind::modelicon, &L, t, sc, 1000000); CHECK(ref.record);
        if (tall) { set_icon_height(sc, 48.0f); CHECK(step(p, Kind::modelicon, &L, t, sc, 1000000).record); }
        execute(p, &Q, &L); p.on_present(t + 1); p.on_reset(&L, true);
        Held h; while (p.acquire(0, done(1000000), h)) p.release(h);
        const double hm = height_m(ref);
        if (first < 0) first = hm;
        if (hm > lowest + 1e-9) ++risen;   // the icon rose above a height it had already had
        lowest = std::fmin(lowest, hm);
        if (t >= 10100) { highest_after_settle = std::fmax(highest_after_settle, hm); if (settled_k == 0) settled_k = p.content_scale_now(); }
        ++frames;
    }
    CHECK(frames > 3000 && risen == 0);
    CHECK(settled_k > 0 && p.content_scale_now() == settled_k);   // the scale is constant after the first 10 s
    CHECK(highest_after_settle - lowest < 1e-9);                   // and so is the icon's height over the following 50 s
    CHECK(p.stats().placements == 1 && p.stats().fits == 1 && !p.poisoned());
}

void test_table_stable_under_jitter() {
    // brief3d.md 10 (headset test 5, "the table shakes a lot"): the measured content top flickers because the game omits icons from most map
    // frames (summary-brief3d: 35 map frames, 12 icons). The table (anchor, fit, content scale, and so the SelMap eye matrix) must not move.
    Producer p; fresh(p); Scene sc = make_scene(0.3); int L = 0, Q = 0;
    auto frame = [&](int64_t t, float icon_y, int icons, Plan* sel) {
        set_icon_height(sc, icon_y);
        const Plan s = step(p, Kind::selmap, &L, t, sc, 1000000); CHECK(s.record);
        if (sel) *sel = s;
        for (int i = 0; i < icons; ++i) CHECK(step(p, Kind::modelicon, &L, t, sc, 1000000).record);
        execute(p, &Q, &L); p.on_present(t + 1); p.on_reset(&L, true);
        Held h; while (p.acquire(0, done(1000000), h)) p.release(h);
    };
    frame(100, 48.0f, 2, nullptr); frame(116, 48.0f, 2, nullptr); frame(132, 48.0f, 2, nullptr);   // zoomed content: scale < 1
    const double k0 = p.content_scale_now(); CHECK(k0 < 1.0);
    Plan ref; frame(148, 48.0f, 2, &ref);
    unsigned long long changes = 0; double kprev = k0;
    for (int i = 0; i < 400; ++i) {   // 6.4 s at 16 ms: icons present on every 3rd map frame only (the others read top = 0)
        Plan s; frame(164 + 16 * i, 48.0f, i % 3 == 0 ? 2 : 0, &s);
        const double k = p.content_scale_now(); if (k != kprev) ++changes; kprev = k;
        CHECK(std::memcmp(s.eye_rec[0], ref.eye_rec[0], 64) == 0 && std::memcmp(s.eye_rec[1], ref.eye_rec[1], 64) == 0);   // same table every frame
    }
    CHECK(changes == 0 && p.content_scale_now() == k0);
    CHECK(p.stats().placements == 1 && p.stats().fits == 1 && !p.poisoned());
    // a page flip (gap 3 s, below the 30 s re-latch gap) keeps fit and placement: the table does not jump to the head again
    Plan back; frame(164 + 16 * 400 + 3000, 48.0f, 2, &back);
    CHECK(p.stats().placements == 1 && p.stats().fits == 1);
    // the latch itself: a lower reading must persist kContentHoldMs; a recovery in between resets the hold; relaxation is slew-limited
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    ContentLatch lt; CHECK(near(latch_content_top(lt, 40, 1000), 44.0));
    CHECK(near(latch_content_top(lt, 0, 1100), 44.0) && near(latch_content_top(lt, 40, 1200), 44.0) && lt.low_since_ms == 0);
    CHECK(near(latch_content_top(lt, 0, 2000), 44.0) && near(latch_content_top(lt, 0, 5999), 44.0) && latch_content_top(lt, 0, 6000) == 0.0);
    // headset test 8: no relaxation at all (tightening is immediate, a larger target never raises the scale)
    CHECK(near(relax_content_scale(0.5, 1.0, 1000), 0.5) && relax_content_scale(0.5, 1.0, 10000) == 0.5 && relax_content_scale(0.7, 0.4, 16) == 0.4);
}

void test_refused_open_rolls_observation() {
    // W8-1 P1: content so tall that 0.35 m needs a scale below the 0.1 minimum is REFUSED with a name (no slot), never opened over the cap.
    // W8-1 P2: the refused open must not preserve the obsolete high icon after the game zooms out: the maximum rolls per completed game frame.
    Producer p; fresh(p); Scene sc = make_scene(0.3); int L = 0, Q = 0;
    auto frame = [&](int64_t t, float icon_y, int icons, bool expect_open) {
        set_icon_height(sc, icon_y);
        const uint64_t frames0 = p.stats().frames;
        const Plan s = step(p, Kind::selmap, &L, t, sc, 1000000);
        CHECK(s.record == expect_open);
        if (!expect_open) CHECK(streq(s.why, "brief3d-content-too-tall"));
        for (int i = 0; i < icons; ++i) (void)step(p, Kind::modelicon, &L, t, sc, 1000000);
        CHECK(p.stats().frames == frames0 + (expect_open ? 1u : 0u));   // a refusal allocates no slot
        if (expect_open) execute(p, &Q, &L);
        p.on_present(t + 1); p.on_reset(&L, true);
        Held h; while (p.acquire(0, done(1000000), h)) p.release(h);
    };
    frame(100, 2.0f, 63, true);      // overview
    frame(120, 400.0f, 2, true);     // opened with the previous (low) interval; the tall icons (3.3 m) are seen during it
    frame(140, 400.0f, 2, false);    // 440 units = 3.67 m -> k = 0.095 < 0.1: refused, named
    CHECK(p.content_scale_now() == 1.0);   // refusals commit nothing (the latched scale stays that of the last successful open)
    frame(160, 2.0f, 63, false);     // still sees the completed tall interval of frame 140
    frame(180, 2.0f, 63, true);      // the zoom-out interval is complete: opens at once, no head movement or screen change
    CHECK(p.content_scale_now() == 1.0);
    CHECK(!p.poisoned());
}

// ================================================================================================== UI3D (ui-3d.md 4 items 2-3): the second frame kind
int g_ui_rt = 0, g_other_rt = 0;
// a census main-menu draw (step124/runD1 ee-camera.log event=ui3d-census, kind=draw): ps2_v, RT 2560x1440 fmt 10, PSO rtv0 10 / dsv 20, depth on
// func 8, stencil on, vp 0,0,2560,1440, scissor 320,0,2240,1440, cb0 rc0 682,-448,2048,2048, mapped UPLOAD VB stride 64, 4 vertices, q 1
UiFacts ui_facts(const void* list, int64_t now, float x0, float y0, float x1, float y1, float z, const void* rt = &g_ui_rt) {
    UiFacts f;
    f.list = list; f.now_ms = now; f.vs8 = acz::ui3d::kPs2Vs8; f.vertices = 4; f.instances = 1; f.topology_type = 3;
    f.rt = rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = kCmpAlways; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = acz::ui3d::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f}; f.vb_why = nullptr; f.stride = 64;
    const float xy[4][2] = {{x0, y0}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        float v[16] = {xy[i][0], xy[i][1], z, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, float(i & 1), float(i >> 1), 0, 0, 0.2f, 0.3f, 0.4f, 0};
        std::memcpy(f.vtx + 64 * i, v, 64);
    }
    return f;
}
UiFacts ui_other(const void* list, int64_t now, uint32_t vs8) { UiFacts f; f.list = list; f.now_ms = now; f.vs8 = vs8; return f; }
PoseIn ui_pose(uint32_t gen = 1, double yaw = 0.0) {
    PoseIn p; p.ok = true; p.space_generation = gen;
    p.eyes[0] = make_eye(0.0, 1.6, 0.0, yaw, 0.0, -0.032); p.eyes[1] = make_eye(0.0, 1.6, 0.0, yaw, 0.0, 0.032);
    return p;
}
// the vscreen consumer's placement (ScreenCore publishes it at every placement): 2.5 m ahead of the head at yaw 0
void ui_screen(uint32_t encoded_gen = 1) {
    acz::vscreen::Options o; const float head[3] = {0.0f, 1.6f, 0.0f}, q[4] = {0, 0, 0, 1};
    acz::vscreen::publish_placement(acz::vscreen::place(o, head, q, 16.0f / 9.0f, encoded_gen - 1), head, encoded_gen);
}
Producer& ui_fresh(Producer& p) { acz::ui3d::Options o; o.on = true; p.configure_ui3d(o); p.gpu_ready(kUploadVa); ui_screen(); return p; }
UiPlan ui_step(Producer& p, const UiFacts& f, uint64_t completed = 0, const PoseIn* ps = nullptr) {
    UiPlan pl = p.ui3d_draw(f, ps ? *ps : (p.wants_poses(f.now_ms) ? ui_pose() : PoseIn{}), done(completed));
    if (pl.record) p.ui3d_commit(pl);
    return pl;
}
float vx(const UiPlan& pl, unsigned eye, unsigned v, unsigned k) { float f; std::memcpy(&f, pl.vtx[eye] + 64 * v + 4 * k, 4); return f; }

void test_ui3d_happy_frame() {
    Producer p; CHECK(!p.configured());
    ui_fresh(p);
    CHECK(p.configured() && !p.table_configured() && p.ui3d_on() && !p.ui3d_held(0));
    int L = 0, Q = 0;
    const UiPlan a = ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64));   // backdrop
    CHECK(a.record && a.clear && a.layer == 0 && a.slot >= 0 && a.vertices == 4 && a.bind_dsv && a.eye_draw[0] && a.eye_draw[1]);
    CHECK(a.upload_offset[0] == kUiUploadBase + uint64_t(a.slot) * acz::ui3d::kUploadPerSlot && a.upload_offset[1] == a.upload_offset[0] + 256);
    CHECK(a.upload_offset[1] + 256 <= kUploadTotal && kUploadTotal == 3ull * 196608 + 3ull * 524288 + 3ull * 2097152);   // + the DEBRIEF-3D spans
    // the scissor (320..2240) carried onto the layer: inside the eye image, narrower than it
    CHECK(a.scissor[0][0] > 0 && a.scissor[0][2] < int32_t(kEyeW) && a.scissor[0][2] > a.scissor[0][0] && a.scissor[0][3] > a.scissor[0][1]);
    const UiPlan b = ui_step(p, ui_facts(&L, 101, 1809, 1842, 1841, 1874, 64));   // a panel tile
    CHECK(b.record && !b.clear && b.layer == 1 && b.slot == a.slot && b.upload_offset[0] == a.upload_offset[0] + 512);
    const UiPlan c = ui_step(p, ui_facts(&L, 102, 1824, 1902, 1835, 1926, 1));    // a glyph (z 1)
    CHECK(c.record && c.layer == 2);
    // z, colours and texcoords unchanged; the text layer has a larger stereo disparity than the backdrop
    for (unsigned e = 0; e < 2; ++e) CHECK(vx(c, e, 0, 2) == 1.0f && vx(c, e, 0, 12) == 0.2f && std::memcmp(c.vtx[0] + 16, c.vtx[1] + 16, 48) == 0);
    CHECK(std::fabs(vx(c, 0, 0, 0) - vx(c, 1, 0, 0)) > std::fabs(vx(a, 0, 0, 0) - vx(a, 1, 0, 0)));
    // the final pass to another target: skipped, the frame goes on
    const UiPlan d = ui_step(p, ui_facts(&L, 103, 128, 256, 2688, 1696, 0, &g_other_rt));
    CHECK(!d.record && streq(d.why, "ui3d-off-target"));
    p.on_present(110);
    CHECK(p.tracked_lists() == 1 && !can_acquire(p, 99));
    execute(p, &Q, &L);
    p.on_reset(&L, true);
    Held h; CHECK(p.acquire(0, done(1), h) && h.slot == a.slot && h.meta.opaque && h.meta.replays == 3 && h.meta.space_generation == 1);
    p.release(h);
    const acz::ui3d::Stats st = p.ui3d_stats();
    CHECK(st.frames == 1 && st.usable == 1 && st.draws == 3 && st.layer[0] == 1 && st.layer[1] == 1 && st.layer[2] == 1 && st.refused == 0);
    const std::string line = p.ui3d_summary();
    CHECK(line.find("summary-ui3d;frames=1;usable=1;refused=0;draws=3;") == 0 && line.find(";published=1;") != std::string::npos && line.size() < kLineLimit);
}

void test_ui3d_reject_after_replays() {
    Producer p; ui_fresh(p);
    int L = 0, Q = 0;
    const UiPlan a = ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64));
    CHECK(a.record && ui_step(p, ui_facts(&L, 101, 1809, 1842, 1841, 1874, 64)).record);
    // the movie draw (cinematics) arrives after two replays: the frame is rejected and UI3D is held off
    CHECK(streq(ui_step(p, ui_other(&L, 102, acz::ui3d::kMovieVs8)).why, "ui3d-frame-has-movie"));
    CHECK(p.ui3d_held(103) && !p.ui3d_held(102 + kUiHoldMs - kUiRenewMs + 1));
    CHECK(streq(ui_step(p, ui_facts(&L, 104, 1792, 1824, 2304, 2272, 64)).why, "ui3d-hold"));
    p.on_present(110);
    const auto t = execute(p, &Q, &L);   // the rejected recording WAS executed (fence value t.value)
    p.on_reset(&L, true);
    CHECK(!can_acquire(p, 1000));        // never published
    CHECK(p.ring_slot(unsigned(a.slot)).value == t.value && p.ring_slot(unsigned(a.slot)).state == acz::vscreen::slot_free);
    acz::ui3d::Stats st = p.ui3d_stats();
    CHECK(st.frames == 1 && st.refused == 1 && streq(st.last_why, "ui3d-frame-has-movie"));
    // after the hold: a new frame; while the rejected value has not completed its slot is not reused
    const UiPlan n = ui_step(p, ui_facts(&L, 1200, 1792, 1824, 2304, 2272, 64), t.value - 1);
    CHECK(n.record && n.slot != a.slot);
    p.on_present(1210); execute(p, &Q, &L); p.on_reset(&L, true);
    CHECK(can_acquire(p, t.value + 1));
    // a rejected frame that never executed is simply aborted
    Producer q; ui_fresh(q); int M = 0;
    CHECK(ui_step(q, ui_facts(&M, 100, 1792, 1824, 2304, 2272, 64)).record);
    CHECK(streq(ui_step(q, ui_other(&M, 101, acz::ui3d::kSelMapVs8)).why, "ui3d-frame-has-3d"));
    q.on_reset(&M, true);
    CHECK(!can_acquire(q, 100) && q.ring_slot(0).state == acz::vscreen::slot_free && q.ring_slot(0).value == 0);
    CHECK(q.ui3d_summary().find(";rejected=1;") != std::string::npos);
}

void test_ui3d_refusals_reject_the_frame() {
    struct Case { const char* why; void (*mut)(UiFacts&); };
    const Case cases[] = {
        {"ui3d-indexed-draw", [](UiFacts& f) { f.indexed = true; }},
        {"ui3d-gs-draw", [](UiFacts& f) { f.gs = true; }},
        {"ui3d-instances", [](UiFacts& f) { f.instances = 2; }},
        {"ui3d-draw-too-many-vertices", [](UiFacts& f) { f.vertices = 65; }},
        {"ui3d-pso-unknown", [](UiFacts& f) { f.pso_known = false; }},
        {"ui3d-pso-rtv", [](UiFacts& f) { f.rtv0 = 24; }},
        {"ui3d-pso-dsv", [](UiFacts& f) { f.dsv = 45; }},
        {"ui3d-depth-func", [](UiFacts& f) { f.depth_func = kCmpGreaterEqual; }},
        {"ui3d-stencil-not-always", [](UiFacts& f) { f.stencil_always = false; }},
        {"ui3d-viewport-unknown", [](UiFacts& f) { f.rs_known = false; }},
        {"ui3d-viewport-count", [](UiFacts& f) { f.scissors = 2; }},
        {"ui3d-om-unknown", [](UiFacts& f) { f.om_known = false; }},
        {"ui3d-cb0-in-descriptor-table", [](UiFacts& f) { f.cb_why = "ui3d-cb0-in-descriptor-table"; }},
        {"ui3d-cb0", [](UiFacts& f) { f.cb0.sx = 0; }},
        {"ui3d-vb-torn", [](UiFacts& f) { f.vb_why = "ui3d-vb-torn"; }},
        {"ui3d-vb-stride", [](UiFacts& f) { f.stride = 48; }},
        {"ui3d-topology", [](UiFacts& f) { f.topology_type = 4; }},
    };
    for (const Case& k : cases) {
        Producer p; ui_fresh(p); int L = 0, Q = 0;
        CHECK(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).record);
        UiFacts f = ui_facts(&L, 101, 1809, 1842, 1841, 1874, 64); k.mut(f);
        const UiPlan r = ui_step(p, f);
        CHECK(!r.record && streq(r.why, k.why));
        // the frame is rejected: later draws of the same list cannot reopen before its Reset, nothing is published
        CHECK(streq(ui_step(p, ui_facts(&L, 102, 1792, 1824, 2304, 2272, 64)).why, "ui3d-list-holds-previous-frame"));
        p.on_present(110); execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(!can_acquire(p, 100) && p.ui3d_stats().refused == 1);
        // the same fact on the opening draw: nothing opens at all
        Producer o; ui_fresh(o); int M = 0;
        UiFacts g = ui_facts(&M, 100, 1792, 1824, 2304, 2272, 64); k.mut(g);
        CHECK(!ui_step(o, g).record && o.tracked_lists() == 0 && o.ui3d_stats().frames == 0);
    }
    {   // another list while the frame is open
        Producer p; ui_fresh(p); int L = 0, M = 0;
        CHECK(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).record);
        CHECK(streq(ui_step(p, ui_facts(&M, 101, 1792, 1824, 2304, 2272, 64)).why, "ui3d-frame-other-list"));
        CHECK(p.ui3d_stats().refused == 1);
    }
    {   // no screen placement / another generation / no pose / off / gpu not ready: nothing opens
        Producer p; acz::ui3d::Options o; o.on = true; p.configure_ui3d(o); int L = 0;
        CHECK(streq(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).why, "ui3d-gpu-not-ready"));
        p.gpu_ready(kUploadVa);
        acz::vscreen::clear_placement();
        CHECK(streq(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).why, "ui3d-no-screen-placement"));
        ui_screen(2);
        CHECK(streq(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).why, "ui3d-no-screen-placement"));
        ui_screen(1);
        const PoseIn none;
        CHECK(streq(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64), 0, &none).why, "ui3d-no-pose"));
        CHECK(p.tracked_lists() == 0);
        Producer off; int M = 0; off.gpu_ready(kUploadVa);
        CHECK(streq(ui_step(off, ui_facts(&M, 100, 1792, 1824, 2304, 2272, 64)).why, "ui3d-off") && !off.ui3d_on());
    }
}

void test_ui3d_and_the_table() {
    Producer p; fresh(p); ui_fresh(p);
    CHECK(p.table_configured() && p.ui3d_on());
    const Scene sc = make_scene(0.3); int L = 0, L2 = 0, Q = 0;
    const UiPlan a = ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64));
    CHECK(a.record);
    // the briefing overview: the SelMap draw rejects the UI3D frame (the glue's UI3D seam would have done so first) and holds UI3D off
    const Plan t = step(p, Kind::selmap, &L, 101, sc);
    CHECK(!t.record && streq(t.why, "brief3d-list-holds-previous-frame") && p.ui3d_held(102));
    CHECK(p.ui3d_stats().refused == 1 && streq(p.ui3d_stats().last_why, "ui3d-frame-has-3d"));
    p.on_present(110); execute(p, &Q, &L); p.on_reset(&L, true);
    // the next frame belongs to the table; ps2 draws during it are not replayed by UI3D
    const Plan t2 = step(p, Kind::selmap, &L2, 120, sc);
    CHECK(t2.record);
    const UiPlan u = ui_step(p, ui_facts(&L2, 121, 1792, 1824, 2304, 2272, 64));
    CHECK(!u.record && streq(u.why, "ui3d-table-frame-open"));
    p.on_present(130); execute(p, &Q, &L2); p.on_reset(&L2, true);
    Held h; CHECK(p.acquire(0, done(10), h) && !h.meta.opaque); p.release(h);
    // the held seam: a non-ps2 draw renews; the producer-side hold also refuses opening
    CHECK(streq(ui_step(p, ui_facts(&L, 500, 1792, 1824, 2304, 2272, 64)).why, "ui3d-hold"));
    CHECK(ui_step(p, ui_facts(&L, 1200, 1792, 1824, 2304, 2272, 64)).record);
}

void test_ui3d_budget_and_culling() {
    {   // 64-vertex draws: 8 KiB per draw for both eyes; the 65th of a frame exceeds the 512 KiB span and rejects the frame
        Producer p; ui_fresh(p); int L = 0;
        UiFacts f = ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64);
        f.vertices = 64;
        for (unsigned i = 4; i < 64; ++i) std::memcpy(f.vtx + 64 * i, f.vtx + 64 * (i % 4), 64);
        for (unsigned i = 0; i < 64; ++i) { f.now_ms = 100 + i; CHECK(ui_step(p, f).record); }
        f.now_ms = 200;
        CHECK(streq(ui_step(p, f).why, "ui3d-upload-full"));
    }
    {   // looking back: both eyes culled; the first plan still carries the clear, later ones record nothing
        Producer p; ui_fresh(p); int L = 0;
        const PoseIn back = ui_pose(1, 3.14159265);
        const UiPlan a = ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64), 0, &back);
        CHECK(a.record && a.clear && !a.eye_draw[0] && !a.eye_draw[1]);
        const UiPlan b = ui_step(p, ui_facts(&L, 101, 1809, 1842, 1841, 1874, 64));
        CHECK(!b.record && streq(b.why, "ui3d-culled"));
        p.on_present(110);
        CHECK(p.ui3d_stats().culled == 4 && p.ui3d_stats().usable == 1);
    }
    {   // a cancelled clear-bearing plan invalidates the frame: never published
        Producer p; ui_fresh(p); int L = 0, Q = 0;
        const UiPlan a = p.ui3d_draw(ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64), ui_pose(), done(0));
        CHECK(a.record); p.ui3d_cancel(a);
        CHECK(ui_step(p, ui_facts(&L, 101, 1809, 1842, 1841, 1874, 64)).record);
        p.on_present(110); execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(!can_acquire(p, 100) && p.stats().quarantined == 1);
    }
}

} // namespace

// W12-3 P1-1: a UI3D frame whose list is Reset BEFORE the Present is not published until the Present validates the interval.
void test_ui3d_reset_before_present() {
    {   // reset before present, then a non-ps2 draw in the same game frame: rejected, never published
        Producer p; ui_fresh(p); int L = 0, Q = 0;
        const UiPlan a = ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64));
        CHECK(a.record);
        const auto t = execute(p, &Q, &L);
        p.on_reset(&L, true);
        CHECK(!can_acquire(p, 1000));                                               // not published at the Reset
        CHECK(streq(ui_step(p, ui_other(&L, 101, acz::ui3d::kMovieVs8)).why, "ui3d-frame-has-movie"));
        p.on_present(110);
        CHECK(!can_acquire(p, 1000) && p.ui3d_summary().find(";published=0;") != std::string::npos && p.ui3d_summary().find(";rejected=1;") != std::string::npos);
        CHECK(p.ring_slot(unsigned(a.slot)).value == t.value);                       // the executed value is retained for reuse safety
    }
    {   // reset before present, a ps2 draw from a second list is refused and rejects the frame
        Producer p; ui_fresh(p); int L = 0, M = 0, Q = 0;
        CHECK(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).record);
        execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(streq(ui_step(p, ui_facts(&M, 101, 1792, 1824, 2304, 2272, 64)).why, "ui3d-draw-after-reset"));
        p.on_present(110);
        CHECK(!can_acquire(p, 1000));
    }
    {   // the valid case: reset before present, nothing else in the frame: published when the Present validates it
        Producer p; ui_fresh(p); int L = 0, Q = 0;
        const UiPlan a = ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64));
        execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(!can_acquire(p, 1000));
        p.on_present(110);
        Held h; CHECK(can_acquire(p, 1, &h) && h.slot == a.slot && h.meta.opaque);
    }
    {   // the usual order (present, then reset) is unchanged
        Producer p; ui_fresh(p); int L = 0, Q = 0;
        CHECK(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).record);
        p.on_present(110); execute(p, &Q, &L); p.on_reset(&L, true);
        CHECK(can_acquire(p, 1));
    }
}

// W12-3 P1-2: a draw whose list/PSO metadata is unknown rejects the open frame and holds UI3D off (the game's own draw is untouched by the seam).
void test_ui3d_unknown_draw_fails_closed() {
    Producer p; ui_fresh(p); int L = 0, Q = 0;
    CHECK(ui_step(p, ui_facts(&L, 100, 1792, 1824, 2304, 2272, 64)).record);
    p.ui3d_unknown_draw(101, "ui3d-pso-unknown-lookup");
    CHECK(p.ui3d_held(102) && streq(ui_step(p, ui_facts(&L, 103, 1792, 1824, 2304, 2272, 64)).why, "ui3d-hold"));
    p.on_present(110); execute(p, &Q, &L); p.on_reset(&L, true);
    CHECK(!can_acquire(p, 1000));
    CHECK(streq(p.ui3d_stats().last_why, "ui3d-pso-unknown-lookup") && p.ui3d_summary().find(";rejected=1;") != std::string::npos);
    Producer q; ui_fresh(q);                                                          // without an open frame: only the hold
    q.ui3d_unknown_draw(100, "ui3d-list-unknown");
    CHECK(q.ui3d_held(101) && streq(ui_step(q, ui_facts(&L, 102, 1792, 1824, 2304, 2272, 64)).why, "ui3d-hold"));
}

int main() {
    test_ui3d_reset_before_present();
    test_ui3d_unknown_draw_fails_closed();
    test_refusals_before_ready();
    test_happy_frame_publishes_at_reset();
    test_xr_lines_worst_case();
    test_icons_with_other_pixel_shader();
    test_acquire_lag();
    test_recording_reservation();
    test_submission_reservation();
    test_reexecution_and_reuse();
    test_second_queue();
    test_clear_and_lists();
    test_signal_failure_and_discard();
    test_ring_backpressure();
    test_fit_and_screens();
    test_gate_with_live_facts();
    test_threads();
    test_zoom_content_scale();
    test_icons_rigid_over_time();
    test_table_stable_under_jitter();
    test_zoom_air_icons_stalks();
    test_refused_open_rolls_observation();
    test_ui3d_happy_frame();
    test_ui3d_reject_after_replays();
    test_ui3d_refusals_reject_the_frame();
    test_ui3d_and_the_table();
    test_ui3d_budget_and_culling();
    std::printf("brief3d_live_offline: %u checks passed\n", g_checks.load());
    return 0;
}
