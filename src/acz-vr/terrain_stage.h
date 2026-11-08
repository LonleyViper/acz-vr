#pragma once
// terrain_stage.h -- baked stage terrain (.zvrs, research/world-render-data/stage_baker.py) loader + CPU REFERENCE renderer.
// SDK-free (no Windows/D3D12 types). The CPU functions below are the reference the HLSL terrain pass in xr_stereo_blit.hlsl mirrors
// operation for operation (constants named identically, see the "terrain" block there); tests/acz-vr/terrain_offline_tests.cpp checks the
// loader, the sampling, the ray march and determinism, and tests/acz-vr/xr_composite_warp_harness.cpp compares the real shader to trace().
// Format: see stage_baker.py (header 256 bytes, SHA-256 of the payload). Everything here is INFERENCE-LEVEL for game fidelity (docs/world-render/
// terrain-render-impl.md): only the file handling and the maths are tested.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "world_scale.h"   // WORLD-SCALE

namespace acz::terrain {

inline constexpr uint32_t kZvrsVersion = 3;       // v2 adds the water section (D-299), v3 the optional environment section (WORLD-2); v1/v2 files still load
inline constexpr uint32_t kZvrsMinVersion = 1;
inline constexpr size_t kZvrsHeader = 256;
inline constexpr uint32_t kHDim = 513, kTGrid = 256, kTilePx = 64, kMaxTiles = 1024, kMaxObjects = 4096;
inline constexpr uint32_t kFlagSeaGuess = 1u, kFlagSeaPresent = 2u, kFlagWater = 4u;   // kFlagWater (v2): tile_water/water_key are meaningful
inline constexpr uint32_t kFlagEnv = 8u;          // v3: the environment section (sky/light/cloud/water parameters decoded from mapset/sph/skytex, docs/world-render/world2-impl.md)
// D-344 C1 (docs/world-render/stage-sweep.md sections 4/5): .mti tile-word bit 13 is the SEA tile flag. When at least kSeaMaskMinFrac of a stage's tile words carry it
// (the threshold of stage_baker.sea_rule()), the sea plane is drawn as water only on those tiles (StageData::sea_masked). Stages without bit 13 keep the old
// height-only rule (water wherever the ground is at or below the plane). Derived from the words at load time: no format change, no rebake.
inline constexpr uint16_t kTileSeaBit = 0x2000u;
inline constexpr float kSeaMaskMinFrac = 0.01f;
// ---- WORLD-2 environment section (v3). Parameters are float4 "texels" P[i] (indices below, baked by stage_baker.py from the game's per-stage files; colours /255
// in the game's DISPLAY domain, directions unit vectors in stage space in the game's heading convention: dir(a, e) = (-sin a cos e, sin e, -cos a cos e)).
inline constexpr uint32_t kEnvParams = 32, kEnvMap = 256, kEnvWave = 128, kEnvSprite = 32;
inline constexpr uint32_t kEnvSectionBytes = 8 + kEnvParams * 16 + kEnvMap * kEnvMap + kEnvWave * kEnvWave + 2 * kEnvSprite * kEnvSprite;
inline constexpr uint32_t kEnvTexW = 256, kEnvTexH = 1 + kEnvMap;   // RGBA32F: row 0 = P[0..31]; rows 1..256 = maps (r cloud, g wave tiled, b sprite grey, a sprite alpha)
enum EnvParam : uint32_t {
    kPMoon = 0,       // xyz MOON sprite direction (mapset +0xBC elev, +0xC0 heading; texture = skytex[4], a per-stage moon phase), w strength (+0xC4; 0 = no moon)
    kPLight = 1,      // xyz directional light = the sun (mapset +0x58 elev, +0x5C heading; INFERENCE: the sun disc is drawn there), w 1
    kPLightCol = 2,   // rgb light colour (mapset +0x68)
    kPAmbient = 3,    // rgb ambient (mapset +0x84)
    kPSky = 4,        // 4 + 4*row + stop: sky colours (sph +0x10.. rows 0..2), stop 0 = horizon .. 3 = high sky; row 0 = toward the sprite/light azimuth, 2 = away
    kPWater = 16,     // rgb water colour (mapset +0x1F4)
    kPWaterHi = 17,   // rgb water highlight colour (mapset +0x1F8)
    kPCloudAlt = 18,  // 4 cloud-layer altitudes (mapset +0x210 + 16 i)
    kPCloudOn = 19,   // 4 layer flags (mapset +0x21C + 16 i)
    kPCloudCol = 20,  // 20..23 cloud colours (mapset +0x260..+0x26C)
    kPSkyStops = 24,  // elevations (radians) of the 4 sky stops (FITTED to the step56 game shot, not data)
    kPMisc = 25,      // x sprite half-size (tan), y cloud map repeat (units), z wave map repeat (units), w cloud shadow strength (all INFERENCE)
    // WORLD-4 'game look' (option look:1; docs/world-render/world4-impl.md). NOT in the baked file (the baker leaves P[26..31] zero): build_env_texture writes
    // them from the kLook* constants only when Conventions::look is set, so look:0 leaves the texture (and every image) exactly as WORLD-2 made it.
    kPLook = 26,      // rgb ground gain (multiplies the land texel and the props albedo after lighting), w = 1 when look is on (the shader's switch)
    kPLookFog = 27,   // rgb fog colour F = the haze target looking steeply down (the low-sky colour below e_lo)
    kPLookSky = 28,   // x k (stop-1 colour scale), y e_lo, z e1, w e2 (radians): low sky = lerp(F, k*C1, smoothstep(e_lo, e1, el)); C1 -> C2 over e1..e2
    kPLookFar = 30,   // K00A-FIT3: rgb far-haze tint colour W, w f0 (0 = off): the haze target lerps toward W by a bump of the raw haze fraction (0 at f=0, 1 at f0, 0 at P[31].x)
    kPLookFar2 = 31,  // x f_end of the bump (K00A-FIT3)
    kPLookSky2 = 29,  // x e3 (radians): C2 -> C3 over e2..e3 (eased like WORLD-2's top segment); y K00A-FIT desat; z haze exponent (0 = off); w K00A-FIT2 props trim (> 0: the game-shaded props take look_gain x w; 0 = off)
    // LENS-FLARE (lens_flare.h; docs/world-render/lens-flare.md): NOT baked and outside P[0..31]: row-0 texel 32 of the env texture (zero = off).
    kPFlare = 32,     // x strength k (lflarek, clamped to 0..3), w = 1 when the flare is on (lflare:1 and k > 0)
    // 33, 34: WORLD-CLOUDS (world_clouds.h kPWc / kPWc2), written only with wcloud:1 (zero = off)
};
// Option bits (xr_terrain_stage sub-options sky:1 light:1 cloud:1 water:1 env:1); 0 = the default shading, unchanged. Shader t_bits = bits << kEnvBitShift
// (bits 4..7; bit 3 is WORLD-1's object occlusion).
inline constexpr uint32_t kEnvSky = 1u, kEnvLight = 2u, kEnvCloud = 4u, kEnvWater = 8u, kEnvAll = 15u, kEnvBitShift = 4u;
inline constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;

enum class LoadStatus : uint8_t {
    ok = 0, io_error, too_small, too_large, bad_magic, bad_version, bad_header, bad_size, bad_hash, bad_content,
};
const char* status_name(LoadStatus) noexcept;

struct StageData {
    char id[9] = {};
    char env_id[9] = {};   // WCLOUD-FIX: the stage whose environment this is when adopt_mission_env gave a takeoff stage its mission's (empty = id)
    uint32_t flags = 0, ntiles = 0, nobj = 0;
    float cell = 1024.0f, hstep = 80.0f, half = 262144.0f, sea_level = 0.0f;
    std::vector<uint8_t> heights;   // kHDim*kHDim, row = Z, col = X
    std::vector<uint16_t> words;    // kTGrid*kTGrid, low 10 bits = tile, higher = flags (bit 12 river: tile_water; bit 13 = kTileSeaBit)
    std::vector<uint8_t> tiles;     // ntiles * 64*64 palette indices
    uint8_t clut[1024] = {};        // RGBA
    std::vector<float> objects;     // nobj * 4 (x,y,z, model as float bits)
    uint8_t payload_sha[32] = {}, source_sha[32] = {};
    uint8_t hmax_byte = 0;
    // v2 (D-299): river water. Atlas tile t is a water tile when tile_water[t] != 0 (every word using it has tile flag bit 12); in such a tile the texels whose
    // palette index == water_key are WATER (the stage's flat 'river' key colour). Empty (v1 files / stages without rivers) = no river water.
    std::vector<uint8_t> tile_water;
    uint8_t water_key = 0;
    // v3 (WORLD-2): environment. env_params = kEnvParams*4 floats; maps as baked (cloud coverage 256x256, sea wave 128x128, sky sprite grey/alpha 32x32).
    std::vector<float> env_params;
    std::vector<uint8_t> env_cloud, env_wave, env_sprite_i, env_sprite_a;
    // D-344 C1: sea water only on kTileSeaBit tiles. NOT serialised: parse_stage sets it to sea_tiles_flagged(), apply_sea_convention re-derives it
    // (option seamask:0 clears it = the old height-only rule). Hand-built stages start false (old rule).
    bool sea_masked = false;
    // SKIN-REFL (docs/world-render/aircraft-shading-reflection.md section 7): the stage's reflection map (game file data/map/<stage>/<stage>_ref.gim, a 32x32
    // hemisphere map of the sky indexed by the world normal) from the SIDECAR <stage>_ref.zvrt next to the .zvrs (refl_map_path). NOT part of the .zvrs:
    // load_stage_file reads it after a successful parse; absent or malformed = empty (refl_status says why), the stage itself is unaffected.
    uint32_t refl_w = 0, refl_h = 0;
    std::vector<uint8_t> refl_rgba;   // RGBA8 straight, row 0 = v 0
    LoadStatus refl_status = LoadStatus::io_error;
    bool has_refl() const noexcept { return refl_w && refl_h && refl_rgba.size() == size_t(refl_w) * refl_h * 4u; }
    bool sea_present() const noexcept { return (flags & kFlagSeaPresent) != 0; }
    bool has_water() const noexcept { return (flags & kFlagWater) != 0 && tile_water.size() == ntiles; }
    bool has_env() const noexcept { return (flags & kFlagEnv) != 0 && env_params.size() == kEnvParams * 4; }
};

// Parse + validate a whole file image. All-or-nothing: on any failure `out` is left default and a status names the first problem.
LoadStatus parse_stage(const uint8_t* data, size_t size, StageData& out) noexcept;
// Read `path` (UTF-8, narrow API; plain fopen) and parse. Missing file -> io_error. SKIN-REFL: on success also the optional sidecar refl_map_path(path).
LoadStatus load_stage_file(const std::string& path, StageData& out) noexcept;
// SKIN-REFL sidecar .zvrt (research/world-render-data/refl_map.py): 48-byte header: "ZVRT", u32 version 1, u16 w, u16 h (1..kReflMapMaxDim), u32 kind
// (1 = stage reflection map), SHA-256 of the payload; payload = w*h RGBA8 (row 0 = v 0). Exact size. All-or-nothing (outputs untouched on failure).
inline constexpr uint32_t kReflMapVersion = 1, kReflMapKind = 1, kReflMapMaxDim = 64;
inline constexpr size_t kReflMapHeader = 48;
LoadStatus parse_refl_map(const uint8_t* data, size_t size, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba) noexcept;
std::vector<uint8_t> serialize_refl_map(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);   // tests
// "...\m02a.zvrs" -> "...\m02a_ref.zvrt" (any other path: <path>_ref.zvrt)
std::string refl_map_path(const std::string& stage_file_path);
// Serialise exactly the layout stage_baker.py writes (tests and the synthetic-stage builder; deterministic).
std::vector<uint8_t> serialize_stage(const StageData& s);
void sha256(const void* p, size_t n, uint8_t out[32]) noexcept;
std::string hex(const uint8_t* p, size_t n);
// "m02a" -> %LOCALAPPDATA%\acz-vr\stage\m02a.zvrs ; id must be [A-Za-z0-9_] 1..8 chars else empty.
std::string stage_path(const std::string& id, const std::string& dir_override = std::string());
bool valid_stage_id(const std::string& id) noexcept;

// ------------------------------------------------------------------------------------------------------------- conventions (NAMED PARAMETERS)
// Every convention the live calibration may have to change lives here (defaults = docs/world-render/terrain-calibration.md, PROVISIONAL) and is
// overridable from the xr_terrain_stage value ("m02a;yaw=<deg>;mirror=1;..." keys below). Nothing else in the code base hard-codes them.
struct Conventions {
    // heading h (E6 rot[1], radians) -> forward = (-sin h, 0, -cos h)  [right-handed, +X east, -Z north, Y up, positive yaw turns toward -X].
    float yaw_sign = 1.0f;            // option sgn=-1: flip the sign of the logged yaw
    float yaw_offset_deg = 0.0f;      // option yaw=<deg>: added to the heading (after the sign), degrees
    float pitch_sign = 1.0f;          // option psgn=-1 ; pitch positive = nose up
    // option rsgn=+1/-1. Default -1 (WORLD-1, docs/world-render/terrain-issues-test16.md section 2): the E6 roll is POSITIVE for a LEFT bank (the
    // aircraft up axis = up0*cos(r) - right0*sin(r), measured from campos - air on 173 test-16 samples, |roll| up to 3.05 rad). +1 rolled the terrain
    // the wrong way (headset test 16: "controls inverted", objects/markers off by twice the roll).
    float roll_sign = -1.0f;
    bool mirror_x = false;            // option mirror=1: negate the camera right axis (handedness flip)
    float alt_offset = 0.0f;          // option alt=<units>: added to the camera y (the logged y may be a fixed 4000, flagged)
    bool fixed_alt = false;           // option fixy=<units>: replace the camera y by this value
    float fixed_alt_value = 4000.0f;
    // WORLD-SCALE (docs/world-render/world-scale.md): head offset (metres, OpenXR, incl. the IPD) -> world units for EVERY world layer (terrain, props, trees,
    // clouds, markers and, via world_scale.h, the replayed world objects/smoke) = (upm: option, else the evidenced 5.0 u/m) * wscale. parse_stage_option
    // computes it; it is no longer the producer's gpu_copy_step5_units_per_metre (65 = the replayed cockpit's camera space: 13x too large for the world).
    float head_units_per_metre = 5.0f;
    bool upm_set = false;             // true only with an explicit upm: option
    float world_scale = 1.0f;         // option wscale:<0.25..4> multiplies the world stereo baseline/head translation (>1: world looks smaller)
    // WORLD-1 registration (one recovered camera for terrain, objects and markers):
    //   csrc:game (default) = orientation from the game camera the producer rendered THIS frame with (StereoFrame::game_cam_*; the same matrices the replayed
    //                         objects and the game's marker HUD use), position from the frame's DrawACM_v camera (game_eye) or the frame's E6 campos;
    //                         E6 Euler angles are the fallback (no game camera in the frame) and the cross-check (see game_cam_max_deg).
    //   csrc:e6             = the E6 Euler path only (the pre-WORLD-1 behaviour, with the roll fix).
    //   e6:frame (default)  = the E6 sample read at the frame's own first cockpit member (StereoFrame::e6_*); e6:latest = the newest Present-tick sample.
    bool cam_source_game = true;
    bool e6_frame = true;
    float game_cam_max_deg = 15.0f;   // option gmax:<deg>: a game camera further than this from the E6 frame is refused for that frame (counted, E6 used)
    // CAM-EPOCH (terrain_camera.h, e6-anchor-free.md section 10). cam_admit: the E6 sample is admitted only when fresh (cam_age_ms relative to the frame's
    // publication time, or now for e6:latest), of the current lock/stage epoch, finite/sane and correlated with the frame's recording generation; a
    // frame without an admissible own sample may use the Present-history sample nearest to its recording start within cam_window_ms (0 = no fallback),
    // else it takes the no-camera path. The struct default is false (the pre-CAM-EPOCH path, used by hand-built runtimes in tests); parse_stage_option
    // turns it on (option camadmit:0 turns it off) and the proxy turns it off for e6loc:probe (the previous feed exactly).
    bool cam_admit = false;
    float cam_age_ms = 250.0f;        // option camage:<ms> (20..2000)
    float cam_window_ms = 50.0f;      // option camwin:<ms> (0..200)
    bool cam_log = true;              // option camlog:0|1: the bounded camlag evidence bursts (only with cam_admit)
    bool occlude_objects = true;      // option occl:0/1 (WORLD-1 step 4): terrain nearer than a replayed object pixel hides it (needs the objects' depth fix band)
    // WORLD-1 haze (terrain-issues-test16.md section 4, fitted on the step56 level-flight game shot): f(t) = 1 - exp(-t / haze_l) toward the horizon colour,
    // no start distance; the march ends at `far` and the last 20% of it fade fully into the haze (no flat band). option fogl:<u> / far:<u> (fog1:<u> = far, legacy).
    float haze_l = 55000.0f, far_dist = 160000.0f;   // (not `far`: a Windows macro)
    float sea_override = 0.0f; bool sea_override_set = false; // option sea=<units>; sea=off disables the plane
    bool sea_off = false;
    bool sea_mask = true;             // option seamask:0|1 (D-344 C1, default 1): sea water only on bit-13 tiles where the stage has them; 0 = the old rule everywhere
    bool blend_window = false;        // option blend=1: terrain + the game's world window on top (A/B); default terrain only
    bool fixed_cam = false;           // option cam=x,y,z,yawdeg[,pitchdeg[,rolldeg]]: fixed camera (desktop/no-E6 test), degrees; PROPS-SHADING adds pitch/roll
    float cam[6] = {0, 0, 0, 0, 0, 0}; // (pitch/roll are E6-style: they go through camera_frame with pitch_sign/roll_sign like a live sample)
    // PROPS-SHADING option objdom:display|raw (default raw = unchanged): the game's REPLAYED world objects (DrawACM_v: ground units, the windmill, aircraft)
    // reach the eye composite as the game's DISPLAY values in the FP16 replay target and are written as linear into the sRGB RTV, i.e. sRGB-encoded twice
    // (D-311: the replayed cockpit equals srgb_encode(game window) on 7 landmarks; objects use the same target). display = decode those object-band pixels
    // once (t_srgb_to_linear) so they keep the game's contrast over our terrain. Needs the terrain and the objects depth fix (the object band).
    bool obj_display = false;
    uint32_t env_bits = 0;            // WORLD-2 options sky/light/cloud(s)/water = 1, env = all (kEnv*); 0 = legacy shading (default)
    // WORLD-4 option look:1 (default 0; needs sky:1 or env:1): the fitted game look (kLook* below): ground gain, haze length kLookHazeL (unless fogl: is given),
    // haze target = the low sky along the view ray's SIGNED elevation (fog colour F looking down, the horizon glow just above the horizon), reshaped sky
    // stops, moon x kLookMoon. look:0 = WORLD-2 exactly.
    bool look = false;
    // D-344 C2 option lookfog:stage|fit (default stage): the look:1 fog colour F. stage = the stage's own sph row-2 horizon colour P[kPSky + 8] (m02a (85,102,98),
    // inside the +-4/255 of the m02a fit); fit = the m02a-fitted constant kLookFog on every stage (the pre-D-344 look).
    bool look_fog_fit = false;
    // GPU-COST option q:0..2 (default 0 = the reference march): q1/q2 relax the far-field step and stop fully hazed descending rays early (xr_stereo_blit.hlsl
    // kTQ*; docs/world-render/gpu-cost.md for the measured cost and error). GPU only: the CPU reference trace() is the q0 march.
    uint32_t quality = 0;
    bool haze_l_set = false;          // fogl: was given explicitly (look:1 then keeps it)
    // EFFECTS-OUTSIDE (A) option gamecloud:0|1 (docs/world-render/effects-outside-window.md): replay the GAME's cloud billboards (effects PS 4caf6e2d with
    // depth GREATER_EQUAL) into the effects image or not. -1 = not given: 0 when our own clouds are drawn (cloud:1 / env:1 with an env section), else 1.
    int game_cloud = -1;
    // EFFECTS-OUTSIDE (B/C) option fxmem:0|1|2|3 (default 0). With 1 the proxy sets effects_smoke_capture_flag (effects_game_clouds.h): the producer captures
    // the missile-smoke records and logs fx-smoke-frame lines (fx_smoke.h; capture + log only, no replay); the proxy logs fx-memory;active=capture-log-only.
    // With 2 also effects_smoke_replay_flag: the per-eye replay DRY RUN (records prepared per eye on the CPU, fx-smoke-replay-plan lines; no GPU replay).
    // With 3 (EFFECTS-OUTSIDE (D), fx_census.h): the capture of 1 plus effects_census_flag: fx-census lines identifying explosion/tracer draws (log only).
    // With 4 (EFFECTS-OUTSIDE (C), fx_smoke_gpu.h): the capture + dry run of 2 plus effects_smoke_gpu_flag: the prepared eye copies are replayed on the GPU per
    // eye (eye smoke images composited over the objects) and the smoke leaves the window effects image (no census). The sprite memory (fx_memory.h) has no
    // runtime path yet. With 5 (EFFECTS-OUTSIDE (E)): 4 + the explosion PS 542a11d7 admitted into the window effects image (its coverage clone) + the
    // explosion sprites captured and planned per eye on the CPU (fx-sprite-plan lines; no eye replay yet). With 6 (EFFECTS-OUTSIDE (F), fx_sprite_gpu.h):
    // 5 + effects_sprite_gpu_flag: the planned sprite draws (explosion, indexed GREATER 4caf6e2d) replayed on the GPU per eye from re-projected vertex copies
    // into per-eye sprite images (composited after the smoke) and no longer replayed into the window effects image (docs section 11).
    int fx_memory = 0;
    uint32_t fx_sig = 0;   // EFFECTS-OUTSIDE (D) fxsig:<8 hex> = per-draw log filter for ONE pixel-shader signature (0 = off; docs effects-outside-window.md section 12)
    // EFFECTS-OUTSIDE (M) fxtrail:<s> (0 = off, default; else 1..30 s) and fxtmax:<u> (50..100000, default 1500), fxmem:6 only: the world-space memory of
    // the game's smoke puffs (fx_puff_trail.h; docs effects-outside-window.md section 19): a puff the game drew whole is kept at its world point and
    // re-emitted per eye (age fade over the life) after the game camera left it; fxtmax is the largest world edge kept (the cloud puffs stay out).
    float fx_trail_s = 0.0f, fx_trail_max_edge = 1500.0f;
    // (M3) fxtcls:near|size (default near: a new puff is stored only near a world object drawn that frame) and fxtnear:<u> (100..50000, default 2500).
    int fx_trail_cls = 0; float fx_trail_near = 2500.0f;
    // (M4, docs effects-outside-window.md section 20) fxcloud:<s> (0 = off, default; else 10..600 s, fxmem:6 only): the game's own cloud puff cards kept at
    // their world points and re-emitted per eye (the cloud class of fx_puff_trail.h, its own capacity); fxcfar:<u> (5000..200000, default 40000) the cache
    // radius (distance fade to it); fxtxkey:0|1 (default 1) a stored card may be drawn inside another key's host draw (same game PSO and heaps).
    float fx_cloud_s = 0.0f, fx_cloud_far = 40000.0f; bool fx_trail_xkey = true;
    // (M4c) fxtclip:off|fade|hide (or 0|1|2; default fade): a game puff card cut at the game-window border with no whole stored copy this frame.
    int fx_trail_clip = 1;
    // (M6, docs effects-outside-window.md section 23) fxatlas:0|1 (default 0; fxmem:6 with fxtrail / fxcloud): the stored cards are captured into our own texture
    // atlas and drawn from it in frames whose draws do not bind their texture (fx_puff_atlas.h). Recommended fxatlas:1 after a null-run check.
    bool fx_atlas = false;
    bool hud_rs_skip = true;   // HUD-RS-SKIP hudrs:0|1 (default 1): the HUD replay skips colour SUBTRACT/REV_SUBTRACT draws (sun flare; gpu_copy_step5.h hud_rs_skip)
    bool fx_add = false;       // EFFECTS-OUTSIDE (P) fxadd:0|1 (default 0; fxmem:6 only): the additive 4caf6e2d family and its point lists per eye (effects-outside-window.md 21)
    bool hud_late = false;     // HUD-LATE hudlate:0|1 (default 0): the HUD text drawn after the tone-map hand-off (radio subtitles) is replayed into the HUD image (hud_late.h)
    // TAKEOFF option takeoff:<stage id>|off (default k00a; docs/world-render/takeoff-stage.md): the airbase map of a mission's takeoff segment. The proxy loads
    // it next to the mission stage (skipped when it IS the mission stage or its file is missing) and TakeoffSelector picks it while the aircraft is parked on
    // its ground. Evidence (headset test 19, mission 02): the takeoff rolled 7 units above k00a's flat 1840-unit runway plateau, where m02a's ground is 480.
    char takeoff_id[9] = "k00a";      // empty = off
    // TAKEOFF-LOOK option toenv:mission|own (default mission; docs/world-render/takeoff-stage.md section 'look'): the takeoff stage is drawn with the MISSION
    // stage's environment (sky, haze/fog, light, clouds) instead of its own bake. Evidence (step83 R1b T10 game shot vs k00a): the game's airbase sky is blue
    // (97,109,133), k00a's baked sky is brown in all 12 stops (R > B, e.g. row 0 horizon (106,89,64)); m02a's has B > R.
    bool takeoff_env_own = false;
    // DTEX option dtex:0|1 (default 0; docs/world-render/detail-texture.md): load <stage>.zvdt (the game's per-stage <stage>_dt.gim, detail_baker.py) next to
    // the .zvrs and add its fine grain to the near land (kDt* below). dtex:0 / a missing or bad file = the atlas has no detail slices = every image unchanged.
    bool detail = false;
    // K00A-SNOW option tclut:csm1|raw (default csm1; docs/world-render/k00a-snow-clut.md): the terrain .mtc CLUT is stored in PS2 CSM1 order (index bits 3
    // and 4 swapped, as every 8-bit GIM in the game). Evidence: on all 39 stages the CLUT's luminance run is 4-35% smoother after the swap (k00a 564 -> 382);
    // m02a's river key index 40 resolves to entry 48 = (24,52,33) with the only alpha 0x40 (raw: red-brown 74,39,25), its 'red speckle' index 111 to (76,85,63).
    // raw = the pre-K00A-SNOW decode (palette entry = texel index), kept only for an A/B capture.
    bool clut_raw = false;
    // LENS-FLARE options lflare:0|1 and lflarek:<0..3> (docs/world-render/lens-flare.md): our own head-look lens flare in the eye compositor (needs the
    // terrain with sky:1 / env:1). The struct default is OFF (hand-built conventions in tests keep every image); parse_stage_option turns it ON, so the
    // live default is on and lflare:0 turns it off. build_env_texture writes P[kPFlare].
    bool lens_flare = false;
    float lens_flare_k = 1.0f;
    // WORLD-CLOUDS options (docs/world-render/world-clouds.md; world_clouds.h): wcloud:0|1 (default 0) = our own world-space puff field per eye in the
    // terrain ray (needs an environment: env:1 or any of sky/light/cloud/water, and a v3 stage). wcalt:<u> band centre altitude and wcthk:<u> band thickness
    // (default = the stage's measured puff band, world_clouds.h stage_band; an unmeasured stage: the cloud layer 0 altitude P[kPCloudAlt].x and 3000),
    // wcsize:<u> puff radius, wccell:<u> grid cell (>= 2.5 x wcsize; default max(7500, 2.5 x wcsize)), wcop:<0..1> opacity, wcfill:<0..1> the fraction of
    // cells holding a puff (WCLOUD-FIX),
    // wcdrop:<u> (fxmem:6 only) the world edge above which the GAME's GREATER 4caf6e2d puff quads leave the per-eye sprite replay (0 = keep them),
    // wcdiag:0|1 the read-only fx-cloud-puff census lines (world centres of the game's puff quads; fxmem:5/6).
    bool wcloud = false;
    float wc_alt = -1e9f;             // < -1e8 = the stage's measured band (stage_band), else its layer-0 altitude
    float wc_thick = -1.0f;           // < 0 = the stage's measured band thickness, else 3000
    float wc_size = 3000.0f, wc_cell = 7500.0f, wc_opacity = 0.9f, wc_fill = 0.8f, wc_drop_edge = 2000.0f;
    bool wc_cell_set = false;
    bool wc_tint = false;             // wcdiag:2 (WCLOUD-FIX2): the shader paints its puff coverage magenta (a one-dump proof that the GPU branch runs)         // wccell given (else parse_stage_option sets max(7500, 2.5 x wcsize))
    bool wc_diag = false;
};
// K00A-SNOW: the palette entry of texel index `idx` (CSM1: swap bits 3 and 4; raw: identity).
inline constexpr uint8_t clut_entry(uint8_t idx, bool raw = false) noexcept {
    return raw ? idx : uint8_t((idx & 0xE7u) | ((idx & 0x08u) << 1) | ((idx & 0x10u) >> 1));
}
// true = the producer must NOT replay the game's cloud sprites. `env_cloud_active`: our clouds are really drawn (cloud bit set AND the env texture built).
inline bool drop_game_clouds(const Conventions& c, bool env_cloud_active) noexcept {
    return c.game_cloud == 0 || (c.game_cloud < 0 && env_cloud_active);
}
// ---- WORLD-4 'game look' constants (FITTED, research/world-render-data/world4_fit.py; values +- the spread seen between the fits, docs/world-render/world4-impl.md).
// Evidence: three game-window shots of mission 02 (m02a) at registered E6 poses; cockpit-view pixels divided by the measured canopy glass (0.95, 0.94, 0.935).
inline constexpr float kLookGain[3] = {0.64f, 0.61f, 0.50f};            // +-0.06: raw CLUT texel -> the game's lit ground (PS2 vertex colour ~0x50 [I])
// TAKEOFF-LOOK: k00a (the airbase / snow stage) is NEUTRAL: kLookGain's blue cut (fitted on m02a's fields) turned its grey/white snow khaki-brown. Fit
// (step83 R1b T10 game shot, 5 luminance quantiles of the airbase terrain vs k00a's usage-weighted texels, game = texel x g x (1-f) + sky x f): neutral
// g = 0.54 (f = 0.22) rms 7.0/255; kLookGain on the same model rms 12.7/255. +-0.06 (quantiles, not registered pixels; one shot).
// SNOW-LOOK (docs/world-render/k00a-snow-clut.md section 5) supersedes that fit: the 0.54 was fitted with k00a's BROWN sph rows 0..2 / m02a's teal as the haze
// target and on airbase pixels that are props (the tclut:raw A/B of batch I left them bit-identical, so they are not terrain). Kept for the record only.
inline constexpr float kLookGainNeutral[3] = {0.54f, 0.54f, 0.54f};
// SNOW-LOOK: per-stage look:1 profile (stage-aware look). Every field is the stage's own data or a fit to the game's frames; stages not listed keep the
// m02a WORLD-4 look (kLookGain, the stage's own sky, no haze floor), so m02a is bit-identical.
//  * k00a (airbase, snow): sky = sph +0x40 (100,113,136), the blue-grey colour of k00a's sph rows 3..4 (rows 0..2, which the bake reads, are brown);
//    batch I game frame sky 5..25 deg = (98..105,107..113,129..133), reference (101,115,143); the game's sky is flat over that range, so every stop
//    and the fog F take it. Haze floor 0.15: the game's near airbase ground (57,53,52) = ours unfogged (49,45,35) lerped toward the sky colour by
//    0.157/0.118/0.168 (R/G/B), i.e. the PS2 fog never reaches 0. Gain 0.90 neutral: the game's snowy mountain (94,94,98) = snow texel (97,91,81) x a
//    + sky x f gives a = 0.587, f = 0.371 (G predicted 95), a = g x (1 - f) -> g = 0.93; 0.90 +-0.1 (one crop, texel = the 256 snow tiles' mean).
//  * m01a, m15a (snow, texel means (154,156,156), (125,128,118)): NEUTRAL 0.61 = kLookGain's luminance; kLookGain's blue cut turned their white snow
//    (154,156,156) khaki (99,95,78); their own sph skies are already blue (B > R). No game capture of them yet [I].
struct StageLook {
    float gain[3];
    bool sky_set;        // true: every sky stop + the fog F = sky (look:1 only)
    float sky[3];        // display domain
    float haze_floor;    // haze fraction at distance 0 (look:1 only): f' = floor + (1 - floor) * f^haze_pow
    float desat;         // K00A-FIT: lerp(texel, luminance, desat) before the gain (0 = none): the game turns k00a's warm texels neutral
    float haze_pow;      // K00A-FIT: haze fraction exponent (0 = none = 1): < 1 hazes the middle distance sooner
    float props_gain;    // K00A-FIT2: the propshade:game props get look_gain x this (P[kPLookSky2].w); 0 = unchanged (no look gain on props)
    float far_tint[3];   // K00A-FIT3: far-haze tint colour W (display domain)
    float far_f0;        // K00A-FIT3: raw haze fraction of full tint (0 = off)
    float far_f1;        // K00A-FIT3: raw haze fraction where the tint is gone again (the far terrain must meet the sky)
};
// K00A-FIT (batch Q, docs/world-render/k00a-snow-clut.md section 6) refits k00a to the K3 pair (game vs ours at the takeoff, registered crops): the old
// neutral 0.90 gain + 0.15 floor left the ground warm (R-B +9, game +4) and the ridges 15-20% dark and 8-13 too blue. Model (CPU reference on the real bake):
// display = lerp(g x lerp(c, lum(c), desat), F, floor + (1 - floor) f^pow), g = 1.12, desat = 0.60, floor = 0, pow = 0.60, F = the sky (100,113,136).
inline constexpr float kLookGainSnow = 1.12f, kLookGainSnowFit = 0.61f;
// K00A-FIT2 (docs/world-render/k00a-snow-clut.md section 7): fitted on BOTH live pairs (step113, step117). The near ground in the frames is the PROPS (they share the
// terrain's unit colour (50,43,33)), mid mountains are ~12600 units (haze f 0.2). Per-channel gain (warm texel -> game's neutral), desat 0.80, floor 0, pow 0.56.
inline constexpr float kK00aGain[3] = {1.30f, 1.18f, 1.15f};
inline constexpr float kK00aPropsGain = 0.92f;   // props sit on the linear 46080 fog (f 0.0063 at 290 units vs the terrain's 0.003): trims the near ground to the game's 58,53,52
inline constexpr float kK00aDesat = 0.80f, kK00aHazePow = 0.56f;
inline constexpr float kK00aSky[3] = {100 / 255.0f, 113 / 255.0f, 136 / 255.0f};   // sph k00a +0x40 (= +0x44, +0x50)
inline constexpr float kK00aHazeFloor = 0.0f;                                      // K00A-FIT: the floor is replaced by desat + the haze exponent (was 0.15)
// K00A-FIT3 (section 8): after FIT2 the live mountains (83,87,100) were still ~16% low in red against the game (97,96,101), i.e. too blue: the game's far snow is hazed
// toward a BRIGHT NEUTRAL (first W (131,131,134) gave live (92,93,99) vs game (99,99,104): the live pipeline responds ~0.75x the CPU haze, so W was raised to (154,149,151)), not toward the blue sky colour. A distance dependent haze colour:
// hz' = lerp(hz, W, bump(raw haze f)), bump = f/f0 up to f0 = 0.20 (the 10-20k mountain bin), back to 0 at f1 = 0.70 so the far horizon still meets the sky. Near ground
// (props f 0.0063) gets ~3% of W, sky and all other stages are untouched (P[30].w = 0).
inline constexpr float kK00aFarTint[3] = {154 / 255.0f, 149 / 255.0f, 151 / 255.0f};
inline constexpr float kK00aFarF0 = 0.20f, kK00aFarF1 = 0.70f;
StageLook stage_look(const char* stage_id) noexcept;
// The look:1 ground gain of a stage: stage_look(id).gain (kLookGain for every stage without a profile; m02a unchanged).
void look_gain_for(const char* stage_id, float out[3]) noexcept;
// TAKEOFF-LOOK: the takeoff stage takes the mission stage's environment (params + maps + the env flag) unless c.takeoff_env_own or the mission has none.
// true = adopted (the caller rebuilds the env texture).
bool adopt_mission_env(StageData& takeoff, const StageData& mission, const Conventions& c);
inline constexpr float kLookFog[3] = {85 / 255.0f, 105 / 255.0f, 101 / 255.0f};   // +-4/255; used only with lookfog:fit (default: look_fog_color(), D-344 C2)
inline constexpr float kLookFogTol = 4.0f / 255.0f;                    // the m02a fit's tolerance on F (the stage colour must stay inside it on m02a)
inline constexpr float kLookHazeL = 21500.0f;                          // exponential haze length (the joint fit's minimum is flat over ~20000..35000)
inline constexpr float kLookK = 0.90f;                                 // +-0.02: horizon glow = 0.90 x the sph stop-1 colour
inline constexpr float kLookELoDeg = -3.5f, kLookE1Deg = 2.5f, kLookE2Deg = 8.5f, kLookE3Deg = 20.0f;   // +-0.5 / 0.25 / 0.5 / 3 deg (pre-V sky rows, rms 2.3/255)
inline constexpr float kLookMoon = 0.11f;                              // +-0.03: the game's crescent is ~10/255 above the sky, ours 93/255 at strength 0.85
inline constexpr float kE6YawField = 1; // index of the heading in the logged rot triple (pitch, yaw, roll): the first 'yaw' field is garbage
// "m02a;yaw=10;mirror=1" -> id + conventions. false (and `why`) for any unknown key, bad number, bad id (the stage is then NOT used).
bool parse_stage_option(const std::string& value, std::string& id, Conventions& conv, std::string& why);
// Apply the sea options (sea=off clears the plane, sea=<units> sets/guesses it; seamask:0|1 sets sea_masked) to a loaded stage.
// T4-SEA (headset 2026-10-29 m03k 'lighter rectangles'): with seamask:1 (default) and no sea= option, a GUESSED plane (kFlagSeaGuess) on a stage without
// bit-13 sea tiles is removed (stage_baker.sea_rule; the pre-STAGE-SWEEP bakes of m03/m06a/m10/m16/r01a/v05a still carry it); seamask:0 keeps the old rule.
void apply_sea_convention(StageData& s, const Conventions& c) noexcept;
// D-344 C1: true when at least kSeaMaskMinFrac of the tile words carry kTileSeaBit (false for an empty word grid).
bool sea_tiles_flagged(const StageData&) noexcept;
// D-344 C1: the water test of trace_env and the shader. The plane is present and the raw ground at (x,z) is at or below it (+kSeaEps), and, when
// s.sea_masked, the tile word at (x,z) carries kTileSeaBit.
bool sea_water_at(const StageData&, float x, float z, float hraw) noexcept;
// D-344 C2: the look:1 fog colour F written to P[kPLookFog] (rgb, display domain): the stage's sph row-2 horizon colour P[kPSky + 8], or kLookFog with
// lookfog:fit or a stage without an environment.
void look_fog_color(const StageData&, const Conventions&, float out[3]) noexcept;
// The camera frame: eye position + orthonormal right/up/forward in stage space, from the E6 camera (campos, rot pitch/yaw/roll radians).
struct CamFrame { float eye[3]; float right[3], up[3], fwd[3]; };
bool camera_frame(const float campos[3], float pitch, float yaw, float roll, const Conventions& c, CamFrame& out) noexcept;
// WORLD-1: the camera frame from the game's own recovered camera axes (screen right / up / view forward in world space, as gpu_copy_step5 decompose()
// returns them) and an eye position. Refused (false) unless the axes are finite, orthonormal within 1e-3 and right-handed like camera_frame
// (right x up = -fwd); mirror/alt/fixy conventions apply as in camera_frame.
bool camera_frame_axes(const float eye[3], const float right[3], const float up[3], const float fwd[3], const Conventions& c, CamFrame& out) noexcept;
// Largest angle (degrees) between corresponding axes of two frames.
float frame_angle_deg(const CamFrame& a, const CamFrame& b) noexcept;
// World eye of one XR eye: cam eye + head_units_per_metre * (right*o.x + up*o.y + fwd*(-o.z)), o = eye origin in the anchor frame (metres).
void eye_world(const CamFrame& f, const float o_anchor[3], float upm, float out[3]) noexcept;

// ------------------------------------------------------------------------------------------------------------- reference renderer
// Constants mirrored in xr_stereo_blit.hlsl (kT*).
// COLOUR DOMAIN (WORLD-1): every colour below and every shading step (texel x light, haze, sky, water) is in the game's DISPLAY domain (PS2 framebuffer
// values / 255, what a game-window screenshot shows), exactly as the PS2 composes it. trace() converts the final display colour ONCE to linear
// (srgb_to_linear) because the eye pipeline's sRGB RTV encodes once more; before WORLD-1 the display values were written as linear and so encoded
// twice (test-16 dump top row (167,194,232) = sRGB(0.379,0.533,0.805)).
inline constexpr unsigned kMarchSteps = 320, kBisect = 8;
// kStepGrow 0.008 (was 0.004): with the 128-unit minimum step the march now reaches ~190 000 units in 320 steps (far = 160 000)
inline constexpr float kStepMin = 128.0f, kStepGrow = 0.008f, kStepMax = 8192.0f, kStepFrac = 0.4f;
inline constexpr float kLodScale = 1.0f / 16000.0f;       // lod = max(0, log2(t * kLodScale))
inline constexpr float kAmbient = 0.55f, kDiffuse = 0.60f;
inline constexpr float kSun[3] = {-0.42f, 0.80f, 0.43f};  // un-normalised; normalised in the shader (named, flagged unknown)
// Horizon haze colour = the game's sky at elevation 0 (display (130,148,141), step56 pre-V shot) and the sky elevation table (display, degrees above the
// horizon, same shot, measured to 16.6 deg; the 30 deg entry is an EXTRAPOLATION [I], constant above it).
inline constexpr unsigned kSkyRows = 8;
inline constexpr float kSkyElevDeg[kSkyRows] = {0.0f, 5.6f, 7.9f, 10.2f, 12.4f, 14.5f, 16.6f, 30.0f};
inline constexpr float kSkyTable[kSkyRows][3] = {{130 / 255.0f, 148 / 255.0f, 141 / 255.0f}, {101 / 255.0f, 133 / 255.0f, 136 / 255.0f}, {90 / 255.0f, 127 / 255.0f, 134 / 255.0f},
                                                 {82 / 255.0f, 120 / 255.0f, 130 / 255.0f}, {77 / 255.0f, 115 / 255.0f, 125 / 255.0f}, {70 / 255.0f, 108 / 255.0f, 119 / 255.0f},
                                                 {64 / 255.0f, 103 / 255.0f, 115 / 255.0f}, {45 / 255.0f, 90 / 255.0f, 100 / 255.0f}};
inline constexpr float kHaze[3] = {130 / 255.0f, 148 / 255.0f, 141 / 255.0f};   // == kSkyTable[0]
inline constexpr float kFarFade = 0.8f;                   // the haze reaches 1 between kFarFade*far and far (smoothstep)
inline constexpr float kSeaDeep[3] = {0.07f, 0.20f, 0.30f};
inline constexpr float kRiverDeep[3] = {0.09f, 0.20f, 0.21f};   // D-299: grey-teal (the game's far river measures about (70,88,82)/255 through haze); INFERENCE
inline constexpr float kSeaEps = 0.5f;

// ---- DTEX: the per-stage DETAIL texture (docs/world-render/detail-texture.md). Every game stage ships data/map/<s>/<s>_dt.gim: 128x128, 4-bit, a grey CLUT of
// DARK values (0..~45 of 255, m11a all 0) with one CLUT alpha (0x40 k00a, 0x80 m02a, 0x04 v02a/v03a) [E]. Dark + alpha = an ADDITIVE GS pass (Cs*As + Cd), not a
// 2x modulate (a 2x modulate by values ~15/255 would turn the ground black) [I]. Applied here as a ZERO-MEAN additive term on the lit land colour (display domain),
// so the fitted average look (WORLD-4) is unchanged and only the grain is added:  col += fade(t) * D(x, z, lod),  D = amp * (rgb - mean(amp * rgb)) / 255,
// amp = (CLUT alpha / 0x80) * kDtGain. Constants FITTED on one game shot (step103 runN1 13-runway, k00a parked: the grain's autocorrelation and amplitude
// vs synthetic renders of k00a_dt at candidate scales; research/world-render-data/detail_fit.py): one 128-texel repeat = kDtRepeat units (48..64 fit
// equally; 64 = 1/32 of a 2048-unit tile, 0.5 units = 10 cm per texel), amplitude kDtGain (+-0.3). Fade and LOD are INFERENCE (the game's own LOD unknown).
inline constexpr uint32_t kDtDim = 128, kDtLevels = 8;   // 128 .. 1
inline constexpr uint32_t kDtSlices = 5;                 // GPU packing: atlas slices ntiles+0..3 = the 128x128 level as 4 64x64 quadrants (mip 0), +4 = levels 1..7 as mips 0..6
inline constexpr float kDtRepeat = 64.0f;                // world units per 128-texel repeat (FITTED: 48..64)
inline constexpr float kDtGain = 1.5f;                   // amplitude over the PS2 Cs*As term (FITTED, +-0.3, one shot)
inline constexpr float kDtFadeNear = 1500.0f, kDtFadeFar = 3000.0f;   // fade (smoothstep) of the term with the ray distance; none beyond kDtFadeFar (INFERENCE)
inline constexpr float kDtLodMul = 32.0f * float(kDtDim) / kDtRepeat; // detail lod = log2(t * kLodScale * kDtLodMul): the tile lod rule at the detail's texel size
inline constexpr size_t kZvdtHeader = 128, kZvdtBytes = kZvdtHeader + size_t(kDtDim) * kDtDim * 4;
struct DetailData {               // <stage>.zvdt: header 128 bytes ("ZVDT", u32 version 1, u32 w, u32 h, id[8], +32 payload sha256, +64 source .gim sha256), RGBA8 payload
    char id[9] = {};
    std::vector<uint8_t> rgba;    // 128*128*4: the decoded CLUT rgb + the RAW PS2 CLUT alpha (0..0x80 = 0..1)
    uint8_t payload_sha[32] = {}, source_sha[32] = {};
};
LoadStatus parse_detail(const uint8_t* data, size_t size, DetailData& out) noexcept;
LoadStatus load_detail_file(const std::string& path, DetailData& out) noexcept;
std::vector<uint8_t> serialize_detail(const DetailData&);
std::string detail_path(const std::string& id, const std::string& dir_override = std::string());   // ...\acz-vr\stage\<id>.zvdt
DetailData make_synthetic_detail(uint32_t seed = 1);       // tests: deterministic sparse bright streaks, alpha 0x40 (k00a-like)

struct Atlas {                    // RGBA8 tiles with a full mip chain (7 levels), what the GPU texture holds
    uint32_t ntiles = 0;
    std::vector<uint8_t> level[7]; // level L: ntiles * (64>>L)^2 * 4
    // DTEX: the encoded detail term, level L (128>>L)^2 * 4 bytes, rgb byte = clamp(round(128 + 255 * D)), alpha 255; decoded D = byte/255 - 128/255. Empty = no detail.
    std::vector<uint8_t> dt[kDtLevels];
    bool has_detail() const noexcept { return dt[0].size() == size_t(kDtDim) * kDtDim * 4; }
};
void build_atlas(const StageData&, Atlas&, bool clut_raw = false);   // K00A-SNOW: texel -> CLUT through clut_entry (CSM1 unless clut_raw)
// DTEX: encode `d` into a.dt (false and a.dt cleared for a malformed payload). The mips are box means of the float term, then quantised per level.
bool build_detail(const DetailData& d, Atlas& a);
// DTEX: the runtime hook of every stage load. dtex off -> false, why "off"; else loads detail_path(id) (must name the same stage), build_detail; why = the outcome.
bool attach_detail(const std::string& id, const Conventions& c, Atlas& a, std::string& why);
// DTEX: the faded detail term at the ground point (x, z) seen at ray distance t (display domain, per channel; 0 without detail / beyond kDtFadeFar).
void detail_term(const Atlas& a, float x, float z, float t, float out[3]) noexcept;
float height_bilinear(const StageData&, float x, float z) noexcept;     // world units (bilinear of the bytes * hstep), edge-clamped
float height_effective(const StageData&, float x, float z) noexcept;    // with the sea plane: max(h, sea)
// TAKEOFF (docs/world-render/takeoff-stage.md): which loaded stage the terrain draws, the mission stage or the takeoff airbase stage (k00a). Fed with the
// frame camera eye (stage units) of every fresh composite. ENTER the base only while ARMED (session start, or after a cut back near the first takeoff
// spot) when `enter` consecutive samples are parked (horizontal move <= still) at cockpit height over the base ground (band_lo..band_hi) AND clearly
// above the mission ground (>= mission_clear: the mission map cannot explain an aircraft standing there). LEAVE on a cut (one-sample move > jump: the
// game's switch from the airbase to the mission area) or when the eye is under the base ground (impossible on the base); leaving disarms until a cut.
struct TakeoffRule {
    float band_lo = -40.0f, band_hi = 200.0f;   // eye - base ground (test 19: cockpit eye 1851.9 over the 1840 plateau = +11.9)
    float mission_clear = 300.0f;               // eye - mission ground (test 19: +1372 over m02a)
    float still = 4.0f;                         // units per sample (parked / the first metres of the roll; flight is >= ~10)
    float jump = 20000.0f;                      // units per sample (test 19 cut: ~150000)
    float below = 60.0f;                        // eye this far under the base ground -> leave
    float rearm_radius = 30000.0f;              // a cut re-arms only within this horizontal distance of the first takeoff spot (a mission restart)
    uint32_t enter = 30;                        // consecutive qualifying samples (~0.5 s)
};
class TakeoffSelector {
public:
    enum class Event { none, enter, leave };
    explicit TakeoffSelector(const TakeoffRule& r = TakeoffRule{}) noexcept : r_(r) {}
    Event update(const float eye[3], const StageData& mission, const StageData& base) noexcept;
    bool base_active() const noexcept { return active_; }
    const char* reason() const noexcept { return reason_; }
    float base_ground() const noexcept { return hb_; }      // at the last sample
    float mission_ground() const noexcept { return hm_; }
    uint64_t samples = 0, enters = 0, leaves = 0, jumps = 0, refused = 0;   // refused: unusable sample / stage
private:
    TakeoffRule r_;
    bool have_prev_ = false, active_ = false, armed_ = true, have_spot_ = false;
    uint32_t run_ = 0;
    float prev_[3] = {}, spot_[2] = {}, hb_ = 0, hm_ = 0;
    const char* reason_ = "none";
};
void sky_color(const float d[3], float out[3]) noexcept;          // DISPLAY domain; elevation <= 0 -> kHaze
float srgb_to_linear(float x) noexcept;                            // exact sRGB EOTF (the inverse of the RTV encode)
float linear_to_srgb(float x) noexcept;
float haze_fraction(float t, float haze_l, float far_dist) noexcept;    // 1 - exp(-t/L) * (1 - smoothstep(kFarFade*far, far, t))
// rgb = LINEAR (what the eye shader outputs before exposure); display = the same colour in the display domain (= what a screenshot would show).
struct Trace { bool hit = false, water = false, river = false, sky = false; float t = 0, p[3] = {}, rgb[3] = {}, display[3] = {}; unsigned steps = 0; };
// o: eye (stage units), d: NORMALISED world direction. haze_l / far_dist from Conventions.
Trace trace(const StageData&, const Atlas&, const float o[3], const float d[3], float haze_l, float far_dist) noexcept;
// CLUT/atlas bilinear sample (trilinear over lod) of tile `id` at tile-uv (0..1), the GPU's SampleLevel on the RGBA array.
// out = rgb + coverage alpha (alpha 0 = river water texel, 1 = land; filtered like the colour).
void atlas_sample(const Atlas&, uint32_t id, float u, float v, float lod, float out[4]) noexcept;

// ---- WORLD-2 environment (CPU reference of the shader's t_env_* functions; constants mirrored as kTE* in xr_stereo_blit.hlsl)
inline constexpr float kCloudCovLo = 0.15f, kCloudCovHi = 0.85f, kCloudOpacity = 0.5f, kCloudBelowDark = 0.4f, kCloudBlur = 2.0f;   // 5-tap blur (texels) softens the binary map
inline constexpr float kSeaTint = 0.25f;   // sea base = lerp(the tile texel (the sea is painted in the tiles, m16n), the stage water colour, kSeaTint)
inline constexpr float kWaveAmp = 0.12f, kWaveFade = 20000.0f, kWaterF0 = 0.02f, kWaterSpecPow = 256.0f, kWaterSpec = 0.8f;
inline constexpr float kLightMin = 0.4f, kLightMax = 1.4f, kLightDenMinY = 0.05f;
// sun disc (radius ~0.45 deg, soft edge) + glow at the light direction; skipped when the moon sprite and the light coincide (night stages). All INFERENCE (no game shot shows the sun).
inline constexpr float kSunCos0 = 0.999954f, kSunCos1 = 0.999981f, kSunGlowPow = 300.0f, kSunGlow = 0.25f, kSunMoonSame = 0.99f;
// The RGBA32F environment texture the GPU holds (kEnvTexW x kEnvTexH, row-major float4). `conv` rotates the baked directions by the yaw convention
// (yaw_sign/yaw_offset, mirror) exactly like camera headings. Empty when the stage has no environment.
void build_env_texture(const StageData&, const Conventions&, std::vector<float>& tex);
struct EnvView { const float* tex = nullptr; uint32_t bits = 0; };   // tex = build_env_texture output; bits = kEnv* (0 = legacy)
void env_sky(const EnvView&, const float d[3], bool sprite, float out[3]) noexcept;
void env_haze(const EnvView&, const float d[3], float out[3]) noexcept;   // the sky colour at elevation 0 along d's azimuth (look:1: at d's signed elevation)
void look_gain(const EnvView&, float col[3]) noexcept;                    // WORLD-4 look:1 ground/props gain (no-op unless the texture carries the look)
void look_gain_props(const EnvView&, float col[3]) noexcept;               // K00A-FIT2 (CPU mirror of the propshade:game look in xr_props_composite.hlsli)
bool props_look_on(const EnvView&) noexcept;                              // K00A-FIT2: the propshade:game props take look_gain too (profile flag P[kPLookSky2].w)
void look_haze_tint(const EnvView&, float f_raw, float hz[3]) noexcept;   // K00A-FIT3: far-haze tint of the haze colour (raw haze fraction; no-op without P[30].w)
float look_haze(const EnvView&, float f) noexcept;                       // SNOW-LOOK: haze fraction f with the stage's floor (P[kPLookFog].w; look:1 only)
// trace() with the environment (bits == 0 or tex == nullptr: identical to trace()). Env colours are in the DISPLAY domain like WORLD-1's; with sky:1 the env sky
// replaces the measured sky table and the env horizon colour (along the view azimuth) replaces kHaze as the haze target.
Trace trace_env(const StageData&, const Atlas&, const EnvView&, const float o[3], const float d[3], float haze_l, float far_dist) noexcept;
// WORLD-CLOUDS (world_clouds.h; the shader's wc_accum / wc_apply): the puff field along o + t d (d normalised), t in [t0, t1], with the env texture's
// parameters (P[33..34] zero = nothing). wc_apply: rgb (LINEAR, what trace_env returns) -> decode(C + (1 - A) encode(rgb)); unchanged when A == 0.
struct WcAccum { float c[3] = {0, 0, 0}; float a = 0; unsigned cells = 0, puffs = 0; bool tint = false; };   // tint: wcdiag:2 (magenta coverage)
void world_clouds(const EnvView&, const float o[3], const float d[3], float t0, float t1, float haze_l, float far_dist, WcAccum& out) noexcept;
void world_clouds_apply(const WcAccum&, float rgb[3]) noexcept;
// WCLOUD-FIX2 telemetry (xr-stereo event=summary-wcloud): the CPU mirror of what the shader adds, on an nx x ny grid of one eye's rays (anchor-frame ray
// base + u ux + v vy, rotated into the stage by right/up/fwd as the shader does), over the trace_env terrain colour of each ray. dl = the display
// luminance change in 1/255 steps (what a viewer can see: >= 8 is plainly visible); with = rays with coverage > 0.05.
struct WcProbe { unsigned rays = 0, with = 0, vis8 = 0; float mean_a = 0, max_a = 0, mean_dl = 0, max_dl = 0; bool on = false, tint = false; float alt = 0, thick = 0; };
WcProbe world_clouds_probe(const StageData&, const Atlas&, const EnvView&, const float eye[3], const float base[3], const float ux[3], const float vy[3],
                           const float right[3], const float up[3], const float fwd[3], float haze_l, float far_dist, unsigned nx = 8, unsigned ny = 6) noexcept;
// Synthetic environment for tests (deterministic, all parameters plausible, clouds at `cloud_alt`).
void add_synthetic_env(StageData& s, float cloud_alt = 9000.0f);

// Synthetic stage for tests: smooth hills, 4 tiles, deterministic.
StageData make_synthetic_stage(uint32_t seed = 1, bool sea = false);

} // namespace acz::terrain
