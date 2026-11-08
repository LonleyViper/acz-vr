#pragma once
#include "gpu_copy_witness.h"
#include <memory>
#include <new>

namespace acz::gpu_copy {
constexpr uint64_t ownership_coverage=coverage_bit(Coverage::direct_type)|coverage_bit(Coverage::creation)|
    coverage_bit(Coverage::allocator)|coverage_bit(Coverage::list)|coverage_bit(Coverage::queue)|coverage_bit(Coverage::device_node);
// Addresses are routing arguments only. The provider must return independently
// authenticated, non-reused lifetime tokens, never cast these addresses to IDs.
struct OwnershipProvider {
    void* context=nullptr;
    bool (*reset)(void*,const void*,const void*,Identity&) noexcept=nullptr;
    bool (*execute)(void*,const void*,const Key&,Key&) noexcept=nullptr;
};
struct ObservationFixtureAccess;
class ObservationFeed final {
public:
    // All-null is the production provider: EVERY ownership fact unavailable.
    explicit ObservationFeed(OwnershipProvider provider={}) noexcept:provider_(provider) {}
    void begin_native() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(active_native_) {
            native_ambiguous_=true;if(witness_)witness_->uncertain("native-call-overlap");
        }
        if(active_native_<65535)++active_native_;
    }
    void end_native() noexcept {std::lock_guard<std::mutex> lock(mutex_);if(active_native_)--active_native_;}
    void installed(uint64_t bits) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);apply_installed(bits);
    }
    void reset_result(const void* list,const void* allocator,bool success) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        Identity identity{};
        if(!provider_.reset || !provider_.reset(provider_.context,list,allocator,identity) ||
           !identity.device || !identity.queue || !identity.fence || !identity.list ||
           !identity.allocator || !identity.creation || !identity.node || !identity.direct) {
            if(ownership_unavailable_<65535)++ownership_unavailable_;
            if(witness_)apply_installed(0);
            return;
        }
        if(!witness_) {
            witness_.reset(new(std::nothrow) Witness);
            if(!witness_){if(storage_unavailable_<65535)++storage_unavailable_;return;}
            witness_->enabled_=true;
        }
        apply_installed(installed_|ownership_coverage);
        witness_->reset(success,identity);
        if(native_ambiguous_)witness_->uncertain("native-ambiguous-at-reset");
    }
    void close_result(bool success) noexcept {std::lock_guard<std::mutex> lock(mutex_);if(witness_)witness_->close(success);}
    void event(Event e) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // Lifetime exclusions observed before the first owned Reset must not
        // disappear merely because ownership was unavailable at that moment.
        if(e==Event::protected_change || e==Event::suspended_pass ||
           e==Event::resumed_pass || e==Event::cross_list_pass)native_ambiguous_=true;
        if(e==Event::protected_change)revoked_|=coverage_bit(Coverage::protected_session);
        if(witness_)witness_->event(e);
    }
    // `why`: static literal recorded as the witness's first fault (J-3b5).
    void uncertain(const char* why="feed-uncertain") noexcept {
        std::lock_guard<std::mutex> lock(mutex_);native_ambiguous_=true;if(witness_)witness_->uncertain(why);
    }
    void before_execute(const void* queue,unsigned count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!witness_ || !recorded_.valid())return;
        if(active_native_) {native_ambiguous_=true;witness_->uncertain("execute-during-native-call");}
        Key submitted{};
        const bool authenticated=provider_.execute &&
            provider_.execute(provider_.context,queue,recorded_,submitted);
        ticket_=witness_->before_execute(submitted,count,authenticated);
    }
    // No production caller in 3a. A future owned-fence owner reports the actual
    // Signal HRESULT here AFTER native Execute and its own private Signal.
    bool signal_result(uint64_t ticket,bool success) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return witness_ && witness_->after_signal(ticket,success);
    }
    WitnessSnapshot snapshot() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return witness_?witness_->snapshot():WitnessSnapshot{};}
    // Part 2 (step-1 experiment, Draw seam). The witness's authenticated
    // identity/generation only while insertion is allowed AND no wrapped native
    // list call is in flight on this feed (the in-flight window Astra required:
    // every restriction-bearing wrapper brackets native dispatch with
    // begin_native/end_native, so a Draw reentered from inside a native
    // SetPredication/BeginQuery/... is refused here). `why` classifies refusal:
    // 1 no witness (ownership unavailable), 2 native call in flight, 4 native
    // ambiguity, 8 witness ambiguous, 16 recording not open, 32 coverage
    // incomplete, 64 predicated, 128 query/bundle/indirect exclusion, 256 render
    // pass/suspension, 512 transaction already attached, 1024 no observed Reset.
    bool insertion_candidate(Identity& out,uint64_t& generation,unsigned& why) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);why=0;
        if(!witness_){why=1;return false;}
        if(active_native_)why|=2;
        if(native_ambiguous_)why|=4;
        if(!why && witness_->candidate(out,generation))return true;
        const auto w=witness_->snapshot();
        if(w.ambiguous)why|=8;
        if(!w.open)why|=16;
        if(w.coverage!=required_coverage)why|=32;
        if(w.predicated)why|=64;
        if(w.queries || w.excluded)why|=128;
        if(w.pass || w.suspended)why|=256;
        if(w.tracked)why|=512;
        if(!w.generation)why|=1024;
        if(!why)why=8; // identity mismatch: treat as ambiguity
        return false;
    }
    // J-effects-smoke-fix (Astra issue-31 finding 2): true only when the game's own predication is PROVABLY off at this draw: a witness exists, the
    // recording is open and unambiguous, SetPredication coverage is installed, no native call is in flight and no SetPredication(resource) is active.
    // The effects replay ends with SetPredication(null), which would clear a game predicate before the original draw; unknown counts as active.
    bool game_predication_clear() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!witness_||active_native_||native_ambiguous_)return false;
        const auto w=witness_->snapshot();
        return w.open&&!w.ambiguous&&!w.predicated&&(w.coverage&coverage_bit(Coverage::predication))!=0;
    }
    // Part 2 production attach (replaces the fixture-only path): the ledger is
    // already in recorded_ambiguous for exactly this key; the witness re-checks
    // its own admission and identity under its mutex before attaching.
    bool attach_transaction(Session& session,const Key& key) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!witness_ || active_native_ || native_ambiguous_ || !witness_->attach(session,key))return false;
        recorded_=key;return true;
    }
    unsigned ownership_unavailable() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return ownership_unavailable_;}
    bool constructed() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return bool(witness_);}
    uint64_t ticket() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return ticket_;}
    // Batch scratch, serialized by the hook-state mutex. No native call spans it.
    unsigned occurrences=0,last_ordinal=0;
private:
    friend struct ObservationFixtureAccess;
    void apply_installed(uint64_t bits) noexcept {
        // Re-attesting hooks cannot erase the independently authenticated tuple.
        installed_=(bits ? bits|(installed_&ownership_coverage) : 0) & required_coverage & ~revoked_;
        // A lost installation invalidates current history immediately. Additions
        // only latch at the next successful saved-native Reset.
        if(witness_)for(unsigned n=0;n<static_cast<unsigned>(Coverage::count);++n)
            witness_->coverage(static_cast<Coverage>(n),bool(installed_ & (uint64_t{1}<<n)));
    }
    mutable std::mutex mutex_;
    OwnershipProvider provider_{};
    std::unique_ptr<Witness> witness_;
    Key recorded_{};
    uint64_t installed_=0,ticket_=0,revoked_=0;
    unsigned ownership_unavailable_=0,storage_unavailable_=0;
    unsigned active_native_=0;
    bool native_ambiguous_=false;
};
} // namespace acz::gpu_copy
