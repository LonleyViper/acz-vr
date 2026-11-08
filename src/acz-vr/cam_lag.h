#pragma once
// cam_lag.h -- CAM-EPOCH diagnostic (Astra P2 "the sampling phase/lag is unproven"): measures, over a session, how far the E6 camera sample a
// frame was composed with is from the camera the game actually drew that frame with (the frame's own cockpit camera axes + DrawACM_v eye,
// StereoFrame::game_cam_* / game_eye). No decision reads it. Header only, SDK-free.
//
// Per window it reports (summary-camera-lag):
//   * offset 0 (the frame's own E6 sample vs the frame's game camera): rotation angle mean/max (deg), eye position difference mean/max (units),
//     the sample's age relative to the frame's publication (ms);
//   * best per-frame offset: the game camera of frame k is compared with the E6 samples of the neighbouring CONSUMED frames k-2..k+2 (by frame id)
//     and the offset with the smallest angle is counted (+1 = the next frame's E6 sample matches the draws better: E6 at seam B runs AHEAD of the
//     draws; -1 = behind). Evaluated one frame late (k+1, k+2 must have been consumed); missing neighbours are skipped;
//   * best Present-history offset: the game camera vs the Present-tick publication nearest to (recording start + d), d = -48..+48 ms in 16 ms steps,
//     counted per best d (NaN entries = no publication within 8 ms of that time).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace acz::terrain {

struct CamBasis { float right[3]{}, up[3]{}, fwd[3]{}, eye[3]{}; };

// Rotation angle between two orthonormal bases (degrees) of Q = A B^T (rows right/up/fwd): atan2(|skew(Q)|/2, (trace(Q) - 1)/2), which stays well
// conditioned near 0 (acos of the trace alone loses ~0.03 deg to float rounding).
inline float basis_angle_deg(const CamBasis& a, const CamBasis& b) noexcept {
    const float* ar[3] = {a.right, a.up, a.fwd};
    const float* br[3] = {b.right, b.up, b.fwd};
    double q[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q[i][j] = double(ar[i][0]) * br[j][0] + double(ar[i][1]) * br[j][1] + double(ar[i][2]) * br[j][2];
    const double w0 = q[2][1] - q[1][2], w1 = q[0][2] - q[2][0], w2 = q[1][0] - q[0][1];
    return float(std::atan2(0.5 * std::sqrt(w0 * w0 + w1 * w1 + w2 * w2), (q[0][0] + q[1][1] + q[2][2] - 1.0) * 0.5) * 57.29577951308232);
}
inline float eye_distance(const CamBasis& a, const CamBasis& b) noexcept {
    const double d[3] = {double(a.eye[0]) - b.eye[0], double(a.eye[1]) - b.eye[1], double(a.eye[2]) - b.eye[2]};
    return float(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
}

class CamLagMeter {
public:
    static constexpr int kSpan = 2;
    static constexpr int kPresent = 7;
    static constexpr int64_t kPresentStepNs = 16000000, kPresentTolNs = 8000000;
    static int64_t present_offset_ns(int i) noexcept { return (int64_t(i) - kPresent / 2) * kPresentStepNs; }

    // One composed frame. e6_ok = the E6 sample this frame was composed with (admitted); age_ms = frame publication - sample time.
    // Frames are kept in arrival order (the consumer skips ids when it takes the latest frame); a frame is evaluated once its id is kSpan
    // behind the newest one, or when it leaves the history.
    void frame(uint64_t id, bool game_ok, const CamBasis& game, bool game_eye, bool e6_ok, const CamBasis& e6, double age_ms) noexcept {
        if (!id) return;
        if (e6_ok && std::isfinite(age_ms)) { ++w_.aged; w_.age_sum += age_ms; if (age_ms > w_.age_max) w_.age_max = age_ms; }
        if (count_ == kRing) { evaluate(ring_[head_]); }   // the oldest leaves now
        Obs& o = ring_[head_];
        o = Obs{}; o.id = id; o.game_ok = game_ok; o.game = game; o.game_eye = game_eye; o.e6_ok = e6_ok; o.e6 = e6;
        head_ = (head_ + 1) % kRing; if (count_ < kRing) ++count_;
        for (unsigned i = 0; i < kRing; ++i) if (ring_[i].id && !ring_[i].evaluated && ring_[i].id + uint64_t(kSpan) <= id) evaluate(ring_[i]);
    }
    // The Present-history angles for the CURRENT frame (game camera vs the publication nearest to start + present_offset_ns(i)); NaN = none.
    void present(const float angle_deg[kPresent]) noexcept {
        int best = -1; float ba = 0;
        for (int i = 0; i < kPresent; ++i) {
            if (!std::isfinite(angle_deg[i])) { ++w_.present_missing[i]; continue; }
            w_.present_sum[i] += angle_deg[i]; ++w_.present_n[i];
            if (best < 0 || angle_deg[i] < ba) { best = i; ba = angle_deg[i]; }
        }
        if (best >= 0) ++w_.present_best[best];
    }
    // Totals since construction (tests) and the window line (reset afterwards). false = nothing measured in this window.
    uint64_t total_evaluated() const noexcept { return total_eval_; }
    uint64_t best_count(int offset) const noexcept { return offset >= -kSpan && offset <= kSpan ? w_.best[offset + kSpan] : 0; }
    uint64_t present_best_count(int i) const noexcept { return i >= 0 && i < kPresent ? w_.present_best[i] : 0; }
    double angle0_mean() const noexcept { return w_.n0 ? w_.ang_sum / double(w_.n0) : 0; }
    double angle0_max() const noexcept { return w_.ang_max; }
    double pos0_max() const noexcept { return w_.pos_max; }
    bool line(char* buf, size_t cap, long long t_ms) noexcept {
        if (!buf || !cap) return false;
        if (!w_.n0 && !w_.aged && !any_present()) return false;
        int n = std::snprintf(buf, cap, "xr-stereo;event=summary-camera-lag;t-ms=%lld;frames=%llu;angle-deg-mean=%.3f;angle-deg-max=%.3f;pos-n=%llu;pos-u-mean=%.2f;pos-u-max=%.2f;"
            "e6-age-ms-mean=%.1f;e6-age-ms-max=%.1f;best-frame-offset=-2:%llu,-1:%llu,0:%llu,+1:%llu,+2:%llu;present-best-ms=",
            t_ms, ull(w_.n0), angle0_mean(), w_.ang_max, ull(w_.npos), w_.npos ? w_.pos_sum / double(w_.npos) : 0.0, w_.pos_max,
            w_.aged ? w_.age_sum / double(w_.aged) : 0.0, w_.age_max, ull(w_.best[0]), ull(w_.best[1]), ull(w_.best[2]), ull(w_.best[3]), ull(w_.best[4]));
        for (int i = 0; i < kPresent && n > 0 && size_t(n) < cap; ++i)
            n += std::snprintf(buf + n, cap - size_t(n), "%s%+lld:%llu", i ? "," : "", (long long)(present_offset_ns(i) / 1000000), ull(w_.present_best[i]));
        if (n > 0 && size_t(n) < cap) std::snprintf(buf + n, cap - size_t(n), ";evidence=diagnostic-only;phase=unproven-until-a-live-run");
        w_ = Window{};
        return true;
    }
private:
    static unsigned long long ull(uint64_t v) noexcept { return static_cast<unsigned long long>(v); }
    static constexpr unsigned kRing = 8;
    struct Obs { uint64_t id = 0; bool game_ok = false, game_eye = false, e6_ok = false, evaluated = false; CamBasis game{}, e6{}; };
    struct Window {
        uint64_t n0 = 0, npos = 0, aged = 0, best[2 * kSpan + 1]{}, present_best[kPresent]{}, present_n[kPresent]{}, present_missing[kPresent]{};
        double ang_sum = 0, ang_max = 0, pos_sum = 0, pos_max = 0, age_sum = 0, age_max = 0, present_sum[kPresent]{};
    };
    bool any_present() const noexcept { for (int i = 0; i < kPresent; ++i) if (w_.present_n[i]) return true; return false; }
    const Obs* at(uint64_t id) const noexcept { for (unsigned i = 0; i < kRing; ++i) if (ring_[i].id == id && id) return &ring_[i]; return nullptr; }
    void evaluate(Obs& o) noexcept {
        if (!o.id || o.evaluated) return;
        o.evaluated = true;
        if (!o.game_ok || !o.e6_ok) return;
        ++total_eval_;
        const float a0 = basis_angle_deg(o.game, o.e6);
        ++w_.n0; w_.ang_sum += a0; if (a0 > w_.ang_max) w_.ang_max = a0;
        if (o.game_eye) { const float d = eye_distance(o.game, o.e6); ++w_.npos; w_.pos_sum += d; if (d > w_.pos_max) w_.pos_max = d; }
        int best = 0; float ba = a0;
        for (int k = -kSpan; k <= kSpan; ++k) {
            if (!k || (k < 0 && o.id <= uint64_t(-k))) continue;
            const Obs* n = at(o.id + uint64_t(int64_t(k)));
            if (!n || !n->e6_ok) continue;
            const float a = basis_angle_deg(o.game, n->e6);
            if (a < ba) { ba = a; best = k; }
        }
        ++w_.best[best + kSpan];
    }
    Obs ring_[kRing];
    unsigned head_ = 0, count_ = 0;
    Window w_;
    uint64_t total_eval_ = 0;
};

} // namespace acz::terrain
