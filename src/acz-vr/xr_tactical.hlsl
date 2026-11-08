// TACTICAL overlay (xr_terrain_stage option tac:1; tactical.h): clip-space triangles built on the CPU (radar blips in the HUD image, world-anchored
// markers and cosmetic tracers in the eyes), read from a root SRV (no input layout), premultiplied colour blended ONE / INV_SRC_ALPHA.
struct TacVertex { float4 pos; float4 col; };
StructuredBuffer<TacVertex> g_tac : register(t0);
struct TacOut { float4 p : SV_Position; float4 c : COLOR0; };
TacOut vs_tac(uint id : SV_VertexID) {
    const TacVertex v = g_tac[id];
    TacOut o;
    o.p = v.pos;
    o.c = v.col;
    return o;
}
float4 ps_tac(TacOut i) : SV_Target { return i.c; }

// NBOX (mk360:3; marker360.h build_stamps, docs/world-render/markers-360.md section 12): the game's own box + label pixels, cut from the replayed marker
// image into the stamp atlas (t1), drawn per eye as screen-aligned quads (the same TacVertex stream: pos = clip, col.xy = atlas uv). The texel goes
// through exactly the marker quad's HUD conversion (xr_stereo_blit.hlsl ps_main mode 1 without the HUD-only alpha floors): c = tonemap(rgb x exposure),
// a = saturate(max(a, max(c))); output premultiplied (c, a) blended ONE / INV_SRC_ALPHA like the marker layer.
Texture2D<float4> g_stamp_atlas : register(t1);
SamplerState g_stamp_sampler : register(s0);
cbuffer StampConstants : register(b0) { uint g_stamp_tonemap; float g_stamp_exposure; };
// NEAR-FADE (markers-360.md section 16): col.z = the stamp's opacity (1 = the game's look; < 1 a contact nearer than mk360min fading out at the canopy).
struct StampOut { float4 p : SV_Position; float2 uv : TEXCOORD0; float fade : TEXCOORD1; };
StampOut vs_stamp(uint id : SV_VertexID) {
    const TacVertex v = g_tac[id];
    StampOut o;
    o.p = v.pos;
    o.uv = v.col.xy;
    o.fade = saturate(v.col.z);
    return o;
}
float4 ps_stamp(StampOut i) : SV_Target {
    const float4 s = g_stamp_atlas.SampleLevel(g_stamp_sampler, i.uv, 0);
    float3 c = max(s.rgb * g_stamp_exposure, 0.0);
    c = g_stamp_tonemap == 1 ? c / (1.0 + c) : saturate(c);
    const float hm = max(c.r, max(c.g, c.b));
    return float4(c, saturate(max(max(s.a, 0.0), hm))) * i.fade;   // premultiplied: scaling all four channels fades it toward transparent
}
