#include "root_binding_metadata.h"
#include <utility>
#include <d3d12.h>
#include <memory>
#include <new>
#include <type_traits>
#include <cstring>

namespace acz::metadata {
namespace {
bool valid_kind(BindingKind k) { return uint32_t(k)<=3; }
bool visible(Visibility v, ShaderStage s) {
    if (v==Visibility::all) return true;
    switch(s) {
    case ShaderStage::vertex: return v==Visibility::vertex;
    case ShaderStage::hull: return v==Visibility::hull;
    case ShaderStage::domain: return v==Visibility::domain;
    case ShaderStage::geometry: return v==Visibility::geometry;
    case ShaderStage::pixel: return v==Visibility::pixel;
    default: return false;
    }
}
BindingKind root_kind(ParameterKind k) {
    return k==ParameterKind::srv ? BindingKind::srv : k==ParameterKind::uav ? BindingKind::uav : BindingKind::cbv;
}
bool data_flags(uint32_t f) { const auto d=f&14; return d==0 || d==2 || d==4 || d==8; }
bool overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m) { return a<b+m && b<a+n; }
uint32_t word(const uint8_t* b,size_t offset) noexcept {
    return uint32_t(b[offset]) | (uint32_t(b[offset+1])<<8) |
        (uint32_t(b[offset+2])<<16) | (uint32_t(b[offset+3])<<24);
}
bool extent(size_t length,size_t offset,size_t count,size_t stride) noexcept {
    // Subtraction/division avoid offset+count*stride overflow even on x86.
    return offset<=length && count<=(length-offset)/stride;
}
struct HeaderCheck { SerializedRootFailure failure=SerializedRootFailure::none; const char* detail=nullptr; };
HeaderCheck check_header(const uint8_t* b,size_t length) noexcept {
    using F=SerializedRootFailure;
    // SDK 10.0.26100 d3d12.h:4446 / d3d12.idl:3002 declare a byte-bounded
    // input, but do not document its on-wire fields. The installed serializer's
    // observed layout is verified by header_layout_tests in the offline fixture:
    // DXBC: fixed 32 bytes; size/count at 24/28, chunk offsets from 32;
    // each chunk has fourcc/size in 8 bytes. RTS0: fixed 24 bytes;
    // version, parameter count/offset, sampler count/offset, flags at 0..20.
    // 1.0/1.1 parameter headers occupy 12 bytes, static samplers 52 bytes.
    // No parameter bodies, descriptor ranges, or bindings are decoded here.
    if(length<32) return {F::truncated_header,"serialized container header is truncated"};
    if(word(b,0)!=0x43425844u) return {}; // Leave other representations to SDK.
    const size_t declared=word(b,24), chunks=word(b,28);
    if(declared<32 || declared>length || !extent(declared,32,chunks,4))
        return {F::header_bounds,"serialized container header extent exceeds input"};
    const size_t directory_end=32+chunks*4; // extent checked above.
    for(size_t i=0;i<chunks;++i) {
        const size_t offset=word(b,32+i*4);
        if(offset<directory_end || !extent(declared,offset,1,8))
            return {F::header_bounds,"serialized chunk header extent exceeds input"};
        const size_t payload_size=word(b,offset+4), payload=offset+8;
        if(!extent(declared,payload,payload_size,1))
            return {F::header_bounds,"serialized chunk payload extent exceeds input"};
        if(word(b,offset)!=0x30535452u) continue;
        if(payload_size<24) return {F::truncated_header,"serialized root header is truncated"};
        const auto* root=b+payload; const auto version=word(root,0);
        if(version!=1 && version!=2) continue; // SDK owns unsupported versions.
        const size_t parameters=word(root,4), parameter_offset=word(root,8);
        const size_t samplers=word(root,12), sampler_offset=word(root,16);
        if((parameters && parameter_offset<24) || !extent(payload_size,parameter_offset,parameters,12))
            return {F::header_bounds,"serialized parameter header extent exceeds input"};
        if((samplers && sampler_offset<24) || !extent(payload_size,sampler_offset,samplers,52))
            return {F::header_bounds,"serialized static sampler extent exceeds input"};
    }
    return {};
}
}
SerializedRootResult RootBindingMetadata::from_serialized(const void* bytes,size_t length,bool diagnostic_static_s0) noexcept {
    using F=SerializedRootFailure;
    auto bad=[](F failure,int32_t hr=0) noexcept {
        SerializedRootResult out; out.failure=failure; out.sdk_hresult=hr; return out;
    };
    if(!bytes) return bad(F::null_input);
    if(!length) return bad(F::empty_input);
    if(length>max_serialized_bytes) return bad(F::oversized_input);
    try {
        const auto header=check_header(static_cast<const uint8_t*>(bytes),length);
        if(header.failure!=F::none) {
            auto out=bad(header.failure); out.detail=header.detail; return out;
        }
        struct Release { void operator()(ID3D12VersionedRootSignatureDeserializer* p) const noexcept { if(p) p->Release(); } };
        ID3D12VersionedRootSignatureDeserializer* raw=nullptr;
        const auto hr=D3D12CreateVersionedRootSignatureDeserializer(bytes,length,IID_PPV_ARGS(&raw));
        std::unique_ptr<ID3D12VersionedRootSignatureDeserializer,Release> owned(raw);
        if(FAILED(hr)) return bad(hr==E_OUTOFMEMORY ? F::allocation_failure : F::sdk_rejected,hr);
        const auto* desc=raw ? raw->GetUnconvertedRootSignatureDesc() : nullptr;
        if(!desc) return bad(F::missing_description);
        RootLayout layout;
        std::optional<D3D12_STATIC_SAMPLER_DESC> static_s0;
        auto append=[&](const auto& d,RootVersion version) {
            layout.version=version; layout.flags=uint32_t(d.Flags); layout.static_sampler_count=d.NumStaticSamplers;
            if(diagnostic_static_s0&&d.NumStaticSamplers) {
                // Deliberately narrow. Preserve refusal for every other static
                // layout; no caller adapter can certify serialized sampler facts.
                // Any other static layout is left untouched (static_sampler_count stays
                // nonzero) so from_adapter reports the exact legacy refusal
                // "static samplers unsupported by adapter" under the opt-in too.
                if(d.NumStaticSamplers==1&&d.pStaticSamplers) {
                    const auto& sampler=d.pStaticSamplers[0];
                    const bool supported=!sampler.ShaderRegister&&!sampler.RegisterSpace&&
                        (sampler.ShaderVisibility==D3D12_SHADER_VISIBILITY_PIXEL||sampler.ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL)&&
                        !(uint32_t(d.Flags)&32u)&&uint32_t(sampler.BorderColor)<=2;
                    if(supported) {static_s0=sampler;layout.static_sampler_count=0;}
                }
            }
            if(d.NumParameters>64) return F::parameter_budget;
            if(d.NumParameters && !d.pParameters) return F::missing_parameters;
            for(UINT i=0;i<d.NumParameters;++i) {
                const auto& p=d.pParameters[i]; RootParameter q;
                q.kind=static_cast<ParameterKind>(p.ParameterType); q.visibility=static_cast<Visibility>(p.ShaderVisibility);
                if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    if(p.DescriptorTable.NumDescriptorRanges>64) return F::range_budget;
                    if(p.DescriptorTable.NumDescriptorRanges && !p.DescriptorTable.pDescriptorRanges) return F::missing_ranges;
                    for(UINT j=0;j<p.DescriptorTable.NumDescriptorRanges;++j) {
                        const auto& r=p.DescriptorTable.pDescriptorRanges[j]; DescriptorRange v;
                        v.kind=static_cast<BindingKind>(r.RangeType); v.count=r.NumDescriptors;
                        v.base_register=r.BaseShaderRegister; v.space=r.RegisterSpace; v.offset=r.OffsetInDescriptorsFromTableStart;
                        if constexpr(std::is_same_v<std::decay_t<decltype(r)>,D3D12_DESCRIPTOR_RANGE1>) v.flags=uint32_t(r.Flags);
                        q.ranges.push_back(v);
                    }
                } else if(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                    q.shader_register=p.Constants.ShaderRegister; q.space=p.Constants.RegisterSpace; q.constant_count=p.Constants.Num32BitValues;
                } else {
                    q.shader_register=p.Descriptor.ShaderRegister; q.space=p.Descriptor.RegisterSpace;
                    if constexpr(std::is_same_v<std::decay_t<decltype(p)>,D3D12_ROOT_PARAMETER1>) q.flags=uint32_t(p.Descriptor.Flags);
                }
                layout.parameters.push_back(std::move(q));
            }
            return F::none;
        };
        F failure=F::unsupported_version;
        if(desc->Version==D3D_ROOT_SIGNATURE_VERSION_1_0) failure=append(desc->Desc_1_0,RootVersion::v1_0);
        if(desc->Version==D3D_ROOT_SIGNATURE_VERSION_1_1) failure=append(desc->Desc_1_1,RootVersion::v1_1);
        if(failure!=F::none) return bad(failure);
        auto metadata=from_adapter(layout);
        if(!metadata.valid()) {
            auto out=bad(F::unsupported_layout); out.detail=metadata.rejection(); return out;
        }
        if(static_s0) {
            // Reject overlap with any PS-visible sampler table (even a range
            // starting above s0); only this bounded single-static layout is supported.
            for(const auto& parameter:layout.parameters)if(parameter.visibility==Visibility::all||parameter.visibility==Visibility::pixel)
                for(const auto& range:parameter.ranges)if(range.kind==BindingKind::sampler) {
                    // Same exact legacy detail as any other unsupported static layout.
                    auto out=bad(F::unsupported_layout);out.detail="static samplers unsupported by adapter";return out;
                }
            const auto& d=*static_s0;
            auto bits=[](float f){uint32_t w;std::memcpy(&w,&f,sizeof(w));return w;};
            const auto color=d.BorderColor;
            const uint32_t rgb=color==D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE?0x3f800000u:0u;
            const uint32_t alpha=color==D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK?0u:0x3f800000u;
            metadata.static_s0_words_={uint32_t(d.Filter),uint32_t(d.AddressU),uint32_t(d.AddressV),uint32_t(d.AddressW),
                bits(d.MipLODBias),d.MaxAnisotropy,uint32_t(d.ComparisonFunc),rgb,rgb,rgb,alpha,bits(d.MinLOD),bits(d.MaxLOD)};
            metadata.serialized_s0_=true;metadata.layout_.static_sampler_count=1;
        }
        SerializedRootResult out; out.metadata.emplace(std::move(metadata)); return out;
    } catch(const std::bad_alloc&) { return bad(F::allocation_failure); }
      catch(...) { return bad(F::exception_failure); }
}
RootBindingMetadata RootBindingMetadata::from_adapter(const RootLayout& in) {
    RootBindingMetadata out;
    auto bad=[&](const char* reason) { out.rejection_=reason; return out; };
    if (in.version!=RootVersion::v1_0 && in.version!=RootVersion::v1_1) return bad("unsupported root signature version");
    // SDK 10.0.26100 d3d12.h:4076-4077: deny AS/MS access does not
    // alter classic-stage register/table resolution; AS/MS queries unsupported.
    if (in.flags&~0x37fu) return bad("unsupported root signature flags");
    if (in.static_sampler_count) return bad("static samplers unsupported by adapter");
    if (in.parameters.size()>64) return bad("root parameter count exceeds budget");
    struct Span { BindingKind kind; uint32_t reg,space,count; Visibility visibility; };
    std::vector<Span> spans;
    uint64_t cost=0;
    RootLayout normalized=in; // Deep copy: no caller/deserializer pointers survive.
    for (auto& p:normalized.parameters) {
        if (uint32_t(p.kind)>4 || uint32_t(p.visibility)>5) return bad("unsupported parameter kind/visibility");
        if (p.kind==ParameterKind::table) {
            if (p.flags || p.constant_count || p.shader_register || p.space || p.ranges.empty()) return bad("invalid table adapter fields");
            if (p.ranges.size()>0xffffffffull) return bad("range count exceeds index representation");
            ++cost;
            uint64_t next=0;
            bool sampler=p.ranges.front().kind==BindingKind::sampler;
            std::vector<DescriptorRange> prior;
            for (auto& r:p.ranges) {
                if (!valid_kind(r.kind) || (r.kind==BindingKind::sampler)!=sampler) return bad("invalid/mixed table heap kinds");
                if (!r.count || r.count==0xffffffffu) return bad("zero/unbounded descriptor range unsupported");
                if ((r.flags&~0x1000fu) || !data_flags(r.flags) || ((r.flags&1) && (r.flags&0x10000)) ||
                    (sampler && (r.flags&~1u)) || (in.version==RootVersion::v1_0 && r.flags)) return bad("unsupported/conflicting range flags");
                uint64_t offset=r.offset==append_range ? next : r.offset;
                if (offset+r.count>0xffffffffull || uint64_t(r.base_register)+r.count>0x100000000ull) return bad("range arithmetic overflow");
                r.offset=uint32_t(offset); next=offset+r.count;
                for (const auto& q:prior)
                    if (overlap(r.offset,r.count,q.offset,q.count)) return bad("overlapping descriptor table slots");
                prior.push_back(r);
                spans.push_back({r.kind,r.base_register,r.space,r.count,p.visibility});
            }
        } else {
            if (!p.ranges.empty()) return bad("non-table has ranges");
            if (p.kind==ParameterKind::constants) {
                if (p.flags || !p.constant_count) return bad("invalid constants metadata");
                cost+=p.constant_count;
            } else {
                if (p.constant_count || (p.flags&~14u) || !data_flags(p.flags) || (in.version==RootVersion::v1_0 && p.flags)) return bad("invalid root descriptor flags/fields");
                cost+=2;
            }
            spans.push_back({root_kind(p.kind),p.shader_register,p.space,1,p.visibility});
        }
        if (cost>64) return bad("root signature exceeds 64 DWORD budget");
    }
    for (size_t i=0;i<spans.size();++i) for (size_t j=0;j<i;++j) {
        const auto& a=spans[i]; const auto& b=spans[j];
        if (a.kind==b.kind && a.space==b.space &&
            (a.visibility==Visibility::all || b.visibility==Visibility::all || a.visibility==b.visibility) &&
            overlap(a.reg,a.count,b.reg,b.count)) return bad("ambiguous overlapping register bindings");
    }
    out.layout_=std::move(normalized); out.rejection_.clear(); return out;
}
BindingResolution RootBindingMetadata::resolve(BindingKind k,uint32_t reg,uint32_t space,ShaderStage stage) const {
    BindingResolution out;
    if (!valid()) { out.rejection=rejection_; return out; }
    if (!valid_kind(k) || uint32_t(stage)>5) { out.rejection="unsupported binding query"; return out; }
    uint32_t deny=0;
    switch(stage) {
    case ShaderStage::vertex: deny=2; break;
    case ShaderStage::hull: deny=4; break;
    case ShaderStage::domain: deny=8; break;
    case ShaderStage::geometry: deny=16; break;
    case ShaderStage::pixel: deny=32; break;
    default: break;
    }
    if (layout_.flags&deny) { out.rejection="stage denied root access"; return out; }
    for (size_t i=0;i<layout_.parameters.size();++i) {
        const auto& p=layout_.parameters[i];
        if (!visible(p.visibility,stage)) continue;
        if (p.kind==ParameterKind::table) {
            for (size_t j=0;j<p.ranges.size();++j) {
                const auto& r=p.ranges[j];
                if (r.kind==k && r.space==space && reg>=r.base_register && uint64_t(reg)-r.base_register<r.count) {
                    if (out.valid) { out.valid=false; out.rejection="ambiguous query"; return out; }
                    out.valid=true; out.parameter_kind=p.kind; out.root_index=uint32_t(i); out.range_index=uint32_t(j);
                    out.table_offset=r.offset+(reg-r.base_register);
                }
            }
        } else if (root_kind(p.kind)==k && p.shader_register==reg && p.space==space) {
            if (out.valid) { out.valid=false; out.rejection="ambiguous query"; return out; }
            out.valid=true; out.parameter_kind=p.kind; out.root_index=uint32_t(i);
        }
    }
    if (!out.valid) out.rejection="binding not found for register/space/stage";
    return out;
}
} // namespace acz::metadata
