// Bloom: the bright part of the frame, shrunk to a quarter (sixteen texels averaged in four bilinear taps).
#include "post_common.hlsli"

sampler2D Scene : register(s0);
float4 Params : register(c0);   // source texel size, threshold

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(Scene, uv + Params.xy * float2(-1.0, -1.0)).rgb;
    c += tex2D(Scene, uv + Params.xy * float2(1.0, -1.0)).rgb;
    c += tex2D(Scene, uv + Params.xy * float2(-1.0, 1.0)).rgb;
    c += tex2D(Scene, uv + Params.xy * float2(1.0, 1.0)).rgb;
    c *= 0.25;
    float brightness = max(c.r, max(c.g, c.b));
    float knee = Params.z * 0.5;
    float soft = clamp(brightness - Params.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 0.0001);
    float contribution = max(soft, brightness - Params.z) / max(brightness, 0.0001);
    return float4(c * contribution, 1.0);
}
