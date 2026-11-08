#pragma once
// Look drive over XInput (key xr_look_drive_xinput, default OFF; docs/review/xinput-look-impl.md).
//
// Instead of typing NumPad keys into the focused window, the look is sent as a RIGHT-STICK offset: this injector publishes two small
// integers into a named shared-memory block (../skip-intro/look_ipc.h) that the user's XINPUT1_4.dll proxy (src/skip-intro, only with
// look_xinput=1 in skip-intro.ini) adds to XInputGetState(0). Nothing here writes game memory, hooks or patches anything, and nothing
// depends on which window has the focus.
//
// Fail-safe model: the proxy treats stale (> 150 ms by default), absent, malformed or inactive data as "no offset", hard-clamps what it
// accepts and honours the same kill-switch file as the keyboard route. This injector additionally publishes an explicit inactive zero on
// kill switch, shutdown and every apply() (the key-mask entry point is never used by this route).
//
// Header-only logic against the IpcBackend interface (offline tests drive it with a plain memory block and a manual clock); the Windows
// backend (file mapping, kill file, foreground description) and the registration are in look_drive_keys.cpp.
#include "look_drive_keys.h"
#include "../skip-intro/look_ipc.h"
#include <cstdio>
#include <mutex>

namespace acz::look_drive {

struct IpcBackend {
    virtual ~IpcBackend()=default;
    virtual acz::look_ipc::Block* block() noexcept=0;     // the writable shared block; nullptr = it could not be created
    virtual uint64_t now_ms() noexcept=0;                  // GetTickCount64 in the real backend (the proxy compares against the same clock)
    virtual bool kill_file() noexcept=0;                   // %LOCALAPPDATA%\acz-vr\look-drive.stop exists (cached by the backend)
    virtual void describe_foreground(char* buf,size_t n) noexcept {if(buf&&n)buf[0]=0;}
};

class XInputInjector final:public KeyInjector {
public:
    explicit XInputInjector(IpcBackend& os) noexcept:os_(os) {}
    bool foreground() noexcept override {return true;}     // never gates on window focus (the whole point of this route)
    bool kill_switch() noexcept override {return os_.kill_file();}
    bool analog() const noexcept override {return true;}
    // The key route's entry point: this route has no keys, so it only ever publishes an explicit zero (a request cancel).
    void apply(uint8_t,double) noexcept override {std::lock_guard<std::mutex> lk(m_);publish_locked(0,0,false);}
    // yaw_duty > 0 = look LEFT = stick X NEGATIVE (left); pitch_duty > 0 = look UP = stick Y POSITIVE (up).
    void apply_axes(double yaw_duty,double pitch_duty,double) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        if(os_.kill_file())kill_latched_=true;
        if(kill_latched_||stopping_){publish_locked(0,0,false);return;}
        // pitch down is not offered by the game's look (pitch_min = 0): negative pitch fractions are dropped here as well.
        publish_locked(acz::look_ipc::to_stick(-yaw_duty),acz::look_ipc::to_stick(pitch_duty<0?0.0:pitch_duty),true);
    }
    bool release_all() noexcept override {std::lock_guard<std::mutex> lk(m_);stopping_=true;publish_locked(0,0,false);return true;}
    uint8_t down() const noexcept override {return 0;}
    unsigned fault() const noexcept override {return fault_.load();}
    void status_text(char* buf,size_t n) noexcept override {
        if(!buf||!n)return;
        char fg[96];fg[0]=0;os_.describe_foreground(fg,sizeof fg);
        std::snprintf(buf,n,"ipc=%s;ipc-published=%u;stick=%d,%d;kill-latched=%u;%s",os_.block()?"ok":"UNAVAILABLE",published_.load(),last_x_.load(),last_y_.load(),
            kill_latched_?1u:0u,fg);
    }
    unsigned published() const noexcept {return published_.load();}
    int last_x() const noexcept {return last_x_.load();}
    int last_y() const noexcept {return last_y_.load();}
private:
    void publish_locked(int x,int y,bool active) noexcept {
        acz::look_ipc::Block* b=os_.block();
        if(!b){fault_.fetch_or(fault_no_ipc);return;}
        acz::look_ipc::publish(b,x,y,os_.now_ms(),active);
        ++published_;last_x_=active?x:0;last_y_=active?y:0;
    }
    IpcBackend& os_;std::mutex m_;
    bool stopping_=false;std::atomic<bool> kill_latched_{false};
    std::atomic<unsigned> fault_{0},published_{0};std::atomic<int> last_x_{0},last_y_{0};
};

#ifdef _WIN32
// Test entry: the real Windows backend on a private mapping name and kill-file path (never freed). nullptr when the mapping failed.
KeyInjector* make_named_xinput_injector(const wchar_t* mapping_name,const wchar_t* stop_path) noexcept;
#endif

} // namespace acz::look_drive
