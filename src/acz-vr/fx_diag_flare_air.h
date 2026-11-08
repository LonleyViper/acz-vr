#pragma once
// fx_diag_flare_air.h -- READ-ONLY live diagnostics for two open questions (headset test 2026-10-28): (1) the sun lens flare that renders black with a dark
// ring (docs/world-render/headset-0-3-4-9-10-analysis.md item 3), (2) the aircraft reflection/shadow passes (aircraft-shading-reflection.md section 5).
// Header only, SDK-free. LOG ONLY: nothing here records a command, reads game memory (the producer reads the draw's own mapped vertices through the
// existing guarded reader) or changes a replay decision. Default off.
//
// Arming (key-neutral, no new option key, no extra characters beyond the existing fxsig value): the stage option `fxsig:<8 hex>` already exists (fxmem >= 1;
// the value is stored in effects_sig_filter()). Two reserved sentinel values select these logs instead of a pixel-shader prefix:
//   fxsig:f1a4e000  -> flare log   (fx-flare-draw lines)
//   fxsig:ac0f1a60  -> aircraft log (fx-air-draw lines)
// Neither value equals a real pixel-shader prefix in use (the real fxsig draw log compares ps8 == value and simply never matches them).
// Only one of the two can be armed per run (one fxsig value); arm the one under test.
//
// Start gate (live batch K, step113: the 400-line run cap was used up on the runway, 60 s before takeoff): the Limiter counts nothing and logs nothing until it
// is ARMED, by either (a) the trigger file %LOCALAPPDATA%\acz-vr\fxdiag.arm being created or touched after the first poll (a file already present at the first
// poll is ignored; the autopilot touches it at `autopilot start` = airborne), or (b) kArmDelayMs after the first draw it considered (fallback; default 90 s).
// Arming resets the run and window counters, so the full 400 lines are available in flight.
//
// Flare log policy (batch M rework, FlareTracker below): the first live run logged 400 lines that were all HUD instrument draws, so the cap was gone before the
// sun was in view. No flight detection is needed or used. The flare log is per SIGNATURE (ps8, vs8, blend tuple, depth, coarse bbox bucket): the first sighting
// of a signature is always logged, repeats at most once per kGapMs (kCandGapMs for candidates) and at most kPerSigTotal (kPerSigCandidate) times, and a periodic
// fx-flare-summary / fx-flare-sig block gives every signature's counts and admitted-image routing. See docs/world-render/headset-0-3-4-9-10-analysis.md.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::fxdiag {

inline constexpr uint32_t kArmFlare = 0xf1a4e000u, kArmAir = 0xac0f1a60u;
inline constexpr uint32_t kFuncAlways = 8;              // D3D12_COMPARISON_FUNC_ALWAYS
inline constexpr uint32_t kBlendOne = 2;                // D3D12_BLEND_ONE
inline constexpr unsigned kLineMax = 426;               // 448 (step-1 text) - 21 ("gpu-copy-step1;event=") - 1; also < the 480-byte log bound

// ------------------------------------------------------------------ rate limiter
// Per 1-second window: at most kReadsPerWindow vertex reads, kLinesPerWindow lines, kPerSigLines lines per signature (kPerSigAdditive for additive ones),
// and kRunLines lines per run. Everything else is counted (`sup=` on the next line) and dropped.
struct Limiter {
    static constexpr unsigned kReadsPerWindow = 400, kLinesPerWindow = 24, kPerSigLines = 6, kPerSigAdditive = 12, kSigCap = 24;
    static constexpr uint64_t kRunLines = 400, kWindowMs = 1000;
    static constexpr uint64_t kArmDelayMs = 90000, kPollMs = 1000;
    uint64_t win_start = 0, run_lines = 0, suppressed = 0, suppressed_reported = 0, considered = 0;
    unsigned reads = 0, lines = 0, nsig = 0;
    // start gate
    uint64_t arm_delay_ms = kArmDelayMs, first_ms = 0, armed_ms = 0, held = 0, last_poll = 0, base_stamp = 0; bool armed = false, trigger = false, polled = false;
    // Trigger-file stamp (0 = absent); the first observation is the baseline and never arms. Later: created or changed => trigger.
    void observe_stamp(uint64_t stamp) noexcept {
        if (!polled) { polled = true; base_stamp = stamp; return; }
        if (stamp && stamp != base_stamp) trigger = true;
    }
    // true when the caller should stat the trigger file now (at most once per kPollMs; never once armed)
    bool poll_due(uint64_t now_ms) noexcept {
        if (armed) return false;
        if (last_poll && now_ms - last_poll < kPollMs) return false;
        last_poll = now_ms ? now_ms : 1; return true;
    }
    // true = the diagnostic is armed. Before that every draw is only counted in `held`.
    bool gate(uint64_t now_ms) noexcept {
        if (armed) return true;
        if (!first_ms) first_ms = now_ms ? now_ms : 1;
        if (trigger || now_ms - first_ms >= arm_delay_ms) {
            armed = true; armed_ms = now_ms; run_lines = 0; suppressed = suppressed_reported = 0; reads = lines = nsig = 0; win_start = 0; return true;
        }
        ++held; return false;
    }
    struct S { uint64_t key = 0; unsigned lines = 0; } sig[kSigCap];
    void roll(uint64_t now_ms) noexcept {
        if (!win_start || now_ms - win_start >= kWindowMs) { win_start = now_ms ? now_ms : 1; reads = 0; lines = 0; nsig = 0; }
    }
    // true = this draw may have its vertices read (a budget, not a log decision)
    bool allow_read(uint64_t now_ms) noexcept {
        ++considered; if (!gate(now_ms)) return false; roll(now_ms);
        if (reads >= kReadsPerWindow) return false;
        ++reads; return true;
    }
    // true = log the line
    bool allow_line(uint64_t now_ms, uint64_t sig_key, bool additive) noexcept {
        if (!gate(now_ms)) return false;
        roll(now_ms);
        if (run_lines >= kRunLines || lines >= kLinesPerWindow) { ++suppressed; return false; }
        S* s = nullptr;
        for (unsigned i = 0; i < nsig; ++i) if (sig[i].key == sig_key) { s = &sig[i]; break; }
        if (!s) { if (nsig >= kSigCap) { ++suppressed; return false; } s = &sig[nsig++]; s->key = sig_key; s->lines = 0; }
        if (s->lines >= (additive ? kPerSigAdditive : kPerSigLines)) { ++suppressed; return false; }
        ++s->lines; ++lines; ++run_lines; return true;
    }
    uint64_t take_suppressed() noexcept { const uint64_t d = suppressed - suppressed_reported; suppressed_reported = suppressed; return d; }
};

// ------------------------------------------------------------------ bounding box
struct Bbox {
    unsigned n = 0; float minx = 0, miny = 0, maxx = 0, maxy = 0;
    void add(float x, float y) noexcept {
        if (!(x == x) || !(y == y)) return;   // NaN
        if (!n) { minx = maxx = x; miny = maxy = y; }
        else { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
        ++n;
    }
};
// GS units: the PS2 screen is 640x448 around the primitive offset (2048,2048) in GS coordinates, so the screen centre is (2048,2048) in raw GS space and
// (320,224) once the offset is removed. The proxy's vertices have not been shown to be one or the other, so a bbox is "near the centre" when it comes within
// kCentreRadius of EITHER point. The log carries the raw bbox, so the question is answered from the numbers either way.
inline constexpr float kCentreRadius = 160.0f;
inline bool near_point(const Bbox& b, float px, float py, float r) noexcept {
    if (!b.n) return false;
    const float dx = px < b.minx ? b.minx - px : px > b.maxx ? px - b.maxx : 0.0f;
    const float dy = py < b.miny ? b.miny - py : py > b.maxy ? py - b.maxy : 0.0f;
    return dx * dx + dy * dy <= r * r;
}
inline bool near_centre(const Bbox& b) noexcept { return near_point(b, 2048.0f, 2048.0f, kCentreRadius) || near_point(b, 320.0f, 224.0f, kCentreRadius); }

// ------------------------------------------------------------------ flare
struct FlareFacts {
    uint32_t ps8 = 0, vs8 = 0; uint8_t blend[6] = {}; bool blend_on = false, depth_enable = false, pso_known = false;
    uint32_t depth_func = 0, count = 0, instances = 0; bool indexed = false;
};
inline bool depth_always(const FlareFacts& f) noexcept { return !f.depth_enable || f.depth_func == kFuncAlways; }
// Additive: blending on and the colour destination factor ONE (SRC_ALPHA/ONE or ONE/ONE).
inline bool additive(const FlareFacts& f) noexcept { return f.pso_known && f.blend_on && f.blend[1] == kBlendOne; }
// Cheap pre-filter before any vertex is read: only depth-ALWAYS draws can be the flare sprite.
inline bool flare_prefilter(const FlareFacts& f) noexcept { return f.pso_known && depth_always(f); }
// After the bbox: the draw is logged when it is additive or near the centre (the budget limiter then caps the lines).
inline bool flare_candidate(const FlareFacts& f, const Bbox& b) noexcept { return flare_prefilter(f) && (additive(f) || near_centre(b)); }
inline uint64_t flare_sig_key(const FlareFacts& f) noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint32_t v) { for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xffu; h *= 1099511628211ull; } };
    mix(f.ps8); mix(f.vs8); for (uint8_t b : f.blend) mix(b); mix(f.depth_func); mix(uint32_t(f.blend_on));
    return h;
}
struct FlareLine {
    uint64_t frame = 0, ord = 0; FlareFacts f; Bbox box; unsigned nvtx = 0;
    const char* verdict = "?";       // "admitted" or hud_decide's reason / the region-skip reason
    const char* image = "none";      // "hud" | "markers" | "none"
    const char* hud_class = "?";     // instrument / marker-box / marker-label
    bool depth_tested = false; const char* read_why = nullptr; uint64_t sup = 0;
    const char* tag = "seen";        // "flare-candidate" | "new" | "seen" (tracker policy)
    unsigned score = 0; const char* reasons = ""; uint32_t sg = 0; uint64_t n = 0;   // reasons: A additive, L large, C centre, I intermittent, N late-new
};
inline int format_flare(char* b, size_t n, const FlareLine& d) noexcept {
    if (!b || n < 2) return 0;
    const int k = std::snprintf(b, n,
        "fx-flare-draw;f=%llu;ord=%llu;tag=%.16s;sg=%08x;sc=%u;rs=%.6s;n=%llu;ps=%08x;vs=%08x;bl=%s:%u,%u,%u/%u,%u,%u;add=%u;d=%s%u;cnt=%u;in=%u;idx=%u;nv=%u;"
        "bbox=%.6g,%.6g,%.6g,%.6g;ctr=%u;hud=%.40s;img=%.8s;cls=%.16s;dt=%u;why=%.24s;sup=%llu",
        static_cast<unsigned long long>(d.frame), static_cast<unsigned long long>(d.ord), d.tag ? d.tag : "seen", unsigned(d.sg), d.score,
        d.reasons ? d.reasons : "", static_cast<unsigned long long>(d.n), unsigned(d.f.ps8), unsigned(d.f.vs8),
        d.f.pso_known ? (d.f.blend_on ? "on" : "off") : "unk", d.f.blend[0], d.f.blend[1], d.f.blend[2], d.f.blend[3], d.f.blend[4], d.f.blend[5],
        unsigned(additive(d.f)), d.f.depth_enable ? "f" : "off-f", unsigned(d.f.depth_func), d.f.count, d.f.instances, unsigned(d.f.indexed), d.nvtx,
        double(d.box.minx), double(d.box.miny), double(d.box.maxx), double(d.box.maxy), unsigned(near_centre(d.box)),
        d.verdict ? d.verdict : "?", d.image ? d.image : "none", d.hud_class ? d.hud_class : "?", unsigned(d.depth_tested),
        d.read_why ? d.read_why : "ok", static_cast<unsigned long long>(d.sup));
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}

// ------------------------------------------------------------------ signature tracker (flare log policy)
// Coarse bbox bucket = size class (0..7) << 5 | 4x4 centre cell (0..15; 16 = centre outside the 640x448 screen; 31 = no bbox). The centre is normalised from
// raw GS space (2048 offset) to screen space first, so both coordinate conventions bucket alike.
inline bool bbox_large(const Bbox& b) noexcept {
    if (!b.n) return false;
    const float w = b.maxx - b.minx, h = b.maxy - b.miny;
    return w * h >= 17920.0f || w >= 320.0f || h >= 320.0f;      // >= 1/16 of the 640x448 screen, or a long side >= half the width
}
inline unsigned bbox_bucket(const Bbox& b) noexcept {
    if (!b.n) return 31u;
    const float w = b.maxx - b.minx, h = b.maxy - b.miny, ext = w > h ? w : h;
    const unsigned sc = ext < 16.0f ? 0u : ext < 48.0f ? 1u : ext < 96.0f ? 2u : ext < 192.0f ? 3u : ext < 384.0f ? 4u : ext < 768.0f ? 5u : ext < 2048.0f ? 6u : 7u;
    float cx = (b.minx + b.maxx) * 0.5f, cy = (b.miny + b.maxy) * 0.5f;
    if (cx >= 1024.0f) cx -= 1728.0f;
    if (cy >= 1024.0f) cy -= 1824.0f;
    unsigned cell = 16u;
    if (cx >= 0.0f && cx < 640.0f && cy >= 0.0f && cy < 448.0f) cell = unsigned(cx / 160.0f) * 4u + unsigned(cy / 112.0f);
    return (sc << 5) | cell;
}
// HUD classes (step5 hud_decide cls strings) that are the HUD's own instruments / labels / text: never flare candidates.
inline bool hud_chrome_class(const char* c) noexcept {
    return c && (std::strcmp(c, "instrument") == 0 || std::strcmp(c, "marker-label") == 0 || std::strcmp(c, "hud-text") == 0);
}
// W8-3 F1: the diagnostic class of a draw reaching the flare logger. The replay classifier's `hud_instrument` is its FALLBACK ("everything else"), so
// passing it verbatim marked every caller-reachable depth-ALWAYS draw as chrome and no draw could ever become a candidate. Only POSITIVELY identified
// chrome stays chrome: a label (the text draw right after a marker box) and a text draw (same PSO route as the learned text route). A draw that is
// neither a marker box nor a known label/text is "unclassified" and can be scored (L/C/I) and tagged as a candidate.
inline const char* flare_diag_class(bool is_box, bool is_label, bool is_known_text) noexcept {
    return is_box ? "marker-box" : is_label ? "marker-label" : is_known_text ? "hud-text" : "unclassified";
}
inline uint64_t flare_bucket_key(const FlareFacts& f, unsigned bucket) noexcept {
    uint64_t h = flare_sig_key(f);
    for (int i = 0; i < 2; ++i) { h ^= (bucket >> (8 * i)) & 0xffu; h *= 1099511628211ull; }
    h ^= uint64_t(f.depth_enable); h *= 1099511628211ull;
    return h;
}
struct FlareTracker {
    static constexpr unsigned kSigCap = 64, kPerSigTotal = 3, kPerSigCandidate = 12, kGapEpochs = 2, kLinesPerWindow = 24, kSummaryTop = 8;
    static constexpr uint64_t kGapMs = 30000, kCandGapMs = 5000, kLateMs = 30000, kSummaryMs = 20000, kWindowMs = 1000;
    static constexpr uint64_t kRunLines = 400, kRunLinesNonCandidate = 250;
    struct Sig {
        uint64_t key = 0, count = 0, first_ms = 0, last_ms = 0, last_log_ms = 0;
        unsigned logged = 0, runs = 0, epochs = 0, last_epoch = 0, bucket = 0, score = 0;
        uint64_t n_hud = 0, n_markers = 0, n_none = 0;
        uint32_t ps8 = 0, vs8 = 0, depth_func = 0; uint8_t blend[6] = {}; bool blend_on = false, depth_enable = false, additive = false, large = false, centre = false;
        bool candidate = false, used = false, chrome = false; Bbox box;
        char verdict[24] = {}, cls[16] = {};
    };
    struct Obs { bool log = false, candidate = false, first = false; unsigned score = 0; char reasons[8] = {}; const Sig* sig = nullptr; };
    Sig sig[kSigCap];
    bool started = false; uint64_t arm_ms = 0, win_start = 0, last_summary = 0, last_summary_draws = 0;
    unsigned win_lines = 0, nsig = 0;
    uint64_t draws = 0, unread = 0, overflow = 0, evictions = 0, run_lines = 0, cand_lines = 0, summaries = 0;

    void note_unread() noexcept { ++unread; }
    Sig* find_or_add(uint64_t key, uint64_t now, bool& fresh) noexcept {
        fresh = false;
        for (unsigned i = 0; i < kSigCap; ++i) if (sig[i].used && sig[i].key == key) return &sig[i];
        Sig* v = nullptr;
        for (unsigned i = 0; i < kSigCap; ++i) if (!sig[i].used) { v = &sig[i]; break; }
        if (!v) {   // full: evict the least recently seen non-candidate
            for (unsigned i = 0; i < kSigCap; ++i) if (!sig[i].candidate && (!v || sig[i].last_ms < v->last_ms)) v = &sig[i];
            if (!v) { ++overflow; return nullptr; }
            ++evictions;
        } else ++nsig;
        *v = Sig(); v->used = true; v->key = key; v->first_ms = now; fresh = true;
        return v;
    }
    // One observed (vertex-read) HUD draw. Decides whether to log it and fills the Obs for the line.
    Obs observe(uint64_t now, const FlareFacts& f, const Bbox& b, const char* verdict, const char* image, const char* cls) noexcept {
        Obs o;
        if (!started) { started = true; arm_ms = now; win_start = now; last_summary = now; }
        ++draws;
        if (now - win_start >= kWindowMs) { win_start = now; win_lines = 0; }
        const unsigned epoch = unsigned((now - arm_ms) / kWindowMs), bucket = bbox_bucket(b);
        bool fresh = false;
        Sig* s = find_or_add(flare_bucket_key(f, bucket), now, fresh);
        if (!s) return o;
        bool newrun = false;
        if (fresh) {
            s->runs = 1; s->epochs = 1; s->last_epoch = epoch; s->bucket = bucket; s->ps8 = f.ps8; s->vs8 = f.vs8; s->depth_func = f.depth_func;
            std::memcpy(s->blend, f.blend, 6); s->blend_on = f.blend_on; s->depth_enable = f.depth_enable; s->additive = additive(f);
        } else if (epoch != s->last_epoch) {
            ++s->epochs; if (epoch - s->last_epoch >= kGapEpochs) { ++s->runs; newrun = true; } s->last_epoch = epoch;
        }
        ++s->count; s->last_ms = now; s->box = b;
        if (image && image[0] == 'h') ++s->n_hud; else if (image && image[0] == 'm') ++s->n_markers; else ++s->n_none;
        std::snprintf(s->verdict, sizeof s->verdict, "%s", verdict ? verdict : "?");
        std::snprintf(s->cls, sizeof s->cls, "%s", cls ? cls : "?");
        s->large = bbox_large(b); s->centre = near_centre(b); s->chrome = hud_chrome_class(cls);
        const bool A = s->additive, L = s->large, C = s->centre, I = s->runs >= 2, N = fresh && (now - arm_ms) >= kLateMs;
        const bool base = A || depth_always(f);
        s->candidate = base && !s->chrome && (L || C || I);   // additive alone is not enough (the HUD is drawn additive); instrument/label/text never qualify
        s->score = (A ? 4u : 0u) + (L ? 2u : 0u) + (C ? 2u : 0u) + (I ? 3u : 0u) + (N ? 1u : 0u);
        o.candidate = s->candidate; o.score = s->score; o.first = fresh; o.sig = s;
        { char* r = o.reasons; if (A) *r++ = 'A'; if (L) *r++ = 'L'; if (C) *r++ = 'C'; if (I) *r++ = 'I'; if (N) *r++ = 'N'; *r = 0; }
        const unsigned kmax = s->candidate ? kPerSigCandidate : kPerSigTotal;
        const uint64_t gap = s->candidate ? kCandGapMs : kGapMs;
        // W8-3 F2: EVERY repeat honours the documented spacing (kGapMs / kCandGapMs since the last logged line); a new run (newrun) only feeds the
        // score / the I reason, it no longer bypasses the interval (intermittent signatures could spend their allowance in seconds).
        (void)newrun;
        const bool need = s->logged == 0 || (s->logged < kmax && now - s->last_log_ms >= gap);
        const bool room = win_lines < kLinesPerWindow && run_lines < (s->candidate ? kRunLines : kRunLinesNonCandidate);
        if (need && room) {
            o.log = true; ++s->logged; s->last_log_ms = now ? now : 1; ++win_lines; ++run_lines; if (s->candidate) ++cand_lines;
        }
        return o;
    }
    bool summary_due(uint64_t now) const noexcept { return started && now - last_summary >= kSummaryMs && draws != last_summary_draws; }
    void summary_taken(uint64_t now) noexcept { last_summary = now; last_summary_draws = draws; ++summaries; }
    // Indices of the top signatures: candidates first, then by score, then by draw count. Returns the number written (<= kSummaryTop).
    unsigned top(unsigned* out) const noexcept {
        bool picked[kSigCap] = {}; unsigned k = 0;
        for (; k < kSummaryTop; ++k) {
            int best = -1;
            for (unsigned i = 0; i < kSigCap; ++i) {
                if (!sig[i].used || picked[i]) continue;
                if (best < 0) { best = int(i); continue; }
                const Sig& a = sig[i]; const Sig& c = sig[best];
                if (a.candidate != c.candidate ? a.candidate : a.score != c.score ? a.score > c.score : a.count > c.count) best = int(i);
            }
            if (best < 0) break;
            picked[best] = true; out[k] = unsigned(best);
        }
        return k;
    }
    unsigned candidates() const noexcept { unsigned c = 0; for (const Sig& s : sig) if (s.used && s.candidate) ++c; return c; }
};
inline int format_flare_summary(char* b, size_t n, const FlareTracker& t, uint64_t now, uint64_t queue_dropped) noexcept {
    if (!b || n < 2) return 0;
    const int k = std::snprintf(b, n, "fx-flare-summary;t=%llu;draws=%llu;unread=%llu;sigs=%u;cand=%u;ovf=%llu;evict=%llu;lines=%llu;candlines=%llu;qdrop=%llu;seq=%llu",
        static_cast<unsigned long long>(now - t.arm_ms), static_cast<unsigned long long>(t.draws), static_cast<unsigned long long>(t.unread), t.nsig, t.candidates(),
        static_cast<unsigned long long>(t.overflow), static_cast<unsigned long long>(t.evictions), static_cast<unsigned long long>(t.run_lines),
        static_cast<unsigned long long>(t.cand_lines), static_cast<unsigned long long>(queue_dropped), static_cast<unsigned long long>(t.summaries));
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}
inline int format_flare_sig(char* b, size_t n, const FlareTracker::Sig& s, unsigned rank, uint64_t now, uint64_t arm_ms) noexcept {
    if (!b || n < 2) return 0;
    const unsigned long long ago = (now - s.last_ms) / 1000;
    const int k = std::snprintf(b, n,
        "fx-flare-sig;t=%llu;r=%u;sg=%08x;tag=%s;sc=%u;n=%llu;lg=%u;runs=%u;ep=%u;gone=%u;ps=%08x;vs=%08x;bl=%s:%u,%u,%u/%u,%u,%u;add=%u;d=%s%u;bk=%x;lrg=%u;ctr=%u;"
        "bbox=%.6g,%.6g,%.6g,%.6g;adm=hud:%llu,mk:%llu,no:%llu;last=%.24s;cls=%.16s;seen=%llu-%llus",
        static_cast<unsigned long long>(now - arm_ms), rank, unsigned(s.key & 0xffffffffu), s.candidate ? "flare-candidate" : "other", s.score, static_cast<unsigned long long>(s.count), s.logged, s.runs, s.epochs,
        unsigned(ago >= 3), unsigned(s.ps8), unsigned(s.vs8), s.blend_on ? "on" : "off", s.blend[0], s.blend[1], s.blend[2], s.blend[3], s.blend[4], s.blend[5],
        unsigned(s.additive), s.depth_enable ? "f" : "off-f", unsigned(s.depth_func), s.bucket, unsigned(s.large), unsigned(s.centre),
        double(s.box.minx), double(s.box.miny), double(s.box.maxx), double(s.box.maxy),
        static_cast<unsigned long long>(s.n_hud), static_cast<unsigned long long>(s.n_markers), static_cast<unsigned long long>(s.n_none), s.verdict, s.cls,
        static_cast<unsigned long long>((s.first_ms - arm_ms) / 1000), static_cast<unsigned long long>((s.last_ms - arm_ms) / 1000));
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}

// ------------------------------------------------------------------ aircraft
struct AirLine {
    uint64_t frame = 0, ord = 0; bool cpt = false;
    uint32_t ps8 = 0, vs8 = 0, count = 0, instances = 0; bool indexed = false;
    bool rc_available = false; uint32_t rc_mask = 0, rc[4] = {};
    bool psoKnown = false, blend = false, depth_enable = false, depth_write = false; uint32_t bl[6] = {}, depth_func = 0, write_mask = 0;
    uint64_t rt = 0, rtv = 0; uint64_t rt_w = 0; uint32_t rt_h = 0, rt_fmt = 0; float vp_w = 0, vp_h = 0;
    char roots[132] = {};
    uint64_t sup = 0;
};
// flags (cb10 dword 0): bit 0 = reflection (sphere-map) path, bit 1 = int normals, bit 2 = no fog (aircraft-shading-reflection.md s2.1).
inline bool air_reflect(const AirLine& a) noexcept { return a.rc_available && (a.rc_mask & 1u) && (a.rc[0] & 1u); }
inline int format_air(char* b, size_t n, const AirLine& a) noexcept {
    if (!b || n < 2) return 0;
    char fl[40];
    if (a.rc_available && (a.rc_mask & 1u)) std::snprintf(fl, sizeof fl, "%u(0x%x)", a.rc[0], a.rc[0]); else std::snprintf(fl, sizeof fl, "unknown");
    const int k = std::snprintf(b, n,
        "fx-air-draw;f=%llu;ord=%llu;k=%s;flags=%s;refl=%u;rcm=%x;rc1=%x,%x,%x;ps=%08x;vs=%08x;bl=%s:%u,%u,%u/%u,%u,%u:m%x;d=%s%u:w%u;cnt=%u;in=%u;idx=%u;"
        "rt=%llx(%llux%u:f%u);rtv=%llx;vp=%.5gx%.5g;roots=%.100s;sup=%llu",
        static_cast<unsigned long long>(a.frame), static_cast<unsigned long long>(a.ord), a.cpt ? "cpt" : "acm", fl, unsigned(air_reflect(a)), a.rc_mask,
        a.rc[1], a.rc[2], a.rc[3], unsigned(a.ps8), unsigned(a.vs8), a.psoKnown ? (a.blend ? "on" : "off") : "unk", a.bl[0], a.bl[1], a.bl[2], a.bl[3], a.bl[4],
        a.bl[5], a.write_mask, a.depth_enable ? "f" : "off-f", a.depth_func, unsigned(a.depth_write), a.count, a.instances, unsigned(a.indexed),
        static_cast<unsigned long long>(a.rt), static_cast<unsigned long long>(a.rt_w), a.rt_h, a.rt_fmt, static_cast<unsigned long long>(a.rtv),
        double(a.vp_w), double(a.vp_h), a.roots[0] ? a.roots : "none", static_cast<unsigned long long>(a.sup));
    if (k < 0) { b[0] = 0; return 0; }
    if (size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    return k;
}

// ------------------------------------------------------------------ bounded line queue
struct LineQueue {
    static constexpr unsigned kCap = 32;
    char line[kCap][kLineMax]; unsigned head = 0, count = 0; uint64_t dropped = 0;
    bool push_flare(const FlareLine& d) noexcept {
        if (count >= kCap) { ++dropped; return false; }
        format_flare(line[(head + count) % kCap], kLineMax, d); ++count; return true;
    }
    bool push_air(const AirLine& d) noexcept {
        if (count >= kCap) { ++dropped; return false; }
        format_air(line[(head + count) % kCap], kLineMax, d); ++count; return true;
    }
    bool push_text(const char* t) noexcept {
        if (count >= kCap) { ++dropped; return false; }
        std::snprintf(line[(head + count) % kCap], kLineMax, "%s", t); ++count; return true;
    }
    const char* front() const noexcept { return count ? line[head] : nullptr; }
    void pop() noexcept { if (count) { head = (head + 1) % kCap; --count; } }
};

} // namespace acz::fxdiag
