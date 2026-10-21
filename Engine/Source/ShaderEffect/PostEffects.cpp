#include "Engine/Source/ShaderEffect/PostEffects.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/ShaderEffect/ViewSettings.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Vector.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/Tempest/CRect.h"

#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/ao_blur_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/ao_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/bloom_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/blur_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/bright_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/copy_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/depthfade_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/fill_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/fxaa_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/haze_ps.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/post_vs.h"
#include "Engine/Source/ShaderEffect/PostShaders/vkd3d/sharpen_ps.h"

// What the passes overwrite in the client's own constants (some stay set for a whole frame).
static const uint32_t SAVED_CONSTANTS = 8;

static bool s_aoRequested;
static bool s_bloomRequested;
static bool s_aaRequested;
static bool s_hazeRequested;

// Defaults as Waystone's settings.
static float s_params[PostEffects::Params_Last] = {
    4.0f,     // aoRadius
    1.8f,     // aoStrength
    0.0f,     // aoDebug
    0.45f,    // bloomIntensity
    0.6f,     // bloomThreshold
    1.0f,     // toneMapping
    1.0f,     // toneExposure
    1.05f,    // colorSaturation
    1.0f,     // colorContrast
    0.4f,     // sharpen
    0.0015f,  // aerialFogDensity (Waystone's 0.0005 hardly shows over 3.3.5's own distance fog)
    0.0067f,  // aerialFogFalloff
    80.0f,    // aerialFogStart
    0.7f,     // aerialFogMax
};

static bool s_created;
static bool s_failed;
static bool s_usingDepth;

static CGxShader* s_vs;
static CGxShader* s_hazePs;
static CGxShader* s_aoPs;
static CGxShader* s_aoBlurPs;
static CGxShader* s_copyPs;
static CGxShader* s_brightPs;
static CGxShader* s_blurPs;
static CGxShader* s_bloomPs;
static CGxShader* s_fxaaPs;
static CGxShader* s_sharpenPs;
static CGxShader* s_fillPs;
static CGxShader* s_depthFadePs;

static CTexture* s_sceneTex;
static CTexture* s_aoTex[2];
static CTexture* s_bloomTex[2];
static CTexture* s_bloomWideTex[2];
static uint32_t s_width;
static uint32_t s_height;

static const uint16_t s_quadIndices[4] = { 0, 3, 1, 2 };
static const C3Vector s_quad[4] = { C3Vector(-1.0f, -1.0f, 0.0f), C3Vector(-1.0f, 1.0f, 0.0f), C3Vector(1.0f, 1.0f, 0.0f), C3Vector(1.0f, -1.0f, 0.0f) };

static uint32_t s_logged;

static void LogOnce(uint32_t bit, const char* message) {
    if (s_logged & (1u << bit)) {
        return;
    }

    s_logged |= 1u << bit;
    fprintf(stderr, "Post effects: %s\n", message);
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

static CGxTex* GxTexOf(CTexture* texture) {
    return texture ? TextureGetGxTex(texture, 1, nullptr) : nullptr;
}

// A shader whose code is compiled into the client: created under a name no file has, then given the code.
static CGxShader* CreateEmbedded(EGxShTarget target, const char* name, const unsigned char* code, uint32_t size) {
    CGxShader* shader = nullptr;
    g_theGxDevicePtr->ShaderCreate(&shader, target, "Shaders\\Post", name, 1);

    if (shader && shader->m_code.Count() == 0) {
        shader->m_code.SetCount(size);
        memcpy(shader->m_code.m_data, code, size);
        shader->m_loaded = 0;
    }

    if (!shader || !shader->Valid()) {
        ReleaseShader(shader);
    }

    return shader;
}

static void DestroyTargets() {
    ReleaseTexture(s_sceneTex);

    for (uint32_t i = 0; i < 2; i++) {
        ReleaseTexture(s_aoTex[i]);
        ReleaseTexture(s_bloomTex[i]);
        ReleaseTexture(s_bloomWideTex[i]);
    }

    s_width = 0;
    s_height = 0;
}

static void Destroy() {
    ReleaseShader(s_vs);
    ReleaseShader(s_hazePs);
    ReleaseShader(s_aoPs);
    ReleaseShader(s_aoBlurPs);
    ReleaseShader(s_copyPs);
    ReleaseShader(s_brightPs);
    ReleaseShader(s_blurPs);
    ReleaseShader(s_bloomPs);
    ReleaseShader(s_fxaaPs);
    ReleaseShader(s_sharpenPs);
    ReleaseShader(s_fillPs);
    ReleaseShader(s_depthFadePs);
    DestroyTargets();
    s_created = false;
}

static bool Create() {
    const CGxCaps& caps = g_theGxDevicePtr->Caps();

    if (caps.m_shaderTargets[GxSh_Vertex] < 1 || caps.m_shaderTargets[GxSh_Pixel] < 3 || !caps.m_rttFormat[GxTex_Argb8888] || caps.m_texTarget[GxTex_NonPow2] != 1) {
        LogOnce(0, "off, the device cannot run shader model 3 effects into textures");
        return false;
    }

    // Their shaders are Direct3D 9 ones.
    if (g_theGxDevicePtr->DeviceApi() != GxApi_D3d9) {
        LogOnce(0, "off, they need the Direct3D 9 device (gxApi D3D9)");
        return false;
    }

#define POST_SHADER(var, target, name, code) var = CreateEmbedded(target, name, code, sizeof(code))
    POST_SHADER(s_vs, GxSh_Vertex, "PostEffects", g_post_vs);
    POST_SHADER(s_hazePs, GxSh_Pixel, "PostHaze", g_post_haze_ps);
    POST_SHADER(s_aoPs, GxSh_Pixel, "PostAO", g_post_ao_ps);
    POST_SHADER(s_aoBlurPs, GxSh_Pixel, "PostAOBlur", g_post_ao_blur_ps);
    POST_SHADER(s_copyPs, GxSh_Pixel, "PostCopy", g_post_copy_ps);
    POST_SHADER(s_brightPs, GxSh_Pixel, "PostBright", g_post_bright_ps);
    POST_SHADER(s_blurPs, GxSh_Pixel, "PostBlur", g_post_blur_ps);
    POST_SHADER(s_bloomPs, GxSh_Pixel, "PostBloom", g_post_bloom_ps);
    POST_SHADER(s_fxaaPs, GxSh_Pixel, "PostFXAA", g_post_fxaa_ps);
    POST_SHADER(s_sharpenPs, GxSh_Pixel, "PostSharpen", g_post_sharpen_ps);
#undef POST_SHADER

    if (!s_vs || !s_hazePs || !s_aoPs || !s_aoBlurPs || !s_copyPs || !s_brightPs || !s_blurPs || !s_bloomPs || !s_fxaaPs || !s_sharpenPs) {
        LogOnce(1, "off, a shader could not be created");
        return false;
    }

    s_created = true;
    return true;
}

static CTexture* CreateTarget(uint32_t width, uint32_t height, const char* name) {
    CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1);
    CTexture* texture = TextureCreate(GxTex_NonPow2, width, height, 0, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, name, 0);

    if (texture && !TextureGetGxTex(texture, 1, nullptr)) {
        ReleaseTexture(texture);
    }

    return texture;
}

static uint32_t Shrunk(uint32_t size, uint32_t shift) {
    uint32_t value = size >> shift;
    return value ? value : 1;
}

static bool EnsureTargets() {
    CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(window);
    uint32_t width = static_cast<uint32_t>(window.maxX - window.minX);
    uint32_t height = static_cast<uint32_t>(window.maxY - window.minY);

    if (width < 16 || height < 16) {
        return false;
    }

    if (width != s_width || height != s_height) {
        DestroyTargets();
        s_sceneTex = CreateTarget(width, height, "Post Scene");

        for (uint32_t i = 0; i < 2; i++) {
            s_aoTex[i] = CreateTarget(Shrunk(width, 1), Shrunk(height, 1), "Post AO");
            s_bloomTex[i] = CreateTarget(Shrunk(width, 2), Shrunk(height, 2), "Post Bloom");
            s_bloomWideTex[i] = CreateTarget(Shrunk(width, 3), Shrunk(height, 3), "Post Bloom Wide");
        }

        s_width = width;
        s_height = height;
    }

    return s_sceneTex && s_aoTex[0] && s_aoTex[1] && s_bloomTex[0] && s_bloomTex[1] && s_bloomWideTex[0] && s_bloomWideTex[1];
}

void PostEffects::SetAmbientOcclusion(bool enable) {
    s_aoRequested = enable;
    s_failed = false;
}

void PostEffects::SetBloom(bool enable) {
    s_bloomRequested = enable;
    s_failed = false;
}

void PostEffects::SetAntiAliasing(bool enable) {
    s_aaRequested = enable;
    s_failed = false;
}

void PostEffects::SetAerialFog(bool enable) {
    s_hazeRequested = enable;
    s_failed = false;
}

void PostEffects::SetParam(EParam param, float value) {
    if (param < Params_Last && std::isfinite(value)) {
        s_params[param] = value;
    }
}

void PostEffects::BeginFrame() {
    bool wanted = (s_aoRequested || s_bloomRequested || s_aaRequested || s_hazeRequested) && !s_failed;

    if (wanted && !s_created && !Create()) {
        Destroy();
        s_failed = true;
        wanted = false;
    }

    if (!wanted && s_created) {
        Destroy();
    }

    // The scene depth is kept only while an effect reading it is on.
    bool depth = wanted && (s_aoRequested || s_hazeRequested) && DepthCache::IsSupported();

    if (depth != s_usingDepth) {
        if (depth) {
            DepthCache::AddRef();
        } else {
            DepthCache::Release();
        }

        s_usingDepth = depth;
    }
}

namespace {
    struct PassState {
        CGxTex* color;
        float viewport[6];
        float vertex[SAVED_CONSTANTS * 4];
        float pixel[SAVED_CONSTANTS * 4];

        void Begin() {
            CGxDevice* device = g_theGxDevicePtr;
            this->color = nullptr;
            device->DeviceGetRenderTarget(GxBuffers_Color, this->color);
            GxXformViewport(this->viewport[0], this->viewport[1], this->viewport[2], this->viewport[3], this->viewport[4], this->viewport[5]);
            memcpy(this->vertex, CGxDevice::s_shadowConstants[1].constants, sizeof(this->vertex));
            memcpy(this->pixel, CGxDevice::s_shadowConstants[0].constants, sizeof(this->pixel));

            device->RsPush();
            GxRsSet(GxRs_Culling, 0);
            GxRsSet(GxRs_DepthTest, 0);
            GxRsSet(GxRs_DepthWrite, 0);
            GxRsSet(GxRs_Fog, 0);
            GxRsSet(GxRs_Lighting, 0);
            GxRsSet(GxRs_AlphaRef, 0);
            GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
            device->RsSet(GxRs_VertexShader, s_vs);
        }

        void End() {
            CGxDevice* device = g_theGxDevicePtr;
            device->RsPop();
            GxRenderTargetSet(GxBuffers_Color, this->color, 0);
            GxXformSetViewport(this->viewport[0], this->viewport[1], this->viewport[2], this->viewport[3], this->viewport[4], this->viewport[5]);
            device->ShaderConstantsSet(GxSh_Vertex, 0, this->vertex, SAVED_CONSTANTS);
            device->ShaderConstantsSet(GxSh_Pixel, 0, this->pixel, SAVED_CONSTANTS);
        }
    };

    // The view ray across the screen (camera-relative, one yard along the view axis), for the vertex shader.
    struct ViewRays {
        C3Vector origin;
        C3Vector u;
        C3Vector v;
    };
}

static C3Vector Unproject(const C44Matrix& inverse, float x, float y, float z) {
    float px = x * inverse.a0 + y * inverse.b0 + z * inverse.c0 + inverse.d0;
    float py = x * inverse.a1 + y * inverse.b1 + z * inverse.c1 + inverse.d1;
    float pz = x * inverse.a2 + y * inverse.b2 + z * inverse.c2 + inverse.d2;
    float pw = x * inverse.a3 + y * inverse.b3 + z * inverse.c3 + inverse.d3;
    float w = fabsf(pw) > 1e-12f ? 1.0f / pw : 0.0f;
    return C3Vector(px * w, py * w, pz * w);
}

static C3Vector Ray(const C44Matrix& inverse, float x, float y) {
    return Unproject(inverse, x, y, 0.9f) - Unproject(inverse, x, y, 0.1f);
}

static float Dot(const C3Vector& a, const C3Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static bool CalcViewRays(ViewRays& rays) {
    C44Matrix view;
    GxXformView(view);
    C44Matrix viewProj = view * g_theGxDevicePtr->m_projNative;
    C44Matrix inverse = viewProj.Inverse();

    C3Vector forward = Ray(inverse, 0.0f, 0.0f);
    float length = sqrtf(Dot(forward, forward));

    if (!(length > 1e-6f)) {
        return false;
    }

    forward = forward * (1.0f / length);

    // Screen (0, 0) is the top left: clip (-1, 1).
    C3Vector topLeft = Ray(inverse, -1.0f, 1.0f);
    C3Vector topRight = Ray(inverse, 1.0f, 1.0f);
    C3Vector bottomLeft = Ray(inverse, -1.0f, -1.0f);
    float dl = Dot(topLeft, forward);
    float dr = Dot(topRight, forward);
    float db = Dot(bottomLeft, forward);

    if (!(dl > 1e-6f) || !(dr > 1e-6f) || !(db > 1e-6f)) {
        return false;
    }

    topLeft = topLeft * (1.0f / dl);
    topRight = topRight * (1.0f / dr);
    bottomLeft = bottomLeft * (1.0f / db);

    rays.origin = topLeft;
    rays.u = topRight - topLeft;
    rays.v = bottomLeft - topLeft;
    return true;
}

static void SetVertexConstants(float targetWidth, float targetHeight, const ViewRays* rays) {
    float c[16] = {
        1.0f / targetWidth, 1.0f / targetHeight, 0.5f / targetWidth, 0.5f / targetHeight,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
    };

    // OpenGL puts pixel centres on the texel centres already.
    if (g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl) {
        c[2] = 0.0f;
        c[3] = 0.0f;
    }

    if (rays) {
        c[4] = rays->origin.x;
        c[5] = rays->origin.y;
        c[6] = rays->origin.z;
        c[8] = rays->u.x;
        c[9] = rays->u.y;
        c[10] = rays->u.z;
        c[12] = rays->v.x;
        c[13] = rays->v.y;
        c[14] = rays->v.z;
    }

    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, c, 4);
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

// Draws pixelShader into target (nullptr: the render target the pass started with, full window).
static void Pass(PassState& state, CGxTex* target, CGxShader* pixelShader, const ViewRays* rays) {
    CGxDevice* device = g_theGxDevicePtr;
    CGxTex* color = target ? target : state.color;
    GxRenderTargetSet(GxBuffers_Color, color, 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

    float width = static_cast<float>(target ? target->m_width : s_width);
    float height = static_cast<float>(target ? target->m_height : s_height);
    SetVertexConstants(width, height, rays);
    device->RsSet(GxRs_PixelShader, pixelShader);
    DrawQuad();
}

static void SetPixel(uint32_t reg, float x, float y, float z, float w) {
    float c[4] = { x, y, z, w };
    g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, reg, c, 1);
}

static void SetPixel(uint32_t reg, const C4Vector& v) {
    SetPixel(reg, v.x, v.y, v.z, v.w);
}

static bool CopyScene() {
    CGxTex* scene = GxTexOf(s_sceneTex);

    if (!scene) {
        return false;
    }

    C2iVector origin = { 0, 0 };
    C2iVector size = { static_cast<int32_t>(s_width), static_cast<int32_t>(s_height) };
    return GxTexCopy(scene, origin, origin, size, 0, 0) != 0;
}

void PostEffects::RenderOpaque(const C3Vector& cameraPos, const CImVector& fogColor, float zNear, float zFar, float viewportFarClip) {
    if (!s_created || !(s_aoRequested || s_hazeRequested) || !s_usingDepth) {
        return;
    }

    if (zNear <= 0.0f || zFar <= zNear || viewportFarClip <= 0.0f || !EnsureTargets()) {
        return;
    }

    CGxTex* depth = DepthCache::Prepare() ? DepthCache::GetDepthTex() : nullptr;

    if (!depth) {
        LogOnce(2, "ambient occlusion and haze skipped, the scene depth could not be read this frame");
        return;
    }

    ViewRays rays;

    if (!CalcViewRays(rays)) {
        return;
    }

    C4Vector view = ViewSettings::Params(zNear, zFar, viewportFarClip);
    // The world draws short of the far plane; what lies beyond is sky.
    float skyDepth = viewportFarClip * 0.99999f;

    CGxDevice* device = g_theGxDevicePtr;
    PassState state;
    state.Begin();

    if (s_aoRequested) {
        CGxTex* ao0 = GxTexOf(s_aoTex[0]);
        CGxTex* ao1 = GxTexOf(s_aoTex[1]);

        if (ao0 && ao1) {
            float rayV = sqrtf(Dot(rays.v, rays.v));
            float pixelsPerYard = rayV > 0.0f ? static_cast<float>(s_height) / rayV : 0.0f;

            device->RsSet(GxRs_Texture0, depth);
            SetPixel(0, view);
            SetPixel(1, pixelsPerYard, s_params[Param_AoRadius] > 0.1f ? s_params[Param_AoRadius] : 0.1f, s_params[Param_AoStrength], skyDepth);
            SetPixel(2, 1.0f / s_width, 1.0f / s_height, static_cast<float>(s_width), static_cast<float>(s_height));
            SetPixel(3, rays.origin.x, rays.origin.y, rays.origin.z, 0.0f);
            SetPixel(4, rays.u.x, rays.u.y, rays.u.z, 0.0f);
            SetPixel(5, rays.v.x, rays.v.y, rays.v.z, 0.0f);
            Pass(state, ao0, s_aoPs, &rays);

            // Blur across, then down, keeping depth edges.
            device->RsSet(GxRs_Texture1, depth);
            SetPixel(0, view);
            device->RsSet(GxRs_Texture0, ao0);
            SetPixel(1, 1.0f / ao0->m_width, 0.0f, skyDepth, 0.0f);
            Pass(state, ao1, s_aoBlurPs, nullptr);
            device->RsSet(GxRs_Texture0, ao1);
            SetPixel(1, 0.0f, 1.0f / ao0->m_height, skyDepth, 0.0f);
            Pass(state, ao0, s_aoBlurPs, nullptr);

            // Darken the world with it (or show it alone with aoDebug).
            GxRsSet(GxRs_BlendingMode, s_params[Param_AoDebug] != 0.0f ? GxBlend_Opaque : GxBlend_Mod);
            device->RsSet(GxRs_Texture0, ao0);
            Pass(state, nullptr, s_copyPs, nullptr);
            GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
        }
    }

    if (s_hazeRequested) {
        GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
        device->RsSet(GxRs_Texture0, depth);
        SetPixel(0, view);
        SetPixel(1, s_params[Param_AerialFogDensity], s_params[Param_AerialFogFalloff], s_params[Param_AerialFogStart], s_params[Param_AerialFogMax]);
        SetPixel(2, fogColor.r / 255.0f, fogColor.g / 255.0f, fogColor.b / 255.0f, 1.0f);
        SetPixel(3, cameraPos.z, skyDepth, 0.0f, 0.0f);
        Pass(state, nullptr, s_hazePs, &rays);
    }

    state.End();
}

void PostEffects::RenderBloom() {
    if (!s_created || !s_bloomRequested || !EnsureTargets()) {
        return;
    }

    CGxTex* scene = GxTexOf(s_sceneTex);
    CGxTex* bloom0 = GxTexOf(s_bloomTex[0]);
    CGxTex* bloom1 = GxTexOf(s_bloomTex[1]);
    CGxTex* wide0 = GxTexOf(s_bloomWideTex[0]);
    CGxTex* wide1 = GxTexOf(s_bloomWideTex[1]);

    if (!scene || !bloom0 || !bloom1 || !wide0 || !wide1 || !CopyScene()) {
        return;
    }

    CGxDevice* device = g_theGxDevicePtr;
    PassState state;
    state.Begin();

    // The highlights, a quarter of the size, blurred; then an eighth of the size, blurred, for a wider glow.
    device->RsSet(GxRs_Texture0, scene);
    SetPixel(0, 1.0f / s_width, 1.0f / s_height, s_params[Param_BloomThreshold], 0.0f);
    Pass(state, bloom0, s_brightPs, nullptr);

    device->RsSet(GxRs_Texture0, bloom0);
    SetPixel(0, 1.0f / bloom0->m_width, 0.0f, 0.0f, 0.0f);
    Pass(state, bloom1, s_blurPs, nullptr);
    device->RsSet(GxRs_Texture0, bloom1);
    SetPixel(0, 0.0f, 1.0f / bloom0->m_height, 0.0f, 0.0f);
    Pass(state, bloom0, s_blurPs, nullptr);

    device->RsSet(GxRs_Texture0, bloom0);
    Pass(state, wide0, s_copyPs, nullptr);
    device->RsSet(GxRs_Texture0, wide0);
    SetPixel(0, 1.0f / wide0->m_width, 0.0f, 0.0f, 0.0f);
    Pass(state, wide1, s_blurPs, nullptr);
    device->RsSet(GxRs_Texture0, wide1);
    SetPixel(0, 0.0f, 1.0f / wide0->m_height, 0.0f, 0.0f);
    Pass(state, wide0, s_blurPs, nullptr);

    device->RsSet(GxRs_Texture0, scene);
    device->RsSet(GxRs_Texture1, bloom0);
    device->RsSet(GxRs_Texture2, wide0);
    float rollOff = s_params[Param_ToneMapping];
    rollOff = rollOff < 0.0f ? 0.0f : (rollOff > 1.0f ? 1.0f : rollOff);
    SetPixel(0, s_params[Param_BloomIntensity], rollOff, s_params[Param_ToneExposure], s_params[Param_ColorSaturation]);
    SetPixel(1, s_params[Param_ColorContrast], 0.0f, 0.0f, 0.0f);
    Pass(state, nullptr, s_bloomPs, nullptr);

    state.End();
}

void PostEffects::RenderAntiAliasing() {
    if (!s_created || !s_aaRequested || !EnsureTargets()) {
        return;
    }

    CGxTex* scene = GxTexOf(s_sceneTex);

    if (!scene || !CopyScene()) {
        return;
    }

    CGxDevice* device = g_theGxDevicePtr;
    PassState state;
    state.Begin();

    device->RsSet(GxRs_Texture0, scene);
    SetPixel(0, 1.0f / s_width, 1.0f / s_height, 0.0f, 0.0f);
    Pass(state, nullptr, s_fxaaPs, nullptr);

    float sharpen = s_params[Param_Sharpen];

    if (sharpen > 0.0f) {
        // The copy reads the render target, so the pass state's target is put back first.
        GxRenderTargetSet(GxBuffers_Color, state.color, 0);

        if (CopyScene()) {
            device->RsSet(GxRs_Texture0, scene);
            SetPixel(0, 1.0f / s_width, 1.0f / s_height, sharpen > 1.0f ? 1.0f : sharpen, 0.0f);
            Pass(state, nullptr, s_sharpenPs, nullptr);
        }
    }

    state.End();
}

// Blends a flat colour over a whole texture (the water's reflection, faded towards the fog colour). The shaders
// it needs are made on first use, apart from the other effects.
bool PostEffects::Tint(CGxTex* target, const CImVector& color, float amount) {
    if (!target || amount <= 0.0f) {
        return false;
    }

    if (!s_vs) {
        s_vs = CreateEmbedded(GxSh_Vertex, "PostEffects", g_post_vs, sizeof(g_post_vs));
    }

    if (!s_fillPs) {
        s_fillPs = CreateEmbedded(GxSh_Pixel, "PostFill", g_post_fill_ps, sizeof(g_post_fill_ps));
    }

    if (!s_vs || !s_fillPs) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    PassState state;
    state.Begin();
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    SetPixel(0, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, amount > 1.0f ? 1.0f : amount);
    Pass(state, target, s_fillPs, nullptr);
    state.End();
    return true;
}

// Softens target with a separable Gaussian through temp (the same size), radius in texels.
bool PostEffects::Soften(CGxTex* target, CGxTex* temp, float radius) {
    if (!target || !temp || radius <= 0.0f) {
        return false;
    }

    if (!s_vs) {
        s_vs = CreateEmbedded(GxSh_Vertex, "PostEffects", g_post_vs, sizeof(g_post_vs));
    }

    if (!s_blurPs) {
        s_blurPs = CreateEmbedded(GxSh_Pixel, "PostBlur", g_post_blur_ps, sizeof(g_post_blur_ps));
    }

    if (!s_vs || !s_blurPs) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    PassState state;
    state.Begin();
    // The blur steps 1.4 and 3.2 texels out; scaled so its reach is about radius.
    float scale = radius / 3.2f;
    device->RsSet(GxRs_Texture0, target);
    SetPixel(0, scale / target->m_width, 0.0f, 0.0f, 0.0f);
    Pass(state, temp, s_blurPs, nullptr);
    device->RsSet(GxRs_Texture0, temp);
    SetPixel(0, 0.0f, scale / target->m_height, 0.0f, 0.0f);
    Pass(state, target, s_blurPs, nullptr);
    state.End();
    return true;
}

// Fades target towards color by the distance in depth (drawn with view): nothing before start, all of it from end on.
bool PostEffects::FadeByDepth(CGxTex* target, CGxTex* depth, const C4Vector& view, float skyDepth, float start, float end, const CImVector& color) {
    if (!target || !depth || end <= start) {
        return false;
    }

    if (!s_vs) {
        s_vs = CreateEmbedded(GxSh_Vertex, "PostEffects", g_post_vs, sizeof(g_post_vs));
    }

    if (!s_depthFadePs) {
        s_depthFadePs = CreateEmbedded(GxSh_Pixel, "PostDepthFade", g_post_depthfade_ps, sizeof(g_post_depthfade_ps));
    }

    if (!s_vs || !s_depthFadePs) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    PassState state;
    state.Begin();
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    device->RsSet(GxRs_Texture0, depth);
    SetPixel(0, view);
    SetPixel(1, start, 1.0f / (end - start), skyDepth, 0.0f);
    SetPixel(2, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f);
    Pass(state, target, s_depthFadePs, nullptr);
    state.End();
    return true;
}
