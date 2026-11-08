#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

// This is a private, same-user, per-game control plane.  It is not a HID
// device, is not installed, and cannot affect other applications unless they
// deliberately load this proxy.
namespace acz::xinput_bridge {

constexpr wchar_t mapping_name[] = L"Local\\ACZeroXInputBridge-v1";
constexpr uint32_t magic = 0x425A4341u; // "ACZB" little-endian
constexpr uint16_t version = 1;

struct State {
    uint32_t magic_value = magic;
    uint16_t version_value = version;
    uint16_t size = sizeof(State);
    // Writers publish an odd value, update every field, then publish even.
    volatile uint32_t sequence = 0;
    uint32_t enabled = 0;
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
};

static_assert(sizeof(State) == 32, "The shared state is a fixed v1 ABI.");

struct Input {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
};

inline bool is_valid(const State& state) noexcept {
    return state.magic_value == magic && state.version_value == version &&
        state.size == sizeof(State);
}

inline bool equal(const Input& a, const Input& b) noexcept {
    return std::memcmp(&a, &b, sizeof(Input)) == 0;
}

inline Input decode(const State& state) noexcept {
    return {state.buttons, state.left_trigger, state.right_trigger,
            state.thumb_lx, state.thumb_ly, state.thumb_rx, state.thumb_ry};
}

// XINPUT_GAMEPAD bit masks are duplicated here so the CLI's parser is fully
// CPU-only and testable without linking against the Windows SDK.
struct ButtonName { std::string_view name; uint16_t mask; };
constexpr std::array<ButtonName, 14> buttons{{
    {"dpad-up", 0x0001}, {"dpad-down", 0x0002}, {"dpad-left", 0x0004},
    {"dpad-right", 0x0008}, {"start", 0x0010}, {"back", 0x0020},
    {"left-thumb", 0x0040}, {"right-thumb", 0x0080}, {"left-shoulder", 0x0100},
    {"right-shoulder", 0x0200}, {"a", 0x1000}, {"b", 0x2000},
    {"x", 0x4000}, {"y", 0x8000},
}};

inline bool button_mask(std::string_view name, uint16_t& out) noexcept {
    for (const auto& button : buttons) {
        if (button.name == name) { out = button.mask; return true; }
    }
    return false;
}

} // namespace acz::xinput_bridge
