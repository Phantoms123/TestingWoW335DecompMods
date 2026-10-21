// Draws a texture as it is (the ambient occlusion, modulated over the world, or shown on its own).
sampler2D Source : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
    return float4(tex2D(Source, uv).rgb, 1.0);
}
