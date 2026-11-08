// DEBRIEF-3D stage 2 offline tests: the producer's DEBRIEF frame kind (src/acz-vr/brief3d_live.h Producer::dbr3d_draw, dbr3d.h live route)
// driven with fake lists, queues, fence completions and draws shaped like the live census of the debrief (live-1508, debrief-3d-map.md 7.2):
// arming by the MISSION RESULT window quad, the frame split (map scene on the wraparound, overlay on the panel layers), indexed replays (the
// referenced range and the rebased base vertex), depth GREATER_EQUAL with the DSV, the dbr3d upload span and budget, every refusal rejecting
// the frame, the menu-frame interaction, publication and the summary line. Nothing here touches D3D12; fixture success says nothing about the
// game or a headset.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "brief3d_live.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::brief3d;
using namespace acz::brief3d::live;
using acz::vscreen::Completion;
namespace db = acz::dbr3d;
namespace u3 = acz::ui3d;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

namespace {

const uint64_t kUploadVa = 0x40000000ull;
const u3::Cb0 kGs{682.0f, -448.0f, 2048.0f, 2048.0f};     // the GS mapping [E]
const u3::Cb0 kPix{2560.0f, -1440.0f, 1280.0f, 720.0f};   // the pixel-space mapping [E]
int g_rt = 0;

EyeView make_eye(double px, double py, double pz, double ipd_x) {
    EyeView e;
    e.position[0] = px + ipd_x; e.position[1] = py; e.position[2] = pz;
    e.orientation[0] = 0; e.orientation[1] = 0; e.orientation[2] = 0; e.orientation[3] = 1;
    e.fov[0] = -0.82; e.fov[1] = 0.74; e.fov[2] = 0.79; e.fov[3] = -0.87;
    return e;
}
PoseIn pose(uint32_t gen = 1) {
    PoseIn p; p.ok = true; p.space_generation = gen;
    p.eyes[0] = make_eye(0.0, 1.6, 0.0, -0.032); p.eyes[1] = make_eye(0.0, 1.6, 0.0, 0.032);
    return p;
}
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
void screen(uint32_t encoded_gen = 1) {
    acz::vscreen::Options o; const float head[3] = {0.0f, 1.6f, 0.0f}, q[4] = {0, 0, 0, 1};
    acz::vscreen::publish_placement(acz::vscreen::place(o, head, q, 16.0f / 9.0f, encoded_gen - 1), head, encoded_gen);
}
Producer& fresh(Producer& p, bool dbr = true) {
    u3::Options uo; uo.on = true; p.configure_ui3d(uo);
    if (dbr) { db::Options o; o.on = true; p.configure_dbr3d(o); }
    p.gpu_ready(kUploadVa); screen();
    return p;
}
void execute(Producer& p, const void* q, const void* list) {
    const void* ls[1] = {list};
    const auto t = p.before_execute(q, ls, 1);
    p.after_execute(t, true);
}

// vertices: quads (x0, y0, x1, y1) as strips of 4, z and q per quad
struct Q { float x0, y0, x1, y1, z; };
std::vector<uint8_t> verts(const std::vector<Q>& qs) {
    std::vector<uint8_t> b(qs.size() * 4 * 64, 0);
    for (size_t k = 0; k < qs.size(); ++k) {
        const Q& q = qs[k];
        const float xy[4][2] = {{q.x0, q.y0}, {q.x1, q.y0}, {q.x0, q.y1}, {q.x1, q.y1}};
        for (int i = 0; i < 4; ++i) {
            const float v[16] = {xy[i][0], xy[i][1], q.z, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, float(i & 1), float(i >> 1), 0, 0, 0.2f, 0.3f, 0.4f, 0};
            std::memcpy(b.data() + (k * 4 + i) * 64, v, 64);
        }
    }
    return b;
}
// the state facts of a census debrief draw: ps2_v / d34ee153, RT 2560x1440 fmt 10, PSO rtv 10 / dsv 20, depth ALWAYS, stencil always, vp
// 0,0,2560,1440, scissor 320,0,2240,1440, stride 64, blend SRC_ALPHA / ONE (map content) unless set
DbrFacts facts(const void* list, int64_t now, const u3::Cb0& cb, const std::vector<uint8_t>& v, bool indexed, uint32_t imin = 0) {
    DbrFacts d;
    UiFacts& f = d.ui;
    f.list = list; f.now_ms = now; f.vs8 = u3::kPs2Vs8; f.ps8 = db::kMenuPs8; f.indexed = indexed; f.instances = 1; f.topology_type = 3;
    f.rt = &g_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = kCmpAlways; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = cb; f.vb_why = nullptr; f.stride = 64;
    f.blend = true; f.blend_src = 5; f.blend_dst = 2;
    d.nv = uint32_t(v.size() / 64); d.vtx = v.data(); d.vtx_why = nullptr;
    if (indexed) { d.imin = imin; d.index_count = d.nv + d.nv / 4 - 1; d.start_index = 12; d.base = 0; }
    return d;
}
// the MISSION RESULT window quad as the stage-2 seam sees it (UiFacts, non-indexed, SRC_ALPHA / INV_SRC_ALPHA, z 16,777,216)
UiFacts window_ui(const void* list, int64_t now) {
    UiFacts f;
    const std::vector<uint8_t> v = verts({{1817, 1880, 2278, 2168, 16777216.0f}});
    DbrFacts d = facts(list, now, kGs, v, false);
    f = d.ui; f.vertices = 4; f.blend_src = 5; f.blend_dst = 6;
    std::memcpy(f.vtx, v.data(), 256);
    return f;
}

// one game frame with the window quad through the stage-2 seam (a published MENU frame, value 1) that arms the debrief route
void arm(Producer& p, const void* list, const void* queue) {
    const UiPlan w = p.ui3d_draw(window_ui(list, 90), pose(), done(0));
    CHECK(w.record);
    p.ui3d_commit(w);
    p.on_present(91); execute(p, queue, list); p.on_reset(list, true);
    CHECK(p.dbr3d_armed(100));
}
struct Eyes { std::vector<uint8_t> e[2]; uint8_t* p[2]; Eyes() { for (int i = 0; i < 2; ++i) { e[i].assign(db::kMaxVerticesPerDraw * 64, 0); p[i] = e[i].data(); } } };
DbrPlan step(Producer& p, const DbrFacts& d, Eyes& eyes, uint64_t completed = 0) {
    const DbrPlan pl = p.dbr3d_draw(d, pose(), done(completed), eyes.p);
    if (pl.record) p.dbr3d_commit(pl);
    return pl;
}
float vx(const Eyes& e, unsigned eye, unsigned v, unsigned k) { float f; std::memcpy(&f, e.e[eye].data() + 64 * v + 4 * k, 4); return f; }

// the census frame shapes (debrief-3d-map.md 7.2)
const std::vector<uint8_t> kFill = [] { std::vector<uint8_t> b(2 * 64, 0); const float a[4] = {400, 300, 1, 1}, c[4] = {2100, 300, 1, 1};
                                         std::memcpy(b.data(), a, 16); std::memcpy(b.data() + 64, c, 16); return b; }();   // 2 vertices, pixel space
std::vector<uint8_t> strips() {   // the long-strip draw: 3 strips across the picture (vertices as quads for the fixture)
    std::vector<Q> qs;
    for (int i = 0; i < 60; ++i) qs.push_back({1800.0f + 8.0f * i, 1900.0f + 3.0f * i, 1806.0f + 8.0f * i, 1904.0f + 3.0f * i, 4000.0f});
    return verts(qs);
}
std::vector<uint8_t> text_batch() {   // the 312-quad result-table text, inside the window
    std::vector<Q> qs;
    for (int i = 0; i < 312; ++i) qs.push_back({1830.0f + 10.0f * float(i % 40), 1900.0f + 30.0f * float(i / 40), 1838.0f + 10.0f * float(i % 40), 1914.0f + 30.0f * float(i / 40), 1.0f});
    return verts(qs);
}

void test_arming() {
    Producer p; fresh(p);
    int L = 0;
    CHECK(p.dbr3d_configured() && !p.dbr3d_armed(100));
    // the first debrief frame: stage 2 refuses the indexed draw (nothing open), then opens a MENU frame at the window quad and arms the route
    UiFacts ix = window_ui(&L, 100); ix.indexed = true; ix.vb_why = "ui3d-indexed-draw";
    CHECK(streq(p.ui3d_draw(ix, pose(), done(0)).why, "ui3d-indexed-draw") && !p.dbr3d_armed(100));
    const UiPlan w = p.ui3d_draw(window_ui(&L, 101), pose(), done(0));
    CHECK(w.record && p.dbr3d_armed(101) && p.dbr3d_armed(101 + db::kArmMs) && !p.dbr3d_armed(102 + db::kArmMs));
    p.ui3d_commit(w);
    // the text batch of that frame now goes to the debrief route: the open MENU frame is not a debrief frame -> rejected (flat screen)
    Eyes eyes; const std::vector<uint8_t> tb = text_batch();
    CHECK(streq(step(p, facts(&L, 102, kGs, tb, true, 100), eyes).why, "dbr3d-menu-frame-open"));
    CHECK(p.dbr3d_stats().arms == 1 && p.dbr3d_stats().refused_draws == 1);
    // dbr3d not configured: the window never arms, dbr3d_draw is off
    Producer q; fresh(q, false);
    CHECK(q.ui3d_draw(window_ui(&L, 100), pose(), done(0)).record && !q.dbr3d_armed(100) && !q.dbr3d_configured());
    CHECK(streq(q.dbr3d_draw(facts(&L, 101, kGs, tb, true), pose(), done(0), eyes.p).why, "dbr3d-off"));
}

void test_debrief_frame() {
    Producer p; fresh(p);
    int L = 0, Qu = 0;
    arm(p, &L, &Qu);   // the arming frame: a stage-2 MENU frame with the window quad, published
    CHECK(p.dbr3d_armed(100));
    Eyes eyes;
    // 1. the fills (indexed, pixel space): the frame opens; MAP; the clear; the dbr3d span
    DbrPlan a = step(p, facts(&L, 100, kPix, kFill, true, 7), eyes, 1);
    CHECK(a.record && a.clear && a.map && a.layer == -1 && a.indexed && a.nv == 2 && a.base_vertex == -7 && a.bind_dsv);
    CHECK(a.upload_offset[0] == kDbrUploadBase + uint64_t(a.slot) * db::kUploadPerSlot && a.upload_offset[1] == a.upload_offset[0] + 128);
    CHECK(a.upload_offset[1] + 128 <= kUploadTotal);
    // 2. the long strips: indexed GS, depth GREATER_EQUAL + write with the DSV: MAP, rebased on imin
    const std::vector<uint8_t> st = strips();
    DbrFacts sf = facts(&L, 101, kGs, st, true, 214); sf.ui.depth_func = kCmpGreaterEqual; sf.ui.depth_write = true; sf.ui.blend_dst = 6;
    DbrPlan b = step(p, sf, eyes);
    CHECK(b.record && !b.clear && b.map && b.base_vertex == -214 && b.nv == 240 && b.upload_offset[0] == a.upload_offset[1] + 128);
    CHECK(b.eye_draw[0] && b.eye_draw[1] && b.scissor[0][2] > b.scissor[0][0] && b.scissor[0][3] > b.scissor[0][1]);
    // the map lies on the wraparound: the game's z is kept (its own depth test), x moved, stereo disparity small (far)
    CHECK(vx(eyes, 0, 0, 2) == 4000.0f && vx(eyes, 1, 0, 2) == 4000.0f);
    const double disp_map = double(vx(eyes, 0, 0, 0)) - double(vx(eyes, 1, 0, 0));
    // 3. the window quad: the overlay starts, panel layer 0 (non-indexed)
    const std::vector<uint8_t> wv = verts({{1817, 1880, 2278, 2168, 16777216.0f}});
    DbrFacts wf = facts(&L, 102, kGs, wv, false); wf.ui.blend_dst = 6;
    DbrPlan c = step(p, wf, eyes);
    CHECK(c.record && !c.map && c.layer == 0 && !c.indexed && c.nv == 4);
    // 4. the 312-quad text batch (indexed): panel text layer 2
    const std::vector<uint8_t> tb = text_batch();
    DbrPlan d = step(p, facts(&L, 103, kGs, tb, true, 100), eyes);
    CHECK(d.record && !d.map && d.layer == 2 && d.indexed && d.nv == 1248 && d.base_vertex == -100);
    const double disp_text = double(vx(eyes, 0, 0, 0)) - double(vx(eyes, 1, 0, 0));
    CHECK(disp_map > 0 && disp_text > 2.0 * disp_map);   // the panel text floats in front of the wraparound
    // 5. the narration glyph: text
    DbrPlan e = step(p, facts(&L, 104, kGs, verts({{1817, 2180, 1827, 2204, 1.0f}}), false), eyes);
    CHECK(e.record && e.layer == 2);
    // a draw to another target: skipped, the frame goes on
    DbrFacts off = facts(&L, 105, kGs, tb, true); int other = 0; off.ui.rt = &other;
    CHECK(streq(step(p, off, eyes).why, "ui3d-off-target"));
    p.on_present(110);
    execute(p, &Qu, &L);
    p.on_reset(&L, true);
    Held h; CHECK(p.acquire(0, done(2), h) && h.slot == a.slot && h.meta.opaque && !h.meta.pause && h.meta.replays == 5);
    p.release(h);
    const db::LiveStats s = p.dbr3d_stats();
    CHECK(s.frames == 1 && s.usable == 1 && s.published == 1 && s.rejected == 0 && s.map_draws == 2 && s.panel_draws == 3 && s.indexed == 3);
    CHECK(s.vertices == 2 + 240 + 4 + 1248 + 4);
    char line[512];
    CHECK(p.format_dbr3d(line, sizeof line) > 0 && std::string(line).find("summary-dbr3d;armed=1;arms=1;screens=1;frames=1;usable=1;published=1;rejected=0;map-draws=2;panel-draws=3;indexed=3;") == 0);
    CHECK(p.ui3d_summary().find(";published=2;") != std::string::npos);   // the arming menu frame + the debrief frame
}

void test_refusals_reject_the_frame() {
    struct Case { const char* why; void (*edit)(DbrFacts&); };
    const Case cases[] = {
        {"dbr3d-depth-func", [](DbrFacts& d) { d.ui.depth_func = kCmpLess; }},
        {"dbr3d-depth-without-dsv", [](DbrFacts& d) { d.ui.depth_func = kCmpGreaterEqual; d.ui.dsv = 0; }},
        {"ui3d-stencil-not-always", [](DbrFacts& d) { d.ui.stencil_always = false; }},
        {"ui3d-pso-rtv", [](DbrFacts& d) { d.ui.rtv0 = 28; }},
        {"ui3d-instances", [](DbrFacts& d) { d.ui.instances = 2; }},
        {"ui3d-vb-torn", [](DbrFacts& d) { d.vtx_why = "ui3d-vb-torn"; }},
        {"dbr3d-strip-cut-unknown", [](DbrFacts& d) { d.vtx_why = "dbr3d-strip-cut-unknown"; }},
        {"ui3d-cb0-unwritten", [](DbrFacts& d) { d.ui.cb_why = "ui3d-cb0-unwritten"; }},
        {"dbr3d-index-facts", [](DbrFacts& d) { d.index_count = 0; }},
    };
    const std::vector<uint8_t> st = strips();
    for (const Case& k : cases) {
        Producer p; fresh(p);
        int L = 0, Qu = 0;
        arm(p, &L, &Qu);
        Eyes eyes;
        CHECK(step(p, facts(&L, 100, kGs, st, true, 3), eyes, 1).record);   // the frame is open
        DbrFacts bad = facts(&L, 101, kGs, st, true, 3); k.edit(bad);
        CHECK(streq(step(p, bad, eyes).why, k.why));
        CHECK(p.dbr3d_stats().rejected == 1 && streq(p.dbr3d_stats().last_why, k.why));
        CHECK(streq(step(p, facts(&L, 102, kGs, st, true, 3), eyes).why, "ui3d-list-holds-previous-frame"));   // rejected: nothing joins it
        p.on_present(110); execute(p, &Qu, &L); p.on_reset(&L, true);
        Held h; CHECK(!p.acquire(1, done(5), h));   // never published (only the arming menu frame was)
        CHECK(p.dbr3d_stats().rejected == 1 && p.dbr3d_stats().published == 0);
    }
}

void test_other_draws_and_lists() {
    Producer p; fresh(p);
    int L = 0, M = 0, Qu = 0;
    arm(p, &L, &Qu);
    Eyes eyes; const std::vector<uint8_t> st = strips();
    CHECK(step(p, facts(&L, 100, kGs, st, true), eyes, 1).record);
    // a draw of the open frame in another list: rejected
    CHECK(streq(step(p, facts(&M, 101, kGs, st, true), eyes).why, "ui3d-frame-other-list"));
    p.on_present(110); execute(p, &Qu, &L); p.on_reset(&L, true);
    // a non-ps2_v draw (3D / movie) goes through ui3d_draw: rejects the open debrief frame and holds UI3D off
    CHECK(step(p, facts(&L, 200, kGs, st, true), eyes, 2).record);
    UiFacts mv; mv.list = &L; mv.now_ms = 201; mv.vs8 = u3::kMovieVs8;
    CHECK(streq(p.ui3d_draw(mv, PoseIn{}, done(2)).why, "ui3d-frame-has-movie"));
    CHECK(p.dbr3d_stats().rejected == 2);
    p.on_present(210); execute(p, &Qu, &L); p.on_reset(&L, true);
    CHECK(streq(step(p, facts(&L, 300, kGs, st, true), eyes, 3).why, "ui3d-hold"));   // the hold (1 s): nothing opens
    // a frame with no replayed draw (every draw culled is still a draw; an empty frame is impossible here) -- an empty Present closes nothing
    p.on_present(320);
    CHECK(!p.poisoned());
}

void test_budget() {
    Producer p; fresh(p);
    int L = 0, Qu = 0;
    arm(p, &L, &Qu);
    // 1,024 quads (4,096 vertices: the per-draw cap) = 512 KiB for both eyes: 4 fit in the 2 MiB span, the 5th is refused
    std::vector<Q> qs;
    for (int i = 0; i < 1024; ++i) qs.push_back({1800.0f + float(i % 64) * 7.0f, 1880.0f + float(i / 64) * 18.0f, 1804.0f + float(i % 64) * 7.0f, 1890.0f + float(i / 64) * 18.0f, 1.0f});
    const std::vector<uint8_t> big = verts(qs);
    Eyes eyes;
    for (int k = 0; k < 4; ++k) CHECK(step(p, facts(&L, 100 + k, kGs, big, true), eyes, 1).record);
    CHECK(streq(step(p, facts(&L, 110, kGs, big, true), eyes).why, "dbr3d-upload-full"));
    // over the per-draw cap: refused before any rewrite
    Producer q; fresh(q);
    arm(q, &L, &Qu);
    DbrFacts over = facts(&L, 100, kGs, big, true); over.nv = db::kMaxVerticesPerDraw + 1;
    CHECK(streq(step(q, over, eyes, 1).why, "dbr3d-draw-too-many-vertices"));
}

void test_no_pose_and_placement() {
    Producer p; fresh(p);
    int L = 0, Qu = 0;
    arm(p, &L, &Qu);
    Eyes eyes; const std::vector<uint8_t> st = strips();
    CHECK(streq(p.dbr3d_draw(facts(&L, 100, kGs, st, true), PoseIn{}, done(1), eyes.p).why, "ui3d-no-pose"));
    screen(2);   // the vscreen re-placed in another space generation than the pose's
    CHECK(streq(step(p, facts(&L, 101, kGs, st, true), eyes, 1).why, "ui3d-no-screen-placement"));
    screen(1);
    CHECK(step(p, facts(&L, 102, kGs, st, true), eyes, 1).record);
}

// ---- review 2026-10-31 F2: the head turned toward one side of the wraparound map. One end of a full-width indexed map draw lies behind the eye;
// stage 2 dropped the WHOLE draw for that eye (the map vanished from it). Now the eye draws its own list of the triangles in front of it.
PoseIn turned_pose(double yaw_deg) {
    PoseIn p = pose();
    const double h = 0.5 * yaw_deg * 3.14159265358979323846 / 180.0;
    for (int e = 0; e < 2; ++e) { p.eyes[e].orientation[0] = 0; p.eyes[e].orientation[1] = std::sin(h); p.eyes[e].orientation[2] = 0; p.eyes[e].orientation[3] = std::cos(h); }
    return p;
}
std::vector<uint8_t> full_width() {   // 40 quads across the whole GS window (x 1710 .. 2386), the map's far edges included
    std::vector<Q> qs;
    for (int i = 0; i < 40; ++i) qs.push_back({1710.0f + 16.9f * float(i), 1900.0f, 1710.0f + 16.9f * float(i + 1), 1960.0f, 4000.0f});
    return verts(qs);
}
std::vector<uint16_t> tri_list(unsigned quads) {
    std::vector<uint16_t> idx;
    for (unsigned k = 0; k < quads; ++k) { const uint16_t b = uint16_t(4 * k); for (uint16_t v : {b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2), uint16_t(b + 1), uint16_t(b + 3)}) idx.push_back(v); }
    return idx;
}
struct Lists { std::vector<uint16_t> l[2]; uint16_t* p[2]; Lists() { for (int i = 0; i < 2; ++i) { l[i].assign(db::kMaxListIndices, 0xEEEE); p[i] = l[i].data(); } } };

void test_wrap_head_turned() {
    const std::vector<uint8_t> v = full_width();
    const std::vector<uint16_t> idx = tri_list(40);
    auto mk = [&](const void* L, int64_t now) {
        DbrFacts d = facts(L, now, kGs, v, true, 0);
        d.index_count = uint32_t(idx.size()); d.start_index = 0; d.ia_topology = db::kTopoTriList;
        d.ib = reinterpret_cast<const uint8_t*>(idx.data()); d.ib_stride = 2; d.strip_cut = db::kStripCutDisabled;
        return d;
    };
    unsigned found = 0;
    for (double yaw : {100.0, -100.0}) {
        // the finding (stage-2 rule, still the fallback without list buffers): the eye with a vertex behind it draws NOTHING of the map
        {
            Producer p; fresh(p); int L = 0, Qu = 0; arm(p, &L, &Qu); Eyes eyes;
            const DbrPlan a = p.dbr3d_draw(mk(&L, 100), turned_pose(yaw), done(1), eyes.p);
            CHECK(a.record && a.map);
            if (a.eye_draw[0] && a.eye_draw[1]) continue;   // this side keeps the whole map in front of both eyes
            CHECK(!a.own_list[0] && !a.own_list[1]);
            p.dbr3d_commit(a);
        }
        ++found;
        // the fix: that eye draws the triangles in front of it from its own R16 list
        Producer p; fresh(p); int L = 0, Qu = 0; arm(p, &L, &Qu); Eyes eyes; Lists lists;
        const DbrPlan b = p.dbr3d_draw(mk(&L, 100), turned_pose(yaw), done(1), eyes.p, lists.p);
        CHECK(b.record && b.map && b.eye_draw[0] && b.eye_draw[1]);
        CHECK(b.own_list[0] || b.own_list[1]);
        CHECK(b.list_topology == db::kTopoTriList);
        const uint64_t vbytes = uint64_t(v.size());
        CHECK(b.list_offset[0] == b.upload_offset[1] + vbytes && b.list_offset[1] == b.list_offset[0] + ((uint64_t(b.list_count[0]) * 2 + 3) & ~3ull));
        for (unsigned e = 0; e < 2; ++e) {
            if (!b.own_list[e]) { CHECK(b.list_count[e] == 0); continue; }
            CHECK(b.list_count[e] > 0 && b.list_count[e] < idx.size() && b.list_count[e] % 3 == 0);
            CHECK(b.scissor[e][2] > b.scissor[e][0] && b.scissor[e][3] > b.scissor[e][1]);
            // every listed vertex is in front of the eye: its rewritten q is positive and finite (a culled vertex keeps the game's copy, so check
            // against an independent cull: the listed triangles are exactly the whole quads whose 4 vertices project)
            for (uint32_t i = 0; i < b.list_count[e]; ++i) {
                const uint16_t x = lists.l[e][i];
                CHECK(x < v.size() / 64);
                CHECK(std::isfinite(vx(eyes, e, x, 0)) && vx(eyes, e, x, 3) > 0.0f);
            }
            // the list keeps the game's triangle order and winding (a prefix or suffix of whole quads of the game's list)
            const uint16_t first = lists.l[e][0];
            const size_t at = size_t(first / 4) * 6;
            for (uint32_t i = 0; i < b.list_count[e]; ++i) CHECK(lists.l[e][i] == idx[at + i]);
        }
        p.dbr3d_commit(b);
        p.on_present(110); execute(p, &Qu, &L); p.on_reset(&L, true);
        const db::LiveStats s = p.dbr3d_stats();
        CHECK(s.published == 1 && s.clipped >= 1 && s.culled == 0);
        char line[512]; CHECK(p.format_dbr3d(line, sizeof line) > 0 && std::strstr(line, ";culled=0;clipped=") != nullptr);
    }
    CHECK(found >= 1);   // the fixture reaches the finding's case
    // the head turned all the way round: every vertex behind both eyes: nothing to list, the eyes are culled as before
    {
        Producer p; fresh(p); int L = 0, Qu = 0; arm(p, &L, &Qu); Eyes eyes; Lists lists;
        const DbrPlan c = p.dbr3d_draw(mk(&L, 100), turned_pose(180.0), done(1), eyes.p, lists.p);
        CHECK(!c.eye_draw[0] && !c.eye_draw[1] && !c.own_list[0] && !c.own_list[1]);
    }
    // an unknown IA topology: no list (the stage-2 whole-draw cull stays)
    {
        Producer p; fresh(p); int L = 0, Qu = 0; arm(p, &L, &Qu); Eyes eyes; Lists lists;
        DbrFacts d = mk(&L, 100); d.ia_topology = 0;
        const DbrPlan c = p.dbr3d_draw(d, turned_pose(100.0), done(1), eyes.p, lists.p);
        CHECK(c.record && !c.own_list[0] && !c.own_list[1] && (!c.eye_draw[0] || !c.eye_draw[1]));
    }
}

void test_cull_primitives() {
    uint8_t cull[8] = {0, 0, 1, 0, 0, 0, 0, 0};   // vertex 2 culled
    uint16_t out[64];
    // triangle strip 0..5: triangles (0,1,2) (2,1,3) (2,3,4) (4,3,5): the three using vertex 2 dropped, (4,3,5) kept in strip winding
    db::EyeList L = db::cull_primitives(false, nullptr, 0, 0, 6, 0, 6, db::kTopoTriStrip, cull, out, 64);
    CHECK(!L.why && L.kept == 1 && L.dropped == 3 && L.count == 3 && out[0] == 4 && out[1] == 3 && out[2] == 5);
    // indexed R16 strip with a cut (0xFFFF): assembly restarts, imin rebases
    const uint16_t ib[] = {10, 11, 13, 0xFFFF, 13, 14, 15, 16};
    uint8_t c2[8] = {};
    L = db::cull_primitives(true, reinterpret_cast<const uint8_t*>(ib), 2, db::kStripCut16, 8, 10, 7, db::kTopoTriStrip, c2, out, 64);
    CHECK(!L.why && L.kept == 3 && L.count == 9);
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 3 && out[3] == 3 && out[4] == 4 && out[5] == 5 && out[6] == 5 && out[7] == 4 && out[8] == 6);
    // without the cut value 0xFFFF is an index outside the copy: malformed
    CHECK(streq(db::cull_primitives(true, reinterpret_cast<const uint8_t*>(ib), 2, db::kStripCutDisabled, 8, 10, 7, db::kTopoTriStrip, c2, out, 64).why, "dbr3d-index-outside-copy"));
    // line strip -> line list; points; unknown topology; the cap
    L = db::cull_primitives(false, nullptr, 0, 0, 4, 0, 4, db::kTopoLineStrip, cull, out, 64);
    CHECK(!L.why && L.kept == 1 && L.dropped == 2 && out[0] == 0 && out[1] == 1 && db::list_topology(db::kTopoLineStrip) == db::kTopoLineList);
    L = db::cull_primitives(false, nullptr, 0, 0, 4, 0, 4, db::kTopoPointList, cull, out, 64);
    CHECK(!L.why && L.count == 3 && L.dropped == 1);
    CHECK(streq(db::cull_primitives(false, nullptr, 0, 0, 4, 0, 4, 33, cull, out, 64).why, "dbr3d-topology-unknown"));
    CHECK(streq(db::cull_primitives(false, nullptr, 0, 0, 6, 0, 6, db::kTopoTriList, c2, out, 3).why, "dbr3d-list-cap"));
}

// ---- review 2026-10-31 F4: an indexed draw whose index range has a GAP (an unreferenced vertex with q = 0) refused the whole debrief frame
void test_index_gaps() {
    std::vector<uint8_t> v = verts({{1800, 1900, 1810, 1910, 4000.0f}, {1820, 1900, 1830, 1910, 4000.0f}, {1840, 1900, 1850, 1910, 4000.0f}});   // 12 vertices
    for (int i = 4; i < 8; ++i) std::memset(v.data() + 64 * i, 0, 64);   // quad 1 is a gap: zero vertices (q = 0), never referenced
    std::vector<uint16_t> idx = {0, 1, 2, 2, 1, 3, 8, 9, 10, 10, 9, 11};
    auto mk = [&](const void* L, int64_t now, const std::vector<uint8_t>& vv) {
        DbrFacts d = facts(L, now, kGs, vv, true, 0);
        d.index_count = uint32_t(idx.size()); d.start_index = 0; d.ia_topology = db::kTopoTriList;
        d.ib = reinterpret_cast<const uint8_t*>(idx.data()); d.ib_stride = 2; d.strip_cut = db::kStripCutDisabled;
        return d;
    };
    // the finding: the copy as read refuses the frame
    {
        Producer p; fresh(p); int L = 0, Qu = 0; arm(p, &L, &Qu); Eyes eyes;
        CHECK(streq(step(p, mk(&L, 100, v), eyes, 1).why, "ui3d-vertex-q"));
    }
    // the fix: the glue fills the gaps from the nearest referenced vertex before them, then the draw is replayed
    std::vector<uint8_t> f = v;
    CHECK(db::fill_unreferenced(f.data(), 12, reinterpret_cast<const uint8_t*>(idx.data()), uint32_t(idx.size()), 2, db::kStripCutDisabled, 0) == 4);
    for (int i = 4; i < 8; ++i) CHECK(std::memcmp(f.data() + 64 * i, v.data() + 64 * 3, 64) == 0);   // vertex 3 (the last referenced before the gap)
    for (int i : {0, 1, 2, 3, 8, 9, 10, 11}) CHECK(std::memcmp(f.data() + 64 * i, v.data() + 64 * i, 64) == 0);   // referenced vertices untouched
    Producer p; fresh(p); int L = 0, Qu = 0; arm(p, &L, &Qu); Eyes eyes;
    DbrFacts d = mk(&L, 100, f); d.gaps = 4;
    const DbrPlan a = step(p, d, eyes, 1);
    CHECK(a.record && a.eye_draw[0] && a.eye_draw[1] && a.nv == 12);
    CHECK(p.dbr3d_stats().gaps == 4);
    // cuts are never references; a range whose first vertex is not referenced is left alone; a full range has nothing to fill
    const uint16_t cut[] = {0, 1, 0xFFFF, 3};
    std::vector<uint8_t> g = verts({{1800, 1900, 1810, 1910, 1.0f}});
    CHECK(db::fill_unreferenced(g.data(), 4, reinterpret_cast<const uint8_t*>(cut), 4, 2, db::kStripCut16, 0) == 1);
    CHECK(db::fill_unreferenced(g.data(), 4, reinterpret_cast<const uint8_t*>(cut), 4, 2, db::kStripCut16, 5) == 0);
    const uint16_t all[] = {0, 1, 2, 3};
    CHECK(db::fill_unreferenced(g.data(), 4, reinterpret_cast<const uint8_t*>(all), 4, 2, db::kStripCutDisabled, 0) == 0);
}

} // namespace

int main() {
    test_arming();
    test_debrief_frame();
    test_refusals_reject_the_frame();
    test_other_draws_and_lists();
    test_budget();
    test_no_pose_and_placement();
    test_wrap_head_turned();
    test_cull_primitives();
    test_index_gaps();
    std::printf("dbr3d_live_offline: %u checks passed\n", g_checks);
    return 0;
}
