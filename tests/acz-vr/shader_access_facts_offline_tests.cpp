// CPU-only E10 identity/PSV0 checks and SDK root diagnostic. No shader execution.
#include "shader_access_facts.h"
#include <windows.h>
#include <d3d12.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace acz::pso;
using namespace acz::metadata;
namespace access=acz::shader_access;
namespace {unsigned checks=0;
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
constexpr const char* pins[]={
    "adeb9ef4ff8fda087f49f82e704863249180eb5b0f4798049d01ea78c9ce03dd",
    "7829809366c7ba65b43314a0c9234912e549a7fb213d5b36e8ddc26529da33dd",
    "847102becd00dd71720d193759f7a3fa7f571dca4b5f6f90ba12efc9c72512da"};
void exact(const access::Lookup& r,const char* why) {
    check(!r&&!r.record&&r.rejection==why,"lookup fails closed with exact reason");
}
void root_facts(const std::vector<uint8_t>& bytes,const std::vector<const access::Record*>& records) {
    ID3D12VersionedRootSignatureDeserializer* sdk=nullptr;
    check(SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(bytes.data(),bytes.size(),IID_PPV_ARGS(&sdk))),"SDK real root decode");
    const auto* desc=sdk->GetUnconvertedRootSignatureDesc();check(desc&&desc->Version==D3D_ROOT_SIGNATURE_VERSION_1_1,"root1.1");
    const auto& r=desc->Desc_1_1;
    static_assert(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT==1&&
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS==4&&D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS==8&&
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS==0x100&&D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS==0x200);
    check(r.Flags==0x30d&&r.NumParameters==4&&!r.NumStaticSamplers,"SDK literal flags/counts");
    std::cout<<"SDK real root v1.1 flags=0x30d (IA, deny HS/DS/AS/MS); four parameters, no static samplers\n";
    for(UINT i=0;i<2;++i) {
        const auto& p=r.pParameters[i];check(p.ParameterType==D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE&&
            p.ShaderVisibility==D3D12_SHADER_VISIBILITY_PIXEL&&p.DescriptorTable.NumDescriptorRanges==1,"PS single table");
        const auto& range=p.DescriptorTable.pDescriptorRanges[0];
        check(range.RangeType==(i?D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:D3D12_DESCRIPTOR_RANGE_TYPE_SRV)&&
            range.NumDescriptors==1&&range.BaseShaderRegister==0&&range.RegisterSpace==0&&range.Flags==0&&
            range.OffsetInDescriptorsFromTableStart==D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,"PS t0/s0 range facts");
    }
    const auto& srv=r.pParameters[2];check(srv.ParameterType==D3D12_ROOT_PARAMETER_TYPE_SRV&&srv.ShaderVisibility==0&&
        srv.Descriptor.ShaderRegister==10&&srv.Descriptor.RegisterSpace==0&&srv.Descriptor.Flags==0,"all-stage root SRV t10");
    const auto& cb=r.pParameters[3];check(cb.ParameterType==D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS&&cb.ShaderVisibility==0&&
        cb.Constants.ShaderRegister==10&&cb.Constants.RegisterSpace==0&&cb.Constants.Num32BitValues==2,"all-stage cb10 two DWORD constants");
    sdk->Release();
    const auto decoded=RootBindingMetadata::from_serialized(bytes.data(),bytes.size());check(decoded.valid(),"real root accepted");
    const auto& layout=decoded.metadata->layout();
    check(layout.flags==0x30d&&layout.parameters.size()==4&&layout.parameters[0].ranges[0].offset==0&&
        layout.parameters[1].ranges[0].offset==0,"owned root normalizes APPEND");
    for(const auto* record:records)for(size_t i=0;i<record->resource_count;++i) {
        const auto& resource=record->resources[i];
        const auto kind=resource.resource_class==access::ResourceClass::cbv?BindingKind::cbv:
            resource.resource_class==access::ResourceClass::sampler?BindingKind::sampler:BindingKind::srv;
        auto resolved=decoded.metadata->resolve(kind,resource.reg,resource.space,record->stage);
        check(resolved.valid,"every evidence resource resolves through real root");
        check(resolved.root_index==(kind==BindingKind::cbv?3:kind==BindingKind::sampler?1:resource.reg==10?2:0),"literal parameter index");
    }
}
void table_tests(const std::array<std::vector<uint8_t>,3>& bytes) {
    constexpr ShaderStage stages[]={ShaderStage::vertex,ShaderStage::pixel,ShaderStage::geometry};
    Request request;for(size_t i=0;i<3;++i)request.stages.push_back({stages[i],StageState::present,bytes[i].data(),bytes[i].size()});
    auto decoded=build(request);check(bool(decoded),"real metadata decode");
    std::vector<const access::Record*> records;
    for(size_t i=0;i<3;++i) {
        const auto& stage=decoded.inventory->stage(stages[i]);
        auto result=access::lookup(stage);check(bool(result)&&result.rejection.empty(),"exact real shader identity accepted");
        const auto& r=*result.record;records.push_back(&r);
        check(r.stage==stages[i]&&r.shader_hash==stage.shader.identifier&&std::string(r.container_sha256)==pins[i],"distinct shader HASH and container SHA identities");
        check(std::string(r.evidence_ids)=="e11-drawcpt-dxil; e10-drawcpt-pso","two evidence ids owned in static table");
        check(r.resource_count==(i==0?2:i==1?3:1)&&r.cbv_register_zero==(i!=2),"resource counts and CBV register-zero fact");
        check(r.cb10_consumed_words==(i==0?1u:i==1?3u:0u),"E11 VS x / PS xy / GS no cb10 consumed-word facts");
        auto copy=stage;copy.shader.identifier[0]^=1;exact(access::lookup(copy),"access facts: unknown shader hash");
        copy=stage;copy.container=bytes[(i+1)%3];exact(access::lookup(copy),"access facts: container SHA-256 mismatch");
        copy=stage;copy.container.back()^=1;exact(access::lookup(copy),"access facts: container SHA-256 mismatch");
        copy=stage;copy.container.clear();exact(access::lookup(copy),"access facts: container bytes missing/oversized");
        copy=stage;copy.shader.stage=ShaderStage::compute;exact(access::lookup(copy),"access facts: shader stage/metadata mismatch");
        copy=stage;copy.psv.reset();exact(access::lookup(copy),"access facts: PSV0 resource count mismatch");
        copy=stage;copy.psv->bindings.push_back(copy.psv->bindings[0]);exact(access::lookup(copy),"access facts: PSV0 resource count mismatch");
        copy=stage;copy.psv->bindings.pop_back();exact(access::lookup(copy),"access facts: PSV0 resource count mismatch");
        for(int field=0;field<6;++field) {
            copy=stage;auto& b=copy.psv->bindings[0];
            switch(field){case 0:++b.type;break;case 1:++b.space;break;case 2:++b.lower;break;case 3:++b.upper;break;case 4:++*b.kind;break;case 5:++*b.flags;break;}
            exact(access::lookup(copy),"access facts: PSV0 register/space/class/count mismatch");
        }
        copy=stage;std::reverse(copy.psv->bindings.begin(),copy.psv->bindings.end());check(bool(access::lookup(copy)),"binding multiset is order independent");
        if(i==1) {
            check(!r.srt,"PS contributes no SrtData access");
            copy=stage;copy.psv->bindings[2]={5,0,10,10,12,0};
            exact(access::lookup(copy),"access facts: PSV0 register/space/class/count mismatch");
        } else {
            check(r.srt&&r.srt->reg==10&&!r.srt->space&&!r.srt->element&&r.srt->element_size==416,"literal t10/space0 constant-zero/416 facts");
            const std::vector<std::pair<uint32_t,uint32_t>> expected=i==0?
                std::vector<std::pair<uint32_t,uint32_t>>{{128,176},{192,304},{352,368}}:
                std::vector<std::pair<uint32_t,uint32_t>>{{0,128},{400,416}};
            check(r.srt->count==expected.size(),"E11 range count");
            for(size_t j=0;j<expected.size();++j)check(r.srt->ranges[j].begin==expected[j].first&&r.srt->ranges[j].end==expected[j].second,"E11 conservative half-open read ranges");
            acz::capture::Descriptor d{BindingKind::srv,{1,0,3*416,416},2,1,true,true};
            check(access::validate_srt(r,d,3*416).empty(),"FirstElement plus constant zero fits exact owned boundary");
            for(uint32_t count:{1u,2u,4096u})for(uint32_t start:{0u,19u,UINT32_MAX}) {
                const auto indexed=access::index_srt(r,d,3*416,{false,3,count,0,0,start});
                check(indexed&&indexed.elements==std::vector<uint64_t>(count,0),"every instance reads zero independent of StartInstance");
            }
            for(uint32_t count:{0u,4097u}) {
                const auto indexed=access::index_srt(r,d,3*416,{false,3,count,0,0,0});
                check(!indexed&&indexed.elements.empty()&&indexed.rejection=="access facts: empty/oversized draw arguments","index output bounded and atomic");
            }
            auto forged=r;auto invalid=access::index_srt(forged,d,3*416,{false,3,2,0,0,0});
            check(!invalid&&invalid.rejection=="access facts: record is not lookup-owned","caller copies cannot supply evidence rules");
            auto bad=d;bad.elements=0;check(access::validate_srt(r,bad,3*416)=="access facts: constant SrtData element out of bounds","zero descriptor elements rejected");
            bad=d;bad.view.stride=432;check(access::validate_srt(r,bad,3*416)=="access facts: SrtData descriptor class/416-byte stride mismatch","stride must equal evidence");
            bad=d;bad.first_element=UINT64_MAX;check(access::validate_srt(r,bad,3*416)=="access facts: SrtData byte arithmetic overflow","checked element byte multiply");
            bad=d;bad.elements=2;check(access::validate_srt(r,bad,3*416)=="access facts: descriptor element extent exceeds resource","whole descriptor extent checked without overflow");
            bad=d;bad.view.offset=UINT64_MAX;check(access::validate_srt(r,bad,3*416)=="access facts: SrtData view exceeds owned resource","checked view offset");
            bad=d;--bad.view.size;check(access::validate_srt(r,bad,3*416)=="access facts: constant SrtData element exceeds descriptor/resource","complete element one byte short");
            check(access::validate_srt(r,d,3*416-1)=="access facts: SrtData view exceeds owned resource","resource one byte short");
        }
    }
    root_facts(bytes[0],records);
    // Returned records survive destruction of every caller-owned container/metadata.
    decoded={};for(const auto* r:records)check(access::known_hash(r->shader_hash)&&std::strlen(r->container_sha256)==64,"static ownership after inventory destruction");
}
}
int main(int argc,char** argv) {
    check(argc==2,"E10 path required");std::ifstream file(argv[1],std::ios::binary);check(bool(file),"E10 exists");
    std::string json((std::istreambuf_iterator<char>(file)),{});
    check(sha(reinterpret_cast<const uint8_t*>(json.data()),json.size())=="17f9a48e3d57be77a6b3bcac350573deaffddd2c2daf7388984f4f27f1e41b55","E10 JSON pin");
    std::array<std::vector<uint8_t>,3> bytes;for(size_t i=0;i<3;++i)bytes[i]=blob(json,pins[i]);
    table_tests(bytes);
    std::cout<<checks<<" CPU access-fact checks passed; evidence indexing only, no complete capture/VR proof\n";
}
