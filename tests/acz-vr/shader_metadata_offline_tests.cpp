#include "shader_metadata.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace acz::metadata;
namespace {
int checks=0;
void check(bool b,const char* label) { ++checks; if(!b) { std::cerr<<"FAIL: "<<label<<'\n'; std::exit(1); } }
void put(std::vector<uint8_t>& v,size_t at,uint32_t n) {
    for(int i=0;i<4;++i) v.at(at+i)=uint8_t(n>>(8*i));
}
void tag(std::vector<uint8_t>& v,size_t at,const char* s) { std::copy_n(s,4,v.begin()+at); }
// Hand-built structural fixture, NOT captured game bytecode. Its four-byte
// bitcode marker is deliberately not executable LLVM. Digest bytes are copied
// from the cited disassembly, not calculated from these synthetic bytes.
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> v(104);
    tag(v,0,"DXBC"); std::fill(v.begin()+4,v.begin()+20,uint8_t(0xaa));
    put(v,20,1); put(v,24,104); put(v,28,2); put(v,32,40); put(v,36,76);
    tag(v,40,"DXIL"); put(v,44,28); put(v,48,(2u<<16)|0x60);
    put(v,52,7); tag(v,56,"DXIL"); put(v,60,0x100); put(v,64,16); put(v,68,4);
    put(v,72,0xdec04342); tag(v,76,"HASH"); put(v,80,20); put(v,84,0);
    std::copy(drawcpt_geometry_identifier.begin(),drawcpt_geometry_identifier.end(),v.begin()+88);
    return v;
}
ShaderMetadata decode(const std::vector<uint8_t>& v) { return decode_shader_metadata(v.data(),v.size()); }
void rejected(const std::vector<uint8_t>& v,const char* label) {
    const auto m=decode(v); check(!m.valid && !m.rejection.empty() && !m.matches_drawcpt_geometry(),label);
}
}
int main() {
    auto v=fixture(); const auto m=decode(v);
    check(m.valid && m.rejection.empty() && m.matches_drawcpt_geometry(),"exact GS SM6.0 binary identifier");
    check(m.container_checksum!=m.identifier && m.container_checksum[0]==0xaa,"distinct checksum semantics");
    auto changed=v; changed[88]^=1; check(decode(changed).valid && !decode(changed).matches_drawcpt_geometry(),"identifier mismatch");
    for(uint32_t s=0;s<6;++s) {
        changed=v; put(changed,48,(s<<16)|0x60);
        check(decode(changed).valid && decode(changed).matches_drawcpt_geometry()==(s==2),"stage distinction");
    }
    // Conservative public contract: only SM6.0/DXIL1.0 is supported. These
    // synthetic headers discriminate the reviewed inconsistent-version bug.
    changed=v; put(changed,48,(2u<<16)|0x61);
    rejected(changed,"SM6.1 with DXIL1.0 rejected");
    put(changed,60,0x101); rejected(changed,"SM6.1 with DXIL1.1 unsupported");
    check(decode(changed).rejection=="unsupported shader model: only SM6.0 supported","explicit newer-model rejection");
    changed=v; put(changed,60,0x101); rejected(changed,"SM6.0 with DXIL1.1 rejected");
    for(uint32_t minor=1;minor<16;++minor) {
        changed=v; put(changed,48,(2u<<16)|0x60|minor); put(changed,60,0x100|minor);
        rejected(changed,"all unsupported minor versions fail closed");
    }
    changed=v; put(changed,84,1); check(decode(changed).valid && decode(changed).hash_semantics==HashSemantics::includes_source && !decode(changed).matches_drawcpt_geometry(),"source-inclusive hash conservatively unmatched");
    changed=v; std::fill(changed.begin()+4,changed.begin()+20,uint8_t(0)); check(decode(changed).matches_drawcpt_geometry(),"checksum not identifier");
    changed=v; std::fill(changed.begin()+88,changed.end(),uint8_t(0)); std::copy(drawcpt_geometry_identifier.begin(),drawcpt_geometry_identifier.end(),changed.begin()+4);
    check(!decode(changed).matches_drawcpt_geometry(),"header digest cannot impersonate HASH");
    for(size_t n=0;n<v.size();++n) {
        changed.assign(v.begin(),v.begin()+n); rejected(changed,"every container truncation");
    }
    check(!decode_shader_metadata(nullptr,104).valid,"null input");
    check(!decode_shader_metadata(v.data(),size_t(0x100000000ull)).valid,"oversized input rejected before read");
    struct Mutation { size_t at; uint32_t value; const char* why; };
    const Mutation mutations[]={
        {20,2,"container version"},{24,105,"declared total"},{28,0,"zero count"},{28,0xffffffff,"overflow count"},
        {32,0xffffffff,"overflow offset"},{32,41,"unaligned offset"},{32,36,"part in table"},
        {36,40,"duplicate/overlap part"},{36,100,"truncated part header"},{44,0xffffffff,"overflow part length"},
        {52,0xffffffff,"overflow program words"},{52,6,"program size mismatch"},
        {48,(6u<<16)|0x60,"unsupported stage"},{48,(2u<<16)|0x50,"legacy model"},
        {48,(2u<<16)|0x160,"reserved version bits"},{48,(2u<<16)|0x6f,"unsupported minor"},
        {60,0x101,"DXIL version"},{64,0xffffffff,"bitcode offset overflow"},{64,12,"bitcode in header"},
        {64,17,"unaligned bitcode"},{68,0xffffffff,"bitcode size overflow"},{68,3,"short bitcode"},
        {72,0,"bad bitcode magic"},{80,19,"HASH size"},{84,2,"unknown HASH semantics"}
    };
    for(const auto& x:mutations) { changed=v; put(changed,x.at,x.value); rejected(changed,x.why); }
    changed=v; tag(changed,0,"ABCD"); rejected(changed,"container magic");
    changed=v; tag(changed,56,"ABCD"); rejected(changed,"DXIL header magic");
    changed=v; tag(changed,40,"SHDR"); rejected(changed,"legacy program");
    changed=v; tag(changed,40,"SHEX"); rejected(changed,"legacy extended program");
    changed=v; tag(changed,40,"JUNK"); rejected(changed,"missing program");
    changed=v; tag(changed,76,"JUNK"); rejected(changed,"missing hash");
    changed=v; tag(changed,76,"DXIL"); rejected(changed,"two programs");
    // Insert third offset, shift original parts, append another HASH part.
    changed=v; changed.insert(changed.begin()+40,4,0); changed.resize(136);
    put(changed,24,136); put(changed,28,3); put(changed,32,44); put(changed,36,80); put(changed,40,108);
    tag(changed,108,"HASH"); put(changed,112,20); rejected(changed,"duplicate HASH");
    tag(changed,108,"JUNK"); check(decode(changed).matches_drawcpt_geometry(),"well-formed unknown chunk allowed");
    put(changed,40,72); tag(changed,72,"JUNK"); put(changed,76,0);
    rejected(changed,"overlapping unknown chunk");
    check(decode(changed).rejection=="overlapping parts","overlap detected before program interpretation");
    // Caller destroys its bytes; retained result remains independent.
    v.clear(); v.shrink_to_fit(); check(m.matches_drawcpt_geometry() && m.container_checksum[0]==0xaa,"owned metadata lifetime");
    std::cout<<checks<<" shader metadata offline checks passed (synthetic only)\n";
}
