// A flat colour (c0, its alpha the blend amount), blended over a target.
#include "post_common.hlsli"

float4 Color : register(c0);

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    return Color;
}
