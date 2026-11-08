#include "native_ownership_retirement.h"
namespace acz::native_retirement {
namespace {
namespace tx=gpu_transaction;
bool valid(Dependency d) noexcept { return d.handle&&d.identity&&d.epoch; }
bool same(Dependency a,Dependency b) noexcept {
    return a.handle==b.handle&&a.identity==b.identity&&a.epoch==b.epoch;
}
bool same(const tx::Identity& a,const tx::Identity& b) noexcept {
    return a.capture.capture==b.capture.capture&&a.capture.device_epoch==b.capture.device_epoch&&
        a.capture.list_lifetime==b.capture.list_lifetime&&a.capture.reset_generation==b.capture.reset_generation&&
        a.capture.reset_revision==b.capture.reset_revision&&a.capture.draw_ordinal==b.capture.draw_ordinal&&
        a.capture.submission_serial==b.capture.submission_serial&&a.version==b.version&&a.device==b.device&&
        a.queue==b.queue&&a.queue_epoch==b.queue_epoch&&a.fence==b.fence&&
        a.fence_epoch==b.fence_epoch&&a.value==b.value;
}
bool prepared(const tx::Identity& a) noexcept {
    return a.capture.capture&&a.capture.device_epoch&&a.capture.list_lifetime&&a.capture.reset_generation&&
        a.capture.reset_revision&&a.capture.draw_ordinal&&a.capture.submission_serial&&a.version&&
        a.device&&a.queue&&a.queue_epoch&&!a.fence&&!a.fence_epoch&&!a.value;
}
}
bool QuarantineKeeper::reserve(std::size_t& start) noexcept {
    if(available()<3)return false;
    start=reserved_;reserved_+=3;return true;
}
void QuarantineKeeper::retain(std::size_t slot,Dependency dep) noexcept {
    if(!entries_[slot].handle){entries_[slot]=dep;++count_;}
}
bool QuarantineKeeper::contains(uint64_t handle) const noexcept {
    for(std::size_t i=0;i<reserved_;++i)if(handle&&entries_[i].handle==handle)return true;
    return false;
}
OwnershipAdapter::OwnershipAdapter(ComBackend& com,FenceBackend& fence,QuarantineKeeper& keeper) noexcept:
    com_(com),fence_backend_(fence),keeper_(keeper) {}
OwnershipAdapter::~OwnershipAdapter() noexcept { if(held())quarantine_owned(FailReason::InvalidPhase); }
std::size_t OwnershipAdapter::held() const noexcept { return unsigned(owned_[0])+unsigned(owned_[1])+unsigned(owned_[2]); }
bool OwnershipAdapter::busy() noexcept {
    if(phase_!=Phase::Acquiring&&phase_!=Phase::Polling&&phase_!=Phase::Releasing)return false;
    poisoned_=true;reason_=FailReason::Reentry;return true;
}
bool OwnershipAdapter::fail(FailReason reason) noexcept { reason_=reason;return false; }
void OwnershipAdapter::quarantine_owned(FailReason reason) noexcept {
    phase_=Phase::Quarantined;reason_=reason;
    for(std::size_t i=0;i<3;++i)if(owned_[i]) {
        const auto dep=deps_[i];owned_[i]=false;deps_[i]={};keeper_.retain(reservation_+i,dep);
    }
}
bool OwnershipAdapter::acquire(const Dependency& allocator,const Dependency& list,const Dependency& fence,
                              const tx::Identity& origin) noexcept {
    if(busy())return false;
    if(phase_!=Phase::Idle)return fail(FailReason::InvalidPhase);
    const Dependency inputs[]={allocator,list,fence};const auto identity=origin;
    phase_=Phase::Acquiring;
    if(!valid(inputs[0])||!valid(inputs[1])||!valid(inputs[2])||!prepared(identity)||
       inputs[0].handle==inputs[1].handle||inputs[0].handle==inputs[2].handle||inputs[1].handle==inputs[2].handle||
       inputs[0].identity==inputs[1].identity||inputs[0].identity==inputs[2].identity||inputs[1].identity==inputs[2].identity) {
        phase_=Phase::Failed;return fail(FailReason::InvalidInput);
    }
    if(!keeper_.reserve(reservation_)){phase_=Phase::Failed;return fail(FailReason::QuarantineCapacity);}
    expected_=identity;for(std::size_t i=0;i<3;++i)deps_[i]=inputs[i];
    for(std::size_t i=0;i<3;++i) {
        const bool obtained=com_.add_ref(deps_[i].handle);owned_[i]=obtained;
        if(poisoned_){quarantine_owned(FailReason::Reentry);return false;}
        if(!obtained){reason_=FailReason::BackendFailure;release_owned(Phase::Failed);return false;}
    }
    phase_=Phase::Acquired;reason_=FailReason::None;return true;
}
bool OwnershipAdapter::handoff(const tx::Signal& input) noexcept {
    if(busy())return false;
    if(phase_!=Phase::Acquired)return fail(FailReason::InvalidPhase);
    const auto signal=input;
    if(!signal.fence||!signal.epoch||!signal.value||signal.value==UINT64_MAX||
       signal.fence!=deps_[2].identity||signal.epoch!=deps_[2].epoch) {
        quarantine_owned(FailReason::UnknownSignal);return false;
    }
    expected_.fence=signal.fence;expected_.fence_epoch=signal.epoch;expected_.value=signal.value;
    phase_=Phase::HandedOff;return true;
}
tx::Completion OwnershipAdapter::poll_completion() noexcept {
    if(busy())return {};
    if(phase_!=Phase::HandedOff)return {};
    const auto allocator=deps_[0],list=deps_[1],fence=deps_[2];const auto expected=expected_;
    phase_=Phase::Polling;
    const auto observation=fence_backend_.read(fence.handle,fence.epoch);
    if(poisoned_){quarantine_owned(FailReason::Reentry);return {};}
    phase_=Phase::HandedOff;
    tx::Completion result;
    if(!observation.valid||observation.completed==UINT64_MAX) {
        result.status=tx::CompletionStatus::DeviceLost;
        quarantine_owned(FailReason::InvalidCompletion);return result;
    }
    if(!same(observation.fence,fence)||!same(observation.allocator,allocator)||
       !same(observation.execution_list,list)||!same(observation.transaction,expected)) {
        quarantine_owned(FailReason::IdentityMismatch);return result;
    }
    if(observation.completed<last_observed_) {
        quarantine_owned(FailReason::Regressed);return result;
    }
    last_observed_=observation.completed;
    result.observed=observation.transaction;result.observed.value=observation.completed;
    result.independently_confirmed=true;
    result.status=observation.completed<expected.value?tx::CompletionStatus::Pending:tx::CompletionStatus::Confirmed;
    return result;
}
bool OwnershipAdapter::release_owned(Phase terminal,tx::Transaction* transaction) noexcept {
    Dependency detached[3]{};bool pending[3]{};
    for(std::size_t i=0;i<3;++i){detached[i]=deps_[i];pending[i]=owned_[i];owned_[i]=false;deps_[i]={};}
    phase_=Phase::Releasing;
    for(std::size_t n=3;n>0;--n) {
        const auto i=n-1;if(!pending[i])continue;
        if(transaction&&(transaction->phase()!=tx::Phase::Completed||!same(transaction->expected_identity(),expected_)))
            poisoned_=true;
        if(poisoned_)break;
        const bool released=com_.release(detached[i].handle);pending[i]=!released;
        if(!released){poisoned_=true;reason_=FailReason::BackendFailure;break;}
    }
    if(transaction&&transaction->phase()!=tx::Phase::Completed)poisoned_=true;
    // Detach before callbacks. Conflicting reentry stops the outer loop and
    // retains all remaining references exactly once, using reserved capacity.
    if(poisoned_) {
        for(std::size_t i=0;i<3;++i)if(pending[i])keeper_.retain(reservation_+i,detached[i]);
        phase_=Phase::Quarantined;
        if(reason_!=FailReason::BackendFailure)reason_=FailReason::Reentry;
        return false;
    }
    phase_=terminal;return true;
}
bool OwnershipAdapter::retire(tx::Transaction& transaction) noexcept {
    if(busy())return false;
    if(phase_!=Phase::HandedOff)return fail(FailReason::InvalidPhase);
    if(!same(transaction.expected_identity(),expected_)) {
        quarantine_owned(FailReason::IdentityMismatch);return false;
    }
    if(transaction.phase()!=tx::Phase::Completed)return fail(FailReason::Incomplete);
    for(const auto& dep:deps_) {
        bool leased=false;
        for(std::size_t i=0;i<tx::max_leases;++i) {
            const auto lease=transaction.ledger(i);
            leased|=lease.identity==dep.identity&&lease.prepared&&lease.recorded&&lease.closed&&
                !lease.disposed&&!lease.retired;
        }
        if(!leased){quarantine_owned(FailReason::IdentityMismatch);return false;}
    }
    const auto completion=poll_completion();
    if(phase_!=Phase::HandedOff)return false;
    if(completion.status==tx::CompletionStatus::Pending)return fail(FailReason::Incomplete);
    if(!completion.independently_confirmed||completion.status!=tx::CompletionStatus::Confirmed||
       completion.observed.value!=expected_.value||transaction.phase()!=tx::Phase::Completed||
       !same(transaction.expected_identity(),expected_)) {
        quarantine_owned(FailReason::InvalidCompletion);return false;
    }
    if(!release_owned(Phase::Retired,&transaction))return false;
    if(!transaction.retire()){phase_=Phase::Quarantined;return fail(FailReason::InvalidPhase);}
    reason_=FailReason::None;return true;
}
bool OwnershipAdapter::quarantine_all() noexcept {
    if(busy())return false;
    if(phase_!=Phase::Acquired&&phase_!=Phase::HandedOff)return fail(FailReason::InvalidPhase);
    quarantine_owned(FailReason::None);return true;
}
} // namespace acz::native_retirement
