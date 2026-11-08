#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>
#include <string>

namespace acz::probe {
struct ColourBackend;
class ColourOwner;
// phase3_synthetic_stereo (card d): a1, then synthetic scene + pixel readback.
enum class Mode { zero_layers, phase2_dim_clear, phase3_synthetic_stereo };
inline bool colour_mode(Mode m) {return m!=Mode::zero_layers;}
// Stop and new-work admission have one atomic linearization point. A stop may
// coexist with an already admitted non-cancellable call; it never frees it.
class AdmissionGate {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    std::atomic<unsigned> bits{0}; // 1 admitted call, 2 permanently closed
public:
    bool enter(){unsigned empty=0;return bits.compare_exchange_strong(empty,1);}
    void leave(){bits.fetch_and(~1u);}
    void close(){bits.fetch_or(2);}
    bool closed()const{return (bits.load()&2)!=0;}
};
// Startup/outer caps start at activation; cleanup has one entry deadline
// inside the outer cap. Only steady-state progress has a renewable budget.
struct Caps {
    static constexpr int attempts = 1;
    static constexpr int64_t absolute_ms = 15000;
    static constexpr int64_t startup_ms = 5000;
    static constexpr int64_t cleanup_ms = 2000;
    static constexpr int64_t progress_ms = 250; // steady-state foreign-call / file-progress watchdog
    static constexpr int64_t admission_ms = absolute_ms - cleanup_ms;
    static constexpr int64_t poll_ms = 20; // supervisor and file-worker polling cadence
    static constexpr int64_t write_ms = 100; // per-append log I/O wait, below the 250 ms file-progress watchdog (D-207)
    static constexpr unsigned frames = 120;
    static constexpr unsigned log_bytes = 4 * 1024 * 1024;
    static constexpr unsigned enumeration_limit = 1024;
};
enum class Op { instance, system, views, requirements, device, session, formats,
    spaces, create_space, poll, begin_session, wait_frame, begin_frame, end_frame,
    request_exit, end_session, destroy_space, destroy_session, release_queue,
    release_device, destroy_instance, count };
enum class State { idle, ready, running, stopping, exiting, loss };
const char* name(Op);
enum class Phase { startup, frames, cleanup };
inline bool frame_admitted(int64_t elapsed) { return elapsed < Caps::admission_ms; }
struct Reply { bool ok = true; State state = State::idle; bool admission_closed = false;
    bool should_render=true, discarded=false, space_changed=false; };
// A backend call may be foreign and non-cancellable. Only the worker uses it.
// The process supervisor must cover ALL calls, including init and destruction.
struct Backend {
    virtual ~Backend() = default;
    virtual Reply call(Op, unsigned layer_count = 0, const void* layers = nullptr) = 0;
    virtual ColourBackend* colour_backend() {return nullptr;}
    virtual bool frame_calls_legal() const {return true;}
};
struct Clock {
    virtual ~Clock() = default;
    virtual int64_t now() = 0; // monotonic milliseconds since activation
    virtual void sleep(int64_t bounded_ms) = 0;
};
using Log = std::function<void(const std::string&)>;
struct Result {
    bool success = false, clean = false;
    unsigned waited = 0, begun = 0, ended = 0;
    std::string reason;
    int image_exit_code=0;
    bool stereo_pass=false; // card (d) verdict; never set by a1/a2
};
enum class Watch { keep_waiting, request_stop, terminate_failure };
// Pure watchdog policy shared by the real supervisor and fake tests.
Watch watch(int64_t elapsed, bool done);
enum class StopFileStatus { absent, present, unavailable };
// Preflight policy shared with the supervisor; no worker/runtime activation on refusal.
std::string activation_refusal(StopFileStatus);
class Run {
public:
    Run(Backend&, Clock&, Log, std::function<std::string()> stop,
        std::function<void(Phase)> phase = {}, Mode mode = Mode::zero_layers, AdmissionGate* gate=nullptr);
    ~Run();
    Result execute();
    bool cleanup(); // idempotent, partial-init aware
private:
    bool invoke(Op, unsigned layers=0, const void* projection=nullptr);
    bool await(State, int64_t deadline);
    void fail(const std::string&);
    Backend& b; Clock& c; Log log; std::function<std::string()> stop;
    std::function<void(Phase)> phase;
    Result r; State state = State::idle;
    bool used = false, instance = false, device = false, session = false;
    bool space = false, running = false, cleaned = false, cleanup_ok = true;
    bool observed_loss_or_failure = false; // sticky; protocol polls cannot erase it
    int64_t cleanup_deadline = 0;
    Mode mode;
    AdmissionGate* gate;
    std::unique_ptr<ColourOwner> colour;
    bool should_render=true, discarded=false, space_changed=false;
};
}
