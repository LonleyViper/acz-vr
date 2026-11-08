#pragma once
// terrain_camera.h -- one-writer seqlock mailbox carrying the E6 camera (read-only game memory, ee_camera_probe_win.cpp Present tick) to the
// terrain renderer in the XR consumer (xr owner thread). Header only, no Windows types.
//   words: [0..2] campos (E6 +0xFDD860..68), [3..5] rot = pitch, yaw, roll (E6 +0xFDD870..78), [6] E6 heading (+0xFDD448).
// The yaw used by the renderer is rot[1] (kE6YawField), NOT a separate field (calibration: the first logged 'yaw' field is garbage).
//
// CAM-EPOCH (Astra P2, docs/world-render/e6-anchor-free.md section 10): every published sample is an immutable snapshot that also carries
//   t_ns         steady_clock nanoseconds at publication (the same clock domain as the producer's diag_start_us/diag_publish_us),
//   seq          the mailbox's publication number,
//   lock_epoch   the E6 locator's lock epoch of the READ (captured before it, read_publish; increments on every lock, loss and relock),
//   stage_epoch  the stage epoch of the READ (increments on a stage install/re-detection and on an E6 position discontinuity),
//   tag          the producer recording generation the sample was taken for (frame mailbox; 0 = not frame-bound, Present tick),
//   t_read_ns    steady_clock nanoseconds when the producer STARTED reading the E6 bytes (t_ns = publication, after the read; 0 = not measured).
// Readers admit a sample only through admit_sample()/CamAdmitter (age, epochs, finite/sane, correlation); a refused sample is the explicit
// no-camera path, never "the newest one instead". A small history ring keeps the last kHist publications for a time-bounded fallback.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::terrain {

inline int64_t cam_clock_ns() noexcept {
    return int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A coherent source identity captured BEFORE a read (CAM-EPOCH review 2, Astra P2 "epochs attached after the read"): the locked source base and its
// lock epoch (one seqlock pair, gen even = stable), plus the stage epoch at the same moment.
struct CamSourceId { uint64_t base = 0; uint32_t lock = 0, stage = 0, gen = 0; };

// The CURRENT epochs. lock + locked base: written together by the Present tick (one writer, set_source); stage: bumped by anyone (stage install,
// producer discontinuity). All seq_cst: the seqlock below needs no further fences.
class CamEpochs {
public:
    uint32_t lock() const noexcept { return lock_.load(); }
    uint32_t stage() const noexcept { return stage_.load(); }
    uint64_t base() const noexcept { return base_.load(); }
    // One writer. The base and the lock epoch change as ONE transition (a reader never pairs the new base with the old epoch or vice versa).
    void set_source(uint32_t lock, uint64_t base) noexcept {
        gen_.fetch_add(1);   // odd: in progress
        base_.store(base); lock_.store(lock);
        gen_.fetch_add(1);   // even: stable
    }
    void set_lock(uint32_t e) noexcept { set_source(e, base_.load()); }
    uint32_t bump_stage() noexcept { return stage_.fetch_add(1) + 1; }
    // false: a source transition is in progress (the caller does not read now).
    bool source(CamSourceId& id) const noexcept {
        const uint32_t g = gen_.load();
        if (g & 1u) return false;
        CamSourceId c; c.base = base_.load(); c.lock = lock_.load(); c.stage = stage_.load(); c.gen = g;
        if (gen_.load() != g) return false;
        id = c; return true;
    }
    // true: no lock/base transition and no stage change since `id` was captured.
    bool unchanged(const CamSourceId& id) const noexcept { return gen_.load() == id.gen && stage_.load() == id.stage; }
private:
    std::atomic<uint32_t> lock_{0}, stage_{0}, gen_{0};
    std::atomic<uint64_t> base_{0};
};
inline CamEpochs& cam_epochs() noexcept { static CamEpochs e; return e; }

struct CamSample {
    float campos[3]{}, pitch = 0, yaw = 0, roll = 0, heading = 0;
    uint64_t seq = 0;
    int64_t t_ns = 0;
    uint32_t lock_epoch = 0, stage_epoch = 0;
    uint64_t tag = 0;
    int64_t t_read_ns = 0;
};

class CamMailbox {
public:
    static constexpr unsigned kHist = 32;   // ~0.5 s of Present ticks at 60 Hz
    explicit CamMailbox(CamEpochs* epochs = nullptr) noexcept : ep_(epochs) {}
    // Recording generation stamped into the following publications (frame mailbox: set by the producer before the seam-B read; same thread).
    void set_tag(uint64_t tag) noexcept { tag_.store(tag, std::memory_order_relaxed); }
    // t_ns = 0: stamped with cam_clock_ns() now (tests pass explicit times). The epochs are the CURRENT ones of the epoch source (tests and
    // single-thread callers only: a producer that reads game memory publishes through read_publish(), which stamps the identity captured BEFORE its read).
    // t_read_ns: when the producer started reading the bytes (0 = unknown).
    void publish(const float campos[3], float pitch, float yaw, float roll, float heading, int64_t t_ns = 0, int64_t t_read_ns = 0) noexcept {
        const CamEpochs& ep = ep_ ? *ep_ : cam_epochs();
        publish_epochs(ep.lock(), ep.stage(), campos, pitch, yaw, roll, heading, t_ns, t_read_ns);
    }
    // The same with EXPLICIT epochs (the identity the producer captured for its read).
    void publish_epochs(uint32_t lock_epoch, uint32_t stage_epoch, const float campos[3], float pitch, float yaw, float roll, float heading, int64_t t_ns = 0,
                        int64_t t_read_ns = 0) noexcept {
        uint32_t w[kWords];
        const float v[7] = {campos[0], campos[1], campos[2], pitch, yaw, roll, heading};
        for (int i = 0; i < 7; ++i) std::memcpy(&w[i], &v[i], 4);
        const int64_t t = t_ns ? t_ns : cam_clock_ns();
        const uint64_t tag = tag_.load(std::memory_order_relaxed);
        put64(w, 7, uint64_t(t)); w[9] = lock_epoch; w[10] = stage_epoch; put64(w, 11, tag); w[13] = 1u; put64(w, 14, uint64_t(t_read_ns));
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < kWords; ++i) w_[i].store(w[i], std::memory_order_relaxed);
        valid_.store(1u, std::memory_order_relaxed);
        const uint64_t n = n_.load(std::memory_order_relaxed) + 1;
        n_.store(n, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
        ring_put(w, n);
    }
    // E6-ANCHOR-FREE (ee_e6_locator.h): withdraw the published sample; try_read fails until the next publish (the consumer then counts
    // no-camera instead of rendering a stale or rejected camera). Single writer, like publish. Never called by the e6loc:probe feed.
    // CAM-EPOCH: also records an "invalidated at t" marker in the history (a fallback lookup landing on it is refused).
    void invalidate(int64_t t_ns = 0) noexcept {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        valid_.store(0u, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
        const CamEpochs& ep = ep_ ? *ep_ : cam_epochs();
        uint32_t w[kWords]{};
        put64(w, 7, uint64_t(t_ns ? t_ns : cam_clock_ns())); w[9] = ep.lock(); w[10] = ep.stage(); w[13] = 0u;
        ring_put(w, 0);
    }
    // false: nothing published yet, a publication in progress (try-only), or non-finite values.
    bool try_read(CamSample& out) const noexcept {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 == 0 || (s1 & 1u)) return false;
        uint32_t w[kWords];
        for (unsigned i = 0; i < kWords; ++i) w[i] = w_[i].load(std::memory_order_relaxed);
        const uint64_t n = n_.load(std::memory_order_relaxed);
        const uint32_t valid = valid_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        if (!valid) return false;
        CamSample c;
        if (!decode(w, c)) return false;
        c.seq = n;
        out = c; return true;
    }
    uint64_t published() const noexcept { return n_.load(std::memory_order_relaxed); }
    // CAM-EPOCH diagnostics: the retained history in publication order (oldest first; torn slots skipped). usable[i] = false for an invalidation
    // marker or a non-finite publication (out[i].t_ns still set). Returns the count; `ring_index` (optional) = the newest ring entry number.
    unsigned history(CamSample* out, bool* usable, unsigned cap, uint64_t* ring_index = nullptr) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (ring_index) *ring_index = head;
        if (!head || !out || !usable || !cap) return 0;
        const uint64_t first = head > kHist ? head - kHist + 1 : 1;
        unsigned k = 0;
        for (uint64_t r = first; r <= head && k < cap; ++r) {
            const Slot& sl = ring_[r % kHist];
            const uint64_t s1 = sl.seq.load(std::memory_order_acquire);
            if (s1 != 2 * r) continue;
            uint32_t w[kWords]; for (unsigned i = 0; i < kWords; ++i) w[i] = sl.w[i].load(std::memory_order_relaxed);
            const uint64_t pn = sl.n.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sl.seq.load(std::memory_order_relaxed) != s1) continue;
            CamSample c; const bool u = w[13] != 0u && decode(w, c);
            if (!u) { c = CamSample{}; c.t_ns = int64_t(get64(w, 7)); c.lock_epoch = w[9]; c.stage_epoch = w[10]; }
            c.seq = pn; out[k] = c; usable[k] = u; ++k;
        }
        return k;
    }

    // ---- CAM-EPOCH history: the publication (or invalidation marker) nearest to target_ns.
    enum class Find : uint8_t { ok, empty, missing, overwritten, invalidated };
    // ok: |t - target| <= window_ns and the entry is a sample (out filled, out.seq = its publication number). invalidated: the nearest entry within
    // the window is an invalidation marker. overwritten: target lies before the oldest retained entry by more than the window (the ring no longer
    // reaches back). missing: no entry within the window although the ring covers the target. Torn slots (overwritten while read) are skipped.
    Find nearest(int64_t target_ns, int64_t window_ns, CamSample& out) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (!head) return Find::empty;
        const uint64_t first = head > kHist ? head - kHist + 1 : 1;
        bool any = false, best_valid = false; int64_t best_d = 0, oldest = 0; CamSample best{};
        for (uint64_t r = first; r <= head; ++r) {
            const Slot& sl = ring_[r % kHist];
            const uint64_t s1 = sl.seq.load(std::memory_order_acquire);
            if (s1 != 2 * r) continue;
            uint32_t w[kWords]; for (unsigned i = 0; i < kWords; ++i) w[i] = sl.w[i].load(std::memory_order_relaxed);
            const uint64_t pn = sl.n.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sl.seq.load(std::memory_order_relaxed) != s1) continue;
            const int64_t t = int64_t(get64(w, 7));
            if (!any || t < oldest) oldest = t;
            const int64_t d = t > target_ns ? t - target_ns : target_ns - t;
            CamSample c; const bool usable = w[13] != 0u && decode(w, c);   // markers and non-finite publications are not usable samples
            if (!any || d < best_d || (d == best_d && !usable)) {          // a tie prefers the unusable entry (conservative)
                best_d = d; best_valid = usable; best = c; best.seq = pn;
            }
            any = true;
        }
        if (!any) return Find::empty;
        if (best_d <= window_ns) { if (!best_valid) return Find::invalidated; out = best; return Find::ok; }
        if (target_ns < oldest - window_ns && head > kHist) return Find::overwritten;
        return Find::missing;
    }
private:
    static constexpr unsigned kWords = 16;   // 7 floats, t (2), lock, stage, tag (2), valid, read-start t (2)
    static void put64(uint32_t* w, unsigned i, uint64_t v) noexcept { w[i] = uint32_t(v); w[i + 1] = uint32_t(v >> 32); }
    static uint64_t get64(const uint32_t* w, unsigned i) noexcept { return uint64_t(w[i]) | (uint64_t(w[i + 1]) << 32); }
    static bool decode(const uint32_t* w, CamSample& c) noexcept {
        float v[7];
        for (int i = 0; i < 7; ++i) std::memcpy(&v[i], &w[i], 4);
        for (float x : v) if (!std::isfinite(x)) return false;
        for (int i = 0; i < 3; ++i) c.campos[i] = v[i];
        c.pitch = v[3]; c.yaw = v[4]; c.roll = v[5]; c.heading = v[6];
        c.t_ns = int64_t(get64(w, 7)); c.lock_epoch = w[9]; c.stage_epoch = w[10]; c.tag = get64(w, 11); c.t_read_ns = int64_t(get64(w, 14));
        return true;
    }
    void ring_put(const uint32_t* w, uint64_t pub_n) noexcept {
        const uint64_t r = rn_ + 1; rn_ = r;
        Slot& sl = ring_[r % kHist];
        sl.seq.store(2 * r - 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (unsigned i = 0; i < kWords; ++i) sl.w[i].store(w[i], std::memory_order_relaxed);
        sl.n.store(pub_n, std::memory_order_relaxed);
        sl.seq.store(2 * r, std::memory_order_release);
        head_.store(r, std::memory_order_release);
    }
    struct Slot { std::atomic<uint64_t> seq{0}; std::atomic<uint32_t> w[kWords]{}; std::atomic<uint64_t> n{0}; };
    CamEpochs* ep_ = nullptr;
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> w_[kWords]{};
    std::atomic<uint32_t> valid_{0};
    std::atomic<uint64_t> n_{0};
    std::atomic<uint64_t> tag_{0};
    uint64_t rn_ = 0;                      // writer-only ring counter
    std::atomic<uint64_t> head_{0};
    Slot ring_[kHist];
};
// Process-wide instance (inline: one definition across the DLL and the tests).
inline CamMailbox& cam_mailbox() noexcept { static CamMailbox m; return m; }
// WORLD-1: the FRAME-SYNCHRONOUS sample. Single writer = the producer's first-member seam (ee_camera_probe seam B, called under the step-1 mutex while the
// game records the frame's draws); the producer copies a sample published during that call into the frame (StereoFrame::e6_*), so the consumer composes
// each producer frame with the E6 camera of that same frame instead of the newest Present-tick sample (test 16: 1-3 frames newer than the objects).
inline CamMailbox& frame_cam_mailbox() noexcept { static CamMailbox m; return m; }

// CAM-EPOCH read protocol (review 2, Astra P2): a reader thread that is NOT the lock writer captures a coherent source identity (base + lock epoch +
// stage epoch) BEFORE reading, reads from THAT base, refuses the sample when any transition happened during the read, and publishes the CAPTURED
// identity (never the epochs current at publication). A transition after the check therefore leaves the sample stamped with the old identity, which
// admission refuses. `read(base, v)` fills v = campos[3], pitch, yaw, roll, heading and returns false when there is nothing valid to publish.
// `before_publish` (tests only) runs between the check and the publication.
enum class CamRead : uint8_t { published = 0, busy, no_sample, transition };
struct CamReadNoHook { void operator()() const noexcept {} };
template <class ReadFn, class Hook = CamReadNoHook>
inline CamRead read_publish(CamMailbox& mb, const CamEpochs& ep, ReadFn&& read, Hook&& before_publish = Hook{}) noexcept {
    CamSourceId id;
    if (!ep.source(id)) return CamRead::busy;
    const int64_t t_read = cam_clock_ns();
    float v[7]{};
    if (!read(id.base, v)) return CamRead::no_sample;
    if (!ep.unchanged(id)) return CamRead::transition;
    before_publish();
    mb.publish_epochs(id.lock, id.stage, v, v[3], v[4], v[5], v[6], 0, t_read);
    return CamRead::published;
}

// W15 F1: the qualified FALLBACK base (the probe base used when the locator is searching, e6loc:auto) is part of the source identity too. It is not in
// CamSourceId (the locked base is 0 while it is used), so every change of it (withdrawal, requalification, base replacement, and a same-base
// withdraw/requalify ABA) advances the STAGE epoch, which every reader already captures before its read and checks after it (read_publish) and which
// admission compares against the sample's stamped stage. Order: the new base is stored FIRST, the epoch bumped AFTER: a reader that captured its identity
// before the bump either sees the old base (and is refused by the bump: transition / stage mismatch) or the new one; a reader that captured after the bump
// sees the final base. A frame read in flight during a withdrawal, and a sample published just before it, are refused (never published or admitted).
// Single writer (the Present tick). Returns true when the base changed.
inline bool set_fallback_base(CamEpochs& ep, std::atomic<uint64_t>& fb, uint64_t qualified_base) noexcept {
    const uint64_t prev = fb.exchange(qualified_base, std::memory_order_acq_rel);
    if (prev == qualified_base) return false;
    ep.bump_stage();
    return true;
}

// ------------------------------------------------------------------------------------------------ CAM-EPOCH admission
enum class Admit : uint8_t { ok = 0, none, nonfinite, insane, untimed, lock_epoch, stage_epoch, stale, future, uncorrelated, missing, overwritten, invalidated, count };
inline const char* admit_name(Admit a) noexcept {
    static const char* const n[] = {"ok", "none", "nonfinite", "insane", "untimed", "lock-epoch-mismatch", "stage-epoch-mismatch", "stale", "future",
                                    "uncorrelated", "fallback-missing", "fallback-overwritten", "fallback-invalidated"};
    const unsigned i = unsigned(a);
    return i < unsigned(Admit::count) ? n[i] : "?";
}
struct AdmitLimits {
    int64_t max_age_ns = 250000000;          // live limit (option camage:<ms>, 20..2000): sample older than this relative to the reference -> stale
    int64_t max_future_ns = 20000000;        // a sample this much NEWER than the reference (clock skew between threads is < 1 ms) -> future
    int64_t fallback_window_ns = 50000000;   // option camwin:<ms> (0 = no fallback): the Present-history sample must lie within this of the frame's recording start
    float pos_limit = 600000.0f;             // |campos| sanity (stage 262,144 + margin; the locator itself allows 294,912)
    float angle_limit = 6.3f;                // |pitch|,|yaw|,|roll| <= pi (+slack); heading <= 2 pi
};
// One sample against the CURRENT epochs and a reference time (the frame's publication time for per-frame samples, now for live samples).
inline Admit admit_sample(const CamSample& s, int64_t ref_ns, uint32_t lock_epoch, uint32_t stage_epoch, const AdmitLimits& l) noexcept {
    const float v[7] = {s.campos[0], s.campos[1], s.campos[2], s.pitch, s.yaw, s.roll, s.heading};
    for (float x : v) if (!std::isfinite(x)) return Admit::nonfinite;
    for (int i = 0; i < 3; ++i) if (std::fabs(s.campos[i]) > l.pos_limit) return Admit::insane;
    if (std::fabs(s.pitch) > l.angle_limit || std::fabs(s.yaw) > l.angle_limit || std::fabs(s.roll) > l.angle_limit || std::fabs(s.heading) > 2.0f * l.angle_limit)
        return Admit::insane;
    if (s.t_ns == 0) return Admit::untimed;
    if (s.lock_epoch != lock_epoch) return Admit::lock_epoch;
    if (s.stage_epoch != stage_epoch) return Admit::stage_epoch;
    if (ref_ns - s.t_ns > l.max_age_ns) return Admit::stale;
    if (s.t_ns - ref_ns > l.max_future_ns) return Admit::future;
    return Admit::ok;
}

// The consumer's per-frame selection. Inputs of one producer frame; times in ns of the steady clock (0 = unknown).
// The camera a composed frame actually used (recorded in the frame's snapshot and counted). Only correlated_hybrid pairs the draw's own camera with
// the E6 sample of the SAME frame (cross-checked); every other mode is an explicitly uncorrelated display fallback with a limited contract.
enum class CamMode : uint8_t { none = 0, correlated_hybrid, game_only, e6_frame, e6_game_refused, e6_fallback, e6_latest, fixed, count };
inline const char* cam_mode_name(CamMode m) noexcept {
    static const char* const n[] = {"none", "correlated-hybrid", "game-camera-only", "e6-frame", "e6-only-game-refused", "e6-present-history", "e6-latest", "fixed"};
    const unsigned i = unsigned(m);
    return i < unsigned(CamMode::count) ? n[i] : "?";
}
// Correlation-dependent replacement (terrain occluding replayed objects) needs the terrain camera to BE the camera the objects were drawn with in the same
// frame: the game camera of the frame (hybrid, or game-only with its own DrawACM_v eye). E6-only modes are display fallbacks: refused.
inline bool cam_mode_registers_objects(CamMode m) noexcept { return m == CamMode::correlated_hybrid || m == CamMode::game_only || m == CamMode::fixed; }

// E6 PLAUSIBILITY (2026-10-30, headset session step133 m03k): the E6 sample is the SAME aircraft as the game camera's eye, so in every healthy frame the two eyes
// coincide (measured: 73 hybrid frames, distance 0.00-0.07 units). After the E6 block went stale (a freed block reading 2,2,2 / 2 rad, constant for 470 s, also
// `aircraft-at-origin` in e6-locate) the sample still passed the sanity limits, the angle cross-check refused the moving game camera 2400 times
// (game-vs-e6 144-179 deg, eyes 125,000-165,000 units apart) and the world was drawn from a camera BURIED 2,639 units inside the m03k map corner = a uniform
// (9,9,13) image. An E6 sample whose eye is farther than this from the game camera's eye cannot be the game camera's aircraft: it must not veto the game camera.
inline constexpr float kE6EyeAgreeUnits = 2000.0f;
inline bool e6_eye_agrees(const float game_eye[3], const float e6_eye[3], float limit = kE6EyeAgreeUnits) noexcept {
    double s = 0.0;
    for (int i = 0; i < 3; ++i) { const double d = double(game_eye[i]) - double(e6_eye[i]); s += d * d; }
    return std::isfinite(s) && s <= double(limit) * double(limit);
}
// W15 F2: gross eye disagreement for ANY admitted E6 sample source (frame, Present history, Present latest), independent of whether the sample is
// same-frame eligible for the gmax camera veto (e6_pair). It needs an admitted E6 sample and the frame's own trustworthy game eye; without a game eye there
// is nothing to compare against (false). The consumer uses it to keep a distant E6 sample out of the cockpit animation state.
inline bool e6_eye_far(bool e6_admitted, bool game_eye_valid, const float game_eye[3], const float e6_eye[3]) noexcept {
    return e6_admitted && game_eye_valid && !e6_eye_agrees(game_eye, e6_eye);
}

struct FrameCam {
    bool valid = false;            // the frame carries its own sample (StereoFrame::e6_valid)
    CamSample s{};
    uint64_t expected_tag = 0;     // the frame's recording generation (0 = do not check)
    int64_t start_ns = 0, publish_ns = 0;
};
enum class CamSource : uint8_t { none = 0, frame, fallback, latest };
inline const char* cam_source_name(CamSource s) noexcept {
    switch (s) { case CamSource::frame: return "frame"; case CamSource::fallback: return "present-history"; case CamSource::latest: return "present-latest"; default: return "none"; }
}
struct CamPick { bool ok = false; CamSample s{}; CamSource src = CamSource::none; Admit why = Admit::none; Admit frame_why = Admit::none; };
inline Admit find_admit(CamMailbox::Find f) noexcept {
    switch (f) { case CamMailbox::Find::ok: return Admit::ok; case CamMailbox::Find::overwritten: return Admit::overwritten;
                 case CamMailbox::Find::invalidated: return Admit::invalidated; case CamMailbox::Find::missing: return Admit::missing; default: return Admit::none; }
}
// use_frame = e6:frame. Per-frame sample: age relative to the frame's publication time; tag must equal the recording generation. Missing or refused:
// the bounded fallback = the Present-history publication nearest to the frame's recording start within fallback_window_ns (epochs checked again),
// else refused. e6:latest: the newest Present sample, age relative to now. NEVER an unbounded "newest" substitution.
inline CamPick pick_camera(const FrameCam& fc, bool use_frame, const CamMailbox& present, uint32_t lock_epoch, uint32_t stage_epoch, int64_t now_ns,
                           const AdmitLimits& l) noexcept {
    CamPick p;
    if (use_frame) {
        if (fc.valid) {
            const int64_t ref = fc.publish_ns ? fc.publish_ns : now_ns;
            Admit a = admit_sample(fc.s, ref, lock_epoch, stage_epoch, l);
            if (a == Admit::ok && fc.expected_tag && fc.s.tag != fc.expected_tag) a = Admit::uncorrelated;
            p.frame_why = a;
            if (a == Admit::ok) { p.ok = true; p.s = fc.s; p.src = CamSource::frame; p.why = Admit::ok; return p; }
        } else p.frame_why = Admit::none;
        if (l.fallback_window_ns <= 0) { p.why = p.frame_why; return p; }
        const int64_t target = fc.start_ns ? fc.start_ns : fc.publish_ns ? fc.publish_ns : now_ns;
        CamSample h;
        const CamMailbox::Find f = present.nearest(target, l.fallback_window_ns, h);
        if (f != CamMailbox::Find::ok) { p.why = find_admit(f); return p; }
        const Admit a = admit_sample(h, target, lock_epoch, stage_epoch, l);
        if (a != Admit::ok) { p.why = a; return p; }
        p.ok = true; p.s = h; p.src = CamSource::fallback; p.why = Admit::ok; return p;
    }
    CamSample s;
    if (!present.try_read(s)) { p.why = Admit::none; return p; }
    const Admit a = admit_sample(s, now_ns, lock_epoch, stage_epoch, l);
    p.frame_why = a; p.why = a;
    if (a == Admit::ok) { p.ok = true; p.s = s; p.src = CamSource::latest; }
    return p;
}

} // namespace acz::terrain
