// Bloom composite with filmic tone: the frame plus its blurred highlights, exposure, a soft roll-off of the
// highlights, saturation and contrast.
#include "post_common.hlsli"

sampler2D Scene : register(s0);
sampler2D Bloom : register(s1);
sampler2D BloomWide : register(s2);
float4 Params : register(c0);   // bloom intensity, highlight roll-off (0-1), exposure, saturation
float4 Params2 : register(c1);  // contrast

float3 RollOff(float3 c) {
    // Above 0.6 the curve bends so that brightness never clips: 0.6 + x / (1 + x / 0.4).
    float3 x = max(c - 0.6, 0.0);
    return min(c, 0.6) + x / (1.0 + x / 0.4);
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(Scene, uv).rgb;
    c += (tex2D(Bloom, uv).rgb * 0.6 + tex2D(BloomWide, uv).rgb * 0.4) * Params.x;
    c *= Params.z;
    c = lerp(saturate(c), RollOff(c), Params.y);
    float luma = Luma(c);
    c = lerp(luma.xxx, c, Params.w);
    c = (c - 0.5) * Params2.x + 0.5;
    return float4(saturate(c), 1.0);
}
