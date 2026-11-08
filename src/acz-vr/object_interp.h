#pragma once
// object_interp.h -- OBJECT-INTERP (xr_terrain_stage option interp:1, default OFF; docs/world-render/interp.md).
// Header only, SDK-free, no allocation (fixed arrays), bounded loops: used on the game's render thread by the step-5 producer
// (gpu_copy_step5_hooks.inl, world-object replay), by the consumer (xr_stereo_output.cpp, terrain camera) and by the offline tests
// (tests/acz-vr/object_interp_offline_tests.cpp, xr_composite_warp_harness.cpp).
//
// WHERE the replayed objects are drawn (code reading, [E]): the DrawACM_v world objects are re-recorded per eye by the PRODUCER into the GAME's
// command list (gpu_copy_step5_world.h: record +0 A substituted per eye), i.e. once per GAME frame into the slot's eye images. The consumer only
// composites those images; a re-submitted XR frame reuses the last layers (Output::pace). So the object draws can be given a different model
// matrix (record +256 B) once per game frame, NOT once per XR frame. What this module does with that (interp:1):
//  * identity tracking of every replayed world draw across consecutive producer frames (mesh signature + IA buffer identity + ordinal within the
//    signature + a proximity gate around the predicted position; appear / disappear / teleport (pooled object reused) / scale change (LOD));
//  * each object's world transform is evaluated at a fractional game-frame offset k relative to its newest state: -1 <= k < 0 interpolates between
//    the previous and the newest state (translation lerp, rotation slerp on the orthonormalised 3x3, scale from the newest state), k > 0
//    extrapolates with the last per-frame velocity / rotation rate, clamped to max_extrap_frames, never through a reversal, stop or rate change;
//  * the producer chooses k = the consumer's measured select age (XR predicted display time - the frame's own predicted display time, EMA) x the
//    measured game frame rate, i.e. the same t_display the extrap:1 terrain camera uses; the game camera is dead-reckoned to the SAME t_display
//    (pacing::MotionExtrap, the extrap:1 model) and the rigid camera delta is folded into every object's model matrix (w' = Cf + dR^T (w - Cf - dt)),
//    so the objects are drawn as the DISPLAY-time camera sees them through the frame's own per-eye A. The consumer applies the very same delta
//    (published in the StereoFrame) to the terrain camera: terrain, props, clouds, tactical markers and objects share one camera and one t_display.
//  * what stays at the game rate: the HUD, effects sprites, the game's own screen-space marker sprites, the replayed cockpit (camera-relative).
// Rows of B (row vectors, world = (local,1) x B): r[0..2] = images of the local x/y/z axes (scale included), t = translation (record floats 76..78).
#include "frame_pacing.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::interp {

constexpr unsigned kCap = 96;   // = gpu_copy::step5::world::draw_cap (static_assert where both are visible)

// ------------------------------------------------------------------ process-wide switch and horizon (consumer writes, producer reads)
struct Shared {
    std::atomic<uint32_t> on{0};
    std::atomic<uint32_t> max_frames_milli{1500};   // extrapolation clamp, game frames x 1000
    std::atomic<uint32_t> max_ms_milli{33000};      // camera horizon clamp (extrapms), ms x 1000
    std::atomic<uint32_t> attitude{1};              // extrapatt
    std::atomic<int64_t> horizon_us{-1};            // consumer select-age EMA (us); -1 = not measured yet
};
inline Shared& shared() noexcept { static Shared s; return s; }
inline void arm(bool on, float max_frames, float max_ms, bool attitude) noexcept {
    Shared& s = shared();
    s.max_frames_milli.store(uint32_t(std::lround(double(max_frames) * 1000.0)), std::memory_order_relaxed);
    s.max_ms_milli.store(uint32_t(std::lround(double(max_ms) * 1000.0)), std::memory_order_relaxed);
    s.attitude.store(attitude ? 1u : 0u, std::memory_order_relaxed);
    s.horizon_us.store(-1, std::memory_order_relaxed);
    s.on.store(on ? 1u : 0u, std::memory_order_release);
}
inline bool armed() noexcept { return shared().on.load(std::memory_order_acquire) != 0; }
inline void publish_horizon_ms(double ms) noexcept {
    if (!(ms >= 0.0) || !std::isfinite(ms)) return;
    shared().horizon_us.store(int64_t(ms * 1000.0), std::memory_order_relaxed);
}
inline double horizon_ms() noexcept { const int64_t v = shared().horizon_us.load(std::memory_order_relaxed); return v < 0 ? -1.0 : double(v) * 1e-3; }
// The consumer's smoothing of the per-frame select age (a single late frame does not throw every object forward).
struct HorizonEma {
    double v = -1.0;
    void feed(double ms) noexcept { if (!(ms >= 0.0) || !std::isfinite(ms)) return; v = v < 0 ? ms : 0.8 * v + 0.2 * ms; }
};

// ------------------------------------------------------------------ what the producer publishes per frame (StereoFrame::interp; POD)
struct Facts {
    bool valid = false;       // the producer planned this frame (interp armed, the frame's world camera recovered)
    bool cam_moved = false;   // dt / dR are not the identity
    float k = 0.0f;           // game frames ahead the objects were drawn at (0 = frame time)
    float horizon_ms = 0.0f;  // wall horizon of the camera move
    float dt[3] = {0, 0, 0};  // display camera eye - frame camera eye (world units)
    float dR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // axis_d = dR axis_f (column convention, row-major)
    uint32_t objects = 0, moved = 0, appeared = 0, teleports = 0, scale_changes = 0, held = 0, lost = 0, write_fail = 0;
    const char* why = nullptr;   // static: why k is 0 ("ok" when moved)
    // The first kSamples tracked objects of the frame (draw order): track id, the game's position (frame time) and the drawn display-time position
    // (before the camera delta). The consumer logs them in bounded windows (interp-trace) for the frame-by-frame live comparison (interp.md section 6).
    static constexpr unsigned kSamples = 4;
    struct Sample { uint32_t id = 0; uint8_t kind = 0, moved = 0; float obs[3] = {0, 0, 0}, disp[3] = {0, 0, 0}; };
    uint32_t nsample = 0;
    Sample sample[kSamples];
};

// ------------------------------------------------------------------ transforms
struct Xform { double r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; double t[3] = {0, 0, 0}; };
inline float rec_f(const uint8_t* p, unsigned i) noexcept { float f; std::memcpy(&f, p + 4 * size_t(i), 4); return f; }
// The record's B (416-byte SrtData, +256..+319 = floats 64..79): rows 0..2 xyz and the translation row.
inline void xform_from_record(const uint8_t* raw, Xform& x) noexcept {
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) x.r[r * 3 + c] = double(rec_f(raw, 64u + unsigned(r) * 4u + unsigned(c)));
    for (int c = 0; c < 3; ++c) x.t[c] = double(rec_f(raw, 76u + unsigned(c)));
}
// Writes rows 0..2 xyz and the translation row; column 3 (0,0,0,1) and every other byte are untouched. false = non-finite (nothing written).
inline bool xform_to_record(const Xform& x, uint8_t* out) noexcept {
    float v[12];
    for (int i = 0; i < 9; ++i) v[i] = float(x.r[i]);
    for (int i = 0; i < 3; ++i) v[9 + i] = float(x.t[i]);
    for (float f : v) if (!std::isfinite(f) || std::fabs(f) > 1e12f) return false;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) std::memcpy(out + 4 * size_t(64 + r * 4 + c), &v[r * 3 + c], 4);
    for (int c = 0; c < 3; ++c) std::memcpy(out + 4 * size_t(76 + c), &v[9 + c], 4);
    return true;
}
namespace detail {
inline double dot3(const double* a, const double* b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double len3(const double* a) noexcept { return std::sqrt(dot3(a, a)); }
inline void qnorm(double q[4]) noexcept { const double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (l > 0) for (int i = 0; i < 4; ++i) q[i] /= l; }
inline void qmul(const double a[4], const double b[4], double o[4]) noexcept {   // (w,x,y,z)
    const double w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    const double x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    const double y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    const double z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    o[0] = w; o[1] = x; o[2] = y; o[3] = z;
}
// Rotation matrix (column convention, m[row*3+col]) -> unit quaternion.
inline void quat_from_m(const double m[9], double q[4]) noexcept {
    const double tr = m[0] + m[4] + m[8];
    if (tr > 0) { const double s = std::sqrt(tr + 1.0) * 2; q[0] = 0.25 * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s; }
    else if (m[0] > m[4] && m[0] > m[8]) { const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2; q[0] = (m[7] - m[5]) / s; q[1] = 0.25 * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s; }
    else if (m[4] > m[8]) { const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2; q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25 * s; q[3] = (m[5] + m[7]) / s; }
    else { const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2; q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25 * s; }
    qnorm(q);
}
inline void m_from_quat(const double q[4], double m[9]) noexcept {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}
// Relative rotation q_rel = b * conj(a) as a rotation vector (axis x angle, rad), shortest path.
inline void rel_rotvec(const double a[4], const double b[4], double w[3]) noexcept {
    const double ca[4] = {a[0], -a[1], -a[2], -a[3]};
    double r[4]; qmul(b, ca, r);
    if (r[0] < 0) for (double& v : r) v = -v;
    const double s = std::sqrt(r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    const double ang = 2.0 * std::atan2(s, r[0]);
    if (s < 1e-12) { w[0] = w[1] = w[2] = 0; return; }
    for (int i = 0; i < 3; ++i) w[i] = r[1 + i] / s * ang;
}
// exp(w) * q (w = world-space rotation vector).
inline void rotate_by(const double w[3], const double q[4], double o[4]) noexcept {
    const double ang = len3(w);
    if (ang < 1e-15) { for (int i = 0; i < 4; ++i) o[i] = q[i]; return; }
    const double s = std::sin(ang * 0.5) / ang;
    const double d[4] = {std::cos(ang * 0.5), w[0] * s, w[1] * s, w[2] * s};
    qmul(d, q, o); qnorm(o);
}
} // namespace detail

// B's 3x3 as rotation x per-axis scale: row i = s[i] x (unit row i), the unit rows orthonormal (right-handed after the sign of s[2] absorbs a mirror).
// q = the rotation as a quaternion of M = R^T (column convention: world axis j of local axis i = M[j][i]).
struct Decomp { double q[4] = {1, 0, 0, 0}; double s[3] = {1, 1, 1}; bool ok = false; };
inline bool decompose(const Xform& x, Decomp& d, double ortho_tol = 2e-3) noexcept {
    d = Decomp{};
    double u[9];
    for (int i = 0; i < 3; ++i) {
        const double l = detail::len3(&x.r[i * 3]);
        if (!(l > 1e-9) || !std::isfinite(l)) return false;
        d.s[i] = l; for (int c = 0; c < 3; ++c) u[i * 3 + c] = x.r[i * 3 + c] / l;
    }
    if (std::fabs(detail::dot3(&u[0], &u[3])) > ortho_tol || std::fabs(detail::dot3(&u[0], &u[6])) > ortho_tol || std::fabs(detail::dot3(&u[3], &u[6])) > ortho_tol)
        return false;   // shear: not a rotation x scale
    const double cz[3] = {u[1] * u[5] - u[2] * u[4], u[2] * u[3] - u[0] * u[5], u[0] * u[4] - u[1] * u[3]};
    if (detail::dot3(cz, &u[6]) < 0) { d.s[2] = -d.s[2]; for (int c = 0; c < 3; ++c) u[6 + c] = -u[6 + c]; }
    // Gram-Schmidt (keeps row 0): exact rotation for the quaternion
    { const double p = detail::dot3(&u[0], &u[3]); for (int c = 0; c < 3; ++c) u[3 + c] -= p * u[c]; const double l = detail::len3(&u[3]); for (int c = 0; c < 3; ++c) u[3 + c] /= l; }
    u[6] = u[1] * u[5] - u[2] * u[4]; u[7] = u[2] * u[3] - u[0] * u[5]; u[8] = u[0] * u[4] - u[1] * u[3];
    double m[9];   // M = U^T
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[j * 3 + i] = u[i * 3 + j];
    detail::quat_from_m(m, d.q);
    d.ok = true;
    return true;
}
inline void compose_rows(const Decomp& d, Xform& x) noexcept {
    double m[9]; detail::m_from_quat(d.q, m);
    for (int i = 0; i < 3; ++i) for (int c = 0; c < 3; ++c) x.r[i * 3 + c] = d.s[i] * m[c * 3 + i];
}

// ------------------------------------------------------------------ rigid camera delta (frame-time camera -> display-time camera)
// dR (column convention) maps the frame camera's axes onto the display camera's: axis_d = dR axis_f; dt = Cd - Cf.
struct CamDelta { bool identity = true; double cf[3] = {0, 0, 0}, dt[3] = {0, 0, 0}, dR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; };
inline CamDelta cam_delta(const pacing::Frame& f, const pacing::Frame& d) noexcept {
    CamDelta c;
    const float* af[3] = {f.right, f.up, f.fwd};
    const float* ad[3] = {d.right, d.up, d.fwd};
    // Axes bit-identical (translation-only extrapolation): dR is exactly I (no float-rounding pseudo-rotation).
    const bool same_axes = std::memcmp(f.right, d.right, sizeof f.right) == 0 && std::memcmp(f.up, d.up, sizeof f.up) == 0 && std::memcmp(f.fwd, d.fwd, sizeof f.fwd) == 0;
    if (!same_axes)
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { double s = 0; for (int k = 0; k < 3; ++k) s += double(ad[k][i]) * double(af[k][j]); c.dR[i * 3 + j] = s; }
    bool id = true;
    for (int i = 0; i < 3; ++i) { c.cf[i] = f.eye[i]; c.dt[i] = double(d.eye[i]) - double(f.eye[i]); if (c.dt[i] != 0.0) id = false; }
    if (!same_axes) id = false;
    c.identity = id;
    return c;
}
// The object as the display camera sees it, expressed for the FRAME camera: w' = Cf + dR^T (w - Cf - dt); rows r_i' = dR^T r_i.
inline void compensate(const CamDelta& c, Xform& x) noexcept {
    if (c.identity) return;
    auto mt = [&](const double v[3], double o[3]) { for (int i = 0; i < 3; ++i) o[i] = c.dR[0 * 3 + i] * v[0] + c.dR[1 * 3 + i] * v[1] + c.dR[2 * 3 + i] * v[2]; };
    for (int r = 0; r < 3; ++r) { double o[3]; mt(&x.r[r * 3], o); for (int k = 0; k < 3; ++k) x.r[r * 3 + k] = o[k]; }
    const double rel[3] = {x.t[0] - c.cf[0] - c.dt[0], x.t[1] - c.cf[1] - c.dt[1], x.t[2] - c.cf[2] - c.dt[2]};
    double o[3]; mt(rel, o);
    for (int k = 0; k < 3; ++k) x.t[k] = c.cf[k] + o[k];
}
// The consumer side: the terrain camera moved by the same delta (eye += dt, axes = dR axes). Float in/out (terrain::CamFrame layout).
inline void apply_to_camera(const float dt[3], const float dR[9], float eye[3], float right[3], float up[3], float fwd[3]) noexcept {
    float* ax[3] = {right, up, fwd};
    for (float* a : ax) {
        const float v[3] = {a[0], a[1], a[2]};
        for (int i = 0; i < 3; ++i) a[i] = dR[i * 3 + 0] * v[0] + dR[i * 3 + 1] * v[1] + dR[i * 3 + 2] * v[2];
    }
    for (int i = 0; i < 3; ++i) eye[i] += dt[i];
}

// ------------------------------------------------------------------ object identity tracking and evaluation
struct Config {
    double max_extrap_frames = 1.5;   // interpx: extrapolation clamp (game frames ahead of the newest state)
    double gate_min = 20.0;           // units: allowed prediction error of a matched object = gate_min + gate_vel x |v| (per frame)
    double gate_vel = 1.0;
    double max_step = 400.0;          // units per frame for an object with no velocity yet (first match) = the camera's cut threshold
    double max_rot_frame_deg = 30.0;  // a larger rotation in one frame is a teleport (pooled object reused / new pose), not motion
    double scale_tol = 0.02;          // relative change of a row scale: LOD / size animation -> not interpolated this frame
    double cos_min = 0.7, ratio_min = 0.5, ratio_max = 2.0;   // stable translation (no overshoot through reversals, stops, launches)
    double max_rot_extrap_deg = 10.0; // extrapolated rotation beyond the newest state
    bool attitude = true;
};
struct DrawKey { uint32_t sig = 0; uint64_t vb = 0, ib = 0; };
enum Kind : int { kind_untracked = 0, kind_appeared = 1, kind_matched = 2, kind_teleport = 3, kind_scale = 4 };
struct Result {
    Kind kind = kind_untracked;
    uint32_t id = 0;          // track id (0 = untracked)
    bool moved = false;       // out differs from the observed transform (object motion; the camera delta is separate)
    bool translated = false, rotated = false, clamped = false, interpolated = false;
    const char* why = "none"; // why the object was not moved (static text)
};
struct Track {
    uint32_t id = 0, ordinal = 0; DrawKey key{};
    Xform cur{}, prev{}; Decomp dcur{}, dprev{};
    bool has_prev = false, v_ok = false, vprev_ok = false, w_ok = false, wprev_ok = false, claimed = false;
    double v[3] = {0, 0, 0}, vprev[3] = {0, 0, 0}, w[3] = {0, 0, 0}, wprev[3] = {0, 0, 0};
};
struct TrackerCounters {
    uint64_t frames = 0, objects = 0, appeared = 0, matched = 0, teleports = 0, scale_changes = 0, lost = 0, untracked = 0, gaps = 0,
             moved = 0, interpolated = 0, extrapolated = 0, held = 0, rot_moved = 0, clamped = 0;
};
class ObjectTracker {
public:
    void configure(const Config& c) noexcept { c_ = c; }
    const Config& config() const noexcept { return c_; }
    // One producer frame (seq strictly increasing). A seq gap or a wall gap above 4 x the frame period drops the history (no stale velocity).
    void begin_frame(uint64_t seq, int64_t t_us) noexcept {
        for (unsigned i = 0; i < nprev_; ++i) if (!prev_[i].claimed) { ++n_.lost; ++frame_lost_; }
        last_frame_lost_ = frame_lost_; frame_lost_ = 0;
        Track* t = prev_; prev_ = cur_; cur_ = t; nprev_ = ncur_; ncur_ = 0;
        bool gap = have_seq_ && (seq != seq_ + 1);
        if (have_seq_ && period_us_ > 0 && t_us - t_us_ > int64_t(4.0 * period_us_)) gap = true;
        if (have_seq_ && t_us > t_us_) {
            const double dt = double(t_us - t_us_);
            if (!gap && seq == seq_ + 1) period_us_ = period_us_ > 0 ? (dt < 1.5 * period_us_ ? 0.9 * period_us_ + 0.1 * dt : period_us_ * 1.02) : dt;
        }
        if (gap || !have_seq_) { if (gap) ++n_.gaps; nprev_ = 0; }
        for (unsigned i = 0; i < nprev_; ++i) prev_[i].claimed = false;
        seq_ = seq; t_us_ = t_us; have_seq_ = true; ++n_.frames;
    }
    // k: game frames relative to the newest state: [-1, 0) interpolates between the previous and the newest state, 0 = the newest state,
    // > 0 extrapolates (clamped to max_extrap_frames). out = the transform to draw (= obs when nothing can be done).
    Result observe(const DrawKey& key, const Xform& obs, double k, Xform& out) noexcept {
        Result r; out = obs; ++n_.objects;
        if (ncur_ >= kCap) { ++n_.untracked; r.why = "cap"; return r; }
        uint32_t ordinal = 0;
        for (unsigned i = 0; i < ncur_; ++i) if (cur_[i].key.sig == key.sig) ++ordinal;
        // candidates: the previous frame's unclaimed tracks of the same mesh signature, within the gate around their prediction
        int best = -1; double best_score = 1e300;
        for (unsigned i = 0; i < nprev_; ++i) {
            const Track& p = prev_[i];
            if (p.claimed || p.key.sig != key.sig) continue;
            double pred[3]; for (int j = 0; j < 3; ++j) pred[j] = p.cur.t[j] + (p.v_ok ? p.v[j] : 0.0);
            const double e[3] = {obs.t[0] - pred[0], obs.t[1] - pred[1], obs.t[2] - pred[2]};
            const double err = detail::len3(e);
            const double allowed = p.v_ok ? c_.gate_min + c_.gate_vel * detail::len3(p.v) : c_.max_step;
            if (!(err <= allowed)) continue;
            const bool same_buf = key.vb && key.vb == p.key.vb && key.ib == p.key.ib;
            const double score = err * err + (same_buf ? 0.0 : 4.0) + (p.ordinal == ordinal ? 0.0 : 1.0);
            if (score < best_score) { best_score = score; best = int(i); }
        }
        bool teleport = false;
        if (best < 0) {   // pooled object reused: the same buffers and ordinal, far outside the gate
            for (unsigned i = 0; i < nprev_; ++i) {
                const Track& p = prev_[i];
                if (!p.claimed && p.key.sig == key.sig && key.vb && p.key.vb == key.vb && p.key.ib == key.ib && p.ordinal == ordinal) { best = int(i); teleport = true; break; }
            }
        }
        Track& n = cur_[ncur_++];
        n = Track{}; n.key = key; n.ordinal = ordinal; n.cur = obs;
        const bool dok = decompose(obs, n.dcur);
        if (best < 0) { n.id = ++next_id_; r.kind = kind_appeared; r.id = n.id; r.why = "appeared"; ++n_.appeared; return r; }
        Track& p = prev_[size_t(best)];
        p.claimed = true; n.id = p.id; r.id = n.id;
        if (teleport) { r.kind = kind_teleport; r.why = "teleport"; ++n_.teleports; return r; }
        // rotation and scale continuity
        double w[3] = {0, 0, 0}; bool rot_ok = dok && p.dcur.ok;
        if (rot_ok) {
            detail::rel_rotvec(p.dcur.q, n.dcur.q, w);
            if (detail::len3(w) > c_.max_rot_frame_deg * 0.017453292519943295) { r.kind = kind_teleport; r.why = "teleport-rotation"; ++n_.teleports; return r; }
            for (int i = 0; i < 3; ++i)
                if (std::fabs(n.dcur.s[i] - p.dcur.s[i]) > c_.scale_tol * std::fabs(p.dcur.s[i])) { r.kind = kind_scale; r.why = "scale-change"; ++n_.scale_changes; return r; }
        } else if (dok != p.dcur.ok) { r.kind = kind_scale; r.why = "shear-change"; ++n_.scale_changes; return r; }
        r.kind = kind_matched; ++n_.matched;
        n.prev = p.cur; n.dprev = p.dcur; n.has_prev = true;
        for (int i = 0; i < 3; ++i) { n.v[i] = obs.t[i] - p.cur.t[i]; n.vprev[i] = p.v[i]; }
        n.v_ok = true; n.vprev_ok = p.v_ok;
        if (rot_ok) { for (int i = 0; i < 3; ++i) { n.w[i] = w[i]; n.wprev[i] = p.w[i]; } n.w_ok = true; n.wprev_ok = p.w_ok; }
        evaluate(n, k, out, r);
        count(r);
        return r;
    }
    // A track of the CURRENT frame (by id) evaluated at another k (tests: several display times per game state). false = no such track.
    bool sample(uint32_t id, double k, Xform& out, Result& r) const noexcept {
        for (unsigned i = 0; i < ncur_; ++i)
            if (cur_[i].id == id) {
                r = Result{}; r.id = id; r.kind = cur_[i].has_prev ? kind_matched : kind_appeared;
                if (!cur_[i].has_prev) { out = cur_[i].cur; r.why = "no-history"; return true; }
                evaluate(cur_[i], k, out, r); return true;
            }
        return false;
    }
    const TrackerCounters& counters() const noexcept { return n_; }
    void reset_counters() noexcept { n_ = TrackerCounters{}; }
    unsigned tracked() const noexcept { return ncur_; }
    unsigned lost_last_frame() const noexcept { return last_frame_lost_; }
    double period_us() const noexcept { return period_us_; }
private:
    bool stable_translation(const Track& n) const noexcept {
        const double a = detail::len3(n.v), b = detail::len3(n.vprev);
        if (a < 1e-3 || b < 1e-3 || !n.vprev_ok) return false;
        const double cs = detail::dot3(n.v, n.vprev) / (a * b), ratio = a / b;
        return cs >= c_.cos_min && ratio >= c_.ratio_min && ratio <= c_.ratio_max;
    }
    bool stable_rotation(const Track& n) const noexcept {
        if (!n.w_ok || !n.wprev_ok) return false;
        const double d[3] = {n.w[0] - n.wprev[0], n.w[1] - n.wprev[1], n.w[2] - n.wprev[2]};
        return detail::len3(d) <= 0.5 * detail::len3(n.w) + 2e-4;
    }
    // Pure: the track's transform at k (no counters); r.kind / r.id stay as the caller set them.
    void evaluate(const Track& n, double k, Xform& out, Result& r) const noexcept {
        out = n.cur;
        if (!(k == k) || k == 0.0) { r.why = "k-zero"; return; }
        if (k < 0.0) {   // interpolation between the previous and the newest state (never overshoots)
            const double s = 1.0 + (k < -1.0 ? -1.0 : k);
            for (int i = 0; i < 3; ++i) out.t[i] = n.prev.t[i] + (n.cur.t[i] - n.prev.t[i]) * s;
            r.translated = true;
            if (n.w_ok && detail::len3(n.w) > 1e-12) {
                const double ws[3] = {n.w[0] * s, n.w[1] * s, n.w[2] * s};
                Decomp d = n.dcur; detail::rotate_by(ws, n.dprev.q, d.q); compose_rows(d, out); r.rotated = true;
            }
            r.moved = r.interpolated = true; r.why = "interpolated";
            return;
        }
        double kk = k;
        if (kk > c_.max_extrap_frames) { kk = c_.max_extrap_frames; r.clamped = true; }
        if (stable_translation(n)) { for (int i = 0; i < 3; ++i) out.t[i] = n.cur.t[i] + n.v[i] * kk; r.translated = true; }
        if (c_.attitude && stable_rotation(n)) {
            const double rate = detail::len3(n.w);
            if (rate > 1e-9) {
                double ang = rate * kk; const double lim = c_.max_rot_extrap_deg * 0.017453292519943295;
                if (ang > lim) { ang = lim; r.clamped = true; }
                const double ws[3] = {n.w[0] / rate * ang, n.w[1] / rate * ang, n.w[2] / rate * ang};
                Decomp d = n.dcur; detail::rotate_by(ws, n.dcur.q, d.q); compose_rows(d, out); r.rotated = true;
            }
        }
        if (r.translated || r.rotated) { r.moved = true; r.why = "extrapolated"; }
        else {
            const double a = detail::len3(n.v);
            r.why = !n.vprev_ok ? "warming-up" : a < 1e-3 ? "stopped" : "direction-change";
            r.clamped = false;
        }
    }
    void count(const Result& r) noexcept {
        if (r.moved) { ++n_.moved; if (r.interpolated) ++n_.interpolated; else ++n_.extrapolated; if (r.rotated) ++n_.rot_moved; if (r.clamped) ++n_.clamped; }
        else ++n_.held;
    }
    Config c_{};
    Track a_[kCap]{}, b_[kCap]{};
    Track* prev_ = a_; Track* cur_ = b_;
    unsigned nprev_ = 0, ncur_ = 0, frame_lost_ = 0, last_frame_lost_ = 0;
    uint64_t seq_ = 0; int64_t t_us_ = 0; bool have_seq_ = false; double period_us_ = 0;
    uint32_t next_id_ = 0;
    TrackerCounters n_{};
};

// ------------------------------------------------------------------ one producer frame: camera to t_display, objects to the same t_display
struct FramePlan {
    bool valid = false;        // the planner ran for this frame (interp armed, camera known)
    double k = 0;              // game frames ahead applied to the objects (0 = frame time)
    double horizon_ms = 0;     // wall horizon the camera was moved by (0 = not moved)
    CamDelta delta{};          // frame camera -> display camera
    bool cam_translated = false, cam_rotated = false;
    const char* why = "none";  // why k is 0 (static)
};
class FramePlanner {
public:
    void configure(const Config& oc, const pacing::ExtrapConfig& cc) noexcept { objects_.configure(oc); cam_.configure(cc); cc_ = cc; }
    // The frame's game camera (eye + axes, A-derived) at the frame's publication-time stamp, and the consumer's horizon (ms, < 0 unknown).
    FramePlan begin(uint64_t seq, int64_t t_us, const pacing::Frame& cam, double horizon_ms) noexcept {
        objects_.begin_frame(seq, t_us);
        cam_.update(seq, t_us, cam);
        FramePlan p; p.valid = true;
        plan_ = p;
        if (!(horizon_ms > 0.0)) { plan_.why = horizon_ms < 0 ? "no-horizon" : "zero-horizon"; return plan_; }
        const double fps = cam_.fps();
        if (!(fps > 1.0)) { plan_.why = "no-rate-yet"; return plan_; }
        double h = horizon_ms;
        const double hmax = objects_.config().max_extrap_frames * 1000.0 / fps;
        if (h > hmax) h = hmax;
        pacing::Frame out;
        const pacing::ExtrapResult r = cam_.predict(h, out);
        float v[3];
        const bool vok = cam_.velocity_per_s(v);
        const double per_frame = vok ? std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]) / fps : -1.0;
        const bool stationary = vok && per_frame < 0.5 && std::strcmp(r.why, "cut") != 0;
        if (!r.translated && !stationary) { plan_.why = r.why; return plan_; }   // the camera cannot follow: everything stays at frame time
        p.k = r.frames_ahead; p.horizon_ms = r.horizon_ms; p.cam_translated = r.translated; p.cam_rotated = r.rotated;
        if (r.translated || r.rotated) p.delta = cam_delta(cam, out);
        p.why = "ok";
        plan_ = p;
        return plan_;
    }
    // One replayed object of the current frame. out = the matrix to draw (object at t_display, seen by the display camera through the frame
    // camera), display_t = the object's world position at t_display (tactical markers). Returns true when out differs from obs (write it).
    bool object(const DrawKey& key, const Xform& obs, Xform& out, double display_t[3], Result& r) noexcept {
        r = objects_.observe(key, obs, plan_.valid ? plan_.k : 0.0, out);
        for (int i = 0; i < 3; ++i) display_t[i] = out.t[i];
        if (plan_.valid && !plan_.delta.identity) { compensate(plan_.delta, out); return true; }
        return r.moved;
    }
    const FramePlan& plan() const noexcept { return plan_; }
    const ObjectTracker& tracker() const noexcept { return objects_; }
    ObjectTracker& tracker() noexcept { return objects_; }
    double fps() const noexcept { return cam_.fps(); }
private:
    ObjectTracker objects_{};
    pacing::MotionExtrap cam_{};
    pacing::ExtrapConfig cc_{};
    FramePlan plan_{};
};

// ------------------------------------------------------------------ helpers for k from display times (tests, docs)
// The display time relative to the last two game states stamped t_prev, t_cur (same clock): k = (t_display - t_cur) / (t_cur - t_prev),
// i.e. -1 at the previous state, 0 at the newest, > 0 beyond it. period_us <= 0: nan.
inline double frames_from_times(double t_display, double t_prev, double t_cur) noexcept {
    const double p = t_cur - t_prev;
    return p > 0 ? (t_display - t_cur) / p : std::nan("");
}

} // namespace acz::interp
