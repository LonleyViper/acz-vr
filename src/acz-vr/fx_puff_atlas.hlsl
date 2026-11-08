// fx_puff_atlas.hlsl -- EFFECTS-OUTSIDE (M6, fxatlas:1; docs/world-render/effects-outside-window.md section 23; fx_puff_atlas.h): the CAPTURE pass that
// copies a stored puff card's uv rectangle of the game's texture into a tile of our atlas. The copy PSO is created from the game's 4caf6e2d PSO desc
// with THESE shaders, so it uses the GAME's root signature: t0 is the game's descriptor table (the key the host draw binds), s0 the game's static
// sampler (the same filtering / addressing the game's ps2AtestCA_p uses, also in the gutter beyond the rectangle). No resource of ours is read here.
// vs_5_0 / ps_5_0 (fxc at build time -> g_fx_atlas_vs / g_fx_atlas_ps).
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

struct VIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VOut vs_copy(VIn i) {
    VOut o;
    o.pos = float4(i.pos, 0.0, 1.0);   // clip space over the whole atlas viewport (fx_puff_atlas.h copy_quad)
    o.uv = i.uv;                       // the source uv (rectangle + gutter), linear across the tile
    return o;
}

// The texel as the game's sampler returns it (straight alpha, no colour / fog / alpha test: those stay in the game's PS when the card is drawn from the
// atlas). Implicit derivatives: a rectangle spanning more source texels than the tile selects the matching mip, as the hardware would.
float4 ps_copy(VOut i) : SV_Target {
    return t0.Sample(s0, i.uv);
}
