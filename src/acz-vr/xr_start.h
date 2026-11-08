#pragma once
// xr_start.h -- XR-START: pure diagnosis of one in-proxy XR start attempt (and, from item 2,
// the persistent retry policy). No OpenXR, D3D12 or Windows dependency: the inputs are the
// owner's Report and CallRecords (xr_owner.h), so the offline tests drive it with fakes.
// docs/world-render/xr-start.md explains every token.
#include "xr_owner.h"

#include <cstdint>
#include <functional>

namespace acz::xr_start {

// Rc category name as logged ("ok", "error", "session-lost", "refused", ...).
const char* rc_name(xr_owner::Rc) noexcept;

// Outcome class of one attempt.
//  hooked            the first begun frame completed (logged while the attempt runs)
//  ended_after_start the attempt ended after a completed first frame (game exit, loss, watchdog)
//  transient         a start failure a real headset/runtime can cause and later clear
//  permanent         a start failure that retrying would only repeat (or must not repeat)
enum class Class : uint8_t { hooked, ended_after_start, transient, permanent };
const char* class_name(Class) noexcept;   // "hooked", "ended-after-start", "failed-transient", "failed-permanent"

// Early frames: a progress-deadline (250 ms rule) failure within this many begun frames still
// counts as a start failure (an idle/off-face headset throttling the first frames; D-187/D-188).
constexpr unsigned kStartWindowFrames = 450;   // about 5 s at 90 Hz

// XrResults of a failed start call (other than xrCreateInstance) that a real headset/runtime
// can produce transiently: FORM_FACTOR_UNAVAILABLE (HMD in standby / not connected),
// RUNTIME_FAILURE, SESSION_NOT_READY, INSTANCE_LOST, SESSION_LOST.
bool transient_xr_result(int32_t) noexcept;

struct Diagnosis {
    Class cls = Class::permanent;
    const char* reason = "none";               // static text
    bool has_call = false;                     // a first failing (or slow) call was identified
    xr_owner::Op call = xr_owner::Op::count;
    xr_owner::Rc call_rc = xr_owner::Rc::ok;
    int32_t xr = xr_owner::kNoXrResult;
    int64_t call_entry_ms = 0, call_ms = 0;    // owner time of entry, duration
    // VSEARLY: an ended_after_start attempt that stalled (progress deadline) or lost its session, retired cleanly with no handle held.
    // Informational for the default policy (never read unless RetryPolicy::set_early(true)).
    bool clean_after_start = false;
};

// `records`/`count`: the owner's call records (Owner::records()/record_count() after finished()).
// `consumer_startup_budget`: Output::startup_budget_refused() (first frame refused before any
// submission because setup spent the startup budget).
Diagnosis diagnose(const xr_owner::Report&, const xr_owner::CallRecord* records, unsigned count,
    bool consumer_startup_budget, const xr_owner::Limits& = {}) noexcept;

// ------------------------------------------------------ persistent retry (item 2) ---
// Default ON. Escape: environment variable ACZ_XR_RETRY=0 (also off/false/no, any case) restores
// the previous policy exactly (attempt 1 + at most one immediate cold-start retry).
// `value` = the variable's text, nullptr when unset.
bool persistent_retry_enabled(const char* value) noexcept;
// Backoff before persistent retry k (0-based): 3 s, 5 s, 10 s, then every 15 s.
int64_t backoff_ms(unsigned k) noexcept;
// At most this many attempt STARTS inside any 60 s window (the legacy pair included).
constexpr unsigned kRetryPerMinuteCap = 6;
constexpr int64_t kRetryWindowMs = 60000;
// R-5 bounded policy: measured from the first attempt's start, after 10 min of failed attempts the persistent retry waits at
// least 60 s, after 60 min at least 5 min. It never stops for good while the game runs.
constexpr int64_t kSlowAfterMs = 10 * 60000, kSlowIntervalMs = 60000;
constexpr int64_t kSlowerAfterMs = 60 * 60000, kSlowerIntervalMs = 5 * 60000;
// 0 = normal schedule, 1 = every 60 s, 2 = every 5 min.
unsigned slow_tier(int64_t elapsed_ms) noexcept;
int64_t tier_interval_ms(unsigned tier) noexcept;   // 0, 60000, 300000

struct Decision {
    bool retry = false;
    int64_t delay_ms = -1;      // wait before the next attempt (0 = immediately; -1 = none)
    const char* text = "no";    // static: yes-cold-start | yes-persistent | yes-persistent(rate-capped) | yes-persistent(slow-60s|slow-5min) | no(...)
    unsigned tier = 0;          // slow_tier at this decision (persistent retries only)
    bool tier_changed = false;  // the first decision of a new tier (logged as retry-slowdown)
    int64_t elapsed_ms = 0;     // since the first attempt started
};

// One per process (the monitor's); used by one thread only.
class RetryPolicy {
public:
    explicit RetryPolicy(bool persistent, unsigned per_minute_cap = kRetryPerMinuteCap) noexcept;
    void attempt_started(int64_t now_ms) noexcept;
    // `legacy_retry` = xr_stereo::retry_allowed(attempt, report, consumer_startup_budget): the existing
    // immediate cold-start retry, kept first and unchanged. A persistent retry needs a TRANSIENT
    // diagnosis, which already implies a clean retirement with no retained handle.
    Decision decide(unsigned attempt, bool legacy_retry, const Diagnosis&, int64_t now_ms) noexcept;
    bool persistent() const noexcept { return persistent_; }
    // VSEARLY (docs/world-render/vsearly.md; default false = the policy above exactly): the session also runs in menus and between
    // missions, so (1) an ended_after_start attempt with clean_after_start is retried (`yes-early(after-start)`, backoff from 3 s again;
    // an orderly runtime stop or game exit is not), and (2) a hooked attempt restarts the backoff and the R-5 slow-down clock (measured
    // from the end of the last hooked attempt, not from the first start: test 19 waited 60 s after a 674 s session). Needs `persistent`.
    void set_early(bool on) noexcept { early_ = on; }
    bool early() const noexcept { return early_; }
    unsigned persistent_retries() const noexcept { return k_; }
private:
    static constexpr unsigned kRing = 16;
    bool persistent_;
    unsigned cap_;
    unsigned k_ = 0;
    unsigned tier_ = 0;
    int64_t first_start_ = -1;
    int64_t starts_[kRing] = {};
    unsigned started_ = 0;
    bool early_ = false;
};

// The monitor's attempt loop, injectable for the offline tests. attempt(n, out) runs attempt n to
// completion (false = abort: launch failure, consumer attach refusal, evidence timeout); wait(ms)
// sleeps and returns false when a stop was requested; ended() logs the decision. Returns the
// number of attempts started.
struct AttemptOutcome {
    xr_owner::Report report;
    Diagnosis diag;
    bool legacy_retry = false;
    bool stop = false;          // a process stop was requested: never retried
};
struct LoopHooks {
    std::function<int64_t()> now_ms;
    std::function<bool(unsigned n, AttemptOutcome& out)> attempt;
    std::function<bool(int64_t ms)> wait;
    std::function<void(unsigned n, const AttemptOutcome&, const Decision&)> ended;
};
unsigned run_attempts(RetryPolicy&, const LoopHooks&);

} // namespace acz::xr_start
