// UI3D menu draw census offline tests (src/acz-vr/ui3d_census.h; docs/world-render/ui-3d.md section 4 item 1): sampling (one frame per second, the
// 48-draw cap, other-key counting, stale commits), the sample/line latch and re-arm, bounded line formatting with `?` for every unknown field,
// the screen bbox maths and the Present-side emitter. Nothing here touches D3D12 or the game; fixture success says nothing about the live menus.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui3d_census.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::u3c;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

namespace {

struct Collect {
    std::vector<std::string> v;
    void operator()(const char* t) { v.emplace_back(t); }
};
struct Fail { void operator()(const char*) { std::printf("FAIL unexpected line\n"); std::exit(1); } };

DrawRec full_draw() {
    DrawRec d;
    d.all = 5; d.ord = 2; d.count = 4; d.instances = 1; d.start = 8;
    d.rt_known = true; d.rt_res = 0x1234abcd; d.rt_vfmt = 10; d.rt_rfmt = 9; d.rt_w = 1280; d.rt_h = 720;
    d.pso_known = true; d.pso_rtv0 = 28; d.pso_dsv = 20; d.dsv_view_known = true; d.dsv_view_fmt = 20;
    d.vp_known = true; d.vp[0] = 0; d.vp[1] = 0; d.vp[2] = 1280; d.vp[3] = 720; d.nvp = 1;
    d.sc_known = true; d.sc[2] = 1280; d.sc[3] = 720; d.nsc = 1;
    d.blend_enable = true; d.blend[0] = 5; d.blend[1] = 6; d.blend[2] = 1; d.blend[3] = 1; d.blend[4] = 0; d.blend[5] = 1;
    d.depth_enable = true; d.depth_func = 7; d.depth_write = false; d.stencil = false;
    d.vb_bound = true; d.vb_va = 0x7000100; d.vb_size = 512; d.vb_stride = 64;
    // quad around the viewport centre: ndc = ((x-ox)*2/sx, (y-oy)*2/sy)/q, cb0 = (512, -448, 2048, 2048)
    d.cb_kind = "rc"; d.cb_root = 3; d.cb_mask = 15; d.cb[0] = 512; d.cb[1] = -448; d.cb[2] = 2048; d.cb[3] = 2048;
    const float x0 = 2048 - 128, x1 = 2048 + 128, y0 = 2048 + 112, y1 = 2048 - 112;   // ndc x -1..-0.? -> see test
    const float q[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    for (int i = 0; i < 4; ++i) { d.vtx[i][0] = q[i][0]; d.vtx[i][1] = q[i][1]; d.vtx[i][2] = 100.0f + float(i); d.vtx[i][3] = 1.0f; }
    d.nv = 4;
    return d;
}

// Drive one sampled frame through the accumulator the way the hook + Present thread do.
struct Hook {
    Accumulator a;
    void draw(bool menu, uint32_t vs8, uint32_t ps8, uint32_t count = 4, bool indexed = false) {
        uint32_t serial, slot, all, ord;
        const Admit r = a.admit(menu, vs8, ps8, 3, indexed, false, count, 1, serial, slot, all, ord);
        if (r == Admit::take) { DrawRec d = full_draw(); d.all = all; d.ord = ord; a.commit(serial, slot, d); }
    }
    void menu() { draw(true, kMenuVs8, kMenuPs8); }
};

void test_derive() {
    const DrawRec d = full_draw();
    const Derived v = derive(d);
    CHECK(v.ranges && v.zmin == 100.0f && v.zmax == 103.0f && v.qmin == 1.0f && v.qmax == 1.0f);
    // x: (1920-2048)*2/512 = -0.5 .. +0.5 -> px 320..960; y: (2160-2048)*2/-448 = -0.5 .. +0.5 -> 180..540 (y flips: y0 is the larger GS y)
    CHECK(v.bbox);
    CHECK(std::fabs(v.x0 - 320.0) < 1e-6 && std::fabs(v.x1 - 960.0) < 1e-6);
    CHECK(std::fabs(v.y0 - 180.0) < 1e-6 && std::fabs(v.y1 - 540.0) < 1e-6);
    DrawRec e = d; e.cb_mask = 7; CHECK(!derive(e).bbox && derive(e).ranges);       // an unwritten cb0 dword: no pixel bbox
    e = d; e.vp_known = false; CHECK(!derive(e).bbox);
    e = d; e.vtx[2][3] = 0.0f; CHECK(!derive(e).bbox && derive(e).ranges);          // q == 0: fail closed
    e = d; e.vtx[1][0] = std::nanf(""); CHECK(!derive(e).bbox);
    e = d; e.nv = 0; CHECK(!derive(e).ranges);
    e = d; e.cb[0] = 0.0f; CHECK(!derive(e).bbox);
    e = d; for (int i = 0; i < 4; ++i) e.vtx[i][3] = 2.0f; CHECK(std::fabs(derive(e).x0 - 480.0) < 1e-6);   // /q halves the extent about the centre
}

void test_format_draw() {
    char b[kLineLimit + 1];
    int n = format_draw(full_draw(), 7, b, sizeof b);
    std::string s(b);
    CHECK(n == int(s.size()) && n > 0 && n < int(kLineLimit));
    CHECK(s.rfind("event=ui3d-census;kind=draw;s=7;ord=2;all=5;idx=0;n=4;inst=1;st=8;", 0) == 0);
    CHECK(has(s, "rt=0x1234abcd/v10/r9/1280x720;") && has(s, "pso-rtv0=28;") && has(s, "dsv=20/view20;"));
    CHECK(has(s, "vp=0,0,1280,720/1;") && has(s, "sc=0,0,1280,720/1;"));
    CHECK(has(s, "bl=1:5,6,1,1,0,1;") && has(s, "dp=1/7/0/stc0;"));
    CHECK(has(s, "vb=0x7000100,512,64,mapped-upload;"));
    CHECK(has(s, "v=1920,2160,100,1|2176,2160,101,1|1920,1936,102,1|2176,1936,103,1/nv4;"));
    CHECK(has(s, "cb0=rc3:512,-448,2048,2048;"));
    CHECK(has(s, "bbox=320,180,960,540;z=100,103;q=1,1"));
    CHECK(s.find('\n') == std::string::npos);
    // CBV source
    DrawRec c = full_draw(); c.cb_kind = "cbv"; c.cb_root = 1; c.cb_va = 0xabc;
    format_draw(c, 1, b, sizeof b); CHECK(has(b, "cb0=cbv1@0xabc:512,-448,2048,2048;"));
    // every unknown field prints `?`
    DrawRec u; u.all = 1; u.ord = 1; u.count = 4; u.instances = 1;
    u.vb_why = "u3c-vb-unbound"; u.vtx_why = "u3c-vb-unbound"; u.cb_why = "u3c-root-layout-unknown";
    format_draw(u, 2, b, sizeof b); s = b;
    CHECK(has(s, "rt=?;") && has(s, "pso-rtv0=?;") && has(s, "dsv=?;") && has(s, "vp=?;") && has(s, "sc=?;") && has(s, "bl=?;dp=?;"));
    CHECK(has(s, "vb=?;") && has(s, "v=?(u3c-vb-unbound);") && has(s, "cb0=?(u3c-root-layout-unknown);") && has(s, "bbox=?;z=?;q=?"));
    // partially written cb0 root constants: the missing dwords are `?`
    DrawRec p = full_draw(); p.cb_mask = 0x5; format_draw(p, 1, b, sizeof b); CHECK(has(b, "cb0=rc3:512,?,2048,?;"));
    // dsv view unknown / vb mapped but vertices refused
    DrawRec q = full_draw(); q.dsv_view_known = false; q.nv = 0; q.vtx_why = "u3c-indexed-unread"; q.vb_why = "u3c-vb-not-checked";
    format_draw(q, 1, b, sizeof b); CHECK(has(b, "dsv=20/view?;") && has(b, "v=?(u3c-indexed-unread);") && has(b, "u3c-vb-not-checked;"));
    // worst-case values: bounded, NUL terminated, never longer than the limit; tiny buffers do not overflow
    DrawRec w = full_draw();
    for (auto& v : w.vtx) for (float& f : v) f = -1.23456789e-30f;
    for (float& f : w.cb) f = -9.87654321e+30f;
    w.rt_res = ~0ull; w.vb_va = ~0ull; w.vb_size = ~0ull; w.all = w.ord = w.count = w.instances = w.start = 0xffffffffu;
    n = format_draw(w, 0xffffffffu, b, sizeof b);
    CHECK(n > 0 && size_t(n) <= kLineLimit && b[n] == 0 && std::strlen(b) == size_t(n));
    char tiny[24];
    n = format_draw(w, 1, tiny, sizeof tiny); CHECK(n < int(sizeof tiny) && tiny[n] == 0);
    CHECK(format_draw(w, 1, nullptr, 0) == 0 && format_draw(w, 1, tiny, 0) == 0);
    // typical menu values stay well inside the limit with all fields present
    n = format_draw(full_draw(), 60, b, sizeof b); CHECK(n < 420);
}

void test_frame_and_other() {
    Hook h; h.a.set_armed(true);
    CHECK(!h.a.present(5000000)); CHECK(h.a.sampling);
    for (int i = 0; i < 3; ++i) h.menu();
    h.draw(false, 0x15c8c0bd, 0xf9027c72); h.draw(false, 0xb569109e, 0xc1b58903, 2958, true);
    for (int i = 0; i < 5; ++i) h.draw(false, 0xb569109e, 0xc1b58903, 100, true);
    CHECK(h.a.present(6000000));
    Frame f; const unsigned s = h.a.take(f); CHECK(s == 1);
    CHECK(f.all == 10 && f.menu == 3 && f.menu_verts == 12 && f.others.draws == 7 && f.others.n == 2);
    char b[kLineLimit + 1];
    format_frame(f, s, b, sizeof b);
    CHECK(std::string(b) == "event=ui3d-census;kind=frame;s=1;draws=10;menu=3;logged=3;indexed=0;menu-units=12;other=7;other-keys=2;overflow=0;no-pso=0");
    CHECK(format_other(f, s, b, sizeof b) > 0);
    // the ACM key is ranked first (6 draws), the movie key second; items carry the names of briefing_observe.h
    const std::string o(b);
    CHECK(o.rfind("event=ui3d-census;kind=other;s=1;keys=2;top=b569109e/c1b58903/t3i-/6/3458/2958/ACM,", 0) == 0);
    CHECK(has(o, ",15c8c0bd/f9027c72/t3n-/1/4/4/movie"));
    Frame none; CHECK(format_other(none, 1, b, sizeof b) == 0 && b[0] == 0);
    // top 8 only
    Frame many;
    for (uint32_t k = 0; k < 20; ++k) acz::brobs::note_draw(many.others, 0x1000 + k, 0x2000, 3, false, false, 4, 1);
    format_other(many, 1, b, sizeof b);
    unsigned items = 1; for (const char* c = std::strstr(b, "top="); c && *c; ++c) if (*c == ',') ++items;
    CHECK(items == kOtherTop);
}

void test_cap_and_stale() {
    Hook h; h.a.set_armed(true); h.a.present(1000000);
    for (unsigned i = 0; i < 80; ++i) h.menu();
    CHECK(h.a.f.menu == 80 && h.a.f.all == 80 && h.a.f.menu_verts == 320);
    CHECK(h.a.f.rec[kMaxDraws - 1].ord == kMaxDraws && h.a.f.rec[0].ord == 1);
    h.a.present(1500000);
    Frame f; h.a.take(f);
    char b[kLineLimit + 1]; Collect col; std::vector<std::string>& lines = col.v;
    const unsigned n = emit_frame(f, 1, 1000, col);
    CHECK(n == 1 + kMaxDraws && lines.size() == n);                              // frame line + 48 draws, no other keys
    CHECK(has(lines[0], "menu=80;logged=48;") && has(lines[1], "ord=1;") && has(lines[48], "ord=48;"));
    // a short budget drops draws first and keeps the frame line; the other-keys line has priority over the last draws
    Frame g; g.all = 5; g.menu = 3; g.rec[0] = full_draw(); g.rec[1] = full_draw(); g.rec[2] = full_draw();
    acz::brobs::note_draw(g.others, 0xb569109e, 0xc1b58903, 3, true, false, 10, 1);
    lines.clear(); unsigned en = emit_frame(g, 1, 3, col); CHECK(en == 3);
    CHECK(has(lines[0], "kind=frame") && has(lines[1], "kind=draw") && has(lines[2], "kind=other"));
    lines.clear(); en = emit_frame(g, 1, 0, col); CHECK(en == 0 && lines.empty());
    lines.clear(); en = emit_frame(g, 1, 1, col); CHECK(en == 1 && has(lines[0], "kind=frame"));
    // a commit from a frame that closed meanwhile is ignored (the serial advanced)
    Hook s; s.a.set_armed(true); s.a.present(1000000);
    uint32_t serial, slot, all, ord;
    CHECK(s.a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
    s.a.present(1500000); Frame t; s.a.take(t);
    DrawRec d = full_draw(); s.a.commit(serial, slot, d);
    CHECK(s.a.f.rec[0].count == 0 && t.menu == 1);
    (void)b;
}

void test_sampling_period_and_latch() {
    Hook h; h.a.set_armed(true);
    uint64_t t = 1000000;
    CHECK(!h.a.present(t) && h.a.sampling);                                       // first present while armed starts a sample
    uint32_t serial, slot, all, ord;
    CHECK(h.a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
    t += 16000; CHECK(h.a.present(t));
    Frame f; unsigned s = h.a.take(f); CHECK(s == 1 && !h.a.sampling);
    CHECK(!h.a.finish(t, 3));
    // not sampling until a second has passed since the last emission
    for (int i = 0; i < 20; ++i) { t += 16000; CHECK(!h.a.present(t) && !h.a.sampling); }
    t += 800000; CHECK(!h.a.present(t) && h.a.sampling);
    // an empty frame is not emitted: the next one is sampled instead
    t += 16000; CHECK(!h.a.present(t) && h.a.sampling);
    // draws while not sampling are skipped (the hot path's early out)
    Hook idle; idle.a.set_armed(true);
    CHECK(idle.a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::skip);
    // the sample cap latches and says why; it stays latched until the file is removed and created again
    Hook c; c.a.set_armed(true);
    const char* why = nullptr; uint64_t u = 2000000; unsigned frames = 0;
    for (unsigned i = 0; i < 200 && !c.a.latched; ++i) {
        c.a.present(u);
        if (c.a.sampling) { c.menu(); u += 16000; if (c.a.present(u)) { Frame x; c.a.take(x); ++frames; why = c.a.finish(u, 2); } }
        u += 1000000;
    }
    CHECK(frames == kMaxSamples && c.a.latched && why && std::strcmp(why, "samples") == 0);
    CHECK(c.a.samples == kMaxSamples && c.a.lines == 2 * kMaxSamples);
    c.a.present(u + 5000000); CHECK(!c.a.sampling);
    CHECK(c.a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::skip);
    c.a.set_armed(true); CHECK(c.a.latched);                                      // still armed: still latched
    c.a.set_armed(false); c.a.set_armed(true);
    CHECK(!c.a.latched && c.a.samples == 0 && c.a.lines == 0 && !c.a.latch_why);
    c.a.present(u); CHECK(c.a.sampling);
    // the line cap: the budget always leaves room for the final latched line
    Accumulator L; L.set_armed(true);
    CHECK(L.budget() == kMaxLines - 1);
    L.samples = 1; CHECK(L.finish(10, kMaxLines - 2) && std::strcmp(L.latch_why, "lines") == 0 && L.latched);
    Accumulator M; M.set_armed(true); M.samples = 1; CHECK(!M.finish(10, kMaxLines - 4) && M.budget() == 3);
    // disarming drops an open frame
    Hook d; d.a.set_armed(true); d.a.present(1000000); d.menu(); d.a.set_armed(false);
    CHECK(!d.a.sampling && d.a.f.all == 0 && !d.a.present(9000000));
    char b[kLineLimit + 1];
    int n = format_latched("samples", 60, 1234, b, sizeof b);
    CHECK(n > 0 && has(b, "event=ui3d-census;latched;reason=samples;samples=60;lines=1234;") && std::strncmp(b, "event=ui3d-census;latched", 25) == 0);
}

void test_present_emitter() {
    // The process-wide path: arm, hook one frame, present, check the emitted lines, latch after the cap, disarm.
    set_armed(false);
    Fail fail; present(1000000, fail);                          // not armed: nothing, no lock
    CHECK(!sampling());
    set_armed(true);
    Collect sink; std::vector<std::string>& lines = sink.v;
    present(2000000, sink);                                                       // starts the sample
    CHECK(sampling() && lines.empty());
    uint32_t serial, slot, all, ord;
    CHECK(admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
    DrawRec d = full_draw(); d.all = all; d.ord = ord; commit(serial, slot, d);
    CHECK(admit(false, 0x384ade02, 0xaaaaaaaa, 3, true, false, 100, 1, serial, slot, all, ord) == Admit::count);
    present(2016000, sink);
    CHECK(!sampling() && lines.size() == 3);
    CHECK(has(lines[0], "kind=frame;s=1;draws=2;menu=1;") && has(lines[1], "kind=draw;s=1;ord=1;"));
    CHECK(has(lines[2], "kind=other;s=1;keys=1;top=384ade02/aaaaaaaa/t3i-/1/100/100/SelMap"));
    lines.clear();
    // run to the latch
    uint64_t t = 4000000; unsigned guard = 0;
    while (guard++ < 500 && (lines.empty() || !has(lines.back(), "latched"))) {
        present(t, sink); t += 1000;
        if (sampling()) {
            CHECK(admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
            d.all = all; d.ord = ord; commit(serial, slot, d); t += 16000; present(t, sink);
        }
        t += 1000000;
    }
    CHECK(!lines.empty() && has(lines.back(), "event=ui3d-census;latched;reason=samples;samples=60;"));
    CHECK(lines.size() <= kMaxLines);
    set_armed(false);
    CHECK(!sampling());
}


// ---- ACM mode (ui3d-census-acm.arm; ui-3d.md 8.4)
AcmRec acm_rec(uint32_t all, uint32_t ord, bool readable, float shift) {
    AcmRec r;
    r.c = full_draw(); r.c.all = all; r.c.ord = ord; r.c.indexed = true; r.c.count = 600; r.c.start = 30; r.c.instances = 1;
    r.base = -4; r.ib_stride = 2;
    r.cb_why = nullptr; r.cb_root = 5; r.cb_mask = 3; r.cb[0] = 258; r.cb[1] = 0x3f800000u;
    r.t10_va = 0x7f0000;
    if (readable) {
        r.t10_why = nullptr;
        for (int i = 0; i < 16; ++i) { r.a[i] = float(i) + shift; r.b[i] = (i % 5 == 0) ? 1.0f : 0.0f; }
        r.k360 = 1.0f; r.k400[0] = 512; r.k400[1] = -448; r.k400[2] = 2048; r.k400[3] = 2048;
    } else r.t10_why = "u3c-t10-read-fault";
    return r;
}
void test_acm_format() {
    char b[kLineLimit + 1];
    const AcmRec first = acm_rec(7, 2, true, 0.0f), same = acm_rec(9, 2, true, 0.0f), diff = acm_rec(11, 3, true, 0.5f), bad = acm_rec(13, 3, false, 0.0f);
    int n = format_acm(first, &first, 4, b, sizeof b); std::string s(b);
    CHECK(n == int(s.size()) && n < int(kLineLimit));
    CHECK(has(s, "event=ui3d-census;kind=acm;s=4;all=7;ord=2;idx=1;n=600;st=30;base=-4;inst=1;ibs=2;"));
    CHECK(has(s, "rt=0x1234abcd/v10/r9/1280x720;") && has(s, "pso-rtv0=28;") && has(s, "dsv=20/view20;") && has(s, "bl=1:5,6,1,1,0,1;") && has(s, "dp=1/7/0/stc0;"));
    CHECK(has(s, "cb10=rc5:0x102,0x3f800000,?,?;"));   // words print as hex: 258 = 0x102
    CHECK(has(s, "t10=0x7f0000/read;k360=1;k400=512,-448,2048,2048;A=first"));
    format_acm(same, &first, 4, b, sizeof b); CHECK(has(b, "A=same"));
    format_acm(diff, &first, 4, b, sizeof b); CHECK(has(b, "A=differs"));
    format_acm(bad, &first, 4, b, sizeof b); s = b;      // unreadable record: `?` everywhere, the reason kept, never numbers
    CHECK(has(s, "t10=0x7f0000(u3c-t10-read-fault);k360=?;k400=?;A=?") && !has(s, "A=same"));
    format_acm(diff, &bad, 4, b, sizeof b); CHECK(has(b, "A=?"));
    AcmRec nocb = first; nocb.cb_why = "u3c-cb10-unwritten"; nocb.t10_va = 0; nocb.t10_why = "u3c-t10-unbound"; nocb.c.rt_known = false; nocb.c.pso_known = false;
    format_acm(nocb, &first, 4, b, sizeof b); s = b;
    CHECK(has(s, "rt=?;pso-rtv0=?;dsv=?;bl=?;dp=?;cb10=?(u3c-cb10-unwritten);t10=?(u3c-t10-unbound);"));
    format_acm_mat(first, true, 4, b, sizeof b); s = b;
    CHECK(has(s, "kind=acm-a;s=4;all=7;A=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15") && s.size() < kLineLimit);
    format_acm_mat(first, false, 4, b, sizeof b); CHECK(has(b, "kind=acm-b;s=4;all=7;B=1,0,0,0,0,1,") && strlen(b) < kLineLimit);
    format_acm_mat(bad, false, 4, b, sizeof b); CHECK(has(b, "B=?(u3c-t10-read-fault)"));
    // worst-case widths still fit the line limit
    AcmRec w = first; for (int i = 0; i < 16; ++i) { w.a[i] = -123456.789f * float(i + 1); w.b[i] = -0.000123456789f; }
    format_acm_mat(w, true, 99999, b, sizeof b); CHECK(strlen(b) < kLineLimit);
    format_acm_mat(w, false, 99999, b, sizeof b); CHECK(strlen(b) < kLineLimit);
    AcmRec big = first; big.base = -2147483647; big.c.count = 4294967295u; big.c.start = 4294967295u; big.cb_mask = 15; for (auto& c : big.cb) c = 0xffffffffu;
    big.t10_va = 0xffffffffffffffffull; big.c.rt_res = 0xffffffffffffffffull; big.k360 = -1.234567e30f;
    format_acm(big, &first, 99999, b, sizeof b); CHECK(strlen(b) < kLineLimit);
}
void test_acm_sampling() {
    // Not in acm mode: an ACM draw is only counted in the OTHER keys (no take_acm), the menu path is unchanged.
    Accumulator a; a.set_armed(true); a.present(1000000);
    uint32_t serial, slot, all, ord;
    CHECK(a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
    CHECK(a.admit(false, kAcmVs8, 0xc1b58903u, 4, true, false, 99, 1, serial, slot, all, ord) == Admit::count && all == 2);
    // acm mode: take_acm with slot = running ACM index, ord = menu draws before, all shared with the ps2_v ordinals
    Accumulator m; m.set_armed(true, false, true); m.present(1000000);
    CHECK(m.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take && all == 1 && ord == 1);
    CHECK(m.admit(false, kAcmVs8, 0xc1b58903u, 4, true, false, 600, 1, serial, slot, all, ord) == Admit::take_acm && all == 2 && ord == 1 && slot == 0);
    const uint32_t s0 = serial, sl0 = slot;
    CHECK(m.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take && all == 3 && ord == 2);
    CHECK(m.admit(false, 0x384ade02u, 0xaaaaaaaau, 3, true, false, 5, 1, serial, slot, all, ord) == Admit::count);   // another key: not ACM
    CHECK(m.admit(false, kAcmVs8, 0xc1b58903u, 4, true, false, 300, 1, serial, slot, all, ord) == Admit::take_acm && all == 5 && ord == 2 && slot == 1);
    m.commit_acm(s0, sl0, acm_rec(2, 1, true, 0.0f));
    // fill past the cap: draws beyond kMaxAcm are counted (acm=), not taken
    for (unsigned i = 2; i < kMaxAcm; ++i) CHECK(m.admit(false, kAcmVs8, 0xc1b58903u, 4, true, false, 3, 1, serial, slot, all, ord) == Admit::take_acm && slot == i);
    CHECK(m.admit(false, kAcmVs8, 0xc1b58903u, 4, true, false, 3, 1, serial, slot, all, ord) == Admit::count);
    CHECK(m.present(1500000)); Frame f; m.take(f);
    CHECK(f.acm_mode && f.acm_n == kMaxAcm + 1);
    Collect col; emit_frame(f, 1, 1000, col);
    // frame line carries the ACM totals; slot 0 committed, slot 1 admitted-not-committed, the rest are uncommitted defaults (never numbers)
    CHECK(has(col.v[0], "kind=frame;") && has(col.v[0], ";acm=33;acm-logged=32"));
    unsigned acm = 0, mat_a = 0, mat_b = 0, inc = 0;
    for (auto& l : col.v) { if (has(l, "kind=acm;")) ++acm; if (has(l, "kind=acm-a;")) ++mat_a; if (has(l, "kind=acm-b;")) ++mat_b; if (has(l, "kind=acm;") && has(l, "incomplete=admitted-not-committed")) ++inc; }
    CHECK(acm == kMaxAcm && mat_a == 1 && mat_b == 1 && inc == kMaxAcm - 1);
    for (auto& l : col.v) CHECK(l.size() < kLineLimit);
    // the line budget: whole ACM draws only, and the frame line always first
    Collect tiny; const unsigned n = emit_frame(f, 1, 8, tiny);
    CHECK(n <= 8 && tiny.v.size() == n && has(tiny.v[0], "kind=frame"));
    // a stale commit (frame closed) and a commit in a non-acm frame are ignored
    m.commit_acm(s0, 0, acm_rec(2, 1, true, 0.0f)); CHECK(m.f.acm_n == 0 && m.f.acm_pending[0] == false);
    Accumulator n2; n2.set_armed(true); n2.f.acm_mode = false; n2.commit_acm(n2.f.serial, 0, acm_rec(2, 1, true, 0.0f)); CHECK(!n2.f.acm_mode);
    // switching the mode while armed restarts the sample; disarming clears the ACM state
    m.set_armed(true, false, false); CHECK(!m.sampling && !m.acm);
    m.set_armed(false); CHECK(!m.armed && !m.f.acm_mode);
}
// POV-RENDER census (other-povs.md section 2): `kind=acm-a` for the first draw AND the first draw of every further distinct camera (an offscreen pass first,
// then the scene camera), at most kMaxAcmCameras per frame; an unreadable record never prints a matrix; a one-camera frame prints exactly one (unchanged).
void test_acm_distinct_cameras() {
    static Frame f;
    f.acm_mode = true; f.acm_n = 7;
    f.acm[0] = acm_rec(1, 0, true, 0.0f);    // camera P (offscreen pass)
    f.acm[1] = acm_rec(2, 0, true, 0.0f);    // P again
    f.acm[2] = acm_rec(3, 0, true, 0.5f);    // camera S (scene)
    f.acm[3] = acm_rec(4, 0, false, 0.0f);   // unreadable: no matrix
    f.acm[4] = acm_rec(5, 0, true, 0.5f);    // S again
    f.acm[5] = acm_rec(6, 0, true, 0.0f);    // P again
    f.acm[6] = acm_rec(7, 0, true, 2.0f);    // camera T
    Collect col; emit_frame(f, 3, 1000, col);
    std::vector<std::string> mats;
    for (auto& l : col.v) if (has(l, "kind=acm-a;")) mats.push_back(l);
    CHECK(mats.size() == 3 && has(mats[0], "all=1;A=0,1,") && has(mats[1], "all=3;A=0.5,1.5,") && has(mats[2], "all=7;A=2,3,"));
    unsigned b = 0; for (auto& l : col.v) if (has(l, "kind=acm-b;")) ++b;
    CHECK(b == 7);
    // the cap: 6 distinct cameras -> kMaxAcmCameras matrices
    static Frame g;
    g.acm_mode = true; g.acm_n = 6;
    for (unsigned i = 0; i < 6; ++i) g.acm[i] = acm_rec(i + 1, 0, true, float(i));
    Collect c2; emit_frame(g, 4, 1000, c2);
    unsigned a = 0; for (auto& l : c2.v) if (has(l, "kind=acm-a;")) ++a;
    CHECK(a == kMaxAcmCameras);
    // one camera: exactly one matrix (the previous output)
    static Frame h;
    h.acm_mode = true; h.acm_n = 3;
    for (unsigned i = 0; i < 3; ++i) h.acm[i] = acm_rec(i + 1, 0, true, 0.0f);
    Collect c3; emit_frame(h, 5, 1000, c3);
    a = 0; for (auto& l : c3.v) if (has(l, "kind=acm-a;")) ++a;
    CHECK(a == 1);
}

}  // namespace

// W12-3 P2: a snapshot taken between admit and commit must never label a previous sample's record (or an empty one) as the current draw.
void test_snapshot_never_carries_old_record() {
    Hook h; h.a.set_armed(true); h.a.present(1000000);
    h.menu(); h.menu();
    CHECK(h.a.present(1500000)); Frame first; h.a.take(first); h.a.finish(1500000, 3);   // sample 1: two committed draws (rec[0], rec[1] populated)
    CHECK(!h.a.present(2600000) && h.a.sampling);                                      // sample 2 window opens
    uint32_t serial, slot, all, ord;
    CHECK(h.a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
    CHECK(h.a.present(2700000)); Frame t; h.a.take(t);                                  // snapshot BEFORE the draw commits
    h.a.commit(serial, slot, full_draw());                                             // late commit: rejected by the serial
    Collect col; emit_frame(t, 2, 1000, col);
    CHECK(col.v.size() == 2 && has(col.v[0], "kind=frame") && has(col.v[0], "menu=1;"));
    CHECK(has(col.v[1], "kind=draw") && has(col.v[1], "ord=1;") && has(col.v[1], "incomplete=") && !has(col.v[1], "vb="));
    // a committed draw in the same sample still prints normally
    CHECK(!h.a.present(3800000));
    CHECK(h.a.admit(true, kMenuVs8, kMenuPs8, 3, false, false, 4, 1, serial, slot, all, ord) == Admit::take);
    h.a.commit(serial, slot, full_draw());
    CHECK(h.a.present(3900000)); Frame u; h.a.take(u);
    Collect c2; emit_frame(u, 3, 1000, c2);
    CHECK(c2.v.size() == 2 && !has(c2.v[1], "incomplete="));
}

// GEO (debrief-3d-map.md 7): index scan with strip-cut markers, vertex ranges, the samples in index order, the line, the emitter placement.
void test_geo() {
    // two 4-vertex strips separated by one 0xFFFF marker (the debrief's sprite batches: n = 4 k + (k - 1)), base indices 10..17
    const uint16_t idx16[9] = {10, 11, 12, 13, 0xffff, 14, 15, 16, 17};
    uint8_t ib[64]; std::memcpy(ib, idx16, sizeof idx16);
    Geo g; geo_scan_indices(ib, 9, 2, g);
    CHECK(!g.why && g.nread == 9 && g.cuts == 1 && g.imin == 10 && g.imax == 17 && g.nsidx == 3 && g.sidx[0] == 10 && g.sidx[2] == 12);
    for (uint32_t i = 0; i < 8; ++i) {
        const float f[4] = {1800.0f + 10.0f * float(i), 1900.0f + float(i), 16777216.0f, 1.0f};
        geo_add_vertex(g, i, f);
    }
    CHECK(!g.why && g.nv == 8 && g.x0 == 1800.0f && g.x1 == 1870.0f && g.y0 == 1900.0f && g.y1 == 1907.0f && g.q0 == 1.0f && g.q1 == 1.0f);
    CHECK(g.nsv == 3 && g.sv[1][0] == 1810.0f && g.sv[2][1] == 1902.0f);
    // 32-bit markers; only markers; bad stride; non-finite vertex
    const uint32_t idx32[3] = {5, 0xffffffffu, 7};
    uint8_t ib4[12]; std::memcpy(ib4, idx32, sizeof idx32);
    Geo h; geo_scan_indices(ib4, 3, 4, h); CHECK(!h.why && h.cuts == 1 && h.imin == 5 && h.imax == 7);
    const uint16_t only[2] = {0xffff, 0xffff}; uint8_t ib2[4]; std::memcpy(ib2, only, 4);
    Geo m; geo_scan_indices(ib2, 2, 2, m); CHECK(m.why && std::strcmp(m.why, "u3c-geo-only-markers") == 0);
    Geo z; geo_scan_indices(ib, 9, 3, z); CHECK(z.why && std::strcmp(z.why, "u3c-geo-no-indices") == 0 && z.nread == 0);
    Geo nf; geo_scan_indices(ib, 9, 2, nf);
    const float bad[4] = {1.0f, std::nanf(""), 0.0f, 1.0f}; geo_add_vertex(nf, 0, bad); CHECK(nf.why && std::strcmp(nf.why, "u3c-geo-vertex-nonfinite") == 0);
    // the line: GS ranges, pixel bbox through cb0 (682,-448,2048,2048) over 2560 x 1440, samples
    DrawRec d = full_draw(); d.indexed = true; d.ia = 5; d.nv = 0; d.vtx_why = "u3c-indexed-unread";
    d.cb[0] = 682; d.vp[2] = 2560; d.vp[3] = 1440;
    d.geo = g; d.geo.ibs = 2; d.geo.cut = 1; d.geo.ni = 9; d.geo.base = 0;
    char b[kLineLimit + 1];
    const int n = format_geo(d, 4, b, sizeof b); std::string s(b);
    CHECK(n > 0 && n < int(kLineLimit) && s.rfind("event=ui3d-census;kind=geo;s=4;ord=2;all=5;ia=5;cut=1;ibs=2;base=0;ni=9/9;cuts=1;imin=10;imax=17;nv=8;", 0) == 0);
    CHECK(has(s, "x=1800,1870;y=1900,1907;z=1.67772e+07,1.67772e+07;q=1,1;"));
    CHECK(has(s, "px=349.09,244.29,611.85,266.79;") &&has(s, "v=i10:1800,1900,1.67772e+07,1|i11:1810,1901,1.67772e+07,1|i12:1820,1902,1.67772e+07,1"));
    format_draw(d, 4, b, sizeof b); CHECK(has(b, ";ia=5") && has(b, "v=?(u3c-indexed-unread);"));
    // failures print `?`
    DrawRec e = d; e.geo = Geo{}; e.geo.why = "u3c-ib-address-unresolved"; e.geo.ibs = 2;
    format_geo(e, 1, b, sizeof b); CHECK(has(b, "geo=?(u3c-ib-address-unresolved)") && !has(b, "x="));
    e.geo = g; e.geo.why = "u3c-vb-read-fault"; format_geo(e, 1, b, sizeof b); CHECK(has(b, "imax=17;nv=8;geo=?(u3c-vb-read-fault)"));
    e.geo = g; e.cb_mask = 7; format_geo(e, 1, b, sizeof b); CHECK(has(b, "px=?;"));
    e.geo = g; e.geo.vtrunc = true; format_geo(e, 1, b, sizeof b); CHECK(has(b, "nv=8t;"));
    // long-index worst case stays bounded
    e.geo = g; e.geo.x0 = -1.2345678e30f; e.geo.ni = 4294967295u; e.geo.imin = 4294967294u; e.geo.base = -2147483647;
    CHECK(format_geo(e, 60, b, sizeof b) < int(kLineLimit));
    // the emitter: a geo line follows its indexed draw; a non-indexed draw and a not-read indexed draw get none
    Frame f; f.menu = 3; f.all = 3;
    f.rec[0] = full_draw(); f.rec[0].ord = 1;
    f.rec[1] = d; f.rec[1].ord = 2;
    f.rec[2] = d; f.rec[2].ord = 3; f.rec[2].geo = Geo{};   // not attempted (over kMaxGeo)
    Collect col; emit_frame(f, 9, 1000, col);
    CHECK(col.v.size() == 5 && has(col.v[1], "kind=draw;s=9;ord=1;") && has(col.v[2], "kind=draw;s=9;ord=2;") && has(col.v[3], "kind=geo;s=9;ord=2;"));
    CHECK(has(col.v[4], "kind=draw;s=9;ord=3;"));
    Collect c3; emit_frame(f, 9, 3, c3); CHECK(c3.v.size() == 3);   // the budget bounds geo lines like draw lines
}

int main() {
    test_geo();
    test_snapshot_never_carries_old_record();
    test_derive();
    test_format_draw();
    test_frame_and_other();
    test_cap_and_stale();
    test_sampling_period_and_latch();
    test_present_emitter();
    test_acm_format();
    test_acm_sampling();
    test_acm_distinct_cameras();
    std::printf("ui3d_census_offline: %u checks passed\n", g_checks);
    return 0;
}
