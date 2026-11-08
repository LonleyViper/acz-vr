#include "pso_stage_inventory.h"
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
void sig(const SignatureElement& e,const char* name,uint32_t ix,uint32_t system,uint32_t reg,uint8_t mask,uint8_t rw) {
    check(e.semantic==name&&e.index==ix&&e.system_value==system&&e.component_type==3&&e.reg==reg&&e.mask==mask&&e.read_write_mask==rw&&e.stream==0&&e.min_precision==0,"literal ISG1/OSG1 record");
}
void binding(const ResourceBinding& b,uint32_t type,uint32_t reg,uint32_t kind){check(b.type==type&&b.space==0&&b.lower==reg&&b.upper==reg&&b.kind==kind&&b.flags==0,"literal PSV binding");}
void psig(const PsvElement& e,const char* name,uint32_t ix,uint8_t row,uint8_t columns,uint8_t kind,uint8_t interpolation) {
    check(e.semantic==name&&e.indices==std::vector<uint32_t>{ix}&&e.rows==1&&e.start_row==row&&e.cols_and_start==uint8_t(0x40|columns)&&e.semantic_kind==kind&&e.component_type==3&&e.interpolation==interpolation&&e.dynamic_mask_and_stream==0,"literal PSV signature record");
}
void real(const Inventory& inventory) {
    const auto& vs=inventory.stage(ShaderStage::vertex);const auto& ps=inventory.stage(ShaderStage::pixel);const auto& gs=inventory.stage(ShaderStage::geometry);
    const Stage* stages[]={&vs,&ps,&gs};const uint8_t kinds[]={1,0,2};const size_t counts[]={2,3,1};
    for(size_t i=0;i<3;++i) {
        const auto& s=*stages[i];check(s.state==StageState::present&&s.shader.valid&&s.psv&&s.input&&s.output&&s.rts0&&s.root,"real parts present");
        const auto& p=*s.psv;
        check(p.version==3&&p.runtime_size==52&&p.binding_stride==24&&p.bindings.size()==counts[i]&&p.shader_stage==kinds[i]&&p.uses_view_id==0,"literal PSV version/stage/binding count");
        check(p.minimum_wave==0&&p.maximum_wave==0xffffffffu&&p.threads==std::array<uint32_t,3>{0,0,0},"literal runtime wave/thread fields");
        check(p.entry=="main","literal version3 entry name");
        check(s.rts0->size()==160&&sha(s.rts0->data(),s.rts0->size())=="e710e76d70a18fdb9147b303a7d8b4cf83dc3d80b70069149edf0d81a70b86c5","RTS0 pin all stages");
        std::cout<<"Full root stage "<<unsigned(kinds[i])<<": failure "<<unsigned(s.root->failure)<<", HRESULT "<<s.root->sdk_hresult<<", detail '"<<s.root->detail<<"'\n";
        // E10's SDK flags 0x30d allow IA and deny HS/DS/AS/MS access.
        // They preserve VS/PS/GS binding resolution in the bounded adapter.
        check(s.root->valid()&&s.root->detail.empty()&&s.root->sdk_hresult==0,"full E10 SDK description accepted");
        const auto& root=s.root->metadata->layout();
        using namespace acz::metadata;
        check(root.version==RootVersion::v1_1&&root.flags==0x30d&&root.static_sampler_count==0&&root.parameters.size()==4,"literal E10 root version/flags/counts");
        for(size_t j=0;j<2;++j) {
            const auto& table=root.parameters[j];
            check(table.kind==ParameterKind::table&&table.visibility==Visibility::pixel&&table.shader_register==0&&table.space==0&&table.flags==0&&table.constant_count==0&&table.ranges.size()==1,"literal pixel-only E10 table parameters");
            const auto& range=table.ranges[0];
            check(range.kind==(j?BindingKind::sampler:BindingKind::srv)&&range.count==1&&range.base_register==0&&range.space==0&&range.flags==0&&range.offset==0,"literal E10 t0/s0 ranges with normalized APPEND");
        }
        const auto& srv=root.parameters[2];
        check(srv.kind==ParameterKind::srv&&srv.visibility==Visibility::all&&srv.shader_register==10&&srv.space==0&&srv.flags==0&&srv.constant_count==0&&srv.ranges.empty(),"literal all-stage E10 root SRV t10");
        const auto& cb=root.parameters[3];
        check(cb.kind==ParameterKind::constants&&cb.visibility==Visibility::all&&cb.shader_register==10&&cb.space==0&&cb.flags==0&&cb.constant_count==2&&cb.ranges.empty(),"literal all-stage E10 cb10 two DWORD constants");
        auto bare=acz::metadata::RootBindingMetadata::from_serialized(s.rts0->data(),s.rts0->size());
        check(bare.failure==SerializedRootFailure::sdk_rejected&&bare.sdk_hresult==int32_t(0x80070057u)&&!bare.metadata,"bare RTS0 SDK rejects E_INVALIDARG");
        auto original=acz::metadata::RootBindingMetadata::from_serialized(s.container.data(),s.container.size());
        check(original.failure==s.root->failure&&original.detail==s.root->detail&&original.sdk_hresult==s.root->sdk_hresult,"full container reader matches SDK seam");
        std::cout<<"E10 "<<unsigned(kinds[i])<<": PSV v3, bindings "<<counts[i]<<", entry '"<<*p.entry<<"', dependencies "<<p.dependencies.size()<<", RTS0 SDK failure "<<unsigned(s.root->failure)<<", HRESULT "<<s.root->sdk_hresult<<'\n';
    }
    check(vs.input->size()==4&&vs.output->size()==2,"VS signature counts");
    for(uint32_t i=0;i<4;++i)sig((*vs.input)[i],"ATTRIBUTE",i,0,i,15,i==3?3:7);
    sig((*vs.output)[0],"POSITION",0,0,0,15,0);sig((*vs.output)[1],"COLOR",0,0,1,15,0);
    check(ps.input->size()==3&&ps.output->size()==2,"PS signature counts");
    sig((*ps.input)[0],"SV_Position",0,1,0,15,0);sig((*ps.input)[1],"COLOR",0,0,1,7,7);sig((*ps.input)[2],"TEXCOORD",0,0,2,3,3);
    for(uint32_t i=0;i<2;++i)sig((*ps.output)[i],"SV_Target",i,64,i,15,0);
    check(gs.input->size()==2&&gs.output->size()==3,"GS signature counts");
    sig((*gs.input)[0],"POSITION",0,0,0,15,15);sig((*gs.input)[1],"COLOR",0,0,1,15,15);
    sig((*gs.output)[0],"SV_Position",0,1,0,15,0);sig((*gs.output)[1],"COLOR",0,0,1,7,8);sig((*gs.output)[2],"TEXCOORD",0,0,2,3,12);
    binding(vs.psv->bindings[0],2,10,13);binding(vs.psv->bindings[1],5,10,12);
    binding(ps.psv->bindings[0],2,10,13);binding(ps.psv->bindings[1],1,0,14);binding(ps.psv->bindings[2],3,0,2);
    binding(gs.psv->bindings[0],5,10,12); // structured SRV t10, space0; PSV has no SrtData name/index expression
    check(vs.psv->signatures[0].size()==4&&vs.psv->signatures[1].size()==2&&vs.psv->signatures[2].empty(),"VS PSV signature counts");
    check(ps.psv->signatures[0].size()==3&&ps.psv->signatures[1].size()==2&&ps.psv->signatures[2].empty(),"PS PSV signature counts");
    check(gs.psv->signatures[0].size()==2&&gs.psv->signatures[1].size()==3&&gs.psv->signatures[2].empty(),"GS PSV signature counts");
    check(vs.psv->input_vectors==4&&vs.psv->output_vectors[0]==2&&ps.psv->input_vectors==3&&ps.psv->output_vectors[0]==2&&gs.psv->input_vectors==2&&gs.psv->output_vectors[0]==3,"literal PSV vector counts");
    for(const auto* s:stages)check(s->psv->dependencies.size()==1,"single input/output dependency table");
    for(uint32_t i=0;i<4;++i)psig(vs.psv->signatures[0][i],"ATTRIBUTE",i,uint8_t(i),4,0,0);
    psig(vs.psv->signatures[1][0],"POSITION",0,0,4,0,2);psig(vs.psv->signatures[1][1],"COLOR",0,1,4,0,2);
    psig(ps.psv->signatures[0][0],"",0,0,4,3,4);psig(ps.psv->signatures[0][1],"COLOR",0,1,3,0,2);psig(ps.psv->signatures[0][2],"TEXCOORD",0,2,2,0,2);
    for(uint32_t i=0;i<2;++i)psig(ps.psv->signatures[1][i],"",i,uint8_t(i),4,16,0);
    psig(gs.psv->signatures[0][0],"POSITION",0,0,4,0,2);psig(gs.psv->signatures[0][1],"COLOR",0,1,4,0,2);
    psig(gs.psv->signatures[1][0],"",0,0,4,3,4);psig(gs.psv->signatures[1][1],"COLOR",0,1,3,0,2);psig(gs.psv->signatures[1][2],"TEXCOORD",0,2,2,0,2);
    check(vs.psv->dependencies[0].words==std::vector<uint32_t>{1,2,4,0,248,248,248,0,16,32,64,0,8,128,0,0},"VS literal dependency words");
    check(ps.psv->dependencies[0].words==std::vector<uint32_t>{0,0,0,0,1,2,4,0,143,143,0,0},"PS literal dependency words");
    check(gs.psv->dependencies[0].words==std::vector<uint32_t>{895,895,895,256,16,32,64,512},"GS literal dependency words");
    check(vs.psv->stage_info==std::array<uint8_t,16>{}&&ps.psv->stage_info==std::array<uint8_t,16>{},"VS/PS runtime union zero bytes");
    check(gs.psv->stage_info==std::array<uint8_t,16>{3,0,0,0,5,0,0,0,1,0,0,0,1,0,0,0}&&gs.psv->max_vertices_or_patch_vectors==9,"GS runtime union primitive/topology/stream/position and maxvertices");
    check(vs.psv->runtime_stage.output_position==0&&!vs.psv->runtime_stage.depth_output&&ps.psv->runtime_stage.depth_output==0&&ps.psv->runtime_stage.sample_frequency==0,"VS/PS union selected literal fields");
    check(gs.psv->runtime_stage.input_primitive==3&&gs.psv->runtime_stage.output_topology==5&&gs.psv->runtime_stage.output_stream_mask==1&&gs.psv->runtime_stage.output_position==1&&!gs.psv->runtime_stage.input_control_points,"GS decoded union fields");
}
void malformed(const std::vector<uint8_t>& b,ShaderStage stage) {
    const auto pv=part(b,"PSV0"),is=part(b,"ISG1"),os=part(b,"OSG1");
    // Every proper prefix of all real containers is checked (bounded by pinned lengths).
    for(size_t n=0;n<b.size();++n){auto q=one(b,stage);q.stages[0].size=n;auto r=build(q);check(!r&&!r.inventory,"truncation prefix sweep");}
    auto change=[&](size_t off,uint32_t value,const char* why){auto c=b;put(c,off,value);reject(c,stage,why);};
    change(0,0,"magic");change(24,0,"size mismatch");change(28,0xffffffffu,"part count");
    change(32,0xfffffffcu,"part offset");change(is+4,0xffffffffu,"overflowing part");
    change(36,u(b,32),"overlapping parts");
    change(os,0x31475349u,"duplicate container part");
    change(is+8,0xffffffffu,"signature element count");change(is+12,12,"unsupported signature ParamOffset");
    change(is+20,0xffffffffu,"signature semantic string");
    change(pv+8,53,"unsupported PSV0 runtime-info size 53");
    change(pv+8,56,"unsupported PSV0 runtime-info size 56"); // PSVRuntimeInfo4 unsupported, no size-prefix assumption
    change(pv+8+56,0xffffffffu,"resource count/stride overflow");
    change(pv+8+60,20,"resource binding stride");
    const size_t strings=pv+8+64+24*u(b,pv+8+56),stringn=u(b,strings);
    change(strings,3,"string table alignment");change(strings+4+stringn,0xffffffffu,"semantic index table overflow");
    const size_t semcount=strings+4+stringn, sigstride=semcount+4+4*u(b,semcount);
    change(sigstride,20,"signature element stride");change(sigstride+4,0xffffffffu,"signature semantic offset");
    change(sigstride+8,0xffffffffu,"semantic index bounds");
    change(pv+8+4+48,0xffffffffu,"entry string offset");
    auto no_null=b;no_null[strings+3+stringn]='x';reject(no_null,stage,"string table missing final null");
    auto mismatch=b;mismatch[pv+8+4+24]=5;reject(mismatch,stage,"PSV0 shader stage mismatch");
    auto short_table=b;put(short_table,pv+4,u(b,pv+4)-4);reject(short_table,stage,"input-to-output table stream 0");
    // Include aligned extra bytes INSIDE PSV0, shifting later parts and container size.
    auto c=b;const size_t tail=pv+8+u(b,pv+4);c.insert(c.begin()+tail,4,0);put(c,24,uint32_t(c.size()));put(c,pv+4,u(b,pv+4)+4);
    for(size_t i=0;i<u(b,28);++i){const auto off=u(b,32+4*i);if(off>=tail)put(c,32+4*i,off+4);}reject(c,stage,"unexpected trailing bytes/table");
    // Missing optional parts are literal absence, never default metadata.
    for(const auto tag:{"PSV0","ISG1","OSG1","RTS0"}) {
        c=b;const auto off=part(b,tag);std::memcpy(c.data()+off,"NONE",4);
        auto r=build(one(c,stage));check(bool(r),"optional part absence accepted");const auto& s=r.inventory->stage(stage);
        if(!std::strcmp(tag,"PSV0"))check(!s.psv,"missing PSV0 absent");
        if(!std::strcmp(tag,"ISG1"))check(!s.input,"missing ISG1 absent");
        if(!std::strcmp(tag,"OSG1"))check(!s.output,"missing OSG1 absent");
        if(!std::strcmp(tag,"RTS0"))check(!s.rts0&&!s.root,"missing RTS0 no decoder facts");
    }
}

// J-043: deterministic, no PRNG. Dense metadata bytes use XOR 01/ff;
// remaining opaque bytes use offsets 0,97,194,... (excluding dense bytes).
// Boundary dwords: 0,1,blob size,size+1,7fffffff,ffffffff,fffffffc.
// Print exact category counts, including all 256 possible appended bytes.
void root_outcome(const acz::metadata::SerializedRootResult& d) {
    using F=SerializedRootFailure;
    check(d.failure!=F::exception_failure && d.failure!=F::allocation_failure,
          "root sweep has no exception/allocation failure");
    if(d.valid()) {
        check(d.failure==F::none && d.detail.empty() && d.sdk_hresult==0 &&
              d.metadata->valid(),"complete root success");
        const auto& l=d.metadata->layout();
        check(l.parameters.size()<=64,"root owned parameter budget");
        for(const auto& p:l.parameters)check(p.ranges.size()<=64,"root owned range budget");
    } else {
        check(!d.metadata && d.failure!=F::none,"root failure publishes nothing");
        // Public seam deliberately has no SDK parse string: HRESULT is its
        // precise diagnostic. Only unsupported_layout has an adapter detail.
        if(d.failure==F::sdk_rejected)check(d.sdk_hresult<0,"literal SDK rejection HRESULT");
        if(d.failure==F::unsupported_layout)check(!d.detail.empty(),"exact adapter rejection detail");
    }
}
void consistent(const Stage& s,ShaderStage stage) {
    const auto& b=s.container;
    check(s.state==StageState::present && s.shader.valid &&
          s.shader.stage==stage,"owned accepted stage");
    check(b.size()>=32 && u(b,24)==b.size() && u(b,28)<=(b.size()-32)/4,
          "owned container header/table counts");
    check(std::equal(s.shader.container_checksum.begin(),s.shader.container_checksum.end(),b.begin()+4),
          "owned checksum equals container");
    bool pv=false,is=false,os=false,rt=false;
    for(size_t i=0;i<u(b,28);++i) {
        const size_t o=u(b,32+4*i);
        check(o>=32+4*u(b,28) && o<=b.size() && b.size()-o>=8,"owned part header span");
        const size_t n=u(b,o+4),start=o+8;
        check(n<=b.size()-start,"owned part payload span");
        if(!std::memcmp(b.data()+o,"ISG1",4)||!std::memcmp(b.data()+o,"OSG1",4)) {
            const bool input=!std::memcmp(b.data()+o,"ISG1",4);
            const auto& sigs=input?s.input:s.output;
            if(input)is=true;else os=true;
            check(sigs && n>=8 && u(b,start)==sigs->size() &&
                  sigs->size()<=(n-8)/32,"owned signature count matches record table");
            for(size_t j=0;j<sigs->size();++j) {
                const auto& e=(*sigs)[j];const size_t rec=start+8+32*j,name=u(b,rec+4);
                check(name<n && e.semantic.size()<n-name &&
                      std::memcmp(e.semantic.c_str(),b.data()+start+name,e.semantic.size()+1)==0,
                      "owned signature semantic span/value");
            }
        } else if(!std::memcmp(b.data()+o,"PSV0",4)) {
            pv=true;check(s.psv.has_value(),"owned PSV publication");
            const auto& v=*s.psv;
            check(v.runtime_size+8<=n && u(b,start)==v.runtime_size &&
                  u(b,start+4+v.runtime_size)==v.bindings.size(),"owned PSV resource count");
            size_t at=start+8+v.runtime_size;
            if(!v.bindings.empty()) {
                check(u(b,at)==v.binding_stride,"owned binding stride");
                at+=4+v.binding_stride*v.bindings.size();
            }
            check(at<=start+n,"owned binding span");
            if(v.version) {
                check(u(b,at)==v.string_table.size(),"owned PSV string count");
                at+=4;check(v.string_table.size()<=start+n-at &&
                    std::equal(v.string_table.begin(),v.string_table.end(),b.begin()+at),"owned PSV string span/value");
                at+=v.string_table.size();check(u(b,at)==v.semantic_indices.size(),"owned semantic index count");
                at+=4;
                for(auto ix:v.semantic_indices){check(u(b,at)==ix,"owned semantic index value");at+=4;}
                size_t total=0;
                for(size_t group=0;group<3;++group) {
                    check(v.signatures[group].size()==b[start+4+28+group],"owned PSV signature counts");
                    total+=v.signatures[group].size();
                    for(const auto& e:v.signatures[group])check(e.rows==e.indices.size(),"owned PSV row/index counts");
                }
                if(total){check(u(b,at)==16,"owned PSV signature stride");at+=4+16*total;}
                for(const auto& table:v.dependencies)for(auto w:table.words) {
                    check(u(b,at)==w,"owned dependency word/span");at+=4;
                }
                check(at==start+n,"owned PSV consumed tables match payload");
            } else check(at==start+n,"owned PSV0 resource table span");
        } else if(!std::memcmp(b.data()+o,"RTS0",4)) {
            rt=true;check(s.rts0 && s.root && s.rts0->size()==n &&
                std::equal(s.rts0->begin(),s.rts0->end(),b.begin()+start),"owned root payload span/value");
            root_outcome(*s.root);
        }
    }
    check(pv==bool(s.psv) && is==bool(s.input) && os==bool(s.output) &&
          rt==bool(s.rts0) && rt==bool(s.root),"optional parts match owned table");
}
void mutation_sweep(const std::vector<uint8_t>& b,ShaderStage stage) {
    const auto before=sha(b.data(),b.size());
    size_t accepted=0,rejected_count=0,root_accepted=0,total=0;
    auto run=[&](std::vector<uint8_t> c) {
        ++total;
        auto r=build(one(c,stage));
        auto root=acz::metadata::RootBindingMetadata::from_serialized(c.data(),c.size());
        root_outcome(root);
        // A deep value snapshot, retained only by the test, is compared after free.
        const auto root_copy=root.metadata;
        if(root.valid())++root_accepted;
        if(r) {
            ++accepted;check(r.failure==Failure::none && r.rejection.empty(),"sweep complete inventory success");
            check(r.inventory->stage(stage).container==c &&
                  r.inventory->stage(stage).container.data()!=c.data(),"sweep exact independent container copy");
            consistent(r.inventory->stage(stage),stage);
            for(const auto& other:r.inventory->stages)if(&other!=&r.inventory->stage(stage))
                check(other.state==StageState::unknown && other.container.empty() &&
                      !other.psv && !other.input && !other.output && !other.rts0 && !other.root,
                      "sweep omitted stages have no partial facts");
        } else {
            ++rejected_count;check(!r.inventory && r.failure==Failure::malformed &&
                !r.rejection.empty(),"sweep precise rejection without partial inventory");
        }
        std::fill(c.begin(),c.end(),0);c.clear();c.shrink_to_fit();
        if(r)consistent(r.inventory->stage(stage),stage);
        if(root_copy) {
            const auto& old=root_copy->layout();const auto& now=root.metadata->layout();
            check(old.parameters.size()==now.parameters.size() && old.flags==now.flags,
                  "full-container root independent after input free");
            check(root.metadata->valid(),"owned full-container root remains valid");
        }
    };
    std::vector<bool> dense(b.size(),false);
    std::vector<size_t> fields={20,24,28};
    auto region=[&](size_t start,size_t n) {
        check(start<=b.size() && n<=b.size()-start,"sweep original region bounds");
        std::fill(dense.begin()+start,dense.begin()+start+n,true);
    };
    region(0,32+4*u(b,28));
    for(size_t i=0;i<u(b,28);++i) {
        fields.push_back(32+4*i);const size_t o=u(b,32+4*i),p=o+8,n=u(b,o+4);
        region(o,8);fields.push_back(o+4);
        if(!std::memcmp(b.data()+o,"ISG1",4)||!std::memcmp(b.data()+o,"OSG1",4)) {
            region(p,n);fields.push_back(p);fields.push_back(p+4);
            for(size_t j=0;j<u(b,p);++j)fields.push_back(p+8+32*j+4);
        } else if(!std::memcmp(b.data()+o,"PSV0",4)) {
            region(p,n);
            fields.insert(fields.end(),{p,p+56,p+60,p+52});
            const size_t strings=p+64+24*u(b,p+56);
            const size_t indices=strings+4+u(b,strings);
            const size_t stride=indices+4+4*u(b,indices);
            fields.insert(fields.end(),{strings,indices,stride});
            const size_t records=size_t(b[p+32])+b[p+33]+b[p+34];
            for(size_t j=0;j<records;++j){fields.push_back(stride+4+16*j);fields.push_back(stride+8+16*j);}
        } else if(!std::memcmp(b.data()+o,"RTS0",4)) {
            region(p,n);
            // Opaque SDK input: every aligned word, no root-format claims.
            for(size_t j=0;j+4<=n;j+=4)fields.push_back(p+j);
        } else if(!std::memcmp(b.data()+o,"DXIL",4)) {
            region(p,24);fields.insert(fields.end(),{p+4,p+16,p+20});
        }
    }
    std::sort(fields.begin(),fields.end());fields.erase(std::unique(fields.begin(),fields.end()),fields.end());
    size_t flips=0,words=0;
    for(size_t i=0;i<b.size();++i)if(dense[i]||i%97==0)
        for(uint8_t mask:{uint8_t(1),uint8_t(255)}){auto c=b;c[i]^=mask;run(std::move(c));++flips;}
    for(auto at:fields)
        for(uint32_t value:{0u,1u,uint32_t(b.size()),uint32_t(b.size()+1),0x7fffffffu,0xffffffffu,0xfffffffcu}) {
            auto c=b;put(c,at,value);run(std::move(c));++words;
        }
    // PSV packed uint8/uint16 counts/vector lengths (RuntimeInfo1) also
    // get boundaries, narrowed to their actual field widths.
    const size_t pv=part(b,"PSV0")+8;
    for(size_t at=pv+30;at<pv+40;++at)
        for(unsigned value:{0u,1u,unsigned(b.size()),unsigned(b.size()+1),0x7fu,0xffu,0xfcu}) {
            auto c=b;c[at]=uint8_t(value);run(std::move(c));++words;
        }
    for(unsigned value:{0u,1u,unsigned(b.size()),unsigned(b.size()+1),0x7fffu,0xffffu,0xfffcu}) {
        auto c=b;c[pv+30]=uint8_t(value);c[pv+31]=uint8_t(value>>8);run(std::move(c));++words;
    }
    for(size_t n=0;n<b.size();++n)run({b.begin(),b.begin()+n});
    for(unsigned value=0;value<256;++value){auto c=b;c.push_back(uint8_t(value));run(std::move(c));}
    size_t permutations=0;
    for(size_t i=1;i<u(b,28);++i) {
        auto c=b;put(c,32,u(b,32+4*i));put(c,32+4*i,u(b,32));run(std::move(c));++permutations;
        c=b;put(c,32+4*i,u(b,32));run(std::move(c));++permutations;
    }
    check(total==flips+words+b.size()+256+permutations,"exact bounded sweep count");
    check(accepted && rejected_count,"sweep covers both outcomes");
    check(sha(b.data(),b.size())==before,"original container reverified after mutations");
    std::cout<<"J-043 stage "<<unsigned(stage)<<": stride 97; flips "<<flips
        <<", boundaries "<<words<<", prefixes "<<b.size()<<", extensions 256, reorder/duplicate "
        <<permutations<<", total "<<total<<"; accepted "<<accepted<<", rejected "<<rejected_count
        <<", root accepted "<<root_accepted<<"\n";
}

void versions(const std::vector<uint8_t>& b) {
    const size_t o=part(b,"PSV0"),p=o+8, count=u(b,p+56),tail=p+64+24*count;
    for(uint32_t version=0;version<3;++version) {
        const uint32_t runtime=version==0?24:version==1?36:48;
        std::vector<uint8_t> payload(4);put(payload,0,runtime);
        payload.insert(payload.end(),b.begin()+p+4,b.begin()+p+4+runtime);
        const size_t count_at=payload.size();payload.resize(count_at+8);put(payload,count_at,count);
        const uint32_t stride=version<2?16:24;put(payload,count_at+4,stride);
        for(size_t i=0;i<count;++i)payload.insert(payload.end(),b.begin()+p+64+24*i,b.begin()+p+64+24*i+stride);
        if(version)payload.insert(payload.end(),b.begin()+tail,b.begin()+p+u(b,o+4));
        auto c=replace_psv(b,payload);auto result=build(one(c));
        check(bool(result),"synthetic runtime0/1/2 fixture accepted");const auto& v=*result.inventory->stage(ShaderStage::vertex).psv;
        check(v.version==version&&v.runtime_size==runtime&&v.binding_stride==stride&&!v.entry&&v.bindings.size()==2,"runtime version selected by SIZE, absent fields remain absent");
        if(version<2)check(!v.bindings[0].kind&&!v.bindings[0].flags,"16-byte PSV binding has no kind/flags");
        check(version==2?v.threads.has_value():!v.threads,"threads absent before runtime2");
        if(!version)check(!v.shader_stage&&!v.uses_view_id&&v.signatures[0].empty()&&v.dependencies.empty(),"runtime0 has no version1 tables");
        else check(v.signatures[0].size()==4&&v.dependencies[0].words.size()==16,"version1/2 defined tables consumed");
        auto extra=payload;extra.insert(extra.end(),4,0);reject(replace_psv(b,extra),ShaderStage::vertex,"unexpected trailing bytes/table");
    }
    // UsesViewID adds its mask BEFORE the input/output dependency table.
    std::vector<uint8_t> payload(b.begin()+p,b.begin()+p+u(b,o+4));
    payload[4+25]=1;payload.insert(payload.end()-64,{0x03,0,0,0});
    auto view=build(one(replace_psv(b,payload)));
    check(view&&view.inventory->stage(ShaderStage::vertex).psv->dependencies.size()==2,"ViewID mask table present");
    const auto& deps=view.inventory->stage(ShaderStage::vertex).psv->dependencies;
    check(deps[0].name=="PSV0 ViewID output mask stream 0"&&deps[0].words==std::vector<uint32_t>{3}&&deps[1].words.size()==16,"literal ViewID mask/remaining dependency table");
    // Resource count zero has NO binding-stride word.
    payload.assign(32,0);put(payload,0,24);auto empty=build(one(replace_psv(b,payload)));
    check(empty&&empty.inventory->stage(ShaderStage::vertex).psv->bindings.empty()&&empty.inventory->stage(ShaderStage::vertex).psv->binding_stride==0,"zero resource count omits binding stride");
    // Synthetic HS/DS metadata exercises the two extra defined dependency
    // tables. Program header stage is changed; opaque bitcode/HASH is neither
    // authenticated nor interpreted. This does not create executable shaders.
    for(const auto stage:{ShaderStage::hull,ShaderStage::domain}) {
        payload.assign(b.begin()+p,b.begin()+p+u(b,o+4));
        payload[4+24]=uint8_t(stage);payload[4+26]=1;payload[4+27]=0;
        const size_t words=stage==ShaderStage::hull?16:4;
        payload.insert(payload.end(),words*4,0);
        auto c=replace_psv(b,payload);const auto program=part(c,"DXIL")+8;
        put(c,program,(uint32_t(stage)<<16)|0x60);
        auto parsed=build(one(c,stage));check(bool(parsed),"synthetic HS/DS dependency tables accepted");
        const auto& v=*parsed.inventory->stage(stage).psv;
        check(v.dependencies.size()==2&&v.dependencies.back().words==std::vector<uint32_t>(words,0),"HS/DS table size and literal words");
        check(v.dependencies.back().name==(stage==ShaderStage::hull?"PSV0 input-to-patch table":"PSV0 patch-to-output table"),"HS/DS precise table name");
        check(v.runtime_stage.input_control_points==0&&v.runtime_stage.tessellator_domain==0,"HS/DS runtime union decoded");
        put(c,part(c,"PSV0")+4,u(c,part(c,"PSV0")+4)-4);
        reject(c,stage,stage==ShaderStage::hull?"input-to-patch table":"patch-to-output table");
    }
}
}
int main(int argc,char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    check(argc==2,"required evidence path argument");std::ifstream file(argv[1],std::ios::binary);check(bool(file),"E10 evidence file must exist");
    const std::string json((std::istreambuf_iterator<char>(file)),{});
    check(sha(reinterpret_cast<const uint8_t*>("abc"),3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","CNG known SHA vector");
    check(sha(reinterpret_cast<const uint8_t*>(json.data()),json.size())=="17f9a48e3d57be77a6b3bcac350573deaffddd2c2daf7388984f4f27f1e41b55","entire E10 JSON pin");
    auto vs=blob(json,"adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd");
    auto ps=blob(json,"7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd");
    auto gs=blob(json,"847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da");
    check(vs.size()==4268&&ps.size()==3160&&gs.size()==8964,"all pinned lengths");
    Request q=one(vs);q.stages.push_back({ShaderStage::pixel,StageState::present,ps.data(),ps.size()});q.stages.push_back({ShaderStage::geometry,StageState::present,gs.data(),gs.size()});
    q.stages.push_back({ShaderStage::domain,StageState::explicitly_absent});q.stages.push_back({ShaderStage::hull,StageState::explicitly_absent});
    for(uint32_t i=0;i<4;++i)q.input_layout.push_back({"ATTRIBUTE",i,2,0,16*i,0,0});
    q.root_provenance=RootProvenance::embedded_container;
    auto r=build(q);if(!r)std::cerr<<r.rejection<<'\n';check(bool(r),"real E10 inventory build");real(*r.inventory);
    check(r.inventory->stage(ShaderStage::domain).state==StageState::explicitly_absent&&r.inventory->stage(ShaderStage::hull).state==StageState::explicitly_absent,"explicit absent honoured");
    auto omitted=build(one(vs));check(omitted&&omitted.inventory->stage(ShaderStage::domain).state==StageState::unknown&&omitted.inventory->stage(ShaderStage::hull).state==StageState::unknown,"omitted unknown never absent");
    check(r.inventory->root_provenance==RootProvenance::embedded_container&&r.inventory->input_layout.size()==4,"root provenance and IA retained");
    auto bad=q;bad.stages[0].state=StageState::explicitly_absent;check(!build(bad),"absence with bytes rejected");
    bad=q;bad.stages[0].stage=ShaderStage::pixel;check(!build(bad),"stage container mismatch");
    bad=q;bad.stages.push_back(bad.stages[0]);check(!build(bad),"stage count budget");
    bad=one(vs);bad.stages.push_back(bad.stages[0]);check(!build(bad),"duplicate stage");
    bad=one(vs);bad.stages[0].state=StageState::unknown;check(!build(bad),"unknown with bytes rejected");
    bad=q;bad.root_provenance=RootProvenance::explicit_root;check(!build(bad),"explicit root needs caller identity");
    bad.explicit_root_identity=317;check(bool(build(bad)),"explicit root assertion retained");
    bad=one(vs);bad.root_provenance=RootProvenance::embedded_container;bad.embedded_root_stage=ShaderStage::pixel;check(!build(bad),"embedded unknown stage rejected");
    bad=q;bad.input_layout.resize(33);check(!build(bad),"IA count budget");
    bad=q;bad.input_layout[0].slot=32;check(!build(bad),"IA slot bounds");
    bad=q;bad.input_layout[0].semantic=std::string(256,'a');check(!build(bad),"IA semantic budget");
    bad=q;bad.input_layout[0].step_rate=1;check(!build(bad),"per-vertex step rate");
    malformed(vs,ShaderStage::vertex);malformed(ps,ShaderStage::pixel);malformed(gs,ShaderStage::geometry);
    mutation_sweep(vs,ShaderStage::vertex);mutation_sweep(ps,ShaderStage::pixel);mutation_sweep(gs,ShaderStage::geometry);
    versions(vs);
    bool success=false;size_t failures=0;
    for(long budget=0;budget<3000;++budget) {
        probe::remaining=budget;auto attempt=build(q);probe::remaining=-1;
        if(attempt){real(*attempt.inventory);success=true;break;}
        check(!attempt.inventory&&attempt.failure==Failure::allocation,"allocation sweep no partial publication");++failures;
    }
    check(success&&failures>50,"allocation sweep reaches complete publication");
    check(sha(reinterpret_cast<const uint8_t*>(json.data()),json.size())=="17f9a48e3d57be77a6b3bcac350573deaffddd2c2daf7388984f4f27f1e41b55","E10 JSON reverified after sweeps");
    check(sha(vs.data(),vs.size())=="adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd" &&
          sha(ps.data(),ps.size())=="7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd" &&
          sha(gs.data(),gs.size())=="847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da","all E10 pins reverified before lifetime test");
    q.input_layout[0].semantic="changed";std::fill(vs.begin(),vs.end(),0);ps.clear();ps.shrink_to_fit();gs.clear();gs.shrink_to_fit();
    real(*r.inventory);check(r.inventory->input_layout[0].semantic=="ATTRIBUTE","owned IA lifetime independence");
    std::cout<<checks<<" offline checks passed; literal metadata only, no runtime/indexing/VR proof\n";
}
