// LAUNCH-MODE (docs/world-render/launch-mode.md): the "VR Mode: On / Off" combo in acz.exe's launcher window and the factory gate.
//
// The launcher (window title "ACE COMBAT ZERO THE BELKAN WAR") is a plain Win32 window built by acz.exe itself: CreateWindowExW children of
// class "ComboBox" and "Button" over a GDI-painted background (acz.exe imports CreateWindowExW, StretchBlt, ExtTextOutW, DrawThemeBackground,
// comctl32 #410/#413 = SetWindowSubclass/DefSubclassProc, GetMessageA/DispatchMessageA; no dialog resource). Our dxgi.dll is a static import
// of acz.exe, so it is loaded before the launcher appears, but nothing in it runs until the first CreateDXGIFactory* call, which comes ~0.4 s
// AFTER the Launch Game click (2026-11-01 user session: setting.ini written 09:04:39.77, `armed` at ~09:04:40.2).
//
// Threads:
//   * watcher (created in DllMain, never touches the loader lock afterwards): EnumWindows every 50 ms until the launcher appears, then a
//     WH_GETMESSAGE hook on the launcher's UI thread runs inject() ONCE inside that thread's own GetMessage (no cross-thread window creation,
//     no AttachThreadInput, no SendMessage from the UI thread to us). The hook is removed right after.
//   * launcher UI thread: inject(), the subclass procedures, the selection write (a few hundred bytes to launch-mode.txt), the label paint.
//     Nothing here waits on another thread.
//   * any thread: vr_enabled() (the first CreateDXGIFactory*/early-device call latches the mode; reads the window state with
//     IsWindowVisible/GetWindowRect only: these never send messages).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include "launch_mode_win.h"
#include "launch_mode.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace acz::launch_mode {
namespace {

using SetSubclassFn = BOOL(WINAPI*)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
using RemoveSubclassFn = BOOL(WINAPI*)(HWND, SUBCLASSPROC, UINT_PTR);
using DefSubclassFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
SetSubclassFn g_set = nullptr;
RemoveSubclassFn g_remove = nullptr;
DefSubclassFn g_def = nullptr;

constexpr UINT_PTR kSubclassId = 0x41435A56;   // 'ACZV'
constexpr int kComboId = 0x7A1;
constexpr wchar_t kLabel[] = L"VR Mode";

// ------------------------------------------------------------------------------------------------ shared state
std::atomic<int> g_latched{-1};          // -1 undecided, 0 on, 1 off
std::atomic<int> g_selection{-1};        // -1 no combo, 0 on, 1 off (the combo's value)
std::atomic<bool> g_launch_pressed{false};
std::atomic<bool> g_ui_alive{false};
std::atomic<HWND> g_top{nullptr}, g_button{nullptr}, g_combo{nullptr};
std::atomic<long> g_rect_w{0}, g_rect_h{0};   // launcher window size at injection
HINSTANCE g_self = nullptr;
std::atomic<bool> g_inject_ran{false}, g_inject_done{false};
SRWLOCK g_decide_lock = SRWLOCK_INIT;
SRWLOCK g_log_lock = SRWLOCK_INIT;
bool g_log_started = false;              // under g_log_lock
unsigned g_provisional_logged = 0;       // under g_decide_lock
UINT g_remove_msg = 0;
// UI-thread-only (set in inject(), read by the subclass procedures on the same thread)
HFONT g_font = nullptr;
RECT g_label{};
RECT g_client_at_inject{};
bool g_button_subclassed = false, g_combo_subclassed = false, g_parent_subclassed = false;

// ------------------------------------------------------------------------------------------------ paths, file, log
std::wstring base_dir() {
    wchar_t local[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::wstring(local) + L"\\acz-vr";
}
std::wstring file_path() { const auto b = base_dir(); return b.empty() ? b : b + L"\\launch-mode.txt"; }

void log_line(const char* text) noexcept {
    try {
        const auto b = base_dir();
        if (b.empty()) return;
        CreateDirectoryW(b.c_str(), nullptr);
        const std::wstring dir = b + L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring p = dir + L"\\launch-mode.log";
        AcquireSRWLockExclusive(&g_log_lock);
        // One file per process: truncated by the first line of this process (bounded size), appended afterwards. Shared access for readers.
        HANDLE f = CreateFileW(p.c_str(), FILE_APPEND_DATA | (g_log_started ? 0 : GENERIC_WRITE), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, g_log_started ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            g_log_started = true;
            char line[600];
            const int len = std::snprintf(line, sizeof line, "%llu|launch-mode;%s\r\n", static_cast<unsigned long long>(GetTickCount64()), text);
            if (len > 0) {
                DWORD wrote = 0;
                SetFilePointer(f, 0, nullptr, FILE_END);
                WriteFile(f, line, static_cast<DWORD>(len < int(sizeof line) ? len : int(sizeof line) - 1), &wrote, nullptr);
            }
            CloseHandle(f);
        }
        ReleaseSRWLockExclusive(&g_log_lock);
    } catch (...) {}
}

FileValue read_file() noexcept {
    FileValue v;
    try {
        const auto p = file_path();
        if (p.empty()) { v.state = FileState::unreadable; return v; }
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            v.state = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? FileState::missing : FileState::unreadable;
            return v;
        }
        char buf[4096];
        DWORD got = 0;
        const BOOL ok = ReadFile(f, buf, sizeof buf, &got, nullptr);
        CloseHandle(f);
        if (!ok) { v.state = FileState::unreadable; return v; }
        return parse_file(std::string(buf, got));
    } catch (...) { v.state = FileState::unreadable; return v; }
}

// Temp file + MoveFileEx(REPLACE_EXISTING): a reader never sees a half-written file. Returns the Win32 error (0 = ok).
DWORD write_file(VrMode m) noexcept {
    try {
        const auto b = base_dir();
        if (b.empty()) return ERROR_PATH_NOT_FOUND;
        CreateDirectoryW(b.c_str(), nullptr);
        const std::wstring p = b + L"\\launch-mode.txt", tmp = p + L".tmp";
        const std::string text = format_file(m);
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return GetLastError();
        DWORD wrote = 0;
        const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
        const DWORD werr = ok ? 0 : GetLastError();
        CloseHandle(f);
        if (!ok) { DeleteFileW(tmp.c_str()); return werr ? werr : ERROR_WRITE_FAULT; }
        if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { const DWORD e = GetLastError(); DeleteFileW(tmp.c_str()); return e; }
        return 0;
    } catch (...) { return ERROR_OUTOFMEMORY; }
}

// ------------------------------------------------------------------------------------------------ the launcher state (any thread)
bool launcher_showing() noexcept {
    const HWND top = g_top.load(std::memory_order_acquire);
    if (!top || !g_ui_alive.load(std::memory_order_acquire) || g_launch_pressed.load(std::memory_order_acquire)) return false;
    if (!IsWindow(top) || !IsWindowVisible(top)) return false;
    RECT r{};
    if (!GetWindowRect(top, &r)) return false;
    // Size only: the user may move the launcher; Launch Game grows the same window to the game size.
    if (r.right - r.left != g_rect_w.load() || r.bottom - r.top != g_rect_h.load()) return false;
    const HWND btn = g_button.load(std::memory_order_acquire);
    return btn && IsWindow(btn) && IsWindowVisible(btn);
}

// ------------------------------------------------------------------------------------------------ UI thread: removal, paint, subclasses
LRESULT CALLBACK parent_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK button_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK combo_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void remove_ui(const char* why) noexcept {
    if (!g_ui_alive.exchange(false, std::memory_order_acq_rel)) return;
    const HWND top = g_top.load(), btn = g_button.load(), combo = g_combo.exchange(nullptr);
    if (g_combo_subclassed && combo && IsWindow(combo)) g_remove(combo, combo_proc, kSubclassId);
    g_combo_subclassed = false;
    if (combo && IsWindow(combo)) DestroyWindow(combo);
    if (g_button_subclassed && btn && IsWindow(btn)) g_remove(btn, button_proc, kSubclassId);
    g_button_subclassed = false;
    if (g_parent_subclassed && top && IsWindow(top)) {
        g_remove(top, parent_proc, kSubclassId);   // legal from inside parent_proc (comctl32 defers the free)
        InvalidateRect(top, &g_label, TRUE);       // the launcher repaints its background over our label (harmless on the game window)
    }
    g_parent_subclassed = false;
    char t[160];
    std::snprintf(t, sizeof t, "event=ui-removed;why=%s;launch-pressed=%u;latched=%d", why, g_launch_pressed.load() ? 1u : 0u, g_latched.load());
    log_line(t);
}

void mark_launch(const char* via) noexcept {
    if (g_launch_pressed.exchange(true)) return;
    char t[160];
    const int s = g_selection.load();
    std::snprintf(t, sizeof t, "event=launch-pressed;via=%s;selection=%s;latched=%d", via, s < 0 ? "none" : s == 1 ? "off" : "on", g_latched.load());
    log_line(t);
}

void on_select(HWND combo) noexcept {
    const LRESULT idx = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (idx != 0 && idx != 1) return;
    const VrMode m = idx == 1 ? VrMode::off : VrMode::on;
    const int prev = g_selection.exchange(m == VrMode::off ? 1 : 0);
    if (prev == (m == VrMode::off ? 1 : 0)) return;
    const DWORD err = write_file(m);
    const int latched = g_latched.load();
    char t[200];
    std::snprintf(t, sizeof t, "event=selection;vr=%s;file-write=%s;error=%lu;applies=%s", mode_name(m), err ? "failed" : "ok", static_cast<unsigned long>(err),
        latched < 0 ? "this-launch" : "next-launch(mode-latched-earlier)");
    log_line(t);
}

void draw_label_dc(HDC dc) noexcept {
    const int saved = SaveDC(dc);
    if (g_font) SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 235));   // the launcher's labels: light grey on the dark red background (step75 launcher.png)
    RECT r = g_label;
    r.bottom -= 3;                          // the launcher's labels sit ~3 px above their combo
    DrawTextW(dc, kLabel, -1, &r, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    if (saved) RestoreDC(dc, saved);
}

void draw_label(HWND top, const RECT& clip) noexcept {
    if (!g_ui_alive.load()) return;
    HDC dc = GetDC(top);
    if (!dc) return;
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    draw_label_dc(dc);
    ReleaseDC(top, dc);
}

LRESULT CALLBACK parent_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) && reinterpret_cast<HWND>(lp) == g_combo.load()) {
            // Our own control: never forwarded (the launcher would see an unknown control id).
            const WORD code = HIWORD(wp);
            if (code == CBN_SELCHANGE) on_select(reinterpret_cast<HWND>(lp));
            if (code == CBN_SELCHANGE || code == CBN_CLOSEUP || code == CBN_SETFOCUS || code == CBN_KILLFOCUS) InvalidateRect(reinterpret_cast<HWND>(lp), nullptr, FALSE);
            return 0;
        }
        if (HIWORD(wp) == BN_CLICKED && reinterpret_cast<HWND>(lp) && reinterpret_cast<HWND>(lp) == g_button.load()) mark_launch("bn-clicked");
        break;
    case WM_PAINT: {
        // Only where the launcher has just repainted its background (a partial repaint elsewhere must not draw the text twice).
        RECT upd{}, hit{};
        const BOOL has = GetUpdateRect(h, &upd, FALSE);
        const LRESULT r = g_def(h, msg, wp, lp);
        if (has && IntersectRect(&hit, &upd, &g_label)) draw_label(h, hit);
        return r;
    }
    case WM_PRINTCLIENT: {   // PrintWindow / thumbnails: the label goes into the given DC as well
        const LRESULT r = g_def(h, msg, wp, lp);
        if (g_ui_alive.load() && wp) draw_label_dc(reinterpret_cast<HDC>(wp));
        return r;
    }
    case WM_WINDOWPOSCHANGED: {
        const LRESULT r = g_def(h, msg, wp, lp);
        RECT c{};
        if (g_ui_alive.load() && GetClientRect(h, &c) && (c.right != g_client_at_inject.right || c.bottom != g_client_at_inject.bottom)) remove_ui("launcher-resized");
        return r;
    }
    case WM_NCDESTROY:
        remove_ui("launcher-destroyed");
        g_remove(h, parent_proc, kSubclassId);
        return g_def(h, msg, wp, lp);
    default:
        if (msg == g_remove_msg && g_remove_msg) {
            if (g_launch_pressed.load()) remove_ui("mode-latched-after-launch");
            return 0;
        }
        break;
    }
    return g_def(h, msg, wp, lp);
}

LRESULT CALLBACK button_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_LBUTTONUP: {
        RECT c{};
        const POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (GetClientRect(h, &c) && PtInRect(&c, p)) mark_launch("button-up");
        break;
    }
    case BM_CLICK: mark_launch("bm-click"); break;
    case WM_KEYDOWN: if (wp == VK_RETURN) mark_launch("key-return"); break;
    case WM_KEYUP: if (wp == VK_SPACE) mark_launch("key-space"); break;
    case WM_SHOWWINDOW:
        if (!wp) { const LRESULT r = g_def(h, msg, wp, lp); remove_ui("launch-button-hidden"); return r; }
        break;
    case WM_NCDESTROY: {
        g_remove(h, button_proc, kSubclassId);
        g_button_subclassed = false;
        const LRESULT r = g_def(h, msg, wp, lp);
        remove_ui("launch-button-destroyed");
        return r;
    }
    default: break;
    }
    return g_def(h, msg, wp, lp);
}

// The launcher draws its combos itself (white field, thin grey frame, small chevron; step75 launcher.png). Our closed field is painted the
// same way; the drop-down list stays the standard list. Any paint failure leaves the standard combo look.
void paint_combo(HWND h, HDC dc) noexcept {
    const int saved = SaveDC(dc);
    RECT rc{};
    GetClientRect(h, &rc);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HBRUSH frame = CreateSolidBrush(RGB(122, 122, 122));
    if (frame) { FrameRect(dc, &rc, frame); DeleteObject(frame); }
    wchar_t text[16]{};
    const LRESULT idx = SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (idx >= 0 && SendMessageW(h, CB_GETLBTEXTLEN, static_cast<WPARAM>(idx), 0) < 15) SendMessageW(h, CB_GETLBTEXT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(text));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0));
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    RECT tr = rc;
    tr.left += 4;
    tr.right -= 18;
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(110, 110, 110));
    if (pen) {
        HGDIOBJ op = SelectObject(dc, pen);
        const int cx = rc.right - 10, cy = (rc.top + rc.bottom) / 2;
        MoveToEx(dc, cx - 3, cy - 1, nullptr);
        LineTo(dc, cx, cy + 2);
        LineTo(dc, cx + 4, cy - 2);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    if (GetFocus() == h && !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) {
        RECT fr = tr;
        InflateRect(&fr, 1, -3);
        DrawFocusRect(dc, &fr);
    }
    if (saved) RestoreDC(dc, saved);
}

LRESULT CALLBACK combo_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) paint_combo(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT && wp) { paint_combo(h, reinterpret_cast<HDC>(wp)); return 0; }
    if (msg == WM_NCDESTROY) { g_remove(h, combo_proc, kSubclassId); g_combo_subclassed = false; }
    return g_def(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ UI thread: injection
struct Children {
    HWND top = nullptr;
    LayoutInput in;
    HWND combo_hwnd[16]{};
    int ncombo = 0;
};
BOOL CALLBACK collect(HWND h, LPARAM p) {
    auto& c = *reinterpret_cast<Children*>(p);
    if (GetParent(h) != c.top || !IsWindowVisible(h)) return TRUE;
    wchar_t cls[32]{};
    GetClassNameW(h, cls, 32);
    RECT r{};
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, c.top, reinterpret_cast<POINT*>(&r), 2);
    const Rect rr{r.left, r.top, r.right, r.bottom};
    if (_wcsicmp(cls, L"ComboBox") == 0 && c.ncombo < 16) { c.in.combos.push_back(rr); c.combo_hwnd[c.ncombo++] = h; }
    else c.in.others.push_back(rr);
    return TRUE;
}

void inject() noexcept {
    char t[420];
    try {
        const HWND top = g_top.load(), btn = g_button.load();
        if (!top || !IsWindow(top) || !btn || !IsWindow(btn)) { log_line("event=inject;outcome=refused;why=launcher-gone"); return; }
        HMODULE cc = GetModuleHandleW(L"comctl32.dll");   // acz.exe imports it (#410/#413/#17): already loaded
        if (!cc) cc = LoadLibraryW(L"comctl32.dll");       // (a host without it; UI thread, outside the loader lock)
        if (cc) {
            g_set = reinterpret_cast<SetSubclassFn>(GetProcAddress(cc, MAKEINTRESOURCEA(410)));
            g_remove = reinterpret_cast<RemoveSubclassFn>(GetProcAddress(cc, MAKEINTRESOURCEA(412)));
            g_def = reinterpret_cast<DefSubclassFn>(GetProcAddress(cc, MAKEINTRESOURCEA(413)));
        }
        if (!g_set || !g_remove || !g_def) { log_line("event=inject;outcome=refused;why=no-comctl32-subclass(410/412/413)"); return; }
        Children ch;
        ch.top = top;
        EnumChildWindows(top, collect, reinterpret_cast<LPARAM>(&ch));
        RECT cr{};
        GetClientRect(top, &cr);
        ch.in.client = Rect{cr.left, cr.top, cr.right, cr.bottom};
        const Layout L = place(ch.in);
        if (!L.ok) {
            std::snprintf(t, sizeof t, "event=inject;outcome=refused;why=layout:%s;combos=%d;others=%zu;client=%ldx%ld;fallback=launch-mode.txt(tools/acz-vr-mode.ps1)",
                L.why, ch.ncombo, ch.in.others.size(), cr.right, cr.bottom);
            log_line(t);
            return;
        }
        // The reference combo: the left column's last one (same rect as the layout used).
        HWND ref = nullptr;
        for (int i = 0; i < ch.ncombo; ++i)
            if (ch.in.combos[size_t(i)].l == L.combo.l && ch.in.combos[size_t(i)].t == L.combo.t - L.pitch) ref = ch.combo_hwnd[i];
        if (!ref) ref = ch.combo_hwnd[0];
        g_font = reinterpret_cast<HFONT>(SendMessageW(ref, WM_GETFONT, 0, 0));
        LRESULT ih = SendMessageW(ref, CB_GETITEMHEIGHT, 0, 0);
        if (ih <= 0 || ih > 200) ih = L.combo.h();
        const int dropped = L.combo.h() + 2 * static_cast<int>(ih) + 8;
        HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(top, GWLP_HINSTANCE));
        // WS_EX_NOPARENTNOTIFY: the launcher's own window procedure never hears of our control (no WM_PARENTNOTIFY on create/destroy).
        HWND combo = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"ComboBox", kLabel, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            L.combo.l, L.combo.t, L.combo.w(), dropped, top, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboId)), inst, nullptr);
        if (!combo) {
            std::snprintf(t, sizeof t, "event=inject;outcome=refused;why=create-combo-failed;error=%lu", GetLastError());
            log_line(t);
            return;
        }
        if (g_font) SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"On"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
        const FileValue fv = read_file();
        const VrMode initial = fv.state == FileState::ok ? fv.mode : VrMode::on;
        SendMessageW(combo, CB_SETCURSEL, initial == VrMode::off ? 1 : 0, 0);
        g_label = RECT{L.label.l, L.label.t, L.label.r, L.label.b};
        g_client_at_inject = cr;
        RECT wr{};
        GetWindowRect(top, &wr);
        g_rect_w = wr.right - wr.left; g_rect_h = wr.bottom - wr.top;
        g_combo.store(combo, std::memory_order_release);
        g_selection.store(initial == VrMode::off ? 1 : 0, std::memory_order_release);
        g_combo_subclassed = g_set(combo, combo_proc, kSubclassId, 0) != FALSE;
        g_button_subclassed = g_set(btn, button_proc, kSubclassId, 0) != FALSE;
        g_parent_subclassed = g_set(top, parent_proc, kSubclassId, 0) != FALSE;
        if (!g_parent_subclassed) {
            // Without the parent subclass we cannot see our combo's notifications or remove it at launch: undo everything.
            if (g_combo_subclassed) g_remove(combo, combo_proc, kSubclassId);
            if (g_button_subclassed) g_remove(btn, button_proc, kSubclassId);
            g_combo_subclassed = g_button_subclassed = false;
            g_combo.store(nullptr);
            g_selection.store(-1);
            DestroyWindow(combo);
            log_line("event=inject;outcome=refused;why=parent-subclass-failed;fallback=launch-mode.txt");
            return;
        }
        g_ui_alive.store(true, std::memory_order_release);
        InvalidateRect(top, &g_label, TRUE);
        std::snprintf(t, sizeof t, "event=inject;outcome=ok;combo=%d,%d,%dx%d;label=%d,%d,%dx%d;pitch=%d;combos=%d;font=%s;initial=%s;file=%s;"
            "button-subclass=%u;combo-paint=%s;client=%ldx%ld",
            L.combo.l, L.combo.t, L.combo.w(), L.combo.h(), L.label.l, L.label.t, L.label.w(), L.label.h(), L.pitch, ch.ncombo, g_font ? "launcher-combo" : "system",
            mode_name(initial), file_state_name(fv.state), g_button_subclassed ? 1u : 0u, g_combo_subclassed ? "launcher-style" : "standard", cr.right, cr.bottom);
        log_line(t);
    } catch (...) {
        log_line("event=inject;outcome=refused;why=exception");
    }
}

LRESULT CALLBACK get_msg_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && !g_inject_ran.exchange(true)) {
        inject();
        g_inject_done.store(true, std::memory_order_release);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// ------------------------------------------------------------------------------------------------ watcher thread
struct Found { HWND top = nullptr, button = nullptr; int combos = 0; };
BOOL CALLBACK find_child(HWND h, LPARAM p) {
    auto& f = *reinterpret_cast<Found*>(p);
    if (GetParent(h) != f.top) return TRUE;
    wchar_t cls[32]{};
    GetClassNameW(h, cls, 32);
    if (_wcsicmp(cls, L"ComboBox") == 0) { ++f.combos; return TRUE; }
    if (_wcsicmp(cls, L"Button") != 0 || f.button) return TRUE;
    wchar_t text[64]{};
    DWORD_PTR got = 0;
    // WM_GETTEXT is answered by the launcher's thread inside its GetMessage; bounded so a busy launcher never holds us.
    if (SendMessageTimeoutW(h, WM_GETTEXT, 63, reinterpret_cast<LPARAM>(text), SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &got) && std::wcscmp(text, L"Launch Game") == 0)
        f.button = h;
    return TRUE;
}
BOOL CALLBACK find_top(HWND h, LPARAM p) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    Found f;
    f.top = h;
    EnumChildWindows(h, find_child, reinterpret_cast<LPARAM>(&f));
    if (f.button && f.combos >= 2) { *reinterpret_cast<Found*>(p) = f; return FALSE; }
    return TRUE;
}

DWORD WINAPI watch(LPVOID) {
    char t[300];
    const FileValue fv = read_file();
    std::snprintf(t, sizeof t, "event=process;pid=%lu;file=%s;file-mode=%s;default=on;launcher-option=on", GetCurrentProcessId(), file_state_name(fv.state),
        fv.state == FileState::ok ? mode_name(fv.mode) : "-");
    log_line(t);
    const ULONGLONG t0 = GetTickCount64();
    Found f;
    for (;;) {
        if (g_latched.load() >= 0) {
            std::snprintf(t, sizeof t, "event=launcher-not-seen;why=mode-latched-first(SkipLauncher-or-no-launcher);waited-ms=%llu", GetTickCount64() - t0);
            log_line(t);
            return 0;
        }
        if (GetTickCount64() - t0 > 30ull * 60 * 1000) { log_line("event=launcher-not-seen;why=timeout-30min"); return 0; }
        f = Found{};
        EnumWindows(find_top, reinterpret_cast<LPARAM>(&f));
        if (f.top) break;
        Sleep(50);
    }
    const DWORD tid = GetWindowThreadProcessId(f.top, nullptr);
    std::snprintf(t, sizeof t, "event=launcher-found;waited-ms=%llu;ui-thread=%lu;combos=%d", GetTickCount64() - t0, tid, f.combos);
    log_line(t);
    g_top.store(f.top, std::memory_order_release);
    g_button.store(f.button, std::memory_order_release);
    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, get_msg_hook, nullptr, tid);   // a thread of this process: hMod NULL (SetWindowsHookEx docs)
    if (!hook && GetLastError() == ERROR_HOOK_NEEDS_HMOD && g_self) hook = SetWindowsHookExW(WH_GETMESSAGE, get_msg_hook, g_self, tid);
    if (!hook) {
        std::snprintf(t, sizeof t, "event=inject;outcome=refused;why=hook-failed;error=%lu;fallback=launch-mode.txt", GetLastError());
        log_line(t);
        return 0;
    }
    for (int i = 0; i < 300 && !g_inject_done.load(std::memory_order_acquire); ++i) {   // <= 3 s
        if (i % 50 == 0) PostMessageW(f.top, WM_NULL, 0, 0);   // wakes GetMessage so the hook runs
        Sleep(10);
    }
    UnhookWindowsHookEx(hook);
    if (!g_inject_done.load()) log_line("event=inject;outcome=refused;why=ui-thread-did-not-pump-in-3s;fallback=launch-mode.txt");
    return 0;
}

bool host_is_acz() noexcept {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"acz.exe") == 0;
}

} // namespace

void start_launcher_watch(HINSTANCE self) noexcept {
    // Loader lock: only cheap checks and CreateThread (the thread runs after the lock is released).
    if (!host_is_acz()) return;
    g_self = self;
    wchar_t env[8]{};
    const DWORD n = GetEnvironmentVariableW(L"ACZ_VR_LAUNCHER_OPTION", env, 8);
    if (n && n < 8 && (env[0] == L'0' || env[0] == L'n' || env[0] == L'N' || ((env[0] == L'o' || env[0] == L'O') && (env[1] == L'f' || env[1] == L'F')))) return;
    g_remove_msg = RegisterWindowMessageW(L"AczVrLaunchModeRemove");
    HANDLE th = CreateThread(nullptr, 64 * 1024, watch, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (th) CloseHandle(th);
}

bool vr_enabled() noexcept {
    const int v = g_latched.load(std::memory_order_acquire);
    if (v >= 0) return v == 0;
    static const bool in_game = host_is_acz();   // tools/probes that load this DLL (xr_probe, tests) are never gated by the user's file
    if (!in_game) return true;
    {
        AcquireSRWLockExclusive(&g_decide_lock);
        int now = g_latched.load(std::memory_order_acquire);
        if (now >= 0) { ReleaseSRWLockExclusive(&g_decide_lock); return now == 0; }
        DecideInput in;
        in.launcher_showing = launcher_showing();
        const int sel = g_selection.load(std::memory_order_acquire);
        in.has_selection = sel >= 0;
        in.selection = sel == 1 ? VrMode::off : VrMode::on;
        if (!in.has_selection) in.file = read_file();
        const Decision d = decide(in);
        char t[300];
        if (d.latch) {
            g_latched.store(d.mode == VrMode::off ? 1 : 0, std::memory_order_release);
            std::snprintf(t, sizeof t, "event=latched;vr=%s;source=%s;file=%s;launcher=%s;launch-pressed=%u;effect=%s", mode_name(d.mode), source_name(d.source),
                in.has_selection ? "-" : file_state_name(in.file.state), g_top.load() ? (in.launcher_showing ? "showing" : "gone") : "not-seen",
                g_launch_pressed.load() ? 1u : 0u,
                d.mode == VrMode::off ? "proxies-forward-only(no-factory-wrap,no-hooks,no-xr)" : "runtime.cfg-as-is");
            log_line(t);
            const HWND top = g_top.load();
            if (top && g_ui_alive.load() && g_remove_msg) PostMessageW(top, g_remove_msg, 0, 0);
        } else if (g_provisional_logged++ < 4) {
            std::snprintf(t, sizeof t, "event=provisional-pass-through;vr=off;source=%s;why=launcher-still-showing(decided-again-at-the-next-call)", source_name(d.source));
            log_line(t);
        }
        ReleaseSRWLockExclusive(&g_decide_lock);
        return d.mode == VrMode::on;
    }
}

} // namespace acz::launch_mode
