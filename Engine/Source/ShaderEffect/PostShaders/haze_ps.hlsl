// Aerial haze: exponential height fog in the zone fog colour, from the scene depth, blended over the world.
#include "post_common.hlsli"

sampler2D DepthTex : register(s0);
float4 View : register(c0);
float4 Fog : register(c1);      // density (1/yard), height falloff (1/yard), start (yards), maximum opacity
float4 FogColor : register(c2);
float4 Camera : register(c3);   // camera height, depth beyond which is sky

float4 main(float2 uv : TEXCOORD0, float3 ray : TEXCOORD1) : COLOR {
    float d = tex2D(DepthTex, uv).x;

    if (d >= Camera.y) {
        return float4(FogColor.rgb, 0.0);
    }

    float rayLength = length(ray);
    float dist = ViewDepth(d, View) * rayLength;
    float t = max(dist - Fog.z, 0.0);
    float dirZ = ray.z / rayLength;

    // Density falls off with height above sea level, integrated along the ray past the start distance.
    float startHeight = Camera.x + dirZ * Fog.z;
    float heightTerm = exp(clamp(-Fog.y * startHeight, -20.0, 1.5));
    float k = clamp(Fog.y * dirZ * t, -30.0, 30.0);
    float along = abs(k) > 0.0001 ? (1.0 - exp(-k)) / k : 1.0;
    float optical = Fog.x * t * heightTerm * along;
    float amount = Fog.w * (1.0 - exp(-optical));
    return float4(FogColor.rgb, saturate(amount));
}
