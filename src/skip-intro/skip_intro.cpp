// ACZero "skip to main menu" mod. Deployed as XINPUT1_4.dll next to acz.exe:
// the game loads it in place of the system XInput, and from its first poll we
// patch acz.exe's USER32!GetKeyState import so that VK_RETURN reads as pressed
// in short pulses while the logos/movies/title play. Any real key press by the
// user cancels the mod; so does the end of the schedule in skip-intro.ini.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "look_ipc.h"
#include "xr_view_ipc.h"

namespace {

using GetKeyStateFn = SHORT(WINAPI*)(int);

HMODULE system_xinput;
GetKeyStateFn real_get_key_state;

INIT_ONCE started = INIT_ONCE_STATIC_INIT;
FILE* log_file;
volatile LONG cancelled;
ULONGLONG t0;  // ms at the first XInput poll

struct Config {
    double start = 3.0;     // seconds after the first XInput poll before the first pulse
    double end = 75.0;      // stop pulsing after this many seconds
    double interval = 0.8;  // seconds between pulse starts
    double hold = 0.25;     // seconds each pulse stays down
    int max_pulses = 40;
    double speed = 8.0;      // game clock multiplier while fast-forwarding
    double speed_until = 60; // real seconds after the first poll
    int auto_launch = 0;     // 1 = click Launch Game for you (uses the last saved input choice)
    // Look-drive right-stick injection (DEFAULT OFF; look_ipc.h). When 0 the XInput forwarding below is byte-for-byte what it was.
    int look_xinput = 0;             // 1 = add the acz-vr look-drive's right-stick offset to XInputGetState(0)
    int look_xinput_max = 32767;     // hard clamp of the injected offset, stick units (never above 32767)
    int look_xinput_stale_ms = 150;  // offset data older than this contributes nothing (stick passes through)
    double look_xinput_deadzone = 0.19; // fraction of full stick the game ignores (test 12 fit); the offset is remapped past it, 0 = plain add
    // LOOK-STICK GATE (DEFAULT ON; xr_view_ipc.h, D-355): while acz-vr composes the in-flight VR world from the game camera, the game receives the
    // right stick centred, so the cockpit look-around cannot turn the VR world. Without acz-vr's heartbeat nothing changes. 0 = never gate.
    int xr_block_look_stick = 1;
    int xr_block_look_stick_stale_ms = 500;  // heartbeat older than this = no gate
} cfg;

double now_s() { return double(GetTickCount64() - t0) / 1000.0; }

void log(const char* fmt, ...) {
    if (!log_file) return;
    va_list a;
    va_start(a, fmt);
    fprintf(log_file, "[%7.2f] ", now_s());
    vfprintf(log_file, fmt, a);
    fputc('\n', log_file);
    fflush(log_file);
    va_end(a);
}

void module_dir(wchar_t* out, size_t n) {
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&module_dir), &self);
    GetModuleFileNameW(self, out, DWORD(n));
    if (auto* s = wcsrchr(out, L'\\')) s[1] = 0;
}

void load_config(const wchar_t* dir) {
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%sskip-intro.ini", dir);
    auto rd = [&](const wchar_t* key, double def) {
        wchar_t buf[32], d[32];
        swprintf(d, 32, L"%g", def);
        GetPrivateProfileStringW(L"skip", key, d, buf, 32, ini);
        return wcstod(buf, nullptr);
    };
    cfg.start = rd(L"start", cfg.start);
    cfg.end = rd(L"end", cfg.end);
    cfg.interval = rd(L"interval", cfg.interval);
    cfg.hold = rd(L"hold", cfg.hold);
    cfg.max_pulses = int(rd(L"max_pulses", cfg.max_pulses));
    cfg.speed = rd(L"speed", cfg.speed);
    cfg.speed_until = rd(L"speed_until", cfg.speed_until);
    cfg.auto_launch = int(rd(L"auto_launch", cfg.auto_launch));
    cfg.look_xinput = int(rd(L"look_xinput", cfg.look_xinput)) != 0;
    cfg.look_xinput_max = int(rd(L"look_xinput_max", cfg.look_xinput_max));
    if (cfg.look_xinput_max < 0) cfg.look_xinput_max = 0;
    if (cfg.look_xinput_max > 32767) cfg.look_xinput_max = 32767;
    cfg.look_xinput_stale_ms = int(rd(L"look_xinput_stale_ms", cfg.look_xinput_stale_ms));
    if (cfg.look_xinput_stale_ms < 20) cfg.look_xinput_stale_ms = 20;
    if (cfg.look_xinput_stale_ms > 1000) cfg.look_xinput_stale_ms = 1000;
    cfg.look_xinput_deadzone = rd(L"look_xinput_deadzone", cfg.look_xinput_deadzone);
    if (!(cfg.look_xinput_deadzone >= 0.0)) cfg.look_xinput_deadzone = 0.0;
    if (cfg.look_xinput_deadzone > 0.5) cfg.look_xinput_deadzone = 0.5;
    cfg.xr_block_look_stick = int(rd(L"xr_block_look_stick", cfg.xr_block_look_stick)) != 0;
    cfg.xr_block_look_stick_stale_ms = int(rd(L"xr_block_look_stick_stale_ms", cfg.xr_block_look_stick_stale_ms));
    if (cfg.xr_block_look_stick_stale_ms < 100) cfg.xr_block_look_stick_stale_ms = 100;
    if (cfg.xr_block_look_stick_stale_ms > 2000) cfg.xr_block_look_stick_stale_ms = 2000;
}

bool pulse_active() {
    if (cancelled) return false;
    const double t = now_s();
    if (t < cfg.start) return false;
    if (t > cfg.end) return false;
    const int n = int((t - cfg.start) / cfg.interval);
    if (n >= cfg.max_pulses) return false;
    static int last = -1;
    if (n != last) { last = n; log("pulse %d", n); }
    return fmod(t - cfg.start, cfg.interval) < cfg.hold;
}

SHORT WINAPI hook_get_key_state(int vk) {
    const SHORT real = real_get_key_state(vk);
    static bool seen[256];
    if (vk >= 0 && vk < 256 && !seen[vk]) { seen[vk] = true; log("game queried vk 0x%02x", vk); }
    // A real key press by the user (other than the one we fake) hands control back.
    if (!cancelled && vk != VK_RETURN && vk != VK_LBUTTON && vk != VK_RBUTTON && (real & 0x8000) && now_s() > cfg.start + 2) {
        InterlockedExchange(&cancelled, 1);
        log("user key 0x%02x: mod off", vk);
    }
    if (vk == VK_RETURN) {
        if (real & 0x8000) { if (!InterlockedExchange(&cancelled, 1)) log("user Enter: mod off"); return real; }
        
    }
    return real;
}

using QpcFn = BOOL(WINAPI*)(LARGE_INTEGER*);
using TimeGetTimeFn = DWORD(WINAPI*)();
QpcFn real_qpc;
TimeGetTimeFn real_time_get_time;

// Virtual clocks: run `speed` times faster than real time until speed_until,
// then continue at normal rate from the shifted value (never jump back).
double clock_factor() {
    return (!cancelled && now_s() < cfg.speed_until) ? cfg.speed : 1.0;
}
CRITICAL_SECTION clock_lock;
LONGLONG qpc_real_prev, qpc_virt;
DWORD ms_real_prev, ms_virt;
LONG clock_calls[2];

BOOL WINAPI hook_qpc(LARGE_INTEGER* out) {
    LARGE_INTEGER r;
    BOOL ok = real_qpc(&r);
    if (!ok || !out) return ok;
    EnterCriticalSection(&clock_lock);
    if (!qpc_real_prev) { qpc_real_prev = r.QuadPart; qpc_virt = r.QuadPart; }
    qpc_virt += LONGLONG(double(r.QuadPart - qpc_real_prev) * clock_factor());
    qpc_real_prev = r.QuadPart;
    out->QuadPart = qpc_virt;
    LeaveCriticalSection(&clock_lock);
    if (InterlockedIncrement(&clock_calls[0]) == 1) log("game called QueryPerformanceCounter");
    return ok;
}

DWORD WINAPI hook_time_get_time() {
    const DWORD r = real_time_get_time();
    EnterCriticalSection(&clock_lock);
    if (!ms_real_prev) { ms_real_prev = r; ms_virt = r; }
    ms_virt += DWORD(double(DWORD(r - ms_real_prev)) * clock_factor());
    ms_real_prev = r;
    const DWORD v = ms_virt;
    LeaveCriticalSection(&clock_lock);
    if (InterlockedIncrement(&clock_calls[1]) == 1) log("game called timeGetTime");
    return v;
}

bool patch_import(const char* dll, const char* name, void* hook, void** real) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<char*>(base + d->Name), dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* n = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(n->Name, name)) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            *real = reinterpret_cast<void*>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

HWND game_window;
volatile bool enter_down;
BOOL CALLBACK find_window(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(h, &r);
    if (r.right - r.left < 400) return TRUE;
    game_window = h;
    return FALSE;
}

// Drives enter_down on the schedule; the DirectInput hooks below report it.
void pulse_loop() {
    bool down = false;
    for (;;) {
        Sleep(20);
        if (cancelled && !down) { if (now_s() > cfg.speed_until) return; continue; }
        const bool want = pulse_active();
        if (want != down) { enter_down = want; down = want; log("Enter %s", want ? "down" : "up"); }
        if (now_s() > cfg.end && !down) return;
    }
}


// ---- DirectInput keyboard hook -------------------------------------------
// The game reads the keyboard through dinput8. IDirectInputDevice8 vtables are
// shared per class, so creating our own keyboard device gives us the vtable to
// patch (GetDeviceState = slot 9) for the game's device too.
using GetDeviceStateFn = HRESULT(WINAPI*)(void*, DWORD, void*);
GetDeviceStateFn real_state_a, real_state_w;

void inject(DWORD cb, void* data, const char* tag) {
    static LONG calls[2];
    static DWORD seen_cb[2][8];
    DWORD* known = seen_cb[tag[0] == 'A'];
    for (int i = 0; i < 8; ++i) { if (known[i] == cb) break; if (!known[i]) { known[i] = cb; log("GetDeviceState %s new cb=%lu", tag, cb); break; } }
    (void)calls;
    if (cb == 256 && enter_down && !cancelled) static_cast<BYTE*>(data)[0x1C] = 0x80;
}
HRESULT WINAPI hook_state_a(void* self, DWORD cb, void* data) { HRESULT r = real_state_a(self, cb, data); if (r >= 0) inject(cb, data, "A"); return r; }
HRESULT WINAPI hook_state_w(void* self, DWORD cb, void* data) { HRESULT r = real_state_w(self, cb, data); if (r >= 0) inject(cb, data, "W"); return r; }

bool hook_dinput_variant(HMODULE dll, const GUID& iid, void* hook, void** real) {
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, void*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "DirectInput8Create"));
    if (!create) return false;
    void** di = nullptr;
    if (create(GetModuleHandleW(nullptr), 0x0800, iid, reinterpret_cast<void**>(&di), nullptr) < 0 || !di) return false;
    static const GUID sys_keyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    using CreateDeviceFn = HRESULT(WINAPI*)(void*, REFGUID, void***, void*);
    void** dev = nullptr;
    const HRESULT hr = reinterpret_cast<CreateDeviceFn>((*reinterpret_cast<void***>(di))[3])(di, sys_keyboard, &dev, nullptr);
    if (hr < 0 || !dev) { log("CreateDevice keyboard failed %08lx", hr); return false; }
    void** vtbl = *reinterpret_cast<void***>(dev);
    DWORD old;
    if (!VirtualProtect(&vtbl[9], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *real = vtbl[9];
    vtbl[9] = hook;
    VirtualProtect(&vtbl[9], sizeof(void*), old, &old);
    return true;
}

void hook_dinput() {
    HMODULE dll = nullptr;
    for (int i = 0; i < 100 && !(dll = GetModuleHandleW(L"dinput8.dll")); ++i) Sleep(100);
    if (!dll) { log("dinput8 never loaded"); return; }
    static const GUID iid_a = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    static const GUID iid_w = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    log("dinput8 keyboard GetDeviceState A %s", hook_dinput_variant(dll, iid_a, reinterpret_cast<void*>(&hook_state_a), reinterpret_cast<void**>(&real_state_a)) ? "hooked" : "failed");
    log("dinput8 keyboard GetDeviceState W %s", hook_dinput_variant(dll, iid_w, reinterpret_cast<void*>(&hook_state_w), reinterpret_cast<void**>(&real_state_w)) ? "hooked" : "failed");
}


// The launcher is a dialog inside acz.exe. SkipLauncher=True would skip it but
// also skips the device choice (the game then ignores the keyboard), so click
// "Launch Game" on it instead.
BOOL CALLBACK find_button(HWND h, LPARAM out) {
    wchar_t t[128] = {};
    GetWindowTextW(h, t, 128);
    if (wcsstr(t, L"Launch") || wcsstr(t, L"launch")) { *reinterpret_cast<HWND*>(out) = h; return FALSE; }
    return TRUE;
}
BOOL CALLBACK find_dialog(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    HWND btn = nullptr;
    EnumChildWindows(h, find_button, reinterpret_cast<LPARAM>(&btn));
    if (btn) { *reinterpret_cast<HWND*>(out) = btn; return FALSE; }
    return TRUE;
}
bool click_launcher() {
    for (int i = 0; i < 600 && !cancelled; ++i) {
        HWND btn = nullptr;
        EnumWindows(find_dialog, reinterpret_cast<LPARAM>(&btn));
        if (btn) {
            wchar_t t[128] = {};
            GetWindowTextW(btn, t, 128);
            log("launcher button '%ls' found", t);
            if (cfg.auto_launch) {
                Sleep(500);
                SendMessageW(btn, BM_CLICK, 0, 0);
                return true;
            }
            // Leave the dialog to the user (input device choice); continue once it closes.
            log("waiting for you to press Launch Game");
            for (;;) {
                Sleep(200);
                HWND again = nullptr;
                EnumWindows(find_dialog, reinterpret_cast<LPARAM>(&again));
                if (!again) return true;
            }
        }
        Sleep(100);
    }
    log("no launcher dialog found");
    return false;
}

DWORD WINAPI worker(LPVOID) {
    Sleep(1500);  // let the loader/Steam stub finish with the import table
    InitializeCriticalSection(&clock_lock);
    auto hook = [](const char* dll, const char* name, void* fn, void** real) {
        log("%s!%s %s", dll, name, patch_import(dll, name, fn, real) ? "hooked" : "NOT FOUND");
    };
    hook("KERNEL32.dll", "QueryPerformanceCounter", reinterpret_cast<void*>(&hook_qpc), reinterpret_cast<void**>(&real_qpc));
    hook("WINMM.dll", "timeGetTime", reinterpret_cast<void*>(&hook_time_get_time), reinterpret_cast<void**>(&real_time_get_time));
    hook("USER32.dll", "GetKeyState", reinterpret_cast<void*>(&hook_get_key_state), reinterpret_cast<void**>(&real_get_key_state));
    click_launcher();
    t0 = GetTickCount64();  // schedule runs from here
    log("schedule clock reset");
    hook_dinput();
    pulse_loop();
    log("pulse loop finished");
    return 0;
}

void look_configure();
void gate_configure();
BOOL CALLBACK start_once(PINIT_ONCE, PVOID, PVOID*) {
    t0 = GetTickCount64();
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    module_dir(dir, MAX_PATH);
    load_config(dir);
    swprintf(path, MAX_PATH, L"%sskip-intro.log", dir);
    log_file = _wfopen(path, L"w");
    log("skip-intro start=%g end=%g interval=%g hold=%g max=%d speed=%g until=%g", cfg.start, cfg.end, cfg.interval, cfg.hold, cfg.max_pulses, cfg.speed, cfg.speed_until);
    look_configure();
    gate_configure();
    CloseHandle(CreateThread(nullptr, 0, worker, nullptr, 0, nullptr));
    return TRUE;
}

// ---- look-drive right-stick injection (cfg.look_xinput; look_ipc.h) ------------------------------------------------------------
// Only compiled into the path when look_xinput=1. The acz-vr DLL (same process) publishes a small right-stick offset into a named
// shared-memory block; XInputGetState(0) adds it to the physical pad's right stick. Absent/stale/invalid data or the kill-switch file
// (the look-drive.stop file in the acz-vr folder of LOCALAPPDATA) means: no change at all. No game memory is touched.
acz::look_ipc::Override look_override;
SRWLOCK look_lock = SRWLOCK_INIT;
HANDLE look_map;
const acz::look_ipc::Block* look_view;
ULONGLONG look_open_try;      // last attempt to open the mapping (ms)
wchar_t look_stop_path[MAX_PATH];
ULONGLONG look_stop_check;
bool look_stop_state;
const char* look_logged_why = "";
unsigned look_log_lines;

void look_configure() {
    if (cfg.look_xinput) {
        look_override = acz::look_ipc::Override(cfg.look_xinput_max, cfg.look_xinput_stale_ms, cfg.look_xinput_deadzone);
        log("look-xinput: ON max=%d stale=%dms deadzone=%.3f (right-stick offset from acz-vr; kill switch look-drive.stop)", cfg.look_xinput_max, cfg.look_xinput_stale_ms, cfg.look_xinput_deadzone);
    }
}

void look_open_mapping(ULONGLONG now) {
    if (look_view || (look_open_try && now - look_open_try < 500)) return;
    look_open_try = now ? now : 1;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, acz::look_ipc::kMappingName);
    if (!m) return;
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(acz::look_ipc::Block));
    if (!v) { CloseHandle(m); return; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(v, &mbi, sizeof mbi) || mbi.RegionSize < sizeof(acz::look_ipc::Block)) { UnmapViewOfFile(v); CloseHandle(m); return; }
    look_map = m;
    look_view = static_cast<const acz::look_ipc::Block*>(v);
    log("look-xinput: shared block opened");
}

bool look_kill_file(ULONGLONG now) {
    if (!look_stop_path[0]) {
        wchar_t local[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (n && n < MAX_PATH) _snwprintf(look_stop_path, MAX_PATH, L"%s\\acz-vr\\look-drive.stop", local);
        else look_stop_path[0] = L'?';
    }
    if (look_stop_path[0] == L'?') return false;
    if (look_stop_check && now - look_stop_check < 200) return look_stop_state;
    look_stop_check = now ? now : 1;
    look_stop_state = GetFileAttributesW(look_stop_path) != INVALID_FILE_ATTRIBUTES;
    return look_stop_state;
}

// state: the XINPUT_STATE the real XInputGetState filled (result 0 = connected). Only user index 0 (Player 1) is touched.
void look_inject(UINT_PTR user, UINT_PTR state, DWORD result) {
    if (!cfg.look_xinput || user != 0 || result != 0 || !state) return;
    auto* st = reinterpret_cast<XINPUT_STATE*>(state);
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&look_lock);
    look_open_mapping(now);
    const bool kill = look_kill_file(now);
    const auto res = look_override.apply(st->Gamepad.sThumbRX, st->Gamepad.sThumbRY, look_view, now, kill);
    if (res.injected) {
        st->Gamepad.sThumbRX = res.rx;
        st->Gamepad.sThumbRY = res.ry;
    }
    st->dwPacketNumber += res.packet_bump;   // an injected change must look like a new input packet
    if (strcmp(res.why, look_logged_why) && look_log_lines < 200) {   // log transitions only (bounded)
        look_logged_why = res.why;
        ++look_log_lines;
        log("look-xinput: %s (injected=%llu passed=%llu)", res.why, (unsigned long long)look_override.injected(), (unsigned long long)look_override.passed());
    }
    ReleaseSRWLockExclusive(&look_lock);
}

// ---- LOOK-STICK GATE (cfg.xr_block_look_stick, default 1; xr_view_ipc.h) ----------------------------------------------------------------
// acz-vr publishes a heartbeat (named mapping Local\acz-vr-xr-view-v2-<pid>, this process only) while its VR composite draws the world from the game camera (in flight).
// While it is fresh, XInputGetState(0) reports the RIGHT STICK centred: the game's cockpit look-around (which turns the game camera, and so the
// whole VR world, around our head-tracked cockpit) is not driven. Buttons, triggers, the left stick and other users are untouched. Absent / stale /
// invalid heartbeat = the real stick passes through. No game memory is touched.
acz::xr_view_ipc::StickGate gate{acz::xr_view_ipc::kDefaultStaleMs, 0};   // owner 0 never gates; gate_configure binds this process id
SRWLOCK gate_lock = SRWLOCK_INIT;
const acz::xr_view_ipc::Block* gate_view;
ULONGLONG gate_open_try;
const char* gate_logged_why = "";
unsigned gate_log_lines;

void gate_configure() {
    gate = acz::xr_view_ipc::StickGate(uint32_t(cfg.xr_block_look_stick_stale_ms), GetCurrentProcessId());
    log("look-stick-gate: %s stale=%dms (right stick centred for the game while acz-vr composes in-flight VR frames)",
        cfg.xr_block_look_stick ? "ON" : "OFF(xr_block_look_stick=0)", cfg.xr_block_look_stick_stale_ms);
}

void gate_open_mapping(ULONGLONG now) {
    if (gate_view || (gate_open_try && now - gate_open_try < 500)) return;
    gate_open_try = now ? now : 1;
    // D-358: this game process's own heartbeat only (acz-vr runs in the same process and names the mapping by its process id).
    wchar_t name[acz::xr_view_ipc::kMappingNameMax];
    if (!acz::xr_view_ipc::mapping_name(name, sizeof name / sizeof name[0], GetCurrentProcessId())) return;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!m) return;
    void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(acz::xr_view_ipc::Block));
    if (!v) { CloseHandle(m); return; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(v, &mbi, sizeof mbi) || mbi.RegionSize < sizeof(acz::xr_view_ipc::Block)) { UnmapViewOfFile(v); CloseHandle(m); return; }
    gate_view = static_cast<const acz::xr_view_ipc::Block*>(v);   // the handle stays open with the view for the process lifetime
    log("look-stick-gate: acz-vr heartbeat block opened");
}

// Runs BEFORE look_inject, so a (parked) look-drive offset would still be added to the centred stick.
void gate_apply(UINT_PTR user, UINT_PTR state, DWORD result) {
    if (!cfg.xr_block_look_stick || user != 0 || result != 0 || !state) return;
    auto* st = reinterpret_cast<XINPUT_STATE*>(state);
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&gate_lock);
    gate_open_mapping(now);
    const auto res = gate.apply(st->Gamepad.sThumbRX, st->Gamepad.sThumbRY, gate_view, now, true);
    st->Gamepad.sThumbRX = res.rx;
    st->Gamepad.sThumbRY = res.ry;
    st->dwPacketNumber += res.packet_bump;   // the reported stick changed without a new pad packet when the gate opened/closed
    if (strcmp(res.why, gate_logged_why) && gate_log_lines < 200) {   // transitions only (bounded)
        gate_logged_why = res.why;
        ++gate_log_lines;
        log("look-stick-gate: %s (deflected polls centred so far=%llu)", res.why, (unsigned long long)gate.blocked());
    }
    ReleaseSRWLockExclusive(&gate_lock);
}

// The game imports XInput1_4 by ordinal (2 = XInputGetState, 3 = XInputSetState). Forward every ordinal with all four
// register arguments so no signature is truncated, whatever the game calls.
constexpr int kOrdinals[] = {2, 3, 4, 5, 7, 8, 10, 100, 101, 102, 103, 104, 108, 109};
FARPROC real_ordinal[110];

void resolve_system_xinput() {
    if (system_xinput) return;
    wchar_t path[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 16) return;
    lstrcatW(path, L"\\XInput1_4.dll");
    system_xinput = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!system_xinput) return;
    for (int o : kOrdinals) real_ordinal[o] = GetProcAddress(system_xinput, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(o)));
}

using Fwd = UINT_PTR(WINAPI*)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);
UINT_PTR forward(int ordinal, UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) {
    InitOnceExecuteOnce(&started, start_once, nullptr, nullptr);
    resolve_system_xinput();
    if (!real_ordinal[ordinal]) return ERROR_DEVICE_NOT_CONNECTED;
    const UINT_PTR r = reinterpret_cast<Fwd>(real_ordinal[ordinal])(a, b, c, d);
    if (ordinal == 2) {  // XInputGetState: log connection and button changes only
        static DWORD last_buttons = 0xFFFFFFFF;
        static DWORD last_result = 0xFFFFFFFF;
        const DWORD res = DWORD(r);
        const DWORD btn = (res == 0 && b) ? reinterpret_cast<XINPUT_STATE*>(b)->Gamepad.wButtons : 0;
        if (res != last_result || btn != last_buttons) { log("XInputGetState(%u) -> %lu buttons=0x%04lx", unsigned(a), res, btn); last_result = res; last_buttons = btn; }
    }
    if (cfg.xr_block_look_stick && (ordinal == 2 || ordinal == 100)) gate_apply(a, b, DWORD(r));  // LOOK-STICK GATE (before the look drive)
    if (cfg.look_xinput && (ordinal == 2 || ordinal == 100)) look_inject(a, b, DWORD(r));  // XInputGetState / XInputGetStateEx (same layout)
    return r;
}

} // namespace

extern "C" UINT_PTR WINAPI xi_2(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(2, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_3(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(3, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_4(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(4, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_5(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(5, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_7(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(7, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_8(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(8, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_10(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(10, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_100(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(100, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_101(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(101, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_102(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(102, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_103(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(103, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_104(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(104, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_108(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(108, a, b, c, d); }
extern "C" UINT_PTR WINAPI xi_109(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) { return forward(109, a, b, c, d); }

DWORD WINAPI early_start(LPVOID) { InitOnceExecuteOnce(&started, start_once, nullptr, nullptr); return 0; }

// Keyboard mode never polls XInput, so start from our own load instead.
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) CloseHandle(CreateThread(nullptr, 0, early_start, nullptr, 0, nullptr));
    return TRUE;
}
