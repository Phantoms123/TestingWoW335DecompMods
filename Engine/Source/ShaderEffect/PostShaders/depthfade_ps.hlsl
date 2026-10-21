// Fades what lies far away towards a colour, by the depth it was drawn with (the water's reflection: distant
// terrain, buildings and models fade into the fog instead of showing sharp at any distance).
#include "post_common.hlsli"

sampler2D DepthTex : register(s0);
float4 View : register(c0);
float4 Fade : register(c1);     // start (yards), 1 / (end - start), depth beyond which is sky
float4 Color : register(c2);

float4 main(PS_POSITION float2 uv : TEXCOORD0) : COLOR {
    float d = tex2D(DepthTex, uv).x;

    if (d >= Fade.z) {
        return float4(Color.rgb, 0.0);
    }

    float amount = saturate((ViewDepth(d, View) - Fade.x) * Fade.y);
    amount = amount * amount * (3.0 - 2.0 * amount);
    return float4(Color.rgb, amount);
}
