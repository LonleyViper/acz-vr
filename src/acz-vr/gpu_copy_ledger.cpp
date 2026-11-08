#include "gpu_copy_ledger.h"
#include <limits>

namespace acz::gpu_copy {
namespace {
bool same_timeline(const Key& a,const Key& b) noexcept {
    return a.device==b.device && a.queue==b.queue && a.fence==b.fence;
}
bool same_recording(const Key& a,const Key& b) noexcept {
    return same_timeline(a,b) && a.list==b.list && a.generation==b.generation;
}
bool eligible(const Admission& a) noexcept {
    return a.key.valid() && a.history_complete && !a.retired_generation &&
        a.private_fence && a.authenticated_queue && a.exclusive_signaller &&
        a.allocation_bytes && a.allocation_bytes<=Session::byte_cap;
}
}
Session& diagnostic_session() noexcept { static Session owner; return owner; }
bool Session::arm(bool enabled,Backend& backend,const Admission& a) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled || armed_ || stopped_ || draining_ || !eligible(a))return false;
    backend_=&backend;expected_=a;armed_=true;record_.admission=true;return true;
}
bool Session::reserve(const Admission& a) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!record_.admission || stopped_ || draining_ || record_.records>=record_cap ||
       !eligible(a) || !same_recording(a.key,expected_.key) ||
       a.allocation_bytes!=expected_.allocation_bytes)return false;
    record_.records=1;record_.charged_bytes=a.allocation_bytes;
    record_.state=State::reserved;record_.admission=false;return true;
}
bool Session::created(bool success,uint64_t output,uint64_t source) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!record_.records || creation_received_)return false;
    creation_received_=true;
    private_=output;source_=source;
    record_.private_held=output!=0;record_.source_lease=source!=0;
    if(!success || !output || !source || stopped_ || record_.state==State::quarantined)
        quarantine_locked("creation-incomplete");
    return true;
}
bool Session::possible_recording() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopped_ || draining_ || record_.state!=State::reserved || !private_ || !source_)return false;
    possible_=true;record_.state=State::recorded_ambiguous;return true;
}
void Session::quarantine_locked(const char* why) noexcept {
    stopped_=true;record_.admission=false;
    if(record_.records) {
        if(record_.state!=State::quarantined && !record_.fault)record_.fault=why; // first rule wins
        record_.state=State::quarantined;
    }
}
void Session::uncertain(const char* why) noexcept {std::lock_guard<std::mutex> lock(mutex_);quarantine_locked(why);}
void Session::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_=true;record_.admission=false;
    // Stop is not closure, completion, a timeout release, or global hook closure.
}
uint64_t Session::begin_execute(const Key& key,unsigned occurrences) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Register the Execute-to-Signal join BEFORE forwarding native Execute.
    // Every relevant execution must be observed even after Stop or completion.
    if(!record_.records || !possible_)return 0;
    if(record_.state==State::quarantined)return 0;
    if(!same_recording(key,expected_.key) || occurrences!=1 || join_active_ ||
       submitted_ || record_.recording_closed || record_.owner_busy) {
        quarantine_locked("begin-execute-refused");return 0;
    }
    // A single attempt on a fresh private timeline uses value 1. No caller
    // chooses a fence value, and no session/timeline may be recycled.
    join_active_=true;submitted_=true;record_.fence_value=1;
    record_.state=State::submitted;return record_.fence_value;
}
bool Session::end_signal(uint64_t join,bool success) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!join_active_ || join!=record_.fence_value || !success ||
       record_.state==State::quarantined) {quarantine_locked("end-signal-refused");return false;}
    join_active_=false;signalled_=true;record_.state=State::signalled;return true;
}
bool Session::close_recording(const Key& key) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!possible_ || record_.state==State::quarantined || record_.owner_busy ||
       join_active_ || !same_recording(key,expected_.key)) {quarantine_locked("close-recording-refused");return false;}
    record_.recording_closed=true;record_.state=State::closed;return true;
}
bool Session::poll() noexcept {
    Backend* backend=nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!backend_ || !signalled_ || record_.retired || record_.gpu_complete ||
           record_.owner_busy || join_active_ || record_.state==State::quarantined)return false;
        if(polls_>=50){quarantine_locked("poll-cap");return false;}
        ++polls_;
        record_.owner_busy=true;backend=backend_;
    }
    const auto sample=backend->poll(); // owner lease spans unlocked backend call
    std::lock_guard<std::mutex> lock(mutex_);
    record_.owner_busy=false;
    if(record_.state==State::quarantined)return false;
    // Device removal sentinel MUST precede >=; it never authorizes release.
    if(!sample.valid || sample.value==(std::numeric_limits<uint64_t>::max)() ||
       !same_timeline(sample.timeline,expected_.key) || sample.value<last_observed_) {
        quarantine_locked(!sample.valid?"poll-invalid-sample":
            sample.value==(std::numeric_limits<uint64_t>::max)()?"poll-uint64max":
            sample.value<last_observed_?"poll-regression":"poll-other-timeline");return false;
    }
    last_observed_=sample.value;
    if(sample.value<record_.fence_value)return false;
    record_.gpu_complete=true;record_.state=State::completed;return true;
}
bool Session::read() noexcept {
    Backend* backend=nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!backend_ || !record_.gpu_complete || !record_.recording_closed ||
           record_.owner_busy || join_active_ || record_.retired || record_.consumed ||
           record_.state==State::quarantined)return false;
        record_.owner_busy=true;backend=backend_;
    }
    const bool ok=backend->read(); // fake seam only; no native Map in this step
    std::lock_guard<std::mutex> lock(mutex_);
    record_.owner_busy=false;
    if(record_.state==State::quarantined)return false;
    if(!ok){quarantine_locked("read-failed");return false;}
    record_.consumed=true;return true;
}
bool Session::retire() noexcept {
    Backend* backend=nullptr;uint64_t source=0,output=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!backend_ || record_.retired || record_.owner_busy || join_active_ ||
           record_.state==State::quarantined || !record_.recording_closed ||
           !record_.gpu_complete || !record_.consumed)return false;
        record_.owner_busy=true;backend=backend_;
        source=source_;source_=0;record_.source_lease=false;
    }
    // No permanent upload reference is passed to this API. Source is a logical
    // lease token only. Private owners require their own closure/read completion.
    backend->drop_logical_source_lease(source);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A cleanup callback may report newly uncertain history. Keep every
        // remaining owner; the completed logical lease is already discharged.
        if(record_.state==State::quarantined){record_.owner_busy=false;return false;}
        output=private_;private_=0;record_.private_held=false;
    }
    backend->release_private(output);
    std::lock_guard<std::mutex> lock(mutex_);
    record_.owner_busy=false;record_.retired=true;return true;
}
bool Session::begin_global_drain() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_=true;record_.admission=false;
    if(record_.owner_busy || join_active_ || (record_.records &&
       (!record_.retired || record_.state==State::quarantined)))return false;
    draining_=true;return true;
}
Snapshot Session::snapshot() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return record_;}
} // namespace acz::gpu_copy
