// Offline integration regression: real containers reach unsupported cb10 root constants;
// complete positive results are
// SYNTHETIC ONLY. No device/queue/XR creation, executable shader or VR claim.
#include "stereo_payload.h" // cockpit_math establishes NOMINMAX before SDK headers
#include "capture_requirements_proof.h"
#include "shader_access_facts.h"
#include "capture_enablement.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace acz::pso;
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

};

namespace input=acz::capture_input;
namespace copy=acz::native_copy;
namespace enable=acz::capture_enablement;
constexpr const char* missing_index="SrtData element index expression missing from PSV0/ISG1/OSG1; DXIL bitcode not decoded and no labelled synthetic caller rule";
void exact(const std::string& actual,const char* expected) {
    if(actual!=expected)std::cerr<<"expected '"<<expected<<"', got '"<<actual<<"'\n";
    check(actual==expected,"exact merged rejection reason");
}
// Fake COM byte source: no real COM resource/device. Opaque metadata IDs above
// never use its address. Provider must neither AddRef nor retain source aliases.
struct FakeBuffer final : IUnknown, copy::Source {
    ULONG refs=1;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    int fault=0, inspections=0;
    explicit FakeBuffer(std::shared_ptr<std::vector<uint8_t>> b):bytes(std::move(b)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {
        if(out)*out=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {return --refs;}
    copy::Stamp inspect(const copy::Request&) override {
        ++inspections;
        return {{700,fault==1?9u:10u},fault==4?0u:(fault==2&&inspections==2?2u:1u)};
    }
    size_t copy(const copy::Request&,uint8_t* out,size_t capacity) override {
        check(capacity==bytes->size(),"source exact whole resource");
        const auto n=capacity-(fault==3?1:0);std::memcpy(out,bytes->data(),n);return n;
    }
};
enum class Stop { none, inventory, proof, provider, assemble, freeze, payload, decision };
struct Chain {
    Stop stop=Stop::none;
    std::string reason;
    acz::pso::Result inventory;
    proof::Result derived;
    std::array<copy::Request,2> requests;
    std::array<copy::Result,2> copies;
    input::Request caller;
    input::Result lease;
    acz::capture::Result frozen;
    acz::stereo_payload::Result stereo{acz::stereo_payload::Error::missing_source,{}};
    enable::Decision decision;
    State synthetic_state;
    enable::Inputs facts(Fixture& f) const {
        enable::Inputs in;
        in.opt_in=true;in.state=&synthetic_state;in.observer=&f.observer;in.caller=&caller;
        in.root_id=f.q.root_id;in.root_identity=f.q.root_identity;
        in.pso_identity=f.q.pso_identity;in.pso_root_identity=f.q.root_identity;
        for(size_t i=0;i<2;++i)in.copies.push_back({&requests[i],&copies[i]});
        return in;
    }
};
// Explicit synthetic eye VPs/K; these are NOT native viewport/headset facts.
std::array<acz::Mat4,2> eyes() {
    auto left=acz::identity(),right=acz::identity();left[12]=-0.5;right[12]=0.5;
    return {left,right};
}
acz::Mat4 screen() {auto k=acz::identity();k[0]=2;k[5]=-3;k[12]=10;return k;}
void seed(Fixture& f) {
    for(size_t i=0;i<8;++i) {
        // Sentinel complete payload with labelled synthetic localToWorld +256.
        std::fill(f.srt->begin()+i*416,f.srt->begin()+(i+1)*416,uint8_t(32+i));
        float matrix[16]{};matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;matrix[12]=float(i);
        std::memcpy(f.srt->data()+i*416+256,matrix,sizeof(matrix));
    }
}
// Native State is never edited by this driver. The positive publication seam
// uses a distinct fixture-local copy, exactly as J037's fixture does.
void drive(Fixture& f,Chain& c,int source_fault=0,bool omit_manifest=false) {
    c.inventory=build(f.q.pipeline);
    if(!c.inventory){c.stop=Stop::inventory;c.reason=c.inventory.rejection;return;}
    c.derived=f.run();
    if(!c.derived){c.stop=Stop::proof;c.reason=c.derived.rejection;return;}
    c.synthetic_state=f.state;
    c.synthetic_state.pipeline.resources=c.derived.proof->manifest();
    c.synthetic_state.pipeline.vertex_slots=c.derived.proof->vertex_slots();
    c.synthetic_state.pipeline.requirements_proven=true; // SYNTHETIC caller assertion only.
    if(omit_manifest)c.synthetic_state.pipeline.resources.clear();
    c.caller=c.derived.proof->inputs();
    FakeBuffer sources[]={FakeBuffer(f.vertices),FakeBuffer(f.srt)};
    sources[1].fault=source_fault;
    for(size_t i=0;i<2;++i) {
        const auto& b=c.caller.buffers[i];
        c.requests[i]={b.resource,b.witness,0,b.bytes->size(),1,{700,10},99};
        c.copies[i]=copy::acquire(f.observer,c.synthetic_state,sources[i],c.requests[i]);
        check(sources[i].refs==1,"provider never owns fake COM source");
        if(!c.copies[i]){c.stop=Stop::provider;c.reason=c.copies[i].rejection;return;}
        check(*c.copies[i].buffer.bytes==*b.bytes,"provider bytes match immutable proof capture point");
        // derive owns a separate copy. decide requires the EXACT acquisition
        // storage, so retain derived facts but explicitly supply provider bytes.
        c.caller.buffers[i]=c.copies[i].buffer;
    }
    c.lease=input::assemble(f.observer,c.synthetic_state,c.caller);
    if(!c.lease){c.stop=Stop::assemble;c.reason=c.lease.rejection;return;}
    c.frozen=input::freeze_owned(f.observer,c.synthetic_state,f.cmd,*c.lease.lease);
    if(!c.frozen){c.stop=Stop::freeze;c.reason=c.frozen.rejection;return;}
    c.stereo=acz::stereo_payload::prepare(c.frozen.snapshot,eyes(),screen());
    if(!c.stereo){c.stop=Stop::payload;return;}
    c.decision=enable::decide(c.facts(f));
    if(!c.decision.enabled){c.stop=Stop::decision;c.reason=c.decision.reason;}
}
void unpublished(const Chain& c) {
    check(!c.lease&&!c.frozen&&!c.frozen.snapshot&&!c.stereo.batch&&!c.decision.enabled,
          "upstream rejection publishes no lease/snapshot/eyes/enabled decision");
}
void native_unchanged(const Fixture& f,const State& before) {
    check(!f.state.pipeline.requirements_proven&&!before.pipeline.requirements_proven&&
          f.state.pipeline.resources.empty()&&f.state.pipeline.vertex_slots.empty(),"native manifest readiness remains default");
    check(f.state.list_identity==before.list_identity&&f.state.generation==before.generation&&
          f.state.revision==before.revision&&f.state.pipeline.identity==before.pipeline.identity&&
          f.state.root_identity==before.root_identity&&f.state.roots.size()==before.roots.size(),"native State capture point unchanged");
    acz::capture::Snapshot native;
    check(!native.cpu_data_complete&&!native.gpu_replay_complete,"native snapshot defaults never promoted");
}
void real_failures(const std::array<std::vector<uint8_t>,3>& real) {
    Fixture f(real);const auto before=f.state;
    constexpr ShaderStage stages[]={ShaderStage::vertex,ShaderStage::pixel,ShaderStage::geometry};
    for(size_t i=0;i<3;++i)f.q.pipeline.stages[i]={stages[i],StageState::present,real[i].data(),real[i].size()};
    f.q.serialized_root=real[0];f.q.synthetic_indexing.reset();
    auto root=RootBindingMetadata::from_serialized(real[0].data(),real[0].size());
    check(root.valid()&&root.metadata,"real root now publishes owned metadata");
    check(root.metadata->layout().flags==0x30d,"SDK literal IA and HS/DS/AS/MS deny flags");
    auto inventory=build(f.q.pipeline);check(bool(inventory),"real inventory before proof");
    for(auto stage:stages) {
        const auto facts=acz::shader_access::lookup(inventory.inventory->stage(stage));
        check(bool(facts),"every exact real container passes access table");
        check(std::string(facts.record->evidence_ids)=="e11-drawcpt-dxil; e10-drawcpt-pso","evidence provenance retained");
        check(facts.record->srt ? facts.record->srt->element==0 : stage==ShaderStage::pixel,"E11 constant zero or PS no read");
    }
    Chain c;drive(f,c);check(c.stop==Stop::proof&&bool(c.inventory),"real containers/table/root pass; proof rejects root constants");
    exact(c.reason,"evidence cb10 resolves to root constants; descriptor-backed proof unsupported");check(!c.derived.proof,"real bytes publish no proof");unpublished(c);
    // Literal metadata decoding is allowed to publish an inventory, NOT a proof.
    for(size_t i=0;i<3;++i)check(c.inventory.inventory->stage(stages[i]).container==real[i],"inventory owns exact real stage bytes");
    native_unchanged(f,before);
    enable::Inputs native;native.opt_in=true;native.state=&f.state;native.observer=&f.observer;native.caller=&f.q.copies;
    native.root_id=f.q.root_id;native.root_identity=f.q.root_identity;native.pso_identity=f.q.pso_identity;native.pso_root_identity=f.q.root_identity;
    auto d=enable::decide(native);check(!d.enabled,"real failure cannot enable native State");exact(d.reason,"all-stage manifest assertion absent");
    auto lease=input::assemble(f.observer,f.state,f.q.copies);check(bool(lease),"native metadata-only lease can exist");
    auto frozen=input::freeze_owned(f.observer,f.state,f.cmd,*lease.lease);
    check(!frozen&&!frozen.snapshot,"native requirements false rejects freeze");exact(frozen.rejection,"all-stage resource requirements unproven");
    // Unsupported root constants block the ALL-REAL request. Isolate the missing
    // indexing fact using SYNTHETIC buffer declarations/root with the EXACT REAL
    // DXIL program payloads. Do not pretend this hybrid is the real pipeline.
    Fixture hybrid(real);hybrid.q.synthetic_indexing.reset();
    for(size_t i=0;i<3;++i) {
        auto& b=hybrid.containers[i];const auto dst=part(b,"DXIL"),src=part(real[i],"DXIL");
        const auto old=u(b,dst+4),length=u(real[i],src+4);const size_t end=dst+8+old;
        std::vector<uint8_t> changed(b.begin(),b.begin()+dst+8);
        changed.insert(changed.end(),real[i].begin()+src+8,real[i].begin()+src+8+length);
        changed.insert(changed.end(),b.begin()+end,b.end());put(changed,dst+4,length);put(changed,24,uint32_t(changed.size()));
        for(size_t n=0;n<u(b,28);++n){auto offset=u(b,32+4*n);if(offset>=end)put(changed,32+4*n,offset-old+length);}
        b=std::move(changed);hybrid.q.pipeline.stages[i]={stages[i],StageState::present,b.data(),b.size()};
    }
    Chain h;drive(hybrid,h);check(h.stop==Stop::proof&&!h.derived.proof,"real DXIL bitcode cannot provide indexing proof through synthetic carrier");
    exact(h.reason,missing_index);unpublished(h);
}
void evidence_rejections(const std::array<std::vector<uint8_t>,3>& real) {
    constexpr ShaderStage stages[]={ShaderStage::vertex,ShaderStage::pixel,ShaderStage::geometry};
    for(int fault=0;fault<6;++fault) {
        Fixture f(real);const auto before=f.state;
        f.containers=real;f.q.serialized_root=real[0];f.q.synthetic_indexing.reset();
        if(fault==0)f.containers[0][part(f.containers[0],"HASH")+12]^=1; // unknown shader identity
        if(fault==1) {
            f.containers[0]=synthetic(real[0],ShaderStage::vertex); // right HASH, wrong full bytes
            std::memcpy(f.containers[0].data()+part(f.containers[0],"HASH")+12,
                real[0].data()+part(real[0],"HASH")+12,16);
        }
        if(fault==2)f.containers[0][part(f.containers[0],"DXIL")+8+28]^=1; // opaque byte only
        if(fault==3||fault==4) {
            auto& bytes=f.containers[fault==3?0:1];const auto p=part(bytes,"PSV0")+8;
            const auto binding=p+12+u(bytes,p)+(fault==3?0:2)*24;
            // A container whose PSV0 contradicts the table must first fail its
            // complete byte pin. Direct decoded-record mismatches are tested in
            // shader_access_facts_offline without weakening the byte-pin seam.
            if(fault==3){put(bytes,binding+8,11);put(bytes,binding+12,11);}
            else {put(bytes,binding,5);put(bytes,binding+8,10);put(bytes,binding+12,10);put(bytes,binding+16,12);} // PS falsely claims t10
        }
        if(fault==5)f.q.synthetic_indexing=proof::SyntheticIndexRule{"explicit SYNTHETIC collision",0,0};
        for(size_t i=0;i<3;++i)f.q.pipeline.stages[i]={stages[i],StageState::present,f.containers[i].data(),f.containers[i].size()};
        Chain c;drive(f,c);check(c.stop==Stop::proof&&!c.derived.proof,"one-at-a-time evidence rejection before capture publication");
        exact(c.reason,fault==0?"access facts: unknown shader hash":fault==5?
            "evidence-derived and synthetic caller indexing rules both supplied":"access facts: container SHA-256 mismatch");
        unpublished(c);native_unchanged(f,before);
    }
}
void positive_and_links(const std::array<std::vector<uint8_t>,3>& real) {
    Fixture f(real);seed(f);const auto before=f.state;Chain c;drive(f,c);
    check(c.stop==Stop::none&&c.decision.enabled,"only fully labelled synthetic chain enables offline decision");
    exact(c.decision.reason,"all offline metadata facts present");
    const auto& p=*c.derived.proof;check(p.facts().size()==5,"all fact classifications retained");
    const proof::Origin origins[]={proof::Origin::caller_declared,proof::Origin::derived_from_parts,
        proof::Origin::derived_from_parts,proof::Origin::caller_declared,proof::Origin::caller_declared};
    for(size_t i=0;i<5;++i)check(p.facts()[i].origin==origins[i]&&!p.facts()[i].reason.empty(),"explicit derived versus declared facts");
    for(const auto& fact:p.facts())check(fact.evidence_ids.empty()&&fact.container_sha256.empty(),"synthetic proof never gains evidence provenance");
    check(p.facts()[3].reason==f.q.synthetic_indexing->label,"indexing label retained verbatim");
    check(!p.state().pipeline.requirements_proven,"proof retains unmodified native readiness");
    const auto& s=*c.frozen.snapshot;
    check(s.cpu_data_complete&&!s.gpu_replay_complete,"synthetic snapshot CPU-only readiness");
    check(s.command.instances==2&&s.command.start_instance==19&&s.srt_elements.size()==2,"command and explicit indices retained");
    check(s.srt_elements[0]==std::vector<uint8_t>(f.srt->begin()+416,f.srt->begin()+832)&&
          s.srt_elements[1]==std::vector<uint8_t>(f.srt->begin()+1248,f.srt->begin()+1664),"caller indices 1/3, never start_instance inferred");
    check(c.stereo.batch->source==c.frozen.snapshot,"both eyes retain one identical immutable snapshot");
    for(size_t i=0;i<2;++i) {
        check(c.stereo.batch->eyes[0][i]!=c.stereo.batch->eyes[1][i],"explicit eye VPs yield distinct bytes");
        for(size_t eye=0;eye<2;++eye) {
            const auto& b=c.stereo.batch->eyes[eye][i];check(b.size()==416,"complete eye stride");
            check(std::equal(b.begin()+128,b.end(),s.srt_elements[i].begin()+128),"only +0/+64 matrices replaced");
            float translation=0;std::memcpy(&translation,b.data()+64+48,4);
            const auto eye_translation=float(i?3:1)+(eye?0.5f:-0.5f);
            check(translation==eye_translation,"explicit localToWorld and eye VP row-vector translation");
            std::memcpy(&translation,b.data()+48,4);
            check(translation==eye_translation*2+10,"explicit synthetic K composes screen matrix at +0");
        }
    }
    auto native_lease=input::assemble(f.observer,f.state,c.caller);check(bool(native_lease),"derived/provider input valid with native State");
    auto native_freeze=input::freeze_owned(f.observer,f.state,f.cmd,*native_lease.lease);
    check(!native_freeze&&!native_freeze.snapshot,"positive chain still cannot freeze native State");exact(native_freeze.rejection,"all-stage resource requirements unproven");
    native_unchanged(f,before);native_lease={};
    // Recheck existing publications after EACH one-at-a-time provenance change.
    for(int link=0;link<3;++link) {
        Fixture broken(real);seed(broken);Chain b;drive(broken,b);check(b.decision.enabled,"fresh baseline per mutation");
        if(link==0)++broken.state.generation;
        if(link==1)broken.write(3,416,8,0x1020);
        if(link==2)broken.observer.resource({3,0x20000,broken.srt->size()},99);
        State live=b.synthetic_state;if(link==0)live.generation=broken.state.generation;
        FakeBuffer source(broken.srt);
        auto acquisition=copy::acquire(broken.observer,live,source,b.requests[1]);
        check(!acquisition&&!acquisition.buffer.bytes,"stale link blocks provider publication");exact(acquisition.rejection,"stale copy metadata");
        auto assembled=input::assemble(broken.observer,live,b.caller);check(!assembled&&!assembled.lease,"stale link blocks assembly");
        exact(assembled.rejection,link==0?"stale list lifetime/recording point":"stale buffer witness");
        auto frozen=input::freeze_owned(broken.observer,live,broken.cmd,*b.lease.lease);check(!frozen&&!frozen.snapshot,"stale link blocks freeze recheck");
        exact(frozen.rejection,link==0?"stale list lifetime/recording point":"stale buffer witness");
        auto facts=b.facts(broken);facts.state=&live;auto disabled=enable::decide(facts);check(!disabled.enabled,"stale link disables existing decision inputs");
        exact(disabled.reason,link==0?"stale list identity/generation/revision":"stale copy resource/descriptor provenance");
        Chain rerun;drive(broken,rerun);check(rerun.stop==Stop::proof&&!rerun.derived.proof,"stale upstream link blocks new proof");
        exact(rerun.reason,link==0?"list identity/generation/revision mismatch":"descriptor/resource provenance or owned-copy validation failed");unpublished(rerun);
    }
    // Destroy/mutate ALL source request storage; all published owned outputs live.
    const auto saved_resources=s.resources;const auto saved_eyes=c.stereo.batch->eyes;
    const auto proof_bytes=*p.inputs().buffers[1].bytes;
    const auto proof_container=p.inventory().stage(ShaderStage::geometry).container;
    std::fill(f.srt->begin(),f.srt->end(),0);std::fill(f.vertices->begin(),f.vertices->end(),255);
    for(auto& container:f.containers)container.clear();f.q={};f.vertices.reset();f.srt.reset();
    c.caller={};c.copies={};c.lease={};c.inventory={};
    check(p.inputs().buffers[1].bytes&&*p.inputs().buffers[1].bytes==proof_bytes,"proof outlives mutated source aliases/request");
    check(p.inventory().stage(ShaderStage::geometry).container==proof_container,"proof owns container storage after caller destruction");
    check(c.frozen.snapshot->resources==saved_resources&&c.stereo.batch->eyes==saved_eyes,"snapshot/eye ownership outlives provider and input storage");
    auto again=acz::stereo_payload::prepare(c.frozen.snapshot,eyes(),screen());check(again&&again.batch->eyes==saved_eyes,"owned snapshot independently feeds both eyes after destruction");
}
void removals(const std::array<std::vector<uint8_t>,3>& real) {
    for(int link=0;link<2;++link) {
        Fixture f(real);if(link==0)f.q.pipeline.stages[0]={ShaderStage::vertex,StageState::unknown};else f.q.synthetic_indexing.reset();
        Chain c;drive(f,c);check(c.stop==Stop::proof&&!c.derived.proof,"removed proof link fails at derivation");
        exact(c.reason,link==0?"VS stage unknown; explicit absent assertion required":missing_index);unpublished(c);
    }
    for(int fault=1;fault<=4;++fault) {
        Fixture f(real);seed(f);Chain c;drive(f,c,fault);check(c.stop==Stop::provider&&bool(c.derived),"one missing provider assertion fails after proof");
        exact(c.reason,(fault==1||fault==4)?"source fence/version assertion missing":fault==2?"source mutation detected":"incomplete source copy");unpublished(c);
        auto d=enable::decide(c.facts(f));check(!d.enabled,"provider failure leaves decision disabled");exact(d.reason,"copy provider failed");
    }
    Fixture f(real);seed(f);Chain c;drive(f,c,0,true);
    check(c.stop==Stop::freeze&&!c.frozen.snapshot&&!c.stereo.batch&&!c.decision.enabled,"missing explicit manifest fails freeze before stereo");
    exact(c.reason,"SrtData absent from complete manifest");auto d=enable::decide(c.facts(f));
    check(!d.enabled,"missing manifest disabled");exact(d.reason,"manifest requirements absent or oversized");
    enable::Inputs omitted;check(!enable::decide(omitted).enabled,"default inputs disabled");
    omitted.opt_in=true;auto empty=enable::decide(omitted);check(!empty.enabled,"omitted pointers disabled");exact(empty.reason,"missing inputs");
    check(!input::assemble(f.observer,f.state,{}),"default provider input rejected");
    check(!acz::capture::freeze(State{},Command{},Inputs{}),"default freeze rejected");
    check(!acz::stereo_payload::prepare({},eyes(),screen()),"missing snapshot produces no eye batch");
}
}
int main(int argc,char** argv) {
    check(argc==2,"E10 path required");std::ifstream file(argv[1],std::ios::binary);check(bool(file),"E10 exists");
    std::string json((std::istreambuf_iterator<char>(file)),{});
    check(sha(reinterpret_cast<const uint8_t*>(json.data()),json.size())=="17f9a48e3d57be77a6b3bcac350573deaffddd2c2daf7388984f4f27f1e41b55","E10 SHA256");
    const std::array<std::vector<uint8_t>,3> real{blob(json,"adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd"),
        blob(json,"7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd"),blob(json,"847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da")};
    real_failures(real);evidence_rejections(real);positive_and_links(real);removals(real);
    std::cout<<checks<<" CPU/fake-COM capture-chain checks passed; real root constants fail closed; E11 indexing is pinned evidence; positive path SYNTHETIC ONLY\n";
}
