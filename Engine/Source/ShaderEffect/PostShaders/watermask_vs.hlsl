#include "post_common.hlsli"

// The water's height from the reflected level, per pixel (Liquid::Cata's reflection mask): its own geometry
// through c0-c3 (the world, view and projection rows, a row vector times them), c4 the world matrix's z column.
float4 Row0 : register(c0);
float4 Row1 : register(c1);
float4 Row2 : register(c2);
float4 Row3 : register(c3);
float4 WorldZ : register(c4);

struct VsOut {
    float4 position : VS_POSITION;
    float4 clip : TEXCOORD0;
    float height : TEXCOORD1;
};

VsOut main(float3 position : POSITION) {
    VsOut o;
    o.position = position.x * Row0 + position.y * Row1 + position.z * Row2 + Row3;
    o.clip = o.position;
    o.height = dot(float4(position, 1.0), WorldZ);
    return o;
}
