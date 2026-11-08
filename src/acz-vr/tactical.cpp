// tactical.cpp -- TACTICAL display (tac:1): tracker, radar mapping, firing debounce, cosmetic tracers, overlay geometry. See tactical.h / tactical.md.
#include "tactical.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace acz::tac {
namespace {
bool num(const std::string& v, float& f) {
    if (v.empty()) return false;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(d)) return false;
    f = float(d);
    return true;
}
float dot3(const float a[3], const float b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float len3(const float a[3]) noexcept { return std::sqrt(dot3(a, a)); }
bool finite3(const float a[3]) noexcept { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }
void cross3(const float a[3], const float b[3], float o[3]) noexcept {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
bool norm3(float a[3]) noexcept {
    const float l = len3(a);
    if (!(l > 1e-9f) || !std::isfinite(l)) return false;
    for (int i = 0; i < 3; ++i) a[i] /= l;
    return true;
}
} // namespace

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
                if (k == "tac") { ours = true; ok = flag(opt.on); }
                else if (k == "tacradar") { ours = true; ok = flag(opt.radar); }
                else if (k == "tacboxes" || k == "tacmark") { ours = true; ok = flag(opt.markers); }
                else if (k == "tacgun") { ours = true; ok = flag(opt.tracers); }
                else if (k == "tacsup") { ours = true; ok = flag(opt.suppress); }
                else if (k == "tacr") { ours = true; ok = num(v, f) && f >= 500.0f && f <= 1000000.0f; opt.range_ring3 = f; }
                else if (k == "tacself") { ours = true; ok = num(v, f) && f >= 0.0f && f <= 2000.0f; opt.self_radius = f; }
                else if (k == "tacclu") { ours = true; ok = num(v, f) && f >= 1.0f && f <= 2000.0f; opt.cluster = f; }
                else if (k == "mk360") { ours = true; ok = v == "0" || v == "1" || v == "2" || v == "3"; if (ok) opt.mk360 = uint8_t(v[0] - '0'); }
                else if (k == "mk360min") { ours = true; ok = num(v, f) && f >= 0.0f && f <= 100000.0f; opt.mk360_near_min = f; }
                else if (k == "mk360ref") { ours = true; ok = num(v, f) && f >= 0.0f && f <= 1000000.0f; opt.mk360_near_ref = f; }
                else if (k == "mk360floor") { ours = true; ok = num(v, f) && f >= 0.05f && f <= 1.0f; opt.mk360_near_floor = f; }
                else if (k == "mk360fade") { ours = true; ok = num(v, f) && f >= 0.0f && f <= 100000.0f; opt.mk360_near_fade = f; }
                else if (k == "mk360u") { ours = true; ok = flag(opt.mk360_unknown); }
                else if (k == "mk360lock") { ours = true; ok = flag(opt.mk360_lock); }
                else if (k == "mk360p") { ours = true; ok = flag(opt.mk360_prov); }
                else if (k == "mk360pu") { ours = true; ok = v == "0" || v == "1"; if (ok) opt.mk360_prov_unknown = uint8_t(v[0] - '0'); }
                else if (k == "mk360cen") { ours = true; ok = flag(opt.mk360_census); }
                else if (k == "mk360ld") { ours = true; ok = flag(opt.mk360_lockdump); }
                if (ours && !ok) { why = "bad value for " + k; return false; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}
std::string describe(const Options& o) {
    char t[256];
    std::snprintf(t, sizeof t, "tac=%d;radar=%d;markers=%d;tracers=%d;suppress=%d;range-ring3=%.0f(guess);self-radius=%.0f;cluster=%.0f",
                  o.on ? 1 : 0, o.radar ? 1 : 0, o.markers ? 1 : 0, o.tracers ? 1 : 0, o.suppress ? 1 : 0, double(o.range_ring3), double(o.self_radius), double(o.cluster));
    std::string s = t;
    if (o.mk360) s += std::string(";mk360=") + char('0' + o.mk360) + (o.mk360_unknown ? ";mk360u=1" : "");   // mk360 off: the line is unchanged
    if (o.mk360 == 3 && (o.mk360_near_min != 150.0f || o.mk360_near_ref != 750.0f || o.mk360_near_floor != 0.3f)) {   // defaults: the line is unchanged
        char n[96]; std::snprintf(n, sizeof n, ";mk360min=%.0f;mk360ref=%.0f;mk360floor=%.2f", double(o.mk360_near_min), double(o.mk360_near_ref), double(o.mk360_near_floor));
        s += n;
    }
    if (o.mk360 == 3 && o.mk360_near_fade != 75.0f) {   // NEAR-FADE (markers-360.md section 16): default 75 leaves the line unchanged
        char n[40]; std::snprintf(n, sizeof n, ";mk360fade=%.0f", double(o.mk360_near_fade));
        s += n;
    }
    if (o.mk360 == 3 && !o.mk360_lock) s += ";mk360lock=0";   // LOCK-SHAPE (section 17.4): the default (on) leaves the line unchanged
    if (o.mk360 && o.mk360_census) s += ";mk360cen=1";
    if (o.mk360 && o.mk360_lockdump) s += ";mk360ld=1";
    if (o.mk360 && o.mk360_prov) s += ";mk360p=1";   // PROV is opt-in since headset test 9 (section 22): the default (off) leaves the line unchanged
    if (o.mk360 && o.mk360_prov && o.mk360_prov_unknown != 1) s += ";mk360pu=0";   // IFF unknown policy (section 20): default (neutral) leaves the line unchanged
    return s;
}
uint32_t producer_flags_for(const Options& o) noexcept {
    // MARKERS-360 replaces tacboxes (never both sets). NBOX mk360:3 never hides a game draw: the game's own box/label pixels are cut from the marker image.
    // LOCK-SHAPE (mk360:3, mk360lock not 0) moves the lock diamond from the HUD into the marker image; mk360cen:1 = the one-shot census (any mk360 mode).
    if (o.mk360) return kProducerOn | (o.mk360 == 1 ? kProducerHideAll : 0u) | (o.mk360 == 3 && o.mk360_lock ? kProducerLockShape : 0u) |
                        (o.mk360_census ? kProducerLockCensus : 0u) | (o.mk360_lockdump ? kProducerLockDump : 0u);
    if (!o.on) return 0u;
    return kProducerOn | (o.markers && o.suppress ? kProducerSuppress : 0u);
}

Team team_of(const float rgb[3]) noexcept {
    const float r = rgb[0], g = rgb[1], b = rgb[2];
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) return team_unknown;
    const float mx = std::fmax(r, std::fmax(g, b)), mn = std::fmin(r, std::fmin(g, b));
    if (!(mx > 0.0f) || (mx - mn) < 0.2f * mx) return team_unknown;   // grey / white: no colour
    if (b >= r * 1.25f && b >= g * 1.05f) return team_friend;          // blue-dominant (FRND)
    return team_enemy;
}
Label label_of(uint8_t glyphs, Team t) noexcept {
    if (glyphs == 4) return label_frnd;
    if (glyphs == 3) return label_tgt;
    return t == team_friend ? label_frnd : label_none;
}
float Track::speed() const noexcept { return vel_valid ? len3(vel) : 0.0f; }

void tracker_update(Tracker& tk, const TacFrame& f, const float cam[3], double t, const Options& o) {
    ++tk.frames;
    struct C { float sum[3] = {}; uint32_t n = 0; float pos[3] = {}; int mark = -1; };
    C cl[kObjCap];
    unsigned nc = 0;
    const unsigned nobj = std::min<unsigned>(f.nobj, kObjCap);
    for (unsigned i = 0; i < nobj; ++i) {
        const ObjDraw& d = f.obj[i];
        if (!finite3(d.t)) continue;
        if (cam && finite3(cam)) {
            const float dd[3] = {d.t[0] - cam[0], d.t[1] - cam[1], d.t[2] - cam[2]};
            if (len3(dd) < o.self_radius) { ++tk.self_draws; continue; }
        }
        int j = -1;
        for (unsigned k = 0; k < nc; ++k) {
            const float c[3] = {cl[k].sum[0] / float(cl[k].n), cl[k].sum[1] / float(cl[k].n), cl[k].sum[2] / float(cl[k].n)};
            if (std::fabs(c[0] - d.t[0]) <= o.cluster && std::fabs(c[1] - d.t[1]) <= o.cluster && std::fabs(c[2] - d.t[2]) <= o.cluster) { j = int(k); break; }
        }
        if (j < 0) { if (nc >= kObjCap) continue; j = int(nc++); cl[j] = C{}; }
        for (int k = 0; k < 3; ++k) cl[j].sum[k] += d.t[k];
        ++cl[j].n;
        if (d.marker && d.marker <= f.nmark && cl[j].mark < 0) cl[j].mark = int(d.marker) - 1;
    }
    for (unsigned k = 0; k < nc; ++k) for (int i = 0; i < 3; ++i) cl[k].pos[i] = cl[k].sum[i] / float(cl[k].n);
    tk.clusters += nc;
    // association: greedy nearest pairs within a gate around each track's prediction
    struct P { float d; unsigned tr, c; };
    std::vector<P> pairs;
    for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        const Track& tr = tk.tracks[a];
        const double dt = std::min(2.0, std::max(0.0, t - tr.t_seen));
        float pred[3];
        for (int i = 0; i < 3; ++i) pred[i] = tr.pos[i] + (tr.vel_valid ? tr.vel[i] * float(dt) : 0.0f);
        const float gate = 2.0f * o.cluster + 30.0f + (tr.vel_valid ? 0.5f * tr.speed() * float(std::min(dt, 0.5)) : 0.0f);
        for (unsigned c = 0; c < nc; ++c) {
            const float dv[3] = {cl[c].pos[0] - pred[0], cl[c].pos[1] - pred[1], cl[c].pos[2] - pred[2]};
            const float d = len3(dv);
            if (d <= gate) pairs.push_back({d, a, c});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const P& x, const P& y) { return x.d < y.d || (x.d == y.d && (x.tr < y.tr || (x.tr == y.tr && x.c < y.c))); });
    std::vector<int> c_of_track(tk.tracks.size(), -1), track_of_c(nc, -1);
    for (const P& p : pairs) if (c_of_track[p.tr] < 0 && track_of_c[p.c] < 0) { c_of_track[p.tr] = int(p.c); track_of_c[p.c] = int(p.tr); }
    auto apply_marker = [&](Track& tr, const C& c) {
        tr.marker_now = false; tr.game_marker_now = false;
        if (c.mark < 0) return;
        const MarkerSprite& m = f.mark[c.mark];
        tr.marker_now = true; tr.game_marker_now = !m.suppressed;
        const Team team = team_of(m.rgb);
        if (team != team_unknown) {
            if (tr.team == team_unknown) ++tk.teamed;
            tr.team = team; for (int i = 0; i < 3; ++i) tr.rgb[i] = m.rgb[i];
            tr.label = label_of(m.glyphs, team);
        }
        ++tk.matched;
    };
    for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        Track& tr = tk.tracks[a];
        const int c = c_of_track[a];
        if (c < 0) { tr.seen_now = false; tr.marker_now = false; tr.game_marker_now = false; continue; }
        const double dt = t - tr.t_seen;
        if (dt > 1e-3) {
            float inst[3];
            for (int i = 0; i < 3; ++i) inst[i] = float((cl[c].pos[i] - tr.pos[i]) / dt);
            if (finite3(inst)) {
                if (tr.vel_valid) for (int i = 0; i < 3; ++i) tr.vel[i] = 0.6f * tr.vel[i] + 0.4f * inst[i];
                else { for (int i = 0; i < 3; ++i) tr.vel[i] = inst[i]; tr.vel_valid = true; }
            }
        }
        for (int i = 0; i < 3; ++i) tr.pos[i] = cl[c].pos[i];
        tr.t_seen = t; ++tr.frames; tr.seen_now = true;
        apply_marker(tr, cl[c]);
    }
    for (unsigned c = 0; c < nc; ++c) {
        if (track_of_c[c] >= 0) continue;
        Track tr;
        tr.id = tk.next_id++;
        for (int i = 0; i < 3; ++i) tr.pos[i] = cl[c].pos[i];
        tr.t_seen = tr.t_first = t; tr.frames = 1; tr.seen_now = true;
        apply_marker(tr, cl[c]);
        tk.tracks.push_back(tr);
        ++tk.created;
    }
    const size_t before = tk.tracks.size();
    tk.tracks.erase(std::remove_if(tk.tracks.begin(), tk.tracks.end(), [&](const Track& tr) {
        return t - tr.t_seen > (tr.team != team_unknown ? kCoastTeamed : kCoastUnknown);
    }), tk.tracks.end());
    tk.dropped += before - tk.tracks.size();
}

bool heading_frame(const float fwd[3], const float up[3], float fh[3], float rh[3]) noexcept {
    fh[0] = fwd[0]; fh[1] = 0.0f; fh[2] = fwd[2];
    if (std::sqrt(fh[0] * fh[0] + fh[2] * fh[2]) < 0.2f) {   // near vertical: the nose direction over the ground is where the camera "up" leans away from
        const float s = fwd[1] > 0.0f ? -1.0f : 1.0f;
        fh[0] = s * up[0]; fh[2] = s * up[2];
    }
    if (!norm3(fh)) return false;
    const float worldUp[3] = {0, 1, 0};
    cross3(fh, worldUp, rh);   // right = forward x up (right-handed, Y up: fwd -Z -> right +X)
    return norm3(rh);
}
bool radar_uv(const float rel[3], const float fh[3], const float rh[3], const Options& o, const RadarGeom& g, float aspect, float& u, float& v) noexcept {
    const float k = g.ring[2] / o.range_ring3;   // image heights per world unit
    const float du = dot3(rel, rh) * k * aspect, dv = -dot3(rel, fh) * k;
    u = g.cu + du; v = g.cv + dv;
    if (std::fabs(du) <= g.half_u && std::fabs(dv) <= g.half_v) return true;
    const float s = 0.96f * std::fmin(std::fabs(du) > 0 ? g.half_u / std::fabs(du) : 1e30f, std::fabs(dv) > 0 ? g.half_v / std::fabs(dv) : 1e30f);
    u = g.cu + du * s; v = g.cv + dv * s;
    return false;
}
void radar_blips(const Tracker& tk, const float player[3], const float fh[3], const float rh[3], double t, const Options& o, const RadarGeom& g,
                 float aspect, std::vector<Blip>& out) {
    out.clear();
    for (const Track& tr : tk.tracks) {
        const bool moving = tr.vel_valid && tr.speed() >= kMovingSpeed;
        if (tr.team == team_unknown && !moving) continue;   // static unclassified: buildings/props, not shown
        if (tr.team == team_unknown && !tr.seen_now) continue;
        Blip b;
        b.id = tr.id; b.coasting = !tr.seen_now;
        const float age = float(std::min(kCoastTeamed, std::max(0.0, t - tr.t_seen)));
        float p[3];
        for (int i = 0; i < 3; ++i) p[i] = tr.pos[i] + (b.coasting && tr.vel_valid ? tr.vel[i] * age : 0.0f);
        const float rel[3] = {p[0] - player[0], p[1] - player[1], p[2] - player[2]};
        b.edge = !radar_uv(rel, fh, rh, o, g, aspect, b.u, b.v);
        b.range = std::sqrt(rel[0] * rel[0] + rel[2] * rel[2]);
        b.bearing = std::atan2(dot3(rel, rh), dot3(rel, fh));
        if (moving) { b.arrow = true; b.dir = std::atan2(dot3(tr.vel, rh), dot3(tr.vel, fh)); }
        if (b.edge) b.dir = std::atan2((b.u - g.cu) / aspect, -(b.v - g.cv));
        if (tr.team == team_unknown) b.rgb[0] = b.rgb[1] = b.rgb[2] = 1.0f;
        else for (int i = 0; i < 3; ++i) b.rgb[i] = tr.rgb[i];
        out.push_back(b);
    }
}

bool FiringDetector::update(bool wedge, double t) noexcept {
    if (wedge) {
        gap = 0; gap_t0 = -1;
        if (run == 0) run_t0 = t;
        ++run;
        if (!on && run >= kOnFrames && t - run_t0 >= kOnSeconds) { on = true; ++on_events; }
    } else {
        run = 0; run_t0 = -1;
        if (on) {
            if (gap == 0) gap_t0 = t;
            ++gap;
            if (gap >= kOffFrames && t - gap_t0 >= kOffSeconds) { on = false; gap = 0; gap_t0 = -1; }
        }
    }
    return on;
}

void gun_positions(const float cam[3], const float right[3], const float up[3], const float fwd[3], float gl[3], float gr[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        const float base = cam[i] - 5.0f * up[i] + 10.0f * fwd[i];
        gl[i] = base - 6.0f * right[i];
        gr[i] = base + 6.0f * right[i];
    }
}
void TracerSim::update(bool firing, double t, const float gl[3], const float gr[3], const float fwd[3], const float vel[3]) {
    rounds.erase(std::remove_if(rounds.begin(), rounds.end(), [&](const Round& r) { return t - r.born > kLife || t < r.born - 1.0; }), rounds.end());
    if (!firing || !finite3(gl) || !finite3(gr) || !finite3(fwd) || !finite3(vel)) { next = 0; return; }
    if (next <= 0 || next < t - 3 * kInterval) next = t;
    unsigned guard = 0;
    while (next <= t && guard++ < 16) {
        const float back = float(t - next);   // spawned `back` seconds ago at the muzzle the aircraft has since moved away from
        for (const float* g : {gl, gr}) {
            if (rounds.size() >= 256) break;
            Round r;
            for (int i = 0; i < 3; ++i) { r.v[i] = vel[i] + fwd[i] * kMuzzle; r.p0[i] = g[i] - vel[i] * back; }
            r.born = next;
            rounds.push_back(r);
            ++spawned;
        }
        next += kInterval;
    }
}
bool TracerSim::streak(const Round& r, double t, float a[3], float b[3], float& alpha) noexcept {
    const double age = t - r.born;
    if (age < 0 || age > kLife) return false;
    const double a0 = std::max(0.0, age - double(kStreak));
    for (int i = 0; i < 3; ++i) { b[i] = float(r.p0[i] + r.v[i] * age); a[i] = float(r.p0[i] + r.v[i] * a0); }
    b[1] -= float(0.5 * kGravity * age * age); a[1] -= float(0.5 * kGravity * a0 * a0);
    alpha = float(1.0 - age / kLife);
    return true;
}

// ------------------------------------------------------------------------------------------------ overlay
float srgb_to_linear(float c) noexcept {
    c = std::fmin(1.0f, std::fmax(0.0f, c));
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
namespace {
void colour(const float rgb[3], float a, float out[4]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = srgb_to_linear(rgb[i]) * a;
    out[3] = a;
}
bool room(const Overlay& o, size_t n) noexcept { return o.v.size() + n <= kOverlayCap; }
void tri(Overlay& o, const float a[4], const float b[4], const float c[4], const float col[4]) {
    if (!room(o, 3)) return;
    for (const float* p : {a, b, c}) { Vertex v; for (int i = 0; i < 4; ++i) { v.pos[i] = p[i]; v.col[i] = col[i]; } o.v.push_back(v); }
}
bool clip_of(const EyeView& e, const float p[3], float out[4]) noexcept {
    const float q[3] = {p[0] - e.eye[0], p[1] - e.eye[1], p[2] - e.eye[2]};
    const float* m = e.clip;
    out[0] = m[0] * q[0] + m[1] * q[1] + m[2] * q[2] + m[3];
    out[1] = m[4] * q[0] + m[5] * q[1] + m[6] * q[2] + m[7];
    out[2] = 0.0f;
    out[3] = m[12] * q[0] + m[13] * q[1] + m[14] * q[2] + m[15];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[3]) && out[3] > 1e-4f;
}
// A world quad (4 corners in order) projected; dropped unless every corner is in front of the eye.
bool world_quad(Overlay& o, const EyeView& e, const float w[4][3], const float col[4]) {
    float c[4][4];
    for (int k = 0; k < 4; ++k) if (!clip_of(e, w[k], c[k])) return false;
    tri(o, c[0], c[1], c[2], col); tri(o, c[0], c[2], c[3], col);
    return true;
}
// A segment in a billboard plane (origin P, axes r/u scaled by s), from (x0, y0) to (x1, y1) with thickness th (plane units).
void plane_seg(Overlay& o, const EyeView& e, const float P[3], const float r[3], const float u[3], float s, float x0, float y0, float x1, float y1, float th,
               const float col[4]) {
    float dx = x1 - x0, dy = y1 - y0;
    const float l = std::sqrt(dx * dx + dy * dy);
    if (!(l > 0)) return;
    const float nx = -dy / l * th * 0.5f, ny = dx / l * th * 0.5f;
    const float ex = dx / l * th * 0.5f, ey = dy / l * th * 0.5f;   // extend by half the thickness: closed corners
    x0 -= ex; y0 -= ey; x1 += ex; y1 += ey;
    const float pts[4][2] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    float w[4][3];
    for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) w[k][i] = P[i] + (r[i] * pts[k][0] + u[i] * pts[k][1]) * s;
    world_quad(o, e, w, col);
}
// Stroke font (unit cell, y up) for the two label texts.
struct Seg { float x0, y0, x1, y1; };
const Seg kF[] = {{0, 0, 0, 1}, {0, 1, 1, 1}, {0, 0.5f, 0.75f, 0.5f}};
const Seg kR[] = {{0, 0, 0, 1}, {0, 1, 0.8f, 1}, {0.8f, 1, 1, 0.85f}, {1, 0.85f, 1, 0.65f}, {1, 0.65f, 0.8f, 0.5f}, {0.8f, 0.5f, 0, 0.5f}, {0.4f, 0.5f, 1, 0}};
const Seg kN[] = {{0, 0, 0, 1}, {0, 1, 1, 0}, {1, 0, 1, 1}};
const Seg kD[] = {{0, 0, 0, 1}, {0, 1, 0.65f, 1}, {0.65f, 1, 1, 0.7f}, {1, 0.7f, 1, 0.3f}, {1, 0.3f, 0.65f, 0}, {0.65f, 0, 0, 0}};
const Seg kT[] = {{0, 1, 1, 1}, {0.5f, 1, 0.5f, 0}};
const Seg kG[] = {{1, 0.85f, 0.8f, 1}, {0.8f, 1, 0.2f, 1}, {0.2f, 1, 0, 0.8f}, {0, 0.8f, 0, 0.2f}, {0, 0.2f, 0.2f, 0}, {0.2f, 0, 0.8f, 0}, {0.8f, 0, 1, 0.2f},
                  {1, 0.2f, 1, 0.45f}, {1, 0.45f, 0.55f, 0.45f}};
struct Glyph { const Seg* s; unsigned n; };
Glyph glyph(char c) noexcept {
    switch (c) {
    case 'F': return {kF, 3}; case 'R': return {kR, 7}; case 'N': return {kN, 3}; case 'D': return {kD, 6}; case 'T': return {kT, 2}; case 'G': return {kG, 9};
    default: return {nullptr, 0};
    }
}
} // namespace

void begin_slot(Overlay& o, Slot s) noexcept { o.first[s] = uint32_t(o.v.size()); o.count[s] = 0; }
void end_slot(Overlay& o, Slot s) noexcept { o.count[s] = uint32_t(o.v.size()) - o.first[s]; }

bool eye_uv(const EyeView& e, const float p[3], float& u, float& v) noexcept {
    float c[4];
    if (!e.ok || !clip_of(e, p, c)) return false;
    u = 0.5f * (c[0] / c[3] + 1.0f); v = 0.5f * (1.0f - c[1] / c[3]);
    return true;
}

void build_radar(Overlay& o, const std::vector<Blip>& blips, const RadarGeom& g, float aspect) {
    auto at = [&](float u, float v, float out[4]) { out[0] = 2.0f * u - 1.0f; out[1] = 1.0f - 2.0f * v; out[2] = 0.0f; out[3] = 1.0f; };
    for (const Blip& b : blips) {
        float col[4];
        colour(b.rgb, b.coasting ? 0.45f : 1.0f, col);
        // direction on the HUD image: dir = 0 is up (-v); u is scaled by the aspect (image heights -> widths)
        const float sx = std::sin(b.dir), cy = -std::cos(b.dir);
        auto pt = [&](float along, float side, float out[4]) {   // along/side in image heights
            const float du = (sx * along + (-cy) * side) * aspect, dv = cy * along + sx * side;
            at(b.u + du, b.v + dv, out);
        };
        float p0[4], p1[4], p2[4], p3[4];
        if (b.edge) {          // chevron at the border pointing outward
            pt(0.008f, 0, p0); pt(-0.004f, 0.006f, p1); pt(-0.004f, -0.006f, p2);
            tri(o, p0, p1, p2, col);
        } else if (b.arrow) {  // arrow (the game's aircraft symbol): tip along the heading
            pt(0.011f, 0, p0); pt(-0.007f, 0.0065f, p1); pt(-0.003f, 0, p2); pt(-0.007f, -0.0065f, p3);
            tri(o, p0, p1, p2, col); tri(o, p0, p2, p3, col);
        } else {               // square (a contact without a usable heading)
            const float h = 0.005f;
            at(b.u - h * aspect, b.v - h, p0); at(b.u + h * aspect, b.v - h, p1); at(b.u + h * aspect, b.v + h, p2); at(b.u - h * aspect, b.v + h, p3);
            tri(o, p0, p1, p2, col); tri(o, p0, p2, p3, col);
        }
    }
    (void)g;
}

bool has_eye_frame(const EyeView& e) noexcept { return len3(e.fwd) > 0.5f && len3(e.right) > 0.5f; }

bool marker_screen_frame(const EyeView& e, const float pos[3], float& cx, float& cy, float& sx, float& sy) noexcept {
    if (!e.ok || !finite3(pos)) return false;
    float f[3] = {pos[0] - e.eye[0], pos[1] - e.eye[1], pos[2] - e.eye[2]};
    const float d = len3(f);
    if (!norm3(f)) return false;
    float c[4];
    if (!clip_of(e, pos, c)) return false;   // behind the eye: not drawn (no clamping, no edge indicator)
    float r[3], u[3];
    const bool frame = has_eye_frame(e);
    if (frame) { for (int i = 0; i < 3; ++i) { r[i] = e.right[i]; u[i] = e.up[i]; } }   // the eye's own screen axes: the box never rolls or shears
    else { cross3(f, e.up, r); if (!norm3(r)) return false; cross3(r, f, u); }
    // the local NDC scale per tan, measured along the eye's right (x only) and up (y only) axes: a screen-aligned rectangle, no cross terms (no shear)
    const float step = 0.01f;
    float p[3], a[4], b[4];
    for (int i = 0; i < 3; ++i) p[i] = pos[i] + r[i] * d * step;
    if (!clip_of(e, p, a)) return false;
    for (int i = 0; i < 3; ++i) p[i] = pos[i] + u[i] * d * step;
    if (!clip_of(e, p, b)) return false;
    cx = c[0] / c[3]; cy = c[1] / c[3];
    sx = (a[0] / a[3] - cx) / step; sy = (b[1] / b[3] - cy) / step;
    return std::isfinite(cx) && std::isfinite(cy) && std::isfinite(sx) && std::isfinite(sy) && sx != 0.0f && sy != 0.0f;
}

// A stroke from (x0,y0) to (x1,y1) (tan units around the box centre) drawn AXIS-ALIGNED in the eye's NDC: centre (cx,cy), scale (sx,sy) NDC per tan.
static void ndc_seg(Overlay& o, float cx, float cy, float sx, float sy, float x0, float y0, float x1, float y1, float th, const float col[4]) {
    float dx = x1 - x0, dy = y1 - y0;
    const float l = std::sqrt(dx * dx + dy * dy);
    if (!(l > 0)) return;
    const float nx = -dy / l * th * 0.5f, ny = dx / l * th * 0.5f, ex = dx / l * th * 0.5f, ey = dy / l * th * 0.5f;
    x0 -= ex; y0 -= ey; x1 += ex; y1 += ey;
    const float pts[4][2] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    float c[4][4];
    for (int k = 0; k < 4; ++k) { c[k][0] = cx + pts[k][0] * sx; c[k][1] = cy + pts[k][1] * sy; c[k][2] = 0.0f; c[k][3] = 1.0f; }
    tri(o, c[0], c[1], c[2], col); tri(o, c[0], c[2], c[3], col);
}

bool build_marker(Overlay& o, const EyeView& e, const WorldMarker& m) {
    float cx, cy, sx, sy;
    if (!marker_screen_frame(e, m.pos, cx, cy, sx, sy)) return false;
    float col[4];
    colour(m.rgb, (m.coasting ? 0.5f : 1.0f) * std::fmin(1.0f, std::fmax(0.0f, m.alpha)), col);
    const float h = m.half_tan_x, hv = m.half_tan_y, th = kMarkerLineTan;
    ndc_seg(o, cx, cy, sx, sy, -h, -hv, h, -hv, th, col);
    ndc_seg(o, cx, cy, sx, sy, h, -hv, h, hv, th, col);
    ndc_seg(o, cx, cy, sx, sy, h, hv, -h, hv, th, col);
    ndc_seg(o, cx, cy, sx, sy, -h, hv, -h, -hv, th, col);
    const char* text = m.label == label_frnd ? "FRND" : m.label == label_tgt ? "TGT" : "";
    const unsigned n = unsigned(std::char_traits<char>::length(text));
    if (n) {
        const float gh = kGlyphTan, gw = 0.82f * kGlyphTan, adv = gw + 0.24f * kGlyphTan, gap = 0.3f * kGlyphTan;
        const float x_end = -h - gap, y0 = hv + 0.1f * kGlyphTan;   // right-aligned at the box's left edge, sitting on its top (as the game's)
        float x = x_end - (float(n) * adv - (adv - gw));
        for (unsigned i = 0; i < n; ++i, x += adv) {
            const Glyph gl = glyph(text[i]);
            for (unsigned k = 0; k < gl.n; ++k)
                ndc_seg(o, cx, cy, sx, sy, x + gl.s[k].x0 * gw, y0 + gl.s[k].y0 * gh, x + gl.s[k].x1 * gw, y0 + gl.s[k].y1 * gh, th, col);
        }
    }
    return true;
}

void collect_markers(const Tracker& tk, double t, std::vector<WorldMarker>& out, uint32_t& skipped_game) {
    out.clear(); skipped_game = 0;
    for (const Track& tr : tk.tracks) {
        if (tr.team == team_unknown) continue;
        if (tr.game_marker_now) { ++skipped_game; continue; }
        WorldMarker m;
        const float age = float(std::min(kCoastTeamed, std::max(0.0, t - tr.t_seen)));
        for (int i = 0; i < 3; ++i) { m.pos[i] = tr.pos[i] + (!tr.seen_now && tr.vel_valid ? tr.vel[i] * age : 0.0f); m.rgb[i] = tr.rgb[i]; }
        m.label = tr.label; m.id = tr.id; m.coasting = !tr.seen_now;
        out.push_back(m);
    }
}
unsigned build_tracers(Overlay& o, const EyeView& e, const TracerSim& sim, double t) {
    if (!e.ok) return 0;
    unsigned n = 0;
    static const float kYellow[3] = {1.0f, 0.88f, 0.45f};
    for (const Round& r : sim.rounds) {
        float a[3], b[3], alpha;
        if (!TracerSim::streak(r, t, a, b, alpha)) continue;
        float dir[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, view[3] = {a[0] - e.eye[0], a[1] - e.eye[1], a[2] - e.eye[2]};
        const float dist = len3(view);
        float side[3];
        cross3(dir, view, side);
        if (!norm3(side) || !(dist > 0)) continue;
        const float wdt = std::fmax(1.2f, dist * 0.0012f);   // >= ~0.25 m, and >= ~1.2 mrad: visible but thin
        float w[4][3];
        for (int i = 0; i < 3; ++i) {
            w[0][i] = a[i] + side[i] * wdt; w[1][i] = b[i] + side[i] * wdt; w[2][i] = b[i] - side[i] * wdt; w[3][i] = a[i] - side[i] * wdt;
        }
        float col[4];
        colour(kYellow, 0.9f * alpha, col);
        if (world_quad(o, e, w, col)) ++n;
    }
    return n;
}

} // namespace acz::tac
