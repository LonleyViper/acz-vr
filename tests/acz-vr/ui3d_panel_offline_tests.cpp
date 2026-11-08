// UI3D 2026-10-30d offline tests (docs/world-render/ui-3d.md section 13; headset test 6 items 1-2, step143): one surface per panel (u3pp),
// frame rims (u3lt) and the hangar GLOW MEMORY (u3agm). Pure parts (options, placement_layer, frame_quad, rewrite_rim, GlowMemory) and the
// PRODUCER with the fakes of ui3d_select_offline_tests.cpp. Nothing here touches D3D12, the game or a headset: fixture success says nothing
// about what the user will see.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui3d_hangar.h"
#include "brief3d_live.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::brief3d::live;
namespace u3 = acz::ui3d;
namespace hz = acz::ui3d::hangar;
using acz::vscreen::Completion;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
static unsigned long long field(const std::string& s, const char* key) {
    const size_t at = s.find(key);
    if (at == std::string::npos) return ~0ull;
    return std::strtoull(s.c_str() + at + std::strlen(key), nullptr, 10);
}

namespace {

// ================================================================================================== fakes (as ui3d_select_offline_tests.cpp)
// K10 aircraft select s1 (tick 127149386), A of the first ACM draw (ui-3d.md 8.5)
const char* kRealA = "808.52179,1085.18433,-142.494934,0.558786631,-443.357697,-909.892456,55.2049141,-0.21648325,1873.98059,1554.71289,-204.148453,0.800557733,258920.094,294018.25,16740630,144.465683";
void parse16(const char* s, float out[16]) {
    char* e = nullptr; const char* p = s;
    for (int i = 0; i < 16; ++i) { out[i] = std::strtof(p, &e); CHECK(e != p); p = e; if (*p == ',') ++p; }
}
// the K10 record; `shift` moves the game camera's eye along hangar-world x (row 3 of A: t' = t - shift (A row 0)): another camera position
void real_record(uint8_t rec[hz::kRecBytes], double shift = 0.0) {
    float A[16]; parse16(kRealA, A);
    if (shift != 0.0) for (int c = 0; c < 4; ++c) A[12 + c] = float(double(A[12 + c]) - shift * double(A[c]));
    for (unsigned i = 0; i < hz::kRecBytes; ++i) rec[i] = uint8_t(i * 7 + 3);
    std::memcpy(rec, A, 64);
    const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(rec + 256, I, 64);
    const float one = 1.0f; std::memcpy(rec + 360, &one, 4);
    std::memcpy(rec + 400, hz::kScreen, 16);
}
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
const uint64_t kUploadVa = 0x40000000ull;
int g_ui_rt = 0;
acz::brief3d::EyeView make_eye(double yaw, double ipd) {
    acz::brief3d::EyeView e;
    e.orientation[0] = 0; e.orientation[1] = std::sin(yaw / 2); e.orientation[2] = 0; e.orientation[3] = std::cos(yaw / 2);
    e.position[0] = ipd * std::cos(yaw); e.position[1] = 1.6; e.position[2] = -ipd * std::sin(yaw);
    e.fov[0] = -0.785; e.fov[1] = 0.785; e.fov[2] = 0.785; e.fov[3] = -0.785;
    return e;
}
PoseIn pose() { PoseIn p; p.ok = true; p.space_generation = 1; p.eyes[0] = make_eye(0.0, -0.032); p.eyes[1] = make_eye(0.0, 0.032); return p; }
void screen() {
    acz::vscreen::Options o; const float head[3] = {0.0f, 1.6f, 0.0f}, q[4] = {0, 0, 0, 1};
    acz::vscreen::publish_placement(acz::vscreen::place(o, head, q, 16.0f / 9.0f, 0), head, 1);
}
Producer& configure(Producer& p, const u3::Options& o) { p.configure_ui3d(o); p.gpu_ready(kUploadVa); screen(); return p; }
AcmFacts acm(const void* list, int64_t now, const uint8_t* rec) {
    AcmFacts f; f.list = list; f.now_ms = now; f.indexed = true; f.topology_type = 3; f.instances = 1;
    f.rt = &g_ui_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_write = true; f.depth_func = acz::brief3d::kCmpGreater; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.om_known = true;
    f.t10_why = nullptr; f.t10_param = 5; f.t10_va = 0x30be00000ull; std::memcpy(f.rec, rec, hz::kRecBytes);
    f.cb_why = nullptr; f.cb10_0 = 2;
    return f;
}
// a ps2_v quad on the UI target with the menu cb0 (682,-448,2048,2048), strip order (x0,y0) (x0,y1) (x1,y0) (x1,y1)
UiFacts ps2(const void* list, int64_t now, float x0, float y0, float x1, float y1, float z) {
    UiFacts f;
    f.list = list; f.now_ms = now; f.vs8 = u3::kPs2Vs8; f.vertices = 4; f.instances = 1; f.topology_type = 3;
    f.rt = &g_ui_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = acz::brief3d::kCmpAlways; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = u3::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f};
    f.vb_why = nullptr; f.stride = 64;
    const float xy[4][2] = {{x0, y0}, {x0, y1}, {x1, y0}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        float v[16] = {xy[i][0], xy[i][1], z, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f, float(i & 1), float(i >> 1), 0, 0, 0, 0, 0, 0};
        std::memcpy(f.vtx + 64 * i, v, 64);
    }
    return f;
}
struct Driver {
    Producer& p; int L = 0, Q = 0; uint64_t executed = 0;
    explicit Driver(Producer& pr) : p(pr) {}
    UiPlan ui(const UiFacts& f) {
        UiPlan pl = p.ui3d_draw(f, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(executed));
        if (pl.record) p.ui3d_commit(pl);
        return pl;
    }
    AcmPlan acm_draw(const AcmFacts& f) {
        AcmPlan pl = p.ui3d_acm(f, p.wants_poses(f.now_ms) ? pose() : PoseIn{}, done(executed));
        if (pl.record) p.ui3d_acm_commit(pl);
        return pl;
    }
    unsigned end() {
        const void* ls[1] = {&L};
        const auto tok = p.before_execute(&Q, ls, 1);
        if (tok.signal) { p.after_execute(tok, true); executed = tok.value; }
        p.on_reset(&L, true);
        Held h; unsigned replays = 0;
        if (p.acquire(0, done(executed), h)) { replays = unsigned(h.meta.replays); p.release(h); }
        return replays;
    }
};
float fl(const uint8_t* v, unsigned vertex, unsigned k) { float f; std::memcpy(&f, v + 64 * vertex + 4 * k, 4); return f; }
u3::EyeView eye_of(const PoseIn& ps, unsigned e) { return ui_eye(ps.eyes[e]); }
// the menu layer options and the u3s-scaled screen the producer uses for menu / hangar-overlay frames
u3::Options menu_lopts(const u3::Options& o) { u3::Options lo = o; lo.near_m = o.menu_near_m; lo.far_m = o.menu_far_m; return lo; }
u3::Screen menu_screen(const u3::Options& o) {
    const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();
    CHECK(sp.valid);
    return u3::scaled(u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor), double(o.scale));
}
const u3::SourceRect kFull = u3::rect_from_viewport(0, 0, 2560, 1440, 2560, 1440);

// ================================================================================================== options
void test_options() {
    std::string rest, why; u3::Options o;
    const std::string day = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000";
    CHECK(u3::extract_options(day, rest, o, why) && rest == day && o.glow_memory == 1 && !o.panel_plane && o.frame_mm == 0.0f);
    CHECK(u3::extract_options(day + ";ui3d:1;u3a:1;u3pp:1;u3lt:6;u3agm:2", rest, o, why) && rest == day);
    CHECK(o.on && o.hangar == 1 && o.panel_plane && std::fabs(o.frame_mm - 6.0f) < 1e-6f && o.glow_memory == 2);
    CHECK(has(u3::describe(o), ";u3pp=1;u3lt=6.0;u3agm=2"));
    CHECK(!u3::extract_options("auto;u3pp:2", rest, o, why) && has(why, "u3pp"));
    CHECK(!u3::extract_options("auto;u3lt:60", rest, o, why) && has(why, "u3lt"));
    CHECK(!u3::extract_options("auto;u3lt:-1", rest, o, why) && has(why, "u3lt"));
    CHECK(!u3::extract_options("auto;u3agm:3", rest, o, why) && has(why, "u3agm"));
    // the default line is byte-identical to before (step143's ui3d-option line); u3agm is shown only with u3a and a non-default value
    u3::Options on; on.on = true;
    CHECK(u3::describe(on) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3mn=1.10;u3mf=6.00;u3s=0.70");
    on.hangar = 1;
    CHECK(u3::describe(on) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3a=1;u3au=5.0;u3mn=1.10;u3mf=6.00;u3s=0.70");
    on.glow_memory = 0; CHECK(has(u3::describe(on), ";u3agm=0") && !has(u3::describe(on), "u3pp"));
    on.hangar = 0; CHECK(!has(u3::describe(on), "u3agm"));
    on.frame_mm = 4.5f; CHECK(has(u3::describe(on), ";u3pp=0;u3lt=4.5"));
    // the step143 user cfg (xr_terrain_stage) + this change's tokens fit the 255-character option buffer
    const std::string user = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1;fxtrail:8";
    CHECK(std::string(user + ";u3pp:1;u3lt:6").size() < 255);
}

// ================================================================================================== pure: one surface per panel, frame quads, rims
void test_placement_and_frame_rule() {
    u3::Options o;
    for (unsigned k = 0; k < u3::kLayers; ++k) CHECK(u3::placement_layer(o, k) == k);   // u3pp:0 = the classifier layer (old)
    o.panel_plane = true;
    CHECK(u3::placement_layer(o, 0) == 0 && u3::placement_layer(o, 1) == 2 && u3::placement_layer(o, 2) == 2);   // backdrop keeps its depth
    // the census sizes (step124 runK10 hangar overlay, ui-3d.md 13.1): panels / frames / tiles z 64 are frame quads; glyphs (z 1, also the
    // 0.018 x 0.071 tall ones that classify as medium), the full-screen tint (large), small decorations and other vertex counts are not
    CHECK(u3::frame_quad(4, 0.375, 0.286, true, 64.0f));    // [STATISTICS] panel
    CHECK(u3::frame_quad(4, 0.199, 0.214, true, 64.0f));    // [WEAPONS] panel
    CHECK(u3::frame_quad(4, 0.188, 0.071, true, 64.0f));    // PLAYER box
    CHECK(u3::frame_quad(4, 0.047, 0.071, true, 64.0f));    // colour-column / button tiles
    CHECK(u3::frame_quad(4, 0.023, 0.286, true, 64.0f));    // a strip
    CHECK(!u3::frame_quad(4, 0.018, 0.071, true, 1.0f));    // a tall glyph (z 1)
    CHECK(!u3::frame_quad(4, 0.012, 0.054, true, 64.0f));   // small
    CHECK(!u3::frame_quad(4, 0.751, 1.0, true, 64.0f));     // the full-screen tint
    CHECK(!u3::frame_quad(4, 0.375, 0.286, false, 64.0f));  // z unknown
    CHECK(!u3::frame_quad(6, 0.375, 0.286, true, 64.0f));   // not one quad
    CHECK(!u3::frame_quad(4, 0.375, 0.0, true, 64.0f));     // degenerate
}
void test_rim_and_coplanar() {
    u3::Options o; o.panel_plane = true; o.frame_mm = 6.0f;
    const double anchor[3] = {0.0, 1.6, 0.0};
    const float c[3] = {0.0f, 1.6f, -2.5f};
    const u3::Screen s = u3::make_screen(c, 0.0f, 3.2f, 1.8f, anchor);
    CHECK(s.valid);
    const u3::Cb0 cb{682.0f, -448.0f, 2048.0f, 2048.0f};
    UiFacts panel = ps2(nullptr, 0, 1780, 2065, 2036, 2193, 64);   // the K10 [STATISTICS] panel (0.375 x 0.286)
    UiFacts glyph = ps2(nullptr, 0, 1780, 2065, 1790, 2081, 1);    // a glyph whose top-left corner is the panel's top-left corner
    u3::EyeView moved; moved.p[0] = 0.25; moved.p[1] = 1.75; moved.p[2] = 0.15;   // a head 0.3 m off the anchor, turned 20 degrees
    moved.q[1] = std::sin(0.1745); moved.q[3] = std::cos(0.1745);
    u3::EyeView at_anchor; at_anchor.p[0] = anchor[0]; at_anchor.p[1] = anchor[1]; at_anchor.p[2] = anchor[2];
    // coplanar: with u3pp:1 the panel (classifier layer 1) and the glyph (layer 2) share one surface, so the shared corner lands on the same
    // eye pixel from ANY head position; with u3pp:0 (the step143 layers 1.86 m / 1.1 m) it separates as soon as the head leaves the anchor
    for (int pp = 0; pp < 2; ++pp) {
        u3::Options lo = o; lo.panel_plane = pp == 1; lo.near_m = 1.1f; lo.far_m = 6.0f;
        uint8_t a[4 * 64], b[4 * 64];
        CHECK(u3::rewrite_vertices(panel.vtx, 4, cb, kFull, s, lo, u3::placement_layer(lo, 1), moved, a).why == nullptr);
        CHECK(u3::rewrite_vertices(glyph.vtx, 4, cb, kFull, s, lo, u3::placement_layer(lo, 2), moved, b).why == nullptr);
        const double dx = std::fabs(double(fl(a, 0, 0)) - double(fl(b, 0, 0))), dy = std::fabs(double(fl(a, 0, 1)) - double(fl(b, 0, 1)));
        if (pp == 1) CHECK(dx < 1e-3 && dy < 1e-3);
        else CHECK(dx > 1.0 || dy > 1.0);   // GS units (1/341 of the half image): the step143 offset
        // from the anchor both agree either way (the anchor-alignment invariant)
        CHECK(u3::rewrite_vertices(panel.vtx, 4, cb, kFull, s, lo, u3::placement_layer(lo, 1), at_anchor, a).why == nullptr);
        CHECK(u3::rewrite_vertices(glyph.vtx, 4, cb, kFull, s, lo, u3::placement_layer(lo, 2), at_anchor, b).why == nullptr);
        CHECK(std::fabs(fl(a, 0, 0) - fl(b, 0, 0)) < 1e-2 && std::fabs(fl(a, 0, 1) - fl(b, 0, 1)) < 1e-2);
    }
    // the rim = the slab without its back face: walls 1..4 then the front face; the front face lies on the panel surface (= the text plane)
    u3::Options lo = o; lo.near_m = 1.1f; lo.far_m = 6.0f;
    const unsigned place = u3::placement_layer(lo, 1);
    CHECK(std::fabs(u3::rim_front(lo, place) - 1.1) < 1e-6 && std::fabs(u3::rim_back(lo, place) - 1.106) < 1e-6);
    uint8_t rim[u3::kRimCopies * 4 * 64], slab[u3::kSlabCopies * 4 * 64], flat[4 * 64];
    CHECK(u3::rewrite_rim(panel.vtx, 4, cb, kFull, s, lo, u3::rim_front(lo, place), u3::rim_back(lo, place), moved, rim).why == nullptr);
    CHECK(u3::rewrite_slab(panel.vtx, 4, cb, kFull, s, lo, u3::rim_front(lo, place), u3::rim_back(lo, place), moved, slab).why == nullptr);
    CHECK(std::memcmp(rim, slab + 4 * 64, sizeof rim) == 0);
    CHECK(u3::rewrite_vertices(panel.vtx, 4, cb, kFull, s, lo, place, moved, flat).why == nullptr);
    CHECK(std::memcmp(rim + 4 * 4 * 64, flat, sizeof flat) == 0);   // the front face IS the coplanar panel
    // each wall joins a front vertex to the same vertex 6 mm behind: the pairs differ in position (thickness visible off-axis), not in attributes
    for (unsigned w = 0; w < 4; ++w) {
        const uint8_t* q = rim + w * 4 * 64;
        CHECK(std::memcmp(q + 16, q + 64 + 16, 48) == 0 && std::memcmp(q + 128 + 16, q + 192 + 16, 48) == 0);
        CHECK(std::fabs(fl(q, 0, 0) - fl(q, 1, 0)) + std::fabs(fl(q, 0, 1) - fl(q, 1, 1)) > 1e-4);
    }
    CHECK(u3::rewrite_rim(glyph.vtx, 3, cb, kFull, s, lo, 1.1, 1.106, moved, rim).why != nullptr);   // not a quad: refused (the producer never asks)
}

// ================================================================================================== producer: a menu frame with u3pp / u3lt
struct Frame { UiPlan backdrop, tile, glyph; UiFacts tile_f, glyph_f; };
Frame menu_frame(Driver& d, int64_t t) {
    Frame m;
    d.p.on_present(t);
    m.backdrop = d.ui(ps2(&d.L, t + 1, 1792, 1824, 2304, 2272, 64));
    m.tile_f = ps2(&d.L, t + 1, 1809, 1842, 1841, 1874, 64);   // 0.047 x 0.071: a medium tile (z 64)
    m.tile = d.ui(m.tile_f);
    m.glyph_f = ps2(&d.L, t + 1, 1824, 1902, 1835, 1926, 1);   // a glyph (z 1)
    m.glyph = d.ui(m.glyph_f);
    return m;
}
void test_menu_panel_plane_and_rims() {
    for (int variant = 0; variant < 3; ++variant) {   // 0 old (u3pp:0, u3lt:0), 1 u3pp:1, 2 u3pp:1 + u3lt:6
        Producer p; u3::Options o; o.on = true; o.panel_plane = variant >= 1; o.frame_mm = variant == 2 ? 6.0f : 0.0f; configure(p, o);
        Driver d(p);
        const Frame f = menu_frame(d, 1000);
        CHECK(f.backdrop.record && f.backdrop.layer == 0 && f.tile.record && f.tile.layer == 1 && f.glyph.record && f.glyph.layer == 2);   // classes unchanged
        const u3::Options lo = menu_lopts(o);
        const u3::Screen scr = menu_screen(o);
        const PoseIn ps = pose();
        for (unsigned e = 0; e < 2; ++e) {
            uint8_t ex[u3::kRimCopies * 4 * 64] = {};
            // the glyph stays on the text layer in every variant
            CHECK(u3::rewrite_vertices(f.glyph_f.vtx, 4, f.glyph_f.cb0, kFull, scr, lo, 2, eye_of(ps, e), ex).why == nullptr);
            CHECK(std::memcmp(ex, f.glyph.vtx[e], 4 * 64) == 0 && f.glyph.copies == 1);
            if (variant == 0) {
                CHECK(f.tile.copies == 1);
                CHECK(u3::rewrite_vertices(f.tile_f.vtx, 4, f.tile_f.cb0, kFull, scr, lo, 1, eye_of(ps, e), ex).why == nullptr);
                CHECK(std::memcmp(ex, f.tile.vtx[e], 4 * 64) == 0);
            } else if (variant == 1) {   // the tile on the text plane
                CHECK(f.tile.copies == 1);
                CHECK(u3::rewrite_vertices(f.tile_f.vtx, 4, f.tile_f.cb0, kFull, scr, lo, 2, eye_of(ps, e), ex).why == nullptr);
                CHECK(std::memcmp(ex, f.tile.vtx[e], 4 * 64) == 0);
            } else {                     // the tile as a rim on the text plane
                CHECK(f.tile.copies == u3::kRimCopies && f.tile.vertices == 4);
                CHECK(u3::rewrite_rim(f.tile_f.vtx, 4, f.tile_f.cb0, kFull, scr, lo, u3::rim_front(lo, 2), u3::rim_back(lo, 2), eye_of(ps, e), ex).why == nullptr);
                CHECK(std::memcmp(ex, f.tile.vtx[e], sizeof ex) == 0);
                CHECK(f.tile.upload_offset[1] == f.tile.upload_offset[0] + u3::kRimCopies * 4 * 64);
                CHECK(f.tile.scissor[e][2] > f.tile.scissor[e][0] && f.tile.scissor[e][3] > f.tile.scissor[e][1]);
            }
            CHECK(f.backdrop.copies == 1);   // large: never a rim, the backdrop keeps its far layer
        }
        p.on_present(1016);
        CHECK(d.end() == 3);
        const std::string s = p.ui3d_summary();
        CHECK(has(s, ";published=1;") && has(s, ";rejected=0;"));
        if (variant == 2) CHECK(has(s, ";rim=1;rim-flat=0")); else CHECK(!has(s, ";rim="));
    }
}
// The real overlay of the K10 PLAYER weapon screen (runK10 ticks 127156569-570, census vertices; ui3d_select_offline test_k10) in a hangar
// frame with u3pp:1;u3lt:6: the three full-screen draws dropped (u3at:0), every panel / box / tile a rim whose FRONT FACE is the panel on the
// text plane, every glyph flat on the same plane: one surface for the whole panel group.
struct RealDraw { int all; float v[4][3]; };
const RealDraw kK10[] = {
    {155, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},   // 0.751 x 1.0 (tint / grid / frame): dropped
    {158, {{1780, 2065, 64}, {1780, 2193, 64}, {2036, 2065, 64}, {2036, 2193, 64}}},   // [STATISTICS] panel
    {159, {{1806, 1858, 64}, {1806, 1986, 64}, {2062, 1858, 64}, {2062, 1986, 64}}},   // [SP WEAPON NAME] panel
    {160, {{2152, 2092, 64}, {2152, 2188, 64}, {2288, 2092, 64}, {2288, 2188, 64}}},   // [WEAPONS] panel
    {161, {{2168, 1878, 64}, {2168, 1910, 64}, {2296, 1878, 64}, {2296, 1910, 64}}},   // PLAYER box
    {162, {{1809, 1834, 64}, {1809, 1866, 64}, {1841, 1834, 64}, {1841, 1866, 64}}},   // button tile
    {165, {{1820, 1889, 1}, {1826, 1889, 1}, {1820, 1905, 1}, {1826, 1905, 1}}},       // glyphs
    {166, {{1826, 1889, 1}, {1832, 1889, 1}, {1826, 1905, 1}, {1832, 1905, 1}}},
    {181, {{1820, 1901, 1}, {1832, 1901, 1}, {1820, 1933, 1}, {1832, 1933, 1}}},       // a tall glyph (medium by size, z 1)
};
UiFacts real_draw(const void* list, int64_t now, const RealDraw& r) {
    UiFacts f = ps2(list, now, 0, 0, 1, 1, 0);
    for (int i = 0; i < 4; ++i) {
        const float xyzq[4] = {r.v[i][0], r.v[i][1], r.v[i][2], 1.0f};
        std::memcpy(f.vtx + 64 * i, xyzq, 16);
    }
    return f;
}
void test_k10_hangar_overlay_one_surface() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    Producer p; u3::Options o; o.on = true; o.hangar = 1; o.panel_plane = true; o.frame_mm = 6.0f; configure(p, o);
    Driver d(p);
    p.on_present(1000);
    CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);
    const u3::Options lo = menu_lopts(o);
    const u3::Screen scr = menu_screen(o);
    const PoseIn ps = pose();
    unsigned rims = 0, glyphs = 0;
    for (const RealDraw& rd : kK10) {
        const UiFacts f = real_draw(&d.L, 1001, rd);
        const UiPlan pl = d.ui(f);
        if (rd.all == 155) { CHECK(!pl.record && streq(pl.why, "u3a-tint-dropped")); continue; }
        CHECK(pl.record && pl.eye_draw[0] && pl.eye_draw[1]);
        for (unsigned e = 0; e < 2; ++e) {
            uint8_t flat[4 * 64];
            CHECK(u3::rewrite_vertices(f.vtx, 4, f.cb0, kFull, scr, lo, 2, eye_of(ps, e), flat).why == nullptr);   // on the text plane
            if (rd.v[0][2] == 1.0f) CHECK(pl.copies == 1 && std::memcmp(pl.vtx[e], flat, sizeof flat) == 0);
            else CHECK(pl.copies == u3::kRimCopies && std::memcmp(pl.vtx[e] + 4 * 4 * 64, flat, sizeof flat) == 0);   // the rim's front face
        }
        if (rd.v[0][2] == 1.0f) ++glyphs; else ++rims;
    }
    CHECK(rims == 5 && glyphs == 3);
    p.on_present(1016);
    d.end();
    const std::string s = p.ui3d_summary();
    CHECK(has(s, ";u3a-rejected=0;") && has(s, ";u3a-published=1;") && has(s, ";rim=5;rim-flat=0"));
}

// ================================================================================================== the hangar glow memory (u3agm)
// A light is a 6 x 6 unit hangar-world quad (strip order x0y0, x1y0, x0y1, x1y1) `ahead` units in front of the K10 camera, offset (ox, oy)
// along its right / up axes; its glow draw is the GS quad the game's A gives (as ui3d_select_offline test_hangar_glow_reprojected).
struct Light { double P[4][3]; };
Light light(const hz::Camera& cam, double ahead, double ox, double oy) {
    Light L;
    for (int i = 0; i < 4; ++i) {
        const double dx = (i & 1) ? 3.0 : -3.0, dy = (i & 2) ? -3.0 : 3.0;
        for (int k = 0; k < 3; ++k) L.P[i][k] = cam.eye[k] + ahead * cam.fwd[k] + (ox + dx) * cam.right[k] + (oy + dy) * cam.up[k];
    }
    return L;
}
UiFacts glow(const void* list, int64_t now, const uint8_t* rec, const Light& L, uint64_t key) {
    float A[16]; std::memcpy(A, rec, 64);
    UiFacts g = ps2(list, now, 0, 0, 1, 1, 0);
    for (int i = 0; i < 4; ++i) {
        double nx, ny, w, depth; CHECK(hz::shader_ndc(A, hz::kScreen, L.P[i], nx, ny, w, depth));
        const float v[4] = {float(2048.0 + nx * 341.0), float(2048.0 - ny * 224.0), float(depth * 16777216.0), 1.0f};
        std::memcpy(g.vtx + 64 * i, v, 16);
        CHECK(v[2] > 256.0f);
    }
    g.state_key = key;
    return g;
}
// The eye image GS position of a hangar-world point through the latched room (the eye matrix the ACM replays use): the oracle.
void expect_at(const UiPlan& pl, unsigned copy, const Light& L, const hz::Camera& room_cam) {
    const PoseIn ps = pose();
    double hp[3]; for (int i = 0; i < 3; ++i) hp[i] = 0.5 * (ps.eyes[0].position[i] + ps.eyes[1].position[i]);
    hz::Room room; CHECK(hz::make_room(room_cam, hp, ps.eyes[0].orientation, 5.0, room) == nullptr);
    for (unsigned e = 0; e < 2; ++e) {
        hz::Eye ev; for (int i = 0; i < 3; ++i) ev.p[i] = ps.eyes[e].position[i];
        for (int i = 0; i < 4; ++i) { ev.q[i] = ps.eyes[e].orientation[i]; ev.fov[i] = ps.eyes[e].fov[i]; }
        float Ae[16]; CHECK(hz::eye_matrix(room, room_cam, hz::kScreen, ev, Ae) == nullptr);
        const uint8_t* q = pl.vtx[e] + copy * 4 * 64;
        for (unsigned i = 0; i < 4; ++i) {
            double nx, ny, we, depth; CHECK(hz::shader_ndc(Ae, hz::kScreen, L.P[i], nx, ny, we, depth));
            const double x = 2048.0 + nx * 341.0, y = 2048.0 - ny * 224.0;
            if (!(std::fabs(fl(q, i, 0) - x) < 0.1 && std::fabs(fl(q, i, 1) - y) < 0.1))
                std::printf("copy %u e%u v%u got %.4f %.4f want %.4f %.4f\n", copy, e, i, fl(q, i, 0), fl(q, i, 1), x, y);
            CHECK(std::fabs(fl(q, i, 0) - x) < 0.1 && std::fabs(fl(q, i, 1) - y) < 0.1);
        }
    }
}
const uint64_t kKeyA = 0x1111, kKeyB = 0x2222;

// The step143 failure: the game emits L2 in frame 1 and not in frame 2 (its camera no longer sees it). With the memory, frame 2's L1 draw
// carries L2 as a second copy at L2's room position; without (u3agm:0) L2 is gone (the lamp "turns off").
void test_glow_memory_keeps_lights_on() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    hz::Camera cam; CHECK(hz::decompose_record(rec, cam) == nullptr);
    const Light L1 = light(cam, 150.0, 12.0, 20.0), L2 = light(cam, 160.0, -30.0, 25.0);
    for (unsigned mem : {1u, 0u}) {
        Producer p; u3::Options o; o.on = true; o.hangar = 1; o.glow_memory = mem; configure(p, o);
        Driver d(p);
        p.on_present(1000);                                            // frame 1: both lights
        CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);
        const UiPlan a1 = d.ui(glow(&d.L, 1001, rec, L1, kKeyA));
        const UiPlan a2 = d.ui(glow(&d.L, 1001, rec, L2, kKeyA));
        CHECK(a1.record && a1.copies == 1 && a2.record && a2.copies == 1);   // first seen: exactly the memory-less plan
        expect_at(a1, 0, L1, cam); expect_at(a2, 0, L2, cam);
        d.end();
        p.on_present(1016);                                            // frame 2: the game emits L1 only
        CHECK(d.acm_draw(acm(&d.L, 1017, rec)).record);
        const UiPlan b1 = d.ui(glow(&d.L, 1017, rec, L1, kKeyA));
        CHECK(b1.record && b1.vertices == 4 && b1.eye_draw[0] && b1.eye_draw[1]);
        if (mem) {
            CHECK(b1.copies == 2);
            expect_at(b1, 0, L1, cam); expect_at(b1, 1, L2, cam);         // L2 stays lit where the lamp is
            CHECK(b1.upload_offset[1] == b1.upload_offset[0] + 2 * 4 * 64);
            CHECK(std::memcmp(b1.vtx[0] + 4 * 64 + 16, a2.vtx[0] + 16, 48) == 0);   // L2's own colours / texcoords
        } else CHECK(b1.copies == 1);
        d.end();
        p.on_present(1032);                                            // frame 3: both again; L2's real draw refreshes it, no double
        CHECK(d.acm_draw(acm(&d.L, 1033, rec)).record);
        const UiPlan c1 = d.ui(glow(&d.L, 1033, rec, L1, kKeyA));
        const UiPlan c2 = d.ui(glow(&d.L, 1033, rec, L2, kKeyA));
        if (mem) {
            CHECK(c1.record && c1.copies == 2);                          // L2 drawn from memory at L1 (the earliest place it may go)...
            CHECK(!c2.record && streq(c2.why, "u3a-glow-remembered"));    // ...so its real draw adds nothing (never twice as bright)
        } else CHECK(c1.copies == 1 && c2.record && c2.copies == 1);
        d.end();
        p.on_present(1048);
        const std::string s = p.ui3d_summary();
        CHECK(has(s, ";u3a-rejected=0;") && has(s, ";u3a-glow-why=none"));
        CHECK(field(s, ";u3a-glow=") == 5);                               // 5 real glows of the game, all re-projected
        if (mem) CHECK(field(s, ";u3a-glow-mem=") == 2 && field(s, ";u3a-glow-lights=") == 2 && field(s, ";u3a-glow-missed=") == 0);
        else CHECK(!has(s, "u3a-glow-mem"));
    }
}
// Painter's order: a remembered glow is never drawn earlier in the game's order than where the game last drew it (its ACM draws before it):
// frame 1 draws L1 after 1 ACM draw and L2, L3 after 2; frame 2 draws L1 (1 ACM) and L3 (2 ACM) only: L1 carries nothing, L3 carries L2.
// A light with another state key is never carried by another draw (it is missed and counted).
void test_glow_memory_order_and_keys() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    hz::Camera cam; CHECK(hz::decompose_record(rec, cam) == nullptr);
    const Light L1 = light(cam, 150.0, 12.0, 20.0), L2 = light(cam, 160.0, -30.0, 25.0), L3 = light(cam, 140.0, 40.0, 18.0), L4 = light(cam, 155.0, 0.0, 30.0);
    Producer p; u3::Options o; o.on = true; o.hangar = 1; configure(p, o);
    Driver d(p);
    p.on_present(1000);
    CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);
    CHECK(d.ui(glow(&d.L, 1001, rec, L1, kKeyA)).copies == 1);
    CHECK(d.ui(glow(&d.L, 1001, rec, L4, kKeyB)).copies == 1);           // another texture / PSO
    CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);
    CHECK(d.ui(glow(&d.L, 1001, rec, L2, kKeyA)).copies == 1);
    CHECK(d.ui(glow(&d.L, 1001, rec, L3, kKeyA)).copies == 1);
    d.end();
    p.on_present(1016);
    CHECK(d.acm_draw(acm(&d.L, 1017, rec)).record);
    const UiPlan b1 = d.ui(glow(&d.L, 1017, rec, L1, kKeyA));
    CHECK(b1.record && b1.copies == 1);                                   // L2 / L3 were drawn after the 2nd ACM draw: not yet; L4: other key
    CHECK(d.acm_draw(acm(&d.L, 1017, rec)).record);
    const UiPlan b3 = d.ui(glow(&d.L, 1017, rec, L3, kKeyA));
    CHECK(b3.record && b3.copies == 2);
    expect_at(b3, 0, L3, cam); expect_at(b3, 1, L2, cam);
    d.end();
    p.on_present(1032);
    CHECK(d.acm_draw(acm(&d.L, 1033, rec)).record);                      // frame 3 opens: frame 2 missed L4 (no key-B carrier)
    d.end();
    p.on_present(1048);
    const std::string s = p.ui3d_summary();
    CHECK(field(s, ";u3a-glow-missed=") == 1 && field(s, ";u3a-glow-mem=") == 1 && field(s, ";u3a-glow-lights=") == 4);
    {   // a draw without a state key (the glue could not resolve t0) never uses the memory: the old single-copy path
        Producer q; configure(q, o);
        Driver e(q);
        q.on_present(1000);
        CHECK(e.acm_draw(acm(&e.L, 1001, rec)).record);
        CHECK(e.ui(glow(&e.L, 1001, rec, L1, 0)).copies == 1 && e.ui(glow(&e.L, 1001, rec, L2, 0)).copies == 1);
        e.end();
        q.on_present(1016);
        CHECK(e.acm_draw(acm(&e.L, 1017, rec)).record);
        CHECK(e.ui(glow(&e.L, 1017, rec, L1, 0)).copies == 1);
        e.end();
        q.on_present(1032);
        CHECK(field(q.ui3d_summary(), ";u3a-glow-lights=") == 0);
    }
}
// A camera cut (another screen) clears the memory with u3agm:1 and keeps it with u3agm:2 (the remembered light re-projected through the new
// room); a gap (> 2 s without a hangar frame: a new visit) clears it in both modes.
void test_glow_memory_cut_and_gap() {
    uint8_t rec[hz::kRecBytes], rec2[hz::kRecBytes]; real_record(rec); real_record(rec2, 20.0);   // the eye moved 20 units: a cut
    hz::Camera cam, cam2; CHECK(hz::decompose_record(rec, cam) == nullptr && hz::decompose_record(rec2, cam2) == nullptr);
    CHECK(hz::cut(cam, cam2));
    const Light L1 = light(cam, 150.0, 12.0, 20.0), L2 = light(cam, 160.0, -30.0, 25.0);
    for (unsigned mode : {1u, 2u}) {
        Producer p; u3::Options o; o.on = true; o.hangar = 1; o.glow_memory = mode; configure(p, o);
        Driver d(p);
        p.on_present(1000);
        CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);
        CHECK(d.ui(glow(&d.L, 1001, rec, L1, kKeyA)).record && d.ui(glow(&d.L, 1001, rec, L2, kKeyA)).record);
        d.end();
        p.on_present(1016);                                            // the cut: the game's new camera sees L1 only (the same world quad)
        CHECK(d.acm_draw(acm(&d.L, 1017, rec2)).record);
        const UiPlan b = d.ui(glow(&d.L, 1017, rec2, L1, kKeyA));
        CHECK(b.record);
        if (mode == 1) CHECK(b.copies == 1);
        else { CHECK(b.copies == 2); expect_at(b, 0, L1, cam2); expect_at(b, 1, L2, cam2); }   // the room re-targeted at the cut
        d.end();
        p.on_present(5000);                                            // a gap: a new hangar visit
        CHECK(d.acm_draw(acm(&d.L, 5001, rec2)).record);
        CHECK(d.ui(glow(&d.L, 5001, rec2, L1, kKeyA)).copies == 1);
        d.end();
        p.on_present(5016);
        const std::string s = p.ui3d_summary();
        CHECK(has(s, ";u3a-rejected=0;") && field(s, ";u3a-cuts=") == 1 && field(s, ";u3a-latches=") == 2);
    }
}
// The pure GlowMemory: match tolerance / same-frame sprites / capacity / pending order / forget + compact.
void test_glow_memory_pure() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    hz::Camera cam; CHECK(hz::decompose_record(rec, cam) == nullptr);
    const Light L1 = light(cam, 150.0, 12.0, 20.0);
    const UiFacts g = glow(nullptr, 0, rec, L1, kKeyA);
    double c[3], r = 0;
    CHECK(hz::effect_centre(g.vtx, 4, g.cb0, cam, c, r) == nullptr);
    double want[3] = {0, 0, 0};
    for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) want[k] += 0.25 * L1.P[i][k];
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(c[k] - want[k]) < 0.05);
    CHECK(std::fabs(r - std::sqrt(18.0)) < 0.05);                     // a 6 x 6 quad: half diagonal
    UiFacts bad = g; const float q0 = 0.0f; std::memcpy(bad.vtx + 12, &q0, 4);
    CHECK(streq(hz::effect_centre(bad.vtx, 4, bad.cb0, cam, c, r), "u3a-glow-q"));
    hz::GlowMemory m;
    CHECK(hz::effect_centre(g.vtx, 4, g.cb0, cam, c, r) == nullptr);
    CHECK(m.match(kKeyA, c, r, 1) == -1);
    const int i0 = m.remember(-1, kKeyA, g.vtx, g.cb0, cam, c, r, 3, 1);
    CHECK(i0 == 0 && m.size() == 1);
    CHECK(m.match(kKeyA, c, r, 1) == -1);                              // seen in frame 1 already: a second sprite there is another entry
    CHECK(m.match(kKeyA, c, r, 2) == 0 && m.match(kKeyB, c, r, 2) == -1 && m.match(0, c, r, 2) == -1);
    double far[3] = {c[0] + 3.0, c[1], c[2]};                          // tolerance clamp(r / 2 = 2.1, 1, 8): 3 units is another light
    CHECK(m.match(kKeyA, far, r, 2) == -1);
    double nearby[3] = {c[0] + 1.5, c[1], c[2]};
    CHECK(m.match(kKeyA, nearby, r, 2) == 0);
    int out[hz::kGlowCarrierQuads];
    CHECK(m.pending(kKeyA, 2, 2, -1, hz::kGlowCarrierQuads, out) == 0);   // seen after 3 ACM draws: not at 2
    CHECK(m.pending(kKeyA, 3, 2, -1, hz::kGlowCarrierQuads, out) == 1 && out[0] == 0);
    CHECK(m.pending(kKeyA, 3, 2, 0, hz::kGlowCarrierQuads, out) == 0);    // skip
    m.at(0).drawn = 2; CHECK(m.pending(kKeyA, 9, 2, -1, hz::kGlowCarrierQuads, out) == 0 && m.missed(2) == 0 && m.missed(3) == 1);
    for (unsigned i = 1; i < hz::kGlowMemCap; ++i) CHECK(m.remember(-1, kKeyA, g.vtx, g.cb0, cam, c, r, 0, 1) == int(i));
    CHECK(m.remember(-1, kKeyA, g.vtx, g.cb0, cam, c, r, 0, 1) == -1 && m.size() == hz::kGlowMemCap);   // full: drawn, not remembered
    CHECK(m.pending(kKeyA, 0, 5, -1, 3, out) == 3);                     // capped by `max`
    m.forget(0); m.forget(5); CHECK(m.size() == hz::kGlowMemCap && m.missed(7) == hz::kGlowMemCap - 2);
    m.compact(); CHECK(m.size() == hz::kGlowMemCap - 2 && m.at(0).key == kKeyA);
    m.clear(); CHECK(m.size() == 0);
    uint8_t quad[4 * 64]; std::memcpy(quad, g.vtx, sizeof quad); hz::degenerate_quad(quad);
    for (unsigned i = 1; i < 4; ++i) CHECK(std::memcmp(quad, quad + 64 * i, 64) == 0);
}

} // namespace

int main() {
    test_options();
    test_placement_and_frame_rule();
    test_rim_and_coplanar();
    test_menu_panel_plane_and_rims();
    test_k10_hangar_overlay_one_surface();
    test_glow_memory_pure();
    test_glow_memory_keeps_lights_on();
    test_glow_memory_order_and_keys();
    test_glow_memory_cut_and_gap();
    std::printf("ui3d_panel_offline: %u checks passed\n", g_checks);
    return 0;
}
