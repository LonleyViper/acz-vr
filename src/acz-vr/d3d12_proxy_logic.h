#pragma once
// J-3b4: decision logic of the optional forwarding d3d12.dll proxy, kept free of
// loader calls so the offline fixture can drive it with fakes.
//
// The proxy exports every system d3d12.dll export with the same ordinal and name
// (generated d3d12_proxy.def); all but D3D12CreateDevice are loader forwarders to
// C:\Windows\System32\d3d12.dll. D3D12CreateDevice is this thin wrapper:
//   - the real function (loaded once, by absolute System32 path) is called
//     EXACTLY once per call with the caller's unchanged arguments;
//   - its HRESULT and output are returned unchanged, always;
//   - only after a success that published a non-null device, and only when the
//     dxgi proxy's AczVrNotifyDeviceCreated export is present, the created
//     device and the caller's IID are handed to it. The wrapper takes no
//     reference and holds no lock; the notification takes its own reference
//     only if it keeps the device, and is itself a no-op unless enabled
//     (gpu_copy_early_device_hook plus the step-1 opt-ins).
#include <windows.h>
#include <d3dcommon.h>

namespace acz::d3d12proxy {
using CreateDeviceFn=HRESULT(WINAPI*)(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** device);
using NotifyFn=unsigned(WINAPI*)(IUnknown* device,const GUID* iid);
// Looks up the notification export; called only after a qualifying success.
using ResolveNotifyFn=NotifyFn(*)() noexcept;
inline constexpr const char* notify_export="AczVrNotifyDeviceCreated";

// Notify only after a success that published a device (a support query with a
// null output returns S_FALSE and publishes nothing).
inline bool should_notify(HRESULT hr,void** out) noexcept {return SUCCEEDED(hr)&&out&&*out;}

inline HRESULT create_device(CreateDeviceFn real,ResolveNotifyFn resolve,IUnknown* adapter,D3D_FEATURE_LEVEL level,
                             REFIID iid,void** out) noexcept {
    if(!real)return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND); // never loads or calls anything else
    const HRESULT hr=real(adapter,level,iid,out);
    if(resolve&&should_notify(hr,out)) {
        // Optional: never changes hr or the published output.
        try {if(auto notify=resolve())(void)notify(static_cast<IUnknown*>(*out),&iid);} catch(...) {}
    }
    return hr;
}
}
