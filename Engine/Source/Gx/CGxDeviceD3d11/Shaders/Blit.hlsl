// Copies a rectangle of a texture onto the bound target, filtered (TexStretch). cbuffer: x0 y0 x1 y1 of the source
// in texture coordinates.
cbuffer Blit : register(b0)
{
    float4 g_source;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID)
{
    float2 corner = float2((id << 1) & 2, id & 2);
    VsOut o;
    o.pos = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = lerp(g_source.xy, g_source.zw, corner);
    return o;
}

float4 PsMain(VsOut input) : SV_Target
{
    return g_texture.Sample(g_sampler, input.uv);
}
