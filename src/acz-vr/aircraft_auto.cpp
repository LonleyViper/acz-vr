// AIRCRAFT-AUTO core (see aircraft_auto.h). SDK-free.
#include "aircraft_auto.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace acz::aircraft {

// Mirrored from research/world-render-data/aircraft_cockpits.json (type, model_id, pvb eye, stick == "side"); aircraft_auto_offline_tests
// compares every row with the JSON file. Order = plmodel/playerlist.txt (the tie rule's "first in table order").
const AircraftType kAircraft[kAircraftCount] = {
    {"a10a", "a10ak_vrcockpit", {0.0f, 7.9f, -29.2f}, 0},
    {"e06b", "e06bk_vrcockpit", {-1.885f, 3.76f, -32.7f}, 0},
    {"e18g", "e18gk_vrcockpit", {0.0f, 5.25f, -34.75f}, 0},
    {"f01a", "f01ak_vrcockpit", {0.0f, 1.96f, -37.5f}, 0},
    {"f02a", "f02ak_vrcockpit", {0.0f, 4.0f, -33.1f}, 0},
    {"f04e", "f04ek_vrcockpit", {0.0f, 6.99f, -35.9f}, 0},
    {"f05e", "f05ek_vrcockpit", {0.0f, 3.97f, -21.9f}, 0},
    {"f14d", "f14dk_vrcockpit", {0.0f, 5.45f, -41.0f}, 0},
    {"f15c", "f15ck_vrcockpit", {0.0f, 5.63f, -49.05f}, 0},
    {"f15e", "f15ek_vrcockpit", {0.0f, 5.63f, -49.05f}, 0},
    {"f15s", "f15sk_vrcockpit", {0.0f, 5.63f, -49.05f}, 0},
    {"f16c", "f16ck_vrcockpit", {0.0f, 3.8f, -33.2f}, 1},
    {"f16x", "f16xk_vrcockpit", {0.0f, 3.81f, -39.45f}, 1},
    {"f17a", "f17ak_vrcockpit", {0.0f, 6.0f, -38.1f}, 0},
    {"f18c", "f18ck_vrcockpit", {0.0f, 4.88f, -36.91f}, 0},
    {"f20a", "f20ak_vrcockpit", {0.0f, 3.961f, -22.5f}, 0},
    {"f35c", "f35ck_vrcockpit", {0.0f, 4.9f, -31.2f}, 1},
    {"fa22", "fa22k_vrcockpit", {0.0f, 5.73f, -48.4f}, 1},
    {"j35j", "j35jk_vrcockpit", {0.0f, 3.575f, -36.3f}, 0},
    {"j39c", "j39ck_vrcockpit", {0.0f, 2.2f, -31.8f}, 0},
    {"m21b", "m21bk_vrcockpit", {0.0f, 3.875f, -19.0f}, 0},
    {"m29a", "m29ak_vrcockpit", {0.0f, 5.19f, -34.6f}, 0},
    {"m31b", "m31bk_vrcockpit", {0.0f, 4.59f, -51.5f}, 0},
    {"mr2d", "mr2dk_vrcockpit", {0.0f, 4.46f, -32.6f}, 0},
    {"mrgn", "mrgnk_vrcockpit", {0.0f, 3.9f, -52.45f}, 0},
    {"rm01", "rm01k_vrcockpit", {0.0f, 5.25f, -33.4f}, 1},
    {"su27", "su27k_vrcockpit", {0.0f, 6.9f, -46.8f}, 0},
    {"su32", "su32k_vrcockpit", {-2.1f, 6.4f, -46.9f}, 0},
    {"su37", "su37k_vrcockpit", {0.0f, 7.2f, -46.8f}, 1},
    {"su47", "su47k_vrcockpit", {0.0f, 3.83f, -49.4f}, 1},
    {"tnd4", "tnd4k_vrcockpit", {0.0f, 2.01f, -30.4f}, 0},
    {"typn", "typnk_vrcockpit", {0.0f, 5.5f, -34.2f}, 0},
    {"x02a", "x02ak_vrcockpit", {0.0f, 2.8f, -49.6f}, 0},
    {"x29a", "x29ak_vrcockpit", {0.0f, 3.95f, -27.1f}, 0},
    {"yf23", "yf23k_vrcockpit", {0.0f, 4.85f, -53.0f}, 1},
    {"zoef", "zoefk_vrcockpit", {0.0f, 5.4f, -39.1f}, 0},
};

namespace {
constexpr double kPi = 3.14159265358979323846;
double wrap(double a) noexcept {
    if (!std::isfinite(a)) return a;
    a = std::fmod(a + kPi, 2 * kPi); if (a < 0) a += 2 * kPi; return a - kPi;
}
float dist3(const float* a, const float* b) noexcept {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

int find_type(const char* type) noexcept {
    if (!type) return -1;
    for (unsigned i = 0; i < kAircraftCount; ++i) if (std::strcmp(kAircraft[i].type, type) == 0) return int(i);
    return -1;
}
uint64_t same_eye_mask(unsigned i) noexcept {
    if (i >= kAircraftCount) return 0;
    uint64_t m = 0;
    for (unsigned j = 0; j < kAircraftCount; ++j) if (dist3(kAircraft[i].eye, kAircraft[j].eye) <= 1e-4f) m |= uint64_t(1) << j;
    return m;
}
float min_distinct_eye_separation() noexcept {
    float best = 1e30f;
    for (unsigned i = 0; i < kAircraftCount; ++i)
        for (unsigned j = i + 1; j < kAircraftCount; ++j) {
            const float d = dist3(kAircraft[i].eye, kAircraft[j].eye);
            if (d > 1e-4f) best = std::min(best, d);
        }
    return best;
}
float nearest_eye_distance(const float local[3]) noexcept {
    if (!local || !std::isfinite(local[0]) || !std::isfinite(local[1]) || !std::isfinite(local[2])) return 1e30f;
    float best = 1e30f;
    for (unsigned i = 0; i < kAircraftCount; ++i) best = std::min(best, dist3(local, kAircraft[i].eye));
    return best;
}
bool plausible_eye(const float local[3]) noexcept { return nearest_eye_distance(local) <= kPlausibleEyeDist; }

bool eye_in_aircraft(const acz::e6loc::Sample& s, float local[3], bool& centred) noexcept {
    const double d[3] = {double(s.cam[0]) - s.air[0], double(s.cam[1]) - s.air[1], double(s.cam[2]) - s.air[2]};
    const double p = s.rot[0], y = s.rot[1], r = s.rot[2];
    if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2]) || !std::isfinite(p) || !std::isfinite(y) || !std::isfinite(r)) return false;
    // local = Rz(-r) Rx(-p) Ry(-y) d
    const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
    const double a0 = cy * d[0] - sy * d[2], a1 = d[1], a2 = sy * d[0] + cy * d[2];       // Ry(-y)
    const double b0 = a0, b1 = cp * a1 + sp * a2, b2 = -sp * a1 + cp * a2;                // Rx(-p)
    const double c0 = cr * b0 + sr * b1, c1 = -sr * b0 + cr * b1, c2 = b2;                // Rz(-r)
    local[0] = float(c0); local[1] = float(c1); local[2] = float(c2);
    centred = std::fabs(double(s.look[0])) <= kLookCentredRad && std::fabs(double(s.look[1])) <= kLookCentredRad &&
              std::fabs(double(s.look[2])) <= kLookCentredRad && std::isfinite(double(s.heading)) &&
              std::fabs(wrap(double(s.rot[1]) - double(s.heading))) <= kLookCentredRad;
    return std::isfinite(local[0]) && std::isfinite(local[1]) && std::isfinite(local[2]);
}

const char* verdict_name(Verdict v) noexcept {
    switch (v) {
    case Verdict::match: return "match";
    case Verdict::no_match: return "no-match";
    case Verdict::unstable: return "unstable";
    case Verdict::ambiguous: return "ambiguous";
    }
    return "?";
}

WindowResult Detector::evaluate(const std::vector<float>& xyz, const DetectOptions& o) noexcept {
    WindowResult w;
    const size_t n = xyz.size() / 3;
    w.samples = unsigned(n);
    if (!n) { w.verdict = Verdict::unstable; return w; }
    // Runtime-safety review (docs/review/overnight-runtime-review.md R-1): this noexcept function runs on the XR owner thread; an allocation
    // failure (commit at 94-99.8 % in step65) must not reach std::terminate and take the game down. The window is then simply unstable.
    std::vector<float> c;
    try { c.resize(n); } catch (...) { w.verdict = Verdict::unstable; return w; }
    for (int k = 0; k < 3; ++k) {
        for (size_t i = 0; i < n; ++i) c[i] = xyz[3 * i + k];
        std::nth_element(c.begin(), c.begin() + n / 2, c.end());
        float m = c[n / 2];
        if (!(n & 1)) { const float lo = *std::max_element(c.begin(), c.begin() + n / 2); m = 0.5f * (m + lo); }
        w.median[k] = m;
    }
    size_t inliers = 0;
    for (size_t i = 0; i < n; ++i) {
        const float r = dist3(&xyz[3 * i], w.median);
        w.spread = std::max(w.spread, r);
        inliers += r <= o.spread_tol;
    }
    w.inlier_frac = float(inliers) / float(n);
    // nearest and second-nearest DISTINCT eye points (types with an identical eye are one candidate: the first of them in table order)
    for (unsigned i = 0; i < kAircraftCount; ++i) {
        bool dup = false;
        for (unsigned j = 0; j < i && !dup; ++j) dup = dist3(kAircraft[i].eye, kAircraft[j].eye) <= 1e-4f;
        if (dup) continue;
        const float r = dist3(w.median, kAircraft[i].eye);
        if (w.best < 0 || r < w.best_res) { w.second = w.best; w.second_res = w.best_res; w.best = int(i); w.best_res = r; }
        else if (w.second < 0 || r < w.second_res) { w.second = int(i); w.second_res = r; }
    }
    if (w.inlier_frac < o.min_inliers) { w.verdict = Verdict::unstable; return w; }
    if (w.best < 0 || w.best_res > o.match_tol) { w.verdict = Verdict::no_match; return w; }
    if (w.second >= 0 && w.second_res <= o.match_tol) { w.verdict = Verdict::ambiguous; return w; }   // cannot happen while match_tol < separation / 2
    w.ties = same_eye_mask(unsigned(w.best));
    w.chosen = w.best;
    if (w.ties & o.prefer)
        for (unsigned i = 0; i < kAircraftCount; ++i) if ((w.ties & o.prefer) & (uint64_t(1) << i)) { w.chosen = int(i); break; }
    w.verdict = Verdict::match;
    w.confidence = o.match_tol > 0 ? std::max(0.0f, 1.0f - w.best_res / o.match_tol) : 0.0f;
    return w;
}

void Detector::assume(int type) noexcept {
    if (type < 0 || unsigned(type) >= kAircraftCount) return;
    current_ = type; chosen_epoch_ = epoch_; assumed_ = true; confirmed_ = false; pending_ = -2; pending_n_ = 0;
}

bool Detector::feed(const float local[3], bool centred, uint32_t lock_epoch) noexcept {
    changed_ = false; confirmed_ = false;
    if (!have_epoch_ || lock_epoch != epoch_) {
        if (have_epoch_) { buf_.clear(); pending_ = -2; pending_n_ = 0; }
        have_epoch_ = true; epoch_ = lock_epoch; ++epochs;
    }
    if (!centred) { ++off_centre_samples; return false; }
    ++centred_samples;
    try { buf_.insert(buf_.end(), local, local + 3); }
    catch (...) { buf_.clear(); return false; }   // R-1: out of memory in a noexcept path -> drop the partial window, never terminate
    if (buf_.size() < size_t(o_.window) * 3) return false;
    last_ = evaluate(buf_, o_);
    buf_.clear();
    ++windows;
    // Hysteresis: match -> that type, no-match -> none (-1); unstable/ambiguous windows decide nothing and break a pending streak.
    int cand;
    if (last_.verdict == Verdict::match) cand = last_.chosen;
    else if (last_.verdict == Verdict::no_match) cand = -1;
    else { pending_ = -2; pending_n_ = 0; return true; }
    if (cand == current_) {
        if (assumed_) { assumed_ = false; confirmed_ = true; }   // the assumption is now evidence-backed
        chosen_epoch_ = epoch_;
        pending_ = -2; pending_n_ = 0; return true;
    }
    if (assumed_ && cand < 0) { pending_ = -2; pending_n_ = 0; return true; }   // no-match never drops an assumed type
    // W5-3 P2 #1: no-match never drops a type chosen / confirmed in THIS lock epoch either (a longer streak of an eye-offset error would unload the model and,
    // with the look turned away, no window could ever restore it). Only a new lock epoch (mission change / re-lock) lets the no-match streak drop it.
    if (cand < 0 && current_ >= 0 && epoch_ == chosen_epoch_) { pending_ = -2; pending_n_ = 0; return true; }
    if (cand == pending_) ++pending_n_; else { pending_ = cand; pending_n_ = 1; }
    const unsigned need = (current_ < 0 && cand >= 0) ? 1u : (cand < 0 ? std::max(1u, o_.drop_confirm) : std::max(1u, o_.confirm));
    if (pending_n_ >= need) { current_ = cand; chosen_epoch_ = epoch_; pending_ = -2; pending_n_ = 0; changed_ = true; assumed_ = false; ++switches; }
    return true;
}

// ------------------------------------------------------------------------------------------------ config
bool is_auto(const std::string& value) noexcept {
    return value.compare(0, 4, "auto") == 0 && (value.size() == 4 || value[4] == ';');
}

bool parse_auto(const std::string& value, AutoConfig& out, std::string& why) {
    if (!is_auto(value)) { why = "not an auto value"; return false; }
    AutoConfig c;
    // Split off prefer: (ours) and refuse the per-model options; everything else goes through the regular cockpit option parser.
    std::string rest = "auto";
    size_t pos = 5;
    while (pos <= value.size() && value.size() > 4) {
        size_t semi = value.find(';', pos);
        if (semi == std::string::npos) semi = value.size();
        const std::string item = value.substr(pos, semi - pos);
        pos = semi + 1;
        const size_t eq = item.find_first_of(":=");
        const std::string k = eq == std::string::npos ? item : item.substr(0, eq);
        if (k == "prefer" && eq != std::string::npos) {
            const std::string v = item.substr(eq + 1);
            size_t p2 = 0;
            for (;;) {
                size_t c2 = v.find(',', p2); if (c2 == std::string::npos) c2 = v.size();
                const std::string t = v.substr(p2, c2 - p2);
                const int idx = find_type(t.c_str());
                if (idx < 0) { why = "prefer: unknown aircraft type '" + t + "'"; return false; }
                c.prefer |= uint64_t(1) << unsigned(idx);
                p2 = c2 + 1;
                if (c2 == v.size()) break;
            }
            c.prefer_text = v;
        } else if (k == "default" && eq != std::string::npos) {   // AIRCRAFT-AUTO-FAST: assumed model when no remembered aircraft exists
            const std::string v = item.substr(eq + 1);
            if (v.find_first_of("/\\:.%") != std::string::npos) { why = "default: must be a model stem, not a path"; return false; }
            const int t = find_type_or_model(v);
            if (t < 0) { why = "default: unknown aircraft model '" + v + "' (a table model stem such as j35jk_vrcockpit, or a type such as j35j)"; return false; }
            c.default_type = t; c.default_text = v;
        } else if (k == "stale" && eq != std::string::npos) {   // AIRCRAFT-AUTO-STALE: ms without any locked E6 sample before the shown model is dropped (0 = never)
            const std::string v = item.substr(eq + 1);
            if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != std::string::npos || std::stoul(v) > 600000ul) { why = "stale: must be 0..600000 (ms)"; return false; }
            c.stale_ms = unsigned(std::stoul(v));
        } else if (k == "grace" && eq != std::string::npos) {   // AIRCRAFT-AUTO-GRACE: extra ms an unconfirmed (remembered/default) model is trusted before the stale rule
            const std::string v = item.substr(eq + 1);
            if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != std::string::npos || std::stoul(v) > 600000ul) { why = "grace: must be 0..600000 (ms)"; return false; }
            c.grace_ms = unsigned(std::stoul(v));
        } else if (k == "hide" || k == "show" || k == "eye" || k == "clipbox") {   // clipbox: = per model (the table gives each type its own)
            why = k + ": is a per-model option (not allowed with auto)"; return false;
        } else {
            if (k == "stick") c.stick_explicit = true;
            rest += ';'; rest += item;
        }
        if (semi == value.size()) break;
    }
    std::string id;
    if (!acz::cockpit::parse_options(rest, id, c.opt, why)) return false;
    out = c;
    return true;
}

acz::cockpit::Options options_for(const AutoConfig& c, unsigned type) noexcept {
    acz::cockpit::Options o = c.opt;
    if (!c.stick_explicit && type < kAircraftCount) o.stick = kAircraft[type].stick;
    return o;
}

namespace {
bool plain_name(const std::string& s) noexcept {
    if (s.empty() || s.size() > 24) return false;
    for (char ch : s) if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    return true;
}
// The string value of "key" in a flat JSON object: no escapes, <= 64 characters. false when absent or not a plain string.
bool json_string(const std::string& t, const char* key, std::string& out, std::string& why) {
    const std::string k = std::string("\"") + key + "\"";
    const size_t p = t.find(k);
    if (p == std::string::npos) { why = std::string("missing-") + key; return false; }
    if (t.find(k, p + 1) != std::string::npos) { why = std::string("duplicate-") + key; return false; }
    size_t i = p + k.size();
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
    if (i >= t.size() || t[i] != ':') { why = std::string("bad-") + key; return false; }
    ++i;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
    if (i >= t.size() || t[i] != '"') { why = std::string("bad-") + key; return false; }
    const size_t e = t.find('"', i + 1);
    if (e == std::string::npos || e - i - 1 > 64) { why = std::string("bad-") + key; return false; }
    out = t.substr(i + 1, e - i - 1);   // raw: a backslash (escape or path) is refused by the caller's character check
    return true;
}
} // namespace

int find_type_or_model(const std::string& name) noexcept {
    if (!plain_name(name)) return -1;
    for (unsigned i = 0; i < kAircraftCount; ++i)
        if (name == kAircraft[i].type || name == kAircraft[i].model_id) return int(i);
    return -1;
}

std::string remembered_path() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return std::string();
    return std::string(la) + "\\acz-vr\\last-aircraft.json";
}

std::string remembered_json(unsigned type) {
    if (type >= kAircraftCount) return std::string();
    return std::string("{\"version\":1,\"type\":\"") + kAircraft[type].type + "\",\"model\":\"" + kAircraft[type].model_id + "\"}\n";
}

bool parse_remembered(const std::string& text, int& type, std::string& why) {
    type = -1;
    if (text.size() > kRememberedMaxBytes) { why = "too-large"; return false; }
    if (text.find('\0') != std::string::npos) { why = "binary"; return false; }
    size_t a = text.find_first_not_of(" \t\r\n");
    const size_t b = text.find_last_not_of(" \t\r\n");
    if (a == std::string::npos || text[a] != '{' || text[b] != '}') { why = "not-a-json-object"; return false; }
    std::string t, m;
    if (!json_string(text, "type", t, why) || !json_string(text, "model", m, why)) return false;
    if (t.find_first_of("/\\:.%") != std::string::npos || m.find_first_of("/\\:.%") != std::string::npos) { why = "path-characters-refused"; return false; }
    if (!plain_name(t) || !plain_name(m)) { why = "bad-characters"; return false; }
    const int i = find_type(t.c_str());
    if (i < 0) { why = "unknown-type"; return false; }
    if (m != kAircraft[i].model_id) { why = "model-not-the-types-table-model"; return false; }
    type = i;
    return true;
}

bool read_remembered(const std::string& path, int& type, std::string& why) {
    type = -1;
    if (path.empty()) { why = "no-path"; return false; }
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(path);
    if (!std::filesystem::exists(p, ec)) { why = "no-file"; return false; }
    if (!std::filesystem::is_regular_file(p, ec)) { why = "not-a-file"; return false; }
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) { why = "unreadable"; return false; }
    if (sz > kRememberedMaxBytes) { why = "too-large"; return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { why = "unreadable"; return false; }
    std::string text(size_t(sz), '\0');
    f.read(&text[0], std::streamsize(sz));
    if (size_t(f.gcount()) != size_t(sz)) { why = "short-read"; return false; }
    return parse_remembered(text, type, why);
}

bool write_remembered(const std::string& path, unsigned type, std::string& why) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    const std::string text = remembered_json(type);
    if (text.empty()) { why = "bad-type"; return false; }
    if (path.empty()) { why = "no-path"; return false; }
    try {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::path(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        std::filesystem::path tmp = p; tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { why = "temp-open-failed"; return false; }
            f.write(text.data(), std::streamsize(text.size()));
            f.flush();
            if (!f) { why = "temp-write-failed"; f.close(); std::filesystem::remove(tmp, ec); return false; }
        }
        std::filesystem::rename(tmp, p, ec);   // MSVC: MoveFileExW(MOVEFILE_REPLACE_EXISTING): the old file or the new one, never a torn mix
        if (ec) { why = "rename-failed"; std::filesystem::remove(tmp, ec); return false; }
        return true;
    } catch (...) { why = "exception"; return false; }
}

int resolve_assumed(const AutoConfig& cfg, const std::string& path, int& remembered, std::string& source, std::string& why) {
    remembered = -1; source.clear(); why.clear();
    int t = -1;
    try {
        if (read_remembered(path, t, why)) { remembered = t; source = "remembered-file"; return t; }
    } catch (...) { why = "exception"; }
    if (cfg.default_type >= 0 && unsigned(cfg.default_type) < kAircraftCount) { source = "config-default"; return cfg.default_type; }
    return -1;
}

} // namespace acz::aircraft
