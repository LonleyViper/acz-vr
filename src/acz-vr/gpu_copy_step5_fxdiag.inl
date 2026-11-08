// gpu_copy_step5_fxdiag.inl -- READ-ONLY flare / aircraft diagnostics (fx_diag_flare_air.h). Included by gpu_copy_step5_hooks.inl inside its anonymous namespace,
// after gpu_copy_step5_fxcensus.inl (it uses step5FxSigFilter, step3MappedSpan_locked and guardedCpuRead). Armed only by the reserved fxsig sentinels
// (kArmFlare / kArmAir); default: one relaxed load per HUD draw / per draw. Nothing here is recorded into a command list or changes a replay decision.
inline bool step5DiagFlareOn() noexcept {return step5FxSigFilter()==acz::fxdiag::kArmFlare;}
inline bool step5DiagAirOn() noexcept {return step5FxSigFilter()==acz::fxdiag::kArmAir;}
// Start gate (fx_diag_flare_air.h): stat %LOCALAPPDATA%\acz-vr\fxdiag.arm at most once a second until the limiter is armed. Step-1 mutex held by the caller.
inline void step5DiagArmPoll(acz::fxdiag::Limiter& lim,uint64_t now) noexcept {
    if(!lim.poll_due(now))return;
    uint64_t stamp=0;wchar_t local[MAX_PATH],path[MAX_PATH];
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(n&&n<MAX_PATH-48) {
        _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\fxdiag.arm",local);
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if(GetFileAttributesExW(path,GetFileExInfoStandard,&d)&&!(d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
            stamp=((((uint64_t(d.ftLastWriteTime.dwHighDateTime)<<32)|d.ftLastWriteTime.dwLowDateTime)*31u)+((uint64_t(d.ftCreationTime.dwHighDateTime)<<32)|d.ftCreationTime.dwLowDateTime))|1u;
    }
    lim.observe_stamp(stamp);
}
// Step-1 mutex held. One HUD-region draw after hud_decide: `verdict` = "admitted" or the refusal reason, `image` = "hud" / "markers" / "none".
// Reads at most 48 position float4 (indexed: the first 48 indices) of a depth-ALWAYS draw within the per-second read budget; logs it when additive or near the
// screen centre (rate limited). Fail closed: any read problem is named in the line (why=) and the bbox stays empty.
__declspec(noinline) void step5FlareLog(Step5State& s5,const gpu_copy::step3::Shape& d,uint32_t ps8,uint32_t vs8,const uint8_t* bl,const char* verdict,
                                        const char* image,const char* cls,bool depthTested) noexcept {
    namespace fd=acz::fxdiag;
    fd::FlareFacts f;f.ps8=ps8;f.vs8=vs8;std::memcpy(f.blend,bl,6);f.blend_on=d.pso.blend0;f.depth_enable=d.pso.depth_enable;f.pso_known=d.pso.known;
    f.depth_func=d.pso.depth_func;f.count=d.count;f.instances=d.instances;f.indexed=d.indexed;
    if(!fd::flare_prefilter(f))return;
    const uint64_t now=GetTickCount64();
    step5DiagArmPoll(s5.diagFlareLim,now);
    if(!s5.diagFlareLim.allow_read(now)){if(s5.diagFlareLim.armed)s5.diagFlareTrk.note_unread();return;}
    fd::Bbox box;unsigned nv=0;const char* why=nullptr;
    if(!d.vb_va||!d.vb_bytes||!d.vb_address)why="vb-unbound";
    else if(d.vb_stride<16||d.vb_stride>4096)why="vb-stride";
    else if(!d.count)why="no-count";
    else if(d.indexed&&(d.ib_stride!=2&&d.ib_stride!=4))why="ib-stride";
    else if(d.indexed&&(!d.ib_va||!d.ib_bytes||!d.ib_address))why="ib-unbound";
    if(!why) {
        auto& s=state();std::lock_guard lock(s.mutex);
        const unsigned nq=d.count<48?d.count:48u;
        int64_t vi[48]={};unsigned n=0;
        if(d.indexed) {
            const uint8_t* ib=nullptr;const uint64_t ibOff=uint64_t(d.start)*d.ib_stride;
            if(ibOff+uint64_t(nq)*d.ib_stride>d.ib_bytes)why="ib-outside-view";
            else if(const char* bad=step3MappedSpan_locked(d.ib_address,d.ib_va+ibOff,uint64_t(nq)*d.ib_stride,ib))why=bad;
            else for(unsigned i=0;i<nq;++i) {
                uint32_t v=0;
                if(!guardedCpuRead(ib+uint64_t(i)*d.ib_stride,&v,d.ib_stride)){why="ib-read-fault";break;}
                vi[n++]=int64_t(v)+d.base;
            }
        } else for(unsigned i=0;i<nq;++i)vi[n++]=int64_t(d.start)+int64_t(i);
        const uint8_t* vb=nullptr;
        if(!why)if(const char* bad=step3MappedSpan_locked(d.vb_address,d.vb_va,d.vb_bytes,vb))why=bad;
        if(!why)for(unsigned i=0;i<n;++i) {
            float p[4]={};
            if(vi[i]<0||(uint64_t(vi[i])*d.vb_stride)+16>d.vb_bytes){why=nv?"vertex-outside-view-partial":"vertex-outside-view";break;}
            if(!guardedCpuRead(vb+uint64_t(vi[i])*d.vb_stride,p,16)){why="vertex-read-fault";break;}
            box.add(p[0],p[1]);++nv;
        }
    }
    // Every observed draw updates its signature (ps, vs, blend, depth, coarse bbox bucket); the tracker decides whether a line is logged (first sighting always;
    // repeats rate limited per signature; candidates get a larger allowance) and the periodic summary below carries the counts and the image routing.
    const fd::FlareTracker::Obs o=s5.diagFlareTrk.observe(now,f,box,verdict,image,cls);
    if(o.log&&o.sig) {
        fd::FlareLine x;x.frame=s5.nextFrameId+1;x.ord=d.draw_ordinal;x.f=f;x.box=box;x.nvtx=nv;x.verdict=verdict;x.image=image;x.hud_class=cls;
        x.depth_tested=depthTested;x.read_why=why;x.sup=0;
        x.tag=o.candidate?"flare-candidate":o.first?"new":"seen";x.score=o.score;x.reasons=o.reasons;x.sg=uint32_t(o.sig->key&0xffffffffu);x.n=o.sig->count;
        s5.diagQ.push_flare(x);
    }
    if(s5.diagFlareTrk.summary_due(now)&&s5.diagQ.count+1+fd::FlareTracker::kSummaryTop<=fd::LineQueue::kCap) {
        char b[fd::kLineMax];unsigned top[fd::FlareTracker::kSummaryTop];
        fd::format_flare_summary(b,sizeof b,s5.diagFlareTrk,now,s5.diagQ.dropped);s5.diagQ.push_text(b);
        const unsigned nt=s5.diagFlareTrk.top(top);
        for(unsigned i=0;i<nt;++i){fd::format_flare_sig(b,sizeof b,s5.diagFlareTrk.sig[top[i]],i+1,now,s5.diagFlareTrk.arm_ms);s5.diagQ.push_text(b);}
        s5.diagFlareTrk.summary_taken(now);
    }
}
// Worker (step-1 mutex held): the queued fx-flare-draw / fx-air-draw lines.
void step5FxDiagEmit(Step1State& st,Step5State& s5) noexcept {
    while(const char* l=s5.diagQ.front()) {
        std::snprintf(st.text,sizeof st.text,"gpu-copy-step1;event=%s",l);
        s5.diagQ.pop();
        step5Log(st,s5,false);
    }
}
