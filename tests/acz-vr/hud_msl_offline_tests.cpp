// hud_msl_offline_tests.cpp -- MSL-HUD (hud_msl.h; docs/world-render/aircraft-auto.md section 14): the HUD MSSL count read from the game's glyph quads.
// No GPU, no game. REAL data: the step171 null salvo run's first glyph dump (98 glyph records, MSSL --50 / SAAM ---8 / DMG --0%) is embedded
// below (kStep171Dump1) and must decode to the gameshot values; its MSSL field re-textured with the table's 49..46 must give 4 launches at the
// step171 tap ticks. The other frames are SYNTHETIC, laid out from the step167 gameshot g0070 with the measured value font (hud_msl.h kFont*).
// Cases: quad extraction (indexed strip with cuts, indexed list, non-indexed list, rejects), the block and the MSSL row with every special-weapon label
// shape (4 and 3 letters), distractors (pitch-ladder numbers in the MSSL row, TIME / ALT / SPEED / SCORE text), the selected weapon's larger glyphs,
// 50 -> 46 for a 4-tap salvo at 7 s spacing (launch events 0.3..1 s after each tap), glitch frames, missing frames, rearm, another HUD font
// (refused; then the DMG anchor), the option parse, the dump round trip and the replay tool.
//
// Replay tool (the next null salvo flight): hud_msl_offline_tests --replay <step5-producer.log>   (mslhud:2 dump lines) prints, per dump, the
// block rows (label/value glyph keys and cells) and the reading, and the full-frame dumps' glyphs by row: the material for a real font table.
#include "hud_msl.h"
#include "store_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

using namespace acz::hudmsl;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

// ---------------------------------------------------------------------------------------------------------------- synthetic font + HUD
struct Atlas {   // the game's HUD atlas as measured (step171): digits / '-' / '%' on the value row, letters on row 0, the rest on row 1
    float row_v = kFontRowV, zero_u = kFontZeroU;   // other_font(): the digits on another row and offset (a different HUD font)
    static Atlas game() { return Atlas{}; }
    static Atlas other_font() { Atlas a; a.row_v = 2048.0f / 4096.0f; a.zero_u = 400.0f / 4096.0f; return a; }
    void uv(char ch, float& u0, float& v0, float& u1, float& v1) const {
        const float w = 112.0f / 4096.0f, hv = 176.0f / 4096.0f;
        if (ch >= '0' && ch <= '9') { u0 = zero_u + float(ch - '0') * kFontPitch; v0 = row_v; }
        else if (ch == '-') { u0 = kFontDashU; v0 = row_v; }
        else if (ch == '%') { u0 = 496.0f / 4096.0f; v0 = row_v; }
        else if (ch >= 'A' && ch <= 'Z') { u0 = float(ch - 'A') * 112.0f / 4096.0f; v0 = 0.0f; }
        else { u0 = float(int(ch) & 31) * 128.0f / 4096.0f; v0 = 1024.0f / 4096.0f; }
        u1 = u0 + w; v1 = v0 + hv;
    }
};
static const DigitFont kNoDmg{0.0f, 0.0f, kFontPitch, false};
struct Text { std::string s; float xl, cy, adv, w, h; };   // xl = the first glyph's centre x

// One indexed strip draw (4 vertices + a cut per glyph; index values from 100 to exercise the slot map), as the game's sprite text draws.
static void draw_text(GlyphFrame& f, Scratch& sc, const Atlas& a, const std::vector<Text>& items) {
    unsigned nv = 0, ni = 0;
    for (const Text& t : items)
        for (size_t k = 0; k < t.s.size(); ++k) {
            if (t.s[k] == ' ') continue;
            const float cx = t.xl + float(k) * t.adv;
            float u0, v0, u1, v1; a.uv(t.s[k], u0, v0, u1, v1);
            const float xs[4] = {cx - 0.5f * t.w, cx + 0.5f * t.w, cx - 0.5f * t.w, cx + 0.5f * t.w};
            const float ys[4] = {t.cy - 0.5f * t.h, t.cy - 0.5f * t.h, t.cy + 0.5f * t.h, t.cy + 0.5f * t.h};
            const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
            for (int c = 0; c < 4; ++c) {
                Vtx& v = sc.v[nv]; v = Vtx{}; v.x = xs[c]; v.y = ys[c]; v.u = us[c]; v.v = vs[c]; v.rgb[0] = 0.4f; v.rgb[1] = 1.0f; v.rgb[2] = 0.5f;
                sc.uniq[nv] = 100 + nv; sc.stream[ni++] = 100 + nv; ++nv;
            }
            sc.stream[ni++] = kCut;
        }
    add_draw(f, sc, nv, ni, true, kTopoStrip, uint16_t(f.draws));
}

struct Hud {
    std::string gun = "----", msl = "--50", sp_label = "SAAM", sp = "---8", dmg = "--0%";
    bool big_msl = true;       // the selected weapon's field drawn larger (g0070)
    bool distract = true;      // TIME / ALT / SPEED / SCORE / ladder numbers, one in the MSSL row
    bool show_msl_row = true;
};
// GS layout from g0070 (x = 1728 + 640 px / 2405, y = 1824 + 448 (py - 30) / 1323): rows y 2129 / 2150 / 2171 / 2192, value glyph centres 2309.5..2341
// (advance 10.5), labels right aligned to a last centre at 2283 (advance 9.6), glyph 8 x 11 GS.
static void build(GlyphFrame& f, Scratch& sc, const Atlas& a, const Hud& h) {
    f.clear();
    const float ry[4] = {2129, 2150, 2171, 2192};
    auto label = [&](const std::string& s, float y) { return Text{s, 2283.0f - float(s.size() - 1) * 9.6f, y, 9.6f, 8.0f, 11.0f}; };
    auto value = [&](const std::string& s, float y, bool big) { return Text{s, 2309.5f, y, 10.5f, big ? 9.2f : 8.0f, big ? 12.6f : 11.0f}; };
    std::vector<Text> labels = {label("GUN", ry[0]), label(h.sp_label, ry[2]), label("DMG", ry[3])};
    if (h.show_msl_row) labels.push_back(label("MSSL", ry[1]));
    draw_text(f, sc, a, labels);
    std::vector<Text> values = {value(h.gun, ry[0], false), value(h.sp, ry[2], false), value(h.dmg, ry[3], false)};
    if (h.show_msl_row) values.push_back(value(h.msl, ry[1], h.big_msl));
    draw_text(f, sc, a, values);
    if (h.distract) {
        draw_text(f, sc, a, {Text{"TIME", 2245, 1872, 10, 8, 11}, Text{"17'54\"33", 2190, 1893, 10, 8, 11}, Text{"ALT", 2190, 2030, 10, 8, 11},
                             Text{"9052", 2200, 2052, 10, 8, 11}, Text{"SPEED", 1880, 2030, 10, 8, 11}, Text{"341", 1890, 2052, 10, 8, 11},
                             Text{"SCORE", 1880, 1872, 10, 8, 11}, Text{"0", 1950, 1893, 10, 8, 11}});
        draw_text(f, sc, a, {Text{"15", 2128, 2080, 10, 8, 11}, Text{"10", 2190, 2150, 10, 8, 11}, Text{"COVER", 1980, 2236, 10, 8, 11}});
        // the MSSL field box sides as thin textured sprites (2 x 16 GS) right of the last digit and left of the first: not glyphs
        draw_text(f, sc, a, {Text{"||", 2302.0f, 2150, 46.0f, 2.0f, 16.0f}});
    }
}

// ---------------------------------------------------------------------------------------------------------------- cases
static std::unique_ptr<Scratch> g_sc(new Scratch);
static std::unique_ptr<GlyphFrame> g_f(new GlyphFrame);
static std::unique_ptr<Work> g_w(new Work);

static void test_extract() {
    Scratch& sc = *g_sc; GlyphFrame& f = *g_f; const Atlas a = Atlas::game();
    f.clear();
    draw_text(f, sc, a, {Text{"--50", 2309.5f, 2150, 10.5f, 8, 11}});
    CHECK(f.n == 4 && f.draws == 1);
    CHECK(std::fabs(f.g[3].x0 - (2341.0f - 4.0f)) < 1e-3f && std::fabs(f.g[3].y1 - 2155.5f) < 1e-3f);
    { float u0, v0, u1, v1; a.uv('0', u0, v0, u1, v1); CHECK(f.g[3].u0 == u0 && f.g[3].v1 == v1); CHECK(digit_of(f.g[3], DigitFont{}) == 0 && digit_of(f.g[2], DigitFont{}) == 5); CHECK(digit_of(f.g[0], DigitFont{}) == -1); }
    CHECK(key_of(f.g[0]) == key_of(f.g[1]) && key_of(f.g[0]) != key_of(f.g[2]));
    // indexed triangle list: 6 indices per quad (0,1,2 / 2,1,3)
    f.clear();
    for (unsigned q = 0; q < 3; ++q) for (int c = 0; c < 4; ++c) {
        Vtx& v = sc.v[q * 4 + c]; v = Vtx{}; v.x = 2200.0f + 12.0f * float(q) + (c & 1 ? 8.0f : 0.0f); v.y = 2100.0f + (c & 2 ? 11.0f : 0.0f);
        v.u = 0.1f * float(q) + (c & 1 ? 0.0625f : 0.0f); v.v = 0.25f + (c & 2 ? 0.0625f : 0.0f); sc.uniq[q * 4 + c] = q * 4 + c;
    }
    unsigned ni = 0;
    for (unsigned q = 0; q < 3; ++q) { const uint32_t b = q * 4; for (uint32_t x : {b, b + 1, b + 2, b + 2, b + 1, b + 3}) sc.stream[ni++] = x; }
    add_draw(f, sc, 12, ni, true, kTopoList, 0);
    CHECK(f.n == 3);
    // non-indexed list: 6 vertices per quad, the shared corners joined by value
    f.clear();
    unsigned nv = 0;
    for (unsigned q = 0; q < 2; ++q) {
        const float x = 2200.0f + 12.0f * float(q);
        const float c[6][4] = {{x, 2100, 0, 0}, {x + 8, 2100, .0625f, 0}, {x, 2111, 0, .0625f}, {x, 2111, 0, .0625f}, {x + 8, 2100, .0625f, 0}, {x + 8, 2111, .0625f, .0625f}};
        for (int k = 0; k < 6; ++k) { Vtx& v = sc.v[nv++]; v = Vtx{}; v.x = c[k][0]; v.y = c[k][1]; v.u = c[k][2] + 0.25f * float(q); v.v = c[k][3]; }
    }
    add_draw(f, sc, nv, 0, false, kTopoList, 0);
    CHECK(f.n == 2);
    // rejects: an untextured quad (a box side), a too-large panel, a line topology
    f.clear();
    for (int c = 0; c < 4; ++c) { Vtx& v = sc.v[c]; v = Vtx{}; v.x = 2200.0f + (c & 1 ? 8.0f : 0.0f); v.y = 2100.0f + (c & 2 ? 11.0f : 0.0f); sc.uniq[c] = uint32_t(c); sc.stream[c] = uint32_t(c); }
    add_draw(f, sc, 4, 4, true, kTopoStrip, 0);
    CHECK(f.n == 0 && f.rej_untextured == 1);
    for (int c = 0; c < 4; ++c) { Vtx& v = sc.v[c]; v.x = 2000.0f + (c & 1 ? 200.0f : 0.0f); v.u = c & 1 ? 0.5f : 0.0f; v.v = c & 2 ? 0.5f : 0.0f; }
    add_draw(f, sc, 4, 4, true, kTopoStrip, 0);
    CHECK(f.n == 0 && f.rej_size == 1);
    add_draw(f, sc, 4, 4, true, 3u /* line strip */, 0);
    CHECK(f.n == 0 && f.rej_shape == 1);
    // an index outside the read vertices joins nothing; the stream cut separates quads
    std::printf("extract ok\n");
}

static void test_block_and_reading() {
    Scratch& sc = *g_sc; GlyphFrame& f = *g_f; Work& w = *g_w; const Atlas a = Atlas::game();
    build(f, sc, a, Hud{});
    const Block b = find_block(f, w);
    CHECK(!b.why && b.nrows == 4);
    CHECK(b.row[0].label_n == 3 && b.row[1].label_n == 4 && b.row[2].label_n == 4 && b.row[3].label_n == 3);   // the ladder "10" in the MSSL row is not label
    const Reading r = read_frame(f, w, kNoDmg, anchor_none);
    CHECK(r.ok && r.value == 50 && r.sp == 8 && r.dmg == 0 && r.anchor == anchor_table);
    // every special-weapon label shape: 4 letters (pairs anywhere) and 3 letters; the MSSL row is still the second
    for (const char* sp : {"QAAM", "XLAA", "UGBL", "LAGM", "FAEB", "XMAA", "RCL", "GPB", "TLS", "EML", "SOD"}) {
        Hud h; h.sp_label = sp; h.sp = "--16"; h.msl = "-100"; build(f, sc, a, h);
        const Reading x = read_frame(f, w, kNoDmg, anchor_none);
        CHECK(x.ok && x.value == 100 && x.sp == 16);
    }
    // no distractors, small MSSL glyphs (another weapon selected), 1- and 3-digit counts, damage on
    for (const char* m : {"---0", "---9", "--12", "-120"}) {
        Hud h; h.msl = m; h.big_msl = false; h.distract = false; h.dmg = "-35%"; build(f, sc, a, h);
        const Reading x = read_frame(f, w, kNoDmg, anchor_none);
        CHECK(x.ok && x.value == uint32_t(std::atoi(m + std::strspn(m, "-"))) && x.dmg == 35);
    }
    // refusals: all dashes; the MSSL row missing (3 rows: never the special weapon instead); a 5-glyph field; a label that is not M S S L
    { Hud h; h.msl = "----"; build(f, sc, a, h); CHECK(!read_frame(f, w, kNoDmg, anchor_none).ok); }
    { Hud h; h.show_msl_row = false; build(f, sc, a, h); const Reading x = read_frame(f, w, kNoDmg, anchor_none); CHECK(!x.ok && std::strcmp(x.why, "rows-under-4") == 0); }
    { Hud h; h.sp_label = "MSSL"; h.msl = "--50"; build(f, sc, a, h); CHECK(read_frame(f, w, kNoDmg, anchor_none).value == 50); }
    { Hud h; h.sp_label = "SAAM"; build(f, sc, a, h);
      // swap the MSSL label for "MSL " shape (3 glyphs): refused
      Hud h2; h2.distract = false; build(f, sc, a, h2);
      for (unsigned i = 0; i < f.n; ++i) { const float cy = 0.5f * (f.g[i].y0 + f.g[i].y1), cx = 0.5f * (f.g[i].x0 + f.g[i].x1); if (std::fabs(cy - 2150) < 1 && cx < 2260) { f.g[i] = f.g[f.n - 1]; --f.n; break; } }
      const Reading x = read_frame(f, w, kNoDmg, anchor_none); CHECK(!x.ok && std::strcmp(x.why, "msl-label-not-4") == 0); }
    std::printf("block ok\n");
}

// The 4-tap salvo of step167/170 (-FireSalvo 4 -FireEvery 7 -FireAt 70): taps at 70, 77, 84, 91 s; the HUD falls by one 0.4 s after each tap.
static void test_salvo() {
    Scratch& sc = *g_sc; GlyphFrame& f = *g_f; Work& w = *g_w; const Atlas a = Atlas::game();
    Decoder d; acz::stores::Signal sig;
    const double taps[4] = {70000, 77000, 84000, 91000};
    std::vector<std::pair<double, Event>> evs;
    for (double t = 0; t <= 100000; t += 100.0) {
        int fired = 0; for (double tp : taps) if (t >= tp + 400.0) ++fired;
        Hud h; char m[8]; std::snprintf(m, sizeof m, "--%02d", 50 - fired); h.msl = m;
        if (std::fmod(t, 3700.0) < 100.0) h.distract = false;                           // the text set varies
        if (t > 30000 && t < 30300) { f.clear(); d.frame(f, w, t); continue; }           // HUD gone (pause / radio box): nothing happens
        if (std::fabs(t - 50000.0) < 1.0) h.msl = "--40";                                // one garbage frame: never believed
        build(f, sc, a, h);
        const Event e = d.frame(f, w, t);
        if (e.launches || e.rearm || e.rebase) evs.push_back({t, e});
        for (unsigned i = 0; i < e.launches; ++i) sig.note_launch();
        if (e.rearm) sig.note_rearm();
    }
    CHECK(d.has_baseline() && d.baseline() == 46);
    CHECK(evs.size() == 4);
    for (size_t i = 0; i < evs.size() && i < 4; ++i) {
        const double dt = evs[i].first - taps[i];
        CHECK(evs[i].second.launches == 1 && !evs[i].second.rearm && dt >= 300.0 && dt <= 1000.0);
        CHECK(evs[i].second.old_value == 50 - i && evs[i].second.new_value == 49 - i);
    }
    CHECK(sig.launches() == 4 && d.stats().launches == 4 && d.stats().rearms == 0);
    // the summary fits a log line (step-1 text 448 - "gpu-copy-step1;event=") and names the value
    char b[512];
    const int n = format_summary(b, sizeof b, 100000, 1, d);
    CHECK(n > 0 && n < 426 && std::strstr(b, "summary-msl-hud;") == b && std::strstr(b, ";value=46;") && std::strstr(b, ";launches=4;"));
    char e[200]; format_event(e, sizeof e, 77400, evs[1].second); CHECK(std::strcmp(e, "msl-hud;event=launch;old=49;new=48;launches=1;t-ms=77400") == 0);
    // rearm (refill / respawn) and a two-missile step in one sample gap
    for (double t = 100100; t <= 102000; t += 100.0) { Hud h; h.msl = "--50"; build(f, sc, a, h); const Event x = d.frame(f, w, t); if (x.rearm) sig.note_rearm(); }
    CHECK(d.baseline() == 50 && d.stats().rearms == 1);
    unsigned launches = 0;
    for (double t = 102100; t <= 104000; t += 100.0) { Hud h; h.msl = "--48"; build(f, sc, a, h); launches += d.frame(f, w, t).launches; }
    CHECK(launches == 2 && d.baseline() == 48);
    // a fall bigger than 8 is a rebaseline counting one launch, not 20 (and not 0: step171 lost tap 2 that way)
    unsigned rb = 0;
    for (double t = 104100; t <= 106000; t += 100.0) { Hud h; h.msl = "--28"; build(f, sc, a, h); const Event x = d.frame(f, w, t); launches += x.launches; rb += x.rebase; }
    CHECK(launches == 3 && rb == 1 && d.baseline() == 28);   // a misread / big fall still counts ONE launch
    std::printf("salvo ok\n");
}

// Another HUD font (digits on another atlas row and offset): the table refuses it (no reading, nothing signalled), the raw tuple changes are still
// counted; the DMG ones digit (damage 0 during the first frames) then anchors the digits with the same pitch and the count reads right.
// SP-STORES: the special-weapon row (SAAM ---8) on its own hysteresis: MSSL 50 unchanged, SP 8 -> 7 at 70.4 s and -> 6 at 77.4 s (taps 70 s / 77 s), plus a MSSL
// shot at 84 s and an SP refill at 95 s. BEFORE this change the decoder read the SP value (Reading::sp) and discarded it: no sp_event, no sp counters, no SP lines.
static void test_sp_salvo() {
    Scratch& sc = *g_sc; GlyphFrame& f = *g_f; Work& w = *g_w; const Atlas a = Atlas::game();
    Decoder d; acz::stores::Signal sig;
    std::vector<std::pair<double, Event>> evs; unsigned mssl_launches = 0;
    for (double t = 0; t <= 100000; t += 100.0) {
        const int sp = t >= 95400.0 ? 8 : t >= 77400.0 ? 6 : t >= 70400.0 ? 7 : 8;
        const int msl = t >= 84400.0 ? 49 : 50;
        Hud h; char m[8], s[8]; std::snprintf(m, sizeof m, "--%02d", msl); h.msl = m; std::snprintf(s, sizeof s, "---%d", sp); h.sp = s;
        if (t > 20000 && t < 20300) h.sp = "----";                                      // the SP field blanks for a moment: nothing happens (no weapon switch event)
        if (std::fabs(t - 50000.0) < 1.0) h.sp = "---3";                                // one garbage frame: never believed
        if (t > 30000 && t < 30300) { f.clear(); d.frame(f, w, t); continue; }           // HUD gone
        build(f, sc, a, h);
        const Event e = d.frame(f, w, t);
        if (e.launches) ++mssl_launches;
        const Event& se = d.sp_event();
        if (se.launches || se.rearm || se.rebase) evs.push_back({t, se});
        for (unsigned i = 0; i < se.launches; ++i) sig.note_launch();
        if (se.rearm) sig.note_rearm();
    }
    CHECK(d.has_baseline() && d.baseline() == 49 && mssl_launches == 1 && d.stats().launches == 1);   // the MSSL decoder is unchanged by the SP row
    CHECK(d.sp().has_baseline() && d.sp().baseline() == 8);                                        // after the refill
    CHECK(evs.size() == 3 && d.sp().launches() == 2 && d.sp().rearms() == 1 && d.sp().rebases() == 0);
    CHECK(!evs[0].second.rearm && evs[0].second.launches == 1 && evs[0].second.old_value == 8 && evs[0].second.new_value == 7 && evs[0].first >= 70700.0 && evs[0].first <= 71400.0);
    CHECK(!evs[1].second.rearm && evs[1].second.launches == 1 && evs[1].second.old_value == 7 && evs[1].second.new_value == 6 && evs[1].first >= 77700.0 && evs[1].first <= 78400.0);
    CHECK(evs[2].second.rearm && evs[2].second.old_value == 6 && evs[2].second.new_value == 8);
    CHECK(sig.launches() == 2);
    // a fall of more than kMaxLaunches is one launch and a rebase; an undecodable field (v < 0) never moves anything
    { StableCount c; double t = 0; Event e;
      auto stable = [&](int v) { Event last; for (int i = 0; i < 40; ++i, t += 100.0) { e = c.feed(v, t); if (e.launches || e.rearm) last = e; } return last; };
      CHECK(!stable(16).launches && c.has_baseline() && c.baseline() == 16);
      const Event r = stable(5); CHECK(r.rebase && r.launches == 1 && c.baseline() == 5 && c.rebases() == 1 && c.launches() == 1);
      CHECK(!stable(-1).launches && c.baseline() == 5);
      for (int i = 0; i < 10; ++i, t += 100.0) c.feed(-1, t);
      const Event up = stable(9); CHECK(up.rearm && c.baseline() == 9 && c.rearms() == 1); }
    // the log lines: unconditional summary with the stable value, the event line names sp- (the MSSL ones stay as they were)
    char b[200]; const int n = format_sp_summary(b, sizeof b, 100000, d);
    CHECK(n > 0 && n < 200 && std::strstr(b, "summary-msl-hud-sp;") == b && std::strstr(b, ";sp-value=8;") && std::strstr(b, ";sp-launches=2;") && std::strstr(b, ";sp-rearms=1;"));
    char e[200]; format_sp_event(e, sizeof e, 70900, evs[0].second); CHECK(std::strcmp(e, "msl-hud;event=sp-launch;old=8;new=7;launches=1;t-ms=70900") == 0);
    format_sp_event(e, sizeof e, 95800, evs[2].second); CHECK(std::strstr(e, "event=sp-rearm;") != nullptr);
    Decoder fresh; CHECK(format_sp_summary(b, sizeof b, 1, fresh) > 0 && std::strstr(b, ";sp-last=-1;sp-value=none;sp-launches=0;"));   // a silent reader is told from a dead one: the line exists
    std::printf("sp ok\n");
}

static void test_other_font() {
    Scratch& sc = *g_sc; GlyphFrame& f = *g_f; Work& w = *g_w;
    const Atlas a = Atlas::other_font();
    {
        Hud h; build(f, sc, a, h);
        const Reading r = read_frame(f, w, kNoDmg, anchor_table);
        CHECK(!r.ok && std::strcmp(r.why, "digit-not-in-table") == 0);
    }
    Decoder d;
    for (double t = 0; t < 3000; t += 100.0) { Hud h; build(f, sc, a, h); d.frame(f, w, t); }
    CHECK(d.dmg_font().valid && std::fabs(d.dmg_font().zero_u - a.zero_u) < 1e-6f && d.has_baseline() && d.baseline() == 50 && d.anchor() == anchor_dmg);
    unsigned launches = 0;
    for (double t = 3000; t < 5000; t += 100.0) { Hud h; h.msl = "--49"; build(f, sc, a, h); launches += d.frame(f, w, t).launches; }
    CHECK(launches == 1 && d.baseline() == 49 && d.stats().tuple_changes >= 1);
    std::printf("other font ok\n");
}

static void test_option() {
    CHECK(parse_mode(nullptr) == 0 && parse_mode("") == 0);
    CHECK(parse_mode("auto") == 1 && parse_mode("auto;stwatch:1;vrn:1") == 1);   // default ON (step172 live pass): no token = on; mslhud:0 is the off switch
    CHECK(parse_mode("auto;mslhud:0") == 0 && parse_mode("auto;mslhud=0") == 0 && parse_mode("auto;mslhud:2;x:1") == 2 && parse_mode("auto;mslhud:1") == 1);
    CHECK(parse_mode("auto;stores:0;mslhud:1") == 0 && parse_mode("auto;mslhud:1;stores:0") == 0 && parse_mode("auto;stores:1;mslhud:1") == 1 && parse_mode("auto;stores:1") == 1 && parse_mode("auto;stores:0") == 0);
    CHECK(valid_mode_value("0") && valid_mode_value("2") && !valid_mode_value("3") && !valid_mode_value("12") && !valid_mode_value(""));
    // SP-STORES: on with the reader, `spstores:0` is the off switch, the reader off (mslhud:0 / stores:0 / no option string) turns it off too
    CHECK(!parse_sp(nullptr) && !parse_sp("") && parse_sp("auto") && parse_sp("auto;spstores:1;mslhud:2") && parse_sp("auto;stwatch:1"));
    CHECK(!parse_sp("auto;spstores:0") && !parse_sp("auto;spstores=0") && !parse_sp("auto;mslhud:1;spstores:0;strel:2") && !parse_sp("auto;mslhud:0") && !parse_sp("auto;stores:0"));
    std::printf("option ok\n");
}

// ---------------------------------------------------------------------------------------------------------------- dump replay (the analysis tool)
struct DumpFrame { unsigned id = 0; double t = 0; std::string head; std::vector<Glyph> g; };
static std::vector<DumpFrame> parse_dump(std::istream& in) {
    std::vector<DumpFrame> out; std::string line;
    while (std::getline(in, line)) {
        const size_t h = line.find("msl-hud-dump;dump=");
        if (h != std::string::npos) {
            DumpFrame d; d.id = unsigned(std::strtoul(line.c_str() + h + 18, nullptr, 10)); d.head = line.substr(h);
            const size_t tp = line.find(";t-ms=", h); if (tp != std::string::npos) d.t = std::atof(line.c_str() + tp + 6);
            out.push_back(d); continue;
        }
        const size_t gl = line.find("msl-hud-glyph;dump=");
        if (gl == std::string::npos) continue;
        const unsigned id = unsigned(std::strtoul(line.c_str() + gl + 19, nullptr, 10));
        const size_t gp = line.find(";g=", gl); if (gp == std::string::npos) continue;
        DumpFrame* d = nullptr; for (auto it = out.rbegin(); it != out.rend(); ++it) if (it->id == id) { d = &*it; break; }
        if (!d) continue;
        std::string rest = line.substr(gp + 3);
        size_t p = 0;
        while (p < rest.size()) {
            size_t e = rest.find('|', p); if (e == std::string::npos) e = rest.size();
            Glyph g; if (parse_glyph(rest.substr(p, e - p).c_str(), g)) d->g.push_back(g);
            p = e + 1;
        }
    }
    return out;
}
static Reading replay_one(const DumpFrame& d, bool print) {
    GlyphFrame& f = *g_f; Work& w = *g_w;
    f.clear(); for (const Glyph& g : d.g) if (f.n < kGlyphCap) f.g[f.n++] = g;
    const Reading r = read_frame(f, w, kNoDmg, anchor_none);
    if (!print) return r;
    std::printf("dump %u t-ms=%.0f glyphs=%u: %s\n", d.id, d.t, f.n, d.head.c_str());
    const Block& b = r.block;
    std::printf("  block: cand=%u y-rows=%u fields=%u aligned=%u why=%s\n", b.cand, b.rows, b.fields, b.group, b.why ? b.why : "ok");
    for (unsigned i = 0; i < b.nrows; ++i) {
        const Field& fl = b.row[i];
        std::printf("  row %u y=%.1f adv=%.1f h=%.1f xr=%.1f label(%u):", i, fl.cy, fl.adv, fl.h, fl.xr, fl.label_n);
        for (unsigned k = 0; k < fl.label_n; ++k) std::printf(" %08X/c%d", key_of(f.g[fl.label[k]]), cell_of(f.g[fl.label[k]]));
        std::printf("  value:");
        for (int k = 0; k < 4; ++k) std::printf(" %08X/d%d", key_of(f.g[fl.val[k]]), digit_of(f.g[fl.val[k]], DigitFont{}));
        std::printf("\n");
    }
    std::printf("  reading: %s value=%d sp=%d dmg=%d anchor=%s dash=%08X zero-u=%.0f\n", r.ok ? "ok" : r.why, r.ok ? int(r.value) : -1, r.sp, r.dmg,
                anchor_name(r.anchor), r.dash, double(r.font.zero_u) * 4096.0);
    return r;
}
static int replay_main(const char* path) {
    std::ifstream in(path);
    if (!in) { std::printf("cannot open %s\n", path); return 2; }
    const std::vector<DumpFrame> ds = parse_dump(in);
    std::printf("%zu dumps\n", ds.size());
    for (const DumpFrame& d : ds) {
        replay_one(d, true);
        if (d.head.find("scope=all") != std::string::npos) {   // the whole HUD font in use: glyphs by row (y), left to right, with key and cell
            std::vector<Glyph> g = d.g;
            std::sort(g.begin(), g.end(), [](const Glyph& a, const Glyph& b) { const float ya = std::round(0.5f * (a.y0 + a.y1) / 4), yb = std::round(0.5f * (b.y0 + b.y1) / 4);
                return ya < yb || (ya == yb && a.x0 < b.x0); });
            for (const Glyph& x : g) std::printf("    glyph x=%.1f..%.1f y=%.1f..%.1f uv=%.4f,%.4f..%.4f,%.4f key=%08X cell=%d digit=%d q=%.3g draw=%u\n",
                                                 x.x0, x.x1, x.y0, x.y1, x.u0, x.v0, x.u1, x.v1, key_of(x), cell_of(x), digit_of(x, DigitFont{}), x.q, unsigned(x.draw));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------- step171 regression (real glyphs)
// D:\acz-evidence-archive\step171\kill\runAGB2\logs\step5-producer.log, dump 1 (t-ms 292821156, before the salvo), verbatim (log prefix cut).
// Gameshots g0066-g0100: MSSL 50 until 02:17:50.8, 49 from 02:17:51.8, 48 from 02:17:58.9, 47 from 02:18:06.0, 46 from 02:18:13.2 (one step per tap;
// SAAM 8, DMG 0% throughout). The first build read this frame as 60 (ASCII-cell guess) and logged rebase 60->49, 49->48, 48->47 (launches=2, end 47).
static const char* kStep171Dump1 = R"DUMP(
msl-hud-dump;dump=1;frame=6;t-ms=292821156;scope=all;glyphs=98;listed=98;draws=63;unread=0;dropped=0;rej=37/49/11;why=ok;value=60
msl-hud-glyph;dump=1;first=0;g=1936.0,2042.0,1942.0,2052.0,304.0,-8.0,400.0,160.0,9F6,1,2|1927.0,2042.0,1933.0,2052.0,192.0,-8.0,288.0,160.0,9F6,1,2|1918.0,2042.0,1924.0,2052.0,304.0,-8.0,400.0,160.0,9F6,1,2|1905.0,2025.0,1911.0,2035.0,3112.0,-8.0,3224.0,168.0,9F6,1,2|1913.0,2025.0,1919.0,2035.0,2760.0,-8.0,2872.0,168.0,9F6,1,2
msl-hud-glyph;dump=1;first=5;g=1921.0,2025.0,1927.0,2035.0,1544.0,-8.0,1656.0,168.0,9F6,1,2|1929.0,2025.0,1935.0,2035.0,1544.0,-8.0,1656.0,168.0,9F6,1,2|1937.0,2025.0,1943.0,2035.0,1432.0,-8.0,1544.0,168.0,9F6,1,2|2153.0,2025.0,2159.0,2035.0,1096.0,-8.0,1208.0,168.0,9F6,1,4|2161.0,2025.0,2167.0,2035.0,2296.0,-8.0,2408.0,168.0,9F6,1,4
msl-hud-glyph;dump=1;first=10;g=2169.0,2025.0,2175.0,2035.0,3224.0,-8.0,3336.0,168.0,9F6,1,4|2155.0,2042.0,2159.0,2052.0,112.0,-8.0,176.0,160.0,9F6,1,4|2163.0,2042.0,2169.0,2052.0,192.0,-8.0,288.0,160.0,9F6,1,4|2172.0,2042.0,2178.0,2052.0,880.0,-8.0,976.0,160.0,9F6,1,4|2182.0,2042.0,2186.0,2052.0,112.0,-8.0,176.0,160.0,9F6,1,4
msl-hud-glyph;dump=1;first=15;g=2190.0,2042.0,2196.0,2052.0,544.0,-8.0,640.0,160.0,9F6,1,4|2038.4,1922.0,2044.4,1932.0,2536.0,-8.0,2648.0,168.0,9F6,1,8|2046.4,1922.0,2056.4,1932.0,3560.0,-8.0,3736.0,168.0,9F6,1,8|2038.5,2048.0,2057.5,2057.0,3552.0,1520.0,3936.0,1648.0,9F6,1,12|2038.5,2038.0,2057.5,2056.0,3264.0,1104.0,3648.0,1344.0,9F6,1,12
msl-hud-glyph;dump=1;first=20;g=1994.8,1954.4,1998.8,1963.4,1728.0,624.0,1792.0,768.0,9F6,1,12|2001.8,1954.4,2007.8,1963.4,1616.0,624.0,1712.0,768.0,9F6,1,12|2090.0,1954.4,2094.0,1963.4,1728.0,624.0,1792.0,768.0,9F6,1,12|2097.0,1954.4,2103.0,1963.4,1616.0,624.0,1712.0,768.0,9F6,1,12|1997.8,1999.2,2003.8,2008.2,2144.0,624.0,2240.0,768.0,9F6,1,15
msl-hud-glyph;dump=1;first=25;g=2093.0,1999.2,2099.0,2008.2,2144.0,624.0,2240.0,768.0,9F6,1,15|1997.8,2044.0,2003.8,2053.0,1616.0,624.0,1712.0,768.0,9F6,1,18|2093.0,2044.0,2099.0,2053.0,1616.0,624.0,1712.0,768.0,9F6,1,18|1997.8,2088.8,2003.8,2097.8,2144.0,624.0,2240.0,768.0,9F6,1,21|2093.0,2088.8,2099.0,2097.8,2144.0,624.0,2240.0,768.0,9F6,1,21
msl-hud-glyph;dump=1;first=30;g=1994.8,2133.6,1998.8,2142.6,1728.0,624.0,1792.0,768.0,9F6,1,25|2001.8,2133.6,2007.8,2142.6,1616.0,624.0,1712.0,768.0,9F6,1,25|2090.0,2133.6,2094.0,2142.6,1728.0,624.0,1792.0,768.0,9F6,1,25|2097.0,2133.6,2103.0,2142.6,1616.0,624.0,1712.0,768.0,9F6,1,25|1936.0,1940.0,1942.0,1950.0,-8.0,-8.0,96.0,160.0,9F6,1,29
msl-hud-glyph;dump=1;first=35;g=1905.0,1923.0,1911.0,1933.0,3112.0,-8.0,3224.0,168.0,9F6,1,31|1913.0,1923.0,1919.0,1933.0,1320.0,-8.0,1432.0,168.0,9F6,1,31|1921.0,1923.0,1927.0,1933.0,2648.0,-8.0,2760.0,168.0,9F6,1,31|1929.0,1923.0,1935.0,1933.0,3000.0,-8.0,3112.0,168.0,9F6,1,31|1937.0,1923.0,1943.0,1933.0,1544.0,-8.0,1656.0,168.0,9F6,1,31
msl-hud-glyph;dump=1;first=40;g=2153.0,1924.0,2159.0,1934.0,3224.0,-8.0,3336.0,168.0,9F6,1,31|2161.0,1924.0,2165.0,1934.0,1992.0,-8.0,2072.0,168.0,9F6,1,31|2167.0,1924.0,2174.0,1934.0,2408.0,-8.0,2536.0,168.0,9F6,1,31|2176.0,1924.0,2182.0,1934.0,1544.0,-8.0,1656.0,168.0,9F6,1,31|2155.0,1940.0,2159.0,1950.0,112.0,-8.0,176.0,160.0,9F6,1,31
msl-hud-glyph;dump=1;first=45;g=2163.0,1940.0,2169.0,1950.0,992.0,-8.0,1088.0,160.0,9F6,1,31|2175.0,1940.0,2181.0,1950.0,544.0,-8.0,640.0,160.0,9F6,1,31|2184.0,1940.0,2190.0,1950.0,656.0,-8.0,752.0,160.0,9F6,1,31|2193.0,1940.0,2196.0,1950.0,160.0,176.0,208.0,336.0,9F6,1,31|2198.0,1940.0,2204.0,1950.0,992.0,-8.0,1088.0,160.0,9F6,1,31
msl-hud-glyph;dump=1;first=50;g=2207.0,1940.0,2213.0,1950.0,304.0,-8.0,400.0,160.0,9F6,1,31|2215.0,2120.0,2221.0,2130.0,1768.0,-8.0,1880.0,168.0,9F6,1,33|2223.0,2120.0,2229.0,2130.0,3336.0,-8.0,3448.0,168.0,9F6,1,33|2231.0,2120.0,2237.0,2130.0,2536.0,-8.0,2648.0,168.0,9F6,1,33|2206.0,2140.0,2213.0,2150.0,2408.0,-8.0,2536.0,168.0,9F6,1,33
msl-hud-glyph;dump=1;first=55;g=2215.0,2140.0,2221.0,2150.0,3112.0,-8.0,3224.0,168.0,9F6,1,33|2223.0,2140.0,2229.0,2150.0,3112.0,-8.0,3224.0,168.0,9F6,1,33|2231.0,2140.0,2237.0,2150.0,2296.0,-8.0,2408.0,168.0,9F6,1,33|2206.0,2160.0,2212.0,2170.0,3112.0,-8.0,3224.0,168.0,9F6,1,33|2214.0,2160.0,2220.0,2170.0,1096.0,-8.0,1208.0,168.0,9F6,1,33
msl-hud-glyph;dump=1;first=60;g=2222.0,2160.0,2228.0,2170.0,1096.0,-8.0,1208.0,168.0,9F6,1,33|2230.0,2160.0,2237.0,2170.0,2408.0,-8.0,2536.0,168.0,9F6,1,33|2214.0,2181.0,2220.0,2191.0,1432.0,-8.0,1544.0,168.0,9F6,1,33|2222.0,2181.0,2229.0,2191.0,2408.0,-8.0,2536.0,168.0,9F6,1,33|2231.0,2181.0,2237.0,2191.0,1768.0,-8.0,1880.0,168.0,9F6,1,33
msl-hud-glyph;dump=1;first=65;g=2249.0,2118.0,2256.0,2129.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2258.0,2118.0,2265.0,2129.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2267.0,2118.0,2274.0,2129.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2276.0,2118.0,2283.0,2129.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2249.0,2139.0,2256.0,2150.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37
msl-hud-glyph;dump=1;first=70;g=2258.0,2139.0,2265.0,2150.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2267.0,2139.0,2274.0,2150.0,2192.0,3168.0,2304.0,3344.0,9F6,1,37|2276.0,2139.0,2283.0,2150.0,1552.0,3168.0,1664.0,3344.0,9F6,1,37|2249.0,2160.0,2256.0,2171.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2258.0,2160.0,2265.0,2171.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37
msl-hud-glyph;dump=1;first=75;g=2267.0,2160.0,2274.0,2171.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2276.0,2160.0,2283.0,2171.0,2576.0,3168.0,2688.0,3344.0,9F6,1,37|2249.0,2181.0,2256.0,2192.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2258.0,2181.0,2265.0,2192.0,1264.0,3168.0,1376.0,3344.0,9F6,1,37|2267.0,2181.0,2274.0,2192.0,1552.0,3168.0,1664.0,3344.0,9F6,1,37
msl-hud-glyph;dump=1;first=80;g=2276.0,2181.0,2283.0,2192.0,496.0,3168.0,608.0,3344.0,9F6,1,37|2222.0,2198.0,2258.0,2246.0,0.0,0.0,3925.3,3925.3,9F6,1,38|2213.0,2202.0,2237.0,2234.0,0.0,0.0,4096.0,4096.0,9F6,1,39|2246.0,2202.0,2270.0,2234.0,0.0,0.0,4096.0,4096.0,9F6,1,39|1970.0,2214.0,1975.0,2222.0,1128.0,344.0,1224.0,488.0,222,1,41
msl-hud-glyph;dump=1;first=85;g=1977.0,2214.0,1982.0,2222.0,2248.0,344.0,2344.0,488.0,222,1,41|1984.0,2214.0,1989.0,2222.0,2920.0,344.0,3016.0,488.0,222,1,41|1991.0,2214.0,1996.0,2222.0,1320.0,344.0,1416.0,488.0,222,1,41|1998.0,2214.0,2003.0,2222.0,2536.0,344.0,2632.0,488.0,222,1,41|1931.0,2210.0,1940.0,2225.0,448.0,2640.0,656.0,2880.0,9F6,1,42
msl-hud-glyph;dump=1;first=90;g=1931.0,2231.0,1940.0,2246.0,664.0,2608.0,872.0,2848.0,9F6,1,42|1925.0,2221.0,1934.0,2235.0,232.0,2640.0,424.0,2864.0,9F6,1,42|1938.0,2221.0,1947.0,2235.0,232.0,2640.0,424.0,2864.0,9F6,1,42|1914.0,2210.0,1921.0,2220.0,896.0,2672.0,1056.0,2832.0,9F6,1,42|1914.0,2222.0,1921.0,2232.0,896.0,2672.0,1056.0,2832.0,9F6,1,42
msl-hud-glyph;dump=1;first=95;g=1914.0,2234.0,1921.0,2244.0,896.0,2672.0,1056.0,2832.0,9F6,1,42|1858.8,2172.2,1866.5,2186.2,0.0,2912.0,192.0,3136.0,FFF,1,58|1858.1,2170.2,1867.8,2187.8,0.0,3824.0,192.0,4048.0,FFF,1,58
)DUMP";
static void test_step171() {
    std::istringstream in(kStep171Dump1);
    const std::vector<DumpFrame> ds = parse_dump(in);
    CHECK(ds.size() == 1 && ds[0].g.size() == 98);
    const Reading r = replay_one(ds[0], false);
    CHECK(r.ok && r.value == 50 && r.sp == 8 && r.dmg == 0 && r.anchor == anchor_table);
    CHECK(r.label_n[0] == 3 && r.label_n[1] == 4 && r.label_n[2] == 4 && r.label_n[3] == 3);
    // the MSSL value glyphs of this frame (row 1 of the block), re-textured with the table for the counts the gameshots show after each tap
    GlyphFrame& f = *g_f; Work& w = *g_w;
    f.clear(); for (const Glyph& g : ds[0].g) f.g[f.n++] = g;
    const Block b = find_block(f, w);
    CHECK(!b.why);
    const uint16_t tens = b.row[1].val[2], ones = b.row[1].val[3];
    const GlyphFrame base = f;
    auto set_count = [&](unsigned v) {
        f = base;
        for (auto [gi, d] : {std::pair<uint16_t, unsigned>{tens, v / 10}, std::pair<uint16_t, unsigned>{ones, v % 10}}) {
            const float wdt = f.g[gi].u1 - f.g[gi].u0; f.g[gi].u0 = kFontZeroU + float(d) * kFontPitch; f.g[gi].u1 = f.g[gi].u0 + wdt;
        }
    };
    for (unsigned v : {49u, 48u, 47u, 46u}) { set_count(v); const Reading x = read_frame(f, w, kNoDmg, anchor_none); CHECK(x.ok && x.value == v && x.sp == 8); }
    // the salvo on the step171 clock: taps at ticks 292945484 / 292952484 / 292959468 / 292966484, the HUD one step down ~0.4 s after each
    const double taps[4] = {292945484.0, 292952484.0, 292959468.0, 292966484.0};
    Decoder d; std::vector<std::pair<double, Event>> evs;
    for (double t = 292821156.0; t <= 292990000.0; t += 100.0) {
        unsigned fired = 0; for (double tp : taps) if (t >= tp + 400.0) ++fired;
        set_count(50 - fired);
        const Event e = d.frame(f, w, t);
        if (e.launches || e.rearm || e.rebase) evs.push_back({t, e});
    }
    CHECK(evs.size() == 4 && d.baseline() == 46 && d.stats().launches == 4 && d.stats().rebases == 0);
    for (size_t i = 0; i < evs.size() && i < 4; ++i) CHECK(evs[i].second.launches == 1 && evs[i].first - taps[i] >= 300.0 && evs[i].first - taps[i] <= 1000.0);
    // the old misread (a stable wrong 60 then the true 49) now costs no launch: rebase + one launch
    Decoder m; unsigned ml = 0, mr = 0;
    for (double t = 0; t < 3000; t += 100.0) { set_count(60); ml += m.frame(f, w, t).launches; }
    for (double t = 3000; t < 5000; t += 100.0) { set_count(49); const Event e = m.frame(f, w, t); ml += e.launches; mr += e.rebase; }
    CHECK(m.baseline() == 49 && ml == 1 && mr == 1);
    std::printf("step171 ok\n");
}

static void test_dump_roundtrip() {
    Scratch& sc = *g_sc; GlyphFrame& f = *g_f; const Atlas a = Atlas::game();
    build(f, sc, a, Hud{});
    // the producer's lines (format_dump_head + kDumpPerLine glyphs per msl-hud-glyph line), as they land in step5-producer.log
    std::string log;
    char b[512];
    Work& w = *g_w;
    const Reading r0 = read_frame(f, w, kNoDmg, anchor_none);
    format_dump_head(b, sizeof b, 1234, 70500, 7, f, f.n, false, r0);
    log += std::string("287627328|gpu-copy-step1;event=") + b + "\n";
    for (unsigned c = 0; c < f.n;) {
        int o = std::snprintf(b, sizeof b, "msl-hud-glyph;dump=7;first=%u;g=", c);
        for (unsigned k = 0; k < kDumpPerLine && c < f.n; ++k) { o += format_glyph(b + o, sizeof b - size_t(o), f.g[c]); ++c; if (k + 1 < kDumpPerLine && c < f.n) b[o++] = '|', b[o] = 0; }
        CHECK(o > 0 && o < 426);
        log += std::string("287627329|gpu-copy-step1;event=") + b + "\n";
    }
    const std::string path = "hud_msl_replay_selftest.log";
    { std::ofstream o(path); o << log; }
    std::vector<DumpFrame> ds;
    { std::ifstream in(path); ds = parse_dump(in); }   // closed before the remove below (Windows keeps an open file)
    CHECK(ds.size() == 1 && ds[0].id == 7 && ds[0].g.size() == f.n && ds[0].t == 70500);
    const Reading r = replay_one(ds[0], false);
    CHECK(r.ok && r.value == 50 && r.sp == 8 && r.dmg == 0);
    std::remove(path.c_str());
    std::printf("dump ok\n");
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--replay") == 0) return replay_main(argv[2]);
    test_option();
    test_extract();
    test_block_and_reading();
    test_salvo();
    test_sp_salvo();
    test_other_font();
    test_step171();
    test_dump_roundtrip();
    std::printf("hud_msl_offline: %u checks passed\n", g_checks);
    return 0;
}
