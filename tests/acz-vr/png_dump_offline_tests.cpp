// png_dump_offline_tests.cpp -- PERF (docs/world-render/perf-impl.md): the xr_png_once write path's memory. A counting global operator new/delete
// measures the heap of (1) png_stream against png_encode (byte identity, peak), (2) the sequential writer queue (png_dump_queue.h) for the three
// 2400x1350 images of one capture against a model of the previous path (a copy per image + png_encode on three concurrent threads).
// No GPU, no files: the sinks write into counters / an in-memory hash.
#include "gpu_copy_step3.h"
#include "png_dump_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

// ------------------------------------------------------------------ counting allocator (all threads)
namespace {
std::atomic<int64_t> g_cur{0}, g_peak{0};
std::atomic<uint64_t> g_allocs{0};
constexpr size_t kHdr = 16;
void note(int64_t d) {
    const int64_t now = g_cur.fetch_add(d) + d;
    int64_t p = g_peak.load();
    while (now > p && !g_peak.compare_exchange_weak(p, now)) {}
}
void* counted_alloc(size_t n) {
    void* raw = std::malloc(n + kHdr);
    if (!raw) throw std::bad_alloc();
    std::memcpy(raw, &n, sizeof n);
    note(int64_t(n)); ++g_allocs;
    return static_cast<char*>(raw) + kHdr;
}
void counted_free(void* p) noexcept {
    if (!p) return;
    void* raw = static_cast<char*>(p) - kHdr;
    size_t n; std::memcpy(&n, raw, sizeof n);
    note(-int64_t(n));
    std::free(raw);
}
} // namespace
void* operator new(size_t n) { return counted_alloc(n); }
void* operator new[](size_t n) { return counted_alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return counted_alloc(n); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return counted_alloc(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace s3 = acz::gpu_copy::step3;
namespace pd = acz::png_dump;

// Peak heap ABOVE the level at the start of the measured region.
struct Region {
    int64_t base;
    Region() : base(g_cur.load()) { g_peak.store(base); }
    int64_t peak() const { return g_peak.load() - base; }
    int64_t now() const { return g_cur.load() - base; }
};

static std::vector<uint8_t> image(uint32_t w, uint32_t h, uint32_t seed) {
    std::vector<uint8_t> v(size_t(w) * h * 4);
    uint32_t r = seed;
    for (auto& b : v) { r = r * 1664525u + 1013904223u; b = uint8_t(r >> 24); }
    return v;
}
static std::vector<uint8_t> streamed(const uint8_t* p, uint32_t w, uint32_t h, uint64_t pitch, bool bgra, size_t buf = size_t(64) << 10) {
    std::vector<uint8_t> out;
    out.reserve(size_t(s3::png_stored_size(w, h)));
    const bool ok = s3::png_stream(p, w, h, pitch, bgra, [&](const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); return true; }, buf);
    if (!ok) out.clear();
    return out;
}

static void test_identity() {
    // sizes: 1x1, odd widths, exactly one stored block (raw 65535 = (64*4+1)*255), exactly two blocks, a padded pitch, BGRA, the 2400x1350 eye
    struct S { uint32_t w, h, pad; bool bgra; size_t buf; };
    const S cases[] = {{1, 1, 0, false, 64}, {3, 7, 0, true, 64}, {64, 255, 0, false, 4096}, {64, 510, 12, false, 65536}, {257, 300, 0, true, 100},
                       {2400, 1350, 0, false, size_t(64) << 10}, {2400, 1350, 256, true, size_t(1) << 20}};
    for (const S& c : cases) {
        const uint64_t pitch = uint64_t(c.w) * 4 + c.pad;
        std::vector<uint8_t> src(size_t(pitch) * c.h);
        uint32_t r = c.w * 31 + c.h;
        for (auto& b : src) { r = r * 1664525u + 1013904223u; b = uint8_t(r >> 24); }
        std::vector<uint8_t> ref;
        CHECK(s3::png_encode(src.data(), c.w, c.h, pitch, c.bgra, ref));
        const std::vector<uint8_t> got = streamed(src.data(), c.w, c.h, pitch, c.bgra, c.buf);
        CHECK(!got.empty() && got == ref);
        CHECK(uint64_t(ref.size()) == s3::png_stored_size(c.w, c.h));
    }
    // refusals and a failing writer
    const std::vector<uint8_t> px = image(4, 4, 1);
    auto sink = [](const uint8_t*, size_t) { return true; };
    CHECK(!s3::png_stream(px.data(), 0, 4, 16, false, sink));
    CHECK(!s3::png_stream(px.data(), 4, 4, 15, false, sink));
    CHECK(!s3::png_stream(nullptr, 4, 4, 16, false, sink));
    int calls = 0;
    CHECK(!s3::png_stream(px.data(), 4, 4, 16, false, [&](const uint8_t*, size_t) { return ++calls < 2; }, 64));
    std::printf("identity: png_stream == png_encode for %zu cases\n", sizeof cases / sizeof cases[0]);
}

static void test_peaks() {
    const uint32_t w = 2400, h = 1350;
    const size_t img = size_t(w) * h * 4;
    const std::vector<uint8_t> px = image(w, h, 7);
    // png_encode (the previous path, per image)
    int64_t enc_peak = 0;
    {
        Region r;
        std::vector<uint8_t> file;
        CHECK(s3::png_encode(px.data(), w, h, uint64_t(w) * 4, false, file));
        enc_peak = r.peak();
    }
    // png_stream into a counting writer (the bytes are hashed, not kept)
    int64_t st_peak = 0; uint64_t bytes = 0, crc = 0;
    {
        Region r;
        CHECK(s3::png_stream(px.data(), w, h, uint64_t(w) * 4, false, [&](const uint8_t* p, size_t n) { bytes += n; crc = s3::crc32(p, n, uint32_t(crc)); return true; }));
        st_peak = r.peak();
    }
    std::printf("2400x1350 image %.1f MB: png_encode peak heap %.1f MB (%.2f x image), png_stream peak heap %.1f KB\n",
        double(img) / 1e6, double(enc_peak) / 1e6, double(enc_peak) / double(img), double(st_peak) / 1e3);
    CHECK(bytes == s3::png_stored_size(w, h));
    CHECK(enc_peak > int64_t(2 * img));             // the model of the old path: several copies of the image
    CHECK(st_peak < int64_t(256) << 10);            // HARD bound: one row + the 64 KiB buffer, independent of the image size
}

// The whole png_once hand-over for one capture: three images (as png_take returns them) handed to the sink.
static void test_queue() {
    const uint32_t w = 2400, h = 1350;
    const size_t img = size_t(w) * h * 4;
    std::vector<uint8_t> a = image(w, h, 1), b = image(w, h, 2), c = image(w, h, 3);
    std::vector<uint8_t> ref[3];
    CHECK(s3::png_encode(a.data(), w, h, uint64_t(w) * 4, false, ref[0]) && s3::png_encode(b.data(), w, h, uint64_t(w) * 4, false, ref[1]) &&
          s3::png_encode(c.data(), w, h, uint64_t(w) * 4, false, ref[2]));
    std::vector<uint32_t> want; for (auto& f : ref) want.push_back(s3::crc32(f.data(), f.size()));
    std::vector<uint32_t> got; std::vector<std::string> names;
    std::atomic<int> active{0}, max_active{0};
    pd::Queue q([&](const pd::Job& j) {
        const int now = ++active; int m = max_active.load(); while (now > m && !max_active.compare_exchange_weak(m, now)) {}
        uint32_t crc = 0;
        const bool ok = s3::png_stream(j.rgba.data(), j.w, j.h, uint64_t(j.w) * 4, false, [&](const uint8_t* p, size_t n) { crc = s3::crc32(p, n, crc); return true; });
        got.push_back(crc); names.push_back(j.name);
        --active;
        return ok;
    });
    // free the reference files first: the measured region starts with exactly the three images alive (what the consumer holds after png_take)
    for (auto& f : ref) std::vector<uint8_t>().swap(f);
    Region r;
    CHECK(q.push("x-final.png", a, w, h) && q.push("x-alpha.png", b, w, h) && q.push("x-worldfx.png", c, w, h));
    CHECK(a.empty() && b.empty() && c.empty());          // taken, not copied
    CHECK(q.wait_idle(60000));
    const int64_t peak = r.peak(), after = r.now();
    std::printf("queue: 3 x %.1f MB images, peak heap above the three images %.1f KB, after %.1f MB (images freed), max concurrent writers %d\n",
        double(img) / 1e6, double(peak) / 1e3, double(after) / 1e6, max_active.load());
    CHECK(q.written() == 3 && q.failed() == 0 && max_active.load() == 1);
    CHECK(got == want);                                   // file bytes unchanged
    CHECK(names.size() == 3 && names[0] == "x-final.png" && names[2] == "x-worldfx.png");   // push order
    CHECK(peak < int64_t(1) << 20);                       // HARD: no copy of any image, < 1 MiB of extra heap
    CHECK(after <= -int64_t(3 * img) + (int64_t(64) << 10));   // all three images released by the writer
    // the previous path, modelled: a copy per sink call + png_encode on three concurrent threads
    {
        std::vector<uint8_t> a2 = image(w, h, 1), b2 = image(w, h, 2), c2 = image(w, h, 3);
        Region o;
        std::vector<std::thread> t;
        for (std::vector<uint8_t>* v : {&a2, &b2, &c2}) {
            std::vector<uint8_t> copy = *v;   // the lambda capture copy (one per image)
            t.emplace_back([copy = std::move(copy)] { std::vector<uint8_t> file; s3::png_encode(copy.data(), 2400, 1350, 2400 * 4, false, file); });
        }
        for (auto& x : t) x.join();
        std::printf("previous path (model): peak heap above the three images %.1f MB (%.1f x image)\n", double(o.peak()) / 1e6, double(o.peak()) / double(img));
        CHECK(o.peak() > int64_t(3 * img));
    }
    // refusals: size cap / inconsistent size; nothing is taken
    std::vector<uint8_t> small(16);
    CHECK(!q.push("bad.png", small, 3, 1) && small.size() == 16);
    CHECK(!pd::admissible(size_t(9000) * 10 * 4, 9000, 10) && !pd::admissible(0, 0, 0) && pd::admissible(img, w, h));
    CHECK(q.pushed() == 3);
}

// SD-3: push() success = queued only; the writer's final outcome reaches the result callback (one per job), also for a failing writer.
static void test_result_callback() {
    std::vector<std::pair<std::string, bool>> results;   // worker thread only; read after wait_idle
    pd::Queue q([](const pd::Job& j) { return j.name != "bad.png"; }, [&](const pd::Job& j, bool ok) { results.emplace_back(j.name, ok); });
    std::vector<uint8_t> a(2 * 2 * 4, 1), b(2 * 2 * 4, 2), c(2 * 2 * 4, 3);
    CHECK(q.push("good.png", a, 2, 2));
    CHECK(q.push("bad.png", b, 2, 2));        // accepted (queued) ...
    CHECK(b.empty());                          // ... the pixels were taken
    CHECK(q.push("good2.png", c, 2, 2));
    CHECK(q.wait_idle(60000));
    CHECK(results.size() == 3 && results[0] == std::make_pair(std::string("good.png"), true) &&
          results[1] == std::make_pair(std::string("bad.png"), false) && results[2] == std::make_pair(std::string("good2.png"), true));   // ... then failed
    CHECK(q.written() == 2 && q.failed() == 1 && q.pushed() == 3);
    // a throwing writer and a throwing result callback never kill the worker
    pd::Queue q2([](const pd::Job&) -> bool { throw 1; }, [](const pd::Job&, bool) { throw 2; });
    std::vector<uint8_t> d(2 * 2 * 4, 1), e(2 * 2 * 4, 1);
    CHECK(q2.push("x.png", d, 2, 2) && q2.push("y.png", e, 2, 2) && q2.wait_idle(60000) && q2.failed() == 2);
}

int main() {
    test_result_callback();
    test_identity();
    test_peaks();
    test_queue();
    std::printf("png_dump_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
