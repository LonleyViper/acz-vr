// J-w1-ee-camera core (see ee_camera_probe.h). SDK-free.
#include "ee_camera_probe.h"
#include "ee_look_write.h"
#include "ee_camera_diff.h"
#include <cstdio>

namespace acz::ee_camera {
bool FinalReportBox::publish(const RegionInfo* regions,unsigned n,const RegionCensus& census,
                             uint64_t passes,const DiffScanner& diff) noexcept {
    try {
        auto report=std::make_shared<FinalReport>();
        report->regions=n;report->wide_passes=passes;
        struct Capture final:Sink {
            FinalReport& report;
            explicit Capture(FinalReport& r):report(r) {}
            void line(bool,const char* text) noexcept override {
                if(report.lines<12)std::snprintf(report.text[report.lines++],448,"%s",text);
            }
        } capture(*report);
        log_region_table(regions,n,census,capture,false);
        log_diff(diff,regions,n,capture,"final");
        std::shared_ptr<const FinalReport> next=std::move(report);
        {std::lock_guard<std::mutex> lock(mutex_);latest_.swap(next);}
        return true; // old snapshot released outside the publication lock
    } catch(...) {return false;}
}

bool FinalReportBox::try_report(const char* trigger,Sink& sink) noexcept {
    std::shared_ptr<const FinalReport> report;
    {
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()||!latest_)return false;
        report=latest_;
    }
    char text[160];
    std::snprintf(text,sizeof text,"ee-camera;event=final-report;trigger=%s;regions=%u;wide-passes=%llu",
        trigger,report->regions,static_cast<unsigned long long>(report->wide_passes));
    sink.line(false,text);
    for(unsigned i=0;i<report->lines;++i)sink.line(false,report->text[i]);
    return true;
}

namespace {
struct Circ {double s=0,c=0;unsigned n=0;
    void add(double a) noexcept {if(std::isfinite(a)){s+=std::sin(a);c+=std::cos(a);++n;}}
    double mean() const noexcept {return n?std::atan2(s,c):0;}
    double std() const noexcept {if(!n)return 1e9;const double R=std::hypot(s,c)/n;return R>=1?0:std::sqrt(-2*std::log(R>1e-12?R:1e-12));}
};
struct Lin {double s=0,ss=0;unsigned n=0;
    void add(double v) noexcept {if(std::isfinite(v)){s+=v;ss+=v*v;++n;}}
    double mean() const noexcept {return n?s/n:0;}
    double var() const noexcept {if(!n)return 1e18;const double m=s/n;const double v=ss/n-m*m;return v>0?v:0;}
};
// The structural words of one E6 structure (the 14 words the scorer uses): sane + yaw + vertical + horizontal
// (+ eye when a basis is given), with `allow` extra radians on the yaw relation for a moving look.
bool structure_holds(const float* v,const Basis* basis,double allow) noexcept {
    const uint32_t need=rel_sane|rel_yaw|rel_vert|rel_horiz|(basis?rel_eye:0u);
    return (relations(v,basis,allow,0,nullptr)&need)==need;
}
// Append formatted text without overflowing.
template<size_t N,class... A> void appendf(char (&buf)[N],size_t& len,const char* fmt,A... a) noexcept {
    if(len>=N-1)return;
    const int w=std::snprintf(buf+len,N-len,fmt,a...);
    if(w>0)len=len+size_t(w)<N?len+size_t(w):N-1;
}
}

void Experiment::emit(Sink& log,bool important,const char* text) noexcept {
    if(important){if(important_>=28)important=false;else ++important_;}
    log.line(important,text);
}

unsigned Experiment::build_trials() noexcept {
    ntrials_=0;
    auto add=[&](const TrialDef& d){if(ntrials_<max_trials)trials_[ntrials_++]=d;};
    TrialDef b;b.family=fam_baseline;b.write_ms=3000;b.settle_ms=0;b.name="baseline";add(b);
    bool positionDone=false;
    for(uint32_t seam=seam_A;seam<=seam_C;++seam) {
        if(!(opt_.points&(1u<<(seam-1))))continue;
        auto ang=[&](uint8_t fam,uint32_t field,float off,uint32_t mode,const char* name) {
            TrialDef d;d.family=fam;d.seam=seam;d.mode=mode;d.n=1;d.field[0]=field;d.offset[0]=off;
            d.write_ms=4000;d.settle_ms=1000;d.name=name;add(d);
        };
        ang(fam_yaw_abs,look_yaw,ten_degrees,mode_absolute,"look-yaw-abs+10");
        ang(fam_yaw_delta,look_yaw,ten_degrees,mode_delta,"look-yaw-delta+10/frame");
        ang(fam_pitch,look_pitch,ten_degrees,mode_absolute,"look-pitch-abs+10");
        ang(fam_pitch,look_pitch,-ten_degrees,mode_absolute,"look-pitch-abs-10");
        ang(fam_roll,look_roll,ten_degrees,mode_absolute,"look-roll-abs+10");
        if(opt_.position&&!positionDone) {
            positionDone=true;
            for(float d:{1.f,5.f}) {
                TrialDef p;p.family=fam_position;p.seam=seam;p.mode=mode_absolute;p.n=2;p.field[0]=cam_x;p.field[1]=cam_z;
                p.offset[0]=d;p.write_ms=3000;p.settle_ms=1000;p.name=d<2?"cam-pos-side+1":"cam-pos-side+5";add(p);
            }
        }
    }
    return ntrials_;
}

bool Experiment::paused(const Snapshot& s) noexcept {
    const uint32_t b[3]={bits(s.v[air_x]),bits(s.v[air_y]),bits(s.v[air_z])};
    const bool same=haveAir_&&b[0]==lastAir_[0]&&b[1]==lastAir_[1]&&b[2]==lastAir_[2];
    stillStreak_=same?stillStreak_+1:0;
    std::memcpy(lastAir_,b,sizeof b);haveAir_=true;
    return stillStreak_>=pause_ticks;
}

void Experiment::tick(Memory& mem,Shared& sh,Scanner& scan,ScanRequester& req,Sink& log,double now,const Basis* basis) noexcept {
    sh.frame.fetch_add(1,std::memory_order_acq_rel);
    fresh_=false;
    if(basis&&basis->seq&&basis->seq!=lastBasisSeq_){lastBasisSeq_=basis->seq;basis_=*basis;haveBasis_=true;fresh_=true;sinceBasis_=0;}
    else if(sinceBasis_<1000000)++sinceBasis_;
    const bool cockpit=haveBasis_&&sinceBasis_<=cockpit_ticks;
    if(lastNow_>=0&&cockpit&&phase_==searching)cockpitSearchMs_+=now-lastNow_;
    lastNow_=now;
    switch(phase_) {
        case searching:search(mem,sh,scan,req,log,now);break;
        case stabilising:stabilise(mem,sh,log,now);break;
        case observing:{Snapshot s;if(opt_.write_trials&&read_snapshot(mem,sh.base.load(),s))maybe_start_trials(mem,sh,log,now,s);break;}
        case running:run(mem,sh,log,now);break;
        default:break;
    }
    observe(mem,sh,log,now); // read-only, in every phase once a block is known
    timeline(mem,log,now);   // rev 4: candidate timeline + tracking classes (read-only)
}

void Experiment::search(Memory& mem,Shared& sh,Scanner& scan,ScanRequester& req,Sink& log,double now) noexcept {
    char t[448];
    if(searchStart_<0)searchStart_=now;
    if(now-searchStart_>search_cap_ms) {
        phase_=refused;reason_="not-located-within-20-min";
        std::snprintf(t,sizeof t,"ee-camera;event=refused;reason=%s;blocks=%u;candidates=%u;scans=%u",reason_,nblocks_,loc_.count(),relScans_);
        emit(log,true,t);return;
    }
    // Block discovery (address-space walk): every 5 s without a block, else every 30 s.
    if(now-lastScan_>=(nblocks_?6*scan_every_ms:scan_every_ms)) {
        lastScan_=now;coll_.reset();scan.scan(coll_);coll_.finish();
        bool changed=coll_.count()!=nblocks_||coll_.total()!=blocksTotal_;
        for(unsigned i=0;!changed&&i<coll_.count();++i)changed=coll_.base(i)!=blocks_[i];
        if(changed) {
            nblocks_=coll_.count();blocksTotal_=coll_.total();
            for(unsigned i=0;i<nblocks_;++i)blocks_[i]=coll_.base(i);
            scanIdx_=0;scansDone_=0;
            size_t n=size_t(std::snprintf(t,sizeof t,"ee-camera;event=candidates;size=0x4000000;private-rw-committed;total=%u;checked=%u;bases=",
                blocksTotal_,nblocks_));
            for(unsigned i=0;i<nblocks_;++i)appendf(t,n,i?",%llx":"%llx",static_cast<unsigned long long>(blocks_[i]));
            emit(log,false,t);
            for(unsigned i=0;i<nblocks_;++i)dump_e6(mem,blocks_[i],log,"discovery");
        }
    }
    // Relative-geometry scans on the worker, only in the cockpit view: every
    // 2 s for the first 60 s of cockpit time, then every 10 s, until validated.
    const bool cockpit=haveBasis_&&sinceBasis_<=cockpit_ticks;
    const double every=cockpitSearchMs_<rel_scan_fast_window_ms?rel_scan_fast_ms:rel_scan_slow_ms;
    // D-245: each request is a WIDE pass (block 0) over every readable region of
    // the process, not only 64 MiB blocks; the worker resumes across passes.
    if(cockpit&&!scanPending_&&now-lastRelScan_>=every) {
        const bool dump=relScans_<2||relScans_%10==0;
        if(req.request(0,&basis_,dump)){scanPending_=true;lastRelScan_=now;++relScans_;}
    }
    if(scanPending_) {
        if(const ScanResult* r=req.poll()) {
            scanPending_=false;unsigned added=0;if(r->complete)++scansDone_;
            for(unsigned i=0;i<r->nfull;++i) {
                if(r->full_off[i]<field_offset[look_yaw])continue;
                const uint64_t eff=r->full_off[i]-field_offset[look_yaw];
                added+=loc_.add(eff,r->full_lo[i],r->full_hi[i]);
            }
            std::snprintf(t,sizeof t,"ee-camera;event=scan-done;scan=%u;regions=%u;first=%llx;full=%llu;near=%llu;degrees=%llu;added=%u;locator=%u;dropped=%u",
                relScans_,r->regions,static_cast<unsigned long long>(r->block),static_cast<unsigned long long>(r->full),
                static_cast<unsigned long long>(r->near),static_cast<unsigned long long>(r->degrees),added,loc_.count(),loc_.dropped());
            emit(log,false,t);
            set_tracks(*r,log);   // rev 4: best candidates -> observation + timeline
            lastScan_=-1e300;     // re-enumerate the 64 MiB blocks after every pass
        }
    }
    // A complete wide pass must have finished before anything can lock (a second
    // matching structure elsewhere must be able to make it ambiguous).
    const auto st=loc_.count()&&scansDone_>=1?loc_.step(mem):Locator::searching;
    if(st==Locator::refused) {
        phase_=refused;reason_=loc_.reason();
        size_t n=size_t(std::snprintf(t,sizeof t,"ee-camera;event=refused;reason=%s;validating=",reason_));
        for(unsigned i=0;i<loc_.count();++i)appendf(t,n,"%s%llx:%u",i?",":"",static_cast<unsigned long long>(loc_.candidate(i)),loc_.streak(i));
        emit(log,true,t);return;
    }
    if(st==Locator::locked) {
        Snapshot s;const uint64_t base=loc_.base(),block=loc_.locked_block(),hi=loc_.locked_hi();
        (void)read_snapshot(mem,base,s);
        sh.block_lo.store(block,std::memory_order_release);sh.block_hi.store(hi,std::memory_order_release);
        sh.base.store(base,std::memory_order_release); // no command is armed; writes stay disabled
        phase_=stabilising;stableSince_=-1;badStreak_=0;
        const auto& v=s.v;
        std::snprintf(t,sizeof t,"ee-camera;event=located;base=%llx;block=%llx;shift=%+lld;look-yaw-at=+%llX;frames=%u;tracking=1;look=%.5f,%.5f,%.5f;"
            "air=%.2f,%.2f,%.2f;heading=%.5f;cam=%.2f,%.2f,%.2f;rot=%.5f,%.5f,%.5f;eye-h=%.3f;render=%s",
            static_cast<unsigned long long>(base),static_cast<unsigned long long>(block),static_cast<long long>(base)-static_cast<long long>(block),
            static_cast<unsigned long long>(base-block+field_offset[look_yaw]),validate_frames,v[look_pitch],v[look_yaw],v[look_roll],
            v[air_x],v[air_y],v[air_z],v[heading],v[cam_x],v[cam_y],v[cam_z],v[cam_pitch],v[cam_yaw],v[cam_roll],
            double(v[cam_y])-double(v[air_y]),s.render_ok?"readable":"outside-block");
        emit(log,true,t);
        return;
    }
    if(now-lastStatus_>=10000) {
        lastStatus_=now;
        size_t n=size_t(std::snprintf(t,sizeof t,"ee-camera;event=search;blocks=%u;scans=%u;cockpit-ms=%.0f;candidates=%u",nblocks_,relScans_,
            cockpitSearchMs_,loc_.count()));
        for(unsigned i=0;i<loc_.count();++i)appendf(t,n,";c%u=%llx:%s:%u",i,static_cast<unsigned long long>(loc_.candidate(i)),loc_.why(i),loc_.streak(i));
        emit(log,false,t);
    }
}

void Experiment::stabilise(Memory& mem,Shared& sh,Sink& log,double now) noexcept {
    char t[448];Snapshot s;
    auto backToSearch=[&](const char* why) {
        sh.base.store(0,std::memory_order_release);loc_.reset();lastRelScan_=-1e300;phase_=searching;stableSince_=-1;obsValidated_=false;
        std::snprintf(t,sizeof t,"ee-camera;event=relocate;reason=%s",why);emit(log,false,t);
    };
    const uint64_t base=sh.base.load(std::memory_order_acquire);
    if(!read_snapshot(mem,base,s)){backToSearch("block-unreadable");return;}
    const char* why=validate(s);const bool p=paused(s);const bool cockpit=haveBasis_&&sinceBasis_<=cockpit_ticks;
    if(!why&&!p&&cockpit) {
        badStreak_=0;
        if(stableSince_<0)stableSince_=now;
        if(now-stableSince_>=stable_ms) {
            phase_=observing;
            std::snprintf(t,sizeof t,"ee-camera;event=validated;stable-ms=%.0f;base=%llx;mode=%s",now-stableSince_,static_cast<unsigned long long>(base),
                opt_.write_trials?"observe-then-write-trials-if-prerequisites":"observe-only;writes=disabled(ee_camera_write_trials=false)");
            emit(log,true,t);
            if(opt_.write_trials)maybe_start_trials(mem,sh,log,now,s);
        }
        return;
    }
    stableSince_=-1;
    badStreak_=why?badStreak_+1:0;
    if(badStreak_>=600){backToSearch(why);return;}
    if(now-lastStatus_>=5000) {
        lastStatus_=now;
        std::snprintf(t,sizeof t,"ee-camera;event=wait;validation=%s;paused=%u;cockpit=%u;basis-age-ticks=%u",why?why:"ok",unsigned(p),unsigned(cockpit),sinceBasis_);
        emit(log,false,t);
    }
}

// Write trials need ALL of: the key, a validated block (we are in `observing`
// after 5 s of stable validation), demonstrated boundary ordering, and an
// independent world-path observer. Production has no observer: never writes.
void Experiment::maybe_start_trials(Memory& mem,Shared& sh,Sink& log,double now,const Snapshot& s) noexcept {
    char t[448];
    const Ordering o=ordering(sh);
    const char* why=!opt_.write_trials?"key-off":!opt_.world?"no-world-path-observer":!o.demonstrated?"ordering-not-demonstrated":
                    validate(s)?"validation":!(haveBasis_&&sinceBasis_<=cockpit_ticks)?"no-cockpit":nullptr;
    if(why) {
        if(!writeRefusedLogged_||(now-lastStatus_>=10000&&std::strcmp(why,"ordering-not-demonstrated")==0)) {
            writeRefusedLogged_=true;lastStatus_=now;
            std::snprintf(t,sizeof t,"ee-camera;event=write-trials-refused;reason=%s;ordering=%s%s:share=%.3f,rate=%.3f,visits=%llu",why,
                seam_name(o.from),seam_name(o.to),o.share,o.rate,static_cast<unsigned long long>(o.visits));
            emit(log,false,t);
        }
        return;
    }
    build_trials();runStart_=now;phase_=running;cur_=0;settle_=false;nidle_=0;idleHead_=0;
    sh.writes_enabled.store(true,std::memory_order_release);
    double total=0;for(unsigned i=0;i<ntrials_;++i)total+=trials_[i].write_ms+trials_[i].settle_ms;
    std::snprintf(t,sizeof t,"ee-camera;event=start;trials=%u;points=%s%s%s;position=%u;planned-ms=%.0f;cap-ms=%.0f;ordering=%s%s:%.3f;"
        "bounds=+-%.2frad,+-%.1funits;one-scalar-per-write;restore=anchored;one-attempt",ntrials_,opt_.points&1?"A":"",opt_.points&2?"B":"",
        opt_.points&4?"C":"",unsigned(opt_.position),total,run_cap_ms,seam_name(o.from),seam_name(o.to),o.share,angle_bound,position_bound);
    emit(log,true,t);
    begin_trial(mem,sh,log,now,s);
}

// Read-only observation (Astra's stage B): per-frame seam tuples and the
// field-activity map. Never writes; runs whenever a block is known and the
// cockpit has been seen.
void Experiment::observe(Memory& mem,Shared& sh,Sink& log,double now) noexcept {
    char t[448];
    const bool cockpit=haveBasis_&&sinceBasis_<=cockpit_ticks;
    // Observation base (rev 4): the validated effective base (sticky), else the
    // BEST-SCORING scan candidate (re-picked every pass), else the E6 base of the first block.
    uint64_t want=0,lo=0,hi=0;bool validated=false;char kind='e';
    if(const uint64_t b=sh.base.load(std::memory_order_acquire)){want=b;lo=sh.block_lo.load();hi=sh.block_hi.load();validated=true;kind='v';}
    else if(obsValidated_){want=obsBase_;lo=sh.block_lo.load();hi=sh.block_hi.load();validated=true;kind='v';}
    else if(bestEff_){want=bestEff_;lo=bestLo_;hi=bestHi_;kind='b';}
    else if(nblocks_){want=blocks_[0];lo=blocks_[0];hi=lo+block_bytes;}
    if(!want)return;
    if(want!=obsBase_||kind!=obsKind_) {
        obsBase_=want;obsValidated_=validated;obsKind_=kind;obsLogged_=0;actHave_=false;actSamples_=0;std::memset(actCount_,0,sizeof actCount_);
        betweenAB_=betweenBC_=betweenCA_=betweenFrames_=0;prevC_=ObsSample{};
        if(!validated){sh.block_lo.store(lo,std::memory_order_release);sh.block_hi.store(hi,std::memory_order_release);}
        sh.obs_base.store(want,std::memory_order_release);
        std::snprintf(t,sizeof t,"ee-camera;event=observe-base;base=%llx;block=%llx;kind=%s;series=first-%u-frames-then-every-%u;activity=+-0x2000@E6-offsets/1s;read-only",
            static_cast<unsigned long long>(want),static_cast<unsigned long long>(lo),
            kind=='v'?"validated":kind=='b'?"best-candidate":"e6-unvalidated",obs_full_frames,obs_every);
        emit(log,true,t);
    }
    if(cockpit)cockpitSeen_=true;
    if(!cockpitSeen_)return;
    ++obsFrames_;
    const uint64_t k=sh.frame.load(std::memory_order_acquire);
    ObsSample s[4];
    for(uint32_t i=1;i<=3;++i){if(!read_record(sh.seamRec[i],s[i])||s[i].interval!=k)s[i].valid=false;}
    // Field changes between seams of the same frame (any of the 23 field words).
    auto differ=[](const ObsSample& x,const ObsSample& y){for(unsigned i=0;i<23;++i)if(bits(x.w[i])!=bits(y.w[i]))return true;return false;};
    if(s[1].valid&&s[2].valid&&s[3].valid) {
        ++betweenFrames_;betweenAB_+=differ(s[1],s[2]);betweenBC_+=differ(s[2],s[3]);
        if(prevC_.valid)betweenCA_+=differ(prevC_,s[1]);
    }
    if(s[3].valid)prevC_=s[3];
    kind=obsKind_;
    if(obsLogged_<obs_full_frames||(obsFrames_<3000?obsFrames_%obs_every==0:obsFrames_%60==0)) {
        ++obsLogged_;
        if(obsQpc0_==0)for(uint32_t i=1;i<=3;++i)if(s[i].valid){obsQpc0_=double(s[i].qpc);break;}
        auto seam=[&](char (&buf)[448],size_t& n,uint32_t i) {
            const ObsSample& o=s[i];
            if(!o.valid){appendf(buf,n,";%s=-",seam_name(i));return;}
            const float* w=o.w;
            appendf(buf,n,";%s=%.0f,%u,%u:%.6g,%.6g,%.6g,%.6g,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g,%.6g,%.6g,%.6g",seam_name(i),double(o.qpc)-obsQpc0_,o.tid,o.ok,
                obs_field(w,look_pitch),obs_field(w,look_yaw),obs_field(w,look_roll),obs_field(w,heading),obs_field(w,air_x),obs_field(w,air_y),
                obs_field(w,air_z),obs_field(w,cam_x),obs_field(w,cam_y),obs_field(w,cam_z),obs_field(w,cam_pitch),obs_field(w,cam_yaw),obs_field(w,cam_roll));
        };
        size_t n=0;appendf(t,n,"ee-camera;obs;f=%llu;k=%c;fields=lp,ly,lr,hdg,ax,ay,az,cx,cy,cz,rp,ry,rr",static_cast<unsigned long long>(k),kind);
        seam(t,n,1);seam(t,n,2);emit(log,false,t);
        n=0;appendf(t,n,"ee-camera;obs2;f=%llu",static_cast<unsigned long long>(k));seam(t,n,3);
        if(s[2].valid) {
            const float* w=s[2].w;
            appendf(t,n,";rcB=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g",w[23],w[24],w[25],w[26],w[27],w[28],w[29],w[30],w[31]);
            if(haveBasis_) {
                const double d[3]={double(obs_field(w,cam_x))-obs_field(w,air_x),double(obs_field(w,cam_y))-obs_field(w,air_y),
                                   double(obs_field(w,cam_z))-obs_field(w,air_z)};
                const double r16=std::sqrt((basis_.eye[0]/16-d[0])*(basis_.eye[0]/16-d[0])+(basis_.eye[1]/16-d[1])*(basis_.eye[1]/16-d[1])+
                                           (basis_.eye[2]/16-d[2])*(basis_.eye[2]/16-d[2]));
                appendf(t,n,";basis=%llu/%llu:eye=%.5g,%.5g,%.5g:fwd=%.5f,%.5f,%.5f:eye16-vs-cam-air=%.4g",static_cast<unsigned long long>(basis_.seq),
                    static_cast<unsigned long long>(s[2].basis),basis_.eye[0],basis_.eye[1],basis_.eye[2],basis_.fwd[0],basis_.fwd[1],basis_.fwd[2],r16);
            }
        }
        emit(log,false,t);
    }
    // Field activity: every aligned float within +-0x2000 bytes of the E6 offsets.
    {
        const uint64_t blo=sh.block_lo.load(),bhi=sh.block_hi.load();
        const uint64_t a0=obsBase_+act_lo0,a1=obsBase_+act_lo1;
        const bool ok0=a0>=blo&&a0+act_floats0*4<=bhi&&mem.read(a0,actCur_,act_floats0*4);
        const bool ok1=a1>=blo&&a1+act_floats1*4<=bhi&&mem.read(a1,actCur_+act_floats0,act_floats1*4);
        if(actHave_)for(unsigned i=0;i<act_floats;++i) {
            if((i<act_floats0?ok0:ok1)&&bits(actCur_[i])!=bits(actPrev_[i])&&actCount_[i]<0xFFFF)++actCount_[i];
        }
        std::memcpy(actPrev_,actCur_,sizeof actPrev_);actHave_=ok0||ok1;++actSamples_;
        const double every=actSummaries_<120?1000:10000;
        if(now-lastActivity_>=every&&actSamples_>1) {
            lastActivity_=now;++actSummaries_;unsigned lines=0,runs=0,changed=0;
            for(int w=0;w<2;++w) {
                const unsigned first=w?act_floats0:0,last=w?act_floats:act_floats0;const uint32_t wlo=w?act_lo1:act_lo0;
                size_t n=0;bool open=false;
                appendf(t,n,"ee-camera;event=activity;s=%u;samples=%u;win=%d;readable=%u;runs=",actSummaries_,actSamples_,w,unsigned(w?ok1:ok0));
                size_t header=n;
                for(unsigned i=first;i<=last;++i) {
                    const bool c=i<last&&actCount_[i]>0;
                    if(c)++changed;
                    if(c&&!open){open=true;unsigned j=i,mx=0;while(j<last&&actCount_[j]>0){mx=actCount_[j]>mx?actCount_[j]:mx;++j;}
                        ++runs;
                        if(n>380){if(lines<6){emit(log,false,t);++lines;}n=header;t[n]=0;}
                        appendf(t,n,"%s%X-%X:%u",n>header?",":"",wlo+(i-first)*4,wlo+(j-1-first)*4,mx);}
                    if(!c)open=false;
                }
                if(lines<6){emit(log,false,t);++lines;}
            }
            std::memset(actCount_,0,sizeof actCount_);actSamples_=0;(void)runs;(void)changed;
        }
    }
    if(now-lastObsSummary_>=10000) {
        lastObsSummary_=now;const Ordering o=ordering(sh);
        size_t n=0;
        appendf(t,n,"ee-camera;event=observe-summary;frames=%llu;logged=%llu;kind=%c;between-seams-changed=AB:%llu,BC:%llu,CA:%llu/%llu;"
            "ordering=%s%s:share=%.3f,rate=%.3f,visits=%llu,demonstrated=%u;tids=A:%u,B:%u,C:%u;writes=%s",
            static_cast<unsigned long long>(obsFrames_),static_cast<unsigned long long>(obsLogged_),kind,static_cast<unsigned long long>(betweenAB_),
            static_cast<unsigned long long>(betweenBC_),static_cast<unsigned long long>(betweenCA_),static_cast<unsigned long long>(betweenFrames_),
            seam_name(o.from),seam_name(o.to),o.share,o.rate,static_cast<unsigned long long>(o.visits),unsigned(o.demonstrated),
            s[1].tid,s[2].tid,s[3].tid,sh.writes_enabled.load()?"enabled":"disabled");
        appendf(t,n,";ee-phase=");bool first=true;
        for(uint32_t a=1;a<=3;++a)for(uint32_t b=1;b<=3;++b){const uint32_t v=sh.phase_visits[a][b].load(),c=sh.phase_changes[a][b].load();
            if(!v)continue;appendf(t,n,"%s%s%s:%u/%u",first?"":",",seam_name(a),seam_name(b),c,v);first=false;}
        emit(log,false,t);
    }
}

void Experiment::quantities(const Raw& r,Q& q) const noexcept {
    const auto& v=r.s.v;
    q.rec=r.basis;
    if(q.rec) {
        double y=r.recYaw;
        if(yawComp_<0)y=wrap(y-v[heading]);else if(yawComp_>0)y=wrap(y+v[heading]);
        q.recYaw=y;q.recPitch=r.recPitch;q.recRoll=r.recRoll;
        const float air[3]={v[air_x],v[air_y],v[air_z]};
        for(int k=0;k<3;++k)q.recEye[k]=r.eye[k]-(eyeComp_?double(air[k]):0.0);
    }
    q.compYaw=wrap(double(v[cam_yaw])-double(v[heading]));q.compPitch=v[cam_pitch];q.compRoll=v[cam_roll];
    q.compPos[0]=double(v[cam_x])-double(v[air_x]);q.compPos[1]=double(v[cam_z])-double(v[air_z]);
    for(int j=0;j<5;++j) {
        q.hasRend[j]=rendIdx_[j]>=0;if(!q.hasRend[j])continue;
        const double x=r.s.render[rendIdx_[j]];
        q.rend[j]=j==1?wrap(x-v[heading]):j==3?x-v[air_x]:j==4?x-v[air_z]:x;
    }
}

void Experiment::reference(Q& out) const noexcept {
    out=Q{};
    const Raw* src=idle_;unsigned n=nidle_;
    if(n<5&&nbase_){src=base_;n=nbase_;}
    Circ ry,rp,rr,cy,cp,cr,re[5];Lin e[3],cpos[2],rpos[5];
    for(unsigned i=0;i<n;++i) {
        Q q;quantities(src[i],q);
        if(q.rec){ry.add(q.recYaw);rp.add(q.recPitch);rr.add(q.recRoll);for(int k=0;k<3;++k)e[k].add(q.recEye[k]);}
        cy.add(q.compYaw);cp.add(q.compPitch);cr.add(q.compRoll);cpos[0].add(q.compPos[0]);cpos[1].add(q.compPos[1]);
        for(int j=0;j<5;++j)if(q.hasRend[j]){if(j<3)re[j].add(q.rend[j]);else rpos[j].add(q.rend[j]);}
    }
    out.rec=ry.n>0;out.recYaw=ry.mean();out.recPitch=rp.mean();out.recRoll=rr.mean();for(int k=0;k<3;++k)out.recEye[k]=e[k].mean();
    out.compYaw=cy.mean();out.compPitch=cp.mean();out.compRoll=cr.mean();out.compPos[0]=cpos[0].mean();out.compPos[1]=cpos[1].mean();
    for(int j=0;j<5;++j){out.hasRend[j]=j<3?re[j].n>0:rpos[j].n>0;out.rend[j]=j<3?re[j].mean():rpos[j].mean();}
}

void Experiment::begin_trial(Memory&,Shared& sh,Sink& log,double now,const Snapshot& s) noexcept {
    char t[448];
    const TrialDef& d=trials_[cur_];TrialResult& r=results_[cur_];
    r=TrialResult{};r.family=d.family;r.seam=d.seam;r.name=d.name;
    nsamples_=0;wFrames_=0;wLen_=0;settle_=false;phaseEnd_=now+d.write_ms;havePrev_=false;
    trialTick_=sh.frame.load(std::memory_order_acquire);firstStamp_=0;lastLoggedStamp_=0;writesAtBegin_=sh.writes.load();
    reference(ref_);yawAllow_=0;
    if(d.family==fam_baseline) {
        nbase_=0;
        std::snprintf(t,sizeof t,"ee-camera;event=trial-begin;trial=%u;name=baseline;writes=none;ms=%.0f",cur_,d.write_ms);
        emit(log,false,t);return;
    }
    Command c;c.seam=d.seam;c.mode=d.mode;c.n=d.n;
    float off[2]={d.offset[0],d.offset[1]};
    if(d.family==fam_position) { // sideways ASSUMPTION: forward = (sin h, 0, cos h), right = (cos h, 0, -sin h)
        const double h=s.v[heading];off[0]=float(d.offset[0]*std::cos(h));off[1]=float(-d.offset[0]*std::sin(h));
    }
    for(uint32_t i=0;i<d.n;++i) {
        const uint32_t f=d.field[i];orig_[i]=s.v[f];c.field[i]=f;
        const double bound=angle_field(f)?angle_bound:position_bound;
        // Position: relative to the aircraft's CURRENT position (anchor), so the
        // held value is "eye offset at trial start + sideways", never a stale world point.
        anchor_[i]=position_field(f)?(f==cam_x?uint32_t(air_x):uint32_t(air_z)):no_anchor;
        anchorOrig_[i]=anchor_[i]!=no_anchor?s.v[anchor_[i]]:0.f;
        c.anchor[i]=anchor_[i];
        const double rel=double(orig_[i])-double(anchorOrig_[i]); // == orig for angles
        c.lo[i]=float(rel-bound);c.hi[i]=float(rel+bound);
        if(d.mode==mode_absolute) {
            double v=rel+off[i];v=v<c.lo[i]?c.lo[i]:(v>c.hi[i]?c.hi[i]:v);c.value[i]=float(v);
        } else c.step[i]=off[i];
    }
    if(d.field[0]==look_yaw)yawAllow_=angle_bound+1e-3;
    sh.cmd.publish(c);
    size_t n=size_t(std::snprintf(t,sizeof t,"ee-camera;event=trial-begin;trial=%u;name=%s;seam=%s;mode=%s;ms=%.0f;settle-ms=%.0f",
        cur_,d.name,seam_name(d.seam),d.mode==mode_delta?"delta":"absolute",d.write_ms,d.settle_ms));
    for(uint32_t i=0;i<d.n;++i)
        appendf(t,n,";f%u=%s@+%X;orig=%.5f;%s=%.5f;lo=%.5f;hi=%.5f",i,field_name[c.field[i]],field_offset[c.field[i]],orig_[i],
            d.mode==mode_delta?"step":"value",d.mode==mode_delta?c.step[i]:c.value[i],c.lo[i],c.hi[i]);
    emit(log,false,t);
}

void Experiment::flush_w(Sink& log) noexcept {
    if(!wLen_){wFrames_=0;return;}
    char t[448];const TrialDef& d=trials_[cur_];
    std::snprintf(t,sizeof t,"ee-camera;event=w;trial=%u;seam=%s;off=+%X;%s",cur_,seam_name(d.seam),field_offset[d.field[0]],w_);
    emit(log,false,t);wLen_=0;wFrames_=0;w_[0]=0;
}

void Experiment::restore(Memory& mem,Shared& sh,Sink& log) noexcept {
    char t[448];
    sh.cmd.disarm();
    for(unsigned spin=0;sh.inflight.load(std::memory_order_seq_cst)!=0&&spin<2000000;++spin){}
    const TrialDef& d=trials_[cur_];TrialResult& r=results_[cur_];
    r.restore="no-write";
    const uint64_t base=sh.base.load(std::memory_order_acquire);
    if(!base||!d.n)return;
    for(uint32_t i=0;i<d.n;++i) {
        const uint64_t st=sh.rec[i].stamp.load(std::memory_order_acquire);
        if(!st||st<trialTick_)continue;
        const uint32_t lastNew=uint32_t(sh.rec[i].old_new.load(std::memory_order_relaxed));
        const uint64_t a=base+field_offset[d.field[i]];float cur=0,target=orig_[i];const char* status;
        float anc=0;
        const bool anchored=anchor_[i]!=no_anchor;
        if(!read_float(mem,a,cur)||(anchored&&!read_float(mem,base+field_offset[anchor_[i]],anc)))status="read-fault";
        else {
            // Still ours? Angles: bit-identical to the last write. Anchored
            // position: the same offset from the aircraft as the last write
            // (a game that integrates the camera with the aircraft moves both).
            const double lastRel=double(from_bits(lastNew))-double(from_bits(sh.rec[i].anchor.load(std::memory_order_relaxed)));
            const bool ours=anchored?std::fabs((double(cur)-double(anc))-lastRel)<=0.05:bits(cur)==lastNew;
            if(anchored)target=float(double(anc)+double(orig_[i])-double(anchorOrig_[i]));
            if(!ours)status="game-owned"; // the game already replaced our value: leave it
            else status=mem.write_float(a,target)?"restored":"write-fault";
        }
        r.restore=status;
        std::snprintf(t,sizeof t,"ee-camera;event=restore;trial=%u;off=+%X;current=%.5f;last-written=%.5f;orig=%.5f;target=%.5f;status=%s",
            cur_,field_offset[d.field[i]],cur,from_bits(lastNew),orig_[i],target,status);
        emit(log,false,t);
    }
}

void Experiment::end_write(Memory& mem,Shared& sh,Sink& log,double) noexcept {
    char t[448];
    flush_w(log);
    restore(mem,sh,log);
    TrialResult& r=results_[cur_];
    float cmd[max_samples],rec[max_samples],comp[max_samples],rend[max_samples];int lag[max_samples];
    unsigned persisted=0,reverted=0;
    for(unsigned i=0;i<nsamples_;++i) {
        cmd[i]=samples_[i].cmd;rec[i]=samples_[i].rec;comp[i]=samples_[i].comp;rend[i]=samples_[i].rend;lag[i]=samples_[i].lag;
        persisted+=samples_[i].rb==rb_persisted;reverted+=samples_[i].rb==rb_reverted;
    }
    r.rec=classify(cmd,rec,lag,nsamples_);r.comp=classify(cmd,comp,lag,nsamples_);r.rend=classify(cmd,rend,lag,nsamples_);
    r.samples=nsamples_;r.persisted=nsamples_?double(persisted)/nsamples_:0;r.reverted=nsamples_?double(reverted)/nsamples_:0;
    r.writes=unsigned(sh.writes.load()-writesAtBegin_);
    r.hyp=hypothesis(r.rec.verdict,r.comp.verdict,r.rend.verdict,r.persisted,r.reverted);r.done=true;
    std::snprintf(t,sizeof t,"ee-camera;event=trial;trial=%u;name=%s;seam=%s;samples=%u;writes=%u;stale=%u;rec=%s;lag=%d;ratio=%+.3f;mean=%+.4f;min=%+.4f;"
        "max=%+.4f;cmd=%+.4f;peak-cmd=%.4f;comp=%s;comp-lag=%d;comp-ratio=%+.3f;rend=%s;rend-ratio=%+.3f;readback=persist:%.2f,revert:%.2f;restore=%s;hyp=%s",
        cur_,r.name,seam_name(r.seam),r.samples,r.writes,r.stale,verdict_name(r.rec.verdict),r.rec.lag,r.rec.ratio,r.rec.mean_resp,r.rec.min_resp,
        r.rec.max_resp,r.rec.mean_cmd,r.peak_cmd,verdict_name(r.comp.verdict),r.comp.lag,r.comp.ratio,verdict_name(r.rend.verdict),r.rend.ratio,
        r.persisted,r.reverted,r.restore,r.hyp);
    emit(log,true,t);
}

void Experiment::finish_baseline(Sink& log) noexcept {
    char t[448];
    Circ raw,minus,plus;Lin eRaw[3],eAir[3],dyaw,deye;
    for(unsigned i=0;i<nbase_;++i) {
        const Raw& r=base_[i];if(!r.basis)continue;const auto& v=r.s.v;
        raw.add(r.recYaw);minus.add(wrap(r.recYaw-v[heading]));plus.add(wrap(r.recYaw+v[heading]));
        const double air[3]={v[air_x],v[air_y],v[air_z]},cam[3]={v[cam_x],v[cam_y],v[cam_z]};
        for(int k=0;k<3;++k){eRaw[k].add(r.eye[k]);eAir[k].add(r.eye[k]-air[k]);}
        dyaw.add(wrap(r.recYaw-v[cam_yaw]));
        deye.add(std::sqrt((r.eye[0]-cam[0])*(r.eye[0]-cam[0])+(r.eye[1]-cam[1])*(r.eye[1]-cam[1])+(r.eye[2]-cam[2])*(r.eye[2]-cam[2])));
    }
    // Prefer the raw recovered yaw unless a heading-compensated form is clearly steadier.
    yawComp_=0;
    if(raw.n) {
        const double best=minus.std()<plus.std()?minus.std():plus.std();
        if(best<0.5*raw.std())yawComp_=minus.std()<plus.std()?-1:1;
    }
    const double sRaw=std::sqrt(eRaw[0].var()+eRaw[1].var()+eRaw[2].var()),sAir=std::sqrt(eAir[0].var()+eAir[1].var()+eAir[2].var());
    eyeComp_=raw.n&&sAir<0.5*sRaw;
    // Render-side copy: which of its floats equals each composed field.
    static constexpr unsigned composed[5]={cam_pitch,cam_yaw,cam_roll,cam_x,cam_z};
    for(int j=0;j<5;++j) {
        rendIdx_[j]=-1;double bestErr=1e30;
        for(unsigned m=0;m<render_floats;++m) {
            double err=0;for(unsigned i=0;i<nbase_;++i){const double e=std::fabs(double(base_[i].s.render[m])-double(base_[i].s.v[composed[j]]));err=e>err?e:err;}
            if(nbase_&&err<bestErr){bestErr=err;if(err<=(j<3?1e-3:0.05))rendIdx_[j]=int(m);}
        }
    }
    std::snprintf(t,sizeof t,"ee-camera;event=baseline;frames=%u;basis-frames=%u;yaw-comp=%s;yaw-std=raw:%.4f,minus-heading:%.4f,plus-heading:%.4f;"
        "eye-comp=%s;eye-std=raw:%.3f,minus-air:%.3f;rend-idx=%d,%d,%d,%d,%d;rec-minus-cam-yaw=%+.4f;eye-minus-cam=%.3f",
        nbase_,raw.n,yawComp_<0?"minus-heading":yawComp_>0?"plus-heading":"raw",raw.std(),minus.std(),plus.std(),eyeComp_?"minus-air":"raw",
        sRaw,sAir,rendIdx_[0],rendIdx_[1],rendIdx_[2],rendIdx_[3],rendIdx_[4],dyaw.mean(),deye.mean());
    emit(log,true,t);
}

void Experiment::run(Memory& mem,Shared& sh,Sink& log,double now) noexcept {
    const TrialDef& d=trials_[cur_];
    if(now-runStart_>run_cap_ms){abort(mem,sh,log,"run-time-bound");return;}
    if(const uint32_t f=sh.fault.load()){abort(mem,sh,log,f==1?"seam-read-fault":"seam-write-fault");return;}
    Snapshot s;
    if(!read_snapshot(mem,sh.base.load(std::memory_order_acquire),s)){abort(mem,sh,log,"read-fault");return;}
    if(const char* why=validate(s,yawAllow_,0.5)){abort(mem,sh,log,why);return;}
    if(paused(s)){abort(mem,sh,log,"paused");return;}
    if(!(haveBasis_&&sinceBasis_<=cockpit_ticks)){abort(mem,sh,log,"cockpit-lost");return;}
    Raw raw;raw.s=s;raw.basis=fresh_;
    if(fresh_){raw.recYaw=basis_yaw(basis_);raw.recPitch=basis_pitch(basis_);raw.recRoll=basis_roll(basis_);for(int k=0;k<3;++k)raw.eye[k]=basis_.eye[k];}
    const uint64_t tick=sh.frame.load(std::memory_order_acquire);
    auto pushIdle=[&]{idle_[idleHead_]=raw;idleHead_=(idleHead_+1)%idle_cap;if(nidle_<idle_cap)++nidle_;};
    TrialResult& r=results_[cur_];
    if(d.family==fam_baseline){if(nbase_<baseline_cap)base_[nbase_++]=raw;pushIdle();}
    else if(!settle_) {
        if(!raw.basis)++r.stale;
        const uint64_t st=sh.rec[0].stamp.load(std::memory_order_acquire);
        if(st&&st>=trialTick_&&nsamples_<max_samples) {
            if(!firstStamp_)firstStamp_=st;
            const uint64_t on=sh.rec[0].old_new.load(std::memory_order_relaxed);
            const float old0=from_bits(uint32_t(on>>32)),new0=from_bits(uint32_t(on));
            float old1=0,new1=0;
            if(d.n==2){const uint64_t on1=sh.rec[1].old_new.load(std::memory_order_relaxed);old1=from_bits(uint32_t(on1>>32));new1=from_bits(uint32_t(on1));}
            Q q;quantities(raw,q);
            Sample sm;sm.lag=int(int64_t(tick)-int64_t(firstStamp_+1));
            const float nan=std::nanf("");sm.rec=sm.comp=sm.rend=nan;
            switch(d.family) {
                case fam_yaw_abs:case fam_yaw_delta:
                    sm.cmd=new0-orig_[0];
                    if(q.rec&&ref_.rec)sm.rec=float(wrap(q.recYaw-ref_.recYaw));
                    sm.comp=float(wrap(q.compYaw-ref_.compYaw));
                    if(q.hasRend[1]&&ref_.hasRend[1])sm.rend=float(wrap(q.rend[1]-ref_.rend[1]));
                    break;
                case fam_pitch:case fam_roll: {
                    const bool p=d.family==fam_pitch;const int j=p?0:2;
                    sm.cmd=new0-orig_[0];
                    if(q.rec&&ref_.rec)sm.rec=float(wrap(p?q.recPitch-ref_.recPitch:q.recRoll-ref_.recRoll));
                    sm.comp=float(wrap(p?q.compPitch-ref_.compPitch:q.compRoll-ref_.compRoll));
                    if(q.hasRend[j]&&ref_.hasRend[j])sm.rend=float(wrap(q.rend[j]-ref_.rend[j]));
                    break;
                }
                default: { // position: magnitudes (scale 1:1 between PS2 and the recovered space is an ASSUMPTION)
                    // commanded = (written - aircraft at the write) - (trial-start eye offset)
                    const double a0=from_bits(sh.rec[0].anchor.load(std::memory_order_relaxed));
                    const double a1=from_bits(sh.rec[1].anchor.load(std::memory_order_relaxed));
                    sm.cmd=float(std::hypot((double(new0)-a0)-(double(orig_[0])-anchorOrig_[0]),(double(new1)-a1)-(double(orig_[1])-anchorOrig_[1])));
                    if(q.rec&&ref_.rec){double e=0;for(int k=0;k<3;++k)e+=(q.recEye[k]-ref_.recEye[k])*(q.recEye[k]-ref_.recEye[k]);sm.rec=float(std::sqrt(e));}
                    sm.comp=float(std::hypot(q.compPos[0]-ref_.compPos[0],q.compPos[1]-ref_.compPos[1]));
                    if(q.hasRend[3]&&q.hasRend[4]&&ref_.hasRend[3]&&ref_.hasRend[4])
                        sm.rend=float(std::hypot(q.rend[3]-ref_.rend[3],q.rend[4]-ref_.rend[4]));
                }
            }
            // Readback = the value the seam found just before THIS write versus the
            // previous write of the trial: it spans one whole interval (including
            // the EE frame) whichever seam writes. Anchored fields compare offsets.
            if(st!=lastLoggedStamp_) {
                const bool anchored=anchor_[0]!=no_anchor;
                const double anc=anchored?double(from_bits(sh.rec[0].anchor.load(std::memory_order_relaxed))):0.0;
                const double relOld=double(old0)-anc,relOrig=double(orig_[0])-double(anchorOrig_[0]);
                const double tol=anchored?0.05:1e-5;
                if(havePrev_)sm.rb=(anchored?std::fabs(relOld-prevRel_)<=tol:bits(old0)==bits(prevNew_))?rb_persisted:
                                   (std::fabs(relOld-relOrig)<=tol?rb_reverted:rb_other);
                prevNew_=new0;prevRel_=double(new0)-anc;havePrev_=true;
            }
            samples_[nsamples_++]=sm;
            const double pc=std::fabs(double(sm.cmd));r.peak_cmd=pc>r.peak_cmd?pc:r.peak_cmd;
            const char rbc=sm.rb==rb_persisted?'p':sm.rb==rb_reverted?'r':'o';
            if(st!=lastLoggedStamp_) { // every write is logged once: stamp:old>new/rec/comp/readback
                lastLoggedStamp_=st;
                if(d.n==2)appendf(w_,wLen_,"%s%llu:%.3f>%.3f,%.3f>%.3f/%+.3f/%+.3f/%c",wLen_?",":"",static_cast<unsigned long long>(st),old0,new0,old1,new1,
                    double(sm.rec),double(sm.comp),rbc);
                else appendf(w_,wLen_,"%s%llu:%.5f>%.5f/%+.4f/%+.4f/%c",wLen_?",":"",static_cast<unsigned long long>(st),old0,new0,double(sm.rec),double(sm.comp),rbc);
            }
        } else appendf(w_,wLen_,"%s-",wLen_?",":"");
        if(++wFrames_>=(d.n==2?4u:8u))flush_w(log);
    } else if(now>=phaseEnd_-d.settle_ms*0.5)pushIdle(); // late settle frames feed the next reference
    if(now<phaseEnd_)return;
    if(d.family==fam_baseline){finish_baseline(log);r.done=true;}
    else if(!settle_){end_write(mem,sh,log,now);settle_=true;phaseEnd_=now+d.settle_ms;if(d.settle_ms>0)return;}
    ++cur_;
    if(cur_>=ntrials_){finish(sh,log);return;}
    begin_trial(mem,sh,log,now,s);
}

void Experiment::abort(Memory& mem,Shared& sh,Sink& log,const char* why) noexcept {
    if(phase_==done||phase_==aborted||phase_==refused)return;
    const bool wasRunning=phase_==running;
    if(wasRunning&&!settle_&&trials_[cur_].n){flush_w(log);restore(mem,sh,log);results_[cur_].aborted=true;}
    else sh.cmd.disarm();
    phase_=aborted;reason_=why;
    char t[448];
    std::snprintf(t,sizeof t,"ee-camera;event=abort;reason=%s;phase=%s;trial=%u;restore=%s",why,wasRunning?"running":"pre-run",
        wasRunning?cur_:0u,wasRunning?results_[cur_].restore:"none");
    emit(log,true,t);
    finish(sh,log);
}

void Experiment::finish(Shared& sh,Sink& log) noexcept {
    sh.writes_enabled.store(false,std::memory_order_release);
    sh.cmd.disarm();
    for(unsigned spin=0;sh.inflight.load(std::memory_order_seq_cst)!=0&&spin<2000000;++spin){}
    if(phase_==running)phase_=done;
    // Family summaries: seam:verdict(ratio,lag) per completed trial.
    bool anyYaw=false,anyPartial=false,anyDone=false;
    size_t n=0;
    final_[0]=0;
    if(phase_==aborted)appendf(final_,n,"ee-camera;result=ABORTED:%s",reason_);
    else n=0;
    char fam[fam_count][160]{};size_t fl[fam_count]{};
    for(unsigned i=0;i<ntrials_;++i) {
        const TrialResult& r=results_[i];if(!r.done||r.family==fam_baseline)continue;
        anyDone=true;
        if((r.family==fam_yaw_abs||r.family==fam_yaw_delta)&&r.rec.verdict==v_effect)anyYaw=true;
        if(r.rec.verdict==v_partial||r.rec.verdict==v_flicker||r.rec.verdict==v_effect)anyPartial=true;
        auto& f=fam[r.family];auto& l=fl[r.family];
        const bool sameSeam=i>0&&results_[i-1].family==r.family&&results_[i-1].seam==r.seam&&results_[i-1].done;
        if(r.family==fam_yaw_abs||r.family==fam_yaw_delta)
            appendf(f,l,"%s%s:%s(%.2f,%d)",l?",":"",seam_name(r.seam),verdict_name(r.rec.verdict),r.rec.ratio,r.rec.lag);
        else if(sameSeam)appendf(f,l,"/%s",verdict_name(r.rec.verdict));
        else appendf(f,l,"%s%s:%s",l?",":"",seam_name(r.seam),verdict_name(r.rec.verdict));
    }
    if(phase_!=aborted)appendf(final_,n,"ee-camera;result=%s",anyYaw?"LOOK-YAW-INPUT-DRIVES-CAMERA":anyPartial?"PARTIAL":anyDone?"NO-EFFECT":"NO-DATA");
    appendf(final_,n,";look-yaw-abs=%s;look-yaw-delta=%s;pitch=%s;roll=%s;position=%s",fl[fam_yaw_abs]?fam[fam_yaw_abs]:"-",
        fl[fam_yaw_delta]?fam[fam_yaw_delta]:"-",fl[fam_pitch]?fam[fam_pitch]:"-",fl[fam_roll]?fam[fam_roll]:"-",
        opt_.position?(fl[fam_position]?fam[fam_position]:"-"):"off");
    emit(log,true,final_);
    // Evidence line: hypotheses per look-yaw trial and the EE-phase matrix.
    char t[448];size_t m=size_t(std::snprintf(t,sizeof t,"ee-camera;event=evidence;yaw-comp=%s;eye-comp=%s;hyp=",
        yawComp_<0?"minus-heading":yawComp_>0?"plus-heading":"raw",eyeComp_?"minus-air":"raw"));
    bool first=true;
    for(unsigned i=0;i<ntrials_;++i) {
        const TrialResult& r=results_[i];if(!r.done||(r.family!=fam_yaw_abs&&r.family!=fam_yaw_delta))continue;
        appendf(t,m,"%s%s%s:%s",first?"":",",seam_name(r.seam),r.family==fam_yaw_abs?"abs":"delta",r.hyp);first=false;
    }
    appendf(t,m,";ee-phase=");first=true;
    for(uint32_t a=1;a<=3;++a)for(uint32_t b=1;b<=3;++b) {
        const uint32_t v=sh.phase_visits[a][b].load(),c=sh.phase_changes[a][b].load();
        if(!v)continue;appendf(t,m,"%s%s%s:%u/%u",first?"":",",seam_name(a),seam_name(b),c,v);first=false;
    }
    emit(log,true,t);
    sh.base.store(0,std::memory_order_release); // seams inert from now on
}

// ------------------------------------------------------------------ rev 4: candidate timeline + tracking
void Experiment::set_tracks(const ScanResult& r,Sink& log) noexcept {
    char t[448];
    struct Want {uint64_t eff,lo,hi;const char* src;int score;};
    Want want[track_cap];unsigned nw=0;
    static constexpr const char* bestName[3]={"best0","best1","best2"};
    for(int k=0;k<3&&nw<track_cap;++k) {
        const ScanHit& h=r.best[k];
        if(h.score<3||h.offset<field_offset[look_yaw])continue;
        const uint64_t eff=h.offset-field_offset[look_yaw];bool dup=false;
        for(unsigned j=0;j<nw;++j)dup=dup||want[j].eff==eff;
        if(!dup)want[nw++]={eff,h.lo,h.hi?h.hi:h.lo+block_bytes,bestName[k],h.score};
    }
    for(unsigned b=0;b<nblocks_&&nw<track_cap;++b) {
        bool dup=false;for(unsigned j=0;j<nw;++j)dup=dup||want[j].eff==blocks_[b];
        if(!dup)want[nw++]={blocks_[b],blocks_[b],blocks_[b]+block_bytes,"e6-block",-1};
    }
    // Sticky identity (J-look-locator). A scan that does not list a tracked structure counts as ONE miss
    // (the scan's read can land in a look ramp where the strict yaw relation fails); the structure is
    // only evictable after track_missed_scans misses with no passing per-tick structural re-check.
    bool seen[track_cap]{},keep[track_cap]{};
    for(unsigned s=0;s<track_cap;++s) {
        if(!tr_[s].eff)continue;
        for(unsigned j=0;j<nw;++j) {
            if(tr_[s].eff!=want[j].eff)continue;
            if(want[j].score>=3) {                         // listed by THIS scan's ranking
                keep[s]=true;
                // The score keeps its maximum: a scan that read the block inside a look ramp rates it 4 (no yaw
                // relation) and must not strip its protection.
                seen[s]=true;tr_[s].missed=0;tr_[s].structLast=lastNow_;tr_[s].src=want[j].src;
                if(want[j].score>tr_[s].score)tr_[s].score=want[j].score;
            } else if(tr_[s].score<3) {keep[s]=true;tr_[s].src=want[j].src;tr_[s].score=want[j].score;}   // plain E6-block entry
            // (a scored structure that is merely a block base listed as E6 is NOT seen: it stays only by the sticky rule)
        }
        if(!seen[s])++tr_[s].missed;
    }
    for(unsigned s=0;s<track_cap;++s)
        if(tr_[s].eff&&!seen[s]&&sticky_track(tr_[s])&&!evictable_track(tr_[s])) {
            keep[s]=true;
            std::snprintf(t,sizeof t,"ee-camera;event=track-sticky;slot=%u;base=%llx;class=%s;missed-scans=%u;struct-age-ms=%.0f;score=%d",s,
                static_cast<unsigned long long>(tr_[s].eff),classify_track(tr_[s]),tr_[s].missed,lastNow_-tr_[s].structLast,tr_[s].score);
            emit(log,false,t);
        }
    // Observation follows the best-scoring candidate (re-picked every pass) - but a lower-scored entry
    // (a score-4 junk structure) never displaces a held higher-scored one.
    if(r.best[0].score>=3&&r.best[0].offset>=field_offset[look_yaw]) {
        const uint64_t eff=r.best[0].offset-field_offset[look_yaw];
        bool held=false;
        if(bestEff_&&eff!=bestEff_)
            for(unsigned s=0;s<track_cap;++s)if(tr_[s].eff==bestEff_&&keep[s]&&!evictable_track(tr_[s]))held=true;
        if(eff==bestEff_)bestScore_=r.best[0].score;
        else if(held&&r.best[0].score<bestScore_) {
            std::snprintf(t,sizeof t,"ee-camera;event=observe-follow-kept;base=%llx;score=%d;ignored=%llx;ignored-score=%d",static_cast<unsigned long long>(bestEff_),
                bestScore_,static_cast<unsigned long long>(eff),r.best[0].score);
            emit(log,false,t);
        } else {
            bestEff_=eff;bestLo_=r.best[0].lo;bestHi_=r.best[0].hi?r.best[0].hi:r.best[0].lo+block_bytes;bestScore_=r.best[0].score;
            std::snprintf(t,sizeof t,"ee-camera;event=observe-follow;base=%llx;region=%llx..%llx;score=%d",static_cast<unsigned long long>(eff),
                static_cast<unsigned long long>(bestLo_),static_cast<unsigned long long>(bestHi_),bestScore_);
            emit(log,false,t);
        }
    }
    for(unsigned j=0;j<nw;++j) {
        bool present=false;for(unsigned s=0;s<track_cap;++s)present=present||tr_[s].eff==want[j].eff;
        if(present)continue;
        // A free slot first; else the unprotected slot with the lowest score that this entry does not
        // out-rank in the wrong direction (never displace a higher-scored held structure).
        int slot=-1;
        for(unsigned s=0;s<track_cap&&slot<0;++s)if(!keep[s]&&!tr_[s].eff)slot=int(s);
        for(unsigned s=0;s<track_cap;++s) {
            if(keep[s]||!tr_[s].eff)continue;
            const bool free_to_take=evictable_track(tr_[s])||(!sticky_track(tr_[s])&&tr_[s].score<=want[j].score);
            if(free_to_take&&(slot<0||(tr_[slot].eff&&tr_[s].score<tr_[slot].score)))slot=int(s);
        }
        if(slot<0)continue;
        const unsigned s=unsigned(slot);
        if(tr_[s].eff) {
            std::snprintf(t,sizeof t,"ee-camera;event=track-evict;slot=%u;base=%llx;class=%s;missed-scans=%u;struct-age-ms=%.0f;score=%d;by=%llx",s,
                static_cast<unsigned long long>(tr_[s].eff),classify_track(tr_[s]),tr_[s].missed,lastNow_-tr_[s].structLast,tr_[s].score,
                static_cast<unsigned long long>(want[j].eff));
            emit(log,false,t);
        }
        tr_[s]=Track{};tr_[s].eff=want[j].eff;tr_[s].lo=want[j].lo;tr_[s].hi=want[j].hi;tr_[s].src=want[j].src;tr_[s].score=want[j].score;keep[s]=true;
        tr_[s].structLast=lastNow_;
        for(double& c:tr_[s].changed)c=-1;
        std::snprintf(t,sizeof t,"ee-camera;event=timeline-candidate;slot=%u;base=%llx;region=%llx..%llx;src=%s;score=%d",s,
            static_cast<unsigned long long>(tr_[s].eff),static_cast<unsigned long long>(tr_[s].lo),static_cast<unsigned long long>(tr_[s].hi),
            tr_[s].src,tr_[s].score);
        emit(log,false,t);
    }
}
// A structure worth protecting from a single missed scan: it tracks the look, or the scan scored it 5.
bool Experiment::sticky_track(const Track& t) noexcept {
    return t.eff&&(t.score>=5||std::strcmp(classify_track(t),"tracks-look")==0);
}
bool Experiment::evictable_track(const Track& t) const noexcept {
    return t.missed>=track_missed_scans&&lastNow_-t.structLast>=track_struct_stale_ms;
}
const char* Experiment::classify_track(const Track& t) noexcept {
    if(!t.samples)return "no-samples";
    if(!t.anyChg)return "static";
    if(t.lookEv>=2&&t.lookHit>=0.6*t.lookEv)return "tracks-look";
    if(t.airEv>=2&&t.airHit>=0.6*t.airEv&&t.lookHit<=0.3*t.lookEv)return "tracks-aircraft-only";
    if(t.quiet>t.lookHit+t.airHit)return "noise";
    return "changes-unclassified";
}
void Experiment::look_view(uint64_t lockedEff,LookView& out) const noexcept {
    out=LookView{};
    out.cockpit=haveBasis_&&sinceBasis_<=cockpit_ticks;
    out.basis_ok=haveBasis_;out.basis_seq=lastBasisSeq_;
    if(haveBasis_){out.srt_yaw=basis_yaw(basis_);out.srt_pitch=basis_pitch(basis_);out.srt_air_yaw=basis_air_yaw(basis_);}
    const Track* best=nullptr;const Track* locked=nullptr;
    for(const Track& c:tr_) {
        if(!c.eff)continue;
        if(lockedEff&&c.eff==lockedEff){out.locked_present=true;out.locked_lo=c.lo;out.locked_hi=c.hi;locked=&c;}
        if(std::strcmp(classify_track(c),"tracks-look")==0&&(!best||c.lookHit>best->lookHit))best=&c;
    }
    if(best){out.have=true;out.eff=best->eff;out.lo=best->lo;out.hi=best->hi;out.samples=best->samples;out.look_ev=best->lookEv;out.look_hit=best->lookHit;}
    // J-look-locator: how long the look has been quiet, and when the structure last passed the per-tick re-check.
    const Track* subject=locked?locked:best;
    double lastEv=lastLookEv_;
    if(best&&best->lookChgAt>lastEv)lastEv=best->lookChgAt;
    if(locked&&locked->lookChgAt>lastEv)lastEv=locked->lookChgAt;
    out.quiet_ms=lastEv<=-1e299?1e9:lastNow_-lastEv;
    out.struct_age_ms=subject&&subject->bLast>-1e299?lastNow_-subject->bLast:1e9;
}
void Experiment::timeline(Memory& mem,Sink& log,double now) noexcept {
    const bool cockpit=haveBasis_&&sinceBasis_<=cockpit_ticks;
    if(!cockpit||now-lastTl_<timeline_ms)return;
    lastTl_=now;
    char t[448];
    const double camYaw=basis_yaw(basis_),camPitch=basis_pitch(basis_),airYaw=basis_air_yaw(basis_);
    bool camEv=false,airEv=false;
    if(tlHave_) {
        camEv=std::fmax(std::fabs(wrap(camYaw-tlCam_[0])),std::fabs(camPitch-tlCam_[1]))>5*pi/180;
        airEv=basis_.air_ok&&std::fabs(wrap(airYaw-tlAir_))>2*pi/180;
    }
    tlCam_[0]=camYaw;tlCam_[1]=camPitch;tlAir_=airYaw;const bool first=!tlHave_;tlHave_=true;
    const bool lookEv=camEv&&!airEv;
    if(lookEv)lastLookEv_=now;
    static constexpr unsigned angles[]={look_pitch,look_yaw,look_roll,heading,cam_pitch,cam_yaw,cam_roll};
    for(unsigned s=0;s<track_cap;++s) {
        Track& c=tr_[s];if(!c.eff)continue;
        Snapshot snap;if(!read_snapshot(mem,c.eff,snap))continue;
        uint32_t chg=0;bool angChg=false;
        if(c.have)for(unsigned f=0;f<field_count;++f)if(bits(snap.v[f])!=bits(c.last[f])){chg|=1u<<f;c.changed[f]=now;}
        for(unsigned f:angles)angChg=angChg||((chg>>f)&1u);
        if(c.have&&!first) {
            ++c.samples;if(chg)++c.anyChg;
            if(lookEv){++c.lookEv;c.lookHit+=angChg;}
            if(airEv){++c.airEv;c.airHit+=angChg;}
            if(!camEv&&!airEv&&angChg)++c.quiet;
        }
        // Per-tick structural re-check (J-look-locator): the known words of the structure at this base, read
        // twice, with an allowance for the moving look (the composed camera yaw lags the look word by a frame
        // during a ramp). Independent of the 3.7 s scan, which can land in a ramp and miss the block.
        {
            double dl=0;
            if(c.have)for(unsigned f:{look_pitch,look_yaw,look_roll}) {
                const double d=std::fabs(wrap(double(snap.v[f])-double(c.last[f])));
                if(d>track_look_chg_rad)c.lookChgAt=now;
                if(f==look_yaw)dl=d;
            }
            Snapshot snap2;const bool ok2=read_snapshot(mem,c.eff,snap2);
            if(ok2)dl=std::fmax(dl,std::fabs(wrap(double(snap2.v[look_yaw])-double(snap.v[look_yaw]))));
            const double allow=std::fmin(track_yaw_allow_cap,1.5*dl);
            const bool needEye=c.score>=5&&haveBasis_;
            const bool pass=structure_holds(snap.v,needEye?&basis_:nullptr,allow)||(ok2&&structure_holds(snap2.v,needEye?&basis_:nullptr,allow));
            if(pass){c.structLast=now;c.bLast=now;++c.structPass;}else ++c.structFail;
            if(c.structSeen&&pass!=c.structOk) {
                std::snprintf(t,sizeof t,"ee-camera;event=track-structure;slot=%u;base=%llx;ok=%u;passes=%u;fails=%u;L=%.6g;allow=%.4f",s,
                    static_cast<unsigned long long>(c.eff),unsigned(pass),c.structPass,c.structFail,double(snap.v[look_yaw]),allow);
                emit(log,false,t);
            }
            c.structOk=pass;c.structSeen=true;
        }
        std::memcpy(c.last,snap.v,sizeof c.last);c.have=true;
        if(chg||now-c.lastLine>=timeline_heartbeat_ms) {
            c.lastLine=now;const float* v=snap.v;
            std::snprintf(t,sizeof t,"ee-camera;event=timeline;slot=%u;base=%llx;src=%s;chg=%04x;ev=%s;L=%.6g,%.6g,%.6g;H=%.6g;air=%.7g,%.7g,%.7g;"
                "cam=%.7g,%.7g,%.7g;rot=%.6g,%.6g,%.6g;copy=%.6g;srt-yaw=%.4f;srt-pitch=%.4f;srt-air-yaw=%.4f;eye=%.5g,%.5g,%.5g",s,
                static_cast<unsigned long long>(c.eff),c.src,chg,lookEv?"look":airEv?"aircraft":camEv?"camera":"-",v[look_pitch],v[look_yaw],v[look_roll],v[heading],
                v[air_x],v[air_y],v[air_z],v[cam_x],v[cam_y],v[cam_z],v[cam_pitch],v[cam_yaw],v[cam_roll],v[look_yaw_copy],camYaw,camPitch,airYaw,
                basis_.eye[0],basis_.eye[1],basis_.eye[2]);
            emit(log,false,t);
        }
    }
    if(now-lastTrackLog_>=tracking_log_ms) {
        lastTrackLog_=now;unsigned counts[6]{};
        static constexpr const char* names[6]={"tracks-look","tracks-aircraft-only","static","noise","changes-unclassified","no-samples"};
        for(unsigned s=0;s<track_cap;++s) {
            const Track& c=tr_[s];if(!c.eff)continue;const char* cls=classify_track(c);
            for(unsigned k=0;k<6;++k)if(std::strcmp(cls,names[k])==0)++counts[k];
            size_t n=0;
            appendf(t,n,"ee-camera;event=tracking;slot=%u;base=%llx;src=%s;class=%s;look=%u/%u;aircraft=%u/%u;quiet-changes=%u;changed-samples=%u/%u;last-change-age-ms=",
                s,static_cast<unsigned long long>(c.eff),c.src,cls,c.lookHit,c.lookEv,c.airHit,c.airEv,c.quiet,c.anyChg,c.samples);
            for(unsigned f=0;f<field_count;++f)appendf(t,n,f?",%.0f":"%.0f",c.changed[f]<0?-1.0:now-c.changed[f]);
            appendf(t,n,";struct-ok=%u/%u;missed-scans=%u",c.structPass,c.structPass+c.structFail,c.missed);
            emit(log,false,t);
        }
        size_t n=0;appendf(t,n,"ee-camera;event=tracking-summary");
        for(unsigned k=0;k<6;++k)appendf(t,n,";%s=%u",names[k],counts[k]);
        emit(log,false,t);
    }
}

} // namespace acz::ee_camera
