// Screen space ambient occlusion from the scene depth (scalable ambient obscurance style): positions rebuilt
// from the depth along the view ray, a normal from the neighbouring depths, twelve samples on a spiral.
#include "post_common.hlsli"

sampler2D DepthTex : register(s0);
float4 View : register(c0);
float4 Params : register(c1);   // pixels per yard at depth 1, radius (yards), strength, sky depth
float4 Texel : register(c2);    // depth texel size, screen size in pixels
float4 RayOrigin : register(c3);
float4 RayU : register(c4);
float4 RayV : register(c5);

float3 Position(float2 uv) {
    float d = tex2D(DepthTex, uv).x;
    float3 ray = RayOrigin.xyz + RayU.xyz * uv.x + RayV.xyz * uv.y;
    return ray * ViewDepth(min(d, Params.w), View);
}

float4 main(float2 uv : TEXCOORD0, float3 ray : TEXCOORD1) : COLOR {
    float d = tex2D(DepthTex, uv).x;

    if (d >= Params.w) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    float depth = ViewDepth(d, View);
    float3 p = ray * depth;

    // The normal from whichever neighbour on each axis is nearer in depth (keeps edges sharp).
    float3 px0 = Position(uv - float2(Texel.x, 0.0));
    float3 px1 = Position(uv + float2(Texel.x, 0.0));
    float3 py0 = Position(uv - float2(0.0, Texel.y));
    float3 py1 = Position(uv + float2(0.0, Texel.y));
    float3 dx = dot(px1 - p, px1 - p) < dot(p - px0, p - px0) ? px1 - p : p - px0;
    float3 dy = dot(py1 - p, py1 - p) < dot(p - py0, p - py0) ? py1 - p : p - py0;
    float3 n = normalize(cross(dx, dy));

    if (dot(n, p) > 0.0) {
        n = -n;
    }

    float radius = Params.y;
    float radius2 = radius * radius;
    float radiusPixels = min(Params.x * radius / depth, 80.0);

    if (radiusPixels < 1.0) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    float2 pixel = uv * Texel.zw;
    float angle = frac(52.9829189 * frac(dot(floor(pixel), float2(0.06711056, 0.00583715)))) * 6.2831853;
    float bias = 0.002 * depth + 0.02;
    float sum = 0.0;

    [unroll]
    for (int i = 0; i < 12; i++) {
        float alpha = (i + 0.5) / 12.0;
        float a = alpha * 43.98229 + angle;   // seven turns
        float2 offset = float2(cos(a), sin(a)) * (alpha * radiusPixels) * Texel.xy;
        float3 v = Position(uv + offset) - p;
        float vv = dot(v, v);
        float vn = dot(v, n);
        float f = max(radius2 - vv, 0.0);
        sum += f * f * f * max((vn - bias) / (vv + 0.01), 0.0);
    }

    float ao = sum * 5.0 / (radius2 * radius2 * radius2 * 12.0);
    // Fade it out with distance, where a pixel covers more than the radius anyway.
    float fade = saturate((400.0 - depth) / 200.0);
    ao = saturate(1.0 - ao * Params.z * fade);
    return float4(ao, ao, ao, 1.0);
}
