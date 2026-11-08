// J-xr-stereo-output (revision 3): FP16 linear source -> sRGB swapchain RTV.
// Compiled at build time by the Windows SDK fxc into embedded bytecode headers
// (no runtime compiler). One fullscreen triangle per target; the viewport is
// the full swapchain image (xr_stereo::full_rect), uv spans [0,1] over it.
// CPU mirror: xr_stereo::shade() (tested offline).
//   colour: c = max(rgb * exposure, 0) (NaN -> 0), tonemap 0 = clamp (saturate),
//           1 = Reinhard c/(1+c). Linear output; the sRGB RTV encodes once.
//   alpha : use_depth ? (depth > 0 ? 1 : 0) : 1. The replay alpha is NOT a
//           coverage mask (D-229); the replay depth is (cleared to 0 for the
//           PS2 GREATER test, D-236). Output is PREMULTIPLIED (rgb * a, a).
Texture2D<float4> source_image : register(t0);
Texture2D<float> source_depth : register(t1);   // depth plane SRV (or a null SRV)
SamplerState linear_clamp : register(s0);
//   mode  : 0 = cockpit/world (above). 1 = HUD (J-hud-consumer, CPU mirror
//           xr_stereo::shade_hud): the source is ALREADY premultiplied (PS2 HUD
//           blended into a zero-cleared target) and its alpha is not a reliable
//           coverage mask, so a = saturate(max(max(src.a, 0), max(c.r, c.g, c.b)))
//           after the same exposure/tonemap; output (c, a), c <= a.
//   feather: J-feather (key xr_edge_feather, CPU mirror xr_stereo::edge_ramp /
//           apply_feather): 0 = OFF, the output is returned untouched. > 0 (fraction
//           of the layer half-extent, <= 0.25): per axis d = 2*min(uv, 1-uv),
//           t = saturate(d/feather), ramp = t*t*(3-2t); the PREMULTIPLIED output of
//           every mode is multiplied by ramp.x*ramp.y (fade toward the border).
//           mode 2 (J-world-in-projection, key xr_world_in_projection; CPU mirror xr_stereo::
//           world_ray_uv / composite_over_world, constants packed by world_composite_params): the
//           cockpit eye image (mode 0 output, feathered by its own uv) is composited OVER the world
//           plane seen along this pixel's view ray:  out = cockpit + (1 - cockpit.a) * world, all
//           premultiplied. world = tonemapped world texel (alpha 1) at the ray/plane intersection,
//           multiplied by the same edge ramp over the WORLD uv; transparent (0) off the plane extents,
//           for d.z >= -eps or a hit behind the eye. Anchor-frame ray: d = w_base + uv.x*w_ux + uv.y*w_vy,
//           s = (-w_dist - w_origin.z)/d.z, h = w_origin + s*d, wuv = (h.x*w_au + w_bu, h.y*w_av + w_bv).
//           Constants after b0[4] (17 floats, packed so no float3 straddles a register):
//           w_origin(3) | w_base(3) w_dist | w_ux(3) w_au | w_vy(3) w_bu | w_av w_bv.
//           J-world-hold / J-world-extend (CPU mirror xr_stereo::world_hold_fade / world_ray_extend): b0[22] w_fade multiplies the world
//           RGB (1 = off; the held image fades to black), b0[23] w_extend != 0 selects world_layer_ext (periphery fill outside the window,
//           window rays unchanged, no edge feather on the world). Key off: w_fade = 1, w_extend = 0 -> the code path below is numerically identical.
Texture2D<float4> world_image : register(t2);   // the world snapshot (a null SRV outside mode 2)
//           mode 4 (J-effects-dual): mode 3 whose effects texel is the producer's coverage_v1 image (P premultiplied colour, A RGB coverage; CPU mirror
//           xr_stereo::effects_coverage_over_world): layer = P' + (1 - A) * world with P' = tonemap(P / A) * A. Chosen from StereoFrame::effects_repr.
//           mode 3 (J-effects-layer, key xr_effects_layer, CPU mirror xr_stereo::effects_over_world): mode 2 plus the producer's EFFECTS image
//           (t3: missiles, explosions, tracers = the non-member scene draws between the cockpit members, premultiplied, alpha not a coverage mask)
//           composited over the world texel at the SAME world uv (the effects are screen space like the world): e = HUD-mode shade of the texel
//           (a = saturate(max(a, max(rgb)))), times the world window's edge ramp, layer = e + (1 - e.a) * world; the cockpit goes over that.
//           In the extend periphery the effects are NOT sampled (window rays only). Modes 0/1/2 never read t3.
Texture2D<float4> effects_image : register(t3);  // the effects image (a null SRV outside mode 3)
// J-world-cache (key xr_world_cache, CPU mirrors world_cache_* in xr_stereo_output.cpp; docs/review/world-cache-impl.md). t4..t6 are bound only while the cache
// exists (null SRVs otherwise and w_cache = 0, so no cache code runs). cache_color = R11G11B10_FLOAT environment image (raw world rgb, equirectangular in the
// COCKPIT frame), cache_stamp = R32_FLOAT write time in s (<= 0 = never written), cache_fill = 4x1 RGBA16F: texel 0 sky rgb, 1 ground rgb, 2 (horizon elevation
// in the cache frame, confidence, 0, 1). Entry points: ps_main (reads, mode 2/3/4 eyes with w_cache != 0), ps_cache_update (MRT write of the cache, scissored to
// the window's bounding rectangle), ps_cache_fill (3-pixel draw: sky/ground/horizon of the fresh window).
// J-terrain-render (key xr_terrain_render, default OFF; CPU reference acz::terrain::trace in terrain_stage.cpp, docs/world-render/terrain-render-impl.md).
// t7 = heights R8_UINT 513x513, t8 = tile words R16_UINT 256x256, t9 = tile atlas Texture2DArray RGBA8 (7 mips). b1 = per-eye terrain constants (bit0 of t_bits
// = enabled; zero when off, so no terrain code runs and every other path is unchanged).
Texture2D<uint> t_height : register(t7);
Texture2D<uint> t_words : register(t8);
Texture2DArray<float4> t_atlas : register(t9);
// WORLD-2 (docs/world-render/world2-impl.md): t10 = environment RGBA32F 256x257 (row 0 = parameters P[i], rows 1.. = maps), read with Load only. t_bits bits 3..6 =
// sky / light / cloud / water (acz::terrain::kEnv* << 3); all zero = the legacy terrain shading exactly (no env code runs).
Texture2D<float4> t_env : register(t10);
cbuffer Terrain : register(b1) {
    float3 t_eye; float t_cell;          // eye in stage units | world units per height cell
    float3 t_right; float t_hstep;       // camera basis in stage space (mirror/roll/yaw conventions applied on the CPU) | units per height byte
    float3 t_up; float t_half;           // | terrain half extent
    float3 t_fwd; float t_sea;           // | sea level (valid when bit2)
    float t_hazel; float t_far; float t_hmax; uint t_bits;   // haze length L, march end (WORLD-1), max terrain height (units), bit0 enabled, bit1 blend window, bit2 sea present, bit13 sea tile-masked (D-344), bit14 DTEX detail slices
};
Texture2D<float4> cache_color : register(t4);
Texture2D<float> cache_stamp : register(t5);
Texture2D<float4> cache_fill : register(t6);
// J-test8-fixes (key xr_debug_view, CPU mirror xr_stereo::debug_view_rgb): the constant at b0[3] is `mode_bits` = mode | (debug_view << 8). The
// low byte is the shader mode exactly as before (0..3); the debug view (0 = off) is only ever non-zero for the eye targets (mode 0/2/3).
cbuffer Blit : register(b0) {
    uint use_depth; uint tonemap; float exposure; uint mode_bits; float feather;
    float3 w_origin; float3 w_base; float w_dist; float3 w_ux; float w_au; float3 w_vy; float w_bu; float w_av; float w_bv; float w_fade; float w_extend;
    float w_cache; float look_yaw; float look_pitch; float cache_now;   // J-world-cache (register c6): flag (0 = off), the frame's rendered look, the stamp clock (s)
};
// DECOUPLE (xr_terrain_stage decouple:1; CPU mirrors xr_stereo::reproject_uv / reproject_dir; docs/world-render/decouple.md): b2, 15 root constants, ALL ZERO
// unless the consumer composes this eye at a head pose other than the producer's render pose (flags 0: no code below runs, every path unchanged).
//   rows of H = (dc_a.xyz), (dc_a.w, dc_b.xy), (dc_b.zw, dc_c.x); q = (dc_c.yzw, dc_d.x); fade = dc_d.y; flags = asuint(dc_d.z): bit0 source, bit1 window rays.
cbuffer Decouple : register(b2) { float4 dc_a; float4 dc_b; float4 dc_c; float3 dc_d; };
#define dc_flags asuint(dc_d.z)
#define mode (mode_bits & 255u)
#define fx_mode (mode == 3 || mode == 4)   // mode 4 = mode 3 with the producer's coverage_v1 effects image (J-effects-dual)
#define dbg_view ((mode_bits >> 8) & 255u)
// J-occlusion-fix (key xr_world_window_occlude, CPU mirror xr_stereo::window_occlude_keep): mode_bits bit 16, set only for the eye targets of the world
// composite with use_depth. An object-band depth (0 < depth < 2^-10, the xr_world_objects_depth_fix scale) inside the world WINDOW is hidden (the
// snapshot already holds it correctly occluded); a seam ramp over the window edge, periphery and cockpit depth untouched. Flag clear: no code runs.
#define occlude_bit ((mode_bits & 65536u) != 0u)
static const float kOccludeObjectDepth = 0.0009765625;
static const float kOccludeSeam = 0.1;
// COCKPIT-2 (key xr_cockpit_model, CPU mirror xr_stereo::model_cockpit_cover): mode_bits bit 17 while the model cockpit is drawn over this eye. The replayed
// image keeps only its object band (0 < depth < 2^-10, replayed world objects); its cockpit band (or everything, without a depth plane) gets coverage 0.
#define model_bit ((mode_bits & 131072u) != 0u)
struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    const float2 t = float2((id << 1) & 2, id & 2);       // (0,0) (2,0) (0,2)
    o.uv = t;
    o.position = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
float4 feathered(float4 o, float2 uv) {
    if (!(feather > 0.0)) return o;                     // OFF: unchanged output
    const float2 d = 2.0 * min(uv, 1.0 - uv);           // 0 at the border, 1 at the centre
    const float2 t = saturate(d / feather);
    const float2 r = t * t * (3.0 - 2.0 * t);
    return o * (r.x * r.y);                              // premultiplied: scale rgb and a
}
float smooth01(float e0, float e1, float x) {
    const float t = saturate((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}
// DECOUPLE: the P0 source uv of this P1 eye pixel (rotation-only re-projection through H) and its edge fade (0 outside the source image: no stretch).
float2 dc_source_uv(float2 uv, out float fade) {
    fade = 1.0;
    if ((dc_flags & 1u) == 0u) return uv;
    const float3 d = w_base + uv.x * w_ux + uv.y * w_vy;
    const float3 p = float3(dc_a.x * d.x + dc_a.y * d.y + dc_a.z * d.z, dc_a.w * d.x + dc_b.x * d.y + dc_b.y * d.z, dc_b.z * d.x + dc_b.w * d.y + dc_c.x * d.z);
    if (!(p.z > 1e-6)) { fade = 0.0; return float2(0.5, 0.5); }
    const float2 s = p.xy / p.z;
    if (!(s.x >= 0.0 && s.x <= 1.0 && s.y >= 0.0 && s.y <= 1.0)) fade = 0.0;
    else if (dc_d.y > 0.0) {
        const float2 t = saturate(min(s, 1.0 - s) / dc_d.y);
        const float2 r = t * t * (3.0 - 2.0 * t);
        fade = r.x * r.y;
    }
    return saturate(s);
}
// DECOUPLE: a world-window / effects ray of this P1 pixel in the cockpit frame of the camera the game images were drawn with (q = the camera delta).
float3 dc_rotate(float3 d) {
    if ((dc_flags & 2u) == 0u) return d;
    const float3 q = float3(dc_c.y, dc_c.z, dc_c.w);
    const float w = dc_d.x;
    const float3 t = float3(q.y * d.z - q.z * d.y + w * d.x, q.z * d.x - q.x * d.z + w * d.y, q.x * d.y - q.y * d.x + w * d.z);
    return d + 2.0 * float3(q.y * t.z - q.z * t.y, q.z * t.x - q.x * t.z, q.x * t.y - q.y * t.x);
}
// WORLD-WINDOW FILTER (eyeres > 0 only; CPU mirror acz::eyeres::filter_select / filter_offset in eye_res.h; docs/world-render/eye-resolution.md 3b):
// mode_bits bit 21 (xr_stereo::kModeBitWorldFilter) on the composite eye targets. The world-window texel of world_layer_window is a 2x2 grid of bilinear taps over this
// eye pixel's footprint in the world image (Jacobian from two extra plane hits one eye pixel right / down). Bit clear, px zero, a neighbour ray that
// misses the plane, or a footprint <= 1.25 texels: exactly the old single SampleLevel. The extend / cache window rays keep one tap (adding the filter
// there too broke unrelated modes on WARP again; eye-resolution.md 3b). px = eye uv per eye pixel, taken once at the top of ps_main_body (ddx/ddy of the
// interpolated uv, uniform control flow) and passed down as a parameter (a written static global broke every mode on WARP; not used).
#define wfilter_bit ((mode_bits & 2097152u) != 0u)
static const float kWFOneTap = 1.25;
// Straight-line on purpose (no loops, no out-parameter helpers): a looped / unrolled-grid version passed fxc but broke unrelated modes on WARP
// (eye-resolution.md 3b). k = 0 (bit clear, px zero, a neighbour ray off the plane, or a footprint <= kWFOneTap texels on both axes): the first tap
// is wuv + 0 = exactly the old single SampleLevel and nothing else is sampled.
float3 world_rgb(float2 uv, float2 wuv, bool rot, float2 px) {
    float2 ju = float2(0.0, 0.0), jv = float2(0.0, 0.0);
    float k = 0.0;
    [branch] if (wfilter_bit && px.x > 0.0 && px.y > 0.0) {
        const float3 d0 = w_base + uv.x * w_ux + uv.y * w_vy;
        float3 dx = d0 + px.x * w_ux, dy = d0 + px.y * w_vy;
        if (rot) { dx = dc_rotate(dx); dy = dc_rotate(dy); }
        const float sx = (-w_dist - w_origin.z) / min(dx.z, -1e-6), sy = (-w_dist - w_origin.z) / min(dy.z, -1e-6);
        const float2 hx = (w_origin + sx * dx).xy, hy = (w_origin + sy * dy).xy;
        ju = float2(hx.x * w_au + w_bu, hx.y * w_av + w_bv) - wuv;
        jv = float2(hy.x * w_au + w_bu, hy.y * w_av + w_bv) - wuv;
        uint ww, wh;
        world_image.GetDimensions(ww, wh);
        const float2 sz = float2(float(ww), float(wh));
        const bool valid = dx.z < -1e-6 && dy.z < -1e-6 && sx > 0.0 && sy > 0.0;
        k = valid && max(length(ju * sz), length(jv * sz)) > kWFOneTap ? 0.25 : 0.0;
    }
    const float2 o1 = k > 0.0 ? k * (ju + jv) : float2(0.0, 0.0), o2 = k > 0.0 ? k * (ju - jv) : float2(0.0, 0.0);
    float3 c = world_image.SampleLevel(linear_clamp, wuv + o1, 0).rgb;
    [branch] if (k > 0.0) {
        c += world_image.SampleLevel(linear_clamp, wuv - o1, 0).rgb;
        c += world_image.SampleLevel(linear_clamp, wuv + o2, 0).rgb;
        c += world_image.SampleLevel(linear_clamp, wuv - o2, 0).rgb;
        c *= 0.25;
    }
    return c;
}
// J-world-extend: constants mirror xr_stereo::kExtend* (xr_stereo_output.h); the operations mirror world_ray_extend().
static const float kExtendMinLight = 0.30;
static const float kExtendFalloffRad = 2.2;
static const float kExtendBlurMix = 0.85;
static const float kExtendBlurRad = 0.9;
static const float kExtendBlurEdge = 0.5;
float4 effects_over(float4 base, float2 wuv, bool ramp) {
    const float4 e = effects_image.SampleLevel(linear_clamp, wuv, 0);
    if (mode == 4) {
        // J-effects-dual, CPU mirror xr_stereo::effects_coverage_over_world. coverage_v1 texel = (P premultiplied colour, A RGB coverage):
        // un-premultiply, the same exposure/tonemap as every layer, premultiply again; alpha is the coverage only (never colour or game alpha).
        const float a = saturate(max(e.a, 0.0));
        float3 ec = a > 0.0 ? max((e.rgb / a) * exposure, 0.0) : float3(0.0, 0.0, 0.0);
        ec = tonemap == 1 ? ec / (1.0 + ec) : saturate(ec);
        float4 ef = float4(ec * a, a);
        if (ramp) ef = feathered(ef, wuv);
        return ef + (1.0 - ef.a) * base;
    }
    float3 ec = max(e.rgb * exposure, 0.0);
    ec = tonemap == 1 ? ec / (1.0 + ec) : saturate(ec);
    float4 ef = float4(ec, saturate(max(max(e.a, 0.0), max(ec.r, max(ec.g, ec.b)))));
    if (ramp) ef = feathered(ef, wuv);
    return ef + (1.0 - ef.a) * base;
}
float4 world_layer_ext(float2 uv, float2 px) {
    const float3 d = w_base + uv.x * w_ux + uv.y * w_vy;
    float2 suv = float2(0.0, 0.0);
    float k = 1.0;
    bool inwin = false;
    bool plane = false;
    float2 rawP = float2(0.0, 0.0);
    if (d.z < -1e-6) {
        const float s = (-w_dist - w_origin.z) / d.z;
        if (s > 0.0) {
            const float3 h = w_origin + s * d;
            const float2 wuv = float2(h.x * w_au + w_bu, h.y * w_av + w_bv);
            if (wuv.x >= 0.0 && wuv.x <= 1.0 && wuv.y >= 0.0 && wuv.y <= 1.0) { inwin = true; suv = wuv; }
            else if (abs(wuv.x) < 1e30 && abs(wuv.y) < 1e30) { plane = true; rawP = clamp(wuv, float2(-8.0, -8.0), float2(9.0, 9.0)); }
        }
    }
    if (!inwin) {
        const float len = sqrt(dot(d, d));
        if (!(len > 1e-12)) {
            suv = float2(w_bu, w_bv);
            k = kExtendMinLight;
        } else {
            const float hn = sqrt(d.x * d.x + d.z * d.z);
            const float az = hn < 1e-6 ? 0.0 : atan2(d.x, -d.z);
            const float el = atan2(d.y, hn);
            const float2 rawA = float2(w_bu + tan(clamp(az, -1.5, 1.5)) * (w_dist * w_au), w_bv + tan(clamp(el, -1.5, 1.5)) * (w_dist * w_av));
            const float cosz = -d.z / len;
            const float wa = plane ? 1.0 - smooth01(0.0, 0.5, cosz) : 1.0;
            const float2 outP = max(max(-rawP, rawP - 1.0), 0.0);
            const float2 outA = max(max(-rawA, rawA - 1.0), 0.0);
            const float eu = outP.x + (outA.x - outP.x) * wa;
            const float ev = outP.y + (outA.y - outP.y) * wa;
            const float2 sp = saturate(rawP);
            const float2 uc = sp + (saturate(rawA) - sp) * wa;
            const float3 cdir = float3((uc.x - w_bu) / w_au, (uc.y - w_bv) / w_av, -w_dist);
            const float cl = sqrt(dot(cdir, cdir));
            const float ang = acos(clamp(dot(d, cdir) / (len * cl), -1.0, 1.0));
            k = 1.0 - (1.0 - kExtendMinLight) * smooth01(0.0, kExtendFalloffRad, ang);
            const float tb = smooth01(0.0, kExtendBlurRad, ang) * kExtendBlurMix;
            suv.x = uc.x + (0.5 - uc.x) * tb * smooth01(0.0, kExtendBlurEdge, ev) * (1.0 - smooth01(0.0, kExtendBlurEdge, eu));
            suv.y = uc.y + (0.5 - uc.y) * tb * smooth01(0.0, kExtendBlurEdge, eu) * (1.0 - smooth01(0.0, kExtendBlurEdge, ev));
        }
    }
    float3 c = max(world_image.SampleLevel(linear_clamp, suv, 0).rgb * exposure, 0.0);
    c = tonemap == 1 ? c / (1.0 + c) : saturate(c);
    float4 r = float4(c * (k * w_fade), 1.0);
    if (fx_mode && inwin) r = effects_over(r, suv, false);   // window rays only; the world has no edge feather in this mode
    return r;
}
// ---- J-world-cache. Constants mirror xr_stereo::kCache* (xr_stereo_output.h); the operations mirror the world_cache_* functions.
static const float kCachePi = 3.14159265358979;
static const float kCacheFullS = 3.0;
static const float kCacheZeroS = 12.0;
static const float kCacheFillBlurRad = 0.25;
static const float kCacheFillConfLo = 0.06;
static const float kCacheFillConfHi = 0.25;
static const float kCacheMaxValue = 60000.0;
float3 cache_to_cache(float3 d) {            // window frame -> cache frame: Ry(ly) Rx(lp) d
    const float cy = cos(look_yaw), sy = sin(look_yaw), cp = cos(look_pitch), sp = sin(look_pitch);
    const float ay = cp * d.y - sp * d.z;
    const float az = sp * d.y + cp * d.z;
    return float3(cy * d.x + sy * az, ay, -sy * d.x + cy * az);
}
float3 cache_to_window(float3 g) {           // cache frame -> window frame: Rx(-lp) Ry(-ly) g
    const float cy = cos(look_yaw), sy = sin(look_yaw), cp = cos(look_pitch), sp = sin(look_pitch);
    const float ax = cy * g.x - sy * g.z;
    const float az = sy * g.x + cy * g.z;
    return float3(ax, cp * g.y + sp * az, -sp * g.y + cp * az);
}
float2 cache_dir_uv(float3 g) {
    const float hn = sqrt(g.x * g.x + g.z * g.z);
    const float az = atan2(g.x, -g.z);
    const float el = atan2(g.y, hn);
    return float2(0.5 + az / (2.0 * kCachePi), 0.5 - el / kCachePi);
}
float cache_age_weight(float stamp) {
    if (!(stamp > 0.0)) return 0.0;
    const float age = max(cache_now - stamp, 0.0);
    return saturate((kCacheZeroS - age) / (kCacheZeroS - kCacheFullS));
}
float3 cache_fill_color(float3 g) {          // horizon-fit neutral fill: sky above, ground below, blurred over +-kCacheFillBlurRad
    const float3 sky = max(cache_fill.Load(int3(0, 0, 0)).rgb, 0.0);
    const float3 ground = max(cache_fill.Load(int3(1, 0, 0)).rgb, 0.0);
    const float2 hz = cache_fill.Load(int3(2, 0, 0)).rg;
    const float conf = saturate(hz.y);
    const float el = atan2(g.y, sqrt(g.x * g.x + g.z * g.z));
    return lerp(ground, sky, smooth01(-kCacheFillBlurRad, kCacheFillBlurRad, el - hz.x * conf));
}
float3 cache_periphery(float3 d) {           // raw (pre-exposure) rgb of the ray d outside the live window
    const float len = sqrt(dot(d, d));
    const float3 dn = len > 1e-12 ? d / len : float3(0.0, 0.0, -1.0);
    const float3 g = cache_to_cache(dn);
    uint cw = 1u, ch = 1u;
    cache_color.GetDimensions(cw, ch);
    const float2 cuv = cache_dir_uv(g);
    const float2 p = float2(cuv.x * float(cw) - 0.5, cuv.y * float(ch) - 0.5);
    const float2 fp = floor(p);
    const float2 f = p - fp;
    const int2 p0 = int2(fp);
    float3 acc = float3(0.0, 0.0, 0.0);
    float wsum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            const uint ix = (uint(p0.x + i) + cw) % cw;         // az wraps (unsigned wrap-around makes -1 -> cw - 1)
            const int iy = clamp(p0.y + j, 0, int(ch) - 1);     // the poles clamp
            const float wb = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            const float a = cache_age_weight(cache_stamp.Load(int3(int(ix), iy, 0)));
            acc += (wb * a) * max(cache_color.Load(int3(int(ix), iy, 0)).rgb, 0.0);
            wsum += wb * a;
        }
    }
    return acc + (1.0 - saturate(wsum)) * cache_fill_color(g);
}
float4 world_layer_cache(float2 uv, float2 px) {
    const float3 d = w_base + uv.x * w_ux + uv.y * w_vy;
    float4 res = float4(0.0, 0.0, 0.0, 1.0);
    bool inwin = false;
    float2 wuv = float2(0.0, 0.0);
    if (d.z < -1e-6) {
        const float s = (-w_dist - w_origin.z) / d.z;
        if (s > 0.0) {
            const float3 h = w_origin + s * d;
            const float2 t = float2(h.x * w_au + w_bu, h.y * w_av + w_bv);
            if (t.x >= 0.0 && t.x <= 1.0 && t.y >= 0.0 && t.y <= 1.0) { inwin = true; wuv = t; }
        }
    }
    if (inwin) {   // the live window: exactly the extend path's window pixel
        float3 cw3 = max(world_image.SampleLevel(linear_clamp, wuv, 0).rgb * exposure, 0.0);
        cw3 = tonemap == 1 ? cw3 / (1.0 + cw3) : saturate(cw3);
        res = float4(cw3 * (1.0 * w_fade), 1.0);
        if (fx_mode) res = effects_over(res, wuv, false);
    } else {
        float3 c = max(cache_periphery(d) * exposure, 0.0);
        c = tonemap == 1 ? c / (1.0 + c) : saturate(c);
        res = float4(c * w_fade, 1.0);
    }
    return res;
}
struct CacheOut { float4 color : SV_Target0; float stamp : SV_Target1; };
// Write pass: one texel of the cache per pixel (viewport = the whole cache, scissor = the window's bounding rectangle). The cache extent travels in the
// otherwise unused constants 0 (width) and 1 (height); the window lens in w_dist/w_au/w_bu/w_av/w_bv. Texels the window does not cover, and texels whose
// 4-tap prefiltered world sample is not finite, are left untouched (discard).
CacheOut ps_cache_update(VSOut i) {
    CacheOut o;
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    o.stamp = 0.0;
    const float cwf = float(use_depth);
    const float chf = float(tonemap);
    const float2 cuv = i.position.xy / float2(cwf, chf);
    const float az = (cuv.x - 0.5) * 2.0 * kCachePi;
    const float el = (0.5 - cuv.y) * kCachePi;
    const float3 g = float3(cos(el) * sin(az), sin(el), -cos(el) * cos(az));
    const float3 d = cache_to_window(g);
    if (!(d.z < -1e-6)) discard;
    const float ku = w_dist * w_au;
    const float kv = w_dist * w_av;
    const float2 wuv = float2(w_bu + (d.x / -d.z) * ku, w_bv + (d.y / -d.z) * kv);
    if (!(wuv.x >= 0.0 && wuv.x <= 1.0 && wuv.y >= 0.0 && wuv.y <= 1.0)) discard;
    const float qu = 0.25 * (2.0 * kCachePi / cwf) * ku;     // a quarter of a cache texel, in world-image uv
    const float qv = 0.25 * (kCachePi / chf) * abs(kv);
    float3 c = world_image.SampleLevel(linear_clamp, wuv + float2(-qu, -qv), 0).rgb;
    c += world_image.SampleLevel(linear_clamp, wuv + float2(qu, -qv), 0).rgb;
    c += world_image.SampleLevel(linear_clamp, wuv + float2(-qu, qv), 0).rgb;
    c += world_image.SampleLevel(linear_clamp, wuv + float2(qu, qv), 0).rgb;
    c *= 0.25;
    if (!all(isfinite(c))) discard;
    o.color = float4(clamp(c, 0.0, kCacheMaxValue), 1.0);
    o.stamp = cache_now;
    return o;
}
// Fill pass: a 4x1 viewport, pixels 0..2. 0 = sky = mean of 64 samples of the window's top rows, 1 = ground = mean of 64 samples of the bottom rows,
// 2 = horizon fit: the 16 row means (8 columns each) are split at the row boundary s (3..13) maximizing the L1 distance between the mean above and the mean
// below; output (elevation of that boundary at the window centre column in the cache frame, confidence = smoothstep(kCacheFillConfLo, kCacheFillConfHi, step), 0, 1).
float4 ps_cache_fill(VSOut i) : SV_Target {
    const uint px = uint(i.position.x);
    if (px < 2u) {
        float3 acc = float3(0.0, 0.0, 0.0);
        [unroll] for (int r = 0; r < 4; ++r) {
            const float v = px == 0u ? 0.02 + 0.02 * float(r) : 0.98 - 0.02 * float(r);
            [unroll] for (int c = 0; c < 16; ++c)
                acc += max(world_image.SampleLevel(linear_clamp, float2((float(c) + 0.5) / 16.0, v), 0).rgb, 0.0);
        }
        return float4(acc / 64.0, 1.0);
    }
    if (px > 2u) return float4(0.0, 0.0, 0.0, 1.0);
    float3 pre[17];
    pre[0] = float3(0.0, 0.0, 0.0);
    [unroll] for (int r2 = 0; r2 < 16; ++r2) {
        float3 row = float3(0.0, 0.0, 0.0);
        [unroll] for (int k = 0; k < 8; ++k)
            row += max(world_image.SampleLevel(linear_clamp, float2(0.15 + 0.1 * float(k), (float(r2) + 0.5) / 16.0), 0).rgb, 0.0);
        pre[r2 + 1] = pre[r2] + row * 0.125;
    }
    float best = 0.0;
    float bests = 8.0;
    [unroll] for (int sp = 3; sp <= 13; ++sp) {
        const float3 above = pre[sp] / float(sp);
        const float3 below = (pre[16] - pre[sp]) / float(16 - sp);
        const float3 df = abs(above - below);
        const float stp = df.x + df.y + df.z;
        if (stp > best) { best = stp; bests = float(sp); }
    }
    const float vh = bests / 16.0;
    const float tany = (vh - w_bv) / (w_dist * w_av);
    const float3 g = cache_to_cache(float3(0.0, tany, -1.0));
    const float elh = atan2(g.y, sqrt(g.x * g.x + g.z * g.z));
    const float t = saturate((best - kCacheFillConfLo) / (kCacheFillConfHi - kCacheFillConfLo));
    return float4(elh, t * t * (3.0 - 2.0 * t), 0.0, 1.0);
}
float4 world_layer_window(float2 uv, float2 px) {
    const float3 d = dc_rotate(w_base + uv.x * w_ux + uv.y * w_vy);   // DECOUPLE (flags 0: unchanged)
    if (!(d.z < -1e-6)) return float4(0.0, 0.0, 0.0, 0.0);
    const float s = (-w_dist - w_origin.z) / d.z;
    if (!(s > 0.0)) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 h = w_origin + s * d;
    const float2 wuv = float2(h.x * w_au + w_bu, h.y * w_av + w_bv);
    if (!(wuv.x >= 0.0 && wuv.x <= 1.0 && wuv.y >= 0.0 && wuv.y <= 1.0)) return float4(0.0, 0.0, 0.0, 0.0);
    float3 c = max(world_rgb(uv, wuv, true, px) * exposure, 0.0);   // WORLD-WINDOW FILTER (bit clear: the old single tap)
    c = tonemap == 1 ? c / (1.0 + c) : saturate(c);
    const float4 wl = feathered(float4(c * w_fade, 1.0), wuv);
    if (fx_mode) return effects_over(wl, wuv, true);
    return wl;
}
// ---- J-terrain-render. Constants mirror acz::terrain::k* (terrain_stage.h); the operations mirror trace().
static const uint kTSteps = 320u;
static const uint kTBisect = 8u;
static const float kTStepMin = 128.0, kTStepGrow = 0.008, kTStepMax = 8192.0, kTStepFrac = 0.4;
static const float kTLodScale = 1.0 / 16000.0;
static const float kTAmbient = 0.55, kTDiffuse = 0.60;
static const float3 kTSun = float3(-0.42, 0.80, 0.43);
static const float3 kTSeaDeep = float3(0.07, 0.20, 0.30);
// WORLD-1 colour domain: all terrain shading is in the game's DISPLAY domain (as the PS2 composes it); terrain_trace converts the result ONCE to linear
// (t_srgb_to_linear) because the sRGB RTV encodes again. The sky elevation table in t_sky / the haze colour mirror kSkyElevDeg / kSkyTable / kHaze (display, step56 game shot).
static const float3 kTHaze = float3(130, 148, 141) / 255.0;
static const float kTFarFade = 0.8;
static const float kTSeaEps = 0.5;
static const float3 kTRiverDeep = float3(0.09, 0.20, 0.21);   // D-299 (mirrors kRiverDeep)
// GPU-COST quality (xr_terrain_stage q:0..2 -> t_bits bits 9..10; docs/world-render/gpu-cost.md). q0 = the reference march (every constant above, unchanged).
// q1: the minimum step grows 2x faster beyond kTQNear (near field unchanged), 6 bisections, and a descending ray that marches past the haze saturation
//     distance t_hazel * kTQSatLn (haze >= 1 - 1/512: the colour is the haze colour within 0.002) stops there as fully hazed (no further terrain samples).
// q2: q1 with 3x the far-field growth beyond kTQNear2, kTStepFrac 0.5 and 5 bisections.
static const float kTQNear = 20000.0, kTQNear2 = 10000.0, kTQSatLn = 6.2383;   // ln(512)
float t_h_raw(float x, float z) {
    float u = clamp((x + t_half) / t_cell, 0.0, 512.0), v = clamp((z + t_half) / t_cell, 0.0, 512.0);
    int i = min(int(floor(u)), 511), j = min(int(floor(v)), 511);
    float fu = u - float(i), fv = v - float(j);
    float h00 = float(t_height.Load(int3(i, j, 0))), h10 = float(t_height.Load(int3(i + 1, j, 0)));
    float h01 = float(t_height.Load(int3(i, j + 1, 0))), h11 = float(t_height.Load(int3(i + 1, j + 1, 0)));
    return lerp(lerp(h00, h10, fu), lerp(h01, h11, fu), fv) * t_hstep;
}
float t_h_eff(float x, float z) {
    float h = t_h_raw(x, z);
    return ((t_bits & 4u) != 0u && t_sea > h) ? t_sea : h;
}
// D-344 C1 (mirrors sea_water_at): t_bits bit 13 = the stage's sea is masked by tile-word bit 13 (kTileSeaBit, the .mti SEA tile flag). Clear = the old
// height-only water test (every stage without bit-13 tiles, and seamask:0).
static const uint kTSeaMasked = 8192u, kTTileSeaBit = 8192u;
bool t_sea_water(float3 p, float hraw) {
    float u = clamp((p.x + t_half) / t_cell * 0.5, 0.0, 255.999), v = clamp((p.z + t_half) / t_cell * 0.5, 0.0, 255.999);
    bool tile = (t_bits & kTSeaMasked) == 0u || (t_words.Load(int3(int(floor(u)), int(floor(v)), 0)) & kTTileSeaBit) != 0u;
    return (t_bits & 4u) != 0u && hraw <= t_sea + kTSeaEps && tile;
}
float3 t_sky(float3 d) {   // display domain, piecewise linear in elevation (mirrors sky_color)
    const float e = asin(clamp(d.y, -1.0, 1.0)) * 57.29577951;
    float3 c = float3(130, 148, 141);
    c += saturate(e / 5.6) * (float3(101, 133, 136) - float3(130, 148, 141));
    c += saturate((e - 5.6) / 2.3) * (float3(90, 127, 134) - float3(101, 133, 136));
    c += saturate((e - 7.9) / 2.3) * (float3(82, 120, 130) - float3(90, 127, 134));
    c += saturate((e - 10.2) / 2.2) * (float3(77, 115, 125) - float3(82, 120, 130));
    c += saturate((e - 12.4) / 2.1) * (float3(70, 108, 119) - float3(77, 115, 125));
    c += saturate((e - 14.5) / 2.1) * (float3(64, 103, 115) - float3(70, 108, 119));
    c += saturate((e - 16.6) / 13.4) * (float3(45, 90, 100) - float3(64, 103, 115));
    return c / 255.0;
}
float3 t_srgb_to_linear(float3 x) {   // exact sRGB EOTF (mirrors srgb_to_linear)
    x = saturate(x);
    const float3 lo = x / 12.92;
    const float3 hi = pow((x + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, float3(x <= 0.04045));
}
float t_haze(float t) {   // mirrors haze_fraction: exponential haze, fully hazed between kTFarFade*far and far
    float w = saturate((t - kTFarFade * t_far) / ((1.0 - kTFarFade) * t_far));
    w = w * w * (3.0 - 2.0 * w);
    return 1.0 - exp(-t / t_hazel) * (1.0 - w);
}
// ---- WORLD-2 environment. Mirrors env_sky/env_haze/trace_env in terrain_stage.cpp (constants kTE* = acz::terrain::k*).
static const float kTECloudLo = 0.15, kTECloudHi = 0.85, kTECloudOpacity = 0.5, kTECloudBelowDark = 0.4, kTECloudBlur = 2.0, kTESeaTint = 0.25;
static const float kTEWaveAmp = 0.12, kTEWaveFade = 20000.0, kTEWaterF0 = 0.02, kTEWaterSpecPow = 256.0, kTEWaterSpec = 0.8;
static const float kTELightMin = 0.4, kTELightMax = 1.4, kTELightDenMinY = 0.05;
static const float kTESunCos0 = 0.999954, kTESunCos1 = 0.999981, kTESunGlowPow = 300.0, kTESunGlow = 0.25, kTESunMoonSame = 0.99;
#define TE_SKY 16u
#define TE_LIGHT 32u
#define TE_CLOUD 64u
#define TE_WATER 128u
float4 te_p(uint i) { return t_env.Load(int3(int(i), 0, 0)); }
float te_map(uint ch, float u, float v, bool wrap) {
    float px = u - 0.5, py = v - 0.5;
    float fx = floor(px), fy = floor(py);
    float wx = px - fx, wy = py - fy;
    int x0 = int(fx), y0 = int(fy);
    float acc = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i) {
            int xx = x0 + i, yy = y0 + j;
            if (wrap) { xx = xx & 255; yy = yy & 255; }
            else { xx = clamp(xx, 0, 31); yy = clamp(yy, 0, 31); }
            acc += (i != 0 ? wx : 1.0 - wx) * (j != 0 ? wy : 1.0 - wy) * t_env.Load(int3(xx, 1 + yy, 0))[ch];
        }
    return acc;
}
bool te_look() { return te_p(26u).w > 0.5; }   // WORLD-4 look:1 (P[26..29], written by build_env_texture only when the option is on)
float3 te_look_row(uint r, float el) {   // mirrors the look branch of sky_at (terrain_stage.cpp)
    float4 ls = te_p(28u);
    float3 F = te_p(27u).rgb;
    float3 g = ls.x * te_p(4u * r + 5u).rgb, c2 = te_p(4u * r + 6u).rgb, c3 = te_p(4u * r + 7u).rgb;
    float3 o;
    if (el < ls.z) o = lerp(F, g, smoothstep(ls.y, ls.z, el));
    else if (el < ls.w) o = lerp(g, c2, saturate((el - ls.z) / (ls.w - ls.z)));
    else { float f3 = saturate((el - ls.w) / (te_p(29u).x - ls.w)); f3 = f3 * (2.0 - f3); o = lerp(c2, c3, f3); }
    return o;
}
float3 te_sky_at(float3 d, float el) {
    float4 st = te_p(24u);
    float4 ref = te_p(0u).w > 0.0 ? te_p(0u) : te_p(1u);
    float hl = sqrt(ref.x * ref.x + ref.z * ref.z);
    float c = hl > 1e-4 ? (d.x * ref.x + d.z * ref.z) / hl : 0.0;
    float t = 1.0 - c;
    float3 r0, r1, r2;   // single exit (fxc)
    [branch] if (te_look()) { r0 = te_look_row(0u, el); r1 = te_look_row(1u, el); r2 = te_look_row(2u, el); }
    else {
    uint k = 0u; float f = 0.0;
    if (el <= st.y) { k = 0u; f = saturate((el - st.x) / (st.y - st.x)); }
    else if (el <= st.z) { k = 1u; f = saturate((el - st.y) / (st.z - st.y)); }
    else { k = 2u; f = saturate((el - st.z) / (st.w - st.z)); f = f * (2.0 - f); }
    r0 = lerp(te_p(4u + k).rgb, te_p(5u + k).rgb, f);
    r1 = lerp(te_p(8u + k).rgb, te_p(9u + k).rgb, f);
    r2 = lerp(te_p(12u + k).rgb, te_p(13u + k).rgb, f);
    }
    return lerp(lerp(r0, r1, smoothstep(0.0, 1.0, t)), r2, smoothstep(1.0, 2.0, t));
}
float3 te_sky(float3 d, bool sprite) {
    float els = asin(clamp(d.y, -1.0, 1.0));
    float el = te_look() ? els : max(els, 0.0);   // WORLD-4: the look sky continues below the horizon
    float3 col = te_sky_at(d, el);
    float4 sp = te_p(0u);
    float4 L = te_p(1u);
    if (sprite && L.w > 0.0 && !(sp.w > 0.0 && dot(sp.xyz, L.xyz) > kTESunMoonSame)) {   // sun disc + glow at the light (not when the light IS the moon)
        float cs = dot(d, L.xyz);
        if (cs > 0.0) {
            float3 lc = te_p(2u).rgb;
            float m = max(max(lc.r, lc.g), max(lc.b, 1e-3));
            float disc = smoothstep(kTESunCos0, kTESunCos1, cs), glow = pow(max(cs, 0.0), kTESunGlowPow) * kTESunGlow;
            col = saturate(lerp(col + lc / m * glow, lc / m, disc));
        }
    }
    float q = dot(d, sp.xyz);
    float rl = sqrt(sp.x * sp.x + sp.z * sp.z);
    if (sprite && sp.w > 0.0 && q > 0.0 && rl > 1e-4) {
        float3 R = float3(-sp.z / rl, 0.0, sp.x / rl);
        float3 U = float3(R.y * sp.z - R.z * sp.y, R.z * sp.x - R.x * sp.z, R.x * sp.y - R.y * sp.x);
        float ts = te_p(25u).x;
        float u = dot(d, R) / (q * ts), v = dot(d, U) / (q * ts);
        if (abs(u) < 1.0 && abs(v) < 1.0) {
            float tu = (u * 0.5 + 0.5) * 32.0, tv = (0.5 - v * 0.5) * 32.0;
            float gi = te_map(2u, tu, tv, false), ga = te_map(3u, tu, tv, false) * sp.w;
            col = saturate(col + gi * ga);   // additive (see env_sky)
        }
    }
    return col;
}
float3 te_haze(float3 d) { return te_sky_at(d, te_look() ? asin(clamp(d.y, -1.0, 1.0)) : 0.0); }   // WORLD-4 look: signed elevation (mirrors env_haze)
float3 te_look_gain(float3 col) {   // mirrors look_gain (K00A-FIT: desaturate toward the luminance first)
    if (!te_look()) return col;
    col = lerp(col, dot(col, float3(0.2126, 0.7152, 0.0722)).xxx, te_p(29u).y);
    return col * te_p(26u).rgb;
}
bool te_props_look() { return te_look() && te_p(29u).w > 0.0; }   // K00A-FIT2: mirrors props_look_on
float te_look_haze(float f) {
    if (!te_look()) return f;
    float pw = te_p(29u).z;
    if (pw > 0.0) f = pow(max(f, 0.0), pw);   // K00A-FIT haze exponent (0 = off)
    return lerp(te_p(27u).w, 1.0, f);
}   // SNOW-LOOK: mirrors look_haze (floor 0 = f unchanged)
float3 te_look_tint(float3 hz, float f) {   // K00A-FIT3: mirrors look_haze_tint (P[30] = W, f0; P[31].x = f1)
    if (!te_look()) return hz;
    float4 W = te_p(30u);
    float f1 = te_p(31u).x;
    if (!(W.w > 0.0) || !(f > 0.0)) return hz;
    float w = f < W.w ? f / W.w : (f < f1 ? 1.0 - (f - W.w) / (f1 - W.w) : 0.0);
    return lerp(hz, W.rgb, w);
}
bool te_cloud_on() { return (t_bits & TE_CLOUD) != 0u && te_p(19u).x > 0.5; }
float te_cloud_alpha(float x, float z) {
    float rep = te_p(25u).y;
    float u = x / rep * 256.0, v = z / rep * 256.0;
    float cov = (2.0 * te_map(0u, u, v, true) + te_map(0u, u + kTECloudBlur, v, true) + te_map(0u, u - kTECloudBlur, v, true) +
                 te_map(0u, u, v + kTECloudBlur, true) + te_map(0u, u, v - kTECloudBlur, true)) / 6.0;
    return smoothstep(kTECloudLo, kTECloudHi, cov) * kTECloudOpacity;
}
float4 te_tex(float3 p, float t) {   // the land texel at p (same sampling as the legacy land branch), used for the sea base colour
    float u = clamp((p.x + t_half) / t_cell * 0.5, 0.0, 255.999), v = clamp((p.z + t_half) / t_cell * 0.5, 0.0, 255.999);
    int tx = int(floor(u)), tz = int(floor(v));
    uint id = t_words.Load(int3(tx, tz, 0)) & 1023u;
    float tl = t * kTLodScale;
    float lod = tl > 1.0 ? log2(tl) : 0.0;
    return t_atlas.SampleLevel(linear_clamp, float3(u - float(tx), v - float(tz), float(id)), clamp(lod, 0.0, 6.0));
}
float3 te_water(float3 d, float3 p, float t, float3 w) {
    float3 hl = te_p(17u).rgb, L = te_p(1u).xyz;
    float rep = te_p(25u).z;
    float amp = kTEWaveAmp * saturate(1.0 - t / kTEWaveFade);
    float uw = p.x / rep * 256.0, vw = p.z / rep * 256.0;
    float gxw = te_map(1u, uw + 1.0, vw, true) - te_map(1u, uw - 1.0, vw, true);
    float gzw = te_map(1u, uw, vw + 1.0, true) - te_map(1u, uw, vw - 1.0, true);
    float3 N = float3(-gxw * amp, 1.0, -gzw * amp);
    N = N / sqrt(N.x * N.x + N.y * N.y + N.z * N.z);
    float dn = dot(d, N);
    float3 rf = d - 2.0 * dn * N;
    rf.y = abs(rf.y);
    float fr = kTEWaterF0 + (1.0 - kTEWaterF0) * pow(1.0 - saturate(-dn), 5.0);
    float3 sk = te_sky(rf, false);
    float sp = pow(saturate(dot(rf, L)), kTEWaterSpecPow) * kTEWaterSpec;
    return saturate(lerp(w, sk, fr) + hl * sp);
}
float3 te_clouds(float3 rgb, float3 d, float tlim) {
    float3 res = rgb;
    float H = te_p(18u).x;
    float tc = abs(d.y) > 1e-4 ? (H - t_eye.y) / d.y : -1.0;
    if (te_cloud_on() && abs(d.y) > 1e-4 && tc > 0.0 && tc < tlim) {
        float al = te_cloud_alpha(t_eye.x + d.x * tc, t_eye.z + d.z * tc);
        if (al > 0.0) {
            float3 lit = te_p(21u).rgb, dark = te_p(22u).rgb;
            float3 cc = t_eye.y < H ? lerp(lit, dark, kTECloudBelowDark) : lit;
            float3 hz = (t_bits & TE_SKY) != 0u ? te_haze(d) : kTHaze;
            float f = t_haze(tc);
            cc = lerp(cc, hz, f);
            al *= 1.0 - f;
            res = cc * al + rgb * (1.0 - al);
        }
    }
    return res;
}
// ---- WORLD-CLOUDS (xr_terrain_stage wcloud:1; CPU mirror acz::wcloud::accumulate in world_clouds.h + terrain_stage.cpp world_clouds/world_clouds_apply;
// docs/world-render/world-clouds.md). Our own world-space puff field: one hashed sphere per xz grid cell (inside its cell's column), density = the coverage map
// at its centre; the ray walks the cells it crosses (2D DDA) inside the puff altitude band, front to back, up to tlim. P[33] = (H, thickness, cell, on),
// P[34] = (R, opacity, fill). P[33].w == 0 (every texture without wcloud:1, or no env texture): no code below runs.
static const float kTWcCovLo = 0.2, kTWcCovHi = 0.6, kTWcShapePow = 0.5, kTWcDensFloor = 0.7, kTWcNoise = 0.3, kTWcNoiseRep = 8000.0, kTWcNear0 = 250.0, kTWcNear1 = 1500.0;
static const float kTWcAlphaStop = 0.996, kTWcRadiusMin = 0.6, kTWcMapSize = 256.0;
static const int kTWcMaxCells = 40;
static const float kTWcEdge = 0.45, kTWcEdgeRep1 = 60000.0, kTWcEdgeRep2 = 21000.0, kTWcEdgeW1 = 0.6, kTWcLitGain = 1.3, kTWcHazeScale = 0.5;   // WCLOUD-LOOK (world_clouds.h kEdge..)
bool wc_on() { return (t_bits & 1u) != 0u && (t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u && te_p(33u).w > 0.5; }   // env bound only with env bits
uint wc_hash(int x, int z, uint k) {
    uint h = (asuint(x) * 0x8da6b343u) ^ (asuint(z) * 0xd8163841u) ^ (k * 0xcb1ab31fu);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
float wc_unit(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }
float3 wc_enc(float3 x) {   // = linear_to_srgb (sp_srgb_encode is defined further down)
    x = saturate(x);
    return lerp(1.055 * pow(x, 1.0 / 2.4) - 0.055, 12.92 * x, float3(x <= 0.0031308));
}
float4 wc_accum(float3 o, float3 d, float t0, float t1) {   // premultiplied display colour, coverage
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    const float4 P = te_p(33u), P2 = te_p(34u);
    const float H = P.x, thk = P.y, C = P.z, R = P2.x, op = P2.y, fill = P2.z;
    const float tlo = max(t0, 0.0), thi = min(t1, t_far);
    if (!(thi > tlo)) return acc;
    const float ylo = H - 0.5 * thk - R, yhi = H + 0.5 * thk + R;
    float ts = tlo, te = thi;
    if (abs(d.y) > 1e-6) {
        const float ta = (ylo - o.y) / d.y, tb = (yhi - o.y) / d.y;
        ts = max(ts, min(ta, tb)); te = min(te, max(ta, tb));
    } else if (o.y < ylo || o.y > yhi) return acc;
    if (!(te > ts)) return acc;
    const float3 hz = (t_bits & TE_SKY) != 0u ? te_haze(d) : kTHaze;
    const float3 lit = te_p(21u).rgb, dark = te_p(22u).rgb;
    const float rep = te_p(25u).y;
    const float px = o.x + d.x * ts, pz = o.z + d.z * ts;
    int ci = int(floor(px / C)), ck = int(floor(pz / C));
    const int si = d.x > 0.0 ? 1 : -1, sk = d.z > 0.0 ? 1 : -1;
    const bool mx = abs(d.x) > 1e-9, mz = abs(d.z) > 1e-9;
    const float tdi = mx ? C / abs(d.x) : 1e30, tdk = mz ? C / abs(d.z) : 1e30;
    float tmi = mx ? ts + (float(ci + (d.x > 0.0 ? 1 : 0)) * C - px) / d.x : 1e30;
    float tmk = mz ? ts + (float(ck + (d.z > 0.0 ? 1 : 0)) * C - pz) / d.z : 1e30;
    [loop] for (int n = 0; n < kTWcMaxCells; ++n) {
        const float f1 = wc_unit(wc_hash(ci, ck, 1u)), f2 = wc_unit(wc_hash(ci, ck, 2u)), f3 = wc_unit(wc_hash(ci, ck, 3u)), f4 = wc_unit(wc_hash(ci, ck, 4u));
        const float r = R * (kTWcRadiusMin + (1.0 - kTWcRadiusMin) * f4);
        const float span = C - 2.0 * r;
        const float3 c = float3(float(ci) * C + r + f1 * span, H + (f3 - 0.5) * thk, float(ck) * C + r + f2 * span);
        const float cv = smoothstep(kTWcCovLo, kTWcCovHi, te_map(0u, c.x / rep * kTWcMapSize, c.z / rep * kTWcMapSize, true));
        const float dens = wc_unit(wc_hash(ci, ck, 5u)) < fill ? kTWcDensFloor + (1.0 - kTWcDensFloor) * cv : 0.0;   // WCLOUD-FIX: fill gate, the map modulates
        if (dens > 0.0) {
            const float3 oc = c - o;
            const float tc = oc.x * d.x + oc.y * d.y + oc.z * d.z;
            const float b2 = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - tc * tc;
            const float r2 = r * r;
            if (tc > tlo && tc < thi && b2 < r2) {
                const float3 pp = o + d * tc;
                const float h1 = f1 * kTWcMapSize, h2 = f2 * kTWcMapSize;   // WCLOUD-LOOK: lumpy outline (two octaves of the coverage map, per-cell offset)
                const float e = kTWcEdgeW1 * te_map(0u, pp.x / kTWcEdgeRep1 * kTWcMapSize + h1, (pp.z + pp.y) / kTWcEdgeRep1 * kTWcMapSize + h2, true) +
                                (1.0 - kTWcEdgeW1) * te_map(0u, pp.x / kTWcEdgeRep2 * kTWcMapSize + h2, (pp.z + pp.y) / kTWcEdgeRep2 * kTWcMapSize + h1, true);
                const float body = saturate(((1.0 - b2 / r2) - kTWcEdge * (1.0 - e)) / (1.0 - kTWcEdge));
                float a = dens * op * pow(body, kTWcShapePow);
                const float nn = te_map(0u, pp.x / kTWcNoiseRep * kTWcMapSize, (pp.z + pp.y) / kTWcNoiseRep * kTWcMapSize, true);
                a *= (1.0 - kTWcNoise) + kTWcNoise * nn;
                a *= smoothstep(kTWcNear0, kTWcNear1, tc);
                const float s = saturate(0.5 + 0.5 * (o.y + d.y * (tc - sqrt(r2 - b2)) - c.y) / r);   // WCLOUD-FIX2: the entry point's height
                const float f = t_haze(tc * kTWcHazeScale);   // WCLOUD-LOOK
                float3 col = dark + (min(lit * kTWcLitGain, 1.0) - dark) * s;   // WCLOUD-LOOK: lit tops brighter
                col = col + (hz - col) * f;   // WCLOUD-FIX2: the colour only (the alpha is not hazed a second time)
                const float w = (1.0 - acc.a) * a;
                acc.rgb += w * col;
                acc.a += w;
                if (acc.a > kTWcAlphaStop) break;
            }
        }
        const float tn = tmi < tmk ? tmi : tmk;
        if (!(tn < te)) break;
        if (tmi < tmk) { ci += si; tmi += tdi; } else { ck += sk; tmk += tdk; }
    }
    return acc;
}
float3 wc_over(float4 w, float3 rgb) {   // LINEAR in/out: decode(C + (1 - A) encode(rgb)); unchanged when A == 0
    if (!(w.a > 0.0)) return rgb;
    if (te_p(34u).w > 0.5) return t_srgb_to_linear(lerp(wc_enc(rgb), float3(1.0, 0.0, 1.0), min(4.0 * w.a, 1.0)));   // wcdiag:2: magenta coverage (proof)
    return t_srgb_to_linear(min(w.rgb + (1.0 - w.a) * wc_enc(rgb), 1.0));
}
// ---- DTEX detail texture (xr_terrain_stage dtex:1; constants mirror acz::terrain::kDt*, the operations mirror detail_term; docs/world-render/detail-texture.md).
// t_bits bit 14 = the atlas carries kDtSlices = 5 extra slices after its tiles: slices base+0..3 = the 128x128 level 0 as 64x64 quadrants (q = (y>>6)*2 + (x>>6),
// mip 0), base+4 = levels 1..7 as its mips 0..6. Texel rgb = 128/255 + the signed zero-mean term. Bit clear: no code below runs (every image unchanged).
static const uint kTDtBit = 16384u;
static const float kTDtRepeat = 64.0, kTDtFadeNear = 1500.0, kTDtFadeFar = 3000.0, kTDtLodMul = 64.0;   // kDtLodMul = 32 * 128 / kDtRepeat
float3 t_dt_texel(int base, int L, int x, int y) {
    const int4 c = L == 0 ? int4(x & 63, y & 63, base + (y >> 6) * 2 + (x >> 6), 0) : int4(x, y, base + 4, L - 1);   // single exit (fxc)
    return t_atlas.Load(c).rgb;
}
float3 t_dt_level(int base, int L, float u0, float v0) {   // bilinear, wrapping; u0/v0 in level-0 texels
    const int sz = 128 >> L;
    const float sc = 1.0 / float(1 << L);
    const float px = u0 * sc - 0.5, py = v0 * sc - 0.5;
    const float fx = floor(px), fy = floor(py);
    const float wx = px - fx, wy = py - fy;
    const int x0 = int(fx), y0 = int(fy);
    float3 acc = float3(0.0, 0.0, 0.0);
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i) {
            const float w = (i != 0 ? wx : 1.0 - wx) * (j != 0 ? wy : 1.0 - wy);
            acc += w * (t_dt_texel(base, L, (x0 + i) & (sz - 1), (y0 + j) & (sz - 1)) - 128.0 / 255.0);
        }
    return acc;
}
float3 t_detail(float3 p, float t) {
    if ((t_bits & kTDtBit) == 0u || !(t < kTDtFadeFar)) return float3(0.0, 0.0, 0.0);
    uint aw = 0u, ah = 0u, ael = 0u, alv = 0u;
    t_atlas.GetDimensions(0u, aw, ah, ael, alv);
    const int base = int(ael) - 5;
    const float w = saturate((t - kTDtFadeNear) / (kTDtFadeFar - kTDtFadeNear));
    const float fade = 1.0 - w * w * (3.0 - 2.0 * w);
    const float tl = t * kTLodScale * kTDtLodMul;
    const float lod = min(tl > 1.0 ? log2(tl) : 0.0, 7.0);
    const float u = (p.x - kTDtRepeat * floor(p.x / kTDtRepeat)) * (128.0 / kTDtRepeat);
    const float v = (p.z - kTDtRepeat * floor(p.z / kTDtRepeat)) * (128.0 / kTDtRepeat);
    const int l0 = int(floor(lod)), l1 = min(l0 + 1, 7);
    const float f = lod - float(l0);
    const float3 c0 = t_dt_level(base, l0, u, v);
    const float3 c1 = f > 0.0 ? t_dt_level(base, l1, u, v) : c0;
    return lerp(c0, c1, f) * fade;
}
float3 terrain_trace_t(float3 d, out float t_hit) {   // t_hit: ray distance of the terrain hit (3e38 for a miss)
    t_hit = 3.0e38;
    const float hmax = ((t_bits & 4u) != 0u) ? max(t_hmax, t_sea) : t_hmax;
    const uint tq = (t_bits >> 9) & 3u;   // GPU-COST quality (0 = reference)
    const float qnear = tq >= 2u ? kTQNear2 : kTQNear, qgrow = tq >= 2u ? 2.0 * kTStepGrow : (tq >= 1u ? kTStepGrow : 0.0);
    const float qfrac = tq >= 2u ? 0.5 : kTStepFrac;
    const uint nbis = tq >= 2u ? 5u : (tq >= 1u ? 6u : kTBisect);
    const float tsat = tq >= 1u ? min(t_far, t_hazel * kTQSatLn) : t_far;
    float t = 0.0, prev = 0.0;
    bool hit = false, sat = false;
    [loop] for (uint i = 0u; i < kTSteps; ++i) {
        float3 p = t_eye + d * t;
        float h = t_h_eff(p.x, p.z);
        float dy = p.y - h;
        if (dy < 0.0) { hit = true; break; }
        if (p.y > hmax && d.y >= 0.0) break;
        if (t >= t_far) break;
        if (t >= tsat && d.y < 0.0) { sat = true; break; }   // q1/q2 only (q0: tsat == t_far, already handled above)
        prev = t;
        t += min(max(qfrac * dy, kTStepMin + kTStepGrow * t + qgrow * max(t - qnear, 0.0)), kTStepMax);
    }
    const uint tenv = t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER);   // WORLD-2 (0 = default shading)
    float3 disp = t_sky(d);   // a miss: the sky; below the horizon this IS the haze colour (no seam at the march end)
    [branch] if (sat) {   // GPU-COST q1/q2: fully hazed (haze >= 1 - 1/512 at t): the haze colour, clouds in front as for a hit at t
        disp = (t_bits & TE_SKY) != 0u ? te_haze(d) : kTHaze;
        [branch] if (tenv != 0u) disp = te_clouds(disp, d, t);
        t_hit = t;
    } else if (!hit) {
        [branch] if (tenv != 0u) disp = te_clouds((t_bits & TE_SKY) != 0u ? te_sky(d, true) : disp, d, 1e30);
    } else {
    float lo = prev, hi = t;
    [loop] for (uint k = 0u; k < nbis; ++k) {
        float m = 0.5 * (lo + hi);
        float3 pm = t_eye + d * m;
        if (pm.y < t_h_eff(pm.x, pm.z)) hi = m; else lo = m;
    }
    t = hi;
    float3 p = t_eye + d * t;
    float hraw = t_h_raw(p.x, p.z);
    float3 col;
    if (t_sea_water(p, hraw)) {   // D-344 C1: masked to sea tiles when t_bits bit 13
        if ((t_bits & TE_WATER) != 0u) col = te_water(d, p, t, lerp(te_tex(p, t).rgb, te_p(16u).rgb, kTESeaTint));   // the sea is painted in the tiles (m16n)
        else {
        float3 sk = t_sky(float3(d.x, abs(d.y), d.z));
        float fr = 0.05 + 0.75 * pow(1.0 - saturate(-d.y), 3.0);
        col = lerp(kTSeaDeep, sk, fr);
        }
    } else {
        float u = clamp((p.x + t_half) / t_cell * 0.5, 0.0, 255.999), v = clamp((p.z + t_half) / t_cell * 0.5, 0.0, 255.999);
        int tx = int(floor(u)), tz = int(floor(v));
        uint id = t_words.Load(int3(tx, tz, 0)) & 1023u;
        float tl = t * kTLodScale;
        float lod = tl > 1.0 ? log2(tl) : 0.0;
        const float4 tex = t_atlas.SampleLevel(linear_clamp, float3(u - float(tx), v - float(tz), float(id)), clamp(lod, 0.0, 6.0));
        col = tex.rgb;
        float gx = (t_h_raw(p.x + t_cell, p.z) - t_h_raw(p.x - t_cell, p.z)) / (2.0 * t_cell);
        float gz = (t_h_raw(p.x, p.z + t_cell) - t_h_raw(p.x, p.z - t_cell)) / (2.0 * t_cell);
        float nl = sqrt(gx * gx + 1.0 + gz * gz);
        if ((t_bits & TE_LIGHT) != 0u) {   // WORLD-2: stage light + ambient normalised to flat ground; cloud shadows on the direct term
            float3 L = te_p(1u).xyz, lc = te_p(2u).rgb, am = te_p(3u).rgb;
            float sh = 1.0;
            if (te_cloud_on() && L.y > kTELightDenMinY) {
                float sd = (te_p(18u).x - p.y) / L.y;
                if (sd > 0.0) sh = 1.0 - te_cloud_alpha(p.x + L.x * sd, p.z + L.z * sd) * te_p(25u).w;
            }
            float ndl = max((-gx * L.x + L.y - gz * L.z) / nl, 0.0) * sh;
            float3 den = max(am + lc * max(L.y, kTELightDenMinY), 1e-3);
            col = saturate(col * clamp((am + lc * ndl) / den, kTELightMin, kTELightMax));
        } else {
        float lam = max((-gx * kTSun.x + kTSun.y - gz * kTSun.z) / (nl * length(kTSun)), 0.0);
        col = saturate(col * (kTAmbient + kTDiffuse * lam));
        }
        [branch] if (tenv != 0u) col = te_look_gain(col);   // WORLD-4 look:1 ground gain (land only)
        [branch] if ((t_bits & kTDtBit) != 0u) col = saturate(col + t_detail(p, t));   // DTEX: zero-mean grain on the lit land (bit clear: unchanged)
        if (tex.a < 1.0) {   // D-299 river water: coverage alpha < 1 blends toward the water colour
            if ((t_bits & TE_WATER) != 0u) col = lerp(te_water(d, p, t, te_p(16u).rgb), col, tex.a);
            else {
            float3 sk = t_sky(float3(d.x, abs(d.y), d.z));
            float fr = 0.05 + 0.75 * pow(1.0 - saturate(-d.y), 3.0);
            col = lerp(lerp(kTRiverDeep, sk, fr), col, tex.a);
            }
        }
    }
    float3 hz = (t_bits & TE_SKY) != 0u ? te_haze(d) : kTHaze;   // WORLD-2: haze = the stage's horizon colour along the view azimuth
    [branch] if (tenv != 0u) hz = te_look_tint(hz, t_haze(t));   // K00A-FIT3
    disp = lerp(col, hz, tenv != 0u ? te_look_haze(t_haze(t)) : t_haze(t));   // SNOW-LOOK haze floor (look:1 profile only)
    [branch] if (tenv != 0u) disp = te_clouds(disp, d, t);
    t_hit = t;
    }
    return t_srgb_to_linear(disp);
}
float3 terrain_trace(float3 d) { float th; return terrain_trace_t(d, th); }
// WORLD-1 step 4 (b1 t_bits bit 3): is the terrain in front of the replayed world object at this eye pixel? The object's depth is the producer's depth-fix
// value dz = 2^-10 (1/w - 255 2^-24) (gpu_copy_step5_world.h depth_fix_scale; ACM depth = 1/w - 255 2^-24), w = its view depth along the eye's forward axis;
// the eye forward in the anchor frame is the normal of the planar ray map (cross(w_ux, w_vy), oriented like the rays), so the object's ray distance is
// w / cos(ray, forward). Hidden when the terrain hit is nearer by more than 3% + 50 units (the baked heightfield is not the game's mesh).
static const float kTOccludeRel = 0.97, kTOccludeAbs = 50.0;
float object_ray_dist(float2 uv, float dz) {   // the replayed object's ray distance at this eye pixel (stage units)
    const float3 d = normalize(w_base + uv.x * w_ux + uv.y * w_vy);
    float3 fw = normalize(cross(w_ux, w_vy));
    if (dot(fw, d) < 0.0) fw = -fw;
    const float w = 1.0 / (dz * 1024.0 + 255.0 / 16777216.0);
    return w / max(dot(fw, d), 1e-3);
}
bool terrain_hides_object(float2 uv, float dz, float t_terrain) {
    const float r_obj = object_ray_dist(uv, dz);
    return t_terrain < r_obj * kTOccludeRel - kTOccludeAbs;
}
#include "xr_props_composite.hlsli"   // WORLD-3 props/trees merge (t11/t12, b1 t_bits bit 8; inert when the bit is clear)
bool window_uv(float3 d, out float2 wuv) {
    wuv = float2(0.0, 0.0);
    if (!(d.z < -1e-6)) return false;
    const float s = (-w_dist - w_origin.z) / d.z;
    if (!(s > 0.0)) return false;
    const float3 h = w_origin + s * d;
    wuv = float2(h.x * w_au + w_bu, h.y * w_av + w_bv);
    return wuv.x >= 0.0 && wuv.x <= 1.0 && wuv.y >= 0.0 && wuv.y <= 1.0;
}
// WORLD-CLOUDS: t_from = the ray distance from which our puffs are added to the world layer (0 = all of them; a replayed object pixel passes its own
// distance: the puffs in FRONT of the object are composited over the object by the caller).
float4 world_layer_terrain_t2(float2 uv, float t_from, out float t_hit, float2 px) {
    const float3 d = w_base + uv.x * w_ux + uv.y * w_vy;
    const float3 dw = normalize(t_right * d.x + t_up * d.y + t_fwd * (-d.z));
    float3 tlin = terrain_trace_t(dw, t_hit);
    props_over(uv, dw, tlin, t_hit);   // WORLD-3: a prop nearer than the terrain hit replaces the sample (and its distance)
    [branch] if (wc_on()) tlin = wc_over(wc_accum(t_eye, dw, t_from, t_hit), tlin);   // WORLD-CLOUDS: puffs nearer than the terrain/prop hit
    float3 c = max(tlin * exposure, 0.0);
    c = tonemap == 1 ? c / (1.0 + c) : saturate(c);
    float4 base = float4(c * w_fade, 1.0);
    float4 res = base;
    float2 wuv = float2(0.0, 0.0);
    if (window_uv(dc_rotate(d), wuv)) {   // DECOUPLE: the game's window/effects in the cockpit frame of their own camera (flags 0: d)
        if ((t_bits & 2u) != 0u) {   // xr_terrain_blend: the game's window over the terrain
            const float4 wl = world_layer_window(uv, px);
            res = wl + (1.0 - wl.a) * base;
        } else if (fx_mode) {
            res = effects_over(base, wuv, true);   // terrain only: the effects image (missiles, tracers) stays in the window rays
        }
    }
    return res;
}
float4 world_layer_terrain_t(float2 uv, out float t_hit, float2 px) { return world_layer_terrain_t2(uv, 0.0, t_hit, px); }
float4 world_layer_terrain(float2 uv, float2 px) { float th; return world_layer_terrain_t(uv, th, px); }
float4 world_layer(float2 uv, float2 px) {
    float4 w = float4(0.0, 0.0, 0.0, 0.0);
    if ((t_bits & 1u) != 0u) w = world_layer_terrain(uv, px);   // J-terrain-render (b1 zero when the key is off)
    else if (w_cache != 0.0) w = world_layer_cache(uv, px);        // J-world-cache (only when the backend bound the cache; replaces extend)
    else if (w_extend != 0.0) w = world_layer_ext(uv, px);
    else w = world_layer_window(uv, px);
    return w;
}
float occlude_keep(float2 uv) {
    const float3 d = w_base + uv.x * w_ux + uv.y * w_vy;   // same window test as world_layer_window
    if (!(d.z < -1e-6)) return 1.0;
    const float s = (-w_dist - w_origin.z) / d.z;
    if (!(s > 0.0)) return 1.0;
    const float3 h = w_origin + s * d;
    const float2 wuv = float2(h.x * w_au + w_bu, h.y * w_av + w_bv);
    if (!(wuv.x >= 0.0 && wuv.x <= 1.0 && wuv.y >= 0.0 && wuv.y <= 1.0)) return 1.0;
    const float2 t = saturate((2.0 * min(wuv, 1.0 - wuv)) / kOccludeSeam);   // = edge_ramp(wuv, kOccludeSeam)
    const float2 r = t * t * (3.0 - 2.0 * t);
    return 1.0 - r.x * r.y;
}
// EFFECTS-OUTSIDE (C) fxmem:4 (mode_bits bit 18 = xr_stereo::kModeBitSmoke, eye targets of the world composite with use_depth only; CPU mirror
// acz::fxsmoke::gpu::smoke_layer / smoke_over in fx_smoke_gpu.h). t13 = THIS eye's smoke image (coverage_v1: P premultiplied colour, A coverage), drawn by the
// producer from the eye's own camera and depth-tested against the eye's objects depth (smoke behind a replayed object is already absent). Sampled at the eye
// pixel's source uv (the objects' decouple re-projection, times its edge fade) and composited OVER the objects / terrain / world; the replayed cockpit band
// keeps its pixel unless the model cockpit replaces it (the model is drawn over the result). Bit clear: t13 is never read and every path is unchanged.
Texture2D<float4> smoke_image : register(t13);
#define smoke_bit ((mode_bits & 262144u) != 0u)
float4 smoke_over(float4 under, float2 suv, float fade, float dz) {
    if (!smoke_bit) return under;
    if (!model_bit && dz >= kOccludeObjectDepth) return under;
    const float4 e = smoke_image.SampleLevel(linear_clamp, suv, 0);
    const float a = saturate(max(e.a, 0.0));
    float3 ec = a > 0.0 ? max((e.rgb / a) * exposure, 0.0) : float3(0.0, 0.0, 0.0);
    ec = tonemap == 1 ? ec / (1.0 + ec) : saturate(ec);
    const float4 s = float4(ec * a, a) * fade;
    return s + (1.0 - s.a) * under;
}
// EFFECTS-OUTSIDE (F) fxmem:6 (mode_bits bit 19 = xr_stereo::kModeBitSprite, composite eye targets with use_depth only; on these targets bits 19..23 are
// never the HUD's radar bits, which are read in mode 1 only; CPU mirror acz::fxsprite::gpu::sprite_layer / sprite_over in fx_sprite_gpu.h). t14 = THIS eye's
// sprite image (explosions, GREATER sprites): premultiplied LIGHT P and coverage A, drawn by the producer from re-projected vertex copies and depth-tested
// against the eye's objects depth. Composited AFTER the smoke: s = tonemap(P x exposure) WITHOUT dividing by A (the explosion clone adds light with A
// unchanged: a fireball over empty space has A = 0 and must stay visible), out = s + (1 - A) x under, with the smoke's cockpit-band rule and decouple fade.
// Bit clear: t14 is never read and every path is exactly the previous one.
Texture2D<float4> sprite_image : register(t14);
#define sprite_bit ((mode_bits & 524288u) != 0u)
// EFFECTS-OUTSIDE (I) (docs section 14; CPU mirror fx_sprite_gpu.h srgb_encode / srgb_decode / sprite_over): the sprite image holds the game's DISPLAY values
// and the explosion's light PSO stores the game's (1+a) background brightening as A < 0 (T = 1 - A > 1, floored at kSpriteMinCoverage). The layer is
// composited in DISPLAY space as the game blends: out = decode(min(1, s + T x encode(under))); an empty texel returns `under` untouched.
static const float kSpriteMinCoverage = -4096.0;   // = fx_sprite_gpu.h kMinCoverage (W5-2 P1)
float3 sp_srgb_encode(float3 x) {
    x = saturate(x);
    return lerp(1.055 * pow(x, 1.0 / 2.4) - 0.055, 12.92 * x, float3(x <= 0.0031308));
}
float3 sp_srgb_decode(float3 x) { return t_srgb_to_linear(x); }   // exact sRGB EOTF (saturates)
float4 sprite_over(float4 under, float2 suv, float fade, float dz) {
    if (!sprite_bit) return under;
    if (!model_bit && dz >= kOccludeObjectDepth) return under;
    const float4 e = sprite_image.SampleLevel(linear_clamp, suv, 0);
    // (W5-2) non-finite guard: NaN -> 0, +Inf -> the largest finite fp16 (Reinhard Inf/(1+Inf) would be NaN); mirrors sprite_layer
    float3 ec = max(e.rgb * exposure, 0.0);
    ec = float3(ec.x == ec.x ? min(ec.x, 65504.0) : 0.0, ec.y == ec.y ? min(ec.y, 65504.0) : 0.0, ec.z == ec.z ? min(ec.z, 65504.0) : 0.0);
    ec = tonemap == 1 ? ec / (1.0 + ec) : saturate(ec);
    const float ea = e.a == e.a ? e.a : 0.0;
    const float4 s = float4(ec, clamp(ea, kSpriteMinCoverage, 1.0)) * fade;
    if (all(s == 0.0)) return under;
    const float k = 1.0 - s.a;
    return float4(sp_srgb_decode(min(s.rgb + k * sp_srgb_encode(under.rgb), 1.0)), saturate(s.a + k * under.a));
}
// The eye's own effects over the composited pixel: the smoke (t13, fxmem:4) then the sprites (t14, fxmem:6). Both bits clear: `under` unchanged.
float4 eye_fx_over(float4 under, float2 suv, float fade, float dz) { return sprite_over(smoke_over(under, suv, fade, dz), suv, fade, dz); }
// LENS-FLARE (xr_terrain_stage lflare:0|1 default 1, lflarek:<0..3>; CPU mirror acz::lensflare in lens_flare.h; docs/world-render/lens-flare.md): our own
// HEAD-LOOK flare over the composited eye pixel. Active only for the composite eyes with the terrain + sky (t_bits bit0 + TE_SKY) and the env texel
// P[32] = (k, 0, 0, on) written by build_env_texture (zero = off: no code below runs, every image unchanged). Sun = P[1] (stage light, world); skipped when the
// light is the moon. Geometry on the eye's tangent plane along its optical axis (the ray map normal); added in DISPLAY space like the sprites.
static const float kLFCoreCos0 = 0.57, kLFGhostCos0 = 0.71, kLFViewMargin = 0.1, kLFHorizonLo = -0.02, kLFHorizonHi = 0.06;
static const float kLFCoreA = 0.55, kLFCoreR = 0.035, kLFBloomA = 0.16, kLFBloomR = 0.12, kLFRayA = 0.30, kLFRayR = 0.22, kLFRayPow = 48.0, kLFRay2A = 0.12, kLFRay2R = 0.10;
static const float kLFRingA = 0.05, kLFRingR = 0.40, kLFRingW = 0.03, kLFRingChroma = 0.025, kLFGhostA = 0.10, kLFCoreTint = 0.5;
static const float kLFVisSoft0 = 20.0, kLFVisSoft1 = 0.02, kLFDepthTap = 0.006, kLFMinOut = 1.0 / 1024.0;
static const float4 kLFGhostT = float4(0.55, 1.35, 1.75, 2.30), kLFGhostRad = float4(0.035, 0.060, 0.110, 0.050);
static const float3 kLFGhostCol[4] = {float3(0.25, 0.45, 0.35), float3(0.20, 0.30, 0.55), float3(0.35, 0.25, 0.15), float3(0.30, 0.45, 0.25)};
float lf_terrain_vis(float3 L) {   // mirrors terrain_vis (12 samples, quadratic spacing, soft clearance)
    if (!(L.y > 1e-3)) return 0.0;
    float tmax = (t_hmax - t_eye.y) / L.y;
    if (!(tmax > 0.0)) return 1.0;
    tmax = min(tmax, t_far);
    float vis = 1.0;
    [loop] for (int k = 0; k < 12; ++k) {
        const float fk = float(k + 1) / 12.0;
        const float t = tmax * fk * fk;
        const float3 q = t_eye + t * L;
        vis = min(vis, saturate(0.5 + (q.y - t_h_eff(q.x, q.z)) / (kLFVisSoft0 + kLFVisSoft1 * t)));
    }
    return vis;
}
bool lf_covered(float2 u) {   // replay depth > 0 (cockpit/object) at the eye uv u, through the decouple re-projection
    float fd = 1.0;
    const float2 s = dc_source_uv(u, fd);
    if (!(fd > 0.0)) return false;
    uint w, h;
    source_depth.GetDimensions(w, h);
    const int2 p = min(int2(s * float2(w, h)), int2(int(w) - 1, int(h) - 1));
    return source_depth.Load(int3(p, 0)) > 0.0;
}
float4 lf_over(float4 under, float2 uv) {
    if (!fx_mode && mode != 2) return under;
    if ((t_bits & 1u) == 0u || (t_bits & TE_SKY) == 0u) return under;
    const float4 P = te_p(32u);
    if (!(P.w > 0.0) || !(P.x > 0.0)) return under;
    const float4 L4 = te_p(1u), sp = te_p(0u);
    if (!(L4.w > 0.0) || (sp.w > 0.0 && dot(sp.xyz, L4.xyz) > kTESunMoonSame)) return under;   // no light, or the light is the moon
    float3 f = normalize(cross(w_ux, w_vy));
    if (dot(f, w_base + 0.5 * w_ux + 0.5 * w_vy) < 0.0) f = -f;
    const float3 er = normalize(w_ux - dot(w_ux, f) * f);
    const float3 eu = cross(f, er);
    const float3 lw = normalize(L4.xyz);
    const float3 sa = float3(dot(lw, t_right), dot(lw, t_up), -dot(lw, t_fwd));
    const float cs = dot(sa, f);
    const float hz = smooth01(kLFHorizonLo, kLFHorizonHi, lw.y);
    if (!(cs > kLFCoreCos0) || !(hz > 0.0)) return under;
    // the sun on the image plane (Cramer: u*ux + v*vy - l*sa = -base, l > 0) -> the in-view gate
    const float3 a3 = -sa, rb = -w_base;
    const float D = dot(w_ux, cross(w_vy, a3));
    if (!(abs(D) > 1e-12)) return under;
    const float x1 = dot(rb, cross(w_vy, a3)) / D, x2 = dot(w_ux, cross(rb, a3)) / D, x3 = dot(w_ux, cross(w_vy, rb)) / D;
    if (!(x3 > 0.0)) return under;
    const float2 su = float2(x1, x2);
    const float iv = smooth01(kLFViewMargin, 0.0, max(max(max(-su.x, su.x - 1.0), max(-su.y, su.y - 1.0)), 0.0));
    if (!(iv > 0.0)) return under;
    const float2 s2 = float2(dot(sa, er), dot(sa, eu)) / cs;
    const float3 d = w_base + uv.x * w_ux + uv.y * w_vy;
    const float pd = dot(d, f);
    if (!(pd > 1e-6)) return under;
    const float2 p2 = float2(dot(d, er), dot(d, eu)) / pd;
    const float3 lc = te_p(2u).rgb;
    const float3 tint = lc / max(max(lc.r, lc.g), max(lc.b, 1e-3));
    const float2 dd = p2 - s2;
    const float r = sqrt(dot(dd, dd));
    const float gate = hz * iv;
    const float ac = saturate((cs - kLFCoreCos0) / (1.0 - kLFCoreCos0)) * gate;
    float ag = saturate((cs - kLFGhostCos0) / (1.0 - kLFGhostCos0)); ag = ag * ag * gate;
    float core = kLFCoreA * exp(-(r / kLFCoreR) * (r / kLFCoreR)) + kLFBloomA * exp(-r / kLFBloomR);
    if (r > 1e-5) {
        const float phi = atan2(dd.y, dd.x);
        const float c3 = abs(cos(3.0 * phi)), s3 = abs(sin(3.0 * phi));
        core += kLFRayA * pow(c3, kLFRayPow) * exp(-r / kLFRayR) + kLFRay2A * pow(s3, kLFRayPow) * exp(-r / kLFRay2R);
    }
    core *= ac;
    const float3 rr = kLFRingR * (1.0 + kLFRingChroma * float3(1.0, 0.0, -1.0));
    const float3 q = (r - rr) / kLFRingW;
    const float3 ring = kLFRingA * ac * exp(-q * q);
    float3 gh = float3(0.0, 0.0, 0.0);
    if (ag > 0.0) {
        [unroll] for (int i = 0; i < 4; ++i) {
            const float2 g = s2 * (1.0 - kLFGhostT[i]);
            const float rg = length(p2 - g);
            const float rad = kLFGhostRad[i];
            const float disc = smooth01(rad, 0.75 * rad, rg) * (0.6 + 0.4 * saturate(rg / rad));
            gh += kLFGhostA * ag * disc * kLFGhostCol[i];
        }
    }
    float3 F = core * (1.0 + (tint - 1.0) * kLFCoreTint) + ring * tint + gh * tint;
    if (!(max(F.r, max(F.g, F.b)) * P.x >= kLFMinOut)) return under;
    // occlusion (only where the flare is visible): terrain along the sun ray, replay depth (cockpit/objects) at the sun
    float vis = lf_terrain_vis(lw);
    if (use_depth != 0 && su.x >= 0.0 && su.x <= 1.0 && su.y >= 0.0 && su.y <= 1.0) {
        float n = 0.0;
        n += lf_covered(saturate(su)) ? 1.0 : 0.0;
        n += lf_covered(saturate(su + float2(kLFDepthTap, 0.0))) ? 1.0 : 0.0;
        n += lf_covered(saturate(su - float2(kLFDepthTap, 0.0))) ? 1.0 : 0.0;
        n += lf_covered(saturate(su + float2(0.0, kLFDepthTap))) ? 1.0 : 0.0;
        n += lf_covered(saturate(su - float2(0.0, kLFDepthTap))) ? 1.0 : 0.0;
        vis *= 1.0 - n / 5.0;
    }
    F *= P.x * vis;
    if (!(max(F.r, max(F.g, F.b)) > 0.0)) return under;
    return float4(sp_srgb_decode(min(sp_srgb_encode(under.rgb) + F, 1.0)), under.a);
}
float4 ps_main_body(VSOut i) {
    const float2 px = float2(abs(ddx(i.uv.x)), abs(ddy(i.uv.y)));   // WORLD-WINDOW FILTER: eye uv per pixel (read only with mode_bits bit 21)
    float dcf = 1.0;   // DECOUPLE: edge fade of the re-projected source (1 = flags 0: suv is exactly i.uv)
    const float2 suv = dc_source_uv(i.uv, dcf);
    const float4 s = source_image.SampleLevel(linear_clamp, suv, 0);
    // HUD-LATE display marker (hud_late.h, review F6): a stored HUD alpha < 0 = the pixel holds post-tone-map display values (the radio subtitles):
    // no exposure, no tone map (clamp only), alpha = saturate(-a). Mode 1 only; other modes never see a negative alpha.
    const bool late_display = mode == 1 && s.a < 0.0;
    float3 c = late_display ? saturate(s.rgb) : max(s.rgb * exposure, 0.0);
    c = late_display ? c : (tonemap == 1 ? c / (1.0 + c) : saturate(c));
    // HUD-MENU (hud_menu.h hud_alpha): mode_bits 24..31 = alpha floor (pause-menu backdrop) on the HUD target only; 0 = exactly the old alpha.
    // RADAR-BG (hud_menu.h hud_alpha / radar_bg_bits): mode_bits 19..23 = q, background alpha rb = q/32; the stored alpha is capped to rb + (1 - rb) * max(rgb)
    // (the game's translucent radar square instead of the mask quads' alpha 1). q = 0: exactly the old alpha.
    if (mode == 1) {
        const float hm = max(c.r, max(c.g, c.b));
        float sa = late_display ? saturate(-s.a) : max(s.a, 0.0);
        const uint rq = (mode_bits >> 19) & 31u;
        if (rq != 0u) { const float rb = float(rq) / 32.0; sa = min(sa, rb + (1.0 - rb) * hm); }
        return feathered(float4(c, saturate(max(max(sa, hm), float((mode_bits >> 24) & 255u) / 255.0))), i.uv);
    }
    float a = 1.0;
    float dzv = 0.0;   // WORLD-1: the replay depth of this pixel (object occlusion by the terrain)
    if (use_depth != 0) {
        uint w, h;
        source_depth.GetDimensions(w, h);
        const int2 p = min(int2(suv * float2(w, h)), int2(int(w) - 1, int(h) - 1));
        const float dz = source_depth.Load(int3(p, 0));
        a = dz > 0.0 ? 1.0 : 0.0;   // nearest texel coverage
        dzv = dz;
        if (occlude_bit && (mode == 2 || fx_mode) && dz > 0.0 && dz < kOccludeObjectDepth) a = occlude_keep(i.uv);
        if (model_bit && !(dz > 0.0 && dz < kOccludeObjectDepth)) a = 0.0;
    } else if (model_bit) a = 0.0;
    if ((dc_flags & 1u) != 0u) a *= dcf;   // DECOUPLE: the objects image fades out at its edges (nothing stretched beyond them)
    // PROPS-SHADING objdom:display (b1 t_bits bit 11, set only with the terrain + use_depth): a replayed OBJECT pixel holds the game's DISPLAY value; decode it once
    // so the sRGB RTV does not encode it a second time (D-311). Bit clear (default, and every non-terrain target: b1 is zero): no code runs.
    if ((t_bits & 2048u) != 0u && (mode == 2 || fx_mode) && dzv > 0.0 && dzv < kOccludeObjectDepth) c = t_srgb_to_linear(c);
    // J-test8-fixes xr_debug_view (0 = off: the lines below the block are exactly the previous ones). Output is opaque (a = 1), no feather:
    //   1 = cockpit coverage alpha as grey; 2 = cockpit colour with alpha forced to 1; 3 = the world layer only (modes 2/3, else black);
    //   4 = false colour: red = cockpit alpha 1 (depth > 0), green = world layer alpha, blue = cockpit colour present but NOT covered (depth 0).
    if (dbg_view != 0u) {
        const float4 wl = (mode == 2 || fx_mode) ? world_layer(i.uv, px) : float4(0.0, 0.0, 0.0, 0.0);
        if (dbg_view == 1u) return float4(a, a, a, 1.0);
        if (dbg_view == 2u) return float4(c, 1.0);
        if (dbg_view == 3u) return float4(wl.rgb, 1.0);
        return float4(a, wl.a, (a == 0.0 && max(c.r, max(c.g, c.b)) > 0.0) ? 1.0 : 0.0, 1.0);
    }
    const float4 o = feathered(float4(c * a, a), i.uv);
    if ((mode == 2 || fx_mode) && (t_bits & 8u) != 0u && dzv > 0.0 && dzv < kOccludeObjectDepth) {   // WORLD-1: a replayed OBJECT pixel over the terrain
        float th;
        const bool wcf = wc_on();   // WORLD-CLOUDS: the puffs behind the object go into the world layer, the ones in front over the object
        const float r_obj = wcf ? object_ray_dist(i.uv, dzv) : 0.0;
        const float4 wl = world_layer_terrain_t2(i.uv, r_obj, th, px);
        // the terrain in front of the object: show the terrain (its puffs from r_obj on are none: th < r_obj; the nearer ones are added below)
        float4 ob = terrain_hides_object(i.uv, dzv, th) ? wl : o + (1.0 - o.a) * wl;
        [branch] if (wcf) {   // puffs nearer than the object / that terrain, composited over it (display domain, after exposure: exact for exposure 1 / tonemap 0)
            const float3 d0 = w_base + i.uv.x * w_ux + i.uv.y * w_vy;
            const float3 dw0 = normalize(t_right * d0.x + t_up * d0.y + t_fwd * (-d0.z));
            const float4 wf = wc_accum(t_eye, dw0, 0.0, min(r_obj, th));
            if (wf.a > 0.0) ob = float4(wc_over(wf, ob.rgb), wf.a + (1.0 - wf.a) * ob.a);
        }
        return lf_over(eye_fx_over(ob, suv, dcf, dzv), i.uv);
    }
    // LENS-FLARE: lf_over is the identity unless the terrain + sky run and P[32] switches the flare on (lflare:1, the parsed default)
    if ((mode == 2 || fx_mode) && o.a >= 1.0 && (t_bits & 1u) != 0u) return lf_over(eye_fx_over(o, suv, dcf, dzv), i.uv);   // terrain: no ray march under an opaque cockpit pixel
    if (mode == 2 || fx_mode) return lf_over(eye_fx_over(o + (1.0 - o.a) * world_layer(i.uv, px), suv, dcf, dzv), i.uv);   // fxmem:4/6 bits clear: exactly the previous value
    return o;
}
// POV-RENDER pov:3 (pov_render.h vignette_mode_bits / vignette_gain, other-povs.md section 11): the comfort vignette and the 150 ms blink of the chase /
// cinematic rigs, on the EYE targets only (mode != 1; the HUD target's bits 19..31 are its radar background / alpha floor). mode_bits bit 20 = blink
// (rgb x 0), bits 24..31 = strength q: rr = |2 uv - 1|, inner = 1.45 - q/255, rgb x (1 - smoothstep(inner, inner + 0.35, rr)); alpha kept (premultiplied).
// Bits clear (every job without pov:3, every cockpit frame): the output of ps_main_body unchanged.
float4 ps_main(VSOut i) : SV_Target {
    const float4 o = ps_main_body(i);
    if (mode == 1u) return o;
    const uint vq = (mode_bits >> 24) & 255u;
    const bool vblink = (mode_bits & 1048576u) != 0u;
    if (!vblink && vq == 0u) return o;
    if (vblink) return float4(0.0, 0.0, 0.0, o.a);
    const float rr = length(2.0 * i.uv - 1.0);
    const float inner = 1.45 - float(vq) / 255.0;
    return float4(o.rgb * (1.0 - smooth01(inner, inner + 0.35, rr)), o.a);
}
