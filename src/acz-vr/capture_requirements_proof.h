#pragma once
#include "pso_stage_inventory.h"
#include "capture_input_provider.h"
#include "native_evidence_epochs.h"
#include "root_constant_witness.h"
#include "shader_access_facts.h"
#include "cpu_upload_snapshot.h"

namespace acz::requirements {
// Separate from the buffer-copy Proof below. Checks a retained fixture profile
// against exact E10/E11 stage metadata and L3-A epochs, then checks its candidate
// with the provenance policy. No bytes, native certification or enablement.
// Shader keys/presence and cb10 consumed-component facts come from pinned E10/E11;
// resource identities, texture content/layout, sampler values and index extrema
// remain independently retained fixture expectations, never decoded GPU facts.
// Authenticated records must match their requested pipeline slots. All bindings
// of a root table retain one heap identity/lifetime/revision and table base.
// The complete entry point preserves L3-A's fixture-only epoch origins;
// generic caller assertions cannot replace the retained epoch observations.
// Unsupported: table CBVs, root SRVs, static samplers, per-instance IA and
// unregistered shaders. Next discriminating check requires independently retained
// native binding/content witnesses; synthetic consistency does not supply them.
struct CoverageResult {
    bool synthetic_consistent=false;
    std::string rejection;
};
// Input/profile portion only: does not check epochs. The complete entry point
// below additionally requires L3-A's retained epoch snapshot.
CoverageResult check_selected_inputs(const pso::Inventory&,
    const metadata::RootBindingMetadata&, const draw_provenance::Profile&,
    const draw_provenance::Snapshot&,bool diagnostic_ownership_pins=false) noexcept;
CoverageResult check_selected_coverage(const pso::Inventory&,
    const metadata::RootBindingMetadata&, const evidence_epochs::EpochSnapshot&,
    const draw_provenance::Profile&, const draw_provenance::Snapshot&) noexcept;
enum class Origin { derived_from_parts, caller_declared, rejected, evidence_derived };
struct Fact { std::string name; Origin origin=Origin::rejected; std::string reason; std::string evidence_ids; std::string container_sha256; };
// This is exclusively a synthetic caller rule, NEVER a decoded shader rule.
// index(i) = first + i*step; independent of StartInstanceLocation.
struct SyntheticIndexRule {
    std::string label;
    uint64_t first=0, step=0;
};
struct Request {
    pso::Request pipeline;
    // Explicit serialized root, not an embedded-container root. CPU SDK decode.
    std::vector<uint8_t> serialized_root;
    capture::Id pso_identity=0, root_id=0, root_identity=0;
    capture_input::Request copies;
    std::optional<SyntheticIndexRule> synthetic_indexing;
};
class Proof;
struct Result {
    std::shared_ptr<const Proof> proof;
    std::string rejection;
    explicit operator bool() const noexcept { return bool(proof); }
};
// Caller holds J027's external serialization guard throughout build/use.
// Association of supplied containers/layout/root bytes with the PSO lifetime is
// explicitly caller-declared. Parts do not encode a native PSO identity.
// This class owns independent CPU bytes, decoded parts and capture provenance.
// No method changes State, readiness, hooks or enablement.
class Proof {
public:
    const std::vector<capture::Requirement>& manifest() const noexcept { return manifest_; }
    const std::vector<uint32_t>& vertex_slots() const noexcept { return slots_; }
    const capture_input::Request& inputs() const noexcept { return inputs_; }
    const std::vector<Fact>& facts() const noexcept { return facts_; }
    const pso::Inventory& inventory() const noexcept { return *inventory_; }
    const capture::State& state() const noexcept { return state_; }
    const capture::Command& command() const noexcept { return command_; }
private:
    std::shared_ptr<const pso::Inventory> inventory_;
    capture::State state_;
    capture::Command command_;
    std::vector<uint8_t> root_bytes_;
    std::vector<capture::Requirement> manifest_;
    std::vector<uint32_t> slots_;
    capture_input::Request inputs_;
    std::vector<Fact> facts_;
    friend Result derive(const binding::NativeBindingObserver&, const capture::State&,
                         const capture::Command&, const Request&) noexcept;
};
// SM6.0 classic stages, finite CBV/structured-SRV declarations, explicit offsets,
// R32{,G32,B32,A32}_{FLOAT,UINT,SINT}, per-vertex IA only. Unsupported facts reject.
// Exact E10 containers use E11 evidence-derived constant-zero indexing.
// Other containers require a synthetic rule; mixing caller and evidence rules rejects.
// Root constants, textures and samplers still lack supported buffer-copy proof.
// On allocation/exception failure publishes nothing (detail may be empty).
Result derive(const binding::NativeBindingObserver&, const capture::State&,
              const capture::Command&, const Request&) noexcept;

// D-81 diagnostic, deliberately separate from derive()/Proof. No native bytes
// are read or frozen and no readiness is inferred. An optional J-132 adapter
// evaluates only a previously stable diagnostic CPU snapshot. E10/E11 identity must
// have been checked at PSO creation (all VS/PS/GS hashes AND full containers,
// absent HS/DS). The caller serializes the entire check with live observation.
// These callbacks resolve METADATA only; native adapters must use locked
// variants, never public lock-taking resolvers under the hook mutex.
struct MetadataResolver {
    void* context=nullptr;
    binding::NativeResolution (*table)(void*,const capture::State&,uint32_t,capture::Id,uint32_t)=nullptr;
    binding::NativeResolution (*root)(void*,const capture::State&,uint32_t,uint32_t,uint32_t,uint32_t)=nullptr;
    binding::NativeResolution (*view)(void*,const capture::State&,const capture::BufferView&)=nullptr;
    bool (*current)(void*,const capture::State&,const binding::NativeResolution&)=nullptr;
    bool ia_layout_matches=false; // observed E10 ATTRIBUTE[0..3], float4, offsets 0/16/32/48, slot 0 per-vertex
    // Optional diagnostic source only. Native hook supplies this after Draw
    // returned, while holding its existing observation guard. No caller boolean
    // substitutes for the native J-118 interval/coverage and creation SHA pins.
    const diagnostics::RootConstantWitness::Snapshot* constants=nullptr;
    uint32_t vs_cb10_words=0,ps_cb10_words=0;
    unsigned recording_thread=0;
    // Diagnostic-only texture adapter. Invoked at t0 in requirements order;
    // native implementation rechecks the covered slot and permanent own pin.
    bool (*owned_t0)(void*,const capture::State&,uint32_t,uint32_t)=nullptr;
    // Non-null only for the diagnostic opt-in. Failure must not fall back to
    // an uncovered generic sampler. Serialized static s0 uses immutable root facts.
    bool (*covered_s0)(void*,const capture::State&,uint32_t,uint32_t)=nullptr;
    // Diagnostic-only stable selected-draw bytes. Never a generic Inputs lease
    // or synchronization certificate. The native callback rechecks maps/pins.
    const diagnostics::CpuUploadSnapshot* bytes=nullptr;
    const diagnostics::CpuUploadSnapshot::Request* byte_request=nullptr;
    bool (*bytes_current)(void*,const diagnostics::CpuUploadSnapshot::Request&)=nullptr;
};
struct MetadataResult {
    // Static first failure, or the literal success outcome. No borrowed data.
    const char* outcome="rejected:metadata exception/allocation failure";
    bool proved=false;
    bool index_bytes_verified=false;
    bool constants_covered=false;
    // Partial selected inputs only; Missing origin deliberately prevents their
    // reuse as a complete generic/native provenance certificate. Exact bits
    // survive the adapter even when a later metadata requirement refuses.
    std::array<draw_provenance::Input,2> root_constants{};
    uint32_t cb10_kind=UINT32_MAX,ia_stride=0;
    uint64_t srt_begin=0,srt_end=0,first_element=0,srt_stride=0;
    uint64_t vertex_begin=0,vertex_end=0;
    // Diagnostic-only exact accepted resource spans, never readiness facts.
    std::array<binding::Token,3> cpu_resources{};
    std::array<uint64_t,3> cpu_offsets{},cpu_sizes{};
};
// D-117: a finite diagnostic vocabulary, never a key/address or eligibility
// grant. Order is explicit so a missing later descriptor cannot hide identity.
inline constexpr const char* metadata_identity_reasons[]={
    "rejected:PSO/root PSO lifetime unavailable;ps=0",
    "rejected:PSO/root bound root unobserved;bk=0",
    "rejected:PSO/root bound metadata unaccepted;layout=0",
    "rejected:PSO/root bound metadata invalid;layout=invalid",
    "rejected:PSO/root explicit lifetime unavailable;pr=0",
    "rejected:PSO/root bound lifetime unavailable;br=0",
    "rejected:PSO/root lifetime mismatch;life=ne"
};
inline const char* metadata_identity_failure(const capture::State& s) noexcept {
    if(!s.pipeline.identity)return metadata_identity_reasons[0];
    if(!s.root_id)return metadata_identity_reasons[1];
    if(!s.root)return metadata_identity_reasons[2];
    if(!s.root->valid())return metadata_identity_reasons[3];
    if(!s.pipeline.explicit_root_identity)return metadata_identity_reasons[4];
    if(!s.root_identity)return metadata_identity_reasons[5];
    if(s.pipeline.explicit_root_identity!=s.root_identity)return metadata_identity_reasons[6];
    return nullptr;
}
// Fixed-storage adapter for the RootConstants portion of selected inputs only.
// Other input categories/epochs are NOT certified by this result. The native
// witness is the value source; words are never inferred from fixture defaults.
inline bool covered_constants(const capture::State& s,const MetadataResolver& r,
                              std::array<draw_provenance::Input,2>& input) noexcept {
    namespace dp=draw_provenance;
    using metadata::ParameterKind;
    const auto* w=r.constants;
    if(!w||w->reason!=diagnostics::RootConstantWitness::covered||
       r.vs_cb10_words!=shader_access::e11_vs_cb10_words||
       r.ps_cb10_words!=shader_access::e11_ps_cb10_words||
       w->size!=2||w->stages!=3||(w->mask&3)!=3||
       !w->revision||!w->before||w->after<=w->before||!r.recording_thread||
       w->thread!=r.recording_thread||w->list!=s.list_identity||w->generation!=s.generation||
       w->binding_revision!=s.revision||w->root!=s.root_id||w->token!=s.root_identity||
       !s.root||!s.root->valid())return false;
    // configureConstantWitness independently resolved VS and PS when the root
    // was bound. Matching key/token, stages==3 and index/size reuse that cached
    // resolution; direct indexed layout checks add no Draw-time root scan.
    const auto& layout=s.root->layout();
    if(w->index>=layout.parameters.size())return false;
    const auto& p=layout.parameters[w->index];
    if(p.kind!=ParameterKind::constants||p.shader_register!=10||p.space||
       p.visibility!=metadata::Visibility::all||p.constant_count!=w->size||
       (layout.flags&(2u|32u)))return false;
    for(unsigned i=0;i<2;++i) {
        auto& x=input[i];
        x.key={i?dp::Stage::PS:dp::Stage::VS,dp::Kind::RootConstants,10,0,w->index,0};
        x.root_id=w->root;x.root_lifetime=w->token;
        x.word_count=w->size;x.bytes=w->size*4ull;
        // Do not label this as NativeCertified: the generic Contract is still
        // fixture-only and this partial native check grants no full proof.
        for(unsigned j=0;j<w->size;++j)x.words[j]=w->values[j];
        // The former Required was zero-initialized, then given these exact
        // counts/bytes/words. Only offset and descriptor can still differ.
        // size==2 was checked above, within the generic root-word limit.
        static_assert(dp::max_root_words>=2);
        if(x.offset||x.descriptor!=dp::DescriptorKind::None)return false;
    }
    // Enforce the contract's shared-parameter equality for the two stage views.
    return input[0].key.root_parameter==input[1].key.root_parameter&&
        dp::root_views_match(input[0],input[1]);
}
// Header-only so existing hook-only offline targets need no new link inputs.
// Success means metadata-proved only. The optional byte seam additionally checks
// index bounds in a same-draw CPU snapshot; texture content, synchronization and
// replay are NOT proved. Unsupported texture snapshots
// fail closed with the existing observer/root APIs. The diagnostic opt-in
// additionally handles covered s0 tables or the bounded serialized static s0.
inline void check_metadata(const capture::State& s,const capture::Command& cmd,
                           const MetadataResolver& r,MetadataResult& out) noexcept {
    out.outcome="rejected:metadata exception/allocation failure";
    out.proved=false;out.index_bytes_verified=false;out.constants_covered=false;out.cb10_kind=UINT32_MAX;out.ia_stride=0;
    out.srt_begin=out.srt_end=out.first_element=out.srt_stride=0;
    out.vertex_begin=out.vertex_end=0;out.cpu_resources={};out.cpu_offsets={};out.cpu_sizes={};
    for(auto& input:out.root_constants)input={};
    try {
        auto reject=[&](const char* why){out.outcome=why;};
        if(!s.list_identity||!s.generation||!s.tracking_complete||!r.table||!r.root||!r.view||!r.current)
            return reject("rejected:unknown/stale list generation or incomplete observation");
        if(const auto* why=metadata_identity_failure(s))return reject(why);
        using metadata::BindingKind;using metadata::ParameterKind;using metadata::ShaderStage;
        const auto cb=s.root->resolve(BindingKind::cbv,10,0,ShaderStage::pixel);
        if(cb.valid)out.cb10_kind=static_cast<uint32_t>(cb.parameter_kind);
        binding::Token srt_resource;
        auto resolve=[&](const metadata::BindingResolution& b,uint32_t bytes=0) {
            if(b.parameter_kind!=ParameterKind::table)
                return r.root(r.context,s,b.root_index,bytes,bytes?0u:1u,bytes?0u:416u);
            binding::NativeResolution found;
            for(auto heap:s.heaps) {
                auto candidate=r.table(r.context,s,b.root_index,heap,b.table_offset);
                if(candidate) {
                    // A handle which resolves in two heaps is ambiguous.
                    if(found)return binding::NativeResolution{};
                    found=std::move(candidate);
                }
            }
            return found;
        };
        // Both reading stages independently resolve element zero. Views may
        // differ by visibility; reject differing spans rather than guess.
        for(auto stage:{ShaderStage::vertex,ShaderStage::geometry}) {
            const auto b=s.root->resolve(BindingKind::srv,10,0,stage);
            if(!b.valid)return reject("rejected:t10 register/space/visibility unresolved");
            const auto value=resolve(b);
            if(!value||!r.current(r.context,s,value))return reject("rejected:t10 missing/stale/unsupported descriptor");
            const auto& snap=*value.snapshot;const auto& d=snap.descriptor;
            if(d.kind!=BindingKind::srv||!d.buffer||!d.structured||d.view.stride!=416||!d.elements)
                return reject("rejected:t10 structured stride 416 and element zero required");
            if(d.view.offset>snap.resource_bytes||d.view.size>snap.resource_bytes-d.view.offset||
               d.first_element>UINT64_MAX/416||d.first_element*416>d.view.size||
               416>d.view.size-d.first_element*416)
                return reject("rejected:t10 element zero exceeds current resource span");
            const auto begin=d.view.offset+d.first_element*416;
            if(stage==ShaderStage::geometry&&(out.srt_begin!=begin||out.srt_end!=begin+416||
                out.first_element!=d.first_element||srt_resource!=snap.resource))
                return reject("rejected:VS/GS t10 spans differ");
            srt_resource=snap.resource;
            out.cpu_resources[0]=snap.resource;out.cpu_offsets[0]=begin;out.cpu_sizes[0]=416;
            out.srt_begin=begin;out.srt_end=begin+416;out.first_element=d.first_element;out.srt_stride=416;
        }
        // Preserve the existing refusal unless the opt-in native source is covered.
        if(!cb.valid)return reject("rejected:cb10 register/space/visibility unresolved");
        if(cb.parameter_kind==ParameterKind::constants) {
            if(!covered_constants(s,r,out.root_constants))
                return reject("rejected:evidence cb10 resolves to root constants; descriptor-backed proof unsupported");
            out.constants_covered=true;
        } else for(auto stage:{ShaderStage::vertex,ShaderStage::pixel}) {
            const auto b=s.root->resolve(BindingKind::cbv,10,0,stage);
            if(!b.valid||b.parameter_kind==ParameterKind::constants)
                return reject("rejected:cb10 register/space/visibility unresolved");
            const auto value=resolve(b,256); // root-CBV provider span; E11 reads register zero (16 bytes).
            if(!value||!r.current(r.context,s,value)||value.snapshot->descriptor.kind!=BindingKind::cbv||
               !value.snapshot->descriptor.buffer||value.snapshot->descriptor.view.size<16)
                return reject("rejected:cb10 missing/stale/unsupported descriptor");
        }
        for(auto kind:{BindingKind::srv,BindingKind::sampler}) {
            if(kind==BindingKind::sampler&&r.covered_s0&&s.root->serialized_ps_s0())continue;
            const auto b=s.root->resolve(kind,0,0,ShaderStage::pixel);
            if(!b.valid||b.parameter_kind!=ParameterKind::table)
                return reject(kind==BindingKind::srv?"rejected:t0 register/space/visibility unresolved":"rejected:s0 register/space/visibility unresolved");
            if(kind==BindingKind::srv&&r.owned_t0&&
               r.owned_t0(r.context,s,b.root_index,b.table_offset))continue;
            if(kind==BindingKind::sampler&&r.covered_s0) {
                if(r.covered_s0(r.context,s,b.root_index,b.table_offset))continue;
                return reject("rejected:s0 missing/stale/unsupported descriptor");
            }
            const auto value=resolve(b);
            if(!value||!r.current(r.context,s,value)||value.snapshot->descriptor.kind!=kind)
                return reject(kind==BindingKind::srv?"rejected:t0 missing/stale/unsupported descriptor":"rejected:s0 missing/stale/unsupported descriptor");
        }
        if(!cmd.count||!cmd.instances)return reject("rejected:empty draw arguments");
        if(!r.ia_layout_matches)return reject("rejected:PSO input layout differs from E10 IA contract");
        const auto vertex=s.vertices.find(0);
        if(vertex==s.vertices.end()||vertex->second.stride!=64)
            return reject("rejected:IA slot zero view/stride 64 required");
        out.ia_stride=vertex->second.stride;
        const auto v=r.view(r.context,s,vertex->second);
        if(!v||!r.current(r.context,s,v))return reject("rejected:IA slot zero missing/stale resource span");
        const auto& d=v.snapshot->descriptor.view;
        out.cpu_resources[1]=v.snapshot->resource;out.cpu_offsets[1]=d.offset;out.cpu_sizes[1]=d.size;
        if(!cmd.indexed) {
            const uint64_t end=(uint64_t(cmd.start)+cmd.count)*64; // UINT32 + UINT32, then *64 fits UINT64.
            if(cmd.base_vertex||end>d.size)return reject("rejected:draw vertex range exceeds IA slot zero view");
            out.vertex_begin=d.offset+uint64_t(cmd.start)*64;out.vertex_end=d.offset+end;
        } else {
            // Index values/base-vertex cannot establish vertex fetch bounds
            // without reading bytes. Only validate the current bound IB span.
            if(!s.index||(s.index->stride!=2&&s.index->stride!=4))
                return reject("rejected:index view missing/unsupported stride");
            const auto index=r.view(r.context,s,*s.index);
            if(!index||!r.current(r.context,s,index))return reject("rejected:index view missing/stale resource span");
            if((uint64_t(cmd.start)+cmd.count)*s.index->stride>index.snapshot->descriptor.view.size)
                return reject("rejected:draw index range exceeds index view");
            out.cpu_resources[2]=index.snapshot->resource;
            out.cpu_offsets[2]=index.snapshot->descriptor.view.offset;out.cpu_sizes[2]=index.snapshot->descriptor.view.size;
            out.vertex_begin=d.offset;out.vertex_end=d.offset+d.size;
            if(r.bytes||r.byte_request) {
                const auto* q=r.byte_request;
                if(!r.bytes||!q||!r.bytes_current||!r.bytes_current(r.context,*q)||!r.bytes->matches(*q)||
                   q->list!=s.list_identity||q->generation!=s.generation||q->revision!=s.revision||
                   q->start!=cmd.start||q->count!=cmd.count||q->base!=cmd.base_vertex||
                   q->instances!=cmd.instances||q->start_instance!=cmd.start_instance||
                   q->vertex_stride!=out.ia_stride||q->index_stride!=s.index->stride||
                   q->topology!=s.topology||q->strip_cut!=s.pipeline.strip_cut)
                    return reject("rejected:stale/unstable CPU snapshot");
                for(size_t i=0;i<3;++i)if(q->ranges[i].resource!=out.cpu_resources[i].id||
                    q->ranges[i].lifetime!=out.cpu_resources[i].generation||q->ranges[i].offset!=out.cpu_offsets[i]||
                    q->ranges[i].bytes!=out.cpu_sizes[i])return reject("rejected:CPU snapshot resource span mismatch");
                index_range::Result range;
                const auto ib=size_t(q->ranges[0].bytes+q->ranges[1].bytes);
                index_range::evaluate(r.bytes->first.data()+ib,size_t(q->ranges[2].bytes),q->index_stride,
                    cmd.start,cmd.count,cmd.base_vertex,d.size/out.ia_stride,s.topology,s.pipeline.strip_cut,range);
                if(!range.verified)return reject(range.reason);
                out.index_bytes_verified=true;
                out.vertex_begin=d.offset;out.vertex_end=d.offset;
                if(range.fetched) {
                    out.vertex_begin+=uint64_t(int64_t(range.minimum)+cmd.base_vertex)*out.ia_stride;
                    out.vertex_end+=(uint64_t(int64_t(range.maximum)+cmd.base_vertex)+1)*out.ia_stride;
                }
            }
        }
        out.proved=true;
        // Historical outcome name: index bounds apply ONLY to sampled diagnostic
        // bytes; the selected draw's byte use and GPU completion remain unproven.
        out.outcome=out.index_bytes_verified?"metadata-proved:index-range-verified":
            cmd.indexed?"metadata-proved:index-range-unverified-without-bytes":"metadata-proved";
    } catch(...) {} // Failure is a static literal even when new throws.
}
}
