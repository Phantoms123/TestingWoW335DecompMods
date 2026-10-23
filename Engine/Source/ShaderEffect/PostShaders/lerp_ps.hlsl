// The water's real reflections blended into its sky one. Up to two were drawn, each mirrored in one water level
// (A at the first, the mask's green holding the water's height above it; A2 at the second, Second.x lower);
// each pixel takes the one whose level is nearer its own, read where it lines up: mirrored in the water's own
// level everything reflected lies twice the difference higher, so the point read is found by stepping back along
// that reflection's depth, a few times over. Away from both levels the sky reflection takes over.
#include "post_common.hlsli"

sampler2D A : register(s0);          // the real reflection at the first level
sampler2D B : register(s1);          // the sky reflection
sampler2D Amount : register(s2);     // the water's mask
sampler2D Depth : register(s3);      // the first reflection's depth
sampler2D A2 : register(s4);         // the real reflection at the second level
sampler2D Depth2 : register(s5);     // its depth
float4 View : register(c0);          // the depth's near, far and conversion (ViewSettings::Params)
float4 Up : register(c1);            // world up in view space; w: the mask's height range (yards)
float4 Proj : register(c2);          // the projection's x and y scale and offset (a0, b1, c0, c1)
float4 Far : register(c3);           // x: depth beyond which is sky
float4 Second : register(c4);        // x: first level - second level, y: 1 if the second was drawn; z, w: real
                                     // within z yards of a level, all sky 1 / w yards further

float3 ViewPoint(float d, float2 uv) {
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
    float above1 = (mask.g * 2.0 - 1.0) * Up.w;
    float above2 = above1 + Second.x;
    bool useSecond = Second.y > 0.5 && abs(above2) < abs(above1);
    float above = useSecond ? above2 : above1;
    // CHANGED: read in place. Shifting the reflection to suit water at another level pulled the banks between
    // the two levels into it (orange smears that followed the camera); each level now has its own reflection.
    float3 real = useSecond ? tex2D(A2, uv).rgb : tex2D(A, uv).rgb;
    float sky = saturate((abs(above) - Second.z) * Second.w);
    return float4(lerp(real, tex2D(B, uv).rgb, sky), 1.0);
}
