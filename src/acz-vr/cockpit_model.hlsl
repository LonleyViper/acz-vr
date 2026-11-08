// COCKPIT-2 model cockpit pass (key xr_cockpit_model, default OFF; docs/world-render/cockpit-render-impl.md). Compiled at build time by fxc into
// embedded bytecode (vs_model / ps_model). CPU mirror: acz::cockpit::render_reference / shade_fragment (cockpit_model.cpp), compared on WARP by
// xr_composite_warp_harness case_cockpit_model.
//   t0 = the expanded vertex stream (raw buffer, 32 bytes per corner: pos f32x3 | uv f32x2 | nrm i16x3 | mat u16 | rgba u8x4)
//   t1..t4 = the model's RGBA8 textures (UNORM: the raw game colour/255 domain, like the replayed cockpit), s0 = linear WRAP
//   t5 = SKIN-REFL stage reflection map (RGBA8 UNORM, cflags bit 1)
//   b0 = acz::cockpit::Constants (60 DWORDs since CFID): model->clip rows, light, ambient, gain, exposure, tonemap, alpha cut, base vertex, pass, cflags,
//        tint, then the canopy glass block (eye, refl, linear transmission, dirt, sky colour, F0), then the SKIN-REFL world basis + strength, CFID shadow box
//   CFID (docs/world-render/cockpit-fidelity.md): cflags bit 2 = engine 2 (n.L)^3 sun response on the skin, bit 3 = cockpit-rim sun shadow
//   COCKPIT-3: cflags bit 0 = display domain (default): rgb = srgb_to_linear(saturate(tex * vcol * light * gain)) * exposure, like the terrain/props
// Draw: non-indexed, SV_VertexID + base_vertex; pass 0 = opaque (alpha test, depth write), pass 1 = blend (no depth write). Reversed-Z (GREATER),
// back faces culled (CCW front), output premultiplied (rgb*a, a) blended ONE / INV_SRC_ALPHA over the eye composite in the sRGB RTV.
ByteAddressBuffer verts : register(t0);
Texture2D<float4> tex0 : register(t1);
Texture2D<float4> tex1 : register(t2);
Texture2D<float4> tex2 : register(t3);
Texture2D<float4> tex3 : register(t4);
Texture2D<float4> refl_map : register(t5);   // SKIN-REFL: the drawn stage's <stage>_ref map (null view when absent; cflags bit 1 is then clear)
SamplerState wrap_linear : register(s0);
cbuffer Model : register(b0) {
    float4 m0; float4 m1; float4 m2; float4 m3;
    float3 light; float ambient;
    float gain; float exposure; uint tonemap; float cut;
    uint base_vertex; uint pass_id; uint cflags; uint pad1;
    float3 tint; float shs;    // COCKPIT-3: light colour (stage ambient + sun, mean 1); CFID shadow penumbra (model units)
    float3 eye; float refl;    // COCKPIT-4 canopy glass: model-space eye, Fresnel scale
    float3 trans; float dirt;  //   linear transmission per channel, veil
    float3 sky; float f0;      //   reflection colour (display), Fresnel F0
    float3 rx; float refl_k;   // SKIN-REFL (cflags bit 1): the game's world axes in model space, strength
    float3 ry; float pad4;
    float3 rz; float pad5;
    float4 shb;                // CFID (cflags bit 3): cockpit-rim shadow box (sill_y, half_x, z_front, z_back), model units
};
struct VOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
    float3 n : NORMAL0;
    float3 pm : TEXCOORD1;     // COCKPIT-4: model-space position (glass view vector)
    float3 r : TEXCOORD2;      // SKIN-REFL: per-vertex (u, v, mask) of the reflection map (acz::cockpit::refl_vertex)
    nointerpolation uint mat : MATERIAL0;
};
VOut vs_model(uint id : SV_VertexID) {
    const uint a = (base_vertex + id) * 32u;
    const float4 p = float4(asfloat(verts.Load3(a)), 1.0);
    const uint n01 = verts.Load(a + 20u), n2m = verts.Load(a + 24u), c = verts.Load(a + 28u);
    VOut o;
    o.pos = float4(dot(m0, p), dot(m1, p), dot(m2, p), dot(m3, p));
    o.uv = asfloat(verts.Load2(a + 12u));
    o.col = float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) / 255.0;
    o.n = float3(float(int(n01 << 16) >> 16), float(int(n01) >> 16), float(int(n2m << 16) >> 16)) / 32767.0;
    o.mat = n2m >> 16;
    o.pm = p.xyz;
    // SKIN-REFL (DrawCPT_v label 143): world normal without normalisation; uv = saturate(0.5 * n.xz + 0.5), mask = saturate(10 * n.y)
    const float3 nw = float3(dot(o.n, rx), dot(o.n, ry), dot(o.n, rz));
    o.r = float3(saturate(nw.x * 0.5 + 0.5), saturate(nw.z * 0.5 + 0.5), saturate(10.0 * nw.y));
    return o;
}
float4 sample_slot(uint slot, float2 uv) {
    float4 r = float4(1.0, 1.0, 1.0, 1.0);   // kNoTexture
    [branch] if (slot == 0u) r = tex0.SampleLevel(wrap_linear, uv, 0);
    else if (slot == 1u) r = tex1.SampleLevel(wrap_linear, uv, 0);
    else if (slot == 2u) r = tex2.SampleLevel(wrap_linear, uv, 0);
    else if (slot == 3u) r = tex3.SampleLevel(wrap_linear, uv, 0);
    return r;
}
float3 srgb_to_linear3(float3 x) {   // exact sRGB EOTF (mirrors acz::cockpit::srgb_to_linear / xr_stereo_blit t_srgb_to_linear)
    x = saturate(x);
    const float3 lo = x / 12.92;
    const float3 hi = pow((x + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, float3(x <= 0.04045));
}
// CFID cockpit-rim shadow (mirrors acz::cockpit::shadow_visibility): 1 outside the box footprint / above the sill, else smoothstep of the light ray's
// crossing of the sill plane inside the opening.
float shadow_vis(float3 p) {
    if (p.y >= shb.x || abs(p.x) > shb.y || p.z < shb.z || p.z > shb.w) return 1.0;
    if (!(light.y > 1e-3)) return 0.0;
    const float t = (shb.x - p.y) / light.y;
    const float cx = p.x + t * light.x, cz = p.z + t * light.z;
    const float d = min(shb.y - abs(cx), min(cz - shb.z, shb.w - cz));
    if (!(shs > 0.0)) return d >= 0.0 ? 1.0 : 0.0;
    const float x = saturate((d + shs) / (2.0 * shs));
    return x * x * (3.0 - 2.0 * x);
}
float4 ps_model(VOut i) : SV_Target {
    const uint slot = i.mat & 7u;
    const float4 t = sample_slot(slot, i.uv);
    if (pass_id == 0u && (i.mat & 8u) != 0u && slot < 4u && t.a < cut) discard;   // alpha test (cockpit atlas coverage)
    const float nl = length(i.n);
    const float dl = nl > 1e-6 ? dot(i.n, light) / nl : 0.0;
    const bool skin = (i.mat & 64u) != 0u;
    // CFID: exterior skin takes the engine's lit-branch sun response (DrawCPT_v, +356 == 0): max(0, 2 d^3); the cockpit interior the rim shadow
    float lam = ((cflags & 4u) != 0u && skin) ? max(0.0, 2.0 * dl * dl * dl) : saturate(dl);
    [branch] if ((cflags & 8u) != 0u && !skin) lam *= shadow_vis(i.pm);
    const float li = min(ambient + (1.0 - ambient) * lam, 1.99) * gain;
    const float a = pass_id == 1u ? saturate(t.a * i.col.a) : 1.0;
    float3 c = t.rgb * i.col.rgb * li * tint;
    // SKIN-REFL: the engine's second (flags 7) pass, dst.rgb += dst.a * env.rgb * mask, where dst.a = the skin texel alpha of its first pass (blend off).
    // Folded into this fragment: same geometry, GREATER_EQUAL depth = the same surviving fragments; the add stays in the display domain like the game's.
    [branch] if ((cflags & 2u) != 0u && pass_id == 0u && (i.mat & 64u) != 0u)
        c += refl_k * t.a * i.r.z * refl_map.SampleLevel(wrap_linear, i.r.xy, 0).rgb;
    if ((cflags & 1u) != 0u) c = srgb_to_linear3(c);   // COCKPIT-3: DISPLAY-domain shading (the game's colour), converted once; the RTV encodes it back
    c = max(c * exposure, 0.0);
    c = tonemap == 1u ? c / (1.0 + c) : saturate(c);
    return float4(c * a, a);
}
// COCKPIT-4 canopy glass (pass 2; CPU mirror acz::cockpit::shade_glass). Dual-source blend: rgb = o0.rgb + dst.rgb * o1.rgb (SrcBlend ONE, DestBlend SRC1_COLOR),
// a = o0.a + dst.a * (1 - o0.a). o1 = per-channel transmission (tint^2.2 in linear, reduced by the reflected/veiled share), o0 = the Fresnel sky reflection + veil.
struct GlassOut { float4 add : SV_Target0; float4 trans : SV_Target1; };
GlassOut ps_glass(VOut i) {
    const float3 v = eye - i.pm;
    const float vl = length(v), nl = length(i.n);
    float c = 1.0;
    if (vl > 1e-6 && nl > 1e-6) c = abs(dot(v, i.n) / (vl * nl));
    c = saturate(c);
    const float x = 1.0 - c, x2 = x * x, x5 = x2 * x2 * x;
    const float F = f0 + (1.0 - f0) * x5;
    const float veil = saturate(refl * F + dirt);
    float3 s = max(srgb_to_linear3(sky) * exposure, 0.0);
    s = tonemap == 1u ? s / (1.0 + s) : saturate(s);
    const float3 t = trans * (1.0 - veil);
    GlassOut o;
    o.add = float4(s * veil, saturate(1.0 - (t.x + t.y + t.z) / 3.0));
    o.trans = float4(t, 0.0);
    return o;
}
