// BRIEF3D phase-2 capture offline tests (src/acz-vr/brief3d_capture.h; docs/world-render/brief3d.md 6.6 / 7 B3-2, B3-5): the shared safe reader
// with fake span/read functions (every refusal named, two reads that must agree), cb10 from tracked root words, the sampling state machine (armed,
// one frame per second, frames without a SelMap draw skipped, caps, late commits), the icon-vs-grid matrix comparison, the bounded line (worst case
// for any values) and the thread-safe shared path. NOT tested: the D3D12 glue (brief3d_capture_hooks.inl) against a live device.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "brief3d_capture.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace acz::b3cap;
using acz::brief3d::Kind;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {

void put_matrix(uint8_t* out, float base) { for (int i = 0; i < 16; ++i) { const float v = base + float(i); std::memcpy(out + 4 * i, &v, 4); } }

// ---- fakes for step3MappedSpan_locked / guardedCpuRead
struct FakeMem {
    uint8_t bytes[512]{};
    uint64_t address = 0x1000, gpu_va = 0x7f0000, size = 512;
    const char* span_why = nullptr;   // forced span refusal
    int fault_on = 0;                 // 1 = first read faults, 2 = second
    bool tear = false;                // the second read sees a changed byte
    int reads = 0, spans = 0;
    auto span() {
        return [this](uint64_t a, uint64_t va, uint64_t n, const uint8_t*& out) -> const char* {
            ++spans; out = nullptr;
            if (span_why) return span_why;
            if (a != address) return "no-creation-record";
            if (va < gpu_va || va - gpu_va > size || n > size - (va - gpu_va)) return "span-exceeds-resource";
            out = bytes + (va - gpu_va);
            return nullptr;
        };
    }
    auto read() {
        return [this](const uint8_t* src, uint8_t* dst, size_t n) -> bool {
            ++reads;
            if (fault_on == reads) return false;
            std::memcpy(dst, src, n);
            if (tear && reads == 2) dst[n - 1] ^= 0x5a;
            return true;
        };
    }
};

void test_read_twice() {
    uint8_t out[kRecordCap], scratch[kRecordCap];
    {
        FakeMem m; put_matrix(m.bytes + 64, 1.0f);
        CHECK(read_twice(m.span(), m.read(), m.address, m.gpu_va + 64, 64, out, scratch) == nullptr);
        CHECK(std::memcmp(out, m.bytes + 64, 64) == 0 && m.reads == 2 && m.spans == 1);
        CHECK(read_twice(m.span(), m.read(), m.address, m.gpu_va, 272, out, scratch) == nullptr);   // the icon record size
    }
    {
        FakeMem m;
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, 0, 64, out, scratch), "b3cap-t10-va-unknown") == 0);
        CHECK(std::strcmp(read_twice(m.span(), m.read(), 0, m.gpu_va, 64, out, scratch), "b3cap-t10-address-unresolved") == 0);
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, m.gpu_va, 0, out, scratch), "b3cap-record-size") == 0);
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, m.gpu_va, kRecordCap + 1, out, scratch), "b3cap-record-size") == 0);
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, m.gpu_va, 64, nullptr, scratch), "b3cap-record-size") == 0);
        CHECK(m.spans == 0 && m.reads == 0);   // refused before any span lookup or read
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, m.gpu_va + 500, 64, out, scratch), "span-exceeds-resource") == 0);
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address + 8, m.gpu_va, 64, out, scratch), "no-creation-record") == 0);
        CHECK(m.reads == 0);   // no read without a proven span
    }
    for (const char* why : {"not-upload-buffer", "not-mapped", "map-lifetime-mismatch", "binding-observer-disabled", "outstanding-map-count"}) {
        FakeMem m; m.span_why = why;
        CHECK(read_twice(m.span(), m.read(), m.address, m.gpu_va, 64, out, scratch) == why);   // the reader's own reason, unchanged
        CHECK(m.reads == 0);
    }
    for (int f = 1; f <= 2; ++f) {
        FakeMem m; m.fault_on = f;
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, m.gpu_va, 64, out, scratch), "b3cap-read-fault") == 0);
    }
    {
        FakeMem m; m.tear = true;
        CHECK(std::strcmp(read_twice(m.span(), m.read(), m.address, m.gpu_va, 64, out, scratch), "b3cap-record-torn") == 0);
    }
    {   // a span function that claims success without a pointer
        auto span = [](uint64_t, uint64_t, uint64_t, const uint8_t*& o) -> const char* { o = nullptr; return nullptr; };
        auto rd = [](const uint8_t*, uint8_t*, size_t) { return true; };
        CHECK(std::strcmp(read_twice(span, rd, 1, 1, 64, out, scratch), "b3cap-span-null") == 0);
    }
}

void test_b10() {
    float o[2];
    const float f[2] = {1280.0f, -896.0f};
    uint32_t w[2]; std::memcpy(w, f, 8);
    const bool both[2] = {true, true}, one[2] = {true, false};
    CHECK(b10_pair(w, both, 2, o) == nullptr && o[0] == 1280.0f && o[1] == -896.0f);
    CHECK(std::strcmp(b10_pair(w, one, 2, o), "b3cap-b10-unwritten") == 0);
    CHECK(std::strcmp(b10_pair(w, both, 1, o), "b3cap-b10-short") == 0);
    CHECK(std::strcmp(b10_pair(nullptr, both, 2, o), "b3cap-b10-short") == 0);
    const float bad[2] = {std::numeric_limits<float>::quiet_NaN(), 1.0f}; std::memcpy(w, bad, 8);
    CHECK(std::strcmp(b10_pair(w, both, 2, o), "b3cap-b10-nonfinite") == 0);
}

void test_kind() {
    CHECK(kind_of(0x384ade02u) == Kind::selmap);
    CHECK(kind_of(0xd5a41c2fu) == Kind::modelicon);
    CHECK(kind_of(0xb569109eu) == Kind::none && kind_of(0) == Kind::none);
}

DrawFacts sel_facts(float base) {
    DrawFacts d; d.kind = Kind::selmap; d.classified = true; d.count = 65024; d.index_stride = 2; d.t10_why = nullptr; put_matrix(d.rec, base);
    d.b10_why = nullptr; d.b10[0] = 640; d.b10[1] = 448; d.pso_known = true; d.depth_enable = true; d.depth_func = 7; d.rt_count = 1; d.rtv0 = 28;
    d.dsv = 40; d.samples = 1; d.topo_type = 2; d.ia_topology = 2; d.vb_va = 0x12345000; d.vb_bytes = 262144; d.vb_stride = 16; d.vb_why = nullptr;
    return d;
}
DrawFacts icon_facts(const uint8_t* grid64, int match_at, int second_match = -1) {
    DrawFacts d; d.kind = Kind::modelicon; d.count = 63; d.ia_topology = 5; d.t10_why = nullptr;
    for (int k = 0; k < 4; ++k) put_matrix(d.rec + 64 * k, 100.0f * float(k + 1));
    if (match_at >= 0) std::memcpy(d.rec + 64 * match_at, grid64, 64);
    if (second_match >= 0) std::memcpy(d.rec + 64 * second_match, grid64, 64);
    return d;
}

void test_accumulator() {
    auto a = std::make_unique<Accumulator>();
    uint64_t serial = 0;
    CHECK(a->admit(Kind::selmap, serial) == Admit::skip);   // not armed
    CHECK(!a->present(1000));
    a->set_armed(true);
    CHECK(a->admit(Kind::selmap, serial) == Admit::skip);   // armed, sampling starts at the next present
    CHECK(!a->present(1000) && a->sampling);
    CHECK(a->admit(Kind::none, serial) == Admit::skip);
    CHECK(a->admit(Kind::selmap, serial) == Admit::selmap_full);
    const uint64_t selSerial = serial;
    CHECK(a->admit(Kind::selmap, serial) == Admit::count);   // only the first SelMap is read
    const DrawFacts sel = sel_facts(1.0f);
    a->commit(selSerial, sel);
    CHECK(a->f.have_sel && a->f.sel_draws == 2);
    for (unsigned i = 0; i < kIconCap; ++i) {
        CHECK(a->admit(Kind::modelicon, serial) == Admit::icon_read);
        a->commit(serial, icon_facts(sel.rec, int(i % 4)));
    }
    CHECK(a->admit(Kind::modelicon, serial) == Admit::count);   // over the cap: counted, never read
    CHECK(a->f.icon_draws == kIconCap + 1 && a->f.icon_read == kIconCap);
    DrawFacts bad; bad.kind = Kind::modelicon; bad.t10_why = "not-mapped";
    a->commit(serial, bad);   // a refused icon (would not be admitted past the cap live; counts as unread)
    CHECK(a->f.icon_unread == 1 && std::strcmp(a->f.icon_why, "not-mapped") == 0);
    a->commit(serial + 99, sel);   // stale serial: counted late, ignored
    CHECK(a->f.late == 1);
    const Summary s = compare_icons(a->f);
    CHECK(s.eq[0] == kIconCap / 4 && s.eq[1] == kIconCap / 4 && s.eq[2] == kIconCap / 4 && s.eq[3] == kIconCap / 4);
    CHECK(s.one == kIconCap && s.none == 0 && s.multi == 0 && std::strcmp(s.why, "not-mapped") == 0);
    CHECK(a->present(5000));   // the sampled frame had SelMap: line due
    char line[kLineLimit];
    const int n = format_line(line, sizeof line, a->f);
    CHECK(n > 0 && size_t(n) == std::strlen(line) && size_t(n) < kLineLimit - kLinePrefix);
    CHECK(std::strstr(line, "brief3d-capture;sel=2;cls=1;ix=2*65024;t10=ok;m=1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16;b10=640,448;") == line);
    CHECK(std::strstr(line, ";pso=100,7,0:") != nullptr);
    CHECK(std::strstr(line, ";rt=1:28,40,1;tt=2;ia=2;vb=12345000+262144/16:mapped-upload;") != nullptr);
    CHECK(std::strstr(line, ";ic=129/128/1;iia=5;imax=63;eq=32.32.32.32;cam=128/0/0;iw=not-mapped") != nullptr);
    a->finish_line(5000);
    CHECK(!a->sampling && a->lines == 1);
    CHECK(a->admit(Kind::selmap, serial) == Admit::skip);   // not sampling until a second has passed
    CHECK(!a->present(5000 + kPeriodUs - 1) && !a->sampling);
    CHECK(!a->present(5000 + kPeriodUs) && a->sampling);
    // a sampled frame without SelMap: no line, the next frame is sampled instead
    CHECK(a->admit(Kind::modelicon, serial) == Admit::icon_read);
    CHECK(!a->present(5000 + kPeriodUs + 16000) && a->sampling && a->f.icon_draws == 0);
    // disarm: sampling stops, frame cleared
    CHECK(a->admit(Kind::selmap, serial) == Admit::selmap_full);
    a->set_armed(false);
    CHECK(!a->sampling && a->f.sel_draws == 0);
    CHECK(a->admit(Kind::selmap, serial) == Admit::skip && !a->present(9 * kPeriodUs));
    // QPC going backwards never stalls sampling
    a->set_armed(true); a->last_emit_us = 10 * kPeriodUs;
    CHECK(!a->present(kPeriodUs) && a->sampling);
}

void test_compare_icons() {
    auto f = std::make_unique<Frame>();
    CHECK(std::strcmp(compare_icons(*f).why, "b3cap-no-grid-matrix") == 0);
    f->have_sel = true; f->sel = sel_facts(3.0f);
    const uint8_t* g = f->sel.rec;
    auto add = [&](const DrawFacts& d) { std::memcpy(f->icon_m[f->icon_read++], d.rec, 256); };
    add(icon_facts(g, 1)); add(icon_facts(g, 1)); add(icon_facts(g, -1)); add(icon_facts(g, 0, 3));
    Summary s = compare_icons(*f);
    CHECK(s.why == nullptr && s.eq[0] == 1 && s.eq[1] == 2 && s.eq[2] == 0 && s.eq[3] == 1 && s.one == 2 && s.none == 1 && s.multi == 1);
    // tolerance: 1e-5 relative to the grid's largest magnitude (18 here): +1e-4 passes, +0.05 does not
    DrawFacts close = icon_facts(g, 2); float v; std::memcpy(&v, close.rec + 64 * 2, 4); v += 1e-4f; std::memcpy(close.rec + 64 * 2, &v, 4);
    DrawFacts distant = icon_facts(g, 2); std::memcpy(&v, distant.rec + 64 * 2, 4); v += 0.05f; std::memcpy(distant.rec + 64 * 2, &v, 4);
    f->icon_read = 0; add(close); add(distant);
    s = compare_icons(*f);
    CHECK(s.eq[2] == 1 && s.one == 1 && s.none == 1);
    // an unreadable grid record: never compared
    f->sel.t10_why = "not-mapped";
    CHECK(std::strcmp(compare_icons(*f).why, "b3cap-no-grid-matrix") == 0 && compare_icons(*f).one == 0);
    // non-finite or zero reference: nothing equal
    uint8_t z[64]{}; CHECK(!matrix_equal(z, z));
    float nanm[16]; for (float& x : nanm) x = std::numeric_limits<float>::quiet_NaN();
    uint8_t nb[64]; std::memcpy(nb, nanm, 64); CHECK(!matrix_equal(nb, nb));
}

void test_line_bounds() {
    static const char* const kLong = "a-very-long-refusal-reason-name-that-exceeds-every-field-width-0123456789";
    const float big[] = {-1.23456789e+30f, -1.23456e-30f, std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity(),
                         -123456.7f, 3.4e38f};
    size_t worst = 0;
    for (int variant = 0; variant < 8; ++variant) {
        auto f = std::make_unique<Frame>();
        f->sel_draws = 0xffffffffu; f->icon_draws = 0xffffffffu; f->icon_unread = 0xffffffffu; f->icon_ia = 0xffffffffu; f->icon_max_count = 0xffffffffu;
        f->have_sel = variant != 7;
        DrawFacts& d = f->sel;
        d.classified = true; d.index_stride = 0xffffffffu; d.count = 0xffffffffu;
        d.t10_why = (variant & 1) ? kLong : nullptr;
        for (int i = 0; i < 16; ++i) { const float x = big[(i + variant) % 6]; std::memcpy(d.rec + 4 * i, &x, 4); }
        d.b10_why = (variant & 2) ? kLong : nullptr; d.b10[0] = -1.2345678e+30f; d.b10[1] = -std::numeric_limits<float>::infinity();
        d.depth_enable = d.depth_write = d.stencil = d.blend_enable = true; d.depth_func = 0xffffffffu;
        for (uint32_t& b : d.blend) b = 0xffffffffu;
        d.rt_count = d.rtv0 = d.dsv = d.samples = d.topo_type = d.ia_topology = 0xffffffffu;
        d.vb_va = ~0ull; d.vb_bytes = ~0ull; d.vb_stride = 0xffffffffu; d.vb_why = (variant & 4) ? kLong : nullptr;
        f->icon_read = kIconCap; f->icon_why = kLong;
        for (unsigned i = 0; i < kIconCap; ++i) std::memcpy(f->icon_m[i], d.rec, 64);   // every icon equal to the grid in matrix 0 (when finite)
        char line[kLineLimit];
        const int n = format_line(line, sizeof line, *f);
        CHECK(n > 0 && size_t(n) == std::strlen(line));
        CHECK(size_t(n) < kLineLimit - kLinePrefix);   // never truncated: every field is clamped / width-limited
        CHECK(std::strstr(line, ";iw=") != nullptr);  // the last field is present
        if (size_t(n) > worst) worst = size_t(n);
        char shortbuf[40];
        const int m = format_line(shortbuf, sizeof shortbuf, *f);   // a short buffer truncates safely
        CHECK(m == 39 && std::strlen(shortbuf) == 39);
    }
    std::printf("brief3d-capture worst-case line: %zu bytes (limit %zu)\n", worst, kLineLimit - kLinePrefix);
    static Frame empty;
    char z[1] = {'x'};
    CHECK(format_line(z, 0, empty) == 0 && z[0] == 'x');
    CHECK(format_line(nullptr, 10, empty) == 0);
}

void test_shared_threads() {
    set_armed(true);
    char line[kLineLimit];
    CHECK(present(1, line, sizeof line) == 0 && sampling());
    std::atomic<bool> stop{false};
    std::vector<std::thread> t;
    DrawFacts sel = sel_facts(2.0f);
    for (int k = 0; k < 3; ++k) t.emplace_back([&, k] {
        while (!stop.load()) {
            uint64_t serial = 0;
            const Kind kind = k == 0 ? Kind::selmap : Kind::modelicon;
            const Admit a = admit(kind, serial);
            if (a == Admit::selmap_full) commit(serial, sel);
            else if (a == Admit::icon_read) commit(serial, icon_facts(sel.rec, 1));
        }
    });
    unsigned lines = 0;
    uint64_t now = 2;
    for (int i = 0; i < 20000 && lines < 5; ++i) {
        now += 400000;   // 0.4 s per "present"
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        const int n = present(now, line, sizeof line);
        if (n > 0) { ++lines; CHECK(size_t(n) < kLineLimit - kLinePrefix && std::strncmp(line, "brief3d-capture;", 16) == 0); }
    }
    stop = true;
    for (auto& x : t) x.join();
    CHECK(lines == 5);
    set_armed(false);
    CHECK(!sampling() && present(now + kPeriodUs * 5, line, sizeof line) == 0);
}


void test_census() {
    // icons grouped by index count; the y range of the translation row (element 13) of matrices 1..3
    Frame f; f.icon_read = 5;
    const uint32_t counts[5] = {20, 20, 33, 20, 33};
    const float ys[5] = {100.f, 300.f, 5000.f, -50.f, 7000.f};
    for (int i = 0; i < 5; ++i) {
        f.icon_cnt[i] = counts[i]; std::memset(f.icon_m[i], 0, 256);
        for (int k = 1; k < 4; ++k) { const float y = ys[i] + float(k); std::memcpy(f.icon_m[i] + 64 * k + 4 * 13, &y, 4); }
    }
    const Census c = census_of(f);
    CHECK(c.groups == 2 && c.more == 0 && c.nonfinite == 0 && c.g[0].count == 20 && c.g[0].n == 3 && c.g[1].count == 33 && c.g[1].n == 2);
    CHECK(c.g[0].lo[0] == -49.f && c.g[0].hi[0] == 301.f && c.g[1].lo[2] == 5003.f && c.g[1].hi[2] == 7003.f);
    char line[kLineLimit];
    f.icon_draws = 5;
    const int n = format_census(line, sizeof line, f);
    CHECK(n > 0 && size_t(n) < kLineLimit - kLinePrefix && std::strncmp(line, "brief3d-icons;draws=5;read=5;groups=2;more=0;nonfinite=0;c20:n3:y1=-49..301", 74) == 0);
    // overflow groups, non-finite values and the worst-case width stay bounded
    Frame g; g.icon_read = kIconCap;
    for (unsigned i = 0; i < kIconCap; ++i) {
        g.icon_cnt[i] = 90000 + i;   // 128 distinct counts: 5 groups + more
        for (int k = 1; k < 4; ++k) { const float y = (i % 7 == 0) ? NAN : -1.2345678e+30f * float(i + 1); std::memcpy(g.icon_m[i] + 64 * k + 4 * 13, &y, 4); }
    }
    g.icon_draws = 4000000000u;
    const Census d = census_of(g);
    CHECK(d.groups == kCensusGroups && d.more == kIconCap - kCensusGroups && d.nonfinite > 0);
    const int m = format_census(line, sizeof line, g);
    CHECK(m > 0 && size_t(m) < kLineLimit - kLinePrefix && line[m] == 0);
    // the shared present() delivers the census line only when icons were read
    set_armed(true);
    uint64_t serial = 0; char l1[kLineLimit], l2[kLineLimit]; int n2 = -1;
    CHECK(present(1, l1, sizeof l1, l2, sizeof l2, &n2) == 0 && n2 == 0);
    const DrawFacts sel = sel_facts(2.0f);
    CHECK(admit(Kind::selmap, serial) == Admit::selmap_full); commit(serial, sel);
    CHECK(admit(Kind::modelicon, serial) == Admit::icon_read); commit(serial, icon_facts(sel.rec, 0));
    const int n1 = present(3, l1, sizeof l1, l2, sizeof l2, &n2);
    CHECK(n1 > 0 && n2 > 0 && std::strncmp(l2, "brief3d-icons;draws=1;read=1;groups=1", 37) == 0);
    set_armed(false);
}

void test_census_pos() {
    // matrix 3: translation x (12), z (14) and scale (0) per index-count group (the zoom magnification check, brief3d.md 9.1)
    Frame f; f.icon_read = 3;
    const uint32_t counts[3] = {15, 15, 24};
    const float xs[3] = {-3.f, 5.f, 30.f}, zs[3] = {1.f, -2.f, -31.f}, ss[3] = {0.5f, 0.75f, 0.25f};
    for (int i = 0; i < 3; ++i) {
        f.icon_cnt[i] = counts[i]; std::memset(f.icon_m[i], 0, 256);
        std::memcpy(f.icon_m[i] + 64 * 3 + 4 * 12, &xs[i], 4); std::memcpy(f.icon_m[i] + 64 * 3 + 4 * 14, &zs[i], 4);
        std::memcpy(f.icon_m[i] + 64 * 3, &ss[i], 4);
    }
    char line[kLineLimit];
    const int n = format_census_pos(line, sizeof line, f);
    CHECK(n > 0 && std::string(line) == "brief3d-icons-pos;read=3;groups=2;more=0;c15:n2:x=-3..5:z=-2..1:s=0.5..0.75;c24:n1:x=30..30:z=-31..-31:s=0.25..0.25");
    // worst case: 5 groups + overflow, every value 9 characters wide, non-finite skipped
    Frame g; g.icon_read = kIconCap;
    for (unsigned i = 0; i < kIconCap; ++i) {
        g.icon_cnt[i] = 90000 + i;
        for (int e : {0, 12, 14}) { const float v = (i % 7 == 0) ? NAN : -1.2345678e+30f * float(i + 1); std::memcpy(g.icon_m[i] + 64 * 3 + 4 * e, &v, 4); }
    }
    const int m = format_census_pos(line, sizeof line, g);
    CHECK(m > 0 && size_t(m) < kLineLimit - kLinePrefix && line[m] == 0);
    std::printf("brief3d-icons-pos worst-case line: %d bytes (limit %zu)\n", m, kLineLimit - kLinePrefix);
    // present() delivers it next to the census line
    set_armed(true);
    uint64_t serial = 0; char l1[kLineLimit], l2[kLineLimit], l3[kLineLimit]; int n2 = -1, n3 = -1;
    CHECK(present(5000000, l1, sizeof l1, l2, sizeof l2, &n2, l3, sizeof l3, &n3) == 0 && n2 == 0 && n3 == 0);
    const DrawFacts sel = sel_facts(2.0f);
    CHECK(admit(Kind::selmap, serial) == Admit::selmap_full); commit(serial, sel);
    CHECK(admit(Kind::modelicon, serial) == Admit::icon_read); commit(serial, icon_facts(sel.rec, 0));
    CHECK(present(5000002, l1, sizeof l1, l2, sizeof l2, &n2, l3, sizeof l3, &n3) > 0 && n2 > 0 && n3 > 0 &&
          std::strncmp(l3, "brief3d-icons-pos;read=1;groups=1;more=0", 40) == 0);
    set_armed(false);
}

}  // namespace

int main() {
    test_read_twice();
    test_b10();
    test_kind();
    test_accumulator();
    test_compare_icons();
    test_line_bounds();
    test_shared_threads();
    test_census();
    test_census_pos();
    std::printf("brief3d_capture_offline: %u checks passed\n", g_checks);
    return 0;
}
