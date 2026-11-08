// e6_locator_offline_tests.cpp -- E6-ANCHOR-FREE (ee_e6_locator.h, docs/world-render/e6-anchor-free.md) offline tests, no game process:
// fake memory images with several 64 MiB blocks (the planted, moving E6 structure in one of them; an all-zero allocation like the probe's
// CandidateCollector block of every archived run; plausible-but-static decoys; moving decoys with the wrong geometry; unreadable blocks),
// the xr_terrain_stage e6loc option (values without it unchanged), single-sample checks on REAL E6 bytes/values from the archived logs,
// lock / hold / loss / relock, teleports, ambiguity, the per-tick byte caps, the candidate rules and cap, and the mailbox invalidate.
#include "ee_e6_locator.h"
#include "terrain_camera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace acz::e6loc;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// ------------------------------------------------------------------------------------------------ fake memory
struct FakeBlock { uint64_t base = 0, size = kBlockBytes; bool readable = true; std::map<uint64_t, std::vector<uint8_t>> pages; };
struct FakeMem final : ReadMem {
    std::vector<FakeBlock> blocks;
    uint64_t bytes = 0, reads = 0;
    FakeBlock* find(uint64_t a, uint32_t n) { for (auto& b : blocks) if (a >= b.base && a + n <= b.base + b.size) return &b; return nullptr; }
    bool read(uint64_t a, void* out, uint32_t n) noexcept override {
        ++reads;
        FakeBlock* b = find(a, n);
        if (!b || !b->readable) return false;
        bytes += n;
        auto* o = static_cast<uint8_t*>(out);
        for (uint32_t i = 0; i < n;) {   // page-wise (E6-RELOCATED: the struct scan reads whole 64 MiB blocks)
            const uint64_t x = a + i, pg = x & ~uint64_t(4095);
            const uint32_t k = uint32_t(std::min<uint64_t>(n - i, pg + 4096 - x));
            auto it = b->pages.find(pg);
            if (it == b->pages.end()) std::memset(o + i, 0, k); else std::memcpy(o + i, it->second.data() + (x - pg), k);
            i += k;
        }
        return true;
    }
    void put(uint64_t a, const void* src, size_t n) {
        FakeBlock* b = find(a, uint32_t(n)); if (!b) { std::printf("bad put\n"); std::exit(1); }
        const auto* s = static_cast<const uint8_t*>(src);
        for (size_t i = 0; i < n; ++i) {
            const uint64_t x = a + i, pg = x & ~uint64_t(4095);
            auto& v = b->pages[pg]; if (v.empty()) v.assign(4096, 0);
            v[size_t(x - pg)] = s[i];
        }
    }
    void putf(uint64_t a, float f) { put(a, &f, 4); }
};
// Write an E6 sample at the fixed offsets of block `base` (the inverse of decode()).
static void plant(FakeMem& m, uint64_t base, const Sample& s) {
    for (int i = 0; i < 3; ++i) {
        m.putf(base + kWinA + 4 * i, s.look[i]); m.putf(base + kWinA + 0x20 + 4 * i, s.air[i]);
        m.putf(base + kWinB + 4 * i, s.cam[i]); m.putf(base + kWinB + 0x10 + 4 * i, s.rot[i]);
    }
    m.putf(base + kWinA + 0x3C, s.heading);
    m.putf(base + kWinB + 0xC, 2.0f);   // the unknown word between cam and rot (2.0 in every dump)
}
// A flying aircraft: J35 eye point (0, 3.575, -36.3) in the aircraft frame (D-303), heading h, pitch p (nose up +), roll ignored for the eye.
static Sample flight(double t, double h = -0.7853982, double p = 0.0, double speed = 1400.0, double look_yaw = 0.0, double x0 = -21140.0, double z0 = 128801.0) {
    Sample s;
    const double f[3] = {-std::sin(h) * std::cos(p), std::sin(p), -std::cos(h) * std::cos(p)};
    const double u[3] = {std::sin(h) * std::sin(p), std::cos(p), std::cos(h) * std::sin(p)};
    const double air[3] = {x0 + f[0] * speed * t, 4000.0 + f[1] * speed * t, z0 + f[2] * speed * t};
    for (int i = 0; i < 3; ++i) { s.air[i] = float(air[i]); s.cam[i] = float(air[i] + 36.3 * f[i] + 3.575 * u[i]); }
    s.heading = float(h); s.look[1] = float(look_yaw);
    s.rot[0] = float(p); s.rot[1] = float(std::remainder(h + look_yaw, 2 * 3.14159265358979)); s.rot[2] = 0.0f;
    return s;
}
struct CapSink final : Sink {
    std::vector<std::string> lines;
    void line(bool, const char* t) noexcept override { lines.emplace_back(t); }
    int count(const char* needle) const { int n = 0; for (auto& l : lines) if (l.find(needle) != std::string::npos) ++n; return n; }
    std::string first(const char* needle) const { for (auto& l : lines) if (l.find(needle) != std::string::npos) return l; return ""; }
};
static std::vector<uint8_t> unhex(const char* h) { std::vector<uint8_t> v; for (size_t i = 0; h[i] && h[i + 1]; i += 2) { unsigned x; std::sscanf(h + i, "%2x", &x); v.push_back(uint8_t(x)); } return v; }

constexpr uint64_t kZeroAlloc = 0x1d8e3ac0000ull;     // step67 layout: the allocation-exact all-zero block ...
constexpr uint64_t kE6 = 0x1d924ea0000ull;            // ... and the 64 MiB region holding E6
constexpr uint64_t kStatic = 0x1d9300000000ull, kBadGeom = 0x1d9400000000ull, kUnread = 0x1d9500000000ull, kSmall = 0x1d9600000000ull;

static CandidateBuilder builder_for(const std::vector<RegionDesc>& rs) { CandidateBuilder b; for (auto& r : rs) b.add(r); b.finish(); return b; }
static RegionDesc reg(uint64_t base, uint64_t size, uint64_t alloc, bool rw = true, bool committed = true, bool priv = true) {
    RegionDesc r; r.base = base; r.size = size; r.alloc = alloc; r.committed = committed; r.is_private = priv; r.read_write = rw; return r;
}

// ------------------------------------------------------------------------------------------------ tests
static void test_option() {
    const char* unchanged[] = {"m02a", "auto;env:1", "m02a;env:1;look:1;props:1;trees:1;propdedupe:1", "m02a;cam:100,4000,200,45", "m02a;csrc:e6;e6:latest", "m02a;e6locx:1"};
    for (const char* v : unchanged) {
        std::string rest, why; Mode m = Mode::probe;
        CHECK(extract_mode(v, rest, m, why)); CHECK(rest == v); CHECK(m == Mode::automatic);
        bool ok = false; CHECK(mode_in(v, ok) == Mode::automatic && ok);
    }
    struct C { const char* v; const char* rest; Mode m; } cs[] = {
        {"m02a;e6loc:probe", "m02a", Mode::probe}, {"m02a;env:1;e6loc:content;props:1", "m02a;env:1;props:1", Mode::content},
        {"auto;e6loc=auto;env:1", "auto;env:1", Mode::automatic}, {"m02a;e6loc:probe;e6loc:content", "m02a", Mode::content}};
    for (auto& c : cs) {
        std::string rest, why; Mode m = Mode::automatic;
        CHECK(extract_mode(c.v, rest, m, why)); CHECK(rest == c.rest); CHECK(m == c.m);
        bool ok = false; CHECK(mode_in(c.v, ok) == c.m && ok);
    }
    for (const char* bad : {"m02a;e6loc:1", "m02a;e6loc:", "m02a;e6loc:Auto", "m02a;env:1;e6loc:probes"}) {
        std::string rest, why; Mode m;
        CHECK(!extract_mode(bad, rest, m, why)); CHECK(why.find("e6loc") != std::string::npos);
        bool ok = true; (void)mode_in(bad, ok); CHECK(!ok);
    }
    CHECK(std::string(mode_name(Mode::automatic)) == "auto" && std::string(mode_name(Mode::probe)) == "probe" && std::string(mode_name(Mode::content)) == "content");
}

static void test_real_samples() {
    // step67 ee-camera.log (209188461, tag=scan, base=1d924ea0000 = the E6 block): hexdumps at +FDD400, +FDD440, +FDD850.
    const auto d400 = unhex("00000000000051c100000000000000000000000000000000000000000000803f0000803ff304353f00000000e5065ac600007a45ae87ed472320eb388f4db536");
    const auto d440 = unhex("000080bf8e4db5b6db0f49bf7ec7092bdcdf4244cecccc3e0000000000000000000000000000004600000000cdcccc3d00000000000000000000000000000000");
    const auto d850 = unhex("0b58cd4196c964400b58cdc10000803f39a059c632397a45d87aed47000000408e4db5b6db0f49bf7ec7092b0000000000000000000000000000000000000000");
    CHECK(d400.size() == 64 && d440.size() == 64 && d850.size() == 64);
    uint8_t a[kWinABytes], b[kWinBBytes];
    std::memcpy(a, d400.data() + 0xC, 64 - 0xC); std::memcpy(a + (64 - 0xC), d440.data(), kWinABytes - (64 - 0xC));
    std::memcpy(b, d850.data() + 0x10, kWinBBytes);
    Sample s; decode(a, b, s);
    CHECK(std::fabs(s.air[0] - (-13953.7f)) < 1.0f && s.air[1] == 4000.0f && std::fabs(s.air[2] - 121615.4f) < 10.0f);
    CHECK(std::fabs(s.heading - (-0.7853982f)) < 1e-6f && std::fabs(s.rot[1] - (-0.7853982f)) < 1e-6f);
    CHECK(std::fabs(s.cam[1] - s.air[1] - 3.5748f) < 1e-3f);
    CHECK(check_sample(s) == nullptr);
    CHECK(std::fabs(eye_distance(s) - 36.47f) < 0.1f && eye_angle_deg(s) < 6.0);
    // The same windows of the all-zero allocation-exact block (every archived run): rejected.
    Sample z; uint8_t za[kWinABytes]{}, zb[kWinBBytes]{}; decode(za, zb, z);
    CHECK(std::string(check_sample(z)) == "aircraft-at-origin");
    // step56 nose-down (196764883): pitch -0.517 rad; step50 look (189594254): look yaw 2.475, rot yaw = heading + look;
    // headset test 16 (197188210): roll -2.96 (inverted, the eye's up offset points down).
    Sample n; n.air[0] = -13778.32f; n.air[1] = 3586.286f; n.air[2] = 121439.7f; n.cam[0] = -13754.76f; n.cam[1] = 3571.441f; n.cam[2] = 121416.2f;
    n.heading = -0.785398f; n.rot[0] = -0.517313f; n.rot[1] = -0.785398f;
    CHECK(check_sample(n) == nullptr); CHECK(eye_angle_deg(n) < 7.0);
    Sample flipped = n; flipped.rot[0] = 0.517313f;   // the opposite pitch sign would put the eye 55 deg off the forward axis
    CHECK(std::string(check_sample(flipped)) == "eye-direction");
    Sample l; l.look[1] = 2.47519f; l.heading = -0.785398f; l.air[0] = -3227.176f; l.air[1] = 4000; l.air[2] = 110891; l.cam[0] = -3202.793f; l.cam[1] = 4003.67f; l.cam[2] = 110865.2f;
    l.rot[0] = 3.47329e-06f; l.rot[1] = 1.69106f;
    CHECK(check_sample(l) == nullptr);
    Sample r; r.heading = -0.556928f; r.air[0] = -26055.68f; r.air[1] = 3695.608f; r.air[2] = 132071.1f; r.cam[0] = -26036.3f; r.cam[1] = 3687.822f; r.cam[2] = 132041.2f;
    r.rot[0] = -0.118526f; r.rot[1] = -0.556928f; r.rot[2] = -2.96203f;
    CHECK(check_sample(r) == nullptr);
    // A score-4 'junk' structure the relative scan followed in headset test 16 (243af19c898: air 46.4,-1.25,1.25, cam 1,1,-0.85): rejected.
    Sample j; j.air[0] = 46.44922f; j.air[1] = -1.253906f; j.air[2] = 1.253906f; j.cam[0] = 1.001953f; j.cam[1] = 1; j.cam[2] = -0.848694f;
    j.look[1] = 1; j.look[2] = 1; j.rot[0] = -0.528884f; j.rot[1] = 1; j.rot[2] = 0.84721f; j.heading = 7.09546e-39f;
    CHECK(check_sample(j) != nullptr);
    // Single-check rejections by name.
    Sample g = flight(0); CHECK(check_sample(g) == nullptr);
    { Sample x = g; x.air[1] = std::nanf(""); CHECK(std::string(check_sample(x)) == "nonfinite"); }
    { Sample x = g; x.look[0] = 3.3f; CHECK(std::string(check_sample(x)) == "look-range"); }
    { Sample x = g; x.rot[2] = -3.3f; CHECK(std::string(check_sample(x)) == "rotation-range"); }
    { Sample x = g; x.heading = 7.0f; CHECK(std::string(check_sample(x)) == "heading-range"); }
    { Sample x = flight(0, -0.785, 0, 1400, 0, 300000, 0); CHECK(std::string(check_sample(x)) == "position-range"); }
    { Sample x = g; x.air[1] = 250000; x.cam[1] = 250003.575f; CHECK(std::string(check_sample(x)) == "altitude-range"); }
    { Sample x = g; for (int i = 0; i < 3; ++i) x.cam[i] = x.air[i] + 3.0f * (x.cam[i] - x.air[i]); CHECK(std::string(check_sample(x)) == "eye-distance"); }
    { Sample x = g; for (int i = 0; i < 3; ++i) x.cam[i] = x.air[i] - (x.cam[i] - x.air[i]); CHECK(std::string(check_sample(x)) == "eye-direction"); }
    { Sample x = g; x.rot[1] = x.heading + 1.0f; CHECK(std::string(check_sample(x)) == "yaw-relation"); }
    // Pair checks.
    CHECK(check_pair(flight(0), flight(0.1), 100) == nullptr);
    CHECK(std::string(check_pair(flight(0), flight(0, -0.785, 0, 1400, 0, -11140), 100)) == "jump");
    { Sample x = flight(0.1); for (int i = 0; i < 3; ++i) x.cam[i] = x.air[i] + 1.1f * (x.cam[i] - x.air[i]); CHECK(std::string(check_pair(flight(0), x, 100)) == "eye-distance-unstable"); }
    CHECK(moved(flight(0), flight(0.1)) && !moved(flight(0), flight(0)));
}

static void test_candidates() {
    // The E6 region inside a larger allocation ('r'), an allocation-exact 64 MiB block of 3 regions ('a'), a 64 MiB region that is a whole
    // allocation ('r' once), a non-RW one, a mapped one, a 32 MiB one, an allocation of 64 MiB with a read-only part (not a candidate).
    std::vector<RegionDesc> rs = {
        reg(0x10000000, 0x200000, 0x10000000), reg(0x10200000, kBlockBytes, 0x10000000), reg(0x14200000, 0x1000000, 0x10000000),
        reg(0x20000000, 0x1000000, 0x20000000), reg(0x21000000, 0x2000000, 0x20000000), reg(0x23000000, 0x1000000, 0x20000000),
        reg(0x30000000, kBlockBytes, 0x30000000),
        reg(0x40000000, kBlockBytes, 0x40000000, false),
        reg(0x50000000, kBlockBytes, 0x50000000, true, true, false),
        reg(0x60000000, 0x2000000, 0x60000000),
        reg(0x70000000, 0x2000000, 0x70000000), reg(0x72000000, 0x2000000, 0x70000000, false)};
    CandidateBuilder b = builder_for(rs);
    CHECK(b.count() == 3); CHECK(b.regions_seen() == rs.size());
    CHECK(b.base(0) == 0x10200000 && b.kind(0) == 'r');
    CHECK(b.base(1) == 0x20000000 && b.kind(1) == 'a');
    CHECK(b.base(2) == 0x30000000 && b.kind(2) == 'r');
    // Cap: 20 candidate regions -> 16 kept, 4 dropped.
    std::vector<RegionDesc> many;
    for (uint64_t i = 0; i < 20; ++i) many.push_back(reg(0x100000000ull + i * 0x10000000ull, kBlockBytes, 0x100000000ull + i * 0x10000000ull));
    CandidateBuilder c = builder_for(many);
    CHECK(c.count() == kCandidateCap && c.dropped() == 4);
}

// The fake process: zero allocation (3 regions), static decoy, wrong-geometry moving decoy, unreadable block, a 32 MiB region with E6 planted
// (not a candidate), and the real block.
struct World {
    FakeMem m; double t0 = 0;
    World() {
        for (uint64_t b : {kZeroAlloc, kStatic, kBadGeom, kUnread, kE6}) { FakeBlock fb; fb.base = b; m.blocks.push_back(fb); }
        FakeBlock sm; sm.base = kSmall; sm.size = 0x2000000; m.blocks.push_back(sm);
        m.blocks[3].readable = false;
        plant(m, kStatic, flight(0, -0.3, 0, 1400, 0, 5000, 5000));   // plausible values that never change
    }
    void step(double t) {
        plant(m, kE6, flight(t));
        Sample bad = flight(t, 0.4, 0, 1400, 0, 9000, -9000);
        for (int i = 0; i < 3; ++i) bad.cam[i] = bad.air[i] + 2.2f * (bad.cam[i] - bad.air[i]);   // eye 80 units away: moving but wrong geometry
        plant(m, kBadGeom, bad);
        // The 0xFDD87C bytes do not fit a 32 MiB region: nothing to plant there; reading it would fail (not a candidate anyway).
    }
    CandidateBuilder candidates() const {
        std::vector<RegionDesc> rs = {reg(kZeroAlloc, 0x1000000, kZeroAlloc), reg(kZeroAlloc + 0x1000000, 0x2000000, kZeroAlloc), reg(kZeroAlloc + 0x3000000, 0x1000000, kZeroAlloc),
            reg(kE6 - 0x100000, 0x100000, kE6 - 0x100000), reg(kE6, kBlockBytes, kE6 - 0x100000),
            reg(kStatic, kBlockBytes, kStatic), reg(kBadGeom, kBlockBytes, kBadGeom), reg(kUnread, kBlockBytes, kUnread), reg(kSmall, 0x2000000, kSmall)};
        return builder_for(rs);
    }
};

// Drives the locator at 60 Hz; the glue's enumeration is answered at once. Returns the first lock time (ms) or -1.
static double run(Locator& L, World& w, CapSink& log, double from_ms, double to_ms, uint32_t& max_bytes, unsigned& valid_ticks, std::vector<std::string>* order = nullptr) {
    double lock_at = -1;
    for (double ms = from_ms; ms < to_ms; ms += 1000.0 / 60.0) {
        w.step(ms / 1000.0);
        if (L.wants_candidates(ms)) L.set_candidates(w.candidates(), ms, log);
        const uint64_t before = w.m.bytes;
        TickOut o; L.tick(w.m, ms, log, o);
        const uint32_t read = uint32_t(w.m.bytes - before);
        CHECK(read <= o.bytes);   // the reported cost covers what was read (unreadable reads count 0 in the fake)
        if (o.bytes > max_bytes) max_bytes = o.bytes;
        if (o.valid) { ++valid_ticks; CHECK(o.base == kE6); CHECK(check_sample(o.s) == nullptr); }
        if (L.state() == Locator::locked && lock_at < 0) lock_at = ms;
        if (order && L.state() == Locator::locked) { CHECK(o.bytes == kSampleBytes); }
    }
    return lock_at;
}

static void test_lock() {
    World w; Locator L; CapSink log; uint32_t maxb = 0; unsigned valid = 0;
    const double at = run(L, w, log, 0, 3000, maxb, valid);
    CHECK(at > 0 && at < 1500);                       // 8 samples at >= 100 ms, the first enumeration at tick 0
    CHECK(L.locked_base() == kE6 && L.locks() == 1 && L.losses() == 0);
    CHECK(L.lock_epoch() == 1 && L.discontinuities() == 0);   // CAM-EPOCH
    CHECK(maxb <= 512);                               // searching cap
    CHECK(valid > 60);                                // ~1.5 s of locked 60 Hz ticks, every one valid
    CHECK(log.count("e6-locate;event=candidates;n=5") == 1);   // zero 'a', E6 'r', static, bad-geometry, unreadable (32 MiB one excluded)
    const std::string lk = log.first("e6-locate;event=locked");
    CHECK(lk.find("base=1d924ea0000;kind=r") != std::string::npos && lk.find("look-events-needed=0") != std::string::npos);
    CHECK(log.count("event=ambiguous") == 0);
    // The search line explains every candidate (zero block: aircraft-at-origin, static decoy: static, wrong geometry: eye-distance, unreadable).
    const std::string se = log.first("e6-locate;event=search");
    CHECK(!se.empty());
    L.maybe_summary(3000, log, "auto");
    const std::string su = log.first("e6-locate;event=summary");
    CHECK(su.find("state=locked") != std::string::npos && su.find("aircraft-at-origin") != std::string::npos && su.find("unreadable") != std::string::npos &&
          su.find("eye-distance") != std::string::npos);
    // Static decoy never qualified: its candidate entry carries 'static' after 8 samples.
    World w2; Locator L2; CapSink log2; uint32_t mb2 = 0; unsigned v2 = 0;
    w2.m.blocks[4].readable = false;   // hide the real block: nothing may lock, the static decoy shows as static
    CHECK(run(L2, w2, log2, 0, 6000, mb2, v2) < 0 && v2 == 0 && L2.locks() == 0);
    bool sawStatic = false; for (auto& l : log2.lines) if (l.find("e6-locate;event=search") != std::string::npos && l.find(":static") != std::string::npos) sawStatic = true;
    CHECK(sawStatic);
}

static void test_locked_behaviour() {
    World w; Locator L; CapSink log; uint32_t maxb = 0; unsigned valid = 0;
    std::vector<std::string> dummy;
    CHECK(run(L, w, log, 0, 2000, maxb, valid) > 0);
    // Locked: 92 bytes per tick exactly.
    { maxb = 0; unsigned v = 0; run(L, w, log, 2000, 2500, maxb, v, &dummy); CHECK(maxb == kSampleBytes && v > 25); }
    // Teleport (mission restart): one jump sample rejected (hold), the next consistent with it: stays locked.
    {
        Sample far = flight(0, -0.785, 0, 1400, 0, 60000, -60000);
        plant(w.m, kE6, far); TickOut o; L.tick(w.m, 2600, log, o); CHECK(!o.valid && o.hold && L.state() == Locator::locked);
        Sample far2 = flight(0.0167, -0.785, 0, 1400, 0, 60000, -60000);
        plant(w.m, kE6, far2); L.tick(w.m, 2616.7, log, o); CHECK(o.valid && L.state() == Locator::locked);
        CHECK(L.discontinuities() == 1 && L.lock_epoch() == 1);   // CAM-EPOCH: the teleport is a stage discontinuity, not a lock change
    }
    // Paused game: identical samples stay valid while locked (static is only refused before the lock).
    { TickOut o; for (int i = 0; i < 30; ++i) { L.tick(w.m, 2700 + i * 16.7, log, o); CHECK(o.valid); } }
    // Block freed: unreadable -> hold (< 250 ms) -> no camera -> lost after 1000 ms; then relock when the block is back.
    {
        w.m.blocks[4].readable = false;
        TickOut o; double ms = 3300; bool sawHold = false, sawNone = false;
        for (; L.state() == Locator::locked && ms < 5000; ms += 16.7) {
            L.tick(w.m, ms, log, o); CHECK(!o.valid);
            if (o.hold) { sawHold = true; CHECK(ms - 3300 < 250 + 20); } else if (L.state() == Locator::locked) sawNone = true;
        }
        CHECK(sawHold && sawNone && L.state() == Locator::searching && L.losses() == 1);
        CHECK(L.lock_epoch() == 2 && L.discontinuities() == 1);   // CAM-EPOCH: loss = new lock epoch; unreadable is not a discontinuity
        const double last_valid = 2700 + 29 * 16.7;   // the last paused tick above
        CHECK(ms - last_valid >= 1000 && ms - last_valid < 1100);
        CHECK(log.first("e6-locate;event=lost").find("reason=unreadable") != std::string::npos);
        CHECK(L.wants_candidates(ms));
        w.m.blocks[4].readable = true;
        uint32_t mb = 0; unsigned v = 0;
        CHECK(run(L, w, log, ms, ms + 2500, mb, v) > 0 && L.locks() == 2 && L.locked_base() == kE6);
        CHECK(L.lock_epoch() == 3);   // CAM-EPOCH: relock
    }
    // Garbage written over the locked block (memory reused): lost by content, not only by readability.
    {
        uint8_t junk[kWinABytes]; for (auto& x : junk) x = 0x7f;
        w.m.put(kE6 + kWinA, junk, sizeof junk);
        TickOut o; double ms = 9000; for (int i = 0; i < 80; ++i, ms += 16.7) L.tick(w.m, ms, log, o);
        CHECK(L.state() == Locator::searching && L.losses() == 2);
        CHECK(L.lock_epoch() == 4);
        L.maybe_summary(20000, log, "auto");
        CHECK(log.first("e6-locate;event=summary").find("lock-epoch=") != std::string::npos);
    }
}

static void test_ambiguous_and_look() {
    // Two moving, identical, valid copies: refused (no lock), logged.
    World w; Locator L; CapSink log;
    FakeBlock twin; twin.base = 0x1da000000000ull; w.m.blocks.push_back(twin);
    double ms = 0; bool locked = false;
    for (; ms < 3000; ms += 16.7) {
        w.step(ms / 1000.0); plant(w.m, twin.base, flight(ms / 1000.0));
        if (L.wants_candidates(ms)) {
            std::vector<RegionDesc> rs = {reg(kE6, kBlockBytes, kE6), reg(twin.base, kBlockBytes, twin.base)};
            L.set_candidates(builder_for(rs), ms, log);
        }
        TickOut o; L.tick(w.m, ms, log, o); locked = locked || L.state() == Locator::locked;
    }
    if (locked || log.count("e6-locate;event=ambiguous") < 1) for (auto& l : log.lines) std::printf("%s\n", l.c_str());
    CHECK(!locked && log.count("e6-locate;event=ambiguous") >= 1);
    // A flight with the game look turned (rot yaw = heading + look) and a pitched climb: locks.
    FakeMem m; FakeBlock b; b.base = kE6; m.blocks.push_back(b);
    Locator L2; CapSink log2; std::vector<RegionDesc> rs = {reg(kE6, kBlockBytes, kE6)};
    for (ms = 0; ms < 2000; ms += 16.7) {
        plant(m, kE6, flight(ms / 1000.0, 1.2, 0.35, 1800.0, 2.0));
        if (L2.wants_candidates(ms)) L2.set_candidates(builder_for(rs), ms, log2);
        TickOut o; L2.tick(m, ms, log2, o);
    }
    CHECK(L2.state() == Locator::locked);
}

// 16 candidates (the cap): 15 plausible static decoys before the real block in address order; the 512-byte tick budget (5 samples) must still
// reach every candidate (round robin) and lock the last one; every candidate sampled at most once per 100 ms.
static void test_many_candidates() {
    FakeMem m; std::vector<RegionDesc> rs;
    for (unsigned i = 0; i < 16; ++i) {
        FakeBlock b; b.base = 0x300000000000ull + uint64_t(i) * 0x10000000ull; m.blocks.push_back(b);
        rs.push_back(reg(b.base, kBlockBytes, b.base));
        if (i < 15) plant(m, b.base, flight(0, -0.3 + 0.01 * i, 0, 1400, 0, 1000.0 * i, 2000));
    }
    const uint64_t real = m.blocks[15].base;
    Locator L; CapSink log; double lock_at = -1; uint32_t maxb = 0;
    for (double ms = 0; ms < 4000 && lock_at < 0; ms += 1000.0 / 60.0) {
        plant(m, real, flight(ms / 1000.0));
        if (L.wants_candidates(ms)) L.set_candidates(builder_for(rs), ms, log);
        const uint64_t r0 = m.reads; TickOut o; L.tick(m, ms, log, o);
        CHECK((m.reads - r0) / 2 * kSampleBytes == o.bytes);   // two windows per sample
        maxb = o.bytes > maxb ? o.bytes : maxb;
        if (L.state() == Locator::locked) lock_at = ms;
    }
    CHECK(lock_at > 0 && lock_at < 2000 && L.locked_base() == real && maxb <= 512 && L.max_tick_bytes() <= 512);
}

// ------------------------------------------------------------------------------------------------ E6-RELOCATED (headset 2026-10-30, m03k)
// REAL values, headset 2026-10-30 live-1144 ee-camera.log: the E6 structure at 1c75fc54860 = region 1c75ff87000 +0xCAAC6C - 0xFDD40C (F-5E, airborne
// start ~18.5k), and the static junk structure the probe followed from 154209589 (1c683093140; L=2,2,2 air=2,6.6e-10,45.3 cam=2,2,2 rot=2,2,2).
static Sample m03k_real(int row) {
    static const float v[3][13] = {   // L p/y/r, H, air, cam, rot (timeline 154202972 / 154203223 / 154203490)
        {0.000571174f, 3.76265e-06f, 0, 0.945944f, 121363.3f, 18575.01f, 124996.8f, 121345.4f, 18578.98f, 124984.2f, 0.000675936f, 0.94644f, 0.0458042f},
        {0.000896947f, 0.000832736f, 0, 0.947376f, 121044.2f, 18575.14f, 124766.5f, 121026.3f, 18579.11f, 124753.9f, 0.000702357f, 0.948388f, 0.0413366f},
        {0.000441074f, -9.13151e-05f, 0, 0.948352f, 120693.9f, 18575.15f, 124514.5f, 120676.0f, 18579.12f, 124501.9f, 0.000609122f, 0.948781f, 0.040919f}};
    Sample s; const float* r = v[row];
    for (int i = 0; i < 3; ++i) { s.look[i] = r[i]; s.air[i] = r[4 + i]; s.cam[i] = r[7 + i]; s.rot[i] = r[10 + i]; }
    s.heading = r[3];
    return s;
}
static Sample m03k_junk() {
    Sample s; for (int i = 0; i < 3; ++i) { s.look[i] = 2; s.cam[i] = 2; s.rot[i] = 2; }
    s.heading = -3.42688e-35f; s.air[0] = 2; s.air[1] = 6.592302e-10f; s.air[2] = 45.30196f;
    return s;
}
// An F-5E (eye 0, 3.97, -21.9: aircraft_auto table) flying from the m03k airborne start (143730, 20000, 144754, heading 0.7854).
static Sample f5e(double t, double speed = 1400.0) {
    Sample s; const double h = 0.7853982;
    const double f[3] = {-std::sin(h), 0.0, -std::cos(h)};
    const double air[3] = {143730.0 + f[0] * speed * t, 20000.0, 144754.0 + f[2] * speed * t};
    for (int i = 0; i < 3; ++i) { s.air[i] = float(air[i]); s.cam[i] = float(air[i] + 21.9 * f[i] + (i == 1 ? 3.97 : 0.0)); }
    s.heading = float(h); s.rot[1] = float(h);
    return s;
}
// Writes the sample at a structure base anywhere (plant() with base = the structure base, not a block).
static void plant_struct(FakeMem& m, uint64_t sbase, const Sample& s) { plant(m, sbase, s); }

static void test_real_m03k() {
    for (int r = 0; r < 3; ++r) {
        const Sample s = m03k_real(r);
        CHECK(check_sample(s) == nullptr);
        CHECK(std::fabs(eye_distance(s) - 22.257f) < 0.6f);   // the F-5E eye (aircraft_auto f05e 22.257)
        if (r) CHECK(check_pair(m03k_real(r - 1), s, 260.0) == nullptr && moved(m03k_real(r - 1), s));
    }
    // The junk structure passes the single-sample checks: why the probe-base fallback needs the lock rules (FallbackGate), not check_sample alone.
    CHECK(check_sample(m03k_junk()) == nullptr);
    // The fixed offset of the m03k block read zero/garbage there: 'aircraft-at-origin' in every e6-locate search line of the session.
    Sample g; g.air[0] = 0.006448627f; g.air[1] = -6.413542e+37f; g.air[2] = -1.034627e-08f;
    CHECK(std::string(check_sample(g)) == "aircraft-at-origin");
    CHECK(kWinGap == 0x454 && kStructBytes == 0x470);
}

static void test_scan_struct() {
    // Buffer-level: the real m03k sample planted at an arbitrary 4-aligned position is found with the right base; zero memory gives nothing.
    std::vector<uint8_t> buf(0x6000, 0);
    const uint64_t addr = 0x1c75ff87000ull + 0xCAA000;
    const size_t at = 0xC6C;   // window A at addr + 0xC6C = region + 0xCAAC6C
    const Sample s = m03k_real(0);
    auto put = [&](size_t o, float f) { std::memcpy(buf.data() + o, &f, 4); };
    for (int i = 0; i < 3; ++i) { put(at + 4 * i, s.look[i]); put(at + 0x20 + 4 * i, s.air[i]); put(at + kWinGap + 4 * i, s.cam[i]); put(at + kWinGap + 0x10 + 4 * i, s.rot[i]); }
    put(at + 0x3C, s.heading);
    uint64_t b[4] = {};
    CHECK(scan_struct(buf.data(), buf.size(), addr, b, 4) == 1);
    CHECK(b[0] == 0x1c75fc54860ull);   // = the probe's score-5 base of the session (observe-follow 154176505)
    CHECK(b[0] + kWinA == 0x1c75ff87000ull + 0xCAAC6C);
    // The structure must fit completely: truncated just before window B's end -> not found; the cap limits writes, not the count.
    CHECK(scan_struct(buf.data(), at + kStructBytes - 1, addr, b, 4) == 0);
    CHECK(scan_struct(buf.data(), at + kStructBytes, addr, b, 4) == 1);
    CHECK(scan_struct(buf.data(), buf.size(), addr, nullptr, 0) == 1);
    std::vector<uint8_t> zero(0x4000, 0);
    CHECK(scan_struct(zero.data(), zero.size(), addr, b, 4) == 0);
    // Scanner over fake blocks: chunk overlap (a structure straddling a 1 MiB chunk border is found once), unreadable blocks counted, hits capped.
    FakeMem m; FakeBlock fb; fb.base = 0x500000000000ull; m.blocks.push_back(fb); FakeBlock un; un.base = 0x510000000000ull; un.readable = false; m.blocks.push_back(un);
    const uint64_t straddle = fb.base + StructScanner::kChunk - 0x200 - kWinA;   // window A 0x200 before the chunk border, window B after it
    plant_struct(m, straddle, f5e(0));
    CandidateBuilder cb = builder_for({reg(fb.base, kBlockBytes, fb.base), reg(un.base, kBlockBytes, un.base)});
    StructScanner sc; sc.start(cb);
    CHECK(sc.active() && sc.blocks() == 2 && sc.hits() == 0);
    unsigned calls = 0; while (!sc.step(m, 4u << 20)) ++calls;
    CHECK(calls >= 25 && calls <= 40);   // 128 MiB at 4 MiB per call
    CHECK(!sc.active() && sc.passes() == 1 && sc.hits() == 1 && sc.hit(0) == straddle && sc.hit_block(0) == fb.base);
    CHECK(sc.pass_unreadable_chunks() == 64 && sc.pass_bytes() >= kBlockBytes && sc.pass_bytes() < kBlockBytes + 64 * kStructBytes);
    CandidateBuilder merged = cb; sc.append_hits(merged);
    CHECK(merged.count() == 3 && merged.kind(2) == 's' && merged.base(2) == straddle);
    sc.append_hits(merged); CHECK(merged.count() == 3);   // deduplicated
    for (unsigned i = 0; i < 20; ++i) { Sample j = m03k_junk(); j.air[0] += float(i); j.cam[0] += float(i); plant_struct(m, fb.base + 0x2000000 + 0x1000 * i, j); }
    sc.start(cb); while (!sc.step(m, 64u << 20)) {}
    CHECK(sc.hits() == StructScanner::kHitCap && sc.hits_dropped() == 21 - StructScanner::kHitCap);
    sc.start(cb); CHECK(sc.hits() == StructScanner::kHitCap);   // a running pass keeps the previous hits
    sc.cancel(); CHECK(!sc.active());
}

// The m03k layout: an all-zero 64 MiB region, and the E6 region with garbage at the fixed offset, the F-5E structure at +0xCAAC6C and the static junk.
struct M03k {
    FakeMem m;
    static constexpr uint64_t kZ = 0x1c713260000ull, kR = 0x1c75ff87000ull;
    static constexpr uint64_t kStruct = kR + 0xCAAC6C - kWinA;      // = 0x1c75fc54860
    static constexpr uint64_t kJunk = kR + 0x400000;
    M03k() {
        FakeBlock z; z.base = kZ; m.blocks.push_back(z); FakeBlock r; r.base = kR; m.blocks.push_back(r);
        Sample g; g.air[0] = 0.006448627f; g.air[1] = -6.413542e+37f; g.air[2] = -1.034627e-08f; g.cam[2] = 30.1113f;
        plant(m, kR, g);                                             // the fixed offset: aircraft-at-origin
        plant_struct(m, kJunk, m03k_junk());
    }
    bool frozen = false;   // the aircraft not moving (paused at the airborne spawn)
    void step(double t) { plant_struct(m, kStruct, f5e(frozen ? 0.0 : t)); }
    CandidateBuilder regions() const { return builder_for({reg(kZ, kBlockBytes, kZ), reg(kR - 0x1000, 0x1000, kR - 0x1000), reg(kR, kBlockBytes, kR - 0x1000)}); }
};
// Glue model: enumeration when asked; with `scan`, a StructScanner pass (4 MiB per 60 Hz tick, rescan every 3 s) whose hits join the list.
static double run_m03k(M03k& w, Locator& L, CapSink& log, bool scan, double to_ms, unsigned& valid, uint64_t& valid_base) {
    StructScanner sc; CandidateBuilder regs; double lock_at = -1, pass_end = -1e300; bool have = false;
    for (double ms = 0; ms < to_ms; ms += 1000.0 / 60.0) {
        w.step(ms / 1000.0);
        bool pub = false;
        if (L.wants_candidates(ms)) { regs = w.regions(); have = true; pub = true; }
        if (scan && have && L.state() == Locator::searching) {
            if (!sc.active() && ms - pass_end >= 3000) sc.start(regs);
            if (sc.active() && sc.step(w.m, 4u << 20)) { pass_end = ms; pub = true; }
        }
        if (pub) { CandidateBuilder c = regs; if (scan) sc.append_hits(c); L.set_candidates(c, ms, log); }
        TickOut o; L.tick(w.m, ms, log, o);
        CHECK(o.bytes <= 512);
        if (o.valid) { ++valid; valid_base = o.base; CHECK(check_sample(o.s) == nullptr); }
        if (L.state() == Locator::locked && lock_at < 0) lock_at = ms;
    }
    return lock_at;
}
static void test_relocated_lock() {
    {   // BEFORE (fixed offsets only, the shipped step133 flow): never locks, every sample 'aircraft-at-origin' - the headset 2026-10-30 summary.
        M03k w; Locator L; CapSink log; unsigned valid = 0; uint64_t vb = 0;
        CHECK(run_m03k(w, L, log, false, 8000, valid, vb) < 0);
        CHECK(L.locks() == 0 && valid == 0);
        L.maybe_summary(8000, log, "auto");
        const std::string su = log.first("e6-locate;event=summary");
        CHECK(su.find("state=searching") != std::string::npos && su.find("locks=0") != std::string::npos && su.find("aircraft-at-origin") != std::string::npos);
    }
    {   // AFTER: the scan finds the relocated structure (and the static junk); the locator locks on the structure only, as kind 's'.
        M03k w; Locator L; CapSink log; unsigned valid = 0; uint64_t vb = 0;
        const double at = run_m03k(w, L, log, true, 8000, valid, vb);
        CHECK(at > 0 && at < 2500);   // one pass (~0.55 s) + 8 samples at 100 ms
        CHECK(L.locked_base() == M03k::kStruct && L.locks() == 1 && L.losses() == 0 && vb == M03k::kStruct && valid > 300);
        const std::string lk = log.first("e6-locate;event=locked");
        CHECK(lk.find("base=1c75fc54860;kind=s") != std::string::npos);
        // zero 'r', E6 'r', the structure 's', the junk 's' (+ any other plausible static pattern, e.g. one spanning the garbage at the fixed offset)
        const std::string cl = log.first("1c75fc54860:s");
        CHECK(cl.find("1c713260000:r,1c75ff87000:r,") != std::string::npos && cl.find("1c760387000:s") != std::string::npos);
        CHECK(log.count("event=ambiguous") == 0);
    }
    {   // Frozen at the spawn: the scan finds the structure and the junk, both plausible, neither moves: no lock, both reported 'static'.
        M03k w; w.frozen = true; Locator L; CapSink log; unsigned valid = 0; uint64_t vb = 0;
        CHECK(run_m03k(w, L, log, true, 12000, valid, vb) < 0 && L.locks() == 0 && valid == 0);
        auto token = [](const std::string& l, const char* key) {   // "<base>:s:<streak>/<moves>:<why>" up to the next ';'
            const size_t a = l.find(key); if (a == std::string::npos) return std::string();
            const size_t e = l.find(';', a); return l.substr(a, e == std::string::npos ? std::string::npos : e - a);
        };
        std::string ts, tj;
        for (auto& l : log.lines) if (l.find("e6-locate;event=search") != std::string::npos) { ts = token(l, "1c75fc54860:s:"); tj = token(l, "1c760387000:s:"); }
        CHECK(ts.size() > 7 && ts.compare(ts.size() - 7, 7, ":static") == 0);   // the last search line
        CHECK(tj.size() > 7 && tj.compare(tj.size() - 7, 7, ":static") == 0);
    }
    {   // Relocation while locked (the structure moves to another offset): lost, the search rescans, relocks at the new base.
        M03k w; Locator L; CapSink log; unsigned valid = 0; uint64_t vb = 0;
        StructScanner sc; CandidateBuilder regs; double pass_end = -1e300; bool have = false, moved_away = false;
        const uint64_t kNew = M03k::kR + 0x123450 - kWinA;
        for (double ms = 0; ms < 9000; ms += 1000.0 / 60.0) {
            if (ms >= 3000 && !moved_away) { plant_struct(w.m, M03k::kStruct, Sample{}); moved_away = true; }
            plant_struct(w.m, moved_away ? kNew : M03k::kStruct, f5e(ms / 1000.0));
            bool pub = false;
            if (L.wants_candidates(ms)) { regs = w.regions(); have = true; pub = true; }
            if (have && L.state() == Locator::searching) {
                if (!sc.active() && ms - pass_end >= 3000) sc.start(regs);
                if (sc.active() && sc.step(w.m, 4u << 20)) { pass_end = ms; pub = true; }
            }
            if (pub) { CandidateBuilder c = regs; sc.append_hits(c); L.set_candidates(c, ms, log); }
            TickOut o; L.tick(w.m, ms, log, o);
            if (o.valid) { ++valid; vb = o.base; }
        }
        CHECK(L.losses() == 1 && L.locks() == 2 && L.locked_base() == kNew && vb == kNew);
        CHECK(log.count("e6-locate;event=lost") == 1);
    }
}


// E6-ALIAS (POV1 null run 2026-10-31, D:\acz-evidence-archive\pov1\runPOV1\logs\ee-camera.log): in the chase view the real structure (base ...bd10)
// was lost on eye-distance (85.7 units) and the content scan offered base ...bd0c = the SAME memory 4 bytes earlier, whose shifted read
// (air = 0,x,y; cam = 1.0,x,y; rot = 2.0,pitch,yaw; heading = the word before) passed every check (eye 58.5 units, 5.5 deg) and locked: the
// classifier then called the chase view "HUD-only". The fixture rebuilds that exact layout from the logged chase numbers.
static void plant_pov1(FakeMem& m, uint64_t base, double t, bool chase) {
    const double h = 0.70978, v = 1100.0, f[3] = {-std::sin(h), 0.0, -std::cos(h)};
    Sample s;
    const double air[3] = {92518.3 + f[0] * v * t, 19387.2, 91595.0 + f[2] * v * t};
    for (int i = 0; i < 3; ++i) {
        s.air[i] = float(air[i]);
        s.cam[i] = chase ? float(air[i] - 84.0 * f[i] + (i == 1 ? 17.0 : 0.0)) : float(air[i] + 36.3 * f[i] + (i == 1 ? 3.575 : 0.0));
    }
    s.heading = float(h); s.rot[0] = -0.03368f; s.rot[1] = float(h); s.rot[2] = 0.05104f;
    plant(m, base, s);
    m.putf(base + kWinA + 0x38, s.rot[0]);   // the word before the heading (the alias's "heading"; logged alias heading = the pitch)
    m.putf(base + kWinB - 4, 1.0f);          // the word before the camera (the alias's cam x = 1.00 in the log)
}
static void test_alias_guard() {
    constexpr uint64_t kBlk = 0x1836000000ull, kHome = kBlk + 0x10, kAlias = kHome - 4;
    {   // the fixture reproduces the log: home refused for its chase geometry, the -4 alias valid
        FakeMem m; FakeBlock b; b.base = kBlk; m.blocks.push_back(b);
        plant_pov1(m, kHome, 0.0, true);
        Sample h, a; CHECK(read_sample(m, kHome, h) && read_sample(m, kAlias, a));
        CHECK(std::string(check_sample(h)) == "eye-distance" && std::fabs(eye_distance(h) - 85.7f) < 0.3f);
        CHECK(check_sample(a) == nullptr && a.cam[0] == 1.0f && a.rot[0] == 2.0f && a.air[0] == 0.0f && std::fabs(eye_distance(a) - 57.2f) < 1.0f);
    }
    auto list = [](std::initializer_list<uint64_t> bases) { CandidateBuilder c; for (uint64_t x : bases) c.add_struct(x); return c; };
    {   // cockpit -> chase -> cockpit with the guard: lock home, lose it in chase, the alias never locks, relock home in the cockpit
        FakeMem m; FakeBlock b; b.base = kBlk; m.blocks.push_back(b);
        Locator L; CapSink log; bool aliasLocked = false; double lostAt = -1;
        for (double ms = 0; ms < 14000; ms += 1000.0 / 60.0) {
            const bool chase = ms >= 3000 && ms < 10000;
            plant_pov1(m, kHome, ms / 1000.0, chase);
            if (L.wants_candidates(ms)) L.set_candidates(ms < 3000 ? list({kHome}) : chase ? list({kAlias}) : list({kHome, kAlias}), ms, log);
            TickOut o; L.tick(m, ms, log, o);
            CHECK(o.bytes <= 512 + kSampleBytes);
            if (L.locked_base() == kAlias) aliasLocked = true;
            if (lostAt < 0 && L.losses() > 0) lostAt = ms;
            if (ms > 2500 && ms < 3000) CHECK(L.locked_base() == kHome);
        }
        CHECK(!aliasLocked && L.losses() == 1 && lostAt > 3900 && lostAt < 4300);   // lose_ms 1 s after the switch
        CHECK(L.alias_refused() > 30 && L.locks() == 2 && L.locked_base() == kHome);
        L.maybe_summary(14000, log, "auto");
        CHECK(log.first("e6-locate;event=summary").find("alias-of-locked-base") != std::string::npos);
    }
    {   // control: a locator that never locked the home structure takes the alias (the hazard the guard removes)
        FakeMem m; FakeBlock b; b.base = kBlk; m.blocks.push_back(b);
        Locator L; CapSink log;
        for (double ms = 0; ms < 4000; ms += 1000.0 / 60.0) {
            plant_pov1(m, kHome, ms / 1000.0, true);
            if (L.wants_candidates(ms)) L.set_candidates(list({kAlias}), ms, log);
            TickOut o; L.tick(m, ms, log, o);
        }
        CHECK(L.locked_base() == kAlias && L.alias_refused() == 0);
    }
    {   // the structure really moves within the overlap span (+0x200, windows not overlapping) and its old place is cleared: the dead home
        // (aircraft-at-origin) never blocks the new base
        FakeMem m; FakeBlock b; b.base = kBlk; m.blocks.push_back(b);
        Locator L; CapSink log;
        for (double ms = 0; ms < 3000; ms += 1000.0 / 60.0) {
            plant_pov1(m, kHome, ms / 1000.0, false);
            if (L.wants_candidates(ms)) L.set_candidates(list({kHome}), ms, log);
            TickOut o; L.tick(m, ms, log, o);
        }
        CHECK(L.locked_base() == kHome);
        constexpr uint64_t kMoved = kHome + 0x200;
        CHECK(kMoved - kHome < kStructBytes);
        plant(m, kHome, Sample{});
        for (double ms = 3000; ms < 9000; ms += 1000.0 / 60.0) {
            plant_pov1(m, kMoved, ms / 1000.0, false);
            if (L.wants_candidates(ms)) L.set_candidates(list({kHome, kMoved}), ms, log);
            TickOut o; L.tick(m, ms, log, o);
        }
        CHECK(L.locked_base() == kMoved && L.losses() == 1 && L.alias_refused() == 0);
    }
}

// E6-JUNK-LOCK (2026-10-30, headset: the F-5E session stayed on the Draken). BEFORE: the scan kept the FIRST 12 plausible hits in address order, the real
// structure (+0xCA811C J35 / +0xCAAC6C F-5E) lay behind hundreds of junk hits and was dropped; the locks that happened were on world-origin junk
// (cam 0,0,0 rot 0 heading 0, air 0,0,60 / -0.2,22.4,25.1: logged locks of step133 Batch AG) whose samples never match an aircraft eye point.
static Sample origin_junk() {   // the logged lock 'air=0.00,0.00,60.00;heading=0;cam=0.00,0.00,0.00;rot=0;eye-dist=60.000'
    Sample s; s.air[2] = 60.0f; return s;
}
static Sample quiet_junk(int i) {   // plausible by check_sample, off the origin, static, all angles zero (the worst case for the old address-order cap)
    Sample s; s.air[0] = 100.0f + float(i); s.air[1] = 50.0f; s.air[2] = 1000.0f; s.cam[0] = s.air[0]; s.cam[1] = 50.0f; s.cam[2] = 964.0f; return s;
}
static void test_junk_lock() {
#ifndef OLD_API
    {   // the single-sample rule rejects the world-origin junk (it passed before: |air x|+|air z| = 60, eye 60 = the upper bound)
        CHECK(std::string(check_sample(origin_junk())) == "camera-at-origin");
        Sample b; b.air[0] = -0.2f; b.air[1] = 22.39f; b.air[2] = 25.11f;   // the other logged junk lock (eye 33.6, cam at the origin)
        CHECK(std::string(check_sample(b)) == "camera-at-origin");
        CHECK(check_sample(f5e(0)) == nullptr && check_sample(m03k_real(0)) == nullptr);
        CHECK(hit_score(m03k_real(0)) == 4 && hit_score(f5e(0)) == 4 && hit_score(quiet_junk(0)) == 2 && hit_score(m03k_junk()) == 1);
        uint64_t b4[3] = {1, 2, 3}; uint8_t sc[3] = {4, 1, 4};
        CHECK(!insert_best(b4, sc, 3, 9, 1) && !insert_best(b4, sc, 3, 9, 0) && b4[1] == 2);
        CHECK(insert_best(b4, sc, 3, 9, 3) && b4[1] == 9 && sc[1] == 3);
        CHECK(!insert_best(b4, sc, 0, 9, 4));
    }
#endif
    {   // buffer level: 300 quiet junk hits ahead of the real one, cap 12: the real structure is among the kept hits
        std::vector<uint8_t> buf(0x400000, 0);
        auto put = [&](size_t o, float f) { std::memcpy(buf.data() + o, &f, 4); };
        auto plant_buf = [&](size_t at, const Sample& s) {
            for (int i = 0; i < 3; ++i) { put(at + 4 * i, s.look[i]); put(at + 0x20 + 4 * i, s.air[i]); put(at + kWinGap + 4 * i, s.cam[i]); put(at + kWinGap + 0x10 + 4 * i, s.rot[i]); }
            put(at + 0x3C, s.heading);
        };
        for (int i = 0; i < 300; ++i) plant_buf(0x1000 + 0x1000 * size_t(i), quiet_junk(i));
        const size_t at = 0x3F0000 - 0x1000; plant_buf(at, f5e(0));
        uint64_t b[12] = {};
        const unsigned n = scan_struct(buf.data(), buf.size(), 0x100000, b, 12);
        CHECK(n >= 300);
        bool found = false; for (unsigned i = 0; i < 12; ++i) found = found || b[i] == 0x100000 + at - kWinA;
#ifdef OLD_API
        CHECK(!found);   // BEFORE: the cap kept the first 12 in address order; the real structure was lost
#else
        CHECK(found);    // AFTER: the best-scored hits are kept
#endif
    }
    {   // m03k layout + 300 quiet junk structures + the world-origin junk, scan on: the locator locks on the F-5E structure (not any junk) within 4 s
        M03k w;
        for (int i = 0; i < 300; ++i) plant_struct(w.m, M03k::kR + 0x100000 + 0x1000 * uint64_t(i) - kWinA, quiet_junk(i));
        plant_struct(w.m, M03k::kR + 0x2C0000 - kWinA, origin_junk());
        Locator L; CapSink log; unsigned valid = 0; uint64_t vb = 0;
        const double at = run_m03k(w, L, log, true, 9000, valid, vb);
#ifdef OLD_API
        CHECK(!(at > 0 && L.locked_base() == M03k::kStruct));   // BEFORE: junk hits fill the list, no lock on the real structure
#else
        CHECK(at > 0 && at < 4000 && L.locked_base() == M03k::kStruct && vb == M03k::kStruct && L.losses() == 0 && valid > 300);
        CHECK(log.first("event=locked").find("kind=s") != std::string::npos && log.first("event=locked").find("eye-dist=22.") != std::string::npos);
#endif
    }
}

static void test_fallback_gate() {
    // BEFORE: the shipped e6loc:auto fallback published any probe-base sample passing check_sample: the static junk did (the session's camera
    // for the rest of the flight). AFTER: the gate never admits a static structure, admits a moving consistent one after 8 samples / 3 moves.
    const uint64_t junk = 0x1c683093140ull, real = 0x1c75fc54860ull;
    CHECK(check_sample(m03k_junk()) == nullptr);   // the old rule would publish it
    FallbackGate g; unsigned admitted = 0; const char* last = nullptr;
    for (int i = 0; i < 600; ++i) { last = g.admit(junk, m03k_junk(), i * 16.7); if (!last) ++admitted; }
    CHECK(admitted == 0 && std::string(last) == "static" && g.qualified_base() == 0);
    // The real structure (moving): qualifies at the 8th consistent sample with >= 3 moves; base change resets it.
    int first = -1;
    for (int i = 0; i < 60; ++i) if (!g.admit(real, f5e(i / 60.0), i * 16.7) && first < 0) first = i;
    CHECK(first == 7 && g.qualified_base() == real);
    CHECK(g.admit(junk, m03k_junk(), 1100) != nullptr && g.qualified_base() == 0);   // the probe switched to the junk: withdrawn at once
    for (int i = 0; i < 8; ++i) g.admit(real, f5e(2 + i / 60.0), 1200 + i * 16.7);
    CHECK(g.qualified_base() == real);
    // Paused after qualifying: identical samples stay admitted; a teleport (jump) withdraws until 8 consistent samples again.
    for (int i = 0; i < 30; ++i) CHECK(g.admit(real, f5e(2.2), 1400 + i * 16.7) == nullptr);
    CHECK(std::string(g.admit(real, f5e(60.0), 1950)) == "jump" && g.qualified_base() == 0);
    // A single invalid sample (zeroed) withdraws; reset() clears.
    for (int i = 0; i < 10; ++i) g.admit(real, f5e(60 + i / 60.0), 2000 + i * 16.7);
    CHECK(g.qualified_base() == real);
    CHECK(std::string(g.admit(real, Sample{}, 2200)) == "aircraft-at-origin" && g.qualified_base() == 0);
    g.reset(); CHECK(g.qualified_base() == 0 && std::string(g.admit(0, f5e(0), 0)) == "no-base");
}

static void test_mailbox() {
    acz::terrain::CamMailbox mb; acz::terrain::CamSample cs;
    CHECK(!mb.try_read(cs));
    const float p[3] = {1, 2, 3};
    mb.publish(p, 0.1f, 0.2f, 0.3f, 0.4f);
    CHECK(mb.try_read(cs) && cs.campos[2] == 3 && cs.heading == 0.4f && cs.seq == 1);
    mb.invalidate(); CHECK(!mb.try_read(cs)); CHECK(mb.published() == 1);
    mb.publish(p, 0.5f, 0.2f, 0.3f, 0.4f); CHECK(mb.try_read(cs) && cs.pitch == 0.5f && cs.seq == 2);
    CHECK(cs.t_ns > 0);   // CAM-EPOCH: stamped with the steady clock when no time is given
}

int main() {
    test_option();
    test_real_samples();
    test_candidates();
    test_lock();
    test_locked_behaviour();
    test_ambiguous_and_look();
    test_many_candidates();
    test_real_m03k();
    test_scan_struct();
    test_relocated_lock();
    test_alias_guard();
    test_fallback_gate();
    test_junk_lock();
    test_mailbox();
    std::printf("e6_locator_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
