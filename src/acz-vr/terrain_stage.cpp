#include "terrain_stage.h"
#include "lens_flare.h"
#include "world_clouds.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acz::terrain {
namespace {
// ------------------------------------------------------------------ SHA-256 (FIPS 180-4)
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }
struct Sha {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64]; size_t fill = 0; uint64_t bytes = 0;
    void block(const uint8_t* b) noexcept {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t(b[4 * i]) << 24) | (uint32_t(b[4 * i + 1]) << 16) | (uint32_t(b[4 * i + 2]) << 8) | b[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b2 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b2) ^ (a & c) ^ (b2 & c);
            const uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b2; b2 = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b2; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) noexcept {
        bytes += n;
        while (n) {
            const size_t k = std::min(n, 64 - fill);
            std::memcpy(buf + fill, p, k); fill += k; p += k; n -= k;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    void final(uint8_t out[32]) noexcept {
        const uint64_t bits = bytes * 8;
        const uint8_t one = 0x80; update(&one, 1);
        const uint8_t zero = 0; while (fill != 56) update(&zero, 1);
        uint8_t l[8]; for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 8);
        for (int i = 0; i < 8; ++i) { out[4 * i] = uint8_t(h[i] >> 24); out[4 * i + 1] = uint8_t(h[i] >> 16); out[4 * i + 2] = uint8_t(h[i] >> 8); out[4 * i + 3] = uint8_t(h[i]); }
    }
};
inline uint32_t rd32(const uint8_t* p) noexcept { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline float rdf(const uint8_t* p) noexcept { float v; std::memcpy(&v, p, 4); return v; }
inline void wr32(uint8_t* p, uint32_t v) noexcept { std::memcpy(p, &v, 4); }
inline void wrf(uint8_t* p, float v) noexcept { std::memcpy(p, &v, 4); }
inline float sat(float x) noexcept { return x < 0 ? 0 : x > 1 ? 1 : x; }
inline float lerpf(float a, float b, float t) noexcept { return a + (b - a) * t; }
} // namespace

void sha256(const void* p, size_t n, uint8_t out[32]) noexcept { Sha s; s.update(static_cast<const uint8_t*>(p), n); s.final(out); }
std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef"; std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
const char* status_name(LoadStatus s) noexcept {
    switch (s) {
    case LoadStatus::ok: return "ok"; case LoadStatus::io_error: return "io-error"; case LoadStatus::too_small: return "too-small";
    case LoadStatus::too_large: return "too-large"; case LoadStatus::bad_magic: return "bad-magic"; case LoadStatus::bad_version: return "bad-version";
    case LoadStatus::bad_header: return "bad-header"; case LoadStatus::bad_size: return "bad-size"; case LoadStatus::bad_hash: return "bad-hash";
    case LoadStatus::bad_content: return "bad-content";
    }
    return "?";
}
bool valid_stage_id(const std::string& id) noexcept {
    if (id.empty() || id.size() > 8) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
std::string stage_path(const std::string& id, const std::string& dir_override) {
    if (!valid_stage_id(id)) return std::string();
    std::string dir = dir_override;
    if (dir.empty()) {
        const char* la = std::getenv("LOCALAPPDATA");
        if (!la || !*la) return std::string();
        dir = std::string(la) + "\\acz-vr\\stage";
    }
    return dir + "\\" + id + ".zvrs";
}

LoadStatus parse_stage(const uint8_t* d, size_t n, StageData& out) noexcept {
    try {
        if (!d || n < kZvrsHeader) return LoadStatus::too_small;
        if (n > kMaxFileBytes) return LoadStatus::too_large;
        if (std::memcmp(d, "ZVRS", 4) != 0) return LoadStatus::bad_magic;
        const uint32_t version = rd32(d + 4);
        if (version < kZvrsMinVersion || version > kZvrsVersion) return LoadStatus::bad_version;
        const uint32_t flags = rd32(d + 16), hdim = rd32(d + 20), tgrid = rd32(d + 24), ntiles = rd32(d + 28), tilepx = rd32(d + 32), nobj = rd32(d + 36);
        if (hdim != kHDim || tgrid != kTGrid || tilepx != kTilePx || ntiles == 0 || ntiles > kMaxTiles || nobj > kMaxObjects) return LoadStatus::bad_header;
        const float cell = rdf(d + 40), hstep = rdf(d + 44), half = rdf(d + 48), sea = rdf(d + 52);
        if (!(cell > 0 && cell < 1e6f) || !(hstep > 0 && hstep < 1e4f) || !(half > 0 && half < 1e8f) || !std::isfinite(sea)) return LoadStatus::bad_header;
        if (std::fabs(cell * 512.0f - 2.0f * half) > 1e-3f * half) return LoadStatus::bad_header;
        uint32_t off[5]; for (int i = 0; i < 5; ++i) off[i] = rd32(d + 56 + 4 * i);
        const uint32_t size = rd32(d + 76);
        if (size != n) return LoadStatus::bad_size;
        const uint64_t need[5] = {uint64_t(kHDim) * kHDim, uint64_t(kTGrid) * kTGrid * 2, uint64_t(ntiles) * kTilePx * kTilePx, 1024, uint64_t(nobj) * 16};
        for (int i = 0; i < 5; ++i) {
            if ((off[i] & 3u) || off[i] < kZvrsHeader || uint64_t(off[i]) + need[i] > n) return LoadStatus::bad_size;
            if (i > 0 && off[i] < off[i - 1] + need[i - 1]) return LoadStatus::bad_size;
        }
        uint32_t off_tw = 0, water_key = 0, off_env = 0;
        if (version >= 2) {   // v2: water tile flags (u8 per atlas tile) after the objects; header +80 = offset, +84 = key palette index
            off_tw = rd32(d + 80); water_key = rd32(d + 84);
            if ((off_tw & 3u) || off_tw < off[4] + need[4] || uint64_t(off_tw) + ntiles > n || water_key > 255u) return LoadStatus::bad_size;
            if ((flags & ~(version >= 3 ? 15u : 7u)) != 0) return LoadStatus::bad_header;
        } else if ((flags & kFlagWater) || rd32(d + 80) != 0 || rd32(d + 84) != 0) return LoadStatus::bad_header;   // v1 never had water
        if (version >= 3) {   // v3 (WORLD-2): optional environment section after the water flags; header +88 = offset, +92 = byte size
            off_env = rd32(d + 88);
            const uint32_t env_size = rd32(d + 92);
            if (flags & kFlagEnv) {
                if (env_size != kEnvSectionBytes || (off_env & 3u) || off_env < off_tw + ntiles || uint64_t(off_env) + env_size > n) return LoadStatus::bad_size;
            } else if (off_env != 0 || env_size != 0) return LoadStatus::bad_header;
        } else if (rd32(d + 88) != 0 || rd32(d + 92) != 0) return LoadStatus::bad_header;
        uint8_t hash[32];
        sha256(d + kZvrsHeader, n - kZvrsHeader, hash);
        if (std::memcmp(hash, d + 192, 32) != 0) return LoadStatus::bad_hash;
        StageData s;
        std::memcpy(s.id, d + 8, 8); s.id[8] = 0;
        if (!valid_stage_id(s.id)) return LoadStatus::bad_header;
        s.flags = flags; s.ntiles = ntiles; s.nobj = nobj; s.cell = cell; s.hstep = hstep; s.half = half; s.sea_level = sea;
        s.heights.assign(d + off[0], d + off[0] + need[0]);
        s.words.resize(size_t(kTGrid) * kTGrid);
        std::memcpy(s.words.data(), d + off[1], need[1]);
        for (uint16_t w : s.words) if ((w & 0x3ffu) >= ntiles) return LoadStatus::bad_content;
        s.tiles.assign(d + off[2], d + off[2] + need[2]);
        std::memcpy(s.clut, d + off[3], 1024);
        s.objects.resize(size_t(nobj) * 4);
        if (nobj) std::memcpy(s.objects.data(), d + off[4], need[4]);
        std::memcpy(s.payload_sha, d + 192, 32); std::memcpy(s.source_sha, d + 224, 32);
        s.hmax_byte = *std::max_element(s.heights.begin(), s.heights.end());
        s.sea_masked = sea_tiles_flagged(s);   // D-344 C1 (derived; the file is unchanged)
        if (version >= 2) {
            s.tile_water.assign(d + off_tw, d + off_tw + ntiles);
            for (uint8_t w : s.tile_water) if (w > 1) return LoadStatus::bad_content;
            s.water_key = uint8_t(water_key);
        } else s.flags &= ~kFlagWater;
        if (version >= 3 && (flags & kFlagEnv)) {
            const uint8_t* e = d + off_env;
            if (std::memcmp(e, "ENV1", 4) != 0 || rd32(e + 4) != 1u) return LoadStatus::bad_content;
            s.env_params.resize(size_t(kEnvParams) * 4);
            std::memcpy(s.env_params.data(), e + 8, size_t(kEnvParams) * 16);
            for (float v : s.env_params) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return LoadStatus::bad_content;
            // REVIEW L-5: the renderer divides by the map repeats / sprite size (P[25].xyz) and by the sky stop spacing (P[24]); stage_baker.py writes the
            // constants MISC (all > 0) and SKY_STOPS_DEG (strictly increasing). Zero / unordered values gave inf/NaN colours and texel coordinates.
            const float* misc = &s.env_params[size_t(kPMisc) * 4]; const float* stops = &s.env_params[size_t(kPSkyStops) * 4];
            if (!(misc[0] > 0.0f && misc[1] > 0.0f && misc[2] > 0.0f) || !(stops[0] < stops[1] && stops[1] < stops[2] && stops[2] < stops[3])) return LoadStatus::bad_content;
            const uint8_t* m = e + 8 + kEnvParams * 16;
            s.env_cloud.assign(m, m + kEnvMap * kEnvMap); m += kEnvMap * kEnvMap;
            s.env_wave.assign(m, m + kEnvWave * kEnvWave); m += kEnvWave * kEnvWave;
            s.env_sprite_i.assign(m, m + kEnvSprite * kEnvSprite); m += kEnvSprite * kEnvSprite;
            s.env_sprite_a.assign(m, m + kEnvSprite * kEnvSprite);
        } else s.flags &= ~kFlagEnv;
        out = std::move(s);
        return LoadStatus::ok;
    } catch (...) { return LoadStatus::io_error; }
}
namespace {
LoadStatus read_file(const std::string& path, size_t max_bytes, std::vector<uint8_t>& buf) {
    if (path.empty()) return LoadStatus::io_error;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return LoadStatus::io_error;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return LoadStatus::io_error; }
    if (size_t(sz) > max_bytes) { std::fclose(f); return LoadStatus::too_large; }
    buf.resize(size_t(sz));
    const size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return got == buf.size() ? LoadStatus::ok : LoadStatus::io_error;
}
uint16_t rd16le(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32le(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}
LoadStatus load_stage_file(const std::string& path, StageData& out) noexcept {
    try {
        std::vector<uint8_t> buf;
        const LoadStatus rs = read_file(path, kMaxFileBytes, buf);
        if (rs != LoadStatus::ok) return rs;
        const LoadStatus st = parse_stage(buf.data(), buf.size(), out);
        if (st != LoadStatus::ok) return st;
        // SKIN-REFL: the optional sidecar (never fails the stage)
        out.refl_w = out.refl_h = 0; out.refl_rgba.clear();
        std::vector<uint8_t> rb;
        LoadStatus r = read_file(refl_map_path(path), kReflMapHeader + size_t(kReflMapMaxDim) * kReflMapMaxDim * 4u, rb);
        if (r == LoadStatus::ok) r = parse_refl_map(rb.data(), rb.size(), out.refl_w, out.refl_h, out.refl_rgba);
        out.refl_status = r;
        return st;
    } catch (...) { return LoadStatus::io_error; }
}
LoadStatus parse_refl_map(const uint8_t* d, size_t n, uint32_t& w_out, uint32_t& h_out, std::vector<uint8_t>& rgba) noexcept {
    try {
        if (!d || n < kReflMapHeader) return LoadStatus::too_small;
        if (std::memcmp(d, "ZVRT", 4) != 0) return LoadStatus::bad_magic;
        if (rd32le(d + 4) != kReflMapVersion) return LoadStatus::bad_version;
        const uint32_t w = rd16le(d + 8), h = rd16le(d + 10);
        if (!w || !h || w > kReflMapMaxDim || h > kReflMapMaxDim || rd32le(d + 12) != kReflMapKind) return LoadStatus::bad_header;
        const size_t payload = size_t(w) * h * 4u;
        if (n != kReflMapHeader + payload) return LoadStatus::bad_size;
        uint8_t sha[32];
        sha256(d + kReflMapHeader, payload, sha);
        if (std::memcmp(sha, d + 16, 32) != 0) return LoadStatus::bad_hash;
        rgba.assign(d + kReflMapHeader, d + kReflMapHeader + payload);
        w_out = w; h_out = h;
        return LoadStatus::ok;
    } catch (...) { return LoadStatus::io_error; }
}
std::vector<uint8_t> serialize_refl_map(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> o(kReflMapHeader, 0);
    std::memcpy(o.data(), "ZVRT", 4);
    auto w32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) o[at + i] = uint8_t(v >> (8 * i)); };
    w32(4, kReflMapVersion); o[8] = uint8_t(w); o[9] = uint8_t(w >> 8); o[10] = uint8_t(h); o[11] = uint8_t(h >> 8); w32(12, kReflMapKind);
    sha256(rgba.data(), rgba.size(), &o[16]);
    o.insert(o.end(), rgba.begin(), rgba.end());
    return o;
}
std::string refl_map_path(const std::string& p) {
    static const char kExt[] = ".zvrs";
    if (p.size() > 5) {
        std::string tail = p.substr(p.size() - 5);
        for (char& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (tail == kExt) return p.substr(0, p.size() - 5) + "_ref.zvrt";
    }
    return p + "_ref.zvrt";
}
std::vector<uint8_t> serialize_stage(const StageData& s) {
    std::vector<uint8_t> body;
    uint32_t off[7] = {}; size_t pos = kZvrsHeader;
    auto add = [&](int i, const void* p, size_t n) {
        while (pos & 3) { body.push_back(0); ++pos; }
        off[i] = uint32_t(pos);
        const uint8_t* b = static_cast<const uint8_t*>(p);
        body.insert(body.end(), b, b + n); pos += n;
    };
    add(0, s.heights.data(), s.heights.size());
    add(1, s.words.data(), s.words.size() * 2);
    add(2, s.tiles.data(), s.tiles.size());
    add(3, s.clut, 1024);
    add(4, s.objects.data(), s.objects.size() * 4);
    std::vector<uint8_t> tw = s.tile_water; tw.resize(s.ntiles, 0);
    add(5, tw.data(), tw.size());
    const bool env = s.has_env() && s.env_cloud.size() == kEnvMap * kEnvMap && s.env_wave.size() == kEnvWave * kEnvWave &&
                     s.env_sprite_i.size() == kEnvSprite * kEnvSprite && s.env_sprite_a.size() == kEnvSprite * kEnvSprite;
    if (env) {   // v3 environment section (stage_baker.py writes the same bytes)
        std::vector<uint8_t> e(8, 0);
        std::memcpy(e.data(), "ENV1", 4); e[4] = 1;
        const uint8_t* pp = reinterpret_cast<const uint8_t*>(s.env_params.data());
        e.insert(e.end(), pp, pp + size_t(kEnvParams) * 16);
        e.insert(e.end(), s.env_cloud.begin(), s.env_cloud.end()); e.insert(e.end(), s.env_wave.begin(), s.env_wave.end());
        e.insert(e.end(), s.env_sprite_i.begin(), s.env_sprite_i.end()); e.insert(e.end(), s.env_sprite_a.begin(), s.env_sprite_a.end());
        add(6, e.data(), e.size());
    }
    while (pos & 3) { body.push_back(0); ++pos; }
    std::vector<uint8_t> out(kZvrsHeader, 0);
    std::memcpy(out.data(), "ZVRS", 4); wr32(&out[4], env ? 3u : 2u);   // v2 unless the stage carries the environment (v2 files round-trip byte for byte)
    std::memcpy(&out[8], s.id, 8);
    uint32_t fl = s.has_water() ? (s.flags | kFlagWater) : (s.flags & ~kFlagWater);
    fl = env ? (fl | kFlagEnv) : (fl & ~kFlagEnv);
    wr32(&out[16], fl); wr32(&out[20], kHDim); wr32(&out[24], kTGrid); wr32(&out[28], s.ntiles); wr32(&out[32], kTilePx); wr32(&out[36], s.nobj);
    if (env) { wr32(&out[88], off[6]); wr32(&out[92], kEnvSectionBytes); }
    wrf(&out[40], s.cell); wrf(&out[44], s.hstep); wrf(&out[48], s.half); wrf(&out[52], s.sea_level);
    for (int i = 0; i < 5; ++i) wr32(&out[56 + 4 * i], off[i]);
    wr32(&out[76], uint32_t(pos));
    wr32(&out[80], off[5]); wr32(&out[84], s.water_key);
    sha256(body.data(), body.size(), &out[192]);
    std::memcpy(&out[224], s.source_sha, 32);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ------------------------------------------------------------------ conventions
namespace {
bool num(const std::string& v, float& out) {
    if (v.empty() || v.size() > 16) return false;
    char* e = nullptr; const double x = std::strtod(v.c_str(), &e);
    if (e != v.c_str() + v.size() || !std::isfinite(x) || std::fabs(x) > 1e7) return false;
    out = float(x); return true;
}
}
bool parse_stage_option(const std::string& value, std::string& id, Conventions& c, std::string& why) {
    c = Conventions{};
    c.cam_admit = true;   // CAM-EPOCH: admission on for every parsed (live) value; camadmit:0 restores the pre-CAM-EPOCH camera selection
    c.lens_flare = true;  // LENS-FLARE: on for every parsed (live) value; lflare:0 turns it off
    size_t pos = 0; bool first = true;
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        if (first) {
            first = false;
            if (!valid_stage_id(item)) { why = "bad stage id"; return false; }
            id = item;
            if (semi == value.size()) break;
            continue;
        }
        if (item.empty()) { why = "empty option"; return false; }
        const size_t eq = item.find_first_of(":=");
        if (eq == std::string::npos) { why = "option without : or ="; return false; }
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        float f = 0;
        auto flag = [&](bool& b) { if (v == "1") b = true; else if (v == "0") b = false; else return false; return true; };
        bool ok = true;
        if (k == "yaw") { ok = num(v, f); c.yaw_offset_deg = f; }
        else if (k == "sgn") { ok = num(v, f) && (f == 1.0f || f == -1.0f); c.yaw_sign = f; }
        else if (k == "psgn") { ok = num(v, f) && (f == 1.0f || f == -1.0f); c.pitch_sign = f; }
        else if (k == "rsgn") { ok = num(v, f) && (f == 1.0f || f == -1.0f); c.roll_sign = f; }
        else if (k == "mirror") ok = flag(c.mirror_x);
        else if (k == "blend") ok = flag(c.blend_window);
        else if (k == "alt") { ok = num(v, f); c.alt_offset = f; }
        else if (k == "fixy") { ok = num(v, f); c.fixed_alt = true; c.fixed_alt_value = f; }
        else if (k == "sky" || k == "light" || k == "cloud" || k == "clouds" || k == "water" || k == "env") {   // WORLD-2 environment sub-options
            bool b = false; ok = flag(b);
            const uint32_t bit = k == "sky" ? kEnvSky : k == "light" ? kEnvLight : k == "water" ? kEnvWater : k == "env" ? kEnvAll : kEnvCloud;
            c.env_bits = b ? (c.env_bits | bit) : (c.env_bits & ~bit);
        }
        else if (k == "upm") { ok = num(v, f) && f >= 1.0f && f <= 1000.0f; c.head_units_per_metre = f; c.upm_set = true; }
        else if (k == "wscale") { ok = num(v, f) && f >= acz::world_scale::kWScaleMin && f <= acz::world_scale::kWScaleMax; c.world_scale = f; }   // WORLD-SCALE
        else if (k == "csrc") { if (v == "game") c.cam_source_game = true; else if (v == "e6") c.cam_source_game = false; else ok = false; }
        else if (k == "e6") { if (v == "frame") c.e6_frame = true; else if (v == "latest") c.e6_frame = false; else ok = false; }
        else if (k == "gmax") { ok = num(v, f) && f > 0.0f && f <= 180.0f; c.game_cam_max_deg = f; }
        else if (k == "occl") ok = flag(c.occlude_objects);
        else if (k == "camadmit") ok = flag(c.cam_admit);                                                            // CAM-EPOCH
        else if (k == "camage") { ok = num(v, f) && f >= 20.0f && f <= 2000.0f; c.cam_age_ms = f; }               // CAM-EPOCH
        else if (k == "camlog") ok = flag(c.cam_log);                                                               // CAM-EPOCH
        else if (k == "camwin") { ok = num(v, f) && f >= 0.0f && f <= 200.0f; c.cam_window_ms = f; }              // CAM-EPOCH
        else if (k == "fogl") { ok = num(v, f) && f >= 1000.0f && f <= 1e6f; c.haze_l = f; c.haze_l_set = true; }
        else if (k == "look") ok = flag(c.look);   // WORLD-4 fitted game look (needs sky:1 / env:1; checked below)
        else if (k == "gamecloud") { bool b = false; ok = flag(b); c.game_cloud = b ? 1 : 0; }   // EFFECTS-OUTSIDE (A)
        else if (k == "fxmem") { ok = (v == "0" || v == "1" || v == "2" || v == "3" || v == "4" || v == "5" || v == "6"); if (ok) c.fx_memory = v[0] - '0'; }   // EFFECTS-OUTSIDE (C): 2 = + replay dry run; (D) 3 = 1 + draw census; (C) 4 = 2 + GPU replay per eye; (F) 6 = 5 + sprite GPU replay per eye
        else if (k == "fxsig") {   // EFFECTS-OUTSIDE (D): fxsig:d34ee153 (exactly 8 hex digits, non-zero); active with any fxmem >= 1
            uint32_t x = 0; ok = v.size() == 8;
            for (size_t i = 0; ok && i < 8; ++i) { const char ch = v[i]; const int dg = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; if (dg < 0) ok = false; else x = (x << 4) | uint32_t(dg); }
            ok = ok && x != 0; if (ok) c.fx_sig = x;
        }
        else if (k == "fxtrail") { ok = num(v, f) && (f == 0.0f || (f >= 1.0f && f <= 30.0f)); if (ok) c.fx_trail_s = f; }   // EFFECTS-OUTSIDE (M) fx_puff_trail.h
        else if (k == "fxtmax") { ok = num(v, f) && f >= 50.0f && f <= 100000.0f; if (ok) c.fx_trail_max_edge = f; }      // (M)
        else if (k == "fxtcls") { ok = v == "near" || v == "size"; if (ok) c.fx_trail_cls = v == "size" ? 1 : 0; }         // (M3)
        else if (k == "fxtnear") { ok = num(v, f) && f >= 100.0f && f <= 50000.0f; if (ok) c.fx_trail_near = f; }       // (M3)
        else if (k == "fxcloud") { ok = num(v, f) && (f == 0.0f || (f >= 10.0f && f <= 600.0f)); if (ok) c.fx_cloud_s = f; }   // (M4) fx_puff_trail.h cloud class
        else if (k == "fxcfar") { ok = num(v, f) && f >= 5000.0f && f <= 200000.0f; if (ok) c.fx_cloud_far = f; }       // (M4)
        else if (k == "fxtxkey") ok = flag(c.fx_trail_xkey);                                                         // (M4)
        else if (k == "fxatlas") ok = flag(c.fx_atlas);                                                              // (M6) fx_puff_atlas.h
        else if (k == "fxtclip") { ok = v == "off" || v == "0" || v == "fade" || v == "1" || v == "hide" || v == "2";      // (M4c)
                                   if (ok) c.fx_trail_clip = (v == "off" || v == "0") ? 0 : (v == "hide" || v == "2") ? 2 : 1; }
        else if (k == "hudrs") ok = flag(c.hud_rs_skip);   // HUD-RS-SKIP (default 1; hudrs:0 replays SUBTRACT/REV_SUBTRACT HUD draws again)
        else if (k == "fxadd") ok = flag(c.fx_add);       // EFFECTS-OUTSIDE (P) (default 0; fxadd:1 = the additive 4caf6e2d family + point lists per eye, fxmem:6)
        else if (k == "hudlate") ok = flag(c.hud_late);   // HUD-LATE (default 0; hudlate:1 replays the post-tone-map HUD text, radio subtitles, into the HUD image)
        else if (k == "q") { ok = (v == "0" || v == "1" || v == "2"); if (ok) c.quality = uint32_t(v[0] - '0'); }   // GPU-COST quality
        else if (k == "far" || k == "fog1") { ok = num(v, f) && f >= 10000.0f && f <= 400000.0f; c.far_dist = f; }   // fog1 = the legacy name of the march end
        else if (k == "fog0") { why = "fog0 was removed (WORLD-1: the game's haze has no start distance; use fogl:<L>)"; return false; }
        else if (k == "sea") { if (v == "off") c.sea_off = true; else { ok = num(v, f); c.sea_override = f; c.sea_override_set = true; } }
        else if (k == "seamask") ok = flag(c.sea_mask);                                                             // D-344 C1
        else if (k == "lookfog") { if (v == "stage") c.look_fog_fit = false; else if (v == "fit") c.look_fog_fit = true; else ok = false; }   // D-344 C2
        else if (k == "cam") {   // x,y,z,yaw[,pitch[,roll]] (degrees; PROPS-SHADING: pitch/roll for close-up captures, |pitch| <= 89, |roll| <= 180)
            float a[6] = {0, 0, 0, 0, 0, 0}; size_t p2 = 0; int n = 0;
            for (;;) {
                size_t c2 = v.find(',', p2); if (c2 == std::string::npos) c2 = v.size();
                if (n >= 6 || !num(v.substr(p2, c2 - p2), a[n])) { ok = false; break; }
                ++n; p2 = c2 + 1;
                if (c2 == v.size()) break;
            }
            ok = ok && n >= 4 && n <= 6 && std::fabs(a[4]) <= 89.0f && std::fabs(a[5]) <= 180.0f;
            if (ok) { c.fixed_cam = true; for (int i = 0; i < 6; ++i) c.cam[i] = a[i]; }
        }
        else if (k == "objdom") { if (v == "display") c.obj_display = true; else if (v == "raw") c.obj_display = false; else ok = false; }   // PROPS-SHADING
        else if (k == "takeoff") {   // TAKEOFF: off | a stage id (the airbase map of the takeoff segment)
            if (v == "off" || v == "0") c.takeoff_id[0] = 0;
            else if (valid_stage_id(v)) { std::memset(c.takeoff_id, 0, sizeof c.takeoff_id); std::memcpy(c.takeoff_id, v.data(), v.size()); }
            else ok = false;
        }
        else if (k == "toenv") { if (v == "mission") c.takeoff_env_own = false; else if (v == "own") c.takeoff_env_own = true; else ok = false; }   // TAKEOFF-LOOK
        else if (k == "lflare") ok = flag(c.lens_flare);                                                            // LENS-FLARE
        else if (k == "lflarek") { ok = num(v, f) && f >= 0.0f && f <= acz::lensflare::kStrengthMax; c.lens_flare_k = f; }   // LENS-FLARE strength
        else if (k == "wcloud") ok = flag(c.wcloud);                                                                 // WORLD-CLOUDS
        else if (k == "wcalt") { ok = num(v, f) && f >= -2000.0f && f <= 60000.0f; c.wc_alt = f; }
        else if (k == "wcthk") { ok = num(v, f) && f >= 0.0f && f <= 30000.0f; c.wc_thick = f; }
        else if (k == "wcsize") { ok = num(v, f) && f >= 200.0f && f <= 20000.0f; c.wc_size = f; }
        else if (k == "wccell") { ok = num(v, f) && f >= 1000.0f && f <= 100000.0f; c.wc_cell = f; c.wc_cell_set = true; }
        else if (k == "wcop") { ok = num(v, f) && f > 0.0f && f <= 1.0f; c.wc_opacity = f; }
        else if (k == "wcfill") { ok = num(v, f) && f > 0.0f && f <= 1.0f; c.wc_fill = f; }                         // WCLOUD-FIX
        else if (k == "wcdrop") { ok = num(v, f) && (f == 0.0f || (f >= 200.0f && f <= 100000.0f)); c.wc_drop_edge = f; }
        else if (k == "wcdiag") { if (v == "2") { c.wc_diag = true; c.wc_tint = true; } else { ok = flag(c.wc_diag); c.wc_tint = false; } }   // 2 = + magenta tint
        else if (k == "dtex") ok = flag(c.detail);                                                                   // DTEX detail texture
        else if (k == "tclut") { if (v == "csm1") c.clut_raw = false; else if (v == "raw") c.clut_raw = true; else ok = false; }   // K00A-SNOW
        else { why = "unknown option " + k; return false; }
        if (!ok) { why = "bad value for " + k; return false; }
        if (semi == value.size()) break;
    }
    if (first) { why = "empty"; return false; }
    // WORLD-SCALE: one world head scale (world_scale.h); wscale:1 and no upm: = the evidenced 5.0 u/m.
    c.head_units_per_metre = acz::world_scale::world_head_units_per_metre(c.upm_set ? c.head_units_per_metre : 0.0f, c.world_scale);
    if (!acz::world_scale::final_scale_supported(c.head_units_per_metre)) { why = "upm x wscale must be within 1..100 (the world-object producer's supported range)"; return false; }
    if (!c.wc_cell_set) c.wc_cell = std::max(acz::wcloud::kDefCell, acz::wcloud::kCellPerSize * c.wc_size);   // WCLOUD-FIX: the densest legal grid
    if (c.wc_cell < acz::wcloud::kCellPerSize * c.wc_size) { why = "wccell must be >= 2.5 x wcsize (a puff must fit its cell)"; return false; }   // WORLD-CLOUDS
    if (c.look) {
        if (!(c.env_bits & kEnvSky)) { why = "look:1 needs sky:1 or env:1 (the look lives in the environment texture)"; return false; }
        if (!c.haze_l_set) c.haze_l = kLookHazeL;
    }
    return true;
}
void apply_sea_convention(StageData& s, const Conventions& c) noexcept {
    if (c.sea_off) s.flags &= ~kFlagSeaPresent;
    else if (c.sea_override_set) { s.sea_level = c.sea_override; s.flags |= kFlagSeaPresent | kFlagSeaGuess; }
    else if (c.sea_mask && (s.flags & kFlagSeaPresent) && (s.flags & kFlagSeaGuess) && !sea_tiles_flagged(s))
        s.flags &= ~kFlagSeaPresent;   // T4-SEA: a GUESSED plane on a stage without bit-13 sea tiles is the pre-STAGE-SWEEP false sea (stage_baker.sea_rule)
    s.sea_masked = c.sea_mask && sea_tiles_flagged(s);
}
bool sea_tiles_flagged(const StageData& s) noexcept {
    if (s.words.empty()) return false;
    size_t n = 0;
    for (uint16_t w : s.words) if (w & kTileSeaBit) ++n;
    return double(n) >= double(kSeaMaskMinFrac) * double(s.words.size());
}
bool sea_water_at(const StageData& s, float x, float z, float hraw) noexcept {
    if (!s.sea_present() || !(hraw <= s.sea_level + kSeaEps)) return false;
    if (!s.sea_masked || s.words.size() != size_t(kTGrid) * kTGrid) return true;
    float u = (x + s.half) / s.cell * 0.5f, v = (z + s.half) / s.cell * 0.5f;   // the tile cell, exactly as the land branch of trace_env
    u = u > 0 ? (u < 255.999f ? u : 255.999f) : 0.0f; v = v > 0 ? (v < 255.999f ? v : 255.999f) : 0.0f;
    return (s.words[size_t(int(std::floor(v))) * kTGrid + size_t(int(std::floor(u)))] & kTileSeaBit) != 0;
}
void look_fog_color(const StageData& s, const Conventions& c, float out[3]) noexcept {
    const bool stage = !c.look_fog_fit && s.env_params.size() == size_t(kEnvParams) * 4;
    for (int i = 0; i < 3; ++i) out[i] = stage ? s.env_params[size_t(kPSky + 8u) * 4 + size_t(i)] : kLookFog[i];
}
bool camera_frame(const float campos[3], float pitch, float yaw, float roll, const Conventions& c, CamFrame& o) noexcept {
    if (!std::isfinite(campos[0]) || !std::isfinite(campos[1]) || !std::isfinite(campos[2]) || !std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll)) return false;
    if (std::fabs(campos[0]) > 1e7f || std::fabs(campos[1]) > 1e7f || std::fabs(campos[2]) > 1e7f) return false;
    const double h = double(c.yaw_sign) * double(yaw) + double(c.yaw_offset_deg) * 3.14159265358979323846 / 180.0;
    const double p = double(c.pitch_sign) * double(pitch), r = double(c.roll_sign) * double(roll);
    const double ch = std::cos(h), sh = std::sin(h), cp = std::cos(p), sp = std::sin(p);
    const double f[3] = {-sh * cp, sp, -ch * cp};
    // right0 = fwd x worldUp normalised (horizontal), up0 = right0 x fwd
    double rx = -f[2], rz = f[0];                       // (fwd x (0,1,0)) = (-fz, 0, fx)
    double rl = std::sqrt(rx * rx + rz * rz);
    if (!(rl > 1e-9)) { rx = ch; rz = -sh; rl = 1.0; }  // looking straight up/down: use the heading's own right axis
    rx /= rl; rz /= rl;
    const double r0[3] = {rx, 0.0, rz};
    const double u0[3] = {r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0]}; // right x fwd
    // Roll by r about the forward axis: up = u0 cos r + r0 sin r (r = roll_sign * E6 roll; the default roll_sign -1 gives up = u0 cos - r0 sin = a LEFT
    // bank for a positive E6 roll, as the game's campos offset shows).
    const double cr = std::cos(r), sr = std::sin(r);
    for (int i = 0; i < 3; ++i) {
        double R = r0[i] * cr - u0[i] * sr, U = u0[i] * cr + r0[i] * sr;
        if (c.mirror_x) R = -R;
        o.right[i] = float(R); o.up[i] = float(U); o.fwd[i] = float(f[i]);
        o.eye[i] = campos[i];
    }
    if (c.fixed_alt) o.eye[1] = c.fixed_alt_value;
    o.eye[1] += c.alt_offset;
    return true;
}
bool camera_frame_axes(const float eye[3], const float right[3], const float up[3], const float fwd[3], const Conventions& c, CamFrame& o) noexcept {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(eye[i]) || !std::isfinite(right[i]) || !std::isfinite(up[i]) || !std::isfinite(fwd[i]) || std::fabs(eye[i]) > 1e7f) return false;
    auto dot = [](const float* a, const float* b) { return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]; };
    if (std::fabs(dot(right, right) - 1) > 1e-3 || std::fabs(dot(up, up) - 1) > 1e-3 || std::fabs(dot(fwd, fwd) - 1) > 1e-3) return false;
    if (std::fabs(dot(right, up)) > 1e-3 || std::fabs(dot(right, fwd)) > 1e-3 || std::fabs(dot(up, fwd)) > 1e-3) return false;
    const double cx = double(right[1]) * up[2] - double(right[2]) * up[1], cy = double(right[2]) * up[0] - double(right[0]) * up[2], cz = double(right[0]) * up[1] - double(right[1]) * up[0];
    if (cx * fwd[0] + cy * fwd[1] + cz * fwd[2] > -0.999) return false;   // right x up must be -fwd (the camera_frame handedness)
    for (int i = 0; i < 3; ++i) { o.right[i] = c.mirror_x ? -right[i] : right[i]; o.up[i] = up[i]; o.fwd[i] = fwd[i]; o.eye[i] = eye[i]; }
    if (c.fixed_alt) o.eye[1] = c.fixed_alt_value;
    o.eye[1] += c.alt_offset;
    return true;
}
float frame_angle_deg(const CamFrame& a, const CamFrame& b) noexcept {
    double worst = 1.0;
    const float* pa[3] = {a.right, a.up, a.fwd}; const float* pb[3] = {b.right, b.up, b.fwd};
    for (int k = 0; k < 3; ++k) {
        double d = 0, la = 0, lb = 0;
        for (int i = 0; i < 3; ++i) { d += double(pa[k][i]) * pb[k][i]; la += double(pa[k][i]) * pa[k][i]; lb += double(pb[k][i]) * pb[k][i]; }
        const double cs = (la > 0 && lb > 0) ? d / std::sqrt(la * lb) : -1.0;
        worst = std::min(worst, cs);
    }
    return float(std::acos(std::max(-1.0, std::min(1.0, worst))) * 57.29577951308232);
}
void eye_world(const CamFrame& f, const float o[3], float upm, float out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = f.eye[i] + upm * (f.right[i] * o[0] + f.up[i] * o[1] + f.fwd[i] * (-o[2]));
}

// ------------------------------------------------------------------ reference renderer
void build_atlas(const StageData& s, Atlas& a, bool clut_raw) {
    a.ntiles = s.ntiles;
    a.level[0].assign(size_t(s.ntiles) * 64 * 64 * 4, 0);
    for (uint32_t t = 0; t < s.ntiles; ++t)
        for (uint32_t i = 0; i < 64 * 64; ++i) {
            const uint8_t idx = clut_entry(s.tiles[size_t(t) * 4096 + i], clut_raw);   // K00A-SNOW: CSM1-ordered CLUT (the water key below stays a texel index)
            uint8_t* px = &a.level[0][(size_t(t) * 4096 + i) * 4];
            px[0] = s.clut[idx * 4]; px[1] = s.clut[idx * 4 + 1]; px[2] = s.clut[idx * 4 + 2]; px[3] = 255;
        }
    if (s.has_water())   // D-299: river water texels of water tiles get alpha 0 and the tile's mean LAND colour (so filtering does not smear the red key colour onto the banks)
        for (uint32_t t = 0; t < s.ntiles; ++t) {
            if (!s.tile_water[t]) continue;
            uint32_t sum[3] = {0, 0, 0}, n = 0;
            for (uint32_t i = 0; i < 64 * 64; ++i) {
                const uint8_t* px = &a.level[0][(size_t(t) * 4096 + i) * 4];
                if (s.tiles[size_t(t) * 4096 + i] == s.water_key) continue;
                for (int c = 0; c < 3; ++c) sum[c] += px[c];
                ++n;
            }
            for (uint32_t i = 0; i < 64 * 64; ++i) {
                uint8_t* px = &a.level[0][(size_t(t) * 4096 + i) * 4];
                if (s.tiles[size_t(t) * 4096 + i] != s.water_key) continue;
                for (int c = 0; c < 3; ++c) px[c] = n ? uint8_t((sum[c] + n / 2) / n) : uint8_t(20);
                px[3] = 0;
            }
        }
    for (int L = 1; L < 7; ++L) {
        const uint32_t sz = 64u >> L, ps = sz * 2;
        a.level[L].assign(size_t(s.ntiles) * sz * sz * 4, 0);
        for (uint32_t t = 0; t < s.ntiles; ++t)
            for (uint32_t y = 0; y < sz; ++y)
                for (uint32_t x = 0; x < sz; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const uint8_t* src = &a.level[L - 1][size_t(t) * ps * ps * 4];
                        const uint32_t sum = src[((2 * y) * ps + 2 * x) * 4 + c] + src[((2 * y) * ps + 2 * x + 1) * 4 + c] +
                            src[((2 * y + 1) * ps + 2 * x) * 4 + c] + src[((2 * y + 1) * ps + 2 * x + 1) * 4 + c];
                        a.level[L][(size_t(t) * sz * sz + y * sz + x) * 4 + c] = uint8_t((sum + 2) >> 2);
                    }
    }
}
namespace {
void sample_level(const Atlas& a, uint32_t id, int L, float u, float v, float out[4]) noexcept {
    const int sz = 64 >> L;
    const float px = u * sz - 0.5f, py = v * sz - 0.5f;
    const float fx = std::floor(px), fy = std::floor(py);
    const float wx = px - fx, wy = py - fy;
    const int x0 = int(fx), y0 = int(fy);
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int xx = std::min(std::max(x0 + i, 0), sz - 1), yy = std::min(std::max(y0 + j, 0), sz - 1);
            const float w = (i ? wx : 1 - wx) * (j ? wy : 1 - wy);
            const uint8_t* p = &a.level[L][(size_t(id) * sz * sz + size_t(yy) * sz + xx) * 4];
            for (int c = 0; c < 4; ++c) acc[c] += w * (float(p[c]) / 255.0f);
        }
    for (int c = 0; c < 4; ++c) out[c] = acc[c];
}
} // namespace
void atlas_sample(const Atlas& a, uint32_t id, float u, float v, float lod, float out[4]) noexcept {
    if (a.ntiles == 0) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    if (id >= a.ntiles) id = 0;
    lod = lod < 0 ? 0 : lod > 6 ? 6 : lod;
    const int l0 = int(std::floor(lod)); const int l1 = std::min(l0 + 1, 6); const float f = lod - float(l0);
    float c0[4], c1[4];
    sample_level(a, id, l0, u, v, c0);
    if (f > 0) sample_level(a, id, l1, u, v, c1); else { for (int c = 0; c < 4; ++c) c1[c] = c0[c]; }
    for (int c = 0; c < 4; ++c) out[c] = lerpf(c0[c], c1[c], f);
}
// ------------------------------------------------------------------ DTEX detail texture (docs/world-render/detail-texture.md)
std::string detail_path(const std::string& id, const std::string& dir_override) {
    const std::string p = stage_path(id, dir_override);
    return p.empty() ? p : p.substr(0, p.size() - 5) + ".zvdt";
}
LoadStatus parse_detail(const uint8_t* d, size_t n, DetailData& out) noexcept {
    try {
        if (!d || n < kZvdtHeader) return LoadStatus::too_small;
        if (n > kZvdtBytes) return LoadStatus::too_large;
        if (std::memcmp(d, "ZVDT", 4) != 0) return LoadStatus::bad_magic;
        if (rd32(d + 4) != 1u) return LoadStatus::bad_version;
        if (rd32(d + 8) != kDtDim || rd32(d + 12) != kDtDim) return LoadStatus::bad_header;
        if (n != kZvdtBytes) return LoadStatus::bad_size;
        for (size_t i = 96; i < kZvdtHeader; ++i) if (d[i] != 0) return LoadStatus::bad_header;
        uint8_t hash[32];
        sha256(d + kZvdtHeader, n - kZvdtHeader, hash);
        if (std::memcmp(hash, d + 32, 32) != 0) return LoadStatus::bad_hash;
        DetailData s;
        std::memcpy(s.id, d + 16, 8); s.id[8] = 0;
        if (!valid_stage_id(s.id)) return LoadStatus::bad_header;
        s.rgba.assign(d + kZvdtHeader, d + n);
        std::memcpy(s.payload_sha, d + 32, 32); std::memcpy(s.source_sha, d + 64, 32);
        out = std::move(s);
        return LoadStatus::ok;
    } catch (...) { return LoadStatus::io_error; }
}
LoadStatus load_detail_file(const std::string& path, DetailData& out) noexcept {
    try {
        if (path.empty()) return LoadStatus::io_error;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return LoadStatus::io_error;
        std::vector<uint8_t> buf(kZvdtBytes + 1);
        const size_t got = std::fread(buf.data(), 1, buf.size(), f);   // one byte more than a valid file: a longer file is too_large, never truncated to valid
        std::fclose(f);
        return parse_detail(buf.data(), got, out);
    } catch (...) { return LoadStatus::io_error; }
}
std::vector<uint8_t> serialize_detail(const DetailData& s) {
    std::vector<uint8_t> out(kZvdtHeader, 0);
    std::memcpy(out.data(), "ZVDT", 4); wr32(&out[4], 1u); wr32(&out[8], kDtDim); wr32(&out[12], kDtDim);
    std::memcpy(&out[16], s.id, 8);
    std::vector<uint8_t> px = s.rgba; px.resize(size_t(kDtDim) * kDtDim * 4, 0);
    sha256(px.data(), px.size(), &out[32]);
    std::memcpy(&out[64], s.source_sha, 32);
    out.insert(out.end(), px.begin(), px.end());
    return out;
}
DetailData make_synthetic_detail(uint32_t seed) {
    DetailData d;
    std::memcpy(d.id, "synth", 5);
    d.rgba.assign(size_t(kDtDim) * kDtDim * 4, 0);
    uint32_t r = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < kDtDim * kDtDim; ++i) {
        r = r * 1664525u + 1013904223u;
        const uint32_t x = i % kDtDim, y = i / kDtDim;
        const uint8_t g = uint8_t(((x + y) % 7u == 0u ? 14u : 0u) + ((r >> 24) % 6u));   // diagonal bright streaks + noise, 0..19 like k00a
        uint8_t* p = &d.rgba[size_t(i) * 4];
        p[0] = p[1] = p[2] = g; p[3] = 0x40;
    }
    return d;
}
bool build_detail(const DetailData& d, Atlas& a) {
    for (auto& l : a.dt) l.clear();
    const size_t n = size_t(kDtDim) * kDtDim;
    if (d.rgba.size() != n * 4) return false;
    std::vector<float> v(n * 3);
    double mean[3] = {0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        const float amp = float(d.rgba[i * 4 + 3]) / 128.0f * kDtGain;   // PS2 alpha 0x80 = 1
        for (int c = 0; c < 3; ++c) { v[i * 3 + c] = amp * float(d.rgba[i * 4 + c]) / 255.0f; mean[c] += v[i * 3 + c]; }
    }
    for (int c = 0; c < 3; ++c) mean[c] /= double(n);
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) v[i * 3 + c] -= float(mean[c]);   // zero mean: the fitted average look stays
    for (uint32_t L = 0; L < kDtLevels; ++L) {
        const uint32_t sz = kDtDim >> L;
        if (L > 0) {   // box mean of the previous FLOAT level (no double quantisation)
            std::vector<float> nv(size_t(sz) * sz * 3);
            const uint32_t ps = sz * 2;
            for (uint32_t y = 0; y < sz; ++y) for (uint32_t x = 0; x < sz; ++x) for (int c = 0; c < 3; ++c)
                nv[(size_t(y) * sz + x) * 3 + c] = 0.25f * (v[((2 * y) * ps + 2 * x) * 3 + c] + v[((2 * y) * ps + 2 * x + 1) * 3 + c] +
                                                           v[((2 * y + 1) * ps + 2 * x) * 3 + c] + v[((2 * y + 1) * ps + 2 * x + 1) * 3 + c]);
            v.swap(nv);
        }
        a.dt[L].assign(size_t(sz) * sz * 4, 255);
        for (size_t i = 0; i < size_t(sz) * sz; ++i)
            for (int c = 0; c < 3; ++c) {
                const float q = std::round(128.0f + 255.0f * v[i * 3 + c]);
                a.dt[L][i * 4 + c] = uint8_t(q < 0.0f ? 0.0f : q > 255.0f ? 255.0f : q);
            }
    }
    return true;
}
bool attach_detail(const std::string& id, const Conventions& c, Atlas& a, std::string& why) {
    for (auto& l : a.dt) l.clear();
    if (!c.detail) { why = "off"; return false; }
    DetailData d;
    const std::string p = detail_path(id);
    const LoadStatus st = load_detail_file(p, d);
    if (st != LoadStatus::ok) { why = std::string("load-failed(") + status_name(st) + ")"; return false; }
    if (id != d.id) { why = "stage-mismatch"; return false; }
    if (!build_detail(d, a)) { why = "bad-content"; return false; }
    why = "loaded";
    return true;
}
namespace {
void detail_level(const Atlas& a, int L, float u0, float v0, float out[3]) noexcept {   // bilinear, wrapping; u0/v0 in level-0 texels
    const int sz = int(kDtDim) >> L;
    const float sc = 1.0f / float(1 << L);
    const float px = u0 * sc - 0.5f, py = v0 * sc - 0.5f;
    const float fx = std::floor(px), fy = std::floor(py);
    const float wx = px - fx, wy = py - fy;
    const int x0 = int(fx), y0 = int(fy);
    float acc[3] = {0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int xx = (x0 + i) & (sz - 1), yy = (y0 + j) & (sz - 1);
            const float w = (i ? wx : 1 - wx) * (j ? wy : 1 - wy);
            const uint8_t* p = &a.dt[L][(size_t(yy) * sz + xx) * 4];
            for (int c = 0; c < 3; ++c) acc[c] += w * (float(p[c]) / 255.0f - 128.0f / 255.0f);
        }
    for (int c = 0; c < 3; ++c) out[c] = acc[c];
}
}
void detail_term(const Atlas& a, float x, float z, float t, float out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0f;
    if (!a.has_detail() || !(t < kDtFadeFar)) return;
    for (uint32_t L = 1; L < kDtLevels; ++L) if (a.dt[L].size() != size_t(kDtDim >> L) * (kDtDim >> L) * 4) return;
    float w = sat((t - kDtFadeNear) / (kDtFadeFar - kDtFadeNear));
    const float fade = 1.0f - w * w * (3.0f - 2.0f * w);
    const float tl = t * kLodScale * kDtLodMul;
    float lod = tl > 1.0f ? std::log2(tl) : 0.0f;
    lod = lod < float(kDtLevels - 1) ? lod : float(kDtLevels - 1);
    const float u = (x - kDtRepeat * std::floor(x / kDtRepeat)) * (float(kDtDim) / kDtRepeat);
    const float v = (z - kDtRepeat * std::floor(z / kDtRepeat)) * (float(kDtDim) / kDtRepeat);
    if (!std::isfinite(u) || !std::isfinite(v)) return;
    const int l0 = int(std::floor(lod)), l1 = std::min(l0 + 1, int(kDtLevels) - 1);
    const float f = lod - float(l0);
    float c0[3], c1[3];
    detail_level(a, l0, u, v, c0);
    if (f > 0) detail_level(a, l1, u, v, c1); else for (int c = 0; c < 3; ++c) c1[c] = c0[c];
    for (int c = 0; c < 3; ++c) out[c] = lerpf(c0[c], c1[c], f) * fade;
}
float height_bilinear(const StageData& s, float x, float z) noexcept {
    float u = (x + s.half) / s.cell, v = (z + s.half) / s.cell;
    // REVIEW L-4: written so a NaN coordinate clamps to 0 (the former `u < 0 ? 0 : u > 512 ? 512 : u` passed NaN through to int(floor(NaN)) and indexed
    // the height field out of bounds); identical for every non-NaN value
    u = u > 0 ? (u < 512.0f ? u : 512.0f) : 0.0f; v = v > 0 ? (v < 512.0f ? v : 512.0f) : 0.0f;
    int i = int(std::floor(u)), j = int(std::floor(v));
    if (i > 511) i = 511; if (j > 511) j = 511;
    const float fu = u - float(i), fv = v - float(j);
    const float h00 = s.heights[size_t(j) * kHDim + i], h10 = s.heights[size_t(j) * kHDim + i + 1];
    const float h01 = s.heights[size_t(j + 1) * kHDim + i], h11 = s.heights[size_t(j + 1) * kHDim + i + 1];
    return lerpf(lerpf(h00, h10, fu), lerpf(h01, h11, fu), fv) * s.hstep;
}
TakeoffSelector::Event TakeoffSelector::update(const float eye[3], const StageData& mission, const StageData& base) noexcept {
    ++samples;
    const size_t hn = size_t(kHDim) * kHDim;
    if (!eye || !std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2]) || mission.heights.size() != hn || base.heights.size() != hn) {
        ++refused; return Event::none;
    }
    float dh = 0, d3 = 0;
    if (have_prev_) {
        const float dx = eye[0] - prev_[0], dy = eye[1] - prev_[1], dz = eye[2] - prev_[2];
        dh = std::sqrt(dx * dx + dz * dz); d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const bool cut = have_prev_ && d3 > r_.jump;
    for (int i = 0; i < 3; ++i) prev_[i] = eye[i];
    have_prev_ = true;
    hb_ = height_bilinear(base, eye[0], eye[2]);
    hm_ = height_bilinear(mission, eye[0], eye[2]);
    if (cut) {
        ++jumps; run_ = 0; dh = 0;   // the cut sample itself has no usable motion
        // a cut re-arms only near the first takeoff spot (a mission restart puts the aircraft back on the runway; the airbase -> mission cut lands far away)
        if (have_spot_) {
            const float sx = eye[0] - spot_[0], sz = eye[2] - spot_[1];
            armed_ = std::sqrt(sx * sx + sz * sz) <= r_.rearm_radius;
        } else armed_ = true;
        if (active_) { active_ = false; ++leaves; reason_ = "cut"; return Event::leave; }
    }
    if (active_) {
        if (eye[1] < hb_ - r_.below) { active_ = false; armed_ = false; run_ = 0; ++leaves; reason_ = "under-base-ground"; return Event::leave; }
        return Event::none;
    }
    if (!armed_) return Event::none;
    const float above_base = eye[1] - hb_, above_mission = eye[1] - hm_;
    const bool q = above_base >= r_.band_lo && above_base <= r_.band_hi && above_mission >= r_.mission_clear && dh <= r_.still;
    if (!q) { run_ = 0; return Event::none; }
    if (++run_ < r_.enter) return Event::none;
    active_ = true; ++enters; reason_ = "parked-on-base-ground";
    if (!have_spot_) { spot_[0] = eye[0]; spot_[1] = eye[2]; have_spot_ = true; }
    return Event::enter;
}
float height_effective(const StageData& s, float x, float z) noexcept {
    const float h = height_bilinear(s, x, z);
    return s.sea_present() && s.sea_level > h ? s.sea_level : h;
}
void sky_color(const float d[3], float out[3]) noexcept {
    // elevation (degrees) of the world direction; piecewise linear over kSkyElevDeg in the DISPLAY domain, constant beyond the ends
    const float e = std::asin(d[1] < -1.0f ? -1.0f : d[1] > 1.0f ? 1.0f : d[1]) * 57.29577951f;
    for (int c = 0; c < 3; ++c) out[c] = kSkyTable[0][c];
    for (unsigned k = 1; k < kSkyRows; ++k) {
        const float w = sat((e - kSkyElevDeg[k - 1]) / (kSkyElevDeg[k] - kSkyElevDeg[k - 1]));
        for (int c = 0; c < 3; ++c) out[c] += w * (kSkyTable[k][c] - kSkyTable[k - 1][c]);
    }
}
// ------------------------------------------------------------------ WORLD-2 environment (mirrors t_env_* in xr_stereo_blit.hlsl)
namespace {
inline float smooth01(float lo, float hi, float x) noexcept { const float t = sat((x - lo) / (hi - lo)); return t * t * (3.0f - 2.0f * t); }
inline const float* envp(const EnvView& e, uint32_t i) noexcept { return e.tex + size_t(i) * 4; }
inline float env_texel(const EnvView& e, int x, int y, int ch) noexcept { return e.tex[(size_t(1 + y) * kEnvTexW + size_t(x)) * 4 + size_t(ch)]; }
// bilinear over the 256x256 map region (wrap) or the 32x32 sprite (clamp); u,v in texels
float env_map(const EnvView& e, int ch, float u, float v, bool wrap) noexcept {
    const float px = u - 0.5f, py = v - 0.5f;
    const float fx = std::floor(px), fy = std::floor(py);
    const float wx = px - fx, wy = py - fy;
    const int x0 = int(fx), y0 = int(fy);
    float acc = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int xx = x0 + i, yy = y0 + j;
            if (wrap) { xx &= 255; yy &= 255; }
            else { xx = std::min(std::max(xx, 0), int(kEnvSprite) - 1); yy = std::min(std::max(yy, 0), int(kEnvSprite) - 1); }
            acc += (i ? wx : 1 - wx) * (j ? wy : 1 - wy) * env_texel(e, xx, yy, ch);
        }
    return acc;
}
inline bool look_on(const EnvView& e) noexcept { return e.tex != nullptr && envp(e, kPLook)[3] > 0.5f; }
void sky_at(const EnvView& e, const float d[3], float el, float out[3]) noexcept {
    const float* st = envp(e, kPSkyStops);
    const float* ref = envp(e, kPMoon)[3] > 0.0f ? envp(e, kPMoon) : envp(e, kPLight);
    const float hl = std::sqrt(ref[0] * ref[0] + ref[2] * ref[2]);
    const float c = hl > 1e-4f ? (d[0] * ref[0] + d[2] * ref[2]) / hl : 0.0f;   // scaled by cos(elevation): the azimuth term fades to row 1 at the zenith (no seam there)
    const float t = 1.0f - c;
    if (look_on(e)) {   // WORLD-4 look:1: el is SIGNED here (below the horizon the low sky runs down to the fog colour F at e_lo)
        const float* ls = envp(e, kPLookSky); const float* F = envp(e, kPLookFog);
        const float k = ls[0], elo = ls[1], e1 = ls[2], e2 = ls[3], e3 = envp(e, kPLookSky2)[0];
        float row[3][3];
        for (int r = 0; r < 3; ++r) {
            const float* c1 = envp(e, kPSky + 4u * uint32_t(r) + 1u); const float* c2 = envp(e, kPSky + 4u * uint32_t(r) + 2u); const float* c3 = envp(e, kPSky + 4u * uint32_t(r) + 3u);
            for (int ch = 0; ch < 3; ++ch) {
                const float g = k * c1[ch];
                if (el < e1) row[r][ch] = lerpf(F[ch], g, smooth01(elo, e1, el));
                else if (el < e2) row[r][ch] = lerpf(g, c2[ch], sat((el - e1) / (e2 - e1)));
                else { float f3 = sat((el - e2) / (e3 - e2)); f3 = f3 * (2.0f - f3); row[r][ch] = lerpf(c2[ch], c3[ch], f3); }
            }
        }
        const float a1 = smooth01(0.0f, 1.0f, t), a2 = smooth01(1.0f, 2.0f, t);
        for (int ch = 0; ch < 3; ++ch) out[ch] = lerpf(lerpf(row[0][ch], row[1][ch], a1), row[2][ch], a2);
        return;
    }
    int k = 0; float f;
    if (el <= st[1]) { k = 0; f = sat((el - st[0]) / (st[1] - st[0])); }
    else if (el <= st[2]) { k = 1; f = sat((el - st[1]) / (st[2] - st[1])); }
    else { k = 2; f = sat((el - st[2]) / (st[3] - st[2])); f = f * (2.0f - f); }   // ease-out: no slope break (visible edge) at the top stop; matches the measured decaying slope
    float row[3][3];
    for (int r = 0; r < 3; ++r) {
        const float* a = envp(e, kPSky + 4u * r + uint32_t(k)); const float* b = envp(e, kPSky + 4u * r + uint32_t(k) + 1u);
        for (int ch = 0; ch < 3; ++ch) row[r][ch] = lerpf(a[ch], b[ch], f);
    }
    const float a1 = smooth01(0.0f, 1.0f, t), a2 = smooth01(1.0f, 2.0f, t);   // C1 azimuth blend (a linear blend shows an edge at 90 degrees)
    for (int ch = 0; ch < 3; ++ch) out[ch] = lerpf(lerpf(row[0][ch], row[1][ch], a1), row[2][ch], a2);
}
float cloud_alpha(const EnvView& e, float x, float z) noexcept {
    const float rep = envp(e, kPMisc)[1];
    const float u = x / rep * 256.0f, v = z / rep * 256.0f;
    const float cov = (2.0f * env_map(e, 0, u, v, true) + env_map(e, 0, u + kCloudBlur, v, true) + env_map(e, 0, u - kCloudBlur, v, true) +
                       env_map(e, 0, u, v + kCloudBlur, true) + env_map(e, 0, u, v - kCloudBlur, true)) / 6.0f;
    return smooth01(kCloudCovLo, kCloudCovHi, cov) * kCloudOpacity;
}
inline bool cloud_on(const EnvView& e) noexcept { return (e.bits & kEnvCloud) != 0 && envp(e, kPCloudOn)[0] > 0.5f; }
} // namespace
void env_sky(const EnvView& e, const float d[3], bool sprite, float out[3]) noexcept {
    const float els = std::asin(std::min(std::max(d[1], -1.0f), 1.0f));
    const float el = look_on(e) ? els : std::max(els, 0.0f);   // WORLD-4: the look sky continues below the horizon
    sky_at(e, d, el, out);
    const float* sp = envp(e, kPMoon);
    const float* L = envp(e, kPLight);
    if (sprite && L[3] > 0.0f && !(sp[3] > 0.0f && sp[0] * L[0] + sp[1] * L[1] + sp[2] * L[2] > kSunMoonSame)) {   // sun disc + glow at the light (not when the light IS the moon)
        const float cs = d[0] * L[0] + d[1] * L[1] + d[2] * L[2];
        if (cs > 0.0f) {
            const float* lc = envp(e, kPLightCol);
            const float m = std::max(std::max(lc[0], lc[1]), std::max(lc[2], 1e-3f));
            const float disc = smooth01(kSunCos0, kSunCos1, cs), glow = std::pow(cs, kSunGlowPow) * kSunGlow;
            for (int ch = 0; ch < 3; ++ch) out[ch] = sat(lerpf(out[ch] + lc[ch] / m * glow, lc[ch] / m, disc));
        }
    }
    if (!sprite || !(sp[3] > 0.0f)) return;
    const float q = d[0] * sp[0] + d[1] * sp[1] + d[2] * sp[2];
    const float rl = std::sqrt(sp[0] * sp[0] + sp[2] * sp[2]);
    if (!(q > 0.0f) || !(rl > 1e-4f)) return;
    const float R[3] = {-sp[2] / rl, 0.0f, sp[0] / rl};
    const float U[3] = {R[1] * sp[2] - R[2] * sp[1], R[2] * sp[0] - R[0] * sp[2], R[0] * sp[1] - R[1] * sp[0]};
    const float ts = envp(e, kPMisc)[0];
    const float u = (d[0] * R[0] + d[1] * R[1] + d[2] * R[2]) / (q * ts), v = (d[0] * U[0] + d[1] * U[1] + d[2] * U[2]) / (q * ts);
    if (!(std::fabs(u) < 1.0f && std::fabs(v) < 1.0f)) return;
    const float tu = (u * 0.5f + 0.5f) * float(kEnvSprite), tv = (0.5f - v * 0.5f) * float(kEnvSprite);
    const float gi = env_map(e, 2, tu, tv, false), ga = env_map(e, 3, tu, tv, false) * sp[3];
    for (int ch = 0; ch < 3; ++ch) out[ch] = sat(out[ch] + gi * ga);   // additive: the moon texture's dark square has alpha 0x43 too (the game shows only a brightness excess)
}
void env_haze(const EnvView& e, const float d[3], float out[3]) noexcept {
    // WORLD-4 look:1: the haze target is the low sky at the view ray's signed elevation (F looking down, the glow toward the horizon), as the game's
    // ground fades into the sky gradient below the horizon line; look:0 = WORLD-2's horizon colour along the azimuth.
    sky_at(e, d, look_on(e) ? std::asin(std::min(std::max(d[1], -1.0f), 1.0f)) : 0.0f, out);
}
StageLook stage_look(const char* stage_id) noexcept {
    StageLook l{{kLookGain[0], kLookGain[1], kLookGain[2]}, false, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
    auto is = [&](const char* s) { return stage_id && std::strncmp(stage_id, s, 9) == 0; };
    if (is("k00a")) {   // SNOW-LOOK (terrain_stage.h): the airbase
        l = StageLook{{kK00aGain[0], kK00aGain[1], kK00aGain[2]}, true, {kK00aSky[0], kK00aSky[1], kK00aSky[2]}, kK00aHazeFloor, kK00aDesat, kK00aHazePow, kK00aPropsGain,
                   {kK00aFarTint[0], kK00aFarTint[1], kK00aFarTint[2]}, kK00aFarF0, kK00aFarF1};
    } else if (is("m01a") || is("m15a")) {   // SNOW-LOOK: neutral snow, own sky
        for (float& g : l.gain) g = kLookGainSnowFit;
    }
    return l;
}
void look_gain_for(const char* stage_id, float out[3]) noexcept {
    const StageLook l = stage_look(stage_id);
    for (int c = 0; c < 3; ++c) out[c] = l.gain[c];
}
void look_haze_tint(const EnvView& e, float f, float hz[3]) noexcept {   // K00A-FIT3 (mirrors te_look_tint)
    if (!e.bits || !look_on(e)) return;
    const float* W = envp(e, kPLookFar); const float f0 = W[3], f1 = envp(e, kPLookFar2)[0];
    if (!(f0 > 0.0f) || !(f > 0.0f)) return;
    const float w = f < f0 ? f / f0 : (f < f1 ? 1.0f - (f - f0) / (f1 - f0) : 0.0f);
    for (int c = 0; c < 3; ++c) hz[c] += (W[c] - hz[c]) * w;
}
float look_haze(const EnvView& e, float f) noexcept {   // SNOW-LOOK haze floor (P[kPLookFog].w; 0 = unchanged)
    if (!e.bits || !look_on(e)) return f;   // the shader applies it only with an env bit set (tenv != 0)
    const float fl = envp(e, kPLookFog)[3], pw = envp(e, kPLookSky2)[2];
    if (pw > 0.0f) f = std::pow(std::max(f, 0.0f), pw);   // K00A-FIT
    return fl > 0.0f ? fl + (1.0f - fl) * f : f;
}
bool adopt_mission_env(StageData& takeoff, const StageData& mission, const Conventions& c) {
    if (c.takeoff_env_own || !mission.has_env() || &takeoff == &mission) return false;
    takeoff.env_params = mission.env_params;
    takeoff.env_cloud = mission.env_cloud; takeoff.env_wave = mission.env_wave;
    takeoff.env_sprite_i = mission.env_sprite_i; takeoff.env_sprite_a = mission.env_sprite_a;
    takeoff.flags |= kFlagEnv;
    std::memcpy(takeoff.env_id, mission.env_id[0] ? mission.env_id : mission.id, sizeof takeoff.env_id);   // WCLOUD-FIX: the mission's cloud band
    return true;
}
bool props_look_on(const EnvView& e) noexcept { return e.bits && look_on(e) && envp(e, kPLookSky2)[3] > 0.0f; }
void look_gain_props(const EnvView& e, float col[3]) noexcept {   // K00A-FIT2: look_gain x the props trim (no-op unless props_look_on)
    if (!props_look_on(e)) return;
    // mirrors xr_props_composite.hlsli: col = saturate(albedo); col = saturate(te_look_gain(col) * trim), both BEFORE the fog blend (W8-2 P2)
    const auto sat = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    for (int c = 0; c < 3; ++c) col[c] = sat(col[c]);
    look_gain(e, col);
    for (int c = 0; c < 3; ++c) col[c] = sat(col[c] * envp(e, kPLookSky2)[3]);
}
void look_gain(const EnvView& e, float col[3]) noexcept {
    if (!look_on(e)) return;
    const float* g = envp(e, kPLook);
    const float ds = envp(e, kPLookSky2)[1];   // K00A-FIT desaturation toward the luminance (0 = none)
    if (ds > 0.0f) {
        const float L = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2];
        for (int c = 0; c < 3; ++c) col[c] += (L - col[c]) * ds;
    }
    for (int c = 0; c < 3; ++c) col[c] = col[c] * g[c];
}
void build_env_texture(const StageData& s, const Conventions& c, std::vector<float>& tex) {
    tex.clear();
    if (!s.has_env() || s.env_cloud.size() != kEnvMap * kEnvMap || s.env_wave.size() != kEnvWave * kEnvWave ||
        s.env_sprite_i.size() != kEnvSprite * kEnvSprite || s.env_sprite_a.size() != kEnvSprite * kEnvSprite) return;
    tex.assign(size_t(kEnvTexW) * kEnvTexH * 4, 0.0f);
    std::memcpy(tex.data(), s.env_params.data(), size_t(kEnvParams) * 16);
    // directions: the baked heading convention -> stage frame through the same yaw convention as the camera (default identity)
    const double off = double(c.yaw_offset_deg) * 3.14159265358979323846 / 180.0;
    for (uint32_t i : {uint32_t(kPMoon), uint32_t(kPLight)}) {
        float* v = &tex[size_t(i) * 4];
        const double hl = std::sqrt(double(v[0]) * v[0] + double(v[2]) * v[2]);
        if (hl < 1e-9) continue;
        const double a = std::atan2(-double(v[0]), -double(v[2]));   // heading of the baked direction
        const double a2 = double(c.yaw_sign) * a + off;
        v[0] = float(-std::sin(a2) * hl); v[2] = float(-std::cos(a2) * hl);   // (mirror is a camera-axis flip, not a world flip: not applied)
    }
    if (c.look) {   // WORLD-4 look:1 (P[26..29] are zero in every bake; written only here)
        const float dg = 3.14159265f / 180.0f;
        float* p = tex.data();
        auto P = [&](uint32_t i, float a, float b, float cc, float w) { p[i * 4] = a; p[i * 4 + 1] = b; p[i * 4 + 2] = cc; p[i * 4 + 3] = w; };
        const StageLook sl = stage_look(s.id);   // SNOW-LOOK: per-stage gain / sky / haze floor (kLookGain, own sky, no floor without a profile)
        P(kPLook, sl.gain[0], sl.gain[1], sl.gain[2], 1.0f);
        float F[3]; look_fog_color(s, c, F);   // D-344 C2: the stage's own sph row-2 horizon colour (lookfog:fit = the m02a constant)
        if (sl.sky_set) {   // SNOW-LOOK: a flat sky = the profile colour (the mission's env adopted by toenv:mission or the stage's own); k x C1 = sky
            for (int i = 0; i < 3; ++i) F[i] = sl.sky[i];
            for (uint32_t r = 0; r < 3; ++r)
                for (uint32_t k = 0; k < 4; ++k) {
                    const float sc = k == 1 ? 1.0f / kLookK : 1.0f;
                    P(kPSky + 4u * r + k, std::min(sl.sky[0] * sc, 1.0f), std::min(sl.sky[1] * sc, 1.0f), std::min(sl.sky[2] * sc, 1.0f), 0.0f);
                }
        }
        P(kPLookFog, F[0], F[1], F[2], sl.haze_floor);
        P(kPLookSky, kLookK, kLookELoDeg * dg, kLookE1Deg * dg, kLookE2Deg * dg);
        P(kPLookSky2, kLookE3Deg * dg, sl.desat, sl.haze_pow, sl.props_gain);
        P(kPLookFar, sl.far_tint[0], sl.far_tint[1], sl.far_tint[2], sl.far_f0);
        P(kPLookFar2, sl.far_f1, 0.0f, 0.0f, 0.0f);
        p[kPMoon * 4 + 3] *= kLookMoon;   // the game's moon is a faint brightness excess (~10/255)
    }
    acz::lensflare::param_texel(c.lens_flare, c.lens_flare_k, &tex[size_t(kPFlare) * 4]);   // LENS-FLARE (zero = off)
    acz::wcloud::param_texels(c.wcloud, s.env_id[0] ? s.env_id : s.id, c.wc_alt, s.env_params[size_t(kPCloudAlt) * 4], c.wc_thick, c.wc_cell, c.wc_size,
                              c.wc_opacity, c.wc_fill, &tex[size_t(acz::wcloud::kPWc) * 4], c.wc_tint);   // WORLD-CLOUDS (zero = off; band: option > stage census > layer 0)
    for (uint32_t y = 0; y < kEnvMap; ++y)
        for (uint32_t x = 0; x < kEnvMap; ++x) {
            float* t = &tex[(size_t(1 + y) * kEnvTexW + x) * 4];
            t[0] = float(s.env_cloud[size_t(y) * kEnvMap + x]) / 255.0f;
            t[1] = float(s.env_wave[size_t(y % kEnvWave) * kEnvWave + (x % kEnvWave)]) / 255.0f;
            if (x < kEnvSprite && y < kEnvSprite) {
                t[2] = float(s.env_sprite_i[size_t(y) * kEnvSprite + x]) / 255.0f;
                t[3] = float(s.env_sprite_a[size_t(y) * kEnvSprite + x]) / 255.0f;
            }
        }
}
void world_clouds(const EnvView& e, const float o[3], const float d[3], float t0, float t1, float haze_l, float far_dist, WcAccum& out) noexcept {
    out = WcAccum{};
    if (!e.tex) return;
    const acz::wcloud::Params p = acz::wcloud::params_from(envp(e, acz::wcloud::kPWc), envp(e, acz::wcloud::kPWc2));
    if (!p.on) return;
    float hz[3];
    if (e.bits & kEnvSky) env_haze(e, d, hz); else for (int ch = 0; ch < 3; ++ch) hz[ch] = kHaze[ch];
    const float rep = envp(e, kPMisc)[1];
    const float* lit = envp(e, kPCloudCol + 1); const float* dark = envp(e, kPCloudCol + 2);
    acz::wcloud::Accum a;
    acz::wcloud::accumulate(p, o, d, t0, t1, far_dist,
        [&](float x, float z) { return env_map(e, 0, x / rep * acz::wcloud::kMapSize, z / rep * acz::wcloud::kMapSize, true); },
        [&](float u, float v) { return env_map(e, 0, u, v, true); },
        [&](float t) { return haze_fraction(t, haze_l, far_dist); }, hz, lit, dark, a);
    for (int ch = 0; ch < 3; ++ch) out.c[ch] = a.c[ch];
    out.a = a.a; out.cells = a.cells; out.puffs = a.puffs; out.tint = p.tint;
}
void world_clouds_apply(const WcAccum& w, float rgb[3]) noexcept {
    if (!(w.a > 0.0f)) return;
    if (w.tint) {   // wcdiag:2 (mirrors wc_over): lerp(encode(rgb), magenta, min(4 A, 1)), decoded
        static const float mg[3] = {1.0f, 0.0f, 1.0f};
        const float k = std::min(4.0f * w.a, 1.0f);
        for (int ch = 0; ch < 3; ++ch) { const float e = linear_to_srgb(rgb[ch]); rgb[ch] = srgb_to_linear(e + (mg[ch] - e) * k); }
        return;
    }
    for (int ch = 0; ch < 3; ++ch) {
        float v = w.c[ch] + (1.0f - w.a) * linear_to_srgb(rgb[ch]);
        rgb[ch] = srgb_to_linear(v < 1.0f ? v : 1.0f);
    }
}
WcProbe world_clouds_probe(const StageData& s, const Atlas& at, const EnvView& ev, const float eye[3], const float base[3], const float ux[3], const float vy[3],
                           const float right[3], const float up[3], const float fwd[3], float haze_l, float far_dist, unsigned nx, unsigned ny) noexcept {
    WcProbe r;
    if (!ev.tex || !nx || !ny) return r;
    const acz::wcloud::Params p = acz::wcloud::params_from(envp(ev, acz::wcloud::kPWc), envp(ev, acz::wcloud::kPWc2));
    r.on = p.on; r.tint = p.tint; r.alt = p.alt; r.thick = p.thick;
    if (!p.on) return r;
    double sa = 0, sdl = 0;
    for (unsigned j = 0; j < ny; ++j)
        for (unsigned i = 0; i < nx; ++i) {
            const float u = (float(i) + 0.5f) / float(nx), v = (float(j) + 0.5f) / float(ny);
            float d[3], dw[3];
            for (int k = 0; k < 3; ++k) d[k] = base[k] + u * ux[k] + v * vy[k];
            for (int k = 0; k < 3; ++k) dw[k] = right[k] * d[0] + up[k] * d[1] + fwd[k] * (-d[2]);
            const float l = std::sqrt(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]);
            if (!(l > 0.0f)) continue;
            for (int k = 0; k < 3; ++k) dw[k] /= l;
            const Trace tr = trace_env(s, at, ev, eye, dw, haze_l, far_dist);
            WcAccum w; world_clouds(ev, eye, dw, 0.0f, tr.hit ? tr.t : 3.0e38f, haze_l, far_dist, w);
            float rgb[3] = {tr.rgb[0], tr.rgb[1], tr.rgb[2]};
            const float l0 = 0.2126f * linear_to_srgb(rgb[0]) + 0.7152f * linear_to_srgb(rgb[1]) + 0.0722f * linear_to_srgb(rgb[2]);
            world_clouds_apply(w, rgb);
            const float l1 = 0.2126f * linear_to_srgb(rgb[0]) + 0.7152f * linear_to_srgb(rgb[1]) + 0.0722f * linear_to_srgb(rgb[2]);
            const float dl = std::fabs(l1 - l0) * 255.0f;
            ++r.rays; sa += w.a; sdl += dl;
            if (w.a > 0.05f) ++r.with;
            if (dl >= 8.0f) ++r.vis8;
            r.max_a = std::max(r.max_a, w.a); r.max_dl = std::max(r.max_dl, dl);
        }
    if (r.rays) { r.mean_a = float(sa / r.rays); r.mean_dl = float(sdl / r.rays); }
    return r;
}
float srgb_to_linear(float x) noexcept {
    x = sat(x);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float x) noexcept {
    x = sat(x);
    return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}
float haze_fraction(float t, float haze_l, float far_dist) noexcept {
    float w = sat((t - kFarFade * far_dist) / ((1.0f - kFarFade) * far_dist));
    w = w * w * (3.0f - 2.0f * w);
    return 1.0f - std::exp(-t / haze_l) * (1.0f - w);
}
namespace {
void finish(Trace& r) noexcept { for (int c = 0; c < 3; ++c) r.rgb[c] = srgb_to_linear(r.display[c]); }
}
Trace trace(const StageData& s, const Atlas& a, const float o[3], const float d[3], float haze_l, float far_dist) noexcept {
    return trace_env(s, a, EnvView{}, o, d, haze_l, far_dist);
}
Trace trace_env(const StageData& s, const Atlas& a, const EnvView& ev, const float o[3], const float d[3], float haze_l, float far_dist) noexcept {
    const EnvView e{ev.tex, ev.tex ? ev.bits : 0u};
    Trace r;
    const float hmax = s.sea_present() ? std::max(float(s.hmax_byte) * s.hstep, s.sea_level) : float(s.hmax_byte) * s.hstep;
    float t = 0, prev = 0;
    bool hit = false;
    unsigned i = 0;
    for (; i < kMarchSteps; ++i) {
        const float px = o[0] + d[0] * t, py = o[1] + d[1] * t, pz = o[2] + d[2] * t;
        const float h = height_effective(s, px, pz);
        const float dy = py - h;
        if (dy < 0) { hit = true; break; }
        if (py > hmax && d[1] >= 0) break;
        if (t >= far_dist) break;
        prev = t;
        t += std::min(std::max(kStepFrac * dy, kStepMin + kStepGrow * t), kStepMax);
    }
    r.steps = i;
    auto clouds = [&](float tlim) {   // WORLD-2 cloud sheet (layer 0) over whatever the ray saw before tlim (display domain, before finish())
        if (!cloud_on(e) || !(std::fabs(d[1]) > 1e-4f)) return;
        const float H = envp(e, kPCloudAlt)[0];
        const float tc = (H - o[1]) / d[1];
        if (!(tc > 0.0f && tc < tlim)) return;
        float al = cloud_alpha(e, o[0] + d[0] * tc, o[2] + d[2] * tc);
        if (!(al > 0.0f)) return;
        const float* lit = envp(e, kPCloudCol + 1); const float* dark = envp(e, kPCloudCol + 2);
        float cc[3], hz[3];
        for (int ch = 0; ch < 3; ++ch) cc[ch] = o[1] < H ? lerpf(lit[ch], dark[ch], kCloudBelowDark) : lit[ch];
        if (e.bits & kEnvSky) env_haze(e, d, hz); else for (int ch = 0; ch < 3; ++ch) hz[ch] = kHaze[ch];
        const float f = haze_fraction(tc, haze_l, far_dist);
        for (int ch = 0; ch < 3; ++ch) cc[ch] = lerpf(cc[ch], hz[ch], f);
        al *= 1.0f - f;
        for (int ch = 0; ch < 3; ++ch) r.display[ch] = cc[ch] * al + r.display[ch] * (1.0f - al);
    };
    if (!hit) {   // up: the sky; down (march ended at far / out of steps): the sky at elevation <= 0 IS the haze colour, so no seam with far terrain
        r.sky = true;
        if (e.bits & kEnvSky) env_sky(e, d, true, r.display); else sky_color(d, r.display);
        if (e.bits) clouds(1e30f);
        finish(r);
        return r;
    }
    float lo = prev, hi = t;
    for (unsigned k = 0; k < kBisect; ++k) {
        const float m = 0.5f * (lo + hi);
        if (o[1] + d[1] * m < height_effective(s, o[0] + d[0] * m, o[2] + d[2] * m)) hi = m; else lo = m;
    }
    t = hi;
    r.hit = true; r.t = t;
    for (int c = 0; c < 3; ++c) r.p[c] = o[c] + d[c] * t;
    const float x = r.p[0], z = r.p[2];
    const float hraw = height_bilinear(s, x, z);
    float col[3];
    float tex[4] = {0, 0, 0, 1};
    auto env_water = [&](const float w[3], float wc[3]) {   // WORLD-2 water: base colour w, Fresnel reflection of the sky, a light glint; static wave normal from the _sea map
        const float* hl = envp(e, kPWaterHi); const float* L = envp(e, kPLight);
        const float rep = envp(e, kPMisc)[2];
        const float amp = kWaveAmp * sat(1.0f - t / kWaveFade);
        const float uw = x / rep * 256.0f, vw = z / rep * 256.0f;
        const float gxw = env_map(e, 1, uw + 1.0f, vw, true) - env_map(e, 1, uw - 1.0f, vw, true);
        const float gzw = env_map(e, 1, uw, vw + 1.0f, true) - env_map(e, 1, uw, vw - 1.0f, true);
        float N[3] = {-gxw * amp, 1.0f, -gzw * amp};
        const float nl = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
        for (float& v : N) v /= nl;
        const float dn = d[0] * N[0] + d[1] * N[1] + d[2] * N[2];
        float rf[3] = {d[0] - 2.0f * dn * N[0], d[1] - 2.0f * dn * N[1], d[2] - 2.0f * dn * N[2]};
        rf[1] = std::fabs(rf[1]);
        const float fr = kWaterF0 + (1.0f - kWaterF0) * std::pow(1.0f - sat(-dn), 5.0f);
        float sk[3]; env_sky(e, rf, false, sk);
        const float sp = std::pow(sat(rf[0] * L[0] + rf[1] * L[1] + rf[2] * L[2]), kWaterSpecPow) * kWaterSpec;
        for (int c = 0; c < 3; ++c) wc[c] = sat(lerpf(w[c], sk[c], fr) + hl[c] * sp);
    };
    if (sea_water_at(s, x, z, hraw)) {   // D-344 C1: with s.sea_masked only on bit-13 (sea) tiles; the march still uses the plane everywhere
        r.water = true;
        if (e.bits & kEnvWater) {   // the sea is painted in the tiles (m16n): base = the texel tinted toward the stage water colour
            float u = (x + s.half) / s.cell * 0.5f, v = (z + s.half) / s.cell * 0.5f;
            u = u > 0 ? (u < 255.999f ? u : 255.999f) : 0.0f; v = v > 0 ? (v < 255.999f ? v : 255.999f) : 0.0f;   // NaN -> 0 (REVIEW L-4)
            const int tx = int(std::floor(u)), tz = int(std::floor(v));
            const uint32_t id = s.words[size_t(tz) * kTGrid + tx] & 0x3ffu;
            const float lod = t * kLodScale > 1.0f ? std::log2(t * kLodScale) : 0.0f;
            float tx4[4]; atlas_sample(a, id, u - float(tx), v - float(tz), lod, tx4);
            const float* wcol = envp(e, kPWater);
            const float base[3] = {lerpf(tx4[0], wcol[0], kSeaTint), lerpf(tx4[1], wcol[1], kSeaTint), lerpf(tx4[2], wcol[2], kSeaTint)};
            env_water(base, col);
        } else {
        const float rd[3] = {d[0], std::fabs(d[1]), d[2]};
        float sk[3]; sky_color(rd, sk);
        const float fr = 0.05f + 0.75f * std::pow(1.0f - sat(-d[1]), 3.0f);
        for (int c = 0; c < 3; ++c) col[c] = lerpf(kSeaDeep[c], sk[c], fr);
        }
    } else {
        float u = (x + s.half) / s.cell * 0.5f, v = (z + s.half) / s.cell * 0.5f;
        u = u > 0 ? (u < 255.999f ? u : 255.999f) : 0.0f; v = v > 0 ? (v < 255.999f ? v : 255.999f) : 0.0f;   // NaN -> 0 (REVIEW L-4)
        const int tx = int(std::floor(u)), tz = int(std::floor(v));
        const uint32_t id = s.words[size_t(tz) * kTGrid + tx] & 0x3ffu;
        const float lod = t * kLodScale > 1.0f ? std::log2(t * kLodScale) : 0.0f;
        atlas_sample(a, id, u - float(tx), v - float(tz), lod, tex);
        for (int c = 0; c < 3; ++c) col[c] = tex[c];
        const float gx = (height_bilinear(s, x + s.cell, z) - height_bilinear(s, x - s.cell, z)) / (2.0f * s.cell);
        const float gz = (height_bilinear(s, x, z + s.cell) - height_bilinear(s, x, z - s.cell)) / (2.0f * s.cell);
        const float nl = std::sqrt(gx * gx + 1.0f + gz * gz);
        if (e.bits & kEnvLight) {   // WORLD-2: the stage's directional light + ambient, normalised so flat ground keeps its baked texel colour; cloud shadows on the direct term
            const float* L = envp(e, kPLight); const float* lc = envp(e, kPLightCol); const float* am = envp(e, kPAmbient);
            float sh = 1.0f;
            if (cloud_on(e) && L[1] > kLightDenMinY) {
                const float sd = (envp(e, kPCloudAlt)[0] - r.p[1]) / L[1];
                if (sd > 0.0f) sh = 1.0f - cloud_alpha(e, x + L[0] * sd, z + L[2] * sd) * envp(e, kPMisc)[3];
            }
            const float ndl = std::max((-gx * L[0] + L[1] - gz * L[2]) / nl, 0.0f) * sh;
            for (int c = 0; c < 3; ++c) {
                const float den = std::max(am[c] + lc[c] * std::max(L[1], kLightDenMinY), 1e-3f);
                const float shade = std::min(std::max((am[c] + lc[c] * ndl) / den, kLightMin), kLightMax);
                col[c] = sat(col[c] * shade);
            }
        } else {
        const float sl = std::sqrt(kSun[0] * kSun[0] + kSun[1] * kSun[1] + kSun[2] * kSun[2]);
        const float lam = std::max((-gx * kSun[0] + kSun[1] - gz * kSun[2]) / (nl * sl), 0.0f);
        const float sh = kAmbient + kDiffuse * lam;
        for (int c = 0; c < 3; ++c) col[c] = sat(col[c] * sh);
        }
        if (e.bits) look_gain(e, col);   // WORLD-4 look:1: the game's lit ground is ~0.6 x the raw texel (land only; water keeps the stage water colour)
        if (a.has_detail()) {   // DTEX: the zero-mean additive grain on the lit land (before the river blend and the haze); no detail = unchanged
            float dt[3]; detail_term(a, x, z, t, dt);
            for (int c = 0; c < 3; ++c) col[c] = sat(col[c] + dt[c]);
        }
        if (tex[3] < 1.0f) {   // D-299 river water: coverage alpha < 1 blends toward the water colour (grey-teal deep colour mixed with the sky by a Fresnel-like term)
            if (e.bits & kEnvWater) {
                float wc[3]; env_water(envp(e, kPWater), wc);
                for (int c = 0; c < 3; ++c) col[c] = lerpf(wc[c], col[c], tex[3]);
            } else {
            const float rd[3] = {d[0], std::fabs(d[1]), d[2]};
            float sk[3]; sky_color(rd, sk);
            const float fr = 0.05f + 0.75f * std::pow(1.0f - sat(-d[1]), 3.0f);
            for (int c = 0; c < 3; ++c) col[c] = lerpf(lerpf(kRiverDeep[c], sk[c], fr), col[c], tex[3]);
            }
            if (tex[3] < 0.5f) r.river = true;
        }
    }
    const float fraw = haze_fraction(t, haze_l, far_dist);
    const float f = look_haze(e, fraw);   // SNOW-LOOK: + the stage's haze floor (look:1 profile only)
    float hz[3] = {kHaze[0], kHaze[1], kHaze[2]};
    if (e.bits & kEnvSky) env_haze(e, d, hz);   // WORLD-2: the haze takes the stage's horizon colour along the view azimuth (no seam against the env sky)
    look_haze_tint(e, fraw, hz);   // K00A-FIT3
    for (int c = 0; c < 3; ++c) r.display[c] = lerpf(col[c], hz[c], f);
    if (e.bits) clouds(t);
    finish(r);
    return r;
}

void add_synthetic_env(StageData& s, float cloud_alt) {
    s.env_params.assign(size_t(kEnvParams) * 4, 0.0f);
    auto P = [&](uint32_t i, float a, float b, float c, float w) { float* p = &s.env_params[size_t(i) * 4]; p[0] = a; p[1] = b; p[2] = c; p[3] = w; };
    auto dir = [&](uint32_t i, float el_deg, float az_deg, float w) {
        const double e = el_deg * 3.14159265358979323846 / 180.0, a = az_deg * 3.14159265358979323846 / 180.0;
        P(i, float(-std::sin(a) * std::cos(e)), float(std::sin(e)), float(-std::cos(a) * std::cos(e)), w);
    };
    auto C = [&](uint32_t i, int r, int g, int b) { P(i, r / 255.0f, g / 255.0f, b / 255.0f, 0.0f); };
    dir(kPMoon, 10.0f, -53.0f, 0.85f); dir(kPLight, 50.0f, -130.0f, 1.0f);   // the m02a values
    C(kPLightCol, 83, 83, 68); C(kPAmbient, 110, 121, 105);
    const int sky[3][4][3] = {{{135, 156, 148}, {140, 164, 155}, {92, 132, 141}, {65, 100, 112}},
                              {{111, 125, 120}, {113, 130, 124}, {83, 119, 127}, {49, 92, 106}},
                              {{85, 102, 98}, {84, 107, 104}, {35, 78, 89}, {15, 53, 62}}};
    for (int r = 0; r < 3; ++r) for (int k = 0; k < 4; ++k) C(kPSky + 4u * r + uint32_t(k), sky[r][k][0], sky[r][k][1], sky[r][k][2]);
    C(kPWater, 70, 89, 84); C(kPWaterHi, 224, 225, 217);
    P(kPCloudAlt, cloud_alt, cloud_alt - 1000.0f, cloud_alt + 500.0f, cloud_alt + 2000.0f); P(kPCloudOn, 1, 1, 1, 0);
    C(kPCloudCol, 145, 142, 121); C(kPCloudCol + 1, 155, 153, 123); C(kPCloudCol + 2, 64, 83, 75); C(kPCloudCol + 3, 66, 81, 75);
    const float dg = 3.14159265f / 180.0f;
    P(kPSkyStops, 0.0f, 1.5f * dg, 6.5f * dg, 23.0f * dg);
    P(kPMisc, 0.01396f, 65536.0f, 4096.0f, 0.3f);
    s.env_cloud.resize(size_t(kEnvMap) * kEnvMap); s.env_wave.resize(size_t(kEnvWave) * kEnvWave);
    s.env_sprite_i.resize(size_t(kEnvSprite) * kEnvSprite); s.env_sprite_a.resize(size_t(kEnvSprite) * kEnvSprite);
    for (uint32_t y = 0; y < kEnvMap; ++y) for (uint32_t x = 0; x < kEnvMap; ++x) {   // blobs on a 64-texel lattice (binary like the game's map)
        const int dx = int(x % 64) - 32, dy = int(y % 64) - 32;
        s.env_cloud[size_t(y) * kEnvMap + x] = (dx * dx + dy * dy < 15 * 15 + int((x / 64 + y / 64) % 3) * 60) ? 255 : 0;
    }
    for (uint32_t y = 0; y < kEnvWave; ++y) for (uint32_t x = 0; x < kEnvWave; ++x)
        s.env_wave[size_t(y) * kEnvWave + x] = uint8_t(128 + 100 * std::sin(x * 6.2831853 / 32.0) * std::cos(y * 6.2831853 / 16.0));
    for (uint32_t y = 0; y < kEnvSprite; ++y) for (uint32_t x = 0; x < kEnvSprite; ++x) {
        const float dx = float(x) - 15.5f, dy = float(y) - 15.5f, rr = std::sqrt(dx * dx + dy * dy);
        s.env_sprite_i[size_t(y) * kEnvSprite + x] = 230;
        s.env_sprite_a[size_t(y) * kEnvSprite + x] = rr < 12.0f ? 140 : 0;
    }
    s.flags |= kFlagEnv;
}

StageData make_synthetic_stage(uint32_t seed, bool sea) {
    StageData s;
    std::memcpy(s.id, "synth", 5);
    s.ntiles = 4; s.nobj = 0;
    s.flags = sea ? (kFlagSeaPresent | kFlagSeaGuess) : 0u;
    s.sea_level = sea ? 8.0f * s.hstep : 0.0f;
    s.heights.resize(size_t(kHDim) * kHDim);
    for (uint32_t j = 0; j < kHDim; ++j)
        for (uint32_t i = 0; i < kHDim; ++i) {
            const double x = i / 512.0 * 6.2831853, z = j / 512.0 * 6.2831853;
            const double h = 12.0 + 9.0 * std::sin(x * 3.0 + seed) * std::cos(z * 2.0) + 4.0 * std::sin((x + z) * 7.0);
            s.heights[size_t(j) * kHDim + i] = uint8_t(std::min(std::max(h, 0.0), 40.0));
        }
    s.words.resize(size_t(kTGrid) * kTGrid);
    for (uint32_t j = 0; j < kTGrid; ++j) for (uint32_t i = 0; i < kTGrid; ++i) s.words[j * kTGrid + i] = uint16_t((i / 4 + j / 4 * 3 + seed) & 3u) | (((i ^ j) & 1u) ? 0x400u : 0u);
    s.tiles.resize(size_t(4) * 4096);
    for (uint32_t t = 0; t < 4; ++t)
        for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x)
            s.tiles[t * 4096 + y * 64 + x] = uint8_t(((x / 8 + y / 8) & 1) ? 16 * (t + 1) : 16 * (t + 1) + 8 + (x ^ y) % 5);
    for (uint32_t c = 0; c < 256; ++c) { s.clut[c * 4] = uint8_t(c); s.clut[c * 4 + 1] = uint8_t(255 - c / 2); s.clut[c * 4 + 2] = uint8_t(60 + c / 4); s.clut[c * 4 + 3] = 255; }
    s.hmax_byte = *std::max_element(s.heights.begin(), s.heights.end());
    std::memset(s.source_sha, 0x5a, 32);
    return s;
}

} // namespace acz::terrain
