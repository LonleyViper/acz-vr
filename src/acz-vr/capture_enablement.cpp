#include "capture_enablement.h"
#include <new>

namespace acz::capture_enablement {
Decision decide(const Inputs& in) noexcept {
    try {
        if(!in.opt_in) return {false,"caller opt-in absent"};
        if(!in.state || !in.observer || !in.caller) return {};
        const auto& s=*in.state;
        const auto& r=*in.caller;
        if(!s.tracking_complete || !in.observer->enabled()) return {false,"tracking incomplete"};
        if(!s.list_identity || !s.generation || !r.list_identity || !r.generation ||
           r.list_identity!=s.list_identity || r.generation!=s.generation || r.revision!=s.revision)
            return {false,"stale list identity/generation/revision"};
        if(!s.pipeline.drawcpt) return {false,"cockpit pipeline unidentified"};
        if(!in.root_id || !in.root_identity || !in.pso_identity || !in.pso_root_identity ||
           in.root_id!=s.root_id || in.root_identity!=s.root_identity ||
           in.pso_identity!=s.pipeline.identity || in.pso_root_identity!=s.pipeline.explicit_root_identity ||
           !s.compatible_root()) return {false,"stale or incompatible root/PSO identity"};
        if(!s.pipeline.requirements_proven) return {false,"all-stage manifest assertion absent"};
        if(!r.indexing_proven) return {false,"indexing assertion absent"};
        if(!r.vertex_fetch_proven) return {false,"vertex-fetch assertion absent"};
        if(r.srt_indices.empty() || r.srt_indices.size()>4096)
            return {false,"explicit shader indices absent or oversized"};
        if(r.vertex_access.size()!=s.pipeline.vertex_slots.size())
            return {false,"vertex-fetch manifest mismatch"};
        for(auto slot:s.pipeline.vertex_slots) {
            size_t matches=0;
            for(const auto& a:r.vertex_access) if(a.slot==slot && a.length) ++matches;
            if(matches!=1) return {false,"vertex-fetch manifest mismatch"};
        }
        if(!r.serialized || !r.synchronized) return {false,"serialization/synchronization assertion absent"};
        if(!r.device) return {false,"device identity absent"};
        if(in.copies.empty() || in.copies.size()!=r.buffers.size() || in.copies.size()>64)
            return {false,"owned copy results absent or mismatched"};
        for(size_t i=0;i<in.copies.size();++i) {
            const auto& c=in.copies[i];
            if(!c.request || !c.result || !*c.result || !c.result->rejection.empty())
                return {false,"copy provider failed"};
            const auto& q=*c.request;
            const auto& result=*c.result;
            const auto& b=result.buffer;
            const auto& expected=r.buffers[i];
            if(!q.device || q.device!=r.device || q.resource!=b.resource ||
               b.resource!=expected.resource || b.bytes!=expected.bytes ||
               b.witness.snapshot!=q.witness.snapshot || b.witness.snapshot!=expected.witness.snapshot)
                return {false,"copy provenance mismatch"};
            if(!in.observer->current(q.witness,s,r.device) ||
               !in.observer->current(b.witness,s,r.device) ||
               !in.observer->current(expected.witness,s,r.device) ||
               !b.resource.id || !b.resource.generation ||
               b.resource!=b.witness.snapshot->resource ||
               in.observer->resource_token(b.resource.id)!=b.resource)
                return {false,"stale copy resource/descriptor provenance"};
            if(!q.stride || !q.elements || q.elements>UINT64_MAX/q.stride || q.offset ||
               q.elements*q.stride!=b.witness.snapshot->resource_bytes ||
               b.bytes->empty() || b.bytes->size()!=b.witness.snapshot->resource_bytes ||
               b.bytes->size()>capture::max_copy_bytes)
                return {false,"incomplete owned copy range"};
            const auto& before=result.before;
            const auto& after=result.after;
            if(!q.required.identity || !q.required.value || !before.version ||
               before.version!=after.version || before.completed.identity!=q.required.identity ||
               after.completed.identity!=q.required.identity || before.completed.value<q.required.value ||
               after.completed.value<q.required.value || after.completed.value<before.completed.value)
                return {false,"source fence/version assertion absent or inconsistent"};
        }
        // Reuse J027's currentness/bound-slot/full owned resource checks. This
        // temporary lease never escapes and freeze is deliberately not invoked.
        const auto assembled=capture_input::assemble(*in.observer,s,r);
        if(!assembled) return {false,"caller descriptor/resource provenance rejected"};
        if(s.pipeline.resources.empty() || s.pipeline.resources.size()>64)
            return {false,"manifest requirements absent or oversized"};
        bool srt=false;
        for(const auto& requirement:s.pipeline.resources) {
            const auto resolved=s.root->resolve(requirement.kind,requirement.reg,requirement.space,requirement.stage);
            if(!resolved.valid) return {false,"manifest root binding unresolved"};
            bool found=false;
            for(const auto& d:r.descriptors) {
                if(d.parameter==resolved.root_index && d.table_offset==resolved.table_offset &&
                   d.metadata.snapshot && d.metadata.snapshot->descriptor.kind==requirement.kind) found=true;
            }
            // This bounded seam requires descriptor-backed buffer requirements.
            // Constants/opaque resources are withheld, never guessed.
            if(!found) return {false,"manifest descriptor absent"};
            if(requirement.kind==metadata::BindingKind::srv && requirement.reg==10 && !requirement.space &&
               requirement.stage==metadata::ShaderStage::geometry) srt=true;
        }
        if(!srt) return {false,"SrtData manifest requirement absent"};
        return {true,"all offline metadata facts present"};
    } catch(const std::bad_alloc&) {return {false,"allocation failure"};}
      catch(...) {return {false,"metadata exception"};}
}
} // namespace acz::capture_enablement
