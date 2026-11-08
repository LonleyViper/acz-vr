#pragma once
// tactical.h -- TACTICAL display (xr_terrain_stage option tac:1, default OFF; docs/world-render/tactical.md). Consumer side (XR owner thread), SDK-free:
//  (1) a TRACKER that turns the producer's per-frame replayed world draws (StereoFrame::tac, tactical_frame.h) into persistent contacts, learning each
//      contact's team/colour/label from the game's own in-window marker boxes matched to it (team learning needs the marker to have been seen once);
//  (2) RADAR blips for our own blip layer in the HUD image, at the game's radar widget (lower-left; geometry MEASURED from step62/step71 game shots,
//      research/world-render-data/tactical_radar_measure.py); heading-up, as the game's (verified: blip bearings = marker azimuths in the step62 shot);
//  (3) world-anchored FRND/TGT MARKERS per eye for every teamed contact, in all view directions (only when in view; no clamping);
//  (4) COSMETIC synthetic gun TRACERS while the firing wedge is present (debounced), from the aircraft's gun positions forward (no hit data);
//  (5) the overlay geometry (clip-space triangles) the D3D12 backend draws into the eye / HUD targets.
// Nothing here touches the game; everything is a pure function of the frame facts, the time and the options.
#include "tactical_frame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acz::tac {

// ------------------------------------------------------------------------------------------------ options (inside the xr_terrain_stage value)
struct Options {
    bool on = false;            // tac:1 (everything below needs it)
    // D-355 (headset test 18): our synthetic boxes doubled the game's, outlived killed targets (15 s coasting) and had wrong colours; our blips were
    // wrong (guessed range, drawn over the game's expanded map too). The game's ORIGINAL boxes/radar are the default now: our boxes and blips are OFF
    // unless asked for. tac:1 alone = the cosmetic tracers only (and no game marker is ever suppressed).
    bool radar = false;         // tacradar:1 enables our radar blips (default off)
    bool markers = false;       // tacboxes:1 (alias tacmark:1) enables our world-anchored boxes AND the game-marker suppression (default off)
    bool tracers = true;        // tacgun:0 disables the synthetic tracers
    bool suppress = true;       // tacsup:0 keeps every game marker sprite even with tacboxes:1 (doubles possible); without tacboxes:1 nothing is suppressed
    // tacr:<units> world units at the radar's THIRD ring (radius 0.0858 x screen height). A GUESS (10000 u ~ 2 km at ~5.1 u/m): the game's range
    // scale was not measured (no simultaneous contact position + game radar blip exists in the evidence); the rings themselves are measured.
    float range_ring3 = 10000.0f;
    float self_radius = 80.0f;  // tacself:<u> world draws within this of the game camera are the player's own aircraft (never a contact)
    float cluster = 60.0f;      // tacclu:<u> draws within this of each other form one contact (aircraft parts share one object)
    // MARKERS-360 (D-362, marker360.h, docs/world-render/markers-360.md): independent of tac:1. mk360:1 = ONE set of world-anchored boxes over the whole
    // sphere (the game's in-window sprites hidden and redrawn at their rays + identified contacts outside the window); mk360:2 = the game's in-window
    // boxes kept, ours only outside the window. Replaces tacboxes:1 when both are given. mk360u:1 = also unidentified moving contacts (dim grey; tests).
    // NBOX mk360:3 (markers-360.md section 12): the GAME'S OWN box + label pixels (cut from the replayed marker image, cached per contact) drawn per eye at
    // each contact's direction, in and outside the window; the flat marker quad keeps everything else (its stamp rectangles are cleared). mk360:1 = fallback.
    uint8_t mk360 = 0;
    bool mk360_unknown = false;
    // NBOX near-target policy (markers-360.md section 14): mk360min:<u> stamp distance floor (150), mk360ref:<u> size scales down below this (750; 0 = off),
    // mk360floor:<f> smallest scale (0.3); mk360fade:<u> a stamp nearer than mk360min fades out, invisible at <= this (75; 0 = off; section 16).
    float mk360_near_min = 150.0f, mk360_near_ref = 750.0f, mk360_near_floor = 0.3f, mk360_near_fade = 75.0f;
    // markers-360.md section 17.4: mk360lock:0 keeps the lock diamond in the HUD layer (default 1 with mk360:3: LOCK-SHAPE moves it into the marker image);
    // mk360cen:1 = the one-shot HUD census after box draws (tac-lock-census lines in step5-producer.log; default 0).
    bool mk360_lock = true, mk360_census = false;
    bool mk360_lockdump = false;   // mk360ld:1 = bounded one-shot per-draw dump of rejected lock-diamond candidates (tac-lock-dump lines; markers-360.md section 21.6)
    // mk360p:1 = on: our provisional stroke box for contacts the game never boxed (markers-360.md sections 19-21). Default OFF since headset test 9
    // (section 22): the user wants only the game's own boxes; the provisional ones were the different-green far boxes, the boxes on wrecks and the grey ones.
    bool mk360_prov = false;
    // mk360pu:0|1 (section 20): a provisional box whose team is unknown (formation candidate, no camera velocity): 0 = skipped, 1 = neutral grey (default)
    uint8_t mk360_prov_unknown = 1;
};
// Removes the TACTICAL keys (tac, tacr, tacradar, tacboxes, tacmark, tacgun, tacsup, tacself, tacclu, mk360, mk360u; ':' or '=') from a "m02a;opt:val;..." value and returns the rest.
// false + why for a bad value of one of OUR keys (the terrain then refuses the whole value like any bad option).
bool extract_options(const std::string& value, std::string& rest, Options& opt, std::string& why);
std::string describe(const Options&);
// The producer flags for these options: 0 when tac and mk360 are off (nothing collected, nothing suppressed); kProducerOn with tac:1 or mk360;
// kProducerSuppress only when OUR tac boxes are drawn (tacboxes:1, no mk360) and tacsup is not 0; kProducerHideAll with mk360:1 (every completely read
// box draw is redrawn by the consumer) -- a game marker sprite is never removed unless we draw that marker ourselves.
uint32_t producer_flags_for(const Options&) noexcept;

// ------------------------------------------------------------------------------------------------ the game's radar widget (MEASURED)
// Normalised to the game's client/scene image (u = x / width, v = y / height), from the step62 and step71 2404x1352 game-window shots (identical in 3 shots
// to 0.0002): box (0.0599, 0.6457)..(0.2192, 0.9379), centre = box centre = ring centre (0.13960, 0.79250); rings at 0.0216, 0.0430, 0.0858, 0.1289 of the
// image HEIGHT (ratio 1 : 2 : 4 : 6). The player's arrow sits at the centre; the 90-degree wedge (+-45 deg) opens upward = heading-up.
struct RadarGeom {
    float cu = 0.13960f, cv = 0.79250f;
    float half_u = 0.07966f, half_v = 0.14608f;   // box half extents (fractions of width / height)
    float ring[4] = {0.0216f, 0.0430f, 0.0858f, 0.1289f};
};

// ------------------------------------------------------------------------------------------------ tracker
enum Team : uint8_t { team_unknown = 0, team_friend = 1, team_enemy = 2 };
enum Label : uint8_t { label_none = 0, label_frnd = 1, label_tgt = 2 };
// Team from a marker colour (normalised rgb): blue-dominant = friendly (the game's FRND boxes are blue: step62 shot); any other saturated colour = enemy
// (the game's enemy/target colours are green/yellow/red in the step36 radar: INFERENCE); grey/white = unknown.
Team team_of(const float rgb[3]) noexcept;
// Label from the glyph count of the label cluster: 4 = "FRND", 3 = "TGT" (the game's two label texts), else from the team (friend = FRND, else none).
Label label_of(uint8_t glyphs, Team t) noexcept;

struct Track {
    uint32_t id = 0;
    float pos[3] = {}, vel[3] = {};
    bool vel_valid = false;
    double t_seen = 0, t_first = 0;
    uint32_t frames = 0;
    Team team = team_unknown; Label label = label_none;
    float rgb[3] = {1, 1, 1};
    bool seen_now = false;          // observed in the newest frame
    bool game_marker_now = false;   // the game's own marker for it is drawn (not suppressed) in the newest frame: we do not draw ours (no doubles)
    bool marker_now = false;        // matched to a game marker in the newest frame (suppressed or not)
    float speed() const noexcept;
};
struct Tracker {
    std::vector<Track> tracks;
    uint32_t next_id = 1;
    // counters (cumulative)
    uint64_t frames = 0, clusters = 0, self_draws = 0, matched = 0, created = 0, dropped = 0, teamed = 0;
};
// Coasting: a contact not seen in a frame (the game draws only what its camera sees) is dead-reckoned with its last velocity for up to
// kCoastTeamed (teamed) / kCoastUnknown seconds, then dropped.
constexpr double kCoastTeamed = 15.0, kCoastUnknown = 2.0;
constexpr float kMovingSpeed = 75.0f;   // units/s (~15 m/s at ~5.1 u/m): slower contacts with no team are not shown (static buildings/props)
// One producer frame. cam_eye = the game camera (StereoFrame::game_eye, else the E6 campos), t = seconds (monotonic).
void tracker_update(Tracker&, const TacFrame&, const float cam_eye[3], double t, const Options&);

// ------------------------------------------------------------------------------------------------ radar
// Heading frame: horizontal unit forward/right of the aircraft (from the game camera forward/up; a near-vertical forward uses the up vector).
bool heading_frame(const float fwd[3], const float up[3], float fwd_h[3], float right_h[3]) noexcept;
struct Blip {
    uint32_t id = 0;
    float u = 0, v = 0;             // HUD image normalised position
    float dir = 0;                  // arrow direction on the radar, radians clockwise from up (contact heading relative to the player's)
    bool arrow = false;             // a moving contact (else a square)
    bool edge = false;              // beyond the box: clamped to the box border (chevron)
    bool coasting = false;
    float rgb[3] = {1, 1, 1};
    float range = 0, bearing = 0;   // world units, radians clockwise from the nose (diagnostic)
};
// aspect = image height / width (0.5625 at 2560x1440).
void radar_blips(const Tracker&, const float player[3], const float fwd_h[3], const float right_h[3], double t, const Options&, const RadarGeom&,
                 float aspect, std::vector<Blip>&);
// The radar pixel of a world point (the same mapping, exposed for tests): false when beyond the box (u, v then clamped to the border).
bool radar_uv(const float rel[3], const float fwd_h[3], const float right_h[3], const Options&, const RadarGeom&, float aspect, float& u, float& v) noexcept;

// ------------------------------------------------------------------------------------------------ firing + cosmetic tracers
// Conservative debounce: ON after the wedge was present in >= kOnFrames consecutive producer frames spanning >= kOnSeconds (45 ms); OFF after it was absent for
// >= kOffSeconds (and >= kOffFrames frames). A single stray frame never fires; a missile launch also shows the wedge (D-318) and is NOT told apart.
struct FiringDetector {
    static constexpr uint32_t kOnFrames = 3, kOffFrames = 4;
    static constexpr double kOnSeconds = 0.045, kOffSeconds = 0.15;
    bool on = false;
    uint32_t run = 0, gap = 0;
    double run_t0 = -1, gap_t0 = -1;
    uint64_t on_events = 0;
    bool update(bool wedge, double t) noexcept;
};
struct Round { float p0[3] = {}, v[3] = {}; double born = 0; };
struct TracerSim {
    // COSMETIC: rate, speed, life and gun offsets are illustrative (not the game's ballistics; no hit data).
    static constexpr double kInterval = 0.06;      // one visible tracer per gun every 60 ms
    static constexpr double kLife = 1.1;           // seconds
    static constexpr float kMuzzle = 5100.0f;      // units/s (~1000 m/s at ~5.1 u/m)
    static constexpr float kStreak = 0.014f;       // seconds of flight per streak (~70 units)
    static constexpr float kGravity = 50.0f;       // units/s^2 (~9.8 m/s^2)
    std::vector<Round> rounds;
    double next = 0;
    uint64_t spawned = 0;
    // guns: world positions of the two muzzles; fwd: the aircraft forward (unit); vel: the aircraft velocity (units/s).
    void update(bool firing, double t, const float gun_l[3], const float gun_r[3], const float fwd[3], const float vel[3]);
    // the streak of a round at time t (false = expired)
    static bool streak(const Round&, double t, float a[3], float b[3], float& alpha) noexcept;
};
// The gun positions in the aircraft frame (camera-relative, units): lateral +-6 u (~1.2 m), 5 u below, 10 u ahead of the eye (illustrative).
void gun_positions(const float cam[3], const float right[3], const float up[3], const float fwd[3], float gun_l[3], float gun_r[3]) noexcept;

// ------------------------------------------------------------------------------------------------ overlay geometry
struct Vertex { float pos[4]; float col[4]; };   // clip-space position (x, y, 0, w) + premultiplied linear colour
// slot_eye0/1_stamp (NBOX mk360:3): textured stamps, pos = clip (x, y, 0, 1), col = (atlas u, v, 1, 0); drawn with the stamp pipeline, not the colour one.
enum Slot : unsigned { slot_eye0_pre = 0, slot_eye1_pre = 1, slot_eye0_post = 2, slot_eye1_post = 3, slot_hud = 4, slot_eye0_stamp = 5, slot_eye1_stamp = 6,
                       slot_count = 7 };
struct Overlay {
    std::vector<Vertex> v;
    uint32_t first[slot_count] = {}, count[slot_count] = {};
    void clear() { v.clear(); for (unsigned i = 0; i < slot_count; ++i) first[i] = count[i] = 0; }
    bool empty() const noexcept { return v.empty(); }
};
constexpr uint32_t kOverlayCap = 24576;          // vertices per frame (the backend's upload buffer); more = the rest is dropped (counted)
// One eye's projection: clip = M (p - eye, 1) (acz::props::make_clip of the eye's world composite plane), plus the eye's camera up for billboards.
// right/fwd (optional, with up an orthonormal eye frame; zero = unknown): the boxes are drawn axis-aligned in the eye's screen with them.
struct EyeView { float clip[16] = {}; float eye[3] = {}; float up[3] = {0, 1, 0}; bool ok = false; float right[3] = {}; float fwd[3] = {}; };
// The eye's position and its whole frame (right/up/fwd) in one step: every EyeView of one eye (a fresh frame's AND a DECOUPLE re-composite's) is
// filled by this, so the screen frame of a marker/stamp at the same pose never depends on which path built it (Codex J-203 finding 1).
inline void set_eye_frame(EyeView& e, const float eye[3], const float right[3], const float up[3], const float fwd[3]) noexcept {
    for (int i = 0; i < 3; ++i) { e.eye[i] = eye[i]; e.right[i] = right[i]; e.up[i] = up[i]; e.fwd[i] = fwd[i]; }
}
// true when right/fwd are set (an orthonormal eye frame, not the zero "unknown" default)
bool has_eye_frame(const EyeView&) noexcept;
float srgb_to_linear(float c) noexcept;
// Each builder appends to `o` for slot `s` (call them grouped per slot: begin_slot / end_slot keep first/count).
void begin_slot(Overlay&, Slot) noexcept;
void end_slot(Overlay&, Slot) noexcept;
void build_radar(Overlay&, const std::vector<Blip>&, const RadarGeom&, float aspect);
// Box half-angle (tan) and line width of our markers, matched to the game's boxes in the step62 shot (box ~60/2000 px of a 2 x 0.6152 tan window).
constexpr float kMarkerHalfTan = 0.0185f, kMarkerLineTan = 0.0016f, kGlyphTan = 0.0135f;
// half_tan_x/y: the box half extents (tan); MARKERS-360 sets them from the game sprite's own size, tac keeps the defaults.
struct WorldMarker { float pos[3] = {}; float rgb[3] = {1, 1, 1}; Label label = label_none; uint32_t id = 0; bool coasting = false;
                     float half_tan_x = kMarkerHalfTan, half_tan_y = kMarkerHalfTan;
                     float alpha = 1.0f;   // provisional boxes fade out near the cockpit (markers-360.md section 21)
};
// true when the marker was in front of the eye (drawn).
bool build_marker(Overlay&, const EyeView&, const WorldMarker&);
// The box's screen frame in the eye's NDC: centre (cx,cy) and NDC per tan (sx along the eye right, sy along the eye up); false when not drawable.
bool marker_screen_frame(const EyeView&, const float pos[3], float& cx, float& cy, float& sx, float& sy) noexcept;
// The markers WE draw this frame: every teamed contact, except those whose game marker sprite is drawn (not suppressed) this frame (no doubles).
// A coasting contact is dead-reckoned and drawn dimmer. skipped_game = contacts left to the game's own marker.
void collect_markers(const Tracker&, double t, std::vector<WorldMarker>& out, uint32_t& skipped_game);
unsigned build_tracers(Overlay&, const EyeView&, const TracerSim&, double t);
// The CPU projection the backend's vertices encode (tests): world point -> eye uv (0..1, v down); false behind the eye.
bool eye_uv(const EyeView&, const float p[3], float& u, float& v) noexcept;

} // namespace acz::tac
