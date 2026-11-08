// fx_puff_atlas_warp_game.hlsl -- test-only stand-in for the game's 4caf6e2d pipeline in fx_puff_atlas_warp_harness.cpp: the same resource interface as
// ps2AtestCA_p (t0 in a descriptor table, b1 root constants with the `textured` flag in x, a STATIC s0) and the same colour maths that matter for the
// atlas (texel x COLOR0, discard below 1/255). Vertices are already in clip space (the game's ps2_v projection is not under test).
cbuffer B1 : register(b1) { uint textured; float aref; float pad0; float pad1; };
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

struct VIn { float2 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct VOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };

VOut vs_game(VIn i) {
    VOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

float4 ps_game(VOut i) : SV_Target {
    float4 t = textured != 0 ? t0.Sample(s0, i.uv) * i.col : i.col * 0.502;
    if (t.a < 1.0 / 255.0) discard;
    return saturate(t);
}
