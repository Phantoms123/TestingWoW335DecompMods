#include "Engine/Source/ShaderEffect/SunShafts.h"
#include <cstring>
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxDevice/ShaderCompat.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C4Vector.h"

static const uint32_t SHAFT_TEX_SIZE = 512;
static const uint32_t PROPAGATE_STEPS = 3;
static const uint32_t MASK_PERMUTATIONS = 6;
static const uint32_t GLARE_PIXEL_PERMUTATIONS = 2;
// Pixel shader constant registers 4.3.4's SunShaftsMask reads.
static const uint32_t MASK_CONST_TEXEL = 0;
static const uint32_t MASK_CONST_VIEW_PARAMS = 28;
static const uint32_t MASK_CONST_HOM_TO_SCREEN = 29;

static const uint16_t s_quadIndices[4] = { 0, 3, 1, 2 };
static const C3Vector s_quad[4] = { C3Vector(-1.0f, -1.0f, 0.0f), C3Vector(-1.0f, 1.0f, 0.0f), C3Vector(1.0f, 1.0f, 0.0f), C3Vector(1.0f, -1.0f, 0.0f) };

static CGxShader* s_maskPixelShaders[MASK_PERMUTATIONS];
static CGxShader* s_maskVertexShader;
static CGxShader* s_propagatePixelShader;
static CGxShader* s_propagateVertexShader;
static CGxShader* s_glarePixelShaders[GLARE_PIXEL_PERMUTATIONS];
static CGxShader* s_glareVertexShader;
static CTexture* s_tex[2];
static uint32_t s_mode;
static uint32_t s_requestedMode;
static bool s_bufferReady;

// The client's own shaders keep some constants for a whole frame (the projection in vertex c2-c5, the fog
// colour in pixel c2), so the registers these passes write are put back afterwards.
static const uint32_t SAVED_VERTEX_CONSTANTS = 6;
static const uint32_t SAVED_PIXEL_CONSTANTS = 30;

struct SavedConstants {
    float vertex[SAVED_VERTEX_CONSTANTS * 4];
    float pixel[SAVED_PIXEL_CONSTANTS * 4];

    void Save() {
        // CGxDevice keeps pixel constants in slot 0 and vertex constants in slot 1.
        memcpy(this->vertex, CGxDevice::s_shadowConstants[1].constants, sizeof(this->vertex));
        memcpy(this->pixel, CGxDevice::s_shadowConstants[0].constants, sizeof(this->pixel));
    }

    void Restore() const {
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Vertex, 0, this->vertex, SAVED_VERTEX_CONSTANTS);
        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, this->pixel, SAVED_PIXEL_CONSTANTS);
    }
};

static SavedConstants s_glareConstants;

static void ReleaseShader(CGxShader*& shader) {
    if (shader) {
        g_theGxDevicePtr->ShaderDestroy(shader);
        shader = nullptr;
    }
}

static void Destroy() {
    for (CGxShader*& shader : s_maskPixelShaders) {
        ReleaseShader(shader);
    }

    for (CGxShader*& shader : s_glarePixelShaders) {
        ReleaseShader(shader);
    }

    ReleaseShader(s_maskVertexShader);
    ReleaseShader(s_propagatePixelShader);
    ReleaseShader(s_propagateVertexShader);
    ReleaseShader(s_glareVertexShader);

    for (CTexture*& tex : s_tex) {
        if (tex) {
            HandleClose(reinterpret_cast<HOBJECT>(tex));
            tex = nullptr;
        }
    }

    s_bufferReady = false;
}

static bool Valid(CGxShader* shader) {
    return shader && shader->Valid();
}

// Loads everything the effect uses; false when any of it is missing.
static bool Create() {
    CGxDevice* device = g_theGxDevicePtr;
    device->ShaderCreate(s_maskPixelShaders, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "SunShaftsMask", MASK_PERMUTATIONS);
    device->ShaderCreate(&s_maskVertexShader, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "SunShaftsMask", 1);
    device->ShaderCreate(&s_propagatePixelShader, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "SunShaftsPropagate", 1);
    device->ShaderCreate(&s_propagateVertexShader, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "SunShaftsPropagate", 1);
    device->ShaderCreate(s_glarePixelShaders, GxSh_Pixel, SHADER_COMPAT_PIXEL_PATH, "DNGlare", GLARE_PIXEL_PERMUTATIONS);
    device->ShaderCreate(&s_glareVertexShader, GxSh_Vertex, SHADER_COMPAT_VERTEX_PATH, "DNGlare", 1);

    // The mask permutations this client uses (depth mode 3, quality 1 or 2), and the glare with shafts.
    if (!Valid(s_maskPixelShaders[4]) || !Valid(s_maskPixelShaders[5]) || !Valid(s_maskVertexShader) || !Valid(s_propagatePixelShader) ||
        !Valid(s_propagateVertexShader) || !Valid(s_glarePixelShaders[1]) || !Valid(s_glareVertexShader)) {
        return false;
    }

    CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1);

    for (uint32_t i = 0; i < 2; i++) {
        s_tex[i] = TextureCreate(SHAFT_TEX_SIZE, SHAFT_TEX_SIZE, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, GxTexNullUserFunc, i ? "sunShaftsTex1" : "sunShaftsTex0", 0);

        if (!s_tex[i] || !TextureGetGxTex(s_tex[i], 1, nullptr)) {
            return false;
        }
    }

    return true;
}

void SunShafts::SetMode(uint32_t mode) {
    s_requestedMode = mode > 2 ? 2 : mode;
}

static bool ApplyMode(uint32_t mode) {
    if (mode == s_mode) {
        return true;
    }

    if (!mode) {
        Destroy();
        DepthCache::Release();
        s_mode = 0;
        return true;
    }

    if (!s_mode) {
        if (!DepthCache::IsSupported() || !g_theGxDevicePtr->Caps().m_rttFormat[GxTex_Argb8888]) {
            return false;
        }

        if (!Create()) {
            Destroy();
            DebugLog("SunShafts: off; this device cannot sample depth or the 4.x shaders are not in Shaders\\Cata\n");
            return false;
        }

        DepthCache::AddRef();
    }

    s_mode = mode;
    return true;
}

void SunShafts::BeginFrame() {
    if (s_requestedMode != s_mode && !ApplyMode(s_requestedMode)) {
        // Not available on this device or install: stay off instead of retrying every frame.
        s_requestedMode = s_mode;
    }

    s_bufferReady = false;
}

uint32_t SunShafts::GetMode() {
    return s_mode;
}

bool SunShafts::IsEnabled() {
    return s_mode > 0;
}

bool SunShafts::HasBuffer() {
    return s_mode && s_bufferReady;
}

CGxTex* SunShafts::GetBuffer() {
    return s_tex[1] ? TextureGetGxTex(s_tex[1], 1, nullptr) : nullptr;
}

// 4.3.4 ViewSettings::CalculateParams.
static C4Vector ViewParams(float zNear, float zFar, float viewportFarClip) {
    C4Vector params;
    params.x = zNear;
    params.y = zFar;
    params.z = (1.0f / zFar - 1.0f / zNear) / viewportFarClip;
    params.w = 1.0f / zNear;
    return params;
}

// 4.3.4 ViewSettings::CalculateHomToScreen, with the view extents taken from the current viewport the way
// 4.3.4's CWorldScene::Render records them.
static C4Vector HomToScreen(float offsetX, float offsetY) {
    const CBoundingBox& viewport = g_theGxDevicePtr->m_viewport;
    float extMinX = viewport.m_y.m_l;
    float extMinY = viewport.m_x.m_l;
    float extMaxX = viewport.m_y.m_h;
    float extMaxY = viewport.m_x.m_h;

    float x = ((extMinY + 1.0f) - (1.0f - extMaxY)) * 0.5f;
    float scaleY;
    float y;

    if (g_theGxDevicePtr->Caps().m_texelCenterOnEdge) {
        scaleY = (extMaxX - extMinX) * 0.5f;
        y = (extMinX + 1.0f) - (1.0f - extMaxY);
    } else {
        scaleY = (extMaxX - extMinX) * -0.5f;
        y = ((1.0f - extMaxX) + 1.0f) - extMinX;
    }

    y = y * 0.5f;

    if (g_theGxDevicePtr->Caps().m_pixelCenterOnEdge) {
        x = x + offsetX;
        y = y + offsetY;
    }

    C4Vector result;
    result.x = (extMaxY - extMinY) * 0.5f;
    result.y = scaleY;
    result.z = x;
    result.w = y;
    return result;
}

void SunShafts::Update(const C3Vector& eye, const C3Vector& sun, float zNear, float zFar, float viewportFarClip) {
    s_bufferReady = false;

    CGxTex* depthTex = DepthCache::GetDepthTex();

    if (!s_mode || !depthTex || !s_tex[0] || !s_tex[1] || zNear <= 0.0f || zFar <= zNear || viewportFarClip <= 0.0f) {
        return;
    }

    CGxTex* shaftTex[2] = { TextureGetGxTex(s_tex[0], 1, nullptr), TextureGetGxTex(s_tex[1], 1, nullptr) };

    if (!shaftTex[0] || !shaftTex[1]) {
        return;
    }

    // The sun's position on screen, the way 4.3.4 projects it (the direction from the eye, as the glare is
    // drawn relative to the camera).
    C44Matrix viewProj;
    GxXformViewProj(viewProj);

    C3Vector dir = sun - eye;
    C4Vector pos;
    pos.x = viewProj.a0 * dir.x + viewProj.b0 * dir.y + viewProj.c0 * dir.z + viewProj.d0;
    pos.y = viewProj.a1 * dir.x + viewProj.b1 * dir.y + viewProj.c1 * dir.z + viewProj.d1;
    pos.z = viewProj.a2 * dir.x + viewProj.b2 * dir.y + viewProj.c2 * dir.z + viewProj.d2;
    pos.w = viewProj.a3 * dir.x + viewProj.b3 * dir.y + viewProj.c3 * dir.z + viewProj.d3;

    if (pos.w < 0.00000011920929f) {
        return;
    }

    float invW = 1.0f / pos.w;
    C4Vector sunPos;
    sunPos.x = pos.x * invW;
    sunPos.y = pos.y * invW;
    sunPos.z = pos.z * invW;
    sunPos.w = 1.0f;

    CGxDevice* device = g_theGxDevicePtr;

    // Texel values from the depth texture and the world viewport, before either changes.
    C4Vector texel;
    texel.x = 1.0f / depthTex->m_width;
    texel.y = 1.0f / depthTex->m_height;
    texel.z = texel.x * 0.5f;
    texel.w = texel.y * 0.5f;

    C4Vector viewParams = ViewParams(zNear, zFar, viewportFarClip);
    C4Vector homToScreen = HomToScreen(texel.z, texel.w);

    CGxTex* savedColor = nullptr;
    device->DeviceGetRenderTarget(GxBuffers_Color, savedColor);

    SavedConstants constants;
    constants.Save();

    float minX, maxX, minY, maxY, minZ, maxZ;
    GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

    device->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_BlendingMode, 0);

    CGxBatch batch;
    batch.m_primType = GxPrim_TriangleStrip;
    batch.m_start = 0;
    batch.m_count = 4;
    batch.m_minIndex = 0;
    batch.m_maxIndex = 3;

    // Mask: the sky, read from the scene depth, into the first texture. A texture target gets its own depth
    // buffer, so the depth texture is not bound while it is read.
    GxRenderTargetSet(GxBuffers_Color, shaftTex[0], 0);
    GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    device->RsSet(GxRs_VertexShader, s_maskVertexShader);
    device->RsSet(GxRs_PixelShader, s_maskPixelShaders[s_mode + 2 * DepthCache::DM_Texture - 3]);
    device->RsSet(GxRs_Texture0, depthTex);
    device->ShaderConstantsSet(GxSh_Pixel, MASK_CONST_TEXEL, &texel.x, 1);
    device->ShaderConstantsSet(GxSh_Pixel, MASK_CONST_VIEW_PARAMS, &viewParams.x, 1);
    device->ShaderConstantsSet(GxSh_Pixel, MASK_CONST_HOM_TO_SCREEN, &homToScreen.x, 1);
    GxPrimVertexPtr(4, s_quad, sizeof(C3Vector), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
    GxPrimIndexPtr(4, s_quadIndices);
    device->Draw(&batch, 1);

    // Propagate: blur towards the sun, ping-ponging between the two textures; the last write is texture 1.
    device->RsSet(GxRs_VertexShader, s_propagateVertexShader);
    device->RsSet(GxRs_PixelShader, s_propagatePixelShader);

    texel.x = 1.0f / SHAFT_TEX_SIZE;
    texel.y = 1.0f / SHAFT_TEX_SIZE;
    texel.z = texel.x * 0.5f;
    texel.w = texel.y * 0.5f;

    C4Vector flip;
    flip.x = 1.0f;
    flip.y = -1.0f;
    flip.z = 0.0f;
    flip.w = 0.0f;

    device->ShaderConstantsSet(GxSh_Vertex, 0, &texel.x, 1);
    device->ShaderConstantsSet(GxSh_Vertex, 1, &flip.x, 1);
    device->ShaderConstantsSet(GxSh_Pixel, 0, &texel.x, 1);
    device->ShaderConstantsSet(GxSh_Pixel, 2, &sunPos.x, 1);

    uint32_t target = 0;

    for (uint32_t i = 0; i < PROPAGATE_STEPS; i++) {
        target ^= 1;
        GxRenderTargetSet(GxBuffers_Color, shaftTex[target], 0);
        GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

        C4Vector offset;
        offset.x = texel.z + i * texel.x;
        offset.y = i * texel.x + texel.w;
        offset.z = 0.0f;
        offset.w = 0.0f;
        device->ShaderConstantsSet(GxSh_Pixel, 1, &offset.x, 1);

        CGxTex* source = shaftTex[target ^ 1];

        for (uint32_t tmu = 0; tmu < 8; tmu++) {
            device->RsSet(static_cast<EGxRenderState>(GxRs_Texture0 + tmu), source);
        }

        GxPrimVertexPtr(4, s_quad, sizeof(C3Vector), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0);
        GxPrimIndexPtr(4, s_quadIndices);
        device->Draw(&batch, 1);
    }

    device->RsPop();
    constants.Restore();
    GxRenderTargetSet(GxBuffers_Color, savedColor, 0);
    GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);
    s_bufferReady = true;
}

bool SunShafts::SetupGlare(const C44Matrix& world) {
    CGxTex* shafts = SunShafts::HasBuffer() ? SunShafts::GetBuffer() : nullptr;

    if (!shafts) {
        return false;
    }

    CGxDevice* device = g_theGxDevicePtr;
    C44Matrix view;
    GxXformView(view);
    C44Matrix transform = world * view * device->m_projNative;
    s_glareConstants.Save();

    device->RsSet(GxRs_Texture1, shafts);
    device->RsSet(GxRs_VertexShader, s_glareVertexShader);
    device->RsSet(GxRs_PixelShader, s_glarePixelShaders[1]);
    device->ShaderConstantsSet(GxSh_Vertex, 0, reinterpret_cast<const float*>(&transform), 4);
    return true;
}

void SunShafts::EndGlare() {
    s_glareConstants.Restore();
}
