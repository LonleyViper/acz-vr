#pragma once
// Option 3, stage 2 (D-266, D-267): the minimal REAL production write path for the
// game's own look state. SDK-free core shared by the Windows glue
// (ee_camera_probe_win.cpp) and the offline fixtures (ee_look_write_offline_tests.cpp).
//
//  * Stage 2a  (key ee_camera_look_write_trial, default OFF): ONE bounded trial per
//    process on a validated tracks-look candidate. After a 3 s settle it writes a
//    known yaw (default +0.5, clamped |v|<=1.0, the field's own units) to the
//    look-yaw float for 120 consecutive Present ticks, logging every write and
//    read-backs of the field and of the composed camera fields, then restores the
//    original (neutral) value and verifies by read-back. It writes NOTHING else.
//  * Stage 2b  (key ee_camera_look_drive, default OFF, needs a successful 2a in the
//    same process): drives the look yaw/pitch floats from the head. The head source
//    (LookHeadMailbox) is NOT yet published by the producer (see the findings note);
//    without it the drive never writes.
//
// Nothing here knows about hooks, threads or page protection: the Memory the glue
// passes in does the VirtualQuery/SEH checks (writable private pages only). Every
// write goes through Memory::write_float of that object; the state machines never
// call anything else.
#include "ee_camera_probe.h"
#include <atomic>
#include <cstdint>

namespace acz::ee_camera {

// ------------------------------------------------------------------ constants
inline constexpr float  look_write_default=0.5f;     // stage 2a yaw, field units
inline constexpr float  look_write_clamp=1.0f;       // |written yaw| <= this, whatever is configured
inline constexpr float  look_neutral_band=0.02f;     // the original must be this close to 0 (look released)
inline constexpr float  look_readback_limit=1.25f;   // a field read outside +-this aborts stage 2a (the game's saturation is 2.48)
inline constexpr unsigned look_trial_frames=120;     // writes, one per Present tick
inline constexpr unsigned look_trial_cap=240;        // storage cap for the samples
inline constexpr unsigned look_post_frames=30;       // read-only ticks logged after the restore
inline constexpr unsigned look_min_samples=40;       // timeline samples of the candidate (~10 s at 250 ms)
inline constexpr unsigned look_min_events=6;         // look events with >= 80 % hits
inline constexpr unsigned look_still_ticks=30;       // aircraft position unchanged this many ticks = paused
inline constexpr double look_settle_ms=3000;
// J-look-locator: the settle only completes after the look has been quiet this long (yaw and pitch
// neutral, no look event); a look event during the settle RESTARTS the settle (it is not an abort).
inline constexpr double look_quiet_ms=2000;
inline constexpr float  look_pitch_neutral_band=0.05f;   // 2.5 x look_neutral_band, the same band the drive uses
inline constexpr double look_struct_max_age_ms=1500;     // the structural re-check must have passed this recently
inline constexpr double look_deadline_ms=10000;      // from the first write
inline constexpr double look_wait_log_ms=10000;
// Stage 2b limits (conservative: inside what run 4 observed; the game saturates at 2.48).
inline constexpr float  drive_yaw_clamp=2.0f;
inline constexpr float  drive_pitch_up=1.2f,drive_pitch_down=-0.1f; // look-down was only ever seen to -0.035
inline constexpr float  drive_rate_per_tick=0.10f;    // max change per Present tick (~5.5 units/s at 55 fps)
inline constexpr float  drive_deadzone_rad=0.0087f;   // 0.5 degrees of head angle
inline constexpr float  drive_bound_margin=0.25f;     // read-back beyond clamp+margin aborts
inline constexpr double drive_max_ms=10*60*1000.0;    // one drive session per process
inline constexpr double drive_head_stale_ms=250;
inline constexpr unsigned drive_log_first=120,drive_log_every=30;

// ------------------------------------------------------------------ what the probe tells the writer
struct LookView {
    bool have=false;                         // a tracks-look candidate exists
    uint64_t eff=0,lo=0,hi=0;                // its effective base and the region its fields lie in
    unsigned samples=0,look_ev=0,look_hit=0; // timeline evidence behind the class
    bool locked_present=false;               // the writer's locked eff is still tracked by the probe ...
    uint64_t locked_lo=0,locked_hi=0;        // ... with this region
    bool cockpit=false;                      // the producer published a basis within cockpit_ticks
    // Defaults refuse: "a look event just happened" and "structure never confirmed".
    double quiet_ms=0;                       // ms since the last look event (key hold / look word moving) seen by the probe
    double struct_age_ms=1e9;                // ms since the structure at `eff` (locked eff when present) passed the per-tick re-check
    bool basis_ok=false;uint64_t basis_seq=0;double srt_yaw=0,srt_pitch=0,srt_air_yaw=0; // recovered rendered camera
};
struct LookIn {
    double now_ms=0;uint64_t frame=0;uint32_t tid=0;
    bool stop_file=false;                    // %LOCALAPPDATA%\acz-vr\look-write.stop exists
    LookView v;
};

// ------------------------------------------------------------------ stage 2a
struct LookResult {
    bool attempted=false,aborted=false,restored=false,responded=false,srt_valid=false;
    const char* reason="not-started";
    unsigned writes=0,overwritten=0;int lag_ticks=-1;
    double persisted=0,follow=0,comp_gain=0,srt_gain=0;   // srt_gain: rad of recovered camera yaw per field unit
    float orig=0,written=0,final_value=0;
};
class LookWriter {
public:
    enum Phase : uint8_t {p_off=0,p_waiting,p_settling,p_writing,p_post,p_done,p_aborted};
    struct Options {bool enabled=false;float yaw=look_write_default;unsigned frames=look_trial_frames;
                    double settle_ms=look_settle_ms,deadline_ms=look_deadline_ms;};
    explicit LookWriter(Options o=Options{}) noexcept;
    // One Present tick (single caller at a time).
    void tick(Memory& mem,Sink& log,const LookIn& in) noexcept;
    // EVERY exit path (shutdown, DLL detach, glue exception): restores the original value if
    // anything was written and not yet verified restored. Idempotent; never writes otherwise.
    void abort_and_restore(Memory& mem,Sink& log,const char* why) noexcept;
    Phase phase() const noexcept {return phase_;}
    bool enabled() const noexcept {return o_.enabled;}
    bool dirty() const noexcept {return dirty_;}                 // written, restore not yet verified
    bool attempted() const noexcept {return res_.attempted;}
    bool succeeded() const noexcept {return phase_==p_done&&res_.responded&&res_.restored&&!res_.aborted;}
    const LookResult& result() const noexcept {return res_;}
    uint64_t eff() const noexcept {return eff_;}
    uint64_t locked_eff() const noexcept {return phase_>=p_settling&&phase_<=p_post?eff_:0;}
    // Stage 2b calibration from the trial (valid when succeeded()): field units per radian of
    // visible rotation, from the recovered camera when it responded, else from the composed field.
    bool calibration(double& units_per_rad,const char** source) const noexcept;
private:
    struct Sample {float pre=0,written=0,comp=0;double srt=0;bool srt_ok=false;uint64_t basis=0;};
    bool rd(Memory& m,unsigned f,float& out) const noexcept;
    bool in_block(uint64_t a) const noexcept {return a>=lo_&&a+4>=a&&a+4<=hi_;}
    const char* pre_checks(Memory& m,const LookIn& in,float& yaw,float* cam_yaw,float* cam_pitch,float* heading,float* copy,float* look_pitch) noexcept;
    bool still(Memory& m) noexcept;
    bool restore(Memory& m,Sink& log,const char* why) noexcept;
    void wait_log(Sink& log,double now,const char* why) noexcept;
    void finish(Sink& log,const char* result,const char* reason) noexcept;
    void analyse() noexcept;
    Options o_;Phase phase_=p_off;LookResult res_{};
    uint64_t eff_=0,lo_=0,hi_=0,lastFrame_=~0ull;uint32_t lastAir_[3]{};bool haveAir_=false;unsigned stillTicks_=0;
    double settleStart_=0,trialStart_=0,lastWait_=-1e300;const char* lastWhy_="";bool settleHeld_=false;unsigned settleRestarts_=0;
    float orig_=0,target_=0;bool dirty_=false;unsigned n_=0,post_=0;
    float baseComp_=0,heading0_=0;double baseSrt_=0;bool baseSrtOk_=false;uint64_t baseBasis_=0;
    float prevNew_=0;Sample s_[look_trial_cap]{};
};

// ------------------------------------------------------------------ stage 2b
// Head orientation, neutral-relative, in radians. yaw > 0 = looking LEFT (counter-clockwise from
// above), pitch > 0 = looking UP: the same signs as the game's look fields in run 4.
struct LookHeadMailbox {
    void publish(double yaw,double pitch,double now_ms) noexcept {
        const uint32_t s=seq_.load(std::memory_order_relaxed);
        seq_.store(s+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
        yaw_.store(yaw,std::memory_order_relaxed);pitch_.store(pitch,std::memory_order_relaxed);ms_.store(now_ms,std::memory_order_relaxed);
        seq_.store(s+2,std::memory_order_release);
    }
    bool read(double& yaw,double& pitch,double& ms) const noexcept {
        const uint32_t s1=seq_.load(std::memory_order_acquire);
        if(s1==0||(s1&1u))return false;
        const double y=yaw_.load(std::memory_order_relaxed),p=pitch_.load(std::memory_order_relaxed),t=ms_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(seq_.load(std::memory_order_relaxed)!=s1)return false;
        yaw=y;pitch=p;ms=t;return true;
    }
private:
    std::atomic<uint32_t> seq_{0};std::atomic<double> yaw_{0},pitch_{0},ms_{0};
};
// Process-wide head source. NOTHING publishes into it yet (TODO in J-look-write-findings.md:
// the step-5 producer must publish the neutral-relative head yaw/pitch of the frame it renders).
inline LookHeadMailbox g_look_head;

// Neutral-relative head yaw/pitch from a row-major 3x3 head rotation R (OpenXR: forward = R*(0,0,-1))
// and the seated-recentering heading yaw0 (gpu_copy_step5.h Neutral::yaw0, same sign convention).
bool head_yaw_pitch(const double R[9],double yaw0,double& yaw,double& pitch) noexcept;
// Design note + helper for the DOUBLE ROTATION problem: once the game look follows the head, the
// camera the producer decomposes (VP_ref) already contains `rendered_look`; the per-eye delta must be
// the head orientation MINUS that part, not the neutral-relative head orientation.
inline double residual_after_game_look(double head,double rendered_look) noexcept {return wrap(head-rendered_look);}
// Head angle -> field units (deadzone, gain, clamp). Pure.
float head_to_units(double head_rad,double units_per_rad,float lo,float hi) noexcept;
float rate_limit(float from,float to,float max_step) noexcept;

struct DriveHead {bool valid=false;double yaw=0,pitch=0,age_ms=1e9;};
struct DriveIn {
    LookIn base;                      // now/frame/tid/stop_file/view (view.locked_* refer to the drive's eff)
    bool gate_ok=false;               // LookWriter::succeeded() in this process
    uint64_t gate_eff=0;              // the base stage 2a succeeded on: the drive uses only this one
    double units_per_rad=1.0;bool calibrated=false;
    DriveHead head;
};
class LookDrive {
public:
    enum Phase : uint8_t {p_off=0,p_waiting,p_active,p_done,p_aborted};
    struct Options {bool enabled=false;double max_ms=drive_max_ms;};
    explicit LookDrive(Options o=Options{}) noexcept:o_(o) {phase_=o.enabled?p_waiting:p_off;}
    void tick(Memory& mem,Sink& log,const DriveIn& in) noexcept;
    void abort_and_restore(Memory& mem,Sink& log,const char* why) noexcept;
    Phase phase() const noexcept {return phase_;}
    bool dirty() const noexcept {return dirty_;}
    unsigned writes() const noexcept {return writes_;}
    const char* reason() const noexcept {return reason_;}
    uint64_t locked_eff() const noexcept {return phase_==p_active?eff_:0;}
private:
    bool rd(Memory& m,unsigned f,float& out) const noexcept;
    bool in_block(uint64_t a) const noexcept {return a>=lo_&&a+4>=a&&a+4<=hi_;}
    bool restore(Memory& m,Sink& log,const char* why) noexcept;
    void wait_log(Sink& log,double now,const char* why) noexcept;
    Options o_;Phase phase_=p_off;const char* reason_="not-started";
    uint64_t eff_=0,lo_=0,hi_=0,lastFrame_=~0ull;double start_=0,lastWait_=-1e300;const char* lastWhy_="";
    float orig_[2]{},last_[2]{};bool dirty_=false;unsigned writes_=0,still_=0;uint32_t lastAir_[3]{};bool haveAir_=false;
};

} // namespace acz::ee_camera
