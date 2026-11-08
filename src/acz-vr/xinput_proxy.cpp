#include "xinput_bridge_protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

HMODULE system_xinput = nullptr;
GetStateFn real_get_state = nullptr;
SetStateFn real_set_state = nullptr;
const acz::xinput_bridge::State* shared_state = nullptr;

void resolve_system_xinput() noexcept {
    if (system_xinput) return;
    wchar_t path[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 16) return;
    lstrcatW(path, L"\\XInput1_4.dll");
    system_xinput = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!system_xinput) return;
    real_get_state = reinterpret_cast<GetStateFn>(GetProcAddress(system_xinput,
        reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(2))));
    real_set_state = reinterpret_cast<SetStateFn>(GetProcAddress(system_xinput,
        reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(3))));
}

bool read_virtual_state(XINPUT_STATE& output) noexcept {
    if (!shared_state) {
        const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
            acz::xinput_bridge::mapping_name);
        if (!mapping) return false;
        const auto* mapped = static_cast<const acz::xinput_bridge::State*>(
            MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(acz::xinput_bridge::State)));
        CloseHandle(mapping);
        if (!mapped) return false;
        shared_state = mapped;
    }

    const auto* source = shared_state;
    for (unsigned retry = 0; retry != 4; ++retry) {
        const uint32_t before = source->sequence;
        if (before & 1u) continue;
        MemoryBarrier();
        const auto snapshot = *source;
        MemoryBarrier();
        const uint32_t after = source->sequence;
        if (before != after || (after & 1u) || !acz::xinput_bridge::is_valid(snapshot) ||
            !snapshot.enabled) return false;
        output.dwPacketNumber = snapshot.packet;
        output.Gamepad.wButtons = snapshot.buttons;
        output.Gamepad.bLeftTrigger = snapshot.left_trigger;
        output.Gamepad.bRightTrigger = snapshot.right_trigger;
        output.Gamepad.sThumbLX = snapshot.thumb_lx;
        output.Gamepad.sThumbLY = snapshot.thumb_ly;
        output.Gamepad.sThumbRX = snapshot.thumb_rx;
        output.Gamepad.sThumbRY = snapshot.thumb_ry;
        return true;
    }
    return false;
}

} // namespace

extern "C" DWORD WINAPI XInputGetState(DWORD user_index, XINPUT_STATE* state) noexcept {
    if (!state) return ERROR_BAD_ARGUMENTS;
    if (user_index == 0 && read_virtual_state(*state)) return ERROR_SUCCESS;
    resolve_system_xinput();
    return real_get_state ? real_get_state(user_index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) noexcept {
    resolve_system_xinput();
    return real_set_state ? real_set_state(user_index, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    // No loader-lock work: system XInput and shared memory are opened lazily.
    (void)reason;
    return TRUE;
}
