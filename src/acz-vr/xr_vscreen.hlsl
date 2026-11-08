// VSCREEN capture pass (vscreen_capture_win.cpp): the private copy of the game's back buffer -> the slot's R8G8B8A8_UNORM screen image.
// Raw display values (no colour conversion): the XR swapchain is R8G8B8A8_UNORM_SRGB and receives these bytes by a plain copy, so the
// compositor decodes them exactly as the monitor shows them. Full-screen triangle, bilinear resample to the screen image size, opaque.
Texture2D<float4> source_image : register(t0);
SamplerState linear_clamp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_screen(uint id : SV_VertexID) {
    VSOut o;
    const float2 uv = float2(float((id << 1) & 2), float(id & 2));
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float4 ps_screen(VSOut i) : SV_Target {
    return float4(saturate(source_image.SampleLevel(linear_clamp, i.uv, 0).rgb), 1.0);
}
