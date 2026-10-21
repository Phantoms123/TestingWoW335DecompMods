// Edge anti-aliasing in the manner of FXAA's console path: the edge direction from four diagonal luma samples,
// a blur along it, kept only if it stays within the local luma range.
#include "post_common.hlsli"

sampler2D Scene : register(s0);
float4 Texel : register(c0);    // texel size

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    float3 rgbM = tex2D(Scene, uv).rgb;
    float lumaNW = Luma(tex2D(Scene, uv + Texel.xy * float2(-0.5, -0.5)).rgb);
    float lumaNE = Luma(tex2D(Scene, uv + Texel.xy * float2(0.5, -0.5)).rgb);
    float lumaSW = Luma(tex2D(Scene, uv + Texel.xy * float2(-0.5, 0.5)).rgb);
    float lumaSE = Luma(tex2D(Scene, uv + Texel.xy * float2(0.5, 0.5)).rgb);
    float lumaM = Luma(rgbM);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    // Flat areas stay as they are.
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125)) {
        return float4(rgbM, 1.0);
    }

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = (lumaNW + lumaSW) - (lumaNE + lumaSE);
    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, -8.0, 8.0) * Texel.xy;

    float3 rgbA = 0.5 * (tex2D(Scene, uv + dir * (1.0 / 3.0 - 0.5)).rgb + tex2D(Scene, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (tex2D(Scene, uv - dir * 0.5).rgb + tex2D(Scene, uv + dir * 0.5).rgb);
    float lumaB = Luma(rgbB);
    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(result, 1.0);
}
