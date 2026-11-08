#pragma once
#include <cstdint>
#include <cstddef>
namespace acz::index_range {
// Numeric D3D12 enum values; independent of Windows headers for offline use.
// UINT32_MAX means IBStripCutValue was never observed; DISABLED is zero.
inline constexpr uint32_t unknown_cut=UINT32_MAX;
enum class Index { vertex, restart, unknown };
inline Index classify(uint32_t value,uint32_t stride,uint32_t topology,uint32_t cut) noexcept {
    if(value!=(stride==2?0xffffu:0xffffffffu))return Index::vertex;
    if(!topology||cut==unknown_cut)return Index::unknown;
    const bool strip=topology==3||topology==5||topology==11||topology==13;
    return strip&&((stride==2&&cut==1)||(stride==4&&cut==2))?Index::restart:Index::vertex;
}
struct Result {
    const char* reason="failed:index-byte-range";
    uint64_t minimum=0,maximum=0;
    bool fetched=false,verified=false;
};
// Caller owns the stable bytes and output. Restart markers never fetch vertices.
inline void evaluate(const unsigned char* bytes,size_t length,uint32_t stride,
    uint32_t start,uint32_t count,int32_t base,uint64_t vertices,
    uint32_t topology,uint32_t cut,Result& out) noexcept {
    out={};
    if(!bytes||(stride!=2&&stride!=4)||!count||uint64_t(start)+count>length/stride)return;
    for(uint64_t i=start;i<uint64_t(start)+count;++i) {
        uint32_t value=0;
        for(uint32_t j=0;j<stride;++j)value|=uint32_t(bytes[size_t(i*stride)+j])<<(j*8);
        const auto kind=classify(value,stride,topology,cut);
        if(kind==Index::unknown) {out.reason="strip-cut index requires unavailable topology metadata";return;}
        if(kind==Index::restart)continue;
        if(!out.fetched)out.minimum=out.maximum=value;
        else {if(value<out.minimum)out.minimum=value;if(value>out.maximum)out.maximum=value;}
        out.fetched=true;
    }
    const auto lo=int64_t(out.minimum)+base,hi=int64_t(out.maximum)+base;
    if(out.fetched&&(lo<0||hi<0||uint64_t(hi)>=vertices)) {out.reason="failed:index-vertex-range";return;}
    out.verified=true;out.reason="verified:index-vertex-range";
}
}
