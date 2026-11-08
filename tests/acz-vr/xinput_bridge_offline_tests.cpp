#include "xinput_bridge_protocol.h"

#include <cstdio>
#include <cstring>

int main() {
    using namespace acz::xinput_bridge;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };

    State state{};
    check(is_valid(state), "fresh state has the v1 ABI");
    const auto input = Input{0x1000, 5, 6, -7, 8, -9, 10};
    state.buttons = input.buttons;
    state.left_trigger = input.left_trigger;
    state.right_trigger = input.right_trigger;
    state.thumb_lx = input.thumb_lx;
    state.thumb_ly = input.thumb_ly;
    state.thumb_rx = input.thumb_rx;
    state.thumb_ry = input.thumb_ry;
    check(equal(input, decode(state)), "input round-trips through state");
    uint16_t a = 0;
    check(button_mask("a", a) && a == 0x1000, "A mask");
    check(button_mask("dpad-left", a) && a == 0x0004, "dpad-left mask");
    check(!button_mask("invalid", a), "invalid button rejected");
    state.magic_value = 0;
    check(!is_valid(state), "wrong magic rejected");
    return failures ? 1 : 0;
}
