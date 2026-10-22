#include "post_common.hlsli"

// Per pixel of the water: red, how much of the sky reflection it takes (none within Plane.y yards of the reflected
// level Plane.x, all of it Plane.y + 1 / Plane.z yards away); green and blue, how far (0.5 none, a quarter of the
// screen at most) the real reflection is to be read from, so that it lines up for water above or below the
// reflected level. Water behind the scene (DepthTex) is left out.
sampler2D DepthTex : register(s0);
float4 View : register(c0);
float4 Plane : register(c1);

float4 main(PS_POSITION float4 clip : TEXCOORD0, float4 raised : TEXCOORD1, float height : TEXCOORD2) : COLOR {
    float2 ndc = clip.xy / clip.w;
    float2 uv = float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
    float scene = ViewDepth(tex2D(DepthTex, uv).x, View);
    clip(scene * 1.002 + 0.1 - clip.w);
    float amount = saturate((abs(height - Plane.x) - Plane.y) * Plane.z);
    float2 raisedNdc = raised.xy / max(raised.w, 0.01);
    float2 shift = clamp(float2(0.5, -0.5) * (raisedNdc - ndc), -0.25, 0.25);
    return float4(amount, 0.5 - shift * 2.0, 1.0);
}
