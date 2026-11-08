#pragma once
// PRESET (D-400): xr_terrain_stage preset token vrn:1 ("VR native") expands, at config-parse time, into the headset option set that kept
// outgrowing the line. runtime_config.cpp calls expand() once when it stores the value, so EVERY consumer (vscreen / e6loc / look-stick /
// props / terrain / ui3d / pov ... extractors in xr_stereo_proxy.cpp, xr_flight_stage.cpp, the gpu_copy_step5 hooks, ee_camera_probe_win.cpp)
// reads the already-expanded string through xr_terrain_stage(); there is no second raw read.
// Tokens are separated by ';', a token's key is the text before the first ':' (or '='). Explicit user tokens override the preset's: a preset
// token whose key also appears anywhere outside the vrn token is dropped. The vrn token is replaced in place by the surviving preset tokens.
// vrn:0 is accepted and expands to nothing; any other vrn value (or a repeated vrn token) is refused.
#include <string>
#include <string_view>
#include <vector>
namespace acz::preset {
// Same text as the long form after `fxsig:f1a4e000;` in the 2026-10-31 day value.
inline constexpr const char* kVrn = "ui3d:1;u3p:1;u3a:1;fxtrail:8;fxcloud:120;fxatlas:1;u3pp:1;u3lt:6;dbr3d:1;u3gp:3;hudlate:1;pov:3;povrig:level";
inline std::string_view key_of(std::string_view tok) noexcept {
    const auto p = tok.find_first_of(":=");
    return p == tok.npos ? tok : tok.substr(0, p);
}
inline std::vector<std::string_view> split(std::string_view v) {
    std::vector<std::string_view> t;
    size_t b = 0;
    for (;;) {
        const auto e = v.find(';', b);
        t.push_back(v.substr(b, e == v.npos ? v.npos : e - b));
        if (e == v.npos) break;
        b = e + 1;
    }
    return t;
}
// applied = true when a vrn:1 token was expanded. Returns false (why set) for an unknown preset value or a repeated vrn token; out is then unspecified.
inline bool expand(std::string_view value, std::string& out, bool& applied, std::string& why) {
    out.clear(); applied = false; why.clear();
    const auto toks = split(value);
    size_t vrn_at = toks.size();
    for (size_t i = 0; i < toks.size(); ++i) {
        if (key_of(toks[i]) != "vrn") continue;
        if (vrn_at != toks.size()) { why = "preset vrn given twice"; return false; }
        vrn_at = i;
        if (toks[i] != "vrn:1" && toks[i] != "vrn:0") { why = "unknown preset value (vrn:0 or vrn:1)"; return false; }
    }
    if (vrn_at == toks.size()) { out.assign(value); return true; }
    const bool on = toks[vrn_at] == "vrn:1";
    applied = on;
    bool first = true;
    auto put = [&](std::string_view t) { if (!first) out += ';'; out.append(t); first = false; };
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i != vrn_at) { put(toks[i]); continue; }
        if (!on) continue;
        for (const auto p : split(kVrn)) {
            bool overridden = false;
            for (size_t j = 0; j < toks.size() && !overridden; ++j) overridden = j != vrn_at && key_of(toks[j]) == key_of(p);
            if (!overridden) put(p);
        }
    }
    return true;
}
}
