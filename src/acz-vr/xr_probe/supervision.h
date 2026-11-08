#pragma once
#include "probe_core.h"
#include <algorithm>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <memory>
#include <utility>

namespace acz::probe {
// Healthy frame upper bound: core entry/exit and native entry/exit for
// poll/wait/begin/end (16), predicted-time and submission (2), session event (1).
// One spare line makes the bound 20. Enumeration refuses counts above 1024;
// reserve six full enumerations, startup READY polls, and bounded shutdown polls.
struct LogBounds {
    static constexpr unsigned lines_per_frame=20;
    static constexpr unsigned startup_lines=6*Caps::enumeration_limit+2048;
    static constexpr unsigned shutdown_lines=1024;
    static constexpr unsigned run_lines=Caps::frames*lines_per_frame+startup_lines+shutdown_lines;
    static constexpr unsigned capacity=run_lines;
    // Explicit a2-only slot reserve for two-eye receipts / bounded fence polls.
    // Phase 1 reserve and the common 4 MiB byte cap remain unchanged.
    // Each fence sample: owner entry/exit + two native call pairs (6).
    // Per eye: acquire(4), two waits(8), record(8), Close(4), authenticated
    // Execute(12), Signal(4), release(4). Locate and policy/core add 6+32.
    static constexpr unsigned colour_frame_lines=6*20+2*(4+8+8+4+12+4+4)+6+32;
    // 2 s cleanup: <=20 samples plus releases/destroy, startup inventory/refusals.
    static constexpr unsigned colour_retire_lines=6*20+2*4+512;
    static constexpr unsigned colour_capacity=capacity+60*colour_frame_lines+colour_retire_lines;
    // Card (d) only, on top of the a2 reserve. Per eye: 13 extra recorded
    // list calls (26), owner+Map/Unmap readback (6), verification eye/marker
    // lines (5); pair (1), first-frame views (2) and spare -> 128 per frame.
    // Setup: shader/root-signature/PSO/vertex/depth/DSV/readback creation.
    static constexpr unsigned stereo_frame_lines=128;
    static constexpr unsigned stereo_setup_lines=512;
    static constexpr unsigned stereo_capacity=colour_capacity+60*stereo_frame_lines+stereo_setup_lines;
};
static_assert(LogBounds::capacity>=Caps::frames*LogBounds::lines_per_frame+
    LogBounds::startup_lines+LogBounds::shutdown_lines,"entire bounded run fits without a drain");
static_assert(LogBounds::colour_capacity>=LogBounds::capacity+60*LogBounds::colour_frame_lines+
    LogBounds::colour_retire_lines,"bounded a2 receipt reserve");
static_assert(LogBounds::stereo_frame_lines>=2*(26+6+5)+1+2 &&
    LogBounds::stereo_capacity>=LogBounds::colour_capacity+60*LogBounds::stereo_frame_lines,"bounded card (d) receipt reserve");
// Standalone worker only. Short blocking locks protect queue metadata/strings;
// no I/O, callback or sleep under the lock. The supervisor NEVER touches it.
// Overflow (slots, bytes or line size) still refuses success. Heap-backed slots
// avoid growing the entry-point stack with the full-run reserve.
class Mailbox {
    std::mutex mutex;
    unsigned capacity;
    std::unique_ptr<std::string[]> lines;
    unsigned first=0, count=0;
    std::size_t bytes=0;
public:
    explicit Mailbox(Mode mode=Mode::zero_layers):capacity(mode==Mode::zero_layers?LogBounds::capacity:
        mode==Mode::phase2_dim_clear?LogBounds::colour_capacity:LogBounds::stereo_capacity),
        lines(std::make_unique<std::string[]>(capacity)) {}
    std::atomic<bool> failed{false};
    void push(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        if(count==capacity || line.size()>16384 || bytes+line.size()>Caps::log_bytes){failed=true;return;}
        lines[(first+count)%capacity]=line;bytes+=line.size();++count;
    }
    bool pop(std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!count)return false;
        bytes-=lines[first].size();line=std::move(lines[first]);
        first=(first+1)%capacity;--count;return true;
    }
    bool empty() {
        std::lock_guard<std::mutex> lock(mutex);
        return count==0;
    }
};
enum class Cause : unsigned { none, worker_create, file_create, logger, worker_deadline,
    file_deadline, absolute_cap, operator_stop, stop_file_error, worker_failure,
    image_ledger, image_runtime, image_command, image_fence, image_release, image_teardown, admission_cap,
    // Card (d) only; appended so earlier values and codes are unchanged.
    stereo_setup, stereo_readback, stereo_pixels, stereo_state };
struct Terminal {
    Cause cause=Cause::none; Phase phase=Phase::startup; unsigned source=0; uint32_t detail=0;
};
// A single CAS publishes the entire first-cause record. No locks, allocation,
// spinning or incomplete-record wait on the enforcement thread.
static_assert(std::atomic<uint64_t>::is_always_lock_free);
inline uint64_t pack_terminal(Terminal t) {
    return static_cast<unsigned>(t.cause) | (uint64_t(static_cast<unsigned>(t.phase))<<8) |
        (uint64_t(t.source)<<16) | (uint64_t(t.detail)<<32);
}
inline Terminal unpack_terminal(uint64_t t) {
    return {static_cast<Cause>(t&255),static_cast<Phase>((t>>8)&255),
        static_cast<unsigned>((t>>16)&255),static_cast<uint32_t>(t>>32)};
}
inline int cause_exit(Cause c) {
    switch(c) {
    case Cause::none:return 0;
    case Cause::worker_create:return 4;
    case Cause::file_create:return 5;
    case Cause::logger:return 6;
    case Cause::worker_deadline:return 7;
    case Cause::file_deadline:return 8;
    case Cause::operator_stop:return 9;
    case Cause::stop_file_error:return 10;
    case Cause::absolute_cap:return 11;
    case Cause::admission_cap:return 11;
    case Cause::image_ledger:return 12;
    case Cause::image_runtime:return 13;
    case Cause::image_command:return 14;
    case Cause::image_fence:return 15;
    case Cause::image_release:return 16;
    case Cause::image_teardown:return 17;
    case Cause::stereo_setup:return 18;
    case Cause::stereo_readback:return 19;
    case Cause::stereo_pixels:return 20;
    case Cause::stereo_state:return 21;
    default:return 1;
    }
}
inline const char* cause_name(Cause c) {
    switch(c) {
    case Cause::none:return "none";
    case Cause::worker_create:return "worker-thread-creation";
    case Cause::file_create:return "file-thread-creation";
    case Cause::logger:return "logger-write";
    case Cause::worker_deadline:return "worker-deadline";
    case Cause::file_deadline:return "file-progress-deadline";
    case Cause::absolute_cap:return "absolute-cap";
    case Cause::admission_cap:return "frame-admission-cap";
    case Cause::operator_stop:return "operator-stop";
    case Cause::stop_file_error:return "stop-file-access";
    case Cause::image_ledger:return "image-ledger";
    case Cause::image_runtime:return "image-runtime-format";
    case Cause::image_command:return "image-command-join";
    case Cause::image_fence:return "image-fence-loss";
    case Cause::image_release:return "image-release";
    case Cause::image_teardown:return "image-teardown";
    case Cause::stereo_setup:return "stereo-scene-setup";
    case Cause::stereo_readback:return "stereo-readback";
    case Cause::stereo_pixels:return "stereo-pixel-verification";
    case Cause::stereo_state:return "stereo-image-state";
    default:return "worker-failure";
    }
}
// image_failure_code=N log receipt -> first-cause category (12..21).
inline Cause image_cause(int code) {
    if(code>=12 && code<=17)return static_cast<Cause>(static_cast<unsigned>(Cause::image_ledger)+code-12);
    if(code>=18 && code<=21)return static_cast<Cause>(static_cast<unsigned>(Cause::stereo_setup)+code-18);
    return Cause::none;
}
inline int64_t clamp_pause(int64_t ms) { return std::clamp<int64_t>(ms,0,Caps::poll_ms); }
template<class Pause> void bounded_pause(Pause pause,int64_t ms) { pause(clamp_pause(ms)); }
template<class KeyState> bool escape_down(KeyState key_state) { return (key_state()&0x8000)!=0; }
struct Signals {
    bool phase2=false; // immutable before threads start; preserves a1 cap disposition
    AdmissionGate admission;
    std::atomic<int> stop{0}; // 1 Escape, 2 file, 3 cap, 4 file/log refusal
    std::atomic<bool> worker_done{false}, logger_done{false}, failed{false};
    std::atomic<uint64_t> progress{0};
    std::atomic<int64_t> file_progress{0};
    // One atomic publishes a complete phase policy: nonnegative = fixed absolute
    // deadline; -1 = steady-state last-progress + 250 ms. No torn phase/time pair.
    std::atomic<int64_t> worker_deadline{Caps::startup_ms};
    std::atomic<Phase> phase{Phase::startup};
    std::atomic<uint64_t> terminal{0};
    std::atomic<int> watchdog_exit{0};
};
inline void latch(Signals& s,Cause cause,unsigned source=0,uint32_t detail=0) {
    uint64_t empty=0;
    s.terminal.compare_exchange_strong(empty,pack_terminal({cause,s.phase.load(),source,detail}));
}
inline std::string terminal_record(const Signals& s) {
    auto t=unpack_terminal(s.terminal.load());
    if(t.cause==Cause::none)t.phase=s.phase.load();
    return std::string("terminal cooperative=1 category=")+cause_name(t.cause)+
        " phase="+std::to_string(static_cast<unsigned>(t.phase))+" stop_source="+std::to_string(t.source)+
        " win32_error_or_wait="+std::to_string(t.detail)+" first_cause_exit_code="+std::to_string(cause_exit(t.cause));
}
inline void logger_failure(Signals& s,uint32_t detail=0) {
    latch(s,Cause::logger,4,detail);s.failed=true;
    int expected=0;s.stop.compare_exchange_strong(expected,4);
    s.admission.close();
}
inline void thread_creation_failure(Signals& s,bool worker,uint32_t error) {
    latch(s,worker?Cause::worker_create:Cause::file_create,0,error);s.failed=true;
    s.worker_done=true;
    if(!worker)s.logger_done=true;
}
inline void observe_phase(Signals& s,Phase phase,int64_t now) {
    s.phase=phase;
    s.worker_deadline=phase==Phase::startup?Caps::startup_ms:
        phase==Phase::frames?-1:std::min(Caps::absolute_ms,now+Caps::cleanup_ms);
}
inline void observe_file(Signals& s,StopFileStatus status,uint32_t error=0) {
    int expected=0;
    if(status==StopFileStatus::unavailable){latch(s,Cause::stop_file_error,4,error);s.failed=true;s.stop.compare_exchange_strong(expected,4);s.admission.close();}
    else if(status==StopFileStatus::present){latch(s,Cause::operator_stop,2);s.stop.compare_exchange_strong(expected,2);s.admission.close();}
}
// The REAL supervisor loop. All callbacks must be nonblocking except the
// bounded sleep. No logging, filesystem calls, mutexes, joins or foreign XR.
// terminate is injected by the offline fixture; production terminates only self.
// Startup: 5 s total from activation, never renewed by progress. Frame phase:
// 250 ms without progress. Cleanup: 2 s total from cleanup entry, never renewed.
// All are inside the 15 s outer cap. Stop observations never renew a deadline.
struct NoIterationHook { void operator()() const {} };
template<class Escape,class Pause,class Terminate,class AfterCheck=NoIterationHook>
bool supervise(Clock& clock,Signals& s,Escape escape,Pause pause,Terminate terminate,AfterCheck after_check={}) {
    uint64_t progress=s.progress.load();
    int64_t last_progress=clock.now();
    for(;;) {
        const auto now=clock.now();
        const bool done=s.worker_done.load() && s.logger_done.load();
        const auto policy=watch(now,false);
        if(policy==Watch::terminate_failure){
            s.admission.close();
            latch(s,Cause::absolute_cap,3);s.watchdog_exit=cause_exit(Cause::absolute_cap);
            s.failed=true;if(!done)terminate();return false;
        }
        if(done)return !s.failed.load();
        int expected=0;
        if(escape()){latch(s,Cause::operator_stop,1);s.stop.compare_exchange_strong(expected,1);s.admission.close();}
        expected=0;
        if(policy==Watch::request_stop){
            if(s.phase2)latch(s,Cause::admission_cap,3);
            s.stop.compare_exchange_strong(expected,3);s.admission.close();
        }
        // Sample the publishing policy before progress. If the worker has
        // published frames, its preceding progress publication is visible too.
        const auto phase_deadline=s.worker_deadline.load();
        const auto current=s.progress.load();
        if(current!=progress){progress=current;last_progress=now;}
        // Immutable per-iteration samples: do not reload phase/progress when
        // calculating the pause. A transition after this snapshot is observed
        // next iteration, at most one bounded poll later.
        const auto deadline=phase_deadline<0?last_progress+Caps::progress_ms:phase_deadline;
        const auto file_progress=s.file_progress.load();
        const bool worker_done=s.worker_done.load(),logger_done=s.logger_done.load();
        // Stop observations never replenish a deadline. Only the owner publishes
        // legal phase transitions; file and worker progress stay independent.
        const bool file_due=!logger_done && now-file_progress>=Caps::progress_ms;
        if(file_due || (!worker_done && now>=deadline)) {
            s.admission.close();
            const auto cause=file_due?Cause::file_deadline:Cause::worker_deadline;
            latch(s,cause,4);s.watchdog_exit=cause_exit(cause);
            s.failed=true;expected=0;s.stop.compare_exchange_strong(expected,4);
            terminate();return false;
        }
        after_check(); // injected regression seam; production is a no-op
        auto delay=std::min<int64_t>(Caps::poll_ms,Caps::absolute_ms-now);
        if(now<Caps::admission_ms)delay=std::min(delay,Caps::admission_ms-now);
        if(!worker_done)delay=std::min(delay,deadline-now);
        if(!logger_done)delay=std::min(delay,Caps::progress_ms-(now-file_progress));
        // Scheduling may have advanced since the snapshot; never sleep past a
        // sampled deadline that is already due. Re-evaluate enforcement now.
        delay-=std::max<int64_t>(0,clock.now()-now);
        if(delay<=0)continue;
        bounded_pause(pause,delay);
    }
}
// Only this thread calls file operations. If either callback never returns,
// supervise still makes its decision using atomics. No unsafe thread kill/join.
template<class Probe,class Write,class Pause>
void file_loop(Clock& clock,Signals& s,Mailbox& box,Probe probe,Write write,Pause pause) {
    try {
    bool stop_reported=false;
    for(;;) {
        observe_file(s,probe());
        s.file_progress=clock.now();
        if(s.stop.load() && !stop_reported){
            stop_reported=true;
            if(!write("stop_observation="+std::to_string(s.stop.load())+"; emergency evidence best effort")){logger_failure(s);box.failed=true;}
            s.file_progress=clock.now();
        }
        std::string line;
        // Drain until empty, yielding to stop-file sampling at the time budget,
        // never after an arbitrary line count. A never-returning write is covered
        // independently by supervise's file-progress watchdog.
        const auto batch_start=clock.now();
        while(box.pop(line)){
            if(!write(line)){logger_failure(s);box.failed=true;}
            s.file_progress=clock.now();
            if(clock.now()-batch_start>=Caps::poll_ms)break;
        }
        if(box.failed.load())logger_failure(s);
        s.file_progress=clock.now();
        if(s.worker_done.load() && box.empty()){
            // Non-enforcement thread only; covered by the unchanged file and
            // absolute deadlines. No terminal write after a known log failure.
            if(!box.failed.load())
                if(!write(terminal_record(s)))logger_failure(s);
            s.logger_done=true;return;
        }
        if(box.empty())bounded_pause(pause,Caps::poll_ms);
    }
    } catch(...) {logger_failure(s);s.logger_done=true;}
}
}
