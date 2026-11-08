// LAUNCH-MODE manual harness (docs/world-render/launch-mode.md 6): a FAKE acz.exe launcher (ANSI top-level window titled like the game,
// "ComboBox"/"Button" children at the measured launcher positions, GetMessageA loop) that loads the BUILT dxgi.dll proxy, lets its watcher
// inject the "VR Mode" combo, optionally selects Off, presses Launch Game (BM_CLICK -> WM_COMMAND, where it calls CreateDXGIFactory2 like the
// game, then grows the window and destroys the launcher children) and reports what happened. Not in ctest (it shows a window on the
// desktop). Build target launch_mode_harness (output acz.exe: the proxy only acts inside acz.exe). Run with LOCALAPPDATA pointed at a
// scratch folder so launch-mode.txt / launch-mode.log / runtime.cfg of the real user are never touched:
//   set LOCALAPPDATA=D:\scratch & acz.exe <path-to-dxgi.dll> <on|off|keep|early-off> <shot.bmp>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
HMODULE g_proxy = nullptr;
HWND g_top = nullptr, g_btn = nullptr, g_combos[4]{};
HFONT g_font = nullptr;
std::string g_action;
const char* g_shot = nullptr;
int g_result = 0;
constexpr int kOurId = 0x7A1;

using Factory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
bool vtable_in_proxy(void* obj) {
    if (!obj || !g_proxy) return false;
    const auto base = reinterpret_cast<uintptr_t>(g_proxy);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_proxy);
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(obj);
    return vt >= base && vt < base + nt->OptionalHeader.SizeOfImage;
}
int call_factory(const char* when) {
    auto f = reinterpret_cast<Factory2>(GetProcAddress(g_proxy, "CreateDXGIFactory2"));
    IDXGIFactory2* fac = nullptr;
    const HRESULT hr = f ? f(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac)) : E_FAIL;
    const bool wrapped = SUCCEEDED(hr) && vtable_in_proxy(fac);
    std::printf("factory(%s): hr=0x%08lx wrapped=%d\n", when, static_cast<unsigned long>(hr), wrapped ? 1 : 0);
    if (fac) fac->Release();
    return wrapped ? 1 : 0;
}
HWND our_combo() { return GetDlgItem(g_top, kOurId); }
void save_shot(HWND h, const char* path) {
    RECT r{};
    GetWindowRect(h, &r);
    const int w = r.right - r.left, ht = r.bottom - r.top;
    HDC wdc = GetDC(nullptr), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = ht; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(h, mdc, 0);   // WM_PRINT -> WM_PRINTCLIENT for the window and each child (the window is off-screen)
    GdiFlush();
    if (FILE* f = std::fopen(path, "wb")) {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader; fh.bfSize = fh.bfOffBits + DWORD(w * ht * 4);
        std::fwrite(&fh, sizeof fh, 1, f); std::fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f); std::fwrite(bits, 4, size_t(w) * ht, f);
        std::fclose(f);
    }
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(nullptr, wdc);
}

LRESULT CALLBACK wnd(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: case WM_PRINTCLIENT: {
        PAINTSTRUCT ps{};
        HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp);
        RECT all{}; GetClientRect(h, &all);
        HBRUSH bg = CreateSolidBrush(RGB(70, 12, 10)); FillRect(dc, &all, bg); DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(235, 235, 235)); HGDIOBJ o = SelectObject(dc, g_font);
        const wchar_t* labels[4] = {L"Display Mode", L"Player 1 Input Device", L"Display Resolution", L"Player 2 Input Device"};
        const int lx[4] = {68, 398, 68, 398}, ly[4] = {320, 320, 380, 380};
        for (int i = 0; i < 4; ++i) TextOutW(dc, lx[i], ly[i], labels[i], int(wcslen(labels[i])));
        TextOutW(dc, 108, 510, L"Start the game.", 15);
        SelectObject(dc, o);
        if (msg == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {   // 1.5 s: the combo should be there
            KillTimer(h, 1);
            HWND c = our_combo();
            RECT r{}; if (c) { GetWindowRect(c, &r); MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&r), 2); }
            std::printf("injected=%d rect=%ld,%ld,%ld,%ld sel=%lld\n", c ? 1 : 0, r.left, r.top, r.right, r.bottom, c ? (long long)SendMessageW(c, CB_GETCURSEL, 0, 0) : -1);
            if (g_shot) save_shot(h, g_shot);
            if (g_action == "early-off" && c) {   // select Off, then a factory call WHILE the launcher shows (provisional pass-through)
                SendMessageW(c, CB_SETCURSEL, 1, 0);
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(kOurId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(c));
                call_factory("while-launcher-shows");
            } else if (g_action == "off" && c) {
                SendMessageW(c, CB_SETCURSEL, 1, 0);
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(kOurId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(c));
            } else if (g_action == "on" && c) {
                SendMessageW(c, CB_SETCURSEL, 0, 0);
                SendMessageW(h, WM_COMMAND, MAKEWPARAM(kOurId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(c));
            }
            SetTimer(h, 2, 500, nullptr);
        } else if (wp == 2) {
            KillTimer(h, 2);
            SendMessageW(g_btn, BM_CLICK, 0, 0);
            SetTimer(h, 3, 500, nullptr);
        } else if (wp == 3) {
            KillTimer(h, 3);
            std::printf("after-launch: our-combo-alive=%d\n", our_combo() ? 1 : 0);
            PostQuitMessage(0);
        }
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == g_btn && HIWORD(wp) == BN_CLICKED) {
            g_result = call_factory("after-launch-click");     // like the game: device/factory right after the click
            for (HWND& c : g_combos) { DestroyWindow(c); c = nullptr; }
            DestroyWindow(g_btn); g_btn = nullptr;
            SetWindowPos(h, nullptr, 0, 0, 1600, 900, SWP_NOMOVE | SWP_NOZORDER);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcA(h, msg, wp, lp);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: acz.exe <dxgi.dll> <on|off|keep|early-off> [shot.bmp]\n"); return 2; }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_action = argv[2];
    g_shot = argc > 3 ? argv[3] : nullptr;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_font = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    WNDCLASSEXA wc{sizeof wc};
    wc.lpfnWndProc = wnd; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "AczFakeLauncher"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);
    RECT wr{0, 0, 798, 598};
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU, FALSE);
    // Off-screen and never activated: the harness must not take the user's focus (PrintWindow still renders it).
    g_top = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "AczFakeLauncher", "ACE COMBAT ZERO THE BELKAN WAR", WS_CAPTION | WS_SYSMENU, -4000, -4000,
        wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_top, SW_SHOWNOACTIVATE);
    const int cx[4] = {68, 398, 68, 398}, cy[4] = {342, 342, 402, 402}, cw[4] = {200, 300, 200, 300};
    const wchar_t* first[4] = {L"Windowed", L"Keyboard", L"2560 x 1440", L""};
    for (int i = 0; i < 4; ++i) {
        g_combos[i] = CreateWindowExW(0, L"ComboBox", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, cx[i], cy[i], cw[i], 200, g_top,
            reinterpret_cast<HMENU>(INT_PTR(100 + i)), wc.hInstance, nullptr);
        SendMessageW(g_combos[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SendMessageW(g_combos[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(first[i]));
        SendMessageW(g_combos[i], CB_SETCURSEL, 0, 0);
    }
    g_btn = CreateWindowExW(0, L"Button", L"Launch Game", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 399, 446, 176, 36, g_top, reinterpret_cast<HMENU>(INT_PTR(200)), wc.hInstance, nullptr);
    SendMessageW(g_btn, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    // Like acz.exe: the proxy is in the process before the launcher shows (here: loaded just after; its watcher polls for the window).
    g_proxy = LoadLibraryA(argv[1]);
    if (!g_proxy) { std::printf("cannot load %s (%lu)\n", argv[1], GetLastError()); return 3; }
    SetTimer(g_top, 1, 1500, nullptr);
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    std::printf("wrapped-after-launch=%d\n", g_result);
    return 0;
}
