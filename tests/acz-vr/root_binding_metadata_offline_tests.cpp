#include "root_binding_metadata.h"
#include <cstdlib>
#include <iostream>
#include <d3d12.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <new>
#include <memory>
#include <type_traits>
namespace allocation_probe { long remaining=-1; }
void* operator new(size_t n) {
    if(allocation_probe::remaining==0) throw std::bad_alloc();
    if(allocation_probe::remaining>0) --allocation_probe::remaining;
    if(auto p=std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
void operator delete[](void* p,size_t) noexcept { std::free(p); }
using namespace acz::metadata;
namespace {
// e10-drawcpt-pso.json SHA-256 17f9a48e3d57be77a6b3bcac350573deaffddd2c2daf7388984f4f27f1e41b55.
// VS adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd: RTS0 payload [712,872).
// Payload SHA-256 e710e76d70a18fdb9147b303a7d8b4cf83dc3d80b70069149edf0d81a70b86c5.
// Same embedded bytes as the original fixture; no evidence-file read.
constexpr uint8_t real[]={
        0x02,0,0,0,0x04,0,0,0,0x18,0,0,0,0,0,0,0,0xa0,0,0,0,0x0d,0x03,0,0,
        0,0,0,0,0x05,0,0,0,0x48,0,0,0,0,0,0,0,0x05,0,0,0,0x68,0,0,0,
        0x03,0,0,0,0,0,0,0,0x88,0,0,0,0x01,0,0,0,0,0,0,0,0x94,0,0,0,
        0x01,0,0,0,0x50,0,0,0,0,0,0,0,0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0xff,0xff,0xff,0xff,0x01,0,0,0,0x70,0,0,0,0x03,0,0,0,0x01,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0x0a,0,0,0,0,0,0,0,
        0,0,0,0,0x0a,0,0,0,0,0,0,0,0x02,0,0,0};
int checks=0;
void check(bool b,const char* label) { ++checks; if(!b) { std::cerr<<"FAIL: "<<label<<'\n'; std::exit(1); } }
// Synthetic adapter fixtures; the separately pinned e10 bytes are below.
RootLayout fixture() {
    RootLayout l; l.version=RootVersion::v1_1;
    RootParameter constants; constants.kind=ParameterKind::constants; constants.constant_count=4;
    RootParameter table; table.visibility=Visibility::geometry;
    table.ranges={{BindingKind::cbv,2,0,0,0,5},{BindingKind::srv,4,8,0,0,append_range}};
    l.parameters={constants,table}; return l;
}
void rejected(const RootLayout& l,const char* label) {
    const auto m=RootBindingMetadata::from_adapter(l);
    check(!m.valid() && !m.rejection().empty() && !m.resolve_srt_data().valid,label);
}
// Small self-contained SHA-256, also checked against the standard "abc" vector.
std::string sha256(const uint8_t* data,size_t n) {
    constexpr uint32_t k[]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto r=[](uint32_t x,unsigned s) { return (x>>s)|(x<<(32-s)); };
    std::vector<uint8_t> padded(data,data+n); padded.push_back(0x80);
    while(padded.size()%64!=56) padded.push_back(0);
    for(int i=7;i>=0;--i) padded.push_back(uint8_t((uint64_t(n)*8)>>(i*8)));
    for(size_t off=0;off<padded.size();off+=64) {
        uint32_t w[64];
        for(unsigned i=0;i<16;++i) { w[i]=0; for(unsigned j=0;j<4;++j) w[i]=(w[i]<<8)|padded[off+i*4+j]; }
        for(unsigned i=16;i<64;++i) w[i]=w[i-16]+(r(w[i-15],7)^r(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(r(w[i-2],17)^r(w[i-2],19)^(w[i-2]>>10));
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
        for(unsigned i=0;i<64;++i) {
            auto t1=v+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            auto t2=(r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c));
            v=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
    }
    std::string out; const char* hex="0123456789abcdef";
    for(auto v:h) for(int i=7;i>=0;--i) out.push_back(hex[(v>>(i*4))&15]);
    return out;
}
template<class T> struct Release { void operator()(T* p) const { if(p) p->Release(); } };
template<class P,class R> void fill(const RootLayout& l,std::vector<P>& p,std::vector<std::vector<R>>& ranges) {
    p.resize(l.parameters.size()); ranges.resize(p.size());
    for(size_t i=0;i<p.size();++i) {
        const auto& q=l.parameters[i]; auto& d=p[i];
        d.ParameterType=D3D12_ROOT_PARAMETER_TYPE(q.kind); d.ShaderVisibility=D3D12_SHADER_VISIBILITY(q.visibility);
        if(q.kind==ParameterKind::table) {
            for(const auto& v:q.ranges) {
                R r{}; r.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE(v.kind); r.NumDescriptors=v.count;
                r.BaseShaderRegister=v.base_register; r.RegisterSpace=v.space; r.OffsetInDescriptorsFromTableStart=v.offset;
                if constexpr(std::is_same_v<R,D3D12_DESCRIPTOR_RANGE1>) r.Flags=D3D12_DESCRIPTOR_RANGE_FLAGS(v.flags);
                ranges[i].push_back(r);
            }
            d.DescriptorTable={UINT(ranges[i].size()),ranges[i].data()};
        } else if(q.kind==ParameterKind::constants) d.Constants={q.shader_register,q.space,q.constant_count};
        else {
            d.Descriptor.ShaderRegister=q.shader_register; d.Descriptor.RegisterSpace=q.space;
            if constexpr(std::is_same_v<P,D3D12_ROOT_PARAMETER1>) d.Descriptor.Flags=D3D12_ROOT_DESCRIPTOR_FLAGS(q.flags);
        }
    }
}
std::vector<uint8_t> serialize(const RootLayout& l,D3D_ROOT_SIGNATURE_VERSION version) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{}; d.Version=version;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(l.static_sampler_count);
    for(size_t i=0;i<samplers.size();++i) {
        auto& s=samplers[i]; s.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxAnisotropy=1; s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        s.MaxLOD=D3D12_FLOAT32_MAX; s.ShaderRegister=UINT(i); s.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    }
    std::vector<D3D12_ROOT_PARAMETER> p0; std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> r0;
    std::vector<D3D12_ROOT_PARAMETER1> p1; std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> r1;
    if(version==D3D_ROOT_SIGNATURE_VERSION_1_0) {
        fill(l,p0,r0); d.Desc_1_0={UINT(p0.size()),p0.data(),UINT(samplers.size()),samplers.data(),D3D12_ROOT_SIGNATURE_FLAGS(l.flags)};
    } else {
        fill(l,p1,r1); d.Desc_1_1={UINT(p1.size()),p1.data(),UINT(samplers.size()),samplers.data(),D3D12_ROOT_SIGNATURE_FLAGS(l.flags)};
    }
    ID3DBlob* raw=nullptr; ID3DBlob* error=nullptr;
    auto hr=D3D12SerializeVersionedRootSignature(&d,&raw,&error);
    std::unique_ptr<ID3DBlob,Release<ID3DBlob>> owned(raw),errors(error);
    if(FAILED(hr) && error) std::cerr<<static_cast<const char*>(error->GetBufferPointer())<<'\n';
    check(SUCCEEDED(hr) && raw,"SDK serializer fixture");
    const auto* b=static_cast<const uint8_t*>(raw->GetBufferPointer()); return {b,b+raw->GetBufferSize()};
}
bool same(const RootLayout& a,const RootLayout& b) {
    if(a.version!=b.version || a.flags!=b.flags || a.static_sampler_count!=b.static_sampler_count || a.parameters.size()!=b.parameters.size()) return false;
    for(size_t i=0;i<a.parameters.size();++i) {
        const auto& p=a.parameters[i]; const auto& q=b.parameters[i];
        if(p.kind!=q.kind || p.visibility!=q.visibility || p.shader_register!=q.shader_register || p.space!=q.space || p.flags!=q.flags || p.constant_count!=q.constant_count || p.ranges.size()!=q.ranges.size()) return false;
        for(size_t j=0;j<p.ranges.size();++j) {
            const auto& r=p.ranges[j]; const auto& s=q.ranges[j];
            if(r.kind!=s.kind || r.count!=s.count || r.base_register!=s.base_register || r.space!=s.space || r.flags!=s.flags || r.offset!=s.offset) return false;
        }
    }
    return true;
}
uint32_t read_word(const uint8_t* b,size_t off) {
    uint32_t v=0; std::memcpy(&v,b+off,4); return v;
}
void write_word(std::vector<uint8_t>& b,size_t off,uint32_t v) { std::memcpy(b.data()+off,&v,4); }
class GuardedPrefix {
public:
    GuardedPrefix(const uint8_t* bytes,size_t n) {
        SYSTEM_INFO info{}; GetSystemInfo(&info); check(n<=info.dwPageSize,"prefix fits test page");
        pages_=VirtualAlloc(nullptr,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        check(pages_!=nullptr,"test pages allocated"); DWORD old=0;
        auto* end=static_cast<uint8_t*>(pages_)+info.dwPageSize;
        check(VirtualProtect(end,info.dwPageSize,PAGE_NOACCESS,&old)!=0,"test guard page");
        input=end-n; std::copy_n(bytes,n,input);
    }
    ~GuardedPrefix() { VirtualFree(pages_,0,MEM_RELEASE); }
    GuardedPrefix(const GuardedPrefix&)=delete;
    GuardedPrefix& operator=(const GuardedPrefix&)=delete;
    uint8_t* input=nullptr;
private:
    void* pages_=nullptr;
};
void prefix_tests(const uint8_t* bytes,size_t n,bool sdk_container) {
    using F=SerializedRootFailure;
    for(size_t length=0;length<=n;++length) {
        GuardedPrefix prefix(bytes,length);
        auto d=RootBindingMetadata::from_serialized(prefix.input,length);
        if(sdk_container && length==n) check(d.valid(),"complete SDK fixture unchanged");
        else {
            const F expected=length==0 ? F::empty_input : length<32 ? F::truncated_header :
                sdk_container ? F::header_bounds : F::sdk_rejected;
            check(!d.metadata && d.failure==expected,"every guarded prefix precise rejection");
            if(expected==F::truncated_header)
                check(d.detail=="serialized container header is truncated","short header detail");
            if(expected==F::header_bounds)
                check(d.detail=="serialized container header extent exceeds input","truncated declared size detail");
        }
    }
}
void header_layout_tests() {
    // Controlled SDK serialization verifies EVERY on-wire field/stride used by
    // the production header check; this is observed layout, not an SDK promise.
    using F=SerializedRootFailure;
    static_assert(int(F::exception_failure)==13 && int(F::truncated_header)==14 && int(F::header_bounds)==15);
    for(auto version:{RootVersion::v1_0,RootVersion::v1_1}) {
        for(unsigned count=0;count<=2;++count) for(unsigned sampler_count=0;sampler_count<=2;++sampler_count) {
            RootLayout l; l.version=version; l.flags=1; l.static_sampler_count=sampler_count;
            for(unsigned i=0;i<count;++i) {
                RootParameter p; p.kind=ParameterKind::constants; p.shader_register=i+7; p.constant_count=1;
                l.parameters.push_back(p);
            }
            auto bytes=serialize(l,D3D_ROOT_SIGNATURE_VERSION(version));
            check(read_word(bytes.data(),0)==0x43425844 && read_word(bytes.data(),24)==bytes.size() &&
                read_word(bytes.data(),28)==1 && read_word(bytes.data(),32)==36,"observed DXBC header and directory");
            check(read_word(bytes.data(),36)==0x30535452 && read_word(bytes.data(),40)==bytes.size()-44,"observed RTS0 chunk header");
            const auto* root=bytes.data()+44;
            check(read_word(root,0)==uint32_t(version) && read_word(root,4)==count && read_word(root,8)==24 &&
                read_word(root,12)==sampler_count && read_word(root,16)==24+24*count && read_word(root,20)==1,
                "observed root version/counts/offsets/flags");
            check(bytes.size()==44+24+24*count+52*sampler_count,"observed static sampler stride 52");
            for(unsigned i=0;i<count;++i)
                check(read_word(root,24+i*12)==1 && read_word(root,32+i*12)==24+12*count+12*i,
                    "observed parameter header stride 12");
            auto d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
            check(sampler_count ? d.failure==F::unsupported_layout && d.detail=="static samplers unsupported by adapter" : d.valid(),
                "header-only probes preserve valid adapter outcomes");
        }
    }
    auto layout=fixture(); layout.parameters[0].shader_register=7;
    auto bytes=serialize(layout,D3D_ROOT_SIGNATURE_VERSION_1_1);
    auto reject=[&](size_t off,uint32_t value,const char* detail) {
        auto copy=bytes; write_word(copy,off,value); GuardedPrefix input(copy.data(),copy.size());
        auto d=RootBindingMetadata::from_serialized(input.input,copy.size());
        check(!d.metadata && d.failure==F::header_bounds && d.detail==detail,"header overflow precise rejection");
    };
    for(uint32_t v:{0xffffffffu,0x80000000u}) {
        reject(24,v,"serialized container header extent exceeds input");
        reject(28,v,"serialized container header extent exceeds input");
        reject(32,v,"serialized chunk header extent exceeds input");
        reject(40,v,"serialized chunk payload extent exceeds input");
        reject(48,v,"serialized parameter header extent exceeds input");
        reject(52,v,"serialized parameter header extent exceeds input");
        reject(56,v,"serialized static sampler extent exceeds input");
        reject(60,v,"serialized static sampler extent exceeds input");
    }
    reject(52,uint32_t(bytes.size()-44-12),"serialized parameter header extent exceeds input");
    reject(52,0,"serialized parameter header extent exceeds input");
    auto copy=bytes; write_word(copy,56,1); write_word(copy,60,uint32_t(bytes.size()-44-51));
    auto d=RootBindingMetadata::from_serialized(copy.data(),copy.size());
    check(!d.metadata && d.failure==F::header_bounds && d.detail=="serialized static sampler extent exceeds input","sampler offset plus count beyond input");
    write_word(copy,60,0);
    d=RootBindingMetadata::from_serialized(copy.data(),copy.size());
    check(!d.metadata && d.failure==F::header_bounds && d.detail=="serialized static sampler extent exceeds input","sampler header overlap rejected");
    copy=bytes; copy.resize(44+23); write_word(copy,24,uint32_t(copy.size())); write_word(copy,40,23);
    GuardedPrefix truncated(copy.data(),copy.size());
    d=RootBindingMetadata::from_serialized(truncated.input,copy.size());
    check(!d.metadata && d.failure==F::truncated_header && d.detail=="serialized root header is truncated","root fixed header requires 24 bytes");
}
// Deterministic exhaustive byte flips, aligned boundary words, all prefixes
// and all 256 one-byte extensions. No PRNG. Input copies are freed before
// rechecking every successful owned layout. --trace flushes fault markers.
bool trace_mutations=false;
void mutation_tests(const std::vector<uint8_t>& original,const char* label) {
    using F=SerializedRootFailure;
    const auto pin=sha256(original.data(),original.size());
    size_t total=0,accepted=0;
    auto run=[&](std::vector<uint8_t> bytes,const char* kind,size_t offset,uint32_t value) {
        if(trace_mutations)std::cout<<label<<" "<<kind<<" offset="<<offset<<" value="<<value<<std::endl;
        ++total;
        auto d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
        check(d.failure!=F::exception_failure && d.failure!=F::allocation_failure,"mutation no exception/allocation failure");
        const auto snapshot=d.metadata;
        if(d.valid()) {
            ++accepted; check(d.detail.empty() && d.sdk_hresult==0 && d.metadata->valid(),"mutation complete root success");
            check(d.metadata->layout().parameters.size()<=64,"mutation parameter bound");
            for(const auto& p:d.metadata->layout().parameters)check(p.ranges.size()<=64,"mutation range bound");
            const auto normalized=RootBindingMetadata::from_adapter(d.metadata->layout());
            check(normalized.valid() && same(normalized.layout(),d.metadata->layout()),"accepted mutation internally consistent normalized layout");
        } else {
            check(!d.metadata && d.failure!=F::none,"mutation atomic rejection");
            if(d.failure==F::sdk_rejected)check(d.sdk_hresult==int32_t(E_INVALIDARG),"mutation exact SDK E_INVALIDARG");
            if(d.failure==F::header_bounds || d.failure==F::truncated_header || d.failure==F::unsupported_layout)
                check(!d.detail.empty(),"mutation precise header/adapter detail");
        }
        std::fill(bytes.begin(),bytes.end(),0);bytes.clear();bytes.shrink_to_fit();
        if(snapshot) {
            check(same(snapshot->layout(),d.metadata->layout()),"mutation root owned after input free");
            const auto a=snapshot->resolve_srt_data(),b=d.metadata->resolve_srt_data();
            check(a.valid==b.valid && a.root_index==b.root_index && a.range_index==b.range_index &&
                a.table_offset==b.table_offset && a.rejection==b.rejection,"mutation binding resolution owned after free");
        }
    };
    for(size_t i=0;i<original.size();++i)for(uint8_t mask:{uint8_t(1),uint8_t(255)}) {
        auto c=original;c[i]^=mask;run(std::move(c),"xor",i,mask);
    }
    // All aligned words includes parameter-body offsets, range counts and
    // range-body offsets observed through the installed SDK serializer.
    for(size_t i=0;i+4<=original.size();i+=4)
        for(uint32_t v:{0u,1u,uint32_t(original.size()),uint32_t(original.size()+1),0x7fffffffu,0xffffffffu,0xfffffffcu}) {
            auto c=original;write_word(c,i,v);run(std::move(c),"word",i,v);
        }
    for(size_t n=0;n<original.size();++n)run({original.begin(),original.begin()+n},"prefix",n,0);
    for(unsigned v=0;v<256;++v){auto c=original;c.push_back(uint8_t(v));run(std::move(c),"append",original.size(),v);}
    check(total==3*original.size()+7*(original.size()/4)+256,"exact root mutation count");
    check(sha256(original.data(),original.size())==pin,"root original pin unchanged");
    std::cout<<"J-043 "<<label<<" total="<<total<<" accepted="<<accepted<<" stride=4 boundary words; byte flips exhaustive\n";
}
void serialized_budgets() {
    using F=SerializedRootFailure;
    static_assert(RootBindingMetadata::max_serialized_bytes==65536);
    auto bytes=serialize(RootLayout{},D3D_ROOT_SIGNATURE_VERSION_1_1);
    bytes.resize(65536,0);write_word(bytes,24,65536);
    auto d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
    // Trailing zeros make this a negative SDK representation. Exactly max
    // must reach that SDK diagnostic rather than the byte-budget rejection.
    check(!d.metadata && d.failure==F::sdk_rejected && d.sdk_hresult==int32_t(E_INVALIDARG),
        "exact 64 KiB reaches precise SDK rejection");
    // Size guard must precede reads and any C++ allocation. Only 65536 bytes
    // are readable, deliberately fewer than the supplied rejected length.
    allocation_probe::remaining=0;
    d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size()+1);
    allocation_probe::remaining=-1;
    check(!d.metadata && d.failure==F::oversized_input && d.detail.empty(),"64 KiB+1 rejected before allocation/read");
    for(auto version:{D3D_ROOT_SIGNATURE_VERSION_1_0,D3D_ROOT_SIGNATURE_VERSION_1_1}) {
        RootLayout l;l.version=RootVersion(version);
        for(uint32_t i=0;i<64;++i) {
            RootParameter p;p.kind=ParameterKind::constants;p.shader_register=i;p.constant_count=1;
            l.parameters.push_back(p);
        }
        bytes=serialize(l,version);
        d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
        check(d.valid() && d.metadata->layout().parameters.size()==64,"exact 64 parameters accepted");
        // Serializer enforces the DWORD cost. Build a structurally complete
        // 65-parameter negative from the observed J-041 header/body layout:
        // insert one 12-byte header, shift old body offsets, append constants.
        const size_t root=44,headers=root+24,header_end=headers+64*12;
        check(read_word(bytes.data(),52)==24 && bytes.size()==root+24+64*24,"64 constants wire layout");
        bytes.insert(bytes.begin()+header_end,12,0);
        for(size_t i=0;i<64;++i)write_word(bytes,headers+12*i+8,read_word(bytes.data(),headers+12*i+8)+12);
        write_word(bytes,header_end,1);write_word(bytes,header_end+8,uint32_t(bytes.size()-root));
        const size_t end=bytes.size();bytes.resize(end+12,0);
        write_word(bytes,end,64);write_word(bytes,end+8,1);
        write_word(bytes,48,65);write_word(bytes,60,uint32_t(bytes.size()-root));
        write_word(bytes,24,uint32_t(bytes.size()));write_word(bytes,40,uint32_t(bytes.size()-root));
        allocation_probe::remaining=0;
        d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
        allocation_probe::remaining=-1;
        std::cout<<"65-parameter boundary version="<<unsigned(version)<<" failure="<<int(d.failure)<<" hr="<<d.sdk_hresult<<" detail="<<d.detail<<std::endl;
        // The installed SDK rejects this structurally complete over-DWORD
        // root first. This exercises max+1 rejection with allocation disabled,
        // but cannot establish coverage of our post-SDK parameter_budget arm.
        check(!d.metadata && d.failure==F::sdk_rejected && d.sdk_hresult==int32_t(E_INVALIDARG),
            "SDK rejects 65 parameters before C++ layout allocation");
        for(uint32_t n:{64u,65u}) {
            l.parameters.clear();RootParameter table;
            for(uint32_t i=0;i<n;++i)table.ranges.push_back({BindingKind::srv,1,i,0,0,i});
            l.parameters.push_back(table);bytes=serialize(l,version);
            if(n==64) {
                d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
                check(d.valid() && d.metadata->layout().parameters[0].ranges.size()==64,"exact 64 ranges accepted");
            } else {
                allocation_probe::remaining=0;
                d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
                allocation_probe::remaining=-1;
                check(!d.metadata && d.failure==F::range_budget,"65 ranges rejected before C++ range allocation");
            }
        }
    }
}
void serialized_tests() {
    using F=SerializedRootFailure;
    auto reject=[](const void* p,size_t n,F f) {
        auto d=RootBindingMetadata::from_serialized(p,n); check(!d.valid() && !d.metadata && d.failure==f,"precise decode rejection without publication");
    };
    uint8_t garbage[32]={};
    reject(nullptr,0,F::null_input); reject(nullptr,1,F::null_input);
    reject(garbage,0,F::empty_input); reject(garbage,RootBindingMetadata::max_serialized_bytes+1,F::oversized_input);
    reject(garbage,sizeof garbage,F::sdk_rejected);
    std::vector<uint8_t> non_dxbc(32,0);write_word(non_dxbc,24,32);
    reject(non_dxbc.data(),non_dxbc.size(),F::sdk_rejected);
    for(auto version:{RootVersion::v1_0,RootVersion::v1_1}) {
        auto l=fixture(); l.version=version; l.parameters[0].shader_register=7;
        RootParameter root; root.kind=ParameterKind::srv; root.visibility=Visibility::vertex; root.shader_register=3; root.space=2;
        if(version==RootVersion::v1_1) { root.flags=8; l.parameters[1].ranges[1].flags=8; }
        l.parameters.push_back(root);
        root.kind=ParameterKind::cbv; root.shader_register=9; l.parameters.push_back(root);
        root.kind=ParameterKind::uav; root.shader_register=5; l.parameters.push_back(root);
        RootParameter samplers; samplers.visibility=Visibility::pixel;
        samplers.ranges={{BindingKind::sampler,2,1,3,0,append_range}}; l.parameters.push_back(samplers);
        auto expected=RootBindingMetadata::from_adapter(l);
        auto bytes=serialize(l,D3D_ROOT_SIGNATURE_VERSION(version));
        auto d=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
        check(d.valid() && same(d.metadata->layout(),expected.layout()),"1.0/1.1 complete adapter round-trip");
        mutation_tests(bytes,version==RootVersion::v1_0?"SDK1.0":"SDK1.1");
        check(d.metadata->resolve_srt_data().table_offset==9,"serialized APPEND resolution");
        prefix_tests(bytes.data(),bytes.size(),true); // Includes exact 1.1 length 23.
        reject(bytes.data(),1,F::truncated_header); reject(bytes.data(),bytes.size()/2,F::header_bounds);
        reject(bytes.data(),bytes.size()-1,F::header_bounds);
        bool reached_success=false; unsigned failures=0;
        for(long budget=0;budget<200;++budget) {
            allocation_probe::remaining=budget;
            auto failed=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
            allocation_probe::remaining=-1;
            if(failed.valid()) { check(same(failed.metadata->layout(),expected.layout()),"sweep success complete"); reached_success=true; break; }
            check(!failed.metadata && failed.failure==F::allocation_failure && failed.detail.empty(),"allocation sweep atomic failure"); ++failures;
        }
        check(reached_success && failures>0,"allocation sweep reaches publication");
        std::fill(bytes.begin(),bytes.end(),0); check(same(d.metadata->layout(),expected.layout()),"mutated serialized input independence");
        bytes.clear(); bytes.shrink_to_fit(); check(same(d.metadata->layout(),expected.layout()),"freed serialized input independence");
    }
    auto future=serialize(RootLayout{},D3D_ROOT_SIGNATURE_VERSION_1_2);
    reject(future.data(),future.size(),F::unsupported_version);
    auto unsupported=fixture(); unsupported.parameters[0].shader_register=7; unsupported.flags=0x400;
    auto bytes=serialize(unsupported,D3D_ROOT_SIGNATURE_VERSION_1_1);
    auto bad=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
    check(!bad.metadata && bad.failure==F::unsupported_layout && bad.detail=="unsupported root signature flags","precise unsupported layout");
    bool rejected_success=false;
    for(long budget=0;budget<200;++budget) {
        allocation_probe::remaining=budget;
        auto failure=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
        allocation_probe::remaining=-1;
        check(!failure.metadata,"rejection allocation sweep never publishes");
        if(failure.failure==F::unsupported_layout) {
            check(failure.detail=="unsupported root signature flags","complete rejection detail after sweep");
            rejected_success=true; break;
        }
        check(failure.failure==F::allocation_failure && failure.detail.empty(),"rejection detail allocation failure atomic");
    }
    check(rejected_success,"rejection sweep finishes");
    static_assert(sizeof real==160);
    check(sha256(reinterpret_cast<const uint8_t*>("abc"),3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 known vector");
    check(sha256(real,sizeof real)=="e710e76d70a18fdb9147b303a7d8b4cf83dc3d80b70069149edf0d81a70b86c5","e10 payload hash");
    prefix_tests(real,sizeof real,false);
    mutation_tests({std::begin(real),std::end(real)},"e10-bare-RTS0");
    auto decoded=RootBindingMetadata::from_serialized(real,sizeof real);
    // The installed SDK rejects the bare part payload before giving any desc.
    // Its literal layout cannot be asserted without a different supported SDK
    // input representation; do not infer it by parsing the opaque words.
    check(!decoded.metadata && decoded.failure==F::sdk_rejected && decoded.sdk_hresult==int32_t(E_INVALIDARG),"e10 unsupported SDK input: E_INVALIDARG");
    std::cout<<"e10 RTS0: SDK rejected bare 160-byte payload, HRESULT 0x80070057; no description/parameters published\n";
    auto unknown=std::array<uint8_t,160>{}; std::copy(std::begin(real),std::end(real),unknown.begin());
    unknown[0]=0xff; // Negative fixture only: unsupported version word, never decoded by hand.
    reject(unknown.data(),unknown.size(),F::sdk_rejected);
}
}
int main(int argc,char** argv) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    trace_mutations=argc==2 && std::strcmp(argv[1],"--trace")==0;
    // J-043 discriminating probes. Run separately before promoting a mutation
    // to the regular sweep. No SEH, padding, or exception interception: an SDK
    // fault terminates the process and the last flushed marker identifies it.
    if(argc==2 && std::strcmp(argv[1],"--non-dxbc")==0) {
        std::vector<uint8_t> bytes(32,0); write_word(bytes,24,32);
        std::cout<<"J-043 non-DXBC length=32 offset=24 value=32 sha256="
            <<sha256(bytes.data(),bytes.size())<<std::endl;
        GuardedPrefix input(bytes.data(),bytes.size());
        auto d=RootBindingMetadata::from_serialized(input.input,bytes.size());
        check(!d.metadata && d.failure==SerializedRootFailure::sdk_rejected &&
            d.sdk_hresult==int32_t(E_INVALIDARG),"non-DXBC precise SDK rejection");
        return 0;
    }
    if(argc==5 && std::strcmp(argv[1],"--body-word")==0) {
        const auto version=RootVersion(std::strtoul(argv[2],nullptr,0));
        auto l=fixture(); l.version=version; l.parameters[0].shader_register=7;
        auto bytes=serialize(l,D3D_ROOT_SIGNATURE_VERSION(version));
        const size_t offset=std::strtoul(argv[3],nullptr,0);
        const uint32_t value=uint32_t(std::strtoul(argv[4],nullptr,0));
        check(offset<=bytes.size() && bytes.size()-offset>=4,"body probe word bounds");
        write_word(bytes,offset,value);
        std::cout<<"J-043 body version="<<uint32_t(version)<<" size="<<bytes.size()
            <<" offset="<<offset<<" value="<<value<<" sha256="
            <<sha256(bytes.data(),bytes.size())<<std::endl;
        GuardedPrefix input(bytes.data(),bytes.size());
        auto d=RootBindingMetadata::from_serialized(input.input,bytes.size());
        std::cout<<"returned failure="<<int(d.failure)<<" hr="<<d.sdk_hresult
            <<" detail="<<d.detail<<std::endl;
        check(d.valid() ? d.metadata->valid() : !d.metadata && d.failure!=SerializedRootFailure::none,
            "body probe atomic outcome");
        return 0;
    }
    // Fresh-process diagnostic: --prefix <0=e10|1|2> <length>
    // <module|sdk|zero|module-guard|sdk-guard|zero-guard|header>.
    // No exception interception: the last flushed marker identifies the call.
    if(argc==5 && std::strcmp(argv[1],"--prefix")==0) {
        const auto version=RootVersion(std::strtoul(argv[2],nullptr,10));
        auto l=fixture(); l.version=version; l.parameters[0].shader_register=7;
        RootParameter root; root.kind=ParameterKind::srv; root.visibility=Visibility::vertex; root.shader_register=3; root.space=2;
        if(version==RootVersion::v1_1) { root.flags=8; l.parameters[1].ranges[1].flags=8; }
        l.parameters.push_back(root);
        root.kind=ParameterKind::cbv; root.shader_register=9; l.parameters.push_back(root);
        root.kind=ParameterKind::uav; root.shader_register=5; l.parameters.push_back(root);
        RootParameter samplers; samplers.visibility=Visibility::pixel;
        samplers.ranges={{BindingKind::sampler,2,1,3,0,append_range}}; l.parameters.push_back(samplers);
        auto bytes=uint32_t(version)==0 ? std::vector<uint8_t>(std::begin(real),std::end(real)) :
            serialize(l,D3D_ROOT_SIGNATURE_VERSION(version));
        if(std::strcmp(argv[4],"header")==0) {
            for(size_t i=0;i<bytes.size();i+=4) {
                uint32_t word=0; std::memcpy(&word,bytes.data()+i,4);
                std::cout<<i<<":"<<std::hex<<word<<std::dec<<'\n';
            }
            return 0;
        }
        const size_t n=std::strtoul(argv[3],nullptr,10);
        check(n<=bytes.size(),"prefix in fixture bounds");
        // Exactly n readable fixture bytes; no deliberate safety padding.
        auto prefix=std::make_unique<uint8_t[]>(n ? n : 1);
        std::copy_n(bytes.data(),n,prefix.get());
        void* pages=nullptr;
        uint8_t* input=prefix.get();
        if(std::strstr(argv[4],"guard")) {
            SYSTEM_INFO info{}; GetSystemInfo(&info);
            pages=VirtualAlloc(nullptr,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
            check(pages!=nullptr,"test pages allocated"); DWORD old=0;
            auto* end=static_cast<uint8_t*>(pages)+info.dwPageSize;
            check(VirtualProtect(end,info.dwPageSize,PAGE_NOACCESS,&old)!=0,"test guard page");
            input=end-n; std::copy_n(bytes.data(),n,input);
        }
        std::cout<<"version="<<uint32_t(version)<<" length="<<n<<" total="<<bytes.size()<<std::endl;
        if(std::strstr(argv[4],"zero")) std::fill_n(input,n,uint8_t(0));
        if(std::strstr(argv[4],"sdk")) {
            ID3D12VersionedRootSignatureDeserializer* raw=nullptr;
            std::cout<<"before SDK create"<<std::endl;
            auto hr=D3D12CreateVersionedRootSignatureDeserializer(input,n,IID_PPV_ARGS(&raw));
            std::unique_ptr<ID3D12VersionedRootSignatureDeserializer,Release<ID3D12VersionedRootSignatureDeserializer>> owned(raw);
            std::cout<<"after SDK create hr="<<std::hex<<uint32_t(hr)<<std::dec<<std::endl;
            if(SUCCEEDED(hr) && raw) {
                std::cout<<"before SDK desc"<<std::endl;
                const auto* desc=raw->GetUnconvertedRootSignatureDesc();
                std::cout<<"after SDK desc present="<<(desc!=nullptr)<<std::endl;
            }
        } else {
            std::cout<<"before module"<<std::endl;
            auto result=RootBindingMetadata::from_serialized(input,n);
            std::cout<<"after module failure="<<int(result.failure)<<" hr="<<std::hex<<uint32_t(result.sdk_hresult)<<std::dec<<std::endl;
        }
        if(pages) VirtualFree(pages,0,MEM_RELEASE);
        return 0;
    }
    header_layout_tests();
    serialized_tests();
    // J-129: only the serialized, opt-in single PS s0 variant is supported.
    for(auto version:{RootVersion::v1_0,RootVersion::v1_1}) {
        RootLayout sampler;sampler.version=version;sampler.static_sampler_count=1;
        check(!RootBindingMetadata::from_adapter(sampler).valid(),"caller static layout is not serialized provenance");
        auto bytes=serialize(sampler,D3D_ROOT_SIGNATURE_VERSION(version));
        auto off=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());
        check(!off.valid()&&off.detail=="static samplers unsupported by adapter","default static refusal unchanged");
        auto on=RootBindingMetadata::from_serialized(bytes.data(),bytes.size(),true);
        check(on.valid()&&on.metadata->layout().static_sampler_count==1&&on.metadata->serialized_ps_s0(),"opt-in serialized static s0 cached");
        const auto expected=std::array<uint32_t,13>{0,3,3,3,0,1,8,0,0,0,0,0,0x7f7fffffu};
        check(*on.metadata->serialized_ps_s0()==expected,"static full description words retained");
        sampler.static_sampler_count=2;bytes=serialize(sampler,D3D_ROOT_SIGNATURE_VERSION(version));
        check(!RootBindingMetadata::from_serialized(bytes.data(),bytes.size(),true).valid(),"multiple static samplers refuse bounded variant");
    }
    {
        // Under the opt-in, every NON-supported static layout must keep the exact legacy
        // refusal (unsupported_layout + "static samplers unsupported by adapter"), not a
        // blank detail. Roots are serialized directly so each field can vary.
        const char* legacy="static samplers unsupported by adapter";
        auto build=[&](UINT count,UINT reg,UINT space,D3D12_SHADER_VISIBILITY vis,UINT flags,bool psSamplerTable) {
            D3D12_STATIC_SAMPLER_DESC samplers[2]{};
            for(UINT i=0;i<count&&i<2;++i) {
                samplers[i].Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;samplers[i].AddressU=samplers[i].AddressV=samplers[i].AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                samplers[i].MaxLOD=D3D12_FLOAT32_MAX;samplers[i].ShaderRegister=reg+i;samplers[i].RegisterSpace=space;samplers[i].ShaderVisibility=vis;
            }
            D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,5,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,1};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
            UINT np=1;
            if(psSamplerTable) {
                parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
                parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;np=2;
            }
            D3D12_ROOT_SIGNATURE_DESC desc{np,parameters,count,samplers,D3D12_ROOT_SIGNATURE_FLAGS(flags)};
            ID3DBlob* blob=nullptr;ID3DBlob* errors=nullptr;
            const auto hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);
            if(errors)errors->Release();
            check(SUCCEEDED(hr)&&blob,"variant serializes");
            std::vector<uint8_t> out;
            if(blob){const auto* p=static_cast<const uint8_t*>(blob->GetBufferPointer());out.assign(p,p+blob->GetBufferSize());blob->Release();}
            return out;
        };
        auto expectLegacy=[&](const std::vector<uint8_t>& b,const char* what) {
            auto r=RootBindingMetadata::from_serialized(b.data(),b.size(),true);
            check(!r.valid()&&r.failure==SerializedRootFailure::unsupported_layout&&r.detail==legacy,what);
        };
        const auto ps=D3D12_SHADER_VISIBILITY_PIXEL;
        // The supported shape still works (single PS s0, space 0, no PS sampler table).
        auto good=build(1,0,0,ps,0x1,false);
        check(RootBindingMetadata::from_serialized(good.data(),good.size(),true).valid(),"opt-in supported single PS s0");
        expectLegacy(build(2,0,0,ps,0x1,false),"opt-in two static samplers keep legacy detail");
        expectLegacy(build(1,1,0,ps,0x1,false),"opt-in static s1 keeps legacy detail");
        expectLegacy(build(1,0,1,ps,0x1,false),"opt-in static space 1 keeps legacy detail");
        expectLegacy(build(1,0,0,D3D12_SHADER_VISIBILITY_VERTEX,0x1,false),"opt-in VS-only static s0 keeps legacy detail");
        expectLegacy(build(1,0,0,ps,0x1|D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS,false),"opt-in PS-deny flag keeps legacy detail");
        expectLegacy(build(1,0,0,ps,0x1,true),"opt-in static s0 plus PS sampler table keeps legacy detail");
    }
    serialized_budgets();
    auto l=fixture();
    // Avoid overlap between constants b0 and the table's b0..b1.
    l.parameters[0].shader_register=7;
    const auto m=RootBindingMetadata::from_adapter(l); const auto r=m.resolve_srt_data();
    check(m.valid() && m.rejection().empty(),"valid version1.1 adapter");
    check(r.valid && r.root_index==1 && r.range_index==1 && r.table_offset==9 && r.parameter_kind==ParameterKind::table,"APPEND resolution after explicit gap");
    check(m.layout().parameters[1].ranges[1].offset==7,"owned normalized APPEND");
    check(!m.resolve(BindingKind::srv,10,1,ShaderStage::geometry).valid,"space distinction");
    check(!m.resolve(BindingKind::uav,10,0,ShaderStage::geometry).valid,"register kind distinction");
    check(!m.resolve(BindingKind::srv,10,0,ShaderStage::vertex).valid,"stage visibility");
    check(!m.resolve(BindingKind::srv,12,0,ShaderStage::geometry).valid,"range end exclusive");
    check(m.resolve(BindingKind::srv,8,0,ShaderStage::geometry).table_offset==7,"range beginning");
    auto descending=l;
    descending.parameters[1].ranges={{BindingKind::srv,2,0,0,0,20},
        {BindingKind::srv,2,4,0,0,0},{BindingKind::srv,2,10,0,0,append_range}};
    check(RootBindingMetadata::from_adapter(descending).resolve_srt_data().table_offset==2,"APPEND follows previous range rather than maximum offset");
    auto edge=l;
    edge.parameters[1].ranges={{BindingKind::srv,1,0xffffffffu,2,0,0xfffffffeu}};
    const auto er=RootBindingMetadata::from_adapter(edge).resolve(BindingKind::srv,0xffffffffu,2,ShaderStage::geometry);
    check(er.valid && er.table_offset==0xfffffffeu,"maximum register and bounded offset preserved");
    check(m.resolve(BindingKind::cbv,7,0,ShaderStage::compute).parameter_kind==ParameterKind::constants,"constants are distinguishable from descriptors");
    auto x=l; x.version=RootVersion::v1_0; check(RootBindingMetadata::from_adapter(x).valid(),"version1.0 adapter");
    RootParameter root; root.kind=ParameterKind::srv; root.shader_register=10; root.space=0; root.visibility=Visibility::geometry;
    x=l; x.parameters[1]=root;
    auto rm=RootBindingMetadata::from_adapter(x);
    check(rm.valid() && rm.resolve_srt_data().valid && rm.resolve_srt_data().root_index==1 && rm.resolve_srt_data().parameter_kind==ParameterKind::srv,"root SRV resolution");
    x.parameters[1].space=3; rm=RootBindingMetadata::from_adapter(x);
    check(!rm.resolve_srt_data().valid && rm.resolve(BindingKind::srv,10,3,ShaderStage::geometry).valid,"root space distinction");
    x=l; x.flags=16; check(RootBindingMetadata::from_adapter(x).valid() && !RootBindingMetadata::from_adapter(x).resolve_srt_data().valid,"geometry denied");
    x=l; x.parameters[1].visibility=Visibility::all; check(RootBindingMetadata::from_adapter(x).resolve(BindingKind::srv,10,0,ShaderStage::compute).valid,"ALL compute visibility");
    x=l; x.parameters.push_back(root); rejected(x,"table/root ambiguity");
    x=l; root.visibility=Visibility::vertex; x.parameters.push_back(root); rm=RootBindingMetadata::from_adapter(x);
    check(rm.valid() && rm.resolve_srt_data().root_index==1 && rm.resolve(BindingKind::srv,10,0,ShaderStage::vertex).root_index==2,"disjoint visibility allows same register");
    x=l; x.parameters[0].shader_register=0; rejected(x,"constants/table CBV ambiguity");
    x=l; x.version=static_cast<RootVersion>(3); rejected(x,"unsupported1.2 version");
    x=l; x.static_sampler_count=1; rejected(x,"unsupported static samplers");
    for(uint32_t f:{0x80u,0x400u,0x800u,0x80000000u}) { x=l; x.flags=f; rejected(x,"unsupported root flags"); }
    x=l; x.parameters[1].kind=static_cast<ParameterKind>(99); rejected(x,"unknown parameter");
    x=l; x.parameters[1].visibility=static_cast<Visibility>(6); rejected(x,"unsupported visibility");
    x=l; x.parameters[1].ranges.clear(); rejected(x,"empty table");
    // AS/MS deny flags are neutral for this classic-stage-only resolver. All
    // previously accepted low-bit layouts retain identical binding outcomes.
    for(uint32_t low=0;low<128;++low)for(uint32_t deny:{0x100u,0x200u,0x300u}) {
        x=l;x.flags=low;const auto before=RootBindingMetadata::from_adapter(x);
        x.flags|=deny;const auto after=RootBindingMetadata::from_adapter(x);
        check(before.valid()&&after.valid(),"classic plus AS/MS deny flags accepted");
        for(auto stage:{ShaderStage::vertex,ShaderStage::pixel,ShaderStage::geometry,ShaderStage::hull,ShaderStage::domain})
            for(auto kind:{BindingKind::srv,BindingKind::cbv,BindingKind::sampler}) {
                const auto a=before.resolve(kind,10,0,stage),b=after.resolve(kind,10,0,stage);
                check(a.valid==b.valid&&a.rejection==b.rejection&&a.root_index==b.root_index&&a.table_offset==b.table_offset,
                    "AS/MS deny flags preserve classic resolution");
            }
        for(uint32_t unsupported:{0x80u,0x400u,0x800u,0x1000u,0x80000000u}) {
            auto bad=x;bad.flags|=unsupported;rejected(bad,"unknown/local/direct flags remain rejected with deny flags");
        }
    }
    x=l; x.parameters[1].flags=2; rejected(x,"irrelevant table fields");
    x=l; x.parameters[1].ranges[1].count=0; rejected(x,"zero range");
    x=l; x.parameters[1].ranges[1].count=0xffffffffu; rejected(x,"unbounded range");
    x=l; x.parameters[1].ranges[1].base_register=0xfffffffeu; rejected(x,"register overflow");
    x=l; x.parameters[1].ranges[1].offset=0xfffffffeu; rejected(x,"table offset overflow");
    x=l; x.parameters[1].ranges[0].offset=0xfffffffcu; x.parameters[1].ranges[0].count=3; rejected(x,"APPEND offset overflow");
    x=l; x.parameters[1].ranges[1].offset=6; rejected(x,"overlapping table slots");
    x=l; x.parameters[1].ranges.push_back({BindingKind::srv,1,10,0,0,20}); rejected(x,"overlapping register ranges");
    x=l; x.parameters[1].ranges[1].kind=static_cast<BindingKind>(99); rejected(x,"unknown range kind");
    x=l; x.parameters[1].ranges[1].kind=BindingKind::sampler; rejected(x,"mixed heap kinds");
    for(uint32_t f:{0x10u,6u,0x10001u}) { x=l; x.parameters[1].ranges[1].flags=f; rejected(x,"range invalid/conflicting flags"); }
    x=l; x.parameters[1].ranges[1].flags=0x10008; check(RootBindingMetadata::from_adapter(x).valid(),"supported1.1 range flags");
    x.version=RootVersion::v1_0; rejected(x,"1.1 flags in1.0");
    x=l; x.parameters[0].constant_count=65; rejected(x,"DWORD budget");
    x=l; x.parameters[0].constant_count=0; rejected(x,"empty constants");
    x=l; x.parameters[0].ranges.push_back({}); rejected(x,"constants with ranges");
    x=l; x.parameters.resize(65); rejected(x,"root count limit");
    root.visibility=Visibility::all;
    for(ParameterKind k:{ParameterKind::cbv,ParameterKind::srv,ParameterKind::uav}) {
        x=l; root.kind=k; root.space=4; root.flags=8; x.parameters[1]=root;
        check(RootBindingMetadata::from_adapter(x).valid(),"supported1.1 root descriptor");
        x.version=RootVersion::v1_0; rejected(x,"root flags in1.0");
        x.version=RootVersion::v1_1; x.parameters[1].flags=6; rejected(x,"conflicting root data flags");
    }
    x={}; RootParameter sampler; sampler.ranges={{BindingKind::sampler,2,0,1,0,append_range}}; x.parameters={sampler};
    check(RootBindingMetadata::from_adapter(x).resolve(BindingKind::sampler,1,1,ShaderStage::pixel).table_offset==1,"sampler table");
    x.version=RootVersion::v1_1; x.parameters[0].ranges[0].flags=2; rejected(x,"sampler data flags unsupported");
    check(!m.resolve(static_cast<BindingKind>(99),10,0,ShaderStage::geometry).valid,"invalid query kind");
    check(!m.resolve(BindingKind::srv,10,0,static_cast<ShaderStage>(99)).valid,"invalid query stage");
    l.parameters.clear(); l.parameters.shrink_to_fit();
    check(m.resolve_srt_data().valid && m.resolve_srt_data().table_offset==9,"owned layout independent of input lifetime");
    x={}; check(RootBindingMetadata::from_adapter(x).valid() && !RootBindingMetadata::from_adapter(x).resolve_srt_data().valid,"empty signature has no binding");
    std::cout<<checks<<" root binding metadata offline checks passed (CPU parsing only; no runtime proof)\n";
}
