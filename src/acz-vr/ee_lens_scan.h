#pragma once
// Lens/FOV widen plan (docs/review/lens-fov-widen-plan.md, route 1, USER APPROVED), PHASE 0 ONLY:
// the READ-ONLY lens scanner (key ee_lens_scan, default OFF). Implementation notes and the one-run
// recipe: docs/review/lens-scan-impl.md.
//
// What it does: scans the emulated PS2 (EE) memory image the probe already maps (the tracks-look 64 MiB
// block first, then the other private RW 64 MiB blocks; at most lens_region_cap regions / lens_byte_cap
// bytes / lens_scan_cap_ms) for 4-byte-aligned float32 PAIRS within 64 bytes of each other that match the
// lens candidates of the plan:
//   tan        0.6152 / 0.3460   (horizontal / vertical tan of the half FOV, +-1e-3)
//   scale      1.6255 / 2.8902   (projection scale = reciprocals)
//   tan16/64   the tan pair x16 and x64 (PS2 fixed-point style scaling, as float32)
//   angle-rad  1.1030 / 0.6663   (full FOV angles, radians)
//   angle-deg  63.2 / 38.1       (the same in degrees)
// Every pair (either order) is a candidate; each candidate is re-read once per Present tick for 300
// COCKPIT frames and classified static / stable-per-frame / unstable / unreadable.
//
// Astra review 2026-10-26: data pages only, identity revalidated per re-read, coverage UNKNOWN on any cap, second sample point,
// numerical verdict only ("candidate only; target/consumer unidentified"). See docs/review/lens-scan-impl.md.
//
// What it NEVER does: write game memory (the glue hands it a Memory whose write_float always fails; the
// fixtures count calls), widen the search to code pages, or run unbounded. Kill: the stop file
// %LOCALAPPDATA%\acz-vr\lens-scan.stop (or the look-write.stop kill switch) ends the scan at once.
//
// Output (dedicated line family `lens-scan;event=...` in ee-camera.log + diagnostics) and ONE small file
// logs\lens-scan-candidates.txt whose last line is `sha256=<hex of every byte before that line>`; the same
// hash is logged (`event=file`) so an evidence card can pin it.
//
// SDK-free core shared by the Windows glue (ee_camera_probe_win.cpp) and the offline fixtures
// (tests/acz-vr/ee_lens_scan_offline_tests.cpp). Threads: tick() runs on the Present tick, work() and drain()
// on the probe's worker thread (the fixtures call them in sequence); phase_ is the only shared word.
#include "ee_look_write.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace acz::ee_camera {

// ------------------------------------------------------------------ SHA-256 (pins the candidate file)
struct Sha256 {
    Sha256() noexcept;
    void update(const void* p,size_t n) noexcept;
    void final(uint8_t out[32]) noexcept;
private:
    void block(const uint8_t* b) noexcept;
    uint32_t h_[8];uint64_t bytes_=0;uint8_t buf_[64];size_t fill_=0;
};
void sha256_hex(const void* p,size_t n,char out[65]) noexcept;

// ------------------------------------------------------------------ what is searched
enum LensKind : uint8_t {lk_tan=0,lk_scale,lk_tan16,lk_tan64,lk_angle_rad,lk_angle_deg,lk_kinds};
struct LensKindDef {const char* name;float x,y,tx,ty;};   // x/y = the two values, tx/ty = absolute tolerances
inline constexpr LensKindDef lens_kind[lk_kinds]={
    {"tan",       0.6152f,   0.3460f,   1e-3f,  1e-3f},
    {"scale",     1.6255f,   2.8902f,   3e-3f,  5e-3f},
    {"tan16",     9.8432f,   5.5360f,   0.016f, 0.016f},
    {"tan64",     39.3728f,  22.1440f,  0.064f, 0.064f},
    {"angle-rad", 1.1030f,   0.6663f,   1e-3f,  1e-3f},
    {"angle-deg", 63.2f,     38.1f,     0.1f,   0.1f},
};
inline constexpr float lens_prefilter_lo=0.34f,lens_prefilter_hi=64.0f;   // all targets +- tolerance lie inside

// ------------------------------------------------------------------ bounds (all hard, none configurable by key)
inline constexpr uint32_t lens_chunk_default=1u<<18;       // bytes read per guarded copy (256 KiB)
inline constexpr uint32_t lens_pair_window=64;             // the two values lie within this many bytes
inline constexpr unsigned lens_cand_cap=48,lens_kind_cap=12,lens_region_cap=4,lens_neigh=3;
inline constexpr uint64_t lens_byte_cap=256ull<<20;        // total bytes scanned
inline constexpr uint64_t lens_region_max=block_bytes;     // one region is clipped to 64 MiB
inline constexpr unsigned lens_frames=300;                 // cockpit frames tracked
inline constexpr unsigned lens_chunks_per_call=8;          // worker budget per call (2 MiB), then it yields
inline constexpr unsigned lens_settle_ticks=60;            // cockpit must be live this many ticks before the scan starts
inline constexpr double   lens_anchor_grace_ms=20000;      // wait this long for a tracks-look candidate, then scan without an anchor
inline constexpr double   lens_wait_cap_ms=300000;         // never saw a cockpit: give up
inline constexpr double   lens_scan_cap_ms=120000;
inline constexpr double   lens_track_cap_ms=180000;
inline constexpr double   lens_wait_log_ms=10000;
inline constexpr size_t   lens_file_cap=48*1024;
inline constexpr unsigned lens_log_cap=200;               // log lines per run (the file always holds everything that fits lens_file_cap)
inline constexpr unsigned lens_b_budget=16;               // candidates re-read at the second sample point (seam B) per frame

// Astra review 2026-10-26: the scanner only ever touches committed, readable DATA pages. The glue answers region queries
// (VirtualQuery); `data` = committed, not guard/no-access, not executable, not an image section, not write-combined/uncached.
// Step 47 run 1 (2026-10-26) diagnosis: the 64 MiB EE block is MEM_PRIVATE and passes the probe's rw_protect() check, yet the census
// (ProcessRegions) did NOT keep it, which only happens for write-combined/uncached protect modifiers; the first glue rejected every
// chunk on exactly that modifier. VirtualQuery state/type/protect are therefore carried raw (below) and classified by a pure function
// that the fixtures run with the real attribute values. Policy: committed, readable (RO/RW/WC base protect), not guard, not
// no-access, not executable, not an image section; PAGE_WRITECOMBINE / PAGE_NOCACHE are only tolerated on MEM_PRIVATE memory (an
// ordinary VirtualAlloc block: slow to read, harmless); on MEM_MAPPED they still mean device/GPU mappings and stay excluded.
inline constexpr uint32_t lw_mem_commit=0x1000,lw_mem_reserve=0x2000,lw_mem_free=0x10000,lw_mem_private=0x20000,lw_mem_mapped=0x40000,lw_mem_image=0x1000000;
inline constexpr uint32_t lw_pg_noaccess=0x01,lw_pg_readonly=0x02,lw_pg_readwrite=0x04,lw_pg_writecopy=0x08,lw_pg_exec=0x10,lw_pg_exec_read=0x20,
                          lw_pg_exec_rw=0x40,lw_pg_exec_wc=0x80,lw_pg_guard=0x100,lw_pg_nocache=0x200,lw_pg_writecombine=0x400;
enum LensPageClass : uint8_t {lpc_data=0,lpc_not_committed,lpc_no_access,lpc_guard,lpc_executable,lpc_image,lpc_wc_nocache_not_private,lpc_other_protect,lpc_count};
inline const char* lens_pageclass_name(uint8_t c) noexcept {
    static constexpr const char* n[]={"data","not-committed","no-access","guard","executable","image","write-combined-or-uncached-non-private","other-protect"};
    return c<lpc_count?n[c]:"?";
}
inline LensPageClass lens_classify(uint32_t state,uint32_t type,uint32_t protect) noexcept {
    const uint32_t b=protect&0xFF;
    if(state!=lw_mem_commit)return lpc_not_committed;
    if(protect&lw_pg_guard)return lpc_guard;
    if(b==lw_pg_noaccess||b==0)return lpc_no_access;
    if(b==lw_pg_exec||b==lw_pg_exec_read||b==lw_pg_exec_rw||b==lw_pg_exec_wc)return lpc_executable;
    if(type==lw_mem_image)return lpc_image;
    if(b!=lw_pg_readonly&&b!=lw_pg_readwrite&&b!=lw_pg_writecopy)return lpc_other_protect;
    if((protect&(lw_pg_nocache|lw_pg_writecombine))&&type!=lw_mem_private)return lpc_wc_nocache_not_private;
    return lpc_data;
}
// state/type/protect are the raw VirtualQuery values (0 = not supplied, e.g. fixtures that set the flags directly).
struct LensRegion {uint64_t base=0,size=0,alloc=0;bool committed=false,data=false,exec=false;uint32_t state=0,type=0,protect=0;};
struct LensMem : Memory {
    virtual bool query(uint64_t address,LensRegion& out) noexcept=0;
};

// Classes are NUMERICAL observations only. Identical samples cannot separate an immutable input, a stale copy and an output
// rewritten to the same value each frame (`static` / `stable-per-frame` say nothing about which one it is).
enum LensClass : uint8_t {lc_untracked=0,lc_static,lc_stable,lc_point_varies,lc_unstable,lc_unreadable};
inline const char* lens_class_name(uint8_t c) noexcept {
    static constexpr const char* n[]={"untracked","static","stable-per-frame","varies-between-sample-points","unstable","unreadable-or-identity-changed"};return c<6?n[c]:"?";
}

struct LensCand {
    uint8_t kind=0,cls=lc_untracked;uint32_t region=0;
    // identity of the page region the pair was found in (revalidated before every re-read; a change = unreadable)
    uint64_t idBase=0,idSize=0,idAlloc=0;
    // extra evidence (representation + surrounding structure), computed from the scan window
    uint8_t finiteN=0,zeroN=0,diag4x4=0,recip=0;
    uint64_t ax=0,ay=0;                       // absolute addresses of the two floats
    uint32_t bx0=0,by0=0;                     // bit patterns at scan time
    uint32_t nbx[2*lens_neigh]{},nby[2*lens_neigh]{};   // neighbouring words (3 before, 3 after) of each float, raw bits; 0 outside the region
    // tracking (Present tick)
    uint32_t reads=0,unread=0,changes=0,outside=0,identChanged=0;
    uint32_t lastx=0,lasty=0;bool haveLast=false;            // last sample at point A (Present tick)
    // second sample point B (first replayed member): a field that differs between the two points within/around a frame is NOT
    // a plain constant; equal samples prove nothing (a same-value rewrite is invisible to reads).
    uint32_t bReads=0,bChanges=0,crossDiffs=0,bx=0,by=0;bool haveB=false;
    float minx=0,maxx=0,miny=0,maxy=0;
};

struct LensIn {
    double now_ms=0;uint64_t frame=0;
    bool stop_file=false;                     // lens-scan.stop (or look-write.stop) exists
    LookView v;                               // cockpit, have/eff/lo/hi of the best tracks-look candidate
};
struct LensWorkIn {
    double now_ms=0;bool stop_file=false;
    bool have_blocks=false;const uint64_t* blocks=nullptr;unsigned nblocks=0;   // private RW 64 MiB blocks (CandidateCollector)
};
// Where the candidate file goes (the glue: a file; fixtures: a vector). Called from drain() only (never the render path).
struct LensOut {virtual bool write_file(const char* text,size_t n) noexcept=0;virtual ~LensOut()=default;};

class LensScanner {
public:
    enum Phase : uint8_t {p_off=0,p_waiting,p_scanning,p_tracking,p_finished,p_stopped,p_aborted};
    struct Options {
        bool enabled=false;uint32_t chunk_bytes=lens_chunk_default;uint64_t byte_cap=lens_byte_cap;
        unsigned frames=lens_frames,settle_ticks=lens_settle_ticks,chunks_per_call=lens_chunks_per_call;
        unsigned max_log_lines=lens_log_cap;
        double scan_cap_ms=lens_scan_cap_ms,track_cap_ms=lens_track_cap_ms,wait_cap_ms=lens_wait_cap_ms,anchor_grace_ms=lens_anchor_grace_ms;
    };
    explicit LensScanner(Options o=Options{}) noexcept;
    LensScanner(const LensScanner&)=delete;LensScanner& operator=(const LensScanner&)=delete;

    // Present tick (single caller): waiting gate, then the per-frame re-read of the candidates. READ-ONLY on `mem`.
    void tick(LensMem& mem,Sink& log,const LensIn& in) noexcept;
    // Worker thread (single caller): the bounded scan. READ-ONLY on `mem`. Cheap when nothing is to be done.
    void work(LensMem& mem,Sink& log,const LensWorkIn& in) noexcept;
    // Second sample point (glue: the first replayed member seam, same thread as the Present tick in all earlier probe runs). Cheap
    // (<= lens_b_budget candidates, once per frame, try-lock), read-only, no logging; ignored outside tracking.
    void sample_b(LensMem& mem) noexcept;
    bool needs_blocks() const noexcept {return phase()==p_scanning&&!targets_;}
    // Exit paths: ends whatever is running as a partial result (reason text), then drain() reports it. Idempotent.
    void finish(const char* why) noexcept;
    // Worker/exit path: when a terminal state was reached and not yet reported, logs the summary + candidate lines and
    // writes the hash-pinned file through `out`. Returns true when the file was written by THIS call.
    bool drain(Sink& log,LensOut& out) noexcept;

    Phase phase() const noexcept {return static_cast<Phase>(phase_.load(std::memory_order_acquire));}
    bool enabled() const noexcept {return o_.enabled;}
    bool terminal() const noexcept {const Phase p=phase();return p>=p_finished;}
    const char* reason() const noexcept {return reason_.load(std::memory_order_acquire);}
    unsigned candidates() const noexcept {return n_.load(std::memory_order_acquire);}
    const LensCand& candidate(unsigned i) const noexcept {return c_[i<lens_cand_cap?i:0];}
    unsigned found(unsigned kind) const noexcept {return kind<lk_kinds?found_[kind]:0;}
    unsigned dropped() const noexcept {return dropped_;}
    uint64_t scanned_bytes() const noexcept {return scanned_;}
    unsigned frames_tracked() const noexcept {return framesTracked_;}
    bool scan_complete() const noexcept {return scanComplete_;}
    bool written() const noexcept {return written_;}
    unsigned write_failures() const noexcept {return writeFail_;}
    const char* file_sha256() const noexcept {return sha_;}
    // The deterministic report text (the file without its sha256 line), also used by the fixtures. Returns the byte count;
    // `log` (optional) receives the same lines. Classification is (re)computed first. Not thread-safe against a running scan.
    size_t compose(char* buf,size_t cap,Sink* log) noexcept;

private:
    struct Region {uint64_t lo=0,bytes=0;bool anchor=false;};
    void set_phase(Phase p) noexcept {phase_.store(static_cast<uint8_t>(p),std::memory_order_release);}
    bool terminate(Phase to,const char* why) noexcept;      // moves from a non-terminal phase; arms the report
    void wait_log(Sink& log,double now,const char* why) noexcept;
    bool build_targets(Sink& log,const LensWorkIn& in) noexcept;
    bool scan_step(LensMem& mem) noexcept;
    bool sample_one(LensMem& mem,LensCand& c,bool pointB) noexcept;
    void say(Sink& log,bool important,const char* text) noexcept;                     // one chunk; false = the region is done
    void add_candidate(unsigned kind,unsigned region,uint64_t addrBase,const float* sc,unsigned nw,unsigned wx,unsigned wy,const LensRegion& q) noexcept;
    void classify() noexcept;
    Options o_;
    std::atomic<uint8_t> phase_{p_off};std::atomic<const char*> reason_{"not-started"};
    std::atomic<bool> pending_{false};std::atomic_flag drainBusy_=ATOMIC_FLAG_INIT;
    // anchor / waiting (Present tick)
    double waitStart_=-1,cockpitSince_=-1,lastWait_=-1e300;const char* lastWhy_="";unsigned cockpitTicks_=0;uint64_t lastFrame_=~0ull;
    uint64_t anchorLo_=0,anchorHi_=0,anchorEff_=0;bool haveAnchor_=false;double scanArmedMs_=0;
    // targets + scan (worker)
    Region reg_[lens_region_cap]{};unsigned nreg_=0,ri_=0;uint64_t pos_=0;bool targets_=false;
    double scanStart_=-1,scanMs_=0;uint64_t scanned_=0;unsigned chunks_=0,unreadable_=0;bool scanComplete_=false;const char* scanStop_="running";
    unsigned found_[lk_kinds]{},perKind_[lk_kinds]{};unsigned singles_[lk_kinds][2]{};unsigned dropped_=0;
    LensCand c_[lens_cand_cap]{};std::atomic<unsigned> n_{0};
    // tracking (Present tick)
    double trackStart_=0;unsigned framesTracked_=0,framesOff_=0;bool trackDone_=false;
    // coverage / caps (any of these makes the coverage UNKNOWN, never "not there")
    // first rejected chunk (why a rerun finds nothing): address window, queried region + raw attributes, reason; plus counts per page class
    struct Reject {bool have=false;uint64_t addr=0,len=0,qbase=0,qsize=0,qalloc=0;uint32_t state=0,type=0,protect=0;const char* reason="";} firstRej_{};
    unsigned rejByClass_[lpc_count]{},rejOther_=0;
    void note_reject(uint64_t a,uint64_t n,bool queried,const LensRegion& q,const char* reason) noexcept;
    uint64_t unreadableBytes_=0;unsigned excludedChunks_=0,blocksOffered_=0,blocksSkipped_=0;unsigned logCount_=0,logDropped_=0;bool reportLogged_=false,truncated_=false;
    std::atomic_flag trackBusy_=ATOMIC_FLAG_INIT;uint64_t bFrame_=~0ull;
    // report
    char sha_[65]{};bool written_=false;unsigned writeFail_=0;
    float scratch_[(lens_chunk_default+2*lens_pair_window)/4+16]{};
    char file_[lens_file_cap]{};
};

} // namespace acz::ee_camera
