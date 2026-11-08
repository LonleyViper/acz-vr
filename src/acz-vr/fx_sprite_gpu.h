#pragma once
// fx_sprite_gpu.h -- EFFECTS-OUTSIDE (F) fxmem:6: the GPU replay PER EYE of the game's pre-projected ps2_v SPRITE effects (class 0 = the explosion PS 542a11d7
// extra7_p; class 1 = the indexed GREATER 4caf6e2d ps2AtestCA sprites when the effects layer admitted them). docs/world-render/effects-outside-window.md
// section 11. SDK-free policy for the producer (gpu_copy_step5_fxsprite.inl step5FxSpriteGpu*, step5RecordSprite) and the CPU mirror of the consumer composite
// (xr_stereo_blit.hlsl sprite_over). Template: the fxmem:4 smoke replay (fx_smoke_gpu.h).
//
// What the GPU replay does (one captured sprite draw whose referenced vertices fx_sprite_replay.h plan_draw re-projected for BOTH eyes this frame):
//   the two eye vertex copies (n <= 512 x 64 B each) go to the frame slot's sprite UPLOAD span (this slot's span only, written while the slot is PRODUCING),
//   then, inline before the game's own draw and under the SAME predicate word the effects layer opened for that draw, the draw is recorded twice with the
//   coverage_v1 clone PSO (extra7_p: colour as the game, alpha ZERO/ONE; 4caf6e2d: the dual clone): render target = the slot's per-eye SPRITE image, depth =
//   that eye's objects depth-stencil (test only), IASetVertexBuffers(slot 0) = the eye's copy (stride 64), DrawIndexedInstanced with the game's index count /
//   start / start instance and BaseVertexLocation = -lo (index i addresses copy vertex i - lo). The game's slot-0 vertex buffer view (setter tracking), PSO,
//   predication and render targets are bound back before the game's own draw. The draw is then NOT replayed into the window effects image (no double).
// Why a SECOND per-eye image and not the smoke images: the smoke composite un-premultiplies by the coverage A (xr_stereo_blit.hlsl smoke_over / effects_over
//   mode 4: colour = P / A, 0 when A = 0). extra7_p's clone adds light with alpha ZERO/ONE (A unchanged, docs 10.1), so a fireball over empty space has
//   P > 0 and A = 0 and would vanish there (sprite_vs_smoke_composite below, offline-tested). The sprite image is composited as premultiplied LIGHT + coverage:
//   s = tm(P x exposure) (no division), out = s + (1 - A) x under: exact for pure additive light (A = 0) and for opaque sprites (A = 1).
// (I, docs section 14) The explosion class uses its own "light" PSO in the eye images (A' = A(1+a) - a: the game's background brightening as T = 1 - A > 1)
//   and the sprite layer is composited in DISPLAY space (out = decode(min(1, s + T encode(under)))), matching the game's display-space blend.
//
// Lifetime (held-slot rules; the invariant text for review is in docs section 11.4): the per-eye sprite images and the sprite UPLOAD block are created at most
// ONCE per process, only when fxmem:6 asks for it (at ring creation, or late by the worker tick under the step-1 mutex when the option arrives after the ring),
// published to the frames by spriteReady set last; kept for the process lifetime; never released or recreated. A slot's images and span are written only by
// a recording into that slot (the slot is PRODUCING: step5PickSlot_locked); the consumer reads the images only while it HOLDS the slot, after its queue
// waited on the slot's fence value. A re-execution of the owning recording rewrites the same bytes and is quarantined like every other slot write.
#include "fx_smoke_gpu.h"       // FrameState (replays / predicate commit counting), Px, kCmpGreater*, kObjectBand, kAlign, align_up
#include "fx_sprite_replay.h"   // kMaxVertices, kVertexStride

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace acz::fxsprite::gpu {

namespace sg = acz::fxsmoke::gpu;

// ---- memory
// EFFECTS-OUTSIDE (H, 2026-10-28 headset items 5/6): the slot span is PACKED. Before, every replay owned a fixed 2 x 32 KiB pair (8 draws per frame), so the
// tracer class (G) alone could fill the frame: its persistent non-gun 9-index strip draws 500-600 times per second (batch G census, ~8-10 per 60 Hz frame), the
// GREATER 4caf6e2d sprites 1-3 per frame (batch H), and an explosion arriving after them fell back to the WINDOW effects image ("sprite-gpu-draw-cap"), where
// its light is dropped (A = 0, section 11.3) and, with the terrain renderer, everything is composited UNDER the replayed objects (objects render through it).
// Now each replay takes 2 x align_up(n x 64, 256) bytes at the frame's cursor (a tracer strip of <= 24 vertices: 2 x 1.5 KiB), up to kMaxDraws per frame,
// and the sprite / tracer classes leave kExplosionReserveDraws draws and kExplosionReserveBytes of the span to the explosion class (two 512-vertex draws).
// (K, headset 2026-10-30 m03k; docs section 17) kMaxDraws was 32: the user's m03k flight drew 85-120 class-1 sprite draws per frame in its dense seconds, the
// per-eye path stopped at 30 (`sprite-gpu-draw-cap` on 13 of the 15 fx-sprite-gpu lines that refused anything) and 16,657 sprite draws went to the WINDOW
// effects image (forward rectangle only, under the objects). It is now the effects layer's own per-frame cap (gpu_copy_step5.h effects_draw_cap = 96: a
// sprite draw is replayed per eye only after the effects layer accepted it, so no frame can hold more); the packed span (512 KiB) still bounds the bytes
// (`sprite-gpu-span-full`), e.g. 94 x 24-vertex strips = 288 KiB < the 384 KiB the sprite / tracer classes may use.
inline constexpr unsigned kMaxDraws = 256;                                          // sprite draws replayed per eye per frame (more: window fallback, named)
inline constexpr unsigned kExplosionReserveDraws = 2;                               // of kMaxDraws: only the explosion class may use these
using FrameState = sg::BasicFrameState<kMaxDraws>;   // predicate bookkeeping for every sprite draw (W5-2 P2: the smoke's 16-entry store lost 17..32)
inline constexpr uint64_t kCopyAlign = 256;                                         // eye copy start alignment inside the span (>= the 64-byte vertex)
inline constexpr uint64_t kEyeCopyBytes = uint64_t(kMaxVertices) * kVertexStride;  // 32,768 B: the largest eye copy of one draw (<= 512 vertices)
// (Q, step174 fxadd) kMaxDraws 96 -> 256 (= effects_draw_cap; measured worst frame ~191 candidates) and the span 512 KiB -> 1 MiB per slot. Bytes at the measured
// peak: 131 sprite + 58 tracer draws of ~7 vertices (951 + 867 vertices in the two plan lines) = 189 x 2 x 512 B = ~190 KiB, but the span used to bound 254 draws at
// only 384 KiB (1.5 KiB per 10-vertex draw = exactly the limit); 1 MiB leaves 896 KiB to the sprite / tracer classes (254 x 3 KiB = 24-vertex strips) and the 128 KiB
// explosion reserve. Cost: +512 KiB UPLOAD per slot = +1.5 MiB for 3 slots (total upload 3.75 MiB with the trail region); images unchanged; fx total 181 MB < kTotalCap 200 MB.
inline constexpr uint64_t kSlotUpload = 1048576;                                    // 1 MiB per slot (packed: each replay takes 2 x align_up(n x 64, 256))
static_assert(kSlotUpload % sg::kAlign == 0 && kSlotUpload >= 8 * 2 * kEyeCopyBytes, "the span holds at least 8 full-size draws");
// (N, step158/159 null runs; docs section 17.6) the puff memory's re-emitted cards (fx_puff_trail.h, up to kMaxEmit = 160 cards x 6 vertices per eye) have their OWN
// region behind the game's span in every slot: kTrailBytes, taken through trail_take() and never counted in the game's cursor, so a full cache can neither
// shrink the span the game's own effect sprites see (sprite-gpu-span-full) nor delay them; a cache that does not fit its region is dropped (trail-span-full).
// (M5, headset test 7) 256 KiB: TWO full host selections per frame (2 x 2 eyes x 960 vertices = 245,760 B) plus the groups' alignment. With smoke cards
// re-emitted again (kCap 128) next to the clouds, one frame's distinct cards can exceed the 160 + 10 the 128 KiB region held (trail-span-full drops a host's
// whole selection). +128 KiB per slot (+384 KiB for 3 slots) of UPLOAD memory; the game's span is unchanged.
inline constexpr uint64_t kTrailBytes = 262144;
inline constexpr uint64_t kSlotStride = kSlotUpload + kTrailBytes;                  // one slot of the sprite UPLOAD buffer: [0, kSlotUpload) game replays, then the cache
static_assert(kTrailBytes % sg::kAlign == 0 && kSlotStride % 65536 == 0, "slot spans stay page aligned");
inline constexpr uint64_t kExplosionReserveBytes = uint64_t(kExplosionReserveDraws) * 2 * kEyeCopyBytes;   // 128 KiB: two full-size explosion draws
inline constexpr uint64_t kImageCap = sg::kImageCap;    // one eye sprite image (2560x1440 RGBA16F = 29.5 MB)
inline constexpr uint64_t kTotalCap = 200ull << 20;     // all slots x 2 eyes + the UPLOAD block (3 x 2 x 29.6 MB + 1.5 MiB = 179 MB at 2560x1440)
// Bytes of ONE eye copy of a draw referencing `vertices` vertices (aligned so the next copy starts on kCopyAlign).
inline constexpr uint64_t copy_bytes(uint32_t vertices) noexcept { return sg::align_up(uint64_t(vertices) * kVertexStride, kCopyAlign); }
// Bytes one replay takes from the span (both eyes).
inline constexpr uint64_t replay_bytes(uint32_t vertices) noexcept { return 2 * copy_bytes(vertices); }
// Byte offset of eye e of the replay whose copies start at `cursor` (the frame's span bytes used before it).
inline constexpr uint64_t offset(uint64_t cursor, uint32_t vertices, unsigned e) noexcept { return cursor + uint64_t(e) * copy_bytes(vertices); }
// nullptr = within the caps (`charged` set); else why.
// EYE-RING (eye_ring.h): the eye-size caps (same values as fx_smoke_gpu.h kEyeImageCap / kEyeTotalCap), passed by the producer on the eye ring only.
inline constexpr uint64_t kEyeImageCap = sg::kEyeImageCap;
inline constexpr uint64_t kEyeTotalCap = sg::kEyeTotalCap;
inline const char* budget(unsigned bytes_per_pixel, uint64_t width, uint32_t height, unsigned slots, uint64_t& charged,
                          uint64_t image_cap = kImageCap, uint64_t total_cap = kTotalCap) noexcept {
    charged = 0;
    if (!bytes_per_pixel) return "sprite-format-unsupported";
    if (!width || !height || width > 16384 || height > 16384 || !slots) return "sprite-size-unsupported";
    const uint64_t bytes = width * height * bytes_per_pixel;
    if (bytes > image_cap) return "sprite-over-image-cap";
    const uint64_t total = (sg::align_up(bytes, sg::kAlign) + sg::kAlign) * 2 * slots + kSlotStride * slots;
    if (total > total_cap) return "sprite-over-total-cap";
    charged = total;
    return nullptr;
}

// The cache's eye copies of one host draw, taken from the slot's trail region (`trail_used` = bytes the frame's earlier hosts took there). at[e] = byte offset
// of eye e from the slot start (behind the game's span). nullptr = taken (`used` set), else "trail-span-full" (the caller drops the selection; the game's
// replays are not touched: they never read the trail region and the region never reads their cursor).
struct TrailTake { uint64_t at[2] = {}; uint64_t used = 0; };
inline const char* trail_take(uint64_t trail_used, uint32_t verts0, uint32_t verts1, TrailTake& t) noexcept {
    t = TrailTake{};
    const uint64_t b0 = verts0 ? copy_bytes(verts0) : 0, b1 = verts1 ? copy_bytes(verts1) : 0;
    if (trail_used > kTrailBytes || b0 + b1 > kTrailBytes - trail_used) return "trail-span-full";
    t.at[0] = kSlotUpload + trail_used; t.at[1] = t.at[0] + b0; t.used = b0 + b1;
    return nullptr;
}

// ---- per-draw gate (facts the producer holds at the sprite draw; nullptr = replay per eye, else the named refusal and the window effects replay of the draw
// runs exactly as with fxmem:5)
struct GateIn {
    bool ready = false;            // the slot's two eye sprite images + the sprite UPLOAD block exist
    bool effects_recorded = false; // the effects layer accepted this draw (predicate slot opened, clears scheduled)
    bool dual = false;             // the coverage clone exists for this PSO (extra7_p: alpha ZERO/ONE; 4caf6e2d: the dual clone)
    bool prepared = false;         // this draw's vertices were read and re-projected for BOTH eyes this frame (plan_draw prepared it)
    uint32_t vertices = 0;         // referenced vertex count (hi - lo + 1) = the eye copy size
    unsigned draw_index = 0;       // replays already recorded this frame
    uint64_t span_used = 0;        // (H) span bytes those replays took (the cursor this one would start at)
    bool explosion = false;        // (H) the explosion class (PS 542a11d7): may use the reserve the sprite / tracer classes leave
    bool indexed = false; uint32_t ib_stride = 0, vb_stride = 0, instances = 0;
    bool allow_nonindexed = false;  // (G) tracer / (J) class-1 4caf6e2d: a non-indexed strip quad list is replayed with DrawInstanced from the eye copy
    uint64_t vb_va = 0, vb_bytes = 0; // the game's slot-0 view (restored after the eye draws: must be representable)
    bool eye_depth = false;
    uint32_t pso_dsv = 0, eye_dsv = 0, pso_rtv0 = 0, image_format = 0, pso_samples = 0, pso_rt_count = 0;
    bool depth_enable = false, depth_write = false, stencil = false, stencil_noop = false; uint32_t depth_func = 0;
    bool vp_known = false; unsigned viewports = 0; float vp[4] = {}; uint32_t image_w = 0, image_h = 0;
    unsigned scissors = 0; int32_t sc[4] = {};   // the game's scissor 0 (l, t, r, b): must cover the image (the eye maps the eye frustum onto the whole GS window)
};
inline const char* gate(const GateIn& g) noexcept {
    if (!g.ready) return "sprite-gpu-not-ready";
    if (!g.effects_recorded) return "sprite-effects-not-recorded";
    if (!g.dual) return "sprite-needs-effects-dual";
    if (!g.prepared) return "sprite-not-prepared";
    if (!g.vertices || g.vertices > kMaxVertices) return "sprite-vertex-count";
    if (g.draw_index >= (g.explosion ? kMaxDraws : kMaxDraws - kExplosionReserveDraws)) return "sprite-gpu-draw-cap";
    {   // (H) the packed span: this replay's two eye copies must fit below the class's limit
        const uint64_t limit = g.explosion ? kSlotUpload : kSlotUpload - kExplosionReserveBytes;
        if (g.span_used > limit || replay_bytes(g.vertices) > limit - g.span_used) return "sprite-gpu-span-full";
    }
    if (g.indexed ? g.ib_stride != 2 : !g.allow_nonindexed) return "sprite-not-indexed-r16";
    if (g.vb_stride != kVertexStride) return "sprite-vb-stride";
    if (g.instances != 1) return "sprite-instanced";
    if (!g.vb_va || !g.vb_bytes || g.vb_bytes > 0xFFFFFFFFull) return "sprite-vb-view";
    if (!g.eye_depth || !g.eye_dsv) return "sprite-eye-depth-missing";
    if (g.pso_dsv != g.eye_dsv) return "sprite-dsv-format-differs";
    if (g.pso_samples != 1) return "sprite-pso-multisample";
    if (g.pso_rt_count != 1 || g.pso_rtv0 != g.image_format) return "sprite-rt-format-differs";
    if (!g.depth_enable || !(g.depth_func == sg::kCmpGreater || g.depth_func == sg::kCmpGreaterEqual)) return "sprite-pso-depth-test";
    if (g.depth_write) return "sprite-pso-depth-write";
    if (g.stencil && !g.stencil_noop) return "sprite-pso-stencil";
    if (!g.vp_known || g.viewports != 1) return "sprite-viewport-unknown";
    if (g.vp[0] != 0.0f || g.vp[1] != 0.0f || g.vp[2] != float(g.image_w) || g.vp[3] != float(g.image_h)) return "sprite-viewport-not-image";
    if (g.scissors != 1 || g.sc[0] > 0 || g.sc[1] > 0 || g.sc[2] < int32_t(g.image_w) || g.sc[3] < int32_t(g.image_h)) return "sprite-scissor-not-image";
    return nullptr;
}

// ---- the recorded sequence (native vtable calls; offline-checked against the fake list)
// [ClearRTV(effects) / ClearDSV(HUD) as the effects call scheduled them] [ClearRTV(sprite eye 0), ClearRTV(sprite eye 1): first replay of the frame]
// [SetPredication(word)] SetPipelineState(clone) {OMSet(sprite e, eye objects DSV e), IASetVertexBuffers(0, 1, eye e copy), DrawIndexedInstanced(count, 1,
// start, -lo, start instance)} x 2, IASetVertexBuffers(0, 1, the game's view), SetPipelineState(game), [SetPredication(null)], OMSet(the game's targets).
enum class CmdKind : uint8_t { set_pso_clone, om_eye, vb_eye, draw_eye, vb_game, set_pso_game, om_restore };
struct Cmd { CmdKind kind = CmdKind::om_restore; unsigned eye = 0; };
inline constexpr unsigned kMaxCmds = 10;
inline constexpr unsigned kCmdsPerDraw = 12;   // pred on, PSO clone, 2 x (OM, VB, Draw), VB restore, PSO game, pred off, OM restore
inline constexpr unsigned kCmdsClear = 2;      // the two sprite image clears (once per frame)
inline unsigned replay_commands(Cmd out[kMaxCmds]) noexcept {
    unsigned n = 0;
    out[n++] = {CmdKind::set_pso_clone, 0};
    for (unsigned e = 0; e < 2; ++e) { out[n++] = {CmdKind::om_eye, e}; out[n++] = {CmdKind::vb_eye, e}; out[n++] = {CmdKind::draw_eye, e}; }
    out[n++] = {CmdKind::vb_game, 0};
    out[n++] = {CmdKind::set_pso_game, 0};
    out[n++] = {CmdKind::om_restore, 0};
    return n;
}
// The eye vertex-buffer views and the base vertex of one replay whose copies start `cursor` bytes into the slot span at span_va (the frame's packed cursor).
// false = not representable (no span, an empty / oversized copy, a misaligned cursor, or the pair would end beyond the span). `used` = the span bytes it takes.
struct EyeViews { uint64_t va[2] = {}; uint32_t bytes = 0, stride = kVertexStride; int32_t base = 0; uint64_t used = 0; };
inline bool eye_views(uint64_t span_va, uint64_t cursor, uint32_t vertices, uint32_t lo, EyeViews& v) noexcept {
    v = EyeViews{};
    if (!span_va || !vertices || vertices > kMaxVertices || lo > 0xFFFFu || cursor % kCopyAlign != 0) return false;
    if (cursor > kSlotUpload || replay_bytes(vertices) > kSlotUpload - cursor) return false;
    for (unsigned e = 0; e < 2; ++e) v.va[e] = span_va + offset(cursor, vertices, e);
    v.bytes = vertices * kVertexStride; v.base = -int32_t(lo); v.used = replay_bytes(vertices);
    return true;
}

// ---- EFFECTS-OUTSIDE (I): the per-eye EXPLOSION pipeline state (docs section 14)
// The explosion class (PS 542a11d7 extra7_p) is drawn into the eye sprite image with its own "light" PSO when it exists and the image can hold a negative
// alpha (a float format): alpha SRC1_ALPHA / SRC_ALPHA / REV_SUBTRACT, A' = A (1+a) - a (gpu_copy_step5.h effects_explosion_light_factors), which carries
// the game's (1+a) brightening of the background as transmittance T = 1 - A > 1. Every other case keeps the shared coverage clone (alpha ZERO/ONE, A' = A),
// exactly the previous replay; the window effects image ALWAYS uses the shared clone (its mode-4 composite cannot hold A < 0).
inline constexpr uint32_t kFormatRgba32F = 2, kFormatRgba16F = 10;   // DXGI_FORMAT_R32G32B32A32_FLOAT / R16G16B16A16_FLOAT
enum class ExplosionPso : uint8_t { shared, light };
// nullptr = use the light PSO; else the reason the shared clone is used (logged as xpso=shared(<why>) on the fx-sprite-gpu line).
inline const char* explosion_pso_why(bool explosion_class, bool light_present, uint32_t image_format) noexcept {
    if (!explosion_class) return "not-explosion";
    if (!light_present) return "light-pso-missing";
    if (image_format != kFormatRgba16F && image_format != kFormatRgba32F) return "image-not-float";
    return nullptr;
}
// CPU mirror of the GAME's extra7_p output (docs 10.1): t = texel x COLOR0, a = t.a: o0 = (a t.rgb, 1 + a), o1.a = a / (1 + a).
struct Extra7Out { float rgb[3] = {}; float a0 = 0, a1 = 0; };
inline Extra7Out extra7(const float t[4]) noexcept {
    Extra7Out o; const float a = t[3];
    for (int k = 0; k < 3; ++k) o.rgb[k] = t[k] * a;
    o.a0 = 1.0f + a; o.a1 = a / (1.0f + a);
    return o;
}
// The game's blend into a DISPLAY-value target: RGB' = o0.a o0.rgb + o0.a D.rgb (5,5,1). (The game's alpha, 18/19/1, is not used by us.)
inline void game_explosion_rgb(const Extra7Out& o, float d[3]) noexcept { for (int k = 0; k < 3; ++k) d[k] = o.a0 * o.rgb[k] + o.a0 * d[k]; }
// Our per-eye blend into the sprite image texel p (P premultiplied light, A coverage): RGB as the game; alpha by the chosen PSO.
inline void sprite_explosion_blend(const Extra7Out& o, float p[4], ExplosionPso pso) noexcept {
    for (int k = 0; k < 3; ++k) p[k] = o.a0 * o.rgb[k] + o.a0 * p[k];
    if (pso == ExplosionPso::light) p[3] = p[3] * o.a0 - o.a0 * o.a1;   // REV_SUBTRACT: dest x SRC_ALPHA - src x SRC1_ALPHA = A (1+a) - a
    // shared clone: ZERO / ONE / ADD: A unchanged
}

// ---- consumer composite, CPU mirror of xr_stereo_blit.hlsl sprite_over (mode_bits bit 19 on the composite eye targets)
// (I) The sprite image holds the game's DISPLAY values (D-311: the replayed effects = what the game writes into its window, which is shown without any
// further encode) while the eye pipeline composites LINEAR values into an sRGB RTV (terrain / world layers are decoded: t_srgb_to_linear). Adding display
// light to linear values and encoding again turns the white-hot core tan and pale. The game blends in display space, so the sprite layer is composited
// THERE: out = decode(min(1, s + T x encode(under))), T = 1 - A (A may be negative: the light PSO's T up to (1+a) per overlapping explosion, floored at
// kMinCoverage). An empty texel (P = 0, A = 0) returns `under` unchanged (no round trip).
inline constexpr float kMinCoverage = -4096.0f;   // T <= 4097: A' = 2A-1 per overlapping full-alpha draw (-1,-3,-7,..): 12 stacked draws fit; an fp16 bound only (W5-2 P1: -3 lost unsaturated brightening after 3 overlaps); the display add saturates
inline float srgb_encode(float x) noexcept {
    x = x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f;
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}
inline float srgb_decode(float x) noexcept {
    x = x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f;
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
// The sprite texel (premultiplied light P, coverage A) as a layer: exposure + tonemap on P directly (no un-premultiply: light without coverage stays light),
// A clamped to [kMinCoverage, 1], both times the decouple edge fade (a faded T > 1 tends to 1).
inline sg::Px sprite_layer(const float e[4], float exposure, int tonemap, float fade) noexcept {
    float c[3];
    for (int k = 0; k < 3; ++k) {
        float v = e[k] * exposure; v = v > 0.0f ? v : 0.0f;   // NaN -> 0 (ordered compare)
        if (v > 65504.0f) v = 65504.0f;                       // +Inf -> largest finite fp16 (W5-2 NaN/Inf note; Reinhard Inf/(1+Inf) would be NaN)
        c[k] = tonemap == 1 ? v / (1.0f + v) : (v < 1.0f ? v : 1.0f);
    }
    const float a = e[3] != e[3] ? 0.0f : e[3] > kMinCoverage ? (e[3] < 1.0f ? e[3] : 1.0f) : kMinCoverage;
    sg::Px p; p.r = c[0] * fade; p.g = c[1] * fade; p.b = c[2] * fade; p.a = a * fade;
    return p;
}
// Same cockpit-band rule as the smoke (sg::smoke_applies); composited AFTER the smoke (over it), in display space (above).
inline sg::Px sprite_over(const sg::Px& under, const sg::Px& s, float dz, bool model_cockpit) noexcept {
    if (!sg::smoke_applies(dz, model_cockpit)) return under;
    if (s.r == 0.0f && s.g == 0.0f && s.b == 0.0f && s.a == 0.0f) return under;
    const float k = 1.0f - s.a;
    auto ch = [k](float sv, float uv) { const float d = sv + k * srgb_encode(uv); return srgb_decode(d < 1.0f ? d : 1.0f); };
    sg::Px o; o.r = ch(s.r, under.r); o.g = ch(s.g, under.g); o.b = ch(s.b, under.b);
    const float a = s.a + k * under.a; o.a = a > 0.0f ? (a < 1.0f ? a : 1.0f) : 0.0f;
    return o;
}

// ---- log lines (step5-producer.log; < 400 bytes for any values, offline-tested)
// (I) `light` = explosion replays drawn with the light PSO; `xpso_why` = why the last explosion of the frame used the shared clone (nullptr: none did).
inline int format_frame(char* buf, size_t n, uint64_t frame, const FrameState& s, unsigned fallback_window, unsigned explosion, unsigned sprites,
                        unsigned light = 0, const char* xpso_why = nullptr) noexcept {
    auto cu = [](unsigned v) { return v > 99999u ? 99999u : v; };
    return std::snprintf(buf, n, "fx-sprite-gpu;frame=%llu;replays=%u(explosion:%u,sprite:%u);committed=%u;refused=%u;window-fallback=%u;cleared=%d;valid=%d;"
        "last-why=%.28s;xpso=light:%u,shared:%.20s;mode=gpu-replay(per-eye,eye-vb-copy,eye-objects-depth-test,effects-predicate,window-sprite-suppressed,"
        "display-composite)",
        static_cast<unsigned long long>(frame), cu(s.replays), cu(explosion), cu(sprites), cu(s.committed), cu(s.refused), cu(fallback_window),
        s.cleared ? 1 : 0, s.valid() ? 1 : 0, s.last_why ? s.last_why : "none", cu(light), xpso_why ? xpso_why : "none");
}
inline int format_ring(char* buf, size_t n, bool ready, const char* why, unsigned slots, uint32_t format, uint64_t w, uint32_t h, uint64_t charged) noexcept {
    return std::snprintf(buf, n, "fx-sprite-ring;outcome=%s;reason=%.32s;slots=%u;format=%u;size=%llux%u;charged=%llu;caps=image:%llu,total:%llu,draws:%u,vtx:%u;"
        "per-slot=2x(sprite-rt)+upload:%llu;dsv=eye-objects-depth(test-only);per-draw=%u(+%u-clears-once);lifetime=process(ring)",
        ready ? "ready" : "refused", why ? why : "none", slots, format, static_cast<unsigned long long>(w), h, static_cast<unsigned long long>(charged),
        static_cast<unsigned long long>(kImageCap), static_cast<unsigned long long>(kTotalCap), kMaxDraws, kMaxVertices,
        static_cast<unsigned long long>(kSlotUpload), kCmdsPerDraw, kCmdsClear);
}

} // namespace acz::fxsprite::gpu
