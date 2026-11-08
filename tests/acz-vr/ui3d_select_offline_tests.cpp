// UI3D 2026-10-30c offline tests (docs/world-render/ui-3d.md section 12; headset test 5 items 1-2): the PRODUCER's selection slab on a menu
// frame (u3sel), the dropped hangar tint (u3at) and the re-projected hangar light glows (u3ag), with fakes as ui3d_hangar_offline_tests.cpp.
// The pure parts (options, SelectionRows, rewrite_slab, u3fs) are in ui3d_offline_tests.cpp. Nothing here touches D3D12, the game or a headset:
// fixture success says nothing about what the user will see.
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

namespace {

// ================================================================================================== fakes (as ui3d_hangar_offline_tests.cpp)
// K10 aircraft select s1 (tick 127149386), A of the first ACM draw (ui-3d.md 8.5)
const char* kRealA = "808.52179,1085.18433,-142.494934,0.558786631,-443.357697,-909.892456,55.2049141,-0.21648325,1873.98059,1554.71289,-204.148453,0.800557733,258920.094,294018.25,16740630,144.465683";
void parse16(const char* s, float out[16]) {
    char* e = nullptr; const char* p = s;
    for (int i = 0; i < 16; ++i) { out[i] = std::strtof(p, &e); CHECK(e != p); p = e; if (*p == ',') ++p; }
}
void real_record(uint8_t rec[hz::kRecBytes]) {
    float A[16]; parse16(kRealA, A);
    for (unsigned i = 0; i < hz::kRecBytes; ++i) rec[i] = uint8_t(i * 7 + 3);
    std::memcpy(rec, A, 64);
    const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(rec + 256, I, 64);
    const float one = 1.0f; std::memcpy(rec + 360, &one, 4);
    std::memcpy(rec + 400, hz::kScreen, 16);
}
Completion done(uint64_t v) { Completion c; c.value = v; return c; }
const uint64_t kUploadVa = 0x40000000ull;
int g_ui_rt = 0, g_swap = 0;
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
UiFacts ps2(const void* list, int64_t now, float x0, float y0, float x1, float y1, float z, bool indexed = false) {
    UiFacts f;
    f.list = list; f.now_ms = now; f.vs8 = u3::kPs2Vs8; f.vertices = indexed ? 6 : 4; f.instances = 1; f.topology_type = 3; f.indexed = indexed;
    f.rt = &g_ui_rt; f.rt_w = 2560; f.rt_h = 1440; f.rt_fmt = kRtvFormat;
    f.pso_known = true; f.rt_count = 1; f.rtv0 = kRtvFormat; f.dsv = kDsvFormat; f.samples = 1;
    f.depth_enable = true; f.depth_func = acz::brief3d::kCmpAlways; f.stencil = true; f.stencil_always = true;
    f.rs_known = true; f.viewports = 1; f.scissors = 1; f.vp[2] = 2560; f.vp[3] = 1440; f.sc[0] = 320; f.sc[2] = 2240; f.sc[3] = 1440;
    f.om_known = true; f.cb_why = nullptr; f.cb0 = u3::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f};
    f.vb_why = indexed ? "ui3d-indexed-draw" : nullptr; f.stride = 64;
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
    // the end of a game frame: the list executes and resets; the consumer takes the newest slot (replays of the frame returned in `replays`)
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

// ================================================================================================== the selection slab on a menu frame
// A main-menu-like frame (census runD1 s=10, ui-3d.md 12.1): backdrop, a panel tile, the selection box (0.293 x 0.143, drawn right before the
// glyphs, centred on the first text row), one row of 8 glyphs (z 1) and a second row. Frame 1 has no previous rows: the box stays on its layer.
// Frame 2 matches the box against frame 1's rows: it becomes the slab, every other draw is byte-identical to frame 1's replay.
struct MenuFrame { UiPlan backdrop, tile, box, glyph0; UiFacts box_facts; };
MenuFrame menu_frame(Driver& d, int64_t t) {
    MenuFrame m;
    d.p.on_present(t);
    m.backdrop = d.ui(ps2(&d.L, t + 1, 1792, 1824, 2304, 2272, 64));
    m.tile = d.ui(ps2(&d.L, t + 1, 1809, 1842, 1841, 1874, 64));
    m.box_facts = ps2(&d.L, t + 1, 1810, 1882, 2010, 1946, 64);      // 0.293 x 0.143 of the image, centred on y 1914
    m.box = d.ui(m.box_facts);
    for (int row = 0; row < 2; ++row)
        for (int g = 0; g < 8; ++g) {
            const float y = 1902.0f + 30.0f * float(row), x = 1824.0f + 12.0f * float(g);
            const UiPlan gp = d.ui(ps2(&d.L, t + 1, x, y, x + 11.0f, y + 24.0f, 1));   // glyph 0.016 x 0.054 (z 1)
            if (row == 0 && g == 0) m.glyph0 = gp;
        }
    return m;
}
void test_menu_selection_slab() {
    for (unsigned mode : {2u, 1u, 0u}) {
        Producer p; u3::Options o; o.on = true; o.select = mode; configure(p, o);
        Driver d(p);
        const MenuFrame f1 = menu_frame(d, 1000);
        CHECK(f1.backdrop.record && f1.backdrop.layer == 0 && f1.tile.record && f1.tile.layer == 1 && f1.glyph0.record && f1.glyph0.layer == 2);
        CHECK(f1.box.record && f1.box.layer == 1 && f1.box.copies == 1);   // no rows of a previous frame yet: the old placement
        CHECK(d.end() == 0);
        const MenuFrame f2 = menu_frame(d, 1016);   // its Present closes (publishes) frame 1
        // the other draws are replayed exactly as in frame 1 (lines and panels keep their depth: "offset the lines is fine")
        CHECK(std::memcmp(f1.tile.vtx[0], f2.tile.vtx[0], 4 * 64) == 0 && std::memcmp(f1.backdrop.vtx[1], f2.backdrop.vtx[1], 4 * 64) == 0);
        CHECK(std::memcmp(f1.glyph0.vtx[0], f2.glyph0.vtx[0], 4 * 64) == 0 && f2.glyph0.copies == 1);
        CHECK(f2.box.record && f2.box.layer == 1 && f2.box.eye_draw[0] && f2.box.eye_draw[1]);
        // expected: the menu layer options (u3mn / u3mf) on the u3s-scaled screen, the eyes of the pose
        const acz::vscreen::SharedPlacement sp = acz::vscreen::read_placement();
        CHECK(sp.valid);
        const u3::Screen scr = u3::scaled(u3::make_screen(sp.centre, sp.yaw, sp.width, sp.height, sp.anchor), 0.7);
        u3::Options lo = o; lo.near_m = o.menu_near_m; lo.far_m = o.menu_far_m;
        const u3::SourceRect r = u3::rect_from_viewport(0, 0, 2560, 1440, 2560, 1440);
        const PoseIn ps = pose();
        for (unsigned e = 0; e < 2; ++e) {
            uint8_t ex[u3::kSlabCopies * 4 * 64] = {};
            if (mode == 2) {
                CHECK(f2.box.copies == u3::kSlabCopies && f2.box.vertices == 4);
                CHECK(u3::rewrite_slab(f2.box_facts.vtx, 4, f2.box_facts.cb0, r, scr, lo, u3::selection_front(lo), u3::selection_back(lo), eye_of(ps, e), ex).why == nullptr);
                CHECK(std::memcmp(ex, f2.box.vtx[e], sizeof ex) == 0);
                CHECK(f2.box.upload_offset[1] == f2.box.upload_offset[0] + u3::kSlabCopies * 4 * 64);   // 6 faces per eye copy
            } else if (mode == 1) {
                CHECK(f2.box.copies == 1);
                CHECK(u3::rewrite_at(f2.box_facts.vtx, 4, f2.box_facts.cb0, r, scr, lo, u3::selection_front(lo), 1.0, eye_of(ps, e), ex).why == nullptr);
                CHECK(std::memcmp(ex, f2.box.vtx[e], 4 * 64) == 0);
            } else {
                CHECK(f2.box.copies == 1 && std::memcmp(f1.box.vtx[e], f2.box.vtx[e], 4 * 64) == 0);   // u3sel:0 = the old behaviour
            }
            if (mode != 0) {   // the box sits just behind the text: nearer than its classifier layer (the panel layer at 1.86 m)
                CHECK(u3::selection_front(lo) < u3::layer_distance(lo, 1) && u3::selection_front(lo) - u3::layer_distance(lo, 2) < 0.0041);
                CHECK(f2.box.scissor[e][2] > f2.box.scissor[e][0] && f2.box.scissor[e][3] > f2.box.scissor[e][1]);
            }
        }
        p.on_present(1032);
        d.end();
        const std::string s = p.ui3d_summary();
        CHECK(has(s, mode == 0 ? ";sel=0" : ";sel=1") && s.size() < 480);
    }
}

// ================================================================================================== the hangar tint (u3at) and the glows (u3ag)
void test_hangar_tint_dropped() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    for (bool keep : {false, true}) {
        Producer p; u3::Options o; o.on = true; o.hangar = 1; o.hangar_tint = keep; o.hangar_glow = false; configure(p, o);
        Driver d(p);
        p.on_present(1000);
        CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record && d.acm_draw(acm(&d.L, 1001, rec)).record);
        const UiPlan dim = d.ui(ps2(&d.L, 1001, 1728, 1824, 2368, 2272, 0));     // the full-screen dim (K10: 0.938 x 1.0, z 0)
        const UiPlan tint = d.ui(ps2(&d.L, 1001, 1792, 1824, 2304, 2272, 64));   // the 4:3 tint / grid / frame quad (0.751 x 1.0, z 64)
        const UiPlan box = d.ui(ps2(&d.L, 1001, 1808, 2051, 2024, 2187, 64));    // a panel (the STANDBY-like box)
        const UiPlan glyph = d.ui(ps2(&d.L, 1001, 2249, 1893, 2265, 1905, 1));   // a glyph
        if (!keep) {
            CHECK(!dim.record && streq(dim.why, "u3a-tint-dropped") && !tint.record && streq(tint.why, "u3a-tint-dropped"));
        } else {
            CHECK(dim.record && dim.layer == 0 && tint.record && tint.layer == 0);   // u3at:1 = the old far-layer rectangle
        }
        CHECK(box.record && box.layer == 1 && glyph.record && glyph.layer == 2);   // the selection UI and the text stay (same classes either way)
        p.on_present(1016);
        CHECK(d.end() == (keep ? 6u : 4u));
        const std::string s = p.ui3d_summary();
        CHECK(has(s, keep ? ";u3a-tint=0;" : ";u3a-tint=2;") && has(s, ";u3a-rejected=0;") && has(s, ";u3a-published=1;"));
    }
}

// The glow oracle: a hangar-world quad projected by the GAME's A gives the GS vertices the EE wrote; the replayed eye vertex must land where
// the EYE's own matrix A'_e (eye_matrix of the latched room: the matrix the room's ACM replay uses) puts the same world point.
void test_hangar_glow_reprojected() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    float A[16]; std::memcpy(A, rec, 64);
    hz::Camera cam; CHECK(hz::decompose_record(rec, cam) == nullptr);
    double P[4][3];
    for (int i = 0; i < 4; ++i) {   // a 6 x 6 unit light 150 units ahead, up and to the right (strip order as K10: x0y0, x1y0, x0y1, x1y1)
        const double dx = (i & 1) ? 3.0 : -3.0, dy = (i & 2) ? -3.0 : 3.0;
        for (int k = 0; k < 3; ++k) P[i][k] = cam.eye[k] + 150.0 * cam.fwd[k] + (12.0 + dx) * cam.right[k] + (20.0 + dy) * cam.up[k];
    }
    UiFacts g = ps2(nullptr, 0, 0, 0, 1, 1, 0);
    double wsrc[4];
    for (int i = 0; i < 4; ++i) {
        double nx, ny, w, depth; CHECK(hz::shader_ndc(A, hz::kScreen, P[i], nx, ny, w, depth));
        const float v[4] = {float(2048.0 + nx * 341.0), float(2048.0 - ny * 224.0), float(depth * 16777216.0), 1.0f};
        std::memcpy(g.vtx + 64 * i, v, 16);
        wsrc[i] = w;
        CHECK(v[2] > 256.0f);   // a real-depth quad: an effect (K10 glows: z 3.4e4..2.9e5)
    }
    for (int variant = 0; variant < 3; ++variant) {   // 0 glow on, 1 u3ag:0, 2 glow on but an unusable viewport
        Producer p; u3::Options o; o.on = true; o.hangar = 1; o.hangar_glow = variant != 1; configure(p, o);
        Driver d(p);
        p.on_present(1000);
        CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);
        UiFacts gf = g; gf.list = &d.L; gf.now_ms = 1001;
        if (variant == 2) gf.vp[2] = 1280;
        const UiPlan gp = d.ui(gf);
        CHECK(streq(d.ui(ps2(&d.L, 1001, 0, 0, 0, 0, 0, true)).why, "u3a-fx-dropped"));   // the floor shadow (indexed): dropped as before
        CHECK(d.acm_draw(acm(&d.L, 1001, rec)).record);                                       // more room after the glow (K10 order)
        const UiPlan box = d.ui(ps2(&d.L, 1001, 1808, 2051, 2024, 2187, 64));
        CHECK(box.record);
        if (variant == 0) {
            CHECK(gp.record && gp.copies == 1 && gp.vertices == 4 && gp.eye_draw[0] && gp.eye_draw[1] && gp.bind_dsv);
            CHECK(gp.upload_offset[0] >= kUiUploadBase + uint64_t(gp.slot) * u3::kUploadPerSlot && gp.upload_offset[1] == gp.upload_offset[0] + 4 * 64);
            const PoseIn ps = pose();
            double hp[3]; for (int i = 0; i < 3; ++i) hp[i] = 0.5 * (ps.eyes[0].position[i] + ps.eyes[1].position[i]);
            hz::Room room; CHECK(hz::make_room(cam, hp, ps.eyes[0].orientation, 5.0, room) == nullptr);
            for (unsigned e = 0; e < 2; ++e) {
                CHECK(gp.scissor[e][0] == 0 && gp.scissor[e][1] == 0 && gp.scissor[e][2] == int32_t(kEyeW) && gp.scissor[e][3] == int32_t(kEyeH));
                hz::Eye ev; for (int i = 0; i < 3; ++i) ev.p[i] = ps.eyes[e].position[i];
                for (int i = 0; i < 4; ++i) { ev.q[i] = ps.eyes[e].orientation[i]; ev.fov[i] = ps.eyes[e].fov[i]; }
                float Ae[16]; CHECK(hz::eye_matrix(room, cam, hz::kScreen, ev, Ae) == nullptr);
                for (unsigned i = 0; i < 4; ++i) {
                    double nx, ny, we, depth; CHECK(hz::shader_ndc(Ae, hz::kScreen, P[i], nx, ny, we, depth));
                    const double x = 2048.0 + nx * 341.0, y = 2048.0 - ny * 224.0;
                    if (!(std::fabs(fl(gp.vtx[e], i, 0) - x) < 0.1 && std::fabs(fl(gp.vtx[e], i, 1) - y) < 0.1))
                        std::printf("glow e%u v%u got %.4f %.4f want %.4f %.4f\n", e, i, fl(gp.vtx[e], i, 0), fl(gp.vtx[e], i, 1), x, y);
                    CHECK(std::fabs(fl(gp.vtx[e], i, 0) - x) < 0.1 && std::fabs(fl(gp.vtx[e], i, 1) - y) < 0.1);
                    CHECK(std::fabs(fl(gp.vtx[e], i, 3) - wsrc[i] / we) < 1e-3 * (wsrc[i] / we));   // q' = w_src / w_eye (hangar units)
                    CHECK(fl(gp.vtx[e], i, 2) == fl(gf.vtx, i, 2));                                    // z unchanged
                    CHECK(std::memcmp(gp.vtx[e] + 64 * i + 16, gf.vtx + 64 * i + 16, 48) == 0);        // colours, texcoords unchanged
                }
            }
            // the two eyes see the glow at different places (stereo), and both differ from the game camera's image (a moved, level room)
            CHECK(std::fabs(fl(gp.vtx[0], 0, 0) - fl(gp.vtx[1], 0, 0)) > 1e-3);
        } else {
            CHECK(!gp.record && streq(gp.why, "u3a-fx-dropped"));
        }
        p.on_present(1016);
        CHECK(d.end() == (variant == 0 ? 4u : 3u));   // 2 ACM + [glow] + the box: the room is never rejected by a glow
        const std::string s = p.ui3d_summary();
        CHECK(has(s, ";u3a-rejected=0;") && has(s, ";u3a-published=1;"));
        CHECK(has(s, variant == 0 ? ";u3a-fx=1;" : ";u3a-fx=2;") && has(s, variant == 0 ? ";u3a-glow=1;" : ";u3a-glow=0;"));
        if (variant == 2) CHECK(has(s, ";u3a-glow-why=u3a-glow-viewport"));
        if (variant == 0) CHECK(has(s, ";u3a-glow-why=none"));
    }
    {   // the pure re-projection: from the latch (the eye at the anchor looking along the room's forward with the game's pitch and lens) the
        // effect lands where the game drew it, as A'_e = A for the game's own camera (ui3d_hangar_offline test_eye_matrix_reproduces_game_camera)
        const double head[3] = {0.0, 1.6, 0.0}, q0[4] = {0, 0, 0, 1};
        hz::Room room; CHECK(hz::make_room(cam, head, q0, 5.0, room) == nullptr);
        double l[3], back[3];
        hz::world_to_local(room, P[0], l); hz::local_to_world(room, l, back);
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(back[k] - P[0][k]) < 1e-9 * (1.0 + std::fabs(P[0][k])));   // world_to_local inverts local_to_world
        double G[3], w;
        CHECK(hz::gs_to_world(cam, u3::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f}, fl(g.vtx, 0, 0), fl(g.vtx, 0, 1), fl(g.vtx, 0, 2), G, w));
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(G[k] - P[0][k]) < 0.05);   // float GS x/y/z: within 0.05 units at 150 units
        CHECK(std::fabs(w - wsrc[0]) < 0.05);
        CHECK(!hz::gs_to_world(cam, u3::Cb0{682.0f, -448.0f, 2048.0f, 2048.0f}, 2048, 2048, -300.0, G, w));   // z below b: no depth
    }
}

// ================================================================================================== regression check: step139 runAGU1 "hangar text gone"
// The report: upright\acz-vr-xr-screen-t99313-eye0-table.png shows only "OK", "ST", "De" on yellow cells. The flat image dumped at the same
// instant (t99313-screen.png, xr-stereo.log png-screen t-ms=99331) shows the same: "OK|", "STR", "Dep" on yellow cells = the game's own
// text-reveal animation of the deploy prompt; t83305 / t91313 (aircraft select, weapon select) show every line of text in the eye image.
// The census never saw a batched glyph draw: every non-indexed ps2_v overlay draw is ONE quad (K10 hangar: 3,980 x n=4, 44 x n=3; step139
// runAGU1: 2,371 x n=4). This replays the REAL overlay of the K10 PLAYER weapon screen (runK10 ee-camera.log, ticks 127156569-570, s=8,
// all=154..187: the census vertices exactly) twice in a hangar frame and checks that only the three full-screen draws are dropped, every
// box and glyph is replayed for both eyes, the glyphs stay on the text layer, and frame 2's glyphs (with frame 1's text rows live, i.e. with
// the selection rule armed) are byte-identical to frame 1's.
struct RealDraw { int all; float v[4][3]; };   // census `v=` x,y,z per vertex in the logged order
const RealDraw kK10Overlay[] = {
    {154, {{1768, 1808, 64}, {1768, 2287, 64}, {2328, 1808, 64}, {2328, 2287, 64}}},   // 0.821 x 1.0 additive (tint)
    {155, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},   // 0.751 x 1.0 (tint / grid / frame)
    {156, {{1792, 1824, 64}, {1792, 2272, 64}, {2304, 1824, 64}, {2304, 2272, 64}}},
    {157, {{1924, 2096, 64}, {1924, 2176, 64}, {2004, 2096, 64}, {2004, 2176, 64}}},   // 0.117 x 0.179
    {158, {{1780, 2065, 64}, {1780, 2193, 64}, {2036, 2065, 64}, {2036, 2193, 64}}},   // 0.375 x 0.286 [STATISTICS] panel
    {159, {{1806, 1858, 64}, {1806, 1986, 64}, {2062, 1858, 64}, {2062, 1986, 64}}},   // 0.375 x 0.286 [SP WEAPON NAME] panel
    {160, {{2152, 2092, 64}, {2152, 2188, 64}, {2288, 2092, 64}, {2288, 2188, 64}}},   // 0.199 x 0.214 [WEAPONS] panel
    {161, {{2168, 1878, 64}, {2168, 1910, 64}, {2296, 1878, 64}, {2296, 1910, 64}}},   // 0.188 x 0.071 PLAYER box
    {162, {{1809, 1834, 64}, {1809, 1866, 64}, {1841, 1834, 64}, {1841, 1866, 64}}},   // 0.047 x 0.071 button tiles
    {163, {{1860, 1834, 64}, {1860, 1866, 64}, {1892, 1834, 64}, {1892, 1866, 64}}},
    {164, {{1952, 1834, 64}, {1952, 1866, 64}, {1984, 1834, 64}, {1984, 1866, 64}}},
    {165, {{1820, 1889, 1}, {1826, 1889, 1}, {1820, 1905, 1}, {1826, 1905, 1}}},       // glyphs "[SP WEAPON NAME]" (z 1)
    {166, {{1826, 1889, 1}, {1832, 1889, 1}, {1826, 1905, 1}, {1832, 1905, 1}}},
    {167, {{1832, 1889, 1}, {1838, 1889, 1}, {1832, 1905, 1}, {1838, 1905, 1}}},
    {168, {{1839, 1889, 1}, {1842, 1889, 1}, {1839, 1893, 1}, {1842, 1893, 1}}},
    {169, {{1843, 1889, 1}, {1853, 1889, 1}, {1843, 1905, 1}, {1853, 1905, 1}}},
    {170, {{1853, 1889, 1}, {1859, 1889, 1}, {1853, 1905, 1}, {1859, 1905, 1}}},
    {171, {{1860, 1889, 1}, {1867, 1889, 1}, {1860, 1905, 1}, {1867, 1905, 1}}},
    {172, {{1868, 1889, 1}, {1874, 1889, 1}, {1868, 1905, 1}, {1874, 1905, 1}}},
    {173, {{1874, 1889, 1}, {1881, 1889, 1}, {1874, 1905, 1}, {1881, 1905, 1}}},
    {174, {{1882, 1889, 1}, {1889, 1889, 1}, {1882, 1905, 1}, {1889, 1905, 1}}},
    {175, {{1890, 1889, 1}, {1893, 1889, 1}, {1890, 1893, 1}, {1893, 1893, 1}}},
    {176, {{1893, 1889, 1}, {1900, 1889, 1}, {1893, 1905, 1}, {1900, 1905, 1}}},
    {177, {{1901, 1889, 1}, {1908, 1889, 1}, {1901, 1905, 1}, {1908, 1905, 1}}},
    {178, {{1909, 1889, 1}, {1916, 1889, 1}, {1909, 1905, 1}, {1916, 1905, 1}}},
    {179, {{1916, 1889, 1}, {1922, 1889, 1}, {1916, 1905, 1}, {1922, 1905, 1}}},
    {180, {{1923, 1889, 1}, {1930, 1889, 1}, {1923, 1901, 1}, {1930, 1901, 1}}},
    {181, {{1820, 1901, 1}, {1832, 1901, 1}, {1820, 1933, 1}, {1832, 1933, 1}}},       // glyphs "SAAM" (0.018 x 0.071)
    {182, {{1832, 1901, 1}, {1844, 1901, 1}, {1832, 1933, 1}, {1844, 1933, 1}}},
    {183, {{1845, 1901, 1}, {1857, 1901, 1}, {1845, 1933, 1}, {1857, 1933, 1}}},
    {184, {{1858, 1901, 1}, {1873, 1901, 1}, {1858, 1933, 1}, {1873, 1933, 1}}},
    {185, {{1820, 1928, 1}, {1828, 1928, 1}, {1820, 1952, 1}, {1828, 1952, 1}}},       // glyphs "SEMI-ACTIVE"...
    {186, {{1828, 1928, 1}, {1836, 1928, 1}, {1828, 1952, 1}, {1836, 1952, 1}}},
    {187, {{1837, 1928, 1}, {1845, 1928, 1}, {1837, 1952, 1}, {1845, 1952, 1}}},
};
UiFacts real_draw(const void* list, int64_t now, const RealDraw& r) {
    UiFacts f = ps2(list, now, 0, 0, 1, 1, 0);
    for (int i = 0; i < 4; ++i) {
        const float xyzq[4] = {r.v[i][0], r.v[i][1], r.v[i][2], 1.0f};
        std::memcpy(f.vtx + 64 * i, xyzq, 16);
    }
    return f;
}
void test_k10_hangar_text_all_replayed() {
    uint8_t rec[hz::kRecBytes]; real_record(rec);
    Producer p; u3::Options o; o.on = true; o.hangar = 1; configure(p, o);   // the step139 defaults: u3sel 2, u3at 0, u3ag 1
    Driver d(p);
    const size_t n = sizeof kK10Overlay / sizeof kK10Overlay[0];
    std::vector<UiPlan> frame[2];
    for (int fr = 0; fr < 2; ++fr) {
        const int64_t t = 1000 + 16 * fr;
        p.on_present(t);
        CHECK(d.acm_draw(acm(&d.L, t + 1, rec)).record);
        for (size_t i = 0; i < n; ++i) frame[fr].push_back(d.ui(real_draw(&d.L, t + 1, kK10Overlay[i])));
        d.end();
    }
    unsigned glyphs = 0;
    for (int fr = 0; fr < 2; ++fr)
        for (size_t i = 0; i < n; ++i) {
            const UiPlan& pl = frame[fr][i];
            const bool full_screen = kK10Overlay[i].all <= 156;
            const bool glyph = kK10Overlay[i].v[0][2] == 1.0f;
            if (full_screen) { CHECK(!pl.record && streq(pl.why, "u3a-tint-dropped")); continue; }
            if (!(pl.record && pl.eye_draw[0] && pl.eye_draw[1]))
                std::printf("K10 all=%d frame %d: record=%d why=%s\n", kK10Overlay[i].all, fr, pl.record ? 1 : 0, pl.why ? pl.why : "-");
            CHECK(pl.record && pl.eye_draw[0] && pl.eye_draw[1]);
            for (unsigned e = 0; e < 2; ++e) CHECK(pl.scissor[e][2] > pl.scissor[e][0] && pl.scissor[e][3] > pl.scissor[e][1]);
            if (glyph) {
                CHECK(pl.layer == 2 && pl.copies == 1);   // text: the text layer, never a slab
                if (fr == 1) {
                    ++glyphs;
                    CHECK(std::memcmp(pl.vtx[0], frame[0][i].vtx[0], 4 * 64) == 0 && std::memcmp(pl.vtx[1], frame[0][i].vtx[1], 4 * 64) == 0);
                }
            } else if (pl.copies > 1) {
                CHECK(fr == 1 && pl.copies == u3::kSlabCopies);   // a selection slab only with the previous frame's rows (behind the text)
            }
        }
    CHECK(glyphs == 23);
    p.on_present(1048);
    d.end();
    const std::string s = p.ui3d_summary();
    CHECK(has(s, ";u3a-rejected=0;") && has(s, ";u3a-tint=6;"));
}

} // namespace

int main() {
    test_k10_hangar_text_all_replayed();
    test_menu_selection_slab();
    test_hangar_tint_dropped();
    test_hangar_glow_reprojected();
    std::printf("ui3d_select_offline: %u checks passed\n", g_checks);
    return 0;
}
