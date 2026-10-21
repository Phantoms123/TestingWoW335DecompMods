#include "WoW/Source/LiquidSystem/CataWater.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxDevice/ShaderCompat.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/ShaderEffect/ViewSettings.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C33Matrix.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CRect.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"

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
static uint32_t s_reflectionMode;
static uint32_t s_reflectionDownscale;
static bool s_failed;

static CGxShader* s_vertexShaders[VS_PERMUTATIONS];
static CGxShader* s_abovePixelShaders[ABOVE_PERMUTATIONS];
static CGxShader* s_belowPixelShaders[BELOW_PERMUTATIONS];
static CGxShader* s_blurVertexShader;
static CGxShader* s_blurPixelShader;

static CTexture* s_backBufferTex;
static CTexture* s_fakeReflectionTex;
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
    ReleaseTexture(s_blurredBackTex);
    ReleaseTexture(s_blurTempTex);
    s_screenWidth = 0;
    s_screenHeight = 0;
    s_reflectionWidth = 0;
    s_reflectionHeight = 0;
}

static void Destroy() {
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

static CTexture* CreateTarget(EGxTexTarget target, uint32_t width, uint32_t height, const char* name) {
    CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1);
    CTexture* texture = TextureCreate(target, width, height, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, name, 0);

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
    // The ripple simulation is not ported; its shader permutations need it, so ripples stay at 0.
    (void)detail;
    s_rippleDetail = 0;
}

void Liquid::Cata::SetReflectionMode(uint32_t mode) {
    s_reflectionMode = mode > 3 ? 3 : mode;
}

void Liquid::Cata::SetReflectionDownscale(uint32_t downscale) {
    s_reflectionDownscale = downscale > 4 ? 4 : downscale;
}

uint32_t Liquid::Cata::GetWaterDetail() {
    return s_detail;
}

void Liquid::Cata::BeginFrame() {
    uint32_t wanted = s_requestedDetail >= 2 && !s_failed ? s_requestedDetail : 0;

    if (wanted && !Supported()) {
        wanted = 0;
    }

    if (wanted == s_detail) {
        return;
    }

    if (!wanted) {
        Destroy();
        DepthCache::Release();
        s_detail = 0;
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
static void RenderSkyReflection(CGxTex* target) {
    CGxDevice* device = g_theGxDevicePtr;
    DayNight::DNInfo* info = DayNight::GetInfo();

    GxRenderTargetSet(GxBuffers_Color, target, 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    GxSceneClear(GxBufferBit_Color, info->m_fog.m_color);

    if (!info->m_showSky) {
        return;
    }

    C44Matrix view;
    GxXformView(view);

    C3Vector forward(view.a2, view.b2, view.c2);
    C3Vector mirrored(forward.x, forward.y, -forward.z);
    C3Vector eye(0.0f, 0.0f, 0.0f);
    C3Vector up(0.0f, 0.0f, 1.0f);
    C44Matrix mirroredView;

    if (mirrored.x * mirrored.x + mirrored.y * mirrored.y < 0.0001f) {
        // Straight up or down: no world up to roll against, so a fixed side axis.
        mirroredView = view;
        mirroredView.a2 = -view.a2;
        mirroredView.b2 = -view.b2;
        mirroredView.c2 = -view.c2;
        mirroredView.d0 = 0.0f;
        mirroredView.d1 = 0.0f;
        mirroredView.d2 = 0.0f;
    } else {
        GxuXformCreateLookAtSgCompat(eye, mirrored, up, mirroredView);
    }

    device->XformSetView(mirroredView);
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

void Liquid::Cata::BeginLiquidPass(bool cameraUnderwater) {
    s_cameraUnderwater = cameraUnderwater;
    s_inPass = s_detail >= 2;
    s_passPrepared = false;
    s_passReady = false;

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
        LogOnce(6, "a water type has no 4.3.4 textures (depth tint %d, bump %d, foam %d); is patch-C.MPQ's LiquidType.dbc loaded?", depthTint ? 1 : 0, bump ? 1 : 0, foam ? 1 : 0);
        return false;
    }

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

    // Sky reflection for open water; underwater 4.3.4 draws the surface from below without one.
    uint32_t reflectionKind = 0;
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
    // Ripple maps (vertex c51-c52, pixel c29-c31) stay zero without the ripple simulation.

    SetPs(17, cameraPos.x, cameraPos.y, cameraPos.z, 1.0f);
    SetWaterFog();
    SetPs(22, ViewSettings::Params(s_zNear, s_zFar, s_viewportFarClip));
    SetPs(23, settings->GetFloat(13), settings->GetFloat(14), settings->GetFloat(15), settings->GetFloat(6));

    float width = static_cast<float>(s_screenWidth);
    float height = static_cast<float>(s_screenHeight);
    SetPs(24, width, height, 1.0f / width, 1.0f / height);
    SetPs(25, 0.5f / width, 0.5f / height, width * 2.0f, height * 2.0f);
    SetPs(26, ViewSettings::HomToScreen(0.5f / width, 0.5f / height));
    SetPs(27, settings->GetFloat(12), settings->GetFloat(11), settings->GetFloat(9), settings->GetFloat(10));
    SetPs(28, phase, wave, 1.0f, 1.0f);

    device->RsSet(GxRs_Texture0, backBuffer);
    device->RsSet(GxRs_Texture1, reflection);
    device->RsSet(GxRs_Texture2, depthTint);
    device->RsSet(GxRs_Texture3, blurred);
    device->RsSet(GxRs_Texture5, bump);
    device->RsSet(GxRs_Texture6, depth);
    device->RsSet(GxRs_Texture7, foam);

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
