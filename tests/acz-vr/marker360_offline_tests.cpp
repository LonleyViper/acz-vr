// marker360_offline_tests.cpp -- MARKERS-360 (xr_terrain_stage sub-option mk360:1|2; marker360.h, docs/world-render/markers-360.md) CPU cases, no GPU,
// no game: option parsing / producer flags (key off unchanged), the producer's suppression rule, the geometry of A (camera centre, the ray of a GS
// screen point), and scenario sequences on synthetic producer frames: a wingman identified in the window and still boxed (same colour, label, size) at
// its true 3D position when it is behind the player; gone the first frame its world draws stop; identity dropped when the game stops boxing it in view;
// never-boxed contacts not drawn (mk360u:1 grey); exactly one box per contact per frame in mode 1 and mode 2; unread sprites never doubled; one-frame
// colour mismatches never stick; the producer's own match preferred; the edge margin; world-anchored drawing for a head looking backwards.
#include "marker360.h"
#include "tactical.h"
#include "tactical_producer.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz;

static unsigned g_checks = 0;
// MK360_SOFT=1: report every failing check and continue (the before/after comparison of the BOX-LOCK cases); default: stop at the first failure.
static unsigned g_fails = 0;
static bool soft_() { static const bool s = std::getenv("MK360_SOFT") != nullptr; return s; }
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); if (!soft_()) std::exit(1); } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The game's record +0 for a camera at C with axes r/u/f and lens tangents (as tactical_offline_tests make_A; gpu_copy_step5_world.h screen map).
static void make_A(const float C[3], const float r[3], const float u[3], const float f[3], float A[16], float tx = 0.6152f, float ty = 0.3459f) {
    float vp[4][4] = {};
    for (int i = 0; i < 3; ++i) { vp[i][0] = r[i] / tx; vp[i][1] = -u[i] / ty; vp[i][3] = f[i]; }
    float dr = 0, du = 0, df = 0;
    for (int i = 0; i < 3; ++i) { dr += r[i] * C[i]; du += u[i] * C[i]; df += f[i] * C[i]; }
    vp[3][0] = -dr / tx; vp[3][1] = du / ty; vp[3][3] = -df;
    for (int row = 0; row < 4; ++row) {
        A[row * 4 + 0] = 258.048f * vp[row][0] + 2048.0f * vp[row][3];
        A[row * 4 + 1] = 225.792f * vp[row][1] + 2048.0f * vp[row][3];
        A[row * 4 + 2] = 0.0f;
        A[row * 4 + 3] = vp[row][3];
    }
}
// Camera at the origin, heading yaw (deg) about +Y: yaw 0 looks along -Z (right +X), yaw 90 looks along +X (right +Z): right x up = -fwd.
struct Cam { float C[3] = {0, 1000, 0}, r[3], u[3] = {0, 1, 0}, f[3]; float A[16]; };
static Cam cam_yaw(float yaw_deg) {
    Cam c;
    const float y = yaw_deg * 3.14159265f / 180.0f;
    c.f[0] = std::sin(y); c.f[1] = 0; c.f[2] = -std::cos(y);
    c.r[0] = std::cos(y); c.r[1] = 0; c.r[2] = std::sin(y);
    make_A(c.C, c.r, c.u, c.f, c.A);
    return c;
}
static tac::TacFrame frame_for(const Cam& c) {
    tac::TacFrame f; f.valid = true; f.a_valid = true;
    for (int i = 0; i < 16; ++i) f.A[i] = c.A[i];
    return f;
}
static void add_obj(tac::TacFrame& f, const float p[3]) { tac::ObjDraw& o = f.obj[f.nobj++]; for (int i = 0; i < 3; ++i) o.t[i] = p[i]; }
// A game box sprite on the projection of p (as the game draws it), colour rgb, label glyphs, half size 7.5 x 7.5 GS units.
static int add_sprite(tac::TacFrame& f, const float p[3], const float rgb[3], uint8_t glyphs, bool suppressed = true) {
    float xs, ys, w;
    if (!tac::project_ps2(f.A, p, xs, ys, w)) return -1;
    tac::MarkerSprite& m = f.mark[f.nmark++];
    m.cx = xs; m.cy = ys; m.hw = 7.5f; m.hh = 7.5f; m.glyphs = glyphs; m.suppressed = suppressed;
    for (int i = 0; i < 3; ++i) m.rgb[i] = rgb[i];
    if (glyphs) {   // the label in the box's colour, above-left of the box (tactical_offline_tests' label layout)
        for (int i = 0; i < 3; ++i) m.lrgb[i] = rgb[i];
        m.lx0 = xs - 30; m.lx1 = xs - 30 + 6.0f * glyphs - 1; m.ly0 = ys - 14; m.ly1 = ys - 9;
    }
    return int(f.nmark - 1);
}
static const float kBlue[3] = {0.21f, 0.23f, 1.0f}, kRed[3] = {1.0f, 0.0f, 0.0f}, kYellow[3] = {1.0f, 0.85f, 0.0f};
static float dist3(const float a[3], const float b[3]) { const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); }

static void test_options() {
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;env:1;look:1;interp:1;decouple:1", rest, o, why) && rest == "m02a;env:1;look:1;interp:1;decouple:1");
    CHECK(o.mk360 == 0 && !o.mk360_unknown && tac::producer_flags_for(o) == 0u);
    CHECK(tac::describe(o).find("mk360") == std::string::npos);   // key off: the describe line is the old one
    CHECK(tac::extract_options("auto;env:1;mk360:1;decouple:1", rest, o, why) && rest == "auto;env:1;decouple:1" && o.mk360 == 1 && !o.on);
    CHECK(tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerHideAll));
    CHECK(tac::describe(o).find(";mk360=1") != std::string::npos);
    CHECK(tac::extract_options("m02a;mk360=2;mk360u:1", rest, o, why) && rest == "m02a" && o.mk360 == 2 && o.mk360_unknown);
    CHECK(tac::producer_flags_for(o) == tac::kProducerOn);   // mode 2 never hides a game box
    CHECK(tac::extract_options("m02a;tac:1;tacboxes:1;mk360:1", rest, o, why) && o.on && o.markers && o.mk360 == 1);
    CHECK(tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerHideAll));   // mk360 replaces tacboxes: never kProducerSuppress too
    CHECK(tac::extract_options("m02a;mk360:0", rest, o, why) && rest == "m02a" && tac::producer_flags_for(o) == 0u);
    CHECK(!tac::extract_options("m02a;mk360:4", rest, o, why) && why == "bad value for mk360");
    // NBOX mk360:3: parsed, never hides a game draw (the game's pixels are cut from the marker image), described
    CHECK(tac::extract_options("m02a;env:1;mk360:3", rest, o, why) && rest == "m02a;env:1" && o.mk360 == 3);
    // section 17.4: mk360:3 also moves the lock diamond into the marker image (LOCK-SHAPE, default on; mk360lock:0 = off)
    CHECK(tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerLockShape) && tac::describe(o).find(";mk360=3") != std::string::npos);
    CHECK(tac::describe(o).find("mk360lock") == std::string::npos && tac::describe(o).find("mk360cen") == std::string::npos);
    CHECK(mk360::params_from(o).mode == 3);
    CHECK(!tac::extract_options("m02a;mk360u:x", rest, o, why) && why == "bad value for mk360u");
    // the old tac paths are unchanged
    CHECK(tac::extract_options("m02a;tac:1;tacboxes:1", rest, o, why) && tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerSuppress));
    CHECK(tac::extract_options("m02a;tac:1", rest, o, why) && tac::producer_flags_for(o) == tac::kProducerOn);
    const mk360::Params p = mk360::params_from(o);
    CHECK(p.mode == 0 && near(p.self_radius, 80, 0) && near(p.cluster, 60, 0));
}

static void test_suppress_rule() {
    using tac::suppress_box_draw;
    const uint32_t hide = tac::kProducerOn | tac::kProducerHideAll, sup = tac::kProducerOn | tac::kProducerSuppress, on = tac::kProducerOn;
    CHECK(suppress_box_draw(hide, false, 3, 0));       // mk360:1: every kept box draw, matched or not
    CHECK(!suppress_box_draw(hide, true, 3, 1));       // a cluster lost (cap): the game's draw stays (nothing vanishes)
    CHECK(!suppress_box_draw(hide, true, 0, 0));       // nothing kept: the game's draw stays
    CHECK(suppress_box_draw(sup, true, 2, 0) && !suppress_box_draw(sup, false, 2, 0));   // tacboxes:1 as before
    CHECK(!suppress_box_draw(on, true, 2, 0) && !suppress_box_draw(0u, true, 2, 0));     // tac:1 alone / key off: never
}

static void test_geometry() {
    for (float yaw : {0.0f, 37.0f, 90.0f, 180.0f, -123.0f}) {
        const Cam c = cam_yaw(yaw);
        float C[3];
        CHECK(mk360::camera_centre(c.A, C));
        CHECK(dist3(C, c.C) < 0.05f);
        // a point 30 deg right / 10 deg up inside the window: its GS projection's ray points back at it
        const float p[3] = {c.C[0] + 20000 * c.f[0] + 5000 * c.r[0], c.C[1] + 2000, c.C[2] + 20000 * c.f[2] + 5000 * c.r[2]};
        float xs, ys, w;
        CHECK(tac::project_ps2(c.A, p, xs, ys, w));
        float d[3];
        CHECK(mk360::screen_ray(c.A, xs, ys, d));
        float e[3] = {p[0] - C[0], p[1] - C[1], p[2] - C[2]};
        const float l = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        for (float& v : e) v /= l;
        CHECK(near(d[0], e[0], 1e-4) && near(d[1], e[1], 1e-4) && near(d[2], e[2], 1e-4));
        CHECK(mk360::in_window(c.A, p, 1.0f));
        // the window centre ray = the camera forward; behind = never in the window
        CHECK(mk360::screen_ray(c.A, 2048, 2048, d) && near(d[0], c.f[0], 1e-5) && near(d[2], c.f[2], 1e-5));
        const float behind[3] = {c.C[0] - 20000 * c.f[0], c.C[1], c.C[2] - 20000 * c.f[2]};
        CHECK(!mk360::in_window(c.A, behind, 1.0f));
        // 40 deg right is outside the 31.6 deg half window; 30 deg is inside
        const float t40 = std::tan(40.0f * 3.14159265f / 180.0f), t30 = std::tan(30.0f * 3.14159265f / 180.0f);
        const float p40[3] = {c.C[0] + 1000 * (c.f[0] + t40 * c.r[0]), c.C[1], c.C[2] + 1000 * (c.f[2] + t40 * c.r[2])};
        const float p30[3] = {c.C[0] + 1000 * (c.f[0] + t30 * c.r[0]), c.C[1], c.C[2] + 1000 * (c.f[2] + t30 * c.r[2])};
        CHECK(!mk360::in_window(c.A, p40, 1.0f) && mk360::in_window(c.A, p30, 1.0f) && !mk360::in_window(c.A, p30, 0.92f));
    }
    CHECK(near(mk360::kTanPerGsX * 7.5f, 0.018023f, 1e-5));   // a 15 GS-unit box = the game's ~0.0185 tan half size (step62)
}

// A wingman: identified in the window, then the player turns away: the same box (colour, label, size) at its true position, any direction.
static void test_wingman_all_around(int mode) {
    mk360::Tracker tk;
    mk360::Params p; p.mode = mode;
    const float wing[3] = {3000, 1100, -20000};   // ahead (yaw 0 looks along -Z), slightly right and up
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    double t = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0);
        tac::TacFrame f = frame_for(c);
        add_obj(f, wing);
        float part[3] = {wing[0] + 20, wing[1], wing[2] - 15}; add_obj(f, part);   // a second part of the same aircraft: one contact
        CHECK(add_sprite(f, wing, kBlue, 4, mode == 1) == 0);
        mk360::update(tk, f, c.C, t, p);
        mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(tk.tracks.size() == 1 && tk.tracks[0].in_window && tk.tracks[0].sprite == 0);
        CHECK(tk.tracks[0].ident == (k >= 1));   // confirmed after 2 consecutive same-colour matches
        if (mode == 1) {   // the game's sprite is hidden; ours on its ray, at the contact's distance, game colour + FRND + game size
            CHECK(out.size() == 1 && fc.sprite == 1 && fc.out == 0);
            CHECK(out[0].label == tac::label_frnd && near(out[0].rgb[2], 1.0, 0) && near(out[0].rgb[0], 0.21, 1e-6));
            CHECK(near(out[0].half_tan_x, 7.5 * mk360::kTanPerGsBox, 1e-6) && near(out[0].half_tan_y, 7.5 * mk360::kTanPerGsBox, 1e-6));   // square: one angular scale
            CHECK(dist3(out[0].pos, tk.tracks[0].pos) < 30.0f);   // the sprite ray at the contact's range lands on the contact (+-the part centroid)
        } else CHECK(out.empty() && fc.skipped_in_window == 0);    // mode 2: the game's own box is drawn in the window, not ours
    }
    // the player turns 90 / 180 / 270 deg (the game camera = the aircraft nose; the head looks anywhere): no game box, ours stays at the true position
    for (float yaw : {90.0f, 180.0f, 270.0f}) {
        Cam c = cam_yaw(yaw);
        tac::TacFrame f = frame_for(c);
        add_obj(f, wing);
        mk360::update(tk, f, c.C, t, p); t += 1.0 / 60;
        mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(!tk.tracks[0].in_window && tk.tracks[0].ident);
        CHECK(out.size() == 1 && fc.out == 1 && fc.sprite == 0);
        CHECK(dist3(out[0].pos, wing) < 1e-3f && out[0].label == tac::label_frnd && near(out[0].rgb[1], 0.23, 1e-6) && !out[0].coasting);
        CHECK(near(out[0].half_tan_x, 7.5 * mk360::kTanPerGsBox, 1e-6));
        // drawn for an eye looking at it (head turned towards the contact behind the nose): it lands in the eye centre
        tac::EyeView ev; ev.ok = true;
        float fw[3] = {wing[0] - c.C[0], wing[1] - c.C[1], wing[2] - c.C[2]};
        const float l = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
        for (float& v : fw) v /= l;
        float rt[3] = {-fw[2], 0, fw[0]}; const float rl = std::sqrt(rt[0] * rt[0] + rt[2] * rt[2]); rt[0] /= rl; rt[2] /= rl;
        const float up[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0]};
        for (int i = 0; i < 3; ++i) { ev.clip[i] = rt[i] / 0.9f; ev.clip[4 + i] = up[i] / 0.9f; ev.clip[12 + i] = fw[i]; ev.eye[i] = c.C[i]; ev.up[i] = up[i]; }
        float u, v;
        CHECK(tac::eye_uv(ev, out[0].pos, u, v) && near(u, 0.5, 1e-3) && near(v, 0.5, 1e-3));
        tac::Overlay ov; tac::begin_slot(ov, tac::slot_eye0_post);
        CHECK(tac::build_marker(ov, ev, out[0]));
        tac::end_slot(ov, tac::slot_eye0_post);
        CHECK(ov.count[tac::slot_eye0_post] == (4 + 3 + 7 + 3 + 6) * 6);   // box (4 sides) + "FRND" strokes (F 3, R 7, N 3, D 6), 2 triangles each
        // the same marker for an eye looking along the nose (contact behind it): not drawn
        tac::EyeView fwd_eye = ev;
        for (int i = 0; i < 3; ++i) { fwd_eye.clip[i] = c.r[i]; fwd_eye.clip[4 + i] = c.u[i]; fwd_eye.clip[12 + i] = c.f[i]; }
        if (yaw == 180.0f) CHECK(!tac::eye_uv(fwd_eye, out[0].pos, u, v));
    }
    // killed / no longer reported out of the window: its world draws stop -> not drawn THIS frame, track gone after lost_s
    {
        Cam c = cam_yaw(180);
        tac::TacFrame f = frame_for(c);
        mk360::update(tk, f, c.C, t, p); t += 1.0 / 60;
        mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(out.empty());
        t += 0.3;
        tac::TacFrame f2 = frame_for(c);
        mk360::update(tk, f2, c.C, t, p);
        CHECK(tk.tracks.empty() && tk.st.lost == 1 && tk.st.lost_ident == 1);
    }
}

// A target killed IN VIEW: the wreck is still drawn but the game stops boxing it -> identity dropped after 9 frames / 140 ms (LOCK-FX: a blinking lock box survives); looking away shows nothing.
static void test_kill_in_view() {
    mk360::Tracker tk; mk360::Params p; p.mode = 1;
    const float tgt[3] = {-2000, 900, -15000};
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    double t = 0;
    Cam c = cam_yaw(0);
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        tac::TacFrame f = frame_for(c); add_obj(f, tgt); add_sprite(f, tgt, kRed, 3);
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(out.size() == 1 && out[0].label == tac::label_tgt);
    }
    CHECK(tk.tracks[0].ident);
    for (int k = 0; k < 12; ++k, t += 1.0 / 60) {
        tac::TacFrame f = frame_for(c); add_obj(f, tgt);   // wreck drawn, no box
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(out.empty());   // in the window only the game's boxes of THIS frame count: gone at once
        CHECK(tk.tracks[0].ident == (k < 9));   // frames 0..8 accumulate the miss (>= 9 frames and >= 140 ms at frame 9)
    }
    CHECK(tk.st.ident_cleared_in_view == 1);
    Cam back = cam_yaw(180);
    tac::TacFrame f = frame_for(back); add_obj(f, tgt);
    mk360::update(tk, f, back.C, t, p); mk360::collect(tk, f, back.C, p, out, fc);
    CHECK(out.empty() && !tk.tracks[0].in_window);   // behind now, no identity: nothing
}

// Unread box draws (the game's sprite replayed): no in-view box of ours and no miss counted; one-frame colour mismatches never stick.
static void test_unread_and_mismatch() {
    mk360::Tracker tk; mk360::Params p; p.mode = 1;
    const float a[3] = {1000, 1000, -12000};
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    double t = 0;
    Cam c = cam_yaw(0);
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kBlue, 4); mk360::update(tk, f, c.C, t, p); }
    CHECK(tk.tracks[0].ident);
    // 10 frames whose box draws were not read: the game's sprite is replayed (not suppressed) -> we draw nothing in the window, identity kept
    for (int k = 0; k < 10; ++k, t += 1.0 / 60) {
        tac::TacFrame f = frame_for(c); add_obj(f, a); f.box_unreadable = 1; f.box_draws = 1;
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(out.empty() && tk.tracks[0].ident);
    }
    // a sprite that was read but NOT suppressed (cap): the game draws it, we do not
    { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kBlue, 4, false); mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
      CHECK(out.empty()); t += 1.0 / 60; }
    // one red frame on the blue contact: drawn red in the window (that frame's game sprite) but the identity stays blue
    { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kRed, 3); mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
      CHECK(out.size() == 1 && near(out[0].rgb[0], 1.0, 0) && near(out[0].rgb[2], 0.0, 0)); t += 1.0 / 60; }
    CHECK(near(tk.tracks[0].rgb[2], 1.0, 0) && tk.tracks[0].glyphs == 4);
    // consecutive yellow frames (the game changed the box colour, e.g. a lock): the identity follows after recolour_confirm (6) of them (section 17.2:
    // 2 frames of a crossed box no longer recolour an identified contact)
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kYellow, 0); mk360::update(tk, f, c.C, t, p); }
    CHECK(near(tk.tracks[0].rgb[2], 1.0, 0) && tk.st.ident_recolour == 0 && tk.st.recolour_held == 1);
    for (int k = 2; k < 6; ++k, t += 1.0 / 60) { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kYellow, 0); mk360::update(tk, f, c.C, t, p); }
    CHECK(near(tk.tracks[0].rgb[1], 0.85, 1e-6) && tk.tracks[0].glyphs == 0 && tk.st.ident_recolour == 1);
    // and out of the window it is the yellow unlabeled box (no label is ever invented)
    Cam side = cam_yaw(90);
    tac::TacFrame f = frame_for(side); add_obj(f, a); mk360::update(tk, f, side.C, t, p); mk360::collect(tk, f, side.C, p, out, fc);
    CHECK(out.size() == 1 && out[0].label == tac::label_none && near(out[0].rgb[1], 0.85, 1e-6));
}

// Never boxed contacts: not drawn (no identity); mk360u:1 = dim grey for moving ones outside the window. Self draws never become contacts.
static void test_unknown_and_self() {
    for (bool unknown : {false, true}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 1; p.unknown = unknown;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        Cam c = cam_yaw(0);
        double t = 0;
        for (int k = 0; k < 12; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c);
            const float bandit[3] = {0, 1000, 20000.0f - 4.0f * float(k)};   // behind, moving 240 u/s
            const float house[3] = {5000, 0, 8000};                          // behind, static
            const float self[3] = {c.C[0] + 10, c.C[1] - 5, c.C[2] - 20};    // the player's own aircraft
            add_obj(f, bandit); add_obj(f, house); add_obj(f, self);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        }
        CHECK(tk.tracks.size() == 2 && tk.st.self_draws == 12);
        if (!unknown) CHECK(out.empty());
        else { CHECK(out.size() == 1 && fc.unknown == 1 && out[0].coasting && out[0].label == tac::label_none && near(out[0].rgb[0], 0.6, 1e-6)); }
    }
}

// PROV (headset test 7: 2 of 5 contacts never got a game box, so mode 3 drew nothing for them): moving aircraft-like contacts never boxed by the game and
// within its box range get one provisional TGT-green unlabeled box in every mode; static ones, missiles (too fast), far ones (> 31,000 u) and any contact
// with prov off get none; a contact the game boxed once (ever_ident) never does.
static void test_provisional_box() {
    for (int mode : {1, 2, 3}) for (bool prov : {false, true}) {
        mk360::Tracker tk; mk360::Params p; p.mode = mode; p.prov = prov;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        Cam c = cam_yaw(0);
        double t = 0;
        for (int k = 0; k < 12; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c);
            const float bandit[3] = {0, 1000, 20000.0f - 4.0f * float(k)};     // behind, 240 u/s: aircraft
            const float house[3] = {5000, 0, 8000};                            // static
            const float missile[3] = {-6000, 0, 20000.0f - 60.0f * float(k)};  // 3600 u/s: too fast
            const float far_ac[3] = {9000, 0, 40000.0f - 4.0f * float(k)};     // 240 u/s but beyond the game's box range
            add_obj(f, bandit); add_obj(f, house); add_obj(f, missile); add_obj(f, far_ac);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        }
        CHECK(tk.tracks.size() == 4);
        if (!prov) { CHECK(out.empty() && fc.prov == 0); continue; }
        CHECK(fc.prov == 1);
        bool found = false;
        for (const tac::WorldMarker& w : out) if (w.label == tac::label_none && near(w.rgb[0], 0.29, 1e-5) && near(w.rgb[1], 1.0, 1e-5) && !w.coasting) found = true;
        CHECK(found && out.size() == 1 && tk.st.drawn_prov > 0);
        for (mk360::Track& tr : tk.tracks) tr.ever_ident = true;
        tac::TacFrame f = frame_for(c);
        const float b2[3] = {0, 1000, 20000.0f - 4.0f * 12};
        add_obj(f, b2); mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(fc.prov == 0);
    }
    // headset test 9 (section 22): PROV is opt-in (mk360p:1); the default leaves the tac-config line unchanged
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;mk360:3", rest, o, why) && !o.mk360_prov && !mk360::params_from(o).prov && tac::describe(o).find("mk360p") == std::string::npos);
    CHECK(tac::extract_options("m02a;mk360:3;mk360p:1", rest, o, why) && o.mk360_prov && mk360::params_from(o).prov && rest == "m02a" && tac::describe(o).find(";mk360p=1") != std::string::npos);
    tac::Options o0;
    CHECK(tac::extract_options("m02a;mk360:3;mk360p:0", rest, o0, why) && !o0.mk360_prov && rest == "m02a" && tac::describe(o0).find("mk360p") == std::string::npos);
    CHECK(!tac::extract_options("m02a;mk360p:x", rest, o, why) && why == "bad value for mk360p");
}


// ---------------------------------------------------------------------------------------------------------------- IFF for provisional boxes (section 20)
// Team sources (evidence D:cz-evidence-archive, markers-360.md section 20): the game's box colour on the contact (blue = friendly), sustained formation
// with the player (range <= form_range and relative speed <= form_rel for form_min_s), else presumed enemy; a contact in formation but not yet confirmed
// is "unknown" (neutral grey, or skipped with mk360pu:0).
static Cam cam_at(const float C[3]) { Cam c = cam_yaw(0); for (int i = 0; i < 3; ++i) c.C[i] = C[i]; make_A(c.C, c.r, c.u, c.f, c.A); return c; }
static bool rgb_is(const tac::WorldMarker& w, const float rgb[3]) { return near(w.rgb[0], rgb[0], 0.02) && near(w.rgb[1], rgb[1], 0.02) && near(w.rgb[2], rgb[2], 0.02); }
static const float kProvGreen[3] = {0.29f, 1.0f, 0.22f};
// The player flies -Z at `speed`; a contact keeps `rel` (right, up, ahead-negative z) from it, same velocity; steps `secs` seconds at 60 Hz.
struct Sim {
    mk360::Tracker tk; mk360::Params p; std::vector<tac::WorldMarker> out; mk360::FrameCounts fc; double t = 0; float C[3] = {0, 1000, 0};
    Sim(int mode = 3) { p.mode = mode; p.prov = true; }
    void step(const float rel[3], float speed, const float* sprite_rgb = nullptr) {
        C[2] -= speed / 60.0f;
        Cam c = cam_at(C);
        tac::TacFrame f = frame_for(c);
        const float q[3] = {C[0] + rel[0], C[1] + rel[1], C[2] + rel[2]};
        add_obj(f, q);
        if (sprite_rgb) add_sprite(f, q, sprite_rgb, 4);
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        t += 1.0 / 60;
    }
};
// A wingman the game never boxed, flying formation for 40 s: a BLUE (friendly) box, never the enemy green.
static void test_iff_formation_friend() {
    Sim s;
    const float rel[3] = {300, -50, -400};
    for (int k = 0; k < 40 * 60; ++k) s.step(rel, 1100.0f);
    CHECK(s.out.size() == 1 && s.out[0].label == tac::label_none);
    CHECK(rgb_is(s.out[0], kBlue));
    CHECK(!rgb_is(s.out[0], kProvGreen));
}
// A friend the game boxed (blue) and then stopped boxing in view (the "hit or miss" miss): a blue provisional box. An enemy the game boxed and stopped
// boxing (kill / retarget) gets none (as before).
static void test_iff_learned_team() {
    for (int enemy = 0; enemy < 2; ++enemy) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        Cam c = cam_yaw(0);
        double t = 0;
        for (int k = 0; k < 230; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c);
            const float q[3] = {100.0f + 3.0f * float(k), 1000, -8000};   // 180 u/s across the view, inside the window
            add_obj(f, q);
            if (k < 30) add_sprite(f, q, enemy ? kRed : kBlue, enemy ? 3 : 4);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
            if (k == 20) CHECK(out.empty());   // identified and boxed by the game: nothing of ours
            if (k == 100) CHECK(out.empty());  // section 21: the 2 s hold after the game's last box (a blink / pending identity never shows ours beside it)
        }
        CHECK(tk.st.ident_cleared_in_view == 1);
        if (enemy) CHECK(out.empty() && fc.prov == 0);
        else { CHECK(out.size() == 1 && fc.prov == 1 && rgb_is(out[0], kBlue) && out[0].label == tac::label_none); }
    }
}

// A contact in formation for < 30 s is still UNKNOWN: neutral grey (default) or no box (mk360pu:0); after 30 s a friend; the counters and the iff line say so.
static void test_iff_unknown_candidate() {
    for (int policy : {1, 0}) {
        Sim s; s.p.prov_unknown = uint8_t(policy);
        float rel[3] = {300, -50, -400};
        rel[0] = 1500; rel[1] = -50; rel[2] = -1000;   // first seen beyond form_first_range (1,200 u): the slow 30 s formation rule (section 21: nearer = quick latch)
        for (int k = 0; k < 10 * 60; ++k) s.step(rel, 1100.0f);   // 10 s: candidate, not confirmed
        if (policy) { CHECK(s.out.size() == 1 && s.fc.prov_unknown == 1 && rgb_is(s.out[0], s.p.prov_unknown_rgb) && s.out[0].label == tac::label_none); }
        else { CHECK(s.out.empty() && s.fc.prov == 0 && s.tk.st.prov_unknown_skipped > 100); }
        CHECK(s.tk.st.form_confirmed == 0);
        for (int k = 0; k < 30 * 60; ++k) s.step(rel, 1100.0f);   // 40 s: confirmed
        CHECK(s.tk.st.form_confirmed == 1 && s.out.size() == 1 && s.fc.prov_friend == 1 && rgb_is(s.out[0], kBlue));
        const std::string l = mk360::iff_line(s.tk, s.p, 5);
        CHECK(l.find("event=summary-mk360-iff;t-ms=5;") != std::string::npos && l.find("formation-confirmed=1;now=friend:1,enemy:0,unknown:0;cam-v=ok") != std::string::npos);
        CHECK(l.find(policy ? "unknown-policy=neutral" : "unknown-policy=skip") != std::string::npos && l.size() < 300);
    }
}
// Presumed enemy: a head-on contact (closing 2,200 u/s) never forms a formation candidate: the enemy green from its first frame, no unknown box;
// a contact alongside at a relative speed above form_rel is an enemy too (a formation needs the SAME velocity, not just range).
static void test_iff_presumed_enemy() {
    Sim s;
    unsigned unknown_frames = 0, enemy_frames = 0;
    for (int k = 0; k < 600; ++k) {
        s.C[2] -= 1100.0f / 60;
        Cam c = cam_at(s.C);
        tac::TacFrame f = frame_for(c);
        const float q[3] = {0, 1000, -30000.0f + 1100.0f * float(s.t)};   // flies +Z at 1100 u/s: closing 2200 u/s
        add_obj(f, q);
        mk360::update(s.tk, f, c.C, s.t, s.p); mk360::collect(s.tk, f, c.C, s.p, s.out, s.fc);
        s.t += 1.0 / 60;
        unknown_frames += s.fc.prov_unknown; enemy_frames += s.fc.prov_enemy;
        if (!s.out.empty()) CHECK(rgb_is(s.out[0], kProvGreen));
    }
    CHECK(unknown_frames == 0 && enemy_frames > 300 && s.tk.st.form_confirmed == 0);
    Sim w;   // alongside, 600 u/s faster than the player
    const float rel[3] = {500, 0, -300};
    for (int k = 0; k < 20 * 60; ++k) {
        w.C[2] -= 1100.0f / 60;
        Cam c = cam_at(w.C);
        tac::TacFrame f = frame_for(c);
        const float q[3] = {w.C[0] + rel[0], w.C[1], w.C[2] + rel[2] - 600.0f * float(w.t)};
        add_obj(f, q);
        mk360::update(w.tk, f, c.C, w.t, w.p); mk360::collect(w.tk, f, c.C, w.p, w.out, w.fc);
        w.t += 1.0 / 60;
    }
    CHECK(w.tk.st.form_confirmed == 0 && w.tk.st.prov_friend == 0);
}
// The game's own colour beats formation: a formation friend the game then boxes RED becomes an enemy (team from the colour) and, once the game stops
// boxing it (kill / retarget), draws nothing.
static void test_iff_box_overrides_formation() {
    Sim s;
    const float rel[3] = {200, 0, -600};
    for (int k = 0; k < 40 * 60; ++k) s.step(rel, 1100.0f);
    CHECK(s.out.size() == 1 && rgb_is(s.out[0], kBlue) && s.tk.tracks.size() == 1 && s.tk.tracks[0].form_friend && s.tk.tracks[0].team == tac::team_unknown);
    for (int k = 0; k < 20; ++k) s.step(rel, 1100.0f, kRed);
    CHECK(s.tk.tracks[0].ident && s.tk.tracks[0].team == tac::team_enemy && s.out.empty());
    for (int k = 0; k < 40; ++k) s.step(rel, 1100.0f);
    CHECK(s.tk.st.ident_cleared_in_view == 1 && s.tk.tracks[0].ever_ident && s.out.empty() && s.fc.prov == 0);
}
// No camera velocity = unknown, never a guess: a teleport invalidates it, and a contact seen while it is unknown is neutral, not an enemy.
static void test_iff_camera_velocity() {
    Sim s;
    const float far_rel[3] = {0, 0, -20000};
    for (int k = 0; k < 14; ++k) s.step(far_rel, 600.0f);
    CHECK(s.tk.cam_v_ok);
    s.C[0] += 50000.0f;   // a cut: the camera jumps 50 km in one frame
    s.step(far_rel, 600.0f);
    CHECK(!s.tk.cam_v_ok);
    mk360::Tracker tk2; mk360::Params p2; p2.mode = 3; p2.prov = true;   // update() gets no camera pointer: no camera velocity
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    Cam c = cam_yaw(0); double t = 0;
    for (int k = 0; k < 30; ++k, t += 1.0 / 60) {
        tac::TacFrame f = frame_for(c);
        const float q[3] = {0, 1000, -20000.0f + 4.0f * float(k)};
        add_obj(f, q);
        mk360::update(tk2, f, nullptr, t, p2); mk360::collect(tk2, f, c.C, p2, out, fc);
    }
    CHECK(!tk2.cam_v_ok && fc.prov_unknown == 1 && fc.prov_enemy == 0 && out.size() == 1 && rgb_is(out[0], p2.prov_unknown_rgb));
}
static void test_iff_options() {
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;mk360:3", rest, o, why) && o.mk360_prov_unknown == 1 && mk360::params_from(o).prov_unknown == 1);
    CHECK(tac::describe(o).find("mk360pu") == std::string::npos);   // default: the line is unchanged
    CHECK(tac::extract_options("m02a;mk360:3;mk360p:1;mk360pu:0", rest, o, why) && rest == "m02a" && o.mk360_prov_unknown == 0 && mk360::params_from(o).prov_unknown == 0);
    CHECK(tac::describe(o).find(";mk360pu=0") != std::string::npos);
    CHECK(!tac::extract_options("m02a;mk360pu:2", rest, o, why) && why == "bad value for mk360pu");
    CHECK(!tac::extract_options("m02a;mk360pu:x", rest, o, why) && why == "bad value for mk360pu");
}

// Two contacts in the window, crossing sprites: the producer's per-draw match wins; each sprite -> one contact; each contact -> one box; no doubles
// across mode 1/2 for many frames with the camera sweeping.
static void test_one_box_per_contact() {
    for (int mode : {1, 2}) {
        mk360::Tracker tk; mk360::Params p; p.mode = mode;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        const float a[3] = {500, 1000, -20000}, b[3] = {700, 1000, -20000};   // ~0.6 deg apart: both inside each other's tolerance
        double t = 0;
        unsigned in_frames = 0, out_frames = 0;
        for (int k = 0; k < 400; ++k, t += 1.0 / 60) {
            const float yaw = -20.0f + 0.9f * float(k);   // the nose sweeps -20 .. +340 deg: both contacts leave the window and come back
            Cam c = cam_yaw(yaw);
            tac::TacFrame f = frame_for(c);
            add_obj(f, a); add_obj(f, b);
            const bool ina = mk360::in_window(c.A, a, 1.0f), inb = mk360::in_window(c.A, b, 1.0f);
            // the game boxes what is in its window; its per-draw match points each sprite at its own draw (obj = 1 + index)
            if (ina) { const int s = add_sprite(f, a, kBlue, 4, mode == 1); f.mark[s].obj = 1; }
            if (inb) { const int s = add_sprite(f, b, kRed, 3, mode == 1); f.mark[s].obj = 2; }
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
            CHECK(tk.tracks.size() == 2);
            CHECK(out.size() <= 2);   // never more boxes than contacts, never two for one contact
            for (size_t i = 0; i < out.size(); ++i) for (size_t j = i + 1; j < out.size(); ++j) CHECK(out[i].id != out[j].id);
            if (k >= 2) {
                if (mode == 1) CHECK(out.size() == 2);                                         // always both, any nose direction
                else CHECK(out.size() == unsigned(!ina) + unsigned(!inb));                    // mode 2: the game's inside, ours outside
                for (const auto& m : out) {   // identity never swapped: a is the blue FRND, b the red TGT, each at its own place
                    if (m.label == tac::label_frnd) CHECK(near(m.rgb[2], 1.0, 0) && dist3(m.pos, a) < dist3(m.pos, b));
                    else CHECK(m.label == tac::label_tgt && near(m.rgb[0], 1.0, 0) && dist3(m.pos, b) < dist3(m.pos, a));
                }
                for (const auto& tr : tk.tracks) CHECK(tr.ident);
                if (ina) ++in_frames; else ++out_frames;
            }
        }
        CHECK(in_frames > 50 && out_frames > 250);
        CHECK(tk.st.producer_matched > 0 && tk.st.ident_recolour == 0);
    }
}

// The edge margin: an identified contact in the outer 8 % of the window without a box keeps its identity (the game may clip edge boxes).
static void test_edge_margin() {
    mk360::Tracker tk; mk360::Params p; p.mode = 1;
    Cam c = cam_yaw(0);
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    const float tn = 0.6152f * 0.96f;   // 96 % of the half window to the right
    const float a[3] = {c.C[0] + 10000 * tn, c.C[1], -10000};
    double t = 0;
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kBlue, 4); mk360::update(tk, f, c.C, t, p); }
    CHECK(tk.tracks[0].ident && tk.tracks[0].in_window && !tk.tracks[0].in_inner);
    for (int k = 0; k < 20; ++k, t += 1.0 / 60) { tac::TacFrame f = frame_for(c); add_obj(f, a); mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc); }
    CHECK(tk.tracks[0].ident && out.empty());
    // the nose turns left 10 deg: the contact leaves the window, ours appears where it is
    Cam l = cam_yaw(-10);
    tac::TacFrame f = frame_for(l); add_obj(f, a); mk360::update(tk, f, l.C, t, p); mk360::collect(tk, f, l.C, p, out, fc);
    CHECK(out.size() == 1 && dist3(out[0].pos, a) < 1e-3f);
}

// The log lines carry the counters the live check reads.
static void test_logs() {
    mk360::Tracker tk; mk360::Params p; p.mode = 1;
    Cam c = cam_yaw(0);
    const float a[3] = {0, 1000, -10000};
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    for (int k = 0; k < 3; ++k) { tac::TacFrame f = frame_for(c); add_obj(f, a); add_sprite(f, a, kBlue, 4); mk360::update(tk, f, c.C, k / 60.0, p); mk360::collect(tk, f, c.C, p, out, fc); }
    tac::Options o; o.mk360 = 1;
    const std::string s = mk360::summary_line(tk, o, 5000);
    CHECK(s.find("event=summary-mk360;t-ms=5000;mode=1;") != std::string::npos);
    CHECK(s.find("sprites=3(suppressed:3,matched:3") != std::string::npos);
    CHECK(s.find("sprite-to-nearest-contact-gs=lt8:3,") != std::string::npos);
    CHECK(s.find("drawn=sprite:3,outside:0,unknown:0") != std::string::npos);
    CHECK(s.find("game-boxes=hidden(read-draws)-redrawn-at-rays") != std::string::npos);
    const std::string q = mk360::sample_line(tk, 5000);
    CHECK(q.find("event=mk360-contacts;t-ms=5000;contacts=1:win+box:r10000:b0.0:e0.0:") != std::string::npos);
    CHECK(q.find(":id0.21,0.23,1.00:g4") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------- MATCH-2 (markers-360.md section 9)
// Live run S1 (step83/runS1-logs, mk360:1, mission 02 T45..T70): 3 FRND wingmen in the game window (game-window shot T55: three blue FRND boxes),
// mk360-contacts t-ms=55023: 37:r20259:b-7.4:e11.5, 38:r20839:b-19.9:e11.2, 39:r15900:b0.6:e14.7; per 5 s: 300 frames, 1800 sprites (6 per frame = 2 per
// box), sprite-to-nearest-contact-gs lt16 = 1800 (every sprite 8..16 GS from a projected contact), matched 0. Sprites matched only in windows whose
// nearest fell below 8 (T40: lt8 205, matched 316): the old gate was ~8 GS = box half ~2.7 GS, i.e. the sprites are box PIECES, not 15-GS boxes.
static void s1_contact(float b_deg, float e_deg, float r, float p[3]) {
    const float b = b_deg * 3.14159265f / 180.0f, e = e_deg * 3.14159265f / 180.0f;
    p[0] = r * std::sin(b) * std::cos(e); p[1] = 1000.0f + r * std::sin(e); p[2] = -r * std::cos(b) * std::cos(e);
}
static void add_piece(tac::TacFrame& f, float cx, float cy, float h, const float rgb[3], uint8_t glyphs) {
    tac::MarkerSprite& m = f.mark[f.nmark++];
    m.cx = cx; m.cy = cy; m.hw = h; m.hh = h; m.glyphs = glyphs; m.suppressed = true;
    for (int i = 0; i < 3; ++i) { m.rgb[i] = rgb[i]; if (glyphs) m.lrgb[i] = rgb[i]; }   // the producer records the label's own colour
    if (glyphs) { m.lx0 = cx - 30; m.lx1 = cx - 8; m.ly0 = cy - 14; m.ly1 = cy - 9; }
}
// The old matcher (one sprite = one box, gate = half x 1.5 + 4, no offset, no grouping) through the same code: Params that switch MATCH-2 off.
static mk360::Params legacy_params() { mk360::Params p; p.mode = 1; p.group_gap = -1.0f; p.gate_min = 0.0f; p.goff_min = 0xffffffffu; p.seek = 0.0f; p.pair_gate = 0.0f; return p; }

// shape 0: each 15-GS box read as two corner pieces (half 2) at +-(7.5, 7.5) from the box centre = the projection (nearest 10.6: lt16, as logged)
// shape 1: one small piece (half 2) 11 GS ABOVE the projection (the box anchored above the object's draws): nearest 11 (lt16), same symptom
static void s1_frame(tac::TacFrame& f, const float pts[3][3], int shape) {
    for (int k = 0; k < 3; ++k) {
        add_obj(f, pts[k]);
        float xs, ys, w; tac::project_ps2(f.A, pts[k], xs, ys, w);
        if (shape == 0) { add_piece(f, xs - 7.5f, ys - 7.5f, 2.0f, kBlue, 4); add_piece(f, xs + 7.5f, ys + 7.5f, 2.0f, kBlue, 0); }
        else add_piece(f, xs, ys - 11.0f, 2.0f, kBlue, 4);
    }
}
static void test_s1_replay() {
    float pts[3][3];
    s1_contact(-7.4f, 11.5f, 20259.0f, pts[0]); s1_contact(-19.9f, 11.2f, 20839.0f, pts[1]); s1_contact(0.6f, 14.7f, 15900.0f, pts[2]);
    for (int shape : {0, 1}) {
        // the old rule reproduces the log: every sprite 8..16 GS from its contact, (almost) nothing matched, no identity
        {
            mk360::Tracker tk; const mk360::Params p = legacy_params();
            std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
            Cam c = cam_yaw(0);
            for (int k = 0; k < 300; ++k) { tac::TacFrame f = frame_for(c); s1_frame(f, pts, shape); mk360::update(tk, f, c.C, k / 60.0, p); mk360::collect(tk, f, c.C, p, out, fc); }
            CHECK(tk.st.sprites == (shape == 0 ? 1800u : 900u));
            CHECK(tk.st.near_lt16 == tk.st.sprites);
            CHECK(tk.st.sprites_matched == 0);
            for (const auto& tr : tk.tracks) CHECK(!tr.ident);
        }
        // MATCH-2: grouped / offset-corrected, every box matched from the first frames on, 3 FRND identities, kept outside the window
        mk360::Tracker tk; mk360::Params p; p.mode = 1;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        Cam c = cam_yaw(0);
        double t = 0;
        for (int k = 0; k < 300; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c); s1_frame(f, pts, shape);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
            if (k >= 2) {
                CHECK(out.size() == 3);                                                     // ONE box per contact (a split box drawn once)
                for (size_t i = 0; i < out.size(); ++i) for (size_t j = i + 1; j < out.size(); ++j) CHECK(out[i].id != out[j].id);
                for (const auto& m : out) CHECK(m.label == tac::label_frnd && near(m.rgb[2], 1.0, 0));
                if (shape == 0) for (const auto& m : out) CHECK(near(m.half_tan_x, 9.5 * mk360::kTanPerGsBox, 1e-5));   // the whole box's size
            }
        }
        CHECK(tk.st.sprites_matched >= tk.st.sprites - (shape == 0 ? 0u : 3u));   // shape 1: the first frame may precede the learned offset
        CHECK(tk.st.near_lt16 == tk.st.sprites);
        if (shape == 0) CHECK(tk.st.beyond_raw_gate == 0);                        // the GROUP centre is on the projection: the box-relative gate holds
        else CHECK(tk.st.beyond_raw_gate == tk.st.sprites_matched);              // only the wider / learned gate finds an 11-GS offset piece
        CHECK(tk.st.ident_cleared_in_view == 0 && tk.tracks.size() == 3);
        for (const auto& tr : tk.tracks) CHECK(tr.ident && tr.glyphs == 4);
        if (shape == 0) CHECK(tk.st.groups_multi == 900 && tk.st.groups == 900);
        if (shape == 1) CHECK(tk.goff_n >= p.goff_min && near(tk.goff[0], 0.0, 0.5) && near(tk.goff[1], -11.0, 0.5));
        // the nose turns 90 deg right: all three are behind the window's edge and keep their FRND boxes at their 3D positions
        Cam side = cam_yaw(90);
        tac::TacFrame f = frame_for(side);
        for (int k = 0; k < 3; ++k) add_obj(f, pts[k]);
        mk360::update(tk, f, side.C, t, p); mk360::collect(tk, f, side.C, p, out, fc);
        CHECK(out.size() == 3 && fc.out == 3);
        for (const auto& m : out) { CHECK(m.label == tac::label_frnd); float best = 1e30f; for (int k = 0; k < 3; ++k) best = std::fmin(best, dist3(m.pos, pts[k])); CHECK(best < 1e-3f); }
        const std::string ml = mk360::match_line(tk, 5000);
        CHECK(ml.find("event=summary-mk360-match;t-ms=5000;") != std::string::npos && ml.size() < 480);
        CHECK(ml.find(shape == 0 ? "sprite-half-gs=lt2:0,lt4:1800," : "sprite-half-gs=lt2:0,lt4:900,") != std::string::npos);
    }
}
// Two same-colour boxes side by side (30 GS apart) never merge into one group; two contacts, two boxes, both identified.
static void test_group_limits() {
    mk360::Tracker tk; mk360::Params p; p.mode = 1;
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    Cam c = cam_yaw(0);
    const float tn = 30.0f * mk360::kTanPerGsX;   // 30 GS to the right
    const float a[3] = {0, 1000, -10000}, b[3] = {10000 * tn, 1000, -10000};
    for (int k = 0; k < 5; ++k) {
        tac::TacFrame f = frame_for(c); add_obj(f, a); add_obj(f, b); add_sprite(f, a, kBlue, 4); add_sprite(f, b, kBlue, 4);
        mk360::update(tk, f, c.C, k / 60.0, p); mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(out.size() == 2 && out[0].id != out[1].id);
    }
    CHECK(tk.st.groups_multi == 0 && tk.st.sprites_matched == 10);
    for (const auto& tr : tk.tracks) CHECK(tr.ident);
}

// BOX-WARP: boxes are screen-aligned rectangles of the game's aspect, whatever the roll or the off-axis angle (user report "TGT boxes look warped").
static void test_box_not_warped() {
    // anamorphic fix: a 15 x 15 GS game box is SQUARE in angle; a piece pair union (wide) or an extreme ratio is clamped to 0.8..1.25
    { float tx, ty; mk360::box_half_tan(7.5f, 7.5f, tx, ty); CHECK(near(tx, ty, 1e-6) && near(tx, 0.0144, 0.0003));
      mk360::box_half_tan(14.0f, 5.0f, tx, ty); CHECK(near(tx / ty, 1.25, 1e-3));   // ratio clamp (area kept): 1.25
      mk360::box_half_tan(7.5f, 0.0f, tx, ty); CHECK(near(tx, tac::kMarkerHalfTan, 1e-6)); }
    const float roll_set[] = {0.0f, 0.5f, -0.5f, 1.2f};
    const float dirs[][2] = {{0, 0}, {0.5f, 0.2f}, {-0.4f, 0.9f}, {1.0f, -0.3f}, {0.1f, 1.1f}};   // yaw, pitch (rad) of the marker off the eye's axis
    for (float roll : roll_set) for (const auto& dp : dirs) {
        // the eye looks along -Z with its screen axes rolled by `roll`; its projection is the usual symmetric one (tan 0.9 / 0.9)
        const float cr = std::cos(roll), sr = std::sin(roll);
        const float fw[3] = {0, 0, -1}, rt[3] = {cr, sr, 0}, up[3] = {-sr, cr, 0};
        tac::EyeView ev; ev.ok = true;
        for (int i = 0; i < 3; ++i) { ev.clip[i] = rt[i] / 0.9f; ev.clip[4 + i] = up[i] / 0.9f; ev.clip[12 + i] = fw[i]; ev.eye[i] = 0; ev.up[i] = up[i]; ev.right[i] = rt[i]; ev.fwd[i] = fw[i]; }
        // the marker direction in the eye's own frame (yaw about up, pitch about right), 2000 units away
        const float cyw = std::cos(dp[0]), syw = std::sin(dp[0]), cp = std::cos(dp[1]), sp = std::sin(dp[1]);
        const float lx = syw * cp, ly = sp, lz = cyw * cp;   // right, up, forward components
        tac::WorldMarker m; m.label = tac::label_tgt; m.half_tan_x = 0.0144f; m.half_tan_y = 0.0144f; m.rgb[0] = 1;
        for (int i = 0; i < 3; ++i) m.pos[i] = 2000.0f * (rt[i] * lx + up[i] * ly + fw[i] * lz);
        tac::Overlay ov; tac::begin_slot(ov, tac::slot_eye0_post);
        CHECK(tac::build_marker(ov, ev, m));
        tac::end_slot(ov, tac::slot_eye0_post);
        // 4 box sides + TGT strokes: every stroke quad (6 vertices) must be an axis-aligned rectangle in NDC (the eye's screen), whatever the roll
        float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
        for (unsigned q = 0; q < 4; ++q) {   // the first 4 strokes = the box sides
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (unsigned k = 0; k < 6; ++k) { const tac::Vertex& v = ov.v[ov.first[tac::slot_eye0_post] + q * 6 + k]; x0 = std::fmin(x0, v.pos[0]); x1 = std::fmax(x1, v.pos[0]); y0 = std::fmin(y0, v.pos[1]); y1 = std::fmax(y1, v.pos[1]); }
            for (unsigned k = 0; k < 6; k += 1) {   // the quad's own 4 distinct corners lie on the bounding rectangle's corners (no shear, no rotation)
                const tac::Vertex& v = ov.v[ov.first[tac::slot_eye0_post] + q * 6 + k];
                CHECK((near(v.pos[0], x0, 1e-5) || near(v.pos[0], x1, 1e-5)) && (near(v.pos[1], y0, 1e-5) || near(v.pos[1], y1, 1e-5)));
            }
            bx0 = std::fmin(bx0, x0); bx1 = std::fmax(bx1, x1); by0 = std::fmin(by0, y0); by1 = std::fmax(by1, y1);
        }
        // the box: width / height (NDC) = tan ratio x the local scales; uniform scale (symmetric lens) -> square within 1 % when on-axis, and never flat
        const float w = bx1 - bx0, h = by1 - by0;
        CHECK(w > 0 && h > 0 && w / h > 0.85f && w / h < 1.2f);
        float cx_, cy_, sx, sy; CHECK(tac::marker_screen_frame(ev, m.pos, cx_, cy_, sx, sy));
        CHECK(near(w, 2 * 0.0144f * std::fabs(sx) + 0.0016f * std::fabs(sx), 1e-4) && near(h, 2 * 0.0144f * std::fabs(sy) + 0.0016f * std::fabs(sy), 1e-4));
        if (dp[0] == 0 && dp[1] == 0) CHECK(near(cx_, 0, 1e-5) && near(cy_, 0, 1e-5) && near(double(w) / h, 1.0, 0.02));
    }
}

// LOCK-FX (markers-360.md section 11): the game's per-frame lock appearance of an in-window box (colour flash, size change, blinking, lock text) is
// followed WITHOUT hysteresis (hysteresis is for the identity only); a blinking box does not lose its identity; lock / miss text is no FRND label.
static void test_lock_fx() {
    mk360::Tracker tk; mk360::Params p; p.mode = 1;
    const float tgt[3] = {2500, 1300, -15000};
    const float kLockOrange[3] = {1.0f, 0.4f, 0.0f};
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    double t = 0;
    Cam c = cam_yaw(0);
    auto step = [&](const float* rgb, float half, uint8_t glyphs, bool box) {
        tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        if (box) { const int m = add_sprite(f, tgt, rgb, glyphs); f.mark[m].hw = f.mark[m].hh = half; }
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc); t += 1.0 / 60;
    };
    for (int k = 0; k < 4; ++k) { step(kRed, 7.5f, 3, true); CHECK(out.size() == 1 && out[0].label == tac::label_tgt); }
    const float base_half = out[0].half_tan_x;
    CHECK(tk.tracks[0].ident);
    // a ONE-frame lock flash (orange, tighter box): drawn this frame as the game draws it; the identity keeps the red (a mismatch never sticks)
    step(kLockOrange, 5.0f, 3, true);
    CHECK(out.size() == 1 && near(out[0].rgb[1], 0.4, 1e-4) && out[0].half_tan_x < base_half * 0.75f && out[0].label == tac::label_tgt);
    CHECK(near(tk.tracks[0].rgb[1], 0.0, 1e-4));
    step(kRed, 7.5f, 3, true);
    CHECK(out.size() == 1 && near(out[0].rgb[1], 0.0, 1e-4) && near(out[0].half_tan_x, base_half, 1e-5));
    // a blinking box: off 6 frames (< 9) on the same contact, then on again: the game's rhythm is ours; identity survives
    for (int k = 0; k < 6; ++k) { step(nullptr, 0, 0, false); CHECK(out.empty() && tk.tracks[0].ident); }
    step(kRed, 7.5f, 3, true);
    CHECK(out.size() == 1);
    // the target leaves the window right after a blink: it keeps its (last confirmed) identity outside
    for (int k = 0; k < 6; ++k) step(nullptr, 0, 0, false);
    c = cam_yaw(180);
    { tac::TacFrame f = frame_for(c); add_obj(f, tgt); mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc); t += 1.0 / 60; }
    CHECK(out.size() == 1 && out[0].label == tac::label_tgt);
    // lock / miss text: 4 glyphs in a non-blue colour is NOT FRND; 5 glyphs (a range / name) no label; 4 glyphs blue = FRND
    c = cam_yaw(0);
    tk = mk360::Tracker{}; t = 0;
    step(kRed, 7.5f, 4, true);   CHECK(out.size() == 1 && out[0].label == tac::label_none);
    step(kRed, 7.5f, 5, true);   CHECK(out.size() == 1 && out[0].label == tac::label_none);
    step(kBlue, 7.5f, 4, true);  CHECK(out.size() == 1 && out[0].label == tac::label_frnd);
    // producer rule: only redrawable label text is hidden (n vertices = 4 per glyph)
    auto draw = [](uint32_t n, const float rgb[3]) { static tac::Cluster cl[1]; cl[0] = tac::Cluster{}; cl[0].n = n; for (int i = 0; i < 3; ++i) cl[0].rgb[i] = rgb[i] * float(n); return cl; };
    CHECK(tac::label_draw_redrawable(draw(16, kBlue), 1) && tac::label_draw_redrawable(draw(12, kRed), 1));   // FRND / TGT
    CHECK(!tac::label_draw_redrawable(draw(16, kRed), 1) && !tac::label_draw_redrawable(draw(20, kBlue), 1) && !tac::label_draw_redrawable(draw(8, kBlue), 1));
    CHECK(!tac::label_draw_redrawable(draw(16, kBlue), 0));
}

// ------------------------------------------------------------------------------------------------ NBOX (mk360:3, markers-360.md section 12)
static const float kGreen[3] = {0.29f, 1.0f, 0.22f};
// An eye at C looking along the unit direction fw (no roll), 0.9 tan half-fov (as test_wingman_all_around).
static tac::EyeView eye_towards(const float C[3], const float target[3]) {
    tac::EyeView ev; ev.ok = true;
    float fw[3] = {target[0] - C[0], target[1] - C[1], target[2] - C[2]};
    const float l = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    for (float& v : fw) v /= l;
    float rt[3] = {-fw[2], 0, fw[0]}; const float rl = std::sqrt(rt[0] * rt[0] + rt[2] * rt[2]); rt[0] /= rl; rt[2] /= rl;
    const float up[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0]};
    for (int i = 0; i < 3; ++i) { ev.clip[i] = rt[i] / 0.9f; ev.clip[4 + i] = up[i] / 0.9f; ev.clip[12 + i] = fw[i]; ev.eye[i] = C[i]; ev.up[i] = up[i]; ev.right[i] = rt[i]; ev.fwd[i] = fw[i]; }
    return ev;
}
static bool rect_overlap(const mk360::StampClear& a, const mk360::StampClear& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

static void test_nbox_geometry_and_labels() {
    // atlas: 2560x1440 game image: 5 px per GS across, 3.214 down; cell = +-64 x +-32 GS
    const mk360::AtlasGeom g = mk360::atlas_geom(2560, 1440);
    CHECK(g.ok && g.cell_w == 640 && g.cell_h == 206 && g.w == 3840 && g.h == 824);
    CHECK(mk360::atlas_geom(3840, 2160).ok && !mk360::atlas_geom(8000, 8000).ok && !mk360::atlas_geom(0, 0).ok);
    uint32_t x, y; mk360::cell_origin(g, 7, x, y); CHECK(x == 640 && y == 206);
    CHECK(near(mk360::px_of_gs_x(2048, 2560), 1280, 1e-9) && near(mk360::px_of_gs_y(2048 + 224, 1440), 1440, 1e-9));
    CHECK(near(mk360::gs_of_px_x(mk360::px_of_gs_x(1911.3, 2560), 2560), 1911.3, 1e-9) && near(mk360::gs_of_px_y(mk360::px_of_gs_y(2100.7, 1440), 1440), 2100.7, 1e-9));
    // producer (original-game reference item 14): a RED enemy-aircraft box with a GREEN 4-glyph type label "F-4E" at its upper right and a green
    // 4-digit range "3138" below-right: both labels join the red box (geometry, not colour), their union rectangle and the label colour are kept
    tac::TacFrame f;
    tac::MarkerSprite& red = f.mark[f.nmark++]; red.cx = 2000; red.cy = 2000; red.hw = red.hh = 7.5f; for (int i = 0; i < 3; ++i) red.rgb[i] = kRed[i];
    tac::MarkerSprite& far_box = f.mark[f.nmark++]; far_box.cx = 2150; far_box.cy = 2000; far_box.hw = far_box.hh = 7.5f; for (int i = 0; i < 3; ++i) far_box.rgb[i] = kGreen[i];
    tac::Cluster c[2];
    c[0].x0 = 2012; c[0].x1 = 2034; c[0].y0 = 1986; c[0].y1 = 1991; c[0].n = 16; for (int i = 0; i < 3; ++i) c[0].rgb[i] = kGreen[i] * 16;   // "F-4E"
    c[1].x0 = 2012; c[1].x1 = 2034; c[1].y0 = 2009; c[1].y1 = 2014; c[1].n = 16; for (int i = 0; i < 3; ++i) c[1].rgb[i] = kGreen[i] * 16;   // "3138"
    tac::add_labels(f, 0, c, 2);
    CHECK(f.mark[0].glyphs == 4 && f.mark[1].glyphs == 0);
    CHECK(near(f.mark[0].lx0, 2012, 0) && near(f.mark[0].lx1, 2034, 0) && near(f.mark[0].ly0, 1986, 0) && near(f.mark[0].ly1, 2014, 0));
    CHECK(near(f.mark[0].lrgb[1], 1.0, 1e-6) && f.mark[0].lrgb[2] < 0.3f);
    // mk360:1 fallback: FRND only from a BLUE label: a red (or green) box with a green 4-glyph label is NEVER FRND (headset test item 8)
    CHECK(mk360::label_of(4, f.mark[0].lrgb) == tac::label_none && mk360::label_of(4, kBlue) == tac::label_frnd && mk360::label_of(3, kGreen) == tac::label_tgt);
    // NBOX: the stamp of that red box holds the type label and the range (the game's own pixels: diamond, colours, text)
    {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        Cam cm = cam_yaw(0);
        tac::TacFrame fr = frame_for(cm);
        const float tgt[3] = {-1500, 1200, -18000};
        add_obj(fr, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(fr.A, tgt, xs, ys, w));
        tac::MarkerSprite& m = fr.mark[fr.nmark++]; m.cx = xs; m.cy = ys; m.hw = m.hh = 7.5f; for (int i = 0; i < 3; ++i) { m.rgb[i] = kRed[i]; m.lrgb[i] = kGreen[i]; }
        m.glyphs = 4; m.lx0 = xs + 12; m.lx1 = xs + 46; m.ly0 = ys - 14; m.ly1 = ys + 14;   // "F-4E" upper right, "3138" lower right
        mk360::update(tk, fr, cm.C, 0.0, p);
        mk360::StampPlan plan; mk360::StampCounts sc;
        mk360::plan_stamps(tk, fr, p, 2560, 1440, true, 0.0, plan, sc);
        CHECK(sc.live == 1 && plan.copy.size() == 1);
        CHECK(mk360::gs_of_px_x(plan.copy[0].x + plan.copy[0].w, 2560) >= xs + 48 && mk360::gs_of_px_x(plan.copy[0].x, 2560) <= xs - 9.5);
        CHECK(mk360::gs_of_px_y(plan.copy[0].y, 1440) <= ys - 16 && mk360::gs_of_px_y(plan.copy[0].y + plan.copy[0].h, 1440) >= ys + 16);
    }
}

// A wingman boxed in the window (game box + FRND label), then the nose turns away: in the window ONE live stamp (copied, cleared from the flat quad, drawn
// at its ray); outside the cached stamp at its 3D position from the same cell; nothing once its draws stop. mode 3 never makes synthetic boxes.
// Headset 2026-10-30: stamps (no depth test) of a contact near the eye floated inside the cockpit. Near policy: never nearer than near_min (pushed out
// along the eye ray: screen position unchanged, less parallax), smaller below near_ref (scale about the anchor), floor, knobs off = the old behaviour.
static float stamp_width(const mk360::StampPlan& plan, const tac::EyeView& ev, float* xc = nullptr) {
    tac::Overlay ov; tac::begin_slot(ov, tac::slot_eye0_stamp);
    const unsigned n = mk360::build_stamps(ov, ev, plan);
    tac::end_slot(ov, tac::slot_eye0_stamp);
    if (n != 1) return -1.0f;
    float x0 = 1e9f, x1 = -1e9f;
    for (const tac::Vertex& v : ov.v) { x0 = std::fmin(x0, v.pos[0]); x1 = std::fmax(x1, v.pos[0]); }
    if (xc) *xc = 0.5f * (x0 + x1);
    return x1 - x0;
}
static void test_nbox_near_policy() {
    mk360::StampPlan plan; plan.atlas = mk360::atlas_geom(2560, 1440);
    CHECK(plan.atlas.ok);
    const float C[3] = {0, 0, 0};
    plan.has_ref = true;   // the game camera centre = origin: both eyes share one pushed position
    auto draw_at = [&](float dist) {
        plan.draw.clear();
        mk360::StampDraw d; d.pos[0] = 0; d.pos[1] = 0; d.pos[2] = -dist; d.ox = -10; d.oy = -10; d.cell = 0; d.pw = 100; d.ph = 60; d.id = 1; d.live = true;
        plan.draw.push_back(d);
    };
    const float target[3] = {0, 0, -1};
    plan.near_min = 0; plan.near_ref = 0; plan.near_floor = 1;   // off: the width never depends on the distance (constant angular size)
    draw_at(20000); const float w_far = stamp_width(plan, eye_towards(C, target));
    draw_at(60);    const float w_off = stamp_width(plan, eye_towards(C, target));
    CHECK(w_far > 0 && near(w_far, w_off, 1e-6));
    plan.near_min = 150; plan.near_ref = 750; plan.near_floor = 0.3f;   // defaults: far unchanged, 375 u = half size, 100 u / 10 u = the floor
    draw_at(20000); CHECK(near(stamp_width(plan, eye_towards(C, target)), w_far, 1e-6));
    draw_at(750);   CHECK(near(stamp_width(plan, eye_towards(C, target)), w_far, 1e-6));
    draw_at(375);   CHECK(near(stamp_width(plan, eye_towards(C, target)), 0.5f * w_far, 1e-5));
    draw_at(100);   CHECK(near(stamp_width(plan, eye_towards(C, target)), 0.3f * w_far, 1e-5));
    draw_at(10);    CHECK(near(stamp_width(plan, eye_towards(C, target)), 0.3f * w_far, 1e-5));
    // stereo parallax of a stamp at 3 u (inside the canopy), one eye 0.16 u off the game camera: with the policy it equals that of 150 u
    tac::EyeView e0 = eye_towards(C, target), e1 = e0;
    e1.eye[0] += 0.16f;
    float xa = 0, xb = 0;
    plan.near_min = 0; plan.near_ref = 0; plan.near_floor = 1;
    draw_at(3); stamp_width(plan, e0, &xa); stamp_width(plan, e1, &xb);
    const float disparity_old = xa - xb;   // huge: the box floats in front of the cockpit
    plan.near_min = 150; plan.near_ref = 750; plan.near_floor = 0.3f;
    draw_at(3); stamp_width(plan, e0, &xa); stamp_width(plan, e1, &xb);
    const float disparity_new = xa - xb;
    CHECK(disparity_old > 20.0f * disparity_new && disparity_new > 0);
    CHECK(near(xa, 0.0f, 1e-5));   // the screen position (direction from the eye) is unchanged
    // off-axis anchor 67 u away: pushed to 150 u on the same eye ray
    const float tgt2[3] = {30, 10, -60};
    plan.draw.clear(); { mk360::StampDraw d; d.pos[0] = tgt2[0]; d.pos[1] = tgt2[1]; d.pos[2] = tgt2[2]; d.ox = 0; d.oy = 0; d.cell = 0; d.pw = 20; d.ph = 20; d.id = 2; plan.draw.push_back(d); }
    plan.near_ref = 0;   // push only (the scale would move the stamp centre)
    float xn = 0, xo = 0; stamp_width(plan, eye_towards(C, tgt2), &xn);
    plan.near_min = 0; plan.near_ref = 0; stamp_width(plan, eye_towards(C, tgt2), &xo);
    CHECK(near(xn, xo, 1e-5));
    // options: keys, range checks, describe unchanged by default
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;mk360:3;mk360min:200;mk360ref:1000;mk360floor:0.5", rest, o, why) && rest == "m02a" && o.mk360 == 3 &&
          o.mk360_near_min == 200.0f && o.mk360_near_ref == 1000.0f && o.mk360_near_floor == 0.5f);
    CHECK(tac::describe(o).find("mk360min=200;mk360ref=1000;mk360floor=0.50") != std::string::npos);
    const mk360::Params pp = mk360::params_from(o);
    CHECK(pp.near_min == 200.0f && pp.near_ref == 1000.0f && pp.near_floor == 0.5f);
    tac::Options d0; d0.mk360 = 3; CHECK(tac::describe(d0).find("mk360min") == std::string::npos);
    tac::Options b; CHECK(!tac::extract_options("m02a;mk360floor:0", rest, b, why) && !tac::extract_options("m02a;mk360min:-1", rest, b, why));
    // plan_stamps hands the knobs to build_stamps
    mk360::Tracker tk; mk360::Params p; p.mode = 3; p.near_min = 77; p.near_ref = 88; p.near_floor = 0.4f;
    mk360::StampPlan pl; mk360::StampCounts sc; tac::TacFrame f = frame_for(cam_yaw(0));
    mk360::plan_stamps(tk, f, p, 2560, 1440, true, 0.0, pl, sc);
    CHECK(pl.near_min == 77.0f && pl.near_ref == 88.0f && near(pl.near_floor, 0.4f, 1e-6));
}
// OFF-VIEW (docs/world-render/testhmd-first-flights.md section 5): a stamp whose anchor lies outside the eye's view draws nothing. Before, eye_clip_ only
// refused points behind the eye and the local Jacobian (1/cos^2) blew a friendly box 80-89 deg off the view axis up to a quad wider than the view: the
// step185 test-HMD flights showed whole-eye blue (runAGC1 f5494) and green (runAGB2 mirror m0169) washes during head turns.
static void test_nbox_off_view() {
    mk360::StampPlan plan; plan.atlas = mk360::atlas_geom(2560, 1440);
    CHECK(plan.atlas.ok);
    const float C[3] = {0, 0, 0}, ahead[3] = {0, 0, -1};
    const tac::EyeView ev = eye_towards(C, ahead);   // tan half-fov 0.9 (edge at 42 deg)
    auto draw_at_deg = [&](double deg) {
        plan.draw.clear();
        const double a = deg * 3.14159265358979 / 180.0;
        mk360::StampDraw d; d.pos[0] = float(20000.0 * std::sin(a)); d.pos[1] = 0; d.pos[2] = float(-20000.0 * std::cos(a));
        d.ox = -10; d.oy = -10; d.cell = 0; d.pw = 100; d.ph = 60; d.id = 1; d.live = true;
        plan.draw.push_back(d);
    };
    auto drawn = [&](mk360::Stats* st, float& w) {
        tac::Overlay ov; tac::begin_slot(ov, tac::slot_eye0_stamp);
        const unsigned n = mk360::build_stamps(ov, ev, plan, st);
        tac::end_slot(ov, tac::slot_eye0_stamp);
        float x0 = 1e9f, x1 = -1e9f;
        for (const tac::Vertex& v : ov.v) { x0 = std::fmin(x0, v.pos[0]); x1 = std::fmax(x1, v.pos[0]); }
        w = n ? x1 - x0 : 0.0f;
        return n;
    };
    mk360::Stats st; float w0 = 0, w = 0;
    draw_at_deg(0);  CHECK(drawn(&st, w0) == 1 && w0 > 0 && w0 < 0.5f);
    draw_at_deg(40); CHECK(drawn(&st, w) == 1 && w < 2.0f * w0);                      // inside the view (NDC 0.93): drawn, bounded size
    draw_at_deg(46); CHECK(drawn(&st, w) == 1);                                       // just past the edge (NDC 1.15): its visible part still drawn
    CHECK(st.nb_off_view == 0);
    draw_at_deg(55); CHECK(drawn(&st, w) == 0 && st.nb_off_view == 1);               // NDC 1.59: nothing visible, not drawn
    draw_at_deg(85); CHECK(drawn(&st, w) == 0 && st.nb_off_view == 2);               // the giant-box case (NDC 12.7, ~130x the on-axis scale)
    draw_at_deg(-89); CHECK(drawn(&st, w) == 0 && st.nb_off_view == 3);
    draw_at_deg(120); CHECK(drawn(&st, w) == 0 && st.nb_off_view == 3);              // behind the eye: refused before (eye_clip_), not counted here
    CHECK(mk360::hs6_line(mk360::Tracker{}, 0).find(";off-view=0") != std::string::npos);
}
static void test_nbox_wingman() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    const float wing[3] = {3000, 1100, -20000};
    mk360::StampPlan plan; mk360::StampCounts sc;
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    double t = 0;
    int cell = -1;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0);
        tac::TacFrame f = frame_for(c);
        add_obj(f, wing);
        CHECK(add_sprite(f, wing, kBlue, 4, false) == 0);   // nothing is hidden in mode 3
        mk360::update(tk, f, c.C, t, p);
        mk360::collect(tk, f, c.C, p, out, fc);
        CHECK(out.empty());   // no synthetic box ever
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(plan.fresh && plan.atlas.ok && sc.live == 1 && sc.cached == 0 && plan.draw.size() == 1 && plan.copy.size() == 1 && plan.clear.size() == 1);
        const mk360::StampCopy& cp = plan.copy[0]; const mk360::StampClear& cl = plan.clear[0];
        CHECK(cl.x0 == cp.x && cl.y0 == cp.y && cl.x1 == cp.x + cp.w && cl.y1 == cp.y + cp.h);   // exactly the copied pixels leave the flat quad
        // the stamp holds the whole box (+-7.5 GS) and the label (30 GS left, 14 GS up) plus the 2 GS margin
        const tac::MarkerSprite& s = f.mark[0];
        CHECK(mk360::gs_of_px_x(cp.x, 2560) <= s.cx - 32 && mk360::gs_of_px_x(cp.x + cp.w, 2560) >= s.cx + 9.5);
        CHECK(mk360::gs_of_px_y(cp.y, 1440) <= s.cy - 16 && mk360::gs_of_px_y(cp.y + cp.h, 1440) >= s.cy + 9.5);
        CHECK(cp.w <= plan.atlas.cell_w && cp.h <= plan.atlas.cell_h);
        const mk360::StampDraw& d = plan.draw[0];
        CHECK(d.live && d.id == tk.tracks[0].id && dist3(d.pos, tk.tracks[0].pos) < 30.0f);   // at its ray, at the contact's range
        CHECK(near(d.ox, mk360::gs_of_px_x(cp.x, 2560) - s.cx, 1e-3) && d.pw == cp.w && d.ph == cp.h);
        if (cell < 0) cell = d.cell;
        CHECK(d.cell == cell && tk.tracks[0].cell == cell && tk.tracks[0].cell_ok && tk.cell_owner[cell] == tk.tracks[0].id);   // its own cell, every frame
    }
    const uint16_t pw = plan.draw[0].pw, ph = plan.draw[0].ph;
    for (float yaw : {90.0f, 180.0f, 270.0f}) {
        Cam c = cam_yaw(yaw);
        tac::TacFrame f = frame_for(c);
        add_obj(f, wing);
        mk360::update(tk, f, c.C, t, p);
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.live == 0 && sc.cached == 1 && plan.copy.empty() && plan.clear.empty() && plan.draw.size() == 1);
        const mk360::StampDraw& d = plan.draw[0];
        CHECK(!d.live && d.cell == cell && d.pw == pw && d.ph == ph && dist3(d.pos, wing) < 1e-3f);
        // an eye looking at it: one quad (2 triangles), the box centre (stamp offset 0) on the eye centre, uv inside the contact's cell
        tac::EyeView ev = eye_towards(c.C, wing);
        tac::Overlay ov; tac::begin_slot(ov, tac::slot_eye0_stamp);
        CHECK(mk360::build_stamps(ov, ev, plan) == 1);
        tac::end_slot(ov, tac::slot_eye0_stamp);
        CHECK(ov.count[tac::slot_eye0_stamp] == 6);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (const tac::Vertex& v : ov.v) {
            x0 = std::fmin(x0, v.pos[0]); x1 = std::fmax(x1, v.pos[0]); y0 = std::fmin(y0, v.pos[1]); y1 = std::fmax(y1, v.pos[1]);
            u0 = std::fmin(u0, v.col[0]); u1 = std::fmax(u1, v.col[0]); v0 = std::fmin(v0, v.col[1]); v1 = std::fmax(v1, v.col[1]);
            CHECK(v.pos[3] == 1.0f && v.col[2] == 1.0f);
        }
        CHECK(x0 < 0 && x1 > 0 && y0 < 0 && y1 > 0);   // the anchor (the contact) is inside the stamp
        // GS -> NDC: the flat quad's own scale (kTanPerGsX/Y over the eye's NDC per tan = 1/0.9): the stamp is the game's size, not stretched
        const double gpx = 512.0 / 2560.0, gpy = 448.0 / 1440.0;
        CHECK(near(x1 - x0, (pw - 1) * gpx * mk360::kTanPerGsX / 0.9, 2e-4) && near(y1 - y0, (ph - 1) * gpy * mk360::kTanPerGsY / 0.9, 2e-4));
        uint32_t ux, uy; mk360::cell_origin(plan.atlas, uint16_t(cell), ux, uy);
        CHECK(near(u0, (ux + 0.5) / plan.atlas.w, 1e-6) && near(u1, (ux + pw - 0.5) / plan.atlas.w, 1e-6));   // texel centres: no bleed outside the stamp
        CHECK(near(v0, (uy + 0.5) / plan.atlas.h, 1e-6) && near(v1, (uy + ph - 0.5) / plan.atlas.h, 1e-6));
        // the eye looking the other way: nothing
        float away[3] = {2 * c.C[0] - wing[0], 2 * c.C[1] - wing[1], 2 * c.C[2] - wing[2]};
        tac::Overlay ov2; CHECK(mk360::build_stamps(ov2, eye_towards(c.C, away), plan) == 0 && ov2.v.empty());
    }
    // its world draws stop: nothing this frame
    {
        Cam c = cam_yaw(180);
        tac::TacFrame f = frame_for(c);
        mk360::update(tk, f, c.C, t, p);
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(plan.draw.empty() && plan.copy.empty());
    }
    // the logs carry the plan
    const std::string l = mk360::nbox_line(tk, plan, 1000);
    CHECK(l.find("event=summary-mk360-nbox;") != std::string::npos && l.find("live=3;cached=3;copies=3;") != std::string::npos && l.size() < 480);
    CHECK(l.find("atlas=3840x824(cell:640x206,img:2560x1440)") != std::string::npos);
    mk360::Tracker t0; tac::Options o; o.mk360 = 3;
    CHECK(mk360::summary_line(t0, o, 0).find("game-boxes=native-stamps") != std::string::npos);
}

// Boxes close together (overlapping stamp rectangles): after the cuts no marker-image pixel is in two stamps, each keeps its own box centre; the flat
// quad loses exactly the stamped pixels. A box clipped at the window edge never overwrites its contact's complete cached stamp.
static void test_nbox_exactly_once_and_edge() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float P[3][3] = {{0, 1000, -20000}, {600, 1000, -20000}, {300, 1400, -20000}};   // ~ 7.7 / 4.5 GS... spread: see projections below
    const float* cols[3] = {kRed, kGreen, kYellow};
    double t = 0;
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0);
        tac::TacFrame f = frame_for(c);
        for (int i = 0; i < 3; ++i) { add_obj(f, P[i]); CHECK(add_sprite(f, P[i], cols[i], 3, false) == i); }
        mk360::update(tk, f, c.C, t, p);
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.live == 3 && plan.clear.size() == 3 && sc.cuts >= 1);
        for (size_t i = 0; i < plan.clear.size(); ++i) for (size_t j = i + 1; j < plan.clear.size(); ++j) CHECK(!rect_overlap(plan.clear[i], plan.clear[j]));
        for (int i = 0; i < 3; ++i) {   // each stamp still contains its own box centre
            bool found = false;
            for (const mk360::StampClear& r : plan.clear) {
                const double bx = mk360::px_of_gs_x(f.mark[i].cx, 2560), by = mk360::px_of_gs_y(f.mark[i].cy, 1440);
                if (bx >= r.x0 && bx < r.x1 && by >= r.y0 && by < r.y1) found = true;
            }
            CHECK(found);
        }
        std::vector<uint16_t> cells; for (const mk360::StampCopy& cp : plan.copy) cells.push_back(cp.cell);
        CHECK(cells.size() == 3 && cells[0] != cells[1] && cells[1] != cells[2] && cells[0] != cells[2]);
    }
    // edge: the red contact moves to the window's right edge (its box clipped by the game): the live stamp uses a temporary cell, its complete cached stamp stays
    {   // w12-2 finding 2: the cut pictures above are not cached; a frame with the red contact alone caches its whole picture
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c);
        add_obj(f, P[0]); CHECK(add_sprite(f, P[0], kRed, 3, false) == 0);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.cuts == 0);
    }
    const int red_cell = tk.tracks[0].cell; const uint16_t red_pw = tk.tracks[0].c_pw;
    CHECK(red_cell >= 0 && tk.tracks[0].cell_ok);
    {
        Cam c = cam_yaw(0);
        tac::TacFrame f = frame_for(c);
        const float edge[3] = {20000 * 0.6152f * (254.0f / 256.0f) / 1.0f, 1000, -20000};   // projects ~2 GS inside the right edge: the 7.5-GS box is clipped
        add_obj(f, edge); CHECK(add_sprite(f, edge, kRed, 3, false) == 0);
        mk360::update(tk, f, c.C, t, p);
        const uint64_t kept = tk.st.nb_edge_kept;
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        // the track may be a new one if the jump exceeded the gate; either way no complete cached stamp is overwritten by a clipped one
        if (tk.st.nb_edge_kept == kept + 1) {
            CHECK(sc.live == 1 && plan.copy[0].cell != red_cell);
            for (const mk360::Track& tr : tk.tracks) if (tr.cell == red_cell) CHECK(tr.cell_ok && tr.c_pw == red_pw);
        } else for (const mk360::Track& tr : tk.tracks) if (tr.sprite >= 0) CHECK(!tr.cell_ok);   // a new contact: its clipped stamp is never cached
    }
}

// step111 live: 32% of the recordings counted "refused" were cached-only frames whose boxes were outside both eye views (empty overlay). Classified.
static void test_nbox_refusal_reasons() {
    using R = mk360::NboxRefuse;
    CHECK(mk360::classify_nbox_refusal(true, true, true, true, true) == R::none);
    CHECK(mk360::classify_nbox_refusal(false, true, false, false, false) == R::nothing_in_view);   // cached stamps, none in view: benign
    CHECK(mk360::classify_nbox_refusal(false, true, false, true, true) == R::nothing_in_view);     // copies fine, no eye vertex
    CHECK(mk360::classify_nbox_refusal(false, false, true, false, false) == R::no_atlas);          // defect
    CHECK(mk360::classify_nbox_refusal(false, true, true, true, false) == R::copy_failed);         // defect (invalidates cells)
    CHECK(mk360::classify_nbox_refusal(false, true, true, false, false) == R::pipeline);           // defect: overlay exists, stamps not drawn
}

// Copies not recorded -> the contact forgets the cell (no stale / foreign stamp); no marker image -> no live stamp (nothing cleared) but cached ones stay;
// a new image size forgets every cell; more contacts than cells evict the least recently drawn.
static void test_nbox_failures_and_eviction() {
    mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float wing[3] = {3000, 1100, -20000};
    {
        mk360::Tracker tk; double t = 0;
        for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
            Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, wing); add_sprite(f, wing, kBlue, 4, false);
            mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        }
        CHECK(tk.tracks[0].cell_ok);
        mk360::stamps_not_copied(tk, plan);
        CHECK(!tk.tracks[0].cell_ok && tk.st.nb_invalidated == 1);
        Cam c = cam_yaw(180); tac::TacFrame f = frame_for(c); add_obj(f, wing);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(plan.draw.empty());   // never drawn from a cell the backend did not fill
    }
    {
        mk360::Tracker tk; double t = 0;
        for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
            Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, wing); add_sprite(f, wing, kBlue, 4, false);
            mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        }
        // the marker image is not used this composite: no live stamp, nothing copied or cleared (the flat quad is not shown either)
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, wing); add_sprite(f, wing, kBlue, 4, false);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 0, 0, false, t, plan, sc); t += 1.0 / 60;
        CHECK(plan.draw.empty() && plan.copy.empty() && plan.clear.empty() && tk.st.nb_no_image == 1 && tk.tracks[0].cell_ok);
        // outside the window without the image: the cached stamp is still drawn
        Cam b = cam_yaw(180); tac::TacFrame fb = frame_for(b); add_obj(fb, wing);
        mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 0, 0, false, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.cached == 1 && plan.atlas.ok);
        // a new image size: every cell forgotten
        mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 1920, 1080, true, t, plan, sc);
        CHECK(plan.draw.empty() && !tk.tracks[0].cell_ok && tk.tracks[0].cell < 0 && plan.atlas.img_w == 1920);
    }
    {   // eviction: 14 identified contacts ahead, then the nose turns 180 deg and 14 NEW contacts are boxed: 10 free cells + 4 evicted (the old ones out of view)
        mk360::Tracker tk; double t = 0;
        std::vector<std::array<float, 3>> a, b;
        const float tx[5] = {-0.45f, -0.22f, 0.0f, 0.22f, 0.45f}, ty[3] = {-0.2f, 0.0f, 0.2f};
        for (int i = 0; i < 14; ++i) {
            const float X = tx[i % 5] * 20000, Y = 1000 + ty[i / 5] * 0.5f * 20000;
            a.push_back({X, Y, -20000}); b.push_back({-X, Y, 20000});
        }
        for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
            Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c);
            for (auto& q : a) { add_obj(f, q.data()); add_sprite(f, q.data(), kYellow, 3, false); }
            for (auto& q : b) add_obj(f, q.data());
            mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
            CHECK(sc.live == 14);
        }
        unsigned owned = 0; for (const mk360::Track& tr : tk.tracks) if (tr.cell_ok) ++owned;
        CHECK(owned == 14);
        for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
            Cam c = cam_yaw(180); tac::TacFrame f = frame_for(c);
            for (auto& q : a) add_obj(f, q.data());
            for (auto& q : b) { add_obj(f, q.data()); add_sprite(f, q.data(), kGreen, 3, false); }
            mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
            CHECK(sc.live == 14 && sc.no_cell == 0);
            if (k == 0) CHECK(tk.st.nb_evicted == 4 && sc.cached == 10);   // the other 10 old contacts keep their stamps outside the window
        }
        for (unsigned c = 0; c < mk360::kStampCells; ++c) {   // a cell is owned by at most one contact, and that contact points back at it
            unsigned users = 0; for (const mk360::Track& tr : tk.tracks) if (tr.cell == int(c)) ++users;
            CHECK(users <= 1);
        }
    }
}

// Codex J-203 finding 1 (review-w5-1-marker-stamp-lifetime.md): the fresh frame's EyeView once carried eye/up only (right/fwd zero) while the DECOUPLE
// re-composite's carried the whole frame; marker_screen_frame then measured an off-axis stamp's width along another axis (~1.4x wider fresh than
// re-composited at the same pose). Both paths now build their EyeViews with set_eye_frame; an eye without its frame draws no stamp.
static void test_nbox_fresh_equals_recomposite() {
    mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    mk360::Tracker tk; double t = 0;
    const float wing[3] = {3000, 1100, -20000};
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, wing); add_sprite(f, wing, kBlue, 4, false);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
    }
    Cam c = cam_yaw(180); tac::TacFrame f = frame_for(c); add_obj(f, wing);
    mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
    CHECK(sc.cached == 1 && plan.draw.size() == 1);
    for (float off_deg : {0.0f, 20.0f, 35.0f, -30.0f}) {   // the eye turned away from the contact: the stamp off-axis in the eye
        float d[3] = {wing[0] - c.C[0], wing[1] - c.C[1], wing[2] - c.C[2]};
        const float a = off_deg * 3.14159265f / 180.0f, cs = std::cos(a), sn = std::sin(a);
        const float tgt[3] = {c.C[0] + cs * d[0] + sn * d[2], c.C[1] + d[1], c.C[2] - sn * d[0] + cs * d[2]};
        const tac::EyeView ref = eye_towards(c.C, tgt);   // the re-composite's EyeView (whole frame)
        tac::EyeView fresh; fresh.ok = ref.ok; for (int i = 0; i < 16; ++i) fresh.clip[i] = ref.clip[i];
        tac::set_eye_frame(fresh, ref.eye, ref.right, ref.up, ref.fwd);   // the fresh path's EyeView (tac_eye_views, same helper as the re-composite)
        CHECK(tac::has_eye_frame(fresh) && tac::has_eye_frame(ref));
        tac::Overlay o1, o2;
        CHECK(mk360::build_stamps(o1, ref, plan) == 1 && mk360::build_stamps(o2, fresh, plan) == 1 && o1.v.size() == 6 && o2.v.size() == 6);
        for (size_t i = 0; i < 6; ++i) for (int k = 0; k < 4; ++k) CHECK(o1.v[i].pos[k] == o2.v[i].pos[k] && o1.v[i].col[k] == o2.v[i].col[k]);
        // the reviewed trigger: eye/up only (the old fresh EyeView) measures another width off-axis, the same height
        tac::EyeView bare = ref; for (int i = 0; i < 3; ++i) { bare.right[i] = 0; bare.fwd[i] = 0; }
        float cx0, cy0, sx0, sy0, cx1, cy1, sx1, sy1;
        CHECK(tac::marker_screen_frame(ref, wing, cx0, cy0, sx0, sy0) && tac::marker_screen_frame(bare, wing, cx1, cy1, sx1, sy1));
        CHECK(near(cx0, cx1, 1e-6) && near(cy0, cy1, 1e-6) && near(sy0, sy1, 1e-3 * std::fabs(sy0)));
        if (std::fabs(off_deg) >= 20.0f) CHECK(std::fabs(sx1 / sx0 - 1.0f) > 0.05f);
        // ... so a stamp is never built from such an eye (no silent width jump: a missing frame is a visible refusal, tested here)
        tac::Overlay o3; CHECK(mk360::build_stamps(o3, bare, plan) == 0 && o3.v.empty());
    }
}

// Codex J-203 finding 2: a fresh frame admitted by the pre-check whose marker image the producer rewrote BEFORE the consumer's atlas copies ran is
// rejected after completion (fresh_rewritten_post). The copies wrote the rewritten pixels into its cells: stamps_rejected forgets exactly those cells
// (no later accepted frame samples them) and empties the plan (nothing re-composited); a contact whose cell the rejected plan did not write keeps it.
static void test_nbox_rejected_after_completion() {
    mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    mk360::Tracker tk; double t = 0;
    const float A[3] = {3000, 1100, -20000}, B[3] = {-4000, 900, -20000};
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) {   // two accepted frames: both contacts complete in the window, each with its own cell
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c);
        add_obj(f, A); add_sprite(f, A, kBlue, 4, false); add_obj(f, B); add_sprite(f, B, kYellow, 3, false);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.live == 2);
    }
    int ia = -1, ib = -1;
    for (unsigned i = 0; i < tk.tracks.size(); ++i) { if (dist3(tk.tracks[i].pos, A) < 200) ia = int(i); if (dist3(tk.tracks[i].pos, B) < 200) ib = int(i); }
    CHECK(ia >= 0 && ib >= 0 && tk.tracks[ia].cell_ok && tk.tracks[ib].cell_ok && tk.tracks[ia].cell != tk.tracks[ib].cell);
    const int cell_b = tk.tracks[ib].cell; const uint16_t pw_b = tk.tracks[ib].c_pw;
    {   // frame F: only A boxed this frame (B's world draw without a box): A's cell is planned for a copy, B's is not
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c);
        add_obj(f, A); add_sprite(f, A, kBlue, 4, false); add_obj(f, B);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.live == 1 && plan.fresh && plan.copy.size() == 1 && plan.copy[0].cell == tk.tracks[ia].cell && tk.tracks[ia].cell_ok);
        // the copies were recorded (no stamps_not_copied), the GPU completed, then the post-check found the producer image rewritten
        const uint64_t inv = tk.st.nb_invalidated;
        mk360::stamps_rejected(tk, plan);
        CHECK(!tk.tracks[ia].cell_ok && tk.st.nb_invalidated == inv + 1 && tk.st.nb_rejected == 1);
        CHECK(tk.tracks[ib].cell_ok && tk.tracks[ib].cell == cell_b);   // not written by F: its earlier accepted stamp stays
        CHECK(plan.draw.empty() && plan.copy.empty() && plan.clear.empty() && !plan.fresh);
        // a re-composite of the rejected plan (never retained, but nothing would be drawn anyway) and a second call change nothing
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(c.C, A), plan) == 0);
        mk360::stamps_rejected(tk, plan); CHECK(tk.st.nb_rejected == 1 && tk.st.nb_invalidated == inv + 1);
    }
    {   // the next ACCEPTED frame: both contacts world-drawn outside the window: A (its cell holds the rewritten pixels) is never sampled, B is
        Cam c = cam_yaw(180); tac::TacFrame f = frame_for(c); add_obj(f, A); add_obj(f, B);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.cached == 1 && plan.draw.size() == 1 && plan.draw[0].id == tk.tracks[ib].id && plan.draw[0].cell == cell_b && plan.draw[0].pw == pw_b);
        for (const mk360::StampDraw& d : plan.draw) CHECK(d.id != tk.tracks[ia].id);
    }
    {   // A boxed complete again in an accepted frame: its own fresh pixels, cached again afterwards
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, A); add_sprite(f, A, kBlue, 4, false); add_obj(f, B);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.live == 1 && tk.tracks[ia].cell_ok);
    }
    CHECK(mk360::nbox_line(tk, plan, 0).find(";rejected=1;") != std::string::npos);
}

// Codex wave-4 review J-201 (review-w4-2-marker-lock-miss.md) P1 triggers: a label may be hidden only when hiding is paired with a replacement.
static void test_label_suppress_pairing() {
    tac::TacFrame f;
    tac::MarkerSprite& b = f.mark[f.nmark++]; b.cx = 2048; b.cy = 2048; b.hw = b.hh = 7; b.suppressed = true;
    for (int i = 0; i < 3; ++i) b.rgb[i] = kRed[i];
    auto cluster = [](float x0, float x1, float y0, float y1, uint32_t verts, const float rgb[3]) {
        tac::Cluster c; c.x0 = x0; c.x1 = x1; c.y0 = y0; c.y1 = y1; c.n = verts; for (int i = 0; i < 3; ++i) c.rgb[i] = rgb[i] * float(verts); return c;
    };
    int box_of[tac::kMarkCap];
    // P1-1: the label draw could not be read (> 512 indices / CPU-unreadable): never hidden, whatever the box did
    CHECK(!tac::label_suppress_ok(true, false, nullptr, 0, 0, box_of, 0, f.nmark));
    // P1-2: a readable 3-glyph (12-vertex) cluster at x 2140..2155 (85 GS from the box, beyond kLabelReach): joins no box -> stays the game's
    tac::Cluster far = cluster(2140, 2155, 2040, 2048, 12, kRed);
    tac::add_labels(f, 0, &far, 1, box_of);
    CHECK(box_of[0] == -1 && f.mark[0].glyphs == 0);
    CHECK(tac::label_draw_redrawable(&far, 1));   // the old count/colour-only rule would have hidden it
    CHECK(!tac::label_suppress_ok(true, true, &far, 1, 0, box_of, 0, f.nmark));
    // the same text in reach: joined to the hidden box -> may be hidden (the consumer draws TGT on that box)
    tac::Cluster nearc = cluster(2020, 2035, 2030, 2036, 12, kRed);
    tac::add_labels(f, 0, &nearc, 1, box_of);
    CHECK(box_of[0] == 0 && f.mark[0].glyphs == 3 && tac::label_suppress_ok(true, true, &nearc, 1, 0, box_of, 0, f.nmark));
    // two texts on one box (one replacement only), a dropped cluster, a non-redrawable text (green 4 glyphs = a type name like F-4E), box not hidden: kept
    tac::Cluster two[2] = {cluster(2020, 2035, 2030, 2036, 12, kRed), cluster(2060, 2075, 2050, 2056, 12, kRed)};
    tac::add_labels(f, 0, two, 2, box_of);
    CHECK(box_of[0] == 0 && box_of[1] == 0 && !tac::label_suppress_ok(true, true, two, 2, 0, box_of, 0, f.nmark));
    tac::add_labels(f, 0, &nearc, 1, box_of);
    CHECK(!tac::label_suppress_ok(true, true, &nearc, 1, 1, box_of, 0, f.nmark));
    tac::Cluster name = cluster(2058, 2080, 2030, 2036, 16, kGreen);
    tac::add_labels(f, 0, &name, 1, box_of);
    CHECK(box_of[0] == 0 && !tac::label_suppress_ok(true, true, &name, 1, 0, box_of, 0, f.nmark));
    tac::add_labels(f, 0, &nearc, 1, box_of);
    CHECK(!tac::label_suppress_ok(false, true, &nearc, 1, 0, box_of, 0, f.nmark));
    // a cluster joined to a box of an EARLIER box draw (index < first) is not this draw's replacement
    CHECK(!tac::label_suppress_ok(true, true, &nearc, 1, 0, box_of, 1, f.nmark));
}

// ------------------------------------------------------------------------------------------------ LOCK GROUPING + ROLL (headset test 2026-10-29 item 3)
// A raw game box (no label layout of add_sprite): centre (cx, cy) GS, half h, colour, optional label rectangle + colour.
static tac::MarkerSprite& raw_sprite(tac::TacFrame& f, float cx, float cy, float h, const float rgb[3]) {
    tac::MarkerSprite& m = f.mark[f.nmark++];
    m = tac::MarkerSprite{}; m.cx = cx; m.cy = cy; m.hw = m.hh = h;
    for (int i = 0; i < 3; ++i) m.rgb[i] = rgb[i];
    return m;
}
static void set_label(tac::MarkerSprite& m, float x0, float y0, float x1, float y1, uint8_t glyphs, const float rgb[3]) {
    m.glyphs = glyphs; m.lx0 = x0; m.ly0 = y0; m.lx1 = x1; m.ly1 = y1; for (int i = 0; i < 3; ++i) m.lrgb[i] = rgb[i];
}
static bool covers(const mk360::StampClear& r, double gx0, double gy0, double gx1, double gy1) {   // the stamp's pixels hold the GS rectangle
    return mk360::gs_of_px_x(r.x0, 2560) <= gx0 && mk360::gs_of_px_x(r.x1, 2560) >= gx1 && mk360::gs_of_px_y(r.y0, 1440) <= gy0 && mk360::gs_of_px_y(r.y1, 1440) >= gy1;
}
// The enemy-aircraft lock picture of 2026-10-29-user-enemy-air-box-lock-diamond.png: red box on the contact (green "F-4E" upper right), a separate red
// lock DIAMOND lower left (its own box draw: MATCH-2 keeps it a second group) with the "5" and the green range "3450" right of it. Before: two stamps cut
// at the midline, the diamond + range a track-less stamp at far_units (= the flat-quad distance: 'in the HUD'), the cached stamp outside the window = the
// box half only. Now: ONE stamp of the contact (box + diamond + all text), at the contact's range, cached whole and drawn whole outside the window.
static void test_nbox_lock_diamond() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float tgt[3] = {-1500, 1200, -18000};
    double t = 0;
    uint16_t pw = 0, ph = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
        tac::MarkerSprite& box = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(box, xs + 10, ys - 16, xs + 33, ys - 11, 4, kGreen);   // "F-4E"
        tac::MarkerSprite& dia = raw_sprite(f, xs - 17, ys + 19, 7.5f, kRed); set_label(dia, xs - 6, ys + 14, xs + 30, ys + 26, 5, kGreen);   // "5" "3450"
        mk360::update(tk, f, c.C, t, p);
        CHECK(tk.ngroups == 2);   // the game's two shapes stay two MATCH-2 groups (union half 16 GS > group_half)
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.sat == 1 && sc.cuts == 0 && sc.live == 1 && plan.copy.size() == 1 && plan.clear.size() == 1 && plan.draw.size() == 1);
        // the one stamp holds the box, the diamond and both labels: nothing of the lock picture stays on the flat quad (no double), none is lost
        CHECK(covers(plan.clear[0], xs - 7.5, ys - 16, xs + 33, ys + 7.5) && covers(plan.clear[0], xs - 24.5, ys + 11.5, xs + 30, ys + 26.5));
        const mk360::StampDraw& d = plan.draw[0];
        CHECK(d.live && !d.second && d.id == tk.tracks[0].id && dist3(d.pos, tgt) < 30.0f && d.axes);   // at the CONTACT's range, not far_units
        CHECK(tk.tracks[0].cell_ok && tk.tracks[0].c_q == 2 && !tk.tracks[0].cell2_ok);
        pw = d.pw; ph = d.ph;
    }
    CHECK(tk.st.nb_sat == 3 && mk360::nbox_line(tk, plan, 0).find(";sat=3;second=0;cut-kept=0;") != std::string::npos);
    // head / nose turned away: the cached stamp is the WHOLE lock picture (same size as the live one)
    Cam b = cam_yaw(150); tac::TacFrame fb = frame_for(b); add_obj(fb, tgt);
    mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc);
    CHECK(sc.cached == 1 && plan.draw.size() == 1 && plan.draw[0].pw == pw && plan.draw[0].ph == ph && dist3(plan.draw[0].pos, tgt) < 1e-3f);
    tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(b.C, tgt), plan) == 1);
    // the same shape ON another contact (its world draws there) is that contact's box, never folded into this one
    {
        mk360::Tracker t2; double tt = 0;
        for (int k = 0; k < 2; ++k, tt += 1.0 / 60) {
            Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c);
            float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
            float dir[3]; CHECK(mk360::screen_ray(f.A, xs - 17, ys + 19, dir));
            const float other[3] = {c.C[0] + dir[0] * 18100, c.C[1] + dir[1] * 18100, c.C[2] + dir[2] * 18100};
            add_obj(f, tgt); add_obj(f, other);
            float ox, oy; CHECK(tac::project_ps2(f.A, other, ox, oy, w)); CHECK(std::hypot(ox - (xs - 17), oy - (ys + 19)) < 0.5f);
            raw_sprite(f, xs, ys, 7.5f, kRed);
            raw_sprite(f, ox, oy, 7.5f, kRed);
            mk360::update(t2, f, c.C, tt, p); mk360::plan_stamps(t2, f, p, 2560, 1440, true, tt, plan, sc);
            CHECK(sc.sat == 0 && sc.live == 2);
        }
    }
}
// A lock reticle too far below the box for one cell window (46 GS): a SECOND stamp of the same contact (same anchor, same distance), disjoint from the
// box's, cached in the contact's second cell and drawn with it outside the window; a later picture without it releases that cell.
static void test_nbox_lock_far_second_stamp() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float tgt[3] = {800, 1300, -16000};
    double t = 0;
    for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
        raw_sprite(f, xs, ys, 7.5f, kRed);
        tac::MarkerSprite& r = raw_sprite(f, xs, ys + 46, 7.5f, kRed); set_label(r, xs + 10, ys + 42, xs + 34, ys + 48, 4, kRed);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.sat == 1 && sc.second == 1 && sc.cuts == 0 && sc.live == 2 && plan.copy.size() == 2);
        CHECK(!rect_overlap(plan.clear[0], plan.clear[1]));
        CHECK(covers(plan.clear[0], xs - 7.5, ys - 7.5, xs + 7.5, ys + 7.5) && covers(plan.clear[1], xs - 7.5, ys + 38.5, xs + 34, ys + 53.5));
        const mk360::StampDraw& a = plan.draw[0]; const mk360::StampDraw& s = plan.draw[1];
        CHECK(!a.second && s.second && a.id == s.id && a.id == tk.tracks[0].id && dist3(a.pos, s.pos) < 1e-3f);   // one anchor, one distance
        CHECK(near(s.oy, mk360::gs_of_px_y(plan.copy[1].y, 1440) - ys, 1e-3) && s.oy > 30.0f);   // offset from the BOX centre
        CHECK(tk.tracks[0].cell_ok && tk.tracks[0].cell2_ok && tk.tracks[0].cell2 != tk.tracks[0].cell && tk.cell_owner[tk.tracks[0].cell2] == tk.tracks[0].id);
    }
    const int cell2 = tk.tracks[0].cell2;
    {   // outside the window: both pieces from the contact's own cells
        Cam b = cam_yaw(180); tac::TacFrame fb = frame_for(b); add_obj(fb, tgt);
        mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.cached == 2 && sc.second == 1 && plan.draw.size() == 2 && plan.draw[1].cell == cell2 && plan.draw[1].second);
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(b.C, tgt), plan) == 2);
    }
    {   // lock lost: the next complete picture has no reticle -> the second cell is released (never paired with the new picture)
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w)); raw_sprite(f, xs, ys, 7.5f, kRed);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        CHECK(sc.live == 1 && tk.tracks[0].cell2 < 0 && !tk.tracks[0].cell2_ok && tk.cell_owner[cell2] == 0);
    }
}
// Cut pictures (a neighbour contact's stamp took part of it) never replace a whole cached picture (the 'only bits' outside the window), unless that is
// older than kStampRecacheS; a whole one always replaces a cut one.
static void test_nbox_cut_never_replaces_whole() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float A[3] = {0, 1000, -20000}, B[3] = {200, 1000, -20000};   // ~4 GS apart: their stamps overlap
    double t = 0;
    auto frame = [&](bool with_b) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, A); add_obj(f, B);
        add_sprite(f, A, kRed, 3, false); if (with_b) add_sprite(f, B, kGreen, 3, false);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
    };
    auto track_a = [&]() -> mk360::Track& { for (mk360::Track& tr : tk.tracks) if (dist3(tr.pos, A) < 100) return tr; return tk.tracks[0]; };
    frame(true); frame(true);
    CHECK(sc.cuts >= 1 && !track_a().cell_ok && track_a().c_q == 0);   // w12-2 finding 2: only cut pictures exist: none is cached (a truncated label is never shown outside the window)
    frame(false);
    CHECK(sc.cuts == 0 && track_a().c_q == 2);
    const uint16_t pw = track_a().c_pw, ph = track_a().c_ph;
    frame(true);
    CHECK(sc.cuts >= 1 && track_a().c_q == 2 && track_a().c_pw == pw && track_a().c_ph == ph && tk.st.nb_edge_kept >= 1);   // a cut picture is q 0 now: counted with the kept edge pictures
    for (int n = 0; n < 60 * (mk360::kStampRecacheS + 0.2); ++n) frame(true);   // contacts stay seen (no lost_s gap)
    CHECK(track_a().c_q == 2 && track_a().cell_ok);   // w12-2 finding 2: even a stale whole picture is only replaced by another whole one, never a cut one
}
// w12-2 finding 2 (regression): contact A's label lies under contact B's box. After the midline cut A's picture is truncated; it must not be cached.
static void test_w12_cut_label_not_cached() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float A[3] = {0, 1000, -20000}, B[3] = {600, 1000, -20000};
    double t = 0;
    unsigned cut_frames = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, A); add_obj(f, B);
        float xa, ya, wa, xb, yb, wb; CHECK(tac::project_ps2(f.A, A, xa, ya, wa) && tac::project_ps2(f.A, B, xb, yb, wb));
        tac::MarkerSprite& ma = raw_sprite(f, xa, ya, 4.0f, kRed); set_label(ma, xa + 22, ya - 3, xa + 60, ya + 3, 4, kRed);   // right-hand label reaching B's box
        raw_sprite(f, xb, yb, 4.0f, kGreen);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        // headset test 9 (section 22, CUT-CLEAN): the truncated label is still never cached; A's box alone (q 1, clear of B's box) may be, B's box (under
        // A's label) may not
        if (sc.cuts) {
            ++cut_frames;
            for (const mk360::Track& tr : tk.tracks) {
                if (!tr.cell_ok) continue;
                CHECK(tr.c_q == 1 && dist3(tr.pos, A) < 100);
                CHECK(tr.c_ox + tr.c_pw * 512.0 / 2560.0 <= 7.0);   // ends at A's box margin: no label glyph (from +22 GS), no part of B (from +6.6 GS)
            }
        }
    }
    CHECK(cut_frames >= 1);
}
// w12-2 finding 3 (regression): two satellites of one contact that both need the second cell: the second cell is written once per plan; the other piece
// gets its own temporary cell (never the same cell twice in one plan's copies).
static void test_w12_two_satellites_one_second_cell() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float tgt[3] = {800, 1300, -16000};
    double t = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
        raw_sprite(f, xs, ys, 4.0f, kRed);
        raw_sprite(f, xs, ys - 40, 4.0f, kRed); raw_sprite(f, xs, ys + 40, 4.0f, kRed);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.sat == 2 && sc.second == 2);
        std::vector<uint16_t> cells; for (const mk360::StampCopy& cp : plan.copy) cells.push_back(cp.cell);
        for (size_t i = 0; i < cells.size(); ++i) for (size_t j = i + 1; j < cells.size(); ++j) CHECK(cells[i] != cells[j]);   // no cell written twice in one plan
    }
}
// w12-2 finding 4 (regression): a confirmed new marker colour never shows the previous colour's cached picture outside the window.
static void test_w12_recolour_drops_cache() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float tgt[3] = {20000 * 0.6152f * (254.0f / 256.0f), 1000, -20000};   // projects at the window's right edge
    double t = 0;
    auto in_view = [&](const float rgb[3], bool edge) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
        raw_sprite(f, edge ? xs : xs - 10.0f, ys, 7.5f, rgb);   // edge: the box is clipped by the window edge (quality 0); otherwise it lies wholly inside (quality 2)
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
    };
    in_view(kRed, false); in_view(kRed, false);
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].cell_ok && tk.tracks[0].c_q == 2);
    for (int k = 0; k < 6; ++k) in_view(kBlue, true);   // blue boxes at the edge confirm the new identity (q 0 pictures; recolour_confirm 6, section 17.2)
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].ident && tk.tracks[0].rgb[2] > 0.9f && tk.tracks[0].rgb[0] < 0.5f);
    CHECK(!tk.tracks[0].cell_ok && !tk.tracks[0].cell2_ok);   // the red picture is gone
    Cam b = cam_yaw(180); tac::TacFrame fb = frame_for(b); add_obj(fb, tgt);
    mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc);
    CHECK(sc.cached == 0 && plan.draw.empty());   // outside the window: nothing stale is drawn for the blue contact
}
// The NDC edge directions of a built stamp quad (vertex order of build_stamps: TL, TR, BR, TL, BR, BL): right edge TL->TR, down edge TL->BL.
static void quad_edges(const tac::Overlay& ov, size_t first, double er[2], double ed[2]) {
    er[0] = ov.v[first + 1].pos[0] - ov.v[first].pos[0]; er[1] = ov.v[first + 1].pos[1] - ov.v[first].pos[1];
    ed[0] = ov.v[first + 5].pos[0] - ov.v[first].pos[0]; ed[1] = ov.v[first + 5].pos[1] - ov.v[first].pos[1];
}
// eye_towards rolled by psi (rad) about its forward axis (the head rolled in the cockpit)
static tac::EyeView eye_rolled(const float C[3], const float target[3], float psi) {
    tac::EyeView ev = eye_towards(C, target);
    const float cs = std::cos(psi), sn = std::sin(psi);
    float r[3], u[3];
    for (int i = 0; i < 3; ++i) { r[i] = cs * ev.right[i] + sn * ev.up[i]; u[i] = -sn * ev.right[i] + cs * ev.up[i]; }
    for (int i = 0; i < 3; ++i) { ev.clip[i] = r[i] / 0.9f; ev.clip[4 + i] = u[i] / 0.9f; ev.right[i] = r[i]; ev.up[i] = u[i]; }
    return ev;
}
// the NDC direction of the world axis ax at p in eye e (unit)
static void ndc_dir(const tac::EyeView& e, const float p[3], const float ax[3], double out[2]) {
    float a, b, c, d;
    CHECK(tac::eye_uv(e, p, a, b));
    const float q[3] = {p[0] + ax[0] * 50.0f, p[1] + ax[1] * 50.0f, p[2] + ax[2] * 50.0f};
    CHECK(tac::eye_uv(e, q, c, d));
    out[0] = c - a; out[1] = -(d - b);   // uv v down -> NDC y up
    const double l = std::hypot(out[0], out[1]); out[0] /= l; out[1] /= l;
}
static double angle_between(const double a[2], const double b[2]) { return std::fabs(std::atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])); }
// ROLL (item 3b): the aircraft (game camera) rolled by phi, the head rolled by psi in the cockpit, a contact in the window (live stamp) and behind
// (cached stamp): every stamp's right/down edges follow the GAME screen's right/up axes as this eye sees them (glued to the contact's world picture),
// stay a rectangle (perpendicular, no shear), keep the game's size, and with no relative roll are exactly the old axis-aligned quad.
static void test_nbox_roll() {
    const float tgt[3] = {2500, 1700, -20000};
    for (float phi_deg : {0.0f, 30.0f, -60.0f, 90.0f, 180.0f}) for (float psi_deg : {0.0f, 25.0f, -40.0f}) {
        const float phi = phi_deg * 3.14159265f / 180.0f, psi = psi_deg * 3.14159265f / 180.0f;
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        mk360::StampPlan plan; mk360::StampCounts sc;
        Cam c = cam_yaw(0);
        float r[3], u[3];
        for (int i = 0; i < 3; ++i) { r[i] = std::cos(phi) * c.r[i] + std::sin(phi) * c.u[i]; u[i] = -std::sin(phi) * c.r[i] + std::cos(phi) * c.u[i]; }
        for (int i = 0; i < 3; ++i) { c.r[i] = r[i]; c.u[i] = u[i]; }
        make_A(c.C, c.r, c.u, c.f, c.A);
        double t = 0;
        for (int k = 0; k < 2; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c); add_obj(f, tgt);
            float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
            tac::MarkerSprite& m = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(m, xs + 10, ys - 16, xs + 40, ys - 11, 4, kGreen);
            mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        }
        CHECK(sc.live == 1 && plan.draw[0].axes);
        // live: the stored axes are the rolled camera's screen axes at the box (perpendicular to its ray)
        CHECK(plan.draw[0].sr[0] * c.r[0] + plan.draw[0].sr[1] * c.r[1] + plan.draw[0].sr[2] * c.r[2] > 0.95f);
        CHECK(plan.draw[0].su[0] * c.u[0] + plan.draw[0].su[1] * c.u[1] + plan.draw[0].su[2] * c.u[2] > 0.95f);
        auto check_quad = [&](const tac::EyeView& ev, const mk360::StampPlan& pl, const float anchor[3], const float* sr, const float* su) {
            tac::Overlay ov; CHECK(mk360::build_stamps(ov, ev, pl) == 1 && ov.v.size() == 6);
            double er[2], ed[2], wr[2], wu[2];
            quad_edges(ov, 0, er, ed);
            ndc_dir(ev, anchor, sr, wr); ndc_dir(ev, anchor, su, wu);
            const double ler = std::hypot(er[0], er[1]), led = std::hypot(ed[0], ed[1]);
            const double ner[2] = {er[0] / ler, er[1] / ler}, ned[2] = {-ed[0] / led, -ed[1] / led};   // down edge -> up direction
            CHECK(angle_between(ner, wr) < 0.02 && angle_between(ned, wu) < 0.02);   // follows the game screen's axes (< 1.2 deg)
            CHECK(std::fabs(ner[0] * ned[0] + ner[1] * ned[1]) < 0.02);               // a rectangle: no shear
            const double gpx = 512.0 / 2560.0, gpy = 448.0 / 1440.0;                    // the game's size (eye NDC per tan = 1/0.9 near the axis)
            CHECK(near(ler, (pl.draw[0].pw - 1) * gpx * mk360::kTanPerGsX / 0.9, 0.03 * ler) && near(led, (pl.draw[0].ph - 1) * gpy * mk360::kTanPerGsY / 0.9, 0.03 * led));
            if (std::fabs(angle_between(wr, std::array<double, 2>{1.0, 0.0}.data())) < 1e-4) CHECK(std::fabs(er[1]) < 1e-6 && std::fabs(ed[0]) < 1e-6);
        };
        // live, eye at the contact rolled with the aircraft and the head (psi on top of phi) and a level eye
        check_quad(eye_rolled(c.C, tgt, phi + psi), plan, plan.draw[0].pos, plan.draw[0].sr, plan.draw[0].su);
        check_quad(eye_rolled(c.C, tgt, psi), plan, plan.draw[0].pos, plan.draw[0].sr, plan.draw[0].su);
        // eye rolled exactly with the aircraft: the game screen is upright in it -> the old axis-aligned quad
        {
            tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_rolled(c.C, tgt, phi), plan) == 1);
            double er[2], ed[2]; quad_edges(ov, 0, er, ed);
            CHECK(std::fabs(er[1]) < 1e-2 * std::fabs(er[0]) && std::fabs(ed[0]) < 1e-2 * std::fabs(ed[1]));
        }
        // the nose turns 180 deg (still rolled): the cached stamp behind, with THIS frame's camera axes carried to the contact
        Cam b = c;
        for (int i = 0; i < 3; ++i) { b.f[i] = -c.f[i]; b.r[i] = -c.r[i]; }
        make_A(b.C, b.r, b.u, b.f, b.A);
        tac::TacFrame fb = frame_for(b); add_obj(fb, tgt);
        mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.cached == 1 && plan.draw[0].axes);
        float dct[3] = {tgt[0] - b.C[0], tgt[1] - b.C[1], tgt[2] - b.C[2]}; const float l = std::sqrt(dct[0] * dct[0] + dct[1] * dct[1] + dct[2] * dct[2]);
        for (float& v : dct) v /= l;
        const mk360::StampDraw& d = plan.draw[0];
        CHECK(std::fabs(d.sr[0] * dct[0] + d.sr[1] * dct[1] + d.sr[2] * dct[2]) < 1e-4f && std::fabs(d.su[0] * dct[0] + d.su[1] * dct[1] + d.su[2] * dct[2]) < 1e-4f);
        CHECK(d.su[0] * b.u[0] + d.su[1] * b.u[1] + d.su[2] * b.u[2] > 0.9f);   // up = the camera's (rolled) up, carried to the contact
        // (right, up, toward-contact) keeps the camera's handedness: the stamp is never mirrored
        const float hc = (b.u[1] * b.f[2] - b.u[2] * b.f[1]) * b.r[0] + (b.u[2] * b.f[0] - b.u[0] * b.f[2]) * b.r[1] + (b.u[0] * b.f[1] - b.u[1] * b.f[0]) * b.r[2];
        const float hd = (d.su[1] * dct[2] - d.su[2] * dct[1]) * d.sr[0] + (d.su[2] * dct[0] - d.su[0] * dct[2]) * d.sr[1] + (d.su[0] * dct[1] - d.su[1] * dct[0]) * d.sr[2];
        CHECK(hc * hd > 0);
        check_quad(eye_rolled(b.C, tgt, psi), plan, d.pos, d.sr, d.su);
        // a contact straight above the camera: up falls back to the camera's back (no degenerate frame)
        float sr2[3], su2[3]; CHECK(mk360::contact_axes(b.A, b.u, sr2, su2));
        CHECK(su2[0] * -b.f[0] + su2[1] * -b.f[1] + su2[2] * -b.f[2] > 0.99f && sr2[0] * b.r[0] + sr2[1] * b.r[1] + sr2[2] * b.r[2] > 0.99f);
    }
}

// ------------------------------------------------------------------------------------------------ BOX-LOCK (markers-360.md section 15)
// Headset test 2026-10-30b: "Diamond still off. As you get closer boxes move off of the target models (seen inverted). Occasionally a tgt box would
// vanish outside the forward rectangle." With interp:1 the producer hands the consumer DISPLAY-time object positions (t := td, gpu_copy_step5_hooks.inl
// step5TacClose) while A and the game's box sprites are FRAME-time; the eyes sit at the display-time camera.
static const double kPi = 3.14159265358979;
// A camera at C looking along f with up u (right = f x u ... built as in cam_yaw: right x up = -fwd).
static Cam cam_at(const float C[3], const float r[3], const float u[3], const float f[3]) {
    Cam c; for (int i = 0; i < 3; ++i) { c.C[i] = C[i]; c.r[i] = r[i]; c.u[i] = u[i]; c.f[i] = f[i]; }
    make_A(c.C, c.r, c.u, c.f, c.A);
    return c;
}
// One interp producer frame: A of the frame-time camera, the object at its display-time position (t) with its frame-time one kept (tf), the game's box
// (and an optional lock diamond 'dia' GS below-left with its own label) drawn on the FRAME-time projection, as the game draws them.
static tac::TacFrame interp_frame(const Cam& c, const float Tf[3], const float Td[3], const float rgb[3], float dia_dx = 0, float dia_dy = 0) {
    tac::TacFrame f = frame_for(c);
    tac::ObjDraw& o = f.obj[f.nobj++];
    for (int i = 0; i < 3; ++i) { o.t[i] = Td[i]; o.tf[i] = Tf[i]; }
    o.tf_valid = 1;
    float xs, ys, w; CHECK(tac::project_ps2(f.A, Tf, xs, ys, w));
    raw_sprite(f, xs, ys, 7.5f, rgb);
    if (dia_dx != 0 || dia_dy != 0) raw_sprite(f, xs + dia_dx, ys + dia_dy, 7.5f, rgb);
    return f;
}
// The NDC of the absolute GS point (gx, gy) inside a LIVE stamp quad (first vertex index first) whose atlas copy is cp (texel centres of the copied
// marker-image pixels are the quad corners; the quad is affine in GS).
static void live_point_ndc(const tac::Overlay& ov, size_t first, const mk360::StampCopy& cp, double gx, double gy, double out[2]) {
    const double g0x = mk360::gs_of_px_x(cp.x + 0.5, 2560), g1x = mk360::gs_of_px_x(cp.x + cp.w - 0.5, 2560);
    const double g0y = mk360::gs_of_px_y(cp.y + 0.5, 1440), g1y = mk360::gs_of_px_y(cp.y + cp.h - 0.5, 1440);
    const tac::Vertex& tl = ov.v[first]; const tac::Vertex& tr = ov.v[first + 1]; const tac::Vertex& bl = ov.v[first + 5];
    const double a = (gx - g0x) / (g1x - g0x), b = (gy - g0y) / (g1y - g0y);
    out[0] = tl.pos[0] + a * (tr.pos[0] - tl.pos[0]) + b * (bl.pos[0] - tl.pos[0]);
    out[1] = tl.pos[1] + a * (tr.pos[1] - tl.pos[1]) + b * (bl.pos[1] - tl.pos[1]);
}
// The NDC of a world point in an eye (as build_stamps projects: clip x,y / w).
static void eye_ndc(const tac::EyeView& e, const float p[3], double out[2]) {
    double q[3] = {p[0] - e.eye[0], p[1] - e.eye[1], p[2] - e.eye[2]};
    const double x = e.clip[0] * q[0] + e.clip[1] * q[1] + e.clip[2] * q[2] + e.clip[3];
    const double y = e.clip[4] * q[0] + e.clip[5] * q[1] + e.clip[6] * q[2] + e.clip[7];
    const double w = e.clip[12] * q[0] + e.clip[13] * q[1] + e.clip[14] * q[2] + e.clip[15];
    out[0] = x / w; out[1] = y / w;
}
static double ndc_err_deg(const double a[2], const double b[2]) { return std::atan(std::hypot(a[0] - b[0], a[1] - b[1]) * 0.9) * 180.0 / kPi; }   // eye: 0.9 tan per NDC
// Cause B1 (+ the lock diamond, defect a): display time one game frame (1/60 s) ahead of the frame. In the window the box must sit on the target MODEL
// (display-time position, where the eyes see it) at every range, matched to its contact, and the lock diamond where the game placed it relative to
// the box. Before: the stamp was anchored on the frame-time box ray: the box trailed the model by the target's own motion per horizon (30 u = 6 deg
// at 300 u); with a crossing target the display-time projection also left the 20 GS match gate (test_boxlock_inverted).
static void test_boxlock_interp_close() {
    const double h = 1.0 / 60.0;
    // both flying the same 1800 u/s velocity (a wingman / a chased target in formation: it stays put in the window), so the eyes' camera and the model
    // move by the same 30 u per display horizon while the old stamp stayed at the frame-time point
    const float vc[3] = {-1500, 0, -1000}, vt[3] = {-1500, 0, -1000};
    for (float range : {6000.0f, 1500.0f, 600.0f, 300.0f, 200.0f}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3; p.near_min = 0; p.near_ref = 0;   // the near policy is checked on its own (test_boxlock_near_*)
        mk360::StampPlan plan; mk360::StampCounts sc;
        const float r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0}, f0[3] = {0, 0, -1};
        double worst_box = 0, worst_dia = 0; unsigned matched = 0, frames = 0;
        for (int k = 0; k < 12; ++k) {
            const double t = k * h;
            const float Cf[3] = {float(vc[0] * t), 1000, float(vc[2] * t)};
            const float Tf[3] = {float(0.25f * range + vt[0] * t), 1000.0f + 0.1f * range, float(-range + vt[2] * t)};
            const float Td[3] = {float(Tf[0] + vt[0] * h), Tf[1], float(Tf[2] + vt[2] * h)};
            const float Cd[3] = {float(Cf[0] + vc[0] * h), Cf[1], float(Cf[2] + vc[2] * h)};
            Cam c = cam_at(Cf, r0, u0, f0);
            tac::TacFrame f = interp_frame(c, Tf, Td, kRed, -17, 19);   // box + lock diamond lower left (the 2026-10-29 screenshot layout)
            mk360::update(tk, f, c.C, t, p);
            mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
            tac::EyeView ev[2] = {eye_towards(Cd, Td), eye_towards(Cd, Td)};
            mk360::set_view_centre(plan, ev);
            if (k < 3) continue;   // identity confirmation
            ++frames;
            if (tk.tracks.size() == 1 && tk.tracks[0].sprite >= 0) ++matched;
            if (!(sc.live == 1 && plan.draw.size() == 1 && plan.copy.size() == 1))
                std::printf("  k%d live %u sat %u second %u cuts %u empty %u nocell %u draws %zu groups %u\n", k, sc.live, sc.sat, sc.second, sc.cuts, sc.empty, sc.no_cell, plan.draw.size(), tk.ngroups);
            CHECK(sc.live == 1 && plan.draw.size() == 1 && plan.copy.size() == 1);
            tac::Overlay ov; CHECK(mk360::build_stamps(ov, ev[0], plan) == 1);
            if (ov.v.size() < 6 || plan.copy.empty()) { worst_box = 99; continue; }
            float xs, ys, w; CHECK(tac::project_ps2(f.A, Tf, xs, ys, w));
            double box[2], dia[2], model[2];
            live_point_ndc(ov, 0, plan.copy[0], xs, ys, box);
            live_point_ndc(ov, 0, plan.copy[0], xs - 17, ys + 19, dia);
            eye_ndc(ev[0], Td, model);
            worst_box = std::fmax(worst_box, ndc_err_deg(box, model));
            // the diamond: the game's GS offset from the box at the flat quad's scale, along the GAME screen's axes at the target (as the game laid it
            // out), seen from this eye
            const mk360::StampDraw& sd = plan.draw[0];
            const float dd = dist3(Td, Cd);
            float Pd[3];
            for (int i = 0; i < 3; ++i) Pd[i] = Td[i] + float((sd.sr[i] * -17.0 * mk360::kTanPerGsX - sd.su[i] * 19.0 * mk360::kTanPerGsY) * dd);
            double want[2]; eye_ndc(ev[0], Pd, want);
            worst_dia = std::fmax(worst_dia, ndc_err_deg(dia, want));
        }
        std::printf("  boxlock range %5.0f: box-to-model %.3f deg, diamond-to-game-offset %.3f deg, matched %u/%u\n", range, worst_box, worst_dia, matched, frames);
        CHECK(matched == frames);          // never loses the match at close range (the in-view miss rule would drop its identity: defect c)
        CHECK(worst_box < 0.25);           // ON the model at every range
        CHECK(worst_dia < 0.05);           // the diamond where the game put it relative to the box
    }
}
// Cause B2 + defect c, INVERTED: a crossing target at 400 u. Upright, the track's learned GS offset absorbed the display/frame lag; the aircraft rolls
// inverted (the lag's screen direction flips): before, the stale offset doubled the residual beyond the gate, the box matched nothing for >= 9 frames,
// the identity was dropped in view and the box VANISHED once the target left the window; the offset was also applied in the rolled screen axes
// outside the window (box off the target). After: matched every frame inverted, and outside the window the cached box is on the target.
static void test_boxlock_inverted() {
    const double h = 0.6 / 60.0;   // the display horizon of the live logs (summary-interp k-mean 0.54..0.73 game frames)
    const float vc[3] = {-900, 0, -1000}, vt[3] = {-1500, 0, -1000};   // the target crosses at 600 u/s relative
    mk360::Tracker tk; mk360::Params p; p.mode = 3; p.near_min = 0; p.near_ref = 0;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float up_r[3] = {1, 0, 0}, up_u[3] = {0, 1, 0}, inv_r[3] = {-1, 0, 0}, inv_u[3] = {0, -1, 0}, fw[3] = {0, 0, -1}, bk[3] = {0, 0, 1};
    unsigned inverted_unmatched = 0;
    double worst_out = 0; unsigned cached = 0;
    for (int k = 0; k < 30; ++k) {
        const double t = k * h;
        const float Cf[3] = {float(vc[0] * t), 1000, float(vc[2] * t)};
        const float Tf[3] = {float(160 + vt[0] * t), 1060, float(-400 + vt[2] * t)};
        const float Td[3] = {float(Tf[0] + vt[0] * h), Tf[1], float(Tf[2] + vt[2] * h)};
        const float Cd[3] = {float(Cf[0] + vc[0] * h), Cf[1], float(Cf[2] + vc[2] * h)};
        const int phase = k < 10 ? 0 : k < 20 ? 1 : 2;   // 0 upright, 1 inverted (target in the window), 2 inverted, nose turned away (target behind)
        Cam c = phase == 0 ? cam_at(Cf, up_r, up_u, fw) : phase == 1 ? cam_at(Cf, inv_r, inv_u, fw) : cam_at(Cf, up_r, inv_u, bk);
        tac::TacFrame f = frame_for(c);
        tac::ObjDraw& o = f.obj[f.nobj++];
        for (int i = 0; i < 3; ++i) { o.t[i] = Td[i]; o.tf[i] = Tf[i]; }
        o.tf_valid = 1;
        if (phase < 2) { float xs, ys, w; CHECK(tac::project_ps2(f.A, Tf, xs, ys, w)); raw_sprite(f, xs, ys, 7.5f, kRed); }
        mk360::update(tk, f, c.C, t, p);
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        tac::EyeView ev[2] = {eye_towards(Cd, Td), eye_towards(Cd, Td)};
        mk360::set_view_centre(plan, ev);
        CHECK(tk.tracks.size() == 1);
        if (tk.tracks.empty()) continue;
        if (phase == 1 && tk.tracks[0].sprite < 0) ++inverted_unmatched;
        if (phase == 2) {
            CHECK(!tk.tracks[0].in_window);
            if (sc.cached != 1) continue;
            ++cached;
            tac::Overlay ov; CHECK(mk360::build_stamps(ov, ev[0], plan) == 1 && ov.v.size() == 6);
            if (ov.v.size() < 6) { worst_out = 99; continue; }
            double ctr[2] = {0, 0}, model[2];
            for (int i : {0, 1, 2, 5}) { ctr[0] += 0.25 * ov.v[i].pos[0]; ctr[1] += 0.25 * ov.v[i].pos[1]; }   // a box-only picture: its centre is the box
            eye_ndc(ev[0], Td, model);
            worst_out = std::fmax(worst_out, ndc_err_deg(ctr, model));
        }
    }
    std::printf("  boxlock inverted: unmatched inverted frames %u/10, cached outside %u/10, box-to-model outside %.3f deg\n", inverted_unmatched, cached, worst_out);
    CHECK(inverted_unmatched == 0);
    CHECK(cached == 10);                   // persists outside the window while the contact is tracked
    CHECK(worst_out < 0.25);
}
// Defect c: a target crossing close (220 u, 600 u/s relative, display horizon 0.6 frame). Before: its display-time projection sat 25+ GS from the
// game's box (beyond the 20 GS gate, no learned offset yet): never matched, never identified, so nothing was cached and its box vanished the moment it
// left the window (in the window the stamp went to far_units on the box ray). After: matched from the first frame, identified, cached outside.
static void test_boxlock_close_persist() {
    const double h = 0.6 / 60.0;
    const float vc[3] = {-900, 0, -1000}, vt[3] = {-1500, 0, -1000};
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0}, fw[3] = {0, 0, -1}, rb[3] = {-1, 0, 0}, bk[3] = {0, 0, 1};
    unsigned matched = 0, cached = 0;
    for (int k = 0; k < 22; ++k) {
        const double tt = k / 60.0;
        const float Cf[3] = {float(vc[0] * tt), 1000, float(vc[2] * tt)};
        const float Tf[3] = {float(90 + vt[0] * tt), 1030, float(-200 + vt[2] * tt)};
        const float Td[3] = {float(Tf[0] + vt[0] * h), Tf[1], float(Tf[2] + vt[2] * h)};
        const bool away = k >= 12;
        Cam c = away ? cam_at(Cf, rb, u0, bk) : cam_at(Cf, r0, u0, fw);
        tac::TacFrame f = frame_for(c);
        tac::ObjDraw& o = f.obj[f.nobj++];
        for (int i = 0; i < 3; ++i) { o.t[i] = Td[i]; o.tf[i] = Tf[i]; }
        o.tf_valid = 1;
        if (!away) { float xs, ys, w; CHECK(tac::project_ps2(f.A, Tf, xs, ys, w)); raw_sprite(f, xs, ys, 7.5f, kRed); }
        mk360::update(tk, f, c.C, tt, p);
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, tt, plan, sc);
        if (tk.tracks.size() != 1) continue;
        if (!away && tk.tracks[0].sprite >= 0) ++matched;
        if (away && sc.cached == 1) ++cached;
    }
    std::printf("  boxlock close crossing 220 u: matched in view %u/12, cached outside %u/10\n", matched, cached);
    CHECK(matched == 12);
    CHECK(cached == 10);
}
// Cause B3 (near policy, merged after step134): the stamp of a contact 60 u away was pushed out to mk360min along the ray from the GAME camera of the
// frame; the eyes sit at the display-time camera (interp: 10-15 u ahead, summary-interp cam-shift-mean) + the head: the box moved off the target by
// o x (1/d - 1/min) = 7 deg for 12 u. After: pushed along the ray from the eyes' centre (set_view_centre): the direction from each eye is unchanged.
static void test_boxlock_near_push_origin() {
    mk360::StampPlan plan; plan.atlas = mk360::atlas_geom(2560, 1440); CHECK(plan.atlas.ok);
    plan.near_min = 150; plan.near_ref = 0; plan.near_floor = 1;
    const float Cgame[3] = {0, 0, 0};
    plan.has_ref = true; for (int i = 0; i < 3; ++i) plan.ref[i] = Cgame[i];   // what plan_stamps sets: the frame's game camera
    const float tgt[3] = {20, 5, -60};
    mk360::StampDraw d; for (int i = 0; i < 3; ++i) d.pos[i] = tgt[i];
    d.cell = 0; d.pw = 21; d.ph = 13; d.id = 1;   // a box symmetric about its anchor
    d.ox = float(-0.5 * 21 * 512.0 / 2560); d.oy = float(-0.5 * 13 * 448.0 / 1440);
    plan.draw.push_back(d);
    const float centre[3] = {12, 3, -2};   // the eyes: 12 u to the side of the game camera (+-0.16 u IPD)
    tac::EyeView ev[2];
    for (int e = 0; e < 2; ++e) { float E[3] = {centre[0] + (e ? 0.16f : -0.16f), centre[1], centre[2]}; ev[e] = eye_towards(E, tgt); }
    mk360::set_view_centre(plan, ev);
    double worst = 0;
    for (int e = 0; e < 2; ++e) {
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, ev[e], plan) == 1);
        if (ov.v.size() < 6) { worst = 99; continue; }
        double ctr[2] = {0, 0}, want[2];
        for (int i : {0, 1, 2, 5}) { ctr[0] += 0.25 * ov.v[i].pos[0]; ctr[1] += 0.25 * ov.v[i].pos[1]; }
        eye_ndc(ev[e], tgt, want);
        worst = std::fmax(worst, ndc_err_deg(ctr, want));
    }
    std::printf("  boxlock near push: box-to-target %.3f deg (eyes 12 u from the game camera, target 60 u)\n", worst);
    CHECK(worst < 0.2);
    CHECK(plan.has_ref && near(plan.ref[0], 12.0, 1e-4) && near(plan.ref[2], -2.0, 1e-4));
}
// Cause B3b: the near SCALE shrank the stamp about its anchor = the matched box GROUP. The game's box is read as two pieces (every step133/134 log:
// 2 groups per box, both half >= 8 GS, 8..16 GS either side of the contact, mean residual ~0), so the anchor was a piece 12 GS beside the target and
// the shrunk box slid off it (0.7 x 12 GS = 0.9 deg at the floor). After: the anchor is the contact (the box centre), the box shrinks ON the target.
static void test_boxlock_near_scale_two_pieces() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3; p.near_min = 0; p.near_ref = 750; p.near_floor = 0.3f;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float tgt[3] = {30, 1010, -200};
    Cam c = cam_yaw(0);
    double worst = 0;
    for (int k = 0; k < 3; ++k) {
        tac::TacFrame f = frame_for(c); add_obj(f, tgt);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, tgt, xs, ys, w));
        tac::MarkerSprite& a = raw_sprite(f, xs - 12, ys, 1.0f, kGreen); a.hh = 10;   // left bar
        tac::MarkerSprite& b = raw_sprite(f, xs + 12, ys, 1.0f, kGreen); b.hh = 10;   // right bar
        mk360::update(tk, f, c.C, k / 60.0, p);
        mk360::plan_stamps(tk, f, p, 2560, 1440, true, k / 60.0, plan, sc);
        tac::EyeView ev[2] = {eye_towards(c.C, tgt), eye_towards(c.C, tgt)};
        mk360::set_view_centre(plan, ev);
        CHECK(sc.live == 1 && plan.copy.size() == 1);
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, ev[0], plan) == 1);
        if (ov.v.size() < 6) { worst = 99; continue; }
        double ctr[2] = {0, 0}, want[2];
        for (int i : {0, 1, 2, 5}) { ctr[0] += 0.25 * ov.v[i].pos[0]; ctr[1] += 0.25 * ov.v[i].pos[1]; }
        eye_ndc(ev[0], tgt, want);
        worst = std::fmax(worst, ndc_err_deg(ctr, want));
    }
    std::printf("  boxlock near scale (box read as two pieces, 200 u): box-to-target %.3f deg\n", worst);
    CHECK(worst < 0.1);
}

// ---------------------------------------------------------------------------------------------- headset test 5 (2026-10-30c items 6-8; markers-360.md section 16)
// The quad orientation of one built stamp (vertex order TL, TR, BR, TL, BR, BL): the NDC cross product of (TR - TL) x (BL - TL) is NEGATIVE for an upright,
// unmirrored picture (right edge +x, down edge -y in NDC) and stays negative under any ROTATION; a mirror flips it. uv must run TL->TR in +u, TL->BL in +v.
static bool stamp_unmirrored(const tac::Overlay& ov, size_t first) {
    if (ov.v.size() < first + 6) return false;
    const tac::Vertex& tl = ov.v[first]; const tac::Vertex& tr = ov.v[first + 1]; const tac::Vertex& bl = ov.v[first + 5];
    const double cr = double(tr.pos[0] - tl.pos[0]) * double(bl.pos[1] - tl.pos[1]) - double(tr.pos[1] - tl.pos[1]) * double(bl.pos[0] - tl.pos[0]);
    return cr < 0 && tr.col[0] > tl.col[0] && bl.col[1] > tl.col[1] && near(tr.col[1], tl.col[1], 1e-7) && near(bl.col[0], tl.col[0], 1e-7);
}
// A camera at C pitched up by pitch degrees (yaw 0) and rolled by roll degrees about its forward axis.
static Cam cam_pitch_roll(const float C[3], float pitch_deg, float roll_deg = 0.0f) {
    const float p = pitch_deg * 3.14159265f / 180.0f, q = roll_deg * 3.14159265f / 180.0f;
    const float f[3] = {0, std::sin(p), -std::cos(p)}, u0[3] = {0, std::cos(p), std::sin(p)}, r0[3] = {1, 0, 0};
    float r[3], u[3];
    for (int i = 0; i < 3; ++i) { r[i] = std::cos(q) * r0[i] + std::sin(q) * u0[i]; u[i] = -std::sin(q) * r0[i] + std::cos(q) * u0[i]; }
    return cam_at(C, r, u, f);
}
// Item 8 (user-shots\9.png): the "MiG-21bis" label of a cached box outside the window showed only the bottom half of its glyphs (read as mirrored).
// Cause: the label sits ABOVE the box; a contact leaving the window over its top edge still had its box >= 1 GS inside (complete, q 2) while the game
// had already clipped the label at the window edge, and that picture replaced the whole one. Now a clipped label = q 1: never over a whole picture
// (before kStampRecacheClippedS), cached only when nothing better exists, replaced by the next whole one; and no stamp is ever drawn mirrored.
static void test_hs5_label_whole() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float C[3] = {0, 1000, 0};
    const float P[3] = {0, 1000 + 0.3263f * 20000, -20000};   // pitch 0: the box centre 213 GS above the window centre (GS y 1835: box top 1 GS+ inside, label top out)
    double t = 0;
    auto frame = [&](float pitch, float roll = 0.0f) {
        Cam c = cam_pitch_roll(C, pitch, roll); tac::TacFrame f = frame_for(c); add_obj(f, P);
        float xs, ys, w; const bool vis = tac::project_ps2(f.A, P, xs, ys, w) && std::fabs(xs - 2048) < 256 && std::fabs(ys - 2048) < 224;
        if (vis) { tac::MarkerSprite& m = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(m, xs + 10, ys - 16, xs + 63, ys - 11, 9, kGreen); }   // "MIG-21BIS" upper right
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        return c;
    };
    frame(8); frame(8);   // well inside: the whole picture (box + whole label) is cached
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].cell_ok && tk.tracks[0].c_q == 2);
    const uint16_t whole_ph = tk.tracks[0].c_ph; const float whole_oy = tk.tracks[0].c_oy;
    const uint64_t clipped0 = tk.st.nb_label_clipped, kept0 = tk.st.nb_cut_kept;
    frame(0);   // at the top edge: the box is whole (>= 1 GS inside), the label above is clipped by the window
    CHECK(sc.live == 1 && tk.st.nb_label_clipped == clipped0 + 1 && tk.st.nb_cut_kept == kept0 + 1);
    CHECK(tk.tracks[0].cell_ok && tk.tracks[0].c_q == 2 && tk.tracks[0].c_ph == whole_ph && near(tk.tracks[0].c_oy, whole_oy, 1e-4));   // the whole picture stays
    // outside the window (nose 40 deg down): the cached picture drawn is the WHOLE one, upright
    {
        Cam c = frame(-40);
        CHECK(sc.cached == 1 && plan.draw.size() == 1 && plan.draw[0].ph == whole_ph && near(plan.draw[0].oy, whole_oy, 1e-4));
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(c.C, P), plan) == 1); CHECK(stamp_unmirrored(ov, 0));
    }
    // never mirrored: a cached picture seen from every side (contact beside, behind, above, below) with the aircraft upright / rolled / inverted
    unsigned sides = 0;
    for (float roll : {0.0f, 35.0f, 180.0f, -90.0f}) for (float yaw : {0.0f, 60.0f, 120.0f, 180.0f, -150.0f, -90.0f}) for (float el : {-70.0f, -20.0f, 0.0f, 30.0f, 75.0f}) {
        const float y = yaw * 3.14159265f / 180.0f, e = el * 3.14159265f / 180.0f;
        const float Q[3] = {C[0] + 5000 * std::cos(e) * std::sin(y), C[1] + 5000 * std::sin(e), C[2] - 5000 * std::cos(e) * std::cos(y)};
        Cam c = cam_pitch_roll(C, 0, roll); tac::TacFrame f = frame_for(c); add_obj(f, Q);
        if (mk360::in_window(f.A, Q, 1.0f)) continue;   // cached pictures only outside the window (live ones: the game's own layout)
        mk360::Tracker t2; t2.img_w = 2560; t2.img_h = 1440;
        mk360::update(t2, f, c.C, 0.0, p);
        CHECK(t2.tracks.size() == 1);
        mk360::Track& tr = t2.tracks[0];
        tr.ident = true; tr.cell = 0; tr.cell_ok = true; tr.c_q = 2; tr.c_pw = 300; tr.c_ph = 60; tr.c_ox = -10; tr.c_oy = -20; t2.cell_owner[0] = tr.id;
        mk360::StampPlan pl; mk360::StampCounts s2;
        mk360::plan_stamps(t2, f, p, 2560, 1440, false, 0.0, pl, s2);
        CHECK(s2.cached == 1 && pl.draw.size() == 1 && pl.draw[0].axes);
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(C, Q), pl) == 1);
        CHECK(stamp_unmirrored(ov, 0));
        ++sides;
    }
    CHECK(sides >= 80);
    // a contact only ever seen with its label clipped: that picture IS cached (q 1, better than nothing) and the first whole picture replaces it
    {
        mk360::Tracker t3; double tt = 0;
        auto fr3 = [&](float pitch) {
            Cam c = cam_pitch_roll(C, pitch); tac::TacFrame f = frame_for(c); add_obj(f, P);
            float xs, ys, w; CHECK(tac::project_ps2(f.A, P, xs, ys, w));
            tac::MarkerSprite& m = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(m, xs + 10, ys - 16, xs + 63, ys - 11, 9, kGreen);
            mk360::update(t3, f, c.C, tt, p); mk360::plan_stamps(t3, f, p, 2560, 1440, true, tt, plan, sc); tt += 1.0 / 60;
        };
        fr3(0); fr3(0);
        CHECK(t3.tracks.size() == 1 && t3.tracks[0].cell_ok && t3.tracks[0].c_q == 1);
        fr3(8);
        CHECK(t3.tracks[0].cell_ok && t3.tracks[0].c_q == 2);
        // a clipped picture replaces a whole one only once that is older than kStampRecacheClippedS (a stale picture is not kept forever)
        for (int n = 0; n < int(60 * (mk360::kStampRecacheClippedS - 0.5)); ++n) fr3(0);
        CHECK(t3.tracks[0].c_q == 2);
        for (int n = 0; n < 60; ++n) fr3(0);
        CHECK(t3.tracks[0].c_q == 1);
    }
    // a label wider than the cell window holds (150 GS) is clipped by the stamp itself: q 1 as well
    {
        mk360::Tracker t4; double tt = 0;
        for (int k = 0; k < 2; ++k, tt += 1.0 / 60) {
            Cam c = cam_yaw(0); const float Q[3] = {-3000, 1000, -20000}; tac::TacFrame f = frame_for(c); add_obj(f, Q);
            float xs, ys, w; CHECK(tac::project_ps2(f.A, Q, xs, ys, w));
            tac::MarkerSprite& m = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(m, xs - 70, ys - 16, xs + 80, ys - 11, 20, kGreen);
            mk360::update(t4, f, c.C, tt, p); mk360::plan_stamps(t4, f, p, 2560, 1440, true, tt, plan, sc);
        }
        CHECK(t4.tracks.size() == 1 && t4.tracks[0].c_q == 1 && t4.st.nb_label_clipped == 2);
    }
    const std::string l = mk360::nbox_line(tk, plan, 0);
    CHECK(l.find(";label-clipped=1;") != std::string::npos && l.size() < 480);
}
// Item 6 (friendly boxes hit and miss). live-1936-step137 evidence: no refusal, no-cell 0, no-atlas 0, evicted 0, invalidated 0; identified contacts
// outside the window were drawn every frame (nbox cached/frame = summary identified-outside). The misses were identity gaps: (1) the wingman is boxed
// only after the game has boxed it in its window (by design: D-355, nothing invented); (2) at t 945.9 -> 960.9 s the frames stopped / came without A
// (43), every track was lost and the wingman re-tracked at the same range/bearing as a NEW unidentified contact (id 1 -> 45: r3210 b-175.8 -> r3136
// b-174.1): no box until it crossed the window again ~25 s later. IDENT-HOLD carries the identity and the cached picture over such a dropout.
static void test_hs5_ident_hold() {
    mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float off[3] = {3000, 100, -20000};   // the wingman's offset from the game camera (formation)
    auto seen = [&](mk360::Tracker& tk, const float C[3], float yaw, double t, bool boxed, const float* o = nullptr, bool a_valid = true, bool present = true) {
        Cam c = cam_yaw(yaw); for (int i = 0; i < 3; ++i) c.C[i] = C[i];
        make_A(c.C, c.r, c.u, c.f, c.A);
        tac::TacFrame f = frame_for(c); f.a_valid = a_valid;
        const float* d = o ? o : off;
        const float W[3] = {C[0] + d[0], C[1] + d[1], C[2] + d[2]};
        if (present) { add_obj(f, W); if (boxed) CHECK(add_sprite(f, W, kBlue, 4, false) == 0); }
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
    };
    const float C0[3] = {0, 1000, 0};
    mk360::Tracker tk; double t = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) seen(tk, C0, 0, t, true);   // boxed in the window: identified, its whole picture cached
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].ident && tk.tracks[0].cell_ok);
    const uint32_t id0 = tk.tracks[0].id; const int cell0 = tk.tracks[0].cell;
    seen(tk, C0, 180, t, false); t += 1.0 / 60;
    CHECK(sc.cached == 1);   // behind: drawn from its cell
    // a 15 s dropout (radio cut-in / cutscene): both flew on 40 km; the wingman is still at the same offset
    t += 15.0;
    const float C1[3] = {5000, 1000, -40000};
    seen(tk, C1, 180, t, false); t += 1.0 / 60;
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].id != id0 && tk.tracks[0].ident && tk.st.ghosts == 1 && tk.st.reacquired == 1 && tk.ghosts.empty());
    CHECK(tk.tracks[0].cell == cell0 && tk.tracks[0].cell_ok && tk.cell_owner[cell0] == tk.tracks[0].id && tk.tracks[0].rgb[2] > 0.9f);
    CHECK(sc.cached == 1 && plan.draw.size() == 1 && plan.draw[0].cell == cell0);   // the friendly box is back the same frame
    for (int k = 0; k < 5; ++k, t += 1.0 / 60) { seen(tk, C1, 180, t, false); CHECK(sc.cached == 1); }
    CHECK(mk360::nbox_line(tk, plan, 0).find(";ghosts=1(reacquired:1,held:0);") != std::string::npos);
    CHECK(mk360::sample_line(tk, 0).find(":g4:c2") != std::string::npos);
    // frames WITHOUT A (the game not drawing its flight view) for 0.33 s: also a dropout
    {
        mk360::Tracker t2; double tt = 0;
        for (int k = 0; k < 3; ++k, tt += 1.0 / 60) seen(t2, C0, 0, tt, true);
        for (int k = 0; k < 20; ++k, tt += 1.0 / 60) seen(t2, C0, 0, tt, false, nullptr, false, false);
        CHECK(t2.tracks.empty() && t2.ghosts.size() == 1);
        seen(t2, C0, 180, tt, false);
        CHECK(t2.tracks.size() == 1 && t2.tracks[0].ident && t2.st.reacquired == 1 && sc.cached == 1);
    }
    // a KILL (frames keep coming, its draws stop): no ghost; a new contact later at the same offset stays unidentified (nothing invented)
    {
        mk360::Tracker t2; double tt = 0;
        for (int k = 0; k < 3; ++k, tt += 1.0 / 60) seen(t2, C0, 0, tt, true);
        for (int k = 0; k < 30; ++k, tt += 1.0 / 60) seen(t2, C0, 180, tt, false, nullptr, true, false);
        CHECK(t2.tracks.empty() && t2.ghosts.empty() && t2.st.ghosts == 0);
        seen(t2, C0, 180, tt, false);
        CHECK(t2.tracks.size() == 1 && !t2.tracks[0].ident && sc.cached == 0 && plan.draw.empty());
        CHECK(mk360::sample_line(t2, 0).find(":g0:c-") != std::string::npos && t2.st.nb_out_uncached == 0);   // unidentified: not counted as uncached
    }
    // after a dropout a contact at ANOTHER offset (5 km from the wingman's) does not take the identity; the ghost keeps its cell, then expires
    {
        mk360::Tracker t2; double tt = 0;
        for (int k = 0; k < 3; ++k, tt += 1.0 / 60) seen(t2, C0, 0, tt, true);
        tt += 15.0;
        const float other[3] = {off[0] + 5000, off[1], off[2]};
        seen(t2, C1, 180, tt, false, other); tt += 1.0 / 60;
        CHECK(t2.tracks.size() == 1 && !t2.tracks[0].ident && t2.ghosts.size() == 1 && sc.cached == 0);
        const int gcell = t2.ghosts[0].tr.cell; const uint32_t gid = t2.ghosts[0].tr.id;
        CHECK(gcell >= 0 && t2.cell_owner[gcell] == gid);   // the ghost keeps its cell while it waits
        seen(t2, C1, 180, tt, false, other); tt += 1.0 / 60;
        CHECK(t2.cell_owner[gcell] == gid);
        for (int k = 0; k < 60 * int(mk360::kGhostS + 1.0); k += 30, tt += 0.5) seen(t2, C1, 180, tt, false, other);
        CHECK(t2.ghosts.empty() && t2.st.reacquired == 0 && t2.cell_owner[gcell] != gid);   // expired: its cell freed by the plan
    }
    // an identified contact outside the window without any cached picture is counted (out-uncached): the live check names those misses
    {
        mk360::Tracker t2; double tt = 0;
        for (int k = 0; k < 3; ++k, tt += 1.0 / 60) seen(t2, C0, 0, tt, true);
        t2.tracks[0].cell_ok = false;
        seen(t2, C0, 180, tt, false);
        CHECK(sc.cached == 0 && t2.st.nb_out_uncached == 1 && mk360::sample_line(t2, 0).find(":c-") != std::string::npos);
    }
}
// Item 6 / d: "random blue flashes" = a box at the canopy. The near policy (push-out to mk360min, scale) is colour-blind (friendly stamps included);
// NEAR-FADE adds: below mk360min the stamp fades (vertex col.z = opacity, xr_tactical.hlsl ps_stamp), at or below mk360fade it is not drawn.
static void test_hs5_near_fade() {
    mk360::StampPlan plan; plan.atlas = mk360::atlas_geom(2560, 1440); CHECK(plan.atlas.ok);
    const float C[3] = {0, 0, 0}, target[3] = {0, 0, -1};
    plan.has_ref = true; plan.near_min = 150; plan.near_ref = 750; plan.near_floor = 0.3f; plan.near_fade = 75;
    auto draw_at = [&](float dist, tac::Overlay& ov, mk360::Stats* st) {
        plan.draw.clear();
        mk360::StampDraw d; d.pos[2] = -dist; d.ox = -10; d.oy = -10; d.cell = 0; d.pw = 100; d.ph = 60; d.id = 1; d.live = true;
        plan.draw.push_back(d);
        return mk360::build_stamps(ov, eye_towards(C, target), plan, st);
    };
    mk360::Stats st;
    for (float dist : {20000.0f, 750.0f, 150.0f}) { tac::Overlay ov; CHECK(draw_at(dist, ov, &st) == 1); for (const tac::Vertex& v : ov.v) CHECK(v.col[2] == 1.0f); }
    CHECK(st.nb_near_faded == 0 && st.nb_near_hidden == 0);
    { tac::Overlay ov; CHECK(draw_at(112.5f, ov, &st) == 1); for (const tac::Vertex& v : ov.v) CHECK(near(v.col[2], 0.5, 1e-6)); }
    { tac::Overlay ov; CHECK(draw_at(90.0f, ov, &st) == 1); for (const tac::Vertex& v : ov.v) CHECK(near(v.col[2], 0.2, 1e-6)); }
    { tac::Overlay ov; CHECK(draw_at(75.0f, ov, &st) == 0 && ov.v.empty()); }
    { tac::Overlay ov; CHECK(draw_at(20.0f, ov, &st) == 0 && ov.v.empty()); }
    CHECK(st.nb_near_faded == 2 && st.nb_near_hidden == 2);
    { tac::Overlay ov; CHECK(draw_at(20.0f, ov, nullptr) == 0); }   // no counter: still hidden
    // off (mk360fade:0): the old behaviour, full opacity at any distance
    plan.near_fade = 0;
    { tac::Overlay ov; CHECK(draw_at(20.0f, ov, &st) == 1); for (const tac::Vertex& v : ov.v) CHECK(v.col[2] == 1.0f); }
    // fade >= min: hidden at or below the fade distance, no ramp
    plan.near_fade = 200;
    { tac::Overlay ov; CHECK(draw_at(199.0f, ov, &st) == 0); }
    { tac::Overlay ov; CHECK(draw_at(201.0f, ov, &st) == 1); for (const tac::Vertex& v : ov.v) CHECK(v.col[2] == 1.0f); }
    // a FRIENDLY (blue FRND) wingman ~100 u away through the whole plan path: faded exactly like a red one (the policy does not look at the colour)
    for (const float* col : {kBlue, kRed}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        mk360::StampPlan pl; mk360::StampCounts sc;
        Cam c = cam_yaw(0); const float W[3] = {c.C[0] + 10, c.C[1], c.C[2] - 100};
        for (int k = 0; k < 2; ++k) {
            tac::TacFrame f = frame_for(c); add_obj(f, W); CHECK(add_sprite(f, W, col, 4, false) == 0);
            mk360::update(tk, f, c.C, k / 60.0, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, k / 60.0, pl, sc);
        }
        CHECK(sc.live == 1 && near(pl.near_fade, 75.0, 0));
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(c.C, W), pl, &tk.st) == 1);
        const double d = std::sqrt(10.0 * 10.0 + 100.0 * 100.0);
        for (const tac::Vertex& v : ov.v) CHECK(near(v.col[2], (d - 75.0) / 75.0, 1e-4));
        CHECK(tk.st.nb_near_faded == 1);
    }
    // options: mk360fade parsed, range checked, described only when not the default
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;mk360:3;mk360fade:40", rest, o, why) && rest == "m02a" && o.mk360_near_fade == 40.0f);
    CHECK(tac::describe(o).find(";mk360fade=40") != std::string::npos && mk360::params_from(o).near_fade == 40.0f);
    tac::Options d0; d0.mk360 = 3; CHECK(tac::describe(d0).find("mk360fade") == std::string::npos && mk360::params_from(d0).near_fade == 75.0f);
    tac::Options b; CHECK(!tac::extract_options("m02a;mk360fade:-5", rest, b, why) && why == "bad value for mk360fade");
}
// ---------------------------------------------------------------------------------------------- headset test 6 (2026-10-30d item 7; markers-360.md section 17)
static mk360::Track* track_near(mk360::Tracker& tk, const float p[3]) { for (mk360::Track& tr : tk.tracks) if (dist3(tr.pos, p) < 100.0f) return &tr; return nullptr; }
// The step143 box: TWO bars of one colour 12 GS either side of the contact (half 1 x 10; the label "MIG-21BIS" with the right bar).
static void two_piece_box(tac::TacFrame& f, float xs, float ys, const float rgb[3], bool label) {
    tac::MarkerSprite& a = raw_sprite(f, xs - 12, ys, 1.0f, rgb); a.hh = 10;
    tac::MarkerSprite& b = raw_sprite(f, xs + 12, ys, 1.0f, rgb); b.hh = 10;
    if (label) set_label(b, xs + 16, ys - 12, xs + 60, ys - 6, 9, kGreen);
}
// 17.2 (b)/(d)/(c): step143 identity flip-flop (identity new 129 / cleared-in-view 110 for 67 tracks; contacts with a whole cached picture left
// unidentified = not drawn outside the window, e.g. 17 for 220 s). A second track (a missile / ground object / neighbour on nearly the same line of
// sight) projecting next to one piece took that piece: it was identified with the contact's colour and label within 2 frames, and the contact's own
// learned offset then pointed at the stolen piece. Before (pair_gate 0) reproduced; after: one box, matched to its contact, the other track never boxed.
static void test_hs6_piece_pair() {
    Cam c = cam_yaw(0);
    const float T[3] = {400, 1100, -8000};
    float xs, ys, w; CHECK(tac::project_ps2(c.A, T, xs, ys, w));
    float dir[3]; CHECK(mk360::screen_ray(c.A, xs + 14, ys, dir));
    const float U[3] = {c.C[0] + dir[0] * 11000, c.C[1] + dir[1] * 11000, c.C[2] + dir[2] * 11000};   // 14 GS right of T, 3 km farther
    for (float gate : {0.0f, 10.0f}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3; p.pair_gate = gate;
        mk360::StampPlan plan; mk360::StampCounts sc;
        double t = 0;
        unsigned u_ident_frames = 0;
        for (int k = 0; k < 20; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c); add_obj(f, T); add_obj(f, U);
            two_piece_box(f, xs, ys, kGreen, true);
            mk360::update(tk, f, c.C, t, p);
            mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
            mk360::Track* tu = track_near(tk, U);
            if (tu && tu->ident) ++u_ident_frames;
        }
        mk360::Track* tt = track_near(tk, T); mk360::Track* tu = track_near(tk, U);
        CHECK(tt && tu);
        if (!tt || !tu) continue;
        std::printf("  hs6 piece pair gate %.0f: other track identified %u/20 frames, contact glyphs %u, pairs %llu\n", double(gate), u_ident_frames,
                    unsigned(tt->glyphs), (unsigned long long)tk.st.pairs);
        if (gate == 0.0f) { CHECK(u_ident_frames >= 15 && tu->ident && tu->glyphs == 9); continue; }   // the theft (before)
        CHECK(u_ident_frames == 0 && !tu->ident && tu->sprite < 0);
        CHECK(tt->ident && tt->glyphs == 9 && tt->sprite >= 0 && tt->cell_ok && tt->c_q == 2);
        CHECK(tk.st.pairs == 20 && tk.ngroups == 1 && near(tk.gcx[0], xs, 1e-3) && near(tk.gcy[0], ys, 1e-3));   // the whole box, centred on its contact
        CHECK(sc.live == 1 && sc.sat == 0 && plan.draw.size() == 1 && plan.draw[0].id == tt->id && tk.st.beyond_raw_gate == 0);
        CHECK(near(tt->off[0], 0.0, 1e-3) && near(tt->off[1], 0.0, 1e-3));   // the learned offset no longer points at a piece
        CHECK(mk360::hs6_line(tk, 0).find(";pairs=20;") != std::string::npos);
    }
    // two contacts of one colour side by side (50 GS apart), each read as two pieces: each pair forms on its own contact, never across (the inner
    // pieces' union is centred 25 GS from both). Closer than ~40 GS the existing group_gap rule already merges the inner pieces (not changed here).
    {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        mk360::StampPlan plan; mk360::StampCounts sc;
        CHECK(mk360::screen_ray(c.A, xs + 50, ys, dir));
        const float B[3] = {c.C[0] + dir[0] * 8000, c.C[1] + dir[1] * 8000, c.C[2] + dir[2] * 8000};
        for (int k = 0; k < 4; ++k) {
            tac::TacFrame f = frame_for(c); add_obj(f, T); add_obj(f, B);
            two_piece_box(f, xs, ys, kRed, true); two_piece_box(f, xs + 50, ys, kRed, true);
            mk360::update(tk, f, c.C, k / 60.0, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, k / 60.0, plan, sc);
            CHECK(tk.ngroups == 2);
        }
        CHECK(tk.st.pairs == 8);
        mk360::Track* ta = track_near(tk, T); mk360::Track* tb = track_near(tk, B);
        CHECK(ta && tb && ta->ident && tb->ident && ta->glyphs == 9 && tb->glyphs == 9);
        if (ta && tb) CHECK(near(ta->off[0], 0.0, 1e-3) && near(tb->off[0], 0.0, 1e-3));
    }
    // a lone piece (the other bar not read) is still matched as before (no pair, the wider gate)
    {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        for (int k = 0; k < 3; ++k) {
            tac::TacFrame f = frame_for(c); add_obj(f, T);
            tac::MarkerSprite& a = raw_sprite(f, xs - 12, ys, 1.0f, kGreen); a.hh = 10;
            mk360::update(tk, f, c.C, k / 60.0, p);
        }
        CHECK(tk.st.pairs == 0 && tk.tracks.size() == 1 && tk.tracks[0].ident);
    }
}
// 17.2: an identified contact whose box is crossed by another colour for a few frames keeps its colour and its cached pictures (step143 contact 17:
// green, took a red box for 2 frames = recoloured, cached pictures forgotten, then cleared in view). A real colour change (>= 6 frames) still follows.
static void test_hs6_recolour_hold() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float T[3] = {-800, 1200, -9000};
    double t = 0;
    auto frame = [&](const float rgb[3]) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, T); CHECK(add_sprite(f, T, rgb, 3, false) == 0);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
    };
    frame(kGreen); frame(kGreen);
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].ident && tk.tracks[0].cell_ok && tk.tracks[0].rgb[1] > 0.9f);
    for (int k = 0; k < 5; ++k) frame(kRed);
    CHECK(tk.tracks[0].rgb[1] > 0.9f && tk.tracks[0].rgb[0] < 0.5f && tk.st.ident_recolour == 0 && tk.st.recolour_held == 1);
    frame(kGreen);   // the crossing is over: still green; its whole picture was never forgotten
    CHECK(tk.tracks[0].cell_ok && tk.tracks[0].c_q == 2 && tk.tracks[0].rgb[1] > 0.9f);
    for (int k = 0; k < 6; ++k) frame(kRed);   // the game really changed the colour (e.g. now the mission target)
    CHECK(tk.tracks[0].rgb[0] > 0.9f && tk.tracks[0].rgb[1] < 0.5f && tk.st.ident_recolour == 1);
}
// 17.3 (c): a box group matched to no contact was drawn at far_units (32,500 u) while the frames around it drew it at its contact's range: a one-frame
// depth pop. Now it borrows the range of a seen contact projecting within twice the match gate; with none near it stays at far_units.
static void test_hs6_depth_hold() {
    Cam c = cam_yaw(0);
    const float T[3] = {0, 1000, -6000};
    float xs, ys, w; CHECK(tac::project_ps2(c.A, T, xs, ys, w));
    for (float off_gs : {30.0f, 120.0f}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        mk360::StampPlan plan; mk360::StampCounts sc;
        for (int k = 0; k < 3; ++k) {
            tac::TacFrame f = frame_for(c); add_obj(f, T);
            raw_sprite(f, xs, ys, 7.5f, kRed);                // the contact's own box (matched)
            raw_sprite(f, xs + off_gs, ys, 7.5f, kYellow);    // another colour: no satellite, matched to nothing
            mk360::update(tk, f, c.C, k / 60.0, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, k / 60.0, plan, sc);
        }
        CHECK(sc.live == 2 && plan.draw.size() == 2);
        const mk360::StampDraw* un = nullptr;
        for (const mk360::StampDraw& d : plan.draw) if (d.id & mk360::kTempOwner) un = &d;
        CHECK(un != nullptr);
        if (!un) continue;
        const float r = dist3(un->pos, c.C);
        if (off_gs < 40.0f) { CHECK(near(r, 6000.0, 1.0) && tk.st.nb_depth_held == 3); }   // the contact's range (6000 u), not 32,500
        else { CHECK(near(r, 32500.0, 1.0) && tk.st.nb_depth_held == 0); }               // nothing near: unchanged
        // the direction stays the group's own ray
        float gd[3]; CHECK(mk360::screen_ray(c.A, xs + off_gs, ys, gd));
        const float v[3] = {un->pos[0] - c.C[0], un->pos[1] - c.C[1], un->pos[2] - c.C[2]};
        CHECK(near((v[0] * gd[0] + v[1] * gd[1] + v[2] * gd[2]) / r, 1.0, 1e-5));
    }
}
// 17.1 (a) LABEL-CORE: a contact only ever seen with its label clipped at the window top (q 1) is cached as its BOX only: outside the window no half
// label is drawn; the first whole picture replaces it (box + label again). The cached quad samples the core sub-rectangle of the same cell.
static void test_hs6_label_core() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float C[3] = {0, 1000, 0};
    const float P[3] = {0, 1000 + 0.3263f * 20000, -20000};   // pitch 0: box 1 GS+ inside the window top, the label above clipped
    double t = 0;
    float live_oy = 0; uint16_t live_ph = 0, live_pw = 0;
    auto fr = [&](float pitch) {
        Cam c = cam_pitch_roll(C, pitch); tac::TacFrame f = frame_for(c); add_obj(f, P);
        float xs, ys, w; const bool vis = tac::project_ps2(f.A, P, xs, ys, w) && std::fabs(xs - 2048) < 256 && std::fabs(ys - 2048) < 224;
        if (vis) { tac::MarkerSprite& m = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(m, xs + 10, ys - 16, xs + 63, ys - 11, 9, kGreen); }
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc); t += 1.0 / 60;
        if (vis && !plan.draw.empty()) { live_oy = plan.draw[0].oy; live_ph = plan.draw[0].ph; live_pw = plan.draw[0].pw; }
        return c;
    };
    fr(0); fr(0);
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].cell_ok && tk.tracks[0].c_q == 1 && tk.st.nb_core_cached == 2);
    const mk360::Track tr = tk.tracks[0];   // a copy (the tracker's vector may change below)
    // the core: the box (+- 7.5 GS) + the 2 GS margin, no label: 19 x 19 GS = 95 x 61 px; inside the live stamp's rectangle
    CHECK(tr.c_pw < live_pw && tr.c_ph <= live_ph && tr.c_pw >= 90 && tr.c_pw <= 97 && tr.c_ph >= 58 && tr.c_ph <= 63);
    CHECK(tr.c_u0 + tr.c_pw <= live_pw && tr.c_v0 + tr.c_ph <= live_ph);
    CHECK(tr.c_oy >= live_oy && near(tr.c_ox, -9.5, 0.3) && near(tr.c_oy, -9.5, 0.4));   // its top-left = the box's (- margin) about the contact
    // outside the window: that core is what is drawn, its uv inside the cell at the core's texels
    {
        Cam c = fr(-40);
        CHECK(sc.cached == 1 && plan.draw.size() == 1 && plan.draw[0].u0 == tr.c_u0 && plan.draw[0].v0 == tr.c_v0 && plan.draw[0].pw == tr.c_pw);
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(c.C, P), plan) == 1 && ov.v.size() == 6);
        if (ov.v.size() == 6) {
            uint32_t ux, uy; mk360::cell_origin(plan.atlas, plan.draw[0].cell, ux, uy);
            CHECK(near(ov.v[0].col[0], (ux + tr.c_u0 + 0.5) / plan.atlas.w, 1e-6) && near(ov.v[0].col[1], (uy + tr.c_v0 + 0.5) / plan.atlas.h, 1e-6));
            CHECK(stamp_unmirrored(ov, 0));
        }
    }
    // the first whole picture (box + whole label) replaces it, sampled from the cell's origin again
    fr(8);
    CHECK(tk.tracks[0].c_q == 2 && tk.tracks[0].c_u0 == 0 && tk.tracks[0].c_v0 == 0 && tk.tracks[0].c_pw > 300);
    // a cached draw that would sample beyond its cell is never built
    {
        mk360::StampPlan pl; pl.atlas = mk360::atlas_geom(2560, 1440);
        mk360::StampDraw d; d.pos[2] = -5000; d.cell = 0; d.pw = 100; d.ph = 50; d.u0 = uint16_t(pl.atlas.cell_w - 50); d.id = 1;
        pl.draw.push_back(d);
        tac::Overlay ov; CHECK(mk360::build_stamps(ov, eye_towards(C, P), pl) == 0);
    }
}
// Vertices of the lock diamond's OUTLINE: 4 edges tip to tip, each a thin quad (outer tips + the same tips pulled in by th) = 16 vertices.
static unsigned diamond_outline(tac::SpriteVert* v, float cx, float cy, float hx, float hy, const float rgb[3], float th = 1.5f) {
    const float tx[4] = {cx + hx, cx, cx - hx, cx}, ty[4] = {cy, cy + hy, cy, cy - hy};
    unsigned n = 0;
    for (int e = 0; e < 4; ++e) for (int j : {e, (e + 1) % 4}) for (float s : {1.0f, (std::fmin(hx, hy) - th) / std::fmin(hx, hy)}) {
        tac::SpriteVert& x = v[n++]; x.x = cx + (tx[j] - cx) * s; x.y = cy + (ty[j] - cy) * s; for (int k = 0; k < 3; ++k) x.rgba[k] = rgb[k] * 0.5f; x.rgba[3] = 0.5f;
    }
    return n;
}
// 17.4 (e) LOCK-SHAPE: the producer's pure test (a 45-degree square in a box's colour within satellite reach; never an axis-aligned box, a glyph, a
// thin bar, a shape of another colour or one far from every box), the marker it adds (shape 1), and the consumer: the diamond joins its box's stamp
// (one picture at the contact's depth, cached whole), is never matched to a contact itself, not even one projecting onto it.
static void test_hs6_lock_shape() {
    tac::SpriteVert v[64];
    tac::TacFrame f; f.valid = true;
    const float bx = 2100, by = 2000;
    { tac::MarkerSprite& m = f.mark[f.nmark++]; m.cx = bx; m.cy = by; m.hw = m.hh = 12; for (int k = 0; k < 3; ++k) m.rgb[k] = kRed[k]; }
    unsigned n = diamond_outline(v, bx - 17, by + 19, 12, 12, kRed);
    tac::LockShape r = tac::lock_shape_test(v, n, f);
    CHECK(r.ok && std::string(r.why) == "diamond" && r.box == 0 && near(r.cx, bx - 17, 1e-3) && near(r.cy, by + 19, 1e-3) && near(r.hw, 12, 1e-3));
    CHECK(near(r.dx, -17, 1e-3) && near(r.dy, 19, 1e-3) && near(r.rgb[0], 1.0, 1e-6) && near(r.rgb[1], 0.0, 1e-6));
    // a filled diamond (its 4 tips only) and a GS-anamorphic one (hx 12, hy 10) pass too
    { tac::SpriteVert t4[4]; const float px[4] = {12, 0, -12, 0}, py[4] = {0, 12, 0, -12};
      for (int i = 0; i < 4; ++i) { t4[i].x = bx + px[i]; t4[i].y = by - 30 + py[i]; for (int k = 0; k < 3; ++k) t4[i].rgba[k] = kRed[k]; }
      CHECK(tac::lock_shape_test(t4, 4, f).ok); }
    CHECK(tac::lock_shape_test(v, diamond_outline(v, bx + 20, by - 20, 12, 10, kRed), f).ok);
    // refused: an axis-aligned square (box / bracket corners), a glyph quad, a bar, another colour, far from every box, tiny, too many vertices
    { tac::SpriteVert sq[4]; const float px[4] = {-10, 10, 10, -10}, py[4] = {-10, -10, 10, 10};
      for (int i = 0; i < 4; ++i) { sq[i].x = bx - 20 + px[i]; sq[i].y = by + px[i] * 0 + py[i]; for (int k = 0; k < 3; ++k) sq[i].rgba[k] = kRed[k]; }
      CHECK(std::string(tac::lock_shape_test(sq, 4, f).why) == "corner"); }
    { tac::SpriteVert g4[4]; const float px[4] = {0, 5, 5, 0}, py[4] = {0, 0, 7, 7};
      for (int i = 0; i < 4; ++i) { g4[i].x = bx + 20 + px[i]; g4[i].y = by + py[i]; for (int k = 0; k < 3; ++k) g4[i].rgba[k] = kGreen[k]; }
      CHECK(!tac::lock_shape_test(g4, 4, f).ok); }
    CHECK(std::string(tac::lock_shape_test(v, diamond_outline(v, bx - 17, by + 19, 20, 6, kRed), f).why) == "aspect");
    CHECK(std::string(tac::lock_shape_test(v, diamond_outline(v, bx - 17, by + 19, 12, 12, kGreen), f).why) == "no-box-in-reach");
    CHECK(std::string(tac::lock_shape_test(v, diamond_outline(v, bx + 90, by, 12, 12, kRed), f).why) == "no-box-in-reach");
    CHECK(std::string(tac::lock_shape_test(v, diamond_outline(v, bx - 17, by + 19, 2, 2, kRed, 0.5f), f).why) == "size");
    CHECK(std::string(tac::lock_shape_test(v, 3, f).why) == "few-vertices");
    CHECK(std::string(tac::lock_shape_test(v, 65, f).why) == "many-vertices");
    // the marker it adds: shape 1, never a label's box (add_labels skips it), counted
    n = diamond_outline(v, bx - 17, by + 19, 12, 12, kRed); r = tac::lock_shape_test(v, n, f);
    CHECK(tac::add_lock_shape(f, r, n) && f.nmark == 2 && f.mark[1].shape == 1 && f.lock_shapes == 1 && near(f.mark[1].hw, 12, 1e-3));
    { tac::Cluster lab[1]; lab[0].x0 = bx - 24; lab[0].x1 = bx - 12; lab[0].y0 = by + 30; lab[0].y1 = by + 36; lab[0].n = 8; for (int k = 0; k < 3; ++k) lab[0].rgb[k] = 8 * kGreen[k];
      tac::add_labels(f, 0, lab, 1); CHECK(f.mark[1].glyphs == 0 && f.mark[0].glyphs == 2); }
    // the consumer: box (two pieces on the contact) + the diamond lower left -> ONE stamp of the contact holding both, cached whole
    {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        mk360::StampPlan plan; mk360::StampCounts sc;
        Cam c = cam_yaw(0);
        const float T[3] = {-900, 1150, -12000};
        float xs, ys, w; CHECK(tac::project_ps2(c.A, T, xs, ys, w));
        float dir[3]; CHECK(mk360::screen_ray(c.A, xs - 17, ys + 19, dir));
        const float V[3] = {c.C[0] + dir[0] * 15000, c.C[1] + dir[1] * 15000, c.C[2] + dir[2] * 15000};   // another contact projecting ONTO the diamond
        for (int k = 0; k < 4; ++k) {
            tac::TacFrame fr = frame_for(c); add_obj(fr, T); add_obj(fr, V);
            two_piece_box(fr, xs, ys, kRed, true);
            tac::SpriteVert dv[64]; const unsigned dn = diamond_outline(dv, xs - 17, ys + 19, 12, 12, kRed);
            const tac::LockShape ls = tac::lock_shape_test(dv, dn, fr); CHECK(ls.ok && tac::add_lock_shape(fr, ls, dn));
            mk360::update(tk, fr, c.C, k / 60.0, p); mk360::plan_stamps(tk, fr, p, 2560, 1440, true, k / 60.0, plan, sc);
            CHECK(tk.ngroups == 2 && sc.sat == 1 && sc.live == 1 && sc.cuts == 0 && plan.draw.size() == 1);
            if (!plan.clear.empty()) CHECK(covers(plan.clear[0], xs - 29, ys - 10, xs + 13, ys + 31));   // box + diamond, nothing left on the flat quad
        }
        mk360::Track* tt = track_near(tk, T); mk360::Track* tv = track_near(tk, V);
        CHECK(tt && tv && tt->ident && !tv->ident && tv->sprite < 0 && tt->cell_ok && tt->c_q == 2);
        if (tt) CHECK(plan.draw[0].id == tt->id && dist3(plan.draw[0].pos, T) < 1.0f);   // at the contact's depth (not the HUD layer)
        CHECK(tk.st.lock_shapes == 4 && tk.st.near_none + tk.st.near_lt8 + tk.st.near_lt16 + tk.st.near_lt32 + tk.st.near_lt64 + tk.st.near_ge64 == 8);
        CHECK(mk360::hs6_line(tk, 0).find(";lock-shapes=4") != std::string::npos);
    }
    // options: on by default with mk360:3, mk360lock:0 off, mk360cen:1 the census flag; described only when not the default; bad values refused
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;mk360:3;mk360lock:0", rest, o, why) && rest == "m02a" && !o.mk360_lock && tac::producer_flags_for(o) == tac::kProducerOn);
    CHECK(tac::describe(o).find(";mk360lock=0") != std::string::npos);
    CHECK(tac::extract_options("m02a;mk360:3;mk360cen:1", rest, o, why) && o.mk360_census && o.mk360_lock);
    CHECK(tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerLockShape | tac::kProducerLockCensus) && tac::describe(o).find(";mk360cen=1") != std::string::npos);
    CHECK(tac::extract_options("m02a;mk360:1;mk360cen:1", rest, o, why) && tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerHideAll | tac::kProducerLockCensus));
    CHECK(tac::extract_options("m02a;mk360cen:1", rest, o, why) && tac::producer_flags_for(o) == 0u);   // mk360 off: nothing at all
    CHECK(!tac::extract_options("m02a;mk360:3;mk360lock:2", rest, o, why) && why == "bad value for mk360lock");
    // the census line (step5-producer.log, after the 21-byte "gpu-copy-step1;event=" prefix: < 426 bytes even with extreme values)
    {
        tac::CensusFacts cf; cf.frame = 123456789012ull; cf.ord = 987654321ull; cf.after_box = 37; cf.cls = "instrument"; cf.vs8 = 0x534f29ca; cf.ps8 = 0xd34ee153;
        cf.indexed = true; cf.count = 48; cf.nv = 16; cf.x0 = -12345.6f; cf.y0 = 2034.5f; cf.x1 = 99999.9f; cf.y1 = 2071.5f; cf.rgb[0] = 1;
        cf.q.add(1.0f); cf.q.add(0.99812f); cf.shape = "no-box-in-reach"; cf.box = 3; cf.dx = -17; cf.dy = 19; cf.read_why = "vertex-outside-view";
        char b[426]; const int k = tac::format_lock_census(b, sizeof b, cf);
        const std::string s = b;
        CHECK(k > 0 && k < 426 && s.find("tac-lock-census;f=123456789012;ord=987654321;after-box=37;cls=instrument;vs=534f29ca;ps=d34ee153;idx=1;cnt=48;nv=16;") == 0);
        CHECK(s.find(";q=0.9981,1(off:1);shape=no-box-in-reach;box=3(d:-17.0,19.0);read=vertex-outside-view") != std::string::npos);
    }
}
// 17.1 (a): the reader no longer divides x, y by q (VS 534f29ca: the division cancels); q is kept for the diagnostic. QStat + the mk360-stamp line.
static void test_hs6_stamp_line() {
    tac::QStat q; CHECK(q.n == 0);
    q.add(1.0f); q.add(1.0f); CHECK(q.off == 0 && near(q.lo, 1, 0) && near(q.hi, 1, 0));
    q.add(0.998f); q.add(std::nanf("")); CHECK(q.n == 3 && q.off == 1 && near(q.lo, 0.998, 1e-6) && near(q.hi, 1.0, 0));
    tac::SpriteVert sv; CHECK(sv.q == 1.0f);
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    CHECK(mk360::stamp_line(tk, 5).find("event=mk360-stamp;t-ms=5;none") != std::string::npos);
    const float C[3] = {0, 1000, 0};
    const float P[3] = {0, 1000 + 0.3263f * 20000, -20000};
    for (int k = 0; k < 2; ++k) {
        Cam c = cam_pitch_roll(C, 0); tac::TacFrame f = frame_for(c); add_obj(f, P);
        f.label_q.add(0.998f); f.label_q.add(1.0f); f.box_q.add(1.0f);
        float xs, ys, w; CHECK(tac::project_ps2(f.A, P, xs, ys, w));
        tac::MarkerSprite& m = raw_sprite(f, xs, ys, 7.5f, kRed); set_label(m, xs + 10, ys - 16, xs + 63, ys - 11, 9, kGreen);
        mk360::update(tk, f, c.C, k / 60.0, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, k / 60.0, plan, sc);
    }
    const std::string l = mk360::stamp_line(tk, 7);
    std::printf("  %s\n", l.c_str());
    CHECK(l.find("event=mk360-stamp;t-ms=7;id=") != std::string::npos && l.find(";q=1;label-whole=0;cut=0;complete=1;glyphs=9;box=") != std::string::npos);
    CHECK(l.find(";label-q=0.998,1(off:1/2)") != std::string::npos && l.find(";box-q=1,1(off:0/1)") != std::string::npos && l.size() < 480);
    CHECK(mk360::stamp_line(tk, 8).find(";none") != std::string::npos);   // one sample per period
    const std::string h = mk360::hs6_line(tk, 9);
    CHECK(h.find("event=summary-mk360-hs6;t-ms=9;pairs=0;recolour-held=0;depth-held=0;core-cached=2;lock-shapes=0") != std::string::npos && h.size() < 480);
}
// CENSUS-AIM (markers-360.md section 18.2): the step146 census frames started at the GREATER PS 4caf6e2d cloud draws (near-white, 220 vertices) and spent their
// 20 lines on green HUD instruments far from any box. A frame now starts only with a COLOURED box marker in the gunsight zone; instruments are logged only near a box.
static void test_census_aim() {
    auto mark = [](tac::TacFrame& f, float cx, float cy, float r, float g, float b, uint8_t shape = 0) {
        tac::MarkerSprite& m = f.mark[f.nmark++]; m = tac::MarkerSprite{}; m.cx = cx; m.cy = cy; m.hw = 12; m.hh = 12; m.rgb[0] = r; m.rgb[1] = g; m.rgb[2] = b; m.shape = shape; };
    tac::TacFrame f; f.valid = true;
    CHECK(!tac::census_trigger(f));                                        // no box yet
    mark(f, 2048, 2048, 1.00f, 0.98f, 0.89f);                              // the cloud sprite's colour (rgb=1.00,0.98,0.89) at the centre: never
    CHECK(!tac::census_trigger(f));
    mark(f, 2300, 2100, 1.0f, 0.2f, 0.2f);                                 // a red box far right of the gunsight
    CHECK(!tac::census_trigger(f));
    mark(f, 2050, 2000, 1.0f, 0.2f, 0.2f, 1);                              // a moved diamond (shape 1) does not start a frame
    CHECK(!tac::census_trigger(f));
    mark(f, 2160, 2120, 1.0f, 0.25f, 0.3f);                                // a red box inside the 160 x 120 GS zone
    CHECK(tac::census_trigger(f));
    tac::TacFrame g; g.valid = true; mark(g, 1940, 1960, 0.29f, 1.0f, 0.22f); // green (friendly HUD colour) is coloured too
    CHECK(tac::census_trigger(g));
    tac::TacFrame h; h.valid = true; mark(h, 2209, 2048, 1, 0, 0); mark(h, 2048, 2169, 1, 0, 0); CHECK(!tac::census_trigger(h));   // just outside
    // instruments: the step146 green lines (bbox centres 1180,363 / 2048,2046 / 1120,448) vs a box at 2160,2120
    CHECK(!tac::census_near_box(h, 1180, 363));
    CHECK(tac::census_near_box(f, 2120, 2120));                            // 40 GS left of the box: the seeker diamond's place
    CHECK(tac::census_near_box(f, 2160 - 128, 2120 + 96));
    CHECK(!tac::census_near_box(h, 2209 + 129, 2048));                    // beyond twice the satellite reach (128 GS) of h's boxes
    CHECK(tac::census_near_box(h, 2209 + 128, 2048 + 96));
    CHECK(!tac::census_near_box(f, 1180, 363));
    { tac::TacFrame only; only.valid = true; mark(only, 2050, 2000, 1, 0, 0, 1); CHECK(!tac::census_near_box(only, 2050, 2000)); }   // a diamond is not a box
    CHECK(!tac::census_near_box(f, std::nanf(""), 2000));
    // the boxes line: frame, count, at most 6 markers; under the 426-byte census budget with extreme values
    char b[426]; int k = tac::format_census_boxes(b, sizeof b, 4242, f);
    std::string s(b);
    CHECK(k > 0 && size_t(k) == s.size() && s.find("tac-lock-census-boxes;f=4242;n=4;m0=2048.0,2048.0,12.0,12.0,1.00,0.98,0.89,s0;") == 0);
    CHECK(s.find(";m3=2160.0,2120.0,12.0,12.0,1.00,0.25,0.30,s0") != std::string::npos && s.find(";m2=") != std::string::npos && s.find("s1") != std::string::npos);
    tac::TacFrame big; big.valid = true; for (int i = 0; i < 9; ++i) mark(big, -123456.7f, 98765.4f, 1, 1, 1);
    k = tac::format_census_boxes(b, sizeof b, 18446744073709551615ull, big);
    s = b; CHECK(k > 0 && k < 426 && s.find(";m5=") != std::string::npos && s.find(";m6=") == std::string::npos);
    char tiny[12]; CHECK(tac::format_census_boxes(tiny, sizeof tiny, 1, f) == 11);
    // runLOCK2 follow-up: topology + the first 4 vertices (the 3-vertex target-direction wedge of f=6255: 1893-1960 x 1956-2001)
    tac::CensusFacts cf; cf.cls = "instrument"; cf.vs8 = 0x534f29ca; cf.ps8 = 0xd34ee153; cf.count = 3; cf.nv = 3; cf.shape = "few-vertices";
    cf.topo = 4; cf.nvl = 3; cf.vx[0] = 1893.3f; cf.vy[0] = 1956.4f; cf.vx[1] = 1959.9f; cf.vy[1] = 1980.0f; cf.vx[2] = 1900.0f; cf.vy[2] = 2001.2f;
    char lb[426]; k = tac::format_lock_census(lb, sizeof lb, cf); s = lb;
    CHECK(k > 0 && size_t(k) == s.size() && s.find(";read=ok;topo=4;v=1893,1956/1960,1980/1900,2001") != std::string::npos);
    cf.nvl = 0; k = tac::format_lock_census(lb, sizeof lb, cf); s = lb; CHECK(s.find(";topo=4;v=-") != std::string::npos);
    tac::CensusFacts big2; big2.frame = 18446744073709551615ull; big2.ord = 18446744073709551615ull; big2.after_box = 4000000000u; big2.cls = "lock-moved-xx";
    big2.x0 = big2.y0 = -1e30f; big2.x1 = big2.y1 = 1e30f; big2.nvl = 4; for (int i = 0; i < 4; ++i) { big2.vx[i] = -1e9f; big2.vy[i] = 1e9f; }
    big2.shape = "no-box-in-reach-x"; big2.read_why = "vertex-outside-view-xxxxxxxx"; big2.topo = 4000000000u;
    k = tac::format_lock_census(lb, sizeof lb, big2); CHECK(k > 0 && k < 426 && size_t(k) == std::strlen(lb));
    std::printf("census line max %d bytes\n", k);
}

// ---------------------------------------------------------------------------------------------------------------- headset test 8 (section 21)
static bool is_green(const tac::WorldMarker& w) { return rgb_is(w, kProvGreen); }
// (1a) A game box that the tracker could not match (24 GS from the projection: outside the 20 GS match gate) is still the game's box on that contact: no provisional box
// beside it (no doubles). 60 GS away it is another object's box: the provisional box stays.
static void test8_no_double_beside_game_box() {
    for (int lateral : {0, 462, 1200}) {   // 0 = no game box; 462 u at 8,000 u = ~24 GS; 1,200 u = ~62 GS
        Sim s; s.p.seek = 0.0f;   // no offset learning: the 24 GS box stays outside the 20 GS match gate (with learning the tracker would match it)
        const float rel[3] = {0, 0, -8000};
        for (int k = 0; k < 60; ++k) s.step(rel, 600.0f);   // 600 u/s, same velocity, beyond form_range: presumed enemy
        CHECK(s.out.size() == 1 && is_green(s.out[0]));
        unsigned drawn = 0;
        for (int k = 0; k < 30; ++k) {
            s.C[2] -= 600.0f / 60.0f; Cam c = cam_at(s.C); tac::TacFrame f = frame_for(c);
            const float q[3] = {s.C[0] + rel[0], s.C[1] + rel[1], s.C[2] + rel[2]}, side[3] = {q[0] + float(lateral), q[1], q[2]};
            add_obj(f, q);
            if (lateral) add_sprite(f, side, kRed, 3);
            mk360::update(s.tk, f, c.C, s.t, s.p); mk360::collect(s.tk, f, c.C, s.p, s.out, s.fc); s.t += 1.0 / 60;
            drawn += s.fc.prov;
            if (lateral == 462) CHECK(s.tk.tracks.size() == 1 && s.tk.tracks[0].sprite < 0);   // the tracker did not match it
        }
        if (lateral == 462) CHECK(drawn == 0 && s.tk.st.prov_ref_box >= 30);
        else CHECK(drawn == 30 && s.tk.st.prov_ref_box == 0);
    }
}
// (1b) The game boxed it and the box blinked off (ident cleared in view): no provisional box for 2 s; an enemy never; a friend after the hold, blue.
static void test8_hold_after_game_box() {
    Sim s;
    const float rel[3] = {0, 0, -8000};
    for (int k = 0; k < 40; ++k) s.step(rel, 600.0f, kBlue);
    CHECK(s.tk.tracks[0].ident && s.tk.tracks[0].team == tac::team_friend && s.out.empty());
    unsigned first = 0, k2 = 0;
    for (; k2 < 260; ++k2) { s.step(rel, 600.0f); if (s.fc.prov && !first) first = k2 + 1; }
    CHECK(s.tk.st.ident_cleared_in_view == 1 && first >= 110 && first <= 140);   // 9 frames to clear + 2 s hold (120 frames)
    CHECK(s.out.size() == 1 && rgb_is(s.out[0], kBlue) && s.tk.st.prov_ref_hold > 100);
}
// (1c) Classify a wingman from the earliest evidence: first seen near the camera and in formation = friend after form_quick_s (2 s), never green, never grey after
// that; the latch is permanent: flying out of formation range (and far beyond) stays blue.
static void test8_quick_formation_never_green() {
    Sim s;
    unsigned green = 0, blue = 0;
    float rel[3] = {337, 80, -300};   // Pixy: ~450 u from the camera at the first sighting
    for (int k = 0; k < 90 * 60; ++k) {
        const double u = std::min(1.0, std::max(0.0, (double(k) / 60.0 - 10.0) / 40.0));   // 10 s in formation, then drifts out to ~7,000 u over 40 s (175 u/s relative)
        rel[0] = 337.0f + float(u) * 5000.0f; rel[2] = -300.0f - float(u) * 5000.0f;
        s.step(rel, 1100.0f);
        for (const tac::WorldMarker& w : s.out) { if (is_green(w)) ++green; if (rgb_is(w, kBlue)) ++blue; }
        if (k == 3 * 60) CHECK(s.out.size() == 1 && rgb_is(s.out[0], kBlue) && s.tk.st.form_quick == 1);
    }
    CHECK(green == 0 && blue > 80 * 60 && s.tk.st.form_confirmed == 1 && s.tk.tracks.size() == 1 && s.tk.tracks[0].form_friend);
    // a contact first seen far away (> form_first_range) keeps the slow rule: grey at 5 s
    Sim t;
    const float far_rel[3] = {1500, 0, -1500};
    for (int k = 0; k < 5 * 60; ++k) t.step(far_rel, 1100.0f);
    CHECK(t.out.size() == 1 && rgb_is(t.out[0], t.p.prov_unknown_rgb) && t.tk.st.form_quick == 0);
}
// (1d) A friend lost (no draws for 3 s while frames keep arriving) and re-seen at the same offset from the camera is a friend at once (friend ghost), not green.
static void test8_friend_ghost() {
    Sim s;
    const float rel[3] = {300, 30, -500};
    for (int k = 0; k < 5 * 60; ++k) s.step(rel, 1100.0f);
    CHECK(s.out.size() == 1 && rgb_is(s.out[0], kBlue));
    for (int k = 0; k < 3 * 60; ++k) {   // the game stops drawing it: frames without it
        s.C[2] -= 1100.0f / 60; Cam c = cam_at(s.C); tac::TacFrame f = frame_for(c);
        mk360::update(s.tk, f, c.C, s.t, s.p); mk360::collect(s.tk, f, c.C, s.p, s.out, s.fc); s.t += 1.0 / 60;
    }
    CHECK(s.tk.tracks.empty() && s.tk.fghosts.size() == 1 && s.out.empty());
    const float rel2[3] = {380, 60, -560};
    unsigned green = 0;
    for (int k = 0; k < 3 * 60; ++k) { s.step(rel2, 1100.0f); for (const tac::WorldMarker& w : s.out) if (is_green(w)) ++green; }
    CHECK(green == 0 && s.out.size() == 1 && rgb_is(s.out[0], kBlue) && s.tk.st.freacq == 1 && s.tk.fghosts.empty());
    // a contact far from the ghost's offset is not the wingman
    Sim u;
    for (int k = 0; k < 5 * 60; ++k) u.step(rel, 1100.0f);
    for (int k = 0; k < 60; ++k) { u.C[2] -= 1100.0f / 60; Cam c = cam_at(u.C); tac::TacFrame f = frame_for(c); mk360::update(u.tk, f, c.C, u.t, u.p); u.t += 1.0 / 60; }
    const float far_rel[3] = {0, 0, -9000};
    for (int k = 0; k < 120; ++k) u.step(far_rel, 600.0f);
    CHECK(u.tk.st.freacq == 0 && u.out.size() == 1 && is_green(u.out[0]));
}
// (1e) One aircraft whose parts are tracked as two contacts (150 u apart: beyond the 60 u cluster radius) gets ONE provisional box; two aircraft 800 u apart two.
static void test8_one_box_per_aircraft() {
    for (float sep : {150.0f, 800.0f}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        Cam c = cam_yaw(0); double t = 0;
        for (int k = 0; k < 40; ++k, t += 1.0 / 60) {
            tac::TacFrame f = frame_for(c);
            const float a[3] = {0, 1000, -9000.0f + 4.0f * float(k)}, b[3] = {sep, 1000, -9000.0f + 4.0f * float(k)};
            add_obj(f, a); add_obj(f, b);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        }
        CHECK(tk.tracks.size() == 2);
        if (sep < 500) CHECK(out.size() == 1 && tk.st.prov_ref_dup > 0);
        else CHECK(out.size() == 2 && tk.st.prov_ref_dup == 0);
    }
    // a game-boxed contact beside a provisional one: the game's box takes the aircraft
    mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true;
    std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    Cam c = cam_yaw(0); double t = 0;
    for (int k = 0; k < 40; ++k, t += 1.0 / 60) {
        tac::TacFrame f = frame_for(c);
        const float a[3] = {0, 1000, -9000.0f + 4.0f * float(k)}, b[3] = {150, 1000, -9000.0f + 4.0f * float(k)};
        add_obj(f, a); add_obj(f, b); add_sprite(f, a, kRed, 3);
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
    }
    CHECK(tk.tracks.size() == 2 && out.empty() && tk.st.prov_ref_dup + tk.st.prov_ref_box > 0);
}
// (1f) Debris of a killed (game-boxed) aircraft gets no provisional box; the same aircraft re-tracked after a flicker (same velocity) is not debris.
static void test8_wreck_gets_no_box() {
    for (int debris = 0; debris < 2; ++debris) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        Cam c = cam_yaw(0); double t = 0;
        float x = 0, y = 1000;
        for (int k = 0; k < 90; ++k, t += 1.0 / 60) {   // a boxed enemy at 9,000 u, 300 u/s across
            tac::TacFrame f = frame_for(c);
            const float q[3] = {x, y, -9000}; x += 5.0f;
            add_obj(f, q); add_sprite(f, q, kRed, 3);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        }
        CHECK(tk.tracks.size() == 1 && tk.tracks[0].ident);
        for (int k = 0; k < 30; ++k, t += 1.0 / 60) {   // 0.5 s without its draws (frames keep arriving)
            tac::TacFrame f = frame_for(c); mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc); x += 5.0f;
        }
        CHECK(tk.tracks.empty() && tk.deaths.size() == 1);
        float yy = y;
        unsigned drawn = 0;
        for (int k = 0; k < 120; ++k, t += 1.0 / 60) {   // the heir: debris falling at 600 u/s, or the same aircraft at its own velocity
            tac::TacFrame f = frame_for(c);
            const float q[3] = {x + (debris ? 0.0f : 5.0f * float(k)), yy, -9000};
            if (debris) yy -= 10.0f;
            add_obj(f, q);
            mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
            drawn += fc.prov;
        }
        CHECK(tk.tracks.size() == 1 && tk.tracks[0].frames == 120);
        if (debris) CHECK(drawn == 0 && tk.tracks[0].wreck && tk.st.wrecks == 1 && tk.st.prov_ref_wreck > 50);
        else CHECK(drawn > 80 && !tk.tracks[0].wreck && tk.st.wrecks == 0);
    }
}
// (2) Cockpit guard: a provisional box is never drawn at a non-finite / origin / stale position, nor within near_fade of the game camera; it fades in up to near_min.
static void test8_cockpit_guard() {
    Cam c = cam_yaw(0);
    auto run = [&](float px, float py, float pz, float fx, float fy, float fz) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true;
        std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
        mk360::Track tr; tr.id = 1; tr.seen_now = true; tr.frames = 30; tr.vel_valid = true; tr.vel[0] = 300; tr.t_first = -10;
        tr.pos[0] = px; tr.pos[1] = py; tr.pos[2] = pz; tr.posf[0] = fx; tr.posf[1] = fy; tr.posf[2] = fz;
        tk.tracks.push_back(tr); tk.last_t = 0;
        tac::TacFrame f = frame_for(c);
        mk360::collect_prov(tk, f, c.C, p, out, fc);
        return std::make_pair(out, tk.st);
    };
    { auto r = run(0, 1000, -8000, 0, 1000, -8000); CHECK(r.first.size() == 1 && near(r.first[0].alpha, 1.0, 1e-6)); }                     // control
    { auto r = run(0, 0, 0, 0, 0, 0); CHECK(r.first.empty() && r.second.prov_ref_bad == 1); }                                                // the world origin
    { auto r = run(NAN, 1000, -8000, NAN, 1000, -8000); CHECK(r.first.empty() && r.second.prov_ref_bad == 1); }                              // not finite
    { auto r = run(0, 1000, -8000, 0, 1000, -7500); CHECK(r.first.empty() && r.second.prov_ref_bad == 1); }                                 // display / frame position 500 u apart
    { auto r = run(0, 1000, -8000, 0, 1000, -7900); CHECK(r.first.size() == 1); }                                                            // 100 u: fine
    { auto r = run(0, 1000, -60, 0, 1000, -60); CHECK(r.first.empty() && r.second.prov_ref_near == 1); }                                     // 60 u: in the cockpit
    { auto r = run(0, 1000, -75, 0, 1000, -75); CHECK(r.first.empty() && r.second.prov_ref_near == 1); }                                     // hidden at <= near_fade
    { auto r = run(0, 1000, -112.5f, 0, 1000, -112.5f); CHECK(r.first.size() == 1 && near(r.first[0].alpha, 0.5, 0.01)); }                    // half way up the fade
    { auto r = run(0, 1000, -400, 0, 1000, -400); CHECK(r.first.size() == 1 && near(r.first[0].alpha, 1.0, 1e-6)); }
    // the camera itself missing or not finite: nothing
    { mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true; std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
      mk360::Track tr; tr.id = 1; tr.seen_now = true; tr.frames = 30; tr.vel_valid = true; tr.vel[0] = 300; tr.pos[2] = -8000; tr.posf[2] = -8000; tk.tracks.push_back(tr);
      tac::TacFrame f = frame_for(c); const float bad[3] = {NAN, 0, 0};
      mk360::collect_prov(tk, f, bad, p, out, fc); CHECK(out.empty() && tk.st.prov_ref_bad == 1); }
    // mk360fade:0 turns the near refusal off (the old behaviour)
    { mk360::Tracker tk; mk360::Params p; p.mode = 3; p.prov = true; p.near_fade = 0.0f; std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
      mk360::Track tr; tr.id = 1; tr.seen_now = true; tr.frames = 30; tr.vel_valid = true; tr.vel[0] = 300; tr.pos[1] = 1000; tr.pos[2] = -60; tr.posf[1] = 1000; tr.posf[2] = -60;
      tr.t_first = -10; tk.tracks.push_back(tr); tk.last_t = 0; tac::TacFrame f = frame_for(c);
      mk360::collect_prov(tk, f, c.C, p, out, fc); CHECK(out.size() == 1); }
    // the box itself carries the alpha into its stroke colour
    { tac::WorldMarker w; w.alpha = 0.25f; CHECK(near(w.alpha, 0.25, 1e-6)); tac::WorldMarker d; CHECK(near(d.alpha, 1.0, 1e-6)); }
    // the iff line carries the refusal counters and stays inside the log line bound
    { Sim s; const float rel[3] = {0, 0, -8000}; for (int k = 0; k < 30; ++k) s.step(rel, 600.0f);
      const std::string l = mk360::iff_line(s.tk, s.p, 5);
      CHECK(l.find(";refused=box:0,hold:0,near:0,bad:0,dup:0,wreck:0;quick=0,fghost:0,reacq:0,wrecks:0") != std::string::npos && l.size() < 400); }
}
// (4) LOCK-DUMP (markers-360.md 21.6): the game's own box primitive (step146 census: one indexed draw, 9 indices, 8 vertices, topology 5, a 16 x 24 GS ring:
// the outer corners first) rotated 45 degrees is a diamond ring of the same primitive; lock_shape_test takes it (observed-shape case, step180 had no
// diamond vertices on record), but not the box itself, its other pieces or a wedge. Plus the dump's gates, signature table, line format and option.
static unsigned box_ring8(tac::SpriteVert* v, float cx, float cy, float hx, float hy, float th, bool rotated, const float rgb[3]) {
    // outer corner order of the logged box: (-,-) (+,-) (-,+) (+,+), then the inner four
    const float sx[4] = {-1, 1, -1, 1}, sy[4] = {-1, -1, 1, 1};
    unsigned n = 0;
    for (float inset : {0.0f, th}) for (int i = 0; i < 4; ++i) {
        float x = sx[i] * (hx - inset), y = sy[i] * (hy - inset);
        if (rotated) { // the same strip order on the rhombus: outer tips (-x) (-y) (+y) (+x), then the inner tips pulled in by th
            const float tx[4] = {-1, 0, 0, 1}, ty[4] = {0, -1, 1, 0};
            x = tx[i] * (hx - inset); y = ty[i] * (hy - inset);
        }
        tac::SpriteVert& o = v[n++]; o.x = cx + x; o.y = cy + y; for (int k = 0; k < 3; ++k) o.rgba[k] = rgb[k]; o.rgba[3] = 0.5f; o.q = 1.0f;
    }
    return n;
}
static void test_lock_dump() {
    tac::SpriteVert v[16];
    tac::TacFrame f; f.valid = true;
    const float bx = 2100, by = 2000;
    { tac::MarkerSprite& m = f.mark[f.nmark++]; m.cx = bx; m.cy = by; m.hw = 8; m.hh = 12; for (int k = 0; k < 3; ++k) m.rgb[k] = kRed[k]; }
    // the diamond ring of the box primitive, several sizes / thicknesses: accepted, in the box's colour, within reach
    for (float hx : {8.0f, 12.0f, 16.0f}) for (float th : {0.75f, 1.0f, 1.5f}) {
        const unsigned n = box_ring8(v, bx - 17, by + 19, hx, hx * 1.5f, th, true, kRed);
        const tac::LockShape r = tac::lock_shape_test(v, n, f);
        CHECK(n == 8 && r.ok && r.box == 0 && near(r.hw, hx, 1e-3));
    }
    // the same ring NOT rotated is the game's box: refused ("corner"); a lone outer-corner quad too
    CHECK(std::string(tac::lock_shape_test(v, box_ring8(v, bx - 17, by + 19, 8, 12, 1.0f, false, kRed), f).why) == "corner");
    // dump gates: only a frame with a RED box triggers it (a green one does not)
    CHECK(tac::lock_dump_trigger(f));
    { tac::TacFrame g; g.valid = true; tac::MarkerSprite& m = g.mark[g.nmark++]; m.cx = bx; m.cy = by; m.hw = 8; m.hh = 12; for (int k = 0; k < 3; ++k) m.rgb[k] = kGreen[k];
      CHECK(!tac::lock_dump_trigger(g));
      tac::TacFrame e; e.valid = true; CHECK(!tac::lock_dump_trigger(e));
      m.shape = 1; for (int k = 0; k < 3; ++k) m.rgb[k] = kRed[k]; CHECK(!tac::lock_dump_trigger(g)); }   // a moved diamond (shape 1) is not a box
    // one line per signature, a bounded table
    { tac::LockDumpState st;
      CHECK(tac::lock_dump_new_sig(st, "size", 8, 9, 12.0f, 18.0f) && !tac::lock_dump_new_sig(st, "size", 8, 9, 12.4f, 18.4f) && tac::lock_dump_new_sig(st, "tips", 8, 9, 12.0f, 18.0f));
      CHECK(tac::lock_dump_new_sig(st, "size", 8, 9, 40.0f, 18.0f) && st.nsig == 3);
      for (unsigned i = 0; i < 200; ++i) tac::lock_dump_new_sig(st, "x", i, i, float(i), float(i));
      CHECK(st.nsig == tac::kLockDumpSigs); }
    // line format: fits the log bound with 16 vertices at extreme values, names the reason and the geometry
    {
        tac::LockDumpFacts c; c.frame = 123456789012ull; c.ord = 987654321ull; c.why = "no-box-in-reach"; c.vs8 = 0x534f29ca; c.ps8 = 0xd34ee153; c.topo = 5; c.count = 9; c.indexed = true; c.nv = 8;
        c.after_box = 37; c.rgb[0] = 1; c.nvl = 16; for (unsigned i = 0; i < 16; ++i) { c.vx[i] = -12345.6f + float(i); c.vy[i] = 9999.9f; }
        char b[426]; const int k = tac::format_lock_dump(b, sizeof b, c, -12345.6f, 2034.5f, 99999.9f, 2071.5f);
        const std::string s = b;
        CHECK(k > 0 && k < 426 && s.find("tac-lock-dump;f=123456789012;ord=987654321;why=no-box-in-reach;after-box=37;vs=534f29ca;ps=d34ee153;idx=1;cnt=9;topo=5;nv=8;") == 0);
        CHECK(s.find(";v=") != std::string::npos);
        tac::LockDumpFacts d; d.why = "tips"; d.nvl = 2; d.vx[0] = 2092; d.vy[0] = 2065; d.vx[1] = 2108; d.vy[1] = 2065;
        char b2[426]; tac::format_lock_dump(b2, sizeof b2, d, 2092, 2065, 2108, 2089);
        CHECK(std::string(b2).find(";half=8.0,12.0;rgb=0.00,0.00,0.00;v=2092.0,2065.0/2108.0,2065.0") != std::string::npos);
        char tiny[40]; CHECK(tac::format_lock_dump(tiny, sizeof tiny, d, 0, 0, 1, 1) < 40);
    }
    // option mk360ld:1: producer flag bit 5 with mk360:3 / mk360:1, described only when set, nothing with mk360 off
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("m02a;mk360:3;mk360ld:1", rest, o, why) && o.mk360_lockdump && rest == "m02a");
    CHECK(tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerLockShape | tac::kProducerLockDump) && tac::describe(o).find(";mk360ld=1") != std::string::npos);
    CHECK(tac::extract_options("m02a;mk360:3;mk360cen:1;mk360ld:1", rest, o, why) && tac::producer_flags_for(o) == (tac::kProducerOn | tac::kProducerLockShape | tac::kProducerLockCensus | tac::kProducerLockDump));
    CHECK(tac::extract_options("m02a;mk360ld:1", rest, o, why) && tac::producer_flags_for(o) == 0u);
    CHECK(!tac::extract_options("m02a;mk360:3;mk360ld:2", rest, o, why) && why == "bad value for mk360ld");
    tac::Options d0; CHECK(tac::describe(d0).find("mk360ld") == std::string::npos);
}
// (3) LOCK-WHY: every verdict text of lock_shape_test maps to its own bucket, anything else to "other", and the formatted line fits the log.
static void test8_lock_why() {
    const char* names[] = {"diamond", "few-vertices", "many-vertices", "off-screen", "size", "aspect", "corner", "inside", "tips", "no-box-in-reach"};
    for (unsigned i = 0; i < 10; ++i) CHECK(tac::lock_why_index(names[i]) == i);
    CHECK(tac::lock_why_index("vb-stride") == 10 && tac::lock_why_index(nullptr) == 10 && tac::lock_why_index("not-read") == 10);
    tac::SpriteVert v[64];
    tac::TacFrame f; f.valid = true;
    { tac::MarkerSprite& m = f.mark[f.nmark++]; m.cx = 2100; m.cy = 2000; m.hw = m.hh = 12; for (int k = 0; k < 3; ++k) m.rgb[k] = kRed[k]; }
    tac::LockWhyStats st;
    unsigned n = diamond_outline(v, 2083, 2019, 12, 12, kRed);
    ++st.why[tac::lock_why_index(tac::lock_shape_test(v, n, f).why)];
    ++st.why[tac::lock_why_index(tac::lock_shape_test(v, 3, f).why)];
    ++st.why[tac::lock_why_index(tac::lock_shape_test(v, diamond_outline(v, 2190, 2000, 12, 12, kRed), f).why)];
    CHECK(st.why[0] == 1 && st.why[1] == 1 && st.why[9] == 1);
    st.gate_nobox = 7; st.gate_big = 3; st.why[10] = 2;
    char b[480];
    const int k = tac::format_lock_why(b, sizeof b, "gpu-copy-step1-status;event=continuous", st);
    CHECK(k > 0 && k < 420 && std::string(b).find("-tac-lock;tested=diamond:1,few:1,many:0,offscreen:0,size:0,aspect:0,corner:0,inside:0,tips:0,nobox:1,other:2;"
                                                  "not-tested=refused-draw:0,no-marker:7,too-many-indices:3") != std::string::npos);
    char tiny[8]; CHECK(tac::format_lock_why(tiny, sizeof tiny, "p", st) == 7 && tiny[7] == 0);
}

// ------------------------------------------------------------------------------------------------ headset test 9 (step182, markers-360.md section 22)
// With the options' default (mk360:3, no mk360p) a moving contact the game never boxed gets nothing of ours (no green / grey / blue stroke box).
static void test9_prov_default_off() {
    tac::Options o; std::string rest, why;
    CHECK(tac::extract_options("auto;env:1;mk360:3;vrn:1", rest, o, why) && rest == "auto;env:1;vrn:1");
    const mk360::Params p = mk360::params_from(o);
    CHECK(p.mode == 3 && !p.prov);
    mk360::Tracker tk; std::vector<tac::WorldMarker> out; mk360::FrameCounts fc;
    double t = 0; unsigned drawn = 0;
    for (int k = 0; k < 120; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c);
        const float bandit[3] = {0, 1000, 8000.0f - 10.0f * float(k)};   // behind, 600 u/s, never boxed
        const float wing[3] = {300, 950, -400.0f - 0.0f * float(k)};      // static beside: never boxed either
        add_obj(f, bandit); add_obj(f, wing);
        mk360::update(tk, f, c.C, t, p); mk360::collect(tk, f, c.C, p, out, fc);
        drawn += unsigned(out.size());
    }
    CHECK(drawn == 0 && tk.st.drawn_prov == 0);
}
// A whitish group (the m03k event: 1.00,0.96,0.84 on a 4-contact group at 3,200-5,900 u/s) never makes an identity nor a cached picture, so nothing is drawn
// for those contacts behind the player; a red identity crossed by a whitish group keeps its colour and its cached picture.
static void test9_pale_never_identity() {
    const float kPale[3] = {1.0f, 0.96f, 0.84f}, kPale2[3] = {0.99f, 1.0f, 0.91f};
    CHECK(!mk360::box_colour_ok(kPale) && !mk360::box_colour_ok(kPale2));
    CHECK(mk360::box_colour_ok(kRed) && mk360::box_colour_ok(kGreen) && mk360::box_colour_ok(kBlue) && mk360::box_colour_ok(kYellow));
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float m1[3] = {-1500, 900, -20000}, tgt[3] = {2500, 1100, -20000};
    double t = 0;
    for (int k = 0; k < 20; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, m1); add_obj(f, tgt);
        add_sprite(f, m1, kPale, 0, false);
        add_sprite(f, tgt, k < 10 ? kRed : kPale, 3, false);   // the red target, then a whitish group on it for 10 frames
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.live == 2);   // in the window both stay the game's own live pixels
    }
    CHECK(tk.tracks.size() == 2 && tk.st.ident_pale >= 20);
    for (const mk360::Track& tr : tk.tracks) {
        if (dist3(tr.pos, m1) < 100) CHECK(!tr.ident && !tr.ever_ident && !tr.cell_ok);
        else CHECK(tr.ident && tr.rgb[0] > 0.9f && tr.rgb[1] < 0.1f && tr.cell_ok && tr.c_q == 2);   // still red, its red picture kept
    }
    Cam b = cam_yaw(180); tac::TacFrame fb = frame_for(b); add_obj(fb, m1); add_obj(fb, tgt);
    mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc);
    CHECK(sc.cached == 1 && plan.draw.size() == 1 && dist3(plan.draw[0].pos, tgt) < 1e-3f);   // only the red one behind the player
    CHECK(mk360::hs6_line(tk, 1).find(";pale=") != std::string::npos);
}
// A red identity that flies beyond the game's box range (contact 55 of test 9 at 97-205 km) is not drawn there; back within range its picture returns.
static void test9_far_cached_not_drawn() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    double t = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        const float q[3] = {2000, 1100, -20000};
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, q); add_sprite(f, q, kRed, 4, false);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
    }
    CHECK(tk.tracks.size() == 1 && tk.tracks[0].ident && tk.tracks[0].cell_ok);
    // turned away; the contact recedes 20,000 -> 60,000 u at 2,400 u/s (1,000 frames at 60 Hz: kept tracked, no lost_s gap)
    unsigned drawn_far = 0, drawn_near = 0;
    for (int k = 0; k <= 1000; ++k, t += 1.0 / 60) {
        const float z = -20000.0f - 40.0f * float(k);
        const float q[3] = {2000, 1100, z};
        Cam c = cam_yaw(180); tac::TacFrame f = frame_for(c); add_obj(f, q);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        const float d = dist3(q, c.C);
        if (d > p.box_range + 100.0f) drawn_far += sc.cached; else if (d < p.box_range - 100.0f) drawn_near += sc.cached;
    }
    CHECK(drawn_far == 0 && drawn_near > 0 && tk.st.nb_far > 0 && tk.tracks[0].ident);
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {   // back within range (a jump the tracker follows: same contact, still identified)
        const float q[3] = {2000, 1100, -60000.0f + 40.0f * float(k)};
        Cam c = cam_yaw(180); for (int i = 0; i < 3; ++i) c.C[i] = q[i] + (i == 2 ? 30000.0f : 0.0f); make_A(c.C, c.r, c.u, c.f, c.A);
        tac::TacFrame f = frame_for(c); add_obj(f, q);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
    }
    CHECK(sc.cached == 1);
    mk360::Params p0 = p; p0.box_range = 0.0f;   // 0 = no limit (the old behaviour)
    mk360::Tracker tk0; double t0 = 0;
    for (int k = 0; k < 3; ++k, t0 += 1.0 / 60) {
        const float q[3] = {2000, 1100, -20000};
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, q); add_sprite(f, q, kRed, 4, false);
        mk360::update(tk0, f, c.C, t0, p0); mk360::plan_stamps(tk0, f, p0, 2560, 1440, true, t0, plan, sc);
    }
    {
        const float q[3] = {2000, 1100, -20000.0f}; Cam c = cam_yaw(180); for (int i = 0; i < 3; ++i) c.C[i] = q[i] + (i == 2 ? 80000.0f : 0.0f);
        c.C[0] = 0; c.C[1] = 1000; make_A(c.C, c.r, c.u, c.f, c.A);
        tac::TacFrame f = frame_for(c); add_obj(f, q);
        mk360::update(tk0, f, c.C, t0, p0); mk360::plan_stamps(tk0, f, p0, 2560, 1440, true, t0, plan, sc);
        CHECK(sc.cached == 1 && tk0.st.nb_far == 0);
    }
}
// Two enemies a few GS apart in the window (test 9 t 125-170 s: contacts 5 and 6, 0.7 deg / 2.6 deg apart, labels above-right and the range below): their
// stamps overlap and are cut on every frame. Before: neither picture was cached (q 0), so outside the window only the one once seen alone had a box. Now
// each caches its clean box (q 1: the box without the label text the other's stamp overlaps) and both are drawn behind the player.
static void test9_cut_pair_both_cached() {
    mk360::Tracker tk; mk360::Params p; p.mode = 3;
    mk360::StampPlan plan; mk360::StampCounts sc;
    const float A[3] = {0, 1000, -20000}, B[3] = {286, 1000 - 919, -20000};   // B ~6 GS right and ~30 GS below A at 20,000 u
    double t = 0;
    for (int k = 0; k < 3; ++k, t += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, A); add_obj(f, B);
        float xa, ya, wa, xb, yb, wb; CHECK(tac::project_ps2(f.A, A, xa, ya, wa) && tac::project_ps2(f.A, B, xb, yb, wb));
        CHECK(near(xb - xa, 6.0, 0.5) && near(yb - ya, 30.0, 0.5));
        tac::MarkerSprite& ma = raw_sprite(f, xa, ya, 7.5f, kRed); set_label(ma, xa + 10, ya - 16, xa + 30, ya + 16, 8, kGreen);   // "F-4E" above-right + range below
        tac::MarkerSprite& mb = raw_sprite(f, xb, yb, 7.5f, kGreen); set_label(mb, xb + 10, yb - 16, xb + 30, yb + 16, 8, kGreen);
        mk360::update(tk, f, c.C, t, p); mk360::plan_stamps(tk, f, p, 2560, 1440, true, t, plan, sc);
        CHECK(sc.live == 2 && sc.cuts >= 1);
    }
    CHECK(tk.tracks.size() == 2);
    for (const mk360::Track& tr : tk.tracks) {
        CHECK(tr.ident && tr.cell_ok && tr.c_q == 1);
        // the cached part is the box + its margin only: +-9.5 GS about the contact, nothing of the other contact (30 GS away) or of the label
        CHECK(tr.c_oy >= -10.5f && tr.c_oy + tr.c_ph * 448.0f / 1440.0f <= 10.5f && tr.c_ox >= -10.5f && tr.c_ox + tr.c_pw * 512.0f / 2560.0f <= 10.5f);
    }
    CHECK(tk.st.nb_cut_cached >= 2 && tk.st.nb_label_clipped == 0);
    Cam b = cam_yaw(180); tac::TacFrame fb = frame_for(b); add_obj(fb, A); add_obj(fb, B);
    mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc);
    CHECK(sc.cached == 2 && plan.draw.size() == 2);   // both behind the player
    CHECK(mk360::hs6_line(tk, 2).find(";cut-cached=") != std::string::npos);
    // A's label ends 1 GS before B's box margin: the stamps overlap only in their margins, so the cut runs in the gap and each keeps its whole picture
    mk360::Tracker tk2; double t2 = 0;
    const float B2[3] = {2000, 1000, -20000};   // ~42 GS right of A
    for (int k = 0; k < 3; ++k, t2 += 1.0 / 60) {
        Cam c = cam_yaw(0); tac::TacFrame f = frame_for(c); add_obj(f, A); add_obj(f, B2);
        float xa, ya, wa, xb, yb, wb; CHECK(tac::project_ps2(f.A, A, xa, ya, wa) && tac::project_ps2(f.A, B2, xb, yb, wb));
        tac::MarkerSprite& ma = raw_sprite(f, xa, ya, 7.5f, kRed); set_label(ma, xa + 10, ya - 16, xb - 7.5f - 3.0f, ya - 11, 4, kGreen);   // ends 3 GS short of B's box
        raw_sprite(f, xb, yb, 7.5f, kGreen);
        mk360::update(tk2, f, c.C, t2, p); mk360::plan_stamps(tk2, f, p, 2560, 1440, true, t2, plan, sc);
        CHECK(sc.cuts >= 1);
    }
    for (const mk360::Track& tr : tk2.tracks) CHECK(tr.cell_ok && tr.c_q == 2);   // A: box + whole label; B (no label): its whole box
}
// The HUD tail restarts at each cockpit member (step5TacMember): its markers AND the drop / unread facts about them go (restart_marker_tail). Before, a
// drop before the restart made every frame unsafe (test 9: 26,320 of 33,498; step180 null: 20,840 of 20,840) and the in-window miss rule never cleared
// the identity of a contact the game stopped boxing (a kill in view): its game box followed the wreck outside the window.
static void test9_restart_marker_tail() {
    tac::TacFrame f; f.nmark = 5; f.mark_dropped = 3; f.box_unreadable = 2; f.label_unreadable = 1; f.lock_shapes = 1; f.box_draws = 7;
    uint64_t ub = 10, ul = 20;
    tac::restart_marker_tail(f, ub, ul);
    CHECK(f.nmark == 0 && f.mark_dropped == 0 && f.box_unreadable == 0 && f.label_unreadable == 0 && f.lock_shapes == 0 && f.box_draws == 7);
    CHECK(ub == 12 && ul == 21);
    // a red target killed in view: the game stops boxing it, its wreck keeps drawing; then the head turns to the wreck behind the window
    for (bool restarted : {false, true}) {
        mk360::Tracker tk; mk360::Params p; p.mode = 3;
        mk360::StampPlan plan; mk360::StampCounts sc;
        double t = 0;
        for (int k = 0; k < 40; ++k, t += 1.0 / 60) {
            const float q[3] = {500, 1000 - 5.0f * float(k), -15000};
            Cam c = cam_yaw(0); tac::TacFrame fr = frame_for(c); add_obj(fr, q);
            fr.mark_dropped = 16;   // a drop in the segment before the HUD tail restart (cloud-like draws)
            if (restarted) { uint64_t a = 0, b2 = 0; tac::restart_marker_tail(fr, a, b2); }
            if (k < 5) add_sprite(fr, q, kRed, 3, false);   // boxed, then killed (no box from frame 5)
            mk360::update(tk, fr, c.C, t, p); mk360::plan_stamps(tk, fr, p, 2560, 1440, true, t, plan, sc);
        }
        CHECK(tk.tracks.size() == 1);
        Cam b = cam_yaw(180); tac::TacFrame fb = frame_for(b); const float q[3] = {500, 800, -15000}; add_obj(fb, q);
        mk360::update(tk, fb, b.C, t, p); mk360::plan_stamps(tk, fb, p, 2560, 1440, true, t, plan, sc);
        if (restarted) CHECK(!tk.tracks[0].ident && tk.st.ident_cleared_in_view == 1 && sc.cached == 0);   // the wreck has no box
        else CHECK(tk.tracks[0].ident && tk.st.ident_cleared_in_view == 0 && sc.cached == 1);         // the old defect: the box follows the wreck
    }
}

int main() {
    test9_prov_default_off();
    test9_pale_never_identity();
    test9_far_cached_not_drawn();
    test9_cut_pair_both_cached();
    test9_restart_marker_tail();
    test_census_aim();
    test_hs6_piece_pair();
    test_hs6_recolour_hold();
    test_hs6_depth_hold();
    test_hs6_label_core();
    test_hs6_lock_shape();
    test_hs6_stamp_line();
    test_boxlock_interp_close();
    test_boxlock_inverted();
    test_boxlock_close_persist();
    test_boxlock_near_push_origin();
    test_boxlock_near_scale_two_pieces();
    test_options();
    test_suppress_rule();
    test_geometry();
    test_wingman_all_around(1);
    test_wingman_all_around(2);
    test_kill_in_view();
    test_unread_and_mismatch();
    test_unknown_and_self();
    test_provisional_box();
    test_iff_formation_friend();
    test_iff_learned_team();
    test_iff_unknown_candidate();
    test_iff_presumed_enemy();
    test_iff_box_overrides_formation();
    test_iff_camera_velocity();
    test_iff_options();
    test8_no_double_beside_game_box();
    test8_hold_after_game_box();
    test8_quick_formation_never_green();
    test8_friend_ghost();
    test8_one_box_per_aircraft();
    test8_wreck_gets_no_box();
    test8_cockpit_guard();
    test8_lock_why();
    test_lock_dump();
    test_one_box_per_contact();
    test_edge_margin();
    test_logs();
    test_s1_replay();
    test_group_limits();
    test_box_not_warped();
    test_lock_fx();
    test_nbox_geometry_and_labels();
    test_nbox_wingman();
    test_nbox_near_policy();
    test_nbox_off_view();
    test_nbox_exactly_once_and_edge();
    test_nbox_failures_and_eviction();
    test_nbox_refusal_reasons();
    test_nbox_fresh_equals_recomposite();
    test_nbox_rejected_after_completion();
    test_label_suppress_pairing();
    test_nbox_lock_diamond();
    test_nbox_lock_far_second_stamp();
    test_nbox_cut_never_replaces_whole();
    test_w12_cut_label_not_cached();
    test_w12_two_satellites_one_second_cell();
    test_w12_recolour_drops_cache();
    test_nbox_roll();
    test_hs5_label_whole();
    test_hs5_ident_hold();
    test_hs5_near_fade();
    if (g_fails) { std::printf("marker360_offline: %u of %u checks FAILED\n", g_fails, g_checks); return 1; }
    std::printf("marker360_offline: %u checks passed\n", g_checks);
    return 0;
}
