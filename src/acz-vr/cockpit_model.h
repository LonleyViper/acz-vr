#pragma once
// cockpit_model.h -- COCKPIT-2 (D-302/D-303): baked aircraft/cockpit model (.zvrm, research/world-render-data/model_baker.py) loader, the
// per-eye 6DoF camera of the model-based cockpit, the GPU vertex stream it draws and a CPU REFERENCE rasteriser that the HLSL pass
// (cockpit_model.hlsl) mirrors operation for operation. SDK-free (no Windows/D3D12 types).
// Format: docs/world-render/model-decode.md section 3 (header 160 bytes, SHA-256 of the payload). Design and evidence:
// docs/world-render/cockpit-render-impl.md. Only the file handling and the maths are tested offline; game fidelity is INFERENCE.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acz::cockpit {

inline constexpr uint32_t kZvrmVersion = 1;
inline constexpr size_t kZvrmHeader = 160, kPartBytes = 128, kTexHeader = 16, kVertexStride = 32;
inline constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;
inline constexpr uint32_t kMaxParts = 8192, kMaxTextures = 16, kMaxTexDim = 1024, kMaxVertices = 1u << 21, kMaxIndices = 3u << 21;
inline constexpr uint8_t kFloorRgba[4] = {40, 42, 40, 255};   // floor closure vertex colour (display: ~ the game's dark cockpit tones under the light)
inline constexpr unsigned kGpuTextures = 4;   // texture slots of the GPU pass (t1..t4); a model using more is refused
inline constexpr unsigned kNoTexture = 7;     // GpuVertex::mat low bits: no texture (white)
// Header flags (u32 at byte 60; model_baker.py ZF_*). COCKPIT-3: kZvrmRgbaClut = the textures decoded from embedded GIM CLUTs are R,G,B,A. A file WITHOUT it
// from an mpl/acm source (kind 1/3: the COCKPIT-1 bakes read the CLUT as BGRA, D-303) has R and B swapped; parse_model swaps them back (legacy_clut_fixed).
// COCKPIT-4: kZvrmAnimRest = animated parts carry a FITTED rest rot[0] (part record +120 f32 rest, +124 u32 anim flags; acm_lib.fit_rest_angles): local_to_model
// and the vertices include it, and kPartHingeFitted marks a hinge whose rest pose matched the static geometry. Older files: rest 0, nothing located.
inline constexpr uint32_t kZvrmCcw = 1, kZvrmRestPose = 2, kZvrmRgbaClut = 4, kZvrmAnimRest = 8, kZvrmKnownFlags = 15;
inline constexpr uint32_t kPartHingeFitted = 1, kPartKnownAnimFlags = 1;
// STORES: `source_part` of the attached-store parts the baker adds to a vrcockpit model (weapons '<model>/store<station>/..', the tank '<model>/tank<station>/..'; mpl part 7).
// They are exterior parts but not airframe skin: the cockpit-clip fit ignores them.
inline constexpr uint32_t kStoresSourcePart = 7;
inline constexpr float kModelUnitsPerMetre = 5.0f;   // model-decode.md finding 3 (Draken 5.00 u/m): the size of the COCKPIT-4 procedural interior

// Material flags (model_baker.py MF_*).
enum : uint32_t {
    MF_ALPHA_TEST = 1, MF_BLEND = 2, MF_GLASS = 4, MF_HIDDEN = 8, MF_UNLIT = 16, MF_TEX_ALPHA_NOT_COVERAGE = 32, MF_EXTERIOR = 64, MF_COCKPIT_DUP = 128,
};

enum class LoadStatus : uint8_t { ok = 0, io_error, too_small, too_large, bad_magic, bad_version, bad_header, bad_size, bad_hash, bad_content };
const char* status_name(LoadStatus) noexcept;

struct Part {
    uint32_t name_off = 0; int32_t parent = -1; uint32_t node = 0, game_flags = 0, group = 0; int32_t anim = -1;
    uint32_t mflags = 0; int32_t texture = -1; uint32_t first_index = 0, index_count = 0, first_vertex = 0, vertex_count = 0;
    float local_to_model[16] = {}; float hinge = 0; uint32_t source_part = 0;
    float rest = 0; uint32_t aflags = 0;   // COCKPIT-4 (kZvrmAnimRest): fitted rest rot[0] (radians, included in local_to_model) and kPartHinge* flags
};
struct Texture { uint32_t w = 0, h = 0; std::vector<uint8_t> rgba; };   // RGBA8, straight alpha, row 0 = top (v = 0)
#pragma pack(push, 1)
struct Vertex { float pos[3]; float uv[2]; int16_t nrm[3]; uint16_t part; uint8_t rgba[4]; };
#pragma pack(pop)
static_assert(sizeof(Vertex) == kVertexStride, "zvrm vertex is 32 bytes");

struct ModelData {
    uint32_t flags = kZvrmKnownFlags, source_kind = 1;
    bool has_eye = false;
    bool legacy_clut_fixed = false;   // COCKPIT-3: a pre-flag mpl/acm bake whose texture R/B were swapped back at load (flags now include kZvrmRgbaClut)
    float bbox_min[3] = {}, bbox_max[3] = {}, eye[3] = {};
    std::vector<Part> parts;
    std::vector<Texture> textures;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::string strings = std::string(1, '\0');
    uint8_t payload_sha[32] = {};
    std::string part_name(size_t i) const;
};

// Parse + validate a whole file image. All-or-nothing: on any failure `out` is untouched and the status names the first problem.
LoadStatus parse_model(const uint8_t* data, size_t size, ModelData& out) noexcept;
LoadStatus load_model_file(const std::string& path, ModelData& out) noexcept;
// Exactly the layout model_baker.py Builder.write() produces (tests and the synthetic builder; deterministic). bbox recomputed from the vertices.
std::vector<uint8_t> serialize_model(const ModelData&);
// id = [A-Za-z0-9_]{1,24} (file stem, e.g. "j35jk_vrcockpit"); path = <dir>\<id>.zvrm, default dir %LOCALAPPDATA%\acz-vr\models. Empty on a bad id.
bool valid_model_id(const std::string& id) noexcept;
std::string model_path(const std::string& id, const std::string& dir_override = std::string());

// ----------------------------------------------------------------------------------------------------------------- options
// Config value of xr_cockpit_model: "<id>[;opt:val;...]" (":" or "="). Unknown option / bad value => the model is NOT used (logged), never half applied.
struct Options {
    float upm = 5.0f;          // upm:<units per metre> head translation scale in model space (model-decode.md: ~5.0 u/m)
    float gain = 1.0f;         // gain:<x> colour gain in the DISPLAY domain (COCKPIT-3: fitted to the game window, cockpit-render-impl.md section 10)
    float ambient = 0.6f;      // amb:<0..1> ambient share of the Lambert term (m02a stage light: ambient / (ambient + light) = 0.59)
    bool display = true;       // dom:display|raw  COCKPIT-3: shade in the game's DISPLAY domain and convert once to linear (like the terrain/props); raw = COCKPIT-2
    bool stage_light = true;   // light:stage|fixed  COCKPIT-3: the stage's sun (WORLD-2 env P[1]) in the cockpit frame when the terrain runs; else kLight
    bool floor = true;         // floor:0  COCKPIT-3: close the open bottom of the game's cockpit part (it has no floor: the eye saw the terrain and the inside of the
                               // airframe - the white gear-well roof, p0 n0/n55 - below the panel) with a dark synthetic floor quad; see Mesh::floor
    float cut = 0.5f;          // cut:<0..1> alpha-test threshold of cockpit-atlas texels
    float near_u = 0.05f;      // near:<units> near plane (1 cm at 5 u/m)
    bool exterior = true;      // ext:0 hides every MF_EXTERIOR part (airframe/wings/canopy frame)
    bool glass = true;         // glass:0 hides glass parts
    bool hud_glass = false;    // hudglass:1 places the game's HUD quad at the model's HUD glass distance (default: collimated, world-quad distance)
    bool keep_replay = false;  // replay:1 A/B: the game's replayed cockpit stays composited UNDER the model (registration check)
    bool eye_override = false; // eye:x,y,z overrides the file's pvb eye point (model units)
    float eye[3] = {0, 0, 0};
    std::vector<uint32_t> hide, show;  // hide:<i>,<i>  show:<i>,<i> part indices (show also un-hides MF_HIDDEN parts)
    // COCKPIT-4 (docs/world-render/cockpit4-impl.md)
    bool canopy = true;        // canopy:0|1  the canopy glass (the big cockpit-part glass around the eye, else the exterior canopy glass seen from inside) drawn as a
                               // per-channel TRANSMISSION over the world behind it + a Fresnel sky reflection (dual-source blend); 0 = the COCKPIT-2 straight-alpha glass
    float tint[3] = {0.951f, 0.940f, 0.935f};   // tint:r,g,b  canopy transmission in the DISPLAY domain (WORLD-4 measured: game cockpit view / HUD view, sky rows)
    float refl = 0.2f;         // refl:<0..1>  scale of the Schlick Fresnel reflectance (F0 0.04) of the sky colour (made-up strength: "slight")
    float dirt = 0.0f;         // dirt:<0..0.5>  uniform sky-coloured veil (dust/scratches; made up, default off)
    bool interior = true;      // interior:0|1  procedural seat/headrest/stick/throttle/pedals/tub/bulkhead (MADE UP: the game has no such geometry; replaces the floor:1 quad)
    bool anim = false;         // anim:0|1  control surfaces (fitted hinges of kZvrmAnimRest bakes) + interior stick/throttle/pedals from the E6 attitude rates
    uint8_t stick = 0;         // stick:centre|side|none  interior stick: centre (default; Draken/MiG-21), right-console side-stick (F-16), none
    // STORES-RT (docs/world-render/aircraft-auto.md section 13): stores:0|1 (default 1) the baked weapon parts (name "<model>/store<station>/...") are emitted as one
    // animated-range piece per station so the per-station mask (apply_store_mask) can hide a fired weapon; 0 = they stay ordinary static parts (never hidden).
    // stwatch:0|1 (default 0) logs the launch-signal search (ee_store_watch.h): small counters near the E6 structure that count down by one.
    bool stores = true;
    // SP-STORES (aircraft-auto.md section 15): spstores:0|1 (default 1) the baked special-weapon parts (name "<model>/sp<station>/...", the HUD's SP row weapon) are
    // their own animated-range pieces with their own mask (apply_sp_mask) so the SP pylons empty and reload like the MSSL ones; 0 = they stay ordinary static parts
    // (never hidden). stores:0 turns the whole mechanism off, so SP parts are static then too.
    bool spstores = true;
    bool stwatch = false;
    // stwatch-start:<n> (default 50) the HUD count at the first lock for the wide counter search (ee_counter_search.h); stctr:<hex block offset>,<1|2|4> the found
    // counter that drives acz::stores::signal() (0 = unwired). Both are read from the raw option string by the probe; parsed here so a bad value refuses the model.
    uint32_t stwatch_start = 50;
    uint32_t stctr_off = 0, stctr_width = 4;
    // MSL-HUD (hud_msl.h, aircraft-auto.md section 14): mslhud:0|1|2 (default 1 = on; 0 = off) 1 = the HUD MSSL count read from the game's glyph quads drives the store
    // mask; 2 = that + msl-hud-dump glyph records (default on after the step172 live pass). Read from the raw option string by the step-5 producer; parsed here so a bad value refuses the model.
    uint8_t mslhud = 1;        // default ON since the step172 live pass; mslhud:0 = off
    // strel:<seconds> (default 3.0, 0..60) the pylon reload time: a fired station shows again after it while missiles remain (store_tracker.h; the default is an estimate).
    float strel = 3.0f;
    bool zpre = false;         // zpre:0|1  GPU-COST: opaque model first + depth-tested eye composite (no world shading under the opaque cockpit; same image)
    // COCKPIT-SCALE (D-355 headset feedback "a little bigger"): cscale:<0.8..1.5>  uniform scale of the WHOLE model cockpit (game parts, floor, procedural
    // interior, animated pieces, canopy, HUD glass) about the pvb EYE point. Every ray from the centred eye keeps its direction (HUD/gunsight-glass and world
    // registration unchanged); only distances scale, i.e. the apparent physical size via stereo disparity (IPD) and head-motion parallax. Implemented in the
    // camera, not the mesh: a model scaled by s about pvb seen from pvb + upm*d is exactly the unscaled model seen from pvb + (upm/s)*d with near/s (clip
    // scales by s, NDC and reversed-Z depth identical) -> view_upm()/view_near(). 1.0 = the unscaled model, bit-identical to before the option existed.
    float cscale = 1.0f;
    // HUD-MENU (D-362, docs/world-render/hud-menu.md; constants in hud_menu.h): while the game camera is frozen (pause menu) the HUD layer is shown at the
    // game's angular size (scale 1, lens-centred) at menud metres with an alpha floor menub that darkens every layer below it (the game's menu backdrop).
    bool menu = true;          // menu:0|1
    float menu_distance = 2.0f;   // menud:<0.5..20> metres
    float menu_backdrop = 0.6f;   // menub:<0..0.95>
    // RADAR-BG (docs/world-render/radar-background.md; hud_menu.h radar_bg_bits): the game's radar square is translucent black (scene x ~0.44) but the HUD target
    // stores alpha 1 there (the stencil mask quads), so the HUD layer shows an opaque black square. radarbg:1 caps the HUD alpha to the measured background
    // alpha radarbga (+ the symbol's brightness over it); texels whose stored alpha is already lower (everything outside the square) are unchanged. Default off.
    bool radar_bg = false;        // radarbg:0|1
    float radar_bg_alpha = 0.5625f;   // radarbga:<0.05..0.95> (quantised to 1/32)
    // COCKPIT-CLIP (headset test 19: "the cockpit protrudes out of the nose and behind the cockpit"; cockpit-render-impl.md section 16). clip:1 (default) / clip:0.
    // (a) the procedural interior / floor closure (made-up geometry) is FITTED into the airframe: its large faces are tessellated and every vertex is pulled
    //     inside the exterior skin (incl. the canopy glass) by axis rays, so no tub wall, bulkhead corner or seat rail pokes out of the nose, spine or canopy;
    // (b) every NON-exterior, non-glass static triangle (the game's cockpit part + the interior) is clipped exactly against the per-type clip volume (a box
    //     around the pvb eye, metres: ahead, behind, below, above, half-width) = the aircraft clip table (clip_volume_for) unless clipbox: is given.
    // The exterior airframe, glass (HUD glass, gauges, canopy) and animated pieces are never clipped. In model space, before the camera: cscale-independent.
    bool clip = true;
    bool clip_box_set = false; float clip_box[5] = {0, 0, 0, 0, 0};   // clipbox:<ahead>,<behind>,<below>,<above>,<half> metres (per model; refused with auto)
    // SKIN-REFL (docs/world-render/aircraft-shading-reflection.md section 7): the engine's cb10-bit-0 reflection pass on the exterior skin (kMatTexAlphaNotCov
    // parts, opaque pass): display colour += sreflk * skin_alpha * refl_map(hemisphere uv of the WORLD normal).rgb * saturate(10 * n_world.y), with the
    // drawn stage's <stage>_ref map (sidecar <stage>_ref.zvrt next to the .zvrs). No map / no stage environment = off for that frame. Default off.
    // Since CFID (cockpit-fidelity.md) the default follows cfid: parse_options sets skin_refl = fidelity unless srefl: was given explicitly.
    bool skin_refl = false;    // srefl:0|1
    bool skin_refl_set = false;   // srefl: given explicitly (wins over cfid)
    float skin_refl_k = 1.0f;  // sreflk:<0..4>  strength (1 = the engine's add)
    // CFID (docs/world-render/cockpit-fidelity.md, headset 2026-10-29 item 6): cfid:1 (DEFAULT ON) / cfid:0. Bundles (a) SKIN-REFL (srefl defaults to cfid),
    // (b) the engine's lit-branch sun response on the exterior skin (DrawCPT_v/DrawACM_v +356 == 0 mode: s = max(0, 2 (n.L)^3) instead of max(0, n.L), clamp
    // 1.99; mean over n.L in [0,1] unchanged, so the calibrated level is kept), (c) an analytic sun shadow of the cockpit rim (glare shield + rails) on the
    // cockpit interior (Mesh::shadow box from the game's cockpit part). cfid:0 = the pre-CFID image, bit-identical.
    bool fidelity = true;      // cfid:0|1
    // SEAT-CLEARANCE (headset test 6 item 4: "the seat we rendered in is too far forward so when I look up my head clips into it"; cockpit-fidelity.md section 5).
    // seatback:<m> = the distance (model metres at 5 u/m, in the UNSCALED model like eye:/clipbox:, so the mesh stays independent of cscale) from the eye to the
    // FRONT of the procedural headrest. The COCKPIT-4 placement was 0.21 m (the 0.36 m seat-back clamp - 0.15): 0.24 m physical at cscale 1.15, i.e. 7 cm behind
    // the skull (0.17 m behind the eye) and inside the arc the occiput sweeps when the head pitches up about the neck (~0.30 m). Default 0.30 = 0.345 m physical
    // at cscale 1.15 (0.17 head + 0.17 clearance), 0.30 m at cscale 1.0. The seat is only ever moved BACK (seat_back_max); 0 = the COCKPIT-4 placement, bit-identical.
    float seat_back = 0.30f;   // seatback:<0..0.8> model metres
};
inline constexpr float kSeatBackMax = 0.8f;
// The seat-back reference z (metres behind the eye at 5 u/m; the headrest front is back_max - 0.15, its back face back_max - 0.04, the rails end at back_max):
// back_max_old (the COCKPIT-4 value, clamped 0.10..0.36 inside the footprint) pushed back so the headrest front is at least seatback_m from the eye:
// max(back_max_old, seatback_m + 0.15). seatback_m <= 0 (or NaN) returns back_max_old.
inline float seat_back_max(float back_max_old, float seatback_m) noexcept {
    if (!(seatback_m > 0.0f)) return back_max_old;
    const float want = seatback_m + 0.15f;
    return want > back_max_old ? want : back_max_old;
}
inline constexpr float kCScaleMin = 0.8f, kCScaleMax = 1.5f;
// COCKPIT-CLIP per-type clip volume (metres from the pvb eye; generated by research/world-render-data/cockpit_clip_survey.py table from the 36 bakes, mirrored
// in research/world-render-data/cockpit_clip_volumes.json). nullptr for an unknown model id.
struct ClipVolume { const char* model_id; float ahead, behind, below, above, half; };
inline constexpr unsigned kClipVolumeCount = 36;
extern const ClipVolume kClipVolumes[kClipVolumeCount];
const ClipVolume* clip_volume_for(const std::string& model_id) noexcept;
// Sets o.clip_box from the table for model_id unless clipbox: was given or clip:0. Returns true when a box is in effect afterwards.
bool apply_clip_table(Options& o, const std::string& model_id) noexcept;
inline constexpr float kClipMarginM = 0.02f;   // the fitted interior stays this far inside the skin (metres)
// The head-translation scale (model units per metre of head motion) and near plane the per-eye camera uses for this model: upm / cscale, near / cscale.
// Everything that converts model units to metres for this cockpit (e.g. the hudglass:1 HUD distance) divides by view_upm, not upm.
inline float view_upm(const Options& o) noexcept { return o.upm / o.cscale; }
inline float view_near(const Options& o) noexcept { return o.near_u / o.cscale; }
bool parse_options(const std::string& value, std::string& id, Options& o, std::string& why);

// ----------------------------------------------------------------------------------------------------------------- GPU stream
// One expanded (non-indexed) vertex per triangle corner, read by the vertex shader as a raw buffer (ByteAddressBuffer, 32-byte stride).
// mat: bits 0-2 texture slot (kNoTexture = none), bit 3 alpha test, bit 4 blend pass, bit 5 unlit (reserved, not shaded differently: the
// verified preview ignores it), bit 6 texture alpha is not coverage.
enum : uint16_t { kMatAlphaTest = 8, kMatBlend = 16, kMatUnlit = 32, kMatTexAlphaNotCov = 64 };
#pragma pack(push, 1)
struct GpuVertex { float pos[3]; float uv[2]; int16_t nrm[3]; uint16_t mat; uint8_t rgba[4]; };
#pragma pack(pop)
static_assert(sizeof(GpuVertex) == 32, "GpuVertex mirrors the 32-byte HLSL fetch");
struct HudGlass { bool valid = false; uint32_t part = 0; float centre[3] = {}; float distance = 0; float tan_l = 0, tan_r = 0, tan_u = 0, tan_d = 0; };
// COCKPIT-4 animation. Inputs are normalised pilot commands derived from the aircraft's attitude rates (anim_update): pitch +1 = full nose-up (stick aft),
// roll +1 = full right roll (stick right), yaw +1 = full nose-right (right pedal), throttle 0..1. Every animated piece rotates rigidly about up to two axes
// through its pivot: angle_i = gain_i * clamp(bias + cp*pitch + cr*roll + cy*yaw + ct*throttle, -1, 1); v' = pivot + R(a0, angle0) R(a1, angle1) (v - pivot).
struct AnimInputs { float pitch = 0, roll = 0, yaw = 0, throttle = 0.5f; };
struct AnimAxis { float axis[3] = {1, 0, 0}; float gain = 0, bias = 0, cp = 0, cr = 0, cy = 0, ct = 0; };
enum class AnimRole : uint8_t { none = 0, pitch_surface, roll_surface, rudder, stick, throttle, pedal, store };
// role == store (STORES-RT): all parts of ONE weapon station (the baked weapon parts are named "<model>/store<station>/n<node>", station = the game's
// weapon-station locator group id, e.g. 67), emitted into the animated range so a per-station visibility mask can collapse them at run time; naxes = 0,
// pivot = the station's vertex centroid (model space), station = the group id.
struct AnimPiece { uint32_t first = 0, count = 0; float pivot[3] = {}; AnimAxis ax[2]; uint8_t naxes = 1; AnimRole role = AnimRole::none; int32_t part = -1; uint32_t station = 0;
                   bool sp = false;   // SP-STORES: role store of the special-weapon group ("<model>/sp<station>/...", mask = Mesh::sp_stations) instead of the standard group
                 };
float anim_angle(const AnimAxis&, const AnimInputs&) noexcept;
// Model anim part -> role from the game's node GROUP id (survey of j35j/m21b/f16c, cockpit4-impl.md section 3): 1/2 = left/right elevator (stabilator), 3 = rudder,
// 5/6 = left/right aileron (elevon); others (7 flaps, 0x11 leading-edge flaps, 0xf airbrakes, 0x1d) are not driven (none).
AnimRole surface_role(uint32_t group) noexcept;
struct Mesh {
    std::vector<GpuVertex> v;            // [opaque triangles][blend triangles, parts back-to-front from the eye]
    uint32_t opaque_vertices = 0, blend_vertices = 0;
    std::vector<Texture> textures;       // <= kGpuTextures, slot order
    float eye[3] = {0, 0, 0};            // the cockpit eye point (pvb or eye: override), model units
    uint32_t parts_drawn = 0, parts_hidden = 0, triangles = 0;
    HudGlass hud;
    // COCKPIT-3 floor closure (option floor:1, default): an up-facing opaque quad at the lowest point of the drawn cockpit (non-exterior) geometry, spanning its
    // x/z footprint, untextured, colour kFloorRgba. Synthetic (the game has no cockpit floor: its 63x38 lens never looks that far down). Two triangles.
    bool floor = false;
    float floor_y = 0, floor_x[2] = {0, 0}, floor_z[2] = {0, 0};
    // COCKPIT-4. Vertex order: [opaque static][opaque animated: anim_first .. +anim_vertices][blend][canopy glass: glass_vertices]. opaque_vertices INCLUDES the
    // animated range; the canopy range follows the blend range and is drawn with the dual-source glass pass (pass 2).
    uint32_t glass_vertices = 0, canopy_parts = 0; bool canopy_exterior = false;   // canopy glass taken from the exterior model (flipped to face the eye)
    uint32_t anim_first = 0, anim_vertices = 0;
    std::vector<AnimPiece> anim;
    std::vector<uint32_t> store_stations;   // STORES-RT: the station group ids that have a store piece, in piece order (bit i of a mask = store_stations[i])
    std::vector<uint32_t> sp_stations;      // SP-STORES: the same for the special-weapon pieces (AnimPiece::sp), a separate bit space (bit i = sp_stations[i])
    uint32_t anim_surfaces = 0, anim_unlocated = 0;   // driven model surfaces; animated model parts NOT driven (hinge not fitted / old bake / no role / no range)
    bool interior = false; uint32_t interior_triangles = 0;
    float interior_floor_y = 0;
    float seat_front_u = 0;   // SEAT-CLEARANCE: the procedural headrest front, model units behind the eye (0 = no interior)
    // COCKPIT-CLIP evidence: box in effect (model units: x/y/z lo..hi), game cockpit triangles removed / cut by it, interior triangles removed / cut, interior
    // vertices pulled inside the skin, skin triangles used. tri_src: per emitted triangle 0 exterior, 1 game cockpit part, 2 synthetic (interior/floor).
    bool clip_box = false; float clip_lo[3] = {0, 0, 0}, clip_hi[3] = {0, 0, 0};
    uint32_t clip_removed = 0, clip_cut = 0, clip_interior_removed = 0, clip_interior_cut = 0, fit_moved = 0, fit_skin = 0;
    std::vector<uint8_t> tri_src;
    // CFID cockpit-rim shadow box (model units): {sill_y, half_x, z_front, z_back}; see cockpit_shadow_box. shadow = false: no shadow term.
    bool shadow = false; float shadow_box[4] = {0, 0, 0, 0};
};
// CFID: the cockpit opening of a model, from the game's cockpit part (parts without MF_HIDDEN/MF_EXTERIOR/MF_COCKPIT_DUP/MF_GLASS) and the eye E:
//   z_front = aft-most z of the glare shield (vertices above E.y, ahead of E.z, |x| < kShadowCentreX);
//   sill_y  = max(E.y, highest side vertex (|x| >= kShadowCentreX) between z_front and E.z + kShadowAftU);
//   half_x  = kShadowHalfFrac * max |x| of the cockpit vertices with z_front <= z <= E.z + kShadowAftU;  z_back = max z of the cockpit vertices.
// false (out untouched) when no glare shield / side vertex exists or the box is degenerate (e.g. zoefk). INFERENCE: a single horizontal opening plane.
inline constexpr float kShadowCentreX = 0.5f, kShadowAftU = 2.5f, kShadowHalfFrac = 0.9f, kShadowSoftU = 0.25f;
bool cockpit_shadow_box(const ModelData&, const float eye[3], float out[4]) noexcept;
// CFID shadow visibility (0..1) of a model-space point p for the model-space light L (normalised) and the box: 1 outside the box footprint or above the
// sill; else the light ray p + tL crosses the sill plane at c, d = min(half_x - |c.x|, c.z - z_front, z_back - c.z), vis = smoothstep(-soft, soft, d);
// L.y <= 1e-3 -> 0 (sun below the opening plane). Mirrored operation for operation by ps_model.
float shadow_visibility(const float box[4], float soft, const float L[3], const float p[3]) noexcept;
bool build_mesh(const ModelData&, const Options&, Mesh&, std::string& why);
// COCKPIT-4: the animated vertex range posed for `in` (out = anim_vertices corners, positions and normals rotated). apply_pose copies it into a mesh (tests/CPU).
void pose_anim(const Mesh&, const AnimInputs&, std::vector<GpuVertex>& out);
void apply_pose(Mesh&, const std::vector<GpuVertex>& posed);
// STORES-RT: the station of a baked weapon part name ("<anything>/store<digits>/<rest>" -> digits); false for every other name.
bool store_station_of_name(const std::string& name, uint32_t& station) noexcept;
// STORES-RT: `posed` = the animated range (anim_vertices corners, rest or posed). Every store piece whose bit is set in `hidden` (bit i = store_stations[i])
// collapses to its pivot (zero-area triangles: nothing is rasterised; the range keeps its size, so the one per-job upload is unchanged). Returns the number of
// vertices collapsed. A mask bit without a piece, or a `posed` of the wrong size, changes nothing.
uint32_t apply_store_mask(const Mesh&, uint32_t hidden, std::vector<GpuVertex>& posed);
// The rest range of a mesh (what the GPU already holds) with `hidden` applied; for jobs that do not pose (anim:0).
void pose_stores_only(const Mesh&, uint32_t hidden, std::vector<GpuVertex>& out);
// SP-STORES: the station of a baked special-weapon part name ("<anything>/sp<digits>/<rest>" -> digits); false for every other name (store/tank/p0 names included).
bool sp_station_of_name(const std::string& name, uint32_t& station) noexcept;
// SP-STORES: apply_store_mask for the special-weapon pieces: bit i of `hidden` = sp_stations[i]. The two masks are independent (apply both on the same range).
uint32_t apply_sp_mask(const Mesh&, uint32_t hidden, std::vector<GpuVertex>& posed);
// Both masks on the rest range (jobs that do not pose, anim:0).
void pose_stores_only(const Mesh&, uint32_t hidden, uint32_t sp_hidden, std::vector<GpuVertex>& out);

// COCKPIT-4 attitude -> pilot commands. The aircraft axes of consecutive E6 samples give the body angular velocity (R_prev^T R_cur, axis-angle / dt);
// pitch = w_x / pitch_full, roll = -w_z / roll_full (right roll), yaw = -w_y / yaw_full (nose right), throttle = 0.5 + 0.5 * along-track acceleration / accel_full;
// each smoothed with a first-order lag (tau). APPROXIMATION: the game's stick/throttle are not read (no such memory source is known); the rates come from the
// attitude the game flew, so the stick shows what a pilot WOULD have commanded. Full-scale values from D-300/D-306 (1 s roll tap ~145 deg, 1.2 s pitch ~29 deg).
struct AnimTuning { float pitch_full = 0.61f, roll_full = 2.62f, yaw_full = 0.26f, accel_full = 25.0f, tau = 0.12f, max_dt = 0.5f; };   // rad/s, u/s^2, s
struct AnimState { bool have = false; double t = 0; float axes[9] = {}; float pos[3] = {}; float speed = 0; bool have_speed = false; AnimInputs in; uint64_t updates = 0, resets = 0; };
// right/up/fwd: the aircraft (cockpit camera) axes in stage space, pos: its position (world units), t_s: seconds. Returns the smoothed inputs (state.in).
// A first sample, a time step <= 0 or > max_dt or a non-finite input restarts the differentiation (inputs decay toward neutral only through the lag).
const AnimInputs& anim_update(AnimState&, const float right[3], const float up[3], const float fwd[3], const float pos[3], double t_s,
    const AnimTuning& = AnimTuning{}) noexcept;

// ----------------------------------------------------------------------------------------------------------------- per-eye camera
// The submitted eye (OpenXR LOCAL pose quaternion (x,y,z,w), position in metres, FOV angles left,right,up,down in radians) -> model space:
//   rel = Ry(-yaw)(pos - anchor)                       (anchor = the recentered seat origin, yaw = its heading; the cockpit frame)
//   eye_model = pvb_eye + upm * rel                    (axes: +X right, +Y up, -Z nose: identical to the anchor frame)
//   R = Ry(-yaw) * R_eye                               (head orientation, full 6DoF; NO game look/camera enters)
//   clip = P(asymmetric FOV, reversed-Z infinite, near) * V(R, eye_model) * (p, 1)
// m = the 4x4 model->clip matrix, rows (clip_i = dot(m[4i..4i+3], (p,1))). false for non-finite/degenerate input.
struct EyeCam { float m[16]; float eye[3]; float right[3], up[3], back[3]; };
bool eye_camera(const float quat[4], const float pos[3], const float fov[4], const float anchor[3], float yaw, const float pvb_eye[3], float upm,
    float near_u, EyeCam& out) noexcept;

// SKIN-REFL: the game's world axes (stage frame, game heading convention, +Y up) expressed in MODEL space: n_world = (dot(n, x), dot(n, y), dot(n, z)).
struct ReflBasis { float x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z[3] = {0, 0, 1}; };
// From the camera axes in stage space (right/up/fwd, as stage_light_in_model) and the terrain yaw convention (the baked heading convention -> stage frame:
// heading' = yaw_sign * heading + yaw_offset, as build_env_texture rotates the env directions). false (out untouched) for a degenerate input.
bool refl_basis(const float right[3], const float up[3], const float fwd[3], float yaw_sign, float yaw_offset_deg, ReflBasis& out) noexcept;
// Root constants of the model pass (b0), shared by the HLSL and the CPU reference. 32 DWORDs (COCKPIT-3 added the light tint).
struct Constants {
    float m[16];
    float light[3]; float ambient;       // light: normalised direction in MODEL space
    float gain, exposure; uint32_t tonemap; float cut;   // tonemap 0 = clamp, 1 = Reinhard (xr_stereo::Tonemap)
    uint32_t base_vertex, pass;          // pass 0 opaque (alpha test), 1 blend
    uint32_t flags;                      // kCFlag* (COCKPIT-3)
    uint32_t pad;
    float tint[3];                       // COCKPIT-3: per-channel light colour (the stage's ambient + sun colour, mean 1; 1,1,1 without a stage)
    float shs;                           // CFID: shadow penumbra half-width (model units; was pad2)
    // COCKPIT-4 canopy glass (pass 2): eye = model-space eye (Fresnel view vector), trans = LINEAR transmission (tint^2.2), sky = reflection colour (DISPLAY),
    // refl = Fresnel scale, dirt = veil, f0 = Fresnel F0.
    float eye[3]; float refl;
    float trans[3]; float dirt;
    float sky[3]; float f0;
    // SKIN-REFL (kCFlagRefl): rx/ry/rz = ReflBasis, refl_k = sreflk
    float rx[3]; float refl_k;
    float ry[3]; float pad4;
    float rz[3]; float pad5;
    float shb[4];                        // CFID: Mesh::shadow_box (sill_y, half_x, z_front, z_back)
};
inline constexpr uint32_t kCFlagDisplay = 1;   // shade in the display domain, then the exact sRGB EOTF (the RTV encodes once): output = the game's display colour
inline constexpr uint32_t kCFlagRefl = 2;      // SKIN-REFL: add the stage reflection map on kMatTexAlphaNotCov fragments of the opaque pass (needs the t5 map)
inline constexpr uint32_t kCFlagFid = 4;       // CFID: engine lit-branch sun response 2 (n.L)^3 on kMatTexAlphaNotCov (exterior skin) fragments
inline constexpr uint32_t kCFlagShadow = 8;    // CFID: cockpit-rim sun shadow on non-skin fragments inside the shb footprint
inline constexpr float kFidLightClamp = 1.99f; // DrawCPT_v: min(colour * light, 0x3FFFE000 = 1.99)
static_assert(sizeof(Constants) == 60 * 4, "Constants are 60 root constants (CFID; root signature 61 of 64 DWORDs)");
inline constexpr unsigned kConstantDwords = 60;
inline constexpr float kReflMaskGain = 10.0f;   // DrawCPT_v: colour = saturate(10 * n'.y)
inline constexpr uint32_t kReflMaxDim = 64;     // the stage maps are 32x32
inline constexpr float kGlassF0 = 0.04f, kGlassGamma = 2.2f;   // glass Fresnel F0; display->linear transmission exponent (sRGB ~ gamma 2.2 in the sky's range)
inline constexpr float kDefaultSky[3] = {0.62f, 0.68f, 0.74f};   // reflection colour (display) without a stage env section (INFERENCE: a pale blue-grey sky)
inline constexpr float kLight[3] = {0.35f, 0.8f, 0.45f};   // model_raster.py Raster.shade default light (un-normalised), INFERENCE (game lighting unknown)
// light_model: the light direction in MODEL space (normalised here), nullptr = kLight.
// tint: per-channel light colour (stage_tint), nullptr = white.
// sky: the canopy reflection colour (display, e.g. the stage's sky stop 1), nullptr = kDefaultSky.
// refl: SKIN-REFL basis of this camera; kCFlagRefl is set only with o.skin_refl and a finite basis (the backend clears it again when it holds no map).
// CFID: kCFlagFid = o.fidelity; kCFlagShadow = o.fidelity and a finite shadow_box (Mesh::shadow_box when Mesh::shadow, else nullptr).
Constants make_constants(const EyeCam&, const Options&, float exposure, uint32_t tonemap, const float* light_model = nullptr, const float* tint = nullptr,
    const float* sky = nullptr, const ReflBasis* refl = nullptr, const float* shadow_box = nullptr) noexcept;
// SKIN-REFL per-vertex term (DrawCPT_v label 143, mirrored by vs_model): n_world = basis * n (no normalisation, like the engine), out = (u, v, mask) with
// u = saturate(0.5 * n_world.x + 0.5), v = saturate(0.5 * n_world.z + 0.5), mask = saturate(kReflMaskGain * n_world.y). Interpolated per fragment.
void refl_vertex(const Constants&, const float n[3], float out[3]) noexcept;
// COCKPIT-3: the light colour of a stage: (ambient + sun colour) per channel divided by its mean (so the overall level stays the gain's), clamped to [0.5, 1.5].
// m02a (ambient 110,121,105 + sun 83,83,68)/255 -> (1.016, 1.074, 0.911): the game's cockpit is slightly green-yellow, measured B/G 0.82 (section 10).
bool stage_tint(const float ambient[3], const float light_col[3], float out[3]) noexcept;
// COCKPIT-3: the stage light (stage-space direction, e.g. WORLD-2 env P[1]) in the cockpit/model frame of a camera whose axes in stage space are right/up/fwd
// (model +X = right, +Y = up, -Z = fwd). false (out untouched) for a degenerate input.
bool stage_light_in_model(const float l_stage[3], const float right[3], const float up[3], const float fwd[3], float out[3]) noexcept;
float srgb_to_linear(float display) noexcept;   // exact sRGB EOTF (mirrors cockpit_model.hlsl and xr_stereo_blit t_srgb_to_linear)

// ----------------------------------------------------------------------------------------------------------------- CPU reference
// Premultiplied linear RGBA over whatever `rgba` already holds (the composited eye image), reversed-Z depth (cleared to 0 by the caller).
// Same rules as the HLSL pass: back faces culled (CCW front in NDC), opaque draw then blend draw, GREATER depth test, the blend draw
// does not write depth, alpha test (texture alpha < cut) only on kMatAlphaTest corners, wrap bilinear texture sampling at texel centres.
struct Target { uint32_t w = 0, h = 0; std::vector<float> rgba, depth; };
// SKIN-REFL: refl_basis/refl_map as make_constants / the backend's t5 map; a null map clears kCFlagRefl (the backend's rule).
void render_reference(const Mesh&, const Options&, float exposure, uint32_t tonemap, const EyeCam&, Target&, const float* light_model = nullptr,
    const float* tint = nullptr, const float* sky = nullptr, const ReflBasis* refl_basis = nullptr, const Texture* refl_map = nullptr);
// COCKPIT-4 canopy glass fragment (pass 2; mirrors ps_glass): p = model-space position, n = normal. add = premultiplied linear colour added, trans = per-channel
// transmission (dst' = add + trans * dst), out_a = coverage for the alpha channel (1 - mean(trans)).
void shade_glass(const Constants&, const float p[3], const float n[3], float add[3], float trans[3], float& out_a) noexcept;
// The shaded colour of one fragment (shared by the rasteriser and the tests): tex sample (or white), vertex colour, normal -> premultiplied rgba.
// SKIN-REFL: refl_map + r (the interpolated refl_vertex output) add the reflection when kCFlagRefl, pass 0 and kMatTexAlphaNotCov (null = no term).
// CFID: pm = the model-space position (shadow term; null = no shadow term).
void shade_fragment(const Mesh&, const Constants&, uint16_t mat, const float uv[2], const float vcol[4], const float n[3], float out[4],
    const Texture* refl_map = nullptr, const float r[3] = nullptr, const float pm[3] = nullptr) noexcept;
// CFID sun response of one fragment (shared by shade_fragment and the tests): nl = n.L / |n| (0 for a degenerate normal).
float sun_term(const Constants&, uint16_t mat, float nl, const float pm[3]) noexcept;
void sample_bilinear_wrap(const Texture&, float u, float v, float out[4]) noexcept;   // 0..1 per channel

// ----------------------------------------------------------------------------------------------------------------- tests
// Synthetic model with asymmetric landmarks (no game data): eye (0, 3.5, -36); ahead: a red panel LEFT (-X), a green panel RIGHT (+X), a blue bar
// UP; a near yellow quad in front of a far cyan quad (depth order); a magenta quad whose front faces AWAY from the eye (culled); an alpha-tested
// textured quad with a transparent hole; a glass quad; an exterior "tail" behind the eye; two textures (8x8 checker with a hole, 4x4 skin).
ModelData make_synthetic_model();
// COCKPIT-4: the synthetic model + (10) a canopy glass quad 4 units ahead, 5 wide, facing the eye (MF_BLEND|MF_GLASS, vertex colour (200,200,200,40)) and
// (11) an exterior right "elevator" plate (group 2, anim channel 0, stored hinge 20 deg, fitted rest 0, pivot (4, 1, -44), hinge axis +X, plate x 3..5,
// z -44..-43 aft of the hinge, facing up); flags include kZvrmAnimRest.
ModelData make_synthetic_model_c4();

} // namespace acz::cockpit
