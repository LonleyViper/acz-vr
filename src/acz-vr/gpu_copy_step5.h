#pragma once
// J-3b8: GPU-copy "step 5", the continuous per-frame producer of stereo cockpit
// frames (D-230 FrameSource). SDK-free pose maths shared by the hook adapter
// (gpu_copy_step5_hooks.inl) and the offline fixtures.
//
// Pose-driven eyes (pose-contract.md, cockpit-openxr-layer.md section 1):
//  * The game camera is recovered ONCE per frame from the first replayed member's
//    live SrtData: VP = inverse(M256) * M64 (affine M256), decomposed into the
//    camera eye, right/up/forward axes (up uses the game's negative clip-y scale,
//    sy = -1, as measured in E4/E9), the w scale n3, the symmetric lens
//    tan(half fov) = n3/n0, n3/n1 and the depth map z_c = A*w_c + C. The per-draw
//    clip->PS2-screen conversion S = inverse(M64) * M0 is taken from that member
//    too (E9: identical for all draws of a frame).
//  * Seated recentering: the first valid pose fixes the neutral head centre p0
//    (midpoint of the two eye positions) and heading yaw0 (left eye forward,
//    yaw only). Every later eye pose is expressed relative to it:
//    d' = Ry(-yaw0) (p - p0), R' = Ry(-yaw0) R.
//  * Neutral seat: the recentred head frame coincides with the game's camera
//    (the game's own look direction stays; head motion is ADDED to it). OpenXR
//    +X -> camera right, +Y -> camera up, -Z -> camera forward; metres -> DrawCPT
//    units x 39.6 (E9 scale ASSUMPTION, unmeasured).
//  * Eye VP_e = view(eye pose in game world) x the GAME'S OWN symmetric lens
//    (J-3b9, D-236: the pose changes rotation/translation only; the lens the
//    cockpit is rendered with is published as StereoFrame::lens_tan and the XR
//    layer uses fov = atan(lens_tan)), with the game's w scale and depth map.
//  * DELTA application (exact control): with VP_ref built by the SAME function
//    from the neutral pose and the game's own symmetric lens, D_e = VP_e - VP_ref
//    is precomputed per frame; per member M64'_e = M64 + M256 * D_e and
//    M0'_e = M0 + (M256 * D_e) * S. A zero position/rotation offset gives D_e == 0
//    exactly (same lens, same eye, identical axes), so the substitution keeps the
//    raw bits (a zero delta keeps them, also -0.0). Otherwise rows 0..3 all change
//    (rotation affects the whole matrix). Two 4x4 multiplies per eye per member.
#include "gpu_copy_step3.h"
#include "stereo_frame_source.h"
#include "pov_fixes.h"
#include "effects_game_clouds.h"
#include "hud_late.h"        // HUD-LATE (hudlate:1): post-tone-map HUD text into the HUD image
#include "world_scale.h"   // WORLD-SCALE: the world-object eye scale published by the XR proxy
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::gpu_copy::step5 {
using acz::stereo::EyePose;
constexpr unsigned slots=acz::stereo::kSlots;
constexpr unsigned fail_cap=3;               // consecutive ambiguous frames disable the producer
constexpr unsigned status_every_ticks=50;    // summary line every 5 s (100 ms ticks)
constexpr unsigned reselect_after=120;       // consecutive first-member shape mismatches -> re-select
constexpr unsigned reselect_cap=5;           // re-selections per session, then disabled
// J-compat-race: `producer-dead` evidence line (binding observer permanently disabled ->
// every live SRT unreadable -> every frame skipped). First sighting at once, then at most
// one line per minute, at most 16 lines per process.
constexpr unsigned dead_repeat_ticks=600;    // 100 ms ticks
constexpr unsigned dead_line_cap=16;
constexpr uint64_t slot_upload_bytes=step3::set_upload_bytes; // 64 members x 2 eyes x 512 B
constexpr double sy=-1.0;                    // game clip-y sign (E4/E9: negative y scale)
enum SlotState : int {slot_free=0,slot_producing=1,slot_published=2,slot_held=3,slot_quarantined=4};
inline const char* slot_state_name(int s) noexcept {
    static constexpr const char* n[]={"free","producing","published","held","quarantined"};return s>=0&&s<5?n[s]:"?";
}

struct Camera {double right[3]{},up[3]{},fwd[3]{},eye[3]{};double n0=0,n1=0,n3=0,A=0,C=0,tanX=0,tanY=0;};
struct Neutral {uint32_t generation=0;bool set=false;double p0[3]{};double yaw0=0;};
// Heap-owned scratch so the Draw-path frames stay small.
struct Scratch {double m0[16]{},m64[16]{},m256[16]{},inv[16]{},a[16]{},d64[16]{};double R[2][9]{},ax[3][3]{};};
// J-cockpit-fov (gpu_copy_step5_hmd_fov): one eye's FOV as the eye matrix is built with it.
// t: signed tangents left, right, up, down (left/down negative, XrFovf order); angle: the
// radians published in StereoFrame::cockpit_fov (the XR angle itself unless clamped/snapped).
struct EyeFov {double t[4]{};float angle[4]{};};
struct PoseFrame {
    bool valid=false;const char* why="not-evaluated";
    double D[2][16]{};double S[16]{};double vp[16]{},vpRef[16]{},vpEye[2][16]{};
    Camera cam{};double eyeWorld[2][3]{};
    // J-cockpit-fov: fovHmd = the eye matrices use fov[] (only with valid=true); otherwise the
    // game lens. fovWhy: not-requested, hmd, hmd-clamped or the fallback reason.
    bool fovHmd=false,fovClamped=false;const char* fovWhy="not-requested";EyeFov fov[2]{};
};

inline void load16(const uint8_t* p,double* out) noexcept {float f[16];std::memcpy(f,p,64);for(int i=0;i<16;++i)out[i]=double(f[i]);}
inline void mul44(const double* a,const double* b,double* out) noexcept { // row-vector: out = a * b
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){double s=0;for(int k=0;k<4;++k)s+=a[r*4+k]*b[k*4+c];out[r*4+c]=s;}
}
// General 4x4 inverse in place (Gauss-Jordan, partial pivoting). `a` is destroyed.
inline bool inv44(double* a,double* inv) noexcept {
    for(int i=0;i<16;++i)inv[i]=(i%5==0)?1.0:0.0;
    double scale=0;for(int i=0;i<16;++i)scale=std::fabs(a[i])>scale?std::fabs(a[i]):scale;
    if(!(scale>0)||!std::isfinite(scale))return false;
    for(int c=0;c<4;++c) {
        int piv=c;for(int r=c+1;r<4;++r)if(std::fabs(a[r*4+c])>std::fabs(a[piv*4+c]))piv=r;
        if(std::fabs(a[piv*4+c])<=scale*1e-14)return false;
        if(piv!=c)for(int k=0;k<4;++k){std::swap(a[c*4+k],a[piv*4+k]);std::swap(inv[c*4+k],inv[piv*4+k]);}
        const double d=a[c*4+c];for(int k=0;k<4;++k){a[c*4+k]/=d;inv[c*4+k]/=d;}
        for(int r=0;r<4;++r){if(r==c)continue;const double f=a[r*4+c];if(f==0)continue;
            for(int k=0;k<4;++k){a[r*4+k]-=f*a[c*4+k];inv[r*4+k]-=f*inv[c*4+k];}}
    }
    for(int i=0;i<16;++i)if(!std::isfinite(inv[i]))return false;
    return true;
}
inline double dot3(const double* a,const double* b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
// Column-vector rotation matrix of a unit quaternion (x,y,z,w): v' = R v.
inline bool quat_rot(const float* q,double R[9]) noexcept {
    double x=q[0],y=q[1],z=q[2],w=q[3];const double n=std::sqrt(x*x+y*y+z*z+w*w);
    if(!(n>0.5)||!(n<1.5)||!std::isfinite(n))return false;x/=n;y/=n;z/=n;w/=n;
    R[0]=1-2*(y*y+z*z);R[1]=2*(x*y-z*w);R[2]=2*(x*z+y*w);
    R[3]=2*(x*y+z*w);R[4]=1-2*(x*x+z*z);R[5]=2*(y*z-x*w);
    R[6]=2*(x*z-y*w);R[7]=2*(y*z+x*w);R[8]=1-2*(x*x+y*y);
    return true;
}
// Position and orientation must be sound. The eye's FOV is checked separately (hmd_tangents)
// and only when gpu_copy_step5_hmd_fov asks for it; otherwise the game's lens is kept.
inline bool pose_ok(const EyePose& p) noexcept {
    for(float v:p.position)if(!std::isfinite(v)||std::fabs(v)>1000.0f)return false;
    double R[9];return quat_rot(p.orientation,R);
}
// Row-vector world->clip matrix of an eye at `eye` with axes r/u/fw and the
// asymmetric tangents tl<tr, td<tu, using the game's w scale and depth map.
inline void build_vp(const double* eye,const double* r,const double* u,const double* fw,double tl,double tr,double tu,double td,
                     const Camera& c,double* out) noexcept {
    double X[3],Y[3],W[3];
    for(int k=0;k<3;++k) {
        X[k]=c.n3*(2.0/(tr-tl))*r[k]-c.n3*((tr+tl)/(tr-tl))*fw[k];
        Y[k]=sy*(c.n3*(2.0/(tu-td))*u[k]-c.n3*((tu+td)/(tu-td))*fw[k]);
        W[k]=c.n3*fw[k];
    }
    const double tx=-dot3(X,eye),ty=-dot3(Y,eye),tw=-dot3(W,eye);
    for(int k=0;k<3;++k){out[k*4+0]=X[k];out[k*4+1]=Y[k];out[k*4+2]=c.A*W[k];out[k*4+3]=W[k];}
    out[12]=tx;out[13]=ty;out[14]=c.A*tw+c.C;out[15]=tw;
}
// Camera of a row-vector VP (symmetric lens assumed, as recovered in E3/E9).
inline bool decompose(const double* vp,Camera& c) noexcept {
    double c0[3]={vp[0],vp[4],vp[8]},c1[3]={vp[1],vp[5],vp[9]},c2[3]={vp[2],vp[6],vp[10]},c3[3]={vp[3],vp[7],vp[11]};
    c.n0=std::sqrt(dot3(c0,c0));c.n1=std::sqrt(dot3(c1,c1));c.n3=std::sqrt(dot3(c3,c3));
    if(!(c.n0>0)||!(c.n1>0)||!(c.n3>0))return false;
    for(int k=0;k<3;++k){c.right[k]=c0[k]/c.n0;c.up[k]=sy*c1[k]/c.n1;c.fwd[k]=c3[k]/c.n3;}
    // eye: c0.e + t0 = 0, c1.e + t1 = 0, c3.e + t3 = 0 (Cramer)
    const double M[3][3]={{c0[0],c0[1],c0[2]},{c1[0],c1[1],c1[2]},{c3[0],c3[1],c3[2]}};
    const double b[3]={-vp[12],-vp[13],-vp[15]};
    const double det=M[0][0]*(M[1][1]*M[2][2]-M[1][2]*M[2][1])-M[0][1]*(M[1][0]*M[2][2]-M[1][2]*M[2][0])+M[0][2]*(M[1][0]*M[2][1]-M[1][1]*M[2][0]);
    if(!(std::fabs(det)>0))return false;
    for(int j=0;j<3;++j) {
        double T[3][3];for(int r=0;r<3;++r)for(int k=0;k<3;++k)T[r][k]=k==j?b[r]:M[r][k];
        c.eye[j]=(T[0][0]*(T[1][1]*T[2][2]-T[1][2]*T[2][1])-T[0][1]*(T[1][0]*T[2][2]-T[1][2]*T[2][0])+T[0][2]*(T[1][0]*T[2][1]-T[1][1]*T[2][0]))/det;
    }
    c.A=dot3(c2,c3)/(c.n3*c.n3);c.C=vp[14]-c.A*vp[15];
    c.tanX=c.n3/c.n0;c.tanY=c.n3/c.n1;
    for(double v:{c.A,c.C,c.tanX,c.tanY,c.eye[0],c.eye[1],c.eye[2]})if(!std::isfinite(v))return false;
    return true;
}
// Per frame, from the first member's live bytes, independent of any pose: the
// game camera, its lens (tan half-angles) and S. nullptr or why (lens invalid).
struct Lens {bool valid=false;const char* why="not-evaluated";float tan[4]{};}; // left, right, up, down (magnitudes)
inline const char* camera_frame(const uint8_t* raw,PoseFrame& f,Scratch& sc,Lens& lens) noexcept {
    lens.valid=false;
    auto fail=[&](const char* why){lens.why=why;return why;};
    load16(raw,sc.m0);load16(raw+64,sc.m64);load16(raw+256,sc.m256);
    for(int i=0;i<16;++i)if(!std::isfinite(sc.m0[i])||!std::isfinite(sc.m64[i])||!std::isfinite(sc.m256[i]))return fail("srt-matrix-nonfinite");
    if(sc.m256[3]!=0||sc.m256[7]!=0||sc.m256[11]!=0||sc.m256[15]!=1)return fail("m256-not-affine");
    // VP = inverse(M256) * M64
    std::memcpy(sc.a,sc.m256,sizeof sc.a);if(!inv44(sc.a,sc.inv))return fail("srt-matrix-singular");
    mul44(sc.inv,sc.m64,f.vp);
    if(!decompose(f.vp,f.cam))return fail("camera-degenerate");
    // S = inverse(M64) * M0
    std::memcpy(sc.a,sc.m64,sizeof sc.a);if(!inv44(sc.a,sc.inv))return fail("m64-singular");
    mul44(sc.inv,sc.m0,f.S);
    if(!(f.cam.tanX>0)||!(f.cam.tanY>0)||f.cam.tanX>100||f.cam.tanY>100)return fail("lens-out-of-range");
    lens.tan[0]=lens.tan[1]=float(f.cam.tanX);lens.tan[2]=lens.tan[3]=float(f.cam.tanY);lens.valid=true;lens.why="recovered";
    return nullptr;
}
// ---------------------------------------------------------------- cockpit FOV (J-cockpit-fov)
// gpu_copy_step5_hmd_fov: the eye matrix VP_e is built with the eye's OWN XR tangents instead
// of the game's symmetric lens. The REFERENCE VP_ref stays on the game lens (it reproduces
// the game's own VP = inverse(M256) * M64), so the delta substitution stays exact:
//   M64'_e = M64 + M256 * (VP_e - VP_ref) = M256 * (VP + VP_e - VP_ref) = M256 * VP_e (+ the
//   float rounding of reconstructing VP_ref, ~1e-7 relative, as before),
//   M0'_e = M0 + (M256 * D_e) * S = M256 * VP_e * S  (S = inverse(M64) * M0, the clip->PS2
//   screen map, unchanged), so the GS's ndc = clip.xy / clip.w with VP_e's frustum: the eye's
//   asymmetric edges land on +-1. The render target and viewport stay the game's (2560x1440).
// An eye whose FOV equals the game lens (each tangent within fov_snap_rel) gets EXACTLY the game
// lens tangents, so VP_e, D_e and the substituted bytes equal the key-off path bit for bit.
constexpr double fov_tan_max=4.0;     // |tan| clamp per edge (~76 degrees); clamped frames are counted
constexpr double fov_min_span=1e-3;   // tan(right)-tan(left), tan(up)-tan(down) below this: refused
constexpr double fov_snap_rel=1e-5;   // relative tangent difference treated as the game lens
inline const char* hmd_tangents(const float fov[4],const Camera& c,EyeFov& out,bool& clamped) noexcept {
    out=EyeFov{};
    if(fov[0]==0.0f&&fov[1]==0.0f&&fov[2]==0.0f&&fov[3]==0.0f)return "fov-zero";
    const double lens[4]={-c.tanX,c.tanX,c.tanY,-c.tanY};
    bool changed[4]{};
    for(int i=0;i<4;++i) {
        const double a=double(fov[i]);
        if(!std::isfinite(a))return "fov-nonfinite";
        if(!(std::fabs(a)<1.5707963267948966))return "fov-over-90-degrees";
        double t=std::tan(a);
        if(t>fov_tan_max){t=fov_tan_max;clamped=true;changed[i]=true;}
        else if(t<-fov_tan_max){t=-fov_tan_max;clamped=true;changed[i]=true;}
        if(t!=lens[i]&&std::fabs(t-lens[i])<=fov_snap_rel*std::fabs(lens[i])){t=lens[i];changed[i]=true;}
        out.t[i]=t;
    }
    if(!(out.t[1]>out.t[0])||!(out.t[2]>out.t[3]))return "fov-not-ordered";
    if(!(out.t[1]-out.t[0]>=fov_min_span)||!(out.t[2]-out.t[3]>=fov_min_span))return "fov-span-too-small";
    for(int i=0;i<4;++i)out.angle[i]=changed[i]?float(std::atan(out.t[i])):fov[i];
    return nullptr;
}
// Per-frame pose setup after camera_frame succeeded (f.cam, f.S valid).
// Returns nullptr (f.valid) or why the pose path is not used (fallback: IPD row 3).
// hmdFov (gpu_copy_step5_hmd_fov): use both eyes' own FOV (f.fovHmd), or the game lens for both
// with the reason in f.fovWhy when either eye's FOV is unusable (the pose path still runs).
// lookR (xr_look_drive_keys, look_drive_keys.h; nullptr = key off, byte-identical): the camera-local -> cockpit-local rotation of the look the
// game RENDERED in this frame (row-major 3x3, identity = no look). The camera basis (c.right/up/fwd) already contains that look, so the head
// orientation is applied as R_look^T * (Ry(-yaw0) R): the head minus the look, i.e. cockpit parts and world co-rotate once, not twice.
// The head position offset is rotated the same way (the head sways in the cockpit frame, not in the looking camera's frame).
// eyeOffset (POV-RENDER stage 4, the cinematic anchor rig; nullptr = byte-identical): a world-space offset of BOTH eyes from the game camera (the
// anchor point minus the camera eye). VP_ref stays the game camera's, so D_e = VP_e - VP_ref carries the offset into every substituted record.
inline const char* setup_poses(const EyePose* poses,Neutral& n,PoseFrame& f,Scratch& sc,bool hmdFov=false,double unitsPerMetre=step3::units_per_metre,const double* lookR=nullptr,const pov::Lean* lean=nullptr,const double* eyeOffset=nullptr) noexcept {
    f.valid=false;f.fovHmd=false;f.fovClamped=false;f.fovWhy=hmdFov?"not-evaluated":"not-requested";
    auto fail=[&](const char* why){f.why=why;return why;};
    if(!poses||!pose_ok(poses[0])||!pose_ok(poses[1]))return fail("pose-invalid");
    // Seated recentering (first valid pose fixes the neutral head centre and yaw).
    double* RL=sc.R[0];double* RR=sc.R[1];quat_rot(poses[0].orientation,RL);quat_rot(poses[1].orientation,RR);
    if(!n.set) {
        for(int k=0;k<3;++k)n.p0[k]=0.5*(double(poses[0].position[k])+double(poses[1].position[k]));
        const double fx=-RL[2],fz=-RL[8]; // forward = R (0,0,-1)
        n.yaw0=std::hypot(fx,fz)>1e-6?std::atan2(-fx,-fz):0.0;n.set=true;
    }
    const double cy=std::cos(-n.yaw0),sv=std::sin(-n.yaw0);
    // WORLD-SCALE: [1, 100] (was [20, 100]) so the world-object path can use the evidenced ~5 u/m; the config key itself stays clamped to [20, 100].
    const Camera& c=f.cam;const double s=(unitsPerMetre>=1&&unitsPerMetre<=100)?unitsPerMetre:step3::units_per_metre;
    build_vp(c.eye,c.right,c.up,c.fwd,-c.tanX,c.tanX,c.tanY,-c.tanY,c,f.vpRef);
    // Per-eye tangents (left, right, up, down): the game lens unless the HMD FOV is requested and usable.
    double T[2][4]={{-c.tanX,c.tanX,c.tanY,-c.tanY},{-c.tanX,c.tanX,c.tanY,-c.tanY}};
    bool useHmd=false;
    if(hmdFov) {
        bool clamped=false;const char* why=hmd_tangents(poses[0].fov,c,f.fov[0],clamped);
        if(!why)why=hmd_tangents(poses[1].fov,c,f.fov[1],clamped);
        if(why){f.fovWhy=why;f.fov[0]=f.fov[1]=EyeFov{};}
        else {useHmd=true;f.fovClamped=clamped;f.fovWhy=clamped?"hmd-clamped":"hmd";
            for(int e=0;e<2;++e)for(int i=0;i<4;++i)T[e][i]=f.fov[e].t[i];}
    }
    // F-LEAN (gpu_copy_step5_neck_lever_m; lean == nullptr or inactive: the original expressions below, byte-identical): the head-centre
    // translation minus the neck-pivot arc, times the translation scale; the eye separation (p - midpoint) is kept.
    const bool leanOn=lean&&lean->active();double leanC[3]{},leanM[3]{};
    if(leanOn) {
        double mrel[3],dl[3];for(int k=0;k<3;++k){leanM[k]=0.5*(double(poses[0].position[k])+double(poses[1].position[k]));mrel[k]=leanM[k]-n.p0[k];}
        pov::neck_delta_local(RL,n.yaw0,lean->lever,dl);pov::virtual_centre_offset(mrel,dl,lean->scale,leanC);
    }
    for(unsigned e=0;e<2;++e) {
        const EyePose& p=poses[e];const double* R=e?RR:RL;
        double d[3]={p.position[0]-n.p0[0],p.position[1]-n.p0[1],p.position[2]-n.p0[2]};
        if(leanOn)for(int k=0;k<3;++k)d[k]=(double(p.position[k])-leanM[k])+leanC[k];
        double dr[3]={cy*d[0]+sv*d[2],d[1],-sv*d[0]+cy*d[2]};
        // v' = R_look^T v (only with lookR).
        auto unlook=[&](double* v){if(!lookR)return;const double x=v[0],y=v[1],z=v[2];
            for(int i=0;i<3;++i)v[i]=lookR[i]*x+lookR[3+i]*y+lookR[6+i]*z;};
        unlook(dr);
        auto world=[&](const double* v,double* out){for(int k=0;k<3;++k)out[k]=v[0]*c.right[k]+v[1]*c.up[k]-v[2]*c.fwd[k];};
        double ew[3];world(dr,ew);for(int k=0;k<3;++k)f.eyeWorld[e][k]=eyeOffset?c.eye[k]+eyeOffset[k]+s*ew[k]:c.eye[k]+s*ew[k];
        // Recentred axes: R' = Ry(-yaw0) R; columns are the eye's x, y, z axes.
        double (&ax)[3][3]=sc.ax;
        for(int col=0;col<3;++col){const double v[3]={R[0+col],R[3+col],R[6+col]};
            ax[col][0]=cy*v[0]+sv*v[2];ax[col][1]=v[1];ax[col][2]=-sv*v[0]+cy*v[2];unlook(ax[col]);}
        double re[3],ue[3],fe[3];const double negz[3]={-ax[2][0],-ax[2][1],-ax[2][2]};
        world(ax[0],re);world(ax[1],ue);world(negz,fe);
        // The game's lens (key off) or the eye's own FOV (gpu_copy_step5_hmd_fov).
        build_vp(f.eyeWorld[e],re,ue,fe,T[e][0],T[e][1],T[e][2],T[e][3],c,f.vpEye[e]);
        for(int i=0;i<16;++i){f.D[e][i]=f.vpEye[e][i]-f.vpRef[i];if(!std::isfinite(f.D[e][i]))return fail("pose-matrix-nonfinite");}
    }
    f.fovHmd=useHmd;f.valid=true;f.why="pose";return nullptr;
}
// Rebase before constructing any eye matrices in a new LOCAL coordinate space.
// Unknown generation zero is its own epoch; never reuse an old known neutral.
inline const char* setup_poses_generation(const EyePose* poses,uint32_t generation,Neutral& n,PoseFrame& f,Scratch& sc,bool hmdFov=false,double unitsPerMetre=step3::units_per_metre,const double* lookR=nullptr,const pov::Lean* lean=nullptr,const double* eyeOffset=nullptr) noexcept {
    if(n.generation!=generation){n=Neutral{};n.generation=generation;}
    return setup_poses(poses,n,f,sc,hmdFov,unitsPerMetre,lookR,lean,eyeOffset);
}
// J-quad-yaw (StereoFrame::neutral_valid / neutral_pos / neutral_yaw): the neutral the frame's
// eyes were recentred with. Only for a frame whose pose path ran (poseValid, so `n` is the
// neutral of that frame's space generation: setup_poses_generation resets it on a change
// before use); otherwise invalid + zeros. The cockpit forward in LOCAL is Ry(yaw0) (0,0,-1)
// = (-sin yaw0, 0, -cos yaw0) from p0 (yaw0 = atan2(-fx, -fz) of the first left-eye forward).
struct NeutralExport {bool valid=false;float pos[3]{};float yaw=0.0f;};
inline NeutralExport neutral_export(const Neutral& n,bool poseValid) noexcept {
    NeutralExport e;
    if(!poseValid||!n.set)return e;
    for(int k=0;k<3;++k)if(!std::isfinite(n.p0[k]))return e;
    if(!std::isfinite(n.yaw0))return e;
    for(int k=0;k<3;++k)e.pos[k]=float(n.p0[k]);
    e.yaw=float(n.yaw0);e.valid=true;return e;
}
// Both steps (fixtures and the once-per-frame setup).
inline const char* setup_frame(const uint8_t* raw,const EyePose* poses,Neutral& n,PoseFrame& f,Scratch& sc,Lens* lensOut=nullptr,bool hmdFov=false) noexcept {
    Lens lens;f.valid=false;
    if(const char* why=camera_frame(raw,f,sc,lens)){f.why=why;if(lensOut)*lensOut=lens;return why;}
    if(lensOut)*lensOut=lens;
    return setup_poses(poses,n,f,sc,hmdFov);
}
// Per member (Draw path, heap scratch): out = raw with +64 and +0 replaced by
// raw + M256*D_e and raw + (M256*D_e)*S; every other byte copied unchanged.
inline const char* pose_substitute(const uint8_t* raw,const PoseFrame& f,unsigned eye,uint8_t* out,Scratch& sc) noexcept {
    load16(raw+256,sc.m256);
    if(sc.m256[3]!=0||sc.m256[7]!=0||sc.m256[11]!=0||sc.m256[15]!=1)return "m256-not-affine";
    mul44(sc.m256,f.D[eye],sc.d64);
    std::memcpy(out,raw,size_t(step3::blob_bytes));
    for(int i=0;i<16;++i) {
        double d0=0;for(int k=0;k<4;++k)d0+=sc.d64[(i/4)*4+k]*f.S[k*4+(i%4)];
        float r64,r0;std::memcpy(&r64,raw+64+4*i,4);std::memcpy(&r0,raw+4*i,4);
        // A zero delta keeps the raw bits (also -0.0, which +0.0 would flip).
        const float n64=sc.d64[i]==0.0?r64:float(double(r64)+sc.d64[i]),n0=d0==0.0?r0:float(double(r0)+d0);
        if(!std::isfinite(n64)||!std::isfinite(n0))return "substituted-matrix-nonfinite";
        std::memcpy(out+64+4*i,&n64,4);std::memcpy(out+4*i,&n0,4);
    }
    return nullptr;
}
// ---------------------------------------------------------------- HUD capture (J-hud-capture)
// The post-cockpit HUD draws (live run 7: ~58 draws into the FP16 scene target after the last
// cockpit member, depth ALWAYS/no write, stencil ALWAYS, blending on) are re-issued unchanged
// (game PSO, root arguments, viewport, live Draw arguments) with only the render target replaced
// by a private per-slot target (scene format/size, cleared to 0 once per frame) and, when the PSO
// declares a DSV format, a private depth-stencil of that format (cleared depth 0, stencil 0).
// Screen space and mono: no matrix substitution, one image for both eyes.
// Per draw: OMSet(private), Draw, OMSet(game restore from the bind-time descriptor ring) = 3;
// once per frame (and again after a member resume): ClearRTV [+ ClearDSV].
constexpr unsigned hud_draw_cap=160,hud_command_cap=600,hud_per_draw=3;
// J-hud-radar (gpu_copy_step5_hud_radar, default OFF; J-hud-radar-findings.md): the radar group and the late quads join the replay, so the
// caps are raised for frames armed with that key (key off keeps 160 / 600 exactly). Budget: per frame at most 3 clears (HUD target, marker
// target, shared depth-stencil) + 3 commands per draw; hud_radar_budget() is the single check (also run by the offline tests):
// 256 * 3 + 3 = 771 <= 960 commands, and the command memory estimate (hud_radar_command_bytes per recorded command, the game's list
// allocator grows by it) stays under hud_radar_memory_cap. The private targets are the existing ones (no new resource).
constexpr unsigned hud_radar_draw_cap=256,hud_radar_command_cap=960,hud_radar_clears=3,hud_radar_excursion_cap=8;
constexpr uint64_t hud_radar_command_bytes=64,hud_radar_memory_cap=96ull<<10;
// RADAR-SCENE-RESUME (radar key only; docs/world-render/radar-markers-game-original.md): the scene target leaving RENDER_TARGET WHILE the region is on an
// excursion (the radar's mid-group pass reads the scene: step39 hud-end `excursions=1;returns=0;post-excursion-draws=0;end=scene-barrier`) is resumed when
// the SAME resource is transitioned back INTO RENDER_TARGET by a plain full barrier. At most hud_radar_scene_resume_cap resumes per region lifetime.
// A scene barrier while NOT away (the real tone-map hand-off) still ends the region for good, exactly as before.
constexpr unsigned hud_radar_scene_resume_cap=4;
// nullptr = the transition back may resume the region, else why not. flags: D3D12_RESOURCE_BARRIER_FLAGS (0 = NONE; split halves refuse);
// subresource: 0 or 0xffffffff (all). Pure (offline tests).
inline const char* radar_scene_back(uint32_t flags,uint32_t subresource,unsigned resumes) noexcept {
    if(flags!=0)return "split-barrier";
    if(subresource!=0&&subresource!=0xffffffffu)return "partial-subresource";
    if(resumes>=hud_radar_scene_resume_cap)return "resume-cap";
    return nullptr;
}
inline unsigned hud_draw_cap_for(bool radar) noexcept {return radar?hud_radar_draw_cap:hud_draw_cap;}
inline unsigned hud_command_cap_for(bool radar) noexcept {return radar?hud_radar_command_cap:hud_command_cap;}
// nullptr when the radar caps fit the budgets, else why. Pure (the offline tests also call it with shrunk numbers).
inline const char* hud_radar_budget(unsigned draws,unsigned commands,unsigned clears,uint64_t bytes_per_command,uint64_t memory_cap) noexcept {
    if(!draws||!commands)return "radar-caps-zero";
    if(uint64_t(draws)*hud_per_draw+clears>commands)return "radar-commands-below-draws";
    if(uint64_t(commands)*bytes_per_command>memory_cap)return "radar-command-memory-over-cap";
    return nullptr;
}
enum HudCategory : int {hud_state=0,hud_descriptor=1,hud_format=2,hud_cap=3,hud_categories=4};
inline const char* hud_category_name(int c) noexcept {
    static constexpr const char* n[]={"state","descriptor","format","cap"};return c>=0&&c<hud_categories?n[c]:"?";
}
// radar* (J-hud-radar; all false/0 = the original rules): the frame's stencil proof for the EQUAL relaxation of hud_decide.
//   radar_replace    : a REPLACE writer (stencil ALWAYS, pass op REPLACE) was replayed into the private depth-stencil in this region;
//   radar_gap        : a stencil writer on the scene target was NOT replayed after that REPLACE writer;
//   radar_write_mask : the write mask of the last replayed REPLACE writer (it must cover the EQUAL draw's read mask).
struct HudTarget {uint32_t format=0,ds_format=0;uint64_t width=0;uint32_t height=0;uint64_t scene=0;
                  bool radar=false,radar_replace=false,radar_gap=false;uint32_t radar_write_mask=0;
                  bool depth_gap=false;}; // depth_gap: a depth-writing draw was SKIPPED (HUD-RS-SKIP) this region, so the private depth lacks its writes
struct HudVerdict {const char* why=nullptr;int category=hud_state;bool depth_tested=false;};
// One HUD-region draw whose RT 0 already resolved to the frame's scene target. Replayable
// (why == nullptr) only when everything the replay depends on is known and the private targets
// reproduce the game's result: the replay keeps every game binding except OM, so the root
// signature/arguments must be tracked, the restore must re-bind the bind-time descriptors, the
// PSO must write exactly RT 0 in the private format, and depth/stencil tests must not depend on
// the GAME's depth/stencil contents (the private DS is freshly cleared).
inline HudVerdict hud_decide(const step3::Shape& s,const HudTarget& t) noexcept {
    using namespace step3;
    auto refuse=[](const char* why,int cat){HudVerdict v;v.why=why;v.category=cat;return v;};
    if(!s.tracking_complete||s.tracking_gap)return refuse("binding-tracking-incomplete",hud_descriptor);
    if(!s.root_route)return refuse("root-signature-unknown",hud_descriptor);
    if(!s.pso.known)return refuse("pso-output-unknown",hud_state);
    if(s.pso.so_entries)return refuse("stream-output",hud_state);
    if(s.om_gap)return refuse("om-state-gap",hud_state);
    if(!s.om_known||!s.om_set)return refuse("om-state-unknown",hud_state);
    if(s.om_count!=1)return refuse("om-rt-count-not-1",hud_state);
    if(s.pso.rt_count!=1)return refuse("pso-rt-count-not-1",hud_state);
    if(std::strcmp(s.game_rt_status,"resolved")!=0)return refuse("game-rt-unresolved",hud_state);
    if(!t.scene||s.game_rt!=t.scene)return refuse("not-scene-target",hud_state); // callers end the region first
    if(s.pso.samples!=1||s.pso.quality!=0)return refuse("msaa",hud_format);
    if(s.pso.rtv[0]!=t.format)return refuse("rt-format-mismatch",hud_format);
    if(s.game_rt_format!=t.format||s.game_rt_width!=t.width||s.game_rt_height!=t.height)return refuse("scene-desc-changed",hud_format);
    if(s.pso.dsv!=dxgi::unknown&&s.pso.dsv!=t.ds_format)return refuse("ds-format-mismatch",hud_format);
    HudVerdict ok;
    if(s.pso.depth_enable) {
        if(s.pso.dsv==dxgi::unknown)return refuse("depth-enabled-without-dsv-format",hud_state);
        switch(s.pso.depth_func) {
        case cmp::always:case cmp::never:break;
        // The private depth is cleared to 0 (E2: PS2 depth, larger is nearer): the test passes
        // wherever the GAME's test passes against an empty scene. Occlusion by the scene is lost.
        // A skipped depth writer (HUD-RS-SKIP, t.depth_gap) leaves the private depth different from the game's: a depth-TESTED draw is refused then.
        case cmp::greater:case cmp::greater_equal:
            if(t.depth_gap)return refuse("depth-writer-skipped",hud_state);
            ok.depth_tested=true;break;
        default:return refuse("depth-test-not-replayable",hud_state);
        }
    }
    if(s.pso.stencil) {
        if(s.pso.dsv==dxgi::unknown)return refuse("stencil-enabled-without-dsv-format",hud_state);
        if(s.pso.front_func!=cmp::always||s.pso.back_func!=cmp::always) {
            // J-hud-radar: a pure stencil EQUAL test (both faces, no stencil write) is replayable when the private depth-stencil holds
            // what the game's writers put there: a REPLACE writer was replayed, no writer was skipped since, and its write mask covers the
            // read mask. The stencil reference is the game's own (the list keeps it across the replay's OMSetRenderTargets; untracked).
            if(!t.radar||s.pso.front_func!=cmp::equal||s.pso.back_func!=cmp::equal)return refuse("stencil-test-not-always",hud_state);
            if(s.pso.stencil_write!=0&&!s.pso.stencil_ops_keep)return refuse("radar-equal-writes-stencil",hud_state);
            if(!t.radar_replace)return refuse("radar-equal-without-writer",hud_state);
            if(t.radar_gap)return refuse("stencil-writer-skipped",hud_state);
            if((s.pso.stencil_read&~t.radar_write_mask)!=0)return refuse("radar-mask-not-covered",hud_state);
        }
    }
    if(!s.om_desc_proved)return refuse("om-descriptor-unproved",hud_descriptor); // the restore would not rebind the game's views
    return ok;
}
// The frame's HUD layer is published only when the region replayed every draw.
inline bool hud_frame_valid(bool armed,unsigned draws,unsigned refused) noexcept {return armed&&draws>0&&refused==0;}
// HUD-RS-SKIP (default ON; xr_terrain_stage hudrs:0 turns it off; docs/world-render/headset-0-3-4-9-10-analysis.md item 3): a HUD-region draw whose
// render target 0 blends with colour op SUBTRACT or REV_SUBTRACT is a SKIPPED class: not replayed into the private HUD/marker image, counted, and
// NOT a refusal (hud_frame_valid is unchanged by it). Live null run (2026-10-29): the sun lens flare is two 4-vertex quads, PS d34ee153 / VS 534f29ca,
// depth ALWAYS, blend ONE,ONE,REV_SUBTRACT / ONE,ZERO,ADD (dest - src darkens the game's scene). Replayed over the transparent (0,0,0,0) HUD image,
// dest - src clamps to 0 while alpha = src alpha, so the consumer composited an opaque BLACK ring/core over the eye scene. The game's own draw
// (the window/mirror) is untouched. Every other blend (additive 5,2,1 HUD sprites, blend off, MIN/MAX) keeps the hud_decide rules exactly.
namespace blend_op {constexpr uint32_t add=1,subtract=2,rev_subtract=3,min=4,max=5;}
inline bool hud_rs_subtract(const step3::PsoOutput& p) noexcept {
    // W10-1 P1#1: ONLY the observed flare class (colour REV_SUBTRACT ONE/ONE, alpha ONE/ZERO/ADD). SUBTRACT (src - dest, positive content) and every other
    // tuple keep the hud_decide rules.
    return p.known&&p.blend0&&p.blend_op0==blend_op::rev_subtract&&p.blend_op_alpha0==blend_op::add&&p.rs_flare_factors;
}
// The replay's decision for one HUD-region draw that reached the class gate: true = skip (on = the switch).
inline bool hud_rs_skip(const step3::Shape& s,bool on) noexcept {return on&&hud_rs_subtract(s.pso);}
// ---------------------------------------------------------------- HUD radar group (J-hud-radar)
// gpu_copy_step5_hud_radar (default OFF; needs gpu_copy_step5_hud; src/acz-vr/J-radar-analysis.md, J-hud-radar-findings.md). The radar is drawn
// at the END of the HUD region with a stencil mask: REPLACE quads, a3050-shaped triangles on another target, one NOT_EQUAL quad and EQUAL
// contact draws. The original region latched at the first render-target change, so the contacts were never replayed.
// Segmented region: a draw on (or a bind of) another target is an EXCURSION (not replayed, no refusal); the replay resumes when the scene
// target returns. Draws after an excursion are LATE: they are replayed only inside a radar WINDOW (opened by a REPLACE writer or a stencil
// EQUAL draw, closed by the next excursion); every other late draw is counted and skipped so the unknown bloom quads after the tone-map
// passes stay out of the HUD image until their content is known.
inline bool hud_stencil_writer(const step3::PsoOutput& p) noexcept {return p.known&&p.stencil&&p.stencil_write!=0&&!p.stencil_ops_keep;}
// A stencil writer whose pass op is REPLACE with an ALWAYS test: it paints the mask (the draws that make the private stencil match the game's).
inline bool hud_replace_writer(const step3::PsoOutput& p) noexcept {
    return p.known&&p.stencil&&p.stencil_write!=0&&p.front_pass==3&&p.front_func==step3::cmp::always&&p.back_func==step3::cmp::always;
}
inline bool hud_equal_test(const step3::PsoOutput& p) noexcept {
    return p.known&&p.stencil&&p.front_func==step3::cmp::equal&&p.back_func==step3::cmp::equal;
}
// The a3050-shaped triangle: 3 unindexed vertices, depth LESS with depth WRITE, stencil EQUAL (live: pass ZERO, viewport 2405x1352, om=0 /
// unresolved target). Never replayed: it writes depth and its target is not the scene.
inline bool radar_a3050_shaped(const step3::Shape& s) noexcept {
    return s.pso.known&&!s.indexed&&s.count==3&&s.instances==1&&s.pso.depth_enable&&s.pso.depth_write&&hud_equal_test(s.pso);
}
// The NOT_EQUAL quad: 4 unindexed vertices, stencil NOT_EQUAL on both faces, no depth write (live: ALWAYS blend 5/2, viewport = scene).
inline bool radar_notequal_shaped(const step3::Shape& s) noexcept {
    return s.pso.known&&s.pso.stencil&&!s.indexed&&s.count==4&&s.instances==1&&!s.pso.depth_write&&
           s.pso.front_func==step3::cmp::not_equal&&s.pso.back_func==step3::cmp::not_equal;
}
// Per-frame radar bookkeeping (a Step5Hud member; plain data, restarted with the region).
struct HudRadar {
    // The stencil proof (see HudTarget::radar_*).
    bool replace=false,gap=false;uint32_t write_mask=0;
    // Counters (this region): replays by role, skips by class, region events.
    unsigned replaceReplayed=0,equalReplayed=0,equalIndices=0,lateReplayed=0,windows=0;
    unsigned a3050=0,notEqualSkipped=0,lateSkipped=0,awaySkipped=0,excursionWriters=0,stencilGaps=0,excursions=0,returns=0;
    unsigned equalRefused=0,gapRefused=0;
    // RADAR-SCENE-RESUME: scene barriers out of RENDER_TARGET while away, resumes after the barrier back, refused barriers back (copied at close).
    unsigned sceneLeaves=0,sceneResumes=0,sceneBackRefused=0;
};
inline void radar_target(const HudRadar& r,HudTarget& t) noexcept {t.radar=true;t.radar_replace=r.replace;t.radar_gap=r.gap;t.radar_write_mask=r.write_mask;}
// A draw was replayed into the private targets.
inline void radar_note_replayed(HudRadar& r,const step3::Shape& s,bool late) noexcept {
    if(hud_replace_writer(s.pso)){r.replace=true;r.gap=false;r.write_mask=s.pso.stencil_write;++r.replaceReplayed;}
    else if(hud_equal_test(s.pso)){++r.equalReplayed;if(s.indexed)r.equalIndices+=s.count;}
    if(late)++r.lateReplayed;
}
// A scene-target draw was not replayed (skipped class or refusal): a stencil writer among them leaves the private stencil short of the game's.
inline void radar_note_not_replayed(HudRadar& r,const step3::Shape& s) noexcept {
    if(hud_stencil_writer(s.pso)){r.gap=true;++r.stencilGaps;}
}
enum RadarAction : int {radar_replay=0,radar_refuse=1,radar_skip_a3050=2,radar_skip_notequal=3,radar_skip_late=4};
struct RadarVerdict {int action=radar_replay;HudVerdict v;};
// The radar-aware decision for one scene-RT-or-unknown draw of an armed, not-away region. `late`: an excursion happened before this draw;
// `window`: a radar window is open. Pure; the caller keeps the counters (radar_note_*).
inline RadarVerdict hud_radar_decide(const step3::Shape& s,const HudTarget& base,const HudRadar& r,bool late,bool window) noexcept {
    RadarVerdict out;HudTarget t=base;radar_target(r,t);
    const bool onScene=s.om_known&&s.om_set&&s.om_count==1&&std::strcmp(s.game_rt_status,"resolved")==0&&t.scene&&s.game_rt==t.scene;
    if(radar_a3050_shaped(s)&&!onScene){out.action=radar_skip_a3050;return out;}
    if(late&&!window) {
        if(radar_notequal_shaped(s)){out.action=radar_skip_notequal;return out;}
        if(!(hud_replace_writer(s.pso)||hud_equal_test(s.pso))){out.action=radar_skip_late;return out;}
    }
    out.v=hud_decide(s,t);
    if(!out.v.why)return out;
    if(std::strcmp(out.v.why,"stencil-test-not-always")==0&&radar_notequal_shaped(s)){out.action=radar_skip_notequal;out.v=HudVerdict{};return out;}
    out.action=radar_refuse;return out;
}
// One diagnostic character per draw (stencil test function of the draw) and its disposition.
inline char radar_func_char(const step3::PsoOutput& p) noexcept {
    if(!p.known)return '?';if(!p.stencil)return '-';
    switch(p.front_func) {
    case step3::cmp::always:return 'A';case step3::cmp::equal:return 'E';case step3::cmp::not_equal:return 'N';case step3::cmp::never:return 'X';
    case step3::cmp::less:return 'l';case step3::cmp::less_equal:return 'k';case step3::cmp::greater:return 'g';case step3::cmp::greater_equal:return 'h';
    default:return '?';
    }
}
// Dispositions: 'r' replayed, 'x' refused, 'a' away (another target), 't' no target, 'n' a3050 skipped, 'q' NOT_EQUAL skipped, 'l' late skipped, 's' HUD-RS-SKIP (subtract blend).
// The would-be verdicts and the per-draw strings of the region (the hud-end diagnostic).
constexpr unsigned radar_diag_draws=128,radar_diag_verdicts=8;
struct HudRadarDiag {
    bool haveEnd=false;const char* endWhy=nullptr;                 // the first excursion: cause (rt-change / draw-on-other-target)
    uint64_t endOrdinal=0,endRt=0,endWidth=0;uint32_t endHeight=0,endFormat=0,endOmCount=0;const char* endStatus=nullptr;
    unsigned seen=0,post=0;                                        // draws seen by the seam; draws after the first excursion
    const char* verdictWhy[radar_diag_verdicts]{};unsigned verdictCount[radar_diag_verdicts]{};unsigned verdictOther=0;
    char funcs[radar_diag_draws+1]{};char disp[radar_diag_draws+1]{};  // the LAST radar_diag_draws draws, ring
    void add_draw(char func,char d) noexcept {const unsigned i=seen%radar_diag_draws;funcs[i]=func;disp[i]=d;++seen;}
    void add_verdict(const char* why) noexcept {
        ++post;if(!why)why="accept";
        for(unsigned i=0;i<radar_diag_verdicts;++i){if(verdictWhy[i]==why||(verdictWhy[i]&&std::strcmp(verdictWhy[i],why)==0)){++verdictCount[i];return;}
            if(!verdictWhy[i]){verdictWhy[i]=why;verdictCount[i]=1;return;}}
        ++verdictOther;
    }
    // The last draws in submit order, NUL terminated.
    unsigned ordered(char* f,char* d) const noexcept {
        const unsigned n=seen<radar_diag_draws?seen:radar_diag_draws;const unsigned start=seen<radar_diag_draws?0:seen%radar_diag_draws;
        for(unsigned i=0;i<n;++i){f[i]=funcs[(start+i)%radar_diag_draws];d[i]=disp[(start+i)%radar_diag_draws];}
        f[n]=0;d[n]=0;return n;
    }
};
// ---------------------------------------------------------------- HUD marker split (J-hud-markers)
// gpu_copy_step5_hud_markers (default OFF; src/acz-vr/J-hud-split-analysis.md, J-hud-markers-findings.md):
// the target/friendly markers are replayed into a SECOND private target, everything else stays in the
// HUD target. Classification is by a pure rule over the HUD-region draw sequence of ONE frame:
//  * marker box   : the draw's PSO tests depth with GREATER / GREATER_EQUAL (the only process-independent
//                   signature on record: 3 per frame in run 11, none among the fixed instruments) [E];
//  * marker label : the draw IMMEDIATELY after a box (box or label) that is indexed, not depth-tested, and
//                   uses the text PSO route. The text route is learned as the route of the first such draw
//                   that follows the first box of the frame [G: heuristic, one scenario];
//  * instrument   : everything else (also a text draw that does not directly follow a box).
// A draw belongs to exactly one target. The decision uses only the draw's own Shape, so refused draws
// are classified identically (a refusal is charged to the layer the draw belongs to).
enum HudClass : int {hud_instrument=0,hud_marker_box=1,hud_marker_label=2};
struct HudMarkerState {bool prev_box=false,text_known=false;uint64_t text_route=0;};
// Depth func GREATER/GREATER_EQUAL with depth testing enabled (the Shape does not need to be replayable).
inline bool hud_depth_tested(const step3::Shape& s) noexcept {
    return s.pso.known&&s.pso.depth_enable&&(s.pso.depth_func==step3::cmp::greater||s.pso.depth_func==step3::cmp::greater_equal);
}
inline HudClass hud_classify(HudMarkerState& st,bool depth_tested,bool indexed,uint64_t pso_route) noexcept {
    if(depth_tested){st.prev_box=true;return hud_marker_box;}
    const bool after_box=st.prev_box;st.prev_box=false;
    if(!after_box||!indexed)return hud_instrument;
    if(!st.text_known){st.text_known=true;st.text_route=pso_route;return hud_marker_label;}
    return pso_route==st.text_route?hud_marker_label:hud_instrument;
}
// Memory: one more private colour target per slot (the depth-stencil of the HUD target is shared; the
// marker draws are depth-tested against the same cleared-to-0 depth and write no depth). Charged like
// step3::finalize (64 KiB aligned resource + one 64 KiB descriptor heap) and refused above the caps.
constexpr uint64_t hud_marker_slot_cap=32ull<<20;    // one marker colour target (= step3::rt_byte_cap)
constexpr uint64_t hud_marker_total_cap=128ull<<20;  // all slots (2560x1440 FP16 x 3 = 88.5 MB)
inline const char* hud_markers_budget(uint32_t format,uint64_t width,uint32_t height,unsigned slot_count,uint64_t& charged) noexcept {
    charged=0;
    step3::ColorFormat c;
    if(!step3::color_format(format,c))return "hud-markers-format-unsupported";
    if(!width||!height||width>step3::max_dimension||height>step3::max_dimension)return "hud-markers-size-unsupported";
    const uint64_t bytes=width*height*c.bytes;
    if(bytes>hud_marker_slot_cap)return "hud-markers-over-slot-cap";
    charged=(step3::align_up(bytes,step3::charge_alignment)+step3::charge_alignment)*slot_count;
    if(charged>hud_marker_total_cap){const char* why="hud-markers-over-total-cap";charged=0;return why;}
    return nullptr;
}
// ---------------------------------------------------------------- effects layer (J-effects-layer)
// gpu_copy_step5_effects (default OFF; needs gpu_copy_step5_continuous + gpu_copy_step5_hud; J-effects-layer-findings.md).
// Missiles, explosions and gun tracers are NON-member scene draws between the cockpit members (phase "between-members" of
// the phase observer), drawn after the world snapshot (taken at the first member) and not covered by the HUD replay (which
// keeps only the region after the LAST member). They are replayed inline, exactly like the HUD tail (private render target
// bound, the game's own Draw under the game's own state, original bindings restored), into a third private per-slot target.
// The between/tail distinction is only known when a LATER member (or the close) arrives, so every candidate draw is recorded
// SPECULATIVELY under GPU predication: SetPredication(predicate[epoch], NOT_EQUAL_ZERO) around the replay draw, where the
// epoch is the run of candidates between two members. A later member COMMITS the epoch (the CPU writes 1 into the predicate
// before the list is submitted); the draws of the last epoch (the tail, already covered by the HUD) stay predicated off.
// The classifier is a pure function of the draw's Shape plus the frame phase; an IGNORED draw is not an effect candidate
// (counted nowhere as a failure), a REFUSED draw was a candidate the replay cannot reproduce (charged to the epoch it is in,
// counted only if that epoch commits).
// J-test8-fixes: 96 (was 64). Test 8 (cap 64) showed the cap reached EVERY frame (tail 59.7 + committed 3.0 candidates) and `last-refusal=effects-cap`.
// Candidates are decided in submit order and an epoch only ends at a LATER cockpit member, so the tail (HUD region + after-tonemap draws, about 52-60 per
// frame) is always decided AFTER every between-members candidate of the frame: the cap can only cut the tail (predicated off anyway), never an effect,
// UNLESS a burst (12 committed + 52 HUD) exceeds it with a further member after the HUD draws. 96 leaves the margin; the tail draws themselves are no
// longer what the cap is about (refusals of the open epoch only count when that epoch commits, see EffectsEpochs::commit).
// (Q, step174 fxadd null runs, 2026-11-01) 256 (was 96). With fxadd:1 the additive 4caf6e2d family and the point lists join the sprite classes, and the dense
// frames overran 96: AGC1 `refused=3631(...cap:3631)`, `first-refusal=effects-cap` at draws=96, 49 status lines with cap refusals; AGB2 12 lines. Measured per-frame demand
// (fx-sprite-plan lines, 1 sampled frame per second): class=sprite prepared 131 (AGB2 f9673), 129, 115 (AGC1 f7961), class=tracer 55-58 drawn, explosion 1-2, so a
// coincident worst frame is ~191 candidates. 256 = 1.34 x that. Cost: only the recorded command count (12 per sprite draw, ~3,100 per frame at the cap, no
// command-list byte limit) and the predicate bookkeeping (kMaxDraws words); no per-draw memory is reserved here.
constexpr unsigned effects_draw_cap=256;             // speculative replays per frame (committed + open epoch)
constexpr unsigned effects_epoch_cap=32;             // predicate slots (epochs with >= 1 candidate) per frame
constexpr unsigned effects_per_draw=5;               // SetPredication, OMSet(effects), Draw, SetPredication(null), OMSet(restore)
constexpr unsigned effects_command_cap=effects_draw_cap*effects_per_draw+2; // + ClearRTV, ClearDSV
constexpr uint64_t effects_slot_cap=32ull<<20;       // one effects colour target (= step3::rt_byte_cap)
constexpr uint64_t effects_total_cap=128ull<<20;     // all slots, plus the predicate buffer
constexpr uint64_t effects_predicate_stride=8;       // one 64-bit predicate per epoch
enum EffectsPhase : int {effects_before_first_member=0,effects_open=1,effects_scene_closed=2};
inline const char* effects_phase_name(int p) noexcept {
    static constexpr const char* n[]={"before-first-member","open","scene-closed"};return p>=0&&p<3?n[p]:"?";
}
// ---- J-effects-smoke-fix (D3D12 predication polarity + record-time class allowlist) ----
// ROOT CAUSE (live run L, J-effects-smoke-fix-findings.md; Astra review of issue 31, finding 1): SetPredication does NOT mean "execute when the
// comparison holds". D3D12_PREDICATION_OP_EQUAL_ZERO "enables predication if all 64 bits are zero" and an enabled predication DISCARDS the draw;
// NOT_EQUAL_ZERO discards when any bit is set. The first version recorded NOT_EQUAL_ZERO with 1 = commit, i.e. committed (between-members) epochs
// were DISCARDED and the uncommitted tail (word 0) EXECUTED: the smoke vanished and the HUD tail double-rendered. The default is now EQUAL_ZERO
// (word 0 = discard = tail, word != 0 = execute = committed). Optional override file %LOCALAPPDATA%\acz-vr\effects-predicate.txt:
// "eq0" | "ne0" (the old, inverted op) | "off" (no predication at all: allowlisted candidates always execute).
enum EffectsPredMode : int {effects_pred_eq0=0,effects_pred_ne0=1,effects_pred_off=2};
inline const char* effects_pred_mode_name(int m) noexcept {static constexpr const char* n[]={"eq0","ne0","off"};return m>=0&&m<3?n[m]:"?";}
// File text: optional whitespace, eq0 / ne0 / off (case-insensitive), optional whitespace. -1 = invalid.
inline int parse_effects_pred_mode(const char* t,size_t n) noexcept {
    auto ws=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n'||c==0;};
    size_t i=0;while(i<n&&ws(t[i]))++i;
    size_t e=n;while(e>i&&ws(t[e-1]))--e;
    if(e-i!=3)return -1;
    const char a=char(t[i]|0x20),b=char(t[i+1]|0x20),c=char(t[i+2]|0x20);
    if(a=='e'&&b=='q'&&c=='0')return effects_pred_eq0;
    if(a=='n'&&b=='e'&&c=='0')return effects_pred_ne0;
    if(a=='o'&&b=='f'&&c=='f')return effects_pred_off;
    return -1;
}
// What a D3D12 GPU does with one predicated draw: it executes when the predication is NOT enabled.
inline bool effects_pred_executes(int mode,uint64_t word) noexcept {
    if(mode==effects_pred_off)return true;
    const bool enabled=mode==effects_pred_ne0?word!=0:word==0; // enabled => the draw is discarded
    return !enabled;
}
// Record-time class gate. A candidate is replayed only when its PIXEL SHADER prefix (first 8 hex digits of the creation hash) is on the allowlist AND its
// state is the effects class seen live (blend on, a real GREATER/GREATER_EQUAL depth test, no depth write). The HUD sprite class (ps d34ee153,
// depth ALWAYS) is never replayed. Built-in table + optional file %LOCALAPPDATA%\acz-vr\effects-ps.txt (8-hex prefixes separated by whitespace/commas,
// '#' starts a comment) that can only ADD entries.
constexpr unsigned effects_allow_cap=16,effects_ps_table_cap=24;
constexpr uint32_t effects_ps_deny=0xd34ee153u;
constexpr uint32_t effects_ps_builtin[]={0xd2464ed2u,0x4caf6e2du}; // missile smoke (VS 9eff1f92); sprites (VS 534f29ca, depth GREATER_EQUAL)
struct EffectsAllow {
    uint32_t ps[effects_allow_cap]{};unsigned count=0;
    bool has(uint32_t p) const noexcept {for(unsigned i=0;i<count;++i)if(ps[i]==p)return true;return false;}
    bool add(uint32_t p) noexcept {if(has(p))return true;if(count>=effects_allow_cap)return false;ps[count++]=p;return true;}
    static EffectsAllow defaults() noexcept {EffectsAllow a;for(uint32_t p:effects_ps_builtin)a.add(p);return a;}
};
// Returns the number of NEW entries; `bad` counts malformed tokens (not exactly 8 hex digits) and entries over the cap.
inline unsigned parse_effects_ps_text(const char* t,size_t n,EffectsAllow& a,unsigned& bad) noexcept {
    unsigned added=0;bad=0;size_t i=0;
    auto sep=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n'||c==','||c==';'||c==0;};
    while(i<n) {
        if(sep(t[i])){++i;continue;}
        if(t[i]=='#'){while(i<n&&t[i]!='\n')++i;continue;}
        size_t j=i;uint32_t v=0;unsigned digits=0;bool ok=true;
        while(j<n&&!sep(t[j])) {
            const char c=t[j];
            const int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
            if(d<0)ok=false;else{v=(v<<4)|uint32_t(d);++digits;}
            ++j;
        }
        if(!ok||digits!=8)++bad;
        else if(!a.has(v)){if(a.add(v))++added;else ++bad;}
        i=j;
    }
    return added;
}
// EFFECTS-OUTSIDE (A), docs/world-render/effects-outside-window.md: the GAME's cloud billboards. Evidence (step69 producer PNGs + phase-observe dumps):
// the quiet-flight effects image holds only clouds and one or two small specks; every frame has exactly two allowlisted 4caf6e2d draws, a large
// slowly varying one (19..99 indices, PSOs with depth GREATER_EQUAL) and a small flickering one (4..24 indices, depth GREATER). The slowly varying
// GREATER_EQUAL family is the cloud layer [inference, strong]; the GREATER family (specks, combat sprites) is kept. With our own environment clouds
// (xr_terrain_stage cloud:1 / env:1) the game's clouds would draw a second cloud layer only inside the game window (headset test 17, D-330), so the
// option gamecloud:0 (inside xr_terrain_stage; the default when our clouds are active) stops replaying that class into the effects image.
constexpr uint32_t effects_ps_cloud=0x4caf6e2du;
inline bool effects_is_game_cloud(uint32_t ps8,const step3::PsoOutput& p) noexcept {
    return ps8==effects_ps_cloud&&p.known&&p.depth_enable&&p.depth_func==step3::cmp::greater_equal;
}
// The process-wide switch effects_drop_game_clouds_flag() lives in effects_game_clouds.h (the XR proxy writes it, the producer reads it).
struct EffectsClassIn {const EffectsAllow* allow=nullptr;uint32_t ps8=0;bool writable=false;bool dual=false;bool dropClouds=false;
    bool explosion=false;uint32_t vs8=0;bool tracer=false;bool additive=false;}; // additive: (P) fxadd:1 (effects_additive_flag) admits a PSO with additive_ready; explosion: fxmem:5 (effects_explosion_flag) admits the explosion PS 542a11d7 (extra7_p) with its own clone (dual only); tracer: fxmem:6 (effects_sprite_gpu_flag) admits the gun tracer draw (PS d34ee153 + VS vs8 = 534f29ca + its exact blend, below) // writable: the PSO root signature exposes UAVs
// The PS-prefix candidate table: every draw that reached the class gate (writes colour; blended or really depth-tested), allowlisted or not, so the next
// live run can extend the allowlist from evidence. Bounded; prefixes over the cap are only counted.
struct EffectsPsEntry {uint32_t ps8=0,vs8=0,depthFunc=0;uint64_t seen=0,replayed=0;bool allowed=false,blend=false,depthEnable=false,depthWrite=false,indexed=false;};
struct EffectsPsTable {
    EffectsPsEntry e[effects_ps_table_cap]{};unsigned count=0;uint64_t overflow=0;
    EffectsPsEntry* touch(uint32_t ps8) noexcept {
        for(unsigned i=0;i<count;++i)if(e[i].ps8==ps8)return &e[i];
        if(count>=effects_ps_table_cap){++overflow;return nullptr;}
        e[count].ps8=ps8;return &e[count++];
    }
};
enum EffectsKind : int {effects_ok=0,effects_ignored=1,effects_refused=2};
struct EffectsVerdict {int kind=effects_ok;const char* why=nullptr;int category=hud_state;bool depth_tested=false;};
// D3D12_BLEND values that read the constant blend factor / alpha factor (not tracked by the proxy).
inline bool effects_blend_constant(uint32_t f) noexcept {return f==14||f==15||f==20||f==21;}
// J-effects-dual: the game's blend tuple (D3D12_BLEND 5/6 = SRC_ALPHA/INV_SRC_ALPHA, 18/19 = SRC1_ALPHA/INV_SRC1_ALPHA, op 1 = ADD, write mask 15)
// for render target 0. Output 0 = (S,a), output 1 alpha = b: RGB' = a*S+(1-a)*RGB, A' = a*b+(1-b)*A (the game's alpha, NOT coverage).
inline bool effects_dual_blend_tuple(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask) noexcept {
    return src==5&&dst==6&&op==1&&srcA==18&&dstA==19&&opA==1&&mask==15;
}
// The private clone changes ONLY the alpha factors: ONE / INV_SRC_ALPHA (D3D12_BLEND 2 / 6), ADD. Its alpha is A' = a + (1-a)*A = RGB coverage.
constexpr uint32_t effects_dual_clone_src_alpha=2,effects_dual_clone_dst_alpha=6;
// EFFECTS-OUTSIDE (E) fxmem:5 (docs/world-render/effects-outside-window.md section 10): the EXPLOSION pipeline (PS 542a11d7 = extra7_p, VS 534f29ca =
// ps2AtestCA_v; D-370) blends RT0 with 5,5,1 / 18,19,1: RGB' = SRC_ALPHA*S + SRC_ALPHA*D, A' = SRC1_ALPHA*Sa + INV_SRC1_ALPHA*A. extra7_p (dxc DXIL) writes
// o0 = (t.rgb*a, 1+a) and o1 = (0,0,0, a/(1+a)) with t = tex*COLOR0, a = t.a (discard a < 1/255): RGB' = (1+a)(a t.rgb + D) = an ADDITIVE glow a(1+a)t plus a
// (1+a) brightening of what is behind; A' = a + A/(1+a). In the coverage_v1 effects image (premultiplied RGB + coverage, composited e + (1-A) world) the
// glow is representable, the background brightening of the WORLD is not (lost: the explosion looks a little less bright over terrain): the clone keeps the
// RGB factors (the (1+a) still scales the effects already in the image, as in the game) and sets the alpha factors to ZERO / ONE (A' = A: light adds no
// coverage, the world stays visible behind the glow).
constexpr uint32_t effects_ps_explosion=0x542a11d7u;
inline bool effects_explosion_blend_tuple(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask) noexcept {
    return src==5&&dst==5&&op==1&&srcA==18&&dstA==19&&opA==1&&mask==15;
}
constexpr uint32_t effects_explosion_clone_src_alpha=1,effects_explosion_clone_dst_alpha=2; // D3D12_BLEND_ZERO / ONE
// EFFECTS-OUTSIDE (I) fxmem:6, per-eye explosion ONLY (docs section 14): a SECOND private PSO of the explosion tuple, the "light" PSO, bound by
// step5RecordSprite for the per-eye sprite images and never by the window effects replay. RGB factors unchanged (RGB' = (1+a)(a t + P), as the game);
// alpha SrcBlendAlpha = SRC1_ALPHA (o1.a = a/(1+a)), DestBlendAlpha = SRC_ALPHA (o0.a = 1+a), BlendOpAlpha = REV_SUBTRACT (dest - src):
// A' = A (1+a) - (1+a) a/(1+a) = A (1+a) - a, i.e. the transmittance T = 1 - A becomes T (1+a): the game's (1+a) brightening of what is behind the fireball,
// carried to the eye composite (sprite_over: out = P + T x under). A goes NEGATIVE (T > 1): only a float image and the sprite composite can hold it (the
// window mode-4 composite treats A < 0 as 0 and un-premultiplies: it must keep the shared clone above).
constexpr uint32_t effects_explosion_light_src_alpha=18,effects_explosion_light_dst_alpha=5,effects_explosion_light_op_alpha=3; // SRC1_ALPHA / SRC_ALPHA / REV_SUBTRACT
// The light PSO exists only for the exact explosion tuple (the same proof as its shared clone).
inline bool effects_explosion_light_factors(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask,uint32_t& outSrcA,
                                            uint32_t& outDstA,uint32_t& outOpA) noexcept {
    if(!effects_explosion_blend_tuple(src,dst,op,srcA,dstA,opA,mask))return false;
    outSrcA=effects_explosion_light_src_alpha;outDstA=effects_explosion_light_dst_alpha;outOpA=effects_explosion_light_op_alpha;return true;
}
// EFFECTS-OUTSIDE (G) fxmem:6: the gun tracer draw (batch G, notes step83-verify-run1): RGB SRC_ALPHA/ONE/ADD (D3D12_BLEND 5/2/1), alpha ZERO/ONE/ADD (1/2/1), full mask. Its alpha
// factors already leave the destination alpha untouched (A' = A: light adds no coverage, exactly the explosion clone's rule), so the private clone keeps the game's
// factors (the dual-source trick does not apply: no SRC1); the clone exists so the replay uses a PSO the proxy created and the class gate has one proof (tracer_ready).
constexpr uint32_t effects_ps_tracer=0xd34ee153u,effects_vs_sprite=0x534f29cau; // = effects_ps_deny / the generic ps2_v sprite VS (ps2AtestCA_v)
inline bool effects_tracer_blend_tuple(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask) noexcept {
    return src==5&&dst==2&&op==1&&srcA==1&&dstA==2&&opA==1&&mask==15;
}
// EFFECTS-OUTSIDE (P) fxadd:1 (docs section 21): the ADDITIVE 4caf6e2d family (sparks, glows, fire; quads, lists and one-vertex point lists): RGB SRC_ALPHA/ONE/ADD
// (5/2/1), alpha SRC1_ALPHA/INV_SRC1_ALPHA/ADD (18/19/1), full mask. Without a clone it is refused as dual-blend-unsupported (drawn nowhere in the eye: 741,906
// refusals in the step143 headset session). The clone keeps the RGB factors and sets alpha ZERO / ONE (A' = A: additive light adds no coverage, the explosion and
// tracer rule). The PSO then carries additive_ready (NOT dual_ready): the class gate admits it only while fxadd is on, so the default stays byte-identical.
inline bool effects_additive_blend_tuple(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask) noexcept {
    return src==5&&dst==2&&op==1&&srcA==18&&dstA==19&&opA==1&&mask==15;
}
constexpr uint32_t effects_additive_clone_src_alpha=1,effects_additive_clone_dst_alpha=2; // D3D12_BLEND_ZERO / ONE
// (P2, step162 null run: hard-edged cloud rectangles near the horizon in a pov:3 cinematic) The WINDOW effects image is coverage_v1 and its consumer composite
// (xr_stereo_blit.hlsl effects_over mode 4) un-premultiplies P / A: additive LIGHT carries no coverage, so where it lands on a faint coverage card (a dual cloud
// card's corners, A ~ 0.05) P / A saturates and the card's whole quad comes out as a flat bright rectangle; where A = 0 the light is dropped. The window image
// cannot hold additive light, so the SHARED clone of the additive tuple (the one the window replay binds) draws NOTHING: RGB ZERO / ONE and alpha ZERO / ONE
// (result = destination, the pre-fxadd look: drawn nowhere in the window). The per-eye sprite images (premultiplied light + coverage, composited without
// un-premultiplying: fx_sprite_gpu.h sprite_over) get a second private PSO, the additive "light" PSO: RGB the game's 5,2,1, alpha ZERO / ONE.
constexpr uint32_t effects_additive_window_src=1,effects_additive_window_dst=2; // D3D12_BLEND_ZERO / ONE for RGB in the shared (window) clone
// (P2) per-eye gate: nullptr = fine (not additive, or its light PSO exists); else the refusal (the draw falls back to the window, where its clone draws nothing).
inline const char* effects_additive_pso_why(bool additive_ready,bool light) noexcept {return additive_ready&&!light?"sprite-additive-pso-missing":nullptr;}
inline bool effects_additive_light_factors(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask,uint32_t& outSrcA,
                                           uint32_t& outDstA) noexcept {
    if(!effects_additive_blend_tuple(src,dst,op,srcA,dstA,opA,mask))return false;
    outSrcA=effects_additive_clone_src_alpha;outDstA=effects_additive_clone_dst_alpha;return true;
}
// (P) ADDITIVE SHOT: one eye-dump request (effects_additive_eye_request) on the first published frame whose per-eye sprite replay held an additive draw, and at most
// one more >= kAddShotGapMs later (a frame with point lists preferred for the second: it waits for one). Pure; the producer owns the state.
constexpr unsigned kAddShots=2;constexpr uint64_t kAddShotGapMs=20000;
// (P) the `<prefix>-additive` status line (fxadd:1 only): admitted = additive draws the effects layer accepted (window or per eye), points = the point lists
// among them, per-eye = additive draws replayed per eye (both eyes from one copy), per-eye-points = point lists among those, frames = frames with any per-eye
// additive replay, shots = eye dumps requested.
inline void format_additive_line(char* b,size_t n,const char* prefix,uint64_t admitted,uint64_t points,uint64_t perEye,uint64_t perEyePoints,uint64_t frames,
                                 unsigned shots) noexcept {
    if(!b||!n)return;
    std::snprintf(b,n,"%s-additive;on=1;tuple=5,2,1/18,19,1;admitted=%llu;points=%llu;per-eye=%llu;per-eye-points=%llu;frames=%llu;shots=%u",prefix?prefix:"",
        static_cast<unsigned long long>(admitted),static_cast<unsigned long long>(points),static_cast<unsigned long long>(perEye),
        static_cast<unsigned long long>(perEyePoints),static_cast<unsigned long long>(frames),shots);
}
struct AdditiveShot {
    unsigned shots=0;uint64_t lastMs=0;
    bool step(unsigned additivePerEye,unsigned pointsPerEye,uint64_t nowMs) noexcept {
        if(!additivePerEye||shots>=kAddShots)return false;
        if(shots&&(nowMs-lastMs<kAddShotGapMs||!pointsPerEye))return false;
        ++shots;lastMs=nowMs;return true;
    }
};
// EFFECTS-OUTSIDE (fxmem:5/6) sprite class of one effects draw (gpu_copy_step5_fxsprite.inl step5FxSpriteClass; docs section 15): -1 = not captured,
// 0 = the explosion PS 542a11d7, 1 = the GREATER 4caf6e2d ps2AtestCA sprites, 2 = the gun tracer (fxmem:6 only, PS d34ee153 + VS 534f29ca + tracer_ready).
// (J, headset 2026-10-29 items 5/7) Class 1 used to require an INDEXED draw. The game also draws 4caf6e2d GREATER as NON-indexed 4-vertex strip quads
// (user log headset-2026-10-29-session: effects-replay `ps8=4caf6e2d;depth=greater;indexed=0;count=4`, 5 per frame from the first m02a frame; the
// candidates table alternates `:idx:` / `:vtx:`; ~19,000 of the first 49,167 accepted 4caf6e2d draws were not class-1 reads). Those draws were therefore
// never re-projected per eye: they stayed in the WINDOW effects image, composited only inside the window rectangle (item 5: a cloud patch with a
// rectangular edge) and, with the terrain renderer, UNDER every replayed object (item 7: vehicles drawn through the explosion's smoke/debris). With the
// per-eye GPU replay on (`gpu_on`, fxmem:6) a non-indexed class-1 draw is now captured like the tracer's first-bullet quad (count 4..512, a multiple of 4);
// without it (fxmem:5 dry run) the old rule is kept exactly (indexed only).
inline int effects_sprite_class(uint32_t ps8,uint32_t vs8,bool indexed,uint32_t depth_func,bool tracer_ready,bool gpu_on) noexcept {
    if(ps8==effects_ps_explosion)return 0;
    if(ps8==effects_ps_cloud&&depth_func==step3::cmp::greater&&(indexed||gpu_on))return 1;
    if(ps8==effects_ps_tracer&&vs8==effects_vs_sprite&&tracer_ready&&gpu_on)return 2;
    return -1;
}
// The classes whose NON-indexed draws are captured and replayed per eye (DrawInstanced from the eye copy; fx_sprite_gpu.h GateIn::allow_nonindexed).
inline bool effects_sprite_nonindexed_class(int c) noexcept {return c==1||c==2;}
// w12-2 finding 1: the quad collapse / synthetic restarts only mean something for the proved 4-vertex TRIANGLESTRIP quad (D3D topology 5). A non-indexed
// TRIANGLELIST (4) or any other topology draws different triangles, so such a draw is refused (fail closed) instead of being read as strip quads.
inline bool effects_sprite_nonindexed_topology_ok(uint32_t topology) noexcept {return topology==5;}
// EFFECTS-OUTSIDE (K, headset 2026-10-30 m03k; docs section 17): the shape rule of one NON-indexed sprite-class draw, before any read. nullptr = captured;
// `quads` = the 4-vertex strip-quad list is synthesised (0..n-1 in groups of 4 with 0xFFFF restarts: collapse / widen / quad shapes per quad), false = ONE
// run 0..n-1 without restarts (the plan re-projects every vertex and collapses the WHOLE run when any vertex is unusable: fx_sprite_replay.h collapse_quads).
// The user's m03k flight refused 20,672 class-1 non-indexed draws as `sprite-nonindexed-shape` (27 per frame in the dense frames, the same seconds as the
// fallbacks): with the effects layer accepting them they reached the GPU gate unprepared (`sprite-not-prepared`) and were drawn in the WINDOW image only.
//   class 2 (tracer): exactly as before: TRIANGLESTRIP, count a multiple of 4 (the widen rule is per quad).
//   class 1 (GREATER 4caf6e2d): TRIANGLESTRIP with any count >= 3 (a multiple of 4 keeps the quad list), or TRIANGLELIST with count a multiple of 3 (ONE
//   run: the GPU draws the eye copy under the game's own topology, so the triangles are the game's; only the collapse needs the whole-run rule).
// Every case needs one instance and 1 <= count <= max_vertices (the eye copy).
//   (P, docs section 21) class 1 POINTLIST (D3D topology 1; no GS exists for 4caf6e2d: each vertex is ONE pixel): captured as ONE run only while `points` (fxadd:1)
//   is set; `is_points` then tells the plan to cull a behind-eye point (fx_sprite_replay.h cull_points) instead of collapsing the run. Off: refused by name as before.
inline const char* effects_sprite_nonindexed_shape(int cls,uint32_t count,uint32_t instances,uint32_t topology,uint32_t max_vertices,bool& quads,bool points=false,
                                                   bool* is_points=nullptr) noexcept {
    quads=false;if(is_points)*is_points=false;
    if(!count||count>max_vertices||instances!=1)return "sprite-nonindexed-shape";
    if(cls!=1) {   // the tracer (class 2): the pre-(K) rule, shape before topology
        if(count%4!=0)return "sprite-nonindexed-shape";
        if(!effects_sprite_nonindexed_topology_ok(topology))return "sprite-nonindexed-topology";
        quads=true;return nullptr;
    }
    if(topology==5){if(count<3)return "sprite-nonindexed-shape";quads=count==4;return nullptr;}   // W15: only ONE 4-vertex strip is a quad; a longer strip is one continuous GPU assembly (no restarts exist on the GPU)
    if(topology==4){if(count%3!=0)return "sprite-nonindexed-shape";return nullptr;}
    if(topology==1&&points){if(is_points)*is_points=true;return nullptr;}   // (P) one run of single-pixel points
    return "sprite-nonindexed-topology";
}
// The alpha factors of the private coverage clone for a game blend tuple (false = no clone: the candidate stays refused as dual-blend-unsupported).
inline bool effects_clone_alpha_factors(uint32_t src,uint32_t dst,uint32_t op,uint32_t srcA,uint32_t dstA,uint32_t opA,uint32_t mask,uint32_t& outSrcA,uint32_t& outDstA) noexcept {
    if(effects_dual_blend_tuple(src,dst,op,srcA,dstA,opA,mask)){outSrcA=effects_dual_clone_src_alpha;outDstA=effects_dual_clone_dst_alpha;return true;}
    if(effects_explosion_blend_tuple(src,dst,op,srcA,dstA,opA,mask)){outSrcA=effects_explosion_clone_src_alpha;outDstA=effects_explosion_clone_dst_alpha;return true;}
    if(effects_tracer_blend_tuple(src,dst,op,srcA,dstA,opA,mask)){outSrcA=srcA;outDstA=dstA;return true;}   // (G) tracer: the game's factors, unchanged
    if(effects_additive_blend_tuple(src,dst,op,srcA,dstA,opA,mask)){outSrcA=effects_additive_clone_src_alpha;outDstA=effects_additive_clone_dst_alpha;return true;} // (P) fxadd
    return false;
}
// Representation tag published with the effects image (StereoFrame::effects_repr): 0 = legacy (game alpha / colour heuristic), 1 = coverage_v1
// (FP16 premultiplied RGB + RGB coverage A = 1-product(1-a)). A consumer must never apply coverage maths to a legacy image.
constexpr uint32_t effects_repr_legacy=0,effects_repr_coverage_v1=1;
inline bool effects_uses_constant_blend(const step3::PsoOutput& p) noexcept {return p.blend0&&p.blend_constant;}
// `phase`: where the frame is (open = after the first cockpit member and before the scene target left RENDER_TARGET);
// `draws`: speculative replays already recorded this frame. Rules, first failing wins:
//   ignored : phase not open (nothing is recorded before the first member / after the scene barrier);
//   ignored : writes no colour channel (alpha-only / mask 0), then neither blended nor really depth-tested (GREATER/GREATER_EQUAL; an opaque,
//             always-pass draw is not an effect);
//   refused : everything hud_decide refuses (tracking, root, PSO, OM, scene target, format, stencil EQUAL/non-ALWAYS, depth tests
//             other than GREATER/GREATER_EQUAL/ALWAYS/NEVER, descriptor proof), then
//   refused : the constant blend factor / alpha factor (the proxy does not track OMSetBlendFactor; counted separately),
//   refused : a depth WRITE or a stencil write (the effects target shares the HUD depth-stencil; a writer would change what the
//             HUD and marker draws test against), then
//   refused : the per-frame cap (64).
inline EffectsVerdict effects_decide(const step3::Shape& s,const HudTarget& t,int phase,unsigned draws,const EffectsClassIn* cls=nullptr) noexcept {
    EffectsVerdict v;
    auto ignore=[&](const char* why){v.kind=effects_ignored;v.why=why;return v;};
    auto refuse=[&](const char* why,int cat){v.kind=effects_refused;v.why=why;v.category=cat;return v;};
    if(phase==effects_before_first_member)return ignore("phase-before-first-member");
    if(phase!=effects_open)return ignore("phase-scene-closed");
    // J-effects-content: (1) a draw that writes NO colour channel (render-target write mask without R, G and B: the live game has a full-screen
    // 4-vertex alpha-only pass, mask 0x8, blend off, depth ALWAYS, in every between-members phase) can only perturb the effects target's alpha
    // (the consumer composites with that alpha: e + (1-a)*world), never show anything: ignored. (2) "depth-tested" means a REAL test (GREATER /
    // GREATER_EQUAL); an enabled ALWAYS/NEVER test with blending off is an opaque always-pass draw, not an effect (was: any enabled depth test).
    if(s.pso.known&&(s.pso.write_mask0&7u)==0)return ignore("no-colour-write");
    if(s.pso.known&&!s.pso.blend0&&!(s.pso.depth_enable&&(s.pso.depth_func==step3::cmp::greater||s.pso.depth_func==step3::cmp::greater_equal)))
        return ignore("not-effect-like");
    // J-effects-smoke-fix: the record-time class gate (nullptr = no gate: the first version's rules). Never the HUD sprite class; only a blended,
    // really depth-tested, non-depth-writing draw whose pixel shader prefix is on the allowlist.
    const bool tracer=cls&&cls->tracer&&cls->dual&&cls->ps8==effects_ps_tracer&&cls->vs8==effects_vs_sprite&&s.pso.known&&s.pso.tracer_ready;
    if(cls&&cls->allow&&s.pso.known) {
        // EFFECTS-OUTSIDE (G) fxmem:6: the GUN TRACER (live batch G: PS d34ee153 = the HUD pixel shader, VS 534f29ca, blend 5,2,1/1,2,1 additive, depth GREATER no write,
        // strip) is the one d34ee153 draw admitted: only with the exact tuple (s.pso.tracer_ready, which also implies the private clone), the sprite vertex shader and
        // the coverage key. Every other d34ee153 draw (the HUD sprites: depth ALWAYS, other blends) stays denied.
        if(cls->ps8==effects_ps_deny&&!tracer)return ignore("class-hud-sprite");
        if(!(s.pso.blend0&&s.pso.depth_enable&&(s.pso.depth_func==step3::cmp::greater||s.pso.depth_func==step3::cmp::greater_equal)&&!s.pso.depth_write))
            return ignore("class-state-not-effect");
        // fxmem:5: the explosion PS is admitted only with the coverage (dual) path, whose clone handles its 5,5,1/18,19,1 tuple.
        const bool explosion=cls->explosion&&cls->dual&&cls->ps8==effects_ps_explosion;
        if(!explosion&&!tracer&&!cls->allow->has(cls->ps8))return ignore(cls->explosion&&cls->ps8==effects_ps_explosion?"class-explosion-needs-dual":"class-ps-not-allowlisted");
        if(cls->dropClouds&&effects_is_game_cloud(cls->ps8,s.pso))return ignore("class-game-cloud"); // EFFECTS-OUTSIDE gamecloud:0
    }
    const HudVerdict h=hud_decide(s,t);
    if(h.why)return refuse(h.why,h.category);
    // Astra P1-3: the replay re-issues the draw under the game's own PSO and root arguments; a shader that can write a game-owned UAV would write it twice.
    // No positive side-effect proof exists for any shader, so a root signature that exposes UAVs (root UAV or a descriptor-table UAV range) is refused.
    if(cls&&cls->writable)return refuse("shader-writable-binding",hud_state);
    v.depth_tested=h.depth_tested;
    if(effects_uses_constant_blend(s.pso))return refuse("blend-constant-factor",hud_state);
    // J-effects-dual: under the key a candidate must be the exact dual-source tuple WITH its private clone; every other blend state (additive,
    // blend off, other factors, unknown, clone creation failed) would put game alpha into a coverage image: refused, fail closed.
    // (G) a PSO with the plain additive tracer tuple has a clone only for the tracer class: for any other candidate it is as unsupported as before.
    // (P) fxadd:1: an additive_ready PSO (dual_ready false) counts as ready only with the option; without it the verdict is the pre-(P) one, byte for byte.
    const bool additive=cls&&cls->additive&&s.pso.known&&s.pso.additive_ready;
    if(cls&&cls->dual&&(!(s.pso.dual_ready||additive)||(s.pso.tracer_ready&&!tracer)))return refuse("dual-blend-unsupported",hud_state);
    if(s.pso.depth_enable&&s.pso.depth_write)return refuse("depth-write",hud_state);
    if(s.pso.stencil&&s.pso.stencil_write!=0&&!s.pso.stencil_ops_keep)return refuse("stencil-write",hud_state);
    if(draws>=effects_draw_cap)return refuse("effects-cap",hud_cap);
    return v;
}
// Epoch bookkeeping of one frame (pure; the hook adapter owns the predicate memory). `open_draw` is called for every candidate
// that is replayed, `note_refused` for a refused candidate, `commit` when a later cockpit member is recorded.
struct EffectsEpochs {
    int cur=-1;                       // predicate index of the open epoch (-1: no candidate since the last member)
    unsigned used=0;                  // predicate indices allocated this frame
    unsigned curDraws=0,curRefused=0,curConst=0;unsigned curReason[hud_categories]{};
    unsigned draws=0,refused=0,constBlend=0,commits=0;unsigned reason[hud_categories]{};  // committed epochs
    unsigned tailDraws=0,tailRefused=0;                                                    // set by finish()
    unsigned speculative=0;                                                                // every replay recorded
    // The predicate index a replayed candidate uses, or -1 when no predicate slot is left (the draw must then be refused).
    int open_draw() noexcept {
        if(cur<0){if(used>=effects_epoch_cap)return -1;cur=int(used++);}
        ++curDraws;++speculative;return cur;
    }
    void note_refused(int category,bool constant) noexcept {
        ++curRefused;if(category>=0&&category<hud_categories)++curReason[category];if(constant)++curConst;
    }
    // A later member: the open epoch is BETWEEN members. Returns its predicate index (write 1), or -1 when nothing was recorded.
    int commit() noexcept {
        const int ret=curDraws?cur:-1;
        if(curDraws||curRefused){draws+=curDraws;refused+=curRefused;constBlend+=curConst;for(int i=0;i<hud_categories;++i)reason[i]+=curReason[i];++commits;}
        cur=-1;curDraws=curRefused=curConst=0;for(auto& r:curReason)r=0;
        return ret;
    }
    // The close: whatever is still open came after the last member (the tail: predicated off, covered by the HUD).
    void finish() noexcept {tailDraws=curDraws;tailRefused=curRefused;}
};
// The frame's effects layer is published when at least one committed (between-members) draw was replayed. Refusals are counted
// and published but do not invalidate the layer: a missing effect is not an inconsistent picture.
inline bool effects_frame_valid(bool armed,unsigned committedDraws) noexcept {return armed&&committedDraws>0;}
// Memory: one more colour target per slot (same format/size as the HUD/marker targets; the HUD depth-stencil is shared) and one
// 64 KiB UPLOAD buffer for the predicates. Charged like step3::finalize (64 KiB aligned resource + one 64 KiB descriptor heap per
// target); refused above the caps.
inline const char* effects_budget(uint32_t format,uint64_t width,uint32_t height,unsigned slot_count,uint64_t& charged) noexcept {
    charged=0;
    step3::ColorFormat c;
    if(!step3::color_format(format,c))return "effects-format-unsupported";
    if(!width||!height||width>step3::max_dimension||height>step3::max_dimension)return "effects-size-unsupported";
    const uint64_t bytes=width*height*c.bytes;
    if(bytes>effects_slot_cap)return "effects-over-slot-cap";
    const uint64_t total=(step3::align_up(bytes,step3::charge_alignment)+step3::charge_alignment)*slot_count+step3::charge_alignment; // + predicate buffer
    if(total>effects_total_cap)return "effects-over-total-cap";
    charged=total;return nullptr;
}
// ---------------------------------------------------------------- membership by class (J-cockpit-fov F3)
// gpu_copy_step5_members_by_class: a frame starts at the first DrawCPT draw of a qualified list
// generation whose signature class is ADMITTED and whose per-member state holds (any ordinal), and
// every later DrawCPT draw of that generation is a member under the same rules. The member cap
// and the per-slot upload are raised for this mode (fixed when the ring is created):
//   128 members x 2 eyes x 512 B = 128 KiB upload per slot (384 KiB for 3 slots);
//   commands <= 1100 (4 clears + 5 world + 8 x 128 = 1033).
// Key off keeps 64 members / 600 commands / 64 KiB exactly.
constexpr unsigned class_member_cap=128,class_command_cap=1100;
constexpr uint64_t class_slot_upload_bytes=uint64_t(class_member_cap)*2*step3::blob_stride;
static_assert(4+step3::set_per_member*class_member_cap+5<=class_command_cap,"a full class frame fits the command cap");
// Per-member refusal categories of the by-class mode (counted, logged in the -members status line).
enum MemberCategory : int {member_state=0,member_descriptor=1,member_format=2,member_cap=3,member_class=4,member_camera=5,member_unreadable=6,member_categories=7};
inline const char* member_category_name(int c) noexcept {
    static constexpr const char* n[]={"state","descriptor","format","cap","class","camera","unreadable"};return c>=0&&c<member_categories?n[c]:"?";
}
inline bool out_of_class(const char* why) noexcept {return why&&std::strncmp(why,"out-of-class-",13)==0;}
// A draw into ANOTHER target (viewport or output formats differ from the cockpit ring): not part of
// this frame's cockpit image, so (like out-of-class) it is refused without making the frame partial
// and never leaves a pending skip at the frame start.
inline bool other_target(const char* why) noexcept {
    return why&&(std::strcmp(why,"viewport-size-changed")==0||std::strcmp(why,"rt-format-changed")==0||std::strcmp(why,"ds-format-changed")==0);
}
inline int member_category(const char* why) noexcept {
    if(!why)return member_state;
    auto is=[&](const char* s){return std::strcmp(why,s)==0;};
    auto starts=[&](const char* s){return std::strncmp(why,s,std::strlen(s))==0;};
    if(out_of_class(why)||is("signature-class-differs")||is("pso-not-in-alternate-set")||is("alternate-set-full"))return member_class;
    if(starts("camera-"))return member_camera;
    if(is("live-srt-unreadable"))return member_unreadable;
    if(is("member-cap")||is("command-count-over-cap"))return member_cap;
    if(is("binding-tracking-incomplete")||is("om-descriptor-unproved")||starts("t10-"))return member_descriptor;
    if(is("msaa-unsupported")||is("rt-format-unsupported")||is("ds-format-unsupported")||is("rt-format-changed")||
       is("ds-format-changed")||is("rt-over-cap")||is("ds-over-cap"))return member_format;
    return member_state;
}
// Every member's own camera (VP = inverse(M256) * M64 of ITS SrtData) must be the frame's camera
// (recovered from the first member): with membership by class the first member may be a draw that
// was never replayed before, so its camera is checked against every later member's instead of
// being assumed. Tolerances: eye within 1e-4 x (1 + |eye|) world units, axes within 1e-5 of a unit
// dot product (~0.26 degrees), lens tangents within 1e-4 relative. nullptr or why.
inline const char* camera_differs(const uint8_t* raw,const PoseFrame& f,Scratch& sc) noexcept {
    load16(raw+64,sc.m64);load16(raw+256,sc.m256);
    for(int i=0;i<16;++i)if(!std::isfinite(sc.m64[i])||!std::isfinite(sc.m256[i]))return "camera-srt-nonfinite";
    if(sc.m256[3]!=0||sc.m256[7]!=0||sc.m256[11]!=0||sc.m256[15]!=1)return "camera-m256-not-affine";
    std::memcpy(sc.a,sc.m256,sizeof sc.a);if(!inv44(sc.a,sc.inv))return "camera-singular";
    mul44(sc.inv,sc.m64,sc.d64);
    Camera m;if(!decompose(sc.d64,m))return "camera-degenerate";
    const Camera& c=f.cam;
    const double dx=m.eye[0]-c.eye[0],dy=m.eye[1]-c.eye[1],dz=m.eye[2]-c.eye[2];
    const double mag=std::sqrt(dot3(c.eye,c.eye));
    if(!(std::sqrt(dx*dx+dy*dy+dz*dz)<=1e-4*(1.0+mag)))return "camera-eye-differs";
    if(!(dot3(m.right,c.right)>=1-1e-5)||!(dot3(m.up,c.up)>=1-1e-5)||!(dot3(m.fwd,c.fwd)>=1-1e-5))return "camera-axes-differ";
    if(!(std::fabs(m.tanX-c.tanX)<=1e-4*c.tanX)||!(std::fabs(m.tanY-c.tanY)<=1e-4*c.tanY))return "camera-lens-differs";
    return nullptr;
}
// J-world-hold (diagnostics only; nothing here changes a decision). The world snapshot refusal "entry-state-unknown"
// is split into the exact failing sub-condition of step3Capture (Shape::world_entry_sub; nullptr = capture incomplete),
// and head/game look pitch proxies are accumulated for accepted and refused frames so a live run can correlate them.
constexpr unsigned entry_subs=11;
inline const char* entry_sub_name(unsigned i) noexcept {
    static constexpr const char* n[entry_subs]={"no-transition-tracked","recording-missing","generation-zero","generation-mismatch","watch-missing",
        "serial-missing","resource-mismatch","watch-epoch-mismatch","serial-mismatch","unclassified","capture-incomplete"};
    return i<entry_subs?n[i]:"?";
}
inline unsigned entry_sub_index(const char* why) noexcept {
    if(!why)return entry_subs-1; // capture-incomplete
    for(unsigned i=0;i<entry_subs;++i)if(std::strcmp(why,entry_sub_name(i))==0)return i;
    return 9; // unclassified
}
// Pitch of the head: the elevation of the eye's -Z axis (OpenXR LOCAL +Y up). The recentring neutral is a position and a heading
// (yaw) only, so this equals the pitch relative to neutral. false for an unsound quaternion.
inline bool head_pitch_deg(const float q[4],double& deg) noexcept {
    double R[9];deg=0;
    if(!q||!quat_rot(q,R))return false;
    double y=-R[5];y=y>1.0?1.0:(y<-1.0?-1.0:y);
    deg=std::asin(y)*57.29577951308232;
    return std::isfinite(deg);
}
// min/max accumulators: head pitch (degrees) and the recovered game camera forward axes (world units, unit vector).
struct PitchStat {
    uint64_t n=0,nHead=0,nCam=0;double headMin=0,headMax=0,fwdMin[3]{},fwdMax[3]{};
    void add(bool head,double headDeg,bool cam,const double fwd[3]) noexcept {
        ++n;
        if(head&&std::isfinite(headDeg)){if(!nHead||headDeg<headMin)headMin=headDeg;if(!nHead||headDeg>headMax)headMax=headDeg;++nHead;}
        if(cam&&fwd&&std::isfinite(fwd[0])&&std::isfinite(fwd[1])&&std::isfinite(fwd[2])) {
            for(int k=0;k<3;++k){if(!nCam||fwd[k]<fwdMin[k])fwdMin[k]=fwd[k];if(!nCam||fwd[k]>fwdMax[k])fwdMax[k]=fwd[k];}
            ++nCam;
        }
    }
};
// One refusal record for the per-event detail line (first refusals and the first of each burst).
struct RefusalDetail {
    uint64_t frame=0,ordinal=0,generation=0;unsigned sub=0;bool entry=false;const char* why=nullptr;
    bool head=false,cam=false;double headDeg=0,fwd[3]{};
};
constexpr unsigned refusal_detail_cap=24;
}  // namespace acz::gpu_copy::step5
