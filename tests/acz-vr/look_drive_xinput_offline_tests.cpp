// xr_look_drive_xinput offline fixtures (docs/review/xinput-look-impl.md): the right-stick IPC (src/skip-intro/look_ipc.h: staleness, clamp,
// kill switch, torn/garbage blocks), the XInput injector, the analog controller route, the open-loop default, the idle-reference calibration,
// the measurement jump gate, the per-second status line and the config keys. Nothing here sends a key, loads the proxy DLL, touches the game,
// XInput, the GPU or XR. The only OS objects are a private named file mapping and a private kill-switch file under the temp directory.
#include "look_drive_keys.h"
#include "look_drive_injector.h"
#include "look_drive_xinput.h"
#include "runtime_config.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
namespace ipc = acz::look_ipc;

static int g_failures=0,g_checks=0;
#define CHECK(c) do{++g_checks;if(!(c)){++g_failures;std::printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);}}while(0)
static const double kDeg=kPi/180.0;
static Input base_input(double now) {Input in;in.now_ms=now;in.foreground=true;in.head_valid=true;return in;}

// ------------------------------------------------------------------ IPC block: freshness, staleness, clamp, garbage
static void test_ipc_block() {
    ipc::Block b{};
    // absent / never written
    CHECK(!ipc::read_block(nullptr,1000,150,32767).ok);
    CHECK(!ipc::read_block(&b,1000,150,32767).ok&&std::string(ipc::read_block(&b,1000,150,32767).why)=="bad-header");   // zeroed memory
    // fresh
    ipc::publish(&b,-12000,8000,1000,true);
    auto s=ipc::read_block(&b,1050,150,32767);
    CHECK(s.ok&&s.x==-12000&&s.y==8000);
    // staleness window: exactly 150 ms is still fresh, 151 ms is stale; the producer stopping therefore centres the stick
    CHECK(ipc::read_block(&b,1150,150,32767).ok);
    s=ipc::read_block(&b,1151,150,32767);CHECK(!s.ok&&s.x==0&&s.y==0&&std::string(s.why)=="stale");
    CHECK(!ipc::read_block(&b,5000,150,32767).ok);
    // the configured window is bounded to [20, 1000] ms
    CHECK(!ipc::read_block(&b,1100,1,32767).ok&&ipc::read_block(&b,1015,1,32767).ok);       // 1 ms is raised to 20 ms
    CHECK(ipc::read_block(&b,1900,100000,32767).ok&&!ipc::read_block(&b,2100,100000,32767).ok); // 100000 ms is cut to 1000 ms
    // a stamp from the future (clock mismatch or garbage) is refused beyond the small skew
    CHECK(ipc::read_block(&b,960,150,32767).ok&&!ipc::read_block(&b,900,150,32767).ok);
    CHECK(std::string(ipc::read_block(&b,900,150,32767).why)=="future-stamp");
    // hard clamp: the writer clamps to +-32767, the reader to its own maximum
    ipc::publish(&b,1000000,-1000000,2000,true);
    s=ipc::read_block(&b,2000,150,32767);CHECK(s.ok&&s.x==32767&&s.y==-32767);
    s=ipc::read_block(&b,2000,150,20000);CHECK(s.ok&&s.x==20000&&s.y==-20000);
    s=ipc::read_block(&b,2000,150,0);CHECK(s.ok&&s.x==0&&s.y==0);
    s=ipc::read_block(&b,2000,150,999999);CHECK(s.ok&&s.x==32767);                              // the reader maximum is capped too
    CHECK(ipc::to_stick(2.0)==32767&&ipc::to_stick(-2.0)==-32767&&ipc::to_stick(0.5)==16384&&ipc::to_stick(0.0)==0);
    CHECK(ipc::to_stick(std::nan(""))==0&&ipc::to_stick(INFINITY)==0&&ipc::to_stick(-INFINITY)==0);
    // inactive (explicit stop) contributes nothing
    ipc::publish(&b,5000,5000,3000,false);
    s=ipc::read_block(&b,3000,150,32767);CHECK(!s.ok&&std::string(s.why)=="inactive");
    // garbage: corrupted payload, wrong magic/version, a write in progress (odd sequence)
    ipc::publish(&b,5000,5000,3000,true);
    {ipc::Block c=b;c.x=-5000;CHECK(!ipc::read_block(&c,3000,150,32767).ok&&std::string(ipc::read_block(&c,3000,150,32767).why)=="bad-check");}
    {ipc::Block c=b;c.magic^=1;CHECK(!ipc::read_block(&c,3000,150,32767).ok);}
    {ipc::Block c=b;c.version=2;CHECK(!ipc::read_block(&c,3000,150,32767).ok);}
    {ipc::Block c=b;c.seq|=1u;CHECK(!ipc::read_block(&c,3000,150,32767).ok&&std::string(ipc::read_block(&c,3000,150,32767).why)=="torn");}
    // the sequence advances by two per publish and stays even
    {const uint32_t q0=b.seq;ipc::publish(&b,1,2,3001,true);CHECK(b.seq==q0+2&&(b.seq&1u)==0);}
    CHECK(sizeof(ipc::Block)==40);
}

// ------------------------------------------------------------------ proxy-side policy: real stick + offset, kill switch, packet number
static void test_ipc_override() {
    ipc::Block b{};
    ipc::Override o(32767,150);
    // no data at all: the real stick passes through untouched (including -32768)
    auto r=o.apply(-32768,12,nullptr,1000,false);CHECK(!r.injected&&r.rx==-32768&&r.ry==12&&r.packet_bump==0);
    r=o.apply(-32768,12,&b,1000,false);CHECK(!r.injected&&r.rx==-32768&&r.ry==12);          // zeroed block = absent
    // fresh data adds to the real stick
    ipc::publish(&b,-10000,4000,2000,true);
    r=o.apply(500,-300,&b,2010,false);CHECK(r.injected&&r.rx==-9500&&r.ry==3700&&r.packet_bump==1);
    r=o.apply(500,-300,&b,2020,false);CHECK(r.injected&&r.packet_bump==1);                  // unchanged offset: same packet bump
    ipc::publish(&b,-9000,4000,2030,true);
    r=o.apply(0,0,&b,2031,false);CHECK(r.injected&&r.rx==-9000&&r.packet_bump==2);          // a changed offset looks like a new packet
    // sum clamp: real stick plus offset never leaves +-32767
    ipc::publish(&b,20000,-20000,2100,true);
    r=o.apply(30000,-30000,&b,2100,false);CHECK(r.rx==32767&&r.ry==-32767);
    r=o.apply(-32768,32767,&b,2100,false);CHECK(r.rx==-12768&&r.ry==12767);
    // stale: the stick centres (offset zero) and the real stick is what the game sees; the bump records the change back to zero
    const uint32_t before=o.bump();
    r=o.apply(700,-700,&b,2100+151,false);CHECK(!r.injected&&r.rx==700&&r.ry==-700&&r.packet_bump==before+1&&std::string(r.why)=="stale");
    r=o.apply(700,-700,&b,2100+400,false);CHECK(!r.injected&&r.packet_bump==before+1);
    // the reader maximum clamps the offset
    ipc::Override small(5000,150);
    ipc::publish(&b,30000,-30000,3000,true);
    r=small.apply(0,0,&b,3000,false);CHECK(r.injected&&r.rx==5000&&r.ry==-5000);
    // the kill switch latches for the process: data keeps arriving, nothing is injected, even after the file is gone
    ipc::Override k(32767,150);
    ipc::publish(&b,10000,10000,4000,true);
    r=k.apply(0,0,&b,4000,false);CHECK(r.injected);
    r=k.apply(0,0,&b,4010,true);CHECK(!r.injected&&k.killed()&&std::string(r.why)=="kill-switch");
    ipc::publish(&b,10000,10000,4020,true);
    r=k.apply(0,0,&b,4020,false);CHECK(!r.injected&&r.rx==0&&r.ry==0&&k.killed());
    // counters
    CHECK(k.injected()==1&&k.passed()==2);
}

// ------------------------------------------------------------------ XInputInjector against an in-memory block and a manual clock
struct FakeIpc final:IpcBackend {
    ipc::Block blk{};bool haveBlock=true,kill=false;uint64_t t=10000;
    ipc::Block* block() noexcept override {return haveBlock?&blk:nullptr;}
    uint64_t now_ms() noexcept override {return t;}
    bool kill_file() noexcept override {return kill;}
    void describe_foreground(char* buf,size_t n) noexcept override {std::snprintf(buf,n,"fg=class:Fake,pid:1,self:no");}
};
static void test_injector() {
    FakeIpc os;XInputInjector inj(os);
    CHECK(inj.analog()&&inj.foreground()&&inj.down()==0&&inj.fault()==0);        // never depends on the window focus
    // yaw > 0 (look left) = stick X negative; pitch > 0 (up) = stick Y positive
    inj.apply_axes(0.5,0.25,0);
    auto s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(s.ok&&s.x==-16384&&s.y==8192);
    inj.apply_axes(-1.0,0.0,0);s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(s.ok&&s.x==32767&&s.y==0);
    inj.apply_axes(9.0,-3.0,0);s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(s.ok&&s.x==-32767&&s.y==0);   // clamp; pitch down is dropped
    inj.apply_axes(std::nan(""),std::nan(""),0);s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(s.ok&&s.x==0&&s.y==0);
    // the key-mask entry point never produces a stick offset: it publishes an explicit zero
    inj.apply_axes(0.5,0.5,0);inj.apply(key_left|key_up,0);
    s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(!s.ok&&std::string(s.why)=="inactive");
    // staleness end to end: the producer stops publishing, the stick centres after the window
    inj.apply_axes(0.8,0.0,0);os.t+=100;CHECK(ipc::read_block(&os.blk,os.t,150,32767).ok);
    os.t+=100;s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(!s.ok&&s.x==0);
    // kill switch: explicit inactive zero, latched even after the file disappears; apply_axes then never publishes an offset again
    os.t+=10;inj.apply_axes(0.8,0.4,0);CHECK(ipc::read_block(&os.blk,os.t,150,32767).ok);
    os.kill=true;os.t+=10;inj.apply_axes(0.8,0.4,0);s=ipc::read_block(&os.blk,os.t,150,32767);CHECK(!s.ok&&std::string(s.why)=="inactive");
    os.kill=false;for(int i=0;i<20;++i){os.t+=18;inj.apply_axes(0.8,0.4,0);CHECK(!ipc::read_block(&os.blk,os.t,150,32767).ok);}
    CHECK(inj.kill_switch()==false);                                               // the file is gone; the latch is inside apply_axes
    char txt[256];inj.status_text(txt,sizeof txt);CHECK(std::string(txt).find("kill-latched=1")!=std::string::npos&&std::string(txt).find("fg=class:Fake")!=std::string::npos);
    // shutdown: release_all publishes inactive and nothing after it
    FakeIpc os2;XInputInjector i2(os2);i2.apply_axes(0.5,0.5,0);CHECK(i2.release_all());
    CHECK(!ipc::read_block(&os2.blk,os2.t,150,32767).ok);i2.apply_axes(0.5,0.5,0);CHECK(!ipc::read_block(&os2.blk,os2.t,150,32767).ok);
    // no shared block (mapping could not be created): fail closed with a fault bit, nothing crashes
    FakeIpc os3;os3.haveBlock=false;XInputInjector i3(os3);i3.apply_axes(0.5,0.5,0);i3.release_all();
    CHECK((i3.fault()&fault_no_ipc)!=0);i3.status_text(txt,sizeof txt);CHECK(std::string(txt).find("UNAVAILABLE")!=std::string::npos);
}

#ifdef _WIN32
// The real Windows backend: a private mapping name and kill file; the reader opens it read-only exactly like the proxy does.
static void test_real_mapping() {
    namespace fs=std::filesystem;
    const std::wstring name=L"Local\\acz-vr-look-xinput-test-"+std::to_wstring(GetCurrentProcessId());
    const fs::path root=fs::temp_directory_path()/("acz-look-xinput-test-"+std::to_string(GetCurrentProcessId()));
    std::error_code ec;fs::create_directories(root,ec);
    const fs::path stop=root/"look-drive.stop";
    KeyInjector* inj=make_named_xinput_injector(name.c_str(),stop.wstring().c_str());
    CHECK(inj!=nullptr);if(!inj)return;
    HANDLE m=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
    CHECK(m!=nullptr);if(!m)return;
    const void* v=MapViewOfFile(m,FILE_MAP_READ,0,0,sizeof(ipc::Block));
    CHECK(v!=nullptr);if(!v){CloseHandle(m);return;}
    const auto* view=static_cast<const ipc::Block*>(v);
    ipc::Override o(32767,150);
    CHECK(!o.apply(0,0,view,GetTickCount64(),false).injected);                         // nothing published yet
    inj->apply_axes(0.5,0.5,0);
    auto r=o.apply(100,-100,view,GetTickCount64(),false);CHECK(r.injected&&r.rx==100-16384&&r.ry==-100+16384);
    Sleep(220);                                                                        // no further publish: stale after 150 ms
    r=o.apply(100,-100,view,GetTickCount64(),false);CHECK(!r.injected&&r.rx==100&&r.ry==-100);
    inj->apply_axes(0.5,0.0,0);CHECK(o.apply(0,0,view,GetTickCount64(),false).injected);
    // the kill file: created -> the injector (cached for 200 ms) publishes zero and stays latched; the proxy-side policy latches on its own
    {std::ofstream f(stop);f<<"stop";}
    Sleep(260);inj->apply_axes(0.5,0.5,0);
    CHECK(!o.apply(0,0,view,GetTickCount64(),false).injected);
    fs::remove(stop,ec);Sleep(260);inj->apply_axes(0.5,0.5,0);CHECK(!o.apply(0,0,view,GetTickCount64(),false).injected);
    inj->release_all();
    UnmapViewOfFile(v);CloseHandle(m);
    fs::remove_all(root,ec);
    // the default-named factory is registered for the DLL
    CHECK(g_analog_injector_factory.load()!=nullptr&&g_injector_factory.load()!=nullptr);
    // R-9: a block that already exists (created by another process in the session, or a second game instance) is refused: no IPC, fault no_ipc,
    // and nothing is written into the foreign block.
    {
        const std::wstring squat=L"Local\\acz-vr-look-xinput-squat-"+std::to_wstring(GetCurrentProcessId());
        HANDLE pre=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(ipc::Block)),squat.c_str());
        CHECK(pre!=nullptr);
        if(pre) {
            void* pv=MapViewOfFile(pre,FILE_MAP_WRITE,0,0,sizeof(ipc::Block));
            CHECK(pv!=nullptr);
            if(pv) {
                std::memset(pv,0x5A,sizeof(ipc::Block));
                KeyInjector* sq=make_named_xinput_injector(squat.c_str(),(root/"none.stop").wstring().c_str());
                CHECK(sq!=nullptr);
                if(sq) {
                    sq->apply_axes(0.5,0.5,0);
                    CHECK((sq->fault()&fault_no_ipc)!=0);
                    const auto* bytes=static_cast<const unsigned char*>(pv);bool untouched=true;
                    for(size_t i=0;i<sizeof(ipc::Block);++i)untouched=untouched&&bytes[i]==0x5A;
                    CHECK(untouched);
                    sq->release_all();
                }
                UnmapViewOfFile(pv);
            }
            CloseHandle(pre);
        }
    }
}
#endif

// ------------------------------------------------------------------ analog controller route
struct SimLook {   // the game's look filter driven by a stick fraction
    double L[2]{};double tau=0.1;
    void step(double yawDuty,double pitchDuty,double dt) {
        const double k=1.0-std::exp(-dt/tau);
        L[0]+=(yawDuty*yaw_full_rad-L[0])*k;L[1]+=(pitchDuty*pitch_full_rad-L[1])*k;
    }
};
static void test_analog_controller() {
    const double dt=1000.0/55.0;
    // constant head: the duty settles at target / full, no key is ever requested
    {Controller c;c.set_analog(true);double now=0;unsigned keys=0;
     for(int i=0;i<400;++i){Input in=base_input(now);in.head_yaw=1.24;in.head_pitch=0.7854;keys+=c.step(in);now+=dt;}
     CHECK(keys==0&&std::fabs(c.analog_yaw()-0.5)<0.01&&std::fabs(c.analog_pitch()-0.5)<0.01);}
    // right = negative duty; pitch down is unavailable (0)
    {Controller c;c.set_analog(true);double now=0;
     for(int i=0;i<400;++i){Input in=base_input(now);in.head_yaw=-0.62;in.head_pitch=-0.5;c.step(in);now+=dt;}
     CHECK(std::fabs(c.analog_yaw()+0.25)<0.01&&c.analog_pitch()==0.0);}
    // clamps: |duty| <= clamp / full, never above 1
    {Controller c;c.set_analog(true);double now=0;
     for(int i=0;i<600;++i){Input in=base_input(now);in.head_yaw=9.0;in.head_pitch=9.0;c.step(in);now+=dt;}
     CHECK(c.analog_yaw()<=1.0&&c.analog_yaw()<=yaw_clamp_rad/yaw_full_rad+0.01&&c.analog_pitch()<=pitch_max_rad/pitch_full_rad+0.01);}
    // the game filter tracks the +-60 degree 12 s sinusoid with a small residual (no pulse ripple on the analog route)
    {Controller c;c.set_analog(true);SimLook g;double now=0,sum=0,mx=0;unsigned n=0;
     while(now<24000.0){now+=dt;double ty,tp;sweep_value(now/1000.0,ty,tp);Input in=base_input(now);in.head_yaw=ty;in.head_pitch=0;c.step(in);
         g.step(c.analog_yaw(),c.analog_pitch(),dt/1000.0);const double e=g.L[0]-ty;sum+=e*e;mx=std::max(mx,std::fabs(e));++n;}
     const double rms=std::sqrt(sum/n)/kDeg;CHECK(rms<2.5);CHECK(mx/kDeg<8.0);}
    // the model estimate follows the commanded look (used when the measurement is invalid)
    {Controller c;c.set_analog(true);double now=0;
     for(int i=0;i<300;++i){Input in=base_input(now);in.head_yaw=1.0;c.step(in);now+=dt;}
     CHECK(std::fabs(c.est_yaw()-1.0)<0.03);}
    // not driving (background, kill, no head) = zero duty
    {Controller c;c.set_analog(true);double now=0;
     for(int i=0;i<100;++i){Input in=base_input(now);in.head_yaw=1.2;c.step(in);now+=dt;}
     CHECK(c.analog_yaw()>0.3);
     {Input in=base_input(now);in.head_yaw=1.2;in.foreground=false;c.step(in);now+=dt;}CHECK(c.analog_yaw()==0.0&&c.analog_pitch()==0.0);
     for(int i=0;i<10;++i){Input in=base_input(now);in.head_yaw=1.2;c.step(in);now+=dt;}
     {Input in=base_input(now);in.head_yaw=1.2;in.kill=true;c.step(in);now+=dt;}CHECK(c.analog_yaw()==0.0&&c.killed());
     for(int i=0;i<50;++i){Input in=base_input(now);in.head_yaw=1.2;c.step(in);now+=dt;CHECK(c.analog_yaw()==0.0);}}
    // the optional stick-deadzone rescale (UNVERIFIED game deadzone; default 0): output = dz + (1-dz)*duty past zero
    {Params p;p.analog_deadzone=0.25;Controller c(p);c.set_analog(true);double now=0;
     for(int i=0;i<400;++i){Input in=base_input(now);in.head_yaw=1.24;c.step(in);now+=dt;}
     CHECK(std::fabs(c.analog_yaw()-(0.25+0.75*0.5))<0.01);
     Controller z(p);z.set_analog(true);now=0;for(int i=0;i<100;++i){Input in=base_input(now);in.head_yaw=0.0;z.step(in);now+=dt;}CHECK(z.analog_yaw()==0.0);}
}

// ------------------------------------------------------------------ open loop by default, closed loop opt-in
static void test_open_loop_default() {
    const double dt=1000.0/55.0;
    CHECK(!Controller{}.closed_loop()&&Controller{Params::closed_loop()}.closed_loop());
    CHECK(Params{}.ki==0.0&&Params{}.gain_mu==0.0&&Params::closed_loop().ki>0.0&&Params::closed_loop().gain_mu>0.0);
    // T1 (the analysis): a constant measured offset of 9.2 deg with a zero target must send no keys in the default (open) mode ...
    {Controller c;double now=0;unsigned any=0;
     for(int i=0;i<2000;++i){Input in=base_input(now);in.head_yaw=0.0;in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=9.2*kDeg;in.meas_pitch=0;any+=c.step(in);now+=dt;}
     CHECK(any==0);CHECK(c.gain_yaw()==1.0&&c.gain_pitch()==1.0);}
    // ... and the analog route sends a zero stick
    {Controller c;c.set_analog(true);double now=0;
     for(int i=0;i<2000;++i){Input in=base_input(now);in.head_yaw=0.0;in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=9.2*kDeg;in.meas_pitch=0;c.step(in);now+=dt;}
     CHECK(c.analog_yaw()==0.0&&c.analog_pitch()==0.0);}
    // the opt-in closed loop does react to the same offset (the integral presses RIGHT to cancel it: the test-10 symptom)
    {Controller c(Params::closed_loop());double now=0;unsigned right=0;
     for(int i=0;i<2000;++i){Input in=base_input(now);in.head_yaw=0.0;in.meas_yaw_valid=in.meas_pitch_valid=true;in.meas_yaw=9.2*kDeg;in.meas_pitch=0;right+=(c.step(in)&key_right)?1u:0u;now+=dt;}
     CHECK(right>0);}
    // the measurement never changes the open-loop keys: identical decisions with a wild, valid measurement
    {Controller a,b;double now=0;unsigned diff=0;
     for(int i=0;i<1500;++i){
         Input in=base_input(now);in.head_yaw=0.8*std::sin(i*0.02);in.head_pitch=0.3*std::fabs(std::sin(i*0.01));
         Input w=in;w.meas_yaw_valid=w.meas_pitch_valid=true;w.meas_yaw=((i/5)%2?2.0:-1.5);w.meas_pitch=((i/3)%2?1.0:0.0);
         diff+=a.step(in)!=b.step(w);now+=dt;}
     CHECK(diff==0);}
}

// ------------------------------------------------------------------ fixtures: a camera with a given look relative to the cockpit
struct V3 {double x,y,z;};
static V3 add(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static V3 mulv(V3 a,double s){return {a.x*s,a.y*s,a.z*s};}
static void toArr(V3 a,double* o){o[0]=a.x;o[1]=a.y;o[2]=a.z;}
static void aircraft_axes(V3 e[3]) {e[0]={1,0,0};e[1]={0,1,0};e[2]={0,0,1};}
static void camera_axes(const V3 e[3],double yaw,double pitch,V3 a[3]) {
    double R[9];look_matrix(yaw,pitch,R);
    for(int j=0;j<3;++j)a[j]=add(add(mulv(e[0],R[0*3+j]),mulv(e[1],R[1*3+j])),mulv(e[2],R[2*3+j]));
}
static void m256_from_axes(const V3 e[3],double scale,double* m) {
    std::memset(m,0,16*sizeof(double));
    for(int i=0;i<3;++i){m[i*4+0]=e[i].x*scale;m[i*4+1]=e[i].y*scale;m[i*4+2]=e[i].z*scale;}
    m[15]=1;
}
static void feed_camera(Driver& d,double yaw,double pitch,double now) {
    V3 e[3];aircraft_axes(e);V3 a[3];camera_axes(e,yaw,pitch,a);double m[16];m256_from_axes(e,16.0,m);
    double r[3],u[3],f[3];toArr(a[0],r);toArr(a[1],u);toArr(mulv(a[2],-1.0),f);
    d.note_camera(r,u,f,m,now);
}
struct FakeAnalog final:KeyInjector {
    bool fg=false,kill=false;unsigned axes=0,applies=0,releases=0;double yaw=0,pitch=0;
    bool foreground() noexcept override {return fg;}      // deliberately false: the analog route must not care
    bool kill_switch() noexcept override {return kill;}
    void apply(uint8_t,double) noexcept override {++applies;yaw=pitch=0;}
    bool release_all() noexcept override {++releases;return true;}
    uint8_t down() const noexcept override {return 0;}
    bool analog() const noexcept override {return true;}
    void apply_axes(double y,double p,double) noexcept override {++axes;yaw=y;pitch=p;}
    void status_text(char* buf,size_t n) noexcept override {std::snprintf(buf,n,"fake-analog");}
};

// ------------------------------------------------------------------ driver: XInput route, calibration, jump gate, status line
static void test_driver_xinput() {
    const double dt=1000.0/55.0;double now=1000.0;
    FakeAnalog inj;Driver d;d.set_injector(&inj);d.configure(false,false,true,false);
    CHECK(d.xinput()&&d.controller().analog()&&!d.controller().closed_loop());
    double R[9];look_matrix(0.9,0.0,R);
    for(int i=0;i<110;++i){d.on_frame(now,true,R,0.0);now+=dt;}
    CHECK(inj.axes==110&&inj.applies==0&&inj.yaw>0.3);                                // focus (fg=false) is irrelevant, no key-mask apply
    // kill switch: zeros, latched
    inj.kill=true;d.on_frame(now,true,R,0.0);now+=dt;CHECK(inj.yaw==0.0&&inj.pitch==0.0);
    inj.kill=false;for(int i=0;i<60;++i){d.on_frame(now,true,R,0.0);now+=dt;CHECK(inj.yaw==0.0);}
    // invalid pose: a zero is published, not a stale request
    FakeAnalog j2;Driver d2;d2.set_injector(&j2);d2.configure(false,false,true,false);double n2=1000.0;
    for(int i=0;i<110;++i){d2.on_frame(n2,true,R,0.0);n2+=dt;}
    CHECK(j2.yaw>0.3);for(int i=0;i<5;++i){d2.on_frame(n2,false,nullptr,0.0);n2+=dt;}CHECK(j2.yaw==0.0);
    // shutdown releases
    FakeAnalog j3;Driver d3;d3.set_injector(&j3);d3.configure(false,false,true,false);CHECK(d3.shutdown()&&j3.releases==1);
    // the per-second status line: companion line with route, loop mode, calibration, jump gate and the injector's text
    FakeAnalog j4;Driver d4;d4.set_injector(&j4);d4.configure(false,false,true,true);double n4=1000.0;char buf[640],st[640];
    bool got=false;for(int i=0;i<200&&!got;++i){d4.on_frame(n4,true,R,0.0);got=d4.take_log(n4,buf,sizeof buf);n4+=dt;}
    CHECK(got&&std::string(buf).find("look-drive;")==0&&std::string(buf).size()<480);
    CHECK(d4.take_status(st,sizeof st)&&std::string(st).find("look-drive-status;route=xinput;loop=closed;")==0&&std::string(st).find("fake-analog")!=std::string::npos&&std::string(st).size()<480);
    CHECK(!d4.take_status(st,sizeof st));
}
static void test_calibration() {
    const double dt=1000.0/55.0;
    double R[9];look_matrix(0.0,0.0,R);            // head forward
    // idle look of 9.2 degrees yaw, nothing commanded: after 1 s of stillness the reference is stored and the look reads ~0
    {
        FakeAnalog inj;Driver d;d.set_injector(&inj);d.configure(false,false,true,false);double now=1000.0;
        for(int i=0;i<30;++i){feed_camera(d,9.2*kDeg,0.0,now);d.compensation();d.on_frame(now,true,R,0.0);now+=dt;}
        CHECK(!d.calibrated_ref()&&std::fabs(d.measured().yaw-9.2*kDeg)<1e-6);         // still within the first second: raw
        for(int i=0;i<60;++i){feed_camera(d,9.2*kDeg,0.0,now);d.compensation();d.on_frame(now,true,R,0.0);now+=dt;}
        CHECK(d.calibrated_ref()&&std::fabs(d.cal_yaw()-9.2*kDeg)<1e-6);
        feed_camera(d,9.2*kDeg,0.0,now);CHECK(std::fabs(d.measured().yaw)<1e-6&&std::fabs(d.measured().pitch)<1e-6);
        const double* c=d.compensation();for(int i=0;i<9;++i)CHECK(std::fabs(c[i]-(i%4==0?1.0:0.0))<1e-9);   // identity at idle
        // a later look of +50 deg from that idle reads 50 deg relative to the reference (yaw/pitch stay decoupled)
        now+=400.0;feed_camera(d,(9.2+50.0)*kDeg,0.0,now);CHECK(d.measured().valid&&std::fabs(d.measured().yaw-50.0*kDeg)<1e-6&&std::fabs(d.measured().pitch)<1e-6);
        now+=400.0;feed_camera(d,(9.2-50.0)*kDeg,0.0,now);CHECK(std::fabs(d.measured().yaw+50.0*kDeg)<1e-6);
        // the world-first estimate uses the same reference (same measurement convention as the cockpit's own)
        d.on_frame(now,true,R,0.0);
        V3 e[3];aircraft_axes(e);V3 a[3];camera_axes(e,(9.2+20.0)*kDeg,0.0,a);double r[3],u[3],f[3];toArr(a[0],r);toArr(a[1],u);toArr(mulv(a[2],-1.0),f);
        const double* er=d.estimate_current(r,u,f,now+1.0);CHECK(er!=nullptr);
        if(er){double want[9];look_matrix(20.0*kDeg,0.0,want);double ang=rotation_angle(er,want);CHECK(ang<0.5*kDeg);}
    }
    // a turned head, a commanded look, or a moving look: no calibration
    {
        FakeAnalog inj;Driver d;d.set_injector(&inj);d.configure(false,false,true,false);double now=1000.0;double Rh[9];look_matrix(0.5,0.0,Rh);
        for(int i=0;i<200;++i){feed_camera(d,9.2*kDeg,0,now);d.compensation();d.on_frame(now,true,Rh,0.0);now+=dt;}
        CHECK(!d.calibrated_ref());
        FakeAnalog inj2;Driver d2;d2.set_injector(&inj2);d2.configure(false,false,true,false);now=1000.0;
        for(int i=0;i<200;++i){feed_camera(d2,(9.2+2.0*std::sin(i*0.3))*kDeg,0,now);d2.compensation();d2.on_frame(now,true,R,0.0);now+=dt;}   // +-2 deg wobble
        CHECK(!d2.calibrated_ref());
    }
}
static void test_jump_gate() {
    const double dt=1000.0/55.0;double R[9];look_matrix(0.5,0.0,R);   // head turned: the idle calibration never triggers here
    FakeAnalog inj;Driver d;d.set_injector(&inj);d.configure(false,false,true,false);double now=1000.0;
    for(int i=0;i<20;++i){feed_camera(d,0.3,0.0,now);d.compensation();d.on_frame(now,true,R,0.0);now+=dt;}
    CHECK(d.measured_valid()==20&&d.jump_rejects()==0);
    // a one-frame +-2 rad spike is rejected (the filter cannot slew 25 rad/s * 18 ms = 0.45 rad that far): the previous good look is held
    const double ctlBefore=d.controller().duty_yaw();double base[9];std::memcpy(base,d.compensation(),sizeof base);
    feed_camera(d,2.3,0.0,now);CHECK(d.jump_rejects()==1&&std::fabs(d.measured().yaw-0.3)<1e-9);
    for(int i=0;i<9;++i)CHECK(std::fabs(d.compensation()[i]-base[i])<1e-12);
    d.on_frame(now,true,R,0.0);now+=dt;CHECK(std::fabs(d.controller().duty_yaw()-ctlBefore)<1e-4);   // the controller does not see the spike either (1e-4: the lead low-pass tail of the settling target, was 1e-9 before the filtered lead)
    feed_camera(d,0.3,0.0,now);CHECK(d.jump_rejects()==1&&std::fabs(d.measured().yaw-0.3)<1e-9);now+=dt;
    feed_camera(d,-2.0,0.0,now);CHECK(d.jump_rejects()==2&&std::fabs(d.measured().yaw-0.3)<1e-9);now+=dt;
    feed_camera(d,0.31,0.0,now);CHECK(std::fabs(d.measured().yaw-0.31)<1e-9);now+=dt;
    // a fast but physically possible move (0.4 rad in one 18 ms frame, below full/tau * dt) is accepted
    feed_camera(d,0.71,0.0,now);CHECK(d.jump_rejects()==2&&std::fabs(d.measured().yaw-0.71)<1e-9);now+=dt;
    // a persistent new value is accepted after the resync window (the earlier "good" value may have been the outlier)
    unsigned held=0;for(int i=0;i<12;++i){feed_camera(d,-1.5,0.0,now);held+=std::fabs(d.measured().yaw+1.5)>1e-9?1u:0u;now+=dt;}
    // (the allowed rotation grows with the time since the last GOOD sample: 2.2 rad become plausible after ~90 ms, never later than the 8-frame resync)
    CHECK(held>=3&&held<=8&&std::fabs(d.measured().yaw+1.5)<1e-9);
    // a measurement gap longer than 250 ms never gates (resync)
    now+=400.0;feed_camera(d,0.5,0.0,now);CHECK(std::fabs(d.measured().yaw-0.5)<1e-9);
    // without a clock (legacy callers) there is no gating
    Driver e;e.set_injector(&inj);e.configure(false);V3 ax[3];aircraft_axes(ax);
    {V3 a[3];camera_axes(ax,0.0,0.0,a);double m[16];m256_from_axes(ax,16.0,m);double r[3],u[3],f[3];toArr(a[0],r);toArr(a[1],u);toArr(mulv(a[2],-1.0),f);
     e.note_camera(r,u,f,m);V3 b[3];camera_axes(ax,2.0,0.0,b);toArr(b[0],r);toArr(b[1],u);toArr(mulv(b[2],-1.0),f);e.note_camera(r,u,f,m);
     CHECK(e.jump_rejects()==0&&std::fabs(e.measured().yaw-2.0)<1e-9);}
}


// ------------------------------------------------------------------ test 12 deadzone + lead: simulated game model, old controller vs new
// The game's right stick (D-288 fit, docs/review/look-drive-deadzone-lead.md): magnitudes below `dz` are ignored, the rest is rescaled to 0..1,
// the look follows through a first-order filter (tau 0.1 s) and saturates at the full-scale look.
struct SimGame {
    double dz=0.19,tau=0.1,L[2]{};
    void step(int sx,int sy,double dt) {
        const double k=1.0-std::exp(-dt/tau);
        auto eff=[&](int s){const double m=std::fabs(double(s))/32767.0;const double u=m<=dz?0.0:(m-dz)/(1.0-dz);return s<0?-u:u;};
        L[0]+=(eff(sx)*yaw_full_rad-L[0])*k;L[1]+=(eff(sy)*pitch_full_rad-L[1])*k;
    }
};
// One head trajectory through controller -> stick units -> (optional proxy deadzone remap) -> game. Yaw only. Errors in degrees.
// The controller's yaw is positive = look LEFT = stick X negative; the sim game turns a NEGATIVE stick X into a POSITIVE look (the mapping is
// symmetric, so the sign convention only has to be self-consistent).
struct RunResult {double rms=0,mx=0,fin=0;};
template<class F> static RunResult run_traj(const Params& p,bool remap,double dz_game,double seconds,F head) {
    const double dt=1000.0/55.0;Controller c(p);c.set_analog(true);SimGame g;g.dz=dz_game;
    double now=0,sum=0;unsigned n=0;RunResult r;
    while(now<seconds*1000.0) {
        now+=dt;const double h=head(now/1000.0);Input in=base_input(now);in.head_yaw=h;c.step(in);
        int sx=-ipc::to_stick(c.analog_yaw());if(remap)sx=ipc::deadzone_remap(sx,0.19);
        g.step(sx,0,dt/1000.0);
        const double look=-g.L[0];   // negative stick X = positive look
        if(now>500.0){const double e=look-h;sum+=e*e;++n;r.mx=std::max(r.mx,std::fabs(e));r.fin=e;}
    }
    r.rms=std::sqrt(sum/std::max(1u,n))/kDeg;r.mx/=kDeg;r.fin/=kDeg;return r;
}
static void test_deadzone_lead() {
    // remap unit tests
    CHECK(ipc::deadzone_remap(0,0.19)==0&&ipc::deadzone_remap(1000,0.0)==1000&&ipc::deadzone_remap(-1000,0.0)==-1000);
    CHECK(ipc::deadzone_remap(32767,0.19)==32767&&ipc::deadzone_remap(-32767,0.19)==-32767);
    CHECK(ipc::deadzone_remap(1,0.19)==6227&&ipc::deadzone_remap(-1,0.19)==-6227);                    // any non-zero request is lifted to the game's deadzone edge
    CHECK(ipc::deadzone_remap(16384,0.19)==ipc::clamp_stick(std::llround(0.19*32767+16384*0.81)));
    CHECK(ipc::deadzone_remap(5,std::nan(""))==5&&ipc::deadzone_remap(5,-1.0)==5&&ipc::deadzone_remap(100,5.0)==ipc::clamp_stick(std::llround(0.9*32767+100*0.1)));
    // Override with a deadzone: the offset is remapped before the real stick is added; the default (0) is unchanged (see test_ipc_override)
    {ipc::Block b{};ipc::publish(&b,-16384,0,2000,true);ipc::Override o(32767,150,0.19);
     auto r=o.apply(100,-50,&b,2010,false);CHECK(r.injected&&r.rx==100+ipc::deadzone_remap(-16384,0.19)&&r.ry==-50);   // a zero axis stays zero
     ipc::Override m(20000,150,0.19);r=m.apply(0,0,&b,2010,false);CHECK(r.rx==ipc::deadzone_remap(-16384,0.19)&&r.rx>=-20000);}
    // simulated game: old controller (raw frame-difference lead, no remap) against the new one (filtered bounded lead + proxy remap)
    const Params oldp=Params::legacy_lead(),newp;
    struct Case {const char* name;double secs;double (*head)(double);};
    const Case cases[]={
        {"sweep +-60 deg 12 s",24.0,[](double t){return 60.0*kDeg*std::sin(2*3.14159265358979*t/12.0);}},
        {"fast sweep +-40 deg 3 s",12.0,[](double t){return 40.0*kDeg*std::sin(2*3.14159265358979*t/3.0);}},
        {"step to 30 deg",4.0,[](double t){return t<1.0?0.0:30.0*kDeg;}},
        {"step to 90 deg",4.0,[](double t){return t<1.0?0.0:90.0*kDeg;}},
        {"hold 4 deg",4.0,[](double t){return t<1.0?0.0:4.0*kDeg;}},
        {"small motion +-3 deg 2 s",10.0,[](double t){return 3.0*kDeg*std::sin(2*3.14159265358979*t/2.0);}},
        {"hold 10 deg",4.0,[](double t){return t<1.0?0.0:10.0*kDeg;}},
    };
    std::printf("deadzone/lead sim (game dz 0.19, tau 0.1): case: old rms/max/final -> new rms/max/final (deg)\n");
    double oldSweep=0,newSweep=0;
    for(const Case& cs:cases) {
        const RunResult a=run_traj(oldp,false,0.19,cs.secs,cs.head),b=run_traj(newp,true,0.19,cs.secs,cs.head);
        std::printf("  %-26s %6.2f/%6.2f/%6.2f -> %6.2f/%6.2f/%6.2f\n",cs.name,a.rms,a.mx,a.fin,b.rms,b.mx,b.fin);
        const std::string nm=cs.name;
        CHECK(b.rms<=a.rms+0.05);                                   // never worse than the old controller
        if(nm.find("sweep +-60")==0){oldSweep=a.rms;newSweep=b.rms;}
        if(nm.find("hold")==0){CHECK(std::fabs(b.fin)<0.8);CHECK(std::fabs(a.fin)>std::fabs(b.fin)+1.0);}   // the deadzone swallowed the old hold
        if(nm.find("small")==0){CHECK(b.rms<0.5*a.rms);}
        if(nm.find("step")==0){CHECK(std::fabs(b.fin)<0.8);}
    }
    CHECK(newSweep<oldSweep&&newSweep<1.5);
    // robustness: a mismatch of the assumed deadzone (game really 0.15 or 0.25) stays bounded and is no worse than the old controller
    for(const double gdz:{0.15,0.25}) {
        const auto h=[](double t){return 60.0*kDeg*std::sin(2*3.14159265358979*t/12.0);};
        const RunResult a=run_traj(oldp,false,gdz,24.0,h),b=run_traj(newp,true,gdz,24.0,h);
        std::printf("  mismatch game dz %.2f sweep: old %.2f -> new %.2f deg rms\n",gdz,a.rms,b.rms);
        CHECK(b.rms<=a.rms+0.05&&b.rms<(gdz<0.2?6.0:9.0));
    }
    // lead bound: a 90 degree step never drives the stick past full scale (|duty| <= 1, finite) and settles on target / full
    {Controller c(newp);c.set_analog(true);double now=0,mx=0;const double dt=1000.0/55.0;
     for(int i=0;i<200;++i){now+=dt;Input in=base_input(now);in.head_yaw=i<20?0.0:1.5;c.step(in);mx=std::max(mx,std::fabs(c.analog_yaw()));CHECK(std::isfinite(c.analog_yaw()));}
     CHECK(mx<=1.0&&std::fabs(c.analog_yaw()-1.5/yaw_full_rad)<0.01);}
    // calibration without a still head: the first seconds with no head pose (nothing commanded) calibrate; the value is reported in the status line
    {const double dt=1000.0/55.0;double R[9];look_matrix(0.0,0.0,R);
     FakeAnalog inj;Driver d;d.set_injector(&inj);d.configure(false,false,true,false);double now=1000.0;
     for(int i=0;i<90;++i){feed_camera(d,9.2*kDeg,0.0,now);d.compensation();d.on_frame(now,false,nullptr,0.0);now+=dt;}   // no head pose yet
     CHECK(d.calibrated_ref()&&std::fabs(d.cal_yaw()-9.2*kDeg)<1e-6);
     feed_camera(d,9.2*kDeg,0.0,now);CHECK(std::fabs(d.measured().yaw)<1e-6);
     for(int i=0;i<60;++i){feed_camera(d,9.2*kDeg,0.0,now);d.compensation();d.on_frame(now,true,R,0.0);now+=dt;}
     CHECK(std::fabs(d.controller().duty_yaw())<0.01);                       // idle look reads 0 after the calibration: head forward = no stick
     char buf[640],st[640];bool got=false;for(int i=0;i<80&&!got;++i){feed_camera(d,9.2*kDeg,0.0,now);d.on_frame(now,true,R,0.0);got=d.take_log(now,buf,sizeof buf);now+=dt;}
     CHECK(got&&d.take_status(st,sizeof st)&&std::string(st).find("ref-cal=yes,9.2,")!=std::string::npos);}
}

// ------------------------------------------------------------------ key route status line (foreground window, send counters) against the injector core
struct FakeOs final:OsBackend {
    uint64_t t=1000;uint8_t os=0;
    bool send_key(uint8_t bit,bool up) noexcept override {if(up)os&=uint8_t(~bit);else os|=bit;return true;}
    uint64_t now_ms() noexcept override {return t;}
    bool foreground() noexcept override {return false;}
    bool kill_file() noexcept override {return false;}
    bool start_worker(void (*)(void*),void*) noexcept override {return true;}
    void wake() noexcept override {}
    void wait_ms(unsigned) noexcept override {}
    void describe_foreground(char* buf,size_t n) noexcept override {std::snprintf(buf,n,"fg=class:SteamVR_Theatre,pid:42,self:no");}
};
static void test_status_keys() {
    FakeOs os;InjectorCore core(os);Driver d;d.set_injector(&core);d.configure(false);
    double R[9];look_matrix(0.9,0.0,R);double now=1000.0;const double dt=1000.0/55.0;char buf[640],st[640];bool got=false;
    for(int i=0;i<200&&!got;++i){d.on_frame(now,true,R,0.0);got=d.take_log(now,buf,sizeof buf);now+=dt;}
    CHECK(got&&std::string(buf).find("foreground=no")!=std::string::npos);
    CHECK(d.take_status(st,sizeof st));
    const std::string s=st;
    CHECK(s.find("route=keys;loop=open;")!=std::string::npos&&s.find("fg=class:SteamVR_Theatre,pid:42,self:no")!=std::string::npos&&s.find("sends=down:0,up:0")!=std::string::npos&&s.size()<480);
}

// ------------------------------------------------------------------ config keys
static void test_config() {
    using namespace acz::diagnostics;
    CHECK(!Config{}.xr_look_drive_xinput()&&!Config{}.xr_look_drive_closed_loop());
    CHECK(!parse_config("").config.xr_look_drive_xinput()&&!parse_config("").config.xr_look_drive_closed_loop());
    CHECK(parse_config("xr_look_drive_xinput=true\n").config.xr_look_drive_xinput());
    CHECK(parse_config("xr_look_drive_xinput=1\n").config.xr_look_drive_xinput());
    CHECK(!parse_config("xr_look_drive_xinput=0\n").config.xr_look_drive_xinput());
    CHECK(parse_config("xr_look_drive_closed_loop=true\n").config.xr_look_drive_closed_loop());
    CHECK(!parse_config("xr_look_drive_closed_loop=false\n").config.xr_look_drive_closed_loop());
    for(const char* bad:{"xr_look_drive_xinput=yes\n","xr_look_drive_xinput=\n","xr_look_drive_closed_loop=2\n","xr_look_drive_closed_loop=on\n"})
        CHECK(parse_config(bad).issue==ConfigIssue::out_of_range||parse_config(bad).issue==ConfigIssue::malformed);
    CHECK(parse_config("xr_look_drive_xinput=true\nxr_look_drive_xinput=true\n").issue==ConfigIssue::duplicate);
    // latches: both extend the key route and are inert alone; the default (no new key) latches neither
    const std::string base="log_enabled=true\ngpu_copy_step5_continuous=true\nxr_stereo_output=true\n";
    auto latch=[&](const std::string& extra,bool gate=true){return parse_config(base+extra).config.look_drive_latch(gate);};
    auto l=latch("xr_look_drive_keys=true\n");CHECK(l.keys&&!l.xinput&&!l.closed_loop);
    l=latch("xr_look_drive_keys=true\nxr_look_drive_xinput=true\n");CHECK(l.keys&&l.xinput&&!l.closed_loop);
    l=latch("xr_look_drive_keys=true\nxr_look_drive_closed_loop=true\n");CHECK(l.keys&&!l.xinput&&l.closed_loop);
    l=latch("xr_look_drive_xinput=true\nxr_look_drive_closed_loop=true\n");CHECK(!l.keys&&!l.xinput&&!l.closed_loop);                 // needs the key
    l=latch("xr_look_drive_keys=true\nxr_look_drive_xinput=true\n",false);CHECK(!l.keys&&!l.xinput);                                   // no opt-in
    // the full headset set with -XInputLook: 26 keys + 2 = 28 of 32, within 2048 bytes
    const char* keys[]={"log_enabled=true","diagnostic_ownership_pins=true","gpu_copy_witness_observation=true","gpu_copy_early_device_hook=true",
        "gpu_copy_step3_experiment=true","gpu_copy_step3_whole_set=true","gpu_copy_step5_continuous=true","xr_stereo_output=true","xr_edge_feather=0.06",
        "gpu_copy_step5_members_by_class=true","gpu_copy_step5_hmd_fov=true","gpu_copy_step5_hud=true","xr_hud_layer=true","xr_hud_quad_distance_m=0.8",
        "xr_hud_scale=0.5","xr_world_quad_distance_m=500","gpu_copy_step5_units_per_metre=87","gpu_copy_step5_hud_markers=true","xr_hud_markers_layer=true",
        "xr_world_in_projection=true","gpu_copy_step5_world_objects=true","gpu_copy_step5_world_observe=true","xr_world_hold_last=true","xr_world_extend=true",
        "xr_look_drive_keys=true","xr_look_drive_xinput=true","xr_look_drive_closed_loop=true"};
    std::string cfg;for(const char* k:keys)cfg+=std::string(k)+"\n";
    const auto r=parse_config(cfg);
    CHECK(r.issue==ConfigIssue::none&&r.config.xr_look_drive_keys()&&r.config.xr_look_drive_xinput()&&r.config.xr_look_drive_closed_loop());
    CHECK(std::size(keys)<=max_config_keys&&cfg.size()<=max_config_bytes);
}

int main() {
    test_ipc_block();test_ipc_override();test_injector();
#ifdef _WIN32
    test_real_mapping();
#endif
    test_analog_controller();test_open_loop_default();test_driver_xinput();test_calibration();test_jump_gate();test_deadzone_lead();test_status_keys();test_config();
    std::printf("look_drive_xinput_offline: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
