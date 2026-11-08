#pragma once
// PS2 address probe (docs/world-render/ps2-probe-impl.md, D-293 follow-up; key ee_ps2_probe, default OFF): a READ-ONLY, bounded probe of the
// 64 MiB EE-image block the camera probe located (the tracks-look anchor block). It tests, with block-relative offsets ASSUMED to be PS2 EE
// addresses (the ACZGCT cheat scripts, docs/world-render/ps2-free-camera-cheats.md; web data, unverified for this port), whether that image
// layout holds and logs everything an offline analysis needs:
//   (1) fixed offsets +0x3FFD1C (mission byte), +0x3ACEA0, +0x76587C, +0x765320 (float draw-distance candidate) and every game-id hit found by
//       scanning +0x300000..+0x4000000 for 'SLUS_213.46'-style ids (SLUS/SLES/SLPM/SLPS/SCUS/SCES/SCPS) and for the cheat's 16-byte game-id
//       pattern: value + 32 bytes of context, every 2 s, 20 samples;
//   (2) a scan of +0x800000..+0x1F00000 for the camera struct signature float32 512.0, 448.0 (0x44000000, 0x43E00000) at +0/+4; for each hit
//       (max 4) the distortion pair +0x198/+0x19C, the background pair +0x1C0/+0x1C4, position +0xB30..+0xB50 (9 floats), rotation
//       +0xC20..+0xC28 (3 floats), 10 samples 1 s apart, next to the E6 camera fields (+0xFDD410..+0xFDD878) read the same second;
//   (3) the mission byte with the camera position (first struct hit and E6) once per second for 60 s.
// Gates and discipline are those of the lens scanner (ee_lens_scan.h): committed readable DATA pages only (lens_classify), the whole window inside
// ONE queried region whose allocation is the one seen at the start, all-or-nothing guarded reads, every cap explicit and coverage UNKNOWN whenever
// a cap was hit / a chunk or value was unreadable / the run ended early. It NEVER writes game memory (the glue hands it a Memory whose
// write_float always fails; the fixtures count calls). Kill: %LOCALAPPDATA%\acz-vr\lens-scan.stop or look-write.stop (the glue's stop check
// is shared with the lens scanner); a partial report is still written.
//
// Output: line family `ps2-probe;event=...` in ee-camera.log + diagnostics, and ONE file logs\ps2-probe.txt whose last line is
// `sha256=<hex of every byte before that line>` (the same hash is logged as event=file).
//
// Threads: tick() runs on the Present tick (only stores the anchor, stop flag), work() and drain() on the probe worker (the only logger).
#include "ee_lens_scan.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace acz::ee_camera {

// ------------------------------------------------------------------ what is read (block-relative offsets = assumed PS2 addresses)
inline constexpr uint64_t pp_off_mission=0x3FFD1C,pp_off_gate=0x3ACEA0,pp_off_phase=0x76587C,pp_off_draw=0x765320;
inline constexpr uint64_t pp_id_lo=0x300000,pp_id_hi=0x4000000;           // game-id scan range
inline constexpr uint64_t pp_cam_lo=0x800000,pp_cam_hi=0x1F00000;         // camera-struct scan range
inline constexpr uint32_t pp_sig_w=0x44000000u,pp_sig_h=0x43E00000u;      // float32 512.0, 448.0
// camera struct fields (relative to the hit)
inline constexpr uint32_t pp_c_dist=0x198,pp_c_bg=0x1C0,pp_c_pos=0xB30,pp_c_rot=0xC20;   // dist pair, background pair, 9 position floats, 3 rotation floats
// E6 fields (block-relative; research/experiments/E6-2026-10-12.md)
inline constexpr uint64_t pp_e6_yaw=0xFDD410,pp_e6_air=0xFDD42C,pp_e6_hdg=0xFDD448,pp_e6_pos=0xFDD860,pp_e6_rot=0xFDD870;

// ------------------------------------------------------------------ bounds (all hard, none configurable by key)
inline constexpr uint32_t pp_chunk_default=1u<<18;       // bytes per guarded copy (256 KiB); the window adds pp_overlap
inline constexpr uint32_t pp_overlap=16;                 // bytes read beyond the chunk so that matches straddling a chunk edge are seen
inline constexpr unsigned pp_cam_cap=4,pp_gid_cap=6,pp_aob_cap=2;
inline constexpr uint64_t pp_byte_cap=128ull<<20;        // total bytes scanned (both passes need ~86 MiB)
inline constexpr unsigned pp_chunks_per_call=8;          // worker budget per call (2 MiB), then it yields
inline constexpr unsigned pp_settle_ticks=60;            // the same tracks-look anchor must be present this many distinct Present frames
inline constexpr double   pp_scan_cap_ms=60000,pp_wait_cap_ms=300000,pp_wait_log_ms=10000;
inline constexpr unsigned pp_fixed_samples=20;           // every 2 s
inline constexpr unsigned pp_cam_samples=10;             // every 1 s
inline constexpr unsigned pp_stage_samples=60;           // every 1 s (this is the run length after the scans: 60 s)
inline constexpr double   pp_sample_period_ms=1000;
inline constexpr size_t   pp_file_cap=96*1024;           // report file; the last pp_file_tail bytes are reserved for the summary lines
inline constexpr size_t   pp_file_tail=3*1024;
inline constexpr unsigned pp_log_cap=600;                // log lines per run (the file holds everything that fits pp_file_cap)

struct Ps2WorkIn {double now_ms=0;bool stop_file=false;};
struct Ps2Hit {uint64_t off=0;char text[16]{};};          // block-relative offset; text = the id string (game-id hits)

class Ps2Prober {
public:
    enum Phase : uint8_t {p_off=0,p_waiting,p_scanning,p_sampling,p_finished,p_stopped,p_aborted};
    struct Options {
        bool enabled=false;uint32_t chunk_bytes=pp_chunk_default;uint64_t byte_cap=pp_byte_cap;
        unsigned chunks_per_call=pp_chunks_per_call,settle_ticks=pp_settle_ticks,max_log_lines=pp_log_cap;
        double scan_cap_ms=pp_scan_cap_ms,wait_cap_ms=pp_wait_cap_ms;size_t file_cap=pp_file_cap;
    };
    explicit Ps2Prober(Options o=Options{}) noexcept;
    Ps2Prober(const Ps2Prober&)=delete;Ps2Prober& operator=(const Ps2Prober&)=delete;

    // Present tick (single caller): only records the tracks-look anchor (lo of its region) and the stop flag. No memory access, no logging.
    void tick(const LensIn& in) noexcept;
    // Worker thread (single caller): waiting gate, bounded scans, timed samples. READ-ONLY on `mem`. Cheap when nothing is to be done.
    void work(LensMem& mem,Sink& log,const Ps2WorkIn& in) noexcept;
    // Exit paths: ends whatever is running as a partial result (reason text); drain() reports it. Idempotent.
    void finish(const char* why) noexcept;
    // Worker/exit path: when a terminal state was reached and not yet reported, logs the summary and writes the hash-pinned file through `out`.
    bool drain(Sink& log,LensOut& out) noexcept;

    Phase phase() const noexcept {return static_cast<Phase>(phase_.load(std::memory_order_acquire));}
    bool enabled() const noexcept {return o_.enabled;}
    bool terminal() const noexcept {return phase()>=p_finished;}
    const char* reason() const noexcept {return reason_.load(std::memory_order_acquire);}
    uint64_t base() const noexcept {return base_;}
    unsigned cam_hits() const noexcept {return nCam_;}
    unsigned gid_hits() const noexcept {return nGid_;}
    unsigned aob_hits() const noexcept {return nAob_;}
    const Ps2Hit& cam_hit(unsigned i) const noexcept {return cam_[i<pp_cam_cap?i:0];}
    const Ps2Hit& gid_hit(unsigned i) const noexcept {return gid_[i<pp_gid_cap?i:0];}
    const Ps2Hit& aob_hit(unsigned i) const noexcept {return aob_[i<pp_aob_cap?i:0];}
    unsigned cam_total() const noexcept {return camTotal_;}
    unsigned bg_pair_hits() const noexcept {return bgPair_;}
    unsigned gid_total() const noexcept {return gidTotal_;}
    unsigned aob_total() const noexcept {return aobTotal_;}
    uint64_t scanned_bytes() const noexcept {return scanned_;}
    unsigned stage_done() const noexcept {return stageDone_;}
    unsigned fixed_done() const noexcept {return fixedDone_;}
    unsigned cam_done() const noexcept {return camDone_;}
    unsigned unreadable_chunks() const noexcept {return unreadable_;}
    unsigned excluded_chunks() const noexcept {return excluded_;}
    unsigned value_unreadable() const noexcept {return valUnread_;}
    unsigned rot_matches_e6(unsigned hit) const noexcept {return hit<pp_cam_cap?rotEq_[hit]:0;}
    unsigned log_lines() const noexcept {return logCount_;}
    unsigned log_dropped() const noexcept {return logDropped_;}
    bool coverage_known() const noexcept {return coverageKnown_;}
    bool scan_complete() const noexcept {return scanComplete_;}
    bool written() const noexcept {return written_;}
    unsigned write_failures() const noexcept {return writeFail_;}
    const char* file_sha256() const noexcept {return sha_;}

private:
    void set_phase(Phase p) noexcept {phase_.store(static_cast<uint8_t>(p),std::memory_order_release);}
    bool terminate(Phase to,const char* why) noexcept;
    void say(Sink& log,bool important,const char* text,bool essential=false) noexcept;
    void work_locked(LensMem& mem,Sink& log,const Ps2WorkIn& in) noexcept;
    bool scan_step(LensMem& mem,Sink& log,double now) noexcept;
    void scan_chunk(LensMem& mem,Sink& log,uint64_t pos,uint64_t n,uint64_t hi,bool camPass) noexcept;
    void end_pass(Sink& log,double now) noexcept;
    void begin_sampling(Sink& log,double now) noexcept;
    void sample(LensMem& mem,Sink& log,double now) noexcept;
    void sample_fixed(LensMem& mem,Sink& log,unsigned s,double now) noexcept;
    void sample_cam(LensMem& mem,Sink& log,unsigned s,double now) noexcept;
    bool rd(LensMem& mem,uint64_t off,void* out,uint32_t n) noexcept;      // gated, bounded, all-or-nothing read of base_+off
    void note_reject(uint64_t off,uint64_t n,const char* reason,const LensRegion* q) noexcept;
    void summary(Sink& log) noexcept;

    Options o_;
    std::atomic<uint8_t> phase_{p_off};std::atomic<const char*> reason_{"not-started"};
    std::atomic<bool> pending_{false},stop_{false};std::atomic<const char*> finishReq_{nullptr};
    std::atomic_flag drainBusy_=ATOMIC_FLAG_INIT,workBusy_=ATOMIC_FLAG_INIT;
    // anchor (Present tick -> worker)
    std::atomic<uint64_t> anchorLo_{0};std::atomic<unsigned> anchorTicks_{0};uint64_t lastFrame_=~0ull;
    // worker state
    uint64_t base_=0,allocId_=0;double waitStart_=-1,lastWait_=-1e300,scanStart_=-1,t0_=0,runMs_=0;
    unsigned pass_=0;uint64_t pos_=0;bool passBegun_=false;uint64_t passBytes0_=0;unsigned passHits0_=0;
    uint64_t scanned_=0,unreadableBytes_=0;unsigned unreadable_=0,excluded_=0,identChanged_=0,chunks_=0,valUnread_=0,lateSamples_=0;
    bool scanComplete_=false;const char* scanStop_="running";bool anchorMoved_=false;
    Ps2Hit cam_[pp_cam_cap]{},gid_[pp_gid_cap]{},aob_[pp_aob_cap]{};uint64_t lastPrim_=0;bool havePrim_=false;unsigned bgPair_=0;
    unsigned nCam_=0,nGid_=0,nAob_=0,camTotal_=0,gidTotal_=0,aobTotal_=0;
    struct Reject {bool have=false;uint64_t off=0,len=0,qbase=0,qsize=0,qalloc=0;uint32_t state=0,type=0,protect=0;const char* reason="";} firstRej_{};
    // sampling
    unsigned sIdx_=0,fixedDone_=0,camDone_=0,stageDone_=0;
    unsigned rotEq_[pp_cam_cap]{},posEq_[pp_cam_cap]{},camReads_[pp_cam_cap]{};
    uint8_t missionSeen_[8]{};unsigned nMission_=0,missionChanges_=0;int lastMission_=-1;bool missionOverflow_=false;
    // report
    bool coverageKnown_=false;unsigned logCount_=0,logDropped_=0;bool truncated_=false,capHit_=false;
    char sha_[65]{};bool written_=false;unsigned writeFail_=0;size_t fileLen_=0;
    uint8_t scratch_[pp_chunk_default+pp_overlap+64]{};
    char file_[pp_file_cap+64]{};
};

} // namespace acz::ee_camera
