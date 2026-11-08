// Standalone focused suite. Reuse the pinned E10 decoder and selected fixtures
// without duplicating evidence parsing or installing another CMake target.
// Compile this translation unit in place of capture_requirements_proof_offline_tests.cpp,
// adding native_evidence_epochs.cpp to that target's existing source/link inputs.
#define ACZ_SELECTED_EPOCH_TESTS
#include "capture_requirements_proof_offline_tests.cpp"

namespace ep=acz::evidence_epochs;
namespace ne=acz::native_evidence;
static ne::Export exported;
static ep::Witnesses witnesses;
static void epoch_fixture() {
    exported={};witnesses={};
    const auto& c=selected.candidate;
    exported.outcome=ne::Outcome::MetadataObserved;
    exported.recording_origin=ne::Provenance::ObservedMetadata;
    exported.coverage=ne::Coverage::CreationOrResetBoundaryObserved;
    exported.recording={100,3,5,42,201,301};
    witnesses.device={1,dp::Origin::FixtureObservation,c.identity};
    witnesses.list={2,dp::Origin::FixtureObservation,c.identity};
    witnesses.queue={3,dp::Origin::FixtureObservation,c.identity};
    witnesses.submission={4,dp::Origin::FixtureObservation,c.identity};
    witnesses.draw={5,dp::Origin::FixtureObservation,c.identity};
    witnesses.copy_record_id=6;witnesses.copy=c.copy;
    exported.resource_count=witnesses.resource_count=c.resource_count;
    witnesses.state_count=c.state_count;
    for(size_t i=0;i<c.resource_count;++i) {
        const auto& r=c.resources[i];auto& m=exported.resources[i];
        m.token={r.id,r.generation};m.bytes=r.bytes;m.device=r.device;
        m.token_origin=m.extent_origin=m.device_origin=ne::Provenance::ObservedMetadata;
        witnesses.resources[i]={100+i,r,0,1};
    }
    for(size_t i=0;i<c.state_count;++i) {
        const auto& s=c.states[i];const auto& r=c.resources[s.resource];
        witnesses.states[i]={1000+i,r.id,r.lifetime,r.generation,dp::Origin::FixtureObservation,s.subresource,s.original,s.copy_read,s.restored};
    }
}
int main(int argc,char** argv) {
    run_requirements_tests(argc,argv);
    // The earlier suite leaves pinned immutable containers in selected.inventory.
    // Retain their bytes before resetting the fixture.
    const std::array<std::vector<uint8_t>,3> real{
        selected.inventory->stage(ShaderStage::vertex).container,
        selected.inventory->stage(ShaderStage::pixel).container,
        selected.inventory->stage(ShaderStage::geometry).container};
    for(bool indexed:{false,true})for(bool cbv:{false,true}) {
        selected.reset(real,indexed,cbv);epoch_fixture();
        const auto epochs=ep::build(exported,selected.candidate,witnesses,{ep::max_resources,ep::max_states});
        check(epochs.synthetic_consistent(),"retained selected L3-A epochs");
        auto run=[&] {return proof::check_selected_coverage(*selected.inventory,*selected.root,epochs,selected.profile,selected.candidate);};
        const auto result=run();if(!result.synthetic_consistent)std::cerr<<result.rejection<<'\n';
        check(result.synthetic_consistent,"complete all-stage input plus epoch synthetic coverage");
        check(epochs.unknown_mask()==ne::permanent_unknown_mask&&!epochs.native_proof_available(),"unknown native facts preserved");
        // Compare directly with the retained L3-A predicate. CallerAssertion is
        // valid generic provenance, but cannot replace any retained epoch origin.
        auto origins=[&](dp::Origin& origin) {
            for(auto weaker:{dp::Origin::Missing,dp::Origin::CallerAssertion,dp::Origin::NativeCertified,dp::Origin::Invalid}) {
                origin=weaker;
                check(ep::verify_copy_point(epochs,selected.candidate)!=ep::Refusal::None,"L3-A origin mutation refused");
                check(!run().synthetic_consistent,"selected coverage preserves L3-A origin refusal");
                if(weaker==dp::Origin::CallerAssertion) {
                    auto generic=std::make_unique<dp::Contract>();
                    check(generic->begin({dp::max_resources,dp::max_entries,dp::max_states,dp::max_root_words,dp::max_sampler_words},
                                         selected.profile,selected.candidate)&&generic->finish(),"caller origin accepted only by generic policy");
                }
            }
            origin=dp::Origin::FixtureObservation;
            check(run().synthetic_consistent,"restored epoch origin accepted");
        };
        origins(selected.candidate.origin);origins(selected.candidate.copy.origin);
        for(size_t i=0;i<selected.candidate.resource_count;++i)origins(selected.candidate.resources[i].origin);
        for(size_t i=0;i<selected.candidate.state_count;++i)origins(selected.candidate.states[i].origin);
        selected.candidate.resources[0].history_serial++;
        check(!run().synthetic_consistent,"retained history mismatch refused");selected.candidate.resources[0].history_serial--;
        selected.candidate.states[0].original=dp::State::Common;
        check(!run().synthetic_consistent,"retained state mismatch refused");
    }
    std::cout<<"Selected-draw input and epoch fixtures passed; no native byte/copy/replay proof.\n";
}
