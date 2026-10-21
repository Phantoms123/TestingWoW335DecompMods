#include "post_common.hlsli"

// Full-screen quad for the post effects. c0.zw: half a texel of the target (Direct3D 9 pixel centres).
// c1-c3: the view ray at the screen's top left and its change across the screen (u) and down it (v), scaled so
// that its component along the view axis is 1 (the ray times the view depth is the camera-relative position).
float4 Texel : register(c0);
float4 RayOrigin : register(c1);
float4 RayU : register(c2);
float4 RayV : register(c3);

struct VsOut {
    float4 position : VS_POSITION;
    float2 uv : TEXCOORD0;
    float3 ray : TEXCOORD1;
};

VsOut main(float3 position : POSITION) {
    VsOut o;
    o.position = float4(position.xy, 0.0, 1.0);
    float2 screen = float2(0.5 + 0.5 * position.x, 0.5 - 0.5 * position.y);
    o.uv = screen + Texel.zw;
    o.ray = RayOrigin.xyz + RayU.xyz * screen.x + RayV.xyz * screen.y;
    return o;
}
