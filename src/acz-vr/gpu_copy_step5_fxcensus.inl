// gpu_copy_step5_fxcensus.inl -- EFFECTS-OUTSIDE (D) fxmem:3: census of the effects seam's draws to identify the game's EXPLOSION and GUN-TRACER pipelines
// (fx_census.h; docs/world-render/effects-outside-window.md section 9). Included by gpu_copy_step5_hooks.inl inside its anonymous namespace, after Step5State.
// LOG ONLY: no game memory is read (the Shape, the PSO's tracked blend tuple and the root layout are the proxy's own copies), nothing is recorded into a
// command list and no replay decision changes. Flag off (default): every function returns at once; the only statement executed per candidate is the
// gate-verdict pointer store in step5EffectsConsider_locked.
inline bool step5FxCensusOn() noexcept {return gpu_copy::step5::effects_census_flag().load(std::memory_order_relaxed);}
// Hook-state mutex held: the bound root layout's shape (parameter kinds/visibility/registers), never a root value.
__declspec(noinline) void step5FxCensusRoots_locked(const ListState& ls,acz::fxcensus::RootShape& r) noexcept {
    r=acz::fxcensus::RootShape{};
    const auto& b=ls.bindings;
    if(!b.root||!b.root->valid())return;
    for(const auto& p:b.root->layout().parameters)r.add(uint32_t(p.kind),uint32_t(p.visibility),p.shader_register,p.space,p.constant_count);
}
// Step-1 mutex held; one draw the effects seam saw while the frame's effects layer is armed (after step5EffectsConsider_locked, whose class-gate verdict
// is in s5.fxCenGate). ACM (world objects) and cockpit members never get here.
__declspec(noinline) void step5FxCensusDraw(Step5State& s5,const gpu_copy::step3::Shape& d,const Step5EffId& id,const acz::fxcensus::RootShape& r) noexcept {
    if(!s5.fxCen.open())return;
    acz::fxcensus::DrawFacts x;
    x.ps8=id.ps8;x.vs8=id.vs8;std::memcpy(x.blend,id.blend,sizeof x.blend);
    x.pso_known=d.pso.known;x.write_mask=uint8_t(d.pso.write_mask0);x.blend_on=d.pso.blend0;x.depth_enable=d.pso.depth_enable;x.depth_write=d.pso.depth_write;
    x.stencil=d.pso.stencil;x.depth_func=d.pso.depth_func;x.topology=d.topology;x.indexed=d.indexed;x.count=d.count;x.instances=d.instances;x.vb_stride=d.vb_stride;
    x.root_known=r.known;x.root_n=r.n;x.root_c=r.c;x.root_v=r.v;x.root_t=r.t;x.root_hash=r.hash;
    x.gate=s5.fxCenGate;
    s5.fxCen.add(x);
    s5.fxCenGate="not-scene-target"; // the next draw's default when step5EffectsConsider_locked returns before the class gate
}
// Frame start (step5EffectsArm_locked, armed frames only).
inline void step5FxCensusBegin(Step5State& s5) noexcept {if(step5FxCensusOn()){s5.fxCen.begin_frame();s5.fxCenGate="not-scene-target";}}
// Per published effects-armed frame (step-1 mutex held).
inline void step5FxCensusAccount(Step5State& s5) noexcept {
    if(!s5.fxCen.open())return;
    if(s5.fxCen.end_frame(s5.nextFrameId+1,GetTickCount64()))s5.fxCenWindow=true;
}
// Worker (step-1 mutex held): onsets first (their frame ids are exact), then the closed window's summary and signature lines.
void step5FxCensusEmit(Step1State& st,Step5State& s5) noexcept {
    auto& c=s5.fxCen;
    const int k=std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=");
    if(k<=0||size_t(k)>=sizeof st.text)return;
    for(unsigned i=0;i<c.onset_count();++i){acz::fxcensus::format_onset(st.text+k,sizeof st.text-size_t(k),c,c.onset(i));step5Log(st,s5,false);}
    c.clear_onsets();
    if(!s5.fxCenWindow)return;
    s5.fxCenWindow=false;
    acz::fxcensus::format_window(st.text+k,sizeof st.text-size_t(k),c.summary());step5Log(st,s5,false);
    for(unsigned i=0;i<c.report_count();++i){acz::fxcensus::format_sig(st.text+k,sizeof st.text-size_t(k),c.summary().frame,c.report(i));step5Log(st,s5,false);}
}
// ------------------------------------------------------------- EFFECTS-OUTSIDE (D) fxsig:<ps8>: per-draw log of ONE pixel-shader prefix (docs section 12)
// Active whenever effects_sig_filter() is non-zero (xr_terrain_stage fxsig:, any fxmem >= 1: the effects seam runs for fxmem:3/5/6 alike); independent of the census
// (fxmem:3) and of the 12-line window cap. Reads the first vertices of the draw through the same pinned-mapped-UPLOAD reader as the sprite capture; every failure is a
// named reason in the line (fail closed: nothing else changes, the draw is forwarded and replayed exactly as without the option).
inline uint32_t step5FxSigFilter() noexcept {return gpu_copy::step5::effects_sig_filter().load(std::memory_order_relaxed);}
// Hook-state mutex held (the identity block): the bound root's parameters, "<param>:<K><reg>=<value>" separated by ',' (K: C root constants (first 2 words, '-' =
// unwritten), V root CBV, S root SRV, U root UAV, T descriptor table), at most 6 parameters, at most `n` bytes. This is as much of the "world/view matrix slot" as
// the root tracking exposes: the tracer VS (ps2AtestCA_v) takes pre-projected vertices, so a matrix slot is expected to be absent; the line shows what is bound.
__declspec(noinline) void step5FxSigRoots_locked(const ListState& ls,char* out,size_t n) noexcept {
    if(!out||!n)return;
    out[0]=0;
    const auto& b=ls.bindings;
    if(!b.root||!b.root->valid()){std::snprintf(out,n,"root-layout-unknown");return;}
    size_t len=0;unsigned shown=0;
    const auto& L=b.root->layout();
    for(unsigned i=0;i<L.parameters.size()&&shown<6;++i) {
        const auto& p=L.parameters[i];
        const auto it=b.roots.find(i);
        char t[48];int r=0;
        const char kc=p.kind==acz::metadata::ParameterKind::constants?'C':p.kind==acz::metadata::ParameterKind::cbv?'V':p.kind==acz::metadata::ParameterKind::srv?'S':
            p.kind==acz::metadata::ParameterKind::uav?'U':'T';
        if(it==b.roots.end())r=std::snprintf(t,sizeof t,"%s%u:%c%u=unset",shown?",":"",i,kc,p.shader_register);
        else if(kc=='C') {
            const auto& c=it->second.constants;const auto& w=it->second.written;
            char w0[12]="-",w1[12]="-";
            if(c.size()>0&&w.size()>0&&w[0])std::snprintf(w0,sizeof w0,"%x",c[0]);
            if(c.size()>1&&w.size()>1&&w[1])std::snprintf(w1,sizeof w1,"%x",c[1]);
            r=std::snprintf(t,sizeof t,"%s%u:C%u[%u]=%s/%s",shown?",":"",i,p.shader_register,p.constant_count,w0,w1);
        } else r=std::snprintf(t,sizeof t,"%s%u:%c%u=%llx",shown?",":"",i,kc,p.shader_register,static_cast<unsigned long long>(it->second.value));
        if(r<=0)continue;
        const size_t w=size_t(r)<sizeof t?size_t(r):sizeof t-1;
        if(len+w+1>n)break;
        std::memcpy(out+len,t,w);len+=w;out[len]=0;++shown;
    }
    if(!shown)std::snprintf(out,n,"no-params");
}
// Step-1 mutex held; ONE draw the effects seam saw (called BEFORE step5FxCensusDraw: it reads and resets the class-gate verdict). `roots` = step5FxSigRoots_locked's
// text (empty when the draw is not the filtered signature).
__declspec(noinline) void step5FxSigDraw(Step5State& s5,const gpu_copy::step3::Shape& d,const Step5EffId& id,const char* roots) noexcept {
    const char* gate=s5.fxCenGate;
    if(!s5.fxCen.open())s5.fxCenGate="not-scene-target"; // the census draw (fxmem:3) does this reset itself; with it closed nobody else would
    const uint32_t want=step5FxSigFilter();
    if(!want||id.ps8!=want)return;
    namespace fc=acz::fxcensus;
    const uint64_t frame=s5.nextFrameId+1;unsigned nth=0;
    if(!s5.fxSigLog.admit(frame,GetTickCount64(),nth))return;
    fc::SigDraw x;
    x.frame=frame;x.ord=d.draw_ordinal;x.nth=nth;x.ps8=id.ps8;x.vs8=id.vs8;x.indexed=d.indexed;x.count=d.count;x.instances=d.instances;x.start=d.start;x.base=d.base;
    x.vb_va=d.vb_va;x.vb_res=d.vb_address;x.vb_bytes=d.vb_bytes;x.ib_va=d.ib_va;x.vb_stride=d.vb_stride;x.ib_stride=d.ib_stride;x.topology=d.topology;
    std::memcpy(x.blend,id.blend,sizeof x.blend);x.write_mask=uint8_t(d.pso.write_mask0);x.pso_known=d.pso.known;x.blend_on=d.pso.blend0;
    x.depth_enable=d.pso.depth_enable;x.depth_write=d.pso.depth_write;x.stencil=d.pso.stencil;x.depth_func=d.pso.depth_func;x.tracer_ready=d.pso.tracer_ready;
    x.gate=gate;
    if(roots)std::snprintf(x.roots,sizeof x.roots,"%s",roots);
    // the first vertices (position float4 only, 16 bytes each; any stride >= 16): indexed draws read the first <= 4 indices, non-indexed ones start..start+3
    const char* why=nullptr;
    if(!d.vb_va||!d.vb_bytes||!d.vb_address)why="vb-unbound";
    else if(d.vb_stride<16||d.vb_stride>4096)why="vb-stride";
    else if(!d.count)why="no-count";
    else if(d.indexed&&(d.ib_stride!=2&&d.ib_stride!=4))why="ib-stride";
    else if(d.indexed&&(!d.ib_va||!d.ib_bytes||!d.ib_address))why="ib-unbound";
    if(!why) {
        auto& s=state();std::lock_guard lock(s.mutex);
        const uint8_t* vb=nullptr;const uint8_t* ib=nullptr;
        const unsigned nq=d.count<4?d.count:4u;
        int64_t vi[4]={};unsigned nv=0;
        if(d.indexed) {
            const uint64_t ibOff=uint64_t(d.start)*d.ib_stride;
            if(ibOff+uint64_t(nq)*d.ib_stride>d.ib_bytes)why="ib-outside-view";
            else if(const char* bad=step3MappedSpan_locked(d.ib_address,d.ib_va+ibOff,uint64_t(nq)*d.ib_stride,ib))why=bad;
            else for(unsigned i=0;i<nq;++i) {
                uint32_t v=0;
                if(!guardedCpuRead(ib+uint64_t(i)*d.ib_stride,&v,d.ib_stride)){why="ib-read-fault";break;}   // little-endian: a 16-bit index fills the low half
                x.idx[x.nidx++]=v;vi[nv++]=int64_t(v)+d.base;
            }
        } else for(unsigned i=0;i<nq;++i)vi[nv++]=int64_t(d.start)+int64_t(i);
        if(!why)if(const char* bad=step3MappedSpan_locked(d.vb_address,d.vb_va,d.vb_bytes,vb))why=bad;
        if(!why)for(unsigned i=0;i<nv;++i) {
            if(vi[i]<0||(uint64_t(vi[i])*d.vb_stride)+16>d.vb_bytes){why=x.nvtx?"vertex-outside-view-partial":"vertex-outside-view";break;}
            if(!guardedCpuRead(vb+uint64_t(vi[i])*d.vb_stride,x.pos[i],16)){why="vertex-read-fault";break;}
            ++x.nvtx;
        }
    }
    x.read_why=why;
    s5.fxSigQ.push(x);
}
// Worker (step-1 mutex held): the queued fx-sig-draw lines (a line's own `dropped=` field reports lines lost to a full queue since the previous line).
void step5FxSigEmit(Step1State& st,Step5State& s5) noexcept {
    while(const char* l=s5.fxSigQ.front()) {
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=%s",l);
        s5.fxSigQ.pop();
        step5Log(st,s5,false);
    }
}
