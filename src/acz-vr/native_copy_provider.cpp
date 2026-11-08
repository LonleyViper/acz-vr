#include "native_copy_provider.h"
#include <algorithm>
#include <stdexcept>

namespace acz::native_copy {
namespace {
void require(bool condition, const char* why) {
    if(!condition) throw std::runtime_error(why);
}
void current(const binding::NativeBindingObserver& observer,
             const capture::State& state, const Request& r) {
    require(r.witness.generation && bool(observer.current(r.witness,state,r.device)),"stale copy metadata");
    require(r.resource.id && r.resource.generation &&
        r.resource==r.witness.snapshot->resource &&
        observer.resource_token(r.resource.id)==r.resource,"stale copy resource token");
}
void fence(const Stamp& stamp, const Fence& required) {
    require(stamp.version && stamp.completed.identity==required.identity &&
        stamp.completed.value>=required.value,"source fence/version assertion missing");
}
}
Result acquire(const binding::NativeBindingObserver& observer, const capture::State& state,
               Source& source, const Request& request) noexcept {
    try {
        // Pin caller metadata before invoking any injected code.
        const Request r=request;
        current(observer,state,r);
        require(r.required.identity && r.required.value,"required fence token missing");
        require(r.stride && r.elements && r.elements<=UINT64_MAX/r.stride,"copy stride overflow/empty range");
        const uint64_t length=r.elements*r.stride;
        require(r.offset<=UINT64_MAX-length,"copy range overflow");
        require(!r.offset && length==r.witness.snapshot->resource_bytes,"partial/out-of-bounds copy range");
        require(length<=capture::max_copy_bytes,"copy byte budget exceeded");
        const Stamp before=source.inspect(r);
        fence(before,r.required);
        current(observer,state,r);
        auto bytes=std::make_shared<std::vector<uint8_t>>(size_t(length));
        require(source.copy(r,bytes->data(),bytes->size())==bytes->size(),"incomplete source copy");
        const Stamp after=source.inspect(r);
        fence(after,r.required);
        require(before.version==after.version,"source mutation detected");
        require(after.completed.value>=before.completed.value,"source fence regression");
        current(observer,state,r);
        // No mutable alias escapes; publication happens only after all checks.
        Result result;
        result.buffer={r.resource,r.witness,std::move(bytes)};
        result.before=before;result.after=after;
        return result;
    } catch(const std::exception& e) {
        Result result;try {result.rejection=e.what();} catch(...) {}return result;
    } catch(...) {
        Result result;try {result.rejection="copy source/allocation failure";} catch(...) {}return result;
    }
}

namespace {
bool same_fake_identity(const FakeIdentity& a, const FakeIdentity& b) noexcept {
    return a.device==b.device && a.list==b.list && a.generation==b.generation &&
        a.revision==b.revision && a.queue==b.queue && a.queue_epoch==b.queue_epoch &&
        a.submission==b.submission;
}
bool complete_fake_identity(const FakeIdentity& a) noexcept {
    return a.device && a.list && a.generation && a.revision && a.queue &&
        a.queue_epoch && a.submission;
}
}
OwnedFakeAttempt::OwnedFakeAttempt(size_t bytes, uint32_t subresources)
    : bytes_(bytes), subresources_(subresources) {}
bool OwnedFakeAttempt::write(size_t offset, const uint8_t* data, size_t count) noexcept {
    if (sealed_ || attempted_ || quarantined_ || retired_ || !data || !count ||
        offset>bytes_.size() || count>bytes_.size()-offset || version_==UINT64_MAX)
        return false;
    std::copy_n(data,count,bytes_.data()+offset);
    ++version_;
    return true;
}
bool OwnedFakeAttempt::seal() noexcept {
    if (sealed_ || attempted_ || quarantined_ || retired_ || version_<=1 ||
        bytes_.empty() || bytes_.size()>capture::max_copy_bytes ||
        !subresources_ || subresources_>draw_provenance::max_states)
        return false;
    sealed_=true;
    return true;
}
void OwnedFakeAttempt::unobservable_writer() noexcept { quarantined_=true; }
void OwnedFakeAttempt::submit(const FakeSubmission& event) noexcept {
    if (sealed_ && !submitted_ && !attempted_ && !quarantined_ && !retired_)
        submission_=event; // Preserve even an ambiguous fake commit identity.
    if (!sealed_ || submitted_ || completed_ || attempted_ || quarantined_ || retired_ ||
        !event.submitted || event.ambiguous || !complete_fake_identity(event.identity) ||
        !event.resource.id || !event.resource.generation || event.bytes!=bytes_.size() ||
        !event.signal.identity || !event.signal.value || event.signal.value==UINT64_MAX ||
        event.source_version!=version_) { quarantined_=true; return; }
    submitted_=true;
}
void OwnedFakeAttempt::complete(const FakeCompletion& event) noexcept {
    if (submitted_ && !completed_ && !attempted_ && !quarantined_ && !retired_)
        completion_=event; // Preserve the uncertain completion report.
    if (!submitted_ || completed_ || attempted_ || quarantined_ || retired_ ||
        event.status!=FakeCompletionStatus::Confirmed || !event.independently_confirmed ||
        !same_fake_identity(event.identity,submission_.identity) ||
        event.completed.identity!=submission_.signal.identity ||
        event.completed.value!=submission_.signal.value) { quarantined_=true; return; }
    completed_=true;
}
Result OwnedFakeAttempt::refuse(const char* reason) noexcept {
    quarantined_=true;
    Result result;
    try { result.rejection=reason; } catch (...) {}
    return result;
}
Stamp OwnedFakeAttempt::inspect(const Request&) {
    return {completion_.completed,version_};
}
size_t OwnedFakeAttempt::copy(const Request&, uint8_t* destination, size_t capacity) {
    ++copies_;
    if (capacity!=bytes_.size()) throw std::runtime_error("fake source extent mismatch");
    std::copy(bytes_.begin(),bytes_.end(),destination);
    return bytes_.size();
}
Result OwnedFakeAttempt::finish(const binding::NativeBindingObserver& observer,
                                const capture::State& state, const Request& request,
                                const FakeIdentity& expected, const FakeSubresource* states,
                                size_t state_count) noexcept {
    if (attempted_ || retired_) return refuse("fake attempt already consumed");
    attempted_=true;
    if (quarantined_ || !sealed_ || !submitted_ || !completed_)
        return refuse("fake writer/submission/completion incomplete");
    if (!complete_fake_identity(expected) ||
        !same_fake_identity(expected,submission_.identity) ||
        !same_fake_identity(expected,completion_.identity) ||
        expected.device!=request.device || expected.list!=state.list_identity ||
        expected.generation!=state.generation || expected.revision!=state.revision ||
        request.required.identity!=submission_.signal.identity ||
        request.required.value!=submission_.signal.value ||
        request.resource!=submission_.resource ||
        submission_.source_version!=version_)
        return refuse("fake identity/submission mismatch");
    if (!states || state_count!=subresources_ || subresources_>draw_provenance::max_states)
        return refuse("fake restoration inventory incomplete");
    bool seen[draw_provenance::max_states]{};
    for (size_t i=0;i<state_count;++i) {
        const auto& s=states[i];
        if (s.index>=subresources_ || seen[s.index] ||
            !draw_provenance::known(s.original) ||
            s.copy_read!=draw_provenance::State::CopySource ||
            s.restored!=s.original || !s.original_observed ||
            !s.copy_transition_observed || !s.restoration_observed ||
            !s.alias_excluded || !s.split_barrier_excluded ||
            !s.implicit_transition_excluded)
            return refuse("fake restoration unproven");
        seen[s.index]=true;
    }
    auto result=acquire(observer,state,*this,request);
    if (!result) { quarantined_=true; return result; }
    copied_=true;
    return result;
}
bool OwnedFakeAttempt::retire() noexcept {
    if (!copied_ || quarantined_ || retired_) return false;
    bytes_.clear();
    submission_={};
    completion_={};
    retired_=true;
    return true;
}
} // namespace acz::native_copy
