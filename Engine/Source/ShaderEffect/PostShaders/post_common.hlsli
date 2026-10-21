// Shared by the post effects (Engine/Source/ShaderEffect/PostEffects.cpp). Not part of the original 12340 client.

// The scene depth (ViewSettings::Params): x near, y far, z and w turn a depth value d into the distance along the
// view axis, 1 / (d * z + w).
float ViewDepth(float d, float4 view) {
    return 1.0 / (d * view.z + view.w);
}

float Luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}
