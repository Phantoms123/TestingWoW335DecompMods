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
class CGxTex;
class CGxShader;
class C4Vector;

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
    // Blends color over all of target by amount (0-1).
    bool Tint(CGxTex* target, const CImVector& color, float amount);
    // Blends source over all of target (the same size) by amount (0-1).
    bool Blend(CGxTex* target, CGxTex* source, float amount);
    // The water's real reflection, lined up for water above or below its level by mask and its depth, blended into
    // the sky one (all the size of target). constants: 5 registers (see lerp_ps.hlsl).
    // scene and sceneDepth: the frame before the water, for what it reflects traced on screen (constants c0-c6).
    bool ReflectionBlend(CGxTex* target, CGxTex* real, CGxTex* sky, CGxTex* mask, CGxTex* depth, CGxTex* real2, CGxTex* depth2, CGxTex* scene, CGxTex* sceneDepth, const float* constants);
    // The shaders that draw the water's reflection mask (watermask_vs/ps.hlsl).
    bool WaterMaskShaders(CGxShader*& vertexShader, CGxShader*& pixelShader);
    // Softens target with a Gaussian blur of about radius texels, through temp (the same size).
    bool Soften(CGxTex* target, CGxTex* temp, float radius);
    // Fades target towards color by distance, read from depth (a GxTex_Intz texture drawn with view, see
    // ViewSettings::Params): none before start yards, all of it from end on; sky (depth >= skyDepth) stays.
    bool FadeByDepth(CGxTex* target, CGxTex* depth, const C4Vector& view, float skyDepth, float start, float end, const CImVector& color);
}

#endif
