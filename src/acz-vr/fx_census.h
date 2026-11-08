#pragma once
// fx_census.h -- EFFECTS-OUTSIDE (D) fxmem:3: identify the game's EXPLOSION and GUN-TRACER draws (docs/world-render/effects-outside-window.md section 9).
// Header only, SDK-free. LOG ONLY: nothing here reads game memory, records a command or changes a replay decision.
//
// What the archived logs already show (section 9.1): the effects candidate table (`continuous-hud-effects-ps-candidates`) of every session in which a
// missile HIT something (headset tests 9-17 and 19, null runs step41/step50) lists a pixel shader the class gate never replays:
//   542a11d7 (hash 542a11d7386a6c000682563efb79f16d, 2,948 B) through the generic sprite VS 534f29ca (ps2_v), depth GREATER, blend on, no depth write, indexed,
// in bursts of 4..180 draws per 5 s that start while missile smoke is present; it is ABSENT from every run where missiles missed or nothing was fired
// (step83 runs X, P, Q, Z1, E10; step69 gun-only runs). Its `rep` is always 0 (`class-ps-not-allowlisted`): the explosion candidate never reaches the
// effects image, which matches headset test 19 "no explosions at all". A second unknown, 7a3c9385 (3,072 B, same VS, depth ALWAYS, non-indexed), appears
// WITHOUT smoke (overlay class: never an effect candidate). Gun tracers have no draw of their own (D-318): the candidate is the small GREATER 4caf6e2d
// sprite family (a stored frame holds a 152-unit tracer-like streak in it), already replayed into the window effects image.
//
// What is missing: the blend tuple of 542a11d7 (the dual-source effects path refuses anything but 5,6,1/18,19,1), its topology / counts / vertex stride /
// root layout, per-frame timing against the gun key and kills, and whether the 4caf6e2d GREATER index count rises while the gun fires. The census logs these
// per pipeline SIGNATURE (pixel + vertex shader prefix, blend tuple, depth/stencil state, topology, indexed, vertex stride, root-layout shape):
//   fx-census-onset  : a signature drawing again after >= kGapFrames frames without it (or for the first time): frame id, gap, the frame's draws/counts.
//   fx-census        : once per window (1 s) per active signature (<= kLinesPerWindow, most draws first): frames, draws, per-frame max, count range, ...
//   fx-census-window : the window summary (frames, draws, signatures, overflow, onsets dropped).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::fxcensus {

inline constexpr uint32_t kPsSmoke = 0xd2464ed2u, kPsSprite = 0x4caf6e2du, kPsHudSprite = 0xd34ee153u;
inline constexpr uint32_t kPsExplosionCand = 0x542a11d7u;   // section 9.1: appears only with missile hits; never replayed
inline constexpr uint32_t kPsOverlayCand = 0x7a3c9385u;     // section 9.1: depth ALWAYS overlay without smoke (sun flare / screen flash?)
inline constexpr uint32_t kFuncGreater = 5, kFuncGreaterEqual = 7;   // D3D12_COMPARISON_FUNC

enum Class : uint8_t { cls_other = 0, cls_smoke, cls_cloud, cls_sprite, cls_explosion_cand, cls_overlay_cand, cls_hud_sprite, cls_count };
inline const char* class_name(uint8_t c) noexcept {
    static constexpr const char* n[] = {"other", "smoke", "cloud", "sprite(tracer-cand)", "explosion-cand", "overlay-cand", "hud-sprite"};
    return c < cls_count ? n[c] : "?";
}

// Everything the census keeps about one draw (filled by the producer from the HUD seam's Shape, the bound PSO's blend tuple and the root layout).
struct DrawFacts {
    uint32_t ps8 = 0, vs8 = 0;
    uint8_t blend[6] = {};             // RT0 src, dst, op, srcA, dstA, opA (D3D12_BLEND / D3D12_BLEND_OP)
    uint8_t write_mask = 0;
    bool blend_on = false, depth_enable = false, depth_write = false, stencil = false, indexed = false, pso_known = false;
    uint32_t depth_func = 0, topology = 0;
    uint32_t count = 0, instances = 0, vb_stride = 0;
    // Root layout shape: parameter count, kinds (constants / root views / tables) and a hash over (kind, visibility, register, space, constants).
    uint8_t root_n = 0, root_c = 0, root_v = 0, root_t = 0; uint32_t root_hash = 0; bool root_known = false;
    const char* gate = nullptr;        // the effects class gate's verdict (nullptr = passed effects_decide; else its reason)
};

inline uint8_t classify(const DrawFacts& d) noexcept {
    switch (d.ps8) {
    case kPsSmoke: return cls_smoke;
    case kPsSprite: return d.depth_func == kFuncGreaterEqual ? cls_cloud : cls_sprite;
    case kPsExplosionCand: return cls_explosion_cand;
    case kPsOverlayCand: return cls_overlay_cand;
    case kPsHudSprite: return cls_hud_sprite;
    default: return cls_other;
    }
}

// FNV-1a over the identity fields (everything but the counts and the gate verdict).
inline uint64_t signature_key(const DrawFacts& d) noexcept {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint32_t v) { for (int i = 0; i < 4; ++i) { h ^= (v >> (8 * i)) & 0xffu; h *= 1099511628211ull; } };
    mix(d.ps8); mix(d.vs8);
    for (uint8_t b : d.blend) mix(b);
    mix(d.write_mask); mix(uint32_t(d.blend_on) | uint32_t(d.depth_enable) << 1 | uint32_t(d.depth_write) << 2 | uint32_t(d.stencil) << 3 |
                           uint32_t(d.indexed) << 4 | uint32_t(d.pso_known) << 5 | uint32_t(d.root_known) << 6);
    mix(d.depth_func); mix(d.topology); mix(d.vb_stride);
    mix(uint32_t(d.root_n) | uint32_t(d.root_c) << 8 | uint32_t(d.root_v) << 16 | uint32_t(d.root_t) << 24); mix(d.root_hash);
    return h;
}

// Root layout hash helper (the producer feeds one parameter at a time).
struct RootShape {
    uint8_t n = 0, c = 0, v = 0, t = 0; uint32_t hash = 2166136261u; bool known = false;
    // kind: 0 table, 1 root constants, 2..4 root CBV / SRV / UAV (acz::metadata::ParameterKind order: table, constants, then views).
    void add(uint32_t kind, uint32_t visibility, uint32_t reg, uint32_t space, uint32_t constants) noexcept {
        if (n < 255) ++n;
        if (kind == 0) { if (t < 255) ++t; } else if (kind == 1) { if (c < 255) ++c; } else if (v < 255) ++v;
        const uint32_t w[5] = {kind, visibility, reg, space, constants};
        for (uint32_t x : w) for (int i = 0; i < 4; ++i) { hash ^= (x >> (8 * i)) & 0xffu; hash *= 16777619u; }
        known = true;
    }
};

struct Sig {
    uint64_t key = 0; DrawFacts first{}; uint8_t cls = cls_other;
    // this frame
    uint32_t f_draws = 0; uint64_t f_count = 0;
    // window
    uint32_t w_frames = 0, w_draws = 0, w_frame_max = 0, w_cnt_min = 0, w_cnt_max = 0, w_inst_max = 0; uint64_t w_cnt_sum = 0; const char* w_gate = nullptr;
    // run
    uint64_t t_frames = 0, t_draws = 0, first_frame = 0, last_frame = 0;
};
struct Onset { uint64_t frame = 0, gap = 0; uint32_t sig = 0, draws = 0; uint64_t count = 0; bool fresh = false; };
struct WindowSummary { uint64_t frame = 0, ms = 0; uint32_t frames = 0, draws = 0, sigs = 0, active = 0, onsets = 0; uint64_t overflow = 0, onsets_dropped = 0; };

class Census {
public:
    static constexpr unsigned kSigCap = 48, kOnsetCap = 16, kLinesPerWindow = 12;
    static constexpr uint64_t kGapFrames = 30, kWindowMs = 1000;

    // A frame armed but never accounted (abandoned) leaves its draws in the window counts only; its per-frame counts are dropped here.
    void begin_frame() noexcept { for (unsigned i = 0; i < n_; ++i) { sig_[i].f_draws = 0; sig_[i].f_count = 0; } f_draws_ = 0; open_ = true; }
    bool open() const noexcept { return open_; }
    // One draw of the open frame (no-op when no frame is open). Over the signature cap: counted, not stored.
    void add(const DrawFacts& d) noexcept {
        if (!open_) return;
        ++f_draws_;
        const uint64_t k = signature_key(d);
        Sig* s = nullptr;
        for (unsigned i = 0; i < n_; ++i) if (sig_[i].key == k) { s = &sig_[i]; break; }
        if (!s) {
            if (n_ >= kSigCap) { ++overflow_; return; }
            s = &sig_[n_++]; *s = Sig{}; s->key = k; s->first = d; s->cls = classify(d);
        }
        ++s->f_draws; s->f_count += d.count;
        if (s->w_draws == 0) { s->w_cnt_min = d.count; s->w_cnt_max = d.count; }
        if (d.count < s->w_cnt_min) s->w_cnt_min = d.count;
        if (d.count > s->w_cnt_max) s->w_cnt_max = d.count;
        if (d.instances > s->w_inst_max) s->w_inst_max = d.instances;
        ++s->w_draws; s->w_cnt_sum += d.count;
        if (!s->w_gate || d.gate == nullptr) s->w_gate = d.gate ? d.gate : "candidate";
    }
    // Frame end (published frame `frame`, wall clock `now_ms`). Returns true when a window closed (report() is fresh; the caller emits it).
    bool end_frame(uint64_t frame, uint64_t now_ms) noexcept {
        if (!open_) return false;
        open_ = false; ++w_frames_; w_draws_ += f_draws_; f_draws_ = 0;
        for (unsigned i = 0; i < n_; ++i) {
            Sig& s = sig_[i];
            if (!s.f_draws) continue;
            ++s.w_frames; if (s.f_draws > s.w_frame_max) s.w_frame_max = s.f_draws;
            const bool fresh = s.t_frames == 0;
            const uint64_t gap = fresh ? 0 : frame - s.last_frame;
            if (fresh || gap > kGapFrames) {
                if (on_n_ < kOnsetCap) { Onset& o = on_[on_n_++]; o.frame = frame; o.gap = gap; o.sig = i; o.draws = s.f_draws; o.count = s.f_count; o.fresh = fresh; ++w_onsets_; }
                else ++onsets_dropped_;
            }
            if (fresh) s.first_frame = frame;
            ++s.t_frames; s.t_draws += s.f_draws; s.last_frame = frame;
        }
        if (!w_start_ms_) w_start_ms_ = now_ms;
        if (now_ms - w_start_ms_ < kWindowMs) return false;
        close_window(frame, now_ms);
        return true;
    }
    // The last closed window: its signatures (most draws first, at most kLinesPerWindow) and summary.
    unsigned report_count() const noexcept { return rep_n_; }
    const Sig& report(unsigned i) const noexcept { return rep_[i]; }
    const WindowSummary& summary() const noexcept { return sum_; }
    unsigned onset_count() const noexcept { return on_n_; }
    const Onset& onset(unsigned i) const noexcept { return on_[i]; }
    const Sig& sig(unsigned i) const noexcept { return sig_[i]; }
    unsigned sig_count() const noexcept { return n_; }
    void clear_onsets() noexcept { on_n_ = 0; }
    uint64_t overflow() const noexcept { return overflow_; }

private:
    void close_window(uint64_t frame, uint64_t now_ms) noexcept {
        sum_ = WindowSummary{}; sum_.frame = frame; sum_.ms = now_ms - w_start_ms_; sum_.frames = w_frames_; sum_.draws = w_draws_; sum_.sigs = n_;
        sum_.overflow = overflow_; sum_.onsets = w_onsets_; sum_.onsets_dropped = onsets_dropped_;
        rep_n_ = 0;
        for (unsigned i = 0; i < n_; ++i) {
            Sig& s = sig_[i];
            if (s.w_draws) {
                ++sum_.active;
                // insertion: effect classes before the HUD sprites (run S3: the many hud-sprite signatures pushed the tracer candidates out of the 12
                // lines), then by draws (descending), bounded
                auto before = [](const Sig& a, const Sig& b) {   // a ranks before b
                    const bool ha = a.cls == cls_hud_sprite, hb = b.cls == cls_hud_sprite;
                    return ha != hb ? hb : a.w_draws > b.w_draws;
                };
                unsigned pos = rep_n_;
                while (pos > 0 && before(s, rep_[pos - 1])) --pos;
                if (pos < kLinesPerWindow) {
                    const unsigned last = rep_n_ < kLinesPerWindow ? rep_n_ : kLinesPerWindow - 1;
                    for (unsigned j = last; j > pos; --j) rep_[j] = rep_[j - 1];
                    rep_[pos] = s; if (rep_n_ < kLinesPerWindow) ++rep_n_;
                }
            }
            s.w_frames = s.w_draws = s.w_frame_max = s.w_cnt_min = s.w_cnt_max = s.w_inst_max = 0; s.w_cnt_sum = 0; s.w_gate = nullptr;
        }
        w_frames_ = 0; w_draws_ = 0; w_onsets_ = 0; w_start_ms_ = now_ms;
    }
    Sig sig_[kSigCap]{}; unsigned n_ = 0; uint64_t overflow_ = 0;
    Onset on_[kOnsetCap]{}; unsigned on_n_ = 0; uint64_t onsets_dropped_ = 0;
    Sig rep_[kLinesPerWindow]{}; unsigned rep_n_ = 0; WindowSummary sum_{};
    uint32_t f_draws_ = 0, w_frames_ = 0, w_draws_ = 0, w_onsets_ = 0; uint64_t w_start_ms_ = 0; bool open_ = false;
};

// ------------------------------------------------------------------ log lines (each < 480 bytes for any values; offline-tested)
inline int format_identity(char* b, size_t n, const DrawFacts& d) noexcept {
    return std::snprintf(b, n, "sig=ps%08x/vs%08x;blend=%s:%u,%u,%u/%u,%u,%u:m%x;depth=%s%u:w%u;sten=%u;topo=%u;idx=%u;vbs=%u;roots=%s:n%u:c%u:v%u:t%u:h%08x",
        unsigned(d.ps8), unsigned(d.vs8), d.pso_known ? (d.blend_on ? "on" : "off") : "unknown", d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.blend[4], d.blend[5],
        unsigned(d.write_mask), d.depth_enable ? "f" : "off-f", unsigned(d.depth_func), unsigned(d.depth_write), unsigned(d.stencil), unsigned(d.topology),
        unsigned(d.indexed), unsigned(d.vb_stride), d.root_known ? "known" : "unknown", unsigned(d.root_n), unsigned(d.root_c), unsigned(d.root_v),
        unsigned(d.root_t), unsigned(d.root_hash));
}
inline void format_sig(char* b, size_t n, uint64_t frame, const Sig& s) noexcept {
    if (!b || !n) return;
    int k = std::snprintf(b, n, "fx-census;frame=%llu;class=%s;", static_cast<unsigned long long>(frame), class_name(s.cls));
    if (k < 0 || size_t(k) >= n) return;
    int k2 = format_identity(b + k, n - size_t(k), s.first);
    if (k2 < 0 || size_t(k += k2) >= n) return;
    char gate[32]; std::snprintf(gate, sizeof gate, "%s", s.w_gate ? s.w_gate : "none");
    std::snprintf(b + k, n - size_t(k), ";frames=%u;draws=%u;per-frame-max=%u;cnt=%u-%u(sum:%llu);inst-max=%u;gate=%s;total=%llu/%llu;first-frame=%llu",
        s.w_frames, s.w_draws, s.w_frame_max, s.w_cnt_min, s.w_cnt_max, static_cast<unsigned long long>(s.w_cnt_sum), s.w_inst_max, gate,
        static_cast<unsigned long long>(s.t_frames), static_cast<unsigned long long>(s.t_draws), static_cast<unsigned long long>(s.first_frame));
}
inline void format_onset(char* b, size_t n, const Census& c, const Onset& o) noexcept {
    if (!b || !n) return;
    const Sig& s = c.sig(o.sig);
    int k = std::snprintf(b, n, "fx-census-onset;frame=%llu;class=%s;gap=%s%llu;draws=%u;cnt-sum=%llu;", static_cast<unsigned long long>(o.frame),
        class_name(s.cls), o.fresh ? "new:" : "", static_cast<unsigned long long>(o.gap), o.draws, static_cast<unsigned long long>(o.count));
    if (k < 0 || size_t(k) >= n) return;
    format_identity(b + k, n - size_t(k), s.first);
}
inline void format_window(char* b, size_t n, const WindowSummary& w) noexcept {
    if (!b || !n) return;
    std::snprintf(b, n, "fx-census-window;frame=%llu;ms=%llu;frames=%u;draws=%u;sigs=%u(cap:%u);active=%u;lines<=%u;overflow=%llu;onsets=%u;onsets-dropped=%llu;"
        "mode=identify-log-only(no-replay)", static_cast<unsigned long long>(w.frame), static_cast<unsigned long long>(w.ms), w.frames, w.draws, w.sigs,
        Census::kSigCap, w.active, Census::kLinesPerWindow, static_cast<unsigned long long>(w.overflow), w.onsets, static_cast<unsigned long long>(w.onsets_dropped));
}

// ------------------------------------------------------------------ fxsig:<ps8> per-draw log (docs section 12; live batch G follow-up)
// The census above is per SIGNATURE and capped (12 lines per 1 s window); the gun tracers (ps d34ee153 / vs 534f29ca, additive, strip) were filed under the
// HUD class and had no positions. fxsig logs EVERY draw of one pixel-shader prefix, one line each (< 480 bytes), for kLogMs after an onset (the first draw ever,
// or the first one after > kQuietFrames frames without any), never subject to the census line cap. Pure policy + formatter here; the producer
// (gpu_copy_step5_fxcensus.inl) reads the vertices and queues the lines, the worker writes them.
struct SigLog {
    static constexpr uint64_t kQuietFrames = 30, kLogMs = 3000;
    bool seen = false; uint64_t cur_frame = 0, last_frame = 0, onset_ms = 0, onsets = 0, logged = 0, suppressed = 0; unsigned n = 0;
    // One matching draw of published-frame `frame` at wall clock `now_ms`. true = log it; `nth` = its index among the matching draws of this frame (0-based).
    bool admit(uint64_t frame, uint64_t now_ms, unsigned& nth) noexcept {
        if (!seen || frame != cur_frame) {
            const bool onset = !seen || frame - last_frame > kQuietFrames;   // > 30 frames apart = >= 30 quiet frames in between
            if (onset) { onset_ms = now_ms; ++onsets; }
            seen = true; cur_frame = frame; last_frame = frame; n = 0;
        }
        nth = n++;
        if (now_ms - onset_ms <= kLogMs) { ++logged; return true; }
        ++suppressed; return false;
    }
};
// What one logged draw carries (all producer copies; nothing here points into game memory).
struct SigDraw {
    uint64_t frame = 0, ord = 0; unsigned nth = 0; uint32_t ps8 = 0, vs8 = 0;
    bool indexed = false; uint32_t count = 0, instances = 0, start = 0; int32_t base = 0;
    uint64_t vb_va = 0, vb_res = 0, vb_bytes = 0, ib_va = 0; uint32_t vb_stride = 0, ib_stride = 0, topology = 0;
    uint8_t blend[6] = {}; uint8_t write_mask = 0; bool pso_known = false, blend_on = false, depth_enable = false, depth_write = false, stencil = false;
    uint32_t depth_func = 0; bool tracer_ready = false;
    const char* gate = nullptr;            // effects class gate verdict (nullptr = passed)
    unsigned nidx = 0; uint32_t idx[4] = {};   // the first indices read (indexed draws)
    unsigned nvtx = 0; float pos[4][4] = {};   // the first vertices' raw position float4 (ps2_v: x_s, y_s, z_s, q)
    const char* read_why = nullptr;        // named reason when the vertices could not be read (fail closed)
    char roots[72] = {};                  // root-parameter summary (see gpu_copy_step5_fxcensus.inl), may be empty
};
// One line, always < 426 bytes (+ the 21-byte prefix < the 448-byte step-1 text, < the feature log 480) (offline-tested with extreme values). Returns the length.
inline int format_sig_draw(char* b, size_t n, const SigDraw& d, uint64_t dropped) noexcept {
    if (!b || n < 2) return 0;
    int k = std::snprintf(b, n, "fx-sig-draw;f=%llu;ord=%llu;n=%u;sig=ps%08x/vs%08x;gate=%.20s;%s%u;in=%u;st=%u;bs=%d;vb=%llx(res:%llx,s%u);ib=%u;"
        "bl=%s:%u,%u,%u/%u,%u,%u:m%x;d=%s%u:w%u;sten=%u;topo=%u;trc=%u;",
        static_cast<unsigned long long>(d.frame), static_cast<unsigned long long>(d.ord), d.nth, unsigned(d.ps8), unsigned(d.vs8), d.gate ? d.gate : "pass",
        d.indexed ? "idx=" : "cnt=", d.count, d.instances, d.start, int(d.base), static_cast<unsigned long long>(d.vb_va), static_cast<unsigned long long>(d.vb_res),
        d.vb_stride, d.ib_stride,
        d.pso_known ? (d.blend_on ? "on" : "off") : "unk", d.blend[0], d.blend[1], d.blend[2], d.blend[3], d.blend[4], d.blend[5], unsigned(d.write_mask),
        d.depth_enable ? "f" : "off-f", unsigned(d.depth_func), unsigned(d.depth_write), unsigned(d.stencil), unsigned(d.topology), unsigned(d.tracer_ready));
    if (k < 0 || size_t(k) >= n) { b[n - 1] = 0; return int(n - 1); }
    size_t len = size_t(k);
    char t[160];
    auto put = [&](int r) { if (r <= 0) return; const size_t w = size_t(r) < sizeof t ? size_t(r) : sizeof t - 1; if (len + w < n) { std::memcpy(b + len, t, w); len += w; b[len] = 0; } };
    if (d.nidx) {
        put(std::snprintf(t, sizeof t, "i="));
        for (unsigned i = 0; i < d.nidx && i < 4; ++i) put(std::snprintf(t, sizeof t, "%s%u", i ? "," : "", d.idx[i]));
        put(std::snprintf(t, sizeof t, ";"));
    }
    if (d.nvtx) { for (unsigned i = 0; i < d.nvtx && i < 4; ++i) put(std::snprintf(t, sizeof t, "v%u=%.6g,%.6g,%.6g,%.6g;", i, double(d.pos[i][0]), double(d.pos[i][1]), double(d.pos[i][2]), double(d.pos[i][3]))); }
    else put(std::snprintf(t, sizeof t, "vtx-why=%.32s;", d.read_why ? d.read_why : "none"));
    put(std::snprintf(t, sizeof t, "roots=%.68s", d.roots[0] ? d.roots : "none"));
    if (dropped) put(std::snprintf(t, sizeof t, ";dropped=%llu", static_cast<unsigned long long>(dropped)));
    return int(len);
}
// A bounded FIFO of formatted lines (producer pushes under the step-1 mutex, the worker pops under it): full = dropped (counted, reported on the next line).
struct SigQueue {
    static constexpr unsigned kCap = 40, kLine = 426;   // 426 = diagnostics::text_capacity (448) - the 21-byte "gpu-copy-step1;event=" prefix - 1: the whole log line stays inside the step-1 text buffer
    char line[kCap][kLine]; unsigned head = 0, count = 0; uint64_t dropped = 0, dropped_reported = 0;
    bool push(const SigDraw& d) noexcept {
        if (count >= kCap) { ++dropped; return false; }
        char* dst = line[(head + count) % kCap];
        format_sig_draw(dst, kLine, d, dropped - dropped_reported); dropped_reported = dropped;
        ++count; return true;
    }
    const char* front() const noexcept { return count ? line[head] : nullptr; }
    void pop() noexcept { if (count) { head = (head + 1) % kCap; --count; } }
};

} // namespace acz::fxcensus
