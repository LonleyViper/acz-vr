// J-3b6: the design section 6 "Step 2" adapter. Included by
// gpu_copy_step1_hooks.inl after Step1State and the ledger backend, inside
// d3d12_hook.cpp's anonymous namespace. Default OFF: every function here runs
// only from the shared step-1 owner when Step1State::step2 was latched
// (gpu_copy_step2_experiment + log_enabled + diagnostic_ownership_pins +
// gpu_copy_witness_observation + gpu_copy_early_device_hook).
//
// What differs from step 1 (everything else is the step-1 owner unchanged):
//  - prepare: ONE proxy READBACK buffer (COPY_DEST, 12 KiB, three 4 KiB
//    regions); no proxy UPLOAD buffer, no pattern, no transitions.
//  - Draw seam: at the first qualified matched DrawCPT draw, the draw's
//    CURRENTLY BOUND index buffer (exactly 9 x 16-bit indices), IA slot-0
//    vertex buffer (stride 64, <=4 KiB) and t10 SrtData element zero (416 B,
//    root SRV) are resolved from the proxy's own setter tracking (L3 per-list
//    sources + capture::State views) and the creation-time records (L3
//    resource record + J-142 upload pin). Each copyable role adds one
//    read-only CopyBufferRegion(readback, role*4096, game, offset, span) in
//    the game's list immediately before the original Draw. A refused role
//    records nothing and logs its exact reason. Game resources are never
//    written, mapped, transitioned or redirected.
//  - worker: Map each region after same-timeline completion AND observed
//    Reset closure, SHA-256 + capped hex, CPU-snapshot comparison (logged,
//    never a failure), index decode and range check (unexpected range FAILs).
//
// Lock order is step 1's: step-1 mutex -> hook-state mutex. No native call is
// made with either held except step1Native's own hook-state lookup.
static_assert(gpu_copy::step2::region_bytes==gpu_copy::owner::recipe_byte_cap,"step-2 regions reuse the owner recipe cap");
static_assert(sizeof(gpu_copy::step2::Facts)<=160,"Draw-seam fact scratch stays small");
bool step1CreateBuffer(Step1State&,D3D12_HEAP_TYPE,D3D12_RESOURCE_STATES,ID3D12Resource*&,HRESULT&,uint64_t) noexcept;
void step1Emit(const char*) noexcept;

// ------------------------------------------------------------- Draw seam
// Game thread, step-1 mutex held, no allocation, no logging. Writes the plan
// into the heap-owned Step1State exactly once (the attempt is being consumed).
__declspec(noinline) bool step2Resolve(Step1State& st,void* key) noexcept {
    using namespace gpu_copy::step2;
    auto& plan=st.plan;
    plan.index_start=st.start;plan.index_count=st.count;plan.base=st.base;plan.copyable=0;
    auto& s=state();std::lock_guard lock(s.mutex);
    const auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));
    const capture::State* b=nullptr;diagnostics::L3Observation::List* l=nullptr;const PipelineRecord* p=nullptr;
    if(it!=s.states.end()) {
        b=&it->second.bindings;l=l3_list_locked(it->second);
        const auto pi=s.pipelines.find(it->second.pso.get());if(pi!=s.pipelines.end())p=&pi->second;
    }
    if(b){plan.topology=b->topology;plan.strip_cut=b->pipeline.strip_cut;plan.list=b->list_identity;}
    if(l){plan.generation=l->generation;plan.draw=l->draws;}
    Facts f;
    for(unsigned role=0;role<role_count;++role) {
        f=Facts{};uint64_t relative=0,span=0;const diagnostics::L3Observation::Source* src=nullptr;
        if(!b||!l)f.shape="l3-list-unavailable";
        else if(role==role_ib) {
            src=&l->sources[33];relative=uint64_t(st.start)*ib_stride;span=uint64_t(ib_indices)*ib_stride;
            f.stride=b->index?b->index->stride:0;
            if(!st.indexed)f.shape="draw-not-indexed";
            else if(!b->index)f.shape="ib-unbound";
            else if(b->index->stride!=ib_stride||st.count!=ib_indices)f.shape="ib-shape-changed";
            else if(b->index->resource!=src->va||b->index->size!=src->bytes)f.shape="ib-trackers-disagree";
        } else if(role==role_vb) {
            src=&l->sources[1];span=src->bytes;
            const auto v=b->vertices.find(0);
            if(v!=b->vertices.end()){plan.vb_view_bytes=v->second.size;plan.vb_stride=v->second.stride;f.stride=v->second.stride;}
            if(!p||!p->drawcpt_ia_layout)f.shape="ia-layout-unverified";
            else if(v==b->vertices.end())f.shape="vb-unbound";
            else if(v->second.stride!=vb_stride)f.shape="vb-shape-changed";
            else if(v->second.resource!=src->va||v->second.size!=src->bytes)f.shape="vb-trackers-disagree";
        } else {
            src=&l->sources[0];span=srt_bytes;f.stride=srt_bytes;
            if(l->t10_parameter==UINT32_MAX)f.shape="t10-unresolved";
            else if(l->t10_table)f.shape="t10-descriptor-table-span-unknown";
            else if(src->bytes!=srt_bytes)f.shape="srt-shape-changed";
        }
        if(src) {
            f.address=src->address;f.va=src->va;f.view_bytes=src->bytes;
            if(auto* r=src->address?s.l3.resources.get(src->address):nullptr) {
                f.record=true;f.description=r->description;f.buffer=r->desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;
                f.retired=r->retired;f.serial=r->serial;f.width=r->desc.Width;f.gpu_va=r->gpu_va;f.kind=r->kind;f.heap=r->heap;
                f.cpu_events=r->cpu;
            }
            for(const auto& e:s.uploadPins.entries)if(e.reserved&&e.address==src->address) {
                f.pin=true;f.pin_finished=e.finished;f.pin_creation=e.creation;f.pin_lifetime=e.lifetime;
                f.pin_held=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(e.held));break;
            }
        }
        evaluate(f,role,relative,span,plan.roles[role]);
        if(plan.roles[role].copy)++plan.copyable;
    }
    uint64_t total=0;for(const auto& r:plan.roles)if(r.copy)total+=r.bytes;
    if(total>total_cap) {
        for(auto& r:plan.roles)if(r.copy){r.copy=false;r.reason="total-over-cap";}
        plan.copyable=0;
    }
    return plan.copyable!=0;
}
// Owner recipe bounds for every copyable region (source width, readback width).
__declspec(noinline) bool step2RecipesValid(const Step1State& st) noexcept {
    using namespace gpu_copy::step2;
    for(const auto& r:st.plan.roles) {
        if(!r.copy)continue;
        const gpu_copy::owner::CopyRecipe recipe{source_token,r.offset,private_token,r.dest_offset,r.bytes};
        if(!gpu_copy::owner::recipe_valid(recipe,r.width,readback_bytes))return false;
    }
    return st.plan.copyable!=0;
}
// Game thread, no lock held, immediately before the original Draw. The plan
// is immutable once the attempt was consumed. Sources are the J-142 held
// references (never a historical pointer); the destination is proxy-owned.
__declspec(noinline) void step2Record(ID3D12GraphicsCommandList* self,const Step1State& st,ID3D12Resource* readback) noexcept {
    using CopyFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64);
    const auto fn=reinterpret_cast<CopyFn>(step1Native(self,15));
    for(const auto& r:st.plan.roles)
        if(r.copy)fn(self,readback,r.dest_offset,reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(r.held)),r.offset,r.bytes);
}
// ------------------------------------------------------------- worker
bool step2Prepare(Step1State& st,const char*& why) noexcept {
    using namespace gpu_copy::step2;
    why=nullptr;
    if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,st.readback,st.readbackHr,readback_bytes))
        why="readback-create-failed";
    st.map.readback=st.readback;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=prepare;mode=step2;readback=%llx,heap=READBACK,state=COPY_DEST,"
        "width=%llu,hr=%08lx;regions=ib:0,vb:4096,srt:8192;region-cap=%llu;total-cap=%llu;charged=%llu;upload=none;pattern=none;"
        "transitions=none;game-resources=read-only-copy-source;game-writes=none;game-barriers=none",
        static_cast<unsigned long long>(step1Route(st.readback)),static_cast<unsigned long long>(readback_bytes),
        static_cast<unsigned long>(st.readbackHr),static_cast<unsigned long long>(region_bytes),
        static_cast<unsigned long long>(total_cap),static_cast<unsigned long long>(charged_bytes));
    step1Emit(st.text);
    return !why;
}
bool step2Read(Step1State& st) noexcept {
    using namespace gpu_copy::step2;
    bool all=st.plan.copyable!=0;
    for(unsigned i=0;i<role_count;++i) {
        const auto& r=st.plan.roles[i];if(!r.copy)continue;
        st.roleRead[i]=gpu_copy::owner::read_back(st.timeline,st.timelineTicket,true,st.map,private_token,
            r.dest_offset,r.bytes,st.roleBytes[i].data(),st.roleBytes[i].size());
        all=all&&st.roleRead[i];
    }
    return all;
}
void step2EmitRoles(Step1State& st) noexcept {
    using namespace gpu_copy::step2;
    static constexpr const char* sources[role_count]={"IA-IB","IA-VB0","t10-root-srv"};
    for(unsigned i=0;i<role_count;++i) {
        const auto& r=st.plan.roles[i];
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=role;role=%s;status=%s;reason=%s;source=%s;resource=%llx;serial=%llx;"
            "life=%llx;heap=%s;kind=%u;width=%llx;gpu-va=%llx;view-va=%llx;view-bytes=%llx;stride=%u;offset=%llx;span=%llx;"
            "dest=readback+%llx;state-rule=%s;pin=%s%s%s",role_names[i],r.copy?"copy":"refused",r.copy?"ok":r.reason,sources[i],
            static_cast<unsigned long long>(r.address),static_cast<unsigned long long>(r.serial),
            static_cast<unsigned long long>(r.lifetime),heap_name(r.heap),r.kind,static_cast<unsigned long long>(r.width),
            static_cast<unsigned long long>(r.gpu_va),static_cast<unsigned long long>(r.va),static_cast<unsigned long long>(r.view_bytes),
            r.stride,static_cast<unsigned long long>(r.offset),static_cast<unsigned long long>(r.bytes),
            static_cast<unsigned long long>(r.dest_offset),state_rule_name(r.heap),r.held?"held":"none",
            i==role_srt?";read-mask=":"",i==role_srt?srt_read_mask:"");
        step1Emit(st.text);
    }
}
// Worker, step-1 mutex held, after every copied region was mapped and read.
// Returns the static FAIL reason, or nullptr (then retire -> PASS/REFUSED).
const char* step2Evaluate(Step1State& st) noexcept {
    using namespace gpu_copy::step2;
    auto& plan=st.plan;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=map;mode=step2;subresource=0;hr=%08lx;maps=%u;unmaps=%u;"
        "unmap-written=0:0;roles-read=ib:%u,vb:%u,srt:%u;read-ranges=ib:0+%llu,vb:4096+%llu,srt:8192+%llu",
        static_cast<unsigned long>(st.map.mapHr),st.map.maps,st.map.unmaps,unsigned(st.roleRead[role_ib]),unsigned(st.roleRead[role_vb]),
        unsigned(st.roleRead[role_srt]),static_cast<unsigned long long>(plan.roles[role_ib].copy?plan.roles[role_ib].bytes:0),
        static_cast<unsigned long long>(plan.roles[role_vb].copy?plan.roles[role_vb].bytes:0),
        static_cast<unsigned long long>(plan.roles[role_srt].copy?plan.roles[role_srt].bytes:0));
    step1Emit(st.text);
    const char* fail=nullptr;
    // CPU snapshot (J-131) and observed CPU events since insertion, under the
    // hook-state mutex (nested under the step-1 mutex: the documented order).
    {auto& s=state();std::lock_guard lock(s.mutex);
     const auto& c=s.cpuSnapshot;
     st.cpuReason=c.reason;st.cpuAttempts=c.attempts;
     const bool sample=c.stable&&c.completed.accepted;
     if(sample){st.cpuList=c.completed.list;st.cpuGen=c.completed.generation;st.cpuDraw=c.completed.draw;st.cpuMs=c.sampled_ms;}
     static constexpr unsigned cpuRange[role_count]={2,1,0}; // CpuUploadSnapshot ranges are t10, vb, ib
     for(unsigned i=0;i<role_count;++i) {
         const auto& r=plan.roles[i];if(!r.copy)continue;
         // A new incarnation/retirement of the source after insertion stops the
         // experiment. Observed CPU Map/Unmap/Write events are logged only:
         // persistently mapped writes are invisible either way (writer
         // exclusion stays unproven), so a count is evidence, not a verdict.
         if(const auto* now=s.l3.resources.get(r.address)) {
             st.cpuEventsAfter[i]=now->cpu>=r.cpu_events?now->cpu-r.cpu_events:UINT64_MAX;
             if((now->serial!=r.serial||now->retired)&&!fail)fail="observed-retirement-after-insert";
         } else if(!fail)fail="resource-record-lost";
         if(!sample){st.roleCpu[i]=c.attempts?"sample-not-stable":"absent";continue;}
         const unsigned k=cpuRange[i];const auto& x=c.completed.ranges[k];
         size_t at=0;for(unsigned j=0;j<k;++j)at+=size_t(c.completed.ranges[j].bytes);
         st.cpuHash[i]=c.hashes[k];
         if(x.resource!=r.address)st.roleCpu[i]="different-resource";
         else if(x.lifetime!=r.lifetime)st.roleCpu[i]="different-lifetime";
         else if(r.offset<x.offset||r.bytes>x.bytes||r.offset-x.offset>x.bytes-r.bytes)st.roleCpu[i]="span-not-covered";
         else if(at+size_t(x.bytes)>c.first.size())st.roleCpu[i]="sample-out-of-range";
         else {
             const auto d=gpu_copy::step1::first_mismatch(c.first.data()+at+size_t(r.offset-x.offset),st.roleBytes[i].data(),size_t(r.bytes));
             st.cpuFirstDiff[i]=d;st.roleCpu[i]=d<0?"equal":"mismatch";
         }
     }}
    for(unsigned i=0;i<role_count;++i) {
        const auto& r=plan.roles[i];if(!r.copy)continue;
        step1Sha256(st.roleBytes[i].data(),size_t(r.bytes),st.roleSha[i]);
        char hex[129];const size_t shown=r.bytes<64?size_t(r.bytes):64;
        gpu_copy::step1::hex(st.roleBytes[i].data(),shown,hex);
        const long long age=st.cpuMs&&st.tInsert?static_cast<long long>(st.tInsert)-static_cast<long long>(st.cpuMs):0;
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=role-data;role=%s;bytes=%llu;hex%zu=%s;sha256=%s;cpu=%s;"
            "cpu-diff=%lld;cpu-fnv=%016llx;cpu-id=%llx:%llx:%llx;cpu-age-ms=%lld;cpu-events-after=%llu;"
            "limits=cpu-earlier-sample,no-coherence",role_names[i],static_cast<unsigned long long>(r.bytes),shown,hex,
            st.roleSha[i],st.roleCpu[i],st.cpuFirstDiff[i],static_cast<unsigned long long>(st.cpuHash[i]),
            static_cast<unsigned long long>(st.cpuList),static_cast<unsigned long long>(st.cpuGen),
            static_cast<unsigned long long>(st.cpuDraw),age,static_cast<unsigned long long>(st.cpuEventsAfter[i]));
        step1Emit(st.text);
    }
    if(plan.roles[role_ib].copy) {
        uint32_t indices[ib_indices]{};decode(st.roleBytes[role_ib].data(),ib_stride,ib_indices,indices);
        const uint64_t vertices=plan.vb_stride?plan.vb_view_bytes/plan.vb_stride:0;
        index_range::Result range;
        if(vertices) {
            index_range::evaluate(st.roleBytes[role_ib].data(),size_t(plan.roles[role_ib].bytes),ib_stride,0,ib_indices,plan.base,vertices,
                plan.topology,plan.strip_cut,range);
            st.indexStatus=range.reason;
            if(!range.verified&&!fail)fail="index-range-unexpected";
        } else st.indexStatus="unverified-no-vertex-count";
        int n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=index;count=%u;stride=%u;start=%u;base=%d;indices=",
            ib_indices,ib_stride,plan.index_start,plan.base);
        for(unsigned i=0;i<ib_indices&&n>0&&size_t(n)<sizeof st.text;++i)
            n+=std::snprintf(st.text+n,sizeof st.text-n,i?",%u":"%u",indices[i]);
        if(n>0&&size_t(n)<sizeof st.text)
            std::snprintf(st.text+n,sizeof st.text-n,";min=%llu;max=%llu;fetched=%u;vertices=%llu;vb-bytes=%llu;vb-stride=%u;"
                "topology=%u;strip-cut=%x;range=%s;draw-use-unproven",static_cast<unsigned long long>(range.minimum),
                static_cast<unsigned long long>(range.maximum),unsigned(range.fetched),static_cast<unsigned long long>(vertices),
                static_cast<unsigned long long>(plan.vb_view_bytes),plan.vb_stride,plan.topology,plan.strip_cut,st.indexStatus);
        step1Emit(st.text);
    }
    return fail;
}
// Final per-role summary, immediately before the verdict (worker or refusal).
void step2EmitResult(Step1State& st) noexcept {
    using namespace gpu_copy::step2;
    const char* status[role_count];const char* why[role_count];unsigned read=0;
    for(unsigned i=0;i<role_count;++i) {
        const auto& r=st.plan.roles[i];
        if(!st.recorded&&r.copy){status[i]="copyable-not-recorded:";why[i]=st.armRefusal?st.armRefusal:"none";}
        else if(r.copy){status[i]=st.roleRead[i]?"read":"recorded-unread";why[i]="";}
        else {status[i]="refused:";why[i]=r.reason;}
        if(st.roleRead[i])++read;
    }
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;STEP2_RESULT;ib=%s%s;vb=%s%s;srt=%s%s;copied=%u/3;read=%u;index=%s;"
        "cpu=ib:%s,vb:%s,srt:%s;cpu-sample=%s,attempts:%zu;srt-read-mask=%s;game-writes=none;game-barriers=none;capture=disabled",
        status[role_ib],why[role_ib],status[role_vb],why[role_vb],status[role_srt],why[role_srt],st.recorded?st.plan.copyable:0u,read,st.indexStatus,
        st.roleCpu[role_ib],st.roleCpu[role_vb],st.roleCpu[role_srt],st.cpuReason,st.cpuAttempts,srt_read_mask);
    step1Emit(st.text);
}
