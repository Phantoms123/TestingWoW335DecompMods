#include "post_common.hlsli"

// Per pixel of the water: red, how much of the sky reflection it takes (none within Plane.y yards of the reflected
// level Plane.x, all of it Plane.y + 1 / Plane.z yards away); green, the water's height above the reflected level
// (0.5 level, Plane.w yards either way at 0 and 1; blue its fraction of a step). Water behind the scene (DepthTex) is left out.
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
    // CHANGED: blue the rest of green (green in 255ths, blue the fraction left), so the height is exact enough to
    // trace the reflection on screen from (lerp_ps).
    float q = saturate(0.5 + 0.5 * above / Plane.w) * 255.0;
    float whole = floor(q);
    return float4(amount, whole / 255.0, q - whole, 1.0);
}
