#ifndef ENGINE_SOURCE_SHADEREFFECT_POSTEFFECTS_H
#define ENGINE_SOURCE_SHADEREFFECT_POSTEFFECTS_H

// Screen effects behind the Advanced Effects video options (the settings Waystone's 4.3.4-based client offers):
// ambient occlusion and aerial haze over the opaque world, bloom with filmic tone, and edge anti-aliasing with
// contrast adaptive sharpening over the finished world. Not part of the original 12340 client.
//
// They run as Direct3D 9 pixel shaders (PostShaders/, compiled into the client), read the scene depth from
// DepthCache, and draw nothing when their setting is off or the device cannot run them.

#include <cstdint>

class C3Vector;
class CImVector;

namespace PostEffects {
    enum EParam {
        Param_AoRadius,
        Param_AoStrength,
        Param_AoDebug,
        Param_BloomIntensity,
        Param_BloomThreshold,
        Param_ToneMapping,
        Param_ToneExposure,
        Param_ColorSaturation,
        Param_ColorContrast,
        Param_Sharpen,
        Param_AerialFogDensity,
        Param_AerialFogFalloff,
        Param_AerialFogStart,
        Param_AerialFogMax,
        Params_Last,
    };

    void SetAmbientOcclusion(bool enable);
    void SetBloom(bool enable);
    void SetAntiAliasing(bool enable);
    void SetAerialFog(bool enable);
    void SetParam(EParam param, float value);

    // Once a frame before the world draws (before DepthCache::BeginFrame): creates or frees what the settings need.
    void BeginFrame();
    // After the opaque world, before the liquids: ambient occlusion, then the haze.
    void RenderOpaque(const C3Vector& cameraPos, const CImVector& fogColor, float zNear, float zFar, float viewportFarClip);
    // Over the finished world, before the full screen effects (glow): bloom and tone.
    void RenderBloom();
    // Last, after the full screen effects: anti-aliasing and sharpening.
    void RenderAntiAliasing();
}

#endif
