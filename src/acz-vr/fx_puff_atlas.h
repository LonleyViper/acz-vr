#pragma once
// fx_puff_atlas.h -- EFFECTS-OUTSIDE (M6, headset test 7 2026-10-31 "clouds popping in and out"; docs/world-render/effects-outside-window.md section 23):
// a PERSISTENT TEXTURE ATLAS of our own for the cards of the world-space puff memory (fx_puff_trail.h), so that a cached cloud / smoke card can be drawn per
// eye in ANY frame that has a class-1 host draw of the same game PSO, not only in a frame where some game draw binds that card's texture handle (the F1 key
// rule of section 20.4 / 22.4: `unb`). SDK-free policy + maths (the D3D12 recording lives in gpu_copy_step5_fxsprite.inl; offline-tested).
//
// The two GPU steps (both recorded inline in the game's command list, right before a host draw that the effects layer replays per eye):
//   * CAPTURE, at a draw that binds key K (valid by construction: the game's own draw reads K a moment later): for a stored card of key K sighted in this
//     draw (stored, refreshed or renewed by a cut sighting) that has no ready tile, a tile is taken and the card's uv rectangle of K is resampled into it
//     (kContent x kContent texels plus a kGutter border on every side, the border continuing the source uv so that bilinear filtering at the rectangle's
//     edge reads what the game's sampler would read there). Pipeline: the copy PSO = the GAME's root signature + our fx_puff_atlas.hlsl vs_copy / ps_copy
//     (ps_copy = t0.Sample(s0, uv): the game's own SRV table and static sampler, never a descriptor of ours), render target = the atlas (RGBA16F), no
//     blend, no depth. Barrier atlas PIXEL_SHADER_RESOURCE -> RENDER_TARGET before, back after: the atlas is PIXEL_SHADER_RESOURCE between our uses in
//     every command list (order-independent across lists).
//   * DRAW, at any host draw of the same game PSO: the cards whose own key is not bound by a game draw this frame are drawn from their tiles: the host's
//     clone PSO (the game's VS / PS / blend: same alpha, fog and dual-source test), the eye copies' uv remapped into the tile (map_uv), the root table that
//     holds t0 pointed at OUR shader-visible SRV heap (SetDescriptorHeaps(ours), the table), then the game's heaps and the host's key set back before the
//     game's own draw. (M6b) Only under a root signature whose other descriptor tables are SAMPLER tables (host_why; the live 4caf6e2d signature is root
//     signature 310: b0, b1, the t0 table and an s0 SAMPLER table): the game's sampler heap stays bound next to ours and every table is set again
//     (fx_puff_atlas_d3d12.h record_group).
// Lifetime / residency (fail closed; docs 23.3):
//   * the atlas, its RTV heap and its SRV heap are created ONCE per process (late, by the worker tick, when fxatlas is on and the sprite ring is ready), on
//     the ring's device, charged against kBudget, never released (process lifetime, as every ring resource); a device whose fence reports UINT64_MAX
//     disables the producer (no recording reads or writes the atlas again); a copy PSO made on another device is refused at the host (atlas-device).
//   * no game resource is ever held: the capture reads K only inside the draw that binds it, the draw from the atlas reads only our SRV; the game heaps we
//     bind back are the ones the game has bound in that list at that point.
//   * a tile is READY only once the recording that captured it was SUBMITTED (slot_submitted): a recording that is Reset unsubmitted never makes its tiles
//     ready (they are recaptured: dead_capture), and a recording recorded before that submission never reads the tile.
//   * a tile is REUSED for another (key, rect) only when every recording that captured or read it has been retired: the producer's slot of that recording
//     has been picked again (step5PickSlot_locked picks a slot only after its last GPU use completed and no live owned recording -- one that could be
//     re-executed -- writes it). A quarantined slot is never picked again: its tiles are never reused (fail closed, counted as `stuck`).
//   * the budget: kTiles tiles; a full atlas refuses the capture (`full`), the card keeps the M5 key path. Tiles are shared by every card of the same key
//     and uv rectangle; a ready tile is refreshed in place (same key, same rectangle) at most every kRefreshFrames while its cards are sighted.
// Limits [I]: the tile is a resample of the card's rectangle at kContent texels (exact when the rectangle spans kContent source texels; magnified or
// minified otherwise: a 64-texel rectangle is upsampled 2x, which the game's bilinear filter then reads back within ~1/255 except at sharp texel steps);
// no mip chain (a far card that the game would draw from a smaller mip aliases slightly); a key whose texture the game rewrites keeps the content of the
// last capture until the next refresh (the same as the own-key path, which draws whatever the key holds).
#include "fx_memory.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::fxpuff::atlas {

using fxmem::Vtx;

inline constexpr unsigned kContent = 128;                         // texels of the card's uv rectangle per tile side
inline constexpr unsigned kGutter = 2;                            // border texels on every side (bilinear reads +-1)
inline constexpr unsigned kPitch = kContent + 2 * kGutter;        // 132
inline constexpr unsigned kCols = 16, kRows = 8, kTiles = kCols * kRows;   // 128 tiles
inline constexpr unsigned kWidth = kCols * kPitch, kHeight = kRows * kPitch;   // 2112 x 1056
inline constexpr uint32_t kFormat = 10;                           // DXGI_FORMAT_R16G16B16A16_FLOAT (linear storage: sRGB / UNORM / float sources alike)
inline constexpr unsigned kTexelBytes = 8;
inline constexpr uint64_t kBytes = uint64_t(kWidth) * kHeight * kTexelBytes;   // 17,842,176 B
inline constexpr uint64_t kBudget = 24ull << 20;                  // the atlas (+ its two one-page heaps) must fit
static_assert(kBytes <= kBudget, "the atlas fits its budget");
inline constexpr unsigned kSlotCap = 4;                           // producer slots tracked (gpu_copy::step5::slots = 3)
inline constexpr uint64_t kRefreshFrames = 120;                   // a ready tile whose cards are sighted is recaptured at most every 2 s (60 Hz)
inline constexpr unsigned kCapPerDraw = 8, kCapPerFrame = 32;     // captures recorded per host draw / per frame
struct CopyVtx { float x, y, u, v; };                             // fx_puff_atlas.hlsl vs_copy: POSITION (clip xy), TEXCOORD (source uv)
inline constexpr unsigned kCopyVerts = 6;                         // one TRIANGLELIST quad per capture
inline constexpr unsigned kCopyStride = sizeof(CopyVtx);          // 16
inline constexpr uint64_t kCopyBytes = uint64_t(kCopyVerts) * kCopyStride;   // 96 B per capture
inline constexpr uint64_t kCopyRegion = 65536;                    // the capture vertices of one slot's recording (own UPLOAD block, one region per slot)
inline constexpr unsigned kTableMax = 8;                          // SRV descriptors of our shader-visible heap (all = the atlas)
inline constexpr unsigned kMaxTables = 4;                         // descriptor-table root parameters a host may have (all restored)
inline constexpr uint64_t kGroupKey = ~0ull;                      // the eye-copy group key of the cards drawn from the atlas (sorts last)
inline constexpr float kMaxUv = 64.0f;                            // |uv| beyond this is not a texture rectangle we can tile
static_assert(kCopyBytes * kCapPerFrame <= kCopyRegion && kCopyRegion % 65536 == 0, "a frame's capture quads fit the copy region of a slot");

// The card's uv rectangle (the bounding box of its 4 template uvs).
struct Rect {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool operator==(const Rect& b) const noexcept { return u0 == b.u0 && v0 == b.v0 && u1 == b.u1 && v1 == b.v1; }
    bool operator!=(const Rect& b) const noexcept { return !(*this == b); }
};
inline bool rect_of(const Vtx t[4], Rect& r) noexcept {
    r = Rect{t[0].uv[0], t[0].uv[1], t[0].uv[0], t[0].uv[1]};
    for (int k = 0; k < 4; ++k) {
        const float u = t[k].uv[0], v = t[k].uv[1];
        if (!std::isfinite(u) || !std::isfinite(v) || std::fabs(u) > kMaxUv || std::fabs(v) > kMaxUv) return false;
        r.u0 = std::fmin(r.u0, u); r.u1 = std::fmax(r.u1, u); r.v0 = std::fmin(r.v0, v); r.v1 = std::fmax(r.v1, v);
    }
    return r.u1 - r.u0 > 1e-6f && r.v1 - r.v0 > 1e-6f;
}
// The texel origin of tile i in the atlas.
inline void tile_origin(unsigned i, unsigned& x, unsigned& y) noexcept { x = (i % kCols) * kPitch; y = (i / kCols) * kPitch; }
// Source uv (u, v) of a card whose rectangle is r -> the atlas uv of tile i (the gutter is outside the rectangle's image).
inline void map_uv(unsigned i, const Rect& r, float u, float v, float& au, float& av) noexcept {
    unsigned x = 0, y = 0; tile_origin(i, x, y);
    const double fu = (double(u) - r.u0) / (double(r.u1) - r.u0), fv = (double(v) - r.v0) / (double(r.v1) - r.v0);
    au = float((double(x) + kGutter + fu * kContent) / kWidth);
    av = float((double(y) + kGutter + fv * kContent) / kHeight);
}
// The capture quad of tile i (TRIANGLELIST, the viewport = the whole atlas): clip-space corners of the tile's pitch square, the source uv of the
// rectangle extended by the gutter (texel centre j + 0.5 of the tile <- source uv r.u0 + (j + 0.5 - kGutter) / kContent x (u1 - u0), as map_uv inverts).
inline void copy_quad(unsigned i, const Rect& r, CopyVtx out[kCopyVerts]) noexcept {
    unsigned x = 0, y = 0; tile_origin(i, x, y);
    const float x0 = float(double(x) / kWidth * 2.0 - 1.0), x1 = float(double(x + kPitch) / kWidth * 2.0 - 1.0);
    const float y0 = float(1.0 - double(y) / kHeight * 2.0), y1 = float(1.0 - double(y + kPitch) / kHeight * 2.0);
    const double du = (double(r.u1) - r.u0) / kContent, dv = (double(r.v1) - r.v0) / kContent;
    const float ua = float(r.u0 - kGutter * du), ub = float(r.u1 + kGutter * du), va = float(r.v0 - kGutter * dv), vb = float(r.v1 + kGutter * dv);
    const CopyVtx c[4] = {{x0, y0, ua, va}, {x1, y0, ub, va}, {x0, y1, ua, vb}, {x1, y1, ub, vb}};
    static constexpr int order[kCopyVerts] = {0, 1, 2, 2, 1, 3};
    for (unsigned k = 0; k < kCopyVerts; ++k) out[k] = c[order[k]];
}

// ---- the tile table
struct Tile {
    bool used = false;
    bool ready = false;        // captured by a SUBMITTED recording: drawable
    bool pending = false;      // a first capture recorded, its recording not yet known submitted
    uint64_t key = 0;          // the game binding key the content came from
    Rect r{};
    unsigned refs = 0;         // stored cards referencing it
    unsigned cap_slot = 0;     // the recording of the LAST capture: slot + serial
    uint64_t cap_serial = 0, cap_frame = 0, last_frame = 0;
    uint64_t use[kSlotCap] = {};   // the serial of the last recording of each slot that captured or read the tile (0 = none)
    bool quarantined = false;  // (review 2026-11-01 F3) its pending capture belongs to a quarantined recording: never ready, never joined, never reused
};
struct Counters {
    uint32_t captures = 0;     // first captures recorded (a new tile, or a tile whose recording was discarded)
    uint32_t refreshes = 0;    // in-place recaptures of a ready tile (kRefreshFrames)
    uint32_t shared = 0;       // a card joined a tile of the same key and rectangle (no capture)
    uint32_t full = 0;         // a capture refused: no reusable tile (every tile referenced or not retired)
    uint32_t recycled = 0;     // a retired unreferenced tile taken for another (key, rect)
    uint32_t readied = 0;      // tiles made ready by their recording's submission
    uint32_t dead = 0;         // pending tiles whose recording was discarded (recaptured)
    uint32_t capcap = 0;       // a capture postponed by the per-draw / per-frame capture cap
    uint32_t norect = 0;       // a card whose uv rectangle cannot be tiled
    uint32_t evicted = 0;      // (M6b) a referenced tile taken from cards not sighted / drawn for kEvictAge frames (they keep the key path)
    uint32_t stale = 0;        // (review 2026-11-01 F1) a card found only a stale ready tile of its key and rectangle: refreshed (or capcap), never joined as is
    uint32_t qrel = 0;         // (review 2026-11-01 F3) pending tiles of a quarantined recording whose cards were sent to request a fresh tile
    void add(const Counters& c) noexcept {
        captures += c.captures; refreshes += c.refreshes; shared += c.shared; full += c.full; recycled += c.recycled; readied += c.readied;
        dead += c.dead; capcap += c.capcap; norect += c.norect; evicted += c.evicted; stale += c.stale; qrel += c.qrel;
    }
};
// (M6b, null run step164 AGC1: tiles=128/128 for the whole flight, run cap 1573 -- every tile held by a card, nothing reusable) a full atlas evicts the
// tile least recently sighted or drawn, if that was at least kEvictAge frames ago and every recording that used it has retired.
inline constexpr uint64_t kEvictAge = 30;

class Atlas {
public:
    Tile t[kTiles];
    uint64_t rec[kSlotCap] = {};   // the serial of the recording each slot holds (incremented at every pick of the slot)
    bool quar[kSlotCap] = {};      // (review 2026-11-01 F3) slots quarantined by the producer (never picked again)
    Counters c{};                  // since the last take()

    bool valid(int i) const noexcept { return i >= 0 && unsigned(i) < kTiles && t[i].used; }
    // A new recording took slot s (step5PickSlot_locked succeeded: its previous recording completed on the GPU and can no longer be executed).
    void slot_started(unsigned s) noexcept { if (s < kSlotCap) ++rec[s]; }
    // The recording that holds slot s was submitted (ExecuteCommandLists, the private fence value assigned): its first captures become drawable.
    void slot_submitted(unsigned s) noexcept {
        if (s >= kSlotCap) return;
        for (auto& x : t) if (x.used && x.pending && x.cap_slot == s && x.cap_serial == rec[s]) {
            x.pending = false;
            if (x.refs && !x.quarantined) { x.ready = true; ++c.readied; }   // (F1) an unreferenced capture never becomes joinable without a capture
        }
    }
    // (review 2026-11-01 F3) The producer quarantined slot s (step5SetSlot: a fault, a re-executed recording, a failed owner add). The slot is never picked
    // again, so a capture pending in its recording would never become ready nor count as dead: its cards would wait on it for the rest of the run. Its
    // tiles stay PINNED (the recording may still execute: never reused, never joined) and their cards are released, so that their next request takes a
    // fresh tile. Ready tiles the recording read or refreshed keep their content and stay pinned by `use` (never retired), as before.
    void slot_quarantined(unsigned s) noexcept {
        if (s >= kSlotCap) return;
        quar[s] = true;
        for (auto& x : t) if (x.used && x.pending && !x.quarantined && x.cap_slot == s && x.cap_serial == rec[s]) {
            x.quarantined = true; x.refs = 0; x.ready = false; ++c.qrel;
        }
    }
    // (F3) tiles pinned by a quarantined slot (a capture or read of its last recording, which can never retire): the `qpin=` count.
    unsigned qpinned() const noexcept {
        unsigned n = 0;
        for (const auto& x : t) {
            if (!x.used) continue;
            bool q = x.quarantined;
            for (unsigned s = 0; s < kSlotCap && !q; ++s) q = quar[s] && x.use[s] && rec[s] <= x.use[s];
            n += q;
        }
        return n;
    }
    // Every recording that captured or read the tile has been followed by a new recording of its slot (so it completed and cannot run again).
    bool retired(const Tile& x) const noexcept {
        for (unsigned s = 0; s < kSlotCap; ++s) if (x.use[s] && rec[s] <= x.use[s]) return false;
        return true;
    }
    // A first capture whose recording was discarded (Reset unsubmitted): its slot already holds a newer recording and the tile never became ready.
    bool dead_capture(const Tile& x) const noexcept { return x.pending && x.cap_slot < kSlotCap && rec[x.cap_slot] > x.cap_serial; }
    bool drawable(int i, uint64_t key, const Rect& r) const noexcept { return valid(i) && t[i].ready && t[i].key == key && t[i].r == r; }
    // The current recording (slot s) reads tile i (a card drawn from it): the tile is not reused before that recording retired.
    // (M6b) `frame` (non-zero) also marks it recently used (eviction order).
    void use(int i, unsigned s, uint64_t frame = 0) noexcept {
        if (!valid(i)) return;
        if (s < kSlotCap) t[i].use[s] = rec[s];
        if (frame > t[i].last_frame) t[i].last_frame = frame;
    }
    // (M6b) The referenced tile to evict when the atlas is full: retired, not sighted / drawn for kEvictAge frames, the one used longest ago. -1 = none.
    int evict_candidate(uint64_t frame) const noexcept {
        int best = -1;
        for (unsigned j = 0; j < kTiles; ++j) {
            const Tile& x = t[j];
            if (!x.used || !x.refs || x.pending || x.quarantined || !retired(x) || x.last_frame + kEvictAge > frame) continue;
            if (best < 0 || x.last_frame < t[best].last_frame) best = int(j);
        }
        return best;
    }
    // The caller detached every card that referenced tile i: it is now unreferenced (and retired), the next request may recycle it.
    // (review 2026-11-01 F1) an unreferenced tile is no longer `ready`: a later card of the same key and rectangle captures again instead of joining content
    // that may be minutes (or a mission) old. A pending capture of the current recording stays joinable (cap_slot / cap_serial) until its submission.
    void evicted(int i) noexcept { if (valid(i)) { t[i].refs = 0; t[i].ready = false; ++c.evicted; } }
    // (M7) a second card references tile i (a synthetic plume card sharing the tile of the card it was copied from); -1 when the tile is not live / is quarantined.
    int join(int i) noexcept { if (valid(i) && t[i].refs && !t[i].quarantined) { ++t[i].refs; return i; } return -1; }
    void release(int& i) noexcept { if (valid(i) && t[i].refs && --t[i].refs == 0) t[i].ready = false; i = -1; }
    unsigned live() const noexcept { unsigned n = 0; for (const auto& x : t) n += x.used; return n; }
    unsigned ready_n() const noexcept { unsigned n = 0; for (const auto& x : t) n += x.used && x.ready; return n; }
    // Tiles held by an unretired recording of a slot that will never be picked again cannot be told apart here; `stuck` = used, unreferenced, not retired.
    unsigned stuck() const noexcept { unsigned n = 0; for (const auto& x : t) n += x.used && !x.refs && !retired(x); return n; }
    Counters take() noexcept { const Counters d = c; c = Counters{}; return d; }

    // A stored card (current tile `cur`, -1 = none) was sighted in a draw of the recording in slot s that binds `key`; r = its uv rectangle. Returns the
    // tile it references from now on (cur is released when it changes; -1 = none) and sets `capture` when the caller must record a capture of r from
    // `key` into the returned tile in THIS draw. can_capture = false (the caller's capture cap is spent): no new capture, an existing match is kept.
    int request(int cur, uint64_t key, const Rect& r, unsigned s, uint64_t frame, bool can_capture, bool& capture) noexcept {
        capture = false;
        if (s >= kSlotCap) { release(cur); return -1; }
        if (valid(cur) && t[cur].key == key && t[cur].r == r && !t[cur].quarantined) {
            Tile& x = t[cur]; x.last_frame = frame;
            if (x.cap_slot == s && x.cap_serial == rec[s]) return cur;              // captured in this recording already
            const bool dead = dead_capture(x);
            if (x.pending && !dead) return cur;                                      // waits for its recording's submission
            if (!dead && !(frame >= x.cap_frame + kRefreshFrames)) return cur;      // ready and fresh
            if (!can_capture) { ++c.capcap; return cur; }
            mark_capture(x, s, frame);
            if (dead) { ++c.dead; ++c.captures; } else ++c.refreshes;                // a refresh keeps `ready` (same key, same rectangle)
            capture = true;
            return cur;
        }
        release(cur);
        // A tile of the same key and rectangle: captured in this recording, or ready and FRESH (captured less than kRefreshFrames ago). (review 2026-11-01
        // F1) a ready tile captured longer ago may hold another texture's pixels (the key re-meant after a mission restart, or the texture rewritten): it
        // is joined only with an in-place refresh in this draw, never as is.
        int staleTile = -1;
        for (unsigned j = 0; j < kTiles; ++j) {
            Tile& x = t[j];
            if (!x.used || x.quarantined || x.key != key || x.r != r) continue;
            const bool here = x.cap_slot == s && x.cap_serial == rec[s];
            if (!here && !x.ready) continue;
            if (!here && !(frame < x.cap_frame + kRefreshFrames)) { if (staleTile < 0) staleTile = int(j); continue; }
            if (here && !x.ready) x.pending = true;   // a capture of this recording whose cards all left: readied again by its submission
            ++x.refs; x.last_frame = frame; ++c.shared;
            return int(j);
        }
        if (staleTile >= 0) {
            ++c.stale;
            if (!can_capture) { ++c.capcap; return -1; }
            Tile& x = t[staleTile];
            ++x.refs; x.last_frame = frame;
            mark_capture(x, s, frame);   // ready: an in-place refresh (same key, same rectangle), as the card's own-tile path does
            ++c.refreshes; capture = true;
            return staleTile;
        }
        if (!can_capture) { ++c.capcap; return -1; }
        int best = -1;
        for (unsigned j = 0; j < kTiles; ++j) {   // a free tile; else the retired unreferenced tile used longest ago
            const Tile& x = t[j];
            if (!x.used) { best = int(j); break; }
            if (x.refs || !retired(x)) continue;
            if (best < 0 || x.last_frame < t[best].last_frame) best = int(j);
        }
        if (best < 0) { ++c.full; return -1; }
        if (t[best].used) ++c.recycled;
        Tile& x = t[best];
        x = Tile{};
        x.used = true; x.pending = true; x.key = key; x.r = r; x.refs = 1; x.last_frame = frame;
        mark_capture(x, s, frame);
        ++c.captures; capture = true;
        return best;
    }

private:
    void mark_capture(Tile& x, unsigned s, uint64_t frame) noexcept {
        x.cap_slot = s; x.cap_serial = rec[s]; x.cap_frame = frame; x.use[s] = rec[s];
        if (!x.ready) x.pending = true;
    }
};

// ---- the host's root-signature check (the descriptor tables we must point at our heap and set back)
// One descriptor-table root parameter of the bound root signature: its ranges' kinds and the descriptor span they cover.
struct TableDesc {
    uint32_t param = 0;
    bool srv_only = false, unbounded = false;
    uint32_t span = 0;          // descriptors from the table start to the end of its last range (explicit offsets included)
    bool value_known = false;   // the game's current argument of this parameter is tracked (needed to set it back)
    bool sampler_only = false;  // (M6b) every range is a sampler range (the table lives in the SAMPLER heap)
};
// nullptr = the atlas can be drawn under this host. The key table (`key_param`) holds only SRVs within kTableMax descriptors (our heap fills every one of
// them with the atlas SRV); every OTHER table is a sampler table (it lives in the game's SAMPLER heap, which stays bound next to ours and is set back as it
// was); every table's game value is tracked (it is undefined after a heap change and is set again). `heaps_bound` = descriptor heaps the list has bound:
// a sampler table needs two (the game's CBV/SRV/UAV heap + its sampler heap). Any other CBV / SRV / UAV table would have to be copied into our heap from a
// shader-visible heap: refused by name (atlas-other-view-table), never guessed.
// (M6b, null run step164: hwhy=atlas-other-table on every host) the live 4caf6e2d signature is root signature 310 of docs/interception/stored-draw-manifest-1.md
// 3.2: param0 VS constants b0 x4, param1 PS constants b1 x4, param2 PS SRV table (t0), param3 PS SAMPLER table (s0), no static samplers. M6 assumed a static s0.
inline const char* host_why(const TableDesc* tables, unsigned n, uint32_t key_param, unsigned heaps_bound, unsigned& key_count) noexcept {
    key_count = 0;
    if (!tables || n == 0) return "atlas-no-table";
    if (n > kMaxTables) return "atlas-table-count";
    { bool found = false; for (unsigned i = 0; i < n; ++i) found = found || tables[i].param == key_param; if (!found) return "atlas-key-param"; }
    bool key = false, samplers = false;
    for (unsigned i = 0; i < n; ++i) {
        const TableDesc& d = tables[i];
        if (d.param == key_param) {
            if (!d.value_known) return "atlas-table-unknown";
            if (!d.srv_only) return "atlas-key-table-not-srv";
            if (d.unbounded || d.span == 0 || d.span > kTableMax) return "atlas-key-table-size";
            key = true; key_count = d.span;
        } else {
            if (!d.sampler_only) return "atlas-other-view-table";
            if (!d.value_known) return "atlas-table-unknown";
            samplers = true;
        }
    }
    if (!key) return "atlas-key-param";
    if (samplers && heaps_bound != 2) return "atlas-sampler-heap";
    return nullptr;
}
// The root-signature layout as text for the refusal (fx-puff-atlas `lay=`): per parameter `<i>:<C|V|S|U|T><reg>[x<count>|(<kinds><span>)]<vis>`,
// kinds s = SRV, c = CBV, u = UAV, z = sampler; vis a = all, v = vertex, p = pixel, o = other. E.g. root signature 310: "0:C0x4v,1:C1x4p,2:T(s1)p,3:T(z1)p".
struct ParamDesc { char kind = 'T'; uint32_t reg = 0, count = 0; char kinds[5] = {}; uint32_t span = 0; char vis = 'a'; };
inline void layout_text(const ParamDesc* p, unsigned n, char* out, size_t cap) noexcept {
    if (!out || !cap) return;
    out[0] = 0; size_t len = 0;
    for (unsigned i = 0; i < n && len + 1 < cap; ++i) {
        int r = 0;
        if (p[i].kind == 'T') r = std::snprintf(out + len, cap - len, "%s%u:T(%s%u)%c", i ? "," : "", i, p[i].kinds, p[i].span, p[i].vis);
        else if (p[i].kind == 'C') r = std::snprintf(out + len, cap - len, "%s%u:C%ux%u%c", i ? "," : "", i, p[i].reg, p[i].count, p[i].vis);
        else r = std::snprintf(out + len, cap - len, "%s%u:%c%u%c", i ? "," : "", i, p[i].kind, p[i].reg, p[i].vis);
        if (r <= 0) break;
        len += size_t(r); if (len >= cap) { out[cap - 1] = 0; break; }
    }
}
// The span of a table from its ranges (count, explicit offset or kAppend); unbounded / overflow -> false.
inline constexpr uint32_t kAppend = 0xffffffffu;
inline bool table_span(const uint32_t* counts, const uint32_t* offsets, unsigned n, uint32_t& span) noexcept {
    span = 0; uint64_t end = 0, top = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (counts[i] == 0xffffffffu) return false;
        const uint64_t start = offsets[i] == kAppend ? end : uint64_t(offsets[i]);
        end = start + counts[i];
        if (end > top) top = end;
        if (top > 0xffffu) return false;
    }
    span = uint32_t(top);
    return true;
}

// ---- the commands of one host's atlas group (per eye; native vtable calls in gpu_copy_step5_fxsprite.inl)
// (M6b; fx_puff_atlas_d3d12.h record_group) SetDescriptorHeaps(ours [+ the game's sampler heap]), every sampler table set again to its game value (table
// state is undefined after a heap change), the key table = our SRV table, Draw(group), SetDescriptorHeaps(the game's heaps), EVERY table set again to its
// game value (the key table = the host's own key).
inline constexpr unsigned group_commands(unsigned tables) noexcept { return 3u + 2u * tables; }
// The capture of one host draw (nc quads, one draw): barrier, PSO, OM, viewport, scissor, topology, VB, draw, barrier, viewport, scissor, topology back
// (the replay that follows binds its own PSO / OM / VB and sets the game's back).
inline constexpr unsigned kCaptureCommands = 12;

// ---- the 1/s `fx-puff-atlas` line (step5-producer.log, after fx-puff-mem; < 448 bytes with the 21-byte prefix at every cap, offline-tested)
// res = the resources (ready / why not); tiles = used / ready / stuck of kTiles; cap / ref / shr / full / rec / rdy / dead / ccap / nor = the window's
// Counters (+ ev = evicted, M6b); drawn = cards drawn from the atlas (selections); unb / want = fx-puff-mem's (M6b: unb = wanted card-frames NOT drawn
// whose key no game draw bound, whatever their tile); amiss = the part of unb that had a ready tile (no atlas-capable host took it); hostno = host draws
// that could not draw the atlas group (`hwhy` the last reason, `lay` that host's root-signature layout, layout_text); capno = draws whose sighted cards could
// not be captured (`cwhy`); run = totals (7 digits). live / ready / stuck: tiles in use / drawable / unreferenced but not yet retired.
struct LineIn {
    uint64_t frame = 0; bool on = false; const char* res = "not-requested";
    unsigned live = 0, ready = 0, stuck = 0, qpin = 0;   // (review 2026-11-01 F3) qpin = tiles pinned by a quarantined slot (Atlas::qpinned)
    Counters w{}, run{};
    uint32_t drawn = 0, amiss = 0, unb = 0, want = 0, hostno = 0, capno = 0, run_drawn = 0, run_unb = 0, run_want = 0;
    const char* hwhy = nullptr; const char* cwhy = nullptr; const char* lay = nullptr;
};
inline int format_line(char* b, size_t n, const LineIn& in) noexcept {
    if (!b || !n) return -1;
    auto c = [](uint32_t x) { return x < 99999u ? x : 99999u; };
    auto r = [](uint32_t x) { return x < 9999999u ? x : 9999999u; };
    return std::snprintf(b, n, "fx-puff-atlas;frame=%llu;on=%d;res=%.28s;tiles=%u/%u/%u;of=%u;cap=%u;ref=%u;shr=%u;full=%u;rec=%u;ev=%u;rdy=%u;dead=%u;"
        "ccap=%u;nor=%u;stl=%u;qpin=%u;drawn=%u;amiss=%u;unb=%u;want=%u;hostno=%u;hwhy=%.22s;lay=%.40s;capno=%u;cwhy=%.20s;run=cap:%u,drawn:%u,unb:%u,want:%u",
        static_cast<unsigned long long>(in.frame % 100000000ull), in.on ? 1 : 0, in.res ? in.res : "none", in.live, in.ready, in.stuck, kTiles,
        c(in.w.captures), c(in.w.refreshes), c(in.w.shared), c(in.w.full), c(in.w.recycled), c(in.w.evicted), c(in.w.readied), c(in.w.dead), c(in.w.capcap),
        c(in.w.norect), in.w.stale < 9999u ? in.w.stale : 9999u, in.qpin < 999u ? in.qpin : 999u, c(in.drawn), c(in.amiss), c(in.unb), c(in.want), c(in.hostno), in.hwhy ? in.hwhy : "none", in.lay && *in.lay ? in.lay : "-",
        c(in.capno), in.cwhy ? in.cwhy : "none", r(in.run.captures), r(in.run_drawn), r(in.run_unb), r(in.run_want));
}
// (review 2026-11-01 F2, read only) the compute census, run totals: hook = the Dispatch hook (-1 not installed, 0 patch failed, 1 hooked); groups = atlas
// groups recorded; dispatch = every Dispatch the game recorded; after / ind = Dispatch / ExecuteIndirect recorded into the list of the last atlas group
// after that group (before the list's Reset); lists = such lists. after = ind = 0 over a flight: the game records no compute work after an atlas group.
inline int format_compute_line(char* b, size_t n, uint64_t frame, int hook, uint32_t groups, uint32_t dispatch, uint32_t after, uint32_t ind,
                               uint32_t lists) noexcept {
    if (!b || !n) return -1;
    return std::snprintf(b, n, "fx-puff-atlas-compute;frame=%llu;hook=%d;groups=%u;dispatch=%u;after=%u;ind=%u;lists=%u",
        static_cast<unsigned long long>(frame % 100000000ull), hook < -1 ? -1 : hook > 1 ? 1 : hook, groups, dispatch, after, ind, lists);
}
// (M6b) once per process, the first atlas host's root-signature layout, its verdict, the descriptor heaps it had bound and whether one is the sampler heap.
inline int format_host_line(char* b, size_t n, const char* lay, const char* why, unsigned heaps, bool sampler_heap) noexcept {
    if (!b || !n) return -1;
    return std::snprintf(b, n, "fx-puff-atlas-host;why=%.32s;heaps=%u;sampler-heap=%d;layout=%.70s;legend=<param>:C<reg>x<count>|T(<s|c|u|z kinds><span>)<a|v|p|o vis>",
        why ? why : "ok", heaps, sampler_heap ? 1 : 0, lay && *lay ? lay : "-");
}

} // namespace acz::fxpuff::atlas
