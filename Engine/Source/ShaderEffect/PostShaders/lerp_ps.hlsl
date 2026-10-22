// The water's real reflection, read where Amount's green and blue say (0.5 in place, a quarter of the screen at
// most either way), blended into its sky one by Amount's red.
#include "post_common.hlsli"

sampler2D A : register(s0);
sampler2D B : register(s1);
sampler2D Amount : register(s2);

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    float4 mask = tex2D(Amount, uv);
    float2 shift = (mask.gb - 0.5) * 0.5;
    return float4(lerp(tex2D(A, uv + shift).rgb, tex2D(B, uv).rgb, mask.r), 1.0);
}
