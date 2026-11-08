// WORLD-3: merge of the props/trees pass (xr_props.hlsl) into the terrain composite. Included by xr_stereo_blit.hlsl AFTER terrain_trace_t (uses t_eye,
// t_bits, t_far, t_haze, t_srgb_to_linear, kTHaze/kTSun/kTAmbient/kTDiffuse and WORLD-2's te_* environment). CPU reference: acz::props::prop_color /
// prop_visible (world_props.cpp).
// t11 = props albedo (RGBA8: texture x vertex colour, DISPLAY domain like the terrain tiles), t12 = RGBA32F (unit normal facing the eye, ray distance; 0 = none),
// both eye-viewport sized; bound only with b1 t_bits bit 8 (TP_PROPS), else null SRVs and props_over leaves the terrain sample untouched.
// The prop is shaded exactly like a terrain texel (WORLD-1 display domain, WORLD-2 stage light + cloud shadow when TE_LIGHT, the default sun otherwise), hazed
// with t_haze toward the same haze colour (te_haze along the azimuth with TE_SKY, else kTHaze), clouds in front with TE_CLOUD, then converted to linear once.
Texture2D<float4> p_albedo : register(t11);
Texture2D<float4> p_nd : register(t12);
#define TP_PROPS 256u
// PROPS-SHADING (b1 t_bits bit 12, propshade:game, the default; docs/world-render/props-shading.md): the game's own DrawACM shading of the stage props = texture x
// vertex colour (the props' baked lighting; the live building records have the VS lighting branch OFF) with NO extra light and NO look gain (except the k00a profile, K00A-FIT2), then LINEAR fog
// (kPPropFogEnd, the record's fog slope) toward the same haze colour as the terrain, clouds in front, one sRGB decode. Bit clear = WORLD-3/4 exactly (below).
#define TP_PROPS_GAME 4096u
static const float kPPropFogEnd = 46080.0;   // mirrors acz::props::kPropFogEnd
void props_over(float2 uv, float3 d, inout float3 lin, inout float t_hit) {   // single exit (fxc)
    [branch] if ((t_bits & TP_PROPS) != 0u) {
        uint w = 1u, h = 1u;
        p_nd.GetDimensions(w, h);
        const int2 px = min(int2(uv * float2(w, h)), int2(int(w) - 1, int(h) - 1));
        const float4 nd = p_nd.Load(int3(px, 0));
        const float tp = nd.w;
        [branch] if (tp > 0.0 && tp < t_far && tp < t_hit && (t_bits & TP_PROPS_GAME) != 0u) {   // PROPS-SHADING game shading
            float3 col = saturate(p_albedo.Load(int3(px, 0)).rgb);
            [branch] if ((t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u && te_props_look()) col = saturate(te_look_gain(col) * te_p(29u).w);   // K00A-FIT2: k00a profile only (P[29].w)
            float3 hz = (t_bits & TE_SKY) != 0u ? te_haze(d) : kTHaze;
            const float pf = saturate(tp / kPPropFogEnd);
            [branch] if ((t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u) hz = te_look_tint(hz, pf);   // K00A-FIT3
            float3 disp = lerp(col, hz, (t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u ? te_look_haze(pf) : pf);   // SNOW-LOOK haze floor
            [branch] if ((t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u) disp = te_clouds(disp, d, tp);
            lin = t_srgb_to_linear(disp);
            t_hit = tp;
        } else [branch] if (tp > 0.0 && tp < t_far && tp < t_hit) {   // the terrain hit (3e38 on a miss) is farther: the prop is in front
            float3 col = p_albedo.Load(int3(px, 0)).rgb;
            const float nl = length(nd.xyz);
            const float3 n = nl > 1e-6 ? nd.xyz / nl : float3(0.0, 1.0, 0.0);
            const float3 p = t_eye + d * tp;
            if ((t_bits & TE_LIGHT) != 0u) {   // WORLD-2 stage light, as terrain_trace_t with the prop normal
                float3 L = te_p(1u).xyz, lc = te_p(2u).rgb, am = te_p(3u).rgb;
                float sh = 1.0;
                if (te_cloud_on() && L.y > kTELightDenMinY) {
                    float sd = (te_p(18u).x - p.y) / L.y;
                    if (sd > 0.0) sh = 1.0 - te_cloud_alpha(p.x + L.x * sd, p.z + L.z * sd) * te_p(25u).w;
                }
                float ndl = max(dot(n, L), 0.0) * sh;
                float3 den = max(am + lc * max(L.y, kTELightDenMinY), 1e-3);
                col = saturate(col * clamp((am + lc * ndl) / den, kTELightMin, kTELightMax));
            } else {
                float lam = max(dot(n, kTSun) / length(kTSun), 0.0);
                col = saturate(col * (kTAmbient + kTDiffuse * lam));
            }
            [branch] if ((t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u) col = te_look_gain(col);   // WORLD-4 look:1: same gain as the ground [I]
            float3 hz = (t_bits & TE_SKY) != 0u ? te_haze(d) : kTHaze;
            [branch] if ((t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u) hz = te_look_tint(hz, t_haze(tp));   // K00A-FIT3
            float3 disp = lerp(col, hz, (t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u ? te_look_haze(t_haze(tp)) : t_haze(tp));   // SNOW-LOOK haze floor
            [branch] if ((t_bits & (TE_SKY | TE_LIGHT | TE_CLOUD | TE_WATER)) != 0u) disp = te_clouds(disp, d, tp);
            lin = t_srgb_to_linear(disp);
            t_hit = tp;
        }
    }
}
