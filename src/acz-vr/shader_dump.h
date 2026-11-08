#pragma once
// shader_dump.h -- EFFECTS-OUTSIDE (B) read-only diagnostic: save the game's own shader bytecode of selected pipelines to disk so a shader that is
// not in any stored capture can be disassembled offline (dxc -dumpbin). First use: the missile-smoke pipeline (PS d2464ed2, VS 9eff1f92 =
// DrawMesh_v, GS bcb04cf9...), whose geometry shader decides where the smoke particles are (docs/world-render/effects-outside-window.md, section 4).
// Trigger (no config key): the file %LOCALAPPDATA%\acz-vr\dump-shaders.txt listing 8-hex PIXEL-shader prefixes ('#' comments; read once per process).
// Absent/empty file = nothing at all happens (one failed file open per process). For a pipeline whose PS prefix is listed, its VS, PS and GS
// containers are written once per shader hash to %LOCALAPPDATA%\acz-vr\logs\shaders\<32-hex hash>.dxbc (at most kFileCap files per process).
// Nothing is changed in the game or its pipeline; the bytes are copied from the creation description the proxy already sees.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace acz::shaderdump {

inline constexpr unsigned kTriggerCap = 16, kFileCap = 64;
inline constexpr size_t kMaxBytes = 1u << 20;

struct Trigger {
    uint32_t ps[kTriggerCap] = {};
    unsigned count = 0;
    bool has(uint32_t p) const noexcept { for (unsigned i = 0; i < count; ++i) if (ps[i] == p) return true; return false; }
};
// 8-hex tokens separated by whitespace/','/';', '#' to end of line = comment. Returns the number of entries; `bad` counts malformed tokens.
inline unsigned parse_trigger(const char* t, size_t n, Trigger& out, unsigned& bad) noexcept {
    out = Trigger{}; bad = 0; size_t i = 0;
    auto sep = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';' || c == 0; };
    while (i < n) {
        if (sep(t[i])) { ++i; continue; }
        if (t[i] == '#') { while (i < n && t[i] != '\n') ++i; continue; }
        size_t j = i; uint32_t v = 0; unsigned digits = 0; bool ok = true;
        while (j < n && !sep(t[j])) {
            const char c = t[j];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) ok = false; else { v = (v << 4) | uint32_t(d); ++digits; }
            ++j;
        }
        if (!ok || digits != 8) ++bad;
        else if (!out.has(v)) { if (out.count < kTriggerCap) out.ps[out.count++] = v; else ++bad; }
        i = j;
    }
    return out.count;
}
// First 8 hex digits of a 32-hex shader hash (0 when malformed).
inline uint32_t hex8(const char* h) noexcept {
    if (!h) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        const char c = h[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | uint32_t(d);
    }
    return v;
}
// A 32-hex hash is a safe file name (nothing else is ever used as one).
inline bool valid_hash(const char* h) noexcept {
    if (!h) return false;
    for (int i = 0; i < 32; ++i) { const char c = h[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false; }
    return h[32] == 0;
}
struct Stage { const void* bytes = nullptr; size_t length = 0; const char* hash = nullptr; };

#ifdef _WIN32
// Writes every stage with a valid hash and 0 < length <= kMaxBytes to dir\<hash>.dxbc unless that file exists. `budget` is decremented per file
// written (the process cap). Returns the number written; never throws.
inline unsigned dump(const std::wstring& dir, const Stage* s, unsigned n, unsigned& budget) noexcept {
    unsigned wrote = 0;
    try {
        if (dir.empty() || !s) return 0;
        CreateDirectoryW(dir.c_str(), nullptr);
        for (unsigned i = 0; i < n; ++i) {
            if (!budget || !s[i].bytes || !s[i].length || s[i].length > kMaxBytes || !valid_hash(s[i].hash)) continue;
            std::wstring path = dir + L"\\";
            for (const char* c = s[i].hash; *c; ++c) path.push_back(wchar_t(*c));
            path += L".dxbc";
            const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;   // exists already (or cannot be created): never overwritten
            DWORD w = 0;
            const BOOL ok = WriteFile(h, s[i].bytes, DWORD(s[i].length), &w, nullptr);
            CloseHandle(h);
            if (ok && w == DWORD(s[i].length)) { ++wrote; --budget; } else DeleteFileW(path.c_str());
        }
    } catch (...) {}
    return wrote;
}
// %LOCALAPPDATA%\acz-vr\<leaf> (empty when unavailable).
inline std::wstring local_path(const wchar_t* leaf) {
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH - 64) return std::wstring();
    return std::wstring(local) + L"\\acz-vr\\" + leaf;
}
// The process trigger, read once from %LOCALAPPDATA%\acz-vr\dump-shaders.txt (first call; thread-safe static init).
inline const Trigger& process_trigger() noexcept {
    static const Trigger t = [] {
        Trigger out;
        try {
            const std::wstring p = local_path(L"dump-shaders.txt");
            if (p.empty()) return out;
            const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) return out;
            char buf[1024]; DWORD got = 0;
            const BOOL ok = ReadFile(h, buf, sizeof buf - 1, &got, nullptr);
            CloseHandle(h);
            if (ok) { unsigned bad = 0; parse_trigger(buf, got, out, bad); }
        } catch (...) {}
        return out;
    }();
    return t;
}
#endif

} // namespace acz::shaderdump
