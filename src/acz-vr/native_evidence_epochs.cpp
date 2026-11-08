#include "native_evidence_epochs.h"

namespace acz::evidence_epochs {
namespace ne = native_evidence;
namespace {
bool fixture(dp::Origin o) noexcept { return o==dp::Origin::FixtureObservation; }
bool same_selected_draw_identity(const dp::Identity& a, const dp::Identity& b) noexcept {
    return dp::complete(a) && dp::complete(b) && dp::same_identity(a,b) && dp::same_draw(a.draw,b.draw);
}
bool same_copy(const dp::Snapshot::CopyPoint& a, const dp::Snapshot::CopyPoint& b) noexcept {
    return a.origin==b.origin && a.device==b.device && a.device_epoch==b.device_epoch &&
        a.queue==b.queue && a.queue_epoch==b.queue_epoch &&
        a.after_submission==b.after_submission && a.copy_serial==b.copy_serial && a.ordered==b.ordered;
}
bool same_resource(const dp::Resource& a, const dp::Resource& b) noexcept {
    return a.origin==b.origin && a.id==b.id && a.lifetime==b.lifetime &&
        a.generation==b.generation && a.device==b.device && a.device_epoch==b.device_epoch &&
        a.offset==b.offset && a.bytes==b.bytes && a.copy_offset==b.copy_offset && a.copy_bytes==b.copy_bytes &&
        a.draw_version==b.draw_version && a.copy_version==b.copy_version &&
        a.draw_queue==b.draw_queue && a.draw_queue_epoch==b.draw_queue_epoch && a.draw_submission==b.draw_submission &&
        a.copy_queue==b.copy_queue && a.copy_queue_epoch==b.copy_queue_epoch && a.copy_after_submission==b.copy_after_submission &&
        a.history_serial==b.history_serial && a.alias_group==b.alias_group &&
        a.complete_history==b.complete_history && a.writer_excluded==b.writer_excluded && a.ordered==b.ordered &&
        a.current_at_draw==b.current_at_draw && a.current_at_copy==b.current_at_copy && a.released==b.released &&
        a.version_wrapped==b.version_wrapped && a.descriptor_overwritten==b.descriptor_overwritten && a.aliased==b.aliased;
}
Refusal check_snapshot(const dp::Identity& identity, const Witnesses& w, const dp::Snapshot& p) noexcept {
    if (p.resource_count>max_resources || p.state_count>max_states || p.input_count>dp::max_entries)
        return Refusal::CapacityExceeded;
    if (p.copy_proof || p.replay_proof || p.readiness || p.requirements_proven || p.cpu_data_complete || p.gpu_replay_complete)
        return Refusal::ProofForgery;
    if (!fixture(p.origin)) return Refusal::Origin;
    if (!same_selected_draw_identity(identity,p.identity)) return Refusal::IdentityMismatch;
    const auto& c=p.copy;
    if (!fixture(c.origin) || !same_copy(c,w.copy) || !c.copy_serial || !c.ordered ||
        c.device!=identity.device || c.device_epoch!=identity.device_epoch ||
        c.queue!=identity.queue || c.queue_epoch!=identity.queue_epoch || c.after_submission!=identity.submission_serial)
        return Refusal::CopyPoint;
    if (!p.resource_count || p.resource_count!=w.resource_count) return Refusal::ResourceIdentity;
    if (p.state_count!=w.state_count) return Refusal::Restoration;
    for (std::size_t i=0; i<p.resource_count; ++i) {
        const auto& r=p.resources[i];
        const ResourceWitness* expected=nullptr;
        for (std::size_t j=0; j<w.resource_count; ++j)
            if (w.resources[j].resource.id==r.id) expected=&w.resources[j];
        if (!expected || !fixture(r.origin) || !same_resource(r,expected->resource) ||
            !r.id || !r.lifetime || !r.generation || r.device!=identity.device || r.device_epoch!=identity.device_epoch)
            return Refusal::ResourceIdentity;
        for (std::size_t j=0; j<i; ++j) if (p.resources[j].id==r.id) return Refusal::ResourceIdentity;
        std::uint64_t end=0;
        if (!r.bytes || r.copy_offset!=r.offset || r.copy_bytes!=r.bytes || !dp::checked_add(r.offset,r.bytes,end))
            return Refusal::ResourceRange;
        if (!r.draw_version || r.draw_version!=r.copy_version || !r.history_serial || !r.complete_history ||
            !r.writer_excluded || !r.ordered || !r.current_at_draw || !r.current_at_copy || r.released ||
            r.version_wrapped || r.descriptor_overwritten || r.aliased || r.alias_group ||
            r.draw_queue!=identity.queue || r.draw_queue_epoch!=identity.queue_epoch || r.draw_submission!=identity.submission_serial ||
            r.copy_queue!=c.queue || r.copy_queue_epoch!=c.queue_epoch || r.copy_after_submission!=c.after_submission)
            return Refusal::ResourceHistory;
    }
    for (std::size_t i=0; i<p.state_count; ++i) {
        const auto& st=p.states[i];
        if (st.resource>=p.resource_count) return Refusal::Restoration;
        const auto& r=p.resources[st.resource];
        const StateWitness* expected=nullptr;
        for (std::size_t j=0; j<w.state_count; ++j)
            if (w.states[j].resource_id==r.id && w.states[j].subresource==st.subresource) expected=&w.states[j];
        if (!expected || !fixture(st.origin) || !st.covered || !st.state_known || !st.restoration_known || st.conflicting ||
            !dp::known(st.original) || st.copy_read!=dp::State::CopySource || st.restored!=st.original ||
            st.original!=expected->original || st.copy_read!=expected->copy_read || st.restored!=expected->restored)
            return Refusal::Restoration;
        for (std::size_t j=0; j<i; ++j)
            if (p.states[j].resource==st.resource && p.states[j].subresource==st.subresource) return Refusal::Restoration;
    }
    return Refusal::None;
}
} // namespace
const char* diagnostic(Refusal r) noexcept {
    switch(r) {
    case Refusal::None:return "synthetic consistent";
    case Refusal::ExportRefused:return "export refused";
    case Refusal::MetadataMaskChanged:return "native unknown mask changed";
    case Refusal::RecordingMismatch:return "recording mismatch";
    case Refusal::IdentityMismatch:return "identity mismatch";
    case Refusal::MissingWitness:return "missing fixture witness";
    case Refusal::WitnessMismatch:return "fixture witness mismatch";
    case Refusal::CapacityExceeded:return "capacity exceeded";
    case Refusal::ProofForgery:return "proof forgery";
    case Refusal::Origin:return "fixture origin required";
    case Refusal::CopyPoint:return "copy point mismatch";
    case Refusal::ResourceIdentity:return "resource identity mismatch";
    case Refusal::ResourceRange:return "resource range mismatch";
    case Refusal::ResourceHistory:return "resource history mismatch";
    case Refusal::Restoration:return "restoration coverage mismatch";
    }
    return "invalid refusal";
}
EpochSnapshot build(const ne::Export& e, const dp::Snapshot& p, const Witnesses& w, Limits limits) noexcept {
    EpochSnapshot result;
    const auto fail=[&](Refusal r) noexcept { result.refusal_=r; return result; };
    if (e.outcome!=ne::Outcome::MetadataObserved) return fail(Refusal::ExportRefused);
    if (e.unknown_mask!=ne::permanent_unknown_mask) return fail(Refusal::MetadataMaskChanged);
    if (!limits.resources || limits.resources>max_resources || !limits.states || limits.states>max_states ||
        !w.resource_count || w.resource_count>limits.resources || !w.state_count || w.state_count>limits.states ||
        p.resource_count>limits.resources || p.state_count>limits.states || e.resource_count>max_resources || e.entry_count>ne::max_selections)
        return fail(Refusal::CapacityExceeded);
    const auto& id=p.identity;
    if (!dp::complete(id)) return fail(Refusal::IdentityMismatch);
    if (e.recording_origin!=ne::Provenance::ObservedMetadata || e.coverage!=ne::Coverage::CreationOrResetBoundaryObserved ||
        e.recording.device!=id.device || e.recording.list!=id.list_lifetime ||
        e.recording.generation!=id.reset_generation || e.recording.revision!=id.reset_revision ||
        e.recording.root!=id.root_lifetime || e.recording.pso!=id.pso_lifetime)
        return fail(Refusal::RecordingMismatch);
    const EpochWitness* epochs[]={&w.device,&w.list,&w.queue,&w.submission,&w.draw};
    for (std::size_t i=0; i<5; ++i) {
        if (!epochs[i]->record_id || !fixture(epochs[i]->origin)) return fail(Refusal::MissingWitness);
        if (!same_selected_draw_identity(epochs[i]->identity,id)) return fail(Refusal::WitnessMismatch);
        for (std::size_t j=0; j<i; ++j)
            if (epochs[j]->record_id==epochs[i]->record_id) return fail(Refusal::WitnessMismatch);
    }
    if (!w.copy_record_id || !fixture(w.copy.origin)) return fail(Refusal::MissingWitness);
    std::size_t expected_states=0;
    for (std::size_t i=0; i<w.resource_count; ++i) {
        const auto& rw=w.resources[i]; const auto& r=rw.resource;
        if (!rw.record_id || !rw.subresources || rw.subresources>max_states ||
            rw.first_subresource>UINT32_MAX-(rw.subresources-1) || rw.subresources>limits.states-expected_states)
            return fail(Refusal::Restoration);
        expected_states+=rw.subresources;
        for (std::size_t j=0; j<i; ++j)
            if (w.resources[j].resource.id==r.id || w.resources[j].record_id==rw.record_id) return fail(Refusal::ResourceIdentity);
        const ne::ResourceRecord* metadata=nullptr;
        for (std::size_t j=0; j<e.resource_count; ++j) if (e.resources[j].token.id==r.id) {
            if (metadata) return fail(Refusal::ResourceIdentity);
            metadata=&e.resources[j];
        }
        if (!metadata || metadata->token.generation!=r.generation || metadata->device!=r.device ||
            metadata->token_origin!=ne::Provenance::ObservedMetadata || metadata->extent_origin!=ne::Provenance::ObservedMetadata ||
            metadata->device_origin!=ne::Provenance::ObservedMetadata) return fail(Refusal::ResourceIdentity);
        if (!dp::span(r.offset,r.bytes,metadata->bytes)) return fail(Refusal::ResourceRange);
    }
    if (e.resource_count!=w.resource_count || expected_states!=w.state_count) return fail(Refusal::Restoration);
    for (std::size_t i=0; i<w.state_count; ++i) {
        const auto& st=w.states[i]; const ResourceWitness* rw=nullptr;
        for (std::size_t j=0; j<w.resource_count; ++j) if (w.resources[j].resource.id==st.resource_id) rw=&w.resources[j];
        if (!rw || !st.record_id || !fixture(st.origin) || st.lifetime!=rw->resource.lifetime ||
            st.generation!=rw->resource.generation || st.subresource<rw->first_subresource ||
            st.subresource-rw->first_subresource>=rw->subresources || !dp::known(st.original) ||
            st.copy_read!=dp::State::CopySource || st.restored!=st.original) return fail(Refusal::Restoration);
        for (std::size_t j=0; j<i; ++j)
            if (w.states[j].record_id==st.record_id ||
                (w.states[j].resource_id==st.resource_id && w.states[j].subresource==st.subresource)) return fail(Refusal::Restoration);
    }
    const auto checked=check_snapshot(id,w,p);
    if (checked!=Refusal::None) return fail(checked);
    result.identity_=id; result.witnesses_=w; result.refusal_=Refusal::None;
    return result;
}
Refusal verify_epochs(const EpochSnapshot& s, const dp::Identity& expected) noexcept {
    if (!s.synthetic_consistent()) return s.refusal();
    return same_selected_draw_identity(s.identity(),expected) ? Refusal::None : Refusal::IdentityMismatch;
}
Refusal verify_copy_point(const EpochSnapshot& s, const dp::Snapshot& current) noexcept {
    if (!s.synthetic_consistent()) return s.refusal();
    return check_snapshot(s.identity(),s.witnesses(),current);
}
} // namespace acz::evidence_epochs
