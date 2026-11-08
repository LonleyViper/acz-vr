#pragma once
// hud_msl.h -- MSL-HUD (docs/world-render/aircraft-auto.md section 14): the "missiles remaining" signal read from the game's OWN HUD text.
// SDK-free, header only, no allocation, bounded loops. Producer: gpu_copy_step5_hooks.inl (game render thread, step-1 mutex held) reads the HUD
// region's instrument draws on a sampled frame; consumer of the result: acz::stores::signal() (store_tracker.h).
//
// Why (section 13/14): three EE-memory searches (step167 32 KiB window, step168 u8 table, step170 wide search) found no counter that follows the
// launches, while the HUD shows MSSL 50 -> 46 for a 4-tap salvo (step167 gameshots g0070 vs g0097). The HUD text IS the count.
//
// What the HUD shows [E, step167 g0070, 2405x1323 game window -> GS x = 1728 + 640 px/2405, y = 1824 + 448 (py-30)/1323]: a right-hand block of four
// rows  GUN ----  /  MSSL --50  /  <special weapon> ---8  /  DMG --0%  ; every value field is 4 glyphs, right aligned (dash padded), the right edges of
// the four fields line up (GS x ~2345), rows ~21 GS apart (MSSL row y ~2150). The selected weapon's field is boxed (a line draw, not a glyph).
// The special-weapon label varies by aircraft (SAAM, QAAM, XLAA, UGBL, LAGM, FAEB, 4 letters; RCL, GPB, TLS, EML, 3 letters): it is never read; the
// MSSL row is the SECOND row of the block, checked by its label shape (4 glyphs, the two middle ones the same glyph: M S S L) and the GUN/DMG labels
// (3 glyphs each). Only the MSSL row drives the rack mask.
//
// Glyph identity [E, step171 null salvo run: D:\acz-evidence-archive\step171\kill\runAGB2, 48 mslhud:2 dumps + gameshots]: the value fields use ONE
// atlas row (v0 = 3168/4096 = 0.7734), every glyph quad 112/4096 wide on a 128/4096 pitch: digit d at u0 = (1552 + 128 d)/4096, '-' at u0 = 1264/4096,
// '%' at 496/4096 (dump 1: MSSL --50, SAAM ---8, DMG --0%; gameshots g0066-g0100: 50 -> 49 -> 48 -> 47 -> 46, one step per tap). The labels use another
// row (v0 ~ 0). The first build guessed an ASCII grid of glyph-wide cells (cell = u / glyph width): with the 16/128 gap between quads it read
// 5 -> 6, 6 -> 7, 7 -> 8, 8 -> 9 and refused 9, i.e. 50 -> "60" and 46 -> "47" (step171 events: rebase 60->49, 49->48, 48->47) = exactly what the
// table above predicts for the true 50, 49 (refused), 48, 47, 46. The table is fixed (kFont*); a field whose glyphs are not on that row (another
// HUD font, a different game build) falls back to the DMG anchor: the DMG ones digit while the damage is 0 gives zero_u and the row, with the same
// pitch. Anything else is refused (why=digit-not-in-table) and does NOTHING.
//
// Signal: a decoded value must be seen unchanged for >= 300 ms and 2 reads (hysteresis, as ee_counter_search.h LaunchDetector); the FIRST baseline needs
// >= 1000 ms and 3 reads; a stable fall of 1..8 = that many launches, a stable rise = rearm, a bigger fall = rebaseline that still counts ONE launch (a
// single stable change of the count is one launch at 10 Hz sampling: a misread baseline can no longer swallow a launch, step171 tap 2).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::hudmsl {

// ---------------------------------------------------------------------------------------------------------------- option
// xr_cockpit_model=...;mslhud:0|1|2  (default 1 = ON since the step172 null salvo pass [50 -> 46, one launch event per tap]; mslhud:0 = off. No cockpit model
// option string at all = off; 2 = on + glyph dump lines). stores:0 turns it off too (no masked stations to drive).
inline int parse_mode(const char* opt) noexcept {
    if (!opt || !*opt) return 0;
    if (std::strstr(opt, ";stores:0") || std::strstr(opt, ";stores=0")) return 0;
    const char* p = std::strstr(opt, ";mslhud:");
    if (!p) p = std::strstr(opt, ";mslhud=");
    if (!p) return 1;   // no token: on (default)
    const char c = p[8];
    return c == '0' ? 0 : c == '2' ? 2 : 1;
}
// SP-STORES: the special-weapon row drives the SP pylons unless `;spstores:0` (the off switch) or the whole reader is off (mslhud:0 / stores:0 / no option string).
inline bool parse_sp(const char* opt) noexcept {
    if (parse_mode(opt) <= 0) return false;
    return !(std::strstr(opt, ";spstores:0") || std::strstr(opt, ";spstores=0"));
}
// The cockpit model option parser's view (cockpit_model.cpp): 0, 1 or 2 only.
inline bool valid_mode_value(const char* v) noexcept { return v && (v[0] == '0' || v[0] == '1' || v[0] == '2') && v[1] == 0; }

// ---------------------------------------------------------------------------------------------------------------- glyph quads
inline constexpr unsigned kMaxVerts = 1024, kMaxIdx = 2048, kGlyphCap = 256;
inline constexpr uint32_t kCut = 0xFFFFFFFFu;
inline constexpr uint32_t kTopoList = 4, kTopoStrip = 5;   // D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST / TRIANGLESTRIP
// One ps2_v vertex as read (attr0 GS x,y,z,q at +0; COLOR0 at +16; TEXCOORD uv at +32).
struct Vtx { float x = 0, y = 0, q = 1, u = 0, v = 0, rgb[3] = {}; };
struct Glyph { float x0 = 0, y0 = 0, x1 = 0, y1 = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0, rgb[3] = {}, q = 1; uint16_t draw = 0; uint8_t nv = 0; };
struct GlyphFrame {
    Glyph g[kGlyphCap]; unsigned n = 0;
    unsigned draws = 0, unread = 0, dropped = 0, rej_untextured = 0, rej_size = 0, rej_shape = 0;
    void clear() noexcept { n = draws = unread = dropped = rej_untextured = rej_size = rej_shape = 0; }
};
// Working storage of one draw (heap: Step5State).
struct Scratch {
    Vtx v[kMaxVerts]; uint32_t uniq[kMaxVerts]; uint32_t stream[kMaxIdx];
    uint16_t parent[kMaxVerts]; int16_t slot[kMaxVerts];
};

namespace detail {
inline unsigned find(uint16_t* p, unsigned a) noexcept { while (p[a] != a) { p[a] = p[p[a]]; a = p[a]; } return a; }
inline void unite(uint16_t* p, unsigned a, unsigned b) noexcept { a = find(p, a); b = find(p, b); if (a != b) p[a > b ? a : b] = uint16_t(a < b ? a : b); }
}

// Components of the draw (vertices joined by a triangle) -> glyph quads appended to f. sc.v[0..nv) = the vertices; indexed: sc.uniq[0..nv) = their
// (sorted, unique) index values and sc.stream[0..ni) = the index stream in draw order (kCut = strip cut); non-indexed: ni = 0 (draw order).
// A glyph = a component of 4..6 vertices, 1.5..48 GS on each side, with a uv extent on both axes (textured). Everything else is counted, not kept.
inline void add_draw(GlyphFrame& f, Scratch& sc, unsigned nv, unsigned ni, bool indexed, uint32_t topology, uint16_t draw) noexcept {
    ++f.draws;
    if (nv > kMaxVerts) nv = kMaxVerts;
    if (ni > kMaxIdx) ni = kMaxIdx;
    if (!nv || (topology != kTopoList && topology != kTopoStrip)) { ++f.rej_shape; return; }
    for (unsigned i = 0; i < nv; ++i) sc.parent[i] = uint16_t(i);
    auto slot_of = [&](uint32_t x) -> int {
        const uint32_t* b0 = sc.uniq; const uint32_t* e = sc.uniq + nv; const uint32_t* p = std::lower_bound(b0, e, x);
        return (p != e && *p == x) ? int(p - sc.uniq) : -1;
    };
    if (indexed) {
        if (topology == kTopoList) {
            for (unsigned k = 0; k + 2 < ni; k += 3) {
                const int a = slot_of(sc.stream[k]), b = slot_of(sc.stream[k + 1]), c = slot_of(sc.stream[k + 2]);
                if (a < 0 || b < 0 || c < 0) continue;
                detail::unite(sc.parent, unsigned(a), unsigned(b)); detail::unite(sc.parent, unsigned(a), unsigned(c));
            }
        } else {
            int prev = -1;
            for (unsigned k = 0; k < ni; ++k) {
                if (sc.stream[k] == kCut) { prev = -1; continue; }
                const int a = slot_of(sc.stream[k]);
                if (a < 0) { prev = -1; continue; }
                if (prev >= 0) detail::unite(sc.parent, unsigned(prev), unsigned(a));
                prev = a;
            }
        }
    } else if (topology == kTopoList) {
        for (unsigned k = 0; k + 2 < nv; k += 3) { detail::unite(sc.parent, k, k + 1); detail::unite(sc.parent, k, k + 2); }
        // the two triangles of one quad share corners by value, not by index
        if (nv <= 384)
            for (unsigned i = 0; i < nv; ++i) for (unsigned j = i + 1; j < nv; ++j)
                if (sc.v[i].x == sc.v[j].x && sc.v[i].y == sc.v[j].y && sc.v[i].u == sc.v[j].u && sc.v[i].v == sc.v[j].v) detail::unite(sc.parent, i, j);
    } else {
        if (nv != 4) { ++f.rej_shape; return; }   // a non-indexed strip is one quad only when it has 4 vertices
        for (unsigned k = 1; k < nv; ++k) detail::unite(sc.parent, 0, k);
    }
    for (unsigned i = 0; i < nv; ++i) sc.slot[i] = -1;
    unsigned first = f.n;
    for (unsigned i = 0; i < nv; ++i) {
        const Vtx& v = sc.v[i];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.u) || !std::isfinite(v.v)) continue;
        const unsigned r = detail::find(sc.parent, i);
        if (sc.slot[r] == -1) {
            if (f.n >= kGlyphCap) { ++f.dropped; sc.slot[r] = -2; continue; }
            sc.slot[r] = int16_t(f.n);
            Glyph& g = f.g[f.n++];
            g = Glyph{}; g.x0 = g.x1 = v.x; g.y0 = g.y1 = v.y; g.u0 = g.u1 = v.u; g.v0 = g.v1 = v.v; g.q = v.q; g.draw = draw;
        }
        if (sc.slot[r] < 0) continue;
        Glyph& g = f.g[sc.slot[r]];
        g.x0 = std::fmin(g.x0, v.x); g.x1 = std::fmax(g.x1, v.x); g.y0 = std::fmin(g.y0, v.y); g.y1 = std::fmax(g.y1, v.y);
        g.u0 = std::fmin(g.u0, v.u); g.u1 = std::fmax(g.u1, v.u); g.v0 = std::fmin(g.v0, v.v); g.v1 = std::fmax(g.v1, v.v);
        for (int k = 0; k < 3; ++k) g.rgb[k] += std::isfinite(v.rgb[k]) ? v.rgb[k] : 0.0f;
        if (g.nv < 255) ++g.nv;
    }
    // keep the glyph-like components only (compact in place)
    unsigned w = first;
    for (unsigned i = first; i < f.n; ++i) {
        Glyph g = f.g[i];
        for (int k = 0; k < 3; ++k) g.rgb[k] = g.nv ? g.rgb[k] / float(g.nv) : 0.0f;
        const float gw = g.x1 - g.x0, gh = g.y1 - g.y0;
        if (g.nv < 4 || g.nv > 6) { ++f.rej_shape; continue; }
        if (!(gw >= 1.5f && gw <= 48.0f && gh >= 1.5f && gh <= 48.0f)) { ++f.rej_size; continue; }
        if (!(g.u1 - g.u0 > 1e-6f && g.v1 - g.v0 > 1e-6f)) { ++f.rej_untextured; continue; }
        f.g[w++] = g;
    }
    f.n = w;
}

// ---------------------------------------------------------------------------------------------------------------- the readout block
// Region: the right 40 % and the lower part of the GS window (x 1728..2368, y 1824..2272): leaves out TIME (y ~1890) and ALT (y ~2052).
inline constexpr float kRegionX = 2112.0f, kRegionY = 2072.0f, kRegionX1 = 2400.0f, kRegionY1 = 2300.0f;
inline bool in_region(const Glyph& g) noexcept {
    const float cx = 0.5f * (g.x0 + g.x1), cy = 0.5f * (g.y0 + g.y1);
    return cx > kRegionX && cx < kRegionX1 && cy > kRegionY && cy < kRegionY1;
}
// A character cell's shape (monospace cells: a '1' is as wide as an 'M'): >= 3 x 5 GS, width 0.3..2 x height. A thin textured line (a field box side
// drawn as a sprite) is not a glyph and never becomes the field's last character.
inline bool glyph_shaped(const Glyph& g) noexcept {
    const float w = g.x1 - g.x0, h = g.y1 - g.y0;
    return w >= 3.0f && h >= 5.0f && w <= 2.0f * h && w >= 0.3f * h;
}
inline uint32_t key_of(const Glyph& g) noexcept {   // the glyph's atlas position (uv top-left, 1/4096 steps)
    const auto q = [](float x) { const long v = std::lround(double(x) * 4096.0); return uint32_t(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v); };
    return (q(g.u0) << 16) | q(g.v0);
}
// Cell index under an ASCII-grid assumption: DIAGNOSTIC only (the replay tool prints it; the first build decoded with it and was wrong, see the header).
inline int32_t cell_of(const Glyph& g) noexcept {
    const float du = g.u1 - g.u0, dv = g.v1 - g.v0;
    if (!(du > 1e-5f) || !(dv > 1e-5f)) return INT32_MIN;
    const float uc = 0.5f * (g.u0 + g.u1), vc = 0.5f * (g.v0 + g.v1);
    const double colsf = 1.0 / double(du);
    const long cols = std::lround(colsf);
    const bool norm = g.u1 <= 1.01f && g.v1 <= 1.01f && cols >= 2 && std::fabs(colsf - double(cols)) < 0.15;
    const long col = long(std::floor(double(uc) / double(du))), row = long(std::floor(double(vc) / double(dv)));
    return int32_t(norm ? row * cols + col : row * 4096 + col);
}

// The HUD value font [E step171]: row, zero and pitch in atlas units (uv x 4096).
inline constexpr float kFontRowV = 3168.0f / 4096.0f, kFontZeroU = 1552.0f / 4096.0f, kFontPitch = 128.0f / 4096.0f, kFontDashU = 1264.0f / 4096.0f;
inline constexpr float kFontRowTol = 8.0f / 4096.0f, kFontPitchTol = 0.15f;
struct DigitFont { float zero_u = kFontZeroU, row_v = kFontRowV, pitch = kFontPitch; bool valid = true; };
// The digit a glyph shows under font f: -1 = not a digit of that font (another row, between two digits, outside 0..9).
inline int digit_of(const Glyph& g, const DigitFont& f) noexcept {
    if (!f.valid || !(std::fabs(g.v0 - f.row_v) <= kFontRowTol)) return -1;
    const float k = (g.u0 - f.zero_u) / f.pitch, d = std::round(k);
    if (!(std::fabs(k - d) <= kFontPitchTol) || d < 0.0f || d > 9.0f) return -1;
    return int(d);
}

inline constexpr unsigned kRowCap = 24, kLabelMax = 8;
struct Field {
    float cy = 0, h = 0, adv = 0, xr = 0;
    unsigned label_n = 0; uint16_t label[kLabelMax] = {};   // glyph indices, left to right
    uint16_t val[4] = {};                                   // the 4 value glyphs, left to right
};
struct Block { unsigned nrows = 0; Field row[4]; unsigned cand = 0, rows = 0, fields = 0, group = 0; const char* why = "no-glyphs"; };
struct Work { uint16_t cand[kGlyphCap]; uint16_t rowg[kGlyphCap]; Field fld[kRowCap]; unsigned nf = 0; };

inline float median3(float a, float b, float c) noexcept { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

inline Block find_block(const GlyphFrame& f, Work& w) noexcept {
    Block b;
    unsigned nc = 0;
    for (unsigned i = 0; i < f.n; ++i) if (in_region(f.g[i]) && glyph_shaped(f.g[i])) w.cand[nc++] = uint16_t(i);
    b.cand = nc;
    if (nc < 5) { b.why = nc ? "few-glyphs" : "no-glyphs"; return b; }
    auto cy = [&](unsigned i) { return 0.5f * (f.g[i].y0 + f.g[i].y1); };
    auto cx = [&](unsigned i) { return 0.5f * (f.g[i].x0 + f.g[i].x1); };
    auto gh = [&](unsigned i) { return f.g[i].y1 - f.g[i].y0; };
    std::sort(w.cand, w.cand + nc, [&](uint16_t a, uint16_t c) { return cy(a) < cy(c) || (cy(a) == cy(c) && a < c); });
    w.nf = 0;
    unsigned s = 0;
    while (s < nc) {
        const float y0 = cy(w.cand[s]), tol = 0.45f * gh(w.cand[s]);
        unsigned e = s + 1;
        while (e < nc && cy(w.cand[e]) - y0 <= tol) ++e;
        ++b.rows;
        const unsigned n = e - s;
        if (n >= 5 && w.nf < kRowCap) {
            uint16_t* r = w.rowg;
            for (unsigned k = 0; k < n; ++k) r[k] = w.cand[s + k];
            std::sort(r, r + n, [&](uint16_t a, uint16_t c) { return cx(a) < cx(c) || (cx(a) == cx(c) && a < c); });
            const uint16_t* v = r + (n - 4);
            const float a0 = cx(v[1]) - cx(v[0]), a1 = cx(v[2]) - cx(v[1]), a2 = cx(v[3]) - cx(v[2]);
            const float m = median3(a0, a1, a2), h = median3(gh(v[0]), gh(v[1]), gh(v[2]));
            bool ok = m > 0.0f && m >= 0.5f * h && m <= 2.5f * h;
            for (float a : {a0, a1, a2}) if (!(a >= 0.7f * m && a <= 1.3f * m)) ok = false;
            if (ok) {
                Field fl; fl.adv = m; fl.h = h; fl.xr = f.g[v[3]].x1;
                fl.cy = 0.25f * (cy(v[0]) + cy(v[1]) + cy(v[2]) + cy(v[3]));
                for (int k = 0; k < 4; ++k) fl.val[k] = v[k];
                // the label: glyphs to the left, the first gap <= 4 advances, then <= 1.6 advances between label glyphs
                int j = int(n) - 5; float prev = cx(v[0]); unsigned ln = 0; uint16_t lab[kLabelMax];
                while (j >= 0 && ln < kLabelMax) {
                    const float gap = prev - cx(r[j]);
                    if (gap > (ln ? 1.6f : 4.0f) * m) break;
                    lab[ln++] = r[j]; prev = cx(r[j]); --j;
                }
                fl.label_n = ln;
                for (unsigned k = 0; k < ln; ++k) fl.label[k] = lab[ln - 1 - k];
                if (ln >= 2) w.fld[w.nf++] = fl;
            }
        }
        s = e;
    }
    b.fields = w.nf;
    if (w.nf < 3) { b.why = w.nf ? "few-fields" : "no-fields"; return b; }
    // the aligned group: fields whose right edges agree, the largest group (ties: the lower one)
    unsigned best = 0, bestN = 0;
    for (unsigned i = 0; i < w.nf; ++i) {
        unsigned n = 0;
        for (unsigned k = 0; k < w.nf; ++k) if (std::fabs(w.fld[k].xr - w.fld[i].xr) <= std::fmax(3.0f, 0.4f * w.fld[i].adv)) ++n;
        if (n > bestN || (n == bestN && w.fld[i].cy > w.fld[best].cy)) { best = i; bestN = n; }
    }
    b.group = bestN;
    if (bestN != 4) { b.why = bestN < 4 ? "rows-under-4" : "rows-over-4"; return b; }
    unsigned k4 = 0;
    for (unsigned k = 0; k < w.nf && k4 < 4; ++k)
        if (std::fabs(w.fld[k].xr - w.fld[best].xr) <= std::fmax(3.0f, 0.4f * w.fld[best].adv)) b.row[k4++] = w.fld[k];   // already in y order
    const float d0 = b.row[1].cy - b.row[0].cy, d1 = b.row[2].cy - b.row[1].cy, d2 = b.row[3].cy - b.row[2].cy, dm = median3(d0, d1, d2);
    for (float d : {d0, d1, d2}) if (!(dm > 0.0f && d >= 0.6f * dm && d <= 1.6f * dm)) { b.why = "row-pitch"; return b; }
    b.nrows = 4; b.why = nullptr;
    return b;
}

// ---------------------------------------------------------------------------------------------------------------- one frame's reading
enum Anchor : uint8_t { anchor_none = 0, anchor_table = 1, anchor_dmg = 2 };
struct Reading {
    bool ok = false; const char* why = "no-glyphs";
    uint32_t value = 0; int32_t sp = -1, dmg = -1;          // MSSL count; the special-weapon and damage fields when they decode (-1 = no)
    uint32_t keys[4] = {}; uint32_t dash = 0; DigitFont font{}; Anchor anchor = anchor_none;
    unsigned label_n[4] = {}; Block block;
};
inline const char* anchor_name(Anchor a) noexcept { return a == anchor_table ? "table" : a == anchor_dmg ? "dmg" : "none"; }

// Field digits -> value under font f; -1 = not a number (a dash after a digit, a glyph not in the font, all dashes). `use` = glyphs read (DMG: 3).
inline int32_t field_value(const GlyphFrame& f, const Field& fl, uint32_t dash, const DigitFont& font, unsigned use = 4) noexcept {
    unsigned p = 0;
    while (p < use && key_of(f.g[fl.val[p]]) == dash) ++p;
    if (p >= use) return -1;
    int32_t v = 0;
    for (unsigned k = p; k < use; ++k) {
        if (key_of(f.g[fl.val[k]]) == dash) return -1;
        const int d = digit_of(f.g[fl.val[k]], font);
        if (d < 0) return -1;
        v = v * 10 + d;
    }
    return v;
}

// dmg: the latched DMG-anchor font (valid = false: none yet); want: anchor_none = the first that works (table, then dmg), else only that one.
inline Reading read_frame(const GlyphFrame& f, Work& w, const DigitFont& dmg, Anchor want) noexcept {
    Reading r;
    r.block = find_block(f, w);
    if (r.block.why) { r.why = r.block.why; return r; }
    const Field& gun = r.block.row[0]; const Field& msl = r.block.row[1]; const Field& sp = r.block.row[2]; const Field& dm = r.block.row[3];
    for (int i = 0; i < 4; ++i) r.label_n[i] = r.block.row[i].label_n;
    for (int k = 0; k < 4; ++k) r.keys[k] = key_of(f.g[msl.val[k]]);
    // structure: GUN / MSSL / <special> / DMG
    if (msl.label_n != 4) { r.why = "msl-label-not-4"; return r; }
    {
        const uint32_t l0 = key_of(f.g[msl.label[0]]), l1 = key_of(f.g[msl.label[1]]), l2 = key_of(f.g[msl.label[2]]), l3 = key_of(f.g[msl.label[3]]);
        if (!(l1 == l2 && l0 != l1 && l3 != l2)) { r.why = "msl-label-shape"; return r; }
    }
    if (gun.label_n != 3 || dm.label_n != 3) { r.why = "gun-dmg-label-not-3"; return r; }
    // the pad glyph: the first value glyph of at least 3 of the 4 rows
    uint32_t firsts[4]; for (int i = 0; i < 4; ++i) firsts[i] = key_of(f.g[r.block.row[i].val[0]]);
    unsigned bestc = 0;
    for (int i = 0; i < 4; ++i) { unsigned c = 0; for (int k = 0; k < 4; ++k) if (firsts[k] == firsts[i]) ++c; if (c > bestc) { bestc = c; r.dash = firsts[i]; } }
    if (bestc < 3) { r.why = "no-dash"; return r; }
    bool allDash = true; for (int k = 0; k < 4; ++k) if (r.keys[k] != r.dash) allDash = false;
    if (allDash) { r.why = "msl-all-dash"; return r; }
    int32_t v = -1;
    const DigitFont table{};
    if (want == anchor_none || want == anchor_table) { v = field_value(f, msl, r.dash, table); if (v >= 0) { r.anchor = anchor_table; r.font = table; } }
    if (v < 0 && (want == anchor_none || want == anchor_dmg) && dmg.valid) { v = field_value(f, msl, r.dash, dmg); if (v >= 0) { r.anchor = anchor_dmg; r.font = dmg; } }
    if (v < 0) { r.why = "digit-not-in-table"; return r; }
    r.value = uint32_t(v);
    r.sp = field_value(f, sp, r.dash, r.font);
    r.dmg = field_value(f, dm, r.dash, r.font, 3);   // the 4th DMG glyph is '%'
    r.ok = true; r.why = nullptr;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------- the stateful decoder
inline constexpr double kStableMs = 300.0;
inline constexpr unsigned kStableReads = 2, kMaxLaunches = 8, kDmgLatchReads = 8, kEventLines = 100;
inline constexpr double kBaseStableMs = 1000.0;   // the FIRST baseline: >= 1 s and kBaseReads reads unchanged
inline constexpr unsigned kBaseReads = 3;
struct Event { unsigned launches = 0; bool rearm = false; uint32_t old_value = 0, new_value = 0; bool rebase = false; };
struct Stats {
    uint64_t frames = 0, blocks = 0, readings = 0, launches = 0, rearms = 0, rebases = 0, tuple_changes = 0, glyphs = 0; unsigned glyphs_last = 0;
    uint64_t why_n[8] = {}; const char* why_name[8] = {};    // refusal reasons of the current summary window (first 8 distinct)
};
// SP-STORES: the special-weapon field's count (HUD row 3, `Reading::sp`) with the MSSL decoder's hysteresis (stable >= kStableMs and kStableReads reads; the first
// baseline >= kBaseStableMs and kBaseReads; a fall of 1..kMaxLaunches = that many launches, a bigger fall one launch + rebase, a rise = rearm). A frame whose field did
// not decode (v < 0: all dashes = no special weapon, or a glyph outside the table) is ignored: it neither moves the baseline nor counts as a change.
class StableCount {
public:
    void reset() noexcept { *this = StableCount{}; }
    Event feed(int32_t v, double now_ms) noexcept {
        Event ev;
        if (v < 0) return ev;
        const uint32_t u = uint32_t(v);
        if (!have_pend_ || u != pend_) { pend_ = u; pend_t_ = now_ms; pend_n_ = 1; have_pend_ = true; return ev; }
        ++pend_n_;
        if (pend_n_ < kStableReads || now_ms - pend_t_ < kStableMs) return ev;
        if (!have_base_) {
            if (pend_n_ < kBaseReads || now_ms - pend_t_ < kBaseStableMs) return ev;
            base_ = u; have_base_ = true; return ev;
        }
        if (u < base_) {
            const uint32_t d = base_ - u;
            ev.old_value = base_; ev.new_value = u;
            if (d <= kMaxLaunches) ev.launches = d;
            else { ev.rebase = true; ++rebases_; ev.launches = 1; }
            launches_ += ev.launches; base_ = u;
        } else if (u > base_) { ev.rearm = true; ev.old_value = base_; ev.new_value = u; ++rearms_; base_ = u; }
        return ev;
    }
    bool has_baseline() const noexcept { return have_base_; }
    uint32_t baseline() const noexcept { return base_; }
    uint64_t launches() const noexcept { return launches_; }
    uint64_t rearms() const noexcept { return rearms_; }
    uint64_t rebases() const noexcept { return rebases_; }
private:
    bool have_base_ = false, have_pend_ = false;
    uint32_t base_ = 0, pend_ = 0, pend_n_ = 0;
    double pend_t_ = 0;
    uint64_t launches_ = 0, rearms_ = 0, rebases_ = 0;
};
class Decoder {
public:
    void reset() noexcept { *this = Decoder{}; }
    // One sampled frame. Returns the event (launches / rearm) the stable value produced, if any.
    Event frame(const GlyphFrame& f, Work& w, double now_ms) noexcept {
        Event ev;
        sp_ev_ = Event{};
        ++st_.frames; st_.glyphs += f.n; st_.glyphs_last = f.n;
        // the DMG anchor: the DMG ones digit's glyph while it is the same for kDmgLatchReads blocks in a row (damage 0 at the start)
        last_ = read_frame(f, w, dmg_font_, anchor_);
        const Block& b = last_.block;
        if (!b.why) {
            ++st_.blocks;
            if (!dmg_font_.valid && last_.label_n[3] == 3) {
                const uint32_t c = key_of(f.g[b.row[3].val[2]]);
                if (have_dmg_cand_ && c == dmg_cand_) {
                    if (++dmg_n_ >= kDmgLatchReads) { dmg_font_.zero_u = f.g[b.row[3].val[2]].u0; dmg_font_.row_v = f.g[b.row[3].val[2]].v0; dmg_font_.valid = true; }
                } else { dmg_cand_ = c; dmg_n_ = 1; have_dmg_cand_ = true; }
            }
            // the raw key tuple (diagnostic: how often the MSSL field changed, decodable or not)
            uint32_t t[4]; for (int k = 0; k < 4; ++k) t[k] = key_of(f.g[b.row[1].val[k]]);
            if (have_tuple_ && std::memcmp(t, tuple_, sizeof t) != 0) ++st_.tuple_changes;
            std::memcpy(tuple_, t, sizeof t); have_tuple_ = true;
        }
        if (!last_.ok) { note_why(last_.why); return ev; }
        ++st_.readings;
        sp_ev_ = spc_.feed(last_.sp, now_ms);   // SP-STORES: the special-weapon row on its own hysteresis (independent of the MSSL one)
        const uint32_t v = last_.value;
        if (!have_pend_ || v != pend_) { pend_ = v; pend_t_ = now_ms; pend_n_ = 1; have_pend_ = true; return ev; }
        ++pend_n_;
        if (pend_n_ < kStableReads || now_ms - pend_t_ < kStableMs) return ev;
        if (!have_base_) {   // the anchor is fixed with the baseline, which needs a longer agreement than a change
            if (pend_n_ < kBaseReads || now_ms - pend_t_ < kBaseStableMs) return ev;
            base_ = v; have_base_ = true; anchor_ = last_.anchor; return ev;
        }
        if (v < base_) {
            const uint32_t d = base_ - v;
            ev.old_value = base_; ev.new_value = v;
            if (d <= kMaxLaunches) ev.launches = d;
            else { ev.rebase = true; ++st_.rebases; ev.launches = 1; }   // one stable change = one launch, whatever the misread distance
            st_.launches += ev.launches;
            base_ = v;
        } else if (v > base_) { ev.rearm = true; ev.old_value = base_; ev.new_value = v; ++st_.rearms; base_ = v; }
        return ev;
    }
    bool has_baseline() const noexcept { return have_base_; }
    uint32_t baseline() const noexcept { return base_; }
    // SP-STORES: the special-weapon count's event of the LAST frame() call (launches / rearm / rebase), its baseline and counters.
    const Event& sp_event() const noexcept { return sp_ev_; }
    const StableCount& sp() const noexcept { return spc_; }
    const Reading& last() const noexcept { return last_; }
    const Stats& stats() const noexcept { return st_; }
    const DigitFont& dmg_font() const noexcept { return dmg_font_; }
    Anchor anchor() const noexcept { return anchor_; }
    void window_reset() noexcept { for (auto& n : st_.why_n) n = 0; for (auto& p : st_.why_name) p = nullptr; }
private:
    void note_why(const char* why) noexcept {
        if (!why) return;
        for (int i = 0; i < 8; ++i) {
            if (st_.why_name[i] && std::strcmp(st_.why_name[i], why) == 0) { ++st_.why_n[i]; return; }
            if (!st_.why_name[i]) { st_.why_name[i] = why; st_.why_n[i] = 1; return; }
        }
    }
    Reading last_;
    Stats st_;
    StableCount spc_; Event sp_ev_;
    bool have_base_ = false, have_pend_ = false, have_tuple_ = false;
    uint32_t base_ = 0, pend_ = 0, pend_n_ = 0, tuple_[4] = {};
    double pend_t_ = 0;
    DigitFont dmg_font_{0.0f, 0.0f, kFontPitch, false}; uint32_t dmg_cand_ = 0; bool have_dmg_cand_ = false; unsigned dmg_n_ = 0;
    Anchor anchor_ = anchor_none;
};

// ---------------------------------------------------------------------------------------------------------------- process-wide status (the XR consumer's summary-stores line)
inline constexpr uint32_t kNoValue = 0xFFFFFFFFu;
struct Status { std::atomic<uint32_t> value{kNoValue}; std::atomic<uint32_t> launches{0}; std::atomic<uint32_t> mode{0xFFFFFFFFu};   // mode: 0xFFFFFFFF = not resolved yet
                std::atomic<uint32_t> sp_value{kNoValue}, sp_launches{0}, sp_mode{0xFFFFFFFFu};   // SP-STORES: the special-weapon row's stable count / launches; sp_mode 0 off, 1 on
              };
inline Status& status() noexcept { static Status s; return s; }

// ---------------------------------------------------------------------------------------------------------------- log lines
// summary-msl-hud: every 5 s while enabled (unconditional: a dead reader is told from an idle one).
inline int format_summary(char* b, size_t n, double now_ms, int mode, const Decoder& d) noexcept {
    const Stats& s = d.stats(); const Reading& r = d.last();
    char why[160]; size_t o = 0; why[0] = 0;
    for (int i = 0; i < 8 && s.why_name[i]; ++i) {
        const int k = std::snprintf(why + o, sizeof why - o, "%s%s:%llu", i ? "," : "", s.why_name[i], (unsigned long long)s.why_n[i]);
        if (k < 0 || size_t(k) >= sizeof why - o) break;
        o += size_t(k);
    }
    char base[16]; if (d.has_baseline()) std::snprintf(base, sizeof base, "%u", d.baseline()); else std::snprintf(base, sizeof base, "none");
    char last[16]; if (r.ok) std::snprintf(last, sizeof last, "%u", r.value); else std::snprintf(last, sizeof last, "none");
    return std::snprintf(b, n,
        "summary-msl-hud;t-ms=%.0f;mode=%d;frames=%llu;glyphs-last=%u;cand=%u;y-rows=%u;fields=%u;aligned=%u;blocks=%llu;readings=%llu;value=%s;last=%s;sp=%d;dmg=%d;"
        "anchor=%s;dash=%08X;zero-u=%.0f;dmg-zero=%s;labels=%u/%u/%u/%u;launches=%llu;rearms=%llu;rebases=%llu;tuple-changes=%llu;why=%s",
        now_ms, mode, (unsigned long long)s.frames, s.glyphs_last, r.block.cand, r.block.rows, r.block.fields, r.block.group,
        (unsigned long long)s.blocks, (unsigned long long)s.readings, base, last, r.ok ? r.sp : -1, r.ok ? r.dmg : -1,
        anchor_name(d.anchor()), r.dash, double(r.font.zero_u) * 4096.0, d.dmg_font().valid ? "set" : "none",
        r.label_n[0], r.label_n[1], r.label_n[2], r.label_n[3],
        (unsigned long long)s.launches, (unsigned long long)s.rearms, (unsigned long long)s.rebases, (unsigned long long)s.tuple_changes, o ? why : "none");
}
// SP-STORES: the special-weapon row's own lines (separate from summary-msl-hud: that one is near the 426-character step-1 text budget).
// summary-msl-hud-sp every 5 s while enabled (unconditional): sp-last = the last frame's decoded SP field (-1 = none), sp-value = the stable baseline.
inline int format_sp_summary(char* b, size_t n, double now_ms, const Decoder& d) noexcept {
    const Reading& r = d.last(); const StableCount& s = d.sp();
    char base[16]; if (s.has_baseline()) std::snprintf(base, sizeof base, "%u", s.baseline()); else std::snprintf(base, sizeof base, "none");
    return std::snprintf(b, n, "summary-msl-hud-sp;t-ms=%.0f;sp-last=%d;sp-value=%s;sp-launches=%llu;sp-rearms=%llu;sp-rebases=%llu",
        now_ms, r.ok ? r.sp : -1, base, (unsigned long long)s.launches(), (unsigned long long)s.rearms(), (unsigned long long)s.rebases());
}
inline int format_sp_event(char* b, size_t n, double now_ms, const Event& e) noexcept {
    return std::snprintf(b, n, "msl-hud;event=%s;old=%u;new=%u;launches=%u;t-ms=%.0f",
        e.rearm ? "sp-rearm" : e.rebase ? "sp-rebase" : "sp-launch", e.old_value, e.new_value, e.launches, now_ms);
}
inline int format_event(char* b, size_t n, double now_ms, const Event& e) noexcept {
    return std::snprintf(b, n, "msl-hud;event=%s;old=%u;new=%u;launches=%u;t-ms=%.0f",
        e.rearm ? "rearm" : e.rebase ? "rebase" : "launch", e.old_value, e.new_value, e.launches, now_ms);
}
// Dump (mslhud:2): a header line then the glyph records, `per` glyphs per line: x0,y0,x1,y1 (GS, 0.1) u0,v0,u1,v1 (x 4096) rgb (3 hex digits) q draw.
inline int format_dump_head(char* b, size_t n, uint64_t frame, double now_ms, unsigned dump, const GlyphFrame& f, unsigned listed, bool full, const Reading& r) noexcept {
    return std::snprintf(b, n, "msl-hud-dump;dump=%u;frame=%llu;t-ms=%.0f;scope=%s;glyphs=%u;listed=%u;draws=%u;unread=%u;dropped=%u;rej=%u/%u/%u;why=%s;value=%d",
        dump, (unsigned long long)frame, now_ms, full ? "all" : "region", f.n, listed, f.draws, f.unread, f.dropped, f.rej_shape, f.rej_size, f.rej_untextured,
        r.why ? r.why : "ok", r.ok ? int(r.value) : -1);
}
inline int format_glyph(char* b, size_t n, const Glyph& g) noexcept {
    auto c = [](float x) { const float v = std::fmin(1.0f, std::fmax(0.0f, x)); return unsigned(v * 15.0f + 0.5f); };
    return std::snprintf(b, n, "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%X%X%X,%.3g,%u",
        g.x0, g.y0, g.x1, g.y1, g.u0 * 4096.0f, g.v0 * 4096.0f, g.u1 * 4096.0f, g.v1 * 4096.0f, c(g.rgb[0]), c(g.rgb[1]), c(g.rgb[2]), g.q, unsigned(g.draw));
}
// Parse one glyph record of a dump line (the replay tool / tests): true = parsed.
inline bool parse_glyph(const char* s, Glyph& g) noexcept {
    float a[8]; unsigned rgb = 0; float q = 1; unsigned dr = 0;
    if (std::sscanf(s, "%f,%f,%f,%f,%f,%f,%f,%f,%x,%f,%u", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &rgb, &q, &dr) != 11) return false;
    g = Glyph{}; g.x0 = a[0]; g.y0 = a[1]; g.x1 = a[2]; g.y1 = a[3]; g.u0 = a[4] / 4096.0f; g.v0 = a[5] / 4096.0f; g.u1 = a[6] / 4096.0f; g.v1 = a[7] / 4096.0f;
    g.rgb[0] = float((rgb >> 8) & 15) / 15.0f; g.rgb[1] = float((rgb >> 4) & 15) / 15.0f; g.rgb[2] = float(rgb & 15) / 15.0f; g.q = q; g.draw = uint16_t(dr); g.nv = 4;
    return true;
}

// Dump schedule (mslhud:2): the first 3 dumps list every glyph of the frame (the whole HUD font in use), later ones the region only; one dump per event and every
// kDumpGapMs of sampled frames, at most kDumpMax. Lines are spread over frames by the producer (queue room).
inline constexpr unsigned kDumpMax = 48, kDumpFull = 3, kDumpPerLine = 5;
inline constexpr double kDumpGapMs = 5000.0;   // plus one dump at every msl-hud event (the producer), so the budget covers a salvo

}  // namespace acz::hudmsl
