#pragma once
// SAMPLED DIAGNOSTIC CPU bytes only. Caller holds the observed-invalidation mutex.
// Application writes through mapped pointers bypass the hooks. Two equal reads
// do not exclude concurrent writes, prove the draw used these bytes, or establish
// GPU completion, safe copy, replay readiness, or a capture certificate.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include "index_range.h"
namespace acz::diagnostics {
struct CpuUploadSnapshot {
    static constexpr size_t payload_cap=2048,header_size=320,file_cap=header_size+payload_cap,limit=4;
    struct Range {
        uint64_t resource=0,lifetime=0,map_generation=0,va=0,offset=0,bytes=0,resource_bytes=0;
        const void* mapped=nullptr;
        bool pinned=false,upload=false;
    };
    struct Request {
        uint64_t list=0,generation=0,revision=0,draw=0,observation_epoch=0;
        std::array<Range,3> ranges{};
        uint32_t vertex_stride=0,index_stride=0,start=0,count=0;
        uint32_t topology=0,strip_cut=index_range::unknown_cut,descriptor_before=UINT32_MAX;
        uint32_t instances=0,start_instance=0;
        int32_t base=0;
        bool accepted=false;
    };
    using Reader=bool(*)(const void*,void*,size_t) noexcept;
    std::array<unsigned char,payload_cap> first{},second{};
    std::array<unsigned char,file_cap> file{};
    std::array<Request,limit> attempted{};
    Request completed{}; // identity of first[]; never borrowed mapped bytes
    std::array<uint64_t,3> hashes{};
    size_t attempts=0,file_bytes=0;
    uint64_t minimum=0,maximum=0,vertices=0;
    uint64_t sampled_ms=0; // J-3b6: hook-supplied tick time of the last stable sample (diagnostic only)
    const char* reason="not-evaluated";
    const char* index_reason="not-evaluated";
    bool stable=false;
    bool matches(const Request& r) const noexcept {
        const auto& c=completed;
        if(!stable||!r.accepted||!c.accepted||c.list!=r.list||c.generation!=r.generation||
           c.revision!=r.revision||c.draw!=r.draw||c.observation_epoch!=r.observation_epoch||
           c.vertex_stride!=r.vertex_stride||c.index_stride!=r.index_stride||c.start!=r.start||
           c.count!=r.count||c.base!=r.base||c.instances!=r.instances||c.start_instance!=r.start_instance||
           c.topology!=r.topology||c.strip_cut!=r.strip_cut||c.descriptor_before!=r.descriptor_before)return false;
        for(size_t i=0;i<3;++i) {
            const auto& a=c.ranges[i];const auto& b=r.ranges[i];
            if(!a.pinned||!b.pinned||a.resource!=b.resource||a.lifetime!=b.lifetime||
               a.map_generation!=b.map_generation||a.mapped!=b.mapped||a.va!=b.va||
               a.offset!=b.offset||a.bytes!=b.bytes||a.resource_bytes!=b.resource_bytes||a.upload!=b.upload)return false;
        }
        return true;
    }
    static uint64_t hash(const unsigned char* p,size_t n) noexcept {
        uint64_t h=14695981039346656037ULL;
        for(size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ULL;}return h;
    }
    static void word(unsigned char* p,uint64_t v) noexcept {
        for(unsigned i=0;i<8;++i){p[i]=static_cast<unsigned char>(v);v>>=8;}
    }
    // Four attempts total, including failures; never retry one recording draw.
    // Identity is exact (list, recording generation, matched draw ordinal).
    bool reserve(bool enabled,const Request& r) noexcept {
        if(!enabled||!r.accepted||attempts==limit)return false;
        for(size_t i=0;i<attempts;++i)if(attempted[i].list==r.list&&attempted[i].generation==r.generation&&attempted[i].draw==r.draw)return false;
        attempted[attempts++]=r;return true;
    }
    bool read(const Request& r,Reader reader) noexcept {
        file_bytes=0;stable=false;hashes.fill(0);minimum=maximum=vertices=0;index_reason="not-evaluated";
        reason="metadata-unaccepted";if(!r.accepted)return false;
        size_t total=0;
        for(const auto& x:r.ranges) {
            reason="unmapped";if(!x.mapped||!x.map_generation)return false;
            reason="unpinned";if(!x.pinned)return false;
            reason="not-upload";if(!x.upload)return false;
            reason="out-of-range";
            if(!x.bytes||x.offset>x.resource_bytes||x.bytes>x.resource_bytes-x.offset||x.bytes>payload_cap-total)return false;
            const auto ptr=reinterpret_cast<uintptr_t>(x.mapped);
            if(x.offset>UINTPTR_MAX-ptr||x.bytes>UINTPTR_MAX-(ptr+x.offset))return false;
            total+=size_t(x.bytes);
        }
        reason="unsupported-spans";
        if(r.ranges[0].bytes!=416||!r.vertex_stride||(r.index_stride!=2&&r.index_stride!=4))return false;
        // Separate passes detect some changes, never establish coherence.
        for(unsigned pass=0;pass<2;++pass) {
            std::atomic_signal_fence(std::memory_order_seq_cst); // compiler barrier; no GPU synchronization
            size_t at=0;
            for(const auto& x:r.ranges) {
                auto* out=(pass?second:first).data()+at;
                const auto* in=reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(x.mapped)+uintptr_t(x.offset));
                reason="read-fault";if(!reader(in,out,size_t(x.bytes)))return false;at+=size_t(x.bytes);
            }
        }
        reason="unstable";if(std::memcmp(first.data(),second.data(),total))return false;
        stable=true;completed=r;reason="snapshot-stable";
        size_t at=0;for(size_t i=0;i<3;++i){hashes[i]=hash(first.data()+at,size_t(r.ranges[i].bytes));at+=size_t(r.ranges[i].bytes);}
        vertices=r.ranges[1].bytes/r.vertex_stride;
        const auto ib=size_t(r.ranges[0].bytes+r.ranges[1].bytes);
        index_range::Result range;
        index_range::evaluate(first.data()+ib,size_t(r.ranges[2].bytes),r.index_stride,
            r.start,r.count,r.base,vertices,r.topology,r.strip_cut,range);
        minimum=range.minimum;maximum=range.maximum;index_reason=range.reason;
        file.fill(0);std::memcpy(file.data(),"ACZCPU1",7);
        word(file.data()+8,header_size);word(file.data()+16,total);word(file.data()+24,r.list);
        word(file.data()+32,r.generation);word(file.data()+40,r.draw);word(file.data()+48,r.revision);
        for(size_t i=0;i<3;++i) {
            const auto& x=r.ranges[i];auto* h=file.data()+56+i*64;
            word(h,x.resource);word(h+8,x.lifetime);word(h+16,x.map_generation);word(h+24,x.va);
            word(h+32,x.offset);word(h+40,x.bytes);word(h+48,hashes[i]);word(h+56,x.resource_bytes);
        }
        word(file.data()+248,minimum);word(file.data()+256,maximum);word(file.data()+264,vertices);
        word(file.data()+272,r.vertex_stride);word(file.data()+280,r.index_stride);
        word(file.data()+288,r.start);word(file.data()+296,r.count);word(file.data()+304,uint64_t(int64_t(r.base)));
        word(file.data()+312,std::strncmp(index_reason,"verified",8)==0);
        std::memcpy(file.data()+header_size,first.data(),total);file_bytes=header_size+total;return true;
    }
};
static_assert(sizeof(CpuUploadSnapshot::Request)<=512,"Draw request must stay small");
static_assert(sizeof(CpuUploadSnapshot)<=12*1024,"Fixed HookState snapshot budget");
}
