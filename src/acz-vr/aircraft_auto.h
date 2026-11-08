#pragma once
// aircraft_auto.h -- AIRCRAFT-AUTO: xr_cockpit_model=auto[;opt:val] (docs/world-render/aircraft-auto.md). No new config key.
//
// The player picks the aircraft in the hangar, so the right baked cockpit differs per mission. The E6 structure (located by the anchor-free
// locator, ee_e6_locator.h) holds the aircraft position and the cockpit camera position; with the game's look at rest, the camera minus the
// aircraft position, rotated back into the aircraft frame by the camera rotation, is the type's .pvb eye point (verified on real E6 samples:
// docs/world-render/aircraft-auto.md section 2). The 36 player types have 34 distinct eye points (research/world-render-data/aircraft_cockpits.json);
// the only identical ones are the F-15C/E/S_MTD (resolved by a documented rule, section 4). This module is SDK-free (no Windows/D3D12 types):
//   * the type table (kAircraft, mirrored from aircraft_cockpits.json; a test cross-checks it),
//   * eye_in_aircraft(): one E6 sample -> the eye offset in the aircraft frame + whether the game's look is at rest,
//   * EyeMailbox: one-writer seqlock (probe Present tick -> XR consumer), like terrain_camera.h,
//   * Detector: windows of look-centred samples -> median -> nearest table eye within a tolerance -> a type, with hysteresis,
//   * parse_auto(): the "auto[;opt:val]" config value (cockpit options shared by every model + prefer:<type>).
// READ-ONLY by construction: nothing here touches game memory (the probe glue copies the E6 windows, ee_e6_locator.h).
#include "ee_e6_locator.h"
#include "cockpit_model.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace acz::aircraft {

// ------------------------------------------------------------------------------------------------ type table
struct AircraftType {
    const char* type;       // plmodel/playerlist.txt id
    const char* model_id;   // baked file stem: <type><skin>_vrcockpit (aircraft_cockpits.json model_id)
    float eye[3];           // data/mis/mpl/<type>.pvb bytes 40..52 (model units; +X right, +Y up, -Z nose)
    uint8_t stick;          // interior stick style for the COCKPIT-4 procedural interior (0 centre, 1 side), aircraft_cockpits.json "stick"
};
inline constexpr unsigned kAircraftCount = 36;
extern const AircraftType kAircraft[kAircraftCount];
int find_type(const char* type) noexcept;   // index or -1
// Types whose eye points are identical (within 1e-4) to type i (including i), as a bit mask over kAircraft (36 < 64).
uint64_t same_eye_mask(unsigned i) noexcept;
// The smallest distance between two DIFFERENT eye points of the table (0.1616: e18g/m29a). Detector tolerances must stay below half of it.
float min_distinct_eye_separation() noexcept;
// AIRCRAFT-AUTO-JUNK (headset 2026-10-30 live-1508, F-5E shown as the Draken): the content-scan locator can lock a STATIC junk block (air 0,0,60, cam 0,0,0:
// a look-centred eye offset of 0,0,-60, 8.5 units from every table eye). Such samples are no player cockpit eye: they must not vouch for the shown aircraft
// (the AIRCRAFT-AUTO-STALE clock). nearest_eye_distance = distance to the nearest table eye (1e30 for non-finite); plausible_eye = within
// kPlausibleEyeDist (a real centred sample sits within match_tol + ground wobble 0.15; a turned look pivots the eye by up to ~1 unit).
inline constexpr float kPlausibleEyeDist = 2.0f;
float nearest_eye_distance(const float local[3]) noexcept;
bool plausible_eye(const float local[3]) noexcept;

// ------------------------------------------------------------------------------------------------ E6 sample -> aircraft-frame eye offset
// d = cam - air (world units); R = Ry(rot yaw) * Rx(rot pitch) * Rz(rot roll) (the convention that maps the j35j pvb eye onto the real E6 samples:
// aircraft-auto.md section 2); local = R^T d. centred = the game's look is at rest (|look pitch/yaw/roll| <= kLookCentredRad and
// |wrap(rot yaw - heading)| <= kLookCentredRad): only then is rot the aircraft attitude and the camera at the pvb eye (with the look turned, the
// game pivots the eye by up to ~1 unit: step50 look-back sample 35.69 u vs 36.48). false for non-finite input.
inline constexpr double kLookCentredRad = 0.005;
bool eye_in_aircraft(const acz::e6loc::Sample& s, float local[3], bool& centred) noexcept;

// ------------------------------------------------------------------------------------------------ mailbox (probe Present tick -> XR consumer)
struct EyeSample { float local[3]{}; bool centred = false; uint32_t lock_epoch = 0; uint64_t seq = 0; };
class EyeMailbox {
public:
    void publish(const float local[3], bool centred, uint32_t lock_epoch) noexcept {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 3; ++i) { uint32_t w; std::memcpy(&w, &local[i], 4); w_[i].store(w, std::memory_order_relaxed); }
        w_[3].store(centred ? 1u : 0u, std::memory_order_relaxed);
        w_[4].store(lock_epoch, std::memory_order_relaxed);
        const uint64_t n = n_.load(std::memory_order_relaxed) + 1;
        n_.store(n, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
        // AIRCRAFT-AUTO-FAST: the same sample also goes into a small ring (one seqlock per slot) so the consumer can feed EVERY E6 sample,
        // not only the newest one per composed frame (the XR consumer ticks per composed frame, which can be slower than the Present tick).
        Slot& sl = ring_[n % kRing];
        sl.seq.store(2 * n - 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < 3; ++i) { uint32_t w; std::memcpy(&w, &local[i], 4); sl.w[i].store(w, std::memory_order_relaxed); }
        sl.w[3].store(centred ? 1u : 0u, std::memory_order_relaxed);
        sl.w[4].store(lock_epoch, std::memory_order_relaxed);
        sl.seq.store(2 * n, std::memory_order_release);
        head_.store(n, std::memory_order_release);
    }
    // Every sample published after sequence `after` (oldest first, at most `cap` and at most kRing: older ones are lost and counted in `lost`).
    // A slot overwritten while it was read is skipped (counted as lost). Returns the number written to out; out[i].seq = its publication number.
    static constexpr unsigned kRing = 64;
    unsigned drain(uint64_t after, EyeSample* out, unsigned cap, uint64_t* lost = nullptr) const noexcept {
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (!out || !cap || head <= after) return 0;
        const unsigned lim = cap < kRing ? cap : kRing;
        uint64_t first = after + 1;
        if (head - after > lim) { if (lost) *lost += head - after - lim; first = head - lim + 1; }
        unsigned k = 0;
        for (uint64_t n = first; n <= head; ++n) {
            const Slot& sl = ring_[n % kRing];
            const uint64_t s1 = sl.seq.load(std::memory_order_acquire);
            if (s1 != 2 * n) { if (lost) ++*lost; continue; }
            uint32_t w[5];
            for (int i = 0; i < 5; ++i) w[i] = sl.w[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sl.seq.load(std::memory_order_relaxed) != s1) { if (lost) ++*lost; continue; }
            EyeSample e;
            for (int i = 0; i < 3; ++i) std::memcpy(&e.local[i], &w[i], 4);
            if (!std::isfinite(e.local[0]) || !std::isfinite(e.local[1]) || !std::isfinite(e.local[2])) { if (lost) ++*lost; continue; }
            e.centred = w[3] != 0; e.lock_epoch = w[4]; e.seq = n;
            out[k++] = e;
        }
        return k;
    }
    // false: nothing published yet or a publication in progress (try-only).
    bool try_read(EyeSample& out) const noexcept {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 == 0 || (s1 & 1u)) return false;
        uint32_t w[5];
        for (int i = 0; i < 5; ++i) w[i] = w_[i].load(std::memory_order_relaxed);
        const uint64_t n = n_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        EyeSample e;
        for (int i = 0; i < 3; ++i) std::memcpy(&e.local[i], &w[i], 4);
        if (!std::isfinite(e.local[0]) || !std::isfinite(e.local[1]) || !std::isfinite(e.local[2])) return false;
        e.centred = w[3] != 0; e.lock_epoch = w[4]; e.seq = n;
        out = e; return true;
    }
private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint32_t> w_[5]{};
    std::atomic<uint64_t> n_{0};
    struct Slot { std::atomic<uint64_t> seq{0}; std::atomic<uint32_t> w[5]{}; };
    Slot ring_[kRing];
    std::atomic<uint64_t> head_{0};
};
inline EyeMailbox& eye_mailbox() noexcept { static EyeMailbox m; return m; }

// ------------------------------------------------------------------------------------------------ detector
struct DetectOptions {
    unsigned window = 12;       // look-centred samples per decision window (~0.2 s at 60 Hz; was 24 before AIRCRAFT-AUTO-FAST); non-overlapping
    float match_tol = 0.08f;    // |median - table eye| accepted (< half the smallest distinct separation 0.1616 => at most one eye matches)
    float spread_tol = 0.30f;   // a sample within this of the window median is an inlier ...
    float min_inliers = 0.75f;  // ... and fewer inliers than this fraction = unstable (window ignored; single outliers/torn reads do not matter)
    unsigned confirm = 2;       // consecutive agreeing windows before a CHANGE of the chosen type (the first choice needs one)
    unsigned drop_confirm = 25; // consecutive NO-MATCH windows before a chosen type is dropped (-> the game's replayed cockpit). Headset test 2026-10-28 item 11: on the
                                // k00a airfield the eye wobbled 0.08..0.15 (on the ground) and 2 no-match windows unloaded the model; with the look turned back
                                // (non-centred samples = no windows) nothing could reload it, so the plane vanished. A different table type still switches after `confirm`.
                                // W5-3 P2 #1: within the lock epoch in which the type was chosen / confirmed no-match NEVER drops it (a longer error streak had the same wedge);
                                // only windows of a later lock epoch (mission change / re-lock) count `drop_confirm` no-matches.
    uint64_t prefer = 0;        // tie rule: a preferred type (bit mask) wins inside an identical-eye group; else the first in table order
};
enum class Verdict : uint8_t { match = 0, no_match, unstable, ambiguous };
const char* verdict_name(Verdict) noexcept;
struct WindowResult {
    Verdict verdict = Verdict::no_match;
    int chosen = -1;                 // match: the type index after the tie rule
    uint64_t ties = 0;               // match: every type with the chosen eye point (bit mask; one bit = no tie)
    int best = -1, second = -1;      // nearest and second-nearest DISTINCT eye (index of the first type of that eye)
    float best_res = 0, second_res = 0, spread = 0;   // spread = max |sample - median|
    float inlier_frac = 0;
    float median[3] = {0, 0, 0};
    float confidence = 0;            // 1 - best_res / match_tol (match), else 0
    unsigned samples = 0;
};
class Detector {
public:
    explicit Detector(DetectOptions o = DetectOptions{}) noexcept : o_(o) {}
    // One validated sample of the LOCKED E6 block. Returns true when a window completed (last() is new). A changed lock epoch (the locator lost
    // and re-acquired the block: mission change/reload) discards the partial window; the current choice stays until windows disagree (hysteresis).
    bool feed(const float local[3], bool centred, uint32_t lock_epoch) noexcept;
    // AIRCRAFT-AUTO-FAST: start with an ASSUMED type (the remembered last aircraft or the config's default:) before any window. While assumed:
    // a window matching it CONFIRMS it (confirmed() for that window; no switch counted); `confirm` agreeing windows of another type SWITCH
    // (hysteresis as for any change); no-match windows never drop an assumed type (only a confirmed different aircraft replaces it).
    // Call before the first feed(); type out of range = no assumption (exactly the plain detector).
    void assume(int type) noexcept;
    // AIRCRAFT-AUTO-STALE (headset test 2026-10-29 item 9): the consumer calls this when the locked E6 block produced NO sample for `stale:` ms, i.e. the
    // current choice (an assumption from the remembered file / default:, or a type confirmed in an earlier mission) can no longer be vouched for. The choice
    // is dropped (current() == -1 -> the game's replayed cockpit); the next matching window chooses again with ONE window (first choice).
    void forget() noexcept { current_ = -1; assumed_ = false; confirmed_ = false; pending_ = -2; pending_n_ = 0; changed_ = false; buf_.clear(); ++forgets; }
    uint64_t forgets = 0;
    bool assumed() const noexcept { return assumed_; }         // current() is an assumption not yet confirmed by a window
    bool confirmed() const noexcept { return confirmed_; }     // the last completed window confirmed the assumed type
    int current() const noexcept { return current_; }          // chosen type index, -1 = none (no model: the game's replayed cockpit)
    bool changed() const noexcept { return changed_; }         // the last completed window changed current()
    const WindowResult& last() const noexcept { return last_; }
    const DetectOptions& options() const noexcept { return o_; }
    uint64_t centred_samples = 0, off_centre_samples = 0, windows = 0, switches = 0, epochs = 0;
    static WindowResult evaluate(const std::vector<float>& xyz, const DetectOptions& o) noexcept;   // xyz = 3 floats per sample
private:
    DetectOptions o_;
    std::vector<float> buf_;
    int current_ = -1, pending_ = -2; unsigned pending_n_ = 0; bool changed_ = false; bool have_epoch_ = false; uint32_t epoch_ = 0, chosen_epoch_ = 0;
    bool assumed_ = false, confirmed_ = false;
    WindowResult last_{};
};

// ------------------------------------------------------------------------------------------------ config value
// "auto[;opt:val;...]": every cockpit option of cockpit::parse_options applies to whichever model is chosen, except the per-model ones
// (hide:, show:, eye:) which are refused; stick: given explicitly overrides the table's per-type stick style; prefer:<type>[,<type>] (auto only)
// names the preferred types inside an identical-eye group (F-15C/E/S_MTD). is_auto() = the value's first item is exactly "auto".
// default:<model_stem|type> (auto only; AIRCRAFT-AUTO-FAST): the model assumed at XR start when no remembered aircraft exists; it must name a
// table entry (e.g. default:j35jk_vrcockpit or default:j35j), only [A-Za-z0-9_] (no path), else the value is refused.
bool is_auto(const std::string& value) noexcept;
struct AutoConfig {
    acz::cockpit::Options opt; bool stick_explicit = false; uint64_t prefer = 0; std::string prefer_text;
    int default_type = -1; std::string default_text;   // default: (-1 = none)
    unsigned stale_ms = 15000;   // stale:<ms> (0..600000; 0 = never): AIRCRAFT-AUTO-STALE, see Detector::forget
    // grace:<ms> (0..600000; default 60000; 0 = no extra grace): AIRCRAFT-AUTO-GRACE (headset test 7, 2026-10-31). A model that is only ASSUMED (the remembered
    // file / default:, no window has confirmed it yet) and has seen no implausible E6 sample is kept for max(stale, grace) ms: the locator needs 20-25 s of mission
    // to lock (struct-scan passes 5 s apart), so the 15 s stale limit dropped the CORRECT F-5E for 8.4 s (the cockpit, the airframe and the wings vanished).
    unsigned grace_ms = 60000;
};
bool parse_auto(const std::string& value, AutoConfig& out, std::string& why);
// The options for type i: the shared options, with the table's stick style unless stick: was given.
acz::cockpit::Options options_for(const AutoConfig& c, unsigned type) noexcept;
// A table entry named by its type ("j35j") or its model stem ("j35jk_vrcockpit"); -1 for anything else (also any path character).
int find_type_or_model(const std::string& name) noexcept;

// ------------------------------------------------------------------------------------------------ remembered last aircraft (AIRCRAFT-AUTO-FAST)
// %LOCALAPPDATA%\acz-vr\last-aircraft.json = {"version":1,"type":"j35j","model":"j35jk_vrcockpit"}: written (atomically: temp file + rename)
// when the detector confirms a type, read at XR start so the model cockpit is drawn from the first frame. Read validation: <= 512 bytes, the
// keys "type" and "model" as plain strings (no escapes), characters [A-Za-z0-9_] only (a path separator, '.', ':' or '%' is refused), the type
// must be a table entry and the model must be exactly that entry's model stem (so the file can only ever select one of the 36 table files).
inline constexpr size_t kRememberedMaxBytes = 512;
std::string remembered_path();                                   // "" without LOCALAPPDATA
std::string remembered_json(unsigned type);                      // the file content for a table type ("" when out of range)
bool parse_remembered(const std::string& text, int& type, std::string& why);
bool read_remembered(const std::string& path, int& type, std::string& why);    // why = "no-file" when absent
bool write_remembered(const std::string& path, unsigned type, std::string& why); // temp + rename (replace); serialized
// The type assumed at XR start: the valid remembered file, else the config's default:, else -1. remembered = the file's type (-1 when absent or
// refused; why = the refusal), source = "remembered-file" | "config-default" | "" .
int resolve_assumed(const AutoConfig& cfg, const std::string& path, int& remembered, std::string& source, std::string& why);

} // namespace acz::aircraft
