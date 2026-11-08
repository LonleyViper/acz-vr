#include "xinput_bridge_protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>

#include <cstdio>

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*) noexcept;

void publish(acz::xinput_bridge::State& state, const acz::xinput_bridge::Input& input) {
    const auto previous = state.sequence;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state.sequence),
        static_cast<LONG>(previous | 1u));
    MemoryBarrier();
    ++state.packet;
    state.buttons = input.buttons;
    state.left_trigger = input.left_trigger;
    state.right_trigger = input.right_trigger;
    state.thumb_lx = input.thumb_lx;
    state.thumb_ly = input.thumb_ly;
    state.thumb_rx = input.thumb_rx;
    state.thumb_ry = input.thumb_ry;
    state.enabled = 1;
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&state.sequence),
        static_cast<LONG>((previous | 1u) + 1u));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(acz::xinput_bridge::State), acz::xinput_bridge::mapping_name);
    if (!mapping) return 3;
    const auto state = static_cast<acz::xinput_bridge::State*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(acz::xinput_bridge::State)));
    if (!state) { CloseHandle(mapping); return 4; }
    *state = {};
    const acz::xinput_bridge::Input expected{0x1000, 7, 8, -9, 10, -11, 12};
    publish(*state, expected);

    const auto proxy = LoadLibraryA(argv[1]);
    const auto get_state = proxy ? reinterpret_cast<GetStateFn>(
        GetProcAddress(proxy, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(2)))) : nullptr;
    XINPUT_STATE received{};
    const auto result = get_state ? get_state(0, &received) : ERROR_PROC_NOT_FOUND;
    const bool passed = result == ERROR_SUCCESS && received.dwPacketNumber == 1 &&
        received.Gamepad.wButtons == expected.buttons &&
        received.Gamepad.bLeftTrigger == expected.left_trigger &&
        received.Gamepad.bRightTrigger == expected.right_trigger &&
        received.Gamepad.sThumbLX == expected.thumb_lx && received.Gamepad.sThumbLY == expected.thumb_ly &&
        received.Gamepad.sThumbRX == expected.thumb_rx && received.Gamepad.sThumbRY == expected.thumb_ry;
    if (!passed) std::fprintf(stderr, "proxy state mismatch: result=%lu packet=%lu buttons=0x%04x\n",
        result, received.dwPacketNumber, received.Gamepad.wButtons);

    if (proxy) FreeLibrary(proxy);
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    return passed ? 0 : 1;
}
