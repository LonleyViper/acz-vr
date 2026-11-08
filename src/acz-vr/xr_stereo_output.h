#pragma once
// xr_stereo_output.h -- in-proxy CONSUMER of stereo_frame_source.h (D-230, D-236).
//
// Turns published StereoFrames (the producer's per-eye FP16 cockpit replays on
// the game's device) into OpenXR layers per begun frame, as an
// xr_owner::FrameConsumer on the XR owner thread. Default OFF: nothing here runs
// unless the proxy wiring (xr_stereo_proxy.cpp) sees the keys xr_stereo_output +
// gpu_copy_early_device_hook + gpu_copy_step5_continuous.
//
// Revision 3 (D-235 live result, D-236 interface):
//   * layer 0 (optional): a WORLD QUAD (XrCompositionLayerQuad, both eyes) carrying
//     the frame's `world` snapshot (sky/terrain before the cockpit), world-locked
//     in LOCAL at the neutral head position, D metres ahead (-Z), sized to subtend
//     exactly the cockpit lens FOV;
//   * layer 1: the cockpit PROJECTION layer, two views, FOV = atan(lens_tan) when
//     lens_valid (J-cockpit-fov-consumer: the frame's own per-eye cockpit_fov when
//     cockpit_fov_valid), full-rect mapping into a swapchain with the SOURCE aspect, alpha
//     from the replay depth (depth > 0 = covered), premultiplied
//     (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, no UNPREMULTIPLIED bit);
//   * pacing: after the first StereoFrame the last submitted layers are
//     re-submitted (no new acquire; the compositor reprojects) while their age is
//     below 1 s; only then zero layers. HUD is not included (producer pending).
//
// J-hud-consumer (key xr_hud_layer, default OFF; see J-hud-consumer-findings.md):
//   * layer 2 (optional): a HUD QUAD from StereoFrame::hud (mono, screen space,
//     premultiplied) blitted into a 4th swapchain (target kHud, world-quad extent)
//     in the SAME recording/fence/completion transaction; submitted AFTER the
//     cockpit projection with SOURCE_ALPHA (premultiplied). hud_mode world-quad
//     (default): exactly the world quad's pose/size; head-locked: VIEW-space fallback.
//   * Key OFF: no HUD swapchain, acquire, draw or layer; every line unchanged.
//
// Per-frame ledger (owner thread only; see J-xr-stereo-output-findings.md):
//   try_acquire_latest -> validate -> acquire+wait each target image (cockpit
//   eyes 0/1, world quad 2 when used) -> record ONE proxy-owned list (sources
//   RENDER_TARGET / DEPTH_WRITE -> PIXEL_SHADER_RESOURCE, full-rect draws,
//   sources restored; swapchain images stay RENDER_TARGET) -> Close -> queue
//   Wait(producer fence) unless CPU-observed complete -> ORIGINAL
//   ExecuteCommandLists -> Signal(private fence) -> CPU-observe completion ->
//   release images -> FrameSource::release -> plan the layers.
// Any failure or ambiguity disables the output for the session (sticky); GPU
// work that may still reference an image is quarantined.
#include "aircraft_auto.h"
#include "cockpit_model.h"
#include "store_tracker.h"   // STORES-RT: which baked weapon stations are still loaded
#include "hud_msl.h"         // MSL-HUD: the producer's HUD MSSL reader status (summary-stores)
#include "pov_fixes.h"
#include "stereo_frame_source.h"
#include "tactical.h"   // TACTICAL (xr_terrain_stage option tac:1)
#include "marker360.h" // MARKERS-360 (xr_terrain_stage option mk360:1|2)
#include "cam_lag.h"        // CAM-EPOCH: E6 sample vs the frame's game camera (diagnostic)
#include "frame_pacing.h"   // EXTRAP (xr_terrain_stage option extrap:1; docs/world-render/uncap.md)
#include "hud_menu.h"       // HUD-MENU (glass-pivot HUD scale, pause-menu mode; docs/world-render/hud-menu.md)
#include "view_mode.h"      // VIEW-MODE (xr_terrain_stage option pov:1; docs/world-render/pov.md)
#include "pov_render.h"     // POV-RENDER stage 1 (povc / povrig / povv / povtc; docs/world-render/other-povs.md)
#include "terrain_camera.h"
#include "terrain_stage.h"
#include "world_props.h"
#include "xr_owner.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace acz::xr_stereo {
using xr_owner::Gate;
using xr_owner::Op;
using xr_owner::Rc;

// ------------------------------------------------------- blit mapping (pure)
struct Rect { uint32_t x = 0, y = 0, w = 0, h = 0; };
struct BlitMapping {
    bool ok = false;
    Rect dst;          // destination viewport/scissor in swapchain pixels
    double scale = 0;  // destination pixels per source pixel (horizontal)
};
// Revision 1 mapping (kept as a pure helper, no longer used for the cockpit):
// aspect-preserving letterbox, centred, black outside.
BlitMapping letterbox(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) noexcept;
// Revision 3 mapping: the source fills the FULL destination image (no bars).
// With the swapchain_extent() policy below the aspect matches, so this is a
// pure scale (2560x1440 -> 2560x1440 is 1:1).
BlitMapping full_rect(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) noexcept;
// Source UV sampled for a destination pixel centre; false outside the rect.
bool source_uv(const BlitMapping&, uint32_t px, uint32_t py, double& u, double& v) noexcept;

// Swapchain extent policy (cockpit eyes and the world quad): keep the SOURCE
// aspect, width = min(src_w, cap_w, max_w), height = round(width*src_h/src_w);
// if the height exceeds max_h, height = max_h and width = round(max_h*src_w/src_h).
// The runtime's recommended view size (e.g. 1512x1680) describes the headset's
// own FOV, not the game lens, and is deliberately not used.
struct Extent { uint32_t w = 0, h = 0; };
struct VScreenHook;   // VSCREEN (xr_vscreen.h)
Extent swapchain_extent(uint32_t src_w, uint32_t src_h, uint32_t max_w, uint32_t max_h, uint32_t cap_w) noexcept;

// --------------------------------------------- lens / quad geometry (pure)
// Lens tan half-angles (left, right, up, down; all positive) -> OpenXR FOV:
// angleLeft = -atan(l), angleRight = atan(r), angleUp = atan(u), angleDown = -atan(d).
bool lens_ok(const float tan[4]) noexcept; // finite, each in (0, 10]
xr_owner::Fov fov_from_lens(const float tan[4]) noexcept;
// World quad in LOCAL: centre = neutral + (D*(r-l)/2, D*(u-d)/2, -D), identity
// orientation (faces +Z, i.e. the viewer looking along -Z), size (D*(l+r), D*(u+d)).
// It subtends exactly the lens FOV from `neutral`.
struct Quad { xr_owner::Pose pose; float width = 0, height = 0; };
Quad world_quad(const float neutral[3], const float tan[4], float distance_m) noexcept;
// J-quad-yaw (see J-quad-yaw-findings.md): the same quad turned about +Y by `yaw_rad` around
// `neutral` (the producer's seated-recentering heading yaw0, StereoFrame::neutral_yaw): offset
// o = (D*(r-l)/2, D*(u-d)/2, -D) is rotated, centre = neutral + Ry(yaw) o, orientation
// (0, sin(yaw/2), 0, cos(yaw/2)); the forward axis Ry(yaw)(0,0,-1) = (-sin yaw, 0, -cos yaw) is
// the cockpit forward in LOCAL. Size unchanged, so it still subtends exactly the lens FOV.
Quad world_quad(const float neutral[3], const float tan[4], float distance_m, float yaw_rad) noexcept;
// xr_look_drive_keys: the same quad turned by Ry(yaw_rad) Rx(pitch_rad) (yaw = producer yaw0 + the rendered look yaw, pitch = the rendered look
// pitch): centre = neutral + Ry(yaw) Rx(pitch) o, orientation = Ry(yaw) Rx(pitch). pitch_rad == 0 is exactly world_quad(..., yaw_rad).
Quad world_quad(const float neutral[3], const float tan[4], float distance_m, float yaw_rad, float pitch_rad) noexcept;
// A StereoFrame neutral usable as the quad anchor: neutral_valid, finite, |pos| <= 1000 m, |yaw| <= 4 rad.
bool neutral_ok(const stereo::StereoFrame&) noexcept;

// J-world-in-projection (key xr_world_in_projection; see J-world-in-projection-findings.md): the
// world image is composited INTO each cockpit eye image instead of being submitted as a quad
// layer. Per eye the CPU packs the eye's view ray into the 17 floats below (root constants after
// the five existing ones; the HLSL cbuffer mirrors this order exactly), and ps_main mode 2 does
//   d = base + uv.x*ux + uv.y*vy                       (anchor-frame ray direction of pixel uv, uv.y = 0 at the TOP)
//   hit iff d.z < -kWorldRayEps and s = (-dist - origin.z)/d.z > 0
//   h = origin + s*d ; wuv = (h.x*au + bu, h.y*av + bv) ; hit iff 0 <= wuv <= 1 (both axes)
// i.e. the intersection with the plane z = -dist of the anchor frame (the anchor turned by the
// producer yaw0), which is exactly the plane of world_quad(anchor, lens, dist, yaw0); wuv is the
// world image uv inside that quad (linear in tan, as the lens render). Anchor frame: local = Ry(-yaw)(p - anchor).
struct WorldComposite {
    float origin[3];  // eye position in the anchor frame
    float base[3];    // direction at uv (0,0): R*(tanLeft, tanUp, -1)
    float dist;       // plane distance (the world quad distance), m
    float ux[3];      // R*(tanRight - tanLeft, 0, 0)
    float au;         // 1/(dist*(l+r))
    float vy[3];      // R*(0, tanDown - tanUp, 0)
    float bu;         // l/(l+r)
    float av;         // -1/(dist*(u+d))
    float bv;         // u/(u+d)
};
static_assert(sizeof(WorldComposite) == 17 * sizeof(float), "WorldComposite is the 17 root constants after b0[0..4]");
constexpr float kWorldRayEps = 1e-6f;
// kBlitConstants (b0: use_depth, tonemap, exposure, mode, feather, WorldComposite[17], fade, extend) is declared below with the hold/extend API.
// `eye` = the pose and FOV the projection layer submits for this eye (LOCAL); anchor/yaw as world_quad().
// False for non-finite/degenerate input (never writes partial output meaning).
// pitch_rad (xr_look_drive_keys, default 0 = exactly the previous maths): the world plane is additionally tilted up by the rendered look pitch;
// the rendered look yaw is part of yaw_rad.
bool world_composite_params(const xr_owner::EyeSample& eye, const float anchor[3], float yaw_rad, const float lens_tan[4],
    float distance_m, WorldComposite& out, float pitch_rad = 0.0f) noexcept;
struct WorldHit { bool hit = false; float u = 0, v = 0; };
// CPU mirror of the HLSL world_layer(): eye-image uv (pixel centre, (0,0) top-left) -> world image uv.
WorldHit world_ray_uv(const WorldComposite&, float u, float v) noexcept;

// J-world-hold / J-world-extend (keys xr_world_hold_last / xr_world_extend, both default OFF; see J-world-hold-findings.md).
// Two extra root constants follow the WorldComposite: b0[22] = world fade multiplier (1 = off), b0[23] = extend flag (0 = off).
// Key-off values (1, 0) leave every existing code path numerically identical (x * 1.0 and a skipped branch).
// J-test8-fixes (key xr_debug_view, default 0 = off): b0[3] carries `mode | (debug_view << 8)` for the EYE targets only (the low byte is the shader
// mode exactly as before, so the constant count is unchanged and key off is bit-identical). CPU mirror of the HLSL debug block:
//   1: (cover, cover, cover)                          cockpit coverage alpha (depth > 0) as grey
//   2: colour                                         the (tonemapped) cockpit colour, alpha forced 1
//   3: world.rgb                                      the world layer only (zero outside modes 2/3)
//   4: (cover, world.a, colour present && cover == 0)  red = covered, green = world layer, blue = cockpit colour that the depth rule discards
// `cover` is 1 where the depth plane is > 0 (else 0), `colour` the tonemapped cockpit rgb, `world` the world layer pixel (rgb, a). Output alpha is 1.
constexpr unsigned kDebugViewMax = 4;
inline uint32_t pack_mode_bits(uint32_t mode, uint32_t debug_view) noexcept { return (mode & 255u) | ((debug_view > kDebugViewMax ? 0u : debug_view) << 8); }
void debug_view_rgb(unsigned view, float cover, const float colour[3], const float world[4], float out[3]) noexcept;
// J-occlusion-fix (key xr_world_window_occlude, default OFF; docs/review/occlusion-fix-impl.md): mode_bits bit 16 (no new constant). Eye targets in the
// world composite (modes 2/3/4) with use_depth only. A pixel whose depth is in the replayed-OBJECT band (0 < depth < kOccludeObjectDepth = 2^-10, the
// compressed scale of xr_world_objects_depth_fix; cockpit depth lies above it) and whose view ray hits the world WINDOW has its cockpit coverage scaled by
// keep = 1 - edge_ramp(wu, wv, kOccludeSeam): the world snapshot already holds that object correctly occluded by the terrain. Periphery (no window hit),
// cockpit depth and everything else: keep = 1 (exactly the previous coverage). CPU mirror of the HLSL occlude_keep().
constexpr uint32_t kModeBitOcclude = 1u << 16;
constexpr float kOccludeObjectDepth = 0.0009765625f; // 2^-10 = gpu_copy::step5::world::depth_fix_scale
constexpr float kOccludeSeam = 0.1f;                  // ramp width at the window edge (fraction of the window half-extent, as edge_feather)
// COCKPIT-2 (key xr_cockpit_model, default OFF; docs/world-render/cockpit-render-impl.md): mode_bits bit 17 on the eye targets while the model cockpit is drawn. The replayed
// eye image keeps only its OBJECT band (0 < depth < kOccludeObjectDepth: replayed world objects); its cockpit band (and, without a depth plane, everything) gets
// coverage 0, so the model cockpit drawn over the composite replaces the replayed cockpit while the world/effects layers stay. CPU mirror model_cockpit_cover().
constexpr uint32_t kModeBitModelCockpit = 1u << 17;
// EFFECTS-OUTSIDE (C) fxmem:4 (fx_smoke_gpu.h, docs/world-render/effects-outside-window.md 8.7): mode_bits bit 18 on the eye targets of the world composite
// (with use_depth) while this frame's per-eye smoke image is bound at t13: the smoke (coverage_v1) goes OVER the composited objects/terrain/world at the eye pixel
// (the objects' decouple re-projection and fade), except on the replayed cockpit band without the model cockpit. CPU mirror acz::fxsmoke::gpu::smoke_over.
constexpr uint32_t kModeBitSmoke = 1u << 18;
// EFFECTS-OUTSIDE (F) fxmem:6 (fx_sprite_gpu.h, docs/world-render/effects-outside-window.md 11): mode_bits bit 19 on the COMPOSITE EYE targets only (bits
// 19..23 carry the radar background on the HUD target only; the shader reads them there in mode 1 only) while this frame's per-eye sprite image is bound at
// t14: the sprites (premultiplied light + coverage) go over the smoke result at the eye pixel. CPU mirror acz::fxsprite::gpu::sprite_over.
constexpr uint32_t kModeBitSprite = 1u << 19;
// WORLD-WINDOW FILTER (eyeres > 0 only; eye_res.h filter_select, docs/world-render/eye-resolution.md 3b): mode_bits bit 21 on the COMPOSITE EYE targets
// (free there: 19 sprite, 20 blink, 24..31 vignette; on the HUD target 19..23 are the radar background and this bit is never set). The world-window texel
// of world_layer_window becomes a 2x2 footprint grid of bilinear taps when minified. Clear (every job without eyeres): the old single tap.
constexpr uint32_t kModeBitWorldFilter = 1u << 21;
float model_cockpit_cover(bool use_depth, float depth, float cover) noexcept;
float window_occlude_keep(const WorldComposite&, float u, float v, float depth) noexcept;
constexpr unsigned kBlitConstantsLegacy = 5 + 17; // the constant count before the two keys (log text of the key-off world-composite line)
constexpr unsigned kBlitConstants = kBlitConstantsLegacy + 2;
// Hold: with no world for a frame the last good world image is composited for kWorldHoldMs, full strength for the first half,
// then a linear fade to black reaching 0 at expiry. NaN / negative age -> 1 (just captured); age >= hold -> 0.
constexpr int64_t kWorldHoldMs = 750;
float world_hold_fade(int64_t age_ms, int64_t hold_ms) noexcept;
// Extend: a ray that misses the world window samples the world image with CLAMPED coordinates so the window's edge pixels
// smear outward over the whole sphere, darkened and contracted toward the extreme angles. Window rays are exactly world_ray_uv.
//   plane map (front hemisphere): the window uv of the plane hit, unclamped;
//   angular map (side/back): wu = bu + tan(clamp(azimuth, +-1.5)) / (l+r) with azimuth = atan2(dx, -dz), wv = bv + tan(clamp(elevation, +-1.5))
//                            * dist * av with elevation = atan2(dy, hypot(dx, dz));
//   raw = lerp(plane, angular, wA), wA = 1 - smoothstep(0, 0.5, cosz), cosz = -d.z/|d| (so the maps meet continuously);
//   sample uv S = clamp(raw, 0, 1); ang = angle between the ray and the window point S; k = 1 - (1 - kExtendMinLight) * smoothstep(0, kExtendFalloffRad, ang);
//   the axis ALONG a window edge is contracted toward 0.5 by kExtendBlurMix * smoothstep(0, kExtendBlurRad, ang) * smoothstep(0, kExtendBlurEdge, outside distance of the
//   OTHER axis) * (1 - smoothstep(0, kExtendBlurEdge, outside distance of THIS axis)) (continuous across the corner columns/rows).
// ext = false inside the window (u, v = the window uv, k = 1); the world pixel is opaque everywhere (a = 1).
constexpr float kExtendMinLight = 0.30f, kExtendFalloffRad = 2.2f, kExtendBlurMix = 0.85f, kExtendBlurRad = 0.9f, kExtendBlurEdge = 0.5f;
struct WorldExtendSample { bool ext = false; float u = 0, v = 0, k = 1, ang = 0; };
WorldExtendSample world_ray_extend(const WorldComposite&, float u, float v) noexcept;

// J-world-cache ("periphery fill v2", key xr_world_cache, default OFF; docs/review/world-cache-impl.md). The consumer keeps a persistent
// environment image registered to the COCKPIT frame (equirectangular: u = 0.5 + az/2pi, v = 0.5 - el/pi, az = atan2(x, -z), el = atan2(y, hypot(x, z));
// -Z = ahead). Every fresh world frame is written into it by direction, the eye pixels that miss the live window sample it with a per-texel age
// fade toward a neutral sky/ground colour. Frame maths (all CPU mirrors of xr_stereo_blit.hlsl; ly/lp = the frame's rendered look yaw/pitch, the same
// values world_composite_params turns the plane by):  g (cache frame) = Ry(ly) Rx(lp) d (window frame);  d = Rx(-lp) Ry(-ly) g.
// Key off: nothing below is used (3 constants stay 0, no resource is created) and every output byte is unchanged.
constexpr uint32_t kWorldCacheWidth = 3840, kWorldCacheHeight = 1920;       // R11G11B10 colour + R32F stamp = 8 B/texel = 56.25 MiB (budget 64 MiB)
constexpr uint64_t kWorldCacheBudgetBytes = 64ull * 1024 * 1024;
constexpr unsigned kBlitConstantsAll = kBlitConstants + 4;                  // + w_cache, look_yaw, look_pitch, cache_now (cbuffer register c6)
constexpr float kCacheFullS = 3.0f, kCacheZeroS = 12.0f;                    // age weight: 1 up to kCacheFullS s, linear to 0 at kCacheZeroS s
constexpr float kCacheFillBlurRad = 0.25f;                                  // the sky/ground fill blends over +-this elevation (rad) around the horizon
constexpr float kCacheFillConfLo = 0.06f, kCacheFillConfHi = 0.25f;         // horizon-fit confidence = smoothstep over the L1 row-mean step
constexpr float kCacheMaxValue = 60000.0f;                                  // R11G11B10_FLOAT range guard (written values are clamped to [0, this])
void world_cache_to_cache(float ly, float lp, const float d[3], float g[3]) noexcept;   // window frame -> cache frame
void world_cache_to_window(float ly, float lp, const float g[3], float d[3]) noexcept;  // cache frame -> window frame
void world_cache_dir_uv(const float g[3], float& u, float& v) noexcept;                 // direction (need not be unit) -> cache uv
void world_cache_uv_dir(float u, float v, float g[3]) noexcept;                         // cache uv -> unit direction
float world_cache_age_weight(float stamp, float now) noexcept;                          // stamp <= 0 (or NaN) = never seen = 0
struct CacheWindowSample { bool hit = false; float u = 0, v = 0; };
// The cache texel at uv (cu, cv) as seen by the world window of a frame with look (ly, lp) and lens_tan {l, r, u, d}: the write pass' mapping.
CacheWindowSample world_cache_texel_window(const float lens_tan[4], float ly, float lp, float cu, float cv) noexcept;
struct CacheRect { uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };                          // texel rectangle [x0, x1) x [y0, y1)
// A conservative rectangle holding every cache texel the window can write (whole cache when the cone touches a pole or wraps the az seam).
CacheRect world_cache_update_rect(const float lens_tan[4], float ly, float lp, uint32_t cw, uint32_t ch) noexcept;
float world_cache_horizon_conf(float best_step) noexcept;                               // smoothstep(kCacheFillConfLo, kCacheFillConfHi, step)
float world_cache_fill_t(float el, float el_horizon, float conf) noexcept;              // 0 = ground .. 1 = sky; the horizon elevation is el_horizon * conf

// DECOUPLE (xr_terrain_stage option decouple:1, default OFF; docs/world-render/decouple.md). The eye composite runs on EVERY XR frame with that frame's own
// head pose P1, while the producer's images were rendered at the game frame's pose P0. Root constants b2 (15 dwords, all zero = off: no code runs):
//   h[9]  (rows) H = M0^-1 * Qsrc: the P1 anchor-frame ray d1 = base1 + u*ux1 + v*vy1 (b0, this eye's WorldComposite) maps to the P0 eye image through
//         p = H d1, uv0 = p.xy / p.z (p.z > 0: in front), M0 = [ux0 | vy0 | base0] of the P0 WorldComposite; Qsrc = the camera-delta rotation (below) when the
//         replayed cockpit band is dropped (model cockpit), else identity: ROTATION-ONLY re-projection of the objects image (head translation ignored).
//   q[4]  the camera-delta rotation Qfx as a quaternion (x, y, z, w): world-window/effects rays use q d1 (the game images are screen-space in the cockpit
//         frame of the camera they were drawn with; the terrain uses the display-time camera).
//   fade  edge fade width in source uv (0 = a hard edge); flags: bit0 = re-project the eye source, bit1 = rotate the window rays.
// Camera-delta rotation: anchor-frame direction d maps to world C d with C = [right | up | -fwd] (the terrain shader's dw); Q = Cobj^T Cterrain.
struct Reproject { float h[9]; float q[4]; float fade; uint32_t flags; };
static_assert(sizeof(Reproject) == 15 * sizeof(float), "Reproject is the 15 root constants of b2");
constexpr unsigned kReprojectConstants = 15;
constexpr uint32_t kReprojSource = 1u, kReprojWindow = 2u;
// Q (row-major, Q[i][j] = Cobj_col_i . Ct_col_j) from two camera bases (right/up/fwd each); identical bases give the exact identity.
void camera_delta_rotation(const float obj_right[3], const float obj_up[3], const float obj_fwd[3], const float t_right[3], const float t_up[3],
                           const float t_fwd[3], double Q[3][3]) noexcept;
bool rotation_is_identity(const double Q[3][3]) noexcept;   // exactly the identity (bit equality)
// H = M0^-1 Qsrc and q = quat(Qfx). src/fx select the flags; false (out zeroed, flags 0) for a singular / non-finite M0 or rotation.
bool reproject_params(const WorldComposite& p0, const double Qsrc[3][3], const double Qfx[3][3], bool src, bool fx, float fade, Reproject& out) noexcept;
// CPU mirrors of the shader: the P0 source uv of P1 eye-uv (u, v) (in = inside the source image; fade = the edge ramp, 0 outside), and the window ray.
struct ReprojectSample { bool in = false; float u = 0, v = 0, fade = 1; };
ReprojectSample reproject_uv(const Reproject&, const WorldComposite& p1, float u, float v) noexcept;
void reproject_dir(const Reproject&, const float d[3], float out[3]) noexcept;

// ------------------------------------------- coverage / tonemap (pure)
// Depth(+stencil) formats -> the SRV format of the depth plane (plane 0):
// D32_FLOAT_S8X24_UINT(20)->R32_FLOAT_X8X24_TYPELESS(21), D32_FLOAT(40)->R32_FLOAT(41),
// D24_UNORM_S8_UINT(45)->R24_UNORM_X8_TYPELESS(46), D16_UNORM(55)->R16_UNORM(56).
// 0 = unsupported: the cockpit falls back to opaque alpha = 1.
uint32_t depth_srv_format(uint32_t depth_format) noexcept;
enum class Tonemap : uint32_t { clamp = 0, reinhard = 1 };
struct Shade { float r = 0, g = 0, b = 0, a = 0; };
// CPU mirror of xr_stereo_blit.hlsl ps_main: c = max(rgb*exposure, 0) (NaN -> 0),
// then clamp: saturate(c), or reinhard: c/(1+c); a = use_depth ? (depth > 0 ? 1 : 0) : 1;
// output premultiplied (rgb*a, a). Linear; the sRGB RTV encodes once.
Shade shade(const float rgb[3], bool use_depth, float depth, Tonemap, float exposure) noexcept;
// J-hud-consumer: CPU mirror of the HUD branch of ps_main (mode 1). The producer's HUD
// image is the PS2 HUD blended into a ZERO-cleared target, so its colour is already
// premultiplied; its alpha channel is NOT a reliable coverage mask (PS2 blends write
// destination alpha by their own rules). Output: c = tonemap(max(rgb*exposure, 0)) exactly
// as shade(); a = saturate(max(max(alpha, 0), max(c.r, c.g, c.b))) (NaN -> 0), so every
// lit HUD texel is at least as opaque as its brightest channel and c <= a always holds
// (valid premultiplied colour); output (c, a), NOT multiplied by a again.
Shade shade_hud(const float rgba[4], Tonemap, float exposure) noexcept;
// CPU mirror of the mode-2 combine on premultiplied colour: cockpit + (1 - cockpit.a) * world.
Shade composite_over_world(const Shade& cockpit, const Shade& world) noexcept;
// J-effects-layer: CPU mirror of shader mode 3. The effects image is the PS2 blend into a ZERO-cleared target (premultiplied, alpha not a
// coverage mask): e = shade_hud(rgba) (alpha = saturate(max(a, max(rgb)))), optionally multiplied by `ramp` (the world-uv edge ramp, 1 = none);
// the world layer becomes e + (1 - e.a) * world, and the cockpit is then composited over that exactly as in mode 2. `world` is the (already
// ramped, alpha 1 in the window) world layer sample at the SAME world uv as the effects texel.
Shade effects_over_world(const float effects_rgba[4], Tonemap, float exposure, float ramp, const Shade& world) noexcept;
// J-effects-dual: CPU mirror of shader mode 4 (mode 3 with the producer's coverage_v1 image, StereoFrame::effects_repr). The texel is
// (P premultiplied colour, A RGB coverage) = the game's dual-source RGB blend accumulated over a zero target plus a ONE/INV_SRC_ALPHA alpha:
// RGB_final = P + (1 - A) * world. The colour is un-premultiplied, put through the SAME exposure/tonemap as every layer, then premultiplied by A
// again (so RGB <= A and brightness never changes the opacity); A = clamp(A) is the geometric attenuation of the world, never derived from the
// colour or the game's alpha. A <= 0 (or NaN) returns the world unchanged without a division. `ramp` multiplies the layer like the window edge ramp.
Shade effects_coverage_over_world(const float effects_pa[4], Tonemap, float exposure, float ramp, const Shade& world) noexcept;
// J-feather (key xr_edge_feather, default 0 = OFF; see J-feather-findings.md): CPU mirror
// of the ps_main edge ramp. `feather` is a fraction of the layer HALF-extent (clamped to
// [0, kMaxFeather]); per axis d = 2*min(u, 1-u) (0 at the border, 1 at the centre) and
// ramp = smoothstep(0, feather, d) = t*t*(3-2t), t = saturate(d/feather); the layer ramp is
// the product of both axes. feather <= 0 (or NaN) returns EXACTLY 1 (no change). Every
// path multiplies its PREMULTIPLIED output (rgb, a) by the ramp, so c <= a still holds.
constexpr float kMaxFeather = 0.25f;
float clamp_feather(float) noexcept;          // NaN/<=0 -> 0, > kMaxFeather -> kMaxFeather
float edge_ramp(float u, float v, float feather) noexcept;
Shade apply_feather(const Shade&, float ramp) noexcept; // (r*k, g*k, b*k, a*k)
// J-feather: world-quad distance key xr_world_quad_distance_m, clamped to [5, 2000] m
// (default 40). world_quad() scales the size with the distance, so the angular size
// (the lens FOV) is unchanged at any distance; NaN -> the default.
constexpr float kQuadDistanceDefault = 40.0f, kQuadDistanceMin = 5.0f, kQuadDistanceMax = 2000.0f;
float clamp_quad_distance(float) noexcept;
// J-cockpit-fov-consumer (driven by StereoFrame::cockpit_fov_valid, which only the producer key
// gpu_copy_step5_hmd_fov sets; see J-cockpit-fov-consumer-findings.md): the per-eye FOV the
// cockpit was rendered with ({angleLeft, angleRight, angleUp, angleDown} radians). Accepted only
// when every angle is finite with |angle| <= kMaxCockpitFovRad, left < right and down < up.
// A frame whose flag is set with a refused FOV is released unused and submits NO layer
// (counted cockpit-fov-invalid): a projection whose FOV differs from the render is never sent.
constexpr float kMaxCockpitFovRad = 1.5f;
bool cockpit_fov_ok(const float fov[4]) noexcept;
// J-hud-placement (see J-hud-placement-findings.md; every key default unset = placement unchanged).
// world-quad mode: hud_world_quad() = world_quad(neutral, tan, D) at D = the HUD distance
// (xr_hud_quad_distance_m, 0 = unset -> the world quad's own distance), i.e. the same anchor,
// identity orientation and the same DIRECTION of every corner from the anchor (same angular
// size) at a nearer/farther plane; then scale_quad() by xr_hud_scale about the quad centre.
// Unset distance and scale 1 return EXACTLY world_quad(neutral, tan, world_distance).
// head-locked: hud_head_quad() VIEW-space, -distance on Z, width = width_m*scale, source aspect.
constexpr float kHudQuadDistanceMin = 0.3f, kHudQuadDistanceMax = 2000.0f;
constexpr float kHudScaleDefault = 1.0f, kHudScaleMin = 0.25f, kHudScaleMax = 2.0f;
constexpr float kHudHeadDistanceDefault = 2.5f, kHudHeadDistanceMin = 0.5f, kHudHeadDistanceMax = 20.0f;
constexpr float kHudHeadWidthDefault = 1.6f, kHudHeadWidthMin = 0.3f, kHudHeadWidthMax = 10.0f;
float clamp_hud_quad_distance(float) noexcept; // NaN / <= 0 -> 0 (unset), else [0.3, 2000] (COCKPIT-3: 0.3 so hudglass:1 reaches the j35j glass, 0.395 m)
float clamp_hud_scale(float) noexcept;         // NaN -> 1, else [0.25, 2]
float clamp_hud_head_distance(float) noexcept; // NaN -> 2.5, else [0.5, 20]
float clamp_hud_head_width(float) noexcept;    // NaN -> 1.6, else [0.3, 10]
Quad scale_quad(const Quad&, float scale) noexcept; // same pose, size * scale
Quad hud_world_quad(const float neutral[3], const float tan[4], float world_distance_m, float hud_distance_m, float scale) noexcept;
// J-quad-yaw: hud_world_quad() in the frame turned about +Y by yaw_rad (world_quad(n, t, D, yaw)).
Quad hud_world_quad(const float neutral[3], const float tan[4], float world_distance_m, float hud_distance_m, float scale, float yaw_rad) noexcept;
// xr_hud_follow mode 2: the same quad additionally tilted by pitch_rad (world_quad(n, t, D, yaw, pitch)); pitch 0 is exactly the yaw overload.
Quad hud_world_quad(const float neutral[3], const float tan[4], float world_distance_m, float hud_distance_m, float scale, float yaw_rad, float pitch_rad) noexcept;
Quad hud_head_quad(float distance_m, float width_m, float scale, uint32_t src_w, uint32_t src_h) noexcept;

// ------------------------------------------- layer composition (pure)
// The xrEndFrame layer list for one ProjectionLayer plan, in submission order
// (back to front). The OpenXR backend builds its XrCompositionLayer* array from
// exactly this list. Without `hud` it is the existing [quad,] projection list.
// With `hud` the HUD quad is LAST (on top of the cockpit) with SOURCE_ALPHA and
// never UNPREMULTIPLIED_ALPHA (premultiplied), in LOCAL or VIEW (head-locked).
// J-feather: the world quad carries SOURCE_ALPHA only with ProjectionLayer::quad_source_alpha
// (set only while xr_edge_feather > 0); otherwise it stays opaque exactly as before.
// J-hud-markers: marker_quad (key xr_hud_markers_layer) is submitted after the world quad and before the projection:
// order world, markers, cockpit projection, instrument HUD.
enum class LayerKind : uint8_t { world_quad, projection, hud_quad, marker_quad, screen_quad, table_quad };   // screen_quad: VSCREEN (ProjectionLayer::screen_only); table_quad (target = eye 0/1): the BRIEF3D table as a per-eye quad (ProjectionLayer::table_front)
enum class LayerSpace : uint8_t { local, view };
struct LayerEntry {
    LayerKind kind = LayerKind::projection;
    unsigned target = 0;            // swapchain target (projection: eyes 0/1)
    bool source_alpha = false;      // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    bool unpremultiplied = false;   // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT (never set)
    LayerSpace space = LayerSpace::local;
};
struct LayerList { unsigned count = 0; std::array<LayerEntry, 4> e{}; };
LayerList compose_layers(const xr_owner::ProjectionLayer&) noexcept;
// UI3D 2026-10-30b (ui-3d.md 11): a projection VIEW (pose + asymmetric fov, radians, left/down negative) shown as a quad layer visible to that eye only:
// the plane perpendicular to the view axis at `distance_m`, offset so that every image edge keeps its direction from the eye (the quad subtends
// exactly the view fov from the view pose). Seen from the view pose it is the projection image.
inline constexpr float kTableQuadDistanceM = 1.5f;
Quad view_quad(const xr_owner::EyeSample& view, float distance_m) noexcept;
// The plane depth the table quad of one eye is submitted at: `requested` (ProjectionLayer::table_quad_distance_m) clamped to [0.4, 4] m, 0 / non-finite
// = kTableQuadDistanceM.
inline float table_quad_distance(float requested) noexcept {
    if (!(requested > 0.0f) || !(requested < 1e6f)) return kTableQuadDistanceM;
    return requested < 0.4f ? 0.4f : requested > 4.0f ? 4.0f : requested;
}

// --------------------------------------------- format selection (pure)
// Same rule as xr_probe/format_policy.h select_later_colour_format: prefer 29
// (R8G8B8A8_UNORM_SRGB, has alpha), else 28 (UNORM + MUTABLE_FORMAT, typeless
// resource, sRGB RTV); never BGRA or float. 0 = refuse.
constexpr int64_t rgba8_unorm = 28, rgba8_srgb = 29;
int64_t select_format(const int64_t* formats, uint32_t count) noexcept;
// Source formats the PSO accepts: R16G16B16A16_FLOAT (10) and R32G32B32A32_FLOAT (2).
bool source_format_ok(uint32_t dxgi_format) noexcept;

// ------------------------------------------------- XR swapchain facade
constexpr unsigned max_formats = 64, max_images = 8;
// Target index: 0 = left cockpit eye, 1 = right cockpit eye, 2 = world quad,
// 3 = HUD quad (J-hud-consumer; its swapchain exists only with Settings::hud_layer),
// 4 = marker quad (J-hud-markers; its swapchain exists only with Settings::hud_markers_layer).
constexpr unsigned kTargets = 5, kQuad = 2, kHud = 3, kMarkers = 4;
struct ViewConfig { uint32_t recommended_w = 0, recommended_h = 0, max_w = 0, max_h = 0, recommended_samples = 0; };
struct SwapchainImages {
    uint32_t count = 0;
    std::array<void*, max_images> images{}; // ID3D12Resource*, owned by the runtime
};
// Owner-thread only, every call through Gate::call. Implemented by the real
// OpenXR backend (xr_owner_openxr.cpp) on the owner's session. `target` is
// 0..kTargets-1 (see above).
struct SwapchainRuntime {
    virtual ~SwapchainRuntime() = default;
    virtual Rc view_configs(std::array<ViewConfig, 2>&) = 0;
    // `count` is the runtime's count even when it exceeds capacity (refused).
    virtual Rc formats(std::array<int64_t, max_formats>&, uint32_t& count) = 0;
    virtual Rc create_swapchain(unsigned target, int64_t format, uint32_t w, uint32_t h, SwapchainImages&) = 0;
    virtual Rc acquire(unsigned target, uint32_t& index) = 0;
    virtual Rc wait(unsigned target, int64_t timeout_ns, bool& timed_out) = 0;
    virtual Rc release(unsigned target) = 0;
    virtual Rc destroy_swapchain(unsigned target) = 0;
};

// ------------------------------------------------------ D3D12 blit facade
// Diagnostics only: read after CPU-observed private-fence completion.
struct GpuTiming { bool available = false; double start_ms = 0, blit_ms = 0; };
GpuTiming timestamp_timing(uint64_t start, uint64_t end, uint64_t calibration_gpu,
    uint64_t calibration_cpu, uint64_t execute_cpu, uint64_t gpu_hz, uint64_t cpu_hz) noexcept;
bool needs_clear(const BlitMapping&, uint32_t width, uint32_t height) noexcept;
// Metadata-only acquire/release; no source image is read or submitted.
Extent published_source_extent(stereo::FrameSource*) noexcept;
// XR-START: the same metadata probe with its evidence for the waiting-for-source line:
// tried = a source existed; acquired = try_acquire_latest(0) returned a slot (which was then
// HELD by the probe and released at once); frame_id/slot of that publication.
struct SourceProbe { bool tried = false, acquired = false; uint64_t frame_id = 0; unsigned slot = 0; Extent extent{}; };
SourceProbe probe_source(stereo::FrameSource*) noexcept;
uint32_t configured_cap(const char*) noexcept;
// J-terrain-render (key xr_terrain_render + xr_terrain_stage; default OFF): the loaded stage, its RGBA atlas and the conventions. Immutable after construction; the
// backend keys its GPU copy on `generation`. camera == nullptr uses acz::terrain::cam_mailbox() (E6 fields published by the Present tick of ee_camera_probe_win.cpp).
struct TerrainRuntime {
    acz::terrain::StageData stage;
    acz::terrain::Atlas atlas;
    acz::terrain::Conventions conv;
    std::vector<float> env;   // WORLD-2: build_env_texture output (empty = no environment / options off)
    std::shared_ptr<const acz::props::Runtime> props;   // WORLD-3 (xr_terrain_stage options props:1 / trees:1; null = off): borrowed by the backend per record()
    uint64_t generation = 1;
    std::function<bool(acz::terrain::CamSample&)> camera;   // legacy (conv.cam_admit false) test hook only; the admission path never calls it
    // CAM-EPOCH (conv.cam_admit): the Present-tick mailbox (history + newest) and the current epochs; nullptr = the process-wide instances (tests inject).
    const acz::terrain::CamMailbox* present = nullptr;
    const acz::terrain::CamEpochs* epochs = nullptr;
    acz::tac::Options tac;   // TACTICAL (option tac:1 and its sub-options; default off = nothing runs)
    acz::pacing::Options pacing;   // UNCAP/EXTRAP (uncap:1 is applied by the Present hook; extrap:1 here, default off = nothing runs)
    // TAKEOFF (option takeoff:, default k00a): the airbase stage of the takeoff segment (own stage/atlas/env/props, a distinct generation; conv = the
    // mission's). Null = none. The consumer draws it while its TakeoffSelector says the aircraft is on the base; every other use stays on this runtime.
    std::shared_ptr<const TerrainRuntime> takeoff;
};
// VSEARLY (vsearly:1 with xr_terrain_stage=auto; flight_cycle.h, docs/world-render/vsearly.md): the per-flight terrain. Null (the default) = off: nothing
// changes. frame(): owner thread, every XR frame before its work; may replace `terrain` (null between flights) -> true. recorded(): after a recorded
// job that carried the terrain (the retire rule for stages the props worker may borrow).
struct FlightStageHook {
    virtual ~FlightStageHook() = default;
    virtual bool frame(int64_t now_ms, int64_t last_new_ms, std::shared_ptr<const TerrainRuntime>& terrain) = 0;
    virtual void recorded(uint64_t terrain_gen, bool props_job, bool props_settled) = 0;
    // VIEW-MODE (pov:1, view_mode.h): the keep-alive time for the NEXT frame() (HUD-only / chase views publish no producer frame). Default: ignored.
    virtual void view_alive(int64_t /*alive_ms*/) {}
};
// One eye's terrain camera for a BlitJob (stage space).
struct TerrainEye { float eye[3] = {}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, -1}; };
// COCKPIT-2 (key xr_cockpit_model, default OFF): the loaded model's GPU vertex stream/textures and the options. Immutable after construction; the backend keys its
// GPU copy on generation.
struct CockpitRuntime {
    acz::cockpit::Mesh mesh;
    acz::cockpit::Options opt;
    std::string id;
    uint64_t generation = 1;
};
// AIRCRAFT-AUTO (xr_cockpit_model=auto[;opt:val], docs/world-render/aircraft-auto.md): the consumer chooses the model itself from the E6 eye offset
// (aircraft_auto.h Detector) and loads it lazily on a worker thread; until a type is detected (no E6 lock, look never centred, no match) and while a
// model file is missing, NO model is drawn (the game's replayed cockpit, exactly the key-off path) - unless an ASSUMED type (AIRCRAFT-AUTO-FAST:
// the remembered last aircraft or default:) is loaded before the session: then its model is drawn from the first frame until the detector confirms
// it or switches to another confirmed type. Immutable after construction.
struct CockpitAuto {
    acz::aircraft::AutoConfig cfg;
    acz::aircraft::DetectOptions det;
    std::function<bool(acz::aircraft::EyeSample&)> sample;   // the newest eye sample (seq); null = acz::aircraft::eye_mailbox() (the probe's locked E6 block)
    // Builds the runtime of table type i (on a worker thread when async); null = load_auto_model (the .zvrm file <model dir>\<model_id>.zvrm).
    std::function<std::shared_ptr<CockpitRuntime>(unsigned type, std::string& why)> load;
    std::string model_dir;   // empty = %LOCALAPPDATA%\acz-vr\models (cockpit::model_path)
    bool async = true;       // false (tests): the load runs inside the frame (and the remembered file is written inline)
    // AIRCRAFT-AUTO-FAST: every eye sample published after `after` (oldest first, <= cap); null (and no `sample`) = eye_mailbox().drain.
    std::function<unsigned(uint64_t after, acz::aircraft::EyeSample* out, unsigned cap)> drain;
    // The type assumed at XR start (the remembered last aircraft, else the config's default:; -1 = none = the plain detector). The Output
    // constructor (the XR monitor thread, before the session: never inside a frame) loads its model so the very first composed frame draws it.
    int assumed = -1;
    std::string assumed_source;   // "remembered-file" | "config-default" (log only)
    std::string state_path;       // the remembered-aircraft file written on a confirmed type; empty = never written
    int remembered = -1;          // the type the file held at start (no rewrite while it does not change)
    // AIRCRAFT-AUTO-STALE: ms without ANY locked E6 sample after which the shown/assumed type is forgotten (the game's replayed cockpit; 0 = never).
    // The proxy copies cfg.stale_ms (stale:<ms>, default 15000). now_ms = test clock (null = steady clock).
    uint32_t stale_ms = 15000;
    uint32_t grace_ms = 60000;   // AIRCRAFT-AUTO-GRACE (cfg grace:<ms>): an unconfirmed model without junk samples is trusted for max(stale_ms, grace_ms)
    std::function<double()> now_ms;
};
// Loads + builds kAircraft[type]'s model with options_for(cfg, type); null + why on any failure (a missing file = status io_error). Each success gets a
// fresh generation (the backend keys its GPU copy on it).
std::shared_ptr<CockpitRuntime> load_auto_model(const acz::aircraft::AutoConfig& cfg, unsigned type, const std::string& model_dir, std::string& why);
struct SourceEye { void* resource = nullptr; uint32_t format = 0, width = 0, height = 0; };
// One recording: the cockpit eyes (with optional depth coverage) and the
// optional world image into the quad swapchain.
struct BlitJob {
    std::array<SourceEye, 2> eyes{};
    std::array<SourceEye, 2> depth{};  // resource + DEPTH format (SRV format derived)
    bool use_depth = false;
    SourceEye world{};
    bool use_world = false;
    SourceEye hud{};                          // J-hud-consumer: premultiplied mono HUD -> target kHud
    bool use_hud = false;
    uint32_t hud_backdrop_bits = 0;           // HUD-MENU (hud_menu.h backdrop_bits, 24..31) | RADAR-BG (radar_bg_bits, 19..23): OR-ed into the HUD target's mode_bits; 0 = off
    SourceEye markers{};                   // J-hud-markers: premultiplied mono marker image -> target kMarkers (HUD shader mode)
    bool use_markers = false;
    SourceEye effects{};                      // J-effects-layer: premultiplied mono effects image, sampled in the eyes' world composite (mode 3, t3)
    bool use_effects = false;                 // only with world_composite && !world_held
    bool effects_coverage = false;            // J-effects-dual: the effects image is coverage_v1 (shader mode 4); false = legacy (mode 3)
    std::array<SourceEye, 2> smoke{};         // EFFECTS-OUTSIDE (C) fxmem:4: per-eye smoke image (coverage_v1, eye size), eye e samples smoke[e] at t13
    bool use_smoke = false;                   // only with world_composite && !world_held && use_depth (kModeBitSmoke)
    std::array<SourceEye, 2> sprite{};        // EFFECTS-OUTSIDE (F) fxmem:6: per-eye sprite image (light + coverage, eye size), eye e samples sprite[e] at t14
    bool use_sprite = false;                  // same conditions as use_smoke (kModeBitSprite)
    std::array<uint32_t, kTargets> index{};   // acquired image index per target
    std::array<BlitMapping, kTargets> map{};
    Tonemap tonemap = Tonemap::clamp;
    float exposure = 1.0f;
    float edge_feather = 0.0f;                // J-feather: 0 = no ramp (b0 constant 4, every target)
    uint32_t pov_vignette_bits = 0;           // POV-RENDER pov:3 (pov_render.h vignette_mode_bits): eye targets' mode_bits 20, 24..31; 0 = off (every other job)
    // J-world-in-projection: use_world with world_composite = the world image is NOT drawn into the quad
    // target (none is bound); eye e's draw runs ps_main mode 2 with world_eye[e] and samples `world`.
    bool world_composite = false;
    std::array<WorldComposite, 2> world_eye{};
    // J-world-hold: world_capture = also copy `world` into the backend's held image after the draws (same recording); world_held = the
    // world for this job is the held image (no `world` resource; use_world and world_composite are set); world_fade = b0[22];
    // world_extend = b0[23] != 0 (periphery fill, eyes only). Key off: false / false / 1 / false.
    bool world_capture = false, world_held = false, world_extend = false;
    float world_fade = 1.0f;
    // J-occlusion-fix: mode_bits bit 16 on the eye targets of the world composite (needs use_depth); see window_occlude_keep. Key off: false.
    bool occlude_window = false;
    // WORLD-WINDOW FILTER (eyeres > 0): kModeBitWorldFilter on the composite eye targets. eyeres off: false.
    bool world_filter = false;
    // J-test8-fixes: debug_view (xr_debug_view, 0 = off) is packed into the eye targets' mode constant; png_capture (xr_png_once) additionally renders eye 0
    // twice into a private target (the final composite as submitted, and its cockpit coverage as grey) and copies both to a readback buffer.
    uint32_t debug_view = 0;
    bool png_capture = false;
    // J-terrain-render: terrain = the eyes of the world composite draw the baked stage (b1 constants from terrain_eye, haze from terrain_haze_l / terrain_far) instead of the world
    // image; terrain_blend = the game's world window over the terrain (A/B). The stage/atlas are borrowed for the duration of record(). Key off: false, nothing else changes.
    bool terrain = false, terrain_blend = false;
    const acz::terrain::StageData* terrain_stage = nullptr;
    const acz::terrain::Atlas* terrain_atlas = nullptr;
    uint64_t terrain_gen = 0;
    std::array<TerrainEye, 2> terrain_eye{};
    // WORLD-2: the stage environment texture (acz::terrain::build_env_texture, kEnvTexW x kEnvTexH RGBA32F; borrowed like the stage) and the enabled features
    // (kEnv* bits; 0 or a null/empty texture = the legacy shading). Uploaded with the stage under the same terrain_gen.
    const std::vector<float>* terrain_env = nullptr;
    uint32_t terrain_env_bits = 0;
    // WORLD-1 step 4: terrain_occlude = an eye pixel covered by a replayed world OBJECT (depth in the xr_world_objects_depth_fix band) is hidden where the terrain along
    // its ray is nearer than the object (b1 t_bits bit 3; needs use_depth and the producer's float-depth fix). Key off: false.
    bool terrain_occlude = false;
    float terrain_haze_l = 55000.0f, terrain_far = 160000.0f;   // WORLD-1: haze length L and march end (Conventions::haze_l / far_dist)
    // WORLD-3: props/trees drawn into the terrain composite (only with terrain). Borrowed for record(); null = off (nothing else changes).
    const acz::props::Runtime* props = nullptr;
    // PERF: props_async = the props pipeline (root signature + PSO) and static buffers/texture are built on a WORKER thread started by the first job
    // that needs them; until that worker is done the job draws the terrain WITHOUT props (exactly the props-off picture) and never waits for it.
    // false (tests, harness legacy cases) = built synchronously inside the job, as before. The consumer always sets it.
    bool props_async = false;
    // COCKPIT-2: cockpit = after each eye's composite the model cockpit is drawn over it (depth-tested, premultiplied over) with cockpit_eye[e] (b0 of the model pass),
    // and the eye composite drops the replayed cockpit band (kModeBitModelCockpit) unless cockpit_keep_replay (A/B option replay:1). The mesh is borrowed for record().
    bool cockpit = false, cockpit_keep_replay = false;
    const acz::cockpit::Mesh* cockpit_mesh = nullptr;
    uint64_t cockpit_gen = 0;
    std::array<acz::cockpit::Constants, 2> cockpit_eye{};
    // COCKPIT-4 anim:1: the posed animated vertex range (cockpit_mesh->anim_vertices corners, borrowed for record()); null = the rest pose already in the VB.
    const acz::cockpit::GpuVertex* cockpit_anim = nullptr;
    uint32_t cockpit_anim_count = 0;
    // SKIN-REFL (cockpit option srefl:1): the drawn stage's reflection map (RGBA8 w x h, <= kReflMaxDim; borrowed for record()) and its key (the drawn terrain's
    // generation): the backend copies it into its t5 texture when the key changes. Null = no map: kCFlagRefl is cleared in the eye constants.
    const uint8_t* cockpit_refl = nullptr;
    uint32_t cockpit_refl_w = 0, cockpit_refl_h = 0;
    uint64_t cockpit_refl_gen = 0;
    // J-world-cache (key xr_world_cache; needs world_composite): world_cache = the eye pixels outside the live window sample the backend's persistent
    // cache; world_cache_write = first update the cache from `world` (fresh world only: never with world_held); world_cache_clear = clear it first.
    // The look is the one the plane was turned by (cache read) / the world was rendered with (write). cache_lens = {l, r, u, d} tangents of the world
    // window. Key off: all false/0 and the three trailing root constants stay 0.
    bool world_cache = false, world_cache_write = false, world_cache_clear = false;
    float cache_look_yaw = 0.0f, cache_look_pitch = 0.0f, cache_now_s = 1.0f;
    float cache_lens[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t cache_w = kWorldCacheWidth, cache_h = kWorldCacheHeight;
    // GPU-COST (docs/world-render/gpu-cost.md): pass_timing = record GPU timestamps between the passes of this recording (kPassMark* below), read once
    // with BlitBackend::pass_timing() after the private fence completed. Diagnostic (benchmark tool); false = no extra query, nothing changes.
    bool pass_timing = false;
    // GPU-COST quality (xr_terrain_stage option q:0..2; docs/world-render/gpu-cost.md). 0 = the reference march exactly as before.
    uint32_t terrain_quality = 0;
    // PROPS-SHADING (xr_terrain_stage option objdom:display; docs/world-render/props-shading.md): decode the replayed OBJECT-band pixels (0 < depth < 2^-10) of the eye
    // source once (sRGB EOTF) before the composite, so the game's display values are not encoded twice. Only with the terrain and use_depth. false = as before.
    bool terrain_obj_display = false;
    // GPU-COST zpre (xr_cockpit_model option zpre:1): the model's opaque pass is drawn FIRST with depth, and the eye composite is depth-tested against it, so no
    // world/terrain pixel is shaded under the opaque cockpit (the final image is identical: the opaque model overwrote those pixels anyway). false = as before.
    bool cockpit_prepass = false;
    // TACTICAL (tac:1): clip-space overlay triangles (tactical.h Overlay; borrowed for record()): slots eye0/eye1 "pre" are drawn after the eye's composite
    // and BEFORE the model cockpit (synthetic tracers: the opaque cockpit covers them), "post" after the model (our markers, on top like the game's
    // markers), "hud" into the HUD target after the HUD image (our radar blips). null = off: nothing is created or drawn (key-off recording unchanged).
    const acz::tac::Overlay* tac = nullptr;
    // NBOX (mk360:3; marker360.h StampPlan, borrowed for record()): the tac overlay's slot_eye0/1_stamp quads sample the backend's stamp atlas (after the
    // model cockpit, like the markers). With stamps->fresh the recording first copies stamps->copy from `markers` into the atlas and clears stamps->clear
    // in the marker swapchain image after its blit. null = off (key-off recording unchanged).
    const acz::mk360::StampPlan* stamps = nullptr;
    // DECOUPLE (decouple:1): per eye b2 (Reproject; flags 0 = off: the eye source is sampled at the pixel's own uv, exactly as before).
    std::array<Reproject, 2> reproject{};
};
// GPU-COST pass marks (timestamp indices). Per eye e: kPassEyePre(e) after the model cockpit depth pre-pass (= the previous mark when the pre-pass is off),
// kPassEyeComposite(e) after the eye composite draw (terrain/sky/clouds/water/props merge), kPassEyeModel(e) after the model's opaque + blend passes,
// kPassEyeGlass(e) after the canopy glass pass. kPassProps follows the WORLD-3 props raster pass (both eyes).
constexpr unsigned kPassStart = 0, kPassProps = 1, kPassEnd = 10, kPassMarks = 11;
constexpr unsigned kPassEyePre(unsigned e) { return 2 + 4 * e; }
constexpr unsigned kPassEyeComposite(unsigned e) { return 3 + 4 * e; }
constexpr unsigned kPassEyeModel(unsigned e) { return 4 + 4 * e; }
constexpr unsigned kPassEyeGlass(unsigned e) { return 5 + 4 * e; }
struct PassTiming { bool available = false; uint64_t hz = 0; uint64_t ticks[kPassMarks] = {}; };
// The two images of one png_once capture: RGBA8 (sRGB-encoded as the swapchain RTV writes them), tight rows, row 0 = top.
struct PngCapture {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> final_rgba;   // eye 0 exactly as the compositor receives it (premultiplied rgb over black, alpha shown as 255)
    std::vector<uint8_t> alpha_rgba;   // eye 0 in debug view 1 (cockpit coverage as grey)
    std::vector<uint8_t> worldfx_rgba; // eye 0 in debug view 3 (the world layer only: world + the effects layer composited over it in mode 3)
    // LAYER-DUMP (runLOCK2: the eye images hold neither the HUD nor the marker quad layer, the compositor adds them): the HUD layer image and the marker
    // layer image exactly as rendered for their quads (premultiplied over black; the marker image WITHOUT the NBOX stamp cut-outs). Empty = not rendered.
    uint32_t lw = 0, lh = 0;
    std::vector<uint8_t> hud_rgba, markers_rgba;
};
struct RecordFacts {
    bool ok = false;
    // The recorded state sequence returns every source image to RENDER_TARGET
    // (depth to DEPTH_WRITE) and never transitions a swapchain image.
    bool sources_restored = false, swapchain_untouched = false;
    // The depth was actually bound for coverage (false: absent/unreadable depth,
    // opaque alpha=1 fallback; the projection is then submitted without blending).
    bool depth_used = false;
    // J-hud-consumer: the HUD source was readable and drawn into the HUD image. false
    // with use_hud: the acquired HUD image was only cleared to transparent and the
    // frame submits NO HUD layer (counted hud-invalid); the frame itself proceeds.
    bool hud_used = false;
    bool markers_used = false;   // J-hud-markers: same contract as hud_used for the marker image
    bool effects_used = false;   // J-effects-layer: the effects image was readable and composited (mode 3); false: mode 2 exactly as without it
    bool smoke_used = false;     // EFFECTS-OUTSIDE (C) fxmem:4: both eye smoke images readable and composited (kModeBitSmoke); false: no smoke bit
    bool sprite_used = false;    // EFFECTS-OUTSIDE (F) fxmem:6: both eye sprite images readable and composited (kModeBitSprite); false: no sprite bit
    bool terrain_used = false;    // J-terrain-render: the terrain constants/SRVs were bound for the eyes (resources exist and were uploaded)
    bool terrain_uploaded = false; // J-terrain-render: this recording carried the one-time upload
    bool props_used = false;       // WORLD-3: the props pass ran and the eyes merged it (b1 bit3); false = terrain only, exactly as without props
    bool props_uploaded = false;   // WORLD-3: this recording carried the one-time texture upload
    uint32_t props_instances = 0, props_batches = 0, props_draws = 0;   // WORLD-3: instances drawn (per eye), model batches, draw calls (both eyes)
    bool props_pending = false;    // PERF (props_async): the props GPU init is still running on its worker; this job drew the terrain without props
    bool props_ready = false;      // PERF (props_async): the worker had finished successfully when this job was recorded
    bool props_failed = false;     // PERF (props_async): the worker finished but the pipeline/static resources could not be built (props stay off)
    uint32_t props_init_ms = 0;    // PERF (props_async): worker duration (start to finish), valid with props_ready / props_failed
    bool cockpit_used = false;     // COCKPIT-2: the model cockpit was drawn over the eyes (resources exist); false = the replayed cockpit exactly as with the key off
    bool tac_used = false;         // TACTICAL: the overlay pipeline existed and the job's overlay vertices were uploaded and drawn
    uint32_t tac_vertices = 0;     // TACTICAL: overlay vertices drawn (all targets)
    bool stamp_used = false;       // NBOX (mk360:3): the stamp pipeline + atlas existed and the eye stamps were drawn
    bool stamp_copied = false;     // NBOX: this fresh plan's copies were recorded (false with copies planned = the consumer invalidates those cells)
    uint32_t stamp_copies = 0, stamp_clears = 0;   // NBOX: copies into the atlas / rectangles cleared in the marker swapchain image
    bool cockpit_uploaded = false; // COCKPIT-2: this recording carried the one-time upload
    bool cockpit_anim_uploaded = false; // COCKPIT-4: this recording copied the posed animated range into the VB
    bool cockpit_refl_uploaded = false; // SKIN-REFL: this recording copied a (new) stage reflection map into t5
    bool cockpit_refl_used = false;     // SKIN-REFL: the eye draws ran with kCFlagRefl (a map was resident)
    bool cockpit_prepass = false;  // GPU-COST zpre: the eyes were drawn model-opaque first with the depth-tested composite (BlitJob::cockpit_prepass honoured)
    bool png_recorded = false;  // J-test8-fixes: the png_once copies were recorded (png_take returns them after completion)
    bool world_captured = false; // J-world-hold: the held image now holds this job's world (world_capture succeeded)
    bool held_used = false;      // J-world-hold: the held image was sampled (world_held and a valid held image existed)
    bool cache_read = false;     // J-world-cache: the eye pass sampled the cache (resource exists, finite parameters); false = extend/transparent periphery
    bool cache_written = false;  // J-world-cache: this recording updated the cache from the job's world
    uint64_t cache_bytes = 0;    // J-world-cache: bytes of the persistent cache (colour + stamp), 0 when none
    unsigned barriers_out = 0, barriers_back = 0, draws = 0, clears = 0;
};
// Owner-thread only, every call through Gate::call. Implemented by
// xr_stereo_d3d12.cpp on the game's device with proxy-owned objects only.
struct BlitBackend {
    virtual ~BlitBackend() = default; // deliberately no foreign cleanup
    virtual bool setup() = 0;  // private fence, root signature, PSO, heaps, allocator, closed list
    virtual bool bind(unsigned target, const SwapchainImages&, uint32_t w, uint32_t h, int64_t format) = 0;
    // Reset + record the job into the ONE private list (not closed).
    virtual RecordFacts record(const BlitJob&) = 0;
    virtual bool close() = 0;                                   // actual Close HRESULT
    virtual bool fence_completed(void* fence, uint64_t value) = 0; // CPU check of the producer fence (no wait)
    virtual bool gpu_wait(void* fence, uint64_t value) = 0;    // queue->Wait on the game's queue
    // ORIGINAL (unhooked) ExecuteCommandLists of the one list. `dispatch_possible`
    // is set before the boundary: an uncertain call may already reference images.
    virtual bool execute(bool& dispatch_possible) = 0;
    virtual bool signal(uint64_t value) = 0;                   // private fence, game's queue
    // Nonblocking GetCompletedValue + device-removed check. false = invalid sample.
    virtual bool sample(uint64_t& completed) = 0;
    virtual GpuTiming timing() { return {}; } // cached getter, no foreign call; unavailable never fails a frame
    // J-test8-fixes: after the private fence proved completion of a recording with png_recorded, copy its two images out ONCE and release the private
    // target/readback. false = nothing to take / unreadable (diagnostic only, never fails a frame).
    virtual bool png_take(PngCapture&) { return false; }
    // GPU-COST: the pass timestamps of the last COMPLETED recording made with BlitJob::pass_timing (false = none / unavailable). Diagnostic only.
    virtual bool pass_timing(PassTiming&) { return false; }
    virtual bool destroy() = 0;                                // private objects only, after completion
};

// --------------------------------------------------------------- consumer
// HUD placement. world_quad (default): EXACTLY the world quad's LOCAL pose and size
// (same anchor, lens and distance), so the HUD symbology overlays the world image it
// was drawn for. head_locked: FALLBACK only, a VIEW-space quad hud_head_distance_m ahead,
// hud_head_width_m wide with the source aspect; HUD/world alignment does NOT hold in it.
// J-hud-placement: the "EXACTLY" holds while xr_hud_quad_distance_m is unset and xr_hud_scale = 1;
// a configured distance keeps the direction/angular size, a scale shrinks/grows it about the centre.
enum class HudMode : uint32_t { world_quad = 0, head_locked = 1 };
const char* hud_mode_name(HudMode) noexcept; // "world-quad" / "head-locked" (the hud_mode config values)
struct Settings {
    // Completion windows (step14 run 2): the blit is executed on the GAME's DIRECT
    // queue, so its completion includes every game frame already queued ahead of
    // it. D-207's 100 ms came from the standalone probe (idle GPU); in-game at
    // ~13 published/s with a 2-frame GPU backlog it quarantined the FIRST frame.
    // Steady state stays inside the 250 ms progress rule (samples refresh progress;
    // the window bounds how long one XR frame may block). The first submission of
    // the session also pays first-use costs (new swapchain images, PSO) and gets a
    // longer window, capped so the first XR frame keeps first_end_reserve_ms of the
    // owner's configured startup budget for its xrEndFrame. Insufficient budget
    // for the steady window is refused before submission. Expiry still quarantines.
    int64_t completion_window_ms = 200;
    int64_t first_completion_window_ms = 1000;
    int64_t first_end_reserve_ms = 1000;
    int64_t sample_spacing_ms = 1;      // D-207 FenceCaps::sample_ms
    int64_t retire_sample_ms = 20;
    int64_t wait_retry_ns = 20000000;   // second xrWaitSwapchainImage after a 0 ns timeout
    // Issue 29: re-submit the last layers (their OWN pose/FOV) only while age <=
    // this, tracking is valid and the LOCAL space generation is unchanged.
    int64_t reuse_max_age_ms = 500;
    int64_t summary_ms = 5000;          // per-frame summary period
    uint64_t milestone_frames = 300;    // one provisional-result line after N submitted layers
    uint32_t max_extent = 8192;
    uint32_t cap_width = 2400;          // configurable swapchain width cap
    // EYE-RES (eyeres:<pct> inside xr_terrain_stage, eye_res.h; default 0 = the source-aspect policy above, every line as before): the two EYE
    // swapchains are the runtime's recommended view size x pct/100 (50..200), clamped to the runtime max and eyeres::kPixelBudget. Quads unchanged.
    uint32_t eye_res_pct = 0;
    uint32_t assumed_src_w = 2560, assumed_src_h = 1440; // compatibility defaults; proxy overwrites with published metadata before launch
    float quad_distance_m = 40.0f;      // world quad distance (tunable; key xr_world_quad_distance_m, J-feather)
    // J-feather: edge feather (key xr_edge_feather, fraction of the layer half-extent,
    // 0 = OFF). > 0: every blit ramps its premultiplied output toward the image border,
    // the world quad is submitted with SOURCE_ALPHA and the cockpit projection blends even
    // without depth coverage. `comfort_log` (either J-feather key configured) appends the
    // active values to the first-layer line; both 0/false = every line/layer as before.
    float edge_feather = 0.0f;
    bool comfort_log = false;
    Tonemap tonemap = Tonemap::clamp;   // FP16 -> display (the game's post-process is unknown)
    float exposure = 1.0f;
    bool world_quad = true;             // create the quad swapchain / submit world when valid
    // J-hud-consumer (config key xr_hud_layer, default OFF; hud_mode). OFF: no HUD
    // swapchain, no HUD acquire/blit/layer, every line and layer exactly as before.
    bool hud_layer = false;
    HudMode hud_mode = HudMode::world_quad;
    float hud_head_distance_m = 2.5f;   // head-locked fallback: VIEW-space distance (-Z)
    float hud_head_width_m = 1.6f;      // head-locked fallback: width; height from the source aspect
    // J-hud-placement (keys xr_hud_quad_distance_m / xr_hud_scale / xr_hud_head_distance_m /
    // xr_hud_head_width_m; the last two set the fields above). 0 = unset: the world-quad HUD
    // stays at the world quad's distance. `hud_placement_log` (any of the keys configured)
    // appends the active values to the HUD lines; unset/false = every line as before.
    float hud_quad_distance_m = 0.0f;
    float hud_scale = 1.0f;
    bool hud_placement_log = false;
    // J-hud-markers (key xr_hud_markers_layer, default OFF; needs hud_layer in world-quad mode, else forced off by the
    // constructor): a marker quad with EXACTLY the world quad's pose/size/anchoring (never scaled, never at the HUD distance).
    bool hud_markers_layer = false;
    // J-world-in-projection (key xr_world_in_projection, default OFF = the world quad layer exactly as
    // before): no world quad swapchain/layer; the world image is composited into the per-eye projection
    // images under the cockpit (see WorldComposite). Needs world_quad (the world source) to be true.
    bool world_in_projection = false;
    // J-world-hold (key xr_world_hold_last, default OFF; needs world_in_projection): a frame without a world keeps compositing the last good
    // world image for kWorldHoldMs, fading to black over its second half. J-world-extend (key xr_world_extend, default OFF; needs
    // world_in_projection): the periphery outside the world window is filled (see world_ray_extend). Both are forced off without it.
    bool world_hold_last = false;
    bool world_extend = false;
    // J-world-cache (key xr_world_cache, default OFF; forced off without world_in_projection): world-registered environment cache (see above).
    bool world_cache = false;
    // J-occlusion-fix (key xr_world_window_occlude, default OFF; latched by the diagnostics only with xr_world_in_projection AND the latched
    // xr_world_objects_depth_fix, else never set; forced off here without world_in_projection): replayed world objects are hidden inside the world window.
    bool world_window_occlude = false;
    // WORLD-1: the producer's xr_world_objects_depth_fix is latched (replayed object depth = 2^-10 x the game's ACM depth), so the consumer can decode an object's view
    // depth for the terrain occlusion test. Set by the proxy from the diagnostics key; false = no terrain occlusion of objects.
    bool objects_depth_fix = false;
    // J-effects-layer (key xr_effects_layer, default OFF; needs world_in_projection, else forced off by the constructor): the producer's effects
    // image (StereoFrame::effects) is composited over the world INSIDE the world-in-projection pass (shader mode 3), behind the cockpit.
    bool effects_layer = false;
    // J-terrain-render (keys xr_terrain_render + xr_terrain_stage, default OFF = null; needs world_in_projection, else forced off by the constructor).
    std::shared_ptr<const TerrainRuntime> terrain;
    // COCKPIT-2 (key xr_cockpit_model, default OFF = null; needs world_in_projection, else forced off by the constructor).
    std::shared_ptr<const CockpitRuntime> cockpit;
    // AIRCRAFT-AUTO (xr_cockpit_model=auto; default null = off; needs world_in_projection, else forced off). It sets `cockpit` itself (the proxy leaves
    // `cockpit` null with auto).
    std::shared_ptr<const CockpitAuto> cockpit_auto;
    // J-test8-fixes (keys xr_debug_view / xr_png_once, both default OFF/0, latched with xr_stereo_output; see J-test8-fixes-findings.md): debug_view 0..4 replaces the
    // eye targets' output by a diagnostic colour (see debug_view_rgb); png_once captures the final composited eye-0 image (+ its coverage) ONCE after
    // png_after_new_frames fresh frames and hands the pixels to png_sink (the proxy encodes and writes them on a worker thread; the owner never waits).
    // PERF: the sink may TAKE the pixels (swap/move out of `rgba`, no copy); the consumer never reads them after the call.
    uint32_t debug_view = 0;
    bool png_once = false;
    uint64_t png_after_new_frames = 240;
    std::function<bool(const std::string& name, std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)> png_sink;
    // Timing-anchor diagnostics (docs/review/timing-anchor-fixtures.md; NO config key: the proxy latches it with xr_look_drive_keys, default OFF).
    // ON: one correlated "event=select-corr" record per selected frame, capped: the first kSelCorrFirst, then at most one per
    // kSelCorrPeriodMs, never more than kSelCorrCap in total. OFF: no record, every other line and decision byte-identical.
    // mono_us (optional, tests): the steady_clock-microsecond source used for the consumer's selection stamp.
    bool select_corr_log = false;
    std::function<int64_t()> mono_us;
    // POV fixes (docs/review/pov-fixes-impl.md; defaults = exactly the previous behaviour). lean (key gpu_copy_step5_neck_lever_m): the world, marker
    // and composite anchors are shifted by pov::frame_layer_shift so they stay fixed relative to the producer's virtual eye; the HUD quad only with
    // hud_follow >= 1 (key xr_hud_follow, world-quad mode only): 1 = shifted like the world quads, 2 = shifted and turned about the anchor by a
    // rate-limited fraction of the rendered look (pov::hud_follow_step).
    pov::Lean lean{};
    unsigned hud_follow = 0;
    // VSCREEN (xr_terrain_stage vscreen:1; xr_vscreen.h; default null = off, nothing changes): the game's screen outside flight. Needs hud_layer
    // (its swapchain carries the screen image); the constructor drops it without.
    std::shared_ptr<VScreenHook> vscreen;
    // SCREEN-DUMP (xr_screen_dump.h; needs vscreen + png_sink): the trigger file png-screen.now. consume=false: does it exist; consume=true:
    // delete it. Polled at most once per second and only while the screen is shown; null (default, tests) = never.
    std::function<bool(bool consume)> screen_dump_trigger;
    // FLIGHT-DUMP (effects-outside-window.md 20.8): the trigger file png-eye.now. While png_once is on and its capture is FINISHED (taken or given up),
    // a fresh flight frame that finds the file consumes it and re-arms png_once for one more capture (the same three eye-0 images, named by frame id).
    // consume=false: does it exist; consume=true: delete it. null (default, tests) = never; png_once off = never polled.
    std::function<bool(bool consume)> png_eye_trigger;
    std::shared_ptr<FlightStageHook> flight_stage;   // VSEARLY: per-flight terrain (null = off; xr_flight_stage.h)
    bool pov = false;   // VIEW-MODE (xr_terrain_stage pov:1; view_mode.h; default off): view classifier + flight-stage keep-alive (needs flight_stage)
    bool pov_stage2 = false;   // POV-RENDER stage 2 (pov:2; needs pov): the chase view in stereo from no-cockpit producer frames (other-povs.md section 10)
    bool pov_stage3 = false;   // POV-RENDER stages 3 + 4 (pov:3; implies pov_stage2): + HUD-only, in-engine cinematics, the drawn vignette (section 11)
    acz::povr::Options pov_render{};   // POV-RENDER stage 1 (pov_render.h; default off; needs pov): census + rig DRY RUN (logs only, no render change)
};
inline constexpr unsigned kSelCorrFirst = 8, kSelCorrCap = 400;
inline constexpr int64_t kSelCorrPeriodMs = 500;
// ";hud-quad-distance-m=D|world;hud-scale=S;hud-head-distance-m=H;hud-head-width-m=W" (J-hud-placement).
std::string hud_placement_tokens(const Settings&);
struct Counters {
    uint64_t terrain_frames = 0, terrain_no_camera = 0, terrain_failed = 0; // J-terrain-render (only move with Settings::terrain)
    // WORLD-1 camera provenance (only move with Settings::terrain): orientation from the frame's game camera / from E6; the game camera refused against E6
    // (angle > gmax); position from the DrawACM_v camera / from E6; the frame-synchronous E6 sample used; the largest game-vs-E6 angle seen (centi-degrees).
    uint64_t terrain_src_game = 0, terrain_src_e6 = 0, terrain_game_refused = 0, terrain_eye_game = 0, terrain_eye_e6 = 0, terrain_e6_frame = 0, terrain_e6_angle_max_cdeg = 0;
    uint64_t terrain_occlude_frames = 0;
    uint64_t lf_frames = 0, lf_why[9] = {};   // LENS-FLARE diagnostics: composites with the terrain, and the per-frame reason (acz::lensflare::Why)
    uint64_t terrain_e6_implausible = 0;   // E6-PLAUSIBLE: frames whose E6 eye was > kE6EyeAgreeUnits from the game eye (E6 not allowed to veto the game camera)
    uint64_t cockpit_frames = 0, cockpit_no_view = 0, cockpit_failed = 0;   // COCKPIT-2 (only move with Settings::cockpit)
    uint64_t cockpit_stage_light = 0;   // COCKPIT-3: model frames lit by the stage sun (else the fixed light)
    uint64_t cockpit_fid_frames = 0, cockpit_shadow_frames = 0;   // CFID: frames drawn with kCFlagFid / kCFlagShadow
    uint64_t cockpit_refl_frames = 0, cockpit_refl_uploads = 0, cockpit_refl_nomap = 0;   // SKIN-REFL: drawn with the map / map copies / srefl:1 frames without a map
    uint64_t cockpit_anim_frames = 0, cockpit_anim_updates = 0;   // COCKPIT-4: frames drawn with a posed animated range; E6 samples fed to anim_update
    uint64_t frames = 0, should_render_false = 0, discarded = 0;
    uint64_t submitted = 0, reused = 0, zero_no_frame = 0, zero_no_pose = 0, stale_zero = 0, new_frames = 0;
    uint64_t stale_age = 0, stale_tracking = 0, stale_space = 0, reuse_age_max_ms = 0, deferred = 0;
    // Issue 29 verdict 2: FRESH frames refused (released unused) by the current admission.
    uint64_t fresh_stale_space = 0, fresh_stale_tracking = 0, space_unknown = 0, world_lens_invalid = 0, anchor_rebases = 0;
    // DECOUPLE review 3 (Astra open risk 1): FRESH frames whose source images were not provably the published bytes (FrameSource::images_unchanged
    // false). pre = before any GPU work (released unused); post = after the composite's GPU completion (its layer was never submitted).
    uint64_t fresh_rewritten_pre = 0, fresh_rewritten_post = 0;
    uint64_t acquired_frames = 0, released_frames = 0, partial_frames = 0;
    uint64_t world_incomplete_frames = 0, world_lost_draws = 0; // J-180: StereoFrame::world_incomplete / world_lost (0 with the producer key off)
    uint64_t pose_from_frame = 0, pose_from_located = 0;
    uint64_t lens_frames = 0, depth_frames = 0, world_frames = 0;
    uint64_t gpu_waits = 0, cpu_presynced = 0, fence_lag_max = 0;
    uint64_t completion_ms_max = 0, completion_ms_sum = 0;
    uint64_t wait_frame_ms_max = 0, end_frame_ms_max = 0;
    uint64_t end_failed = 0, last_frame_id = 0, frame_id_gaps = 0;
    // J-hud-consumer (only move with Settings::hud_layer). hud_frames: consumed frames whose
    // HUD was blitted; hud_layers: xrEndFrame-accepted submissions that carried the HUD quad
    // (new or re-submitted); hud_skipped: consumed frames submitted WITHOUT a HUD (no
    // hud_valid, refused HUD, lens invalid in world-quad mode, backend refusal); of those,
    // hud_invalid = hud_valid but refused (resource/format/alias/backend), hud_lens_invalid =
    // world-quad mode without a valid lens.
    uint64_t hud_frames = 0, hud_layers = 0, hud_skipped = 0, hud_invalid = 0, hud_lens_invalid = 0;
    // J-cockpit-fov-consumer: cockpit_fov_frames = new layers submitted with the frame's own
    // cockpit_fov (instead of the lens); cockpit_fov_invalid = flagged frames refused (no layer).
    uint64_t cockpit_fov_frames = 0, cockpit_fov_invalid = 0;
    // J-quad-yaw: anchors captured from the producer's neutral (StereoFrame::neutral_valid), and
    // eye-midpoint anchors replaced by a later neutral of the SAME space generation.
    uint64_t anchor_neutral = 0, anchor_neutral_upgrades = 0;
    // J-hud-markers (only move with Settings::hud_markers_layer). marker_frames: consumed frames whose marker image was
    // blitted; marker_layers: xrEndFrame-accepted submissions that carried the marker quad (new or re-submitted);
    // marker_skipped: consumed frames submitted WITHOUT markers (no hud_markers_valid, refused image, lens invalid, backend
    // refusal); marker_invalid = valid but refused (resource/format/alias/backend); marker_lens_invalid = lens refused.
    uint64_t marker_frames = 0, marker_layers = 0, marker_skipped = 0, marker_invalid = 0, marker_lens_invalid = 0;
    // J-world-in-projection (only move with Settings::world_in_projection): frames whose world was
    // composited into the eye images; world frames that could not be (no resolvable eye views / degenerate
    // ray parameters) and were submitted without a world.
    uint64_t world_composite_frames = 0, world_composite_refused = 0;
    // J-world-hold (only move with Settings::world_hold_last): world_captured = frames whose world was copied into the held image;
    // world_held = frames composited from the held image (no fresh world); world_hold_expired = frames with no world and nothing
    // to hold (never captured, expired, or capture not valid); world_hold_capture_failed = capture requested but not done.
    uint64_t world_captured = 0, world_held = 0, world_hold_expired = 0, world_hold_capture_failed = 0;
    uint64_t world_hold_max_age_ms = 0;
    // J-world-cache (only move with Settings::world_cache): world_cache_frames = recordings whose eye pass sampled the cache; world_cache_written = recordings
    // that updated it; world_cache_skipped = fresh frames not written (look lost after having been valid); world_cache_failed = cache requested but not usable.
    uint64_t world_cache_frames = 0, world_cache_written = 0, world_cache_skipped = 0, world_cache_failed = 0;
    uint64_t world_filter_frames = 0;   // WORLD-WINDOW FILTER: composite recordings with the filter bit (eyeres > 0 only)
    // J-effects-layer (only move with Settings::effects_layer): effects_frames = consumed frames whose effects image was composited (mode 3);
    // effects_skipped = consumed frames composited WITHOUT effects (no effects_valid, held/absent world, no composite); effects_invalid = valid but
    // refused (resource/format/alias/backend).
    uint64_t effects_frames = 0, effects_skipped = 0, effects_invalid = 0;
    uint64_t smoke_seen = 0, smoke_frames = 0;   // EFFECTS-OUTSIDE (C) fxmem:4: fresh frames with smoke_valid / whose eye smoke images were composited
    uint64_t sprite_seen = 0, sprite_frames = 0; // EFFECTS-OUTSIDE (F) fxmem:6: the same for the eye sprite images
    // J-test8-fixes (only move with Settings::effects_layer): the PRODUCER's side of every consumed frame, so a 5 s summary line carries it (test 8 logs ended
    // without effects-result): effects_seen = consumed frames, effects_prod_valid = of those with effects_valid, effects_prod_draws / _refused / _tail = sums of
    // StereoFrame::effects_draws / effects_refused / effects_tail over the valid frames.
    uint64_t effects_seen = 0, effects_prod_valid = 0, effects_prod_draws = 0, effects_prod_refused = 0, effects_prod_tail = 0;
};
// Ledger state of one per-frame transaction (exposed for tests).
struct Ledger {
    bool holding = false;                 // producer slot HELD by us
    stereo::StereoFrame frame{};
    std::array<bool, kTargets> used{}, acquired{}, waited{}, release_attempted{}, released{};
    std::array<uint32_t, kTargets> index{};
    bool recorded = false, closed = false, submitted = false, signalled = false, complete = false;
    bool ambiguous = false;
    uint64_t value = 0;
    int64_t signal_ms = 0;                // owner time of the private Signal (diagnostics)
};

using Log = std::function<void(bool lifecycle, const char* line)>;
using SourceProvider = std::function<stereo::FrameSource*()>;

class Output final : public xr_owner::FrameConsumer {
public:
    Output(SourceProvider, std::shared_ptr<SwapchainRuntime>, std::shared_ptr<BlitBackend>, Log, Settings = {});
    bool setup(Gate&) override;
    void frame(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&) override;
    void ended(Gate&, Rc, bool projection, int64_t end_ms) override;
    void session_event(Gate&, const xr_owner::Event&) override;
    void end_diagnostics(int64_t, bool, double) override;
    bool retire(Gate&) override;
    bool failed() const noexcept override { return failed_.load(std::memory_order_acquire); }
    void finished(const xr_owner::Report&) noexcept override;

    const Counters& counters() const noexcept { return c_; }
    // CAM-EPOCH (terrain conv.cam_admit): cumulative admission statistics, the held world image's camera snapshot, the lag meter.
    struct CamStats {
        uint64_t src[4]{};                                                     // admitted E6 samples by acz::terrain::CamSource (0 unused)
        uint64_t refused[unsigned(acz::terrain::Admit::count)]{};              // frames left without an E6 camera, by the final reason
        uint64_t frame_refused[unsigned(acz::terrain::Admit::count)]{};        // the frame's own sample was present but refused (fallback then tried)
        uint64_t game_only = 0;          // E6 refused, terrain drawn from the frame's own game camera + DrawACM_v eye (frame-correlated by construction)
        uint64_t held = 0;               // held-world frames (their image keeps the snapshot of the frame it was captured from)
        uint64_t anim_epoch_resets = 0;  // COCKPIT-4 anim state restarted on an epoch change
        uint64_t mode[unsigned(acz::terrain::CamMode::count)]{};   // composed frames by the camera mode actually used
        uint64_t occlude_refused = 0;    // terrain-occludes-objects refused: the terrain camera was an E6-only display fallback
        uint64_t transitions = 0;        // outcome changes (one camera-admit line each, capped)
        double age_max_ms = 0;           // the oldest ADMITTED sample (relative to its reference)
    };
    const CamStats& camera_stats() const noexcept { return cam_; }
    bool held_camera(acz::terrain::CamSample& out) const noexcept { if (hold_cam_valid_) out = hold_cam_; return hold_cam_valid_; }
    acz::terrain::CamMode held_camera_mode() const noexcept { return hold_cam_mode_; }
    uint64_t held_camera_frame() const noexcept { return hold_cam_fid_; }
    const acz::terrain::CamLagMeter& camera_lag() const noexcept { return lag_; }
    const Ledger& ledger() const noexcept { return l_; }
    const std::string& failure() const noexcept { return reason_; }
    // PERF: the consumer refused the FIRST frame because the owner's startup budget was already spent before submission (cold start: runtime/session
    // creation and the cold PSO compile of setup). Nothing was submitted; retry_allowed(attempt, report, this) treats it as a cold-start timeout.
    bool startup_budget_refused() const noexcept { return startup_budget_refused_.load(std::memory_order_acquire); }
    // LOOK-STICK GATE (look_stick_option.h, docs/world-render/look-stick-gate.md): composites whose terrain was drawn from a game/E6 camera (in flight).
    // Read by the XR glue's ~100 ms loop (another thread): it publishes the right-stick gate heartbeat to the XINPUT1_4.dll proxy while this count moves.
    uint64_t flight_composites() const noexcept { return flight_composites_.load(std::memory_order_relaxed); }
    int64_t selected_format() const noexcept { return format_; }
    const std::array<ViewConfig, 2>& views() const noexcept { return view_cfg_; }
    const BlitMapping& mapping(unsigned target) const noexcept { return map_[target % kTargets]; }
    Extent extent(unsigned target) const noexcept { return ext_[target % kTargets]; }
    std::string result_line(const xr_owner::Report*) const;

private:
    void fail(const char* why);
    void emit(bool lifecycle, const std::string&) const;
    void emit_lines(bool lifecycle, const std::string&) const;   // one emit per newline-separated line
    bool validate(const stereo::StereoFrame&, const char*& why) const;
    bool transact(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&);
    bool observe_completion(Gate&, int64_t deadline_ms, int64_t spacing_ms);
    void settle_without_gpu(Gate&); // no GPU reference exists: release images + slot
    bool release_images(Gate&);
    void release_slot();
    void summary(Gate&, bool force);
    bool layer_from(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&, bool depth, bool world, bool hud, bool composite = false, bool markers = false,
                    bool fresh = true);   // DECOUPLE: fresh = false for a re-composite (no new-frame counters, the reuse age keeps the game frame's time)
    bool composite_views(const xr_owner::FrameInfo&, std::array<xr_owner::EyeSample, 2>&) const; // layer_from's views, no side effects
    void anchor(const xr_owner::FrameInfo&, const xr_owner::LayerPlan&);
    // F-LEAN: the anchor shifted by this frame's virtual-eye offset (out = neutral_ + shift). False (use neutral_ unchanged) when the lean is
    // off, the anchor is not the producer's neutral, or the frame carries no valid pose/neutral.
    bool lean_anchor(const stereo::StereoFrame&, float out[3]) const;
    pov::HudFollow hud_follow_state_{};
    uint32_t hud_follow_gen_ = 0;
    void summary_hud(int64_t now);
    void pace(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&);
    // VSCREEN (xr_vscreen.h; only with Settings::vscreen): the screen layer for a no-new-frame XR frame / vspov:flat / retire.
    bool vscreen_pace(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&);
    bool vscreen_exclusive() const noexcept;
    bool vscreen_retire(Gate&);
    void screen_dump(Gate&, bool shown);   // SCREEN-DUMP: trigger poll (<= 1/s) + the finished parts to png_sink
    // Timing-anchor diagnostics (select_corr_log): the capped record. `fresh` = a newly selected producer frame (its facts are captured
    // into corr_), else the last selected frame's facts re-reported for a re-submitted layer.
    bool corr_due(Gate&);
    int64_t mono_now_us() const;
    void corr_emit(Gate&, const xr_owner::FrameInfo&, const xr_owner::LayerPlan&, bool fresh, int world_state);
    std::string eye_fov_tokens() const;     // ";eye0-fov=l,r,u,d;eye1-fov=..." (degrees) of the located views

    SourceProvider provider_;
    std::shared_ptr<SwapchainRuntime> xr_;
    std::shared_ptr<BlitBackend> blit_;
    Log log_;
    Settings s_;
    Counters c_{};
    Ledger l_{};
    std::atomic<bool> failed_{false};
    std::atomic<bool> startup_budget_refused_{false};   // PERF (see startup_budget_refused())
    std::atomic<uint64_t> flight_composites_{0};       // LOOK-STICK GATE (see flight_composites())
    bool props_ready_logged_ = false;                   // PERF: the one props-ready / props-init-failed line
    std::string reason_;
    bool setup_attempted_ = false, chains_[kTargets] = {false, false, false, false, false}, blit_setup_ = false, destroyed_ = false;
    bool png_done_ = false;                 // J-test8-fixes: the png_once capture was taken (or given up after png_attempts_)
    unsigned png_attempts_ = 0;
    bool png_eye_refused_logged_ = false;   // FLIGHT-DUMP: one refused line per stuck png-eye.now
    unsigned png_eye_rearms_ = 0;
    int64_t dump_check_ms_ = INT64_MIN / 2, dump_ms_ = 0;   // SCREEN-DUMP: last trigger poll; the active dump's arm time
    bool dump_active_ = false, dump_consume_logged_ = false;
    bool plan_markers_ = false, first_marker_logged_ = false, first_marker_layer_logged_ = false; // J-hud-markers
    uint32_t last_marker_draws_ = 0, last_marker_refused_ = 0;                                        // producer diagnostics of the last marker frame
    bool plan_hud_ = false;                 // the plan handed to the owner this frame carries the HUD quad
    bool first_hud_logged_ = false, first_hud_layer_logged_ = false, first_world_incomplete_logged_ = false;
    bool cockpit_fail_logged_ = false; // COCKPIT-2
    acz::cockpit::AnimState anim_state_;            // COCKPIT-4 anim:1 (attitude rates -> commands)
    std::vector<acz::cockpit::GpuVertex> anim_pose_; // the posed animated range of the current job
    // STORES-RT (store_tracker.h): the consumer-side tracker of the installed model's weapon stations; configured per cockpit generation.
    acz::stores::Tracker stores_;
    uint64_t stores_gen_ = ~0ull;
    bool stores_sticky_ = false;        // a station has been hidden since the install: keep posing (and uploading) every frame so the range can return to rest
    uint32_t stores_mask_ = 0;          // the mask the current job was posed with
    uint64_t stores_changes_ = 0;       // mask transitions since the install (the log counts them)
    // SP-STORES: the same for the special-weapon group (sp<station> parts, driven by the HUD SP row through acz::stores::sp_signal()).
    acz::stores::Tracker sp_stores_;
    uint64_t sp_stores_gen_ = ~0ull;
    bool sp_stores_sticky_ = false;
    uint32_t sp_stores_mask_ = 0;
    uint64_t sp_stores_changes_ = 0;
    bool stwatch_wedge_prev_ = false;   // stwatch:1: the firing wedge's last state (edges are logged for correlation with ee_store_watch decrements)
    uint64_t anim_last_seq_ = 0;
    // TAKEOFF (docs/world-render/takeoff-stage.md): mission stage vs the takeoff airbase stage (s_.terrain->takeoff), per fresh composite
    acz::terrain::TakeoffSelector takeoff_;
    void takeoff_sample(const float eye[3]);
    const TerrainRuntime& drawn_terrain() const noexcept { return s_.terrain->takeoff && takeoff_.base_active() ? *s_.terrain->takeoff : *s_.terrain; }
    // CAM-EPOCH: admission of this frame's E6 sample (false = no E6 camera; src says which sample was admitted), its statistics and transitions.
    bool admit_camera(const stereo::StereoFrame& f, acz::terrain::CamSample& cs, acz::terrain::CamSource& src);
    void camera_outcome(const stereo::StereoFrame& f, acz::terrain::CamMode mode);
    acz::terrain::CamMode cam_mode_ = acz::terrain::CamMode::none;   // this frame's camera mode
    // camlag evidence bursts (offline analysis: tools/live/camera-lag-analyze.py)
    bool camlag_active();
    void camlag_frame(const stereo::StereoFrame& f, bool e6ok, const acz::terrain::CamFrame& ef);
    void lensflare_diag(const std::vector<float>& env, uint64_t gen, uint32_t env_bits, const std::array<TerrainEye, 2>& te, const std::array<WorldComposite, 2>& cp);   // LENS-FLARE diagnostics
    void camlag_reuse(int64_t age_ms);
    void camlag_close();
    struct CamlagBurst { unsigned frames = 0, present = 0, reused = 0, refused = 0, missing = 0, present_gaps = 0, dropped = 0, mode[unsigned(acz::terrain::CamMode::count)]{}; };
    CamlagBurst camlag_b_{};
    bool camlag_in_burst_ = false, camlag_header_ = false;
    int64_t camlag_burst_start_ms_ = -1, camlag_last_p_ns_ = 0;
    uint64_t camlag_last_p_seq_ = 0;
    unsigned camlag_burst_frames_ = 0, camlag_lines_ = 0, camlag_bursts_ = 0;
    uint64_t last_cam_fid_ = 0, last_cam_seq_ = 0;
    acz::terrain::CamMode last_cam_mode_ = acz::terrain::CamMode::none;
    bool e6_far_prev_ = false;   // E6-PLAUSIBLE transition latch
    float lf_peak_ = 0.0f, lf_cos_best_ = -1.0f, lf_sin_el_ = 0.0f;   // LENS-FLARE: per-window peak pre-occlusion gate, best cos(sun, eye axis), the stage sun's sin(elevation)
    uint64_t lf_cfg_gen_ = ~uint64_t(0);   // the stage generation whose lensflare-config line was written
    // WCLOUD-FIX2 telemetry (wcloud:1 only): one CPU probe of eye 0 per summary window (world_clouds_probe), written as event=summary-wcloud
    bool wc_probe_due_ = true;
    unsigned long long wc_probe_n_ = 0;
    float wc_probe_eye_[3] = {0, 0, 0};
    uint64_t wc_probe_gen_ = 0;
    acz::terrain::WcProbe wc_probe_{};
    acz::terrain::CamMode hold_cam_mode_ = acz::terrain::CamMode::none;
    uint64_t hold_cam_fid_ = 0;
    acz::terrain::CamPick cam_pick_{};      // this frame's admission result (camera_outcome reads it)
    void camera_lag_frame(const stereo::StereoFrame& f, bool e6ok, const acz::terrain::CamFrame& ef, double age_ms);
    CamStats cam_{}, cam_prev_{};
    int cam_last_key_ = -1;
    unsigned cam_lines_ = 0;
    uint64_t cam_lines_suppressed_ = 0;
    bool anim_epoch_known_ = false;
    uint32_t anim_lock_ = 0, anim_stage_ = 0;
    acz::terrain::CamLagMeter lag_;
    // the camera snapshot of the frame whose world image is held (J-world-hold): captured with the image, never replaced by a newer sample
    bool hold_cam_valid_ = false;
    acz::terrain::CamSample hold_cam_{};
    bool frame_cam_valid_ = false;          // this frame's admitted snapshot (for the hold capture)
    acz::terrain::CamSample frame_cam_{};
    bool frame_ep_valid_ = false;           // review 2: the epochs this fresh composite's camera was admitted under (DECOUPLE retains them)
    uint32_t frame_ep_lock_ = 0, frame_ep_stage_ = 0;
    // EXTRAP (xr_terrain_stage extrap:1, default OFF; frame_pacing.h, docs/world-render/uncap.md): the terrain camera of each NEW producer frame is
    // dead-reckoned to this XR frame's predicted display time (horizon = select-age, clamped); TACTICAL markers lead by their own velocity.
    struct ExtrapState {
        acz::pacing::MotionExtrap ex;
        bool config_logged = false;
        double lead_s = 0;   // this frame's applied horizon (s), read by tac_frame
        uint64_t frames = 0, translated = 0, rotated = 0, clamped = 0, no_horizon = 0, refused = 0, markers_led = 0;
        double horizon_sum_ms = 0, shift_sum = 0, shift_max = 0;
        const char* last_why = "none";
        int64_t last_summary_ms = 0;
    } extrap_;
    void extrap_camera(const stereo::StereoFrame& f, int64_t now_ms, acz::terrain::CamFrame& cf);
    // OBJECT-INTERP (xr_terrain_stage interp:1, default OFF; object_interp.h, docs/world-render/interp.md): the producer drew the replayed objects at the
    // frame's t_display as the camera moved by StereoFrame::interp (dt, dR) sees them; the terrain camera takes the SAME delta (instead of extrap:1), and
    // the consumer publishes its select-age EMA as the producer's horizon.
    struct InterpState {
        acz::interp::HorizonEma ema;
        bool config_logged = false;
        uint64_t frames = 0, planned = 0, cam_moved = 0, k_pos = 0, objects = 0, moved = 0, appeared = 0, teleports = 0, scale = 0, held = 0, lost = 0,
                 write_fail = 0, no_facts = 0;
        double k_sum = 0, horizon_sum = 0, shift_sum = 0, shift_max = 0;
        const char* last_why = "none";
        int64_t last_summary_ms = 0;
        int64_t next_trace_ms = 0; uint32_t trace_left = 0;   // interp-trace windows: 120 consecutive frames every 20 s
    } interp_;
    // true = the frame carried the producer's plan and the camera took its delta (extrap:1 must then not move it again).
    bool interp_camera(const stereo::StereoFrame& f, int64_t now_ms, acz::terrain::CamFrame& cf, bool fixed_alt);
    // DECOUPLE (xr_terrain_stage decouple:1, default OFF; docs/world-render/decouple.md). The newest game frame's producer slot stays HELD (dc_.frame)
    // after its first composite; every following XR frame without a new game frame composes again from it with that XR frame's own head pose (P1 =
    // the located views), re-projecting the objects image from the render pose P0 (rotation only) and dead-reckoning the terrain camera to the display
    // time. The slot is released exactly once: when a newer frame is admitted, when it grows older than dcage (game stall: the old re-submit path takes
    // over), on a space-generation change, on a lock/stage epoch change of its camera, when the producer re-executed its recording (FrameSource::
    // images_unchanged false), or at retire. Key off: nothing below runs (transact/pace exactly as before).
public:
    struct DecoupleStats {
        uint64_t composites = 0, fresh = 0, recomposed = 0, reuse_fallback = 0, released = 0, released_superseded = 0, released_stall = 0, released_space = 0,
                 released_retire = 0, retained = 0, not_retained = 0, no_views = 0, untracked = 0, reproj_source = 0, reproj_window = 0, reproj_identity = 0,
                 reproj_refused = 0, cam_extrap = 0, cam_held = 0, cam_none = 0, views_xr = 0,
                 released_camera = 0,      // review 2 (Astra P2): the admitted camera's lock/stage epoch changed while held
                 released_rewritten = 0,   // review 2 (Astra P2): the producer re-executed the held slot's recording (images no longer the frame's)
                 rewritten_dropped = 0;    // a re-composite whose source was rewritten during it: its layer was not submitted
        double horizon_sum_ms = 0, age_max_ms = 0, rot_max_deg = 0;
    };
    const DecoupleStats& decouple_stats() const noexcept { return dcs_; }
    bool decouple_holding() const noexcept { return dc_.held; }
    uint64_t decouple_frame_id() const noexcept { return dc_.held ? dc_.frame.frame_id : 0; }
private:
    struct DecoupleState {
        bool held = false;                       // dc_.frame's slot is HELD by the decouple path (not by the ledger)
        stereo::StereoFrame frame{};
        int64_t fresh_ms = 0;                    // owner time of its first composite
        std::array<xr_owner::EyeSample, 2> p0{}; // the render pose/FOV of the images (the old composite_views rule at the first composite)
        bool cam_valid = false;                  // the cameras below are valid (terrain composite)
        acz::terrain::CamFrame cam_obj{};        // the camera the objects image was drawn with (frame camera + the interp delta when applied)
        acz::terrain::CamFrame cam_fresh{};      // the terrain camera of the first composite (fallback when the dead reckoning refuses)
        // review 2 (Astra P2): the epoch identity the first composite's camera was admitted under (cam_ep_valid: CAM-EPOCH admission ran) and the
        // LOCAL space generation P0 belongs to (also for an unposed frame, whose P0 = the located views of that XR frame). Checked before every re-composite.
        bool cam_ep_valid = false;
        uint32_t cam_lock = 0, cam_stage = 0, space_gen = 0;
        bool plan_hud = false, plan_markers = false;   // the first composite's plan carried the HUD / marker quads (their images stay in their swapchains)
        acz::pacing::MotionExtrap ex;            // per-game-frame dead reckoning of the frame camera (fed once per new frame)
        bool configured = false, logged = false;
        // this composite (set by transact / no_new_frame, read by composite_views / layer_from / compose)
        bool views_on = false;                   // compose with views_ (P1) instead of the frame's render pose
        std::array<xr_owner::EyeSample, 2> views{};
        int64_t last_summary_ms = 0;
        uint64_t window_composites = 0, window_fresh = 0;
    } dc_;
    DecoupleStats dcs_{}, dcs_prev_{};
    bool dc_enabled() const noexcept { return s_.terrain && s_.terrain->pacing.decouple && s_.world_in_projection; }
    void dc_release(const char* why);            // releases dc_.frame's slot (once)
    void no_new_frame(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&);   // key off: exactly pace()
    // P1 = this XR frame's located poses with the render FOV of P0 (the swapchain mapping and lens stay those of the images)
    bool dc_views(const xr_owner::FrameInfo& info, const std::array<xr_owner::EyeSample, 2>& p0);
    bool dc_camera(const stereo::StereoFrame& f, const xr_owner::FrameInfo& info, acz::terrain::CamFrame& out, double& horizon_ms);
    void summary_decouple(int64_t now);
    bool compose(Gate&, const xr_owner::FrameInfo&, xr_owner::LayerPlan&, bool fresh, bool presynced);
    // TACTICAL (tac:1; tactical.h). tac_frame() runs once per consumed producer frame (after the terrain camera is known) and fills tac_overlay_.
    struct TacState {
        acz::tac::Tracker tracker;
        acz::tac::FiringDetector fire;
        acz::tac::TracerSim tracers;
        acz::tac::Overlay overlay;
        std::vector<acz::tac::Blip> blips;
        std::vector<acz::tac::WorldMarker> markers;
        bool config_logged = false, have_prev = false;
        double prev_t = 0; float prev_pos[3] = {}, vel[3] = {};
        uint64_t frames = 0, no_frame = 0, no_camera = 0, blips_sum = 0, markers_sum = 0, tracer_sum = 0, overlay_sum = 0, overlay_full = 0, backend_used = 0,
                 backend_refused = 0, wedge_frames = 0, firing_frames = 0, hud_absent = 0, markers_skipped_game = 0;
        int64_t last_sample_ms = 0;
        acz::mk360::Tracker mk;   // MARKERS-360 (mk360:1|2|3; untouched with it off)
        bool mk_config_logged = false;
        acz::mk360::StampPlan stamps;   // NBOX (mk360:3): this game frame's stamps (re-composites draw them again; copies/clears only with the fresh frame)
        uint64_t nb_backend_used = 0, nb_backend_refused = 0, nb_copies_recorded = 0, nb_clears_recorded = 0;
        uint64_t nb_refuse[unsigned(acz::mk360::NboxRefuse::kCount)] = {};   // NBOX: refusals by reason (classify_nbox_refusal)
    } tac_;
    // Returns the overlay to hand to the backend (nullptr = nothing to draw). terrain_ok/te/wc: this frame's terrain eyes and world composites.
    // markers_live (NBOX mk360:3): this composite blits the frame's own marker image (the stamps' live source).
    const acz::tac::Overlay* tac_frame(const stereo::StereoFrame& f, bool terrain_ok, const std::array<TerrainEye, 2>& te,
                                       const std::array<WorldComposite, 2>& wc, const acz::terrain::CamFrame& cam, bool hud, bool rebuild_only = false,
                                       bool markers_live = false);
    void summary_tac(int64_t now);
    // AIRCRAFT-AUTO
    struct AutoLoad { std::shared_ptr<CockpitRuntime> rt; std::string why; double ms = 0; };
    void cockpit_config_lines();               // the cockpit-model / cockpit-model-c4 lines of the current s_.cockpit
    void cockpit_auto_tick();                  // samples -> detector -> lazy load / install / unload (once per frame)
    void cockpit_auto_install(AutoLoad&& r, int type);
    acz::aircraft::Detector auto_det_;
    std::future<AutoLoad> auto_future_;
    int auto_loading_ = -1, auto_installed_ = -1, auto_logged_chosen_ = -3;
    acz::aircraft::Verdict auto_logged_verdict_ = acz::aircraft::Verdict::unstable;
    uint64_t auto_failed_mask_ = 0, auto_last_seq_ = 0, auto_loads_ = 0, auto_load_failures_ = 0, auto_unloads_ = 0, auto_samples_ = 0;
    // AIRCRAFT-AUTO-FAST
    void cockpit_auto_remember(int type);      // writes the remembered-aircraft file (worker thread when async) when the type differs
    void cockpit_auto_feed(const acz::aircraft::EyeSample& e);
    std::string auto_assume_line_;             // the constructor's assume result, emitted by setup()
    int auto_remembered_ = -1;
    uint64_t auto_lost_ = 0, auto_confirms_ = 0, auto_writes_ = 0;
    struct AutoWriteStats { std::atomic<uint64_t> ok{0}, failed{0}; };   // shared with the detached writer (never touches the Output)
    std::shared_ptr<AutoWriteStats> auto_wstats_ = std::make_shared<AutoWriteStats>();
    bool auto_first_frame_logged_ = false;
    double auto_assume_ms_ = 0;
    double auto_last_sample_ms_ = -1;          // AIRCRAFT-AUTO-STALE: clock of the last locked E6 sample (or of the first tick)
    uint64_t auto_stale_forgets_ = 0, auto_stale_seen_samples_ = 0;
    uint64_t auto_vouch_samples_ = 0, auto_implausible_ = 0, auto_stale_seen_junk_ = 0;   // AIRCRAFT-AUTO-JUNK: samples that may refresh the stale clock / junk-lock samples
    uint32_t last_hud_draws_ = 0, last_hud_refused_ = 0; // producer diagnostics of the last HUD frame
    // HUD-MENU (hud_menu.h, docs/world-render/hud-menu.md): pause-menu mode state (owner thread) and the HUD lens/scale/distance the plan uses.
    acz::hudmenu::Detector menu_det_{};
    bool menu_on_ = false;
    uint64_t menu_frames_ = 0, menu_glass_frames_ = 0;
    uint64_t pause_frames_ = 0;   // UI3D stage 3a: composites whose eye images got the pause-menu layers (the HUD quad dropped)
    unsigned menu_logged_ = 0;
    const acz::cockpit::Options* hud_menu_options() const noexcept;
    void hud_menu_frame(const acz::stereo::StereoFrame& f, uint32_t hud_draws);
    // The world-quad HUD's lens tangents, scale and distance for this frame (menu mode / glass-pivot scale / unchanged).
    void hud_quad_params(const float lens[4], float out_lens[4], float& scale, float& distance) noexcept;
    bool first_frame_logged_ = false, milestone_logged_ = false, first_xr_frame_logged_ = false;
    bool completion_seen_ = false;          // a submission completed inside its window (first-window policy)
    uint64_t sample_count_ = 0;             // private-fence samples taken (diagnostics)
    int64_t first_wait_ms_ = 0, first_begin_ms_ = 0, first_t0_ = 0;
    int64_t format_ = 0;
    std::array<ViewConfig, 2> view_cfg_{};
    std::array<SwapchainImages, kTargets> images_{};
    std::array<Extent, kTargets> ext_{};
    bool eye_res_used_ = false;             // EYE-RES: the eye extents came from the recommended view size (eye_res.h)
    bool eye_ring_logged_ = false;          // EYE-RING: the eye-ring-consumer line was written (only frames with per-eye images of another size)
    std::array<BlitMapping, kTargets> map_{};
    uint32_t mapped_w_ = 0, mapped_h_ = 0;
    uint64_t last_id_ = 0, issued_ = 0, last_completed_ = 0;
    int64_t last_sample_ms_ = -1000000;
    bool have_last_ = false, ever_new_ = false, neutral_set_ = false;
    float neutral_[3] = {0, 0, 0};
    // J-quad-yaw: the anchor came from the producer's neutral (else the first frame's eye midpoint,
    // yaw 0 = identity orientation exactly as before) and its heading about +Y.
    bool anchor_neutral_ = false;
    float neutral_yaw_ = 0.0f;
    std::string quad_anchor_tokens() const; // ";quad-anchor=neutral|eye-midpoint;neutral-yaw-deg=..;neutral-pos=x,y,z"
    int64_t last_new_ms_ = 0;
    int64_t last_cockpit_ms_ = 0; bool ever_cockpit_ = false;   // POV-RENDER stage 2: the last fresh frame WITH cockpit members (the classifier's evidence)
    uint64_t pov_nc_frames_ = 0;                                 // POV-RENDER stage 2: no-cockpit chase frames composed
    int64_t pov_last_nc_ms_ = -1;                                // POV-RENDER pov:3: consumer time of the newest no-cockpit frame (the NcGate evidence)
    acz::povr::NcGate pov_gate_;                                 // POV-RENDER pov:3: flat for 1 s when a no-cockpit view gets no frames
    acz::povr::Route pov_route_ = acz::povr::Route::none;        // POV-RENDER pov:3: this XR frame's route and rig (the vignette of the composed frame)
    acz::povr::RigOut pov_rig_{};
    struct PovStage3Counts { uint64_t req[3]{}, vig_frames = 0, blink_frames = 0, a_only = 0; float vig_max = 0; int64_t last_ms = -1; } pov3_{};   // summary-pov-stage3
    uint32_t pov_req_ = 0;                                       // POV-RENDER pov:3: the request sent this XR frame (A-only vignette, cinematic dumps)
    acz::povr::EventBlink pov_event_blink_rig_;                  // POV-RENDER pov:3 A-only: blink on the producer anchor's events
    bool pov_event_blink_ = false;
    acz::povr::CineDump pov_cine_dump_;                          // POV-RENDER pov:3: in-DLL eye dump of each cinematic visit (xr_png_once)
    acz::viewmode::Tracker pov_;   // VIEW-MODE (Settings::pov only)
    acz::povr::Driver pov_render_;  // POV-RENDER stage 1 (Settings::pov_render.active() only)
    uint32_t last_gen_ = 0;                 // LOCAL space generation the last layers were submitted under
    uint32_t neutral_gen_ = 0;              // space generation the world-quad anchor was captured in
    xr_owner::Fov lens_fov_{};
    bool lens_used_ = false;
    // J-cockpit-fov-consumer: the headset's own per-eye FOV from the latest valid located views
    // (diagnostics only) and the FOV source of the last NEW layer: 0 pose, 1 lens, 2 hmd.
    bool hmd_fov_valid_ = false;
    std::array<xr_owner::Fov, 2> hmd_fov_{};
    unsigned cockpit_fov_source_ = 0;
    xr_owner::LayerPlan last_plan_{};
    // J-world-hold: the held world image's state as the consumer knows it (owner thread).
    bool hold_valid_ = false;
    int64_t hold_ms_ = 0;
    float hold_lens_[4] = {0, 0, 0, 0};
    float hold_look_[2] = {0, 0}; // xr_look_drive_keys: the held world's rendered look (yaw, pitch)
    // Timing-anchor diagnostics: the selected frame's facts (copied; the StereoFrame is released with its slot) and the record limiter.
    struct Corr {
        bool valid = false;
        uint64_t frame_id = 0, pose_seq = 0, generation = 0, pose_predicted = 0, xr_frame = 0;
        int64_t xr_predicted = 0, t_start_us = 0, t_publish_us = 0, t_select_us = 0;
        unsigned members = 0, start = 0, look_src = 0;
        bool partial = false, look_meas_valid = false, look_valid = false;
        int world = 0; // 0 none, 1 fresh, 2 held
        float look[2] = {0, 0}, look_meas[2] = {0, 0}, look_est[2] = {0, 0};
        char refusal[40] = {0};
    } corr_, corr_cand_; // corr_ = the last SUBMITTED frame; corr_cand_ = the frame being selected
    unsigned corr_count_ = 0;
    int64_t corr_last_ms_ = -1000000;
    int64_t cache_epoch_ms_ = -1;      // J-world-cache: the stamp clock starts at 1.0 s on the first cache frame (stamp 0 = never written)
    bool cache_look_seen_ = false;     // J-world-cache: a valid rendered look has been seen (a later loss skips the write instead of writing at look 0)
    // J-layer-fov (diagnostics): the last submitted layer fov / swapchain / pixels-per-tan line, once per summary period.
    std::string layer_fov_line(const xr_owner::LayerPlan&) const;
    double gpu_start_sum_ = 0, gpu_blit_sum_ = 0;
    uint64_t gpu_samples_ = 0;
    double period_sum_ = 0, period_max_ = 0, end_late_max_ = 0;
    uint64_t period_samples_ = 0, late_samples_ = 0;
    int64_t frame_cpu_max_ = 0, blocked_max_ = 0;
    uint64_t select_lag_ = 0;
    double select_age_ms_ = 0;
    bool select_age_available_ = false;
    int64_t last_summary_ms_ = 0;
    Counters last_summary_{};
};

// ------------------------------------------- process registry (proxy glue)
// The producer registers its FrameSource once from its own owner context;
// the object must stay valid for the process lifetime (never unregistered
// while the owner thread may use it). Atomic pointer; no lock.
void register_frame_source(stereo::FrameSource*) noexcept;
stereo::FrameSource* registered_frame_source() noexcept;
// The producer reads the XR owner's latest pose snapshot here (try-only,
// never waits; false when no owner runs or the slot is retracted).
bool try_read_pose(xr_owner::PoseSnapshot&) noexcept;
// Proxy glue: publish/retain the running owner for try_read_pose. A later
// publication (a retry) replaces the pointer readers see. Owners stay retained
// until release_owner() (R-5): only for a FINISHED owner, and only once no pose
// reader can still hold its pointer (xr_owner_registry.h); otherwise it stays
// retained as before. Returns the outcome name for the log.
void publish_owner(const std::shared_ptr<xr_owner::Owner>&) noexcept;
const char* release_owner(const std::shared_ptr<xr_owner::Owner>&, int64_t max_wait_ms) noexcept;
struct OwnerCounts { uint64_t published = 0, released = 0, busy = 0; size_t retained = 0; };
OwnerCounts owner_counts() noexcept;

// --------------------------------------------- start / retry policy (pure)
// Lazy start: the XR session is started only once the producer has published
// at least one StereoFrame. One retry after a failed first attempt, only for a
// cold-start timeout (startup deadline, READY timeout or progress deadline)
// with no loss observed and a clean retirement (every handle retired, so a
// second session on the same device/queue is legal).
bool start_ready(const stereo::FrameSource*) noexcept;
bool retry_allowed(unsigned attempt, const xr_owner::Report&) noexcept;
// PERF: the same policy, plus: a consumer failure (Cause::consumer) counts as a cold-start timeout when the consumer reports
// `consumer_startup_budget` (Output::startup_budget_refused(): the first frame was refused before ANY submission because the startup
// deadline was spent in setup). Every other condition (attempt 1, complete, clean, no loss, nothing held, not a success) is unchanged.
bool retry_allowed(unsigned attempt, const xr_owner::Report&, bool consumer_startup_budget) noexcept;
// Best-effort cooperative stop of the running owner (no wait, no join).
void request_stop() noexcept;

// ------------------------------------------- producer fence registration
// Issue 29 (verdict 2): the trust anchor for a GPU queue Wait. The PRODUCER calls
//   acz::xr_stereo::register_producer_fence(fence, epoch)
// whenever it creates or replaces its private fence (fence = its ID3D12Fence*,
// epoch = the non-zero value it also writes into StereoFrame::fence_epoch;
// nullptr clears). A frame's fence is trusted only when (fence, epoch) equals the
// current registration; the first published frame never establishes trust.
// Default (nothing registered): every GPU wait is refused; frames proceed only
// after CPU-observed completion, otherwise they are deferred.
void register_producer_fence(void* fence, uint64_t epoch) noexcept;
bool producer_fence_registered(const void* fence, uint64_t epoch) noexcept;
// StereoFrame::space_generation encoding: PoseSnapshot::space_generation + 1 (0 = unknown).
inline uint32_t encode_space_generation(uint32_t g) noexcept { return g == UINT32_MAX ? 0u : g + 1u; }
} // namespace acz::xr_stereo
