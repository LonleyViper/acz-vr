// BRIEF3D table blit (xr_brief3d_d3d12.cpp; docs/world-render/brief3d.md section 6): one producer eye image (R16G16B16A16_FLOAT, the game's own
// map draws replayed per eye over transparent black with the game's SRC_ALPHA / INV_SRC_ALPHA blend: rgb = a x colour, alpha = a) -> one eye
// swapchain image through an sRGB RTV. The game's colours are display values: un-premultiply, decode them once (sRGB EOTF) so the sRGB RTV writes
// the same display value back, premultiply again for the compositor (SOURCE_ALPHA, premultiplied). Full-screen triangle, bilinear.
// UI3D (ui-3d.md 4 item 4): opaque = 1 for a UI3D menu slot: the game's menu PSOs blend alpha as ZERO/ONE (census bl=...,1,2,1 [E]) so the eye
// alpha stays the clear value 0: rgb is taken as is (display values over the cleared black), decoded once, alpha 1 (the layer is opaque).
// UI3D stage 3a (ui-3d.md 7, UNTESTED LIVE): mode 2 = the in-flight pause menu BLENDED over the flight eye image (PSO blend ONE / INV_SRC_ALPHA):
// rgb as for opaque (display values over the cleared black), alpha = max(floor, max(rgb)) with floor = bits 8..15 / 255 (the hud-menu menub):
// bright text covers the cockpit, the dark panel dims it by the floor (ui3d_pause.h coverage/blend_one mirror this on the CPU).
// b0 bits 0..7 = mode (0 table, 1 UI3D opaque, 2 pause); the table and stage-2 values (0 / 1) are unchanged.
cbuffer Blit : register(b0) { uint opaque; };
Texture2D<float4> eye_image : register(t0);
SamplerState linear_clamp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_table(uint id : SV_VertexID) {
    VSOut o;
    const float2 uv = float2(float((id << 1) & 2), float(id & 2));
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float3 srgb_eotf(float3 c) {
    const float3 lo = c / 12.92;
    const float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
}
// 2026-10-30b (ui-3d.md 11, ui3d_resample.h mirrors this): the 1440 x 1440 eye image is UPSCALED into the 2400 x 1350 eye swapchain. b0 bits 16..17 =
// filter (0 bilinear = the old blit, 1 Catmull-Rom, 2 Catmull-Rom + unsharp mask), bits 24..31 = sharpen amount x 255. The CPU sets them for UI3D
// opaque / pause slots only; the premultiplied table keeps filter 0.
void catmull_rom(float t, out float w[4]) {
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
    w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
}
float4 sample_eye(float2 uv) {
    const uint filt = (opaque >> 16) & 3u;
    const float4 bil = eye_image.SampleLevel(linear_clamp, uv, 0);
    if (filt == 0u || filt == 3u) return bil;
    uint sw, sh;
    eye_image.GetDimensions(sw, sh);
    const int2 size = int2(int(sw), int(sh));
    const float2 p = uv * float2(size) - 0.5;
    const float2 fl = floor(p);
    float wx[4], wy[4];
    catmull_rom(p.x - fl.x, wx);
    catmull_rom(p.y - fl.y, wy);
    const int2 b = int2(fl);
    float4 cr = float4(0.0, 0.0, 0.0, 0.0);
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int k = 0; k < 4; ++k) {
            const int2 q = clamp(b + int2(k - 1, j - 1), int2(0, 0), size - 1);
            cr += wx[k] * wy[j] * eye_image.Load(int3(q, 0));
        }
    }
    if (filt == 1u) return cr;
    const float amount = float((opaque >> 24) & 0xffu) / 255.0;
    return cr + amount * (cr - bil);
}
float4 ps_table(VSOut i) : SV_Target {
    const float4 c = sample_eye(i.uv);
    const uint mode = opaque & 0xffu;
    if (mode == 2u) {
        const float3 m = saturate(c.rgb);
        const float floor_a = float((opaque >> 8) & 0xffu) / 255.0;
        return float4(srgb_eotf(m), max(floor_a, max(m.r, max(m.g, m.b))));
    }
    if (mode != 0u) return float4(srgb_eotf(saturate(c.rgb)), 1.0);
    const float a = saturate(c.a);
    if (a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 u = saturate(c.rgb / a);
    return float4(srgb_eotf(u) * a, a);
}
