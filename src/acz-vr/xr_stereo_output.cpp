#include "xr_stereo_output.h"
#include "xr_owner_registry.h"
#include "xr_vscreen.h"   // VSCREEN hook interface (default null)
#include "lens_flare.h"   // LENS-FLARE diagnostics (lensflare_diag)
#include "hud_late.h"     // HUD-LATE subtitle shot: the producer's eye-dump request (hud_late_eye_request)
#include "effects_game_clouds.h"   // EFFECTS-OUTSIDE (P) ADDITIVE SHOT: effects_additive_eye_request
#include "ui3d_pause.h"   // UI3D stage 3a: the HUD-MENU state published to the producer (pause-menu layers, ui-3d.md 7)
#include "eye_res.h"      // EYE-RES: eyeres:<pct> eye swapchain size policy (docs/world-render/eye-resolution.md)
#include "eye_ring.h"     // EYE-RING: the per-eye producer images at the eye size (StereoFrame::eye_width/eye_height)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <limits>
#include <new>
#include <thread>

namespace acz::xr_stereo {
using xr_owner::succeeded;

uint32_t configured_cap(const char* value) noexcept {
    if (!value || !*value) return 2400;
    uint64_t cap = 0;
    for (const char* p = value; *p; ++p) {
        if (*p < '0' || *p > '9') return 2400;
        cap = cap * 10 + unsigned(*p - '0');
        if (cap > 8192) return 2400;
    }
    return cap ? uint32_t(cap) : 2400;
}
SourceProbe probe_source(stereo::FrameSource* source) noexcept {
    SourceProbe p;
    if (!source) return p;
    p.tried = true;
    stereo::StereoFrame f;
    if (!source->try_acquire_latest(0, f)) return p;
    p.acquired = true;
    p.frame_id = f.frame_id;
    p.slot = f.slot;
    p.extent = Extent{f.width, f.height};
    source->release(f); // metadata only: no consumer GPU reference exists
    return p;
}
Extent published_source_extent(stereo::FrameSource* source) noexcept { return probe_source(source).extent; }
bool needs_clear(const BlitMapping& m, uint32_t w, uint32_t h) noexcept {
    return !m.ok || m.dst.x || m.dst.y || m.dst.w != w || m.dst.h != h;
}
GpuTiming timestamp_timing(uint64_t start, uint64_t end, uint64_t cg, uint64_t cc,
    uint64_t execute, uint64_t ghz, uint64_t chz) noexcept {
    if (!ghz || !chz || end < start) return {};
    // Subtract in floating point to handle a calibration on either side of start.
    const long double delay = (static_cast<long double>(start) - cg) / ghz +
        (static_cast<long double>(cc) - execute) / chz;
    const double ms = double(delay * 1000);
    if (!std::isfinite(ms) || ms < 0) return {};
    return {true, ms, double(static_cast<long double>(end - start) * 1000 / ghz)};
}
// ------------------------------------------------------------- mapping ---
BlitMapping letterbox(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) noexcept {
    BlitMapping m;
    if (!sw || !sh || !dw || !dh) return m;
    // Compare dw/sw with dh/sh exactly: dw*sh vs dh*sw (64-bit, no rounding).
    const uint64_t a = uint64_t(dw) * sh, b = uint64_t(dh) * sw;
    uint64_t w = 0, h = 0;
    if (a <= b) { // width limits: full width, height = round(sh * dw / sw)
        w = dw;
        h = (uint64_t(sh) * dw * 2 + sw) / (uint64_t(sw) * 2);
        m.scale = double(dw) / double(sw);
    } else {      // height limits: full height, width = round(sw * dh / sh)
        h = dh;
        w = (uint64_t(sw) * dh * 2 + sh) / (uint64_t(sh) * 2);
        m.scale = double(dh) / double(sh);
    }
    w = std::min<uint64_t>(std::max<uint64_t>(w, 1), dw);
    h = std::min<uint64_t>(std::max<uint64_t>(h, 1), dh);
    m.dst.w = uint32_t(w); m.dst.h = uint32_t(h);
    m.dst.x = (dw - m.dst.w) / 2; m.dst.y = (dh - m.dst.h) / 2;
    m.ok = true;
    return m;
}
BlitMapping full_rect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) noexcept {
    BlitMapping m;
    if (!sw || !sh || !dw || !dh) return m;
    m.dst = {0, 0, dw, dh};
    m.scale = double(dw) / double(sw);
    m.ok = true;
    return m;
}
bool source_uv(const BlitMapping& m, uint32_t px, uint32_t py, double& u, double& v) noexcept {
    u = v = 0;
    if (!m.ok || px < m.dst.x || py < m.dst.y || px >= m.dst.x + m.dst.w || py >= m.dst.y + m.dst.h) return false;
    u = (double(px - m.dst.x) + 0.5) / double(m.dst.w);
    v = (double(py - m.dst.y) + 0.5) / double(m.dst.h);
    return true;
}
Extent swapchain_extent(uint32_t sw, uint32_t sh, uint32_t max_w, uint32_t max_h, uint32_t cap_w) noexcept {
    Extent e;
    if (!sw || !sh || !max_w || !max_h || !cap_w) return e;
    uint64_t w = std::min({sw, cap_w, max_w});
    uint64_t h = (w * sh * 2 + sw) / (uint64_t(sw) * 2);
    if (h > max_h) { h = max_h; w = (h * sw * 2 + sh) / (uint64_t(sh) * 2); }
    e.w = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(w, max_w)));
    e.h = uint32_t(std::max<uint64_t>(1, h));
    return e;
}
bool lens_ok(const float t[4]) noexcept {
    if (!t) return false;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(t[i]) || !(t[i] > 0.0f) || t[i] > 10.0f) return false;
    return true;
}
xr_owner::Fov fov_from_lens(const float t[4]) noexcept {
    return {-std::atan(t[0]), std::atan(t[1]), std::atan(t[2]), -std::atan(t[3])};
}
Quad world_quad(const float n[3], const float t[4], float d) noexcept {
    Quad q;
    q.pose.position[0] = n[0] + d * (t[1] - t[0]) * 0.5f;
    q.pose.position[1] = n[1] + d * (t[2] - t[3]) * 0.5f;
    q.pose.position[2] = n[2] - d;
    q.pose.orientation[0] = q.pose.orientation[1] = q.pose.orientation[2] = 0.0f; q.pose.orientation[3] = 1.0f;
    q.width = d * (t[0] + t[1]);
    q.height = d * (t[2] + t[3]);
    return q;
}
Quad world_quad(const float n[3], const float t[4], float d, float yaw) noexcept {
    // The unturned offset exactly as world_quad() computes it, then rotated about +Y around n:
    // Ry(a) = [[cos a, 0, sin a], [0, 1, 0], [-sin a, 0, cos a]] (column vectors, right-handed,
    // OpenXR -Z forward), so Ry(a)(0,0,-1) = (-sin a, 0, -cos a) = the producer's cockpit forward.
    Quad q;
    const double ox = double(d * (t[1] - t[0]) * 0.5f), oy = double(d * (t[2] - t[3]) * 0.5f), oz = -double(d);
    const double c = std::cos(double(yaw)), s = std::sin(double(yaw));
    q.pose.position[0] = float(double(n[0]) + c * ox + s * oz);
    q.pose.position[1] = float(double(n[1]) + oy);
    q.pose.position[2] = float(double(n[2]) - s * ox + c * oz);
    q.pose.orientation[0] = q.pose.orientation[2] = 0.0f;
    q.pose.orientation[1] = float(std::sin(0.5 * double(yaw)));
    q.pose.orientation[3] = float(std::cos(0.5 * double(yaw)));
    q.width = d * (t[0] + t[1]);
    q.height = d * (t[2] + t[3]);
    return q;
}
Quad world_quad(const float n[3], const float t[4], float d, float yaw, float pitch) noexcept {
    if (pitch == 0.0f) return world_quad(n, t, d, yaw);   // exactly the yaw-only quad
    // o = (D*(r-l)/2, D*(u-d)/2, -D); centre = n + Ry(yaw) Rx(pitch) o; orientation = Ry(yaw) Rx(pitch).
    // Rx(a) = [[1,0,0],[0,cos a,-sin a],[0,sin a,cos a]]: Rx(a)(0,0,-1) = (0, sin a, -cos a), i.e. up for a > 0.
    Quad q;
    const double ox = double(d * (t[1] - t[0]) * 0.5f), oy = double(d * (t[2] - t[3]) * 0.5f), oz = -double(d);
    const double cp = std::cos(double(pitch)), sp = std::sin(double(pitch));
    const double px = ox, py = cp * oy - sp * oz, pz = sp * oy + cp * oz;
    const double c = std::cos(double(yaw)), s = std::sin(double(yaw));
    q.pose.position[0] = float(double(n[0]) + c * px + s * pz);
    q.pose.position[1] = float(double(n[1]) + py);
    q.pose.position[2] = float(double(n[2]) - s * px + c * pz);
    const double hy = 0.5 * double(yaw), hp = 0.5 * double(pitch);
    const double ay = std::sin(hy), aw = std::cos(hy), bx = std::sin(hp), bw = std::cos(hp);   // qy = (0, ay, 0, aw), qx = (bx, 0, 0, bw)
    q.pose.orientation[0] = float(aw * bx);
    q.pose.orientation[1] = float(ay * bw);
    q.pose.orientation[2] = float(-ay * bx);
    q.pose.orientation[3] = float(aw * bw);
    q.width = d * (t[0] + t[1]);
    q.height = d * (t[2] + t[3]);
    return q;
}
// xr_look_drive_keys: the frame's rendered look, accepted only when finite and inside what the game can do (|yaw| <= 3, pitch in [-0.6, 1.7]).
bool look_of(const stereo::StereoFrame& f, float& yaw, float& pitch) noexcept {
    yaw = pitch = 0.0f;
    if (!f.look_valid || !std::isfinite(f.look_yaw) || !std::isfinite(f.look_pitch)) return false;
    if (std::fabs(f.look_yaw) > 3.0f || f.look_pitch < -0.6f || f.look_pitch > 1.7f) return false;
    yaw = f.look_yaw; pitch = f.look_pitch;
    return true;
}
bool neutral_ok(const stereo::StereoFrame& f) noexcept {
    if (!f.neutral_valid) return false;
    for (float v : f.neutral_pos) if (!std::isfinite(v) || std::fabs(v) > 1000.0f) return false;
    return std::isfinite(f.neutral_yaw) && std::fabs(f.neutral_yaw) <= 4.0f;
}
uint32_t depth_srv_format(uint32_t f) noexcept {
    switch (f) {
    case 20: return 21; // D32_FLOAT_S8X24_UINT -> R32_FLOAT_X8X24_TYPELESS (plane 0)
    case 40: return 41; // D32_FLOAT -> R32_FLOAT
    case 45: return 46; // D24_UNORM_S8_UINT -> R24_UNORM_X8_TYPELESS
    case 55: return 56; // D16_UNORM -> R16_UNORM
    // D-240: the producer publishes the RESOURCE format of its typeless depth (e.g. 19 = R32G8X24_TYPELESS); the SRV format is the same plane view.
    case 19: return 21; // R32G8X24_TYPELESS -> R32_FLOAT_X8X24_TYPELESS (plane 0)
    case 39: return 41; // R32_TYPELESS -> R32_FLOAT
    case 44: return 46; // R24G8_TYPELESS -> R24_UNORM_X8_TYPELESS
    case 53: return 56; // R16_TYPELESS -> R16_UNORM
    default: return 0;
    }
}
Shade shade(const float rgb[3], bool use_depth, float depth, Tonemap tm, float exposure) noexcept {
    float c[3];
    for (int i = 0; i < 3; ++i) {
        float v = rgb[i] * exposure;
        v = v > 0.0f ? v : 0.0f; // HLSL max(v,0): NaN -> 0
        c[i] = tm == Tonemap::reinhard ? v / (1.0f + v) : std::min(v, 1.0f);
    }
    const float a = use_depth ? (depth > 0.0f ? 1.0f : 0.0f) : 1.0f;
    return {c[0] * a, c[1] * a, c[2] * a, a};
}
void debug_view_rgb(unsigned view, float cover, const float colour[3], const float world[4], float out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0f;
    switch (view) {
    case 1: out[0] = out[1] = out[2] = cover; break;
    case 2: for (int i = 0; i < 3; ++i) out[i] = colour[i]; break;
    case 3: for (int i = 0; i < 3; ++i) out[i] = world[i]; break;
    case 4:
        out[0] = cover; out[1] = world[3];
        out[2] = (cover == 0.0f && std::max(colour[0], std::max(colour[1], colour[2])) > 0.0f) ? 1.0f : 0.0f;
        break;
    default: break;
    }
}
Shade shade_hud(const float rgba[4], Tonemap tm, float exposure) noexcept {
    // HUD-LATE display marker (hud_late.h, review F6): alpha < 0 = the pixel holds post-tone-map display values (clamp only, no exposure / tone map).
    const bool display = acz::gpu_copy::step5::hud_late_is_display(rgba[3]);
    const Shade s = display ? shade(rgba, false, 0.0f, Tonemap::clamp, 1.0f) : shade(rgba, false, 0.0f, tm, exposure); // same exposure/tonemap as every other layer, a = 1 ignored
    float a = display ? acz::gpu_copy::step5::hud_late_coverage(rgba[3]) : (rgba[3] > 0.0f ? rgba[3] : 0.0f); // HLSL max(a,0): NaN -> 0
    a = std::max(a, std::max(s.r, std::max(s.g, s.b)));
    a = std::min(a, 1.0f);                                   // saturate
    return {s.r, s.g, s.b, a};
}
bool world_composite_params(const xr_owner::EyeSample& eye, const float anchor[3], float yaw, const float lens_tan[4],
    float dist, WorldComposite& out, float pitch) noexcept {
    if (!anchor || !lens_tan || !lens_ok(lens_tan) || !std::isfinite(dist) || !(dist > 0.0f) || !std::isfinite(yaw) || !std::isfinite(pitch)) return false;
    if (!xr_owner::pose_finite(eye.pose)) return false;
    const float f[4] = {eye.fov.left, eye.fov.right, eye.fov.up, eye.fov.down};
    for (float v : f) if (!std::isfinite(v) || std::fabs(v) >= 1.5f) return false;
    if (!(f[0] < f[1] && f[3] < f[2])) return false;
    const double qx = eye.pose.orientation[0], qy = eye.pose.orientation[1], qz = eye.pose.orientation[2], qw = eye.pose.orientation[3];
    const double qn = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(qn > 0.5 && qn < 2.0)) return false;
    const double x = qx / qn, y = qy / qn, z = qz / qn, w = qw / qn;
    // Columns of the eye rotation (OpenXR: eye +X right, +Y up, -Z forward), then Ry(-yaw) into the anchor frame.
    const double col[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
        {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
        {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
    const double c = std::cos(double(yaw)), s = std::sin(double(yaw));
    // xr_look_drive_keys: after Ry(-yaw) the plane's frame is additionally tilted by the rendered look pitch: Rx(-pitch) (pitch == 0: skipped,
    // the previous maths bit for bit).
    const double cp = std::cos(double(pitch)), sp = std::sin(double(pitch));
    auto turn = [&](const double v[3], double o[3]) {
        o[0] = c * v[0] - s * v[2]; o[1] = v[1]; o[2] = s * v[0] + c * v[2];
        if (pitch != 0.0f) { const double y1 = o[1], z1 = o[2]; o[1] = cp * y1 + sp * z1; o[2] = -sp * y1 + cp * z1; }
    };
    double X[3], Y[3], Z[3], P[3];
    turn(col[0], X); turn(col[1], Y); turn(col[2], Z);
    const double rel[3] = {double(eye.pose.position[0]) - anchor[0], double(eye.pose.position[1]) - anchor[1],
        double(eye.pose.position[2]) - anchor[2]};
    turn(rel, P);
    const double tl = std::tan(double(f[0])), tr = std::tan(double(f[1])), tu = std::tan(double(f[2])), td = std::tan(double(f[3]));
    const double l = lens_tan[0], r = lens_tan[1], u = lens_tan[2], d = lens_tan[3], D = dist;
    WorldComposite o{};
    for (int i = 0; i < 3; ++i) {
        o.origin[i] = float(P[i]);
        o.base[i] = float(tl * X[i] + tu * Y[i] - Z[i]);
        o.ux[i] = float((tr - tl) * X[i]);
        o.vy[i] = float((td - tu) * Y[i]);
    }
    o.dist = dist;
    o.au = float(1.0 / (D * (l + r)));
    o.bu = float(l / (l + r));
    o.av = float(-1.0 / (D * (u + d)));
    o.bv = float(u / (u + d));
    const float* all = reinterpret_cast<const float*>(&o);
    for (unsigned i = 0; i < 17; ++i) if (!std::isfinite(all[i])) return false;
    out = o;
    return true;
}
WorldHit world_ray_uv(const WorldComposite& c, float u, float v) noexcept {
    // Same operations, same order as xr_stereo_blit.hlsl world_layer().
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = c.base[i] + u * c.ux[i] + v * c.vy[i];
    WorldHit h;
    if (!(d[2] < -kWorldRayEps)) return h;
    const float t = (-c.dist - c.origin[2]) / d[2];
    if (!(t > 0.0f)) return h;
    const float hx = c.origin[0] + t * d[0], hy = c.origin[1] + t * d[1];
    const float wu = hx * c.au + c.bu, wv = hy * c.av + c.bv;
    if (!(wu >= 0.0f && wu <= 1.0f && wv >= 0.0f && wv <= 1.0f)) return h;
    h.hit = true; h.u = wu; h.v = wv;
    return h;
}
// ---------------------------------------------------------------- DECOUPLE (decouple:1; docs/world-render/decouple.md)
void camera_delta_rotation(const float orr[3], const float oup[3], const float ofw[3], const float trr[3], const float tup[3], const float tfw[3],
                           double Q[3][3]) noexcept {
    const bool same = std::memcmp(orr, trr, 3 * sizeof(float)) == 0 && std::memcmp(oup, tup, 3 * sizeof(float)) == 0 && std::memcmp(ofw, tfw, 3 * sizeof(float)) == 0;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) Q[i][j] = i == j ? 1.0 : 0.0;
    if (same) return;   // the exact identity (no rounding noise from C^T C)
    // C = [right | up | -fwd]: anchor-frame d -> world C d (the terrain shader's dw = right d.x + up d.y + fwd (-d.z)).
    const float* co[3] = {orr, oup, ofw};
    const float* ct[3] = {trr, tup, tfw};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double s = 0;
        for (int k = 0; k < 3; ++k) s += double(co[i][k]) * double(ct[j][k]);
        Q[i][j] = ((i == 2) != (j == 2)) ? -s : s;   // the -fwd column flips the sign of the mixed (fwd, non-fwd) entries
    }
}
bool rotation_is_identity(const double Q[3][3]) noexcept {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) if (Q[i][j] != (i == j ? 1.0 : 0.0)) return false;
    return true;
}
bool reproject_params(const WorldComposite& p0, const double Qs[3][3], const double Qf[3][3], bool src, bool fx, float fade, Reproject& out) noexcept {
    out = Reproject{};
    if (!src && !fx) return true;
    // M0 columns: ux0, vy0, base0 (anchor-frame ray of P0 uv (u, v): M0 (u, v, 1)); H = M0^-1 Qsrc.
    const double m[3][3] = {{p0.ux[0], p0.vy[0], p0.base[0]}, {p0.ux[1], p0.vy[1], p0.base[1]}, {p0.ux[2], p0.vy[2], p0.base[2]}};
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
    double inv[3][3];
    inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det; inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det; inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det; inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det; inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det; inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det; inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    Reproject r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
        double s = 0;
        for (int k = 0; k < 3; ++k) s += inv[i][k] * Qs[k][j];
        r.h[i * 3 + j] = float(s);
    }
    // Qfx -> quaternion (Shepperd), v' = v + 2 q x (q x v + w v) reproduces Qfx v.
    double qw, qx, qy, qz;
    const double tr = Qf[0][0] + Qf[1][1] + Qf[2][2];
    if (tr > 0) { const double s = std::sqrt(tr + 1.0) * 2; qw = 0.25 * s; qx = (Qf[2][1] - Qf[1][2]) / s; qy = (Qf[0][2] - Qf[2][0]) / s; qz = (Qf[1][0] - Qf[0][1]) / s; }
    else if (Qf[0][0] > Qf[1][1] && Qf[0][0] > Qf[2][2]) { const double s = std::sqrt(1.0 + Qf[0][0] - Qf[1][1] - Qf[2][2]) * 2; qw = (Qf[2][1] - Qf[1][2]) / s; qx = 0.25 * s; qy = (Qf[0][1] + Qf[1][0]) / s; qz = (Qf[0][2] + Qf[2][0]) / s; }
    else if (Qf[1][1] > Qf[2][2]) { const double s = std::sqrt(1.0 + Qf[1][1] - Qf[0][0] - Qf[2][2]) * 2; qw = (Qf[0][2] - Qf[2][0]) / s; qx = (Qf[0][1] + Qf[1][0]) / s; qy = 0.25 * s; qz = (Qf[1][2] + Qf[2][1]) / s; }
    else { const double s = std::sqrt(1.0 + Qf[2][2] - Qf[0][0] - Qf[1][1]) * 2; qw = (Qf[1][0] - Qf[0][1]) / s; qx = (Qf[0][2] + Qf[2][0]) / s; qy = (Qf[1][2] + Qf[2][1]) / s; qz = 0.25 * s; }
    const double qn = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
    if (!(qn > 0.5)) return false;
    r.q[0] = float(qx / qn); r.q[1] = float(qy / qn); r.q[2] = float(qz / qn); r.q[3] = float(qw / qn);
    r.fade = std::isfinite(fade) && fade > 0.0f ? std::min(fade, 0.5f) : 0.0f;
    r.flags = (src ? kReprojSource : 0u) | (fx ? kReprojWindow : 0u);
    for (float v : r.h) if (!std::isfinite(v)) return false;
    for (float v : r.q) if (!std::isfinite(v)) return false;
    out = r;
    return true;
}
ReprojectSample reproject_uv(const Reproject& r, const WorldComposite& c, float u, float v) noexcept {
    // Same operations, same order as xr_stereo_blit.hlsl dc_source_uv().
    ReprojectSample s; s.u = u; s.v = v; s.in = true; s.fade = 1.0f;
    if (!(r.flags & kReprojSource)) return s;
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = c.base[i] + u * c.ux[i] + v * c.vy[i];
    float p[3];
    for (int i = 0; i < 3; ++i) p[i] = r.h[i * 3 + 0] * d[0] + r.h[i * 3 + 1] * d[1] + r.h[i * 3 + 2] * d[2];
    if (!(p[2] > 1e-6f)) { s.in = false; s.fade = 0.0f; s.u = s.v = 0.5f; return s; }
    const float su = p[0] / p[2], sv = p[1] / p[2];
    s.in = su >= 0.0f && su <= 1.0f && sv >= 0.0f && sv <= 1.0f;
    if (!s.in) s.fade = 0.0f;
    else if (r.fade > 0.0f) {
        auto ramp = [&](float x) { const float m = std::min(x, 1.0f - x); float t = m / r.fade; t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t; return t * t * (3.0f - 2.0f * t); };
        s.fade = ramp(su) * ramp(sv);
    }
    s.u = su < 0.0f ? 0.0f : su > 1.0f ? 1.0f : su;
    s.v = sv < 0.0f ? 0.0f : sv > 1.0f ? 1.0f : sv;
    return s;
}
void reproject_dir(const Reproject& r, const float d[3], float out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = d[i];
    if (!(r.flags & kReprojWindow)) return;
    const float* q = r.q;   // v' = v + 2 q x (q x v + w v)
    const float t[3] = {q[1] * d[2] - q[2] * d[1] + q[3] * d[0], q[2] * d[0] - q[0] * d[2] + q[3] * d[1], q[0] * d[1] - q[1] * d[0] + q[3] * d[2]};
    out[0] = d[0] + 2.0f * (q[1] * t[2] - q[2] * t[1]);
    out[1] = d[1] + 2.0f * (q[2] * t[0] - q[0] * t[2]);
    out[2] = d[2] + 2.0f * (q[0] * t[1] - q[1] * t[0]);
}
float model_cockpit_cover(bool use_depth, float depth, float cover) noexcept {   // COCKPIT-2: mirror of the kModeBitModelCockpit branch of ps_main
    if (!use_depth) return 0.0f;
    return (depth > 0.0f && depth < kOccludeObjectDepth) ? cover : 0.0f;
}
float window_occlude_keep(const WorldComposite& c, float u, float v, float depth) noexcept {
    if (!(depth > 0.0f && depth < kOccludeObjectDepth)) return 1.0f; // cockpit depth (above the band), no coverage, NaN: unchanged
    const WorldHit h = world_ray_uv(c, u, v);                        // the same window test as the shader (occlude_keep)
    if (!h.hit) return 1.0f;                                         // periphery: the replay stays
    return 1.0f - edge_ramp(h.u, h.v, kOccludeSeam);
}
float world_hold_fade(int64_t age_ms, int64_t hold_ms) noexcept {
    if (hold_ms <= 0 || age_ms >= hold_ms) return 0.0f;
    const int64_t full = hold_ms / 2; // full strength for the first half, then linear to 0 at expiry
    if (age_ms <= full) return 1.0f;
    return float(double(hold_ms - age_ms) / double(hold_ms - full));
}
namespace {
float sat01(float x) noexcept { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; } // HLSL saturate: NaN -> 0
float smooth01(float e0, float e1, float x) noexcept { const float t = sat01((x - e0) / (e1 - e0)); return t * t * (3.0f - 2.0f * t); }
float clampf(float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
}
WorldExtendSample world_ray_extend(const WorldComposite& c, float u, float v) noexcept {
    // Same operations, same order as xr_stereo_blit.hlsl world_layer_ext().
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = c.base[i] + u * c.ux[i] + v * c.vy[i];
    WorldExtendSample o;
    bool plane = false;
    float rawP[2] = {0.0f, 0.0f};
    if (d[2] < -kWorldRayEps) {
        const float t = (-c.dist - c.origin[2]) / d[2];
        if (t > 0.0f) {
            const float hx = c.origin[0] + t * d[0], hy = c.origin[1] + t * d[1];
            const float wu = hx * c.au + c.bu, wv = hy * c.av + c.bv;
            if (wu >= 0.0f && wu <= 1.0f && wv >= 0.0f && wv <= 1.0f) { o.u = wu; o.v = wv; return o; } // the window: exactly world_ray_uv
            if (std::fabs(wu) < 1e30f && std::fabs(wv) < 1e30f) { plane = true; rawP[0] = clampf(wu, -8.0f, 9.0f); rawP[1] = clampf(wv, -8.0f, 9.0f); }
        }
    }
    o.ext = true;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-12f) || !std::isfinite(len)) { o.u = c.bu; o.v = c.bv; o.k = kExtendMinLight; o.ang = 3.14159265f; return o; } // never produced by world_composite_params
    const float hn = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    const float az = hn < 1e-6f ? 0.0f : std::atan2(d[0], -d[2]);
    const float el = std::atan2(d[1], hn);
    const float rawA[2] = {c.bu + std::tan(clampf(az, -1.5f, 1.5f)) * (c.dist * c.au), c.bv + std::tan(clampf(el, -1.5f, 1.5f)) * (c.dist * c.av)};
    const float cosz = -d[2] / len;
    const float wa = plane ? 1.0f - smooth01(0.0f, 0.5f, cosz) : 1.0f;
    // How far a raw coordinate lies outside [0,1] (window-size units; NaN -> 0). The two maps are blended AFTER clamping (a clamped
    // plane value of +-8 blended with an in-range angular one would sweep the whole range within a few pixels).
    auto outside = [](float r) { const float lo = -r, hi = r - 1.0f; const float m = lo > hi ? lo : hi; return m > 0.0f ? m : 0.0f; };
    auto mix = [wa](float p, float a) { return p + (a - p) * wa; };
    const float eu = mix(outside(rawP[0]), outside(rawA[0])), ev = mix(outside(rawP[1]), outside(rawA[1]));
    const float uc = mix(sat01(rawP[0]), sat01(rawA[0])), vc = mix(sat01(rawP[1]), sat01(rawA[1]));
    // The window point the sample stands for, as a direction from the anchor, and the angle to the ray.
    const float cx = (uc - c.bu) / c.au, cy = (vc - c.bv) / c.av, cz = -c.dist;
    const float cl = std::sqrt(cx * cx + cy * cy + cz * cz);
    const float cosang = clampf((d[0] * cx + d[1] * cy + d[2] * cz) / (len * cl), -1.0f, 1.0f);
    o.ang = std::acos(cosang);
    o.k = 1.0f - (1.0f - kExtendMinLight) * smooth01(0.0f, kExtendFalloffRad, o.ang);
    const float tb = smooth01(0.0f, kExtendBlurRad, o.ang) * kExtendBlurMix;
    // The axis ALONG the edge is contracted toward the middle, ramped in as the other axis leaves the window and out as this one does (continuous at corners).
    o.u = uc + (0.5f - uc) * tb * smooth01(0.0f, kExtendBlurEdge, ev) * (1.0f - smooth01(0.0f, kExtendBlurEdge, eu));
    o.v = vc + (0.5f - vc) * tb * smooth01(0.0f, kExtendBlurEdge, eu) * (1.0f - smooth01(0.0f, kExtendBlurEdge, ev));
    return o;
}
// ---- J-world-cache: CPU mirrors of the HLSL cache helpers (same operations, same order).
namespace {
constexpr float kCachePi = 3.14159265358979f;
}
void world_cache_to_cache(float ly, float lp, const float d[3], float g[3]) noexcept {
    const float cy = std::cos(ly), sy = std::sin(ly), cp = std::cos(lp), sp = std::sin(lp);
    const float ay = cp * d[1] - sp * d[2], az = sp * d[1] + cp * d[2];            // Rx(lp)
    g[0] = cy * d[0] + sy * az; g[1] = ay; g[2] = -sy * d[0] + cy * az;            // Ry(ly)
}
void world_cache_to_window(float ly, float lp, const float g[3], float d[3]) noexcept {
    const float cy = std::cos(ly), sy = std::sin(ly), cp = std::cos(lp), sp = std::sin(lp);
    const float ax = cy * g[0] - sy * g[2], az = sy * g[0] + cy * g[2];            // Ry(-ly)
    d[0] = ax; d[1] = cp * g[1] + sp * az; d[2] = -sp * g[1] + cp * az;            // Rx(-lp)
}
void world_cache_dir_uv(const float g[3], float& u, float& v) noexcept {
    const float hn = std::sqrt(g[0] * g[0] + g[2] * g[2]);
    const float az = std::atan2(g[0], -g[2]), el = std::atan2(g[1], hn);
    u = 0.5f + az / (2.0f * kCachePi);
    v = 0.5f - el / kCachePi;
}
void world_cache_uv_dir(float u, float v, float g[3]) noexcept {
    const float az = (u - 0.5f) * 2.0f * kCachePi, el = (0.5f - v) * kCachePi;
    g[0] = std::cos(el) * std::sin(az); g[1] = std::sin(el); g[2] = -std::cos(el) * std::cos(az);
}
float world_cache_age_weight(float stamp, float now) noexcept {
    if (!(stamp > 0.0f)) return 0.0f;                                             // never written (or NaN)
    const float age = std::max(now - stamp, 0.0f);
    return sat01((kCacheZeroS - age) / (kCacheZeroS - kCacheFullS));
}
CacheWindowSample world_cache_texel_window(const float lens[4], float ly, float lp, float cu, float cv) noexcept {
    CacheWindowSample o;
    float g[3], d[3];
    world_cache_uv_dir(cu, cv, g);
    world_cache_to_window(ly, lp, g, d);
    if (!(d[2] < -kWorldRayEps)) return o;
    const float tx = d[0] / -d[2], ty = d[1] / -d[2];
    const float wu = (lens[0] + tx) / (lens[0] + lens[1]), wv = (lens[2] - ty) / (lens[2] + lens[3]);
    if (!(wu >= 0.0f && wu <= 1.0f && wv >= 0.0f && wv <= 1.0f)) return o;
    o.hit = true; o.u = wu; o.v = wv;
    return o;
}
CacheRect world_cache_update_rect(const float lens[4], float ly, float lp, uint32_t cw, uint32_t ch) noexcept {
    const CacheRect full{0, 0, cw, ch};
    if (!lens || !cw || !ch || !std::isfinite(ly) || !std::isfinite(lp)) return full;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(lens[i]) || !(lens[i] > 0.0f)) return full;
    const float tx = std::max(lens[0], lens[1]), ty = std::max(lens[2], lens[3]);
    const float margin = 3.0f * std::max(2.0f * kCachePi / float(cw), kCachePi / float(ch));
    const float th = std::atan(std::sqrt(tx * tx + ty * ty)) + margin;           // cone half-angle holding the window rectangle (+ 3 texels)
    const float axis[3] = {0.0f, 0.0f, -1.0f};
    float c[3]; world_cache_to_cache(ly, lp, axis, c);
    const float elc = std::asin(clampf(c[1], -1.0f, 1.0f)), azc = std::atan2(c[0], -c[2]);
    const float eh = elc + th, el = elc - th;
    const float half_pi = 0.5f * kCachePi;
    const float v0 = 0.5f - std::min(eh, half_pi) / kCachePi, v1 = 0.5f - std::max(el, -half_pi) / kCachePi;
    CacheRect r{0, 0, cw, ch};
    r.y0 = uint32_t(clampf(std::floor(v0 * float(ch)), 0.0f, float(ch)));
    r.y1 = uint32_t(clampf(std::ceil(v1 * float(ch)), 0.0f, float(ch)));
    if (eh >= half_pi || el <= -half_pi || th >= half_pi) return r;                 // touches a pole: whole rows
    const float sd = std::sin(th) / std::cos(elc);
    if (!(sd < 1.0f)) return r;
    const float daz = std::asin(sd);
    const float u0 = 0.5f + (azc - daz) / (2.0f * kCachePi), u1 = 0.5f + (azc + daz) / (2.0f * kCachePi);
    if (!(u0 >= 0.0f && u1 <= 1.0f)) return r;                                       // wraps the az seam: whole width
    r.x0 = uint32_t(clampf(std::floor(u0 * float(cw)), 0.0f, float(cw)));
    r.x1 = uint32_t(clampf(std::ceil(u1 * float(cw)), 0.0f, float(cw)));
    return r;
}
float world_cache_horizon_conf(float best_step) noexcept { return smooth01(kCacheFillConfLo, kCacheFillConfHi, best_step); }
float world_cache_fill_t(float el, float el_horizon, float conf) noexcept {
    return smooth01(-kCacheFillBlurRad, kCacheFillBlurRad, el - el_horizon * sat01(conf));
}
Shade composite_over_world(const Shade& c, const Shade& w) noexcept {
    const float k = 1.0f - c.a;
    return {c.r + k * w.r, c.g + k * w.g, c.b + k * w.b, c.a + k * w.a};
}
Shade effects_over_world(const float rgba[4], Tonemap t, float exposure, float ramp, const Shade& world) noexcept {
    Shade e = shade_hud(rgba, t, exposure);
    e = apply_feather(e, ramp);
    return composite_over_world(e, world);
}
Shade effects_coverage_over_world(const float rgba[4], Tonemap t, float exposure, float ramp, const Shade& world) noexcept {
    const float a = rgba[3] > 0.0f ? std::min(rgba[3], 1.0f) : 0.0f; // HLSL saturate(max(a, 0)): NaN -> 0
    float rgb[3] = {0.0f, 0.0f, 0.0f};
    if (a > 0.0f) for (int i = 0; i < 3; ++i) rgb[i] = rgba[i] / a;
    const Shade s = shade(rgb, false, 0.0f, t, exposure);
    Shade e{s.r * a, s.g * a, s.b * a, a};
    e = apply_feather(e, ramp);
    return composite_over_world(e, world);
}
float clamp_feather(float f) noexcept {
    if (!(f > 0.0f)) return 0.0f; // NaN / <= 0: OFF
    return f > kMaxFeather ? kMaxFeather : f;
}
namespace {
// One axis of the ps_main edge ramp (same operations, same order as the HLSL).
float ramp_axis(float x, float f) noexcept {
    const float d = 2.0f * std::min(x, 1.0f - x); // 0 at the border, 1 at the centre
    float t = d / f;
    t = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;  // saturate (NaN -> 0)
    return t * t * (3.0f - 2.0f * t);
}
}
float edge_ramp(float u, float v, float feather) noexcept {
    if (!(feather > 0.0f)) return 1.0f; // OFF: exactly 1, the shader skips the ramp
    const float f = clamp_feather(feather);
    return ramp_axis(u, f) * ramp_axis(v, f);
}
Shade apply_feather(const Shade& s, float k) noexcept { return {s.r * k, s.g * k, s.b * k, s.a * k}; }
float clamp_quad_distance(float d) noexcept {
    if (std::isnan(d)) return kQuadDistanceDefault;
    return d < kQuadDistanceMin ? kQuadDistanceMin : d > kQuadDistanceMax ? kQuadDistanceMax : d;
}
bool cockpit_fov_ok(const float f[4]) noexcept {
    if (!f) return false;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(f[i]) || std::fabs(f[i]) > kMaxCockpitFovRad) return false;
    return f[0] < f[1] && f[3] < f[2]; // left < right, down < up
}
namespace {
float clamp_or(float v, float fallback, float lo, float hi) noexcept {
    if (std::isnan(v)) return fallback;
    return v < lo ? lo : v > hi ? hi : v;
}
}
float clamp_hud_quad_distance(float d) noexcept {
    if (!(d > 0.0f)) return 0.0f; // NaN / <= 0: unset (the world quad's distance)
    return clamp_or(d, 0.0f, kHudQuadDistanceMin, kHudQuadDistanceMax);
}
float clamp_hud_scale(float s) noexcept { return clamp_or(s, kHudScaleDefault, kHudScaleMin, kHudScaleMax); }
float clamp_hud_head_distance(float d) noexcept { return clamp_or(d, kHudHeadDistanceDefault, kHudHeadDistanceMin, kHudHeadDistanceMax); }
float clamp_hud_head_width(float w) noexcept { return clamp_or(w, kHudHeadWidthDefault, kHudHeadWidthMin, kHudHeadWidthMax); }
Quad scale_quad(const Quad& q, float s) noexcept {
    Quad out = q;
    if (s == 1.0f) return out; // exact: the unscaled quad
    out.width = q.width * s;
    out.height = q.height * s;
    return out;
}
Quad hud_world_quad(const float n[3], const float t[4], float world_d, float hud_d, float s) noexcept {
    // world_quad() at the HUD distance: centre = n + D*(cx, cy, -1) and size D*(l+r) x D*(u+d), so
    // every corner keeps its direction from the anchor (tan l, r, u, d) at any D.
    return scale_quad(world_quad(n, t, hud_d > 0.0f ? hud_d : world_d), s);
}
Quad hud_world_quad(const float n[3], const float t[4], float world_d, float hud_d, float s, float yaw) noexcept {
    // Same rule in the turned frame: every corner keeps its direction from the anchor at any D.
    return scale_quad(world_quad(n, t, hud_d > 0.0f ? hud_d : world_d, yaw), s);
}
Quad hud_world_quad(const float n[3], const float t[4], float world_d, float hud_d, float s, float yaw, float pitch) noexcept {
    return scale_quad(world_quad(n, t, hud_d > 0.0f ? hud_d : world_d, yaw, pitch), s);
}
Quad hud_head_quad(float d, float w, float s, uint32_t sw, uint32_t sh) noexcept {
    Quad q;
    q.pose.position[2] = -d;
    q.width = w * s;
    q.height = sw ? q.width * float(sh) / float(sw) : 0.0f;
    return q;
}
std::string hud_placement_tokens(const Settings& s) {
    char text[160];
    char dist[24] = "world";
    if (s.hud_quad_distance_m > 0.0f) std::snprintf(dist, sizeof dist, "%.2f", double(s.hud_quad_distance_m));
    std::snprintf(text, sizeof text, ";hud-quad-distance-m=%s;hud-scale=%.3f;hud-head-distance-m=%.2f;hud-head-width-m=%.2f",
        dist, double(s.hud_scale), double(s.hud_head_distance_m), double(s.hud_head_width_m));
    return text;
}
const char* hud_mode_name(HudMode m) noexcept { return m == HudMode::head_locked ? "head-locked" : "world-quad"; }
Quad view_quad(const xr_owner::EyeSample& v, float distance_m) noexcept {
    const float tl = std::tan(v.fov.left), tr = std::tan(v.fov.right), tu = std::tan(v.fov.up), td = std::tan(v.fov.down);
    const float d = distance_m;
    // view-space centre of the image plane (x right, y up, the eye looks along -Z)
    const float o[3] = {d * 0.5f * (tr + tl), d * 0.5f * (tu + td), -d};
    const float qx = v.pose.orientation[0], qy = v.pose.orientation[1], qz = v.pose.orientation[2], qw = v.pose.orientation[3];
    // rotate o by the unit quaternion: o + 2 w (q x o) + 2 q x (q x o)
    const float cx = qy * o[2] - qz * o[1], cy = qz * o[0] - qx * o[2], cz = qx * o[1] - qy * o[0];
    const float dx = qy * cz - qz * cy, dy = qz * cx - qx * cz, dz = qx * cy - qy * cx;
    Quad out;
    out.pose.orientation[0] = qx; out.pose.orientation[1] = qy; out.pose.orientation[2] = qz; out.pose.orientation[3] = qw;
    out.pose.position[0] = v.pose.position[0] + o[0] + 2.0f * (qw * cx + dx);
    out.pose.position[1] = v.pose.position[1] + o[1] + 2.0f * (qw * cy + dy);
    out.pose.position[2] = v.pose.position[2] + o[2] + 2.0f * (qw * cz + dz);
    out.width = d * (tr - tl);
    out.height = d * (tu - td);
    return out;
}
LayerList compose_layers(const xr_owner::ProjectionLayer& l) noexcept {
    LayerList out;
    // VSCREEN: the game's screen alone (opaque quad from the HUD swapchain, LOCAL); nothing else is submitted with it.
    if (l.screen_only) {
        // UI3D 2026-10-30b (table_front): SteamVR draws quad layers ABOVE projection layers, so a screen quad would cover any projection under
        // it (the user saw the flat game image in front of the hangar room and of the briefing table's raised markers).
        if (l.table && l.table_front && l.table_opaque) {   // an opaque menu / hangar slot covers the whole view: nothing else to show
            out.e[out.count++] = {LayerKind::projection, 0, false, false, LayerSpace::local};
            return out;
        }
        out.e[out.count++] = {LayerKind::screen_quad, kHud, false, false, LayerSpace::local};
        if (l.table && l.table_front) {   // the briefing table: one premultiplied quad per eye, after (above) the screen quad
            out.e[out.count++] = {LayerKind::table_quad, 0, true, false, LayerSpace::local};
            out.e[out.count++] = {LayerKind::table_quad, 1, true, false, LayerSpace::local};
            return out;
        }
        // BRIEF3D: the map table on top (premultiplied); UI3D: the menu layers, opaque (covering the quad)
        if (l.table) out.e[out.count++] = {LayerKind::projection, 0, !l.table_opaque, false, LayerSpace::local};
        return out;
    }
    // Back to front: world quad (opaque, LOCAL; premultiplied SOURCE_ALPHA only with the
    // J-feather edge ramp) -> [marker quad] -> cockpit projection -> HUD quad.
    if (l.quad) out.e[out.count++] = {LayerKind::world_quad, kQuad, l.quad_source_alpha, false, LayerSpace::local};
    // J-hud-markers: between the world quad and the cockpit projection (premultiplied, never UNPREMULTIPLIED).
    // With xr_world_in_projection (l.world_composite) there is no world quad layer: the marker quad is an overlay ABOVE the projection.
    if (l.markers && !l.world_composite) out.e[out.count++] = {LayerKind::marker_quad, kMarkers, true, false, LayerSpace::local};
    out.e[out.count++] = {LayerKind::projection, 0, l.source_alpha, false, LayerSpace::local};
    if (l.markers && l.world_composite) out.e[out.count++] = {LayerKind::marker_quad, kMarkers, true, false, LayerSpace::local};
    if (l.hud) out.e[out.count++] = {LayerKind::hud_quad, kHud, true, false, l.hud_view_space ? LayerSpace::view : LayerSpace::local};
    return out;
}
int64_t select_format(const int64_t* formats, uint32_t count) noexcept {
    if (!formats) return 0;
    for (int64_t preferred : {rgba8_srgb, rgba8_unorm})
        for (uint32_t i = 0; i < count; ++i) if (formats[i] == preferred) return preferred;
    return 0;
}
bool source_format_ok(uint32_t f) noexcept { return f == 10 || f == 2; }

namespace {
bool eye_ok(const stereo::EyePose& e) noexcept {
    xr_owner::Pose p;
    for (int i = 0; i < 3; ++i) p.position[i] = e.position[i];
    for (int i = 0; i < 4; ++i) p.orientation[i] = e.orientation[i];
    if (!xr_owner::pose_finite(p)) return false;
    for (float f : e.fov) if (!std::isfinite(f)) return false;
    return e.fov[0] < e.fov[1] && e.fov[3] < e.fov[2];
}
xr_owner::EyeSample sample_of(const stereo::EyePose& e) noexcept {
    xr_owner::EyeSample s;
    for (int i = 0; i < 3; ++i) s.pose.position[i] = e.position[i];
    for (int i = 0; i < 4; ++i) s.pose.orientation[i] = e.orientation[i];
    s.fov = {e.fov[0], e.fov[1], e.fov[2], e.fov[3]};
    return s;
}
std::string u64(uint64_t v) { return std::to_string(v); }
double deg(float r) { return double(r) * 57.29577951308232; }
const char* target_name(unsigned t) { return t == 0 ? "eye0" : t == 1 ? "eye1" : t == kQuad ? "quad" : "hud"; }
const char* cockpit_fov_name(unsigned s) { return s == 2 ? "hmd" : s == 1 ? "lens" : "pose"; }
// Layer order text. Without markers exactly the pre-J-hud-markers strings.
const char* layer_order(const xr_owner::ProjectionLayer& l, bool hud) {
    if (l.markers) {
        if (l.world_composite) return hud ? "projection-then-markers-then-hud" : "projection-then-markers";
        if (l.quad) return hud ? "quad-then-markers-then-projection-then-hud" : "quad-then-markers-then-projection";
        return hud ? "markers-then-projection-then-hud" : "markers-then-projection";
    }
    if (l.quad) return hud ? "quad-then-projection-then-hud" : "quad-then-projection";
    return hud ? "projection-then-hud" : "projection";
}
}

std::string Output::layer_fov_line(const xr_owner::LayerPlan& plan) const {
    // J-layer-fov (diagnostics): the SUBMITTED projection fov, the swapchain it samples into and the pixel density per unit tangent in
    // x and y. A rectilinear image uniform in tan space has px-per-tan = extent / tan-span; the ratio x/y is the anisotropy (1 = isotropic).
    const auto& v = plan.layer.views;
    char text[440];
    int n = std::snprintf(text, sizeof text, "xr-stereo;event=layer-fov;swapchain=%ux%u;cockpit-fov=%s", ext_[0].w, ext_[0].h, cockpit_fov_name(cockpit_fov_source_));
    for (unsigned e = 0; e < 2 && n > 0 && size_t(n) < sizeof text; ++e) {
        const double tl = std::tan(double(v[e].fov.left)), tr = std::tan(double(v[e].fov.right)), tu = std::tan(double(v[e].fov.up)), td = std::tan(double(v[e].fov.down));
        const double sx = tr - tl, sy = tu - td;
        int m;
        if (sx > 0 && sy > 0 && std::isfinite(sx) && std::isfinite(sy) && ext_[e].w && ext_[e].h) {
            const double px = double(ext_[e].w) / sx, py = double(ext_[e].h) / sy;
            m = std::snprintf(text + n, sizeof text - size_t(n),
                ";eye%u-fov-deg=%.2f,%.2f,%.2f,%.2f;eye%u-tan-span=%.4fx%.4f;eye%u-tan-aspect=%.4f;eye%u-px-per-tan=%.1fx%.1f;eye%u-px-per-tan-ratio-x-over-y=%.3f",
                e, deg(v[e].fov.left), deg(v[e].fov.right), deg(v[e].fov.up), deg(v[e].fov.down), e, sx, sy, e, sx / sy, e, px, py, e, px / py);
        } else m = std::snprintf(text + n, sizeof text - size_t(n), ";eye%u-fov=unavailable", e);
        n = m > 0 ? n + m : n;
    }
    if (n > 0 && size_t(n) + 40 < sizeof text) std::snprintf(text + n, sizeof text - size_t(n), ";pixel-aspect=%.4f;no-headset-claim",
        ext_[0].h ? double(ext_[0].w) / double(ext_[0].h) : 0.0);
    return text;
}

std::string Output::eye_fov_tokens() const {
    // The HEADSET's own per-eye FOV (raw located views, degrees), not what the layer submits.
    if (!hmd_fov_valid_) return ";eye0-fov=none;eye1-fov=none";
    char text[128];
    const auto& a = hmd_fov_[0];
    const auto& b = hmd_fov_[1];
    std::snprintf(text, sizeof text, ";eye0-fov=%.2f,%.2f,%.2f,%.2f;eye1-fov=%.2f,%.2f,%.2f,%.2f",
        deg(a.left), deg(a.right), deg(a.up), deg(a.down), deg(b.left), deg(b.right), deg(b.up), deg(b.down));
    return text;
}

// ------------------------------------------------------------- consumer ---
Output::Output(SourceProvider p, std::shared_ptr<SwapchainRuntime> xr, std::shared_ptr<BlitBackend> blit, Log log, Settings s)
    : provider_(std::move(p)), xr_(std::move(xr)), blit_(std::move(blit)), log_(std::move(log)), s_(s) {
    // J-feather: the shader divides by the feather; the quad distance stays inside the key's range.
    s_.edge_feather = clamp_feather(s_.edge_feather);
    s_.quad_distance_m = clamp_quad_distance(s_.quad_distance_m);
    // J-hud-placement: defaults (0 / 1 / 2.5 / 1.6) pass unchanged.
    s_.hud_quad_distance_m = clamp_hud_quad_distance(s_.hud_quad_distance_m);
    s_.hud_scale = clamp_hud_scale(s_.hud_scale);
    s_.hud_head_distance_m = clamp_hud_head_distance(s_.hud_head_distance_m);
    s_.hud_head_width_m = clamp_hud_head_width(s_.hud_head_width_m);
    // J-hud-markers: world-registered markers need the HUD layer in world-quad mode (head-locked has no world alignment).
    if (!s_.hud_layer || s_.hud_mode == HudMode::head_locked) s_.hud_markers_layer = false;
    if (!s_.hud_layer) s_.vscreen.reset();   // VSCREEN: the screen image is carried by the HUD swapchain
    // J-world-in-projection: the world source must exist; without it there is nothing to composite.
    s_.world_in_projection = s_.world_in_projection && s_.world_quad;
    // J-world-hold / J-world-extend: both act on the in-projection composite only.
    s_.world_hold_last = s_.world_hold_last && s_.world_in_projection;
    s_.world_extend = s_.world_extend && s_.world_in_projection;
    s_.world_cache = s_.world_cache && s_.world_in_projection; // J-world-cache: acts on the in-projection composite only
    s_.world_window_occlude = s_.world_window_occlude && s_.world_in_projection; // J-occlusion-fix: acts on the in-projection composite only
    s_.effects_layer = s_.effects_layer && s_.world_in_projection; // J-effects-layer: acts on the in-projection composite only
    if (!s_.world_in_projection) s_.terrain.reset();       // J-terrain-render: acts on the in-projection composite only
    if (!s_.world_in_projection) s_.cockpit.reset();       // COCKPIT-2: uses the composite's views/anchor (and its replay-drop bit)
    if (!s_.world_in_projection) s_.cockpit_auto.reset();  // AIRCRAFT-AUTO: same rule
    if (s_.cockpit_auto) {   // auto installs its own models
        s_.cockpit.reset(); auto_det_ = acz::aircraft::Detector(s_.cockpit_auto->det);
        // AIRCRAFT-AUTO-FAST: the assumed type's model is loaded HERE (the XR monitor thread, before the session exists; never inside a frame) so the
        // very first composed frame already draws it instead of the game's forward-only replayed cockpit. A failed load = the plain detector.
        const CockpitAuto& ca = *s_.cockpit_auto;
        auto_remembered_ = ca.remembered;
        const int t = ca.assumed;
        if (t >= 0 && unsigned(t) < acz::aircraft::kAircraftCount) {
            AutoLoad r;
            const auto t0 = std::chrono::steady_clock::now();
            try { r.rt = ca.load ? ca.load(unsigned(t), r.why) : load_auto_model(ca.cfg, unsigned(t), ca.model_dir, r.why); } catch (...) { r.rt.reset(); r.why = "loader-exception"; }
            if (!r.rt && r.why.empty()) r.why = "loader-returned-null";
            auto_assume_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++auto_loads_;
            char at[480];
            if (r.rt) {
                auto_det_.assume(t);
                s_.cockpit = r.rt; auto_installed_ = t;
                std::snprintf(at, sizeof at, "xr-stereo;event=cockpit-auto;action=assume;type=%s;model=%s;source=%s;installed=yes;triangles=%u;generation=%llu;load-ms=%.1f;"
                    "thread=xr-monitor(before-session);drawn-from=first-frame;detector=confirm-or-switch", acz::aircraft::kAircraft[t].type, r.rt->id.c_str(),
                    ca.assumed_source.empty() ? "unknown" : ca.assumed_source.c_str(), r.rt->mesh.triangles, (unsigned long long)r.rt->generation, auto_assume_ms_);
            } else {
                ++auto_load_failures_; auto_failed_mask_ |= uint64_t(1) << unsigned(t);
                std::snprintf(at, sizeof at, "xr-stereo;event=cockpit-auto;action=assume-failed;type=%s;model=%s;source=%s;reason=%s;fallback=detect-from-E6(replayed-cockpit-until-then)",
                    acz::aircraft::kAircraft[t].type, acz::aircraft::kAircraft[t].model_id, ca.assumed_source.empty() ? "unknown" : ca.assumed_source.c_str(), r.why.c_str());
            }
            auto_assume_line_ = at;
        }
    }
    if (s_.debug_view > kDebugViewMax) s_.debug_view = 0; // J-test8-fixes: out of range = off
    if (!s_.png_sink) s_.png_once = false;                // nowhere to hand the pixels: off
}

void Output::emit(bool lifecycle, const std::string& line) const {
    if (log_) try { log_(lifecycle, line.c_str()); } catch (...) {}
}
void Output::fail(const char* why) {
    if (!failed_.exchange(true, std::memory_order_acq_rel)) {
        reason_ = why ? why : "unknown";
        emit(true, "xr-stereo;event=disabled;reason=" + reason_ + ";frame=" + u64(c_.frames) +
            ";submitted=" + u64(c_.submitted) + ";quarantine=" + ((l_.submitted && !l_.complete) ? "1" : "0") +
            ";held-slot=" + (l_.holding ? "1" : "0"));
    }
}

void Output::cockpit_config_lines() {
    const auto& ck = *s_.cockpit;
    char ct[900];
    std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model;model=%s;triangles=%u;parts-drawn=%u;parts-hidden=%u;textures=%u;eye=%.3f,%.3f,%.3f;upm=%.2f;cscale=%.3f(about-pvb-eye);gain=%.2f;amb=%.2f;cut=%.2f;"
        "near=%.3f;exterior=%u;glass=%u;replay=%s;hud-glass=%s;dom=%s;light=%s;floor=%s;frame=cockpit(seat-anchor,yaw0;no-game-look);"
        "pass=opaque+blend,reversed-z,premultiplied-over-eye-composite;no-headset-claim",
        ck.id.c_str(), ck.mesh.triangles, ck.mesh.parts_drawn, ck.mesh.parts_hidden, unsigned(ck.mesh.textures.size()), double(ck.mesh.eye[0]), double(ck.mesh.eye[1]),
        double(ck.mesh.eye[2]), double(ck.opt.upm), double(ck.opt.cscale), double(ck.opt.gain), double(ck.opt.ambient), double(ck.opt.cut), double(ck.opt.near_u), ck.opt.exterior ? 1u : 0u,
        ck.opt.glass ? 1u : 0u, ck.opt.keep_replay ? "kept-under-model(A/B)" : "cockpit-band-dropped(objects-kept)", ck.mesh.hud.valid ? "found" : "none",
        ck.opt.display ? "display(srgb-eotf-once)" : "raw(COCKPIT-2)", ck.opt.stage_light ? "stage-sun(else-fixed)" : "fixed",
        ck.mesh.floor ? "closed" : "open");
    emit(true, ct);
    // COCKPIT-4: canopy glass / procedural interior / animation
    std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-c4;canopy=%s;canopy-parts=%u;glass-triangles=%u;tint=%.3f,%.3f,%.3f(display);refl=%.2f;dirt=%.2f;"
        "interior=%s;interior-triangles=%u;interior-floor-y=%.3f;anim=%s;anim-surfaces=%u;anim-not-driven=%u;anim-pieces=%u;anim-vertices=%u;"
        "seatback=%.3fm(headrest-front-model;physical=%.3fm-at-cscale);"
        "anim-source=E6-attitude-rates(approximation);made-up=interior+commands;no-headset-claim",
        ck.mesh.canopy_parts ? (ck.mesh.canopy_exterior ? "exterior-flipped" : "cockpit-part") : (ck.opt.canopy ? "none-found" : "off(straight-alpha)"),
        ck.mesh.canopy_parts, ck.mesh.glass_vertices / 3u, double(ck.opt.tint[0]), double(ck.opt.tint[1]), double(ck.opt.tint[2]), double(ck.opt.refl), double(ck.opt.dirt),
        ck.mesh.interior ? "procedural" : "off", ck.mesh.interior_triangles, double(ck.mesh.interior_floor_y), ck.opt.anim ? "on" : "off(rest-pose)",
        ck.mesh.anim_surfaces, ck.mesh.anim_unlocated, unsigned(ck.mesh.anim.size()), ck.mesh.anim_vertices,
        double(ck.opt.seat_back), double(ck.mesh.seat_front_u * ck.opt.cscale / ck.opt.upm));
    emit(true, ct);
    // COCKPIT-CLIP (cockpit-render-impl.md section 16): interior fitted into the airframe + per-type clip box
    std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-model-clip;clip=%s;box-m=%.2f,%.2f,%.2f,%.2f,%.2f(ahead,behind,below,above,half);game-removed=%u;game-cut=%u;"
        "interior-removed=%u;interior-cut=%u;fit-moved=%u;fit-skin=%u", ck.opt.clip ? (ck.mesh.clip_box ? "on(fit+box)" : "on(fit;no-box)") : "off",
        double(ck.opt.clip_box[0]), double(ck.opt.clip_box[1]), double(ck.opt.clip_box[2]), double(ck.opt.clip_box[3]), double(ck.opt.clip_box[4]),
        ck.mesh.clip_removed, ck.mesh.clip_cut, ck.mesh.clip_interior_removed, ck.mesh.clip_interior_cut, ck.mesh.fit_moved, ck.mesh.fit_skin);
    emit(true, ct);
    // COCKPIT-3: the HUD symbology registers with the model cockpit (gunsight glass) and the world only at the game's own angular size (scale 1).
    if (s_.hud_layer && s_.hud_mode == HudMode::world_quad && std::fabs(s_.hud_scale - 1.0f) > 1e-3f) {
        // HUD-MENU: a scale != 1 is now pivoted about the glass's angular centre (hud_menu.h); world-referenced symbols off the pivot read (1-s) of their angle.
        float px = 0.0f, py = 0.0f;
        if (ck.mesh.hud.valid) { const float gl[4] = {ck.mesh.hud.tan_l, ck.mesh.hud.tan_r, ck.mesh.hud.tan_u, ck.mesh.hud.tan_d}; acz::hudmenu::glass_pivot(gl, px, py); }
        std::snprintf(ct, sizeof ct, "xr-stereo;event=cockpit-hud-registration;hud-scale=%.3f;pivot=%s;pivot-deg=%.2f,%.2f;registered=pivot-only;"
            "error-at-10deg=%.2fdeg;effect=hud-angular-size-is-scale-x-the-game's(world-symbols-compressed-toward-the-pivot,markers-layer-unscaled);exact=xr_hud_scale=1",
            double(s_.hud_scale), ck.mesh.hud.valid ? "glass-centre" : "lens-centre(no-glass)", std::atan(double(px)) * 57.29578, std::atan(double(py)) * 57.29578,
            acz::hudmenu::registration_error_deg(0.0, std::tan(10.0 / 57.29578), double(s_.hud_scale)));
        emit(true, ct);
    }
    {
        const auto& o = ck.opt;
        std::snprintf(ct, sizeof ct, "xr-stereo;event=hud-menu-config;menu=%u;menu-distance-m=%.2f;menu-backdrop=%.2f;enter=frozen-camera+hud-draws>=max(%u,baseline+%u)-for-%u-frames;"
            "exit=no-signature-%u-or-moving-%u-frames;menu-scale=1(game-angular-size,lens-centred);needs=xr_hud_layer(world-quad);radar-bg=%u;radar-bg-alpha=%.4f",
            o.menu ? 1u : 0u, double(o.menu_distance),
            double(o.menu_backdrop), acz::hudmenu::kMenuMinDraws, acz::hudmenu::kMenuExtraDraws, acz::hudmenu::kMenuEnterFrames,
            acz::hudmenu::kMenuExitSignatureFrames, acz::hudmenu::kMenuExitMovingFrames, o.radar_bg ? 1u : 0u,
            double(o.radar_bg ? acz::hudmenu::radar_bg_of_bits(acz::hudmenu::radar_bg_bits(o.radar_bg_alpha)) : 0.0f));
        emit(true, ct);
    }
}

// HUD-MENU (hud_menu.h; docs/world-render/hud-menu.md) ----------------------------------------------------------------------------------------------------
const acz::cockpit::Options* Output::hud_menu_options() const noexcept {
    if (s_.cockpit) return &s_.cockpit->opt;
    if (s_.cockpit_auto) return &s_.cockpit_auto->cfg.opt;
    return nullptr;
}
void Output::hud_menu_frame(const acz::stereo::StereoFrame& f, uint32_t hud_draws) {
    const acz::cockpit::Options* o = hud_menu_options();
    const bool was = menu_on_;
    const bool cam = f.game_eye_valid && f.game_cam_valid;
    const uint32_t thr = menu_det_.threshold();   // the threshold this frame is judged against (before its baseline update)
    const bool frozen = menu_det_.update(cam, f.game_eye, f.game_cam_fwd, hud_draws);
    menu_on_ = frozen && o && o->menu && s_.hud_layer && s_.hud_mode == HudMode::world_quad;
    acz::ui3d::pause::publish(menu_on_);   // UI3D stage 3a (u3p:1): the producer's pause trigger (a heartbeat per fresh frame; unused otherwise)
    if (menu_on_) ++menu_frames_;
    if (menu_on_ != was && menu_logged_ < 40) {
        ++menu_logged_;
        char t[300];
        std::snprintf(t, sizeof t, "xr-stereo;event=hud-menu;state=%s;hud-draws=%u;threshold=%u;baseline=%.1f;frozen-rejected=%llu;enters=%llu;exits=%llu;menu-frames=%llu;"
            "distance-m=%.2f;backdrop=%.2f",
            menu_on_ ? "enter(frozen-camera+menu-draws)" : "exit", hud_draws, thr, menu_det_.baseline(),
            static_cast<unsigned long long>(menu_det_.frozen_rejected()), static_cast<unsigned long long>(menu_det_.enters()),
            static_cast<unsigned long long>(menu_det_.exits()), static_cast<unsigned long long>(menu_frames_),
            o ? double(o->menu_distance) : 0.0, o ? double(o->menu_backdrop) : 0.0);
        emit(true, t);
    }
}
void Output::hud_quad_params(const float lens[4], float out_lens[4], float& scale, float& distance) noexcept {
    for (int i = 0; i < 4; ++i) out_lens[i] = lens[i];
    scale = s_.hud_scale;
    distance = s_.hud_quad_distance_m;
    const acz::cockpit::Options* o = hud_menu_options();
    if (menu_on_ && o) { scale = 1.0f; distance = clamp_hud_quad_distance(o->menu_distance); return; }   // the game's angular size, lens-centred, nearer
    if (scale != 1.0f && s_.cockpit && s_.cockpit->mesh.hud.valid) {
        const auto& h = s_.cockpit->mesh.hud;
        const float gl[4] = {h.tan_l, h.tan_r, h.tan_u, h.tan_d};
        float px = 0.0f, py = 0.0f;
        acz::hudmenu::glass_pivot(gl, px, py);
        if (acz::hudmenu::scale_lens_about(lens, scale, px, py, out_lens)) { scale = 1.0f; ++menu_glass_frames_; }
    }
}

// ------------------------------------------------------------- AIRCRAFT-AUTO (xr_cockpit_model=auto; docs/world-render/aircraft-auto.md) ---
std::shared_ptr<CockpitRuntime> load_auto_model(const acz::aircraft::AutoConfig& cfg, unsigned type, const std::string& model_dir, std::string& why) {
    static std::atomic<uint64_t> s_gen{uint64_t(1) << 20};   // never the fixed model's generation 1; unique per load (the backend keys its GPU copy on it)
    if (type >= acz::aircraft::kAircraftCount) { why = "bad-type"; return nullptr; }
    auto rt = std::make_shared<CockpitRuntime>();
    const std::string id = acz::aircraft::kAircraft[type].model_id;
    rt->opt = acz::aircraft::options_for(cfg, type);
    acz::cockpit::apply_clip_table(rt->opt, id);   // COCKPIT-CLIP: the type's clip volume
    const std::string path = acz::cockpit::model_path(id, model_dir);
    acz::cockpit::ModelData model;
    const acz::cockpit::LoadStatus st = acz::cockpit::load_model_file(path, model);
    if (st != acz::cockpit::LoadStatus::ok) { why = std::string("load-failed;status=") + acz::cockpit::status_name(st) + ";path=" + path; return nullptr; }
    std::string w;
    if (!acz::cockpit::build_mesh(model, rt->opt, rt->mesh, w)) { why = "mesh;detail=" + w; return nullptr; }
    rt->id = id;
    rt->generation = s_gen.fetch_add(1) + 1;
    return rt;
}

namespace {
std::string type_list(uint64_t mask) {
    std::string s;
    for (unsigned i = 0; i < acz::aircraft::kAircraftCount; ++i)
        if (mask & (uint64_t(1) << i)) { if (!s.empty()) s += '+'; s += acz::aircraft::kAircraft[i].type; }
    return s.empty() ? std::string("none") : s;
}
const char* type_name(int i) { return i >= 0 && unsigned(i) < acz::aircraft::kAircraftCount ? acz::aircraft::kAircraft[i].type : "none"; }
}

void Output::cockpit_auto_install(AutoLoad&& r, int type) {
    char t[480];
    if (!r.rt) {
        ++auto_load_failures_;
        if (type >= 0) auto_failed_mask_ |= uint64_t(1) << unsigned(type);   // not retried this session; the next frames unload a wrong model
        std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=load-failed;type=%s;model=%s;reason=%s;fallback=replayed-cockpit;"
            "hint=tools/live/install-assets.ps1-ModelDir-models-all", type_name(type), type >= 0 ? acz::aircraft::kAircraft[type].model_id : "none", r.why.c_str());
        emit(true, t);
        return;
    }
    if (type != auto_det_.current()) {   // the detector moved on while this model loaded
        std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=discarded;type=%s;now-wanted=%s;load-ms=%.1f", type_name(type), type_name(auto_det_.current()), r.ms);
        emit(true, t);
        return;
    }
    const bool replaced = auto_installed_ >= 0;
    s_.cockpit = r.rt;
    auto_installed_ = type;
    anim_state_ = acz::cockpit::AnimState{}; anim_last_seq_ = 0; anim_pose_.clear(); cockpit_fail_logged_ = false;   // per-model state starts over
    stores_gen_ = ~0ull;   // STORES-RT: the tracker is configured for the new generation at the next job
    std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=installed;type=%s;model=%s;triangles=%u;stick=%s;generation=%llu;load-ms=%.1f;replaced=%s;thread=%s",
        type_name(type), r.rt->id.c_str(), r.rt->mesh.triangles, r.rt->opt.stick == 1 ? "side" : r.rt->opt.stick == 2 ? "none" : "centre",
        (unsigned long long)r.rt->generation, r.ms, replaced ? "yes" : "no", s_.cockpit_auto->async ? "worker" : "inline");
    emit(true, t);
    cockpit_config_lines();
}

void Output::cockpit_auto_remember(int type) {
    if (type < 0 || unsigned(type) >= acz::aircraft::kAircraftCount || type == auto_remembered_) return;
    const std::string path = s_.cockpit_auto->state_path;
    if (path.empty()) return;
    auto_remembered_ = type; ++auto_writes_;
    char t[400];
    std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=remember;type=%s;model=%s;file=%s;write=temp+rename;thread=%s", acz::aircraft::kAircraft[type].type,
        acz::aircraft::kAircraft[type].model_id, path.c_str(), s_.cockpit_auto->async ? "detached-worker" : "inline");
    emit(true, t);
    auto job = [path, type, st = auto_wstats_]() {
        std::string why;
        bool ok = false;
        try { ok = acz::aircraft::write_remembered(path, unsigned(type), why); } catch (...) { ok = false; }
        (ok ? st->ok : st->failed).fetch_add(1);
    };
    if (s_.cockpit_auto->async) {
        try { std::thread(std::move(job)).detach(); return; } catch (...) { auto_wstats_->failed.fetch_add(1); return; }   // never inside the frame
    }
    job();
}

void Output::cockpit_auto_feed(const acz::aircraft::EyeSample& e) {
    namespace A = acz::aircraft;
    const CockpitAuto& ca = *s_.cockpit_auto;
    auto_last_seq_ = e.seq; ++auto_samples_;
    // AIRCRAFT-AUTO-JUNK: a look-centred sample far from every table eye is a junk lock (not the aircraft camera): fed (no-match windows) but it never
    // vouches for the shown aircraft (stale clock). Off-centre samples cannot be judged (the look pivots the eye) and vouch as before.
    if (e.centred && !A::plausible_eye(e.local)) ++auto_implausible_; else ++auto_vouch_samples_;
    const int before = auto_det_.current();
    const bool was_assumed = auto_det_.assumed();
    if (!auto_det_.feed(e.local, e.centred, e.lock_epoch)) return;
    char t[480];
    const A::WindowResult& w = auto_det_.last();
    if (auto_det_.changed() || auto_det_.confirmed() || w.verdict != auto_logged_verdict_ || w.chosen != auto_logged_chosen_) {   // log changes only (summary counts the rest)
        auto_logged_verdict_ = w.verdict; auto_logged_chosen_ = w.chosen;
        const bool tie = w.ties && (w.ties & (w.ties - 1));
        std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;window=%llu;verdict=%s;candidates=%s:%.3f,%s:%.3f;chosen=%s;model=%s;confidence=%.2f;ties=%s;"
            "tie-rule=%s;samples=%u;spread=%.3f;eye=%.3f,%.3f,%.3f;current=%s;switched=%s",
            (unsigned long long)auto_det_.windows, A::verdict_name(w.verdict), type_name(w.best), double(w.best_res), type_name(w.second), double(w.second_res),
            type_name(w.chosen), w.chosen >= 0 ? A::kAircraft[w.chosen].model_id : "none", double(w.confidence), tie ? type_list(w.ties).c_str() : "none",
            !tie ? "-" : (w.ties & ca.det.prefer) ? "prefer" : "first-in-table", w.samples, double(w.spread), double(w.median[0]), double(w.median[1]),
            double(w.median[2]), type_name(auto_det_.current()), auto_det_.changed() ? "yes" : "no");
        emit(true, t);
    }
    if (auto_det_.confirmed()) {   // AIRCRAFT-AUTO-FAST: the assumed model is the detected one
        ++auto_confirms_;
        std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=confirm;type=%s;model=%s;window=%llu;residual=%.3f;source=%s", type_name(auto_det_.current()),
            A::kAircraft[auto_det_.current()].model_id, (unsigned long long)auto_det_.windows, double(w.best_res), ca.assumed_source.empty() ? "unknown" : ca.assumed_source.c_str());
        emit(true, t);
        cockpit_auto_remember(auto_det_.current());
    } else if (auto_det_.changed()) {
        if (before >= 0 && auto_det_.current() >= 0) {   // e.g. the remembered/default aircraft was wrong: confirmed windows of another type replace it
            std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=switch;from=%s;to=%s;reason=%s;windows=%u;window=%llu", type_name(before),
                type_name(auto_det_.current()), was_assumed ? "assumed-type-contradicted" : "detected-type-changed", ca.det.confirm, (unsigned long long)auto_det_.windows);
            emit(true, t);
        }
        cockpit_auto_remember(auto_det_.current());   // a confirmed (first or switched) type; -1 (no-match) is never remembered
    }
}

void Output::cockpit_auto_tick() {
    namespace A = acz::aircraft;
    const CockpitAuto& ca = *s_.cockpit_auto;
    char t[480];
    // 1. a finished background load
    if (auto_future_.valid() && auto_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        AutoLoad r;
        try { r = auto_future_.get(); } catch (...) { r.rt.reset(); r.why = "worker-exception"; }
        const int type = auto_loading_;
        auto_loading_ = -1;
        cockpit_auto_install(std::move(r), type);
    }
    // 2. the eye samples of the locked E6 block: EVERY sample published since the last tick (AIRCRAFT-AUTO-FAST: per E6 sample, not per composed
    //    frame); a test feed (ca.sample) still hands over the newest one per tick. Each sample is fed once, by sequence.
    if (ca.sample) {
        A::EyeSample e;
        if (ca.sample(e) && e.seq != auto_last_seq_) cockpit_auto_feed(e);
    } else {
        A::EyeSample buf[A::EyeMailbox::kRing];
        const unsigned n = ca.drain ? ca.drain(auto_last_seq_, buf, A::EyeMailbox::kRing) : A::eye_mailbox().drain(auto_last_seq_, buf, A::EyeMailbox::kRing, &auto_lost_);
        for (unsigned i = 0; i < n && i < A::EyeMailbox::kRing; ++i) if (buf[i].seq > auto_last_seq_) cockpit_auto_feed(buf[i]);
    }
    // 2b. AIRCRAFT-AUTO-STALE (headset test 2026-10-29 item 9): the model drawn from the remembered file / default: (or kept from an earlier mission) is only
    //     trusted while the locked E6 block keeps delivering samples. In 'The Round Table' the F-5E mission never re-locked the block (ee-camera.log: search,
    //     'static'), no window ever ran and the remembered Draken stayed on the F-5 cockpit. No sample for `stale:` ms = forget the choice: the game's own
    //     replayed cockpit (never a model of an unverified aircraft); the next matching window picks the real type.
    {
        const double now = ca.now_ms ? ca.now_ms() : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        //     AIRCRAFT-AUTO-JUNK (headset 2026-10-30 live-1508): only PLAUSIBLE samples refresh the clock; a lock on a static junk block (eye 0,0,-60) kept
        //     delivering samples, so the remembered Draken was never forgotten in the F-5E.
        if (auto_last_sample_ms_ < 0 || auto_vouch_samples_ != auto_stale_seen_samples_) {
            auto_last_sample_ms_ = now; auto_stale_seen_samples_ = auto_vouch_samples_; auto_stale_seen_junk_ = auto_implausible_;
        }
        //     AIRCRAFT-AUTO-GRACE (headset test 7, 2026-10-31, F-5E): the E6 locator needs 20-25 s of mission to lock (xr-stereo.log: first frame 236876375, locked
        //     236899658), longer than stale: 15 s, so the remembered AND correct f05e was forgotten at +15.0 s and the cockpit, airframe and wings were gone for 8.4 s
        //     until window 1 matched. A model that is only assumed (no window confirmed it) and has seen no implausible sample is kept for max(stale, grace).
        const bool junk = auto_implausible_ != auto_stale_seen_junk_;
        const double limit = (auto_det_.assumed() && !junk && ca.grace_ms > ca.stale_ms) ? double(ca.grace_ms) : double(ca.stale_ms);
        if (ca.stale_ms > 0 && auto_det_.current() >= 0 && now - auto_last_sample_ms_ > limit) {
            std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=stale-forget;type=%s;was=%s;silent-ms=%.0f;limit-ms=%u;reason=%s;"
                "fallback=replayed-cockpit;next=first-matching-window", type_name(auto_det_.current()), auto_det_.assumed() ? "assumed" : "confirmed",
                now - auto_last_sample_ms_, unsigned(limit), junk ? "implausible-e6-eye(locked-block-not-the-aircraft)" : "no-e6-samples(locator-not-locked)");
            emit(true, t);
            auto_det_.forget(); ++auto_stale_forgets_;
        }
    }
    // 3. reconcile the drawn model with the detector's choice
    const int want = auto_det_.current();
    if (want == auto_installed_) return;
    if (want < 0 || ((auto_failed_mask_ >> unsigned(want)) & 1u)) {
        if (auto_installed_ >= 0) {   // the offset matches no type any more (confirmed windows), or the new type's model is missing: the game's cockpit
            std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=unload;type=%s;reason=%s;fallback=replayed-cockpit", type_name(auto_installed_),
                want < 0 ? "no-matching-type" : "new-type-model-unavailable");
            emit(true, t);
            s_.cockpit.reset(); auto_installed_ = -1; ++auto_unloads_;
        }
        return;
    }
    if (auto_loading_ >= 0) return;   // one load at a time; a changed wish is served when it finishes
    auto_loading_ = want; ++auto_loads_;
    std::snprintf(t, sizeof t, "xr-stereo;event=cockpit-auto;action=load;type=%s;model=%s;current-drawn=%s;thread=%s", type_name(want), A::kAircraft[want].model_id,
        type_name(auto_installed_), ca.async ? "worker" : "inline");
    emit(true, t);
    auto job = [cfg = ca.cfg, dir = ca.model_dir, load = ca.load, type = unsigned(want)]() -> AutoLoad {
        AutoLoad r;
        const auto t0 = std::chrono::steady_clock::now();
        try { r.rt = load ? load(type, r.why) : load_auto_model(cfg, type, dir, r.why); } catch (...) { r.rt.reset(); r.why = "loader-exception"; }
        if (!r.rt && r.why.empty()) r.why = "loader-returned-null";
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };
    if (ca.async) {
        try { auto_future_ = std::async(std::launch::async, std::move(job)); return; }
        catch (...) { AutoLoad r; r.why = "worker-thread-unavailable"; auto_loading_ = -1; cockpit_auto_install(std::move(r), want); return; }
    }
    AutoLoad r = job();
    auto_loading_ = -1;
    cockpit_auto_install(std::move(r), want);
}

bool Output::setup(Gate& g) {
    if (setup_attempted_) { fail("duplicate setup"); return false; }
    setup_attempted_ = true;
    if (!xr_ || !blit_) { fail("missing backend"); return false; }
    Rc rc = g.call(Op::swapchain_query, [&] { return xr_->view_configs(view_cfg_); });
    if (!succeeded(rc)) { fail("view configuration query failed"); return false; }
    for (const auto& v : view_cfg_) {
        if (!v.recommended_w || !v.recommended_h || v.recommended_w > s_.max_extent || v.recommended_h > s_.max_extent ||
            (v.max_w && v.recommended_w > v.max_w) || (v.max_h && v.recommended_h > v.max_h)) {
            fail("recommended view size refused"); return false;
        }
    }
    // Source-aspect extents (the recommended view size is the HMD's FOV, not the lens).
    for (unsigned t = 0; t < kTargets; ++t) {
        const uint32_t mw = t < 2 ? (view_cfg_[t].max_w ? view_cfg_[t].max_w : s_.max_extent) :
            std::min(view_cfg_[0].max_w ? view_cfg_[0].max_w : s_.max_extent, view_cfg_[1].max_w ? view_cfg_[1].max_w : s_.max_extent);
        const uint32_t mh = t < 2 ? (view_cfg_[t].max_h ? view_cfg_[t].max_h : s_.max_extent) :
            std::min(view_cfg_[0].max_h ? view_cfg_[0].max_h : s_.max_extent, view_cfg_[1].max_h ? view_cfg_[1].max_h : s_.max_extent);
        ext_[t] = swapchain_extent(s_.assumed_src_w, s_.assumed_src_h, std::min(mw, s_.max_extent), std::min(mh, s_.max_extent), s_.cap_width);
        if (!ext_[t].w || !ext_[t].h) { fail("swapchain extent refused"); return false; }
    }
    // EYE-RES (eye_res.h): the eyes only, from the runtime's recommended view size; a refused size keeps the source-aspect extent above.
    std::array<Extent, 2> eye_old{ext_[0], ext_[1]};
    const char* eye_clamp[2] = {"off", "off"};
    eye_res_used_ = false;
    if (s_.eye_res_pct) {
        for (unsigned t = 0; t < 2; ++t) {
            const uint32_t mw = std::min(view_cfg_[t].max_w ? view_cfg_[t].max_w : s_.max_extent, s_.max_extent);
            const uint32_t mh = std::min(view_cfg_[t].max_h ? view_cfg_[t].max_h : s_.max_extent, s_.max_extent);
            const acz::eyeres::Size e = acz::eyeres::recommended_extent(view_cfg_[t].recommended_w, view_cfg_[t].recommended_h, mw, mh, s_.eye_res_pct);
            eye_clamp[t] = e.w && e.h ? e.clamp : "refused(old-policy)";
            if (e.w && e.h) { ext_[t] = Extent{e.w, e.h}; eye_res_used_ = true; }
        }
    }
    std::array<int64_t, max_formats> formats{};
    uint32_t count = 0;
    rc = g.call(Op::swapchain_query, [&] { return xr_->formats(formats, count); });
    if (!succeeded(rc) || count > max_formats) { fail("swapchain format enumeration failed/over capacity"); return false; }
    format_ = select_format(formats.data(), count);
    bool f28 = false, f29 = false;
    for (uint32_t i = 0; i < count; ++i) { f28 |= formats[i] == rgba8_unorm; f29 |= formats[i] == rgba8_srgb; }
    emit(true, "xr-stereo;event=formats;count=" + u64(count) + ";offered-28=" + (f28 ? "1" : "0") + ";offered-29=" + (f29 ? "1" : "0") +
        ";selected=" + std::to_string(format_) + ";rtv=29;policy=xr_probe-format_policy");
    if (!format_) { fail("no RGBA8 swapchain format 28/29"); return false; }
    blit_setup_ = true; // attempted: partial private objects are retired by retire()
    rc = g.call(Op::d3d_setup, [&] { return blit_->setup() ? Rc::ok : Rc::error; });
    if (!succeeded(rc)) { fail("D3D12 blit setup (fence/root/PSO/heaps/list) failed"); return false; }
    // Targets created: both eyes, the world quad unless disabled, and the HUD quad ONLY
    // with the xr_hud_layer key (same extent policy as the world quad).
    unsigned order[kTargets] = {0, 1, 0, 0, 0}, targets = 2;
    if (s_.world_quad && !s_.world_in_projection) order[targets++] = kQuad; // J-world-in-projection: no quad swapchain
    if (s_.hud_layer) order[targets++] = kHud;
    if (s_.hud_markers_layer) order[targets++] = kMarkers; // J-hud-markers (last: key off creates exactly the same swapchains)
    for (unsigned k = 0; k < targets; ++k) {
        const unsigned t = order[k];
        SwapchainImages imgs{};
        const Extent e = ext_[t];
        rc = g.call(Op::create_swapchain, [&] { return xr_->create_swapchain(t, format_, e.w, e.h, imgs); });
        if (!succeeded(rc)) { fail("xrCreateSwapchain failed"); return false; }
        chains_[t] = true; // owned even on a qualified success
        images_[t] = imgs;
        if (!imgs.count || imgs.count > max_images) { fail("swapchain image count refused"); return false; }
        for (uint32_t i = 0; i < imgs.count; ++i) if (!imgs.images[i]) { fail("null swapchain image"); return false; }
        rc = g.call(Op::d3d_bind, [&] { return blit_->bind(t, imgs, e.w, e.h, format_) ? Rc::ok : Rc::error; });
        if (!succeeded(rc)) { fail("swapchain image RTV binding refused"); return false; }
        if (g.lost()) { fail("session loss during setup"); return false; }
    }
    char text[768];
    char policy[64];   // EYE-RES: the policy token names the eye policy actually used (eyeres off: exactly the previous text)
    if (eye_res_used_) std::snprintf(policy, sizeof policy, "recommended-x%u%%(eyes),source-aspect(quads)", s_.eye_res_pct);
    else std::snprintf(policy, sizeof policy, "source-aspect-width-min(src,%u,max)", s_.cap_width);
    std::snprintf(text, sizeof text, "xr-stereo;event=swapchains;format=%lld;eye0=%ux%u,images=%u;eye1=%ux%u,images=%u;quad=%s%ux%u,images=%u;"
        "samples=1;policy=%s;recommended=%ux%u-%s;mapping=full-rect;alpha=depth-coverage-premultiplied;"
        "encode=sRGB-RTV-once;counts=submitted-means-accepted-by-xrEndFrame;cap=%u;runtime-limit=%ux%u,%ux%u;source=%ux%u;"
        "recommended-eye1=%ux%u", (long long)format_, ext_[0].w, ext_[0].h, images_[0].count, ext_[1].w, ext_[1].h, images_[1].count,
        chains_[kQuad] ? "" : "none,", ext_[kQuad].w, ext_[kQuad].h, images_[kQuad].count, policy,
        view_cfg_[0].recommended_w, view_cfg_[0].recommended_h, eye_res_used_ ? "used" : "unused", s_.cap_width,
        view_cfg_[0].max_w, view_cfg_[0].max_h, view_cfg_[1].max_w, view_cfg_[1].max_h, s_.assumed_src_w, s_.assumed_src_h,
        view_cfg_[1].recommended_w, view_cfg_[1].recommended_h); // J-cockpit-fov-consumer: additive trailing token
    emit(true, text);
    if (s_.eye_res_pct) {   // EYE-RES: one lifecycle line (eyeres off: no line)
        const auto nb = acz::eyeres::eye_bytes(ext_[0].w, ext_[0].h, ext_[1].w, ext_[1].h, images_[0].count);
        const auto ob = acz::eyeres::eye_bytes(eye_old[0].w, eye_old[0].h, eye_old[1].w, eye_old[1].h, images_[0].count);
        std::snprintf(text, sizeof text, "xr-stereo;event=eye-res;pct=%u;used=%u;eye0=%ux%u;eye1=%ux%u;clamp=%s,%s;old-policy=%ux%u,%ux%u;"
            "mpix-per-eye=%.2f(old-%.2f);budget-mpix=%.2f;est-mib=swapchains:%llu(old-%llu),props:%llu(old-%llu),model-depth:%llu(old-%llu);"
            "quads=source-aspect(unchanged);needs=layer-fov-hmd;world-filter=footprint-grid(max-%ux%u,bit21)", s_.eye_res_pct, eye_res_used_ ? 1u : 0u, ext_[0].w, ext_[0].h, ext_[1].w, ext_[1].h,
            eye_clamp[0], eye_clamp[1], eye_old[0].w, eye_old[0].h, eye_old[1].w, eye_old[1].h,
            acz::eyeres::mpix(ext_[0].w, ext_[0].h), acz::eyeres::mpix(eye_old[0].w, eye_old[0].h), double(acz::eyeres::kPixelBudget) / 1.0e6,
            (unsigned long long)(nb.swapchains >> 20), (unsigned long long)(ob.swapchains >> 20), (unsigned long long)(nb.props >> 20),
            (unsigned long long)(ob.props >> 20), (unsigned long long)(nb.depth >> 20), (unsigned long long)(ob.depth >> 20),
            acz::eyeres::kFilterMaxGrid, acz::eyeres::kFilterMaxGrid);
        emit(true, text);
        // EYE-RING (eye_ring.h, section 5.1 (a)): the producer's per-eye targets follow the eye swapchains (the larger of each axis; read once when the
        // producer creates its ring). Only when the recommended size was used; a refused size keeps the producer at the game size.
        if (eye_res_used_) acz::eyering::publish_extent(std::max(ext_[0].w, ext_[1].w), std::max(ext_[0].h, ext_[1].h));
    }
    if (s_.world_in_projection)
        emit(true, "xr-stereo;event=world-composite;key=xr_world_in_projection;world-composite=projection;quad-layer=none;quad-swapchain=none;"
            "projection-flags=none(opaque);order=projection" + std::string(s_.hud_layer ? "-then-hud" : "") + ";plane-distance-m=" +
            std::to_string(double(s_.quad_distance_m)) + ";edge-feather=" + std::to_string(double(s_.edge_feather)) +
            ";shader=ps_main-mode2;constants=" + u64(s_.world_hold_last || s_.world_extend ? kBlitConstants : kBlitConstantsLegacy) +
            ";hypothesis=steamvr-draws-quad-layers-above-projection");
    if (s_.effects_layer)
        emit(true, "xr-stereo;event=effects-config;key=xr_effects_layer;requires=producer-gpu_copy_step5_effects;composite=inside-world-in-projection-pass;"
            "shader=ps_main-mode3(t3);order=world-then-effects-then-cockpit;mapping=same-world-plane-as-the-world(window-rays-only);alpha=premultiplied,max(a,max(rgb));"
            "effects-repr=legacy|coverage_v1(frame-tagged,mode4:P+(1-A)*world);unknown-repr=invalid,mode2;fallback=mode2;no-headset-claim");
    if (s_.debug_view || s_.png_once)
        emit(true, "xr-stereo;event=diagnostics-config;key=xr_debug_view|xr_png_once;debug-view=" + u64(s_.debug_view) + ";png-once=" + u64(s_.png_once ? 1 : 0) +
            ";png-after-new-frames=" + u64(s_.png_after_new_frames) + ";png-images=final,alpha(view1),world-with-effects(view3);png-source=eye0-private-target(sRGB);"
            "debug-views=1:coverage-grey,2:cockpit-colour,3:world-layer,4:false-colour(r=covered,g=world,b=uncovered-colour);no-headset-claim");
    if (s_.world_hold_last)
        emit(true, "xr-stereo;event=world-hold;key=xr_world_hold_last;hold-ms=" + std::to_string((long long)kWorldHoldMs) +
            ";fade=full-first-half-then-linear-to-black;image=proxy-owned-copy-of-the-last-good-world;applies=frames-without-a-world;constants=" + u64(kBlitConstants));
    if (s_.world_window_occlude)
        emit(true, "xr-stereo;event=window-occlude;key=xr_world_window_occlude;requires=xr_world_in_projection+xr_world_objects_depth_fix+depth;mode-bit=16;"
            "object-band=0<depth<" + std::to_string(double(kOccludeObjectDepth)) + ";inside-window=hidden(snapshot-already-occluded);periphery=kept;seam-ramp=" +
            std::to_string(double(kOccludeSeam)) + ";cockpit=kept;constants=unchanged;no-headset-claim");
    if (s_.world_extend)
        emit(true, "xr-stereo;event=world-extend;key=xr_world_extend;window=unchanged-except-no-edge-feather;periphery=clamped-angular-sample;"
            "min-light=" + std::to_string(double(kExtendMinLight)) + ";falloff-rad=" + std::to_string(double(kExtendFalloffRad)) +
            ";blur-mix=" + std::to_string(double(kExtendBlurMix)) + ";blur-rad=" + std::to_string(double(kExtendBlurRad)) + ";constants=" + u64(kBlitConstants));
    if (s_.world_cache)
        emit(true, "xr-stereo;event=world-cache;key=xr_world_cache;requires=xr_world_in_projection;cache=" + u64(kWorldCacheWidth) + "x" + u64(kWorldCacheHeight) +
            ";format=R11G11B10_FLOAT+R32_FLOAT-stamp;bytes=" + u64(uint64_t(kWorldCacheWidth) * kWorldCacheHeight * 8u) + ";frame=cockpit(look-yaw,look-pitch;no-aircraft-attitude);"
            "window=live-unchanged;periphery=cache-age-faded-to-sky-ground-fill;full-s=" + std::to_string(double(kCacheFullS)) + ";zero-s=" + std::to_string(double(kCacheZeroS)) +
            ";constants=" + u64(kBlitConstantsAll) + ";replaces=xr_world_extend;no-headset-claim");
    if (s_.terrain) {
        const auto& cv = s_.terrain->conv;
        char tt[700];
        std::snprintf(tt, sizeof tt, "xr-stereo;event=terrain;stage=%s;tiles=%u;objects=%u;sea=%s;yaw-sign=%.0f;yaw-offset-deg=%.3f;mirror=%u;pitch-sign=%.0f;roll-sign=%.0f;alt-offset=%.2f;fixed-y=%s;"
            "head-units-per-metre=%.2f(%s);haze=exp(-t/%.0f),far=%.0f;colour=display-domain-decoded-once;blend-window=%u;fixed-cam=%u;cam-source=%s;e6-sample=%s;game-max-deg=%.1f;occlude-objects=%u",
            s_.terrain->stage.id, s_.terrain->stage.ntiles, s_.terrain->stage.nobj, cv.sea_off ? "off" : (s_.terrain->stage.sea_present() ? (s_.terrain->stage.sea_masked ? "plane-sea-tiles" : "plane") : "none"),
            double(cv.yaw_sign), double(cv.yaw_offset_deg), cv.mirror_x ? 1u : 0u, double(cv.pitch_sign), double(cv.roll_sign), double(cv.alt_offset),
            cv.fixed_alt ? "yes" : "no", double(cv.head_units_per_metre), cv.upm_set ? "option" : "world-evidenced-5u/m*wscale", double(cv.haze_l), double(cv.far_dist),
            cv.blend_window ? 1u : 0u, cv.fixed_cam ? 1u : 0u, cv.cam_source_game ? "game-camera(e6-fallback)" : "e6", cv.e6_frame ? "frame" : "latest", double(cv.game_cam_max_deg),
            (cv.occlude_objects && s_.objects_depth_fix) ? 1u : 0u);
        emit(true, tt);
        if (cv.fixed_cam || cv.obj_display) {   // PROPS-SHADING (no line with the defaults: the existing log stays as it was)
            char ft[300];
            std::snprintf(ft, sizeof ft, "xr-stereo;event=terrain-debug;fixed-cam=%u;cam=%.1f,%.1f,%.1f;yaw-deg=%.2f;pitch-deg=%.2f;roll-deg=%.2f;objdom=%s%s;no-headset-claim",
                cv.fixed_cam ? 1u : 0u, double(cv.cam[0]), double(cv.cam[1]), double(cv.cam[2]), double(cv.cam[3]), double(cv.cam[4]), double(cv.cam[5]),
                cv.obj_display ? "display" : "raw", (cv.obj_display && !s_.objects_depth_fix) ? "(inactive:needs-xr_world_objects_depth_fix)" : "");
            emit(true, ft);
        }
        if (const acz::props::Runtime* pr = s_.terrain->props.get()) {   // WORLD-3
            char pt[400];
            std::snprintf(pt, sizeof pt, "xr-stereo;event=props;stage=%s;props=%u;trees=%u;instances=%zu;models=%zu;textures=%u;tree-scale=%.2f;tree-range=%.0f;prop-range=%.0f;cap=%u;"
                "shade=%s;depth=terrain-ray-march-bounded-by-prop-distance;provisional=1;no-headset-claim",
                pr->set.id, pr->opt.props ? 1u : 0u, pr->opt.trees ? 1u : 0u, pr->set.inst.size(), pr->set.models.size(), pr->set.ntex, double(pr->opt.tree_scale),
                double(pr->opt.tree_range), double(pr->opt.prop_range), pr->opt.cap,
                pr->opt.shade_game ? "game(texture*vertex-colour,linear-fog)" : "w3(stage-light+look-gain)");
            emit(true, pt);
        }
        emit(true, "xr-stereo;event=terrain-basis;keys=xr_terrain_render+xr_terrain_stage;requires=xr_world_in_projection+E6-camera(ee_camera_probe);convention=right-handed(+X-east,-Z-north,Y-up);"
            "heading=E6-rot[1];forward=(-sin(h),0,-cos(h));provisional=1;unverified=live-registration;no-headset-claim");
    }
    if (s_.cockpit) cockpit_config_lines();   // COCKPIT-2 (AIRCRAFT-AUTO: emitted again for every model it installs)
    if (s_.cockpit_auto) {   // AIRCRAFT-AUTO
        const auto& ca = *s_.cockpit_auto;
        char at[480];
        std::snprintf(at, sizeof at, "xr-stereo;event=cockpit-auto;state=armed;table-types=%u;window=%u;match-tol=%.3f;spread-tol=%.2f;confirm=%u;prefer=%s;"
            "requires=E6-locator-lock(xr_terrain_render+e6loc:auto|content);look=centred-samples-only;tie-rule=prefer-else-first-in-table;"
            "until-detected=%s;missing-model=replayed-cockpit;load=%s;samples=every-E6-sample;default=%s;remembered=%s",
            acz::aircraft::kAircraftCount, ca.det.window, double(ca.det.match_tol), double(ca.det.spread_tol), ca.det.confirm,
            ca.cfg.prefer_text.empty() ? "none" : ca.cfg.prefer_text.c_str(), auto_det_.assumed() ? "assumed-model" : "replayed-cockpit",
            ca.async ? "worker-thread" : "inline", ca.cfg.default_type >= 0 ? acz::aircraft::kAircraft[ca.cfg.default_type].model_id : "none",
            type_name(ca.remembered));
        emit(true, at);
        if (!auto_assume_line_.empty()) emit(true, auto_assume_line_);
    }
    if (s_.hud_layer) {
        // Separate line: the swapchains line above stays byte-identical to the key-off build.
        std::snprintf(text, sizeof text, "xr-stereo;event=hud-swapchain;key=xr_hud_layer;hud=%ux%u,images=%u;format=%lld;policy=same-as-world-quad;"
            "hud-mode=%s;flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);order=quad-then-projection-then-hud;alpha=max(a,max(rgb))",
            ext_[kHud].w, ext_[kHud].h, images_[kHud].count, (long long)format_, hud_mode_name(s_.hud_mode));
        emit(true, text);
    }
    if (s_.hud_markers_layer) {
        std::snprintf(text, sizeof text, "xr-stereo;event=markers-swapchain;key=xr_hud_markers_layer;markers=%ux%u,images=%u;format=%lld;policy=same-as-world-quad;"
            "flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);order=quad-then-markers-then-projection-then-hud;placement=world-quad-pose-and-size;scale=1;"
            "distance-m=%.1f;alpha=max(a,max(rgb))", ext_[kMarkers].w, ext_[kMarkers].h, images_[kMarkers].count, (long long)format_, double(s_.quad_distance_m));
        emit(true, text);
    }
    return !failed() && !g.lost();
}

bool Output::validate(const stereo::StereoFrame& f, const char*& why) const {
    why = nullptr;
    if (f.frame_id == 0 || f.frame_id <= last_id_) why = "frame_id not newer than the last consumed frame";
    else if (f.slot >= stereo::kSlots) why = "slot out of range";
    else if (!f.eye[0] || !f.eye[1] || f.eye[0] == f.eye[1]) why = "eye images missing/aliased";
    else if (!source_format_ok(f.dxgi_format)) why = "source format not FP16/FP32 RGBA";
    else if (!f.width || !f.height || f.width > s_.max_extent || f.height > s_.max_extent) why = "source size refused";
    else if ((f.eye_width || f.eye_height) && (!f.eye_width || !f.eye_height || f.eye_width > s_.max_extent || f.eye_height > s_.max_extent))
        why = "eye image size refused";   // EYE-RING (eye_ring.h): only frames of the eye ring carry it
    else if (!f.fence || f.fence_value == 0) why = "producer fence missing";
    return why == nullptr;
}

void Output::release_slot() {
    if (!l_.holding) return;
    l_.holding = false;
    if (auto* src = provider_ ? provider_() : nullptr) { src->release(l_.frame); ++c_.released_frames; }
}
bool Output::release_images(Gate& g) {
    bool ok = true;
    for (unsigned t = 0; t < kTargets; ++t) {
        if (!l_.acquired[t] || !l_.waited[t] || l_.release_attempted[t]) continue;
        l_.release_attempted[t] = true; // an accepted failure is never retried
        const Rc rc = g.call(Op::release_image, [&] { return xr_->release(t); });
        if (succeeded(rc)) { l_.released[t] = true; l_.acquired[t] = false; }
        else ok = false;
    }
    return ok;
}
void Output::settle_without_gpu(Gate& g) {
    // Precondition: nothing was submitted, so no GPU work references any image.
    if (l_.submitted) return;
    if (!g.lost()) (void)release_images(g); // waited images only; unwaited acquisitions stay outstanding
    release_slot();
}
bool Output::observe_completion(Gate& g, int64_t deadline, int64_t spacing) {
    const int64_t started = g.now_ms();
    struct Measure { Gate& g; int64_t start; int64_t& maximum;
        ~Measure() { maximum = std::max(maximum, g.now_ms() - start); }
    } measure{g, started, blocked_max_};
    for (;;) {
        if (l_.complete) return true;
        if (g.now_ms() >= deadline) return false;
        const int64_t next = last_sample_ms_ + spacing;
        if (g.now_ms() < next) { g.sleep_ms(std::min(next, deadline) - g.now_ms()); continue; }
        uint64_t done = 0; bool valid = false;
        const Rc rc = g.call(Op::d3d_sample, [&] { valid = blit_->sample(done); return valid ? Rc::ok : Rc::error; });
        last_sample_ms_ = g.now_ms();
        ++sample_count_;
        if (rc == Rc::refused) return false; // deadline closed: no observation
        if (!succeeded(rc) || !valid || done == std::numeric_limits<uint64_t>::max() || done < last_completed_) {
            l_.ambiguous = true;
            fail("invalid/regressing/device-removed fence sample");
            return false;
        }
        last_completed_ = done;
        if (issued_ >= done) c_.fence_lag_max = std::max<uint64_t>(c_.fence_lag_max, issued_ - done);
        if (done >= l_.value) { l_.complete = true; return true; }
    }
}

void Output::anchor(const xr_owner::FrameInfo& info, const xr_owner::LayerPlan& plan) {
    // World-locked anchor: the eye midpoint of the first world (or world-quad HUD) frame
    // OF THIS LOCAL SPACE GENERATION; a space change re-captures it (never reused across
    // generations). The world quad and the world-quad HUD share it, so they coincide.
    // J-quad-yaw: a frame carrying the producer's neutral (StereoFrame::neutral_valid, the
    // seated-recentering p0/yaw0 of this frame's space generation) anchors at p0 turned by yaw0,
    // so the quads sit along the cockpit forward. An eye-midpoint anchor is replaced once by the
    // first neutral of the same generation; without a neutral everything is exactly as before.
    const auto& f = l_.frame;
    const bool neutral = neutral_ok(f);
    const bool regen = !neutral_set_ || neutral_gen_ != info.space_generation;
    if (regen || (neutral && !anchor_neutral_)) {
        if (regen) { if (neutral_set_) ++c_.anchor_rebases; }
        else ++c_.anchor_neutral_upgrades;
        if (neutral) {
            for (int i = 0; i < 3; ++i) neutral_[i] = f.neutral_pos[i];
            neutral_yaw_ = f.neutral_yaw;
            anchor_neutral_ = true;
            ++c_.anchor_neutral;
        } else {
            for (int i = 0; i < 3; ++i) neutral_[i] = 0.5f * (plan.layer.views[0].pose.position[i] + plan.layer.views[1].pose.position[i]);
            neutral_yaw_ = 0.0f;
            anchor_neutral_ = false;
        }
        neutral_set_ = true;
        neutral_gen_ = info.space_generation;
        if (!regen) emit(true, "xr-stereo;event=quad-anchor;for=upgrade" + quad_anchor_tokens());
    }
}
// EXTRAP (extrap:1; frame_pacing.h, docs/world-render/uncap.md). Once per consumed producer frame (new frames only; a re-submitted XR frame reuses
// the last plan unchanged): the chosen terrain camera (game camera or E6) feeds the dead-reckoning model in GAME FRAMES (the simulation advances a
// fixed step per frame, so velocity per frame x the measured game frame rate is the wall velocity even at 15 fps) and is moved forward by the
// select age (this XR frame's predicted display time - the frame's own predicted display time), clamped to extrapms. Direction changes, stops,
// cuts and the first frames are not extrapolated (no overshoot). Terrain, props, clouds and the TACTICAL overlay follow the moved camera; the
// replayed game images (objects, effects, HUD, replayed cockpit) cannot move and stay at their frame time (the shift is logged).
void Output::extrap_camera(const stereo::StereoFrame& f, int64_t now_ms, acz::terrain::CamFrame& cf) {
    const auto& po = s_.terrain->pacing;
    if (!extrap_.config_logged) {
        extrap_.config_logged = true;
        acz::pacing::ExtrapConfig c; c.max_ms = po.extrap_max_ms; c.attitude = po.extrap_attitude;
        extrap_.ex.configure(c);
        extrap_.last_summary_ms = now_ms;
        emit(true, "xr-stereo;event=extrap-config;" + acz::pacing::describe(po) +
                   ";model=dead-reckoning-per-game-frame(fixed-step-sim);horizon=select-age(xr-predicted-minus-frame-predicted);"
                   "moves=terrain,props,clouds,tactical-overlay;static=replayed-objects,effects,hud,replayed-cockpit");
    }
    extrap_.lead_s = 0;
    ++extrap_.frames;
    const int64_t t_us = f.diag_publish_us > 0 ? f.diag_publish_us
        : int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    acz::pacing::Frame in;
    for (int i = 0; i < 3; ++i) { in.eye[i] = cf.eye[i]; in.right[i] = cf.right[i]; in.up[i] = cf.up[i]; in.fwd[i] = cf.fwd[i]; }
    extrap_.ex.update(f.frame_id, t_us, in);
    if (!select_age_available_) { ++extrap_.no_horizon; extrap_.last_why = "no-select-age"; }
    else {
        acz::pacing::Frame out;
        const acz::pacing::ExtrapResult r = extrap_.ex.predict(select_age_ms_, out);
        extrap_.last_why = r.why;
        if (r.translated || r.rotated) {
            const float d[3] = {out.eye[0] - cf.eye[0], out.eye[1] - cf.eye[1], out.eye[2] - cf.eye[2]};
            const double shift = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
            extrap_.shift_sum += shift; extrap_.shift_max = std::max(extrap_.shift_max, shift);
            for (int i = 0; i < 3; ++i) { cf.eye[i] = out.eye[i]; cf.right[i] = out.right[i]; cf.up[i] = out.up[i]; cf.fwd[i] = out.fwd[i]; }
            extrap_.lead_s = r.horizon_ms * 1e-3;
            extrap_.horizon_sum_ms += r.horizon_ms;
            if (r.translated) ++extrap_.translated;
            if (r.rotated) ++extrap_.rotated;
            if (r.clamped) ++extrap_.clamped;
        } else ++extrap_.refused;
    }
    if (now_ms - extrap_.last_summary_ms >= 5000) {
        const uint64_t applied = extrap_.translated > extrap_.rotated ? extrap_.translated : extrap_.rotated;
        char t[460];
        std::snprintf(t, sizeof t, "xr-stereo;event=summary-extrap;frames=%llu;translated=%llu;rotated=%llu;clamped=%llu;refused=%llu;no-horizon=%llu;"
            "horizon-ms-mean=%.1f;shift-units-mean=%.2f;shift-units-max=%.2f;game-fps=%.1f;markers-led=%llu;last-why=%s",
            (unsigned long long)extrap_.frames, (unsigned long long)extrap_.translated, (unsigned long long)extrap_.rotated,
            (unsigned long long)extrap_.clamped, (unsigned long long)extrap_.refused, (unsigned long long)extrap_.no_horizon,
            applied ? extrap_.horizon_sum_ms / double(applied) : 0.0, applied ? extrap_.shift_sum / double(applied) : 0.0, extrap_.shift_max,
            extrap_.ex.fps(), (unsigned long long)extrap_.markers_led, extrap_.last_why);
        emit(true, t);
        extrap_.frames = 0; extrap_.translated = extrap_.rotated = extrap_.clamped = extrap_.refused = extrap_.no_horizon = extrap_.markers_led = 0;
        extrap_.horizon_sum_ms = extrap_.shift_sum = extrap_.shift_max = 0;
        extrap_.last_summary_ms = now_ms;
    }
}
// OBJECT-INTERP (interp:1; object_interp.h, docs/world-render/interp.md). Once per consumed producer frame: the producer drew this frame's replayed
// world objects at t_display (k game frames ahead) as the camera moved by (dt, dR) sees them; the terrain camera takes exactly that delta, so terrain,
// props, clouds, tactical markers (display-time positions from the producer) and objects share one camera. A frame without the plan (world objects
// off, no world camera) returns false: the caller may then run extrap:1 as before.
bool Output::interp_camera(const stereo::StereoFrame& f, int64_t now_ms, acz::terrain::CamFrame& cf, bool fixed_alt) {
    if (!interp_.config_logged) {
        interp_.config_logged = true;
        interp_.last_summary_ms = now_ms;
        emit(true, "xr-stereo;event=interp-config;" + acz::pacing::describe(s_.terrain->pacing) +
                   ";model=per-object-track(mesh-sig+ia-buffers+ordinal+gate)+camera-dead-reckoning(fixed-step-sim);t-display=select-age-ema;"
                   "objects=producer-model-matrix(B)-at-t-display;camera=producer-delta-applied-here;game-rate=hud,effects,game-marker-sprites,replayed-cockpit;"
                   "rate=one-update-per-game-frame(xr-reuse-frames-unchanged)");
    }
    ++interp_.frames;
    bool applied = false;
    if (!f.interp.valid) ++interp_.no_facts;
    else {
        const acz::interp::Facts& fi = f.interp;
        ++interp_.planned;
        interp_.last_why = fi.why ? fi.why : "none";
        if (fi.k > 0) { ++interp_.k_pos; interp_.k_sum += fi.k; interp_.horizon_sum += fi.horizon_ms; }
        if (fi.cam_moved) {
            float dt[3] = {fi.dt[0], fi.dt[1], fi.dt[2]};
            if (fixed_alt) dt[1] = 0.0f;   // the fixed-altitude test convention pins the eye height
            acz::interp::apply_to_camera(dt, fi.dR, cf.eye, cf.right, cf.up, cf.fwd);
            const double shift = std::sqrt(double(dt[0]) * dt[0] + double(dt[1]) * dt[1] + double(dt[2]) * dt[2]);
            interp_.shift_sum += shift; interp_.shift_max = std::max(interp_.shift_max, shift);
            ++interp_.cam_moved;
        }
        interp_.objects += fi.objects; interp_.moved += fi.moved; interp_.appeared += fi.appeared; interp_.teleports += fi.teleports;
        interp_.scale += fi.scale_changes; interp_.held += fi.held; interp_.lost += fi.lost; interp_.write_fail += fi.write_fail;
        applied = true;
        // interp-trace (live plan, interp.md section 6): per frame the publication time, k, the camera delta and up to 4 objects (id, kind, moved, game
        // position at frame time, drawn display-time position), 120 consecutive frames every 20 s (~6 lines/s on average)
        if (now_ms >= interp_.next_trace_ms) { interp_.next_trace_ms = now_ms + 20000; interp_.trace_left = 120; }
        if (interp_.trace_left) {
            --interp_.trace_left;
            char t[900];
            int n = std::snprintf(t, sizeof t, "xr-stereo;event=interp-trace;frame_id=%llu;publish-us=%lld;k=%.4f;horizon-ms=%.2f;why=%s;cam-dt=%.1f,%.1f,%.1f;objects=%u",
                (unsigned long long)f.frame_id, (long long)f.diag_publish_us, double(fi.k), double(fi.horizon_ms), fi.why ? fi.why : "none",
                double(fi.dt[0]), double(fi.dt[1]), double(fi.dt[2]), fi.objects);
            for (uint32_t i = 0; i < fi.nsample && i < acz::interp::Facts::kSamples && n > 0 && size_t(n) < sizeof t; ++i) {
                const auto& sm = fi.sample[i];
                n += std::snprintf(t + n, sizeof t - size_t(n), ";o%u=%u:%u:%u:%.1f,%.1f,%.1f:%.1f,%.1f,%.1f", i, sm.id, unsigned(sm.kind), unsigned(sm.moved),
                    double(sm.obs[0]), double(sm.obs[1]), double(sm.obs[2]), double(sm.disp[0]), double(sm.disp[1]), double(sm.disp[2]));
            }
            emit(false, t);
        }
    }
    if (now_ms - interp_.last_summary_ms >= 5000) {
        char t[560];
        std::snprintf(t, sizeof t, "xr-stereo;event=summary-interp;frames=%llu;planned=%llu;no-plan=%llu;k-positive=%llu;k-mean=%.3f;horizon-ms-mean=%.1f;"
            "horizon-ema-ms=%.1f;cam-moved=%llu;cam-shift-mean=%.2f;cam-shift-max=%.2f;objects=%llu;moved=%llu;held=%llu;appeared=%llu;teleports=%llu;"
            "scale-changes=%llu;lost=%llu;write-fail=%llu;last-why=%s",
            (unsigned long long)interp_.frames, (unsigned long long)interp_.planned, (unsigned long long)interp_.no_facts, (unsigned long long)interp_.k_pos,
            interp_.k_pos ? interp_.k_sum / double(interp_.k_pos) : 0.0, interp_.k_pos ? interp_.horizon_sum / double(interp_.k_pos) : 0.0, interp_.ema.v,
            (unsigned long long)interp_.cam_moved, interp_.cam_moved ? interp_.shift_sum / double(interp_.cam_moved) : 0.0, interp_.shift_max,
            (unsigned long long)interp_.objects, (unsigned long long)interp_.moved, (unsigned long long)interp_.held, (unsigned long long)interp_.appeared,
            (unsigned long long)interp_.teleports, (unsigned long long)interp_.scale, (unsigned long long)interp_.lost, (unsigned long long)interp_.write_fail,
            interp_.last_why);
        emit(true, t);
        const acz::interp::HorizonEma keep = interp_.ema; const bool logged = interp_.config_logged;
        interp_ = InterpState{}; interp_.ema = keep; interp_.config_logged = logged; interp_.last_summary_ms = now_ms;
    }
    return applied;
}
// TACTICAL (tac:1; tactical.h, docs/world-render/tactical.md). Once per consumed producer frame: tracker, firing debounce, cosmetic tracers, radar
// blips and world-anchored markers -> tac_.overlay (drawn by the backend). Never fails the frame: anything missing simply draws less (counted).
// One EyeView per eye for the tactical / NBOX overlays: clip of the eye's world composite plane + the eye's whole frame. The ONLY constructor of the
// tac EyeViews (fresh frame and DECOUPLE re-composite alike), so their markers and stamps match at the same pose.
static void tac_eye_views(const std::array<WorldComposite, 2>& wc, const std::array<TerrainEye, 2>& te, acz::tac::EyeView ev[2]) {
    for (unsigned e = 0; e < 2; ++e) {
        ev[e] = acz::tac::EyeView{};
        ev[e].ok = acz::props::make_clip(wc[e].base, wc[e].ux, wc[e].vy, te[e].right, te[e].up, te[e].fwd, ev[e].clip);
        acz::tac::set_eye_frame(ev[e], te[e].eye, te[e].right, te[e].up, te[e].fwd);
    }
}
const acz::tac::Overlay* Output::tac_frame(const stereo::StereoFrame& f, bool terrain_ok, const std::array<TerrainEye, 2>& te,
                                           const std::array<WorldComposite, 2>& wc, const acz::terrain::CamFrame& cam, bool hud, bool rebuild_only,
                                           bool markers_live) {
    namespace tac = acz::tac;
    const tac::Options& o = s_.terrain->tac;
    // NBOX (mk360:3): a re-composite never copies or clears again (the fresh frame's recording did); a fresh frame starts from an empty plan (no stale
    // copies are ever attributed to it, whatever early return follows).
    if (rebuild_only) { tac_.stamps.fresh = false; tac_.stamps.copy.clear(); tac_.stamps.clear.clear(); }
    else { tac_.stamps.fresh = false; tac_.stamps.draw.clear(); tac_.stamps.copy.clear(); tac_.stamps.clear.clear(); }
    if (rebuild_only) {   // DECOUPLE: a re-composite of the same game frame: the eye overlay again for THIS XR frame's eyes (no tracker/firing update, no HUD slot)
        tac_.overlay.clear();
        if (!f.tac || !f.tac->valid || !terrain_ok) return nullptr;
        const double t = f.diag_publish_us > 0 ? double(f.diag_publish_us) * 1e-6 : tac_.prev_t;
        tac::EyeView ev[2];
        tac_eye_views(wc, te, ev);
        for (unsigned e = 0; e < 2; ++e) {
            const tac::Slot pre = e ? tac::slot_eye1_pre : tac::slot_eye0_pre;
            tac::begin_slot(tac_.overlay, pre);
            if (o.on && o.tracers) (void)tac::build_tracers(tac_.overlay, ev[e], tac_.tracers, t);
            tac::end_slot(tac_.overlay, pre);
        }
        for (unsigned e = 0; e < 2; ++e) {
            const tac::Slot post = e ? tac::slot_eye1_post : tac::slot_eye0_post;
            tac::begin_slot(tac_.overlay, post);
            for (const tac::WorldMarker& m : tac_.markers) (void)tac::build_marker(tac_.overlay, ev[e], m);
            tac::end_slot(tac_.overlay, post);
        }
        tac::begin_slot(tac_.overlay, tac::slot_hud);
        tac::end_slot(tac_.overlay, tac::slot_hud);
        // NBOX (mk360:3): the same stamps for THIS XR frame's eyes, from the atlas (the copies/clears belonged to the fresh frame's recording)
        if (o.mk360 == 3) acz::mk360::set_view_centre(tac_.stamps, ev);   // BOX-LOCK B3: the near policy pushes out from these eyes
        for (unsigned e = 0; e < 2; ++e) {
            const tac::Slot ss = e ? tac::slot_eye1_stamp : tac::slot_eye0_stamp;
            tac::begin_slot(tac_.overlay, ss);
            if (o.mk360 == 3) (void)acz::mk360::build_stamps(tac_.overlay, ev[e], tac_.stamps, e == 0 ? &tac_.mk.st : nullptr);   // NEAR-FADE counted once (eye 0)
            tac::end_slot(tac_.overlay, ss);
        }
        (void)cam;
        return tac_.overlay.empty() ? nullptr : &tac_.overlay;
    }
    if (!tac_.config_logged) {
        tac_.config_logged = true;
        emit(true, "xr-stereo;event=tac-config;" + tac::describe(o) +
                   ";radar-geom=centre(0.1396,0.7925),box-half(0.0797w,0.1461h),rings(0.0216,0.0430,0.0858,0.1289)h(measured-step62/71);"
                   "team=marker-colour(blue=friend,else-enemy);tracers=cosmetic(no-hit-data);needs=gpu_copy_step5_world_objects+gpu_copy_step5_hud+gpu_copy_step5_hud_markers");
    }
    tac_.overlay.clear();
    if (!f.tac || !f.tac->valid) { ++tac_.no_frame; return nullptr; }
    if (!terrain_ok) { ++tac_.no_camera; return nullptr; }
    ++tac_.frames;
    const double t = f.diag_publish_us > 0 ? double(f.diag_publish_us) * 1e-6
                                           : double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) * 1e-6;
    const tac::TacFrame& tf = *f.tac;
    // the player: the game camera (cockpit eye) of this frame; its velocity from consecutive frames (smoothed)
    if (tac_.have_prev && t > tac_.prev_t + 1e-3 && t - tac_.prev_t < 0.5) {
        for (int i = 0; i < 3; ++i) {
            const float inst = float((cam.eye[i] - tac_.prev_pos[i]) / (t - tac_.prev_t));
            if (std::isfinite(inst)) tac_.vel[i] = 0.7f * tac_.vel[i] + 0.3f * inst;
        }
    } else if (tac_.have_prev && !(t > tac_.prev_t)) { /* same time: keep */ }
    else for (float& v : tac_.vel) v = 0.0f;
    tac_.have_prev = true; tac_.prev_t = t; for (int i = 0; i < 3; ++i) tac_.prev_pos[i] = cam.eye[i];
    if (o.on) tac::tracker_update(tac_.tracker, tf, cam.eye, t, o);   // MARKERS-360 alone (mk360 without tac:1): no tac tracker, tracers or blips
    const bool wedge = tf.wedge + tf.wedge_tail > 0;
    if (wedge) ++tac_.wedge_frames;
    const bool firing = tac_.fire.update(wedge, t);
    if (firing) ++tac_.firing_frames;
    if (s_.cockpit && s_.cockpit->opt.stwatch && wedge != stwatch_wedge_prev_) {   // STORES-RT search: a launch (missile or gun) shows the firing wedge (D-318)
        stwatch_wedge_prev_ = wedge;
        char sw[200];
        std::snprintf(sw, sizeof sw, "xr-stereo;event=store-watch;kind=fire-wedge;state=%s;t-s=%.3f;wedge-frames=%llu;note=compare-with-store-watch-decrement-lines(e6-block-counters)",
            wedge ? "on" : "off", t, (unsigned long long)tac_.wedge_frames);
        emit(true, sw);
    }
    if (o.on && o.tracers) {
        float gl[3], gr[3];
        tac::gun_positions(cam.eye, cam.right, cam.up, cam.fwd, gl, gr);
        tac_.tracers.update(firing, t, gl, gr, cam.fwd, tac_.vel);
    }
    // eyes: tracers before the model cockpit, markers after it
    // eyes: the SAME EyeViews as a re-composite at this pose (whole eye frame; Codex J-203 finding 1: the fresh path once left right/fwd zero and
    // marker_screen_frame then measured the stamp width along another axis -> a width jump between fresh and re-composite at an unchanged pose)
    tac::EyeView ev[2];
    tac_eye_views(wc, te, ev);
    unsigned tracers = 0, markers = 0;
    for (unsigned e = 0; e < 2; ++e) {
        const tac::Slot pre = e ? tac::slot_eye1_pre : tac::slot_eye0_pre;
        tac::begin_slot(tac_.overlay, pre);
        if (o.on && o.tracers) tracers += tac::build_tracers(tac_.overlay, ev[e], tac_.tracers, t);
        tac::end_slot(tac_.overlay, pre);
    }
    if (o.mk360) {   // MARKERS-360 (marker360.h): one box per contact the game reports now, whole sphere; replaces tacboxes
        const acz::mk360::Params mp = acz::mk360::params_from(o);
        acz::mk360::update(tac_.mk, tf, cam.eye, t, mp);
        acz::mk360::FrameCounts fc;
        acz::mk360::collect(tac_.mk, tf, cam.eye, mp, tac_.markers, fc);   // mode 3: empty (no synthetic boxes)
        if (o.mk360 == 3) {   // NBOX: the game's own box + label pixels as stamps (markers-360.md section 12)
            acz::mk360::StampCounts sc;
            acz::mk360::plan_stamps(tac_.mk, tf, mp, f.width, f.height, markers_live, t, tac_.stamps, sc);
        }
    } else if (o.markers) {
        uint32_t skipped = 0;
        tac::collect_markers(tac_.tracker, t, tac_.markers, skipped);   // the game's own marker drawn this frame: not ours too (no doubles)
        tac_.markers_skipped_game += skipped;
        if (extrap_.lead_s > 0) {   // EXTRAP (extrap:1): each marker leads by its contact's own velocity, like the camera (clamped to 400 units)
            for (tac::WorldMarker& m : tac_.markers)
                for (const tac::Track& tr : tac_.tracker.tracks)
                    if (tr.id == m.id) { if (tr.vel_valid) { acz::pacing::lead_point(m.pos, tr.vel, extrap_.lead_s, 400.0f); ++extrap_.markers_led; } break; }
        }
    } else tac_.markers.clear();
    for (unsigned e = 0; e < 2; ++e) {
        const tac::Slot post = e ? tac::slot_eye1_post : tac::slot_eye0_post;
        tac::begin_slot(tac_.overlay, post);
        for (const tac::WorldMarker& m : tac_.markers) if (tac::build_marker(tac_.overlay, ev[e], m) && e == 0) ++markers;
        tac::end_slot(tac_.overlay, post);
    }
    if (o.mk360 == 3) acz::mk360::set_view_centre(tac_.stamps, ev);   // BOX-LOCK B3: the near policy pushes out from these eyes
    for (unsigned e = 0; e < 2; ++e) {   // NBOX (mk360:3): the stamps per eye (empty slots otherwise)
        const tac::Slot ss = e ? tac::slot_eye1_stamp : tac::slot_eye0_stamp;
        tac::begin_slot(tac_.overlay, ss);
        if (o.mk360 == 3) { const unsigned n = acz::mk360::build_stamps(tac_.overlay, ev[e], tac_.stamps, e == 0 ? &tac_.mk.st : nullptr); if (e == 0) markers += n; }
        tac::end_slot(tac_.overlay, ss);
    }
    // HUD: radar blips in the game's radar widget (the HUD image is the game's screen, same size as the frame)
    tac::begin_slot(tac_.overlay, tac::slot_hud);
    tac_.blips.clear();
    if (o.on && o.radar && hud && f.width && f.height) {
        float fh[3], rh[3];
        if (tac::heading_frame(cam.fwd, cam.up, fh, rh)) {
            const float aspect = float(f.height) / float(f.width);
            tac::radar_blips(tac_.tracker, cam.eye, fh, rh, t, o, tac::RadarGeom{}, aspect, tac_.blips);
            tac::build_radar(tac_.overlay, tac_.blips, tac::RadarGeom{}, aspect);
        }
    } else if (o.on && o.radar && !hud) ++tac_.hud_absent;
    tac::end_slot(tac_.overlay, tac::slot_hud);
    tac_.blips_sum += tac_.blips.size(); tac_.markers_sum += markers; tac_.tracer_sum += tracers; tac_.overlay_sum += tac_.overlay.v.size();
    if (tac_.overlay.v.size() >= tac::kOverlayCap - 64) ++tac_.overlay_full;
    return tac_.overlay.empty() ? nullptr : &tac_.overlay;
}
void Output::summary_tac(int64_t now) {
    if (s_.terrain->tac.mk360) {   // MARKERS-360: cumulative counters + the current contacts (marker360.h; docs/world-render/markers-360.md section 6)
        emit(false, acz::mk360::summary_line(tac_.mk, s_.terrain->tac, now));
        emit(false, acz::mk360::match_line(tac_.mk, now));   // MATCH-2 (markers-360.md section 9)
        emit(false, acz::mk360::sample_line(tac_.mk, now));
        if (s_.terrain->tac.mk360 == 3) {   // NBOX (markers-360.md section 12): the consumer plan + what the backend recorded
            emit(false, acz::mk360::nbox_line(tac_.mk, tac_.stamps, now));
            emit(false, acz::mk360::hs6_line(tac_.mk, now));     // headset test 6 (markers-360.md section 17)
            emit(false, acz::mk360::iff_line(tac_.mk, acz::mk360::params_from(s_.terrain->tac), now));   // provisional-box team sources (section 20)
            emit(false, acz::mk360::stamp_line(tac_.mk, now));   // section 17.1: one live stamp's box / label / pixel rectangles + the vertices' q
            char nb[360];
            std::snprintf(nb, sizeof nb, "xr-stereo;event=summary-mk360-nbox-backend;t-ms=%lld;used=%llu;refused=%llu;copies=%llu;clears=%llu;"
                          "refused-view=%llu;refused-atlas=%llu;refused-copy=%llu;refused-pipeline=%llu;needs=xr_hud_markers_layer",
                          (long long)now, (unsigned long long)tac_.nb_backend_used, (unsigned long long)tac_.nb_backend_refused,
                          (unsigned long long)tac_.nb_copies_recorded, (unsigned long long)tac_.nb_clears_recorded,
                          (unsigned long long)tac_.nb_refuse[1], (unsigned long long)tac_.nb_refuse[2], (unsigned long long)tac_.nb_refuse[3],
                          (unsigned long long)tac_.nb_refuse[4]);
            emit(false, nb);
        }
        if (!s_.terrain->tac.on) return;
    }
    char text[900];
    const uint64_t n = tac_.frames ? tac_.frames : 1;
    unsigned teamed = 0, friends = 0, enemies = 0, moving = 0, coasting = 0;
    for (const auto& tr : tac_.tracker.tracks) {
        if (tr.team != acz::tac::team_unknown) ++teamed;
        if (tr.team == acz::tac::team_friend) ++friends;
        if (tr.team == acz::tac::team_enemy) ++enemies;
        if (tr.vel_valid && tr.speed() >= acz::tac::kMovingSpeed) ++moving;
        if (!tr.seen_now) ++coasting;
    }
    std::snprintf(text, sizeof text, "xr-stereo;event=summary-tac;t-ms=%lld;frames=%llu;no-producer-facts=%llu;no-camera=%llu;tracks=%zu(teamed:%u,friend:%u,enemy:%u,moving:%u,"
        "coasting:%u);created=%llu;dropped=%llu;self-draws=%llu;marker-matches=%llu;avg-blips=%.2f;avg-markers=%.2f;game-marker-skips=%llu;wedge-frames=%llu;"
        "firing-frames=%llu;fire-events=%llu;tracers-spawned=%llu;avg-tracer-quads=%.2f;avg-overlay-vertices=%.1f;overlay-full=%llu;backend=used:%llu,refused:%llu;"
        "hud-absent=%llu;cosmetic-tracers=1",
        (long long)now, (unsigned long long)tac_.frames, (unsigned long long)tac_.no_frame, (unsigned long long)tac_.no_camera, tac_.tracker.tracks.size(), teamed,
        friends, enemies, moving, coasting, (unsigned long long)tac_.tracker.created, (unsigned long long)tac_.tracker.dropped,
        (unsigned long long)tac_.tracker.self_draws, (unsigned long long)tac_.tracker.matched, double(tac_.blips_sum) / double(n), double(tac_.markers_sum) / double(n),
        (unsigned long long)tac_.markers_skipped_game, (unsigned long long)tac_.wedge_frames, (unsigned long long)tac_.firing_frames,
        (unsigned long long)tac_.fire.on_events, (unsigned long long)tac_.tracers.spawned, double(tac_.tracer_sum) / double(n), double(tac_.overlay_sum) / double(n),
        (unsigned long long)tac_.overlay_full, (unsigned long long)tac_.backend_used, (unsigned long long)tac_.backend_refused, (unsigned long long)tac_.hud_absent);
    emit(false, text);
    // Range calibration sample (the radar scale is a guess): up to 6 current blips with range/bearing and our radar pixel, to compare with a simultaneous
    // game-window shot of the game's own radar.
    std::string line = "xr-stereo;event=tac-blips;t-ms=" + std::to_string(now) + ";range-ring3=" + std::to_string(int(s_.terrain->tac.range_ring3)) + ";blips=";
    unsigned k = 0;
    for (const auto& b : tac_.blips) {
        if (k++ >= 6) break;
        char one[160];
        std::snprintf(one, sizeof one, "%s%u:r%.0f:b%.1f:uv%.4f,%.4f:%s%s:rgb%.2f,%.2f,%.2f", k > 1 ? "," : "", b.id, double(b.range), double(b.bearing * 57.29578f),
                      double(b.u), double(b.v), b.arrow ? "arrow" : "square", b.edge ? ":edge" : "", double(b.rgb[0]), double(b.rgb[1]), double(b.rgb[2]));
        line += one;
    }
    if (!k) line += "none";
    emit(false, line);
}
// ---------------------------------------------- CAM-EPOCH (terrain_camera.h; docs/world-render/e6-anchor-free.md section 10) ---
// The E6 sample of this frame: its own (per-frame) sample when fresh relative to the frame's publication, of the current lock/stage epoch, finite/sane and
// stamped with the frame's recording generation; else the Present-history publication nearest to the frame's recording start within cam_window_ms;
// else none (the frame takes the no-camera path for E6). Reused/re-submitted images are never re-composed, so they keep the camera they were made with.
// TAKEOFF (docs/world-render/takeoff-stage.md): one line per switch between the mission stage and the airbase stage (the backend re-uploads on the
// generation change; props restart their async init for the other runtime).
void Output::takeoff_sample(const float eye[3]) {
    const auto ev = takeoff_.update(eye, s_.terrain->stage, s_.terrain->takeoff->stage);
    if (ev == acz::terrain::TakeoffSelector::Event::none) return;
    char text[400];
    std::snprintf(text, sizeof text, "xr-stereo;event=terrain-takeoff;drawn=%s;switch=%s;reason=%s;eye=%.1f,%.1f,%.1f;base-ground=%.1f;mission-ground=%.1f;"
        "samples=%llu;enters=%llu;leaves=%llu;cuts=%llu", drawn_terrain().stage.id, ev == acz::terrain::TakeoffSelector::Event::enter ? "to-takeoff-base" : "to-mission",
        takeoff_.reason(), double(eye[0]), double(eye[1]), double(eye[2]), double(takeoff_.base_ground()), double(takeoff_.mission_ground()),
        (unsigned long long)takeoff_.samples, (unsigned long long)takeoff_.enters, (unsigned long long)takeoff_.leaves, (unsigned long long)takeoff_.jumps);
    emit(true, text);
}
bool Output::admit_camera(const stereo::StereoFrame& f, acz::terrain::CamSample& cs, acz::terrain::CamSource& src) {
    using namespace acz::terrain;
    const auto& cv = s_.terrain->conv;
    const CamMailbox& mb = s_.terrain->present ? *s_.terrain->present : cam_mailbox();
    const CamEpochs& ep = s_.terrain->epochs ? *s_.terrain->epochs : cam_epochs();
    AdmitLimits lim;
    lim.max_age_ns = int64_t(double(cv.cam_age_ms) * 1e6);
    lim.fallback_window_ns = int64_t(double(cv.cam_window_ms) * 1e6);
    FrameCam fc;
    fc.valid = f.e6_valid;
    for (int i = 0; i < 3; ++i) fc.s.campos[i] = f.e6_campos[i];
    fc.s.pitch = f.e6_pitch; fc.s.yaw = f.e6_yaw; fc.s.roll = f.e6_roll; fc.s.heading = f.e6_heading; fc.s.seq = f.e6_seq;
    fc.s.t_ns = f.e6_t_ns; fc.s.lock_epoch = f.e6_lock_epoch; fc.s.stage_epoch = f.e6_stage_epoch; fc.s.tag = f.e6_tag;
    fc.s.t_read_ns = f.e6_t_read_ns;   // review 2 (Astra P2): the read start travels with an admitted own-frame sample (camlag 'f' line)
    fc.expected_tag = f.diag_generation;
    fc.start_ns = f.diag_start_us > 0 ? f.diag_start_us * 1000 : 0;
    fc.publish_ns = f.diag_publish_us > 0 ? f.diag_publish_us * 1000 : 0;
    const int64_t now_ns = mono_now_us() * 1000;
    // review 2: the epochs of this admission, read once; DECOUPLE retains them with a held frame and checks them before every re-composite
    frame_ep_lock_ = ep.lock(); frame_ep_stage_ = ep.stage(); frame_ep_valid_ = true;
    cam_pick_ = pick_camera(fc, cv.e6_frame, mb, frame_ep_lock_, frame_ep_stage_, now_ns, lim);
    src = cam_pick_.src;
    if (!cam_pick_.ok) return false;
    cs = cam_pick_.s;
    const int64_t ref = src == CamSource::latest ? now_ns : src == CamSource::frame ? (fc.publish_ns ? fc.publish_ns : now_ns) : (fc.start_ns ? fc.start_ns : now_ns);
    const double age = double(ref - cs.t_ns) / 1e6;
    if (age > cam_.age_max_ms) cam_.age_max_ms = age;
    return true;
}
// Counts this frame's outcome and logs one camera-admit line per outcome change (at most kCamLineCap per session; later changes are counted).
void Output::camera_outcome(const stereo::StereoFrame& f, acz::terrain::CamMode mode) {
    using namespace acz::terrain;
    const CamPick& p = cam_pick_;
    if (p.ok) ++cam_.src[unsigned(p.src)];
    else ++cam_.refused[unsigned(p.why)];
    if (f.e6_valid && s_.terrain->conv.e6_frame && p.frame_why != Admit::ok && p.frame_why != Admit::none) ++cam_.frame_refused[unsigned(p.frame_why)];
    ++cam_.mode[unsigned(mode)];
    if (mode == CamMode::game_only) ++cam_.game_only;
    const int key = int(mode) * 64 + (p.ok ? 32 + int(p.src) : int(p.why));
    if (key == cam_last_key_) return;
    char from[64];
    if (cam_last_key_ < 0) std::snprintf(from, sizeof from, "start");
    else {
        const int m = cam_last_key_ / 64, r = cam_last_key_ % 64;
        std::snprintf(from, sizeof from, "%s/%s", cam_mode_name(CamMode(m)), r >= 32 ? cam_source_name(CamSource(r - 32)) : admit_name(Admit(r)));
    }
    cam_last_key_ = key;
    ++cam_.transitions;
    constexpr unsigned kCamLineCap = 200;
    if (cam_lines_ >= kCamLineCap) { ++cam_lines_suppressed_; return; }
    ++cam_lines_;
    const CamEpochs& ep = s_.terrain->epochs ? *s_.terrain->epochs : cam_epochs();
    char text[440];
    std::snprintf(text, sizeof text, "xr-stereo;event=camera-admit;from=%s;to=%s/%s;mode=%s;frame-id=%llu;frame-sample=%s;sample-lock-epoch=%u;sample-stage-epoch=%u;lock-epoch=%u;"
        "stage-epoch=%u;sample-age-ms=%.1f;action=%s;object-registration=%s;lines-suppressed=%llu",
        from, cam_mode_name(mode), p.ok ? cam_source_name(p.src) : admit_name(p.why), cam_mode_name(mode), (unsigned long long)f.frame_id,
        !f.e6_valid ? "absent" : admit_name(p.frame_why), p.ok ? p.s.lock_epoch : f.e6_lock_epoch, p.ok ? p.s.stage_epoch : f.e6_stage_epoch, ep.lock(), ep.stage(),
        f.e6_t_ns && f.diag_publish_us > 0 ? double(f.diag_publish_us * 1000 - f.e6_t_ns) / 1e6 : -1.0,
        mode == CamMode::none ? "no-camera(terrain-off-this-frame)" : mode == CamMode::game_only ? "game-camera-only(frame-correlated,no-e6-cross-check)"
            : mode == CamMode::correlated_hybrid ? "draw-camera+same-frame-e6" : "e6-only(uncorrelated-display-fallback)",
        cam_mode_registers_objects(mode) ? "allowed" : "refused", (unsigned long long)cam_lines_suppressed_);
    emit(true, text);
}
// CAM-EPOCH lag evidence (docs/world-render/e6-anchor-free.md section 10.4; analysed OFFLINE by tools/live/camera-lag-analyze.py). In bursts of kCamlagBurst
// composed frames every kCamlagPeriodMs (session cap kCamlagLineCap lines) one 'f' line per composed frame with the inputs it ACTUALLY used, one 'p' line
// per Present-tick publication (history entries not logged before), one 'r' line per re-submitted (reused) image with its original snapshot, and an 's'
// line closing the burst. Columns: the 'event=camlag-columns' line. No decision reads any of this.
namespace {
constexpr unsigned kCamlagBurst = 300;
constexpr int64_t kCamlagPeriodMs = 60000;
constexpr unsigned kCamlagLineCap = 6000;
}
bool Output::camlag_active() {
    if (!s_.terrain || !s_.terrain->conv.cam_log || !s_.terrain->conv.cam_admit || s_.terrain->conv.fixed_cam) return false;
    if (camlag_lines_ >= kCamlagLineCap) return false;
    const int64_t now_ms = mono_now_us() / 1000;
    if (camlag_in_burst_) return true;
    if (camlag_burst_start_ms_ >= 0 && now_ms - camlag_burst_start_ms_ < kCamlagPeriodMs) return false;
    camlag_in_burst_ = true; camlag_burst_start_ms_ = now_ms; camlag_burst_frames_ = 0; ++camlag_bursts_;
    camlag_b_ = CamlagBurst{};
    if (!camlag_header_) {
        camlag_header_ = true;
        emit(false, "xr-stereo;event=camlag-columns;v=1;f=fid,gen,slot,members,mode,src,why,frame_why,e6seq,e6_read_us,e6_pub_us,e6_lock,e6_stage,cur_lock,cur_stage,"
                    "start_us,publish_us,game,game_eye,gu0,gu1,gu2,gf0,gf1,gf2,ge0,ge1,ge2,e6,eu0,eu1,eu2,ef0,ef1,ef2,ee0,ee1,ee2,lens_l,lens_r,lens_u,lens_d");
        emit(false, "xr-stereo;event=camlag-columns;v=1;p=seq,usable,read_us,pub_us,lock,stage,eu0,eu1,eu2,ef0,ef1,ef2,ee0,ee1,ee2;r=fid,e6seq,mode,age_ms;"
                    "right=fwd_x_up;clock=steady_us;unlogged=submission-occurrence,reference-draw-id");
        camlag_lines_ += 2;
    }
    return true;
}
// LENS-FLARE diagnostics (2026-10-30): the CPU mirror of the shader's early-outs per eye (lens_flare.h make_frame/classify) so a log proves whether the flare was
// switched on, had a sun, and had the sun in view. Pre-occlusion: the GPU terrain/cockpit occlusion is not mirrored. One config line per stage generation.
void Output::lensflare_diag(const std::vector<float>& env, uint64_t gen, uint32_t env_bits, const std::array<TerrainEye, 2>& te, const std::array<WorldComposite, 2>& cp) {
    namespace LF = acz::lensflare;
    namespace T = acz::terrain;
    const bool have_env = env.size() >= size_t(T::kPFlare + 1) * 4 && (env_bits & T::kEnvSky) != 0;
    const float zero4[4] = {0, 0, 0, 0};
    const float* p32 = have_env ? &env[size_t(T::kPFlare) * 4] : zero4;
    const float* light = have_env ? &env[size_t(T::kPLight) * 4] : zero4;
    const float* moon = have_env ? &env[size_t(T::kPMoon) * 4] : zero4;
    LF::Why best = LF::Why::active;
    bool any_active = false;
    float cs_best = -1.0f, gate = 0.0f;
    for (unsigned e = 0; e < 2; ++e) {
        LF::Why w = LF::Why::no_env;
        if (have_env) {
            const LF::Frame fr = LF::make_frame(cp[e].base, cp[e].ux, cp[e].vy, te[e].right, te[e].up, te[e].fwd, light);
            w = LF::classify(p32, light, moon, fr);
            cs_best = std::fmax(cs_best, fr.cs);
            gate = std::fmax(gate, LF::peak_gate(fr));
        }
        if (w == LF::Why::active) any_active = true;
        else if (unsigned(w) > unsigned(best) || best == LF::Why::active) best = w;
    }
    if (any_active) best = LF::Why::active;
    ++c_.lf_frames; ++c_.lf_why[unsigned(best)];
    lf_peak_ = std::fmax(lf_peak_, gate); lf_cos_best_ = std::fmax(lf_cos_best_, cs_best);
    lf_sin_el_ = have_env ? light[1] : 0.0f;
    if (lf_cfg_gen_ != gen) {
        lf_cfg_gen_ = gen;
        char t[420];
        const bool is_moon = have_env && moon[3] > 0.0f && moon[0] * light[0] + moon[1] * light[1] + moon[2] * light[2] > LF::kSunMoonSameCos;
        std::snprintf(t, sizeof t, "xr-stereo;event=lensflare-config;gen=%llu;env=%d;sky-bit=%d;switch=%d;k=%.2f;light=%d;light-is-moon=%d;sun-elevation-deg=%.1f;"
            "camera-basis=terrain-camera(game,else-e6);needs=sun-in-view-of-an-eye(no-e6-lock-needed-when-the-game-camera-is-used)",
            (unsigned long long)gen, env.size() >= size_t(T::kPFlare + 1) * 4 ? 1 : 0, (env_bits & T::kEnvSky) ? 1 : 0, (p32[3] > 0.0f && p32[0] > 0.0f) ? 1 : 0, double(p32[0]),
            light[3] > 0.0f ? 1 : 0, is_moon ? 1 : 0, std::asin(std::fmax(-1.0f, std::fmin(1.0f, light[1]))) * 57.29578);
        emit(false, t);
    }
}

void Output::camlag_frame(const stereo::StereoFrame& f, bool e6ok, const acz::terrain::CamFrame& ef) {
    using namespace acz::terrain;
    if (!camlag_active()) return;
    const auto& cv = s_.terrain->conv;
    const CamEpochs& ep = s_.terrain->epochs ? *s_.terrain->epochs : cam_epochs();
    const CamMailbox& mb = s_.terrain->present ? *s_.terrain->present : cam_mailbox();
    char t[480];
    auto us = [](int64_t ns) { return (long long)(ns / 1000); };
    // Present publications not logged yet (oldest first); a gap between the last logged one and the oldest retained one = history overwritten
    CamSample h[CamMailbox::kHist]; bool u[CamMailbox::kHist];
    const unsigned n = mb.history(h, u, CamMailbox::kHist);
    if (n && camlag_last_p_ns_ && h[0].t_ns > camlag_last_p_ns_ && camlag_last_p_seq_ && h[0].seq > camlag_last_p_seq_ + 1) ++camlag_b_.present_gaps;
    for (unsigned i = 0; i < n && camlag_lines_ < kCamlagLineCap; ++i) {
        if (h[i].t_ns <= camlag_last_p_ns_) continue;
        camlag_last_p_ns_ = h[i].t_ns; if (u[i]) camlag_last_p_seq_ = h[i].seq;
        CamFrame hf{}; const bool hok = u[i] && camera_frame(h[i].campos, h[i].pitch, h[i].yaw, h[i].roll, cv, hf);
        std::snprintf(t, sizeof t, "xr-stereo;event=camlag;p=%llu,%d,%lld,%lld,%u,%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f", (unsigned long long)h[i].seq, hok ? 1 : 0,
            us(h[i].t_read_ns), us(h[i].t_ns), h[i].lock_epoch, h[i].stage_epoch, hf.up[0], hf.up[1], hf.up[2], hf.fwd[0], hf.fwd[1], hf.fwd[2], hf.eye[0], hf.eye[1], hf.eye[2]);
        emit(false, t); ++camlag_lines_; ++camlag_b_.present;
    }
    if (camlag_lines_ >= kCamlagLineCap) { ++camlag_b_.dropped; return; }
    CamFrame gf{}; bool game_ok = false;
    if (f.game_cam_valid) {
        const float zero[3] = {0, 0, 0};
        game_ok = camera_frame_axes(f.game_eye_valid ? f.game_eye : zero, f.game_cam_right, f.game_cam_up, f.game_cam_fwd, cv, gf);
    }
    const CamPick& p = cam_pick_;
    // the 'f' line's E6 columns = the sample actually used (any source; 'src' says which: frame = seam B, present-history / present-latest = Present tick)
    std::snprintf(t, sizeof t, "xr-stereo;event=camlag;f=%llu,%llu,%u,%u,%s,%s,%s,%s,%llu,%lld,%lld,%u,%u,%u,%u,%lld,%lld,%d,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,"
        "%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f",
        (unsigned long long)f.frame_id, (unsigned long long)f.diag_generation, f.slot, f.members, cam_mode_name(cam_mode_), cam_source_name(p.src), admit_name(p.why),
        f.e6_valid ? admit_name(p.frame_why) : "absent", (unsigned long long)(p.ok ? p.s.seq : f.e6_seq), us(p.ok ? p.s.t_read_ns : f.e6_t_read_ns), us(p.ok ? p.s.t_ns : f.e6_t_ns),
        p.ok ? p.s.lock_epoch : f.e6_lock_epoch, p.ok ? p.s.stage_epoch : f.e6_stage_epoch, ep.lock(), ep.stage(), (long long)f.diag_start_us, (long long)f.diag_publish_us,
        game_ok ? 1 : 0, game_ok && f.game_eye_valid ? 1 : 0, gf.up[0], gf.up[1], gf.up[2], gf.fwd[0], gf.fwd[1], gf.fwd[2], gf.eye[0], gf.eye[1], gf.eye[2],
        e6ok ? 1 : 0, ef.up[0], ef.up[1], ef.up[2], ef.fwd[0], ef.fwd[1], ef.fwd[2], ef.eye[0], ef.eye[1], ef.eye[2],
        f.lens_tan[0], f.lens_tan[1], f.lens_tan[2], f.lens_tan[3]);
    emit(false, t); ++camlag_lines_;
    ++camlag_b_.frames; ++camlag_b_.mode[unsigned(cam_mode_)];
    if (!p.ok) ++camlag_b_.refused;
    if (!f.e6_valid) ++camlag_b_.missing;
    if (++camlag_burst_frames_ >= kCamlagBurst) camlag_close();
}
void Output::camlag_reuse(int64_t age_ms) {
    if (!camlag_in_burst_ || camlag_lines_ >= kCamlagLineCap) return;
    char t[200];
    std::snprintf(t, sizeof t, "xr-stereo;event=camlag;r=%llu,%llu,%s,%lld", (unsigned long long)last_cam_fid_, (unsigned long long)last_cam_seq_,
        acz::terrain::cam_mode_name(last_cam_mode_), (long long)age_ms);
    emit(false, t); ++camlag_lines_; ++camlag_b_.reused;
}
void Output::camlag_close() {
    if (!camlag_in_burst_) return;
    camlag_in_burst_ = false;
    using acz::terrain::CamMode;
    char t[440];
    std::snprintf(t, sizeof t, "xr-stereo;event=camlag;s=burst:%u,frames:%u,present:%u,reused:%u,refused:%u,frame-sample-missing:%u,present-gaps:%u,dropped-by-cap:%u,"
        "hybrid:%u,game-only:%u,e6-frame:%u,e6-game-refused:%u,e6-history:%u,e6-latest:%u,none:%u,lines:%u/%u",
        camlag_bursts_, camlag_b_.frames, camlag_b_.present, camlag_b_.reused, camlag_b_.refused, camlag_b_.missing, camlag_b_.present_gaps, camlag_b_.dropped,
        camlag_b_.mode[unsigned(CamMode::correlated_hybrid)], camlag_b_.mode[unsigned(CamMode::game_only)], camlag_b_.mode[unsigned(CamMode::e6_frame)],
        camlag_b_.mode[unsigned(CamMode::e6_game_refused)], camlag_b_.mode[unsigned(CamMode::e6_fallback)], camlag_b_.mode[unsigned(CamMode::e6_latest)],
        camlag_b_.mode[unsigned(CamMode::none)], camlag_lines_ + 1, kCamlagLineCap);
    emit(false, t); ++camlag_lines_;
}
// The lag diagnostic: this frame's game camera (cockpit axes + DrawACM_v eye) vs the E6 sample it was composed with, the E6 samples of the neighbouring
// consumed frames (CamLagMeter) and the Present-history publications at fixed offsets from the recording start. Decisions never read it.
void Output::camera_lag_frame(const stereo::StereoFrame& f, bool e6ok, const acz::terrain::CamFrame& ef, double age_ms) {
    using namespace acz::terrain;
    const auto& cv = s_.terrain->conv;
    CamBasis gb{}, eb{};
    bool game_ok = false;
    if (f.game_cam_valid) {
        const float zero[3] = {0, 0, 0};
        CamFrame gf{};
        game_ok = camera_frame_axes(f.game_eye_valid ? f.game_eye : (e6ok ? ef.eye : zero), f.game_cam_right, f.game_cam_up, f.game_cam_fwd, cv, gf);
        if (game_ok) for (int i = 0; i < 3; ++i) { gb.right[i] = gf.right[i]; gb.up[i] = gf.up[i]; gb.fwd[i] = gf.fwd[i]; gb.eye[i] = gf.eye[i]; }
    }
    if (e6ok) for (int i = 0; i < 3; ++i) { eb.right[i] = ef.right[i]; eb.up[i] = ef.up[i]; eb.fwd[i] = ef.fwd[i]; eb.eye[i] = ef.eye[i]; }
    // the per-frame offsets compare seam-B samples only (a history/latest sample is not "the E6 of that frame")
    lag_.frame(f.frame_id, game_ok, gb, game_ok && f.game_eye_valid, e6ok && cam_pick_.src == CamSource::frame, eb, age_ms);
    if (!game_ok || f.diag_start_us <= 0) return;
    const CamMailbox& mb = s_.terrain->present ? *s_.terrain->present : cam_mailbox();
    float ang[CamLagMeter::kPresent];
    for (int i = 0; i < CamLagMeter::kPresent; ++i) {
        ang[i] = std::nanf("");
        CamSample h; CamFrame hf{};
        if (mb.nearest(f.diag_start_us * 1000 + CamLagMeter::present_offset_ns(i), CamLagMeter::kPresentTolNs, h) == CamMailbox::Find::ok &&
            camera_frame(h.campos, h.pitch, h.yaw, h.roll, cv, hf)) {
            CamBasis hb{}; for (int k = 0; k < 3; ++k) { hb.right[k] = hf.right[k]; hb.up[k] = hf.up[k]; hb.fwd[k] = hf.fwd[k]; hb.eye[k] = hf.eye[k]; }
            ang[i] = basis_angle_deg(gb, hb);
        }
    }
    lag_.present(ang);
}

bool Output::lean_anchor(const stereo::StereoFrame& f, float out[3]) const {
    if (!s_.lean.active() || !anchor_neutral_ || !neutral_ok(f) || !f.pose_valid) return false;
    double sh[3];
    if (!pov::frame_layer_shift(s_.lean, f.pose[0].position, f.pose[1].position, f.pose[0].orientation, f.neutral_pos, f.neutral_yaw, sh)) return false;
    for (int i = 0; i < 3; ++i) out[i] = float(double(neutral_[i]) + sh[i]);
    return true;
}
std::string Output::quad_anchor_tokens() const {
    char text[200];
    if (anchor_neutral_)
        std::snprintf(text, sizeof text, ";quad-anchor=neutral;neutral-yaw-deg=%.2f;neutral-pos=%.4f,%.4f,%.4f",
            double(neutral_yaw_) * 57.29577951308232, double(neutral_[0]), double(neutral_[1]), double(neutral_[2]));
    else
        std::snprintf(text, sizeof text, ";quad-anchor=eye-midpoint;neutral-yaw-deg=none;neutral-pos=none;anchor-pos=%.4f,%.4f,%.4f",
            double(neutral_[0]), double(neutral_[1]), double(neutral_[2]));
    return text;
}

bool Output::composite_views(const xr_owner::FrameInfo& info, std::array<xr_owner::EyeSample, 2>& v) const {
    // The views layer_from() will submit for the frame in l_.frame (same selection, same FOV rule), without
    // counters or state, so the eye images can be composited with exactly the poses/FOV the layer carries.
    if (dc_.views_on) { v = dc_.views; return true; }   // DECOUPLE: this XR frame's own poses (P1) with the images' FOV (key off: never set)
    const auto& f = l_.frame;
    if (f.pose_valid && eye_ok(f.pose[0]) && eye_ok(f.pose[1])) { v[0] = sample_of(f.pose[0]); v[1] = sample_of(f.pose[1]); }
    else if (info.views_valid) v = info.views.eyes;
    else return false;
    if (f.cockpit_fov_valid) {
        for (unsigned e = 0; e < 2; ++e) v[e].fov = {f.cockpit_fov[e][0], f.cockpit_fov[e][1], f.cockpit_fov[e][2], f.cockpit_fov[e][3]};
    } else if (f.lens_valid && lens_ok(f.lens_tan)) {
        v[0].fov = v[1].fov = fov_from_lens(f.lens_tan);
    }
    return true;
}

bool Output::layer_from(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan, bool depth, bool world, bool hud, bool composite, bool markers,
                        bool fresh) {
    plan = {};
    const auto& f = l_.frame;
    if (dc_.views_on) {
        // DECOUPLE: the images were composed with this XR frame's own poses (P1); the FOV block below sets the render FOV exactly as composite_views().
        plan.layer.views = dc_.views;
        ++dcs_.views_xr;
    } else if (f.pose_valid && eye_ok(f.pose[0]) && eye_ok(f.pose[1])) {
        // Head-locked rendering: submit exactly the poses the replay used.
        plan.layer.views[0] = sample_of(f.pose[0]);
        plan.layer.views[1] = sample_of(f.pose[1]);
        ++c_.pose_from_frame;
    } else if (info.views_valid) {
        plan.layer.views = info.views.eyes; // raw located views of THIS frame
        ++c_.pose_from_located;
    } else {
        ++c_.zero_no_pose;
        have_last_ = false;
        return false;
    }
    // The cockpit image was rendered through the game's lens: its FOV, not the HMD's.
    // J-cockpit-fov-consumer: a frame rendered with each eye's own FOV (cockpit_fov_valid,
    // values validated in transact() before any GPU work) submits exactly that FOV per eye;
    // the world quad / HUD below stay lens-based fixed quads.
    lens_used_ = f.lens_valid && lens_ok(f.lens_tan);
    if (lens_used_) lens_fov_ = fov_from_lens(f.lens_tan);
    if (f.cockpit_fov_valid) {
        for (unsigned e = 0; e < 2; ++e)
            plan.layer.views[e].fov = {f.cockpit_fov[e][0], f.cockpit_fov[e][1], f.cockpit_fov[e][2], f.cockpit_fov[e][3]};
        cockpit_fov_source_ = 2;
        ++c_.cockpit_fov_frames;
    } else if (lens_used_) {
        plan.layer.views[0].fov = plan.layer.views[1].fov = lens_fov_;
        cockpit_fov_source_ = 1;
        ++c_.lens_frames;
    } else cockpit_fov_source_ = 0;
    // Premultiplied coverage alpha; J-feather: the edge ramp also needs blending without depth.
    plan.layer.source_alpha = depth || s_.edge_feather > 0.0f;
    if (world) {
        anchor(info, plan);
        // lens_ok checked before the blit; J-quad-yaw: turned by the producer's yaw0 only with a neutral anchor.
        float ly = 0.0f, lp = 0.0f;
        const bool lk = look_of(f, ly, lp); // xr_look_drive_keys: the world image is a view along the rendered look
        float leaned[3];
        const float* na = lean_anchor(f, leaned) ? leaned : neutral_; // F-LEAN (key off: neutral_ itself)
        const Quad q = lk ? world_quad(na, f.lens_tan, s_.quad_distance_m, (anchor_neutral_ ? neutral_yaw_ : 0.0f) + ly, lp)
                          : anchor_neutral_ ? world_quad(na, f.lens_tan, s_.quad_distance_m, neutral_yaw_)
                                            : world_quad(na, f.lens_tan, s_.quad_distance_m);
        plan.layer.quad = true;
        plan.layer.quad_pose = q.pose;
        plan.layer.quad_width = q.width;
        plan.layer.quad_height = q.height;
        plan.layer.quad_source_alpha = s_.edge_feather > 0.0f; // J-feather: OFF keeps the opaque quad
    }
    if (markers) {
        // J-hud-markers: EXACTLY the world quad (same anchor, neutral yaw, lens - lens_ok checked before the blit - and
        // distance) so the markers register with the world image and the targets in it; scale is never applied, and neither
        // xr_hud_quad_distance_m nor xr_hud_scale reaches this quad. Present even without a world quad this frame.
        anchor(info, plan);
        float ly = 0.0f, lp = 0.0f;
        const bool lk = look_of(f, ly, lp); // xr_look_drive_keys: the world image is a view along the rendered look
        float leaned[3];
        const float* na = lean_anchor(f, leaned) ? leaned : neutral_; // F-LEAN (key off: neutral_ itself)
        const Quad q = lk ? world_quad(na, f.lens_tan, s_.quad_distance_m, (anchor_neutral_ ? neutral_yaw_ : 0.0f) + ly, lp)
                          : anchor_neutral_ ? world_quad(na, f.lens_tan, s_.quad_distance_m, neutral_yaw_)
                                            : world_quad(na, f.lens_tan, s_.quad_distance_m);
        plan.layer.markers = true;
        plan.layer.marker_pose = q.pose;
        plan.layer.marker_width = q.width;
        plan.layer.marker_height = q.height;
    }
    if (hud) {
        // J-hud-consumer: submitted AFTER (on top of) the cockpit projection, premultiplied.
        plan.layer.hud = true;
        if (s_.hud_mode == HudMode::head_locked) {
            // FALLBACK: fixed in VIEW space, source aspect; HUD/world alignment does not hold.
            // J-hud-placement: distance/width/scale keys (defaults 2.5 m / 1.6 m / 1 as before).
            plan.layer.hud_view_space = true;
            const Quad q = hud_head_quad(s_.hud_head_distance_m, s_.hud_head_width_m, s_.hud_scale, f.width, f.height);
            plan.layer.hud_pose = q.pose;
            plan.layer.hud_width = q.width;
            plan.layer.hud_height = q.height;
        } else {
            // EXACTLY the world quad (same anchor, lens - lens_ok checked before the blit - and
            // distance) unless J-hud-placement sets a HUD distance (same directions/angular size)
            // or a scale (about the quad centre).
            anchor(info, plan);
            // xr_hud_follow (default 0 = exactly the neutral-anchored quad): 1 = fixed relative to the lean-compensated virtual eye,
            // 2 = additionally turned about the anchor by a rate-limited fraction of the rendered look.
            float hleaned[3];
            const bool follow = s_.hud_follow >= 1 && anchor_neutral_;
            const float* ha = (follow && lean_anchor(f, hleaned)) ? hleaned : neutral_;
            if (follow && s_.hud_follow >= 2) {
                float ly = 0.0f, lp = 0.0f;
                look_of(f, ly, lp);
                if (hud_follow_gen_ != neutral_gen_) { hud_follow_state_ = pov::HudFollow{}; hud_follow_gen_ = neutral_gen_; }
                pov::hud_follow_step(hud_follow_state_, double(ly), double(lp), g.now_ms());
            }
            float hl[4], hs = 1.0f, hd = 0.0f;   // HUD-MENU: menu mode / glass-pivot scale (unchanged lens/scale/distance otherwise)
            hud_quad_params(f.lens_tan, hl, hs, hd);
            const Quad q = (follow && s_.hud_follow >= 2)
                ? hud_world_quad(ha, hl, s_.quad_distance_m, hd, hs,
                                 neutral_yaw_ + float(hud_follow_state_.yaw), float(hud_follow_state_.pitch))
                : anchor_neutral_
                ? hud_world_quad(ha, hl, s_.quad_distance_m, hd, hs, neutral_yaw_)
                : hud_world_quad(neutral_, hl, s_.quad_distance_m, hd, hs);
            plan.layer.hud_pose = q.pose;
            plan.layer.hud_width = q.width;
            plan.layer.hud_height = q.height;
        }
    }
    if (composite) {
        // J-world-in-projection: the world is already inside the eye images (no quad layer, anchor captured
        // before the blit); the projection is submitted opaque - its premultiplied colour is the final picture
        // over black, so a SOURCE_ALPHA blend would only let the compositor background show through.
        std::array<xr_owner::EyeSample, 2> cv{};
        if (!composite_views(info, cv) || std::memcmp(cv.data(), plan.layer.views.data(), sizeof cv) != 0) {
            fail("world composite views differ from the submitted layer views");
            return false;
        }
        plan.layer.world_composite = true;
        plan.layer.source_alpha = false;
    }
    plan.submit = true;
    last_plan_ = plan;
    have_last_ = true;
    ever_new_ = true;
    last_gen_ = info.space_generation;
    if (fresh) {   // DECOUPLE: a re-composite is not a new game frame (new-per-s, png_after_new_frames, the reuse age all stay game-frame based)
        last_new_ms_ = g.now_ms();
        ++c_.new_frames;
        // POV-RENDER stage 2: only a frame WITH cockpit members is cockpit evidence for the view classifier (no-cockpit chase frames never are).
        if (acz::povr::cockpit_composite(fresh, l_.frame.no_cockpit)) { last_cockpit_ms_ = last_new_ms_; ever_cockpit_ = true; }
        else { ++pov_nc_frames_; pov_last_nc_ms_ = last_new_ms_; }   // POV-RENDER pov:3: the NcGate's evidence
    }
    return true;
}

void Output::pace(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
    // Re-submit the last released images with their OWN pose/FOV (valid without a
    // new acquire: xrEndFrame uses the most recently released image; the compositor
    // reprojects) only while young enough, tracked and in the same LOCAL space
    // generation; otherwise zero layers, counted by reason.
    plan = {};
    if (s_.vscreen && vscreen_pace(g, info, plan)) return;   // VSCREEN (xr_vscreen.h): the game's screen when no flight composite is young enough
    if (!have_last_) { if (ever_new_) ++c_.stale_zero; else ++c_.zero_no_frame; return; }
    const int64_t age = g.now_ms() - last_new_ms_;
    if (age > s_.reuse_max_age_ms) { ++c_.stale_age; ++c_.stale_zero; return; }
    if (!info.views_tracked) { ++c_.stale_tracking; ++c_.stale_zero; return; }
    if (info.space_generation != last_gen_) { ++c_.stale_space; ++c_.stale_zero; have_last_ = false; return; }
    ++c_.reused;
    c_.reuse_age_max_ms = std::max<uint64_t>(c_.reuse_age_max_ms, uint64_t(std::max<int64_t>(0, age)));
    camlag_reuse(age);   // CAM-EPOCH: the re-submitted image keeps its original snapshot (logged by identity during a camlag burst)
    plan = last_plan_;
    if (s_.select_corr_log) corr_emit(g, info, plan, false, 0); // timing-anchor diagnostics: the re-submitted layer keeps the last selected frame's facts
}

// ---------------------------------------------------------------- DECOUPLE (decouple:1; docs/world-render/decouple.md)
void Output::dc_release(const char* why) {
    if (!dc_.held) return;
    dc_.held = false;   // exactly once: the flag is cleared before the call
    if (auto* src = provider_ ? provider_() : nullptr) { src->release(dc_.frame); ++c_.released_frames; }
    ++dcs_.released;
    if (!std::strcmp(why, "superseded")) ++dcs_.released_superseded;
    else if (!std::strcmp(why, "stall")) ++dcs_.released_stall;
    else if (!std::strcmp(why, "space")) ++dcs_.released_space;
    else if (!std::strcmp(why, "camera")) ++dcs_.released_camera;
    else if (!std::strcmp(why, "rewritten")) ++dcs_.released_rewritten;
    else ++dcs_.released_retire;
}
bool Output::dc_views(const xr_owner::FrameInfo& info, const std::array<xr_owner::EyeSample, 2>& p0) {
    dc_.views_on = false;
    if (!info.views_tracked) return false;   // implies views_valid (two finite located views)
    for (unsigned e = 0; e < 2; ++e) { dc_.views[e].pose = info.views.eyes[e].pose; dc_.views[e].fov = p0[e].fov; }
    dc_.views_on = true;
    return true;
}
bool Output::dc_camera(const stereo::StereoFrame& f, const xr_owner::FrameInfo& info, acz::terrain::CamFrame& out, double& horizon_ms) {
    horizon_ms = 0;
    if (!dc_.configured || !(f.predicted_display_time > 0) || !(info.timing.predicted_time > 0)) return false;
    horizon_ms = double(static_cast<long double>(info.timing.predicted_time) - static_cast<long double>(f.predicted_display_time)) / 1e6;
    acz::pacing::Frame pf;
    const acz::pacing::ExtrapResult r = dc_.ex.predict(horizon_ms, pf);
    if (!(r.translated || r.rotated)) return false;
    horizon_ms = r.horizon_ms;
    for (int i = 0; i < 3; ++i) { out.eye[i] = pf.eye[i]; out.right[i] = pf.right[i]; out.up[i] = pf.up[i]; out.fwd[i] = pf.fwd[i]; }
    return true;
}
// An XR frame without a new game frame. Key off (or nothing held): pace() exactly. DECOUPLE: the held game frame is composed again for THIS XR frame's
// pose while it is young enough, tracked and of the current space generation; a stall beyond dcage releases it (the producer gets the slot back) and
// the old re-submit path continues up to reuse_max_age_ms.
void Output::no_new_frame(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
    if (dc_enabled() && dc_.held) {
        const int64_t age = g.now_ms() - dc_.fresh_ms;
        const auto& f = dc_.frame;
        // review 2 (Astra P2): P0's LOCAL generation is compared for EVERY held frame (an unposed frame's P0 = the located views of its first composite)
        const bool space_ok = info.space_generation == dc_.space_gen &&
            (!f.pose_valid || (f.space_generation == 0 ? info.space_generation == 0 : f.space_generation == encode_space_generation(info.space_generation)));
        // review 2 (Astra P2): the admitted camera stays valid only under the lock/stage epochs it was admitted with
        const auto& ep = s_.terrain->epochs ? *s_.terrain->epochs : acz::terrain::cam_epochs();
        const bool cam_ok = !dc_.cam_ep_valid || (ep.lock() == dc_.cam_lock && ep.stage() == dc_.cam_stage);
        // review 2 (Astra P2): the held images must still be the published bytes (no re-execution of the owning recording since publication)
        stereo::FrameSource* src = provider_ ? provider_() : nullptr;
        if (!space_ok) dc_release("space");
        else if (!cam_ok) dc_release("camera");
        else if (!src || !src->images_unchanged(f)) dc_release("rewritten");
        else if (double(age) > double(s_.terrain->pacing.decouple_max_age_ms)) dc_release("stall");
        else if (info.views_tracked) {
            dcs_.age_max_ms = std::max(dcs_.age_max_ms, double(age));
            l_ = Ledger{};
            l_.frame = f;   // the held slot stays owned by dc_ (l_.holding false): a failure never releases it from the ledger
            const bool ok = compose(g, info, plan, false, true);   // a failure is sticky (frame() clears the plan); the slot is released at retire
            // The check above is not enough alone (a re-execution submitted after it may run before our reads). compose() returned after observing
            // this composite's GPU completion; the producer records a re-execution BEFORE submitting it, so "unchanged" now proves the composite read
            // the published bytes. Otherwise its layer is not submitted, nothing re-submits it (have_last_ false: those images are suspect) and the
            // hold is released through the normal path (no consumer GPU work in flight: completion was observed).
            if (ok && !src->images_unchanged(dc_.frame)) {
                plan = {};
                have_last_ = false;
                ++dcs_.rewritten_dropped;
                dc_release("rewritten");
            }
            return;
        } else ++dcs_.untracked;
    }
    if (dc_enabled()) ++dcs_.reuse_fallback;
    pace(g, info, plan);
}
void Output::summary_decouple(int64_t now) {
    const double win = double(now - dc_.last_summary_ms);
    if (win < 1.0) return;
    const auto& a = dcs_; const auto& p = dcs_prev_;
    auto d = [&](uint64_t DecoupleStats::* m) { return (unsigned long long)(a.*m - p.*m); };
    const uint64_t ext = a.cam_extrap - p.cam_extrap;
    char t[768];
    std::snprintf(t, sizeof t, "xr-stereo;event=summary-decouple;t-ms=%lld;window-ms=%.0f;composites=%llu;composites-per-s=%.1f;fresh=%llu;new-per-s=%.1f;recomposed=%llu;"
        "reuse-fallback=%llu;retained=%llu;not-retained=%llu;released=%llu(superseded:%llu,stall:%llu,space:%llu,retire:%llu,camera:%llu,rewritten:%llu);"
        "rewritten-dropped=%llu;untracked=%llu;no-views=%llu;"
        "reproj-source=%llu;reproj-window=%llu;reproj-identity=%llu;reproj-refused=%llu;cam-extrap=%llu;cam-held=%llu;cam-none=%llu;horizon-ms-mean=%.1f;"
        "held-age-ms-max=%.1f;cam-delta-deg-max=%.2f;holding=%s",
        (long long)now, win, d(&DecoupleStats::composites), double(a.composites - p.composites) * 1000.0 / win, d(&DecoupleStats::fresh),
        double(a.fresh - p.fresh) * 1000.0 / win, d(&DecoupleStats::recomposed), d(&DecoupleStats::reuse_fallback), d(&DecoupleStats::retained),
        d(&DecoupleStats::not_retained), d(&DecoupleStats::released), d(&DecoupleStats::released_superseded), d(&DecoupleStats::released_stall),
        d(&DecoupleStats::released_space), d(&DecoupleStats::released_retire), d(&DecoupleStats::released_camera), d(&DecoupleStats::released_rewritten),
        d(&DecoupleStats::rewritten_dropped), d(&DecoupleStats::untracked), d(&DecoupleStats::no_views),
        d(&DecoupleStats::reproj_source), d(&DecoupleStats::reproj_window), d(&DecoupleStats::reproj_identity), d(&DecoupleStats::reproj_refused), (unsigned long long)ext,
        d(&DecoupleStats::cam_held), d(&DecoupleStats::cam_none), ext ? (a.horizon_sum_ms - p.horizon_sum_ms) / double(ext) : 0.0, a.age_max_ms, a.rot_max_deg,
        dc_.held ? "yes" : "no");
    emit(false, t);
    dcs_prev_ = dcs_;
    dcs_.age_max_ms = 0; dcs_.rot_max_deg = 0; dcs_prev_.age_max_ms = 0; dcs_prev_.rot_max_deg = 0;
    dc_.last_summary_ms = now;
}

bool Output::transact(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
    for (unsigned t = 0; t < kTargets; ++t)
        if (l_.acquired[t]) { fail("previous frame retained an image; no stale fallback"); return false; }
    if (l_.holding || (l_.submitted && !l_.complete)) { fail("previous frame retained; no stale fallback"); return false; }
    l_ = Ledger{};
    if (s_.vscreen && vscreen_exclusive()) { pace(g, info, plan); return true; }   // VSCREEN vspov:flat: the screen replaces the composite
    stereo::FrameSource* src = provider_ ? provider_() : nullptr;
    stereo::StereoFrame f{};
    if (!src || !src->try_acquire_latest(last_id_, f)) { no_new_frame(g, info, plan); return true; }   // key off: pace() exactly
    l_.holding = true; l_.frame = f; ++c_.acquired_frames;
    // Current producer assigns one frame id per publication; this is a snapshot,
    // and may advance between acquisition and this diagnostic load.
    const uint64_t latest = src->frames_published();
    select_lag_ = latest > f.frame_id ? latest - f.frame_id : 0;
    select_age_available_ = f.predicted_display_time && info.timing.predicted_time > 0;
    select_age_ms_ = select_age_available_ ?
        double(std::max<long double>(0, static_cast<long double>(info.timing.predicted_time) - f.predicted_display_time) / 1e6L) : 0;
    // OBJECT-INTERP (key off: not reached): the producer's horizon for the NEXT frames = the smoothed select age (one late frame does not throw objects).
    if (select_age_available_ && s_.terrain && s_.terrain->pacing.interp) { interp_.ema.feed(select_age_ms_); acz::interp::publish_horizon_ms(interp_.ema.v); }
    if (s_.select_corr_log) { // timing-anchor diagnostics: copy the facts now (the StereoFrame is released with its slot); no decision reads them
        corr_cand_ = Corr{};
        corr_cand_.valid = f.diag_valid; corr_cand_.frame_id = f.frame_id; corr_cand_.pose_seq = f.diag_pose_seq; corr_cand_.generation = f.diag_generation;
        corr_cand_.pose_predicted = f.predicted_display_time; corr_cand_.xr_frame = info.frame_index; corr_cand_.xr_predicted = info.timing.predicted_time;
        corr_cand_.t_start_us = f.diag_start_us; corr_cand_.t_publish_us = f.diag_publish_us;
        corr_cand_.t_select_us = mono_now_us();
        corr_cand_.members = f.members; corr_cand_.partial = f.partial; corr_cand_.start = f.diag_start; corr_cand_.look_src = f.diag_look_src;
        corr_cand_.look_meas_valid = f.diag_look_meas_valid; corr_cand_.look_valid = f.look_valid;
        corr_cand_.look[0] = f.look_yaw; corr_cand_.look[1] = f.look_pitch;
        corr_cand_.look_meas[0] = f.diag_look_meas_yaw; corr_cand_.look_meas[1] = f.diag_look_meas_pitch;
        corr_cand_.look_est[0] = f.diag_look_est_yaw; corr_cand_.look_est[1] = f.diag_look_est_pitch;
        size_t n = 0; // the producer's reason is a static string; copied through a character whitelist all the same
        for (const char* r = f.diag_refusal; r && *r && n + 1 < sizeof corr_cand_.refusal; ++r) {
            const char c = *r;
            corr_cand_.refusal[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') ? c : '?';
        }
        if (!n) std::memcpy(corr_cand_.refusal, "none", 5);
    }
    const char* why = nullptr;
    if (!validate(f, why)) { release_slot(); fail(why); return false; }
    // First-frame polling must stop at the owner's actual startup deadline
    // minus the EndFrame reserve. Require at least the steady completion budget
    // before submission; a late frame must not silently consume the reserve.
    const bool startup_frame = info.frame_index == 1;
    const int64_t startup_deadline = std::max<int64_t>(0, g.startup_deadline_ms());
    const int64_t reserve = std::max<int64_t>(0, s_.first_end_reserve_ms);
    const int64_t completion_boundary = startup_deadline - std::min(startup_deadline, reserve);
    auto startup_budget_ok = [&] {
        if (!startup_frame) return true;
        const int64_t now = g.now_ms();
        if (now < completion_boundary && completion_boundary - now >= s_.completion_window_ms) return true;
        if (!l_.submitted) startup_budget_refused_.store(true, std::memory_order_release);   // PERF: nothing submitted: a cold-start case (retry_allowed)
        settle_without_gpu(g);
        fail("insufficient first-frame completion budget before submission");
        return false;
    };
    if (!startup_budget_ok()) return false;
    // Issue 29 verdict 2 (5): the CURRENT admission applies to FRESH frames too.
    // Tracking invalid now: the frame is released unused and nothing is submitted.
    if (!info.views_tracked) {
        release_slot();
        ++c_.fresh_stale_tracking;
        if (ever_new_) ++c_.stale_zero; else ++c_.zero_no_frame;
        return true;
    }
    // A posed render carries the LOCAL space generation it was sampled under; a
    // render from another generation (e.g. published before a recenter, consumed
    // after it) is released unused, never relabelled into the new coordinates.
    if (f.pose_valid) {
        const bool unknown = f.space_generation == 0;
        if (unknown) ++c_.space_unknown;
        const bool mismatch = unknown ? info.space_generation != 0 : f.space_generation != encode_space_generation(info.space_generation);
        if (mismatch) {
            release_slot();
            ++c_.fresh_stale_space;
            no_new_frame(g, info, plan); // an older image of the CURRENT generation may still be reused (key off: pace() exactly)
            return true;
        }
    }
    // J-cockpit-fov-consumer: the images were rendered with the frame's own per-eye FOV.
    // If that FOV is refused, NO layer FOV can match the render (the lens FOV would
    // stretch/shrink it): the frame is released unused before any acquire or GPU work,
    // counted, and this XR frame submits zero layers; the previous layers are not
    // re-submitted afterwards either (same as the other per-frame refusals).
    if (f.cockpit_fov_valid && !(cockpit_fov_ok(f.cockpit_fov[0]) && cockpit_fov_ok(f.cockpit_fov[1]))) {
        release_slot();
        last_id_ = f.frame_id; // counted once per producer frame, not once per XR frame
        ++c_.cockpit_fov_invalid;
        if (ever_new_) ++c_.stale_zero; else ++c_.zero_no_frame;
        have_last_ = false;
        cockpit_fov_source_ = lens_ok(f.lens_tan) && f.lens_valid ? 1 : 0; // the flag is refused: report the fallback source
        return true;
    }
    // Issue 29 (2): never enqueue a GPU Wait on the game's queue for a value that is
    // not correlated with the producer's publication. CPU-observed completion needs
    // no wait; otherwise the frame must carry the producer's epoch and its own
    // last-signalled bookkeeping (value <= last_signalled) on the registered fence.
    // Anything else is DEFERRED: the slot is released unused (no image acquired, no
    // GPU work); a later XR frame takes the next published frame, whose completion
    // or authentication is checked again. The XR frame itself follows pace().
    bool presynced = false;
    Rc rc0 = g.call(Op::d3d_sample, [&] { presynced = blit_->fence_completed(f.fence, f.fence_value); return Rc::ok; });
    if (!succeeded(rc0)) { release_slot(); fail("producer fence check refused"); return false; }
    if (!presynced) {
        // Trust anchor: the producer's explicit registration, never the frame itself.
        const bool authenticated = f.fence_epoch != 0 && f.last_signalled >= f.fence_value &&
            producer_fence_registered(f.fence, f.fence_epoch);
        if (!authenticated) {
            release_slot();
            ++c_.deferred;
            no_new_frame(g, info, plan);   // key off: pace() exactly
            return true;
        }
    }
    // review 3 (Astra open risk 1): the FIRST composite is held to the same source-validity proof as a re-composite. Images written again since their
    // publication (a re-execution of the owning recording, or an Execute the producer could not examine) are released unused before any GPU work; the
    // previous layers were composed from another frame and stay as they were (no_new_frame; key off: pace() exactly).
    if (!src->images_unchanged(f)) {
        release_slot();
        ++c_.fresh_rewritten_pre;
        no_new_frame(g, info, plan);
        return true;
    }
    // DECOUPLE: a newer game frame is admitted: the held one is released now (its last composite completed, CPU-observed), so the producer gets its slot
    // back before this frame's GPU work (key off: never held).
    if (dc_.held) dc_release("superseded");
    return compose(g, info, plan, true, presynced);
}

// The composite of l_.frame into newly acquired swapchain images. fresh = a newly admitted game frame (the existing path; key off this is the only one);
// fresh = false (DECOUPLE) = the held game frame composed again for this XR frame's own head pose (only the eye targets; the HUD/marker quads keep their
// images and are submitted again from the first composite's plan).
bool Output::compose(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan, bool fresh, bool presynced) {
    const stereo::StereoFrame& f = l_.frame;
    const bool startup_frame = info.frame_index == 1;
    const int64_t startup_deadline = std::max<int64_t>(0, g.startup_deadline_ms());
    const int64_t reserve = std::max<int64_t>(0, s_.first_end_reserve_ms);
    const int64_t completion_boundary = startup_deadline - std::min(startup_deadline, reserve);
    auto startup_budget_ok = [&] {
        if (!startup_frame) return true;
        const int64_t now = g.now_ms();
        if (now < completion_boundary && completion_boundary - now >= s_.completion_window_ms) return true;
        if (!l_.submitted) startup_budget_refused_.store(true, std::memory_order_release);   // PERF: nothing submitted: a cold-start case (retry_allowed)
        settle_without_gpu(g);
        fail("insufficient first-frame completion budget before submission");
        return false;
    };
    if (fresh) {
    if (last_id_ && f.frame_id > last_id_ + 1) c_.frame_id_gaps += f.frame_id - last_id_ - 1;
    last_id_ = f.frame_id;
    c_.last_frame_id = f.frame_id;
    if (f.partial) ++c_.partial_frames;
    }
    // J-180: a frame whose world-object replay lost eligible scene draws. The objects are baked into the eye images (cannot be dropped here): the
    // frame is still submitted (default behaviour), counted, and logged once. Never set with the producer's world-objects key off.
    if (fresh && f.world_incomplete) {
        ++c_.world_incomplete_frames; c_.world_lost_draws += f.world_lost;
        if (!first_world_incomplete_logged_) {
            first_world_incomplete_logged_ = true;
            char text[200];
            std::snprintf(text, sizeof text, "xr-stereo;event=world-incomplete;frame=%llu;frame_id=%llu;slot=%u;world-lost=%u;action=submitted-with-cockpit;no-headset-claim",
                (unsigned long long)info.frame_index, (unsigned long long)f.frame_id, f.slot, f.world_lost);
            emit(true, text);
        }
    }
    const uint32_t dsrv = depth_srv_format(f.depth_format);
    const bool use_depth = f.depth[0] && f.depth[1] && f.depth[0] != f.depth[1] && dsrv != 0;
    bool use_world = f.world_valid && f.world && (s_.world_in_projection || chains_[kQuad]); // composite: no quad swapchain
    // Issue 29 verdict 2 (6): the quad is sized from the lens; invalid lens values
    // refuse the quad for this frame (no fallback values), counted.
    if (use_world && !lens_ok(f.lens_tan)) { use_world = false; if (fresh) ++c_.world_lens_invalid; }
    // J-world-in-projection: the world is composited into the eye images with the layer's own poses/FOV
    // and the world-plane anchor; the anchor is captured here (before the blit) with the layer's views.
    bool composite = false;
    float cache_ly = 0.0f, cache_lp = 0.0f; bool cache_look_ok = true; // J-world-cache: the look the plane is turned by (and whether the producer measured it)
    std::array<xr_owner::EyeSample, 2> cviews{};
    std::array<WorldComposite, 2> cparams{};
    // J-world-hold: no world for this frame (refused/absent at the producer) but a good one was captured within kWorldHoldMs: composite
    // the held image instead, with the lens it was rendered with and a fade to black over the second half of the window. The lens
    // refusal above (a world that exists but whose lens is invalid) is NOT held: it stays counted exactly as before.
    bool held = false;
    float world_fade = 1.0f;
    float lens_used[4] = {f.lens_tan[0], f.lens_tan[1], f.lens_tan[2], f.lens_tan[3]};
    if (s_.world_hold_last && !(f.world_valid && f.world)) {
        const int64_t age = g.now_ms() - hold_ms_;
        if (hold_valid_ && age >= 0 && age < kWorldHoldMs) {
            held = true; use_world = true; world_fade = world_hold_fade(age, kWorldHoldMs);
            for (int i = 0; i < 4; ++i) lens_used[i] = hold_lens_[i];
            if (fresh) c_.world_hold_max_age_ms = std::max<uint64_t>(c_.world_hold_max_age_ms, uint64_t(age));
        } else if (fresh) ++c_.world_hold_expired;
    }
    // DECOUPLE: the render pose P0 of the images (the previous rule) and, when this XR frame is tracked, P1 = this frame's own poses with P0's FOV.
    std::array<xr_owner::EyeSample, 2> dc_p0{};
    bool dc_p0_ok = false;
    if (dc_enabled() && use_world && s_.world_in_projection) {
        if (fresh) { dc_.views_on = false; dc_p0_ok = composite_views(info, dc_p0); }
        else { dc_p0 = dc_.p0; dc_p0_ok = true; }
        if (dc_p0_ok && !dc_views(info, dc_p0)) { if (info.views_valid) ++dcs_.untracked; else ++dcs_.no_views; }
    }
    if (use_world && s_.world_in_projection) {
        composite = composite_views(info, cviews);
        if (composite) {
            xr_owner::LayerPlan tmp;
            tmp.layer.views = cviews;
            anchor(info, tmp);
            const float yaw = anchor_neutral_ ? neutral_yaw_ : 0.0f;
            // xr_look_drive_keys: the world plane is turned by the look it was rendered with (a held world: the look it was captured with).
            float ly = 0.0f, lp = 0.0f;
            if (held) { ly = hold_look_[0]; lp = hold_look_[1]; } else cache_look_ok = look_of(f, ly, lp);
            cache_ly = ly; cache_lp = lp;
            float leaned[3];
            const float* na = lean_anchor(f, leaned) ? leaned : neutral_; // F-LEAN (key off: neutral_ itself)
            for (unsigned e = 0; e < 2 && composite; ++e)
                composite = world_composite_params(cviews[e], na, yaw + ly, lens_used, s_.quad_distance_m, cparams[e], lp);
        }
        if (!composite) { use_world = false; held = false; if (fresh) ++c_.world_composite_refused; }
    }
    if (!composite) dc_.views_on = false;   // DECOUPLE: only a world composite is composed at P1 (and only such a frame is held for re-composites)
    if (fresh) {
    if (use_world && !held) ++c_.world_frames;
    if (composite && !held) ++c_.world_composite_frames;
    if (held) { ++c_.world_held; ++cam_.held; }
    if (s_.cockpit_auto) cockpit_auto_tick();   // AIRCRAFT-AUTO: may install/replace/drop s_.cockpit before this frame's cockpit block (key off: not reached)
    }
    // DECOUPLE: the camera the objects image was drawn with and the terrain camera of this composite (the camera-delta rotation of the re-projection)
    bool dc_cam_ok = false;
    acz::terrain::CamFrame dc_cam_obj{}, dc_cam_t{};
    // J-terrain-render: the stage terrain is the base world layer of this composite. Needs the E6 camera (published by the Present tick); without it the frame
    // keeps the game's own world image exactly as with the key off (counted).
    bool terrain_on = false;
    std::array<TerrainEye, 2> terrain_eyes{};
    acz::terrain::CamFrame tac_cam{};   // TACTICAL: the terrain camera of this frame (game camera, else E6), no head offset
    float stage_light_model[3] = {0, 0, 0}, stage_light_tint[3] = {1, 1, 1};   // COCKPIT-3: the stage sun in the cockpit frame and its colour
    bool stage_light_ok = false, stage_tint_ok = false;
    float canopy_sky[3] = {0, 0, 0}; bool canopy_sky_ok = false;   // COCKPIT-4: the canopy reflection colour (stage sky row 1, stop 1, display)
    acz::cockpit::ReflBasis refl_basis{}; bool refl_basis_ok = false;   // SKIN-REFL: the game's world axes in the cockpit frame of this camera
    if (fresh) {   // DECOUPLE: a re-composite keeps the held frame's camera snapshot / mode (the world-hold capture and the occlusion rule read them)
    frame_cam_valid_ = false;
    frame_ep_valid_ = false;
    cam_mode_ = acz::terrain::CamMode::none;
    }
    // COCKPIT-3/4 lighting of a terrain camera (stage sun in the cockpit frame, canopy sky colour); shared by the first composite and the re-composites.
    auto stage_lighting = [&](const acz::terrain::CamFrame& cf) {
        if (s_.cockpit && s_.terrain->stage.has_env()) {   // COCKPIT-4: the canopy reflects the stage's mid sky colour
            const float* sk = &s_.terrain->stage.env_params.data()[size_t(acz::terrain::kPSky + 4u * 1u + 1u) * 4];
            for (int i = 0; i < 3; ++i) canopy_sky[i] = sk[i];
            canopy_sky_ok = std::isfinite(sk[0]) && std::isfinite(sk[1]) && std::isfinite(sk[2]);
        }
        if (s_.cockpit && s_.cockpit->opt.stage_light && s_.terrain->stage.has_env()) {
            const float* ep = s_.terrain->stage.env_params.data();
            stage_light_ok = acz::cockpit::stage_light_in_model(&ep[size_t(acz::terrain::kPLight) * 4], cf.right, cf.up, cf.fwd, stage_light_model);
            stage_tint_ok = stage_light_ok &&
                acz::cockpit::stage_tint(&ep[size_t(acz::terrain::kPAmbient) * 4], &ep[size_t(acz::terrain::kPLightCol) * 4], stage_light_tint);
        }
        if (s_.cockpit && s_.cockpit->opt.skin_refl)   // SKIN-REFL: same camera frame as the stage light (the drawn stage's map is attached to the job below)
            refl_basis_ok = acz::cockpit::refl_basis(cf.right, cf.up, cf.fwd, s_.terrain->conv.yaw_sign, s_.terrain->conv.yaw_offset_deg, refl_basis);
    };
    if (s_.terrain && composite && !fresh) {
        // DECOUPLE re-composite: no new camera sample exists; the frame camera is dead-reckoned to THIS XR frame's display time (held at the first
        // composite's camera when the model refuses), the objects keep the camera they were drawn with (dc_.cam_obj: the re-projection's rotation).
        if (dc_.cam_valid) {
            const auto& cv = s_.terrain->conv;
            acz::terrain::CamFrame cf = dc_.cam_fresh;
            double h = 0;
            acz::terrain::CamFrame pr{};
            if (!cv.fixed_cam && dc_camera(f, info, pr, h)) { cf = pr; ++dcs_.cam_extrap; dcs_.horizon_sum_ms += h; } else ++dcs_.cam_held;
            terrain_on = true;
            tac_cam = cf;
            for (unsigned e = 0; e < 2; ++e) {
                acz::terrain::eye_world(cf, cparams[e].origin, cv.head_units_per_metre, terrain_eyes[e].eye);
                for (int i = 0; i < 3; ++i) { terrain_eyes[e].right[i] = cf.right[i]; terrain_eyes[e].up[i] = cf.up[i]; terrain_eyes[e].fwd[i] = cf.fwd[i]; }
            }
            stage_lighting(cf);
            dc_cam_ok = true; dc_cam_obj = dc_.cam_obj; dc_cam_t = cf;
            ++c_.terrain_frames;
        } else { ++dcs_.cam_none; ++c_.terrain_no_camera; }
    }
    if (s_.terrain && composite && fresh) {
        acz::terrain::CamSample cs;
        const auto& cv = s_.terrain->conv;
        bool have = false;
        acz::terrain::CamSource cam_src = acz::terrain::CamSource::none;
        if (cv.fixed_cam) {
            cs.campos[0] = cv.cam[0]; cs.campos[1] = cv.cam[1]; cs.campos[2] = cv.cam[2];
            cs.yaw = cv.cam[3] * 3.14159265358979f / 180.0f; have = true;
            cs.pitch = cv.cam[4] * 3.14159265358979f / 180.0f; cs.roll = cv.cam[5] * 3.14159265358979f / 180.0f;   // PROPS-SHADING (0 = the old level camera)
        } else if (cv.cam_admit) {   // CAM-EPOCH: fresh, same-epoch, correlated sample or the explicit no-camera path (never the newest instead)
            have = admit_camera(f, cs, cam_src);
            if (have && cam_src == acz::terrain::CamSource::frame) ++c_.terrain_e6_frame;
        } else if (cv.e6_frame && f.e6_valid) {   // WORLD-1: the E6 sample read while THIS frame was recorded (not the newest one)
            for (int i = 0; i < 3; ++i) cs.campos[i] = f.e6_campos[i];
            cs.pitch = f.e6_pitch; cs.yaw = f.e6_yaw; cs.roll = f.e6_roll; cs.heading = f.e6_heading; cs.seq = f.e6_seq;
            have = true; ++c_.terrain_e6_frame;
        } else have = s_.terrain->camera ? s_.terrain->camera(cs) : acz::terrain::cam_mailbox().try_read(cs);
        acz::terrain::CamFrame ef{}, cf{};
        const bool e6ok = have && acz::terrain::camera_frame(cs.campos, cs.pitch, cs.yaw, cs.roll, cv, ef);
        // WORLD-1: ONE camera for terrain, replayed objects and markers: the game camera of this frame (orientation from the cockpit camera, position from the
        // frame's DrawACM_v camera, else E6), cross-checked against E6; E6 alone when the frame carries no game camera (or csrc:e6 / the fixed test camera).
        bool ok = false, used_game = false, game_refused = false;
        // CAM-EPOCH (cam_admit): the E6 eye and the gmax cross-check only from the E6 sample of the SAME frame (never an old replay orientation with a
        // newer E6 position); legacy: any E6 sample, as before.
        bool e6_pair = e6ok && (!cv.cam_admit || cam_src == acz::terrain::CamSource::frame);
        bool e6_far = false;   // E6-PLAUSIBLE: the E6 eye is nowhere near the game camera's eye (stale/garbage block): it cannot veto the game camera
        if (!cv.fixed_cam && cv.cam_source_game && f.game_cam_valid) {
            const float* eye = f.game_eye_valid ? f.game_eye : (e6_pair ? ef.eye : nullptr);
            acz::terrain::CamFrame gf{};
            if (eye && acz::terrain::camera_frame_axes(eye, f.game_cam_right, f.game_cam_up, f.game_cam_fwd, cv, gf)) {
                // camera_frame_axes applied alt/fixy to the eye; an E6 eye already had them: undo the double application
                if (!f.game_eye_valid) for (int i = 0; i < 3; ++i) gf.eye[i] = ef.eye[i];
                if (e6_pair && f.game_eye_valid && !acz::terrain::e6_eye_agrees(gf.eye, ef.eye)) { e6_far = true; e6_pair = false; ++c_.terrain_e6_implausible; }
                // W15 F2: a Present-history / latest E6 sample (e6_pair false: no gmax veto) that is grossly far from the game eye must not feed the cockpit animation either
                else if (acz::terrain::e6_eye_far(e6ok, f.game_eye_valid, gf.eye, ef.eye)) e6_far = true;
                const float ang = e6_pair ? acz::terrain::frame_angle_deg(gf, ef) : 0.0f;
                if (e6_pair) c_.terrain_e6_angle_max_cdeg = std::max<uint64_t>(c_.terrain_e6_angle_max_cdeg, uint64_t(ang * 100.0f));
                if (!e6_pair || ang <= cv.game_cam_max_deg) { cf = gf; ok = true; used_game = true; ++c_.terrain_src_game; if (f.game_eye_valid) ++c_.terrain_eye_game; else ++c_.terrain_eye_e6; }
                else { ++c_.terrain_game_refused; game_refused = true; }
            }
        }
        if (!ok && e6ok) { cf = ef; ok = true; ++c_.terrain_src_e6; ++c_.terrain_eye_e6; }
        if (e6_far != e6_far_prev_) {   // E6-PLAUSIBLE: one line per transition (the per-window count is in summary-terrain-camera)
            e6_far_prev_ = e6_far;
            char et[420];
            std::snprintf(et, sizeof et, "xr-stereo;event=e6-implausible;state=%s;frame-id=%llu;e6-eye=%.1f,%.1f,%.1f;game-eye=%.1f,%.1f,%.1f;limit-units=%.0f;action=%s",
                e6_far ? "enter" : "exit", (unsigned long long)f.frame_id, double(ef.eye[0]), double(ef.eye[1]), double(ef.eye[2]),
                double(f.game_eye_valid ? f.game_eye[0] : 0.0f), double(f.game_eye_valid ? f.game_eye[1] : 0.0f), double(f.game_eye_valid ? f.game_eye[2] : 0.0f),
                double(acz::terrain::kE6EyeAgreeUnits), e6_far ? "game-camera-kept(e6-cannot-veto)" : "e6-agrees-again");
            emit(false, et);
        }
        if (!cv.fixed_cam && cv.cam_admit) {
            using acz::terrain::CamMode;
            using acz::terrain::CamSource;
            CamMode mode = CamMode::none;
            if (used_game) mode = e6_pair ? CamMode::correlated_hybrid : CamMode::game_only;
            else if (ok) mode = game_refused ? CamMode::e6_game_refused : cam_src == CamSource::frame ? CamMode::e6_frame
                              : cam_src == CamSource::fallback ? CamMode::e6_fallback : CamMode::e6_latest;
            cam_mode_ = mode;
            camera_outcome(f, mode);
            if (e6ok) { frame_cam_valid_ = true; frame_cam_ = cs; }
            const int64_t ref_ns = f.diag_publish_us > 0 ? f.diag_publish_us * 1000 : mono_now_us() * 1000;
            camera_lag_frame(f, e6ok, ef, e6ok ? double(ref_ns - cs.t_ns) / 1e6 : 0.0);
            camlag_frame(f, e6ok, ef);
            last_cam_fid_ = f.frame_id; last_cam_seq_ = e6ok ? cs.seq : 0; last_cam_mode_ = mode;
        }
        // OBJECT-INTERP (key off: not reached): the producer's camera delta, the one the replayed objects were drawn with; extrap:1 only without a plan.
        const acz::terrain::CamFrame cf_raw = cf;   // DECOUPLE: the frame camera (game state at frame time)
        const bool interp_used = ok && s_.terrain->pacing.interp && !cv.fixed_cam && interp_camera(f, g.now_ms(), cf, cv.fixed_alt);
        const acz::terrain::CamFrame cf_obj = cf;   // DECOUPLE: the camera the replayed objects were drawn with (frame camera + the interp delta)
        if (interp_used) extrap_.lead_s = 0;   // tactical positions are already display-time positions
        else if (ok && s_.terrain->pacing.extrap && !cv.fixed_cam) extrap_camera(f, g.now_ms(), cf);   // EXTRAP (key off: not reached)
        else extrap_.lead_s = 0;
        if (ok && dc_.views_on) {
            // DECOUPLE: one display-time camera model for EVERY composite of this game frame (this one and its re-composites): the frame camera dead-reckoned
            // per game frame to this XR frame's display time; when the model refuses, the camera above (interp / extrap / frame) is kept.
            if (!dc_.configured) {
                acz::pacing::ExtrapConfig c; c.max_ms = s_.terrain->pacing.decouple_horizon_ms; c.attitude = s_.terrain->pacing.extrap_attitude;
                dc_.ex.configure(c); dc_.configured = true;
            }
            acz::pacing::Frame in;
            for (int i = 0; i < 3; ++i) { in.eye[i] = cf_raw.eye[i]; in.right[i] = cf_raw.right[i]; in.up[i] = cf_raw.up[i]; in.fwd[i] = cf_raw.fwd[i]; }
            const int64_t t_us = f.diag_publish_us > 0 ? f.diag_publish_us
                : int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if (!cv.fixed_cam) dc_.ex.update(f.frame_id, t_us, in);
            double h = 0;
            acz::terrain::CamFrame pr{};
            if (!cv.fixed_cam && dc_camera(f, info, pr, h)) { cf = pr; ++dcs_.cam_extrap; dcs_.horizon_sum_ms += h; } else ++dcs_.cam_held;
            dc_cam_ok = true; dc_cam_obj = cv.fixed_cam ? cf : cf_obj; dc_cam_t = cf;
        }
        if (ok) {
            terrain_on = true;
            tac_cam = cf;
            if (s_.terrain->takeoff && !cv.fixed_cam) takeoff_sample(cf_raw.eye);   // TAKEOFF: mission stage or the airbase stage for this game frame
            for (unsigned e = 0; e < 2; ++e) {
                acz::terrain::eye_world(cf, cparams[e].origin, cv.head_units_per_metre, terrain_eyes[e].eye);
                for (int i = 0; i < 3; ++i) { terrain_eyes[e].right[i] = cf.right[i]; terrain_eyes[e].up[i] = cf.up[i]; terrain_eyes[e].fwd[i] = cf.fwd[i]; }
            }
            // COCKPIT-3: the stage's directional light (WORLD-2 env P[1], stage space) expressed in the cockpit frame of THIS camera (aircraft attitude included),
            // so the model cockpit is lit like the terrain under it; without an env section the model keeps its fixed light. COCKPIT-4: the canopy sky colour.
            stage_lighting(cf);
            // COCKPIT-4 anim:1: one new E6 sample (by sequence) -> attitude rates -> smoothed stick/pedal/throttle commands. Time = the frame's publication
            // time when the producer filled it, else the consumer's clock (both steady_clock microseconds).
            // CAM-EPOCH (cam_admit): the admitted sample only, keyed by source + sequence (the frame and Present mailboxes number separately), timed by its
            // own publication time, and the rate state restarts when the lock/stage epoch changes (a relock jump is not a stick deflection).
            const uint64_t anim_key = cv.cam_admit ? cs.seq * 4u + uint64_t(cam_src) : cs.seq;
            if (s_.cockpit && s_.cockpit->opt.anim && e6ok && !e6_far && cs.seq != 0 && anim_key != anim_last_seq_) {
                anim_last_seq_ = anim_key;
                if (cv.cam_admit) {
                    if (anim_epoch_known_ && (cs.lock_epoch != anim_lock_ || cs.stage_epoch != anim_stage_) && anim_state_.have) {
                        anim_state_.have = false; anim_state_.have_speed = false; ++anim_state_.resets; ++cam_.anim_epoch_resets;
                    }
                    anim_epoch_known_ = true; anim_lock_ = cs.lock_epoch; anim_stage_ = cs.stage_epoch;
                }
                const int64_t us = cv.cam_admit && cs.t_ns > 0 ? cs.t_ns / 1000 : f.diag_publish_us > 0 ? f.diag_publish_us
                    : int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                const float apos[3] = {cs.campos[0], cs.campos[1], cs.campos[2]};
                acz::cockpit::anim_update(anim_state_, ef.right, ef.up, ef.fwd, apos, double(us) * 1e-6);
                ++c_.cockpit_anim_updates;
            }
            ++c_.terrain_frames;
        } else ++c_.terrain_no_camera;
    }
    // LOOK-STICK GATE: this composite drew the world from the game/E6 camera (which carries the game's cockpit look-around), i.e. we are in flight.
    // The XR glue publishes the right-stick gate heartbeat while this count moves (the fixed test camera does not count).
    if (terrain_on && s_.terrain && !s_.terrain->conv.fixed_cam) flight_composites_.fetch_add(1, std::memory_order_relaxed);
    // COCKPIT-2: the model cockpit is drawn with EXACTLY the views/FOV the projection layer submits, in the cockpit frame (the seat anchor and its heading,
    // never the lean or the game look): cockpit-local eye = pvb eye + upm * Ry(-yaw0)(eye - anchor), orientation Ry(-yaw0) * head.
    bool cockpit_on = false;
    std::array<acz::cockpit::Constants, 2> cockpit_eyes{};
    if (s_.cockpit && composite && !f.no_cockpit) {   // POV-RENDER stage 2: a no-cockpit chase frame has no cockpit (pov.md draw_cockpit_model)
        const float yaw0 = anchor_neutral_ ? neutral_yaw_ : 0.0f;
        const auto& ck = *s_.cockpit;
        bool ok = true;
        for (unsigned e = 0; e < 2 && ok; ++e) {
            const float fov[4] = {cviews[e].fov.left, cviews[e].fov.right, cviews[e].fov.up, cviews[e].fov.down};
            acz::cockpit::EyeCam cam{};
            // COCKPIT-SCALE: view_upm/view_near = upm/cscale, near/cscale (== the model scaled by cscale about the pvb eye; cscale 1 = upm, near exactly).
            ok = acz::cockpit::eye_camera(cviews[e].pose.orientation, cviews[e].pose.position, fov, neutral_, yaw0, ck.mesh.eye, acz::cockpit::view_upm(ck.opt),
                                          acz::cockpit::view_near(ck.opt), cam);
            if (ok) cockpit_eyes[e] = acz::cockpit::make_constants(cam, ck.opt, s_.exposure, uint32_t(s_.tonemap), stage_light_ok ? stage_light_model : nullptr,
                                                                   stage_tint_ok ? stage_light_tint : nullptr, canopy_sky_ok ? canopy_sky : nullptr,
                                                                   refl_basis_ok ? &refl_basis : nullptr,
                                                                   // CFID: the rim shadow only under the moving stage sun (a fixed kLight shadow would be made up)
                                                                   (stage_light_ok && ck.mesh.shadow) ? ck.mesh.shadow_box : nullptr);
        }
        if (ok) {
            cockpit_on = true; if (stage_light_ok) ++c_.cockpit_stage_light;
            if (cockpit_eyes[0].flags & acz::cockpit::kCFlagFid) ++c_.cockpit_fid_frames;
            if (cockpit_eyes[0].flags & acz::cockpit::kCFlagShadow) ++c_.cockpit_shadow_frames;
        } else ++c_.cockpit_no_view;
    }
    const bool capture = s_.world_hold_last && composite && !held && fresh;   // DECOUPLE: a re-composite never re-captures the same world
    // J-hud-consumer: only with the key. The HUD belongs to THIS frame only: a frame
    // without hud_valid (or with a refused HUD) acquires no HUD image and its plan has
    // no HUD quad, so a previous HUD is never shown with a newer cockpit/world.
    // DECOUPLE re-composite: the HUD / marker images of this same game frame stay in their swapchains (last released image); their quads are submitted
    // again from the first composite's plan, nothing is acquired or drawn for them.
    bool use_hud = false;
    if (s_.hud_layer && fresh) {
        if (!f.hud_valid) ++c_.hud_skipped;
        else if (!chains_[kHud] || !f.hud || !source_format_ok(f.hud_format) || f.hud == f.eye[0] || f.hud == f.eye[1] ||
                 (f.world && f.hud == f.world)) { ++c_.hud_invalid; ++c_.hud_skipped; }
        else if (s_.hud_mode == HudMode::world_quad && !lens_ok(f.lens_tan)) { ++c_.hud_lens_invalid; ++c_.hud_skipped; }
        else use_hud = true;
        last_hud_draws_ = f.hud_draws; last_hud_refused_ = f.hud_refused;
        hud_menu_frame(f, f.hud_draws);   // HUD-MENU: fresh game frames only
    }
    // J-hud-markers: only with the key. Like the HUD, the markers belong to THIS frame only (never an older marker image).
    bool use_markers = false;
    if (s_.hud_markers_layer && fresh) {
        if (!f.hud_markers_valid) ++c_.marker_skipped;
        else if (!chains_[kMarkers] || !f.hud_markers || !source_format_ok(f.hud_format) || f.hud_markers == f.eye[0] || f.hud_markers == f.eye[1] ||
                 (f.world && f.hud_markers == f.world) || (f.hud && f.hud_markers == f.hud)) { ++c_.marker_invalid; ++c_.marker_skipped; }
        else if (!lens_ok(f.lens_tan)) { ++c_.marker_lens_invalid; ++c_.marker_skipped; }
        else use_markers = true;
        last_marker_draws_ = f.hud_marker_draws; last_marker_refused_ = f.hud_marker_refused;
    }
    // J-effects-layer: only with the key, inside a FRESH world composite, and only this frame's own effects image (never an older one).
    bool use_effects = false;
    if (s_.effects_layer && !fresh) {   // DECOUPLE re-composite: the same rule for the held frame's own effects image, without the per-frame counters
        use_effects = composite && !held && f.effects_valid && (f.effects_repr == stereo::effects_repr_legacy || f.effects_repr == stereo::effects_repr_coverage_v1) &&
            f.effects && source_format_ok(f.hud_format) && f.effects != f.eye[0] && f.effects != f.eye[1] && f.effects != f.world &&
            !(f.hud && f.effects == f.hud) && !(f.hud_markers && f.effects == f.hud_markers);
    } else if (s_.effects_layer) {
        ++c_.effects_seen; // J-test8-fixes: the producer's side of every consumed frame, for the 5 s summary
        if (f.effects_valid) { ++c_.effects_prod_valid; c_.effects_prod_draws += f.effects_draws; c_.effects_prod_refused += f.effects_refused; c_.effects_prod_tail += f.effects_tail; }
        if (!composite || held || !f.effects_valid) ++c_.effects_skipped;
        else if (f.effects_repr != stereo::effects_repr_legacy && f.effects_repr != stereo::effects_repr_coverage_v1) { ++c_.effects_invalid; ++c_.effects_skipped; } // J-effects-dual: unknown representation: fail closed
        else if (!f.effects || !source_format_ok(f.hud_format) || f.effects == f.eye[0] || f.effects == f.eye[1] || f.effects == f.world ||
                 (f.hud && f.effects == f.hud) || (f.hud_markers && f.effects == f.hud_markers)) { ++c_.effects_invalid; ++c_.effects_skipped; }
        else use_effects = true;
    }
    l_.used = {true, true, use_world && !composite, use_hud, use_markers}; // composite: no quad image is acquired
    if (f.width != mapped_w_ || f.height != mapped_h_) {
        for (unsigned t = 0; t < kTargets; ++t) {
            map_[t] = full_rect(f.width, f.height, ext_[t].w, ext_[t].h);
            if (!map_[t].ok) { release_slot(); fail("full-rect mapping refused"); return false; }
        }
        mapped_w_ = f.width; mapped_h_ = f.height;
        const bool lens = f.lens_valid && lens_ok(f.lens_tan);
        static constexpr float kNominalLens[4] = {0.6152f, 0.6152f, 0.3460f, 0.3460f}; // logging only
        const xr_owner::Fov fv = fov_from_lens(lens ? f.lens_tan : kNominalLens);
        char text[440];
        std::snprintf(text, sizeof text, "xr-stereo;event=mapping;source=%ux%u;fmt=%u;eye0=0,0,%ux%u;eye1=0,0,%ux%u;quad=0,0,%ux%u;scale=%.6f;"
            "aspect-match=%u;lens=%s;fov-deg=%.3f,%.3f,%.3f,%.3f;quad-distance-m=%.1f;tonemap=%s;exposure=%.3f;hud=%s",
            f.width, f.height, f.dxgi_format, ext_[0].w, ext_[0].h, ext_[1].w, ext_[1].h, ext_[kQuad].w, ext_[kQuad].h, map_[0].scale,
            unsigned(uint64_t(f.width) * ext_[0].h == uint64_t(f.height) * ext_[0].w), lens ? "frame" : "none-pose-fov",
            deg(fv.left), deg(fv.right), deg(fv.up), deg(fv.down), double(s_.quad_distance_m),
            s_.tonemap == Tonemap::reinhard ? "reinhard" : "clamp", double(s_.exposure),
            s_.hud_layer ? (s_.hud_mode == HudMode::head_locked ? "layer-head-locked" : "layer-world-quad") : "not-included");
        emit(true, std::string(text) + eye_fov_tokens()); // J-cockpit-fov-consumer: additive trailing tokens
    }
    for (unsigned t = 0; t < kTargets; ++t) {
        if (!l_.used[t]) continue;
        uint32_t index = 0;
        if ((t == kHud || t < 2) && s_.vscreen) s_.vscreen->target_written(t);   // VSCREEN (Astra finding 4) / BRIEF3D: no longer the screen / table
        Rc rc = g.call(Op::acquire_image, [&] { return xr_->acquire(t, index); });
        if (!succeeded(rc)) { settle_without_gpu(g); fail("xrAcquireSwapchainImage failed"); return false; }
        l_.acquired[t] = true; l_.index[t] = index; // owned even on SESSION_LOSS_PENDING
        if (index >= images_[t].count) { settle_without_gpu(g); fail("invalid acquired image index; acquisition retained"); return false; }
        if (g.lost()) { settle_without_gpu(g); fail("session loss during acquire"); return false; }
        bool timed_out = false;
        rc = g.call(Op::wait_image, [&] { return xr_->wait(t, 0, timed_out); });
        if (succeeded(rc) && timed_out) rc = g.call(Op::wait_image, [&] { return xr_->wait(t, s_.wait_retry_ns, timed_out); });
        if (!succeeded(rc) || timed_out) {
            settle_without_gpu(g);
            fail(timed_out ? "xrWaitSwapchainImage timeout; acquisition outstanding" : "xrWaitSwapchainImage failed");
            return false;
        }
        l_.waited[t] = true; // a successful wait establishes RENDER_TARGET
        if (g.lost()) { settle_without_gpu(g); fail("session loss during image wait"); return false; }
    }
    BlitJob job;
    // EYE-RING (eye_ring.h): the per-eye images (eyes, depth, smoke, sprite) may be at the eye swapchain size; the game-screen images keep f.width x f.height.
    const uint32_t ew = stereo::eye_image_width(f), eh = stereo::eye_image_height(f);
    if ((ew != f.width || eh != f.height) && !eye_ring_logged_) {   // once per process; never without the eye ring
        eye_ring_logged_ = true;
        char er[200];
        std::snprintf(er, sizeof er, "xr-stereo;event=eye-ring-consumer;source=%ux%u;per-eye=%ux%u;eye0=%ux%u;sampled=uv-full-rect;game-screen-images=source-size",
            f.width, f.height, ew, eh, ext_[0].w, ext_[0].h);
        emit(true, er);
    }
    job.eyes = {{{f.eye[0], f.dxgi_format, ew, eh}, {f.eye[1], f.dxgi_format, ew, eh}}};
    job.use_depth = use_depth;
    if (use_depth) job.depth = {{{f.depth[0], f.depth_format, ew, eh}, {f.depth[1], f.depth_format, ew, eh}}};
    job.use_world = use_world;
    if (use_world && !held) job.world = {f.world, f.dxgi_format, f.width, f.height};
    job.world_held = held;
    job.world_capture = capture;
    job.world_fade = world_fade;
    job.world_extend = s_.world_extend && composite;
    const bool cache_on = s_.world_cache && composite && !terrain_on;   // J-world-cache (the terrain replaces the base world layer: no cache)
    if (terrain_on) {   // J-terrain-render
        job.terrain = true;
        job.terrain_blend = s_.terrain->conv.blend_window;
        const TerrainRuntime& dt = drawn_terrain();   // TAKEOFF: the airbase stage while the aircraft is on it, else the mission stage
        job.terrain_stage = &dt.stage; job.terrain_atlas = &dt.atlas; job.terrain_gen = dt.generation;
        job.terrain_eye = terrain_eyes;
        if (fresh) lensflare_diag(dt.env, dt.generation, s_.terrain->conv.env_bits, terrain_eyes, cparams);   // LENS-FLARE: proves per window whether the flare could fire
        if (fresh && wc_probe_due_ && s_.terrain->conv.wcloud && !dt.env.empty() && s_.terrain->conv.env_bits) {   // WCLOUD-FIX2: what the shader should add (CPU mirror)
            const acz::terrain::EnvView ev{dt.env.data(), s_.terrain->conv.env_bits};
            const TerrainEye& e0 = terrain_eyes[0]; const WorldComposite& c0 = cparams[0];
            wc_probe_ = acz::terrain::world_clouds_probe(dt.stage, dt.atlas, ev, e0.eye, c0.base, c0.ux, c0.vy, e0.right, e0.up, e0.fwd,
                                                         s_.terrain->conv.haze_l, s_.terrain->conv.far_dist);
            for (int i = 0; i < 3; ++i) wc_probe_eye_[i] = e0.eye[i];
            wc_probe_gen_ = dt.generation; ++wc_probe_n_; wc_probe_due_ = false;
        }
        if (!dt.env.empty() && s_.terrain->conv.env_bits) { job.terrain_env = &dt.env; job.terrain_env_bits = s_.terrain->conv.env_bits; }   // WORLD-2
        job.terrain_haze_l = s_.terrain->conv.haze_l; job.terrain_far = s_.terrain->conv.far_dist;
        job.terrain_quality = s_.terrain->conv.quality;   // GPU-COST q:0..2 (0 = reference)
        // WORLD-1 step 4: hide replayed objects behind nearer terrain. Only when the producer compresses object depth into the float depth-fix band (else the
        // object depth cannot be told from the cockpit's or decoded): typeless D32 (39) / D32_S8X24 (19) resources, the producer key latched, option occl:1.
        job.terrain_occlude = s_.terrain->conv.occlude_objects && s_.objects_depth_fix && use_depth &&
            (f.depth_format == 39u || f.depth_format == 40u || f.depth_format == 19u || f.depth_format == 20u);   // R32 / D32 / R32G8X24 / D32S8X24: float depth
        // CAM-EPOCH: terrain hiding replayed objects needs the terrain camera to be the objects' own camera of this frame; refused on E6-only display fallbacks.
        if (job.terrain_occlude && s_.terrain->conv.cam_admit && !s_.terrain->conv.fixed_cam && !acz::terrain::cam_mode_registers_objects(cam_mode_)) {
            job.terrain_occlude = false; ++cam_.occlude_refused;
        }
        if (job.terrain_occlude) ++c_.terrain_occlude_frames;
        // PROPS-SHADING objdom:display: decode the replayed object band once (same object-band conditions as the occlusion above, without the occl option)
        job.terrain_obj_display = s_.terrain->conv.obj_display && s_.objects_depth_fix && use_depth &&
            (f.depth_format == 39u || f.depth_format == 40u || f.depth_format == 19u || f.depth_format == 20u);
        job.props = dt.props.get();   // WORLD-3 (null = off)
        job.props_async = true;                // PERF: props GPU init on a worker; never inside a frame (terrain without props until ready)
    }
    if (cockpit_on) {   // COCKPIT-2
        job.cockpit = true;
        job.cockpit_keep_replay = s_.cockpit->opt.keep_replay;
        job.cockpit_prepass = s_.cockpit->opt.zpre;   // GPU-COST zpre:1
        job.cockpit_mesh = &s_.cockpit->mesh; job.cockpit_gen = s_.cockpit->generation;
        job.cockpit_eye = cockpit_eyes;
        const auto& ckm = s_.cockpit->mesh;
        // STORES-RT: the baked weapon stations still loaded (store_tracker.h; launches come from the MSL-HUD reader (hud_msl.h); a fired station reloads after strel seconds).
        uint32_t store_hidden = 0;
        if (s_.cockpit->opt.stores && !ckm.store_stations.empty()) {
            if (stores_gen_ != s_.cockpit->generation) {   // a new model: its stations, nothing fired, earlier signals belong to the previous aircraft
                stores_.configure(ckm); stores_.set_reload_ms(double(s_.cockpit->opt.strel) * 1000.0); stores_gen_ = s_.cockpit->generation; stores_sticky_ = false; stores_mask_ = 0; stores_changes_ = 0;
                acz::stores::signal().discard();
            }
            const double stores_now = double(g.now_ms());
            acz::stores::signal().sync(stores_, stores_now);
            store_hidden = stores_.hidden_mask(stores_now);
            if (store_hidden != stores_mask_) { ++stores_changes_; stores_mask_ = store_hidden; }
            if (store_hidden) stores_sticky_ = true;
        }
        // SP-STORES: the special-weapon pylons (sp<station> parts; the HUD SP row's launches; the same strel reload; spstores:0 leaves them static: no pieces, nothing to do).
        uint32_t sp_hidden = 0;
        if (s_.cockpit->opt.stores && s_.cockpit->opt.spstores && !ckm.sp_stations.empty()) {
            if (sp_stores_gen_ != s_.cockpit->generation) {
                sp_stores_.configure(ckm, true); sp_stores_.set_reload_ms(double(s_.cockpit->opt.strel) * 1000.0); sp_stores_gen_ = s_.cockpit->generation;
                sp_stores_sticky_ = false; sp_stores_mask_ = 0; sp_stores_changes_ = 0;
                acz::stores::sp_signal().discard();
            }
            const double sp_now = double(g.now_ms());
            acz::stores::sp_signal().sync(sp_stores_, sp_now);
            sp_hidden = sp_stores_.hidden_mask(sp_now);
            if (sp_hidden != sp_stores_mask_) { ++sp_stores_changes_; sp_stores_mask_ = sp_hidden; }
            if (sp_hidden) sp_stores_sticky_ = true;
        }
        if (s_.cockpit->opt.anim && ckm.anim_vertices) {   // COCKPIT-4: pose the animated range with the current commands (neutral until E6 rates arrive)
            acz::cockpit::pose_anim(ckm, anim_state_.in, anim_pose_);
            acz::cockpit::apply_store_mask(ckm, store_hidden, anim_pose_);
            acz::cockpit::apply_sp_mask(ckm, sp_hidden, anim_pose_);
            job.cockpit_anim = anim_pose_.data(); job.cockpit_anim_count = uint32_t(anim_pose_.size());
        } else if ((stores_sticky_ || sp_stores_sticky_) && ckm.anim_vertices) {   // anim:0: the rest range (what the GPU holds) with the stations hidden
            acz::cockpit::pose_stores_only(ckm, store_hidden, sp_hidden, anim_pose_);
            job.cockpit_anim = anim_pose_.data(); job.cockpit_anim_count = uint32_t(anim_pose_.size());
        }
        if (s_.cockpit->opt.skin_refl) {   // SKIN-REFL: the drawn stage's map (TAKEOFF: the airbase's own while it is drawn), keyed by that runtime's generation
            const TerrainRuntime* rt = s_.terrain ? &drawn_terrain() : nullptr;
            if (rt && refl_basis_ok && rt->stage.has_refl() && rt->stage.refl_w <= acz::cockpit::kReflMaxDim && rt->stage.refl_h <= acz::cockpit::kReflMaxDim) {
                job.cockpit_refl = rt->stage.refl_rgba.data(); job.cockpit_refl_w = rt->stage.refl_w; job.cockpit_refl_h = rt->stage.refl_h;
                job.cockpit_refl_gen = rt->generation;
            } else ++c_.cockpit_refl_nomap;
        }
    }
    if (cache_on) {
        if (cache_look_ok) cache_look_seen_ = true;
        if (cache_epoch_ms_ < 0) cache_epoch_ms_ = g.now_ms();
        job.world_cache = true;
        // Write only a fresh world whose look was measured (or when no look was ever measured: the look drive is off, the window stays at look 0).
        job.world_cache_write = !held && (cache_look_ok || !cache_look_seen_);
        if (!held && !job.world_cache_write) ++c_.world_cache_skipped;
        job.cache_look_yaw = cache_ly; job.cache_look_pitch = cache_lp;
        job.cache_now_s = float(1.0 + double(g.now_ms() - cache_epoch_ms_) * 0.001);
        for (int i = 0; i < 4; ++i) job.cache_lens[i] = lens_used[i];
    }
    job.occlude_window = s_.world_window_occlude && composite && use_depth && !terrain_on; // J-occlusion-fix (not with the terrain: replayed objects must stay visible)
    job.world_filter = s_.eye_res_pct != 0 && composite;   // WORLD-WINDOW FILTER (eyeres > 0 only; eye_res.h)
    if (job.world_filter) ++c_.world_filter_frames;
    job.use_hud = use_hud;
    if (use_hud) job.hud = {f.hud, f.hud_format, f.width, f.height}; // same pixel size as the source frame
    if (use_hud && menu_on_) if (const auto* mo = hud_menu_options()) job.hud_backdrop_bits = acz::hudmenu::backdrop_bits(mo->menu_backdrop);   // HUD-MENU
    if (use_hud) if (const auto* mo = hud_menu_options()) if (mo->radar_bg) job.hud_backdrop_bits |= acz::hudmenu::radar_bg_bits(mo->radar_bg_alpha);   // RADAR-BG
    job.use_markers = use_markers;
    if (use_markers) job.markers = {f.hud_markers, f.hud_format, f.width, f.height};
    job.use_effects = use_effects;
    if (use_effects) job.effects = {f.effects, f.hud_format, f.width, f.height};
    job.effects_coverage = use_effects && f.effects_repr == stereo::effects_repr_coverage_v1; // J-effects-dual: shader mode 4
    // EFFECTS-OUTSIDE (C) fxmem:4: this frame's own per-eye smoke images (fail closed: any missing/aliased resource = no smoke bit, nothing else changes).
    job.use_smoke = s_.effects_layer && composite && !held && use_depth && f.smoke_valid && f.smoke[0] && f.smoke[1] && f.smoke[0] != f.smoke[1] &&
        source_format_ok(f.smoke_format) && f.smoke[0] != f.eye[0] && f.smoke[0] != f.eye[1] && f.smoke[1] != f.eye[0] && f.smoke[1] != f.eye[1] &&
        f.smoke[0] != f.world && f.smoke[1] != f.world && !(f.effects && (f.smoke[0] == f.effects || f.smoke[1] == f.effects));
    if (job.use_smoke) job.smoke = {{{f.smoke[0], f.smoke_format, ew, eh}, {f.smoke[1], f.smoke_format, ew, eh}}};
    if (fresh && f.smoke_valid) ++c_.smoke_seen;
    // EFFECTS-OUTSIDE (F) fxmem:6: this frame's own per-eye sprite images under the same fail-closed rule (also never aliasing the smoke images).
    job.use_sprite = s_.effects_layer && composite && !held && use_depth && f.sprite_valid && f.sprite[0] && f.sprite[1] && f.sprite[0] != f.sprite[1] &&
        source_format_ok(f.sprite_format) && f.sprite[0] != f.eye[0] && f.sprite[0] != f.eye[1] && f.sprite[1] != f.eye[0] && f.sprite[1] != f.eye[1] &&
        f.sprite[0] != f.world && f.sprite[1] != f.world && !(f.effects && (f.sprite[0] == f.effects || f.sprite[1] == f.effects)) &&
        f.sprite[0] != f.smoke[0] && f.sprite[0] != f.smoke[1] && f.sprite[1] != f.smoke[0] && f.sprite[1] != f.smoke[1];
    if (job.use_sprite) job.sprite = {{{f.sprite[0], f.sprite_format, ew, eh}, {f.sprite[1], f.sprite_format, ew, eh}}};
    if (fresh && f.sprite_valid) ++c_.sprite_seen;
    job.index = l_.index;
    job.map = map_;
    job.tonemap = s_.tonemap;
    job.exposure = s_.exposure;
    acz::gpu_copy::step5::hud_late_marker_flag().store(acz::gpu_copy::step5::hud_late_needs_marker(unsigned(s_.tonemap), s_.exposure), std::memory_order_relaxed); // HUD-LATE F6
    job.edge_feather = s_.edge_feather;
    // POV-RENDER pov:3: the comfort vignette / blink of this XR frame's rig, drawn only over a no-cockpit frame of a chase / cinematic route (else 0).
    job.pov_vignette_bits = acz::povr::vignette_for(s_.pov_stage3, pov_route_, pov_rig_, f.no_cockpit);
    // A-only (cutscene / replay around A): the consumer rig follows E6, which is not the camera there; only the producer anchor's blink is drawn.
    if (s_.pov_stage3 && acz::povr::req_a_only(pov_req_)) job.pov_vignette_bits = f.no_cockpit && pov_event_blink_ ? acz::povr::kVigBlinkBit : 0u;
    if (job.pov_vignette_bits) {
        if (job.pov_vignette_bits & acz::povr::kVigBlinkBit) ++pov3_.blink_frames;
        else { ++pov3_.vig_frames; pov3_.vig_max = std::max(pov3_.vig_max, float((job.pov_vignette_bits >> acz::povr::kVigShift) & 255u) / 255.0f); }
    }
    job.world_composite = composite;
    job.world_eye = cparams;
    job.debug_view = s_.debug_view;
    if (s_.png_once && png_done_ && fresh && s_.png_eye_trigger && s_.png_eye_trigger(false)) {   // FLIGHT-DUMP: png-eye.now re-arms png_once
        const bool consumed = s_.png_eye_trigger(true);
        if (consumed) { png_done_ = false; png_attempts_ = 0; ++png_eye_rearms_; }
        if (consumed || !png_eye_refused_logged_)
            emit(true, std::string("xr-stereo;event=png-eye-armed;outcome=") + (consumed ? "armed" : "refused") + ";why=" + (consumed ? "none" : "trigger-not-consumed") +
                ";rearms=" + std::to_string(png_eye_rearms_) + ";trigger=png-eye.now(" + (consumed ? "deleted" : "delete-failed") + ");no-headset-claim");
        png_eye_refused_logged_ = !consumed;
    }
    // POV-RENDER pov:3: one eye dump on the kCineDumpFrame-th no-cockpit frame of each cinematic / A-only visit (no file trigger timing involved).
    if (s_.pov_stage3 && pov_cine_dump_.step(pov_req_, fresh && f.no_cockpit, (job.pov_vignette_bits & acz::povr::kVigBlinkBit) != 0) && s_.png_once && png_done_) {
        png_done_ = false; png_attempts_ = 0; ++png_eye_rearms_;
        emit(true, std::string("xr-stereo;event=png-eye-armed;outcome=armed;why=none;rearms=") + std::to_string(png_eye_rearms_) +
             ";trigger=pov3-cinematic(" + (acz::povr::req_a_only(pov_req_) ? "a-only" : "e6") + ");no-headset-claim");
    }
    // HUD-LATE SUBTITLE SHOT (hud_late.h): the producer requested one eye dump at a subtitle onset; taken once the previous dump is finished.
    if (s_.png_once && png_done_ && fresh && acz::gpu_copy::step5::hud_late_eye_request().exchange(false, std::memory_order_acq_rel)) {
        png_done_ = false; png_attempts_ = 0; ++png_eye_rearms_;
        emit(true, std::string("xr-stereo;event=png-eye-armed;outcome=armed;why=none;rearms=") + std::to_string(png_eye_rearms_) +
             ";trigger=hud-late-onset(subtitle);no-headset-claim");
    }
    // EFFECTS-OUTSIDE (P) ADDITIVE SHOT (fxadd:1, effects_game_clouds.h): the producer requested one eye dump on an additive per-eye frame; taken like the subtitle shot.
    if (s_.png_once && png_done_ && fresh && acz::gpu_copy::step5::effects_additive_eye_request().exchange(false, std::memory_order_acq_rel)) {
        png_done_ = false; png_attempts_ = 0; ++png_eye_rearms_;
        emit(true, std::string("xr-stereo;event=png-eye-armed;outcome=armed;why=none;rearms=") + std::to_string(png_eye_rearms_) +
             ";trigger=fx-additive(first-additive-per-eye-frame);no-headset-claim");
    }
    job.png_capture = s_.png_once && !png_done_ && c_.new_frames >= s_.png_after_new_frames && fresh &&
                      !(job.pov_vignette_bits & acz::povr::kVigBlinkBit);   // pov:3: never dump a blink (black) frame; the next fresh frame is taken
    if (s_.terrain && (s_.terrain->tac.on || s_.terrain->tac.mk360)) {   // TACTICAL (key off: not reached)
        job.tac = tac_frame(f, terrain_on, terrain_eyes, cparams, tac_cam, use_hud, !fresh, use_markers);
        // NBOX: also without an eye overlay (head turned away from every stamp): the copies keep the contacts' cells current, the clears the flat quad single
        if (s_.terrain->tac.mk360 == 3 && tac_.stamps.atlas.ok && (!tac_.stamps.draw.empty() || !tac_.stamps.copy.empty())) job.stamps = &tac_.stamps;
    }
    if (dc_.views_on && composite) {
        // DECOUPLE: re-project the producer's eye images from P0 (their render pose) to P1 (this composite) per eye: rotation only, plus the camera-delta
        // rotation (the objects' camera -> this composite's terrain camera) when the replayed cockpit band is dropped (model cockpit); the window/effects
        // rays always take the camera delta. P1 == P0 and no camera change: flags 0 (the exact previous shader path).
        double Q[3][3], I3[3][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) I3[i][j] = i == j ? 1.0 : 0.0;
        if (dc_cam_ok) camera_delta_rotation(dc_cam_obj.right, dc_cam_obj.up, dc_cam_obj.fwd, dc_cam_t.right, dc_cam_t.up, dc_cam_t.fwd, Q);
        else std::memcpy(Q, I3, sizeof Q);
        const bool qid = rotation_is_identity(Q);
        if (!qid) {
            const double c = std::max(-1.0, std::min(1.0, (Q[0][0] + Q[1][1] + Q[2][2] - 1.0) * 0.5));
            dcs_.rot_max_deg = std::max(dcs_.rot_max_deg, std::acos(c) * 57.29577951308232);
        }
        const bool model_drop = cockpit_on && !s_.cockpit->opt.keep_replay;
        const double (*qs)[3] = model_drop ? Q : I3;
        const bool same_pose = std::memcmp(dc_p0.data(), cviews.data(), sizeof cviews) == 0;
        float leaned[3];
        const float* na = lean_anchor(f, leaned) ? leaned : neutral_;
        const float yaw = (anchor_neutral_ ? neutral_yaw_ : 0.0f) + cache_ly;
        bool any = false, refused = false;
        for (unsigned e = 0; e < 2; ++e) {
            const bool src = !(same_pose && rotation_is_identity(qs));
            const bool fx = !qid;
            WorldComposite p0{};
            if (!(src || fx)) continue;
            if (!world_composite_params(dc_p0[e], na, yaw, lens_used, s_.quad_distance_m, p0, cache_lp) ||
                !reproject_params(p0, qs, Q, src, fx, s_.terrain->pacing.decouple_fade, job.reproject[e])) { refused = true; job.reproject[e] = Reproject{}; continue; }
            any = true;
        }
        if (refused) ++dcs_.reproj_refused;
        if (any) { if (job.reproject[0].flags & kReprojSource) ++dcs_.reproj_source; if (job.reproject[0].flags & kReprojWindow) ++dcs_.reproj_window; }
        else if (!refused) ++dcs_.reproj_identity;
    }
    RecordFacts rf{};
    Rc rc = g.call(Op::d3d_record, [&] { rf = blit_->record(job); return rf.ok ? Rc::ok : Rc::error; });
    if (!succeeded(rc) || !rf.ok) { settle_without_gpu(g); fail("blit record failed"); return false; }
    l_.recorded = true;
    if (job.tac) { if (rf.tac_used) ++tac_.backend_used; else ++tac_.backend_refused; }   // TACTICAL
    if (job.stamps) {   // NBOX: a fresh plan whose copies were not recorded must not leave its contacts pointing at cells holding older pixels
        if (rf.stamp_used) ++tac_.nb_backend_used; else ++tac_.nb_backend_refused;
        ++tac_.nb_refuse[unsigned(acz::mk360::classify_nbox_refusal(rf.stamp_used, job.stamps->atlas.ok, job.tac && !job.tac->empty(),
                                                                    job.stamps->fresh && !job.stamps->copy.empty(), rf.stamp_copied))];
        tac_.nb_copies_recorded += rf.stamp_copies; tac_.nb_clears_recorded += rf.stamp_clears;
        if (job.stamps->fresh && !job.stamps->copy.empty() && !rf.stamp_copied) acz::mk360::stamps_not_copied(tac_.mk, *job.stamps);
    } else if (s_.terrain && s_.terrain->tac.mk360 == 3 && fresh && !tac_.stamps.copy.empty()) acz::mk360::stamps_not_copied(tac_.mk, tac_.stamps);
    if (rf.depth_used && fresh) ++c_.depth_frames;
    if (terrain_on && !rf.terrain_used) ++c_.terrain_failed;   // J-terrain-render: the backend fell back to the game's world image for this recording
    if (s_.flight_stage && job.terrain) s_.flight_stage->recorded(job.terrain_gen, job.props != nullptr, rf.props_ready || rf.props_failed);   // VSEARLY
    if (job.props && !props_ready_logged_ && (rf.props_ready || rf.props_failed)) {   // PERF: when the props worker finished (once)
        props_ready_logged_ = true;
        emit(true, std::string("xr-stereo;event=") + (rf.props_ready ? "props-ready" : "props-init-failed;fallback=terrain-only") + ";init-ms=" +
            std::to_string(rf.props_init_ms) + ";frame=" + u64(info.frame_index) + ";owner-t-ms=" + std::to_string((long long)g.now_ms()) + ";worker=async;no-headset-claim");
    }
    if (s_.cockpit_auto && !auto_first_frame_logged_) {   // AIRCRAFT-AUTO-FAST: what the very first recorded frame showed (evidence for 'no replayed-cockpit frames')
        auto_first_frame_logged_ = true;
        char ft[320];
        std::snprintf(ft, sizeof ft, "xr-stereo;event=cockpit-auto;action=first-frame;frame=%llu;model=%s;model-drawn=%s;replayed-cockpit=%s;assumed=%s",
            (unsigned long long)info.frame_index, s_.cockpit ? s_.cockpit->id.c_str() : "none", cockpit_on && rf.cockpit_used ? "yes" : "no",
            cockpit_on && rf.cockpit_used ? (s_.cockpit->opt.keep_replay ? "kept-under-model(replay:1)" : "dropped") : "shown",
            auto_det_.assumed() ? type_name(auto_det_.current()) : "none");
        emit(true, ft);
    }
    if (cockpit_on) {   // COCKPIT-2: a backend refusal keeps the replayed cockpit (the drop bit is only set with the model drawn); logged once
        if (rf.cockpit_anim_uploaded) ++c_.cockpit_anim_frames;   // COCKPIT-4
        if (rf.cockpit_refl_used) ++c_.cockpit_refl_frames;         // SKIN-REFL
        if (rf.cockpit_refl_uploaded) ++c_.cockpit_refl_uploads;
        if (rf.cockpit_used) ++c_.cockpit_frames;
        else {
            ++c_.cockpit_failed;
            if (!cockpit_fail_logged_) { cockpit_fail_logged_ = true; emit(true, "xr-stereo;event=cockpit-model-disabled;reason=gpu-resources-or-pso;fallback=replayed-cockpit;model=" + s_.cockpit->id); }
        }
    }
    if (cache_on) { // J-world-cache
        if (rf.cache_read) ++c_.world_cache_frames; else ++c_.world_cache_failed;
        if (rf.cache_written) ++c_.world_cache_written;
    }
    if (capture) {
        // The held image is exactly the world of this job when the backend says so; anything else invalidates the hold (it may have been recreated).
        hold_valid_ = rf.world_captured;
        if (rf.world_captured) {
            hold_ms_ = g.now_ms();
            for (int i = 0; i < 4; ++i) hold_lens_[i] = f.lens_tan[i];
            look_of(f, hold_look_[0], hold_look_[1]);
            hold_cam_valid_ = frame_cam_valid_; if (frame_cam_valid_) hold_cam_ = frame_cam_;   // CAM-EPOCH: the held image's own camera snapshot
            hold_cam_mode_ = cam_mode_; hold_cam_fid_ = f.frame_id;
            ++c_.world_captured;
        } else ++c_.world_hold_capture_failed;
    }
    // A HUD the backend could not read is NOT drawn (its acquired image is only cleared
    // to transparent and released with the others); this frame submits no HUD quad.
    const bool hud_drawn = use_hud && rf.hud_used;
    if (use_hud && !hud_drawn) { ++c_.hud_invalid; ++c_.hud_skipped; }
    const bool markers_drawn = use_markers && rf.markers_used; // J-hud-markers: same rule as the HUD
    if (use_markers && !markers_drawn) { ++c_.marker_invalid; ++c_.marker_skipped; }
    if (s_.effects_layer && use_effects && fresh) { if (rf.effects_used) ++c_.effects_frames; else { ++c_.effects_invalid; ++c_.effects_skipped; } } // J-effects-layer
    if (fresh && rf.smoke_used) ++c_.smoke_frames; // EFFECTS-OUTSIDE (C) fxmem:4
    if (fresh && rf.sprite_used) ++c_.sprite_frames; // EFFECTS-OUTSIDE (F) fxmem:6
    if (!rf.sources_restored || !rf.swapchain_untouched) {
        settle_without_gpu(g); fail("resource state restoration not proven; never submitted"); return false;
    }
    rc = g.call(Op::d3d_close, [&] { return blit_->close() ? Rc::ok : Rc::error; });
    if (!succeeded(rc)) { settle_without_gpu(g); fail("private list Close failed"); return false; }
    l_.closed = true;
    if (!startup_budget_ok()) return false; // acquisition/recording may consume budget
    // Producer synchronisation (decided above): CPU-observed completion needs no
    // GPU wait; an authenticated value is waited for on the game's queue.
    if (presynced) { if (fresh) ++c_.cpu_presynced; }   // DECOUPLE re-composite: the producer's value completed before the first composite
    else {
        rc = g.call(Op::d3d_gpu_wait, [&] { return blit_->gpu_wait(f.fence, f.fence_value) ? Rc::ok : Rc::error; });
        if (!succeeded(rc)) { settle_without_gpu(g); fail("queue Wait on producer fence failed"); return false; }
        ++c_.gpu_waits;
    }
    if (!startup_budget_ok()) return false; // queue Wait may also consume budget
    bool dispatch = false; // the backend sets it BEFORE crossing the native boundary
    rc = g.call(Op::d3d_execute, [&] { return blit_->execute(dispatch) ? Rc::ok : Rc::error; });
    l_.submitted = dispatch;
    if (!succeeded(rc) || !dispatch) {
        if (l_.submitted) { l_.ambiguous = true; fail("ExecuteCommandLists ambiguous; quarantine"); return false; }
        settle_without_gpu(g); fail("ExecuteCommandLists refused"); return false;
    }
    const uint64_t value = issued_ + 1;
    rc = g.call(Op::d3d_signal, [&] { return blit_->signal(value) ? Rc::ok : Rc::error; });
    if (!succeeded(rc)) { l_.ambiguous = true; fail("private fence Signal failed/ambiguous; quarantine"); return false; }
    issued_ = value; l_.value = value; l_.signalled = true;
    const int64_t t0 = g.now_ms();
    l_.signal_ms = t0;
    // The blit runs on the game's queue behind its queued frames: the window is a
    // bound on that backlog, not on the blit itself. The first submission gets the
    // longer first-use window; on the owner's first XR frame it is capped so the
    // startup budget keeps a reserve for xrEndFrame. There is no steady-window
    // floor here: Execute/Signal may have consumed time since admission. Pending
    // work still follows the existing quarantine/retirement path, even if late.
    const bool first_submission = !completion_seen_;
    int64_t deadline = t0 + (first_submission ? s_.first_completion_window_ms : s_.completion_window_ms);
    if (startup_frame) deadline = std::min(deadline, completion_boundary);
    const uint64_t samples0 = sample_count_;
    if (!observe_completion(g, deadline, s_.sample_spacing_ms)) {
        if (!l_.ambiguous) {
            // Discriminating facts for a pending completion (step14 run 2): was the
            // PRODUCER's value itself still pending (game-queue backlog ahead of us)
            // or only our own blit? Additive diagnostics; the quarantine is unchanged.
            bool producer_done = false;
            const Rc prc = g.call(Op::d3d_sample, [&] { producer_done = blit_->fence_completed(f.fence, f.fence_value); return Rc::ok; });
            char text[440];
            std::snprintf(text, sizeof text, "xr-stereo;event=completion-pending;frame=%llu;frame_id=%llu;sync=%s;first-submission=%u;"
                "window-ms=%lld;waited-ms=%lld;samples=%llu;private-fence=%llu;private-completed=%llu;producer-fence-value=%llu;"
                "producer-last-signalled=%llu;producer-complete=%s;src=%ux%u;targets=%u;owner-t-ms=%lld",
                (unsigned long long)info.frame_index, (unsigned long long)f.frame_id, presynced ? "cpu-observed" : "queue-wait",
                unsigned(first_submission), (long long)(deadline - t0), (long long)(g.now_ms() - t0),
                (unsigned long long)(sample_count_ - samples0), (unsigned long long)value, (unsigned long long)last_completed_,
                (unsigned long long)f.fence_value, (unsigned long long)f.last_signalled,
                succeeded(prc) ? (producer_done ? "1" : "0") : "unknown", f.width, f.height, 2u + unsigned(use_world && !composite) + unsigned(use_hud) + unsigned(use_markers), (long long)g.now_ms());
            emit(true, text);
        }
        fail(g.stopping() ? "stop while GPU completion pending; quarantine" : "GPU completion pending; quarantine");
        return false;
    }
    completion_seen_ = true;
    const GpuTiming timing = blit_->timing(); // cached by sample after completion; no foreign call
    if (timing.available) { gpu_start_sum_ += timing.start_ms; gpu_blit_sum_ += timing.blit_ms; ++gpu_samples_; }
    const uint64_t took = uint64_t(std::max<int64_t>(0, g.now_ms() - t0));
    c_.completion_ms_max = std::max(c_.completion_ms_max, took);
    c_.completion_ms_sum += took;
    // UI3D stage 3a (u3p:1; ui-3d.md 7; UNTESTED LIVE): in HUD-MENU mode the pause-menu layers are blended INTO the eye images just composited
    // (GPU completion observed above), on the same queue, before their release. true = the HUD quad (which carries the flat menu) is dropped.
    bool pause_drawn = false, pause_called = false;
    if (s_.vscreen && composite && l_.used[0] && l_.used[1]) {
        if (menu_on_) {
            pause_called = true;
            const acz::cockpit::Options* mo = hud_menu_options();
            // F1: the eyes the composite's layer is submitted with (composite_views = layer_from's selection); unknown -> null -> no blend
            acz::brief3d::EyeView pe[2];
            std::array<xr_owner::EyeSample, 2> cv{};
            const bool have_eyes = composite_views(info, cv);
            if (have_eyes)
                for (unsigned e = 0; e < 2; ++e) {
                    for (int k = 0; k < 3; ++k) pe[e].position[k] = double(cv[e].pose.position[k]);
                    for (int k = 0; k < 4; ++k) pe[e].orientation[k] = double(cv[e].pose.orientation[k]);
                    pe[e].fov[0] = double(cv[e].fov.left); pe[e].fov[1] = double(cv[e].fov.right); pe[e].fov[2] = double(cv[e].fov.up); pe[e].fov[3] = double(cv[e].fov.down);
                }
            pause_drawn = s_.vscreen->pause_frame(g, images_[0], images_[1], images_[0].images[l_.index[0] % max_images],
                                                  images_[1].images[l_.index[1] % max_images], ext_[0], ext_[1], info.space_generation,
                                                  mo ? mo->menu_backdrop : 0.6f, have_eyes ? pe : nullptr);
            if (pause_drawn) ++pause_frames_;
        } else s_.vscreen->pause_idle(g);
    }
    if (!release_images(g)) { release_slot(); fail("xrReleaseSwapchainImage failed; not retried"); return false; }
    if (pause_called && s_.vscreen->poisoned()) { release_slot(); fail("ui3d pause blend: completion unprovable; quarantine"); return false; }
    // review 3 (Astra open risk 1): the pre-check in transact() is not enough alone (a re-execution submitted after it may run before our reads).
    // The producer records every write BEFORE submitting it, and this composite's GPU completion was observed above, so "unchanged" now proves it read
    // the published bytes. Otherwise its layer is never submitted, nothing re-submits the swapchain images it wrote (have_last_ false), it is not
    // retained for re-composites, and the slot is released (no consumer GPU work in flight). Re-composites keep their own check (no_new_frame).
    if (fresh) {
        stereo::FrameSource* src = provider_ ? provider_() : nullptr;
        if (!src || !src->images_unchanged(f)) {
            release_slot();
            ++c_.fresh_rewritten_post;
            // NBOX (mk360:3; Codex J-203 finding 2): this frame's atlas copies ran on the REWRITTEN marker image while its plan (cells, ids, offsets)
            // describes the rejected frame: the cells it wrote are forgotten so no later accepted frame samples them, and nothing of it is drawn again.
            if (s_.terrain && s_.terrain->tac.mk360 == 3) acz::mk360::stamps_rejected(tac_.mk, tac_.stamps);
            plan = {};
            have_last_ = false;
            dc_.views_on = false;
            if (ever_new_) ++c_.stale_zero; else ++c_.zero_no_frame;
            return true;
        }
    }
    // DECOUPLE: a world composite of a new game frame composed at P1 stays HELD (by dc_, no longer by the ledger) for the re-composites of the following
    // XR frames; its GPU work has completed (CPU-observed above), so holding it only keeps the producer from reusing it. Key off: released here as before.
    const bool retain = fresh && dc_enabled() && dc_.views_on && composite;
    if (retain) {
        dc_.held = true; dc_.frame = l_.frame; l_.holding = false;
        dc_.fresh_ms = g.now_ms(); dc_.p0 = dc_p0;
        dc_.cam_valid = dc_cam_ok; dc_.cam_obj = dc_cam_obj; dc_.cam_fresh = dc_cam_t;
        dc_.cam_ep_valid = frame_ep_valid_; dc_.cam_lock = frame_ep_lock_; dc_.cam_stage = frame_ep_stage_;   // review 2: the admitted camera's identity
        dc_.space_gen = info.space_generation;   // review 2: P0's LOCAL generation (posed or not)
        ++dcs_.retained;
    } else if (fresh && dc_enabled()) ++dcs_.not_retained;
    release_slot(); // GPU completion observed: the producer may reuse the slot (retained: l_.holding is false, nothing is released here)
    if (job.png_capture) { // J-test8-fixes xr_png_once: the copies completed with the frame; take them once (bounded: 3 attempts, then given up)
        ++png_attempts_;
        PngCapture pc;
        bool taken = false;
        if (rf.png_recorded) (void)g.call(Op::d3d_sample, [&] { taken = blit_->png_take(pc); return Rc::ok; });
        unsigned ok = 0, layers = 0;
        if (taken && s_.png_sink) {
            const std::string base = "acz-vr-xr-eye0-f" + std::to_string((unsigned long long)f.frame_id);
            if (s_.png_sink(base + "-final.png", pc.final_rgba, pc.w, pc.h)) ++ok;
            if (s_.png_sink(base + "-alpha.png", pc.alpha_rgba, pc.w, pc.h)) ++ok;
            if (s_.png_sink(base + "-worldfx.png", pc.worldfx_rgba, pc.w, pc.h)) ++ok;
            // LAYER-DUMP: the HUD layer and marker layer images of the same frame (the compositor's quads; runLOCK2: the eye images hold neither)
            if (!pc.hud_rgba.empty() && s_.png_sink(base + "-hudlayer.png", pc.hud_rgba, pc.lw, pc.lh)) ++layers;
            if (!pc.markers_rgba.empty() && s_.png_sink(base + "-markerlayer.png", pc.markers_rgba, pc.lw, pc.lh)) ++layers;
        }
        if (taken || png_attempts_ >= 3) png_done_ = true;
        emit(true, "xr-stereo;event=png-once;outcome=" + std::string(taken ? "taken" : "failed") + ";frame_id=" + u64(f.frame_id) + ";attempt=" + u64(png_attempts_) +
            ";recorded=" + u64(rf.png_recorded ? 1 : 0) + ";size=" + std::to_string(pc.w) + "x" + std::to_string(pc.h) + ";files=" + std::to_string(ok) + "/3;depth-used=" +
            u64(rf.depth_used ? 1 : 0) + ";composite=" + u64(composite ? 1 : 0) + ";effects-used=" + u64(rf.effects_used ? 1 : 0) +
            ";layers=" + std::to_string(layers) + "(" + std::to_string(pc.lw) + "x" + std::to_string(pc.lh) + ");no-headset-claim");
    }
    if (!first_frame_logged_) {
        char text[440];
        std::snprintf(text, sizeof text, "xr-stereo;event=first-frame;frame=%llu;frame_id=%llu;slot=%u;src=%ux%u;fmt=%u;members=%u;partial=%u;"
            "producer-fence-value=%llu;sync=%s;private-fence=%llu;images=%u,%u,%s%u;barriers=%u+%u;draws=%u;clears=%u;pose=%s;"
            "depth=%s;depth-fmt=%u;world=%s;lens=%s;completion-ms=%llu",
            (unsigned long long)info.frame_index, (unsigned long long)f.frame_id, f.slot, f.width, f.height, f.dxgi_format, f.members,
            unsigned(f.partial), (unsigned long long)f.fence_value, presynced ? "cpu-observed" : "queue-wait",
            (unsigned long long)value, l_.index[0], l_.index[1], (use_world && !composite) ? "" : "none,", l_.index[kQuad], rf.barriers_out, rf.barriers_back,
            rf.draws, rf.clears, f.pose_valid ? "frame" : (info.views_valid ? "located" : "none"), rf.depth_used ? "coverage" : (use_depth ? "unreadable-opaque" : "absent-opaque"),
            f.depth_format, composite ? "projection" : use_world ? "quad" : "none", (f.lens_valid && lens_ok(f.lens_tan)) ? "frame" : "none", (unsigned long long)took);
        emit(true, text);
    }
    if (hud_drawn) {
        ++c_.hud_frames;
        if (!first_hud_logged_) {
            first_hud_logged_ = true;
            char text[440];
            std::snprintf(text, sizeof text, "xr-stereo;event=first-hud-frame;frame=%llu;frame_id=%llu;slot=%u;hud-fmt=%u;src=%ux%u;hud-image=%u;"
                "producer-hud-draws=%u;producer-hud-refused=%u;hud-mode=%s;world=%s;draws=%u;barriers=%u+%u;completion-ms=%llu",
                (unsigned long long)info.frame_index, (unsigned long long)f.frame_id, f.slot, f.hud_format, f.width, f.height, l_.index[kHud],
                f.hud_draws, f.hud_refused, hud_mode_name(s_.hud_mode), composite ? "projection" : use_world ? "quad" : "none", rf.draws, rf.barriers_out, rf.barriers_back,
                (unsigned long long)took);
            emit(true, text);
        }
    }
    if (markers_drawn) {
        ++c_.marker_frames;
        if (!first_marker_logged_) {
            first_marker_logged_ = true;
            char text[440];
            std::snprintf(text, sizeof text, "xr-stereo;event=first-marker-frame;frame=%llu;frame_id=%llu;slot=%u;marker-fmt=%u;src=%ux%u;marker-image=%u;"
                "producer-marker-draws=%u;producer-marker-refused=%u;world=%s;hud=%s;draws=%u;barriers=%u+%u;completion-ms=%llu",
                (unsigned long long)info.frame_index, (unsigned long long)f.frame_id, f.slot, f.hud_format, f.width, f.height, l_.index[kMarkers],
                f.hud_marker_draws, f.hud_marker_refused, composite ? "projection" : use_world ? "quad" : "none", hud_drawn ? "layer" : "none", rf.draws, rf.barriers_out, rf.barriers_back,
                (unsigned long long)took);
            emit(true, text);
        }
    }
    // DECOUPLE re-composite: the HUD / marker quads of the first composite (same frame, images still in their swapchains) are submitted again.
    const bool plan_hud = !pause_drawn && (fresh ? hud_drawn : dc_.plan_hud), plan_markers = fresh ? markers_drawn : dc_.plan_markers;   // UI3D 3a
    const bool layer_ok = layer_from(g, info, plan, rf.depth_used, use_world && !composite, plan_hud, composite, plan_markers, fresh);
    if (retain) { dc_.plan_hud = hud_drawn; dc_.plan_markers = markers_drawn; }
    if (s_.select_corr_log && layer_ok) corr_emit(g, info, plan, fresh, fresh ? (use_world ? (held ? 2 : 1) : 0) : 0);
    if (dc_enabled() && layer_ok) {
        ++dcs_.composites; ++dc_.window_composites;
        if (fresh) { ++dcs_.fresh; ++dc_.window_fresh; } else ++dcs_.recomposed;
    }
    dc_.views_on = false;
    return true;
}

// ---------------------------------------------- timing-anchor diagnostics ---
// One capped, correlated record per selected frame (select_corr_log). Everything printed is a COPY of a published fact or of the layer
// plan; nothing here feeds back into placement, pose, FOV or composite decisions. Existing lines are untouched; with the setting OFF
// this function and the capture in transact() never run.
int64_t Output::mono_now_us() const {
    if (s_.mono_us) return s_.mono_us();
    return int64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool Output::corr_due(Gate& g) {
    if (!s_.select_corr_log || corr_count_ >= kSelCorrCap) return false;
    return corr_count_ < kSelCorrFirst || g.now_ms() - corr_last_ms_ >= kSelCorrPeriodMs;
}
void Output::corr_emit(Gate& g, const xr_owner::FrameInfo& info, const xr_owner::LayerPlan& plan, bool fresh, int world_state) {
    if (fresh) { corr_ = corr_cand_; corr_.world = world_state; } // the facts of the frame actually SUBMITTED (a refused candidate never reaches here)
    if (!corr_.valid || !corr_due(g)) return;
    ++corr_count_; corr_last_ms_ = g.now_ms();
    const int64_t now_us = mono_now_us();
    const auto& v = plan.layer.views[0];
    const double qx = v.pose.orientation[0], qy = v.pose.orientation[1], qz = v.pose.orientation[2], qw = v.pose.orientation[3];
    // forward = q * (0,0,-1): yaw = atan2(-fx, -fz) about +Y (same convention as the producer's yaw0), pitch = asin(fy).
    const double fx = -2.0 * (qx * qz + qw * qy), fy = -2.0 * (qy * qz - qw * qx), fz = -(1.0 - 2.0 * (qx * qx + qy * qy));
    const double yaw = std::atan2(-fx, -fz), pitch = std::asin(std::max(-1.0, std::min(1.0, fy)));
    const double r2d = 57.29577951308232;
    auto ms = [](int64_t a, int64_t b) { return (a > 0 && b > 0) ? double(b - a) / 1000.0 : -1.0; }; // -1 = unavailable
    const char* start = corr_.start == 1 ? "cockpit" : corr_.start == 2 ? "early-adopted" : corr_.start == 3 ? "early-unadopted" : "unknown";
    const char* look = !corr_.look_valid ? "none" : corr_.look_src == 1 ? "measured" : corr_.look_src == 2 ? "estimated" : "unknown";
    char meas[40];
    if (corr_.look_meas_valid) std::snprintf(meas, sizeof meas, "%.2f,%.2f", corr_.look_meas[0] * r2d, corr_.look_meas[1] * r2d);
    else std::snprintf(meas, sizeof meas, "none");
    const double lead_ms = (corr_.pose_predicted && corr_.xr_predicted > 0) ? (double(corr_.xr_predicted) - double(corr_.pose_predicted)) / 1e6 : -1e9;
    char text[480]; // = feature_log::line_cap
    std::snprintf(text, sizeof text,
        "xr-stereo;event=select-corr;n=%u;layer=%s;world=%s;xr-frame=%llu;xr-pred-ns=%lld;id=%llu;gen=%llu;pose-seq=%llu;pose-pred-ns=%llu;"
        "pose-lead-ms=%.2f;start=%s;members=%u;partial=%u;refusal=%s;look=%s;look-deg=%.2f,%.2f;look-meas-deg=%s;look-est-deg=%.2f,%.2f;"
        "sub-pos=%.3f,%.3f,%.3f;sub-yaw-pitch-deg=%.2f,%.2f;sub-fov-deg=%.1f,%.1f,%.1f,%.1f;sel-us=%lld;prod-ms=%.2f;pub-to-sel-ms=%.2f;"
        "age-ms=%.2f;no-headset-claim",
        corr_count_, fresh ? "fresh" : "reused", corr_.world == 1 ? "fresh" : corr_.world == 2 ? "held" : "none",
        (unsigned long long)info.frame_index, (long long)info.timing.predicted_time, (unsigned long long)corr_.frame_id,
        (unsigned long long)corr_.generation, (unsigned long long)corr_.pose_seq, (unsigned long long)corr_.pose_predicted,
        lead_ms, start, corr_.members, unsigned(corr_.partial), corr_.refusal, look, corr_.look[0] * r2d, corr_.look[1] * r2d, meas,
        corr_.look_est[0] * r2d, corr_.look_est[1] * r2d,
        double(v.pose.position[0]), double(v.pose.position[1]), double(v.pose.position[2]), yaw * r2d, pitch * r2d,
        deg(v.fov.left), deg(v.fov.right), deg(v.fov.up), deg(v.fov.down),
        (long long)corr_.t_select_us, ms(corr_.t_start_us, corr_.t_publish_us), ms(corr_.t_publish_us, corr_.t_select_us),
        double(now_us - corr_.t_select_us) / 1000.0);
    emit(false, text);
}

void Output::frame(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
    plan = {};
    plan_hud_ = false;
    plan_markers_ = false;
    dc_.views_on = false;   // DECOUPLE: set per composite only
    if (dc_enabled() && !dc_.logged) {
        dc_.logged = true; dc_.last_summary_ms = g.now_ms();
        emit(true, "xr-stereo;event=decouple-config;" + acz::pacing::describe(s_.terrain->pacing) +
                   ";compose=every-xr-frame(own-predicted-head-pose);objects-effects=newest-game-frame-reprojected(rotation-only+camera-delta);"
                   "terrain-camera=dead-reckoned-to-display-time;hud-markers=own-quad-layers(game-rate-images);fallback=resubmit-last-images-after-dcage");
    }
    ++c_.frames;
    if (info.views_valid) { // J-cockpit-fov-consumer: the headset's own FOV, diagnostics only
        hmd_fov_valid_ = true;
        hmd_fov_ = {info.views.eyes[0].fov, info.views.eyes[1].fov};
    }
    if (info.timing.predicted_period > 0) {
        const double period = double(info.timing.predicted_period) / 1e6;
        period_sum_ += period; period_max_ = std::max(period_max_, period); ++period_samples_;
    }
    if (c_.frames == 1) { first_wait_ms_ = info.wait_ms; first_begin_ms_ = info.begin_ms; first_t0_ = g.now_ms(); }
    c_.wait_frame_ms_max = std::max<uint64_t>(c_.wait_frame_ms_max, uint64_t(std::max<int64_t>(0, info.wait_ms)));
    if (failed()) return;
    if (!setup_attempted_ || destroyed_) { fail("frame without setup"); return; }
    if (s_.pov && s_.flight_stage && s_.world_in_projection) {   // VIEW-MODE (view_mode.h): HUD-only/chase keep the flight stage alive
        // POV-RENDER stage 2: the cockpit evidence is the age of the last frame WITH cockpit members (identical to the last fresh frame without pov:2).
        const auto pv = pov_.step(g.now_ms(), ever_cockpit_ ? std::max<int64_t>(0, g.now_ms() - last_cockpit_ms_) : -1, acz::viewmode::view_mailbox());
        s_.flight_stage->view_alive(pv.alive_ms);
        if (pv.line) emit(pv.important, pv.text);
        if (s_.pov_render.active() || s_.pov_stage2) {   // POV-RENDER stage 1 (route + rig dry run, census; logs only) and stage 2 (the producer request)
            const auto pr = pov_render_.step(g.now_ms(), pv, s_.pov_render);
            for (unsigned i = 0; i < pr.lines; ++i) emit(false, pr.text[i]);
            if (pr.acm_request) acz::povr::acm_request().fetch_add(1, std::memory_order_relaxed);   // the probe's next 5 s window arms the ACM census
            pov_route_ = pr.route.r; pov_rig_ = pr.rig;   // POV-RENDER pov:3: the vignette of the frame composed below (transact)
            if (s_.pov_stage2) {
                // pov:2 = chase only (unchanged); pov:3 = chase + HUD-only + cinematic, through the NcGate (no frames -> flat for 1 s, no flicker).
                const uint32_t want = s_.pov_stage3 ? acz::povr::request_for_stage3(pr.route.r, s_.pov_render) : acz::povr::request_for(true, pr.route.r, s_.pov_render);
                // A-only escalation: a view whose openers keep being refused because A is not the E6 camera (cutscene, replay) is rendered around A.
                const uint32_t req = s_.pov_stage3 ? pov_gate_.step(g.now_ms(), want, pov_last_nc_ms_,
                                                                    acz::povr::producer_e6_mismatch().load(std::memory_order_relaxed), s_.pov_render) : want;
                const uint32_t was = acz::povr::producer_request().exchange(req, std::memory_order_relaxed);
                pov_req_ = req;
                pov_event_blink_ = pov_event_blink_rig_.step(g.now_ms(), acz::povr::producer_anchor_events().load(std::memory_order_relaxed));
                if (acz::povr::req_chase(req)) ++pov3_.req[unsigned(acz::povr::req_kind(req))];
                if (acz::povr::req_a_only(req)) ++pov3_.a_only;
                const bool kind_changed = acz::povr::req_chase(req) && acz::povr::req_chase(was) &&
                    (acz::povr::req_kind(req) != acz::povr::req_kind(was) || acz::povr::req_a_only(req) != acz::povr::req_a_only(was));
                if (acz::povr::req_chase(req) != acz::povr::req_chase(was) || kind_changed) {
                    char t[400];
                    const bool fell_back = s_.pov_stage3 && want && !req;
                    std::snprintf(t, sizeof t, "xr-stereo;event=pov-stage2;request=%s;route=%s;why=%s;rig=%s;tc-ms=%d;no-cockpit-frames=%llu%s",
                        acz::povr::req_chase(req) ? acz::povr::nc_kind_name(acz::povr::req_kind(req)) : "off", acz::povr::route_name(pr.route.r),
                        fell_back ? (pov_gate_.last_fallback_after_frames() ? "pov3-no-frames-gap(flat-1s)" : "pov3-no-first-frame(flat-1s)") : pr.route.why,
                        !acz::povr::req_chase(req) ? "-" : acz::povr::req_a_only(req) ? (acz::povr::req_level(req) ? "anchor(a-only)" : "game(a-only)")
                            : acz::povr::req_kind(req) == acz::povr::NcKind::hud ? "game(eye)"
                            : acz::povr::req_level(req) ? (acz::povr::req_kind(req) == acz::povr::NcKind::cinematic ? "anchor" : "level") : "game",
                        acz::povr::req_tc_ms(req), static_cast<unsigned long long>(pov_nc_frames_),
                        s_.pov_stage3 ? (";gate-fallbacks=" + std::to_string(pov_gate_.fallbacks()) + ";escalations=" + std::to_string(pov_gate_.escalations()) +
                                         ";e6-mismatch=" + std::to_string(acz::povr::producer_e6_mismatch().load(std::memory_order_relaxed))).c_str() : "");
                    emit(true, t);
                }
                if (s_.pov_stage3 && (pov3_.last_ms < 0 || g.now_ms() - pov3_.last_ms >= 10000)) {   // pov:3 summary every 10 s
                    pov3_.last_ms = g.now_ms();
                    char t[360];
                    using ull = unsigned long long;
                    std::snprintf(t, sizeof t, "xr-stereo;event=summary-pov-stage3;request=%s;route=%s;req-frames=chase:%llu,hud-only:%llu,cinematic:%llu;"
                        "no-cockpit-frames=%llu;gate-fallbacks=%llu;vig-frames=%llu;blink-frames=%llu;vig-max=%.2f;povv=%.2f;escalations=%llu;a-only-frames=%llu;"
                        "cine-dumps=%u",
                        acz::povr::req_chase(req) ? acz::povr::nc_kind_name(acz::povr::req_kind(req)) : "off", acz::povr::route_name(pr.route.r),
                        ull(pov3_.req[0]), ull(pov3_.req[1]), ull(pov3_.req[2]), ull(pov_nc_frames_), ull(pov_gate_.fallbacks()),
                        ull(pov3_.vig_frames), ull(pov3_.blink_frames), double(pov3_.vig_max), double(s_.pov_render.vignette),
                        ull(pov_gate_.escalations()), ull(pov3_.a_only), pov_cine_dump_.dumps());
                    emit(false, t);
                }
            }
        }
    }
    if (s_.flight_stage && s_.world_in_projection && s_.flight_stage->frame(g.now_ms(), ever_new_ ? last_new_ms_ : -1, s_.terrain))
        takeoff_ = acz::terrain::TakeoffSelector{};   // VSEARLY: a new flight's stage (or none): the takeoff rule re-armed for it
    if (!info.timing.should_render) ++c_.should_render_false; // zero layers; nothing acquired
    else if (info.discarded) ++c_.discarded;
    else transact(g, info, plan);
    if (failed()) plan = {};
    plan_hud_ = plan.submit && plan.layer.hud;
    plan_markers_ = plan.submit && plan.layer.markers;
    summary(g, false);
}

void Output::session_event(Gate& g, const xr_owner::Event& e) {
    emit(true, "xr-stereo;event=session-state;state=" + std::to_string(unsigned(e.state)) +
        ";xr-time-ns=" + std::to_string(e.event_time) + ";owner-t-ms=" + std::to_string(g.now_ms()) +
        ";same-session=" + (e.same_session ? "1" : "0") + ";name=" + xr_owner::session_state_name(e.state)); // XR-START: name appended
}
void Output::end_diagnostics(int64_t cpu_ms, bool available, double late_ms) {
    frame_cpu_max_ = std::max(frame_cpu_max_, cpu_ms);
    if (available && std::isfinite(late_ms)) { end_late_max_ = late_samples_ ? std::max(end_late_max_, late_ms) : late_ms; ++late_samples_; }
}
void Output::ended(Gate& g, Rc rc, bool projection, int64_t end_ms) {
    c_.end_frame_ms_max = std::max<uint64_t>(c_.end_frame_ms_max, uint64_t(std::max<int64_t>(0, end_ms)));
    if (c_.frames == 1 && !first_xr_frame_logged_) {
        first_xr_frame_logged_ = true;
        char text[256];
        std::snprintf(text, sizeof text, "xr-stereo;event=first-xr-frame;wait-ms=%lld;begin-ms=%lld;end-ms=%lld;end-rc=%s;layer=%u;"
            "owner-t-ms=%lld;budget=startup5000;progress250=from-frame-2",
            (long long)first_wait_ms_, (long long)first_begin_ms_, (long long)end_ms, succeeded(rc) ? "ok" : "failed",
            unsigned(projection), (long long)g.now_ms());
        emit(true, text);
    }
    if (!succeeded(rc)) { ++c_.end_failed; return; }
    if (!projection) return;
    ++c_.submitted;
    if (!first_frame_logged_) {
        first_frame_logged_ = true;
        const auto& l = last_plan_.layer;
        const auto& v = l.views;
        char text[440];
        std::snprintf(text, sizeof text, "xr-stereo;event=first-layer;end-ms=%lld;layers=%u;order=%s;proj-flags=%s;eye0-pos=%.4f,%.4f,%.4f;"
            "eye1-pos=%.4f,%.4f,%.4f;fov=%.4f,%.4f,%.4f,%.4f;quad-pos=%.3f,%.3f,%.3f;quad-size=%.3fx%.3f;rect=full-image;space=LOCAL",
            (long long)end_ms, 1u + unsigned(l.quad) + unsigned(l.markers), layer_order(l, false),
            l.source_alpha ? "BLEND_TEXTURE_SOURCE_ALPHA(premultiplied)" : "none(opaque)",
            v[0].pose.position[0], v[0].pose.position[1], v[0].pose.position[2], v[1].pose.position[0], v[1].pose.position[1], v[1].pose.position[2],
            v[0].fov.left, v[0].fov.right, v[0].fov.up, v[0].fov.down, l.quad_pose.position[0], l.quad_pose.position[1], l.quad_pose.position[2],
            l.quad_width, l.quad_height);
        std::string line(text);
        if (s_.comfort_log) {
            // J-feather: only when a J-feather key is configured (key-off line unchanged).
            char extra[200];
            std::snprintf(extra, sizeof extra, ";edge-feather=%.3f;world-quad-distance-m=%.1f;quad-flags=%s",
                double(s_.edge_feather), double(s_.quad_distance_m),
                !l.quad ? "no-world-quad-this-frame" : l.quad_source_alpha ? "BLEND_TEXTURE_SOURCE_ALPHA(premultiplied)" : "none(opaque)");
            line += extra;
        }
        // J-cockpit-fov-consumer (additive, trailing): the source of the FOV this layer submitted
        // (hmd = the frame's cockpit_fov, lens, pose). The per-eye FOVs go to a companion line:
        // with the J-feather tokens the first-layer line would exceed the router's 480-byte cap.
        const std::string source = std::string(";cockpit-fov=") + cockpit_fov_name(cockpit_fov_source_);
        line += source;
        emit(true, line);
        char fov[160];
        std::snprintf(fov, sizeof fov, ";layer-eye0-fov=%.2f,%.2f,%.2f,%.2f;layer-eye1-fov=%.2f,%.2f,%.2f,%.2f",
            deg(v[0].fov.left), deg(v[0].fov.right), deg(v[0].fov.up), deg(v[0].fov.down),
            deg(v[1].fov.left), deg(v[1].fov.right), deg(v[1].fov.up), deg(v[1].fov.down));
        emit(true, "xr-stereo;event=first-layer-fov" + eye_fov_tokens() + source + fov);
        emit(true, layer_fov_line(last_plan_)); // J-layer-fov
        // J-quad-yaw: where the LOCAL quads of this layer are anchored (companion line: the
        // first-layer line is near the router's 480-byte cap).
        if (l.quad || l.markers || l.world_composite || (l.hud && !l.hud_view_space)) emit(true, "xr-stereo;event=quad-anchor;for=first-layer" + quad_anchor_tokens());
        if (l.world_composite) {
            // J-world-in-projection: what this layer list contains (the quad layer is intentionally absent).
            char wc[300];
            std::snprintf(wc, sizeof wc, "xr-stereo;event=world-composite;for=first-layer;world-composite=projection;layers=%u;order=%s;"
                "proj-flags=%s;plane-distance-m=%.1f;quad-layer=none",
                1u + unsigned(l.hud) + unsigned(l.markers), layer_order(l, l.hud),
                l.source_alpha ? "BLEND_TEXTURE_SOURCE_ALPHA(premultiplied)" : "none(opaque)",
                double(s_.quad_distance_m));
            emit(true, wc);
        }
    }
    if (plan_markers_) {
        ++c_.marker_layers;
        if (!first_marker_layer_logged_) {
            first_marker_layer_logged_ = true;
            const auto& l = last_plan_.layer;
            // Same pose/size as the world quad when one is submitted; with xr_world_in_projection there is no quad layer and the
            // marker quad is an overlay at the world PLANE the in-projection world uses (same anchor, yaw, distance, lens).
            const bool same = l.quad && l.marker_width == l.quad_width && l.marker_height == l.quad_height &&
                std::equal(std::begin(l.marker_pose.position), std::end(l.marker_pose.position), std::begin(l.quad_pose.position)) &&
                std::equal(std::begin(l.marker_pose.orientation), std::end(l.marker_pose.orientation), std::begin(l.quad_pose.orientation));
            // Distance ahead of the anchor and the angles the quad subtends from it (in the frame turned by the neutral yaw).
            double cx = double(l.marker_pose.position[0]) - neutral_[0], cy = double(l.marker_pose.position[1]) - neutral_[1];
            double dist = double(neutral_[2]) - double(l.marker_pose.position[2]);
            if (anchor_neutral_) {
                const double c = std::cos(double(neutral_yaw_)), s = std::sin(double(neutral_yaw_));
                const double rx = double(l.marker_pose.position[0]) - neutral_[0], rz = double(l.marker_pose.position[2]) - neutral_[2];
                cx = c * rx - s * rz;
                dist = -(s * rx + c * rz);
            }
            auto span = [dist](double c, double half) {
                return dist > 0 ? deg(float(std::atan((c + half) / dist) - std::atan((c - half) / dist))) : 0.0;
            };
            char text[440];
            std::snprintf(text, sizeof text, "xr-stereo;event=first-marker-layer;end-ms=%lld;layers=%u;order=%s;marker-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);"
                "marker-space=LOCAL;marker-pos=%.3f,%.3f,%.3f;marker-size=%.3fx%.3f;marker-distance-m=%.3f;marker-angular-deg=%.2fx%.2f;marker-scale=1;"
                "marker-pose-equals-world-quad=%s",
                (long long)end_ms, 2u + unsigned(l.quad) + unsigned(l.hud), layer_order(l, l.hud),
                l.marker_pose.position[0], l.marker_pose.position[1], l.marker_pose.position[2], l.marker_width, l.marker_height, dist,
                span(cx, l.marker_width / 2.0), span(cy, l.marker_height / 2.0),
                l.quad ? (same ? "1" : "0") : l.world_composite ? "world-composite-plane" : "no-world-quad-this-frame");
            emit(true, text);
        }
    }
    if (plan_hud_) {
        ++c_.hud_layers;
        if (!first_hud_layer_logged_) {
            first_hud_layer_logged_ = true;
            const auto& l = last_plan_.layer;
            const bool same = l.quad && !l.hud_view_space && l.hud_width == l.quad_width && l.hud_height == l.quad_height &&
                std::equal(std::begin(l.hud_pose.position), std::end(l.hud_pose.position), std::begin(l.quad_pose.position)) &&
                std::equal(std::begin(l.hud_pose.orientation), std::end(l.hud_pose.orientation), std::begin(l.quad_pose.orientation));
            char text[440];
            std::snprintf(text, sizeof text, "xr-stereo;event=first-hud-layer;end-ms=%lld;layers=%u;order=%s;hud-flags=BLEND_TEXTURE_SOURCE_ALPHA(premultiplied);"
                "hud-mode=%s;hud-space=%s;hud-pos=%.3f,%.3f,%.3f;hud-size=%.3fx%.3f;hud-pose-equals-world-quad=%s",
                (long long)end_ms, 2u + unsigned(l.quad) + unsigned(l.markers), layer_order(l, true),
                hud_mode_name(s_.hud_mode), l.hud_view_space ? "VIEW" : "LOCAL",
                l.hud_pose.position[0], l.hud_pose.position[1], l.hud_pose.position[2], l.hud_width, l.hud_height,
                l.quad ? (same ? "1" : "0") : "no-world-quad-this-frame");
            std::string line(text);
            if (s_.hud_placement_log) {
                // J-hud-placement (only with a placement key configured): the actual HUD distance
                // ahead of the anchor (VIEW: of the head) and the angles it subtends from there.
                const float ax = l.hud_view_space ? 0.0f : neutral_[0], ay = l.hud_view_space ? 0.0f : neutral_[1];
                double dist = double(l.hud_view_space ? 0.0f : neutral_[2]) - double(l.hud_pose.position[2]);
                double cx = double(l.hud_pose.position[0]) - ax, cy = double(l.hud_pose.position[1]) - ay;
                if (!l.hud_view_space && anchor_neutral_) {
                    // J-quad-yaw: measured in the anchor frame turned by yaw0 (local = Ry(-yaw0) (hud - anchor)).
                    const double c = std::cos(double(neutral_yaw_)), s = std::sin(double(neutral_yaw_));
                    const double rx = double(l.hud_pose.position[0]) - neutral_[0], rz = double(l.hud_pose.position[2]) - neutral_[2];
                    cx = c * rx - s * rz;
                    dist = -(s * rx + c * rz);
                }
                auto span = [dist](double c, double half) {
                    return dist > 0 ? deg(float(std::atan((c + half) / dist) - std::atan((c - half) / dist))) : 0.0;
                };
                char extra[200];
                std::snprintf(extra, sizeof extra, ";hud-distance-m=%.3f;hud-angular-deg=%.2fx%.2f", dist,
                    span(cx, l.hud_width / 2.0), span(cy, l.hud_height / 2.0));
                line += extra + hud_placement_tokens(s_);
            }
            emit(true, line);
            if (!l.hud_view_space) emit(true, "xr-stereo;event=quad-anchor;for=first-hud-layer" + quad_anchor_tokens());
        }
    }
    if (!milestone_logged_ && s_.milestone_frames && c_.submitted >= s_.milestone_frames) {
        milestone_logged_ = true;
        emit(true, "xr-stereo;event=milestone;" + result_line(nullptr));
    }
}

void Output::summary(Gate& g, bool force) {
    const int64_t now = g.now_ms();
    if (!force && now - last_summary_ms_ < s_.summary_ms) return;
    const int64_t window_ms = now - last_summary_ms_;
    last_summary_ms_ = now;
    const auto& p = last_summary_;
    const uint64_t done = c_.acquired_frames - p.acquired_frames;
    auto d = [&](uint64_t Counters::* m) { return (unsigned long long)(c_.*m - p.*m); };
    // Counts are XR SUBMISSIONS: "submitted" = layer accepted by xrEndFrame. Nothing
    // here claims delivery, presentation or visibility on a headset.
    char text[440];
    std::snprintf(text, sizeof text, "xr-stereo;event=summary;t-ms=%lld;frames=%llu;submitted=%llu;new=%llu;reused=%llu;"
        "reuse-age-max-ms=%llu;stale-zero=%llu;stale-age=%llu;stale-tracking=%llu;stale-space=%llu;deferred=%llu;"
        "zero-no-frame=%llu;last-id=%llu;no-headset-claim",
        (long long)now, d(&Counters::frames), d(&Counters::submitted), d(&Counters::new_frames), d(&Counters::reused),
        (unsigned long long)c_.reuse_age_max_ms, d(&Counters::stale_zero), d(&Counters::stale_age), d(&Counters::stale_tracking),
        d(&Counters::stale_space), d(&Counters::deferred), d(&Counters::zero_no_frame), (unsigned long long)c_.last_frame_id);
    emit(false, text);
    std::snprintf(text, sizeof text, "xr-stereo;event=summary-admission;t-ms=%lld;fresh-stale-space=%llu;fresh-stale-tracking=%llu;"
        "space-unknown=%llu;world-lens-invalid=%llu;anchor-rebases=%llu;zero-no-pose=%llu;should-render-false=%llu;gaps=%llu;"
        "depth=%llu;world=%llu;lens=%llu",
        (long long)now, d(&Counters::fresh_stale_space), d(&Counters::fresh_stale_tracking), d(&Counters::space_unknown),
        d(&Counters::world_lens_invalid), d(&Counters::anchor_rebases), d(&Counters::zero_no_pose), d(&Counters::should_render_false),
        d(&Counters::frame_id_gaps), d(&Counters::depth_frames), d(&Counters::world_frames), d(&Counters::lens_frames));
    emit(false, text);
    if (s_.terrain) {   // J-terrain-render (key on only)
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-terrain;t-ms=%lld;terrain-frames=%llu;no-camera=%llu;backend-failed=%llu;total-frames=%llu;total-no-camera=%llu;total-failed=%llu",
            (long long)now, d(&Counters::terrain_frames), d(&Counters::terrain_no_camera), d(&Counters::terrain_failed),
            (unsigned long long)c_.terrain_frames, (unsigned long long)c_.terrain_no_camera, (unsigned long long)c_.terrain_failed);
        emit(false, text);
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-terrain-camera;t-ms=%lld;orient-game=%llu;orient-e6=%llu;game-refused=%llu;eye-game=%llu;eye-e6=%llu;e6-frame-sample=%llu;"
            "game-vs-e6-max-deg=%.2f;occlude-frames=%llu;e6-implausible=%llu",
            (long long)now, d(&Counters::terrain_src_game), d(&Counters::terrain_src_e6), d(&Counters::terrain_game_refused), d(&Counters::terrain_eye_game),
            d(&Counters::terrain_eye_e6), d(&Counters::terrain_e6_frame), double(c_.terrain_e6_angle_max_cdeg) / 100.0, d(&Counters::terrain_occlude_frames), d(&Counters::terrain_e6_implausible));
        emit(false, text);
        {   // LENS-FLARE: one line per window; frames = terrain composites seen, the rest = the per-frame reason (active = some eye shows the flare term, pre-occlusion)
            namespace LF = acz::lensflare;
            auto w = [&](LF::Why x) { return (unsigned long long)(c_.lf_why[unsigned(x)] - p.lf_why[unsigned(x)]); };
            std::snprintf(text, sizeof text, "xr-stereo;event=summary-lensflare;t-ms=%lld;frames=%llu;active=%llu;off=%llu;no-env=%llu;no-light=%llu;moon=%llu;below-horizon=%llu;"
                "behind=%llu;out-of-view=%llu;below-min-output=%llu;peak-gate=%.3f;best-cos=%.3f;sun-sin-el=%.3f;total-active=%llu;total-frames=%llu;occlusion=gpu-not-mirrored",
                (long long)now, d(&Counters::lf_frames), w(LF::Why::active), w(LF::Why::off), w(LF::Why::no_env), w(LF::Why::no_light), w(LF::Why::moon),
                w(LF::Why::below_horizon), w(LF::Why::behind), w(LF::Why::out_of_view), w(LF::Why::below_min), double(lf_peak_), double(lf_cos_best_), double(lf_sin_el_),
                (unsigned long long)c_.lf_why[unsigned(LF::Why::active)], (unsigned long long)c_.lf_frames);
            emit(false, text);
            lf_peak_ = 0.0f; lf_cos_best_ = -1.0f;
        }
        if (s_.terrain->conv.wcloud) {   // WCLOUD-FIX2: the last eye-0 probe (CPU mirror of wc_accum / wc_over over the CPU terrain colour; WARP-verified equal to the shader)
            const acz::terrain::WcProbe& q = wc_probe_;
            std::snprintf(text, sizeof text, "xr-stereo;event=summary-wcloud;t-ms=%lld;probes=%llu;gen=%llu;eye=%.0f,%.0f,%.0f;on=%d;tint=%d;band=%.0f..%.0f;rays=%u;with=%u;"
                "mean-a=%.3f;max-a=%.3f;mean-dl=%.2f;max-dl=%.1f;vis8=%u;haze-l=%.0f;far=%.0f",
                (long long)now, wc_probe_n_, (unsigned long long)wc_probe_gen_, double(wc_probe_eye_[0]), double(wc_probe_eye_[1]), double(wc_probe_eye_[2]), q.on ? 1 : 0,
                q.tint ? 1 : 0, double(q.alt - 0.5f * q.thick), double(q.alt + 0.5f * q.thick), q.rays, q.with, double(q.mean_a), double(q.max_a), double(q.mean_dl),
                double(q.max_dl), q.vis8, double(s_.terrain->conv.haze_l), double(s_.terrain->conv.far_dist));
            emit(false, text);
            wc_probe_due_ = true;
        }
    }
    if (s_.terrain && s_.terrain->conv.cam_admit && !s_.terrain->conv.fixed_cam) {   // CAM-EPOCH (admission on only; legacy/key off: no new lines)
        using acz::terrain::Admit;
        auto r = [&](Admit a) { return (unsigned long long)(cam_.refused[unsigned(a)] - cam_prev_.refused[unsigned(a)]); };
        auto fr = [&](Admit a) { return (unsigned long long)(cam_.frame_refused[unsigned(a)] - cam_prev_.frame_refused[unsigned(a)]); };
        auto sr = [&](acz::terrain::CamSource s) { return (unsigned long long)(cam_.src[unsigned(s)] - cam_prev_.src[unsigned(s)]); };
        auto md = [&](acz::terrain::CamMode m) { return (unsigned long long)(cam_.mode[unsigned(m)] - cam_prev_.mode[unsigned(m)]); };
        const auto& ep = s_.terrain->epochs ? *s_.terrain->epochs : acz::terrain::cam_epochs();
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-camera-admit;t-ms=%lld;admitted-frame=%llu;fallback-used=%llu;admitted-latest=%llu;stale-refused=%llu;"
            "future-refused=%llu;epoch-mismatch=%llu(lock:%llu,stage:%llu);uncorrelated=%llu;fallback-missing=%llu;fallback-overwritten=%llu;fallback-invalidated=%llu;"
            "no-sample=%llu;bad-sample=%llu;frame-sample-refused=%llu;game-camera-only=%llu;held=%llu;anim-epoch-resets=%llu;transitions=%llu;"
            "admitted-age-max-ms=%.1f;age-limit-ms=%.0f;window-ms=%.0f;lock-epoch=%u;stage-epoch=%u;modes=hybrid:%llu,game-only:%llu,e6-frame:%llu,e6-game-refused:%llu,"
            "e6-history:%llu,e6-latest:%llu,none:%llu;occlusion-refused=%llu",
            (long long)now, sr(acz::terrain::CamSource::frame), sr(acz::terrain::CamSource::fallback), sr(acz::terrain::CamSource::latest), r(Admit::stale), r(Admit::future),
            r(Admit::lock_epoch) + r(Admit::stage_epoch), r(Admit::lock_epoch), r(Admit::stage_epoch), r(Admit::uncorrelated), r(Admit::missing), r(Admit::overwritten),
            r(Admit::invalidated), r(Admit::none), r(Admit::nonfinite) + r(Admit::insane) + r(Admit::untimed),
            fr(Admit::stale) + fr(Admit::future) + fr(Admit::lock_epoch) + fr(Admit::stage_epoch) + fr(Admit::uncorrelated) + fr(Admit::nonfinite) + fr(Admit::insane) + fr(Admit::untimed),
            (unsigned long long)(cam_.game_only - cam_prev_.game_only), (unsigned long long)(cam_.held - cam_prev_.held),
            (unsigned long long)(cam_.anim_epoch_resets - cam_prev_.anim_epoch_resets), (unsigned long long)(cam_.transitions - cam_prev_.transitions),
            cam_.age_max_ms, double(s_.terrain->conv.cam_age_ms), double(s_.terrain->conv.cam_window_ms), ep.lock(), ep.stage(),
            md(acz::terrain::CamMode::correlated_hybrid), md(acz::terrain::CamMode::game_only), md(acz::terrain::CamMode::e6_frame), md(acz::terrain::CamMode::e6_game_refused),
            md(acz::terrain::CamMode::e6_fallback), md(acz::terrain::CamMode::e6_latest), md(acz::terrain::CamMode::none),
            (unsigned long long)(cam_.occlude_refused - cam_prev_.occlude_refused));
        emit(false, text);
        cam_prev_ = cam_;
        char lag[480];
        if (lag_.line(lag, sizeof lag, (long long)now)) emit(false, lag);
    }
    if (s_.terrain && (s_.terrain->tac.on || s_.terrain->tac.mk360)) summary_tac(now);   // TACTICAL (key on only)
    if (dc_enabled()) summary_decouple(now);                  // DECOUPLE (option on only)
    if (s_.cockpit) {   // COCKPIT-2 (key on only)
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-cockpit;t-ms=%lld;model-frames=%llu;no-view=%llu;backend-failed=%llu;total-frames=%llu;total-no-view=%llu;total-failed=%llu;"
            "stage-light-frames=%llu;anim-frames=%llu;anim-updates=%llu;anim-resets=%llu;stick=%.2f,%.2f;pedal=%.2f;throttle=%.2f;"
            "refl=%s;refl-frames=%llu;refl-uploads=%llu;refl-nomap=%llu;refl-map=%s",
            (long long)now, d(&Counters::cockpit_frames), d(&Counters::cockpit_no_view), d(&Counters::cockpit_failed),
            (unsigned long long)c_.cockpit_frames, (unsigned long long)c_.cockpit_no_view, (unsigned long long)c_.cockpit_failed, d(&Counters::cockpit_stage_light),
            d(&Counters::cockpit_anim_frames), d(&Counters::cockpit_anim_updates), (unsigned long long)anim_state_.resets, double(anim_state_.in.pitch),
            double(anim_state_.in.roll), double(anim_state_.in.yaw), double(anim_state_.in.throttle),
            s_.cockpit->opt.skin_refl ? "on" : "off", d(&Counters::cockpit_refl_frames), d(&Counters::cockpit_refl_uploads), d(&Counters::cockpit_refl_nomap),
            !s_.terrain ? "no-terrain" : drawn_terrain().stage.has_refl() ? "present" : acz::terrain::status_name(drawn_terrain().stage.refl_status));
        emit(false, text);
        if (!s_.cockpit->mesh.store_stations.empty()) {   // STORES-RT: unconditional while the model carries weapon stations (a silent source looks like a dead one)
            // MSL-HUD (hud_msl.h): the step-5 producer's HUD reader (mode 0xFFFFFFFF.. = it has not resolved its option yet: no HUD frame produced)
            const uint32_t msl_mode = acz::hudmsl::status().mode.load(std::memory_order_relaxed), msl_v = acz::hudmsl::status().value.load(std::memory_order_relaxed);
            char msl_val[16];
            if (msl_v == acz::hudmsl::kNoValue) std::snprintf(msl_val, sizeof msl_val, "none"); else std::snprintf(msl_val, sizeof msl_val, "%u", msl_v);
            std::snprintf(text, sizeof text, "xr-stereo;event=summary-stores;t-ms=%lld;stations=%u;hidden-mask=0x%x;fired=%u;launch-signals=%u;tracker-launches=%llu;tracker-rearms=%llu;mask-changes=%llu;"
                "source=%s;launch-src=%s;msl-hud=%s;msl-hud-value=%s;msl-hud-launches=%u",
                (long long)now, stores_.stations(), stores_mask_, stores_.fired(), acz::stores::signal().launches(), (unsigned long long)stores_.launches(),
                (unsigned long long)stores_.rearms(), (unsigned long long)stores_changes_,
                s_.cockpit->opt.stores ? (acz::stores::signal().launches() ? "signalled" : "none(no-launch-signal-known)") : "off(stores:0)",
                // review 2026-11-01 F6: the ONE launch source (ee_counter_search.h Cfg::drives: stctr only with mslhud:0)
                !s_.cockpit->opt.stores ? "off" : s_.cockpit->opt.mslhud ? "msl-hud" : s_.cockpit->opt.stctr_off ? "stctr" : "none",
                msl_mode == 0 ? "off" : msl_mode == 1 ? "on" : msl_mode == 2 ? "on+dump" : "not-started", msl_val, acz::hudmsl::status().launches.load(std::memory_order_relaxed));
            emit(false, text);
        }
        if (!s_.cockpit->mesh.sp_stations.empty()) {   // SP-STORES: unconditional while the model carries special-weapon stations (spstores:0 leaves none: the line is absent then)
            const uint32_t sp_v = acz::hudmsl::status().sp_value.load(std::memory_order_relaxed), sp_mode = acz::hudmsl::status().sp_mode.load(std::memory_order_relaxed);
            char sp_val[16];
            if (sp_v == acz::hudmsl::kNoValue) std::snprintf(sp_val, sizeof sp_val, "none"); else std::snprintf(sp_val, sizeof sp_val, "%u", sp_v);
            std::snprintf(text, sizeof text, "xr-stereo;event=summary-sp-stores;t-ms=%lld;stations=%u;hidden-mask=0x%x;fired=%u;launch-signals=%u;tracker-launches=%llu;tracker-rearms=%llu;mask-changes=%llu;"
                "source=%s;sp-hud=%s;sp-hud-value=%s;sp-hud-launches=%u",
                (long long)now, sp_stores_.stations(), sp_stores_mask_, sp_stores_.fired(), acz::stores::sp_signal().launches(), (unsigned long long)sp_stores_.launches(),
                (unsigned long long)sp_stores_.rearms(), (unsigned long long)sp_stores_changes_,
                !s_.cockpit->opt.stores ? "off(stores:0)" : (acz::stores::sp_signal().launches() ? "signalled" : "none(no-sp-launch-yet)"),
                sp_mode == 0 ? "off" : sp_mode == 1 ? "on" : "not-started", sp_val, acz::hudmsl::status().sp_launches.load(std::memory_order_relaxed));
            emit(false, text);
        }
        const auto& sm = s_.cockpit->mesh;   // CFID (docs/world-render/cockpit-fidelity.md)
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-cockpit-fid;t-ms=%lld;cfid=%s;srefl=%s;fid-frames=%llu;shadow-frames=%llu;shadow-box=%s;"
            "sill=%.2f;half-x=%.2f;z=%.2f..%.2f", (long long)now, s_.cockpit->opt.fidelity ? "on" : "off", s_.cockpit->opt.skin_refl ? "on" : "off",
            d(&Counters::cockpit_fid_frames), d(&Counters::cockpit_shadow_frames), sm.shadow ? "yes" : "none", double(sm.shadow_box[0]),
            double(sm.shadow_box[1]), double(sm.shadow_box[2]), double(sm.shadow_box[3]));
        emit(false, text);
    }
    if (s_.cockpit_auto) {   // AIRCRAFT-AUTO (key value 'auto' only)
        const auto& w = auto_det_.last();
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-cockpit-auto;t-ms=%lld;samples=%llu;centred=%llu;off-centre=%llu;lock-epochs=%llu;windows=%llu;switches=%llu;"
            "current=%s;drawn=%s;loading=%s;loads=%llu;load-failures=%llu;unloads=%llu;last-verdict=%s;last-best=%s:%.3f",
            (long long)now, (unsigned long long)auto_samples_, (unsigned long long)auto_det_.centred_samples, (unsigned long long)auto_det_.off_centre_samples,
            (unsigned long long)auto_det_.epochs, (unsigned long long)auto_det_.windows, (unsigned long long)auto_det_.switches, type_name(auto_det_.current()),
            type_name(auto_installed_), type_name(auto_loading_), (unsigned long long)auto_loads_, (unsigned long long)auto_load_failures_,
            (unsigned long long)auto_unloads_, auto_det_.windows ? acz::aircraft::verdict_name(w.verdict) : "none", type_name(w.best), double(w.best_res));
        emit(false, text);
        // AIRCRAFT-AUTO-FAST: the start state (assumed model, confirmations, ring overruns, remembered-file writes)
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-cockpit-auto-start;t-ms=%lld;assumed=%s;source=%s;confirms=%llu;lost-samples=%llu;remember-writes=%llu;"
            "remember-ok=%llu;remember-failed=%llu;remembered=%s;stale-forgets=%llu;implausible=%llu", (long long)now, auto_det_.assumed() ? type_name(auto_det_.current()) : "no",
            s_.cockpit_auto->assumed_source.empty() ? "none" : s_.cockpit_auto->assumed_source.c_str(), (unsigned long long)auto_confirms_,
            (unsigned long long)auto_lost_, (unsigned long long)auto_writes_, (unsigned long long)auto_wstats_->ok.load(),
            (unsigned long long)auto_wstats_->failed.load(), type_name(auto_remembered_), (unsigned long long)auto_stale_forgets_, (unsigned long long)auto_implausible_);
        emit(false, text);
    }
    std::snprintf(text, sizeof text, "xr-stereo;event=summary-timing;t-ms=%lld;gpu-waits=%llu;cpu-presync=%llu;fence-lag-max=%llu;"
        "completion-ms-max=%llu;completion-ms-mean=%llu;wait-frame-ms-max=%llu;end-frame-ms-max=%llu;end-failed=%llu;"
        "lens-fov-deg=%.2fx%.2f;quad-m=%.0f;reuse-max-age-ms=%lld;hud=%s",
        (long long)now, d(&Counters::gpu_waits), d(&Counters::cpu_presynced), (unsigned long long)c_.fence_lag_max,
        (unsigned long long)c_.completion_ms_max, (unsigned long long)(done ? (c_.completion_ms_sum - p.completion_ms_sum) / done : 0),
        (unsigned long long)c_.wait_frame_ms_max, (unsigned long long)c_.end_frame_ms_max, (unsigned long long)c_.end_failed,
        lens_used_ ? deg(lens_fov_.right - lens_fov_.left) : 0.0, lens_used_ ? deg(lens_fov_.up - lens_fov_.down) : 0.0,
        double(s_.quad_distance_m), (long long)s_.reuse_max_age_ms, s_.hud_layer ? "layer" : "not-included");
    // J-cockpit-fov-consumer (additive, trailing): headset FOV, cockpit FOV source, refusals.
    emit(false, std::string(text) + eye_fov_tokens() + ";cockpit-fov=" + cockpit_fov_name(cockpit_fov_source_) +
        ";cockpit-fov-frames=" + u64(c_.cockpit_fov_frames - p.cockpit_fov_frames) +
        ";cockpit-fov-invalid=" + u64(c_.cockpit_fov_invalid - p.cockpit_fov_invalid));
    if (s_.world_in_projection)
        emit(false, "xr-stereo;event=summary-world-composite;t-ms=" + std::to_string((long long)now) + ";world-composite=projection;frames=" +
            u64(c_.world_composite_frames - p.world_composite_frames) + ";total=" + u64(c_.world_composite_frames) + ";refused=" +
            u64(c_.world_composite_refused - p.world_composite_refused) + ";world-frames=" + u64(c_.world_frames - p.world_frames) +
            ";plane-distance-m=" + std::to_string(double(s_.quad_distance_m)) + ";no-headset-claim");
    if (s_.effects_layer) { // J-test8-fixes: the effects counters every period (the shutdown-only effects-result is missing from runs that end uncleanly)
        const uint64_t seen = c_.effects_seen - p.effects_seen, valid = c_.effects_prod_valid - p.effects_prod_valid;
        const uint64_t draws = c_.effects_prod_draws - p.effects_prod_draws;
        char avg[32];
        std::snprintf(avg, sizeof avg, "%.2f", valid ? double(draws) / double(valid) : 0.0);
        emit(false, "xr-stereo;event=summary-effects;t-ms=" + std::to_string((long long)now) + ";consumed=" + u64(seen) + ";producer-valid=" + u64(valid) +
            ";producer-draws-per-valid-frame=" + avg + ";producer-refused=" + u64(c_.effects_prod_refused - p.effects_prod_refused) +
            ";producer-tail=" + u64(c_.effects_prod_tail - p.effects_prod_tail) + ";composited=" + u64(c_.effects_frames - p.effects_frames) +
            ";skipped=" + u64(c_.effects_skipped - p.effects_skipped) + ";invalid=" + u64(c_.effects_invalid - p.effects_invalid) +
            ";total-composited=" + u64(c_.effects_frames) + ";smoke-eye-valid=" + u64(c_.smoke_seen) + ";smoke-eye-composited=" + u64(c_.smoke_frames) +
            ";sprite-eye-valid=" + u64(c_.sprite_seen) + ";sprite-eye-composited=" + u64(c_.sprite_frames) + ";no-headset-claim");
    }
    if (c_.world_incomplete_frames) // J-180: only when the producer reported lost world draws (never with its key off)
        emit(false, "xr-stereo;event=summary-world-incomplete;t-ms=" + std::to_string((long long)now) + ";frames=" + u64(c_.world_incomplete_frames - p.world_incomplete_frames) +
            ";lost-draws=" + u64(c_.world_lost_draws - p.world_lost_draws) + ";total-frames=" + u64(c_.world_incomplete_frames) +
            ";total-lost-draws=" + u64(c_.world_lost_draws) + ";no-headset-claim");
    if (s_.world_hold_last)
        emit(false, "xr-stereo;event=summary-world-hold;t-ms=" + std::to_string((long long)now) + ";captured=" + u64(c_.world_captured - p.world_captured) +
            ";held=" + u64(c_.world_held - p.world_held) + ";expired=" + u64(c_.world_hold_expired - p.world_hold_expired) +
            ";capture-failed=" + u64(c_.world_hold_capture_failed - p.world_hold_capture_failed) + ";max-held-age-ms=" + u64(c_.world_hold_max_age_ms) +
            ";total-held=" + u64(c_.world_held) + ";total-expired=" + u64(c_.world_hold_expired) + ";hold-ms=" + std::to_string((long long)kWorldHoldMs) + ";no-headset-claim");
    if (s_.world_cache)
        emit(false, "xr-stereo;event=summary-world-cache;t-ms=" + std::to_string((long long)now) + ";frames=" + u64(c_.world_cache_frames - p.world_cache_frames) +
            ";written=" + u64(c_.world_cache_written - p.world_cache_written) + ";skipped=" + u64(c_.world_cache_skipped - p.world_cache_skipped) +
            ";failed=" + u64(c_.world_cache_failed - p.world_cache_failed) + ";total-frames=" + u64(c_.world_cache_frames) + ";total-written=" + u64(c_.world_cache_written) +
            ";no-headset-claim");
    if (s_.eye_res_pct)   // WORLD-WINDOW FILTER: unconditional while eyeres is on (zero frames = no world composite this period)
        emit(false, "xr-stereo;event=summary-world-filter;t-ms=" + std::to_string((long long)now) + ";frames=" + u64(c_.world_filter_frames - p.world_filter_frames) +
            ";total-frames=" + u64(c_.world_filter_frames) + ";bit=21;grid=footprint-1|2x2;eye0=" + std::to_string(ext_[0].w) + "x" + std::to_string(ext_[0].h) +
            ";no-headset-claim");
    if (have_last_) emit(false, layer_fov_line(last_plan_)); // J-layer-fov: once per summary period
    if (s_.hud_layer) {
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-hud;t-ms=%lld;hud-frames=%llu;hud-layers=%llu;hud-skipped=%llu;hud-invalid=%llu;"
            "hud-lens-invalid=%llu;hud-mode=%s;hud-swapchain=%ux%u;producer-hud-draws=%u;producer-hud-refused=%u;no-headset-claim",
            (long long)now, d(&Counters::hud_frames), d(&Counters::hud_layers), d(&Counters::hud_skipped), d(&Counters::hud_invalid),
            d(&Counters::hud_lens_invalid), hud_mode_name(s_.hud_mode), ext_[kHud].w, ext_[kHud].h, last_hud_draws_, last_hud_refused_);
        // J-hud-placement: trailing tokens only with a placement key configured.
        emit(false, s_.hud_placement_log ? std::string(text) + hud_placement_tokens(s_) : std::string(text));
    }
    if (s_.hud_markers_layer) {
        std::snprintf(text, sizeof text, "xr-stereo;event=summary-markers;t-ms=%lld;marker-frames=%llu;marker-layers=%llu;marker-skipped=%llu;marker-invalid=%llu;"
            "marker-lens-invalid=%llu;marker-swapchain=%ux%u;producer-marker-draws=%u;producer-marker-refused=%u;no-headset-claim",
            (long long)now, d(&Counters::marker_frames), d(&Counters::marker_layers), d(&Counters::marker_skipped), d(&Counters::marker_invalid),
            d(&Counters::marker_lens_invalid), ext_[kMarkers].w, ext_[kMarkers].h, last_marker_draws_, last_marker_refused_);
        emit(false, text);
    }
    auto metric = [](bool available, double value) { return available ? std::to_string(value) : std::string("unavailable"); };
    const std::string extra = "xr-stereo;event=summary-timing;gpu-start-ms=" + metric(gpu_samples_ != 0, gpu_samples_ ? gpu_start_sum_ / gpu_samples_ : 0) +
        ";gpu-blit-ms=" + metric(gpu_samples_ != 0, gpu_samples_ ? gpu_blit_sum_ / gpu_samples_ : 0) +
        ";end-late-ms-max=" + metric(late_samples_ != 0, end_late_max_) +
        ";xr-period-ms-mean=" + metric(period_samples_ != 0, period_samples_ ? period_sum_ / period_samples_ : 0) +
        ";xr-period-ms-max=" + metric(period_samples_ != 0, period_max_) +
        ";frame-cpu-ms-max=" + std::to_string(frame_cpu_max_) + ";blocked-ms-max=" + std::to_string(blocked_max_);
    emit(false, extra);
    emit(false, "xr-stereo;event=summary-selection;select-lag=" + u64(select_lag_) +
        ";select-age-ms=" + metric(select_age_available_, select_age_ms_) +
        ";new-per-s=" + metric(window_ms > 0, window_ms > 0 ? double(d(&Counters::new_frames)) * 1000 / window_ms : 0));
    gpu_start_sum_ = gpu_blit_sum_ = period_sum_ = period_max_ = end_late_max_ = 0;
    gpu_samples_ = period_samples_ = late_samples_ = 0;
    frame_cpu_max_ = blocked_max_ = 0;
    last_summary_ = c_;
    // Window maxima restart each period.
    c_.completion_ms_max = c_.wait_frame_ms_max = c_.end_frame_ms_max = c_.fence_lag_max = c_.reuse_age_max_ms = c_.world_hold_max_age_ms = 0;
}

bool Output::retire(Gate& g) {
    if (s_.pov_stage2) acz::povr::producer_request().store(0u, std::memory_order_relaxed);   // POV-RENDER stage 2: no consumer, no chase frames
    if (!setup_attempted_ || destroyed_) return true;
    summary(g, true);
    auto retained = [&](const char* why) {
        emit(true, std::string("xr-stereo;event=retire;outcome=retained;reason=") + why);
        return false;
    };
    // Never manufacture readiness: an ambiguous submission has no proven timeline.
    if (l_.ambiguous) return retained("ambiguous submission; images/slot/swapchains retained");
    if (l_.submitted && !l_.complete) {
        if (!l_.signalled) return retained("submitted without signal");
        // Half of the 2 s cleanup budget: the exit handshake keeps the rest.
        (void)observe_completion(g, g.now_ms() + xr_owner::Limits{}.cleanup_ms / 2, s_.retire_sample_ms);
        if (!l_.complete) return retained("GPU completion not observed inside cleanup");
        // Additive (step14 run 2): how late the quarantined submission actually completed
        // (upper bound, retire_sample_ms resolution) - the real backlog the window missed.
        emit(true, "xr-stereo;event=late-completion;private-fence=" + u64(l_.value) +
            ";after-signal-ms-max=" + u64(uint64_t(std::max<int64_t>(0, last_sample_ms_ - l_.signal_ms))) +
            ";sample-ms=" + u64(uint64_t(s_.retire_sample_ms)));
    }
    if (!g.lost()) {
        for (unsigned t = 0; t < kTargets; ++t) {
            if (!l_.acquired[t] || l_.waited[t]) continue;
            bool timed_out = false;
            const Rc rc = g.call(Op::wait_image, [&] { return xr_->wait(t, s_.wait_retry_ns, timed_out); });
            if (succeeded(rc) && !timed_out) l_.waited[t] = true;
        }
        if (!release_images(g)) return retained("image release failed");
        for (unsigned t = 0; t < kTargets; ++t) if (l_.acquired[t]) return retained("unwaited acquisition outstanding");
    }
    release_slot();
    dc_release("retire");   // DECOUPLE: the held game frame (every recording that read it has completed: proven above)
    if (s_.vscreen && !vscreen_retire(g)) return retained("vscreen copy completion not observed");   // VSCREEN: before any swapchain is destroyed
    if (!g.lost()) {
        for (unsigned t = 0; t < kTargets; ++t) {
            if (!chains_[t]) continue;
            const Rc rc = g.call(Op::destroy_swapchain, [&] { return xr_->destroy_swapchain(t); });
            if (!succeeded(rc)) return retained("xrDestroySwapchain failed");
            chains_[t] = false;
        }
    } // lost: session destruction retires its swapchains
    if (blit_setup_) {
        const Rc rc = g.call(Op::d3d_destroy, [&] { return blit_->destroy() ? Rc::ok : Rc::error; });
        if (!succeeded(rc)) return retained("private D3D12 object release failed");
        blit_setup_ = false;
    }
    destroyed_ = true;
    emit(true, "xr-stereo;event=retire;outcome=clean");
    return true;
}

std::string Output::result_line(const xr_owner::Report* r) const {
    const bool pass = r ? (r->success && !failed() && c_.submitted > 0) : (!failed() && c_.submitted > 0);
    std::string reason = failed() ? reason_ : (c_.submitted ? "layers-submitted" : "no-layer-submitted");
    std::string out = std::string("result=") + (r ? (pass ? "PASS" : "FAIL") : (pass ? "PROVISIONAL" : "FAIL")) +
        ";reason=" + reason + ";frames=" + u64(c_.frames) + ";submitted=" + u64(c_.submitted) + ";new=" + u64(c_.new_frames) +
        ";reused=" + u64(c_.reused) + ";stale-zero=" + u64(c_.stale_zero) +
        ";consumed=" + u64(c_.acquired_frames) + ";released=" + u64(c_.released_frames) + ";zero-no-frame=" + u64(c_.zero_no_frame) +
        ";should-render-false=" + u64(c_.should_render_false) + ";pose-frame=" + u64(c_.pose_from_frame) +
        ";pose-located=" + u64(c_.pose_from_located) + ";depth=" + u64(c_.depth_frames) + ";world=" + u64(c_.world_frames) +
        ";lens=" + u64(c_.lens_frames) + ";deferred=" + u64(c_.deferred) + ";stale-age=" + u64(c_.stale_age) +
        ";stale-tracking=" + u64(c_.stale_tracking) + ";stale-space=" + u64(c_.stale_space) +
        ";fresh-stale-space=" + u64(c_.fresh_stale_space) + ";fresh-stale-tracking=" + u64(c_.fresh_stale_tracking) +
        ";world-lens-invalid=" + u64(c_.world_lens_invalid);
    if (r) out += std::string(";owner-success=") + (r->success ? "1" : "0") + ";clean=" + (r->clean ? "1" : "0") +
        ";stop=" + xr_owner::stop_name(r->stop) + ";first-cause=" + xr_owner::cause_name(r->first_cause) +
        ";projection-layers=" + u64(r->projection_layers) + ";ended=" + u64(r->ended) + ";consumer-retired=" + (r->consumer_retired ? "1" : "0");
    // (markers: reported by the markers-result line; the result line is near the router cap with the world-composite tokens)
    // (the world-composite totals moved to their own line, see finished(): this line exceeded the router cap with them)
    out += s_.hud_layer ? ";hud=layer;no-headset-claim" : ";hud=not-included;no-headset-claim";
    return out;
}
void Output::finished(const xr_owner::Report& r) noexcept {
    try {
        // Totals before the result line (the router's final fence is the result line itself).
        if (s_.hud_layer)
            emit(true, "xr-stereo;event=hud-result;key=xr_hud_layer;hud-mode=" + std::string(hud_mode_name(s_.hud_mode)) +
                ";hud-frames=" + u64(c_.hud_frames) + ";hud-layers=" + u64(c_.hud_layers) + ";hud-skipped=" + u64(c_.hud_skipped) +
                ";hud-invalid=" + u64(c_.hud_invalid) + ";hud-lens-invalid=" + u64(c_.hud_lens_invalid) +
                ";hud-swapchain=" + std::to_string(ext_[kHud].w) + "x" + std::to_string(ext_[kHud].h) + ";no-headset-claim");
        if (s_.hud_markers_layer)
            emit(true, "xr-stereo;event=markers-result;key=xr_hud_markers_layer;marker-frames=" + u64(c_.marker_frames) + ";marker-layers=" + u64(c_.marker_layers) +
                ";marker-skipped=" + u64(c_.marker_skipped) + ";marker-invalid=" + u64(c_.marker_invalid) + ";marker-lens-invalid=" + u64(c_.marker_lens_invalid) +
                ";marker-swapchain=" + std::to_string(ext_[kMarkers].w) + "x" + std::to_string(ext_[kMarkers].h) + ";no-headset-claim");
        if (s_.effects_layer)
            emit(true, "xr-stereo;event=effects-result;key=xr_effects_layer;effects-frames=" + u64(c_.effects_frames) + ";effects-skipped=" + u64(c_.effects_skipped) +
                ";effects-invalid=" + u64(c_.effects_invalid) + ";no-headset-claim");
        if (s_.world_in_projection)
            emit(true, "xr-stereo;event=world-composite-result;world-composite=projection;world-composite-frames=" + u64(c_.world_composite_frames) +
                ";world-composite-refused=" + u64(c_.world_composite_refused) + ";no-headset-claim");
        if (s_.world_hold_last)
            emit(true, "xr-stereo;event=world-hold-result;key=xr_world_hold_last;captured=" + u64(c_.world_captured) + ";held=" + u64(c_.world_held) +
                ";expired=" + u64(c_.world_hold_expired) + ";capture-failed=" + u64(c_.world_hold_capture_failed) + ";hold-ms=" +
                std::to_string((long long)kWorldHoldMs) + ";no-headset-claim");
        emit(true, "xr-stereo;" + result_line(&r));
    } catch (...) {}
}

// ------------------------------------------------------------- registry ---
namespace {
std::atomic<stereo::FrameSource*> g_source{nullptr};
// R-5: process-lifetime registry (never destroyed: readers may run during DLL/CRT teardown).
xr_owner::OwnerRegistry& g_owners = *new xr_owner::OwnerRegistry();
}
void register_frame_source(stereo::FrameSource* s) noexcept {
    stereo::FrameSource* expected = nullptr;
    g_source.compare_exchange_strong(expected, s, std::memory_order_acq_rel); // first registration wins
}
stereo::FrameSource* registered_frame_source() noexcept { return g_source.load(std::memory_order_acquire); }
bool try_read_pose(xr_owner::PoseSnapshot& out) noexcept { return g_owners.try_read_pose(out); }
void publish_owner(const std::shared_ptr<xr_owner::Owner>& owner) noexcept {
    // try_read_pose may run on any thread at any time: every published owner stays alive until release_owner() proves (reader quiescence,
    // xr_owner_registry.h) that no reader can still hold its pointer. Publication replaces the pointer readers see.
    (void)g_owners.publish(owner);
}
const char* release_owner(const std::shared_ptr<xr_owner::Owner>& owner, int64_t max_wait_ms) noexcept {
    return xr_owner::OwnerRegistry::release_name(g_owners.release(owner, max_wait_ms));
}
OwnerCounts owner_counts() noexcept {
    OwnerCounts c;
    c.published = g_owners.published(); c.released = g_owners.released(); c.retained = g_owners.retained(); c.busy = g_owners.busy();
    return c;
}
namespace {
std::atomic<void*> g_fence{nullptr};
std::atomic<uint64_t> g_fence_epoch{0};
std::atomic<uint64_t> g_fence_seq{0}; // even = stable (seqlock; writer is the producer's owner context)
}
void register_producer_fence(void* fence, uint64_t epoch) noexcept {
    g_fence_seq.fetch_add(1, std::memory_order_acq_rel);
    g_fence.store(fence, std::memory_order_release);
    g_fence_epoch.store(fence ? epoch : 0, std::memory_order_release);
    g_fence_seq.fetch_add(1, std::memory_order_acq_rel);
}
bool producer_fence_registered(const void* fence, uint64_t epoch) noexcept {
    if (!fence || !epoch) return false;
    const uint64_t a = g_fence_seq.load(std::memory_order_acquire);
    if (a & 1) return false; // registration in progress: not trusted this frame
    const bool match = g_fence.load(std::memory_order_acquire) == fence && g_fence_epoch.load(std::memory_order_acquire) == epoch;
    return match && g_fence_seq.load(std::memory_order_acquire) == a;
}
bool start_ready(const stereo::FrameSource* s) noexcept { return s && s->frames_published() > 0; }
bool retry_allowed(unsigned attempt, const xr_owner::Report& r) noexcept {
    using xr_owner::Cause;
    const bool cold = r.first_cause == Cause::startup_deadline || r.first_cause == Cause::ready_timeout ||
        r.first_cause == Cause::progress_deadline;
    return attempt == 1 && r.complete && !r.success && cold && !r.loss_observed && r.clean && !r.session_held &&
        !r.instance_held && !r.binding_held;
}
bool retry_allowed(unsigned attempt, const xr_owner::Report& r, bool consumer_startup_budget) noexcept {
    if (retry_allowed(attempt, r)) return true;
    if (!consumer_startup_budget || r.first_cause != xr_owner::Cause::consumer) return false;
    xr_owner::Report as_cold = r;                       // the consumer's budget refusal IS a cold-start deadline (nothing was submitted)
    as_cold.first_cause = xr_owner::Cause::startup_deadline;
    return retry_allowed(attempt, as_cold);
}
void request_stop() noexcept {
    g_owners.request_stop_current();
}

// ---------------------------------------------------------------- VSCREEN (xr_vscreen.h; docs/world-render/vscreen.md; only with Settings::vscreen)
// A hook summary may hold several lines joined by '\n' (BRIEF3D: table line + producer line): one log line each.
void Output::emit_lines(bool lifecycle, const std::string& t) const {
    for (size_t b = 0; b < t.size();) {
        size_t e = t.find('\n', b); if (e == std::string::npos) e = t.size();
        if (e > b) emit(lifecycle, t.substr(b, e - b));
        b = e + 1;
    }
}
bool Output::vscreen_pace(Gate& g, const xr_owner::FrameInfo& info, xr_owner::LayerPlan& plan) {
    if (!chains_[kHud] || !images_[kHud].count) return false;
    VScreenIn in;
    // The age of the last FRESH flight composite (a refused frame or a space change does not make the flight look over).
    in.flight_age_ms = ever_new_ ? std::max<int64_t>(0, g.now_ms() - last_new_ms_) : -1;
    for (unsigned e = 0; e < 2; ++e) { in.eye_images[e] = chains_[e] && images_[e].count ? &images_[e] : nullptr; in.eye_ext[e] = ext_[e]; }   // BRIEF3D
    const bool shown = s_.vscreen->frame(g, info, *xr_, kHud, images_[kHud], ext_[kHud], in, plan);
    if (s_.vscreen->poisoned()) {   // Astra finding 1: an unsettled acquisition on the SHARED HUD swapchain: nothing may touch it again
        plan = {};
        if (const std::string t = s_.vscreen->summary(); !t.empty()) emit_lines(true, t);
        fail("vscreen left the shared HUD swapchain acquisition unsettled");
        return true;
    }
    if (!shown) plan = {};
    if (s_.screen_dump_trigger && s_.png_sink) screen_dump(g, shown);
    if (const std::string t = s_.vscreen->summary(); !t.empty()) emit_lines(false, t);
    return shown;
}
// SCREEN-DUMP (xr_screen_dump.h): the trigger is polled at most once per second, only while the screen is shown and no dump is busy; one trigger
// = one attempt (the file is deleted before arming). The parts come back from the hook after their readback completed (never waited for).
void Output::screen_dump(Gate& g, bool shown) {
    const int64_t now = g.now_ms();
    if (dump_active_) {
        DumpImage d;
        for (unsigned i = 0; i < 2 && s_.vscreen->dump_take(g, d); ++i, d = DumpImage{}) {
            const std::string name = "acz-vr-xr-screen-t" + std::to_string((long long)dump_ms_) + "-" + d.part + ".png";
            const uint32_t w = d.w, h = d.h;
            // `queued` = the sink accepted the image; the asynchronous writer reports the final written/failed (xr_stereo_proxy.cpp: event=png-file;file=<name>).
            const bool wrote = d.ok && s_.png_sink(name, d.rgba, w, h);
            emit(true, "xr-stereo;event=png-screen;part=" + d.part + ";outcome=" + (wrote ? "queued" : "failed") + ";why=" +
                (d.ok ? (wrote ? "none" : "sink") : d.why) + ";size=" + std::to_string(w) + "x" + std::to_string(h) + ";file=" + (wrote ? name : "-") +
                ";armed-ms=" + std::to_string((long long)dump_ms_) + ";t-ms=" + std::to_string((long long)now) +
                ";orientation=" + (d.part == "screen" ? "rot180" : "as-stored(eye,like-xr_png_once)") + ";read-with=xr_dump_upright.py;no-headset-claim");
        }
        if (!s_.vscreen->dump_busy()) dump_active_ = false;
        return;
    }
    if (!shown || now - dump_check_ms_ < 1000) return;
    dump_check_ms_ = now;
    if (!s_.screen_dump_trigger(false)) { dump_consume_logged_ = false; return; }
    if (!s_.screen_dump_trigger(true)) {   // exists but could not be consumed (delete refused): no attempt (else every poll would re-arm); logged once per stuck file
        if (!dump_consume_logged_) {
            dump_consume_logged_ = true;
            emit(true, "xr-stereo;event=png-screen-armed;outcome=refused;why=trigger-not-consumed;table=-;t-ms=" + std::to_string((long long)now) +
                ";trigger=png-screen.now(delete-failed)");
        }
        return;
    }
    dump_consume_logged_ = false;
    const char* why = s_.vscreen->dump_arm(now);
    dump_active_ = why == nullptr; dump_ms_ = now;
    emit(true, std::string("xr-stereo;event=png-screen-armed;outcome=") + (why ? "refused" : "armed") + ";why=" + (why ? why : "none") +
        ";table=" + (why ? "-" : s_.vscreen->dump_table()) + ";t-ms=" + std::to_string((long long)now) + ";trigger=png-screen.now(deleted)");
}
bool Output::vscreen_exclusive() const noexcept { return s_.vscreen && s_.vscreen->exclusive(); }
bool Output::vscreen_retire(Gate& g) {
    const bool ok = s_.vscreen->retire(g);
    if (const std::string t = s_.vscreen->summary(); !t.empty()) emit_lines(true, t);
    return ok;
}
} // namespace acz::xr_stereo
