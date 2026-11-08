// WORLD-3 props/trees pass (xr_props_d3d12.cpp; CPU references in world_props.cpp). Compiled at build time by fxc into embedded headers.
// Per eye, BEFORE the eye composite: instanced, depth-tested (reversed Z, GREATER, D32) rasterisation of the selected props and trees into two private targets
//   SV_Target0 RGBA8_UNORM        : albedo = texture * vertex colour (display domain like the terrain tiles), a = 1
//   SV_Target1 R32G32B32A32_FLOAT : unit normal facing the eye (xyz), distance from the eye along the pixel ray (w; 0 = no prop)
// The composite (xr_props_composite.hlsli) keeps the prop where it is nearer than the terrain hit and lights/hazes it exactly like a terrain texel.
// The projection reproduces the composite's own pixel ray (acz::props::make_clip): no separate FOV/handedness convention exists here.
cbuffer PropsPass : register(b0) {
    float4 p_r0; float4 p_r1; float4 p_r2; float4 p_r3;   // clip = (dot(r0,p), dot(r1,p), dot(r2,p), dot(r3,p)), p = (world - eye, 1)
    float3 p_eye; float p_pad;
};
Texture2DArray<float4> p_tex : register(t0);
SamplerState p_wrap : register(s0);
static const float kPAlphaCut = 0.5;
struct PIn {
    float3 pos : POSITION; float2 uv : TEXCOORD0; float2 n01 : NORMAL0; float n2 : NORMAL1; uint layer : BLENDINDICES0; float4 col : COLOR0;
    float4 inst : INSTANCE0;   // per instance: world position xyz, scale w
};
struct POut {
    float4 position : SV_Position;
    float3 rel : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 n : TEXCOORD2;
    float4 col : COLOR0;
    nointerpolation uint layer : TEXCOORD3;
};
POut vs_props(PIn i) {
    POut o;
    const float3 w = i.inst.xyz + i.inst.w * i.pos;
    o.rel = w - p_eye;
    const float4 p = float4(o.rel, 1.0);
    o.position = float4(dot(p_r0, p), dot(p_r1, p), dot(p_r2, p), dot(p_r3, p));
    o.uv = i.uv;
    o.n = float3(i.n01, i.n2);
    o.col = i.col;
    o.layer = i.layer;
    return o;
}
struct PTargets { float4 albedo : SV_Target0; float4 nd : SV_Target1; };
PTargets ps_props(POut i) {
    const float4 t = p_tex.Sample(p_wrap, float3(i.uv, float(i.layer)));
    if (t.a < kPAlphaCut) discard;
    float3 n = i.n;
    const float nl = length(n);
    n = nl > 1e-12 ? n / nl : float3(0.0, 1.0, 0.0);
    if (dot(n, i.rel) > 0.0) n = -n;   // face the eye (two-sided cards and walls)
    PTargets o;
    o.albedo = float4(saturate(t.rgb * i.col.rgb), 1.0);   // DISPLAY domain like the terrain tiles; lit/hazed in the composite
    o.nd = float4(n, length(i.rel));
    return o;
}
