#include "WoW/Source/LiquidSystem/CataWater.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/BlizzardCore/SFile2-IO.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxDevice/ShaderCompat.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/ShaderEffect/PostEffects.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/ShaderEffect/ViewSettings.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C33Matrix.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Plane.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CRect.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/MapWeather.h"
#include "WoW/Source/WorldClient/World.h"

// Register layout of 4.3.4's ProcWater vertex shader and ProcWaterAbove/ProcWaterBelow pixel shaders, as 4.3.4's
// Liquid::CMaterialWater fills it (vertex c0-c52, pixel c0-c31).
static const uint32_t VS_REGISTERS = 0x35;
static const uint32_t PS_REGISTERS = 0x20;

// Permutation counts of the 4.3.4 shader files.
static const uint32_t VS_PERMUTATIONS = 12;
static const uint32_t ABOVE_PERMUTATIONS = 54;
static const uint32_t BELOW_PERMUTATIONS = 27;

// Blurred copies are this size; the frame copy and the sky reflection follow the window.
static const uint32_t BLUR_SIZE = 512;
static const uint32_t BLUR_PASSES = 4;
static const float BLUR_SPREAD = 0.25f;

// 4.3.4's depth mode for a depth texture the hardware fills (DepthCache::DM_Texture).
static const uint32_t DEPTH_MODE = 3;

static uint32_t s_requestedDetail = 2;
static uint32_t s_detail;
static uint32_t s_rippleDetail;
static uint32_t s_requestedRipple = 1;
static bool s_rippleFailed;
static uint32_t s_reflectionMode;
static uint32_t s_reflectionDownscale;
static float s_reflectionStrength = 1.0f;
static float s_shorelineSoftness = 1.0f;
static bool s_failed;

static CGxShader* s_vertexShaders[VS_PERMUTATIONS];
static CGxShader* s_abovePixelShaders[ABOVE_PERMUTATIONS];
static CGxShader* s_belowPixelShaders[BELOW_PERMUTATIONS];
static CGxShader* s_blurVertexShader;
static CGxShader* s_blurPixelShader;

static CTexture* s_backBufferTex;
static CTexture* s_fakeReflectionTex;
static CTexture* s_realReflectionTex;
static CTexture* s_realReflectionTemp;
static CTexture* s_realReflectionDepth;
// CHANGED: a second real reflection, for the nearest water at another level (a stream above the pond it runs into):
// one mirror is only right for water at its own level.
static CTexture* s_real2ReflectionTex;
static CTexture* s_real2ReflectionDepth;
static bool s_real2Ready;
static bool s_plane2Valid;
static float s_plane2Height;
static uint32_t s_plane2Hold;
// Other water nearest since then (it becomes the first level after a little while).
static float s_pendingHeight;
static uint32_t s_pendingSince;
// The camera's height when water was last drawn (the levels are chosen for the next frame).
static float s_lastCameraZ;
static bool s_lastCameraZValid;
// The nearest water at the second level's height this frame (it keeps the level until other water is much nearer).
static bool s_nextSame2Valid;
static float s_nextSame2Distance;
static float s_nextSame2Height;
static bool s_next2Valid;
static float s_next2Distance;
static float s_next2Height;
static uint32_t s_reflectionIndex;
static uint32_t s_reflectionPlanes = 2;
// CHANGED: reflectionBlend 0 (default) draws the water as before the per-pixel blending (one real reflection,
// eased into the sky one in quarter steps per chunk); 1 the per-pixel blend and second level.
static bool s_reflectionPerPixel = true;
static float s_reflectionSaturation = 0.0f;
// The real reflection eased into the sky one (a quarter, half and three quarters sky), for water a little away
// from the reflected level; made once a pass, when water first needs one.
static const uint32_t REFLECTION_MIXES = 3;
static CTexture* s_reflectionMixTex[REFLECTION_MIXES];
static bool s_reflectionMixTried[REFLECTION_MIXES];
static bool s_reflectionMixReady[REFLECTION_MIXES];
// The per-pixel version: how far from the level each pixel's water lies (0 real, 1 sky; reflection sized), and
// the real and sky reflections blended by it, which then all the water draws.
static CTexture* s_reflectionMaskTex;
static CTexture* s_reflectionBlendTex;
static bool s_reflectionMaskDrawing;
static bool s_reflectionBlendReady;
static float s_maskSavedVs[8 * 4];
static float s_maskSavedPs[8 * 4];
static CGxTex* s_maskSavedColor;
static float s_maskSavedViewport[6];
// Up to this far (yards) from the reflected level water shows the real reflection, from the far one on the sky's.
static const float REFLECTION_BLEND_NEAR = 30.0f;
static const float REFLECTION_BLEND_FAR = 60.0f;
// The heights the mask holds, either way of the reflected level (yards; 8 bits over twice this).
static const float REFLECTION_MASK_RANGE = 64.0f;
// CHANGED: with the reflections traced on screen (reflectionScreenSpace), water this far from a reflected level
// takes them instead (fully 4 yards further), rather than a real reflection mirrored at the wrong height; nearer than
// that the real one stays (a sloping stream's steps took one, then the other, and showed as patches).
static const float REFLECTION_TRACE_NEAR = 6.0f;
static const float REFLECTION_TRACE_FAR = 10.0f;
static const float REFLECTION_TRACE_DISTANCE = 150.0f;
static bool s_reflectionScreenSpace = true;
static float s_maskCameraZ;
static float s_reflectionDistance = 200.0f;
// Models smaller than this part of their distance are left out of the reflection (reflectionModelSize).
static float s_reflectionModelSize = 0.004f;
// How many screens' height above the view the culling reaches for the reflection (reflectionCullHeight).
static float s_reflectionCullHeight = 2.0f;
static bool s_reflectionDepthBound;
static CTexture* s_blurredBackTex;
static CTexture* s_blurTempTex;
static uint32_t s_screenWidth;
static uint32_t s_screenHeight;
static uint32_t s_reflectionWidth;
static uint32_t s_reflectionHeight;

static float s_zNear = 0.2f;
static float s_zFar = 1000.0f;
static float s_viewportFarClip = 1.0f;
static bool s_cameraUnderwater;

// The water plane the real reflection mirrors in: the height of the water drawn nearest the camera last frame.
static bool s_planeValid;
static float s_planeHeight;
static bool s_nextPlaneValid;
static float s_nextPlaneHeight;
static float s_nextPlaneDistance;
// The nearest water at about the level already reflected: other water takes over only when clearly nearer.
static bool s_nextSameValid;
static float s_nextSameDistance;
static float s_nextSameHeight;
// Whether the last see-through pass drew its water with the per-pixel blend (reflectionDebug).
static bool s_lastPassBlended;
static bool s_realReady;
// Smoothing: the plane is kept for a while after its water was last seen, eases between nearby heights, and the
// reflection fades in and out instead of switching at once.
static const uint32_t PLANE_HOLD_FRAMES = 45;
static const float REFLECTION_FADE_PER_SECOND = 3.0f;
static uint32_t s_planeHold;
static bool s_planeSeen;
static float s_reflectionBlend;
static uint32_t s_lastBlendMs;
static bool s_inReflection;

// Per liquid pass: whether the frame copy, blur and sky reflection were made, and whether they could be.
static bool s_inPass;
static bool s_passPrepared;
static bool s_passReady;

static float s_vs[VS_REGISTERS * 4];
static float s_ps[PS_REGISTERS * 4];

// What the pass overwrites in the client's own constants (some stay set for a whole frame).
static float s_savedVs[VS_REGISTERS * 4];
static float s_savedPs[PS_REGISTERS * 4];

static const uint16_t s_quadIndices[4] = { 0, 3, 1, 2 };
static const C3Vector s_quad[4] = { C3Vector(-1.0f, -1.0f, 0.0f), C3Vector(-1.0f, 1.0f, 0.0f), C3Vector(1.0f, 1.0f, 0.0f), C3Vector(1.0f, -1.0f, 0.0f) };

// Logs each kind of event once (to the terminal and the debug log), so a water that stays 3.3.5 says why.
static uint32_t s_logged;

static void LogOnce(uint32_t bit, const char* format, ...) {
    if (s_logged & (1u << bit)) {
        return;
    }

    s_logged |= 1u << bit;
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fprintf(stderr, "Liquid (4.3.4 water): %s\n", message);
    DebugLog("Liquid (4.3.4 water): %s\n", message);
}

static void SetVs(uint32_t reg, float x, float y, float z, float w) {
    float* c = &s_vs[reg * 4];
    c[0] = x;
    c[1] = y;
    c[2] = z;
    c[3] = w;
}

static void SetPs(uint32_t reg, float x, float y, float z, float w) {
    float* c = &s_ps[reg * 4];
    c[0] = x;
    c[1] = y;
    c[2] = z;
    c[3] = w;
}

static void SetVs(uint32_t reg, const C44Matrix& m) {
    memcpy(&s_vs[reg * 4], &m, sizeof(C44Matrix));
}

static void SetPs(uint32_t reg, const C44Matrix& m) {
    memcpy(&s_ps[reg * 4], &m, sizeof(C44Matrix));
}

static void SetPs(uint32_t reg, const C4Vector& v) {
    SetPs(reg, v.x, v.y, v.z, v.w);
}

static void ReleaseShader(CGxShader*& shader) {
    if (shader) {
        g_theGxDevicePtr->ShaderDestroy(shader);
        shader = nullptr;
    }
}

static void ReleaseTexture(CTexture*& texture) {
    if (texture) {
        HandleClose(reinterpret_cast<HOBJECT>(texture));
        texture = nullptr;
    }
}

static void DestroyTargets() {
    ReleaseTexture(s_backBufferTex);
    ReleaseTexture(s_fakeReflectionTex);
    ReleaseTexture(s_realReflectionTex);
    ReleaseTexture(s_realReflectionTemp);
    ReleaseTexture(s_realReflectionDepth);
    ReleaseTexture(s_real2ReflectionTex);
    ReleaseTexture(s_real2ReflectionDepth);

    for (uint32_t i = 0; i < REFLECTION_MIXES; i++) {
        ReleaseTexture(s_reflectionMixTex[i]);
        s_reflectionMixTried[i] = false;
        s_reflectionMixReady[i] = false;
    }

    ReleaseTexture(s_reflectionMaskTex);
    ReleaseTexture(s_reflectionBlendTex);
    s_reflectionBlendReady = false;
    ReleaseTexture(s_blurredBackTex);
    ReleaseTexture(s_blurTempTex);
    s_screenWidth = 0;
    s_screenHeight = 0;
    s_reflectionWidth = 0;
    s_reflectionHeight = 0;
}

static void DestroyRipples();
static void UpdateRippleResources();

static void Destroy() {
    DestroyRipples();
    for (CGxShader*& shader : s_vertexShaders) {
        ReleaseShader(shader);
    }

    for (CGxShader*& shader : s_abovePixelShaders) {
        ReleaseShader(shader);
    }

    for (CGxShader*& shader : s_belowPixelShaders) {
        ReleaseShader(shader);
    }

    ReleaseShader(s_blurVertexShader);
    ReleaseShader(s_blurPixelShader);
    DestroyTargets();
}

static bool Valid(CGxShader* shader) {
    return shader && shader->Valid();
}

// The pixel shader permutation 4.3.4 picks: depth mode, ripple detail and reflection kind (0 = sky reflection,
// 1 = real reflection, 2 = none), and whether the depth texture needs unpacking (not for a hardware depth texture).
static uint32_t AbovePermutation(uint32_t reflection) {
    return 1 + ((s_rippleDetail + (DEPTH_MODE - 1) * 3) * 3 + reflection) * 2;
}

static uint32_t BelowPermutation(uint32_t reflection) {
    return reflection + (s_rippleDetail + (DEPTH_MODE - 1) * 3) * 3;
}

static bool CreateShaders() {
    CGxDevice* device = g_theGxDevicePtr;
    device->ShaderCreate(s_vertexShaders, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "ProcWater", VS_PERMUTATIONS);
    device->ShaderCreate(s_abovePixelShaders, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "ProcWaterAbove", ABOVE_PERMUTATIONS);
    device->ShaderCreate(s_belowPixelShaders, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "ProcWaterBelow", BELOW_PERMUTATIONS);
    device->ShaderCreate(&s_blurVertexShader, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "FourTapBlur", 1);
    device->ShaderCreate(&s_blurPixelShader, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "FourTapBlur", 1);

    bool ok = true;
    const struct {
        CGxShader* shader;
        const char* name;
    } checks[] = {
        { s_vertexShaders[0], "Vertex\\vs_3_0\\ProcWater" },
        { s_abovePixelShaders[AbovePermutation(0)], "Pixel\\ps_3_0\\ProcWaterAbove" },
        { s_belowPixelShaders[BelowPermutation(0)], "Pixel\\ps_3_0\\ProcWaterBelow" },
        { s_blurVertexShader, "Vertex\\vs_3_0\\FourTapBlur" },
        { s_blurPixelShader, "Pixel\\ps_3_0\\FourTapBlur" },
    };

    for (const auto& check : checks) {
        if (!Valid(check.shader)) {
            fprintf(stderr, "Liquid (4.3.4 water): Shaders\\Cata\\%s.bls did not load\n", check.name);
            ok = false;
        }
    }

    return ok;
}

static CTexture* CreateTarget(EGxTexTarget target, uint32_t width, uint32_t height, const char* name, EGxTexFormat format = GxTex_Argb8888) {
    CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1);
    CTexture* texture = TextureCreate(target, width, height, 0, format, format, flags, nullptr, GxTexNullUserFunc, name, 0);

    if (texture && !TextureGetGxTex(texture, 1, nullptr)) {
        ReleaseTexture(texture);
    }

    return texture;
}

// The frame copy and sky reflection follow the window size; recreated when it changes.
static bool EnsureTargets() {
    CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(window);
    uint32_t width = static_cast<uint32_t>(window.maxX - window.minX);
    uint32_t height = static_cast<uint32_t>(window.maxY - window.minY);

    if (width < 16 || height < 16) {
        return false;
    }

    uint32_t shift = s_reflectionDownscale > 4 ? 4 : s_reflectionDownscale;
    uint32_t reflectionWidth = width >> shift;
    uint32_t reflectionHeight = height >> shift;

    if (width != s_screenWidth || height != s_screenHeight || reflectionWidth != s_reflectionWidth || reflectionHeight != s_reflectionHeight) {
        DestroyTargets();
        s_backBufferTex = CreateTarget(GxTex_NonPow2, width, height, "LiquidProc BackBufferTex");
        s_fakeReflectionTex = CreateTarget(GxTex_NonPow2, reflectionWidth, reflectionHeight, "LiquidProc Fake Reflection");
        s_screenWidth = width;
        s_screenHeight = height;
        s_reflectionWidth = reflectionWidth;
        s_reflectionHeight = reflectionHeight;
    }

    if (!s_realReflectionTex && s_detail >= 3) {
        s_realReflectionTex = CreateTarget(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, "LiquidProc Real Reflection");
    }

    // Its depth as a texture, so the distance fade can read it ("LiquidProc Reflection Depth" in 4.3.4).
    if (!s_realReflectionDepth && s_detail >= 3 && g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Intz]) {
        CGxTexFlags flags(GxTex_Nearest, 0, 0, 0, 0, 1);
        s_realReflectionDepth = TextureCreate(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, 0, GxTex_Intz, GxTex_Intz, flags, nullptr, GxTexNullUserFunc, "LiquidProc Reflection Depth", 0);

        if (s_realReflectionDepth && !TextureGetGxTex(s_realReflectionDepth, 1, nullptr)) {
            ReleaseTexture(s_realReflectionDepth);
        }
    }

    if (s_reflectionPlanes >= 2 && s_detail >= 3) {
        if (!s_real2ReflectionTex) {
            s_real2ReflectionTex = CreateTarget(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, "LiquidProc Real Reflection 2");
        }

        if (!s_real2ReflectionDepth && g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Intz]) {
            CGxTexFlags flags(GxTex_Nearest, 0, 0, 0, 0, 1);
            s_real2ReflectionDepth = TextureCreate(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, 0, GxTex_Intz, GxTex_Intz, flags, nullptr, GxTexNullUserFunc, "LiquidProc Reflection Depth 2", 0);

            if (s_real2ReflectionDepth && !TextureGetGxTex(s_real2ReflectionDepth, 1, nullptr)) {
                ReleaseTexture(s_real2ReflectionDepth);
            }
        }
    }

    if (!s_realReflectionTemp && s_detail >= 3) {
        s_realReflectionTemp = CreateTarget(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, "blurredReflection");
    }

    if (!s_blurredBackTex) {
        s_blurredBackTex = CreateTarget(GxTex_2d, BLUR_SIZE, BLUR_SIZE, "blurredBack");
    }

    if (!s_blurTempTex) {
        s_blurTempTex = CreateTarget(GxTex_2d, BLUR_SIZE, BLUR_SIZE, "blurTemp");
    }

    return s_backBufferTex && s_fakeReflectionTex && s_blurredBackTex && s_blurTempTex;
}

static CGxTex* GxTexOf(CTexture* texture) {
    return texture ? TextureGetGxTex(texture, 1, nullptr) : nullptr;
}

static bool Supported() {
    const CGxCaps& caps = g_theGxDevicePtr->Caps();
    bool ok = Liquid::s_shadersEnabled && caps.m_shaderTargets[GxSh_Vertex] >= 1 && caps.m_shaderTargets[GxSh_Pixel] >= 3 && DepthCache::IsSupported() &&
              caps.m_rttFormat[GxTex_Argb8888] && caps.m_texTarget[GxTex_NonPow2] == 1;

    if (!ok) {
        LogOnce(0, "off, the device cannot run it (liquid shaders %d, vs %d, ps %d, depth texture %d, render targets %d, non-power-of-two %d, multisample %u)",
            Liquid::s_shadersEnabled, caps.m_shaderTargets[GxSh_Vertex], caps.m_shaderTargets[GxSh_Pixel], DepthCache::IsSupported() ? 1 : 0,
            caps.m_rttFormat[GxTex_Argb8888], caps.m_texTarget[GxTex_NonPow2], g_theGxDevicePtr->m_format.m_multisampleCount);
    }

    return ok;
}

void Liquid::Cata::SetWaterDetail(uint32_t detail) {
    s_requestedDetail = detail > 3 ? 3 : detail;
    s_failed = false;
}

void Liquid::Cata::SetRippleDetail(uint32_t detail) {
    s_requestedRipple = detail > 2 ? 2 : detail;
    s_rippleFailed = false;
}

void Liquid::Cata::SetReflectionMode(uint32_t mode) {
    s_reflectionMode = mode > 3 ? 3 : mode;
}

void Liquid::Cata::SetReflectionModelSize(float size) {
    s_reflectionModelSize = size < 0.0f ? 0.0f : size;
}

void Liquid::Cata::SetReflectionDistance(float distance) {
    s_reflectionDistance = distance < 0.0f ? 0.0f : distance;
}

static bool s_reflectionDebug;

void Liquid::Cata::SetReflectionSaturation(float saturation) {
    s_reflectionSaturation = saturation < 0.0f ? 0.0f : (saturation > 1.0f ? 1.0f : saturation);
}

void Liquid::Cata::SetReflectionScreenSpace(bool enable) {
    s_reflectionScreenSpace = enable;
}

void Liquid::Cata::SetReflectionPerPixel(bool enable) {
    s_reflectionPerPixel = enable;
}

void Liquid::Cata::SetReflectionPlanes(uint32_t planes) {
    s_reflectionPlanes = planes < 1 ? 1 : (planes > 2 ? 2 : planes);
}

void Liquid::Cata::SetReflectionDebug(bool enable) {
    s_reflectionDebug = enable;
}

void Liquid::Cata::SetShorelineSoftness(float softness) {
    s_shorelineSoftness = softness < 0.0f ? 0.0f : (softness > 4.0f ? 4.0f : softness);
}

void Liquid::Cata::SetReflectionStrength(float strength) {
    s_reflectionStrength = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
}

void Liquid::Cata::SetReflectionDownscale(uint32_t downscale) {
    s_reflectionDownscale = downscale > 4 ? 4 : downscale;
}

// Always: the liquid geometry is built once and kept, so coordinates that followed waterDetail stayed those of the
// setting a chunk was first drawn with (the 4.3.4 magma, switched on later, turned the map's coordinates and
// showed dark blotches). The 3.3.5 magma takes world coordinates as well as the other 3.3.5 liquids do.
bool Liquid::Cata::MagmaUsesWorldCoords() {
    return true;
}

uint32_t Liquid::Cata::GetWaterDetail() {
    return s_detail;
}

void Liquid::Cata::BeginFrame() {
    // reflectionDebug 1: the reflected level and what decides it, twice a second and whenever it jumps.
    if (s_reflectionDebug) {
        static uint32_t s_lastDebugMs;
        static float s_lastDebugPlane;
        uint32_t nowMs = OsGetAsyncTimeMs();

        if (nowMs - s_lastDebugMs > 500 || fabsf(s_planeHeight - s_lastDebugPlane) > 1.0f) {
            std::fprintf(stderr, "Reflection: level %.2f (next %.2f%s), second level %.2f%s, real %s, fade %.2f, blend per pixel %s\n", s_planeHeight, s_nextPlaneHeight, s_nextPlaneValid ? "" : " none", s_plane2Height, s_plane2Valid ? (s_real2Ready ? " drawn" : "") : " none", s_realReady ? "drawn" : "not drawn", s_reflectionBlend, s_lastPassBlended ? "on" : "off");
            s_lastDebugMs = nowMs;
            s_lastDebugPlane = s_planeHeight;
        }
    }

    // Hysteresis: two waters at different levels about as near (a stream running into a pond) otherwise took turns
    // at every step, and the whole reflection slid between the two levels. Other water takes over when its middle
    // is under half as far away as the nearest water at the level reflected now.
    if (s_nextPlaneValid && s_nextSameValid && s_nextPlaneDistance > 0.25f * s_nextSameDistance) {
        s_nextPlaneHeight = s_nextSameHeight;
    }

    s_nextSameValid = false;
    s_nextSameDistance = 0.0f;
    s_planeSeen = s_nextPlaneValid;

    if (s_nextPlaneValid) {
        // A small change of height (the next chunk of a river, the water level settling) eases over; a jump to
        // other water starts there.
        // The ease is slow (about two seconds to settle, whatever the frame rate), so walking along a sloping river
        // or turning the camera does not shift the whole reflection with every step from one chunk's height to the
        // next.
        static uint32_t s_lastEaseMs;
        uint32_t nowMs = OsGetAsyncTimeMs();
        float seconds = s_lastEaseMs ? static_cast<float>(nowMs - s_lastEaseMs) * 0.001f : 0.0f;
        s_lastEaseMs = nowMs;

        // The nearest water now at the second level: the two swap at once (both are drawn, so nothing moves),
        // rather than the first easing across to it and sliding the whole reflection.
        // The nearest water is at the second level: both levels are drawn and each pixel takes its own, so the
        // levels stay as they are (swapping them changed what each reflection was culled for, and the water there
        // changed at once).
        // CHANGED: unless the camera has gone below the first level: nothing is reflected in water seen from
        // under it, and the second level is only drawn alongside the first, so the water at the second level lost
        // its reflection at once (the colour change moving the camera down to a stream below a pond). The two
        // levels then trade places; both heights are kept, so the water at each level keeps its own reflection.
        bool firstAboveCamera = s_lastCameraZValid && s_planeValid && s_planeHeight - s_lastCameraZ > -0.05f;
        bool secondBelowCamera = s_lastCameraZValid && s_plane2Height - s_lastCameraZ <= -0.05f;

        if (s_reflectionPerPixel && s_reflectionPlanes >= 2 && s_plane2Valid && fabsf(s_nextPlaneHeight - s_plane2Height) < 1.0f && fabsf(s_nextPlaneHeight - s_planeHeight) > 1.0f && firstAboveCamera && secondBelowCamera) {
            float first = s_planeHeight;
            s_planeHeight = s_plane2Height;
            s_plane2Height = first;
            s_plane2Hold = OsGetAsyncTimeMs();
        } else if (s_reflectionPerPixel && s_reflectionPlanes >= 2 && s_plane2Valid && fabsf(s_nextPlaneHeight - s_plane2Height) < 1.0f && fabsf(s_nextPlaneHeight - s_planeHeight) > 1.0f) {
            s_next2Valid = true;
            s_next2Height = s_plane2Height;
        } else if (!s_planeValid || fabsf(s_nextPlaneHeight - s_planeHeight) > 1.0f) {
            // CHANGED: other water (more than a yard off) is taken at once, not eased across to: an ease cut short
            // (the nearest water at the second level again) left the level between the two, at no water at all.
            // Only once it has stayed the nearest for a little while, though (unless there is no level, or the
            // camera is below it): rising or turning brought other chunks in and out of view, and the level
            // flipped between two waters a few yards apart, changing the look of both each time.
            if (fabsf(s_pendingHeight - s_nextPlaneHeight) > 1.0f) {
                s_pendingHeight = s_nextPlaneHeight;
                s_pendingSince = nowMs;
            }

            if (!s_planeValid || firstAboveCamera || nowMs - s_pendingSince >= 500) {
                s_planeHeight = s_nextPlaneHeight;
            }
        } else {
            s_pendingHeight = s_planeHeight;
            float ease = 1.0f - expf(-(seconds > 0.25f ? 0.25f : seconds) / 0.6f);
            s_planeHeight += (s_nextPlaneHeight - s_planeHeight) * ease;
        }

        s_planeValid = true;
        s_planeHold = PLANE_HOLD_FRAMES;
    } else if (s_planeHold) {
        s_planeHold--;
    } else {
        s_planeValid = false;
    }

    // CHANGED: the second level stays with its water while that is still in view, unless other water is under half
    // as far away (as the first level does): in a valley of pools and waterfalls the nearest one changed with
    // every few steps, and each change made two pools change their look at once.
    if (s_next2Valid && s_nextSame2Valid && s_next2Distance > 0.25f * s_nextSame2Distance) {
        s_next2Height = s_nextSame2Height;
    }

    s_nextSame2Valid = false;
    s_nextSame2Distance = 0.0f;

    // The second level: the nearest water more than a yard from the first, eased the same way.
    if (s_next2Valid && s_planeValid && fabsf(s_next2Height - s_planeHeight) > 1.0f) {
        if (!s_plane2Valid || fabsf(s_next2Height - s_plane2Height) > 1.0f) {
            s_plane2Height = s_next2Height;
        } else {
            s_plane2Height += (s_next2Height - s_plane2Height) * 0.1f;
        }

        s_plane2Valid = true;
        s_plane2Hold = OsGetAsyncTimeMs();
    } else if (s_plane2Valid && OsGetAsyncTimeMs() - s_plane2Hold < 20000) {
        // Kept for 20 seconds after the other water was last nearby, so it does not drop in and out (each time
        // the water there changed reflection at once).
    } else {
        s_plane2Valid = false;
    }

    if (s_plane2Valid && (!s_planeValid || fabsf(s_plane2Height - s_planeHeight) < 0.5f)) {
        s_plane2Valid = false;
    }

    s_next2Valid = false;
    s_next2Distance = 0.0f;
    s_nextPlaneValid = false;
    s_nextPlaneDistance = 0.0f;
    s_realReady = false;
    s_real2Ready = false;

    uint32_t wanted = s_requestedDetail >= 2 && !s_failed ? s_requestedDetail : 0;

    if (wanted && !Supported()) {
        wanted = 0;
    }

    if (wanted == s_detail) {
        UpdateRippleResources();
        return;
    }

    if (!wanted) {
        Destroy();
        DepthCache::Release();
        s_detail = 0;
        s_rippleDetail = 0;
        return;
    }

    if (!s_detail) {
        if (!CreateShaders()) {
            Destroy();
            s_failed = true;
            LogOnce(1, "off, its shaders are missing from Shaders\\Cata (is patch-C.MPQ in Data?)");
            return;
        }

        DepthCache::AddRef();
    }

    s_detail = wanted;
    LogOnce(2, "on (waterDetail %u)", wanted);
    UpdateRippleResources();
}

void Liquid::Cata::SetView(float zNear, float zFar, float viewportFarClip) {
    if (zNear > 0.0f && zFar > zNear && viewportFarClip > 0.0f) {
        s_zNear = zNear;
        s_zFar = zFar;
        s_viewportFarClip = viewportFarClip;
    }
}

static void SaveConstants() {
    memcpy(s_savedVs, CGxDevice::s_shadowConstants[1].constants, sizeof(s_savedVs));
    memcpy(s_savedPs, CGxDevice::s_shadowConstants[0].constants, sizeof(s_savedPs));
}

static void RestoreConstants() {
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, s_savedVs, VS_REGISTERS);
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, s_savedPs, PS_REGISTERS);
}

static void DrawQuad() {
    CGxBatch batch;
    batch.m_primType = GxPrim_TriangleStrip;
    batch.m_start = 0;
    batch.m_count = 4;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 3;
    GxPrimVertexPtr(4, s_quad, sizeof(C3Vector), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
    GxPrimIndexPtr(4, s_quadIndices);
    g_theGxDevicePtr->Draw(&batch, 1);
}

// 4.3.4's Liquid blur (FourTapBlur): the frame copy shrunk into blurredBack, then blurred back and forth between
// it and blurTemp with a widening offset; the result ends in blurredBack.
static void BlurBackBuffer(CGxTex* backBuffer, CGxTex* blurred, CGxTex* temp) {
    CGxDevice* device = g_theGxDevicePtr;
    device->DeviceCopyTex(backBuffer, 0, blurred, 0);

    device->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_BlendingMode, 0);
    device->RsSet(GxRs_VertexShader, s_blurVertexShader);
    device->RsSet(GxRs_PixelShader, s_blurPixelShader);

    float texel = 1.0f / BLUR_SIZE;
    float c0[4] = { texel, texel, texel * 0.5f, texel * 0.5f };
    device->ShaderConstantsSet(GxSh_Vertex, 0, c0, 1);

    for (uint32_t pass = 0; pass < BLUR_PASSES; pass++) {
        CGxTex* target = pass & 1 ? blurred : temp;
        CGxTex* source = pass & 1 ? temp : blurred;
        GxRenderTargetSet(GxBuffers_Color, target, 0);
        GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

        float offset = pass * texel * BLUR_SPREAD;
        float c1[4] = { offset, offset, 0.0f, 0.0f };
        device->ShaderConstantsSet(GxSh_Vertex, 1, c1, 1);

        for (uint32_t tmu = 0; tmu < 4; tmu++) {
            device->RsSet(static_cast<EGxRenderState>(GxRs_Texture0 + tmu), source);
        }

        DrawQuad();
    }

    device->RsPop();
}

// 4.3.4's "fake" reflection: the sky alone, seen from the camera mirrored in the water plane (a camera-relative
// look along the mirrored view direction, world up), into a window-sized texture.
// The sky alone (stars, sun, moons, sky dome, clouds) through skyView, without depth.
static void DrawSky(const C44Matrix& skyView) {
    CGxDevice* device = g_theGxDevicePtr;
    C44Matrix view;
    GxXformView(view);

    device->XformSetView(skyView);
    device->RsPush();
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);

    DayNight::g_stars.Render();
    DayNight::g_planets.m_sun.Render();
    DayNight::g_planets.m_moon.Render();
    DayNight::g_planets.m_moon2.Render();
    DayNight::g_sky.Render();
    DayNight::g_clouds.Render();

    device->RsPop();
    device->XformSetView(view);
}

static C44Matrix MirrorZ(float height);

static void RenderSkyReflection(CGxTex* target) {
    DayNight::DNInfo* info = DayNight::GetInfo();

    GxRenderTargetSet(GxBuffers_Color, target, 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    GxSceneClear(GxBufferBit_Color, info->m_fog.m_color);

    if (!info->m_showSky) {
        return;
    }

    C44Matrix view;
    GxXformView(view);

    // CHANGED: mirrored in the water as the real reflection is (the view with the world's z turned over), rather
    // than a camera looking up along the mirrored direction: that drew the sky upside down to what the water
    // shader and the per-pixel blend read, so the sky's bands showed as lines on the water moving the wrong way
    // when the camera tilted. Around the camera, as the sky is drawn.
    view.d0 = 0.0f;
    view.d1 = 0.0f;
    view.d2 = 0.0f;
    C44Matrix mirroredView = MirrorZ(0.0f) * view;

    DrawSky(mirroredView);
}

// The first 4.3.4 water draw of a pass makes what it samples from the frame drawn so far.
static bool PreparePass() {
    if (s_passPrepared) {
        return s_passReady;
    }

    s_passPrepared = true;
    s_passReady = false;

    if (!DepthCache::Prepare() || !DepthCache::GetDepthTex()) {
        LogOnce(3, "skipped, the scene depth could not be read this frame");
        return false;
    }

    if (!EnsureTargets()) {
        LogOnce(4, "skipped, its render targets could not be created");
        return false;
    }

    CGxTex* backBuffer = GxTexOf(s_backBufferTex);
    CGxTex* reflection = GxTexOf(s_fakeReflectionTex);
    CGxTex* blurred = GxTexOf(s_blurredBackTex);
    CGxTex* temp = GxTexOf(s_blurTempTex);

    if (!backBuffer || !reflection || !blurred || !temp) {
        return false;
    }

    C2iVector origin = { 0, 0 };
    C2iVector size = { static_cast<int32_t>(s_screenWidth), static_cast<int32_t>(s_screenHeight) };

    if (!GxTexCopy(backBuffer, origin, origin, size, 0, 0)) {
        LogOnce(5, "skipped, the frame could not be copied");
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    CGxTex* savedColor = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, savedColor);

    float minX, maxX, minY, maxY, minZ, maxZ;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

    BlurBackBuffer(backBuffer, blurred, temp);

    if (!s_cameraUnderwater) {
        RenderSkyReflection(reflection);
    }

    GxRenderTargetSet(GxBuffers_Color, savedColor, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);

    s_passReady = true;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Ripples (rippleDetail 1 and 2), as 4.3.4's Liquid displacement maps do them: a height field around the camera,
// stepped with a wave equation at a fixed rate (PropagateDisplMap) and disturbed by units in water and rain drops
// (PerturbDisplMap). The water shaders read it (vertex c51-c52, pixel c29-c31, textures 4 and 8) and bend their
// normals with it.
//
// Each level is two ping-pong maps (x = height now, y = height one step ago) covering RIPPLE_SIZE yards, moved
// with the camera in whole texels. rippleDetail 1: one level stepped every 50 ms. rippleDetail 2: a second level,
// and the first steps every 25 ms. Units and rain disturb the first level; 4.3.4 disturbs the second with the
// collision meshes of moving objects (boats and the like), which is not ported, so it stays calm.

static const uint32_t RIPPLE_LEVELS = 2;
static const uint32_t RIPPLE_RES = 1024;
static const float RIPPLE_SIZE = 128.0f;
static const uint32_t RIPPLE_MAX_PERTURBATIONS = 256;
static const float s_rippleStrengthScale[RIPPLE_LEVELS] = { 1.25f, 0.25f };

struct RipplePerturbation {
    const void* owner;
    C3Vector position;
    float radius;
    float strength;
    uint32_t period;
    uint32_t frame;
};

struct RippleLevel {
    CTexture* maps[2];
    uint32_t current;
    uint32_t stepMs;
    uint32_t lastStep;
    float fraction;
    C3Vector origin;
    C3Vector mapOrigin;
    std::vector<RipplePerturbation> drops;
    std::vector<RipplePerturbation> persistent;
};

static RippleLevel s_ripples[RIPPLE_LEVELS];
static uint32_t s_rippleLevels;
static bool s_rippleClear;
static uint32_t s_rippleStartMs;
static uint32_t s_rippleFrame;
static uint32_t s_rainLastMs;
static CGxShader* s_propagateVs;
static CGxShader* s_propagatePs;
static CGxShader* s_perturbVs;
static CGxShader* s_perturbPs;
static CTexture* s_perturbTex;

static void DestroyRipples() {
    for (RippleLevel& level : s_ripples) {
        ReleaseTexture(level.maps[0]);
        ReleaseTexture(level.maps[1]);
        level.drops.clear();
        level.persistent.clear();
    }

    ReleaseShader(s_propagateVs);
    ReleaseShader(s_propagatePs);
    ReleaseShader(s_perturbVs);
    ReleaseShader(s_perturbPs);
    ReleaseTexture(s_perturbTex);
    s_rippleLevels = 0;
    s_rippleDetail = 0;
}

static bool CreateRipples(uint32_t detail) {
    CGxDevice* device = g_theGxDevicePtr;

    if (!device->Caps().m_rttFormat[GxTex_Gr1616F]) {
        LogOnce(9, "ripples off, the device cannot render to a two-channel float texture");
        return false;
    }

    device->ShaderCreate(&s_propagateVs, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "PropagateDisplMap", 1);
    device->ShaderCreate(&s_propagatePs, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "PropagateDisplMap", 1);
    device->ShaderCreate(&s_perturbVs, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "PerturbDisplMap", 1);
    device->ShaderCreate(&s_perturbPs, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "PerturbDisplMap", 1);

    if (!Valid(s_propagateVs) || !Valid(s_propagatePs) || !Valid(s_perturbVs) || !Valid(s_perturbPs)) {
        LogOnce(10, "ripples off, Shaders\\Cata\\...\\PropagateDisplMap.bls or PerturbDisplMap.bls did not load (is the new patch-C.MPQ in Data?)");
        return false;
    }

    CGxTexFlags flags;
    flags.m_filter = GxTex_Linear;
    flags.m_wrapU = 0;
    flags.m_wrapV = 0;
    CStatus status;
    s_perturbTex = TextureCreate("XTextures\\Ripples\\perturbTex.blp", flags, &status, 0);

    if (!s_perturbTex) {
        LogOnce(11, "ripples off, XTextures\\Ripples\\perturbTex.blp did not load");
        return false;
    }

    s_rippleLevels = detail >= 2 ? 2 : 1;

    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        RippleLevel& level = s_ripples[i];
        level.maps[0] = CreateTarget(GxTex_2d, RIPPLE_RES, RIPPLE_RES, "Liquid DisplMap", GxTex_Gr1616F);
        level.maps[1] = CreateTarget(GxTex_2d, RIPPLE_RES, RIPPLE_RES, "Liquid DisplMap", GxTex_Gr1616F);

        if (!level.maps[0] || !level.maps[1]) {
            LogOnce(12, "ripples off, the ripple maps could not be created");
            return false;
        }

        level.current = 0;
        level.fraction = 0.0f;
        // 4.3.4: one level steps every 50 ms; with two, the first every 25 ms and the second every 50 ms.
        level.stepMs = s_rippleLevels == 2 && i == 0 ? 25 : 50;
    }

    s_rippleClear = true;
    s_rippleStartMs = OsGetAsyncTimeMs();
    s_rainLastMs = s_rippleStartMs;
    LogOnce(13, "ripples on (rippleDetail %u)", detail);
    return true;
}

// Called by BeginFrame once the water detail is settled.
static void UpdateRippleResources() {
    uint32_t wanted = s_detail >= 2 && !s_rippleFailed ? s_requestedRipple : 0;

    if (wanted == s_rippleDetail) {
        return;
    }

    DestroyRipples();
    s_rippleDetail = 0;

    if (wanted && CreateRipples(wanted)) {
        s_rippleDetail = wanted;
    } else if (wanted) {
        DestroyRipples();
        s_rippleFailed = true;
    }
}

bool Liquid::Cata::RipplesActive() {
    return s_rippleDetail != 0;
}

void Liquid::Cata::SetUnitRipple(const void* owner, const C3Vector& position, float radius, float strength, bool force) {
    if (!s_rippleDetail) {
        return;
    }

    std::vector<RipplePerturbation>& list = s_ripples[0].persistent;
    RipplePerturbation* entry = nullptr;

    for (RipplePerturbation& perturbation : list) {
        if (perturbation.owner == owner) {
            entry = &perturbation;
            break;
        }
    }

    if (!entry) {
        // 4.3.4 only starts one for a unit that disturbs the water, and keeps the active player's past the limit.
        if (strength <= 0.0f || (!force && list.size() >= RIPPLE_MAX_PERTURBATIONS)) {
            return;
        }

        list.push_back(RipplePerturbation());
        entry = &list.back();
        entry->owner = owner;
    }

    entry->position = position;
    entry->radius = radius;
    entry->strength = strength;
    entry->period = 800;
    entry->frame = s_rippleFrame;
}

static void AddDrop(uint32_t levelIndex, const C3Vector& position, float radius, float strength) {
    RippleLevel& level = s_ripples[levelIndex < s_rippleLevels ? levelIndex : s_rippleLevels - 1];

    if (level.drops.size() >= RIPPLE_MAX_PERTURBATIONS) {
        return;
    }

    RipplePerturbation drop;
    drop.owner = nullptr;
    drop.position = position;
    drop.radius = radius;
    drop.strength = strength;
    drop.period = 0;
    drop.frame = s_rippleFrame;
    level.drops.push_back(drop);
}

// 4.3.4's rain on water: every 33 ms of rain heavier than 0.2, twenty drops within 50 yards of the camera.
static void AddRainDrops(const C3Vector& camera, uint32_t now) {
    uint32_t elapsed = now - s_rainLastMs;

    if (elapsed <= 32) {
        return;
    }

    uint32_t ticks = elapsed / 33;

    if (ticks < 4) {
        s_rainLastMs += ticks * 33;
    } else {
        ticks = 4;
        s_rainLastMs = now;
    }

    Weather* weather = World::s_weather;

    if (!weather || weather->GetType() != 1 || !(weather->m_intensity > 0.2f)) {
        return;
    }

    for (uint32_t i = 0; i < ticks * 20; i++) {
        float strength = static_cast<float>(rand() % 200) * 0.001f + 0.2f;
        C3Vector position;
        position.x = (static_cast<float>(rand() % 1000) * 0.002f - 1.0f) * 50.0f + camera.x;
        position.y = (static_cast<float>(rand() % 1000) * 0.002f - 1.0f) * 50.0f + camera.y;
        position.z = camera.z;
        AddDrop(0, position, 0.3f, -strength);
    }
}

struct PerturbVertex {
    C3Vector position;
    C2Vector texCoord;
};

// Splats perturbTex, scaled by each perturbation's strength, into the level's current map (additively).
static void DrawPerturbations(uint32_t levelIndex, const std::vector<RipplePerturbation>& list, uint32_t now, bool persistent) {
    RippleLevel& level = s_ripples[levelIndex];
    CGxTex* perturbTex = GxTexOf(s_perturbTex);

    if (list.empty() || !perturbTex) {
        return;
    }

    static std::vector<PerturbVertex> vertices;
    static std::vector<uint16_t> indices;
    vertices.clear();
    indices.clear();

    float res = static_cast<float>(RIPPLE_RES);
    float texel = 1.0f / res;
    // Direct3D 9 only: Direct3D 11 samples at texel centres.
    bool halfTexel = g_theGxDevicePtr->DeviceApi() == GxApi_D3d9 || g_theGxDevicePtr->DeviceApi() == GxApi_D3d9Ex;
    uint32_t elapsed = now - s_rippleStartMs;

    for (const RipplePerturbation& perturbation : list) {
        // A unit's perturbation counts only in a frame its unit updated it.
        if (persistent && s_rippleFrame - perturbation.frame > 1) {
            continue;
        }

        if (fabsf(perturbation.strength) < 0.001f) {
            continue;
        }

        float radiusX = floorf(perturbation.radius / RIPPLE_SIZE * res);
        radiusX = (radiusX > 0.5f ? radiusX : 0.5f) * texel;
        float radiusY = radiusX;
        float u = floorf((perturbation.position.x - level.mapOrigin.x) / RIPPLE_SIZE * res) * texel;
        float v = floorf((perturbation.position.y - level.mapOrigin.y) / RIPPLE_SIZE * res) * texel;

        if (u + radiusX < 0.0f || v + radiusY < 0.0f || u - radiusX > 1.0f || v - radiusY > 1.0f) {
            continue;
        }

        if (halfTexel) {
            u -= texel * 0.5f;
            v -= texel * 0.5f;
        }

        float strength = perturbation.strength * s_rippleStrengthScale[levelIndex];
        strength = strength < -1.0f ? -1.0f : (strength > 1.0f ? 1.0f : strength);

        if (perturbation.period) {
            strength *= sinf(static_cast<float>(elapsed % perturbation.period) * 6.2831855f / static_cast<float>(perturbation.period));
        }

        float x = (u - 0.5f) * 2.0f;
        float y = (0.5f - v) * 2.0f;
        float dx = radiusX * 2.0f;
        float dy = radiusY * 2.0f;
        uint16_t base = static_cast<uint16_t>(vertices.size());

        vertices.push_back({ C3Vector(x - dx, y + dy, strength), C2Vector(0.0f, 0.0f) });
        vertices.push_back({ C3Vector(x + dx, y + dy, strength), C2Vector(1.0f, 0.0f) });
        vertices.push_back({ C3Vector(x - dx, y - dy, strength), C2Vector(0.0f, 1.0f) });
        vertices.push_back({ C3Vector(x + dx, y - dy, strength), C2Vector(1.0f, 1.0f) });

        const uint16_t quad[6] = { 0, 2, 1, 1, 2, 3 };

        for (uint16_t index : quad) {
            indices.push_back(static_cast<uint16_t>(base + index));
        }

        if (vertices.size() >= 0xFFF0) {
            break;
        }
    }

    if (vertices.empty()) {
        return;
    }

    CGxDevice* device = g_theGxDevicePtr;
    GxRsSet(GxRs_BlendingMode, GxBlend_Add);
    device->RsSet(GxRs_VertexShader, s_perturbVs);
    device->RsSet(GxRs_PixelShader, s_perturbPs);
    device->RsSet(GxRs_Texture0, perturbTex);

    float c0[4] = { texel, texel, texel * 0.5f, texel * 0.5f };
    device->ShaderConstantsSet(GxSh_Vertex, 0, c0, 1);

    GxRenderTargetSet(GxBuffers_Color, GxTexOf(level.maps[level.current]), 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    CGxBatch batch;
    batch.m_primType = GxPrim_Triangles;
    batch.m_start = 0;
    batch.m_count = static_cast<uint32_t>(indices.size());
    batch.m_minIndex = 0;
    batch.m_maxIndex = static_cast<uint16_t>(vertices.size() - 1);
    GxPrimVertexPtr(static_cast<uint32_t>(vertices.size()), &vertices[0].position, sizeof(PerturbVertex), nullptr, 0, nullptr, 0, &vertices[0].texCoord, sizeof(PerturbVertex), nullptr, 0);
    GxPrimIndexPtr(static_cast<uint32_t>(indices.size()), indices.data());
    device->Draw(&batch, 1);
}

// One wave step: the other map gets 0.97 * (half the four neighbours - the height a step ago), faded towards the
// edges, read through the camera's movement since the last step.
static void Propagate(uint32_t levelIndex) {
    RippleLevel& level = s_ripples[levelIndex];
    uint32_t source = level.current;
    uint32_t target = source ^ 1;

    float offsetX = level.origin.x - level.mapOrigin.x;
    float offsetY = level.origin.y - level.mapOrigin.y;

    // A jump (a teleport) starts the maps over rather than sliding them.
    if (offsetX * offsetX + offsetY * offsetY >= 10000.0f) {
        offsetX = 0.0f;
        offsetY = 0.0f;
    }

    level.mapOrigin = level.origin;

    float texel = 1.0f / RIPPLE_RES;
    float c0[4] = { texel, texel, texel * 0.5f, texel * 0.5f };
    float c1[4] = { offsetX / RIPPLE_SIZE, offsetY / RIPPLE_SIZE, 0.0f, 0.0f };

    CGxDevice* device = g_theGxDevicePtr;
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    device->RsSet(GxRs_VertexShader, s_propagateVs);
    device->RsSet(GxRs_PixelShader, s_propagatePs);
    device->RsSet(GxRs_Texture0, GxTexOf(level.maps[source]));
    device->ShaderConstantsSet(GxSh_Vertex, 0, c0, 1);
    device->ShaderConstantsSet(GxSh_Vertex, 1, c1, 1);

    GxRenderTargetSet(GxBuffers_Color, GxTexOf(level.maps[target]), 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    DrawQuad();

    level.current = target;
}

void Liquid::Cata::UpdateRipples(const C3Vector& camera) {
    if (!s_rippleDetail) {
        return;
    }

    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        if (!GxTexOf(s_ripples[i].maps[0]) || !GxTexOf(s_ripples[i].maps[1])) {
            return;
        }
    }

    uint32_t now = OsGetAsyncTimeMs();
    float texelsPerYard = RIPPLE_RES / RIPPLE_SIZE;

    // The maps follow the camera in whole texels.
    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        RippleLevel& level = s_ripples[i];
        level.origin.x = floorf((camera.x - RIPPLE_SIZE * 0.5f) * texelsPerYard) / texelsPerYard;
        level.origin.y = floorf((camera.y - RIPPLE_SIZE * 0.5f) * texelsPerYard) / texelsPerYard;
        level.origin.z = camera.z;
    }

    AddRainDrops(camera, now);

    CGxDevice* device = g_theGxDevicePtr;
    CGxTex* savedColor = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, savedColor);
    float minX, maxX, minY, maxY, minZ, maxZ;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);
    float savedVs[8];
    memcpy(savedVs, CGxDevice::s_shadowConstants[1].constants, sizeof(savedVs));

    device->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_AlphaRef, 0);

    if (s_rippleClear) {
        CImVector black = { 0, 0, 0, 0 };

        for (uint32_t i = 0; i < s_rippleLevels; i++) {
            RippleLevel& level = s_ripples[i];

            for (CTexture* map : level.maps) {
                GxRenderTargetSet(GxBuffers_Color, GxTexOf(map), 0);
                GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
                GxSceneClear(GxBufferBit_Color, black);
            }

            level.current = 0;
            level.mapOrigin = level.origin;
            level.lastStep = now - level.stepMs;
        }

        s_rippleClear = false;
    }

    // Drops land at once; units' perturbations go in with every step.
    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        DrawPerturbations(i, s_ripples[i].drops, now, false);
        s_ripples[i].drops.clear();
    }

    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        RippleLevel& level = s_ripples[i];
        uint32_t elapsed = now - level.lastStep;
        uint32_t steps = elapsed / level.stepMs;
        level.fraction = static_cast<float>(elapsed % level.stepMs) / static_cast<float>(level.stepMs);

        if (!steps) {
            continue;
        }

        // At most two steps a frame; a long frame drops the rest.
        if (steps >= 2) {
            steps = 2;
            level.lastStep = now;
        } else {
            level.lastStep += level.stepMs;
        }

        for (uint32_t step = 0; step < steps; step++) {
            DrawPerturbations(i, level.persistent, now, true);
            Propagate(i);
        }
    }

    device->RsPop();
    GxRenderTargetSet(GxBuffers_Color, savedColor, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);
    device->ShaderConstantsSet(GxSh_Vertex, 0, savedVs, 2);

    // Forget units that have not updated theirs for a while (gone, or out of the water).
    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        std::vector<RipplePerturbation>& list = s_ripples[i].persistent;

        for (size_t j = 0; j < list.size();) {
            if (s_rippleFrame - list[j].frame > 300) {
                list[j] = list.back();
                list.pop_back();
            } else {
                j++;
            }
        }
    }

    s_rippleFrame++;
}

// The water shaders' ripple constants: vertex c51-c52 (map origin, relative to the camera when the world is drawn
// camera-relative, and 1 / size), pixel c29-c30 (texel sizes) and c31 (how far each level is into its next step).
static void SetRippleConstants(const C3Vector& cameraPos) {
    if (!s_rippleDetail) {
        return;
    }

    bool relative = Liquid::UseCameraRelativeCoords();
    float texel = 1.0f / RIPPLE_RES;

    for (uint32_t i = 0; i < s_rippleLevels; i++) {
        const RippleLevel& level = s_ripples[i];
        float x = level.mapOrigin.x - (relative ? cameraPos.x : 0.0f);
        float y = level.mapOrigin.y - (relative ? cameraPos.y : 0.0f);
        SetVs(51 + i, x, y, 1.0f / RIPPLE_SIZE, 1.0f / RIPPLE_SIZE);
        SetPs(29 + i, texel, texel, texel * 0.5f, texel * 0.5f);
    }

    SetPs(31, s_ripples[0].fraction, s_rippleLevels > 1 ? s_ripples[1].fraction : 0.0f, 0.0f, 0.0f);
}

static void BindRippleMaps() {
    if (!s_rippleDetail) {
        return;
    }

    CGxDevice* device = g_theGxDevicePtr;
    device->RsSet(GxRs_Texture4, GxTexOf(s_ripples[0].maps[s_ripples[0].current]));

    if (s_rippleLevels > 1) {
        device->RsSet(GxRs_Texture8, GxTexOf(s_ripples[1].maps[s_ripples[1].current]));
    }
}

// The view mirrored in the plane z = height (camera-relative): z becomes 2 * height - z.
static C44Matrix MirrorZ(float height) {
    C44Matrix mirror;
    mirror.c2 = -1.0f;
    mirror.d2 = 2.0f * height;
    return mirror;
}

static C44Matrix s_reflectionSavedView;
static CGxTex* s_reflectionSavedColor;
static float s_reflectionSavedViewport[6];
static C4Plane s_reflectionClip;
static float s_reflectionPlaneZ;
static C44Matrix s_reflectionSavedProjection;
static bool s_reflectionModels = true;

bool Liquid::Cata::BeginReflection(const C3Vector& worldViewOrigin, uint32_t& mode, uint32_t index) {
    mode = s_reflectionMode;

    if (s_inReflection) {
        return false;
    }

    // The second level only alongside the first.
    if (index) {
        float height2 = s_plane2Height - worldViewOrigin.z;

        if (!s_reflectionPerPixel || index >= s_reflectionPlanes || !s_realReady || !s_plane2Valid || height2 > -0.05f || !GxTexOf(s_real2ReflectionTex)) {
            return false;
        }
    }

    s_reflectionIndex = index;

    // Camera-relative height of the water; nothing to see in it from below or right at it.
    float height = (index ? s_plane2Height : s_planeHeight) - worldViewOrigin.z;
    bool possible = s_detail >= 3 && s_reflectionMode >= 2 && s_planeValid && !s_cameraUnderwater && height <= -0.05f;
    bool wanted = possible && s_planeSeen;

    if (index) {
        wanted = true;
    }

    // Fade towards on while the water is in view, towards off otherwise (once a frame, with the first level).
    uint32_t now = index ? s_lastBlendMs : OsGetAsyncTimeMs();
    float elapsed = s_lastBlendMs ? (now - s_lastBlendMs) * 0.001f : 0.0f;
    s_lastBlendMs = now;
    elapsed = elapsed > 0.25f ? 0.25f : elapsed;
    s_reflectionBlend += (wanted ? 1.0f : -1.0f) * elapsed * REFLECTION_FADE_PER_SECOND;
    s_reflectionBlend = s_reflectionBlend < 0.0f ? 0.0f : (s_reflectionBlend > 1.0f ? 1.0f : s_reflectionBlend);

    if (!possible || s_reflectionBlend <= 0.0f || !EnsureTargets()) {
        return false;
    }

    CGxTex* target = GxTexOf(index ? s_real2ReflectionTex : s_realReflectionTex);

    if (!target) {
        LogOnce(21, "real reflections skipped, the reflection texture could not be created");
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    SaveConstants();
    s_reflectionSavedColor = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, s_reflectionSavedColor);
    float* vp = s_reflectionSavedViewport;
    GxXformViewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
    GxXformView(s_reflectionSavedView);

    device->RsPush();
    // The colour target first, so the depth texture is never paired with the (larger) back buffer.
    GxRenderTargetSet(GxBuffers_Color, target, 0);
    CGxTex* depth = GxTexOf(index ? s_real2ReflectionDepth : s_realReflectionDepth);

    if (depth && depth->m_width == target->m_width && depth->m_height == target->m_height) {
        GxRenderTargetSet(GxBuffers_Depth, depth, 0);
    } else {
        depth = nullptr;
    }

    s_reflectionDepthBound = depth != nullptr;
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, vp[4], vp[5]);

    DayNight::DNInfo* info = DayNight::GetInfo();
    GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, info->m_fog.m_color);

    // The sky, mirrored through the camera (it is drawn around it).
    if (info->m_showSky) {
        DrawSky(MirrorZ(0.0f) * s_reflectionSavedView);
    }

    // The world, mirrored in the water: the clip plane keeps what lies above the water, and the mirror turns the
    // winding around.
    C44Matrix mirrored = MirrorZ(height) * s_reflectionSavedView;
    device->XformSetView(mirrored);

    C44Matrix inverse = (mirrored * device->m_projNative).Inverse();
    C4Plane clip;
    clip.n.x = inverse.a2 - height * inverse.a3;
    clip.n.y = inverse.b2 - height * inverse.b3;
    clip.n.z = inverse.c2 - height * inverse.c3;
    clip.d = inverse.d2 - height * inverse.d3;
    s_reflectionClip = clip;
    s_reflectionPlaneZ = height;
    device->ClipPlaneSet(0, clip);
    GxRsSet(GxRs_ClipPlaneMask, 1);
    device->DeviceSetCullFlip(1);

    s_inReflection = true;
    return true;
}

void Liquid::Cata::SetReflectionClipForFixedFunction(bool fixedFunction) {
    if (!s_inReflection) {
        return;
    }

    if (fixedFunction) {
        // Without shaders Direct3D takes the plane in world space: keep z >= the water's (camera-relative) height.
        C4Plane plane;
        plane.n.x = 0.0f;
        plane.n.y = 0.0f;
        plane.n.z = 1.0f;
        plane.d = -s_reflectionPlaneZ;
        g_theGxDevicePtr->ClipPlaneSet(0, plane);
    } else {
        g_theGxDevicePtr->ClipPlaneSet(0, s_reflectionClip);
    }
}

void Liquid::Cata::SetReflectionModels(bool enable) {
    s_reflectionModels = enable;
}

// Models are animated with the view already in their matrices, so they are mirrored through the projection
// instead: the view-space mirror (the view undone, the mirror, the view again) goes in front of it. The whole
// transform then matches the mirrored world's, so the same clip plane holds.
bool Liquid::Cata::BeginModelReflection() {
    if (!s_inReflection || !s_reflectionModels || s_reflectionMode < 3) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    s_reflectionSavedProjection = device->m_projNative;
    C44Matrix viewMirror = s_reflectionSavedView.Inverse() * MirrorZ(s_reflectionPlaneZ) * s_reflectionSavedView;
    device->m_projNative = viewMirror * s_reflectionSavedProjection;
    device->XformSetView(s_reflectionSavedView);
    CShaderEffect::UpdateProjMatrix();
    CM2Scene::s_keepClipPlane = true;
    CM2Scene::s_reflectionCullDistance = s_reflectionDistance;
    CM2Scene::s_reflectionCullPlaneZ = s_reflectionPlaneZ;
    CM2Scene::s_reflectionCullMinSize = s_reflectionModelSize;
    return true;
}

void Liquid::Cata::EndModelReflection() {
    CGxDevice* device = g_theGxDevicePtr;
    CM2Scene::s_keepClipPlane = false;
    device->m_projNative = s_reflectionSavedProjection;
    device->XformSetView(MirrorZ(s_reflectionPlaneZ) * s_reflectionSavedView);
    CShaderEffect::UpdateProjMatrix();
    device->ClipPlaneSet(0, s_reflectionClip);
    GxRsSet(GxRs_ClipPlaneMask, 1);
}

bool Liquid::Cata::ReflectionNeedsWideCulling() {
    return s_detail >= 3 && s_reflectionMode >= 2 && s_planeValid && !s_cameraUnderwater && s_reflectionCullHeight > 0.0f;
}

float Liquid::Cata::ReflectionCullHeight() {
    return s_reflectionCullHeight;
}

void Liquid::Cata::SetReflectionCullHeight(float screens) {
    s_reflectionCullHeight = screens < 0.0f ? 0.0f : (screens > 4.0f ? 4.0f : screens);
}

void Liquid::Cata::EndReflection() {
    if (!s_inReflection) {
        return;
    }

    CGxDevice* device = g_theGxDevicePtr;
    device->DeviceSetCullFlip(0);
    GxRsSet(GxRs_ClipPlaneMask, 0);
    device->XformSetView(s_reflectionSavedView);

    CGxTex* depth = s_reflectionDepthBound ? GxTexOf(s_reflectionIndex ? s_real2ReflectionDepth : s_realReflectionDepth) : nullptr;
    CGxTex* reflection = GxTexOf(s_reflectionIndex ? s_real2ReflectionTex : s_realReflectionTex);
    DayNight::DNInfo* info = DayNight::GetInfo();

    // The depth texture stops being the depth buffer before it is read.
    if (depth) {
        GxRenderTargetSet(GxBuffers_Depth, nullptr, 0);

        // What lies far off fades into the fog from a third of reflectionDistance on, so distant hills,
        // buildings and trees do not stand out in the water or appear in it abruptly.
        // CHANGED: not into the fog colour when the per-pixel blend runs (it fades into the sky reflection itself):
        // at sunset the fog is orange, and the water fogs what it shows again, so the reflections turned orange.
        if (s_reflectionDistance > 0.0f && !s_reflectionPerPixel) {
            C4Vector view = ViewSettings::Params(s_zNear, s_zFar, s_viewportFarClip);
            PostEffects::FadeByDepth(reflection, depth, view, s_viewportFarClip * 0.99999f, s_reflectionDistance * 0.35f, s_reflectionDistance, info->m_fog.m_color);
        }
    }

    // reflectionStrength below 1 fades the reflected world towards the fog colour, so the water mirrors it less;
    // so does the fade in and out.
    float shown = s_reflectionStrength * s_reflectionBlend;

    // A slight blur, as 4.3.4's blurredReflection: thin edges stop shimmering as the camera moves.
    PostEffects::Soften(reflection, GxTexOf(s_realReflectionTemp), 2.5f);

    // CHANGED: towards a grey as bright as the fog, not the fog colour itself: at sunset that is orange, and the
    // water fogs what it shows again, so the reflections turned orange.
    if (shown < 1.0f) {
        CImVector grey = info->m_fog.m_color;
        uint8_t luma = static_cast<uint8_t>(0.299f * grey.r + 0.587f * grey.g + 0.114f * grey.b);
        grey.r = grey.g = grey.b = luma;
        PostEffects::Tint(reflection, grey, 1.0f - shown);
    }
    device->RsPop();

    float* vp = s_reflectionSavedViewport;
    GxRenderTargetSet(GxBuffers_Color, s_reflectionSavedColor, 0);
    GxXformSetViewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
    RestoreConstants();

    s_inReflection = false;

    if (s_reflectionIndex) {
        s_real2Ready = true;
        LogOnce(24, "drawing a second real reflection for water at another level");
    } else {
        s_realReady = true;
    }

    LogOnce(22, "drawing real reflections");
}

void Liquid::Cata::BeginLiquidPass(bool cameraUnderwater) {
    s_cameraUnderwater = cameraUnderwater;
    s_inPass = s_detail >= 2;
    s_passPrepared = false;
    s_passReady = false;

    for (uint32_t i = 0; i < REFLECTION_MIXES; i++) {
        s_reflectionMixTried[i] = false;
        s_reflectionMixReady[i] = false;
    }

    s_reflectionBlendReady = false;
    s_reflectionMaskDrawing = false;

    if (s_inPass) {
        SaveConstants();
    }
}

void Liquid::Cata::EndLiquidPass() {
    if (s_inPass && s_passPrepared) {
        RestoreConstants();
    }

    s_inPass = false;
    s_passPrepared = false;
    s_passReady = false;
    s_lastPassBlended = s_reflectionBlendReady;
    s_reflectionBlendReady = false;
}

bool Liquid::Cata::BeginReflectionMask(const C3Vector& cameraPos) {
    s_reflectionBlendReady = false;
    s_reflectionMaskDrawing = false;

    if (!s_reflectionPerPixel || !s_inPass || s_cameraUnderwater || !s_realReady || !s_planeValid || !GxTexOf(s_realReflectionTex)) {
        return false;
    }

    if (!PreparePass()) {
        return false;
    }

    CGxTex* depth = DepthCache::GetDepthTex();
    CGxShader* vertexShader = nullptr;
    CGxShader* pixelShader = nullptr;

    if (!depth || !PostEffects::WaterMaskShaders(vertexShader, pixelShader)) {
        return false;
    }

    if (!s_reflectionMaskTex) {
        s_reflectionMaskTex = CreateTarget(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, "LiquidProc Reflection Mask");
    }

    if (!s_reflectionBlendTex) {
        s_reflectionBlendTex = CreateTarget(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, "LiquidProc Reflection Blend");
    }

    CGxTex* mask = GxTexOf(s_reflectionMaskTex);

    if (!mask || !GxTexOf(s_reflectionBlendTex)) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    s_maskSavedColor = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, s_maskSavedColor);
    GxXformViewport(s_maskSavedViewport[0], s_maskSavedViewport[1], s_maskSavedViewport[2], s_maskSavedViewport[3], s_maskSavedViewport[4], s_maskSavedViewport[5]);
    memcpy(s_maskSavedVs, CGxDevice::s_shadowConstants[1].constants, sizeof(s_maskSavedVs));
    memcpy(s_maskSavedPs, CGxDevice::s_shadowConstants[0].constants, sizeof(s_maskSavedPs));

    device->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_BlendingMode, 0);

    GxRenderTargetSet(GxBuffers_Color, mask, 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    // No sky, no shift (green and blue 0.5).
    CImVector clear;
    clear.r = 0;
    clear.g = 0x80;
    clear.b = 0x80;
    clear.a = 0xFF;
    GxSceneClear(GxBufferBit_Color, clear);

    device->RsSet(GxRs_VertexShader, vertexShader);
    device->RsSet(GxRs_PixelShader, pixelShader);
    device->RsSet(GxRs_Texture0, depth);

    s_maskCameraZ = cameraPos.z;
    float plane = s_planeHeight - (Liquid::UseCameraRelativeCoords() ? cameraPos.z : 0.0f);
    C4Vector view = ViewSettings::Params(s_zNear, s_zFar, s_viewportFarClip);
    float c[8] = { view.x, view.y, view.z, view.w, plane, REFLECTION_BLEND_NEAR, 1.0f / (REFLECTION_BLEND_FAR - REFLECTION_BLEND_NEAR), REFLECTION_MASK_RANGE };
    device->ShaderConstantsSet(GxSh_Pixel, 0, c, 2);

    s_reflectionMaskDrawing = true;
    return true;
}

void Liquid::Cata::DrawReflectionMask(IGeomFactory* geomFactory, const C3Vector& cameraPos, const C44Matrix* transform, CMaterialSettings* settings) {
    if (!s_reflectionMaskDrawing) {
        return;
    }

    // Only the water RenderWater draws the 4.3.4 way.
    if (!settings->m_textures[1].Count() || !settings->m_textures[2].Count() || !settings->m_textures[3].Count()) {
        return;
    }

    C44Matrix world;
    bool unchanged = geomFactory->SetTransform(const_cast<C44Matrix*>(transform));

    if (!unchanged) {
        world = *transform;
    }

    if (Liquid::UseCameraRelativeCoords()) {
        world.d0 -= cameraPos.x;
        world.d1 -= cameraPos.y;
        world.d2 -= cameraPos.z;
    }

    C44Matrix view;
    GxXformView(view);
    C44Matrix projection = g_theGxDevicePtr->m_projNative;

    if (g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl) {
        projection.c0 *= -1.0f;
        projection.c1 *= -1.0f;
        projection.c2 *= -1.0f;
        projection.c3 *= -1.0f;
    }

    C44Matrix rows = world * view * projection;
    float c[20];
    memcpy(c, &rows, sizeof(float) * 16);
    c[16] = world.a2;
    c[17] = world.b2;
    c[18] = world.c2;
    c[19] = world.d2;
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, c, 5);

    CGxBuf* vertexBuf = nullptr;
    CGxBuf* indexBuf = nullptr;
    CGxBatch batch;
    batch.m_primType = GxPrims_Last;
    batch.m_start = 0;
    batch.m_count = 0;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 0;
    geomFactory->GetBuffers(GxVBF_PNCT2, &vertexBuf, &indexBuf, &batch);

    if (!vertexBuf || !indexBuf || !batch.m_count) {
        return;
    }

    GxPrimVertexPtr(vertexBuf, GxVBF_PNCT2);
    g_theGxDevicePtr->PrimIndexPtr(indexBuf);
    g_theGxDevicePtr->Draw(&batch, 1);
}

void Liquid::Cata::EndReflectionMask() {
    if (!s_reflectionMaskDrawing) {
        return;
    }

    s_reflectionMaskDrawing = false;
    CGxDevice* device = g_theGxDevicePtr;
    device->RsPop();
    GxRenderTargetSet(GxBuffers_Color, s_maskSavedColor, 0);
    GxXformSetViewport(s_maskSavedViewport[0], s_maskSavedViewport[1], s_maskSavedViewport[2], s_maskSavedViewport[3], s_maskSavedViewport[4], s_maskSavedViewport[5]);
    device->ShaderConstantsSet(GxSh_Vertex, 0, s_maskSavedVs, 8);
    device->ShaderConstantsSet(GxSh_Pixel, 0, s_maskSavedPs, 8);

    CGxTex* real = GxTexOf(s_realReflectionTex);
    CGxTex* sky = GxTexOf(s_fakeReflectionTex);
    CGxTex* mask = GxTexOf(s_reflectionMaskTex);
    CGxTex* blend = GxTexOf(s_reflectionBlendTex);

    CGxTex* depth = GxTexOf(s_realReflectionDepth);
    CGxTex* real2 = GxTexOf(s_real2ReflectionTex);
    CGxTex* depth2 = GxTexOf(s_real2ReflectionDepth);
    bool second = s_real2Ready && s_plane2Valid && real2 && depth2;

    if (real && sky && mask && blend && depth && sky->m_width == real->m_width && sky->m_height == real->m_height && depth->m_width == real->m_width && depth->m_height == real->m_height) {
        C44Matrix view;
        GxXformView(view);
        const C44Matrix& projection = g_theGxDevicePtr->m_projNative;
        C4Vector params = ViewSettings::Params(s_zNear, s_zFar, s_viewportFarClip);
        CGxTex* scene = GxTexOf(s_backBufferTex);
        CGxTex* sceneDepth = DepthCache::GetDepthTex();
        bool trace = s_reflectionScreenSpace && scene && sceneDepth;
        float c[28] = {
            params.x, params.y, params.z, params.w,
            view.c0, view.c1, view.c2, REFLECTION_MASK_RANGE,
            projection.a0, projection.b1, projection.c0, projection.c1,
            2.0f, s_reflectionDistance > 0.0f ? s_reflectionDistance * 0.35f : 1.0e6f, s_reflectionDistance > 0.0f ? 1.0f / (s_reflectionDistance * 0.65f) : 0.0f, 0.0f,
            second ? s_planeHeight - s_plane2Height : 0.0f, second ? 1.0f : 0.0f,
            REFLECTION_BLEND_NEAR, 1.0f / (REFLECTION_BLEND_FAR - REFLECTION_BLEND_NEAR),
            s_reflectionSaturation, REFLECTION_TRACE_NEAR, 1.0f / (REFLECTION_TRACE_FAR - REFLECTION_TRACE_NEAR), 0.0f,
            s_planeHeight - s_maskCameraZ, trace ? 1.0f : 0.0f, REFLECTION_TRACE_DISTANCE, 1.5f,
        };
        s_reflectionBlendReady = PostEffects::ReflectionBlend(blend, real, sky, mask, depth, second ? real2 : real, second ? depth2 : depth, scene ? scene : real, sceneDepth ? sceneDepth : depth, c);
    }

    if (s_reflectionBlendReady) {
        LogOnce(23, "blending real and sky reflections per pixel");
    }
}

// 4.3.4's Liquid shader lighting (vertex c0-c4, pixel c0-c4).
static void SetLighting(const CM2Lighting& lighting) {
    C44Matrix view;
    GxXformView(view);

    float fogX = 0.0f;
    float fogY = 1.0f;
    float fogZ = 1.0f;

    if (g_theGxDevicePtr->MasterEnable(GxMasterEnable_Fog) && fabsf(lighting.m_fogScale) >= 2.3841858e-07f && lighting.m_fogEnd > lighting.m_fogStart) {
        float range = 1.0f / (lighting.m_fogEnd - lighting.m_fogStart);
        fogX = -(range * CShaderEffect::s_fogMul);
        fogY = lighting.m_fogEnd * range;
        fogZ = lighting.m_fogDensity;
    }

    SetVs(0, fogX, fogY, fogZ, 0.0f);

    C3Vector sunDir = lighting.m_sunDir * C33Matrix(view);
    SetVs(1, sunDir.x, sunDir.y, sunDir.z, 1.0f);
    SetVs(2, lighting.m_sunAmbient.x, lighting.m_sunAmbient.y, lighting.m_sunAmbient.z, 1.0f);
    SetVs(3, lighting.m_sunDiffuse.x, lighting.m_sunDiffuse.y, lighting.m_sunDiffuse.z, 1.0f);
    SetVs(4, lighting.m_sunSpecular.x, lighting.m_sunSpecular.y, lighting.m_sunSpecular.z, 6.0f);

    SetPs(0, lighting.m_fogColor.x, lighting.m_fogColor.y, lighting.m_fogColor.z, 1.0f);
    SetPs(1, -lighting.m_sunDir.x, -lighting.m_sunDir.y, -lighting.m_sunDir.z, -1.0f);
    SetPs(2, lighting.m_sunAmbient.x, lighting.m_sunAmbient.y, lighting.m_sunAmbient.z, 1.0f);
    SetPs(3, lighting.m_sunDiffuse.x, lighting.m_sunDiffuse.y, lighting.m_sunDiffuse.z, 1.0f);
    SetPs(4, lighting.m_sunSpecular.x, lighting.m_sunSpecular.y, lighting.m_sunSpecular.z, 50.0f);
}

// 4.3.4's fog constants for the water surface (pixel c18-c21): the current world fog, then a fixed fog with the
// sky fog colour, which the shader also uses where the reflection has no sample.
static void SetWaterFog() {
    DayNight::DNInfo* info = DayNight::GetInfo();
    bool fog = g_theGxDevicePtr->MasterEnable(GxMasterEnable_Fog) != 0;

    struct FogSet {
        float start;
        float end;
        float density;
        CImVector color;
    };

    FogSet sets[2] = {
        { info->m_fog.m_start, info->m_fog.m_end, info->m_fog.m_density, info->m_fog.m_color },
        { -10.0f, 200.0f, 5.0f, info->m_fog.m_color },
    };

    for (uint32_t i = 0; i < 2; i++) {
        const FogSet& set = sets[i];
        uint32_t reg = 18 + i * 2;

        if (fog && set.end > set.start) {
            float range = 1.0f / (set.end - set.start);
            SetPs(reg, -(range * CShaderEffect::s_fogMul), range * set.end, set.density, 0.0f);
        } else {
            SetPs(reg, 0.0f, 1.0f, 1.0f, 0.0f);
        }

        SetPs(reg + 1, set.color.r / 255.0f, set.color.g / 255.0f, set.color.b / 255.0f, set.color.a / 255.0f);
    }
}

// 4.3.4's per-draw transforms (vertex c14-c25, pixel c5-c16): world (camera-relative), world-view and projection
// for the vertex shader; view, projection and view-projection for the pixel shader.
static void SetTransforms(const C3Vector& cameraPos, const C44Matrix& transform) {
    C44Matrix world = transform;

    if (Liquid::UseCameraRelativeCoords()) {
        world.d0 -= cameraPos.x;
        world.d1 -= cameraPos.y;
        world.d2 -= cameraPos.z;
    }

    C44Matrix view;
    GxXformView(view);

    C44Matrix projection = g_theGxDevicePtr->m_projNative;

    if (g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl) {
        projection.c0 *= -1.0f;
        projection.c1 *= -1.0f;
        projection.c2 *= -1.0f;
        projection.c3 *= -1.0f;
    }

    SetVs(14, world);
    SetVs(18, world * view);
    SetVs(22, projection);
    SetPs(5, view);
    SetPs(9, projection);
    SetPs(13, view * projection);
}

static C44Matrix TexTransform(float angle, float scale) {
    C44Matrix matrix = C44Matrix::RotationAroundZ(angle);
    matrix.Scale(scale);
    return matrix;
}

// 4.3.4's slow scroll of the depth tint texture.
static C44Matrix ScrollMatrix(uint32_t timeMs, float speedU, float speedV) {
    C44Matrix matrix;

    if (speedU != 0.0f) {
        uint32_t period = static_cast<uint32_t>(1000.0f / speedU);
        matrix.d0 = period ? static_cast<float>(timeMs % period) / static_cast<float>(period) : 0.0f;
    }

    if (speedV != 0.0f) {
        uint32_t period = static_cast<uint32_t>(1000.0f / speedV);
        matrix.d1 = period ? static_cast<float>(timeMs % period) / static_cast<float>(period) : 0.0f;
    }

    return matrix;
}

// The water's own textures filtered anisotropically, so its normal maps and foam keep their detail on distant
// water seen at a low angle instead of blurring flat (they are loaded with plain trilinear filtering).
static void SharpenAtDistance(CGxTex* texture) {
    const CGxCaps& caps = g_theGxDevicePtr->Caps();

    if (!texture || !caps.m_texFilterAnisotropic || texture->m_flags.m_filter == GxTex_Anisotropic) {
        return;
    }

    uint32_t anisotropy = caps.m_maxTexAnisotropy < 16 ? caps.m_maxTexAnisotropy : 16;
    texture->m_flags.m_filter = GxTex_Anisotropic;
    texture->m_flags.m_maxAnisotropy = anisotropy ? anisotropy : 1;
}

// The real reflection with a quarter (index 0), half (1) or three quarters (2) of the sky one over it.
static CGxTex* ReflectionMix(uint32_t index, CGxTex* real, CGxTex* sky) {
    if (s_reflectionMixReady[index]) {
        return GxTexOf(s_reflectionMixTex[index]);
    }

    if (s_reflectionMixTried[index]) {
        return nullptr;
    }

    s_reflectionMixTried[index] = true;

    if (!s_reflectionMixTex[index]) {
        s_reflectionMixTex[index] = CreateTarget(GxTex_NonPow2, s_reflectionWidth, s_reflectionHeight, "LiquidProc Reflection Mix");
    }

    CGxTex* mix = GxTexOf(s_reflectionMixTex[index]);

    if (!mix || mix->m_width != real->m_width || mix->m_height != real->m_height || sky->m_width != real->m_width || sky->m_height != real->m_height) {
        return nullptr;
    }

    float amount = static_cast<float>(index + 1) / static_cast<float>(REFLECTION_MIXES + 1);

    if (!PostEffects::Blend(mix, real, 1.0f) || !PostEffects::Blend(mix, sky, amount)) {
        return nullptr;
    }

    s_reflectionMixReady[index] = true;
    return mix;
}

bool Liquid::Cata::RenderWater(IEnvironment* environment, IGeomFactory* geomFactory, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings) {
    if (s_detail < 2) {
        return false;
    }

    if (!s_inPass) {
        LogOnce(8, "skipped, water drawn outside the see-through liquid pass");
        return false;
    }

    CGxTex* depthTint = settings->GetTexture(1, 1250);
    CGxTex* bump = settings->GetTexture(2, 0);
    CGxTex* foam = settings->GetTexture(3, 0);

    // Water types without the 4.3.4 textures (an older LiquidType.dbc) draw the 3.3.5 way.
    if (!depthTint || !bump || !foam) {
        // A texture still loading leaves it out for this frame only; one the type does not have, for good.
        if (!settings->m_textures[1].Count() || !settings->m_textures[2].Count() || !settings->m_textures[3].Count()) {
            LogOnce(6, "a water type has no 4.3.4 textures (depth tint '%s', bump '%s', foam '%s'); is patch-C.MPQ's LiquidType.dbc loaded?", settings->m_textureNames[1], settings->m_textureNames[2], settings->m_textureNames[3]);
        }

        return false;
    }

    SharpenAtDistance(depthTint);
    SharpenAtDistance(bump);
    SharpenAtDistance(foam);

    if (!PreparePass()) {
        return false;
    }

    CGxTex* depth = DepthCache::GetDepthTex();
    CGxTex* backBuffer = GxTexOf(s_backBufferTex);
    CGxTex* blurred = GxTexOf(s_blurredBackTex);
    CGxTex* reflection = GxTexOf(s_fakeReflectionTex);

    if (!depth || !backBuffer || !blurred || !reflection) {
        return false;
    }

    // The water nearest the camera sets next frame's reflection plane.
    s_lastCameraZ = cameraPos.z;
    s_lastCameraZValid = true;

    if (!s_cameraUnderwater && bounds->c.z < cameraPos.z) {
        // Nearest across the ground (the water around the camera, rather than whichever is nearest along the view).
        C3Vector toWater = bounds->c - cameraPos;
        float distance = toWater.x * toWater.x + toWater.y * toWater.y;

        if (!s_nextPlaneValid || distance < s_nextPlaneDistance) {
            s_nextPlaneValid = true;
            s_nextPlaneDistance = distance;
            s_nextPlaneHeight = bounds->c.z;
        }

        if (s_planeValid && fabsf(bounds->c.z - s_planeHeight) > 1.0f && fabsf(bounds->c.z - s_planeHeight) < 12.0f && (!s_next2Valid || distance < s_next2Distance)) {
            s_next2Valid = true;
            s_next2Distance = distance;
            s_next2Height = bounds->c.z;
        }

        if (s_plane2Valid && fabsf(bounds->c.z - s_plane2Height) < 1.0f && (!s_nextSame2Valid || distance < s_nextSame2Distance)) {
            s_nextSame2Valid = true;
            s_nextSame2Distance = distance;
            s_nextSame2Height = bounds->c.z;
        }

        if (s_planeValid && fabsf(bounds->c.z - s_planeHeight) < 1.0f && (!s_nextSameValid || distance < s_nextSameDistance)) {
            s_nextSameValid = true;
            s_nextSameDistance = distance;
            s_nextSameHeight = bounds->c.z;
        }
    }

    // A real reflection when one was drawn this frame for water at this height, else the sky reflection;
    // underwater 4.3.4 draws the surface from below without one.
    CGxTex* realReflection = GxTexOf(s_realReflectionTex);
    // CHANGED: eased with the height from the reflected level, rather than switched at a few yards: the real
    // reflection is only right for water at that level, and the chunks of a sloping river (or a nearer lake
    // taking over the plane) otherwise flipped between the two beside their neighbours.
    CGxTex* reflectionTex = reflection;
    bool real = false;

    if (s_reflectionBlendReady && realReflection && !s_cameraUnderwater) {
        real = true;
        reflectionTex = GxTexOf(s_reflectionBlendTex);
    } else if (s_realReady && realReflection && !s_cameraUnderwater) {
        float offset = fabsf(bounds->c.z - s_planeHeight);
        float sky = (offset - REFLECTION_BLEND_NEAR) / (REFLECTION_BLEND_FAR - REFLECTION_BLEND_NEAR);
        int32_t level = static_cast<int32_t>(floorf((sky < 0.0f ? 0.0f : (sky > 1.0f ? 1.0f : sky)) * (REFLECTION_MIXES + 1) + 0.5f));

        if (level == 0) {
            real = true;
            reflectionTex = realReflection;
        } else if (level <= static_cast<int32_t>(REFLECTION_MIXES)) {
            CGxTex* mix = ReflectionMix(level - 1, realReflection, reflection);

            if (mix) {
                // The real reflection's shader (its depth fade and distortion) draws the mix too.
                real = true;
                reflectionTex = mix;
            } else if (level <= static_cast<int32_t>(REFLECTION_MIXES) / 2 + 1) {
                real = true;
                reflectionTex = realReflection;
            }
        }
    }
    uint32_t reflectionKind = real ? 1 : 0;
    CGxShader* pixelShader = s_cameraUnderwater ? s_belowPixelShaders[BelowPermutation(reflectionKind)] : s_abovePixelShaders[AbovePermutation(reflectionKind)];
    CGxShader* vertexShader = s_vertexShaders[s_rippleDetail * 4];

    if (!Valid(pixelShader) || !Valid(vertexShader)) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    device->RsPush();
    GxRsSet(GxRs_BlendingMode, 2);
    GxUpdateAlphaRef(GxRs_AlphaRef);
    GxRsSet(GxRs_Fog, 1);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Culling, 0);

    memset(s_vs, 0, sizeof(s_vs));
    memset(s_ps, 0, sizeof(s_ps));

    CM2Lighting lighting(*bounds);
    environment->SelectLights(&lighting);
    SetLighting(lighting);

    C44Matrix identity;
    bool unchanged = geomFactory->SetTransform(const_cast<C44Matrix*>(transform));
    SetTransforms(cameraPos, unchanged ? identity : *transform);

    float coordScale = Liquid::WorldCoordScale();
    uint32_t timeMs = OsGetAsyncTimeMs();

    // Normal map coordinates: both layers turned by float 5 (degrees), scaled by floats 3 and 4.
    float angle = settings->GetFloat(5) * 0.017453292f;
    SetVs(26, TexTransform(angle, settings->GetFloat(3) * coordScale));
    SetVs(30, TexTransform(angle, settings->GetFloat(4) * coordScale));

    // Depth tint coordinate (texture coordinate 1), stretched by float 2.
    C44Matrix depthScale;
    depthScale.b1 = settings->GetFloat(2) * coordScale;
    SetVs(34, depthScale);
    SetVs(38, ScrollMatrix(timeMs, 0.015f, 0.0f));

    float phase = static_cast<float>(timeMs % 10000) * 0.0001f * 3.1415927f;
    float wave = settings->GetFloat(7) * 0.35f;
    SetVs(50, phase, wave, 1.0f, settings->GetFloat(8));
    SetRippleConstants(cameraPos);

    SetPs(17, cameraPos.x, cameraPos.y, cameraPos.z, 1.0f);
    SetWaterFog();
    SetPs(22, ViewSettings::Params(s_zNear, s_zFar, s_viewportFarClip));

    // CHANGED: softer shorelines with distance. 4.3.4 fades the water out over the first few centimetres of its
    // depth, which far away is less than a pixel, so the water looks cut off where it meets the shore. The depths
    // the shader compares (its own, through the view matrix's z column in c5-c8, and the scene's, through c22) are
    // both scaled down for distant water: the fade and the shore foam then cover more of the shallows, while the
    // depth ratios and comparisons it also makes stay the same.
    // The scale is the same for every chunk in a frame (a chunk's own distance gave neighbouring chunks different
    // ones, a visible step along their edge): it follows the camera's height above the water instead, which is how
    // far off the shores seen are.
    {
        // At most 10 yards' worth: from high above, a stronger fade took all of a shallow river's depth, and the
        // water looked missing until the camera came down.
        float aboveWater = s_planeValid ? fabsf(cameraPos.z - s_planeHeight) : 0.0f;
        aboveWater = aboveWater > 10.0f ? 10.0f : aboveWater;
        // Nothing close to the water (4.3.4's own fade, which keeps very shallow stretches visible), growing with the
        // camera's height above it.
        float scale = s_shorelineSoftness > 0.0f ? 1.0f / (1.0f + s_shorelineSoftness * (aboveWater / 10.0f)) : 1.0f;

        for (uint32_t reg = 5; reg <= 8; reg++) {
            s_ps[reg * 4 + 2] *= scale;
        }

        s_ps[22 * 4 + 2] /= scale;
        s_ps[22 * 4 + 3] /= scale;
    }
    SetPs(23, settings->GetFloat(13), settings->GetFloat(14), settings->GetFloat(15), settings->GetFloat(6));

    float width = static_cast<float>(s_screenWidth);
    float height = static_cast<float>(s_screenHeight);
    SetPs(24, width, height, 1.0f / width, 1.0f / height);
    SetPs(25, 0.5f / width, 0.5f / height, width * 2.0f, height * 2.0f);
    SetPs(26, ViewSettings::HomToScreen(0.5f / width, 0.5f / height));
    SetPs(27, settings->GetFloat(12), settings->GetFloat(11), settings->GetFloat(9), settings->GetFloat(10));
    SetPs(28, phase, wave, 1.0f, 1.0f);

    device->RsSet(GxRs_Texture0, backBuffer);
    device->RsSet(GxRs_Texture1, reflectionTex);
    device->RsSet(GxRs_Texture2, depthTint);
    device->RsSet(GxRs_Texture3, blurred);
    device->RsSet(GxRs_Texture5, bump);
    device->RsSet(GxRs_Texture6, depth);
    device->RsSet(GxRs_Texture7, foam);
    BindRippleMaps();

    device->ShaderConstantsSet(GxSh_Vertex, 0, s_vs, VS_REGISTERS);
    device->ShaderConstantsSet(GxSh_Pixel, 0, s_ps, PS_REGISTERS);
    device->RsSet(GxRs_VertexShader, vertexShader);
    device->RsSet(GxRs_PixelShader, pixelShader);

    CGxBuf* vertexBuf = nullptr;
    CGxBuf* indexBuf = nullptr;
    CGxBatch batch;
    batch.m_primType = GxPrims_Last;
    batch.m_start = 0;
    batch.m_count = 0;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 0;
    geomFactory->GetBuffers(GxVBF_PNCT2, &vertexBuf, &indexBuf, &batch);
    GxPrimVertexPtr(vertexBuf, GxVBF_PNCT2);
    device->PrimIndexPtr(indexBuf);
    device->Draw(&batch, 1);

    device->RsPop();
    LogOnce(7, "drawing");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Magma and slime (and the other material 2 liquids), as 4.3.4 draws them with its liquid detail on: the
// ProcMagma shaders blend three scrolling, turning layers (Tex1-3 of LiquidType.dbc) over a base colour (its
// int 2), picked by a 3D noise volume (XTextures\FX\noise.blob, 128 x 128 x 16) that a slow wave slides through.

static CGxShader* s_magmaVertexShader;
static CGxShader* s_magmaPixelShader;
static CTexture* s_noiseTex;
static bool s_magmaTried;
static bool s_magmaReady;

static const uint32_t NOISE_SIZE = 128;
static const uint32_t NOISE_DEPTH = 16;

static bool LoadNoiseVolume() {
    SFileImpl* file = nullptr;

    if (!SFileOpenFile("XTextures\\FX\\noise.blob", &file) || !file) {
        LogOnce(14, "magma off, XTextures\\FX\\noise.blob is missing (is the new patch-C.MPQ in Data?)");
        return false;
    }

    std::vector<uint8_t> data(NOISE_SIZE * NOISE_SIZE * NOISE_DEPTH * 4);
    uint32_t read = 0;
    bool ok = SFileGetFileSize(file, nullptr) == data.size() && SFileReadFile(file, data.data(), static_cast<uint32_t>(data.size()), &read, nullptr, 0) && read == data.size();
    SFileCloseFile(file);

    if (!ok) {
        LogOnce(15, "magma off, XTextures\\FX\\noise.blob is not a 128 x 128 x 16 volume");
        return false;
    }

    CGxTexFlags flags(GxTex_Linear, 1, 1, 0, 0, 0);
    s_noiseTex = TextureCreate(GxTex_2d, NOISE_SIZE, NOISE_SIZE, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, "proceduralNoiseVolTex", 0);
    CGxTex* gxTex = GxTexOf(s_noiseTex);

    if (!gxTex || !g_theGxDevicePtr->DeviceTexSetVolume(gxTex, NOISE_SIZE, NOISE_SIZE, NOISE_DEPTH, data.data())) {
        LogOnce(16, "magma off, the device cannot make the noise volume texture");
        ReleaseTexture(s_noiseTex);
        return false;
    }

    return true;
}

static bool MagmaReady() {
    if (s_magmaTried) {
        return s_magmaReady;
    }

    s_magmaTried = true;
    CGxDevice* device = g_theGxDevicePtr;
    device->ShaderCreate(&s_magmaVertexShader, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "ProcMagma", 1);
    device->ShaderCreate(&s_magmaPixelShader, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "ProcMagma", 1);

    if (!Valid(s_magmaVertexShader) || !Valid(s_magmaPixelShader)) {
        LogOnce(17, "magma off, Shaders\\Cata\\...\\ProcMagma.bls did not load (is the new patch-C.MPQ in Data?)");
    } else if (LoadNoiseVolume()) {
        s_magmaReady = true;
        LogOnce(18, "magma and slime on");
    }

    if (!s_magmaReady) {
        ReleaseShader(s_magmaVertexShader);
        ReleaseShader(s_magmaPixelShader);
    }

    return s_magmaReady;
}

static C44Matrix TurnScaleScroll(float angle, float scale, uint32_t timeMs, float speed) {
    C44Matrix matrix = C44Matrix::RotationAroundZ(angle);
    matrix.Scale(scale);
    return matrix * ScrollMatrix(timeMs, speed, 0.0f);
}

bool Liquid::Cata::RenderMagma(IEnvironment* environment, IGeomFactory* geomFactory, const C3Vector& cameraPos, const C44Matrix* transform, const CAaSphere* bounds, CMaterialSettings* settings) {
    if (s_detail < 2 || !MagmaReady()) {
        return false;
    }

    CGxTex* layers[3] = { settings->GetTexture(1, 0), settings->GetTexture(2, 0), settings->GetTexture(3, 0) };
    CGxTex* noise = GxTexOf(s_noiseTex);

    // Liquid types without the 4.3.4 layers (an older LiquidType.dbc) draw the 3.3.5 way.
    if (!layers[0] || !layers[1] || !layers[2] || !noise) {
        if (!settings->m_textures[1].Count() || !settings->m_textures[2].Count() || !settings->m_textures[3].Count()) {
            LogOnce(19, "a magma type has no 4.3.4 layers ('%s', '%s', '%s'); is patch-C.MPQ's LiquidType.dbc loaded?", settings->m_textureNames[1], settings->m_textureNames[2], settings->m_textureNames[3]);
        }

        return false;
    }

    float savedVs[VS_REGISTERS * 4];
    float savedPs[PS_REGISTERS * 4];
    memcpy(savedVs, CGxDevice::s_shadowConstants[1].constants, sizeof(savedVs));
    memcpy(savedPs, CGxDevice::s_shadowConstants[0].constants, sizeof(savedPs));

    CGxDevice* device = g_theGxDevicePtr;
    device->RsPush();
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_Fog, 1);
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_Culling, 0);

    memset(s_vs, 0, sizeof(s_vs));
    memset(s_ps, 0, sizeof(s_ps));

    CM2Lighting lighting(*bounds);
    environment->SelectLights(&lighting);
    SetLighting(lighting);

    C44Matrix identity;
    bool unchanged = geomFactory->SetTransform(const_cast<C44Matrix*>(transform));
    SetTransforms(cameraPos, unchanged ? identity : *transform);

    float coordScale = Liquid::WorldCoordScale();
    uint32_t timeMs = OsGetAsyncTimeMs();
    const float toRadians = 0.017453292f;
    float speed = settings->GetFloat(11);

    // The slow wave that slides the noise lookup (texture coordinate 2's z).
    C44Matrix wave;
    wave.Scale(0.05f);
    SetVs(26, wave * ScrollMatrix(timeMs, speed, 0.0f));

    // Two noise lookups, turned by float 3 (degrees), at two sizes of float 2.
    float angle = settings->GetFloat(3) * toRadians;
    SetVs(30, TurnScaleScroll(angle, settings->GetFloat(2) * coordScale * 0.617f, timeMs, speed * 0.5f));
    SetVs(34, TurnScaleScroll(angle, settings->GetFloat(2) * coordScale * 0.474f, timeMs, speed * 0.5f));

    // The three layers: turned by floats 7-9, scaled by floats 4-6.
    SetVs(38, TurnScaleScroll(settings->GetFloat(7) * toRadians, settings->GetFloat(4), timeMs, speed));
    SetVs(42, TurnScaleScroll(settings->GetFloat(8) * toRadians, settings->GetFloat(5), timeMs, speed));
    SetVs(46, TurnScaleScroll(settings->GetFloat(9) * toRadians, settings->GetFloat(6), timeMs, speed));

    float phase = static_cast<float>(timeMs % 600000) * 1.6666667e-6f;
    SetVs(50, settings->GetFloat(10), 0.0f, 0.0f, phase);

    // The base colour (int 2, 0xRRGGBB) and the noise volume's depth.
    uint32_t color = static_cast<uint32_t>(settings->GetInt(2));
    SetPs(17, ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, phase);

    device->RsSet(GxRs_Texture0, noise);
    device->RsSet(GxRs_Texture1, layers[0]);
    device->RsSet(GxRs_Texture2, layers[1]);
    device->RsSet(GxRs_Texture3, layers[2]);

    device->ShaderConstantsSet(GxSh_Vertex, 0, s_vs, VS_REGISTERS);
    device->ShaderConstantsSet(GxSh_Pixel, 0, s_ps, PS_REGISTERS);
    device->RsSet(GxRs_VertexShader, s_magmaVertexShader);
    device->RsSet(GxRs_PixelShader, s_magmaPixelShader);

    CGxBuf* vertexBuf = nullptr;
    CGxBuf* indexBuf = nullptr;
    CGxBatch batch;
    batch.m_primType = GxPrims_Last;
    batch.m_start = 0;
    batch.m_count = 0;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 0;
    geomFactory->GetBuffers(GxVBF_PCT, &vertexBuf, &indexBuf, &batch);
    GxPrimVertexPtr(vertexBuf, GxVBF_PCT);
    device->PrimIndexPtr(indexBuf);
    device->Draw(&batch, 1);

    device->RsPop();
    device->ShaderConstantsSet(GxSh_Vertex, 0, savedVs, VS_REGISTERS);
    device->ShaderConstantsSet(GxSh_Pixel, 0, savedPs, PS_REGISTERS);
    LogOnce(20, "drawing magma");
    return true;
}
