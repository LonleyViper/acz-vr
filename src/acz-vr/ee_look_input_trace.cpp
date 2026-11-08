// Option 3 stage 2 follow-up (D-269): READ-ONLY bounded look-input trace. See ee_look_input_trace.h.
#include "ee_look_input_trace.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace acz::ee_camera {
namespace {
// The file format is little endian; this module only builds for x64 Windows (and the x64 fixtures).
template <class T> void put(uint8_t*& p,T v) noexcept {std::memcpy(p,&v,sizeof v);p+=sizeof v;}
template <class T> T get(const uint8_t*& p) noexcept {T v;std::memcpy(&v,p,sizeof v);p+=sizeof v;return v;}
constexpr char trace_magic[8]={'A','C','Z','L','I','T','1','\0'};
}

LookInputTrace::LookInputTrace(Options o) noexcept:o_(o),prev_(bufA_),cur_(bufB_) {
    if(o_.max_windows>trace_max_windows)o_.max_windows=trace_max_windows;
    if(!(o_.window_ms>0)||o_.window_ms>10000)o_.window_ms=trace_window_ms;
    if(!(o_.preroll_ms>=0)||o_.preroll_ms>5000)o_.preroll_ms=trace_preroll_ms;
}

bool LookInputTrace::moved_from_zero(float prev,float cur,float band) noexcept {
    if(!std::isfinite(prev)||!std::isfinite(cur))return false;
    return std::fabs(prev)<=band&&std::fabs(cur)>std::fabs(prev)&&std::fabs(cur-prev)>trace_move_eps;
}

void LookInputTrace::wait_log(Sink& log,double now,const char* why) noexcept {
    if(why==lastWhy_&&now-lastWait_<trace_wait_log_ms)return;
    lastWait_=now;lastWhy_=why;
    char t[224];std::snprintf(t,sizeof t,"ee-camera;event=look-input-trace-wait;reason=%s;key=ee_camera_look_input_trace",why);
    log.line(false,t);
}

// ------------------------------------------------------------------ ring of delta records (pre-roll)
void LookInputTrace::ring_copy(size_t pos,uint8_t* dst,size_t n) const noexcept {
    size_t first=trace_ring_bytes-pos;if(first>n)first=n;
    std::memcpy(dst,ring_+pos,first);
    if(n>first)std::memcpy(dst+first,ring_,n-first);
}
void LookInputTrace::ring_push(const uint8_t* hdr,const uint8_t* deltas,size_t dbytes,double ms) noexcept {
    const size_t bytes=trace_tick_header_bytes+dbytes;
    // Older than the pre-roll horizon, or no room: drop from the front.
    while(mcount_&&(mq_[mhead_].ms<ms-o_.preroll_ms||mcount_>=trace_ring_ticks||rused_+bytes>trace_ring_bytes))ring_pop();
    if(bytes>trace_ring_bytes)return;
    const size_t pos=rhead_;
    size_t first=trace_ring_bytes-pos;if(first>trace_tick_header_bytes)first=trace_tick_header_bytes;
    std::memcpy(ring_+pos,hdr,first);
    if(first<trace_tick_header_bytes)std::memcpy(ring_,hdr+first,trace_tick_header_bytes-first);
    const size_t p2=(pos+trace_tick_header_bytes)%trace_ring_bytes;
    if(dbytes) {
        size_t f2=trace_ring_bytes-p2;if(f2>dbytes)f2=dbytes;
        std::memcpy(ring_+p2,deltas,f2);
        if(f2<dbytes)std::memcpy(ring_,deltas+f2,dbytes-f2);
    }
    rhead_=(pos+bytes)%trace_ring_bytes;rused_+=bytes;
    Meta& m=mq_[(mhead_+mcount_)%trace_ring_ticks];m.pos=pos;m.bytes=uint32_t(bytes);m.ms=ms;++mcount_;
}

// ------------------------------------------------------------------ window
// Marks the changed words of one tick record (already inside the arena) and counts them.
void LookInputTrace::account(const uint8_t* rec) noexcept {
    const uint8_t* p=rec+16+8;                      // after u32 idx, u32 tid, f64 ms, u64 frame
    const uint32_t ns=get<uint32_t>(p),nt=get<uint32_t>(p);
    for(uint32_t i=0;i<ns;++i) {
        const uint32_t off=get<uint32_t>(p);p+=8;
        const uint32_t w=off>>2;
        if(w<trace_words)bitmap_[w>>5]|=1u<<(w&31);
    }
    winDeltas_+=ns;winTrunc_+=nt>ns?nt-ns:0;
}
bool LookInputTrace::arena_put(const uint8_t* hdr,const uint8_t* deltas,size_t dbytes) noexcept {
    const size_t need=trace_tick_header_bytes+dbytes;
    if(w_+need>segEnd_)return false;
    std::memcpy(arena_+w_,hdr,trace_tick_header_bytes);
    if(dbytes)std::memcpy(arena_+w_+trace_tick_header_bytes,deltas,dbytes);
    account(arena_+w_);
    w_+=need;++winTicks_;
    return true;
}
bool LookInputTrace::open_window(Sink& log,uint32_t reason,float yaw,float pitch,double quiet,const LookIn& in,uint32_t tickIdx) noexcept {
    const size_t remaining=trace_arena_bytes-arenaUsed_;
    const unsigned left=o_.max_windows>started_?o_.max_windows-started_:1;
    const size_t budget=remaining/left;       // an unused share flows to the later windows
    if(budget<trace_min_window_bytes) {
        exhausted_=true;
        char t[160];std::snprintf(t,sizeof t,"ee-camera;event=look-input-trace-exhausted;reason=budget;arena-left=%llu",(unsigned long long)remaining);
        log.line(true,t);return false;
    }
    segStart_=arenaUsed_;segEnd_=segStart_+budget;w_=segStart_+trace_header_bytes;
    winIndex_=started_++;winTicks_=0;winDeltas_=0;winTrunc_=0;winBudget_=false;winReason_=reason;winTrigger_=tickIdx;
    winOpenMs_=in.now_ms;winLastMs_=in.now_ms;
    std::memset(bitmap_,0,sizeof bitmap_);
    // Pre-roll: everything in the ring (which already holds this tick), oldest dropped first if the share is too small.
    size_t total=0;for(unsigned i=0;i<mcount_;++i)total+=mq_[(mhead_+i)%trace_ring_ticks].bytes;
    while(mcount_>1&&total>segEnd_-w_){total-=mq_[mhead_].bytes;ring_pop();}
    while(mcount_) {
        const Meta m=mq_[mhead_];
        if(w_+m.bytes<=segEnd_) {ring_copy(m.pos,arena_+w_,m.bytes);account(arena_+w_);w_+=m.bytes;++winTicks_;}
        ring_pop();
    }
    ring_clear();
    winPre_=winTicks_>0?winTicks_-1:0;
    open_=true;armed_=false;
    char t[256];
    std::snprintf(t,sizeof t,"ee-camera;event=look-input-trace-open;n=%u;base=%llx;reason=%s;yaw=%.6g;pitch=%.6g;quiet-ms=%.0f;preroll-ticks=%u;budget=%llu",
        winIndex_+1,(unsigned long long)base_,trace_open_name(reason),double(yaw),double(pitch),quiet>1e8?-1.0:quiet,winPre_,(unsigned long long)budget);
    log.line(false,t);
    return true;
}
void LookInputTrace::close_window(Sink& log,uint32_t reason) noexcept {
    if(!open_)return;
    open_=false;
    unsigned changed=0;
    for(uint32_t w:bitmap_){while(w){w&=w-1;++changed;}}
    const bool early=reason!=tc_timeout;
    if(reason==tc_budget)winBudget_=true;
    const size_t len=w_-segStart_;
    uint8_t* p=arena_+segStart_;
    std::memcpy(p,trace_magic,8);p+=8;
    put<uint32_t>(p,1);put<uint32_t>(p,uint32_t(trace_header_bytes));put<uint32_t>(p,winIndex_);put<uint32_t>(p,winReason_);
    put<uint64_t>(p,base_);put<uint64_t>(p,regionStart_);
    put<uint32_t>(p,trace_region_bytes);put<uint32_t>(p,trace_half);put<uint32_t>(p,winTicks_);put<uint32_t>(p,winPre_);
    put<uint32_t>(p,uint32_t(winDeltas_));put<uint32_t>(p,uint32_t(len-trace_header_bytes));
    put<uint32_t>(p,(winTrunc_||winBudget_?1u:0u)|(early?2u:0u));put<uint32_t>(p,uint32_t(winTrunc_));
    put<double>(p,winOpenMs_);put<double>(p,winLastMs_);put<uint32_t>(p,reason);put<uint32_t>(p,winTrigger_);
    segs_[winIndex_].off=segStart_;segs_[winIndex_].len=len;
    arenaUsed_=w_;
    armed_=false;                       // re-arms only once the look is neutral again
    if(started_>=o_.max_windows)exhausted_=true;
    closed_.store(winIndex_+1,std::memory_order_release);
    char t[448];
    std::snprintf(t,sizeof t,"ee-camera;event=look-input-trace-window;n=%u;base=%llx;region=%llx;open=%s;close=%s;ticks=%u;preroll-ticks=%u;span-ms=%.1f;"
        "changed-words=%u;deltas=%llu;truncated-words=%llu;bytes=%llu;arena-left=%llu;tick-us-avg=%.1f;tick-us-max=%.1f;file=look-input-trace.bin",
        winIndex_+1,(unsigned long long)base_,(unsigned long long)regionStart_,trace_open_name(winReason_),trace_close_name(reason),winTicks_,winPre_,
        winLastMs_-winOpenMs_,changed,(unsigned long long)winDeltas_,(unsigned long long)winTrunc_,(unsigned long long)len,
        (unsigned long long)(trace_arena_bytes-arenaUsed_),costN_?costSum_/double(costN_):0.0,costMax_);
    log.line(true,t);
}
void LookInputTrace::finish(Sink& log,uint32_t reason) noexcept {if(open_)close_window(log,reason);}

// ------------------------------------------------------------------ the tick
void LookInputTrace::tick(Memory& mem,Sink& log,const LookIn& in) noexcept {
    if(!o_.enabled||(exhausted_&&!open_))return;
    const LookView& v=in.v;
    if(!v.have) {
        if(open_)close_window(log,tc_lost);
        havePrev_=false;haveRegion_=false;ring_clear();
        wait_log(log,in.now_ms,"no-tracks-look-candidate");
        return;
    }
    const bool wrap_bad=v.eff>~0ull-trace_l_off||v.eff+trace_l_off<trace_half;
    const uint64_t start=wrap_bad?0:v.eff+trace_l_off-trace_half,end=start+trace_region_bytes;
    if(wrap_bad||!(v.hi>v.lo)||start<v.lo||end>v.hi||end<start) {
        if(open_)close_window(log,tc_region);
        havePrev_=false;haveRegion_=false;ring_clear();
        wait_log(log,in.now_ms,"region-outside-block");
        return;
    }
    if(!haveRegion_||base_!=v.eff) {
        if(open_)close_window(log,tc_identity);
        base_=v.eff;regionStart_=start;haveRegion_=true;havePrev_=false;ring_clear();
        char t[320];
        std::snprintf(t,sizeof t,"ee-camera;event=look-input-trace-armed;base=%llx;region=%llx..%llx;l-offset=0x%x;windows-max=%u;window-ms=%.0f;preroll-ms=%.0f;"
            "arena-bytes=%llu;ring-bytes=%llu;read-only=1",(unsigned long long)base_,(unsigned long long)start,(unsigned long long)end,trace_half,
            o_.max_windows,o_.window_ms,o_.preroll_ms,(unsigned long long)trace_arena_bytes,(unsigned long long)trace_ring_bytes);
        log.line(true,t);
    }
    if(!mem.read(start,cur_,trace_region_bytes)) {
        ++readFail_;
        if(open_)close_window(log,tc_read);
        havePrev_=false;ring_clear();
        wait_log(log,in.now_ms,"read-failure");
        return;
    }
    ++ticks_;
    const uint32_t idx=tickIdx_++;
    const float yaw=from_bits(cur_[trace_l_word]),pitch=from_bits(cur_[trace_l_word-1]);
    uint32_t nTotal=0,nStored=0;
    float pyaw=0,ppitch=0;
    if(havePrev_) {
        pyaw=from_bits(prev_[trace_l_word]);ppitch=from_bits(prev_[trace_l_word-1]);
        for(uint32_t i=0;i<trace_words;++i) {
            if(cur_[i]==prev_[i])continue;
            if(nStored<trace_max_deltas)scratch_[nStored++]={i*4u,prev_[i],cur_[i]};
            ++nTotal;
        }
        if(nTotal>nStored)++overflows_;
    }
    // Trigger: the look value leaves neutral (yaw or pitch) or the probe saw a look event begin; only when armed
    // (the look has been neutral since the previous window closed).
    bool trig=false;uint32_t reason=0;
    if(havePrev_) {
        if(!armed_&&std::isfinite(yaw)&&std::isfinite(pitch)&&std::fabs(yaw)<=trace_zero_band&&std::fabs(pitch)<=trace_pitch_zero_band)armed_=true;
        if(armed_&&!open_&&!exhausted_&&started_<o_.max_windows) {
            if(moved_from_zero(pyaw,yaw,trace_zero_band)||moved_from_zero(ppitch,pitch,trace_pitch_zero_band)){trig=true;reason=to_value;}
            else if(prevQuiet_>=0&&v.quiet_ms<prevQuiet_-1e-6){trig=true;reason=to_event;}
        }
    }
    prevQuiet_=v.quiet_ms;
    if(havePrev_) {
        uint8_t hdr[trace_tick_header_bytes];uint8_t* p=hdr;
        put<uint32_t>(p,idx);put<uint32_t>(p,in.tid);put<double>(p,in.now_ms);put<uint64_t>(p,in.frame);put<uint32_t>(p,nStored);put<uint32_t>(p,nTotal);
        const size_t dbytes=size_t(nStored)*trace_delta_bytes;
        static_assert(sizeof(Delta)==trace_delta_bytes,"delta layout");
        if(open_) {
            if(winTicks_>=trace_max_window_ticks||!arena_put(hdr,reinterpret_cast<const uint8_t*>(scratch_),dbytes))close_window(log,tc_budget);
            else winLastMs_=in.now_ms;
        } else {
            ring_push(hdr,reinterpret_cast<const uint8_t*>(scratch_),dbytes,in.now_ms);
            if(trig)open_window(log,reason,yaw,pitch,v.quiet_ms,in,idx);
        }
        if(open_&&in.now_ms-winOpenMs_>=o_.window_ms)close_window(log,tc_timeout);
    }
    uint32_t* t=prev_;prev_=cur_;cur_=t;havePrev_=true;
}

// ------------------------------------------------------------------ off the render path
unsigned LookInputTrace::drain(TraceOut& out) noexcept {
    if(drainBusy_.test_and_set(std::memory_order_acquire))return 0;
    unsigned n=0;
    const unsigned closed=closed_.load(std::memory_order_acquire);
    while(drained_<closed&&writeFail_<5) {
        if(!out.write(arena_+segs_[drained_].off,segs_[drained_].len)){++writeFail_;break;}
        ++drained_;++n;
    }
    drainBusy_.clear(std::memory_order_release);
    return n;
}
LookInputTrace::Stats LookInputTrace::stats() const noexcept {
    Stats s;s.windows_started=started_;s.windows_closed=closed_.load(std::memory_order_acquire);s.windows_drained=drained_;
    s.ticks=ticks_;s.read_failures=readFail_;s.tick_overflows=overflows_;s.arena_used=arenaUsed_;
    s.cost_avg_us=costN_?costSum_/double(costN_):0;s.cost_max_us=costMax_;s.exhausted=exhausted_;s.write_failures=writeFail_;
    return s;
}

} // namespace acz::ee_camera
