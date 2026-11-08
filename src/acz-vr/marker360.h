#pragma once
// marker360.h -- MARKERS-360 (xr_terrain_stage sub-option mk360:1|2, default OFF; docs/world-render/markers-360.md). Header only, SDK-free, pure.
//
// Headset test 19 (D-362): the game draws its FRND/TGT boxes only for its own forward view (a flat quad 500 m out, 63x38 deg); the user wants them
// around the whole sphere. The earlier synthetic tacboxes:1 boxes were rejected (D-355): doubled the game's, outlived kills by 15 s, wrong colours.
// This module keeps exactly ONE box per contact the GAME currently reports, in the game's own colour/label, world-anchored:
//
//  * IN the game window: every game box sprite is redrawn by us at its own direction (the ray of its GS screen centre through the frame's world->screen
//    matrix A), with its own colour, label glyph count and size (mk360:1, the game's sprite draw is suppressed by the producer, flag kProducerHideAll,
//    only when it was READ completely; an unread draw stays the game's and we draw none of it). mk360:2 keeps the game's in-window boxes instead.
//  * OUTSIDE the game window: a contact keeps the identity (colour, label, size) of the game box last matched to it while it was in the window, and is
//    drawn at its TRUE 3D position = the centroid of its replayed world draws (record +256 translation) of the newest frame. Evidence that the game
//    submits those draws outside its forward view: test-18 tac-blips, unknown-team tracks are listed only while seen in the newest frame, 58 of 91
//    such samples at |bearing| > 35 deg and 31 behind (> 90 deg), 4 distinct contacts, up to 67 km of units away (pre-run-P xr-stereo.log).
//  * GONE when the game stops reporting it: (a) its world draws vanish for > lost_s (0.25 s; a killed aircraft's mesh), or (b) it projects inside the
//    game window (inner margin) and the game drew no box on it for >= miss_frames frames and >= miss_s (a kill / a lock or target change in view).
//    No coasting, no dead reckoning.
//  * Contacts NEVER seen with a game box have no identity: not drawn (mk360u:1 draws them as dim grey unlabeled boxes when moving, for tests only).
//    Radar-derived identity for them is NOT implemented (the game's radar contacts are replayed by gpu_copy_step5_hud_radar but not read).
#include "tactical.h"
#include "tactical_producer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace acz::mk360 {

// The game window in GS screen units (gpu_copy_step5_world.h: ndc.x = (x_s-2048)*2/512, ndc.y = (y_s-2048)*2/-448) and its lens (the marker layer's
// 63.20 x 38.17 deg = tan 0.6152 / 0.3459, D-358 'marker-angular-deg'): GS units -> tangent of the view angle.
constexpr float kWinCx = 2048.0f, kWinCy = 2048.0f, kWinHx = 256.0f, kWinHy = 224.0f;
constexpr float kLensTanX = 0.6152f, kLensTanY = 0.3459f;
constexpr float kTanPerGsX = kLensTanX / kWinHx, kTanPerGsY = kLensTanY / kWinHy;
// The game's window (512 x 448 GS, 8:7) is stretched over the lens (tan 0.6152 x 0.3459, 16:9): kTanPerGsX / kTanPerGsY = 1.56 (anamorphic). The game's
// boxes are square-ish on ITS screen, so scaling a box's x and y half sizes with the two different factors drew them 1.56x too wide (flat rectangles).
// A box is drawn with ONE angular scale (the geometric mean) and the game's own aspect (clamped to 0.8..1.25: a mis-matched piece pair never gives a
// wide flat rectangle). markers-360.md section 10.
constexpr float kTanPerGsBox = 0.001926f;   // sqrt(kTanPerGsX * kTanPerGsY) = sqrt(0.0024031 * 0.0015442)
// NBOX (mk360:3, markers-360.md section 12): the atlas of game stamps (box + label pixels cut from the replayed marker image). A cell holds the box
// centre +- kStampHalfGs (GS): the box (<= 14 GS half, MATCH-2 groups) plus its label (kLabelReach 30 GS from the box edge, ~6 GS per glyph). The
// original-game reference (headset test 2026-10-28 item 13: red lock reticle with its range number "4717" below-right, "AA GUN" right of the crosshair)
// puts text up to ~60 GS right of the box centre: the cell is wider than tall.
constexpr unsigned kStampCols = 6, kStampRows = 4, kStampCells = kStampCols * kStampRows;
constexpr float kStampHalfGsX = 64.0f, kStampHalfGsY = 32.0f, kStampMarginGs = 2.0f;
constexpr uint64_t kStampAtlasBudget = 64ull << 20;   // bytes (R16G16B16A16 at 2560x1440: 3840 x 824 x 8 = 25 MB; 3840x2160: 57 MB)
// LOCK GROUPING (headset test 2026-10-29 item 3, markers-360.md section 13): the game draws a lock diamond / reticle as its OWN depth-GREATER box
// draw next to the target box (screenshot: red square + red diamond lower left + '5' + range). MATCH-2 keeps it a separate group (the union is wider
// than group_half), it matched no contact, and its stamp was CUT against the box's (the range text went with it) and drawn as a track-less stamp at
// far_units (the flat-quad distance, never cached): the diamond sat 'in the HUD', the box's cached stamp outside the window held only bits. An unmatched
// box group of the same colour within this reach of a contact's matched box (centre distance, GS) and with no other contact projecting onto it is
// a SATELLITE of that contact: its pixels join the contact's stamp (one picture, one anchor, one distance, never cut against its own box).
constexpr float kSatReachGsX = 64.0f, kSatReachGsY = 48.0f;
// A cut stamp (a neighbour contact's stamp took part of it) or a window-clipped one never replaces a better cached picture of the contact, unless that
// picture is older than this (s): outside the window the cache is the last WHOLE in-window picture. (Since w12-2 a cut / window-clipped picture is q 0
// and never cached; since headset test 5 the only lesser cacheable picture is q 1 = label clipped, governed by kStampRecacheClippedS below.)
constexpr double kStampRecacheS = 2.0;
// LABEL-WHOLE (headset test 5, 2026-10-30c item 8, user-shots\9.png: "MiG-21bis" outside the window showed only the BOTTOM half of its glyphs and read
// as mirrored): a picture whose box is whole but whose label text is clipped (by the game window's edge: the label sits above / right of the box, so it
// leaves the window first; or by the cell window) is quality 1. It is cached only when the contact has no whole picture, and replaces a whole one only
// when that is older than this (s).
constexpr double kStampRecacheClippedS = 8.0;
// IDENT-HOLD (headset test 5 item 6, friendly boxes hit and miss): an identified contact lost during a world DROPOUT (frames stopped for > kDropoutGapS or
// arrived without A: a radio cut-in / cutscene / load; live-1936-step137 t 945.9 -> 960.9 s, 43 frames without A, every track lost, the wingman re-tracked
// at the same range/bearing as a NEW unidentified contact and unboxed for 25 s) leaves a ghost (its identity + cached pictures + its offset from the game
// camera). A new contact within kGhostS whose camera offset matches (max(kGhostGateMin, kGhostGateFrac x range)) takes it over. Losses while frames keep
// arriving (a kill: the wreck's draws stop) never make a ghost.
constexpr double kGhostS = 45.0;
constexpr float kGhostGateMin = 400.0f, kGhostGateFrac = 0.15f;
constexpr unsigned kGhostCap = 8;
constexpr double kDropoutGapS = 1.0;   // a frame gap longer than this (and lost_s) is a dropout; a shorter hitch is not (a kill during a hitch makes no ghost)


struct Params {
    int mode = 0;               // 0 off, 1 = all ours (game sprites hidden), 2 = game's in the window + ours outside
    // PROV (mk360p:1, opt-in since headset test 9 / section 22; markers-360.md section 19, headset test 7): a moving aircraft-like contact within the game's box range that the
    // game NEVER boxed (never identified: it was never in the window, or the game does not box it) gets our own unlabeled TGT-green box in every mode.
    bool prov = false; float prov_rgb[3] = {0.29f, 1.0f, 0.22f}; float prov_min_speed = 75.0f, prov_max_speed = 2500.0f, prov_max_range = 31000.0f; uint32_t prov_frames = 10;
    // IFF for provisional boxes (section 20): friend = the game's blue (measured FRND box colour 0.21,0.23,1.00), enemy = prov_rgb, unknown = neutral grey.
    // prov_unknown: 0 = skip an unknown contact, 1 = neutral unlabeled box (default; mk360pu). FORMATION: a contact within form_range of the game camera
    // whose velocity differs from the camera's by <= form_rel, for form_min_s of dwell (+dt in, -form_decay dt out, capped form_cap), is a friend (the wingman;
    // evidence: 6 of 7 boxed friendlies, 1 of ~36 boxed enemies). Until then it is unknown. A contact the game boxed takes the team of that colour.
    uint8_t prov_unknown = 1; float prov_friend_rgb[3] = {0.21f, 0.23f, 1.0f}, prov_unknown_rgb[3] = {0.75f, 0.75f, 0.75f};
    float form_range = 2500.0f, form_rel = 300.0f; double form_min_s = 30.0, form_cap = 60.0, form_decay = 0.5;
    // HEADSET TEST 8 (markers-360.md section 21). QUICK-FORMATION: a contact FIRST SEEN within form_first_range of the game camera and in formation latches
    // as a friend after form_quick_s (a wingman spawns beside the player; no enemy was ever first seen that close and co-moving), the latch is never undone
    // while the track lives, and a friend lost is remembered (friend ghost, friend_ghost_s) and re-latched on a new contact at the same offset.
    float form_first_range = 1200.0f; double form_quick_s = 2.0; double friend_ghost_s = 90.0;
    // NO-DOUBLES: no provisional box while a game box group (any colour, a lock diamond too) is within prov_box_gate GS of the contact's projection, nor for
    // prov_hold_s after the game last boxed the contact (in-window blink / pending identity). DEDUP: two provisional contacts closer than prov_dup_deg (angle
    // at the game camera) AND prov_dup_dist (units) are one aircraft (its parts tracked twice): one box.
    float prov_box_gate = 28.0f; double prov_hold_s = 2.0; float prov_dup_deg = 1.5f, prov_dup_dist = 250.0f;
    // WRECK: a contact born within heir_gate of where a game-boxed (identified) track was lost within heir_s, while frames kept arriving, is a debris heir of
    // that aircraft (a kill): its velocity must differ from the dead track's by more than heir_dv x the dead speed to be a wreck (else it is the same
    // aircraft re-tracked). A wreck never gets a provisional box. Needs the heir's velocity (second frame).
    float heir_gate = 300.0f; double heir_s = 1.5; float heir_dv = 0.35f;
    // COCKPIT GUARD: a provisional box is refused at a non-finite / origin / stale position and nearer than near_fade to the game camera (alpha ramp up to
    // near_min), where a stroke box with no depth test lies in or on the cockpit. prov_slip = the largest allowed |display - frame time| position (units).
    float prov_slip = 300.0f;
    bool unknown = false;       // mk360u:1: moving contacts without identity, dim grey, outside the window only (diagnostic)
    float self_radius = 80.0f;  // world draws within this of the game camera are the player's own aircraft (tac's tacself)
    float cluster = 60.0f;      // draws within this of each other are one contact (tac's tacclu)
    double lost_s = 0.25;       // no world draw of a contact for longer than this = gone
    uint32_t miss_frames = 9;   // in-window frames without its game box before the identity is dropped (LOCK-FX: a blinking lock box is off ~100 ms) ...
    double miss_s = 0.14;       // ... spanning at least this long
    float inner = 0.92f;        // the in-window miss rule uses the window shrunk to this fraction (edge boxes may be clipped by the game)
    uint32_t confirm = 2;       // consecutive matches with the same colour before a track takes a NEW identity (one-frame mismatches never stick)
    float far_units = 32500.0f; // the distance of an in-window box that matched no contact (direction exact, distance unknown; ~500 m at 65 u/m)
    float moving = 75.0f;       // units/s for mk360u
    // NBOX near-target policy (markers-360.md section 14; headset 2026-10-30: boxes appeared INSIDE the cockpit when flying close to a target).
    // The stamps have no depth test, so a contact near the eye gives a box with large stereo parallax that floats in the cockpit. Per eye the stamp's
    // distance is never below near_min (units; pushed OUT along the eye ray, the screen position is unchanged) and its size shrinks below near_ref:
    // scale = clamp(d / near_ref, near_floor, 1) about the anchor (box, label and lock diamond are one stamp group: they scale together).
    float near_min = 150.0f;    // mk360min:<u> (150 u = 30 m at 5 u/m, well beyond the canopy); 0 = off
    float near_ref = 750.0f;    // mk360ref:<u> (150 m): a contact nearer than this draws its stamp smaller; 0 = off (full size)
    float near_floor = 0.3f;    // mk360floor:<f> the smallest scale (0.05..1)
    // NEAR-FADE (headset test 5 item 6, "random blue flashes": a wingman passing the canopy drew its box at near_min with the floor scale for a few frames).
    // A stamp whose contact is nearer than near_min fades out: alpha = (d - near_fade) / (near_min - near_fade), 0 (not drawn) at or below near_fade.
    float near_fade = 75.0f;    // mk360fade:<u> (75 u = 15 m); 0 = off (no fade); >= near_min: hidden below near_fade, no ramp
    // MATCH-2 (markers-360.md section 9): live run S1 matched 1.4 % of the sprites although 93 % had a projected contact 8..16 GS units away
    float group_gap = 14.0f;   // same-colour sprites of one frame whose rectangles are this close are ONE game box (a box read as several clusters)
    float group_half = 14.0f;   // ... as long as the merged rectangle stays this small (half extent, GS): two neighbouring boxes never merge
    float gate_min = 20.0f;     // the match radius (GS units, after the learned offset) is never smaller than this (the old box-relative gate: ~8)
    float seek = 40.0f;         // residual learning: only an unambiguous nearest contact within this radius (GS) teaches the screen offset
    uint32_t goff_min = 6;      // global offset samples before it is applied to tracks without their own
    // PIECE-PAIR (markers-360.md section 17.2; headset test 6): the game's box is read as TWO same-colour pieces either side of the contact (step143:
    // groups 47772 of 47992 sprites, every piece half >= 8 GS and 8..16 GS from the contact, mean residual -0.3,-0.5). Before matching, two pieces whose
    // union is box sized (<= pair_half) and whose union CENTRE lies within pair_gate of a seen contact's frame-time projection (closer than either piece)
    // become ONE group (the whole box, centred on its contact). The second piece can then no longer be matched to another track (a missile, a ground
    // object, a neighbour) and give it the contact's colour and label for a few frames. pair_gate 0 = off (the old piece matching).
    float pair_gate = 10.0f, pair_half = 24.0f;
    // A contact that already has an identity takes a NEW colour only after this many consecutive matches of that colour (step143: a contact crossing
    // another's box took its colour after 2 frames, recolour forgets the cached pictures, then lost the identity in view: 17, t 350-355 s).
    uint32_t recolour_confirm = 6;
    // HEADSET TEST 9 (markers-360.md section 22): a cached game picture is drawn outside the window only while its contact is within the game's own box range
    // (units from the game camera; 0 = no limit). Evidence: the game's in-window boxes reached at most 32,354 u (110 samples, p99 28,212); contacts beyond it
    // kept a stale identity (one red F-4E in the window at 192,650 u without a box) and drew their boxes out to 205 km ("random boxes in the distance").
    float box_range = 33000.0f;
};
// HEADSET TEST 9 (section 22): only a COLOURED game box (blue / green / red / yellow) is a contact's box. Whitish groups (1.00,0.96,0.84 etc.: drawn around
// the 3,200-5,900 u/s four-contact group of the m03k event in step180 and test 9, then carried behind the player for 45 s) never give an identity and are
// never cached; in the window they stay the game's own live pixels. Same chroma rule as tac::team_of (grey / white = no team).
inline bool box_colour_ok(const float rgb[3]) noexcept { return acz::tac::team_of(rgb) != acz::tac::team_unknown; }
inline Params params_from(const acz::tac::Options& o) noexcept {
    Params p; p.mode = o.mk360; p.unknown = o.mk360_unknown; p.prov = o.mk360_prov; p.prov_unknown = o.mk360_prov_unknown; p.self_radius = o.self_radius; p.cluster = o.cluster;
    p.near_min = o.mk360_near_min; p.near_ref = o.mk360_near_ref; p.near_floor = o.mk360_near_floor; p.near_fade = o.mk360_near_fade; return p;
}

struct Track {
    uint32_t id = 0;
    float pos[3] = {}, vel[3] = {};
    bool vel_valid = false, seen_now = false;
    double t_seen = 0, t_first = 0;
    uint32_t frames = 0;
    // identity = the game box last matched to it (exact colour, glyph count, size)
    bool ever_ident = false;                             // the game boxed it at some point (a cleared identity = kill / retarget: never provisional)
    // IFF (section 20): team of the game's box colour on it (team_unknown = never boxed), formation dwell (s), its latch and last update time
    acz::tac::Team team = acz::tac::team_unknown; double form_s = 0, form_t = 0; bool form_friend = false;
    // headset test 8 (section 21): born near the game camera (QUICK-FORMATION), last time the game boxed it, a debris heir of a killed track (WRECK)
    bool first_close = false; double t_boxed = -1e30;
    bool heir = false, wreck = false; float heir_v[3] = {}, heir_speed = 0;
    bool ident = false; float rgb[3] = {1, 1, 1}; uint8_t glyphs = 0; float htx = acz::tac::kMarkerHalfTan, hty = acz::tac::kMarkerHalfTan;
    float lrgb[3] = {};                                  // the label's own colour (FRND only from a blue LABEL, never from the box colour)
    double t_ident = 0;
    uint8_t pend_n = 0; float pend_rgb[3] = {};          // a candidate new identity (hysteresis)
    uint32_t miss_n = 0; double miss_t0 = -1;            // in-window frames without its box
    bool in_window = false, in_inner = false;            // this frame, projected with this frame's A
    int sprite = -1;                                     // the sprite matched this frame (index into the frame's mark[]; a game-drawn one first)
    float prev_pos[3] = {}; bool prev_valid = false;     // the position of its previous update (one-frame time offset test)
    // BOX-LOCK (markers-360.md section 15): pos is the DISPLAY-time centroid with interp:1 (where the eyes see the model; stamps are anchored on it);
    // posf the FRAME-time centroid (the time of A and of the game's box sprites: matching, window test, offsets). Equal without interp.
    float posf[3] = {}, prev_posf[3] = {};
    float off[2] = {}; bool off_valid = false;           // learned GS offset: game box centre - projection of pos (EMA over its matches)
    float shw = 0, shh = 0; uint8_t sglyphs = 0;         // this frame's matched box group: half extents (GS) and label glyphs
    float slrgb[3] = {};                                 // ... and that label's colour
    // NBOX (mk360:3): the atlas cell holding this contact's last COMPLETE in-window game stamp (box + label pixels), its pixel size and the offset of
    // its top-left from the game box centre (GS, y down). cell_ok = the cell content is this contact's (drawn outside the window).
    int cell = -1; bool cell_ok = false; float c_ox = 0, c_oy = 0; uint16_t c_pw = 0, c_ph = 0;
    uint8_t c_q = 0; double c_t = 0;                     // the cached picture's quality (2 whole, 1 label clipped, 0 cut / window-clipped: never cached) and time
    uint16_t c_u0 = 0, c_v0 = 0;                         // LABEL-CORE (section 17.1): the cached picture = this texel offset in the cell (a q 1 picture keeps only its boxes)
    // LOCK GROUPING: a second cell for a satellite (lock diamond / reticle) too far from the box to share its cell window; same anchor, written only
    // together with the primary cached picture (cell2_ok = it belongs to the CURRENT cached picture).
    int cell2 = -1; bool cell2_ok = false; float c2_ox = 0, c2_oy = 0; uint16_t c2_pw = 0, c2_ph = 0;
    float rel[3] = {}; bool rel_ok = false;              // IDENT-HOLD: pos - the game camera, at its last sighting with a valid camera
    float speed() const noexcept { return vel_valid ? std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) : 0.0f; }
};
// Cumulative counters (summary-mk360 line).
struct Stats {
    uint64_t frames = 0, no_a = 0, sprites = 0, sprites_suppressed = 0, sprites_matched = 0, producer_matched = 0, consumer_matched = 0;
    uint64_t near_lt8 = 0, near_lt16 = 0, near_lt32 = 0, near_lt64 = 0, near_ge64 = 0, near_none = 0;   // sprite -> nearest projected contact (GS units)
    uint64_t created = 0, lost = 0, lost_ident = 0, ident_new = 0, ident_recolour = 0, ident_cleared_in_view = 0, self_draws = 0;
    uint64_t box_facts_unsafe = 0;                   // frames whose box draws were not all read (no in-window miss counted)
    uint64_t drawn_sprite = 0, drawn_out = 0, drawn_unknown = 0, drawn_prov = 0, skipped_in_window_unmatched = 0;
    uint64_t prov_friend = 0, prov_enemy = 0, prov_unknown = 0, prov_unknown_skipped = 0, form_confirmed = 0;   // IFF (section 20): provisional boxes by team
    // headset test 8 (section 21): provisional boxes refused (cumulative frames x contacts) by reason; quick formation latches; friend ghosts made / re-latched
    uint64_t prov_ref_box = 0, prov_ref_hold = 0, prov_ref_near = 0, prov_ref_bad = 0, prov_ref_dup = 0, prov_ref_wreck = 0, form_quick = 0, fghosts = 0, freacq = 0, wrecks = 0;
    // MATCH-2 diagnostics: box groups, sprite half sizes, signed residual sprite - nearest projection, matches only the wider/learned gate found
    uint64_t groups = 0, groups_multi = 0, half_lt2 = 0, half_lt4 = 0, half_lt8 = 0, half_ge8 = 0;
    double res_dx = 0, res_dy = 0; uint64_t res_n = 0;
    uint64_t beyond_raw_gate = 0, prev_closer = 0, offset_samples = 0;
    // NBOX (mk360:3): planned frames, stamps drawn live (this frame's game pixels) / cached (outside the window), copies into the atlas, overlap cuts, live
    // stamps without a free cell or emptied by a cut (left to the flat quad), frames without a usable marker image, cell evictions, edge refreshes kept
    // out of a contact's cell (clipped box), cells invalidated because the backend did not record the copies, atlas refused (size/budget), fresh plans
    // rejected after completion (producer image rewritten; their written cells invalidated: stamps_rejected).
    uint64_t nb_frames = 0, nb_live = 0, nb_cached = 0, nb_copies = 0, nb_cuts = 0, nb_no_cell = 0, nb_empty = 0, nb_no_image = 0, nb_evicted = 0,
             nb_edge_kept = 0, nb_invalidated = 0, nb_no_atlas = 0, nb_rejected = 0;
    // LOCK GROUPING: satellite groups joined to a contact's stamp, secondary (second-cell) stamps drawn, cut live stamps kept out of a better cache
    uint64_t nb_sat = 0, nb_second = 0, nb_cut_kept = 0;
    // headset test 5: live stamps whose label was clipped (quality 1), identified contacts outside the window drawn by nothing (no cached picture),
    // ghosts made / taken over (IDENT-HOLD), stamp draws faded / hidden by the near fade (eye 0 only)
    uint64_t nb_label_clipped = 0, nb_out_uncached = 0, ghosts = 0, reacquired = 0, nb_near_faded = 0, nb_near_hidden = 0, nb_off_view = 0;
    // headset test 6 (section 17): piece pairs joined into one box, recolours held back by recolour_confirm, live stamps of unmatched groups drawn at a
    // nearby contact's depth instead of far_units, q-1 pictures cached as their boxes only, lock diamonds (shape 1) seen
    uint64_t pairs = 0, recolour_held = 0, nb_depth_held = 0, nb_core_cached = 0, lock_shapes = 0;
    // headset test 9 (section 22): matched whitish groups that gave no identity, cached pictures not drawn beyond box_range, cut live stamps whose clean
    // part (box, or box + whole label) was cached
    uint64_t ident_pale = 0, nb_far = 0, nb_cut_cached = 0;
};
// mk360-stamp (section 17.1 diagnostic): the geometry of one live stamp with a label per summary period (the last label-clipped one, else the last one):
// the box and label rectangles the producer read (GS), the stamp's pixel rectangle, its quality, and the raw q of this frame's box / label vertices.
struct StampFacts { bool valid = false; uint32_t id = 0; uint8_t q = 0, glyphs = 0; bool lab_whole = false, cut = false, complete = false;
                    float box[4] = {}, lab[4] = {}; int px[4] = {}; acz::tac::QStat box_q, label_q; double t = 0; };
struct Ghost { Track tr; double t_lost = 0; };
struct Death { float pos[3] = {}, vel[3] = {}; double t = 0; };   // WRECK: a game-boxed track lost while frames kept arriving (a kill candidate)
struct Tracker {
    std::vector<Track> tracks; uint32_t next_id = 1; Stats st; float cam[3] = {}; float fwd[3] = {0, 0, 1}; bool cam_ok = false;
    float cam_v[3] = {}; bool cam_v_ok = false; float cam_prev[3] = {}; double cam_prev_t = -1.0; bool cam_prev_ok = false;   // the game camera's velocity (formation test)
    float goff[2] = {}; uint32_t goff_n = 0;             // global learned GS offset (box centre - projected contact), EMA
    uint32_t sprite_track_id[acz::tac::kMarkCap] = {};   // this frame: the id of the track each sprite's box group matched (0 = none)
    // this frame's box groups (update -> collect): group of each sprite, the group rectangle (GS), whether every member is suppressed
    int sprite_group[acz::tac::kMarkCap] = {};
    float gcx[acz::tac::kMarkCap] = {}, gcy[acz::tac::kMarkCap] = {}, ghw[acz::tac::kMarkCap] = {}, ghh[acz::tac::kMarkCap] = {};
    bool gall_sup[acz::tac::kMarkCap] = {};
    bool gdia[acz::tac::kMarkCap] = {};                  // LOCK-SHAPE: the group is a lock diamond only (never a contact's own box)
    unsigned ngroups = 0;
    // NBOX (mk360:3): atlas cell owners (0 = free, a track id, or kTempOwner | group for this frame's unmatched live stamp), last use (s), image size
    uint32_t cell_owner[kStampCells] = {}; double cell_t[kStampCells] = {}; uint32_t img_w = 0, img_h = 0;
    // IDENT-HOLD: identified contacts lost in a dropout (their cells stay owned by the ghost's id until taken over, evicted or expired)
    std::vector<Ghost> ghosts; double last_t = -1.0, dropout_t = -1e30;
    std::vector<Ghost> fghosts;                          // QUICK-FORMATION: friend-latched tracks lost (re-latched on a new contact at the same camera offset)
    std::vector<Death> deaths;                           // WRECK
    StampFacts sf;                                       // mk360-stamp: the period's sample (stamp_line clears it)
};
constexpr uint32_t kTempOwner = 0x80000000u;

// ---------------------------------------------------------------------------------------------- geometry of A (record +0, h = (p, 1) x A, row vectors)
inline void col_(const float A[16], int j, double c[3]) noexcept { c[0] = A[j]; c[1] = A[4 + j]; c[2] = A[8 + j]; }
// The camera centre of A: h.x = h.y = h.w = 0 there (columns 0, 1, 3). false = singular.
inline bool camera_centre(const float A[16], float C[3]) noexcept {
    double m[3][3], b[3];
    const int cols[3] = {0, 1, 3};
    for (int r = 0; r < 3; ++r) { double c[3]; col_(A, cols[r], c); for (int k = 0; k < 3; ++k) m[r][k] = c[k]; b[r] = -double(A[12 + cols[r]]); }
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(std::fabs(det) > 1e-30) || !std::isfinite(det)) return false;
    for (int k = 0; k < 3; ++k) {
        double mk[3][3];
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) mk[r][c] = c == k ? b[r] : m[r][c];
        const double dk = mk[0][0] * (mk[1][1] * mk[2][2] - mk[1][2] * mk[2][1]) - mk[0][1] * (mk[1][0] * mk[2][2] - mk[1][2] * mk[2][0]) +
                          mk[0][2] * (mk[1][0] * mk[2][1] - mk[1][1] * mk[2][0]);
        C[k] = float(dk / det);
    }
    return std::isfinite(C[0]) && std::isfinite(C[1]) && std::isfinite(C[2]);
}
// The world direction (unit) of the GS screen point (xs, ys): the line h.x - xs h.w = 0, h.y - ys h.w = 0, oriented to positive w.
inline bool screen_ray(const float A[16], float xs, float ys, float dir[3]) noexcept {
    double c0[3], c1[3], c3[3], n0[3], n1[3];
    col_(A, 0, c0); col_(A, 1, c1); col_(A, 3, c3);
    for (int k = 0; k < 3; ++k) { n0[k] = c0[k] - double(xs) * c3[k]; n1[k] = c1[k] - double(ys) * c3[k]; }
    double d[3] = {n0[1] * n1[2] - n0[2] * n1[1], n0[2] * n1[0] - n0[0] * n1[2], n0[0] * n1[1] - n0[1] * n1[0]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-30) || !std::isfinite(l)) return false;
    const double s = (d[0] * c3[0] + d[1] * c3[1] + d[2] * c3[2]) >= 0 ? 1.0 : -1.0;
    for (int k = 0; k < 3; ++k) dir[k] = float(s * d[k] / l);
    return true;
}
// ROLL (headset test 2026-10-29 item 3b, markers-360.md section 13): the world directions of the game screen's +x (right) and -y (up) axes at the GS
// point (gx, gy), unit and perpendicular to its ray (and to each other). The game laid its box pixels out along these: they carry the camera's roll of
// THAT frame. false = degenerate A.
inline bool unit3_(double v[3]) noexcept {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-12) || !std::isfinite(l)) return false;
    for (int k = 0; k < 3; ++k) v[k] /= l;
    return true;
}
inline bool screen_axes(const float A[16], float gx, float gy, float sr[3], float su[3]) noexcept {
    float d0[3], dx[3], dy[3];
    if (!screen_ray(A, gx, gy, d0) || !screen_ray(A, gx + 1.0f, gy, dx) || !screen_ray(A, gx, gy - 1.0f, dy)) return false;
    double r[3], u[3], d[3];
    for (int k = 0; k < 3; ++k) { d[k] = d0[k]; r[k] = double(dx[k]) - d0[k]; u[k] = double(dy[k]) - d0[k]; }
    const double rd = r[0] * d[0] + r[1] * d[1] + r[2] * d[2];
    for (int k = 0; k < 3; ++k) r[k] -= rd * d[k];
    if (!unit3_(r)) return false;
    const double ud = u[0] * d[0] + u[1] * d[1] + u[2] * d[2], ur = u[0] * r[0] + u[1] * r[1] + u[2] * r[2];
    for (int k = 0; k < 3; ++k) u[k] -= ud * d[k] + ur * r[k];
    if (!unit3_(u)) return false;
    for (int k = 0; k < 3; ++k) { sr[k] = float(r[k]); su[k] = float(u[k]); }
    return true;
}
// The game camera's screen axes carried to the world direction d (unit) of a contact OUTSIDE the window: up = the camera up made perpendicular to d
// (straight above / below the camera: the camera back / forward, as a head tilted up / down sees it), right completes (up, d) with the camera's own
// handedness (a contact behind: the camera's left is the viewer's right). The cached stamp then rolls with the aircraft like the game's HUD boxes.
inline bool contact_axes(const float A[16], const float d[3], float sr[3], float su[3]) noexcept {
    float R[3], U[3], F[3];
    if (!screen_axes(A, kWinCx, kWinCy, R, U) || !screen_ray(A, kWinCx, kWinCy, F)) return false;
    const double hand = (double(U[1]) * F[2] - double(U[2]) * F[1]) * R[0] + (double(U[2]) * F[0] - double(U[0]) * F[2]) * R[1] +
                        (double(U[0]) * F[1] - double(U[1]) * F[0]) * R[2];
    const double s = hand >= 0 ? 1.0 : -1.0;
    const double ud = double(U[0]) * d[0] + double(U[1]) * d[1] + double(U[2]) * d[2];
    double u[3];
    for (int k = 0; k < 3; ++k) u[k] = U[k] - ud * d[k];
    if (std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) < 0.2) {
        const double sg = ud > 0 ? -1.0 : 1.0, fd = double(F[0]) * d[0] + double(F[1]) * d[1] + double(F[2]) * d[2];
        for (int k = 0; k < 3; ++k) u[k] = sg * (F[k] - fd * d[k]);
    }
    if (!unit3_(u)) return false;
    double r[3] = {s * (u[1] * d[2] - u[2] * d[1]), s * (u[2] * d[0] - u[0] * d[2]), s * (u[0] * d[1] - u[1] * d[0])};
    if (!unit3_(r)) return false;
    for (int k = 0; k < 3; ++k) { sr[k] = float(r[k]); su[k] = float(u[k]); }
    return true;
}
// Inside the game window (frac = 1: exactly the window; < 1 shrunk) and in front of the camera.
inline bool in_window(const float A[16], const float p[3], float frac, float* xs_out = nullptr, float* ys_out = nullptr) noexcept {
    float xs, ys, w;
    if (!acz::tac::project_ps2(A, p, xs, ys, w)) return false;
    if (xs_out) *xs_out = xs;
    if (ys_out) *ys_out = ys;
    return std::fabs(xs - kWinCx) <= kWinHx * frac && std::fabs(ys - kWinCy) <= kWinHy * frac;
}
inline bool same_colour(const float a[3], const float b[3]) noexcept {
    return std::fabs(a[0] - b[0]) <= 0.15f && std::fabs(a[1] - b[1]) <= 0.15f && std::fabs(a[2] - b[2]) <= 0.15f;
}
inline void box_half_tan(float hw, float hh, float& tx, float& ty) noexcept {
    if (!(std::isfinite(hw) && std::isfinite(hh) && hw > 0 && hh > 0)) { tx = ty = acz::tac::kMarkerHalfTan; return; }
    const float ratio = std::fmin(1.25f, std::fmax(0.8f, hw / hh)), g = std::sqrt(hw * hh);
    tx = std::fmin(0.08f, std::fmax(0.006f, g * std::sqrt(ratio) * kTanPerGsBox));
    ty = std::fmin(0.08f, std::fmax(0.006f, g / std::sqrt(ratio) * kTanPerGsBox));
}
inline float clamp_tan(float v) noexcept { return std::isfinite(v) ? std::fmin(0.08f, std::fmax(0.006f, v)) : acz::tac::kMarkerHalfTan; }
// rgb = the LABEL's own colour (MarkerSprite::lrgb): a green / red / yellow box never gets FRND from a guess (headset test 2026-10-28 item 8).
inline acz::tac::Label label_of(uint8_t glyphs, const float rgb[3]) noexcept {
    if (!acz::tac::label_text_redrawable(glyphs, rgb)) return acz::tac::label_none;   // never invented: 4 glyphs in a non-blue colour = lock / miss text, not FRND
    return glyphs == 4 ? acz::tac::label_frnd : acz::tac::label_tgt;
}

// WRECK (section 21): a track born within heir_gate of where a game-boxed track was lost within heir_s (dead-reckoned) is its debris heir candidate.
inline void mark_heir(const Tracker& tk, Track& tr, double t, const Params& p) {
    for (const Death& d : tk.deaths) {
        const double dt = t - d.t;
        if (dt < 0.0 || dt > p.heir_s) continue;
        float pr[3];
        for (int i = 0; i < 3; ++i) pr[i] = d.pos[i] + d.vel[i] * float(dt);
        const float dx = tr.pos[0] - pr[0], dy = tr.pos[1] - pr[1], dz = tr.pos[2] - pr[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > p.heir_gate) continue;
        tr.heir = true; for (int i = 0; i < 3; ++i) tr.heir_v[i] = d.vel[i];
        tr.heir_speed = std::sqrt(d.vel[0] * d.vel[0] + d.vel[1] * d.vel[1] + d.vel[2] * d.vel[2]);
        return;
    }
}

// ---------------------------------------------------------------------------------------------- one producer frame
// cam = the game camera (terrain camera of the frame), t = seconds (monotonic, the frame's publication time).
inline void update(Tracker& tk, const acz::tac::TacFrame& f, const float cam[3], double t, const Params& p) {
    using namespace acz::tac;
    Stats& st = tk.st;
    ++st.frames;
    if (!f.a_valid) ++st.no_a;
    // IDENT-HOLD: a dropout = frames stopped for longer than kDropoutGapS (and lost_s), or a frame without A (the game is not drawing its flight view)
    if (!f.a_valid || (tk.last_t >= 0 && t - tk.last_t > std::max(p.lost_s, kDropoutGapS))) tk.dropout_t = t;
    tk.last_t = t;
    // 1. contacts: the frame's replayed world draws minus the player's own, clustered (aircraft parts carry their own matrices near the object)
    struct C { double sum[3] = {}, sumf[3] = {}; uint32_t n = 0; float pos[3] = {}, posf[3] = {}; int pmark = -1; };
    C cl[kObjCap];
    int cl_of_obj[kObjCap];
    unsigned nc = 0;
    const unsigned nobj = std::min<unsigned>(f.nobj, kObjCap);
    const bool cam_ok = cam && std::isfinite(cam[0]) && std::isfinite(cam[1]) && std::isfinite(cam[2]);
    tk.cam_ok = cam_ok;
    if (cam_ok) for (int i = 0; i < 3; ++i) tk.cam[i] = cam[i];
    // game camera velocity (IFF formation test): a gap > 0.5 s, a teleport (> 12,000 u/s: a cut / respawn) or a missing camera invalidates it
    if (cam_ok && tk.cam_prev_ok && t - tk.cam_prev_t > 1e-4 && t - tk.cam_prev_t <= 0.5) {
        const double dtc = t - tk.cam_prev_t;
        float inst[3];
        for (int i = 0; i < 3; ++i) inst[i] = float((cam[i] - tk.cam_prev[i]) / dtc);
        const float sp = std::sqrt(inst[0] * inst[0] + inst[1] * inst[1] + inst[2] * inst[2]);
        if (std::isfinite(sp) && sp <= 12000.0f) {
            if (tk.cam_v_ok) for (int i = 0; i < 3; ++i) tk.cam_v[i] = 0.7f * tk.cam_v[i] + 0.3f * inst[i];
            else for (int i = 0; i < 3; ++i) tk.cam_v[i] = inst[i];
            tk.cam_v_ok = true;
        } else tk.cam_v_ok = false;
    } else if (!cam_ok || t - tk.cam_prev_t > 0.5) tk.cam_v_ok = false;
    tk.cam_prev_ok = cam_ok; tk.cam_prev_t = t;
    if (cam_ok) for (int i = 0; i < 3; ++i) tk.cam_prev[i] = cam[i];
    if (f.a_valid) { double c3[3]; col_(f.A, 3, c3); const double l = std::sqrt(c3[0] * c3[0] + c3[1] * c3[1] + c3[2] * c3[2]);
                     if (l > 1e-12 && std::isfinite(l)) for (int i = 0; i < 3; ++i) tk.fwd[i] = float(c3[i] / l); }   // A's w column = view forward
    for (unsigned i = 0; i < nobj; ++i) {
        cl_of_obj[i] = -1;
        const ObjDraw& d = f.obj[i];
        if (!std::isfinite(d.t[0]) || !std::isfinite(d.t[1]) || !std::isfinite(d.t[2])) continue;
        if (cam_ok) {
            const float dx = d.t[0] - cam[0], dy = d.t[1] - cam[1], dz = d.t[2] - cam[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < p.self_radius) { ++st.self_draws; continue; }
        }
        int j = -1;
        for (unsigned k = 0; k < nc; ++k) {
            const double n = double(cl[k].n);
            if (std::fabs(cl[k].sum[0] / n - d.t[0]) <= p.cluster && std::fabs(cl[k].sum[1] / n - d.t[1]) <= p.cluster &&
                std::fabs(cl[k].sum[2] / n - d.t[2]) <= p.cluster) { j = int(k); break; }
        }
        if (j < 0) { j = int(nc++); cl[j] = C{}; }
        for (int k = 0; k < 3; ++k) { cl[j].sum[k] += d.t[k]; cl[j].sumf[k] += (d.tf_valid && std::isfinite(d.tf[k])) ? d.tf[k] : d.t[k]; }
        ++cl[j].n;
        cl_of_obj[i] = j;
    }
    for (unsigned k = 0; k < nc; ++k) for (int i = 0; i < 3; ++i) { cl[k].pos[i] = float(cl[k].sum[i] / double(cl[k].n)); cl[k].posf[i] = float(cl[k].sumf[i] / double(cl[k].n)); }
    // 2. clusters -> tracks: greedy nearest pairs within a velocity-aware gate (no coasting: a track unseen for lost_s is dropped below)
    struct P { float d; unsigned a, c; };
    std::vector<P> pairs;
    for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        const Track& tr = tk.tracks[a];
        const double dt = std::min(p.lost_s, std::max(0.0, t - tr.t_seen));
        float pred[3];
        for (int i = 0; i < 3; ++i) pred[i] = tr.pos[i] + (tr.vel_valid ? tr.vel[i] * float(dt) : 0.0f);
        const float gate = 2.0f * p.cluster + 30.0f + (tr.vel_valid ? 0.5f * tr.speed() * float(std::max(dt, 1.0 / 60.0)) : 0.0f);
        for (unsigned c = 0; c < nc; ++c) {
            const float dx = cl[c].pos[0] - pred[0], dy = cl[c].pos[1] - pred[1], dz = cl[c].pos[2] - pred[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d <= gate) pairs.push_back({d, a, c});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const P& x, const P& y) { return x.d < y.d || (x.d == y.d && (x.a < y.a || (x.a == y.a && x.c < y.c))); });
    std::vector<int> c_of(tk.tracks.size(), -1), t_of(nc, -1);
    for (const P& q : pairs) if (c_of[q.a] < 0 && t_of[q.c] < 0) { c_of[q.a] = int(q.c); t_of[q.c] = int(q.a); }
    for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        Track& tr = tk.tracks[a];
        tr.sprite = -1;
        const int c = c_of[a];
        if (c < 0) { tr.seen_now = false; continue; }
        const double dt = t - tr.t_seen;
        if (dt > 1e-3) {
            float inst[3];
            for (int i = 0; i < 3; ++i) inst[i] = float((cl[c].pos[i] - tr.pos[i]) / dt);
            if (std::isfinite(inst[0]) && std::isfinite(inst[1]) && std::isfinite(inst[2])) {
                if (tr.vel_valid) for (int i = 0; i < 3; ++i) tr.vel[i] = 0.6f * tr.vel[i] + 0.4f * inst[i];
                else { for (int i = 0; i < 3; ++i) tr.vel[i] = inst[i]; tr.vel_valid = true; }
            }
        }
        for (int i = 0; i < 3; ++i) { tr.prev_pos[i] = tr.pos[i]; tr.pos[i] = cl[c].pos[i]; tr.prev_posf[i] = tr.posf[i]; tr.posf[i] = cl[c].posf[i]; }
        tr.prev_valid = true;
        tr.t_seen = t; ++tr.frames; tr.seen_now = true;
        tr.rel_ok = cam_ok; if (cam_ok) for (int i = 0; i < 3; ++i) tr.rel[i] = tr.pos[i] - cam[i];
    }
    for (unsigned c = 0; c < nc; ++c) {
        if (t_of[c] >= 0) continue;
        Track tr;
        tr.id = tk.next_id++;
        for (int i = 0; i < 3; ++i) { tr.pos[i] = cl[c].pos[i]; tr.posf[i] = cl[c].posf[i]; }
        tr.rel_ok = cam_ok; if (cam_ok) for (int i = 0; i < 3; ++i) tr.rel[i] = tr.pos[i] - cam[i];
        tr.t_seen = tr.t_first = tr.form_t = t; tr.frames = 1; tr.seen_now = true;
        tr.first_close = cam_ok && std::sqrt(tr.rel[0] * tr.rel[0] + tr.rel[1] * tr.rel[1] + tr.rel[2] * tr.rel[2]) <= p.form_first_range;
        mark_heir(tk, tr, t, p);
        t_of[c] = int(tk.tracks.size());
        tk.tracks.push_back(tr);
        c_of.push_back(int(c));
        ++st.created;
    }
    // 2b. IFF formation dwell (section 20): in formation with the game camera = within form_range and moving with it (relative speed <= form_rel)
    for (Track& tr : tk.tracks) {
        if (!tr.seen_now) continue;
        const double dtf = std::min(0.25, std::max(0.0, t - tr.form_t)); tr.form_t = t;
        if (dtf <= 0.0) continue;
        bool in = false;
        if (cam_ok && tk.cam_v_ok && tr.vel_valid) {
            const float dx = tr.pos[0] - cam[0], dy = tr.pos[1] - cam[1], dz = tr.pos[2] - cam[2];
            const float rv[3] = {tr.vel[0] - tk.cam_v[0], tr.vel[1] - tk.cam_v[1], tr.vel[2] - tk.cam_v[2]};
            in = std::sqrt(dx * dx + dy * dy + dz * dz) <= p.form_range && std::sqrt(rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2]) <= p.form_rel;
        }
        // QUICK-FORMATION (section 21): a contact born near the game camera dwells form_min_s / form_quick_s times faster for its first 10 s; the latch is
        // never undone (a known wingman is never an enemy again while the track lives), only the dwell of an unlatched candidate decays
        if (tr.first_close && t - tr.t_first > 10.0) tr.first_close = false;
        const double rate = tr.first_close ? p.form_min_s / std::max(0.1, p.form_quick_s) : 1.0;
        if (in) tr.form_s = std::min(p.form_cap, tr.form_s + dtf * rate); else if (!tr.form_friend) tr.form_s = std::max(0.0, tr.form_s - p.form_decay * dtf);
        if (tr.form_s >= p.form_min_s && !tr.form_friend) { tr.form_friend = true; ++st.form_confirmed; if (rate > 1.0) ++st.form_quick; }
        // WRECK: the heir's velocity is known from its second frame: a debris heir moves unlike the aircraft it came from
        if (tr.heir && !tr.wreck && tr.vel_valid && tr.frames >= 3) {
            const float dv = std::sqrt((tr.vel[0] - tr.heir_v[0]) * (tr.vel[0] - tr.heir_v[0]) + (tr.vel[1] - tr.heir_v[1]) * (tr.vel[1] - tr.heir_v[1]) +
                                       (tr.vel[2] - tr.heir_v[2]) * (tr.vel[2] - tr.heir_v[2]));
            if (dv > p.heir_dv * std::fmax(tr.heir_speed, 50.0f)) { tr.wreck = true; ++st.wrecks; }
            tr.heir = false;
        }
    }
    // 3. window state of every seen track (this frame's A: the camera the game drew its boxes with, at the FRAME-time position: BOX-LOCK)
    for (Track& tr : tk.tracks) {
        tr.in_window = tr.in_inner = false;
        if (tr.seen_now && f.a_valid) { tr.in_window = in_window(f.A, tr.posf, 1.0f); tr.in_inner = tr.in_window && in_window(f.A, tr.posf, p.inner); }
    }
    // 4. game box sprites -> tracks (MATCH-2, markers-360.md section 9).
    //  a. Same-colour sprites whose rectangles touch (group_gap) and whose union stays box sized (group_half) are ONE game box (a box the producer
    //     read as several clusters); the group's rectangle centre is the box centre.
    //  b. Each seen track projects with this frame's A, plus its learned GS offset (else the global one): the screen offset between where the game
    //     centres its box and the replayed draws' centroid. Its previous-frame position is tried too (a one-frame time offset).
    //  c. Candidates within max(box half extent x 1.5 + 4, gate_min) of the corrected projection; the producer's own per-draw match (sprite.obj) is
    //     preferred; score = distance^2, quartered for the track that already carries this colour. Greedy one group <-> one track.
    //  d. Matched residuals teach the track's offset; unambiguous nearest residuals (within seek, second contact >= 2x + 8 farther) the global one.
    const unsigned nm = std::min<unsigned>(f.nmark, kMarkCap);
    for (unsigned m = 0; m < kMarkCap; ++m) tk.sprite_track_id[m] = 0;
    struct Pj { bool ok = false, okp = false; float x = 0, y = 0, xp = 0, yp = 0; };
    std::vector<Pj> pj(tk.tracks.size());
    for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        const Track& tr = tk.tracks[a];
        if (!tr.seen_now || !f.a_valid) continue;
        float w;
        // BOX-LOCK: the game drew its sprites with A at FRAME time: project the frame-time centroid (with interp:1 the display-time one is a game frame
        // ahead: 25 u for a 1500 u/s target = 35 GS at 300 u, beyond the 20 GS gate; the learned offsets absorbed that lag and flipped with roll)
        pj[a].ok = project_ps2(f.A, tr.posf, pj[a].x, pj[a].y, w);
        if (tr.prev_valid) pj[a].okp = project_ps2(f.A, tr.prev_posf, pj[a].xp, pj[a].yp, w);
    }
    // a. groups (union-find over <= kMarkCap sprites)
    int gp[kMarkCap];
    for (unsigned m = 0; m < nm; ++m) gp[m] = int(m);
    auto groot = [&](int x) { while (gp[x] != x) x = gp[x] = gp[gp[x]]; return x; };
    for (unsigned i = 0; i < nm; ++i) for (unsigned j = i + 1; j < nm; ++j) {
        const MarkerSprite& u = f.mark[i]; const MarkerSprite& v = f.mark[j];
        if (!same_colour(u.rgb, v.rgb)) continue;
        const float gx = std::fmax(0.0f, std::fabs(u.cx - v.cx) - u.hw - v.hw), gy = std::fmax(0.0f, std::fabs(u.cy - v.cy) - u.hh - v.hh);
        if (gx > p.group_gap || gy > p.group_gap) continue;
        const int ri = groot(int(i)), rj = groot(int(j));
        if (ri == rj) continue;
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;   // the union of both groups must stay box sized
        for (unsigned k = 0; k < nm; ++k) {
            const int rk = groot(int(k));
            if (rk != ri && rk != rj) continue;
            const MarkerSprite& s = f.mark[k];
            x0 = std::fmin(x0, s.cx - s.hw); x1 = std::fmax(x1, s.cx + s.hw); y0 = std::fmin(y0, s.cy - s.hh); y1 = std::fmax(y1, s.cy + s.hh);
        }
        if (0.5f * (x1 - x0) > p.group_half || 0.5f * (y1 - y0) > p.group_half) continue;
        gp[std::max(ri, rj)] = std::min(ri, rj);
    }
    // a2. PIECE-PAIR (section 17.2): two box pieces (never a lock diamond) of one colour whose union is box sized and centred on a seen contact's frame-time
    //     projection (within pair_gate, and closer to it than either piece) are ONE box. Greedy by that distance; a piece and a contact pair at most once.
    int pair_track_of_root[kMarkCap];
    for (unsigned m = 0; m < kMarkCap; ++m) pair_track_of_root[m] = -1;
    if (p.pair_gate > 0.0f && nm >= 2) {
        struct RR { float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; int rep = -1; bool dia = true; };
        RR rr[kMarkCap];
        for (unsigned m = 0; m < nm; ++m) {
            const int r = groot(int(m)); const MarkerSprite& s = f.mark[m]; RR& q = rr[r];
            if (q.rep < 0) q.rep = int(m);
            q.x0 = std::fmin(q.x0, s.cx - s.hw); q.x1 = std::fmax(q.x1, s.cx + s.hw); q.y0 = std::fmin(q.y0, s.cy - s.hh); q.y1 = std::fmax(q.y1, s.cy + s.hh);
            if (s.shape == 0) q.dia = false;
        }
        struct PC { float e; unsigned a; int r1, r2; };
        std::vector<PC> pcs;
        for (unsigned a = 0; a < tk.tracks.size(); ++a) {
            if (!pj[a].ok) continue;
            for (unsigned r1 = 0; r1 < nm; ++r1) {
                if (rr[r1].rep < 0 || rr[r1].dia || groot(int(r1)) != int(r1)) continue;
                for (unsigned r2 = r1 + 1; r2 < nm; ++r2) {
                    if (rr[r2].rep < 0 || rr[r2].dia || groot(int(r2)) != int(r2)) continue;
                    if (!same_colour(f.mark[rr[r1].rep].rgb, f.mark[rr[r2].rep].rgb)) continue;
                    const float x0 = std::fmin(rr[r1].x0, rr[r2].x0), x1 = std::fmax(rr[r1].x1, rr[r2].x1);
                    const float y0 = std::fmin(rr[r1].y0, rr[r2].y0), y1 = std::fmax(rr[r1].y1, rr[r2].y1);
                    if (0.5f * (x1 - x0) > p.pair_half || 0.5f * (y1 - y0) > p.pair_half) continue;
                    const float ux = 0.5f * (x0 + x1), uy = 0.5f * (y0 + y1);
                    float e = std::hypot(ux - pj[a].x, uy - pj[a].y);
                    const float d1 = std::hypot(0.5f * (rr[r1].x0 + rr[r1].x1) - pj[a].x, 0.5f * (rr[r1].y0 + rr[r1].y1) - pj[a].y);
                    const float d2 = std::hypot(0.5f * (rr[r2].x0 + rr[r2].x1) - pj[a].x, 0.5f * (rr[r2].y0 + rr[r2].y1) - pj[a].y);
                    if (!(e <= p.pair_gate) || !(e + 2.0f < std::fmin(d1, d2))) continue;
                    pcs.push_back({e, a, int(r1), int(r2)});
                }
            }
        }
        std::sort(pcs.begin(), pcs.end(), [](const PC& x, const PC& y) {
            return x.e < y.e || (x.e == y.e && (x.a < y.a || (x.a == y.a && (x.r1 < y.r1 || (x.r1 == y.r1 && x.r2 < y.r2))))); });
        std::vector<uint8_t> a_used(tk.tracks.size(), 0); uint8_t r_used[kMarkCap] = {};
        for (const PC& q : pcs) {
            if (a_used[q.a] || r_used[q.r1] || r_used[q.r2]) continue;
            a_used[q.a] = 1; r_used[q.r1] = r_used[q.r2] = 1;
            gp[q.r2] = q.r1;   // r1 < r2: r1 stays the root
            pair_track_of_root[q.r1] = int(q.a);
            ++st.pairs;
        }
    }
    struct Grp { float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; unsigned n = 0, rep = 0; int prod_track = -1; float cx = 0, cy = 0, hw = 0, hh = 0; bool dia = true; };
    std::vector<Grp> gr; int g_of[kMarkCap];
    for (unsigned m = 0; m < nm; ++m) {
        const MarkerSprite& s = f.mark[m];
        ++st.sprites;
        if (s.suppressed) ++st.sprites_suppressed;
        if (s.shape != 0) ++st.lock_shapes;
        const float hm = std::fmax(s.hw, s.hh);
        if (hm < 2) ++st.half_lt2; else if (hm < 4) ++st.half_lt4; else if (hm < 8) ++st.half_lt8; else ++st.half_ge8;
        // the raw diagnostic (unchanged meaning): this sprite's centre -> the nearest projected seen contact, no offset, and its signed residual
        float nearest = 1e30f, rdx = 0, rdy = 0;
        for (unsigned a = 0; a < tk.tracks.size() && s.shape == 0; ++a) {   // a lock diamond is not a box piece: none (counted in lock_shapes)
            if (!pj[a].ok) continue;
            const float dx = s.cx - pj[a].x, dy = s.cy - pj[a].y, d = std::sqrt(dx * dx + dy * dy);
            if (d < nearest) { nearest = d; rdx = dx; rdy = dy; }
        }
        if (s.shape != 0) {}
        else if (nearest >= 1e29f) ++st.near_none;
        else if (nearest < 8) ++st.near_lt8; else if (nearest < 16) ++st.near_lt16; else if (nearest < 32) ++st.near_lt32;
        else if (nearest < 64) ++st.near_lt64; else ++st.near_ge64;
        if (nearest < 64) { st.res_dx += rdx; st.res_dy += rdy; ++st.res_n; }
        const int r = groot(int(m));
        int g = -1;
        for (unsigned k = 0; k < m; ++k) if (groot(int(k)) == r) { g = g_of[k]; break; }
        if (g < 0) { g = int(gr.size()); gr.push_back(Grp{}); gr.back().rep = m; }
        g_of[m] = g;
        Grp& G = gr[g];
        G.x0 = std::fmin(G.x0, s.cx - s.hw); G.x1 = std::fmax(G.x1, s.cx + s.hw); G.y0 = std::fmin(G.y0, s.cy - s.hh); G.y1 = std::fmax(G.y1, s.cy + s.hh);
        ++G.n;
        // the representative: a box member before a diamond, then a game-drawn member (never a second box, collect)
        { const MarkerSprite& R = f.mark[G.rep];
          if ((R.shape != 0 && s.shape == 0) || (R.shape == s.shape && !s.suppressed && R.suppressed)) G.rep = m; }
        if (s.shape == 0) G.dia = false;
        if (G.prod_track < 0 && s.obj && s.obj <= nobj && cl_of_obj[s.obj - 1] >= 0) G.prod_track = t_of[cl_of_obj[s.obj - 1]];
    }
    // PIECE-PAIR: a pair formed on a contact is that contact's box (the producer's per-piece match may have named a nearer object for one piece)
    for (unsigned m = 0; m < nm; ++m) { const int r = groot(int(m)); if (r == int(m) && pair_track_of_root[r] >= 0) gr[g_of[m]].prod_track = pair_track_of_root[r]; }
    tk.ngroups = unsigned(gr.size());
    for (unsigned g = 0; g < gr.size(); ++g) {
        Grp& G = gr[g];
        G.cx = 0.5f * (G.x0 + G.x1); G.cy = 0.5f * (G.y0 + G.y1); G.hw = 0.5f * (G.x1 - G.x0); G.hh = 0.5f * (G.y1 - G.y0);
        ++st.groups; if (G.n > 1) ++st.groups_multi;
        tk.gcx[g] = G.cx; tk.gcy[g] = G.cy; tk.ghw[g] = G.hw; tk.ghh[g] = G.hh; tk.gall_sup[g] = true; tk.gdia[g] = G.dia;
    }
    for (unsigned m = 0; m < nm; ++m) { tk.sprite_group[m] = g_of[m]; if (!f.mark[m].suppressed) tk.gall_sup[g_of[m]] = false; }
    // d (global part, before matching: the frame's own unambiguous residuals)
    for (const Grp& G : gr) {
        if (G.dia) continue;
        float d1 = 1e30f, d2 = 1e30f, rx = 0, ry = 0;
        for (unsigned a = 0; a < tk.tracks.size(); ++a) {
            if (!pj[a].ok) continue;
            const float dx = G.cx - pj[a].x, dy = G.cy - pj[a].y, d = std::sqrt(dx * dx + dy * dy);
            if (d < d1) { d2 = d1; d1 = d; rx = dx; ry = dy; } else if (d < d2) d2 = d;
        }
        if (!(d1 <= p.seek) || d2 < 2.0f * d1 + 8.0f) continue;
        if (!tk.goff_n) { tk.goff[0] = rx; tk.goff[1] = ry; } else { tk.goff[0] += 0.1f * (rx - tk.goff[0]); tk.goff[1] += 0.1f * (ry - tk.goff[1]); }
        ++tk.goff_n; ++st.offset_samples;
    }
    const bool goff_ok = tk.goff_n >= p.goff_min;
    // b + c
    struct Q { float s; unsigned g, a; bool prod, beyond, prev; };
    std::vector<Q> qs;
    for (unsigned g = 0; g < gr.size(); ++g) {
        const Grp& G = gr[g];
        if (G.dia) continue;   // LOCK-SHAPE: a diamond alone is never a contact's box (it joins the box's stamp as a satellite)
        const float hm = std::fmax(G.hw, G.hh), gate = std::fmax(hm * 1.5f + 4.0f, p.gate_min);
        const MarkerSprite& s = f.mark[G.rep];
        for (unsigned a = 0; a < tk.tracks.size(); ++a) {
            if (!pj[a].ok) continue;
            const Track& tr = tk.tracks[a];
            const float ox = tr.off_valid ? tr.off[0] : goff_ok ? tk.goff[0] : 0.0f, oy = tr.off_valid ? tr.off[1] : goff_ok ? tk.goff[1] : 0.0f;
            const float dx = G.cx - (pj[a].x + ox), dy = G.cy - (pj[a].y + oy);
            float d = std::sqrt(dx * dx + dy * dy);
            bool prev = false;
            if (pj[a].okp) {
                const float ex = G.cx - (pj[a].xp + ox), ey = G.cy - (pj[a].yp + oy), e = std::sqrt(ex * ex + ey * ey);
                if (e < d) { d = e; prev = true; }
            }
            const bool prod = int(a) == G.prod_track;
            if (!prod && d > gate) continue;
            const float rx = std::fabs(G.cx - pj[a].x), ry = std::fabs(G.cy - pj[a].y);   // the old rule: raw projection inside the box (+50 % + 4)
            const bool beyond = rx > G.hw * 1.5f + 4.0f || ry > G.hh * 1.5f + 4.0f;
            float sc = d * d;
            if (tr.ident && same_colour(tr.rgb, s.rgb)) sc *= 0.25f;
            if (prod) sc = -1.0f;   // the producer matched this sprite to one of this contact's draws at frame time
            qs.push_back({sc, g, a, prod, beyond, prev});
        }
    }
    std::sort(qs.begin(), qs.end(), [](const Q& x, const Q& y) { return x.s < y.s || (x.s == y.s && (x.g < y.g || (x.g == y.g && x.a < y.a))); });
    std::vector<int> track_of_group(gr.size(), -1);
    for (const Q& q : qs) {
        if (track_of_group[q.g] >= 0 || tk.tracks[q.a].sprite >= 0) continue;
        const Grp& G = gr[q.g];
        Track& tr = tk.tracks[q.a];
        track_of_group[q.g] = int(q.a); tr.sprite = int(G.rep);
        tr.shw = G.hw; tr.shh = G.hh; tr.sglyphs = 0; for (int i = 0; i < 3; ++i) tr.slrgb[i] = 0.0f;
        for (unsigned m = 0; m < nm; ++m) if (g_of[m] == int(q.g) && f.mark[m].glyphs > tr.sglyphs) {
            tr.sglyphs = f.mark[m].glyphs; for (int i = 0; i < 3; ++i) tr.slrgb[i] = f.mark[m].lrgb[i];
        }
        st.sprites_matched += G.n;
        if (q.prod) st.producer_matched += G.n; else st.consumer_matched += G.n;
        if (q.beyond) st.beyond_raw_gate += G.n;
        if (q.prev) ++st.prev_closer;
        for (unsigned m = 0; m < nm; ++m) if (g_of[m] == int(q.g)) tk.sprite_track_id[m] = tr.id;
        // d (track part): the residual of this match teaches the track's own offset (bounded by seek)
        const float rx = G.cx - pj[q.a].x, ry = G.cy - pj[q.a].y;
        if (std::sqrt(rx * rx + ry * ry) <= p.seek) {
            if (tr.off_valid) { tr.off[0] = 0.7f * tr.off[0] + 0.3f * rx; tr.off[1] = 0.7f * tr.off[1] + 0.3f * ry; }
            else { tr.off[0] = rx; tr.off[1] = ry; tr.off_valid = true; }
        }
    }
    // 5. identity: refresh / confirm from the matched box; drop it when the game draws no box on a contact well inside its window
    const bool box_facts_safe = f.box_unreadable == 0 && f.mark_dropped == 0;
    if (!box_facts_safe) ++st.box_facts_unsafe;
    for (Track& tr : tk.tracks) {
        if (tr.sprite >= 0 || tr.ident) tr.t_boxed = t;   // NO-DOUBLES hold (prov_hold_s): the game boxed it now / holds its identity
        if (tr.sprite >= 0) {
            const MarkerSprite& s = f.mark[tr.sprite];
            tr.miss_n = 0; tr.miss_t0 = -1;
            if (!box_colour_ok(s.rgb)) { ++st.ident_pale; continue; }   // section 22: a whitish group is no contact box (no identity, no recolour)
            float htx, hty; box_half_tan(tr.shw, tr.shh, htx, hty);
            if (tr.ident && same_colour(tr.rgb, s.rgb)) {
                for (int i = 0; i < 3; ++i) tr.rgb[i] = s.rgb[i];
                tr.glyphs = tr.sglyphs; for (int i = 0; i < 3; ++i) tr.lrgb[i] = tr.slrgb[i];
                tr.htx = htx; tr.hty = hty; tr.t_ident = t; tr.pend_n = 0;
                continue;
            }
            if (tr.pend_n && same_colour(tr.pend_rgb, s.rgb)) ++tr.pend_n;
            else { tr.pend_n = 1; for (int i = 0; i < 3; ++i) tr.pend_rgb[i] = s.rgb[i]; }
            // section 17.2: an identified contact changes colour only after recolour_confirm consecutive matches (a box it crosses for a few frames
            // never recolours it and so never drops its cached pictures)
            const uint32_t need = tr.ident ? std::max(p.confirm, p.recolour_confirm) : p.confirm;
            if (tr.ident && tr.pend_n == p.confirm && need > p.confirm) ++st.recolour_held;
            if (tr.pend_n >= need) {
                if (tr.ident) ++st.ident_recolour; else ++st.ident_new;
                tr.ident = true; tr.ever_ident = true; for (int i = 0; i < 3; ++i) tr.rgb[i] = s.rgb[i];
                tr.team = acz::tac::team_of(tr.rgb);   // IFF: the game's own colour (blue = friendly)
                // w12-2 finding 4: the cached pictures show the PREVIOUS identity (colour): a confirmed new / changed identity forgets them (fail closed)
                tr.cell_ok = false; tr.cell2_ok = false;
                tr.glyphs = tr.sglyphs; for (int i = 0; i < 3; ++i) tr.lrgb[i] = tr.slrgb[i];
                tr.htx = htx; tr.hty = hty; tr.t_ident = t; tr.pend_n = 0;
            }
            continue;
        }
        tr.pend_n = 0;
        if (!tr.ident || !tr.in_inner || !box_facts_safe) { if (!tr.in_inner) { tr.miss_n = 0; tr.miss_t0 = -1; } continue; }
        if (tr.miss_n == 0) tr.miss_t0 = t;
        ++tr.miss_n;
        if (tr.miss_n >= p.miss_frames && t - tr.miss_t0 >= p.miss_s) { tr.ident = false; tr.miss_n = 0; tr.miss_t0 = -1; ++st.ident_cleared_in_view; }
    }
    // 6. gone: no world draw for longer than lost_s. IDENT-HOLD: an identified contact lost across a dropout (after its last sighting) leaves a ghost.
    const size_t before = tk.tracks.size();
    tk.tracks.erase(std::remove_if(tk.tracks.begin(), tk.tracks.end(), [&](const Track& tr) {
        const bool gone = t - tr.t_seen > p.lost_s;
        if (gone && tr.ident) ++st.lost_ident;
        if (gone && tr.ident && tr.rel_ok && tk.dropout_t > tr.t_seen) {
            if (tk.ghosts.size() >= kGhostCap) tk.ghosts.erase(tk.ghosts.begin());   // the oldest goes
            Ghost g; g.tr = tr; g.t_lost = t; tk.ghosts.push_back(g); ++st.ghosts;
        }
        // QUICK-FORMATION: a friend-latched contact lost is remembered at its offset from the game camera
        if (gone && tr.form_friend && !tr.ident && tr.rel_ok) {
            if (tk.fghosts.size() >= 4) tk.fghosts.erase(tk.fghosts.begin());
            Ghost g; g.tr = tr; g.t_lost = t; tk.fghosts.push_back(g); ++st.fghosts;
        }
        // WRECK: a game-boxed track lost while frames kept arriving (no dropout since its last sighting) = a kill candidate; its debris is born near it
        if (gone && tr.ever_ident && tr.vel_valid && tk.dropout_t <= tr.t_seen) {
            if (tk.deaths.size() >= 8) tk.deaths.erase(tk.deaths.begin());
            Death d; for (int i = 0; i < 3; ++i) { d.pos[i] = tr.pos[i]; d.vel[i] = tr.vel[i]; } d.t = tr.t_seen; tk.deaths.push_back(d);
        }
        return gone;
    }), tk.tracks.end());
    st.lost += before - tk.tracks.size();
    for (Track& tr : tk.tracks) if (tr.frames == 1 && !tr.heir) mark_heir(tk, tr, t, p);   // debris born the frame its aircraft was lost
    tk.deaths.erase(std::remove_if(tk.deaths.begin(), tk.deaths.end(), [&](const Death& d) { return t - d.t > p.heir_s; }), tk.deaths.end());
    tk.fghosts.erase(std::remove_if(tk.fghosts.begin(), tk.fghosts.end(), [&](const Ghost& g) { return t - g.t_lost > p.friend_ghost_s; }), tk.fghosts.end());
    // QUICK-FORMATION take-over: a contact born after the friend ghost's last sighting, unboxed, whose offset from the game camera matches the ghost's
    if (!tk.fghosts.empty() && cam_ok) {
        for (Track& tr : tk.tracks) {
            if (tr.ident || tr.form_friend || !tr.seen_now || !tr.rel_ok || tr.sprite >= 0) continue;
            for (size_t gi = 0; gi < tk.fghosts.size(); ++gi) {
                const Track& o = tk.fghosts[gi].tr;
                if (tr.t_first < o.t_seen) continue;
                const float dx = tr.rel[0] - o.rel[0], dy = tr.rel[1] - o.rel[1], dz = tr.rel[2] - o.rel[2];
                const float r = std::sqrt(o.rel[0] * o.rel[0] + o.rel[1] * o.rel[1] + o.rel[2] * o.rel[2]);
                if (std::isfinite(r) && std::sqrt(dx * dx + dy * dy + dz * dz) <= std::fmax(500.0f, 0.25f * r)) {
                    tr.form_friend = true; tr.form_s = p.form_cap; ++st.freacq; tk.fghosts.erase(tk.fghosts.begin() + long(gi)); break;
                }
            }
        }
    }
    tk.ghosts.erase(std::remove_if(tk.ghosts.begin(), tk.ghosts.end(), [&](const Ghost& g) { return t - g.t_lost > kGhostS; }), tk.ghosts.end());
    // 7. IDENT-HOLD take-over: a contact first seen after the ghost's last sighting, unidentified, not boxed this frame, whose offset from the game camera
    //    matches the ghost's: the same contact re-tracked after the dropout. Greedy nearest pairs; identity, colour, label and the cached pictures move.
    if (!tk.ghosts.empty() && cam_ok) {
        struct GP { float d; unsigned a, g; };
        std::vector<GP> gps;
        for (unsigned a = 0; a < tk.tracks.size(); ++a) {
            const Track& tr = tk.tracks[a];
            if (tr.ident || !tr.seen_now || tr.sprite >= 0 || !tr.rel_ok) continue;
            for (unsigned gi = 0; gi < tk.ghosts.size(); ++gi) {
                const Track& o = tk.ghosts[gi].tr;
                if (tr.t_first < o.t_seen) continue;   // existed before the ghost was last seen: another contact
                const float dx = tr.rel[0] - o.rel[0], dy = tr.rel[1] - o.rel[1], dz = tr.rel[2] - o.rel[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float r = std::sqrt(o.rel[0] * o.rel[0] + o.rel[1] * o.rel[1] + o.rel[2] * o.rel[2]);
                if (std::isfinite(d) && d <= std::fmax(kGhostGateMin, kGhostGateFrac * r)) gps.push_back({d, a, gi});
            }
        }
        std::sort(gps.begin(), gps.end(), [](const GP& x, const GP& y) { return x.d < y.d || (x.d == y.d && (x.a < y.a || (x.a == y.a && x.g < y.g))); });
        std::vector<uint8_t> a_used(tk.tracks.size(), 0), g_used(tk.ghosts.size(), 0);
        for (const GP& q : gps) {
            if (a_used[q.a] || g_used[q.g]) continue;
            a_used[q.a] = g_used[q.g] = 1;
            Track& tr = tk.tracks[q.a]; const Track& o = tk.ghosts[q.g].tr;
            tr.ident = true; tr.ever_ident = true; tr.t_ident = t; tr.pend_n = 0; tr.miss_n = 0; tr.miss_t0 = -1;
            for (int i = 0; i < 3; ++i) { tr.rgb[i] = o.rgb[i]; tr.lrgb[i] = o.lrgb[i]; }
            tr.glyphs = o.glyphs; tr.htx = o.htx; tr.hty = o.hty; tr.team = o.team; tr.form_s = o.form_s; tr.form_friend = o.form_friend;
            if (o.cell >= 0 && o.cell < int(kStampCells) && tk.cell_owner[o.cell] == o.id) {
                tk.cell_owner[o.cell] = tr.id; tr.cell = o.cell; tr.cell_ok = o.cell_ok; tr.c_ox = o.c_ox; tr.c_oy = o.c_oy; tr.c_pw = o.c_pw; tr.c_ph = o.c_ph;
                tr.c_q = o.c_q; tr.c_t = o.c_t; tr.c_u0 = o.c_u0; tr.c_v0 = o.c_v0;
                if (o.cell2 >= 0 && o.cell2 < int(kStampCells) && tk.cell_owner[o.cell2] == o.id) {
                    tk.cell_owner[o.cell2] = tr.id; tr.cell2 = o.cell2; tr.cell2_ok = o.cell2_ok; tr.c2_ox = o.c2_ox; tr.c2_oy = o.c2_oy; tr.c2_pw = o.c2_pw; tr.c2_ph = o.c2_ph;
                }
            }
            ++st.reacquired;
        }
        size_t k = 0;
        for (size_t gi = 0; gi < tk.ghosts.size(); ++gi) if (!g_used[gi]) tk.ghosts[k++] = tk.ghosts[gi];
        tk.ghosts.resize(k);
    }
}

// ---------------------------------------------------------------------------------------------- the boxes to draw this frame
struct FrameCounts { uint32_t sprite = 0, out = 0, unknown = 0, prov = 0, skipped_in_window = 0, prov_friend = 0, prov_enemy = 0, prov_unknown = 0; };
// PROV: a contact the game never boxed (no identity, nothing to stamp), seen in the newest frame, tracked prov_frames frames, within the game's box range
// (~31,000 u, section 17 (d)) and moving like an aircraft (not static, not a missile), gets one unlabeled TGT-green stroke box at its true position, in or
// outside the window. A contact the game boxed once and stopped boxing (kill / retarget) never does; a track the game boxes THIS frame (identity pending) is left to its pixels.
inline void collect_prov(Tracker& tk, const acz::tac::TacFrame& f, const float cam[3], const Params& p, std::vector<acz::tac::WorldMarker>& out, FrameCounts& fc) {
    using namespace acz::tac;
    if (!p.prov || !f.a_valid || (p.unknown && p.mode != 3)) return;   // mk360u:1 (diagnostic grey) owns the unidentified in modes 1/2
    // headset test 8 (section 21): candidates first (all the refusals), then one box per aircraft
    struct Cand { const Track* tr; int kind; float d, alpha; };
    std::vector<Cand> cand;
    const bool cam_fin = cam && std::isfinite(cam[0]) && std::isfinite(cam[1]) && std::isfinite(cam[2]);
    for (const Track& tr : tk.tracks) {
        if (!tr.seen_now || tr.ident || tr.frames < p.prov_frames || tr.sprite >= 0) continue;
        // a contact the game boxed once and stopped boxing: a kill / retarget (enemy: never), but a FRIEND the game stopped boxing is the "hit or miss" miss
        if (tr.ever_ident && tr.team != team_friend) continue;
        const float v = tr.speed();
        if (!(v >= p.prov_min_speed && v <= p.prov_max_speed)) continue;
        if (!cam_fin) { ++tk.st.prov_ref_bad; continue; }
        const float dx = tr.pos[0] - cam[0], dy = tr.pos[1] - cam[1], dz = tr.pos[2] - cam[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(d) && !(d <= p.prov_max_range)) continue;
        // COCKPIT GUARD: a stroke box has no depth test; a bad position, or a contact nearer than the cockpit's neighbourhood, is refused (never drawn in / on it)
        bool bad = !std::isfinite(d) || !(std::isfinite(tr.posf[0]) && std::isfinite(tr.posf[1]) && std::isfinite(tr.posf[2]));
        if (!bad && std::fabs(tr.pos[0]) < 1.0f && std::fabs(tr.pos[1]) < 1.0f && std::fabs(tr.pos[2]) < 1.0f) bad = true;   // a draw at the world origin
        if (!bad) { const float sx = tr.pos[0] - tr.posf[0], sy = tr.pos[1] - tr.posf[1], sz = tr.pos[2] - tr.posf[2];
                    if (!(std::sqrt(sx * sx + sy * sy + sz * sz) <= p.prov_slip)) bad = true; }                                   // stale / wrong interpolation
        if (bad) { ++tk.st.prov_ref_bad; continue; }
        float alpha = 1.0f;
        if (p.near_fade > 0.0f) {
            if (d <= p.near_fade) { ++tk.st.prov_ref_near; continue; }
            if (p.near_min > p.near_fade && d < p.near_min) alpha = (d - p.near_fade) / (p.near_min - p.near_fade);
        }
        if (tr.wreck) { ++tk.st.prov_ref_wreck; continue; }   // debris of a killed aircraft
        // NO-DOUBLES: the game boxed it a moment ago (blink / identity pending), or one of this frame's game box groups sits on its projection
        if (tk.last_t - tr.t_boxed < p.prov_hold_s) { ++tk.st.prov_ref_hold; continue; }
        {
            float xs = 0, ys = 0, w = 0;
            const bool win = project_ps2(f.A, tr.posf, xs, ys, w) && std::fabs(xs - kWinCx) <= kWinHx * 1.1f && std::fabs(ys - kWinCy) <= kWinHy * 1.1f;
            bool near_box = win && f.box_unreadable > 0;   // a box draw the producer could not read may be this contact's: unknown, so never doubled
            if (win && !near_box) {
                const bool go = tk.goff_n >= p.goff_min;
                const float ox = tr.off_valid ? tr.off[0] : go ? tk.goff[0] : 0.0f, oy = tr.off_valid ? tr.off[1] : go ? tk.goff[1] : 0.0f;
                for (unsigned g = 0; g < tk.ngroups && g < kMarkCap; ++g) {
                    const float gate = std::fmax(p.prov_box_gate, std::fmax(tk.ghw[g], tk.ghh[g]) * 1.5f + 4.0f);
                    if (std::hypot(tk.gcx[g] - (xs + ox), tk.gcy[g] - (ys + oy)) <= gate || std::hypot(tk.gcx[g] - xs, tk.gcy[g] - ys) <= gate) { near_box = true; break; }
                }
            }
            if (near_box) { ++tk.st.prov_ref_box; continue; }
        }
        // IFF (section 20): the game's colour (ever_ident) > formation friend > formation candidate / no camera velocity (unknown) > presumed enemy
        int kind;
        if (tr.ever_ident) kind = 0;
        else if (tr.form_friend) kind = 0;
        else if (tr.form_s > 0.0 || !tk.cam_v_ok) kind = 2;
        else kind = 1;
        if (kind == 2 && !p.prov_unknown) { ++tk.st.prov_unknown_skipped; continue; }
        cand.push_back({&tr, kind, d, alpha});
    }
    // DEDUP: one aircraft tracked as two contacts (its parts further apart than the cluster radius) gets one box: team priority (friend, enemy, unknown),
    // then the older track. A contact the game boxes (identified, seen now) beside a candidate takes its place.
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.kind < b.kind || (a.kind == b.kind && a.tr->id < b.tr->id); });
    const float cos_dup = std::cos(p.prov_dup_deg * 3.14159265f / 180.0f);
    auto same_aircraft = [&](const float a[3], const float b[3]) {
        const float ra[3] = {a[0] - cam[0], a[1] - cam[1], a[2] - cam[2]}, rb[3] = {b[0] - cam[0], b[1] - cam[1], b[2] - cam[2]};
        const float la = std::sqrt(ra[0] * ra[0] + ra[1] * ra[1] + ra[2] * ra[2]), lb = std::sqrt(rb[0] * rb[0] + rb[1] * rb[1] + rb[2] * rb[2]);
        const float sep = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        if (!(la > 1.0f && lb > 1.0f) || sep > p.prov_dup_dist) return false;
        return (ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2]) / (la * lb) >= cos_dup;
    };
    std::vector<const Track*> kept;
    for (const Cand& c : cand) {
        bool dup = false;
        for (const Track& o : tk.tracks) if (&o != c.tr && o.seen_now && (o.ident || o.sprite >= 0) && same_aircraft(c.tr->pos, o.pos)) { dup = true; break; }
        for (size_t i = 0; i < kept.size() && !dup; ++i) if (same_aircraft(c.tr->pos, kept[i]->pos)) dup = true;
        if (dup) { ++tk.st.prov_ref_dup; continue; }
        kept.push_back(c.tr);
        const float* rgb = c.kind == 0 ? p.prov_friend_rgb : c.kind == 1 ? p.prov_rgb : p.prov_unknown_rgb;
        WorldMarker w;
        for (int i = 0; i < 3; ++i) { w.pos[i] = c.tr->pos[i]; w.rgb[i] = rgb[i]; }
        w.id = c.tr->id; w.label = label_none; w.alpha = c.alpha;
        out.push_back(w); ++fc.prov;
        if (c.kind == 0) ++fc.prov_friend; else if (c.kind == 1) ++fc.prov_enemy; else ++fc.prov_unknown;
    }
}
// mode 1: every SUPPRESSED game sprite at its own ray (distance = its matched contact's, else far_units), plus every identified contact seen now that
//         is outside the game window and not drawn through a sprite; mode 2: only the latter. Never a contact the game does not report now.
inline void collect(Tracker& tk, const acz::tac::TacFrame& f, const float cam[3], const Params& p, std::vector<acz::tac::WorldMarker>& out, FrameCounts& fc) {
    using namespace acz::tac;
    out.clear(); fc = FrameCounts{};
    if (!p.mode) return;
    if (p.mode == 3) {   // NBOX (mk360:3) draws the game's own pixels (plan_stamps); only never-boxed moving contacts get a provisional box of ours
        collect_prov(tk, f, cam, p, out, fc); tk.st.drawn_prov += fc.prov; tk.st.prov_friend += fc.prov_friend; tk.st.prov_enemy += fc.prov_enemy; tk.st.prov_unknown += fc.prov_unknown;
        return;
    }
    const unsigned nm = std::min<unsigned>(f.nmark, kMarkCap);
    std::vector<uint8_t> via_sprite(tk.tracks.size(), 0);
    if (p.mode == 1 && f.a_valid) {
        float C[3];
        const bool cc = camera_centre(f.A, C);
        for (unsigned m = 0; m < nm; ++m) {
            const MarkerSprite& s = f.mark[m];
            if (!s.suppressed) continue;   // the game's own draw is replayed for this one (unread / not suppressed): never a second box
            // a box group whose members are all hidden is drawn ONCE (at its first member), at the group rectangle's centre and size
            const int g = m < kMarkCap && tk.sprite_group[m] >= 0 && unsigned(tk.sprite_group[m]) < tk.ngroups ? tk.sprite_group[m] : -1;
            const bool whole = g >= 0 && tk.gall_sup[g];
            if (whole) { bool first = true; for (unsigned k = 0; k < m; ++k) if (tk.sprite_group[k] == g) { first = false; break; } if (!first) continue; }
            const float cx = whole ? tk.gcx[g] : s.cx, cy = whole ? tk.gcy[g] : s.cy, hw = whole ? tk.ghw[g] : s.hw, hh = whole ? tk.ghh[g] : s.hh;
            uint8_t glyphs = s.glyphs; const float* lrgb = s.lrgb;
            if (whole) for (unsigned k = 0; k < nm; ++k) if (tk.sprite_group[k] == g && f.mark[k].glyphs > glyphs) { glyphs = f.mark[k].glyphs; lrgb = f.mark[k].lrgb; }
            float dir[3];
            if (!cc || !screen_ray(f.A, cx, cy, dir)) continue;
            float dist = p.far_units;
            uint32_t id = 0x80000000u | m;
            for (unsigned a = 0; a < tk.tracks.size(); ++a) if (tk.sprite_track_id[m] && tk.tracks[a].id == tk.sprite_track_id[m]) {
                const Track& tr = tk.tracks[a];
                const float dx = tr.pos[0] - C[0], dy = tr.pos[1] - C[1], dz = tr.pos[2] - C[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (std::isfinite(d) && d > 1.0f) dist = d;
                id = tr.id; via_sprite[a] = 1;
                break;
            }
            WorldMarker w;
            for (int i = 0; i < 3; ++i) { w.pos[i] = C[i] + dir[i] * dist; w.rgb[i] = s.rgb[i]; }
            w.label = label_of(glyphs, lrgb); w.id = id;
            box_half_tan(hw, hh, w.half_tan_x, w.half_tan_y);
            out.push_back(w); ++fc.sprite;
        }
    }
    for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        const Track& tr = tk.tracks[a];
        if (!tr.seen_now || via_sprite[a]) continue;
        if (tr.sprite >= 0 && !f.mark[tr.sprite].suppressed) continue;   // the game's own box for it is drawn this frame: never a second one
        if (tr.in_window) { if (tr.ident) ++fc.skipped_in_window; continue; }   // in the window only the game's boxes of THIS frame count
        if (!f.a_valid) continue;
        WorldMarker w;
        for (int i = 0; i < 3; ++i) w.pos[i] = tr.pos[i];
        w.id = tr.id;
        if (tr.ident) {
            if (p.box_range > 0.0f && tk.cam_ok) {   // section 22: not beyond the game's own box range
                const float dx = tr.pos[0] - tk.cam[0], dy = tr.pos[1] - tk.cam[1], dz = tr.pos[2] - tk.cam[2];
                if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= p.box_range)) { ++tk.st.nb_far; continue; }
            }
            for (int i = 0; i < 3; ++i) w.rgb[i] = tr.rgb[i];
            w.label = label_of(tr.glyphs, tr.lrgb); w.half_tan_x = tr.htx; w.half_tan_y = tr.hty;
            out.push_back(w); ++fc.out;
        } else if (p.unknown && tr.frames >= 10 && tr.speed() >= p.moving) {
            w.rgb[0] = w.rgb[1] = w.rgb[2] = 0.6f; w.label = label_none; w.coasting = true;   // dim grey, unlabeled: NOT the game's
            out.push_back(w); ++fc.unknown;
        }
    }
    collect_prov(tk, f, cam, p, out, fc);
    tk.st.drawn_prov += fc.prov; tk.st.prov_friend += fc.prov_friend; tk.st.prov_enemy += fc.prov_enemy; tk.st.prov_unknown += fc.prov_unknown;
    tk.st.drawn_sprite += fc.sprite; tk.st.drawn_out += fc.out; tk.st.drawn_unknown += fc.unknown; tk.st.skipped_in_window_unmatched += fc.skipped_in_window;
}

// ---------------------------------------------------------------------------------------------- NBOX (mk360:3): the game's own box + label pixels
// Headset test 2026-10-28 items 7/8 (screenshots 1 vs 2): the user prefers the GAME's boxes (its hexagons/brackets, its own TGT/APC/FRND text and
// colours) to our stroke boxes, everywhere around the head. The producer hides nothing (kProducerOn only): every box and label draw is replayed into the
// marker image exactly as without mk360. Per consumed frame (markers-360.md section 12):
//  * every box group (MATCH-2) is a STAMP: the marker-image pixel rectangle around its box and its label (+ margin), clipped to one atlas cell, overlaps
//    cut at the midline between the box centres (no pixel in two stamps). It is copied into an atlas cell, CLEARED in the flat marker quad and drawn per
//    eye at the ray of its box centre (at its contact's distance): the same pixels, world-anchored, exactly once;
//  * a contact matched to a complete (not window-clipped) stamp keeps that cell; outside the window, while identified and drawn by the game this frame
//    (its world draws), the cached stamp is drawn at its 3D position (+ its learned box offset): the game's last complete in-window look of it;
//  * nothing for a contact the game does not report now (the mk360:1 rules); pixels in no stamp (unread draws, other text) stay on the flat quad.
struct AtlasGeom { bool ok = false; uint32_t img_w = 0, img_h = 0, cell_w = 0, cell_h = 0, w = 0, h = 0; };
inline AtlasGeom atlas_geom(uint32_t img_w, uint32_t img_h, uint32_t bytes_per_px = 8) noexcept {
    AtlasGeom g;
    if (img_w < 64 || img_h < 64 || img_w > 8192 || img_h > 8192) return g;
    g.img_w = img_w; g.img_h = img_h;
    g.cell_w = uint32_t(std::ceil(2.0 * kStampHalfGsX * double(img_w) / (2.0 * kWinHx)));
    g.cell_h = uint32_t(std::ceil(2.0 * kStampHalfGsY * double(img_h) / (2.0 * kWinHy)));
    g.w = g.cell_w * kStampCols; g.h = g.cell_h * kStampRows;
    g.ok = g.w <= 16384 && g.h <= 16384 && uint64_t(g.w) * g.h * bytes_per_px <= kStampAtlasBudget;
    return g;
}
inline void cell_origin(const AtlasGeom& g, unsigned cell, uint32_t& x, uint32_t& y) noexcept { x = (cell % kStampCols) * g.cell_w; y = (cell / kStampCols) * g.cell_h; }
// GS <-> marker-image pixel: the image is the game window (GS 2048 +- 256 / 2048 +- 224 over its width / height, y down; J-hud-markers).
inline double px_of_gs_x(double gx, uint32_t w) noexcept { return (gx - double(kWinCx - kWinHx)) * double(w) / (2.0 * kWinHx); }
inline double px_of_gs_y(double gy, uint32_t h) noexcept { return (gy - double(kWinCy - kWinHy)) * double(h) / (2.0 * kWinHy); }
inline double gs_of_px_x(double px, uint32_t w) noexcept { return px * (2.0 * kWinHx) / double(w) + double(kWinCx - kWinHx); }
inline double gs_of_px_y(double py, uint32_t h) noexcept { return py * (2.0 * kWinHy) / double(h) + double(kWinCy - kWinHy); }

struct StampCopy { uint16_t cell = 0; uint32_t x = 0, y = 0, w = 0, h = 0; };   // marker-image pixels [x, x+w) x [y, y+h) -> the cell's top-left
struct StampClear { uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };                 // marker-image pixels (half-open): made transparent in the flat quad
// ox/oy = the stamp's top-left pixel corner relative to the anchor direction (GS, y down); pw/ph = its pixel size; live = this frame's game pixels
// sr/su (axes = true): the world directions of the game screen's right / up at the anchor (live: that frame's A at the box centre; cached: this frame's
// camera carried to the contact): the stamp is drawn ROTATED in the eye to follow them (aircraft, camera and head roll), still a rectangle (no shear).
// second = a satellite piece in the contact's second cell (same anchor and distance as its box).
// u0/v0 = the picture's texel offset inside its cell (0 for a live stamp; a cached q-1 picture shows only its boxes: section 17.1 LABEL-CORE).
struct StampDraw { float pos[3] = {}; float ox = 0, oy = 0; uint16_t cell = 0, pw = 0, ph = 0; uint32_t id = 0; bool live = false;
                   float sr[3] = {}, su[3] = {}; bool axes = false, second = false; uint16_t u0 = 0, v0 = 0; };
// One consumed frame's stamps: `draw` for both eyes (a DECOUPLE re-composite draws it again from the atlas), `copy` + `clear` only for the recording
// of the fresh frame (fresh = true).
struct StampPlan { AtlasGeom atlas; float near_min = 0, near_ref = 0, near_floor = 1, near_fade = 0; float ref[3] = {}; bool has_ref = false;   // ref = the game camera centre (the common origin of both eyes' near policy)
                    std::vector<StampDraw> draw; std::vector<StampCopy> copy; std::vector<StampClear> clear; bool fresh = false; };
struct StampCounts { uint32_t live = 0, cached = 0, cuts = 0, no_cell = 0, empty = 0, sat = 0, second = 0; };

// After update() of the same frame. img_w/img_h = the marker image (the frame size); live_ok = that image is used by this composite (its box pixels can
// be copied); t = the frame time (cell LRU).
inline void plan_stamps(Tracker& tk, const acz::tac::TacFrame& f, const Params& p, uint32_t img_w, uint32_t img_h, bool live_ok, double t,
                        StampPlan& plan, StampCounts& sc) {
    using namespace acz::tac;
    plan.draw.clear(); plan.copy.clear(); plan.clear.clear(); plan.fresh = true; sc = StampCounts{};
    plan.near_min = p.near_min; plan.near_ref = p.near_ref; plan.near_floor = p.near_floor; plan.near_fade = p.near_fade;
    plan.has_ref = f.a_valid && camera_centre(f.A, plan.ref);
    if (p.mode != 3) return;
    Stats& st = tk.st;
    ++st.nb_frames;
    if (!live_ok || !img_w || !img_h) { live_ok = false; ++st.nb_no_image; }
    // the cut rectangles are pixels of one image size: a new size forgets every cell
    if (live_ok && (img_w != tk.img_w || img_h != tk.img_h)) {
        for (unsigned c = 0; c < kStampCells; ++c) tk.cell_owner[c] = 0;
        for (Track& tr : tk.tracks) { tr.cell = -1; tr.cell_ok = false; tr.cell2 = -1; tr.cell2_ok = false; }
        for (Ghost& gh : tk.ghosts) { gh.tr.cell = -1; gh.tr.cell_ok = false; gh.tr.cell2 = -1; gh.tr.cell2_ok = false; }
        tk.img_w = img_w; tk.img_h = img_h;
    }
    plan.atlas = atlas_geom(tk.img_w, tk.img_h);
    if (!plan.atlas.ok) { if (tk.img_w) ++st.nb_no_atlas; return; }
    const AtlasGeom& g = plan.atlas;
    // last frame's temporary cells and the cells of vanished tracks are free again; a track pointing at a cell it does not own forgets it
    for (unsigned c = 0; c < kStampCells; ++c) {
        const uint32_t o = tk.cell_owner[c];
        if (!o) continue;
        if (o & kTempOwner) { tk.cell_owner[c] = 0; continue; }
        bool alive = false;
        for (const Track& tr : tk.tracks) if (tr.id == o && (tr.cell == int(c) || tr.cell2 == int(c))) { alive = true; break; }
        for (const Ghost& gh : tk.ghosts) if (gh.tr.id == o && (gh.tr.cell == int(c) || gh.tr.cell2 == int(c))) { alive = true; break; }   // IDENT-HOLD
        if (!alive) tk.cell_owner[c] = 0;
    }
    for (Track& tr : tk.tracks) {
        if (tr.cell >= 0 && (tr.cell >= int(kStampCells) || tk.cell_owner[tr.cell] != tr.id)) { tr.cell = -1; tr.cell_ok = false; }
        if (tr.cell2 >= 0 && (tr.cell2 >= int(kStampCells) || tk.cell_owner[tr.cell2] != tr.id)) { tr.cell2 = -1; tr.cell2_ok = false; }
    }
    bool used[kStampCells] = {};
    auto release2 = [&](Track& tr) { if (tr.cell2 >= 0 && tr.cell2 < int(kStampCells) && tk.cell_owner[tr.cell2] == tr.id) tk.cell_owner[tr.cell2] = 0; tr.cell2 = -1; tr.cell2_ok = false; };
    auto alloc = [&](uint32_t owner) -> int {
        for (unsigned c = 0; c < kStampCells; ++c) if (!tk.cell_owner[c]) { tk.cell_owner[c] = owner; tk.cell_t[c] = t; used[c] = true; return int(c); }
        int lru = -1;   // the least recently drawn contact cell not used this frame (a contact out of view the longest)
        for (unsigned c = 0; c < kStampCells; ++c)
            if (!used[c] && !(tk.cell_owner[c] & kTempOwner) && (lru < 0 || tk.cell_t[c] < tk.cell_t[lru])) lru = int(c);
        if (lru < 0) return -1;
        for (Track& tr : tk.tracks) {
            if (tr.cell2 == lru) { tr.cell2 = -1; tr.cell2_ok = false; }
            if (tr.cell == lru) { tr.cell = -1; tr.cell_ok = false; if (tr.cell2 >= 0 && !used[tr.cell2]) release2(tr); }
        }
        ++st.nb_evicted;
        tk.cell_owner[lru] = owner; tk.cell_t[lru] = t; used[lru] = true;
        return lru;
    };
    const bool goff_ok = tk.goff_n >= p.goff_min;
    std::vector<uint8_t> via_live(tk.tracks.size(), 0);
    // 1. live stamps: this frame's box groups (MATCH-2) with their labels; a contact's satellites (lock diamond / reticle) join its stamp
    float C[3];
    if (live_ok && f.a_valid && camera_centre(f.A, C)) {
        const int W = int(tk.img_w), H = int(tk.img_h);
        const unsigned nm = std::min<unsigned>(f.nmark, kMarkCap);
        const unsigned ng = std::min<unsigned>(tk.ngroups, kMarkCap);
        struct R { float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                   void add(const R& o) { x0 = std::fmin(x0, o.x0); y0 = std::fmin(y0, o.y0); x1 = std::fmax(x1, o.x1); y1 = std::fmax(y1, o.y1); }
                   bool valid() const { return x1 >= x0 && y1 >= y0; } };
        struct G { bool ok = false; R box, lab; float rgb[3] = {}; uint32_t id = 0; int host = -1; };
        std::vector<G> gs(ng);
        for (unsigned gi = 0; gi < ng; ++gi) {
            G& q = gs[gi];
            const float cx = tk.gcx[gi], cy = tk.gcy[gi];
            q.box.x0 = cx - tk.ghw[gi]; q.box.x1 = cx + tk.ghw[gi]; q.box.y0 = cy - tk.ghh[gi]; q.box.y1 = cy + tk.ghh[gi];
            q.ok = std::isfinite(q.box.x0) && std::isfinite(q.box.x1) && std::isfinite(q.box.y0) && std::isfinite(q.box.y1);
            bool col = false;
            for (unsigned m = 0; m < nm; ++m) {
                if (tk.sprite_group[m] != int(gi)) continue;
                if (tk.sprite_track_id[m]) q.id = tk.sprite_track_id[m];
                const MarkerSprite& s = f.mark[m];
                if (!col) { for (int i = 0; i < 3; ++i) q.rgb[i] = s.rgb[i]; col = true; }
                if (s.glyphs && s.lx1 >= s.lx0 && s.ly1 >= s.ly0 && std::isfinite(s.lx0) && std::isfinite(s.lx1) && std::isfinite(s.ly0) && std::isfinite(s.ly1)) {
                    R l; l.x0 = s.lx0; l.x1 = s.lx1; l.y0 = s.ly0; l.y1 = s.ly1; q.lab.add(l);
                }
            }
        }
        // satellites: an unmatched same-colour group near a matched one, onto which no other seen contact projects (that would be ITS box)
        for (unsigned gi = 0; gi < ng; ++gi) {
            G& q = gs[gi];
            if (!q.ok || q.id) continue;
            int best = -1; float bd = 1e30f;
            for (unsigned hi = 0; hi < ng; ++hi) {
                const G& h = gs[hi];
                if (hi == gi || !h.ok || !h.id || !same_colour(h.rgb, q.rgb)) continue;
                const float dx = std::fabs(tk.gcx[gi] - tk.gcx[hi]), dy = std::fabs(tk.gcy[gi] - tk.gcy[hi]);
                if (dx > kSatReachGsX || dy > kSatReachGsY) continue;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d < bd) { bd = d; best = int(hi); }
            }
            if (best < 0) continue;
            bool other = false;
            const float gate = std::fmax(p.gate_min, std::fmax(tk.ghw[gi], tk.ghh[gi]) * 1.5f + 4.0f);
            for (const Track& tr : tk.tracks) {
                if (tk.gdia[gi]) break;   // LOCK-SHAPE: a lock diamond is never another contact's box (it is no box at all)
                if (!tr.seen_now || tr.id == gs[best].id) continue;
                float xs, ys, w;
                if (!project_ps2(f.A, tr.posf, xs, ys, w)) continue;
                xs += tr.off_valid ? tr.off[0] : goff_ok ? tk.goff[0] : 0.0f; ys += tr.off_valid ? tr.off[1] : goff_ok ? tk.goff[1] : 0.0f;
                if (std::hypot(xs - tk.gcx[gi], ys - tk.gcy[gi]) <= gate) { other = true; break; }
            }
            if (other) continue;
            q.host = best; ++sc.sat;
        }
        const float Wg = 2.0f * kStampHalfGsX, Hg = 2.0f * kStampHalfGsY, Mg = kStampMarginGs;
        auto inside = [&](const R& b) {   // the game clips its box at the window edge: only a box >= 1 GS inside is a complete picture
            return b.x0 >= kWinCx - kWinHx + 1.0f && b.x1 <= kWinCx + kWinHx - 1.0f && b.y0 >= kWinCy - kWinHy + 1.0f && b.y1 <= kWinCy + kWinHy - 1.0f;
        };
        // one cell window (Wg x Hg GS) per stamp: it must hold the core (boxes); preferred centred on the anchor box, shifted to hold all of the target
        // (core + labels) when that fits; the stamp = target inside the window
        auto choose = [](float pref, float c0, float c1, float t0, float t1, float span) {
            float lo = c1 - span, hi = c0;
            if (lo > hi) lo = hi = 0.5f * (c0 + c1) - 0.5f * span;
            else if (t1 - t0 <= span) { lo = std::fmax(lo, t1 - span); hi = std::fmin(hi, t0); }
            return std::fmin(hi, std::fmax(lo, pref));
        };
        // lab_whole (LABEL-WHOLE): every label of the stamp lies inside the game window (>= 1 GS: the game clips its text there) and inside the stamp
        // k* = the core's pixel rectangle inside the stamp (boxes + margin, no label): a cached q-1 picture shows only that part (LABEL-CORE, section 17.1);
        // bgs / lgs = the box and label rectangles (GS) for the mk360-stamp diagnostic
        // CUT-CLEAN (headset test 9, section 22): la* = the labels' pixels, p* = the PICTURE = boxes (+ margin, the k* core) and labels, before any cut;
        // a cut stamp caches the part of it no other stamp's picture touches (box + whole label, else the box alone)
        struct L { int x0, y0, x1, y1; double bx, by; unsigned g, ag; uint32_t id; bool complete, cut, lab_whole; int prim; int kx0, ky0, kx1, ky1;
                   float bgs[4], lgs[4]; bool has_lab; int lax0, lay0, lax1, lay1, px0, py0, px1, py1; };
        std::vector<L> ls;
        auto push = [&](const R& core_in, const R& target_in, const R& labels, unsigned own, unsigned anchor, uint32_t id, bool complete, int prim) {
            R core = core_in, target = target_in;
            core.x0 -= Mg; core.x1 += Mg; core.y0 -= Mg; core.y1 += Mg;
            target.x0 -= Mg; target.x1 += Mg; target.y0 -= Mg; target.y1 += Mg;
            const float wx = choose(tk.gcx[own] - kStampHalfGsX, core.x0, core.x1, target.x0, target.x1, Wg);
            const float wy = choose(tk.gcy[own] - kStampHalfGsY, core.y0, core.y1, target.y0, target.y1, Hg);
            const float x0 = std::fmax(target.x0, wx), x1 = std::fmin(target.x1, wx + Wg), y0 = std::fmax(target.y0, wy), y1 = std::fmin(target.y1, wy + Hg);
            L l;
            l.bx = px_of_gs_x(tk.gcx[own], tk.img_w); l.by = px_of_gs_y(tk.gcy[own], tk.img_h);
            l.x0 = std::max(0, int(std::floor(px_of_gs_x(x0, tk.img_w)))); l.x1 = std::min(W, int(std::ceil(px_of_gs_x(x1, tk.img_w))));
            l.y0 = std::max(0, int(std::floor(px_of_gs_y(y0, tk.img_h)))); l.y1 = std::min(H, int(std::ceil(px_of_gs_y(y1, tk.img_h))));
            // rounding outward may exceed the cell by a pixel: trim the side farther from the box centre (the margin, never the box)
            if (l.x1 - l.x0 > int(g.cell_w)) { if (l.bx - l.x0 > l.x1 - l.bx) l.x0 = l.x1 - int(g.cell_w); else l.x1 = l.x0 + int(g.cell_w); }
            if (l.y1 - l.y0 > int(g.cell_h)) { if (l.by - l.y0 > l.y1 - l.by) l.y0 = l.y1 - int(g.cell_h); else l.y1 = l.y0 + int(g.cell_h); }
            l.g = own; l.ag = anchor; l.id = id; l.complete = complete; l.cut = false; l.prim = prim;
            l.lab_whole = !labels.valid() || (inside(labels) && labels.x0 >= x0 && labels.x1 <= x1 && labels.y0 >= y0 && labels.y1 <= y1);
            l.kx0 = std::max(l.x0, int(std::floor(px_of_gs_x(core.x0, tk.img_w)))); l.kx1 = std::min(l.x1, int(std::ceil(px_of_gs_x(core.x1, tk.img_w))));
            l.ky0 = std::max(l.y0, int(std::floor(px_of_gs_y(core.y0, tk.img_h)))); l.ky1 = std::min(l.y1, int(std::ceil(px_of_gs_y(core.y1, tk.img_h))));
            l.bgs[0] = core_in.x0; l.bgs[1] = core_in.y0; l.bgs[2] = core_in.x1; l.bgs[3] = core_in.y1;
            l.has_lab = labels.valid();
            l.lgs[0] = labels.x0; l.lgs[1] = labels.y0; l.lgs[2] = labels.x1; l.lgs[3] = labels.y1;
            l.lax0 = l.lay0 = l.lax1 = l.lay1 = 0;
            l.px0 = l.kx0; l.py0 = l.ky0; l.px1 = l.kx1; l.py1 = l.ky1;
            if (l.has_lab) {
                l.lax0 = std::max(0, int(std::floor(px_of_gs_x(labels.x0, tk.img_w)))); l.lax1 = std::min(W, int(std::ceil(px_of_gs_x(labels.x1, tk.img_w))));
                l.lay0 = std::max(0, int(std::floor(px_of_gs_y(labels.y0, tk.img_h)))); l.lay1 = std::min(H, int(std::ceil(px_of_gs_y(labels.y1, tk.img_h))));
                l.px0 = std::min(l.px0, l.lax0); l.py0 = std::min(l.py0, l.lay0); l.px1 = std::max(l.px1, l.lax1); l.py1 = std::max(l.py1, l.lay1);
            }
            ls.push_back(l);
        };
        for (unsigned gi = 0; gi < ng; ++gi) {
            const G& q = gs[gi];
            if (!q.ok || q.host >= 0) continue;
            R core = q.box, target = q.box; if (q.lab.valid()) target.add(q.lab);
            R core_all = core, target_all = target, lab_all = q.lab; bool complete_all = inside(q.box), any_sat = false;
            for (unsigned si = 0; si < ng; ++si) {
                if (gs[si].host != int(gi)) continue;
                any_sat = true;
                core_all.add(gs[si].box); target_all.add(gs[si].box); if (gs[si].lab.valid()) { target_all.add(gs[si].lab); lab_all.add(gs[si].lab); }
                complete_all = complete_all && inside(gs[si].box);
            }
            const bool fits = core_all.x1 - core_all.x0 + 2.0f * Mg <= Wg && core_all.y1 - core_all.y0 + 2.0f * Mg <= Hg;
            if (!any_sat || fits) { push(core_all, target_all, lab_all, gi, gi, q.id, complete_all, -1); continue; }
            // the satellites do not fit the box's cell window: the box keeps its own stamp, each satellite a SECOND stamp of the same contact
            const int prim = int(ls.size());
            push(core, target, q.lab, gi, gi, q.id, inside(q.box), -1);
            for (unsigned si = 0; si < ng; ++si) {
                if (gs[si].host != int(gi)) continue;
                R sc_ = gs[si].box, st_ = gs[si].box; if (gs[si].lab.valid()) st_.add(gs[si].lab);
                push(sc_, st_, gs[si].lab, si, gi, q.id, inside(gs[si].box), prim);
                // never a pixel in both: the second stamp gives up its overlap with the box's stamp along the axis that separates their centres
                L& a = ls[prim]; L& b = ls.back();
                if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1) {
                    const double dx = b.bx - a.bx, dy = b.by - a.by;
                    if (std::fabs(dy) >= std::fabs(dx)) { if (dy > 0) b.y0 = std::max(b.y0, a.y1); else b.y1 = std::min(b.y1, a.y0); }
                    else { if (dx > 0) b.x0 = std::max(b.x0, a.x1); else b.x1 = std::min(b.x1, a.x0); }
                }
            }
        }
        // overlapping stamps of DIFFERENT contacts (boxes close together): cut both at the integer midline between their box centres along the axis that
        // separates them more; cutting only shrinks, so every pair stays disjoint afterwards (no pixel drawn twice). A contact's own pieces are disjoint
        // by construction and never cut against each other.
        for (size_t i = 0; i < ls.size(); ++i) for (size_t j = i + 1; j < ls.size(); ++j) {
            L& a = ls[i]; L& b = ls[j];
            if (a.id && a.id == b.id) continue;
            if (!(a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1)) continue;
            ++sc.cuts; a.cut = b.cut = true;
            const double dx = b.bx - a.bx, dy = b.by - a.by;
            // CUT-CLEAN (section 22): when the two pictures (box + label, P) do not overlap along that axis (only their margins do), the cut runs in the
            // gap between them, so each stamp keeps its whole picture; otherwise the midline as before
            if (std::fabs(dx) >= std::fabs(dy) && std::fabs(dx) > 0.5) {
                L& lo = dx > 0 ? a : b; L& hi = dx > 0 ? b : a;
                const int cut = lo.px1 <= hi.px0 ? (lo.px1 + hi.px0) / 2 : int(std::lround(0.5 * (a.bx + b.bx)));
                lo.x1 = std::min(lo.x1, cut); hi.x0 = std::max(hi.x0, cut);
            } else if (std::fabs(dy) > 0.5) {
                L& lo = dy > 0 ? a : b; L& hi = dy > 0 ? b : a;
                const int cut = lo.py1 <= hi.py0 ? (lo.py1 + hi.py0) / 2 : int(std::lround(0.5 * (a.by + b.by)));
                lo.y1 = std::min(lo.y1, cut); hi.y0 = std::max(hi.y0, cut);
            } else b.x1 = b.x0;   // the same centre: one stamp only (the other's pixels are inside it)
        }
        // CUT-CLEAN (section 22): the part of a cut stamp that is still this contact's alone. Headset test 9 (t 125-170 s): two enemies a few GS apart in
        // the window cut each other's stamps on every frame, so neither picture was ever cached and outside the window only the one once seen alone had a
        // box ("outside the rectangle it only renders one"). A rectangle is clean when it lies inside the cut stamp and touches no other stamp's picture
        // (another contact's box + margin / label, or an unmatched group). cut_quality: 2 (box + whole label), 1 (box only, LABEL-CORE) or 0.
        auto clean = [&](size_t li, int x0, int y0, int x1, int y1) {
            const L& l = ls[li];
            if (x1 - x0 < 2 || y1 - y0 < 2 || x0 < l.x0 || y0 < l.y0 || x1 > l.x1 || y1 > l.y1) return false;
            for (size_t j = 0; j < ls.size(); ++j) {
                const L& o = ls[j];
                if (j == li || (l.id && o.id == l.id)) continue;   // the contact's own pieces are its pixels
                if (x0 < o.px1 && o.px0 < x1 && y0 < o.py1 && o.py0 < y1) return false;
            }
            return true;
        };
        auto cut_quality = [&](size_t li, int r[4]) -> uint8_t {
            const L& l = ls[li];
            const int kx0 = std::max(l.kx0, l.x0), kx1 = std::min(l.kx1, l.x1), ky0 = std::max(l.ky0, l.y0), ky1 = std::min(l.ky1, l.y1);
            if (l.has_lab && l.lab_whole && clean(li, l.px0, l.py0, l.px1, l.py1)) { r[0] = l.px0; r[1] = l.py0; r[2] = l.px1; r[3] = l.py1; return 2; }
            if (l.kx0 == kx0 && l.kx1 == kx1 && l.ky0 == ky0 && l.ky1 == ky1 && clean(li, kx0, ky0, kx1, ky1)) {   // the whole core survived the cut
                r[0] = kx0; r[1] = ky0; r[2] = kx1; r[3] = ky1; return l.has_lab ? 1 : 2;   // no label at all: the box IS the whole picture
            }
            return 0;
        };
        std::vector<uint8_t> prim_cached(ls.size(), 0), sec_cached(tk.tracks.size(), 0);
        for (size_t li = 0; li < ls.size(); ++li) {
            const L& l = ls[li];
            float dir[3];
            if (l.x1 - l.x0 < 2 || l.y1 - l.y0 < 2 || !screen_ray(f.A, tk.gcx[l.ag], tk.gcy[l.ag], dir)) { ++sc.empty; continue; }   // left to the flat quad
            int ti = -1;
            if (l.id) for (unsigned a = 0; a < tk.tracks.size(); ++a) if (tk.tracks[a].id == l.id) { ti = int(a); break; }
            // w12-2 finding 2: a cut picture holds only part of its label / box (a neighbour contact's stamp took the rest): the cut rectangle itself is
            // drawn live but never cached, so no truncated picture is ever shown outside the window; since headset test 9 its CLEAN part is (CUT-CLEAN
            // above: q 2 box + whole label, q 1 box only). LABEL-WHOLE: a whole box with a clipped label is q 1 (cached only without a whole picture;
            // replaces one only after kStampRecacheClippedS)
            int sub[4] = {0, 0, 0, 0};
            const bool cut_sub = l.complete && l.cut && l.prim < 0;
            const uint8_t q = (l.complete && !l.cut) ? (l.lab_whole ? 2 : 1) : cut_sub ? cut_quality(li, sub) : 0;
            if (q == 1 && !cut_sub) ++st.nb_label_clipped;
            int cell = -1; bool to_track = false;
            if (ti >= 0 && l.prim < 0) {
                Track& tr = tk.tracks[ti];
                const bool stale = tr.cell_ok && q >= 1 && t - tr.c_t > kStampRecacheClippedS;   // only matters for q 1 over a whole picture
                // section 17.2: a box of ANOTHER colour than the contact's identity (a box it crosses, before recolour_confirm) is drawn live but never
                // replaces the contact's cached picture
                const bool colour_ok = box_colour_ok(gs[l.g].rgb) && (!tr.ident || same_colour(tr.rgb, gs[l.g].rgb));   // section 22: never a whitish picture
                if (!colour_ok) {}
                else if (!tr.cell_ok || q >= tr.c_q || stale) {   // never a clipped / cut picture over a better cached one of the contact
                    if (tr.cell >= 0) { cell = tr.cell; used[cell] = true; tk.cell_t[cell] = t; }
                    else if ((cell = alloc(tr.id)) >= 0) tr.cell = cell;
                    to_track = cell >= 0;
                } else if (q == 0) ++st.nb_edge_kept; else ++st.nb_cut_kept;
            } else if (ti >= 0 && prim_cached[size_t(l.prim)] == 1) {   // a satellite piece of the picture being cached: the contact's second cell
                Track& tr = tk.tracks[ti];
                // w12-2 finding 3: one second cell per contact and plan: a further satellite piece (its cell already written by this plan) gets a temporary cell
                if (tr.cell2 >= 0 && used[tr.cell2]) cell = -1;
                else if (tr.cell2 >= 0) { cell = tr.cell2; used[cell] = true; tk.cell_t[cell] = t; }
                else if ((cell = alloc(tr.id)) >= 0) tr.cell2 = cell;
                to_track = cell >= 0;
            }
            if (cell < 0) cell = alloc(kTempOwner | l.g);
            if (cell < 0) { ++sc.no_cell; continue; }   // no cell: not cleared, the flat quad keeps the game's pixels
            StampCopy cp; cp.cell = uint16_t(cell); cp.x = uint32_t(l.x0); cp.y = uint32_t(l.y0); cp.w = uint32_t(l.x1 - l.x0); cp.h = uint32_t(l.y1 - l.y0);
            plan.copy.push_back(cp);
            StampClear cl; cl.x0 = uint32_t(l.x0); cl.y0 = uint32_t(l.y0); cl.x1 = uint32_t(l.x1); cl.y1 = uint32_t(l.y1);
            plan.clear.push_back(cl);
            // BOX-LOCK (markers-360.md section 15): a contact's picture is anchored ON the contact: the world point = its DISPLAY-time centroid (where
            // the eyes see the model with interp:1), the pixel offsets measured from its FRAME-time projection (the game drew the box there, with this
            // A). Before: the anchor was the matched group's frame-time ray at the contact's range (the box trailed the model by the target's own motion
            // per display horizon), and the group may be a PIECE of the box (two pieces per box in every step133/134 log) about which the near scale
            // shrank the picture off the target. An unmatched group keeps its own ray at far_units.
            float ax = tk.gcx[l.ag], ay = tk.gcy[l.ag];
            StampDraw d;
            bool on_contact = false;
            if (ti >= 0) {
                const Track& tr = tk.tracks[ti];
                float xs, ys, w;
                if (project_ps2(f.A, tr.posf, xs, ys, w) && std::isfinite(tr.pos[0]) && std::isfinite(tr.pos[1]) && std::isfinite(tr.pos[2])) {
                    ax = xs; ay = ys; on_contact = true;
                    for (int i = 0; i < 3; ++i) d.pos[i] = tr.pos[i];
                }
                via_live[ti] = 1;
            }
            if (!on_contact) {
                // FLASH (section 17.3): a box group matched to no contact was drawn at far_units (32,500 u) for that frame while the frames around it
                // drew it at its contact's range: a one-frame depth pop of the whole picture (step143: live 25526 stamps vs ~23,700 matched groups).
                // A seen contact projecting within twice the match gate of the group lends its range (the group's own ray is kept).
                float dist = p.far_units, best = 1e30f;
                const float reach = 2.0f * std::fmax(p.gate_min, std::fmax(tk.ghw[l.ag], tk.ghh[l.ag]) * 1.5f + 4.0f);
                for (const Track& tr : tk.tracks) {
                    if (!tr.seen_now) continue;
                    float xs, ys, w;
                    if (!project_ps2(f.A, tr.posf, xs, ys, w)) continue;
                    const float e = std::hypot(xs - tk.gcx[l.ag], ys - tk.gcy[l.ag]);
                    const float dd = std::sqrt((tr.pos[0] - C[0]) * (tr.pos[0] - C[0]) + (tr.pos[1] - C[1]) * (tr.pos[1] - C[1]) + (tr.pos[2] - C[2]) * (tr.pos[2] - C[2]));
                    if (e <= reach && e < best && std::isfinite(dd) && dd > 1.0f) { best = e; dist = dd; }
                }
                if (best < 1e29f) ++st.nb_depth_held;
                for (int i = 0; i < 3; ++i) d.pos[i] = C[i] + dir[i] * dist;
            }
            d.ox = float(gs_of_px_x(l.x0, tk.img_w) - double(ax)); d.oy = float(gs_of_px_y(l.y0, tk.img_h) - double(ay));
            d.cell = uint16_t(cell); d.pw = uint16_t(cp.w); d.ph = uint16_t(cp.h); d.id = l.id ? l.id : (kTempOwner | l.g); d.live = true;
            d.axes = screen_axes(f.A, ax, ay, d.sr, d.su); d.second = l.prim >= 0;
            plan.draw.push_back(d); ++sc.live;
            if (d.second) ++sc.second;
            // mk360-stamp (section 17.1): the period's sample = the last label-clipped live stamp, else the last live stamp with a label
            if (l.has_lab && l.prim < 0 && (q == 1 || !tk.sf.valid || tk.sf.q != 1)) {
                StampFacts& s = tk.sf;
                s.valid = true; s.id = l.id; s.q = q; s.lab_whole = l.lab_whole; s.cut = l.cut; s.complete = l.complete; s.t = t;
                s.glyphs = 0; for (unsigned m = 0; m < std::min<unsigned>(f.nmark, kMarkCap); ++m) if (tk.sprite_group[m] == int(l.g) && f.mark[m].glyphs > s.glyphs) s.glyphs = f.mark[m].glyphs;
                for (int i = 0; i < 4; ++i) { s.box[i] = l.bgs[i]; s.lab[i] = l.lgs[i]; }
                s.px[0] = l.x0; s.px[1] = l.y0; s.px[2] = l.x1; s.px[3] = l.y1; s.box_q = f.box_q; s.label_q = f.label_q;
            }
            if (to_track) {
                Track& tr = tk.tracks[ti];
                if (l.prim < 0) {
                    tr.cell_ok = q >= 1; tr.c_q = q;
                    // a CUT-CLEAN picture takes no satellite piece along (its second-cell stamp was cut too; the old second cell is released below)
                    if (q >= 1) { tr.c_ox = d.ox; tr.c_oy = d.oy; tr.c_pw = d.pw; tr.c_ph = d.ph; tr.c_t = t; tr.c_u0 = tr.c_v0 = 0; prim_cached[li] = cut_sub ? 2 : 1; }
                    // LABEL-CORE (section 17.1): a q-1 picture (its label clipped by the game window or the cell) is cached as its BOXES only (the core
                    // sub-rectangle of the same cell): outside the window it never shows half a label; the next whole picture replaces it.
                    const int kx0 = std::max(l.kx0, l.x0), kx1 = std::min(l.kx1, l.x1), ky0 = std::max(l.ky0, l.y0), ky1 = std::min(l.ky1, l.y1);
                    if (cut_sub && q >= 1) {   // CUT-CLEAN (section 22): only the clean sub-rectangle of the cut stamp's cell
                        tr.c_u0 = uint16_t(sub[0] - l.x0); tr.c_v0 = uint16_t(sub[1] - l.y0); tr.c_pw = uint16_t(sub[2] - sub[0]); tr.c_ph = uint16_t(sub[3] - sub[1]);
                        tr.c_ox = float(gs_of_px_x(sub[0], tk.img_w) - double(ax)); tr.c_oy = float(gs_of_px_y(sub[1], tk.img_h) - double(ay));
                        ++st.nb_cut_cached;
                    } else if (q == 1 && kx1 - kx0 >= 2 && ky1 - ky0 >= 2) {
                        tr.c_u0 = uint16_t(kx0 - l.x0); tr.c_v0 = uint16_t(ky0 - l.y0); tr.c_pw = uint16_t(kx1 - kx0); tr.c_ph = uint16_t(ky1 - ky0);
                        tr.c_ox = float(gs_of_px_x(kx0, tk.img_w) - double(ax)); tr.c_oy = float(gs_of_px_y(ky0, tk.img_h) - double(ay));
                        ++st.nb_core_cached;
                    }
                } else { tr.cell2_ok = true; tr.c2_ox = d.ox; tr.c2_oy = d.oy; tr.c2_pw = d.pw; tr.c2_ph = d.ph; sec_cached[size_t(ti)] = 1; }
            }
        }
        // a new cached picture without a satellite piece: the old second cell belongs to an older picture (never paired with this one)
        for (size_t li = 0; li < ls.size(); ++li) {
            if (!prim_cached[li]) continue;
            for (unsigned a = 0; a < tk.tracks.size(); ++a) if (tk.tracks[a].id == ls[li].id && !sec_cached[a]) release2(tk.tracks[a]);
        }
    }
    // 2. cached stamps: identified contacts the game draws this frame OUTSIDE its window, with a complete stamp of their own (+ its satellite piece)
    float Cc[3];
    const bool cc_ok = f.a_valid && camera_centre(f.A, Cc);
    if (f.a_valid) for (unsigned a = 0; a < tk.tracks.size(); ++a) {
        Track& tr = tk.tracks[a];
        if (!tr.seen_now || via_live[a] || !tr.ident || tr.in_window || tr.sprite >= 0) continue;   // in the window: only this frame's game boxes count
        if (!tr.cell_ok || tr.cell < 0 || tk.cell_owner[tr.cell] != tr.id || tr.c_pw < 2 || tr.c_ph < 2) { ++st.nb_out_uncached; continue; }
        if (p.box_range > 0.0f) {   // section 22: beyond the game's own box range the game reports no box (the identity is kept for a return)
            const float* o = cc_ok ? Cc : tk.cam;
            const float dx = tr.pos[0] - o[0], dy = tr.pos[1] - o[1], dz = tr.pos[2] - o[2];
            if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= p.box_range)) { ++st.nb_far; continue; }
        }
        // BOX-LOCK: the cached offsets are measured from the contact's own frame-time projection (plan above): drawn about the contact as they are,
        // no learned screen offset added (that one held a box PIECE's offset plus, with interp:1, the display lag; applied in the screen axes of a
        // later attitude it moved the box off the target, inverted by twice the lag)
        StampDraw d;
        for (int i = 0; i < 3; ++i) d.pos[i] = tr.pos[i];
        if (cc_ok) {   // this frame's camera axes carried to the contact (rolls with the aircraft like the game's HUD boxes)
            float dc[3] = {tr.pos[0] - Cc[0], tr.pos[1] - Cc[1], tr.pos[2] - Cc[2]};
            const float l = std::sqrt(dc[0] * dc[0] + dc[1] * dc[1] + dc[2] * dc[2]);
            if (l > 1e-3f && std::isfinite(l)) { for (float& v : dc) v /= l; d.axes = contact_axes(f.A, dc, d.sr, d.su); }
        }
        d.ox = tr.c_ox; d.oy = tr.c_oy; d.cell = uint16_t(tr.cell); d.pw = tr.c_pw; d.ph = tr.c_ph; d.id = tr.id; d.live = false; d.u0 = tr.c_u0; d.v0 = tr.c_v0;
        used[tr.cell] = true; tk.cell_t[tr.cell] = t;
        plan.draw.push_back(d); ++sc.cached;
        if (tr.cell2_ok && tr.cell2 >= 0 && tk.cell_owner[tr.cell2] == tr.id && tr.c2_pw >= 2 && tr.c2_ph >= 2) {
            StampDraw d2 = d;
            d2.u0 = d2.v0 = 0;
            d2.ox = tr.c2_ox; d2.oy = tr.c2_oy; d2.cell = uint16_t(tr.cell2); d2.pw = tr.c2_pw; d2.ph = tr.c2_ph; d2.second = true;
            used[tr.cell2] = true; tk.cell_t[tr.cell2] = t;
            plan.draw.push_back(d2); ++sc.cached; ++sc.second;
        }
    }
    st.nb_sat += sc.sat; st.nb_second += sc.second;
    st.nb_live += sc.live; st.nb_cached += sc.cached; st.nb_copies += plan.copy.size(); st.nb_cuts += sc.cuts; st.nb_no_cell += sc.no_cell; st.nb_empty += sc.empty;
}
// The backend did not record this fresh plan's copies (refused job, no atlas): the cells it would have written hold older pixels -> their contacts
// forget them (no stale or foreign stamp is ever drawn outside the window).
inline void stamps_not_copied(Tracker& tk, const StampPlan& plan) noexcept {
    for (const StampCopy& c : plan.copy) {
        if (c.cell >= kStampCells) continue;
        const uint32_t o = tk.cell_owner[c.cell];
        if (!o || (o & kTempOwner)) continue;
        for (Track& tr : tk.tracks) {
            if (tr.id != o) continue;
            if (tr.cell == int(c.cell)) { if (tr.cell_ok) ++tk.st.nb_invalidated; tr.cell_ok = false; }
            if (tr.cell2 == int(c.cell)) tr.cell2_ok = false;
        }
    }
}
// The fresh frame whose plan this is was REJECTED after its GPU completion (the producer rewrote its marker image between the consumer's pre-check and
// its reads: xr_stereo_output.cpp fresh_rewritten_post). Its copies were recorded and executed, so the cells they wrote hold the rewritten producer's
// pixels, not the rejected frame's: those contacts forget their cells (no later accepted frame ever samples them) and the plan keeps nothing to draw
// (no re-composite of a rejected frame). Cells this plan did not write keep their earlier, accepted stamps. Codex J-203 finding 2. A plan that is not
// fresh (a re-composite: no copies) changes nothing.
inline void stamps_rejected(Tracker& tk, StampPlan& plan) noexcept {
    if (plan.fresh) {
        stamps_not_copied(tk, plan);
        ++tk.st.nb_rejected;
    }
    plan.draw.clear(); plan.copy.clear(); plan.clear.clear(); plan.fresh = false;
}
// The eye's clip of a world point (tactical.cpp clip_of: clip = M (p - eye, 1)); false behind the eye.
inline bool eye_clip_(const acz::tac::EyeView& e, const double p[3], double c[4]) noexcept {
    const double q[3] = {p[0] - e.eye[0], p[1] - e.eye[1], p[2] - e.eye[2]};
    const float* m = e.clip;
    c[0] = m[0] * q[0] + m[1] * q[1] + m[2] * q[2] + m[3];
    c[1] = m[4] * q[0] + m[5] * q[1] + m[6] * q[2] + m[7];
    c[2] = 0.0;
    c[3] = m[12] * q[0] + m[13] * q[1] + m[14] * q[2] + m[15];
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[3]) && c[3] > 1e-4;
}
// The eye's NDC per tan along its own image x and y, measured on its optical axis (clip row w = the image-plane normal), where the projection is
// conformal: the metric that tells a rotation from a shear in that eye's NDC (asymmetric / non-square eye fields).
inline bool eye_ndc_per_tan(const acz::tac::EyeView& e, double& kx, double& ky) noexcept {
    double n[3] = {e.clip[12], e.clip[13], e.clip[14]};
    if (!unit3_(n)) return false;
    const double ax[3] = {std::fabs(n[1]) < 0.9 ? 0.0 : 1.0, std::fabs(n[1]) < 0.9 ? 1.0 : 0.0, 0.0};
    double pv[3] = {n[1] * ax[2] - n[2] * ax[1], n[2] * ax[0] - n[0] * ax[2], n[0] * ax[1] - n[1] * ax[0]};
    if (!unit3_(pv)) return false;
    const double qv[3] = {n[1] * pv[2] - n[2] * pv[1], n[2] * pv[0] - n[0] * pv[2], n[0] * pv[1] - n[1] * pv[0]};
    const double st = 0.01;
    double p0[3], p1[3], p2[3], c0[4], c1[4], c2[4];
    for (int k = 0; k < 3; ++k) { p0[k] = e.eye[k] + n[k]; p1[k] = p0[k] + pv[k] * st; p2[k] = p0[k] + qv[k] * st; }
    if (!eye_clip_(e, p0, c0) || !eye_clip_(e, p1, c1) || !eye_clip_(e, p2, c2)) return false;
    const double j00 = (c1[0] / c1[3] - c0[0] / c0[3]) / st, j01 = (c2[0] / c2[3] - c0[0] / c0[3]) / st;
    const double j10 = (c1[1] / c1[3] - c0[1] / c0[3]) / st, j11 = (c2[1] / c2[3] - c0[1] / c0[3]) / st;
    kx = std::sqrt(j00 * j00 + j01 * j01); ky = std::sqrt(j10 * j10 + j11 * j11);
    return std::isfinite(kx) && std::isfinite(ky) && kx > 1e-9 && ky > 1e-9;
}
// ROLL (headset test 2026-10-29 item 3b): a stamp's frame in one eye. (cx, cy) = the anchor's NDC; M (row-major 2x2) maps a tan offset along the GAME
// screen (x right, y up) to an NDC offset: the local Jacobian of the world axes sr/su at the anchor, reduced to a ROTATION times the per-axis local
// scales in the eye's conformal metric. So the stamp follows the game screen's roll as the eye sees it (aircraft roll, the camera delta of a re-composite,
// head roll), stays a rectangle (no shear: the D-383 box-warp fix) and keeps the earlier size; with no roll M = diag(sx, sy) of marker_screen_frame.
inline bool stamp_frame(const acz::tac::EyeView& e, const float pos[3], const float sr[3], const float su[3], double kx, double ky, double& cx, double& cy,
                        double M[4]) noexcept {
    const double P[3] = {pos[0], pos[1], pos[2]};
    double c[4];
    if (!eye_clip_(e, P, c)) return false;   // behind the eye: not drawn
    const double d = std::sqrt((P[0] - e.eye[0]) * (P[0] - e.eye[0]) + (P[1] - e.eye[1]) * (P[1] - e.eye[1]) + (P[2] - e.eye[2]) * (P[2] - e.eye[2]));
    if (!(d > 1e-6) || !std::isfinite(d)) return false;
    const double st = 0.01;
    double pa[3], pb[3], a[4], b[4];
    for (int k = 0; k < 3; ++k) { pa[k] = P[k] + sr[k] * d * st; pb[k] = P[k] + su[k] * d * st; }
    if (!eye_clip_(e, pa, a) || !eye_clip_(e, pb, b)) return false;
    cx = c[0] / c[3]; cy = c[1] / c[3];
    const double jr[2] = {(a[0] / a[3] - cx) / st, (a[1] / a[3] - cy) / st}, ju[2] = {(b[0] / b[3] - cx) / st, (b[1] / b[3] - cy) / st};
    const double A0 = jr[0] / kx, C0 = jr[1] / ky, B0 = ju[0] / kx, D0 = ju[1] / ky;   // iso Jacobian [A0 B0; C0 D0]
    const double det = A0 * D0 - B0 * C0;
    const double th = det >= 0 ? std::atan2(C0 - B0, A0 + D0) : std::atan2(C0, A0);
    const double lx = std::sqrt(A0 * A0 + C0 * C0), ly = std::sqrt(B0 * B0 + D0 * D0), cs = std::cos(th), sn = std::sin(th);
    M[0] = kx * cs * lx; M[1] = -kx * sn * ly; M[2] = ky * sn * lx; M[3] = ky * cs * ly;
    return std::isfinite(cx) && std::isfinite(cy) && std::isfinite(M[0]) && std::isfinite(M[1]) && std::isfinite(M[2]) && std::isfinite(M[3]) && lx > 0 && ly > 0;
}
// Near-target policy of one stamp: `pos` is pushed out along the ray from the game camera (plan.ref; else this eye) to plan.near_min (units; 0 = off) and the returned scale
// clamp(d / near_ref, near_floor, 1) (d = the eye's true distance; near_ref 0 = 1) multiplies the stamp's offsets about its anchor.
// NEAR-FADE: *fade = the stamp's opacity from its TRUE distance d (before the push-out): 1 at d >= near_min, (d - near_fade) / (near_min - near_fade)
// between, 0 at d <= near_fade (near_fade 0 = always 1; near_fade >= near_min = 0 below near_fade, else 1). The policy is colour-blind: a friendly's
// stamp is treated exactly like an enemy's.
inline double near_fade_of(double d, const StampPlan& plan) noexcept {
    const double lo = plan.near_fade, hi = plan.near_min;
    if (!(lo > 0.0) || !std::isfinite(d)) return 1.0;
    if (d <= lo) return 0.0;
    if (!(hi > lo) || d >= hi) return 1.0;
    return (d - lo) / (hi - lo);
}
inline double stamp_near_policy(const float eye[3], float pos[3], const StampPlan& plan, double* fade = nullptr) noexcept {
    // The common origin of both eyes (the game camera centre) when known: both eyes then move the stamp to the SAME point (the parallax shrinks); with only
    // the eye, each eye would slide along its own ray and the disparity would stay.
    const float* o = plan.has_ref ? plan.ref : eye;
    const double v[3] = {double(pos[0]) - o[0], double(pos[1]) - o[1], double(pos[2]) - o[2]};
    const double d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (fade) *fade = std::isfinite(d) ? near_fade_of(d, plan) : 1.0;
    if (!(d > 1e-6) || !std::isfinite(d)) return 1.0;
    double sc = 1.0;
    if (plan.near_ref > 0.0f) sc = std::fmin(1.0, std::fmax(double(plan.near_floor), d / double(plan.near_ref)));
    if (plan.near_min > 0.0f && d < double(plan.near_min)) { const double k = double(plan.near_min) / d; for (int i = 0; i < 3; ++i) pos[i] = float(o[i] + v[i] * k); }
    return sc;
}
// BOX-LOCK (markers-360.md section 15, cause B3): the near policy's common origin = the centre of THIS XR frame's two eyes. plan_stamps set it to the
// game camera of the producer frame (A's centre), but the eyes are at the display-time camera (interp:1 moves it 10-15 u per frame, summary-interp
// cam-shift-mean) plus the head: pushing a 60 u stamp out to 150 u along the game camera's ray moved it off the target by o x (1/d - 1/min) (7 deg for
// 12 u). Called for every composite (fresh and DECOUPLE re-composite) before build_stamps; an eye without a finite position leaves the plan's origin.
inline void set_view_centre(StampPlan& plan, const acz::tac::EyeView ev[2]) noexcept {
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = 0.5f * (ev[0].eye[i] + ev[1].eye[i]);
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) return;
    for (int i = 0; i < 3; ++i) plan.ref[i] = c[i];
    plan.has_ref = true;
}
// OFF-VIEW: the largest |NDC| of a stamp anchor that is still drawn (1 = the eye's image edge). A stamp is at most a few hundredths of the view wide on
// axis, so a box centred beyond this margin has nothing visible, and inside it the off-axis magnification stays bounded (<= ~2.5x for a 90 deg field).
inline constexpr double kStampAnchorNdcMax = 1.25;
// One eye's stamp quads (slot eye0/1_stamp): a rectangle turned with the game screen's axes at the anchor (StampDraw sr/su; stamp_frame), one GS unit =
// kTanPerGsX/Y (the flat quad's own pixel scale: the game's look). A draw without axes (none recoverable) falls back to the camera axes of the EyeView
// (e.right/up). The quad spans the first to the last texel CENTRE and the uv the same centres, so linear filtering never reads outside the stamp
// (neighbour cell, stale texels). Returns the stamps drawn. An eye without its whole frame (right/fwd unset) draws NO stamp (Codex J-203 finding 1).
// NEAR-FADE: the vertex col.z carries the stamp's opacity (xr_tactical.hlsl ps_stamp multiplies the premultiplied texel by it); a stamp at or below
// near_fade emits nothing. st (optional, one eye per composite): counts faded / hidden stamps.
inline unsigned build_stamps(acz::tac::Overlay& o, const acz::tac::EyeView& e, const StampPlan& plan, Stats* st = nullptr) {
    using namespace acz::tac;
    const AtlasGeom& g = plan.atlas;
    if (!g.ok || !has_eye_frame(e)) return 0;
    double kx, ky;
    if (!eye_ndc_per_tan(e, kx, ky)) return 0;
    const double gpx = 2.0 * kWinHx / double(g.img_w), gpy = 2.0 * kWinHy / double(g.img_h);   // GS units per marker-image pixel
    unsigned n = 0;
    for (const StampDraw& d : plan.draw) {
        if (d.pw < 2 || d.ph < 2 || d.cell >= kStampCells) continue;
        if (o.v.size() + 6 > kOverlayCap) break;
        double cx, cy, M[4];
        float pos[3] = {d.pos[0], d.pos[1], d.pos[2]};
        double fade = 1.0;
        const double sc = stamp_near_policy(e.eye, pos, plan, &fade);   // never inside the canopy, smaller when close (the whole group about its anchor)
        if (!(fade > 0.0)) { if (st) ++st->nb_near_hidden; continue; }   // NEAR-FADE: a contact at the canopy draws no box (the blue flashes)
        if (fade < 1.0 && st) ++st->nb_near_faded;
        if (!stamp_frame(e, pos, d.axes ? d.sr : e.right, d.axes ? d.su : e.up, kx, ky, cx, cy, M)) continue;
        // OFF-VIEW (testhmd-first-flights.md section 5): an anchor outside this eye's view draws nothing. eye_clip_ only refuses points behind the eye, and
        // the stamp's local Jacobian grows as 1/cos^2 of the off-axis angle: a friendly box 80-89 deg off the view axis (a fast head turn, DECOUPLE
        // re-composites mid-slew) became a quad several NDC wide whose magnified outline washed the whole eye blue/green (step185 runAGC1 f5494,
        // mirror m0040; runAGB2 m0169). Boxes with the anchor just past the edge (<= kStampAnchorNdcMax) still draw their visible part.
        if (!(std::fabs(cx) <= kStampAnchorNdcMax && std::fabs(cy) <= kStampAnchorNdcMax)) { if (st) ++st->nb_off_view; continue; }
        const double gx[2] = {sc * (d.ox + 0.5 * gpx), sc * (d.ox + (d.pw - 0.5) * gpx)}, gy[2] = {sc * (d.oy + 0.5 * gpy), sc * (d.oy + (d.ph - 0.5) * gpy)};
        uint32_t ux, uy; cell_origin(g, d.cell, ux, uy);
        if (uint32_t(d.u0) + d.pw > g.cell_w || uint32_t(d.v0) + d.ph > g.cell_h) continue;   // never sample outside the cell
        ux += d.u0; uy += d.v0;
        const float u[2] = {float((ux + 0.5) / g.w), float((ux + d.pw - 0.5) / g.w)}, v[2] = {float((uy + 0.5) / g.h), float((uy + d.ph - 0.5) / g.h)};
        float X[2][2], Y[2][2];   // [i: x corner][j: y corner]
        bool fin = true;
        for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
            const double tx = gx[i] * kTanPerGsX, ty = -gy[j] * kTanPerGsY;   // tan offsets on the game screen (y up)
            X[i][j] = float(cx + M[0] * tx + M[1] * ty); Y[i][j] = float(cy + M[2] * tx + M[3] * ty);
            fin = fin && std::isfinite(X[i][j]) && std::isfinite(Y[i][j]);
        }
        if (!fin) continue;
        const int idx[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
        for (const auto& c : idx) {
            Vertex vt;
            vt.pos[0] = X[c[0]][c[1]]; vt.pos[1] = Y[c[0]][c[1]]; vt.pos[2] = 0.0f; vt.pos[3] = 1.0f;
            vt.col[0] = u[c[0]]; vt.col[1] = v[c[1]]; vt.col[2] = float(fade); vt.col[3] = 0.0f;
            o.v.push_back(vt);
        }
        ++n;
    }
    return n;
}
// Why the backend did not draw this recording's stamps (the "refused" counter of summary-mk360-nbox-backend). step111 live: 32% of the recordings were
// counted refused, all of them cached-only frames whose boxes were outside both eye views (build_stamps emits no vertex, the overlay is empty, job.tac
// is null): nothing to draw, not a failure. Only copy_failed (invalidated > 0), no_atlas and pipeline are defects.
enum class NboxRefuse : unsigned { none = 0, nothing_in_view = 1, no_atlas = 2, copy_failed = 3, pipeline = 4, kCount = 5 };
inline NboxRefuse classify_nbox_refusal(bool stamp_used, bool atlas_ok, bool overlay_nonempty, bool fresh_copies, bool copied) {
    if (stamp_used) return NboxRefuse::none;
    if (!atlas_ok) return NboxRefuse::no_atlas;
    if (fresh_copies && !copied) return NboxRefuse::copy_failed;
    if (!overlay_nonempty) return NboxRefuse::nothing_in_view;
    return NboxRefuse::pipeline;
}
inline std::string nbox_line(const Tracker& tk, const StampPlan& plan, int64_t now) {
    const Stats& s = tk.st;
    unsigned cells = 0, track_cells = 0;
    for (unsigned c = 0; c < kStampCells; ++c) if (tk.cell_owner[c]) { ++cells; if (!(tk.cell_owner[c] & kTempOwner)) ++track_cells; }
    char u[640];
    std::snprintf(u, sizeof u, "xr-stereo;event=summary-mk360-nbox;t-ms=%lld;frames=%llu;live=%llu;cached=%llu;copies=%llu;cuts=%llu;no-cell=%llu;empty=%llu;"
        "no-image=%llu;evicted=%llu;edge-kept=%llu;invalidated=%llu;no-atlas=%llu;rejected=%llu;sat=%llu;second=%llu;cut-kept=%llu;cells=%u(contacts:%u)/%u;atlas=%ux%u(cell:%ux%u,img:%ux%u);last=%zu(copies:%zu);"
        "label-clipped=%llu;out-uncached=%llu;ghosts=%llu(reacquired:%llu,held:%zu);near=faded:%llu,hidden:%llu",
        (long long)now, (unsigned long long)s.nb_frames, (unsigned long long)s.nb_live, (unsigned long long)s.nb_cached, (unsigned long long)s.nb_copies,
        (unsigned long long)s.nb_cuts, (unsigned long long)s.nb_no_cell, (unsigned long long)s.nb_empty, (unsigned long long)s.nb_no_image,
        (unsigned long long)s.nb_evicted, (unsigned long long)s.nb_edge_kept, (unsigned long long)s.nb_invalidated, (unsigned long long)s.nb_no_atlas,
        (unsigned long long)s.nb_rejected, (unsigned long long)s.nb_sat, (unsigned long long)s.nb_second, (unsigned long long)s.nb_cut_kept,
        cells, track_cells, kStampCells, plan.atlas.w, plan.atlas.h, plan.atlas.cell_w, plan.atlas.cell_h, plan.atlas.img_w, plan.atlas.img_h,
        plan.draw.size(), plan.copy.size(), (unsigned long long)s.nb_label_clipped, (unsigned long long)s.nb_out_uncached, (unsigned long long)s.ghosts,
        (unsigned long long)s.reacquired, tk.ghosts.size(), (unsigned long long)s.nb_near_faded, (unsigned long long)s.nb_near_hidden);
    return u;
}

// headset test 6 (markers-360.md section 17): its own line (summary-mk360-nbox is already ~420 of the 480-byte log line cap)
inline std::string hs6_line(const Tracker& tk, int64_t now) {
    const Stats& s = tk.st;
    char u[320];
    // headset test 9 (section 22): pale = matched whitish groups (no identity), far = cached pictures not drawn beyond box_range, cut-cached = clean parts
    // of cut stamps cached, unsafe = frames whose box facts disabled the in-window miss rule (same count as summary-mk360 box-facts-unsafe, which the cap cuts)
    std::snprintf(u, sizeof u, "xr-stereo;event=summary-mk360-hs6;t-ms=%lld;pairs=%llu;recolour-held=%llu;depth-held=%llu;core-cached=%llu;lock-shapes=%llu;"
        "pale=%llu;far=%llu;cut-cached=%llu;cleared-in-view=%llu;unsafe=%llu;off-view=%llu",
        (long long)now, (unsigned long long)s.pairs, (unsigned long long)s.recolour_held, (unsigned long long)s.nb_depth_held,
        (unsigned long long)s.nb_core_cached, (unsigned long long)s.lock_shapes, (unsigned long long)s.ident_pale, (unsigned long long)s.nb_far,
        (unsigned long long)s.nb_cut_cached, (unsigned long long)s.ident_cleared_in_view, (unsigned long long)s.box_facts_unsafe,
        (unsigned long long)s.nb_off_view);
    return u;
}
// IFF for provisional boxes (section 20): its own line (the other summaries are at the log line cap). prov-* = boxes drawn (cumulative frames x contacts);
// unknown-skipped with mk360pu:0; formation-confirmed = latches set; now = the tracks not boxed by the game this frame by team; cam-v = camera velocity valid.
inline std::string iff_line(const Tracker& tk, const Params& p, int64_t now) {
    const Stats& s = tk.st;
    unsigned fr = 0, en = 0, un = 0;
    for (const Track& tr : tk.tracks) {
        if (!tr.seen_now || tr.ident) continue;
        if (tr.ever_ident) { if (tr.team == acz::tac::team_friend) ++fr; continue; }
        if (tr.form_friend) ++fr; else if (tr.form_s > 0.0 || !tk.cam_v_ok) ++un; else ++en;
    }
    char u[400];
    std::snprintf(u, sizeof u, "xr-stereo;event=summary-mk360-iff;t-ms=%lld;prov-friend=%llu;prov-enemy=%llu;prov-unknown=%llu;unknown-policy=%s;unknown-skipped=%llu;"
        "formation-confirmed=%llu;now=friend:%u,enemy:%u,unknown:%u;cam-v=%s;"
        "refused=box:%llu,hold:%llu,near:%llu,bad:%llu,dup:%llu,wreck:%llu;quick=%llu,fghost:%llu,reacq:%llu,wrecks:%llu", (long long)now, (unsigned long long)s.prov_friend,
        (unsigned long long)s.prov_enemy, (unsigned long long)s.prov_unknown, p.prov_unknown ? "neutral" : "skip", (unsigned long long)s.prov_unknown_skipped,
        (unsigned long long)s.form_confirmed, fr, en, un, tk.cam_v_ok ? "ok" : "none",
        (unsigned long long)s.prov_ref_box, (unsigned long long)s.prov_ref_hold, (unsigned long long)s.prov_ref_near, (unsigned long long)s.prov_ref_bad,
        (unsigned long long)s.prov_ref_dup, (unsigned long long)s.prov_ref_wreck, (unsigned long long)s.form_quick, (unsigned long long)s.fghosts,
        (unsigned long long)s.freacq, (unsigned long long)s.wrecks);
    return u;
}
// mk360-stamp (section 17.1): the period's sampled live stamp with a label (then forgotten: one sample per period). none = no such stamp this period.
inline std::string stamp_line(Tracker& tk, int64_t now) {
    StampFacts& s = tk.sf;
    char u[480];
    if (!s.valid) { std::snprintf(u, sizeof u, "xr-stereo;event=mk360-stamp;t-ms=%lld;none", (long long)now); return u; }
    std::snprintf(u, sizeof u, "xr-stereo;event=mk360-stamp;t-ms=%lld;id=%u;q=%u;label-whole=%u;cut=%u;complete=%u;glyphs=%u;box=%.1f,%.1f,%.1f,%.1f;"
        "label=%.1f,%.1f,%.1f,%.1f;px=%d,%d,%d,%d;box-q=%.5g,%.5g(off:%u/%u);label-q=%.5g,%.5g(off:%u/%u)",
        (long long)now, s.id, unsigned(s.q), s.lab_whole ? 1u : 0u, s.cut ? 1u : 0u, s.complete ? 1u : 0u, unsigned(s.glyphs),
        double(s.box[0]), double(s.box[1]), double(s.box[2]), double(s.box[3]), double(s.lab[0]), double(s.lab[1]), double(s.lab[2]), double(s.lab[3]),
        s.px[0], s.px[1], s.px[2], s.px[3], double(s.box_q.lo), double(s.box_q.hi), s.box_q.off, s.box_q.n,
        double(s.label_q.lo), double(s.label_q.hi), s.label_q.off, s.label_q.n);
    s = StampFacts{};
    return u;
}

// ---------------------------------------------------------------------------------------------- logs (xr-stereo.log, every summary period)
inline std::string summary_line(const Tracker& tk, const acz::tac::Options& o, int64_t now) {
    const Stats& s = tk.st;
    unsigned ident = 0, seen = 0, inwin = 0, out_ident = 0;
    for (const Track& tr : tk.tracks) {
        if (tr.ident) ++ident;
        if (tr.seen_now) ++seen;
        if (tr.in_window) ++inwin;
        if (tr.ident && tr.seen_now && !tr.in_window) ++out_ident;
    }
    const double n = s.frames ? double(s.frames) : 1.0;
    char t[1100];
    std::snprintf(t, sizeof t, "xr-stereo;event=summary-mk360;t-ms=%lld;mode=%u;unknown=%u;frames=%llu;no-a=%llu;tracks=%zu(seen:%u,identified:%u,in-window:%u,"
        "identified-outside:%u);sprites=%llu(suppressed:%llu,matched:%llu,producer:%llu,consumer:%llu);sprite-to-nearest-contact-gs=lt8:%llu,lt16:%llu,lt32:%llu,"
        "lt64:%llu,ge64:%llu,none:%llu;identity=new:%llu,recolour:%llu,cleared-in-view:%llu;lost=%llu(identified:%llu);created=%llu;self-draws=%llu;"
        "box-facts-unsafe=%llu;drawn=sprite:%llu,outside:%llu,unknown:%llu,prov:%llu;avg-drawn=%.2f;in-window-identified-without-box=%llu;game-boxes=%s",
        (long long)now, unsigned(o.mk360), o.mk360_unknown ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.no_a, tk.tracks.size(), seen, ident, inwin,
        out_ident, (unsigned long long)s.sprites, (unsigned long long)s.sprites_suppressed, (unsigned long long)s.sprites_matched,
        (unsigned long long)s.producer_matched, (unsigned long long)s.consumer_matched, (unsigned long long)s.near_lt8, (unsigned long long)s.near_lt16,
        (unsigned long long)s.near_lt32, (unsigned long long)s.near_lt64, (unsigned long long)s.near_ge64, (unsigned long long)s.near_none,
        (unsigned long long)s.ident_new, (unsigned long long)s.ident_recolour, (unsigned long long)s.ident_cleared_in_view, (unsigned long long)s.lost,
        (unsigned long long)s.lost_ident, (unsigned long long)s.created, (unsigned long long)s.self_draws, (unsigned long long)s.box_facts_unsafe,
        (unsigned long long)s.drawn_sprite, (unsigned long long)s.drawn_out, (unsigned long long)s.drawn_unknown, (unsigned long long)s.drawn_prov,
        double(s.drawn_sprite + s.drawn_out + s.drawn_unknown + s.drawn_prov) / n, (unsigned long long)s.skipped_in_window_unmatched,
        o.mk360 == 1 ? "hidden(read-draws)-redrawn-at-rays" : o.mk360 == 3 ? "native-stamps" : "kept-in-window");
    return t;
}
// MATCH-2 diagnostics, its own line (feature_log line_cap = 480 B: summary-mk360 is already cut after avg-drawn in live run S1): box groups, sprite half
// sizes, the mean signed residual sprite - nearest projection (GS, x right / y down), the learned global offset, matches the old box-relative gate
// would have refused, matches where the previous-frame position was closer, and the two tail fields summary-mk360 loses to the cap.
inline std::string match_line(const Tracker& tk, int64_t now) {
    const Stats& s = tk.st;
    const double rn = s.res_n ? double(s.res_n) : 1.0;
    char u[480];
    std::snprintf(u, sizeof u, "xr-stereo;event=summary-mk360-match;t-ms=%lld;sprites=%llu;matched=%llu;groups=%llu(multi:%llu);"
        "sprite-half-gs=lt2:%llu,lt4:%llu,lt8:%llu,ge8:%llu;nearest-mean-gs=%.1f,%.1f(n:%llu);offset-gs=%.1f,%.1f(n:%u);matched-beyond-raw-gate=%llu;"
        "prev-frame-closer=%llu;in-window-identified-without-box=%llu",
        (long long)now, (unsigned long long)s.sprites, (unsigned long long)s.sprites_matched, (unsigned long long)s.groups,
        (unsigned long long)s.groups_multi, (unsigned long long)s.half_lt2, (unsigned long long)s.half_lt4, (unsigned long long)s.half_lt8,
        (unsigned long long)s.half_ge8, s.res_dx / rn, s.res_dy / rn, (unsigned long long)s.res_n, double(tk.goff[0]), double(tk.goff[1]), tk.goff_n,
        (unsigned long long)s.beyond_raw_gate, (unsigned long long)s.prev_closer, (unsigned long long)s.skipped_in_window_unmatched);
    return u;
}
// Up to 8 current contacts: id, seen/in-window, identity colour + glyphs, range (units) and bearing/elevation (deg) from the game camera's forward (A's w
// column; bearing positive = right of the nose about world up Y): for comparing with a game-window shot and the radar.
inline std::string sample_line(const Tracker& tk, int64_t now) {
    std::string line = "xr-stereo;event=mk360-contacts;t-ms=" + std::to_string(now) + ";contacts=";
    unsigned k = 0;
    for (const Track& tr : tk.tracks) {
        if (!tr.seen_now || !tk.cam_ok) continue;
        if (k >= 8) break;
        const float rel[3] = {tr.pos[0] - tk.cam[0], tr.pos[1] - tk.cam[1], tr.pos[2] - tk.cam[2]};
        const float range = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        const float fh[2] = {tk.fwd[0], tk.fwd[2]};
        const float fl = std::sqrt(fh[0] * fh[0] + fh[1] * fh[1]);
        float bearing = 0, elev = 0;
        if (fl > 1e-6f && range > 1e-3f) {
            const float fx = fh[0] / fl, fz = fh[1] / fl;   // right (horizontal) = fwd x up(Y) = (-fz, 0, fx)
            bearing = std::atan2(rel[0] * -fz + rel[2] * fx, rel[0] * fx + rel[2] * fz) * 57.29578f;
            elev = std::asin(std::fmax(-1.0f, std::fmin(1.0f, rel[1] / range))) * 57.29578f;
        }
        char one[200];
        // :c<q> (headset test 5) = the contact's cached picture quality (2 whole, 1 label clipped) or c- (none: nothing drawn for it outside the window)
        char cq[4] = {'c', '-', 0, 0};
        if (tr.cell_ok && tr.cell >= 0) cq[1] = char('0' + (tr.c_q > 2 ? 2 : tr.c_q));
        std::snprintf(one, sizeof one, "%s%u:%s%s:r%.0f:b%.1f:e%.1f:v%.0f:%s%.2f,%.2f,%.2f:g%u:%s", k ? "," : "", tr.id, tr.in_window ? "win" : "out",
                      tr.sprite >= 0 ? "+box" : "", double(range), double(bearing), double(elev), double(tr.speed()), tr.ident ? "id" : "none",
                      double(tr.rgb[0]), double(tr.rgb[1]), double(tr.rgb[2]), unsigned(tr.glyphs), cq);
        line += one;
        ++k;
    }
    if (!k) line += "none";
    return line;
}

} // namespace acz::mk360
