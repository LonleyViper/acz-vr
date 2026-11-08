#pragma once
// NATIVE-VR START (docs/world-render/launch-mode.md section 3): when the XR session starts once vsearly:1 is effective.
// SDK-free, header-only; the monitor thread in xr_stereo_proxy.cpp asks decide() every 100 ms poll.
//
//   vsstart:first   (default with vsearly:1) start at the game's FIRST Present after Launch Game: logos, intro videos and menus are shown on
//                   the virtual screen (vscreen) from the start; the full stereo path takes over at the first flight composite as before.
//   vsstart:steady  the previous vsearly gate exactly: >= 120 game presents AND >= 3000 ms since `armed` (the device hook).
//   vsstart:lazy    no early start at all: XR starts at the producer's first StereoFrame (start=lazy-first-published-frame), i.e. what
//                   vsearly:0 does (the per-flight stage, frame-stall limit and early retry are then off as with vsearly:0).
// The environment variable ACZ_XR_START=first|steady|lazy overrides the token for one launch without touching runtime.cfg.
//
// The safety of the old gate is kept by the code that already guards every start, not by the wait: the owner's startup budget (5 s, first
// frame included), the cold-start retry and the persistent transient retry (3 s, 5 s, 10 s, then 15 s; 6/min; xr-start.md 2), the vsearly
// frame-stall limit (8 s after the first frame) and the owner thread (no OpenXR call ever runs on the game's Present thread).
#include <cstdint>
#include <cstring>

namespace acz::xr_gate {

enum class StartMode : uint8_t { first, steady, lazy };

inline const char* start_mode_name(StartMode m) noexcept {
    switch (m) {
    case StartMode::first: return "first";
    case StartMode::steady: return "steady";
    default: return "lazy";
    }
}

// Exact lower-case words only (a typo must not silently pick a mode); surrounding blanks are not accepted either.
inline bool parse_start_mode(const char* v, StartMode& out) noexcept {
    if (!v) return false;
    if (std::strcmp(v, "first") == 0) { out = StartMode::first; return true; }
    if (std::strcmp(v, "steady") == 0) { out = StartMode::steady; return true; }
    if (std::strcmp(v, "lazy") == 0) { out = StartMode::lazy; return true; }
    return false;
}

inline constexpr uint64_t kFirstMinPresents = 1;          // vsstart:first: the first counted game Present
inline constexpr uint64_t kSteadyMinPresents = 120;       // vsstart:steady (the previous gate, ~2 s at 60 fps)
inline constexpr int64_t kSteadyMinArmedMs = 3000;

// The early half of the gate (the producer's first StereoFrame always starts XR as well; see decide()).
inline bool early_ready(StartMode m, uint64_t presents, int64_t since_armed_ms) noexcept {
    switch (m) {
    case StartMode::first: return presents >= kFirstMinPresents;
    case StartMode::steady: return presents >= kSteadyMinPresents && since_armed_ms >= kSteadyMinArmedMs;
    default: return false;
    }
}

// Text for the `vsearly-config` line's start= token.
inline const char* start_rule(StartMode m) noexcept {
    switch (m) {
    case StartMode::first: return "first-game-present(presents>=1)";
    case StartMode::steady: return "presents>=120-and-armed>=3000ms";
    default: return "lazy-first-published-frame";
    }
}

enum class Fire : uint8_t { wait, source, early };

struct Input {
    bool early_effective = false;   // vsearly:1 effective (vsearly-config effective=1)
    bool source_ready = false;      // the producer published a frame with a non-zero extent (the lazy start)
    uint64_t presents = 0;          // game presents counted by the vscreen capture (Present hook)
    int64_t since_armed_ms = 0;
};

// One monitor poll. The lazy source start wins whenever it is ready (as before); the early start fires only when vsearly is effective.
inline Fire decide(StartMode m, const Input& in) noexcept {
    if (in.source_ready) return Fire::source;
    if (in.early_effective && early_ready(m, in.presents, in.since_armed_ms)) return Fire::early;
    return Fire::wait;
}

// Resolution of the mode: the token (or the default `first`), then the environment override. `env` = the variable's text or nullptr.
struct Resolved {
    StartMode mode = StartMode::first;
    const char* source = "default";   // default | option | env(ACZ_XR_START) | option(bad-env-ignored) | default(bad-env-ignored)
};
inline Resolved resolve(bool token_given, StartMode token, const char* env) noexcept {
    Resolved r;
    if (token_given) { r.mode = token; r.source = "option"; }
    if (env && env[0]) {
        StartMode e{};
        if (parse_start_mode(env, e)) { r.mode = e; r.source = "env(ACZ_XR_START)"; }
        else r.source = token_given ? "option(bad-env-ignored)" : "default(bad-env-ignored)";
    }
    return r;
}

} // namespace acz::xr_gate
