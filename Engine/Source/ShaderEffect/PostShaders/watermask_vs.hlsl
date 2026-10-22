#include "post_common.hlsli"

// The water's height from the reflected level, per pixel (Liquid::Cata's reflection mask): its own geometry
// through c0-c3 (the world, view and projection rows, a row vector times them), c4 the world matrix's z column,
// c5 the view and projection's z row (a world height change in clip space), c6.x the reflected level.
float4 Row0 : register(c0);
float4 Row1 : register(c1);
float4 Row2 : register(c2);
float4 Row3 : register(c3);
float4 WorldZ : register(c4);
float4 ClipZ : register(c5);
float4 Plane : register(c6);

struct VsOut {
    float4 position : VS_POSITION;
    float4 clip : TEXCOORD0;
    float4 raised : TEXCOORD1;
    float height : TEXCOORD2;
};

VsOut main(float3 position : POSITION) {
    VsOut o;
    o.position = position.x * Row0 + position.y * Row1 + position.z * Row2 + Row3;
    o.clip = o.position;
    o.height = dot(float4(position, 1.0), WorldZ);
    // Mirrored in the water's own height rather than the reflected level, everything reflected sits twice the
    // difference higher: where the water's own point lands then, on screen.
    o.raised = o.position + ClipZ * (2.0 * (o.height - Plane.x));
    return o;
}
