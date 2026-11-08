#include "gpu_copy_witness.h"
#include <limits>
namespace acz::gpu_copy {
namespace {
bool same(const Identity& a,const Identity& b) noexcept {
    return a.device==b.device && a.queue==b.queue && a.fence==b.fence &&
        a.list==b.list && a.allocator==b.allocator && a.creation==b.creation &&
        a.node==b.node && a.direct==b.direct;
}
// Same list lifetime across Resets; the allocator is per-Reset (part 2).
bool same_lifetime(const Identity& a,const Identity& b) noexcept {
    return a.device==b.device && a.queue==b.queue && a.fence==b.fence &&
        a.list==b.list && a.creation==b.creation && a.node==b.node && a.direct==b.direct;
}
bool valid(const Identity& i) noexcept {
    // node is a non-reused device-node identity token, NOT a zero-based index.
    return i.device && i.queue && i.fence && i.list && i.allocator && i.creation && i.node && i.direct;
}
}
void Witness::fail(const char* why) noexcept {
    const bool was_open=open_;
    ambiguous_=true;open_=false;
    if(faults_<(std::numeric_limits<unsigned>::max)())++faults_;
    if(detached_) {
        // J-3b5: the attached recording was already joined and then discarded
        // by an observed successful Reset; this list-level fault concerns a
        // LATER generation that cannot contain the copy. Count it only.
        if(post_closure_faults_<(std::numeric_limits<unsigned>::max)())++post_closure_faults_;
        return;
    }
    if(!fault_){fault_=why;fault_generation_=generation_;fault_closed_=closed_generation_;fault_open_=was_open;}
    if(ledger_)ledger_->uncertain(why);
}
void Witness::coverage(Coverage c,bool supported) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return;
    if(static_cast<unsigned>(c)>=static_cast<unsigned>(Coverage::count)){fail("coverage-invalid");return;}
    const auto bit=coverage_bit(c);
    if(supported)installed_|=bit;
    else {installed_&=~bit;epoch_coverage_&=~bit;if(ledger_)fail("coverage-lost");}
    // Newly installed hooks cannot reconstruct history since the last Reset.
}
void Witness::reset(bool success,const Identity& i) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return;
    if(!success){fail("reset-failed");return;}
    if(generation_==(std::numeric_limits<uint64_t>::max)()){fail("generation-overflow");return;}
    closed_generation_=0;
    ++generation_; // ONLY successful native Reset advances the generation.
    if(!valid(i) || (identity_.list && !same_lifetime(identity_,i))){fail("reset-identity-changed");return;}
    if(ledger_ && !detached_ && !ledger_->snapshot().recording_closed) {
        // This closes the old tombstone; never clears its charge or owners.
        if(!ledger_->close_recording(recorded_)){fail("close-recording-refused");return;}
        // J-3b5: Reset AFTER the transaction's own Execute+Signal joined (the
        // game resets and re-records this list every frame, long before the
        // private fence completes). D3D12 Reset discards the old recording, so
        // no later generation can execute the copy again: the old transaction
        // stays pending on its private fence, independent of this list.
        if(joined_ && !ambiguous_)detached_=true;
    }
    identity_=i;epoch_coverage_=installed_;queries_=0;predicated_=false;pass_=false;
    // A suspended/cross-list context cannot be erased by Reset on this list.
    open_=!ambiguous_ && !suspended_;excluded_=false;
}
void Witness::close(bool success) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return;
    if(!success || !open_ || pass_ || queries_) {
        fail(!success?"close-failed":!open_?"close-not-open":pass_?"close-in-render-pass":"close-with-open-query");return;
    }
    closed_generation_=generation_;
    open_=false; // Close is NOT recording invalidation and does not close ledger.
}
void Witness::uncertain(const char* why) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(enabled_)fail(why);
}
void Witness::event(Event e) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return;
    if(!open_){fail("event-on-closed-list");return;}
    switch(e) {
    case Event::predication_on:predicated_=true;break;
    case Event::predication_off:predicated_=false;break;
    case Event::begin_query:
        if(queries_==(std::numeric_limits<uint64_t>::max)())fail("query-overflow");else ++queries_;break;
    case Event::end_query:
        // Query identities/types are deliberately not tracked: observing any
        // query conservatively excludes this entire recording (below).
        if(!queries_)fail("query-underflow");else --queries_;break;
    case Event::resolve_query:break;
    case Event::begin_pass:if(pass_)fail("render-pass-nesting");else pass_=true;break;
    case Event::end_pass:if(!pass_)fail("render-pass-unbalanced");else pass_=false;break;
    case Event::suspended_pass:case Event::resumed_pass:case Event::cross_list_pass:
        suspended_=true;fail("render-pass-context");break;
    case Event::protected_change:
        installed_&=~coverage_bit(Coverage::protected_session);
        epoch_coverage_&=~coverage_bit(Coverage::protected_session);fail("protected-session-change");break;
    case Event::bundle:case Event::indirect:excluded_=true;break;
    case Event::clear_state:break; // NOT Reset: retains all restriction history.
    default:fail("unknown-event");break;
    }
    if(e==Event::begin_query || e==Event::end_query || e==Event::resolve_query)excluded_=true;
    // J-3b5: an exclusion in a LATER generation (after detachment) cannot
    // concern the closed recording; it still refuses that generation itself.
    if(ledger_ && !detached_ && (ambiguous_ || excluded_))ledger_->uncertain(ambiguous_?"witness-ambiguous":"witness-excluded");
}
void Witness::alias(unsigned version) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return;
    if(version>10 || !(epoch_coverage_ & (coverage_bit(Coverage::alias0)<<version)))fail("alias-uncovered");
}
bool Witness::allowed(const Identity& i) const noexcept {
    return enabled_ && !ambiguous_ && open_ && generation_ && !ledger_ &&
        epoch_coverage_==required_coverage && valid(i) && same(i,identity_) &&
        !predicated_ && !queries_ && !pass_ && !suspended_ && !excluded_;
}
bool Witness::insertion_allowed(const Identity& i) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);return allowed(i);
}
bool Witness::candidate(Identity& out,uint64_t& generation) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!allowed(identity_))return false;
    out=identity_;generation=generation_;return true;
}
bool Witness::attach(Session& s,const Key& k) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!allowed(identity_) || k.device!=identity_.device || k.queue!=identity_.queue ||
       k.fence!=identity_.fence || k.list!=identity_.list || k.generation!=generation_)return false;
    std::lock_guard<std::mutex> ledger_lock(s.mutex_);
    const auto& expected=s.expected_.key;
    if(s.record_.state!=State::recorded_ambiguous || s.stopped_ || !s.possible_ ||
       expected.device!=k.device || expected.queue!=k.queue || expected.fence!=k.fence ||
       expected.list!=k.list || expected.generation!=k.generation)return false;
    ledger_=&s;recorded_=k;return true;
}
uint64_t Witness::before_execute(const Key& k,unsigned count,bool authenticated) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || !ledger_)return 0;
    if(detached_) {
        // J-3b5: the list was Reset after the transaction joined, so this
        // Execute submits a later generation that does not contain the copy.
        // The live-run-4 quarantine (generation != recorded) no longer fires.
        // A malformed later submission (an open or never-Closed generation:
        // invalid API use whose runtime effect is unknown) still quarantines.
        if(open_ || !closed_generation_ || closed_generation_!=generation_) {
            const char* why="later-execute-unclosed";
            if(!fault_){fault_=why;fault_generation_=generation_;fault_closed_=closed_generation_;fault_open_=open_;}
            if(faults_<(std::numeric_limits<unsigned>::max)())++faults_;
            ambiguous_=true;open_=false;ledger_->uncertain(why);return 0;
        }
        if(later_executes_<(std::numeric_limits<unsigned>::max)())++later_executes_;
        return 0;
    }
    if(!authenticated){fail("execute-unauthenticated");return 0;}
    if(ambiguous_){fail("execute-while-ambiguous");return 0;}
    // A Reset since the recording (not after its own join): later generation.
    if(generation_!=recorded_.generation){fail("execute-later-generation");return 0;}
    if(open_){fail("execute-open-generation");return 0;}
    if(!closed_generation_ || closed_generation_!=recorded_.generation){fail("execute-unclosed-generation");return 0;}
    pending_=ledger_->begin_execute(k,count);
    if(!pending_)fail("begin-execute-refused");
    return pending_;
}
bool Witness::after_signal(uint64_t ticket,bool success) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || !ledger_)return false;
    if(!pending_ || ticket!=pending_){fail("signal-ticket-mismatch");return false;}
    const bool ok=ledger_->end_signal(ticket,success);pending_=0;
    if(!ok)fail("end-signal-refused");else joined_=true;
    return ok;
}
WitnessSnapshot Witness::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    WitnessSnapshot out{generation_,epoch_coverage_,enabled_,ambiguous_,open_,ledger_!=nullptr,pending_!=0};
    out.closed_generation=closed_generation_;out.predicated=predicated_;out.queries=queries_!=0;
    out.pass=pass_;out.suspended=suspended_;out.excluded=excluded_;
    out.fault=fault_;out.fault_generation=fault_generation_;out.fault_closed_generation=fault_closed_;
    out.recorded_generation=recorded_.generation;out.faults=faults_;out.later_executes=later_executes_;
    out.post_closure_faults=post_closure_faults_;out.fault_open=fault_open_;out.detached=detached_;
    return out;
}
} // namespace acz::gpu_copy
