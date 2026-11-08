#include "pso_stage_inventory.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace acz::pso {
namespace {
uint32_t word(const uint8_t* p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
void require(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
size_t index(ShaderStage s) {
    switch(s) { case ShaderStage::vertex:return 0; case ShaderStage::pixel:return 1;
    case ShaderStage::domain:return 2; case ShaderStage::hull:return 3; case ShaderStage::geometry:return 4;
    default:throw std::runtime_error("unsupported inventory stage"); }
}
struct Reader {
    const uint8_t* p; size_t n, pos=0;
    const uint8_t* take(size_t count, size_t stride, const char* why) {
        require(stride && count<=(n-pos)/stride,why);
        const auto* r=p+pos; pos+=count*stride; return r;
    }
    uint32_t u(const char* why) { return word(take(1,4,why)); }
};
std::string string_at(const uint8_t* p, size_t n, size_t o, const char* why) {
    require(o<n,why);
    const auto* z=static_cast<const uint8_t*>(std::memchr(p+o,0,n-o));
    require(z!=nullptr,why); return std::string(reinterpret_cast<const char*>(p+o),size_t(z-p-o));
}
std::vector<SignatureElement> signature(const uint8_t* p,size_t n) {
    // DxilContainer.h: DxilProgramSignature::ParamCount/ParamOffset;
    // ISG1/OSG1 use the fixed 32-byte DxilProgramSignatureElement, NOT a stride word.
    Reader r{p,n}; const auto count=r.u("signature header truncated");
    require(r.u("signature header truncated")==8,"unsupported signature ParamOffset/element representation (ISG1/OSG1 fixed 32)");
    const auto* elements=r.take(count,32,"signature element count/size overflow");
    const size_t strings=r.pos;
    std::vector<SignatureElement> out;
    for(size_t i=0;i<count;++i) {
        const auto* e=elements+i*32;
        // DxilProgramSignatureElement: Stream, SemanticName, SemanticIndex,
        // SystemValue, CompType, Register, Mask, AlwaysReads/NeverWrites_Mask, Pad, MinPrecision.
        SignatureElement v; v.stream=word(e); const auto name=word(e+4);
        require(name>=strings,"signature string overlaps elements");
        v.semantic=string_at(p,n,name,"signature semantic string offset/termination");
        v.index=word(e+8);v.system_value=word(e+12);v.component_type=word(e+16);v.reg=word(e+20);
        v.mask=e[24];v.read_write_mask=e[25];v.min_precision=word(e+28);
        require(!e[26]&&!e[27],"signature nonzero Pad");
        require(v.stream<=3 && (out.empty()||v.stream>=out.back().stream),"signature stream order/range");
        require(v.mask<=15 && v.read_write_mask<=15,"signature mask range");
        out.push_back(std::move(v));
    }
    return out;
}
Psv psv(const uint8_t* p,size_t n,ShaderStage expected) {
    // DxilPipelineStateValidation.h, PSVRuntimeInfo0/1/2/3 and ReadOrWrite layout.
    Reader r{p,n}; Psv v; v.runtime_size=r.u("PSV0 runtime size truncated");
    switch(v.runtime_size) {case 24:v.version=0;break;case 36:v.version=1;break;
    case 48:v.version=2;break;case 52:v.version=3;break;
    default:throw std::runtime_error("unsupported PSV0 runtime-info size "+std::to_string(v.runtime_size));}
    const auto* info=r.take(1,v.runtime_size,"PSV0 runtime-info truncated");
    std::copy_n(info,16,v.stage_info.begin());
    // PSVRuntimeInfo0 union members selected by the validated container stage.
    // VSInfo::OutputPositionPresent; PSInfo::DepthOutput/SampleFrequency;
    // GSInfo::InputPrimitive/OutputTopology/OutputStreamMask/OutputPositionPresent;
    // HSInfo::InputControlPointCount/OutputControlPointCount/TessellatorDomain/TessellatorOutputPrimitive;
    // DSInfo::InputControlPointCount/OutputPositionPresent/TessellatorDomain.
    auto& si=v.runtime_stage;
    switch(expected) {
    case ShaderStage::vertex:si.output_position=info[0];break;
    case ShaderStage::pixel:si.depth_output=info[0];si.sample_frequency=info[1];break;
    case ShaderStage::geometry:si.input_primitive=word(info);si.output_topology=word(info+4);
        si.output_stream_mask=word(info+8);si.output_position=info[12];break;
    case ShaderStage::hull:si.input_control_points=word(info);si.output_control_points=word(info+4);
        si.tessellator_domain=word(info+8);si.tessellator_output=word(info+12);break;
    case ShaderStage::domain:si.input_control_points=word(info);si.output_position=info[4];si.tessellator_domain=word(info+8);break;
    default:throw std::runtime_error("unsupported PSV0 runtime union stage");
    }
    v.minimum_wave=word(info+16);v.maximum_wave=word(info+20);
    if(v.version) {
        // PSVRuntimeInfo1::ShaderStage, UsesViewID, MaxVertexCount/SigPatchConstOrPrimVectors,
        // SigInputElements/SigOutputElements/SigPatchConstOrPrimElements, SigInputVectors/SigOutputVectors.
        v.shader_stage=info[24];v.uses_view_id=info[25];
        require(info[24]==uint8_t(expected),"PSV0 shader stage mismatch");
        require(info[25]<=1,"PSV0 UsesViewID range");
        v.max_vertices_or_patch_vectors=uint16_t(info[26])|(uint16_t(info[27])<<8);
        v.input_vectors=info[31];std::copy_n(info+32,4,v.output_vectors.begin());
        if(expected!=ShaderStage::geometry)
            require(!info[33]&&!info[34]&&!info[35],"PSV0 non-GS trailing output stream tables unsupported");
    }
    if(v.version>=2) { // PSVRuntimeInfo2::NumThreadsX/Y/Z
        v.threads.emplace();for(size_t i=0;i<3;++i) (*v.threads)[i]=word(info+36+4*i);
    }
    const auto count=r.u("PSV0 resource count truncated");
    if(count) {
        v.binding_stride=r.u("PSV0 binding stride truncated");
        if(v.binding_stride!=16&&v.binding_stride!=24)
            throw std::runtime_error("unsupported PSV0 resource binding stride "+std::to_string(v.binding_stride)+" (expected 16 or 24)");
        const auto* b=r.take(count,v.binding_stride,"PSV0 resource count/stride overflow");
        for(size_t i=0;i<count;++i) {
            const auto* e=b+i*v.binding_stride;
            // PSVResourceBindInfo0::ResType, Space, LowerBound, UpperBound;
            // PSVResourceBindInfo1::ResKind, ResFlags (present ONLY at stride 24).
            ResourceBinding a; a.type=word(e);a.space=word(e+4);a.lower=word(e+8);a.upper=word(e+12);
            require(a.type>0&&a.type<10&&a.lower<=a.upper,"PSV0 resource type/bounds");
            if(v.binding_stride==24) {a.kind=word(e+16);a.flags=word(e+20);
                require(*a.kind<19&&*a.flags<=1,"PSV0 resource kind/flags unsupported");}
            v.bindings.push_back(a);
        }
    }
    if(v.version) {
        // PSVStringTable::Size/Table, PSVSemanticIndexTable::Entries/Table.
        const auto sn=r.u("PSV0 string table size truncated");
        require(sn%4==0,"PSV0 string table alignment");
        const auto* strings=r.take(sn,1,"PSV0 string table truncated");
        require(!sn||strings[sn-1]==0,"PSV0 string table missing final null");
        v.string_table.assign(strings,strings+sn);
        if(v.version==3) // PSVRuntimeInfo3::EntryFunctionName into PSVStringTable
            v.entry=string_at(strings,sn,word(info+48),"PSV0 entry string offset/termination");
        const auto in=r.u("PSV0 semantic index count truncated");
        const auto* indices=r.take(in,4,"PSV0 semantic index table overflow");
        for(size_t i=0;i<in;++i)v.semantic_indices.push_back(word(indices+4*i));
        const size_t total=size_t(info[28])+info[29]+info[30];
        if(total) {
            const auto stride=r.u("PSV0 signature stride truncated");
            if(stride!=16)throw std::runtime_error("unsupported PSV0 signature element stride "+std::to_string(stride)+" (expected 16)");
            for(size_t group=0;group<3;++group) for(size_t i=0;i<info[28+group];++i) {
                const auto* e=r.take(1,16,"PSV0 signature element table truncated");
                // PSVSignatureElement0::SemanticName/SemanticIndexes, Rows, StartRow,
                // ColsAndStart, SemanticKind, ComponentType, InterpolationMode,
                // DynamicMaskAndStream, Reserved. SemanticIndexes counts Rows dwords.
                PsvElement a; a.semantic=string_at(strings,sn,word(e),"PSV0 signature semantic offset/termination");
                a.rows=e[8];a.start_row=e[9];a.cols_and_start=e[10];a.semantic_kind=e[11];
                a.component_type=e[12];a.interpolation=e[13];a.dynamic_mask_and_stream=e[14];
                const size_t off=word(e+4);
                require(off<=in&&a.rows<=in-off,"PSV0 signature semantic index bounds");
                require(!e[15] && !(e[10]&0x80) && !(e[14]&0xc0),"PSV0 signature reserved bits");
                require(a.rows>0&&a.rows<=32&&(e[10]&15)>0&&(e[10]&15)<=4&&a.semantic_kind<31&&a.component_type<=9&&a.interpolation<8,"PSV0 signature field range");
                if(e[10]&0x40) {
                    const size_t stream=(e[14]>>4)&3;
                    const size_t vectors=group==0?v.input_vectors:group==1?v.output_vectors[stream]:info[26];
                    require(size_t(a.start_row)+a.rows<=vectors&&((e[10]>>4)&3)+(e[10]&15)<=4,"PSV0 allocated signature packing bounds");
                }
                for(size_t j=0;j<a.rows;++j) a.indices.push_back(word(indices+4*(off+j)));
                v.signatures[group].push_back(std::move(a));
            }
        }
        // ReadOrWrite trailing dependency tables; sizes per PSVComputeMaskDwordsFromVectors
        // and PSVComputeInputOutputTableDwords. Literal words only, no usage/proof derivation.
        auto table=[&](std::string name,size_t words) {
            if(!words)return;
            const auto* e=r.take(words,4,name.c_str()); DependencyTable t; t.name=std::move(name);
            for(size_t i=0;i<words;++i)t.words.push_back(word(e+4*i));v.dependencies.push_back(std::move(t));
        };
        auto masks=[](size_t vectors){return (vectors+7)/8;};
        const size_t streams=expected==ShaderStage::geometry?4:1;
        const auto patch=info[26];
        if(*v.uses_view_id) {
            for(size_t s=0;s<streams;++s)table("PSV0 ViewID output mask stream "+std::to_string(s),masks(v.output_vectors[s]));
            if(expected==ShaderStage::hull)table("PSV0 ViewID patch output mask",masks(patch));
        }
        for(size_t s=0;s<streams;++s)table("PSV0 input-to-output table stream "+std::to_string(s),masks(v.output_vectors[s])*v.input_vectors*4);
        if(expected==ShaderStage::hull)table("PSV0 input-to-patch table",masks(patch)*v.input_vectors*4);
        if(expected==ShaderStage::domain)table("PSV0 patch-to-output table",masks(v.output_vectors[0])*patch*4);
    }
    require(r.pos==n,"PSV0 unexpected trailing bytes/table"); return v;
}
void decode(Stage& s,ShaderStage stage,const StageInput& input) {
    require(input.bytes&&input.size&&input.size<=64*1024,"present stage invalid input/64 KiB budget");
    const auto* original=static_cast<const uint8_t*>(input.bytes);
    s.container.assign(original,original+input.size);
    const auto* p=s.container.data(); const size_t n=s.container.size();
    s.shader=metadata::decode_shader_metadata(p,n);
    require(s.shader.valid,s.shader.rejection.c_str());
    require(s.shader.stage==stage,"container shader stage mismatch");
    // Public shader_metadata.h exposes no validated part spans. Its validator is
    // reused above; this bounded walker exposes only the requested records.
    // DxilContainer.h: DxilContainerHeader::PartCount, offset table;
    // DxilPartHeader::PartFourCC/PartSize. Validator already checked alignment,
    // offsets, sizes and overlap. Repeat bounds here to keep the seam explicit.
    const size_t count=word(p+28),end=32+count*4;
    for(size_t i=0;i<count;++i) {
        const size_t o=word(p+32+4*i);
        require(o%4==0&&o>=end&&o<=n&&n-o>=8,"inventory part offset");
        const size_t bytes=word(p+o+4);require(bytes<=n-o-8,"inventory part size");
        for(size_t j=0;j<i;++j)require(std::memcmp(p+o,p+word(p+32+4*j),4)!=0,"duplicate container part FourCC");
        const auto* data=p+o+8;
        if(!std::memcmp(p+o,"PSV0",4))s.psv=psv(data,bytes,stage);
        else if(!std::memcmp(p+o,"ISG1",4))s.input=signature(data,bytes);
        else if(!std::memcmp(p+o,"OSG1",4))s.output=signature(data,bytes);
        else if(!std::memcmp(p+o,"RTS0",4)) {
            s.rts0=std::vector<uint8_t>(data,data+bytes);
            s.root=metadata::RootBindingMetadata::from_serialized(p,n); // FULL original container, never bare RTS0
            if(s.root->failure==metadata::SerializedRootFailure::allocation_failure)throw std::bad_alloc();
            require(s.root->failure!=metadata::SerializedRootFailure::exception_failure,"root decoder exception");
        }
    }
}
}
const Stage& Inventory::stage(ShaderStage s) const {return stages[index(s)];}
Result build(const Request& q) noexcept {
    Result r;
    try {
        require(q.stages.size()<=5,"stage count budget");
        require(q.input_layout.size()<=32,"input layout count budget");
        auto out=std::make_shared<Inventory>(); std::array<bool,5> seen{};
        for(const auto& in:q.stages) {
            const auto i=index(in.stage);require(!seen[i],"duplicate stage assertion");seen[i]=true;
            auto& s=out->stages[i];s.state=in.state;
            if(in.state==StageState::present)decode(s,in.stage,in);
            else {require(in.state==StageState::unknown||in.state==StageState::explicitly_absent,"invalid stage state");
                require(!in.bytes&&!in.size,"non-present stage carries bytes");}
        }
        for(const auto& e:q.input_layout) {
            require(!e.semantic.empty()&&e.semantic.size()<=255&&e.semantic.find('\0')==std::string::npos,"input semantic budget/termination");
            // Known DXGI enum values only; retain the literal format without
            // claiming vertex-fetch compatibility or computing element spans.
            const bool format=(e.format>=1&&e.format<=115)||(e.format>=130&&e.format<=132)||(e.format>=189&&e.format<=191);
            require(e.slot<32&&format,"input slot/format range");
            require(e.classification<=1&&(!e.classification?e.step_rate==0:true),"input classification/step rate");
            require(e.offset==0xffffffffu||e.offset<=2048,"input offset budget");
            out->input_layout.push_back(e);
        }
        require(q.root_provenance==RootProvenance::unknown||q.root_provenance==RootProvenance::explicit_root||q.root_provenance==RootProvenance::embedded_container,"invalid root provenance");
        if(q.root_provenance==RootProvenance::explicit_root)require(q.explicit_root_identity!=0,"explicit root missing identity");
        else require(!q.explicit_root_identity,"non-explicit root identity");
        if(q.root_provenance==RootProvenance::embedded_container)
            require(out->stage(q.embedded_root_stage).rts0.has_value(),"embedded root source missing RTS0");
        out->root_provenance=q.root_provenance;out->explicit_root_identity=q.explicit_root_identity;
        out->embedded_root_stage=q.embedded_root_stage;
        r.inventory=std::move(out);
    } catch(const std::bad_alloc&) {r.failure=Failure::allocation;}
      catch(const std::exception& e) {r.failure=Failure::malformed;try{r.rejection=e.what();}catch(...){}}
      catch(...) {r.failure=Failure::exception;}
    return r;
}
}
