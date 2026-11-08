#pragma once
#include "gpu_copy_ledger.h"

namespace acz::gpu_copy {
// Installation attestations, NOT an absence-of-events inference. Each alias
// denotes its complete applicable vtable, including inherited restrictions.
enum class Coverage : unsigned {
    predication, begin_query, end_query, resolve_query, begin_pass, end_pass,
    pass_context, protected_session, reset, clear_state, bundles, indirect,
    direct_type, creation, allocator, list, queue, device_node,
    alias0, alias1, alias2, alias3, alias4, alias5, alias6, alias7, alias8,
    alias9, alias10, count
};
constexpr uint64_t coverage_bit(Coverage c) noexcept {
    return uint64_t{1} << static_cast<unsigned>(c);
}
constexpr uint64_t required_coverage=(uint64_t{1} << static_cast<unsigned>(Coverage::count))-1;
struct Identity {
    uint64_t device=0, queue=0, fence=0, list=0, allocator=0, creation=0, node=0;
    bool direct=false;
};
enum class Event { predication_on, predication_off, begin_query, end_query,
    resolve_query, begin_pass, end_pass, suspended_pass, resumed_pass,
    cross_list_pass, protected_change, bundle, indirect, clear_state };
struct WitnessSnapshot {
    uint64_t generation=0, coverage=0;
    bool enabled=false, ambiguous=false, open=false, tracked=false, pending=false;
    // Part 2 evidence/refusal classification (read-only copies).
    uint64_t closed_generation=0;
    bool predicated=false, queries=false, pass=false, suspended=false, excluded=false;
    // J-3b5 diagnostics (never decision inputs). `fault`: static literal of the
    // FIRST rule that made this witness ambiguous while its transaction was
    // still attached (nullptr while none), with the generation facts at that
    // moment. `detached`: the attached transaction's own Execute+Signal joined
    // and an observed successful Reset then closed its recording, so later
    // generations of this list can no longer contain the copy; their Executes
    // (`later_executes`) and faults (`post_closure_faults`) no longer reach
    // the ledger.
    const char* fault=nullptr;
    uint64_t fault_generation=0, fault_closed_generation=0, recorded_generation=0;
    unsigned faults=0, later_executes=0, post_closure_faults=0;
    bool fault_open=false, detached=false;
};
struct WitnessFixtureAccess;
// One list lifetime, one retained transaction. The ownership-gated observation
// feed can enable after complete identity authentication; production ownership
// is unavailable in 3a. Transaction attachment remains fixture-only. No native
// calls or Draw integration exist in this class.
class Witness final {
public:
    Witness() noexcept=default;
    Witness(const Witness&)=delete;
    Witness& operator=(const Witness&)=delete;
    void coverage(Coverage, bool supported) noexcept;
    // Identity must be authenticated by the creation/Reset adapter. Part 2:
    // the allocator token may change between Resets (games rotate per-frame
    // allocators); it must still be an authenticated DIRECT allocator token.
    // Every other identity field must stay identical for the list lifetime.
    void reset(bool native_success, const Identity&) noexcept;
    void close(bool native_success) noexcept;
    // Observation loss/overlap is ambiguity, never a fabricated native result.
    void uncertain(const char* why="uncertain") noexcept;
    void event(Event) noexcept;
    void alias(unsigned version) noexcept;
    bool insertion_allowed(const Identity&) const noexcept;
    // Part 2: the witness's own authenticated identity and current generation,
    // only while insertion would be allowed for exactly that identity.
    bool candidate(Identity& out, uint64_t& generation) const noexcept;
    // Key identifies the submitted OLD recording, never the current generation.
    // Caller must register before forwarding Execute; always forward native calls
    // exactly once even if observation refuses (zero is returned).
    // Signal result is supplied after native forwarding.
    uint64_t before_execute(const Key&, unsigned occurrences, bool authenticated) noexcept;
    bool after_signal(uint64_t ticket, bool native_success) noexcept;
    WitnessSnapshot snapshot() const noexcept;
private:
    friend class ObservationFeed;
    friend struct WitnessFixtureAccess;
    bool attach(Session&, const Key&) noexcept;
    bool allowed(const Identity&) const noexcept;
    void fail(const char* why) noexcept;
    mutable std::mutex mutex_;
    bool enabled_=false, ambiguous_=false, open_=false, predicated_=false;
    bool pass_=false, suspended_=false, excluded_=false;
    uint64_t closed_generation_=0; // successful native Close, never inferred from Reset
    uint64_t installed_=0, epoch_coverage_=0, generation_=0, queries_=0, pending_=0;
    Identity identity_{};
    Session* ledger_=nullptr;
    Key recorded_{};
    // J-3b5: the transaction's own join completed (after_signal success), and
    // its recording was then closed by an observed successful Reset.
    bool joined_=false, detached_=false, fault_open_=false;
    const char* fault_=nullptr;
    uint64_t fault_generation_=0, fault_closed_=0;
    unsigned faults_=0, later_executes_=0, post_closure_faults_=0;
};
} // namespace acz::gpu_copy
