#include "world_props.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace acz::props {
namespace {
inline uint32_t rd32(const uint8_t* p) noexcept { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void wr32(uint8_t* p, uint32_t v) noexcept { std::memcpy(p, &v, 4); }
inline float sat(float x) noexcept { return x < 0 ? 0 : x > 1 ? 1 : x; }
bool num(const std::string& s, float& out) {
    if (s.empty() || s.size() > 32) return false;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) return false;
    out = v; return true;
}
} // namespace

const char* status_name(LoadStatus s) noexcept {
    switch (s) {
    case LoadStatus::ok: return "ok"; case LoadStatus::io_error: return "io-error"; case LoadStatus::too_small: return "too-small";
    case LoadStatus::too_large: return "too-large"; case LoadStatus::bad_magic: return "bad-magic"; case LoadStatus::bad_version: return "bad-version";
    case LoadStatus::bad_header: return "bad-header"; case LoadStatus::bad_size: return "bad-size"; case LoadStatus::bad_hash: return "bad-hash";
    case LoadStatus::bad_content: return "bad-content";
    }
    return "?";
}

LoadStatus parse_props(const uint8_t* d, size_t n, PropSet& out) noexcept {
    try {
        out = PropSet{};
        if (!d || n < kZvroHeader) return LoadStatus::too_small;
        if (n > kMaxFileBytes) return LoadStatus::too_large;
        if (std::memcmp(d, "ZVRO", 4) != 0) return LoadStatus::bad_magic;
        const uint32_t version = rd32(d + 4);
        if (version < kZvroMinVersion || version > kZvroVersion) return LoadStatus::bad_version;
        PropSet s;
        std::memcpy(s.id, d + 8, 8); s.id[8] = 0;
        if (!acz::terrain::valid_stage_id(std::string(s.id))) return LoadStatus::bad_header;
        s.flags = rd32(d + 16);
        const uint32_t nm = rd32(d + 20), nt = rd32(d + 24), ts = rd32(d + 28), nv = rd32(d + 32), ni = rd32(d + 36), ninst = rd32(d + 40);
        const uint32_t om = rd32(d + 44), ot = rd32(d + 48), ov = rd32(d + 52), oi = rd32(d + 56), oinst = rd32(d + 60), size = rd32(d + 64);
        if (version >= 2) {   // PERF v2: flag bit0 only; with it, bytes 68..83 = the stage sha prefix (any value), 84..95 zero; without it, 68..95 zero
            if (s.flags & ~kZvroFlagSnapped) return LoadStatus::bad_header;
            s.baked_snap = (s.flags & kZvroFlagSnapped) != 0;
            for (size_t k = s.baked_snap ? 68 + kZvroStageShaBytes : 68; k < 96; ++k) if (d[k]) return LoadStatus::bad_header;
            if (s.baked_snap) std::memcpy(s.stage_sha, d + 68, kZvroStageShaBytes);
        } else {
            for (size_t k = 68; k < 96; ++k) if (d[k]) return LoadStatus::bad_header;
        }
        if (size != n) return LoadStatus::bad_size;
        if (nm > kMaxModels || nt > kMaxTextures || ninst > kMaxInstances || (nt && (ts < 1 || ts > kMaxTexSize || (ts & (ts - 1))))) return LoadStatus::bad_header;
        // sections in order, contiguous, exactly sized
        const uint64_t e_m = uint64_t(om) + uint64_t(nm) * 32u, e_t = uint64_t(ot) + uint64_t(nt) * ts * ts * 4u;
        const uint64_t e_v = uint64_t(ov) + uint64_t(nv) * 32u, e_i = uint64_t(oi) + uint64_t(ni) * 4u, e_n = uint64_t(oinst) + uint64_t(ninst) * 24u;
        if (om != kZvroHeader || ot != e_m || ov != e_t || oi != e_v || oinst != e_i || e_n != n) return LoadStatus::bad_size;
        uint8_t sha[32];
        acz::terrain::sha256(d + kZvroHeader, n - kZvroHeader, sha);
        if (std::memcmp(sha, d + 96, 32) != 0) return LoadStatus::bad_hash;
        std::memcpy(s.payload_sha, sha, 32);
        s.ntex = nt; s.texsize = nt ? ts : 0;
        s.models.resize(nm); if (nm) std::memcpy(s.models.data(), d + om, size_t(nm) * 32u);
        s.tex.assign(d + ot, d + ot + size_t(e_t - ot));
        s.verts.resize(nv); if (nv) std::memcpy(s.verts.data(), d + ov, size_t(nv) * 32u);
        s.idx.resize(ni); if (ni) std::memcpy(s.idx.data(), d + oi, size_t(ni) * 4u);
        s.inst.resize(ninst); if (ninst) std::memcpy(s.inst.data(), d + oinst, size_t(ninst) * 24u);
        // REVIEW (overnight-loaders-review.md L-2): every DISTINCT (first_index, index_count, base_vertex) range is index-checked once, and the distinct ranges
        // may cover at most ni indices in total. objects_baker.py gives every model its own range; models sharing an identical range (a prop mesh reused
        // as a tree, as the tests do) cost nothing extra. Before, partially overlapping ranges made this check O(nm * ni) (minutes on the loader thread).
        struct Range { uint32_t first, count, base; };
        std::vector<Range> distinct;
        distinct.reserve(nm);
        for (const Model& m : s.models) {
            if (m.kind > kKindTree || uint64_t(m.first_index) + m.index_count > ni || m.index_count % 3) return LoadStatus::bad_content;
            if (!(std::isfinite(m.centre[0]) && std::isfinite(m.centre[1]) && std::isfinite(m.centre[2]) && std::isfinite(m.radius) && m.radius >= 0)) return LoadStatus::bad_content;
            if (m.index_count) distinct.push_back(Range{m.first_index, m.index_count, m.base_vertex});
        }
        std::sort(distinct.begin(), distinct.end(), [](const Range& a, const Range& b) {
            return a.first != b.first ? a.first < b.first : a.count != b.count ? a.count < b.count : a.base < b.base; });
        distinct.erase(std::unique(distinct.begin(), distinct.end(), [](const Range& a, const Range& b) {
            return a.first == b.first && a.count == b.count && a.base == b.base; }), distinct.end());
        uint64_t model_indices = 0;
        for (const Range& r : distinct) {
            model_indices += r.count;
            if (model_indices > ni) return LoadStatus::bad_content;
            for (uint32_t k = 0; k < r.count; ++k) if (uint64_t(r.base) + s.idx[r.first + k] >= nv) return LoadStatus::bad_content;
        }
        for (const Vertex& v : s.verts) {
            for (float f : v.pos) if (!std::isfinite(f)) return LoadStatus::bad_content;
            for (float f : v.uv) if (!std::isfinite(f)) return LoadStatus::bad_content;
            if (v.layer >= std::max(nt, 1u)) return LoadStatus::bad_content;
        }
        for (const Instance& i : s.inst) {
            if (i.model >= nm || (i.flags & ~kInstGroundRelative)) return LoadStatus::bad_content;
            if (s.baked_snap && (i.flags & kInstGroundRelative)) return LoadStatus::bad_content;   // a snapped bake has no ground-relative instance left
            if (!(std::isfinite(i.pos[0]) && std::isfinite(i.pos[1]) && std::isfinite(i.pos[2]) && std::isfinite(i.scale) && i.scale > 0)) return LoadStatus::bad_content;
        }
        out = std::move(s);
        return LoadStatus::ok;
    } catch (...) { out = PropSet{}; return LoadStatus::io_error; }
}

LoadStatus load_props_file(const std::string& path, PropSet& out) noexcept {
    try {
        if (path.empty()) return LoadStatus::io_error;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return LoadStatus::io_error;
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz < 0) { std::fclose(f); return LoadStatus::io_error; }
        if (size_t(sz) > kMaxFileBytes) { std::fclose(f); return LoadStatus::too_large; }
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        const size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (got != buf.size()) return LoadStatus::io_error;
        return parse_props(buf.data(), buf.size(), out);
    } catch (...) { return LoadStatus::io_error; }
}

std::vector<uint8_t> serialize_props(const PropSet& s) {
    const uint32_t nm = uint32_t(s.models.size()), nv = uint32_t(s.verts.size()), ni = uint32_t(s.idx.size()), ninst = uint32_t(s.inst.size());
    const uint32_t om = uint32_t(kZvroHeader), ot = om + nm * 32u, ov = ot + uint32_t(s.tex.size()), oi = ov + nv * 32u, oinst = oi + ni * 4u;
    const uint32_t size = oinst + ninst * 24u;
    std::vector<uint8_t> b(size, 0);
    std::memcpy(b.data(), "ZVRO", 4); wr32(&b[4], s.baked_snap ? 2u : 1u);
    std::memcpy(&b[8], s.id, std::min<size_t>(std::strlen(s.id), 8));
    wr32(&b[16], s.baked_snap ? kZvroFlagSnapped : s.flags); wr32(&b[20], nm);
    if (s.baked_snap) std::memcpy(&b[68], s.stage_sha, kZvroStageShaBytes); wr32(&b[24], s.ntex); wr32(&b[28], s.texsize); wr32(&b[32], nv); wr32(&b[36], ni); wr32(&b[40], ninst);
    wr32(&b[44], om); wr32(&b[48], ot); wr32(&b[52], ov); wr32(&b[56], oi); wr32(&b[60], oinst); wr32(&b[64], size);
    if (nm) std::memcpy(&b[om], s.models.data(), size_t(nm) * 32u);
    if (!s.tex.empty()) std::memcpy(&b[ot], s.tex.data(), s.tex.size());
    if (nv) std::memcpy(&b[ov], s.verts.data(), size_t(nv) * 32u);
    if (ni) std::memcpy(&b[oi], s.idx.data(), size_t(ni) * 4u);
    if (ninst) std::memcpy(&b[oinst], s.inst.data(), size_t(ninst) * 24u);
    acz::terrain::sha256(b.data() + kZvroHeader, b.size() - kZvroHeader, &b[96]);
    return b;
}

std::string props_path(const std::string& id, const std::string& dir_override) {
    std::string p = acz::terrain::stage_path(id, dir_override);
    if (p.size() < 5) return std::string();
    return p.substr(0, p.size() - 5) + ".zvro";
}

bool snap_to_terrain(PropSet& s, const acz::terrain::StageData& st) noexcept {
    if (s.baked_snap) {   // PERF: snapped at bake time; only the exact stage bake it was snapped against is accepted
        if (std::memcmp(s.stage_sha, st.payload_sha, kZvroStageShaBytes) != 0) return false;
        s.snapped = true;
        return true;
    }
    if (s.snapped) return true;
    if (st.heights.size() != size_t(acz::terrain::kHDim) * acz::terrain::kHDim) return false;
    for (Instance& i : s.inst)
        if (i.flags & kInstGroundRelative) { i.pos[1] += acz::terrain::height_bilinear(st, i.pos[0], i.pos[2]); i.flags &= ~kInstGroundRelative; }
    s.snapped = true;
    return true;
}
bool bake_snap(PropSet& s, const acz::terrain::StageData& st) noexcept {
    if (s.baked_snap) return std::memcmp(s.stage_sha, st.payload_sha, kZvroStageShaBytes) == 0;
    s.snapped = false;
    if (!snap_to_terrain(s, st)) return false;
    s.snapped = false;   // a freshly parsed copy is "snapped" only after snap_to_terrain checks the stage again
    s.baked_snap = true; s.flags = kZvroFlagSnapped;
    std::memcpy(s.stage_sha, st.payload_sha, kZvroStageShaBytes);
    return true;
}

bool extract_options(const std::string& value, std::string& rest, Options& opt, std::string& why) {
    opt = Options{};
    rest.clear();
    size_t pos = 0; bool first = true;
    while (pos <= value.size()) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        bool ours = false;
        if (!first) {
            const size_t eq = item.find_first_of(":=");
            if (eq != std::string::npos) {
                const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
                float f = 0; bool ok = true;
                auto flag = [&](bool& b) { if (v == "1") b = true; else if (v == "0") b = false; else return false; return true; };
                if (k == "props") { ours = true; ok = flag(opt.props); }
                else if (k == "trees") { ours = true; ok = flag(opt.trees); }
                else if (k == "treescale") { ours = true; ok = num(v, f) && f >= 0.1f && f <= 20.0f; opt.tree_scale = f; }
                else if (k == "treerange") { ours = true; ok = num(v, f) && f >= 100.0f && f <= 262144.0f; opt.tree_range = f; }
                else if (k == "proprange") { ours = true; ok = num(v, f) && f >= 100.0f && f <= 524288.0f; opt.prop_range = f; }
                else if (k == "propdedupe") { ours = true; ok = flag(opt.dedupe); }   // WORLD-4 (world_dedupe.h)
                else if (k == "dedupetol") { ours = true; ok = num(v, f) && f >= 0.01f && f <= 100.0f; opt.dedupe_tol = f; }
                else if (k == "propshade") { ours = true; ok = v == "game" || v == "w3"; opt.shade_game = v == "game"; }   // PROPS-SHADING
                else if (k == "propcap") { ours = true; ok = num(v, f) && f >= 1.0f && f <= 262144.0f && f == std::floor(f); opt.cap = uint32_t(f); }
                if (ours && !ok) { why = "bad value for " + k; return false; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}

namespace {
// Grid column/row of a world coordinate, clamped to [0, kGridDim - 1] BEFORE the float -> int conversion (REVIEW L-3: a finite but huge coordinate or
// reach, or a NaN, made int(floor(..)) undefined; in-range values give exactly the former int-then-clamp result).
inline int grid_index(float w) noexcept {
    const float f = std::floor((w + 262144.0f) / kGridCell);
    if (!(f >= 0.0f)) return 0;
    if (f > float(kGridDim - 1)) return kGridDim - 1;
    return int(f);
}
} // namespace

void build_grid(const PropSet& s, const Options& opt, Grid& g) {
    const size_t cells = size_t(kGridDim) * kGridDim;
    std::vector<uint32_t> cell_of(s.inst.size(), UINT32_MAX);   // UINT32_MAX = not entered (locator-only model)
    auto cell = [](float x, float z) { return uint32_t(grid_index(z) * kGridDim + grid_index(x)); };
    for (int kd = 0; kd < 2; ++kd) { g.start[kd].assign(cells + 1, 0); g.items[kd].clear(); g.max_reach[kd] = 0.0f; }
    for (size_t k = 0; k < s.inst.size(); ++k) {
        const Instance& in = s.inst[k];
        const Model& m = s.models[in.model];
        if (m.index_count == 0) continue;
        const int kd = m.kind == kKindTree ? 1 : 0;
        const float sc = in.scale * (kd ? opt.tree_scale : 1.0f);
        const float reach = std::sqrt(m.centre[0] * m.centre[0] + m.centre[2] * m.centre[2]) * sc + m.radius * sc;
        g.max_reach[kd] = std::max(g.max_reach[kd], reach);
        cell_of[k] = cell(in.pos[0], in.pos[2]);
        ++g.start[kd][cell_of[k] + 1];
    }
    std::vector<uint32_t> fill[2];
    for (int kd = 0; kd < 2; ++kd) {
        for (size_t c = 0; c < cells; ++c) g.start[kd][c + 1] += g.start[kd][c];
        g.items[kd].resize(g.start[kd][cells]);
        fill[kd].assign(g.start[kd].begin(), g.start[kd].end() - 1);
    }
    for (size_t k = 0; k < s.inst.size(); ++k) {
        if (cell_of[k] == UINT32_MAX) continue;
        const int kd = s.models[s.inst[k].model].kind == kKindTree ? 1 : 0;
        g.items[kd][fill[kd][cell_of[k]]++] = uint32_t(k);
    }
}

void select(const PropSet& s, const Grid& g, const Options& opt, const float eye[3], const float view[2][3], Selection& out) {
    out.inst.clear(); out.batches.clear();
    out.considered = out.culled_range = out.culled_behind = out.capped = 0;
    if (!opt.any() || g.start[0].size() != size_t(kGridDim) * kGridDim + 1 || g.start[1].size() != size_t(kGridDim) * kGridDim + 1) return;
    // REVIEW L-3: a non-finite eye would make every distance NaN and the nth_element/sort comparator below not a strict weak order (undefined behaviour)
    if (!(std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2]))) return;
    struct Cand { float d2; uint32_t idx; float pos[3]; float scale; };
    std::vector<Cand> cand;
    for (int kd = 0; kd < 2; ++kd) {
        const bool tree = kd == 1;
        if (tree ? !opt.trees : !opt.props) continue;      // a disabled kind is never scanned
        const std::vector<uint32_t>& st = g.start[kd];
        const std::vector<uint32_t>& items = g.items[kd];
        const float rmax = (tree ? opt.tree_range : opt.prop_range) + g.max_reach[kd];
        const int cx0 = grid_index(eye[0] - rmax), cx1 = grid_index(eye[0] + rmax);
        const int cz0 = grid_index(eye[2] - rmax), cz1 = grid_index(eye[2] + rmax);
        for (int cz = cz0; cz <= cz1; ++cz)
            for (int cx = cx0; cx <= cx1; ++cx) {
                const uint32_t c = uint32_t(cz * kGridDim + cx);
                for (uint32_t k = st[c]; k < st[c + 1]; ++k) {
                    const uint32_t ii = items[k];
                    const Instance& in = s.inst[ii];
                    const Model& m = s.models[in.model];
                    ++out.considered;
                    const float sc = in.scale * (tree ? opt.tree_scale : 1.0f);
                    const float cxw = in.pos[0] + m.centre[0] * sc, cyw = in.pos[1] + m.centre[1] * sc, czw = in.pos[2] + m.centre[2] * sc;
                    const float r = m.radius * sc;
                    const float dx = cxw - eye[0], dy = cyw - eye[1], dz = czw - eye[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    const float range = (tree ? opt.tree_range : opt.prop_range) + r;
                    if (!(d2 <= range * range)) { ++out.culled_range; continue; }   // REVIEW L-3: a NaN distance (0 * inf from an overflowing scale) is culled, never sorted
                    const float s0 = dx * view[0][0] + dy * view[0][1] + dz * view[0][2], s1 = dx * view[1][0] + dy * view[1][1] + dz * view[1][2];
                    if (s0 < -r && s1 < -r) { ++out.culled_behind; continue; }
                    cand.push_back(Cand{d2, ii, {in.pos[0], in.pos[1], in.pos[2]}, sc});
                }
            }
    }
    auto nearer = [](const Cand& a, const Cand& b) { return a.d2 < b.d2 || (a.d2 == b.d2 && a.idx < b.idx); };
    if (cand.size() > opt.cap) {
        std::nth_element(cand.begin(), cand.begin() + opt.cap, cand.end(), nearer);
        out.capped = uint32_t(cand.size() - opt.cap);
        cand.resize(opt.cap);
    }
    std::sort(cand.begin(), cand.end(), [&](const Cand& a, const Cand& b) {
        const uint32_t ma = s.inst[a.idx].model, mb = s.inst[b.idx].model;
        return ma < mb || (ma == mb && a.idx < b.idx);
    });
    out.inst.reserve(cand.size());
    for (const Cand& c : cand) {
        const uint32_t m = s.inst[c.idx].model;
        if (out.batches.empty() || out.batches.back().model != m) out.batches.push_back(Batch{m, uint32_t(out.inst.size()), 0});
        ++out.batches.back().count;
        out.inst.push_back(DrawInst{{c.pos[0], c.pos[1], c.pos[2]}, c.scale});
    }
}

bool make_clip(const float base[3], const float ux[3], const float vy[3], const float right[3], const float up[3], const float fwd[3], float m[16]) noexcept {
    // M = [ux | vy | base] (columns); K = M^-1 * B with B rows (right, up, -fwd).
    const double a[3][3] = {{ux[0], vy[0], base[0]}, {ux[1], vy[1], base[1]}, {ux[2], vy[2], base[2]}};
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return false;
    double inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det; inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det; inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det; inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det; inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det; inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det; inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    const double B[3][3] = {{right[0], right[1], right[2]}, {up[0], up[1], up[2]}, {-fwd[0], -fwd[1], -fwd[2]}};
    double K[3][3];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) K[r][c] = inv[r][0] * B[0][c] + inv[r][1] * B[1][c] + inv[r][2] * B[2][c];
    // rows: x = 2a - c, y = c - 2b, z = kNearC (constant), w = c  (row-major, column-vector: clip = M * (p_rel, 1))
    for (int c = 0; c < 3; ++c) {
        m[0 + c] = float(2.0 * K[0][c] - K[2][c]);
        m[4 + c] = float(K[2][c] - 2.0 * K[1][c]);
        m[8 + c] = 0.0f;
        m[12 + c] = float(K[2][c]);
    }
    m[3] = 0.0f; m[7] = 0.0f; m[11] = kNearC; m[15] = 0.0f;
    for (int k = 0; k < 16; ++k) if (!std::isfinite(m[k])) return false;
    return true;
}

bool project(const float m[16], const float p[3], float& u, float& v, float& c) noexcept {
    const float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    const float y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    const float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    c = w;
    if (!(w > 0.0f)) return false;
    u = 0.5f * (x / w + 1.0f);      // x = 2a - c  ->  x/w + 1 = 2a/c
    v = 0.5f * (1.0f - y / w);      // y = c - 2b  ->  1 - y/w = 2b/c
    return true;
}

bool prop_visible(float tp, float far_dist, bool terrain_hit, float t_hit) noexcept {
    return tp > 0.0f && tp < far_dist && (!terrain_hit || tp < t_hit);
}

void prop_color(const float albedo[3], const float n_in[3], const float view_dir[3], float tp, float haze_l, float far_dist, float out[3]) noexcept {
    using namespace acz::terrain;
    float n[3] = {n_in[0], n_in[1], n_in[2]};
    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl > 1e-12f) for (float& x : n) x /= nl;
    if (n[0] * view_dir[0] + n[1] * view_dir[1] + n[2] * view_dir[2] > 0.0f) for (float& x : n) x = -x;   // face the eye (the pass does this)
    const float sl = std::sqrt(kSun[0] * kSun[0] + kSun[1] * kSun[1] + kSun[2] * kSun[2]);
    const float lam = std::max((n[0] * kSun[0] + n[1] * kSun[1] + n[2] * kSun[2]) / sl, 0.0f);
    const float f = haze_fraction(tp, haze_l, far_dist);
    for (int c = 0; c < 3; ++c) {
        const float col = sat(albedo[c] * (kAmbient + kDiffuse * lam));
        out[c] = srgb_to_linear(col + (kHaze[c] - col) * f);
    }
}

float prop_fog(float tp) noexcept {
    const float f = tp / kPropFogEnd;
    return f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f;
}

void prop_color_game(const float albedo[3], float tp, float out[3]) noexcept {
    using namespace acz::terrain;
    const float f = prop_fog(tp);
    for (int c = 0; c < 3; ++c) {
        const float col = sat(albedo[c]);
        out[c] = srgb_to_linear(col + (kHaze[c] - col) * f);
    }
}

PropSet make_box_set(float size, const uint8_t rgba[4], const std::vector<Instance>& inst, uint32_t texsize) {
    PropSet s;
    std::memcpy(s.id, "synth", 5);
    s.ntex = 1; s.texsize = texsize;
    s.tex.resize(size_t(texsize) * texsize * 4);
    for (size_t k = 0; k < size_t(texsize) * texsize; ++k) std::memcpy(&s.tex[k * 4], rgba, 4);
    const float h = size * 0.5f;
    // 6 faces of a box from y = 0 to y = size (base on the instance point); corners in cyclic order, winding fixed to CCW seen from outside.
    const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        const float* n = N[f];
        const int ax = n[0] != 0 ? 0 : n[1] != 0 ? 1 : 2, a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        const float sgn[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        float P[4][3];
        const uint32_t b = uint32_t(s.verts.size());
        for (int k = 0; k < 4; ++k) {
            Vertex v{};
            v.pos[ax] = n[ax] * h; v.pos[a1] = sgn[k][0] * h; v.pos[a2] = sgn[k][1] * h;
            v.pos[1] += h;
            for (int c = 0; c < 3; ++c) P[k][c] = v.pos[c];
            v.uv[0] = 0.5f * (sgn[k][0] + 1); v.uv[1] = 0.5f * (sgn[k][1] + 1);
            for (int c = 0; c < 3; ++c) v.nrm[c] = int16_t(n[c] * 32767.0f);
            v.layer = 0; v.rgba[0] = v.rgba[1] = v.rgba[2] = v.rgba[3] = 255;
            s.verts.push_back(v);
        }
        const float e1[3] = {P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]}, e2[3] = {P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]};
        const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const bool ccw = cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] > 0;
        const uint32_t q[6] = {0, 1, 2, 0, 2, 3}, qr[6] = {0, 2, 1, 0, 3, 2};
        for (int k = 0; k < 6; ++k) s.idx.push_back(b + (ccw ? q[k] : qr[k]));   // relative to the model's base_vertex (0)
    }
    s.models.push_back(Model{0, uint32_t(s.idx.size()), 0, kKindProp, {0.0f, h, 0.0f}, h * 1.7320508f});
    s.inst = inst;
    return s;
}

std::vector<float> prop_placements(const PropSet& s) {
    std::vector<float> out;
    for (const Instance& i : s.inst) {
        if (i.model >= s.models.size() || s.models[i.model].kind != kKindProp) continue;
        out.push_back(i.pos[0]); out.push_back(i.pos[1]); out.push_back(i.pos[2]);
    }
    return out;
}

} // namespace acz::props
