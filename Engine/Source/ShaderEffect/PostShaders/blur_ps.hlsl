// Nine-tap Gaussian blur in five bilinear fetches, one direction a pass.
sampler2D Source : register(s0);
float4 Step : register(c0);     // one texel along the blur direction

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 c = tex2D(Source, uv).rgb * 0.2270270;
    c += tex2D(Source, uv + Step.xy * 1.3846154).rgb * 0.3162162;
    c += tex2D(Source, uv - Step.xy * 1.3846154).rgb * 0.3162162;
    c += tex2D(Source, uv + Step.xy * 3.2307692).rgb * 0.0702703;
    c += tex2D(Source, uv - Step.xy * 3.2307692).rgb * 0.0702703;
    return float4(c, 1.0);
}
