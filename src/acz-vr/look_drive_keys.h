#pragma once
// Option 3 via OS keyboard input (key xr_look_drive_keys, default OFF; D-266/D-267, J-look-drive-findings.md).
//
// The game's look is a first-order filtered (tau ~0.1 s) response to the digital NumPad look keys, saturating at
// +-2.48 rad (yaw) and +1.57 rad (pitch up; almost no look down) (research/experiments/look-pwm-run1, run G).
// A key switched at frame cadence with first-order sigma-delta therefore yields a filtered look of about
// duty * saturation. This module drives that look from the head so that the game itself renders the right part of
// the cockpit and the world for the head direction. Nothing here writes game memory or hooks anything: the only
// side effect is OS keyboard input through the KeyInjector interface (real implementation: look_drive_keys.cpp).
//
// Header-only logic (so the D3D hook test fixtures need no extra object file); the Windows injector and its
// registration live in look_drive_keys.cpp (linked into the DLL and the look-drive tests only).
//
// Sign convention (the producer's, = the game's look fields): yaw > 0 = looking LEFT, pitch > 0 = looking UP.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::look_drive {

inline constexpr double kPi=3.14159265358979323846;
inline double wrap_pi(double a) noexcept {while(a>kPi)a-=2*kPi;while(a<-kPi)a+=2*kPi;return a;}
inline double clampd(double v,double lo,double hi) noexcept {return v<lo?lo:(v>hi?hi:v);}

// ------------------------------------------------------------------ measured game dynamics (look-pwm-run1)
inline constexpr double yaw_full_rad=2.48;       // steady-state yaw look with the key held (duty 1)
inline constexpr double pitch_full_rad=1.5708;   // steady-state pitch look (up key); pitch down is not available
inline constexpr double filter_tau_s=0.1;        // look filter time constant (0.100 .. 0.13 s measured)
// ------------------------------------------------------------------ limits
inline constexpr double yaw_clamp_rad=2.3;       // target |yaw| (game saturates at 2.48)
inline constexpr double pitch_max_rad=1.4;       // target pitch upper clamp; the lower clamp is 0 (down is unavailable)
inline constexpr double deadzone_rad=0.0087;     // 0.5 degree
inline constexpr double target_rate_rad_s=6.0;   // small rate limit of the target (the filter itself is slower)
inline constexpr double max_key_down_ms=400.0;   // hard cap of continuous key-down time (controller)
inline constexpr double hard_key_down_ms=500.0;  // defence in depth in the real injector
inline constexpr double max_frame_gap_ms=100.0;  // a longer gap between ticks releases everything
inline constexpr double watchdog_ms=250.0;       // injector releases all keys when no update arrives this long
inline constexpr unsigned log_line_cap=600;      // bounded: one line per second, ~10 minutes

// Physical keys: NumPad 8 / 4 / 6 (virtual keys 0x68 / 0x64 / 0x66, scan codes 0x48 / 0x4B / 0x4D, NOT extended),
// exactly the events tools/live/acz-window-driver.ps1 and the run-G burst tool (look-pwm.ps1) sent.
enum KeyBit : uint8_t {key_left=1,key_right=2,key_up=4};

// ------------------------------------------------------------------ injector interface
struct KeyInjector {
    virtual ~KeyInjector()=default;
    virtual bool foreground() noexcept=0;                 // the foreground window belongs to this process
    virtual bool kill_switch() noexcept=0;                // %LOCALAPPDATA%\acz-vr\look-drive.stop exists
    // The request of one decision. EVERY decision is published, including the empty one (the producer never skips a zero): a
    // zero overwrites a request the consumer has not acted on yet, whether or not any key is down. Implementations send edges only.
    virtual void apply(uint8_t mask,double now_ms) noexcept=0;
    // Synchronous key-up of everything held. true = every key is confirmed up; false = at least one release was not confirmed
    // (the fault stays latched in fault() and a release-only owner keeps retrying until success or process exit).
    virtual bool release_all() noexcept=0;
    virtual uint8_t down() const noexcept=0;              // keys currently held at the OS level (a failed key-up leaves the bit set)
    virtual unsigned fault() const noexcept {return 0;}   // Fault bits, see below (0 = healthy); reported by the Driver log
    // Analog route (xr_look_drive_xinput, look_drive_xinput.h): the look is sent as a right-stick offset instead of keys. An analog
    // injector never depends on window focus (foreground() is true) and receives apply_axes() instead of apply().
    virtual bool analog() const noexcept {return false;}
    // yaw_duty in [-1,1] (+ = look LEFT), pitch_duty in [0,1] (+ = look UP): the commanded fraction of the game's full look.
    virtual void apply_axes(double yaw_duty,double pitch_duty,double now_ms) noexcept {(void)yaw_duty;(void)pitch_duty;(void)now_ms;}
    // A short diagnostic text for the per-second log line (foreground window, send counters, IPC state). Empty = nothing to add.
    virtual void status_text(char* buf,size_t n) noexcept {if(buf&&n)buf[0]=0;}
};
// Injection faults (KeyInjector::fault()). thread_start is permanent for the process; release_pending is set while a key-up is
// unconfirmed and cleared once every required key-up succeeded; shutdown_unconfirmed is sticky (a shutdown could not confirm).
enum Fault : unsigned {fault_none=0,fault_thread_start=1,fault_release_pending=2,fault_shutdown_unconfirmed=4,fault_no_ipc=8};
using InjectorFactory=KeyInjector*(*)() noexcept;
inline std::atomic<InjectorFactory> g_injector_factory{nullptr}; // set by look_drive_keys.cpp in the DLL
inline std::atomic<InjectorFactory> g_analog_injector_factory{nullptr}; // the XInput (right-stick) route, same registration

// ------------------------------------------------------------------ measured look from the rendered camera
// The camera basis (right/up/fwd) of the frame relative to the cockpit model's local frame (rows of the +256 matrix:
// the cockpit is fixed to the aircraft, the camera rotates with the look). R maps CAMERA-local coordinates
// (x right, y up, -z forward) to COCKPIT-local coordinates; with no look it is the identity.
struct LookMeasure {
    bool valid=false;const char* why="not-evaluated";
    double R[9]{1,0,0,0,1,0,0,0,1};   // row-major
    double yaw=0,pitch=0;             // radians (yaw > 0 left, pitch > 0 up)
    double E[9]{1,0,0,0,1,0,0,0,1};   // the unit cockpit rows this measurement used (valid only): the aircraft reference of the frame
};
inline void look_matrix(double yaw,double pitch,double R[9]) noexcept { // R = Ry(yaw) * Rx(pitch)
    const double cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
    R[0]=cy;R[1]=sy*sp;R[2]=sy*cp;
    R[3]=0;R[4]=cp;R[5]=-sp;
    R[6]=-sy;R[7]=cy*sp;R[8]=cy*cp;
}
inline double dot3(const double* a,const double* b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool unit3(const double* in,double* out) noexcept {
    const double n=std::sqrt(dot3(in,in));
    if(!(n>1e-9)||!std::isfinite(n))return false;
    for(int k=0;k<3;++k)out[k]=in[k]/n;return true;
}
inline void cross3(const double* a,const double* b,double* o) noexcept {
    o[0]=a[1]*b[2]-a[2]*b[1];o[1]=a[2]*b[0]-a[0]*b[2];o[2]=a[0]*b[1]-a[1]*b[0];
}
// right/up/fwd: the producer's recovered camera (gpu_copy_step5.h Camera). m256: row-major 4x4 local->world of the
// cockpit member (row-vector convention: rows 0..2 are the model's local axes in world coordinates; scale tolerated).
inline bool measure_look_rows(const double (&rows)[3][3],const double* right,const double* up,const double* fwd,LookMeasure& out) noexcept {
    out=LookMeasure{};
    double r[3][3],a[3][3];
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)r[i][j]=rows[i][j];
    const double nf[3]={-fwd[0],-fwd[1],-fwd[2]};
    if(!unit3(right,a[0])||!unit3(up,a[1])||!unit3(nf,a[2])){out.why="camera-degenerate";return false;}
    if(std::fabs(dot3(r[0],r[1]))>0.02||std::fabs(dot3(r[0],r[2]))>0.02||std::fabs(dot3(r[1],r[2]))>0.02){out.why="cockpit-matrix-not-rigid";return false;}
    if(std::fabs(dot3(a[0],a[1]))>0.02||std::fabs(dot3(a[0],a[2]))>0.02||std::fabs(dot3(a[1],a[2]))>0.02){out.why="camera-not-orthonormal";return false;}
    double cx[3];
    cross3(r[1],r[2],cx);const double detE=dot3(r[0],cx);
    cross3(a[1],a[2],cx);const double detC=dot3(a[0],cx);
    if(!(std::fabs(detE)>0.9)||!(std::fabs(detC)>0.9)){out.why="basis-not-rotation";return false;}
    // Mirrored cockpit matrix (reflection): flip its local x so both bases have the same handedness.
    if(detE*detC<0)for(int k=0;k<3;++k)r[0][k]=-r[0][k];
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)out.R[i*3+j]=dot3(r[i],a[j]);
    // forward = R (0,0,-1) = (-R[2], -R[5], -R[8]); yaw > 0 turns it towards -x (left), pitch > 0 up.
    out.yaw=std::atan2(out.R[2],out.R[8]);
    const double fy=clampd(-out.R[5],-1.0,1.0);out.pitch=std::asin(fy);
    // Plausibility: the game cannot look further than its saturation; anything else means a wrong reference.
    if(!std::isfinite(out.yaw)||!std::isfinite(out.pitch)||std::fabs(out.yaw)>2.7||out.pitch<-0.5||out.pitch>1.65){out.why="look-out-of-range";return false;}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)out.E[i*3+j]=rows[i][j];
    out.valid=true;out.why="measured";return true;
}
// Rotation angle (radians) between two row-major rotations: acos((trace(A^T B) - 1) / 2).
inline double rotation_angle(const double* A,const double* B) noexcept {
    double tr=0;for(int i=0;i<9;++i)tr+=A[i]*B[i];
    return std::acos(clampd((tr-1.0)*0.5,-1.0,1.0));
}
inline bool measure_look(const double* right,const double* up,const double* fwd,const double* m256,LookMeasure& out) noexcept {
    double r[3][3];
    const double rows[3][3]={{m256[0],m256[1],m256[2]},{m256[4],m256[5],m256[6]},{m256[8],m256[9],m256[10]}};
    if(!unit3(rows[0],r[0])||!unit3(rows[1],r[1])||!unit3(rows[2],r[2])){out=LookMeasure{};out.why="cockpit-matrix-degenerate";return false;}
    return measure_look_rows(r,right,up,fwd,out);
}
// Neutral-relative head yaw/pitch from a head rotation R (row-major, OpenXR: forward = R (0,0,-1)) and the seated
// recentring heading yaw0 (gpu_copy_step5.h Neutral::yaw0): fwd' = Ry(-yaw0) fwd.
inline bool head_angles(const double R[9],double yaw0,double& yaw,double& pitch) noexcept {
    const double fx=-R[2],fy=-R[5],fz=-R[8];
    const double cy=std::cos(-yaw0),sv=std::sin(-yaw0);
    const double x=cy*fx+sv*fz,z=-sv*fx+cy*fz;
    if(!std::isfinite(x)||!std::isfinite(z)||!std::isfinite(fy))return false;
    yaw=std::atan2(-x,-z);pitch=std::asin(clampd(fy,-1.0,1.0));return std::isfinite(yaw)&&std::isfinite(pitch);
}

// ------------------------------------------------------------------ F-REF: orientation clusters of the replayed members' M256
// (key xr_look_ref_cluster for the behaviour; the diagnostic line rides xr_look_drive_keys). Members may carry their own local rotation
// relative to the aircraft (test 14: recurring constant "looks" 10.1 / 19.7 / 36.1 deg), so the FIRST member is not a reliable aircraft
// reference. Each cockpit member's unit rows (reflection folded like measure_look_rows) join the cluster whose representative (the first
// member that opened it) is within kClusterRad; the majority cluster is the reference.
struct RefClusters {
    static constexpr unsigned kMax=8,kMinMajority=3;
    static constexpr double kClusterRad=0.0349066;  // 2 degrees
    struct Cluster {double E[9]{};unsigned count=0;};
    Cluster c[kMax]{};unsigned n=0,members=0,rejected=0,overflow=0;int first=-1;uint64_t key=~0ull;
    void reset(uint64_t k) noexcept {*this=RefClusters{};key=k;}
    static bool unit_rows(const double* m256,double E[9]) noexcept {
        double r[3][3];
        const double raw[3][3]={{m256[0],m256[1],m256[2]},{m256[4],m256[5],m256[6]},{m256[8],m256[9],m256[10]}};
        for(int i=0;i<3;++i)if(!unit3(raw[i],r[i]))return false;
        if(std::fabs(dot3(r[0],r[1]))>0.02||std::fabs(dot3(r[0],r[2]))>0.02||std::fabs(dot3(r[1],r[2]))>0.02)return false;
        double x[3];cross3(r[1],r[2],x);const double det=dot3(r[0],x);
        if(!(std::fabs(det)>0.9))return false;
        if(det<0)for(int k=0;k<3;++k)r[0][k]=-r[0][k];
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)E[i*3+j]=r[i][j];
        return true;
    }
    // m256: the member's 16 doubles (row-vector local->world). Returns the cluster index or -1 (rejected / overflow).
    int add(const double* m256) noexcept {
        double E[9];
        if(!unit_rows(m256,E)){++rejected;return -1;}
        ++members;
        for(unsigned i=0;i<n;++i)if(rotation_angle(c[i].E,E)<kClusterRad){++c[i].count;if(members==1)first=int(i);return int(i);}
        if(n>=kMax){++overflow;return -1;}
        std::memcpy(c[n].E,E,sizeof E);c[n].count=1;if(members==1)first=int(n);return int(n++);
    }
    // The majority cluster: the largest with at least kMinMajority members and strictly larger than the runner-up. -1 = none/ambiguous.
    int majority() const noexcept {
        int best=-1;unsigned bc=0,second=0;
        for(unsigned i=0;i<n;++i) {
            if(c[i].count>bc){second=bc;bc=c[i].count;best=int(i);}
            else if(c[i].count>second)second=c[i].count;
        }
        return (best>=0&&bc>=kMinMajority&&bc>second)?best:-1;
    }
};

// ------------------------------------------------------------------ scripted test sweep (xr_look_drive_test_sweep)
// yaw: +-60 degrees sinusoid, period 12 s, two periods (24 s); then pitch up 0..40..0 degrees over 12 s; repeat (36 s cycle).
inline void sweep_value(double t_s,double& yaw,double& pitch) noexcept {
    constexpr double deg=kPi/180.0;
    double t=t_s>0?std::fmod(t_s,36.0):0.0;
    if(t<24.0){yaw=60.0*deg*std::sin(2*kPi*t/12.0);pitch=0;}
    else {yaw=0;pitch=40.0*deg*0.5*(1.0-std::cos(2*kPi*(t-24.0)/12.0));}
}
// Rotates both eye poses about the head centre (midpoint of the eyes) by Ry(yaw) Rx(pitch) (pre-multiplied), i.e. the
// synthetic head turn of the test sweep. EyePose-like: position[3] (float), orientation[4] (x,y,z,w).
inline void quat_mul(const double a[4],const double b[4],double o[4]) noexcept {
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
template<class Pose> inline void apply_sweep_to_poses(Pose* poses,double yaw,double pitch) noexcept {
    double R[9];look_matrix(yaw,pitch,R);
    const double qy[4]={0,std::sin(yaw/2),0,std::cos(yaw/2)},qx[4]={std::sin(pitch/2),0,0,std::cos(pitch/2)};
    double qs[4];quat_mul(qy,qx,qs);
    double c[3];for(int k=0;k<3;++k)c[k]=0.5*(double(poses[0].position[k])+double(poses[1].position[k]));
    for(int e=0;e<2;++e) {
        double d[3];for(int k=0;k<3;++k)d[k]=double(poses[e].position[k])-c[k];
        for(int i=0;i<3;++i)poses[e].position[i]=float(c[i]+R[i*3]*d[0]+R[i*3+1]*d[1]+R[i*3+2]*d[2]);
        const double q[4]={poses[e].orientation[0],poses[e].orientation[1],poses[e].orientation[2],poses[e].orientation[3]};
        double o[4];quat_mul(qs,q,o);for(int k=0;k<4;++k)poses[e].orientation[k]=float(o[k]);
    }
}

// ------------------------------------------------------------------ controller
struct Params {
    double yaw_full=yaw_full_rad,pitch_full=pitch_full_rad;
    double yaw_clamp=yaw_clamp_rad,pitch_max=pitch_max_rad,pitch_min=0.0,deadzone=deadzone_rad;
    double rate=target_rate_rad_s;
    unsigned horizon=6;             // frames of the receding-horizon key search
    double press_cost=2e-4,flip_cost=2e-3; // rad^2: prefer fewer presses, avoid left<->right flips inside the horizon
    // OPEN LOOP BY DEFAULT (ki = 0, gain_mu = 0): the test-10 log showed the measured look carrying a constant 9 deg offset and jumps
    // that the integral/gain terms turned into key presses. The measurement is then used for the compensation only. closed_loop()
    // restores the former behaviour (config key xr_look_drive_closed_loop).
    double kp=0.0,ki=0.0,i_clamp=0.5,meas_lp_s=0.15;  // measured look corrects the MEAN only (no proportional term: it would add ripple)
    double gain_mu=0.0;             // per-frame step of the key-gain estimate (measured look / unit-gain model)
    double analog_deadzone=0.0;     // XInput route: fraction of full stick the game ignores (UNVERIFIED; 0 = none); output = dz + (1-dz)*|duty|
    // XInput route lead compensation: the stick is driven for target + lead_tau_s * d(target)/dt so the game's first-order look filter (tau ~0.1 s)
    // does not lag a moving target. The derivative is low-passed (lead_lp_s; 0 = raw frame difference, the former behaviour) and the lead term is
    // bounded (lead_max_rad) so a target step cannot overdrive the stick.
    double lead_tau_s=filter_tau_s,lead_lp_s=0.03,lead_max_rad=0.4;
    static Params legacy_lead() noexcept {Params p;p.lead_lp_s=0.0;p.lead_max_rad=1e9;return p;}   // the pre-test-12 analog controller (tests)
    unsigned meas_delay_frames=2;   // assumed render-to-measurement latency of the model comparison
    double max_down_ms=max_key_down_ms,max_gap_ms=max_frame_gap_ms;
    static Params closed_loop() noexcept {Params p;p.ki=0.2;p.gain_mu=0.01;return p;}
};
struct Input {
    double now_ms=0;
    bool foreground=false,kill=false;
    bool head_valid=false;double head_yaw=0,head_pitch=0;
    bool meas_yaw_valid=false,meas_pitch_valid=false;double meas_yaw=0,meas_pitch=0;
};
struct Stats {                  // since the last log line
    unsigned frames=0,left=0,right=0,up=0,background=0,nohead=0;
    double err2=0;unsigned errN=0;double errMax=0;
};
class Controller {
public:
    explicit Controller(const Params& p=Params{}) noexcept:p_(p) {}
    // One decision per game frame. Returns the desired held-key set.
    uint8_t step(const Input& in) noexcept {
        const bool first=!have_prev_;
        double dtms=first?0.0:in.now_ms-prev_ms_;
        have_prev_=true;prev_ms_=in.now_ms;
        if(in.kill&&!killed_){killed_=true;++kills_;}
        const bool gap=!first&&(dtms<=0||dtms>p_.max_gap_ms);
        const double dt=clampd(dtms/1000.0,1.0/240.0,0.1);
        const char* why="ok";
        if(killed_)why="kill-switch";
        else if(!in.foreground)why="background";
        else if(!in.head_valid)why="no-head";
        else if(first||gap)why="resync";
        why_=why;
        ++st_.frames;if(!in.foreground)++st_.background;if(!in.head_valid)++st_.nohead;
        uint8_t mask=0;
        const bool live=why[0]=='o'; // "ok"
        if(live) {
            if(analog_) { // XInput route: a continuous stick fraction instead of key pulses
                ax_[0]=axis_analog(0,dt,in.head_yaw,in.meas_yaw_valid,in.meas_yaw);
                ax_[1]=axis_analog(1,dt,in.head_pitch,in.meas_pitch_valid,in.meas_pitch);
            } else {
                mask|=axis(0,dt,in.head_yaw,in.meas_yaw_valid,in.meas_yaw);
                mask|=axis(1,dt,in.head_pitch,in.meas_pitch_valid,in.meas_pitch);
            }
        } else {
            // Not driving: keys released, accumulators and integrators reset, targets relax to zero.
            for(int i=0;i<2;++i)idle(a_[i],dt);
            a_[0].meas_init=a_[1].meas_init=false;
            ax_[0]=ax_[1]=0.0;
        }
        // Never opposing keys (cannot happen by construction; belt and braces).
        if((mask&key_left)&&(mask&key_right))mask&=uint8_t(~(key_left|key_right));
        // Hard cap of continuous key-down time (per physical key).
        const uint8_t bits[3]={key_left,key_right,key_up};
        for(int k=0;k<3;++k) {
            if(mask&bits[k]) {
                if(!(held_&bits[k]))downSince_[k]=in.now_ms;
                else if(in.now_ms-downSince_[k]>=p_.max_down_ms){mask&=uint8_t(~bits[k]);++capped_;}
            }
        }
        held_=mask;
        // The commanded estimate of the game's look: the same first-order filter driven by the keys we sent.
        const double k1=1.0-std::exp(-dt/filter_tau_s);
        if(analog_) { // the stick fraction drives the same first-order look filter
            est_[0]+=(ax_[0]*p_.yaw_full-est_[0])*k1;
            est_[1]+=(ax_[1]*p_.pitch_full-est_[1])*k1;
        } else {
            est_[0]+=((mask&key_left?p_.yaw_full:(mask&key_right?-p_.yaw_full:0.0))-est_[0])*k1;
            est_[1]+=((mask&key_up?p_.pitch_full:0.0)-est_[1])*k1;
        }
        // Gain adaptation: the measured look is the model state (delayed by the render pipeline) times an unknown key gain.
        hist_[0][hn_&15]=est_[0];hist_[1][hn_&15]=est_[1];++hn_;
        if(live&&hn_>p_.meas_delay_frames+1) {
            const bool mv[2]={in.meas_yaw_valid,in.meas_pitch_valid};const double mval[2]={in.meas_yaw,in.meas_pitch};
            for(int i=0;i<2;++i) {
                if(!mv[i])continue;
                const double e1=hist_[i][(hn_-1-p_.meas_delay_frames)&15];
                if(std::fabs(e1)<0.25)continue;                        // not enough excitation
                gain_[i]=clampd(gain_[i]+p_.gain_mu*(mval[i]-gain_[i]*e1)/e1,0.6,1.6);
            }
        }
        if(mask&key_left)++st_.left;if(mask&key_right)++st_.right;if(mask&key_up)++st_.up;
        // Residual statistics: target against the measured look (estimate when unmeasured).
        if(live) {
            const double ly=in.meas_yaw_valid?in.meas_yaw:est_yaw(),lp=in.meas_pitch_valid?in.meas_pitch:est_pitch();
            const double ey=a_[0].target-ly,ep=a_[1].target-lp,e=std::sqrt(ey*ey+ep*ep);
            st_.err2+=e*e;++st_.errN;if(e>st_.errMax)st_.errMax=e;
        }
        cur_=in;mask_=mask;sources_=(in.meas_yaw_valid?1u:0u)|(in.meas_pitch_valid?2u:0u);
        return mask;
    }
    // XInput route (set once before the first step): step() then fills analog_yaw()/analog_pitch() and returns an empty key mask.
    void set_analog(bool a) noexcept {analog_=a;}
    bool analog() const noexcept {return analog_;}
    double analog_yaw() const noexcept {return ax_[0];}      // [-1,1], + = look LEFT
    double analog_pitch() const noexcept {return ax_[1];}    // [0,1], + = look UP
    bool closed_loop() const noexcept {return p_.ki>0.0||p_.gain_mu>0.0;}
    const Params& params() const noexcept {return p_;}
    // Estimate of the game's look from the keys sent (used when the measurement is invalid).
    double est_yaw() const noexcept {return gain_[0]*est_[0];}
    double est_pitch() const noexcept {return gain_[1]*est_[1];}
    double gain_yaw() const noexcept {return gain_[0];}
    double gain_pitch() const noexcept {return gain_[1];}
    double target_yaw() const noexcept {return a_[0].target;}
    double target_pitch() const noexcept {return a_[1].target;}
    double duty_yaw() const noexcept {return a_[0].duty;}
    double duty_pitch() const noexcept {return a_[1].duty;}
    uint8_t mask() const noexcept {return mask_;}
    const char* why() const noexcept {return why_;}
    bool killed() const noexcept {return killed_;}
    unsigned capped() const noexcept {return capped_;}
    const Stats& stats() const noexcept {return st_;}
    // One bounded line per second of ticks (call every tick; false when not due or capped).
    bool take_log(double now_ms,const char* mode,char* buf,size_t n) noexcept {
        if(lines_>=log_line_cap)return false;
        if(last_log_ms_<0){last_log_ms_=now_ms;return false;}
        if(now_ms-last_log_ms_<1000.0)return false;
        last_log_ms_=now_ms;++lines_;
        constexpr double d=180.0/kPi;
        const double rms=st_.errN?std::sqrt(st_.err2/st_.errN)*d:0.0;
        const bool my=(sources_&1u)!=0,mp=(sources_&2u)!=0;
        std::snprintf(buf,n,"look-drive;n=%u;mode=%s;frames=%u;target=%.1f,%.1fdeg;look=%.1f,%.1fdeg;look-src=%s,%s;duty=%.2f,%.2f;"
            "keys-frames=L:%u,R:%u,U:%u;foreground=%s;state=%s;residual-rms=%.2fdeg;residual-max=%.2fdeg;capped=%u;killed=%u%s",
            lines_,mode,st_.frames,a_[0].target*d,a_[1].target*d,(my?cur_.meas_yaw:est_yaw())*d,(mp?cur_.meas_pitch:est_pitch())*d,
            my?"measured":"estimate",mp?"measured":"estimate",a_[0].duty,a_[1].duty,st_.left,st_.right,st_.up,
            cur_.foreground?"yes":"no",why_,rms,st_.errMax*d,capped_,killed_?1u:0u,lines_==log_line_cap?";log-cap-reached":"");
        st_=Stats{};return true;
    }
private:
    struct Axis {
        double target=0,integ=0,lp=0,duty=0,slope_lp=0;bool meas_init=false;int last=0;
    };
    void idle(Axis& a,double dt) noexcept {
        const double step=p_.rate*dt;a.target+=clampd(0.0-a.target,-step,step);
        a.integ=0;a.duty=0;a.last=0;a.slope_lp=0;
    }
    // axis 0 yaw (keys left/right), axis 1 pitch (key up only). Returns the key bit to hold this frame (0 or one bit).
    //
    // First-order sigma-delta on the FILTER STATE, as a receding-horizon search: the model of the game's look filter
    // (est_, driven only by the keys we sent) is stepped through every key sequence of `horizon` frames (3^N for yaw,
    // 2^N for pitch) and the first key of the sequence with the smallest summed squared deviation from the target
    // trajectory is pressed. Plain duty-cycle sigma-delta needs the same mean but spreads pulses less well; at 55 fps
    // each one-frame pulse moves the look by (2.48 - look) * (1 - e^(-dt/tau)) ~ 0.3 rad, so the pulse placement
    // decides the ripple (see J-look-drive-findings.md). The measured look only corrects the MEAN (integral term):
    // proportional feedback on a ripple-carrying, delayed measurement adds ripple instead of removing it.
    // Target shaping shared by both routes: clamp, deadzone, rate limit; the (optional) closed-loop correction. Returns u0 (the look to
    // command, radians) and the target slope.
    double shape(int i,double dt,double raw,bool haveMeas,double meas,double& slope) noexcept {
        Axis& a=a_[i];
        const double full=i?p_.pitch_full:p_.yaw_full;
        const double lo=i?p_.pitch_min:-p_.yaw_clamp,hi=i?p_.pitch_max:p_.yaw_clamp;
        double t=clampd(raw,lo,hi);if(std::fabs(t)<p_.deadzone)t=0;
        const double prev=a.target;
        a.target+=clampd(t-a.target,-p_.rate*dt,p_.rate*dt);
        slope=clampd((a.target-prev)/dt,-p_.rate,p_.rate);
        double bias=0;
        if(haveMeas) {
            if(!a.meas_init){a.lp=meas;a.meas_init=true;}
            else a.lp+=(meas-a.lp)*(1.0-std::exp(-dt/p_.meas_lp_s));
            const double e=a.target-a.lp;
            if(p_.ki>0.0&&std::fabs(a.target+a.integ)<full*0.98)a.integ=clampd(a.integ+p_.ki*e*dt,-p_.i_clamp,p_.i_clamp); // anti-windup
            bias=p_.kp*e+a.integ;
        } else {a.integ=0;a.meas_init=false;}  // open loop: the model alone
        return a.target+bias;
    }
    // XInput route: the stick fraction. The game's look follows duty*full through the first-order filter (tau), so adding tau * slope to
    // the commanded look cancels the lag for a moving target. The result is a continuous fraction, no pulse ripple.
    double axis_analog(int i,double dt,double raw,bool haveMeas,double meas) noexcept {
        Axis& a=a_[i];
        const double full=i?p_.pitch_full:p_.yaw_full;
        double slope=0;
        const double u0=shape(i,dt,raw,haveMeas,meas,slope);
        if(p_.lead_lp_s>0.0)a.slope_lp+=(slope-a.slope_lp)*(1.0-std::exp(-dt/p_.lead_lp_s));else a.slope_lp=slope;
        const double lead=clampd(p_.lead_tau_s*a.slope_lp,-p_.lead_max_rad,p_.lead_max_rad);
        double duty=clampd((u0+lead)/(full*gain_[i]),i?0.0:-1.0,1.0);
        const double dz=clampd(p_.analog_deadzone,0.0,0.9);
        if(dz>0.0&&std::fabs(duty)>1e-4)duty=(duty<0?-1.0:1.0)*(dz+(1.0-dz)*std::fabs(duty)); // rescale past the game's stick deadzone
        a.duty=duty;a.last=0;
        return duty;
    }
    uint8_t axis(int i,double dt,double raw,bool haveMeas,double meas) noexcept {
        Axis& a=a_[i];
        const double full=i?p_.pitch_full:p_.yaw_full;
        const double lo=i?p_.pitch_min:-p_.yaw_clamp,hi=i?p_.pitch_max:p_.yaw_clamp;
        double slope=0;
        const double u0=shape(i,dt,raw,haveMeas,meas,slope);
        a.duty=clampd(u0/full,i?0.0:-1.0,1.0);
        const double k1=1.0-std::exp(-dt/filter_tau_s);
        const unsigned nact=i?2u:3u;           // 0 none, 1 left|up, 2 right
        const unsigned horizon=p_.horizon<1?1u:(p_.horizon>8?8u:p_.horizon);
        unsigned total=1;for(unsigned k=0;k<horizon;++k)total*=nact;
        double bestCost=1e300;unsigned best=0;
        for(unsigned code=0;code<total;++code) {
            double e=est_[i],cost=0;unsigned c=code,prevAct=unsigned(a.last);
            for(unsigned k=0;k<horizon;++k) {
                const unsigned act=c%nact;c/=nact;
                const double T=act==1?full:(act==2?-full:0.0);
                e+=(T-e)*k1;
                const double ut=clampd(u0+slope*dt*double(k+1),lo,hi)/gain_[i];   // the model state is unit-gain: look = gain * state
                cost+=(e-ut)*(e-ut);
                if(act)cost+=p_.press_cost;
                if(act&&prevAct&&act!=prevAct)cost+=p_.flip_cost;
                prevAct=act;
            }
            if(cost<bestCost-1e-15){bestCost=cost;best=code%nact;}
        }
        a.last=int(best);
        if(!best)return 0;
        return i?uint8_t(key_up):uint8_t(best==1?key_left:key_right);
    }
    Params p_;Axis a_[2]{};bool analog_=false;double ax_[2]{};
    bool have_prev_=false,killed_=false;double prev_ms_=0;
    uint8_t held_=0,mask_=0;double downSince_[3]{};unsigned capped_=0,kills_=0;
    double est_[2]{};                 // unit-gain model of the look filter driven by the keys we sent (the real look = gain_ * est_)
    double gain_[2]{1.0,1.0};double hist_[2][16]{};unsigned hn_=0;
    const char* why_="not-started";Input cur_{};unsigned sources_=0;
    Stats st_{};double last_log_ms_=-1.0;unsigned lines_=0;
};

// ------------------------------------------------------------------ producer-side driver
// Everything the step-5 producer needs, in one object (owned by Step5State, used under the step-1 mutex).
class Driver {
public:
    // no_xr (only meaningful with the sweep): the test sweep runs WITHOUT xr_stereo_output / an XR runtime. The head is the scripted sweep, the
    // producer's per-frame camera measurement and the controller run from the cockpit draws alone; no pose is read, nothing is compensated
    // and nothing is published to a consumer (J-look-drive-findings.md, "No-XR test sweep").
    // xinput: send the look as a right-stick offset through the XInput proxy (xr_look_drive_xinput) instead of NumPad keys; the window focus is
    // then irrelevant. closed_loop: opt in to the integral/gain correction (xr_look_drive_closed_loop); the default is open loop.
    void configure(bool sweep,bool no_xr=false,bool xinput=false,bool closed_loop=false,bool ref_cluster=false) noexcept {
        active_=true;ref_cluster_=ref_cluster;sweep_=sweep;no_xr_=sweep&&no_xr;xinput_=xinput;
        ctl_=Controller(closed_loop?Params::closed_loop():Params{});
        ctl_.set_analog(xinput);
    }
    bool xinput() const noexcept {return xinput_;}
    bool active() const noexcept {return active_;}
    bool sweep() const noexcept {return sweep_;}
    bool no_xr() const noexcept {return active_&&sweep_&&no_xr_;}
    // The sweep head of this frame without any pose (no-XR mode; the XR mode applies it to the poses with sweep_poses).
    void advance_sweep(double now_ms) noexcept {
        if(!active_||!sweep_)return;
        if(sweep_t0_<0)sweep_t0_=now_ms;
        sweep_value((now_ms-sweep_t0_)/1000.0,sw_yaw_,sw_pitch_);
    }
    double sweep_yaw() const noexcept {return sw_yaw_;}
    double sweep_pitch() const noexcept {return sw_pitch_;}
    // Tests install a fake; the DLL gets the real injector from the factory the first time it is needed.
    void set_injector(KeyInjector* inj) noexcept {inj_=inj;owned_by_test_=true;}
    KeyInjector* injector() noexcept {
        if(!inj_&&!owned_by_test_) {
            if(auto f=(xinput_?g_analog_injector_factory:g_injector_factory).load())inj_=f();
        }
        return inj_;
    }
    Controller& controller() noexcept {return ctl_;}
    // Synthetic head turn of the test sweep, applied to the poses right after they are read from the provider.
    template<class Pose> void sweep_poses(Pose* poses,double now_ms) noexcept {
        if(!active_||!sweep_||!poses)return;
        if(sweep_t0_<0)sweep_t0_=now_ms;
        sweep_value((now_ms-sweep_t0_)/1000.0,sw_yaw_,sw_pitch_);
        apply_sweep_to_poses(poses,sw_yaw_,sw_pitch_);
    }
    // The frame's camera (after camera_frame succeeded): the look the game actually rendered.
    // now_ms (>= 0): the frame clock; enables the plausibility gate (a look that moved faster than the game's first-order filter can slew
    // is a measurement glitch: the previous good measurement is held instead) and the idle-reference calibration.
    void note_camera(const double* right,const double* up,const double* fwd,const double* m256,double now_ms=-1.0) noexcept {
        for(int k=0;k<3;++k){cam_[k]=right[k];cam_[3+k]=up[k];cam_[6+k]=fwd[k];}cam_valid_=true;
        used_ref_=0;
        // F-REF: the aircraft reference is the previous frame's majority orientation cluster (the clusters of THIS frame do not exist yet:
        // the cockpit members follow the first one). It lags the aircraft by at most one frame of its own rotation. No majority yet or a
        // stale one (> 250 ms): the first member, exactly as before (counted).
        if(ref_cluster_) {
            if(maj_valid_&&(now_ms<0||(now_ms>=maj_ms_&&now_ms-maj_ms_<=250.0))) {
                double rows[3][3];for(int i=0;i<3;++i)for(int j=0;j<3;++j)rows[i][j]=maj_E_[i*3+j];
                measure_look_rows(rows,right,up,fwd,raw_);used_ref_=1;++ref_used_;
            } else {++ref_fallbacks_;measure_look(right,up,fwd,m256,raw_);}
        } else measure_look(right,up,fwd,m256,raw_);
        if(!raw_.valid){meas_=raw_;++meas_invalid_;ref_valid_=false;return;}
        if(now_ms>=0&&have_good_&&now_ms>good_ms_&&now_ms-good_ms_<250.0) {
            const double dt=(now_ms-good_ms_)/1000.0;
            const double allowed=yaw_full_rad/filter_tau_s*dt+jump_slack_rad;
            if(rotation_angle(good_raw_R_,raw_.R)>allowed) {
                if(++jump_streak_<=jump_resync_frames) { // hold the previous good measurement (not the glitch)
                    ++jump_rejects_;meas_=good_meas_;ref_valid_=true;return;
                }
            }
        }
        jump_streak_=0;
        have_good_=true;good_ms_=now_ms>=0?now_ms:good_ms_;std::memcpy(good_raw_R_,raw_.R,sizeof good_raw_R_);
        ++meas_valid_;ref_valid_=true;std::memcpy(ref_E_,raw_.E,sizeof ref_E_);
        meas_=calibrated(raw_);good_meas_=meas_;
    }
    void note_no_camera() noexcept {meas_=LookMeasure{};meas_.why="no-camera";raw_=meas_;ref_valid_=false;}
    // Idle-reference calibration (test 10: the measured idle look was a constant 9.2 deg yaw, i.e. the cockpit reference rows are not aligned
    // with the camera at look 0 in this scene). Once nothing is being commanded (the head need not be forward or still: test 12) and the measured look has been
    // still for 1 s, that measurement IS the zero of the look: R0 is stored and every later measurement is R0^T R.
    static constexpr double cal_hold_ms=1000.0,cal_still_rad=0.0087,cal_est_rad=0.01,cal_stick_eps=0.003;
    bool calibrated_ref() const noexcept {return cal_valid_;}
    double cal_yaw() const noexcept {return cal_yaw_;}
    double cal_pitch() const noexcept {return cal_pitch_;}
    unsigned jump_rejects() const noexcept {return jump_rejects_;}
    const LookMeasure& raw_measured() const noexcept {return raw_;}
    // World-first frames (a world draw opens the frame before its cockpit member exists): the look of THIS frame, derived from the world
    // camera (its axes equal the cockpit camera's, verified at adoption) against the aircraft reference of the previous VALID cockpit
    // measurement (the cockpit model is fixed to the aircraft, so only the aircraft's own frame-to-frame rotation remains as error; the
    // adoption compares this result with the cockpit's own measurement). nullptr (refuse the early open) when there is no reference:
    // none yet, the previous frame had no valid measurement, or the last decision tick is older than max_frame_gap_ms.
    const double* estimate_current(const double* right,const double* up,const double* fwd,double now_ms) noexcept {
        if(!ref_valid_||last_frame_ms_<0||now_ms<last_frame_ms_||now_ms-last_frame_ms_>max_frame_gap_ms)return nullptr;
        double rows[3][3];for(int i=0;i<3;++i)for(int j=0;j<3;++j)rows[i][j]=ref_E_[i*3+j];
        LookMeasure m;if(!measure_look_rows(rows,right,up,fwd,m))return nullptr;
        if(cal_valid_)mul_t(R0_,m.R,early_R_);else std::memcpy(early_R_,m.R,sizeof early_R_);
        return early_R_;
    }
    // Adoption check: the compensation a world-first frame used vs the one the cockpit member measured (radians of rotation).
    static constexpr double early_look_tolerance_rad=0.05; // ~2.9 degrees: more than an aircraft turns in one frame, far below a look step
    const LookMeasure& measured() const noexcept {return meas_;}
    // F-REF bookkeeping (the cockpit members of a frame call ref_member(); the frame's close calls ref_close()). The diagnostic line is produced
    // whenever the driver is active; the behaviour change needs ref_cluster. `key` identifies the frame (a new key drops an unclosed one).
    bool ref_cluster() const noexcept {return ref_cluster_;}
    void ref_member(uint64_t key,const double* m256) noexcept {if(!active_)return;if(key!=cl_.key)cl_.reset(key);cl_.add(m256);}
    void ref_close(uint64_t key,double now_ms) noexcept {
        if(!active_||key!=cl_.key||!cl_.members)return;
        const int maj=cl_.majority();
        if(maj>=0){std::memcpy(maj_E_,cl_.c[maj].E,sizeof maj_E_);maj_valid_=true;maj_ms_=now_ms;}
        ++ref_frames_;
        if(maj<0)++ref_no_majority_;
        if(diag_lines_<log_line_cap&&(diag_lines_<8||diag_last_ms_<0||now_ms-diag_last_ms_>=1000.0)) {
            diag_last_ms_=now_ms;++diag_lines_;format_diag(maj);diag_pending_=true;
        }
        cl_.reset(~0ull);
    }
    bool take_ref_diag(char* buf,size_t n) noexcept {
        if(!diag_pending_||!buf||!n)return false;
        diag_pending_=false;std::snprintf(buf,n,"%s",diag_);return true;
    }
    unsigned ref_fallbacks() const noexcept {return ref_fallbacks_;}
    unsigned ref_used_frames() const noexcept {return ref_used_;}
    unsigned ref_frames() const noexcept {return ref_frames_;}
    bool ref_majority_valid() const noexcept {return maj_valid_;}
    // Camera-local -> cockpit-local rotation to cancel in the per-eye head rotation: the measured look of this frame,
    // else the commanded estimate. Never null while the driver is active.
    const double* compensation() noexcept {
        if(meas_.valid){comp_yaw_=meas_.yaw;comp_pitch_=meas_.pitch;comp_measured_=true;return meas_.R;}
        comp_yaw_=ctl_.est_yaw();comp_pitch_=ctl_.est_pitch();comp_measured_=false;
        look_matrix(comp_yaw_,comp_pitch_,est_R_);return est_R_;
    }
    // The yaw/pitch of the rotation compensation() last returned (published as StereoFrame::look_*).
    double comp_yaw() const noexcept {return comp_yaw_;}
    double comp_pitch() const noexcept {return comp_pitch_;}
    bool comp_measured() const noexcept {return comp_measured_;}
    // Timing-anchor diagnostics: the commanded (key-model) estimate, whether or not a measurement exists (the measurement is measured()).
    double est_yaw_now() const noexcept {return ctl_.est_yaw();}
    double est_pitch_now() const noexcept {return ctl_.est_pitch();}
    // Once per frame after the pose setup. R0: eye-0 head rotation (row-major), yaw0: the neutral heading.
    void on_frame(double now_ms,bool pose_valid,const double* R0,double yaw0) noexcept {
        Input in;in.now_ms=now_ms;last_frame_ms_=now_ms;
        KeyInjector* inj=injector();
        const bool analog=inj&&inj->analog();
        if(!inj){no_injector_=true;}
        else {in.foreground=analog?true:inj->foreground();in.kill=inj->kill_switch();} // the XInput route does not depend on window focus
        double hy=0,hp=0;bool ok=false;
        if(pose_valid&&(R0||sweep_)){   // the sweep head needs no pose rotation (no-XR mode passes none)
            if(sweep_)ok=true,hy=sw_yaw_,hp=sw_pitch_;
            else ok=head_angles(R0,yaw0,hy,hp);
        }
        in.head_valid=ok;in.head_yaw=hy;in.head_pitch=hp;
        in.meas_yaw_valid=meas_.valid;in.meas_pitch_valid=meas_.valid;in.meas_yaw=meas_.yaw;in.meas_pitch=meas_.pitch;
        const uint8_t mask=ctl_.step(in);
        update_calibration(now_ms,in,mask);
        if(inj) {
            // EVERY decision is published, a zero included: it overwrites a request the consumer has not acted on yet (down() alone
            // cannot tell, an undelivered press is not a held key). An injector in fault gets nothing but zeros.
            if(analog)inj->apply_axes(inj->fault()?0.0:ctl_.analog_yaw(),inj->fault()?0.0:ctl_.analog_pitch(),now_ms);
            else inj->apply(inj->fault()?uint8_t(0):mask,now_ms);
            ++applied_;
        }
    }
    // Frames without a pose/camera must still release: publish the empty request unconditionally (it cancels a pending press).
    void release(double now_ms) noexcept {if(auto* inj=inj_)inj->apply(0,now_ms);}
    // true = every key confirmed up (or no injector); false = an unconfirmed release remains (reported by take_log).
    bool shutdown() noexcept {if(!inj_)return true;const bool ok=inj_->release_all();shutdown_ok_=ok;shutdown_done_=true;if(!ok||fault_lines_)fault_logged_=~0u;return ok;}  // a clean shutdown with no fault history stays silent
    bool take_log(double now_ms,char* buf,size_t n) noexcept {
        if(!active_)return false;
        if(no_injector_&&!no_injector_logged_){no_injector_logged_=true;std::snprintf(buf,n,"look-drive;state=no-injector;keys=none");return true;}
        if(inj_) { // injection faults and the shutdown outcome are reported once per change, ahead of the periodic line
            const unsigned fl=inj_->fault();
            if(fl!=fault_logged_&&(fl||fault_logged_!=0u)&&fault_lines_<8) {
                fault_logged_=fl;++fault_lines_;
                std::snprintf(buf,n,"look-drive;state=injector-fault;fault=%u(thread-start:%u,release-pending:%u,shutdown-unconfirmed:%u);keys-down=%u;shutdown=%s",
                    fl,fl&fault_thread_start?1u:0u,fl&fault_release_pending?1u:0u,fl&fault_shutdown_unconfirmed?1u:0u,unsigned(inj_->down()),
                    shutdown_done_?(shutdown_ok_?"released":"UNCONFIRMED"):"not-run");
                return true;
            }
        }
        if(!ctl_.take_log(now_ms,sweep_?(no_xr_?"test-sweep-no-xr":"test-sweep"):"head",buf,n))return false;
        // Per-second drive status (test 10 needed it: which window held the focus, did the sends arrive, was the reference calibrated). A
        // separate line (the feature log truncates lines at 480 bytes), fetched with take_status() right after this one.
        char ex[256];ex[0]=0;if(inj_)inj_->status_text(ex,sizeof ex);
        std::snprintf(status_,sizeof status_,"look-drive-status;route=%s;loop=%s;ref-cal=%s,%.1f,%.1fdeg;jump-rejects=%u%s%s",xinput_?"xinput":"keys",
            ctl_.closed_loop()?"closed":"open",cal_valid_?"yes":"no",cal_yaw_*180.0/kPi,cal_pitch_*180.0/kPi,jump_rejects_,ex[0]?";":"",ex);
        status_pending_=true;
        return true;
    }
    // The companion line of the last take_log() (once per periodic line).
    bool take_status(char* buf,size_t n) noexcept {
        if(!status_pending_||!buf||!n)return false;
        status_pending_=false;std::snprintf(buf,n,"%s",status_);return true;
    }
    unsigned measured_valid() const noexcept {return meas_valid_;}
    unsigned measured_invalid() const noexcept {return meas_invalid_;}
private:
    void format_diag(int maj) noexcept {
        constexpr double d=180.0/kPi;
        int used=-1;double usedYaw=0,usedPitch=0;
        if(raw_.valid) {
            double best=1e9;for(unsigned i=0;i<cl_.n;++i){const double a=rotation_angle(cl_.c[i].E,raw_.E);if(a<best&&a<RefClusters::kClusterRad*2.0){best=a;used=int(i);}}
            usedYaw=raw_.yaw*d;usedPitch=raw_.pitch*d;
        }
        int len=std::snprintf(diag_,sizeof diag_,"look-ref-diag;n=%u;members=%u;clusters=%u;maj=%d/%u;first=%d;ref=%s;used-cluster=%d;rej=%u;ovf=%u;raw-look=%.1f,%.1fdeg;c=",
            diag_lines_,cl_.members,cl_.n,maj,maj>=0?cl_.c[maj].count:0u,cl_.first,used_ref_?"majority":"first",used,cl_.rejected,cl_.overflow,usedYaw,usedPitch);
        for(unsigned i=0;i<cl_.n&&len>0&&size_t(len)<sizeof diag_-48;++i) {
            double rows[3][3];for(int a=0;a<3;++a)for(int b=0;b<3;++b)rows[a][b]=cl_.c[i].E[a*3+b];
            LookMeasure m;char ang[40];
            if(cam_valid_&&measure_look_rows(rows,cam_,cam_+3,cam_+6,m))std::snprintf(ang,sizeof ang,"%.1f/%.1f",m.yaw*d,m.pitch*d);else std::snprintf(ang,sizeof ang,"x/x");
            const double rel=(cl_.first>=0)?rotation_angle(cl_.c[cl_.first].E,cl_.c[i].E)*d:0.0;
            len+=std::snprintf(diag_+len,sizeof diag_-size_t(len),"%s%u@%s/%.1f",i?",":"",cl_.c[i].count,ang,rel);
        }
    }
    static void mul_t(const double* A,const double* B,double* o) noexcept { // o = A^T B (row-major 3x3)
        for(int i=0;i<3;++i)for(int j=0;j<3;++j){double v=0;for(int k=0;k<3;++k)v+=A[k*3+i]*B[k*3+j];o[i*3+j]=v;}
    }
    LookMeasure calibrated(const LookMeasure& m) const noexcept {
        if(!cal_valid_||!m.valid)return m;
        LookMeasure o=m;mul_t(R0_,m.R,o.R);
        o.yaw=std::atan2(o.R[2],o.R[8]);o.pitch=std::asin(clampd(-o.R[5],-1.0,1.0));
        return o;
    }
    void update_calibration(double now_ms,const Input& in,uint8_t mask) noexcept {
        if(cal_valid_)return;
        // Nothing commanded (the published stick is zero, as it is whenever there is no head pose yet, and the model estimate agrees) and the raw
        // measured look still: the head does NOT need to be still or forward (test 12: the first seconds before the head pose arrives are enough).
        const bool quiet=raw_.valid&&!ctl_.killed()&&mask==0&&std::fabs(ctl_.analog_yaw())<cal_stick_eps&&std::fabs(ctl_.analog_pitch())<cal_stick_eps&&
            std::fabs(ctl_.est_yaw())<cal_est_rad&&std::fabs(ctl_.est_pitch())<cal_est_rad;
        if(!quiet||cal_last_ms_<0||now_ms-cal_last_ms_>max_frame_gap_ms||rotation_angle(cal_R_,raw_.R)>cal_still_rad) {
            cal_last_ms_=now_ms;
            if(quiet){std::memcpy(cal_R_,raw_.R,sizeof cal_R_);cal_t0_=now_ms;}else cal_t0_=-1.0;
            return;
        }
        cal_last_ms_=now_ms;
        if(cal_t0_>=0&&now_ms-cal_t0_>=cal_hold_ms) {
            std::memcpy(R0_,raw_.R,sizeof R0_);cal_valid_=true;cal_yaw_=raw_.yaw;cal_pitch_=raw_.pitch;
        }
    }
    bool active_=false,sweep_=false,no_xr_=false,owned_by_test_=false,no_injector_=false,no_injector_logged_=false,xinput_=false;
    char status_[420]{};bool status_pending_=false;
    // measurement gate and calibration state
    static constexpr double jump_slack_rad=0.03;static constexpr unsigned jump_resync_frames=8;
    LookMeasure raw_{},good_meas_{};double good_raw_R_[9]{1,0,0,0,1,0,0,0,1};bool have_good_=false;double good_ms_=-1.0;unsigned jump_streak_=0,jump_rejects_=0;
    bool cal_valid_=false;double R0_[9]{1,0,0,0,1,0,0,0,1},cal_R_[9]{1,0,0,0,1,0,0,0,1};double cal_t0_=-1.0,cal_last_ms_=-1.0,cal_yaw_=0,cal_pitch_=0;
    KeyInjector* inj_=nullptr;Controller ctl_;LookMeasure meas_{};double est_R_[9]{};
    double comp_yaw_=0,comp_pitch_=0;bool comp_measured_=false;
    double ref_E_[9]{1,0,0,0,1,0,0,0,1},early_R_[9]{};bool ref_valid_=false;double last_frame_ms_=-1.0;
    unsigned fault_logged_=0,fault_lines_=0;bool shutdown_done_=false,shutdown_ok_=true;
    bool ref_cluster_=false,cam_valid_=false,maj_valid_=false,diag_pending_=false;int used_ref_=0;
    double cam_[9]{},maj_E_[9]{1,0,0,0,1,0,0,0,1},maj_ms_=-1.0,diag_last_ms_=-1.0;
    RefClusters cl_{};unsigned ref_fallbacks_=0,ref_used_=0,ref_frames_=0,ref_no_majority_=0,diag_lines_=0;char diag_[460]{};
    double sweep_t0_=-1.0,sw_yaw_=0,sw_pitch_=0;unsigned meas_valid_=0,meas_invalid_=0;uint64_t applied_=0;
};

} // namespace acz::look_drive
