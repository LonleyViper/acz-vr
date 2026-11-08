#include "xr_owner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace acz::xr_owner {

bool Limits::valid() const noexcept {
    const Limits ceiling{};
    return absolute_ms > 0 && absolute_ms <= ceiling.absolute_ms &&
        cleanup_ms > 0 && cleanup_ms <= ceiling.cleanup_ms && cleanup_ms < absolute_ms &&
        startup_ms > 0 && startup_ms <= ceiling.startup_ms && startup_ms <= admission_ms() &&
        progress_ms > 0 && progress_ms <= ceiling.progress_ms &&
        poll_ms > 0 && poll_ms <= ceiling.poll_ms && poll_ms <= progress_ms &&
        max_frames > 0 && max_frames <= ceiling.max_frames &&
        events_per_batch > 0 && events_per_batch <= ceiling.events_per_batch &&
        (frame_stall_ms == 0 || (frame_stall_ms >= progress_ms && frame_stall_ms <= kFrameStallCeilingMs));   // VSEARLY (0 = off)
}

const char* cause_name(Cause c) noexcept {
    switch (c) {
    case Cause::none: return "none";
    case Cause::disabled: return "disabled";
    case Cause::invalid_limits: return "invalid-limits";
    case Cause::already_used: return "single-attempt-already-used";
    case Cause::startup_call: return "startup-call-failed";
    case Cause::compatibility: return "compatibility-refusal";
    case Cause::ready_timeout: return "ready-timeout";
    case Cause::event_flood: return "event-flood";
    case Cause::events_lost: return "events-lost";
    case Cause::session_mismatch: return "session-mismatch";
    case Cause::loss: return "session-or-instance-loss";
    case Cause::exiting: return "unexpected-exiting";
    case Cause::frame_call: return "frame-call-failed";
    case Cause::end_frame_failed: return "end-frame-failed-no-retry";
    case Cause::startup_deadline: return "startup-deadline";
    case Cause::progress_deadline: return "progress-deadline";
    case Cause::absolute_cap: return "absolute-cap";
    case Cause::cleanup_deadline: return "cleanup-deadline";
    case Cause::teardown_incomplete: return "teardown-incomplete";
    case Cause::space_change_overlap: return "reference-space-change-overlap";
    case Cause::exception: return "exception";
    case Cause::records_lost: return "records-lost";
    case Cause::operator_stop_before_lifecycle: return "operator-stop-before-lifecycle";
    case Cause::consumer: return "consumer-refused-or-disabled";
    }
    return "unknown";
}
const char* session_state_name(SessionState s) noexcept {
    switch (s) {
    case SessionState::unknown: return "UNKNOWN";
    case SessionState::idle: return "IDLE";
    case SessionState::ready: return "READY";
    case SessionState::synchronized: return "SYNCHRONIZED";
    case SessionState::visible: return "VISIBLE";
    case SessionState::focused: return "FOCUSED";
    case SessionState::stopping: return "STOPPING";
    case SessionState::loss_pending: return "LOSS_PENDING";
    case SessionState::exiting: return "EXITING";
    }
    return "UNKNOWN";
}
const char* stop_name(StopReason s) noexcept {
    switch (s) {
    case StopReason::none: return "none";
    case StopReason::requested: return "requested";
    case StopReason::time_cap: return "time-cap";
    case StopReason::frame_cap: return "frame-cap";
    case StopReason::runtime_stopping: return "runtime-stopping";
    case StopReason::failure: return "failure";
    }
    return "unknown";
}
const char* op_name(Op op) noexcept {
    static const char* names[] = {"create_instance", "query_system", "graphics_requirements",
        "query_binding", "create_session", "query_spaces", "create_local_space",
        "create_view_space", "poll_event", "begin_session", "wait_frame", "begin_frame",
        "locate_views", "locate_head", "end_frame", "request_exit", "end_session",
        "destroy_view_space", "destroy_local_space", "destroy_session", "release_binding",
        "destroy_instance", "swapchain_query", "create_swapchain", "acquire_image", "wait_image",
        "release_image", "destroy_swapchain", "end_frame_projection", "d3d_setup", "d3d_bind",
        "d3d_record", "d3d_close", "d3d_gpu_wait", "d3d_execute", "d3d_signal", "d3d_sample",
        "d3d_destroy"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<unsigned>(Op::count), "op names");
    return op < Op::count ? names[static_cast<unsigned>(op)] : "invalid";
}

// ------------------------------------------------------------ pure policy ---
bool compatible(const GraphicsRequirements& req, const BindingFacts& b) noexcept {
    return b.device_present && b.queue_present && req.min_feature_level != 0 &&
        b.luid_low == req.luid_low && b.luid_high == req.luid_high &&
        b.max_feature_level >= req.min_feature_level && b.queue_direct && b.queue_on_device;
}
unsigned compat_mismatch(const GraphicsRequirements& req, const BindingFacts& b) noexcept {
    unsigned m = 0;
    if (!b.device_present) m |= compat_device_absent;
    if (!b.queue_present) m |= compat_queue_absent;
    if (req.min_feature_level == 0) m |= compat_min_fl_zero;
    if (b.luid_low != req.luid_low) m |= compat_luid_low;
    if (b.luid_high != req.luid_high) m |= compat_luid_high;
    if (b.max_feature_level < req.min_feature_level) m |= compat_feature_level;
    if (!b.queue_direct) m |= compat_queue_not_direct;
    if (!b.queue_on_device) m |= compat_queue_off_device;
    return m;
}
const char* compat_stage_name(CompatStage s) noexcept {
    switch (s) {
    case CompatStage::none: return "none";
    case CompatStage::system: return "system";
    case CompatStage::binding: return "binding";
    case CompatStage::spaces: return "spaces";
    }
    return "unknown";
}
int format_compat_refusal(char* out, size_t size, unsigned attempt, const Report& r) noexcept {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (r.compat_stage != CompatStage::binding)
        return std::snprintf(out, size, "xr-stereo;event=compat-refusal;n=%u;stage=%s", attempt, compat_stage_name(r.compat_stage));
    static const char* const names[] = {"device-absent", "queue-absent", "min-fl-zero", "luid-low", "luid-high",
        "feature-level", "queue-not-direct", "queue-off-device"};
    const auto& q = r.compat_req; const auto& b = r.compat_bind;
    const unsigned m = compat_mismatch(q, b);
    char failed[160] = "none";
    size_t used = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!(m & (1u << i))) continue;
        const int n = std::snprintf(failed + used, sizeof failed - used, "%s%s", used ? "," : "", names[i]);
        if (n < 0 || static_cast<size_t>(n) >= sizeof failed - used) break;
        used += static_cast<size_t>(n);
    }
    // LUIDs as high:low hex (LUID.HighPart is signed; printed as its bit pattern).
    return std::snprintf(out, size,
        "xr-stereo;event=compat-refusal;n=%u;stage=binding;req-luid=0x%08x:0x%08x;req-min-fl=0x%x;"
        "bind-device=%u;bind-queue=%u;bind-luid=0x%08x:0x%08x;bind-max-fl=0x%x;bind-queue-direct=%u;"
        "bind-queue-on-device=%u;failed=%s",
        attempt, static_cast<uint32_t>(q.luid_high), q.luid_low, q.min_feature_level,
        b.device_present ? 1u : 0u, b.queue_present ? 1u : 0u, static_cast<uint32_t>(b.luid_high), b.luid_low,
        b.max_feature_level, b.queue_direct ? 1u : 0u, b.queue_on_device ? 1u : 0u, failed);
}
bool select_blend(const SystemFacts& s, uint32_t& out) noexcept {
    // Enumerated, never hardcoded: refuse an over-capacity enumeration, and
    // select OPAQUE only when the runtime actually offered it.
    if (s.blend_count == 0 || s.blend_count > max_blend_modes) return false;
    for (uint32_t i = 0; i < s.blend_count; ++i)
        if (s.blend_modes[i] == blend_opaque) { out = blend_opaque; return true; }
    return false;
}
// Finite position/orientation and a normalized quaternion (diagnostic tolerance).
bool pose_finite(const Pose& p) noexcept {
    double norm = 0;
    for (float v : p.position) if (!std::isfinite(v)) return false;
    for (float v : p.orientation) { if (!std::isfinite(v)) return false; norm += double(v) * v; }
    return std::fabs(norm - 1.0) <= 1e-3;
}
static bool fov_finite(const Fov& f) noexcept {
    return std::isfinite(f.left) && std::isfinite(f.right) && std::isfinite(f.up) && std::isfinite(f.down) &&
        f.left < f.right && f.down < f.up;
}

// -------------------------------------------------------------- snapshots ---
bool SnapshotSlot::try_publish(const PoseSnapshot& s) noexcept {
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    value_ = s;
    busy_.store(false, std::memory_order_release);
    live_.store(true, std::memory_order_release);
    return true;
}
bool SnapshotSlot::try_read(PoseSnapshot& out) const noexcept {
    if (!live_.load(std::memory_order_acquire)) return false;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
        return false; // contended: no retry, no wait
    out = value_;
    busy_.store(false, std::memory_order_release);
    return live_.load(std::memory_order_acquire); // a concurrent retraction wins
}

// ------------------------------------------------------------------ owner ---
Owner::Owner(std::shared_ptr<Runtime> rt, std::shared_ptr<Clock> clock, Config cfg, Limits lim)
    : rt_(std::move(rt)), clock_(std::move(clock)), cfg_(cfg), lim_(lim) {}

void Owner::latch(Cause c) noexcept {
    uint8_t empty = 0;
    first_cause_.compare_exchange_strong(empty, static_cast<uint8_t>(c), std::memory_order_acq_rel);
}

namespace { std::atomic<CallBracket> g_bracket{nullptr}; }
void set_call_bracket(CallBracket b) noexcept { g_bracket.store(b, std::memory_order_release); }

const char* xr_result_name(int32_t r) noexcept {
    // Core OpenXR 1.0 values (openxr.h); the real backend static_asserts the ones the start policy uses.
    switch (r) {
    case kNoXrResult: return "none";
    case 0: return "XR_SUCCESS";
    case 1: return "XR_TIMEOUT_EXPIRED";
    case 3: return "XR_SESSION_LOSS_PENDING";
    case 4: return "XR_EVENT_UNAVAILABLE";
    case 7: return "XR_SPACE_BOUNDS_UNAVAILABLE";
    case 8: return "XR_SESSION_NOT_FOCUSED";
    case 9: return "XR_FRAME_DISCARDED";
    case -1: return "XR_ERROR_VALIDATION_FAILURE";
    case -2: return "XR_ERROR_RUNTIME_FAILURE";
    case -3: return "XR_ERROR_OUT_OF_MEMORY";
    case -4: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case -6: return "XR_ERROR_INITIALIZATION_FAILED";
    case -7: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case -8: return "XR_ERROR_FEATURE_UNSUPPORTED";
    case -9: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case -10: return "XR_ERROR_LIMIT_REACHED";
    case -11: return "XR_ERROR_SIZE_INSUFFICIENT";
    case -12: return "XR_ERROR_HANDLE_INVALID";
    case -13: return "XR_ERROR_INSTANCE_LOST";
    case -14: return "XR_ERROR_SESSION_RUNNING";
    case -16: return "XR_ERROR_SESSION_NOT_RUNNING";
    case -17: return "XR_ERROR_SESSION_LOST";
    case -18: return "XR_ERROR_SYSTEM_INVALID";
    case -19: return "XR_ERROR_PATH_INVALID";
    case -20: return "XR_ERROR_PATH_COUNT_EXCEEDED";
    case -21: return "XR_ERROR_PATH_FORMAT_INVALID";
    case -22: return "XR_ERROR_PATH_UNSUPPORTED";
    case -23: return "XR_ERROR_LAYER_INVALID";
    case -24: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
    case -25: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
    case -26: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case -27: return "XR_ERROR_ACTION_TYPE_MISMATCH";
    case -28: return "XR_ERROR_SESSION_NOT_READY";
    case -29: return "XR_ERROR_SESSION_NOT_STOPPING";
    case -30: return "XR_ERROR_TIME_INVALID";
    case -31: return "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED";
    case -32: return "XR_ERROR_FILE_ACCESS_ERROR";
    case -33: return "XR_ERROR_FILE_CONTENTS_INVALID";
    case -34: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case -35: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case -36: return "XR_ERROR_API_LAYER_NOT_PRESENT";
    case -37: return "XR_ERROR_CALL_ORDER_INVALID";
    case -38: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case -39: return "XR_ERROR_POSE_INVALID";
    case -40: return "XR_ERROR_INDEX_OUT_OF_RANGE";
    case -41: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
    case -42: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
    case -44: return "XR_ERROR_NAME_DUPLICATED";
    case -45: return "XR_ERROR_NAME_INVALID";
    case -46: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
    case -47: return "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED";
    case -48: return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
    case -49: return "XR_ERROR_LOCALIZED_NAME_INVALID";
    case -50: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case -51: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    }
    return r >= 0 ? "XR_UNKNOWN_SUCCESS" : "XR_UNKNOWN_FAILURE";
}

template <class F> Rc Owner::call(Op op, F&& fn) {
    const int64_t start = elapsed();
    const Phase p = phase();
    const bool first_frame = p == Phase::frames && first_frame_open_.load(std::memory_order_acquire);
    int64_t deadline = absolute();
    Cause late = Cause::absolute_cap;
    if (p == Phase::startup || p == Phase::await_ready || first_frame) { deadline = lim_.startup_ms; late = Cause::startup_deadline; }
    else if (p == Phase::cleanup) { deadline = cleanup_deadline_.load(std::memory_order_acquire); late = Cause::cleanup_deadline; }
    // XR-START: report start-path calls (startup, READY wait, first frame, cleanup) to the observer.
    const bool observed = observer_ && (p == Phase::startup || p == Phase::await_ready || p == Phase::cleanup || first_frame);
    auto keep = [&](Rc rc, int64_t exit_ms, int32_t xr) {
        const CallRecord rec{op, rc, start, exit_ms, xr};
        if (records_used_ < record_capacity) records_[records_used_++] = rec;
        else if (cfg_.continuous) ++r_.records_overflow; // continuous: first calls kept, rest counted
        else ++r_.records_lost;
        if (observed && (op != Op::poll_event || !succeeded(rc))) {
            try { observer_->call(rec, p, first_frame); } catch (...) {}
        }
    };
    if (start >= deadline || start >= absolute()) {
        policy_failure_.store(true, std::memory_order_release);
        fail(start >= absolute() ? Cause::absolute_cap : late);
        keep(Rc::refused, start, kNoXrResult);
        return Rc::refused;
    }
    // Only a NEW xrWaitFrame is refused at admission close (orderly time cap);
    // an already waited frame is still begun and ended.
    if (op == Op::wait_frame && start >= admission()) { keep(Rc::refused, start, kNoXrResult); return Rc::refused; }
    last_progress_.store(start, std::memory_order_release);
    Rc rc = Rc::error;
    try { (void)rt_->take_last_xr_result(); } catch (...) {} // XR-START: clear, so the record carries only this call's result
    {
        const bool window = op != Op::wait_frame && op != Op::poll_event && op != Op::locate_views &&
            op != Op::locate_head && op != Op::d3d_sample;
        struct Bracket {
            CallBracket b; bool w;
            Bracket(CallBracket x, bool win) : b(x), w(win) { if (b) b(true, w); }
            ~Bracket() { if (b) b(false, w); }
        } bracket(g_bracket.load(std::memory_order_acquire), window);
        try { rc = fn(); } catch (...) { rc = Rc::error; fail(Cause::exception); }
    }
    const int64_t end = elapsed();
    last_progress_.store(end, std::memory_order_release);
    ++r_.runtime_calls;
    int32_t xr = kNoXrResult;
    try { xr = rt_->take_last_xr_result(); } catch (...) {}
    keep(rc, end, xr);
    // The owner's own post-hoc progress check, independent of the watchdog thread.
    if (p == Phase::frames && !first_frame && end - start >= lim_.frame_progress_ms()) {   // VSEARLY: frame_stall_ms when set
        policy_failure_.store(true, std::memory_order_release);
        fail(Cause::progress_deadline);
    }
    // First frame (continuous): its calls must finish inside the startup budget.
    if (first_frame && end >= lim_.startup_ms) {
        policy_failure_.store(true, std::memory_order_release);
        fail(Cause::startup_deadline);
    }
    if (rc == Rc::loss_pending) observe_loss(); // qualified success: side effects kept
    if (rc == Rc::session_lost || rc == Rc::instance_lost) { hard_lost_ = true; observe_loss(); }
    return rc;
}

bool Owner::watchdog_tick() noexcept {
    if (!activated_.load(std::memory_order_acquire) || finished()) return false;
    if (policy_failure_.load(std::memory_order_acquire)) return true;
    const Phase p = phase();
    const int64_t now = clock_->now_ms() - activation_atomic_.load(std::memory_order_acquire);
    Cause c = Cause::none;
    if (now >= absolute()) c = Cause::absolute_cap;
    else if ((p == Phase::startup || p == Phase::await_ready) && now >= lim_.startup_ms) c = Cause::startup_deadline;
    else if (p == Phase::frames && first_frame_open_.load(std::memory_order_acquire)) { if (now >= lim_.startup_ms) c = Cause::startup_deadline; }
    else if (p == Phase::frames && now - last_progress_.load(std::memory_order_acquire) >= lim_.frame_progress_ms()) c = Cause::progress_deadline;
    else if (p == Phase::cleanup && now >= cleanup_deadline_.load(std::memory_order_acquire)) c = Cause::cleanup_deadline;
    if (c != Cause::none) {
        // Detection only. No runtime call, no destruction, no thread kill.
        latch(c);
        policy_failure_.store(true, std::memory_order_release);
        stop_.store(true, std::memory_order_release);
        // Additive: an unhealthy owner must not leave a pose readers could use.
        // An atomic store only; the owner thread publishes nothing after a
        // policy failure.
        slot_.retract();
        return true;
    }
    if (p == Phase::frames && now >= admission()) {
        time_cap_requested_.store(true, std::memory_order_release);
        stop_.store(true, std::memory_order_release);
    }
    return false;
}

Report Owner::report() const {
    if (!finished()) return Report{};
    return r_;
}

Report Owner::run() {
    if (used_.exchange(true, std::memory_order_acq_rel)) {
        Report refused;
        refused.complete = true;
        refused.failure_observed = true;
        refused.first_cause = Cause::already_used;
        return refused;
    }
    r_ = Report{};
    if (!cfg_.enabled || !lim_.valid()) {
        // Zero runtime calls, zero clock reads. Default-OFF path.
        latch(cfg_.enabled ? Cause::invalid_limits : Cause::disabled);
        r_.complete = true;
        r_.failure_observed = cfg_.enabled;
        r_.first_cause = first_cause();
        set_phase(Phase::done);
        finished_.store(true, std::memory_order_release);
        return r_;
    }
    try {
        records_.reset(new CallRecord[record_capacity]);
        activation_ = clock_->now_ms();
        activation_atomic_.store(activation_, std::memory_order_release);
        last_progress_.store(0, std::memory_order_release);
        set_phase(Phase::startup);
        activated_.store(true, std::memory_order_release);
        binding_ = true; // the backend's adopted device/queue references
        if (startup() && await_ready()) {
            const Rc rc = call(Op::begin_session, [&] { return rt_->begin_session(); });
            if (succeeded(rc)) { running_ = true; r_.session_begun = true; }
            else if (!failure_ && !loss_) fail(Cause::startup_call);
        }
        if (running_ && !failure_ && !loss_ && !policy_failure_.load()) frames();
        else if (r_.stop == StopReason::none && (failure_ || loss_ || policy_failure_.load())) r_.stop = StopReason::failure;
        cleanup();
    } catch (...) {
        // Owner bookkeeping threw (e.g. allocation). Handles stay retained.
        fail(Cause::exception);
        teardown_ok_ = false;
    }
    r_.complete = true;
    r_.loss_observed = loss_;
    r_.policy_failure = policy_failure_.load(std::memory_order_acquire);
    r_.instance_held = instance_; r_.session_held = session_; r_.session_running = running_;
    r_.local_space_held = local_; r_.view_space_held = view_; r_.binding_held = binding_;
    r_.selected_blend = blend_;
    r_.publish_dropped = static_cast<unsigned>(slot_.dropped());
    r_.consumer_attached = consumer_ != nullptr;
    r_.clean = teardown_ok_ && !instance_ && !session_ && !running_ && !local_ && !view_ && !binding_;
    if (r_.records_lost) fail(Cause::records_lost);
    r_.failure_observed = failure_ || r_.policy_failure;
    r_.first_cause = first_cause();
    const bool orderly = r_.stop == StopReason::requested || r_.stop == StopReason::time_cap ||
        r_.stop == StopReason::frame_cap || r_.stop == StopReason::runtime_stopping;
    r_.success = orderly && !r_.failure_observed && !r_.loss_observed && r_.clean && r_.session_begun &&
        r_.begun > 0 && r_.begun == r_.ended && r_.waited == r_.begun;
    set_phase(Phase::done);
    finished_.store(true, std::memory_order_release);
    if (consumer_) consumer_->finished(r_); // owner thread, after every runtime call
    return r_;
}

bool Owner::set_consumer(std::shared_ptr<FrameConsumer> c) noexcept {
    if (used_.load(std::memory_order_acquire) || launched_.load(std::memory_order_acquire) || consumer_) return false;
    consumer_ = std::move(c);
    return consumer_ != nullptr;
}
bool Owner::set_call_observer(std::shared_ptr<CallObserver> c) noexcept {
    if (used_.load(std::memory_order_acquire) || launched_.load(std::memory_order_acquire) || observer_) return false;
    observer_ = std::move(c);
    return observer_ != nullptr;
}
bool Owner::set_pose_filter(std::shared_ptr<PoseFilter> f) noexcept {
    if (used_.load(std::memory_order_acquire) || launched_.load(std::memory_order_acquire) || pose_filter_) return false;
    pose_filter_ = std::move(f);
    return pose_filter_ != nullptr;
}
void Owner::OwnerGate::sleep_ms(int64_t ms) { o.clock_->sleep_ms(std::clamp<int64_t>(ms, 0, o.lim_.poll_ms)); }
Rc Owner::OwnerGate::invoke(Op op, Rc (*fn)(void*), void* ctx) { return o.call(op, [&] { return fn(ctx); }); }

bool Owner::startup() {
    auto step = [&](Op op, auto fn, bool* owned) -> bool {
        if (stop_requested()) { latch(Cause::operator_stop_before_lifecycle); r_.stop = StopReason::requested; return false; }
        if (policy_failure_.load(std::memory_order_acquire)) return false;
        const Rc rc = call(op, fn);
        if (!succeeded(rc)) { if (!failure_ && !loss_) fail(Cause::startup_call); return false; }
        if (owned) *owned = true; // created even on a qualified success
        return !loss_ && !failure_;
    };
    SystemFacts sys; GraphicsRequirements req; BindingFacts bind; SpaceFacts spaces;
    if (!step(Op::create_instance, [&] { return rt_->create_instance(); }, &instance_)) return false;
    if (!step(Op::query_system, [&] { return rt_->query_system(sys); }, nullptr)) return false;
    if (!sys.hmd_system || !sys.primary_stereo || sys.view_count != 2 || !select_blend(sys, blend_)) {
        r_.compat_stage = CompatStage::system;
        fail(Cause::compatibility); return false;
    }
    // Requirements must be queried before xrCreateSession (XR_KHR_D3D12_enable).
    if (!step(Op::graphics_requirements, [&] { return rt_->graphics_requirements(req); }, nullptr)) return false;
    if (!step(Op::query_binding, [&] { return rt_->query_binding(bind); }, nullptr)) return false;
    if (!compatible(req, bind)) {
        // Keep every compared field of both sides for the refusal log line.
        r_.compat_stage = CompatStage::binding; r_.compat_req = req; r_.compat_bind = bind;
        fail(Cause::compatibility); return false;
    }
    if (!step(Op::create_session, [&] { return rt_->create_session(); }, &session_)) return false;
    if (!step(Op::query_spaces, [&] { return rt_->query_spaces(spaces); }, nullptr)) return false;
    if (!spaces.local || !spaces.view) { r_.compat_stage = CompatStage::spaces; fail(Cause::compatibility); return false; }
    if (!step(Op::create_local_space, [&] { return rt_->create_local_space(); }, &local_)) return false;
    if (!step(Op::create_view_space, [&] { return rt_->create_view_space(); }, &view_)) return false;
    if (consumer_) {
        // One attempt, inside the startup deadline. Partial consumer state is
        // retired (or retained) by cleanup().
        if (stop_requested()) { latch(Cause::operator_stop_before_lifecycle); r_.stop = StopReason::requested; return false; }
        consumer_setup_ = true;
        bool ok = false;
        try { ok = consumer_->setup(gate_); } catch (...) { fail(Cause::exception); }
        if (!ok) { if (!failure_ && !loss_) fail(Cause::consumer); return false; }
        if (loss_ || failure_ || policy_failure_.load(std::memory_order_acquire)) return false;
    }
    return true;
}

bool Owner::await_ready() {
    set_phase(Phase::await_ready);
    for (;;) {
        if (stop_requested()) { latch(Cause::operator_stop_before_lifecycle); r_.stop = StopReason::requested; return false; }
        if (policy_failure_.load(std::memory_order_acquire)) return false;
        // Bounded not-ready outcome (e.g. idle/off-face headset): inconclusive, never success.
        if (elapsed() >= lim_.startup_ms) { fail(Cause::ready_timeout); return false; }
        if (!poll_batch()) return false;
        if (state_ == SessionState::ready) return true;
        if (state_ == SessionState::stopping || state_ == SessionState::exiting) { fail(Cause::exiting); return false; }
        const int64_t now = elapsed();
        if (now >= lim_.startup_ms) continue;
        clock_->sleep_ms(std::clamp<int64_t>(lim_.startup_ms - now, 0, lim_.poll_ms));
    }
}

bool Owner::poll_batch() {
    // Returns false only for a failure/loss observed in THIS batch, so the
    // cleanup handshake can still poll after an earlier (sticky) failure.
    const Cause failure_cause = phase() == Phase::frames ? Cause::frame_call : Cause::startup_call;
    const bool failed_before = failure_, lost_before = loss_;
    auto fresh = [&] { return (failure_ && !failed_before) || (loss_ && !lost_before); };
    for (unsigned i = 0; i < lim_.events_per_batch; ++i) {
        Event e;
        const Rc rc = call(Op::poll_event, [&] { return rt_->poll_event(e); });
        if (rc == Rc::event_unavailable) return !fresh();
        if (!succeeded(rc)) { if (!fresh()) fail(failure_cause); return false; }
        ++r_.events;
        handle(e);
        if (fresh()) return false;
    }
    // A full batch without EVENT_UNAVAILABLE: stop admission rather than drain forever.
    flood_ = true;
    fail(Cause::event_flood);
    return false;
}

void Owner::handle(const Event& e) {
    switch (e.type) {
    case EventType::session_state:
        if (consumer_) try { consumer_->session_event(gate_, e); } catch (...) {}
        if (!e.same_session) { fail(Cause::session_mismatch); return; }
        state_ = e.state;
        if (e.state == SessionState::loss_pending) observe_loss();
        else if (e.state == SessionState::exiting && running_) fail(Cause::exiting);
        break;
    case EventType::instance_loss_pending: observe_loss(); break;
    case EventType::events_lost: fail(Cause::events_lost); break;
    case EventType::reference_space_change:
        if (e.space != SpaceType::local) break; // VIEW/STAGE changes do not affect LOCAL samples
        ++r_.space_changes;
        if (!e.same_session) { fail(Cause::session_mismatch); return; }
        if (change_pending_) { fail(Cause::space_change_overlap); return; } // conservative stop
        change_pending_ = true;
        change_time_ = e.change_time;
        pending_gen_ = space_gen_ + 1;
        break;
    default: break;
    }
}

uint32_t Owner::generation_at(int64_t time) noexcept {
    // Samples at or after changeTime are relative to the new LOCAL origin;
    // earlier samples keep the old generation. Predicted times are monotonic.
    if (change_pending_ && time >= change_time_) { space_gen_ = pending_gen_; change_pending_ = false; }
    // Software recenter: a requested recenter is a generation of its own (never mixed with a pending runtime change, which keeps its changeTime
    // and now follows this generation).
    const uint32_t req = recenter_req_.load(std::memory_order_acquire);
    if (req != recenter_seen_) {
        recenter_seen_ = req;
        ++space_gen_;
        if (change_pending_) pending_gen_ = space_gen_ + 1;
        ++r_.recenters;
    }
    return space_gen_;
}

void Owner::publish(uint64_t frame, const FrameTiming& t, Rc vr, const ViewSample& v, Rc hr, const HeadSample& h) {
    PoseSnapshot s;
    s.frame_index = frame;
    s.predicted_time = t.predicted_time;
    s.predicted_period = t.predicted_period;
    s.should_render = t.should_render;
    s.space_generation = generation_at(t.predicted_time);
    s.views_located = succeeded(vr);
    s.head_located = succeeded(hr);
    if (s.views_located) {
        s.view_count = v.view_count;
        s.view_state_flags = v.view_state_flags;
        s.eyes = v.eyes; // raw; head/IPD already included
        s.eyes_finite = v.view_count == 2 && pose_finite(v.eyes[0].pose) && pose_finite(v.eyes[1].pose) &&
            fov_finite(v.eyes[0].fov) && fov_finite(v.eyes[1].fov);
        ++r_.located;
    }
    if (s.head_located) {
        s.head_flags = h.location_flags;
        s.head = h.pose;
        s.head_finite = pose_finite(h.pose);
    }
    if (s.head_centre_qualified()) ++r_.head_qualified;
    if (!s.views_located && !s.head_located) return;
    s.sequence = ++sequence_;
    if (slot_.try_publish(s)) ++r_.published;
}

void Owner::frames() {
    last_progress_.store(elapsed(), std::memory_order_release);
    if (cfg_.continuous) first_frame_open_.store(true, std::memory_order_release);
    set_phase(Phase::frames);
    for (;;) {
        if (failure_ || loss_ || policy_failure_.load(std::memory_order_acquire)) { r_.stop = StopReason::failure; break; }
        if (stop_requested()) {
            r_.stop = time_cap_requested_.load(std::memory_order_acquire) ? StopReason::time_cap : StopReason::requested;
            break;
        }
        if (!cfg_.continuous && r_.begun >= lim_.max_frames) { r_.stop = StopReason::frame_cap; break; }
        if (elapsed() >= admission()) { r_.stop = StopReason::time_cap; break; }
        if (!poll_batch()) { r_.stop = StopReason::failure; break; }
        if (state_ == SessionState::stopping) { r_.stop = StopReason::runtime_stopping; break; }
        if (stop_requested()) { // re-check: no new wait is admitted after a stop
            r_.stop = policy_failure_.load(std::memory_order_acquire) ? StopReason::failure :
                time_cap_requested_.load(std::memory_order_acquire) ? StopReason::time_cap : StopReason::requested;
            break;
        }

        FrameTiming t;
        const int64_t wait_start = elapsed();
        Rc rc = call(Op::wait_frame, [&] { return rt_->wait_frame(t); });
        const int64_t wait_ms = elapsed() - wait_start;
        if (rc == Rc::refused && !failure_) { r_.stop = StopReason::time_cap; break; }
        if (!succeeded(rc)) { if (!failure_ && !loss_) fail(Cause::frame_call); r_.stop = StopReason::failure; break; }
        ++r_.waited; // wait-returned: an accepted operation that a stop must not erase
        if (hard_lost_) { ++r_.waited_unbegun; r_.stop = StopReason::failure; break; }

        const int64_t begin_start = elapsed();
        rc = call(Op::begin_frame, [&] { return rt_->begin_frame(); });
        const int64_t begin_ms = elapsed() - begin_start;
        if (r_.begun == 0) { r_.first_wait_ms = wait_ms; r_.first_begin_ms = begin_ms; }
        if (!succeeded(rc)) {
            ++r_.waited_unbegun;
            if (!failure_ && !loss_) fail(Cause::frame_call);
            r_.stop = StopReason::failure;
            break;
        }
        const uint64_t frame = ++r_.begun; // begin-accepted: owns exactly one end attempt
        const bool discarded = rc == Rc::frame_discarded;
        if (discarded) ++r_.discarded;

        // Pose readout does not depend on shouldRender.
        ViewSample v; HeadSample h;
        Rc vr = Rc::refused, hr = Rc::refused;
        if (!hard_lost_) vr = call(Op::locate_views, [&] { return rt_->locate_views(t.predicted_time, v); });
        if (!hard_lost_) hr = call(Op::locate_head, [&] { return rt_->locate_head(t.predicted_time, h); });
        const bool locate_failed = (!succeeded(vr) || !succeeded(hr)) && !hard_lost_;
        // NULL-HEAD (test only): one synthetic head turn for the snapshot AND the consumer (same v/h below). generation_at is idempotent for one
        // predicted time, so publish()/its else path return the same generation afterwards.
        if (pose_filter_ && !hard_lost_) pose_filter_->apply(elapsed(), generation_at(t.predicted_time), succeeded(vr), v, succeeded(hr), h);
        if (!loss_ && !failure_ && !policy_failure_.load(std::memory_order_acquire)) publish(frame, t, vr, v, hr, h);
        else generation_at(t.predicted_time);

        if (hard_lost_) { r_.stop = StopReason::failure; break; } // end is not legal on a lost session
        // Additive consumer seam. Without a consumer the plan stays empty and
        // the zero-layer end below is exactly the ladder behaviour.
        LayerPlan plan;
        if (consumer_ && !loss_ && !failure_ && !policy_failure_.load(std::memory_order_acquire)) {
            FrameInfo info;
            info.frame_index = frame;
            info.timing = t;
            info.discarded = discarded;
            info.views = v;
            info.views_valid = succeeded(vr) && v.view_count == 2 && (v.view_state_flags & valid_bits) == valid_bits &&
                pose_finite(v.eyes[0].pose) && pose_finite(v.eyes[1].pose) && fov_finite(v.eyes[0].fov) && fov_finite(v.eyes[1].fov);
            info.wait_ms = wait_ms;
            info.begin_ms = begin_ms;
            info.views_tracked = info.views_valid && (v.view_state_flags & tracked_bits) == tracked_bits;
            info.space_generation = space_gen_; // generation_at(predicted_time) ran in publish()/its else path
            try { consumer_->frame(gate_, info, plan); } catch (...) { plan = LayerPlan{}; fail(Cause::exception); }
            if (hard_lost_) { r_.stop = StopReason::failure; break; } // a consumer call saw SESSION_LOST
        }
        const bool projection = plan.submit && !loss_ && !failure_ && !policy_failure_.load(std::memory_order_acquire);
        const int64_t end_start = elapsed();
        if (projection) rc = call(Op::end_frame_projection, [&] { return rt_->end_frame_projection(t.predicted_time, blend_, plan.layer); });
        else rc = call(Op::end_frame, [&] { return rt_->end_frame_zero_layers(t.predicted_time, blend_); });
        if (frame == 1) r_.first_end_ms = elapsed() - end_start;
        if (succeeded(rc)) { ++r_.ended; if (projection) ++r_.projection_layers; }
        else if (!failure_ && !loss_) fail(Cause::end_frame_failed); // never retried
        if (consumer_) {
            try {
                double late = 0;
                const bool available = rt_->end_lateness_ms(late);
                consumer_->end_diagnostics(elapsed() - begin_start, available, late);
                consumer_->ended(gate_, rc, projection, elapsed() - end_start); } catch (...) { fail(Cause::exception); }
            if (consumer_->failed() && !failure_ && !loss_) fail(Cause::consumer);
        }
        if (locate_failed && !failure_ && !loss_) fail(Cause::frame_call);
        if (frame == 1 && first_frame_open_.load(std::memory_order_acquire)) {
            // The first frame (and its compositor handshake) is complete: the
            // 250 ms progress rule starts now, measured from this point.
            r_.first_frame_done_ms = elapsed();
            last_progress_.store(r_.first_frame_done_ms, std::memory_order_release);
            first_frame_done_atomic_.store(r_.first_frame_done_ms, std::memory_order_release); // XR-START
            first_frame_open_.store(false, std::memory_order_release);
        }
    }
    first_frame_open_.store(false, std::memory_order_release);
}

void Owner::destroy(Op op, bool& held) {
    if (!held) return;
    Rc rc = Rc::error;
    switch (op) {
    case Op::destroy_view_space: rc = call(op, [&] { return rt_->destroy_view_space(); }); break;
    case Op::destroy_local_space: rc = call(op, [&] { return rt_->destroy_local_space(); }); break;
    case Op::destroy_session: rc = call(op, [&] { return rt_->destroy_session(); }); break;
    case Op::release_binding: rc = call(op, [&] { return rt_->release_binding(); }); break;
    case Op::destroy_instance: rc = call(op, [&] { return rt_->destroy_instance(); }); break;
    default: break;
    }
    if (succeeded(rc)) held = false;
    else teardown_ok_ = false; // failed destruction retains the handle and its parents
}

bool Owner::await_stopping() {
    for (;;) {
        if (!poll_batch()) return false;
        if (state_ == SessionState::stopping) return true;
        const int64_t now = elapsed();
        const int64_t deadline = cleanup_deadline_.load(std::memory_order_acquire);
        if (now >= deadline) {
            policy_failure_.store(true, std::memory_order_release);
            fail(Cause::cleanup_deadline);
            return false;
        }
        clock_->sleep_ms(std::clamp<int64_t>(deadline - now, 0, lim_.poll_ms));
    }
}

void Owner::cleanup() {
    const int64_t now = elapsed();
    cleanup_deadline_.store(std::min(absolute(), now + lim_.cleanup_ms), std::memory_order_release);
    set_phase(Phase::cleanup);
    slot_.retract(); // no stale pose survives a stop, loss or failure
    auto incomplete = [&] { teardown_ok_ = false; latch(Cause::teardown_incomplete); };
    auto lost = [&] { return loss_ || hard_lost_; };
    // Consumer first: no frame is in flight any more. It observes outstanding
    // GPU completion, releases images and destroys its swapchains. A refusal
    // means submitted work may still reference an image: the swapchains, the
    // session (whose destruction would destroy them) and every parent are kept.
    bool consumer_retired = true;
    if (consumer_ && consumer_setup_) {
        try { consumer_retired = consumer_->retire(gate_); } catch (...) { consumer_retired = false; fail(Cause::exception); }
    }
    r_.consumer_retired = consumer_ && consumer_setup_ && consumer_retired;
    if (running_ && !lost()) {
        if (state_ != SessionState::stopping) {
            const Rc rc = call(Op::request_exit, [&] { return rt_->request_exit(); });
            if (!succeeded(rc) && !lost()) { incomplete(); return; } // never destroy a running session
            if (!lost() && !await_stopping() && !lost()) { incomplete(); return; }
        }
        if (!lost()) {
            const Rc rc = call(Op::end_session, [&] { return rt_->end_session(); });
            if (succeeded(rc)) running_ = false; // a qualified LOSS_PENDING still ended it
            else if (!lost()) { incomplete(); return; }
        }
    }
    if (!consumer_retired) { incomplete(); return; } // exit handshake done; retain all handles
    // LOSS_PENDING / lost: the session is destroyed without the exit handshake.
    destroy(Op::destroy_view_space, view_);
    if (view_) { incomplete(); return; }
    destroy(Op::destroy_local_space, local_);
    if (local_) { incomplete(); return; }
    destroy(Op::destroy_session, session_);
    if (session_) { incomplete(); return; }
    running_ = false;
    destroy(Op::release_binding, binding_); // dependants (session) settled first
    if (binding_) { incomplete(); return; }
    destroy(Op::destroy_instance, instance_);
    if (instance_) incomplete();
}

// ----------------------------------------------------------------- launch ---
namespace {
struct SteadyClock final : Clock {
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    int64_t now_ms() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count();
    }
    void sleep_ms(int64_t ms) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<int64_t>(ms, 0, Limits{}.poll_ms)));
    }
};
}
std::shared_ptr<Clock> make_steady_clock() { return std::make_shared<SteadyClock>(); }

Launch launch(const std::shared_ptr<Owner>& owner) {
    if (!owner || !owner->enabled()) return Launch::disabled; // no thread, no call
    if (owner->launched_.exchange(true, std::memory_order_acq_rel) || owner->used_.load()) return Launch::already_used;
    try {
        std::thread([owner] { owner->run(); }).detach();
    } catch (...) {
        return Launch::thread_failure;
    }
    try {
        std::thread([owner] {
            // Detection only: exits once finished or once a policy failure is latched.
            while (!owner->finished()) {
                if (owner->watchdog_tick()) return;
                owner->clock_->sleep_ms(owner->lim_.poll_ms);
            }
        }).detach();
    } catch (...) {
        owner->request_stop(); // no watchdog: stop cooperatively
        return Launch::thread_failure;
    }
    return Launch::started;
}

} // namespace acz::xr_owner
