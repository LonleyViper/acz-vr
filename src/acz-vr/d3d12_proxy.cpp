// J-3b4: optional forwarding d3d12.dll proxy (built only with
// ACZ_VR_BUILD_D3D12_PROXY=ON; never installed by the build). See
// d3d12_proxy_logic.h for the contract. No game import table or game memory is
// touched and nothing is patched inline: the game's own static import of
// d3d12.dll resolves here because the application directory is searched before
// System32 and d3d12.dll is not a KnownDLL on the target machine.
#include "d3d12_proxy_logic.h"
#include "d3d12_proxy_exports.h" // generated ordinal/name export table
#include <psapi.h>

namespace {
HMODULE self() noexcept {
    HMODULE module=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&self),&module);
    return module;
}
INIT_ONCE g_realOnce=INIT_ONCE_STATIC_INIT;
acz::d3d12proxy::CreateDeviceFn g_real=nullptr;
// Outside loader lock (first D3D12CreateDevice call). Absolute System32 path
// only, never the game directory; refuses to bind to this proxy itself.
BOOL CALLBACK loadReal(PINIT_ONCE,PVOID,PVOID*) {
    wchar_t path[MAX_PATH]{};
    const UINT n=GetSystemDirectoryW(path,MAX_PATH);
    if(!n||n>=MAX_PATH-11)return TRUE;
    lstrcatW(path,L"\\d3d12.dll");
    HMODULE real=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!real||real==self())return TRUE; // process-lifetime reference; never freed
    g_real=reinterpret_cast<acz::d3d12proxy::CreateDeviceFn>(GetProcAddress(real,"D3D12CreateDevice"));
    return TRUE;
}
bool exportsNotify(HMODULE module,acz::d3d12proxy::NotifyFn& out) noexcept {
    if(!module||module==self())return false;
    out=reinterpret_cast<acz::d3d12proxy::NotifyFn>(GetProcAddress(module,acz::d3d12proxy::notify_export));
    return out!=nullptr;
}
// The dxgi proxy is the game's static dxgi.dll import. The real System32
// dxgi.dll (loaded by that proxy) has the same base name and lacks the export,
// so fall back to the loaded-module list when the base-name lookup finds it.
acz::d3d12proxy::NotifyFn resolveNotify() noexcept {
    acz::d3d12proxy::NotifyFn fn=nullptr;
    if(exportsNotify(GetModuleHandleW(L"dxgi.dll"),fn))return fn;
    HMODULE modules[512]{};DWORD bytes=0;
    if(!K32EnumProcessModules(GetCurrentProcess(),modules,sizeof modules,&bytes))return nullptr;
    const DWORD count=(bytes<sizeof modules?bytes:sizeof modules)/sizeof(HMODULE);
    for(DWORD i=0;i<count;++i) {
        wchar_t name[MAX_PATH]{};
        if(!K32GetModuleBaseNameW(GetCurrentProcess(),modules[i],name,MAX_PATH))continue;
        if(lstrcmpiW(name,L"dxgi.dll")==0&&exportsNotify(modules[i],fn))return fn;
    }
    return nullptr;
}
}

extern "C" HRESULT WINAPI ProxyD3D12CreateDevice(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** device) {
    InitOnceExecuteOnce(&g_realOnce,&loadReal,nullptr,nullptr);
    if(!g_real)return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    return acz::d3d12proxy::create_device(g_real,&resolveNotify,adapter,level,iid,device);
}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    // Loader lock: no loading, no notification, no allocation here.
    if(reason==DLL_PROCESS_ATTACH)DisableThreadLibraryCalls(instance);
    return TRUE;
}
