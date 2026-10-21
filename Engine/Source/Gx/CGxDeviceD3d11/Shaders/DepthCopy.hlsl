// Copies the scene depth into a sampled depth texture (DeviceCopySampledDepth): bound as the only target, written
// through SV_Depth. Multisampled depth takes sample 0, as Direct3D 9's RESZ resolve does.
cbuffer Copy : register(b0)
{
    float4 g_info;    // x: the source is multisampled
};

Texture2D<float4> g_depth : register(t0);
Texture2DMS<float4> g_depthMS : register(t1);

float PsMain(float4 pos : SV_Position) : SV_Depth
{
    int2 p = int2(pos.xy);

    if (g_info.x > 0.0) {
        return g_depthMS.Load(p, 0).x;
    }

    return g_depth.Load(int3(p, 0)).x;
}
