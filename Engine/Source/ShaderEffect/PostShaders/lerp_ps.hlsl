// Two textures blended by a third (its red): the water's real reflection eased into its sky one.
#include "post_common.hlsli"

sampler2D A : register(s0);
sampler2D B : register(s1);
sampler2D Amount : register(s2);

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    return float4(lerp(tex2D(A, uv).rgb, tex2D(B, uv).rgb, tex2D(Amount, uv).r), 1.0);
}
