#pragma once
// fx_puff_trail.h -- EFFECTS-OUTSIDE (M, headset test 5 2026-10-30c item 5; M4, headset test 6 2026-10-30d items 5-6): a WORLD-SPACE memory of the game's
// camera-facing PUFF cards (the GREATER 4caf6e2d dual-blend quads, class 1 of the fxmem:6 sprite replay): damaged-aircraft SMOKE (fxtrail) and the game's own
// CLOUD puffs (fxcloud, M4), so that what the game camera has left behind keeps existing outside the game's forward rectangle, drawn with the game's own puff
// art. docs/world-render/effects-outside-window.md sections 19 and 20. Header only (no D3D12, no fx_memory.cpp: fx_sprite_replay.h maths).
//
// Why (section 19.1, evidence): the game submits every constant-depth puff quad already CLIPPED to its own window (x 1792 / 2304, y 1824 / 2272 in GS units)
// and culls the ones outside it. Re-projecting what the game drew can only ever show the part its camera saw. The game camera does NOT follow the head (look:1
// is the WORLD-4 colour / haze look; the look drive is parked, D-291, and lookstick:block centres the right stick): it looks along the aircraft nose, so the
// memory fills with what the nose has swept (section 20.2).
//
// What this keeps (per card, from the last frame the game drew it WHOLE: no vertex on or beyond the border):
//   kind (smoke / cloud), centre (world, the mean of the 4 unprojected corners), half extents along the game screen x / y, the edge, the 4 game vertices as a
//   template (colour, alpha, uv, second colour, the z / q form and the observation depth), the corner signs, the texture binding key (the draw's first
//   descriptor-table root argument), the host identity (the game PSO and the descriptor heaps the key belongs to), the frame / time it was last seen whole,
//   how many frames it was seen whole.
// Rules (fail closed; every refusal counted):
//   * observe (every class-1 draw the CPU plan reads, fxmem:6): a whole, constant-depth 4-vertex strip quad (a CARD: aspect below fs::kStreakAspect, edge
//     >= kMinEdge) refreshes the nearest stored card of its key that was not yet refreshed this frame and lies within kMatchFrac x the larger edge (M4:
//     ONE card refreshes ONE puff per frame; the M1 radius of 0.5 x the edge merged the neighbouring puffs of a trail into one that hopped between them);
//     a card at the position of a puff already refreshed this frame (within kDupFrac x the edge) is the same card drawn twice (dup, nothing stored);
//     otherwise it is stored as NEW: SMOKE when born at an aircraft (kClsNear: within max(fxtnear, 0.75 x edge) of a world object drawn this frame, the
//     player's own aircraft at the game camera excluded) or, kClsSize, when its edge <= fxtmax; a CLOUD (fxcloud) when it is not smoke and its edge lies in
//     [kCloudMinEdge, kCloudMaxEdge]. Each kind has its own capacity (kCap smoke, kCloudCap cloud); a full kind evicts its card seen longest ago;
//   * a CLIPPED quad whose world centre lies on a stored card of the same key that is re-emitted this frame is collapsed in the eye copies (the whole stored
//     card replaces the game's cut part; no double);
//   * select (at every HOST draw = a class-1 draw replayed per eye, with the frame's game map; M4): every stored card that (a) was seen whole on >= kMinSeen
//     frames (a spark that never holds still is never re-emitted), (b) is not wholly inside the game window in front of its camera (the game draws that one
//     itself; M1 used "not drawn whole in this or the previous frame", which re-emitted a dissipated puff as a ghost for two frames), (c) is not emitted yet
//     this frame, (d) can be drawn under this host: the host's own key, or (xkey) the same game PSO and descriptor heaps with its key bound by a game class-1
//     draw EARLIER IN THE SAME FRAME under the same heaps (review 2026-10-31 F1: M4 allowed a key bound within 120 frames, but the key is a raw GPU descriptor
//     handle: once the game stops binding it, its slot may be rewritten (ring / per-frame slots: wrong texture) or describe a released resource (GPU page
//     fault). A key the game binds this frame describes, at execution, exactly what the game's own draw reads. A card whose key the game no longer binds is
//     drawn only under its own key's host again), (e) with the eye map: projects in front of the eye overlapping its window. Over the cap the
//     nearest are kept. The selection is ordered by key (the host's first), then far to near (alpha blending).
//   * build_eye: each selected card becomes a BILLBOARD facing the eye, corners projected with the eye A'_e exactly as fx_sprite_replay.h reproject does
//     (z form z' = depth_scale x (Z0 / d' + b), q' = q d / d'; q form z' = 0, q' = 1 / d'), alpha x the fade (smoke: 1 - age / life; cloud: 1 until the last
//     kCloudTail of its life, a distance fade from kFarFadeFrom x fxcfar to fxcfar (the EDGE of the cache) and a near fade under kNearFade1 x its edge).
//     One group (key, first vertex, count) per key so that the GPU draw can bind each key's texture table.
//   * end of frame: a card the game did not draw for kCullFrames frames although it lies wholly inside the game window in front of the game camera is dropped
//     (the game removed it); a card older than its kind's life (now - last seen whole) expires; a cloud beyond kCloudDropFar x fxcfar from the game camera is
//     dropped (gone); a frame with a card that wanted to be drawn but no host draw at all is counted (nohost).
// Limits: a stored card is frozen at its last whole observation (drift / growth after it is not followed); only cards the game drew whole at least twice are
// kept; a frame without any class-1 host draw draws none of the stored cards (counted: nohost); the texture identity is the binding key, not its contents.
// (M5, headset test 7 2026-10-31, docs section 22) a card the game still draws CUT at its window border is still there: the matched cut quad renews its
// life (renew; M4 let such a cloud expire 120 s after its last WHOLE sighting while the game kept drawing it); a stored SMOKE card is refreshed within one
// edge (kSmokeMatchFrac; M4: 0.25 x the edge, and no smoke card of that session was ever refreshed, so none reached kMinSeen or was re-emitted) or within
// kCamSlack x the game camera's step of the frame; a wanted card whose key no game draw binds in the frame cannot be drawn at all (F1) and is counted
// (unbound): the cloud popping that only a texture copy of our own can remove (section 22.4).
// (M6, fxatlas:1, docs section 23; fx_puff_atlas.h) that texture copy: a card sighted in a host draw gets a tile of our own atlas (captured from its key
// while the game binds it); a wanted card whose key no game draw binds this frame is drawn from its READY tile under any host of the same game PSO that can
// draw the atlas (Host::atlas), in its own group (atlas::kGroupKey, uv remapped into the tile). unbound now counts only cards with neither a bound key nor
// a ready tile; amiss = cards with a ready tile that no atlas-capable host took.
// (M7, headset test 8 2026-11-01, docs section 25; card_axes) the billboard no longer uses the eye basis: it faces the eye POINT with the world's up
// (+Y), so neither the aircraft's roll nor the head's roll turns a stored cloud / smoke card; near the game window it takes the game camera's roll
// (weighted, kSeamBand) so that the hand-over to the game's own screen-aligned card at the border does not jump.
#include "fx_sprite_replay.h"
#include "fx_puff_atlas.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace acz::fxpuff {

namespace fs = acz::fxsprite;
using fxmem::Vtx;

enum Kind : uint8_t { kSmoke = 0, kCloud = 1 };
inline constexpr unsigned kCap = 128;                         // stored smoke puffs
inline constexpr unsigned kCloudCap = 384;                    // (M4) stored cloud cards
inline constexpr unsigned kSlots = kCap + kCloudCap;          // ~210 KB (heap: Step5State)
inline constexpr unsigned kMaxEmit = 160;                     // cards re-emitted per host draw (every kind, every key)
inline constexpr unsigned kVertsPerPuff = 6;                  // TRIANGLELIST (0,1,2), (2,1,3)
inline constexpr unsigned kMaxEmitVerts = kMaxEmit * kVertsPerPuff;   // 960 vertices = 60 KiB per eye
inline constexpr unsigned kMaxKeys = 16;                      // binding keys remembered (alive table) and groups per host draw
inline constexpr unsigned kCullFrames = 3;                    // frames unseen while wholly inside the game window before a card is dropped
inline constexpr double kMatchFrac = 0.25;                    // (M4) refresh: centres within 0.25 x the larger edge (M1: 0.5)
inline constexpr double kSmokeMatchFrac = 1.0;                // (M5) a stored SMOKE card: within one edge (a drifting / growing puff; still one card, one puff per frame)
inline constexpr double kCamSlack = 1.25;                     // (M5) a stored SMOKE card: or within 1.25 x the game camera's step of this frame
inline constexpr double kMaxCamStep = 1000.0;                 // (M5) world units: a larger step (a camera cut / a skipped frame) adds no slack
inline constexpr double kDupFrac = 0.02;                      // (M4) the same card drawn twice in one frame
inline constexpr unsigned kMinSeen = 2;                       // (M4) whole observations (distinct frames) before a card may be re-emitted
inline constexpr uint64_t kKeyAliveFrames = 0;                // (F1) xkey re-binds only a key a game draw bound in the SAME frame (M4: 120 = stale handles)
inline constexpr double kSelfRadius = 300.0;                  // (M4) world objects this close to the game camera (the player's aircraft) are not "near"
// (M7, headset test 8 2026-11-01, docs section 25) plume continuation: a smoking aircraft's puffs are born at the aircraft, and the game submits only the ones
// inside its window (it culls / clips the rest on the CPU), so a plume whose emitter has left the window was never seen and nothing could be remembered. A STREAM
// follows the newest whole smoke card born at a world object (position + velocity per frame); while its predicted position is not wholly inside the game window
// it spawns one synthetic card per frame from that card (the game's own art), for at most kStreamMaxFrames after the last real birth.
inline constexpr unsigned kStreams = 4;                       // emitters followed
inline constexpr unsigned kStreamMinBirths = 6;               // real births before a stream may spawn (a one-off spark is not a plume)
inline constexpr uint64_t kStreamMaxFrames = 240;             // longest extrapolation after the last real birth (4 s at 60 Hz)
inline constexpr uint64_t kStreamQuiet = 8;                   // frames without a birth while the predicted card lies wholly inside the window: the emitter stopped
inline constexpr double kStreamGatherMin = 1500.0, kStreamMaxSpeed = 2000.0;   // world units: gather radius floor; a faster head is a different emitter
inline constexpr double kBorderEps = 0.01;                    // GS units: a vertex this close to the window edge is ON the border (the game's clip)
inline constexpr double kWholeMargin = 2.0;                   // GS units: a re-projected card counts as wholly inside the game window with this margin
inline constexpr double kEyeMargin = 32.0;                    // GS units: an eye card overlapping the eye window grown by this is visible
inline constexpr double kDefLifeS = 8.0, kMinLifeS = 1.0, kMaxLifeS = 30.0;
inline constexpr double kDefMaxEdge = 1500.0, kMinMaxEdge = 50.0, kMaxMaxEdge = 100000.0;
inline constexpr double kMinEdge = 1.0;                       // world units: below this a quad is a speck, not a puff
// (M4) the cloud class: fxcloud:<s> (life), fxcfar:<u> (the cache radius: distance fade to it, drop beyond kCloudDropFar x it)
inline constexpr double kCloudMinEdge = 1000.0, kCloudMaxEdge = 30000.0;   // every whole cloud card in the archive: 3,800-11,000
inline constexpr double kDefCloudLifeS = 120.0, kMinCloudLifeS = 10.0, kMaxCloudLifeS = 600.0;
inline constexpr double kDefCloudFar = 40000.0, kMinCloudFar = 5000.0, kMaxCloudFar = 200000.0;
inline constexpr double kCloudDropFar = 1.25, kFarFadeFrom = 0.75, kCloudTail = 0.2, kNearFade0 = 0.25, kNearFade1 = 0.75;
// (M3) the classification of a NEW whole card:
//   kClsNear (default): SMOKE is born at an aircraft: within max(near, 0.75 x its edge) of a world object drawn this frame (DrawACM record translation).
//   kClsSize: the M1 rule (edge <= fxtmax), kept for comparison runs.
enum Cls : int { kClsNear = 0, kClsSize = 1 };
inline constexpr double kNearMaxEdge = 20000.0;
inline constexpr double kDefNear = 2500.0, kMinNear = 100.0, kMaxNear = 50000.0;
inline constexpr unsigned kObjCap = 64;                       // world objects remembered per frame for the near rule
// (M4c, POV run runPOV2b f5207: a game cloud card ending in a hard vertical edge at the game-window border, step146: ~1/4 of the clipped cards had a whole
// stored copy to replace them) fxtclip: what happens to a game card CUT at the window border that no re-emitted whole copy replaces this frame:
//   kClipFade (default): the vertices ON the border get alpha 0 in the eye copies, so the card fades out toward the cut instead of ending in a straight
//     edge. PS 4caf6e2d (ps2AtestCA_p) = texture x COLOR0 (alpha = t.a x COLOR0.a), discard only below 1/255 [E: DXIL step97 X2-shaders], so the
//     interpolated vertex alpha is a smooth ramp across the visible part; the part inside the window keeps most of its cover;
//   kClipHide: the whole cut card is collapsed (missing rather than cut);
//   kClipOff: the game's cut card is left as drawn (M4).
// Applies to cloud-size cut cards (edge >= kCloudMinEdge) while fxcloud is on and to smoke-class cut cards (near an object / size rule) while fxtrail is on.
enum ClipMode : int { kClipOff = 0, kClipFade = 1, kClipHide = 2 };
// (M7, docs section 25) the orientation of a re-emitted card (card_axes below): eye basis (M4..M6) / world up / world up + the game's roll at the seam.
enum UpMode : int { kUpEye = 0, kUpWorld = 1, kUpSeam = 2 };
inline const char* clip_name(int m) noexcept { return m == kClipOff ? "off" : m == kClipHide ? "hide" : "fade"; }

struct Options {
    bool on = false;                // smoke || cloud: the memory runs
    bool smoke = false;             // fxtrail > 0
    double life_ms = kDefLifeS * 1000.0;
    double max_edge = kDefMaxEdge;
    int cls = kClsNear;
    double near_units = kDefNear;   // (never `near` / `far`: windef.h macros)
    bool cloud = false;             // (M4) fxcloud > 0
    double cloud_life_ms = kDefCloudLifeS * 1000.0;
    double cloud_far = kDefCloudFar;
    bool xkey = true;               // (M4) fxtxkey:1 (default): a card may be drawn inside another key's host draw (same PSO + heaps, its key alive)
    int clip = kClipFade;           // (M4c) fxtclip: the cut cards without a whole copy
    int up = kUpSeam;               // (M7) card orientation (UpMode); no cfg key: kUpSeam in the DLL, the other modes for tests
    bool atlas = false;             // (M6) fxatlas:1: cards are captured into our atlas and drawn from it when their key is not bound (fx_puff_atlas.h)
    double size_cap() const noexcept { return cls == kClsSize ? max_edge : kNearMaxEdge; }
    double life_of(int kind) const noexcept { return kind == kCloud ? cloud_life_ms : life_ms; }
};
// From the stage switches (lives in seconds, 0 = off).
inline Options options(double life_s, double max_edge, int cls = kClsNear, double near_units = kDefNear, double cloud_s = 0.0,
                       double cloud_far = kDefCloudFar, bool xkey = true, int clip = kClipFade, bool atlas = false) noexcept {
    Options o;
    o.smoke = life_s > 0.0 && std::isfinite(life_s);
    o.life_ms = (o.smoke ? life_s : kDefLifeS) * 1000.0;
    o.max_edge = max_edge > 0.0 && std::isfinite(max_edge) ? max_edge : kDefMaxEdge;
    o.cls = cls == kClsSize ? kClsSize : kClsNear;
    o.near_units = near_units > 0.0 && std::isfinite(near_units) ? near_units : kDefNear;
    o.cloud = cloud_s > 0.0 && std::isfinite(cloud_s);
    o.cloud_life_ms = (o.cloud ? cloud_s : kDefCloudLifeS) * 1000.0;
    o.cloud_far = cloud_far > 0.0 && std::isfinite(cloud_far) ? cloud_far : kDefCloudFar;
    o.xkey = xkey;
    o.clip = clip == kClipOff || clip == kClipHide ? clip : kClipFade;
    o.on = o.smoke || o.cloud;
    o.atlas = atlas && o.on;
    return o;
}
inline const char* cls_name(int cls) noexcept { return cls == kClsSize ? "size" : "near"; }
// The distance from p to the nearest of n object positions (huge when n = 0); (M4) objects within kSelfRadius of `self` (the game camera) are skipped.
inline double nearest_object(const double p[3], const float (*obj)[3], unsigned n, const double* self = nullptr) noexcept {
    double best = 1e300;
    for (unsigned i = 0; obj && i < n; ++i) {
        if (self) {
            const double sx = self[0] - obj[i][0], sy = self[1] - obj[i][1], sz = self[2] - obj[i][2];
            if (sx * sx + sy * sy + sz * sz <= kSelfRadius * kSelfRadius) continue;
        }
        const double dx = p[0] - obj[i][0], dy = p[1] - obj[i][1], dz = p[2] - obj[i][2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best) best = d;
    }
    return best;
}
inline double dist3(const double a[3], const double b[3]) noexcept {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
inline double smooth01(double t) noexcept { t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3.0 - 2.0 * t); }

// ---- one quad
inline bool on_border(double x, double y) noexcept {
    const double x0 = fxmem::kGsCentre - fxmem::kGsHalfW, x1 = fxmem::kGsCentre + fxmem::kGsHalfW;
    const double y0 = fxmem::kGsCentre - fxmem::kGsHalfH, y1 = fxmem::kGsCentre + fxmem::kGsHalfH;
    return std::fabs(x - x0) < kBorderEps || std::fabs(x - x1) < kBorderEps || std::fabs(y - y0) < kBorderEps || std::fabs(y - y1) < kBorderEps ||
           !fxmem::in_window(x, y);
}
struct Quad {
    bool ok = false;          // every vertex has a world point
    bool constant = false;    // one depth for the 4 vertices (a camera-facing card)
    bool clipped = false;     // a vertex on / beyond the window border
    bool card = false;        // (M4) whole + constant + card shape + edge >= kMinEdge (any size)
    bool puff = false;        // card + edge <= the smoke size cap (the M1 meaning)
    double c[3] = {}, w[4][3] = {}, hu = 0, hv = 0, depth = 0, edge = 0;
    int8_t sx[4] = {}, sy[4] = {};   // +1 = right of / below the quad's screen centre
    bool zform = false;
};
// `q4` = the quad's 4 vertices in strip order (a, b, c, d: a-b and c-d along the first edge, a-c / b-d along the second).
inline Quad analyse_quad(const fxmem::ScreenMap& game, const Vtx* const q4[4], const Options& o) noexcept {
    Quad q;
    if (!game.ok) return q;
    double d[4];
    for (int k = 0; k < 4; ++k) {
        fxmem::DepthSource src;
        d[k] = fs::sp_vertex_depth(game, q4[k]->pos[2], q4[k]->pos[3], src);
        if (src == fxmem::depth_none || !(d[k] > 0)) return q;
        if (k == 0) q.zform = src == fxmem::depth_z;
        else if (q.zform != (src == fxmem::depth_z)) return q;
        if (!fs::sp_unproject(game, q4[k]->pos[0], q4[k]->pos[1], d[k], q.w[k])) return q;
        if (on_border(q4[k]->pos[0], q4[k]->pos[1])) q.clipped = true;
    }
    q.ok = true;
    double dmin = d[0], dmax = d[0];
    for (int k = 1; k < 4; ++k) { dmin = std::fmin(dmin, d[k]); dmax = std::fmax(dmax, d[k]); }
    q.constant = dmax - dmin <= 1e-4 * dmax;
    q.depth = 0.25 * (d[0] + d[1] + d[2] + d[3]);
    for (int c = 0; c < 3; ++c) q.c[c] = 0.25 * (q.w[0][c] + q.w[1][c] + q.w[2][c] + q.w[3][c]);
    auto len = [&](int i, int j) { return dist3(q.w[i], q.w[j]); };
    const double eu = 0.5 * (len(0, 1) + len(2, 3)), ev = 0.5 * (len(0, 2) + len(1, 3));
    q.edge = std::fmax(eu, ev);
    // the corner signs on the GAME screen (x right, y down) and the half extents along screen x / y
    double cx = 0, cy = 0;
    for (int k = 0; k < 4; ++k) { cx += 0.25 * q4[k]->pos[0]; cy += 0.25 * q4[k]->pos[1]; }
    unsigned seen = 0;
    for (int k = 0; k < 4; ++k) {
        q.sx[k] = q4[k]->pos[0] > cx ? 1 : -1; q.sy[k] = q4[k]->pos[1] > cy ? 1 : -1;
        seen |= 1u << ((q.sx[k] > 0 ? 1 : 0) + (q.sy[k] > 0 ? 2 : 0));
    }
    // the first edge runs along screen x when a and b differ in x sign
    const bool uIsX = q.sx[0] != q.sx[1];
    q.hu = 0.5 * (uIsX ? eu : ev); q.hv = 0.5 * (uIsX ? ev : eu);
    const bool shape = seen == 0xFu && q.hu > 0 && q.hv > 0 && std::fmax(eu, ev) / std::fmax(std::fmin(eu, ev), 1e-12) < fs::kStreakAspect;
    q.card = !q.clipped && q.constant && shape && q.edge >= kMinEdge;
    q.puff = q.card && q.edge <= o.size_cap();
    return q;
}

// ---- the camera basis of a map (right, up, forward; orthonormalised: the eye maps carry an asymmetric frustum)
inline bool basis(const fxmem::ScreenMap& m, double R[3], double U[3], double F[3]) noexcept {
    if (!m.ok) return false;
    double r[3], u[3], f[3];
    for (int i = 0; i < 3; ++i) { f[i] = m.a[i * 4 + 3]; r[i] = m.a[i * 4 + 0] - fxmem::kGsCentre * f[i]; u[i] = -(m.a[i * 4 + 1] - fxmem::kGsCentre * f[i]); }
    auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto nrm = [&](double* a) { const double l = std::sqrt(dot(a, a)); if (!(l > 1e-30) || !std::isfinite(l)) return false; for (int i = 0; i < 3; ++i) a[i] /= l; return true; };
    if (!nrm(f)) return false;
    double k = dot(r, f); for (int i = 0; i < 3; ++i) r[i] -= k * f[i];
    if (!nrm(r)) return false;
    k = dot(u, f); const double k2 = dot(u, r); for (int i = 0; i < 3; ++i) u[i] -= k * f[i] + k2 * r[i];
    if (!nrm(u)) return false;
    for (int i = 0; i < 3; ++i) { R[i] = r[i]; U[i] = u[i]; F[i] = f[i]; }
    return true;
}

// (M4) the game draw that hosts the re-emission: its binding key, its game PSO and its descriptor heaps (opaque identities, never dereferenced here).
// (M6) atlas: the producer can draw the atlas group under this host (resources ready, the root tables checked, fx_puff_atlas.h host_why).
struct Host { uint64_t key = 0, pso = 0, heap = 0; bool atlas = false; };
// (M4) one key's run of vertices in an eye copy (the GPU draw binds the key's texture table for it).
struct Group { uint64_t key = 0; uint32_t first = 0, count = 0; };

struct Puff {
    bool used = false;
    uint8_t kind = kSmoke;
    uint64_t key = 0, pso = 0, heap = 0;
    double c[3] = {}, hu = 0, hv = 0, depth = 0, edge = 0;
    Vtx tmpl[4] = {};
    int8_t sx[4] = {}, sy[4] = {};
    bool zform = false;
    uint64_t full_frame = 0, clip_frame = 0, emit_frame = 0, refresh_frame = 0, seen_ms = 0, born_ms = 0;
    double c0[3] = {};        // (M4b) the world centre at birth: a refresh measures the drift from it (cdrift: a world-static card stays near 0)
    bool emit_x = false;      // (M4b) selected this frame under another key's host (unselect takes it back out of xkey)
    bool emit_atlas = false;  // (M6) selected this frame to be drawn from its atlas tile (group atlas::kGroupKey)
    int16_t tile = -1;        // (M6) its atlas tile (-1 = none; released when the card goes)
    unsigned miss = 0, seen_n = 0;
    bool synth = false;       // (M7) spawned by a plume stream, never seen by the game
};
// The 4 world corners of a puff as a billboard of the basis (R, U): corner k = c + sx hu R - sy hv U (screen y runs down).
inline void corners(const Puff& p, const double R[3], const double U[3], double out[4][3]) noexcept {
    for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) out[k][i] = p.c[i] + double(p.sx[k]) * p.hu * R[i] - double(p.sy[k]) * p.hv * U[i];
}
// ---- (M7, headset test 8 2026-11-01: "clouds ... roll when I roll the aircraft", "missile smoke sprites rotate with my hmd"; docs section 25)
// The ORIENTATION of a re-emitted card. M4..M6 built every card in the EYE basis (corners(x, eR, eU)): the eye pose is the cockpit pose x the HMD pose, so
// the card took the aircraft's roll AND the head's roll. The stored card is a sphere-like puff anchored at a world point; nothing about it is attached to
// the viewer, so its orientation must not be either:
//   kUpWorld: the card faces the EYE POINT (its normal = the ray eye -> centre, not the eye's forward: a head turn / roll does not turn it at all) and its
//     up is the world +Y (stage frame, +Y up: pov_render.h, cockpit_model.h) projected perpendicular to that ray. Within kPoleSin of straight up / down
//     (|up_perp| < 0.05, ~3 degrees) the eye's up is blended in, continuously, so a card overhead never spins on a degenerate axis;
//   kUpSeam (default): kUpWorld, except near the game window: the game's own in-window cards are screen-aligned in the GAME camera (fx_sprite_replay.h:
//     planar cards facing the game camera, which in the cockpit carries the aircraft's roll, never the head's). A card whose centre projects inside the
//     game window or within kSeamBand x the window half extent beyond its border takes the game camera's roll, weighted 1 at the border down to 0 at the
//     band's edge (smoothstep), so a card handed over to / from the game at the border does not jump in roll. Away from the nose: pure world up;
//   kUpEye: M4..M6 (the eye basis; kept for comparison tests only).
// The eye's roll (HMD roll) never enters except inside the pole cone; the aircraft's roll enters only inside the seam band.
inline constexpr double kWorldUp[3] = {0.0, 1.0, 0.0};
inline constexpr double kSeamBand = 0.5;   // in game-window half extents beyond the border
inline constexpr double kPoleSin = 0.05;   // |world up perpendicular to the ray| below this: the eye's up blends in
// The seam weight of a world point: 1 inside the game window (in front of the game camera), smoothly to 0 at kSeamBand half extents beyond its border.
inline double seam_weight(const fxmem::ScreenMap& game, const double c[3]) noexcept {
    double xs = 0, ys = 0, d = 0;
    if (!game.ok || !fs::sp_project(game, c, xs, ys, d) || !(d > 1.0)) return 0.0;
    const double ox = (std::fabs(xs - fxmem::kGsCentre) - fxmem::kGsHalfW) / fxmem::kGsHalfW;
    const double oy = (std::fabs(ys - fxmem::kGsCentre) - fxmem::kGsHalfH) / fxmem::kGsHalfH;
    const double o = std::fmax(0.0, std::fmax(ox, oy));
    if (!std::isfinite(o)) return 0.0;
    return 1.0 - smooth01(o / kSeamBand);
}
// The card axes (R right, U up; the corners() basis) of a card centred at `c` seen from the eye point E with the eye basis (eR, eU, eF), the game map and
// the game camera's up gU (nullptr: no seam blend). `wseam` (optional) receives the seam weight used. false only for non-finite input (then R, U = the
// eye basis: the M4 behaviour).
inline bool card_axes(const double c[3], const double E[3], const double eR[3], const double eU[3], const double eF[3], const fxmem::ScreenMap* game,
                      const double* gU, int mode, double R[3], double U[3], double* wseam = nullptr) noexcept {
    if (wseam) *wseam = 0.0;
    auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto cross = [](const double* a, const double* b, double* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; };
    auto nrm = [&](double* a) { const double l = std::sqrt(dot(a, a)); if (!(l > 1e-12) || !std::isfinite(l)) return false; for (int i = 0; i < 3; ++i) a[i] /= l; return true; };
    auto eye_basis = [&]() { for (int i = 0; i < 3; ++i) { R[i] = eR[i]; U[i] = eU[i]; } };
    if (mode == kUpEye) { eye_basis(); return true; }
    double f[3] = {c[0] - E[0], c[1] - E[1], c[2] - E[2]};
    if (!nrm(f)) { for (int i = 0; i < 3; ++i) f[i] = eF[i]; if (!nrm(f)) { eye_basis(); return false; } }
    // the handedness of the map's basis (the test maps and the game's differ): R = hand x (F x U)
    double fu[3]; cross(eF, eU, fu);
    const double hand = dot(fu, eR) >= 0.0 ? 1.0 : -1.0;
    double u[3]; const double k = dot(kWorldUp, f);
    for (int i = 0; i < 3; ++i) u[i] = kWorldUp[i] - k * f[i];
    const double s = std::sqrt(dot(u, u));
    if (s < kPoleSin) {   // near straight up / down: the eye's up (perpendicular to the ray) blends in, continuously (weight 0 at s = kPoleSin)
        double ue[3]; const double ke = dot(eU, f);
        for (int i = 0; i < 3; ++i) ue[i] = eU[i] - ke * f[i];
        if (nrm(ue)) { const double t = 1.0 - s / kPoleSin; for (int i = 0; i < 3; ++i) u[i] += t * kPoleSin * ue[i]; }
    }
    if (!nrm(u)) { eye_basis(); return false; }
    if (mode == kUpSeam && game && gU) {
        const double w = seam_weight(*game, c);
        double g[3]; const double kg = dot(gU, f);
        for (int i = 0; i < 3; ++i) g[i] = gU[i] - kg * f[i];
        if (w > 0.0 && nrm(g)) {   // rotate u about the ray toward the game camera's up by w x the angle between them
            double ux[3]; cross(u, g, ux);
            const double th = w * std::atan2(dot(ux, f), dot(u, g));
            double fx[3]; cross(f, u, fx);
            const double ct = std::cos(th), st = std::sin(th);
            for (int i = 0; i < 3; ++i) u[i] = u[i] * ct + fx[i] * st;
            if (!nrm(u)) { eye_basis(); return false; }
            if (wseam) *wseam = w;
        }
    }
    double r[3]; cross(f, u, r);
    for (int i = 0; i < 3; ++i) { R[i] = hand * r[i]; U[i] = u[i]; }
    for (int i = 0; i < 3; ++i) if (!std::isfinite(R[i]) || !std::isfinite(U[i])) { eye_basis(); return false; }
    return true;
}

// (M4) the alpha factor of a stored card seen from distance `d` at time `now` (0 = invisible).
inline double fade(const Puff& x, uint64_t now_ms, double d, const Options& o) noexcept {
    const double life = o.life_of(x.kind), age = double(now_ms - x.seen_ms);
    if (!(life > 0) || age >= life) return 0.0;
    if (x.kind != kCloud) return 1.0 - age / life;
    const double t0 = (1.0 - kCloudTail) * life;
    double f = age <= t0 ? 1.0 : 1.0 - (age - t0) / (life - t0);
    const double far1 = o.cloud_far, far0 = kFarFadeFrom * o.cloud_far;
    if (d >= far1) return 0.0;
    if (d > far0) f *= smooth01((far1 - d) / (far1 - far0));
    const double n0 = kNearFade0 * x.edge, n1 = kNearFade1 * x.edge;
    if (d <= n0) return 0.0;
    if (d < n1) f *= smooth01((d - n0) / (n1 - n0));
    return f;
}

struct Stats {   // one frame (Memory::end_frame), a log window (the producer adds frames) or the run (Memory::run)
    uint32_t observed = 0, stored = 0, refreshed = 0, clipped = 0, collapsed = 0, culled = 0, expired = 0, evicted = 0, selected = 0, emitted = 0,
             behind = 0, refused = 0;
    // (M2) the quad census of observe (see format_line)
    uint32_t draws = 0, nokey = 0, runs = 0, quads = 0, nonconst = 0, whole = 0, big = 0, streak = 0, tiny = 0;
    double edge_max = 0;
    // (M3) wnear / wfar: whole cards near / not near a world object (far = whole cards stored as nothing); objs; nocam; edge_min
    uint32_t wnear = 0, wfar = 0, objs = 0, nocam = 0;
    double edge_min = 0;
    // (M4) cstored / crefreshed: cloud cards stored new / refreshed; dup: a card drawn twice in a frame; hosts: host draws that selected; nohost: frames
    // with a card that wanted to be drawn and no host draw; xkey: cards emitted inside another key's host draw; nomatch: wanted cards a host could not
    // take (other PSO / heaps, key not alive, xkey off); offeye: wanted cards outside the eye; gone: clouds beyond the cache; cexpired / cemitted: clouds.
    uint32_t cstored = 0, crefreshed = 0, dup = 0, hosts = 0, nohost = 0, xkey = 0, nomatch = 0, offeye = 0, gone = 0, cexpired = 0, cemitted = 0;
    // (M4b, step146 null runs) cdrift: the largest distance of a refreshed CLOUD card from its birth centre. cref >> cst alone does not prove the cards are
    // world-static: a card that moved < 0.25 x its edge per frame (a camera-attached field) would also be refreshed every frame, its stored centre following it.
    double cdrift = 0;
    // (M4c) efade / ehide: cut cards softened (border vertices alpha 0) / hidden because no whole copy replaced them this frame
    uint32_t efade = 0, ehide = 0;
    // (M5, headset test 7) want: wanted card-frames (end_frame: seen whole >= kMinSeen, not shown whole by the game); unbound: of them, not drawn because
    // no game draw bound their key in the frame (F1: undrawable: the popping); renew: life renewals by a CUT sighting; kchurn: whole cards that matched no
    // stored card of their key but lie on a card of ANOTHER key (the game re-bound that card's texture under a new key); swide / scam: smoke refreshes
    // beyond 0.25 x the edge within one edge / beyond one edge (camera slack only)
    uint32_t want = 0, unbound = 0, renew = 0, kchurn = 0, swide = 0, scam = 0;
    // (M6) adrawn: cards selected to be drawn from the atlas; amiss: wanted card-frames with a ready tile, not emitted (no atlas-capable host of their PSO)
    uint32_t adrawn = 0, amiss = 0;
    // (M7) synth: synthetic plume cards spawned; streams: emitters followed at the end of the frame (a window / run keeps the largest)
    uint32_t synth = 0, streams = 0;
    const char* last_why = nullptr;
    void add(const Stats& s) noexcept {
        observed += s.observed; stored += s.stored; refreshed += s.refreshed; clipped += s.clipped; collapsed += s.collapsed; culled += s.culled;
        expired += s.expired; evicted += s.evicted; selected += s.selected; emitted += s.emitted; behind += s.behind; refused += s.refused;
        draws += s.draws; nokey += s.nokey; runs += s.runs; quads += s.quads; nonconst += s.nonconst; whole += s.whole; big += s.big;
        streak += s.streak; tiny += s.tiny; if (s.edge_max > edge_max) edge_max = s.edge_max;
        wnear += s.wnear; wfar += s.wfar; objs += s.objs; nocam += s.nocam;
        if (s.edge_min > 0 && (edge_min == 0 || s.edge_min < edge_min)) edge_min = s.edge_min;
        cstored += s.cstored; crefreshed += s.crefreshed; dup += s.dup; hosts += s.hosts; nohost += s.nohost; xkey += s.xkey; nomatch += s.nomatch;
        offeye += s.offeye; gone += s.gone; cexpired += s.cexpired; cemitted += s.cemitted;
        if (s.cdrift > cdrift) cdrift = s.cdrift;
        efade += s.efade; ehide += s.ehide;
        want += s.want; unbound += s.unbound; renew += s.renew; kchurn += s.kchurn; swide += s.swide; scam += s.scam;
        adrawn += s.adrawn; amiss += s.amiss;
        synth += s.synth; if (s.streams > streams) streams = s.streams;
        if (s.last_why) last_why = s.last_why;
    }
};

class Memory {
public:
    Puff p[kSlots];
    Stats fs_{};          // the current frame
    Stats run{};          // the whole run (added at end_frame)
    struct KeySeen { uint64_t key = 0, frame = 0, heap = 0; };   // (F1) heap: the descriptor heaps bound when the game last bound the key
    KeySeen keys[kMaxKeys] = {};   // (M4) the binding keys the game bound on class-1 draws, with the last frame
    struct Cand { uint16_t i; double d; uint64_t g; };   // (M6) g = the card's group key (its own key, or atlas::kGroupKey)
    Cand cand_[kSlots] = {};       // (M4) select scratch
    atlas::Atlas atlas;            // (M6) the tile table of our atlas (fxatlas)
    static constexpr unsigned kSeenCap = 64;
    uint16_t seen_[kSeenCap] = {}; // (M6) the cards the current observe() call stored / refreshed / renewed (capture candidates of this draw)
    unsigned seen_n_ = 0;
    // (M6) the producer slot of the recording being built (atlas::Atlas::use / request); kSlotCap = none
    void set_slot(unsigned s) noexcept { rec_slot_ = s; }
    unsigned slot() const noexcept { return rec_slot_; }
    // (M6) whether stored card x can be drawn from the atlas now (a ready tile of its key and current uv rectangle)
    bool atlas_drawable(const Puff& x) const noexcept {
        if (x.tile < 0) return false;
        atlas::Rect r; return atlas::rect_of(x.tmpl, r) && atlas.drawable(x.tile, x.key, r);
    }
    // (M6) after observe() of a draw the GPU plan replays per eye: request tiles for the cards that draw sighted (key = the draw's key, bound by it).
    // Writes up to `cap` capture jobs (tile + rectangle); returns their count. Nothing when fxatlas is off.
    unsigned plan_captures(uint64_t frame, const Options& o, uint16_t* tiles, atlas::Rect* rects, unsigned cap) noexcept {
        unsigned n = 0;
        if (o.atlas && rec_slot_ < atlas::kSlotCap)
            for (unsigned k = 0; k < seen_n_; ++k) {
                Puff& x = p[seen_[k]];
                if (!x.used) continue;
                // (M6b) only a card that can be re-emitted (seen whole kMinSeen times) gets a tile: sparks and one-frame puffs churned the atlas full
                if (x.tile < 0 && x.seen_n < kMinSeen) continue;
                atlas::Rect r;
                if (!atlas::rect_of(x.tmpl, r)) { int t = x.tile; atlas.release(t); x.tile = -1; ++atlas.c.norect; continue; }
                bool capture = false;
                const bool can = n < cap && tiles && rects;
                const uint32_t full0 = atlas.c.full;
                int t = atlas.request(x.tile, x.key, r, rec_slot_, frame, can, capture);
                if (t < 0 && atlas.c.full != full0) {   // (M6b) full: evict the tile least recently sighted / drawn (its cards keep the key path), retry once
                    const int ev = atlas.evict_candidate(frame);
                    if (ev >= 0) {
                        for (auto& y : p) if (y.used && y.tile == ev) y.tile = -1;
                        atlas.evicted(ev);
                        --atlas.c.full;
                        t = atlas.request(-1, x.key, r, rec_slot_, frame, can, capture);
                    }
                }
                x.tile = int16_t(t);
                if (capture) { tiles[n] = uint16_t(t); rects[n] = r; ++n; }
            }
        seen_n_ = 0;
        return n;
    }

    unsigned live() const noexcept { unsigned n = 0; for (const auto& x : p) n += x.used; return n; }
    unsigned live_of(int kind) const noexcept { unsigned n = 0; for (const auto& x : p) n += x.used && x.kind == kind; return n; }
    void refuse(const char* why) noexcept { ++fs_.refused; fs_.last_why = why; }
    // (M3) a class-1 draw of a frame without a world camera: counted, nothing observed (an older frame's A would misplace this frame's cards).
    void note_nocam() noexcept { ++fs_.nocam; }
    // (M4, F1) whether a game class-1 draw bound `key` in frame `frame` itself (kKeyAliveFrames = 0) under the descriptor heaps `heap`: only then does the
    // handle name, when our draw executes, the descriptors the game's own draw of this frame reads.
    bool key_alive(uint64_t key, uint64_t frame, uint64_t heap) const noexcept {
        for (const auto& k : keys) if (k.key == key && k.key) return frame >= k.frame && frame - k.frame <= kKeyAliveFrames && k.heap == heap;
        return false;
    }
    // (M5) whether any game class-1 draw bound `key` in `frame` (under any heaps): otherwise no host of the frame can draw a card of that key.
    bool key_bound(uint64_t key, uint64_t frame) const noexcept {
        for (const auto& k : keys) if (k.key == key && k.key) return k.frame == frame;
        return false;
    }
    // (M5) the game camera's step from the previous frame to `frame` (0 = unknown, frames not consecutive, or a jump beyond kMaxCamStep).
    double cam_step(uint64_t frame) const noexcept { return cam_frame_ == frame ? cam_step_ : 0.0; }

    // Observe one captured class-1 draw: `v` = the referenced vertices (re-based by lo), `idx` its indices (0xFFFF restarts). `collapse` (n flags, may be
    // nullptr) receives 1 for every vertex of a clipped game quad that a stored card re-emitted this frame replaces. Returns the number of such quads.
    // (M3) obj / nobj: the world positions of the objects drawn this frame (the near rule; may be empty). (M4) pso / heap: the draw's host identity.
    unsigned observe(const fxmem::ScreenMap& game, const Vtx* v, size_t n, const uint16_t* idx, size_t ni, uint32_t lo, bool whole_run, uint64_t key,
                     uint64_t frame, uint64_t now_ms, const Options& o, uint8_t* collapse, const float (*obj)[3] = nullptr, unsigned nobj = 0,
                     uint64_t pso = 0, uint64_t heap = 0, uint8_t* fade = nullptr) noexcept {
        seen_n_ = 0;
        if (!o.on || !game.ok || !v || !idx) return 0;
        ++fs_.draws; fs_.objs += nobj;
        if (!key) { ++fs_.nokey; refuse("trail-binding-key-unknown"); return 0; }
        note_key(key, frame, heap);
        if (whole_run) { ++fs_.runs; return 0; }
        double self[3]; const bool haveSelf = fs::game_eye(game, self);
        note_cam(frame, haveSelf ? self : nullptr);
        unsigned marked = 0;
        size_t s = 0;
        while (s < ni) {
            size_t e = s; while (e < ni && idx[e] != 0xFFFF) ++e;
            if ((e - s) % 4 == 0) for (size_t k = s; k + 3 < e; k += 4) {
                const Vtx* q4[4]; bool okIdx = true;
                for (int j = 0; j < 4; ++j) { const uint16_t ix = idx[k + size_t(j)]; if (ix < lo || ix - lo >= n) { okIdx = false; break; } q4[j] = &v[ix - lo]; }
                if (!okIdx) continue;
                const Quad q = analyse_quad(game, q4, o);
                if (!q.ok) continue;
                ++fs_.quads;
                if (!q.constant) { ++fs_.nonconst; continue; }
                if (!q.clipped) {
                    ++fs_.whole;
                    if (q.edge > fs_.edge_max) fs_.edge_max = q.edge;
                    if (fs_.edge_min == 0 || q.edge < fs_.edge_min) fs_.edge_min = q.edge;
                    const double dobj = nearest_object(q.c, obj, nobj, haveSelf ? self : nullptr);
                    const bool isNear = dobj <= std::fmax(o.near_units, 0.75 * q.edge);
                    if (isNear) ++fs_.wnear;
                    if (!q.card) { if (q.edge < kMinEdge) ++fs_.tiny; else ++fs_.streak; continue; }
                    // the class a NEW card would get (independent of which classes are on: a smoke card never becomes a cloud because fxtrail is off)
                    const bool smokeLike = o.cls == kClsSize ? q.puff : (isNear && q.edge <= kNearMaxEdge);
                    int kind = -1;
                    if (smokeLike && o.smoke) kind = kSmoke;
                    else if (!smokeLike && o.cloud && q.edge >= kCloudMinEdge && q.edge <= kCloudMaxEdge) kind = kCloud;
                    const int r = store(q, q4, key, pso, heap, frame, now_ms, kind);
                    if (r == 1 && kind == kSmoke && isNear && stored_) note_head(*stored_, dobj);   // (M7) a smoke card born at an aircraft: the stream head
                    if (r < 0) { ++fs_.wfar; if (!smokeLike && !o.cloud && q.edge > o.size_cap()) ++fs_.big; }
                    else if (r != 2) ++fs_.observed;
                    continue;
                }
                ++fs_.clipped;
                Puff* m = match_clipped(q, key);
                // (M5) the game still draws this card, cut at its border: renew its life. M4 let a cloud the game kept drawing cut expire 120 s after its
                // last whole sighting (headset test 7: live c21 -> c0 at the end of a 2-minute stretch with whole=0 and 6 cut cloud draws per frame). The
                // position and template stay those of the last whole sighting (a cut quad's centre is that of its visible part, not of the card).
                if (m) { m->clip_frame = frame; m->miss = 0; if (m->seen_ms != now_ms) { m->seen_ms = now_ms; ++fs_.renew; } note_seen(*m); }
                if (m && m->emit_frame == frame && collapse) {   // the whole stored card is drawn this frame: it replaces the game's cut part
                    ++marked; ++fs_.collapsed;
                    for (int j = 0; j < 4; ++j) collapse[idx[k + size_t(j)] - lo] = 1;
                    continue;
                }
                // (M4c) no whole copy this frame: soften (fade) or hide the game's cut at its border
                if (o.clip == kClipOff) continue;
                const bool cloudCut = o.cloud && q.edge >= kCloudMinEdge;
                const bool smokeCut = o.smoke && (o.cls == kClsSize ? q.edge <= o.max_edge
                                                                    : nearest_object(q.c, obj, nobj, haveSelf ? self : nullptr) <= std::fmax(o.near_units, 0.75 * q.edge));
                if (!cloudCut && !smokeCut) continue;
                if (o.clip == kClipHide) {
                    if (!collapse) continue;
                    ++marked; ++fs_.ehide;
                    for (int j = 0; j < 4; ++j) collapse[idx[k + size_t(j)] - lo] = 1;
                } else if (fade) {
                    ++fs_.efade;
                    for (int j = 0; j < 4; ++j) if (on_border(q4[j]->pos[0], q4[j]->pos[1])) fade[idx[k + size_t(j)] - lo] = 1;
                }
            }
            s = e + 1;
        }
        return marked;
    }
    // (M4) whether the game shows this card itself: wholly inside its window, in front of its camera (R, U = the game basis).
    static bool shown_by_game(const Puff& x, const fxmem::ScreenMap& game, const double R[3], const double U[3]) noexcept {
        double w[4][3]; corners(x, R, U, w);
        for (int k = 0; k < 4; ++k) {
            double xs = 0, ys = 0, d = 0;
            if (!(fs::sp_project(game, w[k], xs, ys, d) && d > 1.0 && fxmem::in_window(xs, ys, kWholeMargin))) return false;
        }
        return true;
    }
    // (M4) wanted this frame: seen whole often enough, not drawn whole this frame, not shown whole by the game camera (gb = the game basis is valid).
    static bool wanted(const Puff& x, uint64_t frame, const fxmem::ScreenMap* game, bool gb, const double R[3], const double U[3]) noexcept {
        if (!x.used || x.seen_n < kMinSeen || x.full_frame == frame) return false;
        if (gb && game) return !shown_by_game(x, *game, R, U);
        return x.full_frame + 1 < frame;   // no game map: the M1 hand-over rule
    }
    // At a HOST draw (M4; see the header): writes up to `cap` indices into `out`, ordered by key (the host's first) then far to near. Returns the count.
    unsigned select(const Host& h, uint64_t frame, const fxmem::ScreenMap& game, const fxmem::ScreenMap* eye, uint16_t* out, unsigned cap,
                    const Options& o) noexcept {
        if (!o.on || !h.key || !out || !game.ok) return 0;
        ++fs_.hosts;
        double R[3] = {}, U[3] = {}, F[3] = {};
        const bool gb = basis(game, R, U, F);
        double eR[3] = {}, eU[3] = {}, eF[3] = {}, eE[3] = {};
        const bool eb = eye && eye->ok && basis(*eye, eR, eU, eF) && fs::game_eye(*eye, eE);
        Cand* const cand = cand_; unsigned nc = 0;   // member scratch (8 KiB): not on the hook thread's stack
        for (unsigned i = 0; i < kSlots; ++i) {
            Puff& x = p[i];
            if (x.emit_frame == frame || !wanted(x, frame, &game, gb, R, U)) continue;
            const bool own = x.key == h.key;
            const bool cross = !own && o.xkey && h.pso && x.pso == h.pso && x.heap == h.heap && key_alive(x.key, frame, h.heap);
            // (M6) neither: from its atlas tile, under a host of the same game PSO that can draw the atlas (any key, any heaps: our own SRV heap is bound)
            const bool fromAtlas = !own && !cross && o.atlas && h.atlas && h.pso && x.pso == h.pso && atlas_drawable(x);
            if (!own && !cross && !fromAtlas) { ++fs_.nomatch; continue; }
            double d = 0;
            if (eb) {   // (M7) the same card axes as build_eye (world up; the game's roll at the seam)
                double cR[3], cU[3]; card_axes(x.c, eE, eR, eU, eF, &game, gb ? U : nullptr, o.up, cR, cU);
                double w[4][3]; corners(x, cR, cU, w);
                double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300; bool front = true;
                for (int k = 0; k < 4 && front; ++k) {
                    double xs = 0, ys = 0, dk = 0;
                    front = fs::sp_project(*eye, w[k], xs, ys, dk) && dk > 1.0;
                    x0 = std::fmin(x0, xs); x1 = std::fmax(x1, xs); y0 = std::fmin(y0, ys); y1 = std::fmax(y1, ys);
                }
                const double gx0 = fxmem::kGsCentre - fxmem::kGsHalfW - kEyeMargin, gx1 = fxmem::kGsCentre + fxmem::kGsHalfW + kEyeMargin;
                const double gy0 = fxmem::kGsCentre - fxmem::kGsHalfH - kEyeMargin, gy1 = fxmem::kGsCentre + fxmem::kGsHalfH + kEyeMargin;
                if (!front || x1 < gx0 || x0 > gx1 || y1 < gy0 || y0 > gy1) { ++fs_.offeye; continue; }
                d = dist3(x.c, eE);
            }
            cand[nc++] = Cand{uint16_t(i), d, fromAtlas ? atlas::kGroupKey : x.key};
        }
        const unsigned lim = std::min(cap, kMaxEmit);
        if (nc > lim) {   // the nearest win (the far ones are faded out by the cache edge anyway)
            std::nth_element(cand, cand + lim, cand + nc, [](const Cand& a, const Cand& b) { return a.d < b.d; });
            nc = lim;
        }
        const uint64_t hk = h.key;
        std::sort(cand, cand + nc, [&](const Cand& a, const Cand& b) {
            const uint64_t ka = a.g, kb = b.g;   // (M6) the atlas group (kGroupKey) sorts last
            if (ka != kb) { if (ka == hk) return true; if (kb == hk) return false; return ka < kb; }
            return a.d > b.d;
        });
        for (unsigned k = 0; k < nc; ++k) {
            Puff& x = p[cand[k].i];
            x.emit_frame = frame; out[k] = cand[k].i;
            x.emit_atlas = cand[k].g == atlas::kGroupKey;
            x.emit_x = !x.emit_atlas && x.key != hk;
            if (x.emit_x) ++fs_.xkey;
            if (x.emit_atlas) { ++fs_.adrawn; atlas.use(x.tile, rec_slot_, frame); }
        }
        fs_.selected += nc;
        return nc;
    }
    // The GPU plan could not take the selection (window fallback / span full): the cards are not drawn by this host; a later host may select them again.
    void unselect(uint64_t frame, const uint16_t* sel, unsigned n, const char* why) noexcept {
        for (unsigned i = 0; i < n; ++i) if (sel[i] < kSlots && p[sel[i]].emit_frame == frame) {
            Puff& x = p[sel[i]];
            x.emit_frame = 0;
            if (x.emit_x && fs_.xkey) --fs_.xkey;   // (M4b) xkey counts cross-key selections a host kept, not ones handed back
            if (x.emit_atlas && fs_.adrawn) --fs_.adrawn;
            x.emit_x = false; x.emit_atlas = false;
        }
        if (fs_.selected >= n) fs_.selected -= n;
        if (why) refuse(why);
    }
    // The eye copy of the selected cards: TRIANGLELIST vertices into `out` (cap >= n x 6). Returns the vertex count. (M4) `groups` (kMaxKeys, may be
    // nullptr) receives one run per key in the selection's order; `ngroups` their count.
    unsigned build_eye(const fxmem::ScreenMap& eye, double depth_scale, double near_units, const uint16_t* sel, unsigned n, uint64_t now_ms, const Options& o,
                       Vtx* out, unsigned cap, Group* groups = nullptr, unsigned* ngroups = nullptr, const fxmem::ScreenMap* game = nullptr) noexcept {
        if (ngroups) *ngroups = 0;
        if (!o.on || !eye.ok || !out || !(depth_scale > 0)) return 0;
        double R[3], U[3], F[3], E[3];
        if (!basis(eye, R, U, F)) { refuse("trail-eye-basis"); return 0; }
        const bool haveE = fs::game_eye(eye, E);
        // (M7) the game camera's up for the seam blend (kUpSeam; `game` = the frame's game map, nullptr: pure world up)
        double gR[3] = {}, gU[3] = {}, gF[3] = {};
        const bool gb = game && game->ok && basis(*game, gR, gU, gF);
        unsigned m = 0, ng = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (sel[i] >= kSlots || !p[sel[i]].used) continue;
            const Puff& x = p[sel[i]];
            if (m + kVertsPerPuff > cap) { refuse("trail-eye-cap"); break; }
            const double f = fade(x, now_ms, haveE ? dist3(x.c, E) : x.depth, o);
            if (!(f > 0.0)) continue;   // faded out (cache edge / end of life): nothing to draw
            atlas::Rect ar{};
            const bool fromAtlas = x.emit_atlas && x.tile >= 0 && atlas::rect_of(x.tmpl, ar);   // (M6) the card's uv mapped into its tile
            if (x.emit_atlas && !fromAtlas) continue;
            const uint64_t gk = fromAtlas ? atlas::kGroupKey : x.key;
            // (M7) world up facing the eye point (no aircraft roll, no HMD roll); the game's roll only near its window (card_axes). No eye point: M4 basis.
            double cR[3], cU[3];
            if (haveE) card_axes(x.c, E, R, U, F, game, gb ? gU : nullptr, o.up, cR, cU);
            else for (int i = 0; i < 3; ++i) { cR[i] = R[i]; cU[i] = U[i]; }
            double w[4][3]; corners(x, cR, cU, w);
            Vtx c4[4]; bool ok = true;
            for (int k = 0; k < 4 && ok; ++k) {
                double xs = 0, ys = 0, d2 = 0;
                if (!fs::sp_project(eye, w[k], xs, ys, d2) || !(d2 > near_units)) { ok = false; break; }
                Vtx t = x.tmpl[k];
                t.pos[0] = float(xs); t.pos[1] = float(ys);
                if (x.zform) {
                    t.pos[2] = float(depth_scale * fxmem::game_z_for_depth(eye, d2));
                    if (std::isfinite(x.tmpl[k].pos[3]) && x.tmpl[k].pos[3] > 0.0f) t.pos[3] = float(double(x.tmpl[k].pos[3]) * x.depth / d2);
                } else { t.pos[2] = 0.0f; t.pos[3] = float(1.0 / d2); }
                t.col[3] = float(double(t.col[3]) * f);
                if (fromAtlas) atlas::map_uv(unsigned(x.tile), ar, x.tmpl[k].uv[0], x.tmpl[k].uv[1], t.uv[0], t.uv[1]);
                for (int c = 0; c < 4; ++c) if (!std::isfinite(t.pos[c])) ok = false;
                c4[k] = t;
            }
            if (!ok) { ++fs_.behind; continue; }
            if (groups) {
                if (!ng || groups[ng - 1].key != gk) {
                    if (ng >= kMaxKeys) { refuse("trail-group-cap"); break; }
                    groups[ng++] = Group{gk, m, 0};
                }
                groups[ng - 1].count += kVertsPerPuff;
            }
            static constexpr int order[kVertsPerPuff] = {0, 1, 2, 2, 1, 3};
            for (int j = 0; j < int(kVertsPerPuff); ++j) out[m++] = c4[order[j]];
            ++fs_.emitted; if (x.kind == kCloud) ++fs_.cemitted;
        }
        if (ngroups) *ngroups = ng;
        return m;
    }
    // End of a frame: cull the cards the game stopped drawing although they lie wholly in its window, expire the old ones, drop the clouds beyond the
    // cache, count a frame that wanted a draw but had no host, add the frame to the run. Returns the finished frame's statistics.
    Stats end_frame(const fxmem::ScreenMap* game, uint64_t frame, uint64_t now_ms, const Options& o) noexcept {
        double R[3] = {}, U[3] = {}, F[3] = {}, E[3] = {};
        const bool gb = game && game->ok && basis(*game, R, U, F);
        const bool ge = gb && fs::game_eye(*game, E);
        bool want = false;
        for (auto& x : p) {
            if (!x.used) continue;
            if (now_ms - x.seen_ms > uint64_t(o.life_of(x.kind))) { drop(x); ++fs_.expired; if (x.kind == kCloud) ++fs_.cexpired; continue; }
            if (ge && x.kind == kCloud && dist3(x.c, E) > kCloudDropFar * o.cloud_far) { drop(x); ++fs_.gone; continue; }
            if (wanted(x, frame, game, gb, R, U)) {
                want = true; ++fs_.want;
                // (M5) not drawn this frame and no game draw bound its key; (M6b) whatever its tile: `unb` counts every such miss (M6 moved the ones with a
                // ready tile to amiss only, so a run whose atlas never drew showed unb 0, step164); amiss = the part of unb that had a ready tile
                if (x.emit_frame != frame && !key_bound(x.key, frame)) {
                    ++fs_.unbound;
                    if (o.atlas && atlas_drawable(x)) ++fs_.amiss;
                }
            }
            if (!gb || x.full_frame == frame || x.clip_frame == frame) { if (x.full_frame == frame) x.miss = 0; continue; }
            if (!shown_by_game(x, *game, R, U)) { x.miss = 0; continue; }
            if (++x.miss >= kCullFrames) { drop(x); ++fs_.culled; }
        }
        update_streams(game, gb, R, U, frame, now_ms, o);   // (M7) after the cull: a synthetic card is first judged next frame
        if (want && fs_.hosts == 0) ++fs_.nohost;
        run.add(fs_);
        const Stats done = fs_;
        fs_ = Stats{};
        return done;
    }

private:
    // ---- (M7) plume streams (see kStreams). head_: the best candidate of the frame (a smoke card stored NEW nearest to a world object).
    struct Stream {
        bool used = false;
        double pos[3] = {}, vel[3] = {};   // pos = the last REAL birth position; vel per frame (smoothed)
        uint64_t birth_frame = 0;          // the frame of the last real birth
        unsigned births = 0;
        Puff tmpl;                         // the newest real birth (copied: its slot may be reused)
        int tile = -1;                     // (M8) a reference on the atlas tile of tmpl's key and uv rectangle, held for the stream's life (see stream_tile)
    };
    Stream streams_[kStreams];
    Puff head_{}; double head_d_ = 0; bool head_ok_ = false;
    Puff* stored_ = nullptr;               // the slot the last store() wrote
    void note_head(const Puff& x, double dobj) noexcept {
        if (head_ok_ && !(dobj < head_d_)) return;
        head_ = x; head_d_ = dobj; head_ok_ = true;
    }
    static double vlen(const double v[3]) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    // (M8) keeps the stream's reference on the atlas tile of its template (same key and uv rectangle): re-validated every call (the tile may have been
    // evicted or recycled), re-taken from any live real card of that key and rectangle that has a tile. No donor now: the stream keeps none (retried).
    void stream_tile(Stream& s) noexcept {
        atlas::Rect want{};
        if (!atlas::rect_of(s.tmpl.tmpl, want)) { atlas.release(s.tile); return; }
        if (atlas.valid(s.tile) && atlas.t[s.tile].refs && !atlas.t[s.tile].quarantined && atlas.t[s.tile].key == s.tmpl.key && atlas.t[s.tile].r == want) return;
        atlas.release(s.tile);
        for (auto& x : p) if (x.used && !x.synth && x.kind == kSmoke && x.tile >= 0 && x.key == s.tmpl.key) {
            atlas::Rect r; if (atlas::rect_of(x.tmpl, r) && r == want) { s.tile = atlas.join(x.tile); if (s.tile >= 0) return; }
        }
    }
    void stream_reset(Stream& s) noexcept { atlas.release(s.tile); s = Stream{}; }
    // A synthetic smoke card at world point `pos` from stream `s`'s template (the game's own art: colour, uv, key, host, size). One card per stream per frame.
    void spawn_synth(Stream& s, const double pos[3], uint64_t now_ms) noexcept {
        Puff* slot = nullptr;
        if (live_of(kSmoke) >= kCap) {
            // (M8, headset test 9) the oldest SYNTHETIC card goes first: M7 evicted the oldest card of any kind, so the real cards (the only atlas tile donors) were
            // gone after ~2 s of a plume and every later synthetic card had no tile (nomatch / unb ~ want in the test 9 log)
            for (auto& x : p) if (x.used && x.synth && x.kind == kSmoke && (!slot || x.seen_ms < slot->seen_ms)) slot = &x;
            if (!slot) for (auto& x : p) if (x.used && x.kind == kSmoke && (!slot || x.seen_ms < slot->seen_ms)) slot = &x;
            ++fs_.evicted;
        } else for (auto& x : p) if (!x.used) { slot = &x; break; }
        if (!slot) return;
        { int t = slot->tile; atlas.release(t); }
        // the tile of the stream's own reference (held since a real card of its key had one), else of a live real card of the same key and uv rectangle
        int tile = -1;
        stream_tile(s);
        if (s.tile >= 0) tile = atlas.join(s.tile);
        *slot = s.tmpl;
        slot->used = true; slot->kind = kSmoke; slot->synth = true; slot->tile = int16_t(tile);
        for (int c = 0; c < 3; ++c) { slot->c[c] = pos[c]; slot->c0[c] = pos[c]; }
        slot->full_frame = 0; slot->clip_frame = 0; slot->emit_frame = 0; slot->refresh_frame = 0;
        slot->seen_ms = now_ms; slot->born_ms = now_ms; slot->seen_n = kMinSeen; slot->miss = 0;
        slot->emit_x = false; slot->emit_atlas = false;
        ++fs_.synth;
    }
    // End of frame: the frame's head candidate feeds / starts a stream; every other stream is extrapolated and, while its predicted card is not wholly in the
    // game window, spawns one synthetic card. R / U / gb = the game basis (gb false: no game camera this frame, the streams coast without spawning).
    void update_streams(const fxmem::ScreenMap* game, bool gb, const double R[3], const double U[3], uint64_t frame, uint64_t now_ms, const Options& o) noexcept {
        if (head_ok_ && o.smoke) {
            Stream* best = nullptr; double bd = 0;
            for (auto& s : streams_) {
                if (!s.used || s.birth_frame >= frame) continue;
                const double dt = double(frame - s.birth_frame);
                double pr[3]; for (int c = 0; c < 3; ++c) pr[c] = s.pos[c] + s.vel[c] * dt;
                const double d = dist3(pr, head_.c);
                if (d <= kStreamGatherMin + 2.0 * vlen(s.vel) * dt && (!best || d < bd)) { best = &s; bd = d; }
            }
            if (best) {
                Stream& s = *best;
                const double dt = double(frame - s.birth_frame);
                double inst[3]; for (int c = 0; c < 3; ++c) inst[c] = (head_.c[c] - s.pos[c]) / dt;
                if (vlen(inst) <= kStreamMaxSpeed) {
                    for (int c = 0; c < 3; ++c) s.vel[c] = s.births >= 2 ? 0.5 * s.vel[c] + 0.5 * inst[c] : inst[c];
                    for (int c = 0; c < 3; ++c) s.pos[c] = head_.c[c];
                    s.birth_frame = frame; if (s.births < 255) ++s.births; s.tmpl = head_;
                    stream_tile(s);
                }
            } else {
                Stream* slot = nullptr;
                for (auto& s : streams_) if (!s.used) { slot = &s; break; }
                if (!slot) for (auto& s : streams_) if (!slot || s.birth_frame < slot->birth_frame) slot = &s;
                stream_reset(*slot);
                slot->used = true; slot->births = 1; slot->birth_frame = frame; slot->tmpl = head_;
                stream_tile(*slot);
                for (int c = 0; c < 3; ++c) slot->pos[c] = head_.c[c];
            }
        }
        head_ok_ = false;
        unsigned n = 0;
        for (auto& s : streams_) {
            if (!s.used) continue;
            const uint64_t dt = frame - s.birth_frame;
            if (!o.smoke || dt > kStreamMaxFrames) { stream_reset(s); continue; }
            ++n;
            if (dt == 0 || !gb || !game) continue;   // a real birth this frame: the game draws it; no camera: coast
            double pr[3]; for (int c = 0; c < 3; ++c) pr[c] = s.pos[c] + s.vel[c] * double(dt);
            Puff t = s.tmpl; for (int c = 0; c < 3; ++c) t.c[c] = pr[c];
            if (shown_by_game(t, *game, R, U)) {   // the game would draw a card here and does not: the emitter stopped (or the aircraft is gone)
                if (dt >= kStreamQuiet) { stream_reset(s); --n; }
                continue;
            }
            if (s.births >= kStreamMinBirths) spawn_synth(s, pr, now_ms);
        }
        fs_.streams = n;
    }
    unsigned rec_slot_ = atlas::kSlotCap;   // (M6) set_slot
    // (M6) a card leaves the memory: its tile reference goes with it
    void drop(Puff& x) noexcept { int t = x.tile; atlas.release(t); x.tile = -1; x.used = false; }
    void note_seen(const Puff& x) noexcept {
        const uint16_t i = uint16_t(&x - p);
        for (unsigned k = 0; k < seen_n_; ++k) if (seen_[k] == i) return;
        if (seen_n_ < kSeenCap) seen_[seen_n_++] = i;
    }
    double cam_[3] = {}, cam_step_ = 0;   // (M5) the game camera of frame cam_frame_ (cam_ok_) and its step from the previous frame
    uint64_t cam_frame_ = 0;
    bool cam_ok_ = false;
    void note_cam(uint64_t frame, const double* self) noexcept {
        if (cam_frame_ == frame) return;   // the frame's first class-1 draw decides
        double step = 0;
        if (self && cam_ok_ && cam_frame_ + 1 == frame) { step = dist3(self, cam_); if (!(step <= kMaxCamStep)) step = 0; }
        cam_step_ = step; cam_frame_ = frame; cam_ok_ = self != nullptr;
        if (self) for (int c = 0; c < 3; ++c) cam_[c] = self[c];
    }
    // (M5) the refresh radius of stored card x for a whole card of edge `qe` in `frame`: 0.25 x the larger edge (M4); a stored SMOKE card also within one
    // edge and within kCamSlack x the frame's camera step. The nearest card not yet refreshed this frame still wins, so neighbouring puffs keep their cards.
    double match_radius(const Puff& x, double qe, uint64_t frame) const noexcept {
        const double e = std::fmax(x.edge, qe);
        double r = kMatchFrac * e;
        if (x.kind == kSmoke) r = std::fmax(r, std::fmax(kSmokeMatchFrac * e, kCamSlack * cam_step(frame)));
        return r;
    }
    void note_key(uint64_t key, uint64_t frame, uint64_t heap) noexcept {
        KeySeen* slot = nullptr;
        for (auto& k : keys) { if (k.key == key) { k.frame = frame; k.heap = heap; return; } if (!slot || k.frame < slot->frame) slot = &k; }
        *slot = KeySeen{key, frame, heap};   // a new key replaces the one bound longest ago
    }
    // (M5) kchurn: a whole card that matched no stored card of its key but lies on a stored card of ANOTHER key (diagnostic only; nothing changes).
    void churn(const Quad& q, uint64_t key, uint64_t frame) noexcept {
        for (const auto& x : p) if (x.used && x.key != key && x.refresh_frame != frame && dist3(q.c, x.c) <= match_radius(x, q.edge, frame)) { ++fs_.kchurn; return; }
    }
    Puff* match_clipped(const Quad& q, uint64_t key) noexcept {
        Puff* best = nullptr; double bd = 0;
        for (auto& x : p) {
            if (!x.used || x.key != key) continue;
            const double r = std::hypot(x.hu, x.hv);
            const double d = dist3(q.c, x.c);
            if (!(d <= r)) continue;
            bool corners_in = true;
            for (int k = 0; k < 4 && corners_in; ++k) corners_in = dist3(q.w[k], x.c) <= 1.5 * r;
            if (!corners_in) continue;
            if (!best || d < bd) { best = &x; bd = d; }
        }
        return best;
    }
    // (M4) -1 = no stored card matched and `kind` < 0 (no new card allowed); 0 = refreshed; 1 = stored new; 2 = duplicate of a card refreshed this frame.
    int store(const Quad& q, const Vtx* const q4[4], uint64_t key, uint64_t pso, uint64_t heap, uint64_t frame, uint64_t now_ms, int kind) noexcept {
        Puff* slot = nullptr; double bd = 0; Puff* nearest = nullptr; double nd = 0;
        for (auto& x : p) {
            if (!x.used || x.key != key) continue;
            const double d = dist3(q.c, x.c);
            if (!nearest || d < nd) { nearest = &x; nd = d; }
            if (x.refresh_frame == frame) continue;   // one card refreshes one puff per frame
            if (d <= match_radius(x, q.edge, frame) && (!slot || d < bd)) { slot = &x; bd = d; }
        }
        if (nearest && nearest->refresh_frame == frame && nd <= kDupFrac * std::fmax(nearest->edge, q.edge) && (!slot || nd < bd)) { ++fs_.dup; return 2; }
        int r = 0;
        if (slot) {
            ++fs_.refreshed; ++slot->seen_n;
            if (slot->kind == kCloud) { ++fs_.crefreshed; const double dr = dist3(q.c, slot->c0); if (dr > fs_.cdrift) fs_.cdrift = dr; }
            else { const double e = std::fmax(slot->edge, q.edge); if (bd > kSmokeMatchFrac * e) ++fs_.scam; else if (bd > kMatchFrac * e) ++fs_.swide; }
        }
        else {
            churn(q, key, frame);
            if (kind < 0) return -1;
            const unsigned cap = kind == kCloud ? kCloudCap : kCap;
            if (live_of(kind) >= cap) {   // evict this kind's card seen longest ago
                for (auto& x : p) if (x.used && x.kind == kind && (!slot || x.seen_ms < slot->seen_ms)) slot = &x;
                ++fs_.evicted;
            } else for (auto& x : p) if (!x.used) { slot = &x; break; }
            if (!slot) return -1;   // unreachable: the kinds' caps sum to kSlots
            ++fs_.stored; if (kind == kCloud) ++fs_.cstored;
            { int t = slot->tile; atlas.release(t); }   // (M6) an evicted card's tile reference goes with it
            *slot = Puff{};
            slot->kind = uint8_t(kind); slot->born_ms = now_ms; slot->seen_n = 1;
            for (int c = 0; c < 3; ++c) slot->c0[c] = q.c[c];
            r = 1;
        }
        Puff& x = *slot;
        x.used = true; x.key = key; x.pso = pso; x.heap = heap;
        for (int c = 0; c < 3; ++c) x.c[c] = q.c[c];
        x.hu = q.hu; x.hv = q.hv; x.depth = q.depth; x.zform = q.zform; x.edge = q.edge;
        for (int k = 0; k < 4; ++k) { x.tmpl[k] = *q4[k]; x.sx[k] = q.sx[k]; x.sy[k] = q.sy[k]; }
        x.full_frame = frame; x.refresh_frame = frame; x.seen_ms = now_ms; x.miss = 0;
        note_seen(x);   // (M6) a capture candidate of this draw
        stored_ = &x;
        return r;
    }
};

// (M2) The worker writes one `fx-puff-trail` line per second while the option is active (life > 0), whatever happened: a run always proves the path.
inline bool line_due(bool active, uint64_t now_ms, uint64_t last_ms) noexcept { return active && (last_ms == 0 || now_ms - last_ms >= 1000); }
// `fx-puff-trail` line: `w` = the frames accounted since the previous line (`frames` of them; 0 = no armed fxmem:6 frame reached the memory: `open=gG,rR`
// says why: G = the per-eye sprite replay switch, R = its resources), `run` the totals. Short names: nc = nonconst, emin / emax = the smallest / largest
// whole card edge, obs = observed (stored or refreshed), coll = collapsed, sel = selected, exp = expired, evict = evicted. Counters are capped at 99999.
// live = every stored card (smoke + cloud; the fx-puff-mem line splits them). < 448 bytes with the 21-byte prefix for any values (offline-tested).
inline void format_line(char* b, size_t n, uint64_t frame, unsigned frames, const Stats& w, const Stats& run, unsigned live, bool sprite_gpu, bool ready,
                        int cls = kClsNear) noexcept {
    if (!b || !n) return;
    auto c = [](uint32_t x) { return x < 99999u ? x : 99999u; };
    auto e = [](double x) { return x < 99999.0 ? x : 99999.0; };
    std::snprintf(b, n, "fx-puff-trail;frame=%llu;frames=%u;open=g%u,r%u;cls=%s;live=%u;draws=%u;nocam=%u;nokey=%u;objs=%u;quads=%u;nc=%u;whole=%u;near=%u;"
        "far=%u;big=%u;streak=%u;emin=%.0f;emax=%.0f;obs=%u;stored=%u;clipped=%u;coll=%u;sel=%u;emitted=%u;culled=%u;exp=%u;evict=%u;refused=%u;"
        "run=stored:%u,emitted:%u;last-why=%.24s",
        static_cast<unsigned long long>(frame % 100000000ull), c(frames), sprite_gpu ? 1u : 0u, ready ? 1u : 0u, cls_name(cls), live, c(w.draws), c(w.nocam),
        c(w.nokey), c(w.objs), c(w.quads), c(w.nonconst), c(w.whole), c(w.wnear), c(w.wfar), c(w.big), c(w.streak), e(w.edge_min), e(w.edge_max), c(w.observed),
        c(w.stored), c(w.clipped), c(w.collapsed), c(w.selected), c(w.emitted), c(w.culled), c(w.expired), c(w.evicted), c(w.refused), c(run.stored),
        c(run.emitted), run.last_why ? run.last_why : "none");
}
// (M4) the companion `fx-puff-mem` line (same tick, right after fx-puff-trail): the two kinds and the M4 rules. live=sS,cC: smoke / cloud cards stored;
// cst / cref: clouds stored new / refreshed; dup: duplicate cards; hosts: host draws that selected; nohost: frames that wanted a draw and had no host;
// xkey: cards selected inside another key's host draw; nomatch: wanted cards no host could take; offeye: wanted cards outside the eye; gone: clouds dropped
// beyond the cache; cexp: clouds expired; cemit: cloud cards emitted (per eye); (M4b) cdrift: the largest drift of a refreshed cloud from its birth centre
// (world units; world-static clouds: small). nomatch / offeye are counted per HOST call (a card a frame's 4 hosts cannot take counts up to 4 times).
// (M5) per CARD-FRAME instead: want = wanted card-frames, unb = of them undrawable because no game draw bound their key in the frame (unb / want = the share
// of the cache the F1 key rule hides); renew = life renewals by cut sightings; kch = key churn; syn / strm = (M7) synthetic plume cards spawned / emitters followed. run=: totals. < 448 bytes with the prefix for any values (offline-tested).
inline void format_mem_line(char* b, size_t n, uint64_t frame, const Stats& w, const Stats& run, unsigned smoke_live, unsigned cloud_live, const Options& o) noexcept {
    if (!b || !n) return;
    auto c = [](uint32_t x) { return x < 99999u ? x : 99999u; };
    std::snprintf(b, n, "fx-puff-mem;frame=%llu;smoke=%u;cloud=%u;xk=%u;live=s%u,c%u;cst=%u;cref=%u;dup=%u;hosts=%u;nohost=%u;xkey=%u;nomatch=%u;offeye=%u;"
        "gone=%u;cexp=%u;cemit=%u;cdrift=%.0f;clip=%s;efade=%u;ehide=%u;want=%u;unb=%u;renew=%u;kch=%u;syn=%u;strm=%u;"
        "run=cst:%u,cemit:%u,xkey:%u,nohost:%u,dup:%u,cdrift:%.0f,efade:%u,ehide:%u,unb:%u",
        static_cast<unsigned long long>(frame % 100000000ull), o.smoke ? unsigned(o.life_ms / 1000.0 + 0.5) : 0u, o.cloud ? unsigned(o.cloud_life_ms / 1000.0 + 0.5) : 0u,
        o.xkey ? 1u : 0u, smoke_live, cloud_live, c(w.cstored), c(w.crefreshed), c(w.dup), c(w.hosts), c(w.nohost), c(w.xkey), c(w.nomatch), c(w.offeye),
        c(w.gone), c(w.cexpired), c(w.cemitted), w.cdrift < 999999.0 ? w.cdrift : 999999.0, clip_name(o.clip), c(w.efade), c(w.ehide),
        c(w.want), c(w.unbound), c(w.renew), c(w.kchurn), c(w.synth), c(w.streams), c(run.cstored),
        c(run.cemitted), c(run.xkey), c(run.nohost), c(run.dup), run.cdrift < 999999.0 ? run.cdrift : 999999.0, c(run.efade), c(run.ehide), c(run.unbound));
}

} // namespace acz::fxpuff
