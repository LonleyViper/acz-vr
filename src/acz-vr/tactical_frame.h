#pragma once
// tactical_frame.h -- TACTICAL (xr_terrain_stage option tac:1, default OFF; docs/world-render/tactical.md): the per-frame facts the step-5 producer
// collects for the consumer's tactical display, carried additively in StereoFrame::tac. Header only, SDK-free, fixed size (no allocation on the
// game's render thread). With tac off the producer never fills it (valid = false, every count 0) and the consumer never reads it.
//
// What is EVIDENCE and what is INFERENCE (see tactical.md for the citations):
//  * obj[]: one entry per REPLAYED DrawACM_v world draw of the frame (the same draws the world-object replay draws into the eyes). t = the record's
//    model->world translation (record +256 B, floats 76..78: the same read as world_dedupe.h, which matched .pdl building placements to 0.49 u [E]);
//    axes = the 3x3 of B (rows; may include scale) [E: layout]; sig = a mesh signature of the draw (index/vertex count, stride, indexed) [E: values;
//    that one model part keeps its signature across frames is INFERENCE].
//  * A = the frame's world -> PS2-screen matrix (record +0, row vectors; every record of the frame carries the same A, gpu_copy_step5_world.h) [E].
//  * mark[]: the game's own target-marker boxes of the frame's HUD tail (gpu_copy_step5_hud_markers classification: depth GREATER box draws), one
//    entry per spatial cluster of the box draw's vertices (ps2_v ATTRIBUTE0 = pre-projected GS screen x,y, ATTRIBUTE1 = vertex colour;
//    stored-draw-manifest-1.md section 3.1) [E: vertex format of ps2_v; INFERENCE: one cluster = one marker, the box is centred on its object].
//  * wedge: draws of the firing-only HUD wedge key (VS 534f29ca / PS d34ee153, non-indexed 4 vertices, additive blend src5/dst2/op1, depth GREATER
//    no write, drawn BETWEEN cockpit members; D-318 step69: present exactly while the gun is held, ALSO while a missile is fired) [E step69].
#include <atomic>
#include <cstdint>

namespace acz::tac {

constexpr unsigned kObjCap = 96;     // = gpu_copy::step5::world::draw_cap (world draws recorded per frame)
constexpr unsigned kMarkCap = 16;    // marker clusters kept per frame (the game showed <= ~12 contacts on the radar, step36)

struct ObjDraw {
    float t[3] = {};        // model -> world translation (world units, the terrain/E6 space)
    float axes[9] = {};     // B rows 0..2 (xyz), as stored
    uint32_t sig = 0;       // mesh signature (mesh_signature())
    uint8_t marker = 0;     // 1 + index into TacFrame::mark of the marker matched to this draw (0 = none)
    // OBJECT-INTERP (interp:1): the draw's world position at the frame's display time (object_interp.h). The producer matches the game's marker
    // sprites with t (frame time, the sprites' own time) and replaces t by td when the frame closes, so the consumer's tracker sees display-time positions.
    float td[3] = {};
    uint8_t td_valid = 0;
    // BOX-LOCK (markers-360.md section 15): the FRAME-time translation kept when t is replaced by td (tf_valid = 1). The game drew its marker sprites
    // with the frame's A at this position; the consumer must match / project sprites against tf, and anchor what it draws on t (display time).
    float tf[3] = {};
    uint8_t tf_valid = 0;
};
struct MarkerSprite {
    float cx = 0, cy = 0;   // box centre, PS2 GS screen units (2048 +- 256 / 2048 +- 224 = the game window)
    float hw = 0, hh = 0;   // half extents (GS units)
    float rgb[3] = {};      // mean vertex colour, normalised so the largest channel is 1 (raw scale UNKNOWN)
    uint32_t verts = 0;     // box vertices in the cluster
    uint8_t glyphs = 0;     // glyph quads of the label cluster associated with this box (0 = no label seen)
    uint8_t obj = 0;        // 1 + index into TacFrame::obj of the matched draw (0 = unmatched)
    bool suppressed = false;// the game's box draw holding this marker was not replayed (every marker of it matched a tracked draw)
    // NBOX (mk360:3, markers-360.md section 12): the union rectangle (GS) of the label clusters associated with this box (valid when glyphs > 0) and the
    // label's OWN colour (normalised): the game's label pixels are cut out with the box; the synthetic fallback (mk360:1) writes FRND only for a BLUE label.
    float lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
    float lrgb[3] = {};
    // LOCK-SHAPE (markers-360.md section 17): 0 = a cluster of a depth-GREATER box draw; 1 = the lock DIAMOND, an instrument draw the producer moved into
    // the marker image (tactical_producer.h lock_shape_test). A diamond is never matched to a contact itself: it joins its box's stamp as a satellite.
    uint8_t shape = 0;
};
// Raw ps2_v attribute-0 w (= the GS Q) of the vertices read this frame (box / label draws). VS 534f29ca divides the WHOLE position by q, so the screen
// position is x, y as stored whatever q is (markers-360.md section 17.1); kept for the mk360-stamp diagnostic only.
struct QStat {
    float lo = 0, hi = 0; uint32_t n = 0, off = 0;   // min / max over n vertices; off = vertices with |q - 1| > 1e-4
    void add(float q) noexcept {
        if (!(q == q)) return;
        if (!n) lo = hi = q; else { if (q < lo) lo = q; if (q > hi) hi = q; }
        ++n; if (q > 1.0001f || q < 0.9999f) ++off;
    }
};
struct TacFrame {
    bool valid = false;               // the producer had tac on for this frame
    bool a_valid = false;             // A recovered (at least one world draw replayed)
    float A[16] = {};
    uint32_t nobj = 0;
    ObjDraw obj[kObjCap];
    uint32_t obj_dropped = 0;         // world draws beyond kObjCap (cannot happen while draw_cap == kObjCap; counted anyway)
    uint32_t nmark = 0;
    MarkerSprite mark[kMarkCap];
    uint32_t mark_dropped = 0;        // clusters beyond kMarkCap
    uint32_t box_draws = 0, label_draws = 0;           // HUD-tail marker draws seen (box / label)
    uint32_t box_unreadable = 0, label_unreadable = 0; // vertex data not readable (not a pinned mapped UPLOAD buffer, too many vertices, ...)
    uint32_t boxes_suppressed = 0, labels_suppressed = 0;
    uint32_t wedge = 0;               // firing-wedge draws proven BETWEEN cockpit members this frame
    uint32_t wedge_tail = 0;          // firing-wedge-shaped draws after the last member (not counted as firing)
    const char* why = nullptr;        // static: why marker learning is unavailable this frame (nullptr = available)
    uint32_t lock_shapes = 0;         // LOCK-SHAPE: instrument draws moved into the marker image this frame (lock diamonds)
    QStat box_q, label_q;             // raw q of the box / label vertices read this frame (diagnostic)
};

// The producer's switch: set by the consumer when it parses tac:1 (process-wide; one DLL). 0 = off (the producer does nothing at all).
// bit 0 = collect, bit 1 = suppress matched game marker sprites, bit 2 = MARKERS-360 mk360:1: suppress every completely READ box draw (and its label)
// whatever matched (the consumer redraws each of its sprites at its own ray; marker360.h).
// bit 3 = LOCK-SHAPE (mk360:3, mk360lock not 0): an instrument draw after a box draw whose vertices form a 45-degree square in a box's colour within
// satellite reach of that box is replayed into the MARKER image (markers-360.md section 17.4); bit 4 = mk360cen:1, the one-shot HUD census after box draws; bit 5 = mk360ld:1, the bounded per-draw dump of rejected lock-diamond candidates (markers-360.md 21.6).
constexpr uint32_t kProducerOn = 1u, kProducerSuppress = 2u, kProducerHideAll = 4u, kProducerLockShape = 8u, kProducerLockCensus = 16u, kProducerLockDump = 32u;
inline std::atomic<uint32_t>& producer_flags_slot() noexcept { static std::atomic<uint32_t> s{0}; return s; }
inline uint32_t producer_flags() noexcept { return producer_flags_slot().load(std::memory_order_acquire); }
inline void set_producer_flags(uint32_t v) noexcept { producer_flags_slot().store(v, std::memory_order_release); }

} // namespace acz::tac
