// cam_epoch_offline_tests.cpp -- CAM-EPOCH (Astra P2; docs/world-render/e6-anchor-free.md section 10): the immutable timed/epoch-stamped E6 sample,
// the Present-history lookup, the admission rules (age, epochs, sanity, correlation, bounded fallback), the lag meter, a randomized comparison
// against an independent reference model and a multi-threaded torn-read stress. SDK-free; no game, no GPU.
#include "cam_lag.h"
#include "terrain_camera.h"
#include "terrain_stage.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace acz::terrain;

static unsigned g_checks = 0, g_fail = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static constexpr int64_t kMs = 1000000;
static constexpr int64_t kT0 = 5000 * kMs;   // an arbitrary steady-clock origin (never 0: 0 = untimed)

static void pub(CamMailbox& m, int64_t t, float x, float yaw = 0.1f) { const float p[3] = {x, 1000.0f, -x}; m.publish(p, 0.05f, yaw, -0.2f, 0.3f, t); }

// ---- basics: stamping, immutability, invalidate marker
static void test_basics() {
    CamEpochs ep; CamMailbox m(&ep); CamSample s;
    CHECK(!m.try_read(s));
    CamSample h; CHECK(m.nearest(kT0, kMs, h) == CamMailbox::Find::empty);
    ep.set_lock(3); ep.bump_stage(); ep.bump_stage();
    m.set_tag(77);
    pub(m, kT0, 10);
    CHECK(m.try_read(s) && s.t_ns == kT0 && s.lock_epoch == 3 && s.stage_epoch == 2 && s.tag == 77 && s.seq == 1 && s.campos[0] == 10);
    const CamSample copy = s;
    ep.set_lock(4); pub(m, kT0 + 16 * kMs, 20);
    CHECK(copy.campos[0] == 10 && copy.lock_epoch == 3 && copy.seq == 1);   // a held copy never changes with newer publications
    CHECK(m.try_read(s) && s.lock_epoch == 4 && s.seq == 2);
    m.invalidate(kT0 + 40 * kMs);
    CHECK(!m.try_read(s) && m.published() == 2);
    CHECK(m.nearest(kT0 + 41 * kMs, 5 * kMs, h) == CamMailbox::Find::invalidated);
    CHECK(m.nearest(kT0 + 15 * kMs, 5 * kMs, h) == CamMailbox::Find::ok && h.seq == 2 && h.campos[0] == 20);
    CHECK(m.nearest(kT0 + 28 * kMs, 5 * kMs, h) == CamMailbox::Find::missing);
    // non-finite publications never read back
    const float nanp[3] = {std::nanf(""), 0, 0}; m.publish(nanp, 0, 0, 0, 0, kT0 + 50 * kMs);
    CHECK(!m.try_read(s));
    CHECK(m.nearest(kT0 + 50 * kMs, kMs, h) == CamMailbox::Find::invalidated);   // a non-finite entry is not a usable sample
}

// ---- admission rules one by one
static void test_admit_rules() {
    AdmitLimits l; CamSample s; s.campos[0] = 100; s.campos[1] = 2000; s.campos[2] = -300; s.pitch = 0.1f; s.yaw = 1; s.roll = -0.5f; s.heading = 0.4f;
    s.t_ns = kT0; s.lock_epoch = 1; s.stage_epoch = 2;
    CHECK(admit_sample(s, kT0 + 100 * kMs, 1, 2, l) == Admit::ok);
    CHECK(admit_sample(s, kT0 + 250 * kMs, 1, 2, l) == Admit::ok);       // exactly at the limit
    CHECK(admit_sample(s, kT0 + 251 * kMs, 1, 2, l) == Admit::stale);
    CHECK(admit_sample(s, kT0 - 21 * kMs, 1, 2, l) == Admit::future);
    CHECK(admit_sample(s, kT0, 2, 2, l) == Admit::lock_epoch);
    CHECK(admit_sample(s, kT0, 1, 3, l) == Admit::stage_epoch);
    CamSample u = s; u.t_ns = 0; CHECK(admit_sample(u, kT0, 1, 2, l) == Admit::untimed);
    CamSample n = s; n.roll = std::nanf(""); CHECK(admit_sample(n, kT0, 1, 2, l) == Admit::nonfinite);
    CamSample i = s; i.campos[2] = 1e7f; CHECK(admit_sample(i, kT0, 1, 2, l) == Admit::insane);
    CamSample a = s; a.yaw = 9.0f; CHECK(admit_sample(a, kT0, 1, 2, l) == Admit::insane);
    for (unsigned k = 0; k < unsigned(Admit::count); ++k) CHECK(std::string(admit_name(Admit(k))) != "?");
}

// ---- publication followed by source loss: the last sample goes stale -> no camera (latest and frame modes)
static void test_source_loss() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l;
    for (int k = 0; k < 10; ++k) pub(m, kT0 + k * 16 * kMs, float(k));
    const int64_t last = kT0 + 9 * 16 * kMs;
    FrameCam none;
    // e6:latest: fresh at +100 ms, stale after the limit (the source stopped publishing; nothing invalidated it)
    CamPick p = pick_camera(none, false, m, 0, 0, last + 100 * kMs, l);
    CHECK(p.ok && p.src == CamSource::latest && p.s.campos[0] == 9);
    p = pick_camera(none, false, m, 0, 0, last + 300 * kMs, l);
    CHECK(!p.ok && p.why == Admit::stale);
    // e6:frame: a new frame (recorded after the loss) without its own sample: the history has nothing near its recording start -> missing, not newest
    FrameCam fc; fc.start_ns = last + 200 * kMs; fc.publish_ns = last + 210 * kMs;
    p = pick_camera(fc, true, m, 0, 0, last + 230 * kMs, l);
    CHECK(!p.ok && p.why == Admit::missing && p.src == CamSource::none);
    // its own (old) sample carried in a frame published 300 ms after it: stale relative to the frame's publication time; no history near its start
    FrameCam fs; fs.valid = true; CHECK(m.try_read(fs.s)); fs.start_ns = last + 290 * kMs; fs.publish_ns = last + 300 * kMs;
    p = pick_camera(fs, true, m, 0, 0, last + 310 * kMs, l);
    CHECK(!p.ok && p.frame_why == Admit::stale && p.why == Admit::missing);
    // the producer invalidates on loss (auto/content): a frame recorded right at the invalidation is refused as invalidated
    m.invalidate(last + 20 * kMs);
    FrameCam f2; f2.start_ns = last + 22 * kMs; f2.publish_ns = last + 30 * kMs;
    p = pick_camera(f2, true, m, 0, 0, last + 40 * kMs, l);
    CHECK(!p.ok && p.why == Admit::invalidated);
    // e6:latest after the invalidation: nothing
    p = pick_camera(none, false, m, 0, 0, last + 25 * kMs, l);
    CHECK(!p.ok && p.why == Admit::none);
}

// ---- stage change: samples of the old stage are refused (frame, fallback and latest); new samples are admitted
static void test_stage_change() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l;
    pub(m, kT0, 1);
    FrameCam fc; fc.valid = true; CHECK(m.try_read(fc.s)); fc.start_ns = kT0 - 2 * kMs; fc.publish_ns = kT0 + 10 * kMs;
    CHECK(pick_camera(fc, true, m, ep.lock(), ep.stage(), kT0 + 20 * kMs, l).ok);
    ep.bump_stage();
    CamPick p = pick_camera(fc, true, m, ep.lock(), ep.stage(), kT0 + 20 * kMs, l);
    CHECK(!p.ok && p.frame_why == Admit::stage_epoch && p.why == Admit::stage_epoch);   // the fallback finds the same old-stage publication: refused too
    p = pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), kT0 + 20 * kMs, l);
    CHECK(!p.ok && p.why == Admit::stage_epoch);
    pub(m, kT0 + 30 * kMs, 2);
    p = pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), kT0 + 35 * kMs, l);
    CHECK(p.ok && p.s.campos[0] == 2 && p.s.stage_epoch == ep.stage());
}

// ---- relock after loss: lock epochs 1 (lock) -> 2 (loss) -> 3 (relock)
static void test_relock() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l;
    ep.set_lock(1); pub(m, kT0, 1);
    CamSample old; CHECK(m.try_read(old));
    ep.set_lock(2); m.invalidate(kT0 + 1100 * kMs);                    // lost
    FrameCam fc; fc.valid = true; fc.s = old; fc.publish_ns = kT0 + 5 * kMs;
    CamPick p = pick_camera(fc, true, m, ep.lock(), ep.stage(), kT0 + 10 * kMs, l);
    CHECK(!p.ok && p.frame_why == Admit::lock_epoch);
    ep.set_lock(3); pub(m, kT0 + 1500 * kMs, 5);                       // relocked
    CamSample fresh; CHECK(m.try_read(fresh) && fresh.lock_epoch == 3);
    FrameCam f2; f2.valid = true; f2.s = fresh; f2.publish_ns = kT0 + 1510 * kMs; f2.start_ns = kT0 + 1490 * kMs;
    p = pick_camera(f2, true, m, ep.lock(), ep.stage(), kT0 + 1520 * kMs, l);
    CHECK(p.ok && p.src == CamSource::frame && p.s.campos[0] == 5);
    // the old-lock sample stays refused even though it is the only one of its frame and fresh by time
    fc.publish_ns = kT0 + 10 * kMs; p = pick_camera(fc, true, m, 3, 0, kT0 + 12 * kMs, l);
    CHECK(!p.ok && p.frame_why == Admit::lock_epoch);
}

// ---- correlation: wrong recording generation -> uncorrelated; bounded fallback to the history (never the newest)
static void test_correlation_and_fallback() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l;
    for (int k = 0; k < 20; ++k) pub(m, kT0 + k * 16 * kMs, float(k));   // 0..304 ms
    FrameCam fc; fc.valid = true; CHECK(m.try_read(fc.s)); fc.s.tag = 41; fc.expected_tag = 42;
    fc.start_ns = kT0 + 100 * kMs; fc.publish_ns = kT0 + 110 * kMs;
    fc.s.t_ns = kT0 + 101 * kMs;
    CamPick p = pick_camera(fc, true, m, 0, 0, kT0 + 310 * kMs, l);
    CHECK(p.frame_why == Admit::uncorrelated && p.ok && p.src == CamSource::fallback);
    CHECK(p.s.campos[0] == 6 && p.s.t_ns == kT0 + 96 * kMs);          // nearest to the recording start (100 ms), NOT the newest (19)
    // fallback disabled (camwin:0): refused with the frame's reason
    AdmitLimits nofb = l; nofb.fallback_window_ns = 0;
    p = pick_camera(fc, true, m, 0, 0, kT0 + 310 * kMs, nofb);
    CHECK(!p.ok && p.why == Admit::uncorrelated);
    // the window bounds the fallback: a recording start 60 ms after the last publication is too far (window 50 ms)
    FrameCam late; late.start_ns = kT0 + 364 * kMs; late.publish_ns = kT0 + 370 * kMs;
    p = pick_camera(late, true, m, 0, 0, kT0 + 380 * kMs, l);
    CHECK(!p.ok && p.why == Admit::missing);
    // a correlated, fresh own sample wins over any history entry
    FrameCam good = fc; good.s.tag = 42; p = pick_camera(good, true, m, 0, 0, kT0 + 310 * kMs, l);
    CHECK(p.ok && p.src == CamSource::frame);
}

// ---- the history ring overwritten: a frame recorded before the oldest retained publication is refused as overwritten
static void test_ring_overwritten() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l;
    for (int k = 0; k < 40; ++k) pub(m, kT0 + k * 16 * kMs, float(k));   // 40 > kHist 32: entries 0..7 overwritten
    CamSample h;
    CHECK(m.nearest(kT0, 10 * kMs, h) == CamMailbox::Find::overwritten);
    CHECK(m.nearest(kT0 + 8 * 16 * kMs, 10 * kMs, h) == CamMailbox::Find::ok && h.campos[0] == 8 && h.seq == 9);
    CHECK(m.nearest(kT0 + 7 * 16 * kMs, 10 * kMs, h) == CamMailbox::Find::overwritten);   // 16 ms before the oldest retained, window 10 ms
    FrameCam fc; fc.start_ns = kT0 + 2 * 16 * kMs; fc.publish_ns = fc.start_ns + 5 * kMs;
    const CamPick p = pick_camera(fc, true, m, 0, 0, kT0 + 640 * kMs, l);
    CHECK(!p.ok && p.why == Admit::overwritten);
}

// ---- lag meter
static CamBasis yaw_basis(float yaw_deg, float x = 0) {
    const float r = yaw_deg * 3.14159265f / 180.0f;
    CamBasis b; b.right[0] = std::cos(r); b.right[2] = -std::sin(r); b.up[1] = 1; b.fwd[0] = -std::sin(r); b.fwd[2] = -std::cos(r);
    b.eye[0] = x; return b;
}
static void test_lag_meter() {
    CHECK(std::fabs(basis_angle_deg(yaw_basis(10), yaw_basis(25)) - 15.0f) < 0.01f);
    // the game camera of frame k equals the E6 sample of frame k+1 (E6 at seam B AHEAD of the draws by one frame): best offset +1
    CamLagMeter m;
    for (uint64_t id = 1; id <= 60; ++id) m.frame(id, true, yaw_basis(float(id + 1) * 2.0f, float(id + 1)), true, true, yaw_basis(float(id) * 2.0f, float(id)), 12.0);
    CHECK(m.best_count(1) >= 55 && m.best_count(0) == 0);
    CHECK(std::fabs(m.angle0_mean() - 2.0) < 0.01 && m.pos0_max() > 0.99 && m.pos0_max() < 1.01);
    char buf[480]; CHECK(m.line(buf, sizeof buf, 1234)); std::string l(buf);
    CHECK(l.find("event=summary-camera-lag") != std::string::npos && l.find("+1:5") != std::string::npos && l.find("e6-age-ms-mean=12.0") != std::string::npos);
    CHECK(l.size() < 480);
    CHECK(!m.line(buf, sizeof buf, 1235));   // window reset, nothing new
    // consumed ids skip (the consumer takes the latest frame): frames are still evaluated; equal cameras -> offset 0
    CamLagMeter z;
    for (uint64_t id = 3; id <= 300; id += 3) z.frame(id, true, yaw_basis(float(id)), false, true, yaw_basis(float(id)), 5.0);
    CHECK(z.total_evaluated() >= 95 && z.best_count(0) == z.total_evaluated() && z.angle0_max() < 0.1);
    // Present offsets: the publication 16 ms after the recording start matches best
    CamLagMeter pm; float ang[CamLagMeter::kPresent];
    for (int r = 0; r < 10; ++r) {
        for (int i = 0; i < CamLagMeter::kPresent; ++i) ang[i] = std::fabs(float(CamLagMeter::present_offset_ns(i) / kMs) - 16.0f) * 0.1f;
        ang[0] = std::nanf("");
        pm.present(ang);
    }
    CHECK(pm.present_best_count(4) == 10 && CamLagMeter::present_offset_ns(4) == 16 * kMs);
}

// ---- option parsing
static void test_options() {
    std::string id, why; Conventions c;
    CHECK(parse_stage_option("m02a", id, c, why) && c.cam_admit && c.cam_age_ms == 250.0f && c.cam_window_ms == 50.0f);
    CHECK(parse_stage_option("m02a;camadmit:0", id, c, why) && !c.cam_admit);
    CHECK(parse_stage_option("m02a;camage:500;camwin:0", id, c, why) && c.cam_age_ms == 500.0f && c.cam_window_ms == 0.0f);
    CHECK(!parse_stage_option("m02a;camage:5", id, c, why));
    CHECK(!parse_stage_option("m02a;camwin:300", id, c, why));
    CHECK(!parse_stage_option("m02a;camadmit:2", id, c, why));
    CHECK(!Conventions{}.cam_admit);   // hand-built runtimes keep the pre-CAM-EPOCH selection
}

// ---- randomized comparison against an independent reference model
struct RefEntry { int64_t t; bool sample; CamSample s; };
struct RefModel {
    std::vector<RefEntry> h; uint64_t published = 0; uint32_t lock = 0, stage = 0; uint64_t tag = 0; bool valid = false; CamSample newest{};
    static bool finite(const CamSample& s) { const float v[7] = {s.campos[0], s.campos[1], s.campos[2], s.pitch, s.yaw, s.roll, s.heading};
        for (float x : v) if (!std::isfinite(x)) return false; return true; }
    void publish(const CamSample& in, int64_t t) {
        CamSample s = in; s.t_ns = t; s.lock_epoch = lock; s.stage_epoch = stage; s.tag = tag; s.seq = ++published;
        const bool f = finite(s);
        h.push_back({t, f, s}); valid = f; newest = s;   // a non-finite newest reads back as nothing
    }
    void invalidate(int64_t t) { CamSample s; s.t_ns = t; h.push_back({t, false, s}); valid = false; }
    static Admit admit(const CamSample& s, int64_t ref, uint32_t lk, uint32_t st, const AdmitLimits& l) {
        if (!finite(s)) return Admit::nonfinite;
        if (std::fabs(s.campos[0]) > l.pos_limit || std::fabs(s.campos[1]) > l.pos_limit || std::fabs(s.campos[2]) > l.pos_limit) return Admit::insane;
        if (std::fabs(s.pitch) > l.angle_limit || std::fabs(s.yaw) > l.angle_limit || std::fabs(s.roll) > l.angle_limit || std::fabs(s.heading) > 2 * l.angle_limit) return Admit::insane;
        if (!s.t_ns) return Admit::untimed;
        if (s.lock_epoch != lk) return Admit::lock_epoch;
        if (s.stage_epoch != st) return Admit::stage_epoch;
        if (ref - s.t_ns > l.max_age_ns) return Admit::stale;
        if (s.t_ns - ref > l.max_future_ns) return Admit::future;
        return Admit::ok;
    }
    // nearest over the last 32 entries: smallest |dt|; a marker among the tied best wins; first-come among tied samples
    Admit nearest(int64_t target, int64_t win, CamSample& out) const {
        if (h.empty()) return Admit::none;
        const size_t first = h.size() > CamMailbox::kHist ? h.size() - CamMailbox::kHist : 0;
        int64_t best = INT64_MAX, oldest = INT64_MAX;
        for (size_t i = first; i < h.size(); ++i) { const int64_t d = std::llabs(h[i].t - target); if (d < best) best = d; if (h[i].t < oldest) oldest = h[i].t; }
        if (best <= win) {
            const RefEntry* pick = nullptr;
            for (size_t i = first; i < h.size(); ++i) if (std::llabs(h[i].t - target) == best) { if (!h[i].sample) return Admit::invalidated; if (!pick) pick = &h[i]; }
            out = pick->s; return Admit::ok;
        }
        if (target < oldest - win && h.size() > CamMailbox::kHist) return Admit::overwritten;
        return Admit::missing;
    }
    CamPick pick(const FrameCam& fc, bool use_frame, int64_t now, const AdmitLimits& l) const {
        CamPick p;
        if (use_frame) {
            if (fc.valid) {
                Admit a = admit(fc.s, fc.publish_ns ? fc.publish_ns : now, lock, stage, l);
                if (a == Admit::ok && fc.expected_tag && fc.s.tag != fc.expected_tag) a = Admit::uncorrelated;
                p.frame_why = a;
                if (a == Admit::ok) { p.ok = true; p.s = fc.s; p.src = CamSource::frame; p.why = a; return p; }
            }
            if (l.fallback_window_ns <= 0) { p.why = p.frame_why; return p; }
            const int64_t target = fc.start_ns ? fc.start_ns : fc.publish_ns ? fc.publish_ns : now;
            CamSample s; const Admit f = nearest(target, l.fallback_window_ns, s);
            if (f != Admit::ok) { p.why = f; return p; }
            const Admit a = admit(s, target, lock, stage, l);
            if (a != Admit::ok) { p.why = a; return p; }
            p.ok = true; p.s = s; p.src = CamSource::fallback; p.why = a; return p;
        }
        if (!valid) { p.why = Admit::none; return p; }
        const Admit a = admit(newest, now, lock, stage, l); p.why = a; p.frame_why = a;
        if (a == Admit::ok) { p.ok = true; p.s = newest; p.src = CamSource::latest; }
        return p;
    }
};

static void test_reference_model(unsigned iterations, unsigned seed) {
    std::mt19937_64 rng(seed);
    auto U = [&](int64_t a, int64_t b) { return std::uniform_int_distribution<int64_t>(a, b)(rng); };
    unsigned mismatches = 0, queries = 0, oks = 0, fallbacks = 0;
    unsigned reasons[unsigned(Admit::count)] = {};
    for (unsigned it = 0; it < iterations; ++it) {
        CamEpochs ep; CamMailbox m(&ep); RefModel ref;
        int64_t now = kT0 + U(0, 1000) * kMs;
        std::vector<CamSample> seen;   // samples the "producer" could have copied into frames
        const unsigned ops = unsigned(U(5, 90));
        for (unsigned k = 0; k < ops; ++k) {
            now += U(0, 30) * kMs + U(0, 999999);
            const int op = int(U(0, 99));
            if (op < 50) {
                CamSample s; s.campos[0] = float(U(-300000, 300000)); s.campos[1] = float(U(0, 20000)); s.campos[2] = float(U(-300000, 300000));
                s.pitch = float(U(-300, 300)) / 100.0f; s.yaw = float(U(-314, 314)) / 100.0f; s.roll = float(U(-314, 314)) / 100.0f; s.heading = float(U(-600, 600)) / 100.0f;
                if (U(0, 40) == 0) s.yaw = std::nanf("");
                if (U(0, 40) == 0) s.campos[1] = 9e6f;
                const uint64_t tag = uint64_t(U(1, 4)); m.set_tag(tag); ref.tag = tag;
                m.publish(s.campos, s.pitch, s.yaw, s.roll, s.heading, now); ref.publish(s, now);
                CamSample back; if (m.try_read(back)) seen.push_back(back);
            } else if (op < 58) { m.invalidate(now); ref.invalidate(now); }
            else if (op < 64) { const uint32_t e = uint32_t(U(0, 3)); ep.set_lock(e); ref.lock = e; }
            else if (op < 68) { ep.bump_stage(); ++ref.stage; }
            else {
                FrameCam fc; fc.valid = U(0, 2) != 0 && !seen.empty();
                if (fc.valid) {
                    fc.s = seen[size_t(U(0, int64_t(seen.size()) - 1))];
                    if (U(0, 10) == 0) fc.s.t_ns = 0;
                }
                fc.expected_tag = uint64_t(U(0, 4));
                fc.start_ns = U(0, 6) == 0 ? 0 : now - U(0, 400) * kMs;
                fc.publish_ns = U(0, 6) == 0 ? 0 : (fc.start_ns ? fc.start_ns : now) + U(0, 40) * kMs;
                AdmitLimits l; l.max_age_ns = U(20, 300) * kMs; l.fallback_window_ns = U(0, 3) == 0 ? 0 : U(1, 60) * kMs; l.max_future_ns = U(0, 30) * kMs;
                const bool use_frame = U(0, 3) != 0;
                const CamPick a = pick_camera(fc, use_frame, m, ep.lock(), ep.stage(), now, l);
                const CamPick b = ref.pick(fc, use_frame, now, l);
                ++queries;
                const bool same = a.ok == b.ok && a.src == b.src && a.why == b.why && a.frame_why == b.frame_why &&
                                  (!a.ok || (a.s.seq == b.s.seq && a.s.t_ns == b.s.t_ns && a.s.campos[0] == b.s.campos[0] && a.s.lock_epoch == b.s.lock_epoch));
                if (!same) {
                    if (++mismatches <= 5) std::printf("mismatch it=%u ok=%d/%d src=%d/%d why=%s/%s fwhy=%s/%s seq=%llu/%llu\n", it, a.ok, b.ok, int(a.src), int(b.src),
                        admit_name(a.why), admit_name(b.why), admit_name(a.frame_why), admit_name(b.frame_why), (unsigned long long)a.s.seq, (unsigned long long)b.s.seq);
                }
                oks += a.ok; fallbacks += a.ok && a.src == CamSource::fallback; ++reasons[unsigned(a.why)];
                // invariants independent of the reference: an admitted sample is never older than the limit and is of the current epochs
                if (a.ok) CHECK(a.s.lock_epoch == ep.lock() && a.s.stage_epoch == ep.stage() && a.s.t_ns != 0);
                if (a.ok && a.src == CamSource::latest) CHECK(now - a.s.t_ns <= l.max_age_ns);
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(queries > iterations && oks > 0 && fallbacks > 0);
    // every refusal class was exercised
    for (Admit r : {Admit::stale, Admit::lock_epoch, Admit::stage_epoch, Admit::missing, Admit::invalidated, Admit::uncorrelated, Admit::none})
        CHECK(reasons[unsigned(r)] > 0);
    std::printf("reference model: %u iterations, %u queries, %u admitted (%u fallback), %u mismatches\n", iterations, queries, oks, fallbacks, mismatches);
}

// ---- multi-threaded stress: one writer (publish/invalidate/epochs/tag), three readers; every sample read must be internally consistent
static void test_threads(unsigned publications) {
    CamEpochs ep; CamMailbox m(&ep);
    std::atomic<bool> done{false};
    std::atomic<unsigned> torn{0}, reads{0}, hist{0}, last_k{0};
    auto consistent = [&](const CamSample& s) {
        const double k = s.campos[0];
        return s.campos[1] == float(k + 0.5) && s.t_ns == kT0 + int64_t(k) * 1000 && s.tag == uint64_t(k) && s.lock_epoch == uint32_t(int64_t(k) / 100) &&
               s.yaw == float(std::fmod(k, 3.0)) && s.seq != 0;
    };
    std::vector<std::thread> rd;
    for (int r = 0; r < 3; ++r) rd.emplace_back([&, r] {
        std::mt19937 g(r + 11); unsigned local = 0;
        while (!done.load(std::memory_order_acquire) || local < 20000) {
            CamSample s;
            if (m.try_read(s)) { ++reads; if (!consistent(s)) ++torn; }
            CamSample h;
            const unsigned lk = last_k.load(std::memory_order_relaxed);   // aim at the retained window
            const int64_t target = kT0 + (int64_t(lk) - int64_t(g() % 40)) * 1000;
            if (m.nearest(target, 2000, h) == CamMailbox::Find::ok) { ++hist; if (!consistent(h)) ++torn; }
            ++local;
        }
    });
    for (unsigned k = 1; k <= publications; ++k) {
        ep.set_lock(k / 100); m.set_tag(k);
        const float p[3] = {float(k), float(k + 0.5), 0};
        m.publish(p, 0.0f, float(std::fmod(double(k), 3.0)), 0.0f, 0.0f, kT0 + int64_t(k) * 1000);
        if (k % 7 == 0) m.invalidate(kT0 + int64_t(k) * 1000 + 500);
        if (k % 1000 == 0) ep.bump_stage();
        last_k.store(k, std::memory_order_relaxed);
    }
    done.store(true, std::memory_order_release);
    for (auto& t : rd) t.join();
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 1000 && hist.load() > 1000);
    std::printf("thread stress: %u publications, %u newest reads, %u history reads, %u torn\n", publications, reads.load(), hist.load(), torn.load());
}

// ---- review 2 (Astra P2 "epochs attached after the read"): read_publish captures base + lock + stage BEFORE the read, refuses a transition
// during it and publishes the CAPTURED identity. Deterministic interleavings: the "Present tick" acts inside the read callback (during the read) or
// in the before_publish hook (after the check, before the publication).
static void fill(float v[7], float x) { v[0] = x; v[1] = 1000.0f; v[2] = -x; v[3] = 0.05f; v[4] = 0.1f; v[5] = -0.2f; v[6] = 0.3f; }
static void test_read_interleavings() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l; CamSample s;
    const int64_t now = cam_clock_ns();
    auto latest = [&](CamSample& out) { const CamPick p = pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), cam_clock_ns(), l); out = p.s; return p; };
    // locked at base A (lock 1): published with the captured identity, the read callback got base A, read start <= publication
    ep.set_source(1, 0xA000);
    uint64_t seen = 0;
    CHECK(read_publish(m, ep, [&](uint64_t b, float v[7]) { seen = b; fill(v, 1); return true; }) == CamRead::published);
    CHECK(seen == 0xA000 && m.try_read(s) && s.lock_epoch == 1 && s.stage_epoch == 0 && s.campos[0] == 1 && s.t_read_ns >= now && s.t_read_ns <= s.t_ns);
    // nothing to publish
    CHECK(read_publish(m, ep, [&](uint64_t, float*) { return false; }) == CamRead::no_sample && m.published() == 1);
    // LOSS during the read (seam B read lock A's bytes; Present lost A and advanced the epoch): refused, never published under the new epoch
    CHECK(read_publish(m, ep, [&](uint64_t b, float v[7]) { ep.set_source(2, 0); fill(v, 2); return b == 0xA000; }) == CamRead::transition);
    CHECK(m.published() == 1 && m.try_read(s) && s.campos[0] == 1 && s.lock_epoch == 1);
    CamPick p = latest(s);
    CHECK(!p.ok && p.why == Admit::lock_epoch);                                     // the old lock's sample is not current either
    // RELOCK during the read (started unlocked, base 0): refused
    CHECK(read_publish(m, ep, [&](uint64_t b, float v[7]) { ep.set_source(3, 0xB000); fill(v, 3); return b == 0; }) == CamRead::transition);
    CHECK(m.published() == 1);
    // after the relock: base B, lock 3, admitted
    CHECK(read_publish(m, ep, [&](uint64_t b, float v[7]) { seen = b; fill(v, 4); return true; }) == CamRead::published && seen == 0xB000);
    p = latest(s);
    CHECK(p.ok && s.lock_epoch == 3 && s.campos[0] == 4);
    // STAGE change during the read: refused
    CHECK(read_publish(m, ep, [&](uint64_t, float v[7]) { ep.bump_stage(); fill(v, 5); return true; }) == CamRead::transition && m.published() == 2);
    // LOSS between the check and the publication: published with the CAPTURED (old) lock epoch -> admission refuses it
    CHECK(read_publish(m, ep, [&](uint64_t, float v[7]) { fill(v, 6); return true; }, [&] { ep.set_source(4, 0); }) == CamRead::published);
    CHECK(m.try_read(s) && s.campos[0] == 6 && s.lock_epoch == 3 && s.stage_epoch == 1);
    p = latest(s);
    CHECK(!p.ok && p.why == Admit::lock_epoch);
    // STAGE change between the check and the publication: the old stage is stamped -> refused
    ep.set_source(5, 0xC000);
    CHECK(read_publish(m, ep, [&](uint64_t, float v[7]) { fill(v, 7); return true; }, [&] { ep.bump_stage(); }) == CamRead::published);
    CHECK(m.try_read(s) && s.campos[0] == 7 && s.lock_epoch == 5 && s.stage_epoch == 1 && ep.stage() == 2);
    p = latest(s);
    CHECK(!p.ok && p.why == Admit::stage_epoch);
    // and a clean read afterwards is admitted
    CHECK(read_publish(m, ep, [&](uint64_t, float v[7]) { fill(v, 8); return true; }) == CamRead::published);
    p = latest(s);
    CHECK(p.ok && s.campos[0] == 8 && s.lock_epoch == 5 && s.stage_epoch == 2);
}
// The (base, lock) pair is coherent under a concurrent writer: the reader encodes the base it was handed into the sample; every published sample's
// lock epoch must be the epoch that base was installed with (the writer installs base k << 8 with lock k).
static void test_read_threads(unsigned updates) {
    CamEpochs ep; CamMailbox m(&ep);
    std::atomic<bool> done{false};
    unsigned published = 0, transitions = 0, busy = 0, bad = 0;
    std::thread rd([&] {
        while (!done.load() || published < 1000) {
            const CamRead r = read_publish(m, ep, [&](uint64_t b, float v[7]) {
                fill(v, float(b >> 8));
                for (int k = 0; k < 20; ++k) std::atomic_signal_fence(std::memory_order_seq_cst);   // a read that takes a little time
                return true; });
            if (r == CamRead::transition) ++transitions;
            else if (r == CamRead::busy) ++busy;
            else if (r == CamRead::published) {
                ++published; CamSample s;
                if (!m.try_read(s) || s.lock_epoch != uint32_t(s.campos[0])) ++bad;
            }
        }
    });
    for (unsigned k = 1; k <= updates; ++k) ep.set_source(k, uint64_t(k) << 8);
    done.store(true);
    rd.join();
    CHECK(bad == 0 && published > 0);
    std::printf("read/publish race: %u updates, %u published, %u transitions refused, %u busy, %u incoherent\n", updates, published, transitions, busy, bad);
}

// ---- E6-PLAUSIBLE (2026-10-30 headset session step133, m03k): replay of real camlag rows. The healthy hybrid frames have game eye == E6 eye (0.00-0.07 units);
// from 154209 s the E6 block read a constant 2,2,2 and every frame (2400 logged) was 125,000-165,000 units from the game eye while the game camera was refused.
static void test_e6_plausible() {
    struct Row { const char* what; float ge[3], ee[3]; bool agrees; };
    const Row rows[] = {
        {"hybrid 154177", {141646.61f, 20008.15f, 142673.22f}, {141646.61f, 20008.15f, 142673.22f}, true},
        {"hybrid 0.07 off", {141646.61f, 20008.15f, 142673.22f}, {141646.66f, 20008.19f, 142673.27f}, true},
        {"refused 154232", {109655.19f, 16593.76f, 121649.75f}, {2.0f, 2.0f, 2.0f}, false},
        {"refused 154414", {105767.99f, 18021.58f, 92653.74f}, {2.0f, 2.0f, 2.0f}, false},
        {"refused 154657", {88189.94f, 24858.48f, 85976.05f}, {2.0f, 2.0f, 2.0f}, false},
        {"game-only start, e6 zero block", {144976.53f, 20003.97f, 146000.58f}, {0.0f, 0.0f, 0.0f}, false},
    };
    for (const Row& r : rows) CHECK(e6_eye_agrees(r.ge, r.ee) == r.agrees);
    // the limit itself and non-finite input
    const float a[3] = {1000.0f, 500.0f, -300.0f};
    const float b1[3] = {1000.0f + 1999.0f, 500.0f, -300.0f}, b2[3] = {1000.0f + 2001.0f, 500.0f, -300.0f};
    CHECK(e6_eye_agrees(a, b1) && !e6_eye_agrees(a, b2));
    CHECK(e6_eye_agrees(a, b2, 3000.0f));
    const float nanv[3] = {std::nanf(""), 0.0f, 0.0f};
    CHECK(!e6_eye_agrees(a, nanv) && !e6_eye_agrees(nanv, a));
    // the separation margin in the real data is huge (0.07 vs 125,606): any limit between 1 and 100,000 splits the two populations
    CHECK(kE6EyeAgreeUnits > 100.0f && kE6EyeAgreeUnits < 100000.0f);
}

// ---- W15 F1: the qualified FALLBACK base (locked base 0, locator searching) is part of the source identity. Withdrawal/requalification/base change advance the
// stage epoch (set_fallback_base), so a frame read in flight during a withdrawal is refused, and a sample published just before it is not admitted.
static void test_fallback_withdrawal() {
    CamEpochs ep; CamMailbox m(&ep); AdmitLimits l; CamSample s;
    std::atomic<uint64_t> fb{0};
    const uint64_t B = 0xB000;
    // qualification: base B (stage advances once); the render seam reads the fallback base only while it equals fbBase
    CHECK(set_fallback_base(ep, fb, B) && fb.load() == B && ep.stage() == 1);
    CHECK(!set_fallback_base(ep, fb, B) && ep.stage() == 1);                         // unchanged base: no new transition
    auto frame_read = [&](std::function<void()> during, std::function<void()> before_publish = {}) {
        return read_publish(m, ep, [&](uint64_t base, float v[7]) {
            if (!base) base = fb.load();                                              // seam B: the gate-qualified probe base
            if (base != B) return false;
            fill(v, 1); if (during) during(); return true; }, [&] { if (before_publish) before_publish(); });
    };
    // quiet read: published and admitted (control)
    CHECK(frame_read({}) == CamRead::published && m.published() == 1);
    CamPick p = pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), cam_clock_ns(), l);
    CHECK(p.ok && p.s.stage_epoch == 1);
    // F1 trigger: withdrawal while the guarded copy is in flight (plausible sample returned, locked source stays 0, lock epoch unchanged): refused, nothing published
    CHECK(frame_read([&] { set_fallback_base(ep, fb, 0); }) == CamRead::transition && m.published() == 1);
    // the same withdrawal followed by a same-base REQUALIFICATION (ABA) during the read: still refused
    set_fallback_base(ep, fb, B);
    CHECK(frame_read([&] { set_fallback_base(ep, fb, 0); set_fallback_base(ep, fb, B); }) == CamRead::transition && m.published() == 1);
    // base replacement during the read: refused
    CHECK(frame_read([&] { set_fallback_base(ep, fb, 0xC000); }) == CamRead::transition && m.published() == 1);
    set_fallback_base(ep, fb, B);
    // withdrawal between the check and the publication: the sample carries the OLD stage; admission refuses it
    CHECK(frame_read({}, [&] { set_fallback_base(ep, fb, 0); }) == CamRead::published);
    p = pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), cam_clock_ns(), l);
    CHECK(!p.ok && p.why == Admit::stage_epoch);
    // a sample published BEFORE the withdrawal (Present history) is refused after it, and a read after the withdrawal gets no base at all
    set_fallback_base(ep, fb, B);
    CHECK(frame_read({}) == CamRead::published);
    CHECK(pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), cam_clock_ns(), l).ok);
    set_fallback_base(ep, fb, 0);
    CHECK(!pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), cam_clock_ns(), l).ok);
    CHECK(frame_read({}) == CamRead::no_sample);
    // requalified at the same base: a fresh read is admitted again under the new stage
    set_fallback_base(ep, fb, B);
    CHECK(frame_read({}) == CamRead::published && pick_camera(FrameCam{}, false, m, ep.lock(), ep.stage(), cam_clock_ns(), l).ok);
}
// ---- W15 F2: gross eye disagreement is source-independent (frame / Present history / latest all go through the same admitted-sample comparison)
static void test_e6_eye_far() {
    const float g[3] = {1000.0f, 500.0f, -300.0f};
    const float near_e[3] = {1000.0f + 1999.0f, 500.0f, -300.0f}, far_e[3] = {1000.0f + 2001.0f, 500.0f, -300.0f}, stale[3] = {-105768.0f, 18021.0f, 92654.0f};
    CHECK(e6_eye_far(true, true, g, far_e) && e6_eye_far(true, true, g, stale));
    CHECK(!e6_eye_far(true, true, g, near_e) && !e6_eye_far(true, true, g, g));          // the 2000-unit boundary: inclusive agreement
    CHECK(!e6_eye_far(true, false, g, far_e));                                           // no trustworthy game eye: nothing to compare against
    CHECK(!e6_eye_far(false, true, g, far_e));                                           // no admitted E6 sample
}

int main(int argc, char** argv) {
    unsigned iters = 4000;
    if (argc > 1) iters = unsigned(std::strtoul(argv[1], nullptr, 10));
    test_basics();
    test_admit_rules();
    test_source_loss();
    test_stage_change();
    test_relock();
    test_correlation_and_fallback();
    test_ring_overwritten();
    test_lag_meter();
    test_options();
    test_reference_model(iters, 20261027u);
    test_reference_model(iters / 4, 7u);
    test_threads(200000);
    test_read_interleavings();
    test_read_threads(200000);
    test_e6_plausible();
    test_fallback_withdrawal();
    test_e6_eye_far();
    std::printf("cam_epoch_offline_tests: %u checks, %u failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
