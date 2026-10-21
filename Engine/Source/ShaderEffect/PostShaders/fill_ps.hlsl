// A flat colour (c0, its alpha the blend amount), blended over a target.
float4 Color : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
    return Color;
}
