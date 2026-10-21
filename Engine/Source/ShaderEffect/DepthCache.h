#ifndef ENGINE_SOURCE_SHADEREFFECT_DEPTHCACHE_H
#define ENGINE_SOURCE_SHADEREFFECT_DEPTHCACHE_H

// The scene depth as a texture shaders can read, for effects ported from the 4.3.4 client.
//
// Not part of the original 12340 client. 4.3.4's DepthCache copies the depth buffer into an INTZ texture with
// NVAPI or AMD's RESZ trick. Here, without multisampling, the INTZ texture is the back buffer's depth buffer itself
// (CGxDevice::DeviceSetSampledDepth), so there is nothing to copy. INTZ cannot be multisampled, so with
// multisampling the depth is copied into it with RESZ (DXVK and AMD drivers offer it; Prepare does the copy).

#include <cstdint>

class CGxTex;

namespace DepthCache {
    // 4.3.4's depth modes; effects pick shader permutations from it.
    enum EDepthMode {
        DM_Unknown = 0,
        DM_Texture = 3,
    };

    // Users of the depth texture; it exists while the count is above zero.
    void AddRef();
    void Release();
    // True when the device can provide it.
    bool IsSupported();
    // Called once a frame before the world is drawn: makes the depth texture the back buffer's depth buffer,
    // sized to the window, or drops it when nothing uses it.
    void BeginFrame();
    bool IsValid();
    EDepthMode GetDepthMode();
    // Before a pass samples the depth: with multisampling, copies the depth drawn so far into the texture.
    // False when the texture is not valid this frame.
    bool Prepare();
    // The depth texture, or nullptr when it is not valid this frame.
    CGxTex* GetDepthTex();
}

#endif
