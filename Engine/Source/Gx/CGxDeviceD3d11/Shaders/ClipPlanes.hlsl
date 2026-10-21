// Not part of the original 12340 client. Direct3D 9 applies user clip planes to shader draws in clip space; the
// client's shader model 4 vertex shaders (Shaders\Vertex\vs_4_0) cannot, so while clip planes are on (the water
// reflection's) this geometry shader passes their triangles through and adds the clip distances. All of those
// shaders write the same outputs, the ones below, and the pixel shaders read them by register.
cbuffer ClipPlanes : register(b0)
{
    float4 g_planes[6];       // clip space
    int4 g_mask;              // x: enabled planes
};

struct Vertex
{
    float4 pos : SV_Position;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float4 tc4 : TEXCOORD4;
    float4 tc5 : TEXCOORD5;
    float4 tc6 : TEXCOORD6;
    float4 tc7 : TEXCOORD7;
    float4 fog : FOG;
};

struct Clipped
{
    float4 pos : SV_Position;
    float4 color0 : COLOR0;
    float4 color1 : COLOR1;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float4 tc4 : TEXCOORD4;
    float4 tc5 : TEXCOORD5;
    float4 tc6 : TEXCOORD6;
    float4 tc7 : TEXCOORD7;
    float4 fog : FOG;
    float4 clip0 : SV_ClipDistance0;
    float2 clip1 : SV_ClipDistance1;
};

float Distance(int plane, float4 pos)
{
    return (g_mask.x & (1 << plane)) ? dot(pos, g_planes[plane]) : 1.0;
}

[maxvertexcount(3)]
void GsMain(triangle Vertex input[3], inout TriangleStream<Clipped> stream)
{
    for (int i = 0; i < 3; i++) {
        Clipped o;
        o.pos = input[i].pos;
        o.color0 = input[i].color0;
        o.color1 = input[i].color1;
        o.tc0 = input[i].tc0;
        o.tc1 = input[i].tc1;
        o.tc2 = input[i].tc2;
        o.tc3 = input[i].tc3;
        o.tc4 = input[i].tc4;
        o.tc5 = input[i].tc5;
        o.tc6 = input[i].tc6;
        o.tc7 = input[i].tc7;
        o.fog = input[i].fog;
        float4 p = input[i].pos;
        o.clip0 = float4(Distance(0, p), Distance(1, p), Distance(2, p), Distance(3, p));
        o.clip1 = float2(Distance(4, p), Distance(5, p));
        stream.Append(o);
    }
}
