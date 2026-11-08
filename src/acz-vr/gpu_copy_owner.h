#pragma once
// GPU-copy step 3b part 1 (offline, default OFF, fake-driven).
//
// Part 2 (J-3b2): the production DLL constructs ONE Registry and ONE Timeline
// inside the default-OFF step-1 experiment owner (gpu_copy_step1_hooks.inl) and
// enables them only when gpu_copy_step1_experiment and its diagnostics opt-ins
// are all latched. With the experiment OFF nothing here is constructed.
// It supplies the owner pieces Astra's issue-28 review required before any
// native copy: (1) an authenticated ownership provider fed ONLY by positively
// witnessed creation records, (2) a private proxy-owned fence on one
// authenticated timeline with a retained Execute batch record, (3) the
// in-flight restriction window, and (4) the interface that part 2 (the real
// CopyBufferRegion record and READBACK Map) will implement. No D3D12 type,
// COM call or Windows header appears here; every native effect is behind an
// abstract seam that is a fake in tests.
//
// Lock order (all leaves downstream, no callback under any of these mutexes):
//   hook-state -> ObservationFeed -> Witness -> Session (ledger)
//   hook-state -> Timeline -> Registry
//   ObservationFeed -> Registry (provider callbacks)
// Registry never calls out. Timeline calls FenceBackend only with no lock held.
#include "gpu_copy_observation.h"
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace acz::gpu_copy::owner {
// A native interface address. Routing argument ONLY: never a token, never
// compared across lifetimes, never dereferenced or dispatched by this module.
using Route = uintptr_t;
template<class T> Route route(const T* p) noexcept {return reinterpret_cast<Route>(p);}

enum class Kind : uint8_t {none, device, queue, allocator, list, fence};
// Diagnostic outcome of one creation witness (J-3b3). Never an input to any
// ownership decision; the registry state alone decides lookups.
//   off        registry disabled or null route (nothing recorded)
//   recorded   live, non-ambiguous record with fresh tokens
//   failed     unsuccessful creation with a published output (D-171 tombstone)
//   collision  route already had a live/ambiguous record (missed Release: tombstone)
//   parent     parent device/allocator not live-witnessed, or token mint failed (ambiguous)
//   exhausted  capacity exhausted (registry saturated)
//   repeated   device only: same live device returned again (token kept)
enum class Created : uint8_t {off, recorded, failed, collision, parent, exhausted, repeated};

struct OwnerFixtureAccess;

// ---------------------------------------------------------------------------
// (1) Authenticated ownership registry and production provider.
//
// Creation witnesses are called by a future creation wrapper AFTER the saved
// native creation returned, outside every hook/feed mutex, with the actual
// result. A failed creation that still published a non-null output tombstones
// that route (D-171). A creation at a route that already has a live record
// means a Release was missed: both become an ambiguous tombstone (devices are
// the documented exception, see device_created). Tokens are minted from one
// monotonic counter, never derived from an address, never reused.
//
// The provider callbacks are PURE lookups: one leaf mutex, fixed scans, no
// writes, no QI/AddRef/Release, no native dispatch, no allocation, no callback.
// Storage is a fixed array inside the object (heap/static only; ~7 KiB).
class Registry final {
public:
    // Exhausting capacity saturates the registry: every later lookup refuses,
    // because an unrecorded failed-creation tombstone could otherwise be lost.
    // Part 2: 1024 (was 128). Live play creates hundreds of lists (L3f: 556
    // CreateCommandList observations); released slots are reusable.
    static constexpr unsigned capacity=1024;
    Registry() noexcept=default;
    // One-way production enable (step-1 experiment owner only). Never disabled.
    void enable() noexcept;
    Registry(const Registry&)=delete;
    Registry& operator=(const Registry&)=delete;

    // ASSUMPTION (to validate against the SDK): D3D12CreateDevice returns the
    // existing device for an adapter, so a repeated success at a LIVE device
    // route keeps its token. Every other kind treats it as a missed Release.
    Created device_created(bool success, Route device) noexcept;
    Created queue_created(bool success, Route queue, Route device, uint32_t node_mask, bool direct) noexcept;
    Created allocator_created(bool success, Route allocator, Route device, bool direct) noexcept;
    // ID3D12Device::CreateCommandList: the list is created OPEN on `allocator`,
    // which must itself be a live witnessed allocator of the same device/type.
    Created list_created(bool success, Route list, Route device, Route allocator,
                         uint32_t node_mask, bool direct) noexcept;
    // J-3b3: ID3D12Device4::CreateCommandList1 creates the list CLOSED with no
    // allocator; the only parent is the live witnessed device. Its first
    // recording still needs an observed successful Reset on a witnessed
    // allocator (lookup_reset), so nothing about allocators is assumed here.
    Created closed_list_created(bool success, Route list, Route device,
                                uint32_t node_mask, bool direct) noexcept;
    // Proxy-owned fence: only Timeline::create reports it.
    Created fence_created(bool success, Route fence, Route device) noexcept;
    // Final Release witnessed before the outgoing native Release. An ambiguous
    // tombstone stays ambiguous: its lifetime was never authenticated.
    void released(Route) noexcept;
    // Lost coverage for a route (e.g. displaced alias, certification failure).
    void uncertain(Route) noexcept;

    // Binds the single private timeline: a live DIRECT queue and a live
    // proxy-owned fence on the same device. One binding per Registry lifetime;
    // never rebound, even after loss.
    bool bind_timeline(Route queue, Route fence) noexcept;
    struct TimelineFacts {uint64_t device=0, queue=0, fence=0, node=0; bool bound=false, lost=false;};
    TimelineFacts timeline() const noexcept;
    void timeline_lost() noexcept;

    // Pure lookups. live_token: 0 unless a live, non-ambiguous record of kind.
    uint64_t live_token(Route, Kind) const noexcept;
    Route device_route_of(Route queue) const noexcept;
    bool lookup_reset(Route list, Route allocator, Identity& out) const noexcept;
    bool lookup_execute(Route queue, const Key& recorded, Key& out) const noexcept;
    // Diagnostic classification of why lookup_reset would refuse (pure lookup,
    // no write). Bits: 1 disabled, 2 saturated, 4 timeline lost, 8 unbound,
    // 16 list unwitnessed, 32 list ambiguous, 64 allocator unwitnessed,
    // 128 allocator ambiguous, 256 list not DIRECT/other device,
    // 512 allocator not DIRECT/other device, 1024 node mismatch. 0 = accepted.
    unsigned reset_refusal(Route list, Route allocator) const noexcept;
    unsigned live_records() const noexcept;
    // Pass to ObservationFeed. Context is this registry; it must outlive the feed.
    OwnershipProvider provider() noexcept;

    unsigned exhausted() const noexcept;     // saturating creation-capacity refusals
    unsigned tombstones() const noexcept;    // ambiguous routes
private:
    friend struct OwnerFixtureAccess;
    struct Record {
        Route route=0;
        uint64_t token=0, device=0, node=0, creation=0, allocator=0;
        uint32_t node_mask=0;
        Kind kind=Kind::none;
        bool direct=false, live=false, ambiguous=false;
    };
    Record* find_locked(Route) noexcept;
    const Record* find_locked(Route) const noexcept;
    const Record* live_locked(Route, Kind) const noexcept;
    const Record* token_locked(uint64_t, Kind) const noexcept;
    Record* claim_locked(Route) noexcept;
    void tombstone_locked(Route) noexcept;
    uint64_t mint_locked() noexcept;
    Created create_locked(bool success, Route, Kind, const Record& facts, bool parent_ok) noexcept;
    mutable std::mutex mutex_;
    Record records_[capacity]{};
    uint64_t next_=0, bound_queue_=0, bound_fence_=0, bound_device_=0, bound_node_=0;
    unsigned exhausted_=0;
    bool enabled_=false, bound_ever_=false, lost_=false, saturated_=false;
};

// ---------------------------------------------------------------------------
// (2) Private proxy-owned fence on one authenticated timeline.
//
// FenceBackend is the native seam (fake in tests). Part 2 maps it to
// CreateFence(0, D3D12_FENCE_FLAG_NONE), ID3D12CommandQueue::Signal on the
// bound queue, and ID3D12Fence::GetCompletedValue. There is deliberately NO
// CPU-side Signal entry: the timeline is exclusively GPU-signalled.
struct FenceBackend {
    virtual ~FenceBackend()=default;
    // `fence` may be non-null even on failure; it is then tombstoned/retained.
    virtual bool create(Route device, Route& fence) noexcept=0;
    virtual bool signal(Route queue, Route fence, uint64_t value) noexcept=0;
    virtual uint64_t completed(Route fence) noexcept=0;
};

enum class BatchState : uint8_t {none, registered, signalled, signal_failed, quarantined};
// Small, fixed copy of the retained record (members are read individually).
struct BatchSummary {
    uint64_t ticket=0, value=0, queue=0, list=0, generation=0, submission=0;
    uint32_t count=0, ordinal=0, occurrences=0;
    BatchState state=BatchState::none;
    bool unknown=false, duplicate=false, cross_queue=false, reentrant=false;
    bool oversize=false, unclosed=false, unauthenticated=false, re_executed=false;
};
// Inputs from the Execute wrapper, BEFORE native forwarding. `lists` are the
// canonical list routes in the exact submitted order. The generation facts
// come from the candidate list's Witness (current recorded generation and the
// generation of its last successful native Close).
struct BatchInput {
    Route queue=0;
    unsigned count=0;
    const Route* lists=nullptr;
    uint64_t candidate_generation=0, closed_generation=0;
    // Part 2: the caller attests every route is the CANONICAL key from the hook's
    // certified alias map (all 11 variants + 3 ancestors + IUnknown of the
    // recorded list map to one key). An unwitnessed route is then a different
    // object, not an alias of the recording, and does not quarantine. It is
    // still counted (BatchSummary::unknown) for evidence. Default false keeps
    // the part-1 conservative rule.
    bool aliases_certified=false;
};
struct PollResult {uint64_t value=0; bool sampled=false, lost=false, timed_out=false;};

class Timeline final {
public:
    static constexpr unsigned batch_cap=64, poll_cap=50;
    Timeline() noexcept=default;
    Timeline(const Timeline&)=delete;
    Timeline& operator=(const Timeline&)=delete;
    void enable() noexcept; // one-way production enable (step-1 owner only)
    // Part 2: armed from the ledger's attached key at the Draw seam (after the
    // ledger reservation, before the possible recording). One arm per lifetime.
    // Members are matched by authenticated list token, never by address.
    bool arm(const Key& recorded) noexcept;

    // Outside-loader-lock owner only. One attempt: creates the proxy fence
    // (no lock held), reports it to the registry and binds the timeline.
    bool create(Registry&, FenceBackend&, Route queue) noexcept;
    // Stop closes new arming only. A recording that may already exist is still
    // joined and Signalled so its completion can be established later.
    void stop() noexcept;

    // Registration BEFORE native Execute. Returns the nonzero ticket only for
    // an authenticated, single-occurrence, fully known, closed-generation batch
    // on the bound queue. Any batch that might submit the retained recording in
    // any other way quarantines it permanently and returns 0. The caller must
    // forward the original Execute exactly once regardless of the result.
    uint64_t begin(const BatchInput&) noexcept;
    // AFTER native Execute returned: issues exactly one queue Signal with the
    // ticket's value on the bound queue (no lock held), records the result.
    // Must be called once for every nonzero ticket; a repeated call is refused
    // without a native Signal.
    bool signal(uint64_t ticket) noexcept;
    // Owner poll, bounded (poll_cap), nonblocking. UINT64_MAX is loss, never
    // completion; regression or a value beyond the last issued value (another
    // signaller) is loss. Loss is permanent and marks the registry timeline lost.
    PollResult poll() noexcept;
    bool complete(uint64_t ticket) const noexcept;
    // For the ledger Backend::poll seam (same authenticated tokens).
    FenceSample sample() const noexcept;

    BatchSummary batch() const noexcept;
    uint64_t member(unsigned index) const noexcept; // 0 = unknown member
    bool quarantined() const noexcept;
    bool lost() const noexcept;
    Route fence_route() const noexcept; // retained private owner (released in part 2)
private:
    friend struct OwnerFixtureAccess;
    void quarantine_locked() noexcept;
    void lose_locked() noexcept;
    mutable std::mutex mutex_;
    Registry* registry_=nullptr;
    FenceBackend* backend_=nullptr;
    Registry::TimelineFacts facts_{};
    Route queue_=0, fence_=0;
    Key recorded_{};
    BatchSummary record_{};
    uint64_t members_[batch_cap]{};
    uint64_t issued_=0, observed_=0, submissions_=0;
    unsigned polls_=0;
    bool enabled_=false, created_=false, armed_=false, stopped_=false;
    bool in_flight_=false, signal_issued_=false, lost_=false, timed_out_=false;
};

// ---------------------------------------------------------------------------
// (3) In-flight restriction window, one per list lifetime.
//
// The 3a wrappers publish restriction events AFTER native dispatch, so a Draw
// reentered from inside native SetPredication would see stale eligibility.
// Part 2 calls begin() BEFORE every native dispatch of a list method it wraps
// (restrictions, Reset, Close, ClearState, bundles, indirect) and end() after it
// returns. Intent is latched at begin(): any restriction excludes the current
// generation; render passes and protected sessions exclude the lifetime.
enum class Restriction : uint8_t {
    native_other, predication, begin_query, end_query, resolve_query,
    begin_pass, end_pass, protected_session, bundle, indirect
};
struct Eligibility {uint64_t generation=0, epoch=0; bool ok=false;};
class RestrictionGate final {
public:
    RestrictionGate() noexcept=default;
    RestrictionGate(const RestrictionGate&)=delete;
    RestrictionGate& operator=(const RestrictionGate&)=delete;
    void begin(Restriction) noexcept;
    void end(Restriction) noexcept;
    // After end(native_other) of a native Reset, with its actual result.
    void reset(bool native_success) noexcept;
    void uncertain() noexcept;
    Eligibility check() const noexcept;
    // Recheck immediately before the possible recording: same generation and
    // epoch (no native list call began since check), nothing in flight.
    bool still_valid(const Eligibility&) const noexcept;
private:
    bool ok_locked() const noexcept;
    mutable std::mutex mutex_;
    uint64_t generation_=0, epoch_=0;
    uint32_t in_flight_=0;
    bool intent_=false, lifetime_=false, ambiguous_=false;
};

// ---------------------------------------------------------------------------
// (4) Part-2 interface. Fakes in part 1; part 2 implements these with the
// game's open DIRECT list (CopyBufferRegion via saved dispatch, bypassing proxy
// observation) and the private READBACK buffer (Map subresource 0 with the
// exact read range, Unmap with an empty written range).
struct CopyRecipe {uint64_t source=0, source_offset=0, dest=0, dest_offset=0, bytes=0;};
constexpr uint64_t recipe_byte_cap=4096;
// Nonzero, <=recipe_byte_cap, subtraction-based bounds for both resources.
bool recipe_valid(const CopyRecipe&, uint64_t source_size, uint64_t dest_size) noexcept;

struct CopyRecorder {
    virtual ~CopyRecorder()=default;
    // Void native command: any call may have recorded. Return is diagnostic.
    virtual bool record(Route list, const CopyRecipe&) noexcept=0;
};
struct ReadbackMap {
    virtual ~ReadbackMap()=default;
    virtual bool map(uint64_t dest, uint64_t offset, uint64_t bytes, const uint8_t*& data) noexcept=0;
    virtual void unmap(uint64_t dest) noexcept=0; // empty written range
};
// Marks the ledger possible_recording BEFORE the void call (fake in part 1).
struct PossibleRecording {
    void* context=nullptr;
    bool (*mark)(void*) noexcept=nullptr;
};
enum class Insert : uint8_t {refused, possibly_recorded};
// Order: recipe -> gate.still_valid -> witness_allowed -> mark -> record.
// No call is made before every check passed; after mark the result is
// possibly_recorded whatever record returns (no rollback).
Insert insert_before_draw(const RestrictionGate&, const Eligibility&, bool witness_allowed,
                          PossibleRecording, CopyRecorder&, Route list, const CopyRecipe&,
                          uint64_t source_size, uint64_t dest_size) noexcept;
// Reads only after authenticated completion of `ticket` AND recording closure
// (observed successful Reset of the old recording). Copies into caller-owned
// bounded storage, unmaps once. Never maps on loss/timeout/quarantine.
bool read_back(const Timeline&, uint64_t ticket, bool recording_closed, ReadbackMap&,
               uint64_t dest, uint64_t offset, uint64_t bytes, uint8_t* out, size_t capacity) noexcept;
} // namespace acz::gpu_copy::owner
