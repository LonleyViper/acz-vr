// J-3b7: the GPU-copy "Step 3" adapter. Included by gpu_copy_step1_hooks.inl
// after Step1State and the step-2 adapter, inside d3d12_hook.cpp's anonymous
// namespace. Default OFF: every function here runs only from the shared step-1
// owner when Step1State::step3 was latched (gpu_copy_step3_experiment +
// log_enabled + diagnostic_ownership_pins + gpu_copy_witness_observation +
// gpu_copy_early_device_hook).
//
// Sequence (single attempt, one transaction, step-1 witness/ledger/fence):
//  1. Draw seam, first qualified matched DrawCPT draw: OBSERVE only. The
//     draw's shape (PSO output facts, RS/OM tracking, t10 root SRV, draw args)
//     is copied under the hook-state mutex and judged by step3::decide. Nothing
//     is recorded into the game's list. A refusal ends the experiment (REFUSED).
//  2. Owner context (worker): take the J-131 CPU snapshot of THIS draw's
//     SrtData (same draw ordinal, resource and VA), compute the substituted
//     blobs (zero-offset control, left, right), create the private RT/DS
//     (exact PSO formats, viewport extents), RTV/DSV heaps (CPU-only),
//     UPLOAD (blobs written once, writes then cease) and READBACK (four
//     footprints), then set `armed`.
//  3. Draw seam, next qualified draw with the same shape: the attempt is
//     consumed; in the game's list, before the original Draw:
//       OMSet(private RTV/DSV)
//       4x {ClearRTV, [ClearDSV], [SRV t10 = blob], Draw(same args),
//           Barrier RT->COPY_SOURCE, CopyTextureRegion(readback footprint i),
//           Barrier COPY_SOURCE->RT}
//       SRV t10 = the game's original VA; OMSet(the game's original arguments)
//     then the original Draw is forwarded unchanged. Barriers touch only the
//     proxy RT. No game resource is written, mapped, transitioned or bound
//     somewhere new; every state changed is re-set from the proxy's tracking.
//  4. Worker: after same-timeline completion AND observed Reset closure, Map
//     the readback once, then (no lock held) pixel statistics, SHA-256, PNG.
//
// Lock order is step 1's: step-1 mutex -> hook-state mutex. The Draw seam
// performs no allocation, logging, resource creation or Map.
static_assert(sizeof(gpu_copy::step3::Shape)<=448,"Draw-seam shape stays small (heap-owned)");
#ifdef ACZ_VR_OFFLINE_TEST
unsigned g_step3CandidateCap=0; // fixture override of the 400-candidate selection cap
#endif
inline uint64_t step3CandidateCap() noexcept {
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step3CandidateCap)return g_step3CandidateCap;
#endif
    return gpu_copy::step3::candidate_cap;
}
void step1Finish(Step1State&,const char*,const char*) noexcept;
// J-3b9 world-snapshot barrier evidence (set by d3d12_hook.cpp's barrier seams).
std::atomic<bool> g_step5EnhancedUntracked{false};   // an enhanced-barrier hook could not be installed
std::atomic<uintptr_t> g_step5WorldWatch{0};          // the selection's game RT (world source), 0 = none
// Every capture table of the list has the legacy ResourceBarrier seam (l3Barriers).
__declspec(noinline) bool step3BarriersHooked_locked(const ListState& list) noexcept {
    auto& s=state();bool any=false;
    for(unsigned i=1;i<12;++i)if(auto* table=list.tables[i]) {
        auto it=s.lists.find(table);if(it==s.lists.end()||!it->second.originals[26])return false;any=true;
    }
    return any;
}

// World candidates are observed only through LIVE creation/view arguments. Selection
// and Draw consult saved tables; neither dereferences the historical world address.
// Reuse the buffer path's Release owner/dispatch, without adding QI or ownership.
bool step5Configured() noexcept;
bool step5WorldCovered_locked(uintptr_t address) noexcept {
    const auto& records=state().step5WorldLifetimes;auto r=records.find(address);
    return r!=records.end()&&r->second.serial&&releaseCovered_locked(r->second.table);
}
void step5NoteWatchLifetime_locked(uintptr_t address,bool created) noexcept;
void step5TrackWorldLifetime_locked(IUnknown* object,bool created) {
    if(!step3Mode()||!step5Configured())return;
    auto& s=state();const auto address=reinterpret_cast<uintptr_t>(object);auto* table=vtable(object);
    // Exactly the Release half of the existing buffer lifetime mechanism.
    // Failure leaves no positive record, even if a previous observation succeeded.
    const bool covered=trackMetadataLifetime_locked(object)&&releaseCovered_locked(table);
    if(!covered){s.step5WorldLifetimes.erase(address);step5NoteWatchLifetime_locked(address,false);return;}
    auto& record=s.step5WorldLifetimes[address];
    const bool renewed=created||record.table!=table||!record.serial;
    if(renewed)record.serial=++s.step5WatchClock;
    record.table=table;
    // Creation's caller publishes the watch serial; an RTV table replacement or
    // re-established coverage must likewise invalidate earlier recording facts.
    if(!created&&renewed)step5NoteWatchLifetime_locked(address,true);
}
uint64_t step5WatchSerial_locked() noexcept {
    const auto watch=g_step5WorldWatch.load();
    return watch&&step5WorldCovered_locked(watch)?state().step5WatchSerial:0;
}
void step5NoteWatchLifetime_locked(uintptr_t address,bool created) noexcept {
    auto& s=state();const auto watch=g_step5WorldWatch.load();
    if(!watch||address!=watch)return;
    s.step5WatchSerial=created&&step5WorldCovered_locked(address)?++s.step5WatchClock:0;
}
// Set only under the hook mutex, in step-1 -> hook lock order. A new selection
// invalidates earlier per-recording facts; creation serials also reject address reuse.
void step5SetWorldWatch(uintptr_t watch) noexcept {
    auto& s=state();std::lock_guard lock(s.mutex);
    if(g_step5WorldWatch.load()==watch)return;
    g_step5WorldWatch.store(watch);++s.step5WatchEpoch;
    s.step5WatchSerial=watch&&step5WorldCovered_locked(watch)?++s.step5WatchClock:0;
    s.step5RtCarriedKnown=false;s.step5RtPending=0;s.step5RtResource=0;s.step5RtSerial=0;s.step5RtQueue=nullptr;
    for(auto& entry:s.states) {
        auto& t=entry.second.step3;
        t.rtTransition=t.rtBad=t.rtPendingCounted=false;t.rtAfter=0;t.rtWhy=nullptr;
        t.rtResource=0;t.rtWatchEpoch=0;t.rtSerial=0;t.rtGeneration=0;
        // enhanced is recording-wide; a watch change cannot make it tracked.
    }
}
// Per-native-Draw reservation, including early refusals/exceptions. Nesting uses
// a stack so reentrant hooks cannot release an outer Draw's reservation.
struct Step3OmPin;
thread_local Step3OmPin* g_step3OmPin=nullptr;
struct Step3OmPin {
    Step3OmPin* previous=g_step3OmPin;unsigned slot=UINT_MAX;
    Step3OmPin() noexcept {g_step3OmPin=this;}
    Step3OmPin(const Step3OmPin&)=delete;
    ~Step3OmPin() {
        if(slot!=UINT_MAX){auto& s=state();std::lock_guard lock(s.mutex);--s.step3Om.pins[slot];}
        g_step3OmPin=previous;
    }
};
// ------------------------------------------------------------- Draw seam
// Game thread, step-1 mutex held. `restore`: also capture the game's current
// Hook-state mutex held: the bound OM descriptors are unchanged since the bind (J-3b9).
__declspec(noinline) bool step3OmProved_locked(const ListState::Step3Track& t) noexcept {
    // J-3b9c (review 2): the restore binds the PRIVATE bind-time copies, so neither a later write
    // to nor the release of the game's descriptor heap matters. Proved when the copies were
    // taken and their exact ring version is still present. The age bound is conservative;
    // a reservation, acquired atomically with validation, protects the restore interval.
    auto& s=state();
    return t.omSet&&t.privOk&&s.step3Om.ready&&
        s.step3Om.versions[t.privSeq%HookState::Step3OmRing::binds]==t.privSeq&&
        s.step3Om.seq-t.privSeq<HookState::Step3OmRing::binds/2;
}
// OM arguments and t10 VA for the restore (insertion draw only).
__declspec(noinline) void step3Capture(Step1State& st,void* key,gpu_copy::step3::Shape& out,bool restore) noexcept {
    using namespace gpu_copy::step3;
    out=Shape{};
    out.indexed=st.s3.aIndexed;out.count=st.s3.aCount;out.instances=st.s3.aInstances;out.start=st.s3.aStart;out.base=st.s3.aBase;
    out.start_instance=st.s3.aStartInstance;
    auto& s=state();std::lock_guard lock(s.mutex);
    const auto it=s.states.find(static_cast<ID3D12GraphicsCommandList*>(key));
    if(it==s.states.end())return; // tracking_complete stays false
    const auto& ls=it->second;const auto& b=ls.bindings;
    out.tracking_complete=b.tracking_complete&&ls.capture_hooks;
    out.tracking_gap=b.tracking_gap?b.tracking_gap:ls.observation_gap;
    out.topology=b.topology;out.list=b.list_identity;
    auto* pso=ls.pso.get();out.pso_route=step1Route(pso);
    if(const auto p=s.pipelines.find(pso);p!=s.pipelines.end())out.pso=p->second.output;
    const auto& t=ls.step3;
    out.rs_known=out.om_known=t.known;out.rs_gap=out.om_gap=t.gap;
    out.viewports=t.viewports;out.scissors=t.scissors;
    out.vp0={t.vp0.TopLeftX,t.vp0.TopLeftY,t.vp0.Width,t.vp0.Height,t.vp0.MinDepth,t.vp0.MaxDepth};
    out.sc0={int32_t(t.sc0.left),int32_t(t.sc0.top),int32_t(t.sc0.right),int32_t(t.sc0.bottom)};
    out.vp_right=t.right;out.vp_bottom=t.bottom;out.vp_left=t.left;out.vp_top=t.top;
    out.om_set=t.omSet;out.om_count=t.omCount;out.om_single=t.omSingle!=FALSE;out.om_dsv=t.dsv;
    // J-3b9 (review P2): the restore re-issues CPU descriptor HANDLES; it restores the
    // bind-time OM state only if no observed write/copy changed those descriptors since.
    out.om_desc_proved=step3OmProved_locked(t);
    if(restore&&out.om_desc_proved) {
        auto* pin=g_step3OmPin;
        if(!pin||pin->slot!=UINT_MAX)out.om_desc_proved=false;
        else {pin->slot=unsigned(t.privSeq%HookState::Step3OmRing::binds);++s.step3Om.pins[pin->slot];}
    }
    out.root_route=step1Route(ls.root.get());
    out.world_barriers_tracked=step3BarriersHooked_locked(ls)&&!g_step5EnhancedUntracked.load();
    out.world_rt_transition=t.rtTransition;out.world_rt_after=t.rtAfter;out.world_enhanced=t.enhanced;
    out.world_barrier_why=t.rtBad?t.rtWhy:nullptr;
    const auto* recording=l3_list_locked(ls);
    const auto watch=g_step5WorldWatch.load();const uint64_t serial=step5WatchSerial_locked();
    out.world_lifetime_covered=watch&&step5WorldCovered_locked(watch)&&serial;
    if(!recording||!recording->generation||t.rtGeneration!=recording->generation||
       !watch||!serial||t.rtResource!=watch||t.rtWatchEpoch!=s.step5WatchEpoch||t.rtSerial!=serial) {
        out.world_rt_transition=false;
    }
    // J-world-hold: the exact sub-condition (first failing, in the order of the test above; diagnostics only, no behaviour).
    out.world_entry_sub=nullptr;
    if(!out.world_rt_transition) {
        out.world_entry_sub=!t.rtTransition?"no-transition-tracked":!recording?"recording-missing":!recording->generation?"generation-zero":
            t.rtGeneration!=recording->generation?"generation-mismatch":!watch?"watch-missing":!serial?"serial-missing":
            t.rtResource!=watch?"resource-mismatch":t.rtWatchEpoch!=s.step5WatchEpoch?"watch-epoch-mismatch":
            t.rtSerial!=serial?"serial-mismatch":"unclassified";
    }
    // J-155: carried entry states are never a stable pre-recording prerequisite.
    out.world_inherited_ok=false; // J-155: carried histories never authorize injection
    // J-3b7b diagnostic: describe the game's current RT 0 when the proxy saw its view.
    if(!t.omCount)out.game_rt_status="unresolved:no-rtv-bound";
    else {
        out.game_rtv=t.rtv[0].ptr;out.game_rt_status="unresolved:rtv-not-tracked";
        for(const auto& v:s.step3Views)if(v.handle&&v.handle==t.rtv[0].ptr&&!v.depth) {
            out.game_rt=v.resource;out.game_rt_status="unresolved:no-description";
            if(v.described) {
                out.game_rt_width=v.width;out.game_rt_height=v.height;out.game_rt_format=v.format;out.game_rt_heap=v.heap;
                out.game_rt_samples=v.samples;out.game_rt_flags=v.flags;out.game_rt_dim=v.dim;out.game_rt_view_sub0=v.sub0;
                out.game_rt_status="resolved";
            }
            break;
        }
    }
    if(auto* l=l3_list_locked(ls)) {
        out.t10_parameter=l->t10_parameter;out.t10_table=l->t10_table;out.t10_va=l->sources[0].va;
        out.t10_address=l->sources[0].address;out.draw_ordinal=l->draws;out.generation=l->generation;
        out.vb_address=l->sources[1].address;out.ib_address=l->sources[33].address; // J-3b7d IA resource keys
        // J-180 (Codex review P2): a DrawACM_v draw's VERTEX shader reads t10: with a vertex-stage binding of its own (a root that also carries a
        // GEOMETRY-visible t10 in another parameter) that binding replaces the cockpit's. No vertex-stage binding: the general one, as before.
        if(l->t10_vs_parameter!=UINT32_MAX)
            if(const auto p=s.pipelines.find(pso);p!=s.pipelines.end()&&p->second.acm_world_matched) {
                out.t10_parameter=l->t10_vs_parameter;out.t10_table=l->t10_vs_table;out.t10_va=l->vs_source.va;out.t10_address=l->vs_source.address;out.t10_vs=true;
            }
    }
    // J-3b7d: IA views from the capture setter tracking (values only).
    if(b.index){out.ib_va=b.index->resource;out.ib_bytes=b.index->size;out.ib_stride=b.index->stride;}
    if(const auto v=b.vertices.find(0);v!=b.vertices.end()){out.vb_va=v->second.resource;out.vb_bytes=v->second.size;out.vb_stride=v->second.stride;}
    out.strip_cut=b.pipeline.strip_cut;
    if(out.t10_parameter!=UINT32_MAX) {
        const auto r=b.roots.find(out.t10_parameter);
        out.t10_root_value=r!=b.roots.end()&&r->second.kind==metadata::ParameterKind::srv&&r->second.value==out.t10_va;
    }
    if(restore) {
        auto& s3=st.s3;
        // J-3b9c: restore from the private copies (other handle values, the same bound views).
        s3.omCount=t.omCount;s3.omSingle=t.omSingle;s3.omRtv=t.privOk?t.privRtv:t.rtv;s3.omDsv=t.dsv;s3.omDsvHandle=t.privOk?t.privDsv:t.dsvHandle;
        s3.t10=out.t10_parameter;s3.t10Original=out.t10_va;
    }
}
bool guardedCpuRead(const void* source,void* dest,size_t bytes) noexcept; // d3d12_hook.cpp (SEH-guarded copy)
bool cpuMapCoverage_locked(const diagnostics::L3Observation::Resource* r) noexcept; // d3d12_hook.cpp (J-131/J-142)
// Hook-state mutex held. A CPU-visible view of `bytes` at GPU VA `va` inside the
// committed UPLOAD buffer `address`. J-3b9 (review issue 29, P2): the SAME
// coverage/currentness rules as the J-131/J-142 CPU snapshot contract
// (cpuRequestCurrent_locked), not a weaker cached-pointer check:
//  * the native binding observer is enabled (its lifetime tracking is complete);
//  * the L3 creation record is live (not retired) for an UPLOAD buffer;
//  * Map/Unmap interception covers this resource (cpuMapCoverage_locked: our
//    l3Map/l3Unmap are installed in the resource's captured table, no pending
//    protection change);
//  * the native lifetime token names this address, the observed mapping belongs
//    to that lifetime, exactly one outstanding Map (subresource 0), no map gap or
//    map in flight;
//  * the J-142 pin is held for the same creation, lifetime, map generation and
//    mapped pointer (uploadPins.lookup).
// It establishes WHERE the bytes are, not WHEN the game wrote them: that the
// game's CPU writes precede recording the draw is an ASSUMPTION (D-223), not
// witnessed. nullptr or why.
const char* step3MappedSpan_locked(uint64_t address,uint64_t va,uint64_t bytes,const uint8_t*& out) noexcept {
    auto& s=state();out=nullptr;
    if(!s.nativeBindings.enabled())return "binding-observer-disabled";
    const auto* r=address?s.l3.resources.get(address):nullptr;
    if(!r||!r->description)return "no-creation-record";
    if(r->retired||!r->serial)return "resource-retired";
    if(r->desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||r->heap!=D3D12_HEAP_TYPE_UPLOAD)return "not-upload-buffer";
    if(!cpuMapCoverage_locked(r))return "map-unmap-coverage-incomplete";
    if(!r->mapped||r->mapped_subresource!=0||r->map_gap||r->map_inflight)return "not-mapped";
    if(r->outstanding!=1)return "outstanding-map-count";
    const auto token=s.nativeBindings.resource_token(address);
    if(token.id!=address||!token.generation)return "native-lifetime-unknown";
    if(r->mapped_lifetime!=token.generation)return "map-lifetime-mismatch";
    if(const char* why=s.uploadPins.lookup(address,r->serial,token.generation,r->map_generation,r->mapped))return why;
    if(!r->gpu_va||va<r->gpu_va)return "gpu-va-unknown";
    const uint64_t off=va-r->gpu_va;
    if(!bytes||off>r->desc.Width||bytes>r->desc.Width-off)return "span-exceeds-resource";
    out=static_cast<const uint8_t*>(r->mapped)+off;
    return nullptr;
}
// Draw seam, step-1 mutex held (nests the hook-state mutex, documented order).
// J-3b7e INLINE substitution: read THIS draw's live 416 B SrtData at its own t10
// VA from the game's pinned, mapped UPLOAD memory (ASSUMED, not witnessed: the
// game CPU-writes it before recording the draw and does not rewrite it before
// that frame's fence; the read proves the location only), then
// derive the three blobs with the fixed-size delta form (control = raw copy).
// Heap-owned outputs; no allocation, logging or native call.
__declspec(noinline) bool step3InlineSrt(Step1State& st) noexcept {
    auto& s3=st.s3;const uint8_t* p=nullptr;
    {auto& s=state();std::lock_guard lock(s.mutex);
     if(const char* why=step3MappedSpan_locked(s3.live.t10_address,s3.live.t10_va,416,p)){s3.freshness=why;++s3.freshUnreadable;return false;}
     if(!guardedCpuRead(p,s3.liveSrt.data(),416)){s3.freshness="read-fault";++s3.freshUnreadable;return false;}}
    gpu_copy::step3::DeltaFacts f;
    if(const char* why=gpu_copy::step3::delta_substitute(s3.liveSrt.data(),s3.inlineBlobs,f)){s3.freshness=why;++s3.freshUnreadable;return false;}
    s3.freshness="inline-live";s3.srcVa=s3.live.t10_va;s3.srcTick=GetTickCount64();
    return true;
}
// true: the attempt may be consumed at this draw. Never consumes anything itself.
__declspec(noinline) bool step3Gate(Step1State& st,void* key) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;
    if(s3.refusal||s3.finished)return false;
    s3.aIndexed=st.indexed;s3.aCount=st.count;s3.aInstances=st.instances;s3.aStart=st.start;s3.aBase=st.base;s3.aStartInstance=st.startInstance;
    if(!s3.observed) {
        // J-3b7d selection: record a fixed-size candidate; the worker evaluates it.
        if(s3.selectionClosed||s3.recorded>=step3CandidateCap())return false;
        if(s3.ringHead-s3.ringTail>=candidate_ring){if(s3.dropped<UINT64_MAX)++s3.dropped;return false;}
        auto& slot=s3.ring[size_t(s3.ringHead%candidate_ring)];
        step3Capture(st,key,slot.shape,false);
        slot.seq=++s3.recorded;slot.tick=GetTickCount64();++s3.ringHead;
        return false;
    }
    if(!s3.armed){if(s3.waitingDraws<UINT64_MAX)++s3.waitingDraws;return false;}
    step3Capture(st,key,s3.live,true);
    // Matching rule (J-3b7e): PSO + draw arguments + ordinal within the list
    // recording, then the full shape (no t10 VA/resource: the game's SrtData
    // moves through a ring of upload buffers every frame). A draw with the same
    // PSO and arguments at another ordinal is counted (`same-signature-other-
    // ordinal`) and never used: if the order shifts, the attempt is refused.
    if(s3.live.draw_ordinal!=s3.shape.draw_ordinal) {
        if(s3.live.pso_route==s3.shape.pso_route&&s3.live.count==s3.shape.count&&s3.live.start==s3.shape.start&&
           s3.live.base==s3.shape.base&&s3.live.instances==s3.shape.instances)++s3.orderShifted;
        else ++s3.skipped;
        return false;
    }
    const char* why=same_shape(s3.shape,s3.live);
    Targets now;
    if(!why)why=decide(s3.live,now); // the current state must still be sound and recoverable
    if(!why&&(now.width!=s3.targets.width||now.height!=s3.targets.height||now.color.name!=s3.targets.color.name||
              now.has_depth!=s3.targets.has_depth||now.depth_clear!=s3.targets.depth_clear))why="targets-shape-changed";
    if(why){if(s3.mismatches<UINT64_MAX)++s3.mismatches;if(!s3.mismatch)s3.mismatch=why;return false;}
    return step3InlineSrt(st); // not consumed when the live bytes cannot be read soundly
}
__declspec(noinline) bool step3PlanValid(const Step1State& st) noexcept {
    const auto& s3=st.s3;const auto& t=s3.targets;
    return s3.armed&&t.commands&&t.commands<=(s3.wholeSet?gpu_copy::step3::set_command_cap:gpu_copy::step3::command_cap)&&
        s3.t10==s3.live.t10_parameter&&
        s3.t10Original&&s3.uploadVa&&t.readback_bytes&&s3.pixelBytes==t.readback_bytes&&s3.omCount>=1;
}
__declspec(noinline) bool step3PrivateReady(const Step1State& st) noexcept {
    const auto& s3=st.s3;
    return s3.rt&&s3.upload&&s3.uploadPtr&&s3.readback&&s3.rtvHeap&&s3.rtv.ptr&&s3.pixels&&
        (!s3.targets.has_depth||(s3.ds&&s3.dsvHeap&&s3.dsv.ptr))&&
        (!s3.wholeSet||(s3.rt2&&s3.rtvHeap2&&s3.rtv2.ptr&&(!s3.targets.has_depth||(s3.ds2&&s3.dsvHeap2&&s3.dsv2.ptr))));
}
// ------------------------------------------------------------- whole set (J-3b7f), game thread
using Step3OmFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
using Step3SrvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS);
using Step3DrawIndexedFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
using Step3DrawFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
// After the LAST member (or at the list's Close): both persistent RTs to the
// READBACK footprints (left 0, right 1), full target. 6 commands. No lock held.
__declspec(noinline) void step3RecordSetCopies(ID3D12GraphicsCommandList* self,Step1State& st) noexcept {
    using BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    using CopyFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,
        const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
    auto& s3=st.s3;const auto& t=s3.targets;
    const auto barrier=reinterpret_cast<BarrierFn>(step1Native(self,26));
    const auto copy=reinterpret_cast<CopyFn>(step1Native(self,16));
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=0;
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    dst.pResource=s3.readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT(s3.shape.pso.rtv[0]);dst.PlacedFootprint.Footprint.Width=t.crop_w;
    dst.PlacedFootprint.Footprint.Height=t.crop_h;dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=UINT(t.row_pitch);
    const D3D12_BOX box{t.crop_left,t.crop_top,0,t.crop_left+t.crop_w,t.crop_top+t.crop_h,1};
    for(unsigned eye=0;eye<2;++eye) {
        ID3D12Resource* rt=eye?s3.rt2:s3.rt;
        b.Transition.pResource=rt;b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier(self,1,&b);
        src.pResource=rt;dst.PlacedFootprint.Offset=uint64_t(eye)*t.footprint_stride;copy(self,&dst,0,0,0,&src,&box);
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier(self,1,&b);
    }
    s3.issued.fetch_add(gpu_copy::step3::set_copies);
}
// One member: [the one-time clears of both pairs], left {OMSet, SRV, Draw},
// right {OMSet, SRV, Draw}, restore {SRV original, OMSet game} = 8 commands.
// The member's live blobs go into its own private UPLOAD slots first.
__declspec(noinline) void step3RecordSetMember(ID3D12GraphicsCommandList* self,Step1State& st,unsigned idx,bool first,bool last) noexcept {
    using namespace gpu_copy::step3;
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    auto& s3=st.s3;const auto& t=s3.targets;const auto& d=s3.live;
    const auto om=reinterpret_cast<Step3OmFn>(step1Native(self,46));
    const auto srv=reinterpret_cast<Step3SrvFn>(step1Native(self,40));
    void* const draw=step1Native(self,d.indexed?13:12);
    uint8_t* slot=s3.uploadPtr+uint64_t(2*idx)*blob_stride;
    std::memcpy(slot,s3.inlineBlobs[1].data(),size_t(blob_bytes));std::memcpy(slot+blob_stride,s3.inlineBlobs[2].data(),size_t(blob_bytes));
    std::atomic_thread_fence(std::memory_order_release);
    unsigned n=0;
    if(first) {
        const auto clearRtv=reinterpret_cast<ClearRtvFn>(step1Native(self,48));
        const auto clearDsv=reinterpret_cast<ClearDsvFn>(step1Native(self,47));
        static constexpr FLOAT clear[4]={0,0,0,0};
        const D3D12_CLEAR_FLAGS flags=D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH|(t.depth.stencil?D3D12_CLEAR_FLAG_STENCIL:0));
        clearRtv(self,s3.rtv,clear,0,nullptr);clearRtv(self,s3.rtv2,clear,0,nullptr);n+=2;
        if(t.has_depth){clearDsv(self,s3.dsv,flags,t.depth_clear,0,0,nullptr);clearDsv(self,s3.dsv2,flags,t.depth_clear,0,0,nullptr);n+=2;}
    }
    const D3D12_GPU_VIRTUAL_ADDRESS va=s3.uploadVa+uint64_t(2*idx)*blob_stride;
    for(unsigned eye=0;eye<2;++eye) {
        om(self,1,eye?&s3.rtv2:&s3.rtv,FALSE,t.has_depth?(eye?&s3.dsv2:&s3.dsv):nullptr);
        srv(self,s3.t10,va+uint64_t(eye)*blob_stride);
        if(d.indexed)reinterpret_cast<Step3DrawIndexedFn>(draw)(self,d.count,d.instances,d.start,d.base,d.start_instance);
        else reinterpret_cast<Step3DrawFn>(draw)(self,d.count,d.instances,d.start,d.start_instance);
        n+=3;
    }
    srv(self,s3.t10,s3.t10Original);
    om(self,s3.omCount,s3.omCount?s3.omRtv.data():nullptr,s3.omSingle,s3.omDsv?&s3.omDsvHandle:nullptr);n+=2;
    s3.issued.fetch_add(n);
    if(last)step3RecordSetCopies(self,st);
}
// The first member: the consumed attempt (step1Insert already armed the ledger
// and attached the witness). Later members run from step3SetMember.
void step3RecordSetFirst(ID3D12GraphicsCommandList* self,Step1State& st) noexcept {
    bool last=false;
    {std::lock_guard lock(st.mutex);auto& s3=st.s3;
     s3.members[0].status="recorded";s3.setRecorded=1;s3.setNext=1;last=s3.memberCount==1;s3.copiesRecorded=last;}
    step3RecordSetMember(self,st,0,true,last);
    if(!last)g_step3SetActive.store(1);
}
// Draw path for later members (phase inserted, set active). Matching rule:
// the same list (route) and recording as the consumed attempt; members are
// visited in ordinal order; an unsupported member is skipped (logged); any
// inconsistency (an ordinal passed without its member, unreadable live bytes)
// stops further members. The copies then happen at the list's Close.
__declspec(noinline) void step3SetMember(ID3D12GraphicsCommandList* self,uint64_t countInstances,uint64_t startBase,uint64_t firstIndexed) noexcept {
    auto* st=g_step1.load();if(!st)return;
    try {
        void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
        unsigned idx=0;bool record=false,last=false,copies=false;
        {std::lock_guard lock(st->mutex);auto& s3=st->s3;
         if(!g_step3SetActive.load()||s3.copiesRecorded||s3.setStop||step1Route(key)!=st->candidate)return;
         if(s3.setNext>=s3.memberCount)return;
         s3.aCount=UINT(countInstances);s3.aInstances=UINT(countInstances>>32);s3.aStart=UINT(startBase);
         s3.aBase=INT(uint32_t(startBase>>32));s3.aStartInstance=UINT(firstIndexed);s3.aIndexed=(firstIndexed>>32)!=0;
         step3Capture(*st,key,s3.live,true);
         auto& m=s3.members[s3.setNext];
         if(s3.live.draw_ordinal<m.shape.draw_ordinal)return;
         if(s3.live.draw_ordinal>m.shape.draw_ordinal){s3.setStop="member-ordinal-passed";return;}
         gpu_copy::step3::Targets now;const char* why=gpu_copy::step3::same_shape(m.shape,s3.live);
         if(!why)why=gpu_copy::step3::decide(s3.live,now);
         if(!why&&(now.width!=s3.targets.width||now.height!=s3.targets.height||now.color.name!=s3.targets.color.name||
                   now.has_depth!=s3.targets.has_depth||now.depth_clear!=s3.targets.depth_clear))why="targets-shape-changed";
         if(why) {
             m.status=why;++s3.setSkipped;++s3.setNext;
             if(s3.setNext==s3.memberCount){copies=true;s3.copiesRecorded=true;g_step3SetActive.store(0);}
         } else if(!step3InlineSrt(*st)){m.status=s3.freshness;s3.setStop="live-srt-unreadable";return;}
         else {
             record=true;idx=s3.setNext;m.status="recorded";++s3.setRecorded;++s3.setNext;last=s3.setNext==s3.memberCount;
             if(last){s3.copiesRecorded=true;g_step3SetActive.store(0);}
         }}
        if(record)step3RecordSetMember(self,*st,idx,false,last);
        else if(copies)step3RecordSetCopies(self,*st);
    } catch(...) {std::lock_guard lock(st->mutex);if(!st->s3.setStop)st->s3.setStop="member-exception";}
}
// l3Close, before the native Close of the candidate list: record the copies
// if the last member was not reached (PARTIAL), so they are still in order.
void step3SetClose(ID3D12GraphicsCommandList* self) noexcept {
    auto* st=g_step1.load();if(!st)return;
    try {
        void* key=nullptr;{auto& s=state();std::lock_guard lock(s.mutex);key=listKey_locked(self);}
        bool copies=false;
        {std::lock_guard lock(st->mutex);auto& s3=st->s3;
         if(!g_step3SetActive.load()||step1Route(key)!=st->candidate)return;
         g_step3SetActive.store(0);
         if(!s3.copiesRecorded){s3.copiesRecorded=true;s3.copiesAtClose=true;copies=true;
             if(s3.setNext<s3.memberCount&&!s3.setStop)s3.setStop="list-closed-before-members";}}
        if(copies)step3RecordSetCopies(self,*st);
    } catch(...) {}
}
// Game thread, no lock held, immediately before the original Draw. The plan
// and restore facts are immutable once the attempt was consumed. Native
// dispatch only (never the proxy's own hooks, so tracking is not disturbed).
__declspec(noinline) void step3Record(ID3D12GraphicsCommandList* self,Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    using OmFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*);
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    using SrvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS);
    using DrawIndexedFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT);
    using DrawFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT);
    using BarrierFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
    using CopyFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,
        const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*);
    if(st.s3.wholeSet){step3RecordSetFirst(self,st);return;}
    const auto& s3=st.s3;const auto& t=s3.targets;const auto& d=s3.live;
    const auto om=reinterpret_cast<OmFn>(step1Native(self,46));
    const auto clearRtv=reinterpret_cast<ClearRtvFn>(step1Native(self,48));
    const auto clearDsv=reinterpret_cast<ClearDsvFn>(step1Native(self,47));
    const auto srv=reinterpret_cast<SrvFn>(step1Native(self,40));
    const auto barrier=reinterpret_cast<BarrierFn>(step1Native(self,26));
    const auto copy=reinterpret_cast<CopyFn>(step1Native(self,16));
    void* const draw=step1Native(self,d.indexed?13:12);
    // J-3b7e: the one CPU write to the private UPLOAD slot (persistently mapped
    // in the owner context), before any command that reads it is recorded.
    for(unsigned b=0;b<blob_count;++b)std::memcpy(s3.uploadPtr+b*blob_stride,s3.inlineBlobs[b].data(),size_t(blob_bytes));
    std::atomic_thread_fence(std::memory_order_release);
    unsigned n=0;
    om(self,1,&s3.rtv,FALSE,t.has_depth?&s3.dsv:nullptr);++n;
    static constexpr FLOAT clear[4]={0,0,0,0};
    const D3D12_CLEAR_FLAGS flags=D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH|(t.depth.stencil?D3D12_CLEAR_FLAG_STENCIL:0));
    D3D12_RESOURCE_BARRIER toCopy{};toCopy.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource=s3.rt;toCopy.Transition.Subresource=0;
    toCopy.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;toCopy.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER toTarget=toCopy;
    toTarget.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;toTarget.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=s3.rt;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=0;
    dst.pResource=s3.readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT(d.pso.rtv[0]);dst.PlacedFootprint.Footprint.Width=t.crop_w;
    dst.PlacedFootprint.Footprint.Height=t.crop_h;dst.PlacedFootprint.Footprint.Depth=1;
    dst.PlacedFootprint.Footprint.RowPitch=UINT(t.row_pitch);
    // J-3b7b: only the crop box of the RT is copied (never the whole frame).
    const D3D12_BOX box{t.crop_left,t.crop_top,0,t.crop_left+t.crop_w,t.crop_top+t.crop_h,1};
    for(unsigned i=0;i<replay_count;++i) {
        clearRtv(self,s3.rtv,clear,0,nullptr);++n;
        if(t.has_depth){clearDsv(self,s3.dsv,flags,t.depth_clear,0,0,nullptr);++n;}
        if(i)srv(self,s3.t10,s3.uploadVa+(i-1)*blob_stride),++n; // original keeps the game's binding
        if(d.indexed)reinterpret_cast<DrawIndexedFn>(draw)(self,d.count,d.instances,d.start,d.base,d.start_instance);
        else reinterpret_cast<DrawFn>(draw)(self,d.count,d.instances,d.start,d.start_instance);
        ++n;
        barrier(self,1,&toCopy);++n;
        dst.PlacedFootprint.Offset=uint64_t(i)*t.footprint_stride;
        copy(self,&dst,0,0,0,&src,&box);++n;
        barrier(self,1,&toTarget);++n;
    }
    // Restore: every state the replays changed, from the captured game values.
    srv(self,s3.t10,s3.t10Original);++n;
    om(self,s3.omCount,s3.omCount?s3.omRtv.data():nullptr,s3.omSingle,s3.omDsv?&s3.omDsvHandle:nullptr);++n;
    st.s3.issued.store(n);
}
// ------------------------------------------------------------- owner context
// J-3b9c owner context: the OM descriptor ring (2048 RTV + 256 DSV, CPU-only heaps).
void step3CreateOmRing(Step1State& st) noexcept {
    if(!st.device)return;
    using HeapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**);
    using StartFn=D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,D3D12_CPU_DESCRIPTOR_HANDLE*);
    using IncFn=UINT(STDMETHODCALLTYPE*)(ID3D12Device*,D3D12_DESCRIPTOR_HEAP_TYPE);
    constexpr unsigned binds=HookState::Step3OmRing::binds;
    ID3D12DescriptorHeap* heaps[2]{};D3D12_CPU_DESCRIPTOR_HANDLE starts[2]{};bool ok=true;
    for(unsigned k=0;k<2&&ok;++k) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};d.Type=k?D3D12_DESCRIPTOR_HEAP_TYPE_DSV:D3D12_DESCRIPTOR_HEAP_TYPE_RTV;d.NumDescriptors=k?binds:binds*8;
        d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;void* v=nullptr;
        ok=SUCCEEDED(reinterpret_cast<HeapFn>(step1Native(st.device,14))(st.device,&d,__uuidof(ID3D12DescriptorHeap),&v))&&v;
        heaps[k]=static_cast<ID3D12DescriptorHeap*>(v);
        if(ok)reinterpret_cast<StartFn>(step1Native(heaps[k],9))(heaps[k],&starts[k]);
        ok=ok&&starts[k].ptr;
    }
    const auto inc=reinterpret_cast<IncFn>(step1Native(st.device,15));void* const copyNative=step1Native(st.device,24); // before the hook-state lock (step1Native takes it)
    const UINT rtvInc=ok?inc(st.device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV):0,dsvInc=ok?inc(st.device,D3D12_DESCRIPTOR_HEAP_TYPE_DSV):0;
    {auto& s=state();std::lock_guard lock(s.mutex);auto& r=s.step3Om;
     r.rtvHeap=heaps[0];r.dsvHeap=heaps[1]; // retained for the process lifetime
     if(ok){r.device=st.device;r.copy=copyNative;r.rtvBase=starts[0].ptr;r.dsvBase=starts[1].ptr;r.rtvInc=rtvInc;r.dsvInc=dsvInc;
         r.ready=r.copy&&r.rtvInc&&r.dsvInc;}}
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=om-ring;status=%s;binds=%u;rtv=%u;dsv=%u;restore=private-bind-time-copies",
        state().step3Om.ready?"ready":"failed(restore-refused)",binds,binds*8,binds);
    step1Emit(st.text);
}
void step3Prepare(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    step3CreateOmRing(st);
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=prepare;mode=step3;targets=deferred-until-observed;replays=%u;"
        "order=original,control,left,right;ipd-m=%.3f;units-per-m=%.1f(E9-assumption);half-offset-units=%.4f;rt-cap=%llu;ds-cap=%llu;"
        "readback-cap=%llu(full-target,crop-diagnostic);command-cap=%u;game-barriers=none;game-writes=none",replay_count,ipd_metres,units_per_metre,half_ipd_units,
        static_cast<unsigned long long>(rt_byte_cap),static_cast<unsigned long long>(ds_byte_cap),
        static_cast<unsigned long long>(readback_byte_cap),command_cap);
    step1Emit(st.text);
}
void step3EmitObserve(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    const auto& s=st.s3.shape;const auto& t=st.s3.targets;ColorFormat c;color_format(s.pso.rtv[0],c);
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=observe;status=%s;reason=%s;pso=%llx;rt=%u:%s;rt-count=%u;dsv=%u;"
        "samples=%u,%u;depth=%u,%s,w%u;stencil=%u;blend0=%u;mask0=%x;so=%u;om=k%u,g%u,s%u,n%u,single%u,dsv%u;vp=%u:%g,%g,%g,%g,%g,%g;"
        "extent=%gx%g;sc=%u:%d,%d,%d,%d;t10=p%u,table%u,va%llx,root%u;draw=%u,%u,%u,%u,%d,%u;ordinal=%llu;target-size=%ux%u;track=%u",
        st.s3.refusal?"refused":"accepted",st.s3.refusal?st.s3.refusal:"ok",static_cast<unsigned long long>(s.pso_route),
        s.pso.rtv[0],c.name,s.pso.rt_count,s.pso.dsv,s.pso.samples,s.pso.quality,unsigned(s.pso.depth_enable),
        func_name(s.pso.depth_func),unsigned(s.pso.depth_write),unsigned(s.pso.stencil),unsigned(s.pso.blend0),s.pso.write_mask0,
        s.pso.so_entries,unsigned(s.om_known),unsigned(s.om_gap),unsigned(s.om_set),s.om_count,unsigned(s.om_single),unsigned(s.om_dsv),
        s.viewports,s.vp0.x,s.vp0.y,s.vp0.w,s.vp0.h,s.vp0.min,s.vp0.max,s.vp_right,s.vp_bottom,s.scissors,s.sc0.l,s.sc0.t,s.sc0.r,s.sc0.b,
        s.t10_parameter,unsigned(s.t10_table),static_cast<unsigned long long>(s.t10_va),unsigned(s.t10_root_value),
        unsigned(s.indexed),s.count,s.instances,s.start,s.base,s.start_instance,static_cast<unsigned long long>(s.draw_ordinal),
        t.width,t.height,unsigned(s.tracking_complete&&!s.tracking_gap));
    step1Emit(st.text);
}
// EYE-RING: the same texture at an explicit size (the step-5 per-eye targets at the eye size); step3CreateTexture = the targets' size.
bool step3CreateTextureWH(Step1State& st,uint32_t width,uint32_t height,uint32_t format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial,
                          const D3D12_CLEAR_VALUE& clear,ID3D12Resource*& out,HRESULT& hr) noexcept {
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;
    desc.Height=height;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT(format);
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;desc.Flags=flags;
    void* value=nullptr;
    hr=reinterpret_cast<Fn>(step1Native(st.device,27))(st.device,&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,&clear,
        __uuidof(ID3D12Resource),&value);
    out=static_cast<ID3D12Resource*>(value);
    return SUCCEEDED(hr)&&out;
}
bool step3CreateTexture(Step1State& st,uint32_t format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES initial,
                        const D3D12_CLEAR_VALUE& clear,ID3D12Resource*& out,HRESULT& hr) noexcept {
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=st.s3.targets.width;
    desc.Height=st.s3.targets.height;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT(format);
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;desc.Flags=flags;
    void* value=nullptr;
    hr=reinterpret_cast<Fn>(step1Native(st.device,27))(st.device,&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,&clear,
        __uuidof(ID3D12Resource),&value);
    out=static_cast<ID3D12Resource*>(value);
    return SUCCEEDED(hr)&&out;
}
bool step3CreateHeap(Step1State& st,D3D12_DESCRIPTOR_HEAP_TYPE type,ID3D12DescriptorHeap*& out,HRESULT& hr,
                     D3D12_CPU_DESCRIPTOR_HANDLE& start) noexcept {
    using Fn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_DESCRIPTOR_HEAP_DESC*,REFIID,void**);
    using StartFn=D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,D3D12_CPU_DESCRIPTOR_HANDLE*);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=type;desc.NumDescriptors=1;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    void* value=nullptr;
    hr=reinterpret_cast<Fn>(step1Native(st.device,14))(st.device,&desc,__uuidof(ID3D12DescriptorHeap),&value);
    out=static_cast<ID3D12DescriptorHeap*>(value);
    if(FAILED(hr)||!out)return false;
    // MSVC member ABI: the struct result is returned through a hidden pointer.
    reinterpret_cast<StartFn>(step1Native(out,9))(out,&start);
    return start.ptr!=0;
}
// Owner context: map the private UPLOAD buffer once and keep the pointer
// (UPLOAD heaps may stay mapped). The only write is the inline one at the
// inserting draw; Unmap (written range 0..1536) happens at release.
const char* step3MapUpload(Step1State& st) noexcept {
    auto& s3=st.s3;
    using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
    const D3D12_RANGE none{0,0};void* p=nullptr;
    s3.writeHr=reinterpret_cast<MapFn>(step1Native(s3.upload,8))(s3.upload,0,&none,&p);
    if(FAILED(s3.writeHr)||!p)return "upload-map-failed";
    s3.uploadPtr=static_cast<uint8_t*>(p);
    return nullptr;
}
// Owner context, no lock held. Returns the static refusal or nullptr.
const char* step3CreateTargets(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;const auto& t=s3.targets;
    D3D12_CLEAR_VALUE colour{};colour.Format=DXGI_FORMAT(s3.shape.pso.rtv[0]);
    if(!step3CreateTexture(st,s3.shape.pso.rtv[0],D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,
        colour,s3.rt,s3.rtHr))return "rt-create-failed";
    if(t.has_depth) {
        D3D12_CLEAR_VALUE depth{};depth.Format=DXGI_FORMAT(s3.shape.pso.dsv);depth.DepthStencil.Depth=t.depth_clear;
        if(!step3CreateTexture(st,s3.shape.pso.dsv,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE,
            depth,s3.ds,s3.dsHr))return "ds-create-failed";
    }
    if(s3.wholeSet) {
        if(!step3CreateTexture(st,s3.shape.pso.rtv[0],D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,
            colour,s3.rt2,s3.rt2Hr))return "rt2-create-failed";
        if(t.has_depth) {
            D3D12_CLEAR_VALUE depth{};depth.Format=DXGI_FORMAT(s3.shape.pso.dsv);depth.DepthStencil.Depth=t.depth_clear;
            if(!step3CreateTexture(st,s3.shape.pso.dsv,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE,
                depth,s3.ds2,s3.ds2Hr))return "ds2-create-failed";
        }
    }
    if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,s3.upload,s3.uploadHr,
        s3.wholeSet?set_upload_bytes:upload_bytes))
        return "upload-create-failed";
    if(!step1CreateBuffer(st,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,s3.readback,s3.readbackHr,t.readback_bytes))
        return "readback-create-failed";
    if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,s3.rtvHeap,s3.rtvHeapHr,s3.rtv))return "rtv-heap-failed";
    if(t.has_depth&&!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_DSV,s3.dsvHeap,s3.dsvHeapHr,s3.dsv))return "dsv-heap-failed";
    if(s3.wholeSet) {
        HRESULT hr2=E_PENDING;
        if(!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,s3.rtvHeap2,hr2,s3.rtv2))return "rtv2-heap-failed";
        if(t.has_depth&&!step3CreateHeap(st,D3D12_DESCRIPTOR_HEAP_TYPE_DSV,s3.dsvHeap2,hr2,s3.dsv2))return "dsv2-heap-failed";
    }
    using RtvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_RENDER_TARGET_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    using DsvFn=void(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Resource*,const D3D12_DEPTH_STENCIL_VIEW_DESC*,D3D12_CPU_DESCRIPTOR_HANDLE);
    reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,s3.rt,nullptr,s3.rtv);
    if(t.has_depth)reinterpret_cast<DsvFn>(step1Native(st.device,21))(st.device,s3.ds,nullptr,s3.dsv);
    if(s3.wholeSet) {
        reinterpret_cast<RtvFn>(step1Native(st.device,20))(st.device,s3.rt2,nullptr,s3.rtv2);
        if(t.has_depth)reinterpret_cast<DsvFn>(step1Native(st.device,21))(st.device,s3.ds2,nullptr,s3.dsv2);
    }
    using VaFn=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
    s3.uploadVa=reinterpret_cast<VaFn>(step1Native(s3.upload,11))(s3.upload);
    if(!s3.uploadVa)return "upload-va-unknown";
    if(const char* why=step3MapUpload(st))return why;
    s3.pixels.reset(new(std::nothrow) uint8_t[size_t(t.readback_bytes)]);
    if(!s3.pixels)return "pixel-buffer-unavailable";
    s3.pixelBytes=t.readback_bytes;
    return nullptr;
}
void step3ReleasePrivate(Step1State& st) noexcept {
    auto& s3=st.s3;
    if(s3.upload&&s3.uploadPtr) {
        using UnmapFn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
        const D3D12_RANGE written{0,static_cast<SIZE_T>(s3.wholeSet?uint64_t(2*s3.memberCount)*gpu_copy::step3::blob_stride:
            uint64_t(gpu_copy::step3::blob_count)*gpu_copy::step3::blob_stride)};
        reinterpret_cast<UnmapFn>(step1Native(s3.upload,9))(s3.upload,0,&written);
        s3.uploadPtr=nullptr;
    }
    for(IUnknown** p:{reinterpret_cast<IUnknown**>(&s3.rt),reinterpret_cast<IUnknown**>(&s3.ds),reinterpret_cast<IUnknown**>(&s3.upload),
            reinterpret_cast<IUnknown**>(&s3.readback),reinterpret_cast<IUnknown**>(&s3.rtvHeap),reinterpret_cast<IUnknown**>(&s3.dsvHeap),
            reinterpret_cast<IUnknown**>(&s3.rt2),reinterpret_cast<IUnknown**>(&s3.ds2),reinterpret_cast<IUnknown**>(&s3.rtvHeap2),
            reinterpret_cast<IUnknown**>(&s3.dsvHeap2)})
        if(*p){releaseOwned(*p);*p=nullptr;}
    s3.pixels.reset();
}
void step3Hex16(const float* m,char (&out)[16*9+1]) noexcept {
    int n=0;
    for(int i=0;i<16;++i){uint32_t v;std::memcpy(&v,m+i,4);n+=std::snprintf(out+n,sizeof out-size_t(n),i?",%08x":"%08x",v);}
}
void step3EmitSrt(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    const auto& s3=st.s3;const auto& sub=s3.sub;const auto& c=sub.camera;
    const long long age=s3.snapMs&&s3.tObserved?static_cast<long long>(s3.tObserved)-static_cast<long long>(s3.snapMs):0;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-source;source=selection-sample;id=%llx:%llx:%llx;age-ms=%lld;va=%llx;"
        "resource=%llx;camera-eye=%.5g,%.5g,%.5g;right=%.5f,%.5f,%.5f;forward=%.5f,%.5f,%.5f;tan-half-fov=%.5f,%.5f;"
        "ortho-residual=%.3g;handedness=%g;half-offset-units=%.4f",
        static_cast<unsigned long long>(s3.snapList),static_cast<unsigned long long>(s3.snapGen),static_cast<unsigned long long>(s3.snapDraw),
        age,static_cast<unsigned long long>(s3.snapVa),static_cast<unsigned long long>(s3.snapResource),c.eye[0],c.eye[1],c.eye[2],
        c.right[0],c.right[1],c.right[2],c.forward[0],c.forward[1],c.forward[2],c.tanHalfFovX,c.tanHalfFovY,
        c.axesOrthogonalityResidual,c.handedness,half_ipd_units);
    step1Emit(st.text);
    char a[16*9+1],b[16*9+1];
    {acz::Mat4f m0=acz::toFloat32(sub.m0),m64=acz::toFloat32(sub.m64),m256=acz::toFloat32(sub.m256);
     step3Hex16(m0.data(),a);step3Hex16(m64.data(),b);
     std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-matrix;replay=original;f32-hex;m0=%s;m64=%s",a,b);step1Emit(st.text);
     step3Hex16(m256.data(),a);
     std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-matrix;replay=all;f32-hex;m256=%s;unchanged=bytes-128-415",a);step1Emit(st.text);}
    for(unsigned i=0;i<blob_count;++i) {
        step3Hex16(sub.m0f[i].data(),a);step3Hex16(sub.m64f[i].data(),b);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-matrix;replay=%s;offset=%+.4f;f32-hex;m0=%s;m64=%s",
            replay_names[i+1],blob_sign[i]*half_ipd_units,a,b);
        step1Emit(st.text);
    }
    const double w=s3.shape.vp0.w;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-checks;control-rel-err=m0:%.3g,m64:%.3g;control-bytes-differ=%u/128;"
        "untouched-bytes-equal=%u;eye-clip-residual=%.3g,%.3g,%.3g;expected-disparity-px=%.2f@%g,%.2f@%g,%.2f@%g;viewport-w=%g;"
        "read-mask=%s;not-substituted=+128,+192(camera-dependence-unknown);estimate-only",
        sub.control_m0_rel,sub.control_m64_rel,sub.control_bytes_differ,unsigned(sub.untouched_equal),
        sub.eye_clip_residual[0],sub.eye_clip_residual[1],sub.eye_clip_residual[2],
        expected_disparity_px(sub,depth_near_units,w),depth_near_units,expected_disparity_px(sub,depth_median_units,w),depth_median_units,
        expected_disparity_px(sub,depth_far_units,w),depth_far_units,w,srt_read_mask);
    step1Emit(st.text);
}
void step3EmitTargets(Step1State& st) noexcept {
    const auto& s3=st.s3;const auto& t=s3.targets;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=targets;rt=%llx,%s,%ux%u,RENDER_TARGET,hr=%08lx;ds=%llx,%s,DEPTH_WRITE,"
        "clear=%g,hr=%08lx;upload=%llx,va=%llx,blobs=3x416@512,write=%08lx,writes-ceased=1;readback=%llx,%llu,pitch=%llu,stride=%llu,"
        "hr=%08lx;rtv=%llx;dsv=%llx;heaps=cpu-only;charged=%llu;commands=%u",static_cast<unsigned long long>(step1Route(s3.rt)),
        t.color.name,t.width,t.height,static_cast<unsigned long>(s3.rtHr),static_cast<unsigned long long>(step1Route(s3.ds)),
        t.has_depth?t.depth.name:"none",t.depth_clear,static_cast<unsigned long>(s3.dsHr),
        static_cast<unsigned long long>(step1Route(s3.upload)),static_cast<unsigned long long>(s3.uploadVa),
        static_cast<unsigned long>(s3.writeHr),static_cast<unsigned long long>(step1Route(s3.readback)),
        static_cast<unsigned long long>(t.readback_bytes),static_cast<unsigned long long>(t.row_pitch),
        static_cast<unsigned long long>(t.footprint_stride),static_cast<unsigned long>(s3.readbackHr),
        static_cast<unsigned long long>(s3.rtv.ptr),static_cast<unsigned long long>(s3.dsv.ptr),
        static_cast<unsigned long long>(t.charged),t.commands);
    step1Emit(st.text);
}
// Worker, ready phase. false when the experiment finished (verdict emitted).
void step3EmitGameRt(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    const auto& s=st.s3.shape;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=game-rt;rtv=%llx;status=%s;resource=%llx;size=%llux%u;format=%u;heap=%s;"
        "viewport-extent=%gx%g;dsv-format=%u;stencil=%u,read:%x,write:%x,front:%s,back:%s,pass:%u;stencil-ref=untracked-unchanged;"
        "replay-stencil-clear=%u",static_cast<unsigned long long>(s.game_rtv),s.game_rt_status,
        static_cast<unsigned long long>(s.game_rt),static_cast<unsigned long long>(s.game_rt_width),s.game_rt_height,
        s.game_rt_format,gpu_copy::step2::heap_name(s.game_rt_heap),s.vp_right,s.vp_bottom,s.pso.dsv,unsigned(s.pso.stencil),
        s.pso.stencil_read,s.pso.stencil_write,func_name(s.pso.front_func),func_name(s.pso.back_func),s.pso.front_pass,
        unsigned(st.s3.targets.depth.stencil));
    step1Emit(st.text);
}
// J-3b7c diagnostics: the projection is logged, never a refusal.
void step3EmitProjection(Step1State& st,bool emitClip=false) noexcept {
    using namespace gpu_copy::step3;
    const auto& p=st.s3.crop;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=crop;diagnostic-only;status=%s;box=%u,%u,%u,%u;clamped=%u;pad=%u;"
        "vertices=%u;stride=%u;indices=%u;triangles=%u;gs-rejected=o:%u,c:%u,l:%u,r:%u;w<=0=o:%u,c:%u,l:%u,r:%u;front=w>0(gs-outcode);"
        "p400=%g,%g,%g,%g;ext-x=%.5g:%.5g,%.5g:%.5g,%.5g:%.5g,%.5g:%.5g",p.status,p.left,p.top,p.right,p.bottom,unsigned(p.clamped),crop_pad,
        p.vertices,st.s3.vbStride,st.s3.shape.indexed?st.s3.shape.count:0u,p.triangles,p.culled[0],p.culled[1],p.culled[2],p.culled[3],
        p.w_nonpositive[0],p.w_nonpositive[1],p.w_nonpositive[2],p.w_nonpositive[3],
        double(p.params[0]),double(p.params[1]),double(p.params[2]),double(p.params[3]),
        p.extent[0][0],p.extent[0][2],p.extent[1][0],p.extent[1][2],p.extent[2][0],p.extent[2][2],p.extent[3][0],p.extent[3][2]);
    step1Emit(st.text);
    int n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=vertices;source=selection-sample-vb;attribute0-xyz=");
    for(uint32_t i=0;i<p.vertices&&i<8&&n>0&&size_t(n)<sizeof st.text;++i)
        n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),i?";%.5g,%.5g,%.5g":"%.5g,%.5g,%.5g",
            double(p.xyz[i][0]),double(p.xyz[i][1]),double(p.xyz[i][2]));
    step1Emit(st.text);
    if(!emitClip)return;
    for(unsigned r=0;r<replay_count&&p.vertices;++r) {
        n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=clip;replay=%s;xyzw=",replay_names[r]);
        for(uint32_t i=0;i<p.vertices&&i<8&&n>0&&size_t(n)<sizeof st.text;++i)
            n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),i?";%.4g,%.4g,%.4g,%.4g":"%.4g,%.4g,%.4g,%.4g",
                p.clip[r][i][0],p.clip[r][i][1],p.clip[r][i][2],p.clip[r][i][3]);
        step1Emit(st.text);
    }
}
// ------------------------------------------------------------- selection (worker)
// Hook-state mutex taken here; two SEH-guarded passes of each span must agree.
// Index span: `count` indices from `start`; vertex span: [min,max]+base of the
// referenced vertices (non-indexed: start..start+count-1). Caps: 12288
// indices, 4096 vertices, stride 12..256. Returns nullptr or the reason.
const char* step3Sample(Step1State& st,const gpu_copy::step3::Shape& d,std::array<uint8_t,416>& srt,std::vector<uint8_t>& vb,
                        uint32_t& vbFirst,uint32_t& vbCount,std::vector<uint8_t>& ib) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;
    try {
        if(!d.count||!d.instances)return "empty-draw";
        if(d.indexed&&(d.ib_stride!=2&&d.ib_stride!=4))return "index-format";
        if(d.indexed&&d.count>max_indices)return "index-count-over-cap";
        if(d.vb_stride<12||d.vb_stride>max_vertex_stride)return "vertex-stride";
        auto& s=state();std::lock_guard lock(s.mutex);
        auto read=[&](uint64_t address,uint64_t va,uint64_t bytes,uint8_t* out)->const char* {
            const uint8_t* p=nullptr;
            if(const char* why=step3MappedSpan_locked(address,va,bytes,p))return why;
            if(s3.scratch.size()<bytes)s3.scratch.resize(size_t(bytes));
            if(!guardedCpuRead(p,out,size_t(bytes))||!guardedCpuRead(p,s3.scratch.data(),size_t(bytes)))return "read-fault";
            return std::memcmp(out,s3.scratch.data(),size_t(bytes))?"unstable":nullptr;
        };
        if(const char* why=read(d.t10_address,d.t10_va,416,srt.data()))return why;
        uint64_t lo=0,hi=0;
        if(d.indexed) {
            const uint64_t off=uint64_t(d.start)*d.ib_stride,bytes=uint64_t(d.count)*d.ib_stride;
            if(off>d.ib_bytes||bytes>d.ib_bytes-off)return "index-span-exceeds-view";
            ib.resize(size_t(bytes));
            if(const char* why=read(d.ib_address,d.ib_va+off,bytes,ib.data()))return why;
            lo=UINT64_MAX;
            for(uint32_t k=0;k<d.count;++k) {
                uint64_t x=0;
                if(d.ib_stride==2){uint16_t v;std::memcpy(&v,ib.data()+2*k,2);if(v==0xffff&&d.strip_cut)continue;x=v;}
                else {uint32_t v;std::memcpy(&v,ib.data()+4*k,4);if(v==0xffffffffu&&d.strip_cut)continue;x=v;}
                const int64_t a=int64_t(x)+d.base;if(a<0)return "negative-vertex";
                lo=uint64_t(a)<lo?uint64_t(a):lo;hi=uint64_t(a)>hi?uint64_t(a):hi;
            }
            if(lo==UINT64_MAX)return "no-indices";
        } else {lo=d.start;hi=uint64_t(d.start)+d.count-1;ib.clear();}
        if(hi-lo+1>max_vertices)return "vertex-count-over-cap";
        const uint64_t voff=lo*d.vb_stride,vbytes=(hi-lo+1)*d.vb_stride;
        if(voff>d.vb_bytes||vbytes>d.vb_bytes-voff)return "vertex-span-exceeds-view";
        vb.resize(size_t(vbytes));
        if(const char* why=read(d.vb_address,d.vb_va+voff,vbytes,vb.data()))return why;
        vbFirst=uint32_t(lo);vbCount=uint32_t(hi-lo+1);
        return nullptr;
    } catch(...) {return "sample-allocation-failed";}
}
std::string& s3t(Step1State& st) noexcept {return st.s3.tableText;}
void step3EmitCandidate(Step1State& st,const Step3Fields::Candidate& c,const gpu_copy::step3::CandidateEval& e,const char* why,
                        uint64_t ageMs) noexcept {
    const auto& d=c.shape;char text[diagnostics::text_capacity];
    std::snprintf(text,sizeof text,"gpu-copy-step3-table;seq=%llu;ordinal=%llu;list=%llx;gen=%llx;pso=%llx;draw=%u,%u,%u,%u,%d,%u;topology=%u;"
        "ib=%u;vb-stride=%u;vertices=%u;indices=%u;status=%s;w=%.4g:%.4g;tris=%u/%u;area=%.5f;ndc-bbox=%.3f,%.3f,%.3f,%.3f;t10-va=%llx;"
        "sample-age-ms=%llu",static_cast<unsigned long long>(c.seq),static_cast<unsigned long long>(d.draw_ordinal),
        static_cast<unsigned long long>(d.list),static_cast<unsigned long long>(d.generation),static_cast<unsigned long long>(d.pso_route),
        unsigned(d.indexed),d.count,d.instances,d.start,d.base,d.start_instance,d.topology,d.ib_stride,d.vb_stride,e.vertices,e.indices,
        why?why:e.status,e.wmin==1e300?0.0:e.wmin,e.wmax==-1e300?0.0:e.wmax,e.visible,e.triangles,e.area,e.bbox[0],e.bbox[1],e.bbox[2],e.bbox[3],
        static_cast<unsigned long long>(d.t10_va),static_cast<unsigned long long>(ageMs));
    // Ordinary log (not the reserved partition) AND the step-3 table file next
    // to the PNGs (J-3b7e: the ordinary log may not retain them).
    try {s3t(st).append(text);s3t(st).push_back('\n');} catch(...) {}
#ifdef ACZ_VR_OFFLINE_TEST
    if(g_step1Capture)g_step1Capture(text);
#endif
    diagnostics::emit(diagnostics::record(diagnostics::Event::requirements_metadata,0,0,0,text));
}
void step3EmitSelect(Step1State& st,const char* outcome) noexcept {
    const auto& s3=st.s3;const auto& b=s3.best.shape;const auto& e=s3.bestEval;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=select;outcome=%s;recorded=%llu;evaluated=%llu;dropped=%llu;unreadable=%llu;"
        "visible=%llu;table-lines=%u;policy=visible,max-ndc-area,lowest-ordinal;chosen=seq:%llu,ordinal:%llu,draw:%u,%u,%u,%d,tris:%u/%u,"
        "area:%.5f,w:%.4g:%.4g,vertices:%u;match=pso+args+ordinal-in-list;srt=inline-live-at-draw",
        outcome,static_cast<unsigned long long>(s3.recorded),static_cast<unsigned long long>(s3.evaluated),
        static_cast<unsigned long long>(s3.dropped),static_cast<unsigned long long>(s3.unreadable),
        static_cast<unsigned long long>(s3.visibleCount),s3.tableLines,static_cast<unsigned long long>(s3.best.seq),
        static_cast<unsigned long long>(b.draw_ordinal),b.count,b.instances,b.start,b.base,e.visible,e.triangles,e.area,
        e.wmin==1e300?0.0:e.wmin,e.wmax==-1e300?0.0:e.wmax,s3.bestVbCount);
    step1Emit(st.text);
}
std::wstring step3OutputDir() noexcept;
bool step3WriteFile(const std::wstring& path,const std::vector<uint8_t>& bytes) noexcept;
void step3WriteTable(Step1State& st) noexcept {
    auto& s3=st.s3;
    std::snprintf(s3.tableName,sizeof s3.tableName,"acz-vr-step3-%lu-%llu-candidates.txt",GetCurrentProcessId(),
        static_cast<unsigned long long>(st.tStart));
    const std::wstring dir=step3OutputDir();
    step1Sha256(s3.tableText.data(),s3.tableText.size(),s3.tableSha);
    if(dir.empty()){s3.tableOk=false;return;}
    try {
        std::wstring path=dir+L"\\";for(const char* c=s3.tableName;*c;++c)path.push_back(wchar_t(*c));
        const std::vector<uint8_t> bytes(s3.tableText.begin(),s3.tableText.end());
        s3.tableOk=step3WriteFile(path,bytes);
    } catch(...) {s3.tableOk=false;}
}
void step3EmitTableLine(Step1State& st) noexcept {
    const auto& s3=st.s3;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=table;file=%s;written=%u;lines=%u;bytes=%llu;sha256=%s",s3.tableName,
        unsigned(s3.tableOk),s3.tableLines,static_cast<unsigned long long>(s3.tableText.size()),s3.tableSha);
    step1Emit(st.text);
}
// Worker, step-1 mutex held. Builds s3.members from the visible candidates.
const char* step3BuildSet(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;const size_t n=s3.visibleCands.size();
    if(!n)return "no-visible-candidate";
    // Group with the largest visible area sum (ties: the earliest record).
    size_t bestI=0;double bestArea=-1;
    for(size_t i=0;i<n;++i) {
        const auto& a=s3.visibleCands[i].shape;double sum=0;bool seenEarlier=false;
        for(size_t j=0;j<n;++j) {
            const auto& b=s3.visibleCands[j].shape;
            if(b.list!=a.list||b.generation!=a.generation)continue;
            if(j<i){seenEarlier=true;break;}
            sum+=s3.visibleEvals[j].area;
        }
        if(!seenEarlier&&sum>bestArea){bestArea=sum;bestI=i;}
    }
    s3.setGroupList=s3.visibleCands[bestI].shape.list;s3.setGroupGen=s3.visibleCands[bestI].shape.generation;s3.setArea=bestArea;
    // Members in ordinal order.
    s3.memberCount=0;s3.setExcluded=0;
    Targets ref{};bool haveRef=false;
    for(;;) {
        size_t pick=n;
        for(size_t j=0;j<n;++j) {
            const auto& b=s3.visibleCands[j].shape;
            if(b.list!=s3.setGroupList||b.generation!=s3.setGroupGen)continue;
            if(s3.memberCount&&b.draw_ordinal<=s3.members[s3.memberCount-1].shape.draw_ordinal)continue;
            if(pick==n||b.draw_ordinal<s3.visibleCands[pick].shape.draw_ordinal)pick=j;
        }
        if(pick==n)break;
        if(s3.memberCount>=set_cap){++s3.setExcluded;break;}
        const auto& c=s3.visibleCands[pick];Targets t{};decide(c.shape,t);
        if(!haveRef){ref=t;haveRef=true;}
        else if(t.width!=ref.width||t.height!=ref.height||t.color.name!=ref.color.name||t.has_depth!=ref.has_depth||
                t.depth_clear!=ref.depth_clear||c.shape.pso.rtv[0]!=s3.members[0].shape.pso.rtv[0]||c.shape.pso.dsv!=s3.members[0].shape.pso.dsv) {
            ++s3.setExcluded;
            // Keep the ordinal ordering; an excluded draw is never replayed (logged in the table).
            try {char text[160];std::snprintf(text,sizeof text,"gpu-copy-step3-table;set-excluded;ordinal=%llu;reason=targets-differ\n",
                static_cast<unsigned long long>(c.shape.draw_ordinal));s3.tableText.append(text);} catch(...) {}
            // advance past this ordinal by recording a sentinel member temporarily
            s3.members[s3.memberCount].shape=c.shape;s3.members[s3.memberCount].status="excluded";
            ++s3.memberCount;continue;
        }
        auto& m=s3.members[s3.memberCount++];m.shape=c.shape;m.seq=c.seq;m.eval=s3.visibleEvals[pick];m.status="pending";
    }
    // Drop the excluded sentinels (they only kept the ordinal scan moving).
    unsigned w=0;for(unsigned i=0;i<s3.memberCount;++i)if(std::string_view(s3.members[i].status)!="excluded")s3.members[w++]=s3.members[i];
    s3.memberCount=w;
    if(!s3.memberCount)return "no-visible-candidate";
    if(set_commands(s3.memberCount,ref.has_depth)>set_command_cap)return "set-command-cap";
    return nullptr;
}
void step3EmitSet(Step1State& st) noexcept {
    const auto& s3=st.s3;
    int n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=set;members=%u;excluded=%u;group=list:%llx,gen:%llx;area-sum=%.4f;"
        "commands=%u(clears+8/member+6);cap=%u;ordinals=",s3.memberCount,s3.setExcluded,static_cast<unsigned long long>(s3.setGroupList),
        static_cast<unsigned long long>(s3.setGroupGen),s3.setArea,gpu_copy::step3::set_commands(s3.memberCount,true),gpu_copy::step3::set_command_cap);
    for(unsigned i=0;i<s3.memberCount&&n>0&&size_t(n)<sizeof st.text-24;++i)
        n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),i?",%llu":"%llu",static_cast<unsigned long long>(s3.members[i].shape.draw_ordinal));
    step1Emit(st.text);
}
const char* step5Arm(Step1State& st) noexcept; // J-3b8 (gpu_copy_step5_hooks.inl)
// Worker, ready phase, before observation. Drains the ring, evaluates every
// candidate, closes on the candidate cap or 45 s after the first record, then
// chooses. Returns 0 continue, 1 chosen (observation facts set), 2 finished.
int step3SelectTick(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;
    std::array<Step3Fields::Candidate,candidate_ring> local;unsigned n=0;bool closing=false;
    {std::lock_guard lock(st.mutex);
     if(s3.recorded&&++s3.selectTicks>=selection_ticks)s3.selectionClosed=true;
     if(s3.recorded>=step3CandidateCap())s3.selectionClosed=true;
     closing=s3.selectionClosed; // closed under this mutex: the seam records nothing more
     while(s3.ringTail<s3.ringHead&&n<local.size())local[n++]=s3.ring[size_t(s3.ringTail++%candidate_ring)];}
    const uint64_t now=GetTickCount64();
    for(unsigned i=0;i<n;++i) {
        const auto& c=local[i];CandidateEval e;std::array<uint8_t,416> srt{};uint32_t first=0,count=0;
        // Only candidates whose state the replay can soundly recover are eligible.
        {Targets t;if(const char* dr=decide(c.shape,t)) {
            e.status=dr;++s3.evaluated;
            if(!s3.firstRefusal){s3.firstRefusal=dr;s3.firstRefused=c.shape;}
            if(s3.tableLines<table_line_cap&&s3.evaluated<=32){++s3.tableLines;step3EmitCandidate(st,c,e,dr,now-c.tick);}
            continue;
        }}
        const char* why=step3Sample(st,c.shape,srt,s3.sampleVb,first,count,s3.sampleIb);
        if(!why)why=evaluate_draw(srt.data(),s3.sampleVb.data(),first,count,c.shape.vb_stride,
            c.shape.indexed?s3.sampleIb.data():nullptr,c.shape.ib_stride,c.shape,e);
        else {e.status=why;++s3.unreadable;}
        ++s3.evaluated;if(e.visible)++s3.visibleCount;
        if(s3.wholeSet&&e.visible&&!why)try {s3.visibleCands.push_back(c);s3.visibleEvals.push_back(e);} catch(...) {}
        if(s3.tableLines<table_line_cap&&(s3.evaluated<=32||e.visible)){++s3.tableLines;step3EmitCandidate(st,c,e,why,now-c.tick);}
        if(why&&std::string_view(e.status)!="gs-rejected"&&std::string_view(e.status)!="visible")continue;
        if(!s3.haveBest||better(e,c.shape.draw_ordinal,c.seq,s3.bestEval,s3.best.shape.draw_ordinal,s3.best.seq)) {
            try {s3.bestVb=s3.sampleVb;s3.bestIb=s3.sampleIb;} catch(...) {continue;}
            s3.haveBest=true;s3.best=c;s3.bestEval=e;s3.bestSrt=srt;s3.bestVbFirst=first;s3.bestVbCount=count;s3.srcTick=now;
        }
    }
    {std::lock_guard lock(st.mutex);if(s3.ringTail<s3.ringHead)return 0;} // drain first
    if(!closing)return 0;
    step3WriteTable(st); // file I/O outside the step-1 mutex
    std::lock_guard lock(st.mutex);
    step3EmitTableLine(st);
    if(!s3.haveBest&&s3.firstRefusal) {
        // No candidate passed the recoverability rules: report the first rule
        // through the ordinary observation refusal path (observe line + verdict).
        step3EmitSelect(st,"none-acceptable");
        s3.shape=s3.firstRefused;s3.refusal=s3.firstRefusal;s3.targets=Targets{};
        s3.observed=true;s3.tObserved=GetTickCount64();s3.waitReason="refused";
        return 1;
    }
    if(!s3.haveBest||!s3.bestEval.visible) {
        step3EmitSelect(st,"none-visible");
        s3.finished=true;st.released=true;s3.waitReason="no-visible-candidate";
        step1Finish(st,"REFUSED","no-visible-candidate");
        return 2;
    }
    if(s3.wholeSet) {
        // Whole set: the list recording (list, generation) with the largest total
        // visible area; its visible, recoverable draws in ordinal order (one per
        // ordinal), all rendering into the first member's target shape.
        const char* why=step3BuildSet(st);
        step3EmitSelect(st,why?"set-refused":"set-chosen");step3EmitSet(st);
        if(why) {
            s3.finished=true;st.released=true;s3.waitReason=why;
            step1Finish(st,"REFUSED",why);return 2;
        }
        s3.shape=s3.members[0].shape;s3.srcSrt=s3.bestSrt;s3.srcVa=s3.shape.t10_va;s3.vbStride=s3.shape.vb_stride;
        s3.snapList=s3.best.shape.list;s3.snapGen=s3.best.shape.generation;s3.snapDraw=s3.best.shape.draw_ordinal;s3.snapMs=s3.best.tick;
        s3.snapVa=s3.best.shape.t10_va;s3.snapResource=s3.best.shape.t10_address;
        s3.refusal=decide(s3.shape,s3.targets);
        s3.observed=true;s3.tObserved=GetTickCount64();s3.waitReason=s3.refusal?"refused":"targets-pending";
        return 1;
    }
    step3EmitSelect(st,"chosen");
    s3.shape=s3.best.shape;s3.srcSrt=s3.bestSrt;s3.srcVa=s3.best.shape.t10_va;s3.vbStride=s3.best.shape.vb_stride;
    s3.snapList=s3.best.shape.list;s3.snapGen=s3.best.shape.generation;s3.snapDraw=s3.best.shape.draw_ordinal;s3.snapMs=s3.best.tick;
    s3.snapVa=s3.best.shape.t10_va;s3.snapResource=s3.best.shape.t10_address;
    s3.refusal=decide(s3.shape,s3.targets);
    s3.observed=true;s3.tObserved=GetTickCount64();s3.waitReason=s3.refusal?"refused":"targets-pending";
    return 1;
}
bool step3ReadyTick(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    // J-3b7b: owner-context waits are bounded separately from the 600 s
    // qualification window, so every decided outcome is logged promptly.
    constexpr unsigned match_ticks=300; // 30 s at the 100 ms cadence
    {bool selecting;{std::lock_guard lock(st.mutex);
      if(g_step1Phase.load()!=static_cast<int>(Step1Phase::ready)||st.s3.finished)return true;
      selecting=!st.s3.observed;}
     if(selecting){const int r=step3SelectTick(st);if(r==2)return false;if(r==0)return true;}}
    const char* refusal=nullptr;bool prepare=false;
    {std::lock_guard lock(st.mutex);
     if(g_step1Phase.load()!=static_cast<int>(Step1Phase::ready)||st.s3.finished)return true;
     if(st.s3.observed&&!st.s3.emittedObserve){st.s3.emittedObserve=true;step3EmitObserve(st);step3EmitGameRt(st);}
     if(st.s3.refusal)refusal=st.s3.refusal;
     else if(st.s3.armed&&++st.s3.armedTicks>=match_ticks) {
         st.s3.finished=true; // the Draw seam checks this under the same mutex: nothing can be recorded now
         refusal=st.s3.freshUnreadable?"live-srt-unreadable-within-30s-after-arm":
             st.s3.orderShifted&&!st.s3.mismatches?"draw-order-shifted-within-30s-after-arm":
             st.s3.mismatch?"no-matching-draw-within-30s-after-arm":"no-qualified-draw-within-30s-after-arm";
     } else prepare=st.s3.observed&&!st.s3.armed;}
    auto finish=[&](const char* why) {
        step3ReleasePrivate(st); // never recorded: no GPU reference exists
        std::lock_guard lock(st.mutex);
        st.s3.finished=true;st.released=true;
        if(std::string_view(st.s3.waitReason)=="armed"||std::string_view(st.s3.waitReason)=="targets-pending")st.s3.waitReason=why;
        step1Finish(st,"REFUSED",why);
        return false;
    };
    if(refusal)return finish(refusal);
    if(!prepare)return true;
    if(!substitute(st.s3.srcSrt.data(),st.s3.sub))return finish(st.s3.sub.error?st.s3.sub.error:"substitution-failed");
    // J-3b7c: the projection/crop is diagnostic only; the readback is the full target.
    if(!st.s3.wholeSet) {
        (void)project(st.s3.shape,st.s3.targets,st.s3.bestVb.data(),st.s3.bestVbFirst,st.s3.bestVbCount,st.s3.vbStride,
                      st.s3.shape.indexed?st.s3.bestIb.data():nullptr,st.s3.shape.ib_stride,st.s3.sub,st.s3.crop);
        {std::lock_guard lock(st.mutex);step3EmitProjection(st);}
    } else st.s3.targets.commands=set_commands(st.s3.memberCount,st.s3.targets.has_depth);
    if(st.s3.continuous) {
        // J-3b8: no one-shot transaction; the ring (created once) is used per frame.
        if(const char* why=step5Arm(st))return finish(why);
        std::lock_guard lock(st.mutex);
        step3EmitSrt(st);
        st.s3.armed=false;st.s3.tArmed=GetTickCount64();st.s3.waitReason="continuous";
        g_step1Phase.store(static_cast<int>(Step1Phase::continuous)); // the one-shot gate never consumes (armed stays false)
        return true;
    }
    if(const char* why=st.s3.wholeSet?finalize(st.s3.targets,0,0,st.s3.targets.width,st.s3.targets.height,2,2,set_upload_bytes):
                                      finalize(st.s3.targets,0,0,st.s3.targets.width,st.s3.targets.height)) {
        {std::lock_guard lock(st.mutex);step3EmitSrt(st);}return finish(why);
    }
    if(const char* why=step3CreateTargets(st)) {
        {std::lock_guard lock(st.mutex);step3EmitTargets(st);}
        return finish(why);
    }
    std::lock_guard lock(st.mutex);
    step3EmitSrt(st);step3EmitTargets(st);
    st.s3.armed=true;st.s3.tArmed=GetTickCount64();st.s3.waitReason="armed";
    return true;
}
const char* step3ExpiryReason(const Step1State& st) noexcept {
    if(!st.s3.observed)return "no-qualified-matched-draw-within-600s";
    if(!st.s3.armed)return "targets-not-armed-within-600s";
    return "no-matching-draw-after-arm-within-600s";
}
// ------------------------------------------------------------- after completion
// Ledger read callback (unlocked): one Map of the whole readback, exact read
// range, empty written range. Only after same-timeline completion and closure.
bool step3Read(Step1State& st) noexcept {
    auto& s3=st.s3;
    if(!s3.readback||!s3.pixels||!st.timeline.complete(st.timelineTicket))return false;
    using MapFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
    using UnmapFn=void(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*);
    const D3D12_RANGE range{0,static_cast<SIZE_T>(s3.pixelBytes)},none{0,0};
    void* p=nullptr;
    s3.mapHr=reinterpret_cast<MapFn>(step1Native(s3.readback,8))(s3.readback,0,&range,&p);
    if(FAILED(s3.mapHr)||!p)return false;
    std::memcpy(s3.pixels.get(),p,size_t(s3.pixelBytes));
    reinterpret_cast<UnmapFn>(step1Native(s3.readback,9))(s3.readback,0,&none);
    s3.read=true;return true;
}
bool step3PixelSha(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,uint32_t bpp,char (&out)[65]) noexcept {
    std::snprintf(out,sizeof out,"unavailable");
    BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&hash,nullptr,0,nullptr,0,0)<0)return false;
    bool ok=true;
    for(uint32_t y=0;y<h&&ok;++y)ok=BCryptHashData(hash,const_cast<PUCHAR>(base+uint64_t(y)*pitch),ULONG(uint64_t(w)*bpp),0)>=0;
    uint8_t digest[32]{};ok=ok&&BCryptFinishHash(hash,digest,sizeof digest,0)>=0;BCryptDestroyHash(hash);
    if(ok)gpu_copy::step1::hex(digest,sizeof digest,out);
    return ok;
}
bool step3WriteFile(const std::wstring& path,const std::vector<uint8_t>& bytes) noexcept {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    size_t at=0;bool ok=true;
    while(ok&&at<bytes.size()) {
        const DWORD chunk=static_cast<DWORD>(bytes.size()-at<(1u<<20)?bytes.size()-at:(1u<<20));DWORD wrote=0;
        ok=WriteFile(file,bytes.data()+at,chunk,&wrote,nullptr)&&wrote==chunk;at+=wrote;
    }
    ok=CloseHandle(file)&&ok;
    return ok;
}
char g_step3PngDirUtf8[200]{};
std::wstring step3OutputDir() noexcept {
    try {
#ifdef ACZ_VR_OFFLINE_TEST
        if(g_step3PngDir)return g_step3PngDir;
#endif
        return diagnostics::DefaultPaths().paths().logs.wstring();
    } catch(...) {return {};}
}
// Worker, NO lock held: statistics, hashes, comparisons and PNG files.
void step3ProcessSet(Step1State& st) noexcept;
void step3Process(Step1State& st) noexcept {
    if(st.s3.wholeSet){step3ProcessSet(st);return;}
    using namespace gpu_copy::step3;
    auto& s3=st.s3;if(!s3.read||s3.processed)return;
    s3.processed=true;
    const auto& t=s3.targets;const uint8_t* base=s3.pixels.get();
    auto at=[&](unsigned i){return base+uint64_t(i)*t.footprint_stride;};
    const uint32_t bpp=t.color.bytes,w=t.crop_w,h=t.crop_h;
    for(unsigned i=0;i<replay_count;++i) {
        analyse(at(i),w,h,t.row_pitch,bpp,s3.stats[i]);
        step3PixelSha(at(i),w,h,t.row_pitch,bpp,s3.sha[i]); // raw bytes (FP16 halves for float targets)
        if(t.color.half)float_stats(at(i),w,h,t.row_pitch,s3.f16[i]);
    }
    s3.controlDiff=differing(at(replay_control),at(replay_original),w,h,t.row_pitch,bpp,s3.controlFirst);
    s3.eyeDiff=differing(at(replay_left),at(replay_right),w,h,t.row_pitch,bpp,s3.eyeFirst);
    const std::wstring dir=step3OutputDir();
    if(!dir.empty()) {
        const int n=WideCharToMultiByte(CP_UTF8,0,dir.c_str(),-1,g_step3PngDirUtf8,int(sizeof g_step3PngDirUtf8),nullptr,nullptr);
        if(n<=0)std::snprintf(g_step3PngDirUtf8,sizeof g_step3PngDirUtf8,"unprintable");
    } else std::snprintf(g_step3PngDirUtf8,sizeof g_step3PngDirUtf8,"unavailable");
    bool allPng=!dir.empty();
    for(unsigned i=0;i<replay_count;++i) {
        std::snprintf(s3.png[i],sizeof s3.png[i],"acz-vr-step3-%lu-%llu-%s.png",GetCurrentProcessId(),
            static_cast<unsigned long long>(st.tStart),replay_names[i]);
        std::vector<uint8_t> png,rgba;
        bool ok=!dir.empty()&&to_rgba8(at(i),w,h,t.row_pitch,t.color,rgba)&&png_encode(rgba.data(),w,h,uint64_t(w)*4,false,png);
        auto write=[&](const char* name,const std::vector<uint8_t>& bytes) {
            std::wstring path;
            try {path=dir+L"\\";for(const char* c=name;*c;++c)path.push_back(wchar_t(*c));} catch(...) {return false;}
            return step3WriteFile(path,bytes);
        };
        if(ok) {
            ok=write(s3.png[i],png);
            s3.pngBytes[i]=png.size();step1Sha256(png.data(),png.size(),s3.pngSha[i]);
        } else std::snprintf(s3.pngSha[i],sizeof s3.pngSha[i],"none");
        // J-3b7c: a box-filtered copy at most 640 texels wide for easy viewing.
        std::snprintf(s3.pngSmall[i],sizeof s3.pngSmall[i],"acz-vr-step3-%lu-%llu-%s-small.png",GetCurrentProcessId(),
            static_cast<unsigned long long>(st.tStart),replay_names[i]);
        std::vector<uint8_t> smallRgba,smallPng;uint32_t sw=0,sh=0;
        bool smallOk=ok&&downscale_rgba8(rgba,w,h,downscale_factor(w,640),smallRgba,sw,sh)&&png_encode(smallRgba.data(),sw,sh,uint64_t(sw)*4,false,smallPng);
        if(smallOk){smallOk=write(s3.pngSmall[i],smallPng);s3.pngSmallBytes[i]=smallPng.size();step1Sha256(smallPng.data(),smallPng.size(),s3.pngSmallSha[i]);
            s3.smallW=sw;s3.smallH=sh;}
        else std::snprintf(s3.pngSmallSha[i],sizeof s3.pngSmallSha[i],"none");
        ok=ok&&smallOk;
        s3.pngOk[i]=ok;allPng=allPng&&ok;
    }
    if(!s3.stats[replay_original].count)s3.contentFail="original-replay-empty";
    else if(!s3.stats[replay_left].count||!s3.stats[replay_right].count)s3.contentFail="eye-replay-empty";
    else if(!allPng)s3.contentFail="png-write-failed";
}
// J-3b7e: the evidence of WHICH bytes were substituted at the inserting draw:
// the live SrtData's VA/resource/list/ordinal and SHA-256, every blob's SHA-256,
// control == raw (byte compare), the substituted row-3 floats, and the clip
// coordinates of the first 8 sampled vertices under those live matrices.
void step3EmitLive(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;
    char live[65],b0[65],b1[65],b2[65];
    step1Sha256(s3.liveSrt.data(),416,live);step1Sha256(s3.inlineBlobs[0].data(),416,b0);
    step1Sha256(s3.inlineBlobs[1].data(),416,b1);step1Sha256(s3.inlineBlobs[2].data(),416,b2);
    unsigned differ=0,eyeDiffer[2]={0,0};
    for(size_t i=0;i<416;++i){differ+=s3.inlineBlobs[0][i]!=s3.liveSrt[i];eyeDiffer[0]+=s3.inlineBlobs[1][i]!=s3.liveSrt[i];eyeDiffer[1]+=s3.inlineBlobs[2][i]!=s3.liveSrt[i];}
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-live;source=inline-at-draw;va=%llx;resource=%llx;list=%llx;gen=%llx;ordinal=%llu;"
        "sha256=%s;control-bytes-differ=%u/416;eye-bytes-differ=%u,%u(rows3-only);blob-sha256=%.16s,%.16s,%.16s",
        static_cast<unsigned long long>(s3.live.t10_va),static_cast<unsigned long long>(s3.live.t10_address),
        static_cast<unsigned long long>(s3.live.list),static_cast<unsigned long long>(s3.live.generation),
        static_cast<unsigned long long>(s3.live.draw_ordinal),live,differ,eyeDiffer[0],eyeDiffer[1],b0,b1,b2);
    step1Emit(st.text);
    for(unsigned b=1;b<blob_count;++b) {
        float r0[4],r64[4];std::memcpy(r0,s3.inlineBlobs[b].data()+48,16);std::memcpy(r64,s3.inlineBlobs[b].data()+112,16);
        float l0[4],l64[4];std::memcpy(l0,s3.liveSrt.data()+48,16);std::memcpy(l64,s3.liveSrt.data()+112,16);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=srt-delta;replay=%s;m0-row3=%.9g,%.9g,%.9g,%.9g;was=%.9g,%.9g,%.9g,%.9g;"
            "m64-row3=%.9g,%.9g,%.9g,%.9g;was=%.9g,%.9g,%.9g,%.9g",replay_names[b+1],r0[0],r0[1],r0[2],r0[3],l0[0],l0[1],l0[2],l0[3],
            r64[0],r64[1],r64[2],r64[3],l64[0],l64[1],l64[2],l64[3]);
        step1Emit(st.text);
    }
    // Clip of the first sampled vertices under the LIVE matrices (all four replays).
    if(!s3.wholeSet&&substitute(s3.liveSrt.data(),s3.sub)) {
        (void)project(s3.live,s3.targets,s3.bestVb.data(),s3.bestVbFirst,s3.bestVbCount,s3.vbStride,
                      s3.live.indexed?s3.bestIb.data():nullptr,s3.live.ib_stride,s3.sub,s3.crop);
        int n=0;
        for(unsigned r=0;r<replay_count&&s3.crop.vertices;++r) {
            n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=clip;source=live-srt;replay=%s;xyzw=",replay_names[r]);
            for(uint32_t i=0;i<s3.crop.vertices&&i<8&&n>0&&size_t(n)<sizeof st.text;++i)
                n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),i?";%.4g,%.4g,%.4g,%.4g":"%.4g,%.4g,%.4g,%.4g",
                    s3.crop.clip[r][i][0],s3.crop.clip[r][i][1],s3.crop.clip[r][i][2],s3.crop.clip[r][i][3]);
            step1Emit(st.text);
        }
    }
}
void step3EmitSetReplay(Step1State& st) noexcept;
void step3EmitReplay(Step1State& st) noexcept {
    if(st.s3.wholeSet){step3EmitSetReplay(st);return;}
    const auto& s3=st.s3;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=replay;order=OMSet(private),4x[ClearRTV%s,%sDraw,Bar(RT->CS),"
        "Copy,Bar(CS->RT)],SRV(t10:original),OMSet(game-copies);srv-set=control,left,right;planned=%u;issued=%u;"
        "restore=om:n%u,single%u,rtv0:%llx,dsv:%u:%llx;t10:p%u,va:%llx;observed-va=%llx;va-same=%u;mismatched-draws=%llu;"
        "waiting-draws=%llu;srt=%s;srt-unreadable=%llu;same-signature-other-ordinal=%llu;skipped=%llu;game-barriers=none",
        s3.targets.has_depth?",ClearDSV":"","[SRV],",s3.targets.commands,s3.issued.load(),
        s3.omCount,unsigned(s3.omSingle!=FALSE),static_cast<unsigned long long>(s3.omRtv[0].ptr),unsigned(s3.omDsv),
        static_cast<unsigned long long>(s3.omDsvHandle.ptr),s3.t10,static_cast<unsigned long long>(s3.t10Original),
        static_cast<unsigned long long>(s3.shape.t10_va),unsigned(s3.t10Original==s3.shape.t10_va),
        static_cast<unsigned long long>(s3.mismatches),static_cast<unsigned long long>(s3.waitingDraws),s3.freshness,
        static_cast<unsigned long long>(s3.freshUnreadable),static_cast<unsigned long long>(s3.orderShifted),
        static_cast<unsigned long long>(s3.skipped));
    step1Emit(st.text);
    step3EmitLive(st);
}
// Worker, step-1 mutex held: emits what step3Process computed.
void step3EvaluateSet(Step1State& st) noexcept;
void step3Evaluate(Step1State& st) noexcept {
    if(st.s3.wholeSet){step3EvaluateSet(st);return;}
    using namespace gpu_copy::step3;
    const auto& s3=st.s3;const auto& t=s3.targets;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=map;mode=step3;hr=%08lx;range=0:%llu;unmap-written=0:0;footprints=%u;"
        "stride=%llu;pitch=%llu;crop=%ux%u@%u,%u;rt=%ux%u;format=%s;png-dir=%s",static_cast<unsigned long>(s3.mapHr),
        static_cast<unsigned long long>(s3.pixelBytes),replay_count,static_cast<unsigned long long>(t.footprint_stride),
        static_cast<unsigned long long>(t.row_pitch),t.crop_w,t.crop_h,t.crop_left,t.crop_top,t.width,t.height,t.color.name,g_step3PngDirUtf8);
    step1Emit(st.text);
    // Coordinates are RT texels (crop origin added); sha256 is over the raw
    // crop rows in the RT format (FP16 halves for float targets).
    const uint32_t ox=t.crop_left,oy=t.crop_top;
    for(unsigned i=0;i<replay_count;++i) {
        const auto& p=s3.stats[i];
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=pixels;replay=%s;covered=%llu;bbox=%u,%u,%u,%u;centroid=%.3f,%.3f;"
            "alpha=%u-%u;raw-sha256=%s;area=%ux%u@%u,%u",replay_names[i],
            static_cast<unsigned long long>(p.count),p.minx+ox,p.miny+oy,p.maxx+ox,p.maxy+oy,p.cx+ox,p.cy+oy,p.alpha_min,p.alpha_max,s3.sha[i],
            t.crop_w,t.crop_h,ox,oy);
        step1Emit(st.text);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png;replay=%s;png=%s;png-ok=%u;png-bytes=%llu;png-sha256=%s;small=%s;"
            "small-size=%ux%u;small-bytes=%llu;small-sha256=%s",replay_names[i],s3.png[i],unsigned(s3.pngOk[i]),
            static_cast<unsigned long long>(s3.pngBytes[i]),s3.pngSha[i],s3.pngSmall[i],s3.smallW,s3.smallH,
            static_cast<unsigned long long>(s3.pngSmallBytes[i]),s3.pngSmallSha[i]);
        step1Emit(st.text);
    }
    if(t.color.half)for(unsigned i=0;i<replay_count;++i) {
        const auto& f=s3.f16[i];
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=f16;replay=%s;texels=%llu;nonfinite=%llu;r=%.4g/%.4g/%.4g;g=%.4g/%.4g/%.4g;"
            "b=%.4g/%.4g/%.4g;a=%.4g/%.4g/%.4g(min/max/mean,covered);raw-sha256=%s;png=clamp01-x255-linear-no-gamma",replay_names[i],
            static_cast<unsigned long long>(f.count),static_cast<unsigned long long>(f.nonfinite),f.min[0],f.max[0],f.mean[0],
            f.min[1],f.max[1],f.mean[1],f.min[2],f.max[2],f.mean[2],f.min[3],f.max[3],f.mean[3],s3.sha[i]);
        step1Emit(st.text);
    }
    const auto& l=s3.stats[replay_left];const auto& r=s3.stats[replay_right];
    const double dx=l.count&&r.count?l.cx-r.cx:0,dy=l.count&&r.count?l.cy-r.cy:0;
    const double w=s3.shape.vp0.w;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=stereo;disparity-px=%.3f;dy-px=%.3f;bbox-dx=%d,%d;sign=%s;"
        "expected-px=%.2f@%g,%.2f@%g,%.2f@%g(estimate);control-vs-original=%s:%llu,first:%lld;left-vs-right-differ=%llu;"
        "channels=raw-%s%s",dx,dy,int(l.minx)-int(r.minx),int(l.maxx)-int(r.maxx),
        dx>0?"left-minus-right-positive":dx<0?"left-minus-right-negative":"zero",
        expected_disparity_px(s3.sub,depth_near_units,w),depth_near_units,expected_disparity_px(s3.sub,depth_median_units,w),
        depth_median_units,expected_disparity_px(s3.sub,depth_far_units,w),depth_far_units,
        s3.controlDiff?"differ":"equal",static_cast<unsigned long long>(s3.controlDiff),static_cast<long long>(s3.controlFirst),
        static_cast<unsigned long long>(s3.eyeDiff),t.color.half?"fp16-png-clamped-scaled":t.color.srgb?"srgb-encoded":"linear",
        t.color.bgra?"-bgra-reordered-to-rgba":"");
    step1Emit(st.text);
}
void step3EmitResult(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    const auto& s3=st.s3;
    unsigned pngs=0;for(bool b:s3.pngOk)pngs+=b;
    const auto& l=s3.stats[replay_left];const auto& r=s3.stats[replay_right];
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;STEP3_RESULT;observed=%u;observe=%s;armed=%u;inserted=%u;issued=%u;read=%u;"
        "covered=o:%llu,c:%llu,l:%llu,r:%llu;disparity-px=%.3f;control=%s:%llu;pngs=%u/4;content=%s;wait=%s;mismatch=%s:%llu;"
        "observe-to-end-ms=%llu;restore=om+t10-from-tracking;game-barriers=none;game-writes=none",unsigned(s3.observed),s3.refusal?s3.refusal:"ok",
        unsigned(s3.armed),unsigned(st.recorded),s3.issued.load(),unsigned(s3.read),
        static_cast<unsigned long long>(s3.stats[0].count),static_cast<unsigned long long>(s3.stats[1].count),
        static_cast<unsigned long long>(l.count),static_cast<unsigned long long>(r.count),
        l.count&&r.count?l.cx-r.cx:0.0,s3.controlDiff?"differ":"equal",static_cast<unsigned long long>(s3.controlDiff),pngs,
        s3.read?(s3.contentFail?s3.contentFail:"ok"):"unread",s3.waitReason,s3.mismatch?s3.mismatch:"none",
        static_cast<unsigned long long>(s3.mismatches),
        static_cast<unsigned long long>(s3.tObserved?GetTickCount64()-s3.tObserved:0));
    step1Emit(st.text);
}

// ------------------------------------------------------------- whole set: results (worker)
void step3EmitSetReplay(Step1State& st) noexcept {
    const auto& s3=st.s3;
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=set-replay;members=%u;recorded=%u;skipped=%u;not-reached=%u;stop=%s;"
        "copies=%s;issued=%u;planned=%u;per-member=8;clears=%u;copies-cmds=6;restore=after-every-member;srt=inline-live-per-member;"
        "same-signature-other-ordinal=%llu;game-barriers=none",s3.memberCount,s3.setRecorded,s3.setSkipped,
        s3.memberCount-s3.setRecorded-s3.setSkipped,s3.setStop?s3.setStop:"none",s3.copiesAtClose?"at-close":"after-last-member",
        s3.issued.load(),s3.targets.commands,s3.targets.has_depth?4u:2u,static_cast<unsigned long long>(s3.orderShifted));
    step1Emit(st.text);
    int n=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=set-members;ordinal:status=");
    for(unsigned i=0;i<s3.memberCount&&n>0&&size_t(n)<sizeof st.text-40;++i)
        n+=std::snprintf(st.text+n,sizeof st.text-size_t(n),i?",%llu:%s":"%llu:%s",
            static_cast<unsigned long long>(s3.members[i].shape.draw_ordinal),s3.members[i].status);
    step1Emit(st.text);
    step3EmitLive(st); // the LAST recorded member's live bytes and blobs
}
inline void step3ChannelMeans(const uint8_t* base,uint32_t w,uint32_t h,uint64_t pitch,const gpu_copy::step3::ColorFormat& f,
                              gpu_copy::step3::FloatStats& out) noexcept {
    using namespace gpu_copy::step3;
    if(f.half){float_stats(base,w,h,pitch,out);return;}
    out=FloatStats{};std::array<double,4> sum{};
    for(int c=0;c<4;++c){out.min[size_t(c)]=1e300;out.max[size_t(c)]=-1e300;}
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const uint8_t* p=base+uint64_t(y)*pitch+uint64_t(x)*4;if(!covered(p,4))continue;++out.count;
        const double v[4]={(f.bgra?p[2]:p[0])/255.0,p[1]/255.0,(f.bgra?p[0]:p[2])/255.0,p[3]/255.0};
        for(int c=0;c<4;++c){sum[size_t(c)]+=v[c];if(v[c]<out.min[size_t(c)])out.min[size_t(c)]=v[c];if(v[c]>out.max[size_t(c)])out.max[size_t(c)]=v[c];}
    }
    for(int c=0;c<4;++c){if(out.count)out.mean[size_t(c)]=sum[size_t(c)]/double(out.count);else out.min[size_t(c)]=out.max[size_t(c)]=0;}
}
// Worker, NO lock held. Footprint 0 = left, 1 = right.
void step3ProcessSet(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    auto& s3=st.s3;if(!s3.read||s3.processed)return;
    s3.processed=true;
    const auto& t=s3.targets;const uint8_t* base=s3.pixels.get();const uint32_t bpp=t.color.bytes,w=t.crop_w,h=t.crop_h;
    auto at=[&](unsigned i){return base+uint64_t(i)*t.footprint_stride;};
    for(unsigned eye=0;eye<2;++eye) {
        analyse(at(eye),w,h,t.row_pitch,bpp,s3.stats[eye]);
        step3PixelSha(at(eye),w,h,t.row_pitch,bpp,s3.sha[eye]);
        step3ChannelMeans(at(eye),w,h,t.row_pitch,t.color,s3.f16[eye]);
    }
    s3.eyeDiff=differing(at(0),at(1),w,h,t.row_pitch,bpp,s3.eyeFirst);
    profile_disparity(at(0),at(1),w,h,t.row_pitch,t.color,int(w/4<512?w/4:512),s3.xcorr);
    const std::wstring dir=step3OutputDir();
    if(!dir.empty()) {
        const int n=WideCharToMultiByte(CP_UTF8,0,dir.c_str(),-1,g_step3PngDirUtf8,int(sizeof g_step3PngDirUtf8),nullptr,nullptr);
        if(n<=0)std::snprintf(g_step3PngDirUtf8,sizeof g_step3PngDirUtf8,"unprintable");
    } else std::snprintf(g_step3PngDirUtf8,sizeof g_step3PngDirUtf8,"unavailable");
    auto write=[&](const char* name,const std::vector<uint8_t>& bytes) {
        std::wstring path;
        try {path=dir+L"\\";for(const char* c=name;*c;++c)path.push_back(wchar_t(*c));} catch(...) {return false;}
        return step3WriteFile(path,bytes);
    };
    // Full + <=640-wide box-filtered copy of an RGBA8 image.
    auto emitPair=[&](const std::vector<uint8_t>& rgba,char* full,size_t fullCap,char* lite,size_t liteCap,char (&fullSha)[65],
                      char (&liteSha)[65],uint64_t& fullBytes,uint64_t& liteBytes,const char* eye,const char* variant)->bool {
        std::snprintf(full,fullCap,"acz-vr-step3-%lu-%llu-%s%s.png",GetCurrentProcessId(),static_cast<unsigned long long>(st.tStart),eye,variant);
        std::snprintf(lite,liteCap,"acz-vr-step3-%lu-%llu-%s%s-small.png",GetCurrentProcessId(),static_cast<unsigned long long>(st.tStart),eye,variant);
        std::vector<uint8_t> png,sm,smPng;uint32_t sw=0,sh=0;
        bool ok=!dir.empty()&&png_encode(rgba.data(),w,h,uint64_t(w)*4,false,png)&&write(full,png);
        fullBytes=png.size();step1Sha256(png.data(),png.size(),fullSha);
        const bool sok=ok&&downscale_rgba8(rgba,w,h,downscale_factor(w,640),sm,sw,sh)&&png_encode(sm.data(),sw,sh,uint64_t(sw)*4,false,smPng)&&
            write(lite,smPng);
        liteBytes=smPng.size();step1Sha256(smPng.data(),smPng.size(),liteSha);s3.smallW=sw;s3.smallH=sh;
        return ok&&sok;
    };
    bool allPng=true;
    static constexpr const char* eyes[2]={"left","right"};
    for(unsigned eye=0;eye<2;++eye) {
        std::vector<uint8_t> rgba,srgb;uint64_t sb=0,ssb=0;
        bool ok=to_rgba8(at(eye),w,h,t.row_pitch,t.color,rgba)&&
            emitPair(rgba,s3.png[eye],sizeof s3.png[eye],s3.pngSmall[eye],sizeof s3.pngSmall[eye],s3.pngSha[eye],s3.pngSmallSha[eye],
                     s3.pngBytes[eye],s3.pngSmallBytes[eye],eyes[eye],"");
        ok=ok&&to_srgb8(at(eye),w,h,t.row_pitch,t.color,srgb)&&
            emitPair(srgb,s3.pngSrgb[eye],sizeof s3.pngSrgb[eye],s3.pngSrgbSmall[eye],sizeof s3.pngSrgbSmall[eye],s3.pngSrgbSha[eye],
                     s3.pngSrgbSmallSha[eye],sb,ssb,eyes[eye],"-srgb");
        s3.pngOk[eye]=ok;allPng=allPng&&ok;
    }
    if(!s3.stats[0].count||!s3.stats[1].count)s3.contentFail="eye-set-empty";
    else if(!allPng)s3.contentFail="png-write-failed";
}
void step3EvaluateSet(Step1State& st) noexcept {
    using namespace gpu_copy::step3;
    const auto& s3=st.s3;const auto& t=s3.targets;
    static constexpr const char* eyes[2]={"left","right"};
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=map;mode=step3-set;hr=%08lx;range=0:%llu;unmap-written=0:0;footprints=2;"
        "stride=%llu;pitch=%llu;size=%ux%u;format=%s;png-dir=%s",static_cast<unsigned long>(s3.mapHr),
        static_cast<unsigned long long>(s3.pixelBytes),static_cast<unsigned long long>(t.footprint_stride),
        static_cast<unsigned long long>(t.row_pitch),t.crop_w,t.crop_h,t.color.name,g_step3PngDirUtf8);
    step1Emit(st.text);
    for(unsigned eye=0;eye<2;++eye) {
        const auto& p=s3.stats[eye];const auto& f=s3.f16[eye];
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=pixels;eye=%s;covered=%llu;bbox=%u,%u,%u,%u;centroid=%.3f,%.3f;"
            "mean-rgba=%.4g,%.4g,%.4g,%.4g;max-rgba=%.4g,%.4g,%.4g,%.4g;nonfinite=%llu;raw-sha256=%s",eyes[eye],
            static_cast<unsigned long long>(p.count),p.minx,p.miny,p.maxx,p.maxy,p.cx,p.cy,f.mean[0],f.mean[1],f.mean[2],f.mean[3],
            f.max[0],f.max[1],f.max[2],f.max[3],static_cast<unsigned long long>(f.nonfinite),s3.sha[eye]);
        step1Emit(st.text);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png;eye=%s;png=%s;png-bytes=%llu;png-sha256=%s;small=%s;small-size=%ux%u;"
            "small-sha256=%s;linear-clamp01",eyes[eye],s3.png[eye],static_cast<unsigned long long>(s3.pngBytes[eye]),s3.pngSha[eye],
            s3.pngSmall[eye],s3.smallW,s3.smallH,s3.pngSmallSha[eye]);
        step1Emit(st.text);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=png-srgb;eye=%s;png=%s;png-sha256=%s;small=%s;small-sha256=%s;"
            "ok=%u;linear-to-srgb-gamma,alpha-linear",eyes[eye],s3.pngSrgb[eye],s3.pngSrgbSha[eye],s3.pngSrgbSmall[eye],
            s3.pngSrgbSmallSha[eye],unsigned(s3.pngOk[eye]));
        step1Emit(st.text);
    }
    const auto& l=s3.stats[0];const auto& r=s3.stats[1];
    std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=stereo-set;centroid-dx=%.3f;centroid-dy=%.3f;xcorr-shift=%.3f;xcorr-score=%.4f;"
        "xcorr-valid=%u;xcorr=column-luminance-profile,central-rows;diff-pixels=%llu;sign=%s;scene=cockpit-draws-only",
        l.count&&r.count?l.cx-r.cx:0.0,l.count&&r.count?l.cy-r.cy:0.0,s3.xcorr.shift,s3.xcorr.score,unsigned(s3.xcorr.valid),
        static_cast<unsigned long long>(s3.eyeDiff),s3.xcorr.shift>0?"left-minus-right-positive":s3.xcorr.shift<0?"left-minus-right-negative":"zero");
    step1Emit(st.text);
}
