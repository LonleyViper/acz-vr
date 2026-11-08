// BRIEF3D offline tests (src/acz-vr/brief3d.h; docs/world-render/brief3d.md): options, draw identity + root layout gate (the dumped RTS0 layouts),
// the PS2-form eye projection against an independent OpenXR projection, the table placement, the model fit (up axis, side, handedness) on a synthetic
// 128 x 128 grid seen by a synthetic game camera, the eye guard, the SelMap / ModelIcon records, the root swap + command sequence, the budget, the
// gate and the log lines. Nothing here touches D3D12; fixture success says nothing about the live game or a headset.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "brief3d.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::brief3d;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near_eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

namespace {

const float kCb10[2] = {512.0f, -448.0f};   // the smoke / DrawACM_v live values; the SelMap value is logged live (B3-2)

void test_options() {
    std::string rest, why; Options o;
    const std::string day = "auto;env:1;props:1;trees:1;vscreen:1;vsearly:1";
    CHECK(extract_options(day, rest, o, why) && rest == day && !o.on && !o.any_token);
    CHECK(extract_options("auto;brief3d:1;env:1;b3w=1.5;b3d:0.9;b3y:-0.6;b3v:2", rest, o, why));
    CHECK(rest == "auto;env:1" && o.on && o.any_token);
    CHECK(near_eq(o.width_m, 1.5, 1e-6) && near_eq(o.dist_m, 0.9, 1e-6) && near_eq(o.y_m, -0.6, 1e-6) && near_eq(o.vexag, 2.0, 1e-6));
    CHECK(extract_options("brief3d:1", rest, o, why) && rest == "brief3d:1" && !o.on);   // the first item is the stage id, never an option
    CHECK(!extract_options("auto;brief3d:2", rest, o, why) && why.find("brief3d") != std::string::npos);
    CHECK(!extract_options("auto;b3w:9", rest, o, why));
    CHECK(!extract_options("auto;b3v:0.1", rest, o, why));
    CHECK(!extract_options("auto;b3y:x", rest, o, why));
    CHECK(extract_options("auto;brief3d:0", rest, o, why) && rest == "auto" && !o.on && o.any_token);
    Options on; on.on = true;
    CHECK(streq(prerequisites(Options{}, true, true, true), "off"));
    CHECK(streq(prerequisites(on, false, true, true), "needs-vscreen:1"));
    CHECK(streq(prerequisites(on, true, false, true), "needs-vsearly:1"));
    CHECK(streq(prerequisites(on, true, true, false), "needs-xr_hud_layer"));
    CHECK(prerequisites(on, true, true, true) == nullptr);
    CHECK(describe(on) == "brief3d=1;b3w=1.00;b3d=0.75;b3y=-0.45;b3v=1.00;b3l=1;b3p=1");
    CHECK(extract_options("auto;brief3d:1;b3l:0", rest, o, why) && rest == "auto" && o.on && !o.latch);   // brief3d.md 11: the old replay path
    CHECK(extract_options("auto;brief3d:1", rest, o, why) && o.latch);                                     // the latched per-XR-frame render is the default
    CHECK(!extract_options("auto;b3l:2", rest, o, why) && why.find("b3l") != std::string::npos);
    // headset test 9: the world-fixed table plane is the default, b3p:0 restores the per-eye quads
    CHECK(extract_options("auto;brief3d:1", rest, o, why) && o.fixed_plane);
    CHECK(extract_options("auto;brief3d:1;b3p:0", rest, o, why) && rest == "auto" && !o.fixed_plane && o.latch);
    CHECK(!extract_options("auto;b3p:2", rest, o, why) && why.find("b3p") != std::string::npos);
}

void test_identity_and_root() {
    CHECK(classify(kSelMapVs8, kMapPs8, kTopoLine, true) == Kind::selmap);
    CHECK(classify(kModelIconVs8, kMapPs8, kTopoTriangle, true) == Kind::modelicon);
    CHECK(classify(kSelMapVs8, kMapPs8, kTopoLine, false) == Kind::none);
    CHECK(classify(kSelMapVs8, kMapPs8, kTopoTriangle, true) == Kind::none);
    CHECK(classify(kSelMapVs8, 0xd34ee153u, kTopoLine, true) == Kind::none);
    // D-384: the icon VS with another pixel shader (a per-class variant) is still an icon draw; the grid and every other VS stay strict
    CHECK(classify(kModelIconVs8, 0xd34ee153u, kTopoTriangle, true) == Kind::modelicon);
    CHECK(classify(kModelIconVs8, 0u, kTopoTriangle, true) == Kind::none);
    CHECK(classify(kModelIconVs8, 0xd34ee153u, kTopoLine, true) == Kind::none);
    CHECK(classify(kModelIconVs8, 0xd34ee153u, kTopoTriangle, false) == Kind::none);
    CHECK(classify(0x534f29cau, kMapPs8, kTopoTriangle, true) == Kind::none);
    CHECK(srt_bytes(Kind::selmap) == 64 && srt_bytes(Kind::modelicon) == 272 && srt_bytes(Kind::none) == 0);
    // the RTS0 layouts of the R4 dumps (384ade02: p0 SRV t10, p1 SRV t11, p2 constants b10 x2; d5a41c2f: p0 SRV t10, p1 constants b10 x2; vis vertex)
    const uint32_t flags = 0x33d;
    RootParam sel[3] = {{kParamSrv, kVisVertex, 10, 0, 0}, {kParamSrv, kVisVertex, 11, 0, 0}, {kParamConstants, kVisVertex, 10, 0, 2}};
    RootParam ico[2] = {{kParamSrv, kVisVertex, 10, 0, 0}, {kParamConstants, kVisVertex, 10, 0, 2}};
    RootPlan rp;
    CHECK(root_layout_check(Kind::selmap, sel, 3, flags, rp) == nullptr && rp.t10 == 0 && rp.t11 == 1 && rp.b10 == 2);
    CHECK(root_layout_check(Kind::modelicon, ico, 2, flags, rp) == nullptr && rp.t10 == 0 && rp.b10 == 1 && rp.t11 == UINT32_MAX);
    CHECK(streq(root_layout_check(Kind::selmap, ico, 2, flags, rp), "brief3d-root-t11-absent"));
    CHECK(streq(root_layout_check(Kind::selmap, sel, 3, flags | kDenyVs, rp), "brief3d-root-vs-denied"));
    RootParam tab[3] = {sel[0], sel[1], {kParamTable, kVisAll, 0, 0, 0}};
    CHECK(streq(root_layout_check(Kind::selmap, tab, 3, flags, rp), "brief3d-root-table"));
    RootParam cbv[3] = {sel[0], sel[1], {2, kVisVertex, 10, 0, 0}};
    CHECK(streq(root_layout_check(Kind::selmap, cbv, 3, flags, rp), "brief3d-root-other-binding"));
    RootParam twice[3] = {sel[0], sel[0], sel[2]};
    CHECK(streq(root_layout_check(Kind::modelicon, twice, 3, flags, rp), "brief3d-root-t10-ambiguous"));
    RootParam shortc[2] = {ico[0], {kParamConstants, kVisVertex, 10, 0, 1}};
    CHECK(streq(root_layout_check(Kind::modelicon, shortc, 2, flags, rp), "brief3d-root-b10-short"));
    RootParam pixel[2] = {{kParamSrv, 5, 10, 0, 0}, ico[1]};   // a pixel-only t10 is not the vertex shader's
    CHECK(streq(root_layout_check(Kind::modelicon, pixel, 2, flags, rp), "brief3d-root-t10-absent"));
    CHECK(streq(root_layout_check(Kind::none, sel, 3, flags, rp), "brief3d-kind"));
}

// independent reference: LOCAL point -> NDC + view depth for an OpenXR eye
bool reference_ndc(const EyeView& e, const double p[3], double ndc[2], double& w) {
    const double d[3] = {p[0] - e.position[0], p[1] - e.position[1], p[2] - e.position[2]};
    const double qi[4] = {-e.orientation[0], -e.orientation[1], -e.orientation[2], e.orientation[3]};
    double v[3]; quat_rotate(qi, d, v);
    w = -v[2];
    if (!(w > 0)) return false;
    const double tl = std::tan(e.fov[0]), tr = std::tan(e.fov[1]), tu = std::tan(e.fov[2]), td = std::tan(e.fov[3]);
    ndc[0] = (2 * v[0] / w - (tr + tl)) / (tr - tl);
    ndc[1] = (2 * v[1] / w - (tu + td)) / (tu - td);
    return true;
}
EyeView make_eye(double px, double py, double pz, double yaw, double pitch, double ipd_x) {
    // q = yaw about y then pitch about x
    const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    EyeView e;
    e.orientation[0] = cy * sp; e.orientation[1] = sy * cp; e.orientation[2] = -sy * sp; e.orientation[3] = cy * cp;
    const double off[3] = {ipd_x, 0, 0}; double r[3]; quat_rotate(e.orientation, off, r);
    e.position[0] = px + r[0]; e.position[1] = py + r[1]; e.position[2] = pz + r[2];
    e.fov[0] = -0.82; e.fov[1] = 0.74; e.fov[2] = 0.79; e.fov[3] = -0.87;   // asymmetric, like a real headset eye
    return e;
}

void test_projection() {
    for (int mode = 0; mode < 2; ++mode) {
        const DepthMode dm = mode ? DepthMode::standard : DepthMode::reverse;
        const EyeView e = make_eye(0.1, 1.2, -0.3, 0.6, -0.5, 0.032);
        Mat4 P; CHECK(ps2_projection(e.fov, kCb10, 0.05, dm, P) == nullptr);
        const Mat4 M = mul(view_from_eye(e), P);
        for (int i = 0; i < 40; ++i) {
            const double p[3] = {-0.8 + 0.04 * i, 0.4 + 0.01 * (i % 7), -1.2 - 0.02 * (i % 11)};
            double ndc[2], w;
            if (!reference_ndc(e, p, ndc, w)) continue;
            double sv[3];
            CHECK(emulate_selmap(M, kCb10, p, sv));
            CHECK(near_eq(sv[0], ndc[0], 1e-9) && near_eq(sv[1], ndc[1], 1e-9));
            CHECK(near_eq(sv[2], dm == DepthMode::reverse ? 0.05 / w : 1.0 - 0.05 / w, 1e-12));
        }
        // a point behind the eye: the shader would mirror it, the emulation reports it
        const double behind[3] = {e.position[0] + 0.0, e.position[1], e.position[2] + 0.0};
        double fwd[3]; const double f0[3] = {0, 0, 1}; quat_rotate(e.orientation, f0, fwd);
        const double b2[3] = {behind[0] + fwd[0], behind[1] + fwd[1], behind[2] + fwd[2]};
        double sv[3]; CHECK(!emulate_selmap(M, kCb10, b2, sv));
    }
    Mat4 P; double badfov[4] = {0.5, 0.4, 0.5, -0.5};
    CHECK(streq(ps2_projection(badfov, kCb10, 0.05, DepthMode::reverse, P), "brief3d-fov"));
    const float zero[2] = {0.0f, -448.0f}; const double fov[4] = {-0.8, 0.8, 0.8, -0.8};
    CHECK(streq(ps2_projection(fov, zero, 0.05, DepthMode::reverse, P), "brief3d-cb10"));
    CHECK(streq(ps2_projection(fov, kCb10, 0.0, DepthMode::reverse, P), "brief3d-near"));
    DepthMode m; float clear;
    CHECK(depth_mode(false, 0, m, clear) == nullptr && m == DepthMode::reverse);
    CHECK(depth_mode(true, kCmpGreaterEqual, m, clear) == nullptr && m == DepthMode::reverse && clear == 0.0f);
    CHECK(depth_mode(true, kCmpLessEqual, m, clear) == nullptr && m == DepthMode::standard && clear == 1.0f);
    CHECK(streq(depth_mode(true, kCmpEqual, m, clear), "brief3d-pso-depth-func"));
}

void test_placement() {
    Options o; o.dist_m = 1.0f; o.y_m = -0.5f;
    const EyeView head = make_eye(0.3, 1.4, 0.2, 0.9, 0.4, 0.0);   // pitch is ignored
    const Placement p = place(o, head.position, head.orientation, 7);
    CHECK(p.valid && p.space_generation == 7 && near_eq(p.yaw, 0.9, 1e-9));
    CHECK(near_eq(p.centre[0], 0.3 - std::sin(0.9), 1e-9) && near_eq(p.centre[1], 0.9, 1e-9) && near_eq(p.centre[2], 0.2 - std::cos(0.9), 1e-9));
    const Mat4 t = table_to_local(p);
    const double far_edge[3] = {0, 0, -0.5}; double h[4]; xform(t, far_edge, h);   // -z of the table = away from the user
    CHECK(near_eq(h[0], p.centre[0] - 0.5 * std::sin(0.9), 1e-9) && near_eq(h[2], p.centre[2] - 0.5 * std::cos(0.9), 1e-9));
}

// ---- synthetic briefing: a 128 x 128 grid in a PS2-like model space, a game camera looking down at it
struct Synth {
    std::vector<uint8_t> vb;
    Mat4 game;
};
// up: model axis of the height; up_sign: +1 heights grow along +up; the camera sits on the +up_sign side; mirror: the game image is mirrored (x flipped)
Synth make_synth(int up, int up_sign, bool mirror, double cam_yaw) {
    Synth s;
    const int n = 128; const double span = 262144.0, step = 2 * span / (n - 1);
    const int a1 = up == 0 ? 1 : 0, a2 = up == 2 ? 1 : 2;
    s.vb.resize(size_t(n) * n * 16);
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) {
        float v[4] = {0, 0, 0, 0};
        v[a1] = float(-span + step * i); v[a2] = float(-span + step * j);
        v[up] = float(up_sign * (3000.0 + 2000.0 * std::sin(i * 0.1) * std::cos(j * 0.07)));
        v[3] = float((i + j) % 256);
        std::memcpy(&s.vb[(size_t(i) * n + j) * 16], v, 16);
    }
    // game camera: position above the map centre on the up side, looking down at 50 deg, yawed; model -> camera view (x right, y up, -z fwd)
    // built in a "canonical" frame (X = a1, Y = up*up_sign, Z = a2 or -a2 to keep it right-handed) then projected with the PS2 form.
    Mat4 toCanon; for (double& v : toCanon.m) v = 0;
    toCanon.m[a1 * 4 + 0] = 1; toCanon.m[up * 4 + 1] = up_sign;
    // right-handedness of (a1, up, a2) with signs: det of the permutation x up_sign decides the sign of a2
    const int perm_sign = (a1 == 0 && up == 1 && a2 == 2) ? 1 : (a1 == 1 && up == 0 && a2 == 2) ? -1 : (a1 == 0 && up == 2 && a2 == 1) ? -1 : 1;
    toCanon.m[a2 * 4 + 2] = perm_sign * up_sign; toCanon.m[15] = 1;
    EyeView cam = make_eye(std::sin(cam_yaw) * 600000.0, 700000.0, std::cos(cam_yaw) * 600000.0, cam_yaw, -0.87, 0.0);
    cam.fov[0] = -0.5; cam.fov[1] = 0.5; cam.fov[2] = 0.3; cam.fov[3] = -0.3;
    Mat4 P; ps2_projection(cam.fov, kCb10, 1000.0 / 1e6 * 10, DepthMode::reverse, P);   // near irrelevant here
    Mat4 mir;
    if (mirror) mir.m[0] = -1;
    s.game = mul(mul(mul(toCanon, mir), view_from_eye(cam)), P);
    return s;
}

// orientation (cross sign, D3D NDC) of three model points as an eye sees them through m
double ndc_cross(const Mat4& m, const double p0[3], const double p1[3], const double p2[3]) {
    double s[3][3];
    const double* pts[3] = {p0, p1, p2};
    for (int i = 0; i < 3; ++i) if (!emulate_selmap(m, kCb10, pts[i], s[i])) return 0;
    return (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[1][1] - s[0][1]) * (s[2][0] - s[0][0]);
}

void test_fit() {
    const int ups[3] = {0, 1, 2};
    for (int ui = 0; ui < 3; ++ui) for (int sgn = -1; sgn <= 1; sgn += 2) for (int mir = 0; mir < 2; ++mir) for (int yi = 0; yi < 3; ++yi) {
        const int up = ups[ui];
        const Synth s = make_synth(up, sgn, mir != 0, 0.4 + 2.0 * yi);
        Box box; CHECK(box_from_vertices(s.vb.data(), s.vb.size(), 16, 0, 128 * 128, box) == nullptr && box.vertices == 16384);
        Options o; o.width_m = 1.2f;
        Fit f; const char* why = fit_model(box, s.game, kCb10, o, f);
        CHECK(why == nullptr);
        CHECK(f.up == up && f.up_sign == sgn);
        // width: the longer horizontal extent = 1.2 m
        const Volume v = volume(box, f);
        CHECK(near_eq(2 * std::fmax(v.half_x, v.half_z), 1.2, 1e-6));
        CHECK(v.height > 0.0 && v.height < 0.05);
        // the lowest grid point sits on the table top, the map centre on the table centre
        double lowest = 1e9; double hmid[4];
        const double mid[3] = {0.5 * (box.lo[0] + box.hi[0]), 0.5 * (box.lo[1] + box.hi[1]), 0.5 * (box.lo[2] + box.hi[2])};
        xform(f.model_to_table, mid, hmid);
        CHECK(near_eq(hmid[0], 0, 1e-9) && near_eq(hmid[2], 0, 1e-9));
        for (size_t i = 0; i < 16384; ++i) {
            float q[4]; std::memcpy(q, &s.vb[i * 16], 16); const double p[3] = {q[0], q[1], q[2]};
            double h[4]; xform(f.model_to_table, p, h); lowest = std::fmin(lowest, h[1]);
            CHECK(h[1] >= -1e-9);
        }
        CHECK(near_eq(lowest, 0.0, 1e-9));
        // never mirrored: an eye above the table, looking down, sees the map with the game's orientation
        Placement pl; pl.valid = true; pl.yaw = 0.3; pl.centre[1] = 0.8;
        EyeView eye = make_eye(0.0, 1.8, 0.05, 0.3, -1.45, 0.0);
        Mat4 em; CHECK(eye_matrix(f, pl, eye, kCb10, EyeCfg{}, em) == nullptr);
        double p1[3] = {mid[0], mid[1], mid[2]}, p2[3] = {mid[0], mid[1], mid[2]};
        p1[f.a1] += 0.1 * (box.hi[f.a1] - box.lo[f.a1]); p2[f.a2] += 0.1 * (box.hi[f.a2] - box.lo[f.a2]);
        const double game_cross = ndc_cross(s.game, mid, p1, p2), eye_cross = ndc_cross(em, mid, p1, p2);
        CHECK(game_cross != 0 && eye_cross != 0 && ((game_cross > 0) == (eye_cross > 0)));
    }
    // refusals
    const Synth s = make_synth(1, 1, false, 0.4);
    Box box; box_from_vertices(s.vb.data(), s.vb.size(), 16, 0, 128 * 128, box);
    Options o; Fit f;
    Box tall = box; tall.hi[1] = tall.lo[1] + 0.9 * (box.hi[0] - box.lo[0]);
    CHECK(streq(fit_model(tall, s.game, kCb10, o, f), "brief3d-box-not-flat") && !f.valid);
    Box inv; CHECK(streq(fit_model(inv, s.game, kCb10, o, f), "brief3d-box-invalid"));
    Mat4 sing; for (double& v : sing.m) v = 0;
    CHECK(streq(fit_model(box, sing, kCb10, o, f), "brief3d-camera-singular"));
    const Synth below = make_synth(1, 1, false, 0.4);
    Box hi = box; hi.hi[1] = 2e6; hi.lo[1] = -2e6;   // the camera is inside the height range
    hi.hi[0] *= 20; hi.lo[0] *= 20; hi.hi[2] *= 20; hi.lo[2] *= 20;
    CHECK(streq(fit_model(hi, below.game, kCb10, o, f), "brief3d-camera-not-above-map"));
    // vertex buffer refusals
    Box b2;
    CHECK(streq(box_from_vertices(s.vb.data(), s.vb.size(), 16, 0, 0, b2), "brief3d-box-empty"));
    CHECK(streq(box_from_vertices(s.vb.data(), s.vb.size(), 8, 0, 4, b2), "brief3d-box-stride"));
    CHECK(streq(box_from_vertices(s.vb.data(), s.vb.size(), 16, 1, 128 * 128, b2), "brief3d-box-span"));
    CHECK(box_from_vertices(s.vb.data(), s.vb.size() - 4, 16, 0, 128 * 128, b2) == nullptr);   // the last w is not read
    std::vector<uint8_t> bad = s.vb; const float nanv = std::nanf(""); std::memcpy(&bad[16 * 77 + 4], &nanv, 4);
    CHECK(streq(box_from_vertices(bad.data(), bad.size(), 16, 0, 128 * 128, b2), "brief3d-box-nonfinite") && !b2.valid);
    // camera centre of a synthetic camera = its position mapped into model space
    double c[3]; CHECK(camera_centre(s.game, c) == nullptr && c[1] > box.hi[1]);
}

void test_content_fit() {
    // brief3d.md 9.1 (zoom): icon heights from matrix 3's translation, the uniform scale, the scaled fit and the content box, both up signs
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        const Synth s = make_synth(1, sgn, false, 0.7);
        Box box; box_from_vertices(s.vb.data(), s.vb.size(), 16, 0, 128 * 128, box);
        Options o; o.width_m = 1.0f; o.y_m = -0.45f;
        Fit f; CHECK(fit_model(box, s.game, kCb10, o, f) == nullptr);
        uint8_t icon[kModelIconSrtBytes] = {};
        const double above_units = 0.75 / f.metres_per_unit;   // the zoomed flight: 0.75 m above the lowest grid point at scale 1
        const float y = float(f.origin[1] + sgn * above_units); std::memcpy(icon + 64 * kIconModelMatrix + 4 * 13, &y, 4);
        double h = 0; CHECK(icon_height(icon, f, h) && near_eq(h, above_units, 1e-3 * above_units));
        CHECK(near_eq(content_max_height(o), 0.35, 1e-6));   // b3y is a float
        Options seat = o; seat.y_m = -0.15f; CHECK(near_eq(content_max_height(seat), 0.10, 1e-6));
        const double k = content_scale(f, h, content_max_height(o));
        CHECK(near_eq(k, 0.35 / 0.75, 1e-3));
        CHECK(content_scale(f, 0.02 / f.metres_per_unit, 0.35) == 1.0);   // the overview (live 0.05 m) is never scaled
        CHECK(content_scale(f, 1e6 / f.metres_per_unit, 0.35) == 0.0);    // W8-1 P1: below the minimum scale = refuse, never a cap-violating 0.1
        CHECK(content_scale(f, 3.4 / f.metres_per_unit, 0.35) > 0.1 && content_scale(f, 3.6 / f.metres_per_unit, 0.35) == 0.0);   // 0.35/3.5 = 0.1
        // the review's trigger: 6.6 m content, 0.35 m cap -> 0.053 < 0.1: refused (old code returned 0.1 = 0.66 m content)
        CHECK(content_scale(f, 6.6 / f.metres_per_unit, 0.35) == 0.0);
        // the scaled fit = k x the fit about the table origin: the icon lands 0.35 m above the table top, the map centre stays on the table centre,
        // the width shrinks by k, the lowest grid point stays on the table top
        const Fit fe = scaled_fit(f, k);
        CHECK(fe.valid && near_eq(fe.metres_per_unit, f.metres_per_unit * k, 1e-15));
        const double ip[3] = {0.5 * (box.lo[0] + box.hi[0]), double(y), 0.5 * (box.lo[2] + box.hi[2])};
        double t[4]; xform(fe.model_to_table, ip, t);
        CHECK(near_eq(t[1], 0.35, 1e-3) && near_eq(t[0], 0, 1e-9) && near_eq(t[2], 0, 1e-9));
        double lowest = 1e9;
        for (size_t i = 0; i < 16384; i += 7) {
            float q[4]; std::memcpy(q, &s.vb[i * 16], 16); const double p[3] = {q[0], q[1], q[2]};
            double a[4], b[4]; xform(f.model_to_table, p, a); xform(fe.model_to_table, p, b);
            for (int c = 0; c < 3; ++c) CHECK(near_eq(b[c], k * a[c], 1e-9));
            lowest = std::fmin(lowest, b[1]);
        }
        CHECK(lowest >= -1e-9);
        CHECK(scaled_fit(f, 1.0).model_to_table.m[0] == f.model_to_table.m[0]);
        // the content box reaches the icon; the guard volume then covers it (height 0.35 m at the scaled fit), widths scaled by k
        const Box cb = content_box(box, f, h);
        const Volume v0 = volume(box, f), v1 = volume(cb, fe);
        CHECK(near_eq(v1.height, 0.35, 1e-3) && near_eq(v1.half_x, k * v0.half_x, 1e-9) && near_eq(v1.half_z, k * v0.half_z, 1e-9));
        CHECK(content_box(box, f, 0.0).hi[1] == box.hi[1] && content_box(box, f, 0.0).lo[1] == box.lo[1]);
        // brief3d 9.2 vertical-only fit: heights x k, horizontal unchanged (the table keeps its width), lowest point on the table top; guard height 0.35
        const Fit fv = squashed_fit(f, k);
        CHECK(fv.valid && fv.metres_per_unit == f.metres_per_unit && near_eq(fv.vexag, f.vexag * k, 1e-12) && squashed_fit(f, 1.0).vexag == f.vexag);
        for (size_t i = 0; i < 16384; i += 11) {
            float q[4]; std::memcpy(q, &s.vb[i * 16], 16); const double p[3] = {q[0], q[1], q[2]};
            double a[4], b[4]; xform(f.model_to_table, p, a); xform(fv.model_to_table, p, b);
            CHECK(near_eq(b[0], a[0], 1e-9) && near_eq(b[2], a[2], 1e-9) && near_eq(b[1], k * a[1], 1e-9) && b[1] >= -1e-9);
        }
        { double t2[4]; xform(fv.model_to_table, ip, t2); CHECK(near_eq(t2[1], 0.35, 1e-3)); }
        const Volume vv = volume(cb, fv); CHECK(near_eq(vv.height, 0.35, 1e-3) && near_eq(vv.half_x, v0.half_x, 1e-12));
        // icon_copies: an AIR icon (the zoomed aircraft): mesh not flattened (M3' then the squashed table = M3 then the unsquashed table, x g),
        // grown to >= 4 cm, kStalkDots dots between the table top and the aircraft, nothing above the cap
        {
            uint8_t ic[kModelIconSrtBytes] = {};
            const double sc_u = 0.01 / f.metres_per_unit;   // a 1 cm mesh unit (live zoom: ~0.9 cm at the old uniform scale)
            float m3[16] = {float(sc_u), 0, 0, 0, 0, float(sc_u), 0, 0, 0, 0, float(sc_u), 0, float(ip[0] + 3.0), y, float(ip[2] - 2.0), 1};
            std::memcpy(ic + 64 * kIconModelMatrix, m3, 64);
            IconCopies cp; icon_copies(ic, f, k, box, 0.35, 64, cp);
            CHECK(cp.air && cp.copies == 1 + kStalkDots);
            Mat4 a = Mat4{}, b = Mat4{}; for (int i = 0; i < 16; ++i) { a.m[i] = cp.m3[0][i]; b.m[i] = m3[i]; }
            const double g = 4.0;   // 0.04 / 0.01 (kAirMaxGrow 4)
            const double v[3] = {0.3, -0.7, 0.5}, o0[3] = {0, 0, 0};
            double pa[4], pb[4], qa[4], qb[4], oa[4], ob[4];
            xform(a, v, pa); xform(b, v, pb); xform(a, o0, oa); xform(b, o0, ob);
            const double pa3[3] = {pa[0], pa[1], pa[2]}, pb3[3] = {pb[0], pb[1], pb[2]}, oa3[3] = {oa[0], oa[1], oa[2]}, ob3[3] = {ob[0], ob[1], ob[2]};
            double ta[4], tb[4], toa[4], tob[4];
            xform(fv.model_to_table, pa3, ta); xform(f.model_to_table, pb3, tb); xform(fv.model_to_table, oa3, toa); xform(f.model_to_table, ob3, tob);
            for (int c = 0; c < 3; ++c) CHECK(near_eq(ta[c] - toa[c], g * (tb[c] - tob[c]), 1e-6));   // same shape, x g, not flattened
            (void)qa; (void)qb;
            CHECK(near_eq(toa[1], 0.35, 1e-3) && toa[1] <= 0.35 + 1e-6);   // the aircraft at the cap, never above
            double prev = -1;
            for (unsigned c = 1; c < cp.copies; ++c) {
                Mat4 d = Mat4{}; for (int i = 0; i < 16; ++i) d.m[i] = cp.m3[c][i];
                double od[4]; xform(d, o0, od); const double od3[3] = {od[0], od[1], od[2]}; double td[4]; xform(fv.model_to_table, od3, td);
                CHECK(td[1] > prev && td[1] > 0 && td[1] < toa[1] && near_eq(td[0], toa[0], 1e-6) && near_eq(td[2], toa[2], 1e-6));
                CHECK(near_eq(std::fabs(double(cp.m3[c][0])), kStalkDotScale * std::fabs(double(cp.m3[0][0])), 1e-6 * std::fabs(double(cp.m3[0][0]))));
                prev = td[1];
            }
            // an aircraft above the cap (between latches) is clamped to the cap; one outside the grid is pulled onto the table
            float m3b[16]; std::memcpy(m3b, m3, 64); m3b[13] = float(f.origin[1] + sgn * 0.6 / (f.metres_per_unit * k)); m3b[12] = float(box.hi[0] + 50.0);
            std::memcpy(ic + 64 * kIconModelMatrix, m3b, 64); icon_copies(ic, f, k, box, 0.35, 64, cp);
            { Mat4 c3 = Mat4{}; for (int i = 0; i < 16; ++i) c3.m[i] = cp.m3[0][i]; double oc[4]; xform(c3, o0, oc); const double oc3[3] = {oc[0], oc[1], oc[2]};
              double tc[4]; xform(fv.model_to_table, oc3, tc); CHECK(near_eq(tc[1], 0.35, 1e-4) && double(cp.m3[0][12]) <= box.hi[0] + 1e-3); }
            // no room in the slot: the aircraft alone (still grown and clamped)
            icon_copies(ic, f, k, box, 0.35, kStalkDots, cp); CHECK(cp.air && cp.copies == 1);
            // a ground icon at kv 1: byte-identical, one copy; at kv < 1 only the up column is compensated
            float m3g[16]; std::memcpy(m3g, m3, 64); m3g[13] = float(f.origin[1]);
            std::memcpy(ic + 64 * kIconModelMatrix, m3g, 64); icon_copies(ic, f, 1.0, box, 0.35, 64, cp);
            CHECK(!cp.air && cp.copies == 1 && std::memcmp(cp.m3[0], m3g, 64) == 0);
            icon_copies(ic, f, 0.5, box, 0.35, 64, cp);
            CHECK(!cp.air && cp.copies == 1 && near_eq(cp.m3[0][5], m3g[5] / 0.5, 1e-3 * std::fabs(m3g[5])) && cp.m3[0][0] == m3g[0] && cp.m3[0][13] == m3g[13]);
            // non-finite: unchanged (the replay then shows what the game drew)
            m3g[3] = NAN; std::memcpy(ic + 64 * kIconModelMatrix, m3g, 64); icon_copies(ic, f, 0.5, box, 0.35, 64, cp);
            CHECK(cp.copies == 1 && !cp.air && std::memcmp(cp.m3[0], m3g, 64) == 0);
        }
        // the plan for copies: span and cap checks
        { RootSwap r; const uint64_t up = 0x10000000ull;
          CHECK(plan_root_swap(Kind::modelicon, 0, 1, up, kSlotUploadBytes, kIconCap - kMaxIconCopies, r, kMaxIconCopies) == nullptr && r.copies == kMaxIconCopies);
          CHECK(streq(plan_root_swap(Kind::modelicon, 0, 1, up, kSlotUploadBytes, kIconCap - kMaxIconCopies + 1, r, kMaxIconCopies), "brief3d-replay-cap"));
          CHECK(streq(plan_root_swap(Kind::modelicon, 0, 1, up, kSlotUploadBytes, 0, r, kMaxIconCopies + 1), "brief3d-copies"));
          CHECK(streq(plan_root_swap(Kind::selmap, 0, 1, up, kSlotUploadBytes, 0, r, 2), "brief3d-copies"));
          CHECK(streq(plan_root_swap(Kind::modelicon, 0, 1, up, kSlotUploadBytes, 0, r, 0), "brief3d-copies")); }
        // unusable positions: non-finite, absurd (> 8 m), no fit
        const float bad = NAN; std::memcpy(icon + 64 * kIconModelMatrix + 4 * 13, &bad, 4); CHECK(!icon_height(icon, f, h));
        const float far = float(f.origin[1] + sgn * 9.0 / f.metres_per_unit); std::memcpy(icon + 64 * kIconModelMatrix + 4 * 13, &far, 4);
        CHECK(!icon_height(icon, f, h));
        CHECK(!icon_height(icon, Fit{}, h) && !icon_height(nullptr, f, h));
        char line[kLineLimit];
        const int n = format_content(line, sizeof line, 1e300, 1e300, f, 1e300, 1e300, ~0ull);
        CHECK(n > 0 && size_t(n) < kLineLimit - kLinePrefix);
    }
}

void test_end_to_end_and_guard() {
    const Synth s = make_synth(1, -1, false, 1.1);   // PS2 style: height along -y
    Box box; box_from_vertices(s.vb.data(), s.vb.size(), 16, 0, 128 * 128, box);
    Options o; o.width_m = 1.0f; o.dist_m = 0.75f; o.y_m = -0.45f;
    Fit f; CHECK(fit_model(box, s.game, kCb10, o, f) == nullptr);
    const EyeView head = make_eye(0.0, 1.2, 0.0, 0.2, -0.6, 0.0);
    const Placement pl = place(o, head.position, head.orientation, 1);
    const EyeView eyes[2] = {make_eye(0.0, 1.2, 0.0, 0.2, -0.6, -0.032), make_eye(0.0, 1.2, 0.0, 0.2, -0.6, 0.032)};
    FrameState fs; fs.begin();
    CHECK(open_eyes(fs, f, box, pl, eyes, kCb10, EyeCfg{}) == nullptr && fs.eyes_ok);
    // every grid vertex: the eye copy projects it exactly where the eye sees its table position
    const Mat4 m2l = mul(f.model_to_table, table_to_local(pl));
    unsigned visible = 0;
    for (size_t i = 0; i < 16384; i += 37) {
        float q[4]; std::memcpy(q, &s.vb[i * 16], 16); const double p[3] = {q[0], q[1], q[2]};
        double L[4]; xform(m2l, p, L);
        for (unsigned e = 0; e < 2; ++e) {
            double ndc[2], w; CHECK(reference_ndc(eyes[e], L, ndc, w));
            uint8_t rec[kSelMapSrtBytes]; CHECK(selmap_record(fs.eye[e], rec) == nullptr);
            const Mat4 back = load(rec);   // what the GPU reads: float32
            double sv[3]; CHECK(emulate_selmap(back, kCb10, p, sv));
            CHECK(near_eq(sv[0], ndc[0], 2e-4) && near_eq(sv[1], ndc[1], 2e-4) && near_eq(sv[2], 0.05 / w, 1e-5));
            if (std::fabs(sv[0]) <= 1 && std::fabs(sv[1]) <= 1) ++visible;
        }
    }
    CHECK(visible > 0);
    // table: centre at the placement, 1 m across, top at eye height - 0.45
    const double mid[3] = {0.5 * (box.lo[0] + box.hi[0]), box.hi[1], 0.5 * (box.lo[2] + box.hi[2])};   // up -1: the LOWEST point is hi[1]
    double L[4]; xform(m2l, mid, L);
    CHECK(near_eq(L[0], pl.centre[0], 1e-6) && near_eq(L[1], 1.2 - 0.45, 1e-6) && near_eq(L[2], pl.centre[2], 1e-6));
    // the eye guard: a head inside the table volume refuses both eyes
    const EyeView inside[2] = {make_eye(pl.centre[0], pl.centre[1] + 0.005, pl.centre[2], 0.2, -0.6, -0.032),
                               make_eye(pl.centre[0], pl.centre[1] + 0.005, pl.centre[2], 0.2, -0.6, 0.032)};
    FrameState f2; f2.begin();
    CHECK(streq(open_eyes(f2, f, box, pl, inside, kCb10, EyeCfg{}), "brief3d-eye-inside-volume") && !f2.eyes_ok);
    // leaning over the table edge with the eyes 3 cm from the grid: refused too (near 5 cm)
    const EyeView close[2] = {make_eye(pl.centre[0], pl.centre[1] + 0.03, pl.centre[2] + 0.45, 0.2, -1.5, -0.032),
                              make_eye(pl.centre[0], pl.centre[1] + 0.03, pl.centre[2] + 0.45, 0.2, -1.5, 0.032)};
    CHECK(streq(open_eyes(f2, f, box, pl, close, kCb10, EyeCfg{}), "brief3d-eye-inside-volume"));
    Placement bad; CHECK(streq(open_eyes(f2, f, box, bad, eyes, kCb10, EyeCfg{}), "brief3d-placement-invalid"));
    Fit nofit; CHECK(streq(open_eyes(f2, nofit, box, pl, eyes, kCb10, EyeCfg{}), "brief3d-fit-invalid"));
}

void test_icon_records() {
    const Synth s = make_synth(1, 1, false, 0.4);
    uint8_t sel[64]; CHECK(selmap_record(s.game, sel) == nullptr);
    uint8_t icon[kModelIconSrtBytes];
    for (unsigned i = 0; i < kModelIconSrtBytes; ++i) icon[i] = uint8_t(i * 7 + 3);
    for (int k = 0; k < 4; ++k) { float m[16]; for (int i = 0; i < 16; ++i) m[i] = float(k * 100 + i + 1); std::memcpy(icon + 64 * k, m, 64); }
    int idx = 7;
    CHECK(streq(icon_camera_index(icon, sel, idx), "brief3d-icon-camera-unmatched") && idx == -1);
    std::memcpy(icon + 128, sel, 64);
    CHECK(icon_camera_index(icon, sel, idx) == nullptr && idx == 2);
    std::memcpy(icon + 192, sel, 64);
    CHECK(streq(icon_camera_index(icon, sel, idx), "brief3d-icon-camera-ambiguous") && idx == -1);
    std::memcpy(icon + 192, icon + 0, 64);   // undo: matrix 3 = matrix 0 again
    CHECK(icon_camera_index(icon, sel, idx) == nullptr && idx == 2);
    CHECK(streq(icon_camera_index(nullptr, sel, idx), "brief3d-icon-camera-inputs"));
    Mat4 eye; for (int i = 0; i < 16; ++i) eye.m[i] = 0.5 * i;
    uint8_t out[kModelIconSrtBytes];
    CHECK(modelicon_record(icon, 2, eye, out) == nullptr);
    CHECK(std::memcmp(out, icon, 128) == 0 && std::memcmp(out + 192, icon + 192, 80) == 0);
    const Mat4 back = load(out + 128);
    for (int i = 0; i < 16; ++i) CHECK(back.m[i] == 0.5 * i);
    CHECK(streq(modelicon_record(icon, 4, eye, out), "brief3d-icon-index"));
    Mat4 inf; inf.m[3] = 1e300;
    CHECK(streq(modelicon_record(icon, 1, inf, out), "brief3d-icon-nonfinite"));
    CHECK(streq(selmap_record(inf, sel), "brief3d-selmap-nonfinite"));
}

void test_replay_plan() {
    RootSwap r;
    const uint64_t up = 0x10000000ull;
    CHECK(plan_root_swap(Kind::selmap, 0, 0xABC000, up, kSlotUploadBytes, 0, r) == nullptr);
    CHECK(r.parameter == 0 && r.restore_va == 0xABC000 && r.eye_va[0] == up && r.eye_va[1] == up + 256);
    CHECK(plan_root_swap(Kind::modelicon, 0, 0xABC000, up, kSlotUploadBytes, kIconCap - 1, r) == nullptr);
    CHECK(r.eye_va[1] + kCopyStrideIcon == up + uint64_t(kSelMapCap) * 2 * kCopyStrideSel + uint64_t(kIconCap) * 2 * kCopyStrideIcon);
    CHECK(r.eye_va[1] + kCopyStrideIcon <= up + kSlotUploadBytes);
    CHECK(streq(plan_root_swap(Kind::selmap, 0, 0xABC000, up, kSlotUploadBytes, kSelMapCap, r), "brief3d-replay-cap") && r.parameter == UINT32_MAX);
    CHECK(streq(plan_root_swap(Kind::modelicon, 0, 0xABC000, up, kSlotUploadBytes, kIconCap, r), "brief3d-replay-cap"));
    CHECK(streq(plan_root_swap(Kind::selmap, UINT32_MAX, 0xABC000, up, kSlotUploadBytes, 0, r), "brief3d-t10-parameter"));
    CHECK(streq(plan_root_swap(Kind::selmap, 0, 0, up, kSlotUploadBytes, 0, r), "brief3d-t10-game-va"));
    CHECK(streq(plan_root_swap(Kind::selmap, 0, 1, up + 4, kSlotUploadBytes, 0, r), "brief3d-upload-va"));
    CHECK(streq(plan_root_swap(Kind::modelicon, 0, 1, up, 4096, 10, r), "brief3d-upload-span"));
    CHECK(streq(plan_root_swap(Kind::none, 0, 1, up, kSlotUploadBytes, 0, r), "brief3d-kind"));
    // no overlap between kinds / eyes / draws
    CHECK(copy_offset(Kind::selmap, kSelMapCap - 1, 1) + kCopyStrideSel <= copy_offset(Kind::modelicon, 0, 0));
    CHECK(copy_offset(Kind::modelicon, 5, 0) + kCopyStrideIcon == copy_offset(Kind::modelicon, 5, 1));
    CHECK(kCopyStrideSel >= kSelMapSrtBytes && kCopyStrideIcon >= kModelIconSrtBytes);
    // the command sequence
    plan_root_swap(Kind::selmap, 0, 0xABC000, up, kSlotUploadBytes, 0, r);
    Cmd c[kMaxCmds];
    unsigned n = replay_commands(r, true, true, c);
    CHECK(n == 18 && n <= kMaxCmds);   // one copy (brief3d 9.2: kMaxCmds covers kMaxIconCopies)
    CHECK(c[0].kind == CmdKind::clear_rt && c[1].kind == CmdKind::clear_ds && c[2].kind == CmdKind::clear_rt && c[3].kind == CmdKind::clear_ds);
    unsigned draws = 0, srv = 0;
    for (unsigned i = 0; i < n; ++i) { if (c[i].kind == CmdKind::draw) ++draws; if (c[i].kind == CmdKind::root_srv) ++srv; }
    CHECK(draws == 2 && srv == 3);
    CHECK(c[7].kind == CmdKind::root_srv && c[7].va == r.eye_va[0] && c[8].kind == CmdKind::draw && c[8].eye == 0);
    CHECK(c[12].kind == CmdKind::root_srv && c[12].va == r.eye_va[1] && c[13].eye == 1);
    CHECK(c[14].kind == CmdKind::root_srv && c[14].va == 0xABC000 && c[15].kind == CmdKind::vp_restore && c[16].kind == CmdKind::sc_restore &&
          c[17].kind == CmdKind::om_restore);
    CHECK(replay_commands(r, false, true, c) == 14 && c[0].kind == CmdKind::om_eye);
    CHECK(replay_commands(r, true, false, c) == 16);
    RootSwap none; CHECK(replay_commands(none, true, true, c) == 0);
}

void test_budget_and_gate() {
    uint64_t ch = 0;
    // D-370 formats: R16G16B16A16_FLOAT (8 B) + D32_FLOAT_S8X24 (8 B), 1440 x 1440: 3 slots x (2 colour + 2 depth + upload) = 191.8 MiB (< 200)
    CHECK(budget(8, 8, 1440, 1440, 3, ch) == nullptr && ch == 3ull * (4 * 16711680ull + 196608ull));
    CHECK(budget(4, 0, 1440, 1440, 3, ch) == nullptr && ch < (55ull << 20));
    CHECK(streq(budget(8, 8, 2560, 1440, 3, ch), "brief3d-over-total-cap") && ch == 0);
    CHECK(streq(budget(16, 4, 2560, 1440, 3, ch), "brief3d-over-image-cap"));
    CHECK(streq(budget(0, 4, 1440, 1440, 3, ch), "brief3d-format-unsupported"));
    CHECK(streq(budget(4, 4, 0, 1440, 3, ch), "brief3d-size-unsupported"));
    GateIn g; g.kind = Kind::selmap; g.ready = g.frame_ok = g.root_ok = g.t10_va = g.b10_written = g.predication_off = true;
    g.pso_rtv0 = 28; g.pso_rt_count = 1; g.image_format = 28; g.pso_dsv = 40; g.image_ds_format = 40; g.pso_samples = 1;
    g.depth_enable = true; g.depth_func = kCmpGreaterEqual; g.vp_known = g.scissor_known = true; g.viewports = g.scissors = 1;
    CHECK(gate(g) == nullptr);
    struct Case { void (*mut)(GateIn&); const char* why; };
    const Case cases[] = {
        {[](GateIn& x) { x.kind = Kind::none; }, "brief3d-not-a-map-draw"},
        {[](GateIn& x) { x.ready = false; }, "brief3d-not-ready"},
        {[](GateIn& x) { x.frame_ok = false; }, "brief3d-frame-not-ready"},
        {[](GateIn& x) { x.root_ok = false; }, "brief3d-root-layout"},
        {[](GateIn& x) { x.t10_va = false; }, "brief3d-t10-va"},
        {[](GateIn& x) { x.b10_written = false; }, "brief3d-b10-unwritten"},
        {[](GateIn& x) { x.predication_off = false; }, "brief3d-predicated"},
        {[](GateIn& x) { x.pso_samples = 4; }, "brief3d-pso-multisample"},
        {[](GateIn& x) { x.pso_rtv0 = 24; }, "brief3d-rt-format-differs"},
        {[](GateIn& x) { x.pso_rt_count = 2; }, "brief3d-rt-format-differs"},
        {[](GateIn& x) { x.pso_dsv = 0; }, "brief3d-dsv-format-differs"},
        {[](GateIn& x) { x.stencil = true; }, "brief3d-pso-stencil-test"},
        {[](GateIn& x) { x.depth_func = kCmpNotEqual; }, "brief3d-pso-depth-func"},
        {[](GateIn& x) { x.scissor_known = false; }, "brief3d-viewport-unknown"},
        {[](GateIn& x) { x.viewports = 2; }, "brief3d-viewport-unknown"},
        {[](GateIn& x) { x.kind = Kind::modelicon; x.icon_camera = false; }, "brief3d-icon-camera"},
    };
    for (const auto& k : cases) { GateIn x = g; k.mut(x); CHECK(streq(gate(x), k.why)); }
    GateIn nodepth = g; nodepth.depth_enable = false; nodepth.depth_func = 0; CHECK(gate(nodepth) == nullptr);
    GateIn icon = g; icon.kind = Kind::modelicon; CHECK(gate(icon) == nullptr);
    GateIn st = g; st.stencil = true; st.stencil_always = true; CHECK(gate(st) == nullptr);   // D-370: stencil on, both faces ALWAYS
}

void test_lines() {
    char buf[kLineLimit + 64];
    FrameState fs; fs.begin(); fs.selmap = 4000000; fs.icons = 4000000; fs.replays = 1; fs.refused = 99; fs.last_why = "brief3d-eye-inside-volume-and-more-text";
    int n = format_frame(buf, sizeof buf, ~0ull, fs, 4000000);
    CHECK(n > 0 && size_t(n) < kLineLimit - kLinePrefix && std::string(buf).find("brief3d-frame;") == 0);
    Box b; b.valid = true; b.vertices = 16384; for (int k = 0; k < 3; ++k) { b.lo[k] = -1.234567e30; b.hi[k] = 9.87654e30; }
    Fit f; f.valid = true; f.up = 1; f.up_sign = -1; f.a1 = 0; f.a2 = 2; f.a2_sign = 1; f.metres_per_unit = 1e-300; f.vexag = 8;
    n = format_fit(buf, sizeof buf, b, f, "a-very-long-reason-that-is-cut-at-thirty-two-characters");
    CHECK(n > 0 && size_t(n) < kLineLimit - kLinePrefix && std::string(buf).find("up=1-") != std::string::npos);
    Fit bad; n = format_fit(buf, sizeof buf, Box{}, bad, "brief3d-box-invalid");
    CHECK(n > 0 && std::string(buf).find("outcome=refused;reason=brief3d-box-invalid") != std::string::npos);
}

} // namespace

int main() {
    test_options();
    test_identity_and_root();
    test_projection();
    test_placement();
    test_fit();
    test_content_fit();
    test_end_to_end_and_guard();
    test_icon_records();
    test_replay_plan();
    test_budget_and_gate();
    test_lines();
    std::printf("brief3d_offline: %u checks passed\n", g_checks);
    return 0;
}
