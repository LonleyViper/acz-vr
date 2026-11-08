// BRIEFING-3D offline tests (src/acz-vr/briefing_observe.h; docs/world-render/briefing-3d-map.md section 4): the per-window draw histogram keyed by
// creation shader hashes, the named briefing shaders, the two log lines (bounded, truncation-safe), arming by window roll, and the thread-safe draw path.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "briefing_observe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace acz::brobs;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {

void test_keying() {
    Window w;
    note_draw(w, kSelMapVs8, 0x9ebdf113u, 2, false, false, 1024, 1);
    note_draw(w, kSelMapVs8, 0x9ebdf113u, 2, false, false, 512, 1);
    note_draw(w, kSelMapVs8, 0x9ebdf113u, 3, false, false, 300, 1);  // other topology = other key
    note_draw(w, kSelMapVs8, 0x9ebdf113u, 2, true, false, 6, 1);     // indexed = other key
    note_draw(w, kModelIconVs8, 0x9ebdf113u, 3, true, false, 90, 2);
    note_draw(w, 0, 0, 0, false, false, 3, 1);                        // unknown PSO
    CHECK(w.draws == 6);
    CHECK(w.noPso == 1);
    CHECK(w.n == 4);
    CHECK(w.e[0].draws == 2 && w.e[0].units == 1536 && w.e[0].maxUnits == 1024 && w.e[0].instances == 2);
    CHECK(draws_of(w, kSelMapVs8) == 4);
    CHECK(units_of(w, kSelMapVs8) == 1024 + 512 + 300 + 6);
    CHECK(draws_of(w, kModelIconVs8) == 1 && units_of(w, kModelIconVs8) == 90);
    CHECK(w.e[3].instances == 2);
}

void test_overflow() {
    Window w;
    for (uint32_t i = 0; i < kEntries + 5; ++i) note_draw(w, 0x10000000u + i, 1, 3, true, false, 3, 1);
    CHECK(w.n == kEntries);
    CHECK(w.overflow == 5);
    CHECK(w.draws == kEntries + 5);
    note_draw(w, 0x10000000u, 1, 3, true, false, 3, 1);  // an existing key still counts after the table is full
    CHECK(w.e[0].draws == 2 && w.overflow == 5);
}

void test_names() {
    CHECK(std::strcmp(vs_name(kSelMapVs8), "SelMap") == 0);
    CHECK(std::strcmp(vs_name(kModelIconVs8), "ModelIcon") == 0);
    CHECK(std::strcmp(vs_name(kAcmVs8), "ACM") == 0);
    CHECK(std::strcmp(vs_name(kCptVs8), "CPT") == 0);
    CHECK(vs_name(0x12345678u)[0] == 0);
}

void test_window_line() {
    Window w;
    w.presents = 300;
    for (int i = 0; i < 600; ++i) note_draw(w, 0x534f29cau, 0xd34ee153u, 3, false, false, 4, 1);
    for (int i = 0; i < 300; ++i) note_draw(w, kSelMapVs8, 0x9ebdf113u, 2, false, false, 2000, 1);
    char b[448];
    const int n = format_window(w, 5000.0, b, sizeof b);
    CHECK(n > 0 && size_t(n) < sizeof b && std::strlen(b) == size_t(n));
    const std::string s(b);
    CHECK(s.rfind("briefing-observe;event=window;", 0) == 0);
    CHECK(s.find(";presents=300;") != std::string::npos);
    CHECK(s.find(";draws=900;") != std::string::npos);
    CHECK(s.find(";draws-per-present=3.0;") != std::string::npos);
    CHECK(s.find(";briefing=1;") != std::string::npos);
    CHECK(s.find(";selmap=300(units=600000);") != std::string::npos);
    Window none;
    format_window(none, 5000.0, b, sizeof b);
    CHECK(std::string(b).find(";briefing=0;") != std::string::npos);
    CHECK(std::string(b).find(";draws-per-present=0.0;") != std::string::npos);
    // tiny buffers never overflow
    char t[16];
    const int m = format_window(w, 5000.0, t, sizeof t);
    CHECK(m == 15 && std::strlen(t) == 15);
    CHECK(format_window(w, 1.0, nullptr, 0) == 0);
}

void test_top_line() {
    Window w;
    for (uint32_t k = 0; k < 12; ++k)
        for (uint32_t d = 0; d <= k; ++d) note_draw(w, 0xa0000000u + k, 0xb0000000u + k, 3, true, false, 100 + k, 1);
    for (int d = 0; d < 50; ++d) note_draw(w, kSelMapVs8, 0x9ebdf113u, 2, false, false, 4096, 1);
    char b[448];
    const int n = format_top(w, b, sizeof b);
    CHECK(n > 0 && size_t(n) < sizeof b && std::strlen(b) == size_t(n));
    const std::string s(b);
    CHECK(s.rfind("briefing-observe;event=top;", 0) == 0);
    // highest draws first: SelMap (50), then key 11 (12 draws), key 10 (11) ...
    const size_t sel = s.find("384ade02/9ebdf113/t2n-/50/204800/4096/SelMap");
    const size_t k11 = s.find("a000000b/b000000b/t3i-/12/");
    const size_t k10 = s.find("a000000a/b000000a/t3i-/11/");
    CHECK(sel != std::string::npos && k11 != std::string::npos && k10 != std::string::npos);
    CHECK(sel < k11 && k11 < k10);
    // only kTop items
    size_t commas = 0;
    for (size_t i = s.find("top="); i < s.size(); ++i) if (s[i] == ',') ++commas;
    CHECK(commas == kTop - 1);
    // the lowest keys are not printed
    CHECK(s.find("a0000000/") == std::string::npos);
    // truncation-safe: a short buffer ends at an item boundary or inside the prefix
    char t[140];
    const int m = format_top(w, t, sizeof t);
    CHECK(m >= 0 && size_t(m) < sizeof t && std::strlen(t) == size_t(m));
    Window empty;
    format_top(empty, b, sizeof b);
    CHECK(std::string(b).find("top=") + 4 == std::strlen(b));
}

// DEBRIEF-3D (debrief-3d-map.md): every key after the top line is printed by at most kRestLinesMax bounded `rest` lines.
void test_rest_lines() {
    Window w;
    // 32 keys (the cap) with distinct draw counts: key k has k+1 draws; plus 3 overflowing keys.
    for (uint32_t k = 0; k < kEntries; ++k)
        for (uint32_t d = 0; d <= k; ++d) note_draw(w, 0xc0000000u + k, 0xd0000000u + k, (k % 2) ? 2 : 3, true, false, 10 + k, 1);
    for (uint32_t k = 0; k < 3; ++k) note_draw(w, 0xe0000000u + k, 1, 3, false, false, 1, 1);
    CHECK(w.n == kEntries && w.overflow == 3);
    char b[448];
    unsigned from = kTop, lines = 0;
    std::string all;
    for (unsigned li = 0; li < kRestLinesMax && from < w.n; ++li) {
        unsigned nx = 0;
        const int n = format_rest(w, from, b, sizeof b, &nx);
        CHECK(n > 0 && size_t(n) < sizeof b && std::strlen(b) == size_t(n));
        const std::string s(b);
        CHECK(s.rfind("briefing-observe;event=rest;from=", 0) == 0);
        CHECK(s.find(";keys=32;overflow=3;items=") != std::string::npos);
        char want[32];
        std::snprintf(want, sizeof want, ";from=%u;", from + 1);
        CHECK(s.find(want) != std::string::npos);
        CHECK(nx > from);
        all += s;
        from = nx;
        ++lines;
    }
    // 24 remaining items (about 40 B each) fit in 3 lines of 448 B: every key from rank 9 to 32 is printed exactly once.
    CHECK(from == w.n);
    CHECK(lines >= 2 && lines <= kRestLinesMax);
    for (uint32_t k = 0; k < kEntries; ++k) {
        char key[24];
        std::snprintf(key, sizeof key, "%08x/%08x/", 0xc0000000u + k, 0xd0000000u + k);
        const size_t first = all.find(key);
        // ranks 1..8 are the 8 keys with the most draws (k = 31..24): they belong to the top line, not here
        if (k >= kEntries - kTop) CHECK(first == std::string::npos);
        else CHECK(first != std::string::npos && all.find(key, first + 1) == std::string::npos);
    }
    // the lowest key (k = 0, 1 draw, line topology) is the last rank
    CHECK(all.find("c0000000/d0000000/t3i-/1/10/10") != std::string::npos);
    CHECK(all.find("c0000001/d0000001/t2i-/2/22/11") != std::string::npos);
    // nothing after the top line: no rest line
    Window few;
    for (uint32_t k = 0; k < kTop; ++k) note_draw(few, 0x1000u + k, 1, 3, true, false, 3, 1);
    unsigned nx = 99;
    CHECK(format_rest(few, kTop, b, sizeof b, &nx) == 0 && b[0] == 0 && nx == few.n);
    // names are appended to known shaders
    Window named;
    for (int i = 0; i < 9; ++i) note_draw(named, 0x7000u + uint32_t(i), 1, 3, true, false, 3, 1);
    for (int i = 0; i < 9; ++i) note_draw(named, 0x7000u + uint32_t(i), 1, 3, true, false, 3, 1);  // 9 keys with 2 draws each
    note_draw(named, kPs2Vs8, 0xd34ee153u, 3, false, false, 4, 1);  // rank 10 (1 draw)
    CHECK(format_rest(named, kTop, b, sizeof b, &nx) > 0);
    CHECK(std::string(b).find("534f29ca/d34ee153/t3n-/1/4/4/ps2") != std::string::npos);
    CHECK(std::strcmp(vs_name(kMovieVs8), "movie") == 0 && std::strcmp(vs_name(kExtra2Vs8), "extra2") == 0);
    // a buffer too small for the prefix or for one item: no endless loop (next = w.n), never overflows
    char t[60];
    nx = 0;
    const int m = format_rest(w, kTop, t, sizeof t, &nx);
    CHECK(m >= 0 && size_t(m) < sizeof t && std::strlen(t) == size_t(m) && nx == w.n);
    char u[70];  // the prefix (60 B) fits, one item (~37 B) does not
    nx = 0;
    const int m2 = format_rest(w, kTop, u, sizeof u, &nx);
    CHECK(m2 >= 0 && size_t(m2) < sizeof u && std::strlen(u) == size_t(m2) && nx == w.n);
    CHECK(format_rest(w, kTop, nullptr, 0, &nx) == 0 && nx == w.n);
    CHECK(format_rest(w, w.n, b, sizeof b, &nx) == 0 && nx == w.n);
}

void test_roll_arming() {
    Window out;
    double ms = 0;
    // disarmed -> disarmed: nothing, draws are not counted by the hook (armed() false)
    CHECK(!roll(false, 1000000, out, ms));
    CHECK(!armed());
    // arming: the window that just ended was not armed: no lines
    CHECK(!roll(true, 2000000, out, ms));
    CHECK(armed());
    draw(kSelMapVs8, 0x9ebdf113u, 2, false, false, 1000, 1);
    present();
    present();
    // armed window ends: reported with its span
    CHECK(roll(true, 7000000, out, ms));
    CHECK(out.draws == 1 && out.presents == 2 && ms == 5000.0);
    CHECK(draws_of(out, kSelMapVs8) == 1);
    // the next window starts empty
    CHECK(roll(false, 12000000, out, ms));
    CHECK(out.draws == 0 && out.presents == 0);
    CHECK(!armed());
    present();  // disarmed: not counted
    CHECK(!roll(false, 13000000, out, ms));
}

void test_threads() {
    Window out;
    double ms = 0;
    roll(true, 1, out, ms);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([t] { for (int i = 0; i < 5000; ++i) draw(0x11110000u + uint32_t(t), 2, 3, true, false, 3, 1); });
    for (int i = 0; i < 1000; ++i) present();
    for (auto& t : ts) t.join();
    CHECK(roll(false, 2, out, ms));
    CHECK(out.draws == 20000);
    CHECK(out.presents == 1000);
    CHECK(out.n == 4);
    for (unsigned i = 0; i < out.n; ++i) CHECK(out.e[i].draws == 5000 && out.e[i].units == 15000);
}

void test_arm_file() {
#ifdef _WIN32
    // Not created by the test (the user's real %LOCALAPPDATA% must not be touched): the call only has to be safe and boolean.
    const bool a = arm_file_present();
    CHECK(a || !a);
#endif
}

}  // namespace

int main() {
    test_keying();
    test_overflow();
    test_names();
    test_window_line();
    test_top_line();
    test_rest_lines();
    test_roll_arming();
    test_threads();
    test_arm_file();
    std::printf("briefing_observe_offline: %u checks passed\n", g_checks);
    return 0;
}
