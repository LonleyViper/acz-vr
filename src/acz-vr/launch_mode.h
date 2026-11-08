#pragma once
// LAUNCH-MODE (docs/world-render/launch-mode.md): the "VR Mode: On / Off" choice in the game's own launcher window.
// SDK-free, header-only core (offline-tested in tests/acz-vr/launch_mode_offline_tests.cpp); the Win32 parts live in launch_mode_win.cpp.
//
//   * The choice is stored in its OWN file, %LOCALAPPDATA%\acz-vr\launch-mode.txt (runtime.cfg is never rewritten). Missing/unreadable/
//     unrecognised file = On (the previous behaviour: runtime.cfg as it is).
//   * On  = the proxies work exactly as before (runtime.cfg decides every feature).
//   * Off = the dxgi.dll proxy forwards CreateDXGIFactory* to the system dxgi.dll without wrapping the factory, never activates the
//           diagnostics/config, never installs a hook, never starts the XR monitor; the d3d12.dll proxy's device notification returns at once.
//           (The skip-intro XINPUT1_4.dll is not VR-specific and keeps its intro skip; its look-stick gate stays passive without our heartbeat.)
//   * The mode is decided (latched) once per process at the first factory/device call that is NOT made while the launcher still shows an
//     Off choice: see decide().
#include <cstdint>
#include <string>
#include <vector>

namespace acz::launch_mode {

enum class VrMode : uint8_t { on, off };
inline const char* mode_name(VrMode m) noexcept { return m == VrMode::off ? "off" : "on"; }

// ------------------------------------------------------------------------------------------------ the file
enum class FileState : uint8_t { ok, missing, unreadable, unrecognised };
inline const char* file_state_name(FileState s) noexcept {
    switch (s) {
    case FileState::ok: return "ok";
    case FileState::missing: return "missing";
    case FileState::unreadable: return "unreadable";
    default: return "unrecognised";
    }
}

struct FileValue {
    VrMode mode = VrMode::on;
    FileState state = FileState::missing;
};

namespace detail {
inline char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
inline std::string trim_lower(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    std::string r;
    for (size_t i = b; i < e; ++i) r += lower(s[i]);
    return r;
}
inline bool word(const std::string& v, VrMode& out) {
    if (v == "on" || v == "1" || v == "true" || v == "yes") { out = VrMode::on; return true; }
    if (v == "off" || v == "0" || v == "false" || v == "no") { out = VrMode::off; return true; }
    return false;
}
} // namespace detail

// Text of launch-mode.txt: the first line that is neither blank nor a comment (# or ;) decides. Accepted: `vr=on|off` (also `vr:`,
// `vr_mode=`, `vrmode=`), or the bare word; on/off/1/0/true/false/yes/no; any case; a UTF-8 BOM and CRLF are fine. Anything else on that
// line = unrecognised (mode On). An empty file (no deciding line) = unrecognised too.
inline FileValue parse_file(const std::string& text) {
    FileValue r;
    r.state = FileState::unrecognised;
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF)
        body.erase(0, 3);
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        const std::string line = detail::trim_lower(body.substr(pos, nl - pos));
        pos = nl + 1;
        if (!line.empty() && line[0] != '#' && line[0] != ';') {
            std::string v = line;
            const size_t eq = line.find_first_of("=:");
            if (eq != std::string::npos) {
                const std::string k = detail::trim_lower(line.substr(0, eq));
                if (k != "vr" && k != "vr_mode" && k != "vrmode") return r;
                v = detail::trim_lower(line.substr(eq + 1));
            }
            VrMode m{};
            if (detail::word(v, m)) { r.mode = m; r.state = FileState::ok; }
            return r;
        }
        if (nl == body.size()) break;
    }
    return r;
}

inline std::string format_file(VrMode m) {
    return std::string("vr=") + mode_name(m) +
           "\r\n# ACE COMBAT ZERO VR mode (on|off), written by the VR Mode option in the game's launcher.\r\n"
           "# on = VR with runtime.cfg as it is; off = the plain flat game (no XR session, the proxies only forward).\r\n"
           "# Delete this file for the default (on). docs/world-render/launch-mode.md\r\n";
}

// ------------------------------------------------------------------------------------------------ the decision (latch)
// Inputs at one factory (CreateDXGIFactory*) or early-device call.
struct DecideInput {
    bool launcher_showing = false;   // the launcher window is still up and Launch Game was not pressed (launch_mode_win.cpp)
    bool has_selection = false;      // the injected combo exists (its value is `selection`, initialised from the file)
    VrMode selection = VrMode::on;
    FileValue file;                  // read at this call when there is no selection
};
enum class Source : uint8_t { launcher, file, default_on };
inline const char* source_name(Source s) noexcept { return s == Source::launcher ? "launcher" : s == Source::file ? "file" : "default"; }
struct Decision {
    VrMode mode = VrMode::on;
    Source source = Source::default_on;
    bool latch = true;               // false = pass this call through unwrapped and decide again at the next call (see below)
};
// The launcher's own selection wins over the file; the file over the default (On).
// Off is decided (latched) only once the launcher is gone: a factory call made WHILE the launcher still shows an Off choice (not seen in any
// log: the first factory call came 0.4 s after the Launch Game click in the 2026-11-01 user session; an overlay or the launcher itself could
// probe dxgi) is passed through unwrapped WITHOUT latching, so switching back to On before Launch still works. On is latched at once, so a
// factory the game creates during the launcher is wrapped exactly as before (the previous behaviour for every On launch).
inline Decision decide(const DecideInput& in) noexcept {
    Decision d;
    if (in.has_selection) { d.mode = in.selection; d.source = Source::launcher; }
    else if (in.file.state == FileState::ok) { d.mode = in.file.mode; d.source = Source::file; }
    else { d.mode = VrMode::on; d.source = Source::default_on; }
    d.latch = !(d.mode == VrMode::off && in.launcher_showing);
    return d;
}

// ------------------------------------------------------------------------------------------------ the launcher layout
struct Rect { int l = 0, t = 0, r = 0, b = 0; int w() const noexcept { return r - l; } int h() const noexcept { return b - t; } };
inline bool intersects(const Rect& a, const Rect& b) noexcept { return a.l < b.r && b.l < a.r && a.t < b.b && b.t < a.b; }

struct LayoutInput {
    std::vector<Rect> combos;        // the launcher's ComboBox children (client coordinates of the launcher window, closed height)
    std::vector<Rect> others;        // every other visible child (the Launch Game button, ...)
    Rect client;                     // the launcher's client rectangle
};
struct Layout {
    bool ok = false;
    const char* why = "";
    Rect combo;                      // our combo (closed height = the reference combo's)
    Rect label;                      // where "VR Mode" is drawn (the gap above our combo, like the launcher's own labels)
    int pitch = 0;
};
// The launcher stacks its combos in two columns (Display Mode / Display Resolution on the left, Player 1 / Player 2 on the right, labels painted
// in the gap above each combo; screenshot step75 launcher.png). The new combo goes one row below the LEFT column's last combo with the same
// pitch, width and height, i.e. beside the Launch Game button; its label in the gap above it. Refused (fallback: launch-mode.txt / the
// companion script) when the column cannot be recognised or the new combo would overlap another child or leave the client area.
inline Layout place(const LayoutInput& in) noexcept {
    Layout L;
    if (in.combos.size() < 2) { L.why = "fewer-than-2-combos"; return L; }
    int left = in.combos[0].l;
    for (const auto& c : in.combos) if (c.l < left) left = c.l;
    const Rect* col[8]{};
    int n = 0;
    for (const auto& c : in.combos) if (c.l - left <= 4 && c.l - left >= -4 && n < 8) col[n++] = &c;
    if (n < 2) { L.why = "left-column-has-fewer-than-2-combos"; return L; }
    for (int i = 1; i < n; ++i) for (int j = i; j > 0 && col[j]->t < col[j - 1]->t; --j) { const Rect* t = col[j]; col[j] = col[j - 1]; col[j - 1] = t; }
    const Rect& prev = *col[n - 2];
    const Rect& last = *col[n - 1];
    const int pitch = last.t - prev.t;
    const int h = last.h();
    if (h <= 0 || last.w() <= 0) { L.why = "empty-combo-rect"; return L; }
    if (pitch < h + 4 || pitch > 6 * h) { L.why = "irregular-pitch"; return L; }
    Rect c{last.l, last.t + pitch, last.r, last.t + pitch + h};
    if (c.l < in.client.l || c.r > in.client.r || c.t < in.client.t || c.b > in.client.b - 2) { L.why = "outside-client"; return L; }
    for (const auto& o : in.others) if (intersects(c, o)) { L.why = "overlaps-a-child"; return L; }
    for (const auto& o : in.combos) if (intersects(c, o)) { L.why = "overlaps-a-combo"; return L; }
    L.ok = true;
    L.why = "ok";
    L.combo = c;
    L.label = Rect{c.l, last.b, c.r, c.t};
    L.pitch = pitch;
    return L;
}

} // namespace acz::launch_mode
