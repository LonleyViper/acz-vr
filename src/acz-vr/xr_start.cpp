#include "xr_start.h"

namespace acz::xr_start {
using xr_owner::Cause;
using xr_owner::Op;
using xr_owner::Rc;

const char* rc_name(Rc rc) noexcept {
    switch (rc) {
    case Rc::ok: return "ok";
    case Rc::loss_pending: return "loss-pending";
    case Rc::frame_discarded: return "frame-discarded";
    case Rc::event_unavailable: return "event-unavailable";
    case Rc::error: return "error";
    case Rc::session_lost: return "session-lost";
    case Rc::instance_lost: return "instance-lost";
    case Rc::refused: return "refused";
    }
    return "unknown";
}

const char* class_name(Class c) noexcept {
    switch (c) {
    case Class::hooked: return "hooked";
    case Class::ended_after_start: return "ended-after-start";
    case Class::transient: return "failed-transient";
    case Class::permanent: return "failed-permanent";
    }
    return "unknown";
}

bool transient_xr_result(int32_t r) noexcept {
    // XR_ERROR_RUNTIME_FAILURE -2, XR_ERROR_INSTANCE_LOST -13, XR_ERROR_SESSION_LOST -17,
    // XR_ERROR_SESSION_NOT_READY -28, XR_ERROR_FORM_FACTOR_UNAVAILABLE -35 (static_asserted in xr_owner_openxr.cpp).
    return r == -2 || r == -13 || r == -17 || r == -28 || r == -35;
}

namespace {
const char* transient_reason(int32_t r) noexcept {
    switch (r) {
    case -35: return "hmd-unavailable(form-factor-unavailable)";
    case -2: return "runtime-failure";
    case -28: return "session-not-ready";
    case -13: return "instance-lost";
    case -17: return "session-lost";
    }
    return "runtime-error";
}
void take(Diagnosis& d, const xr_owner::CallRecord& c) noexcept {
    d.has_call = true;
    d.call = c.op;
    d.call_rc = c.rc;
    d.xr = c.xr;
    d.call_entry_ms = c.entry_ms;
    d.call_ms = c.exit_ms - c.entry_ms;
}
} // namespace

Diagnosis diagnose(const xr_owner::Report& r, const xr_owner::CallRecord* rec, unsigned n, bool budget,
    const xr_owner::Limits& lim) noexcept {
    Diagnosis d;
    if (!rec) n = 0;
    // The call evidence first: the first failing call, else the call that crossed the startup
    // deadline, else the first call that broke the 250 ms progress rule.
    const xr_owner::CallRecord* failing = nullptr;
    const xr_owner::CallRecord* crossing = nullptr;
    const xr_owner::CallRecord* slow = nullptr;
    for (unsigned i = 0; i < n; ++i) {
        const auto& c = rec[i];
        if (!failing && !xr_owner::succeeded(c.rc)) failing = &c;
        if (!crossing && c.entry_ms < lim.startup_ms && c.exit_ms >= lim.startup_ms) crossing = &c;
        // VSEARLY (Astra 6458816088 P3): the attempt's EFFECTIVE frame-phase limit (frame_stall_ms when set): a tolerated 2 s call is never
        // named as the cause of a later 8 s failure. Default limits: progress_ms exactly as before.
        if (!slow && r.first_frame_done_ms >= 0 && c.entry_ms >= r.first_frame_done_ms && c.exit_ms - c.entry_ms >= lim.frame_progress_ms()) slow = &c;
    }
    const Cause cause = r.first_cause;
    if (cause == Cause::startup_deadline && crossing) take(d, *crossing);
    else if (cause == Cause::progress_deadline && slow) take(d, *slow);
    else if (failing) take(d, *failing);

    const bool hooked = r.first_frame_done_ms >= 0;
    if (r.success) { d.cls = Class::ended_after_start; d.reason = "orderly-stop"; return d; }
    if (r.stop == xr_owner::StopReason::requested || cause == Cause::operator_stop_before_lifecycle) {
        d.cls = hooked ? Class::ended_after_start : Class::permanent;
        d.reason = "operator-stop(game-exit)";
        return d;
    }
    const bool early_progress = cause == Cause::progress_deadline && r.begun <= kStartWindowFrames;
    if (hooked && !early_progress) {
        d.cls = Class::ended_after_start; d.reason = xr_owner::cause_name(cause);
        d.clean_after_start = (cause == Cause::progress_deadline || cause == Cause::loss) && r.complete && r.clean && !r.instance_held &&
            !r.session_held && !r.binding_held;   // VSEARLY only (RetryPolicy::set_early)
        return d;
    }
    // A retained handle forbids another session on this device/queue (clean-retirement rule).
    if (!r.complete || !r.clean || r.instance_held || r.session_held || r.binding_held) {
        d.cls = Class::permanent; d.reason = "unclean-retirement(handles-retained)"; return d;
    }
    switch (cause) {
    case Cause::startup_deadline:
        d.cls = Class::transient;
        d.reason = d.has_call && d.call == Op::create_instance ? "startup-deadline(runtime-slow-to-start)" : "startup-deadline";
        return d;
    case Cause::ready_timeout: d.cls = Class::transient; d.reason = "ready-timeout(session-never-READY)"; return d;
    case Cause::progress_deadline: d.cls = Class::transient; d.reason = "progress-deadline(early-frame)"; return d;
    case Cause::loss: d.cls = Class::transient; d.reason = "session-or-instance-lost-during-start"; return d;
    case Cause::consumer:
        if (budget) { d.cls = Class::transient; d.reason = "first-frame-budget(cold-setup)"; }
        else { d.cls = Class::permanent; d.reason = "consumer-refused"; }
        return d;
    case Cause::startup_call:
        if (d.has_call && d.call == Op::create_instance) {
            d.cls = Class::permanent;
            d.reason = d.xr == -51 ? "runtime-unavailable(no-openxr-runtime)" : "instance-refused";
            return d;
        }
        if (d.has_call && transient_xr_result(d.xr)) { d.cls = Class::transient; d.reason = transient_reason(d.xr); return d; }
        d.cls = Class::permanent; d.reason = "runtime-error";
        return d;
    default:
        d.cls = Class::permanent; d.reason = xr_owner::cause_name(cause);
        return d;
    }
}

// ------------------------------------------------------------------ retry ---
namespace {
bool same_lower(const char* a, const char* b) noexcept {
    for (; *a && *b; ++a, ++b) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? char(*a - 'A' + 'a') : *a;
        if (ca != *b) return false;
    }
    return *a == 0 && *b == 0;
}
} // namespace

bool persistent_retry_enabled(const char* v) noexcept {
    if (!v) return true;
    return !(same_lower(v, "0") || same_lower(v, "off") || same_lower(v, "false") || same_lower(v, "no"));
}

int64_t backoff_ms(unsigned k) noexcept {
    static constexpr int64_t steps[] = {3000, 5000, 10000};
    return k < 3 ? steps[k] : 15000;
}

RetryPolicy::RetryPolicy(bool persistent, unsigned cap) noexcept
    : persistent_(persistent), cap_(cap == 0 ? 1 : (cap > kRing ? kRing : cap)) {}

unsigned slow_tier(int64_t elapsed) noexcept { return elapsed >= kSlowerAfterMs ? 2u : elapsed >= kSlowAfterMs ? 1u : 0u; }
int64_t tier_interval_ms(unsigned tier) noexcept { return tier >= 2 ? kSlowerIntervalMs : tier == 1 ? kSlowIntervalMs : 0; }

void RetryPolicy::attempt_started(int64_t now) noexcept {
    if (first_start_ < 0) first_start_ = now;
    starts_[started_++ % kRing] = now;
}

Decision RetryPolicy::decide(unsigned attempt, bool legacy_retry, const Diagnosis& d, int64_t now) noexcept {
    Decision out;
    if (legacy_retry && attempt == 1) { out.retry = true; out.delay_ms = 0; out.text = "yes-cold-start"; return out; }
    const bool early_after = early_ && d.cls == Class::ended_after_start && d.clean_after_start;   // VSEARLY (false unless set_early)
    if (d.cls != Class::transient && !early_after) {
        out.text = d.cls == Class::permanent ? "no(permanent)" : "no(after-start)";
        return out;
    }
    if (!persistent_) { out.text = "no(persistent-retry-off:ACZ_XR_RETRY=0)"; return out; }
    if (early_after) {   // VSEARLY: the session had run; start the schedule again from its end (backoff 3 s, no slow-down tier)
        k_ = 0; tier_ = 0; first_start_ = now;
        out.retry = true; out.delay_ms = backoff_ms(k_++); out.text = "yes-early(after-start)";
        if (started_ >= cap_) {
            const int64_t earliest = starts_[(started_ - cap_) % kRing] + kRetryWindowMs;
            if (now + out.delay_ms < earliest) { out.delay_ms = earliest - now; out.text = "yes-early(rate-capped)"; }
        }
        return out;
    }
    out.retry = true;
    out.delay_ms = backoff_ms(k_++);
    out.text = "yes-persistent";
    // R-5: slow down after 10 / 60 minutes of failed attempts (never a permanent stop).
    out.elapsed_ms = first_start_ < 0 ? 0 : now - first_start_;
    out.tier = slow_tier(out.elapsed_ms);
    if (out.tier != tier_) { out.tier_changed = out.tier > tier_; tier_ = out.tier; }
    if (tier_interval_ms(out.tier) > out.delay_ms) {
        out.delay_ms = tier_interval_ms(out.tier);
        out.text = out.tier >= 2 ? "yes-persistent(slow-5min)" : "yes-persistent(slow-60s)";
    }
    // Rate cap: the next start may not be the (cap+1)-th inside one window.
    if (started_ >= cap_) {
        const int64_t earliest = starts_[(started_ - cap_) % kRing] + kRetryWindowMs;
        if (now + out.delay_ms < earliest) { out.delay_ms = earliest - now; out.text = "yes-persistent(rate-capped)"; }
    }
    return out;
}

unsigned run_attempts(RetryPolicy& policy, const LoopHooks& h) {
    for (unsigned n = 1;; ++n) {
        policy.attempt_started(h.now_ms());
        AttemptOutcome out;
        if (!h.attempt(n, out)) return n;
        Decision d;
        if (out.stop) d.text = "no(stop)";
        else d = policy.decide(n, out.legacy_retry, out.diag, h.now_ms());
        if (h.ended) h.ended(n, out, d);
        if (!d.retry) return n;
        if (d.delay_ms > 0 && !h.wait(d.delay_ms)) return n;
    }
}

} // namespace acz::xr_start
