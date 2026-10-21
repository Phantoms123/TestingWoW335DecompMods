#ifndef ENGINE_SOURCE_SHADEREFFECT_SUNSHAFTS_H
#define ENGINE_SOURCE_SHADEREFFECT_SUNSHAFTS_H

// Light shafts from the sun, ported from the 4.3.4 client (Engine/Source/ShaderEffect/SunShafts.cpp there).
//
// Not part of the original 12340 client. The sky is masked out of the scene depth into a 512x512 texture,
// which is then blurred towards the sun's screen position three times. The sun glare draws with 4.3.4's
// DNGlare shader, which adds that texture. Uses 4.3.4's own shaders, read from
// Shaders\Cata\{Pixel,Vertex}\<profile>\ (SunShaftsMask, SunShaftsPropagate, DNGlare).

#include <cstdint>

class C3Vector;
class C44Matrix;
class CGxTex;

namespace SunShafts {
    // 0 off, 1 and 2 the 4.3.4 quality modes. Takes effect at the next BeginFrame; a mode the device cannot
    // run (no sampled depth, or the 4.x shaders missing) leaves the effect off.
    void SetMode(uint32_t mode);
    // Once a frame before the world is drawn.
    void BeginFrame();
    uint32_t GetMode();
    bool IsEnabled();
    // Once the opaque world is drawn: eye and sun are world positions, zNear/zFar the camera's clip
    // planes, viewportFarClip the depth range the world draws into.
    void Update(const C3Vector& eye, const C3Vector& sun, float zNear, float zFar, float viewportFarClip);
    // True when this frame's shaft texture is ready.
    bool HasBuffer();
    CGxTex* GetBuffer();
    // Inside the sun glare's render state push: switches the glare to 4.3.4's DNGlare shader with the shaft
    // texture. world is the glare's world matrix. Returns false, changing nothing, when shafts are off.
    bool SetupGlare(const C44Matrix& world);
    // After a glare SetupGlare switched has been drawn: puts back the constants it overwrote.
    void EndGlare();
}

#endif
