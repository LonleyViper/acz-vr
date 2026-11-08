#include "probe_core.h"
#include "colour_owner.h"
#include <algorithm>
#include <sstream>
#include <thread>

namespace acz::probe {
std::string activation_refusal(StopFileStatus status) {
    if (status == StopFileStatus::present) return "stop file already exists at launch";
    if (status == StopFileStatus::unavailable) return "stop file status unavailable at launch";
    return {};
}
const char* name(Op op) {
    static const char* names[] = {"instance", "system", "views", "requirements",
        "device", "session", "formats", "spaces", "create_space", "poll",
        "begin_session", "wait_frame", "begin_frame", "end_frame", "request_exit",
        "end_session", "destroy_space", "destroy_session", "release_queue",
        "release_device", "destroy_instance"};
    return names[static_cast<unsigned>(op)];
}
Watch watch(int64_t elapsed, bool done) {
    if (done) return Watch::keep_waiting;
    if (elapsed >= Caps::absolute_ms)
        return Watch::terminate_failure;
    if (elapsed >= Caps::admission_ms) return Watch::request_stop;
    return Watch::keep_waiting;
}
Run::Run(Backend& backend, Clock& clock, Log sink, std::function<std::string()> token,
    std::function<void(Phase)> observe_phase, Mode selected, AdmissionGate* admission)
    : b(backend), c(clock), log(sink), stop(token), phase(observe_phase), mode(selected), gate(admission) {}
Run::~Run()=default; // ColourOwner performs no foreign cleanup
void Run::fail(const std::string& why) { observed_loss_or_failure = true; log("failure_reason="+why); if (r.reason.empty()) r.reason = why; }
bool Run::invoke(Op op,unsigned layers,const void* projection) {
    if(op==Op::end_frame && colour_mode(mode) && !b.frame_calls_legal()) {
        fail("actual session/instance loss; frame end no longer legal");return false;
    }
    auto start = c.now();
    if (start >= Caps::absolute_ms) { fail("absolute time cap"); log("FAILURE call refused at absolute cap"); return false; }
    if (op == Op::wait_frame && !frame_admitted(start)) {
        r.reason = "time cap"; log("frame admission refused at 13 s"); return false;
    }
    std::ostringstream entry;
    entry << "core entry=" << name(op) << " ms=" << start << " thread=" << std::this_thread::get_id();
    log(entry.str());
    if (c.now() >= Caps::absolute_ms) { fail("absolute time cap"); return false; }
    if (op == Op::wait_frame && !frame_admitted(c.now())) {
        r.reason = "time cap"; log("frame admission refused at 13 s"); return false;
    }
    struct Grant {AdmissionGate* g=nullptr;~Grant(){if(g)g->leave();}} grant;
    if(op==Op::wait_frame && colour_mode(mode) && gate) {
        if(!gate->enter()){
            r.reason=stop();if(r.reason.empty())r.reason=frame_admitted(c.now())?"stop requested":"time cap";
            log("frame admission stopped reason="+r.reason);return false;
        }
        grant.g=gate;
    }
    auto reply = b.call(op, layers, projection);
    if(op==Op::wait_frame)should_render=reply.should_render;
    if(op==Op::begin_frame)discarded=reply.discarded;
    space_changed |= reply.space_changed;
    log(std::string("core exit=") + name(op) + " ms=" + std::to_string(c.now()) +
        " duration_ms=" + std::to_string(c.now() - start) + " ok=" + (reply.ok ? "1" : "0"));
    if (reply.state == State::loss || reply.state == State::exiting) {
        observed_loss_or_failure = true;
        log("failure_observation=session lost/exiting (sticky)");
    }
    if (op == Op::poll || reply.state == State::loss || reply.state == State::exiting) state = reply.state;
    if (reply.admission_closed && op == Op::wait_frame) {
        r.reason = "time cap"; log("frame admission refused at 13 s"); return false;
    }
    if (!reply.ok) fail(std::string("backend failure: ") + name(op));
    return reply.ok;
}
bool Run::await(State wanted, int64_t deadline) {
    while (c.now() < deadline) {
        if (!invoke(Op::poll)) return false;
        if (state == wanted) return true;
        if (state == State::loss || state == State::exiting) { fail("session lost/exiting"); return false; }
        if (wanted == State::ready) {
            auto why = stop();
            if (!why.empty()) { fail(why); return false; }
        }
        c.sleep(std::max<int64_t>(0, std::min(Caps::poll_ms, deadline - c.now())));
    }
    fail(wanted == State::ready ? "startup timeout" : "STOPPING timeout");
    return false;
}
Result Run::execute() {
    if (used) { Result duplicate; duplicate.reason = "single attempt: retry refused"; return duplicate; }
    used = true;
    log("card=a phase=1 attempts="+std::to_string(Caps::attempts)+" absolute_ms="+std::to_string(Caps::absolute_ms)+
        " startup_ms="+std::to_string(Caps::startup_ms)+" cleanup_ms="+std::to_string(Caps::cleanup_ms)+
        " frame_progress_ms="+std::to_string(Caps::progress_ms)+" frames="+std::to_string(Caps::frames)+
        " poll_ms="+std::to_string(Caps::poll_ms)+" layers=0 xrWaitFrame=non-cancellable");
    auto init = [&](Op op, bool* owned = nullptr) {
        if (c.now() >= Caps::startup_ms) { fail("startup timeout"); return false; }
        auto token = stop();
        if (!token.empty()) { fail(token); return false; }
        // Even a failed multi-call stage may have created a handle. Backends
        // make destruction of absent handles a no-op.
        if (owned && op != Op::begin_session) *owned = true;
        if (!invoke(op)) return false;
        if (owned) *owned = true;
        if (observed_loss_or_failure) return false;
        if (c.now() >= Caps::startup_ms) { fail("startup timeout"); return false; }
        return true;
    };
    if (init(Op::instance, &instance) && init(Op::system) && init(Op::views) &&
        init(Op::requirements) && init(Op::device, &device) && init(Op::session, &session) &&
        init(Op::formats) && init(Op::spaces) && init(Op::create_space, &space) &&
        await(State::ready, Caps::startup_ms) && init(Op::begin_session, &running)) {
        if (phase) phase(Phase::frames);
        bool dim=false;
        unsigned dim_begun=0;
        while (true) {
            if (state == State::loss || state == State::exiting) { fail("session lost/exiting"); break; }
            auto why = stop();
            if (!why.empty()) { r.reason = why; break; }
            if (!frame_admitted(c.now())) { r.reason = "time cap"; break; }
            if (!dim && r.begun >= Caps::frames) {
                if(mode==Mode::zero_layers){r.reason="frame cap";break;}
                // a1 must finish its full balanced zero-layer budget before a2.
                if(observed_loss_or_failure || r.waited!=r.begun || r.begun!=r.ended || space_changed) {
                    fail("a1 not clean; a2 refused");break;
                }
                auto* cb=b.colour_backend();
                if(!cb){log("image_failure_code=12 reason=a2 backend unavailable");r.image_exit_code=12;fail("a2 backend unavailable");break;}
                colour=std::make_unique<ColourOwner>(*cb,c,log,stop,gate,mode==Mode::phase3_synthetic_stereo);
                if(!colour->setup()){
                    r.image_exit_code=colour->failure_code();
                    if(!colour->stop_reason().empty())r.reason=colour->stop_reason();else fail("a2 setup");break;
                }
                dim=true;log(mode==Mode::phase3_synthetic_stereo?"card=d phase=synthetic-stereo frames=60 a1_balanced=120 pixel_verification=1":
                    "card=a2 phase=dim-clear frames=60 a1_balanced=120");
            }
            if(dim && dim_begun>=60){r.reason="frame cap";break;}
            if (!invoke(Op::poll)) break;
            if(colour_mode(mode) && space_changed){fail("reference-space change; stop without stale space");break;}
            if (state == State::stopping) { r.reason = "runtime STOPPING"; break; }
            if (state == State::exiting || state == State::loss) { fail("session lost/exiting"); break; }
            // xrWaitFrame cannot be cancelled. Supervisor terminates this harness
            // (never a foreign process/thread) at the 15 s total cap or the
            // earlier 250 ms no-progress watchdog.
            if (!invoke(Op::wait_frame)) break;
            ++r.waited;
            if (!invoke(Op::begin_frame)) break;
            ++r.begun;
            Projection projection{};
            if(dim) {
                ++dim_begun;
                if(!observed_loss_or_failure && !space_changed && !discarded)
                    projection=colour->render(r.begun,should_render);
                if(colour->failed())r.image_exit_code=colour->failure_code();
                if(!colour->stop_reason().empty())r.reason=colour->stop_reason();
            }
            if(dim && (!stop().empty() || observed_loss_or_failure || space_changed))projection={};
            // An accepted begin, including FRAME_DISCARDED/loss-pending, owns
            // one end attempt. No stale/one-eye projection and no end retry.
            if (!invoke(Op::end_frame,projection.sample?1u:0u,projection.sample?&projection:nullptr)) break;
            ++r.ended;
            if(dim && !colour->stop_reason().empty())break;
            if(dim && (observed_loss_or_failure || colour->failed())){fail("frame/image failure");break;}
        }
    }
    // Preserve session-begin evidence before cleanup clears running. Clean
    // retirement alone is not evidence that a lifecycle actually ran.
    const bool session_begun = running;
    const bool operator_stop = r.reason == "Escape" || r.reason == "stop file";
    if (operator_stop && (!session_begun || r.begun == 0)) {
        r.reason = "ABORTED operator stop before lifecycle: " + r.reason;
        log(r.reason);
    }
    bool retired = cleanup();
    if(colour && colour->failed())r.image_exit_code=colour->failure_code();
    r.clean = retired && c.now() <= Caps::absolute_ms;
    bool orderly = r.reason == "frame cap" || r.reason == "time cap" ||
        r.reason == "Escape" || r.reason == "stop file" || r.reason == "stop requested" || r.reason == "runtime STOPPING";
    r.success = !observed_loss_or_failure && r.clean && orderly && session_begun && r.begun > 0 &&
        r.begun == r.ended && r.waited == r.begun;
    if(colour_mode(mode))r.success &= colour && !colour->failed() && r.begun>Caps::frames;
    log("balance wait=" + std::to_string(r.waited) + " begin=" + std::to_string(r.begun) +
        " end=" + std::to_string(r.ended) + " abort_reason=" + r.reason);
    log(std::string(r.success ? "SUCCESS " : "FAILURE ") + (r.clean ? "clean-retirement" : "unclean-retirement"));
    if(mode==Mode::phase3_synthetic_stereo) {
        const auto verdict=colour?colour->stereo_verdict(r.success):
            "SYNTHETIC_STEREO_RESULT=FAIL reason=synthetic stereo phase never started frames_rendered=0 frames_verified=0";
        r.stereo_pass=verdict.rfind("SYNTHETIC_STEREO_RESULT=PASS ",0)==0;
        log(verdict);
    }
    return r;
}
bool Run::cleanup() {
    if (cleaned) return cleanup_ok;
    cleaned = true;
    if (phase) phase(Phase::cleanup);
    cleanup_deadline = std::min(Caps::absolute_ms, c.now() + Caps::cleanup_ms);
    if(colour && !colour->retire(cleanup_deadline)) {
        r.image_exit_code=colour->failure_code()?colour->failure_code():17;
        cleanup_ok=false;fail("XR images/dependencies quarantined; parent handles retained");return false;
    }
    auto destroy = [&](Op op, bool& exists) {
        if (!exists) return;
        if (c.now() >= cleanup_deadline) { cleanup_ok = false; fail("cleanup timeout"); return; }
        bool ok = invoke(op); if (ok) exists = false; cleanup_ok = ok && cleanup_ok;
    };
    if (running) {
        if (state != State::stopping) {
            if (c.now() >= cleanup_deadline || !invoke(Op::request_exit) ||
                !await(State::stopping, cleanup_deadline)) {
                cleanup_ok = false;
                // Never destroy a still-running session with unproven retirement.
                log("FAILURE running session retained until harness process exit");
                return false;
            }
        }
        if (c.now() >= cleanup_deadline || !invoke(Op::end_session)) {
            cleanup_ok = false; return false;
        }
        running = false;
    }
    destroy(Op::destroy_space, space);
    if (space) return cleanup_ok = false;
    destroy(Op::destroy_session, session);
    if (!session && device) {
        if (c.now() >= cleanup_deadline || !invoke(Op::release_queue)) return cleanup_ok = false;
        destroy(Op::release_device, device);
    }
    // D3D objects were created after the instance, hence released before it.
    if (!session && !device) destroy(Op::destroy_instance, instance);
    if (space || session || device || instance || c.now() > cleanup_deadline) cleanup_ok = false;
    return cleanup_ok;
}
}
