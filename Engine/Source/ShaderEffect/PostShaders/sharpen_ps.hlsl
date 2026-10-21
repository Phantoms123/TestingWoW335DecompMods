// Contrast adaptive sharpening (after AMD FidelityFX CAS): sharpens less where the local contrast is high.
sampler2D Scene : register(s0);
float4 Params : register(c0);   // texel size, sharpness (0-1)

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 e = tex2D(Scene, uv).rgb;
    float3 b = tex2D(Scene, uv + float2(0.0, -Params.y)).rgb;
    float3 d = tex2D(Scene, uv + float2(-Params.x, 0.0)).rgb;
    float3 f = tex2D(Scene, uv + float2(Params.x, 0.0)).rgb;
    float3 h = tex2D(Scene, uv + float2(0.0, Params.y)).rgb;

    float3 mn = min(e, min(min(b, d), min(f, h)));
    float3 mx = max(e, max(max(b, d), max(f, h)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 0.0001)));
    float peak = -1.0 / lerp(8.0, 5.0, saturate(Params.z));
    float3 w = amp * peak;
    float3 c = (b + d + f + h) * w + e;
    return float4(saturate(c / (1.0 + 4.0 * w)), 1.0);
}
