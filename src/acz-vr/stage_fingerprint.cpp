#include "stage_fingerprint.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace acz::stagefp {
namespace {

// ------------------------------------------------------------------------------------------------ minimal JSON (the fixed schema only)
struct JVal {
    enum class T : uint8_t { null, boolean, number, string, array, object } t = T::null;
    double num = 0; bool b = false; std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal* get(const char* k) const {
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};
struct JParser {
    const char* p; const char* e; int depth = 0; std::string err;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool fail(const char* w) { if (err.empty()) err = w; return false; }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return fail("expected string");
        ++p;
        while (p < e && *p != '"') {
            if (*p == '\\') {
                if (++p >= e) return fail("bad escape");
                const char c = *p++;
                if (c == '"' || c == '\\' || c == '/') out.push_back(c);
                else if (c == 'n') out.push_back('\n'); else if (c == 't') out.push_back('\t'); else if (c == 'r') out.push_back('\r');
                else if (c == 'b') out.push_back('\b'); else if (c == 'f') out.push_back('\f');
                else if (c == 'u') {                     // only ASCII escapes are expected in this schema
                    if (e - p < 4) return fail("bad \\u");
                    unsigned v = 0;
                    for (int i = 0; i < 4; ++i) { const char h = *p++; v <<= 4; if (h >= '0' && h <= '9') v |= unsigned(h - '0'); else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10); else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10); else return fail("bad \\u"); }
                    if (v > 0x7f) return fail("non-ASCII \\u");
                    out.push_back(char(v));
                } else return fail("bad escape");
            } else out.push_back(*p++);
            if (out.size() > 4096) return fail("string too long");
        }
        if (p >= e) return fail("unterminated string");
        ++p;
        return true;
    }
    bool val(JVal& v) {
        ws();
        if (p >= e) return fail("unexpected end");
        if (++depth > 16) return fail("too deep");
        bool ok = true;
        if (*p == '{') {
            v.t = JVal::T::object; ++p; ws();
            if (p < e && *p == '}') { ++p; --depth; return true; }
            for (;;) {
                ws(); std::string k; if (!str(k)) return false;
                ws(); if (p >= e || *p != ':') return fail("expected :"); ++p;
                JVal c; if (!val(c)) return false;
                v.obj.emplace_back(std::move(k), std::move(c));
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                return fail("expected , or }");
            }
        } else if (*p == '[') {
            v.t = JVal::T::array; ++p; ws();
            if (p < e && *p == ']') { ++p; --depth; return true; }
            for (;;) {
                JVal c; if (!val(c)) return false;
                v.arr.push_back(std::move(c));
                if (v.arr.size() > kMaxSigs + 16) return fail("array too long");
                ws(); if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; break; }
                return fail("expected , or ]");
            }
        } else if (*p == '"') { v.t = JVal::T::string; ok = str(v.str); }
        else if (e - p >= 4 && std::memcmp(p, "true", 4) == 0) { v.t = JVal::T::boolean; v.b = true; p += 4; }
        else if (e - p >= 5 && std::memcmp(p, "false", 5) == 0) { v.t = JVal::T::boolean; p += 5; }
        else if (e - p >= 4 && std::memcmp(p, "null", 4) == 0) { p += 4; }
        else {
            char buf[40]; size_t k = 0;
            while (p < e && k < sizeof buf - 1 && (std::strchr("+-0123456789.eE", *p))) buf[k++] = *p++;
            buf[k] = 0;
            if (!k) return fail("unexpected character");
            char* end = nullptr; v.num = std::strtod(buf, &end);
            if (end != buf + k) return fail("bad number");
            v.t = JVal::T::number;
        }
        --depth;
        return ok;
    }
};

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 8) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool as_uint(const JVal* v, double lo, double hi, uint32_t& out) {
    if (!v || v->t != JVal::T::number || !(v->num >= lo && v->num <= hi) || v->num != double(uint32_t(v->num))) return false;
    out = uint32_t(v->num); return true;
}
constexpr uint64_t kMaskWord = 0xA5A5A5A5A5A5A5A5ull;
static_assert(kMask == 0xA5, "mask word");
inline uint64_t load64(const uint8_t* p) noexcept { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t hash64(uint64_t k) noexcept { return k * 0x9E3779B97F4A7C15ull; }

} // namespace

const char* kind_name(Result::Kind k) noexcept {
    switch (k) { case Result::Kind::match: return "match"; case Result::Kind::ambiguous: return "ambiguous"; default: return "no-match"; }
}

bool parse_table(const char* json, size_t n, Table& out, std::string& why) {
    out = Table{};
    if (!json || !n) { why = "empty"; return false; }
    if (n > kMaxJsonBytes) { why = "file too large"; return false; }
    JParser jp{json, json + n};
    JVal root;
    if (!jp.val(root)) { why = "json: " + jp.err; return false; }
    jp.ws();
    if (jp.p != jp.e) { why = "json: trailing data"; return false; }
    if (root.t != JVal::T::object) { why = "root is not an object"; return false; }
    const JVal* fmt = root.get("format");
    if (!fmt || fmt->t != JVal::T::string || fmt->str != "acz-stage-fingerprints") { why = "format"; return false; }
    uint32_t ver = 0, window = 0, min_hits = 0;
    if (!as_uint(root.get("version"), 1, 1000, ver) || ver != kFormatVersion) { why = "version"; return false; }
    if (!as_uint(root.get("window"), 1, 4096, window) || window != kWindow) { why = "window"; return false; }
    if (!as_uint(root.get("min_hits"), 1, 1000, min_hits)) { why = "min_hits"; return false; }
    Table t; t.window = window; t.min_hits = min_hits;
    const JVal* gs = root.get("groups");
    if (!gs || gs->t != JVal::T::array || gs->arr.empty() || gs->arr.size() > kMaxGroups) { why = "groups"; return false; }
    std::set<std::string> ids;
    for (const JVal& g : gs->arr) {
        Group G;
        const JVal* st = g.get("stage"); const JVal* mem = g.get("members"); const JVal* sh = g.get("zvrs_sha256");
        if (g.t != JVal::T::object || !st || st->t != JVal::T::string || !valid_id(st->str)) { why = "group stage id"; return false; }
        G.stage = st->str;
        if (!mem || mem->t != JVal::T::array || mem->arr.empty() || mem->arr.size() > 64) { why = "group members"; return false; }
        bool rep_in = false;
        for (const JVal& m : mem->arr) {
            if (m.t != JVal::T::string || !valid_id(m.str)) { why = "group member id"; return false; }
            if (!ids.insert(m.str).second) { why = "stage id in two groups: " + m.str; return false; }
            rep_in = rep_in || m.str == G.stage;
            G.members.push_back(m.str);
        }
        if (!rep_in) { why = "representative not a member: " + G.stage; return false; }
        if (sh && sh->t == JVal::T::string) G.zvrs_sha256 = sh->str.substr(0, 64);
        t.groups.push_back(std::move(G));
    }
    const JVal* ss = root.get("sigs");
    if (!ss || ss->t != JVal::T::array || ss->arr.empty() || ss->arr.size() > kMaxSigs) { why = "sigs"; return false; }
    std::set<std::string> seen;
    for (const JVal& s : ss->arr) {
        const JVal* hx = s.get("hex"); const JVal* og = s.get("groups"); const JVal* src = s.get("src");
        if (s.t != JVal::T::object || !hx || hx->t != JVal::T::string || hx->str.size() != 2 * kWindow) { why = "sig hex length"; return false; }
        if (!seen.insert(hx->str).second) { why = "duplicate signature"; return false; }
        Sig S{};
        for (uint32_t i = 0; i < kWindow; ++i) {
            const int a = hexv(hx->str[2 * i]), b = hexv(hx->str[2 * i + 1]);
            if (a < 0 || b < 0) { why = "sig hex digit"; return false; }
            S.masked[i] = uint8_t(((a << 4) | b) ^ kMask);
        }
        uint8_t raw8[8]; for (int i = 0; i < 8; ++i) raw8[i] = uint8_t(S.masked[i] ^ kMask);
        S.key_masked = load64(raw8) ^ kMaskWord;
        std::memset(raw8, 0, sizeof raw8);
        if (!og || og->t != JVal::T::array || og->arr.empty() || og->arr.size() > t.groups.size()) { why = "sig groups"; return false; }
        for (const JVal& gi : og->arr) {
            uint32_t g = 0;
            if (!as_uint(&gi, 0, double(t.groups.size() - 1), g)) { why = "sig group index"; return false; }
            if (std::find(S.groups.begin(), S.groups.end(), uint16_t(g)) != S.groups.end()) { why = "sig group repeated"; return false; }
            S.groups.push_back(uint16_t(g));
        }
        if (src && src->t == JVal::T::string) std::snprintf(S.src, sizeof S.src, "%s", src->str.c_str());
        else std::snprintf(S.src, sizeof S.src, "?");
        t.sigs.push_back(std::move(S));
    }
    out = std::move(t);
    return true;
}

bool load_table_file(const std::string& path, Table& out, std::string& why) {
    out = Table{};
    if (path.empty()) { why = "no path (LOCALAPPDATA unset)"; return false; }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { why = "cannot open " + path; return false; }
    std::string buf;
    char tmp[65536];
    for (;;) {
        const size_t r = std::fread(tmp, 1, sizeof tmp, f);
        buf.append(tmp, r);
        if (buf.size() > kMaxJsonBytes) { std::fclose(f); why = "file too large"; return false; }
        if (r < sizeof tmp) break;
    }
    std::fclose(f);
    return parse_table(buf.data(), buf.size(), out, why);
}

std::string table_path(const std::string& dir_override) {
    std::string dir = dir_override;
    if (dir.empty()) {
        const char* la = std::getenv("LOCALAPPDATA");
        if (!la || !*la) return std::string();
        dir = std::string(la) + "\\acz-vr\\stage";
    }
    return dir + "\\fingerprints.json";
}

Matcher::Matcher(const Table& t) : t_(t) {
    size_t cap = 64;
    while (cap < t.sigs.size() * 4) cap <<= 1;
    slots_.assign(cap, -1);
    mask_ = cap - 1;
    shift_ = 64;
    for (size_t c = cap; c > 1; c >>= 1) --shift_;
    for (size_t i = 0; i < t.sigs.size(); ++i) {
        uint64_t h = hash64(t.sigs[i].key_masked ^ kMaskWord) >> shift_;
        while (slots_[h] >= 0) h = (h + 1) & mask_;
        slots_[h] = int32_t(i);
    }
    seen_.assign(t.sigs.size(), 0);
}

void Matcher::reset() noexcept { std::fill(seen_.begin(), seen_.end(), uint8_t(0)); carry_n_ = 0; bytes_ = 0; seen_n_ = 0; }

void Matcher::scan(const uint8_t* p, size_t n) noexcept {
    if (n < kWindow || t_.sigs.empty()) return;
    const size_t last = n - kWindow;
    const int32_t* slots = slots_.data();
    for (size_t i = 0; i <= last; ++i) {
        const uint64_t k = load64(p + i);
        uint64_t h = hash64(k) >> shift_;
        int32_t s = slots[h];
        if (s < 0) continue;
        const uint64_t km = k ^ kMaskWord;
        do {
            const Sig& S = t_.sigs[size_t(s)];
            if (S.key_masked == km && !seen_[size_t(s)]) {
                bool eq = true;
                for (uint32_t b = 8; b < kWindow; ++b) if (uint8_t(p[i + b] ^ kMask) != S.masked[b]) { eq = false; break; }
                if (eq) { seen_[size_t(s)] = 1; ++seen_n_; }
            }
            h = (h + 1) & mask_;
            s = slots[h];
        } while (s >= 0);
    }
}

void Matcher::feed(const uint8_t* p, size_t n) noexcept {
    if (!p || !n) return;
    bytes_ += n;
    constexpr size_t W1 = kWindow - 1;
    if (carry_n_) {   // windows that start in the carried tail and end in this chunk
        uint8_t tmp[2 * kWindow];
        const size_t take = n < W1 ? n : W1;
        std::memcpy(tmp, carry_, carry_n_);
        std::memcpy(tmp + carry_n_, p, take);
        const size_t tn = carry_n_ + take;
        if (tn >= kWindow) {
            // only start positions inside the carry: positions inside p are scanned below
            const size_t lim = std::min(carry_n_, tn - kWindow + 1);
            uint8_t win[2 * kWindow];
            std::memcpy(win, tmp, lim + kWindow - 1);
            scan(win, lim + kWindow - 1);
        }
    }
    scan(p, n);
    if (n >= W1) { std::memcpy(carry_, p + n - W1, W1); carry_n_ = W1; }
    else {
        uint8_t tmp[2 * kWindow];
        std::memcpy(tmp, carry_, carry_n_); std::memcpy(tmp + carry_n_, p, n);
        const size_t total = carry_n_ + n, keep = total < W1 ? total : W1;
        std::memcpy(carry_, tmp + total - keep, keep); carry_n_ = keep;
    }
}

Result Matcher::decide() const {
    Result r;
    std::vector<uint32_t> hits(t_.groups.size(), 0);
    for (size_t i = 0; i < seen_.size(); ++i) {
        if (!seen_[i]) continue;
        ++r.sigs_seen;
        for (uint16_t g : t_.sigs[i].groups) ++hits[g];
    }
    for (size_t g = 0; g < hits.size(); ++g) if (hits[g]) r.ranking.emplace_back(int(g), hits[g]);
    std::stable_sort(r.ranking.begin(), r.ranking.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (r.ranking.empty()) { r.kind = Result::Kind::no_match; r.reason = "no-signature"; return r; }
    r.best = r.ranking[0].second;
    r.second = r.ranking.size() > 1 ? r.ranking[1].second : 0;
    if (r.best < t_.min_hits) { r.kind = Result::Kind::no_match; r.reason = "below-min-hits"; return r; }
    if (r.second == r.best) { r.kind = Result::Kind::ambiguous; r.reason = "tie"; return r; }
    r.kind = Result::Kind::match; r.reason = "match";
    r.group = r.ranking[0].first;
    r.stage = t_.groups[size_t(r.group)].stage;
    return r;
}

void Matcher::lead(uint32_t& best, uint32_t& second, int& group) const noexcept {
    // decide() without allocating (callable from noexcept scan loops): per-group hits on the stack (kMaxGroups is enforced by the parser).
    uint32_t hits[kMaxGroups] = {};
    best = 0; second = 0; group = -1;
    const size_t ng = t_.groups.size() < kMaxGroups ? t_.groups.size() : kMaxGroups;
    for (size_t i = 0; i < seen_.size(); ++i) {
        if (!seen_[i]) continue;
        for (uint16_t g : t_.sigs[i].groups) if (g < ng) ++hits[g];
    }
    for (size_t g = 0; g < ng; ++g) {   // same order as decide(): stable sort keeps the lowest index first on equal hits
        if (hits[g] > best) { second = best; best = hits[g]; group = int(g); }
        else if (hits[g] > second) second = hits[g];
    }
}

std::string Matcher::source_summary() const {
    std::map<std::string, unsigned> m;
    for (size_t i = 0; i < seen_.size(); ++i) if (seen_[i]) ++m[t_.sigs[i].src];
    std::string s;
    for (const auto& kv : m) { if (!s.empty()) s += ','; s += kv.first + "=" + std::to_string(kv.second); }
    return s.empty() ? std::string("none") : s;
}

} // namespace acz::stagefp
