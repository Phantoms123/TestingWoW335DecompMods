#ifndef ENGINE_SOURCE_SHADEREFFECT_VIEWSETTINGS_H
#define ENGINE_SOURCE_SHADEREFFECT_VIEWSETTINGS_H

// Shader constants describing the view, from the 4.3.4 client's ViewSettings (ViewSettings::CalculateParams and
// ViewSettings::CalculateHomToScreen). Not part of the original 12340 client; shared by the 4.3.4 passes ported
// into it (SunShafts, Liquid::Cata).

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Tempest/C4Vector.h"

namespace ViewSettings {
    // x = near, y = far, and z, w turn a depth buffer value d into a view distance: 1 / (d * z + w).
    // viewportFarClip is the depth the far plane lands on (the world draws into a depth range short of 1).
    inline C4Vector Params(float zNear, float zFar, float viewportFarClip) {
        C4Vector params;
        params.x = zNear;
        params.y = zFar;
        params.z = (1.0f / zFar - 1.0f / zNear) / viewportFarClip;
        params.w = 1.0f / zNear;
        return params;
    }

    // Maps a homogeneous screen position (x, y in -1..1) to a texture coordinate of the current viewport:
    // uv = xy * result.xy + result.zw. offsetX/Y is half a texel, added where pixel centres sit on edges.
    inline C4Vector HomToScreen(float offsetX, float offsetY) {
        const CBoundingBox& viewport = g_theGxDevicePtr->m_viewport;
        float extMinX = viewport.m_y.m_l;
        float extMinY = viewport.m_x.m_l;
        float extMaxX = viewport.m_y.m_h;
        float extMaxY = viewport.m_x.m_h;

        float x = ((extMinY + 1.0f) - (1.0f - extMaxY)) * 0.5f;
        float scaleY;
        float y;

        // 4.3.4 flips the screen's y into texture space except on OpenGL (its caps flag differs from this
        // client's m_texelCenterOnEdge, which the Direct3D devices also set), and adds half a texel on Direct3D 9.
        bool openGl = g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl;

        if (openGl) {
            scaleY = (extMaxX - extMinX) * 0.5f;
            y = (extMinX + 1.0f) - (1.0f - extMaxY);
        } else {
            scaleY = (extMaxX - extMinX) * -0.5f;
            y = ((1.0f - extMaxX) + 1.0f) - extMinX;
        }

        y = y * 0.5f;

        if (!openGl) {
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
}

#endif
