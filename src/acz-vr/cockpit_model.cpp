#include "cockpit_model.h"
#include "terrain_stage.h"   // acz::terrain::sha256 (one SHA-256 implementation for both baked formats)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acz::cockpit {
namespace {
inline uint32_t rd32(const uint8_t* p) noexcept { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline int32_t rdi(const uint8_t* p) noexcept { int32_t v; std::memcpy(&v, p, 4); return v; }
inline float rdf(const uint8_t* p) noexcept { float v; std::memcpy(&v, p, 4); return v; }
inline void wr32(std::vector<uint8_t>& b, uint32_t v) { const size_t o = b.size(); b.resize(o + 4); std::memcpy(&b[o], &v, 4); }
inline void wrf(std::vector<uint8_t>& b, float v) { const size_t o = b.size(); b.resize(o + 4); std::memcpy(&b[o], &v, 4); }
inline float sat(float x) noexcept { return x < 0 ? 0 : x > 1 ? 1 : x; }
bool finite_all(const float* f, size_t n) noexcept { for (size_t i = 0; i < n; ++i) if (!std::isfinite(f[i])) return false; return true; }
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
std::string ModelData::part_name(size_t i) const {
    if (i >= parts.size() || parts[i].name_off >= strings.size()) return std::string();
    return std::string(strings.c_str() + parts[i].name_off);
}
bool valid_model_id(const std::string& id) noexcept {
    if (id.empty() || id.size() > 24) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
std::string model_path(const std::string& id, const std::string& dir_override) {
    if (!valid_model_id(id)) return std::string();
    std::string dir = dir_override;
    if (dir.empty()) {
        const char* la = std::getenv("LOCALAPPDATA");
        if (!la || !*la) return std::string();
        dir = std::string(la) + "\\acz-vr\\models";
    }
    return dir + "\\" + id + ".zvrm";
}

// ------------------------------------------------------------------------------------------------------------------- loader
LoadStatus parse_model(const uint8_t* d, size_t n, ModelData& out) noexcept {
    try {
        if (!d || n < kZvrmHeader) return LoadStatus::too_small;
        if (n > kMaxFileBytes) return LoadStatus::too_large;
        if (std::memcmp(d, "ZVRM", 4) != 0) return LoadStatus::bad_magic;
        if (rd32(d + 4) != kZvrmVersion) return LoadStatus::bad_version;
        const uint32_t hb = rd32(d + 8), total = rd32(d + 12), np = rd32(d + 16), nt = rd32(d + 20), nv = rd32(d + 24), ni = rd32(d + 28);
        const uint32_t p_off = rd32(d + 32), t_off = rd32(d + 36), v_off = rd32(d + 40), i_off = rd32(d + 44), s_off = rd32(d + 48), s_len = rd32(d + 52);
        const uint32_t stride = rd32(d + 56), flags = rd32(d + 60);
        if (hb != kZvrmHeader || stride != kVertexStride || (flags & ~kZvrmKnownFlags) != 0) return LoadStatus::bad_header;
        if (total != n) return LoadStatus::bad_size;
        if (np > kMaxParts || nt > kMaxTextures || nv > kMaxVertices || ni > kMaxIndices || (ni % 3) != 0) return LoadStatus::bad_header;
        float hf[9]; for (int i = 0; i < 9; ++i) hf[i] = rdf(d + 64 + 4 * i);
        const uint32_t has_eye = rd32(d + 100), kind = rd32(d + 104);
        if (!finite_all(hf, 9) || has_eye > 1 || kind < 1 || kind > 3) return LoadStatus::bad_header;
        // Section layout exactly as model_baker.py writes it: parts, texture headers, texture data, pad to 16, vertices, indices, strings (to the end).
        const uint64_t data0 = uint64_t(t_off) + uint64_t(nt) * kTexHeader;
        if (p_off != kZvrmHeader || uint64_t(t_off) != uint64_t(p_off) + uint64_t(np) * kPartBytes || (v_off & 3u) || v_off < data0 ||
            uint64_t(i_off) != uint64_t(v_off) + uint64_t(nv) * kVertexStride || uint64_t(s_off) != uint64_t(i_off) + uint64_t(ni) * 4u ||
            uint64_t(s_off) + s_len != n || s_len == 0) return LoadStatus::bad_size;
        uint8_t hash[32];
        acz::terrain::sha256(d + kZvrmHeader, n - kZvrmHeader, hash);
        if (std::memcmp(hash, d + 108, 32) != 0) return LoadStatus::bad_hash;
        ModelData m;
        m.flags = flags; m.source_kind = kind; m.has_eye = has_eye != 0;
        for (int i = 0; i < 3; ++i) { m.bbox_min[i] = hf[i]; m.bbox_max[i] = hf[3 + i]; m.eye[i] = hf[6 + i]; }
        std::memcpy(m.payload_sha, d + 108, 32);
        m.strings.assign(reinterpret_cast<const char*>(d + s_off), s_len);
        if (m.strings.back() != '\0') return LoadStatus::bad_content;
        m.textures.resize(nt);
        uint64_t prev_end = data0;
        for (uint32_t k = 0; k < nt; ++k) {
            const uint8_t* th = d + t_off + k * kTexHeader;
            const uint32_t w = rd32(th), h = rd32(th + 4), fmt = rd32(th + 8), off = rd32(th + 12);
            if (w == 0 || h == 0 || w > kMaxTexDim || h > kMaxTexDim || fmt != 1) return LoadStatus::bad_content;
            const uint64_t bytes = uint64_t(w) * h * 4u;
            if (off < prev_end || uint64_t(off) + bytes > v_off) return LoadStatus::bad_content;
            m.textures[k].w = w; m.textures[k].h = h; m.textures[k].rgba.assign(d + off, d + off + bytes);
            prev_end = uint64_t(off) + bytes;
        }
        m.vertices.resize(nv);
        if (nv) std::memcpy(m.vertices.data(), d + v_off, size_t(nv) * kVertexStride);
        for (const Vertex& v : m.vertices) {
            if (!finite_all(v.pos, 3) || !finite_all(v.uv, 2) || v.part >= np) return LoadStatus::bad_content;
            for (float p : v.pos) if (std::fabs(p) > 1e6f) return LoadStatus::bad_content;
            for (float t : v.uv) if (std::fabs(t) > 1e4f) return LoadStatus::bad_content;
        }
        m.indices.resize(ni);
        if (ni) std::memcpy(m.indices.data(), d + i_off, size_t(ni) * 4u);
        for (uint32_t ix : m.indices) if (ix >= nv) return LoadStatus::bad_content;
        m.parts.resize(np);
        uint64_t part_indices = 0;   // REVIEW (overnight-loaders-review.md L-1): model_baker.py gives every part its own index range, so the sum is <= ni;
                                     // overlapping ranges would make the checks below O(np * ni) and build_mesh expand up to np * ni corners (TB-sized heap)
        for (uint32_t k = 0; k < np; ++k) {
            const uint8_t* pr = d + p_off + k * kPartBytes;
            Part& p = m.parts[k];
            p.name_off = rd32(pr); p.parent = rdi(pr + 4); p.node = rd32(pr + 8); p.game_flags = rd32(pr + 12); p.group = rd32(pr + 16); p.anim = rdi(pr + 20);
            p.mflags = rd32(pr + 24); p.texture = rdi(pr + 28); p.first_index = rd32(pr + 32); p.index_count = rd32(pr + 36);
            p.first_vertex = rd32(pr + 40); p.vertex_count = rd32(pr + 44);
            for (int i = 0; i < 16; ++i) p.local_to_model[i] = rdf(pr + 48 + 4 * i);
            p.hinge = rdf(pr + 112); p.source_part = rd32(pr + 116);
            p.rest = rdf(pr + 120); p.aflags = rd32(pr + 124);   // COCKPIT-4 (zero in older files)
            if (!std::isfinite(p.rest) || std::fabs(p.rest) > 7.0f || (p.aflags & ~kPartKnownAnimFlags) != 0 ||
                (!(flags & kZvrmAnimRest) && (p.rest != 0.0f || p.aflags != 0))) return LoadStatus::bad_content;
            if (p.name_off >= s_len || p.parent < -1 || p.parent >= int32_t(k) || p.texture < -1 || p.texture >= int32_t(nt)) return LoadStatus::bad_content;
            if ((p.first_index % 3) || (p.index_count % 3) || uint64_t(p.first_index) + p.index_count > ni ||
                uint64_t(p.first_vertex) + p.vertex_count > nv) return LoadStatus::bad_content;
            part_indices += p.index_count;
            if (part_indices > ni) return LoadStatus::bad_content;
            if (!finite_all(p.local_to_model, 16) || !std::isfinite(p.hinge)) return LoadStatus::bad_content;
            for (uint32_t i = 0; i < p.index_count; ++i) {
                const uint32_t ix = m.indices[p.first_index + i];
                if (ix < p.first_vertex || ix >= p.first_vertex + p.vertex_count) return LoadStatus::bad_content;
            }
        }
        // COCKPIT-3: a pre-flag mpl/acm bake decoded the GIM CLUT as BGRA (D-303); the PS2 CT32 CLUT is RGBA (the game's replayed cockpit, the trees and the
        // WORLD-2 env GIMs agree). Swap R and B back so every loaded model is in the RGBA domain; prop bakes (kind 2, pmtex CLUT) were always RGBA.
        if (!(m.flags & kZvrmRgbaClut)) {
            if (kind == 1 || kind == 3) {
                for (Texture& t : m.textures) for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) std::swap(t.rgba[i], t.rgba[i + 2]);
                m.legacy_clut_fixed = true;
            }
            m.flags |= kZvrmRgbaClut;
        }
        out = std::move(m);
        return LoadStatus::ok;
    } catch (...) { return LoadStatus::io_error; }
}
LoadStatus load_model_file(const std::string& path, ModelData& out) noexcept {
    try {
        if (path.empty()) return LoadStatus::io_error;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return LoadStatus::io_error;
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz < 0) { std::fclose(f); return LoadStatus::io_error; }
        if (size_t(sz) > kMaxFileBytes) { std::fclose(f); return LoadStatus::too_large; }
        std::vector<uint8_t> buf(static_cast<size_t>(sz), 0);
        const size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (got != buf.size()) return LoadStatus::io_error;
        return parse_model(buf.data(), buf.size(), out);
    } catch (...) { return LoadStatus::io_error; }
}
std::vector<uint8_t> serialize_model(const ModelData& m) {
    const uint32_t np = uint32_t(m.parts.size()), nt = uint32_t(m.textures.size()), nv = uint32_t(m.vertices.size()), ni = uint32_t(m.indices.size());
    std::vector<uint8_t> body;
    for (const Part& p : m.parts) {
        wr32(body, p.name_off); wr32(body, uint32_t(p.parent)); wr32(body, p.node); wr32(body, p.game_flags); wr32(body, p.group); wr32(body, uint32_t(p.anim));
        wr32(body, p.mflags); wr32(body, uint32_t(p.texture)); wr32(body, p.first_index); wr32(body, p.index_count); wr32(body, p.first_vertex); wr32(body, p.vertex_count);
        for (float f : p.local_to_model) wrf(body, f);
        wrf(body, p.hinge); wr32(body, p.source_part); wrf(body, p.rest); wr32(body, p.aflags);
    }
    const uint32_t t_off = uint32_t(kZvrmHeader + body.size());
    const uint32_t data0 = t_off + nt * uint32_t(kTexHeader);
    uint32_t run = data0;
    for (const Texture& t : m.textures) { wr32(body, t.w); wr32(body, t.h); wr32(body, 1); wr32(body, run); run += uint32_t(t.rgba.size()); }
    for (const Texture& t : m.textures) body.insert(body.end(), t.rgba.begin(), t.rgba.end());
    const uint32_t v_off = (run + 15u) & ~15u;
    body.resize(body.size() + (v_off - run), 0);
    const uint8_t* vb = reinterpret_cast<const uint8_t*>(m.vertices.data());
    body.insert(body.end(), vb, vb + size_t(nv) * kVertexStride);
    const uint32_t i_off = v_off + nv * uint32_t(kVertexStride);
    const uint8_t* ib = reinterpret_cast<const uint8_t*>(m.indices.data());
    body.insert(body.end(), ib, ib + size_t(ni) * 4u);
    const uint32_t s_off = i_off + ni * 4u;
    body.insert(body.end(), m.strings.begin(), m.strings.end());
    const uint32_t total = uint32_t(kZvrmHeader + body.size());
    std::vector<uint8_t> hdr;
    hdr.insert(hdr.end(), {'Z', 'V', 'R', 'M'});
    for (uint32_t v : {kZvrmVersion, uint32_t(kZvrmHeader), total, np, nt, nv, ni, uint32_t(kZvrmHeader), t_off, v_off, i_off, s_off, uint32_t(m.strings.size()),
                       uint32_t(kVertexStride), m.flags}) wr32(hdr, v);
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    for (size_t i = 0; i < m.vertices.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            const float p = m.vertices[i].pos[c];
            if (i == 0 || p < lo[c]) lo[c] = p;
            if (i == 0 || p > hi[c]) hi[c] = p;
        }
    for (float f : lo) wrf(hdr, f);
    for (float f : hi) wrf(hdr, f);
    for (int c = 0; c < 3; ++c) wrf(hdr, m.has_eye ? m.eye[c] : 0.0f);
    wr32(hdr, m.has_eye ? 1u : 0u); wr32(hdr, m.source_kind);
    hdr.resize(hdr.size() + 32);
    acz::terrain::sha256(body.data(), body.size(), &hdr[108]);
    hdr.resize(kZvrmHeader, 0);
    hdr.insert(hdr.end(), body.begin(), body.end());
    return hdr;
}

// ------------------------------------------------------------------------------------------------------------------- options
namespace {
bool num(const std::string& v, float& out) {
    if (v.empty() || v.size() > 16) return false;
    char* e = nullptr; const double x = std::strtod(v.c_str(), &e);
    if (e != v.c_str() + v.size() || !std::isfinite(x) || std::fabs(x) > 1e7) return false;
    out = float(x); return true;
}
bool list_u(const std::string& v, std::vector<uint32_t>& out) {
    size_t p = 0;
    for (;;) {
        size_t c = v.find(',', p); if (c == std::string::npos) c = v.size();
        const std::string t = v.substr(p, c - p);
        if (t.empty() || t.size() > 6) return false;
        for (char ch : t) if (ch < '0' || ch > '9') return false;
        if (out.size() >= 64) return false;
        out.push_back(uint32_t(std::strtoul(t.c_str(), nullptr, 10)));
        if (c == v.size()) return true;
        p = c + 1;
    }
}
}
bool parse_options(const std::string& value, std::string& id, Options& o, std::string& why) {
    o = Options{};
    size_t pos = 0; bool first = true;
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        if (first) {
            first = false;
            if (!valid_model_id(item)) { why = "bad model id"; return false; }
            id = item;
            if (semi == value.size()) break;
            continue;
        }
        if (item.empty()) { why = "empty option"; return false; }
        const size_t eq = item.find_first_of(":=");
        if (eq == std::string::npos) { why = "option without : or ="; return false; }
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        float f = 0; bool ok = true;
        auto flag = [&](bool& b) { if (v == "1") b = true; else if (v == "0") b = false; else return false; return true; };
        if (k == "upm") { ok = num(v, f) && f >= 0.5f && f <= 100.0f; o.upm = f; }
        else if (k == "gain") { ok = num(v, f) && f >= 0.0f && f <= 8.0f; o.gain = f; }
        else if (k == "amb") { ok = num(v, f) && f >= 0.0f && f <= 1.0f; o.ambient = f; }
        else if (k == "cut") { ok = num(v, f) && f >= 0.0f && f <= 1.0f; o.cut = f; }
        else if (k == "near") { ok = num(v, f) && f >= 0.001f && f <= 10.0f; o.near_u = f; }
        else if (k == "ext") ok = flag(o.exterior);
        else if (k == "glass") ok = flag(o.glass);
        else if (k == "hudglass") ok = flag(o.hud_glass);
        else if (k == "floor") ok = flag(o.floor);
        else if (k == "dom") { if (v == "display") o.display = true; else if (v == "raw") o.display = false; else ok = false; }
        else if (k == "light") { if (v == "stage") o.stage_light = true; else if (v == "fixed") o.stage_light = false; else ok = false; }
        else if (k == "replay") ok = flag(o.keep_replay);
        else if (k == "canopy") ok = flag(o.canopy);                                   // COCKPIT-4
        else if (k == "interior") ok = flag(o.interior);
        else if (k == "anim") ok = flag(o.anim);
        else if (k == "stores") ok = flag(o.stores);                                   // STORES-RT
        else if (k == "spstores") ok = flag(o.spstores);                               // SP-STORES
        else if (k == "stwatch") ok = flag(o.stwatch);
        else if (k == "mslhud") { ok = v.size() == 1 && v[0] >= '0' && v[0] <= '2'; if (ok) o.mslhud = uint8_t(v[0] - '0'); }   // MSL-HUD (hud_msl.h; the step-5 producer parses the raw string)
        else if (k == "strel") { ok = num(v, f) && f >= 0.0f && f <= 60.0f; o.strel = f; }   // STORES reload seconds (store_tracker.h)
        else if (k == "stwatch-start") { ok = num(v, f) && f >= 1.0f && f <= 9999.0f && f == std::floor(f); o.stwatch_start = uint32_t(f); }   // STORES-RT wide search: HUD count at the first lock
        else if (k == "sttrack") {   // up to 8 hex block offsets, comma separated (ee_counter_search.h)
            size_t p2 = 0; unsigned cnt = 0;
            for (;;) {
                size_t c2 = v.find(',', p2); if (c2 == std::string::npos) c2 = v.size();
                const std::string t = v.substr(p2, c2 - p2); char* e = nullptr; const unsigned long x = std::strtoul(t.c_str(), &e, 16);
                if (t.empty() || *e || x >= 0x4000000ul || ++cnt > 8) { ok = false; break; }
                if (c2 == v.size()) break;
                p2 = c2 + 1;
            }
        }
        else if (k == "stctr") {   // STORES-RT wiring: <hex block offset>,<1|2|4> of the found missile counter (the probe parses the raw string too: ee_counter_search.h)
            char* e = nullptr; const unsigned long off = std::strtoul(v.c_str(), &e, 16);
            ok = !v.empty() && e != v.c_str() && *e == ',' && off > 0 && off < 0x4000000ul && (e[1] == '1' || e[1] == '2' || e[1] == '4') && e[2] == 0;
            if (ok) { o.stctr_off = uint32_t(off); o.stctr_width = uint32_t(e[1] - '0'); }
        }
        else if (k == "zpre") ok = flag(o.zpre);                                       // GPU-COST
        else if (k == "cscale") { ok = num(v, f) && f >= kCScaleMin && f <= kCScaleMax; o.cscale = f; }   // COCKPIT-SCALE (num refuses NaN/inf)
        else if (k == "menu") ok = flag(o.menu);                                       // HUD-MENU
        else if (k == "menud") { ok = num(v, f) && f >= 0.5f && f <= 20.0f; o.menu_distance = f; }
        else if (k == "menub") { ok = num(v, f) && f >= 0.0f && f <= 0.95f; o.menu_backdrop = f; }
        else if (k == "radarbg") ok = flag(o.radar_bg);                                // RADAR-BG
        else if (k == "radarbga") { ok = num(v, f) && f >= 0.05f && f <= 0.95f; o.radar_bg_alpha = f; }
        else if (k == "stick") { if (v == "centre" || v == "center") o.stick = 0; else if (v == "side") o.stick = 1; else if (v == "none") o.stick = 2; else ok = false; }
        else if (k == "refl") { ok = num(v, f) && f >= 0.0f && f <= 1.0f; o.refl = f; }
        else if (k == "dirt") { ok = num(v, f) && f >= 0.0f && f <= 0.5f; o.dirt = f; }
        else if (k == "srefl") { ok = flag(o.skin_refl); o.skin_refl_set = ok; }      // SKIN-REFL (explicit: wins over cfid)
        else if (k == "sreflk") { ok = num(v, f) && f >= 0.0f && f <= 4.0f; o.skin_refl_k = f; }
        else if (k == "cfid") ok = flag(o.fidelity);                                   // CFID
        else if (k == "seatback") { ok = num(v, f) && f >= 0.0f && f <= kSeatBackMax; o.seat_back = f; }   // SEAT-CLEARANCE (0 = off)
        else if (k == "tint") {
            float a[3]; size_t p2 = 0; int cnt = 0;
            for (;;) {
                size_t c2 = v.find(',', p2); if (c2 == std::string::npos) c2 = v.size();
                if (cnt >= 3 || !num(v.substr(p2, c2 - p2), a[cnt]) || !(a[cnt] >= 0.0f && a[cnt] <= 1.0f)) { ok = false; break; }
                ++cnt; p2 = c2 + 1;
                if (c2 == v.size()) break;
            }
            ok = ok && cnt == 3;
            if (ok) for (int i = 0; i < 3; ++i) o.tint[i] = a[i];
        }
        else if (k == "clip") ok = flag(o.clip);                                       // COCKPIT-CLIP
        else if (k == "clipbox") {   // <ahead>,<behind>,<below>,<above>,<half> metres from the pvb eye, each 0.05..5
            float a[5]; size_t p2 = 0; int cnt = 0;
            for (;;) {
                size_t c2 = v.find(',', p2); if (c2 == std::string::npos) c2 = v.size();
                if (cnt >= 5 || !num(v.substr(p2, c2 - p2), a[cnt]) || !(a[cnt] >= 0.05f && a[cnt] <= 5.0f)) { ok = false; break; }
                ++cnt; p2 = c2 + 1;
                if (c2 == v.size()) break;
            }
            ok = ok && cnt == 5;
            if (ok) { o.clip_box_set = true; for (int i = 0; i < 5; ++i) o.clip_box[i] = a[i]; }
        }
        else if (k == "hide") ok = list_u(v, o.hide);
        else if (k == "show") ok = list_u(v, o.show);
        else if (k == "eye") {
            float a[3]; size_t p2 = 0; int cnt = 0;
            for (;;) {
                size_t c2 = v.find(',', p2); if (c2 == std::string::npos) c2 = v.size();
                if (cnt >= 3 || !num(v.substr(p2, c2 - p2), a[cnt])) { ok = false; break; }
                ++cnt; p2 = c2 + 1;
                if (c2 == v.size()) break;
            }
            ok = ok && cnt == 3;
            if (ok) { o.eye_override = true; for (int i = 0; i < 3; ++i) o.eye[i] = a[i]; }
        }
        else { why = "unknown option " + k; return false; }
        if (!ok) { why = "bad value for " + k; return false; }
        if (semi == value.size()) break;
    }
    if (first) { why = "empty"; return false; }
    if (!o.skin_refl_set) o.skin_refl = o.fidelity;   // CFID: the reflection pass follows cfid unless srefl: was given
    return true;
}

// ------------------------------------------------------------------------------------------------------------------- COCKPIT-CLIP table
// Generated by research/world-render-data/cockpit_clip_survey.py table from the 36 vrcockpit bakes (cockpit_clip_volumes.json; rule there and in
// cockpit-render-impl.md section 16): metres from the pvb eye {ahead, behind, below, above, half-width}. Only f15c/e/s, f16x and zoef lose game cockpit
// triangles to it (parts behind the cockpit that stick out of the airframe); for every other type it only bounds the procedural interior.
const ClipVolume kClipVolumes[kClipVolumeCount] = {
    {"a10ak_vrcockpit", 1.45f, 1.25f, 1.12f, 0.32f, 0.51f},
    {"e06bk_vrcockpit", 1.33f, 1.35f, 1.12f, 0.48f, 1.24f},
    {"e18gk_vrcockpit", 1.45f, 1.94f, 1.12f, 0.51f, 0.47f},
    {"f01ak_vrcockpit", 3.00f, 1.06f, 1.12f, 0.46f, 0.41f},
    {"f02ak_vrcockpit", 0.60f, 1.74f, 1.12f, 0.35f, 0.44f},
    {"f04ek_vrcockpit", 1.08f, 1.59f, 1.12f, 0.52f, 0.47f},
    {"f05ek_vrcockpit", 1.20f, 0.88f, 1.12f, 0.27f, 0.43f},
    {"f14dk_vrcockpit", 1.00f, 3.05f, 1.12f, 0.46f, 0.48f},
    {"f15ck_vrcockpit", 0.89f, 2.54f, 1.12f, 0.37f, 0.54f},
    {"f15ek_vrcockpit", 0.59f, 2.55f, 1.12f, 0.54f, 0.54f},
    {"f15sk_vrcockpit", 0.59f, 2.55f, 1.12f, 0.54f, 0.54f},
    {"f16ck_vrcockpit", 0.75f, 2.35f, 1.12f, 0.34f, 0.45f},
    {"f16xk_vrcockpit", 0.75f, 2.58f, 1.12f, 0.42f, 0.44f},
    {"f17ak_vrcockpit", 1.34f, 1.13f, 1.12f, 0.52f, 1.03f},
    {"f18ck_vrcockpit", 0.85f, 1.83f, 1.12f, 0.35f, 0.42f},
    {"f20ak_vrcockpit", 1.20f, 1.25f, 1.12f, 0.27f, 0.43f},
    {"f35ck_vrcockpit", 1.24f, 1.62f, 1.12f, 0.25f, 0.58f},
    {"fa22k_vrcockpit", 1.40f, 1.50f, 1.12f, 0.22f, 0.52f},
    {"j35jk_vrcockpit", 1.12f, 0.84f, 1.12f, 0.18f, 0.42f},
    {"j39ck_vrcockpit", 0.79f, 1.55f, 1.12f, 0.34f, 0.46f},
    {"m21bk_vrcockpit", 1.85f, 0.82f, 1.12f, 0.37f, 0.43f},
    {"m29ak_vrcockpit", 1.31f, 1.29f, 1.12f, 0.28f, 0.42f},
    {"m31bk_vrcockpit", 2.04f, 0.70f, 1.12f, 0.29f, 0.50f},
    {"mr2dk_vrcockpit", 0.77f, 2.27f, 1.12f, 0.53f, 0.41f},
    {"mrgnk_vrcockpit", 0.67f, 1.29f, 1.12f, 0.27f, 0.41f},
    {"rm01k_vrcockpit", 0.68f, 2.26f, 1.12f, 0.30f, 0.47f},
    {"su27k_vrcockpit", 1.87f, 1.50f, 1.12f, 0.30f, 0.57f},
    {"su32k_vrcockpit", 2.24f, 1.20f, 1.12f, 0.59f, 1.34f},
    {"su37k_vrcockpit", 1.95f, 1.51f, 1.12f, 0.29f, 0.61f},
    {"su47k_vrcockpit", 2.34f, 1.51f, 1.33f, 0.29f, 0.59f},
    {"tnd4k_vrcockpit", 1.44f, 1.65f, 1.12f, 0.51f, 0.50f},
    {"typnk_vrcockpit", 0.66f, 2.02f, 1.12f, 0.23f, 0.45f},
    {"x02ak_vrcockpit", 0.90f, 1.72f, 1.12f, 0.24f, 0.47f},
    {"x29ak_vrcockpit", 1.21f, 1.25f, 1.12f, 0.27f, 0.43f},
    {"yf23k_vrcockpit", 1.64f, 1.44f, 1.12f, 0.31f, 0.57f},
    {"zoefk_vrcockpit", 1.40f, 2.70f, 1.71f, 0.52f, 0.70f},
};
const ClipVolume* clip_volume_for(const std::string& model_id) noexcept {
    for (const ClipVolume& c : kClipVolumes) if (model_id == c.model_id) return &c;
    return nullptr;
}
bool apply_clip_table(Options& o, const std::string& model_id) noexcept {
    if (!o.clip) return false;
    if (o.clip_box_set) return true;
    const ClipVolume* c = clip_volume_for(model_id);
    if (!c) return false;
    o.clip_box[0] = c->ahead; o.clip_box[1] = c->behind; o.clip_box[2] = c->below; o.clip_box[3] = c->above; o.clip_box[4] = c->half;
    o.clip_box_set = true;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------- mesh
namespace {
inline float clampf(float x, float lo, float hi) noexcept { return x < lo ? lo : x > hi ? hi : x; }
GpuVertex make_corner(const float p[3], const float n[3], const uint8_t rgba[4]) {
    GpuVertex g{};
    std::memcpy(g.pos, p, sizeof g.pos);
    for (int a = 0; a < 3; ++a) g.nrm[a] = int16_t(std::lround(clampf(n[a], -1.0f, 1.0f) * 32767.0f));
    g.mat = uint16_t(kNoTexture);
    std::memcpy(g.rgba, rgba, 4);
    return g;
}
// One flat quad c0..c3 (counter-clockwise seen from the side the normal n points to) -> two CCW-front triangles.
void emit_quad(std::vector<GpuVertex>& v, const float c[4][3], const float n[3], const uint8_t rgba[4]) {
    for (int ix : {0, 1, 2, 0, 2, 3}) v.push_back(make_corner(c[ix], n, rgba));
}
// Axis-aligned box lo..hi, six outward faces (12 triangles, CCW front from outside).
void emit_box(std::vector<GpuVertex>& v, const float lo[3], const float hi[3], const uint8_t rgba[4]) {
    const float x0 = lo[0], y0 = lo[1], z0 = lo[2], x1 = hi[0], y1 = hi[1], z1 = hi[2];
    const float f[6][4][3] = {
        {{x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}},   // +X
        {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},   // -X
        {{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}},   // +Y
        {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},   // -Y
        {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}},   // +Z
        {{x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}}};  // -Z
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int k = 0; k < 6; ++k) emit_quad(v, f[k], n[k], rgba);
}
// Rodrigues rotation matrix (row-major) about a unit axis.
void axis_rot(const float a[3], float ang, float R[9]) noexcept {
    const float c = std::cos(ang), s = std::sin(ang), t = 1.0f - c, x = a[0], y = a[1], z = a[2];
    R[0] = t * x * x + c;     R[1] = t * x * y - s * z; R[2] = t * x * z + s * y;
    R[3] = t * x * y + s * z; R[4] = t * y * y + c;     R[5] = t * y * z - s * x;
    R[6] = t * x * z - s * y; R[7] = t * y * z + s * x; R[8] = t * z * z + c;
}
void mat3_mul(const float A[9], const float B[9], float C[9]) noexcept {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) C[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
}
// ---- COCKPIT-CLIP helpers (cockpit-render-impl.md section 16)
// A flat quad c0..c3 (CCW seen from the side n points to) tessellated into n_div x n_div cells (bilinear), so the airframe fit can bend it along the skin.
void emit_quad_grid(std::vector<GpuVertex>& v, const float c[4][3], const float n[3], const uint8_t rgba[4], int n_div) {
    auto at = [&](int i, int j, float o[3]) {
        const float s = float(i) / float(n_div), t = float(j) / float(n_div);
        for (int a = 0; a < 3; ++a) o[a] = (1 - s) * (1 - t) * c[0][a] + s * (1 - t) * c[1][a] + s * t * c[2][a] + (1 - s) * t * c[3][a];
    };
    for (int j = 0; j < n_div; ++j)
        for (int i = 0; i < n_div; ++i) {
            float q[4][3]; at(i, j, q[0]); at(i + 1, j, q[1]); at(i + 1, j + 1, q[2]); at(i, j + 1, q[3]);
            emit_quad(v, q, n, rgba);
        }
}
GpuVertex lerp_vertex(const GpuVertex& a, const GpuVertex& b, float t) {
    GpuVertex g = a;
    for (int k = 0; k < 3; ++k) g.pos[k] = a.pos[k] + t * (b.pos[k] - a.pos[k]);
    for (int k = 0; k < 2; ++k) g.uv[k] = a.uv[k] + t * (b.uv[k] - a.uv[k]);
    for (int k = 0; k < 3; ++k) g.nrm[k] = int16_t(std::lround(float(a.nrm[k]) + t * float(b.nrm[k] - a.nrm[k])));
    for (int k = 0; k < 4; ++k) g.rgba[k] = uint8_t(std::lround(float(a.rgba[k]) + t * (float(b.rgba[k]) - float(a.rgba[k]))));
    return g;
}
// Exact clip of the triangles v[first..) against the box lo..hi (Sutherland-Hodgman per triangle, the polygon re-fanned in its own order: winding kept,
// attributes interpolated linearly). removed = triangles entirely outside, cut = triangles that lost a part.
void clip_range(std::vector<GpuVertex>& v, size_t first, const float lo[3], const float hi[3], uint32_t& removed, uint32_t& cut) {
    std::vector<GpuVertex> out; out.reserve(v.size() - first);
    std::vector<GpuVertex> poly, next;
    for (size_t i = first; i + 2 < v.size(); i += 3) {
        bool inside = true;
        for (size_t k = i; k < i + 3 && inside; ++k)
            for (int a = 0; a < 3; ++a) if (!(v[k].pos[a] >= lo[a] && v[k].pos[a] <= hi[a])) { inside = false; break; }
        if (inside) { out.insert(out.end(), v.begin() + std::ptrdiff_t(i), v.begin() + std::ptrdiff_t(i + 3)); continue; }
        poly.assign(v.begin() + std::ptrdiff_t(i), v.begin() + std::ptrdiff_t(i + 3));
        for (int pl = 0; pl < 6 && !poly.empty(); ++pl) {
            const int a = pl >> 1; const bool upper = (pl & 1) != 0; const float b = upper ? hi[a] : lo[a];
            auto dist = [&](const GpuVertex& g) { return upper ? b - g.pos[a] : g.pos[a] - b; };   // >= 0 inside
            next.clear();
            for (size_t k = 0; k < poly.size(); ++k) {
                const GpuVertex& p0 = poly[k]; const GpuVertex& p1 = poly[(k + 1) % poly.size()];
                const float d0 = dist(p0), d1 = dist(p1);
                if (d0 >= 0) next.push_back(p0);
                if ((d0 >= 0) != (d1 >= 0)) next.push_back(lerp_vertex(p0, p1, d0 / (d0 - d1)));
            }
            poly.swap(next);
        }
        if (poly.size() < 3) { ++removed; continue; }
        ++cut;
        for (size_t k = 1; k + 1 < poly.size(); ++k) { out.push_back(poly[0]); out.push_back(poly[k]); out.push_back(poly[k + 1]); }
    }
    v.resize(first);
    v.insert(v.end(), out.begin(), out.end());
}
// The exterior skin (drawn exterior triangles incl. the canopy glass) near the cockpit, 9 floats per triangle.
struct Skin { std::vector<float> t; };
// Every coordinate along axis a at which the line through p parallel to that axis crosses the skin (2D barycentric test in the other two axes).
void axis_hits(const Skin& s, const float p[3], int a, std::vector<float>& out) {
    const int b = (a + 1) % 3, c = (a + 2) % 3;
    out.clear();
    for (size_t i = 0; i + 8 < s.t.size(); i += 9) {
        const float* A = &s.t[i]; const float* B = A + 3; const float* C = A + 6;
        const float d = (B[b] - A[b]) * (C[c] - A[c]) - (B[c] - A[c]) * (C[b] - A[b]);
        if (!(std::fabs(d) > 1e-12f)) continue;
        const float w1 = ((p[b] - A[b]) * (C[c] - A[c]) - (p[c] - A[c]) * (C[b] - A[b])) / d;
        const float w2 = ((B[b] - A[b]) * (p[c] - A[c]) - (B[c] - A[c]) * (p[b] - A[b])) / d;
        const float w0 = 1.0f - w1 - w2;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        out.push_back(w0 * A[a] + w1 * B[a] + w2 * C[a]);
    }
}
// Pull p inside the skin (margin m): sideways to the outermost skin crossing on its side of the centre plane x = cx at its height, then vertically between
// the lowest and highest skin crossing of its column (only when the column has two distinct crossings, i.e. a closed airframe, not a single sheet/wing edge).
bool fit_point(const Skin& s, float p[3], float cx, float m, std::vector<float>& h) {
    bool moved = false;
    for (int it = 0; it < 3; ++it) {
        axis_hits(s, p, 0, h);
        const float side = p[0] >= cx ? 1.0f : -1.0f;
        float hw = -1.0f; for (float x : h) hw = std::max(hw, (x - cx) * side);
        if (hw > m && (p[0] - cx) * side > hw - m) { p[0] = cx + side * (hw - m); moved = true; }
        axis_hits(s, p, 1, h);
        if (h.size() >= 2) {
            const float lo = *std::min_element(h.begin(), h.end()), hi = *std::max_element(h.begin(), h.end());
            if (hi - lo > 2 * m) {
                const float y = clampf(p[1], lo + m, hi - m);
                if (y != p[1]) { p[1] = y; moved = true; }
            }
        }
    }
    return moved;
}
// Flat normals from the geometry for v[first..), dropping triangles that became degenerate (the fit can collapse a cell onto the skin line).
void renormal_range(std::vector<GpuVertex>& v, size_t first) {
    size_t w = first;
    for (size_t i = first; i + 2 < v.size(); i += 3) {
        const float* a = v[i].pos; const float* b = v[i + 1].pos; const float* c = v[i + 2].pos;
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float g[3] = {u[1] * e[2] - u[2] * e[1], u[2] * e[0] - u[0] * e[2], u[0] * e[1] - u[1] * e[0]};
        const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (!(len > 1e-6f)) continue;
        for (size_t k = 0; k < 3; ++k) {
            GpuVertex t = v[i + k];
            for (int q = 0; q < 3; ++q) t.nrm[q] = int16_t(std::lround(clampf(g[q] / len, -1.0f, 1.0f) * 32767.0f));
            v[w + k] = t;
        }
        w += 3;
    }
    v.resize(w);
}
// Interior colours (vertex RGBA in the DISPLAY domain, untextured; dark grey-green like the game's cockpit, cockpit-render-impl.md section 10). MADE UP.
constexpr uint8_t kSeatRgba[4] = {58, 60, 50, 255}, kSeatFrameRgba[4] = {38, 40, 38, 255}, kStickRgba[4] = {52, 52, 50, 255}, kGripRgba[4] = {22, 22, 22, 255},
                  kQuadrantRgba[4] = {34, 36, 34, 255}, kKnobRgba[4] = {26, 26, 26, 255}, kPedalRgba[4] = {46, 46, 44, 255}, kTubRgba[4] = {38, 40, 37, 255},
                  kTubFloorRgba[4] = {34, 36, 34, 255}, kBulkheadRgba[4] = {42, 44, 40, 255};
}
float anim_angle(const AnimAxis& a, const AnimInputs& in) noexcept {
    const float m = a.bias + a.cp * in.pitch + a.cr * in.roll + a.cy * in.yaw + a.ct * in.throttle;
    return a.gain * clampf(std::isfinite(m) ? m : 0.0f, -1.0f, 1.0f);
}
AnimRole surface_role(uint32_t group) noexcept {
    switch (group) {
    case 1: case 2: return AnimRole::pitch_surface;
    case 3: return AnimRole::rudder;
    case 5: case 6: return AnimRole::roll_surface;
    default: return AnimRole::none;
    }
}

bool build_mesh(const ModelData& m, const Options& o, Mesh& out, std::string& why) {
    Mesh r;
    if (o.eye_override) for (int i = 0; i < 3; ++i) r.eye[i] = o.eye[i];
    else if (m.has_eye) for (int i = 0; i < 3; ++i) r.eye[i] = m.eye[i];
    else { why = "model has no eye point (bake with the .pvb or set eye:x,y,z)"; return false; }
    for (uint32_t i : o.hide) if (i >= m.parts.size()) { why = "hide: part index out of range"; return false; }
    for (uint32_t i : o.show) if (i >= m.parts.size()) { why = "show: part index out of range"; return false; }
    const float* E = r.eye;
    const size_t np = m.parts.size();
    std::vector<int> slot(m.textures.size(), -1);
    auto visible = [&](size_t i) {
        const Part& p = m.parts[i];
        bool hidden = (p.mflags & (MF_HIDDEN | MF_COCKPIT_DUP)) != 0;
        if (!o.exterior && (p.mflags & MF_EXTERIOR)) hidden = true;
        if (!o.glass && (p.mflags & MF_GLASS)) hidden = true;
        if (std::find(o.show.begin(), o.show.end(), uint32_t(i)) != o.show.end()) hidden = false;
        if (std::find(o.hide.begin(), o.hide.end(), uint32_t(i)) != o.hide.end()) hidden = true;
        return !hidden;
    };
    // ---- COCKPIT-CLIP: the clip box (model units) and the triangle source tags
    const float U0 = kModelUnitsPerMetre;
    const bool use_box = o.clip && o.clip_box_set;
    if (use_box) {
        r.clip_box = true;
        r.clip_lo[0] = E[0] - o.clip_box[4] * U0; r.clip_hi[0] = E[0] + o.clip_box[4] * U0;
        r.clip_lo[1] = E[1] - o.clip_box[2] * U0; r.clip_hi[1] = E[1] + o.clip_box[3] * U0;
        r.clip_lo[2] = E[2] - o.clip_box[0] * U0; r.clip_hi[2] = E[2] + o.clip_box[1] * U0;
    }
    auto tag = [&](uint8_t src) { r.tri_src.resize(r.v.size() / 3, src); };
    auto bbox = [&](size_t i, float lo[3], float hi[3]) {
        const Part& p = m.parts[i];
        for (int a = 0; a < 3; ++a) { lo[a] = 1e30f; hi[a] = -1e30f; }
        for (uint32_t k = 0; k < p.index_count; ++k) {
            const float* q = m.vertices[m.indices[p.first_index + k]].pos;
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], q[a]); hi[a] = std::max(hi[a], q[a]); }
        }
    };
    auto centroid = [&](size_t i, float c[3]) {
        const Part& p = m.parts[i];
        double s[3] = {0, 0, 0};
        for (uint32_t k = 0; k < p.index_count; ++k) for (int a = 0; a < 3; ++a) s[a] += m.vertices[m.indices[p.first_index + k]].pos[a];
        for (int a = 0; a < 3; ++a) c[a] = p.index_count ? float(s[a] / p.index_count) : 0.0f;
    };
    // ---- COCKPIT-4 classification: canopy glass parts (glass pass), driven animated parts (+ their descendants)
    // Canopy glass: preferably the EXTERIOR model's canopy + windscreen glass (they enclose the cockpit; seen from inside they are back faces, so they are
    // emitted flipped). On the Draken the cockpit part's own big glass (p2 n1) covers only the overhead/side canopy, not the windscreen (cockpit4-impl.md
    // section 1), so it is the fallback; when the exterior glass is used, the cockpit canopy part is not drawn (it would tint the overhead view twice).
    std::vector<uint8_t> canopy(np, 0), skip(np, 0);
    if (o.canopy) {
        const float wide = 1.0f;   // the glass must extend > 1 unit (20 cm) left AND right of the eye: the canopy, not a gauge/gunsight glass
        std::vector<size_t> cand[2];   // 0 exterior, 1 cockpit part
        for (size_t i = 0; i < np; ++i) {
            const Part& p = m.parts[i];
            if (!p.index_count || !visible(i) || !(p.mflags & MF_BLEND)) continue;
            const bool ext = (p.mflags & MF_EXTERIOR) != 0;
            if (!ext && !(p.mflags & MF_GLASS)) continue;
            float lo[3], hi[3]; bbox(i, lo, hi);
            if (!(lo[0] < E[0] - wide && hi[0] > E[0] + wide && hi[1] > E[1] + (ext ? 0.0f : 0.5f))) continue;
            if (ext && !(lo[1] < E[1] + 3.0f && hi[2] > E[2] - 15.0f && lo[2] < E[2] + 15.0f)) continue;
            cand[ext ? 0 : 1].push_back(i);
        }
        const int use = cand[0].empty() ? 1 : 0;
        for (size_t i : cand[use]) { canopy[i] = 1; ++r.canopy_parts; }
        r.canopy_exterior = use == 0 && r.canopy_parts > 0;
        if (use == 0) for (size_t i : cand[1]) skip[i] = 1;
    }
    std::vector<int32_t> driven(np, -1);   // part -> index of the driving AnimPiece (itself or an animated ancestor)
    std::vector<AnimPiece> pieces;
    // STORES-RT: the baked weapon parts of one station form one piece (role store, no axes) in the animated range: apply_store_mask collapses it at run time.
    // Blended parts stay static (a mask cannot reach them); none of the baked stores are blended.
    if (o.stores) {
        for (size_t i = 0; i < np; ++i) {
            const Part& p = m.parts[i];
            uint32_t st = 0;
            bool sp = false;   // SP-STORES: "/sp<station>/" parts form their own pieces and mask bit space (spstores:0 leaves them ordinary static parts)
            if (!p.index_count || !visible(i) || canopy[i] || (p.mflags & MF_BLEND)) continue;
            if (!store_station_of_name(m.part_name(i), st)) {
                if (!o.spstores || !sp_station_of_name(m.part_name(i), st)) continue;
                sp = true;
            }
            size_t k = 0;
            while (k < pieces.size() && !(pieces[k].role == AnimRole::store && pieces[k].station == st && pieces[k].sp == sp)) ++k;
            if (k == pieces.size()) {
                AnimPiece a; a.role = AnimRole::store; a.naxes = 0; a.station = st; a.part = int32_t(i); a.sp = sp;
                pieces.push_back(a); (sp ? r.sp_stations : r.store_stations).push_back(st);
            }
            driven[i] = int32_t(k);
        }
        for (size_t k = 0; k < pieces.size(); ++k) {   // pivot = the vertex centroid of the station's parts
            double s3[3] = {0, 0, 0}; uint64_t n = 0;
            for (size_t i = 0; i < np; ++i) {
                if (driven[i] != int32_t(k)) continue;
                const Part& p = m.parts[i];
                for (uint32_t q = 0; q < p.index_count; ++q) { const float* v = m.vertices[m.indices[p.first_index + q]].pos; for (int a = 0; a < 3; ++a) s3[a] += v[a]; ++n; }
            }
            for (int a = 0; a < 3; ++a) pieces[k].pivot[a] = n ? float(s3[a] / double(n)) : 0.0f;
        }
    }
    if (o.anim) {
        for (size_t i = 0; i < np; ++i) {
            const Part& p = m.parts[i];
            if (p.anim < 0) continue;
            const AnimRole role = surface_role(p.group);
            const float range = p.hinge - p.rest;
            // located: the baker fitted the rest pose to the static geometry, or the rest is 0 and the stored rot[0] is itself the ~20 deg range (the
            // COCKPIT-1 reading; F-16/MiG-21 tails and rudders do not touch the static mesh, so their fit cannot confirm it). A non-zero unfitted rest is refused.
            const bool located = (p.aflags & kPartHingeFitted) || p.rest == 0.0f;
            const float deg = std::fabs(range) * 57.29578f;
            if (driven[i] >= 0) continue;   // STORES-RT: a weapon part is never a control surface, whatever its node group
            if (!p.index_count || !visible(i) || role == AnimRole::none || !located || !(deg >= 5.0f && deg <= 35.0f) || canopy[i]) {
                if (p.index_count && visible(i)) ++r.anim_unlocated;
                continue;
            }
            AnimPiece a; a.role = role; a.part = int32_t(i); a.naxes = 1;
            const float* M = p.local_to_model;   // row-major, column vectors: axis = local X (column 0), pivot = translation (column 3)
            float ax[3] = {M[0], M[4], M[8]}; const float an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (!(an > 1e-6f)) { ++r.anim_unlocated; continue; }
            for (int k = 0; k < 3; ++k) { a.ax[0].axis[k] = ax[k] / an; a.pivot[k] = M[4 * k + 3]; }
            // hinge sanity: elevators/ailerons hinge about a roughly LATERAL axis, rudders about an axis in the symmetry plane (the MiG-21 stabilator nodes
            // have a 50 deg swept local X and do not fit the static mesh: not driven rather than rotated about a wrong axis)
            if ((role == AnimRole::rudder) ? !(std::fabs(a.ax[0].axis[0]) < 0.3f) : !(std::fabs(a.ax[0].axis[0]) > 0.85f)) { ++r.anim_unlocated; continue; }
            float c[3]; centroid(i, c);
            const float d[3] = {c[0] - a.pivot[0], c[1] - a.pivot[1], c[2] - a.pivot[2]};
            const float* u = a.ax[0].axis;
            const float cr[3] = {u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0]};   // trailing-edge motion per +radian
            const float side = c[0] >= E[0] ? 1.0f : -1.0f;
            if (role == AnimRole::rudder) { a.ax[0].gain = (cr[0] >= 0 ? 1.0f : -1.0f) * std::fabs(range); a.ax[0].cy = 1.0f; }   // right pedal: trailing edge to +X
            else {
                a.ax[0].gain = (cr[1] >= 0 ? 1.0f : -1.0f) * std::fabs(range);   // positive command = trailing edge UP
                if (role == AnimRole::pitch_surface) { a.ax[0].cp = 1.0f; a.ax[0].cr = 0.5f * side; }   // stick aft: TE up; tailerons: right roll lifts the right TE
                else a.ax[0].cr = side;                                                                  // ailerons/elevons: right roll = right TE up, left TE down
            }
            driven[i] = int32_t(pieces.size());
            pieces.push_back(a);
            ++r.anim_surfaces;
        }
        for (size_t i = 0; i < np; ++i)   // descendants move with their animated ancestor (parents precede children: loader check)
            if (driven[i] < 0 && m.parts[i].parent >= 0 && driven[size_t(m.parts[i].parent)] >= 0 && !canopy[i]) driven[i] = driven[size_t(m.parts[i].parent)];
    }
    struct BlendPart { size_t part; float dist; };
    std::vector<BlendPart> blend, glass;
    auto emit_part_raw = [&](size_t i, bool flip) -> bool {
        const Part& p = m.parts[i];
        uint16_t mat = kNoTexture;
        if (p.texture >= 0) {
            int& s = slot[size_t(p.texture)];
            if (s < 0) {
                if (r.textures.size() >= kGpuTextures) return false;
                s = int(r.textures.size());
                r.textures.push_back(m.textures[size_t(p.texture)]);
            }
            mat = uint16_t(s);
        }
        if (p.mflags & MF_ALPHA_TEST) mat |= kMatAlphaTest;
        if (p.mflags & MF_BLEND) mat |= kMatBlend;
        if (p.mflags & MF_UNLIT) mat |= kMatUnlit;
        if (p.mflags & MF_TEX_ALPHA_NOT_COVERAGE) mat |= kMatTexAlphaNotCov;
        for (uint32_t k = 0; k < p.index_count; ++k) {
            const uint32_t kk = flip ? (k - k % 3) + (k % 3 == 0 ? 0u : 3u - k % 3) : k;   // flip: corners 0,2,1 (the exterior canopy faces the eye)
            const Vertex& s = m.vertices[m.indices[p.first_index + kk]];
            GpuVertex g{};
            std::memcpy(g.pos, s.pos, sizeof g.pos); std::memcpy(g.uv, s.uv, sizeof g.uv); std::memcpy(g.nrm, s.nrm, sizeof g.nrm);
            if (flip) for (int a = 0; a < 3; ++a) g.nrm[a] = int16_t(-std::max<int>(s.nrm[a], -32767));
            std::memcpy(g.rgba, s.rgba, 4); g.mat = mat;
            r.v.push_back(g);
        }
        return true;
    };
    // COCKPIT-CLIP: a non-exterior, non-glass part emitted with clippable = true is clipped against the box; every emitted triangle is tagged with its source.
    auto emit_part = [&](size_t i, bool flip, bool clippable) -> bool {
        const Part& p = m.parts[i];
        const size_t start = r.v.size();
        if (!emit_part_raw(i, flip)) return false;
        if (clippable && use_box && !(p.mflags & (MF_EXTERIOR | MF_GLASS))) clip_range(r.v, start, r.clip_lo, r.clip_hi, r.clip_removed, r.clip_cut);
        tag((p.mflags & MF_EXTERIOR) ? 0 : 1);
        return true;
    };
    auto eye_dist2 = [&](size_t i) {
        float c[3]; centroid(i, c);
        float d2 = 0; for (int a = 0; a < 3; ++a) { const float dv = c[a] - E[a]; d2 += dv * dv; }
        return d2;
    };
    std::vector<BlendPart> dblend;   // driven blended parts (anim:1): emitted unclipped at the head of the blend range, posed with their piece
    // ---- [opaque static]
    for (size_t i = 0; i < np; ++i) {
        const Part& p = m.parts[i];
        if (!p.index_count) continue;
        if (!visible(i) || skip[i]) { ++r.parts_hidden; continue; }
        ++r.parts_drawn;
        if (canopy[i]) { glass.push_back({i, eye_dist2(i)}); continue; }
        if (p.mflags & MF_BLEND) { (driven[i] >= 0 ? dblend : blend).push_back({i, eye_dist2(i)}); continue; }   // driven blend: animated blend range below
        if (driven[i] >= 0) continue;   // emitted in the animated range below
        if (!emit_part(i, false, true)) { why = "more than 4 textures used"; return false; }
    }
    // The drawn cockpit (non-exterior, opaque) geometry's lowest point and x/z footprint: the COCKPIT-3 floor closure or the COCKPIT-4 interior.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < np; ++i) {
        const Part& p = m.parts[i];
        if (!p.index_count || (p.mflags & (MF_EXTERIOR | MF_BLEND)) || !visible(i)) continue;
        for (uint32_t k = 0; k < p.index_count; ++k) {
            const float* q = m.vertices[m.indices[p.first_index + k]].pos;
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], q[a]); hi[a] = std::max(hi[a], q[a]); }
        }
    }
    if (use_box) for (int a = 0; a < 3; ++a) { lo[a] = std::max(lo[a], r.clip_lo[a]); hi[a] = std::min(hi[a], r.clip_hi[a]); }   // COCKPIT-CLIP: inside the box
    const bool footprint = lo[0] < hi[0] && lo[2] < hi[2] && lo[1] < E[1];
    const float U = kModelUnitsPerMetre;
    // COCKPIT-CLIP fit: the drawn exterior skin (incl. the canopy glass) in the cockpit's z slice (+-1 u; every fit ray runs at a synthetic vertex's z, which
    // lies in the footprint, along X or Y). Empty (no airframe around the cockpit, e.g. the synthetic test models) = nothing is tessellated or moved.
    Skin skin;
    if (o.clip && footprint) {
        for (size_t i = 0; i < np; ++i) {
            const Part& p = m.parts[i];
            if (!p.index_count || !(p.mflags & MF_EXTERIOR) || p.source_part == kStoresSourcePart || !visible(i)) continue;   // STORES: attached stores are not skin
            for (uint32_t k = 0; k + 2 < p.index_count; k += 3) {
                const float* q[3] = {m.vertices[m.indices[p.first_index + k]].pos, m.vertices[m.indices[p.first_index + k + 1]].pos,
                                     m.vertices[m.indices[p.first_index + k + 2]].pos};
                const float mn = std::min({q[0][2], q[1][2], q[2][2]}), mx = std::max({q[0][2], q[1][2], q[2][2]});
                if (mx >= lo[2] - 1.0f && mn <= hi[2] + 1.0f) for (int c = 0; c < 3; ++c) skin.t.insert(skin.t.end(), q[c], q[c] + 3);
            }
        }
        r.fit_skin = uint32_t(skin.t.size() / 9);
    }
    const int grid = skin.t.empty() ? 1 : 8;   // tessellation of the large synthetic faces (only when there is a skin to fit them to)
    // Fit + clip the synthetic geometry v[first..): pull every vertex inside the skin (each distinct position once: grid corners are shared by up to 6
    // triangle corners), re-derive flat normals, then clip against the box.
    auto finish_synthetic = [&](size_t first) {
        if (!skin.t.empty()) {
            std::vector<float> h;
            const float mg = kClipMarginM * U;
            std::vector<size_t> order(r.v.size() - first);
            for (size_t k = 0; k < order.size(); ++k) order[k] = first + k;
            auto key = [&](size_t k) { return std::array<float, 3>{r.v[k].pos[0], r.v[k].pos[1], r.v[k].pos[2]}; };
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
            std::array<float, 3> last_in{}, last_out{}; bool have = false, last_moved = false;
            for (size_t k : order) {
                const std::array<float, 3> in = key(k);
                if (!have || in != last_in) {
                    float p[3] = {in[0], in[1], in[2]};
                    last_moved = fit_point(skin, p, 0.0f, mg, h);   // the airframe's symmetry plane x = 0 (e06b/su32 seat the pilot off-centre)
                    last_in = in; last_out = {p[0], p[1], p[2]}; have = true;
                }
                if (last_moved) { std::memcpy(r.v[k].pos, last_out.data(), sizeof r.v[k].pos); ++r.fit_moved; }
            }
            renormal_range(r.v, first);
        }
        if (use_box) clip_range(r.v, first, r.clip_lo, r.clip_hi, r.clip_interior_removed, r.clip_interior_cut);
    };
    std::vector<GpuVertex> interior_anim;   // the interior's animated pieces (vertices), appended after the model's animated parts
    std::vector<AnimPiece> interior_pieces;
    if (o.interior && footprint) {
        // ---- COCKPIT-4 procedural interior (MADE UP; cockpit4-impl.md section 2). Metres relative to the eye, converted at 5 u/m, clamped into the cockpit
        // footprint: a closed tub (floor 1.05 m below the eye, side walls up to 0.30 m below the eye, front wall under the panel, rear bulkhead), an ejection
        // seat (pan, back, headrest, side rails), a centre stick, a throttle quadrant on the left, rudder pedals. All untextured, opaque, vertex-coloured.
        const size_t first_static = r.v.size();
        const float zf = lo[2], xl = lo[0], xr = hi[0];
        // SEAT-CLEARANCE: the seat is pushed back (seat_back_max) beyond the old footprint-clamped position. The tub (floor, side walls, rear bulkhead) must still
        // enclose it, else the floor/pan ahead of and below the moved seat is a hole: the rear edge follows the seat's rear (+0.02 m) when that is further back than
        // the cockpit footprint (never on a real model: footprints end >= 0.68 m behind the eye, the seat at <= 0.45 m), and never with seatback:0.
        const float back_old = std::max(0.10f, std::min(0.36f, (hi[2] - E[2]) / U - 0.02f));   // the seat stays inside the footprint
        float back_max = seat_back_max(back_old, o.seat_back);
        const float zr = (back_max > back_old) ? std::max(hi[2], E[2] + (back_max + 0.02f) * U) : hi[2];
        float fy = std::min(E[1] - 1.05f * U, lo[1] - 0.05f * U);             // tub floor (never above the old floor closure)
        if (use_box) fy = std::max(fy, r.clip_lo[1]);                          // COCKPIT-CLIP: a box above it keeps the floor (at the box) instead of opening it
        const float wall_top = std::max(lo[1], E[1] - 0.30f * U);
        r.interior_floor_y = fy;
        { const float c[4][3] = {{xl, fy, zr}, {xr, fy, zr}, {xr, fy, zf}, {xl, fy, zf}}, n[3] = {0, 1, 0}; emit_quad_grid(r.v, c, n, kTubFloorRgba, grid); }        // floor (up)
        { const float c[4][3] = {{xl, fy, zr}, {xl, fy, zf}, {xl, wall_top, zf}, {xl, wall_top, zr}}, n[3] = {1, 0, 0}; emit_quad_grid(r.v, c, n, kTubRgba, grid); } // left wall (faces +X)
        { const float c[4][3] = {{xr, fy, zf}, {xr, fy, zr}, {xr, wall_top, zr}, {xr, wall_top, zf}}, n[3] = {-1, 0, 0}; emit_quad_grid(r.v, c, n, kTubRgba, grid); } // right wall (faces -X)
        {   // front wall (faces aft, +Z) behind the panel, up to 0.25 m below the eye but never above the line of sight over the glareshield (it closes
            // the gaps under/between the panel parts without hiding any of the forward view)
            float tan_gs = -1e9f;
            for (size_t i = 0; i < np; ++i) {
                const Part& p = m.parts[i];
                if (!p.index_count || (p.mflags & (MF_EXTERIOR | MF_BLEND)) || !visible(i)) continue;
                for (uint32_t k = 0; k < p.index_count; ++k) {
                    const float* q = m.vertices[m.indices[p.first_index + k]].pos;
                    if (E[2] - q[2] > 1.0f && std::fabs(q[0] - E[0]) < 0.5f) tan_gs = std::max(tan_gs, (q[1] - E[1]) / (E[2] - q[2]));
                }
            }
            float yt = E[1] - 0.25f * U;
            if (tan_gs > -1e8f) yt = std::min(yt, E[1] + (tan_gs - 0.02f) * (E[2] - zf));
            yt = std::max(yt, lo[1] + 0.02f * U);
          const float c[4][3] = {{xl, fy, zf}, {xr, fy, zf}, {xr, yt, zf}, {xl, yt, zf}}, n[3] = {0, 0, 1}; emit_quad_grid(r.v, c, n, kTubRgba, grid); }
        {   // rear bulkhead behind the seat (faces forward, -Z), up to the airframe's top line just behind the cockpit: seen from inside the canopy the spine's
            // skin is back-facing (culled), so a lower bulkhead would leave a hole into the empty fuselage. Clamped to [eye - 0.20 m, eye + 0.50 m].
            float spine = E[1] - 0.20f * U;
            for (size_t i = 0; i < np; ++i) {
                const Part& p = m.parts[i];
                if (!p.index_count || !(p.mflags & MF_EXTERIOR) || (p.mflags & MF_BLEND) || !visible(i)) continue;
                for (uint32_t k = 0; k < p.index_count; ++k) {
                    const float* q = m.vertices[m.indices[p.first_index + k]].pos;
                    if (q[0] > xl && q[0] < xr && q[2] > zr - 0.2f * U && q[2] < zr + 0.4f * U) spine = std::max(spine, q[1]);
                }
            }
            const float yt = std::min(spine, E[1] + 0.50f * U);
          const float c[4][3] = {{xr, fy, zr}, {xl, fy, zr}, {xl, yt, zr}, {xr, yt, zr}}, n[3] = {0, 0, -1}; emit_quad_grid(r.v, c, n, kBulkheadRgba, grid); }
        auto box = [&](float x0, float x1, float y0, float y1, float z0, float z1, const uint8_t* rgba, std::vector<GpuVertex>& dst) {
            const float b0[3] = {E[0] + x0 * U, y0, E[2] + z0 * U}, b1[3] = {E[0] + x1 * U, y1, E[2] + z1 * U};
            emit_box(dst, b0, b1, rgba);
        };
        // back_max: see above (SEAT-CLEARANCE: never nearer than o.seat_back model metres; the clip box / skin fit still apply)
        r.seat_front_u = (back_max - 0.15f) * U;
        const float pan_top = E[1] - 0.80f * U;
        box(-0.20f, 0.20f, fy, pan_top - 0.10f * U, -0.15f, back_max - 0.08f, kSeatFrameRgba, r.v);                         // seat bucket base
        box(-0.23f, 0.23f, pan_top - 0.10f * U, pan_top, -0.22f, back_max - 0.08f, kSeatRgba, r.v);                          // seat pan cushion
        box(-0.22f, 0.22f, pan_top, E[1] - 0.02f * U, back_max - 0.12f, back_max - 0.02f, kSeatRgba, r.v);                  // seat back
        box(-0.12f, 0.12f, E[1] - 0.06f * U, E[1] + 0.16f * U, back_max - 0.15f, back_max - 0.04f, kSeatRgba, r.v);         // headrest
        {   // side consoles along the tub walls (the game's cockpit part only has the slanted upper side panels): from the floor to 0.47 m below the eye,
            // inboard edge 0.245 m from the centre line, 0.30 m ahead of the eye to the seat back
            const float ctop = E[1] - 0.47f * U, cz0 = std::max(-0.30f, (zf - E[2]) / U + 0.02f), cz1 = back_max - 0.02f;
            const float lx = (xl - E[0]) / U, rx = (xr - E[0]) / U;
            if (lx < -0.26f) box(lx, -0.245f, fy, ctop, cz0, cz1, kQuadrantRgba, r.v);
            if (rx > 0.26f) box(0.245f, rx, fy, ctop, cz0, cz1, kQuadrantRgba, r.v);
        }
        box(-0.27f, -0.23f, pan_top - 0.10f * U, E[1] + 0.24f * U, back_max - 0.10f, back_max, kSeatFrameRgba, r.v);       // seat rails (ejection seat)
        box(0.23f, 0.27f, pan_top - 0.10f * U, E[1] + 0.24f * U, back_max - 0.10f, back_max, kSeatFrameRgba, r.v);
        // animated interior pieces (posed even with anim:0 = rest; anim:1 drives them)
        auto piece = [&](AnimRole role, const float pivot[3], std::vector<GpuVertex>& verts) {
            AnimPiece a; a.role = role; a.naxes = 1;
            std::memcpy(a.pivot, pivot, sizeof a.pivot);
            a.first = uint32_t(interior_anim.size()); a.count = uint32_t(verts.size());
            interior_anim.insert(interior_anim.end(), verts.begin(), verts.end());
            interior_pieces.push_back(a);
            return &interior_pieces.back();
        };
        if (o.stick == 1) {   // side-stick on the right console (F-16): short grip, the console top 0.47 m below the eye, 0.12 m ahead of the eye
            const float ctop = E[1] - 0.47f * U, sx = 0.31f, sz = -0.12f;
            const float pv[3] = {E[0] + sx * U, ctop, E[2] + sz * U};
            std::vector<GpuVertex> s;
            box(sx - 0.03f, sx + 0.03f, ctop - 0.01f * U, ctop + 0.03f * U, sz - 0.04f, sz + 0.04f, kSeatFrameRgba, s);    // boot
            box(sx - 0.018f, sx + 0.018f, ctop + 0.03f * U, ctop + 0.15f * U, sz - 0.03f, sz + 0.025f, kGripRgba, s);   // grip
            AnimPiece* a = piece(AnimRole::stick, pv, s);
            a->naxes = 2;   // a force stick barely moves: +-4 deg
            a->ax[0].axis[0] = 1; a->ax[0].axis[1] = 0; a->ax[0].axis[2] = 0; a->ax[0].gain = 0.07f; a->ax[0].cp = 1.0f;
            a->ax[1].axis[0] = 0; a->ax[1].axis[1] = 0; a->ax[1].axis[2] = 1; a->ax[1].gain = 0.07f; a->ax[1].cr = -1.0f;
        }
        if (o.stick == 0) {   // centre stick: pivot near the floor 0.40 m ahead of the eye; pitch about +X (pull = top aft), roll about +Z (right = top to +X)
            const float sz = std::max(-0.40f, (zf - E[2]) / U + 0.15f);
            const float pv[3] = {E[0], fy + 0.05f * U, E[2] + sz * U};
            const float top = E[1] - 0.52f * U;
            std::vector<GpuVertex> s;
            box(-0.06f, 0.06f, fy, pv[1] + 0.04f * U, sz - 0.06f, sz + 0.06f, kSeatFrameRgba, s);   // boot
            box(-0.015f, 0.015f, pv[1], top, sz - 0.015f, sz + 0.015f, kStickRgba, s);             // shaft
            box(-0.022f, 0.022f, top, top + 0.13f * U, sz - 0.03f, sz + 0.025f, kGripRgba, s);     // grip
            AnimPiece* a = piece(AnimRole::stick, pv, s);
            a->naxes = 2;
            a->ax[0].axis[0] = 1; a->ax[0].axis[1] = 0; a->ax[0].axis[2] = 0; a->ax[0].gain = 0.30f; a->ax[0].cp = 1.0f;     // +-17 deg
            a->ax[1].axis[0] = 0; a->ax[1].axis[1] = 0; a->ax[1].axis[2] = 1; a->ax[1].gain = 0.26f; a->ax[1].cr = -1.0f;    // +-15 deg
        }
        {   // throttle quadrant on the left console + lever (forward = more throttle)
            const float qx = -0.30f, qtop = E[1] - 0.42f * U, qz0 = -0.12f, qz1 = 0.14f;
            box(qx - 0.03f, qx + 0.03f, qtop - 0.10f * U, qtop, qz0, qz1, kQuadrantRgba, r.v);
            const float pv[3] = {E[0] + qx * U, qtop, E[2] + 0.5f * (qz0 + qz1) * U};
            std::vector<GpuVertex> s;
            box(qx - 0.008f, qx + 0.008f, qtop - 0.01f * U, qtop + 0.12f * U, 0.01f - 0.008f, 0.01f + 0.008f, kStickRgba, s);   // lever
            box(qx - 0.025f, qx + 0.02f, qtop + 0.12f * U, qtop + 0.17f * U, 0.01f - 0.03f, 0.01f + 0.03f, kKnobRgba, s);       // handle
            AnimPiece* a = piece(AnimRole::throttle, pv, s);
            a->ax[0].gain = -0.52f; a->ax[0].bias = -1.0f; a->ax[0].ct = 2.0f;   // throttle 0 -> lever aft (+30 deg), 1 -> forward (-30 deg)
        }
        for (int sgn : {-1, 1}) {   // rudder pedals hanging from a pivot 0.30 m above them; right pedal forward for nose-right
            const float pz = std::max(-0.85f, (zf - E[2]) / U + 0.08f), px = 0.12f * float(sgn);
            const float py0 = fy + 0.08f * U, py1 = fy + 0.24f * U;
            const float pv[3] = {E[0] + px * U, py1 + 0.15f * U, E[2] + pz * U};
            std::vector<GpuVertex> s;
            box(px - 0.04f, px + 0.04f, py0, py1, pz - 0.012f, pz + 0.012f, kPedalRgba, s);
            AnimPiece* a = piece(AnimRole::pedal, pv, s);
            a->ax[0].gain = 0.17f; a->ax[0].cy = float(sgn);   // +-10 deg
        }
        finish_synthetic(first_static);   // COCKPIT-CLIP: fit the static interior into the airframe, clip it to the box
        r.interior_triangles = uint32_t((r.v.size() - first_static) / 3);
        tag(2);
        r.interior = true;
    } else if (o.floor && footprint) {
        // COCKPIT-3 floor closure: an up-facing opaque quad at the lowest point of the drawn cockpit geometry spanning its x/z footprint.
        r.floor = true; r.floor_y = lo[1]; r.floor_x[0] = lo[0]; r.floor_x[1] = hi[0]; r.floor_z[0] = lo[2]; r.floor_z[1] = hi[2];
        const size_t first_floor = r.v.size();
        const float c[4][3] = {{lo[0], lo[1], hi[2]}, {hi[0], lo[1], hi[2]}, {hi[0], lo[1], lo[2]}, {lo[0], lo[1], lo[2]}}, n[3] = {0, 1, 0};   // CCW from above
        emit_quad_grid(r.v, c, n, kFloorRgba, grid);
        finish_synthetic(first_floor);
        tag(2);
    }
    // ---- [opaque animated]: the driven model parts (each piece followed by its descendants), then the interior's animated pieces
    r.anim_first = uint32_t(r.v.size());
    for (size_t k = 0; k < pieces.size(); ++k) {
        pieces[k].first = uint32_t(r.v.size());
        for (size_t i = 0; i < np; ++i)
            if (driven[i] == int32_t(k) && m.parts[i].index_count && visible(i) && !(m.parts[i].mflags & MF_BLEND))
                if (!emit_part(i, false, false)) { why = "more than 4 textures used"; return false; }
        pieces[k].count = uint32_t(r.v.size()) - pieces[k].first;
    }
    for (AnimPiece& a : interior_pieces) { a.first += uint32_t(r.v.size()); pieces.push_back(a); }
    r.v.insert(r.v.end(), interior_anim.begin(), interior_anim.end());
    tag(2);
    r.interior_triangles += uint32_t(interior_anim.size() / 3);
    r.opaque_vertices = uint32_t(r.v.size());
    // Driven blended parts continue the animated range (so it stays one contiguous upload) at the head of the blend range: never box-clipped, one extra
    // AnimPiece (same hinge) per driving piece. They are drawn before the static blend parts (their relative order to those is not depth sorted).
    {
        const size_t nmodel = pieces.size() - interior_pieces.size();
        std::stable_sort(dblend.begin(), dblend.end(), [](const BlendPart& a, const BlendPart& b) { return a.dist > b.dist; });
        for (size_t k = 0; k < nmodel; ++k) {
            AnimPiece a = pieces[k]; a.first = uint32_t(r.v.size());
            for (const BlendPart& b : dblend)
                if (driven[b.part] == int32_t(k)) if (!emit_part(b.part, false, false)) { why = "more than 4 textures used"; return false; }
            a.count = uint32_t(r.v.size()) - a.first;
            if (a.count) pieces.push_back(a);
        }
    }
    r.anim_vertices = uint32_t(r.v.size()) - r.anim_first;   // opaque animated range + the driven blend vertices that follow it
    r.anim = std::move(pieces);
    // ---- [blend] back to front from the eye, then [canopy glass] back to front
    std::stable_sort(blend.begin(), blend.end(), [](const BlendPart& a, const BlendPart& b) { return a.dist > b.dist; });
    for (const BlendPart& b : blend) if (!emit_part(b.part, false, true)) { why = "more than 4 textures used"; return false; }
    r.blend_vertices = uint32_t(r.v.size()) - r.opaque_vertices;
    std::stable_sort(glass.begin(), glass.end(), [](const BlendPart& a, const BlendPart& b) { return a.dist > b.dist; });
    for (const BlendPart& b : glass) if (!emit_part(b.part, r.canopy_exterior, false)) { why = "more than 4 textures used"; return false; }
    r.glass_vertices = uint32_t(r.v.size()) - r.opaque_vertices - r.blend_vertices;
    r.triangles = uint32_t(r.v.size() / 3);   // every range above is whole triangles (the clip and the fit re-fan / drop whole triangles)
    if (r.v.empty()) { why = "no visible triangles"; return false; }
    // HUD glass: the drawn, non-exterior glass part whose centroid is closest to the forward axis from the eye (1..40 units ahead; j35j: the reflector sight glass n4, 1.97 u ahead).
    float best = 1e9f;
    std::vector<BlendPart> hudc = blend; hudc.insert(hudc.end(), dblend.begin(), dblend.end());
    for (const BlendPart& b : hudc) {
        const Part& p = m.parts[b.part];
        if (!(p.mflags & MF_GLASS) || (p.mflags & MF_EXTERIOR)) continue;
        double c[3] = {0, 0, 0};
        for (uint32_t k = 0; k < p.index_count; ++k) for (int a = 0; a < 3; ++a) c[a] += m.vertices[m.indices[p.first_index + k]].pos[a];
        float dv[3]; for (int a = 0; a < 3; ++a) dv[a] = float(c[a] / p.index_count) - r.eye[a];
        const float ahead = -dv[2];
        if (!(ahead > 1.0f && ahead < 40.0f)) continue;
        const float ang = std::atan2(std::sqrt(dv[0] * dv[0] + dv[1] * dv[1]), ahead);
        if (ang >= best) continue;
        float tl = 1e9f, tr = -1e9f, tu = -1e9f, td = 1e9f; bool okc = true;
        for (uint32_t k = 0; k < p.index_count; ++k) {
            const float* q = m.vertices[m.indices[p.first_index + k]].pos;
            const float az = -(q[2] - r.eye[2]);
            if (!(az > 0.1f)) { okc = false; break; }
            const float tx = (q[0] - r.eye[0]) / az, ty = (q[1] - r.eye[1]) / az;
            tl = std::min(tl, tx); tr = std::max(tr, tx); td = std::min(td, ty); tu = std::max(tu, ty);
        }
        if (!okc) continue;
        best = ang;
        r.hud.valid = true; r.hud.part = uint32_t(b.part);
        for (int a = 0; a < 3; ++a) r.hud.centre[a] = float(c[a] / p.index_count);
        r.hud.distance = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
        r.hud.tan_l = -tl; r.hud.tan_r = tr; r.hud.tan_u = tu; r.hud.tan_d = -td;
    }
    // CFID: the cockpit-rim shadow box (model units, rest pose; the camera's cscale does not change the mesh)
    r.shadow = cockpit_shadow_box(m, r.eye, r.shadow_box);
    out = std::move(r);
    return true;
}
void pose_anim(const Mesh& me, const AnimInputs& in, std::vector<GpuVertex>& out) {
    out.assign(me.v.begin() + me.anim_first, me.v.begin() + me.anim_first + me.anim_vertices);
    for (const AnimPiece& a : me.anim) {
        float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (int k = a.naxes - 1; k >= 0; --k) {   // R = R(ax0) * R(ax1): ax1 applied first
            float Q[9], T[9]; axis_rot(a.ax[k].axis, anim_angle(a.ax[k], in), Q);
            mat3_mul(Q, R, T); std::memcpy(R, T, sizeof R);
        }
        for (uint32_t i = 0; i < a.count; ++i) {
            const uint32_t src = a.first + i;
            if (src < me.anim_first || src - me.anim_first >= out.size()) continue;
            GpuVertex& g = out[src - me.anim_first];
            const GpuVertex& s = me.v[src];
            const float d[3] = {s.pos[0] - a.pivot[0], s.pos[1] - a.pivot[1], s.pos[2] - a.pivot[2]};
            const float n[3] = {s.nrm[0] / 32767.0f, s.nrm[1] / 32767.0f, s.nrm[2] / 32767.0f};
            for (int r = 0; r < 3; ++r) {
                g.pos[r] = a.pivot[r] + R[r * 3] * d[0] + R[r * 3 + 1] * d[1] + R[r * 3 + 2] * d[2];
                g.nrm[r] = int16_t(std::lround(clampf(R[r * 3] * n[0] + R[r * 3 + 1] * n[1] + R[r * 3 + 2] * n[2], -1.0f, 1.0f) * 32767.0f));
            }
        }
    }
}
// "<anything><tag><digits>/<rest>" -> digits; the first match from the left that has digits and a closing slash wins (tag = "/store" or "/sp").
static bool station_after_tag(const std::string& name, const char* tag, size_t taglen, uint32_t& station) noexcept {
    size_t at = 0;
    for (;;) {
        const size_t f = name.find(tag, at);
        if (f == std::string::npos) return false;
        size_t q = f + taglen, d = q;
        uint64_t v = 0;
        while (d < name.size() && name[d] >= '0' && name[d] <= '9' && d - q < 9) { v = v * 10 + uint64_t(name[d] - '0'); ++d; }
        if (d > q && d < name.size() && name[d] == '/') { station = uint32_t(v); return true; }
        at = f + 1;
    }
}
bool store_station_of_name(const std::string& name, uint32_t& station) noexcept { return station_after_tag(name, "/store", 6, station); }
bool sp_station_of_name(const std::string& name, uint32_t& station) noexcept { return station_after_tag(name, "/sp", 3, station); }
// Collapse every piece of one group (sp = special weapon, else standard) whose bit is set in `hidden` (bit i = stations[i]) to its pivot.
static uint32_t apply_group_mask(const Mesh& me, bool sp, const std::vector<uint32_t>& stations, uint32_t hidden, std::vector<GpuVertex>& posed) {
    if (!hidden || posed.size() != me.anim_vertices) return 0;
    uint32_t n = 0;
    for (const AnimPiece& a : me.anim) {
        if (a.role != AnimRole::store || a.sp != sp) continue;
        unsigned bit = 0;
        while (bit < stations.size() && stations[bit] != a.station) ++bit;
        if (bit >= 32 || bit >= stations.size() || !(hidden & (1u << bit))) continue;
        for (uint32_t i = 0; i < a.count; ++i) {
            const uint32_t src = a.first + i;
            if (src < me.anim_first || src - me.anim_first >= posed.size()) continue;
            GpuVertex& g = posed[src - me.anim_first];
            for (int r = 0; r < 3; ++r) g.pos[r] = a.pivot[r];
            ++n;
        }
    }
    return n;
}
uint32_t apply_store_mask(const Mesh& me, uint32_t hidden, std::vector<GpuVertex>& posed) { return apply_group_mask(me, false, me.store_stations, hidden, posed); }
uint32_t apply_sp_mask(const Mesh& me, uint32_t hidden, std::vector<GpuVertex>& posed) { return apply_group_mask(me, true, me.sp_stations, hidden, posed); }
void pose_stores_only(const Mesh& me, uint32_t hidden, std::vector<GpuVertex>& out) {
    out.assign(me.v.begin() + me.anim_first, me.v.begin() + me.anim_first + me.anim_vertices);
    apply_store_mask(me, hidden, out);
}
void pose_stores_only(const Mesh& me, uint32_t hidden, uint32_t sp_hidden, std::vector<GpuVertex>& out) {
    out.assign(me.v.begin() + me.anim_first, me.v.begin() + me.anim_first + me.anim_vertices);
    apply_store_mask(me, hidden, out);
    apply_sp_mask(me, sp_hidden, out);
}
void apply_pose(Mesh& me, const std::vector<GpuVertex>& posed) {
    if (posed.size() != me.anim_vertices) return;
    std::copy(posed.begin(), posed.end(), me.v.begin() + me.anim_first);
}
const AnimInputs& anim_update(AnimState& s, const float right[3], const float up[3], const float fwd[3], const float pos[3], double t, const AnimTuning& k) noexcept {
    float cur[9];
    for (int i = 0; i < 3; ++i) { cur[i] = right[i]; cur[3 + i] = up[i]; cur[6 + i] = -fwd[i]; }   // body columns: right, up, back
    if (!finite_all(cur, 9) || !finite_all(pos, 3) || !std::isfinite(t)) { if (s.have) ++s.resets; s.have = false; s.have_speed = false; return s.in; }
    const double dt = t - s.t;
    if (!s.have || !(dt > 1e-4) || dt > double(k.max_dt)) {
        if (s.have) ++s.resets;
        s.have = true; s.have_speed = false; s.t = t;
        std::memcpy(s.axes, cur, sizeof s.axes); std::memcpy(s.pos, pos, sizeof s.pos);
        return s.in;
    }
    float Rr[9];   // R_prev^T R_cur: the rotation since the previous sample, in the previous body frame
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        Rr[i * 3 + j] = s.axes[3 * i] * cur[3 * j] + s.axes[3 * i + 1] * cur[3 * j + 1] + s.axes[3 * i + 2] * cur[3 * j + 2];
    const float w[3] = {Rr[7] - Rr[5], Rr[2] - Rr[6], Rr[3] - Rr[1]};
    const float wn = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const float ang = std::atan2(0.5f * wn, 0.5f * (Rr[0] + Rr[4] + Rr[8] - 1.0f));
    float om[3] = {0, 0, 0};
    if (wn > 1e-9f) for (int i = 0; i < 3; ++i) om[i] = w[i] / wn * ang / float(dt);
    const float tp = clampf(om[0] / k.pitch_full, -1, 1), tr = clampf(-om[2] / k.roll_full, -1, 1), ty = clampf(-om[1] / k.yaw_full, -1, 1);
    const float dp[3] = {pos[0] - s.pos[0], pos[1] - s.pos[1], pos[2] - s.pos[2]};
    const float speed = std::sqrt(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]) / float(dt);
    const float a = 1.0f - std::exp(-float(dt) / k.tau), at = 1.0f - std::exp(-float(dt) / (4.0f * k.tau));
    s.in.pitch += (tp - s.in.pitch) * a; s.in.roll += (tr - s.in.roll) * a; s.in.yaw += (ty - s.in.yaw) * a;
    if (s.have_speed) {
        const float th = 0.5f + 0.5f * clampf((speed - s.speed) / float(dt) / k.accel_full, -1, 1);
        s.in.throttle += (th - s.in.throttle) * at;
    }
    s.speed = speed; s.have_speed = true; s.t = t;
    std::memcpy(s.axes, cur, sizeof s.axes); std::memcpy(s.pos, pos, sizeof s.pos);
    ++s.updates;
    return s.in;
}

// ------------------------------------------------------------------------------------------------------------------- camera
bool eye_camera(const float quat[4], const float pos[3], const float fov[4], const float anchor[3], float yaw, const float pvb[3], float upm,
    float near_u, EyeCam& o) noexcept {
    if (!finite_all(quat, 4) || !finite_all(pos, 3) || !finite_all(fov, 4) || !finite_all(anchor, 3) || !finite_all(pvb, 3) || !std::isfinite(yaw)) return false;
    if (!(upm > 0.0f && upm < 1000.0f) || !(near_u > 0.0f && near_u < 100.0f)) return false;
    for (int i = 0; i < 4; ++i) if (!(std::fabs(fov[i]) < 1.55f)) return false;
    if (!(fov[0] < fov[1] && fov[3] < fov[2])) return false;
    const double qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
    const double qn = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (!(qn > 0.5 && qn < 2.0)) return false;
    const double x = qx / qn, y = qy / qn, z = qz / qn, w = qw / qn;
    const double col[3][3] = {   // the eye axes in LOCAL (same expression as xr_stereo::world_composite_params)
        {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
        {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
        {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
    const double c = std::cos(double(yaw)), s = std::sin(double(yaw));
    auto turn = [&](const double v[3], double t[3]) { t[0] = c * v[0] - s * v[2]; t[1] = v[1]; t[2] = s * v[0] + c * v[2]; };
    double X[3], Y[3], Z[3], P[3];
    turn(col[0], X); turn(col[1], Y); turn(col[2], Z);
    const double rel[3] = {double(pos[0]) - anchor[0], double(pos[1]) - anchor[1], double(pos[2]) - anchor[2]};
    turn(rel, P);
    double E[3];
    for (int i = 0; i < 3; ++i) E[i] = double(pvb[i]) + double(upm) * P[i];
    const double tl = std::tan(double(fov[0])), tr = std::tan(double(fov[1])), tu = std::tan(double(fov[2])), td = std::tan(double(fov[3]));
    const double ax = 2.0 / (tr - tl), bx = (tr + tl) / (tr - tl), ay = 2.0 / (tu - td), by = (tu + td) / (tu - td);
    auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    double M[16];
    for (int i = 0; i < 3; ++i) {
        M[i] = ax * X[i] + bx * Z[i];
        M[4 + i] = ay * Y[i] + by * Z[i];
        M[8 + i] = 0.0;
        M[12 + i] = -Z[i];
    }
    M[3] = -(ax * dot(X, E) + bx * dot(Z, E));
    M[7] = -(ay * dot(Y, E) + by * dot(Z, E));
    M[11] = double(near_u);
    M[15] = dot(Z, E);
    EyeCam r{};
    for (int i = 0; i < 16; ++i) r.m[i] = float(M[i]);
    for (int i = 0; i < 3; ++i) { r.eye[i] = float(E[i]); r.right[i] = float(X[i]); r.up[i] = float(Y[i]); r.back[i] = float(Z[i]); }
    if (!finite_all(r.m, 16) || !finite_all(r.eye, 3)) return false;
    o = r;
    return true;
}
Constants make_constants(const EyeCam& e, const Options& o, float exposure, uint32_t tonemap, const float* light_model, const float* tint, const float* sky,
    const ReflBasis* refl, const float* shadow_box) noexcept {
    Constants k{};
    std::memcpy(k.m, e.m, sizeof k.m);
    const float* L = kLight;
    if (light_model && finite_all(light_model, 3) &&
        (light_model[0] * light_model[0] + light_model[1] * light_model[1] + light_model[2] * light_model[2]) > 1e-12f) L = light_model;
    const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int i = 0; i < 3; ++i) k.light[i] = L[i] / ln;
    k.ambient = o.ambient; k.gain = o.gain; k.exposure = std::isfinite(exposure) ? exposure : 1.0f; k.tonemap = tonemap ? 1u : 0u; k.cut = o.cut;
    k.base_vertex = 0; k.pass = 0; k.flags = o.display ? kCFlagDisplay : 0u;
    for (int i = 0; i < 3; ++i) k.tint[i] = (tint && std::isfinite(tint[i])) ? std::min(1.5f, std::max(0.5f, tint[i])) : 1.0f;
    // COCKPIT-4 canopy glass
    const bool sky_ok = sky && finite_all(sky, 3);
    for (int i = 0; i < 3; ++i) {
        k.eye[i] = e.eye[i];
        k.trans[i] = std::pow(sat(o.tint[i]), kGlassGamma);   // display transmission -> linear (the blend runs in linear)
        k.sky[i] = sat(sky_ok ? sky[i] : kDefaultSky[i]);
    }
    k.refl = sat(o.refl); k.dirt = std::min(0.5f, std::max(0.0f, o.dirt)); k.f0 = kGlassF0;
    // SKIN-REFL
    const ReflBasis b0{};
    const bool rb = o.skin_refl && refl && finite_all(refl->x, 3) && finite_all(refl->y, 3) && finite_all(refl->z, 3);
    const ReflBasis& b = rb ? *refl : b0;
    for (int i = 0; i < 3; ++i) { k.rx[i] = b.x[i]; k.ry[i] = b.y[i]; k.rz[i] = b.z[i]; }
    k.refl_k = std::isfinite(o.skin_refl_k) ? std::min(4.0f, std::max(0.0f, o.skin_refl_k)) : 0.0f;
    if (rb) k.flags |= kCFlagRefl;
    // CFID
    if (o.fidelity) {
        k.flags |= kCFlagFid;
        if (shadow_box && finite_all(shadow_box, 4) && shadow_box[1] > 0.0f && shadow_box[3] > shadow_box[2]) {
            k.flags |= kCFlagShadow;
            for (int i = 0; i < 4; ++i) k.shb[i] = shadow_box[i];
            k.shs = kShadowSoftU;
        }
    }
    return k;
}
bool cockpit_shadow_box(const ModelData& m, const float eye[3], float out[4]) noexcept {
    if (!eye || !finite_all(eye, 3)) return false;
    const uint32_t skip = MF_HIDDEN | MF_EXTERIOR | MF_COCKPIT_DUP | MF_GLASS;
    auto each = [&](auto&& fn) {
        for (const Part& p : m.parts) {
            if ((p.mflags & skip) || !p.index_count || size_t(p.first_index) + p.index_count > m.indices.size()) continue;
            for (uint32_t k = 0; k < p.index_count; ++k) {
                const uint32_t vi = m.indices[p.first_index + k];
                if (vi < m.vertices.size()) fn(m.vertices[vi].pos);
            }
        }
    };
    bool gs = false; float zf = -1e30f;
    each([&](const float* q) { if (q[1] > eye[1] && q[2] < eye[2] && std::fabs(q[0]) < kShadowCentreX) { gs = true; zf = std::max(zf, q[2]); } });
    if (!gs) return false;
    const float zaft = eye[2] + kShadowAftU;
    bool side = false; float sill = -1e30f, hx = 0.0f, zb = -1e30f;
    each([&](const float* q) {
        zb = std::max(zb, q[2]);
        if (q[2] < zf || q[2] > zaft) return;
        hx = std::max(hx, std::fabs(q[0]));
        if (std::fabs(q[0]) >= kShadowCentreX && q[2] > zf && q[2] < eye[2]) { side = true; sill = std::max(sill, q[1]); }   // strictly aft of the glare shield's own edge
    });
    if (!side) return false;
    const float r[4] = {std::max(eye[1], sill), kShadowHalfFrac * hx, zf, zb};
    if (!finite_all(r, 4) || !(r[1] > 0.0f) || !(r[3] > r[2])) return false;
    for (int i = 0; i < 4; ++i) out[i] = r[i];
    return true;
}
float shadow_visibility(const float b[4], float soft, const float L[3], const float p[3]) noexcept {
    if (p[1] >= b[0] || std::fabs(p[0]) > b[1] || p[2] < b[2] || p[2] > b[3]) return 1.0f;
    if (!(L[1] > 1e-3f)) return 0.0f;
    const float t = (b[0] - p[1]) / L[1];
    const float cx = p[0] + t * L[0], cz = p[2] + t * L[2];
    const float d = std::min(b[1] - std::fabs(cx), std::min(cz - b[2], b[3] - cz));
    if (!(soft > 0.0f)) return d >= 0.0f ? 1.0f : 0.0f;
    const float x = sat((d + soft) / (2.0f * soft));
    return x * x * (3.0f - 2.0f * x);
}
float sun_term(const Constants& k, uint16_t mat, float nl, const float pm[3]) noexcept {
    float s;
    if ((k.flags & kCFlagFid) && (mat & kMatTexAlphaNotCov)) s = std::max(0.0f, 2.0f * nl * nl * nl);   // DrawCPT_v +356 == 0: 2 d^3, then max 0
    else s = sat(nl);
    if ((k.flags & kCFlagShadow) && !(mat & kMatTexAlphaNotCov) && pm) s *= shadow_visibility(k.shb, k.shs, k.light, pm);
    return s;
}
bool refl_basis(const float right[3], const float up[3], const float fwd[3], float yaw_sign, float yaw_offset_deg, ReflBasis& out) noexcept {
    if (!finite_all(right, 3) || !finite_all(up, 3) || !finite_all(fwd, 3) || !std::isfinite(yaw_sign) || !std::isfinite(yaw_offset_deg)) return false;
    // the game's +X / +Z in the stage frame: heading a of a horizontal baked direction d is atan2(-d.x, -d.z); stage heading = sign * a + offset
    // (terrain_stage.cpp build_env_texture). +X: a = -pi/2, +Z: a = pi. The map is orthogonal (a reflection when sign < 0), so n_game = M^T n_stage.
    const double kPi = 3.14159265358979323846, off = double(yaw_offset_deg) * kPi / 180.0, sg = yaw_sign < 0 ? -1.0 : 1.0;
    const double ax = sg * (-kPi / 2) + off, az = sg * kPi + off;
    const float gx[3] = {float(-std::sin(ax)), 0.0f, float(-std::cos(ax))}, gy[3] = {0.0f, 1.0f, 0.0f}, gz[3] = {float(-std::sin(az)), 0.0f, float(-std::cos(az))};
    ReflBasis r;
    if (!stage_light_in_model(gx, right, up, fwd, r.x) || !stage_light_in_model(gy, right, up, fwd, r.y) || !stage_light_in_model(gz, right, up, fwd, r.z)) return false;
    out = r;
    return true;
}
void refl_vertex(const Constants& k, const float n[3], float out[3]) noexcept {
    const float wx = n[0] * k.rx[0] + n[1] * k.rx[1] + n[2] * k.rx[2];
    const float wy = n[0] * k.ry[0] + n[1] * k.ry[1] + n[2] * k.ry[2];
    const float wz = n[0] * k.rz[0] + n[1] * k.rz[1] + n[2] * k.rz[2];
    out[0] = sat(wx * 0.5f + 0.5f); out[1] = sat(wz * 0.5f + 0.5f); out[2] = sat(kReflMaskGain * wy);
}
void shade_glass(const Constants& k, const float p[3], const float n[3], float add[3], float trans[3], float& out_a) noexcept {
    float v[3] = {k.eye[0] - p[0], k.eye[1] - p[1], k.eye[2] - p[2]};
    const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float c = 1.0f;
    if (vl > 1e-6f && nl > 1e-6f) c = std::fabs((v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) / (vl * nl));
    c = sat(c);
    const float x = 1.0f - c, x2 = x * x, x5 = x2 * x2 * x;
    const float F = k.f0 + (1.0f - k.f0) * x5;
    const float veil = sat(k.refl * F + k.dirt);
    float tsum = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float s = srgb_to_linear(k.sky[i]) * k.exposure;
        s = s > 0.0f ? s : 0.0f;
        s = k.tonemap == 1 ? s / (1.0f + s) : sat(s);
        add[i] = s * veil;
        trans[i] = k.trans[i] * (1.0f - veil);
        tsum += trans[i];
    }
    out_a = sat(1.0f - tsum / 3.0f);
}
bool stage_tint(const float am[3], const float lc[3], float out[3]) noexcept {
    if (!finite_all(am, 3) || !finite_all(lc, 3)) return false;
    float s[3], mean = 0;
    for (int i = 0; i < 3; ++i) { s[i] = std::max(am[i], 0.0f) + std::max(lc[i], 0.0f); mean += s[i] / 3.0f; }
    if (!(mean > 1e-4f)) return false;
    for (int i = 0; i < 3; ++i) out[i] = std::min(1.5f, std::max(0.5f, s[i] / mean));
    return true;
}
bool stage_light_in_model(const float l[3], const float right[3], const float up[3], const float fwd[3], float out[3]) noexcept {
    if (!finite_all(l, 3) || !finite_all(right, 3) || !finite_all(up, 3) || !finite_all(fwd, 3)) return false;
    const float r[3] = {l[0] * right[0] + l[1] * right[1] + l[2] * right[2], l[0] * up[0] + l[1] * up[1] + l[2] * up[2],
                        -(l[0] * fwd[0] + l[1] * fwd[1] + l[2] * fwd[2])};
    const float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(n > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) out[i] = r[i] / n;
    return true;
}
float srgb_to_linear(float x) noexcept {
    x = sat(x);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

// ------------------------------------------------------------------------------------------------------------------- reference
void sample_bilinear_wrap(const Texture& t, float u, float v, float out[4]) noexcept {
    const float x = u * float(t.w) - 0.5f, y = v * float(t.h) - 0.5f;
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    const float fx = x - fx0, fy = y - fy0;
    auto wrap = [](long long i, uint32_t n) { long long r = i % (long long)n; return uint32_t(r < 0 ? r + n : r); };
    const uint32_t x0 = wrap((long long)fx0, t.w), x1 = wrap((long long)fx0 + 1, t.w), y0 = wrap((long long)fy0, t.h), y1 = wrap((long long)fy0 + 1, t.h);
    auto px = [&](uint32_t xi, uint32_t yi, int c) { return float(t.rgba[(size_t(yi) * t.w + xi) * 4 + c]) / 255.0f; };
    for (int c = 0; c < 4; ++c)
        out[c] = (px(x0, y0, c) * (1 - fx) + px(x1, y0, c) * fx) * (1 - fy) + (px(x0, y1, c) * (1 - fx) + px(x1, y1, c) * fx) * fy;
}
void shade_fragment(const Mesh& mesh, const Constants& k, uint16_t mat, const float uv[2], const float vcol[4], const float n[3], float out[4],
    const Texture* refl_map, const float r[3], const float pm[3]) noexcept {
    float tex[4] = {1, 1, 1, 1};
    const unsigned slot = mat & 7u;
    if (slot < mesh.textures.size()) sample_bilinear_wrap(mesh.textures[slot], uv[0], uv[1], tex);
    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float d = nl > 1e-6f ? (n[0] * k.light[0] + n[1] * k.light[1] + n[2] * k.light[2]) / nl : 0.0f;
    const float lam = sun_term(k, mat, d, pm);   // CFID; without kCFlagFid/kCFlagShadow = saturate(n.L / |n|) as before
    const float li = std::min(k.ambient + (1.0f - k.ambient) * lam, kFidLightClamp) * k.gain;
    float a = 1.0f;
    if (k.pass == 1) a = sat(tex[3] * vcol[3]);
    // SKIN-REFL: the engine's second pass dst.rgb += dst.a * env.rgb * mask with dst.a = the skin texel alpha its first pass wrote (blend off), in the display domain
    float env[4] = {0, 0, 0, 0}, ew = 0.0f;
    if ((k.flags & kCFlagRefl) && k.pass == 0 && (mat & kMatTexAlphaNotCov) && refl_map && r && refl_map->w && refl_map->h) {
        sample_bilinear_wrap(*refl_map, r[0], r[1], env);
        ew = k.refl_k * tex[3] * r[2];
    }
    for (int c = 0; c < 3; ++c) {
        float v = tex[c] * vcol[c] * li * k.tint[c] + ew * env[c];
        if (k.flags & kCFlagDisplay) v = srgb_to_linear(v);   // COCKPIT-3: display colour -> linear once (the sRGB RTV encodes it back)
        v *= k.exposure;
        v = v > 0.0f ? v : 0.0f;
        v = k.tonemap == 1 ? v / (1.0f + v) : sat(v);
        out[c] = v * a;
    }
    out[3] = a;
}
namespace {
struct CV { float c[4]; float uv[2]; float col[4]; float n[3]; float p[3]; float r[3]; };
CV lerp_cv(const CV& a, const CV& b, float t) {
    CV r;
    for (int i = 0; i < 4; ++i) { r.c[i] = a.c[i] + (b.c[i] - a.c[i]) * t; r.col[i] = a.col[i] + (b.col[i] - a.col[i]) * t; }
    for (int i = 0; i < 2; ++i) r.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    for (int i = 0; i < 3; ++i) { r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * t; r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t; r.r[i] = a.r[i] + (b.r[i] - a.r[i]) * t; }
    return r;
}
}
void render_reference(const Mesh& mesh, const Options& o, float exposure, uint32_t tonemap, const EyeCam& cam, Target& t, const float* light_model,
    const float* tint, const float* sky, const ReflBasis* refl_basis, const Texture* refl_map) {
    Constants k = make_constants(cam, o, exposure, tonemap, light_model, tint, sky, refl_basis, mesh.shadow ? mesh.shadow_box : nullptr);
    if (!refl_map || !refl_map->w || !refl_map->h) k.flags &= ~kCFlagRefl;   // the backend's rule: no uploaded map = no term
    const float W = float(t.w), H = float(t.h);
    for (uint32_t pass = 0; pass < 3; ++pass) {   // 0 opaque, 1 blend, 2 canopy glass (COCKPIT-4)
        k.pass = pass;
        const uint32_t first = pass == 0 ? 0 : pass == 1 ? mesh.opaque_vertices : mesh.opaque_vertices + mesh.blend_vertices;
        const uint32_t count = pass == 0 ? mesh.opaque_vertices : pass == 1 ? mesh.blend_vertices : mesh.glass_vertices;
        for (uint32_t tri = 0; tri + 3 <= count; tri += 3) {
            CV in[3];
            const uint16_t mat = mesh.v[first + tri].mat;
            for (int j = 0; j < 3; ++j) {
                const GpuVertex& g = mesh.v[first + tri + j];
                const float p[4] = {g.pos[0], g.pos[1], g.pos[2], 1.0f};
                for (int r = 0; r < 4; ++r) in[j].c[r] = k.m[4 * r] * p[0] + k.m[4 * r + 1] * p[1] + k.m[4 * r + 2] * p[2] + k.m[4 * r + 3] * p[3];
                in[j].uv[0] = g.uv[0]; in[j].uv[1] = g.uv[1];
                for (int c = 0; c < 4; ++c) in[j].col[c] = float(g.rgba[c]) / 255.0f;
                for (int c = 0; c < 3; ++c) { in[j].n[c] = float(g.nrm[c]) / 32767.0f; in[j].p[c] = g.pos[c]; }
                refl_vertex(k, in[j].n, in[j].r);   // SKIN-REFL: per vertex like vs_model (and DrawCPT_v)
            }
            // clip against the near plane z <= w (reversed-Z: z = near), the only plane that can cut a visible triangle (x/y: guard band)
            CV poly[4]; int np = 0;
            for (int j = 0; j < 3; ++j) {
                const CV& a = in[j]; const CV& b = in[(j + 1) % 3];
                const float da = a.c[3] - a.c[2], db = b.c[3] - b.c[2];
                if (da >= 0) poly[np++] = a;
                if ((da >= 0) != (db >= 0)) poly[np++] = lerp_cv(a, b, da / (da - db));
            }
            if (np < 3) continue;
            float sx[4], sy[4], zn[4], iw[4];
            for (int j = 0; j < np; ++j) {
                iw[j] = 1.0f / poly[j].c[3];
                sx[j] = (poly[j].c[0] * iw[j] * 0.5f + 0.5f) * W;
                sy[j] = (0.5f - poly[j].c[1] * iw[j] * 0.5f) * H;
                zn[j] = poly[j].c[2] * iw[j];
            }
            for (int f = 1; f + 1 < np; ++f) {
                const int ix[3] = {0, f, f + 1};
                // screen y points down: a CCW (front) triangle in NDC has a NEGATIVE screen-space area here
                const float area = (sx[ix[1]] - sx[ix[0]]) * (sy[ix[2]] - sy[ix[0]]) - (sx[ix[2]] - sx[ix[0]]) * (sy[ix[1]] - sy[ix[0]]);
                if (!(area < 0.0f)) continue;
                int x0 = int(std::floor(std::min({sx[ix[0]], sx[ix[1]], sx[ix[2]]}))), x1 = int(std::ceil(std::max({sx[ix[0]], sx[ix[1]], sx[ix[2]]})));
                int y0 = int(std::floor(std::min({sy[ix[0]], sy[ix[1]], sy[ix[2]]}))), y1 = int(std::ceil(std::max({sy[ix[0]], sy[ix[1]], sy[ix[2]]})));
                x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, int(t.w) - 1); y1 = std::min(y1, int(t.h) - 1);
                for (int py = y0; py <= y1; ++py)
                    for (int px = x0; px <= x1; ++px) {
                        const float cx = float(px) + 0.5f, cy = float(py) + 0.5f;
                        const float l1 = ((cx - sx[ix[0]]) * (sy[ix[2]] - sy[ix[0]]) - (sx[ix[2]] - sx[ix[0]]) * (cy - sy[ix[0]])) / area;
                        const float l2 = ((sx[ix[1]] - sx[ix[0]]) * (cy - sy[ix[0]]) - (cx - sx[ix[0]]) * (sy[ix[1]] - sy[ix[0]])) / area;
                        const float l0 = 1.0f - l1 - l2;
                        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                        const float z = l0 * zn[ix[0]] + l1 * zn[ix[1]] + l2 * zn[ix[2]];
                        float& dz = t.depth[size_t(py) * t.w + px];
                        if (!(z > dz)) continue;
                        const float w0 = l0 * iw[ix[0]], w1 = l1 * iw[ix[1]], w2 = l2 * iw[ix[2]], ws = w0 + w1 + w2;
                        const CV& A = poly[ix[0]]; const CV& B = poly[ix[1]]; const CV& C = poly[ix[2]];
                        float uv[2], col[4], n[3], pm[3], rr[3];
                        for (int c = 0; c < 2; ++c) uv[c] = (w0 * A.uv[c] + w1 * B.uv[c] + w2 * C.uv[c]) / ws;
                        for (int c = 0; c < 4; ++c) col[c] = (w0 * A.col[c] + w1 * B.col[c] + w2 * C.col[c]) / ws;
                        for (int c = 0; c < 3; ++c) n[c] = (w0 * A.n[c] + w1 * B.n[c] + w2 * C.n[c]) / ws;
                        for (int c = 0; c < 3; ++c) rr[c] = (w0 * A.r[c] + w1 * B.r[c] + w2 * C.r[c]) / ws;
                        float* d = &t.rgba[(size_t(py) * t.w + px) * 4];
                        for (int c = 0; c < 3; ++c) pm[c] = (w0 * A.p[c] + w1 * B.p[c] + w2 * C.p[c]) / ws;
                        if (pass == 2) {   // COCKPIT-4 canopy glass: dst = add + trans * dst (dual-source), alpha = a + (1 - a) dst.a; no depth write
                            float add[3], tr[3], ga;
                            shade_glass(k, pm, n, add, tr, ga);
                            for (int c = 0; c < 3; ++c) d[c] = add[c] + tr[c] * d[c];
                            d[3] = ga + (1.0f - ga) * d[3];
                            continue;
                        }
                        if (pass == 0 && (mat & kMatAlphaTest) && (mat & 7u) < mesh.textures.size()) {
                            float tex[4]; sample_bilinear_wrap(mesh.textures[mat & 7u], uv[0], uv[1], tex);
                            if (tex[3] < k.cut) continue;
                        }
                        float s[4]; shade_fragment(mesh, k, mat, uv, col, n, s, refl_map, rr, pm);
                        for (int c = 0; c < 4; ++c) d[c] = s[c] + (1.0f - s[3]) * d[c];
                        if (pass == 0) dz = z;
                    }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------- synthetic
ModelData make_synthetic_model() {
    ModelData m;
    m.has_eye = true; m.eye[0] = 0.0f; m.eye[1] = 3.5f; m.eye[2] = -36.0f;
    // texture 0: 8x8 dark-grey/white checker with a transparent 2x2 hole in the middle (cockpit atlas: alpha = coverage)
    Texture t0; t0.w = 8; t0.h = 8; t0.rgba.resize(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
        uint8_t* p = &t0.rgba[(y * 8 + x) * 4];
        const bool red = ((x / 2) + (y / 2)) & 1;
        p[0] = p[1] = p[2] = red ? 50 : 255; p[3] = (x >= 3 && x <= 4 && y >= 3 && y <= 4) ? 0 : 255;
    }
    // texture 1: 4x4 flat grey skin whose alpha (77) is a reflection mask, NOT coverage
    Texture t1; t1.w = 4; t1.h = 4; t1.rgba.resize(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) { t1.rgba[i * 4] = 150; t1.rgba[i * 4 + 1] = 160; t1.rgba[i * 4 + 2] = 170; t1.rgba[i * 4 + 3] = 77; }
    m.textures = {t0, t1};
    auto add_quad = [&](const char* name, float cx, float cy, float cz, float hw, float hh, bool facing_eye, const uint8_t rgba[4], uint32_t mflags, int tex) {
        Part p;
        p.name_off = uint32_t(m.strings.size()); m.strings += name; m.strings.push_back('\0');
        p.node = uint32_t(m.parts.size()); p.mflags = mflags; p.texture = tex;
        for (int i = 0; i < 4; ++i) p.local_to_model[i * 5] = 1.0f;
        p.first_vertex = uint32_t(m.vertices.size()); p.vertex_count = 4;
        p.first_index = uint32_t(m.indices.size()); p.index_count = 6;
        const float zs = facing_eye ? 1.0f : -1.0f;   // facing the eye = normal +Z (the eye looks along -Z)
        const float xs[4] = {-hw, hw, hw, -hw}, ys[4] = {-hh, -hh, hh, hh};
        const float us[4] = {0, 1, 1, 0}, vs[4] = {1, 1, 0, 0};
        for (int i = 0; i < 4; ++i) {
            Vertex v{};
            v.pos[0] = cx + xs[i] * zs; v.pos[1] = cy + ys[i]; v.pos[2] = cz;   // mirrored in x when facing away: CCW seen from -Z
            v.uv[0] = facing_eye ? us[i] : 1.0f - us[i]; v.uv[1] = vs[i];
            v.nrm[0] = 0; v.nrm[1] = 0; v.nrm[2] = int16_t(32767 * zs);
            v.part = uint16_t(m.parts.size());
            std::memcpy(v.rgba, rgba, 4);
            m.vertices.push_back(v);
        }
        const uint32_t b = p.first_vertex;
        for (uint32_t ix : {b, b + 1, b + 2, b, b + 2, b + 3}) m.indices.push_back(ix);
        m.parts.push_back(p);
    };
    const uint8_t red[4] = {230, 30, 30, 255}, green[4] = {30, 200, 30, 255}, blue[4] = {40, 60, 230, 255}, yellow[4] = {230, 220, 30, 255};
    const uint8_t cyan[4] = {30, 210, 220, 255}, magenta[4] = {220, 30, 220, 255}, white[4] = {255, 255, 255, 255}, glass[4] = {200, 230, 255, 64};
    add_quad("left_red", -3.0f, 3.5f, -46.0f, 1.0f, 1.0f, true, red, MF_UNLIT, -1);                 // 0
    add_quad("right_green", 3.0f, 3.5f, -46.0f, 1.0f, 1.0f, true, green, 0, -1);                     // 1
    add_quad("up_blue", 0.0f, 6.5f, -46.0f, 2.0f, 0.3f, true, blue, 0, -1);                          // 2
    add_quad("far_cyan", 0.0f, 0.8f, -50.0f, 1.5f, 0.75f, true, cyan, 0, -1);                         // 3
    add_quad("near_yellow", 0.75f, 1.96f, -44.0f, 0.75f, 0.5f, true, yellow, 0, -1);                  // 4: overlaps far_cyan's left-centre from the eye
    add_quad("backface_magenta", -3.0f, 0.5f, -45.0f, 1.0f, 1.0f, false, magenta, 0, -1);            // 5: faces away (culled from the eye)
    add_quad("checker_holes", 3.0f, 0.5f, -45.0f, 1.0f, 1.0f, true, white, MF_ALPHA_TEST, 0);        // 6
    add_quad("hud_glass", 0.0f, 6.5f, -42.0f, 0.75f, 0.4f, true, glass, MF_BLEND | MF_GLASS, -1);   // 7
    add_quad("tail_exterior", 0.0f, 3.5f, -20.0f, 6.0f, 6.0f, false, white, MF_EXTERIOR | MF_TEX_ALPHA_NOT_COVERAGE, 1);  // 8: behind the eye, faces it
    add_quad("pilot_hidden", 0.0f, 3.5f, -37.0f, 3.0f, 3.0f, true, magenta, MF_HIDDEN | MF_COCKPIT_DUP | MF_EXTERIOR, 1); // 9: would cover the view
    return m;
}
ModelData make_synthetic_model_c4() {
    ModelData m = make_synthetic_model();
    m.flags |= kZvrmAnimRest;
    auto add_part = [&](const char* name, const float (*pos)[3], const float nrm[3], const uint8_t rgba[4], uint32_t mflags, int tex) -> Part& {
        Part p;
        p.name_off = uint32_t(m.strings.size()); m.strings += name; m.strings.push_back('\0');
        p.node = uint32_t(m.parts.size()); p.mflags = mflags; p.texture = tex;
        for (int i = 0; i < 4; ++i) p.local_to_model[i * 5] = 1.0f;
        p.first_vertex = uint32_t(m.vertices.size()); p.vertex_count = 4;
        p.first_index = uint32_t(m.indices.size()); p.index_count = 6;
        const float us[4] = {0, 1, 1, 0}, vs[4] = {1, 1, 0, 0};
        for (int i = 0; i < 4; ++i) {
            Vertex v{};
            for (int a = 0; a < 3; ++a) { v.pos[a] = pos[i][a]; v.nrm[a] = int16_t(std::lround(nrm[a] * 32767.0f)); }
            v.uv[0] = us[i]; v.uv[1] = vs[i]; v.part = uint16_t(m.parts.size()); std::memcpy(v.rgba, rgba, 4);
            m.vertices.push_back(v);
        }
        const uint32_t b = p.first_vertex;
        for (uint32_t ix : {b, b + 1, b + 2, b, b + 2, b + 3}) m.indices.push_back(ix);
        m.parts.push_back(p);
        return m.parts.back();
    };
    const uint8_t glass[4] = {200, 200, 200, 40}, plate[4] = {255, 255, 255, 255};
    {   // 10: canopy glass, z -40, x -2.5..2.5, y 1.5..6, CCW seen from the eye (normal +Z)
        const float q[4][3] = {{-2.5f, 1.5f, -40.0f}, {2.5f, 1.5f, -40.0f}, {2.5f, 6.0f, -40.0f}, {-2.5f, 6.0f, -40.0f}}, n[3] = {0, 0, 1};
        add_part("canopy_glass", q, n, glass, MF_BLEND | MF_GLASS, -1);
    }
    {   // 11: right elevator plate in y = 1 from the hinge (z -44) aft to z -43, CCW seen from above (normal +Y)
        const float q[4][3] = {{3.0f, 1.0f, -43.0f}, {5.0f, 1.0f, -43.0f}, {5.0f, 1.0f, -44.0f}, {3.0f, 1.0f, -44.0f}}, n[3] = {0, 1, 0};
        Part& p = add_part("elevator_r", q, n, plate, MF_EXTERIOR | MF_TEX_ALPHA_NOT_COVERAGE, 1);
        p.group = 2; p.anim = 0; p.hinge = 0.349f; p.rest = 0.0f; p.aflags = kPartHingeFitted;
        p.local_to_model[3] = 4.0f; p.local_to_model[7] = 1.0f; p.local_to_model[11] = -44.0f;
    }
    return m;
}

} // namespace acz::cockpit
