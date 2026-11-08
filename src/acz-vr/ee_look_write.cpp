// Option 3 stage 2a/2b look-write state machines (see ee_look_write.h and J-look-write-findings.md).
#include "ee_look_write.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace acz::ee_camera {
namespace {
inline unsigned long long ull(uint64_t v) noexcept {return static_cast<unsigned long long>(v);}
bool fin(float f) noexcept {return std::isfinite(f);}
float clampf(float v,float lo,float hi) noexcept {return v<lo?lo:(v>hi?hi:v);}
// The fields the writer/drive ever touch; all must lie inside the region before anything starts.
constexpr unsigned used_fields[]={look_pitch,look_yaw,heading,cam_pitch,cam_yaw,look_yaw_copy,air_x,air_y,air_z};
}

float head_to_units(double head_rad,double units_per_rad,float lo,float hi) noexcept {
    if(!std::isfinite(head_rad)||!std::isfinite(units_per_rad)||units_per_rad<=0)return 0.f;
    const double a=std::fabs(head_rad);
    if(a<drive_deadzone_rad)return 0.f;
    const double d=(a-drive_deadzone_rad)*(head_rad<0?-1.0:1.0);   // continuous through the deadzone edge
    return clampf(float(d*units_per_rad),lo,hi);
}
float rate_limit(float from,float to,float max_step) noexcept {
    if(!fin(from)||!fin(to))return 0.f;
    const float d=to-from;
    return d>max_step?from+max_step:(d<-max_step?from-max_step:to);
}
bool head_yaw_pitch(const double R[9],double yaw0,double& yaw,double& pitch) noexcept {
    for(int i=0;i<9;++i)if(!std::isfinite(R[i]))return false;
    const double fx=-R[2],fy=-R[5],fz=-R[8];
    if(std::hypot(fx,fz)<1e-6||std::fabs(fy)>1.0+1e-6||!std::isfinite(yaw0))return false;
    yaw=wrap(std::atan2(-fx,-fz)-yaw0);
    pitch=std::asin(fy>1?1:(fy<-1?-1:fy));
    return true;
}

// =================================================================== stage 2a
LookWriter::LookWriter(Options o) noexcept:o_(o) {
    phase_=o.enabled?p_waiting:p_off;
    if(o_.frames<1)o_.frames=1;
    if(o_.frames>look_trial_cap)o_.frames=look_trial_cap;
    if(!fin(o_.yaw))o_.yaw=look_write_default;
    o_.yaw=clampf(o_.yaw,-look_write_clamp,look_write_clamp);
    target_=o_.yaw;
}
bool LookWriter::rd(Memory& m,unsigned f,float& out) const noexcept {
    const uint64_t a=eff_+field_offset[f];
    return in_block(a)&&m.read(a,&out,4);
}
bool LookWriter::still(Memory& m) noexcept {
    uint32_t b[3]={};float v;
    for(int i=0;i<3;++i){if(!rd(m,air_x+i,v)){stillTicks_=0;haveAir_=false;return false;}b[i]=bits(v);}
    const bool same=haveAir_&&b[0]==lastAir_[0]&&b[1]==lastAir_[1]&&b[2]==lastAir_[2];
    stillTicks_=same?stillTicks_+1:0;std::memcpy(lastAir_,b,sizeof b);haveAir_=true;
    return stillTicks_>=look_still_ticks;
}
void LookWriter::wait_log(Sink& log,double now,const char* why) noexcept {
    if(std::strcmp(why,lastWhy_)==0&&now-lastWait_<look_wait_log_ms)return;
    lastWhy_=why;lastWait_=now;
    char t[256];std::snprintf(t,sizeof t,"ee-camera;event=look-write-wait;reason=%s;key=ee_camera_look_write_trial",why);
    log.line(false,t);
}
bool LookWriter::restore(Memory& m,Sink& log,const char* why) noexcept {
    char t[384];
    const uint64_t a=eff_+field_offset[look_yaw];
    float rb=std::nanf("");bool ok=false;unsigned tries=0;
    if(in_block(a))
        for(;tries<3&&!ok;++tries) {
            const bool w=m.write_float(a,orig_);
            ok=w&&m.read(a,&rb,4)&&bits(rb)==bits(orig_);
        }
    res_.restored=ok;res_.final_value=rb;if(ok)dirty_=false;
    std::snprintf(t,sizeof t,"ee-camera;event=look-restore;ok=%u;why=%s;addr=%llx;orig=%.9g;readback=%.9g;attempts=%u;writes=%u",unsigned(ok),why,
        ull(a),double(orig_),double(rb),tries,res_.writes);
    log.line(true,t);
    return ok;
}
void LookWriter::abort_and_restore(Memory& m,Sink& log,const char* why) noexcept {
    if(phase_==p_off||phase_==p_done||phase_==p_aborted) {
        // A previous restore may have failed: try again once on the final exit path.
        if(dirty_)restore(m,log,why);
        return;
    }
    res_.aborted=true;res_.reason=why;
    if(dirty_)restore(m,log,why);
    char t[320];
    std::snprintf(t,sizeof t,"ee-camera;event=look-write-result;result=aborted;reason=%s;writes=%u;restored=%u;attempted=%u",why,res_.writes,
        unsigned(res_.restored||!dirty_),unsigned(res_.attempted));
    log.line(true,t);
    phase_=p_aborted;
}
void LookWriter::analyse() noexcept {
    const unsigned n=res_.writes;
    res_.persisted=n>1?1.0-double(res_.overwritten)/double(n-1):0.0;
    const double d=double(target_)-double(orig_);
    res_.lag_ticks=-1;res_.follow=0;res_.comp_gain=0;res_.srt_gain=0;res_.srt_valid=false;res_.responded=false;
    if(n<8||std::fabs(d)<1e-6)return;
    for(unsigned i=1;i<n;++i)
        if(std::fabs(wrap(double(s_[i].comp)-double(baseComp_)))>=0.5*std::fabs(d)){res_.lag_ticks=int(i);break;}
    if(res_.lag_ticks<0)return;
    const double tol=std::fmax(0.02,0.1*std::fabs(d));unsigned ok=0,cnt=0;
    for(unsigned i=unsigned(res_.lag_ticks);i<n;++i){++cnt;ok+=std::fabs(wrap(double(s_[i].comp)-double(baseComp_))-d)<=tol;}
    res_.follow=cnt?double(ok)/cnt:0;
    double sc=0,ss=0;unsigned nc=0,ns=0;
    for(unsigned i=n/2;i<n;++i) {
        sc+=wrap(double(s_[i].comp)-double(baseComp_))/d;++nc;
        if(s_[i].srt_ok&&baseSrtOk_&&s_[i].basis!=baseBasis_){ss+=wrap(s_[i].srt-baseSrt_)/d;++ns;}
    }
    res_.comp_gain=nc?sc/nc:0;
    if(ns>=10){res_.srt_gain=ss/ns;res_.srt_valid=true;}
    res_.responded=res_.follow>=0.8;
}
bool LookWriter::calibration(double& upr,const char** source) const noexcept {
    if(!succeeded())return false;
    if(res_.srt_valid&&std::fabs(res_.srt_gain)>0.1&&std::fabs(res_.srt_gain)<10){upr=1.0/std::fabs(res_.srt_gain);if(source)*source="recovered-camera";return true;}
    if(std::fabs(res_.comp_gain)>0.1&&std::fabs(res_.comp_gain)<10){upr=1.0/std::fabs(res_.comp_gain);if(source)*source="composed-field";return true;}
    return false;
}
void LookWriter::finish(Sink& log,const char* result,const char* reason) noexcept {
    char t[512];
    std::snprintf(t,sizeof t,"ee-camera;event=look-write-result;result=%s;reason=%s;writes=%u;written=%.6g;orig=%.9g;final=%.9g;restored=%u;"
        "persisted=%.3f;overwritten=%u;composed-lag-ticks=%d;composed-follow=%.3f;composed-gain=%.4f;srt-gain-rad-per-unit=%.4f;srt-valid=%u;responded=%u",
        result,reason,res_.writes,double(target_),double(orig_),double(res_.final_value),unsigned(res_.restored),res_.persisted,res_.overwritten,
        res_.lag_ticks,res_.follow,res_.comp_gain,res_.srt_gain,unsigned(res_.srt_valid),unsigned(res_.responded));
    log.line(true,t);
    res_.reason=reason;
}

void LookWriter::tick(Memory& m,Sink& log,const LookIn& in) noexcept {
    if(phase_==p_off||phase_==p_done||phase_==p_aborted)return;
    if(in.frame==lastFrame_)return;
    lastFrame_=in.frame;
    char t[640];
    const LookView& v=in.v;
    if(phase_==p_waiting) {
        const char* why=nullptr;
        if(in.stop_file)why="kill-switch-file";
        else if(!v.have)why="no-tracks-look-candidate";
        else if(v.samples<look_min_samples||v.look_ev<look_min_events||v.look_hit*5<v.look_ev*4)why="candidate-evidence-weak";
        else if(!v.cockpit)why="no-cockpit";
        else if(v.hi<=v.lo)why="bad-region";
        else if(v.struct_age_ms>look_struct_max_age_ms)why="structure-unconfirmed";
        else if(v.quiet_ms<look_quiet_ms)why="look-event-recent";
        if(!why) {
            eff_=v.eff;lo_=v.lo;hi_=v.hi;
            for(unsigned f:used_fields){const uint64_t a=eff_+field_offset[f];if(!in_block(a)){why="fields-outside-region";break;}}
            float y=0,p=0;
            if(!why&&(!rd(m,look_yaw,y)||!rd(m,look_pitch,p)))why="read-failure";
            else if(!why&&(!fin(y)||std::fabs(y)>look_neutral_band))why="look-not-neutral";
            else if(!why&&(!fin(p)||std::fabs(p)>look_pitch_neutral_band))why="look-pitch-not-neutral";
            if(why)eff_=0;
        }
        if(why){wait_log(log,in.now_ms,why);return;}
        haveAir_=false;stillTicks_=0;settleStart_=in.now_ms;settleHeld_=false;settleRestarts_=0;phase_=p_settling;lastWhy_="";
        std::snprintf(t,sizeof t,"ee-camera;event=look-write-settle;base=%llx;region=%llx..%llx;samples=%u;look=%u/%u;settle-ms=%.0f;tid=%u",ull(eff_),ull(lo_),ull(hi_),
            v.samples,v.look_hit,v.look_ev,o_.settle_ms,in.tid);
        log.line(true,t);
        return;
    }
    if(phase_==p_settling) {
        const char* why=nullptr;float y=0,h=0,pp=0;
        if(in.stop_file)why="kill-switch-file";
        else if(!v.locked_present||v.locked_lo!=lo_||v.locked_hi!=hi_)why="identity-changed";
        else if(!v.cockpit)why="no-cockpit";
        else if(v.struct_age_ms>look_struct_max_age_ms)why="structure-unconfirmed";
        else if(!rd(m,look_yaw,y)||!rd(m,heading,h)||!rd(m,look_pitch,pp))why="read-failure";
        else if(!fin(y)||std::fabs(y)>look_neutral_band)why="look-not-neutral";
        else if(!fin(pp)||std::fabs(pp)>look_pitch_neutral_band)why="look-pitch-not-neutral";
        else if(still(m))why="paused";
        if(why) {
            std::snprintf(t,sizeof t,"ee-camera;event=look-write-settle-reset;reason=%s",why);log.line(false,t);
            phase_=p_waiting;eff_=0;lastWhy_=why;lastWait_=in.now_ms;return;
        }
        // A look key event (the probe saw the camera/look words move) restarts the settle: not an abort.
        if(v.quiet_ms<look_quiet_ms) {
            settleStart_=in.now_ms;++settleRestarts_;
            if(!settleHeld_){
                settleHeld_=true;
                std::snprintf(t,sizeof t,"ee-camera;event=look-write-settle-restart;reason=look-event;quiet-ms=%.0f",v.quiet_ms);log.line(false,t);
            }
            return;
        }
        settleHeld_=false;
        // The settle is at least look_quiet_ms of continuous quiet (yaw+pitch neutral, no look event).
        if(in.now_ms-settleStart_<(o_.settle_ms>look_quiet_ms?o_.settle_ms:look_quiet_ms))return;
        // Begin the trial. The original is the value just read (bit-exact, neutral).
        float cy=0;
        if(!rd(m,cam_yaw,cy)||!fin(cy)||!fin(h)){phase_=p_waiting;eff_=0;wait_log(log,in.now_ms,"read-failure");return;}
        orig_=y;res_.attempted=true;res_.orig=orig_;res_.written=target_;res_.reason="running";
        baseComp_=float(wrap(double(cy)-double(h)));baseSrtOk_=v.basis_ok;baseSrt_=v.srt_yaw;baseBasis_=v.basis_seq;
        heading0_=h;trialStart_=in.now_ms;n_=0;
        std::snprintf(t,sizeof t,"ee-camera;event=look-write-begin;base=%llx;addr=%llx;orig=%.9g;target=%.6g;clamp=%.2f;frames=%u;deadline-ms=%.0f;"
            "readback-limit=%.2f;fields=look-yaw-only;kill-switch=look-write.stop;tid=%u",ull(eff_),ull(eff_+field_offset[look_yaw]),double(orig_),
            double(target_),double(look_write_clamp),o_.frames,o_.deadline_ms,double(look_readback_limit),in.tid);
        log.line(true,t);
        phase_=p_writing;      // the first write happens in this same tick
    }
    if(phase_==p_writing) {
        const char* why=nullptr;
        float pre=0,cy=0,cp=0,hd=0,cpy=0,lp=0;
        if(in.stop_file)why="kill-switch-file";
        else if(in.now_ms-trialStart_>o_.deadline_ms)why="deadline";
        else if(!v.locked_present||v.locked_lo!=lo_||v.locked_hi!=hi_)why="identity-changed";
        else if(!v.cockpit)why="cockpit-lost";
        else if(!rd(m,look_yaw,pre)||!rd(m,cam_yaw,cy)||!rd(m,cam_pitch,cp)||!rd(m,heading,hd)||!rd(m,look_yaw_copy,cpy)||!rd(m,look_pitch,lp))why="read-failure";
        else if(!fin(pre)||!fin(cy)||!fin(cp)||!fin(hd)||!fin(cpy)||!fin(lp))why="nonfinite-field";
        else if(std::fabs(pre)>look_readback_limit)why="readback-out-of-bounds";
        else if(std::fabs(wrap(double(hd)-double(heading0_)))>0.5)why="heading-changed";
        else if(still(m))why="paused";
        else if(!(std::fabs(target_)<=look_write_clamp))why="value-out-of-bounds";
        if(why){abort_and_restore(m,log,why);return;}
        const uint64_t addr=eff_+field_offset[look_yaw];
        dirty_=true;   // from here on a restore is owed, even if the store faults half way
        if(!m.write_float(addr,target_)){abort_and_restore(m,log,"write-failure");return;}
        ++n_;++res_.writes;
        const unsigned ovw=n_>=2&&bits(pre)!=bits(prevNew_)?1u:0u;res_.overwritten+=ovw;prevNew_=target_;
        float rb=0,rcy=0;
        if(!rd(m,look_yaw,rb)||!rd(m,cam_yaw,rcy)){abort_and_restore(m,log,"read-failure");return;}
        Sample& s=s_[n_-1];
        s.pre=pre;s.written=target_;s.comp=float(wrap(double(cy)-double(hd)));s.srt=v.srt_yaw;s.srt_ok=v.basis_ok;s.basis=v.basis_seq;
        std::snprintf(t,sizeof t,"ee-camera;event=look-write;n=%u;ms=%.2f;frame=%llu;tid=%u;addr=%llx;old=%.9g;new=%.9g;rb=%.9g;ovw=%u;"
            "pre-cam-yaw=%.6g;rb-cam-yaw=%.6g;cam-pitch=%.6g;heading=%.6g;copy=%.6g;look-pitch=%.6g;srt-yaw=%.5f;srt-air-yaw=%.5f;basis=%llu",
            n_,in.now_ms-trialStart_,ull(in.frame),in.tid,ull(addr),double(pre),double(target_),double(rb),ovw,double(cy),double(rcy),double(cp),
            double(hd),double(cpy),double(lp),v.srt_yaw,v.srt_air_yaw,ull(v.basis_seq));
        log.line(false,t);
        if(n_>=o_.frames) {
            if(!restore(m,log,"trial-complete")){abort_and_restore(m,log,"restore-failed");return;}
            phase_=p_post;post_=0;
        }
        return;
    }
    if(phase_==p_post) {
        float y=std::nanf(""),cy=std::nanf(""),hd=std::nanf("");
        const bool ok=rd(m,look_yaw,y)&&rd(m,cam_yaw,cy)&&rd(m,heading,hd);
        std::snprintf(t,sizeof t,"ee-camera;event=look-post;n=%u;read=%u;field=%.9g;cam-yaw=%.6g;heading=%.6g;srt-yaw=%.5f;frame=%llu;tid=%u",post_+1,unsigned(ok),
            double(y),double(cy),double(hd),v.srt_yaw,ull(in.frame),in.tid);
        log.line(false,t);
        if(++post_>=look_post_frames) {
            analyse();
            finish(log,res_.responded?"ok":"no-response",res_.responded?"field-responded":"composed-camera-did-not-follow");
            phase_=p_done;
        }
    }
}

// =================================================================== stage 2b
bool LookDrive::rd(Memory& m,unsigned f,float& out) const noexcept {
    const uint64_t a=eff_+field_offset[f];
    return in_block(a)&&m.read(a,&out,4);
}
void LookDrive::wait_log(Sink& log,double now,const char* why) noexcept {
    if(std::strcmp(why,lastWhy_)==0&&now-lastWait_<look_wait_log_ms)return;
    lastWhy_=why;lastWait_=now;
    char t[256];std::snprintf(t,sizeof t,"ee-camera;event=look-drive-wait;reason=%s;key=ee_camera_look_drive",why);
    log.line(false,t);
}
bool LookDrive::restore(Memory& m,Sink& log,const char* why) noexcept {
    char t[384];bool all=true;float rb[2]{std::nanf(""),std::nanf("")};
    const unsigned fields[2]={look_yaw,look_pitch};
    for(int i=0;i<2;++i) {
        const uint64_t a=eff_+field_offset[fields[i]];bool ok=false;
        if(in_block(a))for(unsigned k=0;k<3&&!ok;++k)ok=m.write_float(a,orig_[i])&&m.read(a,&rb[i],4)&&bits(rb[i])==bits(orig_[i]);
        all=all&&ok;
    }
    if(all)dirty_=false;
    std::snprintf(t,sizeof t,"ee-camera;event=look-drive-restore;ok=%u;why=%s;yaw-orig=%.9g;yaw-rb=%.9g;pitch-orig=%.9g;pitch-rb=%.9g;writes=%u",unsigned(all),why,
        double(orig_[0]),double(rb[0]),double(orig_[1]),double(rb[1]),writes_);
    log.line(true,t);
    return all;
}
void LookDrive::abort_and_restore(Memory& m,Sink& log,const char* why) noexcept {
    if(phase_==p_active){reason_=why;if(dirty_)restore(m,log,why);phase_=p_aborted;
        char t[256];std::snprintf(t,sizeof t,"ee-camera;event=look-drive-result;result=aborted;reason=%s;writes=%u;restored=%u",why,writes_,unsigned(!dirty_));
        log.line(true,t);return;}
    if(phase_==p_waiting){reason_=why;phase_=p_aborted;return;}
    if(dirty_)restore(m,log,why);   // a failed earlier restore is retried on the final exit path
}
void LookDrive::tick(Memory& m,Sink& log,const DriveIn& d) noexcept {
    if(phase_==p_off||phase_==p_done||phase_==p_aborted)return;
    const LookIn& in=d.base;const LookView& v=in.v;
    if(in.frame==lastFrame_)return;
    lastFrame_=in.frame;
    char t[512];
    const bool headFresh=d.head.valid&&d.head.age_ms<=drive_head_stale_ms&&std::isfinite(d.head.yaw)&&std::isfinite(d.head.pitch);
    if(phase_==p_waiting) {
        const char* why=nullptr;
        if(in.stop_file)why="kill-switch-file";
        else if(!d.gate_ok)why="stage-2a-not-succeeded";
        else if(!v.have||v.eff!=d.gate_eff)why="candidate-differs-from-2a";
        else if(!v.cockpit)why="no-cockpit";
        else if(!headFresh)why="no-head-source";
        else if(v.hi<=v.lo)why="bad-region";
        float y=0,p=0;
        if(!why){
            eff_=v.eff;lo_=v.lo;hi_=v.hi;
            for(unsigned f:used_fields){const uint64_t a=eff_+field_offset[f];if(!in_block(a)){why="fields-outside-region";break;}}
            if(!why&&(!rd(m,look_yaw,y)||!rd(m,look_pitch,p)))why="read-failure";
            else if(!why&&(!fin(y)||!fin(p)||std::fabs(y)>look_neutral_band||std::fabs(p)>2.5f*look_neutral_band))why="look-not-neutral";
            if(why)eff_=0;
        }
        if(why){wait_log(log,in.now_ms,why);return;}
        orig_[0]=y;orig_[1]=p;last_[0]=y;last_[1]=p;start_=in.now_ms;haveAir_=false;still_=0;
        phase_=p_active;reason_="running";
        std::snprintf(t,sizeof t,"ee-camera;event=look-drive-begin;base=%llx;region=%llx..%llx;yaw-orig=%.9g;pitch-orig=%.9g;units-per-rad=%.4f;calibrated=%u;"
            "yaw-clamp=%.2f;pitch=[%.2f,%.2f];rate-per-tick=%.3f;deadzone-rad=%.4f;max-ms=%.0f;tid=%u",ull(eff_),ull(lo_),ull(hi_),double(y),double(p),
            d.units_per_rad,unsigned(d.calibrated),double(drive_yaw_clamp),double(drive_pitch_down),double(drive_pitch_up),double(drive_rate_per_tick),
            double(drive_deadzone_rad),o_.max_ms,in.tid);
        log.line(true,t);
    }
    if(phase_!=p_active)return;
    const char* why=nullptr;bool normalEnd=false;
    float py=0,pp=0;
    if(in.stop_file)why="kill-switch-file";
    else if(in.now_ms-start_>o_.max_ms){why="duration-cap";normalEnd=true;}
    else if(!v.locked_present||v.locked_lo!=lo_||v.locked_hi!=hi_)why="identity-changed";
    else if(!v.cockpit)why="cockpit-lost";
    else if(!d.gate_ok)why="stage-2a-gate-lost";
    else if(!rd(m,look_yaw,py)||!rd(m,look_pitch,pp))why="read-failure";
    else if(!fin(py)||!fin(pp))why="nonfinite-field";
    else if(py>drive_yaw_clamp+drive_bound_margin||py<-drive_yaw_clamp-drive_bound_margin||pp>drive_pitch_up+drive_bound_margin||pp<drive_pitch_down-drive_bound_margin)
        why="readback-out-of-bounds";
    else {
        uint32_t b[3]={};float a=0;bool ok=true;
        for(int i=0;i<3&&ok;++i){ok=rd(m,air_x+i,a);b[i]=bits(a);}
        const bool same=ok&&haveAir_&&b[0]==lastAir_[0]&&b[1]==lastAir_[1]&&b[2]==lastAir_[2];
        still_=same?still_+1:0;std::memcpy(lastAir_,b,sizeof b);haveAir_=ok;
        if(!ok)why="read-failure";else if(still_>=look_still_ticks)why="paused";
    }
    if(why) {
        if(!normalEnd){abort_and_restore(m,log,why);return;}
        reason_=why;if(dirty_)restore(m,log,why);
        std::snprintf(t,sizeof t,"ee-camera;event=look-drive-result;result=done;reason=%s;writes=%u;restored=%u",why,writes_,unsigned(!dirty_));
        log.line(true,t);phase_=p_done;return;
    }
    float ty=0,tp=0;
    if(headFresh) {
        ty=head_to_units(d.head.yaw,d.units_per_rad,-drive_yaw_clamp,drive_yaw_clamp);
        tp=head_to_units(d.head.pitch,d.units_per_rad,drive_pitch_down,drive_pitch_up);
    } else {ty=orig_[0];tp=orig_[1];}   // head source stale: relax to the original (neutral) value, rate limited
    const float ny=clampf(rate_limit(last_[0],ty,drive_rate_per_tick),-drive_yaw_clamp,drive_yaw_clamp);
    const float np=clampf(rate_limit(last_[1],tp,drive_rate_per_tick),drive_pitch_down,drive_pitch_up);
    const uint64_t ay=eff_+field_offset[look_yaw],ap=eff_+field_offset[look_pitch];
    dirty_=true;
    if(!m.write_float(ay,ny)){abort_and_restore(m,log,"write-failure");return;}
    if(!m.write_float(ap,np)){abort_and_restore(m,log,"write-failure");return;}
    last_[0]=ny;last_[1]=np;++writes_;
    if(writes_<=drive_log_first||writes_%drive_log_every==0) {
        std::snprintf(t,sizeof t,"ee-camera;event=look-drive;n=%u;ms=%.2f;frame=%llu;tid=%u;head=%.5f,%.5f;fresh=%u;old=%.6g,%.6g;new=%.6g,%.6g;srt-yaw=%.5f",
            writes_,in.now_ms-start_,ull(in.frame),in.tid,d.head.yaw,d.head.pitch,unsigned(headFresh),double(py),double(pp),double(ny),double(np),v.srt_yaw);
        log.line(false,t);
    }
}

} // namespace acz::ee_camera
