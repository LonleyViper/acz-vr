// Real OS keyboard injector for xr_look_drive_keys (see look_drive_keys.h, look_drive_injector.h and J-look-drive-findings.md).
// SendInput of NumPad 8 / 4 / 6 with scan codes, only while the GAME'S main window is the foreground window.
// Compiled into the DLL (and the look-drive offline test, which never calls the real injector's key path).
#include "look_drive_keys.h"
#include "look_drive_injector.h"
#include "look_drive_xinput.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>

namespace acz::look_drive {
namespace {

// Run G (the only validated injection) sent wVk + the hardware scan code WITHOUT KEYEVENTF_SCANCODE (exactly like
// tools/live/acz-window-driver.ps1), and the game reacted. With the flag set Windows ignores wVk and maps the scan
// code through the keyboard state (NumLock off turns 0x4B into VK_LEFT). Keep the validated form.
constexpr bool kUseScanFlag=false;
struct KeyDef {uint8_t bit;WORD vk;WORD scan;};
constexpr KeyDef kKeys[3]={{key_left,0x64,0x4B},{key_right,0x66,0x4D},{key_up,0x68,0x48}}; // NumPad 4 / 6 / 8, not extended

// Which window holds the foreground (class name, process id, whether it is this process): the per-second look-drive line reports it
// (test 10: 69% of the seconds had the game window in the background and nothing recorded who held the focus).
void describe_fg(char* buf,size_t n) noexcept {
    if(!buf||!n)return;
    HWND h=GetForegroundWindow();
    if(!h){std::snprintf(buf,n,"fg=none");return;}
    char cls[48]="?";GetClassNameA(h,cls,sizeof cls);
    for(char* c=cls;*c;++c)if(*c==';'||*c==','||*c=='='||*c<32)*c='_';
    DWORD pid=0;GetWindowThreadProcessId(h,&pid);
    std::snprintf(buf,n,"fg=class:%s,pid:%lu,self:%s",cls,static_cast<unsigned long>(pid),pid==GetCurrentProcessId()?"yes":"no");
}

class WinBackend final:public OsBackend {
public:
    WinBackend() noexcept {
        wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
        if(n&&n<MAX_PATH)_snwprintf_s(stop_,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\look-drive.stop",local);
        wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }
    bool send_key(uint8_t bit,bool up) noexcept override {
        for(const auto& k:kKeys)if(k.bit==bit) {
            INPUT in{};in.type=INPUT_KEYBOARD;in.ki.wVk=k.vk;in.ki.wScan=k.scan;
            in.ki.dwFlags=(up?KEYEVENTF_KEYUP:0)|(kUseScanFlag?KEYEVENTF_SCANCODE:0);
            return SendInput(1,&in,sizeof in)==1;
        }
        return false;
    }
    uint64_t now_ms() noexcept override {return GetTickCount64();}
    // The foreground window must be THIS process's main game window, not merely any window of the process (an OpenXR runtime or
    // overlay window of the same process does not authorize input). The main window is the largest visible, unowned, non-tool
    // top-level window of the process, re-resolved at most every 2 s or when the cached handle is gone. Fails closed: when no such
    // window exists, or another window of the process has focus, no key is sent (the log shows foreground=no).
    bool foreground() noexcept override {
        HWND h=GetForegroundWindow();if(!h)return false;
        DWORD pid=0;GetWindowThreadProcessId(h,&pid);
        if(pid!=GetCurrentProcessId())return false;
        const ULONGLONG now=GetTickCount64();
        HWND main=reinterpret_cast<HWND>(main_.load());
        if(!main||!IsWindow(main)||now-mainAt_.load()>2000){main=find_main();main_.store(reinterpret_cast<uintptr_t>(main));mainAt_.store(now);}
        return main&&h==main;
    }
    bool kill_file() noexcept override {
        if(!stop_[0])return false;
        const ULONGLONG now=GetTickCount64();
        if(now-lastCheck_.load()<200)return lastStop_.load();
        lastCheck_.store(now);const bool s=GetFileAttributesW(stop_)!=INVALID_FILE_ATTRIBUTES;lastStop_.store(s);return s;
    }
    bool start_worker(void (*fn)(void*),void* ctx) noexcept override {
        try{std::thread(fn,ctx).detach();return true;}catch(...){return false;}
    }
    void describe_foreground(char* buf,size_t n) noexcept override {describe_fg(buf,n);}
    void wake() noexcept override {if(wake_)SetEvent(wake_);}
    void wait_ms(unsigned ms) noexcept override {if(wake_)WaitForSingleObject(wake_,ms);else Sleep(ms<20?ms:20);}
private:
    struct Find {DWORD pid;HWND best;long area;};
    static BOOL CALLBACK enum_cb(HWND h,LPARAM lp) noexcept {
        auto* f=reinterpret_cast<Find*>(lp);
        DWORD pid=0;GetWindowThreadProcessId(h,&pid);
        if(pid!=f->pid||!IsWindowVisible(h)||GetWindow(h,GW_OWNER)!=nullptr)return TRUE;
        if(GetWindowLongW(h,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return TRUE;
        RECT rc{};if(!GetClientRect(h,&rc))return TRUE;
        const long area=(rc.right-rc.left)*(rc.bottom-rc.top);
        if(area>f->area){f->area=area;f->best=h;}
        return TRUE;
    }
    static HWND find_main() noexcept {Find f{GetCurrentProcessId(),nullptr,0};EnumWindows(&enum_cb,reinterpret_cast<LPARAM>(&f));return f.best;}
    HANDLE wake_=nullptr;wchar_t stop_[MAX_PATH]{};
    std::atomic<ULONGLONG> lastCheck_{0};std::atomic<bool> lastStop_{false};
    std::atomic<uintptr_t> main_{0};std::atomic<ULONGLONG> mainAt_{0};
};

struct Holder {WinBackend os;InjectorCore core;Holder() noexcept:core(os) {}};
Holder* g_holder=nullptr;
void release_at_exit() {if(g_holder)(void)g_holder->core.release_all();}
KeyInjector* make_win_injector() noexcept {
    static std::once_flag once;
    std::call_once(once,[]{
        g_holder=new(std::nothrow) Holder(); // intentionally leaked: the worker thread may outlive any owner
        if(g_holder)std::atexit(&release_at_exit);
    });
    return g_holder?&g_holder->core:nullptr;
}
// ---- XInput route (xr_look_drive_xinput): shared-memory block + kill file; the logic is look_drive_xinput.h -------------------------------------------
class WinIpcBackend final:public IpcBackend {
public:
    WinIpcBackend(const wchar_t* name,const wchar_t* stop) noexcept {
        if(stop)_snwprintf_s(stop_,MAX_PATH,_TRUNCATE,L"%s",stop);
        else {
            wchar_t local[MAX_PATH];const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
            if(n&&n<MAX_PATH)_snwprintf_s(stop_,MAX_PATH,_TRUNCATE,L"%s\\acz-vr\\look-drive.stop",local);
        }
        map_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,static_cast<DWORD>(sizeof(acz::look_ipc::Block)),name?name:acz::look_ipc::kMappingName);
        // R-9: this process must CREATE the block (readers only open it). An existing object was made by someone else (another process in the
        // logon session, or a second game instance): refuse it, so no foreign writer can feed stick offsets; block() stays null -> fault no_ipc.
        if(map_&&GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(map_);map_=nullptr;}
        if(map_) {
            void* v=MapViewOfFile(map_,FILE_MAP_WRITE,0,0,sizeof(acz::look_ipc::Block));
            blk_=static_cast<acz::look_ipc::Block*>(v);
        }
    }
    acz::look_ipc::Block* block() noexcept override {return blk_;}
    uint64_t now_ms() noexcept override {return GetTickCount64();}
    bool kill_file() noexcept override {
        if(!stop_[0])return false;
        const ULONGLONG now=GetTickCount64();
        if(now-lastCheck_.load()<200)return lastStop_.load();
        lastCheck_.store(now);const bool s=GetFileAttributesW(stop_)!=INVALID_FILE_ATTRIBUTES;lastStop_.store(s);return s;
    }
    void describe_foreground(char* buf,size_t n) noexcept override {describe_fg(buf,n);}
private:
    HANDLE map_=nullptr;acz::look_ipc::Block* blk_=nullptr;wchar_t stop_[MAX_PATH]{};
    std::atomic<ULONGLONG> lastCheck_{0};std::atomic<bool> lastStop_{false};
};
struct XHolder {WinIpcBackend os;XInputInjector inj;XHolder(const wchar_t* n,const wchar_t* s) noexcept:os(n,s),inj(os) {}};
XHolder* g_xholder=nullptr;
void release_xinput_at_exit() {if(g_xholder)(void)g_xholder->inj.release_all();}
KeyInjector* make_win_xinput_injector() noexcept {
    static std::once_flag once;
    std::call_once(once,[]{
        g_xholder=new(std::nothrow) XHolder(nullptr,nullptr);
        if(g_xholder)std::atexit(&release_xinput_at_exit);
    });
    return g_xholder?&g_xholder->inj:nullptr;
}
struct Registrar {Registrar() noexcept {g_injector_factory.store(&make_win_injector);g_analog_injector_factory.store(&make_win_xinput_injector);}} g_registrar;

} // namespace

KeyInjector* make_named_xinput_injector(const wchar_t* mapping_name,const wchar_t* stop_path) noexcept {
    auto* h=new(std::nothrow) XHolder(mapping_name,stop_path);   // intentionally leaked (test entry; a distinct name per call)
    return h?&h->inj:nullptr;
}
} // namespace acz::look_drive
#endif
