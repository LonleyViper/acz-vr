// fx_memory.cpp -- EFFECTS-OUTSIDE (B) particle memory core. See fx_memory.h and docs/world-render/effects-outside-window.md.
#include "fx_memory.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace acz::fxmem {
namespace {
double dot3(const double* a, const double* b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const double* a, const double* b, double* o) noexcept {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
bool norm3(double* a) noexcept {
    const double n = std::sqrt(dot3(a, a));
    if (!(n > 1e-300) || !std::isfinite(n)) return false;
    a[0] /= n; a[1] /= n; a[2] /= n; return true;
}
double dist3(const double* a, const double* b) noexcept {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return std::sqrt(dot3(d, d));
}
// Solve N x = r for the 3x3 N (rows = the A columns 0, 1, 3 over the xyz rows). Cramer; false when singular.
bool solve(const double* a, const double r[3], double x[3]) noexcept {
    const int col[3] = {0, 1, 3};
    double n[3][3];
    for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) n[i][k] = a[k * 4 + col[i]];
    const double det = n[0][0] * (n[1][1] * n[2][2] - n[1][2] * n[2][1]) - n[0][1] * (n[1][0] * n[2][2] - n[1][2] * n[2][0]) +
                       n[0][2] * (n[1][0] * n[2][1] - n[1][1] * n[2][0]);
    if (!(std::fabs(det) > 1e-300) || !std::isfinite(det)) return false;
    for (int k = 0; k < 3; ++k) {
        double m[3][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[i][j] = j == k ? r[i] : n[i][j];
        x[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
    return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}
} // namespace

bool screen_map_from_a(const double a16[16], ScreenMap& out, const char*& why) noexcept {
    out = ScreenMap{};
    why = nullptr;
    for (int i = 0; i < 16; ++i) { if (!std::isfinite(a16[i])) { why = "a-not-finite"; return false; } out.a[i] = a16[i]; }
    // depth column: a[r*4+2] = b * a[r*4+3] on the xyz rows, Z0 = a[14] - b a[15] ~ 2^24
    double bsum = 0, wsum = 0;
    for (int r = 0; r < 3; ++r) { bsum += out.a[r * 4 + 2] * out.a[r * 4 + 3]; wsum += out.a[r * 4 + 3] * out.a[r * 4 + 3]; }
    if (!(wsum > 1e-30)) { why = "a-w-column-zero"; return false; }
    const double b = bsum / wsum;
    double res = 0, ref = 0;
    for (int r = 0; r < 3; ++r) { const double e = out.a[r * 4 + 2] - b * out.a[r * 4 + 3]; res += e * e; ref += out.a[r * 4 + 2] * out.a[r * 4 + 2]; }
    if (res > 1e-8 * (ref + 1e-30)) { why = "a-depth-column-not-b-times-w"; return false; }
    const double z0 = out.a[14] - b * out.a[15];
    if (!depth_model_ok(b, z0)) { why = "a-depth-model"; return false; }   // per-view b / Z0 (cockpit 2^24; chase / HUD-only ~9e7): fx_memory.h
    out.b = b; out.z0 = z0;
    const double zero[3] = {-out.a[12], -out.a[13], -out.a[15]};
    if (!solve(out.a, zero, out.eye)) { why = "a-singular"; return false; }
    out.fwd[0] = out.a[3]; out.fwd[1] = out.a[7]; out.fwd[2] = out.a[11];
    if (!norm3(out.fwd)) { why = "a-w-column-zero"; return false; }
    out.ok = true;
    return true;
}
bool screen_map_from_a(const float a16[16], ScreenMap& out, const char*& why) noexcept {
    double d[16];
    for (int i = 0; i < 16; ++i) d[i] = double(a16[i]);
    return screen_map_from_a(d, out, why);
}
bool project(const ScreenMap& m, const double p[3], double& xs, double& ys, double& depth) noexcept {
    double h[4];
    for (int c = 0; c < 4; ++c) h[c] = p[0] * m.a[c] + p[1] * m.a[4 + c] + p[2] * m.a[8 + c] + m.a[12 + c];
    depth = h[3];
    if (!(h[3] > 1e-9)) return false;
    xs = h[0] / h[3]; ys = h[1] / h[3];
    return std::isfinite(xs) && std::isfinite(ys);
}
bool unproject(const ScreenMap& m, double xs, double ys, double depth, double p[3]) noexcept {
    if (!m.ok || !(depth > 0) || !std::isfinite(depth) || !std::isfinite(xs) || !std::isfinite(ys)) return false;
    const double r[3] = {xs * depth - m.a[12], ys * depth - m.a[13], depth - m.a[15]};
    return solve(m.a, r, p);
}
double vertex_depth(const ScreenMap& m, float z, float w, DepthSource& src) noexcept {
    src = depth_none;
    if (std::isfinite(z) && z > 0.0f) {
        const double d = m.z0 / (double(z) - m.b);
        if (d > 0 && std::isfinite(d)) { src = depth_z; return d; }
        return 0;
    }
    if (std::isfinite(w) && w > 0.0f && w < 1.0f) { src = depth_q; return 1.0 / double(w); }
    return 0;
}

size_t extract_sprites(const ScreenMap& m, const Vtx* v, size_t nv, const uint16_t* idx, size_t ni, uint8_t cls, std::vector<Sprite>& out,
                       ExtractStats& st) {
    const size_t before = out.size();
    if (!m.ok || !v) return 0;
    auto quad = [&](const uint32_t q[4]) {
        ++st.quads;
        double c[4][3]; double dsum = 0; DepthSource s0 = depth_none;
        for (int k = 0; k < 4; ++k) {
            if (q[k] >= nv) { ++st.bad_index; return; }
            const Vtx& x = v[q[k]];
            DepthSource s; const double d = vertex_depth(m, x.pos[2], x.pos[3], s);
            if (s == depth_none) { ++st.no_depth; return; }
            if (k == 0) s0 = s;
            if (!unproject(m, x.pos[0], x.pos[1], d, c[k])) { ++st.behind; return; }
            dsum += d;
        }
        Sprite sp;
        for (int i = 0; i < 3; ++i) sp.centre[i] = 0.25 * (c[0][i] + c[1][i] + c[2][i] + c[3][i]);
        sp.half_w = 0.25 * (dist3(c[1], c[0]) + dist3(c[3], c[2]));
        sp.half_h = 0.25 * (dist3(c[2], c[0]) + dist3(c[3], c[1]));
        if (!(sp.half_w > 0) || !(sp.half_h > 0) || !std::isfinite(sp.half_w) || !std::isfinite(sp.half_h)) { ++st.degenerate; return; }
        sp.depth = 0.25 * dsum; sp.depth_src = s0; sp.cls = cls;
        sp.uv0[0] = sp.uv0[1] = std::numeric_limits<float>::max(); sp.uv1[0] = sp.uv1[1] = -std::numeric_limits<float>::max();
        for (int k = 0; k < 4; ++k) {
            const Vtx& x = v[q[k]];
            for (int i = 0; i < 4; ++i) sp.colour[i] += 0.25f * x.col[i];
            for (int i = 0; i < 2; ++i) { sp.uv0[i] = std::min(sp.uv0[i], x.uv[i]); sp.uv1[i] = std::max(sp.uv1[i], x.uv[i]); }
        }
        out.push_back(sp); ++st.sprites;
    };
    std::vector<uint32_t> strip;
    auto flush = [&] {
        if (strip.empty()) return;
        if (strip.size() % 4 != 0) ++st.degenerate;
        else for (size_t i = 0; i + 4 <= strip.size(); i += 4) quad(&strip[i]);
        strip.clear();
    };
    if (idx) {
        for (size_t i = 0; i < ni; ++i) { if (idx[i] == 0xFFFFu) flush(); else strip.push_back(idx[i]); }
    } else {
        for (size_t i = 0; i < nv; ++i) strip.push_back(uint32_t(i));
    }
    flush();
    return out.size() - before;
}

double Memory::fade(const Particle& q, double t) const noexcept {
    if (t <= q.last_seen) return 1.0;
    if (!(p_.decay_s > 0)) return 0.0;
    return std::max(0.0, 1.0 - (t - q.last_seen) / p_.decay_s);
}
UpdateStats Memory::update(double t, const std::vector<Sprite>& seen, const ScreenMap& view) {
    UpdateStats u; u.seen = uint32_t(seen.size());
    std::vector<uint8_t> matched(list_.size(), 0);
    for (const Sprite& s : seen) {
        size_t best = list_.size(); double bestd = std::numeric_limits<double>::max();
        for (size_t i = 0; i < list_.size(); ++i) {
            if (matched[i] || list_[i].s.cls != s.cls) continue;
            const double r = p_.match_rel * std::max(std::max(s.half_w, s.half_h), std::max(list_[i].s.half_w, list_[i].s.half_h)) + p_.match_abs;
            const double d = dist3(s.centre, list_[i].s.centre);
            if (d <= r && d < bestd) { bestd = d; best = i; }
        }
        if (best < list_.size()) {
            Particle& q = list_[best]; q.s = s; q.last_seen = t; ++q.hits; matched[best] = 1; ++u.matched;
        } else {
            Particle q; q.id = next_id_++; q.s = s; q.first_seen = q.last_seen = t; q.hits = 1;
            list_.push_back(q); matched.push_back(1); ++u.born;
        }
    }
    std::vector<Particle> keep; keep.reserve(list_.size());
    for (size_t i = 0; i < list_.size(); ++i) {
        const Particle& q = list_[i];
        if (!matched[i]) {
            double xs = 0, ys = 0, d = 0;
            // Not seen although the game could see it (in front, inside the window): it died in the game (or was culled): drop.
            if (view.ok && project(view, q.s.centre, xs, ys, d) && in_window(xs, ys, p_.view_margin)) { ++u.died_in_view; continue; }
            if (t - q.last_seen >= p_.decay_s || t - q.first_seen >= p_.max_age_s) { ++u.expired; continue; }
            ++u.kept_outside;
        }
        keep.push_back(q);
    }
    if (keep.size() > p_.cap) {
        std::stable_sort(keep.begin(), keep.end(), [](const Particle& a, const Particle& b) { return a.last_seen > b.last_seen; });
        u.evicted = uint32_t(keep.size() - p_.cap);
        keep.resize(p_.cap);
    }
    list_.swap(keep);
    return u;
}

void billboard(const Particle& q, const double eye[3], double c[4][3]) noexcept {
    double dir[3] = {q.s.centre[0] - eye[0], q.s.centre[1] - eye[1], q.s.centre[2] - eye[2]};
    if (!norm3(dir)) { dir[0] = 0; dir[1] = 0; dir[2] = -1; }
    double up_w[3] = {0, 1, 0};
    if (std::fabs(dot3(dir, up_w)) > 0.999) { up_w[1] = 0; up_w[2] = 1; }
    double r[3], u[3];
    cross3(dir, up_w, r); norm3(r);
    cross3(r, dir, u); norm3(u);
    for (int i = 0; i < 3; ++i) {
        const double cr = r[i] * q.s.half_w, cu = u[i] * q.s.half_h;
        c[0][i] = q.s.centre[i] - cr + cu; c[1][i] = q.s.centre[i] + cr + cu;
        c[2][i] = q.s.centre[i] - cr - cu; c[3][i] = q.s.centre[i] + cr - cu;
    }
}

bool eye_project(const Eye& e, const double p[3], double& px, double& py) noexcept {
    const double cy = std::cos(e.yaw), sy = std::sin(e.yaw), cp = std::cos(e.pitch), sp = std::sin(e.pitch);
    const double f[3] = {-sy * cp, sp, -cy * cp}, r[3] = {cy, 0, -sy};
    double u[3]; cross3(r, f, u);
    const double v[3] = {p[0] - e.pos[0], p[1] - e.pos[1], p[2] - e.pos[2]};
    const double z = dot3(v, f);
    if (!(z > 1e-6)) return false;
    const double x = dot3(v, r) / (z * e.tan_half), y = dot3(v, u) / (z * e.tan_half);
    px = (x + 1.0) * 0.5 * e.size; py = (1.0 - y) * 0.5 * e.size;
    return std::isfinite(px) && std::isfinite(py);
}

void render_reference(const Memory& mem, double t, const Eye& e, std::vector<float>& rgba) {
    rgba.assign(size_t(e.size) * size_t(e.size) * 4, 0.0f);
    std::vector<const Particle*> order;
    for (const Particle& q : mem.particles()) if (mem.fade(q, t) > 0) order.push_back(&q);
    std::sort(order.begin(), order.end(), [&](const Particle* a, const Particle* b) { return dist3(a->s.centre, e.pos) > dist3(b->s.centre, e.pos); });
    for (const Particle* q : order) {
        double c[4][3], px[4], py[4];
        billboard(*q, e.pos, c);
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k) ok = eye_project(e, c[k], px[k], py[k]);
        if (!ok) continue;
        const float a = float(std::min(1.0, std::max(0.0, double(q->s.colour[3]) * mem.fade(*q, t))));
        const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2], px[3]})))), x1 = std::min(e.size - 1, int(std::ceil(std::max({px[0], px[1], px[2], px[3]}))));
        const int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2], py[3]})))), y1 = std::min(e.size - 1, int(std::ceil(std::max({py[0], py[1], py[2], py[3]}))));
        auto inside = [&](double x, double y, int i0, int i1, int i2) {
            auto ed = [&](int i, int j) { return (px[j] - px[i]) * (y - py[i]) - (py[j] - py[i]) * (x - px[i]); };
            const double e0 = ed(i0, i1), e1 = ed(i1, i2), e2 = ed(i2, i0);
            return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
        };
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
            const double sx = x + 0.5, sy = y + 0.5;
            if (!inside(sx, sy, 0, 1, 3) && !inside(sx, sy, 0, 3, 2)) continue;   // TL TR BR / TL BR BL
            float* d = &rgba[(size_t(y) * size_t(e.size) + size_t(x)) * 4];
            for (int i = 0; i < 3; ++i) d[i] = q->s.colour[i] * a + d[i] * (1.0f - a);
            d[3] = a + d[3] * (1.0f - a);
        }
    }
}

} // namespace acz::fxmem
