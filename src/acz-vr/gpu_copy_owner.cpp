#include "gpu_copy_owner.h"
#include <cstring>
#include <limits>

namespace acz::gpu_copy::owner {
namespace {
constexpr uint64_t u64max=(std::numeric_limits<uint64_t>::max)();
uint32_t normal_mask(uint32_t mask) noexcept {return mask?mask:1u;} // D3D12: 0 means node 0
bool reset_thunk(void* context,const void* list,const void* allocator,Identity& out) noexcept {
    return context && static_cast<const Registry*>(context)->lookup_reset(
        reinterpret_cast<Route>(list),reinterpret_cast<Route>(allocator),out);
}
bool execute_thunk(void* context,const void* queue,const Key& recorded,Key& out) noexcept {
    return context && static_cast<const Registry*>(context)->lookup_execute(
        reinterpret_cast<Route>(queue),recorded,out);
}
} // namespace

// ----------------------------------------------------------------- Registry
void Registry::enable() noexcept {std::lock_guard<std::mutex> lock(mutex_);enabled_=true;}
unsigned Registry::reset_refusal(Route list,Route allocator) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    unsigned r=0;
    if(!enabled_)r|=1;
    if(saturated_)r|=2;
    if(lost_)r|=4;
    if(!bound_queue_)r|=8;
    const auto* l=find_locked(list);
    const auto* a=find_locked(allocator);
    if(!l || l->kind!=Kind::list)r|=16;else if(l->ambiguous || !l->live)r|=32;
    if(!a || a->kind!=Kind::allocator)r|=64;else if(a->ambiguous || !a->live)r|=128;
    if(l && l->live && !l->ambiguous && (!l->direct || (bound_queue_ && l->device!=bound_device_)))r|=256;
    if(a && a->live && !a->ambiguous && (!a->direct || (bound_queue_ && a->device!=bound_device_)))r|=512;
    if(l && l->live && !l->ambiguous && bound_queue_) {
        const auto* q=token_locked(bound_queue_,Kind::queue);
        if(!q || q->node_mask!=l->node_mask)r|=1024;
    }
    return r;
}
unsigned Registry::live_records() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);unsigned n=0;
    for(const auto& rec:records_)if(rec.live && !rec.ambiguous)++n;
    return n;
}
Registry::Record* Registry::find_locked(Route r) noexcept {
    if(!r)return nullptr;
    for(auto& rec:records_)if(rec.route==r && (rec.live || rec.ambiguous))return &rec;
    return nullptr;
}
const Registry::Record* Registry::find_locked(Route r) const noexcept {
    if(!r)return nullptr;
    for(const auto& rec:records_)if(rec.route==r && (rec.live || rec.ambiguous))return &rec;
    return nullptr;
}
const Registry::Record* Registry::live_locked(Route r,Kind k) const noexcept {
    const auto* rec=find_locked(r);
    return rec && rec->live && !rec->ambiguous && rec->kind==k ? rec : nullptr;
}
const Registry::Record* Registry::token_locked(uint64_t t,Kind k) const noexcept {
    if(!t)return nullptr;
    for(const auto& rec:records_)if(rec.token==t && rec.kind==k && rec.live && !rec.ambiguous)return &rec;
    return nullptr;
}
Registry::Record* Registry::claim_locked(Route r) noexcept {
    // Retired (non-live, non-ambiguous) slots are reusable; their tokens are not.
    for(auto& rec:records_)if(!rec.live && !rec.ambiguous){rec=Record{};rec.route=r;return &rec;}
    if(exhausted_<65535)++exhausted_;
    saturated_=true;
    return nullptr;
}
void Registry::tombstone_locked(Route r) noexcept {
    if(auto* rec=find_locked(r)){rec->live=false;rec->ambiguous=true;}
    else if(auto* slot=claim_locked(r))slot->ambiguous=true;
    if(auto* rec=find_locked(r)) {
        if((bound_queue_ && rec->token==bound_queue_) || (bound_fence_ && rec->token==bound_fence_))lost_=true;
    }
}
uint64_t Registry::mint_locked() noexcept {
    if(next_==u64max){saturated_=true;return 0;}
    return ++next_;
}
Created Registry::create_locked(bool success,Route r,Kind kind,const Record& facts,bool parent_ok) noexcept {
    if(!enabled_ || !r)return Created::off;
    if(auto* existing=find_locked(r)) {
        if(success && kind==Kind::device && existing->kind==Kind::device && existing->live && !existing->ambiguous)
            return Created::repeated; // ASSUMPTION: same live device object returned again
        // Missed Release or ambiguous history: never trust either lifetime.
        tombstone_locked(r);return Created::collision;
    }
    if(!success){tombstone_locked(r);return Created::failed;} // D-171: failed output still tombstoned
    auto* slot=claim_locked(r);
    if(!slot)return Created::exhausted;
    *slot=facts;slot->route=r;slot->kind=kind;
    slot->token=mint_locked();slot->creation=mint_locked();
    if(!slot->token || !slot->creation || !parent_ok){slot->live=false;slot->ambiguous=true;return Created::parent;}
    slot->live=true;slot->ambiguous=false;
    return Created::recorded;
}
Created Registry::device_created(bool success,Route device) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return create_locked(success,device,Kind::device,Record{},true);
}
Created Registry::queue_created(bool success,Route queue,Route device,uint32_t mask,bool direct) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return Created::off;
    const auto* d=live_locked(device,Kind::device);
    Record facts{};facts.device=d?d->token:0;facts.node_mask=normal_mask(mask);facts.direct=direct;
    facts.node=d?mint_locked():0;
    return create_locked(success,queue,Kind::queue,facts,d && facts.node);
}
Created Registry::allocator_created(bool success,Route allocator,Route device,bool direct) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return Created::off;
    const auto* d=live_locked(device,Kind::device);
    Record facts{};facts.device=d?d->token:0;facts.direct=direct;
    return create_locked(success,allocator,Kind::allocator,facts,d!=nullptr);
}
Created Registry::list_created(bool success,Route list,Route device,Route allocator,uint32_t mask,bool direct) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return Created::off;
    const auto* d=live_locked(device,Kind::device);
    const auto* a=live_locked(allocator,Kind::allocator);
    const bool parent=d && a && a->device==d->token && a->direct==direct;
    Record facts{};facts.device=d?d->token:0;facts.allocator=a?a->token:0;
    facts.node_mask=normal_mask(mask);facts.direct=direct;
    return create_locked(success,list,Kind::list,facts,parent);
}
Created Registry::closed_list_created(bool success,Route list,Route device,uint32_t mask,bool direct) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return Created::off;
    const auto* d=live_locked(device,Kind::device);
    Record facts{};facts.device=d?d->token:0;facts.allocator=0; // closed: no creation allocator
    facts.node_mask=normal_mask(mask);facts.direct=direct;
    return create_locked(success,list,Kind::list,facts,d!=nullptr);
}
Created Registry::fence_created(bool success,Route fence,Route device) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_)return Created::off;
    const auto* d=live_locked(device,Kind::device);
    Record facts{};facts.device=d?d->token:0;
    return create_locked(success,fence,Kind::fence,facts,d!=nullptr);
}
void Registry::released(Route r) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* rec=find_locked(r);
    if(!rec || rec->ambiguous)return;
    if((bound_queue_ && rec->token==bound_queue_) || (bound_fence_ && rec->token==bound_fence_))lost_=true;
    rec->live=false; // slot reusable; token retired forever
}
void Registry::uncertain(Route r) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(find_locked(r))tombstone_locked(r);
}
bool Registry::bind_timeline(Route queue,Route fence) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || bound_ever_)return false;
    bound_ever_=true; // one attempt, never rebound
    const auto* q=live_locked(queue,Kind::queue);
    const auto* f=live_locked(fence,Kind::fence);
    if(saturated_ || !q || !f || !q->direct || q->device!=f->device || !token_locked(q->device,Kind::device)) {
        lost_=true;return false;
    }
    bound_device_=q->device;bound_queue_=q->token;bound_fence_=f->token;bound_node_=q->node;
    return true;
}
Registry::TimelineFacts Registry::timeline() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return {bound_device_,bound_queue_,bound_fence_,bound_node_,bound_queue_!=0,lost_};
}
void Registry::timeline_lost() noexcept {std::lock_guard<std::mutex> lock(mutex_);lost_=true;}
uint64_t Registry::live_token(Route r,Kind k) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || saturated_)return 0;
    const auto* rec=live_locked(r,k);return rec?rec->token:0;
}
Route Registry::device_route_of(Route queue) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || saturated_)return 0;
    const auto* q=live_locked(queue,Kind::queue);
    const auto* d=q?token_locked(q->device,Kind::device):nullptr;
    return d?d->route:0;
}
bool Registry::lookup_reset(Route list,Route allocator,Identity& out) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || saturated_ || lost_ || !bound_queue_)return false;
    const auto* l=live_locked(list,Kind::list);
    const auto* a=live_locked(allocator,Kind::allocator);
    const auto* q=token_locked(bound_queue_,Kind::queue);
    const auto* f=token_locked(bound_fence_,Kind::fence);
    if(!l || !a || !q || !f || !token_locked(bound_device_,Kind::device))return false;
    if(!l->direct || !a->direct || l->device!=bound_device_ || a->device!=bound_device_ ||
       l->node_mask!=q->node_mask)return false;
    out=Identity{bound_device_,bound_queue_,bound_fence_,l->token,a->token,l->creation,bound_node_,true};
    return true;
}
bool Registry::lookup_execute(Route queue,const Key& recorded,Key& out) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || saturated_ || lost_ || !bound_queue_ || !recorded.valid())return false;
    const auto* q=live_locked(queue,Kind::queue);
    if(!q || q->token!=bound_queue_ || recorded.device!=bound_device_ ||
       recorded.queue!=bound_queue_ || recorded.fence!=bound_fence_)return false;
    const auto* l=token_locked(recorded.list,Kind::list);
    if(!l || !l->direct)return false;
    out=recorded;return true;
}
OwnershipProvider Registry::provider() noexcept {return {this,&reset_thunk,&execute_thunk};}
unsigned Registry::exhausted() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return exhausted_;}
unsigned Registry::tombstones() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);unsigned n=0;
    for(const auto& rec:records_)if(rec.ambiguous)++n;
    return n;
}

// ----------------------------------------------------------------- Timeline
void Timeline::enable() noexcept {std::lock_guard<std::mutex> lock(mutex_);enabled_=true;}
bool Timeline::create(Registry& registry,FenceBackend& backend,Route queue) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!enabled_ || created_ || stopped_ || !queue)return false;
        created_=true;registry_=&registry;backend_=&backend;queue_=queue;
    }
    const Route device=registry.device_route_of(queue);
    Route fence=0;bool ok=false;
    if(device)ok=backend.create(device,fence); // no lock held
    if(fence)registry.fence_created(ok,fence,device);
    const bool bound=ok && fence && registry.bind_timeline(queue,fence);
    const auto facts=registry.timeline();
    std::lock_guard<std::mutex> lock(mutex_);
    fence_=fence;facts_=facts; // a failed non-null fence is still retained here
    if(!bound || facts.lost || !facts.bound)lost_=true;
    return !lost_;
}
void Timeline::stop() noexcept {std::lock_guard<std::mutex> lock(mutex_);stopped_=true;}
bool Timeline::arm(const Key& k) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || !created_ || lost_ || stopped_ || armed_ || !k.valid() ||
       k.device!=facts_.device || k.queue!=facts_.queue || k.fence!=facts_.fence)return false;
    armed_=true;recorded_=k;return true;
}
void Timeline::quarantine_locked() noexcept {record_.state=BatchState::quarantined;}
void Timeline::lose_locked() noexcept {
    lost_=true;
    if(record_.state!=BatchState::none)quarantine_locked();
    if(registry_)registry_->timeline_lost(); // registry is a leaf lock
}
uint64_t Timeline::begin(const BatchInput& in) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!enabled_ || !armed_)return 0;
    if(submissions_<u64max)++submissions_;
    if(in_flight_) {
        // Concurrent or reentrant Execute inside the join window: the Signal
        // would no longer follow exactly the registered batch.
        record_.reentrant=true;quarantine_locked();return 0;
    }
    // A successful Reset discarded the recorded generation: nothing in this
    // batch can execute the copy any more.
    if(in.candidate_generation!=recorded_.generation)return 0;
    if(in.count>batch_cap || (in.count && !in.lists)) {
        record_.oversize=true;quarantine_locked();return 0; // incomplete scan
    }
    unsigned occurrences=0,first=0;bool unknown=false;
    for(unsigned i=0;i<in.count;++i) {
        const uint64_t token=registry_->live_token(in.lists[i],Kind::list);
        if(!token)unknown=true;
        if(token==recorded_.list){if(!occurrences)first=i;++occurrences;}
    }
    if(!occurrences) {
        // An unknown member may be an unrecognised alias of the recording,
        // unless the caller certified canonical alias routing (part 2).
        if(unknown && !in.aliases_certified){record_.unknown=true;quarantine_locked();}
        return 0;
    }
    if(record_.state!=BatchState::none) {
        // Second execution of the same recording: a second destination writer.
        record_.re_executed=true;quarantine_locked();return 0;
    }
    const uint64_t queue=registry_->live_token(in.queue,Kind::queue);
    record_.count=in.count;record_.ordinal=first;record_.occurrences=occurrences;
    record_.queue=queue;record_.list=recorded_.list;record_.generation=recorded_.generation;
    record_.submission=submissions_;
    for(unsigned i=0;i<in.count;++i)members_[i]=registry_->live_token(in.lists[i],Kind::list);
    record_.unknown=unknown;record_.duplicate=occurrences!=1;
    record_.unauthenticated=!queue || lost_;
    record_.cross_queue=queue && queue!=facts_.queue;
    record_.unclosed=in.closed_generation!=recorded_.generation;
    if((record_.unknown && !in.aliases_certified) || record_.duplicate || record_.unauthenticated || record_.cross_queue ||
       record_.unclosed || issued_==u64max) {
        quarantine_locked();return 0;
    }
    record_.value=++issued_;record_.ticket=record_.value;record_.state=BatchState::registered;
    in_flight_=true;signal_issued_=false;
    return record_.ticket;
}
bool Timeline::signal(uint64_t ticket) noexcept {
    FenceBackend* backend=nullptr;Route queue=0,fence=0;uint64_t value=0;bool issue=false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!in_flight_ || !ticket || ticket!=record_.ticket || signal_issued_) {
            if(in_flight_ && ticket!=record_.ticket)quarantine_locked();
            return false;
        }
        signal_issued_=true;
        issue=record_.state==BatchState::registered && !lost_;
        backend=backend_;queue=queue_;fence=fence_;value=record_.value;
    }
    const bool ok=issue && backend->signal(queue,fence,value); // exactly once, no lock
    std::lock_guard<std::mutex> lock(mutex_);
    in_flight_=false;
    if(!issue){quarantine_locked();return false;}
    if(!ok){record_.state=BatchState::signal_failed;return false;} // ambiguous: retain forever
    if(record_.state==BatchState::registered)record_.state=BatchState::signalled;
    return record_.state==BatchState::signalled;
}
PollResult Timeline::poll() noexcept {
    FenceBackend* backend=nullptr;Registry* registry=nullptr;Route fence=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!enabled_ || !created_ || lost_ || !fence_ || timed_out_)return {observed_,false,lost_,timed_out_};
        if(record_.state==BatchState::signalled && observed_>=record_.value)return {observed_,false,false,false};
        if(polls_>=poll_cap){timed_out_=true;if(record_.state!=BatchState::none)quarantine_locked();return {observed_,false,false,true};}
        ++polls_;backend=backend_;registry=registry_;fence=fence_;
    }
    const auto facts=registry->timeline();
    const uint64_t value=backend->completed(fence); // nonblocking, no lock held
    std::lock_guard<std::mutex> lock(mutex_);
    if(facts.lost || facts.fence!=facts_.fence || facts.queue!=facts_.queue)lose_locked();
    else if(value==u64max)lose_locked();     // device removal: loss, never completion
    else if(value<observed_)lose_locked();   // regression
    else if(value>issued_)lose_locked();     // another signaller: not exclusive
    else observed_=value;
    return {observed_,true,lost_,false};
}
bool Timeline::complete(uint64_t ticket) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticket && ticket==record_.ticket && record_.state==BatchState::signalled &&
        !lost_ && !timed_out_ && observed_>=record_.value;
}
FenceSample Timeline::sample() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    FenceSample s{};
    s.timeline=Key{facts_.device,facts_.queue,facts_.fence,0,0};
    s.value=observed_;s.valid=created_ && fence_ && !lost_ && !timed_out_;
    return s;
}
BatchSummary Timeline::batch() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return record_;}
uint64_t Timeline::member(unsigned i) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return i<record_.count && i<batch_cap ? members_[i] : 0;
}
bool Timeline::quarantined() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_.state==BatchState::quarantined || record_.state==BatchState::signal_failed;
}
bool Timeline::lost() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return lost_;}
Route Timeline::fence_route() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return fence_;}

// ---------------------------------------------------------- RestrictionGate
void RestrictionGate::begin(Restriction r) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    if(in_flight_==(std::numeric_limits<uint32_t>::max)())ambiguous_=true;else ++in_flight_;
    if(r!=Restriction::native_other)intent_=true; // latched BEFORE native dispatch
    if(r==Restriction::begin_pass || r==Restriction::end_pass || r==Restriction::protected_session)
        lifetime_=true;
}
void RestrictionGate::end(Restriction) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    if(!in_flight_)ambiguous_=true;else --in_flight_;
}
void RestrictionGate::reset(bool success) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ++epoch_;
    if(!success || in_flight_ || generation_==u64max){ambiguous_=true;return;}
    ++generation_;intent_=false; // lifetime exclusions and ambiguity persist
}
void RestrictionGate::uncertain() noexcept {std::lock_guard<std::mutex> lock(mutex_);++epoch_;ambiguous_=true;}
bool RestrictionGate::ok_locked() const noexcept {
    return generation_ && !in_flight_ && !intent_ && !lifetime_ && !ambiguous_;
}
Eligibility RestrictionGate::check() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return {generation_,epoch_,ok_locked()};
}
bool RestrictionGate::still_valid(const Eligibility& e) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return e.ok && ok_locked() && e.generation==generation_ && e.epoch==epoch_;
}

// ------------------------------------------------------- part-2 interface
bool recipe_valid(const CopyRecipe& r,uint64_t source_size,uint64_t dest_size) noexcept {
    return r.source && r.dest && r.source!=r.dest && r.bytes && r.bytes<=recipe_byte_cap &&
        r.source_offset<=source_size && r.bytes<=source_size-r.source_offset &&
        r.dest_offset<=dest_size && r.bytes<=dest_size-r.dest_offset;
}
Insert insert_before_draw(const RestrictionGate& gate,const Eligibility& e,bool witness_allowed,
                          PossibleRecording possible,CopyRecorder& recorder,Route list,
                          const CopyRecipe& recipe,uint64_t source_size,uint64_t dest_size) noexcept {
    if(!list || !recipe_valid(recipe,source_size,dest_size) || !gate.still_valid(e) ||
       !witness_allowed || !possible.mark || !possible.mark(possible.context))return Insert::refused;
    // From here the void native command may have been recorded: no rollback.
    (void)recorder.record(list,recipe);
    return Insert::possibly_recorded;
}
bool read_back(const Timeline& timeline,uint64_t ticket,bool closed,ReadbackMap& map,
               uint64_t dest,uint64_t offset,uint64_t bytes,uint8_t* out,size_t capacity) noexcept {
    if(!closed || !out || !bytes || bytes>capacity || bytes>recipe_byte_cap || !timeline.complete(ticket))return false;
    const uint8_t* data=nullptr;
    if(!map.map(dest,offset,bytes,data))return false;
    if(data)std::memcpy(out,data,static_cast<size_t>(bytes));
    map.unmap(dest);
    return data!=nullptr;
}
} // namespace acz::gpu_copy::owner
