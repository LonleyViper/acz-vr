#pragma once
// LOOK-STICK GATE option inside the existing key xr_terrain_stage (no new config key; docs/world-render/look-stick-gate.md):
//   lookstick:block (DEFAULT) - while the in-flight world is composed from the game camera, acz-vr asks the XINPUT1_4.dll proxy to report the
//                               right stick centred (the game's cockpit look-around then cannot turn the VR world; D-355 problem 3, D-291).
//   lookstick:game            - restore: no heartbeat is published, the game receives the real right stick (the previous behaviour).
// Removed from the value before the other parsers see it (like e6loc:); a value without it is unchanged byte for byte. SDK-free, header-only.
#include <cstring>
#include <string>

namespace acz::look_stick {

enum class Mode { block, game };

inline const char* mode_name(Mode m) noexcept { return m == Mode::game ? "game" : "block"; }

inline bool extract_option(const std::string& value, std::string& rest, Mode& mode, std::string& why) {
    mode = Mode::block;
    rest.clear();
    size_t pos = 0; bool first = true;
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        bool ours = false;
        if (!first) {   // the first item is the stage id, never an option
            const size_t eq = item.find_first_of(":=");
            if (eq == 9 && item.compare(0, 9, "lookstick") == 0) {
                ours = true;
                const std::string v = item.substr(eq + 1);
                if (v == "block" || v == "1") mode = Mode::block;
                else if (v == "game" || v == "0") mode = Mode::game;
                else { why = "bad value for lookstick (block|game)"; return false; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}

} // namespace acz::look_stick
