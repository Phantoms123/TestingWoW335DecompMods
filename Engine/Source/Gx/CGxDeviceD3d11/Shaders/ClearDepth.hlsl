// Clears the depth within the viewport only, as Direct3D 9's Clear does (SceneClear): drawn over the viewport
// with the blit vertex shader, writing the far plane through SV_Depth.
float PsMain(float4 pos : SV_Position) : SV_Depth
{
    return 1.0;
}
