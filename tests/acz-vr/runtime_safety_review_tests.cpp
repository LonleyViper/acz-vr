// runtime_safety_review_tests.cpp -- adversarial offline tests from the overnight runtime-safety review (docs/review/overnight-runtime-review.md).
// No game process, no OpenXR runtime, no headset. Windows-only parts use REAL process memory of this test executable:
//   1. the SEH/VirtualQuery read primitives (ee_camera_guarded.h: range_ok + guarded_copy, the E6 locator's read path) against a thread that
//      decommits, re-commits and re-protects the pages being read (block freed / mission transition) - never a crash, never a wrong copy;
//   2. the xr_terrain_stage=auto scanner (stage_detect_win.cpp scan_process) against concurrent VirtualAlloc/VirtualFree churn, with a
//      PAGE_GUARD page and a PAGE_NOACCESS page that carry a planted signature (must NOT be read: the guard bit survives, the signature stays
//      unseen) and a PAGE_READONLY region that carries another one (must be seen, bytes and protection unchanged: read-only);
//   3. the E6 locator (ee_e6_locator.h) against a block that disappears while locked, torn samples (aircraft written, camera not yet),
//      and a randomized readability/content fuzz with the per-tick byte caps and the publication invariants checked on every tick;
//   4. AIRCRAFT-AUTO Detector under allocation failure (review R-1: noexcept + std::vector growth used to reach std::terminate).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "aircraft_auto.h"
#include "ee_camera_guarded.h"
#include "ee_e6_locator.h"
#include "stage_detect_win.h"
#include "stage_fingerprint.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::fflush(stdout); std::exit(1); } } while (0)

// ------------------------------------------------------------------------------------------------ allocation failure injection (test 4)
#if !defined(__SANITIZE_ADDRESS__)
static std::atomic<bool> g_fail_alloc{false};
void* operator new(size_t n) {
    if (g_fail_alloc.load(std::memory_order_relaxed)) throw std::bad_alloc();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
#define ACZ_ALLOC_INJECTION 1
#endif

// ------------------------------------------------------------------------------------------------ 1. guarded reads vs. a racing decommit
static void test_guarded_read_race() {
    constexpr size_t kPages = 16, kPage = 4096, kBytes = kPages * kPage;
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(base);
    for (size_t i = 0; i < kBytes; ++i) base[i] = uint8_t(0x5A);
    std::atomic<bool> stop{false};
    std::atomic<unsigned> flips{0};
    std::thread churn([&] {
        std::mt19937 rng(1234);
        while (!stop.load(std::memory_order_relaxed)) {
            const size_t pg = rng() % kPages;
            uint8_t* p = base + pg * kPage;
            switch (rng() % 3) {
            case 0:   // freed (decommitted), then committed again (zero-filled -> refilled)
                VirtualFree(p, kPage, MEM_DECOMMIT);
                VirtualAlloc(p, kPage, MEM_COMMIT, PAGE_READWRITE);
                std::memset(p, 0x5A, kPage);
                break;
            case 1: { DWORD old; VirtualProtect(p, kPage, PAGE_NOACCESS, &old); VirtualProtect(p, kPage, PAGE_READWRITE, &old); break; }
            default: { DWORD old; VirtualProtect(p, kPage, PAGE_READONLY, &old); VirtualProtect(p, kPage, PAGE_READWRITE, &old); break; }
            }
            flips.fetch_add(1, std::memory_order_relaxed);
        }
    });
    unsigned ok = 0, refused = 0, faulted = 0;
    uint8_t out[92];
    std::mt19937 rng(99);
    for (unsigned it = 0; it < 200000; ++it) {
        const size_t off = rng() % (kBytes - sizeof out);
        const uint64_t a = uint64_t(uintptr_t(base + off));
        if (!acz::ee_camera::range_ok(a, sizeof out)) { ++refused; continue; }      // the gate (TOCTOU window follows)
        std::memset(out, 0, sizeof out);
        if (!acz::ee_camera::guarded_copy(out, reinterpret_cast<const void*>(uintptr_t(a)), sizeof out)) { ++faulted; continue; }   // SEH net
        ++ok;
        for (uint8_t b : out) CHECK(b == 0x5A || b == 0);   // a copy is the page content (0 only from a just re-committed page)
    }
    stop = true; churn.join();
    std::printf("guarded-read race: ok=%u refused-by-query=%u faulted-after-query=%u churn=%u\n", ok, refused, faulted, flips.load());
    CHECK(ok > 0 && flips.load() > 100);
    // Overflow / empty ranges are refused without a query.
    CHECK(!acz::ee_camera::range_ok(~uint64_t(0) - 8, 92));
    CHECK(!acz::ee_camera::range_ok(uint64_t(uintptr_t(base)), 0));
    // A fully released block: refused, and a copy from it is caught instead of crashing.
    VirtualFree(base, 0, MEM_RELEASE);
    CHECK(!acz::ee_camera::range_ok(uint64_t(uintptr_t(base)), 92));
    CHECK(!acz::ee_camera::guarded_copy(out, base, sizeof out));
}

// ------------------------------------------------------------------------------------------------ 2. the stage scanner on real memory
// Masked signatures (XOR kMask) so the raw window bytes exist ONLY where the test plants them (never in a stack/heap temporary).
static void masked_sig(uint8_t out[32], uint8_t seed) { for (int i = 0; i < 32; ++i) out[i] = uint8_t((seed * 37 + i * 11 + (i * i) * 3) ^ 0x6C); }
static void plant_raw(uint8_t* dst, const uint8_t masked[32]) { for (int i = 0; i < 32; ++i) dst[i] = uint8_t(masked[i] ^ acz::stagefp::kMask); }
static std::string hex_of_masked(const uint8_t masked[32]) {
    // The JSON carries the RAW bytes as hex text: a hex string never equals the raw bytes, so the table text cannot self-match.
    std::string s; char h[3];
    for (int i = 0; i < 32; ++i) { std::snprintf(h, sizeof h, "%02x", unsigned(uint8_t(masked[i] ^ acz::stagefp::kMask))); s += h; }
    return s;
}
static void test_stage_scan_real_memory() {
    using namespace acz::stagefp;
    uint8_t mRo[32], mGuard[32], mNoAcc[32];
    masked_sig(mRo, 1); masked_sig(mGuard, 2); masked_sig(mNoAcc, 3);
    const std::string json = std::string("{\"format\":\"acz-stage-fingerprints\",\"version\":1,\"window\":32,\"min_hits\":1,") +
        "\"groups\":[{\"stage\":\"ro\",\"members\":[\"ro\"]},{\"stage\":\"guard\",\"members\":[\"guard\"]},{\"stage\":\"noacc\",\"members\":[\"noacc\"]}]," +
        "\"sigs\":[{\"hex\":\"" + hex_of_masked(mRo) + "\",\"groups\":[0],\"src\":\"ro\"},{\"hex\":\"" + hex_of_masked(mGuard) + "\",\"groups\":[1],\"src\":\"guard\"}," +
        "{\"hex\":\"" + hex_of_masked(mNoAcc) + "\",\"groups\":[2],\"src\":\"noacc\"}]}";
    Table t; std::string why;
    CHECK(parse_table(json.data(), json.size(), t, why));
    // Regions: [RO 64 KiB with sig 0] [guard page with sig 1] [NOACCESS page with sig 2]
    auto* ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* gp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* na = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(ro && gp && na);
    std::memset(ro, 0x11, 65536); plant_raw(ro + 40000, mRo);
    std::memset(gp, 0x22, 4096); plant_raw(gp + 1000, mGuard);
    std::memset(na, 0x33, 4096); plant_raw(na + 2000, mNoAcc);
    uint64_t sumRo = 0; for (size_t i = 0; i < 65536; ++i) sumRo = sumRo * 131 + ro[i];
    DWORD old;
    CHECK(VirtualProtect(ro, 65536, PAGE_READONLY, &old));
    CHECK(VirtualProtect(gp, 4096, PAGE_READWRITE | PAGE_GUARD, &old));
    CHECK(VirtualProtect(na, 4096, PAGE_NOACCESS, &old));
    // Concurrent churn: regions appear and disappear while the scan walks (mission transition: scenes unload).
    std::atomic<bool> stop{false};
    std::thread churn([&] {
        std::vector<void*> live;
        std::mt19937 rng(7);
        while (!stop.load(std::memory_order_relaxed)) {
            if (live.size() < 64 && (rng() & 1)) {
                const size_t sz = size_t(1 + rng() % 64) * 65536;
                if (void* p = VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) { std::memset(p, 0x44, 4096); live.push_back(p); }
            } else if (!live.empty()) {
                const size_t k = rng() % live.size();
                VirtualFree(live[k], 0, MEM_RELEASE); live.erase(live.begin() + std::ptrdiff_t(k));
            }
        }
        for (void* p : live) VirtualFree(p, 0, MEM_RELEASE);
    });
    unsigned seenRo = 0;
    for (int pass = 0; pass < 5; ++pass) {
        Matcher m(t); ScanStats st;
        scan_process(m, ScanLimits{}, st);
        CHECK(!st.truncated && st.regions > 0 && st.bytes > 0);
        CHECK(m.seen()[1] == 0);   // guard page never read
        CHECK(m.seen()[2] == 0);   // no-access page never read
        seenRo += m.seen()[0];
        std::printf("stage scan pass %d: regions=%u/%u bytes=%llu nonresident-skipped=%llu stop=%s ms=%u read-failures=%u skipped-image=%u sigs-seen=%u\n", pass,
            st.regions, st.regions_eligible, static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.bytes_nonresident), st.stop, st.ms,
            st.read_failures, st.regions_skipped_image, unsigned(m.seen()[0] + m.seen()[1] + m.seen()[2]));
        CHECK(st.ws_query_failures == 0);   // QueryWorkingSetEx works for this process
    }
    stop = true; churn.join();
    CHECK(seenRo == 5);            // the read-only region is read every pass
    // Read-only guarantee: protections and contents untouched; the guard bit is still armed (a touch would have consumed it).
    MEMORY_BASIC_INFORMATION q{};
    CHECK(VirtualQuery(gp, &q, sizeof q) == sizeof q && (q.Protect & PAGE_GUARD) != 0);
    CHECK(VirtualQuery(na, &q, sizeof q) == sizeof q && q.Protect == PAGE_NOACCESS);
    CHECK(VirtualQuery(ro, &q, sizeof q) == sizeof q && q.Protect == PAGE_READONLY);
    uint64_t sumRo2 = 0; for (size_t i = 0; i < 65536; ++i) sumRo2 = sumRo2 * 131 + ro[i];
    CHECK(sumRo2 == sumRo);
    // Truncation caps hold (tiny byte budget, then a zero time budget).
    { Matcher m(t); ScanStats st; ScanLimits lim; lim.max_bytes = 1; scan_process(m, lim, st); CHECK(st.truncated && st.bytes <= (4u << 20)); }
    { Matcher m(t); ScanStats st; ScanLimits lim; lim.max_ms = 0; scan_process(m, lim, st); CHECK(st.truncated); }
    VirtualFree(ro, 0, MEM_RELEASE); VirtualProtect(gp, 4096, PAGE_READWRITE, &old); VirtualFree(gp, 0, MEM_RELEASE); VirtualFree(na, 0, MEM_RELEASE);
}

// ------------------------------------------------------------------------------------------------ 3. E6 locator adversarial
namespace e6 {
using namespace acz::e6loc;
struct Mem final : ReadMem {
    struct Blk { uint64_t base; bool readable = true; std::vector<uint8_t> a = std::vector<uint8_t>(kWinABytes), b = std::vector<uint8_t>(kWinBBytes); };
    std::vector<Blk> blocks;
    uint64_t bytes = 0;
    bool read(uint64_t addr, void* out, uint32_t n) noexcept override {
        for (auto& k : blocks) {
            if (!k.readable) continue;
            if (addr == k.base + kWinA && n == kWinABytes) { std::memcpy(out, k.a.data(), n); bytes += n; return true; }
            if (addr == k.base + kWinB && n == kWinBBytes) { std::memcpy(out, k.b.data(), n); bytes += n; return true; }
        }
        return false;
    }
};
void put(Mem::Blk& k, const Sample& s) {
    auto f = [](std::vector<uint8_t>& v, size_t off, float x) { std::memcpy(v.data() + off, &x, 4); };
    for (int i = 0; i < 3; ++i) { f(k.a, 4 * i, s.look[i]); f(k.a, 0x20 + 4 * i, s.air[i]); f(k.b, 4 * i, s.cam[i]); f(k.b, 0x10 + 4 * i, s.rot[i]); }
    f(k.a, 0x3C, s.heading);
}
Sample flight(double t, double x0 = -21140.0, double z0 = 128801.0) {
    const double h = -0.7853982, sp = 1400.0;
    Sample s;
    const double fw[3] = {-std::sin(h), 0.0, -std::cos(h)};
    for (int i = 0; i < 3; ++i) { const double air = (i == 0 ? x0 : i == 1 ? 4000.0 : z0) + fw[i] * sp * t; s.air[i] = float(air); s.cam[i] = float(air + 36.3 * fw[i] + (i == 1 ? 3.575 : 0.0)); }
    s.heading = float(h); s.rot[1] = float(h);
    return s;
}
struct NullSink final : Sink { unsigned lost = 0, locked = 0; void line(bool, const char* t) noexcept override { if (std::strstr(t, "event=lost")) ++lost; if (std::strstr(t, "event=locked")) ++locked; } };
CandidateBuilder cands(const Mem& m) {
    CandidateBuilder b;
    for (auto& k : m.blocks) { RegionDesc r; r.base = k.base; r.size = kBlockBytes; r.alloc = k.base; r.committed = r.is_private = r.read_write = true; b.add(r); }
    b.finish(); return b;
}
} // namespace e6

static void test_e6_block_disappears_while_locked() {
    using namespace e6;
    Mem m; m.blocks.push_back({0x10000000}); Locator L; NullSink log;
    double ms = 0; const double dt = 1000.0 / 60.0;
    for (; ms < 3000 && L.state() != Locator::locked; ms += dt) {
        put(m.blocks[0], flight(ms / 1000));
        if (L.wants_candidates(ms)) L.set_candidates(cands(m), ms, log);
        TickOut o; L.tick(m, ms, log, o);
    }
    CHECK(L.state() == Locator::locked);
    // Mission end: the block is freed (every read fails). Publications stop at once; 'hold' only inside hold_ms; lost at lose_ms.
    const double gone = ms;
    unsigned holds = 0, valids = 0;
    for (; ms < gone + 1500; ms += dt) {
        m.blocks[0].readable = false;
        if (L.wants_candidates(ms)) L.set_candidates(cands(m), ms, log);
        TickOut o; L.tick(m, ms, log, o);
        valids += o.valid; holds += o.hold;
        if (o.hold) CHECK(ms - gone < 250 + dt);
        CHECK(o.bytes <= 512);
    }
    CHECK(valids == 0 && holds > 0 && log.lost == 1 && L.state() == Locator::searching && L.locked_base() == 0);
    // The block comes back with a DIFFERENT, all-zero content at the same address (reused allocation): never locks, never publishes.
    m.blocks[0].readable = true; m.blocks[0].a.assign(kWinABytes, 0); m.blocks[0].b.assign(kWinBBytes, 0);
    for (double end = ms + 3000; ms < end; ms += dt) {
        if (L.wants_candidates(ms)) L.set_candidates(cands(m), ms, log);
        TickOut o; L.tick(m, ms, log, o); CHECK(!o.valid);
    }
    CHECK(L.state() == Locator::searching && log.locked == 1);
    // ... and the new mission's real structure relocks.
    for (double end = ms + 3000; ms < end && L.state() != Locator::locked; ms += dt) {
        put(m.blocks[0], flight(ms / 1000, 50000.0, -40000.0));
        if (L.wants_candidates(ms)) L.set_candidates(cands(m), ms, log);
        TickOut o; L.tick(m, ms, log, o);
    }
    CHECK(L.state() == Locator::locked && L.locks() == 2);
}

static void test_e6_torn_sample() {
    using namespace e6;
    Mem m; m.blocks.push_back({0x20000000}); Locator L; NullSink log;
    double ms = 0; const double dt = 1000.0 / 60.0;
    for (; ms < 3000 && L.state() != Locator::locked; ms += dt) {
        put(m.blocks[0], flight(ms / 1000));
        if (L.wants_candidates(ms)) L.set_candidates(cands(m), ms, log);
        TickOut o; L.tick(m, ms, log, o);
    }
    CHECK(L.state() == Locator::locked);
    // The game writes the aircraft position 2 s ahead while the camera window still holds the old frame (a torn read across the two windows).
    Sample cur = flight(ms / 1000), fut = flight(ms / 1000 + 2.0);
    Mem::Blk& k = m.blocks[0]; put(k, cur);
    std::vector<uint8_t> keepB = k.b; put(k, fut); k.b = keepB;
    TickOut o; L.tick(m, ms, log, o);
    CHECK(!o.valid);   // eye-distance / jump: rejected, the torn sample is never published
}

static void test_e6_fuzz() {
    using namespace e6;
    Mem m;
    for (unsigned i = 0; i < 20; ++i) m.blocks.push_back({0x100000000ull + i * 0x10000000ull});   // more than the candidate cap
    Locator L; NullSink log; std::mt19937 rng(42);
    std::uniform_real_distribution<float> any(-3.0e5f, 3.0e5f);
    double ms = 0; const double dt = 1000.0 / 60.0;
    unsigned valid = 0;
    for (unsigned it = 0; it < 60000; ++it, ms += dt) {
        for (unsigned i = 0; i < m.blocks.size(); ++i) {
            auto& k = m.blocks[i];
            if (rng() % 200 == 0) k.readable = !k.readable;
            if (i == 3) put(k, flight(ms / 1000));                       // one real structure (may flicker unreadable)
            else if (rng() % 4 == 0) { Sample s; for (float* f : {s.look, s.air, s.cam, s.rot}) for (int j = 0; j < 3; ++j) f[j] = (rng() % 3) ? any(rng) : std::nanf(""); s.heading = any(rng); put(k, s); }
        }
        if (rng() % 500 == 0) L.set_candidates(cands(m), ms, log);      // a list can arrive in any state
        else if (L.wants_candidates(ms)) L.set_candidates(cands(m), ms, log);
        TickOut o; L.tick(m, ms, log, o);
        CHECK(o.bytes <= 512);
        CHECK(L.candidates() <= kCandidateCap);
        if (o.valid) { ++valid; CHECK(check_sample(o.s) == nullptr); CHECK(L.state() == Locator::locked && o.base == L.locked_base()); }
        if (o.hold) CHECK(L.state() == Locator::locked && !o.valid);
    }
    CHECK(L.max_tick_bytes() <= 512);
    std::printf("e6 fuzz: ticks=60000 valid=%u locks=%llu losses=%llu rejected=%llu\n", valid, (unsigned long long)L.locks(),
        (unsigned long long)L.losses(), (unsigned long long)L.rejected_samples());
}

// ------------------------------------------------------------------------------------------------ 4. Detector under allocation failure
static void test_detector_alloc_failure() {
#ifdef ACZ_ALLOC_INJECTION
    using namespace acz::aircraft;
    const float eye[3] = {0.0f, 3.575f, -36.3f};
    {   // feed(): the first centred sample grows the buffer -> bad_alloc inside a noexcept function (was std::terminate).
        Detector d;
        g_fail_alloc = true;
        const bool done = d.feed(eye, true, 1);
        g_fail_alloc = false;
        CHECK(!done && d.current() == -1);
        for (unsigned i = 0; i < d.options().window; ++i) (void)d.feed(eye, true, 1);   // recovers: the next full window decides
        CHECK(d.current() == find_type("j35j"));
    }
    {   // evaluate(): the median scratch vector.
        std::vector<float> xyz;
        for (int i = 0; i < 24; ++i) xyz.insert(xyz.end(), eye, eye + 3);
        g_fail_alloc = true;
        const WindowResult w = Detector::evaluate(xyz, DetectOptions{});
        g_fail_alloc = false;
        CHECK(w.verdict == Verdict::unstable && w.samples == 24);
        CHECK(Detector::evaluate(xyz, DetectOptions{}).verdict == Verdict::match);   // valid path unchanged
    }
#else
    std::printf("detector allocation-failure test skipped (ASan build)\n");
#endif
}

int main() {
    test_guarded_read_race();
#if !defined(__SANITIZE_ADDRESS__)
    test_stage_scan_real_memory();
#else
    // The whole-process scan deliberately reads every committed readable region, including ASan's own poisoned stack/heap redzones, which
    // ASan reports as stack-buffer-underflow inside the scanner's memcpy (an instrumentation artefact, not a defect): skipped under ASan.
    std::printf("stage scan real-memory test skipped (ASan build: the scanner reads ASan redzones by design)\n");
#endif
    test_e6_block_disappears_while_locked();
    test_e6_torn_sample();
    test_e6_fuzz();
    test_detector_alloc_failure();
    std::printf("runtime_safety_review: %u checks passed\n", g_checks);
    return 0;
}
