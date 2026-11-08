#pragma once
// Observation inventory only. Opaque addresses are NOT identities or pins.
// Caller serializes with the hook mutex. No allocation, COM or GPU calls.
#include <d3d12.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include "descriptor_table_diagnostics.h"

namespace acz::diagnostics {
struct L3Observation {
    // Eight hexadecimal digits bound every observation counter/serial. A full
    // clock is a coverage gap, never permission to compare saturated epochs.
    static void inc(uint64_t& n) noexcept {if(n<UINT32_MAX)++n;}
    static uint64_t add(uint64_t a,uint64_t b) noexcept {return b>UINT32_MAX-a?UINT32_MAX:a+b;}
    static size_t hash(uint64_t a) noexcept {a^=a>>33;a*=0xff51afd7ed558ccdULL;a^=a>>33;return size_t(a);}
    // Eight probes, no eviction. Collision/capacity loss is explicit and sticky.
    template<class T,size_t N> struct Store {
        std::array<T,N> entries{};uint64_t lost=0;
        T* get(uint64_t key,bool create=false) noexcept {
            if(!key)return nullptr;
            const auto start=hash(key);
            for(size_t p=0;p<8;++p) {
                auto& e=entries[(start+p)%N];
                if(e.key==key)return &e;
                if(!e.key) {if(!create)return nullptr;e.key=key;return &e;}
            }
            if(create)inc(lost);return nullptr;
        }
    };
    struct Resource {
        uint64_t key=0,serial=0,version=0,last_sub=0,last_version=0,last_facts=0;
        D3D12_RESOURCE_DESC desc{};
        const void* mapped=nullptr;
        const char* upload_pin_reason="upload-pin-not-created";
        uint64_t map_generation=0,mapped_lifetime=0,gpu_va=0;
        unsigned mapped_subresource=UINT32_MAX,map_inflight=0;
        void invalidate_map() noexcept {mapped=nullptr;mapped_lifetime=0;mapped_subresource=UINT32_MAX;if(map_generation<UINT64_MAX)++map_generation;}

        uint64_t maps=0,unmaps=0,outstanding=0,map_pointers=0,cpu=0,copies=0,cpu_serial=0,writes=0;
        uint64_t barrier_serial=0;uint32_t before=0,after=0,subresource=0,barrier_flags=0;
        uint32_t heap=0,page=0,pool=0,kind=0; // 0 unknown, 1 committed, 2 placed, 3 reserved
        void** map_table=nullptr; // captured only in a live creation interval; never dereference a historical object
        bool description=false,retired=false,map_gap=false,hooks=false;
        struct Seen {uint64_t sub=0,version=0,facts=0;};
        std::array<Seen,3> seen{};
    };
    struct State {
        uint64_t key=0,list=0,generation=0,resource=0,transition=0,write=0,uav=0,alias=0;
        uint64_t earlier=0;uint32_t earlier_before=0,earlier_after=0,earlier_subresource=0,earlier_flags=0;
        uint32_t before=0,after=0,subresource=0,flags=0;
        bool collision=false;
    };
    struct Source {uint64_t address=0,va=0,bytes=0;};
    struct List {
        uint64_t key=0,generation=0;
        uint64_t draws=0,resets=0,closes=0,executes=0,last_sub=0;
        uint64_t global_uav=0,global_alias=0;
        // One record per source class/IA slot; NOT a draw-time byte snapshot.
        std::array<Source,34> sources{}; // t10, 32 VB, IB
        std::array<uint64_t,64> selected{};
        std::array<unsigned char,64> selected_roles{};
        unsigned selected_unknown=0;
        unsigned selected_count=0;
        uint32_t t10_parameter=UINT32_MAX,t10_offset=0;
        // J-180 (Codex review P2): the t10 binding of the VERTEX stage (DrawACM_v has no geometry shader), tracked next to the cockpit's GS binding
        // (t10_parameter). They are the same parameter in the usual layouts; a root with a VERTEX-visible t10 and a GEOMETRY-visible t10 in different
        // parameters keeps both. Only set with a world key (l3ConfigureT10). A table candidate is recorded (unsupported downstream), never tracked.
        uint32_t t10_vs_parameter=UINT32_MAX,t10_vs_offset=0;bool t10_vs_table=false;Source vs_source{};
        uint64_t table_cpu=0,table_epoch=0;
        uint64_t table_identity=0;
        bool t10_table=false,closed=false,gap=false;
        const DescriptorTableDiagnostics::Heap* table_heap=nullptr;
        const DescriptorTableDiagnostics::Slot* table_slot=nullptr;
        const DescriptorTableDiagnostics* table_diagnostics=nullptr;
    };
    Store<List,512> lists;
    Store<Resource,8192> resources;
    Store<State,16384> states;
    uint64_t clock=0,creation=0,submission=0,draws=0,submissions_with_draw=0;
    uint64_t sub_total=0,sub_min=UINT64_MAX,sub_max=0,list_total=0,list_min=UINT64_MAX,list_max=0;
    uint64_t repeat=0,rewritten=0,new_address=0,unknown=0;
    struct Repeats {uint64_t fresh=0,same=0,write=0,unknown=0;};
    std::array<Repeats,3> repeats{};
    std::array<uint64_t,5> heaps{}; // unknown, DEFAULT, UPLOAD, READBACK, CUSTOM; selected sightings
    uint64_t descriptor_epoch=0,patch_gaps=0,untracked_events=0;
    bool enabled=false;
    List* list(uint64_t key,uint64_t generation) noexcept {
        if(!enabled||!key)return nullptr;
        auto* l=lists.get(key,true);if(!l)return nullptr;
        if(l->generation!=generation) {
            l->generation=generation;l->draws=l->executes=l->closes=l->last_sub=0;
            l->global_uav=l->global_alias=0;l->closed=false;l->gap=false;
            l->sources.fill(Source{});l->selected.fill(0);l->selected_roles.fill(0);l->selected_unknown=0;l->selected_count=0;
            l->table_heap=nullptr;l->table_slot=nullptr;l->table_diagnostics=nullptr;l->t10_parameter=UINT32_MAX;l->t10_table=false;
            l->t10_vs_parameter=UINT32_MAX;l->t10_vs_table=false;l->vs_source=Source{};
        }
        return l;
    }
    uint64_t tick() noexcept {inc(clock);return clock;}
    void create(uint64_t address,const D3D12_RESOURCE_DESC* desc,unsigned kind,
                const D3D12_HEAP_PROPERTIES* props=nullptr) noexcept {
        if(!enabled||!address)return;
        // DrawCPT's three source classes are buffers. Do not let unrelated
        // texture creation exhaust their inventory (J-125's live lesson).
        if(desc&&desc->Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER&&!resources.get(address))return;
        auto* r=resources.get(address,true);if(!r)return;
        // Keep the address slot but start a new observation incarnation.
        const auto last_sub=r->last_sub,last_version=r->last_version,last_facts=r->last_facts;
        const auto seen=r->seen;
        *r=Resource{};r->key=address;inc(creation);r->serial=creation;r->version=tick();r->kind=kind;
        r->last_sub=last_sub;r->last_version=last_version;r->last_facts=last_facts;
        r->seen=seen;
        if(desc){r->desc=*desc;r->description=true;}
        if(props){r->heap=unsigned(props->Type);r->page=unsigned(props->CPUPageProperty);r->pool=unsigned(props->MemoryPoolPreference);}
    }
    void view(uint64_t address,const D3D12_RESOURCE_DESC& desc) noexcept {
        if(!enabled||!address)return;
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER&&!resources.get(address))return;
        auto* r=resources.get(address,true);if(!r)return;
        // View-write GetDesc supplies shape, never creation/heap or map coverage.
        const auto& old=r->desc;
        const bool same=r->description&&old.Dimension==desc.Dimension&&old.Alignment==desc.Alignment&&old.Width==desc.Width&&
            old.Height==desc.Height&&old.DepthOrArraySize==desc.DepthOrArraySize&&old.MipLevels==desc.MipLevels&&old.Format==desc.Format&&
            old.SampleDesc.Count==desc.SampleDesc.Count&&old.SampleDesc.Quality==desc.SampleDesc.Quality&&old.Layout==desc.Layout&&old.Flags==desc.Flags;
        if(same)return;
        if(r->description){r->invalidate_map();r->gpu_va=0;r->serial=0;r->heap=r->page=r->pool=r->kind=0;r->map_gap=true;r->hooks=false;}
        r->desc=desc;r->description=true;r->version=tick();
    }
    void map(uint64_t address,bool success,bool pointer,unsigned subresource,const void* data=nullptr,uint64_t lifetime=0) noexcept {
        if(!enabled)return;auto* r=resources.get(address);if(!r){inc(untracked_events);return;}
        if(!success)return;
        inc(r->maps);inc(r->outstanding);if(pointer)inc(r->map_pointers);
        if(subresource)r->map_gap=true;
        r->invalidate_map();
        if(data&&pointer&&!subresource&&r->outstanding==1&&!r->map_gap&&r->map_generation!=UINT64_MAX) {
            r->mapped=data;r->mapped_lifetime=lifetime;r->mapped_subresource=subresource;
        }
        inc(r->cpu);r->cpu_serial=r->version=tick();
    }
    void unmap(uint64_t address,unsigned subresource) noexcept {
        if(!enabled)return;auto* r=resources.get(address);if(!r){inc(untracked_events);return;}
        r->invalidate_map();
        inc(r->unmaps);if(r->outstanding)--r->outstanding;else r->map_gap=true;
        if(!r->outstanding)r->map_pointers=0;
        if(subresource)r->map_gap=true;
        inc(r->cpu);r->cpu_serial=r->version=tick();
    }
    void cpu_write(uint64_t address) noexcept {
        if(!enabled)return;auto* r=resources.get(address);if(!r){inc(untracked_events);return;}
        inc(r->cpu);inc(r->writes);r->cpu_serial=r->version=tick();
    }
    State* state(uint64_t list,uint64_t generation,uint64_t address,bool create=false) noexcept {
        if(!list||!address)return nullptr;
        const auto key=uint64_t(hash(list)^hash(generation+0x9e3779b97f4a7c15ULL)^hash(address+1))|1;
        auto* s=states.get(key,create);if(!s)return nullptr;
        if(!s->list){s->list=list;s->generation=generation;s->resource=address;}
        if(s->list!=list||s->generation!=generation||s->resource!=address){s->collision=true;if(create)inc(states.lost);return nullptr;}
        return s;
    }
    static bool write_like(uint32_t value) noexcept {
        return (value&(D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_UNORDERED_ACCESS|
            D3D12_RESOURCE_STATE_DEPTH_WRITE|D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_RESOLVE_DEST|D3D12_RESOURCE_STATE_STREAM_OUT))!=0;
    }
    void copy(uint64_t list,uint64_t generation,uint64_t address) noexcept {
        if(!enabled)return;auto* r=resources.get(address);if(!r){inc(untracked_events);return;}
        const auto n=tick();inc(r->copies);r->version=n;
        if(auto* s=state(list,generation,address,true))s->write=n;
    }
    void barrier(List& l,uint64_t list,uint64_t generation,const D3D12_RESOURCE_BARRIER& b) noexcept {
        if(!enabled)return;const auto n=tick();
        if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            const auto address=reinterpret_cast<uintptr_t>(b.Transition.pResource);
            if(!resources.get(address)){inc(untracked_events);return;}
            auto* previous=resources.get(address);
            if(auto* s=state(list,generation,address,true))if(!s->transition&&previous) {
                s->earlier=previous->barrier_serial;s->earlier_before=previous->before;s->earlier_after=previous->after;
                s->earlier_subresource=previous->subresource;s->earlier_flags=previous->barrier_flags;
            }
            if(auto* r=resources.get(address,true)) {
                r->barrier_serial=n;r->before=unsigned(b.Transition.StateBefore);r->after=unsigned(b.Transition.StateAfter);
                r->subresource=b.Transition.Subresource;r->barrier_flags=unsigned(b.Flags);
                if(write_like(r->before)||write_like(r->after))r->version=n;
            }
            if(auto* s=state(list,generation,address,true)) {
                s->transition=n;s->before=unsigned(b.Transition.StateBefore);s->after=unsigned(b.Transition.StateAfter);
                s->subresource=b.Transition.Subresource;s->flags=unsigned(b.Flags);
                // Leaving a write state is a transition AFTER the last write,
                // not another write. Keep conservative resource versioning above.
                if(write_like(s->after))s->write=n;
            }
        } else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            if(!b.UAV.pResource)l.global_uav=n;
            else if(resources.get(reinterpret_cast<uintptr_t>(b.UAV.pResource))) {
                if(auto* s=state(list,generation,reinterpret_cast<uintptr_t>(b.UAV.pResource),true))s->uav=n;
            } else inc(untracked_events);
        } else if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            if(!b.Aliasing.pResourceBefore||!b.Aliasing.pResourceAfter)l.global_alias=n;
            for(auto* p:{b.Aliasing.pResourceBefore,b.Aliasing.pResourceAfter})if(p) {
                const auto a=reinterpret_cast<uintptr_t>(p);
                if(!resources.get(a)){inc(untracked_events);continue;}
                if(auto* r=resources.get(a,true))r->version=n;
                if(auto* s=state(list,generation,a,true))s->alias=n;
            }
        }
    }
    // Count recording occurrences, independent of throttled requirements diagnostics.
    void draw(List& l,bool indexed=true) noexcept {
        if(!enabled)return;inc(draws);inc(l.draws);
        if(l.t10_table) {
            const auto* h=l.table_heap;const auto* s=l.table_slot;
            // Diagnostic nodes never erase; retirement marks heap.live=false,
            // replacement changes its serial, and slot writes replace in place.
            const auto* d=l.table_diagnostics;
            l.sources[0].address=h&&s&&d&&!d->gap&&!d->overflow&&h->live&&h->serial==l.table_epoch&&s->type==DescriptorTableDiagnostics::srv&&
                !(s->flags&(DescriptorTableDiagnostics::invalidated|DescriptorTableDiagnostics::unsupported_srv))?s->resource:0;
        }
        for(unsigned i=0;i<l.sources.size();++i) {
            const auto& source=l.sources[i];const unsigned role=i==0?0:i==33?2:1;
            if(!source.address) {
                if(i==0||i==1||(i==33&&indexed)||source.va)l.selected_unknown|=1u<<role;
                continue;
            }
            const size_t start=hash(source.address)%l.selected.size();bool stored=false;
            for(size_t p=0;p<8;++p) {
                auto& key=l.selected[(start+p)%l.selected.size()];
                if(key==source.address){l.selected_roles[(start+p)%l.selected.size()]|=static_cast<unsigned char>(1u<<role);stored=true;break;}
                if(!key){key=source.address;l.selected_roles[(start+p)%l.selected.size()]=static_cast<unsigned char>(1u<<role);++l.selected_count;stored=true;break;}
            }
            if(!stored)l.gap=true;
        }
    }
    uint64_t begin_submission() noexcept {if(enabled)inc(submission);return submission;}
    uint64_t execute(List& l) noexcept {
        if(!enabled)return 0;
        inc(l.executes);if(l.last_sub==submission)return 0;l.last_sub=submission;
        if(l.executes==1){list_total=add(list_total,l.draws);if(l.draws<list_min)list_min=l.draws;if(l.draws>list_max)list_max=l.draws;}
        for(unsigned i=0;i<l.selected.size();++i)if(const auto a=l.selected[i]) {
            const auto* s=state(l.key,l.generation,a);
            // A re-executed list submits its recorded write-like operations
            // again. This is still a requested operation, never completion.
            if(s&&s->write)if(auto* r=resources.get(a))r->version=tick();
            sight(a);
            for(unsigned role=0;role<3;++role)if(l.selected_roles[i]&(1u<<role)) {
                auto& counts=repeats[role];auto* r=resources.get(a);
                if(!r||r->retired||!r->description){inc(counts.unknown);continue;}
                auto& seen=r->seen[role];if(seen.sub==submission)continue;
                if(!seen.sub)inc(counts.fresh);
                else if(seen.facts!=r->serial)inc(counts.unknown);
                else if(seen.version!=r->version)inc(counts.write);
                else inc(counts.same);
                seen.sub=submission;seen.version=r->version;seen.facts=r->serial;
            }
        }
        for(unsigned role=0;role<3;++role)if(l.selected_unknown&(1u<<role))inc(repeats[role].unknown);
        return l.draws;
    }
    void end_submission(uint64_t count) noexcept {
        if(!enabled)return;if(count)inc(submissions_with_draw);sub_total=add(sub_total,count);
        if(count<sub_min)sub_min=count;if(count>sub_max)sub_max=count;
    }
    void sight(uint64_t address) noexcept {
        if(!enabled)return;
        auto* r=resources.get(address);if(!r||r->retired||!r->description){inc(unknown);return;}
        inc(heaps[r->heap<=4?r->heap:0]);
        if(r->last_sub==submission)return;
        if(!r->last_sub)inc(new_address);
        else if(r->serial!=r->last_facts)inc(unknown);
        else if(r->version!=r->last_version)inc(rewritten);
        else inc(repeat);
        r->last_sub=submission;r->last_version=r->version;r->last_facts=r->serial;
    }
    void facts(char* text,size_t bytes,const char* prefix,const Source& source,unsigned slot) noexcept {
        const auto* r=resources.get(source.address);
        const D3D12_RESOURCE_DESC empty{};const auto& d=r?r->desc:empty;
        const char* heap=!r?"unknown":r->heap==1?"DEFAULT":r->heap==2?"UPLOAD":r->heap==3?"READBACK":r->heap==4?"CUSTOM":"unknown";
        std::snprintf(text,bytes,
            "%s;slot=%x;addr=%llx;va=%llx;bytes=%llx;obs=%u;heap=%s;kind=%u;desc=%x,%llx,%x,%x,%x,%x,%x,%x,%x,%x;align=%llx;page=%x;pool=%x;map=%llx;unmap=%llx;out=%llx;ptr=%llx;held=unknown;cpu=%llx;copy=%llx;wts=%llx;gap=%u;hooks=%u;serial=%llx;retired=%u;cov=partial",
            prefix,slot,source.address,source.va,source.bytes,unsigned(r&&r->description),heap,r?r->kind:0,
            unsigned(d.Dimension),d.Width,d.Height,unsigned(d.DepthOrArraySize),unsigned(d.MipLevels),unsigned(d.Format),d.SampleDesc.Count,d.SampleDesc.Quality,unsigned(d.Layout),unsigned(d.Flags),
            d.Alignment,r?r->page:0,r?r->pool:0,r?r->maps:0,r?r->unmaps:0,r?r->outstanding:0,r?r->map_pointers:0,r?r->cpu:0,r?r->copies:0,r?r->writes:0,
            unsigned(!r||!r->serial||r->map_gap||!r->hooks||clock==UINT32_MAX||resources.lost),unsigned(r&&r->hooks),r?r->serial:0,unsigned(r&&r->retired));
    }
    void state_text(char* text,size_t bytes,const List& l,uint64_t list,uint64_t generation,const Source& source,unsigned slot) noexcept {
        const auto* s=state(list,generation,source.address);const auto* r=resources.get(source.address);
        const auto write=(r&&r->cpu_serial>(s?s->write:0))?r->cpu_serial:(s?s->write:0);
        const auto earlier=s&&s->transition?s->earlier:r?r->barrier_serial:0;
        std::snprintf(text,bytes,
            "l3-state;slot=%x;addr=%llx;l=%llx;g=%llx;d=%llx;state=%s;tr=%llx;before=%x;after=%x;sr=%x;fl=%x;prior=%s;pr=%llx;pb=%x;pa=%x;ps=%x;pf=%x;write=%llx;bt=%u;bu=%u;ba=%u;gap=%u",
            slot,source.address,list,generation,l.draws,s&&s->transition?"observed":"unknown",s?s->transition:0,s?s->before:0,s?s->after:0,s?s->subresource:0,s?s->flags:0,
            earlier?"observed":"unknown",earlier,s&&s->transition?s->earlier_before:r?r->before:0,s&&s->transition?s->earlier_after:r?r->after:0,
            s&&s->transition?s->earlier_subresource:r?r->subresource:0,s&&s->transition?s->earlier_flags:r?r->barrier_flags:0,write,
            unsigned(write&&s&&s->transition>write),unsigned(write&&((s&&s->uav>write)||l.global_uav>write)),
            unsigned(write&&((s&&s->alias>write)||l.global_alias>write)),unsigned(l.gap||states.lost||resources.lost||clock==UINT32_MAX));
    }
    void counts(char* text,size_t bytes) const noexcept {
        std::snprintf(text,bytes,
            "l3-counts;unit=sub;present=unobserved;sub=%llx;recorded=%llx;subs=%llx;sub-total=%llx;sub-min=%llx;sub-max=%llx;list-total=%llx;list-min=%llx;list-max=%llx;new=%llx;repeat=%llx;rewrite=%llx;unknown=%llx;heaps=%llx,%llx,%llx,%llx,%llx;resource-loss=%llx;state-loss=%llx;patch-gap=%llx;list-loss=%llx;untracked=%llx;saturated=%u",
            submission,draws,submissions_with_draw,sub_total,sub_min==UINT64_MAX?0:sub_min,sub_max,list_total,list_min==UINT64_MAX?0:list_min,list_max,
            new_address,repeat,rewritten,unknown,heaps[0],heaps[1],heaps[2],heaps[3],heaps[4],resources.lost,states.lost,patch_gaps,lists.lost,untracked_events,unsigned(clock==UINT32_MAX||submission==UINT32_MAX));
    }
    void repeat_text(char* text,size_t bytes) const noexcept {
        std::snprintf(text,bytes,"l3-repeats;unit=sub;sub=%llx;roles=t10,vb,ib;new=%llx,%llx,%llx;repeat=%llx,%llx,%llx;rewrite=%llx,%llx,%llx;unknown=%llx,%llx,%llx;held-pointer-writes=unknown",
            submission,repeats[0].fresh,repeats[1].fresh,repeats[2].fresh,repeats[0].same,repeats[1].same,repeats[2].same,
            repeats[0].write,repeats[1].write,repeats[2].write,repeats[0].unknown,repeats[1].unknown,repeats[2].unknown);
    }
};
static_assert(sizeof(L3Observation::List)<=2048,"Observer lists live only in the heap-owned store");
static_assert(sizeof(L3Observation)<=6*1024*1024,"Fixed process-heap diagnostic budget");
}
