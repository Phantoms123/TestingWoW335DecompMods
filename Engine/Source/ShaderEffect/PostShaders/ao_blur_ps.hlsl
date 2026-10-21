// Depth-aware blur of the ambient occlusion, one direction a pass.
#include "post_common.hlsli"

sampler2D AoTex : register(s0);
sampler2D DepthTex : register(s1);
float4 View : register(c0);
float4 Step : register(c1);     // one texel along the blur direction, sky depth

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float centre = ViewDepth(min(tex2D(DepthTex, uv).x, Step.z), View);
    float total = 0.0;
    float weights = 0.0;

    [unroll]
    for (int i = -4; i <= 4; i++) {
        float2 at = uv + Step.xy * i;
        float depth = ViewDepth(min(tex2D(DepthTex, at).x, Step.z), View);
        float w = exp(-0.18 * i * i) * exp(-abs(depth - centre) / (centre * 0.03 + 0.1));
        total += tex2D(AoTex, at).x * w;
        weights += w;
    }

    float ao = total / weights;
    return float4(ao, ao, ao, 1.0);
}
