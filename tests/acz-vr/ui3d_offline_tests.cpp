// UI3D stage 1 offline tests (src/acz-vr/ui3d.h; docs/world-render/ui-3d.md): options (the day value unchanged byte for byte), GS -> screen mapping,
// layer distances, direction preservation (plane, tilt, cylinder), alignment of all layers at the anchor eye (= the flat image), stereo parallax,
// perspective-correct attribute interpolation of a rewritten quad on a tilted layer, culling, refusals, the online classifier and the summary line.
// Nothing here touches D3D12; fixture success says nothing about the live game or a headset.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui3d.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz::ui3d;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
static bool near_eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static bool streq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

namespace {

const Cb0 kCb{512.0f, -448.0f, 2048.0f, 2048.0f};   // the in-flight ps2_v values (fx_sprite_replay.h); the menu value is read live (stage 2 census)
const SourceRect kPillar{0.125, 0.0, 0.75, 1.0};     // a 4:3 viewport pillarboxed in 16:9 (R4 screenshots) [I]

Screen default_screen(float yaw = 0.0f, const double anchor_in[3] = nullptr) {
    const double a0[3] = {0.1, 1.2, -0.3};
    const double* a = anchor_in ? anchor_in : a0;
    // vscreen::place defaults: 2.5 m ahead at the head yaw, 3.2 m wide, 16:9, vsy 0
    const float fx = -std::sin(yaw), fz = -std::cos(yaw);
    const float c[3] = {float(a[0] + fx * 2.5), float(a[1]), float(a[2] + fz * 2.5)};
    return make_screen(c, yaw, 3.2f, 1.8f, a);
}
EyeView eye_at(const double p[3], float yaw) {
    EyeView e;
    for (int i = 0; i < 3; ++i) e.p[i] = p[i];
    e.q[0] = 0; e.q[1] = std::sin(yaw * 0.5); e.q[2] = 0; e.q[3] = std::cos(yaw * 0.5);
    e.fov[0] = -0.85; e.fov[1] = 0.75; e.fov[2] = 0.80; e.fov[3] = -0.85;   // asymmetric like a real headset eye
    return e;
}
void put_vertex(uint8_t* v, float x, float y, float z, float q, float s, float t) {
    float f[16] = {x, y, z, q, 0.5f, 0.25f, 0.75f, 1.0f, s, t, 0.0f, 0.125f, 0.3f, 0.6f, 0.9f, 0.0f};
    std::memcpy(v, f, 64);
}

void test_options() {
    std::string rest, why; Options o;
    const std::string day = "auto;env:1;props:1;trees:1;vscreen:1;vsearly:1;brief3d:1;mk360:3";
    CHECK(extract_options(day, rest, o, why) && rest == day && !o.on && !o.any_token);
    CHECK(extract_options("auto;ui3d:1;env:1;u3n=1.5;u3f:4;u3t:10;u3c:1", rest, o, why));
    CHECK(rest == "auto;env:1" && o.on && o.any_token && o.curve);
    CHECK(near_eq(o.near_m, 1.5, 1e-6) && near_eq(o.far_m, 4.0, 1e-6) && near_eq(o.tilt_deg, 10.0, 1e-6));
    CHECK(extract_options("ui3d:1", rest, o, why) && rest == "ui3d:1" && !o.on);   // the first item is the stage id
    CHECK(!extract_options("auto;ui3d:2", rest, o, why) && why.find("ui3d") != std::string::npos);
    CHECK(!extract_options("auto;u3n:0.1", rest, o, why));
    CHECK(!extract_options("auto;u3f:99", rest, o, why));
    CHECK(!extract_options("auto;u3t:45", rest, o, why));
    CHECK(!extract_options("auto;u3c:x", rest, o, why));
    CHECK(!extract_options("auto;u3n:3;u3f:2", rest, o, why) && why.find("u3n") != std::string::npos);
    CHECK(extract_options("auto;ui3d:0", rest, o, why) && rest == "auto" && !o.on && o.any_token);
    CHECK(streq(prerequisites(Options{}, true, true, true), "off"));
    Options on; on.on = true;
    CHECK(streq(prerequisites(on, false, true, true), "needs-vscreen:1"));
    CHECK(streq(prerequisites(on, true, false, true), "needs-vsearly:1"));
    CHECK(streq(prerequisites(on, true, true, false), "needs-xr_hud_layer"));
    CHECK(prerequisites(on, true, true, true) == nullptr);
    CHECK(describe(on) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3mn=1.10;u3mf=6.00;u3s=0.70");
    // the terrain option buffer holds 255 characters: the day value + the longest ui3d set still fits
    CHECK(std::string(day + ";ui3d:1;u3n:1.8;u3f:3.5;u3t:-12.5;u3c:1;u3mn:1.1;u3mf:6.0;u3s:0.7").size() < 255);
}

void test_layers() {
    Options o;
    const double f = layer_distance(o, 0), m = layer_distance(o, 1), n = layer_distance(o, 2);
    CHECK(near_eq(f, 3.5, 1e-6) && near_eq(n, 1.8, 1e-6) && n < m && m < f);
    CHECK(near_eq(1.0 / m, 0.5 * (1.0 / double(o.far_m) + 1.0 / double(o.near_m)), 1e-12));   // halfway in dioptres
    CHECK(near_eq(layer_distance(o, 7), n, 1e-12));                  // clamped
}

void test_gs_mapping() {
    double u, v;
    gs_to_screen(1792, 1824, kCb, kPillar, u, v);   // GS top-left of the window (sy < 0: y grows downwards)
    CHECK(near_eq(u, -0.75, 1e-12) && near_eq(v, 1.0, 1e-12));
    gs_to_screen(2304, 2272, kCb, kPillar, u, v);
    CHECK(near_eq(u, 0.75, 1e-12) && near_eq(v, -1.0, 1e-12));
    gs_to_screen(2048, 2048, kCb, SourceRect{}, u, v);
    CHECK(near_eq(u, 0.0, 1e-12) && near_eq(v, 0.0, 1e-12));
    CHECK(!cb0_ok(Cb0{}) && cb0_ok(kCb) && rect_ok(kPillar) && !rect_ok(SourceRect{0.5, 0, 0.8, 1}) && !rect_ok(SourceRect{0, 0, 0, 1}));
    double w, h; uint8_t q[4 * 64];
    put_vertex(q + 0, 1792, 1824, 0, 1, 0, 0); put_vertex(q + 64, 2304, 1824, 0, 1, 1, 0);
    put_vertex(q + 128, 1792, 2272, 0, 1, 0, 1); put_vertex(q + 192, 2304, 2272, 0, 1, 1, 1);
    CHECK(screen_bbox(q, 4, kCb, kPillar, w, h) && near_eq(w, 0.75, 1e-9) && near_eq(h, 1.0, 1e-9));
}

void test_direction_preservation() {
    const float yaws[3] = {0.0f, 0.7f, -2.4f};
    for (float yaw : yaws) {
        const Screen s = default_screen(yaw);
        CHECK(s.valid && near_eq(s.dist, 2.5, 1e-5));
        for (int mode = 0; mode < 3; ++mode) {
            Options o; o.tilt_deg = mode == 1 ? 15.0f : 0.0f; o.curve = mode == 2;
            for (unsigned k = 0; k < kLayers; ++k)
                for (double u = -1; u <= 1.0001; u += 0.25)
                    for (double v = -1; v <= 1.0001; v += 0.5) {
                        double S[3], P[3]; screen_point(s, u, v, S);
                        CHECK(layer_point(s, o, k, u, v, P) == nullptr);
                        const double a[3] = {S[0] - s.anchor[0], S[1] - s.anchor[1], S[2] - s.anchor[2]};
                        const double b[3] = {P[0] - s.anchor[0], P[1] - s.anchor[1], P[2] - s.anchor[2]};
                        const double cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
                        CHECK(near_eq(std::sqrt(cx * cx + cy * cy + cz * cz), 0.0, 1e-9) && dot3(a, b) > 0);   // same ray from the anchor
                        const double D = layer_distance(o, k);
                        if (mode == 0) CHECK(near_eq(dot3(b, s.fwd), D, 1e-9));
                        if (mode == 1) {   // on the tilted plane through anchor + D fwd
                            const double tr = 15.0 * 3.14159265358979323846 / 180.0;
                            double n[3]; for (int i = 0; i < 3; ++i) n[i] = std::cos(tr) * s.fwd[i] - std::sin(tr) * s.up[i];
                            CHECK(near_eq(dot3(b, n), D * std::cos(tr), 1e-9));
                            if (v > 0.5) CHECK(dot3(b, s.fwd) > D);    // the top recedes
                            if (v < -0.5) CHECK(dot3(b, s.fwd) < D);
                        }
                        if (mode == 2) CHECK(near_eq(std::hypot(dot3(b, s.right), dot3(b, s.fwd)), D, 1e-9));
                    }
        }
    }
    Screen bad; Options o; double P[3];
    CHECK(streq(layer_point(bad, o, 0, 0, 0, P), "ui3d-screen-invalid"));
}

// From the anchor eye every layer projects exactly where the flat screen point projects: the composite is the game's flat image.
void test_anchor_alignment_and_parallax() {
    const float yaw = 0.4f;
    const Screen s = default_screen(yaw);
    const EyeView e0 = eye_at(s.anchor, yaw);
    for (int mode = 0; mode < 3; ++mode) {
        Options o; o.tilt_deg = mode == 1 ? -20.0f : 0.0f; o.curve = mode == 2;
        for (double u = -1; u <= 1.0001; u += 0.5)
            for (double v = -1; v <= 1.0001; v += 0.5) {
                double S[3], nx0, ny0, w0; screen_point(s, u, v, S);
                CHECK(project(e0, S, nx0, ny0, w0));
                for (unsigned k = 0; k < kLayers; ++k) {
                    double P[3], nx, ny, w; CHECK(layer_point(s, o, k, u, v, P) == nullptr);
                    CHECK(project(e0, P, nx, ny, w));
                    CHECK(near_eq(nx, nx0, 1e-9) && near_eq(ny, ny0, 1e-9));
                }
            }
    }
    // Stereo: the right eye (+32 mm along the screen's right axis) sees every layer shifted left by ipd/2 / D (in tan units) relative to the anchor.
    Options o;
    double pr[3]; for (int i = 0; i < 3; ++i) pr[i] = s.anchor[i] + 0.032 * s.right[i];
    const EyeView er = eye_at(pr, yaw);
    const double span = std::tan(er.fov[1]) - std::tan(er.fov[0]);
    double shift[kLayers];
    for (unsigned k = 0; k < kLayers; ++k) {
        double P[3], nx0, ny0, w0, nx, ny, w;
        CHECK(layer_point(s, o, k, 0.0, 0.0, P) == nullptr);
        CHECK(project(e0, P, nx0, ny0, w0) && project(er, P, nx, ny, w));
        shift[k] = nx - nx0;
        CHECK(near_eq(shift[k], -2.0 * 0.032 / layer_distance(o, k) / span, 1e-9));
        CHECK(near_eq(ny, ny0, 1e-9));
    }
    CHECK(shift[2] < shift[1] && shift[1] < shift[0] && shift[0] < 0);   // the near (text) layer has the largest disparity
}

// The rewritten quad is drawn by the game's own shader: X = (x - ox) 2 / sx, w_clip = 1 / q, attributes perspective-correct. Simulate the
// rasteriser along an edge of the eye image and compare the interpolated texcoord with the source texcoord of the same LAYER point.
void test_rewrite_and_perspective() {
    const float yaw = 0.0f;
    const Screen s = default_screen(yaw);
    double pe[3] = {s.anchor[0] + 0.25, s.anchor[1] - 0.15, s.anchor[2] + 0.1};   // a moved head, not the anchor
    const EyeView eye = eye_at(pe, 0.15f);
    Options o; o.tilt_deg = 25.0f;
    const unsigned layer = 1;
    uint8_t src[4 * 64], dst[4 * 64];
    // a panel quad in GS coordinates (strip order), uv 0..1, q = 1 (affine on the PS2 screen)
    put_vertex(src + 0, 1850, 1900, 1234.0f, 1.0f, 0, 0); put_vertex(src + 64, 2200, 1900, 1234.0f, 1.0f, 1, 0);
    put_vertex(src + 128, 1850, 2150, 1234.0f, 1.0f, 0, 1); put_vertex(src + 192, 2200, 2150, 1234.0f, 1.0f, 1, 1);
    const RewriteResult r = rewrite_vertices(src, 4, kCb, kPillar, s, o, layer, eye, dst);
    CHECK(r.why == nullptr && !r.culled);
    for (int i = 0; i < 4; ++i) {
        CHECK(std::memcmp(src + i * 64 + 8, dst + i * 64 + 8, 4) == 0);      // z unchanged
        CHECK(std::memcmp(src + i * 64 + 16, dst + i * 64 + 16, 48) == 0);   // colours + texcoord unchanged
    }
    auto fl = [&](const uint8_t* v, int i) { float f; std::memcpy(&f, v + 4 * i, 4); return double(f); };
    // what the game's VS outputs for the rewritten vertex: eye NDC, w = 1/q
    auto ndc = [&](const uint8_t* v, double& X, double& Y, double& w) {
        X = (fl(v, 0) - kCb.ox) * 2.0 / kCb.sx; Y = (fl(v, 1) - kCb.oy) * 2.0 / kCb.sy; w = 1.0 / fl(v, 3);
    };
    // every vertex lands where an independent projection of its layer point lands
    for (int i = 0; i < 4; ++i) {
        double u, v, P[3], nx, ny, w, X, Y, wq;
        gs_to_screen(fl(src + i * 64, 0), fl(src + i * 64, 1), kCb, kPillar, u, v);
        CHECK(layer_point(s, o, layer, u, v, P) == nullptr && project(eye, P, nx, ny, w));
        ndc(dst + i * 64, X, Y, wq);
        const double PA[3] = {P[0] - s.anchor[0], P[1] - s.anchor[1], P[2] - s.anchor[2]};
        CHECK(near_eq(X, nx, 2e-6) && near_eq(Y, ny, 2e-6) && near_eq(wq, w / dot3(PA, s.fwd), 1e-5 * wq));   // 1/q' = w_eye / w_anchor
    }
    // edge 0 -> 2 (the left edge, s = 0, t 0..1): sample screen positions, interpolate t with weights 1/w (hardware), and compare with the t of
    // the layer point seen at that eye pixel (unprojected through the eye onto the layer plane, then back along the anchor ray to the PS2 screen).
    double X0, Y0, W0, X2, Y2, W2; ndc(dst, X0, Y0, W0); ndc(dst + 128, X2, Y2, W2);
    const double tl = std::tan(eye.fov[0]), tr = std::tan(eye.fov[1]), tu = std::tan(eye.fov[2]), td = std::tan(eye.fov[3]);
    const double trad = 25.0 * 3.14159265358979323846 / 180.0, D = layer_distance(o, layer);
    double n[3]; for (int i = 0; i < 3; ++i) n[i] = std::cos(trad) * s.fwd[i] - std::sin(trad) * s.up[i];
    for (double a = 0.1; a < 0.95; a += 0.2) {
        const double X = X0 + a * (X2 - X0), Y = Y0 + a * (Y2 - Y0);
        const double hw = (1 - a) / W0 + a / W2;
        const double t_hw = (a / W2) / hw;   // perspective-correct t (t0 = 0, t2 = 1)
        // the eye ray through (X, Y): view dir = (tanx, tany, -1), rotate to LOCAL
        const double tx = 0.5 * (X * (tr - tl) + (tr + tl)), ty = 0.5 * (Y * (tu - td) + (tu + td));
        const double qx = eye.q[0], qy = eye.q[1], qz = eye.q[2], qw = eye.q[3];
        const double vd[3] = {tx, ty, -1.0};
        const double t2[3] = {2 * (qy * vd[2] - qz * vd[1]), 2 * (qz * vd[0] - qx * vd[2]), 2 * (qx * vd[1] - qy * vd[0])};
        const double d[3] = {vd[0] + qw * t2[0] + (qy * t2[2] - qz * t2[1]), vd[1] + qw * t2[1] + (qz * t2[0] - qx * t2[2]),
                             vd[2] + qw * t2[2] + (qx * t2[1] - qy * t2[0])};
        // intersect with the layer plane dot(P - anchor, n) = D cos
        const double ea[3] = {eye.p[0] - s.anchor[0], eye.p[1] - s.anchor[1], eye.p[2] - s.anchor[2]};
        const double lam = (D * std::cos(trad) - dot3(ea, n)) / dot3(d, n);
        double P[3]; for (int i = 0; i < 3; ++i) P[i] = eye.p[i] + lam * d[i];
        // back along the anchor ray onto the flat screen, then to PS2 GS y
        const double b[3] = {P[0] - s.anchor[0], P[1] - s.anchor[1], P[2] - s.anchor[2]};
        const double k = s.dist / dot3(b, s.fwd);
        double S[3]; for (int i = 0; i < 3; ++i) S[i] = s.anchor[i] + k * b[i];
        const double sc[3] = {S[0] - s.c[0], S[1] - s.c[1], S[2] - s.c[2]};
        const double v = dot3(sc, s.up) / s.half_h;            // screen v
        const double py = (1.0 - v) * 0.5;                      // image y fraction
        const double Yg = 1.0 - 2.0 * (py - kPillar.y0) / kPillar.h;
        const double gy = kCb.oy + Yg * kCb.sy * 0.5;           // GS y
        const double t_true = (gy - 1900.0) / (2150.0 - 1900.0);   // affine on the PS2 screen (q = 1)
        CHECK(near_eq(t_hw, t_true, 2e-5));
        const double t_affine = a;                               // what q unchanged (w = 1) would give
        if (a > 0.2 && a < 0.8) CHECK(std::fabs(t_affine - t_true) > 1e-4);   // the tilt really needs the perspective weights
    }
}

void test_cull_and_refusals() {
    const Screen s = default_screen(0.0f);
    Options o;
    uint8_t src[4 * 64], dst[4 * 64];
    put_vertex(src + 0, 1850, 1900, 0, 1, 0, 0); put_vertex(src + 64, 2200, 1900, 0, 1, 1, 0);
    put_vertex(src + 128, 1850, 2150, 0, 1, 0, 1); put_vertex(src + 192, 2200, 2150, 0, 1, 1, 1);
    const EyeView back = eye_at(s.anchor, 3.14159265f);   // looking the other way
    RewriteResult r = rewrite_vertices(src, 4, kCb, kPillar, s, o, 0, back, dst);
    CHECK(r.why == nullptr && r.culled);
    const EyeView side = eye_at(s.anchor, 1.5707963f);    // 90 deg left: the screen's left part is beside / behind the eye plane
    r = rewrite_vertices(src, 4, kCb, kPillar, s, o, 2, side, dst);
    CHECK(r.why == nullptr && r.culled);
    const EyeView e = eye_at(s.anchor, 0.0f);
    CHECK(streq(rewrite_vertices(src, 4, Cb0{}, kPillar, s, o, 0, e, dst).why, "ui3d-cb0"));
    CHECK(streq(rewrite_vertices(src, 4, kCb, SourceRect{0, 0, 0, 0}, s, o, 0, e, dst).why, "ui3d-viewport"));
    CHECK(streq(rewrite_vertices(src, 4, kCb, kPillar, Screen{}, o, 0, e, dst).why, "ui3d-screen-invalid"));
    CHECK(streq(rewrite_vertices(src, 4, kCb, kPillar, s, o, 3, e, dst).why, "ui3d-layer"));
    CHECK(streq(rewrite_vertices(src, 0, kCb, kPillar, s, o, 0, e, dst).why, "ui3d-no-vertices"));
    CHECK(streq(rewrite_vertices(src, 65, kCb, kPillar, s, o, 0, e, dst).why, "ui3d-draw-too-many-vertices"));
    uint8_t bad[4 * 64]; std::memcpy(bad, src, sizeof bad);
    put_vertex(bad + 64, 2200, 1900, 0, 0.0f, 1, 0);
    CHECK(streq(rewrite_vertices(bad, 4, kCb, kPillar, s, o, 0, e, dst).why, "ui3d-vertex-q"));
    put_vertex(bad + 64, NAN, 1900, 0, 1.0f, 1, 0);
    CHECK(streq(rewrite_vertices(bad, 4, kCb, kPillar, s, o, 0, e, dst).why, "ui3d-vertex-nonfinite"));
    // the anchor eye, layer 0..2: never culled for a screen straight ahead
    for (unsigned k = 0; k < kLayers; ++k) { r = rewrite_vertices(src, 4, kCb, kPillar, s, o, k, e, dst); CHECK(!r.why && !r.culled); }
}

DrawFacts quad(double w, double h, uint32_t vs = kPs2Vs8) { DrawFacts d; d.vs8 = vs; d.vertices = 4; d.bbox_w = w; d.bbox_h = h; return d; }

void test_classifier() {
    {   // a main-menu-like frame: backdrop, its particles, panel frames, text, a highlight bar after text, a fade overlay
        LayerAssigner a;
        CHECK(a.assign(quad(0.75, 1.0)) == 0);     // the backdrop
        CHECK(a.assign(quad(0.01, 0.01)) == 0);    // a particle before any panel: backdrop
        CHECK(a.assign(quad(0.6, 0.05)) == 1);     // the panel bar
        CHECK(a.assign(quad(0.02, 0.03)) == 2);    // a glyph
        CHECK(a.assign(quad(0.3, 0.06)) == 2);     // a panel after text: never back to a farther layer
        CHECK(a.assign(quad(0.75, 1.0)) == 2);     // a full-screen overlay after content stays on top
        DrawFacts off = quad(1, 1); off.on_target = false;
        CHECK(a.assign(off) == kSkip);             // the final pass to another target: not replayed
        CHECK(a.usable() && a.draws() == 6 && a.skipped() == 1 && a.per_layer(0) == 2 && a.per_layer(1) == 1 && a.per_layer(2) == 3);
        Stats st; count_frame(st, a, 3);
        CHECK(st.frames == 1 && st.usable == 1 && st.draws == 6 && st.culled == 3 && st.layer[2] == 3);
    }
    {   // the briefing overview: a SelMap draw refuses the frame (brief3d owns those eye images)
        LayerAssigner a;
        CHECK(a.assign(quad(0.75, 1.0)) == 0);
        CHECK(a.assign(quad(1, 1, kSelMapVs8)) == kRefused && streq(a.why(), "ui3d-frame-has-3d"));
        CHECK(a.assign(quad(0.02, 0.02)) == kRefused && !a.usable());
        Stats st; count_frame(st, a, 0);
        CHECK(st.refused == 1 && streq(st.last_why, "ui3d-frame-has-3d"));
    }
    { LayerAssigner a; CHECK(a.assign(quad(1, 1, kAcmVs8)) == kRefused && streq(a.why(), "ui3d-frame-has-3d")); }        // the aircraft select model
    { LayerAssigner a; CHECK(a.assign(quad(1, 1, kMovieVs8)) == kRefused && streq(a.why(), "ui3d-frame-has-movie")); }
    { LayerAssigner a; CHECK(a.assign(quad(1, 1, 0x12345678u)) == kRefused && streq(a.why(), "ui3d-frame-has-other-shader")); }
    { LayerAssigner a; DrawFacts d = quad(1, 1, kAcmVs8); d.on_target = false; CHECK(a.assign(d) == kSkip && !a.refused()); }
    { LayerAssigner a; DrawFacts d = quad(0.1, 0.1); d.indexed = true; CHECK(a.assign(d) == kRefused && streq(a.why(), "ui3d-indexed-draw")); }
    { LayerAssigner a; DrawFacts d = quad(0.1, 0.1); d.vertices = 65; CHECK(a.assign(d) == kRefused && streq(a.why(), "ui3d-draw-too-many-vertices")); }
    { LayerAssigner a; CHECK(a.assign(quad(NAN, 0.1)) == kRefused && streq(a.why(), "ui3d-bbox")); }
    {   LayerAssigner a;
        for (uint32_t i = 0; i < kMaxDrawsPerFrame; ++i) CHECK(a.assign(quad(0.02, 0.02)) >= 0);
        CHECK(a.assign(quad(0.02, 0.02)) == kRefused && streq(a.why(), "ui3d-too-many-draws"));
    }
    {   // text only (no panel): everything stays on the backdrop layer = flat, never wrong
        LayerAssigner a;
        for (int i = 0; i < 10; ++i) CHECK(a.assign(quad(0.02, 0.03)) == 0);
    }
    { LayerAssigner a; CHECK(!a.usable()); }   // an empty frame is not shown in 3D
}

// ---- the live census of 2026-10-29 (D:\acz-evidence-archive\step124\runD1\logs\ee-camera.log, event=ui3d-census; ui-3d.md 6) as fixtures
const Cb0 kMenuCb{682.0f, -448.0f, 2048.0f, 2048.0f};   // cb0=rc0:682,-448,2048,2048 on every main/mission-menu draw [E]
struct CensusDraw { float x0, y0, x1, y1, z; };          // the logged v= corners (a 4-vertex quad: (x0,y0) (x0,y1) (x1,y0) (x1,y1))
DrawFacts census_facts(const CensusDraw& c, uint8_t* buf) {
    put_vertex(buf + 0, c.x0, c.y0, c.z, 1, 0, 0); put_vertex(buf + 64, c.x0, c.y1, c.z, 1, 0, 1);
    put_vertex(buf + 128, c.x1, c.y0, c.z, 1, 1, 0); put_vertex(buf + 192, c.x1, c.y1, c.z, 1, 1, 1);
    DrawFacts d; d.vs8 = kPs2Vs8; d.vertices = 4; d.z_known = true; d.z = c.z;
    const SourceRect r = rect_from_viewport(0, 0, 2560, 1440, 2560, 1440);   // vp=0,0,2560,1440 on rt 2560x1440 [E]
    CHECK(screen_bbox(buf, 4, kMenuCb, r, d.bbox_w, d.bbox_h));
    return d;
}

void test_census_fixtures() {
    const SourceRect r = rect_from_viewport(0, 0, 2560, 1440, 2560, 1440);
    CHECK(near_eq(r.x0, 0, 1e-12) && near_eq(r.w, 1, 1e-12) && near_eq(r.h, 1, 1e-12) && rect_ok(r));
    CHECK(!rect_ok(rect_from_viewport(0, 0, 2560, 1440, 0, 1440)));
    uint8_t b[256];
    // the backdrop: bbox=319.06,0,2240.9,1440 -> 0.7507 x 1.0 of the image (the 4:3 area comes from cb0, not from the viewport)
    DrawFacts d = census_facts({1792, 1824, 2304, 2272, 64}, b);
    CHECK(near_eq(d.bbox_w, 1921.84 / 2560.0, 2e-4) && near_eq(d.bbox_h, 1.0, 1e-9));
    // a main-menu glyph: 1824,1902..1835,1926 -> 41.3 x 77.1 px = 0.0161 x 0.0536: "small" only with the census threshold (> 0.05)
    d = census_facts({1824, 1902, 1835, 1926, 1}, b);
    CHECK(near_eq(d.bbox_w, 41.29 / 2560.0, 2e-4) && near_eq(d.bbox_h, 77.14 / 1440.0, 2e-4));
    CHECK(d.bbox_h > 0.05 && d.bbox_h <= kSmallDim && d.bbox_w <= kSmallDim);
    // the smallest panel tile seen (1809,1842..1841,1874: 0.047 x 0.071) stays medium
    d = census_facts({1809, 1842, 1841, 1874, 64}, b);
    CHECK(d.bbox_w <= kSmallDim && d.bbox_h > kSmallDim);
    {   // main menu, census s=3 ord 1..48 (the logged prefix of its 347 draws): 5 backdrop quads, 2 strips, 4 scroll pieces (z 64), 14 degenerate
        // lines, 2 tiles + the title panel, then glyphs (z 1). Stage-1 rules put the tiles/title on the text layer (the z-64 pieces promoted it);
        // the census rule keeps them with the panels and only the z-1 glyphs go near.
        const CensusDraw s3[] = {
            {1792, 1824, 2304, 2272, 64}, {1792, 1824, 2304, 2272, 64}, {1792, 1823, 2304, 2271, 64}, {1792, 1824, 2304, 2272, 64}, {1792, 1824, 2304, 2272, 64},
            {2265, 1887, 2281, 2047, 64}, {2259, 1898, 2275, 2166, 64},
            {2006, 1899, 2022, 1915, 64}, {2006, 1914, 2022, 1930, 64}, {2006, 1929, 2022, 1945, 64}, {2006, 1944, 2022, 1960, 64},
            {2006, 1966, 2022, 1966, 64}, {2006, 1981, 2022, 1981, 64}, {2006, 2010, 2022, 2010, 64}, {2006, 1995, 2022, 1995, 64},
            {2006, 1980, 2022, 1980, 64}, {2006, 1965, 2022, 1965, 64}, {2006, 1996, 2022, 1996, 64}, {2006, 2011, 2022, 2011, 64},
            {2006, 2026, 2022, 2026, 64}, {2006, 2041, 2022, 2041, 64}, {2006, 2056, 2022, 2056, 64}, {2006, 2071, 2022, 2071, 64},
            {2006, 2086, 2022, 2086, 64}, {2006, 2101, 2022, 2101, 64},
            {1809, 1842, 1841, 1874, 64}, {1860, 1842, 1892, 1874, 64}, {1814, 1881, 2014, 1945, 64},
            {1824, 1902, 1834, 1926, 1}, {1834, 1902, 1845, 1926, 1}, {1845, 1902, 1856, 1926, 1}, {1856, 1902, 1867, 1926, 1},
            {1846, 1932, 1851, 1938, 1}, {1824, 1962, 1835, 1986, 1}};
        const int want[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2};
        static_assert(sizeof want / sizeof want[0] == sizeof s3 / sizeof s3[0], "one expectation per draw");
        LayerAssigner a;
        for (size_t i = 0; i < sizeof s3 / sizeof s3[0]; ++i) CHECK(a.assign(census_facts(s3[i], b)) == want[i]);
        CHECK(a.usable() && a.per_layer(0) == 5 && a.per_layer(2) == 6);
        // the same frame without z (stage-1 rule): the first scroll piece already promotes to the text layer
        LayerAssigner old;
        int last = -9;
        for (size_t i = 0; i < 8; ++i) { DrawFacts f = census_facts(s3[i], b); f.z_known = false; last = old.assign(f); }
        CHECK(last == 2);
    }
    {   // mission menu, census s=58: 5 backdrop quads, 2 thin z-64 lines BEFORE any panel (backdrop), degenerate lines, one tile, glyphs
        const CensusDraw s58[] = {
            {1792, 1824, 2304, 2272, 64}, {1792, 1824, 2304, 2272, 64}, {1792, 1824, 2304, 2272, 64}, {1792, 1824, 2304, 2272, 64}, {1792, 1824, 2304, 2272, 64},
            {2006, 1909, 2022, 1910, 64}, {2006, 1939, 2022, 1940, 64}, {2006, 1970, 2022, 1970, 64}, {2006, 2000, 2022, 2000, 64},
            {1809, 1842, 1841, 1874, 64}, {1824, 1902, 1835, 1926, 1}, {1835, 1902, 1845, 1926, 1}, {1878, 1902, 1883, 1908, 1}};
        const int want[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2, 2};
        LayerAssigner a;
        for (size_t i = 0; i < sizeof s58 / sizeof s58[0]; ++i) CHECK(a.assign(census_facts(s58[i], b)) == want[i]);
    }
    {   // a degenerate draw never promotes, also at the start of a frame
        LayerAssigner a; uint8_t c[256];
        CHECK(a.assign(census_facts({2006, 1966, 2022, 1966, 64}, c)) == 0);
        CHECK(a.assign(census_facts({1792, 1824, 2304, 2272, 64}, c)) == 0);
    }
    {   // glyphs with the backdrop's z (no panel) stay flat; glyphs with another z over a backdrop (no panel) go near
        LayerAssigner a; uint8_t c[256];
        CHECK(a.assign(census_facts({1792, 1824, 2304, 2272, 64}, c)) == 0);
        CHECK(a.assign(census_facts({1824, 1902, 1835, 1926, 64}, c)) == 0);
        CHECK(a.assign(census_facts({1824, 1902, 1835, 1926, 1}, c)) == 2);
    }
    {   // the census vertices rewritten for the anchor eye land where the flat image puts them (layer 2, 1.8 m)
        const Screen s = default_screen(0.0f);
        Options o;
        uint8_t src[256], dst[256];
        census_facts({1824, 1902, 1835, 1926, 1}, src);
        const EyeView e = eye_at(s.anchor, 0.0f);
        const RewriteResult rr = rewrite_vertices(src, 4, kMenuCb, r, s, o, 2, e, dst);
        CHECK(!rr.why && !rr.culled);
        for (int i = 0; i < 4; ++i) {   // z (1) and the attribute bytes unchanged
            float z; std::memcpy(&z, dst + 64 * i + 8, 4); CHECK(z == 1.0f);
            CHECK(std::memcmp(dst + 64 * i + 16, src + 64 * i + 16, 48) == 0);
        }
    }
}

void test_eye_scissor() {
    const int32_t sc[4] = {320, 0, 2240, 1440};   // the menus' scissor [E]
    Options o;
    for (int curve = 0; curve < 2; ++curve) {
        o.curve = curve == 1;
        const Screen s = default_screen(0.2f);
        const EyeView e = eye_at(s.anchor, 0.2f);
        for (unsigned k = 0; k < kLayers; ++k) {
            PixRect pr;
            CHECK(eye_scissor(sc, 2560, 1440, s, o, k, e, 1440, 1440, pr));
            CHECK(pr.l >= 0 && pr.r <= 1440 && pr.t >= 0 && pr.b <= 1440 && pr.r > pr.l && pr.b > pr.t);
            // a point inside the scissor (the glyph) projects inside the rect; the overshooting title backdrop corner (s=1: 1473,1545) outside
            auto px = [&](float x, float y, double& X, double& Y) {
                double u, v; gs_to_screen(x, y, kMenuCb, SourceRect{}, u, v);
                double P[3], nx, ny, w; CHECK(!layer_point(s, o, k, u, v, P) && project(e, P, nx, ny, w));
                X = (nx + 1) * 0.5 * 1440; Y = (1 - ny) * 0.5 * 1440;
            };
            double X, Y;
            px(1830, 1914, X, Y); CHECK(X > pr.l && X < pr.r && Y > pr.t && Y < pr.b);
            px(1473, 1545, X, Y); CHECK(X < pr.l || Y < pr.t);
            // the scissor's own left edge maps onto the rect's left edge within a pixel (planes: exact; cylinder: the rect contains it)
            px(1792, 2048, X, Y); CHECK(X >= pr.l - 1.0 && X <= pr.l + 1.0);
        }
    }
    {   // looking back: the corners are behind the eye -> the whole eye image (the vertices cull the draw themselves)
        const Screen s = default_screen(0.0f);
        PixRect pr;
        CHECK(eye_scissor(sc, 2560, 1440, s, Options{}, 0, eye_at(s.anchor, 3.14159265f), 1440, 1440, pr) && pr.l == 0 && pr.r == 1440 && pr.b == 1440);
    }
    {   // empty / bad inputs
        const Screen s = default_screen(0.0f); PixRect pr;
        const int32_t empty[4] = {500, 0, 500, 1440};
        CHECK(!eye_scissor(empty, 2560, 1440, s, Options{}, 0, eye_at(s.anchor, 0.0f), 1440, 1440, pr));
        CHECK(!eye_scissor(sc, 0, 1440, s, Options{}, 0, eye_at(s.anchor, 0.0f), 1440, 1440, pr));
        // a scissor entirely left of the eye's view clamps to an empty rect: no draw for that eye
        const int32_t left[4] = {0, 0, 40, 1440};
        CHECK(!eye_scissor(left, 2560, 1440, s, Options{}, 0, eye_at(s.anchor, -0.9f), 1440, 1440, pr));
    }
}

void test_budget_and_summary() {
    CHECK(upload_bytes(350, 4) == 350ull * 2 * 4 * 64);
    CHECK(kUploadPerSlot == 512ull * 1024);
    Stats st;
    st.frames = st.usable = st.refused = st.draws = st.culled = ~0ull;
    for (auto& l : st.layer) l = ~0ull;
    st.last_why = "ui3d-frame-has-other-shader-and-a-very-long-tail-that-is-cut";
    const std::string line = summary_line(st);
    CHECK(line.size() < 256 && line.find("summary-ui3d;frames=9999999999") == 0);
    CHECK(summary_line(Stats{}).find("last-why=none") != std::string::npos);
}

} // namespace

// 2026-10-30b (step134 headset: the flat screen in front of the hangar room / briefing table, blurry panel): u3q / u3r / u3k.
void test_layering_and_filter_options() {
    std::string rest, why; Options o;
    CHECK(extract_options("auto;ui3d:1", rest, o, why));
    CHECK(o.layering == 1 && o.filter == 2 && near_eq(o.sharpen, 0.35, 1e-6));   // defaults: the new layer list, Catmull-Rom + unsharp
    CHECK(describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3mn=1.10;u3mf=6.00;u3s=0.70");   // the default line is byte-identical to before
    CHECK(extract_options("auto;ui3d:1;u3q:0;u3r:1;u3k:0.6", rest, o, why) && rest == "auto" && o.any_token);
    CHECK(o.layering == 0 && o.filter == 1 && near_eq(o.sharpen, 0.6, 1e-6));
    CHECK(describe(o).find(";u3s=0.70;u3q=0;u3r=1;u3k=0.60") != std::string::npos);
    CHECK(extract_options("auto;u3r:0", rest, o, why) && o.filter == 0);
    CHECK(!extract_options("auto;u3q:2", rest, o, why) && why.find("u3q") != std::string::npos);
    CHECK(!extract_options("auto;u3r:3", rest, o, why) && why.find("u3r") != std::string::npos);
    CHECK(!extract_options("auto;u3k:1.5", rest, o, why) && why.find("u3k") != std::string::npos);
    CHECK(!extract_options("auto;u3k:-0.1", rest, o, why));
    // the terrain option buffer (255 characters) still holds the longest set with the new tokens
    const char* day = "auto;vscreen:1;vsearly:1;terrain:1;cam:1,2,3,4;e6loc:auto;rc:1";
    CHECK(std::string(std::string(day) + ";ui3d:1;u3n:1.8;u3f:3.5;u3t:-12.5;u3c:1;u3mn:1.1;u3mf:6.0;u3s:0.7;u3q:1;u3r:2;u3k:0.35").size() < 255);
}

// 2026-10-30 (step133 headset feedback: the menus looked like the flat virtual screen): u3mn / u3mf / u3s, the scaled screen, the depth separation.
void test_menu_depth_and_scale() {
    std::string rest, why; Options o;
    CHECK(extract_options("auto;ui3d:1", rest, o, why));
    CHECK(near_eq(o.menu_near_m, 1.1, 1e-6) && near_eq(o.menu_far_m, 6.0, 1e-6) && near_eq(o.scale, 0.7, 1e-6));
    CHECK(near_eq(o.near_m, 1.8, 1e-6) && near_eq(o.far_m, 3.5, 1e-6));   // the pause menu's u3n / u3f are unchanged
    CHECK(extract_options("auto;ui3d:1;u3mn:0.8;u3mf=9;u3s:0.5", rest, o, why) && rest == "auto");
    CHECK(near_eq(o.menu_near_m, 0.8, 1e-6) && near_eq(o.menu_far_m, 9.0, 1e-6) && near_eq(o.scale, 0.5, 1e-6));
    CHECK(describe(o).find(";u3mn=0.80;u3mf=9.00;u3s=0.50") != std::string::npos);
    CHECK(!extract_options("auto;u3mn:0.2", rest, o, why) && why.find("u3mn") != std::string::npos);
    CHECK(!extract_options("auto;u3mf:40", rest, o, why) && why.find("u3mf") != std::string::npos);
    CHECK(!extract_options("auto;u3s:0.1", rest, o, why) && why.find("u3s") != std::string::npos);
    CHECK(!extract_options("auto;u3s:1.6", rest, o, why));
    CHECK(!extract_options("auto;u3mn:5;u3mf:4", rest, o, why) && why.find("u3mn") != std::string::npos);

    // scaled(): the same plane and anchor, every point's direction scaled: the panel's angular size is k times the vscreen's
    const Screen s = default_screen(0.4f);
    const Screen h = scaled(s, 0.5);
    CHECK(h.valid && near_eq(h.half_w, 0.5 * s.half_w, 1e-12) && near_eq(h.half_h, 0.5 * s.half_h, 1e-12) && near_eq(h.dist, s.dist, 1e-12));
    CHECK(!scaled(s, 0.0).valid && !scaled(s, -1.0).valid && !scaled(s, std::nan("")).valid);
    Options mo; mo.near_m = 1.1f; mo.far_m = 6.0f;
    for (unsigned k = 0; k < kLayers; ++k) {
        double P[3], C[3];
        CHECK(layer_point(h, mo, k, 1.0, 0.0, P) == nullptr && layer_point(h, mo, k, 0.0, 0.0, C) == nullptr);
        const double b[3] = {P[0] - s.anchor[0], P[1] - s.anchor[1], P[2] - s.anchor[2]};
        const double c[3] = {C[0] - s.anchor[0], C[1] - s.anchor[1], C[2] - s.anchor[2]};
        CHECK(near_eq(std::fabs(dot3(c, s.right)), 0.0, 1e-5) && near_eq(dot3(c, s.fwd), layer_distance(mo, k), 1e-5));   // the centre stays in front
        CHECK(near_eq(dot3(b, s.right) / dot3(b, s.fwd), 0.5 * s.half_w / s.dist, 1e-5));                                   // tan(half angle) halved
    }
    // depth separation: the stereo disparity difference between the text layer and the backdrop is far larger than the old 1.8 / 3.5 pair's
    auto disparity_gap = [&](const Options& op) {
        double ndc[2][3];
        for (unsigned k : {0u, 2u}) {
            double P[3]; CHECK(layer_point(s, op, k, 0.0, 0.0, P) == nullptr);
            for (int e = 0; e < 2; ++e) {
                double p[3] = {s.anchor[0] + (e ? 0.032 : -0.032) * std::cos(0.4), s.anchor[1], s.anchor[2] + (e ? 0.032 : -0.032) * (-std::sin(0.4))};
                EyeView ev = eye_at(p, 0.4f); double ny, w;
                CHECK(project(ev, P, ndc[e][k], ny, w));
            }
        }
        return (ndc[1][2] - ndc[0][2]) - (ndc[1][0] - ndc[0][0]);   // near disparity minus far disparity (NDC)
    };
    Options legacy; legacy.near_m = 1.8f; legacy.far_m = 3.5f;
    const double dm = std::fabs(disparity_gap(mo)), dl = std::fabs(disparity_gap(legacy));
    CHECK(dl > 0 && dm > 2.0 * dl);
    CHECK(layer_distance(mo, 0) / layer_distance(mo, 2) > 5.0);
}

// 2026-10-30c (headset test 5 item 1: "offset the lines is fine, but the selection box could be 3d and closer to the text so i can use it in
// VR"): u3sel / u3sg / u3sd / u3fs / u3at / u3ag, the selection rule on the census rows, the slab geometry, the backdrop scale.
void test_selection_slab_and_backdrop_scale() {
    std::string rest, why; Options o;
    CHECK(extract_options("auto;ui3d:1", rest, o, why));
    CHECK(o.select == 2 && near_eq(o.sel_gap_mm, 4.0, 1e-6) && near_eq(o.sel_depth_mm, 12.0, 1e-6) && near_eq(o.far_scale, 1.0, 1e-6));
    CHECK(!o.hangar_tint && o.hangar_glow);
    CHECK(describe(o) == "ui3d=1;u3n=1.80;u3f=3.50;u3t=0.0;u3c=0;u3mn=1.10;u3mf=6.00;u3s=0.70");   // the default line is unchanged
    CHECK(extract_options("auto;ui3d:1;u3sel:1;u3sg:2.5;u3sd:20;u3fs:1.5;u3a:1;u3at:1;u3ag:0", rest, o, why) && rest == "auto");
    CHECK(o.select == 1 && near_eq(o.sel_gap_mm, 2.5, 1e-6) && near_eq(o.sel_depth_mm, 20.0, 1e-6) && near_eq(o.far_scale, 1.5, 1e-6));
    CHECK(o.hangar_tint && !o.hangar_glow);
    CHECK(describe(o).find(";u3sel=1;u3sg=2.5;u3sd=20.0;u3fs=1.50;u3at=1;u3ag=0") != std::string::npos);
    CHECK(!extract_options("auto;u3sel:3", rest, o, why) && why.find("u3sel") != std::string::npos);
    CHECK(!extract_options("auto;u3sg:60", rest, o, why) && why.find("u3sg") != std::string::npos);
    CHECK(!extract_options("auto;u3sd:0.5", rest, o, why) && why.find("u3sd") != std::string::npos);
    CHECK(!extract_options("auto;u3fs:0.9", rest, o, why) && why.find("u3fs") != std::string::npos);
    CHECK(!extract_options("auto;u3at:2", rest, o, why) && !extract_options("auto;u3ag:x", rest, o, why));
    const char* day = "auto;vscreen:1;vsearly:1;terrain:1;cam:1,2,3,4;e6loc:auto;rc:1";
    CHECK(std::string(std::string(day) + ";ui3d:1;u3n:1.8;u3f:3.5;u3t:-12.5;u3c:1;u3mn:1.1;u3mf:6.0;u3s:0.7;u3q:1;u3r:2;u3k:0.35;u3sel:2;u3sg:4;u3sd:12;u3fs:1.5").size() < 255);

    // the selection rule on the census main-menu frame (step124 runD1 s=10/59, ui-3d.md 12.1): rows of glyphs 0.0536 high at y 0.174 / 0.241 /
    // 0.308 / 0.375 (x from 0.172); the 0.293 x 0.143 box at (0.157, 0.127) is the selection; the 0.375 x 0.143 header at (0.132, 0.109), the
    // panel strip, the tile, the backdrop and a bar on no row are not
    SelectionRows rows;
    auto box = [](double x0, double y0, double w, double h) { Box b; b.x0 = x0; b.y0 = y0; b.x1 = x0 + w; b.y1 = y0 + h; return b; };
    CHECK(!rows.selects(box(0.157, 0.127, 0.293, 0.143)));   // no rows yet (the first frame of a screen)
    for (double y : {0.174, 0.241, 0.308, 0.375})
        for (int g = 0; g < 8; ++g) rows.add_glyph(box(0.172 + 0.016 * g, y, 0.015, 0.0536));
    CHECK(rows.size() == 4);
    CHECK(rows.selects(box(0.157, 0.127, 0.293, 0.143)));
    CHECK(rows.selects(box(0.157, 0.194, 0.293, 0.143)));    // the box moved down one row (the user pressed down): still the selection
    CHECK(!rows.selects(box(0.132, 0.109, 0.375, 0.143)));   // the header panel: centre 0.38 rows off
    CHECK(!rows.selects(box(0.818, 0.141, 0.023, 0.357)));   // panel strip
    CHECK(!rows.selects(box(0.150, 0.040, 0.047, 0.071)));   // tile (not a bar)
    CHECK(!rows.selects(box(0.125, 0.0, 0.751, 1.0)));       // backdrop
    CHECK(!rows.selects(box(0.600, 0.174, 0.200, 0.054)));   // a bar on the row's line but beside the text
    CHECK(!rows.selects(box(0.157, 0.500, 0.293, 0.143)));   // a bar on no row
    rows.clear(); CHECK(rows.size() == 0 && !rows.selects(box(0.157, 0.127, 0.293, 0.143)));
    {   // screen_box: fractions, y down
        const SourceRect full{0, 0, 1, 1}; const Cb0 menu{682.0f, -448.0f, 2048.0f, 2048.0f};
        uint8_t q[4 * 64];
        put_vertex(q + 0, 1792, 1824, 64, 1, 0, 0); put_vertex(q + 64, 1792, 2272, 64, 1, 0, 1);
        put_vertex(q + 128, 2304, 1824, 64, 1, 1, 0); put_vertex(q + 192, 2304, 2272, 64, 1, 1, 1);
        Box b; CHECK(screen_box(q, 4, menu, full, b));
        CHECK(near_eq(b.x0, 0.125, 1e-3) && near_eq(b.x1, 0.875, 1e-3) && near_eq(b.y0, 0.0, 1e-9) && near_eq(b.y1, 1.0, 1e-9));
    }

    // the slab: back face, 4 walls, front face
    const Screen s = default_screen(0.3f);
    Options so;
    CHECK(near_eq(selection_front(so), layer_distance(so, 2) + 0.004, 1e-9) && near_eq(selection_back(so), selection_front(so) + 0.012, 1e-9));
    uint8_t src[4 * 64];
    put_vertex(src + 0, 1900, 1900, 64, 1, 0, 0); put_vertex(src + 64, 1900, 1950, 64, 1, 0, 1);
    put_vertex(src + 128, 2100, 1900, 64, 1, 1, 0); put_vertex(src + 192, 2100, 1950, 64, 1, 1, 1);
    double side[3]; for (int i = 0; i < 3; ++i) side[i] = s.anchor[i] + 0.25 * s.right[i] + 0.05 * s.up[i];
    const EyeView moved = eye_at(side, 0.3f);
    uint8_t slab[kSlabCopies * 4 * 64], ff[4 * 64], bf[4 * 64];
    const RewriteResult rs = rewrite_slab(src, 4, kCb, kPillar, s, so, selection_front(so), selection_back(so), moved, slab);
    CHECK(rs.why == nullptr && !rs.culled);
    CHECK(rewrite_at(src, 4, kCb, kPillar, s, so, selection_front(so), 1.0, moved, ff).why == nullptr);
    CHECK(rewrite_at(src, 4, kCb, kPillar, s, so, selection_back(so), 1.0, moved, bf).why == nullptr);
    CHECK(std::memcmp(slab, bf, sizeof bf) == 0 && std::memcmp(slab + 5 * 256, ff, sizeof ff) == 0);   // back first, front last (painter's)
    for (unsigned e = 0; e < 4; ++e) {
        const uint8_t* w = slab + (1 + e) * 256;
        const unsigned a = kSlabEdges[e][0], b = kSlabEdges[e][1];
        CHECK(std::memcmp(w + 0, ff + a * 64, 64) == 0 && std::memcmp(w + 64, bf + a * 64, 64) == 0);
        CHECK(std::memcmp(w + 128, ff + b * 64, 64) == 0 && std::memcmp(w + 192, bf + b * 64, 64) == 0);
    }
    // real depth: from the moved eye the front and back faces do not coincide (the walls have area); from the anchor eye the front face covers
    // exactly the flat image's box (direction kept), i.e. the same eye pixels as the text layer would give it
    auto fl = [](const uint8_t* v, int i) { float f; std::memcpy(&f, v + 4 * i, 4); return double(f); };
    CHECK(std::fabs(fl(ff, 0) - fl(bf, 0)) > 0.05);
    const EyeView at_anchor = eye_at(s.anchor, 0.3f);
    uint8_t fa[4 * 64], ta[4 * 64];
    CHECK(rewrite_at(src, 4, kCb, kPillar, s, so, selection_front(so), 1.0, at_anchor, fa).why == nullptr);
    CHECK(rewrite_vertices(src, 4, kCb, kPillar, s, so, 2, at_anchor, ta).why == nullptr);
    for (int i = 0; i < 4; ++i) CHECK(near_eq(fl(fa + 64 * i, 0), fl(ta + 64 * i, 0), 1e-3) && near_eq(fl(fa + 64 * i, 1), fl(ta + 64 * i, 1), 1e-3));
    // registration: the stereo disparity of the front face differs from the text layer's by < 2 % of the text-to-panel difference (0.9 %)
    auto disparity = [&](double D) {
        double P[3]; CHECK(surface_point(s, so, D, 1.0, 0.0, 0.0, P) == nullptr);
        double pl[3], pr[3]; for (int i = 0; i < 3; ++i) { pl[i] = s.anchor[i] - 0.032 * s.right[i]; pr[i] = s.anchor[i] + 0.032 * s.right[i]; }
        double xl, xr, y, w; CHECK(project(eye_at(pl, 0.3f), P, xl, y, w) && project(eye_at(pr, 0.3f), P, xr, y, w));
        return xl - xr;
    };
    const double dt = disparity(layer_distance(so, 2)), dp = disparity(layer_distance(so, 1)), dsf = disparity(selection_front(so));
    CHECK(std::fabs(dt - dsf) < 0.02 * std::fabs(dt - dp) && dsf < dt);   // just behind the text, far nearer to it than the panel layer
    CHECK(streq(rewrite_slab(src, 3, kCb, kPillar, s, so, 1.1, 1.2, moved, slab).why, "ui3d-slab-not-quad"));
    CHECK(streq(rewrite_slab(src, 4, kCb, kPillar, s, so, 1.2, 1.1, moved, slab).why, "ui3d-slab-depth"));
    {   // looking back: culled for that eye
        const EyeView back = eye_at(s.anchor, 0.3f + 3.14159265f);
        CHECK(rewrite_slab(src, 4, kCb, kPillar, s, so, selection_front(so), selection_back(so), back, slab).culled);
    }

    // u3fs: the backdrop layer spreads by k about the screen centre; the panel and text layers are unchanged
    Options big; big.far_scale = 1.5f;
    double P1[3], P0[3], Q1[3], Q0[3];
    CHECK(layer_point(s, big, 0, 0.6, -0.4, P1) == nullptr && surface_point(s, so, layer_distance(so, 0), 1.0, 0.9, -0.6, P0) == nullptr);
    for (int i = 0; i < 3; ++i) CHECK(near_eq(P1[i], P0[i], 1e-9));
    CHECK(layer_point(s, big, 2, 0.6, -0.4, Q1) == nullptr && layer_point(s, so, 2, 0.6, -0.4, Q0) == nullptr);
    for (int i = 0; i < 3; ++i) CHECK(near_eq(Q1[i], Q0[i], 1e-12));
    // its scissor grows with it (the game's 4:3 scissor carried onto the larger backdrop)
    const int32_t sc[4] = {320, 0, 2240, 1440};
    PixRect a1, a0;
    CHECK(eye_scissor(sc, 2560, 1440, s, big, 0, at_anchor, 1440, 1440, a1) && eye_scissor(sc, 2560, 1440, s, so, 0, at_anchor, 1440, 1440, a0));
    CHECK(a1.r - a1.l > a0.r - a0.l && a1.b - a1.t >= a0.b - a0.t);
}

int main() {
    test_selection_slab_and_backdrop_scale();
    test_menu_depth_and_scale();
    test_layering_and_filter_options();
    test_options();
    test_layers();
    test_gs_mapping();
    test_direction_preservation();
    test_anchor_alignment_and_parallax();
    test_rewrite_and_perspective();
    test_cull_and_refusals();
    test_classifier();
    test_census_fixtures();
    test_eye_scissor();
    test_budget_and_summary();
    std::printf("ui3d_offline: %u checks passed\n", g_checks);
    return 0;
}
