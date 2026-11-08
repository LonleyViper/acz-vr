// Card (d) synthetic stereo shaders. Compiled at build time by the Windows SDK
// fxc into embedded bytecode; no runtime compiler dependency.
// Root constants b0: one row-major, row-vector world(LOCAL)->clip matrix per
// eye, exactly the 16 floats produced by stereo::eye_matrix (synthetic_scene.h).
cbuffer Eye : register(b0) { row_major float4x4 world_to_clip; };
struct VSIn { float3 position : POSITION; float3 colour : COLOR; };
struct VSOut { float4 position : SV_Position; nointerpolation float3 colour : COLOR; };
VSOut vs_main(VSIn input) {
    VSOut output;
    output.position = mul(float4(input.position, 1.0), world_to_clip); // row vector * M
    output.colour = input.colour;
    return output;
}
// Linear, opaque output; the sRGB RTV performs the single encoding.
float4 ps_main(VSOut input) : SV_Target { return float4(input.colour, 1.0); }
