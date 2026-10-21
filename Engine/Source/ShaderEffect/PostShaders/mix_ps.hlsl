// A texture blended over a target by an amount (c0.a): the water's real reflection eased into its sky one.
#include "post_common.hlsli"

sampler2D Source : register(s0);
float4 Amount : register(c0);

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    return float4(tex2D(Source, uv).rgb, Amount.a);
}
