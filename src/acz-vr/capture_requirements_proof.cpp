#include "capture_requirements_proof.h"
#include "shader_access_facts.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace acz::requirements {
namespace {
using namespace metadata;
void need(bool b,const char* why) { if(!b) throw std::runtime_error(why); }
uint64_t add(uint64_t a,uint64_t b) { need(b<=UINT64_MAX-a,"proof arithmetic addition overflow");return a+b; }
uint64_t mul(uint64_t a,uint64_t b) { need(!a||b<=UINT64_MAX/a,"proof arithmetic multiplication overflow");return a*b; }
bool same(const capture::Requirement& a,const capture::Requirement& b) {
    return a.kind==b.kind&&a.reg==b.reg&&a.space==b.space&&a.stage==b.stage;
}
bool layout_equal(const RootLayout& a,const RootLayout& b) {
    if(a.version!=b.version||a.flags!=b.flags||a.static_sampler_count!=b.static_sampler_count||a.parameters.size()!=b.parameters.size())return false;
    for(size_t i=0;i<a.parameters.size();++i) {
        const auto& p=a.parameters[i];const auto& q=b.parameters[i];
        if(p.kind!=q.kind||p.visibility!=q.visibility||p.shader_register!=q.shader_register||p.space!=q.space||
           p.flags!=q.flags||p.constant_count!=q.constant_count||p.ranges.size()!=q.ranges.size())return false;
        for(size_t j=0;j<p.ranges.size();++j) {
            const auto& r=p.ranges[j];const auto& s=q.ranges[j];
            if(r.kind!=s.kind||r.count!=s.count||r.base_register!=s.base_register||r.space!=s.space||r.flags!=s.flags||r.offset!=s.offset)return false;
        }
    }return true;
}
bool semantic(const std::string& a,const std::string& b) {
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](unsigned char x,unsigned char y){return std::toupper(x)==std::toupper(y);});
}
struct Format {uint32_t bytes,component,mask;};
Format format(uint32_t f) {
    // Literal DXGI R32 formats; require full allocated signature masks. No
    // typeless conversion, packed formats or default-component guessing.
    switch(f) {
    case 2:return {16,3,15};case 3:return {16,1,15};case 4:return {16,2,15};
    case 6:return {12,3,7};case 7:return {12,1,7};case 8:return {12,2,7};
    case 16:return {8,3,3};case 17:return {8,1,3};case 18:return {8,2,3};
    case 41:return {4,3,1};case 42:return {4,1,1};case 43:return {4,2,1};
    default:throw std::runtime_error("unsupported input-layout format");
    }
}
const capture_input::Buffer& buffer(const capture_input::Request& r,capture::Id id) {
    auto b=std::find_if(r.buffers.begin(),r.buffers.end(),[&](const auto& v){return v.resource.id==id;});
    need(b!=r.buffers.end(),"vertex/index resource has no owned full buffer witness");return *b;
}
void bounds(const capture::BufferView& v,const capture_input::Request& r) {
    need(v.resource&&v.size,"missing/empty bound buffer view");
    const auto& b=buffer(r,v.resource);
    need(b.bytes&&add(v.offset,v.size)<=b.bytes->size(),"bound view exceeds owned resource");
}
}
Result derive(const binding::NativeBindingObserver& observer,const capture::State& state,
              const capture::Command& cmd,const Request& q) noexcept {
    Result result;
    try {
        need(q.copies.serialized&&q.copies.synchronized,"external serialization/synchronization assertions missing");
        need(q.copies.device&&q.copies.list_identity&&q.copies.generation&&
             q.copies.list_identity==state.list_identity&&q.copies.generation==state.generation&&q.copies.revision==state.revision,
             "list identity/generation/revision mismatch");
        need(q.pso_identity&&q.pso_identity==state.pipeline.identity&&q.root_id&&q.root_id==state.root_id&&
             q.root_identity&&q.root_identity==state.root_identity&&state.compatible_root(),"PSO/root identity mismatch");
        need(q.pipeline.root_provenance==pso::RootProvenance::explicit_root,"embedded or unknown root provenance");
        need(q.pipeline.explicit_root_identity==q.root_identity,"inventory/PSO root identity mismatch");
        const auto decoded=pso::build(q.pipeline);
        need(bool(decoded),decoded.rejection.empty()?"stage metadata decode/allocation failed":decoded.rejection.c_str());
        const auto& inv=*decoded.inventory;
        constexpr ShaderStage stages[]={ShaderStage::vertex,ShaderStage::pixel,ShaderStage::domain,ShaderStage::hull,ShaderStage::geometry};
        constexpr const char* names[]={"VS","PS","DS","HS","GS"};
        for(size_t i=0;i<5;++i) {
            const auto& s=inv.stage(stages[i]);
            if(s.state==pso::StageState::unknown)throw std::runtime_error(std::string(names[i])+" stage unknown; explicit absent assertion required");
            if(s.state==pso::StageState::explicitly_absent)continue;
            need(s.shader.valid&&s.shader.stage==stages[i]&&s.shader.major==6&&!s.shader.minor,"missing/unsupported SM6.0 stage metadata");
            if(!s.psv||!s.input||!s.output)throw std::runtime_error(std::string(names[i])+" PSV0/ISG1/OSG1 unavailable; bitcode-only usage unsupported");
            need(!s.psv->uses_view_id.value_or(0),"ViewID-dependent stage unsupported");
        }
        need(inv.stage(ShaderStage::vertex).state==pso::StageState::present&&
             inv.stage(ShaderStage::geometry).state==pso::StageState::present,"VS/GS must be present for cockpit proof");
        // A single pinned stage selects the evidence path for the entire pipeline;
        // partial evidence/caller-rule mixtures are rejected, never downgraded.
        bool evidence=false;
        for(auto stage:stages)if(inv.stage(stage).state==pso::StageState::present&&
            shader_access::known_hash(inv.stage(stage).shader.identifier))evidence=true;
        std::array<const shader_access::Record*,5> access{};
        if(evidence) {
            need(!q.synthetic_indexing,"evidence-derived and synthetic caller indexing rules both supplied");
            for(size_t i=0;i<5;++i)if(inv.stage(stages[i]).state==pso::StageState::present) {
                auto fact=shader_access::lookup(inv.stage(stages[i]));
                need(bool(fact),fact.rejection.empty()?"access facts unavailable":fact.rejection.c_str());
                access[i]=fact.record;
            }
        }
        auto root=RootBindingMetadata::from_serialized(q.serialized_root.data(),q.serialized_root.size());
        need(root.valid(),root.detail.empty()?"explicit serialized root decode failed":root.detail.c_str());
        if(evidence) {
            bool constants=false;
            for(const auto* record:access)if(record)for(size_t i=0;i<record->resource_count;++i) {
                const auto& r=record->resources[i];
                auto kind=r.resource_class==shader_access::ResourceClass::cbv?BindingKind::cbv:
                    r.resource_class==shader_access::ResourceClass::sampler?BindingKind::sampler:BindingKind::srv;
                for(uint32_t j=0;j<r.count;++j) {
                    auto resolved=root.metadata->resolve(kind,r.reg+j,r.space,record->stage);
                    need(resolved.valid,"evidence resource register/space/visibility not resolved by root");
                    constants|=resolved.parameter_kind==ParameterKind::constants;
                }
            }
            // Resolution is a fact of the root bytes; root constants are not a
            // descriptor-backed, whole-resource witness in the current provider.
            need(!constants,"evidence cb10 resolves to root constants; descriptor-backed proof unsupported");
        }
        need(layout_equal(root.metadata->layout(),state.root->layout()),"decoded root/state layout mismatch");
        // First validate all supplied descriptors, bound slots and independent
        // full-resource tokens with J027. No proof or readiness inferred here.
        need(bool(capture_input::assemble(observer,state,q.copies)),"descriptor/resource provenance or owned-copy validation failed");
        auto out=std::make_shared<Proof>();
        out->inventory_=decoded.inventory;out->state_=state;out->command_=cmd;out->root_bytes_=q.serialized_root;
        out->inputs_=q.copies;
        out->inputs_.indexing_proven=false;out->inputs_.vertex_fetch_proven=false;
        out->inputs_.srt_indices.clear();out->inputs_.vertex_access.clear();
        // Independent storage: even a mutable alias to caller storage cannot
        // mutate this proof after construction. External guard excludes races.
        for(auto& b:out->inputs_.buffers)b.bytes=std::make_shared<const std::vector<uint8_t>>(*b.bytes);
        std::vector<bool> used(q.copies.descriptors.size());
        const capture::Descriptor* srt=nullptr;
        for(auto stage:stages) {
            const auto& s=inv.stage(stage);if(s.state!=pso::StageState::present)continue;
            for(const auto& b:s.psv->bindings) {
                // PSVResourceType/Kind/Flag in DxilPipelineStateValidation.h.
                // BindInfo0 lacks kind; do not infer it from bitcode or views.
                need(b.kind&&b.flags,"PSV0 binding kind/flags absent; bitcode-only view kind unsupported");
                need(*b.flags==0,"PSV0 resource flags unsupported");
                need(b.upper!=UINT32_MAX&&uint64_t(b.upper)-b.lower<64,"unbounded/oversized PSV0 register interval");
                BindingKind kind;
                if(b.type==2&&*b.kind==13)kind=BindingKind::cbv;
                else if(b.type==5&&*b.kind==12)kind=BindingKind::srv;
                else throw std::runtime_error("texture/UAV/sampler/raw/typed PSV0 resource unsupported");
                for(uint64_t reg=b.lower;reg<=b.upper;++reg) {
                    capture::Requirement requirement{kind,uint32_t(reg),b.space,stage};
                    need(out->manifest_.size()<64,"all-stage manifest exceeds budget");
                    need(std::none_of(out->manifest_.begin(),out->manifest_.end(),[&](const auto& x){return same(x,requirement);}),"duplicate PSV0 stage binding");
                    const auto resolved=root.metadata->resolve(kind,uint32_t(reg),b.space,stage);
                    need(resolved.valid,"PSV0 register/space/visibility not resolved by root");
                    need(resolved.parameter_kind!=ParameterKind::constants,"root constants cannot supply descriptor-backed PSV0 binding");
                    const capture_input::Descriptor* found=nullptr;
                    for(size_t i=0;i<q.copies.descriptors.size();++i) {
                        const auto& d=q.copies.descriptors[i];
                        if(d.parameter==resolved.root_index&&d.table_offset==resolved.table_offset) {
                            need(!found,"duplicate supplied root binding");found=&d;used[i]=true;
                        }
                    }
                    need(found,"missing descriptor for PSV0 stage binding");
                    const auto& d=found->metadata.snapshot->descriptor;
                    need(d.kind==kind&&d.buffer,"resource kind mismatch/texture view");
                    need(kind!=BindingKind::srv||d.structured,"raw/typed SRV view unsupported");
                    bounds(d.view,out->inputs_);
                    if(kind==BindingKind::srv)need(d.elements&&d.view.stride&&
                        mul(add(d.first_element,d.elements),d.view.stride)<=d.view.size,"structured descriptor bounds invalid");
                    if(evidence&&kind==BindingKind::srv&&reg==10&&!b.space) {
                        const shader_access::Record* record=nullptr;
                        for(const auto* a:access)if(a&&a->stage==stage)record=a;
                        need(record&&record->srt,"access facts: stage has no SrtData read");
                        auto indexed=shader_access::index_srt(*record,d,buffer(out->inputs_,d.view.resource).bytes->size(),cmd);
                        need(bool(indexed),indexed.rejection.empty()?"access indexing unavailable":indexed.rejection.c_str());
                        if(stage==ShaderStage::geometry)out->inputs_.srt_indices=std::move(indexed.elements);
                    }
                    out->manifest_.push_back(requirement);
                    if(kind==BindingKind::srv&&reg==10&&!b.space&&stage==ShaderStage::geometry) {
                        need(!srt,"duplicate GS t10 binding");srt=&d;
                    }
                }
            }
        }
        need(std::all_of(used.begin(),used.end(),[](bool b){return b;}),"extra supplied root binding outside PSV0 manifest");
        need(srt,"GS t10 space0 SrtData binding missing");
        need(srt->view.stride>=srt::declared_size&&srt->view.stride<=2048&&srt->view.stride%4==0,
             "SrtData descriptor stride cannot supply complete payload");
        if(!state.pipeline.resources.empty()) {
            need(state.pipeline.resources.size()==out->manifest_.size(),"state manifest extra/missing binding");
            for(const auto& r:state.pipeline.resources)need(std::count_if(out->manifest_.begin(),out->manifest_.end(),[&](const auto& d){return same(r,d);})==1,"state manifest differs from PSV0");
            for(const auto& r:out->manifest_)need(std::count_if(state.pipeline.resources.begin(),state.pipeline.resources.end(),[&](const auto& d){return same(r,d);})==1,"state manifest duplicates/omits PSV0 binding");
        }
        need(cmd.count&&cmd.instances&&cmd.instances<=4096,"empty/oversized draw arguments");
        if(evidence) {
            // Constant element zero is independent of instance ordinal AND
            // StartInstanceLocation. Each reading stage's descriptor/ranges was
            // independently checked above, including FirstElement and resource.
            need(out->inputs_.srt_indices.size()==cmd.instances,"evidence constant-zero indices unavailable");
        } else {
            need(q.synthetic_indexing&& !q.synthetic_indexing->label.empty()&&q.synthetic_indexing->label.size()<=255,
                 "SrtData element index expression missing from PSV0/ISG1/OSG1; DXIL bitcode not decoded and no labelled synthetic caller rule");
            need(add(q.synthetic_indexing->first,mul(cmd.instances-1,q.synthetic_indexing->step))<srt->elements,
                 "synthetic caller-declared SrtData index out of bounds");
            for(uint64_t i=0;i<cmd.instances;++i) {
                auto n=add(q.synthetic_indexing->first,mul(i,q.synthetic_indexing->step));
                need(n<srt->elements,"synthetic caller-declared SrtData index out of bounds");
                out->inputs_.srt_indices.push_back(n);
            }
        }
        uint64_t first=cmd.start,last=add(cmd.start,uint64_t(cmd.count)-1);
        bool fetched=!cmd.indexed;
        if(cmd.indexed) {
            need(state.index.has_value(),"index view missing");const auto& v=*state.index;
            need((v.stride==2||v.stride==4)&&cmd.count<=capture::max_copy_bytes/v.stride,"index stride/count unsupported");
            bounds(v,out->inputs_);need(mul(add(cmd.start,cmd.count),v.stride)<=v.size,"index fetch exceeds bound view");
            const auto& bytes=*buffer(out->inputs_,v.resource).bytes;
            first=UINT64_MAX;last=0;
            for(uint64_t i=0;i<cmd.count;++i) {
                const auto o=add(v.offset,mul(add(cmd.start,i),v.stride));uint32_t index=0;
                for(uint32_t j=0;j<v.stride;++j)index|=uint32_t(bytes[size_t(o+j)])<<(8*j);
                const auto kind=index_range::classify(index,v.stride,state.topology,state.pipeline.strip_cut);
                need(kind!=index_range::Index::unknown,"strip-cut index requires unavailable topology metadata");
                if(kind==index_range::Index::restart)continue;
                const int64_t n=int64_t(index)+cmd.base_vertex;need(n>=0,"negative base-vertex fetch");
                fetched=true;
                first=(std::min)(first,uint64_t(n));last=(std::max)(last,uint64_t(n));
            }
        } else need(cmd.base_vertex==0,"nonindexed draw has extraneous base vertex");
        const auto& vs=*inv.stage(ShaderStage::vertex).input;
        const auto& psv_inputs=inv.stage(ShaderStage::vertex).psv->signatures[0];
        need(psv_inputs.size()==vs.size(),"VS PSV0/ISG1 signature count mismatch or unavailable PSV0 signature");
        need(vs.size()==inv.input_layout.size(),"input-layout/VS ISG1 element count mismatch");
        std::vector<bool> matched(vs.size());
        std::map<uint32_t,std::pair<uint64_t,uint64_t>> spans;
        std::map<uint32_t,std::vector<std::pair<uint64_t,uint64_t>>> elements;
        for(const auto& e:inv.input_layout) {
            need(!e.classification&&!e.step_rate,"per-instance/step-rate input layout unsupported");
            need(e.offset!=UINT32_MAX,"APPEND input-layout offset unsupported");
            const auto f=format(e.format);const pso::SignatureElement* sig=nullptr;
            for(const auto& old:elements[e.slot])need(e.offset>=old.second||add(e.offset,f.bytes)<=old.first,"aliased input-layout elements unsupported");
            elements[e.slot].push_back({e.offset,add(e.offset,f.bytes)});
            for(size_t i=0;i<vs.size();++i)if(semantic(e.semantic,vs[i].semantic)&&e.index==vs[i].index) {
                need(!sig&&!matched[i],"duplicate input-layout/ISG1 semantic");sig=&vs[i];matched[i]=true;
            }
            need(sig&&sig->stream==0&&!sig->system_value&&!sig->min_precision&&sig->reg<32&&
                 sig->component_type==f.component&&sig->mask==f.mask,"input-layout/ISG1 semantic/register/mask/format mismatch");
            // PSVSignatureElement0 uses DxilProgramSigCompType, like ISG1.
            // This bounded subset excludes packed rows and system-value IA.
            size_t psv_matches=0;
            for(const auto& p:psv_inputs) {
                const auto cols=p.cols_and_start&15,start=(p.cols_and_start>>4)&3;
                if(semantic(p.semantic,sig->semantic)&&p.indices==std::vector<uint32_t>{sig->index}&&
                   p.rows==1&&(p.cols_and_start&0x40)&&p.start_row==sig->reg&&
                   p.component_type==sig->component_type&&p.semantic_kind==0&&
                   ((p.dynamic_mask_and_stream>>4)&3)==0&&(((1u<<cols)-1)<<start)==sig->mask)++psv_matches;
            }
            need(psv_matches==1,"VS PSV0/ISG1 semantic/register/mask mismatch");
            for(const auto& other:vs)if(&other!=sig)need(other.reg!=sig->reg,"packed/aliased ISG1 registers unsupported");
            auto v=state.vertices.find(e.slot);need(v!=state.vertices.end()&&v->second.stride,"vertex slot has no bound view/stride");
            bounds(v->second,out->inputs_);
            need(e.offset%4==0&&v->second.stride%4==0,"unaligned R32 input-layout offset/stride unsupported");
            need(add(e.offset,f.bytes)<=v->second.stride,"input element exceeds vertex stride");
            const auto begin=fetched?add(mul(first,v->second.stride),e.offset):0;
            const auto end=fetched?add(add(mul(last,v->second.stride),e.offset),f.bytes):0;
            need(end<=v->second.size,"vertex fetch exceeds bound view");
            auto it=spans.find(e.slot);
            if(it==spans.end())spans.emplace(e.slot,std::make_pair(begin,end));
            else {it->second.first=(std::min)(it->second.first,begin);it->second.second=(std::max)(it->second.second,end);}
        }
        need(std::all_of(matched.begin(),matched.end(),[](bool b){return b;}),"VS ISG1 element missing from input layout");
        for(const auto& v:spans) {
            out->slots_.push_back(v.first);out->inputs_.vertex_access.push_back({v.first,v.second.first,v.second.second-v.second.first});
        }
        if(!state.pipeline.vertex_slots.empty())need(state.pipeline.vertex_slots==out->slots_,"state vertex-slot manifest mismatch");
        out->inputs_.indexing_proven=true;out->inputs_.vertex_fetch_proven=true;
        out->facts_={{"PSO association, stage presence/absence, explicit root, descriptor shapes, layout and draw arguments",Origin::caller_declared,"native creation history is not encoded in parts"},
            {"all-stage finite resource manifest and root binding resolution",Origin::derived_from_parts,"PSV0 and SDK-decoded explicit root; current observer snapshots"},
            {"vertex fetch spans",Origin::derived_from_parts,"ISG1 matched to explicit per-vertex input layout; checked draw/index arithmetic"},
            {"SrtData indexing",evidence?Origin::evidence_derived:Origin::caller_declared,evidence?"E11 constant element zero; per-stage checked read ranges":q.synthetic_indexing->label},
            {"CPU synchronization and native observation completeness",Origin::caller_declared,"external guard/provider obligations; no runtime verification"}};
        if(evidence)for(const auto* record:access)if(record) {
            out->facts_[3].evidence_ids=record->evidence_ids;
            if(!out->facts_[3].container_sha256.empty())out->facts_[3].container_sha256+="; ";
            out->facts_[3].container_sha256+=record->container_sha256;
            out->facts_.push_back({record->srt?"stage SrtData constant element zero and read ranges":"stage has no SrtData read",
                Origin::evidence_derived,"exact shader HASH and recomputed full-container SHA-256; PSV0 multiset matched",
                record->evidence_ids,record->container_sha256});
        }
        need(bool(capture_input::assemble(observer,state,out->inputs_)),"derived inputs rejected by capture_input_provider");
        result.proof=std::move(out);
    } catch(const std::exception& e) {try{result.rejection=e.what();}catch(...){}}
      catch(...) {try{result.rejection="proof exception/allocation failure";}catch(...){}}
    return result;
}
}

namespace acz::requirements {
CoverageResult check_selected_coverage(const pso::Inventory& inv,
    const metadata::RootBindingMetadata& root, const evidence_epochs::EpochSnapshot& epochs,
    const draw_provenance::Profile& profile, const draw_provenance::Snapshot& candidate) noexcept {
    namespace dp=draw_provenance;
    CoverageResult out;
    try {
        need(epochs.synthetic_consistent(),"selected epochs unavailable");
        need(evidence_epochs::verify_epochs(epochs,profile.identity)==evidence_epochs::Refusal::None,
             "selected epoch/profile mismatch");
        need(root.valid(),"selected root metadata unavailable");
        need(profile.count<=dp::max_entries,"selected profile capacity");
        need(evidence_epochs::verify_copy_point(epochs,candidate)==evidence_epochs::Refusal::None,
             "selected candidate differs from retained epochs");
        return check_selected_inputs(inv,root,profile,candidate);
    } catch(const std::exception& e) {try {out.rejection=e.what();}catch(...) {}}
      catch(...) {try {out.rejection="selected coverage exception/allocation failure";}catch(...) {}}
    return out;
}
CoverageResult check_selected_inputs(const pso::Inventory& inv,
    const metadata::RootBindingMetadata& root, const draw_provenance::Profile& profile,
    const draw_provenance::Snapshot& candidate,bool diagnostic_ownership_pins) noexcept {
    namespace dp=draw_provenance;
    CoverageResult out;
    try {
        need(root.valid(),"selected root metadata unavailable");
        need(inv.root_provenance==pso::RootProvenance::explicit_root&&
             inv.explicit_root_identity==profile.identity.root_lifetime,"selected inventory/root lifetime mismatch");
        need(profile.identity.draw.count&&profile.identity.draw.instances,"selected empty draw");
        need(profile.count<=dp::max_entries,"selected profile capacity");
        constexpr metadata::ShaderStage stages[]={metadata::ShaderStage::vertex,metadata::ShaderStage::pixel,
            metadata::ShaderStage::geometry,metadata::ShaderStage::hull,metadata::ShaderStage::domain};
        constexpr dp::Stage mapped[]={dp::Stage::VS,dp::Stage::PS,dp::Stage::GS,dp::Stage::HS,dp::Stage::DS};
        std::vector<dp::Key> expected;
        struct Table { uint32_t parameter; uint64_t heap,lifetime,revision,capacity,base,extent;
            dp::HeapClass heap_class; };
        std::vector<Table> tables;
        uint32_t used=1u<<unsigned(dp::Stage::IA),absent=1u<<unsigned(dp::Stage::CS);
        for(size_t i=0;i<5;++i) {
            const auto& stage=inv.stage(stages[i]);
            if(stage.state==pso::StageState::explicitly_absent) {
                need(i>=3,"selected VS/PS/GS absent");absent|=1u<<unsigned(mapped[i]);continue;
            }
            need(stage.state==pso::StageState::present,"selected stage unknown");
            const auto access=shader_access::lookup(stage);
            need(bool(access),access.rejection.empty()?"selected stage access unavailable":access.rejection.c_str());
            need(access.record->stage==stages[i],"selected authenticated shader stage differs from pipeline slot");
            used|=1u<<unsigned(mapped[i]);
            for(size_t j=0;j<access.record->resource_count;++j) {
                const auto& resource=access.record->resources[j];
                using RC=shader_access::ResourceClass;
                const auto binding=resource.resource_class==RC::cbv?metadata::BindingKind::cbv:
                    resource.resource_class==RC::sampler?metadata::BindingKind::sampler:metadata::BindingKind::srv;
                const auto resolved=root.resolve(binding,resource.reg,resource.space,stages[i]);
                need(resolved.valid,"selected register/space/visibility unresolved (static samplers unsupported)");
                dp::Kind kind=dp::Kind::Unknown;
                if(resource.resource_class==RC::cbv)kind=resolved.parameter_kind==metadata::ParameterKind::constants?
                    dp::Kind::RootConstants:dp::Kind::RootDescriptor;
                else if(resource.resource_class==RC::srv_structured)kind=dp::Kind::StructuredBuffer;
                else if(resource.resource_class==RC::srv_typed)kind=dp::Kind::Texture;
                else if(resource.resource_class==RC::sampler)kind=dp::Kind::Sampler;
                need(resource.count==1,"selected resource array unsupported");
                // Table CBVs need their own bounded category; do not relabel them
                // as root descriptors. Root SRVs likewise remain a later subset.
                need((kind==dp::Kind::RootConstants&&resolved.parameter_kind==metadata::ParameterKind::constants)||
                     (kind==dp::Kind::RootDescriptor&&resolved.parameter_kind==metadata::ParameterKind::cbv)||
                     ((kind==dp::Kind::StructuredBuffer||kind==dp::Kind::Texture||kind==dp::Kind::Sampler)&&
                      resolved.parameter_kind==metadata::ParameterKind::table),"selected binding form unsupported");
                dp::Key key{mapped[i],kind,resource.reg,resource.space,resolved.root_index,0};
                expected.push_back(key);
                const dp::Required* req=nullptr;
                for(size_t k=0;k<profile.count;++k)if(dp::equal(profile.required[k].key,key)) {
                    need(!req,"selected binding ambiguous");req=&profile.required[k];
                }
                need(req,kind==dp::Kind::Texture?"selected PS texture input missing":
                    kind==dp::Kind::Sampler?"selected PS sampler input missing":
                    kind==dp::Kind::RootConstants?"selected cb10 root words missing":
                    kind==dp::Kind::RootDescriptor?"selected cb10 root CBV missing":"selected SrtData input missing");
                if(kind==dp::Kind::RootConstants) {
                    const auto& parameter=root.layout().parameters[resolved.root_index];
                    const auto count=parameter.constant_count;
                    // E11 authenticates consumed VS x / PS xy. Only this two-word
                    // layout may use the diagnostic eight-byte subset. Everything
                    // else retains the original four-word minimum.
                    const bool narrow=diagnostic_ownership_pins&&count==2&&parameter.visibility==metadata::Visibility::all&&
                        access.record->cbv_register_zero&&
                        access.record->cb10_consumed_words==(i==0?shader_access::e11_vs_cb10_words:
                            i==1?shader_access::e11_ps_cb10_words:0)&&i<2;
                    need(req->offset==0&&req->root_words<=count&&
                        (narrow?(req->root_words==2&&req->bytes==8):(req->root_words>=4&&req->bytes>=16)),
                        "selected cb10 root words incomplete");
                }
                if(kind==dp::Kind::RootDescriptor)need(req->offset==0&&req->bytes>=16,"selected cb10 buffer range incomplete");
                if(kind==dp::Kind::StructuredBuffer)need(req->stride==416&&req->bytes>=416,"selected SrtData element zero incomplete");
                if(resolved.parameter_kind==metadata::ParameterKind::table) {
                    need(req->descriptor==dp::DescriptorKind::Dynamic&&req->descriptor_slot>=resolved.table_offset,
                         "selected descriptor table offset incomplete");
                    const uint64_t base=req->descriptor_slot-resolved.table_offset;
                    const auto& parameter=root.layout().parameters[resolved.root_index];
                    uint64_t extent=0;
                    for(const auto& range:parameter.ranges) {
                        need(range.count&&range.offset!=metadata::append_range&&
                             uint64_t(range.count)<=UINT64_MAX-uint64_t(range.offset),
                             "selected descriptor table range invalid");
                        const uint64_t end=uint64_t(range.offset)+uint64_t(range.count);
                        if(end>extent)extent=end;
                    }
                    const auto heap_class=binding==metadata::BindingKind::sampler?
                        dp::HeapClass::Sampler:dp::HeapClass::CbvSrvUav;
                    need(req->heap_class==heap_class&&req->heap_capacity&&req->table_extent==extent&&
                         req->table_base==base&&base<req->heap_capacity&&
                         extent<=req->heap_capacity-base,
                         "selected descriptor table heap class or bounds incomplete");
                    for(const auto& table:tables)if(table.parameter==resolved.root_index)
                        need(table.heap==req->heap&&table.lifetime==req->heap_lifetime&&
                             table.revision==req->heap_revision&&table.heap_class==req->heap_class&&
                             table.capacity==req->heap_capacity&&table.base==base&&table.extent==extent,
                             "selected shared table heap or base conflict");
                    tables.push_back({resolved.root_index,req->heap,req->heap_lifetime,req->heap_revision,
                        req->heap_capacity,base,extent,req->heap_class});
                }
            }
        }
        need(inv.input_layout.size()==4,"selected IA layout incomplete");
        for(size_t i=0;i<4;++i) {
            const auto& e=inv.input_layout[i];
            need(semantic(e.semantic,"ATTRIBUTE")&&e.index==i&&e.format==2&&e.slot==0&&e.offset==i*16&&
                 !e.classification&&!e.step_rate,"selected IA layout unsupported");
        }
        expected.push_back({dp::Stage::IA,dp::Kind::Vertex,0,0,0,0});
        if(profile.identity.draw.indexed)expected.push_back({dp::Stage::IA,dp::Kind::Index,0,0,0,0});
        need(profile.used_stages==used&&profile.absent_stages==absent,"selected all-stage profile incomplete");
        need(profile.count==expected.size(),"selected input category missing or extra");
        for(const auto& key:expected) {
            size_t matches=0;
            for(size_t i=0;i<profile.count;++i)if(dp::equal(key,profile.required[i].key)) {
                ++matches;
                if(key.kind==dp::Kind::Vertex)need(profile.required[i].stride==64,"selected IA stride unsupported");
            }
            need(matches==1,key.kind==dp::Kind::Vertex?"selected IA vertex input missing or ambiguous":
                key.kind==dp::Kind::Index?"selected IA index input missing or ambiguous":"selected input missing or ambiguous");
        }
        // The selected E10 root metadata resolves each shared register to one
        // retained root/table binding. This restriction belongs to this adapter;
        // the generic provenance contract permits independently pinned stage views.
        for(size_t i=0;i<profile.count;++i) {
            const auto& a=profile.required[i];
            if(a.key.stage==dp::Stage::IA || a.key.kind==dp::Kind::RootConstants ||
               a.key.kind==dp::Kind::Sampler)continue;
            for(size_t j=0;j<i;++j) {
                const auto& b=profile.required[j];
                if(b.key.stage==dp::Stage::IA || b.key.kind!=a.key.kind ||
                   b.key.root_parameter!=a.key.root_parameter ||
                   b.key.reg!=a.key.reg || b.key.space!=a.key.space)continue;
                need(a.resource_id==b.resource_id && a.resource_lifetime==b.resource_lifetime &&
                     a.resource_generation==b.resource_generation && a.resource_version==b.resource_version &&
                     a.resource_window_offset==b.resource_window_offset && a.resource_window_bytes==b.resource_window_bytes &&
                     a.offset==b.offset && a.bytes==b.bytes && a.stride==b.stride &&
                     a.descriptor==b.descriptor && a.heap==b.heap && a.heap_lifetime==b.heap_lifetime &&
                     a.heap_revision==b.heap_revision && a.descriptor_slot==b.descriptor_slot &&
                     a.descriptor_revision==b.descriptor_revision && a.descriptor_source==b.descriptor_source,
                     "selected shared binding descriptor conflict");
            }
        }
        auto contract=std::make_unique<dp::Contract>();
        need(contract->begin({dp::max_resources,dp::max_entries,dp::max_states,dp::max_root_words,dp::max_sampler_words},profile,candidate),
             "selected provenance capacity/attempt refused");
        const auto finished=contract->finish();
        need(finished,dp::diagnostic(contract->reason()));
        out.synthetic_consistent=true;
    } catch(const std::exception& e) {try {out.rejection=e.what();}catch(...) {}}
      catch(...) {try {out.rejection="selected coverage exception/allocation failure";}catch(...) {}}
    return out;
}
}
