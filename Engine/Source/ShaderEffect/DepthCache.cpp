#include "Engine/Source/ShaderEffect/DepthCache.h"
#include "Engine/Source/Base/Handle.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Tempest/CRect.h"

static int32_t s_refCount;
static CTexture* s_depthTex;
static uint32_t s_width;
static uint32_t s_height;
static bool s_valid;
// The texture is a copy made by Prepare (multisampling) rather than the depth buffer itself.
static bool s_copied;

static void DestroyTexture() {
    g_theGxDevicePtr->DeviceSetSampledDepth(nullptr);

    if (s_depthTex) {
        HandleClose(reinterpret_cast<HOBJECT>(s_depthTex));
        s_depthTex = nullptr;
    }

    s_width = 0;
    s_height = 0;
    s_valid = false;
}

static bool Multisampled() {
    return g_theGxDevicePtr->m_format.m_multisampleCount > 1;
}

// CHANGED: the depth is copied (Prepare) with multisampling, and always on the Direct3D 11 device, which cannot
// read a texture while it is the bound depth buffer.
static bool Copied() {
    return Multisampled() || g_theGxDevicePtr->DeviceApi() != GxApi_D3d9;
}

bool DepthCache::IsSupported() {
    const CGxCaps& caps = g_theGxDevicePtr->Caps();

    if (!caps.m_rttFormat[GxTex_Intz] || caps.m_texTarget[GxTex_NonPow2] != 1) {
        return false;
    }

    return !Copied() || g_theGxDevicePtr->DeviceCanCopySampledDepth();
}

void DepthCache::AddRef() {
    s_refCount++;
}

void DepthCache::Release() {
    if (s_refCount > 0 && --s_refCount == 0) {
        DestroyTexture();
    }
}

void DepthCache::BeginFrame() {
    if (!s_refCount || !DepthCache::IsSupported()) {
        if (s_depthTex) {
            DestroyTexture();
        }

        return;
    }

    CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_theGxDevicePtr->CapsWindowSize(window);
    uint32_t width = static_cast<uint32_t>(window.maxX - window.minX);
    uint32_t height = static_cast<uint32_t>(window.maxY - window.minY);

    if (width < 8 || height < 8) {
        s_valid = false;
        return;
    }

    if (!s_depthTex || width != s_width || height != s_height) {
        DestroyTexture();

        CGxTexFlags flags(GxTex_Nearest, 0, 0, 0, 0, 1);
        s_depthTex = TextureCreate(GxTex_NonPow2, width, height, 0, GxTex_Intz, GxTex_Intz, flags, nullptr, GxTexNullUserFunc, "DepthCache", 0);
        s_width = width;
        s_height = height;
    }

    CGxTex* gxTex = s_depthTex ? TextureGetGxTex(s_depthTex, 1, nullptr) : nullptr;
    s_copied = Copied();
    // Set every frame: a device reset recreates the texture and drops the device's reference to it. With
    // multisampling the device keeps its own depth buffer and Prepare copies it.
    g_theGxDevicePtr->DeviceSetSampledDepth(s_copied ? nullptr : gxTex);
    s_valid = gxTex != nullptr;
}

bool DepthCache::IsValid() {
    return s_valid;
}

DepthCache::EDepthMode DepthCache::GetDepthMode() {
    return s_valid ? DM_Texture : DM_Unknown;
}

bool DepthCache::Prepare() {
    if (!s_valid || !s_depthTex) {
        return false;
    }

    if (!s_copied) {
        return true;
    }

    CGxTex* gxTex = TextureGetGxTex(s_depthTex, 1, nullptr);
    return gxTex && g_theGxDevicePtr->DeviceCopySampledDepth(gxTex);
}

CGxTex* DepthCache::GetDepthTex() {
    return s_valid && s_depthTex ? TextureGetGxTex(s_depthTex, 1, nullptr) : nullptr;
}
