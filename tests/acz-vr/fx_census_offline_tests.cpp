// EFFECTS-OUTSIDE (D) offline tests: the fxmem:3 effects-draw census (src/acz-vr/fx_census.h) and the CPU core of the ps2_v sprite head-pose replay
// (src/acz-vr/fx_sprite_replay.h); docs/world-render/effects-outside-window.md section 9. Synthetic data only (no game data).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fx_census.h"
#include "fx_sprite_replay.h"
#include "fx_sprite_gpu.h"   // EFFECTS-OUTSIDE (F) fxmem:6
#include "gpu_copy_step5.h"
#include "effects_stage_flags.h"   // EFFECTS-OUTSIDE (P) fxadd stage switch

#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace acz;

static unsigned g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTanX = 0.6152, kTanY = 0.3460;
constexpr size_t kLineCap = 448 - 22;   // diagnostics::text_capacity minus "gpu-copy-step1;event=" and the terminator
double deg(double d) { return d * kPi / 180.0; }

// ------------------------------------------------------------------ census
fxcensus::DrawFacts facts(uint32_t ps, uint32_t func, uint32_t count, bool indexed = true) {
    fxcensus::DrawFacts d;
    d.ps8 = ps; d.vs8 = 0x534f29cau; d.pso_known = true; d.blend_on = true; d.depth_enable = true; d.depth_func = func; d.count = count; d.instances = 1;
    d.indexed = indexed; d.topology = 5; d.vb_stride = 64; d.write_mask = 15;
    const uint8_t b[6] = {5, 6, 1, 18, 19, 1}; std::memcpy(d.blend, b, 6);
    fxcensus::RootShape r; r.add(1, 0, 10, 0, 2); r.add(3, 4, 10, 0, 0); r.add(0, 5, 0, 0, 0);
    d.root_known = r.known; d.root_n = r.n; d.root_c = r.c; d.root_v = r.v; d.root_t = r.t; d.root_hash = r.hash;
    d.gate = "class-ps-not-allowlisted";
    return d;
}

void test_classify_and_key() {
    using namespace fxcensus;
    CHECK(classify(facts(kPsSmoke, kFuncGreater, 6, false)) == cls_smoke);
    CHECK(classify(facts(kPsSprite, kFuncGreaterEqual, 89)) == cls_cloud);
    CHECK(classify(facts(kPsSprite, kFuncGreater, 14)) == cls_sprite);
    CHECK(classify(facts(kPsExplosionCand, kFuncGreater, 9)) == cls_explosion_cand);
    CHECK(classify(facts(kPsOverlayCand, 8, 4, false)) == cls_overlay_cand);
    CHECK(classify(facts(kPsHudSprite, 8, 4)) == cls_hud_sprite);
    CHECK(classify(facts(0x12345678u, kFuncGreater, 4)) == cls_other);
    CHECK(std::strcmp(class_name(cls_explosion_cand), "explosion-cand") == 0 && std::strcmp(class_name(200), "?") == 0);
    // the key ignores counts and the gate verdict, but not the state
    auto a = facts(kPsExplosionCand, kFuncGreater, 9), b = facts(kPsExplosionCand, kFuncGreater, 900);
    b.gate = nullptr; b.instances = 7;
    CHECK(signature_key(a) == signature_key(b));
    b.blend[1] = 2; CHECK(signature_key(a) != signature_key(b));                     // additive (SRC_ALPHA / ONE) is another signature
    b = a; b.depth_func = kFuncGreaterEqual; CHECK(signature_key(a) != signature_key(b));
    b = a; b.root_hash ^= 1; CHECK(signature_key(a) != signature_key(b));
    b = a; b.topology = 4; CHECK(signature_key(a) != signature_key(b));
    // root shape counts
    RootShape r; r.add(1, 0, 10, 0, 2); r.add(2, 0, 0, 0, 0); r.add(3, 0, 0, 0, 0); r.add(4, 0, 0, 0, 0); r.add(0, 0, 0, 0, 0);
    CHECK(r.known && r.n == 5 && r.c == 1 && r.v == 3 && r.t == 1);
    RootShape r2; r2.add(1, 0, 11, 0, 2); r2.add(2, 0, 0, 0, 0); r2.add(3, 0, 0, 0, 0); r2.add(4, 0, 0, 0, 0); r2.add(0, 0, 0, 0, 0);
    CHECK(r2.hash != r.hash);
}

// 60 fps over ~2.5 s: the cloud card every frame, tracer-like sprites only while "firing" (frames 30..59, 2 draws), an explosion burst at frames 70..79,
// none for 31 frames, a second burst at 120..129; the smoke while frames 60..140; one window per second.
void test_census_timeline() {
    using namespace fxcensus;
    Census c;
    c.add(facts(kPsSprite, kFuncGreater, 4));   // no open frame: ignored
    CHECK(c.sig_count() == 0);
    std::vector<std::string> windows, onsets;
    uint64_t ms = 100000;
    char line[512];
    for (uint64_t f = 1; f <= 150; ++f) {
        c.begin_frame();
        c.add(facts(kPsSprite, kFuncGreaterEqual, 89));
        if (f >= 30 && f < 60) { c.add(facts(kPsSprite, kFuncGreater, 14)); c.add(facts(kPsSprite, kFuncGreater, 9 + uint32_t(f % 3) * 4)); }
        if ((f >= 70 && f < 80) || (f >= 120 && f <= 129)) c.add(facts(kPsExplosionCand, kFuncGreater, 24 + uint32_t(f - 70)));
        if (f >= 60 && f <= 140) { auto s = facts(kPsSmoke, kFuncGreater, 6, false); s.gate = nullptr; c.add(s); }
        const bool closed = c.end_frame(f, ms);
        for (unsigned i = 0; i < c.onset_count(); ++i) { format_onset(line, sizeof line, c, c.onset(i)); onsets.push_back(line); }
        c.clear_onsets();
        if (closed) {
            format_window(line, sizeof line, c.summary()); windows.push_back(line);
            for (unsigned i = 0; i < c.report_count(); ++i) { format_sig(line, sizeof line, f, c.report(i)); windows.push_back(line); }
        }
        ms += 16 + (f % 3 == 0);   // ~16.67 ms
    }
    // onsets: cloud new at 1, sprite(tracer-cand) new at 30, smoke new at 60, explosion new at 70 and again at 120 (gap 41 > 30)
    CHECK(onsets.size() == 5);
    CHECK(onsets[0].find("frame=1;class=cloud;gap=new:0;draws=1;cnt-sum=89") == 0 + std::strlen("fx-census-onset;"));
    CHECK(onsets[1].find("frame=30;class=sprite(tracer-cand);gap=new:0;draws=2;cnt-sum=") != std::string::npos);
    CHECK(onsets[2].find("frame=60;class=smoke;gap=new:0") != std::string::npos);
    CHECK(onsets[3].find("frame=70;class=explosion-cand;gap=new:0;draws=1;cnt-sum=24;sig=ps542a11d7/vs534f29ca;blend=on:5,6,1/18,19,1:mf;depth=f5:w0") != std::string::npos);
    CHECK(onsets[4].find("frame=120;class=explosion-cand;gap=41;") != std::string::npos);
    // windows: first closes at the frame where 1000 ms passed (~frame 60)
    CHECK(!windows.empty() && windows[0].find("fx-census-window;") == 0);
    bool sawTracerWindow = false, sawExplosion = false;
    for (const auto& w : windows) {
        CHECK(w.size() < kLineCap);
        if (w.find("class=sprite(tracer-cand)") != std::string::npos) {
            sawTracerWindow = true;
            CHECK(w.find("frames=30;draws=60;per-frame-max=2;cnt=9-17(sum:") != std::string::npos);
            CHECK(w.find("gate=class-ps-not-allowlisted") != std::string::npos);
        }
        if (w.find("class=explosion-cand") != std::string::npos) {
            sawExplosion = true;
            CHECK(w.find("roots=known:n3:c1:v1:t1:h") != std::string::npos && w.find(";topo=5;idx=1;vbs=64;") != std::string::npos);
        }
        if (w.find("class=smoke") != std::string::npos) CHECK(w.find("gate=candidate") != std::string::npos);
    }
    CHECK(sawTracerWindow && sawExplosion);
    // the explosion: 20 frames, 20 draws in the whole run
    for (unsigned i = 0; i < c.sig_count(); ++i) if (c.sig(i).cls == cls_explosion_cand) CHECK(c.sig(i).t_frames == 20 && c.sig(i).t_draws == 20 && c.sig(i).first_frame == 70);
    // exactly 30 frames of gap is NOT a new onset
    Census g; uint64_t t = 0;
    for (uint64_t f = 1; f <= 40; ++f) { g.begin_frame(); if (f == 1 || f == 31) g.add(facts(kPsExplosionCand, kFuncGreater, 4)); g.end_frame(f, t += 16); }
    CHECK(g.onset_count() == 1);
    std::printf("  census timeline: %zu window lines, %zu onsets\n", windows.size(), onsets.size());
}

void test_census_caps_and_lengths() {
    using namespace fxcensus;
    Census c; uint64_t ms = 1;
    c.begin_frame();
    for (uint32_t i = 0; i < 60; ++i) for (uint32_t k = 0; k <= i % 5; ++k) c.add(facts(0x10000000u + i, kFuncGreater, 4));
    CHECK(c.sig_count() == Census::kSigCap && c.overflow() > 0);
    CHECK(!c.end_frame(1, ms));
    CHECK(c.onset_count() == Census::kOnsetCap);   // 48 fresh signatures, 16 onsets kept, the rest dropped (counted)
    c.begin_frame(); c.add(facts(0x10000004u, kFuncGreater, 4));
    CHECK(c.end_frame(2, ms + 1000));
    CHECK(c.report_count() == Census::kLinesPerWindow && c.summary().active == Census::kSigCap && c.summary().onsets_dropped == Census::kSigCap - Census::kOnsetCap);
    for (unsigned i = 1; i < c.report_count(); ++i) CHECK(c.report(i - 1).w_draws >= c.report(i).w_draws);
    CHECK(c.report(0).w_draws == 6);   // 0x10000004: 5 draws in frame 1 + 1 in frame 2
    // line lengths: realistic maxima (frame ids 1e9, counts 1e7, sums 1e12, D3D12 enum ranges, the longest gate reason) fit the 448-byte diagnostics text
    // with the event prefix; arbitrary values are cut by snprintf (never an overrun).
    Sig s{}; s.cls = cls_explosion_cand; s.first = facts(0xffffffffu, 8, 9999999);
    std::memset(s.first.blend, 19, 6); s.first.write_mask = 15; s.first.topology = 33; s.first.vb_stride = 256; s.first.instances = 9999999; s.first.stencil = true;
    s.first.root_known = false; s.first.root_n = s.first.root_c = s.first.root_v = s.first.root_t = 64; s.first.root_hash = 0xffffffffu;
    s.w_frames = s.w_draws = s.w_frame_max = s.w_cnt_min = s.w_cnt_max = s.w_inst_max = 9999999u; s.w_cnt_sum = 999999999999ull;
    s.t_frames = s.t_draws = s.first_frame = 999999999ull;
    s.w_gate = "game-predication-not-provably-off";
    char line[1024];
    format_sig(line, sizeof line, 999999999ull, s); CHECK(std::strlen(line) < kLineCap);
    CHECK(std::string(line).find("gate=game-predication-not-provably-o;") != std::string::npos);   // reasons cut at 31 characters
    WindowSummary w; w.frame = w.ms = 999999999ull; w.overflow = w.onsets_dropped = 999999999ull; w.frames = w.draws = w.sigs = w.active = w.onsets = 9999999u;
    format_window(line, sizeof line, w); CHECK(std::strlen(line) < kLineCap);
    Census one; one.begin_frame(); s.first.count = 9999999; one.add(s.first); one.end_frame(999999999ull, 5);
    format_onset(line, sizeof line, one, one.onset(0)); CHECK(std::strlen(line) < kLineCap);
    s.w_cnt_sum = ~0ull; s.t_frames = s.t_draws = s.first_frame = ~0ull;
    char cap[448]; format_sig(cap, sizeof cap, ~0ull, s); CHECK(std::strlen(cap) < sizeof cap);
    char tiny[8]; format_sig(tiny, sizeof tiny, 1, s); CHECK(std::strlen(tiny) < 8);   // truncation never overruns
}

// ------------------------------------------------------------------ sprite re-projection (fx_sprite_replay.h)
struct Cam { double e[3]; double yaw, pitch; };
void axes(double yaw, double pitch, double f[3], double r[3], double u[3]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    f[0] = -sy * cp; f[1] = sp; f[2] = -cy * cp; r[0] = cy; r[1] = 0; r[2] = -sy;
    u[0] = r[1] * f[2] - r[2] * f[1]; u[1] = r[2] * f[0] - r[0] * f[2]; u[2] = r[0] * f[1] - r[1] * f[0];
}
// Same A form as fx_memory_offline (the stored frame's form). tan_scale widens the lens (an HMD eye maps a wider frustum onto the same GS window).
void make_a(const Cam& c, double a[16], double tan_scale = 1.0) {
    double f[3], r[3], u[3]; axes(c.yaw, c.pitch, f, r, u);
    auto lin = [&](const double v[3], double s, double col[4]) { for (int i = 0; i < 3; ++i) col[i] = s * v[i]; col[3] = -s * (v[0] * c.e[0] + v[1] * c.e[1] + v[2] * c.e[2]); };
    double w[4], cx[4], cy[4];
    lin(f, 1.0, w); lin(r, 1.0 / (1.008 * kTanX * tan_scale), cx); lin(u, -1.0 / (1.008 * kTanY * tan_scale), cy);
    for (int k = 0; k < 4; ++k) {
        a[k * 4 + 0] = fxmem::kGsScaleX * cx[k] + fxmem::kGsCentre * w[k];
        a[k * 4 + 1] = fxmem::kGsScaleY * cy[k] + fxmem::kGsCentre * w[k];
        a[k * 4 + 2] = fxmem::kDepthB * w[k] + (k == 3 ? fxmem::kZ0 : 0.0);
        a[k * 4 + 3] = w[k];
    }
}
fxmem::ScreenMap map_for(const Cam& c, double tan_scale = 1.0) {
    double a[16]; make_a(c, a, tan_scale); fxmem::ScreenMap m; const char* why = nullptr;
    CHECK(fxmem::screen_map_from_a(a, m, why) && m.ok);
    return m;
}
// The game's quad for world point p, half size R, facing the game camera; corners returned in world units too.
void game_quad(const fxmem::ScreenMap& m, const Cam& c, const double p[3], double R, bool q_form, fxmem::Vtx out[4], double corners[4][3]) {
    double f[3], r[3], u[3]; axes(c.yaw, c.pitch, f, r, u);
    const double sx[4] = {-1, 1, -1, 1}, sy[4] = {1, 1, -1, -1};
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < 3; ++i) corners[k][i] = p[i] + R * (sx[k] * r[i] + sy[k] * u[i]);
        double xs, ys, d; CHECK(fxmem::project(m, corners[k], xs, ys, d));
        out[k] = fxmem::Vtx{};
        out[k].pos[0] = float(xs); out[k].pos[1] = float(ys);
        out[k].pos[2] = q_form ? 0.0f : float(fxmem::game_z_for_depth(d)); out[k].pos[3] = q_form ? float(1.0 / d) : 1.0f;
        out[k].col[0] = 1.0f; out[k].col[1] = 0.5f; out[k].col[2] = 0.1f; out[k].col[3] = 0.8f; out[k].uv[0] = float(k & 1); out[k].uv[1] = float(k >> 1);
    }
}

void test_reproject_identity_and_eye() {
    const Cam g{{-14359.7, 3491.8, 123218.1}, deg(-46.3), deg(0.2)};
    const fxmem::ScreenMap gm = map_for(g);
    double f[3], r[3], u[3]; axes(g.yaw, g.pitch, f, r, u);
    // an explosion 900 units ahead, 10 deg right, half size 40; a second quad in q form far away (cloud-like) for the q rule
    double p1[3], p2[3];
    for (int i = 0; i < 3; ++i) { p1[i] = g.e[i] + 900 * f[i] + 160 * r[i] + 20 * u[i]; p2[i] = g.e[i] + 30000 * f[i] - 4000 * r[i] + 3000 * u[i]; }
    fxmem::Vtx in[8]; double cw[8][3];
    game_quad(gm, g, p1, 40, false, in, cw); game_quad(gm, g, p2, 900, true, in + 4, cw + 4);
    const uint16_t idx[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
    fxmem::Vtx out[8]; uint8_t bad[8]; fxsprite::ReprojectStats st;
    // (1) the eye IS the game camera: positions unchanged within float rounding, colour/uv bytes identical
    CHECK(fxsprite::reproject(gm, gm, in, 8, out, 1.0, 1.0, st, bad) == nullptr);
    CHECK(st.vertices == 8 && st.z_form == 4 && st.q_form == 4 && st.behind_eye == 0);
    double worst = 0;
    for (int k = 0; k < 8; ++k) {
        worst = std::fmax(worst, std::fabs(out[k].pos[0] - in[k].pos[0]) + std::fabs(out[k].pos[1] - in[k].pos[1]));
        CHECK(std::fabs(out[k].pos[2] - in[k].pos[2]) <= 1e-3 * std::fmax(1.0, std::fabs(in[k].pos[2])));
        CHECK(std::fabs(out[k].pos[3] - in[k].pos[3]) <= 1e-5 * std::fabs(in[k].pos[3]));
        CHECK(std::memcmp(out[k].col, in[k].col, sizeof in[k].col) == 0 && std::memcmp(out[k].uv, in[k].uv, sizeof in[k].uv) == 0);
    }
    CHECK(worst < 2e-3);
    // (2) a head 25 deg left and 6 deg up, 0.032 units right (half IPD), wider HMD lens: every vertex lands on the eye's projection of its world corner
    const Cam e{{g.e[0] + 0.032 * r[0], g.e[1] + 0.032 * r[1], g.e[2] + 0.032 * r[2]}, g.yaw + deg(25), g.pitch + deg(6)};
    const fxmem::ScreenMap em = map_for(e, 1.9);
    const double scale = 0.0009765625;   // the objects' depth fix scale
    CHECK(fxsprite::reproject(gm, em, in, 8, out, scale, 1.0, st, bad) == nullptr && st.behind_eye == 0);
    double wpx = 0, wz = 0;
    for (int k = 0; k < 8; ++k) {
        double xs, ys, d; CHECK(fxmem::project(em, cw[k], xs, ys, d));
        wpx = std::fmax(wpx, std::fabs(out[k].pos[0] - xs) + std::fabs(out[k].pos[1] - ys));
        if (k < 4) {
            const double z = scale * fxmem::game_z_for_depth(d);
            wz = std::fmax(wz, std::fabs(out[k].pos[2] - z) / z);
            CHECK(std::fabs(out[k].pos[3] - in[k].pos[3] * ((fxmem::kZ0 / (in[k].pos[2] - fxmem::kDepthB)) / d)) < 1e-4);   // q' = q d / d'
        } else {
            CHECK(out[k].pos[2] == 0.0f && std::fabs(out[k].pos[3] * d - 1.0) < 1e-4);
        }
    }
    CHECK(wpx < 0.05 && wz < 2e-3);   // float32 z at 900 units: ~1e-4 relative depth; xy within 0.05 GS px
    // the head turned left: the explosion (10 deg right of the game axis) is now ~35 deg right of the eye axis: still inside the wider eye frustum
    CHECK(fxmem::in_window(out[0].pos[0], out[0].pos[1]) && out[0].pos[0] > in[0].pos[0]);
    std::printf("  reproject: identity worst %.2e GS px, eye worst %.2e GS px, z %.2e\n", worst, wpx, wz);
    // (3) the head turned 180 deg: everything behind the eye, every quad collapsed to a point
    const Cam back{{g.e[0], g.e[1], g.e[2]}, g.yaw + deg(180), g.pitch};
    CHECK(fxsprite::reproject(gm, map_for(back), in, 8, out, 1.0, 1.0, st, bad) == nullptr && st.behind_eye == 8);
    fxsprite::collapse_quads(out, 8, bad, idx, 9, st);
    CHECK(st.quads == 2 && st.quads_collapsed == 2 && st.bad_index == 0);
    for (int k = 1; k < 4; ++k) CHECK(std::memcmp(out[k].pos, out[0].pos, sizeof out[0].pos) == 0);
    // (4) one corner behind a near plane: only that quad collapses (non-indexed list of strips)
    const Cam side{{p1[0] - 41 * r[0] + 10 * f[0], p1[1] - 41 * r[1] + 10 * f[1], p1[2] - 41 * r[2] + 10 * f[2]}, g.yaw, g.pitch};   // just behind-left corners
    CHECK(fxsprite::reproject(gm, map_for(side), in, 8, out, 1.0, 1.0, st, bad) == nullptr);
    fxsprite::collapse_quads(out, 8, bad, nullptr, 0, st);
    CHECK(st.behind_eye > 0 && st.quads == 2 && st.quads_collapsed == 1);
    // (5) refusals: a flat 2D overlay vertex (z 0, q 1) refuses the draw; invalid maps; bad index strips are counted
    fxmem::Vtx flat[4]; std::memcpy(flat, in, sizeof flat); flat[2].pos[2] = 0.0f; flat[2].pos[3] = 1.0f;
    CHECK(std::strcmp(fxsprite::reproject(gm, gm, flat, 4, out, 1.0, 1.0, st, bad), "sprite-no-depth") == 0 && st.no_depth == 1);
    fxmem::ScreenMap none;
    CHECK(std::strcmp(fxsprite::reproject(none, gm, in, 4, out, 1.0, 1.0, st, bad), "sprite-game-map-invalid") == 0);
    CHECK(std::strcmp(fxsprite::reproject(gm, none, in, 4, out, 1.0, 1.0, st, bad), "sprite-eye-map-invalid") == 0);
    CHECK(std::strcmp(fxsprite::reproject(gm, gm, in, 4, out, 0.0, 1.0, st, bad), "sprite-depth-scale") == 0);
    CHECK(std::strcmp(fxsprite::reproject(gm, gm, in, 0, out, 1.0, 1.0, st, bad), "sprite-no-vertices") == 0);
    const uint16_t badIdx[7] = {0, 1, 2, 0xFFFF, 4, 5, 9};
    fxsprite::ReprojectStats s2; uint8_t allBad[8]; std::memset(allBad, 1, sizeof allBad);
    fxsprite::collapse_quads(out, 8, allBad, badIdx, 7, s2);
    CHECK(s2.bad_index == 2 && s2.quads == 0);
}
} // namespace

namespace {
// The window lines list effect classes before HUD sprites (14 hud-sprite signatures with many draws no longer hide a 1-draw tracer candidate).
void test_census_priority() {
    using namespace fxcensus;
    Census c; c.begin_frame();
    for (uint32_t i = 0; i < 14; ++i) { auto h = facts(kPsHudSprite, 8, 4); h.topology = 100 + i; for (int k = 0; k < 20; ++k) c.add(h); }
    c.add(facts(kPsSprite, kFuncGreater, 9));
    c.end_frame(1, 10); c.begin_frame(); c.end_frame(2, 1100);
    CHECK(c.report_count() == Census::kLinesPerWindow && c.report(0).cls == cls_sprite && c.report(1).cls == cls_hud_sprite);
}
// ------------------------------------------------------------------ HUD-only / chase depth model (headset test 7, docs effects-outside-window.md section 22.3)
// The logged A of the game's absolute world camera, verbatim (D:\acz-evidence-archive\pov1\runPOV3\logs\ee-camera.log, `ui3d-census;kind=acm-a`):
// the cockpit view has b = -255.01 / Z0 = 2^24, the chase and the HUD-only view b = -1,474.5 / -1,341.4 and Z0 = 9.67e7 / 8.80e7. The old check
// (Z0 within 1e-3 of 2^24) refused them: `sprite-world-a-form` for the whole view (nothing replayed per eye, no puff host).
void test_hudonly_depth_model() {
    struct Logged { const char* what; double a[16]; double b, z0; };
    const Logged L[] = {
        {"cockpit all=17 (204002287.126)", {-1151.54333,-1448.15491,180.317764,-0.707106888,3.58666803e-05,-652.512085,-4.46595823e-06,1.75130275e-08,-1744.76648,-1448.15491,180.317764,-0.707106888,409323392,422072224,-34151824,199716.625}, -255.01, 16777472},
        {"cockpit all=4 (204002287.102)", {-1357.64526,-1448.15491,180.317764,-0.707106888,3.58666803e-05,-127.999977,-4.46595823e-06,1.75130275e-08,-1538.66455,-1448.15491,180.317764,-0.707106888,100844.141,103620.5,16764887,49.3459473}, -255.01, 16777471},
        {"HUD-only all=19 (204084889.040)", {-1036.37085,-1319.10754,884.882751,-0.659652293,-147.345123,-798.73999,96.8959885,-0.072232917,-1809.55432,-1497.34668,1003.51904,-0.748091936,219116288,229474528,-57887428,108737.133}, -1341.44, 87976709},
        {"HUD-only all=19 (204092553.889)", {-1039.87085,-1324.89819,886.885437,-0.66114527,-134.342575,-785.993286,88.3585892,-0.0658685565,-1808.55774,-1498.97803,1002.53827,-0.747360826,201302464,212146688,-46173808,100004.984}, -1341.4, 87976000},
    };
    size_t accepted = 0;
    for (const Logged& l : L) {
        fxmem::ScreenMap g; const char* why = nullptr;
        CHECK(fxsprite::game_map_from_a(l.a, g) && g.ok);
        fxmem::ScreenMap twin; CHECK(fxmem::screen_map_from_a(l.a, twin, why) && twin.ok);   // the fx_memory.cpp twin agrees
        CHECK(std::fabs(g.b - l.b) < 0.2);
        CHECK(std::fabs(g.z0 - l.z0) < 1000.0);
        if (l.b < -1000) CHECK(std::fabs(g.z0 - fxmem::kZ0) > 1e-3 * fxmem::kZ0);   // the old Z0 rule refused this one
        // a z-form vertex of a world point 2,500 units ahead: the depth round-trips with the map's OWN model (the cockpit constants give a wrong depth)
        const double d = 2500.0; const float z = float(g.z0 / d + g.b);
        fxmem::DepthSource src; const double back = fxsprite::sp_vertex_depth(g, z, 1.0f, src);
        CHECK(src == fxmem::depth_z && std::fabs(back - d) < 1e-3 * d);
        if (l.b < -1000) { const double wrong = fxmem::kZ0 / (double(z) - fxmem::kDepthB); CHECK(std::fabs(wrong - d) > 0.1 * d); }
        // eye map: shares the model; reproject a z-form quad to an eye on the game camera: the eye z equals the game z
        fxmem::ScreenMap e; CHECK(fxsprite::eye_map_from_a(l.a, e, &g) && e.ok && e.b == g.b && e.z0 == g.z0);
        fxmem::Vtx in[4]{}, out[4]{}; uint8_t bad[4]; fxsprite::ReprojectStats st;
        for (int k = 0; k < 4; ++k) {
            double p[3]; CHECK(fxsprite::sp_unproject(g, 2000.0 + 20.0 * (k & 1), 2040.0 + 16.0 * (k >> 1), d, p));
            double xs, ys, dd; CHECK(fxsprite::sp_project(g, p, xs, ys, dd) && std::fabs(dd - d) < 1e-6 * d);
            in[k].pos[0] = float(xs); in[k].pos[1] = float(ys); in[k].pos[2] = z; in[k].pos[3] = 1.0f; in[k].col[3] = 0.8f;
        }
        CHECK(fxsprite::reproject(g, e, in, 4, out, 1.0, 1.0, st, bad) == nullptr && st.z_form == 4 && st.behind_eye == 0);
        for (int k = 0; k < 4; ++k) CHECK(std::fabs(double(out[k].pos[2]) - double(in[k].pos[2])) <= 1e-4 * std::fabs(double(in[k].pos[2])));
        ++accepted;
    }
    // a Z0 / b pair outside the PS2 form is still refused (a non-world matrix)
    double bad[16]; std::memcpy(bad, L[2].a, sizeof bad); bad[14] += 5.0e7; fxmem::ScreenMap m; CHECK(!fxsprite::game_map_from_a(bad, m));
    CHECK(!fxmem::depth_model_ok(-255.0, 1.0e6) && !fxmem::depth_model_ok(255.0, 16777216.0) && fxmem::depth_model_ok(-1474.51, 96697272.0));
    std::printf("  hud-only depth model: %zu logged world A accepted with their own b / Z0\n", accepted);
}
// ------------------------------------------------------------------ fxmem:5 (section 10)
// The header-only maths of fx_sprite_replay.h equal fx_memory.cpp's.
void test_header_maths_match_fx_memory() {
    const Cam c{{-14359.7, 3491.8, 123218.1}, deg(-46.3), deg(0.2)};
    double a[16]; make_a(c, a);
    fxmem::ScreenMap ref; const char* why = nullptr; CHECK(fxmem::screen_map_from_a(a, ref, why));
    fxmem::ScreenMap m; CHECK(fxsprite::game_map_from_a(a, m) && m.ok && std::fabs(m.b - ref.b) < 1e-12 && std::fabs(m.z0 - ref.z0) < 1e-6);
    double bad[16]; std::memcpy(bad, a, sizeof bad); bad[2] += 1.0; CHECK(!fxsprite::game_map_from_a(bad, m));
    CHECK(fxsprite::game_map_from_a(a, m));
    for (int i = 0; i < 40; ++i) {
        const double xs = 1850 + 10.0 * i, ys = 1880 + 8.0 * i, d = 30.0 * (i + 1) * (i + 1);
        double p1[3], p2[3]; CHECK(fxmem::unproject(ref, xs, ys, d, p1) && fxsprite::sp_unproject(m, xs, ys, d, p2));
        CHECK(std::fabs(p1[0] - p2[0]) + std::fabs(p1[1] - p2[1]) + std::fabs(p1[2] - p2[2]) < 1e-6);
        double x1, y1, d1, x2, y2, d2; CHECK(fxmem::project(ref, p1, x1, y1, d1) && fxsprite::sp_project(m, p1, x2, y2, d2));
        CHECK(x1 == x2 && y1 == y2 && d1 == d2);
        const float z = float(fxmem::game_z_for_depth(d)); fxmem::DepthSource s1, s2;
        CHECK(fxmem::vertex_depth(ref, z, 1.0f, s1) == fxsprite::sp_vertex_depth(m, z, 1.0f, s2) && s1 == s2);
    }
    // eye maps only need finite values and a w column
    fxmem::ScreenMap e; CHECK(fxsprite::eye_map_from_a(a, e) && e.ok);
    double nan[16]; std::memcpy(nan, a, sizeof nan); nan[5] = std::nan(""); CHECK(!fxsprite::eye_map_from_a(nan, e));
    double zw[16]; std::memcpy(zw, a, sizeof zw); zw[3] = zw[7] = zw[11] = 0; CHECK(!fxsprite::eye_map_from_a(zw, e));
}

// The explosion blend (D-370: 5,5,1/18,19,1) and extra7_p's outputs (DXIL: o0 = (t.rgb a, 1+a), o1.a = a/(1+a)): the game's result over a background, the
// coverage clone's effects pixel (alpha ZERO/ONE) and the consumer composite e + (1-A) world. The glow and the effects underneath are exact; the only loss is
// the a x world brightening (named in the docs).
void test_explosion_blend_model() {
    uint32_t sa = 0, da = 0;
    CHECK(gpu_copy::step5::effects_clone_alpha_factors(5, 6, 1, 18, 19, 1, 15, sa, da) && sa == 2 && da == 6);        // the standard dual tuple (unchanged)
    CHECK(gpu_copy::step5::effects_clone_alpha_factors(5, 5, 1, 18, 19, 1, 15, sa, da) && sa == 1 && da == 2);        // the explosion: ZERO / ONE
    CHECK(!gpu_copy::step5::effects_clone_alpha_factors(5, 5, 1, 18, 19, 1, 7, sa, da));                              // other write mask
    // (P) the additive 4caf6e2d tuple now gets its clone (ZERO / ONE); the class gate still refuses it unless fxadd is on (p_points::test_additive_gate)
    CHECK(gpu_copy::step5::effects_clone_alpha_factors(5, 2, 1, 18, 19, 1, 15, sa, da) && sa == 1 && da == 2);
    CHECK(!gpu_copy::step5::effects_clone_alpha_factors(5, 2, 1, 18, 19, 1, 7, sa, da));                              // other write mask: no clone
    CHECK(!gpu_copy::step5::effects_clone_alpha_factors(2, 6, 1, 2, 6, 1, 15, sa, da));
    CHECK(gpu_copy::step5::effects_explosion_blend_tuple(5, 5, 1, 18, 19, 1, 15) && !gpu_copy::step5::effects_dual_blend_tuple(5, 5, 1, 18, 19, 1, 15));
    double worst = 0;
    for (int k = 1; k <= 255; k += 7) {
        const double a = k / 255.0, t = 0.8, world = 0.35, under = 0.2, underA = 0.4;   // texel*colour, background world, an effect already in the image
        const double o0 = t * a, o0a = 1 + a, o1a = a / (1 + a);
        // the game, over (world + the effect under it): RGB' = SRC_ALPHA*o0 + SRC_ALPHA*D
        const double gameD = under + (1 - underA) * world, game = o0a * o0 + o0a * gameD;
        // the clone into the effects image (RGB factors unchanged, alpha ZERO/ONE), then the consumer composite
        const double e = o0a * o0 + o0a * under, A = 0 * o0a + 1 * underA, comp = e + (1 - A) * world;
        CHECK(A == underA);                                                         // light adds no coverage
        CHECK(std::fabs((game - comp) - a * (1 - underA) * world) < 1e-12);        // the only difference: the a x world brightening
        worst = std::fmax(worst, std::fabs(game - comp));
        (void)o1a;
    }
    std::printf("  explosion blend: largest loss (a x world) %.3f at world 0.35\n", worst);
}

// The class gate admits PS 542a11d7 only with fxmem:5 AND the dual path; nothing else changes.
// EFFECTS-OUTSIDE (I) (docs section 14): the per-eye explosion's own "light" PSO and the display-space sprite composite against the GAME's blend in display
// space. (1) the light factors exist only for the explosion tuple; (2) PSO choice and refusals; (3) one explosion over a background: exact (game display
// value) with the light PSO + display composite for every a and background, while the shared clone + linear composite (the previous path) loses the
// (1+a) x background term and is paler; (4) two overlapping explosions and a coverage smoke puff under one: still exact (T multiplies); (5) the colour
// target: full-alpha core WHITE, partial-alpha rim orange (r > g > b, saturated), the previous path tan; (6) the coverage floor and fade.
void test_explosion_light() {
    namespace g = fxsprite::gpu; namespace sg = acz::fxsmoke::gpu; namespace s5 = gpu_copy::step5;
    uint32_t sa = 0, da = 0, oa = 0;
    CHECK(s5::effects_explosion_light_factors(5, 5, 1, 18, 19, 1, 15, sa, da, oa) && sa == 18 && da == 5 && oa == 3);   // SRC1_ALPHA / SRC_ALPHA / REV_SUBTRACT
    CHECK(!s5::effects_explosion_light_factors(5, 6, 1, 18, 19, 1, 15, sa, da, oa));   // the standard dual tuple: no light PSO
    CHECK(!s5::effects_explosion_light_factors(5, 2, 1, 1, 2, 1, 15, sa, da, oa));     // the tracer tuple: no light PSO
    CHECK(!s5::effects_explosion_light_factors(5, 5, 1, 18, 19, 1, 7, sa, da, oa));    // another write mask
    CHECK(s5::effects_clone_alpha_factors(5, 5, 1, 18, 19, 1, 15, sa, da) && sa == 1 && da == 2);   // the SHARED clone is unchanged (window image)
    // (2) the choice
    CHECK(g::explosion_pso_why(true, true, 10) == nullptr && g::explosion_pso_why(true, true, 2) == nullptr);
    CHECK(std::string(g::explosion_pso_why(true, false, 10)) == "light-pso-missing");
    CHECK(std::string(g::explosion_pso_why(true, true, 28)) == "image-not-float");   // RGBA8 UNORM cannot hold A < 0
    CHECK(std::string(g::explosion_pso_why(false, true, 10)) == "not-explosion");
    // (3) one explosion, every a, three backgrounds (display values W_d; the eye pipeline holds decode(W_d) under the sprite layer)
    const float tx[3] = {1.0f, 0.55f, 0.15f};   // an orange texel x COLOR0 (assumed; the real texture is not decoded)
    double worstNew = 0, worstOld = 0;
    for (float wd : {0.0f, 0.35f, 0.7f})
        for (int k = 1; k <= 255; k += 6) {
            const float a = k / 255.0f, t[4] = {tx[0], tx[1], tx[2], a};
            const g::Extra7Out o = g::extra7(t);
            float game[3] = {wd, wd, wd}; g::game_explosion_rgb(o, game);
            float pl[4] = {0, 0, 0, 0}, ps[4] = {0, 0, 0, 0};
            g::sprite_explosion_blend(o, pl, g::ExplosionPso::light); g::sprite_explosion_blend(o, ps, g::ExplosionPso::shared);
            CHECK(std::fabs(pl[3] + a) < 1e-6 && ps[3] == 0.0f && pl[0] == ps[0]);   // T = 1 + a; RGB identical between the PSOs
            sg::Px under; under.r = under.g = under.b = g::srgb_decode(wd); under.a = 1.0f;
            const sg::Px n = g::sprite_over(under, g::sprite_layer(pl, 1.0f, 0, 1.0f), 0.0f, false);
            const float nd[3] = {g::srgb_encode(n.r), g::srgb_encode(n.g), g::srgb_encode(n.b)};
            // the previous path: shared clone (A = 0), linear add of the saturated display light, encoded by the sRGB RTV
            float od[3];
            for (int c = 0; c < 3; ++c) od[c] = g::srgb_encode(std::fmin(1.0f, std::fmin(1.0f, ps[c]) + under.r));
            for (int c = 0; c < 3; ++c) {
                const float gd = std::fmin(1.0f, game[c]);
                CHECK(std::fabs(nd[c] - gd) < 2e-4);
                worstNew = std::fmax(worstNew, std::fabs(nd[c] - gd)); worstOld = std::fmax(worstOld, std::fabs(od[c] - gd));
            }
        }
    CHECK(worstNew < 2e-4 && worstOld > 0.2);
    // (4) two overlapping explosions (T = (1+a1)(1+a2) <= 4) and a coverage puff (P = c x b, A = b) under an explosion: exact
    {
        const float a1 = 0.9f, a2 = 0.7f, wd = 0.3f, t1[4] = {tx[0], tx[1], tx[2], a1}, t2[4] = {0.9f, 0.4f, 0.1f, a2};
        float game[3] = {wd, wd, wd}; g::game_explosion_rgb(g::extra7(t1), game); g::game_explosion_rgb(g::extra7(t2), game);
        float p[4] = {0, 0, 0, 0}; g::sprite_explosion_blend(g::extra7(t1), p, g::ExplosionPso::light); g::sprite_explosion_blend(g::extra7(t2), p, g::ExplosionPso::light);
        CHECK(std::fabs((1.0f - p[3]) - (1 + a1) * (1 + a2)) < 1e-5 && p[3] >= g::kMinCoverage);
        sg::Px under; under.r = under.g = under.b = g::srgb_decode(wd); under.a = 1.0f;
        const sg::Px n = g::sprite_over(under, g::sprite_layer(p, 1.0f, 0, 1.0f), 0.0f, false);
        CHECK(std::fabs(g::srgb_encode(n.r) - std::fmin(1.0f, game[0])) < 2e-4 && std::fabs(g::srgb_encode(n.b) - std::fmin(1.0f, game[2])) < 2e-4);
        const float b = 0.5f, cpuff = 0.3f, x = 0.25f, t3[4] = {tx[0], tx[1], tx[2], x};
        float gp[3]; for (int c = 0; c < 3; ++c) gp[c] = b * cpuff + (1 - b) * 0.2f;   // the game: puff over W_d = 0.2 (SRC_ALPHA / INV_SRC_ALPHA), then the explosion
        g::game_explosion_rgb(g::extra7(t3), gp);
        float q[4] = {b * cpuff, b * cpuff, b * cpuff, b}; g::sprite_explosion_blend(g::extra7(t3), q, g::ExplosionPso::light);
        sg::Px u2; u2.r = u2.g = u2.b = g::srgb_decode(0.2f); u2.a = 1.0f;
        const sg::Px m = g::sprite_over(u2, g::sprite_layer(q, 1.0f, 0, 1.0f), 0.0f, false);
        CHECK(std::fabs(g::srgb_encode(m.r) - std::fmin(1.0f, gp[0])) < 2e-4 && std::fabs(g::srgb_encode(m.b) - std::fmin(1.0f, gp[2])) < 2e-4);
    }
    // (5) the look: full-alpha core over a 0.35 background = white; a = 0.3 rim = saturated orange; the previous path's rim is paler (tan)
    float coreD[3] = {}, rimD[3] = {}, rimOld[3] = {};
    {
        const float wd = 0.35f; sg::Px under; under.r = under.g = under.b = g::srgb_decode(wd); under.a = 1.0f;
        for (int pass = 0; pass < 2; ++pass) {
            const float a = pass == 0 ? 1.0f : 0.3f, t[4] = {tx[0], tx[1], tx[2], a};
            float pl[4] = {0, 0, 0, 0}, ps[4] = {0, 0, 0, 0};
            g::sprite_explosion_blend(g::extra7(t), pl, g::ExplosionPso::light); g::sprite_explosion_blend(g::extra7(t), ps, g::ExplosionPso::shared);
            const sg::Px n = g::sprite_over(under, g::sprite_layer(pl, 1.0f, 0, 1.0f), 0.0f, false);
            float* dst = pass == 0 ? coreD : rimD;
            dst[0] = g::srgb_encode(n.r); dst[1] = g::srgb_encode(n.g); dst[2] = g::srgb_encode(n.b);
            if (pass == 1) for (int c = 0; c < 3; ++c) rimOld[c] = g::srgb_encode(std::fmin(1.0f, std::fmin(1.0f, ps[c]) + under.r));
        }
        CHECK(coreD[0] > 0.99f && coreD[1] > 0.99f && coreD[2] > 0.99f);                  // white-hot core
        CHECK(rimD[0] > rimD[1] && rimD[1] > rimD[2] && rimD[0] > 0.8f);                   // orange rim
        CHECK((rimD[0] - rimD[2]) > (rimOld[0] - rimOld[2]) && rimD[0] > rimOld[0]);       // more saturated and brighter than the previous path
    }
    // (6) the coverage floor; a faded texel tends to T = 1
    {
        const float deep[4] = {0.5f, 0.5f, 0.5f, -1.0e5f};
        CHECK(g::sprite_layer(deep, 1.0f, 0, 1.0f).a == g::kMinCoverage && g::sprite_layer(deep, 1.0f, 0, 0.25f).a == g::kMinCoverage * 0.25f);
        const float tiny[4] = {0, 0, 0, -1.0f};   // pure brightening (T = 2), no light: under x 2 in display
        sg::Px under; under.r = 0.1f; under.g = 0.1f; under.b = 0.1f; under.a = 1.0f;
        const sg::Px n = g::sprite_over(under, g::sprite_layer(tiny, 1.0f, 0, 1.0f), 0.0f, false);
        CHECK(std::fabs(g::srgb_encode(n.r) - 2 * g::srgb_encode(0.1f)) < 1e-4 && n.a == 1.0f);
    }
    // (6b) W5-2 P1: three overlapping full-alpha draws, t = 0.01 per channel, over display 0.05 (unsaturated): game 0.12 -> 0.26 -> 0.54
    {
        const float t[4] = {0.01f, 0.01f, 0.01f, 1.0f}; const g::Extra7Out o = g::extra7(t);
        float game[3] = {0.05f, 0.05f, 0.05f}; float p[4] = {0, 0, 0, 0};
        for (int i = 0; i < 3; ++i) { g::game_explosion_rgb(o, game); g::sprite_explosion_blend(o, p, g::ExplosionPso::light); }
        CHECK(std::fabs(p[3] + 7.0f) < 1e-5 && game[0] < 1.0f && std::fabs(game[0] - 0.54f) < 0.05f);
        sg::Px under; under.r = under.g = under.b = g::srgb_decode(0.05f); under.a = 1.0f;
        const sg::Px n = g::sprite_over(under, g::sprite_layer(p, 1.0f, 0, 1.0f), 0.0f, false);
        CHECK(std::fabs(g::srgb_encode(n.r) - game[0]) < 2e-4);   // was 0.34 with the -3 floor
        // a later coverage sprite (b = 0.5, c = 0.3) composited into the same image keeps the stack (A' = A + b (1 - A)... via ONE/INV: here the CPU layer only)
        float p2[4] = {p[0], p[1], p[2], p[3]};
        for (int i = 0; i < 9; ++i) g::sprite_explosion_blend(o, p2, g::ExplosionPso::light);   // 12 stacked: A = -4095, still above the floor
        CHECK(std::fabs(p2[3] + 4095.0f) < 1.0f && p2[3] >= g::kMinCoverage);
    }
    // (6c) non-finite texels never produce NaN (NaN -> nothing, +Inf -> largest finite fp16 before the tonemap)
    {
        const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
        const float bad[4] = {inf, nan, 1.0f, nan};
        for (int tm = 0; tm < 2; ++tm) {
            const sg::Px l = g::sprite_layer(bad, 1.0f, tm, 1.0f);
            CHECK(std::isfinite(l.r) && std::isfinite(l.g) && std::isfinite(l.b) && std::isfinite(l.a));
            sg::Px under; under.r = under.g = under.b = 0.2f; under.a = 1.0f;
            const sg::Px n = g::sprite_over(under, l, 0.0f, false);
            CHECK(std::isfinite(n.r) && std::isfinite(n.g) && std::isfinite(n.b));
        }
    }
    // (6d) W5-2 P2: the sprite frame state tracks all kMaxDraws replays (the smoke's 16-entry store lost 17..32)
    {
        g::FrameState fs; fs.begin(true); fs.cleared = true;
        static_assert(g::FrameState::kCap == g::kMaxDraws && sg::FrameState::kCap == 64, "sprite kMaxDraws, smoke 64 (raised from 16, effects-outside-window.md section 18)");
        uint64_t words[g::kMaxDraws] = {};
        for (unsigned i = 0; i < g::kMaxDraws; ++i) { fs.note_replay(true, i * 8); words[i] = (i != 30) ? 1 : 0; }   // all but #30 commit
        fs.close(reinterpret_cast<const uint8_t*>(words), 0);
        CHECK(fs.replays == g::kMaxDraws && fs.committed == g::kMaxDraws - 1 && fs.valid());
        g::FrameState f2; f2.begin(true); f2.cleared = true;
        for (unsigned i = 0; i < 20; ++i) f2.note_replay(true, i * 8);
        uint64_t zeros[20] = {}; for (unsigned i = 16; i < 20; ++i) zeros[i] = 1;
        f2.close(reinterpret_cast<const uint8_t*>(zeros), 0);
        CHECK(f2.replays == 20 && f2.committed == 4);   // late entries counted (16..19 only)
    }
    std::printf("  explosion light: game display value matched within %.1e (previous path off by up to %.2f); core %.2f,%.2f,%.2f rim %.2f,%.2f,%.2f (previous rim %.2f,%.2f,%.2f)\n",
                worstNew, worstOld, coreD[0], coreD[1], coreD[2], rimD[0], rimD[1], rimD[2], rimOld[0], rimOld[1], rimOld[2]);
}

void test_explosion_class_gate() {
    using namespace gpu_copy::step5;
    gpu_copy::step3::Shape s; s.pso.known = true; s.pso.blend0 = true; s.pso.write_mask0 = 15; s.pso.depth_enable = true; s.pso.depth_func = gpu_copy::step3::cmp::greater;
    HudTarget t;
    const EffectsAllow allow = EffectsAllow::defaults();
    EffectsClassIn cin; cin.allow = &allow; cin.ps8 = effects_ps_explosion; cin.dual = true;
    auto why = [&]() { const EffectsVerdict v = effects_decide(s, t, effects_open, 0, &cin); return std::string(v.why ? v.why : "ok"); };
    CHECK(why() == "class-ps-not-allowlisted");                     // fxmem < 5: as before (D-370 seen455 rep0)
    cin.explosion = true; cin.dual = false;
    CHECK(why() == "class-explosion-needs-dual");
    cin.dual = true;
    const std::string w = why();
    CHECK(w.rfind("class-", 0) != 0);                               // past the class gate (the rest of the rules decide: hud_decide, dual_ready, ...)
    s.pso.dual_ready = false;
    cin.ps8 = 0x12345678u; CHECK(why() == "class-ps-not-allowlisted");   // the flag admits only the explosion PS
    cin.ps8 = effects_ps_explosion; s.pso.depth_write = true; CHECK(why() == "class-state-not-effect");
}

// The CPU dry run of fxmem:5: explosion quads in the game window, eyes from the game camera +- half IPD; a sprite the game submitted partly outside its window
// becomes eye-only for a head turned toward it; no world camera = refused with the reason; the line fits the diagnostics text.
void test_sprite_plan() {
    const Cam g{{1000, 4000, 2000}, deg(30), deg(-5)};
    double ag[16]; make_a(g, ag); fxmem::ScreenMap gm; CHECK(fxsprite::game_map_from_a(ag, gm));
    double f[3], r[3], u[3]; axes(g.yaw, g.pitch, f, r, u);
    std::vector<fxmem::Vtx> v; std::vector<uint16_t> idx; double cw[4][3];
    auto quad = [&](double ahead, double right, double R) {
        double p[3]; for (int i = 0; i < 3; ++i) p[i] = g.e[i] + ahead * f[i] + right * r[i];
        fxmem::Vtx q[4]; game_quad(gm, g, p, R, false, q, cw);
        if (!idx.empty()) idx.push_back(0xFFFF);
        for (int k = 0; k < 4; ++k) { idx.push_back(uint16_t(100 + v.size())); v.push_back(q[k]); }   // indices start at 100 (re-based by lo)
    };
    quad(1200, 0, 60); quad(1500, 200, 80);
    quad(1000, 640, 60);   // centre ~33 deg right: the game window edge is ~31.6 deg -> two corners outside the game window
    uint32_t lo = 0, hi = 0; CHECK(fxsprite::index_range(idx.data(), idx.size(), lo, hi) && lo == 100 && hi == 111);
    const uint16_t cuts[3] = {0xFFFF, 0xFFFF, 0xFFFF}; CHECK(!fxsprite::index_range(cuts, 3, lo, hi));
    CHECK(fxsprite::index_range(idx.data(), idx.size(), lo, hi));
    std::vector<fxmem::Vtx> scratch(v.size()); std::vector<uint8_t> bad(v.size());
    auto eyeMap = [&](double yawOff, double side, double tanScale) {
        const Cam e{{g.e[0] + side * r[0], g.e[1] + side * r[1], g.e[2] + side * r[2]}, g.yaw + deg(yawOff), g.pitch};
        double a[16]; make_a(e, a, tanScale); fxmem::ScreenMap m; CHECK(fxsprite::eye_map_from_a(a, m)); return m;
    };
    // (1) eyes = the game camera +- 0.032 units, same lens: everything visible as in the game window, nothing eye-only
    fxmem::ScreenMap e0 = eyeMap(0, -0.032, 1.0), e1 = eyeMap(0, 0.032, 1.0);
    const fxmem::ScreenMap* eyes[2] = {&e0, &e1};
    fxsprite::Plan p; p.begin();
    fxsprite::plan_draw(gm, eyes, v.data(), v.size(), idx.data(), idx.size(), lo, 1.0, scratch.data(), bad.data(), p);
    CHECK(p.draws == 1 && p.prepared == 1 && p.refused == 0 && p.vertices == 12 && p.quads == 3 && p.z_form == 12 && p.no_depth == 0);
    CHECK(p.outside_window == 2 && p.in_window == 10);              // the game submitted two corners outside its window
    CHECK(p.eye_visible[0] == 10 && p.eye_visible[1] == 10 && p.eye_only == 0 && p.behind == 0);
    // (2) the head turned 30 deg right with a wider HMD lens: the off-window corners are now inside the eye (eye-only)
    fxmem::ScreenMap t0 = eyeMap(-30, -0.032, 1.9), t1 = eyeMap(-30, 0.032, 1.9);
    const fxmem::ScreenMap* turned[2] = {&t0, &t1};
    p.begin(); fxsprite::plan_draw(gm, turned, v.data(), v.size(), idx.data(), idx.size(), lo, 1.0, scratch.data(), bad.data(), p);
    CHECK(p.prepared == 1 && p.eye_only == 2 && p.eye_visible[0] == 12 && p.eye_visible[1] == 12);
    // (3) no world camera: the window statistics are still counted, the eye part is refused with the reason
    const fxmem::ScreenMap* none[2] = {nullptr, nullptr};
    p.begin(); p.world_why = "sprite-world-no-object-record";
    fxsprite::plan_draw(gm, none, v.data(), v.size(), idx.data(), idx.size(), lo, 1.0, scratch.data(), bad.data(), p);
    CHECK(p.refused == 1 && p.prepared == 0 && p.vertices == 12 && std::string(p.last_why) == "sprite-world-no-object-record");
    // (4) a 2D vertex refuses the draw
    std::vector<fxmem::Vtx> flat = v; flat[5].pos[2] = 0.0f; flat[5].pos[3] = 1.0f;
    p.begin(); fxsprite::plan_draw(gm, eyes, flat.data(), flat.size(), idx.data(), idx.size(), lo, 1.0, scratch.data(), bad.data(), p);
    CHECK(p.refused == 1 && p.no_depth == 1 && std::string(p.last_why) == "sprite-no-depth");
    CHECK(p.streaks == 0);   // square explosion quads are never streaks
    // (5) tracer discrimination: the square explosion quads are not streaks; a 150 x 4 unit quad along the view axis 300 units ahead (pale yellow) is
    {
        std::vector<fxmem::Vtx> sv = v; std::vector<uint16_t> si = idx;
        double base[3]; for (int i = 0; i < 3; ++i) base[i] = g.e[i] + 300 * f[i] + 20 * r[i] - 15 * u[i];
        const double off[4][2] = {{-2, 0}, {2, 0}, {-2, 150}, {2, 150}};   // (lateral, along the view axis)
        si.push_back(0xFFFF);
        for (int k = 0; k < 4; ++k) {
            double w[3]; for (int i = 0; i < 3; ++i) w[i] = base[i] + off[k][0] * r[i] + off[k][1] * f[i];
            double xs, ys, d; CHECK(fxsprite::sp_project(gm, w, xs, ys, d));
            fxmem::Vtx q{}; q.pos[0] = float(xs); q.pos[1] = float(ys); q.pos[2] = float(fxmem::game_z_for_depth(d)); q.pos[3] = 1.0f;
            q.col[0] = 0.9f; q.col[1] = 0.85f; q.col[2] = 0.5f; q.col[3] = 0.7f;
            si.push_back(uint16_t(100 + sv.size())); sv.push_back(q);
        }
        std::vector<fxmem::Vtx> sc(sv.size()); std::vector<uint8_t> sb(sv.size());
        p.begin(); fxsprite::plan_draw(gm, eyes, sv.data(), sv.size(), si.data(), si.size(), lo, 1.0, sc.data(), sb.data(), p);
        CHECK(p.quads == 4 && p.streaks == 1 && p.prepared == 1);
        CHECK(std::fabs(p.streak_rgb[0] - 0.9) < 1e-6 && std::fabs(p.streak_rgb[2] - 0.5) < 1e-6);
        CHECK(std::fabs(p.streak_len_max - 150.0) < 0.5 && p.streak_near > 300 && p.streak_near < 400 && p.near_depth == p.streak_near);
        char sl[600]; fxsprite::format_plan(sl, sizeof sl, 7, "sprite(tracer-cand)", p, 0);
        CHECK(std::string(sl).find("streaks=1(rgb:0.90,0.85,0.50;near:") != std::string::npos);
    }
    // line length (realistic maxima)
    fxsprite::Plan w; w.draws = w.read = w.prepared = w.refused = 99999; w.vertices = w.quads = w.in_window = w.outside_window = w.z_form = w.q_form = w.no_depth = 9999999;
    w.eye_visible[0] = w.eye_visible[1] = w.eye_only = w.behind = w.collapsed = 9999999; w.last_why = "sprite-world-camera-unavailable";
    char line[600]; w.streaks = 9999999; w.streak_rgb[0] = w.streak_rgb[1] = w.streak_rgb[2] = 9999999.0 * 2; w.near_depth = w.streak_near = w.streak_len_max = 99999;
    fxsprite::format_plan(line, sizeof line, 999999999ull, "sprite(tracer-cand)", w, 99999); CHECK(std::strlen(line) < kLineCap);
    std::printf("  sprite plan: game window %u in / %u out; head 30 deg right: eye-only %u\n", 10u, 2u, 2u);
}

// ------------------------------------------------------------------ fxmem:6 (section 11, fx_sprite_gpu.h)
// (1) plan_draw's keep copies are exactly the per-eye re-projection + collapse (what the GPU uploads); a refused draw is not marked prepared;
// (2) budget, span offsets, eye views (base vertex -lo); (3) every gate refusal; (4) the command sequence; (5) the composite: an additive explosion texel
// (A = 0) survives the sprite composite and would vanish in the smoke composite (the reason for the second image); opaque A = 1 equals the smoke composite;
// (6) the eye depth order through the copy: a sprite behind a replayed object fails the GREATER test, in front of it passes; (7) line lengths.
void test_sprite_gpu() {
    namespace g = fxsprite::gpu; namespace sg = acz::fxsmoke::gpu;
    const Cam gc{{1000, 4000, 2000}, deg(30), deg(-5)};
    double ag[16]; make_a(gc, ag); fxmem::ScreenMap gm; CHECK(fxsprite::game_map_from_a(ag, gm));
    double f[3], r[3], u[3]; axes(gc.yaw, gc.pitch, f, r, u);
    std::vector<fxmem::Vtx> v; std::vector<uint16_t> idx; double cw[4][3];
    std::vector<std::array<double, 3>> corners;
    auto quad = [&](double ahead, double right, double R) {
        double p[3]; for (int i = 0; i < 3; ++i) p[i] = gc.e[i] + ahead * f[i] + right * r[i];
        fxmem::Vtx q[4]; game_quad(gm, gc, p, R, false, q, cw);
        if (!idx.empty()) idx.push_back(0xFFFF);
        for (int k = 0; k < 4; ++k) { idx.push_back(uint16_t(200 + v.size())); v.push_back(q[k]); corners.push_back({cw[k][0], cw[k][1], cw[k][2]}); }
    };
    quad(1200, 0, 60); quad(1000, 640, 60);   // the second quad straddles the game window's right edge (the L3 out-win case)
    uint32_t lo = 0, hi = 0; CHECK(fxsprite::index_range(idx.data(), idx.size(), lo, hi) && lo == 200 && hi == 207);
    auto eyeMap = [&](double yawOff, double side, double tanScale) {
        const Cam e{{gc.e[0] + side * r[0], gc.e[1] + side * r[1], gc.e[2] + side * r[2]}, gc.yaw + deg(yawOff), gc.pitch};
        double a[16]; make_a(e, a, tanScale); fxmem::ScreenMap m; CHECK(fxsprite::eye_map_from_a(a, m)); return m;
    };
    fxmem::ScreenMap t0 = eyeMap(-30, -0.032, 1.9), t1 = eyeMap(-30, 0.032, 1.9);
    const fxmem::ScreenMap* eyes[2] = {&t0, &t1};
    std::vector<fxmem::Vtx> scratch(v.size()), k0(v.size()), k1(v.size()); std::vector<uint8_t> bad(v.size());
    fxsprite::Plan p; p.begin();
    fxsprite::plan_draw(gm, eyes, v.data(), v.size(), idx.data(), idx.size(), lo, 1.0, scratch.data(), bad.data(), p, k0.data(), k1.data());
    CHECK(p.prepared == 1 && p.outside_window > 0 && p.eye_only > 0);
    // (1) the copies are each eye's re-projection of the world corners (and carry the game's colour / uv bytes)
    double worst = 0;
    for (int e = 0; e < 2; ++e) {
        const auto& kk = e ? k1 : k0; const fxmem::ScreenMap& em = e ? t1 : t0;
        for (size_t i = 0; i < v.size(); ++i) {
            double xs, ys, d; CHECK(fxsprite::sp_project(em, corners[i].data(), xs, ys, d));
            worst = std::fmax(worst, std::fmax(std::fabs(kk[i].pos[0] - xs), std::fabs(kk[i].pos[1] - ys)));
            CHECK(std::fabs(kk[i].pos[2] - fxmem::game_z_for_depth(d)) < 1e-3 * fxmem::game_z_for_depth(d));
            CHECK(std::memcmp(kk[i].col, v[i].col, sizeof v[i].col) == 0 && std::memcmp(kk[i].uv, v[i].uv, sizeof v[i].uv) == 0);
        }
    }
    CHECK(worst < 0.05);
    CHECK(std::memcmp(k0.data(), k1.data(), k0.size() * sizeof(fxmem::Vtx)) != 0);   // the eyes differ (IPD)
    {   // a refused draw (2D vertex) is not prepared: the GPU plan must not use its (undefined) copies
        std::vector<fxmem::Vtx> flat = v; flat[3].pos[2] = 0.0f; flat[3].pos[3] = 1.0f;
        fxsprite::Plan q; q.begin();
        fxsprite::plan_draw(gm, eyes, flat.data(), flat.size(), idx.data(), idx.size(), lo, 1.0, scratch.data(), bad.data(), q, k0.data(), k1.data());
        CHECK(q.prepared == 0 && q.refused == 1);
    }
    // (2) memory and span layout
    uint64_t charged = 0;
    CHECK(g::budget(8, 2560, 1440, 3, charged) == nullptr && charged > 170000000ull && charged < g::kTotalCap);
    CHECK(std::string(g::budget(0, 2560, 1440, 3, charged)) == "sprite-format-unsupported" && charged == 0);
    CHECK(std::string(g::budget(8, 4096, 2160, 3, charged)) == "sprite-over-image-cap");
    CHECK(std::string(g::budget(8, 0, 1440, 3, charged)) == "sprite-size-unsupported");
    // (H) packed span: copies aligned to 256 B, a full-size draw = 2 x 32 KiB, a 24-vertex tracer strip = 2 x 1.5 KiB; 8 full-size draws still fit
    CHECK(g::copy_bytes(1) == 256 && g::copy_bytes(4) == 256 && g::copy_bytes(24) == 1536 && g::copy_bytes(fxsprite::kMaxVertices) == g::kEyeCopyBytes);
    CHECK(g::replay_bytes(fxsprite::kMaxVertices) * 16 == g::kSlotUpload && g::kSlotUpload == 1048576 && g::kExplosionReserveBytes == 131072);   // (Q) 1 MiB: 16 full-size draws
    CHECK(g::offset(0, 8, 0) == 0 && g::offset(0, 8, 1) == 512 && g::offset(4096, 24, 1) == 4096 + 1536 && g::offset(4096, 24, 1) % 16 == 0);
    g::EyeViews ev;
    CHECK(g::eye_views(0x10000, 1024, 8, 200, ev) && ev.va[0] == 0x10000 + 1024 && ev.va[1] == 0x10000 + 1024 + 512 && ev.bytes == 8 * 64 &&
          ev.stride == 64 && ev.base == -200 && ev.used == 1024);
    CHECK(!g::eye_views(0, 0, 8, 0, ev) && !g::eye_views(0x10000, 0, 0, 0, ev) && !g::eye_views(0x10000, 0, fxsprite::kMaxVertices + 1, 0, ev) &&
          !g::eye_views(0x10000, 100, 8, 0, ev) &&                                             // misaligned cursor
          !g::eye_views(0x10000, g::kSlotUpload - 256, 8, 0, ev) &&                            // the pair would end beyond the span
          !g::eye_views(0x10000, g::kSlotUpload + 256, 8, 0, ev) && ev.used == 0);
    CHECK(g::eye_views(0x10000, g::kSlotUpload - 512, 4, 0, ev) && ev.va[1] + 256 == 0x10000 + g::kSlotUpload);   // exactly fills the span
    {   // a frame of 30 tracer strips (24 vertices) then an explosion (512 vertices): every one fits, no two copies overlap, the explosion is not starved
        uint64_t cur = 0; unsigned r = 0; std::vector<std::pair<uint64_t, uint64_t>> ranges;
        auto take = [&](uint32_t n, bool expl) {
            g::GateIn x; x.ready = x.effects_recorded = x.dual = x.prepared = true; x.vertices = n; x.draw_index = r; x.span_used = cur; x.explosion = expl;
            x.indexed = true; x.ib_stride = 2; x.vb_stride = 64; x.instances = 1; x.vb_va = 0x7000; x.vb_bytes = 65536; x.eye_depth = true; x.eye_dsv = x.pso_dsv = 20;
            x.pso_rtv0 = x.image_format = 10; x.pso_samples = 1; x.pso_rt_count = 1; x.depth_enable = true; x.depth_func = sg::kCmpGreater; x.vp_known = true;
            x.viewports = 1; x.vp[2] = 2560; x.vp[3] = 1440; x.image_w = 2560; x.image_h = 1440; x.scissors = 1; x.sc[2] = 2560; x.sc[3] = 1440;
            const char* w = g::gate(x); if (w) return std::string(w);
            g::EyeViews e; if (!g::eye_views(0x10000, cur, n, 0, e)) return std::string("eye-view");
            for (unsigned k = 0; k < 2; ++k) ranges.push_back({e.va[k] - 0x10000, e.va[k] - 0x10000 + n * 64});
            cur += e.used; ++r; return std::string("none");
        };
        for (unsigned i = 0; i < g::kMaxDraws - g::kExplosionReserveDraws; ++i) CHECK(take(24, false) == "none");
        CHECK(take(24, false) == "sprite-gpu-draw-cap");   // the sprite / tracer classes stop at kMaxDraws - reserve
        CHECK(take(fxsprite::kMaxVertices, true) == "none" && take(fxsprite::kMaxVertices, true) == "none");   // two full-size explosions still fit
        CHECK(take(8, true) == "sprite-gpu-draw-cap" && r == g::kMaxDraws && cur <= g::kSlotUpload);
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size(); ++i) CHECK(ranges[i].first >= ranges[i - 1].second);
    }
    {   // the byte reserve: sprite draws of 512 vertices stop at 896 KiB (Q: 1 MiB span), an explosion still gets its 128 KiB
        g::GateIn x; x.ready = x.effects_recorded = x.dual = x.prepared = true; x.vertices = fxsprite::kMaxVertices; x.indexed = true; x.ib_stride = 2;
        x.vb_stride = 64; x.instances = 1; x.vb_va = 0x7000; x.vb_bytes = 65536; x.eye_depth = true; x.eye_dsv = x.pso_dsv = 20; x.pso_rtv0 = x.image_format = 10;
        x.pso_samples = 1; x.pso_rt_count = 1; x.depth_enable = true; x.depth_func = sg::kCmpGreater; x.vp_known = true; x.viewports = 1; x.vp[2] = 2560;
        x.vp[3] = 1440; x.image_w = 2560; x.image_h = 1440; x.scissors = 1; x.sc[2] = 2560; x.sc[3] = 1440;
        x.draw_index = 13; x.span_used = 13 * g::replay_bytes(fxsprite::kMaxVertices); CHECK(g::gate(x) == nullptr);   // the 14th full-size sprite draw ends at 896 KiB
        x.draw_index = 14; x.span_used = 14 * g::replay_bytes(fxsprite::kMaxVertices);
        CHECK(std::string(g::gate(x)) == "sprite-gpu-span-full");
        x.explosion = true; CHECK(g::gate(x) == nullptr);
        x.span_used = g::kSlotUpload; CHECK(std::string(g::gate(x)) == "sprite-gpu-span-full");
        x.span_used = g::kSlotUpload + 64; CHECK(std::string(g::gate(x)) == "sprite-gpu-span-full");   // never underflows
    }
    {   // (N) step158/159: a DENSE class-1 frame with a FULL puff memory (160 cards = 960 vertices per eye re-emitted at the first host draw) refuses no game sprite
        const unsigned kCards = 160, kCardVerts = kCards * 6;
        auto gate_for = [&](uint32_t n, unsigned draw_index, uint64_t span_used) {
            g::GateIn x; x.ready = x.effects_recorded = x.dual = x.prepared = true; x.vertices = n; x.draw_index = draw_index; x.span_used = span_used;
            x.indexed = true; x.ib_stride = 2; x.vb_stride = 64; x.instances = 1; x.vb_va = 0x7000; x.vb_bytes = 65536; x.eye_depth = true; x.eye_dsv = x.pso_dsv = 20;
            x.pso_rtv0 = x.image_format = 10; x.pso_samples = 1; x.pso_rt_count = 1; x.depth_enable = true; x.depth_func = sg::kCmpGreater; x.vp_known = true;
            x.viewports = 1; x.vp[2] = 2560; x.vp[3] = 1440; x.image_w = 2560; x.image_h = 1440; x.scissors = 1; x.sc[2] = 2560; x.sc[3] = 1440;
            const char* w = g::gate(x); return std::string(w ? w : "none");
        };
        CHECK(g::replay_bytes(kCardVerts) == 122880 && g::replay_bytes(kCardVerts) <= g::kTrailBytes && g::kSlotStride == g::kSlotUpload + g::kTrailBytes);
        struct Frame { unsigned draws; uint32_t verts; };
        // the step159 shape (94 draws of ~10 vertices: frame 19944 read 101, 938 vertices) and heavier ones that fit the game's span ALONE
        const Frame frames[] = {{94, 10}, {94, 4}, {100, 64}, {50, 128}};   // (Q) the heavy ones are sized so the game's span alone (896 KiB) still fits
        for (const Frame& fr : frames) {
            const uint64_t game_total = uint64_t(fr.draws) * g::replay_bytes(fr.verts);
            CHECK(game_total <= g::kSlotUpload - g::kExplosionReserveBytes);   // the game's own sprites fit the game's span alone
            // OLD accounting (the cache shares the game's cursor and is taken first): the game draws it would refuse
            unsigned old_refused = 0;
            { uint64_t c0 = g::replay_bytes(kCardVerts); for (unsigned i = 0; i < fr.draws; ++i) {
                if (gate_for(fr.verts, i, c0) != "none") ++old_refused; else c0 += g::replay_bytes(fr.verts); } }
            // NEW accounting: the cache takes its own region first, the game's cursor never sees it
            g::TrailTake tk; uint64_t trail_used = 0, cur = 0; unsigned refused = 0;
            CHECK(g::trail_take(trail_used, kCardVerts, kCardVerts, tk) == nullptr && tk.used == 122880);
            CHECK(tk.at[0] == g::kSlotUpload && tk.at[1] == g::kSlotUpload + g::copy_bytes(kCardVerts) && tk.at[1] + g::copy_bytes(kCardVerts) <= g::kSlotStride);
            trail_used += tk.used;
            for (unsigned i = 0; i < fr.draws; ++i) {
                if (gate_for(fr.verts, i, cur) != "none") ++refused;
                g::EyeViews ev; CHECK(g::eye_views(0x10000, cur, fr.verts, 0, ev) && ev.va[1] + g::copy_bytes(fr.verts) <= 0x10000 + g::kSlotUpload);   // below the trail region
                cur += ev.used;
            }
            CHECK(refused == 0 && cur == game_total);
            // (M5) a second host of the same frame with a full selection still fits (smoke + clouds); a THIRD is dropped by name, nothing of the game's moves
            CHECK(g::kTrailBytes == 262144 && g::kSlotStride == 1310720);
            CHECK(g::trail_take(trail_used, kCardVerts, kCardVerts, tk) == nullptr && tk.used == 122880 && tk.at[1] + g::copy_bytes(kCardVerts) <= g::kSlotStride);
            trail_used += tk.used;
            CHECK(std::string(g::trail_take(trail_used, kCardVerts, kCardVerts, tk)) == "trail-span-full" && tk.used == 0);
            CHECK(g::trail_take(trail_used, 0, 48, tk) == nullptr && tk.at[0] >= g::kSlotUpload + trail_used);   // a small selection still fits behind the first
            std::printf("  (N) dense %u x %u vertices: shared cursor would refuse %u game draws, own region 0\n", fr.draws, unsigned(fr.verts), old_refused);
            if (fr.verts >= 64) CHECK(old_refused > 0);   // the heavier frames show the starvation the shared cursor allowed
        }
        g::TrailTake tz; CHECK(std::string(g::trail_take(g::kTrailBytes + 1, 1, 0, tz)) == "trail-span-full" && g::trail_take(0, 0, 0, tz) == nullptr);   // never underflows
    }
    // index i of the game's IB addresses copy vertex i - lo: every index of the draw lands inside the copy
    CHECK(g::eye_views(0x10000, 0, hi - lo + 1, lo, ev));   // (the refusals above reset ev)
    for (uint16_t i : idx) if (i != 0xFFFF) CHECK(int32_t(i) + ev.base >= 0 && uint32_t(int32_t(i) + ev.base) < hi - lo + 1);
    // (3) the gate
    g::GateIn ok; ok.ready = ok.effects_recorded = ok.dual = ok.prepared = true; ok.vertices = 8; ok.draw_index = 0; ok.indexed = true; ok.ib_stride = 2;
    ok.vb_stride = 64; ok.instances = 1; ok.vb_va = 0x7000; ok.vb_bytes = 65536; ok.eye_depth = true; ok.eye_dsv = ok.pso_dsv = 20; ok.pso_rtv0 = ok.image_format = 10;
    ok.pso_samples = 1; ok.pso_rt_count = 1; ok.depth_enable = true; ok.depth_func = sg::kCmpGreater; ok.stencil = true; ok.stencil_noop = true;
    ok.vp_known = true; ok.viewports = 1; ok.vp[2] = 2560; ok.vp[3] = 1440; ok.image_w = 2560; ok.image_h = 1440; ok.scissors = 1; ok.sc[2] = 2560; ok.sc[3] = 1440;
    CHECK(g::gate(ok) == nullptr);
    auto why = [&](auto mod) { g::GateIn x = ok; mod(x); const char* w = g::gate(x); return std::string(w ? w : "none"); };
    CHECK(why([](g::GateIn& x) { x.ready = false; }) == "sprite-gpu-not-ready");
    CHECK(why([](g::GateIn& x) { x.effects_recorded = false; }) == "sprite-effects-not-recorded");
    CHECK(why([](g::GateIn& x) { x.dual = false; }) == "sprite-needs-effects-dual");
    CHECK(why([](g::GateIn& x) { x.prepared = false; }) == "sprite-not-prepared");
    CHECK(why([](g::GateIn& x) { x.vertices = fxsprite::kMaxVertices + 1; }) == "sprite-vertex-count");
    CHECK(why([](g::GateIn& x) { x.draw_index = g::kMaxDraws; }) == "sprite-gpu-draw-cap");
    CHECK(why([](g::GateIn& x) { x.draw_index = g::kMaxDraws - g::kExplosionReserveDraws; }) == "sprite-gpu-draw-cap");
    CHECK(why([](g::GateIn& x) { x.draw_index = g::kMaxDraws - 1; x.explosion = true; }) == "none");
    CHECK(why([](g::GateIn& x) { x.span_used = g::kSlotUpload - g::kExplosionReserveBytes; }) == "sprite-gpu-span-full");
    CHECK(why([](g::GateIn& x) { x.ib_stride = 4; }) == "sprite-not-indexed-r16");
    CHECK(why([](g::GateIn& x) { x.vb_stride = 32; }) == "sprite-vb-stride");
    CHECK(why([](g::GateIn& x) { x.instances = 2; }) == "sprite-instanced");
    CHECK(why([](g::GateIn& x) { x.vb_bytes = 0x100000000ull; }) == "sprite-vb-view");
    CHECK(why([](g::GateIn& x) { x.eye_depth = false; }) == "sprite-eye-depth-missing");
    CHECK(why([](g::GateIn& x) { x.pso_dsv = 40; }) == "sprite-dsv-format-differs");
    CHECK(why([](g::GateIn& x) { x.pso_samples = 4; }) == "sprite-pso-multisample");
    CHECK(why([](g::GateIn& x) { x.pso_rtv0 = 28; }) == "sprite-rt-format-differs");
    CHECK(why([](g::GateIn& x) { x.depth_func = 2; }) == "sprite-pso-depth-test");
    CHECK(why([](g::GateIn& x) { x.depth_write = true; }) == "sprite-pso-depth-write");
    CHECK(why([](g::GateIn& x) { x.stencil_noop = false; }) == "sprite-pso-stencil");
    CHECK(why([](g::GateIn& x) { x.viewports = 2; }) == "sprite-viewport-unknown");
    CHECK(why([](g::GateIn& x) { x.vp[2] = 1280; }) == "sprite-viewport-not-image");
    CHECK(why([](g::GateIn& x) { x.sc[2] = 2000; }) == "sprite-scissor-not-image");
    CHECK(why([](g::GateIn& x) { x.scissors = 0; }) == "sprite-scissor-not-image");
    CHECK(why([](g::GateIn& x) { x.depth_func = sg::kCmpGreaterEqual; x.stencil = false; }) == "none");
    // (4) the command sequence
    g::Cmd cmds[g::kMaxCmds]; const unsigned n = g::replay_commands(cmds);
    CHECK(n == 10 && n + 2 == g::kCmdsPerDraw);   // + SetPredication on / off around them
    CHECK(cmds[0].kind == g::CmdKind::set_pso_clone && cmds[7].kind == g::CmdKind::vb_game && cmds[8].kind == g::CmdKind::set_pso_game && cmds[9].kind == g::CmdKind::om_restore);
    for (unsigned e = 0; e < 2; ++e)
        CHECK(cmds[1 + 3 * e].kind == g::CmdKind::om_eye && cmds[2 + 3 * e].kind == g::CmdKind::vb_eye && cmds[3 + 3 * e].kind == g::CmdKind::draw_eye &&
              cmds[1 + 3 * e].eye == e && cmds[2 + 3 * e].eye == e && cmds[3 + 3 * e].eye == e);
    // (5) the composite: extra7_p over EMPTY space (clone alpha ZERO/ONE: A stays 0) = light P = a (1 + a) t
    const double a = 0.8, t[3] = {1.0, 0.55, 0.15};
    const float glow[4] = {float(a * (1 + a) * t[0]), float(a * (1 + a) * t[1]), float(a * (1 + a) * t[2]), 0.0f};
    sg::Px under; under.r = 0.2f; under.g = 0.3f; under.b = 0.4f; under.a = 1.0f;
    const sg::Px sp = g::sprite_over(under, g::sprite_layer(glow, 1.0f, 0, 1.0f), 0.0f, false);
    // (I) composited in DISPLAY space: decode(min(1, P + encode(under)))
    auto dsp = [&](float pv, float uv) { return g::srgb_decode(std::fmin(1.0f, std::fmin(1.0f, pv) + g::srgb_encode(uv))); };
    CHECK(std::fabs(sp.r - dsp(glow[0], 0.2f)) < 1e-5 && std::fabs(sp.g - dsp(glow[1], 0.3f)) < 1e-5 && std::fabs(sp.b - dsp(glow[2], 0.4f)) < 1e-5 && sp.a == 1.0f);
    CHECK(sp.b > glow[2] + 0.4f);   // brighter than the old linear add (display light over decoded under)
    const sg::Px sm = sg::smoke_over(under, sg::smoke_layer(glow, 1.0f, 0, 1.0f), 0.0f, false);
    CHECK(sm.r == under.r && sm.g == under.g && sm.b == under.b);   // the smoke composite drops coverage-free light: why the sprites have their own image
    {   // an opaque covered texel (A = 1, P <= 1): the under pixel is hidden; the display value P is decoded once (the smoke composite keeps it as linear)
        const float opaque[4] = {0.6f, 0.5f, 0.4f, 1.0f};
        const sg::Px x = g::sprite_over(under, g::sprite_layer(opaque, 1.0f, 0, 1.0f), 0.0f, false), y = sg::smoke_over(under, sg::smoke_layer(opaque, 1.0f, 0, 1.0f), 0.0f, false);
        CHECK(std::fabs(x.r - g::srgb_decode(0.6f)) < 1e-6 && std::fabs(x.g - g::srgb_decode(0.5f)) < 1e-6 && std::fabs(x.b - g::srgb_decode(0.4f)) < 1e-6 && x.a == 1.0f);
        CHECK(std::fabs(g::srgb_encode(x.r) - y.r) < 1e-5 && std::fabs(x.a - y.a) < 1e-6);
        const float empty[4] = {0, 0, 0, 0};   // an empty texel: `under` returned bit for bit (no encode / decode round trip)
        const sg::Px z = g::sprite_over(under, g::sprite_layer(empty, 1.0f, 0, 1.0f), 0.0f, false);
        CHECK(z.r == under.r && z.g == under.g && z.b == under.b && z.a == under.a);
    }
    {   // the cockpit band keeps its pixel (as the smoke); Reinhard keeps the light finite; the decouple fade scales it
        const sg::Px c = g::sprite_over(under, g::sprite_layer(glow, 1.0f, 0, 1.0f), 0.002f, false);
        CHECK(c.r == under.r && c.g == under.g);
        const sg::Px rh = g::sprite_layer(glow, 4.0f, 1, 0.5f);
        CHECK(rh.r > 0 && rh.r < 0.5f && rh.a == 0.0f);
    }
    // (6) depth order through the copy: z_s of a copy vertex at view depth d' is the eye depth an object at d' writes (GREATER test)
    {
        const double scale = 0.0009765625, dObj = 1100.0;
        const double objDepth = sg::eye_depth(dObj, scale);
        for (double d : {900.0, 1300.0}) {
            const double zs = scale * fxmem::game_z_for_depth(d) * acz::fxsmoke::kDepthUnit;
            CHECK(std::fabs(zs - sg::eye_depth(d, scale)) < 1e-12);
            CHECK(sg::depth_passes(zs, objDepth, sg::kCmpGreater) == (d < dObj));   // in front of the object: drawn; behind: discarded
        }
    }
    // (7) lines
    g::FrameState fs; fs.begin(true); fs.replays = fs.committed = fs.refused = 999999; fs.last_why = "sprite-scissor-not-image";
    char line[600]; g::format_frame(line, sizeof line, 999999999ull, fs, 999999, 999999, 999999); CHECK(std::strlen(line) < kLineCap);
    CHECK(std::string(line).find("replays=99999(explosion:99999,sprite:99999);committed=99999;refused=99999;window-fallback=99999;") != std::string::npos);
    g::format_ring(line, sizeof line, false, "sprite-over-total-cap", 3, 10, 16384, 16384, ~0ull); CHECK(std::strlen(line) + 40 < kLineCap);   // + ";created=late(...)"
    std::printf("  sprite gpu: copies within %.4f GS px of the eye projection; glow kept by the sprite composite, dropped by the smoke composite\n", worst);
}
// ------------------------------------------------------------------ fxsig:<ps8> per-draw log (docs section 12)
void test_sig_log() {
    using namespace fxcensus;
    // policy: every draw of an onset window is logged (uncapped), 3 s after the onset, a new onset after > 30 quiet frames
    SigLog g; unsigned nth = 0;
    CHECK(g.admit(100, 10000, nth) && nth == 0 && g.onsets == 1);
    CHECK(g.admit(100, 10001, nth) && nth == 1);
    for (unsigned i = 2; i < 40; ++i) CHECK(g.admit(100, 10002, nth) && nth == i);   // far above the census cap of 12: all admitted
    CHECK(g.admit(101, 10016, nth) && nth == 0 && g.onsets == 1);                     // next frame: index restarts, same onset
    CHECK(g.admit(160, 13000, nth) && g.onsets == 2 && nth == 0);                      // 59 frames after frame 101 (> 30 quiet): a NEW onset, logged
}
void test_sig_log_onsets() {
    using namespace fxcensus;
    SigLog g; unsigned nth = 0;
    CHECK(g.admit(10, 1000, nth) && g.onsets == 1);
    CHECK(g.admit(40, 1500, nth) && g.onsets == 1);                                    // 30 frames later (29 quiet frames): same onset
    CHECK(g.admit(41, 4000, nth) && g.onsets == 1);                                    // 3000 ms after the onset (1000): inside
    CHECK(!g.admit(42, 4001, nth) && g.suppressed == 1);                               // past 3 s: not logged
    CHECK(!g.admit(43, 9000, nth));
    CHECK(g.admit(75, 9500, nth) && g.onsets == 2);                                    // 32 frames after frame 43 (> 30 quiet frames): a new onset, 3 s of logging again
    CHECK(g.admit(76, 12500, nth) && !g.admit(77, 12501, nth));
}
fxcensus::SigDraw sample_draw() {
    fxcensus::SigDraw d; d.frame = 123456; d.ord = 1234; d.nth = 3; d.ps8 = 0xd34ee153u; d.vs8 = 0x534f29cau; d.indexed = true; d.count = 24; d.instances = 1; d.start = 0; d.base = 0;
    d.vb_va = 0x7ff612345678ull; d.vb_res = 0x1a2b3c4d5e6full; d.vb_bytes = 65536; d.ib_va = 0x7ff612399000ull; d.vb_stride = 64; d.ib_stride = 2; d.topology = 5;
    const uint8_t b[6] = {5, 2, 1, 1, 2, 1}; std::memcpy(d.blend, b, 6); d.write_mask = 15; d.pso_known = d.blend_on = d.depth_enable = d.stencil = true; d.depth_func = 5;
    d.tracer_ready = true; d.nidx = 4; for (unsigned i = 0; i < 4; ++i) d.idx[i] = i + 20000; d.nvtx = 4;
    for (unsigned i = 0; i < 4; ++i) { d.pos[i][0] = 2012.5f + float(i); d.pos[i][1] = 1798.25f; d.pos[i][2] = 1.2345e7f; d.pos[i][3] = 0.01234f; }
    std::snprintf(d.roots, sizeof d.roots, "0:C0[2]=1f/-,1:S10=7ff612000000,2:T=0,3:T=0,4:T=0");
    return d;
}
void test_sig_log_lines() {
    using namespace fxcensus;
    char b[SigQueue::kLine];
    SigDraw d = sample_draw();
    const int n = format_sig_draw(b, sizeof b, d, 0);
    CHECK(size_t(n) == std::strlen(b) && size_t(n) < SigQueue::kLine && SigQueue::kLine + 21 <= 448 && SigQueue::kLine + 21 < 480);
    const std::string s = b;
    CHECK(s.find("sig=psd34ee153/vs534f29ca") != std::string::npos && s.find("idx=24") != std::string::npos && s.find("vb=7ff612345678(res:1a2b3c4d5e6f,s64)") != std::string::npos);
    CHECK(s.find("bl=on:5,2,1/1,2,1:mf") != std::string::npos && s.find("d=f5:w0") != std::string::npos && s.find("topo=5") != std::string::npos);
    CHECK(s.find("i=20000,20001,20002,20003;") != std::string::npos && s.find("v0=2012.5,1798.25,1.2345e+07,0.01234;") != std::string::npos);
    CHECK(s.find("roots=0:C0[2]=1f/-") != std::string::npos);
    // extreme values never overflow (the line is cut, never over the buffer)
    SigDraw x = sample_draw(); x.frame = x.ord = ~0ull; x.nth = ~0u; x.count = x.instances = x.start = ~0u; x.base = INT32_MIN; x.vb_va = x.vb_res = ~0ull; x.vb_stride = ~0u;
    for (unsigned i = 0; i < 4; ++i) for (int c = 0; c < 4; ++c) x.pos[i][c] = -3.4028235e38f;
    for (unsigned i = 0; i < 4; ++i) x.idx[i] = ~0u;
    std::memset(x.roots, 'r', sizeof x.roots - 1); x.roots[sizeof x.roots - 1] = 0; x.gate = "class-explosion-needs-dual-and-more-text";
    char bx[SigQueue::kLine]; std::memset(bx, 'Z', sizeof bx);
    const int nx = format_sig_draw(bx, sizeof bx, x, ~0ull);
    CHECK(nx > 0 && size_t(nx) < SigQueue::kLine && bx[nx] == 0 && std::strlen(bx) == size_t(nx) && std::strncmp(bx, "fx-sig-draw;", 12) == 0);
    // a failed read names its reason
    SigDraw f = sample_draw(); f.nvtx = 0; f.read_why = "map-unmap-coverage-incomplete";
    format_sig_draw(b, sizeof b, f, 3); CHECK(std::string(b).find("vtx-why=map-unmap-coverage-incomplete;") != std::string::npos && std::string(b).find(";dropped=3") != std::string::npos);
    // the realistic line leaves room for the dropped counter and the roots (the tail is what a long line loses)
    CHECK(std::strlen(b) < SigQueue::kLine - 20);
}
void test_sig_queue() {
    using namespace fxcensus;
    static SigQueue q;   // 17 KB
    SigDraw d = sample_draw();
    for (unsigned i = 0; i < SigQueue::kCap; ++i) { d.nth = i; CHECK(q.push(d)); }
    CHECK(!q.push(d) && !q.push(d) && q.dropped == 2 && q.count == SigQueue::kCap);   // full: dropped, counted
    CHECK(std::string(q.front()).find(";n=0;") != std::string::npos); q.pop();
    CHECK(q.push(d));                                                                 // room again; its line reports the 2 dropped lines
    for (unsigned i = 1; i < SigQueue::kCap - 1; ++i) q.pop();
    CHECK(q.count == 2); q.pop(); CHECK(std::string(q.front()).find(";dropped=2") != std::string::npos); q.pop(); CHECK(!q.front() && q.count == 0);
}
// ------------------------------------------------------------------ EFFECTS-OUTSIDE (G) the gun tracer class (docs section 12)
fxmem::Vtx gsv(double x, double y) { fxmem::Vtx v{}; v.pos[0] = float(x); v.pos[1] = float(y); v.pos[2] = 1.0e6f; v.pos[3] = 1.0f; return v; }
double px_dist(const fxmem::Vtx& a, const fxmem::Vtx& b, double gx, double gy) { return std::hypot((a.pos[0] - b.pos[0]) / gx, (a.pos[1] - b.pos[1]) / gy); }
void test_tracer_class() {
    using namespace gpu_copy::step5;
    // (1) blend tuple + clone: the additive tracer keeps the game's factors; nothing else becomes a clone tuple
    uint32_t sa = 0, da = 0;
    CHECK(effects_tracer_blend_tuple(5, 2, 1, 1, 2, 1, 15) && effects_clone_alpha_factors(5, 2, 1, 1, 2, 1, 15, sa, da) && sa == 1 && da == 2);
    CHECK(!effects_tracer_blend_tuple(5, 2, 1, 1, 2, 1, 7) && !effects_tracer_blend_tuple(5, 2, 1, 18, 19, 1, 15) && !effects_tracer_blend_tuple(5, 6, 1, 1, 2, 1, 15));
    CHECK(!effects_clone_alpha_factors(5, 2, 1, 1, 2, 1, 7, sa, da));
    CHECK(effects_ps_tracer == effects_ps_deny && effects_vs_sprite == 0x534f29cau);
    // (2) the class gate: the HUD pixel shader is admitted ONLY as the tracer (fxmem:6 flag, key, exact tuple PSO, sprite VS)
    gpu_copy::step3::Shape s; s.pso.known = true; s.pso.blend0 = true; s.pso.write_mask0 = 15; s.pso.depth_enable = true; s.pso.depth_func = gpu_copy::step3::cmp::greater;
    s.pso.dual_ready = s.pso.tracer_ready = true;
    HudTarget t; const EffectsAllow allow = EffectsAllow::defaults();
    EffectsClassIn cin; cin.allow = &allow; cin.ps8 = effects_ps_deny; cin.vs8 = effects_vs_sprite; cin.dual = true; cin.tracer = true;
    auto why = [&]() { const EffectsVerdict v = effects_decide(s, t, effects_open, 0, &cin); return std::string(v.why ? v.why : "ok"); };
    CHECK(why().rfind("class-", 0) != 0);                                   // past the class gate
    cin.tracer = false; CHECK(why() == "class-hud-sprite");                  // fxmem:5 and below: as before
    cin.tracer = true; cin.dual = false; CHECK(why() == "class-hud-sprite");
    cin.dual = true; cin.vs8 = 0x12345678u; CHECK(why() == "class-hud-sprite");
    cin.vs8 = effects_vs_sprite; s.pso.tracer_ready = false; CHECK(why() == "class-hud-sprite");   // any other d34ee153 PSO (a HUD sprite blend)
    s.pso.tracer_ready = true; s.pso.depth_func = gpu_copy::step3::cmp::always; CHECK(why() != "ok" && why().rfind("class-", 0) == 0);
    s.pso.depth_func = gpu_copy::step3::cmp::greater; s.pso.depth_write = true; CHECK(why() == "class-state-not-effect");
    s.pso.depth_write = false;
    // the tracer-tuple PSO is not a clone tuple for any OTHER candidate (default behaviour unchanged): refused as before
    cin.ps8 = effects_ps_cloud; cin.tracer = true;
    CHECK(why() == "dual-blend-unsupported" || why().rfind("class-", 0) != 0);   // past the class gate; then the tracer-only clone does not serve it
    { EffectsClassIn c2 = cin; c2.tracer = false; const EffectsVerdict v = effects_decide(s, t, effects_open, 0, &c2); if (!v.why || std::string(v.why).rfind("class-", 0) != 0) CHECK(v.kind != effects_ok); }
    // (3) minimum width: a thin quad grows to >= 1 eye pixel across its short side, length / centre unchanged; wide quads, specks and collapsed quads untouched
    const double gx = 512.0 / 2560, gy = 448.0 / 1440;
    const uint16_t idx[4] = {0, 1, 2, 3};
    {
        fxmem::Vtx v[4] = {gsv(2000, 2000), gsv(2000.04, 2000), gsv(2000, 2008), gsv(2000.04, 2008)};   // 0.2 px wide, 25 px long (U short)
        fxmem::Vtx o[4]; std::memcpy(o, v, sizeof v);
        CHECK(fxsprite::widen_thin_quads(v, 4, idx, 4, fxsprite::Widen{gx, gy, 1.0}) == 1);
        CHECK(std::fabs(px_dist(v[0], v[1], gx, gy) - 1.0) < 3e-3 && std::fabs(px_dist(v[2], v[3], gx, gy) - 1.0) < 3e-3);
        CHECK(std::fabs(px_dist(v[0], v[2], gx, gy) - px_dist(o[0], o[2], gx, gy)) < 3e-3);                // length unchanged
        CHECK(std::fabs((v[0].pos[0] + v[1].pos[0]) / 2 - 2000.02) < 1e-3 && std::fabs(v[0].pos[1] - 2000.0) < 3e-3);   // centre line kept
        CHECK(v[1].pos[0] > v[0].pos[0] && v[0].pos[2] == o[0].pos[2] && v[0].pos[3] == o[0].pos[3] && std::memcmp(v[2].col, o[2].col, sizeof o[2].col) == 0);   // orientation, z, q, colours
        CHECK(fxsprite::widen_thin_quads(v, 4, idx, 4, fxsprite::Widen{gx, gy, 1.0}) == 0);               // already 1 px: idempotent
    }
    {   // V short (a strip whose first edge is the long one), diagonal orientation
        const double c = std::cos(0.6), sn = std::sin(0.6), L = 30 * gx, W = 0.1 * gx;   // 30 px long along (c, sn), 0.1 px wide across
        auto at = [&](double l, double w) { return gsv(2000 + l * c - w * sn, 2000 + l * sn + w * c); };
        fxmem::Vtx v[4] = {at(0, 0), at(L, 0), at(0, W), at(L, W)};
        CHECK(fxsprite::widen_thin_quads(v, 4, idx, 4, fxsprite::Widen{gx, gy, 1.0}) == 1);
        CHECK(std::fabs(px_dist(v[0], v[2], gx, gx) - 1.0) < 0.02 || std::fabs(px_dist(v[0], v[2], gx, gy) - 1.0) < 0.3);   // anisotropic pixels: width ~ 1 px
        CHECK(std::fabs(v[0].pos[0] - v[2].pos[0]) + std::fabs(v[0].pos[1] - v[2].pos[1]) > 0.0);
    }
    {
        fxmem::Vtx wide[4] = {gsv(2000, 2000), gsv(2002, 2000), gsv(2000, 2008), gsv(2002, 2008)};      // 10 px wide
        fxmem::Vtx speck[4] = {gsv(2000, 2000), gsv(2000.01, 2000), gsv(2000, 2000.02), gsv(2000.01, 2000.02)};   // 0.05 x 0.06 px: no long side
        fxmem::Vtx coll[4] = {gsv(2000, 2000), gsv(2000, 2000), gsv(2000, 2000), gsv(2000, 2000)};
        fxmem::Vtx a[4], b[4], c[4]; std::memcpy(a, wide, sizeof a); std::memcpy(b, speck, sizeof b); std::memcpy(c, coll, sizeof c);
        const fxsprite::Widen w{gx, gy, 1.0};
        CHECK(fxsprite::widen_thin_quads(a, 4, idx, 4, w) == 0 && std::memcmp(a, wide, sizeof a) == 0);
        CHECK(fxsprite::widen_thin_quads(b, 4, idx, 4, w) == 0 && std::memcmp(b, speck, sizeof b) == 0);
        CHECK(fxsprite::widen_thin_quads(c, 4, idx, 4, w) == 0 && std::memcmp(c, coll, sizeof c) == 0);
        CHECK(fxsprite::widen_thin_quads(a, 4, idx, 4, fxsprite::Widen{}) == 0);                            // off (gx = 0)
        CHECK(fxsprite::widen_thin_quads(nullptr, 4, idx, 4, w) == 0 && fxsprite::widen_thin_quads(a, 3, idx, 4, w) == 0);   // index out of range: skipped
        // two quads with a restart: both widened, one by one
        fxmem::Vtx two[8] = {gsv(2000, 2000), gsv(2000.04, 2000), gsv(2000, 2008), gsv(2000.04, 2008), gsv(2020, 2000), gsv(2020.04, 2000), gsv(2020, 2008), gsv(2020.04, 2008)};
        const uint16_t i9[9] = {0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7};
        CHECK(fxsprite::widen_thin_quads(two, 8, i9, 9, w) == 2);
    }
    // (4) plan_draw with the minimum width: a far thin tracer (150 x 0.04 units, 400 ahead) is < 1 eye px wide un-widened, >= 1 px with the option
    {
        const Cam g{{1000, 4000, 2000}, deg(30), deg(-5)};
        double ag[16]; make_a(g, ag); fxmem::ScreenMap gm; CHECK(fxsprite::game_map_from_a(ag, gm));
        double f[3], r[3], u[3]; axes(g.yaw, g.pitch, f, r, u);
        double base[3]; for (int i = 0; i < 3; ++i) base[i] = g.e[i] + 400 * f[i] + 10 * r[i] - 8 * u[i];
        const double off[4][2] = {{-0.02, 0}, {0.02, 0}, {-0.02, 150}, {0.02, 150}};
        fxmem::Vtx v[4];
        for (int k = 0; k < 4; ++k) {
            double w[3]; for (int i = 0; i < 3; ++i) w[i] = base[i] + off[k][0] * r[i] + off[k][1] * f[i];
            double xs, ys, d; CHECK(fxsprite::sp_project(gm, w, xs, ys, d));
            v[k] = gsv(xs, ys); v[k].pos[2] = float(fxmem::game_z_for_depth(d)); v[k].col[0] = 1.0f;
        }
        double a0[16]; make_a(g, a0); fxmem::ScreenMap e0; CHECK(fxsprite::eye_map_from_a(a0, e0));
        const fxmem::ScreenMap* eyes[2] = {&e0, &e0};
        fxmem::Vtx scratch[4], k0[4], k1[4], n0[4], n1[4]; uint8_t bad[4];
        fxsprite::Plan p; p.begin();
        fxsprite::plan_draw(gm, eyes, v, 4, idx, 4, 0, 1.0, scratch, bad, p, n0, n1);
        CHECK(p.prepared == 1 && p.widened == 0 && px_dist(n0[0], n0[1], gx, gy) < 1.0);              // without the option: thinner than a pixel
        const fxsprite::Widen w{gx, gy, 1.0}; p.begin();
        fxsprite::plan_draw(gm, eyes, v, 4, idx, 4, 0, 1.0, scratch, bad, p, k0, k1, &w);
        // each END of the streak (near a-b, far c-d) is >= 1 px wide (the pair distance also holds the shear of the projected quad: a few % above 1)
        CHECK(p.prepared == 1 && p.widened == 2 && px_dist(k0[0], k0[1], gx, gy) >= 0.999 && px_dist(k0[0], k0[1], gx, gy) < 1.05 &&
              px_dist(k1[2], k1[3], gx, gy) >= 0.999 && px_dist(k1[2], k1[3], gx, gy) < 1.05);
        CHECK(std::fabs(px_dist(k0[0], k0[2], gx, gy) - px_dist(n0[0], n0[2], gx, gy)) < 0.05 * px_dist(n0[0], n0[2], gx, gy));          // length unchanged (within shear)
        char sl[600]; fxsprite::format_plan(sl, sizeof sl, 9, "tracer", p, 0); CHECK(std::string(sl).find(";wide=2") != std::string::npos && std::strlen(sl) < kLineCap);
        std::printf("  tracer: 150 x 0.04 unit streak at 400 units: %.3f eye px wide as re-projected, widened to %.3f\n", px_dist(n0[0], n0[1], gx, gy), px_dist(k0[0], k0[1], gx, gy));
    }
    // (5) the GPU gate: a non-indexed quad is replayed only for the tracer class (allow_nonindexed), everything else as before
    {
        namespace g = fxsprite::gpu; namespace sg = acz::fxsmoke::gpu;
        g::GateIn ok; ok.ready = ok.effects_recorded = ok.dual = ok.prepared = true; ok.vertices = 4; ok.draw_index = 0; ok.indexed = false; ok.allow_nonindexed = true;
        ok.vb_stride = 64; ok.instances = 1; ok.vb_va = 0x7000; ok.vb_bytes = 65536; ok.eye_depth = true; ok.eye_dsv = ok.pso_dsv = 20; ok.pso_rtv0 = ok.image_format = 10;
        ok.pso_samples = 1; ok.pso_rt_count = 1; ok.depth_enable = true; ok.depth_func = sg::kCmpGreater; ok.stencil = true; ok.stencil_noop = true;
        ok.vp_known = true; ok.viewports = 1; ok.vp[2] = 2560; ok.vp[3] = 1440; ok.image_w = 2560; ok.image_h = 1440; ok.scissors = 1; ok.sc[2] = 2560; ok.sc[3] = 1440;
        CHECK(g::gate(ok) == nullptr);
        g::GateIn x = ok; x.allow_nonindexed = false; CHECK(std::string(g::gate(x)) == "sprite-not-indexed-r16");
        x = ok; x.indexed = true; x.ib_stride = 4; CHECK(std::string(g::gate(x)) == "sprite-not-indexed-r16");
        x = ok; x.indexed = true; x.ib_stride = 2; CHECK(g::gate(x) == nullptr);
    }
    // (6) (J, headset 2026-10-29 items 5/7, m02a AND m03k logs) the NON-indexed GREATER 4caf6e2d quads are class 1 with the per-eye replay on (fxmem:6), so they
    // leave the window effects image (window-only clouds, under the objects); fxmem:5 (gpu off) keeps the old indexed-only rule; the cloud cards
    // (GREATER_EQUAL) and every other class are unchanged.
    {
        namespace s5 = gpu_copy::step5; namespace cmp = gpu_copy::step3::cmp;
        const uint32_t cloud = s5::effects_ps_cloud, xpl = s5::effects_ps_explosion, trc = s5::effects_ps_tracer, vs = s5::effects_vs_sprite;
        CHECK(s5::effects_sprite_class(cloud, vs, true, cmp::greater, false, false) == 1);    // indexed: both modes
        CHECK(s5::effects_sprite_class(cloud, vs, true, cmp::greater, false, true) == 1);
        CHECK(s5::effects_sprite_class(cloud, vs, false, cmp::greater, false, true) == 1);    // NEW: non-indexed quad, fxmem:6
        CHECK(s5::effects_sprite_class(cloud, vs, false, cmp::greater, false, false) == -1);  // fxmem:5 dry run: unchanged
        CHECK(s5::effects_sprite_class(cloud, vs, false, cmp::greater_equal, false, true) == -1 && s5::effects_sprite_class(cloud, vs, true, cmp::greater_equal, false, true) == -1);
        CHECK(s5::effects_sprite_class(cloud, vs, false, cmp::always, false, true) == -1);
        CHECK(s5::effects_sprite_class(xpl, vs, false, cmp::greater, false, false) == 0 && s5::effects_sprite_class(xpl, vs, true, cmp::greater, false, true) == 0);
        CHECK(s5::effects_sprite_class(trc, vs, false, cmp::greater, true, true) == 2 && s5::effects_sprite_class(trc, vs, false, cmp::greater, true, false) == -1);
        CHECK(s5::effects_sprite_class(trc, vs, true, cmp::greater, false, true) == -1 && s5::effects_sprite_class(trc, 0x12345678u, true, cmp::greater, true, true) == -1);
        CHECK(s5::effects_sprite_class(0xd2464ed2u, 0x9eff1f92u, false, cmp::greater, false, true) == -1);   // missile smoke: its own path
        CHECK(!s5::effects_sprite_nonindexed_class(-1) && !s5::effects_sprite_nonindexed_class(0) && s5::effects_sprite_nonindexed_class(1) && s5::effects_sprite_nonindexed_class(2));
        // w12-2 finding 1: a non-indexed capture needs the proved TRIANGLESTRIP (5); a TRIANGLELIST (4: only triangle 0-1-2 of 4 vertices is drawn) or anything else is refused
        CHECK(s5::effects_sprite_nonindexed_topology_ok(5) && !s5::effects_sprite_nonindexed_topology_ok(4) && !s5::effects_sprite_nonindexed_topology_ok(0) && !s5::effects_sprite_nonindexed_topology_ok(3));
        // the plan line: suffix only when a non-indexed draw was captured (indexed-only lines byte-identical), longest quad edge, within the line cap
        fxsprite::Plan p; p.begin(); p.draws = 1; char a[600], b[600];
        fxsprite::format_plan(a, sizeof a, 5, "sprite(tracer-cand)", p, 0);
        CHECK(std::string(a).find(";ni=") == std::string::npos);
        p.nonindexed = 5; p.quad_len_max = 21000.4; fxsprite::format_plan(b, sizeof b, 5, "sprite(tracer-cand)", p, 0);
        CHECK(std::string(b) == std::string(a) + ";ni=5;qm=21000");
        fxsprite::Plan w; w.draws = w.read = w.prepared = w.refused = 99999; w.vertices = w.quads = w.in_window = w.outside_window = w.z_form = w.q_form = w.no_depth = 9999999;
        w.eye_visible[0] = w.eye_visible[1] = w.eye_only = w.behind = w.collapsed = 9999999; w.last_why = "sprite-world-camera-unavailable";
        w.streaks = 9999999; w.streak_rgb[0] = w.streak_rgb[1] = w.streak_rgb[2] = 9999999.0 * 2; w.near_depth = w.streak_near = w.streak_len_max = 99999;
        w.widened = w.nonindexed = 99999; w.quad_len_max = 1e30;   // clamped in the line: ni <= 9999, qm <= 999999
        fxsprite::format_plan(b, sizeof b, 999999999ull, "sprite(tracer-cand)", w, 99999);
        std::printf("  plan line with ni/qm at maxima: %zu bytes (cap %zu)\n", std::strlen(b), kLineCap);
        CHECK(std::strlen(b) < kLineCap && std::string(b).find(";ni=9999;qm=999999") != std::string::npos);
    }
}
} // namespace

// EFFECTS-OUTSIDE (K, headset 2026-10-30 m03k; docs/world-render/effects-outside-window.md section 17): the clouds stayed in the forward rectangle.
namespace k_cloud {
namespace g = fxsprite::gpu; namespace sg = acz::fxsmoke::gpu; namespace s5 = gpu_copy::step5;
// One per-eye sprite gate input as the producer builds it for a prepared 4-vertex class-1 quad (the m03k dense frames: 85-120 such draws).
g::GateIn quad_gate(unsigned draw_index, uint64_t span_used, bool explosion = false) {
    g::GateIn x; x.ready = x.effects_recorded = x.dual = x.prepared = true; x.vertices = 4; x.draw_index = draw_index; x.span_used = span_used; x.explosion = explosion;
    x.indexed = false; x.allow_nonindexed = true; x.vb_stride = 64; x.instances = 1; x.vb_va = 0x7000; x.vb_bytes = 65536; x.eye_depth = true; x.eye_dsv = x.pso_dsv = 20;
    x.pso_rtv0 = x.image_format = 10; x.pso_samples = 1; x.pso_rt_count = 1; x.depth_enable = true; x.depth_func = sg::kCmpGreater; x.vp_known = true;
    x.viewports = 1; x.vp[2] = 2560; x.vp[3] = 1440; x.image_w = 2560; x.image_h = 1440; x.scissors = 1; x.sc[2] = 2560; x.sc[3] = 1440;
    return x;
}
// (1) A dense cloud frame: the effects layer accepts up to effects_draw_cap draws per frame; every accepted class-1 quad must be replayed per eye. Before (K)
// the sprite / tracer classes stopped at kMaxDraws - reserve = 30 and draw #31.. fell back to the WINDOW image (`sprite-gpu-draw-cap`, the m03k fallbacks).
void test_dense_frame_all_per_eye() {
    unsigned fallback = 0, replayed = 0; uint64_t cur = 0; const char* first = nullptr;
    const unsigned accepted = s5::effects_draw_cap - g::kExplosionReserveDraws;   // the effects layer's per-frame cap, leaving the explosion reserve
    for (unsigned i = 0; i < accepted; ++i) {
        const char* why = g::gate(quad_gate(replayed, cur));
        g::EyeViews ev;
        if (!why && !g::eye_views(0x10000, cur, 4, 0, ev)) why = "sprite-eye-view";
        if (why) { ++fallback; if (!first) first = why; continue; }
        cur += ev.used; ++replayed;
    }
    std::printf("  (K) dense frame: %u accepted class-1 quads -> %u per eye, %u window fallbacks (first %s), span %llu B\n", accepted, replayed, fallback,
                first ? first : "none", static_cast<unsigned long long>(cur));
    CHECK(fallback == 0 && replayed == accepted);
    CHECK(g::gate(quad_gate(replayed, cur, true)) == nullptr);   // the explosion reserve is still there after them
}
// (2) Collapse of a run that is not a 4-vertex quad list (a plain strip of 6 vertices): a vertex behind the eye collapses the WHOLE run (zero area). Before
// (K) the run kept its bad vertex (re-projected behind the eye): a triangle stretched across the eye view.
void test_odd_run_collapse() {
    fxmem::Vtx out[6]; for (int i = 0; i < 6; ++i) { out[i] = fxmem::Vtx{}; out[i].pos[0] = 1900.0f + 10.0f * float(i); out[i].pos[1] = 2000.0f + float(i % 2); out[i].pos[3] = 1.0f; }
    uint8_t bad[6] = {0, 0, 0, 1, 0, 0};
    const uint16_t idx[6] = {0, 1, 2, 3, 4, 5};
    fxsprite::ReprojectStats st; fxsprite::collapse_quads(out, 6, bad, idx, 6, st);
    CHECK(st.quads == 0 && st.bad_index == 1);   // not a quad list (counted as before)
    for (int i = 1; i < 6; ++i) CHECK(std::memcmp(out[i].pos, out[0].pos, sizeof out[0].pos) == 0);
    // a clean run is left exactly as it is
    fxmem::Vtx keep[6]; for (int i = 0; i < 6; ++i) { keep[i] = fxmem::Vtx{}; keep[i].pos[0] = float(i); keep[i].pos[3] = 1.0f; }
    uint8_t none[6] = {}; fxsprite::ReprojectStats s2; fxsprite::collapse_quads(keep, 6, none, idx, 6, s2);
    for (int i = 0; i < 6; ++i) CHECK(keep[i].pos[0] == float(i));
    CHECK(st.runs_collapsed == 1 && s2.runs_collapsed == 0);
}
// (3) The non-indexed shape rule: class 1 takes strips of any count 3..512 and lists of 3n; the tracer keeps the strip-quad rule; caps and instancing fail closed.
void test_nonindexed_shape_table() {
    auto why = [](int c, uint32_t n, uint32_t inst, uint32_t topo, bool* quads = nullptr) {
        bool q = true; const char* w = s5::effects_sprite_nonindexed_shape(c, n, inst, topo, fxsprite::kMaxVertices, q); if (quads) *quads = q;
        return std::string(w ? w : "none");
    };
    bool q = false;
    CHECK(why(1, 4, 1, 5, &q) == "none" && q);          // the J quad: quad list
    CHECK(why(1, 8, 1, 5, &q) == "none" && !q);         // W15 (w15-2 F1): a longer strip is ONE continuous GPU assembly (no restarts exist on the GPU): whole-run rule
    CHECK(why(1, 12, 1, 5, &q) == "none" && !q);
    CHECK(why(1, 12, 1, 4, &q) == "none" && !q);        // W15: a list of 12 is four triangles, never three quads
    CHECK(why(1, 6, 1, 5, &q) == "none" && !q);         // (K) a 6-vertex strip: one run
    CHECK(why(1, 3, 1, 5, &q) == "none" && !q);
    CHECK(why(1, 6, 1, 4, &q) == "none" && !q);         // (K) a 2-triangle list: one run
    CHECK(why(1, 4, 1, 4) == "sprite-nonindexed-shape"); // a list of 4 is not whole triangles
    CHECK(why(1, 2, 1, 5) == "sprite-nonindexed-shape" && why(1, 0, 1, 5) == "sprite-nonindexed-shape");
    CHECK(why(1, fxsprite::kMaxVertices + 1, 1, 5) == "sprite-nonindexed-shape" && why(1, 6, 2, 5) == "sprite-nonindexed-shape");
    CHECK(why(1, 6, 1, 0) == "sprite-nonindexed-topology" && why(1, 6, 1, 1) == "sprite-nonindexed-topology");
    CHECK(why(2, 4, 1, 5, &q) == "none" && q);          // tracer: unchanged
    CHECK(why(2, 6, 1, 5) == "sprite-nonindexed-shape" && why(2, 4, 1, 4) == "sprite-nonindexed-topology" && why(2, 6, 1, 4) == "sprite-nonindexed-shape");
    // the class totals line names refused non-indexed shapes (suffix only when some were refused: otherwise byte-identical); the plan line is unchanged
    fxsprite::Plan p; p.begin(); p.draws = 1; char a[600], b[600], t[448];
    fxsprite::format_plan(a, sizeof a, 5, "sprite(tracer-cand)", p, 0);
    p.note_nonindexed_refused(600, 5); p.note_nonindexed_refused(10, 4);
    fxsprite::format_plan(b, sizeof b, 5, "sprite(tracer-cand)", p, 0);
    CHECK(std::string(b) == std::string(a));
    const char* totals = "fx-sprite-totals;class=sprite(tracer-cand);frames=1;read=1;refused=0;caps=idx:1536,vtx:512;mode=plan+gpu-replay(per-eye,fx-sprite-gpu-lines)";
    std::snprintf(t, sizeof t, "%s", totals); fxsprite::Plan none; none.begin(); fxsprite::append_nonindexed_refused(t, sizeof t, none);
    CHECK(std::string(t) == totals);
    fxsprite::append_nonindexed_refused(t, sizeof t, p);
    CHECK(std::string(t) == std::string(totals) + ";nir=2(c:10,t:4)");
    fxsprite::Plan w; w.ni_refused = 99999; w.ni_last_count = 999999; w.ni_last_topology = 999;
    std::snprintf(t, sizeof t, "gpu-copy-step1;event=fx-sprite-totals;class=sprite(tracer-cand);frames=99999999999999;read=99999999999999;refused=99999999999999;"
                               "caps=idx:1536,vtx:512;mode=plan+gpu-replay(per-eye,fx-sprite-gpu-lines)");
    fxsprite::append_nonindexed_refused(t, sizeof t, w);
    std::printf("  (K) totals line with nir at maxima: %zu bytes (cap %zu)\n", std::strlen(t), kLineCap);
    CHECK(std::strlen(t) < kLineCap && std::string(t).find(";nir=9999(c:99999,t:99)") != std::string::npos);
}
} // namespace k_cloud

// EFFECTS-OUTSIDE (P, docs/world-render/effects-outside-window.md section 21) fxadd:1: the additive 4caf6e2d family (5,2,1/18,19,1) through its private clone and its
// one-vertex POINT lists per eye. Default (fxadd off) byte-identical: the same verdicts, shapes, lines.
namespace p_points {
namespace g = fxsprite::gpu; namespace sg = acz::fxsmoke::gpu; namespace s5 = gpu_copy::step5; namespace cmp = gpu_copy::step3::cmp;
// (1) the tuple and the clone factors
void test_additive_tuple() {
    uint32_t sa = 9, da = 9;
    CHECK(s5::effects_additive_blend_tuple(5, 2, 1, 18, 19, 1, 15));
    CHECK(!s5::effects_additive_blend_tuple(5, 2, 1, 1, 2, 1, 15) && !s5::effects_additive_blend_tuple(5, 6, 1, 18, 19, 1, 15) && !s5::effects_additive_blend_tuple(5, 5, 1, 18, 19, 1, 15));
    CHECK(!s5::effects_additive_blend_tuple(5, 2, 1, 18, 19, 1, 7) && !s5::effects_additive_blend_tuple(5, 2, 2, 18, 19, 1, 15));
    CHECK(s5::effects_clone_alpha_factors(5, 2, 1, 18, 19, 1, 15, sa, da) && sa == 1 && da == 2);   // ZERO / ONE: A' = A (light adds no coverage)
    // the other tuples keep their factors exactly
    CHECK(s5::effects_clone_alpha_factors(5, 6, 1, 18, 19, 1, 15, sa, da) && sa == 2 && da == 6);
    CHECK(s5::effects_clone_alpha_factors(5, 5, 1, 18, 19, 1, 15, sa, da) && sa == 1 && da == 2);
    CHECK(s5::effects_clone_alpha_factors(5, 2, 1, 1, 2, 1, 15, sa, da) && sa == 1 && da == 2);
    CHECK(!s5::effects_dual_blend_tuple(5, 2, 1, 18, 19, 1, 15) && !s5::effects_tracer_blend_tuple(5, 2, 1, 18, 19, 1, 15));
    // the additive clone over a background: the effects pixel adds a*t, coverage unchanged, so e + (1 - A) world = the game's a*t + D (exact, no loss)
    for (int k = 1; k <= 255; k += 9) {
        const double a = k / 255.0, t = 0.9, world = 0.3, under = 0.15, underA = 0.25;
        const double game = a * t + (under + (1 - underA) * world);
        const double e = a * t + under, A = 0 * 1 + 1 * underA, comp = e + (1 - A) * world;
        CHECK(A == underA && std::fabs(game - comp) < 1e-12);
    }
}
// A Shape that passes hud_decide (the gpu_copy_step1 dual-rules fixture), PS 4caf6e2d GREATER, blend on.
gpu_copy::step3::Shape passing_shape(s5::HudTarget& t) {
    gpu_copy::step3::Shape s; s.tracking_complete = true; s.root_route = 0x10; s.pso.known = true; s.pso.rt_count = 1; s.pso.rtv[0] = 10; s.pso.dsv = 20; s.pso.samples = 1;
    s.om_known = s.om_set = true; s.om_count = 1; s.om_desc_proved = true; s.game_rt_status = "resolved"; s.game_rt = 0x5000; s.game_rt_format = 10;
    s.game_rt_width = 2560; s.game_rt_height = 1440; s.pso.depth_enable = true; s.pso.depth_func = cmp::greater; s.pso.blend0 = true; s.pso.write_mask0 = 15;
    t.format = 10; t.ds_format = 20; t.width = 2560; t.height = 1440; t.scene = 0x5000;
    return s;
}
// (2) the class gate: an additive_ready PSO is refused as dual-blend-unsupported exactly as a clone-less one unless fxadd is on; dual_ready PSOs are unaffected
void test_additive_gate() {
    s5::HudTarget t; gpu_copy::step3::Shape s = passing_shape(t);
    const s5::EffectsAllow allow = s5::EffectsAllow::defaults();
    s5::EffectsClassIn cin; cin.allow = &allow; cin.ps8 = s5::effects_ps_cloud; cin.vs8 = s5::effects_vs_sprite; cin.dual = true;
    auto v = [&](const gpu_copy::step3::Shape& x, const s5::EffectsClassIn& c, unsigned draws = 0) { return s5::effects_decide(x, t, s5::effects_open, draws, &c); };
    auto same = [](const s5::EffectsVerdict& a, const s5::EffectsVerdict& b) {
        return a.kind == b.kind && a.category == b.category && a.depth_tested == b.depth_tested && std::string(a.why ? a.why : "") == std::string(b.why ? b.why : "");
    };
    gpu_copy::step3::Shape before = s;                    // the pre-(P) PSO: no clone, dual_ready false
    gpu_copy::step3::Shape add = s; add.pso.additive_ready = true;   // the (P) PSO: clone exists, dual_ready still false
    const s5::EffectsVerdict v0 = v(before, cin);
    CHECK(v0.kind == s5::effects_refused && std::string(v0.why) == "dual-blend-unsupported");
    CHECK(same(v(add, cin), v0));                         // fxadd off: byte-identical verdict
    s5::EffectsClassIn on = cin; on.additive = true;
    CHECK(same(v(before, on), v0));                       // fxadd on but no clone: refused as before
    const s5::EffectsVerdict va = v(add, on);
    CHECK(va.kind == s5::effects_ok && va.why == nullptr && va.depth_tested);   // fxadd on + clone: admitted
    { gpu_copy::step3::Shape d = s; d.pso.dual_ready = true; CHECK(same(v(d, cin), v(d, on)) && v(d, cin).kind == s5::effects_ok); }   // dual tuple: unchanged by the flag
    { gpu_copy::step3::Shape x = add; x.pso.depth_write = true; CHECK(std::string(v(x, on).why) == "class-state-not-effect"); }        // earlier rules still win
    { gpu_copy::step3::Shape x = add; x.pso.blend_constant = true; CHECK(std::string(v(x, on).why) == "blend-constant-factor"); }
    { const s5::EffectsVerdict c = v(add, on, s5::effects_draw_cap); CHECK(c.kind == s5::effects_refused && std::string(c.why) == "effects-cap"); }
    { s5::EffectsClassIn o2 = on; o2.dual = false; CHECK(same(v(add, o2), v(before, o2))); }   // dual key off: no dual rule at all, as before
    // the tracer-tuple rule is untouched: a tracer_ready PSO for a non-tracer candidate stays refused even with fxadd
    { gpu_copy::step3::Shape x = s; x.pso.dual_ready = x.pso.tracer_ready = true; CHECK(std::string(v(x, on).why) == "dual-blend-unsupported"); }
    // the line of the dual-refused status is fed from dual_ready, which the additive clone leaves false: the default line keeps `dual-ready=0`
    CHECK(!add.pso.dual_ready);
}
// (3) the shape rule: topology 1 only with the flag, one run, `is_points`; every other shape exactly as before
void test_point_shape() {
    auto why = [](int c, uint32_t n, uint32_t inst, uint32_t topo, bool pts, bool* quads = nullptr, bool* isp = nullptr) {
        bool q = true, ip = true; const char* w = s5::effects_sprite_nonindexed_shape(c, n, inst, topo, fxsprite::kMaxVertices, q, pts, &ip);
        if (quads) *quads = q; if (isp) *isp = ip; return std::string(w ? w : "none");
    };
    bool q = true, ip = true;
    CHECK(why(1, 1, 1, 1, false, &q, &ip) == "sprite-nonindexed-topology" && !ip);   // the step159 `nir=27(c:1,t:1)` refusal, unchanged with fxadd off
    CHECK(why(1, 1, 1, 1, true, &q, &ip) == "none" && !q && ip);                     // fxadd: one run of one point
    CHECK(why(1, 27, 1, 1, true, &q, &ip) == "none" && !q && ip);
    CHECK(why(1, 0, 1, 1, true) == "sprite-nonindexed-shape" && why(1, fxsprite::kMaxVertices + 1, 1, 1, true) == "sprite-nonindexed-shape");
    CHECK(why(1, 1, 2, 1, true) == "sprite-nonindexed-shape");                         // instanced: refused
    CHECK(why(2, 1, 1, 1, true) == "sprite-nonindexed-shape" && why(2, 4, 1, 1, true) == "sprite-nonindexed-topology");   // tracer: unchanged
    CHECK(why(1, 4, 1, 5, true, &q, &ip) == "none" && q && !ip);                      // a quad stays a quad
    CHECK(why(1, 6, 1, 4, true, &q, &ip) == "none" && !q && !ip);
    CHECK(why(1, 6, 1, 0, true) == "sprite-nonindexed-topology" && why(1, 6, 1, 2, true) == "sprite-nonindexed-topology");   // line lists etc.: still refused
    // the legacy 6-argument call (no is_points pointer) is the pre-(P) rule
    bool q2 = true; CHECK(std::string(s5::effects_sprite_nonindexed_shape(1, 1, 1, 1, fxsprite::kMaxVertices, q2)) == "sprite-nonindexed-topology");
}
// (4) cull_points: a bad point leaves every viewport (ps2_v: NDC = 2 (xy - centre) / (512, -448)); good points, colour, uv, z, q untouched
void test_cull_points() {
    fxmem::Vtx p[4]; for (int i = 0; i < 4; ++i) { p[i] = fxmem::Vtx{}; p[i].pos[0] = 1900.0f + i; p[i].pos[1] = 2000.0f + i; p[i].pos[2] = 123.0f; p[i].pos[3] = 0.5f;
        p[i].col[0] = 0.9f; p[i].col[3] = 0.7f; p[i].uv[0] = 0.25f; }
    fxmem::Vtx ref[4]; std::memcpy(ref, p, sizeof p);
    const uint8_t bad[4] = {0, 1, 0, 1};
    CHECK(fxsprite::cull_points(p, 4, bad) == 2);
    for (int i = 0; i < 4; ++i) {
        CHECK(std::memcmp(p[i].col, ref[i].col, sizeof p[i].col) == 0 && std::memcmp(p[i].uv, ref[i].uv, sizeof p[i].uv) == 0);
        CHECK(p[i].pos[2] == ref[i].pos[2] && p[i].pos[3] == ref[i].pos[3]);
        if (bad[i]) { CHECK(p[i].pos[0] == 0.0f && p[i].pos[1] == 0.0f); }
        else CHECK(p[i].pos[0] == ref[i].pos[0] && p[i].pos[1] == ref[i].pos[1]);
    }
    const double ndcx = 2.0 * (fxsprite::kPointCullX - 2048.0) / 512.0, ndcy = 2.0 * (fxsprite::kPointCullY - 2048.0) / -448.0;
    CHECK(std::fabs(ndcx) > 1.0 && std::fabs(ndcy) > 1.0 && !fxmem::in_window(fxsprite::kPointCullX, fxsprite::kPointCullY));
    CHECK(fxsprite::cull_points(nullptr, 4, bad) == 0 && fxsprite::cull_points(p, 4, nullptr) == 0);
}
// (5) plan_draw of a point run: every point lands on the eye projection of its world point (both depth forms, game colour/uv bytes); a point behind an eye is
// culled in THAT eye's copy and the others keep their place (no whole-run collapse); the keep copies equal the planned ones; the plan counts the points.
void test_point_plan() {
    const Cam gc{{1000, 4000, 2000}, deg(30), deg(-5)};
    double ag[16]; make_a(gc, ag); fxmem::ScreenMap gm; CHECK(fxsprite::game_map_from_a(ag, gm));
    double f[3], r[3], u[3]; axes(gc.yaw, gc.pitch, f, r, u);
    // five points: ahead / right / left / far (q form) / near
    const double where[5][3] = {{1200, 0, 0}, {900, 450, 30}, {900, -480, -20}, {20000, 3000, 1500}, {300, 40, -10}};
    fxmem::Vtx pts[5]; double world[5][3];
    for (int k = 0; k < 5; ++k) {
        for (int i = 0; i < 3; ++i) world[k][i] = gc.e[i] + where[k][0] * f[i] + where[k][1] * r[i] + where[k][2] * u[i];
        double xs, ys, d; CHECK(fxsprite::sp_project(gm, world[k], xs, ys, d));
        const bool qf = k == 3;
        pts[k] = fxmem::Vtx{}; pts[k].pos[0] = float(xs); pts[k].pos[1] = float(ys);
        pts[k].pos[2] = qf ? 0.0f : float(fxmem::game_z_for_depth(d)); pts[k].pos[3] = qf ? float(1.0 / d) : 1.0f;
        pts[k].col[0] = 1.0f; pts[k].col[1] = 0.6f; pts[k].col[2] = 0.2f; pts[k].col[3] = 0.1f * float(k + 1); pts[k].uv[0] = 0.5f; pts[k].uv[1] = 0.125f * float(k);
    }
    const uint16_t idx[5] = {0, 1, 2, 3, 4};   // the synthesised one-run list of a non-indexed point draw (lo = 0)
    auto eyeMap = [&](double yawOff, double side, double tanScale, fxmem::ScreenMap& m) {
        const Cam e{{gc.e[0] + side * r[0], gc.e[1] + side * r[1], gc.e[2] + side * r[2]}, gc.yaw + deg(yawOff), gc.pitch};
        double a[16]; make_a(e, a, tanScale); CHECK(fxsprite::eye_map_from_a(a, m));
    };
    auto run = [&](double yawOff, unsigned& culled, double& worst) {
        fxmem::ScreenMap e0, e1; eyeMap(yawOff, -0.032, 1.9, e0); eyeMap(yawOff, 0.032, 1.9, e1);
        const fxmem::ScreenMap* eyes[2] = {&e0, &e1};
        fxmem::Vtx scratch[5], keep0[5], keep1[5]; uint8_t bad[5];
        fxsprite::Plan p; p.begin();
        fxsprite::plan_draw(gm, eyes, pts, 5, idx, 5, 0, 1.0, scratch, bad, p, keep0, keep1, nullptr, 0.0, true, nullptr, true);
        CHECK(p.prepared == 1 && p.refused == 0 && p.points == 5 && p.quads == 0 && p.collapsed == 0);
        culled = p.points_culled; worst = 0;
        for (int e = 0; e < 2; ++e) {
            const fxmem::Vtx* kp = e == 0 ? keep0 : keep1; const fxmem::ScreenMap& m = e == 0 ? e0 : e1;
            for (int k = 0; k < 5; ++k) {
                CHECK(std::memcmp(kp[k].col, pts[k].col, sizeof pts[k].col) == 0 && std::memcmp(kp[k].uv, pts[k].uv, sizeof pts[k].uv) == 0);
                double xs, ys, d; const bool vis = fxsprite::sp_project(m, world[k], xs, ys, d) && d > 1.0;
                if (vis) { worst = std::fmax(worst, std::fabs(kp[k].pos[0] - xs) + std::fabs(kp[k].pos[1] - ys));
                           if (k == 3) CHECK(kp[k].pos[2] == 0.0f && std::fabs(kp[k].pos[3] * d - 1.0) < 1e-4); }
                else CHECK(kp[k].pos[0] == 0.0f && kp[k].pos[1] == 0.0f);   // culled: off every viewport
            }
        }
        return p;
    };
    unsigned culled = 0; double worst = 0;
    run(25, culled, worst);                      // head 25 deg left: all five in front
    CHECK(culled == 0 && worst < 0.05);
    const double w25 = worst;
    run(100, culled, worst);                     // head 100 deg left: the right-hand points are behind both eyes, the left one stays
    CHECK(culled > 0 && culled < 10 && worst < 0.05);
    const unsigned c100 = culled;
    run(180, culled, worst);                     // turned round: every point behind both eyes
    CHECK(culled == 10);
    std::printf("  (P) point plan: eye registration %.1e GS px; head 100 deg left culls %u of 10 eye points; 180 deg culls all\n", w25, c100);
    // the same run planned as an ordinary one-run list (pre-(P) path) would collapse every point onto point 0: the reason for cull_points
    {
        fxmem::ScreenMap e0, e1; eyeMap(100, -0.032, 1.9, e0); eyeMap(100, 0.032, 1.9, e1); const fxmem::ScreenMap* eyes[2] = {&e0, &e1};
        fxmem::Vtx scratch[5], keep0[5]; uint8_t bad[5]; fxsprite::Plan p; p.begin();
        fxsprite::plan_draw(gm, eyes, pts, 5, idx, 5, 0, 1.0, scratch, bad, p, keep0, nullptr, nullptr, 0.0, true);
        CHECK(p.points == 0 && p.collapsed > 0);
        for (int k = 1; k < 5; ++k) CHECK(std::memcmp(keep0[k].pos, keep0[0].pos, sizeof keep0[0].pos) == 0);
    }
    // no world camera: refused by name, no points counted
    { const fxmem::ScreenMap* none[2] = {nullptr, nullptr}; fxmem::Vtx scratch[5]; uint8_t bad[5]; fxsprite::Plan p; p.begin(); p.world_why = "sprite-world-no-object-record";
      fxsprite::plan_draw(gm, none, pts, 5, idx, 5, 0, 1.0, scratch, bad, p, nullptr, nullptr, nullptr, 0.0, true, nullptr, true);
      CHECK(p.refused == 1 && p.prepared == 0 && p.points == 0 && std::string(p.last_why) == "sprite-world-no-object-record"); }
    // the puff drop edge and widening never apply to a point run (forced off inside plan_draw)
    { fxmem::ScreenMap e0, e1; eyeMap(25, -0.032, 1.9, e0); eyeMap(25, 0.032, 1.9, e1); const fxmem::ScreenMap* eyes[2] = {&e0, &e1};
      fxmem::Vtx scratch[5]; uint8_t bad[5]; fxsprite::Plan p; p.begin(); const fxsprite::Widen wd{0.2, 0.31, 1.0};
      fxsprite::plan_draw(gm, eyes, pts, 5, idx, 5, 0, 1.0, scratch, bad, p, nullptr, nullptr, &wd, 1.0, false, nullptr, true);
      CHECK(p.wc_dropped == 0 && p.widened == 0 && p.collapsed == 0 && p.points == 5); }
}
// (6) the lines: `;pts=` only with points (else byte-identical), the -additive status line, both within the text cap
void test_point_lines() {
    const char* totals = "fx-sprite-totals;class=sprite(tracer-cand);frames=1;read=1;refused=0;caps=idx:1536,vtx:512;mode=plan+gpu-replay(per-eye,fx-sprite-gpu-lines)";
    char t[448]; std::snprintf(t, sizeof t, "%s", totals);
    fxsprite::Plan none; none.begin(); fxsprite::append_points(t, sizeof t, none); CHECK(std::string(t) == totals);
    fxsprite::Plan p; p.begin(); p.points = 27; p.points_culled = 4; fxsprite::append_points(t, sizeof t, p);
    CHECK(std::string(t) == std::string(totals) + ";pts=27(cull:4)");
    fxsprite::Plan w; w.ni_refused = 99999; w.ni_last_count = 999999; w.ni_last_topology = 999; w.points = 9999999; w.points_culled = 9999999;
    std::snprintf(t, sizeof t, "gpu-copy-step1;event=fx-sprite-totals;class=sprite(tracer-cand);frames=99999999999999;read=99999999999999;refused=99999999999999;"
                               "caps=idx:1536,vtx:512;mode=plan+gpu-replay(per-eye,fx-sprite-gpu-lines)");
    fxsprite::append_nonindexed_refused(t, sizeof t, w); fxsprite::append_points(t, sizeof t, w);
    CHECK(std::strlen(t) < kLineCap && std::string(t).find(";pts=99999(cull:99999)") != std::string::npos);
    char a[448];
    s5::format_additive_line(a, sizeof a, "gpu-copy-step1-status;event=continuous-hud-effects", 1234, 27, 900, 26, 45, 1);
    CHECK(std::string(a) == "gpu-copy-step1-status;event=continuous-hud-effects-additive;on=1;tuple=5,2,1/18,19,1;admitted=1234;points=27;per-eye=900;per-eye-points=26;frames=45;shots=1");
    s5::format_additive_line(a, sizeof a, "gpu-copy-step1-status;event=continuous-hud-effects", ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, 99);
    std::printf("  (P) additive status line at maxima: %zu bytes (cap 448)\n", std::strlen(a));
    CHECK(std::strlen(a) < 448 - 1);
}
// (7) the eye-dump shot: the first additive per-eye frame, then at most one more >= 20 s later on a frame with points
void test_additive_shot() {
    s5::AdditiveShot s;
    CHECK(!s.step(0, 0, 1000));
    CHECK(s.step(3, 0, 2000) && s.shots == 1);
    CHECK(!s.step(5, 2, 3000));                    // too soon
    CHECK(!s.step(5, 0, 2000 + s5::kAddShotGapMs)); // no points yet
    CHECK(s.step(5, 27, 2000 + s5::kAddShotGapMs) && s.shots == 2);
    CHECK(!s.step(5, 27, 900000));                  // at most kAddShots
}
// (Q) step174 fxadd null runs: the densest measured frames (fx-sprite-plan: class=sprite prepared 131 in AGB2 f9673, 115 in AGC1 f7961; class=tracer 58 drawn; explosion 1-2)
// replayed per eye in ONE frame with the plan lines' own vertex counts (951 and 867 vertices), then a 254-draw frame of 24-vertex strips. Before Q (kMaxDraws 96, span
// 512 KiB) draw #95 of the frame fell back to the window image (`sprite-gpu-draw-cap`) and 254 heavy strips overflowed the 384 KiB the sprite classes may use.
void test_step174_peak_frame() {
    struct Cls { unsigned draws; unsigned verts; };
    const Cls peak[] = {{131, 951 / 131}, {58, 867 / 58}};                       // 7 and 14 vertices per draw (averages of the measured plan lines)
    unsigned replayed = 0, fallback = 0; uint64_t cur = 0; const char* first = nullptr;
    for (const Cls& c : peak) for (unsigned i = 0; i < c.draws; ++i) {
        g::GateIn x = k_cloud::quad_gate(replayed, cur); x.vertices = c.verts;
        const char* why = g::gate(x); g::EyeViews ev;
        if (!why && !g::eye_views(0x10000, cur, c.verts, 0, ev)) why = "sprite-eye-view";
        if (why) { ++fallback; if (!first) first = why; continue; }
        cur += ev.used; ++replayed;
    }
    for (unsigned i = 0; i < 2; ++i) { g::GateIn x = k_cloud::quad_gate(replayed, cur, true); x.vertices = 24; CHECK(g::gate(x) == nullptr); g::EyeViews ev; CHECK(g::eye_views(0x10000, cur, 24, 0, ev)); cur += ev.used; ++replayed; }
    std::printf("  (Q) step174 peak frame: %u per eye, %u fallbacks (first %s), span %llu of %llu B\n", replayed, fallback, first ? first : "none",
                static_cast<unsigned long long>(cur), static_cast<unsigned long long>(g::kSlotUpload));
    CHECK(fallback == 0 && replayed == 131 + 58 + 2);
    CHECK(g::kMaxDraws * 100 >= (131 + 58 + 2) * 130);                         // >= 30 % draw margin over the measured peak
    CHECK(cur * 2 <= g::kSlotUpload - g::kExplosionReserveBytes);              // the measured peak uses at most half of the sprite classes' bytes
    {   // the worst the cap allows: every sprite-class draw a 24-vertex strip
        uint64_t c2 = 0; unsigned r = 0, fb = 0;
        for (unsigned i = 0; i < g::kMaxDraws - g::kExplosionReserveDraws; ++i) {
            g::GateIn x = k_cloud::quad_gate(r, c2); x.vertices = 24;
            if (g::gate(x)) { ++fb; continue; }
            c2 += g::replay_bytes(24); ++r;
        }
        CHECK(fb == 0 && r == g::kMaxDraws - g::kExplosionReserveDraws && c2 <= g::kSlotUpload - g::kExplosionReserveBytes);
    }
}
// (8) a dense crash frame: 27 one-vertex points + 60 additive quads + 7 dual puffs: all per eye (cap, span); a point's copy is one vertex per eye (aligned)
void test_dense_points_frame() {
    unsigned replayed = 0, fallback = 0; uint64_t cur = 0;
    for (unsigned i = 0; i < 27 + 60 + 7; ++i) {
        g::GateIn x = k_cloud::quad_gate(replayed, cur); const unsigned n = i < 27 ? 1u : 4u; x.vertices = n;
        const char* why = g::gate(x); g::EyeViews ev;
        if (!why && !g::eye_views(0x10000, cur, n, 0, ev)) why = "sprite-eye-view";
        if (why) { ++fallback; continue; }
        if (i == 0) CHECK(ev.used == g::replay_bytes(1) && ev.bytes == 64u);   // one 64-byte vertex per eye, each copy aligned (kCopyAlign)
        cur += ev.used; ++replayed;
    }
    std::printf("  (P) dense crash frame: %u per eye, %u fallbacks, span %llu B\n", replayed, fallback, static_cast<unsigned long long>(cur));
    CHECK(fallback == 0 && replayed == 94);
}
// (10) P2 (step162 null run, runAGB2 f11356: hard-edged cloud rectangles): a TEXTURED additive card through both paths, against the game.
// PS 4caf6e2d (DXIL ps2AtestCA_p): t = tex x COLOR0, a = sat(t.a) (discard a < 1/255), o0.rgb = lerp(fog, t.rgb, f) (COLOR1 = fog colour + factor), o0.a = a.
// The game (display-referred target): D' = a o0.rgb + D. Under the card lies a faint DUAL cloud card (coverage 0.05 over its whole quad, as the real puffs'
// low-alpha corners). Blend factors are D3D12_BLEND values evaluated per channel: 1 ZERO, 2 ONE, 5 SRC_ALPHA, 6 INV_SRC_ALPHA (enough for these tuples).
double bf(uint32_t f, double sa) { return f == 1 ? 0.0 : f == 2 ? 1.0 : f == 5 ? sa : f == 6 ? 1.0 - sa : -1.0; }
// The window composite of a coverage_v1 texel, CPU mirror of xr_stereo_blit.hlsl effects_over mode 4 (exposure 1, clamp tonemap): un-premultiply, clamp, premultiply.
double mode4(double P, double A, double world) { const double a = A > 0 ? (A < 1 ? A : 1) : 0; double ec = a > 0 ? P / a : 0; ec = ec < 0 ? 0 : ec > 1 ? 1 : ec; return ec * a + (1 - a) * world; }
void test_additive_soft_edges() {
    constexpr int N = 33; const double fogRgb = 0.8, cardRgb = 0.9, fogF = 0.4, world = 0.45, cov = 0.05, covRgb = 0.7;
    auto texA = [&](int x, int y) { const double dx = (x - 16) / 16.0, dy = (y - 16) / 16.0, r = std::sqrt(dx * dx + dy * dy);   // soft puff: 0.9 centre .. 0.02 corners
        double a = 0.9 * (1 - r) * (1 - r); if (r >= 1) a = 0; return a > 0.02 ? a : 0.02; };
    uint32_t sa = 0, da = 0;
    CHECK(s5::effects_additive_light_factors(5, 2, 1, 18, 19, 1, 15, sa, da) && sa == 1 && da == 2);
    CHECK(!s5::effects_additive_light_factors(5, 6, 1, 18, 19, 1, 15, sa, da) && !s5::effects_additive_light_factors(5, 5, 1, 18, 19, 1, 15, sa, da));
    CHECK(s5::effects_additive_window_src == 1 && s5::effects_additive_window_dst == 2);
    CHECK(s5::effects_additive_pso_why(false, false) == nullptr && s5::effects_additive_pso_why(true, true) == nullptr &&
          std::string(s5::effects_additive_pso_why(true, false)) == "sprite-additive-pso-missing");
    double eyeWorst = 0, winWorst = 0, gameC = 0, gameK = 0, eyeC = 0, eyeK = 0, oldC = 0, oldK = 0;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        const double a = texA(x, y), rgb = (1 - fogF) * fogRgb + fogF * cardRgb;   // o0.rgb (fog lerp), o0.a = a
        // the game: the dual card first (5,6 / 18,19), then the additive card (5,2,1)
        const double underGame = cov * covRgb + (1 - cov) * world, game = bf(5, a) * rgb + bf(2, a) * underGame;
        // per-eye sprite image (premultiplied light, coverage): dual card through its clone (ONE/INV alpha = coverage), then the additive LIGHT PSO (RGB 5,2,1, alpha ZERO/ONE)
        double P = cov * covRgb, A = cov;
        P = bf(5, a) * rgb + bf(2, a) * P; A = bf(1, a) * a + bf(2, a) * A;
        CHECK(A == cov);                                              // light adds no coverage
        const sg::Px s = g::sprite_layer(std::array<float, 4>{float(P), float(P), float(P), float(A)}.data(), 1.0f, 0, 1.0f);
        sg::Px u; u.r = u.g = u.b = g::srgb_decode(float(world)); u.a = 1.0f;
        const double eye = g::srgb_encode(g::sprite_over(u, s, 0.0f, false).r);
        eyeWorst = std::fmax(eyeWorst, std::fabs(eye - std::fmin(1.0, game)));
        // the WINDOW image under the P2 shared clone (RGB ZERO/ONE, alpha ZERO/ONE): the additive card leaves P and A exactly as the dual card left them
        double Pw = cov * covRgb, Aw = cov;
        Pw = bf(s5::effects_additive_window_src, a) * rgb + bf(s5::effects_additive_window_dst, a) * Pw; Aw = bf(1, a) * a + bf(2, a) * Aw;
        winWorst = std::fmax(winWorst, std::fabs(mode4(Pw, Aw, world) - mode4(cov * covRgb, cov, world)));
        // the step162 path: the WINDOW image under the first clone (RGB 5,2,1 kept) composited by mode 4: P / A saturates over the whole dual card
        const double Po = bf(5, a) * rgb + cov * covRgb, old = mode4(Po, cov, world);
        if (x == 16 && y == 16) { gameC = game - underGame; eyeC = eye - underGame; oldC = old - mode4(cov * covRgb, cov, world); }
        if (x == 1 && y == 1) { gameK = game - underGame; eyeK = eye - underGame; oldK = old - mode4(cov * covRgb, cov, world); }
    }
    std::printf("  (P2) textured additive card: lift centre/corner game %.3f/%.4f, per-eye %.3f/%.4f, step162 window %.4f/%.4f; per-eye worst %.1e, window change %.1e\n",
                gameC, gameK, eyeC, eyeK, oldC, oldK, eyeWorst, winWorst);
    CHECK(eyeWorst < 2e-3);                                           // per eye = the game's display value at every texel (soft falloff kept)
    CHECK(eyeC > 0.4 && eyeK < 0.02 && eyeK > 0.0 && eyeC > 20 * eyeK); // bright centre, faint corners: the alpha shapes the card
    CHECK(winWorst == 0.0);                                           // the window image is untouched by the additive card (no rectangle, as before fxadd)
    CHECK(std::fabs(oldC - oldK) < 1e-9 && oldK > 0.01);              // the bug, reproduced: one flat lift over the whole quad (a hard-edged rectangle) ...
    CHECK(oldC < 0.1 * std::fmin(1.0, gameC) && eyeC > 0.5);          // ... and the card's own soft shape lost (centre light clipped to the coverage)
}
// (9) the stage switch: fxadd only with fxmem:6; default off; the option line
void test_stage_switch() {
    CHECK(!s5::stage_effects(6, false).additive && !s5::StageEffects{}.additive);
    CHECK(s5::stage_effects(6, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{}, false, true).additive);
    CHECK(!s5::stage_effects(5, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{}, false, true).additive);
    s5::apply_stage_effects(s5::stage_effects(6, false, 0, true, 0.0f, false, 0.0f, 1500.0f, 0, 2500.0f, s5::PuffCloud{}, false, true));
    CHECK(s5::effects_additive_flag().load());
    s5::clear_stage_effects(); CHECK(!s5::effects_additive_flag().load());
    char b[448];
    CHECK(!s5::fx_additive_line(b, sizeof b, 6, false));
    CHECK(s5::fx_additive_line(b, sizeof b, 6, true) && std::string(b).find("event=fx-additive-option;requested=1;active=1;") != std::string::npos);
    CHECK(s5::fx_additive_line(b, sizeof b, 5, true) && std::string(b).find("active=0;reason=needs-fxmem-6") != std::string::npos && std::strlen(b) < 447);
}
} // namespace p_points

int main() {
    std::printf("fx_census_offline\n");
    test_tracer_class();
    test_sig_log(); test_sig_log_onsets(); test_sig_log_lines(); test_sig_queue();
    test_classify_and_key();
    test_census_timeline();
    test_census_caps_and_lengths();
    test_reproject_identity_and_eye();
    test_census_priority();
    test_header_maths_match_fx_memory();
    test_hudonly_depth_model();
    test_explosion_blend_model();
    test_explosion_light();
    test_explosion_class_gate();
    test_sprite_plan();
    test_sprite_gpu();
    k_cloud::test_odd_run_collapse();
    k_cloud::test_dense_frame_all_per_eye();
    k_cloud::test_nonindexed_shape_table();
    p_points::test_additive_tuple(); p_points::test_additive_gate(); p_points::test_point_shape(); p_points::test_cull_points();
    p_points::test_point_plan(); p_points::test_point_lines(); p_points::test_additive_shot(); p_points::test_dense_points_frame(); p_points::test_step174_peak_frame(); p_points::test_stage_switch(); p_points::test_additive_soft_edges();
    std::printf("PASS fx_census_offline (%u checks)\n", g_checks);
    return 0;
}
