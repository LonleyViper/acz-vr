// EFFECTS-OUTSIDE (B) offline tests for the particle-memory core (src/acz-vr/fx_memory.h; docs/world-render/effects-outside-window.md).
// A synthetic game camera builds the world -> PS2-screen matrix A exactly in the form the stored frame has (x_s = 258.048 x_c + 2048 w, ..., z column
// Z0 e3 + b w); synthetic particles are "drawn by the game" as ps2_v quads (screen x/y, z = 2^24/w - 255.0078 or the cloud form z = 0 + q = 1/w), extracted,
// remembered and rendered by the CPU reference from a head pose 40 deg away from the game window. Optional real data: ACZ_E10_BLOBS=<dir with the E10
// RenderDoc export blobs 000334/000335> checks the stored frame-11906 cloud cards and tracer streaks (not in git: game capture data).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fx_memory.h"
#include "shader_dump.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace acz::fxmem;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTanX = 0.6152, kTanY = 0.3460;   // the game lens (E3: 63.2 x 38.1 deg)
double deg(double d) { return d * kPi / 180.0; }

struct Cam { double e[3]; double yaw, pitch; };
void axes(double yaw, double pitch, double f[3], double r[3], double u[3]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    f[0] = -sy * cp; f[1] = sp; f[2] = -cy * cp; r[0] = cy; r[1] = 0; r[2] = -sy;
    u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
}
// A for a game camera (row vectors, h = [P,1] A): Vw = f.(P-E), Vcx = r.(P-E)/(1.008 tanx), Vcy = -u.(P-E)/(1.008 tany) (GS y grows downward).
void make_a(const Cam& c, double a[16]) {
    double f[3], r[3], u[3]; axes(c.yaw, c.pitch, f, r, u);
    auto lin = [&](const double v[3], double s, double col[4]) {   // s * v.(P - E)
        for (int i = 0; i < 3; ++i) col[i] = s * v[i];
        col[3] = -s * (v[0] * c.e[0] + v[1] * c.e[1] + v[2] * c.e[2]);
    };
    double w[4], cx[4], cy[4];
    lin(f, 1.0, w); lin(r, 1.0 / (1.008 * kTanX), cx); lin(u, -1.0 / (1.008 * kTanY), cy);
    for (int k = 0; k < 4; ++k) {
        a[k * 4 + 0] = kGsScaleX * cx[k] + kGsCentre * w[k];
        a[k * 4 + 1] = kGsScaleY * cy[k] + kGsCentre * w[k];
        a[k * 4 + 2] = kDepthB * w[k] + (k == 3 ? kZ0 : 0.0);
        a[k * 4 + 3] = w[k];
    }
}
// The game's sprite for world point p with half size R facing the game camera: 4 strip vertices TL, TR, BL, BR at the centre's depth.
// cloud_form: z = 0 and q = 1/w (the stored cloud cards); else z = 2^24/w - 255.0078 and q = 1.
bool game_sprite(const ScreenMap& m, const Cam& c, const double p[3], double R, bool cloud_form, float alpha, Vtx out[4]) {
    double f[3], r[3], u[3]; axes(c.yaw, c.pitch, f, r, u);
    const double sx[4] = {-1, 1, -1, 1}, sy[4] = {1, 1, -1, -1};
    for (int k = 0; k < 4; ++k) {
        double q[3]; for (int i = 0; i < 3; ++i) q[i] = p[i] + R * (sx[k] * r[i] + sy[k] * u[i]);
        double xs, ys, d;
        if (!project(m, q, xs, ys, d)) return false;
        Vtx& v = out[k];
        v = Vtx{};
        v.pos[0] = float(xs); v.pos[1] = float(ys);
        v.pos[2] = cloud_form ? 0.0f : float(game_z_for_depth(d)); v.pos[3] = cloud_form ? float(1.0 / d) : 1.0f;
        v.col[0] = 0.9f; v.col[1] = 0.6f; v.col[2] = 0.3f; v.col[3] = alpha;
        v.uv[0] = sx[k] > 0 ? 1.0f : 0.0f; v.uv[1] = sy[k] > 0 ? 0.0f : 1.0f;
    }
    return true;
}
std::vector<Sprite> observe(const ScreenMap& m, const Cam& c, const std::vector<std::vector<double>>& pts, double R, bool cloud_form = false) {
    std::vector<Vtx> v; std::vector<uint16_t> idx;
    for (const auto& p : pts) {
        double xs, ys, d;
        if (!project(m, p.data(), xs, ys, d) || !in_window(xs, ys)) continue;   // the game only draws what is in its window
        Vtx q[4]; if (!game_sprite(m, c, p.data(), R, cloud_form, 1.0f, q)) continue;
        if (!idx.empty()) idx.push_back(0xFFFF);
        for (int k = 0; k < 4; ++k) { idx.push_back(uint16_t(v.size())); v.push_back(q[k]); }
    }
    std::vector<Sprite> out; ExtractStats st;
    extract_sprites(m, v.data(), v.size(), idx.data(), idx.size(), class_sprite, out, st);
    CHECK(st.no_depth == 0 && st.degenerate == 0 && st.bad_index == 0 && st.behind == 0);
    return out;
}
ScreenMap map_for(const Cam& c) {
    double a[16]; make_a(c, a); ScreenMap m; const char* why = nullptr;
    CHECK(screen_map_from_a(a, m, why) && m.ok);
    return m;
}
// Centroid (alpha weighted) of the rendered image; returns the covered pixel count.
int centroid(const std::vector<float>& img, int size, double& cx, double& cy) {
    double sx = 0, sy = 0, sw = 0; int n = 0;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float a = img[(size_t(y) * size + x) * 4 + 3];
        if (a > 0) { sx += a * (x + 0.5); sy += a * (y + 0.5); sw += a; ++n; }
    }
    if (sw > 0) { cx = sx / sw; cy = sy / sw; }
    return n;
}

void test_map_and_unproject() {
    const Cam c{{-14359.7, 3491.8, 123218.1}, deg(-46.3), deg(0.2)};
    const ScreenMap m = map_for(c);
    CHECK(std::fabs(m.eye[0] - c.e[0]) < 1e-6 && std::fabs(m.eye[1] - c.e[1]) < 1e-6 && std::fabs(m.eye[2] - c.e[2]) < 1e-6);
    CHECK(std::fabs(m.z0 - kZ0) < 1e-3 && std::fabs(m.b - kDepthB) < 1e-9);
    // round trip over the window and depths 50 .. 80000 units, both depth encodings
    double worst = 0;
    for (int iz = 0; iz < 6; ++iz) for (int ix = -2; ix <= 2; ++ix) for (int iy = -2; iy <= 2; ++iy) {
        const double d = 50.0 * std::pow(4.3, iz);
        const double xs = kGsCentre + ix * 120.0, ys = kGsCentre + iy * 100.0;
        double p[3]; CHECK(unproject(m, xs, ys, d, p));
        double x2, y2, d2; CHECK(project(m, p, x2, y2, d2));
        worst = std::max(worst, std::fabs(x2 - xs) + std::fabs(y2 - ys) + std::fabs(d2 - d) / d);
        DepthSource s;
        if (d < 60000.0) {   // the GS z reaches 0 at 2^24/255 = 65 793 units (the far plane): farther points only carry q
            const double dz = vertex_depth(m, float(game_z_for_depth(d)), 1.0f, s);
            CHECK(s == depth_z && std::fabs(dz - d) / d < (d > 10000 ? 2e-3 : 1e-4));   // float32 z: ~0.1% at 17 000 units
        } else CHECK(game_z_for_depth(d) <= 0.0 && vertex_depth(m, float(game_z_for_depth(d)), 1.0f, s) == 0 && s == depth_none);
        const double dq = vertex_depth(m, 0.0f, float(1.0 / d), s);
        CHECK(s == depth_q && std::fabs(dq - d) / d < 1e-6);
    }
    CHECK(worst < 1e-6);
    DepthSource s; CHECK(vertex_depth(m, 0.0f, 1.0f, s) == 0 && s == depth_none);   // a flat 2D sprite (z 0, q 1): no depth -> never remembered
    // malformed A is refused
    double a[16]; make_a(c, a); a[2] += 1.0; ScreenMap bad; const char* why = nullptr;
    CHECK(!screen_map_from_a(a, bad, why) && why && !bad.ok);
    make_a(c, a); a[14] += 1e6; CHECK(!screen_map_from_a(a, bad, why));
    std::printf("  map/unproject: round trip worst %.2e\n", worst);
}

void test_extract() {
    const Cam c{{1000, 4000, 2000}, deg(30), deg(-5)};
    const ScreenMap m = map_for(c);
    double f[3], r[3], u[3]; axes(c.yaw, c.pitch, f, r, u);
    std::vector<std::vector<double>> pts;
    for (int i = 0; i < 5; ++i) { std::vector<double> p(3); for (int k = 0; k < 3; ++k) p[k] = c.e[k] + f[k] * (300.0 + 900.0 * i) + r[k] * (40.0 * i - 80) + u[k] * 30.0; pts.push_back(p); }
    for (bool cloud : {false, true}) {
        const auto sp = observe(m, c, pts, 25.0, cloud);
        CHECK(sp.size() == pts.size());
        for (size_t i = 0; i < sp.size(); ++i) {
            const double e = std::sqrt(std::pow(sp[i].centre[0] - pts[i][0], 2) + std::pow(sp[i].centre[1] - pts[i][1], 2) + std::pow(sp[i].centre[2] - pts[i][2], 2));
            CHECK(e < 0.05 * (1 + i));   // float32 screen coordinates
            CHECK(std::fabs(sp[i].half_w - 25.0) < 0.05 && std::fabs(sp[i].half_h - 25.0) < 0.05);
            CHECK(sp[i].depth_src == (cloud ? depth_q : depth_z));
            CHECK(sp[i].uv0[0] == 0 && sp[i].uv1[0] == 1);
        }
    }
    // non-indexed strip, a 3-vertex strip is degenerate, out-of-range index
    Vtx q[4]; CHECK(game_sprite(m, c, pts[0].data(), 10, false, 1, q));
    std::vector<Sprite> out; ExtractStats st;
    CHECK(extract_sprites(m, q, 4, nullptr, 0, class_sprite, out, st) == 1);
    const uint16_t three[] = {0, 1, 2}; CHECK(extract_sprites(m, q, 4, three, 3, class_sprite, out, st) == 0 && st.degenerate == 1);
    const uint16_t oob[] = {0, 1, 2, 9}; CHECK(extract_sprites(m, q, 4, oob, 4, class_sprite, out, st) == 0 && st.bad_index == 1);
}

// The required registration case: observed inside the window, the game turns away, the head looks 40 deg away from the game window.
void test_registration_40deg() {
    const Cam g1{{0, 3000, 0}, 0.0, 0.0};
    const ScreenMap m1 = map_for(g1);
    double f[3], r[3], u[3]; axes(g1.yaw, g1.pitch, f, r, u);
    std::vector<double> P(3); for (int k = 0; k < 3; ++k) P[k] = g1.e[k] + f[k] * 1000.0 + r[k] * 300.0;   // 16.7 deg right of the game axis
    Memory mem;
    auto s1 = observe(m1, g1, {P}, 30.0);
    CHECK(s1.size() == 1);
    auto u1 = mem.update(0.0, s1, m1);
    CHECK(u1.born == 1 && mem.particles().size() == 1);
    // frame 2 (0.5 s later): the game camera turned 60 deg left; the particle (now 76.7 deg right of the game axis) is outside the window: not drawn.
    const Cam g2{{0, 3000, 0}, deg(60), 0.0};
    const ScreenMap m2 = map_for(g2);
    auto s2 = observe(m2, g2, {P}, 30.0);
    CHECK(s2.empty());
    auto u2 = mem.update(0.5, s2, m2);
    CHECK(u2.kept_outside == 1 && u2.died_in_view == 0 && mem.particles().size() == 1);
    // the head looks 40 deg right of the game window centre (yaw 60 - 40 = 20 deg); the window spans 40 +- 31.6 deg to the left of the head axis
    Eye e; for (int k = 0; k < 3; ++k) e.pos[k] = g2.e[k]; e.yaw = deg(20); e.tan_half = 1.0; e.size = 512;
    std::vector<float> img; render_reference(mem, 1.0, e, img);
    double cx = 0, cy = 0; const int n = centroid(img, e.size, cx, cy);
    double ex, ey; CHECK(eye_project(e, P.data(), ex, ey));
    const double win_right_px = (std::tan(deg(-40 + 31.6)) / e.tan_half + 1.0) * 0.5 * e.size;   // right edge of the game window in this eye
    // ground truth: the same billboard placed by hand at the true position and size, through the same renderer
    Memory gt; Sprite truth; for (int k = 0; k < 3; ++k) truth.centre[k] = P[k];
    truth.half_w = truth.half_h = 30.0; truth.colour[0] = 0.9f; truth.colour[1] = 0.6f; truth.colour[2] = 0.3f; truth.colour[3] = 1.0f;
    gt.update(0.0, {truth}, ScreenMap{});
    std::vector<float> ref; render_reference(gt, 1.0, e, ref);
    double rx = 0, ry = 0; const int rn = centroid(ref, e.size, rx, ry);
    double mx, my; CHECK(eye_project(e, mem.particles()[0].s.centre, mx, my));
    unsigned differ = 0; for (size_t i = 3; i < img.size(); i += 4) differ += std::fabs(img[i] - ref[i]) > 1e-3f;
    std::printf("  40 deg: remembered centre at (%.3f, %.3f) px, true (%.3f, %.3f) px; drawn centroid (%.2f, %.2f) vs truth (%.2f, %.2f), %d/%d px, %u differ;"
                " window right edge x=%.1f, alpha %.3f\n", mx, my, ex, ey, cx, cy, rx, ry, n, rn, differ, win_right_px, img[(size_t(ey) * e.size + size_t(ex)) * 4 + 3]);
    CHECK(std::fabs(mx - ex) < 0.05 && std::fabs(my - ey) < 0.05);              // registration of the remembered position: < 0.05 px at 512 px / 90 deg
    CHECK(n > 50 && std::fabs(cx - rx) < 0.1 && std::fabs(cy - ry) < 0.1 && differ <= 4);   // the drawn billboard = the true one (edge pixels at most)
    CHECK(ex > win_right_px + 50);                                     // outside the game's rectangle: only the memory can show it
    CHECK(std::fabs(img[(size_t(ey) * e.size + size_t(ex)) * 4 + 3] - 0.75f) < 1e-3);   // faded 1 s / 4 s
    // its angular size is right too: 30 units at 1044 units -> ~2*atan(30/1044) wide
    // later than decay_s after the last sighting: gone
    auto u3 = mem.update(4.0, {}, m2);
    CHECK(u3.expired == 1 && mem.particles().empty());
}

// A remembered particle the game COULD see but did not draw died in the game: dropped at once (no ghost inside the window).
void test_died_in_view() {
    const Cam g{{0, 3000, 0}, deg(10), deg(3)};
    const ScreenMap m = map_for(g);
    double f[3], r[3], u[3]; axes(g.yaw, g.pitch, f, r, u);
    std::vector<double> P(3), Q(3); for (int k = 0; k < 3; ++k) { P[k] = g.e[k] + f[k] * 800.0; Q[k] = g.e[k] + f[k] * 1200.0 - r[k] * 200.0; }
    Memory mem;
    mem.update(0.0, observe(m, g, {P, Q}, 20.0), m);
    CHECK(mem.particles().size() == 2);
    auto upd = mem.update(0.016, observe(m, g, {Q}, 20.0), m);   // P vanished inside the window
    CHECK(upd.died_in_view == 1 && upd.matched == 1 && mem.particles().size() == 1);
    // a particle moving 15 units per frame (tracer-like) stays one particle
    std::vector<double> Q2 = Q; for (int k = 0; k < 3; ++k) Q2[k] += f[k] * 15.0;
    auto v = mem.update(0.033, observe(m, g, {Q2}, 20.0), m);
    CHECK(v.matched == 1 && v.born == 0 && mem.particles().size() == 1 && mem.particles()[0].hits == 3);
}

// The aircraft flies through a trail it saw ahead: the trail is behind the camera afterwards and is drawn where it is for a head looking back.
void test_fly_through() {
    Memory mem;
    std::vector<std::vector<double>> trail;
    for (int i = 0; i < 8; ++i) trail.push_back({20.0 * i - 70.0, 3000.0 + 5.0 * i, -1500.0 - 150.0 * i});
    double t = 0;
    for (int step = 0; step < 20; ++step, t += 0.1) {   // camera moves -Z at 1000 u/s (100 units per step)
        const Cam g{{0, 3000, -100.0 * step}, 0.0, 0.0};
        const ScreenMap m = map_for(g);
        mem.update(t, observe(m, g, trail, 25.0), m);
    }
    CHECK(mem.particles().size() == 8);
    // now at z = -1900: the first three puffs are behind the camera. Head looks back (yaw 180 deg).
    Eye e; e.pos[0] = 0; e.pos[1] = 3000; e.pos[2] = -1900; e.yaw = kPi; e.size = 512;
    std::vector<float> img; render_reference(mem, t, e, img);
    int visible = 0;
    for (const auto& p : trail) {
        double ex, ey; if (!eye_project(e, p.data(), ex, ey) || ex < 0 || ey < 0 || ex >= e.size || ey >= e.size) continue;
        ++visible;
        CHECK(img[(size_t(ey) * e.size + size_t(ex)) * 4 + 3] > 0.3f);   // a remembered puff covers its true position
    }
    std::printf("  fly-through: %zu remembered, %d behind the aircraft drawn at their true positions\n", mem.particles().size(), visible);
    CHECK(visible >= 3);
}

void test_cap_and_fade() {
    Params p; p.cap = 4; p.decay_s = 2.0;
    Memory mem(p);
    const Cam g{{0, 3000, 0}, 0.0, 0.0};
    const ScreenMap m = map_for(g);
    std::vector<std::vector<double>> pts;
    for (int i = 0; i < 6; ++i) pts.push_back({-150.0 + 60.0 * i, 3000.0, -1000.0});
    mem.update(0, observe(m, g, pts, 10.0), m);
    CHECK(mem.particles().size() == 4);
    Particle q; q.last_seen = 1.0;
    CHECK(mem.fade(q, 0.5) == 1.0 && std::fabs(mem.fade(q, 2.0) - 0.5) < 1e-12 && mem.fade(q, 3.5) == 0.0);
}

// Optional: the stored frame 11906 (E10 export). Cloud cards (z 0, q) come out as pairs of a horizontal sheet and a vertical card of one size; the two
// tracer-like streaks (z form) as two equal-length segments on one line.
void test_real_frame() {
    const char* dir = std::getenv("ACZ_E10_BLOBS");
    if (!dir || !*dir) { std::printf("  real frame: skipped (ACZ_E10_BLOBS not set)\n"); return; }
    auto load = [&](const char* name) { std::ifstream f(std::string(dir) + "\\" + name, std::ios::binary); return std::vector<char>((std::istreambuf_iterator<char>(f)), {}); };
    const auto b297 = load("000334"), b329 = load("000335");
    if (b297.size() < 120000 || b329.size() < 967184 + 64) { std::printf("  real frame: skipped (blobs missing)\n"); return; }
    float a[16]; std::memcpy(a, b329.data() + 967184, sizeof a);
    ScreenMap m; const char* why = nullptr;
    CHECK(screen_map_from_a(a, m, why));
    CHECK(std::fabs(m.eye[0] + 14359.69) < 1.0 && std::fabs(m.eye[1] - 3491.78) < 1.0 && std::fabs(m.eye[2] - 123218.14) < 1.0);
    const Vtx* cv = reinterpret_cast<const Vtx*>(b297.data() + 65728); const uint16_t* ci = reinterpret_cast<const uint16_t*>(b297.data() + 68800);
    std::vector<Sprite> clouds; ExtractStats st;
    CHECK(extract_sprites(m, cv, 48, ci, 59, class_cloud, clouds, st) == 12 && st.no_depth == 0);
    unsigned horizontal = 0;
    for (int q = 0; q < 12; ++q) {
        double y[4];
        for (int k = 0; k < 4; ++k) { DepthSource s; const Vtx& v = cv[4 * q + k]; double p[3]; CHECK(unproject(m, v.pos[0], v.pos[1], vertex_depth(m, v.pos[2], v.pos[3], s), p)); y[k] = p[1]; CHECK(s == depth_q); }
        const double spread = std::max(std::max(y[0], y[1]), std::max(y[2], y[3])) - std::min(std::min(y[0], y[1]), std::min(y[2], y[3]));
        if (q % 2 == 1) { CHECK(spread < 20.0); ++horizontal; }   // the sheet: one altitude to +-10 units over 17 000 units
        CHECK(std::fabs(clouds[q].half_w - clouds[0].half_w) < 0.01 * clouds[0].half_w);   // all cards one width (22 940 units)
    }
    CHECK(horizontal == 6);
    const Vtx* sv = reinterpret_cast<const Vtx*>(b297.data() + 113984); const uint16_t* si = reinterpret_cast<const uint16_t*>(b297.data() + 114496);
    std::vector<Sprite> streak;
    CHECK(extract_sprites(m, sv, 8, si, 9, class_sprite, streak, st) == 2 && streak[0].depth_src == depth_z);
    CHECK(std::fabs(streak[0].half_h - streak[1].half_h) < 0.02 * streak[0].half_h);   // equal world lengths at 430..900 units
    std::printf("  real frame 11906: 12 cloud cards (6 level sheets), streak lengths %.1f / %.1f units at depths %.0f / %.0f\n", 2 * streak[0].half_h,
                2 * streak[1].half_h, streak[0].depth, streak[1].depth);
}
// The dump-shaders.txt diagnostic (shader_dump.h): trigger parsing, hash checks, write-once files with a budget.
void test_shader_dump() {
    using namespace acz::shaderdump;
    Trigger t; unsigned bad = 0;
    const char* txt = "# smoke\nd2464ed2, 4CAF6E2D;zz 123 d2464ed2\n";
    CHECK(parse_trigger(txt, std::strlen(txt), t, bad) == 2 && bad == 2 && t.has(0xd2464ed2u) && t.has(0x4caf6e2du) && !t.has(0x534f29cau));
    CHECK(parse_trigger("", 0, t, bad) == 0 && t.count == 0);
    CHECK(hex8("d2464ed248084cc90a44398e374553df") == 0xd2464ed2u && hex8("") == 0 && hex8(nullptr) == 0);
    CHECK(valid_hash("bcb04cf9326de0739e17a13b34f46d00") && !valid_hash("") && !valid_hash("..\\..\\x") && !valid_hash("BCB04CF9326DE0739E17A13B34F46D00"));
#ifdef _WIN32
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"acz-fxmem-dump-" + std::to_wstring(GetCurrentProcessId());
    const char a[] = "DXBC-gs-bytes", b[] = "DXBC-ps";
    Stage s[3] = {{a, sizeof a, "bcb04cf9326de0739e17a13b34f46d00"}, {b, sizeof b, "d2464ed248084cc90a44398e374553df"}, {nullptr, 0, ""}};
    unsigned budget = 1;
    CHECK(dump(dir, s, 3, budget) == 1 && budget == 0);          // the budget stops the second file
    budget = 8;
    CHECK(dump(dir, s, 3, budget) == 1 && budget == 7);          // the first file exists: never overwritten; the second is written now
    std::ifstream f(dir + L"\\bcb04cf9326de0739e17a13b34f46d00.dxbc", std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(f)), {}); f.close();
    CHECK(got == std::string(a, sizeof a));
    DeleteFileW((dir + L"\\bcb04cf9326de0739e17a13b34f46d00.dxbc").c_str()); DeleteFileW((dir + L"\\d2464ed248084cc90a44398e374553df.dxbc").c_str());
    RemoveDirectoryW(dir.c_str());
#endif
}
} // namespace

int main() {
    test_shader_dump();
    test_map_and_unproject();
    test_extract();
    test_registration_40deg();
    test_died_in_view();
    test_fly_through();
    test_cap_and_fade();
    test_real_frame();
    std::printf("fx_memory_offline: %u checks passed\n", g_checks);
    return 0;
}
