#pragma once
// POV / lean / HUD fixes after headset test 14 (docs/review/pov-fixes-impl.md; all behind default-OFF keys).
//
// Header-only, SDK-free maths shared by the producer (gpu_copy_step5.h setup_poses) and the consumer (xr_stereo_output.cpp):
//  * F-LEAN  (key gpu_copy_step5_neck_lever_m = "lever[,scale]"): the head position of every eye is the NECK-PIVOT arc of the real head. A pure
//    head rotation about a pivot behind the eyes translates the eyes by R n - R0 n (n = pivot -> eye midpoint in head axes). Subtracting that
//    arc leaves a rotation about the eye point; real leaning stays. scale (0..1) multiplies the remaining head-centre translation
//    (0 = rotation-only). The eye SEPARATION (IPD) is never scaled.
//  * F-HUD   (key xr_hud_follow): the LOCAL quads (HUD, world, markers) are shifted by the same amount so they stay fixed relative to the
//    virtual eye; mode 2 additionally turns the HUD about the anchor by a rate-limited fraction of the rendered look.
// Nothing here is called with the keys off; the key-off paths of the callers are the original expressions.
#include <cmath>
#include <cstdint>

namespace acz::pov {

// Pivot -> eye-midpoint lever in head axes (OpenXR: +y up, -z forward) is n = (0, kPitchLever * f, -f): test 13/14 fits gave u/f = 0.67..0.70.
inline constexpr double kPitchLever = 0.7;
inline constexpr double kLeverMax = 0.30;   // metres; larger values are clamped
struct Lean {
    double lever = 0.0, scale = 1.0;
    bool active() const noexcept { return lever > 0.0 || scale != 1.0; }
};
inline Lean make_lean(double lever, double scale) noexcept {
    Lean l;
    l.lever = (lever > 0.0 && std::isfinite(lever)) ? (lever > kLeverMax ? kLeverMax : lever) : 0.0;
    l.scale = (scale >= 0.0 && std::isfinite(scale)) ? (scale > 1.0 ? 1.0 : scale) : 1.0;
    return l;
}
// Column-vector rotation of an OpenXR quaternion (x,y,z,w), v' = R v, row-major. False when the quaternion is not unit-ish.
inline bool quat_rot(const float* q, double R[9]) noexcept {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0.5) || !(n < 1.5) || !std::isfinite(n)) return false;
    x /= n; y /= n; z /= n; w /= n;
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
    return true;
}
// Delta = R n - R0 n in LOCAL coordinates: R the current head rotation (row-major, LOCAL), R0 = Ry(yaw0) the neutral heading (pure yaw; the
// producer's Neutral::yaw0). Ry(a): x' = cos a x + sin a z, z' = -sin a x + cos a z.
inline void neck_delta_local(const double R[9], double yaw0, double lever, double out[3]) noexcept {
    const double n[3] = {0.0, kPitchLever * lever, -lever};
    const double c = std::cos(yaw0), s = std::sin(yaw0);
    const double r0[3] = {c * n[0] + s * n[2], n[1], -s * n[0] + c * n[2]};
    for (int i = 0; i < 3; ++i) out[i] = (R[i * 3] * n[0] + R[i * 3 + 1] * n[1] + R[i * 3 + 2] * n[2]) - r0[i];
}
// The virtual head centre, relative to the neutral p0 (LOCAL): scale * ((m - p0) - Delta). `m_rel` = m - p0 (m = eye midpoint).
inline void virtual_centre_offset(const double m_rel[3], const double delta[3], double scale, double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = scale * (m_rel[i] - delta[i]);
}
// Shift of a cockpit-frame-registered LOCAL layer so that it stays fixed relative to the virtual eye: real eye midpoint m minus virtual
// centre v: (m - p0) - scale * ((m - p0) - Delta) = (1 - scale) (m - p0) + scale Delta. Zero for lever 0 and scale 1 (the layers do not move).
inline void layer_shift(const double m_rel[3], const double delta[3], double scale, double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = (1.0 - scale) * m_rel[i] + scale * delta[i];
}
// Everything the consumer knows: the recorded eye poses and the frame's neutral. False (shift 0) for invalid input.
inline bool frame_layer_shift(const Lean& lean, const float pos0[3], const float pos1[3], const float orient0[4], const float neutral_pos[3],
                              float neutral_yaw, double out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0;
    if (!lean.active()) return false;
    double R[9];
    if (!quat_rot(orient0, R)) return false;
    double m_rel[3], d[3];
    for (int i = 0; i < 3; ++i) m_rel[i] = 0.5 * (double(pos0[i]) + double(pos1[i])) - double(neutral_pos[i]);
    neck_delta_local(R, double(neutral_yaw), lean.lever, d);
    layer_shift(m_rel, d, lean.scale, out);
    for (int i = 0; i < 3; ++i) if (!std::isfinite(out[i]) || std::fabs(out[i]) > 5.0) { out[0] = out[1] = out[2] = 0.0; return false; }
    return true;
}

// ---------------------------------------------------------------- F-HUD mode 2: look-follow with a rate limit
inline constexpr double kHudFollowAlpha = 0.5;               // fraction of the rendered look the HUD follows
inline constexpr double kHudFollowRateRadS = 1.0471975512;   // 60 deg/s
struct HudFollow {
    double yaw = 0.0, pitch = 0.0;   // the HUD's current extra turn (yaw > 0 = left, pitch > 0 = up, as the world quad)
    bool init = false;
    int64_t last_ms = 0;
};
// One consumer frame: the HUD's extra turn moves towards alpha * look at no more than rate rad/s. The first call (or after reset) starts AT
// the target, so there is no start-up sweep. dt is capped at 0.25 s.
inline void hud_follow_step(HudFollow& s, double look_yaw, double look_pitch, int64_t now_ms, double alpha = kHudFollowAlpha,
                            double rate = kHudFollowRateRadS) noexcept {
    const double ty = std::isfinite(look_yaw) ? alpha * look_yaw : 0.0, tp = std::isfinite(look_pitch) ? alpha * look_pitch : 0.0;
    if (!s.init) { s.yaw = ty; s.pitch = tp; s.init = true; s.last_ms = now_ms; return; }
    double dt = double(now_ms - s.last_ms) / 1000.0;
    s.last_ms = now_ms;
    if (!(dt > 0.0)) return;
    if (dt > 0.25) dt = 0.25;
    const double step = rate * dt;
    auto move = [step](double cur, double target) {
        const double d = target - cur;
        return d > step ? cur + step : (d < -step ? cur - step : target);
    };
    s.yaw = move(s.yaw, ty);
    s.pitch = move(s.pitch, tp);
}

}  // namespace acz::pov
