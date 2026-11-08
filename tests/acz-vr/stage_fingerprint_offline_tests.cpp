// stage_fingerprint_offline_tests.cpp -- STAGES (xr_terrain_stage=auto) offline tests, no game data: the fingerprints.json parser (the
// Python-built synthetic fixture + refusal cases) and the Matcher fed with FAKE byte streams (signatures embedded in noise, chunked feeds
// with windows across chunk boundaries, separate streams, identity groups, family signatures, ties, min-hits, self-scan safety).
// The Windows memory walk (stage_detect_win.cpp) is glue only; docs/world-render/stages-and-detection.md section 3.
#include "stage_fingerprint.h"
#include "stage_scan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace acz::stagefp;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

static std::string read_text(const char* p) {
    std::string s; FILE* f = std::fopen(p, "rb"); if (!f) return s;
    char b[4096]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, r);
    std::fclose(f); return s;
}
struct Lcg { uint64_t s; explicit Lcg(uint64_t v) : s(v) {} uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); } };
static std::vector<uint8_t> noise(size_t n, uint64_t seed) { Lcg r(seed); std::vector<uint8_t> v(n); for (auto& b : v) b = uint8_t(r.next()); return v; }
static std::string hexs(const uint8_t* p, size_t n) { static const char* d = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
static std::vector<uint8_t> raw(const Sig& s) { std::vector<uint8_t> v(kWindow); for (uint32_t i = 0; i < kWindow; ++i) v[i] = uint8_t(s.masked[i] ^ kMask); return v; }
static bool parse(const std::string& j, Table& t, std::string& why) { return parse_table(j.data(), j.size(), t, why); }

// ---- synthetic table: groups 0 = {a1,a2}, 1 = {b1}, 2 = {c1}; sigs: 5 A-only, 5 B-only, 6 A+B family, 2 C-only; min_hits 3
struct Synth { std::string json; std::vector<std::vector<uint8_t>> a, b, ab, c; };
static Synth make_synth(uint32_t min_hits = 3) {
    Synth s; Lcg r(99);
    auto win = [&] { std::vector<uint8_t> w(kWindow); for (auto& x : w) x = uint8_t(r.next()); return w; };
    for (int i = 0; i < 5; ++i) s.a.push_back(win());
    for (int i = 0; i < 5; ++i) s.b.push_back(win());
    for (int i = 0; i < 6; ++i) s.ab.push_back(win());
    for (int i = 0; i < 2; ++i) s.c.push_back(win());
    std::string sigs;
    auto add = [&](const std::vector<uint8_t>& w, const char* groups, const char* src) {
        if (!sigs.empty()) sigs += ",";
        sigs += std::string("{\"src\":\"") + src + "\",\"stage\":\"a1\",\"off\":0,\"groups\":[" + groups + "],\"hex\":\"" + hexs(w.data(), w.size()) + "\"}";
    };
    for (auto& w : s.a) add(w, "0", "mapset");
    for (auto& w : s.b) add(w, "1", "sph");
    for (auto& w : s.ab) add(w, "0,1", "mhd");
    for (auto& w : s.c) add(w, "2", "mtc");
    s.json = "{\"format\":\"acz-stage-fingerprints\",\"version\":1,\"window\":32,\"min_hits\":" + std::to_string(min_hits) +
             ",\"groups\":[{\"stage\":\"a1\",\"members\":[\"a1\",\"a2\"],\"zvrs_sha256\":\"ab\"},{\"stage\":\"b1\",\"members\":[\"b1\"]},"
             "{\"stage\":\"c1\",\"members\":[\"c1\"],\"zvrs_sha256\":\"\"}],\"sigs\":[" + sigs + "]}";
    return s;
}
// noise with the given windows embedded at scattered, unaligned offsets
static std::vector<uint8_t> stream_with(const std::vector<std::vector<uint8_t>>& ws, size_t n, uint64_t seed) {
    std::vector<uint8_t> v = noise(n, seed);
    Lcg r(seed + 1);
    size_t step = n / (ws.size() + 1);
    for (size_t i = 0; i < ws.size(); ++i) {
        const size_t o = (i + 1) * step - 7 + (r.next() % 13);
        std::memcpy(&v[o], ws[i].data(), kWindow);
    }
    return v;
}
static Result run(const Table& t, const std::vector<uint8_t>& v, size_t chunk = 0, uint64_t seed = 3) {
    Matcher m(t);
    m.begin_stream();
    if (!chunk) m.feed(v.data(), v.size());
    else {
        Lcg r(seed); size_t o = 0;
        while (o < v.size()) { size_t c = 1 + r.next() % chunk; if (c > v.size() - o) c = v.size() - o; m.feed(v.data() + o, c); o += c; }
    }
    m.end_stream();
    CHECK(m.bytes_fed() == v.size());
    return m.decide();
}

static void test_fixture() {
    const std::string j = read_text(ACZ_STAGEFP_FIXTURE);
    CHECK(!j.empty());
    Table t; std::string why;
    CHECK(parse(j, t, why));
    CHECK(t.groups.size() == 3 && t.sigs.size() == 32 && t.window == 32 && t.min_hits == 3);
    CHECK(t.groups[0].stage == "aa1" && t.groups[0].members.size() == 2 && t.groups[0].members[1] == "aa2");
    // every group's own signatures, embedded in noise, select that group (fixture built by stage_fingerprints.py --fixture)
    for (int g = 0; g < 3; ++g) {
        std::vector<std::vector<uint8_t>> own;
        for (const auto& s : t.sigs) if (s.groups.size() == 1 && s.groups[0] == g) own.push_back(raw(s));
        CHECK(own.size() >= 4);
        const Result r = run(t, stream_with(own, 1 << 16, 10 + g), 4096);
        CHECK(r.kind == Result::Kind::match && r.group == g && r.stage == t.groups[size_t(g)].stage && r.best == own.size());
    }
    // the shared-heights family signatures alone: a tie between aa1 and bb1 -> ambiguous (refused)
    std::vector<std::vector<uint8_t>> fam;
    for (const auto& s : t.sigs) if (s.groups.size() == 2) fam.push_back(raw(s));
    CHECK(fam.size() >= 3);
    const Result r = run(t, stream_with(fam, 1 << 16, 77));
    CHECK(r.kind == Result::Kind::ambiguous && r.reason == "tie" && r.best == fam.size() && r.second == fam.size());
}

static void test_parse_errors() {
    const Synth s = make_synth();
    Table t; std::string why;
    CHECK(parse(s.json, t, why));
    CHECK(t.sigs.size() == 18 && t.groups.size() == 3 && t.groups[0].zvrs_sha256 == "ab" && t.groups[1].zvrs_sha256.empty());
    // the table never holds the raw window bytes (a scan of this process must not find the table itself)
    for (const auto& sg : t.sigs) CHECK(std::memcmp(sg.masked, raw(sg).data(), kWindow) != 0);
    auto bad = [&](std::string j, const char* expect) {
        Table u; std::string w;
        const bool ok = parse(j, u, w);
        if (ok || w.find(expect) == std::string::npos) std::printf("  case expected '%s' got ok=%d why='%s'\n", expect, ok ? 1 : 0, w.c_str());
        CHECK(!ok && w.find(expect) != std::string::npos);
        CHECK(u.sigs.empty() && u.groups.empty());   // all-or-nothing
    };
    auto rep = [&](const std::string& from, const std::string& to) { std::string j = s.json; const size_t p = j.find(from); CHECK(p != std::string::npos); j.replace(p, from.size(), to); return j; };
    bad("", "empty");
    bad("[1,2]", "root");
    bad(s.json + "x", "trailing");
    bad(s.json.substr(0, s.json.size() - 3), "json");
    bad(rep("acz-stage-fingerprints", "other"), "format");
    bad(rep("\"version\":1", "\"version\":2"), "version");
    bad(rep("\"window\":32", "\"window\":16"), "window");
    bad(rep("\"min_hits\":3", "\"min_hits\":0"), "min_hits");
    bad(rep("\"stage\":\"b1\"", "\"stage\":\"b-1\""), "group stage id");
    bad(rep("\"members\":[\"b1\"]", "\"members\":[\"a2\"]"), "two groups");
    bad(rep("\"members\":[\"b1\"]", "\"members\":[\"b2\"]"), "representative");
    bad(rep("\"groups\":[0]", "\"groups\":[3]"), "group index");
    bad(rep("\"groups\":[0]", "\"groups\":[0,0]"), "repeated");
    bad(rep("\"groups\":[0]", "\"groups\":[]"), "sig groups");
    { std::string h = hexs(s.a[0].data(), kWindow); bad(rep(h, h.substr(0, 62)), "hex length"); bad(rep(h, "zz" + h.substr(2)), "hex digit");
      bad(rep(hexs(s.a[1].data(), kWindow), h), "duplicate"); }
    std::string deep = "{\"format\":"; for (int i = 0; i < 40; ++i) deep += "["; bad(deep, "json");
    std::string why2; CHECK(!load_table_file("Z:\\no\\such\\fingerprints.json", t, why2) && why2.find("cannot open") != std::string::npos);
    CHECK(table_path("C:\\x") == "C:\\x\\fingerprints.json");
}

static void test_matcher() {
    const Synth s = make_synth();
    Table t; std::string why; CHECK(parse(s.json, t, why));
    // A's own signatures (+ some family): A
    { auto ws = s.a; ws.push_back(s.ab[0]); const Result r = run(t, stream_with(ws, 300000, 1));
      CHECK(r.kind == Result::Kind::match && r.stage == "a1" && r.best == 6 && r.second == 1 && r.sigs_seen == 6); }
    // same stream fed in random small chunks (windows straddle many chunk boundaries) and in 1-byte pieces near a signature
    { auto v = stream_with(s.b, 200000, 2);
      for (size_t c : {1u, 7u, 31u, 32u, 33u, 64u, 4099u}) { const Result r = run(t, v, c, c); CHECK(r.kind == Result::Kind::match && r.stage == "b1" && r.best == 5); } }
    // a signature exactly across one chunk boundary, at every split point
    for (uint32_t split = 1; split < kWindow; ++split) {
        std::vector<uint8_t> v = noise(1000, 5); std::memcpy(&v[500 - split], s.b[2].data(), kWindow);
        Matcher m(t); m.begin_stream(); m.feed(v.data(), 500); m.feed(v.data() + 500, 500); m.end_stream();
        CHECK(m.decide().sigs_seen == 1);
    }
    // ... but never across two SEPARATE streams (different memory ranges)
    { std::vector<uint8_t> v = noise(1000, 6); std::memcpy(&v[490], s.b[2].data(), kWindow);
      Matcher m(t); m.begin_stream(); m.feed(v.data(), 500); m.end_stream(); m.begin_stream(); m.feed(v.data() + 500, 500); m.end_stream();
      CHECK(m.decide().sigs_seen == 0 && m.decide().reason == "no-signature"); }
    // family only -> tie (refused); family + one B-only -> B (7 vs 6)
    { const Result r = run(t, stream_with(s.ab, 100000, 3)); CHECK(r.kind == Result::Kind::ambiguous && r.best == 6 && r.second == 6 && r.stage.empty()); }
    { auto ws = s.ab; ws.push_back(s.b[4]); const Result r = run(t, stream_with(ws, 100000, 4));
      CHECK(r.kind == Result::Kind::match && r.stage == "b1" && r.best == 7 && r.second == 6); }
    // both A and B fully present (e.g. stale data of an earlier mission): equal -> ambiguous; one more A -> A
    { auto ws = s.a; ws.insert(ws.end(), s.b.begin(), s.b.end()); CHECK(run(t, stream_with(ws, 100000, 8)).kind == Result::Kind::ambiguous); }
    // below min_hits: two C-only signatures -> no match; a lower threshold table accepts them
    { const Result r = run(t, stream_with(s.c, 50000, 5)); CHECK(r.kind == Result::Kind::no_match && r.reason == "below-min-hits" && r.best == 2); }
    { const Synth s2 = make_synth(2); Table t2; CHECK(parse(s2.json, t2, why)); const Result r = run(t2, stream_with(s2.c, 50000, 5)); CHECK(r.kind == Result::Kind::match && r.stage == "c1"); }
    // nothing / repeated copies count once / partial (31-byte) windows never count
    { const Result r = run(t, noise(1 << 20, 9)); CHECK(r.kind == Result::Kind::no_match && r.sigs_seen == 0); }
    { std::vector<std::vector<uint8_t>> ws(9, s.c[0]); ws.push_back(s.c[1]); const Result r = run(t, stream_with(ws, 100000, 11)); CHECK(r.sigs_seen == 2 && r.best == 2); }
    { std::vector<uint8_t> v = noise(4096, 12); for (int i = 0; i < 5; ++i) { std::memcpy(&v[100 + 200 * i], s.a[size_t(i)].data(), kWindow - 1); v[size_t(100 + 200 * i) + kWindow - 1] = uint8_t(s.a[size_t(i)][kWindow - 1] ^ 0xFF); }
      CHECK(run(t, v).sigs_seen == 0); }
    // a scan of the table's own (masked) storage finds nothing
    { std::vector<uint8_t> v; for (const auto& sg : t.sigs) v.insert(v.end(), sg.masked, sg.masked + kWindow);
      CHECK(run(t, v).sigs_seen == 0); }
    // source summary + reset
    { Matcher m(t); auto v = stream_with({s.a[0], s.b[0], s.ab[0]}, 10000, 13); m.feed(v.data(), v.size());
      CHECK(m.source_summary() == "mapset=1,mhd=1,sph=1"); m.reset(); CHECK(m.decide().sigs_seen == 0 && m.source_summary() == "none" && m.bytes_fed() == 0); }
    // throughput smoke (no assertion beyond a generous bound): 64 MiB of noise
    { const std::vector<uint8_t> v = noise(64u << 20, 21); const auto t0 = std::chrono::steady_clock::now();
      Matcher m(t); m.feed(v.data(), v.size()); const double s_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::printf("  matcher throughput: %.0f MiB/s\n", 64.0 / (s_ > 1e-6 ? s_ : 1e-6)); CHECK(s_ < 30.0); }
}

// ---- R-3: the bounded scan core (stage_scan.cpp) against a FAKE address space + FAKE page-state provider.
// Bytes are synthesised from the address (no big buffers: this machine is memory-constrained); planted windows overlay them.
struct FakeSpace final : PageSource {
    struct Plant { uint64_t addr; std::vector<uint8_t> w; };
    std::vector<Plant> plants;
    std::map<uint64_t, bool> nonresident;       // page address -> true when NOT resident
    bool fail_query = false;
    uint64_t clock = 0, ms_per_read = 0;
    uint64_t reads = 0, bytes = 0, max_read = 0, touched_nonresident = 0, yields = 0, queries = 0;
    static uint8_t at(uint64_t a) { uint64_t x = a * 0x9E3779B97F4A7C15ull; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; return uint8_t(x >> 56); }
    bool resident(uint64_t addr, uint32_t pages, uint8_t* flags) noexcept override {
        ++queries;
        if (fail_query) return false;
        for (uint32_t i = 0; i < pages; ++i) flags[i] = nonresident.count(addr + uint64_t(i) * kPage) ? 0 : 1;
        return true;
    }
    bool read(uint64_t addr, void* out, size_t n) noexcept override {
        ++reads; bytes += n; if (n > max_read) max_read = n; clock += ms_per_read;
        for (uint64_t pg = addr & ~uint64_t(kPage - 1); pg < addr + n; pg += kPage) if (nonresident.count(pg)) ++touched_nonresident;
        auto* o = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < n; ++i) o[i] = at(addr + i);
        for (const auto& p : plants)
            for (size_t i = 0; i < p.w.size(); ++i) if (p.addr + i >= addr && p.addr + i < addr + n) o[p.addr + i - addr] = p.w[i];
        return true;
    }
    uint64_t now_ms() noexcept override { return clock; }
    void yield(uint32_t ms) noexcept override { ++yields; clock += ms; }
    void plant(uint64_t a, const std::vector<uint8_t>& w) { plants.push_back({a, w}); }
};
static int g_stop_reads = -1; static FakeSpace* g_stop_space = nullptr;
static bool fake_stop() noexcept { return g_stop_space && g_stop_reads >= 0 && g_stop_space->reads >= uint64_t(g_stop_reads); }

static Region reg(uint64_t base, uint64_t size, uint8_t cls, uint64_t span = 0) { Region r; r.base = base; r.size = size; r.cls = cls; r.alloc_base = base; r.alloc_span = span ? span : size; return r; }

static void test_scan_core() {
    const Synth s = make_synth();
    Table t; std::string why; CHECK(parse(s.json, t, why));
    constexpr uint64_t MiB = 1ull << 20;
    // classification + order: ee64 first, then private RW 1..256 MiB largest first, then the rest by address
    CHECK(classify(true, true, 64 * MiB) == kEeLike && classify(true, true, 8 * MiB) == kPrivRwMid && classify(true, true, 512 * 1024) == kPrivRwOther &&
          classify(true, true, 512 * MiB) == kPrivRwOther && classify(true, false, 64 * MiB) == kPrivOther && classify(false, true, 64 * MiB) == kMapped);
    { std::vector<Region> v = {reg(0x9000000, MiB, kMapped), reg(0x1000000, MiB, kPrivRwMid, 2 * MiB), reg(0x2000000, MiB, kPrivRwMid, 32 * MiB),
                               reg(0x3000000, MiB, kPrivRwOther), reg(0x8000000, MiB, kEeLike, 64 * MiB), reg(0x0500000, MiB, kPrivOther)};
      order_regions(v);
      CHECK(v[0].cls == kEeLike && v[1].alloc_span == 32 * MiB && v[2].alloc_span == 2 * MiB && v[3].cls == kPrivRwOther && v[4].cls == kPrivOther && v[5].cls == kMapped); }
    std::vector<uint8_t> buf(kMaxChunk);
    auto fresh = [] { ScanLimits l; l.decisive_hits = 6; l.decisive_margin = 3; return l; };
    // (1) decisive early stop: the EE-like block holds 5 A-only + 1 family signature (A 6 vs B 1): the scan stops inside it and never
    //     reads the 4 x 64 MiB regions that come later in the order (one of them, mapped, has a lower address).
    { FakeSpace f; std::vector<Region> v = {reg(0x10000000, 64 * MiB, kMapped), reg(0x40000000, 16 * MiB, kEeLike, 64 * MiB),
                                            reg(0x80000000, 64 * MiB, kPrivRwMid), reg(0xC0000000, 64 * MiB, kPrivRwOther), reg(0x100000000, 64 * MiB, kPrivOther)};
      for (int i = 0; i < 5; ++i) f.plant(0x40000000 + uint64_t(i) * 700000 + 13, s.a[size_t(i)]);
      f.plant(0x40000000 + 5 * 700000 + 13, s.ab[0]);
      order_regions(v);
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), fresh(), st);
      const Result r = m.decide();
      CHECK(std::strcmp(st.stop, "decisive") == 0 && !st.truncated && r.kind == Result::Kind::match && r.stage == "a1" && r.best == 6 && r.second == 1);
      CHECK(st.regions == 1 && st.bytes <= 16 * MiB && st.bytes == f.bytes && st.decisive_at == st.bytes && st.candidate_at > 0 && st.candidate_at <= st.decisive_at);
      CHECK(st.hit_n == 1 && st.hit[0].rank == 0 && st.hit[0].cls == kEeLike && st.hit[0].hits == 6 && st.hit[0].alloc_span == 64 * MiB);
      CHECK(st.bytes_eligible == 16 * MiB + 4 * 64 * MiB && f.max_read <= kMaxChunk);
      std::printf("  decisive: read %llu of %llu eligible bytes\n", static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.bytes_eligible)); }
    // (1b) not decisive (family only = tie): everything eligible is read, the answer equals the unbounded Matcher's
    { FakeSpace f; std::vector<Region> v = {reg(0x40000000, 8 * MiB, kEeLike, 64 * MiB), reg(0x80000000, 8 * MiB, kPrivRwMid)};
      for (int i = 0; i < 6; ++i) f.plant(0x80000000 + uint64_t(i) * 1000003, s.ab[size_t(i)]);
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), fresh(), st);
      CHECK(std::strcmp(st.stop, "complete") == 0 && !st.truncated && st.bytes == 16 * MiB && m.decide().kind == Result::Kind::ambiguous && st.candidate_at == 0); }
    // (1c) the default thresholds (16 hits) cannot be reached with 5 hits: full scan, still a match
    { FakeSpace f; std::vector<Region> v = {reg(0x40000000, 8 * MiB, kEeLike, 64 * MiB)};
      for (int i = 0; i < 5; ++i) f.plant(0x40000000 + uint64_t(i) * 1000003, s.b[size_t(i)]);
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), ScanLimits{}, st);
      CHECK(std::strcmp(st.stop, "complete") == 0 && st.bytes == 8 * MiB && m.decide().stage == "b1"); }
    // (2) byte caps: hard cap without any candidate; post-candidate cap after a non-decisive lead; time cap
    { FakeSpace f; std::vector<Region> v; for (int i = 0; i < 8; ++i) v.push_back(reg(0x100000000ull + uint64_t(i) * 0x10000000, 32 * MiB, kPrivRwMid));
      ScanLimits l = fresh(); l.max_bytes = 10 * MiB;
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), l, st);
      CHECK(std::strcmp(st.stop, "byte-cap") == 0 && st.truncated && st.bytes >= 10 * MiB && st.bytes <= 10 * MiB + kMaxChunk && f.bytes == st.bytes && st.cap_bytes == 10 * MiB); }
    { FakeSpace f; std::vector<Region> v; for (int i = 0; i < 8; ++i) v.push_back(reg(0x100000000ull + uint64_t(i) * 0x10000000, 32 * MiB, kPrivRwMid));
      for (int i = 0; i < 3; ++i) f.plant(0x100000000ull + 5 * MiB + uint64_t(i) * 4099, s.a[size_t(i)]);   // A 3 vs 0: candidate, not decisive
      ScanLimits l = fresh(); l.post_candidate_bytes = 12 * MiB;
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), l, st);
      CHECK(std::strcmp(st.stop, "post-candidate-cap") == 0 && st.truncated && st.candidate_at == 8 * MiB && st.cap_bytes == 20 * MiB &&
            st.bytes >= 20 * MiB && st.bytes <= 20 * MiB + kMaxChunk && m.decide().stage == "a1"); }
    { FakeSpace f; f.ms_per_read = 10; std::vector<Region> v = {reg(0x40000000, 256 * MiB, kPrivRwMid)};
      ScanLimits l = fresh(); l.max_ms = 50; l.yield_ms = 0;
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), l, st);
      CHECK(std::strcmp(st.stop, "time-cap") == 0 && st.truncated && f.reads == 5 && st.yields == 0); }
    // chunking + yields: every read <= the chunk, one yield per full chunk of bytes read (the 12 KiB tail does not yield)
    { FakeSpace f; std::vector<Region> v = {reg(0x40000000, 10 * MiB + 3 * kPage, kPrivRwMid)};
      ScanLimits l = fresh(); l.chunk = 1 * MiB + 123;   // rounded down to whole pages
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), l, st);
      CHECK(f.max_read == 1 * MiB && f.reads == 11 && st.yields == 10 && f.yields == 10 && st.bytes == 10 * MiB + 3 * kPage); }
    // (3) non-resident pages are never read: signatures on them stay unseen; a window across a resident/non-resident boundary is not
    //     joined; skipped bytes are counted; a failed residency query skips the whole chunk; skip_nonresident=false reads everything.
    { FakeSpace f; const uint64_t B = 0x40000000;
      std::vector<Region> v = {reg(B, 8 * MiB, kEeLike, 64 * MiB)};
      for (uint64_t pg = B + 1 * MiB; pg < B + 3 * MiB; pg += kPage) f.nonresident[pg] = true;   // 2 MiB paged out / never touched
      f.nonresident[B + 6 * MiB] = true;                                                       // one page after a resident one
      f.plant(B + 100, s.a[0]);                          // resident
      f.plant(B + 2 * MiB + 77, s.a[1]);                 // non-resident
      f.plant(B + 6 * MiB - 16, s.a[2]);                 // straddles resident -> non-resident
      f.plant(B + 7 * MiB, s.a[3]);                      // resident
      { Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), fresh(), st);
        CHECK(f.touched_nonresident == 0 && st.bytes_nonresident == 2 * MiB + kPage && st.bytes == 8 * MiB - st.bytes_nonresident && st.bytes == f.bytes);
        CHECK(m.seen_count() == 2 && m.seen()[0] && !m.seen()[1] && !m.seen()[2] && m.seen()[3]); }
      { FakeSpace g = f; g.fail_query = true; g.reads = g.bytes = 0;
        Matcher m(t); ScanStats st; scan_regions(m, v, g, buf.data(), fresh(), st);
        CHECK(g.reads == 0 && st.bytes == 0 && st.bytes_nonresident == 8 * MiB && st.ws_query_failures == 2 && m.seen_count() == 0); }
      { FakeSpace g = f; g.reads = g.bytes = 0; g.queries = 0; ScanLimits l = fresh(); l.skip_nonresident = false;
        Matcher m(t); ScanStats st; scan_regions(m, v, g, buf.data(), l, st);
        CHECK(g.queries == 0 && st.bytes == 8 * MiB && m.seen_count() == 4); } }
    // orderly stop (game exit / drain): checked before every chunk
    { FakeSpace f; std::vector<Region> v = {reg(0x40000000, 64 * MiB, kPrivRwMid)};
      g_stop_space = &f; g_stop_reads = 2; ScanLimits l = fresh(); l.stop = &fake_stop;
      Matcher m(t); ScanStats st; scan_regions(m, v, f, buf.data(), l, st);
      g_stop_space = nullptr;
      CHECK(std::strcmp(st.stop, "stop") == 0 && st.truncated && f.reads == 2); }
    // no buffer: nothing read
    { FakeSpace f; std::vector<Region> v = {reg(0x40000000, MiB, kPrivRwMid)};
      Matcher m(t); ScanStats st; scan_regions(m, v, f, nullptr, fresh(), st); CHECK(f.reads == 0 && std::strcmp(st.stop, "no-buffer") == 0); }
    // lead() agrees with decide()
    { Matcher m(t); auto vv = stream_with({s.a[0], s.a[1], s.ab[0], s.b[0]}, 20000, 31); m.feed(vv.data(), vv.size());
      uint32_t b = 0, c = 0; int g = -1; m.lead(b, c, g); const Result r = m.decide();
      CHECK(b == r.best && c == r.second && g == r.ranking[0].first && m.seen_count() == r.sigs_seen); }
}

// Offline tool mode (not a ctest case): run the real Matcher over files, each file one stream.
//   stage_fingerprint_offline_tests --scan-files <fingerprints.json> <file>...
static int scan_files(int argc, char** argv) {
    Table t; std::string why;
    if (!load_table_file(argv[2], t, why)) { std::printf("table: %s\n", why.c_str()); return 2; }
    int rc = 0;
    for (int i = 3; i < argc; ++i) {
        FILE* f = std::fopen(argv[i], "rb");
        if (!f) { std::printf("%s: cannot open\n", argv[i]); rc = 2; continue; }
        Matcher m(t); m.begin_stream();
        std::vector<uint8_t> buf(1 << 20); size_t r;
        while ((r = std::fread(buf.data(), 1, buf.size(), f)) > 0) m.feed(buf.data(), r);
        std::fclose(f); m.end_stream();
        const Result res = m.decide();
        std::string top;
        for (size_t k = 0; k < res.ranking.size() && k < 4; ++k) top += (k ? "," : "") + t.groups[size_t(res.ranking[k].first)].stage + ":" + std::to_string(res.ranking[k].second);
        std::printf("%s result=%s stage=%s best=%u second=%u sigs=%u sources=%s top=%s\n", argv[i], kind_name(res.kind), res.stage.empty() ? "-" : res.stage.c_str(),
                    res.best, res.second, res.sigs_seen, m.source_summary().c_str(), top.c_str());
    }
    return rc;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--scan-files") == 0) return scan_files(argc, argv);
    test_fixture();
    test_parse_errors();
    test_matcher();
    test_scan_core();
    std::printf("stage_fingerprint_offline_tests: %u checks passed\n", g_checks);
    return 0;
}
