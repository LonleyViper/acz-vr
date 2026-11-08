#include "xinput_bridge_protocol.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using acz::xinput_bridge::Input;
using acz::xinput_bridge::State;

void usage() {
    std::fputs("Usage: acz-input-bridge status | clear | tap <button> <milliseconds>\n"
               "Buttons: a b x y start back dpad-up dpad-down dpad-left dpad-right\n"
               "         left-thumb right-thumb left-shoulder right-shoulder\n", stderr);
}

bool parse_duration(std::string_view text, DWORD& duration) {
    unsigned long value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > 5000) return false;
    duration = static_cast<DWORD>(value);
    return true;
}

class Mapping {
public:
    Mapping() {
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(State), acz::xinput_bridge::mapping_name);
        if (!mapping_) return;
        state_ = static_cast<State*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
        if (state_ && !acz::xinput_bridge::is_valid(*state_)) *state_ = {};
    }
    ~Mapping() {
        if (state_) UnmapViewOfFile(state_);
        if (mapping_) CloseHandle(mapping_);
    }
    bool valid() const { return state_ != nullptr; }
    State snapshot() const { return *state_; }
    void publish(const Input& input, bool enabled) {
        const auto sequence = state_->sequence;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&state_->sequence),
            static_cast<LONG>((sequence | 1u)));
        MemoryBarrier();
        const auto previous = acz::xinput_bridge::decode(*state_);
        if (!acz::xinput_bridge::equal(previous, input)) ++state_->packet;
        state_->buttons = input.buttons;
        state_->left_trigger = input.left_trigger;
        state_->right_trigger = input.right_trigger;
        state_->thumb_lx = input.thumb_lx;
        state_->thumb_ly = input.thumb_ly;
        state_->thumb_rx = input.thumb_rx;
        state_->thumb_ry = input.thumb_ry;
        state_->enabled = enabled ? 1u : 0u;
        MemoryBarrier();
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&state_->sequence),
            static_cast<LONG>((sequence | 1u) + 1u));
    }
private:
    HANDLE mapping_ = nullptr;
    State* state_ = nullptr;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    Mapping mapping;
    if (!mapping.valid()) { std::fputs("Could not create same-user control mapping.\n", stderr); return 1; }
    const std::string_view command(argv[1]);
    if (command == "status") {
        const auto state = mapping.snapshot();
        std::printf("enabled=%u packet=%u buttons=0x%04x\n", state.enabled, state.packet, state.buttons);
        return 0;
    }
    if (command == "clear") {
        mapping.publish({}, false);
        return 0;
    }
    if (command != "tap" || argc != 4) { usage(); return 2; }
    uint16_t button = 0;
    DWORD duration = 0;
    if (!acz::xinput_bridge::button_mask(argv[2], button) || !parse_duration(argv[3], duration)) {
        usage(); return 2;
    }
    Input pressed{};
    pressed.buttons = button;
    mapping.publish(pressed, true);
    Sleep(duration);
    mapping.publish({}, true);
    // Keep the released state observable for at least one poll before closing.
    Sleep(32);
    return 0;
}
