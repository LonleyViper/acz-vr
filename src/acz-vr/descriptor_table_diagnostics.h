#pragma once
#include "root_constant_witness.h"
#include "texture_description_registry.h"
#include "view_write_description.h"
#include <map>
#include <utility>

namespace acz::diagnostics {
// Entirely separate from binding eligibility. Caller holds the hook mutex.
// Allocations occur only on creation/write/copy, never on snapshot/Draw.
struct DescriptorTableDiagnostics {
    enum Flag : unsigned { no_table=1, unknown_base=2, incomplete=4, wrong_heap=8,
        unwritten=16, overwritten=32, copied=64, stale_heap=128,
        wrong_type=256, unsupported_srv=512, unknown_resource=1024,
        uncovered=2048, capacity=4096, invalidated=8192 };
    enum Type : unsigned { unknown, srv, cbv, uav, sampler, other };
    enum Writer : unsigned { writer_unknown, create_srv, create_cbv, create_uav,
        create_sampler, create_other, copy_ranges, copy_simple };
    enum Timing : unsigned { before_bind=1, after_bind=2, during_draw=4, unordered=8 };
    enum Ownership : unsigned { pin_uncovered=0, pin_owned=1, pin_overwritten=2,
        pin_retired=3, pin_shape=4, pin_address=5, pin_cap=6, pin_exception=7,
        pin_reference=8, pin_pending=9, pin_candidate_missing=10, pin_candidate_slot_cap=11 };
    struct Heap {
        uint64_t key=0,serial=0,device=0,cpu=0,gpu=0;
        unsigned count=0,increment=0,type=0,heap_flags=0;bool live=false;
    };
    struct Slot {
        // Caller pointer is never retained. Fixed storage, tied to this write.
        std::array<uint32_t,13> sampler_words{};
        unsigned sampler_ordinal=0;bool sampler_observed=false;
        TextureDescriptionRegistry::Resolution texture;
        ViewWriteDescription view_write;
        unsigned writes=0,type=unknown,dimension=0,format=0;
        unsigned flags=0;bool resource_known=false;
        unsigned ordinal=0,writer=writer_unknown,origin=writer_unknown,desc=0;
        // desc: 0 unknown/not SRV, 1 NULL SRV pDesc, 2 explicit SRV pDesc.
        uint64_t source_heap=0;
        unsigned source_class=UINT32_MAX,source_slot=0,source_flags=0;
        uint64_t resource=0,resource_generation=0;
    };
    struct Location {
        uint64_t key=0,serial=0;unsigned slot=0,type=UINT32_MAX,flags=0;
    };
    struct Snapshot {
        // Borrow only the fixed slot words (or immutable accepted root words).
        // Consume under the caller's hook mutex, before any registry mutation;
        // never retain this snapshot across an unlock or slot replacement.
        const std::array<uint32_t,13>* sampler_words=nullptr;
        unsigned sampler_ordinal=0;bool sampler_observed=false;
        enum SamplerReason : unsigned { sampler_covered, sampler_uncovered,
            sampler_retired, sampler_wrong_heap, sampler_wrong_type,
            sampler_unobserved, sampler_replaced, sampler_copy, sampler_static };
        unsigned sampler_reason() const noexcept {
            if(flags&stale_heap)return sampler_retired;
            if(heap_class!=1||flags&wrong_heap)return sampler_wrong_heap;
            if(flags&(no_table|unknown_base|incomplete|unwritten|uncovered|capacity|invalidated)||
               (timing!=before_bind&&timing!=after_bind))return sampler_uncovered;
            if(type!=sampler)return sampler_wrong_type;
            if(writer==copy_ranges||writer==copy_simple)return sampler_copy;
            if(!sampler_observed||!sampler_words)return sampler_unobserved;
            if(!ordinal||ordinal!=sampler_ordinal)return sampler_replaced;
            return writer==create_sampler&&origin==create_sampler?sampler_covered:sampler_unobserved;
        }
        void append_sampler(char* text,size_t bytes,bool is_static=false) const noexcept {
            const auto n=std::strlen(text);if(n>=bytes)return;
            static constexpr std::array<uint32_t,13> empty{};
            const auto& words=sampler_words?*sampler_words:empty;
            // Floats are lossless IEEE-754 words; no locale or NaN formatting.
            std::snprintf(text+n,bytes-n,
                ";sr=%x;so=%x;sn=%x;sw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                is_static?unsigned(sampler_static):sampler_reason(),unsigned(sampler_observed),sampler_ordinal,
                words[0],words[1],words[2],words[3],words[4],
                words[5],words[6],words[7],words[8],words[9],
                words[10],words[11],words[12]);
        }
        TextureDescriptionRegistry::Resolution texture;
        ViewWriteDescription view_write;
        unsigned flags=0,parameter=UINT32_MAX,offset=0,slot=0,type=unknown,dimension=0,format=0,heap_class=UINT32_MAX;
        uint64_t heap=0;
        unsigned writes=0,ordinal=0,writer=0,origin=0,desc=0,timing=unordered;
        unsigned bind_before=0,bind_after=0,draw_before=0;
        uint64_t source_heap=0;
        unsigned source_class=UINT32_MAX,source_slot=0,source_flags=0;
        Ownership ownership_reason() const noexcept {
            if(flags&stale_heap)return pin_retired;
            if(!ordinal||ordinal!=view_write.ordinal)return pin_overwritten;
            if(flags&(no_table|unknown_base|incomplete|wrong_heap|unwritten|wrong_type|
                      uncovered|capacity|invalidated)||heap_class!=0||type!=srv||
               (timing!=before_bind&&timing!=after_bind))return pin_uncovered;
            // Only the null-desc SRV has the default Texture2D view semantics
            // whose full resource description was read by the actual creator.
            if(desc!=1||origin!=create_srv||!view_write.observed||view_write.reasons||
               view_write.dimension!=D3D12_SRV_DIMENSION_TEXTURE2D)return pin_shape;
            if(!view_write.address)return pin_address;
            return pin_owned; // shape only; caller MUST hold this publication's independently acquired reference
        }
        void classify(unsigned before,unsigned after,unsigned draw,bool covered) noexcept {
            bind_before=before;bind_after=after;draw_before=draw;timing=unordered;
            // Any observed mutation during the setter, or active binding event
            // at either boundary (UINT32_MAX), makes native ordering unknown.
            if(!covered||!ordinal||before!=after||after>draw||draw==UINT32_MAX||
               (flags&(incomplete|capacity|invalidated|unknown_base|stale_heap|unwritten|uncovered)))return;
            timing=ordinal<=before?before_bind:ordinal<=after?unordered:
                ordinal<=draw?after_bind:during_draw;
        }
        // Compact append-only suffix also fits the reserved first example.
        void append_last(char* text,size_t bytes) const noexcept {
            const auto n=std::strlen(text);if(n>=bytes)return;
            std::snprintf(text+n,bytes-n,";wf=%x;wo=%x;wn=%x;wk=%x;nd=%x",
                timing,ordinal,writes,writer,desc);
        }
        // Detailed copy provenance uses a separate bounded companion.
        void append_write(char* text,size_t bytes) const noexcept {
            const auto n=std::strlen(text);if(n>=bytes)return;
            std::snprintf(text+n,bytes-n,
                ";th=%llx;ts=%x;wf=%x;wo=%x;wn=%x;wk=%x;ok=%x;nd=%x;sd=%x;sf=%x;wb=%x;we=%x;wd=%x;sh=%llx;sc=%x;ss=%x;sv=%x",
                static_cast<unsigned long long>(heap),slot,timing,ordinal,writes,writer,origin,desc,
                dimension,format,bind_before,bind_after,draw_before,
                static_cast<unsigned long long>(source_heap),source_class,source_slot,source_flags);
        }
        void append(char* text,size_t bytes) const noexcept {
            const auto n=std::strlen(text);if(n>=bytes)return;
            std::snprintf(text+n,bytes-n,";tf=%04x;tp=%x;to=%x;th=%llx;hc=%x;ts=%x;dk=%x;sd=%x;sf=%x",
                flags,parameter,offset,static_cast<unsigned long long>(heap),heap_class,slot,type,dimension,format);
        }
    };
    static constexpr size_t limit=65536;
    std::map<uint64_t,Heap> heaps;
    std::map<std::pair<uint64_t,unsigned>,Slot> slots;
    uint64_t serial=0;
    unsigned write_ordinal=0;
    std::array<unsigned,4> creations{},textures{};
    unsigned missing_desc=0;
    void creation(unsigned seam,unsigned dimension) noexcept {
        if(seam>=creations.size())return;
        auto increment=[](unsigned& value){if(value<65535)++value;};
        increment(creations[seam]);
        if(dimension>=2&&dimension<=4)increment(textures[seam]);
        if(!dimension)increment(missing_desc);
    }
    void append_creations(char* text,size_t bytes) const noexcept {
        const auto n=std::strlen(text);if(n>=bytes)return;
        std::snprintf(text+n,bytes-n,";ra=%04x,%04x,%04x,%04x;rx=%04x,%04x,%04x,%04x;rn=%04x",
            creations[0],creations[1],creations[2],creations[3],
            textures[0],textures[1],textures[2],textures[3],missing_desc);
    }
    bool overflow=false;
    bool history_complete=true,gap=false;
    void heap(uint64_t key,uint64_t device,uint64_t cpu,uint64_t gpu,
              unsigned count,unsigned increment,unsigned type,unsigned flags=0) {
        if(serial==UINT64_MAX||heaps.size()>=limit){overflow=true;return;}
        heaps[key]={key,++serial,device,cpu,gpu,count,increment,type,flags,true};
    }
    void retire(uint64_t key) noexcept {auto h=heaps.find(key);if(h!=heaps.end())h->second.live=false;}
    Location locate(uint64_t handle,uint64_t device,bool gpu) const noexcept {
        Location out;bool found=false;
        for(const auto& item:heaps) {
            const auto& h=item.second;const auto base=gpu?h.gpu:h.cpu;
            if(!h.live||h.device!=device||!base||!h.increment||handle<base)continue;
            const auto delta=handle-base;
            if(delta%h.increment||delta/h.increment>=h.count)continue;
            if(found){out.flags=unknown_base;out.key=out.serial=0;return out;}
            found=true;out={h.key,h.serial,unsigned(delta/h.increment),h.type,0};
        }
        if(!found)out.flags=unknown_base;
        return out;
    }
    // Only at setters: at Draw use this cached Location, never a heap scan.
    Location bound(uint64_t key,uint64_t handle,uint64_t device) const noexcept {
        Location out;auto h=heaps.find(key);
        if(h==heaps.end()){out.flags=unknown_base;return out;}
        const auto& v=h->second;out={key,v.serial,0,v.type,0};
        if(!v.live){out.flags=stale_heap;return out;}
        if(v.device!=device||!v.gpu||!v.increment||handle<v.gpu||
           (handle-v.gpu)%v.increment||(handle-v.gpu)/v.increment>=v.count) {out.flags=unknown_base;return out;}
        out.slot=unsigned((handle-v.gpu)/v.increment);return out;
    }
    void store(Location l,Slot value) {
        if(l.flags||!l.serial)return;
        const auto key=std::make_pair(l.serial,l.slot);auto old=slots.find(key);
        if(old==slots.end()&&slots.size()>=limit){overflow=true;return;}
        // Counts are destination-local, never inherited from a copy source.
        value.writes=0;
        if(write_ordinal==UINT32_MAX){overflow=true;value.ordinal=0;}
        else value.ordinal=++write_ordinal;
        if(value.view_write.observed&&!value.view_write.ordinal)value.view_write.ordinal=value.ordinal;
        if(value.sampler_observed&&!value.sampler_ordinal)value.sampler_ordinal=value.ordinal;
        if(old!=slots.end()) {
            value.flags|=(old->second.flags&(overwritten|copied|invalidated))|overwritten;
            value.writes=old->second.writes;
        }
        if(value.writes<65535)++value.writes;
        slots[key]=value;
    }
    void write(uint64_t cpu,uint64_t device,unsigned type,unsigned dimension=0,unsigned format=0,
               uint64_t resource=0,uint64_t generation=0,unsigned flags=0,unsigned writer=writer_unknown,unsigned desc=0,TextureDescriptionRegistry::Resolution texture={},const ViewWriteDescription* view_write=nullptr,const std::array<uint32_t,13>* sampler_words=nullptr) {
        Slot v;v.texture=texture;if(view_write)v.view_write=*view_write;v.type=type;v.dimension=dimension;v.format=format;
        if(sampler_words){v.sampler_words=*sampler_words;v.sampler_observed=true;}
        v.flags=flags;v.writer=writer;v.origin=writer;v.desc=desc;
        v.resource=resource;v.resource_generation=generation;v.resource_known=resource&&generation;
        if(type==unknown||type==other)v.flags|=invalidated;
        store(locate(cpu,device,false),v);
    }
    // Diagnostic copy preserves unsupported texture facts without making them
    // usable by the registry. Any unprovable copy taints all diagnostic slots.
    void invalidate() noexcept {gap=true;for(auto& s:slots)s.second.flags|=invalidated;}
    Snapshot snapshot(Location l,unsigned offset,unsigned expected=srv) const noexcept {
        Snapshot s;s.flags=l.flags;s.heap=l.serial;s.heap_class=l.type;
        s.slot=l.slot;s.offset=offset;
        if(overflow)s.flags|=capacity;
        if(!history_complete)s.flags|=incomplete;
        if(gap)s.flags|=invalidated;
        auto h=heaps.find(l.key);
        if(!l.serial||h==heaps.end()){s.flags|=unknown_base;return s;}
        if(!h->second.live||h->second.serial!=l.serial)s.flags|=stale_heap;
        if(l.type!=(expected==sampler?1u:0u))s.flags|=wrong_heap;
        if(offset>UINT32_MAX-l.slot||l.slot>=h->second.count||offset>=h->second.count-l.slot) {
            s.flags|=unknown_base;return s;
        }
        s.slot+=offset;
        auto it=slots.find({l.serial,s.slot});
        if(it==slots.end()){s.flags|=unwritten;return s;}
        const auto& v=it->second;s.texture=v.texture;s.view_write=v.view_write;s.flags|=v.flags;s.type=v.type;s.dimension=v.dimension;s.format=v.format;
        s.sampler_words=&v.sampler_words;s.sampler_observed=v.sampler_observed;s.sampler_ordinal=v.sampler_ordinal;
        s.writes=v.writes;s.ordinal=v.ordinal;s.writer=v.writer;s.origin=v.origin;s.desc=v.desc;
        s.source_heap=v.source_heap;s.source_class=v.source_class;
        s.source_slot=v.source_slot;s.source_flags=v.source_flags;
        if(v.type!=expected)s.flags|=wrong_type;
        // The production observer supports only structured BUFFER SRVs.
        if(v.type==srv&&(v.dimension!=1||v.format!=0))s.flags|=unsupported_srv;
        if(v.type==srv&&!v.resource_known)s.flags|=unknown_resource;
        return s;
    }
};

struct RootDescriptorTableWitness {
    uint64_t root=0,token=0,list=0,generation=0,revision=0,before=0,after=0,handle=0;
    unsigned parameter=UINT32_MAX,offset=0,thread=0;
    unsigned descriptor_before=0,descriptor_after=0;
    bool configured=false,written=false;
    DescriptorTableDiagnostics::Location location;
    void bind(uint64_t key,uint64_t identity,unsigned index,unsigned slot,bool accepted) noexcept {
        if(root!=key||token!=identity||parameter!=index||offset!=slot) {
            written=false;location={};handle=0;descriptor_before=descriptor_after=0;
        }
        root=key;token=identity;parameter=index;offset=slot;configured=accepted;
    }
    void write(const RootConstantWitness::Call& call,const RootConstantWitness& clock,
               unsigned index,uint64_t base,DescriptorTableDiagnostics::Location loc,
               unsigned descriptorBefore=0,unsigned descriptorAfter=0) noexcept {
        if(!configured||index!=parameter)return;
        list=call.list;generation=call.generation;revision=clock.revision;
        before=call.before;after=call.after;thread=call.thread;handle=base;location=loc;written=true;
        descriptor_before=descriptorBefore;descriptor_after=descriptorAfter;
    }
    bool covered(const RootConstantWitness& c,uint64_t id,uint64_t gen,uint64_t key,
                 uint64_t identity,unsigned tid,bool complete) const noexcept {
        return configured&&written&&complete&&id&&gen&&c.poison==RootConstantWitness::covered&&
            c.active==1&&c.owner==tid&&c.list==id&&c.generation==gen&&
            c.currentBefore&&c.currentAfter>c.currentBefore&&
            list==id&&generation==gen&&root==key&&token==identity&&thread==tid&&
            revision&&revision<=c.revision&&before&&after>before&&after<c.currentBefore;
    }
};
struct DescriptorTableCounters {
    unsigned sticky=0;
    std::array<unsigned,14> reasons{};
    void record(unsigned flags) noexcept {
        sticky|=flags;for(unsigned i=0;i<reasons.size();++i)if((flags&(1u<<i))&&reasons[i]<65535)++reasons[i];
    }
    void append(char* text,size_t bytes) const noexcept {
        auto n=std::strlen(text);if(n>=bytes)return;
        std::snprintf(text+n,bytes-n,";tg=%04x;tc=",sticky);
        for(auto value:reasons){n=std::strlen(text);if(n>=bytes)return;std::snprintf(text+n,bytes-n,"%04x",value);}
    }
};
} // namespace acz::diagnostics
