// E6-ANCHOR-FREE core (see ee_e6_locator.h). SDK-free.
#include "ee_e6_locator.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace acz::e6loc {
namespace {
constexpr double kPi = 3.14159265358979323846;
double wrap(double a) noexcept {
    if (!std::isfinite(a)) return a;
    a = std::fmod(a + kPi, 2 * kPi); if (a < 0) a += 2 * kPi; return a - kPi;
}
float rd(const uint8_t* p) noexcept { float f; std::memcpy(&f, p, 4); return f; }
template <size_t N, class... A> void appendf(char (&buf)[N], size_t& len, const char* fmt, A... a) noexcept {
    if (len >= N - 1) return;
    const int w = std::snprintf(buf + len, N - len, fmt, a...);
    if (w > 0) len = len + size_t(w) < N ? len + size_t(w) : N - 1;
}
unsigned long long ull(uint64_t v) noexcept { return static_cast<unsigned long long>(v); }
bool parse_mode(const char* v, size_t n, Mode& m) noexcept {
    auto is = [&](const char* s) { return std::strlen(s) == n && std::strncmp(v, s, n) == 0; };
    if (is("auto")) { m = Mode::automatic; return true; }
    if (is("probe")) { m = Mode::probe; return true; }
    if (is("content")) { m = Mode::content; return true; }
    return false;
}
} // namespace

const char* mode_name(Mode m) noexcept {
    switch (m) { case Mode::automatic: return "auto"; case Mode::probe: return "probe"; case Mode::content: return "content"; }
    return "?";
}

bool extract_mode(const std::string& value, std::string& rest, Mode& mode, std::string& why) {
    mode = Mode::automatic;
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
            if (eq != std::string::npos && item.compare(0, eq, "e6loc") == 0 && eq == 5) {
                ours = true;
                const std::string v = item.substr(eq + 1);
                if (!parse_mode(v.data(), v.size(), mode)) { why = "bad value for e6loc (auto|probe|content)"; return false; }
            }
        }
        if (!ours) { if (!first) rest += ';'; rest += item; }
        first = false;
        if (semi == value.size()) break;
    }
    return true;
}

Mode mode_in(const char* value, bool& ok) noexcept {
    ok = true; Mode m = Mode::automatic;
    if (!value) return m;
    const char* p = std::strchr(value, ';');
    while (p) {
        const char* item = p + 1;
        const char* end = std::strchr(item, ';');
        const size_t n = end ? size_t(end - item) : std::strlen(item);
        if (n > 6 && std::strncmp(item, "e6loc", 5) == 0 && (item[5] == ':' || item[5] == '=')) {
            if (!parse_mode(item + 6, n - 6, m)) { ok = false; return Mode::automatic; }
        } else if (n == 6 && std::strncmp(item, "e6loc", 5) == 0 && (item[5] == ':' || item[5] == '=')) { ok = false; return Mode::automatic; }
        p = end;
    }
    return m;
}

void decode(const uint8_t a[kWinABytes], const uint8_t b[kWinBBytes], Sample& s) noexcept {
    for (int i = 0; i < 3; ++i) { s.look[i] = rd(a + 4 * i); s.air[i] = rd(a + 0x20 + 4 * i); s.cam[i] = rd(b + 4 * i); s.rot[i] = rd(b + 0x10 + 4 * i); }
    s.heading = rd(a + 0x3C);
}

float eye_distance(const Sample& s) noexcept {
    const double d[3] = {double(s.cam[0]) - s.air[0], double(s.cam[1]) - s.air[1], double(s.cam[2]) - s.air[2]};
    return float(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
}
double eye_angle_deg(const Sample& s) noexcept {
    const double d[3] = {double(s.cam[0]) - s.air[0], double(s.cam[1]) - s.air[1], double(s.cam[2]) - s.air[2]};
    const double m = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(m > 0)) return 180.0;
    // Aircraft forward in the game's right-handed frame (D-297): (-sin h, 0, -cos h) at pitch 0; pitch from rot minus the look pitch
    // (positive = nose up, verified on the step56/step58 nose-down samples: 5.7 deg vs 54 deg with the opposite sign).
    const double h = s.heading, p = double(s.rot[0]) - double(s.look[0]);
    const double f[3] = {-std::sin(h) * std::cos(p), std::sin(p), -std::cos(h) * std::cos(p)};
    double c = (d[0] * f[0] + d[1] * f[1] + d[2] * f[2]) / m;
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    return std::acos(c) * 180.0 / kPi;
}

const char* check_sample(const Sample& s) noexcept {
    const float* all[] = {s.look, s.air, &s.heading, s.cam, s.rot};
    const int cnt[] = {3, 3, 1, 3, 3};
    for (int g = 0; g < 5; ++g) for (int i = 0; i < cnt[g]; ++i) if (!std::isfinite(all[g][i])) return "nonfinite";
    if (std::fabs(s.air[0]) + std::fabs(s.air[2]) <= 1.0f) return "aircraft-at-origin";
    // E6-JUNK-LOCK (2026-10-30): the content scan found world-origin test structures (cam 0,0,0, rot/heading 0, aircraft a few tens of units away) and locked them.
    if (std::fabs(s.cam[0]) + std::fabs(s.cam[2]) <= 1.0f) return "camera-at-origin";
    for (float v : s.look) if (std::fabs(double(v)) > kAngleMax) return "look-range";
    for (float v : s.rot) if (std::fabs(double(v)) > kAngleMax) return "rotation-range";
    if (std::fabs(double(s.heading)) > kHeadingMax) return "heading-range";
    const float lim = kWorldHalf + kWorldMargin;
    if (std::fabs(s.air[0]) > lim || std::fabs(s.air[2]) > lim || std::fabs(s.cam[0]) > lim || std::fabs(s.cam[2]) > lim) return "position-range";
    if (s.air[1] < kAltMin || s.air[1] > kAltMax) return "altitude-range";
    const float d = eye_distance(s);
    if (!(d >= kEyeMin && d <= kEyeMax)) return "eye-distance";
    if (eye_angle_deg(s) > kEyeAngleMaxDeg) return "eye-direction";
    if (std::fabs(wrap(double(s.rot[1]) - double(s.heading) - double(s.look[1]))) > kYawRelMax) return "yaw-relation";
    return nullptr;
}

const char* check_pair(const Sample& prev, const Sample& cur, double dt_ms) noexcept {
    const double dx = double(cur.air[0]) - prev.air[0], dy = double(cur.air[1]) - prev.air[1], dz = double(cur.air[2]) - prev.air[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double dt = dt_ms > 0 ? dt_ms : 0;
    if (dist > kMaxSpeed * dt / 1000.0 + kJumpSlack) return "jump";
    if (std::fabs(eye_distance(cur) - eye_distance(prev)) > kEyeDistJitter) return "eye-distance-unstable";
    return nullptr;
}

bool moved(const Sample& prev, const Sample& cur) noexcept {
    return std::memcmp(prev.air, cur.air, sizeof cur.air) != 0;
}

bool read_sample(ReadMem& m, uint64_t base, Sample& s) noexcept {
    if (!base || base + kWinB + kWinBBytes < base) return false;
    uint8_t a[kWinABytes], b[kWinBBytes];
    if (!m.read(base + kWinA, a, kWinABytes) || !m.read(base + kWinB, b, kWinBBytes)) return false;
    decode(a, b, s);
    return true;
}

// ------------------------------------------------------------------------------------------------ candidates
void CandidateBuilder::push(uint64_t b, char k) noexcept {
    for (unsigned i = 0; i < n_; ++i) if (base_[i] == b) return;
    if (n_ >= kCandidateCap) { ++dropped_; return; }
    base_[n_] = b; kind_[n_] = k; ++n_;
}
void CandidateBuilder::close() noexcept {
    if (!open_) return;
    open_ = false;
    if (ok_ && span_ == kBlockBytes && nreg_ > 1) push(start_, 'a');
}
void CandidateBuilder::add(const RegionDesc& r) noexcept {
    ++seen_;
    const bool good = r.committed && r.is_private && r.read_write;
    if (!open_ || r.alloc != alloc_) { close(); open_ = true; alloc_ = r.alloc; start_ = r.base; span_ = 0; ok_ = true; nreg_ = 0; }
    if (good && r.size == kBlockBytes) push(r.base, 'r');   // after closing the previous allocation: candidates stay in address order
    if (r.base != start_ + span_) ok_ = false;
    span_ += r.size; ++nreg_;
    ok_ = ok_ && good;
}
void CandidateBuilder::finish() noexcept { close(); }

// ------------------------------------------------------------------------------------------------ E6-RELOCATED: struct scan
// Full list: replaces the FIRST entry of the lowest score when `sc` beats it (address order kept among equals). True = inserted.
bool insert_best(uint64_t* bases, uint8_t* scores, unsigned n, uint64_t b, uint8_t sc) noexcept {
    if (!n) return false;
    unsigned lo = 0;
    for (unsigned i = 1; i < n; ++i) if (scores[i] < scores[lo]) lo = i;
    if (sc <= scores[lo]) return false;
    bases[lo] = b; scores[lo] = sc;
    return true;
}
int hit_score(const Sample& s) noexcept {
    // A flying aircraft: heading and rot yaw non-zero, pitch within +-90 deg, the game's look near rest. Junk: zeros, denormals, 2.0 fill words.
    return (std::fabs(s.heading) > 1e-6f) + (std::fabs(s.rot[1]) > 1e-6f) + (std::fabs(s.rot[0]) <= 1.6f)
        + (std::fabs(s.look[0]) <= 0.7f && std::fabs(s.look[1]) <= 0.7f && std::fabs(s.look[2]) <= 0.7f);
}

unsigned scan_struct(const uint8_t* buf, size_t len, uint64_t addr, uint64_t* bases, unsigned cap) noexcept {
    uint8_t sc[kMaxScoredCap];
    return scan_struct_scored(buf, len, addr, bases, sc, cap < kMaxScoredCap ? cap : kMaxScoredCap);
}

unsigned scan_struct_scored(const uint8_t* buf, size_t len, uint64_t addr, uint64_t* bases, uint8_t* scores, unsigned cap) noexcept {
    if (!buf || len < kStructBytes) return 0;
    unsigned n = 0, kept = 0;
    const float e2min = kEyeMin * kEyeMin, e2max = kEyeMax * kEyeMax;
    const size_t last = len - kStructBytes;
    for (size_t o = 0; o <= last; o += 4) {
        // Prefilter (cheap, implied by check_sample): the camera 15..60 units from the aircraft (NaN fails), the aircraft not at the origin.
        const uint8_t* a = buf + o;
        const float ax = rd(a + 0x20), ay = rd(a + 0x24), az = rd(a + 0x28);
        const float dx = rd(a + kWinGap) - ax, dy = rd(a + kWinGap + 4) - ay, dz = rd(a + kWinGap + 8) - az;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!(d2 >= e2min && d2 <= e2max)) continue;
        if (!(std::fabs(ax) + std::fabs(az) > 1.0f)) continue;
        Sample s; decode(a, a + kWinGap, s);
        if (check_sample(s)) continue;
        ++n;
        if (!bases || !scores || !cap) continue;
        const uint64_t b = addr + o - kWinA; const uint8_t sc = uint8_t(hit_score(s));
        if (kept < cap) { bases[kept] = b; scores[kept] = sc; ++kept; }
        else { insert_best(bases, scores, kept, b, sc); }
    }
    return n;
}

void StructScanner::start(const CandidateBuilder& b) noexcept {
    nb_ = 0;
    for (unsigned i = 0; i < b.count() && nb_ < kCandidateCap; ++i) if (b.kind(i) == 'r' || b.kind(i) == 'a') blocks_[nb_++] = b.base(i);
    bi_ = 0; off_ = 0; nc_ = 0; curSeen_ = 0; unread_ = 0; bytes_ = 0;
    if (!buf_) buf_.reset(new (std::nothrow) uint8_t[kChunk + kStructBytes]);
    active_ = nb_ > 0 && buf_ != nullptr;
}

void StructScanner::add_hits(const uint64_t* b, const uint8_t* sc, unsigned n, unsigned total, uint64_t block) noexcept {
    // E6-JUNK-LOCK: the cap keeps the BEST-scored hits (hit_score), not the first in address order: on m03k hundreds of junk hits ahead of the
    // real structure (+0xCA811C / +0xCAAC6C) filled the 12 slots and the real one was dropped.
    for (unsigned i = 0; i < n; ++i) {
        if (nc_ < kHitCap) { cur_[nc_] = b[i]; curBlock_[nc_] = block; curScore_[nc_] = sc[i]; ++nc_; }
        else {
            unsigned lo = 0;
            for (unsigned k = 1; k < nc_; ++k) if (curScore_[k] < curScore_[lo]) lo = k;
            if (sc[i] > curScore_[lo]) { cur_[lo] = b[i]; curBlock_[lo] = block; curScore_[lo] = sc[i]; }
        }
    }
    curSeen_ += total;
}

bool StructScanner::step(ReadMem& m, uint64_t budget) noexcept {
    if (!active_) return false;
    uint64_t used = 0;
    while (bi_ < nb_ && (used == 0 || used < budget)) {
        const uint64_t blk = blocks_[bi_];
        if (off_ + kStructBytes > kBlockBytes) { ++bi_; off_ = 0; continue; }
        const uint64_t want = kBlockBytes - off_ < uint64_t(kChunk) + kStructBytes - 4 ? kBlockBytes - off_ : uint64_t(kChunk) + kStructBytes - 4;
        if (m.read(blk + off_, buf_.get(), uint32_t(want))) {
            uint64_t tmp[kHitCap]; uint8_t tsc[kHitCap];
            const unsigned total = scan_struct_scored(buf_.get(), size_t(want), blk + off_, tmp, tsc, kHitCap);
            add_hits(tmp, tsc, total < kHitCap ? total : kHitCap, total, blk);
            bytes_ += want;
        } else ++unread_;
        used += want;
        off_ += kChunk;
    }
    if (bi_ < nb_) return false;
    // address order (stable for logs / the Locator's candidate list), whatever the replacement order was
    for (unsigned i = 1; i < nc_; ++i)
        for (unsigned j = i; j > 0 && cur_[j] < cur_[j - 1]; --j) {
            const uint64_t a = cur_[j]; cur_[j] = cur_[j - 1]; cur_[j - 1] = a;
            const uint64_t bl = curBlock_[j]; curBlock_[j] = curBlock_[j - 1]; curBlock_[j - 1] = bl;
        }
    for (unsigned i = 0; i < nc_; ++i) { hits_[i] = cur_[i]; hitBlock_[i] = curBlock_[i]; }
    nh_ = nc_; dropped_ = curSeen_ > nc_ ? curSeen_ - nc_ : 0; passUnread_ = unread_; passBytes_ = bytes_; ++passes_;
    active_ = false;
    return true;
}

// ------------------------------------------------------------------------------------------------ E6-RELOCATED: fallback gate
const char* FallbackGate::admit(uint64_t base, const Sample& s, double now) noexcept {
    if (base != base_) { reset(); base_ = base; }
    if (!base) return "no-base";
    if (const char* r = check_sample(s)) { streak_ = moves_ = 0; have_ = qualified_ = false; return r; }
    if (have_) {
        if (const char* r = check_pair(prev_, s, now - lastT_)) { streak_ = moves_ = 0; qualified_ = false; prev_ = s; lastT_ = now; return r; }
        if (moved(prev_, s)) ++moves_;
    }
    ++streak_; prev_ = s; have_ = true; lastT_ = now;
    if (!qualified_ && streak_ >= need_ && moves_ >= moves_need_) qualified_ = true;
    if (qualified_) return nullptr;
    return streak_ >= need_ ? "static" : "qualifying";
}

// ------------------------------------------------------------------------------------------------ locator
void Locator::count_reason(const char* r) noexcept {
    for (unsigned i = 0; i < kReasons; ++i) {
        if (rname_[i] == r || (rname_[i] && std::strcmp(rname_[i], r) == 0)) { ++rcount_[i]; return; }
        if (!rname_[i]) { rname_[i] = r; rcount_[i] = 1; return; }
    }
}

const char* Locator::sample_one(ReadMem& m, uint64_t base, double now, Sample& s, Sample& prev, bool& havePrev, double& lastT, bool& mv) noexcept {
    mv = false;
    if (!read_sample(m, base, s)) { havePrev = false; lastT = now; return "unreadable"; }
    const char* single = check_sample(s);
    const char* r = single;
    if (!r && havePrev) r = check_pair(prev, s, now - lastT);
    mv = havePrev && moved(prev, s);
    prev = s; havePrev = single == nullptr; lastT = now;
    return r;
}

// E6-ALIAS: does the last locked structure still read as a live camera? Valid, or refused only for its eye geometry (the chase / orbit camera is
// not the cockpit eye). Unreadable, zero (aircraft-at-origin), junk: not alive, the overlap rule is off (a structure that really moved may lock).
// Sampled at most every sample_ms; `bytes` = what this call read (0 or kSampleBytes).
bool Locator::home_alive(ReadMem& m, double now, uint32_t& bytes, bool may_read) noexcept {
    bytes = 0;
    if (!homeBase_) return false;
    if (!may_read || now - homeCheckT_ < o_.sample_ms) return homeAlive_;
    homeCheckT_ = now; bytes = kSampleBytes;
    Sample s;
    if (!read_sample(m, homeBase_, s)) { homeAlive_ = false; return false; }
    const char* r = check_sample(s);
    const bool geomOk = !r || std::strcmp(r, "eye-distance") == 0 || std::strcmp(r, "eye-direction") == 0 || std::strcmp(r, "yaw-relation") == 0;
    // A stale copy left behind by a structure that really moved does not move: only a MOVING home (or the first look at it) blocks its aliases.
    const bool moving = !homeHavePrev_ || std::memcmp(homePrev_.air, s.air, sizeof s.air) != 0 || std::memcmp(homePrev_.cam, s.cam, sizeof s.cam) != 0;
    homePrev_ = s; homeHavePrev_ = true;
    homeAlive_ = geomOk && moving;
    return homeAlive_;
}

void Locator::set_candidates(const CandidateBuilder& b, double now_ms, Sink& log) noexcept {
    bool changed = b.count() != nc_;
    for (unsigned i = 0; !changed && i < b.count(); ++i) changed = c_[i].base != b.base(i);
    haveCands_ = true; candAt_ = now_ms;
    if (!changed) return;
    Cand next[kCandidateCap]{};
    for (unsigned i = 0; i < b.count(); ++i) {
        next[i].base = b.base(i); next[i].kind = b.kind(i);
        for (unsigned j = 0; j < nc_; ++j) if (c_[j].base == next[i].base) { next[i] = c_[j]; next[i].kind = b.kind(i); }
    }
    for (unsigned i = 0; i < kCandidateCap; ++i) c_[i] = next[i];
    nc_ = b.count(); rr_ = 0;
    char t[448]; size_t n = 0;
    appendf(t, n, "e6-locate;event=candidates;n=%u;regions-seen=%u;dropped=%u;rule=private-rw-committed-64MiB(region:r,allocation:a);list=", nc_, b.regions_seen(), b.dropped());
    for (unsigned i = 0; i < nc_; ++i) appendf(t, n, "%s%llx:%c", i ? "," : "", ull(c_[i].base), c_[i].kind);
    log.line(false, t);
}

void Locator::tick(ReadMem& m, double now, Sink& log, TickOut& out) noexcept {
    out = TickOut{};
    char t[448];
    if (state_ == locked) {
        Sample s; bool mv = false;
        const char* r = sample_one(m, lockedBase_, now, s, lprev_, lhavePrev_, llastT_, mv);
        out.bytes = kSampleBytes; out.base = lockedBase_;
        if (!r) { out.valid = true; out.s = s; lastValid_ = now; ++valid_; }
        else {
            ++rejected_; count_reason(r); lastReason_ = r;
            if (std::strcmp(r, "jump") == 0) ++discont_;   // CAM-EPOCH: a locked teleport (the glue bumps the stage epoch)
            const double since = now - lastValid_;
            if (since >= o_.lose_ms) {
                std::snprintf(t, sizeof t, "e6-locate;event=lost;base=%llx;kind=%c;reason=%s;no-valid-ms=%.0f;fallback=no-camera;action=search", ull(lockedBase_), lockedKind_, r, since);
                log.line(true, t);
                state_ = searching; ++losses_; haveCands_ = false; lockedBase_ = 0; lhavePrev_ = false;
                for (unsigned i = 0; i < nc_; ++i) { c_[i].streak = 0; c_[i].moves = 0; c_[i].havePrev = false; c_[i].why = "after-loss"; }
            } else if (since < o_.hold_ms) out.hold = true;
        }
    } else {
        uint32_t budget = o_.tick_byte_cap;
        const unsigned start = rr_;
        for (unsigned k = 0; k < nc_; ++k) {
            const unsigned i = (start + k) % nc_;
            Cand& c = c_[i];
            if (now - c.lastT < o_.sample_ms) continue;
            if (budget < kSampleBytes) { rr_ = i; break; }
            budget -= kSampleBytes; out.bytes += kSampleBytes;
            // E6-ALIAS: a candidate overlapping the last locked base (a shifted read of the same structure) never builds a streak while that
            // structure still reads as a live camera (the chase view: valid memory, only its eye geometry refused).
            if (homeBase_ && c.base != homeBase_ && (c.base > homeBase_ ? c.base - homeBase_ : homeBase_ - c.base) < kStructBytes) {
                uint32_t hb = 0;
                const bool alive = home_alive(m, now, hb, budget >= kSampleBytes);   // over the tick cap: the last verdict
                out.bytes += hb; budget -= hb;
                if (alive) {
                    c.lastT = now; c.havePrev = false; c.streak = 0; c.moves = 0; c.why = "alias-of-locked-base"; ++c.rejects; ++rejected_; ++aliasRefused_;
                    count_reason("alias-of-locked-base");
                    rr_ = (i + 1) % nc_;
                    continue;
                }
            }
            Sample s; bool mv = false;
            const char* r = sample_one(m, c.base, now, s, c.prev, c.havePrev, c.lastT, mv);
            if (!r) {
                ++c.streak; if (mv) ++c.moves;
                c.why = c.streak >= o_.lock_samples && c.moves < o_.lock_moves ? "static" : "valid";
            } else { c.streak = 0; c.moves = 0; c.why = r; ++c.rejects; ++rejected_; count_reason(r); }
            rr_ = (i + 1) % nc_;
        }
        // Lock: exactly one candidate qualifies AND no other one is a moving structure still building its streak (a second live copy
        // sampled a tick later must make it ambiguous, not lose the race). Static decoys (no moves) never block.
        unsigned q = 0, which = 0, building = 0;
        for (unsigned i = 0; i < nc_; ++i) {
            if (c_[i].streak >= o_.lock_samples && c_[i].moves >= o_.lock_moves) { ++q; which = i; }
            else if (c_[i].streak >= 2 && c_[i].moves >= 1) ++building;
        }
        if (q == 1 && building) {
            // wait: the other moving candidate either qualifies too (ambiguous) or fails a check within a few samples
        } else if (q == 1) {
            Cand& c = c_[which];
            state_ = locked; lockedBase_ = c.base; lockedKind_ = c.kind; lprev_ = c.prev; lhavePrev_ = c.havePrev; llastT_ = c.lastT; lastValid_ = now; ++locks_;
            homeBase_ = c.base; homeCheckT_ = -1e300; homeHavePrev_ = false;   // E6-ALIAS: the home structure of the overlap rule
            const Sample& s = c.prev;
            std::snprintf(t, sizeof t, "e6-locate;event=locked;base=%llx;kind=%c;samples=%u;moves=%u;air=%.2f,%.2f,%.2f;heading=%.5f;cam=%.2f,%.2f,%.2f;rot=%.5f,%.5f,%.5f;"
                "eye-dist=%.3f;eye-angle-deg=%.2f;candidates=%u;look-events-needed=0;read-only=1",
                ull(c.base), c.kind, c.streak, c.moves, double(s.air[0]), double(s.air[1]), double(s.air[2]), double(s.heading), double(s.cam[0]), double(s.cam[1]),
                double(s.cam[2]), double(s.rot[0]), double(s.rot[1]), double(s.rot[2]), double(eye_distance(s)), eye_angle_deg(s), nc_);
            log.line(true, t);
            if (c.lastT == now) { out.valid = true; out.s = s; out.base = c.base; ++valid_; }
        } else if (q >= 2 && now - lastAmbig_ >= o_.search_log_ms) {
            lastAmbig_ = now;
            size_t n = 0; appendf(t, n, "e6-locate;event=ambiguous;qualifying=%u;action=no-lock;bases=", q);
            for (unsigned i = 0; i < nc_; ++i) if (c_[i].streak >= o_.lock_samples && c_[i].moves >= o_.lock_moves) appendf(t, n, "%llx:%c,", ull(c_[i].base), c_[i].kind);
            log.line(false, t);
        }
        if (state_ == searching && now - lastSearchLog_ >= o_.search_log_ms) {
            lastSearchLog_ = now;
            size_t n = 0; appendf(t, n, "e6-locate;event=search;candidates=%u;need=%u-samples/%u-moves", nc_, o_.lock_samples, o_.lock_moves);
            for (unsigned i = 0; i < nc_; ++i) appendf(t, n, ";c%u=%llx:%c:%u/%u:%s", i, ull(c_[i].base), c_[i].kind, c_[i].streak, c_[i].moves, c_[i].why);
            log.line(false, t);
        }
    }
    if (out.bytes > maxTickBytes_) maxTickBytes_ = out.bytes;
}

void Locator::maybe_summary(double now, Sink& log, const char* mode) noexcept {
    if (now - lastSummary_ < o_.summary_ms) return;
    lastSummary_ = now;
    char t[448]; size_t n = 0;
    appendf(t, n, "e6-locate;event=summary;mode=%s;state=%s;base=%llx;locks=%llu;losses=%llu;valid=%llu;rejected=%llu;fallback-published=%llu;fallback-rejected=%llu;"
        "no-camera-ticks=%llu;max-tick-bytes=%u;lock-epoch=%u;discontinuities=%llu;reasons=", mode, state_ == locked ? "locked" : "searching", ull(lockedBase_), ull(locks_),
        ull(losses_), ull(valid_), ull(rejected_), ull(fallback_published), ull(fallback_rejected), ull(no_camera_ticks), maxTickBytes_, lock_epoch(), ull(discont_));
    bool any = false;
    for (unsigned i = 0; i < kReasons && rname_[i]; ++i) { appendf(t, n, "%s%s:%llu", any ? "," : "", rname_[i], ull(rcount_[i])); any = true; }
    if (!any) appendf(t, n, "none");
    log.line(false, t);
}

} // namespace acz::e6loc
