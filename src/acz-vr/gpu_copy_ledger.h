#pragma once
// J-144, step 1 only. No native adapter, Draw seam, Reset witness or copy exists.
#include <cstdint>
#include <mutex>

namespace acz::gpu_copy {
enum class State { reserved, recorded_ambiguous, submitted, signalled,
                   completed, closed, quarantined };
struct Key {
    // Non-reused lifetime/epoch tokens, never raw addresses. Step 2 must supply
    // authenticated native witnesses; this step accepts only injected fixtures.
    uint64_t device=0, queue=0, fence=0, list=0, generation=0;
    bool valid() const noexcept { return device && queue && fence && list && generation; }
};
struct Admission {
    Key key{};
    uint64_t allocation_bytes=0;
    bool history_complete=false, retired_generation=true;
    bool private_fence=false, authenticated_queue=false, exclusive_signaller=false;
};
struct FenceSample { Key timeline{}; uint64_t value=0; bool valid=false; };
// Injected fake backend only in this step. No COM adapter is provided. The
// backend must outlive its owner, including quarantine. Callbacks are unlocked;
// they may Stop/reenter, but must not destroy the owner or throw.
struct Backend {
    virtual ~Backend()=default;
    virtual FenceSample poll() noexcept=0;
    virtual bool read() noexcept=0;
    virtual void drop_logical_source_lease(uint64_t) noexcept=0;
    virtual void release_private(uint64_t) noexcept=0;
};
struct Snapshot {
    // Diagnostic label only: closure before polling can move closed->completed.
    // Orthogonal fact flags are authoritative; never gate on this label alone.
    State state=State::reserved;
    uint64_t records=0, charged_bytes=0, fence_value=0;
    bool admission=false, source_lease=false, private_held=false;
    bool recording_closed=false, gpu_complete=false, consumed=false, retired=false;
    bool owner_busy=false;
    // J-3b5: static literal naming the FIRST rule that quarantined this record
    // (nullptr while none). Diagnostic only; never an input to any decision.
    const char* fault=nullptr;
};
struct FixtureAccess;
class Session final {
public:
    // One attempt per session, never reset/recycled. Actual allocation bytes
    // (including all private dependencies) must be charged before creation.
    // J-3b7 consciously raises byte_cap 1 MiB -> 128 MiB: step 3 charges its
    // render/depth targets and crop readback (J-3b7b: <= 32 + 32 + 32 MiB plus 64 KiB roundings).
    // Steps 1/2 charge 64 KiB and are unaffected.
    static constexpr uint64_t record_cap=1, byte_cap=256ull*1024*1024; // J-3b7c: full-target readback (<= 32+32+160 MiB)
    Session() noexcept=default;
    ~Session()=default; // Deliberately NO foreign cleanup, including at detach.
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    // Explicit diagnostic-owner entrypoints: caller runs outside loader lock.
    // Stop closes experimental admission only; saved native forwarding stays open.
    void stop() noexcept;
    bool poll() noexcept;
    bool read() noexcept;
    bool retire() noexcept;
    // Atomically closes reservations and vetoes drain for pending/quarantined
    // records or any owner lease. Success is irrevocable, including switch OFF.
    bool begin_global_drain() noexcept;
    Snapshot snapshot() const noexcept;
private:
    friend struct FixtureAccess;
    friend class Witness;
    // Part 2: the default-OFF step-1 experiment owner in d3d12_hook.cpp arms
    // the shared diagnostic session through this narrow friend only.
    friend struct Step1Owner;
    bool arm(bool, Backend&, const Admission&) noexcept;
    bool reserve(const Admission&) noexcept;
    // Takes already-owned dependencies; never AddRef a historical pointer.
    // true means the first completion for an existing reservation was accepted:
    // ownership of every nonzero dependency transferred, even on failure, Stop
    // or quarantine. It does NOT authorize recording; possible_recording() does.
    // Failed/incomplete/stopped/uncertain completions remain charged quarantined
    // tombstones forever. false means no reservation or a duplicate completion:
    // nothing transferred or changed; the caller retains any supplied owners.
    // Receipt is consumed even if both dependencies are zero; no creation retry.
    bool created(bool success, uint64_t private_output, uint64_t logical_source) noexcept;
    bool possible_recording() noexcept;
    uint64_t begin_execute(const Key&, unsigned occurrences) noexcept;
    bool end_signal(uint64_t join, bool success) noexcept;
    bool close_recording(const Key&) noexcept; // injected closure, NOT a Reset witness
    void uncertain(const char* why="uncertain") noexcept;
    void quarantine_locked(const char* why) noexcept;
    mutable std::mutex mutex_;
    Backend* backend_=nullptr;
    Admission expected_{};
    Snapshot record_{};
    uint64_t source_=0, private_=0, last_observed_=0;
    unsigned polls_=0; // hard bounded, nonblocking owner work; no automatic retry
    bool armed_=false, stopped_=false, draining_=false, join_active_=false;
    bool submitted_=false, signalled_=false, possible_=false, creation_received_=false;
};
// Shared production owner. Inert/default OFF; no production arming API yet.
// Reachable Stop/poll/read/retire calls never require DllMain or a worker thread.
Session& diagnostic_session() noexcept;
} // namespace acz::gpu_copy
