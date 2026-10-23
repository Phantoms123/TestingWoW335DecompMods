#include "post_common.hlsli"

// Per pixel of the water: red, how much of the sky reflection it takes (none within Plane.y yards of the reflected
// level Plane.x, all of it Plane.y + 1 / Plane.z yards away); green, the water's height above the reflected level
// (0.5 level, Plane.w yards either way at 0 and 1). Water behind the scene (DepthTex) is left out.
sampler2D DepthTex : register(s0);
float4 View : register(c0);
float4 Plane : register(c1);

float4 main(PS_POSITION float4 clip : TEXCOORD0, float height : TEXCOORD1) : COLOR {
    float2 ndc = clip.xy / clip.w;
    float2 uv = float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
    float scene = ViewDepth(tex2D(DepthTex, uv).x, View);
    clip(scene * 1.002 + 0.1 - clip.w);
    float above = height - Plane.x;
    float amount = saturate((abs(above) - Plane.y) * Plane.z);
    return float4(amount, saturate(0.5 + 0.5 * above / Plane.w), 0.5, 1.0);
}
