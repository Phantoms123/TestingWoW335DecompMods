// Shared by the post effects (Engine/Source/ShaderEffect/PostEffects.cpp). Not part of the original 12340 client.

// Built twice (build.sh): shader model 3 for Direct3D 9 and, with SM4 defined, shader model 4 for Direct3D 11.
// Shader model 4 stages link by register, so every pixel shader takes the position first, as the vertex shader
// writes it (o0 position, o1 TEXCOORD0, o2 TEXCOORD1).
#ifdef SM4
#define VS_POSITION SV_Position
#define PS_POSITION float4 screenPosition : SV_Position,
#else
#define VS_POSITION POSITION
#define PS_POSITION
#endif

// The scene depth (ViewSettings::Params): x near, y far, z and w turn a depth value d into the distance along the
// view axis, 1 / (d * z + w).
float ViewDepth(float d, float4 view) {
    return 1.0 / (d * view.z + view.w);
}

float Luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}
