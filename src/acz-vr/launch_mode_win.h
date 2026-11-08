#pragma once
// LAUNCH-MODE (docs/world-render/launch-mode.md): Win32 side of launch_mode.h. DLL-only (dxgi.dll proxy).
#include <windows.h>

namespace acz::launch_mode {
// DllMain(DLL_PROCESS_ATTACH) of the dxgi.dll proxy: creates ONE thread (nothing else under the loader lock) that waits for the launcher
// window and adds the "VR Mode" combo on the launcher's own UI thread. Only in acz.exe; ACZ_VR_LAUNCHER_OPTION=0 turns the combo off
// (launch-mode.txt still decides).
void start_launcher_watch(HINSTANCE self) noexcept;
// The factory/device gate: true = VR On (the proxy works as before), false = VR Off (forward only, activate nothing). Latched once per
// process (launch_mode.h decide()); never blocks on another thread.
bool vr_enabled() noexcept;
}
