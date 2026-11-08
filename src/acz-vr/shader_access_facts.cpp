#include "shader_access_facts.h"
#include <algorithm>
#include <stdexcept>

namespace acz::shader_access {
namespace {
using metadata::ShaderStage;
constexpr metadata::ShaderIdentifier identifier(const char* s) {
    metadata::ShaderIdentifier out{};
    auto digit=[](char c) constexpr {return c<='9'?c-'0':c-'a'+10;};
    for(size_t i=0;i<16;++i)out[i]=uint8_t(digit(s[2*i])*16+digit(s[2*i+1]));
    return out;
}
constexpr Resource vs[]={{ResourceClass::cbv,10,0,1,2,13},{ResourceClass::srv_structured,10,0,1,5,12}};
constexpr Resource ps[]={{ResourceClass::cbv,10,0,1,2,13},{ResourceClass::sampler,0,0,1,1,14},{ResourceClass::srv_typed,0,0,1,3,2}};
constexpr Resource gs[]={{ResourceClass::srv_structured,10,0,1,5,12}};
// E11 bufferLoad.f32 sites: VS 128/144/160,192/208/224/240/256/272/288,352;
// GS 0/16/32/48/64/80/96/112,400. All use literal element index i32 0.
// Each load is conservatively covered by 16 bytes, including unused lanes.
constexpr ByteRange vr[]={{128,176},{192,304},{352,368}},gr[]={{0,128},{400,416}};
constexpr SrtAccess va{10,0,0,416,vr,3},ga{10,0,0,416,gr,2};
constexpr Record records[]={
    {ShaderStage::vertex,identifier("f4cfd928c11d0a822baeb92e09767134"),"adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd","e11-drawcpt-dxil; e10-drawcpt-pso",vs,2,&va,true,e11_vs_cb10_words},
    {ShaderStage::pixel,identifier("e1372ac10d7191c7e4d1c03b66465260"),"7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd","e11-drawcpt-dxil; e10-drawcpt-pso",ps,3,nullptr,true,e11_ps_cb10_words},
    {ShaderStage::geometry,identifier("06084b6262839d7b336b1d8d0d0b86b8"),"847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da","e11-drawcpt-dxil; e10-drawcpt-pso",gs,1,&ga,false}};std::string sha256(const uint8_t* data,size_t n) {
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
}
bool known_hash(const metadata::ShaderIdentifier& hash) noexcept {
    for(const auto& r:records)if(r.shader_hash==hash)return true;
    return false;
}
Lookup lookup(const pso::Stage& stage) noexcept {
    Lookup out;
    try {
        auto reject=[&](const char* why){out.rejection=why;return out;};
        const Record* record=nullptr;
        for(const auto& r:records)if(r.shader_hash==stage.shader.identifier){record=&r;break;}
        if(!record)return reject("access facts: unknown shader hash");
        if(stage.state!=pso::StageState::present||!stage.shader.valid||stage.shader.stage!=record->stage)
            return reject("access facts: shader stage/metadata mismatch");
        if(stage.container.empty()||stage.container.size()>64*1024)return reject("access facts: container bytes missing/oversized");
        if(sha256(stage.container.data(),stage.container.size())!=record->container_sha256)
            return reject("access facts: container SHA-256 mismatch");
        const auto actual=metadata::decode_shader_metadata(stage.container.data(),stage.container.size());
        if(!actual.valid||actual.identifier!=record->shader_hash||actual.stage!=record->stage)
            return reject("access facts: decoded shader hash/stage mismatch");
        if(!stage.psv||stage.psv->bindings.size()!=record->resource_count)
            return reject("access facts: PSV0 resource count mismatch");
        for(size_t i=0;i<record->resource_count;++i) {
            const auto& r=record->resources[i];size_t matches=0;
            for(const auto& b:stage.psv->bindings)if(b.type==r.psv_type&&b.space==r.space&&b.lower==r.reg&&
                uint64_t(b.upper)+1==uint64_t(r.reg)+r.count&&b.kind&&*b.kind==r.psv_kind&&b.flags&&!*b.flags)++matches;
            if(matches!=1)return reject("access facts: PSV0 register/space/class/count mismatch");
        }
        out.record=record;
    } catch(...) {try{out.rejection="access facts: allocation/exception failure";}catch(...){}}
    return out;
}
std::string validate_srt(const Record& r,const capture::Descriptor& d,uint64_t bytes) {
    if(std::none_of(std::begin(records),std::end(records),[&](const auto& owned){return &owned==&r;}))
        return "access facts: record is not lookup-owned";
    if(!r.srt)return "access facts: stage has no SrtData read";
    const auto& a=*r.srt;
    if(d.kind!=metadata::BindingKind::srv||!d.buffer||!d.structured||d.view.stride!=a.element_size)
        return "access facts: SrtData descriptor class/416-byte stride mismatch";
    if(a.element>=d.elements)return "access facts: constant SrtData element out of bounds";
    // Subtraction and division avoid overflow in offset + (FirstElement+index)*stride.
    if(d.first_element>UINT64_MAX-a.element)return "access facts: SrtData element arithmetic overflow";
    const auto index=d.first_element+a.element;
    if(index>UINT64_MAX/a.element_size)return "access facts: SrtData byte arithmetic overflow";
    const auto offset=index*a.element_size;
    if(d.view.offset>bytes||d.view.size>bytes-d.view.offset||offset>d.view.size)
        return "access facts: SrtData view exceeds owned resource";
    if(a.element_size>d.view.size-offset)return "access facts: constant SrtData element exceeds descriptor/resource";
    if(d.elements>(d.view.size-offset)/a.element_size)return "access facts: descriptor element extent exceeds resource";
    for(size_t i=0;i<a.count;++i) {
        const auto& range=a.ranges[i];
        if(range.begin>=range.end||range.end>a.element_size||range.end>d.view.size-offset)
            return "access facts: SrtData read range exceeds descriptor/resource";
    }
    return {};
}
IndexedRead index_srt(const Record& record,const capture::Descriptor& d,uint64_t bytes,
                      const capture::Command& cmd) noexcept {
    IndexedRead out;
    try {
        bool owned=false;for(const auto& r:records)owned|=&r==&record;
        if(!owned)out.rejection="access facts: record is not lookup-owned";
        else if(!cmd.count||!cmd.instances||cmd.instances>4096)out.rejection="access facts: empty/oversized draw arguments";
        else out.rejection=validate_srt(record,d,bytes);
        if(out.rejection.empty())out.elements.assign(cmd.instances,record.srt->element);
        // StartInstanceLocation is deliberately irrelevant to literal i32 0.
    } catch(...) {
        out.elements.clear();try{out.rejection="access facts: indexing allocation/exception failure";}catch(...){}
    }
    return out;
}
}
