// The water's real reflection, as seen in water above or below the level it was drawn for, blended into the sky
// reflection by Amount's red. Mirrored in the water's own level, everything reflected lies twice the difference
// (Amount's green) higher than in the reflection drawn: the point read is found by stepping back along the
// reflection's own depth, a few times over.
#include "post_common.hlsli"

sampler2D A : register(s0);          // the real reflection
sampler2D B : register(s1);          // the sky reflection
sampler2D Amount : register(s2);     // the water's mask
sampler2D Depth : register(s3);      // the real reflection's depth
float4 View : register(c0);          // the depth's near, far and conversion (ViewSettings::Params)
float4 Up : register(c1);            // world up in view space; w: the mask's height range (yards)
float4 Proj : register(c2);          // the projection's x and y scale and offset (a0, b1, c0, c1)
float4 Far : register(c3);           // x: depth beyond which is sky

float3 ViewPoint(float2 uv) {
    float d = tex2D(Depth, uv).x;
    float z = d >= Far.x ? 2000.0 : ViewDepth(d, View);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3((ndc.x - Proj.z) * z / Proj.x, (ndc.y - Proj.w) * z / Proj.y, z);
}

float2 ScreenUv(float3 p) {
    float2 ndc = float2(p.x * Proj.x / p.z + Proj.z, p.y * Proj.y / p.z + Proj.w);
    return float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
}

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    float4 mask = tex2D(Amount, uv);
    float raise = 2.0 * (mask.g * 2.0 - 1.0) * Up.w;
    float2 read = uv;

    for (int i = 0; i < 4; i++) {
        float3 p = ViewPoint(read) + Up.xyz * raise;
        float2 seen = p.z > 0.05 ? ScreenUv(p) : read;
        read = clamp(read - (seen - uv), 0.0, 1.0);
    }

    return float4(lerp(tex2D(A, read).rgb, tex2D(B, uv).rgb, mask.r), 1.0);
}
