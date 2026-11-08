// xr_look_drive_keys offline fixtures: controller, look measurement, double-rotation fix, sweep, injector contract and config keys,
// against fakes and a simulated game look filter. Nothing here sends a key, touches the game, the GPU or XR.
#include "look_drive_keys.h"
#include "look_drive_injector.h"
#include "gpu_copy_step5.h"
#include "runtime_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace acz::look_drive;

static int g_failures=0,g_checks=0;
#define CHECK(c) do{++g_checks;if(!(c)){++g_failures;std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);}}while(0)
static const double kDeg=kPi/180.0;
static bool verbose(){static const bool v=std::getenv("LOOK_DRIVE_VERBOSE")!=nullptr;return v;}

// ------------------------------------------------------------------ fakes
struct FakeInjector final:KeyInjector {
    bool fg=true,kill=false;uint8_t cur=0;
    unsigned downs=0,ups=0,downsWhileBackground=0,applies=0,releases=0;
    uint8_t last=0;
    bool foreground() noexcept override {return fg;}
    bool kill_switch() noexcept override {return kill;}
    void apply(uint8_t mask,double) noexcept override {
        ++applies;last=mask;
        // Edge semantics as the real injector: nothing is typed into another window; a background apply releases only.
        const uint8_t want=fg?mask:uint8_t(0);
        for(uint8_t b:{key_left,key_right,key_up}) {
            if((cur&b)&&!(want&b)){cur&=uint8_t(~b);++ups;}
            else if(!(cur&b)&&(want&b)){cur|=b;++downs;if(!fg)++downsWhileBackground;}
        }
    }
    bool release_all() noexcept override {if(cur){++releases;cur=0;++ups;}return true;}
    uint8_t down() const noexcept override {return cur;}
};

// ------------------------------------------------------------------ helpers: rotations, fixture camera
struct V3 {double x,y,z;};
static V3 add(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static V3 mulv(V3 a,double s){return {a.x*s,a.y*s,a.z*s};}
static void toArr(V3 a,double* o){o[0]=a.x;o[1]=a.y;o[2]=a.z;}
// Cockpit local axes (rows of the +256 matrix, unit) of an aircraft with heading theta, pitch ap, roll ar (any proper rotation).
static void aircraft_axes(double theta,double ap,double ar,V3 e[3]) {
    const double cy=std::cos(theta),sy=std::sin(theta),cp=std::cos(ap),sp=std::sin(ap),cr=std::cos(ar),sr=std::sin(ar);
    // R = Ry(theta) Rx(ap) Rz(ar) acting on the local axes (columns), rows of the row-vector matrix = images of local x,y,z
    const double Ry[9]={cy,0,sy,0,1,0,-sy,0,cy},Rx[9]={1,0,0,0,cp,-sp,0,sp,cp},Rz[9]={cr,-sr,0,sr,cr,0,0,0,1};
    double A[9],B[9];
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double s=0;for(int k=0;k<3;++k)s+=Ry[i*3+k]*Rx[k*3+j];A[i*3+j]=s;}
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double s=0;for(int k=0;k<3;++k)s+=A[i*3+k]*Rz[k*3+j];B[i*3+j]=s;}
    for(int j=0;j<3;++j)e[j]={B[0*3+j],B[1*3+j],B[2*3+j]};   // image of local axis j (column j of B)
}
// Camera axes in world for a look (yaw,pitch) relative to the cockpit frame: a_j = sum_i e_i R_ij, R = Ry(yaw) Rx(pitch).
static void camera_axes(const V3 e[3],double yaw,double pitch,V3 a[3]) {
    double R[9];look_matrix(yaw,pitch,R);
    for(int j=0;j<3;++j)a[j]=add(add(mulv(e[0],R[0*3+j]),mulv(e[1],R[1*3+j])),mulv(e[2],R[2*3+j]));
}
static void m256_from_axes(const V3 e[3],double scale,V3 t,double* m) {
    std::memset(m,0,16*sizeof(double));
    for(int i=0;i<3;++i){m[i*4+0]=e[i].x*scale;m[i*4+1]=e[i].y*scale;m[i*4+2]=e[i].z*scale;}
    m[12]=t.x;m[13]=t.y;m[14]=t.z;m[15]=1;
}
static bool measure(const V3 e[3],double yaw,double pitch,LookMeasure& out,bool mirror=false) {
    V3 a[3];camera_axes(e,yaw,pitch,a);
    double m[16];V3 ee[3]={e[0],e[1],e[2]};
    if(mirror)ee[0]=mulv(ee[0],-1.0);
    m256_from_axes(ee,16.0,{100,200,300},m);
    double r[3],u[3],f[3];toArr(a[0],r);toArr(a[1],u);toArr(mulv(a[2],-1.0),f);
    return measure_look(r,u,f,m,out);
}

// ------------------------------------------------------------------ config
static void test_config() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.xr_look_drive_keys()&&!Config{}.xr_look_drive_test_sweep());
    CHECK(!parse_config("").config.xr_look_drive_keys());
    CHECK(parse_config("xr_look_drive_keys=true\n").config.xr_look_drive_keys());
    CHECK(parse_config("xr_look_drive_keys=1\n").config.xr_look_drive_keys());
    CHECK(!parse_config("xr_look_drive_keys=false\n").config.xr_look_drive_keys());
    CHECK(!parse_config("xr_look_drive_keys=true\n").config.xr_look_drive_test_sweep());
    CHECK(parse_config("xr_look_drive_test_sweep=1\n").config.xr_look_drive_test_sweep());
    CHECK(!parse_config("xr_look_drive_test_sweep=0\n").config.xr_look_drive_test_sweep());
    for(const char* bad:{"xr_look_drive_keys=yes\n","xr_look_drive_keys=\n","xr_look_drive_test_sweep=2\n","xr_look_drive_test_sweep=maybe\n"})
        CHECK(parse_config(bad).issue==ConfigIssue::out_of_range||parse_config(bad).issue==ConfigIssue::malformed);
    CHECK(parse_config("xr_look_drive_keys=true\nxr_look_drive_test_sweep=true\n").issue==ConfigIssue::none);
    CHECK(parse_config("xr_look_drive_keys=true\nxr_look_drive_keys=true\n").issue==ConfigIssue::duplicate);
    // The cap: 24 headset/test keys + the two new ones fit; the cap itself is 32 (raised from 24).
    CHECK(max_config_keys==32);
    const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=87","gpu_copy_step5_hud_markers=true","xr_hud_markers_layer=true",
        "xr_world_in_projection=true","gpu_copy_step5_world_objects=true","gpu_copy_step5_world_observe=true","xr_world_hold_last=true","xr_world_extend=true",
        "xr_look_drive_keys=true","xr_look_drive_test_sweep=true"};
    std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\n";
    const auto r=parse_config(cfg);
    CHECK(r.issue==ConfigIssue::none&&r.config.xr_look_drive_keys()&&r.config.xr_look_drive_test_sweep()&&r.config.xr_stereo_output());
    CHECK(std::size(keys)==26&&std::size(keys)<=max_config_keys&&cfg.size()<=max_config_bytes);
    // Latch (no-XR test sweep): the real head-driven mode still needs xr_stereo_output; the sweep alone runs without it.
    {const std::string base="log_enabled=true\ngpu_copy_step5_continuous=true\n";
     auto latch=[&](const std::string& extra,bool gate=true){return parse_config(base+extra).config.look_drive_latch(gate);};
     auto l=latch("xr_look_drive_keys=true\n");CHECK(!l.keys&&!l.sweep&&!l.no_xr);                                   // head mode, no XR: refused
     l=latch("xr_look_drive_keys=true\nxr_stereo_output=true\n");CHECK(l.keys&&!l.sweep&&!l.no_xr);                // head mode with XR
     l=latch("xr_look_drive_keys=true\nxr_look_drive_test_sweep=true\n");CHECK(l.keys&&l.sweep&&l.no_xr);           // no-XR sweep
     l=latch("xr_look_drive_keys=true\nxr_look_drive_test_sweep=true\nxr_stereo_output=true\n");CHECK(l.keys&&l.sweep&&!l.no_xr);
     l=latch("xr_look_drive_test_sweep=true\n");CHECK(!l.keys&&!l.sweep&&!l.no_xr);                                 // the sweep needs the key
     l=latch("xr_look_drive_keys=true\nxr_look_drive_test_sweep=true\n",false);CHECK(!l.keys);                      // no opt-in
     l=parse_config("log_enabled=false\ngpu_copy_step5_continuous=true\nxr_look_drive_keys=true\nxr_look_drive_test_sweep=true\n").config.look_drive_latch(true);CHECK(!l.keys); // no logging
     l=parse_config("log_enabled=true\nxr_look_drive_keys=true\nxr_look_drive_test_sweep=true\n").config.look_drive_latch(true);CHECK(!l.keys);          // no continuous producer
    }
    // The no-XR key set of vr-test-start.ps1 -LookSweepNoXr parses and latches.
    {const char* nk[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","gpu_copy_step5_members_by_class=true",
        "xr_look_drive_keys=true","xr_look_drive_test_sweep=true","ee_camera_probe=true"};
     std::string c;for(const char* k:nk)c+=std::string(k)+"\n";
     const auto pr=parse_config(c);CHECK(pr.issue==ConfigIssue::none&&pr.config.look_drive_latch(true).no_xr&&!pr.config.xr_stereo_output());}
    // 33 keys are still refused.
    std::string many;for(size_t i=0;i<max_config_keys;++i)many+="k"+std::to_string(i)+"=1\n";
    CHECK(parse_config(many).unknown_keys==max_config_keys);many+="extra=1\n";CHECK(parse_config(many).issue==ConfigIssue::oversized);
}

// ------------------------------------------------------------------ measured look
static void test_measure() {
    LookMeasure m;
    // The run-G evidence: srt-yaw - srt-air-yaw - pi == L for a yaw-only aircraft (heading -0.7854, camera yaw 2.3562 at L = 0).
    {
        const double theta=-0.785398;V3 e[3];aircraft_axes(theta,0,0,e);
        for(double L:{0.0,0.3,1.187,2.387,2.475,-1.3,-2.48}) {
            CHECK(measure(e,L,0,m));
            CHECK(std::fabs(m.yaw-L)<1e-9&&std::fabs(m.pitch)<1e-9);
            // the producer-side formula used in the evidence logs: atan2(fwd.x,fwd.z) - atan2(air.x,air.z) - pi
            V3 a[3];camera_axes(e,L,0,a);const V3 fwd=mulv(a[2],-1.0);
            const double srtYaw=std::atan2(fwd.x,fwd.z),airYaw=std::atan2(e[2].x,e[2].z);
            CHECK(std::fabs(wrap_pi(srtYaw-airYaw-kPi)-L)<1e-9);
        }
        // identity look
        CHECK(measure(e,0,0,m));
        for(int i=0;i<9;++i)CHECK(std::fabs(m.R[i]-(i%4==0?1.0:0.0))<1e-12);
    }
    // pitch up, and yaw+pitch together; the aircraft attitude (pitch/roll/heading) does not matter
    for(double theta:{0.0,2.0,-2.9})for(double ap:{0.0,0.4,-0.6})for(double ar:{0.0,0.7,-1.2}) {
        V3 e[3];aircraft_axes(theta,ap,ar,e);
        for(double L:{-1.5,0.0,0.9})for(double P:{0.0,0.5,1.4}) {
            CHECK(measure(e,L,P,m));
            CHECK(std::fabs(m.yaw-L)<1e-9&&std::fabs(m.pitch-P)<1e-9);
        }
    }
    // a mirrored cockpit matrix (reflection) is handled: same look
    {V3 e[3];aircraft_axes(1.0,0.1,0.2,e);CHECK(measure(e,0.8,0.3,m,true));CHECK(std::fabs(m.yaw-0.8)<1e-9&&std::fabs(m.pitch-0.3)<1e-9);}
    // invalid inputs fall back (valid=false, a reason)
    {
        double z[16]{};double r[3]{1,0,0},u[3]{0,1,0},f[3]{0,0,-1};
        CHECK(!measure_look(r,u,f,z,m)&&!m.valid&&std::string(m.why)=="cockpit-matrix-degenerate");
        V3 e[3];aircraft_axes(0,0,0,e);double mm[16];m256_from_axes(e,16.0,{0,0,0},mm);
        double zero[3]{0,0,0};CHECK(!measure_look(zero,u,f,mm,m)&&std::string(m.why)=="camera-degenerate");
        double r2[3]{1,0.5,0};CHECK(!measure_look(r2,u,f,mm,m)&&std::string(m.why)=="camera-not-orthonormal");
        double sheared[16];std::memcpy(sheared,mm,sizeof sheared);sheared[1]=0.5*sheared[0];   // row 0 not orthogonal to the others
        CHECK(!measure_look(r,u,f,sheared,m)&&std::string(m.why)=="cockpit-matrix-not-rigid");
        // look out of the game's range (a wrong reference): pitch 1.7 rad
        CHECK(!measure(e,0.0,1.7,m)&&std::string(m.why)=="look-out-of-range");
        CHECK(!measure(e,0.0,-0.8,m)&&!m.valid);
        double nan3[3]{std::nan(""),0,1};CHECK(!measure_look(nan3,u,f,mm,m)&&!m.valid);
    }
    // head angles: the same conventions as the look (yaw > 0 left, pitch > 0 up) and neutral-relative
    {
        double R[9],y,p;
        look_matrix(40*kDeg,0,R);CHECK(head_angles(R,0.0,y,p)&&std::fabs(y-40*kDeg)<1e-12&&std::fabs(p)<1e-12);
        CHECK(head_angles(R,40*kDeg,y,p)&&std::fabs(y)<1e-12);          // yaw0 removed
        CHECK(head_angles(R,-3.0,y,p)&&std::fabs(wrap_pi(y-(40*kDeg+3.0)))<1e-9);
        look_matrix(0,25*kDeg,R);CHECK(head_angles(R,0.0,y,p)&&std::fabs(p-25*kDeg)<1e-12&&std::fabs(y)<1e-12);
        look_matrix(-30*kDeg,-10*kDeg,R);CHECK(head_angles(R,0.0,y,p)&&std::fabs(y+30*kDeg)<1e-12&&std::fabs(p+10*kDeg)<1e-12);
        R[0]=std::nan("");R[2]=std::nan("");CHECK(!head_angles(R,0.0,y,p));
    }
}

// ------------------------------------------------------------------ double rotation fix in setup_poses
namespace s5 = acz::gpu_copy::step5;
static acz::stereo::EyePose eye_pose(double x,double y,double z,double yaw,double pitch) {
    acz::stereo::EyePose p{};p.position[0]=float(x);p.position[1]=float(y);p.position[2]=float(z);
    const double qy[4]={0,std::sin(yaw/2),0,std::cos(yaw/2)},qx[4]={std::sin(pitch/2),0,0,std::cos(pitch/2)};double q[4];quat_mul(qy,qx,q);
    for(int i=0;i<4;++i)p.orientation[i]=float(q[i]);
    p.fov[0]=-0.55f;p.fov[1]=0.55f;p.fov[2]=0.33f;p.fov[3]=-0.33f;return p;
}
// A SrtData blob whose camera looks at the cockpit with the given look: m256 = rigid cockpit matrix (scale 16), m64 = m256 * VP, m0 = m64.
static void make_blob(const V3 e[3],double lookYaw,double lookPitch,uint8_t* blob) {
    std::memset(blob,0,416);
    V3 a[3];camera_axes(e,lookYaw,lookPitch,a);
    double m256[16];m256_from_axes(e,16.0,{1000,2000,3000},m256);
    s5::Camera c{};
    toArr(a[0],c.right);toArr(a[1],c.up);toArr(mulv(a[2],-1.0),c.fwd);
    c.eye[0]=1010;c.eye[1]=2005;c.eye[2]=2970;c.n3=500;c.A=1.0;c.C=-0.1;c.tanX=0.6152;c.tanY=0.3460;
    double vp[16];s5::build_vp(c.eye,c.right,c.up,c.fwd,-c.tanX,c.tanX,c.tanY,-c.tanY,c,vp);
    double m64[16];s5::mul44(m256,vp,m64);
    float f[16];
    for(int i=0;i<16;++i)f[i]=float(m64[i]);std::memcpy(blob+0,f,64);std::memcpy(blob+64,f,64);
    for(int i=0;i<16;++i)f[i]=float(m256[i]);std::memcpy(blob+256,f,64);
}
namespace s3 = acz::gpu_copy::step3;
// Both eyes of a head turned (yaw, pitch) about its centre and leaning by `lean` metres along OpenXR z (positive = backwards).
static void head_poses(double yaw,double pitch,double lean,acz::stereo::EyePose* out) {
    out[0]=eye_pose(-0.032,1.2,0,0,0);out[1]=eye_pose(0.032,1.2,0,0,0);
    apply_sweep_to_poses(out,yaw,pitch);
    for(int e=0;e<2;++e)out[e].position[2]+=float(lean);
}
// Axes (right, up, forward) of the eye matrix as built by setup_poses.
static bool eye_axes(const s5::PoseFrame& f,int eye,s5::Camera& c) {return s5::decompose(f.vpEye[eye],c);}
static double angle_between(const double* a,const double* b){
    const double na=std::sqrt(dot3(a,a)),nb=std::sqrt(dot3(b,b));
    if(!(na>0)||!(nb>0))return 99.0;
    return std::acos(clampd(dot3(a,b)/(na*nb),-1.0,1.0));
}
static void test_double_rotation() {
    V3 e[3];aircraft_axes(-0.785398,0.05,0.1,e);
    const double hy=40*kDeg;
    uint8_t blobLook[416],blobFlat[416];make_blob(e,hy,0,blobLook);make_blob(e,0,0,blobFlat);
    s5::Scratch sc;s5::PoseFrame f;s5::Lens lens;
    acz::stereo::EyePose neutral[2],head[2];head_poses(0,0,0,neutral);head_poses(hy,0,0,head);
    // (a) game look 40 rendered, head 40, compensation ON: the eye axes equal the RENDERED camera (identity residual)
    {
        s5::Neutral n;CHECK(!s5::camera_frame(blobLook,f,sc,lens));
        LookMeasure m;CHECK(measure_look(f.cam.right,f.cam.up,f.cam.fwd,sc.m256,m)&&std::fabs(m.yaw-hy)<1e-5);
        CHECK(!s5::setup_poses(neutral,n,f,sc)&&n.set&&std::fabs(n.yaw0)<1e-6);   // neutral fixed at yaw 0
        CHECK(!s5::setup_poses(head,n,f,sc,false,s3::units_per_metre,m.R));
        for(int ee=0;ee<2;++ee){s5::Camera a;CHECK(eye_axes(f,ee,a));
            CHECK(angle_between(a.fwd,f.cam.fwd)<2e-5&&angle_between(a.right,f.cam.right)<2e-5&&angle_between(a.up,f.cam.up)<2e-5);}
    }
    // (a') the same with compensation OFF (key-off behaviour): the head turn is added on top of the look = 80 degrees (double rotation)
    {
        s5::Neutral n;CHECK(!s5::camera_frame(blobLook,f,sc,lens));
        CHECK(!s5::setup_poses(neutral,n,f,sc)&&!s5::setup_poses(head,n,f,sc));
        s5::Camera a;CHECK(eye_axes(f,0,a));CHECK(std::fabs(angle_between(a.fwd,f.cam.fwd)-hy)<2e-5);
    }
    // (b) head 40, game look 0: the eye is 40 degrees to the left of the rendered camera, with or without the (identity) compensation
    {
        s5::Neutral n1,n2;s5::PoseFrame g;
        CHECK(!s5::camera_frame(blobFlat,f,sc,lens));LookMeasure m;
        CHECK(measure_look(f.cam.right,f.cam.up,f.cam.fwd,sc.m256,m)&&std::fabs(m.yaw)<1e-9);
        CHECK(!s5::setup_poses(neutral,n1,f,sc)&&!s5::setup_poses(head,n1,f,sc));
        s5::Camera a;CHECK(eye_axes(f,0,a));
        CHECK(std::fabs(angle_between(a.fwd,f.cam.fwd)-hy)<2e-5);
        const double sinToRight=a.fwd[0]*f.cam.right[0]+a.fwd[1]*f.cam.right[1]+a.fwd[2]*f.cam.right[2];
        CHECK(std::fabs(sinToRight+std::sin(hy))<2e-5);                              // left = towards -right
        CHECK(!s5::camera_frame(blobFlat,g,sc,lens));
        CHECK(!s5::setup_poses(neutral,n2,g,sc,false,s3::units_per_metre,m.R)&&!s5::setup_poses(head,n2,g,sc,false,s3::units_per_metre,m.R));
        double dd=0;for(int ee=0;ee<2;++ee)for(int i=0;i<16;++i)dd=std::max(dd,std::fabs(f.D[ee][i]-g.D[ee][i]));
        CHECK(dd<1e-9);
        // an exact identity matrix reproduces the key-off bytes exactly
        s5::Neutral n3;s5::PoseFrame h;const double I[9]={1,0,0,0,1,0,0,0,1};
        CHECK(!s5::camera_frame(blobFlat,h,sc,lens));
        CHECK(!s5::setup_poses(neutral,n3,h,sc,false,s3::units_per_metre,I)&&!s5::setup_poses(head,n3,h,sc,false,s3::units_per_metre,I));
        CHECK(std::memcmp(h.D,f.D,sizeof f.D)==0&&std::memcmp(h.vpEye,f.vpEye,sizeof f.vpEye)==0&&std::memcmp(h.eyeWorld,f.eyeWorld,sizeof f.eyeWorld)==0);
    }
    // (c) pitch: head pitch 25 with the game looking 25 up -> identity; the head at neutral while the game looks 25 up -> 25 down
    {
        const double hp=25*kDeg;uint8_t blobP[416];make_blob(e,0,hp,blobP);
        acz::stereo::EyePose headP[2];head_poses(0,hp,0,headP);
        s5::Neutral n;CHECK(!s5::camera_frame(blobP,f,sc,lens));LookMeasure m;
        CHECK(measure_look(f.cam.right,f.cam.up,f.cam.fwd,sc.m256,m)&&std::fabs(m.pitch-hp)<1e-5);
        CHECK(!s5::setup_poses(neutral,n,f,sc)&&!s5::setup_poses(headP,n,f,sc,false,s3::units_per_metre,m.R));
        s5::Camera a;CHECK(eye_axes(f,0,a)&&angle_between(a.fwd,f.cam.fwd)<2e-5&&angle_between(a.up,f.cam.up)<2e-5);
        CHECK(!s5::setup_poses(neutral,n,f,sc,false,s3::units_per_metre,m.R));
        CHECK(eye_axes(f,0,a)&&std::fabs(angle_between(a.fwd,f.cam.fwd)-hp)<2e-5);
        const double upDot=a.fwd[0]*f.cam.up[0]+a.fwd[1]*f.cam.up[1]+a.fwd[2]*f.cam.up[2];
        CHECK(std::fabs(upDot+std::sin(hp))<2e-5);                                   // down
    }
    // (d) head sway stays in the cockpit frame: with a look of 40 a 0.1 m lean forward moves the eyes along the COCKPIT forward axis
    {
        acz::stereo::EyePose lean[2];head_poses(hy,0,-0.1,lean);
        s5::Neutral n;CHECK(!s5::camera_frame(blobLook,f,sc,lens));LookMeasure m;CHECK(measure_look(f.cam.right,f.cam.up,f.cam.fwd,sc.m256,m));
        CHECK(!s5::setup_poses(neutral,n,f,sc)&&!s5::setup_poses(lean,n,f,sc,false,s3::units_per_metre,m.R));
        double mv[3];for(int k=0;k<3;++k)mv[k]=(f.eyeWorld[0][k]+f.eyeWorld[1][k])*0.5-f.cam.eye[k];
        const double len=std::sqrt(mv[0]*mv[0]+mv[1]*mv[1]+mv[2]*mv[2]);
        const double want[3]={-e[2].x,-e[2].y,-e[2].z};
        CHECK(len>3.0&&angle_between(mv,want)<2e-5);
        // key off: the same lean moves along the looking camera's forward instead (the double-rotation side effect)
        CHECK(!s5::setup_poses(lean,n,f,sc));
        for(int k=0;k<3;++k)mv[k]=(f.eyeWorld[0][k]+f.eyeWorld[1][k])*0.5-f.cam.eye[k];
        CHECK(angle_between(mv,want)>0.3);
    }
}

// ------------------------------------------------------------------ test sweep + synthetic head
struct TPose {float position[3];float orientation[4];};
static void test_sweep_no_xr() { // no pose at all: the scripted head drives the controller; keys follow the sweep sign
    FakeInjector inj;Driver d;d.set_injector(&inj);d.configure(true,true);
    CHECK(d.no_xr()&&d.sweep());
    Driver d2;d2.configure(false,true);CHECK(!d2.no_xr()); // no_xr only with the sweep
    unsigned left=0,right=0;double now=1000.0;
    for(int i=0;i<(55*8);++i) { // first 8 s of the sweep at 55 fps (peak left at 3 s, peak right at 9 s)
        d.advance_sweep(now);d.on_frame(now,true,nullptr,0.0);
        if(inj.down()&key_left)++left;
        if(inj.down()&key_right)++right;
        now+=1000.0/55.0;
    }
    CHECK(left>20);
    CHECK(std::fabs(d.sweep_yaw())<=61*kDeg);
    d.shutdown();CHECK(inj.down()==0);
    char buf[512];now+=1500;d.on_frame(now,true,nullptr,0.0);bool got=false;for(int i=0;i<3&&!got;++i){now+=1000;d.advance_sweep(now);d.on_frame(now,true,nullptr,0.0);got=d.take_log(now,buf,sizeof buf);}
    CHECK(got&&std::strstr(buf,"mode=test-sweep-no-xr;")&&std::strstr(buf,"residual-rms="));
    (void)right;
}
static void test_sweep() {
    double y,p;
    sweep_value(0,y,p);CHECK(std::fabs(y)<1e-12&&std::fabs(p)<1e-12);
    sweep_value(3,y,p);CHECK(std::fabs(y-60*kDeg)<1e-9&&p==0);          // quarter period of 12 s
    sweep_value(9,y,p);CHECK(std::fabs(y+60*kDeg)<1e-9);
    sweep_value(12,y,p);CHECK(std::fabs(y)<1e-9);
    sweep_value(15,y,p);CHECK(std::fabs(y-60*kDeg)<1e-9);               // second period
    sweep_value(24,y,p);CHECK(std::fabs(y)<1e-9&&std::fabs(p)<1e-9);    // pitch phase starts at 0
    sweep_value(30,y,p);CHECK(y==0&&std::fabs(p-40*kDeg)<1e-9);          // peak
    sweep_value(36,y,p);CHECK(std::fabs(p)<1e-9&&std::fabs(y)<1e-9);    // cycle end
    sweep_value(39,y,p);CHECK(std::fabs(y-60*kDeg)<1e-9);                // repeats
    sweep_value(-5,y,p);CHECK(y==0&&p==0);
    // the synthetic head turn is recovered by head_angles from the first eye's rotation and keeps the eye separation
    for(double t:{3.0,9.0,27.0,30.0,33.0}) {
        sweep_value(t,y,p);
        TPose pp[2]={{{-0.032f,1.2f,0.f},{0,0,0,1}},{{0.032f,1.2f,0.f},{0,0,0,1}}};
        apply_sweep_to_poses(pp,y,p);
        double q[4]={pp[0].orientation[0],pp[0].orientation[1],pp[0].orientation[2],pp[0].orientation[3]};
        s5::EyePose a{};for(int i=0;i<4;++i)a.orientation[i]=pp[0].orientation[i];
        double R[9];CHECK(s5::quat_rot(a.orientation,R));
        double hy,hp;CHECK(head_angles(R,0.0,hy,hp)&&std::fabs(hy-y)<1e-5&&std::fabs(hp-p)<1e-5);
        double d=0;for(int k=0;k<3;++k){const double v=double(pp[1].position[k])-pp[0].position[k];d+=v*v;}
        CHECK(std::fabs(std::sqrt(d)-0.064)<1e-6);
        CHECK(std::fabs((pp[0].position[1]+pp[1].position[1])*0.5-1.2)<1e-5||p!=0);   // yaw keeps the head centre
        (void)q;
    }
}

// ------------------------------------------------------------------ controller: duty math, limits, gating
static Input base_input(double now) {Input in;in.now_ms=now;in.foreground=true;in.head_valid=true;return in;}
static void test_controller_math() {
    // feed-forward duty = |target| / 2.48 (yaw) and / 1.5708 (pitch), no measurement (open loop)
    {
        Controller c;unsigned n=0,L=0,R=0,U=0;
        const double dt=1000.0/55.0;double now=0;
        for(int i=0;i<400;++i){
            Input in=base_input(now);in.head_yaw=1.24;in.head_pitch=0.0;
            const uint8_t m=c.step(in);now+=dt;
            if(i>=100){++n;L+=(m&key_left)!=0;R+=(m&key_right)!=0;U+=(m&key_up)!=0;}
        }
        CHECK(std::fabs(double(L)/n-0.5)<0.03&&R==0&&U==0);
        CHECK(std::fabs(c.duty_yaw()-0.5)<0.01);
    }
    {   // right, and pitch
        Controller c;unsigned n=0,L=0,R=0,U=0;const double dt=1000.0/55.0;double now=0;
        for(int i=0;i<400;++i){Input in=base_input(now);in.head_yaw=-0.62;in.head_pitch=0.785398;const uint8_t m=c.step(in);now+=dt;
            if(i>=100){++n;L+=(m&key_left)!=0;R+=(m&key_right)!=0;U+=(m&key_up)!=0;}}
        CHECK(std::fabs(double(R)/n-0.25)<0.03&&L==0&&std::fabs(double(U)/n-0.5)<0.03);
    }
    // pitch DOWN is unavailable: a negative pitch target is clamped to 0 (no key at all)
    {
        Controller c;const double dt=1000.0/55.0;double now=0;unsigned any=0;
        for(int i=0;i<200;++i){Input in=base_input(now);in.head_pitch=-0.6;any+=c.step(in);now+=dt;}
        CHECK(any==0&&c.target_pitch()==0.0);
    }
    // deadzone (0.5 degree), clamps and the rate limit
    {
        Controller c;const double dt=1000.0/55.0;double now=0;unsigned any=0;
        for(int i=0;i<200;++i){Input in=base_input(now);in.head_yaw=0.005;in.head_pitch=0.005;any+=c.step(in);now+=dt;}
        CHECK(any==0&&c.target_yaw()==0.0);
        Controller d;now=0;
        for(int i=0;i<300;++i){Input in=base_input(now);in.head_yaw=4.0;in.head_pitch=3.0;d.step(in);now+=dt;}
        CHECK(std::fabs(d.target_yaw()-yaw_clamp_rad)<1e-9&&std::fabs(d.target_pitch()-pitch_max_rad)<1e-9);
        CHECK(d.duty_yaw()<=yaw_clamp_rad/yaw_full_rad+0.01&&d.duty_pitch()<=pitch_max_rad/pitch_full_rad+0.01);
        Controller e;now=0;{Input in=base_input(now);in.head_yaw=0.0;e.step(in);}     // first frame: resync
        now+=dt;{Input in=base_input(now);in.head_yaw=0.0;e.step(in);}
        now+=dt;{Input in=base_input(now);in.head_yaw=1.0;e.step(in);}                // a 1 rad step: limited to rate * dt
        CHECK(e.target_yaw()<=target_rate_rad_s*dt/1000.0+1e-9&&e.target_yaw()>0.0);
        Controller f;now=0;for(int i=0;i<5;++i){Input in=base_input(now);in.head_yaw=-5.0;in.head_pitch=-5.0;f.step(in);now+=dt;}
        CHECK(f.target_yaw()>=-yaw_clamp_rad&&f.target_pitch()==0.0);
    }
    // never opposing keys, even for a head that flips sign every few frames; a direction flip releases the old key first
    {
        Controller c;const double dt=1000.0/55.0;double now=0;unsigned both=0;
        for(int i=0;i<3000;++i){
            Input in=base_input(now);in.head_yaw=((i/7)%2?1.0:-1.0)*std::fabs(std::sin(i*0.01))*2.0;in.head_pitch=0.7*std::fabs(std::sin(i*0.003));
            in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=-in.head_yaw*0.5;in.meas_pitch=0;
            const uint8_t m=c.step(in);now+=dt;both+=((m&key_left)&&(m&key_right))?1u:0u;
        }
        CHECK(both==0);
    }
    // sigma-delta density: a constant duty d presses d of the frames to within one frame
    for(double duty:{0.1,0.25,0.5,0.75,0.9}) {
        Controller c;const double dt=1000.0/55.0;double now=0;unsigned pressed=0,N=0;
        for(int i=0;i<700;++i){Input in=base_input(now);in.head_yaw=duty*yaw_full_rad;const uint8_t m=c.step(in);now+=dt;if(i>=100){++N;pressed+=(m&key_left)!=0;}}
        CHECK(std::fabs(double(pressed)/N-duty)<0.02);
    }
    // hard cap on continuous key-down time: duty 1 (clamp raised) is interrupted every 400 ms
    {
        Params p;p.yaw_clamp=3.0;Controller c(p);const double dt=1000.0/55.0;double now=0,downFor=0,maxDown=0;
        for(int i=0;i<600;++i){Input in=base_input(now);in.head_yaw=3.0;const uint8_t m=c.step(in);now+=dt;
            if(m&key_left){downFor+=dt;maxDown=std::max(maxDown,downFor);}else downFor=0;}
        CHECK(maxDown<=max_key_down_ms+dt+1e-6);CHECK(c.capped()>0);
    }
    // foreground gating: nothing while the game is not foreground; keys resume after a resync frame
    {
        Controller c;const double dt=1000.0/55.0;double now=0;
        for(int i=0;i<60;++i){Input in=base_input(now);in.head_yaw=1.2;c.step(in);now+=dt;}
        unsigned bg=0;for(int i=0;i<60;++i){Input in=base_input(now);in.foreground=false;in.head_yaw=1.2;bg+=c.step(in);now+=dt;}
        CHECK(bg==0&&std::string(c.why())=="background");
        unsigned fg=0;for(int i=0;i<60;++i){Input in=base_input(now);in.head_yaw=1.2;fg+=c.step(in)?1u:0u;now+=dt;}
        CHECK(fg>10);
    }
    // missing head data: no keys
    {
        Controller c;const double dt=1000.0/55.0;double now=0;unsigned any=0;
        for(int i=0;i<60;++i){Input in=base_input(now);in.head_valid=false;in.head_yaw=2.0;any+=c.step(in);now+=dt;}
        CHECK(any==0&&std::string(c.why())=="no-head");
    }
    // a frame gap (> 100 ms) releases and resyncs; no integration across the gap
    {
        Controller c;const double dt=1000.0/55.0;double now=0;
        for(int i=0;i<60;++i){Input in=base_input(now);in.head_yaw=1.2;c.step(in);now+=dt;}
        now+=500;Input in=base_input(now);in.head_yaw=1.2;CHECK(c.step(in)==0&&std::string(c.why())=="resync");
    }
    // kill switch: latched for the session even if the file disappears
    {
        Controller c;const double dt=1000.0/55.0;double now=0;
        for(int i=0;i<30;++i){Input in=base_input(now);in.head_yaw=1.2;c.step(in);now+=dt;}
        {Input in=base_input(now);in.head_yaw=1.2;in.kill=true;CHECK(c.step(in)==0&&c.killed());now+=dt;}
        unsigned any=0;for(int i=0;i<100;++i){Input in=base_input(now);in.head_yaw=1.2;any+=c.step(in);now+=dt;}
        CHECK(any==0&&std::string(c.why())=="kill-switch");
    }
}

// ------------------------------------------------------------------ driver + injector contract
static void test_driver_injector() {
    FakeInjector inj;Driver d;d.set_injector(&inj);d.configure(false);
    const double dt=1000.0/55.0;double now=1000.0;
    double R[9];look_matrix(0.9,0.0,R);        // head yaw 0.9 rad (left), neutral yaw0 = 0
    for(int i=0;i<110;++i){d.on_frame(now,true,R,0.0);now+=dt;}
    CHECK(inj.downs>10&&inj.cur!=key_right&&(inj.cur&key_right)==0);
    // foreground lost while a key is held: released at once, and nothing is pressed while in the background
    inj.cur=inj.cur?inj.cur:key_left;
    const unsigned downsBefore=inj.downs;inj.fg=false;
    for(int i=0;i<55;++i){d.on_frame(now,true,R,0.0);now+=dt;}
    CHECK(inj.cur==0&&inj.downs==downsBefore&&inj.downsWhileBackground==0);
    // the pose becomes invalid: keys released
    inj.fg=true;for(int i=0;i<60;++i){d.on_frame(now,true,R,0.0);now+=dt;}
    bool anyHeld=false;for(int i=0;i<30;++i){d.on_frame(now,false,nullptr,0.0);now+=dt;anyHeld|=inj.cur!=0;}
    CHECK(!anyHeld&&inj.cur==0);
    // kill switch released everything and stays released
    for(int i=0;i<60;++i){d.on_frame(now,true,R,0.0);now+=dt;}
    inj.kill=true;d.on_frame(now,true,R,0.0);now+=dt;CHECK(inj.cur==0);
    inj.kill=false;unsigned later=0;for(int i=0;i<120;++i){d.on_frame(now,true,R,0.0);now+=dt;later+=inj.cur?1u:0u;}
    CHECK(later==0);
    // shutdown releases everything
    FakeInjector inj2;Driver d2;d2.set_injector(&inj2);d2.configure(false);now=0;
    for(int i=0;i<200&&!inj2.cur;++i){d2.on_frame(now,true,R,0.0);now+=dt;}
    CHECK(inj2.cur!=0);d2.shutdown();CHECK(inj2.cur==0&&inj2.releases==1);
    // no injector (non-Windows / factory missing): no crash, a line says so once
    Driver d3;d3.set_injector(nullptr);d3.configure(false);for(int i=0;i<10;++i)d3.on_frame(1000+i*18.0,true,R,0.0);
    char buf[256];CHECK(d3.take_log(2000,buf,sizeof buf)&&std::string(buf).find("no-injector")!=std::string::npos);CHECK(!d3.take_log(2001,buf,sizeof buf)||std::string(buf).find("no-injector")==std::string::npos);
    // measured look vs commanded estimate: invalid measurement -> estimate compensation, valid -> the measured rotation
    {
        Driver d4;d4.set_injector(&inj);d4.configure(false);
        d4.note_no_camera();const double* c=d4.compensation();for(int i=0;i<9;++i)CHECK(std::fabs(c[i]-(i%4==0?1.0:0.0))<1e-12);
        V3 e[3];aircraft_axes(0.3,0,0,e);V3 a[3];camera_axes(e,0.7,0.2,a);double m[16];m256_from_axes(e,16.0,{0,0,0},m);
        double r[3],u[3],f[3];toArr(a[0],r);toArr(a[1],u);toArr(mulv(a[2],-1.0),f);
        d4.note_camera(r,u,f,m);CHECK(d4.measured().valid&&std::fabs(d4.measured().yaw-0.7)<1e-9);
        double want[9];look_matrix(0.7,0.2,want);const double* c2=d4.compensation();for(int i=0;i<9;++i)CHECK(std::fabs(c2[i]-want[i])<1e-9);
    }
}

// ------------------------------------------------------------------ closed loop against a simulated game
struct SimGame {
    double L[2]{},gain=1.0,tau=0.1;std::deque<std::pair<double,double>> hist;unsigned delay=2;
    void step(uint8_t mask,double dt) {
        const double ty=(mask&key_left?yaw_full_rad*gain:(mask&key_right?-yaw_full_rad*gain:0.0));
        const double tp=(mask&key_up?pitch_full_rad*gain:0.0);
        const double k=1.0-std::exp(-dt/tau);
        L[0]+=(ty-L[0])*k;L[1]+=(tp-L[1])*k;
        L[0]=clampd(L[0],-yaw_full_rad,yaw_full_rad);L[1]=clampd(L[1],0.0,pitch_full_rad);
        hist.push_back({L[0],L[1]});while(hist.size()>delay+1)hist.pop_front();
    }
    std::pair<double,double> measured() const {return hist.empty()?std::pair<double,double>{0,0}:hist.front();}
};
struct SimResult {double rmsYaw=0,rmsPitch=0,maxYaw=0,rippleYaw=0;unsigned both=0,keyFrames=0,frames=0;double maxDown=0;};
static SimResult run_sim(double seconds,bool useMeasurement,double gain,unsigned delay,bool sweepAll,double fixedYaw=1e9,uint32_t seed=1,double fps=55.0,bool closed=true) {
    SimGame g;g.gain=gain;g.delay=delay;Controller c(closed?Params::closed_loop():Params{});SimResult r;
    uint32_t rng=seed;auto rnd=[&]{rng=rng*1664525u+1013904223u;return (double(rng>>8)/double(1<<24))-0.5;};
    double now=0,downFor=0;double sumY=0,sumP=0;unsigned n=0;
    const double base=1000.0/fps;
    double rmin=1e9,rmax=-1e9;
    while(now<seconds*1000.0) {
        const double jitter=rnd()*4.0;           // +-2 ms frame jitter
        const double dtms=base+jitter;now+=dtms;
        double ty,tp;if(fixedYaw<1e8){ty=fixedYaw;tp=0;}else sweep_value(now/1000.0,ty,tp);
        if(!sweepAll&&fixedYaw>1e8){tp=0;}
        Input in=base_input(now);in.head_yaw=ty;in.head_pitch=tp;
        if(useMeasurement){auto m=g.measured();in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=m.first+rnd()*0.01;in.meas_pitch=m.second+rnd()*0.01;}
        const uint8_t mask=c.step(in);
        g.step(mask,dtms/1000.0);
        ++r.frames;r.both+=((mask&key_left)&&(mask&key_right))?1u:0u;r.keyFrames+=mask?1u:0u;
        if(mask&key_left||mask&key_right||mask&key_up){downFor+=dtms;r.maxDown=std::max(r.maxDown,downFor);}else downFor=0;
        const double ey=g.L[0]-ty,ep=g.L[1]-std::max(0.0,tp);
        sumY+=ey*ey;sumP+=ep*ep;++n;r.maxYaw=std::max(r.maxYaw,std::fabs(ey));
        if(fixedYaw<1e8&&now>seconds*1000.0*0.5){rmin=std::min(rmin,g.L[0]);rmax=std::max(rmax,g.L[0]);}
    }
    r.rmsYaw=std::sqrt(sumY/n)/kDeg;r.rmsPitch=std::sqrt(sumP/n)/kDeg;r.maxYaw/=kDeg;r.rippleYaw=(rmax-rmin)/kDeg;
    return r;
}
static void test_closed_loop() {
    // The brief: 55 fps frames, tau 0.1 s, saturation 2.48, key -> target +-2.48; track the 60 degree / 12 s sinusoid, RMS < 8 degrees.
    const double yawSecs=24.0;
    const auto cl=run_sim(yawSecs,true,1.0,2,false);
    if(verbose())std::printf("  closed loop (measured, delay 2): rms yaw %.2f deg, max %.2f deg, key-frames %u/%u, max down %.0f ms\n",cl.rmsYaw,cl.maxYaw,cl.keyFrames,cl.frames,cl.maxDown);
    CHECK(cl.rmsYaw<8.0&&cl.both==0&&cl.maxDown<=max_key_down_ms+25.0);
    // delay and gain robustness: measurement latency up to 4 frames, key gain 0.85 .. 1.15 (run G: 0.87 .. 1.06)
    for(unsigned delay:{0u,1u,3u,4u})for(double gain:{0.85,1.0,1.15}) {
        const auto r=run_sim(yawSecs,true,gain,delay,false,1e9,7);
        if(verbose())std::printf("  closed loop delay %u gain %.2f: rms %.2f deg max %.2f\n",delay,gain,r.rmsYaw,r.maxYaw);
        // the key gain is learned from the measurement; the floor is the one-frame pulse ripple (~7 deg RMS at 55 fps)
        CHECK(r.rmsYaw<(gain>1.1?9.0:8.0)&&r.both==0);
    }
    // open loop (measurement invalid): the model alone; right gain passes, a wrong gain cannot be corrected without the measurement
    for(double gain:{1.0,0.87,1.06}) {
        const auto r=run_sim(yawSecs,false,gain,2,false,1e9,3);
        if(verbose())std::printf("  open loop gain %.2f: rms %.2f deg max %.2f\n",gain,r.rmsYaw,r.maxYaw);
        CHECK(r.rmsYaw<(gain==1.0?8.0:9.5)&&r.both==0);
    }
    // whole 36 s cycle including the pitch phase (0..40 degrees up)
    {
        const auto r=run_sim(36.0,true,1.0,2,true);
        if(verbose())std::printf("  full sweep cycle: rms yaw %.2f pitch %.2f deg\n",r.rmsYaw,r.rmsPitch);
        CHECK(r.rmsYaw<8.0&&r.rmsPitch<8.0&&r.both==0);
    }
    // a different frame rate and a static look (ripple): 40 degrees held, ripple of the filtered look reported
    for(double fps:{40.0,55.0,75.0}) {
        const auto r=run_sim(12.0,true,1.0,2,false,40*kDeg,5,fps);
        if(verbose())std::printf("  hold 40 deg @%.0f fps: rms %.2f deg, ripple (p-p of the true look) %.1f deg\n",fps,r.rmsYaw,r.rippleYaw);
        CHECK(r.rmsYaw<(fps<50.0?12.0:8.0));   // the ripple grows as the frame rate drops (40 fps: ~10 deg RMS)
    }
    // the response to an unreachable pitch (down) and a step: no wind-up, comes back
    {
        SimGame g;Controller c(Params::closed_loop());double now=0;const double dt=1000.0/55.0;
        for(int i=0;i<220;++i){now+=dt;Input in=base_input(now);in.head_yaw=1.0;auto m=g.measured();in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=m.first;in.meas_pitch=m.second;g.step(c.step(in),dt/1000.0);}
        CHECK(std::fabs(g.L[0]-1.0)<0.12);
        for(int i=0;i<220;++i){now+=dt;Input in=base_input(now);in.head_yaw=0.0;auto m=g.measured();in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=m.first;in.meas_pitch=m.second;g.step(c.step(in),dt/1000.0);}
        CHECK(std::fabs(g.L[0])<0.12);
    }
}

// ------------------------------------------------------------------ J-179 resolution: injector core against a fault-injecting OS backend
// The OS is a fake: no key is ever sent. send results are scripted (partial SendInput failures), the clock is manual, the worker thread is
// never started (tick() is called by hand), so every interleaving is deterministic.
struct FakeOs final:OsBackend {
    uint64_t t=1000;bool fg=true,kill=false,startOk=true;unsigned startCalls=0,wakes=0,sends=0;
    uint8_t os=0;bool opposingEver=false;          // the OS-level held set (what the game would see)
    unsigned failUps=0,failDowns=0;bool failAllUps=false;
    std::function<void()> onForeground;            // runs inside the worker's foreground query (= between the request read and the send)
    bool send_key(uint8_t bit,bool up) noexcept override {
        ++sends;
        if(up&&(failAllUps||failUps)){if(failUps)--failUps;return false;}
        if(!up&&failDowns){--failDowns;return false;}
        if(up)os&=uint8_t(~bit);
        else {if((bit==key_left&&(os&key_right))||(bit==key_right&&(os&key_left)))opposingEver=true;os|=bit;}
        return true;
    }
    uint64_t now_ms() noexcept override {return t;}
    bool foreground() noexcept override {if(onForeground){auto f=onForeground;onForeground=nullptr;f();}return fg;}
    bool kill_file() noexcept override {return kill;}
    bool start_worker(void (*)(void*),void*) noexcept override {++startCalls;return startOk;}
    void wake() noexcept override {++wakes;}
    void wait_ms(unsigned) noexcept override {}
};
static void test_core_thread_start_failure() {
    FakeOs os;os.startOk=false;InjectorCore c(os);
    // the producer requests LEFT, the worker cannot start: fail closed, latched, reported, never a key-down
    c.apply(key_left,0);
    CHECK(c.fault()&fault_thread_start);CHECK(os.os==0&&os.sends==0&&c.down()==0);
    for(int i=0;i<20;++i){os.t+=18;c.apply(key_left|key_up,0);}   // the producer keeps asking (and then stalls): still nothing
    CHECK(os.os==0&&os.sends==0&&c.down()==0&&os.startCalls==1&&(c.fault()&fault_thread_start));
    CHECK(c.release_all());                                         // nothing held: confirmed
    // the Driver reports the fault once and stops asking for keys
    Driver d;d.set_injector(&c);d.configure(false);double R[9];look_matrix(0.9,0.0,R);double now=5000;
    char buf[400];bool saw=false;
    for(int i=0;i<30;++i){d.on_frame(now,true,R,0.0);now+=18;if(d.take_log(now,buf,sizeof buf)&&std::string(buf).find("injector-fault")!=std::string::npos&&std::string(buf).find("thread-start:1")!=std::string::npos)saw=true;}
    CHECK(saw&&os.os==0&&os.sends==0);
}
static void test_core_normal_and_watchdog() {
    FakeOs os;InjectorCore c(os);
    c.apply(key_left,0);CHECK(os.startCalls==1&&c.down()==0);          // consumption is the worker's
    CHECK(c.tick()&&c.down()==key_left&&os.os==key_left&&c.fault()==0);
    os.t+=100;c.apply(key_left,0);CHECK(c.tick()&&os.os==key_left);
    os.t+=300;CHECK(c.tick());CHECK(os.os==0&&c.down()==0);             // the producer stalled: the watchdog releases (250 ms)
    c.apply(0,0);CHECK(c.tick()&&os.os==0);
    // a zero before anything was ever requested starts no thread
    FakeOs os2;InjectorCore c2(os2);c2.apply(0,0);CHECK(os2.startCalls==0);
    // the hard cap (500 ms) releases and blocks the key until the request dropped it once
    FakeOs os3;InjectorCore c3(os3);
    for(int i=0;i<40;++i){os3.t+=18;c3.apply(key_left,0);c3.tick();}
    CHECK(os3.os==0&&c3.down()==0);
    os3.t+=18;c3.apply(0,0);c3.tick();os3.t+=18;c3.apply(key_left,0);c3.tick();CHECK(os3.os==key_left);
}
static void test_core_release_failure() {
    // (a) shutdown key-up fails: reported, latched, NOT silently discarded; the worker stays on as the release-only owner
    {
        FakeOs os;InjectorCore c(os);c.apply(key_left,0);c.tick();CHECK(os.os==key_left);
        os.failAllUps=true;
        CHECK(!c.release_all());
        CHECK(c.down()==key_left&&os.os==key_left);
        CHECK((c.fault()&fault_release_pending)&&(c.fault()&fault_shutdown_unconfirmed));
        c.apply(key_right,0);                                          // nothing may be pressed after a shutdown
        for(int i=0;i<5;++i){os.t+=50;CHECK(c.tick());}                // the owner keeps retrying (and keeps living)
        CHECK(os.os==key_left&&c.up_failures()>=6);
        os.failAllUps=false;os.t+=50;CHECK(!c.tick());                 // the retry succeeded: released, the owner may exit
        CHECK(os.os==0&&c.down()==0&&!(c.fault()&fault_release_pending)&&(c.fault()&fault_shutdown_unconfirmed));
        CHECK(!os.opposingEver);
        // and a shutdown that did release says so
        FakeOs o2;InjectorCore c2(o2);c2.apply(key_up,0);c2.tick();CHECK(c2.release_all()&&o2.os==0&&!c2.fault());
    }
    // (b) reversal LEFT -> RIGHT with a failing LEFT key-up: RIGHT must NOT go down on top of the held LEFT
    {
        FakeOs os;InjectorCore c(os);c.apply(key_left,0);c.tick();
        os.t+=18;c.apply(key_right,0);os.failUps=1;c.tick();
        CHECK(os.os==key_left&&c.down()==key_left&&(c.fault()&fault_release_pending)&&!os.opposingEver);
        os.t+=18;c.apply(key_right,0);c.tick();                         // the retry: LEFT up confirmed first, then RIGHT down
        CHECK(os.os==key_right&&c.down()==key_right&&!(c.fault()&fault_release_pending)&&!os.opposingEver);
    }
    // (c) any failed required key-up suppresses EVERY new down (also an unrelated key) until all required ups succeeded
    {
        FakeOs os;InjectorCore c(os);c.apply(key_left,0);c.tick();
        os.t+=18;c.apply(key_up,0);os.failUps=2;c.tick();
        CHECK(os.os==key_left&&(c.fault()&fault_release_pending));
        os.t+=18;c.apply(key_up,0);c.tick();CHECK(os.os==key_left);   // still failing (second scripted failure)
        os.t+=18;c.apply(key_up,0);c.tick();CHECK(os.os==key_up&&!(c.fault()&fault_release_pending));
    }
    // (d) the opposing guard uses the ACTUAL held set: a stuck LEFT blocks a RIGHT request even when LEFT is no longer wanted at all
    {
        FakeOs os;InjectorCore c(os);c.apply(key_left,0);c.tick();
        os.failAllUps=true;
        for(int i=0;i<10;++i){os.t+=18;c.apply(key_right,0);c.tick();}
        CHECK(os.os==key_left&&!os.opposingEver&&c.down_failures()==0);
    }
    // (e) a failed key-down is not a held key and does not wedge the machine
    {
        FakeOs os;InjectorCore c(os);os.failDowns=1;c.apply(key_left,0);c.tick();
        CHECK(os.os==0&&c.down()==0&&c.down_failures()==1&&c.fault()==0);
        os.t+=18;c.apply(key_left,0);c.tick();CHECK(os.os==key_left);
    }
}
static void test_core_cancellation_and_gates() {
    // a zero overwrites a request the worker has not consumed
    {FakeOs os;InjectorCore c(os);c.apply(key_left,0);os.t+=5;c.apply(0,0);CHECK(c.tick());CHECK(os.sends==0&&os.os==0);}
    // the cancellation arrives between the worker's request read and its send
    {FakeOs os;InjectorCore c(os);c.apply(key_left,0);os.onForeground=[&]{c.apply(0,0);};c.tick();CHECK(os.sends==0&&os.os==0);}
    // the worker honours the kill file itself (fresh stamp, foreground), latched for the process
    {FakeOs os;InjectorCore c(os);c.apply(key_left,0);os.kill=true;c.tick();CHECK(os.sends==0&&os.os==0);
     os.kill=false;os.t+=18;c.apply(key_left,0);c.tick();CHECK(os.sends==0&&os.os==0);}
    // a held key is released by a kill that appears later
    {FakeOs os;InjectorCore c(os);c.apply(key_left,0);c.tick();CHECK(os.os==key_left);os.kill=true;os.t+=18;c.apply(key_left,0);c.tick();CHECK(os.os==0);}
    // foreground lost: nothing pressed, a held key released
    {FakeOs os;InjectorCore c(os);os.fg=false;c.apply(key_left,0);c.tick();CHECK(os.sends==0);
     os.fg=true;os.t+=18;c.apply(key_left,0);c.tick();CHECK(os.os==key_left);os.fg=false;c.tick();CHECK(os.os==0);}
}
// A consumer that, like the real worker, acts on a request later than it was made. down() is what is held, NOT what is requested.
struct DeferredInjector final:KeyInjector {
    bool fg=true,kill=false,pending=false;uint8_t want=0,held=0;unsigned downs=0,applies=0;unsigned faultBits=0;bool releaseOk=true;
    bool foreground() noexcept override {return fg;}
    bool kill_switch() noexcept override {return kill;}
    void apply(uint8_t mask,double) noexcept override {++applies;want=mask;pending=true;}   // newest request wins; not acted on yet
    bool release_all() noexcept override {want=0;pending=true;if(releaseOk)held=0;return releaseOk;}
    uint8_t down() const noexcept override {return held;}
    unsigned fault() const noexcept override {return faultBits;}
    void consume() {if(!pending)return;pending=false;const uint8_t w=(fg&&!kill)?want:uint8_t(0);if((w&~held))++downs;held=w;}
};
static void test_driver_deferred_consumption() {
    const double dt=1000.0/55.0;double R[9];look_matrix(0.9,0.0,R);
    auto primeUntilPending=[&](DeferredInjector& inj,Driver& d,double& now){
        for(int i=0;i<200;++i){now+=dt;d.on_frame(now,true,R,0.0);if(inj.want){return;}inj.consume();}
    };
    // pose loss between enqueue and consumption
    {
        DeferredInjector inj;Driver d;d.set_injector(&inj);d.configure(false);double now=1000;
        primeUntilPending(inj,d,now);CHECK(inj.want==key_left&&inj.held==0);       // a press is queued, nothing is down
        now+=dt;d.on_frame(now,false,nullptr,0.0);                                  // the pose disappears
        CHECK(inj.want==0);inj.consume();CHECK(inj.held==0&&inj.downs==0);
    }
    // kill switch between enqueue and consumption
    {
        DeferredInjector inj;Driver d;d.set_injector(&inj);d.configure(false);double now=1000;
        primeUntilPending(inj,d,now);CHECK(inj.want==key_left);
        inj.kill=true;now+=dt;d.on_frame(now,true,R,0.0);CHECK(inj.want==0);inj.consume();CHECK(inj.held==0&&inj.downs==0);
    }
    // foreground lost between enqueue and consumption
    {
        DeferredInjector inj;Driver d;d.set_injector(&inj);d.configure(false);double now=1000;
        primeUntilPending(inj,d,now);inj.fg=false;now+=dt;d.on_frame(now,true,R,0.0);CHECK(inj.want==0);inj.consume();CHECK(inj.downs==0);
    }
    // Driver::release cancels a pending request even though down() is zero
    {
        DeferredInjector inj;Driver d;d.set_injector(&inj);d.configure(false);double now=1000;
        primeUntilPending(inj,d,now);CHECK(inj.want==key_left&&inj.held==0);
        d.release(now+dt);CHECK(inj.want==0);inj.consume();CHECK(inj.downs==0);
    }
    // an injector in fault receives only zeros; the fault and a failed shutdown are reported
    {
        DeferredInjector inj;Driver d;d.set_injector(&inj);d.configure(false);double now=1000;
        inj.faultBits=fault_release_pending;for(int i=0;i<50;++i){now+=dt;d.on_frame(now,true,R,0.0);CHECK(inj.want==0);}
        inj.releaseOk=false;CHECK(!d.shutdown());
        char buf[400];bool saw=false;if(d.take_log(now,buf,sizeof buf))saw=std::string(buf).find("injector-fault")!=std::string::npos&&std::string(buf).find("UNCONFIRMED")!=std::string::npos;
        CHECK(saw);
    }
}

// ------------------------------------------------------------------ J-179 finding 4: the look of a world-first frame
static void test_world_first_look() {
    V3 e[3];aircraft_axes(-0.785398,0.05,0.1,e);
    FakeInjector inj;Driver d;d.set_injector(&inj);d.configure(false);
    s5::Lens lens;acz::stereo::EyePose neutral[2];head_poses(0,0,0,neutral);double Rh[9];look_matrix(0,0,Rh);
    auto axes=[&](const uint8_t* blob,const double* lookR,s5::Camera& out,s5::Camera& rendered,double yaw)->bool{
        s5::Scratch sc;s5::PoseFrame f;s5::Neutral n;acz::stereo::EyePose head[2];head_poses(yaw,0,0,head);
        if(s5::camera_frame(blob,f,sc,lens))return false;rendered=f.cam;
        if(s5::setup_poses(neutral,n,f,sc)||s5::setup_poses(head,n,f,sc,false,s3::units_per_metre,lookR))return false;
        return eye_axes(f,0,out);
    };
    // frame 1: the game rendered look 0.0; the cockpit member measured it (the aircraft reference)
    uint8_t b1[416],b2[416];make_blob(e,0.0,0,b1);
    {s5::Scratch sc;s5::PoseFrame f;CHECK(!s5::camera_frame(b1,f,sc,lens));d.note_camera(f.cam.right,f.cam.up,f.cam.fwd,sc.m256);d.on_frame(1000.0,true,Rh,0.0);}
    CHECK(d.measured().valid);
    double Rprev[9];std::memcpy(Rprev,d.measured().R,sizeof Rprev);
    // frame 2: the look changed to 0.3 rad (head 0.3). A world draw opens the frame BEFORE its cockpit member.
    const double look2=0.3;make_blob(e,look2,0,b2);
    s5::Scratch scw;s5::PoseFrame fw;CHECK(!s5::camera_frame(b2,fw,scw,lens));          // the world camera = the camera of the frame
    const double* Rw=d.estimate_current(fw.cam.right,fw.cam.up,fw.cam.fwd,1018.0);CHECK(Rw!=nullptr);
    if(!Rw)return;
    double Rworld[9];std::memcpy(Rworld,Rw,sizeof Rworld);
    s5::Camera aw,cw,ao,co,ac,cc;
    CHECK(axes(b2,Rworld,aw,cw,look2));                                                   // world-first replay (the fix)
    CHECK(axes(b2,Rprev,ao,co,look2));                                                    // the previous frame's look (the J-179 defect)
    {d.note_camera(fw.cam.right,fw.cam.up,fw.cam.fwd,scw.m256);}                         // the cockpit member measures the current look
    CHECK(d.measured().valid&&std::fabs(d.measured().yaw-look2)<1e-5);
    CHECK(axes(b2,d.compensation(),ac,cc,look2));                                         // cockpit replay
    CHECK(angle_between(aw.fwd,ac.fwd)<2e-5&&angle_between(aw.right,ac.right)<2e-5&&angle_between(aw.up,ac.up)<2e-5);   // world == cockpit
    CHECK(angle_between(aw.fwd,cw.fwd)<2e-5);                                                  // and both equal the rendered camera (identity residual)
    CHECK(angle_between(ao.fwd,ac.fwd)>0.25);                                                 // the defect: an extra 0.3 rad turn in the world
    CHECK(rotation_angle(Rworld,d.compensation())<1e-6);                                       // the adoption check passes
    // the aircraft turns by 0.02 rad between the two frames: the estimate is off by that much, still within the adoption tolerance
    {
        V3 e2[3];aircraft_axes(-0.785398+0.02,0.05,0.1,e2);uint8_t b3[416];make_blob(e2,look2,0,b3);
        Driver d2;d2.set_injector(&inj);d2.configure(false);
        {s5::Scratch sc;s5::PoseFrame f;CHECK(!s5::camera_frame(b1,f,sc,lens));d2.note_camera(f.cam.right,f.cam.up,f.cam.fwd,sc.m256);d2.on_frame(1000.0,true,Rh,0.0);}
        s5::Scratch s3c;s5::PoseFrame f3;CHECK(!s5::camera_frame(b3,f3,s3c,lens));
        const double* r3=d2.estimate_current(f3.cam.right,f3.cam.up,f3.cam.fwd,1018.0);CHECK(r3!=nullptr);
        double used[9];std::memcpy(used,r3,sizeof used);d2.note_camera(f3.cam.right,f3.cam.up,f3.cam.fwd,s3c.m256);
        const double err=rotation_angle(used,d2.compensation());
        CHECK(err>0.005&&err<Driver::early_look_tolerance_rad);
        // a 0.2 rad aircraft turn is a mismatch the adoption must refuse
        V3 e3[3];aircraft_axes(-0.785398+0.2,0.05,0.1,e3);uint8_t b4[416];make_blob(e3,look2,0,b4);
        Driver d3;d3.set_injector(&inj);d3.configure(false);
        {s5::Scratch sc;s5::PoseFrame f;CHECK(!s5::camera_frame(b1,f,sc,lens));d3.note_camera(f.cam.right,f.cam.up,f.cam.fwd,sc.m256);d3.on_frame(1000.0,true,Rh,0.0);}
        s5::Scratch s4c;s5::PoseFrame f4;CHECK(!s5::camera_frame(b4,f4,s4c,lens));
        const double* r4=d3.estimate_current(f4.cam.right,f4.cam.up,f4.cam.fwd,1018.0);CHECK(r4!=nullptr);
        std::memcpy(used,r4,sizeof used);d3.note_camera(f4.cam.right,f4.cam.up,f4.cam.fwd,s4c.m256);
        CHECK(rotation_angle(used,d3.compensation())>Driver::early_look_tolerance_rad);
    }
    // no valid reference -> the early open is refused (nullptr): none yet, a frame without a camera, an invalid measurement, a stale tick
    {
        Driver f0;f0.set_injector(&inj);f0.configure(false);
        CHECK(f0.estimate_current(fw.cam.right,fw.cam.up,fw.cam.fwd,1018.0)==nullptr);
        s5::Scratch sc;s5::PoseFrame f;CHECK(!s5::camera_frame(b1,f,sc,lens));
        f0.note_camera(f.cam.right,f.cam.up,f.cam.fwd,sc.m256);f0.on_frame(1000.0,true,Rh,0.0);
        CHECK(f0.estimate_current(fw.cam.right,fw.cam.up,fw.cam.fwd,1018.0)!=nullptr);
        CHECK(f0.estimate_current(fw.cam.right,fw.cam.up,fw.cam.fwd,1200.0)==nullptr);   // older than max_frame_gap_ms
        f0.note_no_camera();CHECK(f0.estimate_current(fw.cam.right,fw.cam.up,fw.cam.fwd,1018.0)==nullptr);
        double bad[16]{};f0.note_camera(f.cam.right,f.cam.up,f.cam.fwd,bad);f0.on_frame(1030.0,true,Rh,0.0);
        CHECK(!f0.measured().valid&&f0.estimate_current(fw.cam.right,fw.cam.up,fw.cam.fwd,1040.0)==nullptr);
    }
}

#ifdef _WIN32
// The real injector (registered by look_drive_keys.cpp): its kill-switch file and foreground query against a private LOCALAPPDATA.
// It is constructed but NEVER given a key: this test cannot type anything.
static void test_real_injector_kill_file() {
    CHECK(g_injector_factory.load()!=nullptr);
    namespace fs=std::filesystem;
    const fs::path root=fs::temp_directory_path()/("acz-look-drive-test-"+std::to_string(GetCurrentProcessId()));
    std::error_code ec;fs::create_directories(root/"acz-vr",ec);
    SetEnvironmentVariableW(L"LOCALAPPDATA",root.wstring().c_str());
    KeyInjector* inj=g_injector_factory.load()();
    CHECK(inj!=nullptr);
    if(!inj)return;
    CHECK(inj->down()==0);
    CHECK(!inj->kill_switch());                                       // no file yet
    {std::ofstream f(root/"acz-vr"/"look-drive.stop");f<<"stop";}
    Sleep(260);                                                       // the injector caches the file check for 200 ms
    CHECK(inj->kill_switch());
    fs::remove(root/"acz-vr"/"look-drive.stop",ec);
    Sleep(260);
    CHECK(!inj->kill_switch());
    // the controller latches it for the session even after the file is gone
    Controller c;const double dt=1000.0/55.0;double now=0;
    for(int i=0;i<10;++i){Input in=base_input(now);in.head_yaw=1.0;c.step(in);now+=dt;}
    {Input in=base_input(now);in.head_yaw=1.0;in.kill=true;CHECK(c.step(in)==0);now+=dt;}
    {Input in=base_input(now);in.head_yaw=1.0;in.kill=false;CHECK(c.step(in)==0&&c.killed());}
    (void)inj->foreground();                                          // no crash; the console process may or may not own the foreground
    fs::remove_all(root,ec);
}
#endif

int main() {
    test_config();test_measure();test_double_rotation();test_sweep();test_sweep_no_xr();test_controller_math();test_driver_injector();test_closed_loop();
    test_core_thread_start_failure();test_core_normal_and_watchdog();test_core_release_failure();test_core_cancellation_and_gates();test_driver_deferred_consumption();test_world_first_look();
    // the real injector is registered (never driven here: no key is ever sent by this test)
#ifdef _WIN32
    test_real_injector_kill_file();
#endif
    std::printf("look_drive_keys_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
