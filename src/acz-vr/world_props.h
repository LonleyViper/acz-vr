#pragma once
// world_props.h -- WORLD-3: stage props (buildings, towers, bridges: .pdl -> pmmdl) and trees (.wad/.wpd -> <stage>_treeN) for the terrain pass.
// SDK-free (no Windows/D3D12 types): the .zvro loader (research/world-render-data/objects_baker.py), the xr_terrain_stage option extraction
// ("props:1;trees:1;..."), ground snapping, the spatial grid + per-frame selection (range culling, behind-the-eyes culling, instance cap) and the
// CPU REFERENCES the GPU pass mirrors (projection of the props pass, the visibility test and the prop shading of the composite).
// GPU side: xr_props_d3d12.{h,cpp} (pass) + xr_props.hlsl (pass shaders) + xr_props_composite.hlsli (merge into the terrain composite).
// Decode evidence and what is inference: docs/world-render/world3-impl.md.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "terrain_stage.h"

namespace acz::props {

// PERF (docs/world-render/perf-impl.md): v2 = v1 + header flag bit0 kZvroFlagSnapped: every instance y is ABSOLUTE, snapped at bake time
// (objects_baker.py --stage <zvrs>) against the stage whose payload sha256 starts with header bytes 68..83 (84..95 zero). A snapped set is only
// accepted with exactly that stage bake (snap_to_terrain returns false otherwise). v2 without the flag = v1 semantics; v1 files still load.
inline constexpr uint32_t kZvroVersion = 2, kZvroMinVersion = 1;
inline constexpr uint32_t kZvroFlagSnapped = 1u;
inline constexpr size_t kZvroHeader = 128, kZvroStageShaBytes = 16;
inline constexpr size_t kMaxFileBytes = 128u * 1024u * 1024u;
inline constexpr uint32_t kMaxModels = 4096, kMaxTextures = 64, kMaxTexSize = 256, kMaxInstances = 2'000'000;
inline constexpr uint32_t kKindProp = 0, kKindTree = 1;
inline constexpr uint32_t kInstGroundRelative = 1u;   // instance flag bit0: y is relative to the terrain (height_bilinear added at load)

enum class LoadStatus : uint8_t { ok = 0, io_error, too_small, too_large, bad_magic, bad_version, bad_header, bad_size, bad_hash, bad_content };
const char* status_name(LoadStatus) noexcept;

#pragma pack(push, 1)
struct Vertex { float pos[3]; float uv[2]; int16_t nrm[3]; uint16_t layer; uint8_t rgba[4]; };   // 32 bytes, the GPU vertex buffer layout
struct Model { uint32_t first_index, index_count, base_vertex, kind; float centre[3], radius; };  // 32 bytes
struct Instance { float pos[3]; float scale; uint32_t model, flags; };                          // 24 bytes
#pragma pack(pop)
static_assert(sizeof(Vertex) == 32 && sizeof(Model) == 32 && sizeof(Instance) == 24, "zvro record sizes");

struct PropSet {
    char id[9] = {};
    uint32_t flags = 0, texsize = 0, ntex = 0;
    std::vector<uint8_t> tex;           // ntex * texsize^2 * 4, RGBA8 straight alpha
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;          // relative to the model's base_vertex
    std::vector<Model> models;
    std::vector<Instance> inst;
    uint8_t payload_sha[32] = {};
    bool snapped = false;               // ground-relative instances were made absolute (snap_to_terrain)
    bool baked_snap = false;            // PERF: v2 kZvroFlagSnapped (y already absolute; bound to stage_sha)
    uint8_t stage_sha[kZvroStageShaBytes] = {};   // PERF: prefix of the .zvrs payload sha256 the bake snapped against (baked_snap only)
};
LoadStatus parse_props(const uint8_t* data, size_t size, PropSet& out) noexcept;   // all-or-nothing
LoadStatus load_props_file(const std::string& path, PropSet& out) noexcept;
// The layout objects_baker.py writes (tests): v1 unless baked_snap (then v2 + flag + stage sha prefix).
std::vector<uint8_t> serialize_props(const PropSet&);
std::string props_path(const std::string& stage_id, const std::string& dir_override = std::string());  // <stage dir>\<id>.zvro
// Ground-relative instances (trees) get y += terrain height_bilinear(x, z) of the SAME stage the ray march uses; done once after loading.
// PERF: a baked (v2 snapped) set is not touched; it is accepted only when its stage sha prefix equals the stage's payload sha (no CPU work).
// false = refused (baked against another stage bake, or no height field): the caller drops the props.
bool snap_to_terrain(PropSet&, const acz::terrain::StageData&) noexcept;
// PERF: what objects_baker.py --stage does, in C++ (tests, cross-check): snap at "bake time" and mark the set v2 snapped for `st`.
bool bake_snap(PropSet&, const acz::terrain::StageData&) noexcept;

// ------------------------------------------------------------------------------------------------ options (inside the xr_terrain_stage value)
struct Options {
    bool props = false, trees = false;      // props:1 / trees:1   (both default OFF)
    float tree_scale = 4.5f;                // treescale:<f>  trees drawn this much larger than the model (fitted to the step56 game window, INFERENCE)
    // treerange:<u> tree draw distance. WORLD-4: 16000 (was 12000): the step56 game window shows trees up to the 12-16 km band, and mapset +0x14 holds
    // 16000 (a draw distance candidate [I]); docs/world-render/world4-impl.md.
    float tree_range = 16000.0f;
    float prop_range = 58000.0f;            // proprange:<u>  prop draw distance (the game's haze end ~58000)
    uint32_t cap = 16384;                   // propcap:<n>    per-frame instance cap (nearest kept)
    bool dedupe = false;                    // propdedupe:1   WORLD-4: the producer skips replayed world records at a prop placement (world_dedupe.h)
    float dedupe_tol = 2.0f;                // dedupetol:<u>  match tolerance per axis (measured offset 0.49 units)
    // PROPS-SHADING propshade:game|w3 (default game; docs/world-render/props-shading.md). game = the game's own DrawACM shading of the stage props, read from
    // the live SrtData records of the .pdl buildings (+352.x = 0: the VS lighting branch is OFF, COLOR = vertex colour / 128) and the DrawACM_p shader
    // (texture x COLOR, then LINEAR fog toward the record fog colour, fog = w * 0.00553385 / 255): albedo (texture x vertex colour) with NO extra light and
    // NO look gain, linear fog over kPropFogEnd toward the same haze colour as the terrain. w3 = WORLD-3/4 exactly (stage light normalised to flat ground,
    // WORLD-4 ground gain, the terrain's exponential haze).
    bool shade_game = true;
    bool any() const noexcept { return props || trees; }
};
// PROPS-SHADING: the game's object fog end (units along the view): rec[+340] = -0.00553385 (0xBBB55555) with rec[+336] = 255 in every logged m02a building and
// aircraft record (headset test 17), fog = saturate(-w * rec[+340] / 255) => 255 / 0.00553385 = 46080. Other stages: unverified (the record carries it).
inline constexpr float kPropFogEnd = 46080.0f;
// Removes the WORLD-3/4 keys (props, trees, treescale, treerange, proprange, propcap, propdedupe, dedupetol, propshade; ':' or '=') from a "m02a;opt:val;..." value and returns the rest
// for acz::terrain::parse_stage_option. false + why for a bad value of one of OUR keys (the whole terrain is then refused like any bad option).
bool extract_options(const std::string& value, std::string& rest, Options& opt, std::string& why);

// ------------------------------------------------------------------------------------------------ spatial grid + per-frame selection
inline constexpr float kGridCell = 4096.0f;
inline constexpr int kGridDim = 128;               // 128 * 4096 = 524288 = the stage extent
// PERF: one CSR grid PER KIND (0 props, 1 trees): a frame scans only the cells within that kind's own range (+ its reach), and a disabled kind
// is never touched (trees:0 with 1.16M trees scans the ~100 props only). Locator-only models (no indices) are not entered at all.
struct Grid {
    std::vector<uint32_t> start[2];                // kGridDim^2 + 1 offsets into `items[kind]` (CSR)
    std::vector<uint32_t> items[2];                // instance indices, ascending inside a cell
    float max_reach[2] = {0.0f, 0.0f};             // largest |model centre offset (xz)| * scale + radius * scale of the kind (cell search margin)
};
void build_grid(const PropSet&, const Options&, Grid&);
struct DrawInst { float pos[3]; float scale; };    // 16 bytes: the per-instance vertex stream
struct Batch { uint32_t model, first, count; };
struct Selection {
    std::vector<DrawInst> inst;                    // sorted by model (batches index into it)
    std::vector<Batch> batches;
    uint32_t considered = 0, culled_range = 0, culled_behind = 0, capped = 0;
};
// eye: camera position (stage units); view[2]: the two eyes' centre view directions (unit). An instance is drawn when its bound sphere is within the
// kind's range of `eye` and not entirely behind BOTH eye planes; above `cap` the nearest `cap` are kept (deterministic: ties broken by index).
void select(const PropSet&, const Grid&, const Options&, const float eye[3], const float view[2][3], Selection&);

// ------------------------------------------------------------------------------------------------ the props pass projection (CPU reference)
// The terrain composite builds the pixel ray d = base + u*ux + v*vy (anchor frame, uv over the eye viewport) and turns it into stage space with
// right/up/fwd: dw = right*d.x + up*d.y + fwd*(-d.z). The props pass inverts exactly that: for a point P (relative to the eye) q = (P.right, P.up,
// -P.fwd), (a, b, c) = [ux | vy | base]^-1 q, u = a/c, v = b/c.  clip = (2a - c, c - 2b, kNearC, c): reversed-Z, depth = kNearC / c (GREATER test).
inline constexpr float kNearC = 1.0f;              // c >= kNearC (about 1 unit along the view) is drawn
bool make_clip(const float base[3], const float ux[3], const float vy[3], const float right[3], const float up[3], const float fwd[3], float m[16]) noexcept;
// p relative to the eye -> uv and c (c > 0 in front). false when behind.
bool project(const float m[16], const float p_rel[3], float& u, float& v, float& c) noexcept;

// ------------------------------------------------------------------------------------------------ composite merge (CPU reference of xr_props_composite.hlsli)
// The prop at distance tp along the pixel ray is shown when 0 < tp < far and it is nearer than the terrain hit of the SAME ray (terrain_trace_t / trace();
// t_hit = 3e38 on a miss).
bool prop_visible(float tp, float far_dist, bool terrain_hit, float t_hit) noexcept;
// Default (no WORLD-2 light/sky/cloud) prop colour, LINEAR like Trace::rgb: albedo (display domain) x (kAmbient + kDiffuse max(n.L, 0)) with n flipped to face
// the eye (view_dir = the pixel ray), saturated, hazed toward kHaze by haze_fraction(tp), then srgb_to_linear (WORLD-1 colour domain).
void prop_color(const float albedo[3], const float n[3], const float view_dir[3], float tp, float haze_l, float far_dist, float out_linear[3]) noexcept;
// PROPS-SHADING propshade:game (default), no WORLD-2 environment: albedo (display) unlit, fogged LINEARLY toward kHaze by prop_fog(tp), srgb_to_linear.
float prop_fog(float tp) noexcept;   // saturate(tp / kPropFogEnd) (the ray distance: rotation-invariant in a headset; = the game's view depth on the axis)
void prop_color_game(const float albedo[3], float tp, float out_linear[3]) noexcept;

// Runtime bundle (built by the proxy, borrowed by the backend for one record()).
struct Runtime {
    PropSet set;
    Grid grid;
    Options opt;
    uint64_t generation = 0;
};

// Synthetic set for tests: one box model (side `size`, one flat-colour texture layer) and the given instances.
// WORLD-4 propdedupe: the absolute positions (x, y, z per instance) of the kind-prop instances (trees excluded; call after snap_to_terrain).
std::vector<float> prop_placements(const PropSet&);

PropSet make_box_set(float size, const uint8_t rgba[4], const std::vector<Instance>& inst, uint32_t texsize = 8);

} // namespace acz::props
