// The water's real reflections blended into its sky one. Up to two were drawn, each mirrored in one water level
// (A at the first, the mask's green and blue holding the water's height above it; A2 at the second, Second.x
// lower); each pixel takes the one whose level is nearer its own, read in place. Water away from both levels
// takes what it reflects from the frame itself (traced on screen, SSR) where that can be found, else the sky.
#include "post_common.hlsli"

sampler2D A : register(s0);          // the real reflection at the first level
sampler2D B : register(s1);          // the sky reflection
sampler2D Amount : register(s2);     // the water's mask
sampler2D Depth : register(s3);      // the first reflection's depth
sampler2D A2 : register(s4);         // the real reflection at the second level
sampler2D Depth2 : register(s5);     // its depth
sampler2D Scene : register(s6);      // the frame drawn before the water
sampler2D SceneDepth : register(s7); // its depth
float4 View : register(c0);          // the depth's near, far and conversion (ViewSettings::Params)
float4 Up : register(c1);            // world up in view space; w: the mask's height range (yards)
float4 Proj : register(c2);          // the projection's x and y scale and offset (a0, b1, c0, c1)
float4 Far : register(c3);           // x: depth beyond which is sky; y, z: distance fade start, 1 / length
float4 Second : register(c4);        // x: first level - second level, y: 1 if the second was drawn; z, w: real
                                     // within z yards of a level, none 1 / w yards further
float4 Tone : register(c5);          // x: colour kept in the reflection (0 grey, 1 as reflected)
float4 Trace : register(c6);         // x: the first level's height from the camera, y: 1 traces on screen,
                                     // z: furthest traced (yards), w: how thick what is hit is taken to be

static const int STEPS = 40;
static const int REFINE = 6;

float2 ScreenUv(float3 p) {
    float2 ndc = float2(p.x * Proj.x / p.z + Proj.z, p.y * Proj.y / p.z + Proj.w);
    return float2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
}

float SceneZ(float2 uv) {
    return ViewDepth(tex2Dlod(SceneDepth, float4(uv, 0.0, 0.0)).x, View);
}

// What the flat water at this pixel reflects, from the frame: rgb, and a how much it can be trusted.
float4 TraceScene(float2 uv, float above1) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 ray = float3((ndc.x - Proj.z) / Proj.x, (ndc.y - Proj.w) / Proj.y, 1.0);
    float3 up = Up.xyz;
    float height = Trace.x + above1;
    float along = dot(up, ray);

    // The water lies below the camera, along a ray looking down at it.
    if (height >= -0.01 || along >= -1.0e-4) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float3 start = ray * (height / along);
    float3 dir = normalize(reflect(normalize(ray), up));
    // Lifted a little off the water, so it does not hit the banks right at its edge.
    start += up * 0.05;

    float last = 0.0;
    float dist = 0.5;
    float hit = -1.0;
    float limit = Trace.z;

    [loop]
    for (int i = 0; i < STEPS; i++) {
        float3 p = start + dir * dist;

        if (p.z <= View.x) {
            break;
        }

        float2 suv = ScreenUv(p);

        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) {
            break;
        }

        float behind = p.z - SceneZ(suv);

        if (behind > 0.0) {
            // Behind what is drawn there: a hit when not much behind (else it passed behind it, unseen).
            if (behind < Trace.w + dist * 0.08) {
                hit = dist;
            }

            break;
        }

        last = dist;
        dist = min(dist * 1.18 + 0.25, limit);

        if (last >= limit) {
            break;
        }
    }

    if (hit < 0.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float lo = last;
    float hi = hit;

    [loop]
    for (int j = 0; j < REFINE; j++) {
        float mid = 0.5 * (lo + hi);
        float3 p = start + dir * mid;

        if (p.z - SceneZ(ScreenUv(p)) > 0.0) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    float3 p = start + dir * hi;
    float2 suv = ScreenUv(p);
    // Less near the screen's edges, the furthest traced, and for rays turning back towards the camera.
    float2 edge = min(suv, 1.0 - suv);
    float trust = saturate(min(edge.x, edge.y) * 12.0);
    trust *= saturate((limit - hi) / (limit * 0.3));
    trust *= saturate(dir.z * 4.0 + 1.0);
    return float4(tex2Dlod(Scene, float4(suv, 0.0, 0.0)).rgb, trust);
}

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    float4 mask = tex2D(Amount, uv);
    float above1 = ((floor(mask.g * 255.0 + 0.5) + mask.b) / 255.0 * 2.0 - 1.0) * Up.w;
    float above2 = above1 + Second.x;
    // Between the two levels the two reflections blend over a couple of yards around the midpoint, so no seam.
    float second = Second.y > 0.5 ? saturate((abs(above1) - abs(above2)) * 0.5 + 0.5) : 0.0;
    float above = lerp(abs(above1), abs(above2), second);
    float3 real = lerp(tex2D(A, uv).rgb, tex2D(A2, uv).rgb, second);
    // Away from both levels neither reflection is right for the water there.
    float away = saturate((above - Second.z) * Second.w);
    // What the reflection shows far off fades away too (reflectionDistance).
    float d = lerp(tex2D(Depth, uv).x, tex2D(Depth2, uv).x, second);
    float fade = saturate((ViewDepth(d, View) - Far.y) * Far.z);
    fade = fade * fade * (3.0 - 2.0 * fade);
    float3 sky = tex2D(B, uv).rgb;
    float3 other = sky;

    // CHANGED: the frame itself where it holds what that water reflects (reflectionScreenSpace).
    if (Trace.y > 0.5 && max(away, fade) > 0.001) {
        float4 traced = TraceScene(uv, above1);
        other = lerp(sky, traced.rgb, traced.a);
    }

    float3 c = lerp(real, other, max(away, fade));
    // CHANGED: less colour in what the water reflects (reflectionSaturation): a sunset sky turned all of it orange.
    return float4(lerp(Luma(c).xxx, c, Tone.x), 1.0);
}
