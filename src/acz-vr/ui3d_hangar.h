#pragma once
// UI3D stage 3b (docs/world-render/ui-3d.md section 9): the HANGAR screens (aircraft select, special-weapon selects) as a 3D ROOM in the headset.
// SDK-free, header-only, pure: the record checks, the camera decomposition of the hangar's DrawACM_v matrix A, the room placement (latch) and the
// per-eye replacement matrix A'_e. The producer frame kind is in brief3d_live.h (Producer::ui3d_acm + the hangar rules of ui3d_draw), the D3D12
// replay in ui3d_replay_hooks.inl (UNTESTED LIVE). Default off (u3a:0). Nothing here ran in the game or a headset.
//
// Facts it relies on (step130 null run K10, ee-camera.log `ui3d-census;kind=acm`, 20 hangar frames x 32 logged ACM draws; ui-3d.md 8.5) [E]:
//  * Every hangar frame starts with a DrawACM_v draw; all its ACM draws come BEFORE its first 2D overlay draw (16 of 20 frames show the overlay
//    inside the 48 logged ps2_v draws; none has an ACM draw after it). Between the ACM blocks: ps2_v quads with REAL depth (GS z 4.5e4..2e5) or
//    indexed (unread) = the light glows (SRC_ALPHA/ONE) and the floor shadow (REV_SUBTRACT). The overlay's z is 0, 1 or 64.
//  * ACM record: B (+256) = identity in all 640 logged draws (the geometry is stored in world units), A (+0) bit-identical across a frame's
//    logged draws (620 same / 20 first), +360 = 1.0, +400 = (682, -448, 2048, 2048) (the flight's is 512), cb10 dword 0 = 2 (no fade bit 8).
//    PSO RTV 10 / DSV 20, depth GREATER (5) with write, blend SRC_ALPHA/INV_SRC_ALPHA, the UI target 2560 x 1440.
//  * A is an exact pinhole camera in every frame: w column xyz unit (|f| = 1.000000), axes orthogonal (< 2e-7), right.y = 0 (no roll, world
//    up = +Y), right x up = -forward, tan 0.8325 x 0.4688 over the 2560 x 1440 target with the 682 constants (square pixels; the 4:3 scissor
//    320..2240 shows tan 0.624 x 0.469), depth column = b w + Z0 with b = -255.008, Z0 = 16,777,470 (residual 2e-8): the flight's model.
//  * A CHANGES between frames (the camera orbits ~9 deg/s around the aircraft; a cut between screens); B does not: the turntable is the camera.
//    So the aircraft is NOT separable from the hangar by B (all identity) in the logged draws; draws 33+ of each frame were not logged [U].
// Route (u3a): every ACM draw of a hangar frame is replayed per eye with A replaced by A'_e = (world -> eye e clip in the shader's PS2 form),
// built from the HMD eye pose and a LOCAL <-> hangar-world mapping (latched: u3a:1; following the game camera's yaw: u3a:2). B and every other
// byte stay the game's. The VS forms h = (local,1) B A, ndc.x = (h.x/h.w - k2) / (k0/2), ndc.y = (h.y/h.w - k3) / (k1/2), depth = h.z/h.w * k360
// * 2^-24 (gpu_copy_step5_world.h header; the flight per-eye replay of the same shader runs live).
#include "ui3d.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::ui3d::hangar {

inline constexpr uint32_t kRecBytes = 416;          // the SrtData record (A +0, B +256, +360 depth scale, +400 screen constants)
inline constexpr uint32_t kRecStride = 512;         // one eye copy in the upload span (256-aligned root SRV address)
inline constexpr float kScreen[4] = {682.0f, -448.0f, 2048.0f, 2048.0f};   // +400 on the hangar screens [E]; the flight's is 512 (not a hangar)
inline constexpr float kDepthScale = 1.0f;          // +360 [E]
inline constexpr uint32_t kFadeFlag = 8;            // cb10 dword 0 bit 8: the fade branch reads +64 (C), which the substitution leaves stale
inline constexpr float kOverlayMaxZ = 256.0f;       // a ps2_v quad with GS z above this is a scene effect (glow/shadow), at or below the 2D overlay
inline constexpr unsigned kMaxAcmPerFrame = 256;    // K10: 132..145 per frame
inline constexpr int64_t kGapMs = 2000;             // no hangar frame for this long: the room is re-latched
inline constexpr double kCutUnits = 8.0;            // a camera jump larger than this (hangar units) between two hangar frames = a cut (K10: ~0.2/frame drift)
inline constexpr double kCutCos = 0.98480775301;    // or a forward change above 10 degrees
inline constexpr double kRelTol = 1e-6;             // A of a later draw against the frame's first (as the flight's world records)

inline float rf(const uint8_t* p, unsigned i) noexcept { float f; std::memcpy(&f, p + 4 * size_t(i), 4); return f; }
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const double a[3], const double b[3], double o[3]) noexcept {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
inline bool normalize3(double v[3]) noexcept {
    const double n = std::sqrt(dot3(v, v));
    if (!(n > 1e-12) || !std::isfinite(n)) return false;
    for (int i = 0; i < 3; ++i) v[i] /= n;
    return true;
}

// ================================================================================================== record checks
// nullptr = a hangar record the substitution handles; "u3a-not-hangar" = +400 is not the hangar's (flight, offscreen passes): not ours.
inline const char* record_check(const uint8_t* rec) noexcept {
    for (unsigned i = 0; i < 4; ++i) if (rf(rec, 100 + i) != kScreen[i]) return "u3a-not-hangar";
    for (unsigned i = 0; i < 16; ++i) { const float f = rf(rec, i); if (!std::isfinite(f) || std::fabs(f) > 1e12f) return "u3a-record-a-nonfinite"; }
    for (unsigned i = 64; i < 80; ++i) if (!std::isfinite(rf(rec, i))) return "u3a-record-b-nonfinite";
    if (rf(rec, 64 + 3) != 0.0f || rf(rec, 64 + 7) != 0.0f || rf(rec, 64 + 11) != 0.0f || rf(rec, 64 + 15) != 1.0f) return "u3a-record-b-not-affine";
    if (!(rf(rec, 90) == kDepthScale)) return "u3a-record-depth-scale";
    return nullptr;
}
inline const char* cb10_check(const char* cb_why, uint32_t dword0) noexcept {
    if (cb_why) return cb_why;
    return (dword0 & kFadeFlag) ? "u3a-cb10-fade" : nullptr;
}
// A of a later draw against the frame's first (+0..+63).
inline bool same_a(const uint8_t* rec, const uint8_t* ref) noexcept {
    for (unsigned i = 0; i < 16; ++i) {
        const double a = rf(rec, i), b = rf(ref, i);
        if (!(std::fabs(a - b) <= kRelTol * std::fmax(std::fabs(b), 1.0))) return false;
    }
    return true;
}

// ================================================================================================== the game camera from A
// Row vectors: h = (p, 1) A, A[r*4 + c]. Column c of A = (A[c], A[4+c], A[8+c], A[12+c]).
struct Camera {
    bool ok = false;
    double eye[3] = {0, 0, 0}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};   // hangar world; right x up = -fwd
    double tanX = 0, tanY = 0;     // half-extents of the target over the whole viewport (ndc +-1)
    double b = 0, z0 = 0;          // depth column = b * w column + (0,0,0,z0)
};
inline const char* decompose(const float A[16], const float k[4], Camera& c) noexcept {
    c = Camera{};
    for (int i = 0; i < 16; ++i) if (!std::isfinite(A[i])) return "u3a-camera-nonfinite";
    if (!(std::fabs(k[0]) > 1) || !(std::fabs(k[1]) > 1)) return "u3a-camera-screen";
    double W[4], X[4], Y[4], Z[4];
    for (int r = 0; r < 4; ++r) {
        W[r] = A[r * 4 + 3]; X[r] = double(A[r * 4 + 0]) - double(k[2]) * W[r]; Y[r] = double(A[r * 4 + 1]) - double(k[3]) * W[r]; Z[r] = A[r * 4 + 2];
    }
    const double fn = std::sqrt(W[0] * W[0] + W[1] * W[1] + W[2] * W[2]);
    if (!(std::fabs(fn - 1.0) < 1e-3)) return "u3a-camera-w-not-unit";   // [E] 1.000000: w = view depth in hangar units
    const double sx = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]), sy = std::sqrt(Y[0] * Y[0] + Y[1] * Y[1] + Y[2] * Y[2]);
    if (!(sx > 1e-9) || !(sy > 1e-9)) return "u3a-camera-degenerate";
    const double gx = k[0] > 0 ? 1.0 : -1.0, gy = k[1] > 0 ? 1.0 : -1.0;
    for (int i = 0; i < 3; ++i) { c.fwd[i] = W[i] / fn; c.right[i] = gx * X[i] / sx; c.up[i] = gy * Y[i] / sy; }
    if (std::fabs(dot3(c.right, c.up)) > 1e-4 || std::fabs(dot3(c.right, c.fwd)) > 1e-4 || std::fabs(dot3(c.up, c.fwd)) > 1e-4) return "u3a-camera-not-orthogonal";
    double rxu[3]; cross3(c.right, c.up, rxu);
    if (!(dot3(rxu, c.fwd) < -0.999)) return "u3a-camera-handedness";   // [E] right x up = -fwd: (right, up, back) right-handed
    if (std::fabs(c.right[1]) > 1e-3 || !(c.up[1] > 0)) return "u3a-camera-roll";   // [E] right.y = 0, world up = +Y
    // the eye: (e,1) . column = 0 for the x, y and w columns (Cramer on the 3x3 system M^T e = -t)
    const double m[3][3] = {{X[0], X[1], X[2]}, {Y[0], Y[1], Y[2]}, {W[0], W[1], W[2]}};
    const double t[3] = {-X[3], -Y[3], -W[3]};
    auto det3 = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double D = det3(m);
    if (!(std::fabs(D) > 1e-12)) return "u3a-camera-degenerate";
    for (int j = 0; j < 3; ++j) {
        double mj[3][3]; std::memcpy(mj, m, sizeof m);
        for (int r = 0; r < 3; ++r) mj[r][j] = t[r];
        c.eye[j] = det3(mj) / D;
    }
    c.tanX = std::fabs(double(k[0])) * 0.5 * fn / sx; c.tanY = std::fabs(double(k[1])) * 0.5 * fn / sy;
    if (!(c.tanX > 0.05 && c.tanX < 20) || !(c.tanY > 0.05 && c.tanY < 20)) return "u3a-camera-lens-out-of-range";
    // depth column = b x w column on xyz; the translation's remainder is Z0
    const double ww = W[0] * W[0] + W[1] * W[1] + W[2] * W[2];
    c.b = (Z[0] * W[0] + Z[1] * W[1] + Z[2] * W[2]) / ww;
    double res = 0, mag = 0;
    for (int i = 0; i < 3; ++i) { const double r = Z[i] - c.b * W[i]; res += r * r; mag += Z[i] * Z[i]; }
    if (!(std::sqrt(res) <= 1e-4 * std::sqrt(mag))) return "u3a-camera-depth-column";
    c.z0 = Z[3] - c.b * W[3];
    if (!(c.b < 0) || !(c.z0 > 0)) return "u3a-camera-depth-model";
    for (double v : c.eye) if (!std::isfinite(v)) return "u3a-camera-nonfinite";
    c.ok = true;
    return nullptr;
}
inline const char* decompose_record(const uint8_t* rec, Camera& c) noexcept {
    float A[16], k[4];
    std::memcpy(A, rec, 64); std::memcpy(k, rec + 400, 16);
    return decompose(A, k, c);
}
// A camera jump between two hangar frames (a new screen).
inline bool cut(const Camera& a, const Camera& b) noexcept {
    const double d[3] = {a.eye[0] - b.eye[0], a.eye[1] - b.eye[1], a.eye[2] - b.eye[2]};
    return std::sqrt(dot3(d, d)) > kCutUnits || dot3(a.fwd, b.fwd) < kCutCos;
}

// ================================================================================================== the room: LOCAL <-> hangar world
// hangar = E + upm * Rm (local - anchor). Rm maps the head's level frame at the latch (right, up, back) onto the game camera's level frame
// (right_h, +Y, -fwd_h): a proper rotation (both right-handed), yaw only: the game camera's pitch is NOT applied (a level room), the user's own
// head pitch is (the HMD pose). anchor = the mid-eye point at the latch.
struct Room { bool valid = false; double E[3] = {0, 0, 0}, Rm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, anchor[3] = {0, 0, 0}, upm = 5.0; };
inline void quat_rotate(const double q[4], const double v[3], double o[3]) noexcept {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}
inline bool quat_ok(const double q[4]) noexcept {
    const double n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return std::isfinite(n) && std::fabs(n - 1.0) < 1e-3;
}
// The head's level yaw frame from an orientation: false = looking (almost) straight up/down.
inline bool head_level(const double q[4], double hr[3], double hb[3]) noexcept {
    const double mz[3] = {0, 0, -1};
    double f[3]; quat_rotate(q, mz, f);
    f[1] = 0;
    if (!normalize3(f)) return false;
    if (std::fabs(f[0]) < 1e-6 && std::fabs(f[2]) < 1e-6) return false;
    hb[0] = -f[0]; hb[1] = 0; hb[2] = -f[2];
    hr[0] = -f[2]; hr[1] = 0; hr[2] = f[0];   // right = fwd x up = (-fz, 0, fx)
    return true;
}
// The room from a game camera and the head (mid-eye position, orientation) at the latch.
inline const char* make_room(const Camera& c, const double head_p[3], const double head_q[4], double upm, Room& out) noexcept {
    out = Room{};
    if (!c.ok) return "u3a-room-camera";
    if (!quat_ok(head_q)) return "u3a-room-head-quat";
    for (int i = 0; i < 3; ++i) if (!std::isfinite(head_p[i])) return "u3a-room-head-position";
    if (!(upm >= 1.0 && upm <= 200.0)) return "u3a-room-upm";
    double hr[3], hb[3];
    if (!head_level(head_q, hr, hb)) return "u3a-room-head-vertical";
    double fh[3] = {c.fwd[0], 0, c.fwd[2]};
    if (std::fabs(c.fwd[1]) > 0.95 || !normalize3(fh)) return "u3a-room-camera-vertical";
    const double yg[3] = {0, 1, 0}, yl[3] = {0, 1, 0};
    double rh[3]; cross3(fh, yg, rh);                               // right_h = fwd_h x up  [E: equals the camera's right]
    const double bh[3] = {-fh[0], 0, -fh[2]};
    // Rm = rh hr^T + yg yl^T + bh hb^T (row-major 3x3)
    for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) out.Rm[r * 3 + k] = rh[r] * hr[k] + yg[r] * yl[k] + bh[r] * hb[k];
    for (int i = 0; i < 3; ++i) { out.E[i] = c.eye[i]; out.anchor[i] = head_p[i]; }
    out.upm = upm; out.valid = true;
    return nullptr;
}
inline void mul_rm(const double Rm[9], const double v[3], double o[3]) noexcept {
    for (int r = 0; r < 3; ++r) o[r] = Rm[r * 3 + 0] * v[0] + Rm[r * 3 + 1] * v[1] + Rm[r * 3 + 2] * v[2];
}
inline void local_to_world(const Room& m, const double l[3], double g[3]) noexcept {
    const double d[3] = {l[0] - m.anchor[0], l[1] - m.anchor[1], l[2] - m.anchor[2]};
    double r[3]; mul_rm(m.Rm, d, r);
    for (int i = 0; i < 3; ++i) g[i] = m.E[i] + m.upm * r[i];
}

// ================================================================================================== the per-eye matrix
// One located eye (LOCAL position, orientation x y z w, FOV angles in radians: left < 0 < right, down < 0 < up).
struct Eye { double p[3] = {0, 0, 0}; double q[4] = {0, 0, 0, 1}; double fov[4] = {-0.785, 0.785, 0.785, -0.785}; };
// A'_e such that the game's VS, with the record's own +400 constants k and depth model (b, z0), puts a hangar-world point where eye e sees it:
//   w' = (p - G) . f        (G, r, u, f = the eye's position and axes in hangar units)
//   h.x = (k0/2)/sx (x_v - cx w') + k2 w'   (x_v = (p - G) . r; cx, sx = centre and half-span of the eye's tan extents)
//   h.y = (k1/2)/sy (y_v - cy w') + k3 w'
//   h.z = b w' + z0        (the game's depth model: reversed, GREATER, near ~1 unit, far ~65,536 units)
inline const char* eye_matrix(const Room& m, const Camera& c, const float k[4], const Eye& e, float out[16]) noexcept {
    if (!m.valid || !c.ok) return "u3a-eye-room";
    if (!quat_ok(e.q)) return "u3a-eye-quat";
    for (int i = 0; i < 3; ++i) if (!std::isfinite(e.p[i])) return "u3a-eye-position";
    const double tl = std::tan(e.fov[0]), tr = std::tan(e.fov[1]), tu = std::tan(e.fov[2]), td = std::tan(e.fov[3]);
    if (!std::isfinite(tl + tr + tu + td) || !(tr - tl > 1e-6) || !(tu - td > 1e-6)) return "u3a-eye-fov";
    const double cx = 0.5 * (tr + tl), sx = 0.5 * (tr - tl), cy = 0.5 * (tu + td), sy = 0.5 * (tu - td);
    double G[3]; local_to_world(m, e.p, G);
    const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    double lx[3], ly[3], lz[3]; quat_rotate(e.q, ex, lx); quat_rotate(e.q, ey, ly); quat_rotate(e.q, ez, lz);
    double r[3], u[3], b[3]; mul_rm(m.Rm, lx, r); mul_rm(m.Rm, ly, u); mul_rm(m.Rm, lz, b);
    const double f[3] = {-b[0], -b[1], -b[2]};
    const double colW[4] = {f[0], f[1], f[2], -dot3(G, f)};
    const double colR[4] = {r[0], r[1], r[2], -dot3(G, r)};
    const double colU[4] = {u[0], u[1], u[2], -dot3(G, u)};
    const double ax = 0.5 * double(k[0]) / sx, ay = 0.5 * double(k[1]) / sy;
    const double bx = double(k[2]) - ax * cx, by = double(k[3]) - ay * cy;
    for (int row = 0; row < 4; ++row) {
        const double c0 = ax * colR[row] + bx * colW[row];
        const double c1 = ay * colU[row] + by * colW[row];
        const double c2 = c.b * colW[row] + (row == 3 ? c.z0 : 0.0);
        const double c3 = colW[row];
        const double v[4] = {c0, c1, c2, c3};
        for (int col = 0; col < 4; ++col) {
            const float x = float(v[col]);
            if (!std::isfinite(x)) return "u3a-eye-matrix-nonfinite";
            out[row * 4 + col] = x;
        }
    }
    return nullptr;
}
// The eye copy of one record: A replaced, every other byte the game's.
inline void substitute(const uint8_t* rec, const float a[16], uint8_t* out) noexcept {
    std::memcpy(out, rec, kRecBytes);
    std::memcpy(out, a, 64);
}
// What the game's VS computes for a hangar-world point with matrix A (identity B): ndc and w (test oracle; also the producer's sanity check).
inline bool shader_ndc(const float A[16], const float k[4], const double p[3], double& nx, double& ny, double& w, double& depth) noexcept {
    double h[4];
    for (int col = 0; col < 4; ++col) h[col] = p[0] * A[col] + p[1] * A[4 + col] + p[2] * A[8 + col] + A[12 + col];
    w = h[3];
    if (!(std::fabs(w) > 1e-12)) return false;
    nx = (h[0] / w - double(k[2])) / (0.5 * double(k[0]));
    ny = (h[1] / w - double(k[3])) / (0.5 * double(k[1]));
    depth = h[2] / w * double(kDepthScale) / 16777216.0;
    return std::isfinite(nx) && std::isfinite(ny);
}

// ================================================================================================== ps2_v draws of a hangar frame
enum class Ps2Class { effect, overlay };
// Before the overlay: an indexed draw (its vertices are not read) or a quad with real depth is a scene effect (ui-3d.md 9.2): the indexed ones
// (the floor shadow) are dropped; the non-indexed quads (the ceiling-light glows) are re-projected (u3ag:1, below) or dropped (u3ag:0).
inline Ps2Class classify_ps2(bool indexed, float z) noexcept { return indexed || !(z <= kOverlayMaxZ) ? Ps2Class::effect : Ps2Class::overlay; }

// ================================================================================================== the light glows (2026-10-30c, u3ag)
// Headset test 5 item 2: the bright ceiling-light glows of the flat hangar (user-shots 7.webp) were absent in VR. Cause [E]: they are not ACM
// geometry but ps2_v quads the EE already projected with the menu camera (K10: 4-vertex quads, additive SRC_ALPHA/ONE, depth ALWAYS without
// write, q = 1, ONE GS z per quad 3.4e4..2.9e5, between the ACM blocks), and ui3d_draw dropped every such quad (`u3a-fx-dropped`; the
// step137 headset log: u3a-fx = 271,209 over 6,627 hangar frames = ~41 per frame). A camera substitution cannot move them, but the frame's own
// camera (decompose: eye, axes, lens, depth model) inverts them: the GS z is the game's depth model z = b + Z0 / w (b = -255.008, Z0 =
// 16,777,470 [E for the ACM draws]; the glows share the target, the depth buffer and its range [I]), so each vertex is a hangar-world point
//   w = Z0 / (z - b),  P = eye + w (fwd + ndc_x tanX right + ndc_y tanY up),  ndc from the draw's own cb0 over the whole target (= k400 [E]).
// P -> LOCAL through the latched room, then the eye's projection (as the 2D layers): the glow sits where the light is in the room, for every
// eye position (a sprite facing the GAME camera; from the anchor it equals the flat image). q' = q (w_src / upm) / w_eye (perspective-correct
// on the sprite's plane; the scale of q' does not affect clipping). z unchanged (the PSO's depth ALWAYS: painter's order as in the game).
inline void world_to_local(const Room& m, const double g[3], double l[3]) noexcept {
    const double d[3] = {(g[0] - m.E[0]) / m.upm, (g[1] - m.E[1]) / m.upm, (g[2] - m.E[2]) / m.upm};
    for (int r = 0; r < 3; ++r) l[r] = m.anchor[r] + m.Rm[0 * 3 + r] * d[0] + m.Rm[1 * 3 + r] * d[1] + m.Rm[2 * 3 + r] * d[2];   // Rm^T d
}
inline constexpr double kEffectMinW = 0.5, kEffectMaxW = 70000.0;   // hangar units: in front of the camera, inside the depth model's far range
// One GS vertex (x, y, z) of a pre-projected effect -> the hangar-world point and its view depth w (hangar units). false = unusable.
inline bool gs_to_world(const Camera& c, const acz::ui3d::Cb0& cb, double x, double y, double z, double P[3], double& w) noexcept {
    if (!c.ok || !acz::ui3d::cb0_ok(cb) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    const double den = z - c.b;
    if (!(den > 0)) return false;
    w = c.z0 / den;
    if (!(w >= kEffectMinW && w <= kEffectMaxW)) return false;
    const double nx = (x - double(cb.ox)) * 2.0 / double(cb.sx), ny = (y - double(cb.oy)) * 2.0 / double(cb.sy);
    for (int i = 0; i < 3; ++i) P[i] = c.eye[i] + w * (c.fwd[i] + nx * c.tanX * c.right[i] + ny * c.tanY * c.up[i]);
    return std::isfinite(P[0] + P[1] + P[2]);
}
// The eye copy of one effect quad (src: n vertices of 64 B as the game's VB holds them). why = unusable (the glow is dropped, never a frame
// rejection); culled = a vertex behind / too near this eye (no draw for it).
inline acz::ui3d::RewriteResult rewrite_effect(const uint8_t* src, uint32_t n, const acz::ui3d::Cb0& cb, const Camera& c, const Room& m,
                                               const acz::ui3d::EyeView& eye, uint8_t* dst) noexcept {
    namespace u3 = acz::ui3d;
    u3::RewriteResult res;
    if (!src || !dst || n == 0 || n > u3::kMaxVerticesPerDraw) { res.why = "u3a-glow-vertices"; return res; }
    if (!c.ok || !m.valid || !(m.upm > 0)) { res.why = "u3a-glow-room"; return res; }
    std::memcpy(dst, src, size_t(n) * u3::kVertexStride);
    for (uint32_t i = 0; i < n; ++i) {
        float f[4]; std::memcpy(f, src + size_t(i) * u3::kVertexStride, 16);
        if (!(f[3] > 0.0f) || !std::isfinite(f[3])) { res.why = "u3a-glow-q"; return res; }
        double G[3], ws;
        if (!gs_to_world(c, cb, f[0], f[1], f[2], G, ws)) { res.why = "u3a-glow-unproject"; return res; }
        double L[3]; world_to_local(m, G, L);
        double nx, ny, we;
        if (!u3::project(eye, L, nx, ny, we)) { res.culled = true; continue; }
        const float x2 = float(double(cb.ox) + nx * double(cb.sx) * 0.5), y2 = float(double(cb.oy) + ny * double(cb.sy) * 0.5);
        const float q2 = float(double(f[3]) * (ws / m.upm) / we);
        if (!std::isfinite(x2) || !std::isfinite(y2) || !std::isfinite(q2) || !(q2 > 0.0f)) { res.culled = true; continue; }
        std::memcpy(dst + size_t(i) * u3::kVertexStride + 0, &x2, 4);
        std::memcpy(dst + size_t(i) * u3::kVertexStride + 4, &y2, 4);
        std::memcpy(dst + size_t(i) * u3::kVertexStride + 12, &q2, 4);
    }
    return res;
}

// ================================================================================================== the glow memory (2026-10-30d, u3agm)
// Headset test 6 item 2: "hangar lights turn off (intermittently)". Evidence [E: live-2210-step143 ee-camera.log, 22 summary-ui3d windows
// over 6,268 hangar frames]: u3a-glow-why stayed `none` for the whole session (no glow was ever dropped by a fact: not the depth model, not
// the viewport, not the upload budget), u3a-rejected 0, u3a-published = u3a-frames - 1, u3a-fx ~2-3 per frame (the floor shadow only), yet
// the re-projected glows per frame ran 36, 40, 24, 7, 39, 27, 38, 33, 44, 37, 9, 3.5, 5.5, 5.7, 7, 21, 27, 25, 24, 37, 52 per 5 s window; the
// fall from 44 to 3.5 per frame happened inside a stretch with NO cut, NO moving frame and NO latch (u3a-cuts 22, u3a-moving 55 unchanged).
// So the game itself stopped emitting most glows while the user's room stayed put: the EE pre-projects a glow only for a light ITS camera
// sees [I: the aircraft-select camera orbits at ~9 deg/s (ui-3d.md 8.5), a 40 s turn; the low stretch lasted ~25 s], so in the latched room
// the lamps blink with the game camera's orbit (10.webp: dark lamp fixtures on the ceiling). Not our drop, the game's emission.
// Fix: every re-projected glow is remembered in HANGAR-WORLD space (its 4 source vertices + the camera / cb0 that projected them; the hangar
// is static between cuts), and redrawn while the game does not emit it. A remembered glow is drawn as an EXTRA COPY of a later real glow draw
// with the same state key (PSO + pixel texture / sampler tables: the copy uses the carrier's PSO, texture and blend), never earlier in the
// game's order than where it was last seen (its count of ACM draws before it: geometry drawn after a glow in the game can never cover a
// remembered glow that the game itself would not have covered). A real glow already drawn from memory in this frame is not drawn again.
inline constexpr unsigned kGlowMemCap = 96;                                       // K10 / step143: up to ~50 glows per frame [E]; the orbit shows more over a turn
inline constexpr unsigned kGlowCarrierQuads = acz::ui3d::kMaxVerticesPerDraw / 4;   // 16 quads per carried draw (the eye copy buffer)
inline constexpr double kGlowMatchMin = 1.0, kGlowMatchMax = 8.0;                 // hangar units (0.2 .. 1.6 m at 5 u/m)
struct GlowEntry {
    uint64_t key = 0;                       // the state key (0 = a forgotten entry)
    double centre[3] = {0, 0, 0};           // hangar world
    double radius = 0;                      // centre -> farthest vertex (hangar units)
    uint32_t acm_before = 0;                // ACM draws of its frame before it when last seen
    uint64_t seen = 0, drawn = 0;           // frame serials: last seen from the game / last drawn (real or remembered)
    acz::ui3d::Cb0 cb;                      // the source projection of `src`
    Camera cam;
    uint8_t src[4 * acz::ui3d::kVertexStride] = {};
};
// The hangar-world centre (mean of the 4 vertices) and radius of a pre-projected effect quad. nullptr = usable; else the drop reason.
inline const char* effect_centre(const uint8_t* src, uint32_t n, const acz::ui3d::Cb0& cb, const Camera& c, double centre[3], double& radius) noexcept {
    if (!src || n != 4) return "u3a-glow-vertices";
    double P[4][3];
    for (int k = 0; k < 3; ++k) centre[k] = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        float f[4]; std::memcpy(f, src + size_t(i) * acz::ui3d::kVertexStride, 16);
        if (!(f[3] > 0.0f) || !std::isfinite(f[3])) return "u3a-glow-q";
        double w;
        if (!gs_to_world(c, cb, f[0], f[1], f[2], P[i], w)) return "u3a-glow-unproject";
        for (int k = 0; k < 3; ++k) centre[k] += 0.25 * P[i][k];
    }
    radius = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const double d[3] = {P[i][0] - centre[0], P[i][1] - centre[1], P[i][2] - centre[2]};
        radius = std::fmax(radius, std::sqrt(dot3(d, d)));
    }
    return std::isfinite(radius) ? nullptr : "u3a-glow-unproject";
}
class GlowMemory {
public:
    void clear() noexcept { n_ = 0; }
    unsigned size() const noexcept { return n_; }
    const GlowEntry& at(unsigned i) const noexcept { return e_[i]; }
    GlowEntry& at(unsigned i) noexcept { return e_[i]; }
    // The remembered glow a real glow of frame `frame` refreshes: the nearest entry with the same key, not yet seen in this frame (two sprites
    // of one lamp in one frame stay two entries), within clamp(radius / 2, 1, 8) units. -1 = none.
    int match(uint64_t key, const double c[3], double radius, uint64_t frame) const noexcept {
        int best = -1; double bd = 0;
        for (unsigned i = 0; i < n_; ++i) {
            const GlowEntry& g = e_[i];
            if (!key || g.key != key || g.seen == frame) continue;
            const double d[3] = {c[0] - g.centre[0], c[1] - g.centre[1], c[2] - g.centre[2]};
            const double dist = std::sqrt(dot3(d, d));
            const double tol = std::fmin(kGlowMatchMax, std::fmax(kGlowMatchMin, 0.5 * std::fmax(radius, g.radius)));
            if (dist <= tol && (best < 0 || dist < bd)) { best = int(i); bd = dist; }
        }
        return best;
    }
    // Refresh entry `idx` (>= 0) or insert a new one (-1). Returns its index, or -1 when the memory is full (the glow is drawn, not remembered).
    int remember(int idx, uint64_t key, const uint8_t* src, const acz::ui3d::Cb0& cb, const Camera& cam, const double c[3], double radius,
                 uint32_t acm_before, uint64_t frame) noexcept {
        if (!key || !src) return -1;
        if (idx < 0) {
            if (n_ >= kGlowMemCap) return -1;
            idx = int(n_++); e_[idx] = GlowEntry{};
        }
        GlowEntry& g = e_[idx];
        g.key = key; g.radius = radius; g.acm_before = acm_before; g.seen = frame; g.cb = cb; g.cam = cam;
        for (int k = 0; k < 3; ++k) g.centre[k] = c[k];
        std::memcpy(g.src, src, sizeof g.src);
        return idx;
    }
    // The remembered glows a carrier (state key, ACM draws so far) may draw now: same key, not drawn in `frame`, last seen no later in the
    // game's order (acm_before <= acm), `skip` excluded; at most `max`, in memory order. Returns the count written to `out`.
    unsigned pending(uint64_t key, uint32_t acm, uint64_t frame, int skip, unsigned max, int* out) const noexcept {
        unsigned k = 0;
        for (unsigned i = 0; i < n_ && k < max; ++i) {
            const GlowEntry& g = e_[i];
            if (!key || g.key != key || g.drawn == frame || int(i) == skip || g.acm_before > acm) continue;
            out[k++] = int(i);
        }
        return k;
    }
    void forget(unsigned i) noexcept { if (i < n_) e_[i].key = 0; }
    // Forgotten entries leave (called between frames only: indices change).
    void compact() noexcept {
        unsigned w = 0;
        for (unsigned i = 0; i < n_; ++i) if (e_[i].key) { if (w != i) e_[w] = e_[i]; ++w; }
        n_ = w;
    }
    // Remembered glows not drawn in `frame` (no carrier with their key came at or after their place in the game's order).
    unsigned missed(uint64_t frame) const noexcept {
        unsigned k = 0;
        for (unsigned i = 0; i < n_; ++i) if (e_[i].key && e_[i].drawn != frame) ++k;
        return k;
    }
private:
    GlowEntry e_[kGlowMemCap];
    unsigned n_ = 0;
};
// A copy that must draw nothing for one eye (culled there): every vertex = vertex 0 (zero-area triangles; finite, q > 0 kept).
inline void degenerate_quad(uint8_t* q) noexcept {
    for (unsigned i = 1; i < 4; ++i) std::memcpy(q + size_t(i) * acz::ui3d::kVertexStride, q, acz::ui3d::kVertexStride);
}

} // namespace acz::ui3d::hangar
