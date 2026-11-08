#pragma once
// J-w1-ee-camera: in-process probe of the emulated PS2 (EE) camera fields
// (E6/E7, D-237..D-241, issue 29). Default OFF (key ee_camera_probe).
//
// Re-scoped after Astra's review (issue 29, 2026-10-23T08:13Z) and live run 1
// (D-241: one 64 MiB block found, every frame failed eye-height at the fixed
// E6 offsets, nothing written):
//
//  * DEFAULT = READ-ONLY OBSERVATION. ee_camera_probe=true performs the block
//    locator, a relative-geometry scan on a worker thread, diagnostics, a
//    per-frame seam time series (A/B/C: frame id, QPC, thread id, field values,
//    recovered basis) and a once-per-second field-activity summary. It never
//    publishes a write command.
//  * Writes (look yaw/pitch/roll, optional anchored position) exist only behind
//    ee_camera_write_trials=true (default off, NOT for the next live run) AND a
//    validated block AND demonstrated boundary ordering AND an independent
//    world-path observer (WorldPathObserver: placeholder, none implemented, so
//    production can never write today). Writes are one scalar field at a time:
//    the game never sees the three look angles change atomically (a mixed pose
//    is possible); see the findings file.
//
// SDK-free core shared by the Windows glue (ee_camera_probe_win.cpp) and the
// offline fixtures (tests/acz-vr/ee_camera_probe_offline_tests.cpp).
#include "ee_camera_basis.h"
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace acz::ee_camera {

inline constexpr uint64_t block_bytes=0x4000000ull;    // exactly 64 MiB
inline constexpr double pi=3.14159265358979323846;
inline constexpr double angle_bound=0.35;              // rad, any write vs the trial's original
inline constexpr double position_bound=5.0;            // units
inline constexpr float ten_degrees=0.17453292f;
inline constexpr unsigned validate_frames=10;
inline constexpr double stable_ms=5000,run_cap_ms=90000,search_cap_ms=20*60*1000.0;
inline constexpr double scan_every_ms=5000;
inline constexpr double rel_scan_fast_ms=2000,rel_scan_slow_ms=10000,rel_scan_fast_window_ms=60000;
inline constexpr unsigned pause_ticks=30,cockpit_ticks=10;

enum Field : unsigned {
    look_pitch,look_yaw,look_roll,air_x,air_y,air_z,heading,cam_x,cam_y,cam_z,cam_pitch,cam_yaw,cam_roll,look_yaw_copy,field_count
};
// E6 offsets from the E6 block base. The relative scan treats them as fixed
// DELTAS from the look-yaw field (an "effective base" = look-yaw address - 0xFDD410).
inline constexpr uint32_t field_offset[field_count]={
    0xFDD40C,0xFDD410,0xFDD414,0xFDD42C,0xFDD430,0xFDD434,0xFDD448,0xFDD860,0xFDD864,0xFDD868,0xFDD870,0xFDD874,0xFDD878,0xFDD894};
inline constexpr const char* field_name[field_count]={
    "look-pitch","look-yaw","look-roll","air-x","air-y","air-z","heading","cam-x","cam-y","cam-z","cam-pitch","cam-yaw","cam-roll","look-yaw-copy"};
inline constexpr uint32_t render_offset=0x312E30;       // render-side copy, +0x312E30..+0x312E50
inline constexpr unsigned render_floats=9;
inline constexpr uint32_t rel_lo=4,rel_hi=0x488;        // field span around look yaw: [L-4, L+0x488)
inline constexpr bool angle_field(unsigned f) noexcept {return f==look_pitch||f==look_yaw||f==look_roll;}
inline constexpr bool position_field(unsigned f) noexcept {return f==cam_x||f==cam_z;}

inline double wrap(double a) noexcept {
    if(!std::isfinite(a))return a;
    a=std::fmod(a+pi,2*pi);if(a<0)a+=2*pi;return a-pi;
}
inline double wrap_deg(double a) noexcept {
    if(!std::isfinite(a))return a;
    a=std::fmod(a+180,360);if(a<0)a+=360;return a-180;
}
inline uint32_t bits(float f) noexcept {uint32_t u;std::memcpy(&u,&f,4);return u;}
inline float from_bits(uint32_t u) noexcept {float f;std::memcpy(&f,&u,4);return f;}

// ------------------------------------------------------------------ memory
// Production: VirtualQuery-checked ranges + SEH-guarded copies (no
// IsBadReadPtr). Fakes: sparse blocks with injectable faults.
struct Memory {
    virtual bool read(uint64_t address,void* out,uint32_t bytes) noexcept=0;
    virtual bool write_float(uint64_t address,float value) noexcept=0;
    virtual ~Memory()=default;
};
inline bool read_float(Memory& m,uint64_t a,float& out) noexcept {return m.read(a,&out,4);}

// ------------------------------------------------------------------ blocks
struct Region {uint64_t base=0,size=0,allocation_base=0;bool committed=false,is_private=false,read_write=false;};
// Streams VirtualQuery-style regions (ascending base) grouped by allocation
// base. A block is an allocation whose regions are ALL committed private
// read-write and whose total span is exactly 64 MiB.
class CandidateCollector {
public:
    static constexpr unsigned cap=8;
    void reset() noexcept {*this=CandidateCollector{};}
    void add(const Region& r) noexcept {
        if(!open_||r.allocation_base!=alloc_){close();open_=true;alloc_=r.allocation_base;start_=r.base;span_=0;ok_=true;}
        if(r.base!=start_+span_)ok_=false;
        span_+=r.size;ok_=ok_&&r.committed&&r.is_private&&r.read_write;
    }
    void finish() noexcept {close();}
    unsigned count() const noexcept {return stored_;}
    unsigned total() const noexcept {return total_;}
    uint64_t base(unsigned i) const noexcept {return i<stored_?bases_[i]:0;}
private:
    void close() noexcept {
        if(!open_)return;open_=false;
        if(ok_&&span_==block_bytes){++total_;if(stored_<cap)bases_[stored_++]=start_;}
    }
    bool open_=false,ok_=false;uint64_t alloc_=0,start_=0,span_=0;
    uint64_t bases_[cap]{};unsigned stored_=0,total_=0;
};

// ------------------------------------------------------------------ relations
struct Snapshot {float v[field_count]{};float render[render_floats]{};bool render_ok=false;};
// Fields at `base` (effective base); the render copy is optional (it may lie
// outside the block when the effective base is shifted).
inline bool read_snapshot(Memory& m,uint64_t base,Snapshot& s) noexcept {
    if(!base)return false;
    for(unsigned i=0;i<field_count;++i)if(!m.read(base+field_offset[i],&s.v[i],4))return false;
    s.render_ok=m.read(base+render_offset,s.render,sizeof s.render);
    if(!s.render_ok)for(float& f:s.render)f=std::nanf("");
    return true;
}
// Relation bits (D-241 relaxed form; E6/E7 measured 3.6 up and ~36 ahead):
enum Rel : uint32_t {
    rel_sane=1,    // finite, look <= pi, rotations <= 2pi(+1 for scan), positions sane, aircraft not at the origin
    rel_yaw=2,     // |wrap(C - (H + L))| < 2e-3 (+extra), not all three zero
    rel_vert=4,    // camera y - aircraft y in [1, 8]
    rel_horiz=8,   // horizontal camera-aircraft distance in [20, 60]
    rel_eye=16,    // s * (camera - aircraft) == recovered eye (s = 16, 1, 1/16), 5% (diagnostic only)
    rel_degrees=32 // the yaw relation holds in DEGREES and angles exceed 2pi+1: flagged, never written
};
struct RelFacts {double yaw_res=0,vert=0,horiz=0,eye_err=0,eye_scale=0;};
uint32_t relations(const float* v,const Basis* basis,double extra_yaw,double slack,RelFacts* facts) noexcept;
// nullptr = valid; otherwise the first failed relation (stable names).
inline const char* validate(const Snapshot& s,double extra_yaw=0,double slack=0) noexcept {
    for(float f:s.v)if(!std::isfinite(f))return "nonfinite";
    for(unsigned f:{look_pitch,look_yaw,look_roll})if(std::fabs(s.v[f])>pi)return "look-range";
    for(unsigned f:{heading,cam_pitch,cam_yaw,cam_roll})if(std::fabs(s.v[f])>2*pi)return "rotation-range";
    for(unsigned f:{air_x,air_z,cam_x,cam_z})if(std::fabs(s.v[f])>1e7f)return "position-range";
    if(s.v[air_y]<-2000.f||s.v[air_y]>200000.f)return "altitude-range";
    if(std::fabs(s.v[air_x])+std::fabs(s.v[air_z])<=1.f)return "aircraft-at-origin";
    const uint32_t m=relations(s.v,nullptr,extra_yaw,slack,nullptr);
    if(!(m&rel_vert))return "eye-height";
    if(!(m&rel_horiz))return "eye-horizontal";
    if(!(m&rel_yaw))return "camera-yaw-relation";
    return nullptr;
}

// Per-frame validation of every (effective base, block) candidate; locks only
// when EXACTLY one candidate validated N consecutive frames while TRACKING the
// live aircraft (position/heading changed over the streak) and no other one is
// validating. Two or more full validations: refused.
class Locator {
public:
    static constexpr unsigned cap=8;
    enum State {searching,locked,refused};
    // eff = effective base; [lo, hi) = the region the fields lie in (all accesses stay inside).
    bool add(uint64_t eff,uint64_t lo,uint64_t hi=0) noexcept {
        for(unsigned i=0;i<n_;++i)if(eff_[i]==eff)return false;
        if(n_>=cap){++dropped_;return false;}
        eff_[n_]=eff;block_[n_]=lo;hi_[n_]=hi?hi:lo+block_bytes;streak_[n_]=0;why_[n_]="unchecked";++n_;return true;
    }
    unsigned count() const noexcept {return n_;}
    unsigned dropped() const noexcept {return dropped_;}
    uint64_t candidate(unsigned i) const noexcept {return i<n_?eff_[i]:0;}
    uint64_t block(unsigned i) const noexcept {return i<n_?block_[i]:0;}
    const char* why(unsigned i) const noexcept {return i<n_?why_[i]:"";}
    unsigned streak(unsigned i) const noexcept {return i<n_?streak_[i]:0;}
    State state() const noexcept {return state_;}
    uint64_t base() const noexcept {return locked_;}
    uint64_t locked_block() const noexcept {return lockedBlock_;}
    uint64_t locked_hi() const noexcept {return lockedHi_;}
    const char* reason() const noexcept {return reason_;}
    State step(Memory& m) noexcept {
        if(state_!=searching)return state_;
        unsigned full=0,partial=0,which=0;
        for(unsigned i=0;i<n_;++i) {
            Snapshot s;const char* w=read_snapshot(m,eff_[i],s)?validate(s):"unreadable";
            if(w){streak_[i]=0;why_[i]=w;continue;}
            if(streak_[i]==0)std::memcpy(start_[i],&s.v[air_x],sizeof start_[i]),startHeading_[i]=s.v[heading];
            ++streak_[i];why_[i]="valid";
            if(streak_[i]>=validate_frames) {
                const bool moved=std::fabs(s.v[air_x]-start_[i][0])>1e-3f||std::fabs(s.v[air_y]-start_[i][1])>1e-3f||
                                 std::fabs(s.v[air_z]-start_[i][2])>1e-3f||std::fabs(s.v[heading]-startHeading_[i])>1e-6f;
                if(moved){++full;which=i;}else why_[i]="static-not-tracking";
            } else ++partial;
        }
        if(full>=2){state_=refused;reason_="ambiguous-several-candidates-validate";return state_;}
        if(full==1&&partial==0){state_=locked;locked_=eff_[which];lockedBlock_=block_[which];lockedHi_=hi_[which];}
        return state_;
    }
    void reset() noexcept {*this=Locator{};}
private:
    uint64_t eff_[cap]{},block_[cap]{},hi_[cap]{};uint64_t lockedHi_=0;unsigned streak_[cap]{};const char* why_[cap]{};
    float start_[cap][3]{};float startHeading_[cap]{};
    unsigned n_=0,dropped_=0;State state_=searching;uint64_t locked_=0,lockedBlock_=0;const char* reason_="";
};

// ------------------------------------------------------------------ relative scan
// Worker-thread scan of a whole block (16M float positions, chunked guarded
// copies): every 4-byte position i is treated as a look-yaw field and the E6
// structure is tested at the fixed relative deltas. Diagnostics only; its full
// passes become Locator candidates (never written before per-frame validation).
struct ScanHit {uint64_t offset=0,lo=0,hi=0;uint32_t mask=0;float v[field_count]{};RelFacts f{};int score=-1;};
struct ScanResult {
    static constexpr unsigned full_cap=8,near_cap=32,deg_cap=8,prop_cap=8;
    // Addresses below are ABSOLUTE; lo/hi = the region (allocation part) they lie in.
    uint64_t block=0,bytes=0;double ms=0;unsigned chunks=0,unreadable_chunks=0,regions=0;bool basis=false,complete=true;
    uint64_t counts[6]{};               // positions passing each relation alone: sane,yaw,vert,horiz,eye,degrees
    uint64_t full=0,near=0,degrees=0;
    uint64_t full_off[full_cap]{},full_lo[full_cap]{},full_hi[full_cap]{};unsigned nfull=0;
    ScanHit nearHits[near_cap]{};unsigned nnear=0;
    uint64_t deg_off[deg_cap]{},deg_lo[deg_cap]{};unsigned ndeg=0;
    ScanHit best[3]{};
    uint64_t prop=0,prop16=0;uint64_t prop_off[prop_cap]{},prop_lo[prop_cap]{};float prop_ratio[prop_cap]{};unsigned nprop=0; // route 2
};
struct Sink {virtual void line(bool important,const char* text) noexcept=0;virtual ~Sink()=default;};
struct RegionInfo;
struct RegionCensus;
class DiffScanner;
// Built exclusively by the scan worker, including all region/candidate formatting.
// Fixed storage: region summary + diff summary + at most ten ranked candidates.
struct FinalReport {
    unsigned regions=0,lines=0;
    uint64_t wide_passes=0;
    char text[12][448]{};
};
class FinalReportBox {
public:
    // Worker only. Allocation failure preserves the previous complete snapshot.
    bool publish(const RegionInfo* regions,unsigned n,const RegionCensus& census,
                 uint64_t wide_passes,const DiffScanner& diff) noexcept;
    // Callback: try-lock once, copy an immutable handle, unlock before output.
    // False means unavailable/busy; never waits for the worker.
    bool try_report(const char* trigger,Sink& sink) noexcept;
private:
    std::mutex mutex_;
    std::shared_ptr<const FinalReport> latest_;
};
// Hex dump + float decode of 64-byte windows around the E6 offsets of `base`.
void dump_e6(Memory& mem,uint64_t base,Sink& log,const char* tag) noexcept;
// One whole-block scan. `scratch` must hold scan_scratch_floats floats.
inline constexpr uint32_t scan_chunk=1u<<20;
inline constexpr size_t scan_scratch_floats=(scan_chunk+rel_lo+rel_hi+16)/4;
// ACCUMULATES into `out` (callers reset it): one call per region [block, block+bytes).
void run_relative_scan(Memory& mem,uint64_t block,uint64_t bytes,const Basis* basis,float* scratch,ScanResult& out,
                       void (*yield)()=nullptr) noexcept;
void log_scan(const ScanResult& r,Sink& log) noexcept;
// Asynchronous scan service (glue: a worker thread; fixtures: synchronous).
struct ScanRequester {
    // block == 0: a WIDE pass over all readable regions of the process (D-245).
    virtual bool request(uint64_t block,const Basis* basis,bool dump) noexcept=0; // false: busy
    virtual const ScanResult* poll() noexcept=0;                                  // non-null once per completed request
    virtual ~ScanRequester()=default;
};

// ------------------------------------------------------------------ seams
enum Seam : uint32_t {seam_none=0,seam_A=1,seam_B=2,seam_C=3};
inline const char* seam_name(uint32_t s) noexcept {static constexpr const char* n[]={"-","A","B","C"};return s<4?n[s]:"?";}
enum Mode : uint32_t {mode_absolute=0,mode_delta=1};

inline constexpr uint32_t no_anchor=0xFFFFFFFFu;
inline constexpr bool anchor_field(uint32_t f) noexcept {return f==air_x||f==air_z;}
struct Command {
    uint32_t seam=seam_none,mode=mode_absolute,n=0;
    uint32_t field[2]{},anchor[2]{no_anchor,no_anchor};float value[2]{},step[2]{},lo[2]{},hi[2]{};
};
// Single writer (the Present tick), any number of try-only readers.
class CommandBox {
public:
    static constexpr int words=15;
    void publish(const Command& c) noexcept {
        const uint32_t s=seq_.load(std::memory_order_relaxed);
        seq_.store(s+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
        uint32_t w[words]={c.seam,c.mode,c.n,c.field[0],c.field[1],c.anchor[0],c.anchor[1],bits(c.value[0]),bits(c.value[1]),
                           bits(c.step[0]),bits(c.step[1]),bits(c.lo[0]),bits(c.lo[1]),bits(c.hi[0]),bits(c.hi[1])};
        for(int i=0;i<words;++i)w_[i].store(w[i],std::memory_order_relaxed);
        seq_.store(s+2,std::memory_order_release);
    }
    void disarm() noexcept {publish(Command{});}
    bool read(Command& c) const noexcept {
        const uint32_t s1=seq_.load(std::memory_order_acquire);
        if(s1==0||(s1&1u))return false;
        uint32_t w[words];for(int i=0;i<words;++i)w[i]=w_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(seq_.load(std::memory_order_relaxed)!=s1)return false;
        c.seam=w[0];c.mode=w[1];c.n=w[2];c.field[0]=w[3];c.field[1]=w[4];c.anchor[0]=w[5];c.anchor[1]=w[6];
        c.value[0]=from_bits(w[7]);c.value[1]=from_bits(w[8]);c.step[0]=from_bits(w[9]);c.step[1]=from_bits(w[10]);
        c.lo[0]=from_bits(w[11]);c.lo[1]=from_bits(w[12]);c.hi[0]=from_bits(w[13]);c.hi[1]=from_bits(w[14]);
        return c.seam!=seam_none;
    }
private:
    std::atomic<uint32_t> seq_{0};std::atomic<uint32_t> w_[words]{};
};
struct WriteRecord {
    std::atomic<uint64_t> stamp{0};std::atomic<uint64_t> old_new{0};std::atomic<uint32_t> anchor{0};
};
// Per-seam observation record (one per interval, first visit): QPC, thread id,
// the candidate fields and (B) the basis publication number.
// Words: [0..15] = +FDD40C..+FDD44C (look p/y/r, ..., air xyz, ..., heading),
// [16..22] = +FDD860..+FDD87C (camera xyz, -, rot pyr), [23..31] = render copy.
inline constexpr unsigned obs_words=32;
struct SeamRecord {
    std::atomic<uint32_t> seq{0};std::atomic<uint64_t> interval{0},qpc{0},basis{0};std::atomic<uint32_t> tid{0},ok{0};
    std::atomic<uint32_t> w[obs_words]{};
};
inline float obs_field(const float* w,unsigned f) noexcept {
    const uint32_t o=field_offset[f];
    return o<0xFDD44C?w[(o-0xFDD40C)/4]:(o>=0xFDD860&&o<0xFDD87C?w[16+(o-0xFDD860)/4]:std::nanf(""));
}
// Everything the seams touch: atomics only.
struct Shared {
    std::atomic<uint64_t> base{0};             // validated effective base (writes only): nonzero after validation
    std::atomic<uint64_t> obs_base{0};         // observation base (validated, else the E6 base of the block)
    std::atomic<uint64_t> block_lo{0},block_hi{0}; // the real allocation: every seam access stays inside
    std::atomic<uint64_t> frame{0};            // Present ticks completed
    std::atomic<bool> position_allowed{false},writes_enabled{false};
    CommandBox cmd;
    std::atomic<uint64_t> last_write_interval{0};
    std::atomic<int> inflight{0};
    std::atomic<uint32_t> fault{0};
    std::atomic<uint64_t> writes{0};
    WriteRecord rec[2];
    SeamRecord seamRec[4];
    std::atomic<uint64_t> phase_interval[4]{};
    std::atomic<uint32_t> phase_bits{0},phase_last{0};
    std::atomic<uint32_t> phase_visits[4][4]{},phase_changes[4][4]{};
};
inline bool in_block(const Shared& sh,uint64_t a,uint32_t n) noexcept {
    const uint64_t lo=sh.block_lo.load(std::memory_order_acquire),hi=sh.block_hi.load(std::memory_order_acquire);
    return lo&&a>=lo&&a+n<=hi&&a+n>=a;
}

// Seam write path (only with writes_enabled; production never sets it today).
// No allocation, a ~100-byte frame. ONE scalar per field: no atomic multi-field pose.
inline bool seam_apply(Shared& sh,uint32_t at,Memory& mem) noexcept {
    const uint64_t base=sh.base.load(std::memory_order_acquire);
    if(!base||!sh.writes_enabled.load(std::memory_order_acquire)||sh.fault.load(std::memory_order_relaxed))return false;
    const uint64_t interval=sh.frame.load(std::memory_order_acquire)+1;
    if(sh.last_write_interval.load(std::memory_order_relaxed)==interval)return false;
    sh.inflight.fetch_add(1,std::memory_order_seq_cst);
    bool wrote=false;Command c;
    if(sh.cmd.read(c)&&c.seam==at&&c.n>=1&&c.n<=2) {
        for(uint32_t i=0;i<c.n;++i) {
            const uint32_t f=c.field[i];
            if(f>=field_count||!(angle_field(f)||(position_field(f)&&sh.position_allowed.load(std::memory_order_relaxed))))break;
            const uint64_t a=base+field_offset[f];
            if(!in_block(sh,a,4))break;
            float old=0,anc=0;
            if(!read_float(mem,a,old)){sh.fault.store(1);break;}
            if(c.anchor[i]!=no_anchor) {
                const uint64_t aa=base+field_offset[c.anchor[i] < field_count ? c.anchor[i] : 0];
                if(!anchor_field(c.anchor[i])||!in_block(sh,aa,4))break;
                if(!read_float(mem,aa,anc)){sh.fault.store(1);break;}
            }
            float nv=c.mode==mode_delta?old+c.step[i]:anc+c.value[i];
            if(nv<anc+c.lo[i])nv=anc+c.lo[i];
            if(nv>anc+c.hi[i])nv=anc+c.hi[i];
            if(!std::isfinite(nv)||!std::isfinite(old))break;
            if(!mem.write_float(a,nv)){sh.fault.store(2);break;}
            sh.rec[i].anchor.store(bits(anc),std::memory_order_relaxed);
            sh.rec[i].old_new.store((uint64_t(bits(old))<<32)|bits(nv),std::memory_order_relaxed);
            sh.rec[i].stamp.store(interval-1,std::memory_order_release);
            wrote=true;
        }
        if(wrote){sh.last_write_interval.store(interval,std::memory_order_relaxed);sh.writes.fetch_add(1,std::memory_order_relaxed);}
    }
    sh.inflight.fetch_sub(1,std::memory_order_seq_cst);
    return wrote;
}
// Read-only seam observation, once per interval per seam: the record above
// plus the EE-phase matrix (does aircraft x change between seams?). Three
// guarded copies (~128 bytes on the stack); no allocation, no logging.
inline void seam_observe(Shared& sh,uint32_t at,Memory& mem,uint64_t qpc,uint32_t tid,uint64_t basis_seq) noexcept {
    const uint64_t base=sh.obs_base.load(std::memory_order_acquire);
    if(!base||at<1||at>3)return;
    const uint64_t interval=sh.frame.load(std::memory_order_acquire)+1;
    if(sh.phase_interval[at].exchange(interval,std::memory_order_relaxed)==interval)return;
    float w[obs_words];uint32_t ok=0;
    for(float& f:w)f=std::nanf("");
    if(in_block(sh,base+0xFDD40C,64)&&mem.read(base+0xFDD40C,w,64))ok|=1;
    if(in_block(sh,base+0xFDD860,28)&&mem.read(base+0xFDD860,w+16,28))ok|=2;
    if(in_block(sh,base+render_offset,36)&&mem.read(base+render_offset,w+23,36))ok|=4;
    SeamRecord& r=sh.seamRec[at];
    const uint32_t s=r.seq.load(std::memory_order_relaxed);
    r.seq.store(s+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
    r.interval.store(interval,std::memory_order_relaxed);r.qpc.store(qpc,std::memory_order_relaxed);
    r.tid.store(tid,std::memory_order_relaxed);r.ok.store(ok,std::memory_order_relaxed);r.basis.store(basis_seq,std::memory_order_relaxed);
    for(unsigned i=0;i<obs_words;++i)r.w[i].store(bits(w[i]),std::memory_order_relaxed);
    r.seq.store(s+2,std::memory_order_release);
    if(!(ok&1))return;
    const uint32_t vb=bits(w[(field_offset[air_x]-0xFDD40C)/4]);
    const uint32_t prev=sh.phase_last.exchange(at,std::memory_order_relaxed);
    const uint32_t old=sh.phase_bits.exchange(vb,std::memory_order_relaxed);
    if(prev<1||prev>3)return;
    sh.phase_visits[prev][at].fetch_add(1,std::memory_order_relaxed);
    if(old!=vb)sh.phase_changes[prev][at].fetch_add(1,std::memory_order_relaxed);
}
struct ObsSample {bool valid=false;uint64_t interval=0,qpc=0,basis=0;uint32_t tid=0,ok=0;float w[obs_words]{};};
inline bool read_record(const SeamRecord& r,ObsSample& out) noexcept {
    const uint32_t s1=r.seq.load(std::memory_order_acquire);
    if(s1==0||(s1&1u))return false;
    ObsSample o;o.interval=r.interval.load(std::memory_order_relaxed);o.qpc=r.qpc.load(std::memory_order_relaxed);
    o.tid=r.tid.load(std::memory_order_relaxed);o.ok=r.ok.load(std::memory_order_relaxed);o.basis=r.basis.load(std::memory_order_relaxed);
    for(unsigned i=0;i<obs_words;++i)o.w[i]=from_bits(r.w[i].load(std::memory_order_relaxed));
    std::atomic_thread_fence(std::memory_order_acquire);
    if(r.seq.load(std::memory_order_relaxed)!=s1)return false;
    o.valid=true;out=o;return true;
}
// Boundary ordering from the phase matrix: demonstrated when >= 300 seam
// transitions were seen, aircraft x changed in >= 50% of frames and >= 95% of
// the changes fell into ONE inter-seam interval (e.g. "AB": between Present
// and the first cockpit member). Writes (if ever enabled) need this.
struct Ordering {bool demonstrated=false;uint32_t from=0,to=0;uint64_t visits=0,changes=0;double share=0,rate=0;};
inline Ordering ordering(const Shared& sh) noexcept {
    Ordering o;uint64_t best=0;
    for(uint32_t a=1;a<=3;++a)for(uint32_t b=1;b<=3;++b) {
        const uint64_t v=sh.phase_visits[a][b].load(),c=sh.phase_changes[a][b].load();
        o.visits+=v;o.changes+=c;if(c>best){best=c;o.from=a;o.to=b;}
    }
    const uint64_t frames=sh.phase_visits[1][2].load()+sh.phase_visits[1][3].load()+sh.phase_visits[1][1].load();
    o.share=o.changes?double(best)/double(o.changes):0;o.rate=frames?double(o.changes)/double(frames):0;
    o.demonstrated=o.visits>=300&&o.rate>=0.5&&o.share>=0.95;
    return o;
}

// ------------------------------------------------------------------ future prerequisites
// PLACEHOLDERS ONLY (Astra, issue 29): no implementation exists.
// (1) Any causal trial must observe the WORLD path independently of the
//     cockpit SrtData (frame-correlated pre-projected world vertices or static
//     world-feature motion with the expected sign/magnitude, HUD/cockpit
//     excluded, aircraft motion controlled), with baseline/zero-offset controls.
struct WorldPathResponse {uint64_t frame=0;double yaw=0,pitch=0,roll=0;bool valid=false;};
struct WorldPathObserver {
    virtual bool observe(uint64_t frame,WorldPathResponse& out) noexcept=0;
    virtual ~WorldPathObserver()=default;
};
// (2) No double application of head pose: if an upstream input rotates world
//     AND cockpit, the cockpit replay must account for that source camera
//     before adding the residual per-eye pose (neutral identity, known
//     yaw/pitch/roll and per-eye offsets proven against an independent
//     transform calculation before the paths are connected).
struct SourceCameraAccount {
    // The upstream camera rotation the game has ALREADY applied this frame
    // (row-major 3x3), to be removed from the per-eye pose before substitution.
    virtual bool applied_rotation(uint64_t frame,double R[9]) noexcept=0;
    virtual ~SourceCameraAccount()=default;
};

// ------------------------------------------------------------------ classification
enum Verdict : uint8_t {v_no_data=0,v_none,v_partial,v_flicker,v_effect};
inline const char* verdict_name(uint8_t v) noexcept {static constexpr const char* n[]={"NO-DATA","NONE","PARTIAL","FLICKER","EFFECT"};return v<5?n[v]:"?";}
struct Stats {
    unsigned n=0,affected=0,alternations=0;
    double mean_cmd=0,mean_resp=0,min_resp=0,max_resp=0,ratio=0;
    int lag=-1;uint8_t verdict=v_no_data;
};
inline Stats classify(const float* cmd,const float* resp,const int* lag_frames,unsigned count) noexcept {
    Stats s;int first=-1;bool prevA=false,havePrev=false;
    for(unsigned i=0;i<count;++i) {
        if(!std::isfinite(resp[i])||!std::isfinite(cmd[i])||cmd[i]==0.f)continue;
        const bool a=std::fabs(double(resp[i]))>=0.5*std::fabs(double(cmd[i]));
        if(s.n==0){s.min_resp=s.max_resp=resp[i];}
        s.min_resp=resp[i]<s.min_resp?resp[i]:s.min_resp;s.max_resp=resp[i]>s.max_resp?resp[i]:s.max_resp;
        ++s.n;s.affected+=a;
        if(havePrev&&a!=prevA)++s.alternations;
        prevA=a;havePrev=true;
        if(a&&first<0){first=int(i);s.lag=lag_frames[i];}
    }
    if(!s.n)return s;
    double sc=0,sr=0;unsigned steady=0,steadyA=0,tot=0;double ac=0,ar=0;
    for(unsigned i=0;i<count;++i) {
        if(!std::isfinite(resp[i])||!std::isfinite(cmd[i])||cmd[i]==0.f)continue;
        ac+=cmd[i];ar+=resp[i];++tot;
        if(first>=0&&int(i)>=first){sc+=cmd[i];sr+=resp[i];++steady;steadyA+=std::fabs(double(resp[i]))>=0.5*std::fabs(double(cmd[i]));}
    }
    s.mean_cmd=ac/tot;s.mean_resp=ar/tot;
    s.ratio=steady&&sc!=0?sr/sc:(ac!=0?ar/ac:0);
    const double frac=double(s.affected)/s.n,steadyFrac=steady?double(steadyA)/steady:0;
    const double ar_=std::fabs(s.ratio);
    if(s.n>=4&&frac>=0.2&&frac<=0.8&&s.alternations>=0.6*(s.n-1))s.verdict=v_flicker;
    else if(steady&&steadyFrac>=0.8&&ar_>=0.8&&ar_<=1.2)s.verdict=v_effect;
    else if(frac>=0.1||ar_>=0.2)s.verdict=v_partial;
    else s.verdict=v_none;
    return s;
}
enum Readback : uint8_t {rb_none=0,rb_persisted,rb_reverted,rb_other};
// Labels are INDICATIONS from the fake/trial data only; Astra: do not claim
// (i)/(ii)/(iii) without the observation-stage ordering data and an
// independent world-path response.
inline const char* hypothesis(uint8_t recovered,uint8_t composed,uint8_t render,double persisted,double reverted) noexcept {
    if(recovered==v_effect)return "consumed";
    if(recovered==v_flicker||composed==v_flicker)return "race-flicker";
    if(recovered==v_partial)return "partially-consumed";
    if(recovered==v_no_data)return "no-recovered-data";
    if(composed==v_effect||composed==v_partial)
        return render==v_effect||render==v_partial?"iii-composed-and-copied-but-not-rendered":"iii-composed-not-copied-render-copy-consumed";
    if(reverted>=0.8)return "i-overwritten-before-use";
    if(persisted>=0.8)return "ii-composed-before-boundary-or-not-an-input";
    return "unclear";
}

// ------------------------------------------------------------------ experiment
struct Options {
    uint32_t points=0x7;bool position=false; // bit0 A, bit1 B, bit2 C
    bool write_trials=false;                 // ee_camera_write_trials (default off)
    WorldPathObserver* world=nullptr;        // required for any write trial; none exists in production
};
struct Scanner {virtual void scan(CandidateCollector& out) noexcept=0;virtual ~Scanner()=default;};

enum Family : uint8_t {fam_baseline=0,fam_yaw_abs,fam_yaw_delta,fam_pitch,fam_roll,fam_position,fam_count};
inline const char* family_name(uint8_t f) noexcept {
    static constexpr const char* n[]={"baseline","look-yaw-abs","look-yaw-delta","look-pitch","look-roll","position"};return f<fam_count?n[f]:"?";
}
struct TrialDef {
    uint8_t family=fam_baseline;uint32_t seam=seam_none,mode=mode_absolute,n=0;
    uint32_t field[2]{};float offset[2]{};double write_ms=0,settle_ms=0;const char* name="baseline";
};
struct TrialResult {
    bool done=false,aborted=false;uint8_t family=0;uint32_t seam=0;const char* name="";
    Stats rec{},comp{},rend{};double persisted=0,reverted=0,peak_cmd=0;
    unsigned samples=0,stale=0,writes=0;const char* restore="none";const char* hyp="";
};
struct Raw {bool basis=false;double recYaw=0,recPitch=0,recRoll=0,eye[3]{};Snapshot s{};};

inline constexpr unsigned max_trials=24,max_samples=600,idle_cap=32,baseline_cap=600;
// Field-activity windows: +-0x2000 bytes around the E6 offsets (merged).
inline constexpr uint32_t act_lo0=0xFDD40C-0x2000,act_hi0=0xFDD894+0x2004,act_lo1=render_offset-0x2000,act_hi1=render_offset+0x24+0x2000;
inline constexpr unsigned act_floats0=(act_hi0-act_lo0)/4,act_floats1=(act_hi1-act_lo1)/4,act_floats=act_floats0+act_floats1;
inline constexpr unsigned obs_full_frames=300,obs_every=10;
inline constexpr unsigned track_cap=6;            // best 3 scan candidates + E6 structure of up to 3 blocks
// J-look-locator (D-268 follow-up): sticky identity of a tracked structure. A tracks-look / score-5 track survives
// scans that miss it (a scan read can land in a look ramp, where the strict 2e-3 yaw relation fails) and is
// only evictable after `track_missed_scans` consecutive misses AND `track_struct_stale_ms` without a passing
// per-tick structural re-check. A lower-scored scan entry never displaces a higher-scored one.
inline constexpr unsigned track_missed_scans=3;
inline constexpr double track_struct_stale_ms=2000;
inline constexpr double track_look_chg_rad=1e-3;  // a look word moving by more than this is a "look event" (denormal jitter is not)
inline constexpr double track_yaw_allow_cap=0.5;  // cap of the moving-look allowance of the structural re-check (rad)
inline constexpr double timeline_ms=250,timeline_heartbeat_ms=2000,tracking_log_ms=10000;

struct LookView;   // ee_look_write.h
class Experiment {
public:
    enum Phase {searching,stabilising,observing,running,done,aborted,refused};
    explicit Experiment(Options o) noexcept:opt_(o) {}
    // One Present boundary. `basis`: the latest recovered camera (nullptr when unavailable).
    void tick(Memory& mem,Shared& sh,Scanner& scan,ScanRequester& req,Sink& log,double now_ms,const Basis* basis) noexcept;
    Phase phase() const noexcept {return phase_;}
    const char* reason() const noexcept {return reason_;}
    unsigned trial_count() const noexcept {return ntrials_;}
    const TrialDef& trial(unsigned i) const noexcept {return trials_[i<max_trials?i:0];}
    const TrialResult& result(unsigned i) const noexcept {return results_[i<max_trials?i:0];}
    int yaw_comp() const noexcept {return yawComp_;}
    bool eye_comp() const noexcept {return eyeComp_;}
    const char* final_line() const noexcept {return final_;}
    uint64_t observed_frames() const noexcept {return obsFrames_;}
    unsigned scans() const noexcept {return relScans_;}
    void abort(Memory& mem,Shared& sh,Sink& log,const char* why) noexcept;
    unsigned build_trials() noexcept;
private:
    struct Sample {float cmd=0,rec=0,comp=0,rend=0;int lag=0;uint8_t rb=rb_none;};
    struct Q {double recYaw=0,recPitch=0,recRoll=0,recEye[3]{},compYaw=0,compPitch=0,compRoll=0,compPos[2]{};
              double rend[5]{};bool rec=false,hasRend[5]{};};
    void search(Memory&,Shared&,Scanner&,ScanRequester&,Sink&,double) noexcept;
    void stabilise(Memory&,Shared&,Sink&,double) noexcept;
    void maybe_start_trials(Memory&,Shared&,Sink&,double,const Snapshot&) noexcept;
    void observe(Memory&,Shared&,Sink&,double) noexcept;
    void run(Memory&,Shared&,Sink&,double) noexcept;
    void begin_trial(Memory&,Shared&,Sink&,double,const Snapshot&) noexcept;
    void end_write(Memory&,Shared&,Sink&,double) noexcept;
    void finish_baseline(Sink&) noexcept;
    void finish(Shared&,Sink&) noexcept;
    void restore(Memory&,Shared&,Sink&) noexcept;
    void quantities(const Raw&,Q&) const noexcept;
    void reference(Q& out) const noexcept;
    void flush_w(Sink&) noexcept;
    bool paused(const Snapshot&) noexcept;
    void emit(Sink& log,bool important,const char* text) noexcept;
    Options opt_;Phase phase_=searching;const char* reason_="";
    Locator loc_;CandidateCollector coll_;
    uint64_t blocks_[CandidateCollector::cap]{};unsigned nblocks_=0,blocksTotal_=0,scanIdx_=0;
    double searchStart_=-1,lastScan_=-1e300,lastStatus_=-1e300,stableSince_=-1,runStart_=0,phaseEnd_=0,lastNow_=-1;
    double lastRelScan_=-1e300,cockpitSearchMs_=0;unsigned relScans_=0,scansDone_=0;bool scanPending_=false;
    unsigned badStreak_=0,stillStreak_=0;uint32_t lastAir_[3]{};bool haveAir_=false;
    uint64_t lastBasisSeq_=0;unsigned sinceBasis_=1000;Basis basis_{};bool haveBasis_=false,fresh_=false;
    // observation
    bool obsValidated_=false,cockpitSeen_=false;uint64_t obsFrames_=0,obsLogged_=0,obsBase_=0;double lastActivity_=-1e300,obsQpc0_=0;
    uint64_t betweenAB_=0,betweenBC_=0,betweenCA_=0,betweenFrames_=0;ObsSample prevC_{};
    float actPrev_[act_floats]{},actCur_[act_floats]{};uint16_t actCount_[act_floats]{};unsigned actSamples_=0,actSummaries_=0;bool actHave_=false;
    double lastObsSummary_=-1e300;bool writeRefusedLogged_=false;char obsKind_=0;
    // rev 4 (D-247): observation follows the best-scoring candidate; candidate
    // timeline (time-based, 250 ms) and tracking classes for up to 6 structures.
    uint64_t bestEff_=0,bestLo_=0,bestHi_=0;int bestScore_=-1;
    struct Track {uint64_t eff=0,lo=0,hi=0;const char* src="";int score=0;bool have=false;float last[field_count]{};double changed[field_count]{};
                  double lastLine=-1e300;uint32_t lookHit=0,lookEv=0,airHit=0,airEv=0,quiet=0,anyChg=0,samples=0;
                  // sticky identity + per-tick structural re-check (J-look-locator)
                  uint32_t missed=0,structPass=0,structFail=0;double structLast=-1e300,bLast=-1e300,lookChgAt=-1e300;bool structOk=false,structSeen=false;};
    Track tr_[track_cap]{};double lastTl_=-1e300,lastTrackLog_=-1e300,tlCam_[2]{},tlAir_=0;bool tlHave_=false;
    void set_tracks(const ScanResult& r,Sink& log) noexcept;
    void timeline(Memory&,Sink&,double) noexcept;
    static bool sticky_track(const Track& t) noexcept;
    bool evictable_track(const Track& t) const noexcept;
    double lastLookEv_=-1e300;
public:
    static const char* classify_track(const Track& t) noexcept;
    const char* track_class_of(uint64_t eff) const noexcept {for(const Track& t:tr_)if(t.eff==eff&&t.eff)return classify_track(t);return "untracked";}
    uint64_t observe_base() const noexcept {return obsBase_;}
    // Option 3 stage 2 (ee_look_write.h): the best tracks-look candidate, the still-tracked region of
    // `locked_eff` (0 = none) and the cockpit/basis state. Read-only; fills the fields of LookView.
    void look_view(uint64_t locked_eff,LookView& out) const noexcept;
private:
    // trials
    TrialDef trials_[max_trials]{};TrialResult results_[max_trials]{};unsigned ntrials_=0,cur_=0;bool settle_=false;
    bool havePrev_=false;float prevNew_=0;double prevRel_=0;
    float orig_[2]{},anchorOrig_[2]{};uint32_t anchor_[2]{no_anchor,no_anchor};uint64_t trialTick_=0,firstStamp_=0,lastLoggedStamp_=0;uint64_t writesAtBegin_=0;double yawAllow_=0;
    Sample samples_[max_samples]{};unsigned nsamples_=0;
    Raw idle_[idle_cap]{};unsigned nidle_=0,idleHead_=0;
    Raw base_[baseline_cap]{};unsigned nbase_=0;
    Q ref_{};int yawComp_=0;bool eyeComp_=false;int rendIdx_[5]{-1,-1,-1,-1,-1};
    char w_[448]{};unsigned wFrames_=0;size_t wLen_=0;
    char final_[448]{};unsigned important_=0;
};

} // namespace acz::ee_camera
