#include "capture_requirements_proof.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>

namespace probe { long remaining=-1; }
void* operator new(size_t n) {
    if(probe::remaining==0)throw std::bad_alloc();
    if(probe::remaining>0)--probe::remaining;
    if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
void operator delete[](void* p,size_t) noexcept {std::free(p);}
using namespace acz::pso;
using acz::metadata::SerializedRootFailure;
namespace {
unsigned checks=0;
void check(bool b,const char* text) {++checks;if(!b){std::cerr<<"FAIL: "<<text<<'\n';std::exit(1);}}
std::string sha(const uint8_t* p,size_t n) {
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"CNG SHA provider");
    DWORD size=0,got=0;
    check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0)==0,"CNG object size");
    std::vector<uint8_t> object(size);std::array<uint8_t,32> digest{};
    check(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)==0,"CNG create hash");
    check(n<=ULONG_MAX&&BCryptHashData(hash,const_cast<PUCHAR>(p),ULONG(n),0)==0,"CNG hash bytes");
    check(BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0)==0,"CNG finish hash");
    BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    std::string result;for(auto b:digest){result.push_back("0123456789abcdef"[b>>4]);result.push_back("0123456789abcdef"[b&15]);}return result;
}
// Narrow extraction is used ONLY after the entire evidence JSON's SHA-256 is
// checked. No arbitrary JSON, escaped strings, missing blobs or skips accepted.
std::vector<uint8_t> blob(const std::string& json,const char* pin) {
    const auto table=json.find("\"blobs\"");check(table!=std::string::npos,"JSON blobs table required");
    const auto key=json.find(std::string("\"")+pin+"\"",table);check(key!=std::string::npos,"JSON pinned blob required");
    const auto base=json.find("\"base64\"",key),end=json.find('}',key);
    check(base!=std::string::npos&&base<end,"JSON base64 field required in blob");
    const auto colon=json.find(':',base),begin=json.find('"',colon),last=json.find('"',begin+1);
    check(begin<last&&last<end,"JSON base64 quoted string bounds");
    const auto s=json.substr(begin+1,last-begin-1);
    check(s.size()%4==0,"base64 complete quads");
    auto digit=[](char c)->int {if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;
        if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;};
    std::vector<uint8_t> bytes;
    for(size_t i=0;i<s.size();i+=4) {
        const auto a=digit(s[i]),b=digit(s[i+1]);check(a>=0&&b>=0,"base64 leading digits");
        const bool cpad=s[i+2]=='=',dpad=s[i+3]=='=';
        const auto c=cpad?0:digit(s[i+2]),d=dpad?0:digit(s[i+3]);
        check(c>=0&&d>=0&&(!cpad||dpad)&&(!(cpad||dpad)||i+4==s.size()),"base64 terminal padding");
        check((!cpad||!(b&15))&&(!dpad||cpad||!(c&3)),"base64 canonical pad bits");
        bytes.push_back(uint8_t((a<<2)|(b>>4)));
        if(!cpad)bytes.push_back(uint8_t((b<<4)|(c>>2)));
        if(!dpad)bytes.push_back(uint8_t((c<<6)|d));
    }
    check(sha(bytes.data(),bytes.size())==pin,"blob SHA-256 pin");return bytes;
}
uint32_t u(const std::vector<uint8_t>& b,size_t o){check(o+4<=b.size(),"fixture word bounds");return uint32_t(b[o])|(uint32_t(b[o+1])<<8)|(uint32_t(b[o+2])<<16)|(uint32_t(b[o+3])<<24);}
void put(std::vector<uint8_t>& b,size_t o,uint32_t v){check(o+4<=b.size(),"fixture edit bounds");for(size_t i=0;i<4;++i)b[o+i]=uint8_t(v>>(8*i));}
size_t part(const std::vector<uint8_t>& b,const char* tag){for(size_t i=0;i<u(b,28);++i){const size_t o=u(b,32+4*i);if(!std::memcmp(b.data()+o,tag,4))return o;}check(false,"required fixture part");return 0;}
Request one(const std::vector<uint8_t>& b,ShaderStage s=ShaderStage::vertex){Request q;q.stages.push_back({s,StageState::present,b.data(),b.size()});return q;}
std::vector<uint8_t> replace_psv(const std::vector<uint8_t>& b,const std::vector<uint8_t>& payload) {
    const size_t o=part(b,"PSV0"),old=u(b,o+4),end=o+8+old;
    check(payload.size()%4==0,"synthetic PSV payload aligned");
    std::vector<uint8_t> c(b.begin(),b.begin()+o+8);c.insert(c.end(),payload.begin(),payload.end());c.insert(c.end(),b.begin()+end,b.end());
    put(c,o+4,uint32_t(payload.size()));put(c,24,uint32_t(c.size()));
    for(size_t i=0;i<u(b,28);++i){const auto off=u(b,32+4*i);if(off>=end)put(c,32+4*i,uint32_t(off-old+payload.size()));}
    return c;
}
void reject(const std::vector<uint8_t>& b,ShaderStage s,const char* why){auto r=build(one(b,s));check(!r&&!r.inventory&&r.failure!=Failure::none,"malformed no partial publication");check(r.rejection.find(why)!=std::string::npos,"precise rejection");}
namespace proof=acz::requirements;
using namespace acz::capture;
using namespace acz::metadata;
// Entirely synthetic, non-executable containers. ISG1/OSG1 and PSV signature
// tables follow the real E10 records; resource declarations are explicitly
// changed to the bounded buffer-only fixture. Bitcode is just magic bytes, not
// the game's code. These fixtures cannot establish executable shader behaviour.
std::vector<uint8_t> synthetic(const std::vector<uint8_t>& real,ShaderStage stage) {
    std::vector<std::pair<std::string,std::vector<uint8_t>>> parts;
    std::vector<uint8_t> program(28);put(program,0,(uint32_t(stage)<<16)|0x60);put(program,4,7);
    std::memcpy(program.data()+8,"DXIL",4);put(program,12,0x100);put(program,16,16);put(program,20,4);put(program,24,0xdec04342);
    parts.push_back({"DXIL",program});parts.push_back({"HASH",std::vector<uint8_t>(20)});
    for(const auto tag:{"ISG1","OSG1"}) {
        const auto o=part(real,tag);parts.push_back({tag,{real.begin()+o+8,real.begin()+o+8+u(real,o+4)}});
    }
    auto o=part(real,"PSV0")+8;const size_t runtime=u(real,o),count=u(real,o+4+runtime);
    const size_t tail=o+4+runtime+4+(count?4+count*u(real,o+4+runtime+4):0);
    std::vector<uint8_t> p(real.begin()+o,real.begin()+o+4+runtime);
    p.resize(p.size()+4);put(p,4+runtime,stage==ShaderStage::pixel?0:1);
    if(stage!=ShaderStage::pixel) {
        const auto start=p.size();p.resize(start+28);put(p,start,24);put(p,start+4,5);
        put(p,start+12,stage==ShaderStage::vertex?20:10);put(p,start+16,stage==ShaderStage::vertex?20:10);put(p,start+20,12);
    }
    const auto po=part(real,"PSV0");p.insert(p.end(),real.begin()+tail,real.begin()+po+8+u(real,po+4));parts.push_back({"PSV0",p});
    std::vector<uint8_t> out(32+parts.size()*4);std::memcpy(out.data(),"DXBC",4);put(out,20,1);put(out,28,uint32_t(parts.size()));
    for(size_t i=0;i<parts.size();++i) {
        put(out,32+4*i,uint32_t(out.size()));const auto begin=out.size();out.resize(begin+8);
        std::memcpy(out.data()+begin,parts[i].first.data(),4);put(out,begin+4,uint32_t(parts[i].second.size()));
        out.insert(out.end(),parts[i].second.begin(),parts[i].second.end());while(out.size()%4)out.push_back(0);
    }put(out,24,uint32_t(out.size()));return out;
}
std::vector<uint8_t> root_bytes(uint32_t vsreg=20,uint32_t space=0,D3D12_SHADER_VISIBILITY vis=D3D12_SHADER_VISIBILITY_VERTEX) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};D3D12_ROOT_PARAMETER parameters[2]{};
    for(size_t i=0;i<2;++i) {
        ranges[i]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,i?10:vsreg,i?0:space,0};
        parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].ShaderVisibility=i?D3D12_SHADER_VISIBILITY_GEOMETRY:vis;
        parameters[i].DescriptorTable={1,&ranges[i]};
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};desc.Version=D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0.NumParameters=2;desc.Desc_1_0.pParameters=parameters;
    desc.Desc_1_0.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* bytes=nullptr;ID3DBlob* errors=nullptr;
    auto hr=D3D12SerializeVersionedRootSignature(&desc,&bytes,&errors);if(errors)errors->Release();
    check(SUCCEEDED(hr)&&bytes,"CPU synthetic root serialization");
    const auto* p=static_cast<const uint8_t*>(bytes->GetBufferPointer());std::vector<uint8_t> out(p,p+bytes->GetBufferSize());bytes->Release();return out;
}
struct Fixture {
    acz::binding::NativeBindingObserver observer;
    State state;
    Command cmd{false,3,2,1,0,19};
    proof::Request q;
    std::array<std::vector<uint8_t>,3> containers;
    std::shared_ptr<std::vector<uint8_t>> vertices=std::make_shared<std::vector<uint8_t>>(512),srt=std::make_shared<std::vector<uint8_t>>(8*416);
    Fixture(const std::array<std::vector<uint8_t>,3>& real) {
        constexpr ShaderStage stages[]={ShaderStage::vertex,ShaderStage::pixel,ShaderStage::geometry};
        for(size_t i=0;i<3;++i)containers[i]=synthetic(real[i],stages[i]);
        q.pipeline.stages={{ShaderStage::vertex,StageState::present,containers[0].data(),containers[0].size()},
            {ShaderStage::pixel,StageState::present,containers[1].data(),containers[1].size()},
            {ShaderStage::geometry,StageState::present,containers[2].data(),containers[2].size()},
            {ShaderStage::hull,StageState::explicitly_absent},{ShaderStage::domain,StageState::explicitly_absent}};
        for(uint32_t i=0;i<4;++i)q.pipeline.input_layout.push_back({"ATTRIBUTE",i,2,0,i*16,0,0});
        q.pipeline.root_provenance=RootProvenance::explicit_root;
        state.pipeline.identity=new_lifetime_identity();state.pipeline.explicit_root_identity=new_lifetime_identity();state.pipeline.drawcpt=true;
        q.pso_identity=state.pipeline.identity;q.root_id=88;q.root_identity=state.pipeline.explicit_root_identity;
        q.pipeline.explicit_root_identity=q.root_identity;q.serialized_root=root_bytes();
        auto r=RootBindingMetadata::from_serialized(q.serialized_root.data(),q.serialized_root.size());check(r.valid(),"synthetic root decode");
        state.tracking_complete=true;state.set_root(q.root_id,r.metadata,q.root_identity);state.set_heaps({4});
        state.set_value(0,ParameterKind::table,0x2000);state.set_value(1,ParameterKind::table,0x2020);
        state.vertices[0]={2,0,vertices->size(),64};
        observer.heap({4,0x1000,0x2000,8,32,0,1,0},99);
        observer.resource({2,0x10000,vertices->size()},99);observer.resource({3,0x20000,srt->size()},99);
        write(2,64,8,0x1000);write(3,416,8,0x1020);
        q.synthetic_indexing=proof::SyntheticIndexRule{"SYNTHETIC caller-declared first + ordinal*step, not decoded",1,2};refresh();
    }
    void write(uint64_t id,uint32_t stride,uint32_t n,uint64_t cpu) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=n;d.Buffer.StructureByteStride=stride;
        observer.srv(id,&d,cpu,99);
    }
    void refresh() {
        auto& c=q.copies;c={};c.list_identity=state.list_identity;c.generation=state.generation;c.revision=state.revision;c.device=99;
        c.serialized=c.synchronized=true;
        auto a=observer.table(state,0,4,0,99),b=observer.table(state,1,4,0,99);check(a&&b,"synthetic table witnesses");
        c.buffers={{observer.resource_token(2),a,vertices},{observer.resource_token(3),b,srt}};
        c.descriptors={{0,0,a},{1,0,b}};
    }
    proof::Result run(){return proof::derive(observer,state,cmd,q);}
    void patch_binding(size_t stage,size_t word,uint32_t value) {
        auto& b=containers[stage];put(b,part(b,"PSV0")+8+64+word*4,value);
    }
};
template<class F> void rejected(const std::array<std::vector<uint8_t>,3>& bytes,const char* text,F mutate) {
    Fixture f(bytes);mutate(f);auto r=f.run();
    if(r||r.rejection.find(text)==std::string::npos)std::cerr<<"expected '"<<text<<"', got '"<<r.rejection<<"'\n";
    check(!r&&!r.proof&&r.rejection.find(text)!=std::string::npos,"specific rejection/no publication");
}
void tests(const std::array<std::vector<uint8_t>,3>& bytes) {
    {
        Fixture strip(bytes);strip.cmd={true,9,1,0,0,0};strip.state.index=BufferView{2,0,18,2};
        const uint16_t live[]={0,1,2,3,0xffff,4,5,6,7};
        std::memcpy(strip.vertices->data(),live,sizeof live);strip.state.pipeline.strip_cut=1;
        for(uint32_t topology:{3u,5u,11u,13u}) {
            strip.state.topology=topology;const auto result=strip.run();
            check(result&&result.proof->inputs().vertex_access[0].offset==0&&
                result.proof->inputs().vertex_access[0].length==512,"matching strip cut excluded from fetched span");
            auto lease=acz::capture_input::assemble(strip.observer,strip.state,result.proof->inputs());
            check(lease&&lease.lease->inputs().topology==topology&&lease.lease->inputs().strip_cut==1,
                "provider retains observed topology/cut facts");
            check(!strip.state.pipeline.requirements_proven,"strip proof cannot enable native capture");
        }
        auto refused=[&](const char* why) {const auto result=strip.run();
            check(!result&&result.rejection.find(why)!=std::string::npos,"strip/index specific refusal");};
        strip.state.topology=4;refused("vertex fetch exceeds");
        strip.state.topology=5;strip.state.pipeline.strip_cut=0;refused("vertex fetch exceeds");
        strip.state.pipeline.strip_cut=2;refused("vertex fetch exceeds");
        strip.state.pipeline.strip_cut=1;strip.state.topology=0;refused("strip-cut index requires unavailable topology metadata");
        strip.state.topology=5;strip.state.pipeline.strip_cut=acz::index_range::unknown_cut;
        refused("strip-cut index requires unavailable topology metadata");
        strip.state.pipeline.strip_cut=1;strip.cmd.base_vertex=-1;refused("negative base-vertex fetch");
        strip.cmd.start=5;strip.cmd.count=4;strip.cmd.base_vertex=-4;
        auto result=strip.run();check(result&&result.proof->inputs().vertex_access[0].length==256,
            "strip index range applies signed base after restart filtering");
        const uint32_t wide[]={0,1,2,3,0xffffffffu,4,5,6,7};
        std::memcpy(strip.vertices->data(),wide,sizeof wide);strip.state.index=BufferView{2,0,36,4};
        strip.cmd={true,9,1,0,0,0};strip.state.pipeline.strip_cut=2;
        result=strip.run();check(bool(result),"32-bit matching restart accepted");
        strip.state.pipeline.strip_cut=1;refused("vertex fetch exceeds");
        strip.state.pipeline.strip_cut=0;refused("vertex fetch exceeds");
        strip.state.pipeline.strip_cut=2;strip.state.topology=12;refused("vertex fetch exceeds");
    }
    Fixture f(bytes);auto r=f.run();if(!r)std::cerr<<r.rejection<<'\n';check(bool(r),"synthetic positive proof");
    const auto& p=*r.proof;
    check(p.manifest().size()==2&&p.manifest()[0].reg==20&&p.manifest()[1].reg==10,"manifest derived from every stage PSV0");
    check(p.vertex_slots()==std::vector<uint32_t>{0}&&p.inputs().vertex_access.size()==1&&p.inputs().vertex_access[0].offset==64&&p.inputs().vertex_access[0].length==192,"vertex spans from real-shaped VS signatures and synthetic draw");
    check(p.inputs().srt_indices==std::vector<uint64_t>{1,3},"labelled caller rule evaluated");
    check(p.facts().size()==5&&p.facts()[1].origin==proof::Origin::derived_from_parts&&p.facts()[3].origin==proof::Origin::caller_declared,"per-fact origins");
    check(!f.state.pipeline.requirements_proven&&!p.state().pipeline.requirements_proven,"proof never sets native requirements");
    auto lease=acz::capture_input::assemble(f.observer,f.state,p.inputs());check(bool(lease),"proof feeds J027 assemble");
    auto blocked=acz::capture_input::freeze_owned(f.observer,f.state,f.cmd,*lease.lease);check(!blocked,"native unproven State still rejected");
    // Explicit fixture-only publication of the synthetic manifest. The library
    // never writes these State fields or native readiness flags.
    f.state.pipeline.resources=p.manifest();f.state.pipeline.vertex_slots=p.vertex_slots();f.state.pipeline.requirements_proven=true;
    auto frozen=acz::capture_input::freeze_owned(f.observer,f.state,f.cmd,*lease.lease);
    check(frozen&&frozen.snapshot->cpu_data_complete&&!frozen.snapshot->gpu_replay_complete,"synthetic proof inputs feed existing freeze_owned");
    (*f.vertices)[0]=99;check((*p.inputs().buffers[0].bytes)[0]==0,"independent owned proof bytes");
    f.cmd.start=2;auto changed=f.run();check(changed&&changed.proof->inputs().vertex_access[0].offset==128,"draw argument changes derived span");
    f.cmd.instances=3;changed=f.run();check(changed&&changed.proof->inputs().srt_indices==std::vector<uint64_t>{1,3,5},"instance count changes explicit rule indices");
    Fixture indexed(bytes);indexed.cmd.indexed=true;indexed.cmd.start=0;indexed.state.index=BufferView{2,0,12,2};
    (*indexed.vertices)[0]=1;(*indexed.vertices)[2]=3;(*indexed.vertices)[4]=2;
    auto ir=indexed.run();check(ir&&ir.proof->inputs().vertex_access[0].offset==64&&ir.proof->inputs().vertex_access[0].length==192,"indexed fetch reads owned index bytes and signed base vertex");
    indexed.cmd.base_vertex=-1;ir=indexed.run();check(ir&&ir.proof->inputs().vertex_access[0].offset==0,"signed base vertex changes span");
    rejected(bytes,"VS stage unknown",[](auto& f){f.q.pipeline.stages[0]={ShaderStage::vertex,StageState::unknown};});
    rejected(bytes,"PS stage unknown",[](auto& f){f.q.pipeline.stages[1]={ShaderStage::pixel,StageState::unknown};});
    rejected(bytes,"HS stage unknown",[](auto& f){f.q.pipeline.stages.erase(f.q.pipeline.stages.begin()+3);});
    rejected(bytes,"DS stage unknown",[](auto& f){f.q.pipeline.stages.pop_back();});
    rejected(bytes,"index expression missing",[](auto& f){f.q.synthetic_indexing.reset();});
    rejected(bytes,"index expression missing",[](auto& f){f.q.synthetic_indexing->label.clear();});
    rejected(bytes,"extra supplied root",[](auto& f){f.write(2,64,8,0x1040);auto d=f.observer.table(f.state,0,4,2,99);f.q.copies.descriptors.push_back({0,2,d});});
    rejected(bytes,"missing descriptor",[](auto& f){f.q.copies.descriptors.erase(f.q.copies.descriptors.begin());});
    rejected(bytes,"register/space/visibility",[](auto& f){f.patch_binding(0,2,21);f.patch_binding(0,3,21);});
    rejected(bytes,"register/space/visibility",[](auto& f){f.patch_binding(0,3,21);});
    rejected(bytes,"register/space/visibility",[](auto& f){f.patch_binding(0,1,1);});
    rejected(bytes,"register/space/visibility",[](auto& f){auto b=root_bytes(20,0,D3D12_SHADER_VISIBILITY_PIXEL);f.q.serialized_root=b;f.state.root=RootBindingMetadata::from_serialized(b.data(),b.size()).metadata;});
    for(uint32_t type:{1u,3u,4u,6u,7u,8u,9u})rejected(bytes,"texture/UAV/sampler/raw/typed",[=](auto& f){f.patch_binding(0,0,type);});
    rejected(bytes,"unbounded",[](auto& f){f.patch_binding(0,3,UINT32_MAX);});
    rejected(bytes,"input-layout/ISG1",[](auto& f){f.q.pipeline.input_layout[0].semantic="WRONG";});
    rejected(bytes,"input-layout/ISG1",[](auto& f){f.q.pipeline.input_layout[0].format=3;});
    rejected(bytes,"VS PSV0/ISG1",[](auto& f){auto& b=f.containers[0];put(b,part(b,"ISG1")+8+8+20,7);});
    rejected(bytes,"unsupported input-layout format",[](auto& f){f.q.pipeline.input_layout[0].format=28;});
    rejected(bytes,"no bound view",[](auto& f){f.state.vertices.clear();});
    rejected(bytes,"vertex fetch exceeds",[](auto& f){f.state.vertices[0].size=200;});
    rejected(bytes,"per-instance/step-rate",[](auto& f){f.q.pipeline.input_layout[0].classification=1;});
    rejected(bytes,"per-instance/step-rate",[](auto& f){f.q.pipeline.input_layout[0].classification=1;f.q.pipeline.input_layout[0].step_rate=2;});
    rejected(bytes,"APPEND input-layout",[](auto& f){f.q.pipeline.input_layout[0].offset=UINT32_MAX;});
    rejected(bytes,"aliased input-layout",[](auto& f){f.q.pipeline.input_layout[1].offset=0;});
    rejected(bytes,"embedded or unknown",[](auto& f){f.q.pipeline.root_provenance=RootProvenance::embedded_container;});
    rejected(bytes,"embedded or unknown",[](auto& f){f.q.pipeline.root_provenance=RootProvenance::unknown;});
    rejected(bytes,"PSO/root identity mismatch",[](auto& f){++f.q.pso_identity;});
    rejected(bytes,"PSO/root identity mismatch",[](auto& f){++f.q.root_identity;});
    rejected(bytes,"list identity/generation/revision",[](auto& f){++f.q.copies.list_identity;});
    rejected(bytes,"list identity/generation/revision",[](auto& f){++f.q.copies.generation;});
    rejected(bytes,"list identity/generation/revision",[](auto& f){++f.q.copies.revision;});
    rejected(bytes,"provenance",[](auto& f){f.write(2,64,8,0x1000);});
    rejected(bytes,"provenance",[](auto& f){f.observer.remove_resource(f.observer.resource_token(2));});
    rejected(bytes,"provenance",[](auto& f){f.observer.resource({2,0x10000,f.vertices->size(),1,true},99);});
    rejected(bytes,"provenance",[](auto& f){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;f.observer.srv(2,&d,0x1000,99);});
    rejected(bytes,"provenance",[](auto& f){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Format=DXGI_FORMAT_R32_TYPELESS;d.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;d.Buffer.NumElements=128;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;f.observer.srv(2,&d,0x1000,99);});
    rejected(bytes,"unsupported shader model",[](auto& f){auto& b=f.containers[0];put(b,part(b,"DXIL")+8,(uint32_t(ShaderStage::vertex)<<16)|0x61);});
    rejected(bytes,"PSV0/ISG1/OSG1 unavailable",[](auto& f){auto& b=f.containers[0];std::memcpy(b.data()+part(b,"PSV0"),"NONE",4);});
    rejected(bytes,"PSV0/ISG1/OSG1 unavailable",[](auto& f){auto& b=f.containers[1];std::memcpy(b.data()+part(b,"ISG1"),"NONE",4);});
    rejected(bytes,"index view missing",[](auto& f){f.cmd.indexed=true;});
    rejected(bytes,"negative base-vertex",[](auto& f){f.cmd.indexed=true;f.cmd.start=0;f.cmd.base_vertex=-1;f.state.index=BufferView{2,0,12,2};});
    rejected(bytes,"strip-cut index",[](auto& f){f.cmd.indexed=true;f.cmd.start=0;f.state.index=BufferView{2,0,12,2};(*f.vertices)[0]=255;(*f.vertices)[1]=255;});
    rejected(bytes,"addition overflow",[](auto& f){f.q.synthetic_indexing->first=UINT64_MAX;f.q.synthetic_indexing->step=1;f.cmd.instances=2;});
    rejected(bytes,"multiplication overflow",[](auto& f){f.q.synthetic_indexing->first=0;f.q.synthetic_indexing->step=UINT64_MAX;f.cmd.instances=3;});
    rejected(bytes,"addition overflow",[](auto& f){f.state.vertices[0].offset=UINT64_MAX;});
    rejected(bytes,"state manifest extra/missing",[](auto& f){f.state.pipeline.resources={{BindingKind::srv,10,0,ShaderStage::geometry}};});
    rejected(bytes,"state manifest duplicates",[](auto& f){f.state.pipeline.resources={{BindingKind::srv,20,0,ShaderStage::vertex},{BindingKind::srv,20,0,ShaderStage::vertex}};});
    // Allocation exhaustion at every publication boundary; no partial object.
    Fixture allocation(bytes);bool succeeded=false;size_t failures=0;
    for(long n=0;n<2000;++n) {
        probe::remaining=n;auto trial=allocation.run();probe::remaining=-1;
        if(trial){succeeded=true;break;}check(!trial.proof,"allocation failure no partial proof");++failures;
    }check(succeeded&&failures>50,"allocation sweep reaches success");
}
}
namespace {
namespace dp=acz::draw_provenance;
struct SelectedFixture {
    dp::Profile profile{};
    dp::Snapshot candidate{};
    std::shared_ptr<const Inventory> inventory;
    std::optional<RootBindingMetadata> root;
    void reset(const std::array<std::vector<uint8_t>,3>& real,bool indexed=false,bool cbv=false) {
        profile={};candidate={};
        Request pipeline;pipeline.root_provenance=RootProvenance::explicit_root;pipeline.explicit_root_identity=201;
        for(size_t i=0;i<3;++i)pipeline.stages.push_back({i==0?ShaderStage::vertex:i==1?ShaderStage::pixel:ShaderStage::geometry,StageState::present,real[i].data(),real[i].size()});
        pipeline.stages.push_back({ShaderStage::hull,StageState::explicitly_absent});
        pipeline.stages.push_back({ShaderStage::domain,StageState::explicitly_absent});
        for(uint32_t i=0;i<4;++i)pipeline.input_layout.push_back({"ATTRIBUTE",i,2,0,i*16,0,0});
        auto built=build(pipeline);check(bool(built),"selected E10 inventory");inventory=built.inventory;
        RootLayout layout;RootParameter c;c.kind=cbv?ParameterKind::cbv:ParameterKind::constants;c.shader_register=10;c.constant_count=cbv?0:4;
        layout.parameters.push_back(c);
        for(auto k:{BindingKind::srv,BindingKind::srv,BindingKind::sampler}) {
            RootParameter t;t.kind=ParameterKind::table;t.ranges.push_back({k,1,layout.parameters.size()==1?10u:0u,0,0,0});layout.parameters.push_back(t);
        }
        root=RootBindingMetadata::from_adapter(layout);check(root->valid(),"selected synthetic root");
        auto& id=profile.identity;
        id.capture=1;id.device=42;id.device_epoch=10;id.list_lifetime=100;id.reset_generation=3;id.reset_revision=5;
        id.draw_ordinal=7;id.submission_serial=50;id.queue=60;id.queue_epoch=20;id.pso=300;id.pso_lifetime=301;
        id.root=200;id.root_lifetime=201;id.input_layout=400;id.profile_revision=1;id.all_stage_profile=500;
        id.draw={true,indexed,3,1,0,0,0};
        profile.origin=dp::Origin::FixtureObservation;profile.copy_serial=70;profile.copy_after_submission=50;
        profile.used_stages=(1u<<unsigned(dp::Stage::VS))|(1u<<unsigned(dp::Stage::PS))|(1u<<unsigned(dp::Stage::GS))|(1u<<unsigned(dp::Stage::IA));
        profile.absent_stages=(1u<<unsigned(dp::Stage::HS))|(1u<<unsigned(dp::Stage::DS))|(1u<<unsigned(dp::Stage::CS));
        candidate.origin=dp::Origin::FixtureObservation;candidate.identity=id;
        candidate.used_stages=profile.used_stages;candidate.absent_stages=profile.absent_stages;
        candidate.copy={dp::Origin::FixtureObservation,42,10,60,20,50,70,true};
        const auto cb=cbv?dp::Kind::RootDescriptor:dp::Kind::RootConstants;
        add({dp::Stage::VS,cb,10,0,0,0});add({dp::Stage::VS,dp::Kind::StructuredBuffer,10,0,1,0});
        add({dp::Stage::PS,cb,10,0,0,0});add({dp::Stage::PS,dp::Kind::Sampler,0,0,3,0});
        add({dp::Stage::PS,dp::Kind::Texture,0,0,2,0});add({dp::Stage::GS,dp::Kind::StructuredBuffer,10,0,1,0});
        add({dp::Stage::IA,dp::Kind::Vertex,0,0,0,0});
        if(indexed)add({dp::Stage::IA,dp::Kind::Index,0,0,0,0});
    }
    void add(dp::Key key) {
        const auto n=profile.count++;auto& r=profile.required[n];auto& x=candidate.inputs[candidate.input_count++];
        r.key=x.key=key;x.origin=dp::Origin::FixtureObservation;x.root_id=200;x.root_lifetime=201;
        r.bytes=x.bytes=key.kind==dp::Kind::RootConstants?16:512;
        r.stride=x.stride=key.kind==dp::Kind::StructuredBuffer?416:64;
        if(key.kind==dp::Kind::RootConstants) {r.root_words=x.word_count=4;}
        else if(key.kind==dp::Kind::Sampler) {
            r.sampler_words=x.sampler_word_count=1;r.sampler_values[0]=x.sampler_words[0]=123;
            r.sampler_identity=x.sampler_identity=1;r.sampler_lifetime=x.sampler_lifetime=2;
            r.sampler_revision=x.sampler_revision=3;r.sampler_source=x.sampler_source=4;x.sampler_immutable=true;
        } else if(key.stage==dp::Stage::GS || (key.stage==dp::Stage::PS&&key.kind==dp::Kind::RootDescriptor)) {
            const size_t prior=key.stage==dp::Stage::GS?1:0;
            x.resource=candidate.inputs[prior].resource;
            r.resource_id=profile.required[prior].resource_id;r.resource_lifetime=profile.required[prior].resource_lifetime;
            r.resource_generation=1;r.resource_version=5;r.resource_window_bytes=512;
        } else {
            x.resource=uint32_t(candidate.resource_count++);auto& b=candidate.resources[x.resource];
            b.origin=dp::Origin::FixtureObservation;b.id=1000+x.resource;b.lifetime=2000+x.resource;b.generation=1;
            b.device=42;b.device_epoch=10;b.bytes=b.copy_bytes=512;b.draw_version=b.copy_version=5;
            b.draw_queue=b.copy_queue=60;b.draw_queue_epoch=b.copy_queue_epoch=20;b.draw_submission=b.copy_after_submission=50;
            b.history_serial=9;b.complete_history=b.writer_excluded=b.ordered=b.current_at_draw=b.current_at_copy=true;
            r.resource_id=b.id;r.resource_lifetime=b.lifetime;r.resource_generation=1;r.resource_version=5;r.resource_window_bytes=512;
            candidate.states[candidate.state_count++]={dp::Origin::FixtureObservation,x.resource,0,dp::State::ShaderResource,dp::State::CopySource,dp::State::ShaderResource,true,true,true,false};
        }
        if(key.kind==dp::Kind::StructuredBuffer||key.kind==dp::Kind::Texture||key.kind==dp::Kind::Sampler) {
            const bool sampler=key.kind==dp::Kind::Sampler;
            r.descriptor=x.descriptor=dp::DescriptorKind::Dynamic;r.heap=x.heap=sampler?6:1;r.heap_lifetime=x.heap_lifetime=2;
            r.heap_revision=x.heap_revision=3;r.descriptor_revision=x.descriptor_revision=4;r.descriptor_source=x.descriptor_source=5;
            r.heap_class=x.heap_class=sampler?dp::HeapClass::Sampler:dp::HeapClass::CbvSrvUav;
            r.heap_capacity=x.heap_capacity=64;r.table_base=x.table_base=0;r.table_extent=x.table_extent=1;
        }
        if(key.kind==dp::Kind::Texture) {
            r.texture_lifetime=x.texture_lifetime=1;r.texture_version=x.texture_version=2;r.texture_view=x.texture_view=3;
            r.texture_content=x.texture_content=4;r.texture_layout=x.texture_layout=5;r.texture_dimension=x.dimension=2;
            r.texture_format=x.format=28;r.subresources=x.subresources=1;
        }
        if(key.kind==dp::Kind::Vertex) {r.vertex_count=x.vertex_count=8;r.instance_count=x.instance_count=1;}
        if(key.kind==dp::Kind::Index) {
            r.stride=x.stride=2;r.index_format=x.index_format=dp::IndexFormat::UInt16;
            r.index_count=x.index_count=3;r.min_index=x.min_index=0;r.max_index=x.max_index=2;
            r.vertex_count=x.vertex_count=8;x.index_values_known=x.index_access_complete=true;
        }
    }
    proof::CoverageResult run() {return proof::check_selected_inputs(*inventory,*root,profile,candidate);}
};
static SelectedFixture selected;
void selected_tests(const std::array<std::vector<uint8_t>,3>& real) {
    // The opt-in narrow rule is limited to the authenticated E11 two-word
    // layout. This remains a fixture-only whole-input contract, not readiness.
    auto two_words=[&] {
        selected.reset(real);
        auto layout=selected.root->layout();layout.parameters[0].constant_count=2;
        selected.root=RootBindingMetadata::from_adapter(layout);
        for(size_t i:{size_t(0),size_t(2)}) {
            auto& r=selected.profile.required[i];auto& x=selected.candidate.inputs[i];
            r.root_words=x.word_count=2;r.bytes=x.bytes=8;
            r.words[0]=x.words[0]=0x80000000u;r.words[1]=x.words[1]=0;
        }
    };
    two_words();
    check(!selected.run().synthetic_consistent,"two-word default-OFF path unchanged");
    auto narrow=[&] {return proof::check_selected_inputs(*selected.inventory,*selected.root,
        selected.profile,selected.candidate,true);};
    check(narrow().synthetic_consistent,"authenticated E11 two-word diagnostic subset accepted");
    selected.candidate.inputs[2].words[1]=1;
    check(!narrow().synthetic_consistent,"two-word shared stage exact-bit mismatch refused");
    two_words();selected.profile.required[0].root_words=selected.candidate.inputs[0].word_count=1;
    selected.profile.required[0].bytes=selected.candidate.inputs[0].bytes=4;
    check(!narrow().synthetic_consistent,"two-word subset cannot omit PS-consumed y through VS view");
    two_words();auto changed=*selected.inventory;changed.stages[0].container.back()^=1;
    check(!proof::check_selected_inputs(changed,*selected.root,selected.profile,selected.candidate,true).synthetic_consistent,
          "two-word opt-in cannot bypass full-container pin");
    for(bool indexed:{false,true})for(bool cbv:{false,true}) {
        selected.reset(real,indexed,cbv);auto r=selected.run();if(!r.synthetic_consistent)std::cerr<<r.rejection<<'\n';
        check(r.synthetic_consistent,"complete selected synthetic input profile");
        check(!selected.candidate.readiness&&!selected.candidate.requirements_proven,"coverage never promotes readiness");
    }
    auto reject=[&](auto mutate,const char* why) {
        selected.reset(real,true);mutate(selected);const auto r=selected.run();
        if(r.synthetic_consistent||r.rejection.find(why)==std::string::npos)std::cerr<<"coverage expected "<<why<<", got "<<r.rejection<<'\n';
        check(!r.synthetic_consistent&&r.rejection.find(why)!=std::string::npos,"selected precise refusal");
    };
    for(size_t i=0;i<8;++i)reject([=](auto& f){f.profile.required[i].key.kind=dp::Kind::Unknown;},"selected");
    for(size_t i=0;i<8;++i)reject([=](auto& f){f.candidate.inputs[i].origin=dp::Origin::Missing;},"invalid origin");
    reject([](auto& f){f.profile.used_stages=1;},"all-stage");
    reject([](auto& f){f.profile.required[0].root_words=3;},"root words incomplete");
    reject([](auto& f){f.candidate.inputs[0].words[0]=1;},"root words");
    reject([](auto& f){f.candidate.inputs[4].texture_content=0;},"texture");
    reject([](auto& f){f.candidate.inputs[3].sampler_immutable=false;},"sampler");
    reject([](auto& f){f.candidate.inputs[7].index_values_known=false;},"index");
    reject([](auto& f){f.candidate.inputs[6].vertex_count=1;},"vertex");
    reject([](auto& f){f.candidate.inputs[f.candidate.input_count++]=f.candidate.inputs[4];},"duplicate input");
    reject([](auto& f){f.candidate.resources[0].copy_version++;},"resource version");
    reject([](auto& f){f.profile.required[1].bytes=415;},"element zero incomplete");
    reject([](auto& f){f.profile.required[6].stride=32;},"IA stride");
    reject([](auto& f){f.profile.required[1]=f.profile.required[0];},"ambiguous");
    reject([](auto& f){f.candidate.inputs[5].offset=1;f.profile.required[5].offset=1;},"descriptor");
    reject([](auto& f){f.candidate.inputs[3].sampler_words[0]++;},"sampler");
    reject([](auto& f){f.candidate.inputs[4].subresources=2;},"texture");
    reject([](auto& f){f.candidate.inputs[7].min_index=3;},"index");
    reject([](auto& f){f.candidate.inputs[4].descriptor_revision++;},"descriptor");
    reject([](auto& f){f.candidate.readiness=true;},"proof forgery");
    // Independently retained stage descriptors remain valid generic provenance.
    // Only the selected-draw adapter requires one shared E10 root/table view.
    for(bool cbv:{false,true}) {
        selected.reset(real,true,cbv);
        selected.candidate.inputs[5].descriptor_slot=1;
        selected.profile.required[5].descriptor_slot=1;
        selected.candidate.inputs[5].table_base=1;
        selected.profile.required[5].table_base=1;
        auto generic=std::make_unique<dp::Contract>();
        check(generic->begin({dp::max_resources,dp::max_entries,dp::max_states,dp::max_root_words,dp::max_sampler_words},
                             selected.profile,selected.candidate)&&generic->finish(),
              "generic independently pinned stage views remain accepted");
        const auto scoped=selected.run();
        check(!scoped.synthetic_consistent&&scoped.rejection.find("shared table heap or base conflict")!=std::string::npos,
              "selected shared binding conflict remains adapter scoped");
    }
    // Each record remains authenticated, but its requested pipeline slot is false.
    // Remove PS t0/s0 as well: stage confusion must never erase those obligations.
    for(bool duplicate:{false,true}) {
        selected.reset(real);
        auto confused=*selected.inventory;
        if(duplicate)confused.stages[1]=confused.stages[0];
        else std::swap(confused.stages[1],confused.stages[4]);
        for(size_t i=0;i<selected.profile.count;) {
            const auto kind=selected.profile.required[i].key.kind;
            if(kind==dp::Kind::Texture||kind==dp::Kind::Sampler) {
                for(size_t j=i+1;j<selected.profile.count;++j)selected.profile.required[j-1]=selected.profile.required[j];
                --selected.profile.count;
            } else ++i;
        }
        const auto r=proof::check_selected_inputs(confused,*selected.root,selected.profile,selected.candidate);
        check(!r.synthetic_consistent&&r.rejection.find("shader stage differs")!=std::string::npos,
              "valid swapped/duplicated pinned stages cannot omit PS texture/sampler");
    }
    // Different SRV registers and categories share one table, with a nonzero
    // base and explicit nonzero range offsets. Slot equality is not base equality.
    auto shared_table=[&] {
        selected.reset(real);
        auto layout=selected.root->layout();
        layout.parameters[1].ranges={{BindingKind::srv,1,10,0,0,3},{BindingKind::srv,1,0,0,0,8}};
        // Remove the old t0 table to keep root resolution unambiguous.
        layout.parameters[2]={};layout.parameters[2].kind=ParameterKind::constants;
        layout.parameters[2].shader_register=11;layout.parameters[2].constant_count=4;
        selected.root=RootBindingMetadata::from_adapter(layout);
        check(selected.root->valid(),"shared mixed-category SRV table metadata");
        for(size_t i:{size_t(1),size_t(4),size_t(5)}) {
            auto& r=selected.profile.required[i];auto& x=selected.candidate.inputs[i];
            r.key.root_parameter=x.key.root_parameter=1;
            r.descriptor_slot=x.descriptor_slot=i==4?18:13;
            r.table_base=x.table_base=10;r.table_extent=x.table_extent=9;
        }
    };
    shared_table();check(selected.run().synthetic_consistent,"consistent cross-register cross-kind table base");
    for(unsigned field=0;field<4;++field) {
        shared_table();auto& r=selected.profile.required[4];auto& x=selected.candidate.inputs[4];
        if(field==0)++r.descriptor_slot,++x.descriptor_slot,++r.table_base,++x.table_base;
        if(field==1)++r.heap,++x.heap;
        if(field==2)++r.heap_lifetime,++x.heap_lifetime;
        if(field==3)++r.heap_revision,++x.heap_revision;
        auto generic=std::make_unique<dp::Contract>();
        check(generic->begin({dp::max_resources,dp::max_entries,dp::max_states,dp::max_root_words,dp::max_sampler_words},
                             selected.profile,selected.candidate)&&generic->finish(),"generic separately retained table entries accepted");
        const auto r2=selected.run();
        check(!r2.synthetic_consistent&&r2.rejection.find("shared table heap or base conflict")!=std::string::npos,
              "selected cross-register cross-kind table conflict refused");
    }
    // A table has one typed heap and a bounded slot interval. The generic
    // contract also refuses a sampler and SRV table claiming the same heap.
    selected.reset(real);selected.profile.required[4].descriptor_slot=12;
    selected.candidate.inputs[4].descriptor_slot=12;
    selected.profile.required[4].table_base=selected.candidate.inputs[4].table_base=12;
    check(selected.run().synthetic_consistent,"different root parameters may retain different table bases");
    auto generic_reject=[&](auto mutate,const char* label) {
        selected.reset(real);mutate(selected);
        auto generic=std::make_unique<dp::Contract>();
        check(!(generic->begin({dp::max_resources,dp::max_entries,dp::max_states,dp::max_root_words,dp::max_sampler_words},
                              selected.profile,selected.candidate)&&generic->finish()),label);
        check(!selected.run().synthetic_consistent,label);
    };
    generic_reject([](auto& f){f.profile.required[1].heap_class=f.candidate.inputs[1].heap_class=dp::HeapClass::None;},
                   "missing dynamic heap class refused");
    generic_reject([](auto& f){f.profile.required[1].heap_capacity=f.candidate.inputs[1].heap_capacity=0;},
                   "missing dynamic heap capacity refused");
    shared_table();selected.profile.required[4].heap_capacity=selected.candidate.inputs[4].heap_capacity=18;
    {auto generic=std::make_unique<dp::Contract>();
     check(!(generic->begin({dp::max_resources,dp::max_entries,dp::max_states,dp::max_root_words,dp::max_sampler_words},
                            selected.profile,selected.candidate)&&generic->finish()),"table base plus extent beyond heap refused");}
    check(!selected.run().synthetic_consistent,"selected table base plus extent beyond heap refused");
    generic_reject([](auto& f){f.profile.required[1].table_extent=f.candidate.inputs[1].table_extent=UINT64_MAX;},
                   "table extent overflow refused");
    generic_reject([](auto& f){auto& r=f.profile.required[3];auto& x=f.candidate.inputs[3];r.heap=x.heap=1;},
                   "SRV and sampler heap identity collision refused");
    shared_table();selected.profile.required[4].heap_capacity=selected.candidate.inputs[4].heap_capacity=63;
    check(!selected.run().synthetic_consistent,"shared table capacity disagreement refused");
    selected.reset(real,true);auto unknown=*selected.inventory;unknown.stages[0].state=StageState::unknown;
    check(!proof::check_selected_inputs(unknown,*selected.root,selected.profile,selected.candidate).synthetic_consistent,"unknown stage refused");
    unknown=*selected.inventory;unknown.stages[0].container.back()^=1;
    check(!proof::check_selected_inputs(unknown,*selected.root,selected.profile,selected.candidate).synthetic_consistent,"container pin mismatch refused");
    auto unsupported=selected.root->layout();unsupported.parameters[0].kind=ParameterKind::table;unsupported.parameters[0].constant_count=0;unsupported.parameters[0].shader_register=0;
    unsupported.parameters[0].ranges={{BindingKind::cbv,1,10,0,0,0}};
    const auto table_cbv=RootBindingMetadata::from_adapter(unsupported);check(table_cbv.valid(),"table CBV fixture metadata");
    check(!proof::check_selected_inputs(*selected.inventory,table_cbv,selected.profile,selected.candidate).synthetic_consistent,"table CBV later subset refused");
    selected.reset(real);bool coverage_succeeded=false;size_t allocation_refusals=0;
    for(long n=0;n<256;++n) {
        probe::remaining=n;const auto trial=selected.run();probe::remaining=-1;
        if(trial.synthetic_consistent){coverage_succeeded=true;break;}
        ++allocation_refusals;
    }
    check(coverage_succeeded&&allocation_refusals>5,"selected coverage allocation failures publish no success");
    selected.reset(real);const auto unavailable=proof::check_selected_coverage(*selected.inventory,*selected.root,acz::evidence_epochs::EpochSnapshot{},selected.profile,selected.candidate);
    check(!unavailable.synthetic_consistent&&unavailable.rejection=="selected epochs unavailable","full coverage refuses absent L3-A epochs");
}
}

int run_requirements_tests(int argc,char** argv) {
    check(argc==2,"E10 path required");std::ifstream file(argv[1],std::ios::binary);check(bool(file),"E10 exists");
    std::string json((std::istreambuf_iterator<char>(file)),{});
    check(sha(reinterpret_cast<const uint8_t*>(json.data()),json.size())=="17f9a48e3d57be77a6b3bcac350573deaffddd2c2daf7388984f4f27f1e41b55","E10 SHA256");
    const std::array<std::vector<uint8_t>,3> real{blob(json,"adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd"),
        blob(json,"7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd"),blob(json,"847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da")};
    Fixture f(real);
    for(size_t i=0;i<3;++i)f.q.pipeline.stages[i]={i==0?ShaderStage::vertex:i==1?ShaderStage::pixel:ShaderStage::geometry,StageState::present,real[i].data(),real[i].size()};
    auto decoded=build(f.q.pipeline);check(bool(decoded),"all real E10 stages via merged reader");
    check(decoded.inventory->stages[0].psv->bindings.size()==2&&decoded.inventory->stages[1].psv->bindings.size()==3&&decoded.inventory->stages[4].psv->bindings.size()==1,"real PSV binding counts");
    f.q.synthetic_indexing.reset();
    auto r=f.run();check(!r&&r.rejection.find("register/space/visibility")!=std::string::npos,"real E10 cannot match synthetic explicit root");
    f.q.serialized_root=real[0];r=f.run();check(!r&&r.rejection=="evidence cb10 resolves to root constants; descriptor-backed proof unsupported","real E10 resolves all evidence resources then rejects unsupported root constants");
    tests(real);
    selected_tests(real);
    std::cout<<checks<<" CPU offline checks passed; synthetic metadata/rules only, no native indexing or VR proof\n";
    return 0;
}

#ifndef ACZ_SELECTED_EPOCH_TESTS
int main(int argc,char** argv) {return run_requirements_tests(argc,argv);}
#endif
