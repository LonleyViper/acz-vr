#include "shader_metadata.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace acz::metadata {
namespace {
uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
bool tag(const uint8_t* p, const char* s) { return std::memcmp(p,s,4)==0; }
ShaderMetadata fail(const char* why) { ShaderMetadata r; r.rejection=why; return r; }
}
bool ShaderMetadata::matches_drawcpt_geometry() const {
    // Evidence supplies no HASH flags; conservatively match binary hashes only.
    return valid && stage==ShaderStage::geometry && major==6 && minor==0 &&
        hash_semantics==HashSemantics::binary && identifier==drawcpt_geometry_identifier;
}
ShaderMetadata decode_shader_metadata(const void* data, size_t size) {
    if (!data || size<32 || size>std::numeric_limits<uint32_t>::max()) return fail("invalid container size/input");
    const auto* p=static_cast<const uint8_t*>(data);
    if (!tag(p,"DXBC")) return fail("unsupported container magic");
    if (u32(p+20)!=1) return fail("unsupported container version");
    if (u32(p+24)!=size) return fail("declared container size mismatch");
    const size_t count=u32(p+28);
    if (!count || count>(size-32)/4) return fail("invalid part count/table");
    const size_t table_end=32+4*count;
    struct Part { size_t begin,end; };
    std::vector<Part> parts;
    size_t program=0, program_size=0, hash=0;
    for (size_t i=0;i<count;++i) {
        const size_t off=u32(p+32+4*i);
        if (off%4 || off<table_end || off>size || size-off<8) return fail("invalid part offset");
        const size_t n=u32(p+off+4);
        if (n>size-off-8) return fail("truncated/overflowing part");
        parts.push_back({off,off+8+n});
        if (tag(p+off,"DXIL")) {
            if (program) return fail("ambiguous DXIL programs");
            program=off+8; program_size=n;
        } else if (tag(p+off,"HASH")) {
            if (hash) return fail("ambiguous HASH parts");
            if (n!=20) return fail("invalid HASH size");
            hash=off+8;
        } else if (tag(p+off,"SHDR") || tag(p+off,"SHEX")) return fail("legacy/mixed program unsupported");
    }
    std::sort(parts.begin(),parts.end(),[](Part a,Part b){return a.begin<b.begin;});
    for (size_t i=1;i<parts.size();++i)
        if (parts[i].begin<parts[i-1].end) return fail("overlapping parts");
    if (!program || !hash) return fail("missing DXIL program or HASH identifier");
    if (program_size<28 || uint64_t(u32(p+program+4))*4!=program_size) return fail("invalid program word size");
    const uint32_t version=u32(p+program);
    if ((version>>16)>5 || (version&0xff00)!=0 || ((version>>4)&15)!=6)
        return fail("unsupported shader stage/model");
    // The registered disassembly establishes SM6.0 only. Do not accept newer
    // models paired with a DXIL1.0 header merely because their minor fits.
    if ((version&15)!=0) return fail("unsupported shader model: only SM6.0 supported");
    if (!tag(p+program+8,"DXIL") || u32(p+program+12)!=0x100) return fail("unsupported DXIL header/version");
    const size_t bitoff=u32(p+program+16), bitsize=u32(p+program+20);
    // BitcodeOffset is relative to the DXIL bitcode header, not the program.
    if (bitoff<16 || bitoff%4 || bitoff>program_size-8 || bitsize<4 || bitsize>program_size-8-bitoff)
        return fail("invalid bitcode interval");
    const uint8_t magic[]={0x42,0x43,0xc0,0xde};
    if (std::memcmp(p+program+8+bitoff,magic,4)!=0) return fail("invalid bitcode magic");
    const uint32_t flags=u32(p+hash);
    if (flags>1) return fail("unsupported HASH flags");
    ShaderMetadata r;
    r.stage=static_cast<ShaderStage>(version>>16); r.major=6; r.minor=uint8_t(version&15);
    r.hash_semantics=flags ? HashSemantics::includes_source : HashSemantics::binary;
    std::copy_n(p+hash+4,16,r.identifier.begin());
    std::copy_n(p+4,16,r.container_checksum.begin());
    r.valid=true; return r;
}
} // namespace acz::metadata
