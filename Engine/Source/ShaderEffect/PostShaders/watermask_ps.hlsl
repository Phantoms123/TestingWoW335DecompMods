#include "post_common.hlsli"

// How much of the sky reflection the water at this pixel takes: none within Plane.y yards of the reflected level
// (Plane.x), all of it Plane.y + 1 / Plane.z yards away. Water behind the scene (DepthTex) is left out.
sampler2D DepthTex : register(s0);
float4 View : register(c0);
float4 Plane : register(c1);

float4 main(PS_POSITION float4 clip : TEXCOORD0, float height : TEXCOORD1) : COLOR {
    float2 ndc = clip.xy / clip.w;
    float2 uv = float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
    float scene = ViewDepth(tex2D(DepthTex, uv).x, View);
    clip(scene * 1.002 + 0.1 - clip.w);
    float amount = saturate((abs(height - Plane.x) - Plane.y) * Plane.z);
    return float4(amount, amount, amount, 1.0);
}
