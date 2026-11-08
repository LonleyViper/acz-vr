// gpu_copy_step5_fxsprite.inl -- EFFECTS-OUTSIDE (E) fxmem:5: capture of the game's ps2_v SPRITE effects and the CPU dry run of their per-eye re-projection
// (fx_sprite_replay.h; docs/world-render/effects-outside-window.md section 10). Two classes: 0 = the EXPLOSION pipeline (PS 542a11d7 = extra7_p), 1 = the
// indexed GREATER 4caf6e2d sprite draws (ps2AtestCA_p: the gun-tracer candidates; quad shapes logged to tell tracers from specks). Included by
// gpu_copy_step5_hooks.inl inside its anonymous namespace, after Step5State. READ ONLY: the draw's index and vertex bytes are copied from the game's pinned,
// CPU-mapped UPLOAD memory (step3MappedSpan_locked + guarded reads, as the TACTICAL sprite reads); with fxmem:5 nothing is recorded into a command list (the
// window effects replay of the draw is decided by the class gate as before). EFFECTS-OUTSIDE (F) fxmem:6 (bottom of this file; docs section 11): a planned,
// admitted draw is replayed per eye from its re-projected vertex copies INSTEAD of the window replay. Flag off (default): every function returns at once.
inline bool step5FxSpriteOn() noexcept {return gpu_copy::step5::effects_explosion_flag().load(std::memory_order_relaxed);}
// EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h; docs section 11): the planned draws replayed per eye on the GPU (resources: step5CreateSpriteGpu).
inline bool step5FxSpriteGpuOn() noexcept {return gpu_copy::step5::effects_sprite_gpu_flag().load(std::memory_order_relaxed);}
// (K) the per-eye sprite path takes every draw the effects layer can accept in one frame (docs section 17: a smaller cap sent the excess to the window image)
static_assert(acz::fxsprite::gpu::replay_bytes(acz::fxpuff::kMaxEmitVerts)<=acz::fxsprite::gpu::kTrailBytes,"the cache's region holds one full host selection");
static_assert(acz::fxsprite::gpu::kMaxDraws>=gpu_copy::step5::effects_draw_cap,"fxmem:6 sprite draw cap below the effects layer's per-frame cap");
// The class of a draw (-1 = not captured).
// (G) fxmem:6: the GUN TRACER (live batch G): PS d34ee153 + VS 534f29ca with the exact additive blend (the PSO carries tracer_ready), indexed strips (2-5 quads,
// 0xFFFF restarts) or a non-indexed single quad. Without fxmem:6 the class gate denies it (HUD sprite shader) and nothing is captured.
// (J, headset 2026-10-29 items 5/7) with fxmem:6 the NON-indexed GREATER 4caf6e2d quads are class 1 too (gpu_copy_step5.h effects_sprite_class): before, they
// stayed in the window effects image (window-only clouds, under the objects).
inline int step5FxSpriteClass(const Step5EffId& id,const gpu_copy::step3::Shape& d) noexcept {
    return gpu_copy::step5::effects_sprite_class(id.ps8,id.vs8,d.indexed,d.pso.depth_func,d.pso.tracer_ready,step5FxSpriteGpuOn());
}
inline const char* step5FxSpriteClassName(int c) noexcept {return c==0?"explosion":c==2?"tracer":"sprite(tracer-cand)";}
// Frame start (step5EffectsArm_locked).
inline void step5FxSpriteBegin(Step5State& s5) noexcept {s5.fxSprOpen=step5FxSpriteOn();if(s5.fxSprOpen){s5.fxSpr[0].begin();s5.fxSpr[1].begin();s5.fxSpr[2].begin();}
    // WORLD-CLOUDS wcdiag:1: this frame samples the game's puff quads when no line is pending and >= 250 ms passed since the last one
    s5.fxPuffOpen=s5.fxSprOpen&&!s5.fxPuffPending&&gpu_copy::step5::effects_cloud_puff_diag_flag().load(std::memory_order_relaxed)&&GetTickCount64()-s5.fxPuffLastMs>=250;
    if(s5.fxPuffOpen){s5.fxPuffN=0;s5.fxPuffSeen=0;s5.fxPuffEyeOk=false;}
    // EFFECTS-OUTSIDE (M) fxtrail (fxmem:6 only): the puff memory's frame (its own counter: constant within a frame, +1 per armed frame)
    s5.fxTrailOpt=acz::fxpuff::options(double(gpu_copy::step5::effects_puff_trail_life_s().load(std::memory_order_relaxed)),
        double(gpu_copy::step5::effects_puff_trail_max_edge().load(std::memory_order_relaxed)),gpu_copy::step5::effects_puff_trail_cls().load(std::memory_order_relaxed),
        double(gpu_copy::step5::effects_puff_trail_near().load(std::memory_order_relaxed)),
        double(gpu_copy::step5::effects_puff_cloud_life_s().load(std::memory_order_relaxed)),double(gpu_copy::step5::effects_puff_cloud_far().load(std::memory_order_relaxed)),
        gpu_copy::step5::effects_puff_trail_xkey().load(std::memory_order_relaxed),gpu_copy::step5::effects_puff_clip_mode().load(std::memory_order_relaxed),
        gpu_copy::step5::effects_puff_atlas_flag().load(std::memory_order_relaxed)); // (M4) fxcloud / fxcfar / fxtxkey, (M4c) fxtclip, (M6) fxatlas
    s5.fxTrailOpen=s5.fxSprOpen&&step5FxSpriteGpuOn()&&s5.fxTrailOpt.on;
    if(s5.fxTrailOpen){++s5.fxTrailFrame;s5.fxTrailGameOk=false;}}
// EFFECTS-OUTSIDE (M) the binding key of a class-1 draw: the value of the bound root signature's FIRST descriptor-table parameter (the live 4caf6e2d draws:
// parameter 2, `2:T0=15678a0012a280` in fx-sig-draw); 0 = unknown (the puff memory then neither stores nor re-emits for the draw). Hook-state mutex held.
// (M4) also the host identity of the draw: `param` = that root parameter's index (the GPU replay re-binds a stored card's key there), `pso` = the game PSO,
// `heap` = the XOR of the bound descriptor heaps (a key is only re-bound under the same heaps). Opaque comparison values, never dereferenced.
__declspec(noinline) uint64_t step5FxTrailKey_locked(const ListState& ls,uint32_t& param,uint64_t& pso,uint64_t& heap) noexcept {
    param=UINT32_MAX;pso=uint64_t(reinterpret_cast<uintptr_t>(ls.pso.get()));heap=0;
    for(const auto& h:ls.heaps)heap^=uint64_t(reinterpret_cast<uintptr_t>(h.get()));
    const auto& b=ls.bindings;
    if(!b.root||!b.root->valid())return 0;
    const auto& L=b.root->layout();
    for(unsigned i=0;i<L.parameters.size();++i) {
        if(L.parameters[i].kind!=acz::metadata::ParameterKind::table)continue;
        const auto it=b.roots.find(i);
        if(it==b.roots.end())return 0;
        param=i;
        return uint64_t(it->second.value);
    }
    return 0;
}
// EFFECTS-OUTSIDE (M6) fxatlas (fx_puff_atlas.h; docs section 23): what a host draw needs to capture into / draw from our atlas, read at the Draw seam
// (hook-state mutex held; no COM call here: step1Native takes that mutex). The heaps are the ones the game has bound in this list right now (the game keeps
// them alive while the list can run); our recording only binds them back. `why` = nullptr: the atlas group can be drawn under this host.
struct Step5AtlasHost {
    const char* why="atlas-not-checked";
    ID3D12PipelineState* copy=nullptr;uintptr_t copyDevice=0;   // this game PSO's capture pipeline (null: no capture at this draw) and its device
    ID3D12DescriptorHeap* game[2]{};unsigned ngame=0;          // the game's bound descriptor heaps (bound back after the atlas group)
    ID3D12DescriptorHeap* sampler=nullptr;                      // (M6b) the one of them that is the SAMPLER heap (classified outside the hook mutex)
    unsigned keyCount=0;                                        // descriptors of the key table (our heap holds kTableMax copies of the atlas SRV)
    // (M6b) every descriptor table of the bound root signature and its game value (all set again after the heap change), and the layout as text
    uint32_t tparam[acz::fxpuff::atlas::kMaxTables]{};uint64_t tvalue[acz::fxpuff::atlas::kMaxTables]{};unsigned ntables=0;bool samplerTables=false;
    char layout[72]{};
};
__declspec(noinline) void step5FxAtlasHost_locked(const ListState& ls,const PipelineRecord* rec,uint32_t keyParam,Step5AtlasHost& h) noexcept {
    namespace A=acz::fxpuff::atlas;
    h=Step5AtlasHost{};
    if(rec){h.copy=rec->effectsAtlasCopy.get();h.copyDevice=rec->effectsAtlasDevice;}
    const auto& b=ls.bindings;
    if(keyParam==UINT32_MAX){h.why="atlas-key-param";return;}
    if(!b.root||!b.root->valid()){h.why="atlas-root-unknown";return;}
    const auto& L=b.root->layout();
    {   // (M6b) the layout as text (logged with a refusal: the null run's atlas-other-table named no layout)
        A::ParamDesc pd[8];unsigned np=0;
        using PK=acz::metadata::ParameterKind;using BK=acz::metadata::BindingKind;using VI=acz::metadata::Visibility;
        for(unsigned i=0;i<L.parameters.size()&&np<8;++i) {
            const auto& P=L.parameters[i];A::ParamDesc& q=pd[np++];
            q.kind=P.kind==PK::constants?'C':P.kind==PK::cbv?'V':P.kind==PK::srv?'S':P.kind==PK::uav?'U':'T';
            q.reg=P.shader_register;q.count=P.constant_count;
            q.vis=P.visibility==VI::all?'a':P.visibility==VI::vertex?'v':P.visibility==VI::pixel?'p':'o';
            unsigned nk=0;uint32_t cnt[8]{},off[8]{};unsigned nr=0;
            for(const auto& r:P.ranges) {
                const char k=r.kind==BK::srv?'s':r.kind==BK::cbv?'c':r.kind==BK::uav?'u':'z';
                bool have=false;for(unsigned j=0;j<nk;++j)have=have||q.kinds[j]==k;
                if(!have&&nk<4)q.kinds[nk++]=k;
                if(nr<8){cnt[nr]=r.count;off[nr]=r.offset;++nr;}
            }
            if(!A::table_span(cnt,off,nr,q.span))q.span=0xffffu;
        }
        A::layout_text(pd,np,h.layout,sizeof h.layout);
    }
    if(ls.heaps.empty()||ls.heaps.size()>2){h.why="atlas-heap-count";return;}
    for(const auto& hp:ls.heaps){if(!hp){h.why="atlas-heap-null";return;}h.game[h.ngame++]=hp.get();}
    A::TableDesc t[A::kMaxTables];unsigned n=0;
    for(unsigned i=0;i<L.parameters.size();++i) {
        const auto& P=L.parameters[i];
        if(P.kind!=acz::metadata::ParameterKind::table)continue;
        if(n>=A::kMaxTables){h.why="atlas-table-count";return;}
        A::TableDesc& d=t[n++];d.param=i;d.srv_only=!P.ranges.empty()&&P.ranges.size()<=8;d.sampler_only=d.srv_only;
        uint32_t counts[8]{},offsets[8]{};unsigned nr=0;
        for(const auto& r:P.ranges){if(nr>=8)break;d.srv_only=d.srv_only&&r.kind==acz::metadata::BindingKind::srv;
            d.sampler_only=d.sampler_only&&r.kind==acz::metadata::BindingKind::sampler;counts[nr]=r.count;offsets[nr]=r.offset;++nr;}
        d.unbounded=!A::table_span(counts,offsets,nr,d.span);
        const auto it=b.roots.find(i);d.value_known=it!=b.roots.end();
        h.tparam[n-1]=i;h.tvalue[n-1]=d.value_known?uint64_t(it->second.value):0;
        if(d.param!=keyParam)h.samplerTables=true;
    }
    h.ntables=n;
    h.why=A::host_why(t,n,keyParam,h.ngame,h.keyCount);
}
// (M6b) Step-1 mutex held, hook-state mutex NOT held (step1Native takes it: the documented order): which of the host's bound heaps is the SAMPLER heap.
// Only when the root signature has sampler tables (otherwise the atlas group binds our heap alone). No match = refused (atlas-sampler-heap).
inline void step5FxAtlasClassify(Step5AtlasHost& h) noexcept {
    if(h.why||!h.samplerTables)return;
    using DescFn=D3D12_DESCRIPTOR_HEAP_DESC*(STDMETHODCALLTYPE*)(ID3D12DescriptorHeap*,D3D12_DESCRIPTOR_HEAP_DESC*);
    unsigned samplers=0,views=0;
    for(unsigned k=0;k<h.ngame;++k) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};reinterpret_cast<DescFn>(step1Native(h.game[k],8))(h.game[k],&d);
        if(d.Type==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER){h.sampler=h.game[k];++samplers;}
        else if(d.Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)++views;
    }
    if(samplers!=1||views!=1){h.sampler=nullptr;h.why="atlas-sampler-heap";}
}
// (M4) the memory runs while fxtrail OR fxcloud is set (fxmem:6).
inline bool step5FxTrailActive() noexcept {
    return gpu_copy::step5::effects_puff_trail_life_s().load(std::memory_order_relaxed)>0.0f||gpu_copy::step5::effects_puff_cloud_life_s().load(std::memory_order_relaxed)>0.0f;
}
// (M3) the world position (record translation, as TACTICAL object_from_record) of every world object drawn this frame, for the near rule. `first` = the
// frame's opener record (the list restarts). Step-1 mutex held; only while fxtrail is set.
inline void step5FxTrailObject(Step5State& s5,const uint8_t* raw,bool first) noexcept {
    if(!raw||!step5FxTrailActive())return;
    if(first)s5.fxTrailObjN=0;
    if(s5.fxTrailObjN>=acz::fxpuff::kObjCap)return;
    float t[3];std::memcpy(t,raw+4*76,12);
    if(!std::isfinite(t[0])||!std::isfinite(t[1])||!std::isfinite(t[2]))return;
    for(int k=0;k<3;++k)s5.fxTrailObj[s5.fxTrailObjN][k]=t[k];
    ++s5.fxTrailObjN;
}
inline bool step5FxTrailWanted() noexcept {return step5FxSpriteGpuOn()&&step5FxTrailActive();}
// (M) drop the current draw's selection (the GPU plan did not take it): the puffs are not drawn this frame; a later replay of the key may select them again.
inline void step5FxTrailDrop(Step5State& s5,const char* why) noexcept {
    if(s5.fxTrailSelN)s5.fxTrail.unselect(s5.fxTrailFrame,s5.fxTrailSel,s5.fxTrailSelN,why);
    s5.fxTrailSelN=0;s5.fxTrailVerts[0]=s5.fxTrailVerts[1]=0;s5.fxTrailGroupN[0]=s5.fxTrailGroupN[1]=0;
}
// Step-1 mutex held; one draw of class `c` (`d` = the HUD seam's live Shape). Nests the hook-state mutex for the mapped reads (documented order).
// fxmem:6 (`keep`: the frame latched the GPU replay): a draw that ends PREPARED also leaves both eye copies in s5.fxSprKeep and fxSprLast* says so (the GPU
// plan of the same draw reads them right after, under the same mutex hold); every other outcome leaves fxSprLastPrepared false.
// (M4) trailPso / trailHeap / trailParam: the host identity of the draw (step5FxTrailKey_locked). (M6) atlasHost: this draw can draw the atlas group
// (fxatlas on, the atlas ready, step5FxAtlasHost_locked passed).
__declspec(noinline) void step5FxSpriteCapture(Step5State& s5,const gpu_copy::step3::Shape& d,int c,bool keep=false,uint64_t trailKey=0,uint64_t trailPso=0,
                                               uint64_t trailHeap=0,uint32_t trailParam=UINT32_MAX,bool atlasHost=false) noexcept {
    namespace fs=acz::fxsprite;
    step5FxTrailDrop(s5,s5.fxTrailSelN?"trail-stale-selection":nullptr); // (M) never left over from an earlier draw
    s5.fxTrail.seen_n_=0; // (M6) capture candidates belong to THIS draw's observe only (they are captured from the key this draw binds)
    s5.fxSprLastPrepared=false;s5.fxSprLastClass=c;s5.fxSprLastLo=0;s5.fxSprLastN=0;
    if(!s5.fxSprOpen||c<0||c>2)return;
    auto& p=s5.fxSpr[c];
    const char* why=nullptr;uint32_t lo=0,hi=0;
    // (G) the tracer class also takes a NON-indexed draw (the first bullet: one 4-vertex strip quad): its vertices are [start, start+count) of the slot-0 view and
    // the quad list is synthesised (0..count-1 in groups of 4 separated by 0xFFFF restarts) so that plan_draw / collapse / widen treat it like the indexed strips.
    const bool nonIdx=gpu_copy::step5::effects_sprite_nonindexed_class(c)&&!d.indexed; // (J) class 1 too (non-indexed GREATER 4caf6e2d quads, fxmem:6 only)
    size_t ni=d.count; // index list length handed to plan_draw
    // bounds BEFORE any read
    bool niQuads=true; // (K) non-indexed: the strip-quad list (true) or ONE run without restarts (class 1 strips / lists of other counts)
    bool isPts=false;  // (P) fxadd:1: a class-1 POINT-list run (one pixel per vertex; behind-eye points culled)
    const bool ptsOn=c==1&&gpu_copy::step5::effects_additive_flag().load(std::memory_order_relaxed);
    if(!d.indexed&&!nonIdx)why="sprite-not-indexed";
    else if(d.vb_stride!=fs::kVertexStride)why="sprite-vb-stride";
    else if(!nonIdx&&d.ib_stride!=2)why="sprite-ib-not-r16";
    else if(nonIdx&&(why=gpu_copy::step5::effects_sprite_nonindexed_shape(c,d.count,d.instances,d.topology,fs::kMaxVertices,niQuads,ptsOn,&isPts))!=nullptr) // w12-2 finding 1 + (K) + (P)
        p.note_nonindexed_refused(d.count,d.topology);
    else if(!d.count||d.count>fs::kMaxIndices)why="sprite-index-count";
    else if(!d.vb_va||!d.vb_bytes||(!nonIdx&&(!d.ib_va||!d.ib_bytes)))why="sprite-ia-unbound";
    else if(!nonIdx&&(uint64_t(d.start)+d.count)*2>d.ib_bytes)why="sprite-index-outside-view";
    else if(nonIdx&&(uint64_t(d.start)+d.count)*fs::kVertexStride>d.vb_bytes)why="sprite-vertex-outside-view";
    if(!why&&nonIdx) {
        auto& s=state();std::lock_guard lock(s.mutex);const uint8_t* vb=nullptr;
        if(const char* bad=step3MappedSpan_locked(d.vb_address,d.vb_va,d.vb_bytes,vb))why=bad;
        else if(!guardedCpuRead(vb+uint64_t(d.start)*fs::kVertexStride,s5.fxSprVtx,size_t(d.count)*fs::kVertexStride))why="sprite-vertex-read-fault";
        else {
            ni=0;for(uint32_t i=0;i<d.count;++i){if(niQuads&&i&&i%4==0)s5.fxSprIdx[ni++]=0xFFFF;s5.fxSprIdx[ni++]=uint16_t(i);}
            lo=0;hi=d.count-1;
        }
    } else if(!why) {
        auto& s=state();std::lock_guard lock(s.mutex);const uint8_t* ib=nullptr;const uint8_t* vb=nullptr;
        if(const char* bad=step3MappedSpan_locked(d.ib_address,d.ib_va+uint64_t(d.start)*2,uint64_t(d.count)*2,ib))why=bad;
        else if(!guardedCpuRead(ib,s5.fxSprIdx,size_t(d.count)*2))why="sprite-index-read-fault";
        else if(!fs::index_range(s5.fxSprIdx,d.count,lo,hi))why="sprite-no-index";
        else if(hi-lo+1>fs::kMaxVertices)why="sprite-vertex-span";
        else {
            const int64_t first=int64_t(lo)+d.base;
            if(first<0||(uint64_t(first)+(hi-lo+1))*fs::kVertexStride>d.vb_bytes)why="sprite-vertex-outside-view";
            else if(const char* bad=step3MappedSpan_locked(d.vb_address,d.vb_va,d.vb_bytes,vb))why=bad;
            else if(!guardedCpuRead(vb+uint64_t(first)*fs::kVertexStride,s5.fxSprVtx,size_t(hi-lo+1)*fs::kVertexStride))why="sprite-vertex-read-fault";
        }
    }
    if(nonIdx)++p.nonindexed; // (J) logged as ;ni=N;qm=L on the plan line
    if(why){++p.draws;++p.refused;p.last_why=why;++s5.fxSprRefused[c];return;}
    ++p.read;++s5.fxSprRead[c];
    // the frame's world camera (as the smoke dry run: the first accepted DrawACM_v record's A and this frame's eye deltas)
    const auto& w=s5.wobj;
    const char* wwhy=!s5.frame.fxObjA?"sprite-world-no-object-record":(!w.camera||w.failed)?"sprite-world-camera-unavailable":!w.pf.valid?"sprite-world-pose-unavailable":nullptr;
    acz::fxmem::ScreenMap game,eye[2];const acz::fxmem::ScreenMap* eyes[2]={nullptr,nullptr};
    if(!wwhy){float af[16];double a[16];std::memcpy(af,w.ref,64);for(int i=0;i<16;++i)a[i]=double(af[i]);if(!fs::game_map_from_a(a,game))wwhy="sprite-world-a-form";}
    if(!wwhy)for(int e=0;e<2;++e){double a[16];
        if(acz::fxsmoke::eye_a_from_delta(w.ref,w.pf.D[e],w.pf.S,a)&&fs::eye_map_from_a(a,eye[e],&game))eyes[e]=&eye[e];else wwhy="sprite-eye-a";}
    if(wwhy){eyes[0]=eyes[1]=nullptr;p.world_why=wwhy;}
    const double scale=gpu_copy::step5::world::depth_fix_scale_for(worldDepthFixConfigured(),s5.ringDsv);
    const uint32_t prepared0=p.prepared;
    // (G) tracer: keep >= 1 eye pixel of width (the eye FX image is s5.ringW x s5.ringH over the 512 x 448 GS window; docs section 12.4)
    fs::Widen widen;if(c==2&&s5.ringW&&s5.ringH){widen.gx=512.0/double(s5.ringW);widen.gy=448.0/double(s5.ringH);widen.min_px=1.0;}
    // WORLD-CLOUDS (wcloud:1 + wcdrop, fxmem:6): class-1 quads with a world edge >= the drop edge are the game's cloud puffs: collapsed in the eye copies
    const double wcDrop=c==1&&!isPts?double(gpu_copy::step5::effects_world_cloud_drop_edge().load(std::memory_order_relaxed)):0.0;
    // EFFECTS-OUTSIDE (M) fxtrail / (M4) fxcloud (docs sections 19-20): with the frame's world camera, every class-1 draw replayed per eye is a HOST: it selects
    // the stored cards the game is not showing whole that it can draw (its own key, or (xkey) another live key under the same PSO and heaps: re-bound per group),
    // their eye copies are built now (uploaded by the GPU plan of this draw); then the draw's whole cards refresh / enter the memory and its CLIPPED quads that a
    // selected card replaces are collapsed in the eye copies (trailDrop -> plan_draw extra_drop).
    uint8_t trailDrop[fs::kMaxVertices];bool trailAny=false;
    if(isPts) {} // (P) a point run is never a puff host nor a card: the trail memory neither observes nor selects for it (its GPU plan drops nothing)
    else if(c==1&&s5.fxTrailOpen&&!wwhy&&game.ok) {
        const uint64_t now=GetTickCount64();const auto& o=s5.fxTrailOpt;
        s5.fxTrailGame=game;s5.fxTrailGameOk=true;
        {   // (M2) observe counts the draw and refuses a missing binding key itself (named on the line)
            if(trailKey&&keep&&eyes[0]&&eyes[1]) {
                // (M4) no known table parameter: no re-binding possible, so the host takes only its own key (pso 0 disables xkey in select)
                const acz::fxpuff::Host host{trailKey,trailParam==UINT32_MAX?0:trailPso,trailHeap,atlasHost&&trailParam!=UINT32_MAX}; // (M6) atlas group
                s5.fxTrailSelN=s5.fxTrail.select(host,s5.fxTrailFrame,game,eyes[0],s5.fxTrailSel,acz::fxpuff::kMaxEmit,o);
                s5.fxTrailSelKey=trailKey;s5.fxTrailParam=trailParam;
                for(int e=0;e<2&&s5.fxTrailSelN;++e)
                    s5.fxTrailVerts[e]=s5.fxTrail.build_eye(eye[e],scale,1.0,s5.fxTrailSel,s5.fxTrailSelN,now,o,s5.fxTrailEye[e],acz::fxpuff::kMaxEmitVerts,
                        s5.fxTrailGroups[e],&s5.fxTrailGroupN[e],&game); // (M7) game map: world-up cards, the game's roll only at the seam
                if(s5.fxTrailSelN&&!s5.fxTrailVerts[0]&&!s5.fxTrailVerts[1])step5FxTrailDrop(s5,nullptr); // every selected card behind both eyes / faded out
            }
            std::memset(trailDrop,0,sizeof trailDrop);
            const uint32_t fade0=s5.fxTrail.fs_.efade;
            if(o.clip==acz::fxpuff::kClipFade)std::memset(s5.fxTrailFade,0,size_t(hi-lo+1));
            trailAny=s5.fxTrail.observe(game,s5.fxSprVtx,hi-lo+1,s5.fxSprIdx,ni,lo,nonIdx&&!niQuads,trailKey,s5.fxTrailFrame,now,o,trailDrop,
                s5.fxTrailObj,s5.fxTrailObjN,trailPso,trailHeap,o.clip==acz::fxpuff::kClipFade?s5.fxTrailFade:nullptr)>0;
            // (M4c) fxtclip:fade: a cut card without a whole copy: its border vertices get alpha 0 in the CPU copy the eye copies are built from (plan_draw
            // below), so the card fades out toward the cut; the game's own buffer is never written
            if(s5.fxTrail.fs_.efade!=fade0)for(uint32_t i=0;i<=hi-lo;++i)if(s5.fxTrailFade[i])s5.fxSprVtx[i].col[3]=0.0f;
        }
    } else if(c==1&&s5.fxTrailOpen)s5.fxTrail.note_nocam(); // (M3) no world camera this frame: counted (nocam), the memory coasts (not a refusal)
    fs::plan_draw(game,eyes,s5.fxSprVtx,hi-lo+1,s5.fxSprIdx,ni,lo,scale,s5.fxSprEye,s5.fxSprBad,p,keep?s5.fxSprKeep[0]:nullptr,keep?s5.fxSprKeep[1]:nullptr,
        widen.on()?&widen:nullptr,wcDrop,nonIdx&&!niQuads,trailAny?trailDrop:nullptr,isPts);   // W15: a non-indexed run without synthetic restarts is ONE continuous GPU assembly: whole-run collapse / puff marking; (P) points culled
    if(c==1&&!isPts&&s5.fxPuffOpen&&game.ok) { // WORLD-CLOUDS wcdiag:1 (read-only census of the copies already read)
        if(!s5.fxPuffEyeOk)s5.fxPuffEyeOk=fs::game_eye(game,s5.fxPuffEye);
        s5.fxPuffN+=fs::puff_samples(game,s5.fxSprVtx,hi-lo+1,s5.fxSprIdx,ni,lo,1000.0,s5.fxPuff+s5.fxPuffN,fs::kPuffPerLine-s5.fxPuffN,s5.fxPuffSeen,nonIdx&&!niQuads);
    }
    if(keep&&p.prepared==prepared0+1){s5.fxSprLastPrepared=true;s5.fxSprLastLo=lo;s5.fxSprLastN=hi-lo+1;}
}
// Per published effects-armed frame (step-1 mutex held): a frame with captured draws becomes the class's pending line when the rate limit admits it; for the
// tracer candidates a frame whose quads include streaks after >= 30 frames without any is always admitted (onset: lines up with the gun key).
// fxmem:6: the frame's GPU replay facts become the pending fx-sprite-gpu line (own rate limit) and are added to the run totals.
inline void step5FxSpriteAccount(Step5State& s5,const Step5Frame& f) noexcept {
    if(!s5.fxSprOpen)return;
    s5.fxSprOpen=false;
    const uint64_t frame=s5.nextFrameId+1;
    if(s5.fxPuffOpen&&s5.fxPuffN){s5.fxPuffPending=true;s5.fxPuffPendFrame=frame;s5.fxPuffLastMs=GetTickCount64();} // WORLD-CLOUDS wcdiag:1
    s5.fxPuffOpen=false;
    if(s5.fxTrailOpen) { // EFFECTS-OUTSIDE (M) fxtrail: end of the puff memory's frame (cull / expire) and the rate-limited fx-puff-trail line
        s5.fxTrailOpen=false;step5FxTrailDrop(s5,s5.fxTrailSelN?"trail-stale-selection":nullptr);
        const uint64_t now=GetTickCount64();
        const auto fst=s5.fxTrail.end_frame(s5.fxTrailGameOk?&s5.fxTrailGame:nullptr,s5.fxTrailFrame,now,s5.fxTrailOpt);
        s5.fxTrailWin.add(fst);++s5.fxTrailWinFrames;s5.fxTrailPendFrame=frame; // (M2) the worker's 1/s line takes the window (step5FxTrailTick)
    }
    if(const auto& sp=f.eff.sp;sp.on&&(sp.replays||sp.refused)) {
        ++s5.spriteFrames;if(sp.valid())++s5.spriteValidFrames;s5.spriteReplays+=sp.replays;s5.spriteCommitted+=sp.committed;s5.spriteRefused+=sp.refused;
        s5.spriteFallback+=f.eff.spFallback;if(sp.last_why)s5.spriteLastWhy=sp.last_why;
        if(f.eff.spAdditive) { // (P) fxadd:1: run totals of the additive family per eye, and the ADDITIVE SHOT eye-dump request (consumer: xr_png_once re-arm)
            s5.effAddPerEye+=f.eff.spAdditive;s5.effAddPtsPerEye+=f.eff.spAddPoints;++s5.effAddFrames;
            if(s5.effAddShot.step(f.eff.spAdditive,f.eff.spAddPoints,GetTickCount64())) {
                gpu_copy::step5::effects_additive_eye_request().store(true,std::memory_order_release);
                char b[160];std::snprintf(b,sizeof b,"fx-additive-shot;frame=%llu;additive=%u;points=%u;shot=%u;request=eye-dump(needs-xr_png_once)",
                    static_cast<unsigned long long>(frame),f.eff.spAdditive,f.eff.spAddPoints,s5.effAddShot.shots);
                (void)s5.diagQ.push_text(b);
            }
        }
        unsigned skipped=0;
        if(s5.spriteGate.admit(GetTickCount64(),skipped)){s5.spriteGpuPend=sp;s5.spriteGpuPendFallback=f.eff.spFallback;s5.spriteGpuPendExplosion=f.eff.spExplosion;
            s5.spriteGpuPendSprite=f.eff.spSprite;s5.spriteGpuPendLight=f.eff.spLight;s5.spriteGpuPendXpsoWhy=f.eff.spXpsoWhy;s5.spriteGpuPendFrame=frame;s5.spriteGpuPending=true;}
    }
    for(int c=0;c<3;++c) {
        const auto& p=s5.fxSpr[c];
        if(!p.draws)continue;
        ++s5.fxSprFrames[c];
        bool onset=false;
        if(c==1&&p.streaks){onset=!s5.fxSprStreakLast||frame-s5.fxSprStreakLast>30;s5.fxSprStreakLast=frame;}
        if(c==2){onset=!s5.fxSprTracerLast||frame-s5.fxSprTracerLast>30;s5.fxSprTracerLast=frame;} // (G) tracer: every onset after >= 30 quiet frames is admitted (lines up with the gun key and fxsig)
        unsigned skipped=0;
        if(!s5.fxSprGate[c].admit(GetTickCount64(),skipped)&&!onset)continue;
        if(s5.fxSprPending[c])skipped+=s5.fxSprPendSkipped[c]+1;
        s5.fxSprPend[c]=p;s5.fxSprPendFrame[c]=frame;s5.fxSprPendSkipped[c]=skipped;s5.fxSprPending[c]=true;
    }
}
// EFFECTS-OUTSIDE (M2) worker tick (step-1 mutex held), called on EVERY effects status tick: one fx-puff-trail line per second while fxtrail is set
// (life > 0), whatever the frames did (step139 null runs: the old line was written only when a puff was stored / emitted / refused, so a run that never
// stored one printed nothing and could not be told from a dead path). frames=0 + open=gpu:/ready: name a closed path.
void step5FxTrailTick(Step1State& st,Step5State& s5) noexcept {
    const bool active=step5FxTrailActive(); // (M4) fxtrail or fxcloud
    const uint64_t now=GetTickCount64();
    if(!acz::fxpuff::line_due(active,now,s5.fxTrailLineMs))return;
    s5.fxTrailLineMs=now;
    const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
    if(k>0&&size_t(k)<sizeof st.text) {
        acz::fxpuff::format_line(st.text+k,sizeof st.text-size_t(k),s5.fxTrailPendFrame,s5.fxTrailWinFrames,s5.fxTrailWin,s5.fxTrail.run,s5.fxTrail.live(),
            step5FxSpriteGpuOn(),s5.spriteReady,s5.fxTrailOpt.cls);
        step5Log(st,s5,false);
        // (M4) the companion fx-puff-mem line: the two kinds and the host / dedupe counters of the same window
        acz::fxpuff::format_mem_line(st.text+k,sizeof st.text-size_t(k),s5.fxTrailPendFrame,s5.fxTrailWin,s5.fxTrail.run,s5.fxTrail.live_of(acz::fxpuff::kSmoke),
            s5.fxTrail.live_of(acz::fxpuff::kCloud),s5.fxTrailOpt);
        step5Log(st,s5,false);
        // (M6) fxatlas: the fx-puff-atlas line (while the option is on, or once it was tried): tiles, captures, atlas draws, what is still undrawable
        const bool atlasOn=gpu_copy::step5::effects_puff_atlas_flag().load(std::memory_order_relaxed);
        if(atlasOn||s5.atlasTried) {
            namespace A=acz::fxpuff::atlas;
            A::LineIn li;li.frame=s5.fxTrailPendFrame;li.on=atlasOn;
            li.res=s5.atlasReady?"ready":s5.atlasTried?(s5.atlasWhy?s5.atlasWhy:"failed"):"pending(sprite-ring)";
            li.live=s5.fxTrail.atlas.live();li.ready=s5.fxTrail.atlas.ready_n();li.stuck=s5.fxTrail.atlas.stuck();li.qpin=s5.fxTrail.atlas.qpinned();
            li.w=s5.fxTrail.atlas.take();s5.fxAtlasRun.add(li.w);li.run=s5.fxAtlasRun;
            li.drawn=s5.fxTrailWin.adrawn;li.amiss=s5.fxTrailWin.amiss;li.unb=s5.fxTrailWin.unbound;li.want=s5.fxTrailWin.want;
            li.hostno=s5.fxAtlasHostNo;li.hwhy=s5.fxAtlasHostWhy;li.capno=s5.fxAtlasCapNo;li.cwhy=s5.fxAtlasCapWhy;
            li.run_drawn=s5.fxTrail.run.adrawn;li.run_unb=s5.fxTrail.run.unbound;li.run_want=s5.fxTrail.run.want;li.lay=s5.fxAtlasHostLayout;
            A::format_line(st.text+k,sizeof st.text-size_t(k),li);
            step5Log(st,s5,false);
            s5.fxAtlasHostNo=0;s5.fxAtlasCapNo=0;s5.fxAtlasHostWhy=nullptr;s5.fxAtlasCapWhy=nullptr;s5.fxAtlasHostLayout[0]=0;
            // review 2026-11-01 F2: the compute census (run totals; read only)
            A::format_compute_line(st.text+k,sizeof st.text-size_t(k),s5.fxTrailPendFrame,g_fxAtlasDispatchHook.load(std::memory_order_relaxed),
                g_fxAtlasGroups.load(std::memory_order_relaxed),g_fxAtlasDispatchAll.load(std::memory_order_relaxed),
                g_fxAtlasDispatchAfter.load(std::memory_order_relaxed),g_fxAtlasIndirectAfter.load(std::memory_order_relaxed),
                g_fxAtlasListsAfter.load(std::memory_order_relaxed));
            step5Log(st,s5,false);
            if(s5.fxAtlasLayoutPending) { // (M6b) once: the first host's root-signature layout, its verdict and its heaps
                s5.fxAtlasLayoutPending=false;
                A::format_host_line(st.text+k,sizeof st.text-size_t(k),s5.fxAtlasFirstLayout,s5.fxAtlasFirstWhy,s5.fxAtlasFirstHeaps,s5.fxAtlasFirstSampler);
                step5Log(st,s5,false);
            }
        }
    }
    s5.fxTrailWin=acz::fxpuff::Stats{};s5.fxTrailWinFrames=0;
}
// Worker (step-1 mutex held).
void step5FxSpriteEmit(Step1State& st,Step5State& s5) noexcept {
    if(s5.fxPuffPending) { // WORLD-CLOUDS wcdiag:1: one fx-cloud-puff line
        s5.fxPuffPending=false;
        const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
        if(k>0&&size_t(k)<sizeof st.text) {
            const double z[3]={0,0,0};
            acz::fxsprite::format_puff_line(st.text+k,sizeof st.text-size_t(k),s5.fxPuffPendFrame,s5.fxPuffEyeOk?s5.fxPuffEye:z,s5.fxPuff,s5.fxPuffN,s5.fxPuffSeen);
            step5Log(st,s5,false);
        }
    }
    if(s5.spriteGpuPending) { // fxmem:6: the frame's GPU replay facts + the run totals
        s5.spriteGpuPending=false;
        const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
        if(k>0&&size_t(k)<sizeof st.text) {
            acz::fxsprite::gpu::format_frame(st.text+k,sizeof st.text-size_t(k),s5.spriteGpuPendFrame,s5.spriteGpuPend,s5.spriteGpuPendFallback,
                s5.spriteGpuPendExplosion,s5.spriteGpuPendSprite,s5.spriteGpuPendLight,s5.spriteGpuPendXpsoWhy);
            step5Log(st,s5,false);
            using ull=unsigned long long;
            std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=fx-sprite-gpu-totals;frames=%llu;valid-frames=%llu;replays=%llu;committed=%llu;refused=%llu;"
                "window-fallback=%llu;last-why=%.28s",ull(s5.spriteFrames),ull(s5.spriteValidFrames),ull(s5.spriteReplays),ull(s5.spriteCommitted),ull(s5.spriteRefused),
                ull(s5.spriteFallback),s5.spriteLastWhy?s5.spriteLastWhy:"none");
            step5Log(st,s5,false);
        }
    }
    for(int c=0;c<3;++c) {
        if(!s5.fxSprPending[c])continue;
        s5.fxSprPending[c]=false;
        const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
        if(k<=0||size_t(k)>=sizeof st.text)return;
        acz::fxsprite::format_plan(st.text+k,sizeof st.text-size_t(k),s5.fxSprPendFrame[c],step5FxSpriteClassName(c),s5.fxSprPend[c],s5.fxSprPendSkipped[c]);
        step5Log(st,s5,false);
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=fx-sprite-totals;class=%s;frames=%llu;read=%llu;refused=%llu;caps=idx:%u,vtx:%u;"
            "mode=%s",step5FxSpriteClassName(c),static_cast<unsigned long long>(s5.fxSprFrames[c]),
            static_cast<unsigned long long>(s5.fxSprRead[c]),static_cast<unsigned long long>(s5.fxSprRefused[c]),acz::fxsprite::kMaxIndices,acz::fxsprite::kMaxVertices,
            step5FxSpriteGpuOn()&&s5.spriteReady?"plan+gpu-replay(per-eye,fx-sprite-gpu-lines)":"dry-run(cpu-only,no-eye-replay)");
        acz::fxsprite::append_nonindexed_refused(st.text,sizeof st.text,s5.fxSprPend[c]); // (K) the pending frame's refused non-indexed shape (absent when none)
        acz::fxsprite::append_points(st.text,sizeof st.text,s5.fxSprPend[c]); // (P) fxadd:1: the pending frame's point lists per eye (absent when none)
        acz::fxsprite::append_world_cloud_dropped(st.text,sizeof st.text,s5.fxSprPend[c]); // WORLD-CLOUDS: the pending frame's puff quads collapsed per eye (absent when none)
        step5Log(st,s5,false);
    }
}
// ------------------------------------------------------------- EFFECTS-OUTSIDE (F) fxmem:6: the per-eye GPU replay of the planned sprite draws (fx_sprite_gpu.h)
// The draw's eye vertex copies (plan_draw keep0/keep1, just computed by step5FxSpriteCapture) are recorded per eye on the GPU INSTEAD of the window effects
// replay of the same draw. Latched per frame at the effects arm (f.eff.sp.on) only when the resources exist; every refusal falls back to the window replay
// (exactly fxmem:5) and is counted (`window-fallback`).
// Everything one per-eye sprite replay needs, copied under the step-1 mutex; recorded after it is released together with the effects call (its clears,
// predicate, clone/game PSOs, the game's OM targets and draw arguments).
// (I) pso: the PSO of the eye draws (nullptr = the effects call's shared clone; the explosion's light PSO when fx_sprite_gpu.h explosion_pso_why passes).
struct Step5SpriteCall {D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{},dsv[2]{};bool clear=false,indexed=true;D3D12_VERTEX_BUFFER_VIEW eye[2]{},game{};INT base=0;ID3D12PipelineState* pso=nullptr;
    // (M) fxtrail: the re-emitted puffs of this draw's binding key, one TRIANGLELIST draw per eye from their eye copies, then the game's topology back
    bool trail=false;D3D12_VERTEX_BUFFER_VIEW trailVb[2]{};UINT trailVerts[2]{};UINT gameTopology=0;
    // (M4) one draw per key group; a group of another key re-binds root parameter trailParam to that key, the host key is bound back after the groups
    acz::fxpuff::Group trailGroups[2][acz::fxpuff::kMaxKeys]{};unsigned trailGroupN[2]{};UINT trailParam=UINT32_MAX;uint64_t trailHostKey=0;
    // (M6) fxatlas: capN capture quads (one TRIANGLELIST draw into the atlas under the copy PSO, the game's root signature and bound table = the key), recorded
    // before the replay, unpredicated; then the game's viewport / scissor / topology back (the replay binds its own PSO / OM / VB and sets the game's back)
    unsigned capN=0;ID3D12PipelineState* capPso=nullptr;D3D12_VERTEX_BUFFER_VIEW capVb{};ID3D12Resource* atlas=nullptr;D3D12_CPU_DESCRIPTOR_HANDLE atlasRtv{};
    D3D12_VIEWPORT gameVp{};D3D12_RECT gameSc{};UINT capTopology=0;
    Step5EyeRs rs{}; // EYE-RING (eye_ring.h): the eye viewport/scissor of the eye draws and the game's to set back (off: rs.on = false, no call)
    // (M6) the atlas group (atlas::kGroupKey): our shader-visible heap + SRV table, then the game's heaps and the host key back
    // (M6b) with every table of the root signature and the game's sampler heap (fx_puff_atlas_d3d12.h GroupBind / record_group)
    bool atlasDraw=false;acz::fxpuff::atlas::GroupBind group{};};
// Step-1 mutex held, right after the effects layer accepted a sprite-class draw (`ecall` filled) and step5FxSpriteCapture planned the same draw. true =
// record the per-eye replay INSTEAD of the window effects draw (out filled, the eye copies written into this slot's sprite UPLOAD span).
// (M6) ah: the draw's atlas facts (step5FxAtlasHost_locked; nullptr = fxatlas off).
__declspec(noinline) bool step5FxSpriteGpuPlan_locked(Step1State& st,Step5State& s5,Step5Frame& f,const gpu_copy::step3::Shape& d,const Step5EffCall& ecall,
                                                      Step5SpriteCall& out,ID3D12PipelineState* light=nullptr,const Step5AtlasHost* ah=nullptr) noexcept {
    namespace g=acz::fxsprite::gpu;auto& sp=f.eff.sp;
    if(!sp.on)return false;
    const auto& sl=s5.slot[f.slot];
    g::GateIn in;
    in.ready=s5.spriteReady&&sl.spriteRt[0]&&sl.spriteRt[1]&&s5.spriteUploadPtr&&s5.spriteUploadVa;
    in.effects_recorded=true;in.dual=ecall.clone&&ecall.game;
    in.prepared=s5.fxSprLastPrepared;in.vertices=s5.fxSprLastN;in.draw_index=sp.replays;
    in.span_used=f.eff.spBytes;in.explosion=s5.fxSprLastClass==0; // (H) packed span; the explosion class may use the reserve
    in.indexed=d.indexed&&ecall.indexed;in.allow_nonindexed=gpu_copy::step5::effects_sprite_nonindexed_class(s5.fxSprLastClass)&&!d.indexed&&!ecall.indexed;in.ib_stride=d.ib_stride;in.vb_stride=d.vb_stride;in.instances=d.instances;in.vb_va=d.vb_va;in.vb_bytes=d.vb_bytes;
    in.eye_depth=st.s3.targets.has_depth&&sl.ds[0]&&sl.ds[1];in.eye_dsv=s5.ringDsv;
    in.pso_dsv=d.pso.dsv;in.pso_rtv0=d.pso.rtv[0];in.pso_rt_count=d.pso.rt_count;in.image_format=s5.hudFormat;in.pso_samples=d.pso.samples;
    in.depth_enable=d.pso.depth_enable;in.depth_write=d.pso.depth_write;in.stencil=d.pso.stencil;in.depth_func=d.pso.depth_func;
    in.stencil_noop=d.pso.front_func==gpu_copy::step3::cmp::always&&d.pso.back_func==gpu_copy::step3::cmp::always&&(d.pso.stencil_write==0||d.pso.stencil_ops_keep);
    in.vp_known=d.rs_known&&!d.rs_gap;in.viewports=d.viewports;in.vp[0]=d.vp0.x;in.vp[1]=d.vp0.y;in.vp[2]=d.vp0.w;in.vp[3]=d.vp0.h;
    in.image_w=s5.gameW;in.image_h=s5.gameH;in.scissors=d.scissors;in.sc[0]=d.sc0.l;in.sc[1]=d.sc0.t;in.sc[2]=d.sc0.r;in.sc[3]=d.sc0.b; // EYE-RING: the GAME image
    const char* why=g::gate(in);
    if(!why)why=step5EyeRsNote(s5,step5EyeRsPlan(s5,d,out.rs)); // EYE-RING: nullptr and rs.on = false without it
    // (P2) an additive draw needs its per-eye light PSO: the shared clone it would otherwise bind is the WINDOW one, which draws nothing (gpu_copy_step5.h).
    if(!why)why=gpu_copy::step5::effects_additive_pso_why(d.pso.additive_ready,light!=nullptr);
    const uint64_t span=uint64_t(f.slot)*g::kSlotStride;
    g::EyeViews v;
    if(!why&&!g::eye_views(s5.spriteUploadVa+span,f.eff.spBytes,s5.fxSprLastN,s5.fxSprLastLo,v))why="sprite-eye-view";
    if(why){sp.note_refused(why);++f.eff.spFallback;step5FxTrailDrop(s5,"trail-window-fallback");s5.fxTrail.seen_n_=0;return false;}
    // The eye copies into THIS slot's span: the slot is PRODUCING (not held, not owned by another live recording, its last GPU use completed: step5PickSlot_locked).
    // (H) packed: this replay owns [cursor, cursor + v.used) of the span; the cursor only grows within the frame (no overwrite within a recording).
    const uint64_t cursor=f.eff.spBytes;
    for(unsigned e=0;e<2;++e)std::memcpy(s5.spriteUploadPtr+span+g::offset(cursor,s5.fxSprLastN,e),s5.fxSprKeep[e],size_t(s5.fxSprLastN)*acz::fxsprite::kVertexStride);
    f.eff.spBytes=cursor+v.used;
    std::atomic_thread_fence(std::memory_order_release);
    for(unsigned e=0;e<2;++e){out.rtv[e]=sl.spriteRtv[e];out.dsv[e]=sl.dsv[e];out.eye[e]={v.va[e],v.bytes,v.stride};}
    out.game={d.vb_va,UINT(d.vb_bytes),d.vb_stride};out.base=v.base;out.indexed=d.indexed; // non-indexed (tracer first bullet): DrawInstanced(count, 1, 0, startInstance) from the eye copy
    out.clear=!sp.cleared;sp.cleared=true;
    sp.note_replay(ecall.predRes!=nullptr,ecall.predOffset);
    if(s5.fxTrailSelN) { // EFFECTS-OUTSIDE (M) fxtrail: the selected puffs' eye copies after this replay's copies (sprite / tracer share of the span only)
        const char* tw=nullptr;
        if(s5.fxSprLastClass!=1)tw="trail-not-class-1";
        else if(d.topology!=4&&d.topology!=5)tw="trail-topology"; // the clone PSO is a TRIANGLE pipeline: TRIANGLELIST is drawable, the game's strip / list is restored
        g::TrailTake tk;   // (N) the cache's own region behind the game's span: its bytes are not in f.eff.spBytes, so the game's replays never see them
        if(!tw)tw=g::trail_take(f.eff.trBytes,unsigned(s5.fxTrailVerts[0]),unsigned(s5.fxTrailVerts[1]),tk);
        if(tw)step5FxTrailDrop(s5,tw);
        else {
            const uint64_t* at=tk.at;
            for(unsigned e=0;e<2;++e) {
                if(s5.fxTrailVerts[e])std::memcpy(s5.spriteUploadPtr+span+at[e],s5.fxTrailEye[e],size_t(s5.fxTrailVerts[e])*acz::fxsprite::kVertexStride);
                out.trailVb[e]={s5.spriteUploadVa+span+at[e],UINT(s5.fxTrailVerts[e])*acz::fxsprite::kVertexStride,acz::fxsprite::kVertexStride};
                out.trailVerts[e]=s5.fxTrailVerts[e];
            }
            out.trail=true;out.gameTopology=d.topology;f.eff.trBytes+=tk.used;
            out.trailParam=s5.fxTrailParam;out.trailHostKey=s5.fxTrailSelKey;
            // (M6) the atlas group (select admitted it only under an atlas-capable host: ah->why == nullptr, the atlas ready): our heap + table, the game's back
            unsigned atlasTables=0;
            if(ah&&!ah->why&&s5.atlasReady) {
                auto& gb=out.group;gb.ours=s5.atlasSrvHeap;gb.our_srv=s5.atlasSrv;gb.ngame=ah->ngame;for(unsigned k=0;k<ah->ngame;++k)gb.game[k]=ah->game[k];
                gb.game_sampler=ah->sampler;gb.key_param=s5.fxTrailParam;gb.host_key=s5.fxTrailSelKey;gb.ntables=ah->ntables;
                for(unsigned k=0;k<ah->ntables;++k){gb.tparam[k]=ah->tparam[k];gb.tvalue[k]=ah->tvalue[k];}
                const char* gw=acz::fxpuff::atlas::group_why(gb);
                out.atlasDraw=gw==nullptr;atlasTables=gb.ntables;
                if(gw){++s5.fxAtlasHostNo;s5.fxAtlasHostWhy=gw;}
            }
            for(unsigned e=0;e<2;++e) { // (M4) per eye: VB + topology + one draw per group (+ one table bind per foreign group, + the host's back) + topology back
                out.trailGroupN[e]=s5.fxTrailGroupN[e];unsigned foreign=0,atl=0;
                for(unsigned gi=0;gi<s5.fxTrailGroupN[e];++gi) {
                    out.trailGroups[e][gi]=s5.fxTrailGroups[e][gi];
                    if(s5.fxTrailGroups[e][gi].key==acz::fxpuff::atlas::kGroupKey)++atl;else foreign+=s5.fxTrailGroups[e][gi].key!=s5.fxTrailSelKey;
                }
                if(s5.fxTrailVerts[e])f.eff.commands+=3u+(s5.fxTrailGroupN[e]?s5.fxTrailGroupN[e]:1u)+foreign+(foreign?1u:0u)+atl*(acz::fxpuff::atlas::group_commands(atlasTables)-1u);
            }
            std::atomic_thread_fence(std::memory_order_release);
            s5.fxTrailSelN=0;s5.fxTrailVerts[0]=s5.fxTrailVerts[1]=0; // taken: emitted this frame (emit_frame stays set)
        }
    }
    // (M6) fxatlas: the cards this draw sighted (observe: stored / refreshed / renewed under ITS key, which it binds) get atlas tiles; a tile without a
    // capture recorded yet gets one now, recorded before this draw's replay. Needs: a class-1 draw, the atlas ready, this game PSO's capture pipeline made on
    // the ring's device, the frame's capture cap. Otherwise nothing is requested (the cards keep the M5 key path and are asked again at their next sighting).
    {
        namespace A=acz::fxpuff::atlas;
        const char* cw=nullptr;
        if(!s5.fxTrailOpt.atlas||!s5.fxTrail.seen_n_)cw="";
        else if(!s5.atlasReady||!s5.atlasUploadPtr)cw="atlas-not-ready";
        else if(s5.fxSprLastClass!=1)cw="atlas-not-class-1";
        else if(!ah||!ah->copy)cw="atlas-no-copy-pso";
        else if(ah->copyDevice!=uintptr_t(st.device))cw="atlas-device";
        else if(f.eff.acCaps>=A::kCapPerFrame)cw="atlas-frame-cap";
        else if(!d.rs_known||d.rs_gap||d.viewports!=1||d.scissors!=1)cw="atlas-rs-unknown"; // the game's viewport / scissor are set back exactly
        if(!cw) {
            const unsigned room=std::min(A::kCapPerDraw,A::kCapPerFrame-f.eff.acCaps);
            uint16_t tiles[A::kCapPerDraw];A::Rect rects[A::kCapPerDraw];
            const unsigned n=s5.fxTrail.plan_captures(s5.fxTrailFrame,s5.fxTrailOpt,tiles,rects,room);
            if(n) {
                const uint64_t base=uint64_t(f.slot)*A::kCopyRegion+f.eff.acBytes;
                for(unsigned k=0;k<n;++k){A::CopyVtx q[A::kCopyVerts];A::copy_quad(tiles[k],rects[k],q);std::memcpy(s5.atlasUploadPtr+base+k*A::kCopyBytes,q,sizeof q);}
                std::atomic_thread_fence(std::memory_order_release);
                out.capN=n;out.capPso=ah->copy;out.atlas=s5.atlasTex;out.atlasRtv=s5.atlasRtv;
                out.capVb={s5.atlasUploadVa+base,UINT(n*A::kCopyBytes),A::kCopyStride};
                out.gameVp={d.vp0.x,d.vp0.y,d.vp0.w,d.vp0.h,d.vp0.min,d.vp0.max};out.gameSc={LONG(d.sc0.l),LONG(d.sc0.t),LONG(d.sc0.r),LONG(d.sc0.b)};
                out.capTopology=d.topology;
                f.eff.acBytes+=n*A::kCopyBytes;f.eff.acCaps+=n;f.eff.commands+=A::kCaptureCommands;
            }
        } else {
            s5.fxTrail.seen_n_=0;
            if(*cw){++s5.fxAtlasCapNo;s5.fxAtlasCapWhy=cw;}
        }
    }
    if(s5.fxSprLastClass==0) {   // (I) the explosion's own per-eye PSO (A' = A(1+a) - a); else the shared clone, named
        ++f.eff.spExplosion;
        if(const char* xw=g::explosion_pso_why(true,light!=nullptr,s5.hudFormat))f.eff.spXpsoWhy=xw;else{out.pso=light;++f.eff.spLight;}
    } else ++f.eff.spSprite;
    if(d.pso.additive_ready){++f.eff.spAdditive;if(!d.indexed&&d.topology==1)++f.eff.spAddPoints; // (P) fxadd: the additive family per eye (dual_ready is false for it)
        out.pso=light;}   // (P2) the additive light PSO (RGB 5,2,1, alpha ZERO/ONE); the shared clone is the window's no-op (checked non-null above)
    f.eff.commands+=g::kCmdsPerDraw-(gpu_copy::step5::effects_per_draw+2)+(out.clear?g::kCmdsClear:0u); // the effects call already counted 5 + 2 (dual)
    if(out.rs.on){f.eff.commands+=::acz::eyering::kExtraCommands;++s5.eyeFxReplays;++s5.eyeVpSub;++s5.eyeScSub;++s5.eyeVpRestore;} // EYE-RING
    return true;
}
// The effects call's once-per-frame clears, [the two eye sprite image clears], SetPredication(the effects epoch's word), then fx_sprite_gpu.h replay_commands
// (clone PSO, per eye OMSet(sprite image, eye objects DSV) + IASetVertexBuffers(0, eye copy) + DrawIndexedInstanced(base = -lo), the game's VB view, game PSO),
// SetPredication(null), OMSet(game targets). The game's own draw follows, forwarded by the caller, under exactly its own state. Native vtable calls only.
__declspec(noinline) void step5RecordSprite(ID3D12GraphicsCommandList* self,const Step5EffCall& c,const Step5SpriteCall& s) noexcept {
    namespace g=acz::fxsprite::gpu;
    using ClearRtvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT*,UINT,const D3D12_RECT*);
    using ClearDsvFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*);
    using SetPsoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*);
    using VbFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*);
    const auto om=reinterpret_cast<Step5OmFn>(step1Native(self,46));
    const auto pred=reinterpret_cast<Step5PredFn>(step1Native(self,55));
    const auto vb=reinterpret_cast<VbFn>(step1Native(self,44));
    static constexpr FLOAT transparent[4]={0,0,0,0};
    if(c.clear)reinterpret_cast<ClearRtvFn>(step1Native(self,48))(self,c.rtv,transparent,0,nullptr);
    if(c.clearDs)reinterpret_cast<ClearDsvFn>(step1Native(self,47))(self,c.dsv,c.dsFlags,0.0f,0,0,nullptr);
    if(s.clear)for(unsigned e=0;e<2;++e)reinterpret_cast<ClearRtvFn>(step1Native(self,48))(self,s.rtv[e],transparent,0,nullptr);
    using TopoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY);
    if(s.capN&&s.capPso&&s.atlas) {   // (M6) fxatlas: the captures (unpredicated: the key is bound by the game's draw that follows, whatever the effects word)
        namespace A=acz::fxpuff::atlas;
        using VpFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*);
        using ScFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*);
        const auto barrier=reinterpret_cast<Step5BarrierFn>(step1Native(self,26));
        const auto topo=reinterpret_cast<TopoFn>(step1Native(self,20));
        const D3D12_RESOURCE_BARRIER toRt=A::transition(s.atlas,true),back=A::transition(s.atlas,false);   // fx_puff_atlas_d3d12.h (WARP-tested)
        barrier(self,1,&toRt);
        reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,s.capPso);   // same root signature as the bound one: the game's root arguments stay
        om(self,1,&s.atlasRtv,FALSE,nullptr);
        const D3D12_VIEWPORT av=A::viewport();const D3D12_RECT ar=A::scissor();
        reinterpret_cast<VpFn>(step1Native(self,21))(self,1,&av);reinterpret_cast<ScFn>(step1Native(self,22))(self,1,&ar);
        topo(self,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        vb(self,0,1,&s.capVb);
        reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,s.capN*A::kCopyVerts,1,0,0);
        barrier(self,1,&back);
        reinterpret_cast<VpFn>(step1Native(self,21))(self,1,&s.gameVp);reinterpret_cast<ScFn>(step1Native(self,22))(self,1,&s.gameSc);
        topo(self,D3D12_PRIMITIVE_TOPOLOGY(s.capTopology));
        // the PSO, OM and slot-0 VB are bound by the replay below and set back to the game's at its end (vb_game, set_pso_game, om_restore)
    }
    if(c.predRes)pred(self,c.predRes,c.predOffset,c.predOp);
    step5EyeRsSet(self,s.rs,true); // EYE-RING: after the atlas captures (which set the game's viewport back); the list's om_restore sets the game's back again
    g::Cmd cmds[g::kMaxCmds];const unsigned n=g::replay_commands(cmds);
    for(unsigned i=0;i<n;++i) {
        const auto& k=cmds[i];
        switch(k.kind) {
        case g::CmdKind::set_pso_clone:reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,s.pso?s.pso:c.clone);break; // (I) the explosion's light PSO when chosen
        case g::CmdKind::om_eye:om(self,1,&s.rtv[k.eye],FALSE,&s.dsv[k.eye]);break;
        case g::CmdKind::vb_eye:vb(self,0,1,&s.eye[k.eye]);break;
        case g::CmdKind::draw_eye:
            if(s.indexed)reinterpret_cast<Step3DrawIndexedFn>(step1Native(self,13))(self,c.count,c.instances,c.start,s.base,c.startInstance);
            else reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,c.count,c.instances,0,c.startInstance);   // the eye copy holds vertices start..start+count-1 at 0..count-1
            if(s.trail&&s.trailVerts[k.eye]) { // (M) fxtrail / (M4) fxcloud: the stored cards, same PSO / OM / root arguments, own vertices, TRIANGLELIST
                using TopoFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY);
                using TableFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE);
                const auto topo=reinterpret_cast<TopoFn>(step1Native(self,20));
                const auto table=reinterpret_cast<TableFn>(step1Native(self,32)); // SetGraphicsRootDescriptorTable
                vb(self,0,1,&s.trailVb[k.eye]);
                topo(self,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                const unsigned ng=s.trailGroupN[k.eye];
                if(!ng)reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,s.trailVerts[k.eye],1,0,c.startInstance);
                bool rebound=false;
                for(unsigned gi=0;gi<ng;++gi) {
                    const auto& tg=s.trailGroups[k.eye][gi]; // (never `g`: the gpu namespace alias of this function)
                    if(!tg.count)continue;
                    if(tg.key==acz::fxpuff::atlas::kGroupKey) { // (M6) fxatlas: our heap + SRV table (the atlas), then the game's heaps and the host key back
                        if(!s.atlasDraw||s.trailParam==UINT32_MAX)continue;
                        using HeapsFn=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,ID3D12DescriptorHeap* const*);
                        const auto heaps=reinterpret_cast<HeapsFn>(step1Native(self,28)); // SetDescriptorHeaps
                        const auto drawFn=reinterpret_cast<Step3DrawFn>(step1Native(self,12));
                        // (M6b) fx_puff_atlas_d3d12.h record_group (the WARP harness runs the same sequence): ours [+ the game's sampler heap], the sampler
                        // tables again, the key table = our SRV, the draw, the game's heaps, EVERY table back (the key = the host's own key)
                        acz::fxpuff::atlas::record_group(s.group,[&](UINT n,ID3D12DescriptorHeap* const* hs){heaps(self,n,hs);},
                            [&](UINT p,D3D12_GPU_DESCRIPTOR_HANDLE h){table(self,p,h);},[&]{drawFn(self,tg.count,1,tg.first,c.startInstance);});
                        fxAtlasGroupRecorded(self); // review 2026-11-01 F2: census of compute work recorded after it in this list (read only)
                        rebound=false;
                        continue;
                    }
                    if(tg.key!=s.trailHostKey) { // (M4) xkey: select lets a foreign key through only with a known parameter, the same PSO and heaps, the key bound by a game draw earlier in THIS frame (F1)
                        if(s.trailParam==UINT32_MAX)continue;
                        D3D12_GPU_DESCRIPTOR_HANDLE h{};h.ptr=tg.key;table(self,s.trailParam,h);rebound=true;
                    } else if(rebound){D3D12_GPU_DESCRIPTOR_HANDLE h{};h.ptr=s.trailHostKey;table(self,s.trailParam,h);rebound=false;}
                    reinterpret_cast<Step3DrawFn>(step1Native(self,12))(self,tg.count,1,tg.first,c.startInstance);
                }
                if(rebound){D3D12_GPU_DESCRIPTOR_HANDLE h{};h.ptr=s.trailHostKey;table(self,s.trailParam,h);} // the game's own table back before its draw
                topo(self,D3D12_PRIMITIVE_TOPOLOGY(s.gameTopology)); // the game's topology back (setter-tracked: bindings.topology)
            }
            break;
        case g::CmdKind::vb_game:vb(self,0,1,&s.game);break;
        case g::CmdKind::set_pso_game:reinterpret_cast<SetPsoFn>(step1Native(self,25))(self,c.game);break;
        case g::CmdKind::om_restore:
            if(c.predRes)pred(self,nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO); // only after our own SetPredication (the effects gate proved the game's off)
            step5EyeRsSet(self,s.rs,false); // EYE-RING: the game's viewport/scissor back before its own draw (off: no call)
            om(self,c.omCount,c.omCount?c.omRtv.data():nullptr,c.omSingle,c.omDsv?&c.omDsvHandle:nullptr);break;
        }
    }
}
// At the list's Close (step-1 mutex held): the predicate words are final; count the replays the GPU executes.
inline void step5FxSpriteGpuClose(Step5State& s5,Step5Frame& f) noexcept {
    if(f.eff.armed&&f.eff.sp.on)f.eff.sp.close(s5.effPredPtr,s5.effPredMode);
}
