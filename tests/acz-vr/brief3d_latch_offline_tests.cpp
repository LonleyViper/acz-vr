// BRIEF3D section 11 offline tests (src/acz-vr/brief3d_latch.h, brief3d_live.h Producer::latch_draw, xr_brief3d_core.h LatchCore): the briefing
// table is LATCHED once per game frame and re-drawn by the XR consumer every XR frame, per eye, with that frame's head pose. The central check
// (test_world_locked_across_head_motion): the head moves and turns every XR frame while the game updates the map only every 8th XR frame (with
// its own camera orbiting); every grid vertex, decoded from the exact float32 record the GPU would use and un-projected with an independent
// OpenXR view model, lands on the SAME LOCAL point in every frame and both eyes. Fakes only: no device, no headset, no game - fixture success says
// nothing about the live game or SteamVR.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "xr_brief3d_core.h"
#include "xr_stereo_output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace acz;
using namespace acz::xr_stereo;
namespace b3 = acz::brief3d;
namespace lt = acz::brief3d::latch;
namespace live = acz::brief3d::live;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {

struct FakeGate final : xr_owner::Gate {
    int64_t t = 1000; bool lost_ = false;
    int64_t now_ms() override { return t; }
    void sleep_ms(int64_t ms) override { t += ms; }
    bool lost() const override { return lost_; }
    bool stopping() const override { return false; }
protected:
    xr_owner::Rc invoke(xr_owner::Op, xr_owner::Rc (*fn)(void*), void* p) override { return fn(p); }
};
struct FakeChains final : SwapchainRuntime {
    bool acquired[5] = {}, waited[5] = {}; uint32_t next[5] = {}; unsigned violations = 0;
    xr_owner::Rc view_configs(std::array<ViewConfig, 2>&) override { return xr_owner::Rc::ok; }
    xr_owner::Rc formats(std::array<int64_t, max_formats>&, uint32_t& n) override { n = 0; return xr_owner::Rc::ok; }
    xr_owner::Rc create_swapchain(unsigned, int64_t, uint32_t, uint32_t, SwapchainImages&) override { return xr_owner::Rc::ok; }
    xr_owner::Rc acquire(unsigned t, uint32_t& i) override {
        if (acquired[t]) ++violations;
        acquired[t] = true; waited[t] = false; i = next[t]; next[t] = (next[t] + 1) % 3; return xr_owner::Rc::ok;
    }
    xr_owner::Rc wait(unsigned t, int64_t, bool& to) override { if (!acquired[t] || waited[t]) ++violations; to = false; waited[t] = true; return xr_owner::Rc::ok; }
    xr_owner::Rc release(unsigned t) override { if (!acquired[t] || !waited[t]) ++violations; acquired[t] = false; waited[t] = false; return xr_owner::Rc::ok; }
    xr_owner::Rc destroy_swapchain(unsigned) override { return xr_owner::Rc::ok; }
};
// Records every render's plan (what the D3D12 render would upload and draw).
struct FakeGpu final : TableGpu {
    FakeChains* chains = nullptr;
    uint64_t value = 0, completed_v = 0; bool signal_fails = false;
    unsigned renders = 0, blits = 0, unwaited = 0, destroys = 0;
    std::vector<lt::FramePlan> plans;
    bool setup(const SwapchainImages&, const SwapchainImages&, const char*&) override { return true; }
    vscreen::Completion completed() override { vscreen::Completion c; c.value = completed_v; return c; }
    Submit blit(unsigned, void*, void*, const TableImages&, Extent, Extent) override { ++blits; return Submit{}; }
    Submit render(unsigned, void*, void*, const lt::FramePlan& f, Extent, Extent) override {
        ++renders; Submit s;
        if (!chains->waited[0] || !chains->waited[1]) ++unwaited;
        plans.push_back(f);
        s.executed = true;
        if (signal_fails) return s;
        s.signalled = true; s.value = ++value; return s;
    }
    void destroy() override { ++destroys; }
};
struct ProdLatch final : LatchSource {
    live::Producer* p = nullptr;
    std::shared_ptr<const lt::Scene> scene() override { return p->latched_scene(); }
    bool dead() override { return p->poisoned(); }
};

// ---- quaternions (x, y, z, w)
struct Q { double x = 0, y = 0, z = 0, w = 1; };
Q qmul(const Q& a, const Q& b) {
    return Q{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q axis(double ax, double ay, double az, double ang) { const double s = std::sin(ang / 2); return Q{ax * s, ay * s, az * s, std::cos(ang / 2)}; }
Q ypr(double yaw, double pitch, double roll) { return qmul(qmul(axis(0, 1, 0, yaw), axis(1, 0, 0, pitch)), axis(0, 0, 1, roll)); }

// a head pose -> the two located views (IPD 64 mm, slightly asymmetric FOVs like a real HMD)
xr_owner::FrameInfo frame_info(const double head[3], const Q& q, uint32_t raw_gen = 0) {
    xr_owner::FrameInfo info;
    info.views_valid = true; info.views_tracked = true; info.space_generation = raw_gen;
    info.views.view_count = 2;
    const double qa[4] = {q.x, q.y, q.z, q.w};
    for (unsigned e = 0; e < 2; ++e) {
        const double off[3] = {e ? 0.032 : -0.032, 0, 0};
        double r[3]; b3::quat_rotate(qa, off, r);
        auto& s = info.views.eyes[e];
        for (int k = 0; k < 3; ++k) s.pose.position[k] = float(head[k] + r[k]);
        for (int k = 0; k < 4; ++k) s.pose.orientation[k] = float(qa[k]);
        s.fov.left = e ? -0.80f : -0.86f; s.fov.right = e ? 0.86f : 0.80f; s.fov.up = 0.84f; s.fov.down = -0.88f;
    }
    return info;
}
live::PoseIn pose_in(const xr_owner::FrameInfo& info) {
    live::PoseIn p; p.ok = true; p.space_generation = encode_space_generation(info.space_generation);
    for (unsigned e = 0; e < 2; ++e) p.eyes[e] = eye_view(info.views.eyes[e]);
    return p;
}

// ---- a synthetic briefing grid (the live shape: 128 x 128 vertices, float4 x, height, z, colour index; 2 x 128 x 127 line segments)
constexpr unsigned kN = 128;
std::shared_ptr<std::vector<uint8_t>> grid_vb() {
    auto v = std::make_shared<std::vector<uint8_t>>(size_t(kN) * kN * 16);
    for (unsigned j = 0; j < kN; ++j)
        for (unsigned i = 0; i < kN; ++i) {
            const float p[4] = {-32.0f + 64.0f * float(i) / float(kN - 1), float(1.5 * (1 + std::sin(i * 0.11)) * (1 + std::cos(j * 0.07)) / 4.0 * 2.0),
                                -32.0f + 64.0f * float(j) / float(kN - 1), float((i + j) % 256)};
            std::memcpy(v->data() + (size_t(j) * kN + i) * 16, p, 16);
        }
    return v;
}
std::shared_ptr<std::vector<uint8_t>> grid_ib() {
    std::vector<uint16_t> ix;
    for (unsigned j = 0; j < kN; ++j) for (unsigned i = 0; i + 1 < kN; ++i) { ix.push_back(uint16_t(j * kN + i)); ix.push_back(uint16_t(j * kN + i + 1)); }
    for (unsigned j = 0; j + 1 < kN; ++j) for (unsigned i = 0; i < kN; ++i) { ix.push_back(uint16_t(j * kN + i)); ix.push_back(uint16_t((j + 1) * kN + i)); }
    auto b = std::make_shared<std::vector<uint8_t>>(ix.size() * 2);
    std::memcpy(b->data(), ix.data(), b->size());
    return b;
}
// the game's own SelMap matrix: a camera orbiting above the map (model y up), `orbit` radians about y
void game_matrix(double orbit, uint8_t out[64]) {
    b3::EyeView cam;
    const Q q = qmul(axis(0, 1, 0, orbit), axis(1, 0, 0, -0.39));
    cam.orientation[0] = q.x; cam.orientation[1] = q.y; cam.orientation[2] = q.z; cam.orientation[3] = q.w;
    cam.position[0] = 60 * std::sin(orbit); cam.position[1] = 60; cam.position[2] = 60 * std::cos(orbit);
    cam.fov[0] = -0.6; cam.fov[1] = 0.6; cam.fov[2] = 0.35; cam.fov[3] = -0.35;
    const float b10[2] = {682.0f, -448.0f};
    b3::Mat4 P; b3::ps2_projection(cam.fov, b10, 0.5, b3::DepthMode::reverse, P);
    b3::store(b3::mul(b3::view_from_eye(cam), P), out);
}
struct Handles { std::shared_ptr<int> pso = std::make_shared<int>(1), root = std::make_shared<int>(2); };

live::DrawFacts facts(b3::Kind k, int64_t now, const uint8_t* rec) {
    live::DrawFacts f; f.kind = k; f.list = nullptr; f.now_ms = now; f.root_why = nullptr; f.t10_param = 0; f.b10_param = k == b3::Kind::selmap ? 2 : 1;
    f.t10_va = 0x10; f.t10_why = nullptr; f.b10_why = nullptr; f.b10[0] = 682; f.b10[1] = -448;
    std::memcpy(f.rec, rec, b3::srt_bytes(k));
    f.pso_known = true; f.rt_count = 1; f.rtv0 = live::kRtvFormat; f.dsv = live::kDsvFormat; f.samples = 1; f.depth_enable = true; f.depth_write = true;
    f.depth_func = b3::kCmpGreaterEqual; f.stencil = true; f.stencil_always = true;
    return f;
}
lt::Geom grid_geom(const Handles& h, const lt::Bytes& vb, const lt::Bytes& ib) {
    lt::Geom g; g.vb = vb; g.ib = ib; g.t11 = std::make_shared<std::vector<uint8_t>>(4096, uint8_t(0x40));
    g.index_bytes = 2; g.index_count = uint32_t(ib->size() / 2); g.stride = 16; g.base_vertex = 0; g.ia_topology = lt::kIaLineList; g.t11_param = 1;
    g.pso = h.pso; g.root = h.root;
    return g;
}

struct Rig {
    live::Producer prod;
    FakeGate g; FakeChains xr;
    std::shared_ptr<FakeGpu> gpu = std::make_shared<FakeGpu>();
    std::shared_ptr<ProdLatch> src = std::make_shared<ProdLatch>();
    std::shared_ptr<LatchCore> core;
    SwapchainImages eye[2];
    Handles h;
    lt::Bytes vb = grid_vb(), ib = grid_ib();
    b3::Box box;
    // `plane` = b3p (headset test 9): the tests written before it keep the per-eye quads; test_fixed_plane_* run the default
    explicit Rig(bool plane = false) {
        b3::Options o; o.on = true; o.fixed_plane = plane; prod.configure(o);
        CHECK(prod.latch_mode());
        gpu->chains = &xr; src->p = &prod;
        core = std::make_shared<LatchCore>(gpu, src);
        for (unsigned e = 0; e < 2; ++e) { eye[e].count = 3; for (unsigned i = 0; i < 3; ++i) eye[e].images[i] = reinterpret_cast<void*>(uintptr_t(0x1000 * (e + 1) + i)); }
        CHECK(b3::box_from_vertices(vb->data(), vb->size(), 16, 0, kN * kN, box) == nullptr);
    }
    // one game frame: the grid draw (+ optional extra draws by the caller), then the Present. The producer clock = the gate clock.
    void game_frame(double orbit, const xr_owner::FrameInfo& head, const char* geom_why = nullptr) {
        uint8_t rec[64]; game_matrix(orbit, rec);
        const live::DrawFacts f = facts(b3::Kind::selmap, g.t, rec);
        prod.latch_draw(f, pose_in(head), prod.wants_box(g.t) ? &box : nullptr, nullptr, grid_geom(h, vb, ib), geom_why);
        prod.on_present(g.t);
    }
    bool xr_frame(const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
        const Extent ext{1440, 1600};
        return core->frame(g, xr, &eye[0], &eye[1], ext, ext, info, plan);
    }
};

// ------------------------------------------------------------------------------------------------ tests
void test_read_cache() {
    lt::ReadCache c;
    std::vector<uint8_t> src(200000);
    for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i * 7);
    unsigned reads = 0; uint64_t bytes = 0; bool tear = false;
    auto read = [&](uint64_t off, uint64_t n, uint8_t* dst) {
        ++reads; bytes += n;
        std::memcpy(dst, src.data() + off, size_t(n));
        if (tear && n == src.size()) { src[size_t(lt::ReadCache::offset(src.size(), 5))] ^= 0xFF; tear = false; }   // the game rewrites it right after the copy
        return true;
    };
    lt::Bytes a, b;
    CHECK(c.get(src.size(), 100, read, a) == nullptr && a && a->size() == src.size() && std::memcmp(a->data(), src.data(), src.size()) == 0);
    CHECK(c.stats().full == 1);
    CHECK(c.get(src.size(), 110, read, b) == nullptr && b.get() == a.get() && c.stats().hits == 1);   // same content: the same blob, no full read
    {   // review 2026-10-31 F7: the same sampled content at ANOTHER buffer (a new map whose sampled words match) is not served from the cache, and a
        // gap without reads (the screen was left) empties it
        lt::ReadCache k; lt::Bytes x, y;
        CHECK(k.get(src.size(), 100, read, x, 0x1000) == nullptr && k.stats().full == 1);
        CHECK(k.get(src.size(), 110, read, y, 0x1000) == nullptr && y.get() == x.get() && k.stats().hits == 1);
        CHECK(k.get(src.size(), 200, read, y, 0x9000) == nullptr && y.get() != x.get() && k.stats().full == 2 && k.stats().hits == 1);
        CHECK(k.get(src.size(), 210, read, y, 0x1000) == nullptr && y.get() == x.get() && k.stats().hits == 2);   // both alternating buffers cached
        CHECK(k.get(src.size(), 210 + lt::kCacheIdleClearMs + 1, read, y, 0x1000) == nullptr && y.get() != x.get() && k.stats().full == 3 &&
              k.stats().idle_clears == 1);
    }
    src[size_t(lt::ReadCache::offset(src.size(), 3))] ^= 1;                                           // a sampled word changed: a miss
    // headset test 7: inside the window's byte budget the miss is read at once (it used to wait for the next 50 ms window: b3l-read-rate)
    CHECK(c.get(src.size(), 120, read, b) == nullptr && b.get() != a.get() && c.stats().full == 2 && c.stats().miss_fp == 1 && c.stats().rate == 0);
    CHECK(c.get(src.size(), 120 + lt::kRefreshMs, read, b) == nullptr && c.stats().full == 3 && c.stats().miss_age == 1);   // re-read at least once per second
    tear = true;   // torn: the source changes between the copy and the second sample pass
    src[size_t(lt::ReadCache::offset(src.size(), 7))] ^= 1;
    CHECK(std::string(c.get(src.size(), 5000, read, b)) == "b3l-read-torn" && !b && c.stats().torn == 1);
    std::vector<uint8_t> small(300, 9);
    auto rs = [&](uint64_t off, uint64_t n, uint8_t* dst) { std::memcpy(dst, small.data() + off, size_t(n)); return true; };
    CHECK(c.get(300, 6000, rs, b) == nullptr && b->size() == 300 && (*b)[299] == 9);                   // small: read whole every time
    CHECK(std::string(c.get(0, 6000, rs, b)) == "b3l-read-empty");
    CHECK(std::string(c.get(lt::kBufCap + 1, 6000, rs, b)) == "b3l-read-too-large");
    // index ranges: the strip cut value is skipped only when asked
    const uint16_t ix[6] = {7, 3, 0xFFFF, 9, 4, 5};
    uint32_t lo = 0, hi = 0;
    CHECK(lt::index_range(reinterpret_cast<const uint8_t*>(ix), 2, 6, true, lo, hi) && lo == 3 && hi == 9);
    CHECK(lt::index_range(reinterpret_cast<const uint8_t*>(ix), 2, 6, false, lo, hi) && lo == 3 && hi == 0xFFFF);
    CHECK(!lt::index_range(reinterpret_cast<const uint8_t*>(ix), 3, 6, true, lo, hi));
}

// Headset test 7 (step160, D:\acz-evidence-archive\headset-test7-20261031): the briefing table updated at 20-36 scenes/s instead of 60 because
// every map frame needs TWO large buffers (grid IB 130,048 B + grid VB 520,192 B) and the cache admitted one large read per 50 ms: a frame in
// which both missed always lost the second (b3l-read-rate) and was dropped (live: incomplete 225 of 507 map frames, rate 225). The model here
// is the glue's per-draw sequence (IB get, then VB get over the IB's vertex range) at 60 game frames per second, worst case: the VB content
// changes in EVERY frame. Failing before: about one frame in three published. Now: every frame after the first publishes, a served copy is
// never older than kStaleServeMs, and the bytes read per window stay within the budget.
struct LiveBuf {
    uint64_t va; std::vector<uint8_t> data; uint32_t gen = 0; std::vector<int64_t> written;   // written[gen] = the frame time that content belongs to
    LiveBuf(uint64_t v, size_t n) : va(v), data(n) { rewrite(0); }
    void rewrite(int64_t t) { ++gen; written.push_back(t); for (size_t i = 0; i < data.size(); i += 8) { const uint64_t w = (uint64_t(gen) << 32) | i; std::memcpy(&data[i], &w, 8); } }
    uint32_t gen_of(const lt::Bytes& b) const { uint64_t w; std::memcpy(&w, b->data(), 8); return uint32_t(w >> 32); }
};
struct BudgetRun { unsigned frames = 0, complete = 0, complete_after_warmup = 0; int64_t max_served_age = 0; uint64_t max_window_bytes = 0; };
BudgetRun run_live_pattern(bool alternate, unsigned frames, unsigned warmup) {
    lt::ReadCache c;
    LiveBuf ib_a(0x10000000, 130048), vb_a(0x20000000, 520192), ib_b(0x30000000, 130048), vb_b(0x40000000, 520192);
    BudgetRun out;
    uint64_t win_bytes = 0; int64_t win_start = 0;
    for (unsigned f = 0; f < frames; ++f) {
        const int64_t t = 1000 + int64_t(f) * 50 / 3;                  // 60 Hz, millisecond clock
        const bool b_set = alternate && (f & 1);
        LiveBuf& vb = b_set ? vb_b : vb_a; LiveBuf& ib = b_set ? ib_b : ib_a;
        vb.rewrite(t);                                                   // the game wrote this frame's grid VB (content changed)
        uint64_t frame_bytes = 0;
        auto reader = [&](LiveBuf& src) { return [&src, &frame_bytes](uint64_t off, uint64_t n, uint8_t* dst) {
            if (n > 8) frame_bytes += n; std::memcpy(dst, src.data.data() + off, size_t(n)); return true; }; };
        lt::Bytes ibc, vbc;
        const char* w1 = c.get(ib.data.size(), t, reader(ib), ibc, ib.va);
        const char* w2 = w1 ? "skipped" : c.get(vb.data.size(), t, reader(vb), vbc, vb.va);
        if (t - win_start >= lt::kLargeReadGapMs) { win_start = t; win_bytes = 0; }
        win_bytes += frame_bytes; out.max_window_bytes = std::max(out.max_window_bytes, win_bytes);
        ++out.frames;
        if (!w1 && !w2) {
            ++out.complete; if (f >= warmup) ++out.complete_after_warmup;
            out.max_served_age = std::max(out.max_served_age, t - vb.written[vb.gen_of(vbc) - 1]);
            CHECK(ib.gen_of(ibc) >= 1 && vbc->size() == vb.data.size());
        }
    }
    return out;
}
void test_read_budget_live_pattern() {
    {   // one buffer set (the IB static, the VB rewritten every frame)
        const BudgetRun r = run_live_pattern(false, 300, 1);
        CHECK(r.complete == r.frames);                                   // failing before: ~100 of 300 (the VB waited for the next window)
        CHECK(r.max_served_age <= lt::kStaleServeMs);
        CHECK(r.max_window_bytes <= lt::kLargeReadBudget);
    }
    {   // the live layout [E D-370]: two alternating buffer sets, both rewritten every frame
        const BudgetRun r = run_live_pattern(true, 300, 8);
        CHECK(r.complete_after_warmup == r.frames - 8);
        CHECK(r.max_served_age <= lt::kStaleServeMs);
        CHECK(r.max_window_bytes <= lt::kLargeReadBudget + 520192);      // at most one forced (starvation-guard) read on top of the budget
    }
    {   // starvation guard: a 1 MiB buffer read first in every window exhausts the budget. The second buffer waits at most kStaleServeMs for its
        // first copy, is then never refused, and is refreshed (forced) before its copy is older than kStaleServeMs
        lt::ReadCache c;
        LiveBuf big(0x50000000, lt::kLargeReadBudget), vb(0x20000000, 520192);
        unsigned refused = 0, refused_after = 0, served = 0; int64_t max_age = 0, first_ok = -1;
        for (unsigned f = 0; f < 200; ++f) {
            const int64_t t = 1000 + int64_t(f) * 50 / 3;
            big.rewrite(t); vb.rewrite(t);
            auto rd = [](LiveBuf& s) { return [&s](uint64_t off, uint64_t n, uint8_t* dst) { std::memcpy(dst, s.data.data() + off, size_t(n)); return true; }; };
            lt::Bytes x, y;
            CHECK(c.get(big.data.size(), t, rd(big), x, big.va) == nullptr);
            const char* w = c.get(vb.data.size(), t, rd(vb), y, vb.va);
            if (w) { ++refused; if (first_ok >= 0) ++refused_after; CHECK(std::string(w) == "b3l-read-rate"); continue; }
            if (first_ok < 0) first_ok = t;
            ++served; max_age = std::max(max_age, t - vb.written[vb.gen_of(y) - 1]);
        }
        CHECK(first_ok >= 0 && first_ok - 1000 <= lt::kStaleServeMs + 17 && refused_after == 0 && served == 200 - refused);
        CHECK(c.stats().forced >= 5 && c.stats().old > 0 && max_age <= lt::kStaleServeMs);
    }
    {   // a buffer never read before is still refused over the budget (no copy to serve)
        lt::ReadCache c;
        LiveBuf a(0x1000, 800000), b(0x2000, 400000);
        auto rd = [](LiveBuf& s) { return [&s](uint64_t off, uint64_t n, uint8_t* dst) { std::memcpy(dst, s.data.data() + off, size_t(n)); return true; }; };
        lt::Bytes x, y;
        CHECK(c.get(a.data.size(), 100, rd(a), x, a.va) == nullptr);
        CHECK(std::string(c.get(b.data.size(), 110, rd(b), y, b.va)) == "b3l-read-rate" && c.stats().rate == 1 && c.stats().miss_new == 2);
        CHECK(c.get(b.data.size(), 150, rd(b), y, b.va) == nullptr);       // the next window
        b.rewrite(160);
        CHECK(c.get(a.data.size(), 200, rd(a), x, a.va) == nullptr && c.stats().hits == 1);   // window [200..): a unchanged = hit
        CHECK(c.get(b.data.size(), 205, rd(b), y, b.va) == nullptr && c.stats().full == 3);    // b changed: read inside the budget
    }
}

// Headset test 7: in a frame whose grid draw was refused (b3l-read-rate), the icons used to be checked against the PREVIOUS frame's grid matrix
// (lt_have_grid_ survived the Present); with the map camera orbiting every icon was refused (brief3d-icon-camera-unmatched). The frame is still
// dropped, but only the grid draw counts as refused now.
void test_grid_matrix_per_frame() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
    r.g.t += 16; r.game_frame(0.0, info);
    CHECK(r.prod.latched_scene() && r.prod.latched_scene()->serial == 1);
    uint8_t grid[64]; game_matrix(0.1, grid);   // the camera orbited
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, grid), pose_in(info), nullptr, nullptr, lt::Geom{}, "b3l-read-rate");
    uint8_t icon[272] = {};
    std::memcpy(icon, grid, 64);
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(icon + 64, id, 64); std::memcpy(icon + 128, id, 64); std::memcpy(icon + 192, id, 64);
    lt::Geom ig;
    {
        const uint16_t ix[4] = {0, 1, 2, 3};
        auto ib = std::make_shared<std::vector<uint8_t>>(8); std::memcpy(ib->data(), ix, 8);
        auto vb = std::make_shared<std::vector<uint8_t>>(4 * 24, uint8_t(0));
        ig.ib = ib; ig.vb = vb; ig.index_bytes = 2; ig.index_count = 4; ig.stride = 24; ig.ia_topology = 5; ig.pso = r.h.pso; ig.root = r.h.root;
    }
    r.prod.latch_draw(facts(b3::Kind::modelicon, r.g.t, icon), pose_in(info), nullptr, nullptr, ig, nullptr);
    r.prod.on_present(r.g.t);
    CHECK(r.prod.latched_scene()->serial == 1);   // the frame is dropped (its grid is missing)
    char line[b3::kLineLimit];
    CHECK(r.prod.take_latch_line(line, sizeof line) && std::strstr(line, "incomplete=1;") && std::strstr(line, ";refused=1;") && std::strstr(line, "geom-refused=1;"));
    char sum[1024];
    CHECK(r.prod.format_summary(sum, sizeof sum) > 0 && std::strstr(sum, "last-why=b3l-read-rate;"));   // failing before: brief3d-icon-camera-unmatched
}

// The requested proof: the head pose changes between game updates (every XR frame; the game updates every 8th) and the world-space table
// vertices do not move.
void test_world_locked_across_head_motion() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    xr_owner::FrameInfo info0 = frame_info(head0, ypr(0, -0.3, 0));
    r.game_frame(0.0, info0);
    std::shared_ptr<const lt::Scene> s0 = r.prod.latched_scene();
    CHECK(s0 && s0->serial == 1 && s0->draws.size() == 1 && s0->records == 1 && s0->placement.valid);
    // the LOCAL positions every grid vertex must keep (the fit and placement latched at entry)
    const b3::Mat4 m_tl = b3::mul(s0->fit.model_to_table, b3::table_to_local(s0->placement));
    std::vector<unsigned> sample;
    for (unsigned k = 0; k < kN * kN; k += 97) sample.push_back(k);
    std::vector<std::array<double, 3>> want(sample.size());
    for (size_t i = 0; i < sample.size(); ++i) {
        float p[3]; std::memcpy(p, r.vb->data() + size_t(sample[i]) * 16, 12);
        const double q[3] = {p[0], p[1], p[2]}; double h4[4]; b3::xform(m_tl, q, h4);
        want[i] = {h4[0], h4[1], h4[2]};
    }
    double max_err = 0; unsigned decoded = 0, frames = 0, layers = 0;
    double orbit = 0.0;
    for (unsigned n = 0; n < 96; ++n) {
        r.g.t += 11;   // 90 Hz XR
        // the head walks and turns every frame: +-0.25 m sideways, +-0.1 m up/down, +-0.2 m forward/back, yaw +-40 deg, pitch -10..-50 deg, roll
        const double t = n * 0.011;
        const double head[3] = {0.25 * std::sin(t * 2.1), 1.2 + 0.1 * std::sin(t * 3.3), 0.2 * std::cos(t * 1.7) - 0.2};
        const Q q = ypr(0.7 * std::sin(t * 1.3), -0.52 - 0.35 * std::sin(t * 2.6), 0.15 * std::sin(t * 4.0));
        const xr_owner::FrameInfo info = frame_info(head, q);
        if (n % 8 == 0) { orbit += 0.17; r.game_frame(orbit, info); }   // the game updates its map (camera orbiting) only every 8th XR frame
        xr_owner::LayerPlan plan;
        const bool ok = r.xr_frame(info, plan);
        ++frames;
        CHECK(ok && plan.layer.table && !plan.layer.table_opaque);
        ++layers;
        r.gpu->completed_v = r.gpu->value;   // the GPU keeps up
        const lt::FramePlan& fp = r.gpu->plans.back();
        for (unsigned e = 0; e < 2; ++e) {
            // the layer is submitted with exactly THIS frame's views, and the scene was drawn with them
            CHECK(std::memcmp(&plan.layer.views[e], &info.views.eyes[e], sizeof info.views.eyes[e]) == 0);
            const b3::EyeView ev = eye_view(info.views.eyes[e]);
            for (int k = 0; k < 3; ++k) CHECK(fp.eyes[e].position[k] == ev.position[k]);
            for (int k = 0; k < 4; ++k) CHECK(fp.eyes[e].orientation[k] == ev.orientation[k] && fp.eyes[e].fov[k] == ev.fov[k]);
            const b3::Mat4 rec = b3::load(fp.eye[e].rec.data());   // the float32 record the GPU reads
            const float b10[2] = {682.0f, -448.0f};
            for (size_t i = 0; i < sample.size(); ++i) {
                float p[3]; std::memcpy(p, r.vb->data() + size_t(sample[i]) * 16, 12);
                const double q3[3] = {p[0], p[1], p[2]};
                double sv[3];
                if (!b3::emulate_selmap(rec, b10, q3, sv) || !(sv[2] > 0) || sv[2] > 1) continue;   // behind / closer than the near plane: not drawn
                double local[3];
                CHECK(lt::unproject_reverse(ev, lt::kNearM, sv, local));
                for (int k = 0; k < 3; ++k) max_err = std::fmax(max_err, std::fabs(local[k] - want[i][k]));
                ++decoded;
            }
        }
    }
    std::printf("world-lock: %u frames, %u layers, %u vertex decodes, max LOCAL error %.3g m, scenes %u\n", frames, layers, decoded, max_err,
                unsigned(r.core->stats().scenes));
    CHECK(decoded > 20000);
    CHECK(max_err < 5e-4);                          // every vertex at its latched LOCAL point (float32 records): the table does not move
    CHECK(r.gpu->renders == frames && r.core->stats().layers == frames);   // one render per XR frame, not per game update
    CHECK(r.core->stats().scenes == 12);            // the 12 game updates of the loop were shown, each re-drawn 8 times
    CHECK(r.xr.violations == 0 && r.gpu->unwaited == 0);
    // every game update kept the same fit and placement (the game camera's orbit does not move the table)
    const std::shared_ptr<const lt::Scene> sN = r.prod.latched_scene();
    CHECK(sN->serial == 13);
    for (int i = 0; i < 16; ++i) CHECK(sN->fit.model_to_table.m[i] == s0->fit.model_to_table.m[i]);
    CHECK(sN->placement.centre[0] == s0->placement.centre[0] && sN->placement.centre[1] == s0->placement.centre[1] &&
          sN->placement.centre[2] == s0->placement.centre[2] && sN->placement.yaw == s0->placement.yaw);
    // why the old path shook: a record rendered for pose A shown at pose B (a 4 cm head translation, same orientation) misplaces the vertices
    const double ha[3] = {0, 1.2, 0}, hb[3] = {0.04, 1.2, 0};
    const xr_owner::FrameInfo ia = frame_info(ha, ypr(0, -0.4, 0)), ib = frame_info(hb, ypr(0, -0.4, 0));
    lt::FramePlan pa;
    const b3::EyeView ea[2] = {eye_view(ia.views.eyes[0]), eye_view(ia.views.eyes[1])};
    CHECK(lt::plan_frame(sN, ea, pa) == nullptr);
    const b3::Mat4 reca = b3::load(pa.eye[0].rec.data());
    const b3::EyeView eb = eye_view(ib.views.eyes[0]);
    double moved = 0; const float b10[2] = {682.0f, -448.0f};
    for (size_t i = 0; i < sample.size(); ++i) {
        float p[3]; std::memcpy(p, r.vb->data() + size_t(sample[i]) * 16, 12);
        const double q3[3] = {p[0], p[1], p[2]}; double sv[3], local[3];
        if (!b3::emulate_selmap(reca, b10, q3, sv) || !(sv[2] > 0) || sv[2] > 1) continue;
        if (lt::unproject_reverse(eb, lt::kNearM, sv, local)) moved = std::fmax(moved, std::fabs(local[0] - want[i][0]));
    }
    CHECK(moved > 0.03);   // a stale image (rotation-only reprojection) carries the head translation into the table: the old swim
}

// Leaning into the table: no frame refusal (the old eye guard refused the whole frame), only the segments that reach the eye plane are culled.
void test_lean_in_no_refusal() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo entry = frame_info(head0, ypr(0, -0.3, 0));
    r.g.t += 11; r.game_frame(0.0, entry);
    const std::shared_ptr<const lt::Scene> s = r.prod.latched_scene();
    CHECK(s);
    // the head 0.10 m above the table top, over its centre, looking steeply down: inside the old guard volume
    const double head[3] = {s->placement.centre[0], s->placement.centre[1] + 0.10, s->placement.centre[2] + 0.05};
    const xr_owner::FrameInfo info = frame_info(head, ypr(0, -1.2, 0));
    const b3::EyeView ev[2] = {eye_view(info.views.eyes[0]), eye_view(info.views.eyes[1])};
    {   // the old path's guard for this pose
        b3::FrameState fs; fs.begin(); b3::EyeCfg cfg;
        const b3::Box cb = b3::content_box(r.box, s->base_fit, 0);
        CHECK(std::string(b3::open_eyes(fs, s->fit, cb, s->placement, ev, s->b10, cfg)) == "brief3d-eye-inside-volume");
    }
    r.g.t += 11;
    xr_owner::LayerPlan plan;
    CHECK(r.xr_frame(info, plan) && plan.layer.table);   // drawn, not refused
    const lt::FramePlan& fp = r.gpu->plans.back();
    CHECK(fp.clipped > 0);
    for (unsigned e = 0; e < 2; ++e) {
        const lt::EyePlan& ep = fp.eye[e];
        CHECK(ep.mode[0] == lt::kModeCulled && ep.mask[0] == 1);
        // brute force: the segments with both ends at w >= near, in index order
        std::vector<uint16_t> keep;
        const uint16_t* ix = reinterpret_cast<const uint16_t*>(r.ib->data());
        auto w = [&](uint16_t v) { float p[3]; std::memcpy(p, r.vb->data() + size_t(v) * 16, 12); return lt::w_of(ep.m, p[0], p[1], p[2]); };
        for (size_t k = 0; k + 1 < r.ib->size() / 2; k += 2) if (w(ix[k]) >= lt::kNearM && w(ix[k + 1]) >= lt::kNearM) { keep.push_back(ix[k]); keep.push_back(ix[k + 1]); }
        CHECK(!keep.empty() && keep.size() < r.ib->size() / 2);
        CHECK(ep.cull_count[0] == keep.size() && ep.cull.size() == keep.size() * 2);
        CHECK(std::memcmp(ep.cull.data() + ep.cull_off[0], keep.data(), keep.size() * 2) == 0);
        // the kept vertices are still at their latched LOCAL points
        const b3::Mat4 m_tl = b3::mul(s->fit.model_to_table, b3::table_to_local(s->placement));
        const b3::Mat4 rec = b3::load(ep.rec.data());
        const float b10[2] = {682.0f, -448.0f};
        double max_err = 0;
        for (size_t k = 0; k < keep.size(); k += 37) {
            float p[3]; std::memcpy(p, r.vb->data() + size_t(keep[k]) * 16, 12);
            const double q3[3] = {p[0], p[1], p[2]}; double sv[3], local[3], h4[4];
            CHECK(b3::emulate_selmap(rec, b10, q3, sv));
            CHECK(lt::unproject_reverse(ev[e], lt::kNearM, sv, local));
            b3::xform(m_tl, q3, h4);
            for (int c = 0; c < 3; ++c) max_err = std::fmax(max_err, std::fabs(local[c] - h4[c]));
        }
        CHECK(max_err < 5e-4);
    }
    // leaning back out: the whole range again
    const xr_owner::FrameInfo out = frame_info(head0, ypr(0, -0.3, 0));
    r.gpu->completed_v = r.gpu->value;
    r.g.t += 11;
    CHECK(r.xr_frame(out, plan) && r.gpu->plans.back().eye[0].mode[0] == lt::kModeFull && r.gpu->plans.back().clipped == 0);
    // the producer never refused anything in latch mode for the head pose
    CHECK(r.prod.stats().refused == 0);
}

// A game frame with a refused map draw never replaces the shown scene with an incomplete one; icons are latched with their copies.
void test_incomplete_frames_and_icons() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
    r.g.t += 16; r.game_frame(0.0, info);
    CHECK(r.prod.latched_scene()->serial == 1);
    // next game frame: the grid, then an icon whose geometry cannot be read -> nothing published
    uint8_t grid[64]; game_matrix(0.1, grid);
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, grid), pose_in(info), nullptr, nullptr, grid_geom(r.h, r.vb, r.ib), nullptr);
    uint8_t icon[272] = {};
    std::memcpy(icon, grid, 64);
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(icon + 64, id, 64); std::memcpy(icon + 128, id, 64);
    const float m3[16] = {1.5f, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 1.5f, 0, 5.0f, 3.2f, -4.0f, 1};   // a unit 3.2 units above the fit origin (0.05 m)
    std::memcpy(icon + 192, m3, 64);
    r.prod.latch_draw(facts(b3::Kind::modelicon, r.g.t, icon), pose_in(info), nullptr, nullptr, lt::Geom{}, "step3-not-upload-buffer");
    r.prod.on_present(r.g.t);
    CHECK(r.prod.latched_scene()->serial == 1);   // kept
    char line[b3::kLineLimit];
    CHECK(r.prod.take_latch_line(line, sizeof line) && std::strstr(line, "incomplete=1") && std::strstr(line, "why=step3-not-upload-buffer"));
    // a complete frame with the icon (strip with a restart index, 6 vertices of 24 B)
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, grid), pose_in(info), nullptr, nullptr, grid_geom(r.h, r.vb, r.ib), nullptr);
    lt::Geom ig;
    {
        const uint16_t ix[7] = {0, 1, 2, 0xFFFF, 3, 4, 5};
        auto ib = std::make_shared<std::vector<uint8_t>>(14); std::memcpy(ib->data(), ix, 14);
        auto vb = std::make_shared<std::vector<uint8_t>>(6 * 24, uint8_t(0));
        for (unsigned v = 0; v < 6; ++v) { const float p[3] = {float(v % 2) - 0.5f, 0.3f, float(v / 2) * 0.5f - 0.5f}; std::memcpy(vb->data() + v * 24, p, 12); }
        ig.ib = ib; ig.vb = vb; ig.index_bytes = 2; ig.index_count = 7; ig.stride = 24; ig.ia_topology = 5; ig.strip_cut = 1; ig.pso = r.h.pso; ig.root = r.h.root;
    }
    r.prod.latch_draw(facts(b3::Kind::modelicon, r.g.t, icon), pose_in(info), nullptr, nullptr, ig, nullptr);
    r.prod.on_present(r.g.t);
    const std::shared_ptr<const lt::Scene> s = r.prod.latched_scene();
    CHECK(s->serial == 2 && s->draws.size() == 2 && s->selmap == 1 && s->icons == 1 && s->records == 1 + s->draws[1].copies);
    CHECK(s->draws[1].radius > 0.5 && s->draws[1].radius < 1.0);
    CHECK(s->draws[0].t11_off % 256 == 0 && s->draws[1].ib_off % 256 == 0 && s->geom_bytes <= lt::kGeomCap);
    // the consumer: the icon record = the game's record with matrix 0 = this frame's eye matrix and matrix 3 = the copy's
    r.g.t += 11;
    xr_owner::LayerPlan plan;
    CHECK(r.xr_frame(info, plan));
    const lt::FramePlan& fp = r.gpu->plans.back();
    for (unsigned e = 0; e < 2; ++e) {
        const uint8_t* rec = fp.eye[e].rec.data() + lt::kRecStride;
        uint8_t m0[64]; CHECK(b3::store(fp.eye[e].m, m0)); CHECK(std::memcmp(rec, m0, 64) == 0);
        CHECK(std::memcmp(rec + 64, icon + 64, 128) == 0);
        CHECK(std::memcmp(rec + 192, s->draws[1].m3[0], 64) == 0);
        CHECK(fp.eye[e].mask[1] == 1);
    }
    // the head at the icon: that copy is culled for this frame only, the grid stays
    const b3::Mat4 m_tl = b3::mul(s->fit.model_to_table, b3::table_to_local(s->placement));
    const double ip[3] = {s->draws[1].m3[0][12], s->draws[1].m3[0][13], s->draws[1].m3[0][14]}; double h4[4]; b3::xform(m_tl, ip, h4);
    const double at[3] = {h4[0], h4[1] + 0.01, h4[2] + 0.01};
    r.g.t += 11;
    CHECK(r.xr_frame(frame_info(at, ypr(0, -0.5, 0)), plan) && plan.layer.table);
    CHECK(r.gpu->plans.back().culled_copies >= 1 && r.gpu->plans.back().eye[0].mask[1] == 0);
}

// Review 2026-10-31 F5: our list binds only IA slot 0. A map draw with another vertex buffer slot bound used to be latched (counted only) and drawn
// with those attributes reading 0. Now the draw is refused and the frame keeps the previous scene.
void test_extra_vb_refused() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
    r.g.t += 16; r.game_frame(0.0, info);
    CHECK(r.prod.latched_scene() && r.prod.latched_scene()->serial == 1);
    uint8_t grid[64]; game_matrix(0.1, grid);
    r.g.t += 16;
    lt::Geom g = grid_geom(r.h, r.vb, r.ib); g.extra_vb = 1;   // slot 1 bound as well
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, grid), pose_in(info), nullptr, nullptr, std::move(g), nullptr);
    r.prod.on_present(r.g.t);
    CHECK(r.prod.latched_scene()->serial == 1);   // not replaced by a scene with that draw
    char line[b3::kLineLimit];
    CHECK(r.prod.take_latch_line(line, sizeof line) && std::strstr(line, "extra-vb=1;") && std::strstr(line, "why=b3l-extra-vb"));
    // the same draw without the extra slot is latched again
    r.g.t += 16; r.game_frame(0.2, info);
    CHECK(r.prod.latched_scene()->serial == 2);
}

// The consumer keeps each allocator's scene (the game's PSO / root references) until its fence value completed; stale / other-generation
// scenes are never drawn.
void test_lifetime_stale_generation() {
    Rig r;
    std::weak_ptr<int> pso_alive = r.h.pso;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
    r.g.t += 16; r.game_frame(0.0, info);
    std::weak_ptr<const lt::Scene> first = r.prod.latched_scene();
    xr_owner::LayerPlan plan;
    r.g.t += 11; CHECK(r.xr_frame(info, plan));                          // allocator 0 holds scene 1
    r.g.t += 16; r.game_frame(0.1, info);                                // the producer replaced it
    CHECK(r.core->inflight() == 1);
    r.g.t += 11; CHECK(r.xr_frame(info, plan));                          // allocator 1 (never used): scene 2
    CHECK(r.core->inflight() == 2);
    r.g.t += 11; CHECK(r.xr_frame(info, plan));                          // (F3) allocator 2 (never used): scene 2 again
    CHECK(r.core->inflight() == 3 && r.gpu->renders == 3);
    // allocator 0: its value has not completed: (F3) the last render's layer is resubmitted, nothing rendered
    r.g.t += 11; CHECK(r.xr_frame(info, plan) && r.core->stats().alloc_busy == 1 && r.core->stats().resubmits == 1 && r.gpu->renders == 3);
    r.gpu->plans.clear();                                                // (the fake keeps copies of the plans)
    CHECK(!first.expired());                                             // the GPU has not finished with scene 1: still referenced
    r.gpu->completed_v = r.gpu->value;
    r.g.t += 11; CHECK(r.xr_frame(info, plan));
    r.gpu->plans.clear();
    CHECK(first.expired());                                              // released once its value completed
    // stale: no new scene for > 500 ms
    r.g.t += 600; r.gpu->completed_v = r.gpu->value;
    const unsigned before = r.gpu->renders;
    CHECK(!r.xr_frame(info, plan) && r.gpu->renders == before && r.core->stats().stale >= 1);
    // another LOCAL generation (recentre): refused until the producer re-places the table in it
    r.g.t += 16; r.game_frame(0.2, info);
    const xr_owner::FrameInfo moved = frame_info(head0, ypr(0, -0.3, 0), 5);
    r.g.t += 11; CHECK(!r.xr_frame(moved, plan) && r.core->stats().generation_refused == 1);
    r.g.t += 16; r.game_frame(0.3, moved);
    CHECK(r.prod.latched_scene()->space_generation == encode_space_generation(5));
    r.gpu->completed_v = r.gpu->value;
    r.g.t += 11; CHECK(r.xr_frame(moved, plan));
    // retire: waits for the last value, drops every scene, destroys
    r.gpu->completed_v = r.gpu->value;
    CHECK(r.core->retire(r.g) && r.gpu->destroys == 1 && r.core->inflight() == 0);
    CHECK(!pso_alive.expired());   // still referenced by the producer's current scene (and the rig)
}

// Review 2026-10-31 F3: at 90 Hz with the game's DIRECT queue several XR frames behind, the allocator of the oldest in-flight render is busy.
// Stage 1 (2 allocators) submitted NO table layer for such a frame: the table blinked. Now: 3 allocators, and a busy frame resubmits the last
// render's layer with that render's views, as long as the eye swapchains still hold it.
void test_busy_resubmits() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo a = frame_info(head0, ypr(0.00, -0.3, 0)), b = frame_info(head0, ypr(0.05, -0.3, 0)), c = frame_info(head0, ypr(0.10, -0.3, 0));
    r.g.t += 16; r.game_frame(0.0, a);
    xr_owner::LayerPlan plan;
    // the GPU never completes anything in this stretch (completed_v stays 0): three renders still go out (allocators 0, 1, 2)
    r.g.t += 11; CHECK(r.xr_frame(a, plan) && plan.layer.table);
    r.g.t += 11; CHECK(r.xr_frame(b, plan) && plan.layer.table);
    r.g.t += 11; plan = xr_owner::LayerPlan{}; CHECK(r.xr_frame(c, plan) && plan.layer.table);   // stage 1: alloc-busy here, no layer
    CHECK(r.gpu->renders == 3 && r.core->stats().alloc_busy == 0);
    // the fourth: allocator 0 busy -> the layer of render 3, with render 3's views (c), not this frame's; nothing acquired or released
    const xr_owner::FrameInfo d = frame_info(head0, ypr(0.15, -0.3, 0));
    const unsigned viol = r.xr.violations;
    r.g.t += 11; plan = xr_owner::LayerPlan{};
    CHECK(r.xr_frame(d, plan) && plan.layer.table && !plan.layer.table_opaque);
    CHECK(r.gpu->renders == 3 && r.core->stats().alloc_busy == 1 && r.core->stats().resubmits == 1 && r.core->stats().layers == 4);
    CHECK(!r.xr.acquired[0] && !r.xr.acquired[1] && r.xr.violations == viol && !r.core->touched());
    for (unsigned e = 0; e < 2; ++e) {
        CHECK(plan.layer.views[e].pose.orientation[1] == c.views.eyes[e].pose.orientation[1]);
        CHECK(plan.layer.views[e].pose.orientation[1] != d.views.eyes[e].pose.orientation[1]);
    }
    // still busy for several frames: resubmitted every frame (no blink)
    for (int i = 0; i < 5; ++i) { r.g.t += 11; plan = xr_owner::LayerPlan{}; CHECK(r.xr_frame(d, plan) && plan.layer.table); }
    CHECK(r.core->stats().resubmits == 6 && r.core->stats().alloc_busy == 6);
    // the GPU catches up: a real render again, with this frame's views
    r.gpu->completed_v = r.gpu->value;
    r.g.t += 11; plan = xr_owner::LayerPlan{};
    CHECK(r.xr_frame(d, plan) && r.gpu->renders == 4 && plan.layer.views[0].pose.orientation[1] == d.views.eyes[0].pose.orientation[1]);
    // the summary line carries alloc-busy and resubmits
    char lb[b3::kLineLimit]; LatchCore::format_line(lb, sizeof lb, false, r.g.t, r.core->stats(), false, false, nullptr, nullptr);
    CHECK(std::strstr(lb, ";alloc-busy=6;resubmits=6;") != nullptr);
    // never resubmitted once another writer released into the eye swapchains (Output's flight composite / the slot path's blit)
    {
        Rig q; q.g.t += 16; q.game_frame(0.0, a);
        for (int i = 0; i < 3; ++i) { q.g.t += 11; CHECK(q.xr_frame(a, plan)); }
        q.core->target_written(1);
        q.g.t += 11; plan = xr_owner::LayerPlan{};
        CHECK(!q.xr_frame(a, plan) && !plan.layer.table && q.core->stats().alloc_busy == 1 && q.core->stats().resubmits == 0);
    }
    // ... nor in another LOCAL generation, nor once its scene's publication is stale
    {
        Rig q; q.g.t += 16; q.game_frame(0.0, a);
        for (int i = 0; i < 3; ++i) { q.g.t += 11; CHECK(q.xr_frame(a, plan)); }
        q.g.t += 16; q.game_frame(0.0, frame_info(head0, ypr(0, -0.3, 0), 4));   // the producer re-placed the table in generation 4
        q.g.t += 11; plan = xr_owner::LayerPlan{};
        CHECK(!q.xr_frame(frame_info(head0, ypr(0, -0.3, 0), 4), plan) && !plan.layer.table && q.core->stats().resubmits == 0 && q.core->stats().alloc_busy == 1);
    }
    {
        Rig q; q.g.t += 16; q.game_frame(0.0, a);
        for (int i = 0; i < 3; ++i) { q.g.t += 11; CHECK(q.xr_frame(a, plan)); }
        q.g.t += 450; q.game_frame(0.0, a);        // a newer scene keeps the core going, but the shown render's scene is 480+ ms old
        q.g.t += 60; plan = xr_owner::LayerPlan{};
        CHECK(!q.xr_frame(a, plan) && !plan.layer.table && q.core->stats().resubmits == 0);
    }
    // after an invalidation, the next real render makes the layer resubmittable again
    {
        Rig q; q.g.t += 16; q.game_frame(0.0, a);
        for (int i = 0; i < 3; ++i) { q.g.t += 11; CHECK(q.xr_frame(a, plan)); }
        q.core->target_written(0);   // (as a slot blit through Brief3dHook would do)
        q.gpu->completed_v = q.gpu->value; q.g.t += 11; CHECK(q.xr_frame(a, plan));   // a render again: resubmittable again
        q.g.t += 11; CHECK(q.xr_frame(a, plan)); q.g.t += 11; CHECK(q.xr_frame(a, plan));
        q.g.t += 11; plan = xr_owner::LayerPlan{}; CHECK(q.xr_frame(a, plan) && plan.layer.table && q.core->stats().resubmits == 1);
    }
}

// A failed Signal quarantines the scene (never released) and poisons; the hook reports it.
void test_signal_failure() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
    r.g.t += 16; r.game_frame(0.0, info);
    r.gpu->signal_fails = true;
    xr_owner::LayerPlan plan;
    r.g.t += 11; CHECK(!r.xr_frame(info, plan) && r.core->poisoned() && r.core->quarantined());
    CHECK(!r.core->retire(r.g));
    const std::string l = r.core->summary();
    CHECK(l.find("brief3d-latch-result") != std::string::npos && l.find("poisoned=1") != std::string::npos);
}

// Unreadable geometry from the start (e.g. the grid IB is not a mapped UPLOAD buffer in the live game): after kLatchGiveUpFrames map frames
// without any scene the producer falls back to the old replay path (latch_mode() false) instead of leaving the briefing without a table. A
// session that already latched a scene never falls back on a later read failure (the previous scene stays).
void test_fallback() {
    {
        Rig r;
        const double head0[3] = {0.0, 1.2, 0.0};
        const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
        for (unsigned n = 0; n < 119; ++n) { r.g.t += 16; r.game_frame(0.0, info, "not-upload-buffer"); }
        CHECK(r.prod.latch_mode() && !r.prod.latch_fell_back());
        r.g.t += 16; r.game_frame(0.0, info, "not-upload-buffer");
        CHECK(!r.prod.latch_mode() && r.prod.latch_fell_back() && !r.prod.latched_scene());
        char line[b3::kLineLimit];
        CHECK(r.prod.take_latch_line(line, sizeof line) && std::strstr(line, "fallback=1;fallback-why=not-upload-buffer"));
    }
    {
        Rig r;
        const double head0[3] = {0.0, 1.2, 0.0};
        const xr_owner::FrameInfo info = frame_info(head0, ypr(0, -0.3, 0));
        r.g.t += 16; r.game_frame(0.0, info);
        for (unsigned n = 0; n < 300; ++n) { r.g.t += 16; r.game_frame(0.0, info, "b3l-read-rate"); }
        CHECK(r.prod.latch_mode() && r.prod.latched_scene()->serial == 1);
    }
}

void test_line_limits() {
    LatchCore::Stats s;
    uint64_t* f = &s.renders;
    for (size_t i = 0; i < sizeof s / sizeof(uint64_t); ++i) f[i] = ~0ull;
    char buf[1024];
    const int n = LatchCore::format_line(buf, sizeof buf, true, INT64_MAX, s, true, true, "abcdefghijklmnopqrstuvwxyz0123456789", "abcdefghijklmnopqrstuvwxyz0123456789");
    CHECK(n > 0 && size_t(n) < b3::kLineLimit);
    lt::Stats ls; ls.draws = ls.scenes = ls.incomplete = ls.refused = ls.geom_refused = ls.extra_vb = ~0ull;
    lt::ReadStats rs; rs.full = rs.hits = rs.bytes = rs.rate = rs.torn = rs.faults = rs.old = rs.forced = rs.miss_fp = rs.miss_age = rs.miss_new = ~0ull;
    const int m = lt::format_line(buf, sizeof buf, ls, rs, ~0ull, ~0ull, 4000000000u, "abcdefghijklmnopqrstuvwxyz0123456789abcdef",
                                  true, "abcdefghijklmnopqrstuvwxyz0123456789abcdef");
    CHECK(m > 0 && size_t(m) + b3::kLinePrefix < b3::kLineLimit);
    CHECK(std::strstr(buf, ";old=9999999;forced=9999999;miss=9999999/9999999/9999999") != nullptr);
}

} // namespace

// ---- headset test 7 ("Briefing render doesnt fully render now at all") / null run step164 runAGU1, dump t66335-eye0-table (2400 x 1350,
// eye FOV +-45 deg both axes, head at the LOCAL origin, identity): the table showed as a 1 m wide, 13-pixel strip at rows 917..929 (65 vertical
// lines, 4 horizontal ones). The live grid: flat (box -32,0,-32..32,0,32, up=1+, a2=2+, m-per-unit 0.01562).
// A flat live-shaped grid (128 x 128 vertices) whose colour index runs ROW-MAJOR over a 64 x 64 ColorMap starting at the FAR edge of the
// table (index = (j / 2) * 64 + i / 2, max 4095) [I: the layout that reproduces the dump; the live VB's w values were never dumped].
std::shared_ptr<std::vector<uint8_t>> flat_grid_wide_colours(bool far_is_low_z) {
    auto v = std::make_shared<std::vector<uint8_t>>(size_t(kN) * kN * 16);
    for (unsigned j = 0; j < kN; ++j)
        for (unsigned i = 0; i < kN; ++i) {
            const unsigned row = far_is_low_z ? j : kN - 1 - j;   // colour rows counted from the table's far edge
            const float p[4] = {-32.0f + 64.0f * float(i) / float(kN - 1), 0.0f, -32.0f + 64.0f * float(j) / float(kN - 1), float((row / 2) * 64 + i / 2)};
            std::memcpy(v->data() + (size_t(j) * kN + i) * 16, p, 16);
        }
    return v;
}
// independent of brief3d_latch.h: the ColorMap entries the vertices index (DrawSelMap_v: ColorMap[int(in.w)])
uint64_t indexed_entries(const std::vector<uint8_t>& vb) {
    uint64_t n = 0;
    for (size_t o = 0; o + 16 <= vb.size(); o += 16) { float w; std::memcpy(&w, vb.data() + o + 12, 4); n = std::max<uint64_t>(n, uint64_t(w) + 1); }
    return n;
}
bool scene_colours_covered(const lt::Scene& s) {
    for (const lt::Draw& d : s.draws)
        if (d.kind == b3::Kind::selmap && (!d.g.t11 || d.g.t11->size() < indexed_entries(*d.g.vb) * 16)) return false;
    return true;
}
// the null runtime's views: both eyes at the head, identity, +-45 deg
xr_owner::FrameInfo null_runtime_views() {
    xr_owner::FrameInfo info;
    info.views_valid = true; info.views_tracked = true; info.views.view_count = 2;
    for (unsigned e = 0; e < 2; ++e) {
        auto& s = info.views.eyes[e];
        for (int k = 0; k < 3; ++k) s.pose.position[k] = 0.0f;
        s.pose.orientation[0] = s.pose.orientation[1] = s.pose.orientation[2] = 0.0f; s.pose.orientation[3] = 1.0f;
        s.fov.left = -0.7853982f; s.fov.right = 0.7853982f; s.fov.up = 0.7853982f; s.fov.down = -0.7853982f;
    }
    return info;
}
// NDC -> the row of the upright 2400 x 1350 dump
double dump_row(double ndc_y) { return (1.0 - ndc_y) * 0.5 * 1350.0; }

// The projected extent of the table from the null pose (a head 45 cm above the table top, 0.25..1.25 m ahead): the geometry is right. Its far
// edge lands on the dump's far edge (row 918 vs 917, 960 vs 963 px wide), the near edge is below the image (NDC y -1.8): a full table should
// fill rows 918..1350. Passes before and after the fix: the projection, the fit and the placement are NOT the cause.
void test_null_pose_table_extent() {
    Rig r;
    const xr_owner::FrameInfo info = null_runtime_views();
    for (int far_low = 0; far_low < 2; ++far_low) {
        lt::Bytes vb = flat_grid_wide_colours(far_low != 0);
        b3::Box box; CHECK(b3::box_from_vertices(vb->data(), vb->size(), 16, 0, kN * kN, box) == nullptr);
        CHECK(box.lo[1] == 0 && box.hi[1] == 0);
        b3::Fit fit; uint8_t rec[64]; game_matrix(0.0, rec);
        const float b10[2] = {682.0f, -448.0f};
        CHECK(b3::fit_model(box, b3::load(rec), b10, b3::Options{}, fit) == nullptr && fit.up == 1 && fit.a1 == 0 && fit.a2 == 2);
        CHECK(std::fabs(fit.metres_per_unit - 1.0 / 64.0) < 1e-12);
        const double head[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1};
        b3::Options o; o.on = true;
        const b3::Placement pl = b3::place(o, head, q, 0);
        CHECK(pl.valid && std::fabs(pl.centre[1] + 0.45) < 1e-6 && std::fabs(pl.centre[2] + 0.75) < 1e-6);
        b3::Mat4 m; b3::EyeCfg cfg;
        CHECK(b3::eye_matrix(fit, pl, eye_view(info.views.eyes[0]), b10, cfg, m) == nullptr);
        double far_y = 0, near_y = 0, far_x = 0;
        for (int c = 0; c < 4; ++c) {
            const double p[3] = {(c & 1) ? 32.0 : -32.0, 0.0, (c & 2) ? 32.0 : -32.0};
            double sv[3]; CHECK(b3::emulate_selmap(m, b10, p, sv));
            double t[4]; b3::xform(b3::mul(fit.model_to_table, b3::table_to_local(pl)), p, t);
            if (t[2] < -1.0) { far_y = sv[1]; far_x = std::max(far_x, std::fabs(sv[0])); } else near_y = sv[1];
        }
        CHECK(std::fabs(far_y + 0.36) < 1e-3 && std::fabs(far_x - 0.4) < 1e-3);   // 0.45 / 1.25 below, 0.5 / 1.25 to each side
        CHECK(std::fabs(near_y + 1.8) < 1e-3);                                  // 0.45 / 0.25: below the image (|y| > 1)
        CHECK(std::fabs(dump_row(far_y) - 918.0) < 0.5 && std::fabs(far_x * 2400.0 - 960.0) < 1.0);   // the dump: row 917, 963 px
    }
}

// The cause: the latched t11 copy held 256 ColorMap entries (the first 4 KiB) while the vertices index 4096. Reproduction: with that copy, the
// only vertices whose colour is inside it are the 8 vertex rows nearest the FAR edge; from the null pose they project to dump rows 918..931 (the
// dump: 917..929, four horizontal lines); every other vertex reads the zeroed upload memory after the copy (alpha 0). Failing before: the
// producer published that scene (scene_colours_covered false).
void test_t11_covers_colour_indices() {
    Rig r;
    const xr_owner::FrameInfo info = null_runtime_views();
    uint8_t rec[64]; game_matrix(0.0, rec);
    // which model z edge is far for this game matrix (the fit's a2 sign; live: a2=2+, model z -32 far)
    b3::Fit probe; { const float b10[2] = {682.0f, -448.0f}; CHECK(b3::fit_model(r.box, b3::load(rec), b10, b3::Options{}, probe) == nullptr); }
    lt::Bytes vb = flat_grid_wide_colours(probe.a2_sign > 0);
    CHECK(indexed_entries(*vb) == 4096);
    b3::Box box; CHECK(b3::box_from_vertices(vb->data(), vb->size(), 16, 0, kN * kN, box) == nullptr);
    // the reproduction of the dump with the old 4 KiB copy
    {
        const b3::Placement pl = [] { b3::Options o; o.on = true; const double h[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}; return b3::place(o, h, q, 0); }();
        b3::Fit fit; const float b10[2] = {682.0f, -448.0f};
        CHECK(b3::fit_model(box, b3::load(rec), b10, b3::Options{}, fit) == nullptr);
        b3::Mat4 m; b3::EyeCfg cfg; CHECK(b3::eye_matrix(fit, pl, eye_view(info.views.eyes[0]), b10, cfg, m) == nullptr);
        double lo = 1e9, hi = -1e9; unsigned visible = 0;
        for (size_t v = 0; v < size_t(kN) * kN; ++v) {
            float p4[4]; std::memcpy(p4, vb->data() + v * 16, 16);
            if (uint64_t(p4[3]) >= 256) continue;   // past the 4 KiB copy: alpha 0
            const double p[3] = {p4[0], p4[1], p4[2]}; double sv[3]; CHECK(b3::emulate_selmap(m, b10, p, sv));
            lo = std::min(lo, dump_row(sv[1])); hi = std::max(hi, dump_row(sv[1])); ++visible;
        }
        CHECK(visible == 8 * kN);
        CHECK(lo > 917.0 && lo < 919.0 && hi > 929.0 && hi < 932.0);   // the 13-pixel strip of the dump (rows 917..929)
    }
    // what the latch glue read before: the first 4 KiB of t11
    lt::Geom g = grid_geom(r.h, vb, r.ib);
    CHECK(g.t11->size() == 4096);
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, rec), pose_in(info), &box, nullptr, g, nullptr);
    r.prod.on_present(r.g.t);
    std::shared_ptr<const lt::Scene> s = r.prod.latched_scene();
    CHECK(!s || scene_colours_covered(*s));   // failing before: published with 256 of 4096 entries (15/16 of the grid invisible)
    // now: refused (the frame keeps the previous scene; a persistent shortfall reaches the b3l:0 fallback with the game's own t11)
    CHECK(!s);
    char line[b3::kLineLimit];
    CHECK(r.prod.take_latch_line(line, sizeof line) && std::strstr(line, "why=b3l-t11-short;") && std::strstr(line, "incomplete=1;"));
    // the glue's read size: every entry the latched vertices index, bounded by the resource
    uint64_t bad = 0;
    CHECK(lt::colour_entries(vb->data(), vb->size(), 16, &bad) == 4096 && bad == 0);
    CHECK(lt::t11_read_bytes(4096, 1ull << 20) == 65536);
    CHECK(lt::t11_read_bytes(4096, 4096) == 4096);   // the resource itself ends early: short -> refused by layout()
    CHECK(lt::t11_read_bytes(0, 4096) == 16 && lt::t11_read_bytes(5, 70) == 64 && lt::t11_read_bytes(5, 8) == 0);
    {
        auto odd = std::make_shared<std::vector<uint8_t>>(*vb);
        const float bads[3] = {-1.0f, std::nanf(""), 3.0e9f}, frac = 4200.9f;
        for (int k = 0; k < 3; ++k) std::memcpy(odd->data() + size_t(k) * 16 + 12, &bads[k], 4);
        std::memcpy(odd->data() + 3 * 16 + 12, &frac, 4);   // fptosi truncates: entry 4200
        CHECK(lt::colour_entries(odd->data(), odd->size(), 16, &bad) == 4201 && bad == 3);
        CHECK(lt::colour_entries(odd->data(), odd->size(), 12) == lt::kColourDefaultEntries);
        // (review 2026-11-01 F4) the memo walks the index list: the same counts for a grid IB that references every vertex; `bad` comes back with it
        lt::ColourMemo memo; const uint32_t ni = uint32_t(r.ib->size() / 2); uint64_t mb = 0;
        CHECK(memo.get(vb, r.ib, 16, 2, ni, false, 0) == 4096 && memo.get(odd, r.ib, 16, 2, ni, false, 0, &mb) == 4201 && mb == 8 &&
              memo.get(vb, r.ib, 16, 2, ni, false, 0, &mb) == 4096 && mb == 0 && memo.get(nullptr, r.ib, 16, 2, ni, false, 0) == lt::kColourDefaultEntries);
    }
    // the full copy: published, every vertex's colour inside it, and the whole grid drawn from the null pose (no segment culled)
    g.t11 = std::make_shared<std::vector<uint8_t>>(size_t(lt::t11_read_bytes(lt::colour_entries(vb->data(), vb->size(), 16), 1ull << 20)), uint8_t(0x40));
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, rec), pose_in(info), nullptr, nullptr, g, nullptr);
    r.prod.on_present(r.g.t);
    s = r.prod.latched_scene();
    CHECK(s && s->serial == 1 && scene_colours_covered(*s) && s->draws[0].colours == 4096);
    lt::FramePlan fp; b3::EyeView eyes[2] = {eye_view(info.views.eyes[0]), eye_view(info.views.eyes[1])};
    CHECK(lt::plan_frame(s, eyes, fp) == nullptr && fp.eye[0].mode[0] == lt::kModeFull && fp.clipped == 0);
    // an unchanged vb blob reuses the previous scene's count (no rescan) and stays covered
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, rec), pose_in(info), nullptr, nullptr, g, nullptr);
    r.prod.on_present(r.g.t);
    CHECK(r.prod.latched_scene()->serial == 2 && r.prod.latched_scene()->draws[0].colours == 4096);
}

// review 2026-11-01 F4: t11 coverage counts only the vertices the index list REFERENCES, and a referenced vertex with an unreadable colour index
// (w <= -1, NaN) is refused. Failing before: (1) a vertex inside the latched range [lo, hi] that no index draws, with a stale w of 100000, made the
// draw b3l-t11-short every frame although every drawn vertex is covered by the 256-entry copy; (2) a DRAWN vertex with w = -1 was published (fptosi
// -> a negative index: the GPU reads before our t11 copy through the unbounded root SRV).
void test_t11_coverage_referenced_vertices_only() {
    Rig r;
    const xr_owner::FrameInfo info = null_runtime_views();
    uint8_t rec[64]; game_matrix(0.0, rec);
    const uint32_t V = 64 * kN + 64;   // an interior vertex
    auto vb = std::make_shared<std::vector<uint8_t>>(*r.vb);
    const float stale = 100000.0f; std::memcpy(vb->data() + size_t(V) * 16 + 12, &stale, 4);
    // the grid IB without the 4 segments that touch V (V stays inside [lo, hi] of the latched range)
    std::vector<uint16_t> ix(r.ib->size() / 2), kept; std::memcpy(ix.data(), r.ib->data(), r.ib->size());
    for (size_t k = 0; k + 1 < ix.size(); k += 2) if (ix[k] != V && ix[k + 1] != V) { kept.push_back(ix[k]); kept.push_back(ix[k + 1]); }
    CHECK(kept.size() == ix.size() - 8);
    auto ib = std::make_shared<std::vector<uint8_t>>(kept.size() * 2); std::memcpy(ib->data(), kept.data(), ib->size());
    uint64_t bad = 9;
    CHECK(lt::colour_entries(vb->data(), vb->size(), 16) == 100001);   // the old count over the whole latched range
    CHECK(lt::colour_entries_indexed(vb->data(), vb->size(), 16, ib->data(), 2, uint32_t(kept.size()), false, 0, &bad) == 255 && bad == 0);   // w = (i + j) % 256 <= 254
    b3::Box box; CHECK(b3::box_from_vertices(vb->data(), vb->size(), 16, 0, kN * kN, box) == nullptr);
    lt::Geom g = grid_geom(r.h, vb, ib);   // the 4 KiB t11 copy: 256 entries, every DRAWN vertex's colour (w = (i + j) % 256)
    r.g.t += 16;
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, rec), pose_in(info), &box, nullptr, g, nullptr);
    r.prod.on_present(r.g.t);
    std::shared_ptr<const lt::Scene> s = r.prod.latched_scene();
    CHECK(s && s->serial == 1 && s->draws[0].colours == 255 && s->draws[0].colour_bad == 0);   // failing before: refused, b3l-t11-short
    // a DRAWN vertex with w = -1 (and one with NaN): refused, the frame keeps the previous scene
    for (const float w : {-1.0f, std::nanf("")}) {
        auto vb2 = std::make_shared<std::vector<uint8_t>>(*r.vb);
        std::memcpy(vb2->data() + size_t(5) * 16 + 12, &w, 4);
        CHECK(lt::colour_entries_indexed(vb2->data(), vb2->size(), 16, r.ib->data(), 2, uint32_t(r.ib->size() / 2), false, 0, &bad) == 255 && bad == 3);   // vertex 5 is drawn by 3 segments
        r.g.t += 16;
        r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, rec), pose_in(info), &r.box, nullptr, grid_geom(r.h, vb2, r.ib), nullptr);
        r.prod.on_present(r.g.t);
        CHECK(r.prod.latched_scene() && r.prod.latched_scene()->serial == 1);   // failing before: published (serial 2) with the negative index
        char line[b3::kLineLimit];
        CHECK(r.prod.take_latch_line(line, sizeof line) && std::strstr(line, "why=b3l-t11-index;"));
    }
    // an index past the latched vb, the rebased base vertex, a strip cut
    const uint16_t past[2] = {0, uint16_t(kN * kN)};
    CHECK(lt::colour_entries_indexed(r.vb->data(), r.vb->size(), 16, reinterpret_cast<const uint8_t*>(past), 2, 2, false, 0, &bad) == 1 && bad == 1);
    const uint16_t reb[2] = {10, 11};
    CHECK(lt::colour_entries_indexed(r.vb->data(), r.vb->size(), 16, reinterpret_cast<const uint8_t*>(reb), 2, 2, false, -10, &bad) == 2 && bad == 0);
    const uint16_t cut[3] = {1, 0xFFFF, 2};
    CHECK(lt::colour_entries_indexed(r.vb->data(), r.vb->size(), 16, reinterpret_cast<const uint8_t*>(cut), 2, 3, true, 0, &bad) == 3 && bad == 0);
    CHECK(lt::skips_cut(5, 2, 1) && !lt::skips_cut(2, 2, 1) && lt::skips_cut(13, 4, 2) && !lt::skips_cut(5, 4, 1));
}


// ---- Headset test 8 (2026-11-01): "the briefing 3D render isn't fixed properly in the world and moves when I move my head; the icons floated a
// little higher after a few seconds". docs/world-render/brief3d-worldlock.md.
// One game frame with an AIR icon: the grid draw, the icon's draw (the game's record: matrix 0 = this frame's SelMap matrix, matrix 3 = the model
// matrix; one triangle strip), the Present.
void game_frame_icon(Rig& r, double orbit, const xr_owner::FrameInfo& head, const float pos[3]) {
    uint8_t grid[64]; game_matrix(orbit, grid);
    r.prod.latch_draw(facts(b3::Kind::selmap, r.g.t, grid), pose_in(head), r.prod.wants_box(r.g.t) ? &r.box : nullptr, nullptr,
                      grid_geom(r.h, r.vb, r.ib), nullptr);
    uint8_t icon[272] = {};
    std::memcpy(icon, grid, 64);
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(icon + 64, id, 64); std::memcpy(icon + 128, id, 64);
    const float m3[16] = {1.5f, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 1.5f, 0, pos[0], pos[1], pos[2], 1};
    std::memcpy(icon + 192, m3, 64);
    lt::Geom ig;
    const uint16_t ix[7] = {0, 1, 2, 0xFFFF, 3, 4, 5};
    auto ib = std::make_shared<std::vector<uint8_t>>(14); std::memcpy(ib->data(), ix, 14);
    auto vb = std::make_shared<std::vector<uint8_t>>(6 * 24, uint8_t(0));
    for (unsigned v = 0; v < 6; ++v) { const float p[3] = {float(v % 2) - 0.5f, 0.3f, float(v / 2) * 0.5f - 0.5f}; std::memcpy(vb->data() + v * 24, p, 12); }
    ig.ib = ib; ig.vb = vb; ig.index_bytes = 2; ig.index_count = 7; ig.stride = 24; ig.ia_topology = 5; ig.strip_cut = 1; ig.pso = r.h.pso; ig.root = r.h.root;
    r.prod.latch_draw(facts(b3::Kind::modelicon, r.g.t, icon), pose_in(head), nullptr, nullptr, ig, nullptr);
    r.prod.on_present(r.g.t);
}
// The angle (rad) between where an eye `delta` away from the LOCATED eye sees the table's picture of the LOCAL point P (the quad plane at depth dq
// along the located eye's view axis, the picture drawn for the located eye) and where P really is.
double quad_parallax(const xr_owner::EyeSample& v, double dq, const double P[3], const double delta[3]) {
    const double q[4] = {v.pose.orientation[0], v.pose.orientation[1], v.pose.orientation[2], v.pose.orientation[3]};
    const double f0[3] = {0, 0, -1}; double f[3]; b3::quat_rotate(q, f0, f);
    double E[3], d[3], axial = 0;
    for (int k = 0; k < 3; ++k) { E[k] = v.pose.position[k]; d[k] = P[k] - E[k]; axial += d[k] * f[k]; }
    double t[3], u[3], nt = 0, nu = 0, dot = 0;
    for (int k = 0; k < 3; ++k) {
        const double Q = E[k] + d[k] * (dq / axial);
        t[k] = Q - (E[k] + delta[k]); u[k] = P[k] - (E[k] + delta[k]); nt += t[k] * t[k]; nu += u[k] * u[k]; dot += t[k] * u[k];
    }
    return std::acos(std::fmin(1.0, dot / std::sqrt(nt * nu)));
}
double axial_depth(const xr_owner::EyeSample& v, const double P[3]) {
    const double q[4] = {v.pose.orientation[0], v.pose.orientation[1], v.pose.orientation[2], v.pose.orientation[3]};
    const double f0[3] = {0, 0, -1}; double f[3]; b3::quat_rotate(q, f0, f);
    double a = 0; for (int k = 0; k < 3; ++k) a += (P[k] - v.pose.position[k]) * f[k];
    return a;
}
void test_worldlock_quad_depth_and_icons() {
    Rig r;
    const double head0[3] = {0.0, 1.2, 0.0};
    const float icon_pos[3] = {6.0f, 12.0f, -5.0f};   // 12 units = 0.19 m above the grid at 64 units per metre: an AIR icon
    r.g.t += 16; game_frame_icon(r, 0.0, frame_info(head0, ypr(0.3, -0.3, 0)), icon_pos);   // placement latched here (head yaw 0.3 rad)
    const std::shared_ptr<const lt::Scene> s0 = r.prod.latched_scene();
    CHECK(s0 && s0->serial == 1 && s0->icons == 1 && s0->placement.valid && r.prod.stats().placements == 1);
    const double* centre = s0->placement.centre;
    double orbit = 0.0, icon_h_min = 1e9, icon_h_max = -1e9, icon_err = 0, max_plan = 0, max_default = 0;
    unsigned measured = 0, frames = 0, decoded = 0;
    // 60 s of 90 Hz XR frames; the head walks +-0.3 m, turns +-40 deg, nods; the game's map updates at 45 Hz with its camera orbiting
    for (unsigned n = 0; n < 5400; ++n) {
        r.g.t += 11;
        const double t = n * 0.0111;
        const double head[3] = {0.30 * std::sin(t * 0.9), 1.2 + 0.1 * std::sin(t * 1.3), 0.25 * std::cos(t * 0.7) - 0.2};
        const xr_owner::FrameInfo info = frame_info(head, ypr(0.3 + 0.7 * std::sin(t * 0.6), -0.45 - 0.3 * std::sin(t * 1.1), 0.1 * std::sin(t * 2.0)));
        if (n % 2 == 0) { orbit += 0.03; game_frame_icon(r, orbit, info, icon_pos); }
        xr_owner::LayerPlan plan;
        CHECK(r.xr_frame(info, plan) && plan.layer.table);
        r.gpu->completed_v = r.gpu->value;
        const std::shared_ptr<const lt::Scene> sc = r.prod.latched_scene();
        // the head's motion never re-places the table (centre, yaw, generation) and the game's orbit never changes the fit
        for (int k = 0; k < 3; ++k) CHECK(sc->placement.centre[k] == centre[k]);
        CHECK(sc->placement.yaw == s0->placement.yaw && r.prod.stats().placements == 1 && sc->icons == 1);
        for (int i = 0; i < 16; ++i) CHECK(sc->fit.model_to_table.m[i] == s0->fit.model_to_table.m[i]);   // no content rescale: the icon height is fixed
        // the icon (copy 0): decoded from the float32 record the GPU reads and un-projected with an independent view model, it is at the same LOCAL
        // point in every frame and both eyes, and its height above the table top never changes
        const b3::Mat4 m_tl = b3::mul(sc->fit.model_to_table, b3::table_to_local(sc->placement));
        const double ip[3] = {sc->draws[1].m3[0][12], sc->draws[1].m3[0][13], sc->draws[1].m3[0][14]}; double want[4];
        b3::xform(m_tl, ip, want);
        const double h = want[1] - centre[1];
        icon_h_min = std::fmin(icon_h_min, h); icon_h_max = std::fmax(icon_h_max, h);
        const lt::FramePlan& fp = r.gpu->plans.back();
        const float b10[2] = {682.0f, -448.0f};
        for (unsigned e = 0; e < 2; ++e) {
            const b3::EyeView ev = eye_view(info.views.eyes[e]);
            const b3::Mat4 rec = b3::load(fp.eye[e].rec.data() + lt::kRecStride);
            double sv[3], local[3];
            if (fp.eye[e].mask[1] && b3::emulate_selmap(rec, b10, ip, sv) && sv[2] > 0 && sv[2] <= 1 && lt::unproject_reverse(ev, lt::kNearM, sv, local)) {
                for (int k = 0; k < 3; ++k) icon_err = std::fmax(icon_err, std::fabs(local[k] - want[k]));
                ++decoded;
            }
        }
        r.gpu->plans.clear();
        // the table's picture: the quad plane at the table's own depth per eye (this frame's located eye), so 1 cm of head-pose error does not move
        // the table; the 1.5 m default plane would move it by 5-20 mrad
        for (unsigned e = 0; e < 2; ++e) {
            const float d = plan.layer.table_quad_distance_m[e];
            CHECK(d == table_axial_depth(info.views.eyes[e], centre));
            const double ax = axial_depth(info.views.eyes[e], centre);
            if (ax < 0.45 || ax > 3.9) continue;   // leaning in / looking away: the plane is clamped, no claim
            CHECK(table_quad_distance(d) == d);
            const double dl[3] = {0.01, 0.0, 0.0};
            max_plan = std::fmax(max_plan, quad_parallax(info.views.eyes[e], double(table_quad_distance(d)), centre, dl));
            max_default = std::fmax(max_default, quad_parallax(info.views.eyes[e], double(kTableQuadDistanceM), centre, dl));
            ++measured;
        }
        ++frames;
    }
    std::printf("worldlock: %u XR frames, icon decodes %u, icon height %.6f..%.6f m, max icon LOCAL error %.3g m; quad parallax %u measures, plan %.3g rad vs 1.5 m plane %.3g rad\n",
                frames, decoded, icon_h_min, icon_h_max, icon_err, measured, max_plan, max_default);
    CHECK(decoded > 4000 && icon_err < 5e-4);          // the icon is rigidly attached to the table: the same LOCAL point whatever the head does
    CHECK(icon_h_max - icon_h_min < 1e-9);             // its height above the table top is the same in every frame of the 60 s
    CHECK(icon_h_max > 0.15 && icon_h_max < 0.25);     // and it is the game's 12 units at 64 units per metre
    CHECK(measured > 4000 && max_plan < 2e-4 && max_default > 4e-3);
    // the table_quad_distance clamp itself
    CHECK(table_quad_distance(0.0f) == kTableQuadDistanceM && table_quad_distance(-1.0f) == kTableQuadDistanceM && table_quad_distance(NAN) == kTableQuadDistanceM);
    CHECK(table_quad_distance(0.1f) == 0.4f && table_quad_distance(9.0f) == 4.0f && table_quad_distance(0.75f) == 0.75f);
    // behind / beside the eye (axial depth < 0.3 m): the straight distance, never 0 or negative
    xr_owner::EyeSample away = frame_info(head0, ypr(3.14159, 0, 0)).views.eyes[0];
    CHECK(table_axial_depth(away, centre) > 0.3f);
}

// ---- headset test 9 (b3p:1): the table quad on a WORLD-FIXED plane
double angle3(const double a[3], const double b[3]) {
    double d = 0, na = 0, nb = 0;
    for (int k = 0; k < 3; ++k) { d += a[k] * b[k]; na += a[k] * a[k]; nb += b[k] * b[k]; }
    return std::acos(std::fmax(-1.0, std::fmin(1.0, d / std::sqrt(na * nb))));
}
// Where the compositor shows the picture's NDC (x, y) on the submitted quad (LOCAL): the image fills the quad, NDC +1 = its right / top edge.
// Independent of plane_view: only the submitted pose / size.
void quad_point(const xr_owner::Pose& pose, float w, float h, const double sv[2], double Q[3]) {
    const double q[4] = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    const double local[3] = {sv[0] * 0.5 * w, sv[1] * 0.5 * h, 0.0};
    double r[3]; b3::quat_rotate(q, local, r);
    for (int k = 0; k < 3; ++k) Q[k] = pose.position[k] + r[k];
}
// The angle between the direction in which an eye at E sees the picture of LOCAL point P on the quad (picture = record `rec`) and P itself.
// false = P not on the picture (behind the eye plane / outside the rectangle).
bool fixed_error(const b3::Mat4& rec, const double model[3], const double P[3], const xr_owner::Pose& pose, float w, float h, const double E[3], double& err) {
    const float b10[2] = {682.0f, -448.0f};
    double sv[3];
    if (!b3::emulate_selmap(rec, b10, model, sv) || !(sv[2] > 0) || sv[2] > 1 || std::fabs(sv[0]) > 1 || std::fabs(sv[1]) > 1) return false;
    double Q[3]; quad_point(pose, w, h, sv, Q);
    const double a[3] = {Q[0] - E[0], Q[1] - E[1], Q[2] - E[2]}, b[3] = {P[0] - E[0], P[1] - E[1], P[2] - E[2]};
    err = angle3(a, b);
    return true;
}
bool same_pose(const xr_owner::Pose& a, const xr_owner::Pose& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// The table quad is posed ONCE (the placement's plane) and every grid vertex and the air icon, decoded from the float32 record the GPU reads and
// put on the submitted quad, lies on the ray from the located eye to its latched LOCAL point, in every frame and both eyes, while the head walks,
// turns and nods for 60 s. Head rotation alone does not change the picture; a picture one frame old (whatever SteamVR does to a quad between
// submission and display) is wrong by under 6 mrad in a 150 deg/s head turn where the per-eye quad of before is wrong by the turn itself.
void test_fixed_plane_world_locked() {
    Rig r(true);
    const double head0[3] = {0.0, 1.2, 0.0};
    const float icon_pos[3] = {6.0f, 12.0f, -5.0f};
    r.g.t += 16; game_frame_icon(r, 0.0, frame_info(head0, ypr(0.3, -0.3, 0)), icon_pos);
    const std::shared_ptr<const lt::Scene> s0 = r.prod.latched_scene();
    CHECK(s0 && s0->placement.valid && s0->plane.valid);
    const lt::TablePlane& pl = s0->plane;
    CHECK(pl.width > 0.5 && pl.width <= 2 * lt::kPlaneMaxHalfM + 1e-9 && pl.height > 0.5 && pl.height <= 2 * lt::kPlaneMaxHalfM + 1e-9);
    {   // it faces the seated head, its x axis is the table's horizontal x (no roll)
        const double toh[3] = {head0[0] - pl.centre[0], head0[1] - pl.centre[1], head0[2] - pl.centre[2]};
        CHECK(angle3(pl.axis[2], toh) < 0.15 && std::fabs(pl.axis[0][1]) < 1e-9);
    }
    const Quad q0 = plane_quad(pl);
    double max_err = 0;
    unsigned decoded = 0, frames = 0;
    std::vector<unsigned> sample;
    for (unsigned k = 0; k < kN * kN; k += 97) sample.push_back(k);
    double orbit = 0.0;
    for (unsigned n = 0; n < 5400; ++n) {
        r.g.t += 11;
        const double t = n * 0.0111;
        const double head[3] = {0.30 * std::sin(t * 0.9), 1.2 + 0.1 * std::sin(t * 1.3), 0.25 * std::cos(t * 0.7) - 0.2};
        const xr_owner::FrameInfo info = frame_info(head, ypr(0.3 + 0.7 * std::sin(t * 0.6), -0.45 - 0.3 * std::sin(t * 1.1), 0.1 * std::sin(t * 2.0)));
        if (n % 2 == 0) { orbit += 0.03; game_frame_icon(r, orbit, info, icon_pos); }
        xr_owner::LayerPlan plan;
        CHECK(r.xr_frame(info, plan) && plan.layer.table && plan.layer.table_front && plan.layer.table_quad_fixed);
        r.gpu->completed_v = r.gpu->value;
        // the quad never moves: the placement's plane in every frame
        CHECK(same_pose(plan.layer.table_quad_pose, q0.pose) && plan.layer.table_quad_width == q0.width && plan.layer.table_quad_height == q0.height);
        const std::shared_ptr<const lt::Scene> sc = r.prod.latched_scene();
        const b3::Mat4 m_tl = b3::mul(sc->fit.model_to_table, b3::table_to_local(sc->placement));
        const lt::FramePlan& fp = r.gpu->plans.back();
        for (unsigned e = 0; e < 2; ++e) {
            // the layer's view = the located eye's position with the plane's orientation (what the picture was drawn with)
            for (int k = 0; k < 3; ++k) CHECK(plan.layer.views[e].pose.position[k] == info.views.eyes[e].pose.position[k]);
            for (int k = 0; k < 4; ++k) CHECK(plan.layer.views[e].pose.orientation[k] == q0.pose.orientation[k]);
            const double E[3] = {info.views.eyes[e].pose.position[0], info.views.eyes[e].pose.position[1], info.views.eyes[e].pose.position[2]};
            const b3::Mat4 rec = b3::load(fp.eye[e].rec.data());
            for (unsigned k : sample) {
                float p[3]; std::memcpy(p, r.vb->data() + size_t(k) * 16, 12);
                const double mp[3] = {p[0], p[1], p[2]}; double P4[4]; b3::xform(m_tl, mp, P4);
                double err = 0;
                if (fixed_error(rec, mp, P4, q0.pose, q0.width, q0.height, E, err)) { max_err = std::fmax(max_err, err); ++decoded; }
            }
            if (fp.eye[e].mask[1]) {   // the air icon (copy 0, record 1)
                const b3::Mat4 irec = b3::load(fp.eye[e].rec.data() + lt::kRecStride);
                const double ip[3] = {sc->draws[1].m3[0][12], sc->draws[1].m3[0][13], sc->draws[1].m3[0][14]}; double P4[4]; b3::xform(m_tl, ip, P4);
                double err = 0;
                if (fixed_error(irec, ip, P4, q0.pose, q0.width, q0.height, E, err)) { max_err = std::fmax(max_err, err); ++decoded; }
            }
        }
        r.gpu->plans.clear();
        ++frames;
    }
    std::printf("fixed plane: quad %.3f x %.3f m, %u XR frames, %u decodes, max direction error %.3g rad, plane frames %llu, plane-off %llu\n",
                double(q0.width), double(q0.height), frames, decoded, max_err, (unsigned long long)r.core->stats().plane_frames,
                (unsigned long long)r.core->stats().plane_off);
    CHECK(decoded > 500000 && max_err < 2e-4);
    CHECK(r.core->stats().plane_frames == frames && r.core->stats().plane_off == 0);
    // head rotation with the eyes in place: the very same picture (only the eye positions enter it)
    {
        r.g.t += 11;
        xr_owner::FrameInfo a = frame_info(head0, ypr(0.3, -0.4, 0)), b = a;
        const double qb[4] = {0.10, 0.30, 0.05, 0.95}; const double nb = std::sqrt(0.01 + 0.09 + 0.0025 + 0.9025);
        for (unsigned e = 0; e < 2; ++e) for (int k = 0; k < 4; ++k) b.views.eyes[e].pose.orientation[k] = float(qb[k] / nb);
        xr_owner::LayerPlan pa, pb;
        CHECK(r.xr_frame(a, pa)); r.gpu->completed_v = r.gpu->value;
        r.g.t += 11; CHECK(r.xr_frame(b, pb)); r.gpu->completed_v = r.gpu->value;
        const size_t np = r.gpu->plans.size();
        CHECK(np >= 2);
        for (unsigned e = 0; e < 2; ++e) CHECK(r.gpu->plans[np - 1].eye[e].rec == r.gpu->plans[np - 2].eye[e].rec);
        CHECK(same_pose(pa.layer.table_quad_pose, pb.layer.table_quad_pose));
    }
    // a picture one XR frame old in a 150 deg/s yaw turn (neck pivot 10 cm behind / below the eyes, looking down at the table)
    {
        const std::shared_ptr<const lt::Scene> sc = r.prod.latched_scene();
        const b3::Mat4 m_tl = b3::mul(sc->fit.model_to_table, b3::table_to_local(sc->placement));
        auto head_at = [&](double yaw, double out[3]) {
            const Q qy = ypr(0.3 + yaw, 0, 0); const double off0[3] = {0, 0.1, -0.1}, qa[4] = {qy.x, qy.y, qy.z, qy.w};
            double off[3]; b3::quat_rotate(qa, off0, off);
            out[0] = off[0]; out[1] = 1.1 + off[1]; out[2] = 0.1 + off[2];
        };
        const double w = 150.0 * 3.141592653589793 / 180.0 / 90.0;
        double fixed_max = 0, eye_max = 0; unsigned n_err = 0;
        for (unsigned f = 1; f < 40; ++f) {
            const double ya = (f - 1) * w - 0.3, yb = f * w - 0.3;
            double ha[3], hb[3]; head_at(ya, ha); head_at(yb, hb);
            const xr_owner::FrameInfo ia = frame_info(ha, ypr(0.3 + ya, -0.5, 0)), ib = frame_info(hb, ypr(0.3 + yb, -0.5, 0));
            b3::EyeView va[2];
            CHECK(lt::plane_view(sc->plane, eye_view(ia.views.eyes[0]).position, va[0]) == nullptr);
            CHECK(lt::plane_view(sc->plane, eye_view(ia.views.eyes[1]).position, va[1]) == nullptr);
            lt::FramePlan fa;
            CHECK(lt::plan_frame(sc, va, fa) == nullptr);
            for (unsigned e = 0; e < 2; ++e) {
                const b3::Mat4 rec = b3::load(fa.eye[e].rec.data());
                const b3::EyeView ea = eye_view(ia.views.eyes[e]), eb = eye_view(ib.views.eyes[e]);
                for (unsigned k : sample) {
                    float p[3]; std::memcpy(p, r.vb->data() + size_t(k) * 16, 12);
                    const double mp[3] = {p[0], p[1], p[2]}; double P4[4]; b3::xform(m_tl, mp, P4);
                    double err = 0;
                    if (!fixed_error(rec, mp, P4, q0.pose, q0.width, q0.height, eb.position, err)) continue;
                    fixed_max = std::fmax(fixed_max, err); ++n_err;
                    // the per-eye quad of before: the direction drawn for eye pose a, shown with eye pose b's orientation
                    const double qi[4] = {-ea.orientation[0], -ea.orientation[1], -ea.orientation[2], ea.orientation[3]};
                    const double da[3] = {P4[0] - ea.position[0], P4[1] - ea.position[1], P4[2] - ea.position[2]};
                    double v[3], seen[3]; b3::quat_rotate(qi, da, v); b3::quat_rotate(eb.orientation, v, seen);
                    const double db[3] = {P4[0] - eb.position[0], P4[1] - eb.position[1], P4[2] - eb.position[2]};
                    eye_max = std::fmax(eye_max, angle3(seen, db));
                }
            }
        }
        std::printf("fixed plane: one-frame-old picture in a 150 deg/s turn: %u points, max error %.3g rad (per-eye quad %.3g rad)\n", n_err, fixed_max, eye_max);
        CHECK(n_err > 5000 && fixed_max < 6e-3 && eye_max > 2.5e-2);
    }
    // an eye at / behind the plane: that frame falls back to the per-eye quads (still drawn, counted plane-off)
    {
        r.g.t += 11;
        const double behind[3] = {pl.centre[0] - 0.3 * pl.axis[2][0], pl.centre[1] - 0.3 * pl.axis[2][1], pl.centre[2] - 0.3 * pl.axis[2][2]};
        xr_owner::LayerPlan plan;
        CHECK(r.xr_frame(frame_info(behind, ypr(0.3, -0.3, 0)), plan) && plan.layer.table && !plan.layer.table_quad_fixed);
        CHECK(r.core->stats().plane_off == 1);
        r.gpu->completed_v = r.gpu->value;
        r.g.t += 11; plan = xr_owner::LayerPlan{};
        CHECK(r.xr_frame(frame_info(head0, ypr(0.3, -0.3, 0)), plan) && plan.layer.table_quad_fixed);
        char lb[b3::kLineLimit]; LatchCore::format_line(lb, sizeof lb, false, r.g.t, r.core->stats(), false, false, nullptr, nullptr);
        CHECK(std::strstr(lb, ";plane-off=1;") != nullptr);
    }
    // a busy frame resubmits the last render WITH its fixed quad
    {
        Rig q(true);
        q.g.t += 16; q.game_frame(0.0, frame_info(head0, ypr(0.3, -0.3, 0)));
        xr_owner::LayerPlan plan;
        for (int i = 0; i < 3; ++i) { q.g.t += 11; CHECK(q.xr_frame(frame_info(head0, ypr(0.3, -0.3, 0)), plan)); }
        q.g.t += 11; plan = xr_owner::LayerPlan{};
        CHECK(q.xr_frame(frame_info(head0, ypr(0.4, -0.3, 0)), plan) && q.core->stats().resubmits == 1 && plan.layer.table_quad_fixed);
        CHECK(same_pose(plan.layer.table_quad_pose, plane_quad(q.prod.latched_scene()->plane).pose));
    }
    // b3p:0 (no plane in the scene) and u3q:0 (the projection layer) keep the per-eye path
    {
        Rig q(false);
        q.g.t += 16; q.game_frame(0.0, frame_info(head0, ypr(0.3, -0.3, 0)));
        xr_owner::LayerPlan plan;
        q.g.t += 11; CHECK(q.xr_frame(frame_info(head0, ypr(0.3, -0.3, 0)), plan) && !plan.layer.table_quad_fixed && !q.prod.latched_scene()->plane.valid);
        Rig u(true); u.core->set_layering(0);
        u.g.t += 16; u.game_frame(0.0, frame_info(head0, ypr(0.3, -0.3, 0)));
        u.g.t += 11; plan = xr_owner::LayerPlan{};
        CHECK(u.xr_frame(frame_info(head0, ypr(0.3, -0.3, 0)), plan) && !plan.layer.table_quad_fixed && u.prod.latched_scene()->plane.valid);
        CHECK(u.core->stats().plane_frames == 0 && u.core->stats().plane_off == 0);
    }
}

int main() {
    test_fixed_plane_world_locked();
    test_null_pose_table_extent();
    test_t11_covers_colour_indices();
    test_t11_coverage_referenced_vertices_only();
    test_read_cache();
    test_read_budget_live_pattern();
    test_grid_matrix_per_frame();
    test_world_locked_across_head_motion();
    test_lean_in_no_refusal();
    test_incomplete_frames_and_icons();
    test_lifetime_stale_generation();
    test_busy_resubmits();
    test_worldlock_quad_depth_and_icons();
    test_extra_vb_refused();
    test_signal_failure();
    test_fallback();
    test_line_limits();
    std::printf("brief3d_latch_offline: %u checks passed\n", g_checks);
    return 0;
}
