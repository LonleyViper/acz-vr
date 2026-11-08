// DEBRIEF-3D offline tests (src/acz-vr/dbr3d.h; docs/world-render/debrief-3d-map.md section 7): options (byte-identical rest, bad values,
// the 255-character terrain buffer), the window signature and latch on the values of the live census (live-1508, 60/60 frames), the online
// role assigner on a frame built like the census frame s=1, the table geometry (aspect, orientation, never mirrored), the per-eye vertex rewrite
// (exact centre, perspective-correct q, culling, fail-closed reasons), the eye guard, the upload budget and the summary line.
// Nothing here touches D3D12, the game or a headset; fixture success says nothing about the live debrief.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "dbr3d.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace db = acz::dbr3d;
namespace u3 = acz::ui3d;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near_(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }

namespace {

const u3::Cb0 kGs{682.0f, -448.0f, 2048.0f, 2048.0f};        // the GS mapping of the debrief [E]
const u3::Cb0 kPix{2560.0f, -1440.0f, 1280.0f, 720.0f};      // the pixel-space mapping [E]
constexpr float kZ = 16777216.0f;

struct Quad { float v[4][4]; };
Quad quad(float x0, float y0, float x1, float y1, float z, float q = 1.0f) {
    Quad o{{{x0, y0, z, q}, {x1, y0, z, q}, {x0, y1, z, q}, {x1, y1, z, q}}};
    return o;
}
db::DrawIn draw(const Quad& q, const u3::Cb0& cb, uint32_t src, uint32_t dst, bool indexed = false) {
    db::DrawIn d; d.vs8 = db::kPs2Vs8; d.ps8 = db::kMenuPs8; d.indexed = indexed; d.cb = cb; d.blend = true; d.src = src; d.dst = dst;
    d.v = q.v; d.nv = 4;
    return d;
}
// the live window quad and its neighbours (census s=1, ord 11 / 13 / 19 / 26 / 32)
const Quad kWindow = quad(1817, 1880, 2278, 2168, kZ);
const Quad kHeader = quad(1817.4f, 1880.5f, 2277.5f, 1895.5f, kZ);
const Quad kCorner = quad(1809, 1872, 1841.5f, 1904.5f, kZ);
const Quad kLabel = quad(2167.5f, 2143.5f, 2267.5f, 2157.5f, kZ);
const Quad kGlyph = quad(1817, 2180, 1827, 2204, 1.0f);

const char* kDay = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;"
                   "fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1";   // the step134 day value (runtime.cfg-installed of live-1508)

void test_options() {
    const std::string day = kDay;
    std::string rest, why; db::Options o;
    CHECK(db::extract_options(day, rest, o, why) && rest == day && !o.on && !o.any_token);
    CHECK(db::extract_options(day + ";dbr3d:1", rest, o, why) && rest == day && o.on && o.any_token);
    CHECK(o.width_m == 1.0f && o.dist_m == 0.75f && o.y_m == -0.45f && o.lift_m == 0.03f);
    CHECK(db::extract_options(day + ";dbr3d:1;dbrw:1.5;dbrd:0.9;dbry:-0.5;dbrl:0.06", rest, o, why) && rest == day);
    CHECK(o.width_m == 1.5f && o.dist_m == 0.9f && o.y_m == -0.5f && o.lift_m == 0.06f);
    CHECK(db::extract_options("dbr3d:1", rest, o, why) && rest == "dbr3d:1" && !o.on);   // the first item is the stage id, never an option
    CHECK(!db::extract_options(day + ";dbr3d:2", rest, o, why) && why == "bad value for dbr3d (0|1)");
    CHECK(!db::extract_options(day + ";dbrw:9", rest, o, why) && why == "bad value for dbrw (0.2..4 m)");
    CHECK(!db::extract_options(day + ";dbrl:-0.1", rest, o, why) && why == "bad value for dbrl (0..0.3 m)");
    CHECK(!db::extract_options(day + ";dbry:x", rest, o, why) && why == "bad value for dbry (-2.5..1 m)");
    // the terrain option buffer holds 255 characters (D-368): the day value + the option fits
    CHECK((day + ";dbr3d:1").size() <= 255);
    // other parsers' tokens survive untouched (brief3d / ui3d keys are not ours)
    CHECK(db::extract_options(day + ";dbr3d:1;b3w:1.2", rest, o, why) && rest == day + ";b3w:1.2");
    CHECK(std::string(db::prerequisites(db::Options{}, true, true, true)) == "off");
    o = db::Options{}; o.on = true;
    CHECK(std::string(db::prerequisites(o, false, true, true)) == "needs-vscreen:1");
    CHECK(std::string(db::prerequisites(o, true, false, true)) == "needs-vsearly:1");
    CHECK(std::string(db::prerequisites(o, true, true, false)) == "needs-xr_hud_layer");
    CHECK(db::prerequisites(o, true, true, true) == nullptr);
    CHECK(db::describe(o) == "dbr3d=1;dbrw=1.00;dbrd=0.75;dbry=-0.45;dbrl=0.030;dbrr=4.00;dbrk=1.40");
}

void test_window_signature() {
    db::GsRect r;
    CHECK(db::is_window_quad(draw(kWindow, kGs, 5, 6), r) && r.x0 == 1817 && r.x1 == 2278 && r.y0 == 1880 && r.y1 == 2168);
    CHECK(!db::is_window_quad(draw(kHeader, kGs, 5, 2), r));           // additive header strip, and too thin
    CHECK(!db::is_window_quad(draw(quad(1817.4f, 1880.5f, 2277.5f, 1895.5f, kZ), kGs, 5, 6), r));   // alpha but 15 GS high
    CHECK(!db::is_window_quad(draw(kCorner, kGs, 5, 2), r));
    CHECK(!db::is_window_quad(draw(kGlyph, kGs, 5, 6), r));            // z 1
    CHECK(!db::is_window_quad(draw(kWindow, kPix, 5, 6), r));          // pixel space
    CHECK(!db::is_window_quad(draw(kWindow, kGs, 5, 6, true), r));     // indexed
    CHECK(!db::is_window_quad(draw(quad(1817, 1880, 2278, 2168, kZ, 2.0f), kGs, 5, 6), r));   // q != 1
    Quad skew = kWindow; skew.v[3][0] = 2270;                           // not axis aligned
    CHECK(!db::is_window_quad(draw(skew, kGs, 5, 6), r));
    db::DrawIn acm = draw(kWindow, kGs, 5, 6); acm.vs8 = 0xb569109eu;
    CHECK(!db::is_window_quad(acm, r));
    // the 3-of-60 full-width top band (1792..2304 x 1824..1888.5) is NOT a window: 64.5 GS high = 0.14 of the picture
    CHECK(!db::is_window_quad(draw(quad(1792, 1824, 2304.5f, 1888.5f, kZ), kGs, 5, 6), r));
}

void test_latch() {
    db::Latch l; db::GsRect r;
    CHECK(db::is_window_quad(draw(kWindow, kGs, 5, 6), r));
    CHECK(db::latch_window(l, r, kGs) && l.screens == 1 && l.place_pending);
    l.place_pending = false;
    CHECK(!db::latch_window(l, r, kGs) && l.screens == 1 && !l.place_pending);   // the same window every frame: one screen
    db::GsRect s = r; s.x0 += 1.5; CHECK(!db::latch_window(l, s, kGs));           // within the tolerance
    s = r; s.y1 += 5; CHECK(db::latch_window(l, s, kGs) && l.screens == 2 && l.place_pending);   // a new screen: placed again
}

void test_assigner_census_frame() {
    db::Assigner a;
    // frame 1: fills (pixel space, unread), the strip draw BEFORE the window (no latch yet: flat), the window, header, corner, label, a small
    // sprite, the narration glyph
    a.begin_frame();
    db::DrawIn fill; fill.vs8 = db::kPs2Vs8; fill.ps8 = db::kMenuPs8; fill.indexed = true; fill.cb = kPix;   // nv = 0
    CHECK(a.assign(fill) == db::Role::flat);
    const Quad inside = quad(1900, 1950, 2200, 2100, 0.0f);
    db::DrawIn strip = draw(inside, kGs, 5, 6, true);
    CHECK(a.assign(strip) == db::Role::flat);                                    // nothing latched yet
    CHECK(a.assign(draw(kWindow, kGs, 5, 6)) == db::Role::window);
    CHECK(a.assign(draw(kHeader, kGs, 5, 2)) == db::Role::surface);
    CHECK(a.assign(draw(kCorner, kGs, 5, 2)) == db::Role::flat);               // straddles the window border
    CHECK(a.assign(draw(kLabel, kGs, 5, 2)) == db::Role::surface);              // 100 x 14 GS: 0.22 of the window wide
    CHECK(a.assign(draw(quad(2000, 2000, 2016, 2016, 0.0f), kGs, 5, 2, true)) == db::Role::marker);
    CHECK(a.assign(draw(kGlyph, kGs, 5, 6)) == db::Role::flat);                 // below the window
    CHECK(!a.why() && a.on_table == 4 && a.flat == 4 && a.latch.screens == 1);
    db::Stats st; db::count_frame(st, a);
    // frame 2: the latched window puts the early strip draw on the table
    a.begin_frame();
    CHECK(a.assign(fill) == db::Role::flat);
    CHECK(a.assign(strip) == db::Role::surface);
    CHECK(a.assign(draw(kWindow, kGs, 5, 6)) == db::Role::window && a.latch.screens == 1);
    // an indexed GS draw whose vertices were not read refuses the frame (fail closed), and the frame stays refused
    db::DrawIn unread = strip; unread.v = nullptr; unread.nv = 0;
    CHECK(a.assign(unread) == db::Role::refused && std::strcmp(a.why(), "dbr3d-vertices-unread") == 0);
    CHECK(a.assign(draw(kLabel, kGs, 5, 2)) == db::Role::refused);
    db::count_frame(st, a);
    // frame 3: perspective content (q != 1) inside the window refuses; other shaders refuse
    a.begin_frame();
    CHECK(a.assign(draw(quad(1900, 1950, 2000, 2050, 0.0f, 0.5f), kGs, 5, 2, true)) == db::Role::refused);
    CHECK(std::strcmp(a.why(), "dbr3d-perspective-content") == 0);
    db::count_frame(st, a);
    a.begin_frame();
    db::DrawIn acm = draw(kWindow, kGs, 5, 6); acm.vs8 = 0xb569109eu;
    CHECK(a.assign(acm) == db::Role::refused && std::strcmp(a.why(), "dbr3d-frame-has-other-shader") == 0);
    db::count_frame(st, a);
    a.begin_frame();
    db::DrawIn ps = draw(kWindow, kGs, 5, 6); ps.ps8 = 0x9ebdf113u;
    CHECK(a.assign(ps) == db::Role::refused && std::strcmp(a.why(), "dbr3d-frame-has-other-ps") == 0);
    // non-finite vertex
    a.begin_frame();
    Quad nan = inside; nan.v[2][1] = std::nanf("");
    CHECK(a.assign(draw(nan, kGs, 5, 2, true)) == db::Role::refused && std::strcmp(a.why(), "dbr3d-vertex-invalid") == 0);
    // a frame without any window and nothing latched: flat only
    db::Assigner b; b.begin_frame();
    CHECK(b.assign(draw(kGlyph, kGs, 5, 6)) == db::Role::flat);
    db::count_frame(st, b);
    CHECK(st.frames == 5 && st.tabled == 1 && st.refused == 3 && st.flat_only == 1 && st.screens == 0);
    const std::string line = db::summary_line(st);
    CHECK(line.rfind("summary-dbr3d;frames=5;tabled=1;refused=3;flat-only=1;", 0) == 0 && line.find("last-why=dbr3d-frame-has-other-shader") != std::string::npos);
}

// head at the LOCAL origin, yaw 0 (looking along -z)
const double kHeadP[3] = {0, 0, 0}, kHeadQ[4] = {0, 0, 0, 1};
db::Table table(const db::Options& o) {
    db::Latch l; db::GsRect r;
    CHECK(db::is_window_quad(draw(kWindow, kGs, 5, 6), r) && db::latch_window(l, r, kGs));
    db::Table t;
    CHECK(db::make_table(o, l, u3::SourceRect{}, 2560.0 / 1440.0, kHeadP, kHeadQ, 7, t) == nullptr);
    return t;
}
u3::EyeView down_eye(double x, double y, double z) {   // looking straight down (-90 deg about x): view up = LOCAL -z (the table's far edge)
    u3::EyeView e; e.p[0] = x; e.p[1] = y; e.p[2] = z;
    const double s = std::sqrt(0.5);
    e.q[0] = -s; e.q[1] = 0; e.q[2] = 0; e.q[3] = s;
    return e;
}
void vtx64(const Quad& q, uint8_t out[4 * 64]) {
    std::memset(out, 0, 4 * 64);
    for (int i = 0; i < 4; ++i) {
        std::memcpy(out + i * 64, q.v[i], 16);
        const float col[4] = {0.25f, 0.5f, 0.75f, 1.0f}; std::memcpy(out + i * 64 + 16, col, 16);   // attributes copied unchanged
    }
}
float f_at(const uint8_t* p, int vtx, int word) { float f; std::memcpy(&f, p + vtx * 64 + word * 4, 4); return f; }

void test_table_geometry() {
    db::Options o; o.on = true;
    const db::Table t = table(o);
    CHECK(t.valid && t.pl.space_generation == 7);
    // window on the shown image: u = (x - 2048) * 2 / 682, v = (y - 2048) * 2 / -448
    CHECK(near_(t.u0, (1817 - 2048) * 2.0 / 682) && near_(t.u1, (2278 - 2048) * 2.0 / 682) && near_(t.v0, -120 * 2.0 / 448) && near_(t.v1, 0.75));
    // depth from the window's aspect on the 16:9 image: 1730.4 x 925.7 px -> 1.0 x 0.535 m
    CHECK(near_(t.width, 1.0) && near_(t.depth, 0.53496, 1e-4));
    // the window centre is the table centre, 0.75 m ahead at -0.45 m
    double P[3]; db::table_point(t, 0.5 * (t.u0 + t.u1), 0.5 * (t.v0 + t.v1), 0.0, P);
    CHECK(near_(P[0], 0) && near_(P[1], -0.45) && near_(P[2], -0.75));
    // the window's top edge (v1) is the FAR edge, its left edge is on the left: never mirrored
    db::table_point(t, t.u0, t.v1, 0.0, P);
    CHECK(near_(P[0], -0.5) && near_(P[2], -0.75 - 0.5 * t.depth));
    db::table_point(t, t.u1, t.v0, 0.02, P);
    CHECK(near_(P[0], 0.5) && near_(P[1], -0.43) && near_(P[2], -0.75 + 0.5 * t.depth));
    // a head turned 90 deg left (yaw +90): the table is placed to the LOCAL -x side
    const double kQuarterPi = 0.78539816339744831;
    const double ql[4] = {0, std::sin(kQuarterPi), 0, std::cos(kQuarterPi)};
    db::Latch l; db::GsRect r; db::is_window_quad(draw(kWindow, kGs, 5, 6), r); db::latch_window(l, r, kGs);
    db::Table t2; CHECK(db::make_table(o, l, u3::SourceRect{}, 16.0 / 9.0, kHeadP, ql, 1, t2) == nullptr);
    db::table_point(t2, 0.5 * (t2.u0 + t2.u1), 0.5 * (t2.v0 + t2.v1), 0.0, P);
    CHECK(near_(P[0], -0.75, 1e-9) && near_(P[2], 0.0, 1e-9));
    // failures
    db::Table bad;
    CHECK(std::strcmp(db::make_table(o, db::Latch{}, u3::SourceRect{}, 16.0 / 9.0, kHeadP, kHeadQ, 1, bad), "dbr3d-no-window") == 0 && !bad.valid);
    CHECK(std::strcmp(db::make_table(o, l, u3::SourceRect{}, 0.0, kHeadP, kHeadQ, 1, bad), "dbr3d-image-aspect") == 0);
    u3::SourceRect sr; sr.w = 0; CHECK(std::strcmp(db::make_table(o, l, sr, 16.0 / 9.0, kHeadP, kHeadQ, 1, bad), "dbr3d-viewport") == 0);
}

void test_rewrite() {
    db::Options o; o.on = true;
    const db::Table t = table(o);
    // eye 1 m straight above the table centre: the window centre lands on the eye's image centre (GS 2048, 2048), q' = 1 / 1 m
    const u3::EyeView top = down_eye(0, 0.55, -0.75);
    const float cx = 0.5f * (1817 + 2278), cy = 0.5f * (1880 + 2168);
    const Quad c = quad(cx, cy, cx, cy, 0.0f);
    uint8_t src[256], dst[256];
    vtx64(c, src);
    db::RewriteResult rr = db::rewrite_vertices(src, 4, t, o, db::Role::surface, top, dst);
    CHECK(!rr.why && !rr.culled);
    CHECK(near_(f_at(dst, 0, 0), 2048.0, 1e-3) && near_(f_at(dst, 0, 1), 2048.0, 1e-3) && near_(f_at(dst, 0, 3), 1.0, 1e-6));
    CHECK(f_at(dst, 0, 2) == 0.0f && f_at(dst, 0, 4) == 0.25f && f_at(dst, 3, 7) == 1.0f);   // z and attributes unchanged
    // the window corners: top-left appears top-left in the eye (GS x < 2048, GS y < 2048: y grows downward), equal q' (same depth)
    vtx64(kWindow, src);
    rr = db::rewrite_vertices(src, 4, t, o, db::Role::window, top, dst);
    CHECK(!rr.why && !rr.culled);
    CHECK(f_at(dst, 0, 0) < 2048.0f && f_at(dst, 0, 1) < 2048.0f && f_at(dst, 3, 0) > 2048.0f && f_at(dst, 3, 1) > 2048.0f);
    CHECK(near_(f_at(dst, 0, 3), 1.0, 1e-6) && near_(f_at(dst, 3, 3), 1.0, 1e-6));
    // left/right symmetric about the centre only by the window's own offset: x'(TL) and x'(BR) straddle 2048
    // a marker is lifted: seen from above it is nearer, so q' = 1 / (1 - lift)
    rr = db::rewrite_vertices(src, 4, t, o, db::Role::marker, top, dst);
    CHECK(!rr.why && near_(f_at(dst, 0, 3), 1.0 / (1.0 - 0.03), 1e-5));
    // from the head (looking ahead, down at the table): the near edge has a larger q' (= 1 / w) than the far edge: perspective-correct
    const u3::EyeView head = [] { u3::EyeView e; return e; }();
    rr = db::rewrite_vertices(src, 4, t, o, db::Role::window, head, dst);
    CHECK(!rr.why && !rr.culled);
    CHECK(f_at(dst, 2, 3) > f_at(dst, 0, 3));   // vertex 2 = bottom-left (near edge), vertex 0 = top-left (far edge)
    CHECK(f_at(dst, 2, 1) > f_at(dst, 0, 1));   // the near edge is lower in the image (larger GS y)
    // two eyes 64 mm apart see the table centre at different x (stereo)
    u3::EyeView le = top, re = top; le.p[0] = -0.032; re.p[0] = 0.032;
    vtx64(c, src);
    uint8_t d2[256];
    db::rewrite_vertices(src, 4, t, o, db::Role::surface, le, dst); db::rewrite_vertices(src, 4, t, o, db::Role::surface, re, d2);
    CHECK(f_at(dst, 0, 0) > 2048.0f && f_at(d2, 0, 0) < 2048.0f);
    // behind the eye: culled for this eye only, no reason
    u3::EyeView back; back.q[0] = 0; back.q[1] = 1; back.q[2] = 0; back.q[3] = 0;   // yaw 180
    rr = db::rewrite_vertices(src, 4, t, o, db::Role::surface, back, dst);
    CHECK(!rr.why && rr.culled);
    // fail-closed reasons
    CHECK(std::strcmp(db::rewrite_vertices(src, 4, t, o, db::Role::flat, top, dst).why, "dbr3d-role-not-on-table") == 0);
    CHECK(std::strcmp(db::rewrite_vertices(src, 0, t, o, db::Role::surface, top, dst).why, "dbr3d-no-vertices") == 0);
    CHECK(std::strcmp(db::rewrite_vertices(src, db::kMaxVerticesPerDraw + 1, t, o, db::Role::surface, top, dst).why, "dbr3d-draw-too-many-vertices") == 0);
    CHECK(std::strcmp(db::rewrite_vertices(src, 4, db::Table{}, o, db::Role::surface, top, dst).why, "dbr3d-table-invalid") == 0);
    Quad qz = c; qz.v[1][3] = 0.0f; vtx64(qz, src);
    CHECK(std::strcmp(db::rewrite_vertices(src, 4, t, o, db::Role::surface, top, dst).why, "dbr3d-vertex-q") == 0);
    Quad qn = c; qn.v[2][0] = std::nanf(""); vtx64(qn, src);
    CHECK(std::strcmp(db::rewrite_vertices(src, 4, t, o, db::Role::surface, top, dst).why, "dbr3d-vertex-nonfinite") == 0);
}

void test_eye_guard_and_budget() {
    db::Options o; o.on = true;
    const db::Table t = table(o);
    CHECK(db::eye_clear(t, o, u3::EyeView{}));                          // the head that placed it: every corner in front
    CHECK(db::eye_clear(t, o, down_eye(0, 0.55, -0.75)));
    CHECK(!db::eye_clear(t, o, down_eye(0, -0.42, -0.75)));             // the eye inside the volume (just above the top)
    u3::EyeView back; back.q[1] = 1; back.q[3] = 0;
    CHECK(!db::eye_clear(t, o, back));
    CHECK(!db::eye_clear(db::Table{}, o, u3::EyeView{}));
    // upload: the live frame (~3,600 table vertices) fits; a 20,000-vertex frame does not
    CHECK(db::upload_bytes(3600) == 3600ull * 128 && db::upload_fits(3600) && db::upload_fits(16384) && !db::upload_fits(20000));
    CHECK(std::string(db::role_name(db::Role::marker)) == "marker" && db::role_height(o, db::Role::marker) == double(0.03f) &&
          db::role_height(o, db::Role::surface) == 0.0);
}

// ================================================================================================== STAGE 2 (debrief-3d-map.md 8): the live route
const char* kDay143 = "auto;env:1;look:1;props:1;trees:1;propdedupe:1;interp:1;decouple:1;objdom:display;mk360:3;vscreen:1;vsearly:1;fxmem:6;brief3d:1;"
                      "fxsig:f1a4e000;ui3d:1;u3p:1;u3a:1;wcloud:1;fxtrail:8";   // the step143 day value (live-2210-step143 runtime.cfg)

void test_stage2_options() {
    const std::string day = kDay143;
    std::string rest, why; db::Options o;
    CHECK(db::extract_options(day + ";dbr3d:1", rest, o, why) && rest == day && o.on && o.radius_m == 4.0f && o.spread == 1.4f);
    CHECK(db::extract_options(day + ";dbr3d:1;dbrr:6;dbrk:1.8", rest, o, why) && rest == day && o.radius_m == 6.0f && o.spread == 1.8f);
    CHECK((day + ";dbr3d:1;dbrr:6;dbrk:1.8").size() <= 255);   // the 255-character terrain buffer (D-368) still holds it
    CHECK(!db::extract_options(day + ";dbr3d:1;dbrr:1", rest, o, why) && why == "bad value for dbrr (1.5..20 m)");
    CHECK(!db::extract_options(day + ";dbr3d:1;dbrk:3", rest, o, why) && why == "bad value for dbrk (0.5..2.5)");
    CHECK(!db::extract_options(day + ";dbrk:x", rest, o, why) && why == "bad value for dbrk (0.5..2.5)");
    db::Options on; on.on = true;
    CHECK(std::string(db::live_prerequisites(db::Options{}, true, true, true, true, true)) == "off");
    CHECK(std::string(db::live_prerequisites(on, false, true, true, true, true)) == "needs-vscreen:1");
    CHECK(std::string(db::live_prerequisites(on, true, true, true, false, true)) == "needs-ui3d:1");
    CHECK(std::string(db::live_prerequisites(on, true, true, true, true, false)) == "needs-ee_camera_probe(present-hook)");
    CHECK(db::live_prerequisites(on, true, true, true, true, true) == nullptr);
}

void test_scan_indices() {
    // 16-bit strips with one cut per strip, as the census batches (49 indices / 40 vertices = 10 quads): imin..imax = 40..79 here
    std::vector<uint16_t> s16;
    for (int q = 0; q < 10; ++q) { if (q) s16.push_back(0xFFFF); for (int k = 0; k < 4; ++k) s16.push_back(uint16_t(40 + 4 * q + k)); }
    CHECK(s16.size() == 49);
    db::IndexScan s = db::scan_indices(reinterpret_cast<const uint8_t*>(s16.data()), 49, 2, db::kStripCut16);
    CHECK(!s.why && s.imin == 40 && s.imax == 79 && s.cuts == 9 && s.used == 40 && s.span() == 40);
    // cut disabled: 0xFFFF is a real vertex index -> the span is too large (fail closed, never a fetch outside the copy)
    s = db::scan_indices(reinterpret_cast<const uint8_t*>(s16.data()), 49, 2, db::kStripCutDisabled);
    CHECK(s.why && std::strcmp(s.why, "dbr3d-index-span") == 0);
    // 32-bit with the 32-bit cut
    const uint32_t s32[7] = {7, 8, 9, 0xFFFFFFFFu, 5, 6, 10};
    s = db::scan_indices(reinterpret_cast<const uint8_t*>(s32), 7, 4, db::kStripCut32);
    CHECK(!s.why && s.imin == 5 && s.imax == 10 && s.cuts == 1 && s.span() == 6);
    // refusals
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(s32), 7, 4, db::kStripCut16).why, "dbr3d-strip-cut-mismatch") == 0);
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(s32), 7, 4, UINT32_MAX).why, "dbr3d-strip-cut-unknown") == 0);
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(s32), 7, 3, db::kStripCut32).why, "dbr3d-ib-stride") == 0);
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(s32), 0, 4, db::kStripCut32).why, "dbr3d-no-indices") == 0);
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(s32), db::kMaxIndicesPerDraw + 1, 4, db::kStripCut32).why, "dbr3d-too-many-indices") == 0);
    const uint16_t cuts[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(cuts), 3, 2, db::kStripCut16).why, "dbr3d-only-cuts") == 0);
    // the live maximum (1,559 indices / 1,248 vertices = 312 quads) fits; a span of kMaxVerticesPerDraw + 1 does not
    std::vector<uint16_t> big;
    for (int q = 0; q < 312; ++q) { if (q) big.push_back(0xFFFF); for (int k = 0; k < 4; ++k) big.push_back(uint16_t(4 * q + k)); }
    s = db::scan_indices(reinterpret_cast<const uint8_t*>(big.data()), uint32_t(big.size()), 2, db::kStripCut16);
    CHECK(big.size() == 1559 && !s.why && s.span() == 1248);
    const uint16_t wide[2] = {0, uint16_t(db::kMaxVerticesPerDraw)};
    CHECK(std::strcmp(db::scan_indices(reinterpret_cast<const uint8_t*>(wide), 2, 2, db::kStripCut16).why, "dbr3d-index-span") == 0);
}

void test_window_from_vertices() {
    uint8_t v[4 * 64];
    vtx64(kWindow, v);
    db::GsRect r;
    CHECK(db::window_from_vertices(db::kPs2Vs8, db::kMenuPs8, false, kGs, true, 5, 6, v, 4, r) && near_(r.x0, 1817) && near_(r.y1, 2168));
    CHECK(!db::window_from_vertices(db::kPs2Vs8, db::kMenuPs8, false, kGs, true, 5, 2, v, 4, r));   // additive: map content, not the window
    CHECK(!db::window_from_vertices(db::kPs2Vs8, db::kMenuPs8, true, kGs, true, 5, 6, v, 4, r));    // indexed
    CHECK(!db::window_from_vertices(db::kPs2Vs8, db::kMenuPs8, false, kGs, true, 5, 6, v, 3, r));   // not 4 vertices
    CHECK(!db::window_from_vertices(db::kPs2Vs8, db::kMenuPs8, false, kPix, true, 5, 6, v, 4, r));  // pixel space
    vtx64(kHeader, v);
    CHECK(!db::window_from_vertices(db::kPs2Vs8, db::kMenuPs8, false, kGs, true, 5, 6, v, 4, r));   // the header bar is too thin
}

// The census frame order (debrief-3d-map.md 7.2) through the stage-2 router: fills (indexed, pixel space), the long strips and the sprite batch
// (indexed, GS) are the MAP; the window quad starts the overlay on the far panel layer; the header bar is a panel; the 312-quad text batch and
// the narration glyphs are text; a later pixel-space draw stays on the panel (overlay), never back to the map.
void test_router_census_frame() {
    db::Router rt; rt.begin_frame();
    unsigned layer = 9;
    db::RouteIn fill; fill.indexed = true; fill.pixel_space = true; fill.nv = 2; fill.bbox_w = 0.2; fill.bbox_h = 0.0;
    for (int i = 0; i < 8; ++i) CHECK(rt.route(fill, layer) == db::Route::map && layer == 0);
    db::RouteIn strips; strips.indexed = true; strips.nv = 976; strips.bbox_w = 0.5; strips.bbox_h = 0.6; strips.quad_max = 0.3;
    CHECK(rt.route(strips, layer) == db::Route::map);
    db::RouteIn sprites; sprites.indexed = true; sprites.nv = 40; sprites.bbox_w = 0.4; sprites.bbox_h = 0.3; sprites.quad_max = 0.02;
    CHECK(rt.route(sprites, layer) == db::Route::map && !rt.overlay() && rt.map_draws() == 10);
    db::RouteIn window; window.nv = 4; window.window = true; window.bbox_w = 0.675; window.bbox_h = 0.643;
    CHECK(rt.route(window, layer) == db::Route::panel && layer == 0 && rt.overlay());
    db::RouteIn header; header.nv = 4; header.bbox_w = 0.674; header.bbox_h = 0.033;
    CHECK(rt.route(header, layer) == db::Route::panel && layer == 1);
    db::RouteIn text; text.indexed = true; text.nv = 1248; text.bbox_w = 0.6; text.bbox_h = 0.5; text.quad_max = 0.03;
    CHECK(rt.route(text, layer) == db::Route::panel && layer == 2);   // indexed, but after the overlay started: the panel's text
    db::RouteIn shade; shade.pixel_space = true; shade.nv = 4; shade.bbox_w = 0.3; shade.bbox_h = 0.2;
    CHECK(rt.route(shade, layer) == db::Route::panel && layer == 2);  // the layer never decreases
    CHECK(rt.map_draws() == 10 && rt.panel_draws() == 4);
    // a new frame starts on the map again
    rt.begin_frame();
    CHECK(rt.route(fill, layer) == db::Route::map && !rt.overlay());
    // a menu frame (first draw non-indexed GS): overlay from the start, classed like the stage-2 menus (large -> 0, medium -> 1, small -> 2)
    rt.begin_frame();
    db::RouteIn back; back.nv = 4; back.bbox_w = 0.75; back.bbox_h = 1.0;
    CHECK(rt.route(back, layer) == db::Route::panel && layer == 0);
    db::RouteIn glyph; glyph.nv = 4; glyph.bbox_w = 0.01; glyph.bbox_h = 0.02;
    CHECK(rt.route(glyph, layer) == db::Route::panel && layer == 2);
    // a degenerate draw keeps the current layer
    rt.begin_frame(); CHECK(rt.route(back, layer) == db::Route::panel && layer == 0);
    db::RouteIn zero; zero.nv = 4;
    CHECK(rt.route(zero, layer) == db::Route::panel && layer == 0);
    // a batch whose quads are not small is a panel element (1), whatever its bbox
    db::RouteIn tiles; tiles.indexed = true; tiles.nv = 56; tiles.bbox_w = 0.6; tiles.bbox_h = 0.5; tiles.quad_max = 0.2;
    CHECK(rt.route(tiles, layer) == db::Route::panel && layer == 1);
}

void test_quad_max_dim() {
    // two glyph quads far apart: the batch bbox is large, each quad small
    uint8_t v[8 * 64] = {};
    vtx64(quad(1817, 1900, 1827, 1924, 1.0f), v);
    vtx64(quad(2200, 2100, 2210, 2124, 1.0f), v + 4 * 64);
    const double m = db::quad_max_dim(v, 8, kGs, u3::SourceRect{});
    CHECK(m > 0 && m <= u3::kSmallDim && near_(m, 24.0 / 448.0, 1e-6));
    double w, h; CHECK(u3::screen_bbox(v, 8, kGs, u3::SourceRect{}, w, h) && w > 0.4);
    CHECK(db::quad_max_dim(v, 7, kGs, u3::SourceRect{}) > 0.4);   // not a multiple of 4: the whole bbox
    CHECK(db::quad_max_dim(nullptr, 8, kGs, u3::SourceRect{}) == 0.0);
}

// The wraparound: the vscreen placement of the live session (3.20 x 1.80 m at 1.97 m, yaw 0) seen from its anchor.
u3::Screen live_screen() {
    const float c[3] = {0.0f, 1.6f, -1.97f}; const double a[3] = {0.0, 1.6, 0.0};
    return u3::make_screen(c, 0.0f, 3.2f, 1.8f, a);
}
u3::EyeView eye_at(double x) { u3::EyeView e; e.p[0] = x; e.p[1] = 1.6; e.p[2] = 0.0; return e; }

void test_wraparound() {
    db::Options o; o.on = true;
    const u3::Screen s = live_screen();
    CHECK(s.valid);
    const u3::Options wo = db::wrap_options(u3::Options{});
    CHECK(wo.curve);
    uint8_t src[4 * 64], dst[4 * 64];
    // the picture centre (GS 2048, 2048): straight ahead on the cylinder at dbrr, q' = 1 from the anchor eye
    vtx64(quad(2048, 2048, 2048, 2048, 5.0f), src);
    u3::RewriteResult rr = db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, s, wo, double(o.radius_m), double(o.spread), eye_at(0.0), dst);
    CHECK(!rr.why && !rr.culled && near_(f_at(dst, 0, 0), 2048.0, 1e-3) && near_(f_at(dst, 0, 1), 2048.0, 1e-3) && near_(f_at(dst, 0, 3), 1.0, 1e-6));
    CHECK(f_at(dst, 0, 2) == 5.0f && f_at(dst, 0, 4) == 0.25f);   // the game's z (its depth test) and attributes unchanged
    // every point lies on the cylinder (horizontal distance dbrr from the anchor) at azimuth atan(u k half_w / dist): the 4:3 picture's right
    // edge (GS x 2304, u = 0.75) at 1.4 x -> 40.4 deg
    for (double u : {-0.75, -0.3, 0.0, 0.5, 0.75})
        for (double v : {-1.0, 0.0, 0.7}) {
            double P[3];
            CHECK(u3::surface_point(s, wo, 4.0, 1.4, u, v, P) == nullptr);
            CHECK(near_(std::hypot(P[0], P[2]), 4.0, 1e-9));
            CHECK(near_(std::atan2(P[0], -P[2]), std::atan(u * 1.4 * 1.6 / 1.97), 1e-9));
        }
    // stereo: two eyes 64 mm apart see the centre at mirrored x; the disparity at 4 m is smaller than at the panel's text layer (1.1 m)
    uint8_t dl[4 * 64], dr[4 * 64];
    db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(-0.032), dl);
    db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(0.032), dr);
    const double disp_wrap = f_at(dl, 0, 0) - f_at(dr, 0, 0);
    const u3::Options po = db::panel_options(u3::Options{}, o);
    const u3::Screen ps = u3::scaled(s, 0.7);
    db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, ps, po, u3::layer_distance(po, 2), 1.0, eye_at(-0.032), dl);
    db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, ps, po, u3::layer_distance(po, 2), 1.0, eye_at(0.032), dr);
    const double disp_text = f_at(dl, 0, 0) - f_at(dr, 0, 0);
    CHECK(disp_wrap > 0 && disp_text > 3.0 * disp_wrap);
    // the panel lies in front of the wraparound: every panel layer nearer than dbrr; far layer = 0.75 dbrr (u3mf 6 m otherwise)
    CHECK(near_(po.near_m, 1.1, 1e-6) && near_(po.far_m, 3.0, 1e-6) && u3::layer_distance(po, 0) < 4.0 && near_(u3::layer_distance(po, 2), 1.1, 1e-6));
    db::Options small = o; small.radius_m = 1.5f;
    CHECK(db::panel_options(u3::Options{}, small).far_m > db::panel_options(u3::Options{}, small).near_m);
    db::Options big = o; big.radius_m = 20.0f;
    CHECK(near_(db::panel_options(u3::Options{}, big).far_m, 6.0, 1e-6));
    // chunking: a 300-vertex batch is rewritten exactly as vertex by vertex (ui3d's 64-vertex cap per call)
    std::vector<uint8_t> bs(300 * 64), bd(300 * 64), one(64);
    for (int i = 0; i < 300; ++i) {
        const float f[8] = {1800.0f + float(i % 50) * 9.0f, 1830.0f + float(i / 50) * 70.0f, 3.0f, 1.0f, float(i), 0, 0, 1};
        std::memcpy(bs.data() + i * 64, f, 32);
    }
    rr = db::rewrite_surface(bs.data(), 300, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(0.03), bd.data());
    CHECK(!rr.why && !rr.culled);
    bool same = true;
    for (int i = 0; i < 300; ++i) {
        u3::rewrite_at(bs.data() + i * 64, 1, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(0.03), one.data());
        same = same && std::memcmp(one.data(), bd.data() + i * 64, 64) == 0;
    }
    CHECK(same);
    // the scissor (320..2240 of 2560) carried onto the wraparound: inside the eye image
    const int32_t sc[4] = {320, 0, 2240, 1440};
    u3::PixRect pr;
    CHECK(u3::eye_scissor_at(sc, 2560, 1440, s, wo, 4.0, 1.4, eye_at(0.0), 1440, 1440, pr) && pr.r > pr.l && pr.b > pr.t);
    // fail closed / culled
    CHECK(std::strcmp(db::rewrite_surface(src, 0, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(0), dst).why, "dbr3d-no-vertices") == 0);
    CHECK(std::strcmp(db::rewrite_surface(bs.data(), db::kMaxVerticesPerDraw + 1, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(0), bd.data()).why,
                      "dbr3d-draw-too-many-vertices") == 0);
    Quad qn = quad(2048, 2048, 2050, 2050, 1.0f); qn.v[3][1] = std::nanf(""); vtx64(qn, src);
    CHECK(std::strcmp(db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, eye_at(0), dst).why, "ui3d-vertex-nonfinite") == 0);
    u3::EyeView back = eye_at(0); back.q[1] = 1; back.q[3] = 0;   // looking behind: culled for this eye, no reason
    vtx64(quad(2048, 2048, 2048, 2048, 1.0f), src);
    rr = db::rewrite_surface(src, 4, kGs, u3::SourceRect{}, s, wo, 4.0, 1.4, back, dst);
    CHECK(!rr.why && rr.culled);
}

void test_live_summary() {
    db::LiveStats s;
    char b[512];
    CHECK(db::format_live(b, sizeof b, s, false, 0) > 0 && std::string(b).find("summary-dbr3d;armed=0;arms=0;screens=0;frames=0;") == 0);
    s.arms = s.frames = s.usable = s.published = s.rejected = s.map_draws = s.panel_draws = s.indexed = s.vertices = s.culled = s.refused_draws = s.clipped = s.gaps = ~0ull;
    s.last_why = "dbr3d-a-very-long-reason-that-is-cut-at-32-characters-here";
    const int n = db::format_live(b, sizeof b, s, true, ~0ull);
    CHECK(n > 0 && n < 400 && std::string(b).find(";frames=999999999;") != std::string::npos && std::string(b).find(";clipped=999999999;gaps=999999999;") != std::string::npos);
}

} // namespace

int main() {
    test_options();
    test_window_signature();
    test_latch();
    test_assigner_census_frame();
    test_table_geometry();
    test_rewrite();
    test_eye_guard_and_budget();
    test_stage2_options();
    test_scan_indices();
    test_window_from_vertices();
    test_router_census_frame();
    test_quad_max_dim();
    test_wraparound();
    test_live_summary();
    std::printf("dbr3d_offline: %u checks passed\n", g_checks);
    return 0;
}
