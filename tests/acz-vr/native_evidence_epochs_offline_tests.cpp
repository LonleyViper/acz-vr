// CPU synthetic fixtures only. No native/GPU operations or proof claims.
#include "native_evidence_epochs.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <initializer_list>
#include <type_traits>

static int allocation_budget=-1;
void* operator new(std::size_t n) {
    if (allocation_budget==0) throw std::bad_alloc();
    if (allocation_budget>0) --allocation_budget;
    if (auto p=std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
static unsigned checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
namespace ep=acz::evidence_epochs;
namespace ne=acz::native_evidence;
namespace dp=acz::draw_provenance;
constexpr auto observed=dp::Origin::FixtureObservation;
static dp::Identity selected_identity() {
    dp::Identity i{};
    i.capture=1; i.device=42; i.device_epoch=10; i.list_lifetime=100;
    i.reset_generation=3; i.reset_revision=5; i.draw_ordinal=7;
    i.submission_serial=50; i.queue=60; i.queue_epoch=20;
    i.pso=300; i.pso_lifetime=301; i.root=200; i.root_lifetime=201;
    i.input_layout=400; i.profile_revision=1; i.all_stage_profile=500;
    i.draw.present=true; i.draw.count=3; i.draw.instances=1;
    return i;
}
static dp::Resource selected_resource(std::size_t n) {
    dp::Resource r{}; r.origin=observed;
    r.id=1000+n; r.lifetime=2000+n; r.generation=1; r.device=42; r.device_epoch=10;
    r.bytes=64; r.copy_bytes=64; r.draw_version=5; r.copy_version=5;
    r.draw_queue=60; r.draw_queue_epoch=20; r.draw_submission=50;
    r.copy_queue=60; r.copy_queue_epoch=20; r.copy_after_submission=50;
    r.history_serial=9; r.complete_history=true; r.writer_excluded=true;
    r.ordered=true; r.current_at_draw=true; r.current_at_copy=true;
    return r;
}
struct Fixture {
    ne::Export metadata{};
    dp::Snapshot candidate{};
    ep::Witnesses retained{};
    ep::Limits limits{ep::max_resources,ep::max_states};
    void reset(std::size_t resources=1, std::uint32_t subresources=2) {
        // Static storage in callers avoids placing multiple large snapshots on
        // the Windows default stack. Retained expectations are initialized
        // independently, then kept unchanged while candidates are mutated.
        metadata={}; candidate={}; retained={}; limits={ep::max_resources,ep::max_states};
        metadata.outcome=ne::Outcome::MetadataObserved;
        metadata.recording_origin=ne::Provenance::ObservedMetadata;
        metadata.coverage=ne::Coverage::CreationOrResetBoundaryObserved;
        metadata.recording={100,3,5,42,201,301};
        candidate.identity=selected_identity(); candidate.origin=observed;
        candidate.copy={observed,42,10,60,20,50,70,true};
        retained.device={1,observed,selected_identity()};
        retained.list={2,observed,selected_identity()};
        retained.queue={3,observed,selected_identity()};
        retained.submission={4,observed,selected_identity()};
        retained.draw={5,observed,selected_identity()};
        retained.copy_record_id=6; retained.copy={observed,42,10,60,20,50,70,true};
        metadata.resource_count=resources; candidate.resource_count=resources; retained.resource_count=resources;
        candidate.state_count=resources*subresources; retained.state_count=resources*subresources;
        for(std::size_t n=0;n<resources;++n) {
            candidate.resources[n]=selected_resource(n);
            retained.resources[n]={100+n,selected_resource(n),0,subresources};
            auto& m=metadata.resources[n]; m.token={1000+n,1}; m.bytes=64; m.device=42;
            m.token_origin=m.extent_origin=m.device_origin=ne::Provenance::ObservedMetadata;
            for(std::uint32_t sub=0;sub<subresources;++sub) {
                const auto k=n*subresources+sub;
                candidate.states[k]={observed,static_cast<std::uint32_t>(n),sub,
                    dp::State::ShaderResource,dp::State::CopySource,dp::State::ShaderResource,true,true,true,false};
                retained.states[k]={1000+k,1000+n,2000+n,1,observed,sub,
                    dp::State::ShaderResource,dp::State::CopySource,dp::State::ShaderResource};
            }
        }
    }
    ep::EpochSnapshot build() const { return ep::build(metadata,candidate,retained,limits); }
};
static Fixture f;
template<class Change> static void reject(Change change) {
    f.reset(); change(f); CHECK(!f.build().synthetic_consistent());
}
static void epoch_and_origin_tests() {
    f.reset(); const auto checked=f.build(); CHECK(checked.synthetic_consistent());
    CHECK(!checked.native_proof_available()); CHECK(checked.unknown_mask()==ne::permanent_unknown_mask);
    CHECK(f.metadata.unknown_mask==ne::permanent_unknown_mask); CHECK(!f.candidate.readiness);
    CHECK(checked.resource(0)!=nullptr); CHECK(checked.resource(ep::max_resources)==nullptr);
    CHECK(ep::EpochSnapshot{}.resource(0)==nullptr);
    static_assert(std::is_same<decltype(checked.witnesses()),const ep::Witnesses&>::value,"immutable witnesses");
    reject([](Fixture& x){x.metadata.unknown_mask=0;});
    for(unsigned bit=0;bit<11;++bit) reject([bit](Fixture& x){x.metadata.unknown_mask &= ~(1u<<bit);});
    reject([](Fixture& x){x.metadata.outcome=ne::Outcome::Refused;});
    reject([](Fixture& x){x.metadata.recording_origin=ne::Provenance::ProviderDeclared;});
    reject([](Fixture& x){x.metadata.coverage=ne::Coverage::Unknown;});
    for(auto member : {&ne::ExpectedPoint::list,&ne::ExpectedPoint::generation,&ne::ExpectedPoint::revision,
                       &ne::ExpectedPoint::device,&ne::ExpectedPoint::root,&ne::ExpectedPoint::pso})
        reject([member](Fixture& x){++(x.metadata.recording.*member);});
    for(auto member : {&ep::Witnesses::device,&ep::Witnesses::list,&ep::Witnesses::queue,
                       &ep::Witnesses::submission,&ep::Witnesses::draw}) {
        reject([member](Fixture& x){(x.retained.*member).record_id=0;});
        reject([member](Fixture& x){(x.retained.*member).origin=dp::Origin::CallerAssertion;});
        reject([member](Fixture& x){++(x.retained.*member).identity.reset_revision;});
    }
    reject([](Fixture& x){x.retained.list.record_id=x.retained.device.record_id;});
    for(auto member : {&dp::Identity::capture,&dp::Identity::device,&dp::Identity::device_epoch,
        &dp::Identity::list_lifetime,&dp::Identity::reset_generation,&dp::Identity::reset_revision,
        &dp::Identity::draw_ordinal,&dp::Identity::submission_serial,&dp::Identity::queue,&dp::Identity::queue_epoch,
        &dp::Identity::pso,&dp::Identity::pso_lifetime,&dp::Identity::root,&dp::Identity::root_lifetime,
        &dp::Identity::input_layout,&dp::Identity::profile_revision,&dp::Identity::all_stage_profile}) {
        auto changed=selected_identity(); ++(changed.*member);
        CHECK(ep::verify_epochs(checked,changed)!=ep::Refusal::None);
        reject([member](Fixture& x){++(x.candidate.identity.*member);});
    }
    for(auto member : {&dp::Draw::count,&dp::Draw::instances,&dp::Draw::start,&dp::Draw::start_instance}) {
        auto changed=selected_identity(); ++(changed.draw.*member);
        CHECK(ep::verify_epochs(checked,changed)!=ep::Refusal::None);
    }
    reject([](Fixture& x){x.candidate.identity.draw.indexed=true;});
    reject([](Fixture& x){x.candidate.identity.draw.present=false;});
    reject([](Fixture& x){x.candidate.identity.draw.base_vertex=1;});
    for(auto origin : {dp::Origin::Missing,dp::Origin::CallerAssertion,dp::Origin::NativeCertified,dp::Origin::Invalid}) {
        reject([origin](Fixture& x){x.candidate.origin=origin;});
        reject([origin](Fixture& x){x.candidate.resources[0].origin=origin;});
        reject([origin](Fixture& x){x.candidate.states[0].origin=origin;});
    }
    for(auto flag : {&dp::Snapshot::copy_proof,&dp::Snapshot::replay_proof,&dp::Snapshot::readiness,
        &dp::Snapshot::requirements_proven,&dp::Snapshot::cpu_data_complete,&dp::Snapshot::gpu_replay_complete})
        reject([flag](Fixture& x){x.candidate.*flag=true;});
}
static void resource_and_copy_tests() {
    for(auto member : {&dp::Resource::id,&dp::Resource::lifetime,&dp::Resource::generation,&dp::Resource::device,
        &dp::Resource::device_epoch,&dp::Resource::offset,&dp::Resource::bytes,&dp::Resource::copy_offset,&dp::Resource::copy_bytes,
        &dp::Resource::draw_version,&dp::Resource::copy_version,&dp::Resource::draw_queue,&dp::Resource::draw_queue_epoch,
        &dp::Resource::draw_submission,&dp::Resource::copy_queue,&dp::Resource::copy_queue_epoch,&dp::Resource::copy_after_submission,
        &dp::Resource::history_serial,&dp::Resource::alias_group})
        reject([member](Fixture& x){++(x.candidate.resources[0].*member);});
    for(auto member : {&dp::Resource::complete_history,&dp::Resource::writer_excluded,&dp::Resource::ordered,
        &dp::Resource::current_at_draw,&dp::Resource::current_at_copy,&dp::Resource::released,
        &dp::Resource::version_wrapped,&dp::Resource::descriptor_overwritten,&dp::Resource::aliased})
        reject([member](Fixture& x){x.candidate.resources[0].*member=!(x.candidate.resources[0].*member);});
    // Even agreement between candidate and a malformed witness cannot override
    // semantic checks. The witness is independent, not inherently trustworthy.
    reject([](Fixture& x){x.candidate.resources[0].copy_version=0; x.retained.resources[0].resource.copy_version=0;});
    reject([](Fixture& x){x.candidate.resources[0].released=true; x.retained.resources[0].resource.released=true;});
    reject([](Fixture& x){x.candidate.resources[0].draw_queue=61; x.retained.resources[0].resource.draw_queue=61;});
    reject([](Fixture& x){x.candidate.resources[0].bytes=65; x.retained.resources[0].resource.bytes=65;});
    reject([](Fixture& x){x.retained.resources[0].record_id=0;});
    reject([](Fixture& x){x.metadata.resources[0].token.generation++;});
    reject([](Fixture& x){x.metadata.resources[0].device++;});
    reject([](Fixture& x){x.metadata.resources[0].bytes=63;});
    reject([](Fixture& x){x.metadata.resources[0].extent_origin=ne::Provenance::ProviderDeclared;});
    reject([](Fixture& x){x.metadata.resource_count=0;});
    reject([](Fixture& x){x.candidate.resource_count=0;});
    reject([](Fixture& x){x.retained.copy_record_id=0;});
    for(auto member : {&dp::Snapshot::CopyPoint::device,&dp::Snapshot::CopyPoint::device_epoch,
        &dp::Snapshot::CopyPoint::queue,&dp::Snapshot::CopyPoint::queue_epoch,
        &dp::Snapshot::CopyPoint::after_submission,&dp::Snapshot::CopyPoint::copy_serial})
        reject([member](Fixture& x){++(x.candidate.copy.*member);});
    reject([](Fixture& x){x.candidate.copy.ordered=false;});
    reject([](Fixture& x){x.candidate.copy.origin=dp::Origin::NativeCertified;});
}
static void restoration_and_bounds_tests() {
    reject([](Fixture& x){x.candidate.state_count=0;});
    reject([](Fixture& x){--x.candidate.state_count;});
    reject([](Fixture& x){--x.candidate.state_count; --x.retained.state_count;}); // expected range still requires two
    reject([](Fixture& x){x.candidate.states[1]=x.candidate.states[0];});
    reject([](Fixture& x){x.retained.states[1]=x.retained.states[0];});
    reject([](Fixture& x){x.candidate.states[0].resource=UINT32_MAX;});
    reject([](Fixture& x){x.candidate.states[0].subresource=2;});
    reject([](Fixture& x){x.retained.states[0].resource_id++;});
    reject([](Fixture& x){x.retained.states[0].lifetime++;});
    reject([](Fixture& x){x.retained.states[0].generation++;});
    reject([](Fixture& x){x.retained.states[0].record_id=0;});
    reject([](Fixture& x){x.retained.resources[0].subresources=0;});
    reject([](Fixture& x){x.retained.resources[0].first_subresource=UINT32_MAX;});
    for(auto member : {&dp::SubresourceState::original,&dp::SubresourceState::copy_read,&dp::SubresourceState::restored})
        reject([member](Fixture& x){x.candidate.states[0].*member=dp::State::Unknown;});
    for(auto member : {&dp::SubresourceState::covered,&dp::SubresourceState::state_known,
                       &dp::SubresourceState::restoration_known,&dp::SubresourceState::conflicting})
        reject([member](Fixture& x){x.candidate.states[0].*member=!(x.candidate.states[0].*member);});
    reject([](Fixture& x){x.candidate.states[0].restored=dp::State::Common;});
    reject([](Fixture& x){x.limits.resources=0;});
    reject([](Fixture& x){x.limits.states=1;});
    reject([](Fixture& x){x.candidate.resource_count=ep::max_resources+1;});
    reject([](Fixture& x){x.retained.resource_count=ep::max_resources+1;});
    reject([](Fixture& x){x.metadata.resource_count=ep::max_resources+1;});
    reject([](Fixture& x){x.candidate.state_count=ep::max_states+1;});
    reject([](Fixture& x){x.retained.state_count=ep::max_states+1;});
    reject([](Fixture& x){x.candidate.input_count=dp::max_entries+1;});
    f.reset(ep::max_resources,2); CHECK(f.build().synthetic_consistent());
    f.candidate.resources[1]=f.candidate.resources[0]; CHECK(!f.build().synthetic_consistent());
    f.reset(2,2); f.candidate.states[2].resource=0; CHECK(!f.build().synthetic_consistent());
}
// Owned fake bytes and private current facts; the caller cannot supply replacement
// versions to copy(). This is a CPU fixture, not a native backend implementation.
struct OwnedCopy { std::array<unsigned char,64> bytes{}; bool complete=false; };
class FakeBackend {
public:
    void initialize(const dp::Snapshot& current,const std::array<unsigned char,64>& bytes) {
        current_=current; bytes_=bytes;
    }
    void later_writer(unsigned char value) {
        bytes_.fill(value); ++current_.resources[0].copy_version; ++current_.resources[0].history_serial;
    }
    OwnedCopy copy(const ep::EpochSnapshot& selected) const {
        OwnedCopy result;
        if (current_.resource_count!=1 || current_.resources[0].offset!=0 || current_.resources[0].bytes!=bytes_.size() ||
            ep::verify_copy_point(selected,current_)!=ep::Refusal::None) return result;
        result.bytes=bytes_; result.complete=true; return result;
    }
    const dp::Snapshot& current() const { return current_; }
private:
    dp::Snapshot current_{};
    std::array<unsigned char,64> bytes_{};
};
static FakeBackend backend;
static dp::Snapshot forged;
static void owned_bytes_tests() {
    f.reset(); const auto selected=f.build();
    std::array<unsigned char,64> source{}; source.fill(0x11);
    backend.initialize(f.candidate,source); source.fill(0x99);
    const auto first=backend.copy(selected); CHECK(first.complete);
    for(auto byte:first.bytes) CHECK(byte==0x11); // backend owned the input
    backend.later_writer(0x22);
    CHECK(dp::same_identity(backend.current().identity,selected.identity()));
    CHECK(!backend.copy(selected).complete);
    forged=backend.current(); forged.resource_count=ep::max_resources+1;
    CHECK(ep::verify_copy_point(selected,forged)==ep::Refusal::CapacityExceeded);
    forged=backend.current(); forged.state_count=ep::max_states+1;
    CHECK(ep::verify_copy_point(selected,forged)==ep::Refusal::CapacityExceeded);
    for(auto byte:first.bytes) CHECK(byte==0x11); // prior output owns its bytes
    forged=backend.current(); forged.resources[0].draw_version=forged.resources[0].copy_version;
    CHECK(ep::verify_copy_point(selected,forged)!=ep::Refusal::None);
    CHECK(!ep::build(f.metadata,forged,f.retained,f.limits).synthetic_consistent());
    // Mutating source expectations after success cannot rewrite the result.
    f.retained.resources[0].resource=forged.resources[0];
    CHECK(selected.resource(0)->draw_version==5);
    CHECK(!backend.copy(selected).complete);
    f.reset(); allocation_budget=0;
    const auto no_alloc=f.build(); CHECK(no_alloc.synthetic_consistent());
    backend.initialize(f.candidate,source); CHECK(backend.copy(no_alloc).complete);
    allocation_budget=-1;
}
int main() {
    epoch_and_origin_tests(); resource_and_copy_tests(); restoration_and_bounds_tests(); owned_bytes_tests();
    std::printf("native evidence epoch CPU fixture checks: %u\n",checks);
    return 0;
}
