// BRIEF3D consumer offline tests (src/acz-vr/xr_brief3d_core.h): the table blit + layer on top of the virtual screen, with a fake Gate, fake
// swapchain runtime, fake GPU, a fake inner vscreen hook and the REAL producer core (brief3d_live.h) behind a fake-fence source. Also the layer
// list (compose_layers) for screen + table. No device, no headset: fixture success says nothing about the game or SteamVR.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "xr_brief3d_core.h"
#include "xr_stereo_output.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace acz;
using namespace acz::xr_stereo;
namespace live = acz::brief3d::live;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {

struct FakeGate final : xr_owner::Gate {
    int64_t t = 0; bool lost_ = false;
    std::vector<xr_owner::Op> refuse;
    int64_t now_ms() override { return t; }
    void sleep_ms(int64_t ms) override { t += ms; }
    bool lost() const override { return lost_; }
    bool stopping() const override { return false; }
protected:
    xr_owner::Rc invoke(xr_owner::Op op, xr_owner::Rc (*fn)(void*), void* p) override {
        for (auto r : refuse) if (r == op) return xr_owner::Rc::refused;
        return fn(p);
    }
};
// Two eye targets; every acquisition must be waited and released (violations counted).
struct FakeChains final : SwapchainRuntime {
    std::vector<std::string> log;
    bool acquired[5] = {}, waited[5] = {}; uint32_t next[5] = {}; unsigned violations = 0;
    std::deque<bool> timeouts; int fail_acquire_target = -1; bool release_fails = false;
    xr_owner::Rc view_configs(std::array<ViewConfig, 2>&) override { return xr_owner::Rc::ok; }
    xr_owner::Rc formats(std::array<int64_t, max_formats>&, uint32_t& n) override { n = 0; return xr_owner::Rc::ok; }
    xr_owner::Rc create_swapchain(unsigned, int64_t, uint32_t, uint32_t, SwapchainImages&) override { return xr_owner::Rc::ok; }
    xr_owner::Rc acquire(unsigned t, uint32_t& i) override {
        if (int(t) == fail_acquire_target) { log.push_back("acquire " + std::to_string(t) + " failed"); return xr_owner::Rc::error; }
        if (acquired[t]) ++violations;
        acquired[t] = true; waited[t] = false; i = next[t]; next[t] = (next[t] + 1) % 3;
        log.push_back("acquire " + std::to_string(t) + " " + std::to_string(i)); return xr_owner::Rc::ok;
    }
    xr_owner::Rc wait(unsigned t, int64_t, bool& to) override {
        if (!acquired[t] || waited[t]) ++violations;
        to = false; if (!timeouts.empty()) { to = timeouts.front(); timeouts.pop_front(); }
        if (!to) waited[t] = true;
        log.push_back("wait " + std::to_string(t) + (to ? " timeout" : " ok")); return xr_owner::Rc::ok;
    }
    xr_owner::Rc release(unsigned t) override {
        if (!acquired[t] || !waited[t]) ++violations;
        log.push_back("release " + std::to_string(t));
        if (release_fails) return xr_owner::Rc::error;
        acquired[t] = false; waited[t] = false; return xr_owner::Rc::ok;
    }
    xr_owner::Rc destroy_swapchain(unsigned) override { return xr_owner::Rc::ok; }
};
struct FakeGpu final : TableGpu {
    FakeChains* chains = nullptr;
    uint64_t value = 0, completed_v = 0; bool lose = false, record_fails = false, signal_fails = false, setup_fails = false;
    unsigned blits = 0, unwaited_writes = 0, setups = 0, destroys = 0;
    std::vector<void*> dst;
    bool setup(const SwapchainImages&, const SwapchainImages&, const char*& why) override { ++setups; if (setup_fails) { why = "rtv-heap"; return false; } return true; }
    vscreen::Completion completed() override { vscreen::Completion c; if (lose) c.lost = true; else c.value = completed_v; return c; }
    bool last_opaque = false;   // UI3D: the slot flag the blit got
    Submit blit(unsigned, void* d0, void* d1, const TableImages& src, Extent, Extent) override {
        ++blits; Submit s; last_opaque = src.opaque;
        if (record_fails || !src.eye[0]) return s;
        if (!chains->waited[0] || !chains->waited[1]) ++unwaited_writes;
        dst.push_back(d0); dst.push_back(d1); s.executed = true;
        if (rb_want) { s.readback = !rb_pending; rb_dst = d0; if (s.readback) { ++rb_recorded; rb_pending = true; } }   // like Readback::record: refused while pending_
        if (signal_fails) return s;
        s.signalled = true; s.value = ++value; return s;
    }
    // SCREEN-DUMP: a 2x2 premultiplied marker image as stored: top-left (40,0,0,64), top-right (0,50,0,128), bottom-left (0,0,60,255), bottom-right 0
    bool rb_want = false; void* rb_dst = nullptr; unsigned rb_recorded = 0, rb_discarded = 0; bool rb_pending = false;
    void readback_discard() override { ++rb_discarded; rb_pending = false; }
    void readback_want(bool w) override { rb_want = w; }
    bool readback_take(std::vector<uint8_t>& v, uint32_t& w, uint32_t& h) override {
        rb_pending = false; w = 2; h = 2; v = {40, 0, 0, 64, 0, 50, 0, 128, 0, 0, 60, 255, 0, 0, 0, 0}; return true;
    }
    void destroy() override { ++destroys; }
};
// The real producer behind a fake fence.
struct ProducerSource final : TableSource {
    live::Producer* p = nullptr; uint64_t fence = 0; unsigned acquired = 0, released = 0, violation = 0; bool holding = false; uint64_t held_id = 0;
    bool acquire(uint64_t after, live::Held& out, TableImages& img) override {
        if (holding) { ++violation; return false; }
        vscreen::Completion c; c.value = fence;
        if (!p->acquire(after, c, out)) return false;
        img.eye[0] = reinterpret_cast<void*>(uintptr_t(0x5000 + out.slot * 2)); img.eye[1] = reinterpret_cast<void*>(uintptr_t(0x5001 + out.slot * 2));
        img.width = live::kEyeW; img.height = live::kEyeH; img.format = live::kRtvFormat; img.opaque = out.meta.opaque;   // as the DLL's source
        holding = true; held_id = out.a.id; ++acquired; return true;
    }
    void release(const live::Held& h) override { if (!holding || h.a.id != held_id) ++violation; holding = false; ++released; p->release(h); }
    bool dead() override { return p->poisoned(); }
    std::string describe() override {   // as the DLL's ProducerSource: table-line tail + '\n' + the producer line
        char t[brief3d::kLineLimit], l[brief3d::kLineLimit]; p->format_xr(t, sizeof t, l, sizeof l);
        return std::string(t) + "\n" + l;
    }
};
struct FakeInner final : VScreenHook {
    bool show = true, poison = false; std::vector<unsigned> written; unsigned retired = 0;
    bool frame(Gate&, const xr_owner::FrameInfo&, SwapchainRuntime&, unsigned, const SwapchainImages&, Extent, const VScreenIn&,
               xr_owner::LayerPlan& plan) override {
        if (!show) return false;
        plan.submit = true; plan.layer = xr_owner::ProjectionLayer{}; plan.layer.screen_only = true; plan.layer.screen_width = 3.2f; return true;
    }
    bool exclusive() const noexcept override { return false; }
    bool retire(Gate&) override { ++retired; return true; }
    std::string summary() override { return {}; }
    bool poisoned() const noexcept override { return poison; }
    void target_written(unsigned t) noexcept override { written.push_back(t); }
    bool dump_ok = true, dump_armed = false;   // SCREEN-DUMP: the screen part (taken at once)
    const char* dump_arm(int64_t) override { if (!dump_ok) return "no-screen-image"; dump_armed = true; return nullptr; }
    bool dump_take(Gate&, DumpImage& d) override {
        if (!dump_armed) return false;
        dump_armed = false; d = DumpImage{}; d.part = "screen"; d.ok = true; d.w = 1; d.h = 1; d.rgba = {1, 2, 3, 255}; return true;
    }
    bool dump_busy() const noexcept override { return dump_armed; }
};

// a producer with one published frame per call of publish()
struct Rig {
    live::Producer prod;
    FakeGate g; FakeChains xr;
    std::shared_ptr<FakeGpu> gpu = std::make_shared<FakeGpu>();
    std::shared_ptr<ProducerSource> src = std::make_shared<ProducerSource>();
    std::shared_ptr<TableCore> table;
    std::shared_ptr<FakeInner> inner = std::make_shared<FakeInner>();
    std::shared_ptr<Brief3dHook> hook;
    SwapchainImages eye[2], hud;
    xr_owner::FrameInfo info;
    int list = 0, queue = 0; int64_t pt = 0;
    Rig() {
        brief3d::Options o; o.on = true; prod.configure(o); prod.gpu_ready(0x40000000ull);
        gpu->chains = &xr; src->p = &prod;
        table = std::make_shared<TableCore>(gpu, src);
        hook = std::make_shared<Brief3dHook>(inner, table);
        for (unsigned e = 0; e < 2; ++e) { eye[e].count = 3; for (unsigned i = 0; i < 3; ++i) eye[e].images[i] = reinterpret_cast<void*>(uintptr_t(0x1000 * (e + 1) + i)); }
        hud.count = 3;
    }
    // one briefing frame produced, executed, presented (the source fence is advanced separately)
    void publish(uint32_t gen = 1) {
        pt = std::max(pt + 16, g.t);   // the producer's Present clock follows the owner clock (one GetTickCount64 in the DLL; W14-1 F1 ages slots on it)
        prod.on_present(pt - 8);       // the previous game frame's Present (a table slot is stamped at its Reset with the last Present time)
        brief3d::EyeView ev[2];
        live::DrawFacts f; f.kind = brief3d::Kind::selmap; f.list = &list; f.now_ms = pt; f.root_why = nullptr; f.t10_param = 0; f.t10_va = 0x10;
        f.t10_why = nullptr; f.b10_why = nullptr; f.b10[0] = 682; f.b10[1] = -448;
        f.pso_known = true; f.rt_count = 1; f.rtv0 = live::kRtvFormat; f.dsv = live::kDsvFormat; f.samples = 1; f.depth_enable = true; f.depth_write = true;
        f.depth_func = brief3d::kCmpGreaterEqual; f.stencil = true; f.stencil_always = true; f.rs_known = true; f.viewports = 1; f.scissors = 1; f.om_known = true;
        // a game camera above a flat map (model y up)
        brief3d::EyeView cam; const double q = std::sin(-0.39), c = std::cos(-0.39);
        cam.orientation[0] = q; cam.orientation[3] = c; cam.position[1] = 60; cam.position[2] = 60; cam.fov[0] = -0.6; cam.fov[1] = 0.6; cam.fov[2] = 0.35; cam.fov[3] = -0.35;
        brief3d::Mat4 P; brief3d::ps2_projection(cam.fov, f.b10, 0.5, brief3d::DepthMode::reverse, P);
        brief3d::store(brief3d::mul(brief3d::view_from_eye(cam), P), f.rec);
        brief3d::Box box; box.valid = true; box.vertices = 4; box.lo[0] = -60; box.hi[0] = 60; box.lo[2] = -60; box.hi[2] = 60; box.hi[1] = 5;
        live::PoseIn pi; pi.ok = true; pi.space_generation = gen;
        for (unsigned e = 0; e < 2; ++e) {
            const double h = std::sin(-0.3), k = std::cos(-0.3);
            pi.eyes[e].orientation[0] = h; pi.eyes[e].orientation[3] = k; pi.eyes[e].position[0] = e ? 0.032 : -0.032; pi.eyes[e].position[1] = 1.2;
        }
        vscreen::Completion done; done.value = src->fence;
        const live::Plan pl = prod.draw(f, pi, prod.wants_box(pt) ? &box : nullptr, nullptr, done);
        CHECK(pl.record);
        prod.commit(pl);
        const void* ls[1] = {&list};
        const auto tok = prod.before_execute(&queue, ls, 1);
        prod.after_execute(tok, true);
        prod.on_reset(&list, true);
        prod.on_present(pt);
    }
    // UI3D (ui-3d.md 4): one menu frame of the second kind (census main-menu draws), executed, reset, presented.
    void publish_ui(uint32_t gen = 1) {
        pt = std::max(pt + 16, g.t);   // the producer's Present clock follows the owner clock (one GetTickCount64 in the DLL; W14-1 F1 ages slots on it)
        acz::ui3d::Options uo; uo.on = true; prod.configure_ui3d(uo);
        vscreen::Options vo; const float head[3] = {0.0f, 1.2f, 0.0f}, q[4] = {0, 0, 0, 1};
        vscreen::publish_placement(vscreen::place(vo, head, q, 16.0f / 9.0f, gen - 1), head, gen);
        live::UiFacts f;
        f.list = &list; f.now_ms = pt; f.vs8 = acz::ui3d::kPs2Vs8; f.vertices = 4; f.rt = &list; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = live::kRtvFormat;
        f.pso_known = true; f.rt_count = 1; f.rtv0 = live::kRtvFormat; f.dsv = live::kDsvFormat; f.samples = 1; f.depth_enable = true;
        f.depth_func = brief3d::kCmpAlways; f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440;
        f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440; f.om_known = true; f.cb_why = nullptr; f.cb0 = acz::ui3d::Cb0{682, -448, 2048, 2048};
        f.vb_why = nullptr; f.stride = 64;
        const float xy[4][2] = {{1792, 1824}, {1792, 2272}, {2304, 1824}, {2304, 2272}};
        for (int i = 0; i < 4; ++i) { const float v[4] = {xy[i][0], xy[i][1], 64.0f, 1.0f}; std::memcpy(f.vtx + 64 * i, v, 16); }
        live::PoseIn pi; pi.ok = true; pi.space_generation = gen;
        for (unsigned e = 0; e < 2; ++e) { pi.eyes[e].position[0] = e ? 0.032 : -0.032; pi.eyes[e].position[1] = 1.2; }
        vscreen::Completion done; done.value = src->fence;
        const live::UiPlan pl = prod.ui3d_draw(f, pi, done);
        CHECK(pl.record && pl.layer == 0);
        prod.ui3d_commit(pl);
        const void* ls[1] = {&list};
        const auto tok = prod.before_execute(&queue, ls, 1);
        prod.after_execute(tok, true);
        prod.on_present(pt);
        prod.on_reset(&list, true);
    }
    bool last_opaque = false; LayerList last_list;
    // one XR frame through the decorator. true = the table layer is in the plan.
    bool frame(int64_t dt = 11) {
        g.t += dt;
        VScreenIn in; in.eye_images[0] = &eye[0]; in.eye_images[1] = &eye[1]; in.eye_ext[0] = in.eye_ext[1] = Extent{2016, 2240};
        xr_owner::LayerPlan plan;
        const bool shown = hook->frame(g, info, xr, kHud, hud, Extent{2400, 1350}, in, plan);
        CHECK(shown == inner->show);
        last_opaque = plan.layer.table_opaque;
        if (plan.layer.table) last_list = compose_layers(plan.layer);
        if (plan.layer.table) {
            CHECK(plan.layer.screen_only && plan.submit);
            CHECK(std::fabs(plan.layer.views[0].pose.position[0] + 0.032f) < 1e-6f && std::fabs(plan.layer.views[1].pose.position[0] - 0.032f) < 1e-6f);
        }
        return plan.layer.table;
    }
};

void test_happy() {
    Rig r;
    CHECK(!r.frame());                         // nothing published yet: the screen alone
    CHECK(r.xr.log.empty());
    r.publish();
    CHECK(!r.frame());                         // published, but the producer fence has not completed: not acquirable
    r.src->fence = 1;
    CHECK(r.frame());
    CHECK((r.xr.log == std::vector<std::string>{"acquire 0 0", "wait 0 ok", "acquire 1 0", "wait 1 ok", "release 1", "release 0"}));
    CHECK(r.gpu->blits == 1 && r.gpu->unwaited_writes == 0 && r.gpu->dst[0] == r.eye[0].images[0] && r.gpu->dst[1] == r.eye[1].images[0]);
    CHECK(r.table->holding() && r.src->acquired == 1);
    CHECK(r.frame() && r.xr.log.size() == 6);  // no new slot: re-submitted without any acquire
    r.gpu->completed_v = 1;
    CHECK(r.frame() && !r.table->holding() && r.src->released == 1);
    r.publish(); r.src->fence = 2;
    CHECK(r.frame() && r.gpu->dst.back() == r.eye[1].images[1]);
    // stale: no new slot for > 500 ms (the map screen was left): the table layer is dropped, the screen stays
    CHECK(!r.frame(600));
    CHECK(r.table->stats().stale == 1);
    r.gpu->completed_v = 2;
    CHECK(r.hook->retire(r.g) && r.gpu->destroys == 1 && r.inner->retired == 1 && r.src->violation == 0 && r.xr.violations == 0);
    CHECK(r.src->acquired == r.src->released);
    const std::string fin = r.table->summary();
    CHECK(fin.find("xr-stereo;event=brief3d-table-result;") == 0);
    // two lines: the table line (+ the producer's fit tail), then the producer line with every counter and the lag fields
    const size_t nl = fin.find('\n');
    CHECK(nl != std::string::npos && fin.find('\n', nl + 1) == std::string::npos && nl < brief3d::kLineLimit && fin.size() - nl - 1 < brief3d::kLineLimit);
    CHECK(fin.find(";frame=2;fits=1;fit-refused=0;placements=1;") != std::string::npos);
    const std::string pl = fin.substr(nl + 1);
    CHECK(pl.find("xr-stereo;event=summary-brief3d-producer;frames=2;published=2;") == 0);
    for (const char* k : {";other-list=", ";reexecutes=", ";discarded=", ";cancelled=", ";invalidated=", ";quarantined=", ";lag-frames=", ";lag-max=",
                          ";age-ms=", ";age-max-ms=", ";poisoned=0;why=none"})
        CHECK(pl.find(k) != std::string::npos);
}
// SCREEN-DUMP: the screen part from the inner hook, the table part (eye 0 of the next blit, over black, rotated 180 degrees) while live.
void test_dump() {
    Rig r;
    r.publish(); r.src->fence = 1;
    CHECK(r.frame());
    r.gpu->completed_v = 1; r.frame();
    CHECK(r.hook->dump_arm(r.g.t) == nullptr && std::string(r.hook->dump_table()) == "armed" && r.hook->dump_busy());
    DumpImage d;
    CHECK(r.hook->dump_take(r.g, d) && d.part == "screen" && d.ok);
    d = DumpImage{};
    CHECK(!r.hook->dump_take(r.g, d) && r.hook->dump_busy());   // the table part waits for the next blit
    r.publish(); r.src->fence = 2;
    CHECK(r.frame() && r.gpu->rb_recorded == 1 && r.gpu->rb_dst == r.gpu->dst[r.gpu->dst.size() - 2]);   // eye 0 of that blit
    CHECK(!r.hook->dump_take(r.g, d));                                  // its fence value has not completed
    r.gpu->completed_v = 2;
    CHECK(r.hook->dump_take(r.g, d) && d.ok && d.part == "eye0-table" && d.w == 2 && d.h == 2);
    // Orientation (Q1a): an EYE image is written as stored, like xr_png_once's eye dumps (NOT rotated here; xr_dump_upright.py rotates every
    // XR dump), premultiplied over black = opaque. The marker stays top-left red, top-right green, bottom-left blue.
    CHECK((d.rgba == std::vector<uint8_t>{40, 0, 0, 255, 0, 50, 0, 255, 0, 0, 60, 255, 0, 0, 0, 255}));
    CHECK(!r.hook->dump_busy());
    // the table stale (the map screen was left): the screen part alone
    CHECK(!r.frame(600));
    CHECK(r.hook->dump_arm(r.g.t) == nullptr && std::string(r.hook->dump_table()) == "table-not-live");
    CHECK(r.hook->dump_take(r.g, d) && d.part == "screen" && !r.hook->dump_busy());
    r.inner->dump_ok = false;
    CHECK(std::string(r.hook->dump_arm(r.g.t)) == "no-screen-image" && !r.hook->dump_busy());
    CHECK(r.src->violation == 0 && r.xr.violations == 0);
}
// SD-2: a timed-out table readback is draining (not released while the GPU may write), reclaimed after proven completion, then a fresh dump works.
void test_dump_timeout_drain() {
    Rig r;
    r.publish(); r.src->fence = 1;
    CHECK(r.frame()); r.gpu->completed_v = 1; r.frame();
    CHECK(r.hook->dump_arm(r.g.t) == nullptr);
    DumpImage d;
    CHECK(r.hook->dump_take(r.g, d) && d.part == "screen");      // the screen part (fake inner) is taken at once
    r.publish(); r.src->fence = 2;
    CHECK(r.frame() && r.gpu->rb_recorded == 1);                 // the table readback is recorded, its fence (2) never completes
    d = DumpImage{};
    r.g.t += DumpTrack::kPendingMs + 10; r.frame();
    CHECK(r.hook->dump_take(r.g, d) && !d.ok && d.part == "eye0-table" && std::string(d.why) == "readback-timeout");
    CHECK(r.hook->dump_busy() && r.gpu->rb_discarded == 0);       // draining
    CHECK(!r.hook->dump_take(r.g, d) && r.gpu->rb_discarded == 0);                  // fence still incomplete: held
    r.gpu->completed_v = r.gpu->value;
    CHECK(!r.hook->dump_take(r.g, d) && r.gpu->rb_discarded == 1 && !r.hook->dump_busy());   // late completion: reclaimed
    r.publish(); r.src->fence = 3; CHECK(r.frame());              // a live table again (the 2 s wait made it stale)
    r.gpu->completed_v = r.gpu->value;
    CHECK(r.hook->dump_arm(r.g.t) == nullptr && std::string(r.hook->dump_table()) == "armed");
    CHECK(r.hook->dump_take(r.g, d) && d.part == "screen");
    r.publish(); r.src->fence = 4;
    CHECK(r.frame() && r.gpu->rb_recorded == 2);                 // second trigger records (no readback-setup)
    r.gpu->completed_v = r.gpu->value;
    d = DumpImage{}; CHECK(r.hook->dump_take(r.g, d) && d.ok && d.part == "eye0-table");
}
// The table line + the producer tail stays < kLineLimit for any value.
void test_line_worst_case() {
    TableCore::Stats s; for (uint64_t* q = &s.blits; q <= &s.generation_dropped; ++q) *q = ~0ull;
    live::Stats ps; for (uint64_t* q = &ps.frames; q <= &ps.age_max_ms; ++q) *q = ~0ull;
    char t[1024], tail[brief3d::kLineLimit];
    const int a = TableCore::format_line(t, sizeof t, false, INT64_MAX, s, 31u, "a-table-reason-longer-than-twenty-four-bytes", ~0ull);
    const int b = live::format_xr_tail(tail, sizeof tail, ps, ~0ull, "a-fit-reason-longer-than-thirty-two-bytes-cut");
    CHECK(a > 0 && b > 0 && size_t(a + b) < brief3d::kLineLimit);
    std::printf("table line worst case %d + tail %d = %d bytes\n", a, b, a + b);
}
void test_screen_hidden_and_written() {
    Rig r; r.publish(); r.src->fence = 1;
    r.inner->show = false;
    CHECK(!r.frame() && r.gpu->blits == 0);    // no screen (flight): no table, nothing acquired
    r.inner->show = true;
    CHECK(r.frame());
    // Output wrote eye 0 for a flight composite: the table is not re-submitted from the swapchain until a new blit
    r.hook->target_written(0);
    CHECK(r.inner->written.size() == 1 && r.inner->written[0] == 0);
    CHECK(!r.frame() && r.table->stats().invalidated == 1);
    r.hook->target_written(3);                 // the HUD target: the table's content stays
    r.gpu->completed_v = 1; r.publish(); r.src->fence = 2;
    CHECK(r.frame());
    r.hook->target_written(3);
    CHECK(r.frame());
}
void test_wait_timeout_poisons() {
    Rig r; r.publish(); r.src->fence = 1;
    r.xr.timeouts = {true, true};
    CHECK(!r.frame());
    CHECK(r.table->poisoned() && r.hook->poisoned() && r.gpu->blits == 0);
    CHECK(!r.hook->retire(r.g));               // retained: the acquisition is unsettled
    CHECK(r.src->released == 1);               // the producer slot was never read: released at once
}
void test_eye1_acquire_fails() {
    Rig r; r.publish(); r.src->fence = 1;
    r.xr.fail_acquire_target = 1;
    CHECK(!r.frame());
    CHECK(!r.table->poisoned() && r.table->dead() && r.gpu->blits == 0);
    CHECK(!r.xr.acquired[0] && r.xr.violations == 0);   // eye 0 released unwritten
    CHECK(r.hook->retire(r.g));
}
void test_signal_and_record_failures() {
    {
        Rig r; r.publish(); r.src->fence = 1; r.gpu->signal_fails = true;
        CHECK(!r.frame());
        CHECK(r.table->quarantined() && r.table->poisoned() && r.table->holding());
        r.gpu->completed_v = 100;
        r.frame();
        CHECK(r.src->released == 0);           // never released: completion can never be proven
        CHECK(!r.hook->retire(r.g));
    }
    {
        Rig r; r.publish(); r.src->fence = 1; r.g.refuse = {xr_owner::Op::d3d_execute};
        CHECK(!r.frame());                     // the owner refused the GPU call: nothing executed
        CHECK(r.gpu->blits == 0 && !r.table->poisoned() && !r.table->content_ok() && r.src->released == 1 && r.xr.violations == 0);
        r.g.refuse.clear(); r.publish(); r.src->fence = 2;
        CHECK(r.frame());                      // recovers with the next slot
    }
    {
        Rig r; r.publish(); r.src->fence = 1; r.gpu->setup_fails = true;
        CHECK(!r.frame() && r.table->dead() && r.src->released == 1 && r.xr.log.empty());
        CHECK(r.hook->retire(r.g) && r.gpu->destroys == 1);   // partial setup destroyed
    }
    {
        Rig r; r.publish(); r.src->fence = 1; r.xr.release_fails = true;
        CHECK(!r.frame() && r.table->poisoned());
    }
}
void test_fence_loss_and_dead_producer() {
    Rig r; r.publish(); r.src->fence = 1;
    CHECK(r.frame());
    r.gpu->lose = true;
    r.frame();
    CHECK(r.table->dead() && r.table->holding() && r.src->released == 0);   // loss is never completion
    Rig d; d.publish(); d.src->fence = 1;
    const void* ls[1] = {&d.list}; int q = 0;
    d.prod.on_reset(&d.list, true);
    live::Plan dummy;
    (void)dummy; (void)ls; (void)q;
    d.prod.gpu_failed("brief3d-gpu-test");
    CHECK(!d.frame() && d.table->dead() && std::string(d.table->why()) == "producer-dead");
}
// Astra 6460640585 P2-4: LOCAL-space generation (recentre). Slots carry the ENCODED generation; FrameInfo the raw one.
void test_space_generation() {
    Rig r; r.publish(1); r.src->fence = 1;
    CHECK(r.frame());                                     // generation 0 -> encoded 1: matches
    // existing content after a recentre: never submitted again; the held source still waits for its blit fence
    r.info.space_generation = 1;                          // encoded 2
    CHECK(!r.frame());
    CHECK(r.table->stats().generation_dropped == 1 && r.table->holding() && r.src->released == 0);
    r.gpu->completed_v = 1;
    CHECK(!r.frame() && !r.table->holding() && r.src->released == 1);
    // a queued READY slot of the OLD generation: refused before any eye acquisition
    const size_t before = r.xr.log.size();
    r.publish(1); r.src->fence = 2;
    CHECK(!r.frame());
    CHECK(r.table->stats().generation_refused == 1 && r.xr.log.size() == before && r.src->released == 2 && !r.table->holding());
    // the next slot rendered in the new generation is shown
    r.publish(2); r.src->fence = 3;
    CHECK(r.frame() && r.xr.log.size() == before + 6);
    // an unknown generation (0) is refused too
    Rig u; u.publish(0); u.src->fence = 1;
    CHECK(!u.frame() && u.table->stats().generation_refused == 1 && u.xr.log.empty());
}
void test_ui3d_opaque() {
    Rig r;
    r.publish_ui(); r.src->fence = 1;
    CHECK(r.frame() && r.last_opaque && r.gpu->last_opaque);
    // 2026-10-30b (u3q:1, the default): SteamVR draws quad layers ABOVE projection layers, so the opaque menu / hangar frame is submitted ALONE:
    // no screen quad (the user saw the flat game image in front of the hangar room), the projection WITHOUT source alpha
    CHECK(r.last_list.count == 1 && r.last_list.e[0].kind == LayerKind::projection && !r.last_list.e[0].source_alpha);
    // a briefing table slot after it: the screen quad and then the table as ONE QUAD PER EYE (premultiplied), so the raised markers are not hidden
    r.gpu->completed_v = 1; CHECK(r.frame());
    r.publish(); r.src->fence = 2;
    CHECK(r.frame() && !r.last_opaque && !r.gpu->last_opaque);
    CHECK(r.last_list.count == 3 && r.last_list.e[0].kind == LayerKind::screen_quad && r.last_list.e[1].kind == LayerKind::table_quad &&
          r.last_list.e[2].kind == LayerKind::table_quad && r.last_list.e[1].target == 0 && r.last_list.e[2].target == 1 &&
          r.last_list.e[1].source_alpha && r.last_list.e[2].source_alpha && !r.last_list.e[1].unpremultiplied);
    {   // u3q:0: the old list (screen quad, then the projection on top of it in list order)
        Rig o; o.table->set_layering(0);
        o.publish_ui(); o.src->fence = 1;
        CHECK(o.frame() && o.last_list.count == 2 && o.last_list.e[0].kind == LayerKind::screen_quad && o.last_list.e[1].kind == LayerKind::projection &&
              !o.last_list.e[1].source_alpha);
        o.gpu->completed_v = 1; CHECK(o.frame());
        o.publish(); o.src->fence = 2;
        CHECK(o.frame() && o.last_list.count == 2 && o.last_list.e[1].source_alpha);
    }
    // stale (> 500 ms without a new slot): dropped -> the flat screen, as for the table
    CHECK(!r.frame(600) && !r.last_opaque);
}
// W14-1 F1: an old READY slot (published, fence complete, never acquired for > 500 ms: e.g. the screen was not shown) must not regain freshness by
// being acquired late: refused before any eye acquisition (released, no blit, no layer) for UI3D (opaque: a hangar/menu room) and table slots alike.
void test_w14_f1_old_publication() {
    {
        Rig r; r.publish_ui(); r.src->fence = 1;            // published at producer time 16 (the fake source's clock = the gate's)
        r.inner->show = true;
        CHECK(!r.frame(600));                               // first acquisition 595 ms after the publication: stale
        CHECK(r.table->stats().stale_published == 1 && r.src->acquired == 1 && r.src->released == 1 && r.gpu->blits == 0 && r.xr.log.empty());
        CHECK(!r.table->holding());
        // a fresh publication is shown again
        r.pt = r.g.t; r.publish_ui(); r.src->fence = 2;      // published at 616
        CHECK(r.frame(400) && r.last_opaque && r.gpu->blits == 1);   // acquired at 1000: 384 ms old, shown
        // retained content expires on its PUBLICATION time (1200 - 616 = 584 ms), although it was acquired only 200 ms ago
        r.gpu->completed_v = 1;
        CHECK(!r.frame(200));
    }
    {
        // a briefing-table slot (stamped at its Reset with the previous Present): aged the same way
        Rig r; r.publish(); r.src->fence = 1;
        CHECK(!r.frame(600) && r.table->stats().stale_published == 1 && r.gpu->blits == 0);
    }
}
void test_layers() {
    xr_owner::ProjectionLayer l; l.screen_only = true;
    LayerList a = compose_layers(l);
    CHECK(a.count == 1 && a.e[0].kind == LayerKind::screen_quad);
    l.table = true;
    a = compose_layers(l);
    CHECK(a.count == 2 && a.e[0].kind == LayerKind::screen_quad && a.e[1].kind == LayerKind::projection && a.e[1].source_alpha && !a.e[1].unpremultiplied);
    xr_owner::ProjectionLayer f; f.table = true;   // without the screen the flag means nothing (flight lists unchanged)
    const LayerList b = compose_layers(f);
    CHECK(b.count == 1 && b.e[0].kind == LayerKind::projection && !b.e[0].source_alpha);
    l.table_opaque = true;   // UI3D (old list, table_front false): the projection covers the quad (opaque) in list order
    a = compose_layers(l);
    CHECK(a.count == 2 && a.e[1].kind == LayerKind::projection && !a.e[1].source_alpha && !a.e[1].unpremultiplied);
    // 2026-10-30b table_front: an opaque slot alone; a premultiplied table as two per-eye quads after the screen quad; no table = the screen alone
    l.table_front = true;
    a = compose_layers(l);
    CHECK(a.count == 1 && a.e[0].kind == LayerKind::projection && !a.e[0].source_alpha);
    l.table_opaque = false;
    a = compose_layers(l);
    CHECK(a.count == 3 && a.e[0].kind == LayerKind::screen_quad && a.e[1].kind == LayerKind::table_quad && a.e[2].kind == LayerKind::table_quad &&
          a.e[1].target == 0 && a.e[2].target == 1 && a.e[1].source_alpha && a.e[2].source_alpha);
    l.table = false;
    a = compose_layers(l);
    CHECK(a.count == 1 && a.e[0].kind == LayerKind::screen_quad);
    xr_owner::ProjectionLayer fl; fl.table = true; fl.table_front = true; fl.table_opaque = true;   // flight lists stay unchanged
    const LayerList fb = compose_layers(fl);
    CHECK(fb.count == 1 && fb.e[0].kind == LayerKind::projection);
    // an OPAQUE slot (UI3D menus, hangar, DEBRIEF 3D: SlotMeta::opaque) with table_front is ONE projection layer, never per-eye quads: there is no quad plane
    // (kTableQuadDistanceM) for it; only the premultiplied briefing table uses table_quad (follow-up 2026-11-01: dbr3d needs no plane fix).
    xr_owner::ProjectionLayer ol; ol.screen_only = true; ol.table = true; ol.table_front = true; ol.table_opaque = true;
    const LayerList ob = compose_layers(ol);
    CHECK(ob.count == 1 && ob.e[0].kind == LayerKind::projection && !ob.e[0].source_alpha);
}
// view_quad: the plane at d perpendicular to the view axis whose edges keep the view's directions (an asymmetric fov, a turned head)
void test_view_quad() {
    using xr_owner::EyeSample;
    EyeSample v;
    v.fov.left = -0.9f; v.fov.right = 0.7f; v.fov.up = 0.8f; v.fov.down = -0.6f;
    v.pose.position[0] = 0.1f; v.pose.position[1] = 1.6f; v.pose.position[2] = -0.2f;
    const float yaw = 0.6f;   // about +Y
    v.pose.orientation[0] = 0; v.pose.orientation[1] = std::sin(yaw * 0.5f); v.pose.orientation[2] = 0; v.pose.orientation[3] = std::cos(yaw * 0.5f);
    const float d = 1.5f;
    const Quad q = view_quad(v, d);
    CHECK(std::fabs(q.width - d * (std::tan(0.7f) + std::tan(0.9f))) < 1e-5f && std::fabs(q.height - d * (std::tan(0.8f) + std::tan(0.6f))) < 1e-5f);
    // the quad centre in the view frame: (d (tr + tl) / 2, d (tu + td) / 2, -d); Ry(yaw) maps view x to (cos, 0, -sin), view z to (sin, 0, cos)
    const float ox = d * 0.5f * (std::tan(0.7f) - std::tan(0.9f)), oy = d * 0.5f * (std::tan(0.8f) - std::tan(0.6f)), oz = -d;
    const float wx = ox * std::cos(yaw) + oz * std::sin(yaw), wy = oy, wz = -ox * std::sin(yaw) + oz * std::cos(yaw);
    CHECK(std::fabs(q.pose.position[0] - (0.1f + wx)) < 1e-5f && std::fabs(q.pose.position[1] - (1.6f + wy)) < 1e-5f && std::fabs(q.pose.position[2] - (-0.2f + wz)) < 1e-5f);
    CHECK(q.pose.orientation[1] == v.pose.orientation[1] && q.pose.orientation[3] == v.pose.orientation[3]);
    // identity head, symmetric fov: straight ahead, centred, width = 2 d tan
    EyeSample s; s.fov = {-0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f};
    const Quad c = view_quad(s, 2.0f);
    CHECK(std::fabs(c.pose.position[0]) < 1e-5f && std::fabs(c.pose.position[1]) < 1e-5f && std::fabs(c.pose.position[2] + 2.0f) < 1e-5f && std::fabs(c.width - 4.0f) < 1e-4f);
}

} // namespace

int main() {
    test_happy();
    test_line_worst_case();
    test_dump();
    test_dump_timeout_drain();
    test_screen_hidden_and_written();
    test_wait_timeout_poisons();
    test_eye1_acquire_fails();
    test_signal_and_record_failures();
    test_fence_loss_and_dead_producer();
    test_space_generation();
    test_layers();
    test_view_quad();
    test_ui3d_opaque();
    test_w14_f1_old_publication();
    std::printf("brief3d_table_offline: %u checks passed\n", g_checks);
    return 0;
}
