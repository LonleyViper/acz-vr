#pragma once
// J-world-objects: SDK-free pose maths and record checks for the 3D-path WORLD OBJECT draws
// (DrawACM_v, docs/interception/stored-draw-manifest-2.md) replayed per eye by the step-5 producer.
// Default OFF (gpu_copy_step5_world_objects). Shared by gpu_copy_step5_hooks.inl and the offline fixtures.
//
// What the evidence says (docs/interception/acm-shader-dataflow.md = DXIL-exact; manifest 2 + survey, capture 11906; NOT measured live):
//  * Same per-draw 416-byte SrtData record layout as the cockpit, read as root SRV t10. The DrawACM_v VS reads
//    +0 A (world -> PS2 screen), +256 B (model -> world), +360 the depth scale and +400 = (512,-448,2048,2048);
//    it forms h = (local,1) x B x A, w = h.w (A's w column composed through B), ndc.x = (h.x/w-2048)*2/512,
//    ndc.y = (h.y/w-2048)*2/-448, depth = (h.z/w) * rec[+360] * 2^-24. There is NO M256 factor on A.
//  * +64 (C) is NOT used for position, depth or w. Its only reader is the cb10 flag bit 8 (value 8) fade branch, which no
//    ACM draw of the capture sets (flags 258, 2 or 7). In the 17 pass-6 ACM records +64..+127 holds non-matrix data
//    (NaN included) while A is bit-identical to pass 0's. So this replay NEVER depends on C: it is neither validated
//    nor used, and +64 is left untouched in the substituted record (it is not read by the VS for these draws).
//  * Measured (the 23 pass-0/6 records, capture dump only): A bit-identical for all 23; C bit-identical only over the 6 pass-0
//    records. (The earlier "A and C identical across all 34" was wrong: the 34 include 11 offscreen-pass records.)
//  * The camera is recovered from A alone, in ABSOLUTE world units (the camera sits at ~(-14360, 3492, 123218), next to the aircraft;
//    the cockpit's matrices are camera-relative), by inverting the analytic PS2 screen map x_s = 258.048 x_c + 2048 w,
//    y_s = 225.792 y_c + 2048 w (columns 0 and 1), leaving column 3 (w) and column 2 untouched:
//        Vp = [ (A0 - 2048 A3)/258.048 | (A1 - 2048 A3)/225.792 | A2 | A3 ]   (pseudo clip matrix, no inverse(C))
//    and decomposing Vp exactly like the cockpit's VP. The eye solves e x M = -b on A's columns 0, 1, 3 (Cramer); float32 quantisation of
//    the 1.6e8 translation entries limits it to ~0.015 units (measured against the C-based eye of draw 3); the orientation and lens
//    agree with the cockpit camera to ~4e-7 (A against the cockpit VP_screen: 4.1e-8).
//  * Depth: A's z column is Z0 e3 + b A3 with b = -255.0078 (residual 1.7e-8 relative), i.e. zs = Z0 + b w: a function of w only.
// Pose substitution for such a record (row vectors, as everywhere in step 5):
//   VP_ref = build_vp(camera from A)      VP_e = build_vp(eye_world(e), ...) (eye_world = camera_eye + units_per_metre x head offset
//            in the camera's right/up/forward axes; the recentring neutral (p0, yaw0) is the cockpit's, shared)
//   D_e    = VP_e - VP_ref
//   S      = analytic clip -> screen map:  x_s = 258.048 x_c + 2048 w, y_s = 225.792 y_c + 2048 w, z_s = b w (D's z-clip column is
//            IGNORED: the direct z-column formulation), w_s = w.  (inverse(C) x A is ill-conditioned, cond ~ 1.6e10, and is not used.)
//   A'_e   = A + D_e x S   (B, C and every other byte untouched). The VS result is then the exact Translate/rotate of the camera
//            applied to the world: validated against Translate(-delta) x A to 1e-6 NDC (acm-shader-dataflow.md 4.1).
// A zero head offset gives D_e == 0 exactly and the raw bytes (also -0.0), as for the cockpit.
#include "gpu_copy_step5.h"
#include "world_dedupe.h"   // WORLD-4 propdedupe (world-dedupe-prop: drawn by the props pass)
#include "tactical_producer.h"   // TACTICAL (tac:1): per-draw object facts for the consumer's tactical display
#include "fx_smoke.h"            // EFFECTS-OUTSIDE (C) fxmem:1: missile-smoke record capture + fx-smoke-frame line (header only; must precede the hooks' anonymous namespace)
#include "fx_census.h"           // EFFECTS-OUTSIDE (D) fxmem:3: draw census (header only; must precede the hooks' anonymous namespace)
#include "fx_smoke_gpu.h"        // EFFECTS-OUTSIDE (C) fxmem:4: per-eye GPU smoke replay policy (header only; same rule)
#include "fx_sprite_replay.h"    // EFFECTS-OUTSIDE (E) fxmem:5: explosion sprite re-projection (header only; same rule)
#include "fx_sprite_gpu.h"       // EFFECTS-OUTSIDE (F) fxmem:6: per-eye GPU sprite replay policy (header only; same rule)
#include "fx_puff_trail.h"       // EFFECTS-OUTSIDE (M) fxtrail: world-space smoke-puff memory (header only; same rule)
#include "fx_diag_flare_air.h"  // READ-ONLY flare / aircraft diagnostics (header only; must precede the hooks' anonymous namespace: namespace acz::fxdiag)
#include "pov_render.h"          // POV-RENDER stage 2 (pov:2): no-cockpit chase frames, the producer rig (header only; same rule)
#include <cmath>
#include <cstdint>
#include <cstring>

namespace acz::gpu_copy::step5::world {
// World draws recorded per frame on top of the cockpit members (they share the member and command caps).
// 96 (was 48; J-test8-fixes): the live observer saw up to 41 scene-RT DrawACM_v draws in one list (25-52 draws, 14-41 of them on the scene RT),
// and test 8 (cap 48) dropped about 2.5 of 32 recorded+refused draws per frame (refused=cap:30999 of 442k) = pop-in. Memory/commands checked
// (step5.h static_asserts): 96 world + cockpit_reserve cockpit members <= class_member_cap 128; 4 + 8 x 128 <= class_command_cap 1100; the
// per-slot UPLOAD is already sized for class_member_cap members, so nothing grows. A world draw is refused as `world-cap` also when fewer than
// cockpit_reserve member slots remain, so an early world burst can never starve the cockpit members that follow it.
constexpr unsigned draw_cap=96;
constexpr unsigned cockpit_reserve=16;
static_assert(draw_cap+cockpit_reserve<=class_member_cap,"world draws plus the cockpit reserve fit the by-class member cap");
static_assert(draw_cap==::acz::tac::kObjCap,"TACTICAL: one object entry per recorded world draw");
// The +400 constants of every DrawACM_v record seen (identical to the ps2_v constants).
constexpr float screen_params[4]={512.0f,-448.0f,2048.0f,2048.0f};
// The PS2 screen map the VS inverts (x_s = 258.048 x_c + 2048 w, y_s = 225.792 y_c + 2048 w; = acz::PS2_SCALE_X/Y/CENTRE, game scale 1.008 x 256/224).
constexpr double ps2_scale_x=258.048,ps2_scale_y=225.792,ps2_centre=2048.0;
// Tolerances: A of a later record against the frame's first record (A only: the VS reads nothing else for position/depth/w), and the
// world camera against the cockpit camera. Measured: A against the cockpit VP_screen 4.1e-8; A-derived orientation/lens vs the cockpit
// ~4e-7 (float32 rounding of A: 1.6e8 translation entries are quantised at 16). axes: dot >= 1-1e-5 (~0.26 deg), lens 1e-4 relative.
// The A-derived EYE differs from the C-derived one by 0.015 units (measured, draw 3) and is never compared with the cockpit's
// (camera-relative vs absolute); the substitution depends only on the DIFFERENCE of two eyes built from it. depth_col_rel_tol: the z
// column of A must equal b x (w column) on the xyz rows to this relative norm (measured 1.7e-8), else the depth model is wrong.
constexpr double vp_rel_tol=1e-6,axes_dot_tol=1e-5,lens_rel_tol=1e-4,fov_abs_tol=1e-3,depth_col_rel_tol=1e-4;
// Record depth scale (+360): the stored ACM records hold 1.0 (the cockpit's 16.0 is never read by DrawCPT; for DrawACM_v 16.0 would select
// the 2^-24 scale AND a clamp to [-1,1] for w>0). Only 1.0 is replayed.
constexpr float depth_scale_ok=1.0f;
// J-test8-fixes, key xr_world_objects_depth_fix (default OFF): the REPLAYED copy of the record carries +360 = depth_fix_scale instead of 1.0, so the VS
// writes d = (h.z/w) * 2^-10 * 2^-24 (clip z = h.z * scale: the near/far clip signs are unchanged, only the magnitude shrinks). Both the ACM objects
// (d = 1/w - 255*2^-24) and the cockpit (saturate(sz*2^-20) = 1/w - 15.9*2^-20, the same function of view depth) are reversed-Z with GREATER tests;
// an object nearer than a cockpit part (smaller w) would win the shared eye depth. After the compression every object depth is below
// 2^-10 * 1/w_min, i.e. below any cockpit depth for w_min >= ~1 unit, while staying > 0 for every unclipped fragment, so the consumer's coverage rule
// (alpha = depth > 0) is unchanged and the ordering among the objects is preserved (uniform positive factor). Needs a FLOAT depth format
// (D32_FLOAT / D32_FLOAT_S8X24): a UNORM depth would quantise 1e-8 to 0 and drop the objects' coverage.
constexpr float depth_fix_scale=0.0009765625f; // 2^-10, exact in binary32
inline bool depth_fix_applicable(uint32_t ds_format) noexcept {return ds_format==step3::dxgi::d32||ds_format==step3::dxgi::d32_s8x24;}
inline float depth_fix_scale_for(bool key,uint32_t ds_format) noexcept {return key&&depth_fix_applicable(ds_format)?depth_fix_scale:depth_scale_ok;}
// cb10 dword 0 (root constants of the draw): bit value 8 selects the fade branch, the ONLY reader of +64. The replay never reads C,
// so a draw with this bit would be rendered from data the substitution leaves stale. flags_check is the pure rule (stored flags:
// 258, 2, 7; none has bit 8). J-world-evidence (Astra reply 5): it is ENFORCED live on every ACM acceptance path (member, early-open)
// through cb10_check: the draw's dword 0 is resolved from the root-constant witness (SetGraphicsRoot32BitConstant(s) at the cb10
// parameter; needs diagnostic_ownership_pins); an unknown / unsupported value REFUSES the draw (category cb10).
constexpr uint32_t fade_flag=8;
inline const char* flags_check(uint32_t cb10_dword0) noexcept {return (cb10_dword0&fade_flag)?"world-flags-fade-reads-c":nullptr;}
// What the live resolver knows about the draw's cb10 root constants (reason = diagnostics::RootConstantWitness::Reason, 0 = covered).
struct Cb10 {
    bool resolved=false;          // the resolver ran for this draw
    bool known=false;             // dword 0 is a witnessed value for THIS draw
    uint32_t value=0;             // dword 0 (valid only when known)
    unsigned reason=0;            // witness reason; meaningful when !known
    uint32_t mask=0,words[4]{};   // written-word mask and the first four words as the witness holds them (diagnostic)
    unsigned size=0,parameter=UINT32_MAX,stages=0;
    uint64_t root=0,token=0,revision=0;
    const char* note="";          // static string: why the resolver could not even try (e.g. ownership pins off)
};
// Provenance of the bound root signature for the cb10 witness (pure; J-world-evidence run H). The witness is tied to the root signature BOUND
// in the recording (its layout resolves cbv b10 to a vertex+pixel constants parameter); the draw's PSO must not contradict it. Inputs are the
// facts the live resolver has: `listOk` (listPointFailure_locked == nullptr), `tracking` (bindings.tracking_complete), `psoRecord` (the PSO
// record exists), `pipelineSame` (bindings.pipeline.identity == record identity), the bound root's identity and the PSO record's root
// identity (0 = the creation did not establish one). Returns the failing sub-condition (static text, logged as `note=`) or nullptr; `inferred`
// is set when the only gap is an UNKNOWN PSO root identity: D3D12 requires the PSO's root signature to match the bound one for the draw
// to be valid, so the bound root's layout is used (documented as an inference, never when the PSO's root is KNOWN and different).
inline const char* cb10_provenance(const char* listFailure,bool tracking,bool psoRecord,bool pipelineSame,uint64_t boundRoot,uint64_t psoRoot,bool& inferred) noexcept {
    inferred=false;
    if(listFailure)return listFailure;
    if(!tracking)return "tracking-incomplete";
    if(!psoRecord)return "no-pipeline-record";
    if(!pipelineSame)return "pipeline-identity-differs";
    if(!boundRoot)return "bound-root-identity-unknown";
    if(psoRoot&&psoRoot!=boundRoot)return "pso-root-differs-from-bound-root";
    inferred=!psoRoot;
    return nullptr;
}
// nullptr = accepted; else the refusal reason. Unknown is never accepted (the fade branch could read the stale C).
inline const char* cb10_check(const Cb10& c) noexcept {
    if(!c.resolved)return "world-cb10-unresolved";
    if(!c.known)return "world-cb10-unknown";
    return flags_check(c.value);
}

// Refusal categories (counted, logged in the -world status line). rt-count and cb10 were appended by J-world-evidence.
enum Category : int {cat_state=0,cat_class=1,cat_record=2,cat_camera=3,cat_pose=4,cat_unreadable=5,cat_cap=6,cat_target=7,cat_rt_count=8,cat_cb10=9,categories=10};
inline const char* category_name(int c) noexcept {
    static constexpr const char* n[]={"state","class","record","camera","pose","unreadable","cap","target","rt-count","cb10"};return c>=0&&c<categories?n[c]:"?";
}
// A PSO that writes other than exactly one render target (decide() rejects it; the eye replay binds ONE target). Checked BEFORE the scene
// target rule so a two-target ACM draw is counted as rt-count and not hidden behind world-offscreen-om-count.
inline const char* rt_count_why(const step3::Shape& live) noexcept {
    if(!live.pso.known)return nullptr;   // unknown PSO output: decide() reports it
    if(live.pso.rt_count==0)return "world-rt-count-zero";
    if(live.pso.rt_count!=1)return "world-rt-count-unsupported";
    return nullptr;
}
inline int category(const char* why) noexcept {
    if(!why)return cat_state;
    auto starts=[&](const char* s){return std::strncmp(why,s,std::strlen(s))==0;};
    if(starts("world-offscreen-"))return cat_target;
    if(starts("world-dedupe-"))return cat_target;   // WORLD-4: the props pass draws this building: out of scope, never a loss
    if(starts("world-class-"))return cat_class;
    if(starts("world-camera-")||starts("world-vp-")||starts("world-fov-"))return cat_camera;
    if(starts("world-pose-"))return cat_pose;
    if(std::strcmp(why,"live-srt-unreadable")==0)return cat_unreadable;
    if(starts("world-rt-count-")||std::strcmp(why,"multiple-render-targets-unsupported")==0||std::strcmp(why,"no-render-target")==0||
       std::strcmp(why,"om-rt-count-below-pso")==0)return cat_rt_count;
    if(starts("world-cb10-")||starts("world-flags-"))return cat_cb10;
    if(starts("world-record-")||starts("world-model-")||starts("world-screen-"))return cat_record;
    if(std::strcmp(why,"world-cap")==0||std::strcmp(why,"member-cap")==0||std::strcmp(why,"command-count-over-cap")==0)return cat_cap;
    return cat_state;
}
// J-180 (Codex review P2, "world-cap omissions publish as a complete frame"): a world draw that is REFUSED is either out of scope (it does not
// write the producer's scene render target: offscreen passes, multi-target draws, ...) or the LOSS of an eligible scene draw (world-cap, unreadable
// record, record / camera / pose / cb10 / class / state refusal). Only losses make the published frame world-incomplete.
// `ready` = the producer knows its scene RT; without it a draw cannot be proven out of scope and is counted conservatively.
inline const char* scene_target_why(const step3::Shape& live,bool ready,uintptr_t source,uint64_t w,uint32_t h,uint32_t format) noexcept; // defined below
inline bool scene_draw(const step3::Shape& live,bool ready,uintptr_t source,uint64_t w,uint32_t h,uint32_t format) noexcept {
    if(!ready)return true;
    return scene_target_why(live,true,source,w,h,format)==nullptr;
}
inline bool loss_eligible(const char* why,const step3::Shape& live,bool ready,uintptr_t source,uint64_t w,uint32_t h,uint32_t format) noexcept {
    if(!why)return false;
    if(category(why)==cat_target)return false;           // offscreen / not the scene RT: out of scope
    if(other_target(why))return false;                    // another viewport / output format than the cockpit ring: another pass (same rule as the cockpit members)
    return scene_draw(live,ready,source,w,h,format);      // a draw that does not write the scene RT is out of scope whatever the refusal said
}
// J-180 (Codex review P2, depth fix): xr_world_objects_depth_fix rewrites the record's depth scale (+360) to s = 2^-10. The VS writes clip z = h.z*s*2^-24 and
// clip w = h.w, so the REPLAYED clip volume is 0 <= z <= w with a different z: the near plane (z >= 0) is unchanged but the plane z <= w moves.
// The evidence (acm-shader-dataflow.md; A's z column = Z0 e3 + b A3, so h.z = Z0 + b w with Z0 = 2^24 (1) and b = -255) makes the clip decision a function of
// w ALONE: with c = s*2^-24 the plane holds for w >= Z0 c / (1 - b c). The fix therefore ADMITS geometry with w in [lo, hi) that the original clips,
// lo = threshold(depth_fix_scale), hi = threshold(1.0) (about 0.001 .. 1.0 view units for the measured records): objects closer than ~1 unit to the camera.
// This cannot be removed by a record-only change (the replay runs the game's own VS and PSO), so the key is a DIAGNOSTIC APPROXIMATION: it is
// default OFF, the band is computed per frame and logged, and clip_changes() is the pure oracle the fixtures use on both sides of z = w.
constexpr double depth_unit=1.0/16777216.0; // 2^-24
// Clip z of a vertex with view depth w for a record with scale `scale` (A = Z0 e3 + b A3 model), and the D3D clip test 0 <= z <= w (w > 0).
inline double clip_z(double z0,double b,double w,double scale) noexcept {return (z0+b*w)*scale*depth_unit;}
inline bool clip_inside(double z0,double b,double w,double scale) noexcept {const double z=clip_z(z0,b,w,scale);return w>0&&z>=0&&z<=w;}
// true: the same vertex is inside the volume with the depth fix and outside without it (or the other way round): the clip criterion changed.
inline bool clip_changes(double z0,double b,double w,double scale_fixed) noexcept {
    return clip_inside(z0,b,w,1.0)!=clip_inside(z0,b,w,scale_fixed);
}
struct ClipBand {bool valid=false;double lo=0,hi=0;};
inline ClipBand depth_fix_clip_band(double z0,double b,double scale_fixed) noexcept {
    ClipBand r;
    if(!(z0>0)||!(b<0)||!(scale_fixed>0)||!(scale_fixed<1))return r; // the model (Z0 > 0, b < 0) is what the evidence measured; anything else: no claim
    auto threshold=[&](double s){const double c=s*depth_unit;return z0*c/(1.0-b*c);};
    r.lo=threshold(scale_fixed);r.hi=threshold(1.0);r.valid=r.lo<r.hi;return r;
}
// The per-frame world state: the camera recovered from the frame's first accepted record and the
// reference A bytes (+0..+63) every later record must equal.
struct Frame {
    PoseFrame pf;uint8_t ref[64]{};bool camera=false;
    double z0=0,b=0;      // J-180: the depth model of A (zs = Z0 + b w) for the clip-band diagnostic
    const char* failed=nullptr;   // why this frame's world setup failed (later draws of the frame fail fast)
    double eyeAbs[3]{};   // the game camera in absolute world units (diagnostic)
    double model[3]{};    // the first record's model translation (diagnostic)
};
inline float rf(const uint8_t* p,unsigned i) noexcept {float f;std::memcpy(&f,p+4*size_t(i),4);return f;}
// Layout validation of one record: exactly what the DrawACM_v VS reads for position/depth/w. +64..+127 (C) is deliberately NOT looked at.
// nullptr or why.
inline const char* record_check(const uint8_t* raw) noexcept {
    for(unsigned i=0;i<16;++i){const float f=rf(raw,i);if(!std::isfinite(f)||std::fabs(f)>1e12f)return "world-record-nonfinite";}  // A: +0..+63
    for(unsigned i=64;i<80;++i)if(!std::isfinite(rf(raw,i)))return "world-record-nonfinite";                                     // B: +256..+319
    if(rf(raw,64+3)!=0.0f||rf(raw,64+7)!=0.0f||rf(raw,64+11)!=0.0f||rf(raw,64+15)!=1.0f)return "world-model-not-affine";
    for(unsigned i=0;i<4;++i)if(rf(raw,100+i)!=screen_params[i])return "world-screen-params";       // +400..+415
    if(!(rf(raw,90)==depth_scale_ok))return "world-record-depth-scale";                             // +360 (NaN refused too)
    return nullptr;
}
// The world camera against the cockpit camera: the same orientation and lens (the eyes differ: absolute vs camera-relative).
inline const char* camera_matches_cockpit(const Camera& c,const Camera& cockpit) noexcept {
    if(!(dot3(c.right,cockpit.right)>=1-axes_dot_tol)||!(dot3(c.up,cockpit.up)>=1-axes_dot_tol)||!(dot3(c.fwd,cockpit.fwd)>=1-axes_dot_tol))
        return "world-camera-axes-differ-from-cockpit";
    if(!(std::fabs(c.tanX-cockpit.tanX)<=lens_rel_tol*cockpit.tanX)||!(std::fabs(c.tanY-cockpit.tanY)<=lens_rel_tol*cockpit.tanY))
        return "world-camera-lens-differs-from-cockpit";
    return nullptr;
}
// The draw's render target against the producer's scene RT (the world snapshot source): a world draw is replayed only when
// it writes THAT target, with its size and format; any other target (128x128 offscreen passes, ...) is never replayed and
// never opens a frame. `ready`: the producer knows its scene RT (else nothing can be proven and nullptr is NOT returned
// by require_known callers; see `scene_target_why`). Pure.
inline const char* scene_target_why(const step3::Shape& live,bool ready,uintptr_t source,uint64_t w,uint32_t h,uint32_t format) noexcept {
    if(!ready||!source)return "world-offscreen-no-scene-source";
    if(live.om_count!=1)return "world-offscreen-om-count";
    if(std::strcmp(live.game_rt_status,"resolved")!=0)return "world-offscreen-rt-unresolved";
    if(!live.game_rt||live.game_rt!=source)return "world-offscreen-target";
    if(live.game_rt_width!=w||live.game_rt_height!=h||live.game_rt_format!=format)return "world-offscreen-desc";
    return nullptr;
}
// First accepted record of a frame: recover the world camera from A ALONE (never C, never inverse(C)), build the analytic clip -> screen
// map S, check the camera is the cockpit's orientation and lens (cockpit != nullptr), keep A as the reference.
inline const char* camera_setup(const uint8_t* raw,const Camera* cockpit,Frame& w,Scratch& sc) noexcept {
    w.camera=false;w.pf=PoseFrame{};
    load16(raw,sc.m0);
    for(int i=0;i<16;++i)if(!std::isfinite(sc.m0[i]))return "world-camera-a-nonfinite";
    // Pseudo clip matrix: undo the PS2 screen map on columns 0 and 1 (row r: A[r][0] = kx X[r] + 2048 W[r]); columns 2 and 3 as stored.
    double* vp=w.pf.vp;
    for(int r=0;r<4;++r) {
        vp[r*4+0]=(sc.m0[r*4+0]-ps2_centre*sc.m0[r*4+3])/ps2_scale_x;
        vp[r*4+1]=(sc.m0[r*4+1]-ps2_centre*sc.m0[r*4+3])/ps2_scale_y;
        vp[r*4+2]=sc.m0[r*4+2];vp[r*4+3]=sc.m0[r*4+3];
    }
    if(!decompose(vp,w.pf.cam))return "world-camera-degenerate";
    const Camera& c=w.pf.cam;
    if(!(c.tanX>0)||!(c.tanY>0)||c.tanX>100||c.tanY>100)return "world-camera-lens-out-of-range";
    // Depth: the z column must be b x (w column) on the xyz rows (b = c.A); the translation row's remainder is Z0 (a constant).
    {double res=0,mag=0;for(int k=0;k<3;++k){const double z=vp[k*4+2],r=z-c.A*vp[k*4+3];res+=r*r;mag+=z*z;}
     if(!(std::sqrt(res)<=depth_col_rel_tol*std::sqrt(mag)))return "world-camera-depth-column";}
    // S: x_s = kx x_c + 2048 w, y_s = ky y_c + 2048 w, z_s = b w, w_s = w (D's z-clip column is ignored: S row 2 is zero).
    w.b=c.A;w.z0=vp[12+2]-c.A*vp[12+3]; // J-180: translation row's z entry minus b x its w entry = Z0
    for(int i=0;i<16;++i)w.pf.S[i]=0.0;
    w.pf.S[0]=ps2_scale_x;w.pf.S[5]=ps2_scale_y;w.pf.S[12]=ps2_centre;w.pf.S[13]=ps2_centre;w.pf.S[14]=c.A;w.pf.S[15]=1.0;
    if(cockpit)if(const char* why=camera_matches_cockpit(c,*cockpit))return why;
    std::memcpy(w.ref,raw,64);                                           // A (+0..+63)
    for(int k=0;k<3;++k){w.eyeAbs[k]=c.eye[k];w.model[k]=double(rf(raw,64+12+unsigned(k)));}
    w.camera=true;w.pf.valid=false;w.pf.why="camera";
    return nullptr;
}
// A later record of the frame: its A must equal the reference (the evidence: bit-identical for the 23 pass-0/6 records). C is not compared.
inline const char* record_matches(const uint8_t* raw,const Frame& w) noexcept {
    for(unsigned i=0;i<16;++i) {
        const double a=rf(raw,i),b=rf(w.ref,i);
        if(!(std::fabs(a-b)<=vp_rel_tol*std::fmax(std::fabs(b),1.0)))return "world-vp-differs";
    }
    return nullptr;
}
// POV-RENDER stage 2 (pov:2, other-povs.md section 10): a record whose A has the reference's rotation/lens rows (rows 0..2, within vp_rel_tol) but
// its own translation row is the SAME camera with the model translated: A = T(p) x A_ref (row vectors, T(p) = [[I,0],[p,1]]). The POV1 chase census
// (draw all=34: rows 0..2 bit-identical to the absolute camera's draw all=19, row 3 = (172032, 184232, 96573408, 84.0)) is the player's aircraft
// drawn camera-relative. p is solved on columns 0, 1, 3 (Cramer) and column 2 must agree. nullptr or why.
inline const char* relative_offset(const uint8_t* raw,const Frame& w,double p[3]) noexcept {
    if(!w.camera)return "world-relative-no-camera";
    double R[3][4],d[4];
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c) {
        const double a=rf(raw,r*4+c),b=rf(w.ref,r*4+c);
        if(!(std::fabs(a-b)<=vp_rel_tol*std::fmax(std::fabs(b),1.0)))return "world-vp-differs";
        R[r][c]=b;
    }
    for(unsigned c=0;c<4;++c){d[c]=double(rf(raw,12+c))-double(rf(w.ref,12+c));if(!std::isfinite(d[c]))return "world-relative-nonfinite";}
    const int cols[3]={0,1,3};
    double M[3][3];for(int i=0;i<3;++i)for(int j=0;j<3;++j)M[i][j]=R[j][cols[i]];   // equation i (column cols[i]): sum_j p_j R[j][col] = d[col]
    auto det3=[](const double (&m)[3][3]){return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])-m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])+m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);};
    const double det=det3(M);
    if(!(std::fabs(det)>0)||!std::isfinite(det))return "world-relative-singular";
    for(int j=0;j<3;++j){double T[3][3];for(int i=0;i<3;++i)for(int k=0;k<3;++k)T[i][k]=k==j?d[cols[i]]:M[i][k];p[j]=det3(T)/det;}
    const double z=p[0]*R[0][2]+p[1]*R[1][2]+p[2]*R[2][2];
    if(!(std::fabs(z-d[2])<=1e-5*std::fmax(std::fabs(d[2]),1.0)+64.0))return "world-relative-depth-column";   // float32 rows of ~2.5e8: ulp 16..32
    for(int j=0;j<3;++j)if(!std::isfinite(p[j]))return "world-relative-nonfinite";
    return nullptr;
}
// out = raw with +0 -> A + T(p) x (D_e x S): rows 0..2 as substitute(); row 3 gains p x (D_e x S)[rows 0..2] (A'_rel = T(p) x A'_ref).
inline const char* substitute_relative(const uint8_t* raw,const PoseFrame& w,unsigned eye,const double p[3],uint8_t* out,float depth_scale=1.0f) noexcept {
    if(!w.valid||eye>1)return "world-pose-unavailable";
    double ds[16];
    for(int i=0;i<16;++i){double v=0;for(int k=0;k<4;++k)v+=w.D[eye][(i/4)*4+k]*w.S[k*4+(i%4)];ds[i]=v;}
    std::memcpy(out,raw,size_t(step3::blob_bytes));
    for(int i=0;i<16;++i) {
        double d0=ds[i];
        if(i>=12)d0+=p[0]*ds[i-12]+p[1]*ds[i-8]+p[2]*ds[i-4];
        float r0;std::memcpy(&r0,raw+4*i,4);
        const float n0=d0==0.0?r0:float(double(r0)+d0);
        if(!std::isfinite(n0))return "world-pose-substituted-nonfinite";
        std::memcpy(out+4*i,&n0,4);
    }
    if(depth_scale!=depth_scale_ok)std::memcpy(out+360,&depth_scale,4);
    return nullptr;
}
// The eyes must have been built with the SAME frustum as the cockpit's (published once per frame as
// StereoFrame::cockpit_fov): the world frame's hmd flag and tangents equal the cockpit frame's.
inline const char* fov_matches(const PoseFrame& world,const PoseFrame& cockpit) noexcept {
    if(world.fovHmd!=cockpit.fovHmd)return "world-fov-mode-differs-from-cockpit";
    if(!world.fovHmd)return nullptr; // both on the game lens: lens equality was checked in camera_setup
    for(int e=0;e<2;++e)for(int i=0;i<4;++i)
        if(!(std::fabs(world.fov[e].t[i]-cockpit.fov[e].t[i])<=fov_abs_tol))return "world-fov-differs-from-cockpit";
    return nullptr;
}
// out = raw with +0 -> A + D_e x S; every other byte copied unchanged (B and +64 (C) included: C is not read by the VS for these draws).
// depth_scale != 1.0 (J-test8-fixes, xr_world_objects_depth_fix): +360 of the REPLAYED copy is set to it (see depth_fix_scale); 1.0 = the raw bytes.
inline const char* substitute(const uint8_t* raw,const PoseFrame& w,unsigned eye,uint8_t* out,float depth_scale=1.0f) noexcept {
    if(!w.valid||eye>1)return "world-pose-unavailable";
    std::memcpy(out,raw,size_t(step3::blob_bytes));
    for(int i=0;i<16;++i) {
        double d0=0;for(int k=0;k<4;++k)d0+=w.D[eye][(i/4)*4+k]*w.S[k*4+(i%4)];
        float r0;std::memcpy(&r0,raw+4*i,4);
        const float n0=d0==0.0?r0:float(double(r0)+d0); // zero delta keeps the raw bits
        if(!std::isfinite(n0))return "world-pose-substituted-nonfinite";
        std::memcpy(out+4*i,&n0,4);
    }
    if(depth_scale!=depth_scale_ok)std::memcpy(out+360,&depth_scale,4); // the record's depth scale/flag (+360); never 16.0 (clamp), see record_check
    return nullptr;
}
}  // namespace acz::gpu_copy::step5::world
